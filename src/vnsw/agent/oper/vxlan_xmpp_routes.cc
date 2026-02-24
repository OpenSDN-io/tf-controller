/*
 * Copyright (c) 2018 Juniper Networks, Inc. All rights reserved.
 * Copyright (c) 2022 - 2026 Matvey Kraposhin.
 * Copyright (c) 2024 - 2026 Elena Zizganova.
 */

#include <oper/operdb_init.h>
#include <oper/route_common.h>
#include <oper/vrf.h>
#include <oper/bridge_route.h>
#include <oper/inet_unicast_route.h>
#include <oper/evpn_route.h>
#include <oper/agent_route.h>
#include <oper/vn.h>
#include <oper/vrf.h>
#include <controller/controller_route_path.h>
#include <oper/vxlan_routing_manager.h>
#include <oper/tunnel_nh.h>
#include <oper/bgp_as_service.h>

template<class RouteTable, class RouteEntry>
static const AgentPath *LocalVmExportInterface(Agent* agent,
    RouteTable *table, RouteEntry *route);

TunnelNHKey* VxlanRoutingManager::AllocateTunnelNextHopKey(
    const IpAddress& dip, const MacAddress& dmac) const {

    const Ip4Address rtr_dip = dip.to_v4();

    const std::vector<IpAddress> nh_addresses(1, dip);
    bool is_zero_mac = dmac.IsZero();

    MacAddress rtr_dmac;
    if (!is_zero_mac) {
        rtr_dmac = dmac;
    } else {
        rtr_dmac = NbComputeMac(rtr_dip, agent_);
    }

    TunnelNHKey *tun_nh_key = new TunnelNHKey(agent_->fabric_vrf_name(),
        agent_->router_id(),
        rtr_dip,
        false,
        TunnelType(TunnelType::VXLAN),
        rtr_dmac);
    return tun_nh_key;
}

void VxlanRoutingManager::XmppAdvertiseEvpnRoute(const IpAddress& prefix_ip,
const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
const RouteParameters& params, const Peer *bgp_peer,
const std::vector<std::string> &peer_sources) {
    VrfEntry* vrf = agent_->vrf_table()->FindVrfFromName(vrf_name);
    EvpnAgentRouteTable *evpn_table =
        dynamic_cast<EvpnAgentRouteTable*>(vrf->GetEvpnRouteTable());
    if (evpn_table == NULL) {
        return;
    }

    // If route is a tunnel
    if (agent_->router_id() != params.nh_addr_.to_v4()) {
        XmppAdvertiseEvpnTunnel(evpn_table,
            prefix_ip, prefix_len, vxlan_id, vrf_name, params, bgp_peer);
        return;
    }

    // Or this is an interface
    XmppAdvertiseEvpnInterface(evpn_table,
            prefix_ip, prefix_len, vxlan_id, vrf_name, params, bgp_peer, peer_sources);
}

void VxlanRoutingManager::XmppAdvertiseInetRoute(const IpAddress& prefix_ip,
    const int prefix_len, const std::string vrf_name,
    const AgentPath* path) {

    VrfEntry* vrf = agent_->vrf_table()->FindVrfFromName(vrf_name);
    InetUnicastAgentRouteTable *inet_table =
        vrf->GetInetUnicastRouteTable(prefix_ip);

    if (path->nexthop() && path->nexthop()->GetType() ==
        NextHop::TUNNEL) {
            XmppAdvertiseInetTunnel(inet_table,
                prefix_ip, prefix_len, vrf_name, path);
        return;
    }

    if (path->nexthop() &&
        (path->nexthop()->GetType() == NextHop::INTERFACE ||
        path->nexthop()->GetType() == NextHop::COMPOSITE)) {
        XmppAdvertiseInetInterfaceOrComposite(inet_table,
            prefix_ip, prefix_len, vrf_name, path);
    }
}

