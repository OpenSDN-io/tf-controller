import json
import logging
import mock
import tempfile
from testtools import ExpectedException

import gevent.monkey
gevent.monkey.patch_all()

import cassandra_fake_impl
from vnc_cfg_api_server.tests.test_case import ApiServerTestCase

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

class TestDbJsonExim(ApiServerTestCase):
    @classmethod
    def setUpClass(cls, *args, **kwargs):
        cls.console_handler = logging.StreamHandler()
        cls.console_handler.setLevel(logging.DEBUG)
        logger.addHandler(cls.console_handler)
        super(TestDbJsonExim, cls).setUpClass(*args, **kwargs)
        cls.to_bgp_ks = '%s_to_bgp_keyspace' %(cls._cluster_id)
        cls.svc_mon_ks = '%s_svc_monitor_keyspace' %(cls._cluster_id)
        cls.dev_mgr_ks = '%s_dm_keyspace' %(cls._cluster_id)
        cls.hub = gevent.get_hub()

    # end setUpClass

    @classmethod
    def tearDownClass(cls, *args, **kwargs):
        logger.removeHandler(cls.console_handler)
        cls.hub.destroy()
        super(TestDbJsonExim, cls).tearDownClass(*args, **kwargs)
    # end tearDownClass

    def test_db_exim_args(self):
        from cfgm_common import db_json_exim
        with ExpectedException(db_json_exim.InvalidArguments,
            'Both --import-from and --export-to cannot be specified'):
            db_json_exim.DatabaseExim("--import-from foo --export-to bar")
    # end test_db_exim_args
    def test_db_export(self):
        from cfgm_common import db_json_exim
        with tempfile.NamedTemporaryFile() as export_dump:
            patch_ks = cassandra_fake_impl.CassandraFakeServer.patch_keyspace
            with patch_ks(self.to_bgp_ks, {}), \
                 patch_ks(self.svc_mon_ks, {}), \
                 patch_ks(self.dev_mgr_ks, {}), \
                 mock.patch.object(gevent, "get_hub",
                                   return_value=self.hub):
                vn_obj = self._create_test_object()
                db_json_exim.DatabaseExim('--export-to %s --cluster_id %s' %(
                    export_dump.name, self._cluster_id)).db_export()
                dump = json.loads(export_dump.readlines()[0])
                dump_cassandra = dump['cassandra']
                dump_zk = json.loads(dump['zookeeper'])
                uuid_table = dump_cassandra['config_db_uuid']['obj_uuid_table']
                self.assertEqual(uuid_table[vn_obj.uuid]['fq_name'][0],
                    json.dumps(vn_obj.get_fq_name()))
                zk_node = [node for node in dump_zk
                    if node[0] == '%s/fq-name-to-uuid/virtual_network:%s/' %(
                        self._cluster_id, vn_obj.get_fq_name_str())]
                self.assertEqual(len(zk_node), 1)
                self.assertEqual(zk_node[0][1][0], vn_obj.uuid)
    # end test_db_export

    def test_db_export_with_omit_keyspaces(self):
        from cfgm_common import db_json_exim
        with tempfile.NamedTemporaryFile() as export_dump, \
             mock.patch.object(gevent, "get_hub",
                               return_value=self.hub):
            vn_obj = self._create_test_object()

            omit_ks = set(db_json_exim.KEYSPACES) - set(['config_db_uuid'])
            args = '--export-to %s --omit-keyspaces ' %(export_dump.name)
            for ks in list(omit_ks):
                args += '%s ' %(ks)
            args += '--cluster_id %s' %(self._cluster_id)
            db_json_exim.DatabaseExim(args).db_export()
            dump = json.loads(export_dump.readlines()[0])
            dump_cassandra = dump['cassandra']
            dump_zk = json.loads(dump['zookeeper'])
            uuid_table = dump_cassandra['config_db_uuid']['obj_uuid_table']
            self.assertEqual(uuid_table[vn_obj.uuid]['fq_name'][0],
                json.dumps(vn_obj.get_fq_name()))
            zk_node = [node for node in dump_zk
                if node[0] == '%s/fq-name-to-uuid/virtual_network:%s/' %(
                    self._cluster_id, vn_obj.get_fq_name_str())]
            self.assertEqual(len(zk_node), 1)
            self.assertEqual(zk_node[0][1][0], vn_obj.uuid)
    # end test_db_export_with_omit_keyspaces

    def test_db_export_and_import(self):
        from cfgm_common import db_json_exim
        with tempfile.NamedTemporaryFile() as dump_f:
            patch_ks = cassandra_fake_impl.CassandraFakeServer.patch_keyspace
            with patch_ks(self.to_bgp_ks, {}), \
                 patch_ks(self.svc_mon_ks, {}), \
                 patch_ks(self.dev_mgr_ks, {}), \
                 mock.patch.object(gevent, "get_hub",
                                   return_value=self.hub):
                vn_obj = self._create_test_object()
                db_json_exim.DatabaseExim('--export-to %s --cluster_id %s' %(
                    dump_f.name, self._cluster_id)).db_export()
                with ExpectedException(db_json_exim.CassandraNotEmptyError):
                    db_json_exim.DatabaseExim(
                        '--import-from %s --cluster_id %s' %(
                        dump_f.name, self._cluster_id)).db_import()

                uuid_cf = self.get_cf(
                    'config_db_uuid', 'obj_uuid_table')
                fq_name_cf = self.get_cf(
                    'config_db_uuid', 'obj_fq_name_table')
                shared_cf = self.get_cf(
                    'config_db_uuid', 'obj_shared_table')
                with uuid_cf.patch_cf({}), fq_name_cf.patch_cf({}), \
                     shared_cf.patch_cf({}):
                    with ExpectedException(
                         db_json_exim.ZookeeperNotEmptyError):
                        db_json_exim.DatabaseExim(
                            '--import-from %s --cluster_id %s' %(
                            dump_f.name, self._cluster_id)).db_import()

                exim_obj = db_json_exim.DatabaseExim(
                    '--import-from %s --cluster_id %s' %(
                    dump_f.name, self._cluster_id))
                with uuid_cf.patch_cf({}), fq_name_cf.patch_cf({}), \
                    shared_cf.patch_cf({}), exim_obj._zookeeper.patch_path(
                        '%s/' %(self._cluster_id), recursive=True):
                    exim_obj.db_import()
                    dump = json.loads(dump_f.readlines()[0])
                    dump_cassandra = dump['cassandra']
                    dump_zk = json.loads(dump['zookeeper'])
                    uuid_table = dump_cassandra['config_db_uuid']['obj_uuid_table']
                    self.assertEqual(uuid_table[vn_obj.uuid]['fq_name'][0],
                        json.dumps(vn_obj.get_fq_name()))
                    zk_node = [node for node in dump_zk
                        if node[0] == '%s/fq-name-to-uuid/virtual_network:%s/' %(
                            self._cluster_id, vn_obj.get_fq_name_str())]
                    self.assertEqual(len(zk_node), 1)
                self.assertEqual(zk_node[0][1][0], vn_obj.uuid)
    # end test_db_export_and_import
# end class TestDbJsonExim