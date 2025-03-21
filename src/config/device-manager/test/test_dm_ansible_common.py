#
# Copyright (c) 2018 Juniper Networks, Inc. All rights reserved.
#
import json
import sys
import uuid
sys.path.append('../common/cfgm_common/tests/mocked_libs')
from device_manager.device_manager import DeviceManager
from .test_case import DMTestCase
from cfgm_common.tests.test_common import retries
from cfgm_common.tests.test_common import retry_exc_handler
from netaddr import IPNetwork
from .test_dm_utils import FakeJobHandler
from vnc_api.vnc_api import *


class RPTerm:
    def __init__(self, name, protocols=[], prefixs=[], prefixtypes=[],
                 extcommunity_list=[], extcommunity_match_all=None,
                 community_match_all=None, action="",
                 local_pref=None, med=None, asn_list=[],
                 termtype='', routes=[], route_types=[], route_values=[],
                 fcommunity_list=[], fcommunity=None,
                 tcommunity_list=[], tcommunity_add=[]):
        self.name = name
        self.protocols = protocols
        self.prefixs = prefixs
        self.prefixtypes = prefixtypes
        self.extcommunity_list = extcommunity_list
        self.extcommunity_match_all = extcommunity_match_all
        self.community_match_all = community_match_all
        self.action = action
        self.local_pref = local_pref
        self.med = med
        self.asn_list = asn_list
        self.type = termtype
        self.routes = routes; self.route_types = route_types
        self.route_values = route_values
        self.fcommunity_list = fcommunity_list
        self.fcommunity = fcommunity
        self.tcommunity_list = tcommunity_list
        self.tcommunity_add = tcommunity_add
# end RPTerm

