#!/usr/bin/python3
#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#


import sys
import time
import argparse
import configparser

from vnc_api.vnc_api import *
from vnc_admin_api import VncApiAdmin
from cfgm_common.exceptions import *


class DevicemgrNodeProvisioner:

    def __init__(self, args_str=None):
        self._args = None
        if not args_str:
            args_str = ' '.join(sys.argv[1:])
        self._parse_args(args_str)

        connected = False
        tries = 0
        while not connected:
            try:
                self._vnc_lib = VncApiAdmin(
                    self._args.use_admin_api,
                    self._args.admin_user, self._args.admin_password,
                    self._args.admin_tenant_name,
                    self._args.api_server_ip,
                    self._args.api_server_port, '/',
                    auth_host=self._args.openstack_ip,
                    api_server_use_ssl=self._args.api_server_use_ssl)
                connected = True
            except ResourceExhaustionError: # haproxy throws 503
                if tries < 10:
                    tries += 1
                    time.sleep(3)
                else:
                    raise

        gsc_obj = self._vnc_lib.global_system_config_read(
            fq_name=['default-global-system-config'])
        self._global_system_config_obj = gsc_obj

        if self._args.oper == 'add':
            self.add_devicemgr_node()
        elif self._args.oper == 'del':
            self.del_devicemgr_node()
        else:
            print("Unknown operation {}. Only 'add' and 'del' supported".format(self._args.oper))

    # end __init__

    def _parse_args(self, args_str):
        '''
        Eg. python provision_devicemgr_node.py --host_name a3s30.contrail.juniper.net
                                        --host_ip 10.1.1.1
                                        --api_server_ip 127.0.0.1
                                        --api_server_port 8082
                                        --api_server_use_ssl False
                                        --oper <add | del>
        '''

        # Source any specified config/ini file
        # Turn off help, so we print all options in response to -h
        conf_parser = argparse.ArgumentParser(add_help=False)

        conf_parser.add_argument("-c", "--conf_file",
                                 help="Specify config file", metavar="FILE")
        args, remaining_argv = conf_parser.parse_known_args(args_str.split())

        defaults = {
            'api_server_ip': '127.0.0.1',
            'api_server_port': '8082',
            'api_server_use_ssl': False,
            'oper': 'add',
        }
        ksopts = {
            'admin_user': 'user1',
            'admin_password': 'password1',
            'admin_tenant_name': 'default-domain'
        }

        if args.conf_file:
            config = configparser.ConfigParser(strict=False)
            config.read([args.conf_file])
            defaults.update(dict(config.items("DEFAULTS")))
            if 'KEYSTONE' in config.sections():
                ksopts.update(dict(config.items("KEYSTONE")))

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
        parser.set_defaults(**defaults)

        parser.add_argument(
            "--host_name", help="hostname name of devicemgr node", required=True)
        parser.add_argument("--host_ip", help="IP address of devicemgr node", required=True)
        parser.add_argument("--api_server_port", help="Port of api server")
        parser.add_argument("--api_server_use_ssl",
                        help="Use SSL to connect with API server")
        parser.add_argument(
            "--oper", default='add',
            help="Provision operation to be done(add or del)")
        parser.add_argument(
            "--admin_user", help="Name of keystone admin user")
        parser.add_argument(
            "--admin_password", help="Password of keystone admin user")
        parser.add_argument(
            "--admin_tenant_name", help="Tenamt name for keystone admin user")
        parser.add_argument(
            "--openstack_ip", help="IP address of openstack node")
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument(
            "--api_server_ip", help="IP address of api server",
            nargs='+', type=str)
        group.add_argument("--use_admin_api",
                            default=False,
                            help = "Connect to local api-server on admin port",
                            action="store_true")

        self._args = parser.parse_args(remaining_argv)

    # end _parse_args

    def add_devicemgr_node(self):
        gsc_obj = self._global_system_config_obj

        dmgr_node_obj = DevicemgrNode(
            self._args.host_name, gsc_obj,
            devicemgr_node_ip_address=self._args.host_ip)
        dmgr_node_exists = True
        try:
            dmgr_node_obj = self._vnc_lib.devicemgr_node_read(
                fq_name=dmgr_node_obj.get_fq_name())
        except NoIdError:
            dmgr_node_exists = False

        if dmgr_node_exists:
            self._vnc_lib.devicemgr_node_update(dmgr_node_obj)
        else:
            try:
                self._vnc_lib.devicemgr_node_create(dmgr_node_obj)
            except RefsExistError:
                print("Already created!")

    # end add_dmgr_node

    def del_devicemgr_node(self):
        gsc_obj = self._global_system_config_obj
        dmgr_node_obj = DevicemgrNode(self._args.host_name, gsc_obj)
        self._vnc_lib.devicemgr_node_delete(
            fq_name=dmgr_node_obj.get_fq_name())
    # end del_devicemgr_node

# end class DmgrNodeProvisioner


def main(args_str=None):
    DevicemgrNodeProvisioner(args_str)
# end main

if __name__ == "__main__":
    main()
