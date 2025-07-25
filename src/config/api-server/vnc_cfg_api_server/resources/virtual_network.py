#
# Copyright (c) 2018 Juniper Networks, Inc. All rights reserved.
#
import copy
import uuid


from cfgm_common import get_bgp_rtgt_max_id
from cfgm_common import get_bgp_rtgt_min_id
from cfgm_common import LINK_LOCAL_VN_FQ_NAME
from cfgm_common import PERMS_NONE, PERMS_RWX, PERMS_RX
from cfgm_common.exceptions import HttpError
from cfgm_common.exceptions import NoIdError
from cfgm_common.exceptions import ResourceExistsError
from cfgm_common.utils import _DEFAULT_ZK_LOCK_TIMEOUT
from kazoo.exceptions import LockTimeout, NodeExistsError
from netaddr import IPAddress, IPNetwork, valid_ipv4
from pysandesh.gen_py.sandesh.ttypes import SandeshLevel
from vnc_api.gen.resource_common import RoutingInstance
from vnc_api.gen.resource_common import VirtualNetwork

from vnc_cfg_api_server.context import get_context
from vnc_cfg_api_server.resources._resource_base import ResourceMixin
from vnc_cfg_api_server.vnc_quota import QUOTA_OVER_ERROR_CODE
from vnc_cfg_api_server.vnc_quota import QuotaHelper


