#!/usr/bin/python3

#
# Copyright (c) 2018 Juniper Networks, Inc. All rights reserved.
#
# This file contains code to gather IPAM config from the fabric management
# virtual network
#

import logging
import time

from cfgm_common.exceptions import NoIdError
from netaddr import IPNetwork
from netifaces import AF_INET, ifaddresses, interfaces
from pyroute2 import IPRoute
from vnc_api.vnc_api import VncApi


class FilterModule(object):

    ZTP_EXCHANGE = 'device_ztp_exchange'
    ZTP_EXCHANGE_TYPE = 'direct'
    CONFIG_FILE_ROUTING_KEY = 'device_ztp.config.file'
    TFTP_FILE_ROUTING_KEY = 'device_ztp.tftp.file'
    ZTP_REQUEST_ROUTING_KEY = 'device_ztp.request'
    ZTP_RESPONSE_ROUTING_KEY = 'device_ztp.response.'
    AMQP_BUFFER_TIME = 55
    VNC_API_REQUEST_BUFFER_TIME = 60

    def filters(self):
        return {
            'ztp_dhcp_config': self.get_ztp_dhcp_config,
            'ztp_tftp_config': self.get_ztp_tftp_config,
            'create_tftp_file': self.create_tftp_file,
            'delete_tftp_file': self.delete_tftp_file,
            'create_dhcp_file': self.create_dhcp_file,
            'delete_dhcp_file': self.delete_dhcp_file,
            'restart_dhcp_server': self.restart_dhcp_server,
            'read_dhcp_leases_using_count': self.read_dhcp_leases_using_count,
            'read_dhcp_leases_using_info': self.read_dhcp_leases_using_info,
            'read_only_dhcp_leases': self.read_only_dhcp_leases,
            'remove_stale_pr_objects': self.remove_stale_pr_objects,
        }

    # Method to get interface name and configured ip address from
    # subnet/ip address from subnet.
    @classmethod
    def get_host_ip_and_name(cls, subnet):
        ip = IPRoute()
        lookup_ip = ''
        route_lst = ip.route('get',
                             dst=(subnet['subnet']['ip_prefix'] +
                                  '/' +
                                  str(subnet['subnet']['ip_prefix_len'])))
        for tup in route_lst[0]['attrs'] or []:
            if tup[0] == 'RTA_PREFSRC':
                lookup_ip = str(tup[1])

        for ifaceName in interfaces() or []:
            addresses = [i['addr'] for i in ifaddresses(ifaceName)
                         .setdefault(AF_INET, [{'addr': 'No IP addr'}])]
            if (addresses[0]) == lookup_ip.decode('utf-8'):
                return lookup_ip, ifaceName

    @classmethod
    def get_ztp_dhcp_config(cls, job_ctx, fabric_uuid):
        dhcp_config = {}
        try:
            vncapi = VncApi(auth_type=VncApi._KEYSTONE_AUTHN_STRATEGY,
                            auth_token=job_ctx.get('auth_token'))
            fabric = vncapi.fabric_read(id=fabric_uuid)
            fabric_dict = vncapi.obj_to_dict(fabric)

            # From here we get the 'management' type virtual network
            vn_uuid = None
            virtual_network_refs = fabric_dict.get(
                'virtual_network_refs') or []
            for virtual_net_ref in virtual_network_refs:
                if 'management' in virtual_net_ref['attr']['network_type']:
                    vn_uuid = virtual_net_ref['uuid']
                    break
            if vn_uuid is None:
                raise NoIdError("Cannot find mgmt virtual network on fabric")

            virtual_net = vncapi.virtual_network_read(id=vn_uuid)
            virtual_net_dict = vncapi.obj_to_dict(virtual_net)

            # Get the IPAM attached to the virtual network
            ipam_refs = virtual_net_dict.get('network_ipam_refs')
            if ipam_refs:
                ipam_ref = ipam_refs[0]
                ipam = vncapi.network_ipam_read(id=ipam_ref['uuid'])
                ipam_dict = vncapi.obj_to_dict(ipam)
                ipam_subnets = ipam_dict.get('ipam_subnets')
                if ipam_subnets:
                    dhcp_config['ipam_subnets'] = ipam_subnets.get('subnets')
                # To support multiple subnet and interface for DHCP, each dhcp
                # option is tagged with interface name. eg.
                # dhcp-option=set:eth0, <ip-range start> <ip-range end>.
                for subnet in dhcp_config['ipam_subnets']:
                    intf_ip, intf_name = cls.get_host_ip_and_name(subnet)
                    if intf_ip and intf_name:
                        subnet.update({'intf_ip': intf_ip})
                        subnet.update({'intf_name': intf_name})
                    cidr = subnet['subnet']['ip_prefix'] +\
                        "/" + str(subnet['subnet']['ip_prefix_len'])
                    ip = IPNetwork(cidr)
                    if len(ip) > 0:
                        subnet.update({'name': str(ip.ip).replace('.', '')})
            # Get static ip configuration for physical routers
            pr_refs = fabric.get_physical_router_back_refs() or []
            pr_uuids = [ref['uuid'] for ref in pr_refs]
            static_ips = {}
            for pr_uuid in pr_uuids:
                pr = vncapi.physical_router_read(id=pr_uuid)
                pr_dict = vncapi.obj_to_dict(pr)
                mac = pr_dict.get('physical_router_management_mac')
                ip = pr_dict.get('physical_router_management_ip')
                if mac and ip:
                    static_ips[ip] = mac
            if static_ips:
                dhcp_config['static_ips'] = static_ips

            # Get user-specified static ip configuration
            static_host_ips = {}
            dynamic_hosts = []
            job_input = job_ctx.get('job_input', {})
            device_to_ztp = job_input.get('device_to_ztp', [])
            for dev in device_to_ztp:
                mgmt_ip = dev.get('mgmt_ip')
                sernum = dev.get('serial_number')
                if sernum:
                    if mgmt_ip:
                        static_host_ips[mgmt_ip] = sernum
                    else:
                        dynamic_hosts.append(sernum)
            if static_host_ips:
                dhcp_config['static_host_ips'] = static_host_ips
            if dynamic_hosts:
                dhcp_config['dynamic_hosts'] = dynamic_hosts

        except Exception as ex:
            logging.error(
                "Error getting ZTP DHCP configuration: {}".format(ex))

        return dhcp_config
    # end get_ztp_dhcp_config

    @classmethod
    def get_ztp_tftp_config(cls, job_ctx, dev_password=None):
        tftp_config = {}
        if job_ctx:
            device_creds = job_ctx['job_input'].get('device_auth')
            if device_creds:
                password = device_creds['root_password']
                tftp_config['password'] = password
        if dev_password:
            tftp_config['password'] = dev_password
        return tftp_config
    # end get_ztp_tftp_config

    @classmethod
    def create_tftp_file(cls, file_contents, file_name,
                         fabric_name, job_ctx):
        return cls._publish_file(file_name, file_contents, 'create',
                                 cls.TFTP_FILE_ROUTING_KEY, fabric_name,
                                 job_ctx)
    # end create_tftp_file

    @classmethod
    def delete_tftp_file(cls, file_name, fabric_name, job_ctx):
        return cls._publish_file(file_name, '', 'delete',
                                 cls.TFTP_FILE_ROUTING_KEY,
                                 fabric_name, job_ctx)
    # end delete_tftp_file

    @classmethod
    def create_dhcp_file(cls, file_contents, file_name,
                         fabric_name, job_ctx):
        return cls._publish_file(file_name, file_contents, 'create',
                                 cls.CONFIG_FILE_ROUTING_KEY, fabric_name,
                                 job_ctx)
    # end create_dhcp_file

    @classmethod
    def delete_dhcp_file(cls, file_name, fabric_name, job_ctx):
        return cls._publish_file(file_name, '', 'delete',
                                 cls.CONFIG_FILE_ROUTING_KEY,
                                 fabric_name, job_ctx)
    # end delete_dhcp_file

    @classmethod
    def read_dhcp_leases_using_count(cls, device_count, ipam_subnets,
                                     file_name, fabric_name, job_ctx):
        return cls.read_dhcp_leases(ipam_subnets, file_name, fabric_name,
                                    job_ctx, 'device_count', int(device_count))
    # end read_dhcp_leases_using_count

    @classmethod
    def read_dhcp_leases_using_info(cls, device_to_ztp, ipam_subnets,
                                    file_name, fabric_name, job_ctx):
        return cls.read_dhcp_leases(ipam_subnets, file_name, fabric_name,
                                    job_ctx, 'device_to_ztp', device_to_ztp)
    # end read_dhcp_leases_using_info

    @classmethod
    def read_only_dhcp_leases(cls, device_to_ztp, ipam_subnets, file_name,
                              fabric_name, job_ctx):
        return cls.read_dhcp_leases(ipam_subnets, file_name, fabric_name,
                                    job_ctx, 'device_to_ztp', device_to_ztp,
                                    action='read')
    # end read_only_dhcp_leases

    @classmethod
    def read_dhcp_leases(cls, ipam_subnets, file_name, fabric_name, job_ctx,
                         payload_key, payload_value, action='create'):
        vnc_api = VncApi(auth_type=VncApi._KEYSTONE_AUTHN_STRATEGY,
                         auth_token=job_ctx.get('auth_token'))
        timeout = job_ctx.get('job_input', {}).get('ztp_timeout', 0)
        # get the ztp_timeout from config properties if not defined in ztp.yml
        if timeout == 0:
            config_props = vnc_api.config_properties_read(
                fq_name=['default-global-system-config', 'config_property'])
            props_list = config_props.properties.key_value_pair
            for props in props_list or []:
                if props.key == 'ztp_timeout':
                    timeout = int(props.value)
        vnc_api = VncApi(auth_type=VncApi._KEYSTONE_AUTHN_STRATEGY,
                         auth_token=job_ctx.get('auth_token'),
                         timeout=timeout + cls.VNC_API_REQUEST_BUFFER_TIME)
        headers = {
            'fabric_name': fabric_name,
            'file_name': file_name,
            'action': action
        }
        payload = {
            'ipam_subnets': ipam_subnets,
            'ztp_timeout': timeout
        }
        payload[payload_key] = payload_value
        amqp_timeout = timeout + cls.AMQP_BUFFER_TIME
        return vnc_api.amqp_request(
            exchange=cls.ZTP_EXCHANGE,
            exchange_type=cls.ZTP_EXCHANGE_TYPE,
            routing_key=cls.ZTP_REQUEST_ROUTING_KEY,
            response_key=cls.ZTP_RESPONSE_ROUTING_KEY + fabric_name,
            headers=headers, payload=payload, amqp_timeout=amqp_timeout)
    # end read_dhcp_leases

    @classmethod
    def restart_dhcp_server(cls, file_name, fabric_name, job_ctx):
        vnc_api = VncApi(auth_type=VncApi._KEYSTONE_AUTHN_STRATEGY,
                         auth_token=job_ctx.get('auth_token'))
        headers = {
            'fabric_name': fabric_name,
            'file_name': file_name,
            'action': 'delete'
        }
        vnc_api.amqp_publish(exchange=cls.ZTP_EXCHANGE,
                             exchange_type=cls.ZTP_EXCHANGE_TYPE,
                             routing_key=cls.ZTP_REQUEST_ROUTING_KEY,
                             headers=headers,
                             payload={})
        return {'status': 'success'}
    # end restart_dhcp_server

    @classmethod
    def remove_stale_pr_objects(cls, job_ctx):
        """
        Clean  up stale temporary PR objects when
        ZTP workflow fails.
        """
        filters = {}

        try:
            vnc_api = VncApi(auth_type=VncApi._KEYSTONE_AUTHN_STRATEGY,
                             auth_token=job_ctx.get('auth_token'))
        except Exception as ex:
            logging.error("Error connecting to API server: {}".format(ex))
            return True

        # A case was noticed where the object deletion is attempted
        # before it is even created. To avoid this, wait for a
        # couple of seconds before trying to delete the PR
        time.sleep(2)

        filters['physical_router_managed_state'] = "dhcp"
        pr_list = vnc_api.physical_routers_list(
            filters=filters).get('physical-routers')

        for pr in pr_list:
            vnc_api.physical_router_delete(id=pr['uuid'])
            logging.info("Router {} in dhcp state deleted".format(
                pr['fq_name'][-1]))

        return True
    # end remove_stale_pr_objects

    @classmethod
    def _publish_file(cls, name, contents, action, routing_key,
                      fabric_name, job_ctx):
        vnc_api = VncApi(auth_type=VncApi._KEYSTONE_AUTHN_STRATEGY,
                         auth_token=job_ctx.get('auth_token'))
        headers = {
            'fabric_name': fabric_name,
            'file_name': name,
            'action': action
        }
        vnc_api.amqp_publish(exchange=cls.ZTP_EXCHANGE,
                             exchange_type=cls.ZTP_EXCHANGE_TYPE,
                             routing_key=routing_key, headers=headers,
                             payload=contents)
        return {'status': 'success'}
    # end _publish_file