class TestAnsibleCommonDM(DMTestCase):
    @classmethod
    def setUpClass(cls):
        dm_config_knobs = [
            ('DEFAULTS', 'push_mode', '1')
        ]
        super(TestAnsibleCommonDM, cls).setUpClass(
            dm_config_knobs=dm_config_knobs)
    # end setUpClass

    def setUp(self, extra_config_knobs=None):
        super(TestAnsibleCommonDM, self).setUp(extra_config_knobs=extra_config_knobs)

    def tearDown(self):
        super(TestAnsibleCommonDM, self).tearDown()

    @retries(5, hook=retry_exc_handler)
    def check_dm_ansible_config_push(self):
        job_input = FakeJobHandler.get_job_input()
        self.assertIsNotNone(job_input)
        DeviceManager.get_instance().logger.debug("Job Input: %s" % json.dumps(job_input, indent=4))
        return job_input
    # end check_dm_ansible_config_push

    def create_features(self, features=[]):
        self.features = {}
        for feature in features:
            feature_obj = Feature(fq_name=[self.GSC, feature],
                                  parent_type='global-system-config',
                                  name=feature, display_name=feature)
            self._vnc_lib.feature_create(feature_obj)
            self.features[feature] = feature_obj
    # end create_features

    def create_physical_roles(self, physical_roles=[]):
        self.physical_roles = {}
        for physical_role in physical_roles:
            physical_role_obj = PhysicalRole(fq_name=[self.GSC, physical_role],
                                             parent_type='global-system-config',
                                             name=physical_role,
                                             display_name=physical_role)
            self._vnc_lib.physical_role_create(physical_role_obj)
            self.physical_roles[physical_role] = physical_role_obj
    # end create_physical_roles

    def create_overlay_roles(self, overlay_roles=[]):
        self.overlay_roles = {}
        for overlay_role in overlay_roles:
            overlay_role_obj = OverlayRole(fq_name=[self.GSC, overlay_role],
                                           parent_type='global-system-config',
                                           name=overlay_role,
                                           display_name=overlay_role)
            self._vnc_lib.overlay_role_create(overlay_role_obj)
            self.overlay_roles[overlay_role] = overlay_role_obj
    # end create_overlay_roles

    def create_role_definition(self, name, physical_role, overlay_role,
            features, feature_configs):
        role_definition = RoleDefinition(fq_name=[self.GSC, name], name=name,
                                         parent_type='global-system-config',
                                         display_name=name)
        role_definition.set_physical_role(self.physical_roles[physical_role])
        role_definition.set_overlay_role(self.overlay_roles[overlay_role])
        for feature in features:
            role_definition.add_feature(self.features[feature])
        self._vnc_lib.role_definition_create(role_definition)
        if feature_configs:
            for feature, config in list(feature_configs.items()):
                kvps = [KeyValuePair(key=key, value=value) for key, value in list(config.items())]
                config = KeyValuePairs(key_value_pair=kvps)
                feature_config = FeatureConfig(name=feature,
                    parent_obj=role_definition,
                    feature_config_additional_params=config)
                self._vnc_lib.feature_config_create(feature_config)
                self.feature_configs.append(feature_config)
        return role_definition
    # end create_role_definition

    def create_role_definitions(self, role_definitions):
        self.role_definitions = {}
        self.feature_configs = []
        for rd in role_definitions:
            self.role_definitions[rd.name] = \
                self.create_role_definition(rd.name, rd.physical_role,
                                            rd.overlay_role, rd.features,
                                            rd.feature_configs)
    # end create_role_definitions

    def create_job_template(self, name):
        job_template = JobTemplate(fq_name=[self.GSC, name], name=name,
                                   parent_type='global-system-config',
                                   display_name=name)
        self._vnc_lib.job_template_create(job_template)
        return job_template
    # end create_job_template

    def create_node_profile(self, name, vendor='juniper', device_family=None,
                            role_mappings=[], job_template=None):
        node_profile_role_mappings = [NodeProfileRoleType(
                                            physical_role=r.physical_role,
                                            rb_roles=r.rb_roles)
                                        for r in role_mappings]
        node_profile_roles = NodeProfileRolesType(
                                role_mappings=node_profile_role_mappings)
        node_profile = NodeProfile(fq_name=[self.GSC, name], name=name,
                                   parent_type='global-system-config',
                                   display_name=name,
                                   node_profile_vendor=vendor,
                                   node_profile_device_family=device_family,
                                   node_profile_roles=node_profile_roles)
        node_profile.set_job_template(job_template)
        self._vnc_lib.node_profile_create(node_profile)

        role_config = RoleConfig(fq_name=[self.GSC, name, 'basic'],
                                 parent_type='node-profile',
                                 name='basic',
                                 display_name='basic')
        self._vnc_lib.role_config_create(role_config)

        return node_profile, role_config
    # end create_node_profile

    def create_fabric(self, name, **kwargs):
        fabric = Fabric(fq_name=[self.GSC, name], name=name,
                        parent_type='global-system-config',
                        display_name=name, manage_underlay=False, **kwargs)
        fabric_uuid = self._vnc_lib.fabric_create(fabric)
        return self._vnc_lib.fabric_read(id=fabric_uuid)
    # end create_fabric

    def create_vn(self, vn_id, subnet, extra_subnets=None, vxlan_id=None):
        subnets = [subnet]
        if extra_subnets and len(extra_subnets) > 0:
            subnets = subnets + extra_subnets
        vn_name = 'vn' + vn_id + '-' + self.id()
        vn_obj = VirtualNetwork(vn_name)
        vn_obj_properties = VirtualNetworkType()
        if vxlan_id is None:
            vxlan_id = 2000 + int(vn_id)
        vn_obj_properties.set_vxlan_network_identifier(vxlan_id)
        vn_obj_properties.set_forwarding_mode('l2_l3')
        vn_obj.set_virtual_network_properties(vn_obj_properties)

        ipam1_obj = NetworkIpam('ipam' + vn_id + '-' + self.id())
        ipam1_uuid = self._vnc_lib.network_ipam_create(ipam1_obj)
        ipam1_obj = self._vnc_lib.network_ipam_read(id=ipam1_uuid)
        subnet_list = []
        for subnet in subnets or []:
            subnet_list.append(IpamSubnetType(SubnetType(subnet, 24)))
        vn_obj.add_network_ipam(ipam1_obj, VnSubnetsType(subnet_list))

        vn_uuid = self._vnc_lib.virtual_network_create(vn_obj)
        return self._vnc_lib.virtual_network_read(id=vn_uuid)
    # end create_vn

    def create_or_update_irt(self, irt_id, prefix_list, irt_obj=None):
        irt_routes = RouteTableType()
        for prefixv in prefix_list or []:
            route = RouteType(prefix=prefixv)
            irt_routes.add_route(route)
        name = 'irt' + irt_id + '-' + self.id()
        if irt_obj is None:
            irt_obj = InterfaceRouteTable(name)
            irt_obj.set_interface_route_table_routes(irt_routes)
            irt_uuid = self._vnc_lib.interface_route_table_create(irt_obj)
        else:
            irt_obj.set_interface_route_table_routes(irt_routes)
            irt_uuid = irt_obj.get_uuid()
            self._vnc_lib.interface_route_table_update(irt_obj)

        return self._vnc_lib.interface_route_table_read(id=irt_uuid)
    # end create_irt

    def create_routing_policy_term(self, protocols=[], prefixs=[],
                                   prefixtypes=[], extcommunity_list=[],
                                   extcommunity_match_all = False,
                                   community_match_all = False, action="",
                                   local_pref=None, med=None, asn_list=[],
                                   routes=[], route_types=[], route_values=[]):
        prefix_list = []
        for i in range(len(prefixs)):
            prefix_list.append(PrefixMatchType(prefix=prefixs[i],
                                               prefix_type=prefixtypes[i]))
        route_filter = None
        for i in range(len(routes)):
            if route_filter is None:
                route_filter = RouteFilterType()
            route_filter.add_route_filter_properties(
                RouteFilterProperties(route=routes[i],
                                      route_type=route_types[i],
                                      route_type_value=route_values[i]))
        tcond = TermMatchConditionType(
            protocol=protocols, prefix=prefix_list,
            community_match_all=community_match_all,
            extcommunity_list=extcommunity_list,
            extcommunity_match_all=extcommunity_match_all,
            route_filter=route_filter)
        aspath = ActionAsPathType(expand=AsListType(asn_list=asn_list))
        updateo = ActionUpdateType(as_path=aspath, local_pref=local_pref,
                                   med=med)
        taction = TermActionListType(action=action, update=updateo)
        term = PolicyTermType(term_match_condition=tcond,
                              term_action_list=taction)
        return term
    # end create_routing_policy_term

    def create_routing_policy(self, rp_name, term_list, termtype=None):
        rp = RoutingPolicy(name=rp_name, term_type=termtype)
        rp.set_routing_policy_entries(PolicyStatementType(term=term_list))
        rp_uuid = self._vnc_lib.routing_policy_create(rp)
        return self._vnc_lib.routing_policy_read(id=rp_uuid)
    # end create_routing_policy

    def verify_rpterms_in_abstract_cfg(self, rpname, termlist, rp_inputdict):
        if rpname not in rp_inputdict or len(rp_inputdict[rpname]) == 0:
            return
        i = 0
        for t in termlist:
            tm = t.get('term_match_condition', None)
            ta = t.get('term_action_list', None)
            if i >= len(rp_inputdict[rpname]):
                continue
            tpassed = rp_inputdict[rpname][i]
            if tm:
                tme = tm.get('extcommunity_list', None)
                for j in range(len(tpassed.extcommunity_list)):
                    self.assertEqual(tme[j], tpassed.extcommunity_list[j])
                tprefix = tm.get('prefix', None)
                for j in range(len(tpassed.prefixs)):
                    self.assertIn(tprefix[j].get('prefix'), tpassed.prefixs)
                    self.assertIn(tprefix[j].get('prefix_type'),
                                  tpassed.prefixtypes)
                troute_filter = tm.get('route_filter', None)
                if troute_filter:
                    rf_props = troute_filter.get('route_filter_properties',[])
                    for j in range(len(tpassed.routes)):
                        self.assertIn(rf_props[j].get('route'), tpassed.routes)
                        self.assertIn(rf_props[j].get('route_type'),
                                      tpassed.route_types)
                        if tpassed.route_values[j] is not None:
                            self.assertIn(
                                rf_props[j].get('route_type_value'),
                                tpassed.route_values)
                if len(tpassed.fcommunity_list) > 0:
                    # for user RP modified to cover community, vrf-import
                    fcommunity_list = tm.get('community_list', [])
                    if len(fcommunity_list) == len(tpassed.fcommunity_list):
                        for j in range(len(tpassed.fcommunity_list)):
                            self.assertIn(fcommunity_list[j],
                                          tpassed.fcommunity_list)
                        self.assertEqual(tm.get('community'),
                                         tpassed.fcommunity)
                tproto = tm.get('protocol', None)
                for j in range(len(tpassed.protocols)):
                    self.assertEqual(tproto[j], tpassed.protocols[j])
            if ta:
                if len(tpassed.action) > 0:
                    self.assertEqual(ta.get('action'), tpassed.action)
                tupdate = ta.get('update', None)
                if tupdate:
                    if tpassed.local_pref is not None:
                        self.assertEqual(tupdate.get('local_pref'),
                                         tpassed.local_pref)
                    if tpassed.med is not None:
                        self.assertEqual(tupdate.get('med'),
                                         tpassed.med)
                    if len(tpassed.asn_list) > 0:
                        tas = tupdate.get('as_path').get('expand').\
                            get('asn_list')
                        for j in range(len(tpassed.asn_list)):
                            self.assertEqual(tas[j], tpassed.asn_list[j])
                    if len(tpassed.tcommunity_add) > 0:
                        if tupdate.get('community') and tupdate.\
                                get('community').get('add') and \
                            tupdate.get('community').get('add').\
                                    get('community'):
                            tac = tupdate.get('community').get('add').\
                                get('community')
                            for j in range(len(tpassed.tcommunity_add)):
                                self.assertEqual(tac[j],
                                                 tpassed.tcommunity_add[j])
                if len(tpassed.tcommunity_list) > 0:
                    tacl = ta.get('community_list', [])
                    if len(tacl) == len(tpassed.tcommunity_list):
                        for j in range(len(tpassed.tcommunity_list)):
                            if tacl[j] not in tpassed.tcommunity_list:
                                continue
                            self.assertIn(tacl[j],
                                          tpassed.tcommunity_list[j])
            i += 1
    # verify_rpterms_in_abstract_cfg

    def verify_routing_policy_in_abstract_cfg(self, abstract_cfg,
                                              rp_inputdict):
        rp_abstract = abstract_cfg.get('routing_policies', None)
        self.assertIsNotNone(rp_abstract)
        for rp_abs in rp_abstract:
            rpname = rp_abs.get('name')
            self.assertIsNotNone(rpname)
            self.assertIn(rpname, rp_inputdict)
            rpterms = rp_abs.get('routing_policy_entries', None)
            self.assertIsNotNone(rpterms)
            termlist = rpterms.get('terms', None)
            self.assertIsNotNone(termlist)
            self.verify_rpterms_in_abstract_cfg(rpname, termlist, rp_inputdict)
    # end

    def get_routing_instance_from_description(self, config, description):
        for ri in config.get('routing_instances', []):
            if description and description == ri.get('description'):
                return ri
        return None
    # end get_routing_instance_from_description

    def create_sg(self, name):
        sg_obj = SecurityGroup(
            name="SG1",
            fq_name=["default-domain", "default-project", name],
            configured_security_group_id=0,
            security_group_entries={
                "policy_rule": [{
                    "direction": ">",
                    "ethertype": "IPv4",
                    "protocol": "tcp",
                    "dst_addresses": [{
                        "network_policy": None,
                        "security_group": None,
                        "subnet": {
                            "ip_prefix": "0.0.0.0",
                            "ip_prefix_len": "0"
                        },
                        "virtual_network": None
                    }],
                    "dst_ports": [{
                        "end_port": "65535",
                        "start_port": "0"
                    }],
                    "src_addresses": [{
                        "network_policy": None,
                        "security_group": "local",
                        "subnet": None,
                        "virtual_network": None
                    }],
                    "src_ports": [{
                        "end_port": "65535",
                        "start_port": "0"
                    }]
                }, {
                    "direction": ">",
                    "ethertype": "IPv6",
                    "protocol": "tcp",
                    "dst_addresses": [{
                        "network_policy": None,
                        "security_group": None,
                        "subnet": {
                            "ip_prefix": "::",
                            "ip_prefix_len": "0"
                        },
                        "virtual_network": None
                    }],
                    "dst_ports": [{
                        "end_port": "65535",
                        "start_port": "0"
                    }],
                    "src_addresses": [{
                        "network_policy": None,
                        "security_group": "local",
                        "subnet": None,
                        "virtual_network": None
                    }],
                    "src_ports": [{
                        "end_port": "65535",
                        "start_port": "0"
                    }]
                }]
            })
        sg_uuid = self._vnc_lib.security_group_create(sg_obj)
        return self._vnc_lib.security_group_read(id=sg_uuid)

    def attach_vmi(self, vmi_id, pi_list, pr_list, vn, sg, fabric, vlan_tag, port_vlan_tag=None):
        pi_obj_list = []
        for idx in range(len(pi_list)):
            try:
                pi_fq_name = pr_list[idx].get_fq_name() + [pi_list[idx]]
                pi = self._vnc_lib.physical_interface_read(fq_name=pi_fq_name)
            except NoIdError:
                pi = PhysicalInterface(pi_list[idx], parent_obj=pr_list[idx])
                pi_uuid = self._vnc_lib.physical_interface_create(pi)
                pi = self._vnc_lib.physical_interface_read(id=pi_uuid)
            pi_obj_list.append(pi)

        vm = VirtualMachine(name='bms' + vmi_id, display_name='bms' + vmi_id,
            fq_name=['bms' + vmi_id], server_type='baremetal-server')
        vm_uuid = self._vnc_lib.virtual_machine_create(vm)
        vm = self._vnc_lib.virtual_machine_read(id=vm_uuid)

        mac_address = '08:00:27:af:94:0' + vmi_id
        fq_name = ['default-domain', 'default-project', 'vmi' + vmi_id + '-' + self.id()]
        vmi = VirtualMachineInterface(fq_name=fq_name, parent_type='project',
            virtual_machine_interface_device_owner='baremetal:none',
            virtual_machine_interface_mac_addresses= {
                   'mac_address': [mac_address]
                })
        if len(pi_list) == 1:
            vmi_profile = "{\"local_link_information\":[{\"switch_id\":\"11:11:11:11:11:11\",\"port_id\":\"%s\",\"switch_info\":\"%s\",\"fabric\":\"%s\"}]}" % (pi_list[0], pr_list[0].get_fq_name()[-1], fabric.get_fq_name()[-1])
        else:
            vmi_profile = "{\"local_link_information\":[{\"switch_id\":\"11:11:11:11:11:11\",\"port_id\":\"%s\",\"switch_info\":\"%s\",\"fabric\":\"%s\"},{\"switch_id\":\"11:11:11:11:11:11\",\"port_id\":\"%s\",\"switch_info\":\"%s\",\"fabric\":\"%s\"}]}" % (pi_list[0], pr_list[0].get_fq_name()[-1], fabric.get_fq_name()[-1], pi_list[1], pr_list[1].get_fq_name()[-1], fabric.get_fq_name()[-1])

        vmi_bindings = {
            "key_value_pair": [{
                "key": "vnic_type",
                "value": "baremetal"
            }, {
                "key": "vif_type",
                "value": "vrouter"
            }, {
                "key": "profile",
                "value": vmi_profile
            }, {
                "key": "host_id",
                "value": vm_uuid
            }]
        }

        if not vlan_tag:
            ll = {
                "key": "tor_port_vlan_id",
                "value": str(port_vlan_tag)
            }
            vmi_bindings['key_value_pair'].append(ll)

        else:
            vmi_properties = {
                "sub_interface_vlan_tag": vlan_tag
            }
            vmi.set_virtual_machine_interface_properties(vmi_properties)


        vmi.set_virtual_machine_interface_bindings(vmi_bindings)
        vmi.add_virtual_network(vn)
        if sg:
            vmi.add_security_group(sg)
        vmi_uuid = self._vnc_lib.virtual_machine_interface_create(vmi)
        vmi = self._vnc_lib.virtual_machine_interface_read(id=vmi_uuid)

        return vmi, vm, pi_obj_list
    # end attach_vmi

    def set_encapsulation_priorities(self, priorities = []):
        fq_name=['default-global-system-config', 'default-global-vrouter-config']
        try:
            config = self._vnc_lib.global_vrouter_config_read(fq_name=fq_name)
        except NoIdError:
            config = GlobalVrouterConfig(fq_name=fq_name)
            self._vnc_lib.global_vrouter_config_create(config)
            config = self._vnc_lib.global_vrouter_config_read(fq_name=fq_name)
        config.set_encapsulation_priorities(EncapsulationPrioritiesType(encapsulation=priorities))
        self._vnc_lib.global_vrouter_config_update(config)
    # end set_encapsulation_priorities

    def delete_role_definitions(self):
        for fc in self.feature_configs:
            self._vnc_lib.feature_config_delete(fq_name=fc.get_fq_name())
        for rd in list(self.role_definitions.values()):
            self._vnc_lib.role_definition_delete(fq_name=rd.get_fq_name())
    # end delete_role_definitions

    def delete_overlay_roles(self):
        for r in list(self.overlay_roles.values()):
            self._vnc_lib.overlay_role_delete(fq_name=r.get_fq_name())
    # end delete_overlay_roles

    def delete_physical_roles(self):
        for r in list(self.physical_roles.values()):
            self._vnc_lib.physical_role_delete(fq_name=r.get_fq_name())
    # end delete_physical_roles

    def delete_features(self):
        for f in list(self.features.values()):
            self._vnc_lib.feature_delete(fq_name=f.get_fq_name())
    # end delete_features

    @retries(5, hook=retry_exc_handler)
    def wait_for_features_delete(self):
        for f in list(self.features.values()):
            try:
                self._vnc_lib.feature_read(fq_name=f.get_fq_name())
                self.assertFalse(True)
            except NoIdError:
                pass
    # end wait_for_features_delete

    def get_phy_interfaces(self, abs_config, name=None):
        interfaces = abs_config.get('physical_interfaces')
        if not interfaces:
            return None
        for intf in interfaces or []:
            if name and name == intf.get('name'):
                return intf
        return None
    # end get_phy_interfaces

    def get_logical_interface(self, phy_intf, name=None):
        log_intf = phy_intf.get('logical_interfaces', [])
        for intf in log_intf:
            if name and name == intf.get('name'):
                return intf
        return None
    # end get_logical_interface

    def get_lag_members(self, phy_intf):
        lag = phy_intf.get('link_aggregation_group')

        self.assertTrue(lag.get('lacp_enabled'))
        return lag.get('link_members')

    def get_vlans(self, abs_config, name=None):
        vlans = abs_config.get('vlans')
        if not vlans:
            return []
        vlan_list = []
        for vlan in vlans or []:
            if name and name == vlan.get('name'):
                return vlan
            else:
                vlan_list.append(vlan)
        return vlan_list
    # end get_vlans

    def get_firewalls(self, abs_config, name=None):
        fw_filters = abs_config.get('firewall').get('firewall_filters')
        if not fw_filters:
            return []
        fw_list = []
        for fw in fw_filters or []:
            if name and name == fw.get('name'):
                return fw
            else:
                fw_list.append(fw)
        return fw_list
    # end get_firewalls

    def fabric_network_name(self, fabric_name, network_type):
        """Fabric network name.

        :param fabric_name: string
        :param network_type: string
        :return: string
        """
        return '%s-%s-network' % (fabric_name, network_type)
    # end fabric_network_name

    def fabric_network_ipam_name(self, fabric_name, network_type):
        """Fabric network IPAM name.

        :param fabric_name: string
        :param network_type: string
        :return: string
        """
        return '%s-%s-network-ipam' % (fabric_name, network_type)
    # end fabric_network_ipam_name

    def carve_out_subnets(self, subnets, cidr):
        """Carve out subnets.

        :param subnets: type=list<Dictionary>
        :param cidr: type=int
            example:
            [
                { 'cidr': '192.168.10.1/24', 'gateway': '192.168.10.1 }
            ]
            cidr = 30
        :return: list<Dictionary>
            example:
            [
                { 'cidr': '192.168.10.1/30'}
            ]
        """
        carved_subnets = []
        for subnet in subnets:
            slash_x_subnets = IPNetwork(subnet.get('cidr')).subnet(cidr)
            for slash_x_sn in slash_x_subnets:
                carved_subnets.append({'cidr': str(slash_x_sn)})
        return carved_subnets
    # end _carve_out_subnets

    def new_subnet(self, cidr):
        """Create a new subnet.

        :param cidr: string, example: '10.1.1.1/24'
        :return: <vnc_api.gen.resource_xsd.SubnetType>
        """
        split_cidr = cidr.split('/')
        return SubnetType(ip_prefix=split_cidr[0], ip_prefix_len=split_cidr[1])
    # end new_subnet

    def add_network_ipam(self, ipam_name, subnets, subnetting):
        """Add network IPAM.

        :param ipam_name: string
        :param subnets: list<Dictionary>
            [
                { 'cidr': '10.1.1.1/24', 'gateway': '10.1.1.1' }
            ]
        :param subnetting: boolean
        :return: <vnc_api.gen.resource_client.NetworkIpam>
        """
        ipam_fq_name = ['default-domain', 'default-project', ipam_name]
        ipam = NetworkIpam(
            name=ipam_name,
            fq_name=ipam_fq_name,
            parent_type='project',
            ipam_subnets=IpamSubnets([
                IpamSubnetType(
                    subnet=self.new_subnet(sn.get('cidr')),
                    default_gateway=sn.get('gateway'),
                    subnet_uuid=str(uuid.uuid1())
                ) for sn in subnets if int(sn.get('cidr').split('/')[-1]) < 31
            ]),
            ipam_subnet_method='flat-subnet',
            ipam_subnetting=subnetting
        )
        try:
            self._vnc_lib.network_ipam_create(ipam)
        except RefsExistError as ex:
            self._vnc_lib.network_ipam_update(ipam)
        return self._vnc_lib.network_ipam_read(fq_name=ipam_fq_name)
    # end add_network_ipam

    def add_cidr_namespace(self, fabric, ns_name, ns_subnets, tag):
        """Add CIDR namespace.

        :param fabric: <vnc_api.gen.resource_client.Fabric>
        :param ns_name:
        :param ns_subnets:
        :param tag:
        :return:
        """
        subnets = []
        for subnet in ns_subnets:
            ip_prefix = subnet['cidr'].split('/')
            subnets.append(SubnetType(
                ip_prefix=ip_prefix[0], ip_prefix_len=ip_prefix[1]))

        ns_fq_name = fabric.fq_name + [ns_name]
        namespace = FabricNamespace(
            name=ns_name,
            fq_name=ns_fq_name,
            parent_type='fabric',
            fabric_namespace_type='IPV4-CIDR',
            fabric_namespace_value=NamespaceValue(
                ipv4_cidr=SubnetListType(subnet=subnets)
            )
        )
        namespace.set_tag_list([{'to': [tag]}])
        try:
            self._vnc_lib.fabric_namespace_create(namespace)
        except RefsExistError:
            self._vnc_lib.fabric_namespace_update(namespace)

        namespace = self._vnc_lib.fabric_namespace_read(fq_name=ns_fq_name)
        return namespace
    # end add_cidr_namespace

    def add_fabric_vn(self, fabric_obj, network_type, subnets, subnetting):
        """Add fabric VN.

        :param fabric_obj: <vnc_api.gen.resource_client.Fabric>
        :param network_type: string, one of the NetworkType constants
        :param subnets: list<Dictionary>
            [
                { 'cidr': '10.1.1.1/24', 'gateway': '10.1.1.1' }
            ]
        :param subnetting: boolean
        :return: <vnc_api.gen.resource_client.VirtualNetwork>
        """
        network_name = self.fabric_network_name(
            str(fabric_obj.name), network_type)
        nw_fq_name = ['default-domain', 'default-project', network_name]

        network = VirtualNetwork(
            name=network_name,
            fq_name=nw_fq_name,
            parent_type='project',
            virtual_network_properties=VirtualNetworkType(
                forwarding_mode='l3'),
            address_allocation_mode='flat-subnet-only')

        try:
            self._vnc_lib.virtual_network_create(network)
        except RefsExistError as ex:
            self._vnc_lib.virtual_network_update(network)

        network = self._vnc_lib.virtual_network_read(fq_name=nw_fq_name)

        ipam_name = self.fabric_network_ipam_name(
            str(fabric_obj.name), network_type)
        ipam = self.add_network_ipam(ipam_name, subnets, subnetting)

        # add vn->ipam link
        network.add_network_ipam(ipam, VnSubnetsType([]))
        self._vnc_lib.virtual_network_update(network)
        fabric_obj.add_virtual_network(
            network, FabricNetworkTag(network_type=network_type))
        self._vnc_lib.fabric_update(fabric_obj)

        return ipam, network
    # end add_fabric_vn
# end TestAnsibleCommonDM