void VxlanRoutingManager::XmppAdvertiseEvpnTunnel(
    EvpnAgentRouteTable *evpn_table, const IpAddress& prefix_ip,
    const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
    const RouteParameters& params, const Peer *peer
) {
    const BgpPeer *bgp_peer = dynamic_cast<const BgpPeer*>(peer);
    if (bgp_peer == NULL) {
        return;
    }
    DBRequest nh_req(DBRequest::DB_ENTRY_ADD_CHANGE);
    std::unique_ptr<TunnelNHKey> tun_nh_key (AllocateTunnelNextHopKey(
        params.nh_addr_, params.nh_mac_));

    ControllerVmRoute *data =
        ControllerVmRoute::MakeControllerVmRoute(dynamic_cast<const BgpPeer*>(bgp_peer),
        agent_->fabric_vrf_name(),
        agent_->router_id(),
        vrf_name,
        tun_nh_key->dip(),
        TunnelType::VxlanType(),
        vxlan_id,
        tun_nh_key->rewrite_dmac(),
        params.vn_list_,
        params.sg_list_,
        params.tag_list_,
        params.path_preference_,
        true,
        EcmpLoadBalance(),
        false);
    evpn_table->AddRemoteVmRouteReq(bgp_peer,
        vrf_name,
        MacAddress(),
        prefix_ip,
        prefix_len,
        0,  // ethernet_tag is zero for Type5
        data);
}

void VxlanRoutingManager::XmppAdvertiseEvpnBgpaasInterface(
    EvpnAgentRouteTable *evpn_table, const IpAddress& prefix_ip,
    const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
    const RouteParameters& params, const Peer *bgp_peer,  const NextHop *nh
) {
    const InterfaceNH *intf_nh = dynamic_cast<const InterfaceNH *>(nh);
    const Interface *intf = intf_nh->GetInterface();
    if (intf->type() != Interface::VM_INTERFACE) {
        return;
    }
    const VmInterface *vm_intf = dynamic_cast<const VmInterface*>
        (intf);
    DBEntryBase::KeyPtr tintf_key_ptr = vm_intf->GetDBRequestKey();
    const VmInterfaceKey* intf_key_ptr =
        dynamic_cast<const VmInterfaceKey *>(tintf_key_ptr.get());

    LocalVmRoute *loc_rt_ptr = new LocalVmRoute(
        *intf_key_ptr,
        MplsTable::kInvalidLabel,  // mpls_label
        vxlan_id,
        false,
        params.vn_list_,
        intf_nh->GetFlags(),  // flags
        params.sg_list_,
        params.tag_list_,
        params.communities_,
        params.path_preference_,
        Ip4Address(0),
        params.ecmp_load_balance_,
        false,
        false,
        params.sequence_number_,
        false,
        false);  // native_encap
        loc_rt_ptr->set_tunnel_bmap(TunnelType::VxlanType());

    {
        DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);

        req.key.reset(new EvpnRouteKey(bgp_peer,
            evpn_table->vrf_entry()->GetName(),
            MacAddress(),
            prefix_ip,
            prefix_len,
            0));
        req.data.reset(loc_rt_ptr);
        evpn_table->Enqueue(&req);
    }
}

void VxlanRoutingManager::XmppAdvertiseEvpnBgpaasComposite(
    EvpnAgentRouteTable *evpn_table, const IpAddress& prefix_ip,
    const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
    const RouteParameters& params, const Peer *bgp_peer,
    ComponentNHKeyList &comp_nh_list ) {
    DBRequest nh_req(DBRequest::DB_ENTRY_ADD_CHANGE);
    const bool comp_nh_policy = true;
    CompositeNHKey *new_comp_nh_key =
        new CompositeNHKey(Composite::LOCAL_ECMP,
            comp_nh_policy, comp_nh_list, vrf_name);

    nh_req.key.reset(new_comp_nh_key);
    nh_req.data.reset(new CompositeNHData());
    const BgpPeer *bgp_peer_ecmp = dynamic_cast<const BgpPeer*>(bgp_peer);
    std::stringstream prefix_str;
    prefix_str << prefix_ip.to_string();
    prefix_str << "/";
    prefix_str << prefix_len;

    ControllerEcmpRoute *rt_data = new ControllerEcmpRoute(
        bgp_peer_ecmp,
        params.vn_list_,
        params.ecmp_load_balance_,
        params.tag_list_,
        params.sg_list_,
        params.path_preference_,
        TunnelType::VxlanType(),
        nh_req,
        prefix_str.str(),
        evpn_table->vrf_name());

    ControllerEcmpRoute::ClonedLocalPathListIter iter =
        rt_data->cloned_local_path_list().begin();
    while (iter != rt_data->cloned_local_path_list().end()) {
        evpn_table->AddClonedLocalPathReq(bgp_peer_ecmp, vrf_name,
            MacAddress(), prefix_ip, 0, (*iter));
        iter++;
    }

    evpn_table->AddRemoteVmRouteReq(bgp_peer_ecmp,
        vrf_name, MacAddress(),
        prefix_ip,
        prefix_len,
        0,  // ethernet tag is 0 for VxLAN
        rt_data);
}

