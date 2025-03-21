#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#
from . import test_case
from vnc_api.vnc_api import *
from device_api.juniper_common_xsd import *
from cfgm_common.tests.test_common import retries

#
# All Generaric utlity method shoud go here
#
class VerifyCommonDM(object):
    def __init__(self):
        pass

    @retries(5)
    def wait_to_get_object(self, obj_class, obj_name, obj_dict=None):
        if not obj_dict:
           obj_dict = obj_class._dict
        if obj_name not in obj_dict:
            raise Exception('%s not found' % obj_name)

    @retries(5)
    def wait_to_delete_object(self, obj_class, obj_name, obj_dict=None):
        if not obj_dict:
           obj_dict = obj_class._dict
        if obj_name in obj_dict:
            raise Exception('%s still found' % obj_name)


class TestCommonDM(test_case.DMTestCase):

    def set_obj_param(self, obj, param, value):
        fun = getattr(obj, "set_" + param)
        fun(value)
    # end set_obj_param

    def get_obj_param(self, obj, param):
        fun = getattr(obj, "get_" + param)
        return fun()
    # end get_obj_param

    def get_bgp_groups(self, config, bgp_type=''):
        protocols = config.get_protocols()
        bgp = protocols.get_bgp()
        bgp_groups = bgp.get_group()
        grps = []
        for gp in bgp_groups or []:
            if not bgp_type or bgp_type == gp.get_type():
                grps.append(gp)
        return grps
    # end get_bgp_groups

    def get_dynamic_tunnels(self, config):
        ri_opts = config.get_routing_options()
        if not ri_opts:
            return None
        return ri_opts.get_dynamic_tunnels()
    # end get_dynamic_tunnels

    def get_routing_instances(self, config, ri_name=''):
        ri_list  = config.get_routing_instances()
        if ri_list:
            ri_list = ri_list.get_instance() or []
        ris = []
        for ri in ri_list or []:
            if not ri_name or ri.get_name() == ri_name:
                ris.append(ri)
        return ris

    def get_interfaces(self, config, name=''):
        interfaces = config.get_interfaces()
        if not interfaces:
            return []
        interfaces = interfaces.get_interface()
        intfs = []
        for intf in interfaces or []:
            if not name or name == intf.get_name():
                intfs.append(intf)
        return intfs
    # end get_interfaces

    def get_ip_list(self, intf, ip_type='v4', unit_name=''):
        units = intf.get_unit() or []
        ips = []
        for ut in units:
            if unit_name and ut.get_name() != unit_name:
                continue
            f = ut.get_family() or Family()
            inet = None
            if ip_type == 'v4':
                inet = f.get_inet()
            else:
                inet = f.get_inet6()
            addrs = inet.get_address() or []
            for a in addrs:
                ips.append(a.get_name())
        return ips

    def set_hold_time(self, bgp_router, value):
        params = self.get_obj_param(bgp_router, 'bgp_router_parameters') or BgpRouterParams()
        self.set_obj_param(params, 'hold_time', 100)
        self.set_obj_param(bgp_router, 'bgp_router_parameters', params)
    # end set_hold_time

    def set_auth_data(self, bgp_router, token, password, auth_type):
        params = self.get_obj_param(bgp_router, 'bgp_router_parameters') or BgpRouterParams()
        key = AuthenticationKeyItem(token, password)
        self.set_obj_param(params, 'auth_data', AuthenticationData(auth_type, [key]))
        self.set_obj_param(bgp_router, 'bgp_router_parameters', params)
    # end set_auth_data

# end
