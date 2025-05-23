import unittest
import uuid

import bottle
from cfgm_common.tests.test_utils import FakeKazooClient
from flexmock import flexmock
from vnc_openstack import neutron_plugin_db as db


class MockDbInterface(db.DBInterface):

    def __init__(self):
        """Create mock DbInterface object."""
        class MockConnection(object):
            def wait(self):
                return

        self._connected_to_api_server = MockConnection()
        self._zookeeper_client = FakeKazooClient()
        self.security_group_lock_prefix = '/vnc_cfg_api_locks/security_group'
        pass


class TestDbInterface(unittest.TestCase):
    _tenant_ids = ['tenant_id_1',
                   'tenant_id_2']

    def _list_resource(self, resource, ret_count=0):
        def _list_others(parent_id, count):
            self.assertEqual(count, True)
            self.assertTrue(parent_id in self._tenant_ids)
            self.assertTrue(resource in ['virtual_networks',
                                         'virtual_machine_interfaces',
                                         'logical_routers',
                                         'network_policys',
                                         'network_ipams',
                                         'route_tables'])
            r = resource.replace("_", "-")
            return {r: {'count': ret_count}}

        def _list_fip(back_ref_id, count):
            self.assertEqual(count, True)
            self.assertTrue(back_ref_id in self._tenant_ids)

            r = resource.replace("_", "-")
            return {r: {'count': ret_count}}

        if resource == "floating_ips":
            return _list_fip

        return _list_others

    def _test_for(self, resource):
        dbi = MockDbInterface()

        kwargs = {"operational": True,
                  resource + "_list": self._list_resource(resource, 1),
                  }
        dbi._vnc_lib = flexmock(**kwargs)

        ret = dbi._resource_count_optimized(
            resource, filters={
                'tenant_id': self._tenant_ids[0]})
        self.assertEqual(ret, 1)

        ret = dbi._resource_count_optimized(
            resource, filters={'tenant_id': self._tenant_ids})
        self.assertEqual(ret, 2)

    def test_resource_count_optimized(self):
        dbi = MockDbInterface()

        ret = dbi._resource_count_optimized('virtual-networks',
                                            filters={'f': 'some-filter'})
        self.assertEqual(ret, None)

        ret = dbi._resource_count_optimized('virtual-networks',
                                            filters={'tenant_id': 'some-id',
                                                     'f': 'some_filter'})
        self.assertEqual(ret, None)

        self._test_for("virtual_networks")
        self._test_for("virtual_machine_interfaces")
        self._test_for("floating_ips")
        self._test_for("logical_routers")
        self._test_for("network_policys")
        self._test_for("network_ipams")
        self._test_for("route_tables")

    def test_floating_show_router_id(self):
        dbi = MockDbInterface()

        def fake_virtual_machine_interface_properties():
            return None

        def fake_virtual_machine_read(id, fq_name=None, fields=None,
                                      parent_id=None):
            if id == 'fip_port_uuid1':
                net_uuid = 'match_vn_uuid'
            elif id == 'fip_port_uuid2':
                net_uuid = 'miss_vn_uuid'
            elif id == 'router_port_uuid':
                net_uuid = 'match_vn_uuid'
            return flexmock(
                uuid=id,
                get_virtual_machine_interface_properties=(
                    fake_virtual_machine_interface_properties),
                get_virtual_network_refs=lambda: [
                    {
                        'uuid': net_uuid}])

        def fake_virtual_machine_interface_list(*args, **kwargs):
            obj_uuids = kwargs.get('obj_uuids', [])
            if 'router_port_uuid' in obj_uuids:
                return [
                    flexmock(
                        uuid='router_port_uuid',
                        get_virtual_machine_interface_properties=(
                            fake_virtual_machine_interface_properties),
                        get_virtual_network_refs=(
                            lambda: [{'uuid': 'match_vn_uuid'}]))]
            return []

        dbi._vnc_lib = flexmock(
            fq_name_to_id=lambda res, name: 'fip_pool_uuid',
            virtual_machine_interface_read=fake_virtual_machine_read,
            virtual_machine_interfaces_list=(
                fake_virtual_machine_interface_list),
            logical_routers_list=lambda parent_id, detail: [
                flexmock(uuid='router_uuid',
                         get_virtual_machine_interface_refs=lambda: [
                             {'uuid': 'router_port_uuid'}])])

        id_perms_obj = flexmock(
            uuid='id_perms_uuid',
            get_created=lambda: 'create_time',
            get_last_modified=lambda: 'last_modified_time',
            get_description=lambda: 'description')

        fip_obj = flexmock(
            uuid='fip_uuid',
            get_fq_name=lambda: ['domain', 'project', 'fip'],
            get_project_refs=lambda: [{'uuid': str(uuid.uuid4())}],
            get_floating_ip_address=lambda: 'fip_ip',
            get_floating_ip_fixed_ip_address=lambda: 'fip_port_ip',
            get_id_perms=lambda: id_perms_obj,
            get_perms2=lambda: flexmock(get_owner=lambda: str(uuid.uuid4())),
            get_tag_refs=lambda: None)

        fip_obj.get_virtual_machine_interface_refs = \
            lambda: [{'uuid': 'fip_port_uuid1'}]
        fip_neutron = dbi._floatingip_vnc_to_neutron(fip_obj)
        self.assertEqual(fip_neutron['router_id'], 'router_uuid')

        fip_obj.get_virtual_machine_interface_refs = \
            lambda: [{'uuid': 'fip_port_uuid2'}]
        fip_neutron = dbi._floatingip_vnc_to_neutron(fip_obj)
        self.assertIsNone(fip_neutron['router_id'])

    def test_default_security_group_delete(self):
        dbi = MockDbInterface()

        sg_obj = None
        delete_called_for = [""]

        def _sg_delete(id):
            delete_called_for[0] = id

        dbi._vnc_lib = flexmock(operational=True,
                                security_group_read=lambda id: sg_obj,
                                security_group_delete=_sg_delete)

        # sg_delete should be called when sg_name != default
        tenant_uuid = str(uuid.uuid4())
        sg_uuid = str(uuid.uuid4())
        sg_obj = flexmock(operational=True,
                          name="non-default",
                          parent_uuid=tenant_uuid)
        dbi._zookeeper_client.create_node(
            '%s/%s' %
            (dbi.security_group_lock_prefix, sg_uuid))
        context = {'tenant_id': tenant_uuid}
        dbi.security_group_delete(context, sg_uuid)
        self.assertEqual(delete_called_for[0], sg_uuid)

        delete_called_for = [""]
        sg_obj = flexmock(operational=True,
                          name="non-default",
                          parent_uuid=str(uuid.uuid4()))
        dbi._zookeeper_client.create_node(
            '%s/%s' %
            (dbi.security_group_lock_prefix, sg_uuid))
        dbi.security_group_delete(context, sg_uuid)
        self.assertEqual(delete_called_for[0], sg_uuid)

        delete_called_for = [""]
        sg_obj = flexmock(operational=True,
                          name="default",
                          parent_uuid=str(uuid.uuid4()))
        dbi._zookeeper_client.create_node(
            '%s/%s' %
            (dbi.security_group_lock_prefix, sg_uuid))
        dbi.security_group_delete(context, sg_uuid)
        self.assertEqual(delete_called_for[0], sg_uuid)

        with self.assertRaises(Exception):
            delete_called_for = [""]
            sg_obj = flexmock(operational=True,
                              name="default",
                              parent_uuid=tenant_uuid)
            dbi._zookeeper_client.create_node(
                '%s/%s' %
                (dbi.security_group_lock_prefix, sg_uuid))
            dbi.security_group_delete(context, sg_uuid)

    def test_floating_update_port_from_another_project(self):
        dbi = MockDbInterface()
        dbi._get_obj_tenant_id = lambda r, id: 'Another project'

        dbi._vnc_lib = flexmock(
            fq_name_to_id=lambda res, name: 'fip_pool_uuid',
            floating_ip_read=lambda id: 'fip_obj',
            virtual_machine_interface_read=lambda id, fq_name, fields: None,
            logical_routers_list=lambda parent_id, detail: [
                flexmock(uuid='router_uuid',
                         get_virtual_machine_interface_refs=lambda: [
                             {'uuid': 'router_port_uuid'}])])

        flexmock(
            uuid='id_perms_uuid',
            get_created=lambda: 'create_time',
            get_last_modified=lambda: 'last_modified_time',
            get_description=lambda: 'description')

        with self.assertRaises(bottle.HTTPError):
            dbi._floatingip_neutron_to_vnc(
                {'tenant': 'tenant', 'is_admin': False},
                {'id': 1, 'port_id': 11},
                db.UPDATE)
