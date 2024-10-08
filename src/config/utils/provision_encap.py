#!/usr/bin/python3
#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#


import sys
import argparse
import configparser

from cfgm_common.exceptions import RefsExistError
from vnc_api.vnc_api import *
from vnc_admin_api import VncApiAdmin


class EncapsulationProvision:

    def __init__(self, args_str=None):
        self._args = None
        if not args_str:
            args_str = ' '.join(sys.argv[1:])
        self._parse_args(args_str)

        self._vnc_lib = VncApiAdmin(
            self._args.use_admin_api,
            self._args.admin_user, self._args.admin_password,
            self._args.admin_tenant_name,
            self._args.api_server_ip,
            self._args.api_server_port, '/',
            api_server_use_ssl=self._args.api_server_use_ssl)

        global_vrouter_fq_name = ['default-global-system-config',
                                  'default-global-vrouter-config']
        if self._args.oper == "add":
            encap_obj = EncapsulationPrioritiesType(
                    encapsulation=self._args.encap_priority.split(","))
            conf_obj = GlobalVrouterConfig(encapsulation_priorities=encap_obj,
                    vxlan_network_identifier_mode=self._args.vxlan_vn_id_mode,
                    fq_name=global_vrouter_fq_name)
            try:
                result = self._vnc_lib.global_vrouter_config_create(conf_obj)
                print('Created.UUID is {}'.format(result))
            except RefsExistError:
                print ("GlobalVrouterConfig Exists Already!")
                result = self._vnc_lib.global_vrouter_config_update(conf_obj)
                print('Updated.{}'.format(result))
            return
        elif self._args.oper != "add":
            encap_obj = EncapsulationPrioritiesType(encapsulation=[])
            conf_obj = GlobalVrouterConfig(encapsulation_priorities=encap_obj,
                    fq_name=global_vrouter_fq_name)
            result = self._vnc_lib.global_vrouter_config_update(conf_obj)
    # end __init__
    
    def _parse_args(self, args_str):
        '''
        Eg. python provision_encap.py 
                                        --api_server_ip 127.0.0.1
                                        --api_server_port 8082
                                        --api_server_use_ssl False
                                        --encap_priority "MPLSoUDP,MPLSoGRE,VXLAN"
                                        --vxlan_vn_id_mode "automatic"
                                        --oper <add | delete>
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
            'encap_priority': 'MPLSoUDP,MPLSoGRE,VXLAN',
            'vxlan_vn_id_mode' : 'automatic'
        }
        ksopts = {
            'admin_user': 'user1',
            'admin_password': 'password1',
            'admin_tenant_name': 'admin'
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

        parser.add_argument("--api_server_port", help="Port of api server")
        parser.add_argument("--api_server_use_ssl",
                        help="Use SSL to connect with API server")
        parser.add_argument(
            "--encap_priority", help="List of Encapsulation priority", required=True)
        parser.add_argument(
            "--vxlan_vn_id_mode", help="Virtual Network id type to be used")
        parser.add_argument(
            "--oper", default='add',help="Provision operation to be done(add or delete)")
        parser.add_argument(
            "--admin_user", help="Name of keystone admin user")
        parser.add_argument(
            "--admin_password", help="Password of keystone admin user")
        parser.add_argument(
            "--admin_tenant_name", help="Tenant name for keystone admin user")
        group = parser.add_mutually_exclusive_group()
        group.add_argument(
            "--api_server_ip", help="IP address of api server")
        group.add_argument("--use_admin_api",
                            default=False,
                            help = "Connect to local api-server on admin port",
                            action="store_true")

        self._args = parser.parse_args(remaining_argv)
        if not self._args.encap_priority:
            parser.error('encap_priority is required')

    # end _parse_args

# end class EncapsulationProvision


def main(args_str=None):
    EncapsulationProvision(args_str)
# end main

if __name__ == "__main__":
    main()
