#
# Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
#
"""
Provides utility routines for modules in api-server
"""

import argparse
from ast import literal_eval
import base64
from collections import OrderedDict
from cfgm_common import jsonutils as json
from configparser import ConfigParser
import vnc_api.gen.resource_xsd
from . import vnc_quota
import cfgm_common
from Crypto.Cipher import AES
from pysandesh.sandesh_base import Sandesh, SandeshSystem, SandeshConfig
from pysandesh.gen_py.sandesh.ttypes import SandeshLevel
from vnc_api.utils import AAA_MODE_VALID_VALUES

_WEB_HOST = '0.0.0.0'
_WEB_PORT = 8082
_ADMIN_PORT = 8095


def user_password(s):
    creds = []

    for cred in s.split():
        if ':' not in cred:
            msg = "User/password must be 'username:password'"
            raise argparse.ArgumentTypeError(msg)
        creds.append(tuple(cred.split(':', 1)))

    return creds


def parse_args(args_str):
    args_obj = None
    # Source any specified config/ini file
    # Turn off help, so we print all options in response to -h
    conf_parser = argparse.ArgumentParser(add_help=False)

    conf_parser.add_argument("-c", "--conf_file", action='append',
                             help="Specify config file", metavar="FILE")
    args, remaining_argv = conf_parser.parse_known_args(args_str.split())

    defaults = {
        'reset_config': False,
        'wipe_config': False,
        'listen_ip_addr': _WEB_HOST,
        'listen_port': _WEB_PORT,
        'admin_port': _ADMIN_PORT,
        'cassandra_server_list': "127.0.0.1:9160",
        'collectors': None,
        'http_server_port': '8084',
        'http_server_ip': _WEB_HOST,
        'log_local': True,
        'log_level': SandeshLevel.SYS_NOTICE,
        'log_category': '',
        'log_file': Sandesh._DEFAULT_LOG_FILE,
        'trace_file': '/var/log/contrail/vnc_openstack.err',
        'use_syslog': False,
        'syslog_facility': Sandesh._DEFAULT_SYSLOG_FACILITY,
        'logging_conf': '',
        'logger_class': None,
        'multi_tenancy': None,
        'aaa_mode': None,
        'zk_server_ip': '127.0.0.1:2181',
        'worker_id': '0',
        'rabbit_server': 'localhost',
        'rabbit_port': '5672',
        'rabbit_user': 'guest',
        'rabbit_password': 'guest',
        'rabbit_vhost': None,
        'rabbit_ha_mode': False,
        'rabbit_max_pending_updates': '4096',
        'rabbit_health_check_interval': '120.0', # in seconds
        'cluster_id': '',
        'max_requests': 1024,
        'paginate_count': 256,
        'region_name': 'RegionOne',
        'stale_lock_seconds': '5', # lock but no resource past this => stale
        'cloud_admin_role': cfgm_common.CLOUD_ADMIN_ROLE,
        'global_read_only_role': cfgm_common.GLOBAL_READ_ONLY_ROLE,
        'rabbit_use_ssl': False,
        'kombu_ssl_version': '',
        'kombu_ssl_keyfile': '',
        'kombu_ssl_certfile': '',
        'kombu_ssl_ca_certs': '',
        'object_cache_entries': '10000', # max number of objects cached for read
        'object_cache_exclude_types': '', # csv of object types to *not* cache
        'debug_object_cache_types': '', # csv of object types to debug cache
        'db_engine': 'cassandra',
        'filter_optimization_enabled': False,
        'max_request_size': 1024000,
        'amqp_timeout': 660,
        'config_api_ssl_enable': False,
        'config_api_ssl_keyfile': '',
        'config_api_ssl_certfile': '',
        'config_api_ssl_ca_cert': '',
        'tcp_keepalive_enable': True,
        'tcp_keepalive_idle_time': 7200,
        'tcp_keepalive_interval': 75,
        'tcp_keepalive_probes': 9,
        'enable_latency_stats_log': False,
        'enable_api_stats_log': False,
        'watch_keepalive_interval': 60,
        'worker_introspect_ports': '',
        'worker_admin_ports': '',
        'contrail_version': '',
        'max_bytes': 5000000,
        'backup_count': 10,
    }
    defaults.update(SandeshConfig.get_default_options(['DEFAULTS']))
    # keystone options
    ksopts = {
        'signing_dir': '/var/lib/contrail/keystone-signing',
        'auth_host': '127.0.0.1',
        'auth_port': '35357',
        'auth_protocol': 'http',
        'admin_user': '',
        'admin_password': '',
        'admin_tenant_name': '',
        'admin_user_domain_name': None,
        'identity_uri': None,
        'project_domain_name': None,
        'insecure': True,
        'cafile': '',
        'certfile': '',
        'keyfile': '',
        'auth_type': 'password',
        'auth_url': '',
        'default_domain_id': 'default',
        'interface': 'admin',
    }
    # cassandra options
    cassandraopts = {
        'cassandra_user'     : None,
        'cassandra_password' : None,
        'cassandra_driver'   : 'cql',
        'num_workers': None,
        'num_groups': None,
    }
    # zookeeper options
    zookeeperopts = {
        'zookeeper_ssl_enable': False,
        'zookeeper_ssl_keyfile': None,
        'zookeeper_ssl_certificate': None,
        'zookeeper_ssl_ca_cert': None,
    }
    # sandesh options
    sandeshopts = SandeshConfig.get_default_options()

    config = None
    saved_conf_file = args.conf_file
    if args.conf_file:
        config = ConfigParser({'admin_token': None}, strict=False, allow_no_value=True)
        config.read(args.conf_file)
        if 'DEFAULTS' in config.sections():
            defaults.update(dict(config.items("DEFAULTS")))
            if 'multi_tenancy' in config.options('DEFAULTS'):
                defaults['multi_tenancy'] = config.getboolean(
                    'DEFAULTS', 'multi_tenancy')
            if 'default_encoding' in config.options('DEFAULTS'):
                default_encoding = config.get('DEFAULTS', 'default_encoding')
                gen.resource_xsd.ExternalEncoding = default_encoding
        if 'KEYSTONE' in config.sections():
            ksopts.update(dict(config.items("KEYSTONE")))
        if 'QUOTA' in config.sections():
            for (k, v) in config.items("QUOTA"):
                try:
                    if str(k) != 'admin_token':
                        vnc_quota.QuotaHelper.default_quota[str(k)] = int(v)
                except ValueError:
                    pass
        if 'CASSANDRA' in config.sections():
                cassandraopts.update(dict(config.items('CASSANDRA')))
        if 'ZOOKEEPER' in config.sections():
                zookeeperopts.update(dict(config.items('ZOOKEEPER')))
        SandeshConfig.update_options(sandeshopts, config)

    def check_maxbytes_range(arg):
        try:
            value = int(arg)
        except ValueError as err:
            raise argparse.ArgumentTypeError(str(err))

        if value < 5000000 or value > 50000000:
            message = ("Expecting 5000000 <= value <= 50000000,"
                      " got value = {}").format(value)
            raise argparse.ArgumentTypeError(message)

        return value

    def check_backupcount_range(arg):
        try:
            value = int(arg)
        except ValueError as err:
            raise argparse.ArgumentTypeError(str(err))

        if value < 10 or value > 100:
            message = ("Expecting 10 <= value <= 100,"
                      " got value = {}").format(value)
            raise argparse.ArgumentTypeError(message)

        return value

    # Override with CLI options
    # Don't surpress add_help here so it will handle -h
    parser = argparse.ArgumentParser(
        # Inherit options from config_parser
        parents=[conf_parser],
        # print script description with -h/--help
        description=__doc__,
        # Don't mess with format of description
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    defaults.update(ksopts)
    defaults.update(cassandraopts)
    defaults.update(zookeeperopts)
    defaults.update(sandeshopts)
    parser.set_defaults(**defaults)

    parser.add_argument(
        "--cassandra_server_list",
        help="List of cassandra servers in IP Address:Port format",
        nargs='+')
    parser.add_argument(
        "--cassandra_driver",
        choices=['cql'],
        help="The Cassandra's driver used. ")
    parser.add_argument(
        "--cassandra_use_ssl", action="store_true",
        help="Enable TLS for cassandra connection")
    parser.add_argument(
        "--cassandra_ca_certs",
        help="Cassandra CA certs")
    parser.add_argument(
        "--zookeeper_ssl_enable",
        help="Enable SSL in rest api server")
    parser.add_argument(
        "--zookeeper_insecure_enable",
        help="Enable insecure mode")
    parser.add_argument(
        "--zookeeper_ssl_certfile",
        help="Location of zookeeper ssl host certificate")
    parser.add_argument(
        "--zookeeper_ssl_keyfile",
        help="Location of zookeeper ssl private key")
    parser.add_argument(
        "--zookeeper_ssl_ca_cert", type=str,
        help="Location of zookeeper ssl CA certificate")
    parser.add_argument(
        "--redis_server_ip",
        help="IP address of redis server")
    parser.add_argument(
        "--redis_server_port",
        help="Port of redis server")
    parser.add_argument(
        "--auth", choices=['keystone', 'noauth', 'no-auth'],
        help="Type of authentication for user-requests")
    parser.add_argument(
        "--reset_config", action="store_true",
        help="Warning! Destroy previous configuration and start clean")
    parser.add_argument(
        "--wipe_config", action="store_true",
        help="Warning! Destroy previous configuration")
    parser.add_argument(
        "--listen_ip_addr",
        help="IP address to provide service on, default %s" % (_WEB_HOST))
    parser.add_argument(
        "--listen_port",
        help="Port to provide service on, default %s" % (_WEB_PORT))
    parser.add_argument(
        "--admin_port",
        help="Port with local auth for admin access, default %s"
              % (_ADMIN_PORT))
    parser.add_argument(
        "--collectors",
        help="List of VNC collectors in ip:port format",
        nargs="+")
    parser.add_argument(
        "--http_server_port",
        help="Port of Introspect HTTP server")
    parser.add_argument(
        "--http_server_ip",
        help="IP address of Introspect HTTP server, default %s" % (_WEB_HOST))
    parser.add_argument(
        "--log_local", action="store_true",
        help="Enable local logging of sandesh messages")
    parser.add_argument(
        "--log_level",
        help="Severity level for local logging of sandesh messages")
    parser.add_argument(
        "--logging_conf",
        help=("Optional logging configuration file, default: None"))
    parser.add_argument(
        "--logger_class",
        help=("Optional external logger class, default: None"))
    parser.add_argument(
        "--log_category",
        help="Category filter for local logging of sandesh messages")
    parser.add_argument(
        "--log_file",
        help="Filename for the logs to be written to")
    parser.add_argument(
        "--trace_file",
        help="Filename for the errors backtraces to be written to")
    parser.add_argument("--use_syslog",
        action="store_true",
        help="Use syslog for logging")
    parser.add_argument("--syslog_facility",
        help="Syslog facility to receive log lines")
    parser.add_argument(
        "--multi_tenancy", action="store_true",
        help="Validate resource permissions (implies token validation)")
    parser.add_argument(
        "--aaa_mode", choices=AAA_MODE_VALID_VALUES,
        help="AAA mode")
    parser.add_argument(
        "--worker_id",
        help="Worker Id")
    parser.add_argument(
        "--zk_server_ip",
        help="Ip address:port of zookeeper server")
    parser.add_argument(
        "--rabbit_server",
        help="Rabbitmq server address")
    parser.add_argument(
        "--rabbit_port",
        help="Rabbitmq server port")
    parser.add_argument(
        "--rabbit_user",
        help="Username for rabbit")
    parser.add_argument(
        "--rabbit_vhost",
        help="vhost for rabbit")
    parser.add_argument(
        "--rabbit_password",
        help="password for rabbit")
    parser.add_argument(
        "--rabbit_ha_mode",
        help="True if the rabbitmq cluster is mirroring all queue")
    parser.add_argument(
        "--rabbit_max_pending_updates",
        help="Max updates before stateful changes disallowed")
    parser.add_argument(
        "--rabbit_health_check_interval",
        help="Interval seconds between consumer heartbeats to rabbitmq")
    parser.add_argument(
        "--cluster_id",
        help="Used for database keyspace separation")
    parser.add_argument(
        "--max_requests", type=int,
        help="Maximum number of concurrent requests served by api server")
    parser.add_argument(
        "--paginate_count", type=int,
        help="Default number of items when pagination is requested")
    parser.add_argument("--cassandra_user",
            help="Cassandra user name")
    parser.add_argument("--cassandra_password",
            help="Cassandra password")
    parser.add_argument("--stale_lock_seconds",
            help="Time after which lock without resource is stale, default 60")
    parser.add_argument( "--cloud_admin_role",
        help="Role name of cloud administrator")
    parser.add_argument( "--global_read_only_role",
        help="Role name of user with Read-Only access to all objects")
    parser.add_argument("--object_cache_entries",
            help="Maximum number of objects cached for read, default 10000")
    parser.add_argument("--object_cache_exclude_types",
            help="Comma separated values of object types to not cache")
    parser.add_argument(
        "--debug_object_cache_types",
        help="Comma separated values of object types to debug trace between "
             "the cache and the DB")
    parser.add_argument("--db_engine",
        help="Database engine to use, default cassandra")
    parser.add_argument("--max_request_size", type=int,
            help="Maximum size of bottle requests served by api server")
    parser.add_argument("--amqp_timeout", help="Timeout for amqp request")
    parser.add_argument("--enable-latency-stats-log",
                         help=("Latency logs for Cassandra,"
                               " Zookeeper, and Keystone"
                               " will be enabled"))
    parser.add_argument("--enable-api-stats-log",
            help="If enabled then api statistics logs will be stored in Db")
    parser.add_argument(
        "--watch_keepalive_interval", type=int,
        help="Interval in seconds after watch api will send keepalive message")
    parser.add_argument("--worker_introspect_ports",
        help="List of introspect ports for uwsgi workers")
    parser.add_argument("--worker_admin_ports",
        help="List of admin ports for uwsgi workers")
    parser.add_argument("--max_bytes",
        type=check_maxbytes_range, nargs="?")
    parser.add_argument("--backup_count",
        type=check_backupcount_range, nargs="?")
    parser.add_argument(
        "--contrail_version",
        help="contrail build version info")
    SandeshConfig.add_parser_arguments(parser)
    args_obj, remaining_argv = parser.parse_known_args(remaining_argv)
    args_obj.conf_file = args.conf_file
    args_obj.config_sections = config
    if isinstance(args_obj.cassandra_server_list, str):
        args_obj.cassandra_server_list =\
            args_obj.cassandra_server_list.split()
    if isinstance(args_obj.collectors, str):
        args_obj.collectors = args_obj.collectors.split()
    args_obj.sandesh_config = SandeshConfig.from_parser_arguments(args_obj)
    args_obj.cassandra_use_ssl = (str(args_obj.cassandra_use_ssl).lower() == 'true')
    args_obj.config_api_ssl_enable = (str(args_obj.config_api_ssl_enable).lower() == 'true')
    args_obj.zookeeper_ssl_enable = (str(args_obj.zookeeper_ssl_enable).lower() == 'true')
    # convert log_local to a boolean
    if not isinstance(args_obj.log_local, bool):
        args_obj.log_local = bool(literal_eval(args_obj.log_local))
    args_obj.conf_file = saved_conf_file
    return args_obj, remaining_argv
# end parse_args

try:
    from termcolor import colored
except ImportError:
    def colored(logmsg, *args, **kwargs):
        return logmsg

class ColorLog(object):

    colormap = dict(
        debug=dict(color='green'),
        info=dict(color='green', attrs=['bold']),
        warn=dict(color='yellow', attrs=['bold']),
        warning=dict(color='yellow', attrs=['bold']),
        error=dict(color='red'),
        critical=dict(color='red', attrs=['bold']),
    )

    def __init__(self, logger):
        self._log = logger

    def __getattr__(self, name):
        if name in ['debug', 'info', 'warn', 'warning', 'error', 'critical']:
            return lambda s, *args: getattr(self._log, name)(
                colored(s, **self.colormap[name]), *args)

        return getattr(self._log, name)
# end ColorLog


def get_filters(data, skips=None):
    """Extracts the filters of query parameters.
    Returns a dict of lists for the filters:
    check=a&check=b&name=Bob&
    becomes:
    {'check': [u'a', u'b'], 'name': [u'Bob']}
    'data' contains filters in format:
    check==a,check==b,name==Bob
    """
    skips = skips or []
    res = OrderedDict()

    if not data:
        return res

    for filter in data.split(','):
        key, value = filter.split('==')
        try:
            value = json.loads(value)
        except ValueError:
            pass
        if key in skips:
            continue
        values = list(set(res.get(key, [])) | set([value]))
        if values:
            res[key] = values
    return res


def encrypt_password(pwd_key, dict_password):
    # AES is a block cipher that only works with block of 16 chars.
    # We need to pad both key and text so that their length is equal
    # to next higher multiple of 16
    # Used https://stackoverflow.com/a/33299416

    pwd_key = pwd_key[-32:]
    key_padding_len = (len(pwd_key) + 16 - 1) // 16
    if key_padding_len == 0:
        key_padding_len = 1
    key = pwd_key.rjust(16 * key_padding_len)
    key_b = key.encode()
    cipher = AES.new(key_b, AES.MODE_ECB)

    text_padding_len = (len(dict_password) + 16 - 1) // 16
    if text_padding_len == 0:
        text_padding_len = 1
    padded_text = dict_password.rjust(16 * text_padding_len)
    padded_text_b = padded_text.encode()
    password = base64.b64encode(cipher.encrypt(padded_text_b))
    return password
# end encrypt_password

