# USAGE STEPS:
# To upload db
# Stop all contrail services including zookeeper.
# Remove/rename /var/lib/zookeeper/version-2
# Remove/rename /var/lib/cassandra/data
# Start Cassandra-database and zookeeper
# Run the provided script to load the db.
# python db_json_exim.py --import-from /import-data/db.json
# Start all the services and wait till they are all up.
# Provision control node

# To take a db snapshot
# python db_json_exim.py --export-to <filename>


import sys
import logging
import argparse
import gzip
import json
import cgitb
import gevent

from cassandra import cluster, auth
import kazoo.client
import kazoo.handlers.gevent
import kazoo.exceptions

from cfgm_common import utils
from cfgm_common.vnc_cassandra import VncCassandraClient


logger = logging.getLogger(__name__)


class NotEmptyError(Exception): pass
class InvalidArguments(Exception): pass
class AllServersUnavailable(Exception): pass
excpetions = (
    NotEmptyError,
    InvalidArguments,
    AllServersUnavailable
)
class CassandraConnectionError(Exception):
    def __init__(self, message):
        # Call the base class constructor with the parameters it needs
        super().__init__(message)


KEYSPACES = ['config_db_uuid',
            'useragent',
            'to_bgp_keyspace',
            'svc_monitor_keyspace',
            'dm_keyspace']


class LongIntEncoder(json.JSONEncoder):
    """LongIntEncoder ensures that all type of integers up to uint64
    is always encode as number, not a string.
    """
    def default(self, obj):
        if isinstance(obj, int):
            return obj
        return super(LongIntEncoder, self).default(obj)