class VirtualNetworkServer(ResourceMixin, VirtualNetwork):
    @classmethod
    def _check_is_provider_network_property(cls, obj_dict, db_conn,
                                            vn_ref=None):
        # no further checks if is_provider_network is not set
        if 'is_provider_network' not in obj_dict:
            return (True, '')
        if not vn_ref:
            return (True, '')
        # compare obj_dict with db
        # update from True to False is not allowed
        if vn_ref.get('is_provider_network', False) and \
           not obj_dict.get('is_provider_network'):
            return (False,
                    'Update is_provider_network property'
                    'from True to False of VN (%s) '
                    'is not allowed' % obj_dict.get('uuid'))
        return (True, '')

    @classmethod
    def _check_provider_network(cls, obj_dict, db_conn, vn_ref=None):
        # no further checks if not linked
        # to a provider network
        if not obj_dict.get('virtual_network_refs'):
            return (True, '')
        # retrieve this VN
        if not vn_ref:
            vn_ref = {'uuid': obj_dict['uuid'],
                      'is_provider_network': obj_dict.get(
                          'is_provider_network')}
        uuids = []
        for vn in obj_dict.get('virtual_network_refs'):
            if 'uuid' not in vn:
                try:
                    uuids += [cls.db_conn.fq_name_to_uuid(cls.object_type,
                                                          vn['to'])]
                except NoIdError as e:
                    return (False, str(e))
            else:
                uuids += [vn['uuid']]

        # if not a provider_vn, not more
        # than one virtual_network_refs is allowed
        if not vn_ref.get('is_provider_network') and \
           len(obj_dict.get('virtual_network_refs')) != 1:
            return(False,
                   'Non Provider VN (%s) can connect to one provider VN but '
                   'trying to connect to VN (%s)' % (vn_ref['uuid'], uuids))

        # retrieve vn_refs of linked VNs
        (ok, provider_vns, _) = db_conn.dbe_list('virtual_network',
                                                 obj_uuids=uuids,
                                                 field_names=[
                                                     'is_provider_network'],
                                                 filters={
                                                     'is_provider_network': [
                                                         True]})
        if not ok:
            return (ok, 'Error reading VN refs (%s)' % uuids)
        if vn_ref.get('is_provider_network'):
            # this is a provider-VN, no linked provider_vns is expected
            if provider_vns:
                return (False,
                        'Provider VN (%s) can not connect to another '
                        'provider VN (%s)' % (vn_ref.get('uuid'), uuids))
        else:
            # this is a non-provider VN, only one provider vn is expected
            if len(provider_vns) != 1:
                return (False,
                        'Non Provider VN (%s) can connect only to one '
                        'provider VN but not (%s)' % (
                            vn_ref.get('uuid'), uuids))
        return (True, '')
    # end _check_provider_network

    @classmethod
    def _check_route_targets(cls, rt_dict):
        route_target_list = rt_dict.get('route_target')
        if not route_target_list:
            return True, ''

        global_asn = cls.server.global_autonomous_system
        for idx, rt in enumerate(route_target_list):
            ok, result, new_rt = cls.server.get_resource_class(
                'route_target').validate_route_target(rt)
            if not ok:
                return False, result
            user_defined_rt = result
            if not user_defined_rt:
                return (False, "Configured route target must use ASN that is "
                        "different from global ASN or route target value must"
                        " be less than %d and greater than %d" %
                        (get_bgp_rtgt_min_id(global_asn),
                         get_bgp_rtgt_max_id(global_asn)))

            if new_rt:
                route_target_list[idx] = new_rt
        return (True, '')

    @staticmethod
    def _check_vxlan_id(obj_dict):
        if ('virtual_network_properties' in obj_dict and
                obj_dict['virtual_network_properties'] is not None and
                'vxlan_network_identifier' in obj_dict[
                    'virtual_network_properties']):
            virtual_network_properties = obj_dict['virtual_network_properties']
            return True, virtual_network_properties.get(
                'vxlan_network_identifier')
        else:
            # return false only when the above fields are not found
            return False, None

    @classmethod
    def _check_provider_details(cls, obj_dict, db_conn, create):
        # we don't check anymore whether the VN is in use or not
        # non-provider VN in-use/not-in-use can be updated to provider VN
        # provider VN cannot be modified with provider_details
        properties = obj_dict.get('provider_properties')
        if not properties:
            return (True, '')

        # VN create
        if create:
            if properties.get('segmentation_id') is None:
                return (False, "Segmenation ID must be configured to create")
            if not properties.get('physical_network'):
                return (False, "Physical Network must be configured to create")
            return (True, '')

        # VN update
        ok, vn = cls.dbe_read(
            db_conn,
            'virtual_network',
            obj_dict['uuid'],
            obj_fields=['virtual_machine_interface_back_refs',
                        'provider_properties', 'is_provider_network'])
        if not ok:
            return (ok, vn)
        if vn.get('is_provider_network', False):
            old_properties = vn.get('provider_properties', {})
            if properties.get('segmentation_id') is None:
                properties['segmentation_id'] = \
                    old_properties.get('segmentation_id')
            if not properties.get('physical_network'):
                properties['physical_network'] = \
                    old_properties.get('physical_network')
            if old_properties != properties:
                return (False,
                        'Update provider_details property'
                        'of provider VN (%s) '
                        'is not allowed' % vn.get('uuid', ''))
            return (True, '')

        if properties.get('segmentation_id') is None:
            return (False, "Segmenation ID must be configured to update")
        if not properties.get('physical_network'):
            return (False, "Physical Network must be configured to update")
        return (True, '')
    # end _check_provider_details

    @classmethod
    def _is_multi_policy_service_chain_supported(cls, obj_dict,
                                                 read_result=None):
        if not ('multi_policy_service_chains_enabled' in obj_dict or
                'route_target_list' in obj_dict or
                'import_route_target_list' in obj_dict or
                'export_route_target_list' in obj_dict):
            return (True, '')

        # Create Request
        if not read_result:
            read_result = {}

        result_obj_dict = copy.deepcopy(read_result)
        result_obj_dict.update(obj_dict)
        if result_obj_dict.get('multi_policy_service_chains_enabled'):
            import_export_targets = result_obj_dict.get('route_target_list',
                                                        {})
            import_targets = result_obj_dict.get('import_route_target_list',
                                                 {})
            export_targets = result_obj_dict.get('export_route_target_list',
                                                 {})
            import_targets_set = set(import_targets.get('route_target', []))
            export_targets_set = set(export_targets.get('route_target', []))
            targets_in_both_import_and_export = \
                import_targets_set.intersection(export_targets_set)
            if ((import_export_targets.get('route_target') or []) or
                    targets_in_both_import_and_export):
                msg = "Multi policy service chains are not supported, "
                msg += "with both import export external route targets"
                return (False, (409, msg))

        return (True, '')

    @classmethod
    def _check_net_mode_for_flat_ipam(cls, obj_dict, db_dict):
        net_mode = None
        vn_props = None
        if 'virtual_network_properties' in obj_dict:
            vn_props = obj_dict['virtual_network_properties']
        elif db_dict:
            vn_props = db_dict.get('virtual_network_properties')
        if vn_props:
            net_mode = vn_props.get('forwarding_mode')

        if net_mode != 'l3':
            return (False, "flat-subnet is allowed only with l3 network")
        else:
            return (True, "")

    @classmethod
    def _check_ipam_network_subnets(cls, obj_dict, db_conn, vn_uuid,
                                    db_dict=None):
        # if Network has subnets in network_ipam_refs, it should refer to
        # atleast one ipam with user-defined-subnet method. If network is
        # attached to all "flat-subnet", vn can not have any VnSubnetType cidrs

        if (('network_ipam_refs' not in obj_dict) and
           ('virtual_network_properties' in obj_dict)):
            # it is a network update without any changes in network_ipam_refs
            # but changes in virtual_network_properties
            # we need to read ipam_refs from db_dict and for any ipam if
            # if subnet_method is flat-subnet, network_mode should be l3
            if db_dict is None:
                return (True, 200, '')
            else:
                db_ipam_refs = db_dict.get('network_ipam_refs') or []

            ipam_with_flat_subnet = False
            ipam_uuid_list = [ipam['uuid'] for ipam in db_ipam_refs]
            if not ipam_uuid_list:
                return (True, 200, '')

            ok, ipam_lists, _ = db_conn.dbe_list(
                'network_ipam',
                obj_uuids=ipam_uuid_list,
                field_names=['ipam_subnet_method'])
            if not ok:
                return False, ipam_lists[0], ipam_lists[1]
            for ipam in ipam_lists:
                if 'ipam_subnet_method' in ipam:
                    subnet_method = ipam['ipam_subnet_method']
                    ipam_with_flat_subnet = True
                    break

            if ipam_with_flat_subnet:
                (ok, result) = cls._check_net_mode_for_flat_ipam(obj_dict,
                                                                 db_dict)
                if not ok:
                    return (ok, 400, result)

        # validate ipam_refs in obj_dict either update or remove
        ipam_refs = obj_dict.get('network_ipam_refs') or []
        ipam_subnets_list = []
        ipam_with_flat_subnet = False
        for ipam in ipam_refs:
            ipam_fq_name = ipam['to']
            ipam_uuid = ipam.get('uuid')
            if not ipam_uuid:
                ipam_uuid = db_conn.fq_name_to_uuid('network_ipam',
                                                    ipam_fq_name)

            ok, ipam_dict = db_conn.dbe_read(obj_type='network_ipam',
                                             obj_id=ipam_uuid,
                                             obj_fields=['ipam_subnet_method',
                                                         'ipam_subnets'])
            if not ok:
                return (ok, 400, ipam_dict)

            subnet_method = ipam_dict.get('ipam_subnet_method')
            if subnet_method is None:
                subnet_method = 'user-defined-subnet'

            if subnet_method == 'flat-subnet':
                ipam_with_flat_subnet = True
                subnets_list = cls.addr_mgmt._ipam_to_subnets(ipam_dict)
                if not subnets_list:
                    subnets_list = []
                ipam_subnets_list = ipam_subnets_list + subnets_list

            # get user-defined-subnet information to validate
            # that there is cidr configured if subnet_method is 'flat-subnet'
            vnsn = ipam['attr']
            ipam_subnets = vnsn['ipam_subnets']
            if (subnet_method == 'flat-subnet'):
                for ipam_subnet in ipam_subnets:
                    subnet_dict = ipam_subnet.get('subnet')
                    if subnet_dict:
                        ip_prefix = subnet_dict.get('ip_prefix')
                        if ip_prefix is not None:
                            msg = ("with flat-subnet, network can not have "
                                   "user-defined subnet")
                            return False, 400, msg

            if subnet_method == 'user-defined-subnet':
                (ok, result) = cls.addr_mgmt.net_check_subnet(ipam_subnets)
                if not ok:
                    return (ok, 409, result)

            if (db_conn.update_subnet_uuid(ipam_subnets)):
                db_conn.config_log(
                    'AddrMgmt: subnet uuid is updated for vn %s'
                    % vn_uuid, level=SandeshLevel.SYS_DEBUG)

        if ipam_with_flat_subnet:
            (ok, result) = cls._check_net_mode_for_flat_ipam(obj_dict,
                                                             db_dict)
            if not ok:
                return (ok, 400, result)

        (ok, result) = cls.addr_mgmt.net_check_subnet_quota(db_dict, obj_dict,
                                                            db_conn)

        if not ok:
            return (ok, QUOTA_OVER_ERROR_CODE, result)

        vn_subnets_list = cls.addr_mgmt._vn_to_subnets(obj_dict)
        if not vn_subnets_list:
            vn_subnets_list = []
        ok, result = cls.addr_mgmt.net_check_subnet_overlap(vn_subnets_list,
                                                            ipam_subnets_list)
        if not ok:
            return (ok, 400, result)

        return (True, 200, '')

    @classmethod
    def _get_fabric_snat(cls, uuid, obj_dict, db_conn):
        ok, result = cls.dbe_read(db_conn,
                                  'virtual_network',
                                  uuid,
                                  obj_fields=['fabric_snat'])
        if not ok:
            return False

        if 'fabric_snat' in result:
            return result['fabric_snat']

        return False

    @classmethod
    def _allocate_instance_ip(cls, iip_name, ip_addr, db_conn, vn_obj):
        kwargs = {'name': iip_name}
        kwargs['instance_ip_address'] = ip_addr
        kwargs['instance_ip_family'] = 'v4'
        kwargs['virtual_network_refs'] = [{'to': vn_obj['fq_name']}]
        ok, result = cls.server.get_resource_class(
            'instance_ip').locate([iip_name], **kwargs)
        if not ok:
            return False, result

        return ok, result

    @classmethod
    def _get_vn_instance_ip_uuid_map(cls, vn_obj, db_conn):
        iip_map = {}
        ok, iip_refs = cls.dbe_read(db_conn,
                                    'virtual_network',
                                    vn_obj['uuid'],
                                    obj_fields=['instance_ip_back_refs'])

        iip_refs = iip_refs.get('instance_ip_back_refs')
        if not iip_refs:
            return {}
        iip_uuid_list = [iip_ref['uuid'] for iip_ref in iip_refs]
        ok, iip_list, _ = db_conn.dbe_list('instance_ip',
                                           obj_uuids=iip_uuid_list,
                                           field_names=['instance_ip_address',
                                                        'uuid'])
        if not ok:
            return {}
        for iip in iip_list or []:
            iip_map[iip.get('instance_ip_address')] = iip.get('uuid')
        return iip_map

    @classmethod
    def _check_and_alloc_loopback_instance_ip(cls, obj_dict, db_conn, vn_obj):
        iip_map = cls._get_vn_instance_ip_uuid_map(vn_obj, db_conn)
        new_ip_map = []
        if obj_dict.get('virtual_network_routed_properties', None) is not None:
            vn_routed_prop = obj_dict.get('virtual_network_routed_properties',
                                          {})
            for routed_prop in vn_routed_prop.get('routed_properties', []):
                ip_addr = routed_prop.get('loopback_ip_address', None)
                ok = True
                if ip_addr is None:
                    pr_uuid = routed_prop.get('physical_router_uuid', None)
                    lr_uuid = routed_prop.get('logical_router_uuid', None)
                    if lr_uuid and pr_uuid:
                        iip_name = pr_uuid.split('-')[-1]\
                            + lr_uuid.split('-')[-1]
                    else:
                        return False, "PR/LR UUID is None for loopback VN"
                    ok, result = cls._allocate_instance_ip(iip_name,
                                                           None,
                                                           db_conn,
                                                           vn_obj)
                    if ok:
                        routed_prop['loopback_ip_address'] =\
                            result['instance_ip_address']
                        ip_addr = result['instance_ip_address']
                else:
                    if len(iip_map) == 0 or iip_map.get(ip_addr, None) is None:
                        ok, result = cls._allocate_instance_ip(ip_addr,
                                                               ip_addr,
                                                               db_conn,
                                                               vn_obj)
                if not ok:
                    return False, result
                new_ip_map.append(ip_addr)
        # Delete case
        old_instance_ip = iip_map.keys()
        for iip in set(set(old_instance_ip) - set(new_ip_map)):
            db_conn.get_api_server().internal_request_delete('instance-ip',
                                                             iip_map[iip])
        return True, "Loopback VN updated successfully"

    @classmethod
    def pre_dbe_create(cls, tenant_name, obj_dict, db_conn):
        (ok, response) = cls._is_multi_policy_service_chain_supported(obj_dict)
        if not ok:
            return (ok, response)

        is_shared = obj_dict.get('is_shared')
        # neutron <-> vnc sharing
        if obj_dict['perms2'].get('global_access', 0) == PERMS_RWX:
            obj_dict['is_shared'] = True
        elif is_shared:
            obj_dict['perms2']['global_access'] = PERMS_RWX
        else:
            obj_dict['is_shared'] = False

        # Does not authorize to set the virtual network ID as it's allocated
        # by the vnc server
        if obj_dict.get('virtual_network_network_id') is not None:
            return False, (403, "Cannot set the virtual network ID")

        # Allocate vxlan_id if it's present in request.
        vxlan_id = None
        (ok, vxlan_id) = cls._check_vxlan_id(obj_dict)
        if vxlan_id is not None:
            try:
                vxlan_fq_name = ':'.join(obj_dict['fq_name']) + '_vxlan'
                cls.vnc_zk_client.alloc_vxlan_id(vxlan_fq_name, int(vxlan_id))
            except ResourceExistsError:
                msg = ("Cannot set VXLAN_ID: %s, it has already been set" %
                       vxlan_id)
                return False, (400, msg)

            def undo_vxlan_id():
                cls.vnc_zk_client.free_vxlan_id(
                    int(vxlan_id),
                    vxlan_fq_name)
                return True, ""
            get_context().push_undo(undo_vxlan_id)

        # Allocate virtual network ID
        vn_id = cls.vnc_zk_client.alloc_vn_id(
            ':'.join(obj_dict['fq_name']))

        def undo_vn_id():
            cls.vnc_zk_client.free_vn_id(
                vn_id, ':'.join(obj_dict['fq_name']))
            return True, ""
        get_context().push_undo(undo_vn_id)
        obj_dict['virtual_network_network_id'] = vn_id

        vn_uuid = obj_dict.get('uuid')
        (ok, return_code, result) = cls._check_ipam_network_subnets(obj_dict,
                                                                    db_conn,
                                                                    vn_uuid)
        if not ok:
            return (ok, (return_code, result))

        rt_dict = obj_dict.get('route_target_list')
        if rt_dict:
            (ok, error) = cls._check_route_targets(rt_dict)
            if not ok:
                return (False, (400, error))

        rt_import_dict = obj_dict.get('import_route_target_list')
        if rt_import_dict:
            (ok, error) = cls._check_route_targets(rt_import_dict)
            if not ok:
                return (False, (400, error))

        rt_export_dict = obj_dict.get('export_route_target_list')
        if rt_export_dict:
            (ok, error) = cls._check_route_targets(rt_export_dict)
            if not ok:
                return (False, (400, error))

        (ok, error) = cls._check_is_provider_network_property(obj_dict,
                                                              db_conn)
        if not ok:
            return (False, (400, error))

        (ok, error) = cls._check_provider_details(obj_dict, db_conn, True)
        if not ok:
            return (False, (400, error))

        (ok, error) = cls._check_provider_network(obj_dict, db_conn)
        if not ok:
            return (False, (400, error))

        # Check if network forwarding mode support BGP VPN types
        ok, result = cls.server.get_resource_class(
            'bgpvpn').check_network_supports_vpn_type(obj_dict)
        if not ok:
            return ok, result

        # Check if we can reference the BGP VPNs
        ok, result = cls.server.get_resource_class(
            'bgpvpn').check_network_has_bgpvpn_assoc_via_router(obj_dict)
        if not ok:
            return ok, result

        ipam_refs = obj_dict.get('network_ipam_refs') or []
        try:
            cls.addr_mgmt.net_create_req(obj_dict)
            # for all ipams which are flat, we need to write a unique id as
            # subnet uuid for all cidrs in flat-ipam
            for ipam in ipam_refs:
                ipam_fq_name = ipam['to']
                ipam_uuid = ipam.get('uuid')
                if not ipam_uuid:
                    ipam_uuid = db_conn.fq_name_to_uuid('network_ipam',
                                                        ipam_fq_name)
                (ok, ipam_dict) = db_conn.dbe_read(
                    obj_type='network_ipam',
                    obj_id=ipam_uuid,
                    obj_fields=['ipam_subnet_method'])
                if not ok:
                    return (ok, (400, ipam_dict))

                subnet_method = ipam_dict.get('ipam_subnet_method')
                if (subnet_method is not None and
                        subnet_method == 'flat-subnet'):
                    subnet_dict = {}
                    flat_subnet_uuid = str(uuid.uuid4())
                    subnet_dict['subnet_uuid'] = flat_subnet_uuid
                    ipam['attr']['ipam_subnets'] = [subnet_dict]

            def undo():
                cls.addr_mgmt.net_delete_req(obj_dict)
                return True, ""
            get_context().push_undo(undo)
        except Exception as e:
            return (False, (500, str(e)))

        return True, ""

    @classmethod
    def _validate_ipv4(cls, ipaddr):
        if valid_ipv4(ipaddr):
            return True, ipaddr
        return False, ipaddr

    @classmethod
    def _routed_vn_bgp_validation(cls, bgp_info):
        if bgp_info.get('peer_ip_address_list', None):
            for peer_ip in bgp_info.get('peer_ip_address_list') or []:
                status, ret = cls._validate_ipv4(peer_ip)
                if status is False:
                    return status, ret
        else:
            return cls._validate_ipv4(bgp_info.get('peer_ip_address'))

        return True, ""

    @classmethod
    def _routed_vn_pim_validation(cls, pim_info):
        rp_ips = pim_info.get('rp_ip_address', None)
        for rp_ip in rp_ips or []:
            status, ret = cls._validate_ipv4(rp_ip)
            if status is False:
                return status, ret
        mode = pim_info.get('mode', None)
        if mode and mode != 'sparse-dense':
            return False, "PIM mode only sparse-dense supported"

        return True, ""

    @classmethod
    def _routed_vn_ospf_validation(cls, ospf_info):
        area_id = ospf_info.get('area_id', None)
        area_type = ospf_info.get('area_type', None)
        if area_id == '0' or area_id == "0.0.0.0":
            if area_type != 'backbone':
                return False, "Area id zero should have area type as backbone"
        return True, ""

    @classmethod
    def _routed_ip_address_validation(cls, ip_addr, ipam_refs, vn_obj):
        for ipam in ipam_refs:
            attr = ipam.get('attr') or {}
            ipam_subnets = attr.get('ipam_subnets') or []
            for ipam in ipam_subnets or []:
                subnet = ipam.get('subnet', None)
                if subnet:
                    ip_prefix = subnet.get('ip_prefix')
                    ip_len = str(subnet.get('ip_prefix_len'))
                    cidr = ip_prefix + '/' + ip_len
                    if IPAddress(ip_addr) in IPNetwork(cidr):
                        return True, ""

        return False, ip_addr

    @classmethod
    def _validate_routed_vn_payload(cls, vn_dict, vn_obj):
        status = True
        result = ""
        if vn_dict.get('virtual_network_routed_properties', None):
            vn_routed_prop = vn_dict.get('virtual_network_routed_properties',
                                         {})
            for routed_param in vn_routed_prop.get('routed_properties', []):
                if not routed_param.get('physical_router_uuid', None):
                    return False, "Physical router Id not present for "\
                                  "routed VN"
                if not routed_param.get('logical_router_uuid', None):
                    return False, "Logical Router Id not present for routed VN"

                rvn_ip_addr = routed_param.get('routed_interface_ip_address',
                                               None)
                if not cls._check_if_overlay_loopback_vn(vn_obj):
                    if not rvn_ip_addr:
                        return False, "routed-interface-ip-address is required"

                    ipam_refs = vn_obj.get('network_ipam_refs', [])
                    status, result = cls._routed_ip_address_validation(
                        rvn_ip_addr,
                        ipam_refs,
                        vn_obj)
                    if status is False:
                        msg = "IP %s does not belong to any subnet" % result
                        return False, msg

                if routed_param.get('routing_protocol'):
                    if routed_param.get('routing_protocol') == 'bgp':
                        bgp_info = routed_param.get('bgp_params', None)
                        status, result = cls._routed_vn_bgp_validation(
                            bgp_info)
                    elif routed_param.get('routing_protocol') == 'pim':
                        pim_info = routed_param.get('pim_params', None)
                        status, result = cls._routed_vn_pim_validation(
                            pim_info)
                    elif routed_param.get('routing_protocol') == 'ospf':
                        ospf_info = routed_param.get('ospf_params', None)
                        status, result = cls._routed_vn_ospf_validation(
                            ospf_info)

                if status is False:
                    return status, result
        return status, result

    @classmethod
    def _check_if_overlay_loopback_vn(cls, vn_obj):
        # this check to reduce unnecessary checks for all routed
        # VNs currently overlay loopback vn is internally set
        # as <fabric-name>-overlay-loopback
        if "overlay-loopback" in vn_obj['fq_name'][-1]:
            # check fabric backref
            fab_back_ref = vn_obj.get('fabric_back_refs', [])
            for ref in fab_back_ref:
                attr = ref.get('attr', None)
                if attr:
                    network_type = attr.get('network_type', None)
                    if network_type == 'overlay-loopback':
                        return True
        return False

    @classmethod
    def post_dbe_create(cls, tenant_name, obj_dict, db_conn):
        api_server = db_conn.get_api_server()

        # Create native/vn-default routing instance
        ri_fq_name = obj_dict['fq_name'][:]
        ri_fq_name.append(obj_dict['fq_name'][-1])
        ri_obj = RoutingInstance(
            parent_type='virtual-network', fq_name=ri_fq_name,
            routing_instance_is_default=True)
        fabric_snat = cls._get_fabric_snat(obj_dict['uuid'], obj_dict, db_conn)
        ri_obj.set_routing_instance_fabric_snat(fabric_snat)
        api_server.internal_request_create(
            'routing-instance',
            ri_obj.serialize_to_json())

        return True, ''

    @classmethod
    def pre_dbe_update(cls, id, fq_name, obj_dict, db_conn, **kwargs):
        if fq_name == LINK_LOCAL_VN_FQ_NAME:
            return True, ""

        # neutron <-> vnc sharing
        global_access = obj_dict.get('perms2', {}).get('global_access')
        is_shared = obj_dict.get('is_shared')
        router_external = obj_dict.get('router_external')
        if global_access is not None or is_shared is not None or \
           router_external is not None:
            if global_access is not None and is_shared is not None:
                # NOTE(gzimin): Check router_external parameter too.
                if is_shared != (global_access == 7) and \
                   (router_external is None or not router_external):
                    msg = ("Inconsistent is_shared (%s) and global_access (%s)"
                           % (is_shared, global_access))
                    return False, (400, msg)
            if global_access is not None and router_external is not None:
                # NOTE(gzimin): Check is_shared parameter too.
                if router_external != (global_access == 5) and \
                   (is_shared is None or not is_shared):
                    msg = ("Inconsistent router_external (%s) and "
                           "global_access (%s)"
                           % (router_external, global_access))
                    return False, (400, msg)
            elif global_access is not None:
                obj_dict['is_shared'] = (global_access != 0)
            else:
                ok, result = cls.dbe_read(db_conn, 'virtual_network', id,
                                          obj_fields=['perms2'])
                if not ok:
                    return ok, result
                obj_dict['perms2'] = result['perms2']
                if is_shared:
                    obj_dict['perms2']['global_access'] = PERMS_RWX
                elif router_external:
                    obj_dict['perms2']['global_access'] = PERMS_RX
                else:
                    obj_dict['perms2']['global_access'] = PERMS_NONE

        rt_dict = obj_dict.get('route_target_list')
        if rt_dict:
            (ok, error) = cls._check_route_targets(rt_dict)
            if not ok:
                return (False, (400, error))

        rt_import_dict = obj_dict.get('import_route_target_list')
        if rt_import_dict:
            (ok, error) = cls._check_route_targets(rt_import_dict)
            if not ok:
                return (False, (400, error))

        rt_export_dict = obj_dict.get('export_route_target_list')
        if rt_export_dict:
            (ok, error) = cls._check_route_targets(rt_export_dict)
            if not ok:
                return (False, (400, error))

        (ok, error) = cls._check_provider_details(obj_dict, db_conn, False)
        if not ok:
            return (False, (409, error))

        ok, read_result = cls.dbe_read(db_conn, 'virtual_network', id)
        if not ok:
            return ok, read_result

        new_vn_id = obj_dict.get('virtual_network_network_id')
        # Does not authorize to update the virtual network ID as it's allocated
        # by the vnc server
        if (new_vn_id is not None and
                new_vn_id != read_result.get('virtual_network_network_id')):
            return (False, (403, "Cannot update the virtual network ID"))

        new_vxlan_id = None
        old_vxlan_id = None
        deallocated_vxlan_network_identifier = None

        (new_vxlan_status, new_vxlan_id) = cls._check_vxlan_id(obj_dict)
        (old_vxlan_status, old_vxlan_id) = cls._check_vxlan_id(read_result)

        if new_vxlan_status and new_vxlan_id != old_vxlan_id:

            vn_fq_name = ':'.join(fq_name)
            vxlan_fq_name = vn_fq_name + '_vxlan'
            if new_vxlan_id is not None:
                # First, check if the new_vxlan_id being updated exist for
                # some other VN.
                new_vxlan_fq_name_in_db = cls.vnc_zk_client.get_vn_from_id(
                    int(new_vxlan_id))

                if new_vxlan_fq_name_in_db and \
                   not vxlan_fq_name.startswith(new_vxlan_fq_name_in_db):
                    msg = ("Cannot set VXLAN_ID: %s, it has already been "
                           "set" % new_vxlan_id)
                    return False, (400, msg)

                # Free vxlan if allocated using vn_fq_name (w/o vxlan suffix)
                if new_vxlan_fq_name_in_db == vn_fq_name:
                    cls.vnc_zk_client.free_vxlan_id(int(new_vxlan_id),
                                                    vn_fq_name)

                # Second, set the new_vxlan_id in Zookeeper.
                cls.vnc_zk_client.alloc_vxlan_id(vxlan_fq_name,
                                                 int(new_vxlan_id))

                def undo_alloc():
                    cls.vnc_zk_client.free_vxlan_id(
                        int(new_vxlan_id), vxlan_fq_name)
                get_context().push_undo(undo_alloc)

            # Third, check if old_vxlan_id is not None, if so, delete it from
            # Zookeeper
            if old_vxlan_id is not None:
                cls.vnc_zk_client.free_vxlan_id(int(old_vxlan_id),
                                                vxlan_fq_name)
                # Add old vxlan_network_identifier to handle
                # dbe_update_notification
                deallocated_vxlan_network_identifier = old_vxlan_id

                def undo_free():
                    cls.vnc_zk_client.alloc_vxlan_id(
                        vxlan_fq_name, int(old_vxlan_id))
                get_context().push_undo(undo_free)

        (ok, error) = cls._check_is_provider_network_property(
            obj_dict, db_conn, vn_ref=read_result)
        if not ok:
            return (False, (400, error))

        (ok, error) = cls._check_provider_network(
            obj_dict, db_conn, vn_ref=read_result)
        if not ok:
            return (False, (400, error))

        (ok, response) = cls._is_multi_policy_service_chain_supported(
            obj_dict, read_result)
        if not ok:
            return (ok, response)

        ok, return_code, result = cls._check_ipam_network_subnets(
            obj_dict, db_conn, id, read_result)
        if not ok:
            return (ok, (return_code, result))

        (ok, result) = cls.addr_mgmt.net_check_subnet_delete(read_result,
                                                             obj_dict)
        if not ok:
            return (ok, (409, result))

        ipam_refs = obj_dict.get('network_ipam_refs') or []
        if ipam_refs:
            (ok, result) = cls.addr_mgmt.net_validate_subnet_update(
                read_result, obj_dict)
            if not ok:
                return (ok, (400, result))

        # Check if network forwarding mode support BGP VPN types
        ok, result = cls.server.get_resource_class(
            'bgpvpn').check_network_supports_vpn_type(obj_dict, read_result)
        if not ok:
            return ok, result

        # Check if we can reference the BGP VPNs
        ok, result = cls.server.get_resource_class(
            'bgpvpn').check_network_has_bgpvpn_assoc_via_router(
                obj_dict, read_result)
        if not ok:
            return ok, result

        # Check if VN is routed and has overlay-loopback in name alloc
        # and de-alloc instance IPs
        vn_category = read_result.get('virtual_network_category', None)
        if vn_category == 'routed':
            ok, error = cls._validate_routed_vn_payload(obj_dict, read_result)
            if ok and cls._check_if_overlay_loopback_vn(read_result):
                (ok, error) = cls._check_and_alloc_loopback_instance_ip(
                    obj_dict,
                    db_conn,
                    read_result)
            if not ok:
                return (False, (409, error))
        try:
            instip_refs = read_result.get('instance_ip_back_refs') or []
            zk_entry_prefix = fq_name[:]
            if ipam_refs:
                cls._update_subnet_iip_refs(instip_refs=instip_refs,
                                            zk_entry_prefix=zk_entry_prefix,
                                            network_ipam_refs=ipam_refs,
                                            db_conn=db_conn,
                                            vn_uuid=obj_dict['uuid'])
            cls.addr_mgmt.net_update_req(fq_name, read_result, obj_dict, id)
            for ipam in ipam_refs:
                ipam_fq_name = ipam['to']
                ipam_uuid = db_conn.fq_name_to_uuid('network_ipam',
                                                    ipam_fq_name)
                (ok, ipam_dict) = db_conn.dbe_read(obj_type='network_ipam',
                                                   obj_id=ipam_uuid)
                if not ok:
                    return (ok, (409, ipam_dict))

                subnet_method = ipam_dict.get('ipam_subnet_method')
                if (subnet_method is not None and
                        subnet_method == 'flat-subnet'):
                    vnsn_data = ipam.get('attr') or {}
                    ipam_subnets = vnsn_data.get('ipam_subnets') or []
                    if (len(ipam_subnets) == 1):
                        continue

                    if (len(ipam_subnets) == 0):
                        subnet_dict = {}
                        flat_subnet_uuid = str(uuid.uuid4())
                        subnet_dict['subnet_uuid'] = flat_subnet_uuid
                        ipam['attr']['ipam_subnets'].insert(0, subnet_dict)

            def undo():
                # failed => update with flipped values for db_dict and req_dict
                cls.addr_mgmt.net_update_req(
                    fq_name, obj_dict, read_result, id)
            get_context().push_undo(undo)
        except Exception as e:
            return (False, (500, str(e)))

        return True, {
            'deallocated_vxlan_network_identifier':
                deallocated_vxlan_network_identifier,
        }

    @classmethod
    def _update_subnet_iip_refs(cls, instip_refs, zk_entry_prefix,
                                network_ipam_refs, db_conn, vn_uuid):

        for ref in instip_refs:
            zk_prefix = zk_entry_prefix[:]
            try:
                (ok, result) = db_conn.dbe_read('instance_ip',
                                                ref['uuid'])
            except NoIdError:
                continue
            inst_ip = result.get('instance_ip_address')
            for ipam in network_ipam_refs:
                for subnet in ipam['attr']['ipam_subnets']:
                    ip_prefix = subnet['subnet']['ip_prefix']
                    ip_prefix_len = str(subnet['subnet']['ip_prefix_len'])
                    if IPAddress(inst_ip) in IPNetwork("/".join(
                            [ip_prefix,
                             ip_prefix_len]
                    )
                    ):
                        new_subnet_id = subnet['subnet_uuid']
                        ip_str = "%(#)010d" % {'#': int(IPAddress(
                            inst_ip))
                        }
                        zk_prefix.append(ip_prefix)
                        zk_entry_prefix_str = ":".join(zk_prefix)
                        zk_entry_full = "/".join(
                            [cls.vnc_zk_client._subnet_path,
                             zk_entry_prefix_str,
                             ip_prefix_len,
                             ip_str]
                        )
                        result['subnet_uuid'] = new_subnet_id
                        ok, result = db_conn.dbe_update(
                            'instance_ip',
                            ref['uuid'],
                            result
                        )
                        if not ok:
                            return (ok, (409, result))
                        msg = "Creating zk path %s %s" % (zk_entry_full,
                                                          inst_ip)
                        cls.addr_mgmt.config_log(
                            msg,
                            level=SandeshLevel.SYS_ERR
                        )
                        zk = cls.vnc_zk_client._zk_client
                        vn_lock_prefix = '%s/vnc_api_server_locks/' \
                                         'virtual_network/%s' % \
                                         (zk.host_ip, vn_uuid)
                        zk_client = cls.vnc_zk_client._zk_client._zk_client
                        lock = zk.lock(vn_lock_prefix)
                        try:
                            lock.acquire(timeout=_DEFAULT_ZK_LOCK_TIMEOUT)
                            cls.addr_mgmt.config_log(
                                "ZK Lock acquired"
                                " for %s" % _DEFAULT_ZK_LOCK_TIMEOUT,
                                level=SandeshLevel.SYS_DEBUG)
                            try:
                                zk_client.create(zk_entry_full,
                                                 ref['uuid'].encode(),
                                                 makepath=True
                                                 )
                            except (NodeExistsError, ResourceExistsError):
                                iip_uuid = zk_client.get(zk_entry_full)[0]
                                zk_path_exist = "Creating zk path" \
                                                ": %s (%s)." \
                                                " Addr lock already" \
                                                " exists for IIP %s" %\
                                                (zk_entry_full, inst_ip,
                                                 iip_uuid)
                                cls.addr_mgmt.config_log(
                                    zk_path_exist,
                                    level=SandeshLevel.SYS_ERR
                                )
                                pass
                        except LockTimeout:
                            cls.addr_mgmt.config_log(
                                "ZK Lock timeout raised",
                                level=SandeshLevel.SYS_ERR
                            )
                        finally:
                            lock.release()
                            cls.addr_mgmt.config_log(
                                "ZK Lock released",
                                level=SandeshLevel.SYS_DEBUG
                            )
        return

    @classmethod
    def post_dbe_update(cls, id, fq_name, obj_dict, db_conn, **kwargs):
        api_server = db_conn.get_api_server()

        # Create native/vn-default routing instance
        ri_fq_name = fq_name[:]
        ri_fq_name.append(fq_name[-1])
        ri_uuid = db_conn.fq_name_to_uuid('routing_instance', ri_fq_name)
        ri_obj = RoutingInstance(
            parent_type='virtual-network', fq_name=ri_fq_name,
            routing_instance_is_default=True)
        fabric_snat = cls._get_fabric_snat(id, obj_dict, db_conn)
        ri_obj.set_routing_instance_fabric_snat(fabric_snat)
        api_server.internal_request_update(
            'routing-instance',
            ri_uuid,
            ri_obj.serialize_to_json())

        return True, ''

    @classmethod
    def pre_dbe_delete(cls, id, obj_dict, db_conn):
        cls.addr_mgmt.net_delete_req(obj_dict)
        if obj_dict['id_perms'].get('user_visible', True) is not False:
            ok, result = QuotaHelper.get_project_dict_for_quota(
                obj_dict['parent_uuid'], db_conn)
            if not ok:
                return False, result, None
            proj_dict = result
            ok, (subnet_count, counter) =\
                cls.addr_mgmt.get_subnet_quota_counter(obj_dict, proj_dict)
            if subnet_count:
                counter -= subnet_count

            def undo_subnet():
                ok, (subnet_count, counter) =\
                    cls.addr_mgmt.get_subnet_quota_counter(obj_dict, proj_dict)
                if subnet_count:
                    counter += subnet_count
            get_context().push_undo(undo_subnet)

        def undo():
            cls.addr_mgmt.net_create_req(obj_dict)
        get_context().push_undo(undo)
        return True, "", None

    @classmethod
    def post_dbe_delete(cls, id, obj_dict, db_conn, **kwargs):
        api_server = db_conn.get_api_server()

        # Delete native/vn-default routing instance
        # For this find backrefs and remove their ref to RI
        ri_fq_name = obj_dict['fq_name'][:]
        ri_fq_name.append(obj_dict['fq_name'][-1])
        ri_uuid = db_conn.fq_name_to_uuid('routing_instance', ri_fq_name)

        backref_fields = RoutingInstance.backref_fields
        children_fields = RoutingInstance.children_fields
        ok, result = cls.dbe_read(db_conn, 'routing_instance', ri_uuid,
                                  obj_fields=backref_fields | children_fields)
        if not ok:
            return ok, result

        ri_obj_dict = result
        backref_field_types = RoutingInstance.backref_field_types
        for backref_name in backref_fields:
            backref_res_type = backref_field_types[backref_name][0]

            def drop_ref(obj_uuid):
                # drop ref from ref_uuid to ri_uuid
                api_server.internal_request_ref_update(
                    backref_res_type, obj_uuid, 'DELETE',
                    'routing-instance', ri_uuid)
            for backref in ri_obj_dict.get(backref_name) or []:
                drop_ref(backref['uuid'])

        children_field_types = RoutingInstance.children_field_types
        for child_name in children_fields:
            child_res_type = children_field_types[child_name][0]
            for child in ri_obj_dict.get(child_name) or []:
                try:
                    api_server.internal_request_delete(child_res_type,
                                                       child['uuid'])
                except HttpError as e:
                    if e.status_code == 404:
                        pass
                    else:
                        raise
                except NoIdError:
                    pass
        api_server.internal_request_delete('routing-instance', ri_uuid)

        # Deallocate the virtual network ID
        cls.vnc_zk_client.free_vn_id(
            obj_dict.get('virtual_network_network_id'),
            ':'.join(obj_dict['fq_name']),
        )

        (ok, vxlan_id) = cls._check_vxlan_id(obj_dict)
        if vxlan_id is not None:
            vxlan_fq_name = ':'.join(obj_dict['fq_name']) + '_vxlan'
            cls.vnc_zk_client.free_vxlan_id(int(vxlan_id), vxlan_fq_name)

        return True, ""

    @classmethod
    def ip_alloc(cls, vn_fq_name, subnet_uuid=None, count=1, family=None):
        if family:
            ip_version = 6 if family == 'v6' else 4
        else:
            ip_version = None

        ip_list = [cls.addr_mgmt.ip_alloc_req(vn_fq_name, sub=subnet_uuid,
                                              asked_ip_version=ip_version,
                                              alloc_id=str(uuid.uuid4()))[0]
                   for i in range(count)]
        msg = 'AddrMgmt: reserve %d IP for vn=%s, subnet=%s - %s' \
            % (count, vn_fq_name, subnet_uuid or '', ip_list)
        cls.addr_mgmt.config_log(msg, level=SandeshLevel.SYS_DEBUG)
        return {'ip_addr': ip_list}

    @classmethod
    def ip_free(cls, vn_fq_name, ip_list):
        msg = 'AddrMgmt: release IP %s for vn=%s' % (ip_list, vn_fq_name)
        cls.addr_mgmt.config_log(msg, level=SandeshLevel.SYS_DEBUG)
        for ip_addr in ip_list:
            cls.addr_mgmt.ip_free_req(ip_addr, vn_fq_name)

    @classmethod
    def subnet_ip_count(cls, vn_fq_name, subnet_list):
        ip_count_list = []
        for item in subnet_list:
            ip_count_list.append(cls.addr_mgmt.ip_count_req(vn_fq_name, item))
        return {'ip_count_list': ip_count_list}

    @classmethod
    def dbe_create_notification(cls, db_conn, obj_id, obj_dict):
        cls.vnc_zk_client.alloc_vn_id(
            ':'.join(obj_dict['fq_name']),
            obj_dict['virtual_network_network_id'],
        )
        vxlan_id = None
        (ok, vxlan_id) = cls._check_vxlan_id(obj_dict)
        if vxlan_id is not None:
            try:
                vxlan_fq_name = ':'.join(obj_dict['fq_name']) + '_vxlan'
                cls.vnc_zk_client.alloc_vxlan_id(
                    vxlan_fq_name, int(vxlan_id), notify=True)
            except ResourceExistsError:
                pass
        cls.addr_mgmt.net_create_notify(obj_id, obj_dict)

        return True, ''

    @classmethod
    def dbe_update_notification(cls, obj_id, extra_dict=None):
        ok, read_result = cls.dbe_read(cls.db_conn, 'virtual_network', obj_id)
        if not ok:
            return ok, read_result

        obj_dict = read_result
        new_vxlan_id = None
        old_vxlan_id = None

        (new_vxlan_status, new_vxlan_id) = cls._check_vxlan_id(obj_dict)

        if extra_dict is not None:
            old_vxlan_id = extra_dict.get(
                'deallocated_vxlan_network_identifier')

        if new_vxlan_status and new_vxlan_id != old_vxlan_id:

            vn_fq_name = ':'.join(obj_dict['fq_name'])
            vxlan_fq_name = vn_fq_name + '_vxlan'
            if new_vxlan_id is not None:
                # First, check if the new_vxlan_id being updated exist for
                # some other VN.
                new_vxlan_fq_name_in_db = cls.vnc_zk_client.get_vn_from_id(
                    int(new_vxlan_id))
                if not vxlan_fq_name.startswith(new_vxlan_fq_name_in_db):
                    return

                # Second, set the new_vxlan_id in Zookeeper.
                cls.vnc_zk_client.alloc_vxlan_id(
                    vxlan_fq_name, int(new_vxlan_id), notify=True)

            # Third, check if old_vxlan_id is not None, if so, delete it from
            # Zookeeper
            if old_vxlan_id is not None:
                cls.vnc_zk_client.free_vxlan_id(
                    int(old_vxlan_id),
                    vxlan_fq_name,
                    notify=True)

        return cls.addr_mgmt.net_update_notify(obj_id)

    @classmethod
    def dbe_delete_notification(cls, obj_id, obj_dict):
        cls.vnc_zk_client.free_vn_id(
            obj_dict.get('virtual_network_network_id'),
            ':'.join(obj_dict['fq_name']),
            notify=True,
        )

        vxlan_id = None
        (ok, vxlan_id) = cls._check_vxlan_id(obj_dict)
        if vxlan_id is not None:
            vxlan_fq_name = ':'.join(obj_dict['fq_name']) + '_vxlan'
            cls.vnc_zk_client.free_vxlan_id(
                int(vxlan_id), vxlan_fq_name, notify=True)

        cls.addr_mgmt.net_delete_notify(obj_id, obj_dict)

        return True, ''