void VxlanRoutingManager::XmppAdvertiseEvpnBgpaas(
    EvpnAgentRouteTable *evpn_table, const IpAddress& prefix_ip,
    const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
    const RouteParameters& params, const Peer *bgp_peer,
    const std::vector<std::string> &peer_sources
) {
        const NextHop *first_intf_nh = nullptr;
        NextHopKey *nh_key = nullptr;
        // Create a new composite
        ComponentNHKeyList new_comp_nh_list;
        for (auto &nexthop_addr : peer_sources) {
            IpAddress nh_ip;
            boost::system::error_code ec;
            int nh_ip_len;
            if (is_ipv4_string(nexthop_addr)) {
                nh_ip = Ip4Address::from_string(ipv4_prefix(nexthop_addr), ec);
                nh_ip_len = ipv4_prefix_len(nexthop_addr);
            } else if (is_ipv6_string(nexthop_addr)) {
                nh_ip = Ip6Address::from_string(ipv6_prefix(nexthop_addr), ec);
                nh_ip_len = ipv6_prefix_len(nexthop_addr);
            } else {
                LOG(ERROR, "Error in VxlanRoutingManager::AddInterfaceComponentToList"
                    << ", nexthop_addr = " << nexthop_addr
                    << " is not an IPv4 or IPv6 prefix");
                return;
            }
            if (ec) {
                continue;
            }
            EvpnRouteEntry *route =evpn_table->FindRoute(MacAddress(),
                nh_ip, nh_ip_len, 0);
            const AgentPath *loc_path =
                LocalVmExportInterface(agent_, evpn_table, route);
            if (loc_path == nullptr) {
                continue;
            }
            if (loc_path->nexthop() == nullptr) {
                continue;
            }
            if (IsBgpaasInterfaceNexthop(agent_, loc_path->nexthop())) {
                if (first_intf_nh == NULL) {
                    first_intf_nh = loc_path->nexthop();
                }
                DBEntryBase::KeyPtr key = loc_path->nexthop()->
                    GetDBRequestKey();
                nh_key = static_cast<NextHopKey *>(key.release());

                std::unique_ptr<const NextHopKey> nh_key_ptr(nh_key);
                ComponentNHKeyPtr component_nh_key(new ComponentNHKey(
                    MplsTable::kInvalidLabel, std::move(nh_key_ptr)));
                new_comp_nh_list.push_back(component_nh_key);
            } else if (IsBgpaasCompositeNexthop(agent_, loc_path->nexthop())) {
                const CompositeNH *composite_nh = dynamic_cast<const CompositeNH*>(
                    loc_path->nexthop());
                const ComponentNHList &components = composite_nh->component_nh_list();
                for (auto &component : components) {
                    if (component.get() &&
                        component->nh() &&
                        component->nh()->GetType() == NextHop::INTERFACE) {
                        if (first_intf_nh == nullptr) {
                            first_intf_nh = component->nh();
                        }
                        DBEntryBase::KeyPtr key = component->nh()->
                            GetDBRequestKey();
                        nh_key = static_cast<NextHopKey *>(key.release());

                        std::unique_ptr<const NextHopKey> nh_key_ptr(nh_key);
                        ComponentNHKeyPtr component_nh_key(new ComponentNHKey(
                            MplsTable::kInvalidLabel, std::move(nh_key_ptr)));
                        new_comp_nh_list.push_back(component_nh_key);
                    }
                }
            }
        }

        if (new_comp_nh_list.size() < 1) {
            return;
        } else if (new_comp_nh_list.size() == 1) {
            XmppAdvertiseEvpnBgpaasInterface(evpn_table, prefix_ip, prefix_len,
            vxlan_id, vrf_name, params, bgp_peer, first_intf_nh);
        } else {
            XmppAdvertiseEvpnBgpaasComposite(evpn_table, prefix_ip, prefix_len,
            vxlan_id, vrf_name, params, bgp_peer, new_comp_nh_list);
        }

}