class DatabaseExim(object):
    def __init__(self, args_str):
        self._parse_args(args_str)

        self._zk_ignore_list = set([
            'consumers',
            'config',
            'controller',
            'isr_change_notification',
            'admin',
            'brokers',
            'zookeeper',
            'controller_epoch',
            'api-server-election',
            'vnc_api_server_locks',
            'vnc_api_server_obj_create',
            'schema-transformer',
            'device-manager',
            'svc-monitor',
            'contrail_cs',
            'lockpath',
            'analytics-discovery-',
            'analytics-discovery-' + self._api_args.cluster_id,
            'vcenter-plugin',
            'vcenter-fabric-manager',
        ])
    # end __init__

    def init_cassandra(self, ks_cf_info=None):
        self._cassandra = VncCassandraClient(
            self._api_args.cassandra_server_list,
            db_prefix=self._api_args.cluster_id,
            rw_keyspaces=ks_cf_info,
            ro_keyspaces=None,
            logger=self.log,
            reset_config=False,
            ssl_enabled=self._api_args.cassandra_use_ssl,
            ca_certs=self._api_args.cassandra_ca_certs,
            cassandra_driver=self._api_args.cassandra_driver,
            zk_servers=self._api_args.zk_server_ip)
        self.driver = self._cassandra._cassandra_driver
        logger.info("Connected to Cassandra")

    def log(self, msg, level):
        logger.debug(msg)

    def _parse_args(self, args_str):
        parser = argparse.ArgumentParser()

        help="Path to contrail-api conf file, default /etc/contrail/contrail-api.conf"
        parser.add_argument(
            "--api-conf", help=help, default="/etc/contrail/contrail-api.conf")
        parser.add_argument(
            "--debug", help="Run in debug mode, default False",
            action='store_true', default=False)
        parser.add_argument(
            "--import-from", help="Import from this json file to database",
            metavar='FILE')
        parser.add_argument(
            "--export-to", help="Export from database to this json file",
            metavar='FILE')
        parser.add_argument(
            "--omit-keyspaces",
            nargs='*',
            help="List of keyspaces to omit in export/import",
            metavar='FILE')
        parser.add_argument(
            "--buffer-size", type=int,
            help="Number of rows fetched at once",
            default=1024)
        parser.add_argument(
            "--dump-timeout", type=int,
            help="Timeout to get db dump",
            default=3600)

        args_obj, remaining_argv = parser.parse_known_args(args_str.split())
        self._args = args_obj

        from vnc_cfg_api_server import utils
        # cfgm_common does not have hard dependence with api-server
        # and should not have to avoid circular dependencies. The
        # ImportError happens when executing unittests for cfgm_common
        # but none of the tests really need it.

        self._api_args = utils.parse_args('-c %s %s'
            %(self._args.api_conf, ' '.join(remaining_argv)))[0]
        logformat = logging.Formatter("%(asctime)s %(levelname)s: %(message)s")
        stdout = logging.StreamHandler(sys.stdout)
        stdout.setFormatter(logformat)
        logger.addHandler(stdout)
        logger.setLevel('DEBUG' if self._args.debug else 'INFO')
        if not getattr(self._api_args, "cassandra_driver"):
            self._api_args.cassandra_driver = "cql"

        if args_obj.import_from is not None and args_obj.export_to is not None:
            raise InvalidArguments(
                'Both --import-from and --export-to cannot be specified %s' %(
                args_obj))

    def db_import(self):
        if self._args.import_from.endswith('.gz'):
            with gzip.open(self._args.import_from, 'rb') as f:
                self.import_data = json.loads(f.read())
        else:
            with open(self._args.import_from, 'r') as f:
                self.import_data = json.loads(f.read())
        logger.info("DB dump file loaded")

        ks_cf_info = dict((ks, dict((c, {}) for c in list(cf.keys())))
            for ks,cf in list(self.import_data['cassandra'].items()))
        self.init_cassandra(ks_cf_info)

        # refuse import if db already has data
        non_empty_errors_cassandra = []
        for ks in list(self.import_data['cassandra'].keys()):
            for cf in list(self.import_data['cassandra'][ks].keys()):
               for i in self.driver.get_range(cf, column_count=1):
                   if len(i) > 0 and i[0] is not None:
                    non_empty_errors_cassandra.append(
                        'Keyspace %s CF %s already has entries.' %(ks, cf))

        zookeeper = kazoo.client.KazooClient(
            self._api_args.zk_server_ip,
            timeout=400,
            handler=kazoo.handlers.gevent.SequentialGeventHandler())
        zookeeper.start()

        non_empty_errors_zookeeper = []
        existing_zk_dirs = set(
            zookeeper.get_children(self._api_args.cluster_id + '/'))
        import_zk_dirs = set([p_v_ts[0].split('/')[1]
                              for p_v_ts in json.loads(self.import_data['zookeeper'] or "[]")])

        for non_empty in existing_zk_dirs & import_zk_dirs - self._zk_ignore_list:
            non_empty_errors_zookeeper.append(
                'Zookeeper has entries at /%s.' %(non_empty))

        if non_empty_errors_zookeeper and non_empty_errors_cassandra:
            raise NotEmptyError(
                '\n'.join(non_empty_errors_cassandra + non_empty_errors_zookeeper))

        # seed cassandra
        if not non_empty_errors_cassandra:
            for ks_name in list(self.import_data['cassandra'].keys()):
                for cf_name in list(self.import_data['cassandra'][ks_name].keys()):
                    for row,cols in list(
                            self.import_data['cassandra'][ks_name][cf_name].items()):
                        # in case of CQL cols is list of lists
                        if isinstance(cols, list):
                            for element in cols:
                                self.driver.insert(row, {element[0]: element[1]},
                                                   cf_name=cf_name)
                        else:
                            # in case of Thrift cols is dict
                            for col_name, col_val_ts in list(cols.items()):
                                self.driver.insert(row, {col_name: col_val_ts[0]},
                                                   cf_name=cf_name)
            logger.info("Cassandra DB restored")

        # seed zookeeper
        if not non_empty_errors_zookeeper:
            for path_value_ts in json.loads(self.import_data['zookeeper'] or "{}"):
                path = path_value_ts[0]
                if path.endswith('/'):
                    path = path[:-1]
                if path.split('/')[1] in self._zk_ignore_list:
                    continue
                value = path_value_ts[1][0]
                zookeeper.create(path, value.encode(), makepath=True)
            logger.info("Zookeeper DB restored")
            zookeeper.stop()

    def _cassandra_dump_cql(self):
        logger.info("Cassandra DB start")

        cassandra_contents = {}
        # Addresses, ports related options
        endpoints, port = [], None
        for address in self._api_args.cassandra_server_list:
            try:
                server, _ = address.split(':', 1)
                endpoints.append(server)
            except ValueError:
                endpoints.append(address)
        auth_provider = auth.PlainTextAuthProvider(
            username=self._api_args.cassandra_user,
            password=self._api_args.cassandra_password)
        try:
            cCluster = cluster.Cluster(
                    endpoints,
                    auth_provider=auth_provider)
        except Exception as error:
            raise CassandraConnectionError("error, {}: {}".format(
                    error, utils.detailed_traceback()))

        with cCluster.connect() as session:
            rows = session.execute(
                'select keyspace_name from system_schema.keyspaces').current_rows
            existing_keyspaces = [row.keyspace_name for row in rows]

            for ks_name in set(KEYSPACES) - set(self._args.omit_keyspaces or []):
                if self._api_args.cluster_id:
                    full_ks_name = '%s_%s' % (self._api_args.cluster_id, ks_name)
                else:
                    full_ks_name = ks_name
                if full_ks_name not in existing_keyspaces:
                    continue
                cassandra_contents[ks_name] = {}
                cql = ("SELECT table_name FROM system_schema.tables "
                       "WHERE keyspace_name = '%s'") % full_ks_name
                column_families = [cf[0] for cf in
                                   session.execute(cql).current_rows]
                for cf_name in column_families:
                    cassandra_contents[ks_name][cf_name] = {}
                    cql = ('SELECT blobAsText(key), blobAsText(column1), '
                           'value, WRITETIME(value) FROM %s.%s'
                           ) % (full_ks_name, cf_name)
                    session.row_factory = lambda c, r: r
                    range = session.execute(cql).current_rows
                    for rw in range:
                        key, row = rw[0], rw[1:]
                        if key in cassandra_contents[ks_name][cf_name]:
                            cassandra_contents[ks_name][cf_name][key].append(row)
                        else:
                            cassandra_contents[ks_name][cf_name][key] = [row]

        cCluster.shutdown()
        logger.info("Cassandra DB dumped")
        return cassandra_contents

    def _zookeeper_dump(self):
        logger.info("Zookeeper DB start")

        def get_nodes(path):
            if path[:-1].rpartition('/')[-1] in self._zk_ignore_list:
                return []

            try:
                if not zk.get_children(path):
                    return [(path, zk.get(path))]
            except kazoo.exceptions.NoNodeError:
                return []

            nodes = []
            for child in zk.get_children(path):
                nodes.extend(get_nodes('%s%s/' %(path, child)))

            return nodes

        zk = kazoo.client.KazooClient(self._api_args.zk_server_ip,
                                      read_only=True,
                                      connection_retry=kazoo.retry.KazooRetry(max_tries=5))
        zk.start()
        nodes = get_nodes(self._api_args.cluster_id+'/')
        zk.stop()
        logger.info("Zookeeper DB dumped")
        return json.dumps(nodes, cls=LongIntEncoder)

    def db_export(self):
        pool = gevent.get_hub().threadpool
        if self._api_args.cassandra_driver == 'cql':
            cass_dump = pool.apply_async(self._cassandra_dump_cql)
        else:
            raise Exception("driver is not supported " + self._api_args.cassandra_driver)
        zoo_dump = pool.apply_async(self._zookeeper_dump)
        db_contents = {
            'cassandra': cass_dump.get(timeout=self._args.dump_timeout),
            'zookeeper': zoo_dump.get(timeout=self._args.dump_timeout),
        }

        with open(self._args.export_to, 'w') as f:
            f.write(json.dumps(db_contents, cls=LongIntEncoder))
        logger.info("DB dump wrote to file %s" % self._args.export_to)
    # end db_export
# end class DatabaseExim


def main(args_str):
    cgitb.enable(format='text')
    try:
        db_exim = DatabaseExim(args_str)
        if 'import-from' in args_str:
            db_exim.db_import()
        if 'export-to' in args_str:
            db_exim.db_export()
    except excpetions as e:
        logger.error(str(e))
        exit(1)


if __name__ == '__main__':
    main(' '.join(sys.argv[1:]))
