import os
import sys
import socket
import errno
import uuid
import logging
import mock


import testtools
from testtools.matchers import Equals, MismatchError, Not, Contains
from testtools import content, content_type, ExpectedException
import unittest
import re
import json
import copy
import inspect
import requests
import bottle

from vnc_cfg_api_server.vnc_addr_mgmt import *
from vnc_api.vnc_api import *
import vnc_api.gen.vnc_api_test_gen
from vnc_api.gen.resource_test import *
import cfgm_common
from cfgm_common.tests import test_common
from cfgm_common import vnc_cgitb
from cfgm_common import rest
vnc_cgitb.enable(format='text')

from . import test_case

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)


class TestSubnetUpdate(test_case.ApiServerTestCase):

    @classmethod
    def setUpClass(cls, *args, **kwargs):
        cls.console_handler = logging.StreamHandler()
        cls.console_handler.setLevel(logging.DEBUG)
        logger.addHandler(cls.console_handler)
        super(TestSubnetUpdate, cls).setUpClass(*args, **kwargs)

    @classmethod
    def tearDownClass(cls, *args, **kwargs):
        logger.removeHandler(cls.console_handler)
        super(TestSubnetUpdate, cls).tearDownClass(*args, **kwargs)

    def test_iip(self):
        """ Change subnet size from 24 to 23 with exist FIP's
        Scenario:
        1. Create VN with /24 subnet
        2. Create FIP
        3. Update subnet from /24 to /23
        """
        project = Project('v4-proj-%s' % (self.id()), Domain())
        self._vnc_lib.project_create(project)

        ipam_subnet = IpamSubnetType(subnet=SubnetType('10.1.2.0', 24))
        ipam0 = NetworkIpam('ipam0', project, IpamType("dhcp"))
        self._vnc_lib.network_ipam_create(ipam0)
        vn = VirtualNetwork('vn0', project)
        vn.add_network_ipam(ipam0, VnSubnetsType([ipam_subnet]))
        self._vnc_lib.virtual_network_create(vn)
        vn = self._vnc_lib.virtual_network_read(id=vn.uuid)
        # Create IIP
        iip_obj = InstanceIp(name=str(uuid.uuid4()), instance_ip_family='v4')
        iip_obj.uuid = iip_obj.name
        iip_obj.add_network_ipam(ipam0)
        iip_obj.add_virtual_network(vn)
        logger.debug('Created V4 Instance IP object %s', iip_obj.uuid)
        iip_id = self._vnc_lib.instance_ip_create(iip_obj)
        iip_obj = self._vnc_lib.instance_ip_read(id=iip_id)
        ip_addr = iip_obj.get_instance_ip_address()
        logger.debug('Created V4 Instance IP address %s', ip_addr)
        vm_obj = VirtualMachine('vm-%s' % (self.id()))
        self._vnc_lib.virtual_machine_create(vm_obj)
        vm_vmi_obj = VirtualMachineInterface('vm-vmi-%s' % (self.id()),
                                             project)
        vm_vmi_obj.add_virtual_network(vn)
        vm_vmi_obj.add_virtual_machine(vm_obj)
        self._vnc_lib.virtual_machine_interface_create(vm_vmi_obj)

        rtr_vmi_obj = VirtualMachineInterface('rtr-vmi-%s' % (self.id()),
                                              project)
        rtr_vmi_obj.add_virtual_network(vn)
        self._vnc_lib.virtual_machine_interface_create(rtr_vmi_obj)
        lr_obj = LogicalRouter('rtr-%s' % (self.id()), project)
        lr_obj.set_logical_router_type('vxlan-routing')
        lr_obj.add_virtual_machine_interface(rtr_vmi_obj)
        self._vnc_lib.logical_router_create(lr_obj)
        vn.network_ipam_refs[0][
            'attr'].ipam_subnets[0].subnet.ip_prefix_len = 23
        vn._pending_field_updates.add('network_ipam_refs')
        self._vnc_lib.virtual_network_update(vn)
        assert vn.network_ipam_refs[0]['attr'].ipam_subnets[0].subnet.ip_prefix_len == 23

    # end test_subnet_overlap

    def test_multiple_subnets(self):
        """ Update subnet with multiple subnets

        """
        project = Project('v4-proj-%s' % (self.id()), Domain())
        self._vnc_lib.project_create(project)
        ipam_subnet0 = IpamSubnetType(subnet=SubnetType('10.1.2.0', 24))
        ipam_subnet1 = IpamSubnetType(subnet=SubnetType('192.1.2.0', 24))
        ipam_subnet2 = IpamSubnetType(subnet=SubnetType('192.168.2.0', 24))
        ipam0 = NetworkIpam('ipam0', project, IpamType("dhcp"))

        self._vnc_lib.network_ipam_create(ipam0)
        vn = VirtualNetwork('vn0', project)
        vn.add_network_ipam(ipam0, VnSubnetsType([ipam_subnet0,
                                                  ipam_subnet1,
                                                  ipam_subnet2]))
        self._vnc_lib.virtual_network_create(vn)
        vn = self._vnc_lib.virtual_network_read(id=vn.uuid)
        # Create IIP
        iip_obj = InstanceIp(name=str(uuid.uuid4()), instance_ip_family='v4')
        iip_obj.uuid = iip_obj.name
        iip_obj.add_network_ipam(ipam0)
        iip_obj.add_virtual_network(vn)
        # Create IIP
        iip_obj = InstanceIp(name=str(uuid.uuid4()), instance_ip_family='v4')
        iip_obj.uuid = iip_obj.name
        iip_obj.add_virtual_network(vn)
        logger.debug('Created V4 Instance IP object %s', iip_obj.uuid)
        iip_id = self._vnc_lib.instance_ip_create(iip_obj)
        iip_obj = self._vnc_lib.instance_ip_read(id=iip_id)
        ip_addr = iip_obj.get_instance_ip_address()
        logger.debug('Created V4 Instance IP address %s', ip_addr)
        vm_obj = VirtualMachine('vm-%s' % (self.id()))
        self._vnc_lib.virtual_machine_create(vm_obj)
        vm_vmi_obj = VirtualMachineInterface('vm-vmi-%s' % (self.id()),
                                             project)
        vm_vmi_obj.add_virtual_network(vn)
        vm_vmi_obj.add_virtual_machine(vm_obj)
        self._vnc_lib.virtual_machine_interface_create(vm_vmi_obj)

        rtr_vmi_obj = VirtualMachineInterface('rtr-vmi-%s' % (self.id()),
                                              project)
        rtr_vmi_obj.add_virtual_network(vn)
        self._vnc_lib.virtual_machine_interface_create(rtr_vmi_obj)
        lr_obj = LogicalRouter('rtr-%s' % (self.id()), project)
        lr_obj.set_logical_router_type('vxlan-routing')
        lr_obj.add_virtual_machine_interface(rtr_vmi_obj)
        self._vnc_lib.logical_router_create(lr_obj)
        vn.network_ipam_refs[0][
            'attr'].ipam_subnets[0].subnet.ip_prefix_len = 22
        vn.network_ipam_refs[0][
            'attr'].ipam_subnets[1].subnet.ip_prefix_len = 21
        vn._pending_field_updates.add('network_ipam_refs')
        self._vnc_lib.virtual_network_update(vn)
        assert vn.network_ipam_refs[0]['attr'].ipam_subnets[0].subnet.ip_prefix_len == 22
        assert vn.network_ipam_refs[0]['attr'].ipam_subnets[1].subnet.ip_prefix_len == 21

    def test_ip_not_in_subnet(self):
        """Update subnet with FIP and FIP not in a new subnet

        """
        project = Project('v4-proj-%s' % (self.id()), Domain())
        self._vnc_lib.project_create(project)

        ipam_subnet = IpamSubnetType(subnet=SubnetType('10.1.2.0', 24))
        ipam0 = NetworkIpam('ipam0', project, IpamType("dhcp"))
        self._vnc_lib.network_ipam_create(ipam0)
        vn = VirtualNetwork('vn0', project)
        vn.add_network_ipam(ipam0, VnSubnetsType([ipam_subnet]))
        self._vnc_lib.virtual_network_create(vn)
        vn = self._vnc_lib.virtual_network_read(id=vn.uuid)
        # Create IIP
        iip_obj = InstanceIp(name=str(uuid.uuid4()), instance_ip_family='v4')
        iip_obj.uuid = iip_obj.name
        iip_obj.add_network_ipam(ipam0)
        iip_obj.add_virtual_network(vn)
        logger.debug('Created V4 Instance IP object %s', iip_obj.uuid)
        iip_id = self._vnc_lib.instance_ip_create(iip_obj)
        iip_obj = self._vnc_lib.instance_ip_read(id=iip_id)
        ip_addr = iip_obj.get_instance_ip_address()
        logger.debug('Created V4 Instance IP address %s', ip_addr)
        vm_obj = VirtualMachine('vm-%s' % (self.id()))
        self._vnc_lib.virtual_machine_create(vm_obj)
        vm_vmi_obj = VirtualMachineInterface('vm-vmi-%s' % (self.id()),
                                             project)
        vm_vmi_obj.add_virtual_network(vn)
        vm_vmi_obj.add_virtual_machine(vm_obj)
        self._vnc_lib.virtual_machine_interface_create(vm_vmi_obj)
        vn.network_ipam_refs[0][
            'attr'].ipam_subnets[0].subnet.ip_prefix_len = 30
        vn._pending_field_updates.add('network_ipam_refs')
        with ExpectedException(cfgm_common.exceptions.RefsExistError) as e:
            self._vnc_lib.virtual_network_update(vn)

    def test_one_of_ips_not_in_subnet(self):
        """Update subnet with multiple FIP's when one of the IP's not in a new subnet

        """
        project = Project('v4-proj-%s' % (self.id()), Domain())
        self._vnc_lib.project_create(project)
        ipam_subnet0 = IpamSubnetType(subnet=SubnetType('10.1.2.0', 24))
        ipam_subnet1 = IpamSubnetType(subnet=SubnetType('192.1.2.0', 24))
        ipam_subnet2 = IpamSubnetType(subnet=SubnetType('192.168.2.0', 24))
        ipam0 = NetworkIpam('ipam0', project)

        self._vnc_lib.network_ipam_create(ipam0)
        vn = VirtualNetwork('vn0', project)
        vn.add_network_ipam(ipam0, VnSubnetsType([ipam_subnet0,
                                                  ipam_subnet1,
                                                  ipam_subnet2]))
        self._vnc_lib.virtual_network_create(vn)
        vn = self._vnc_lib.virtual_network_read(id=vn.uuid)
        # Create IIP
        iip_obj = InstanceIp(name=str(uuid.uuid4()), instance_ip_family='v4')
        iip_obj.uuid = iip_obj.name
        iip_obj.add_network_ipam(ipam0)
        iip_obj.add_virtual_network(vn)
        # Create IIP
        iip_obj = InstanceIp(name=str(uuid.uuid4()), instance_ip_family='v4')
        iip_obj.uuid = iip_obj.name
        iip_obj.add_virtual_network(vn)
        logger.debug('Created V4 Instance IP object %s', iip_obj.uuid)
        iip_id = self._vnc_lib.instance_ip_create(iip_obj)
        iip_obj = self._vnc_lib.instance_ip_read(id=iip_id)
        ip_addr = iip_obj.get_instance_ip_address()
        logger.debug('Created V4 Instance IP address %s', ip_addr)
        vm_obj = VirtualMachine('vm-%s' % (self.id()))
        self._vnc_lib.virtual_machine_create(vm_obj)
        vm_vmi_obj = VirtualMachineInterface('vm-vmi-%s' % (self.id()),
                                             project)
        vm_vmi_obj.add_virtual_network(vn)
        vm_vmi_obj.add_virtual_machine(vm_obj)
        self._vnc_lib.virtual_machine_interface_create(vm_vmi_obj)
        vn.network_ipam_refs[0][
            'attr'].ipam_subnets[0].subnet.ip_prefix_len = 28
        vn.network_ipam_refs[0][
            'attr'].ipam_subnets[1].subnet.ip_prefix_len = 21
        vn.network_ipam_refs[0][
            'attr'].ipam_subnets[2].subnet.ip_prefix_len = 22
        vn._pending_field_updates.add('network_ipam_refs')
        with ExpectedException(cfgm_common.exceptions.RefsExistError) as e:
            self._vnc_lib.virtual_network_update(vn)

    def test_fip(self):
        """Update subnet with fip pool

        """
        # prep objects for testing
        proj_obj = Project('proj-%s' % (self.id()), parent_obj=Domain())
        self._vnc_lib.project_create(proj_obj)

        ipam_obj = NetworkIpam('ipam-%s' % (self.id()), proj_obj)
        self._vnc_lib.network_ipam_create(ipam_obj)

        vn_obj = VirtualNetwork('vn-%s' % (self.id()), proj_obj)
        ipam_sn_v4 = IpamSubnetType(subnet=SubnetType('11.1.1.0', 24))
        vn_obj.add_network_ipam(ipam_obj, VnSubnetsType([ipam_sn_v4]))
        self._vnc_lib.virtual_network_create(vn_obj)

        fip_pool_obj = FloatingIpPool(
            'fip-pool-%s' % (self.id()), parent_obj=vn_obj)
        self._vnc_lib.floating_ip_pool_create(fip_pool_obj)

        aip_pool_obj = AliasIpPool(
            'aip-pool-%s' % (self.id()), parent_obj=vn_obj)
        self._vnc_lib.alias_ip_pool_create(aip_pool_obj)

        iip_obj = InstanceIp('iip-%s' % (self.id()))
        iip_obj.add_virtual_network(vn_obj)
        self._vnc_lib.instance_ip_create(iip_obj)
        # read-in to find allocated address
        iip_obj = self._vnc_lib.instance_ip_read(id=iip_obj.uuid)

        fip_obj = FloatingIp('fip-%s' % (self.id()), fip_pool_obj)
        fip_obj.add_project(proj_obj)
        self._vnc_lib.floating_ip_create(fip_obj)
        # read-in to find allocated address
        fip_obj = self._vnc_lib.floating_ip_read(id=fip_obj.uuid)

        aip_obj = AliasIp('aip-%s' % (self.id()), aip_pool_obj)
        aip_obj.add_project(proj_obj)
        self._vnc_lib.alias_ip_create(aip_obj)
        # read-in to find allocated address
        aip_obj = self._vnc_lib.alias_ip_read(id=aip_obj.uuid)

        vm_obj = VirtualMachine('vm-%s' % (self.id()))
        self._vnc_lib.virtual_machine_create(vm_obj)

        vm_vmi_obj = VirtualMachineInterface('vm-vmi-%s' % (self.id()),
                                             proj_obj)
        vm_vmi_obj.add_virtual_network(vn_obj)
        vm_vmi_obj.add_virtual_machine(vm_obj)
        self._vnc_lib.virtual_machine_interface_create(vm_vmi_obj)

        rtr_vmi_obj = VirtualMachineInterface('rtr-vmi-%s' % (self.id()),
                                              proj_obj)
        rtr_vmi_obj.add_virtual_network(vn_obj)
        self._vnc_lib.virtual_machine_interface_create(rtr_vmi_obj)
        lr_obj = LogicalRouter('rtr-%s' % (self.id()), proj_obj)
        lr_obj.set_logical_router_type('vxlan-routing')
        lr_obj.add_virtual_machine_interface(rtr_vmi_obj)
        self._vnc_lib.logical_router_create(lr_obj)

        isolated_vmi_obj = VirtualMachineInterface('iso-vmi-%s' % (self.id()),
                                                   proj_obj)
        isolated_vmi_obj.add_virtual_network(vn_obj)
        self._vnc_lib.virtual_machine_interface_create(isolated_vmi_obj)
        vn = self._vnc_lib.virtual_network_read(id=vn_obj.uuid)
        vn.network_ipam_refs[0][
            'attr'].ipam_subnets[0].subnet.ip_prefix_len = 23
        vn._pending_field_updates.add('network_ipam_refs')
        self._vnc_lib.virtual_network_update(vn)


if __name__ == '__main__':
    ch = logging.StreamHandler()
    ch.setLevel(logging.DEBUG)
    logger.addHandler(ch)

    unittest.main()
