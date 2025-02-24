#!/usr/bin/python3
#
# Copyright (c) 2016 Juniper Networks, Inc. All rights reserved.
#

import argparse
import configparser
import logging
import sys

from cfgm_common.datastore.keyspace import ConfigKeyspaceMap
from pysandesh.gen_py.sandesh.ttypes import SandeshLevel
from sandesh_common.vns.constants import API_SERVER_KEYSPACE_NAME
from sandesh_common.vns.constants import DEVICE_MANAGER_KEYSPACE_NAME
from sandesh_common.vns.constants import SCHEMA_KEYSPACE_NAME
from sandesh_common.vns.constants import SVC_MONITOR_KEYSPACE_NAME
from sandesh_common.vns.constants import USERAGENT_KEYSPACE_NAME

API_SERVER_CFS = ConfigKeyspaceMap.get_issu_cfs(API_SERVER_KEYSPACE_NAME)
DM_CFS = ConfigKeyspaceMap.get_issu_cfs(DEVICE_MANAGER_KEYSPACE_NAME)
SCHEMA_CFS = ConfigKeyspaceMap.get_issu_cfs(SCHEMA_KEYSPACE_NAME)
SVC_MONITOR_CFS = ConfigKeyspaceMap.get_issu_cfs(SVC_MONITOR_KEYSPACE_NAME)
USERAGENT_CFS = ConfigKeyspaceMap.get_issu_cfs(USERAGENT_KEYSPACE_NAME)


def _myprint(x, level):
    prefix = SandeshLevel._VALUES_TO_NAMES[level] + " "
    logging.info(prefix + str(x))


def lognprint(x, level):
    print(x)
    prefix = SandeshLevel._VALUES_TO_NAMES[level] + " "
    logging.info(prefix + str(x))


logger = _myprint

# Apps register respective translation functions and import paths

issu_info_pre = [
    (None, API_SERVER_KEYSPACE_NAME, {cf: {} for cf in API_SERVER_CFS}),
    (None, SCHEMA_KEYSPACE_NAME, {cf: {} for cf in SCHEMA_CFS}),
    (None, USERAGENT_KEYSPACE_NAME, {cf: {} for cf in USERAGENT_CFS}),
    (None, SVC_MONITOR_KEYSPACE_NAME, {cf: {} for cf in SVC_MONITOR_CFS}),
    (None, DEVICE_MANAGER_KEYSPACE_NAME, {cf: {} for cf in DM_CFS}),
]

issu_keyspace_config_db_uuid = {
    API_SERVER_KEYSPACE_NAME: [(cf) for cf in API_SERVER_CFS]}

issu_keyspace_to_bgp_keyspace = {
    SCHEMA_KEYSPACE_NAME: [(cf) for cf in SCHEMA_CFS]}

issu_keyspace_svc_monitor_keyspace = {
    SVC_MONITOR_KEYSPACE_NAME: [(cf) for cf in SVC_MONITOR_CFS]}

issu_keyspace_user_agent = {
    USERAGENT_KEYSPACE_NAME: [(cf) for cf in USERAGENT_CFS]}

issu_keyspace_dm_keyspace = {
    DEVICE_MANAGER_KEYSPACE_NAME: [(cf) for cf in DM_CFS]}


issu_info_post = [
    (None, SCHEMA_KEYSPACE_NAME, {cf: {} for cf in SCHEMA_CFS}),
    (None, USERAGENT_KEYSPACE_NAME, {cf: {} for cf in USERAGENT_CFS}),
    (None, SVC_MONITOR_KEYSPACE_NAME, {cf: {} for cf in SVC_MONITOR_CFS}),
    (None, DEVICE_MANAGER_KEYSPACE_NAME, {cf: {} for cf in DM_CFS}),
]

issu_info_config_db_uuid = [
    (None, API_SERVER_KEYSPACE_NAME, {cf: {} for cf in API_SERVER_CFS}),
]

issu_znode_list = ['fq-name-to-uuid', 'api-server', 'id']