void VxlanRoutingManager::XmppAdvertiseEvpnInterface(
    EvpnAgentRouteTable *evpn_table, const IpAddress& prefix_ip,
    const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
    const RouteParameters& params, const Peer *bgp_peer,
    const std::vector<std::string> &peer_sources ) {
    EvpnRouteEntry *route = evpn_table->FindRoute(MacAddress(),
        prefix_ip, prefix_len, 0);
    if (RoutePrefixIsEqualTo(route, prefix_ip, prefix_len) == false) {
        XmppAdvertiseEvpnBgpaas(evpn_table, prefix_ip, prefix_len, vxlan_id,
                                vrf_name, params, bgp_peer, peer_sources);
        return;
    }
    const AgentPath * path =
        LocalVmExportInterface(agent_, evpn_table, route);

    if (!path ) {
        XmppAdvertiseEvpnBgpaas(evpn_table, prefix_ip, prefix_len, vxlan_id,
                                vrf_name, params, bgp_peer, peer_sources);
        return;
    }

    CopyInterfacePathToEvpnTable(path,
        prefix_ip,
        prefix_len,
        bgp_peer,
        RouteParameters(IpAddress(),  // NH address, not needed here
            MacAddress(),
            params.vn_list_,
            params.sg_list_,
            params.communities_,
            params.tag_list_,
            params.path_preference_,
            params.ecmp_load_balance_,
            params.sequence_number_),
        evpn_table);
}

void VxlanRoutingManager::XmppAdvertiseInetTunnel(
        InetUnicastAgentRouteTable *inet_table, const IpAddress& prefix_ip,
        const int prefix_len, const std::string vrf_name,
        const AgentPath* path) {

    DBRequest nh_req(DBRequest::DB_ENTRY_ADD_CHANGE);
    TunnelNH *tun_nh = dynamic_cast<TunnelNH*>(path->nexthop());

    TunnelNHKey *tun_nh_key = AllocateTunnelNextHopKey(*(tun_nh->GetDip()),
        tun_nh->rewrite_dmac());
    std::string origin_vn = "";

    nh_req.key.reset(tun_nh_key);
    nh_req.data.reset(new TunnelNHData);

    inet_table->AddEvpnRoutingRoute(prefix_ip,
        prefix_len,
        inet_table->vrf_entry(),
        routing_vrf_vxlan_bgp_peer_,
        path->sg_list(),
        path->communities(),
        path->path_preference(),
        path->ecmp_load_balance(),
        path->tag_list(),
        nh_req,
        path->vxlan_id(),
        path->dest_vn_list(),
        origin_vn);
}

void VxlanRoutingManager::XmppAdvertiseInetInterfaceOrComposite(
        InetUnicastAgentRouteTable *inet_table, const IpAddress& prefix_ip,
        const int prefix_len, const std::string vrf_name,
        const AgentPath* path) {

    CopyPathToInetTable(path,
        prefix_ip,
        prefix_len,
        routing_vrf_vxlan_bgp_peer_,
        RouteParameters(IpAddress(),
            MacAddress(),
            path->dest_vn_list(),
            path->sg_list(),
            path->communities(),
            path->tag_list(),
            path->path_preference(),
            path->ecmp_load_balance(),
            path->peer_sequence_number()),
        inet_table);
}

template<class RouteTable, class RouteEntry>
static const AgentPath *LocalVmExportInterface(Agent* agent,
    RouteTable *table, RouteEntry *route) {
    if (table == NULL || agent == NULL || route == NULL) {
        return NULL;
    }

    const AgentPath *tm_path = NULL;
    const AgentPath *rt_path = NULL;

    for (Route::PathList::const_iterator it =
        route->GetPathList().begin();
        it != route->GetPathList().end(); it++) {
        tm_path =
            static_cast<const AgentPath *>(it.operator->());
        if (tm_path == NULL || tm_path->peer() == NULL) {
            continue;
        }
        if (tm_path->peer()->GetType() ==
            agent->local_vm_export_peer()->GetType()) {
            if (tm_path->nexthop() &&
                tm_path->nexthop()->GetType() == NextHop::INTERFACE) {
                rt_path = tm_path;
            } else if (tm_path->nexthop() &&
                tm_path->nexthop()->GetType() == NextHop::COMPOSITE) {
                return tm_path;
            }
        }
    }
    return rt_path;
}

//
//END-OF-FILE
//