def parse_args(args_str=None):
    defaults = {
        'old_rabbit_user': 'guest',
        'old_rabbit_password': 'guest',
        'old_rabbit_ha_mode': False,
        'old_rabbit_q_name': 'vnc-config.issu-queue',
        'old_rabbit_vhost': None,
        'old_rabbit_port': '5672',
        'old_rabbit_use_ssl': False,
        'old_rabbit_ssl_version': None,
        'old_rabbit_ssl_ca_certs': None,
        'old_rabbit_ssl_keyfile': None,
        'old_rabbit_ssl_certfile': None,
        'new_rabbit_user': 'guest',
        'new_rabbit_password': 'guest',
        'new_rabbit_ha_mode': False,
        'new_rabbit_q_name': 'vnc-config.issu-queue',
        'new_rabbit_vhost': '',
        'new_rabbit_port': '5672',
        'new_rabbit_use_ssl': False,
        'new_rabbit_ssl_version': None,
        'new_rabbit_ssl_ca_certs': None,
        'new_rabbit_ssl_keyfile': None,
        'new_rabbit_ssl_certfile': None,
        'odb_prefix': '',
        'ndb_prefix': '',
        'reset_config': None,
        'old_cassandra_user': None,
        'old_cassandra_password': None,
        'old_cassandra_use_ssl': False,
        'old_cassandra_ca_certs': None,
        'new_cassandra_user': None,
        'new_cassandra_password': None,
        'new_cassandra_use_ssl': False,
        'new_cassandra_ca_certs': None,
        'old_cassandra_address_list': '10.84.24.35:9160',
        'old_zookeeper_address_list': '10.84.24.35:2181',
        'old_rabbit_address_list': '10.84.24.35',
        'new_cassandra_address_list': '10.84.24.35:9160',
        'new_zookeeper_address_list': '10.84.24.35:2181',
        'new_rabbit_address_list': '10.84.24.35',
        'new_api_info': '{"10.84.24.52": [("root"), ("c0ntrail123")]}'

    }
    if not args_str:
        args_str = ' '.join(sys.argv[1:])
    conf_parser = argparse.ArgumentParser(add_help=False)
    conf_parser.add_argument(
        "-c", "--conf_file", action='append',
        help="Specify config file", metavar="FILE")
    args, remaining_argv = conf_parser.parse_known_args(args_str.split())
    if args.conf_file:
        config = configparser.ConfigParser(strict=False)
        config.read(args.conf_file)
        defaults.update(dict(config.items("DEFAULTS")))

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

    parser.set_defaults(**defaults)
    parser.add_argument(
        "--old_rabbit_user",
        help="Old RMQ user name")
    parser.add_argument(
        "--old_rabbit_password",
        help="Old RMQ passwd")
    parser.add_argument(
        "--old_rabbit_ha_mode",
        help="Old RMQ HA mode")
    parser.add_argument(
        "--old_rabbit_q_name",
        help="Q name in old RMQ")
    parser.add_argument(
        "--old_rabbit_vhost",
        help="Old RMQ Vhost")
    parser.add_argument(
        "--old_rabbit_port",
        help="Old RMQ port")
    parser.add_argument(
        "--old_rabbit_use_ssl",
        help="Old RMQ use ssl flag")
    parser.add_argument(
        "--old_rabbit_ssl_ca_certs",
        help="Old RMQ SSL CA certs file path")
    parser.add_argument(
        "--old_rabbit_ssl_keyfile",
        help="Old RMQ SSL key file path")
    parser.add_argument(
        "--old_rabbit_ssl_certfile",
        help="Old RMQ SSL certificate file path")
    parser.add_argument(
        "--old_rabbit_ssl_version",
        help="Old RMQ SSL version")
    parser.add_argument(
        "--new_rabbit_user",
        help="New RMQ user name")
    parser.add_argument(
        "--new_rabbit_password",
        help="New RMQ passwd")
    parser.add_argument(
        "--new_rabbit_ha_mode",
        help="New RMQ HA mode")
    parser.add_argument(
        "--new_rabbit_q_name",
        help="Q name in new RMQ")
    parser.add_argument(
        "--new_rabbit_vhost",
        help="New RMQ Vhost")
    parser.add_argument(
        "--new_rabbit_port",
        help="New RMQ port")
    parser.add_argument(
        "--new_rabbit_use_ssl",
        help="New RMQ use ssl flag")
    parser.add_argument(
        "--new_rabbit_ssl_ca_certs",
        help="New RMQ SSL CA certs file path")
    parser.add_argument(
        "--new_rabbit_ssl_keyfile",
        help="New RMQ SSL key file path")
    parser.add_argument(
        "--new_rabbit_ssl_certfile",
        help="New RMQ SSL certificate file path")
    parser.add_argument(
        "--new_rabbit_ssl_version",
        help="New RMQ SSL version")
    parser.add_argument(
        "--old_cassandra_user",
        help="Old Cassandra user name")
    parser.add_argument(
        "--old_cassandra_password",
        help="Old Cassandra passwd")
    parser.add_argument(
        "--new_cassandra_user",
        help="New Cassandra user name")
    parser.add_argument(
        "--new_cassandra_password",
        help="New Cassandra passwd")
    parser.add_argument(
        "--old_cassandra_use_ssl",
        help="Old Cassandra use ssl flag")
    parser.add_argument(
        "--old_cassandra_ca_certs",
        help="Old Cassandra CA certs file path")
    parser.add_argument(
        "--new_cassandra_use_ssl",
        help="New Cassandra use ssl flag")
    parser.add_argument(
        "--new_cassandra_ca_certs",
        help="New Cassandra CA certs file path")
    parser.add_argument(
        "--old_rabbit_address_list",
        help="Old RMQ addresses")
    parser.add_argument(
        "--old_cassandra_address_list",
        help="Old Cassandra addresses",
        nargs='+')
    parser.add_argument(
        "--old_zookeeper_address_list",
        help="Old zookeeper addresses")
    parser.add_argument(
        "--new_rabbit_address_list",
        help="New RMQ addresses")
    parser.add_argument(
        "--new_cassandra_address_list",
        help="New Cassandra addresses",
        nargs='+')
    parser.add_argument(
        "--new_zookeeper_address_list",
        help="New zookeeper addresses")
    parser.add_argument(
        "--old_db_prefix",
        help="Old DB prefix")
    parser.add_argument(
        "--new_db_prefix",
        help="New DB prefix")
    parser.add_argument(
        "--reset_config",
        help="Reset config")
    parser.add_argument(
        "--new_api_info",
        help="New API info",
        nargs="+")
    args_obj, remaining_argv = parser.parse_known_args(remaining_argv)
    if args.conf_file:
        args_obj.config_sections = config
    if isinstance(args_obj.old_cassandra_address_list, str):
        args_obj.old_cassandra_address_list =\
            args_obj.old_cassandra_address_list.split()
    if isinstance(args_obj.new_cassandra_address_list, str):
        args_obj.new_cassandra_address_list =\
            args_obj.new_cassandra_address_list.split()
    args_obj.old_rabbit_use_ssl = (str(args_obj.old_rabbit_use_ssl).lower() == 'true')
    args_obj.new_rabbit_use_ssl = (str(args_obj.new_rabbit_use_ssl).lower() == 'true')
    args_obj.old_rabbit_ha_mode = (str(args_obj.old_rabbit_ha_mode).lower() == 'true')
    args_obj.new_rabbit_ha_mode = (str(args_obj.new_rabbit_ha_mode).lower() == 'true')
    args_obj.old_cassandra_use_ssl = (str(args_obj.old_cassandra_use_ssl).lower() == 'true')
    args_obj.new_cassandra_use_ssl = (str(args_obj.new_cassandra_use_ssl).lower() == 'true')

    return args_obj, remaining_argv


if __name__ == '__main__':
    parse_args()
