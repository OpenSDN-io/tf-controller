#include <oper/route_common.h>
#include <controller/controller_init.h>
#include <controller/controller_route_path.h>
#include <xmpp_enet_types.h>
#include <xmpp_unicast_types.h>
#include <oper/tunnel_nh.h>
#include <oper/vxlan_routing_manager.h>
#include <oper/bgp_as_service.h>


static void AdvertiseLocalRoute(const IpAddress &prefix_ip,
    const uint32_t plen,
    DBRequest &nh_req,
    const Peer *peer,
    const RouteParameters& params,
    EvpnAgentRouteTable *evpn_table);

static bool IsGivenTypeCompositeNextHop(const NextHop *nh,
    NextHop::Type nh_type, bool strict_match = true) {
    if (nh->GetType() != NextHop::COMPOSITE) {
        return false;
    }
    const CompositeNH *composite_nh =
        dynamic_cast<const CompositeNH*>(nh);
    uint32_t comp_nh_count = composite_nh->ComponentNHCount();
    for (uint32_t i=0; i < comp_nh_count; i++) {
        const NextHop * c_nh = composite_nh->GetNH(i);
        if (c_nh != NULL) {
            // return true if at least one componet is interface
            if (c_nh->GetType() == nh_type &&
                !strict_match) {
                return true;
            }
            // return true only if all components are interfaces
            if (c_nh->GetType() != nh_type &&
                strict_match) {
                return false;
            }
        }
    }
    return strict_match;
}

//
// VxlanRoutingManager members
//

uint32_t VxlanRoutingManager::GetNewLocalSequence(const AgentPath* path) {
    tbb::mutex::scoped_lock lock(mutex_);
    const NextHop *path_nh = path->nexthop();
    if (path_nh->GetType() != NextHop::COMPOSITE) {
        return 0;
    }
    loc_sequence_++;
    return loc_sequence_;
}

bool VxlanRoutingManager::is_ipv4_string(const std::string& prefix_str) {
    return (prefix_str.find(".") != std::string::npos);
}

bool VxlanRoutingManager::is_ipv6_string(const std::string& prefix_str) {
    return (prefix_str.find(":") != std::string::npos) &&
            !is_ipv4_string(prefix_str);
}

uint32_t VxlanRoutingManager::ipv4_prefix_len(const std::string& prefix_str) {
    const std::string::size_type slash_pos = prefix_str.rfind("/");
    if (slash_pos == std::string::npos) {
        return 32;
    }
    const std::string len_str = prefix_str.substr(slash_pos + 1);
    uint32_t prefix_len = 0;
    std::istringstream(len_str) >> prefix_len;
    return std::min(uint32_t(32), prefix_len);
}

std::string VxlanRoutingManager::ipv4_prefix(const std::string& prefix_str) {
    std::string::size_type first_dot_pos = 0;
    std::string::size_type last_colon_pos =
        prefix_str.rfind(":");
    std::string::size_type slash_pos = prefix_str.rfind("/");
    std::string ip_str = "0.0.0.0";
    if ((first_dot_pos = prefix_str.find(".")) != std::string::npos) {
        if (first_dot_pos - last_colon_pos >= 2 &&
            first_dot_pos - last_colon_pos <= 4) {
                ip_str = prefix_str.substr(last_colon_pos + 1);
        }
        if (last_colon_pos == string::npos) {
            ip_str = prefix_str;
        }
        if (slash_pos != string::npos) {
            ip_str = ip_str.substr(0, slash_pos);
        }
    }
    return ip_str;
}

uint32_t VxlanRoutingManager::ipv6_prefix_len(const std::string& prefix_str) {
    const std::string::size_type slash_pos = prefix_str.rfind("/");
    if (slash_pos == std::string::npos) {
        return 128;
    }
    const std::string len_str = prefix_str.substr(slash_pos + 1);
    uint32_t prefix_len = 0;
    std::istringstream(len_str) >> prefix_len;
    return std::min(uint32_t(128), prefix_len);
}

std::string VxlanRoutingManager::ipv6_prefix(const std::string& prefix_str) {
    const std::string zero_mac_str = "00:00:00:00:00:00";
    const std::string::size_type mac_pos = prefix_str.find(zero_mac_str);
    std::string ip_str = prefix_str;

    if (mac_pos != std::string::npos) {
        ip_str = prefix_str.substr(mac_pos + zero_mac_str.size() + 1);
    }

    const std::string::size_type slash_pos = ip_str.rfind("/");
    if (slash_pos != std::string::npos) {
        ip_str = ip_str.substr(0, slash_pos);
    }
    if (ip_str.find(":") == std::string::npos) {
        ip_str = "::";
    }
    return ip_str;
}

bool VxlanRoutingManager::IsVxlanAvailable(const Agent* agent) {
    VxlanRoutingManager *vxlan_rt_mgr = NULL;
    if (agent->oper_db()) {
        vxlan_rt_mgr = agent->oper_db()->vxlan_routing_manager();
    }
    if (vxlan_rt_mgr == NULL) {
        return false;
    }
    return true;
}

std::string VxlanRoutingManager::GetOriginVn(const VrfEntry *routing_vrf,
    const IpAddress& ip_addr,
    const uint8_t& plen) {

    std::string origin_vn = "";
    if (routing_vrf->vn()) {
        VxlanRoutingVrfMapper::RoutedVrfInfo &lr_vrf_info =
            vrf_mapper_.lr_vrf_info_map_
            [routing_vrf->vn()->logical_router_uuid()];
        VxlanRoutingVrfMapper::RoutedVrfInfo::BridgeVnListIter it =
            lr_vrf_info.bridge_vn_list_.begin();
        while (it != lr_vrf_info.bridge_vn_list_.end()) {
            InetUnicastRouteEntry *rt =
                (*it)->GetVrf()->GetUcRoute(ip_addr);
            if (rt && RoutePrefixIsEqualTo(rt, ip_addr, plen)) {
                origin_vn = (*it)->GetName();
                break;
            }
            it++;
        }
    }

    return origin_vn;
}

bool VxlanRoutingManager::RoutePrefixIsEqualTo(const EvpnRouteEntry* route,
    const IpAddress& prefix_ip,
    const uint32_t prefix_len) {
    if (route == NULL ||
        route->prefix_address() != prefix_ip ||
        route->prefix_length() != prefix_len) {
        return false;
    }
    return true;
}

bool VxlanRoutingManager::RoutePrefixIsEqualTo(const InetUnicastRouteEntry* route,
        const IpAddress& prefix_ip,
        const uint32_t prefix_len) {
    if (route == NULL ||
        route->prefix_address() != prefix_ip ||
        route->prefix_length() != prefix_len) {
        return false;
    }
    return true;
}

bool VxlanRoutingManager::IsHostRoute(const IpAddress& prefix_ip, uint32_t prefix_len) {
    if (prefix_ip.is_v4() && prefix_len == 32)
        return true;
    if (prefix_ip.is_v6() && prefix_len == 128)
        return true;
    return false;
}

bool VxlanRoutingManager::IsHostRoute(const EvpnRouteEntry *evpn_rt) {
    if (evpn_rt != NULL) {
        return IsHostRoute(evpn_rt->prefix_address(), evpn_rt->prefix_length());
    }
    return false;
}

bool VxlanRoutingManager::IsHostRouteFromLocalSubnet(const EvpnRouteEntry *rt) {
    if(rt->vrf() == NULL || rt->vrf()->vn() == NULL){
        LOG(ERROR, "Error in VxlanRoutingManager::IsHostRouteFromLocalSubnet"
        << ", vrf == NULL || vrf()->vn() == NULL");
        assert(rt->vrf() && rt->vrf()->vn());
    }

    if (IsHostRoute(rt) == false) {
        return false;
    }

    const boost::uuids::uuid lr_uuid =
        vrf_mapper_.GetLogicalRouterUuidUsingRoute(rt);
    if (lr_uuid == boost::uuids::nil_uuid()) {
        return false;
    }

    const VxlanRoutingVrfMapper::RoutedVrfInfo& vr_info =
        vrf_mapper_.lr_vrf_info_map_.at(lr_uuid);
    const VxlanRoutingVrfMapper::RoutedVrfInfo::BridgeVnList& bridge_vns =
        vr_info.bridge_vn_list_;

    const VnEntry *bridge_vn = NULL;
    VxlanRoutingVrfMapper::RoutedVrfInfo::BridgeVnListIter it_br =
        bridge_vns.begin();
    while (it_br != bridge_vns.end()) {
        bridge_vn = *it_br;
        const std::vector<VnIpam> &VnIpams = bridge_vn->GetVnIpam();
        for (uint32_t j=0; j < VnIpams.size(); j++) {
            if (VnIpams[j].IsSubnetMember(rt->prefix_address())) {
                return true;
            }
        }
        it_br++;
    }
    return false;
}

bool VxlanRoutingManager::IsVrfLocalRoute(EvpnRouteEntry *routing_evpn_rt,
    VrfEntry *bridge_vrf) {
    // check that the Inet table holds the corresponding route
    InetUnicastRouteEntry local_vm_route_key(
        bridge_vrf,
        routing_evpn_rt->prefix_address(),
        routing_evpn_rt->prefix_length(), false);

    InetUnicastAgentRouteTable *inet_table =
        bridge_vrf->GetInetUnicastRouteTable(routing_evpn_rt->prefix_address());
    InetUnicastRouteEntry *inet_rt =
        dynamic_cast<InetUnicastRouteEntry *>
        (inet_table->FindLPM(local_vm_route_key));
    if (inet_rt && RoutePrefixIsEqualTo(inet_rt, routing_evpn_rt->prefix_address(),
        routing_evpn_rt->prefix_length())) {
        return inet_rt->FindPath(agent_->evpn_routing_peer()) ? false : true;
    }
    return false;
}

bool VxlanRoutingManager::HasVrfNexthop(const AgentRoute* rt) {
    const Route::PathList & path_list = rt->GetPathList();
    for (Route::PathList::const_iterator it = path_list.begin();
        it != path_list.end(); ++it) {
        const AgentPath* path =
            dynamic_cast<const AgentPath*>(it.operator->());

        if (path != NULL &&
            path->nexthop() != NULL &&
            path->nexthop()->GetType() == NextHop::VRF) {
            return true;
        }
    }
    return false;
}

bool VxlanRoutingManager::HasBgpPeerPath(EvpnRouteEntry *evpn_rt) {
    const Route::PathList & path_list = evpn_rt->GetPathList();
    for (Route::PathList::const_iterator it = path_list.begin();
        it != path_list.end(); ++it) {
        const AgentPath* path =
            dynamic_cast<const AgentPath*>(it.operator->());
        if (path != NULL &&
            path->peer() != NULL &&
            path->peer()->GetType() == Peer::BGP_PEER) {
            return true;
        }
    }
    return false;
}

bool VxlanRoutingManager::IsRoutingVrf(const VrfEntry* vrf) {
    return vrf && vrf->vn() &&
        vrf->vn()->vxlan_routing_vn();
    // An alternative method
    // if (vrf == NULL) {
    //     return false;
    // }
    // if (vrf->vn() == NULL) {
    //     return false;
    // }
    // if (vrf_mapper_.lr_vrf_info_map_.count(
    //     vrf->vn()->logical_router_uuid())) {
    //     const VxlanRoutingVrfMapper::RoutedVrfInfo &lr_vrf_info =
    //         vrf_mapper_.lr_vrf_info_map_.at(vrf->vn()->logical_router_uuid());
    //     if (lr_vrf_info.routing_vrf_ == vrf) {
    //         return true;
    //     }
    // }
    // return false;
}

bool VxlanRoutingManager::IsBridgeVrf(const VrfEntry* vrf) {
    return vrf && vrf->vn() &&
        !vrf->vn()->vxlan_routing_vn();
}


bool VxlanRoutingManager::IsRoutingVrf(const std::string vrf_name,
    const Agent* agent) {
    VxlanRoutingManager *vxlan_rt_mgr = NULL;
    const VrfEntry *vrf_cand = NULL;
    if (agent->oper_db()) {
        vxlan_rt_mgr = agent->oper_db()->vxlan_routing_manager();
    }
    if (vxlan_rt_mgr == NULL) {
        return false;
    }
    if (agent->vrf_table())
        vrf_cand = agent->vrf_table()->FindVrfFromName(vrf_name);
    if (vrf_cand == NULL) {
        return false;
    }
    return vxlan_rt_mgr->IsRoutingVrf(vrf_cand);
}

const AgentPath* VxlanRoutingManager::FindPathWithGivenPeer(
    const AgentRoute *inet_rt,
    const Peer::Type peer_type) {
    if (inet_rt == NULL)
        return NULL;

    const Route::PathList & path_list = inet_rt->GetPathList();
    for (Route::PathList::const_iterator it = path_list.begin();
        it != path_list.end(); ++it) {
        const AgentPath* path =
            dynamic_cast<const AgentPath*>(it.operator->());

        if (path != NULL &&
            path->peer()->GetType() == peer_type) {
                return path;
        }
    }
    return NULL;
}

const AgentPath* VxlanRoutingManager::FindPathWithGivenPeerAndNexthop(
    const AgentRoute *route,
    const Peer::Type peer_type,
    const NextHop::Type nh_type,
    bool strict_match) {
    if (route == NULL)
        return NULL;

    const Route::PathList & path_list = route->GetPathList();
    for (Route::PathList::const_iterator it = path_list.begin();
        it != path_list.end(); ++it) {
        const AgentPath* path =
            dynamic_cast<const AgentPath*>(it.operator->());

        if (path != NULL &&
            path->nexthop() != NULL &&
            path->peer() != NULL &&
            path->peer()->GetType() == peer_type) {
            if (path->nexthop()->GetType() == nh_type) {
                return path;
            }
            if (IsGivenTypeCompositeNextHop(path->nexthop(), nh_type,
                strict_match)) {
                return path;
            }
        }
    }
    return NULL;
}

const AgentPath* VxlanRoutingManager::FindInterfacePathWithGivenPeer(
    const AgentRoute *inet_rt,
    const Peer::Type peer_type,
    bool strict_match) {
    return FindPathWithGivenPeerAndNexthop(inet_rt,
        peer_type, NextHop::INTERFACE, strict_match);
}

const AgentPath *VxlanRoutingManager::FindInterfacePathWithBgpPeer(
    const AgentRoute *inet_rt,
    bool strict_match) {
    return FindInterfacePathWithGivenPeer(inet_rt, Peer::BGP_PEER,
        strict_match);
}

const AgentPath* VxlanRoutingManager::FindInterfacePathWithLocalVmPeer(
    const AgentRoute *inet_rt,
    bool strict_match) {
    return FindInterfacePathWithGivenPeer(inet_rt, Peer::LOCAL_VM_PORT_PEER,
        strict_match);
}

MacAddress VxlanRoutingManager::NbComputeMac(const Ip4Address& compute_ip,
    const Agent *agent) {
    MacAddress compute_mac;
    VrfEntry *underlay_vrf = agent->fabric_policy_vrf();
    InetUnicastRouteEntry *router_rt = NULL;
    router_rt = underlay_vrf->GetUcRoute(compute_ip);
    if (router_rt != NULL &&
        router_rt->prefix_address() == compute_ip) {
        const AgentPath *apath = FindPathWithGivenPeerAndNexthop(router_rt,
            Peer::BGP_PEER, NextHop::TUNNEL);
        if (apath) {
            const TunnelNH * tunl_nh =
                dynamic_cast<const TunnelNH*>(apath->nexthop());
            if (tunl_nh->GetDmac()) {
                compute_mac = *(tunl_nh->GetDmac());
            }
        }
    }
    return compute_mac;
}

//Finds route in a EVPN table
AgentRoute *VxlanRoutingManager::FindEvpnOrInetRoute(const Agent *agent,
    const std::string &vrf_name,
    const IpAddress &ip_addr,
    unsigned int prefix_len,
    const autogen::EnetNextHopType &nh_item) {

    const unsigned int ethernet_tag = 0;

    EvpnRouteEntry *evpn_rt =
        EvpnAgentRouteTable::FindRoute(agent,
            vrf_name,
            MacAddress(),
            ip_addr,
            prefix_len,
            ethernet_tag);
    if (RoutePrefixIsEqualTo(evpn_rt, ip_addr, prefix_len)) {
        return evpn_rt;
    }
    return NULL;
}

//Finds route in an Inet table
AgentRoute *VxlanRoutingManager::FindEvpnOrInetRoute(const Agent *agent,
    const std::string &vrf_name,
    const IpAddress &ip_addr,
    unsigned int prefix_len,
    const autogen::NextHopType &nh_item) {

    VrfEntry *vrf_entry =
        agent->vrf_table()->FindVrfFromName(vrf_name);
    if (vrf_entry == NULL)
        return NULL;

    InetUnicastAgentRouteTable *inet_tbl =
        vrf_entry->GetInetUnicastRouteTable(ip_addr);
    if (inet_tbl == NULL)
        return NULL;

    InetUnicastRouteEntry local_vm_route_key(inet_tbl->vrf_entry(),
        ip_addr,
        prefix_len, false);
    InetUnicastRouteEntry *inet_rt =
        dynamic_cast<InetUnicastRouteEntry *>
        (inet_tbl->FindLPM(local_vm_route_key));
    if (RoutePrefixIsEqualTo(inet_rt, ip_addr, prefix_len)) {
        return inet_rt;
    }
    return NULL;
}

static bool InitializeNhRequest(const NextHop *path_nh,
    DBRequest &nh_req,
    const std::string& vrf_name) {
    NextHopKey * orig_key = dynamic_cast<NextHopKey*>(
        path_nh->GetDBRequestKey().get())->Clone();

    if(orig_key == NULL) {
        LOG(ERROR, "Error in InitializeNhRequest"
        << ", orig_key == NULL");
        assert(orig_key != NULL);
    }

    nh_req.key.reset(orig_key);
    if (path_nh->GetType() == NextHop::INTERFACE) {
        InterfaceNHKey *intf_orig_key =
            dynamic_cast<InterfaceNHKey*>(orig_key);
        if(intf_orig_key == NULL) {
            LOG(ERROR, "Error in InitializeNhRequest"
            << ", intf_orig_key == NULL");
            assert(intf_orig_key != NULL);
        }
        // if NH is an interface, then update flags
        intf_orig_key->set_flags(intf_orig_key->flags() |
            InterfaceNHFlags::VXLAN_ROUTING);
        nh_req.data.reset(new InterfaceNHData(vrf_name));
    } else if (path_nh->GetType() == NextHop::COMPOSITE) {
        nh_req.data.reset(new CompositeNHData);
    } else {  // other types of NH are not expected here
        LOG(ERROR, "Error in InitializeNhRequest"
        << ", Wrong NH type:" << path_nh->GetType());
        assert(
            path_nh->GetType() == NextHop::INTERFACE ||
            path_nh->GetType() == NextHop::COMPOSITE);
        return false;
    }
    return true;
}

//
// EVPN routes advertising
//

static void AdvertiseLocalRoute(const IpAddress &prefix_ip,
    const uint32_t plen,
    DBRequest &nh_req,
    const Peer *peer,
    const RouteParameters& params,
    EvpnAgentRouteTable *evpn_table
) {
    EvpnRoutingData *rt_data = new EvpnRoutingData(nh_req,
        params.sg_list_,
        params.communities_,
        params.path_preference_,
        params.ecmp_load_balance_,
        params.tag_list_,
        evpn_table->vrf_entry(),
        evpn_table->vrf_entry()->vxlan_id(),
        params.vn_list_);
    evpn_table->AddType5Route(peer,
        evpn_table->vrf_entry()->GetName(),
        prefix_ip,
        0,  // ethernet_tag = 0 for Type5
        rt_data,
        plen);
}

static void AdvertiseInterfaceBgpRoute(const IpAddress &prefix_ip,
    const uint32_t plen,
    DBRequest &nh_req,
    const Peer *peer,
    const AgentPath* path,
    EvpnAgentRouteTable *evpn_table
) {
    const NextHop *path_nh = path->nexthop();

    const InterfaceNH *intf_nh = dynamic_cast<const InterfaceNH *>
        (path_nh);
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
        path->vxlan_id(),
        path->force_policy(),
        path->dest_vn_list(),
        intf_nh->GetFlags(),  // flags
        path->sg_list(),
        path->tag_list(),
        path->communities(),
        path->path_preference(),
        path->subnet_service_ip(),
        path->ecmp_load_balance(),
        path->is_local(),
        path->is_health_check_service(),
        path->sequence(),
        path->etree_leaf(),
        false);  // native_encap
        loc_rt_ptr->set_tunnel_bmap(TunnelType::VxlanType());

    {
        DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);

        req.key.reset(new EvpnRouteKey(peer,
            evpn_table->vrf_entry()->GetName(),
            MacAddress(),
            prefix_ip,
            plen,
            0));
        req.data.reset(loc_rt_ptr);
        evpn_table->Enqueue(&req);
    }
}

static void AdvertiseCompositeInterfaceBgpRoute(const IpAddress &prefix_ip,
    const uint32_t plen,
    DBRequest &nh_req,
    const Peer *peer,
    const AgentPath* path,
    EvpnAgentRouteTable *evpn_table
) {
    const BgpPeer *bgp_peer = dynamic_cast<const BgpPeer*>(peer);
    std::stringstream prefix_str;
    prefix_str << prefix_ip.to_string();
    prefix_str << "/";
    prefix_str << plen;
    std::string vrf_name = evpn_table->vrf_name();

    ControllerEcmpRoute *rt_data = new ControllerEcmpRoute(
        bgp_peer,
        path->dest_vn_list(),
        path->ecmp_load_balance(),
        path->tag_list(),
        path->sg_list(),
        path->path_preference(),
        TunnelType::VxlanType(),
        nh_req,
        prefix_str.str(),
        evpn_table->vrf_name());

    ControllerEcmpRoute::ClonedLocalPathListIter iter =
        rt_data->cloned_local_path_list().begin();
    while (iter != rt_data->cloned_local_path_list().end()) {
        evpn_table->AddClonedLocalPathReq(bgp_peer, vrf_name,
            MacAddress(), prefix_ip, 0, (*iter));
        iter++;
    }

    evpn_table->AddRemoteVmRouteReq(bgp_peer,
        vrf_name, MacAddress(),
        prefix_ip,
        plen,
        0,  // ethernet tag is 0 for VxLAN
        rt_data);
}

//
// Inet routes advertising
//
static void AdvertiseLocalRoute(const IpAddress &prefix_ip,
    const uint32_t plen,
    DBRequest &nh_req,
    const Peer *peer,
    const RouteParameters& params,
    InetUnicastAgentRouteTable *inet_table,
    const std::string& origin_vn
) {
    inet_table->AddEvpnRoutingRoute(prefix_ip,
        plen,
        inet_table->vrf_entry(),
        peer,
        params.sg_list_,
        params.communities_,
        params.path_preference_,
        params.ecmp_load_balance_,
        params.tag_list_,
        nh_req,
        inet_table->vrf_entry()->vxlan_id(),
        params.vn_list_,
        origin_vn);
}

void VxlanRoutingManager::DeleteOldInterfacePath(const IpAddress &prefix_ip,
    const uint32_t plen,
    const Peer *peer,
    EvpnAgentRouteTable *evpn_table) {

    if (peer != routing_vrf_interface_peer_) {
        return;
    }

    EvpnRouteEntry *rt_entry = evpn_table->FindRoute(MacAddress(),
        prefix_ip, plen, 0);
    if (rt_entry && RoutePrefixIsEqualTo(rt_entry, prefix_ip, plen)) {
        // Delete the old non-BGP path if it exists
        AgentPath *old_path = rt_entry->FindPath(peer);
        if (old_path) {
            rt_entry->DeletePathFromPeer(rt_entry->get_table_partition(),
                evpn_table, old_path);
        }
    }
}

void VxlanRoutingManager::CopyInterfacePathToEvpnTable(const AgentPath* path,
    const IpAddress &prefix_ip,
    const uint32_t plen,
    const Peer *peer,
    const RouteParameters &params,
    EvpnAgentRouteTable *evpn_table) {
    const NextHop *path_nh = path != NULL ? path->nexthop() : NULL;

    if (path_nh == NULL) {
        return;
    }

    DBRequest nh_req(DBRequest::DB_ENTRY_ADD_CHANGE);
    if (InitializeNhRequest(path_nh,
        nh_req, evpn_table->vrf_entry()->GetName()) == false) {
        return;
    }

    if (peer->GetType() == Peer::BGP_PEER) {
        if (path_nh->GetType() == NextHop::INTERFACE) {
            AdvertiseInterfaceBgpRoute(
                prefix_ip, plen, nh_req, peer, path, evpn_table);
        } else if (path_nh->GetType() == NextHop::COMPOSITE) {
            AdvertiseCompositeInterfaceBgpRoute(
                prefix_ip, plen, nh_req, peer, path, evpn_table);
        }
    } else {
        AdvertiseLocalRoute(prefix_ip, plen, nh_req, peer, params,
            evpn_table);
    }
}

void VxlanRoutingManager::DeleteOldInterfacePath(const IpAddress &prefix_ip,
    const uint32_t plen,
    const Peer *peer,
    InetUnicastAgentRouteTable *inet_table) {
    if (peer != routing_vrf_interface_peer_) {
        return;
    }

    InetUnicastRouteEntry old_rt_key(inet_table->vrf_entry(),
       prefix_ip, plen, false);
    InetUnicastRouteEntry *rt_entry = inet_table->FindRouteUsingKey(old_rt_key);
    if (rt_entry && RoutePrefixIsEqualTo(rt_entry, prefix_ip, plen)) {
        // Delete the old non-BGP path if it exists
        AgentPath *old_path = rt_entry->FindPath(peer);
        if (old_path) {
            rt_entry->DeletePathFromPeer(rt_entry->get_table_partition(),
                inet_table, old_path);
        }
    }
}

void VxlanRoutingManager::CopyPathToInetTable(const AgentPath* path,
    const IpAddress &prefix_ip,
    const uint32_t plen,
    const Peer *peer,
    const RouteParameters &params,
    InetUnicastAgentRouteTable *inet_table) {
    const NextHop *path_nh = path != NULL ? path->nexthop() : NULL;
    if (path_nh == NULL || inet_table == NULL || peer == NULL) {
        return;
    }


    DBRequest nh_req(DBRequest::DB_ENTRY_ADD_CHANGE);
    if (InitializeNhRequest(path_nh,
        nh_req, inet_table->vrf_entry()->GetName()) == false) {
        return;
    }

   std::string origin_vn = "";
    if (peer->GetType() == Peer::VXLAN_BGP_PEER) {
    } else {
        origin_vn = GetOriginVn(inet_table->vrf_entry(), prefix_ip, plen);
    }
    AdvertiseLocalRoute(prefix_ip, plen, nh_req, peer, params,
        inet_table, origin_vn);
}


void VxlanRoutingManager::PrintEvpnTable(const VrfEntry* const_vrf) {
    if (const_vrf == NULL) {
        std::cout<< "VxlanRoutingManager::PrintEvpnTable"
                 << ", NULL vrf ptr"
                 << std::endl;
        return;
    }
    VrfEntry* routing_vrf = const_cast<VrfEntry*>(const_vrf);
    EvpnAgentRouteTable *evpn_table = dynamic_cast<EvpnAgentRouteTable *>
        (routing_vrf->GetEvpnRouteTable());
    if (evpn_table == NULL) {
        std::cout<< "VxlanRoutingManager::PrintEvpnTable"
                 << ", NULL EVPN tbl ptr"
                 << std::endl;
        return;
    }
    EvpnRouteEntry *c_entry = dynamic_cast<EvpnRouteEntry *>
        (evpn_table->GetTablePartition(0)->GetFirst());
    if (c_entry) {
        if (c_entry->IsType5())
            std::cout << "Evpn Type 5 table:" << std::endl;
        else
            std::cout << "Evpn Type 2 table:" << std::endl;
    }
    while (c_entry) {
        const Route::PathList & path_list = c_entry->GetPathList();
        std::cout<< "  IP:" << c_entry->prefix_address()
                 << ", path count = " << path_list.size()
                 << ", ethernet_tag = " << c_entry->ethernet_tag()
                 << std::endl;
        for (Route::PathList::const_iterator it = path_list.begin();
            it != path_list.end(); ++it) {
            const AgentPath* path =
                dynamic_cast<const AgentPath*>(it.operator->());
            if (!path)
                continue;
            std::cout<< "    NH: "
                        << (path->nexthop() ? path->nexthop()->ToString() :
                        "NULL")
                        << ", " << "Peer:"
                        << (path->peer() ? path->peer()->GetName() : "NULL")
                        << std::endl;
            if (path->nexthop()->GetType() == NextHop::COMPOSITE) {
                CompositeNH *comp_nh = dynamic_cast<CompositeNH *>
                    (path->nexthop());
                std::cout<< "    n components="
                            << comp_nh->ComponentNHCount()
                            << std::endl;
            }
        }
        if (evpn_table && evpn_table->GetTablePartition(0))
            c_entry = dynamic_cast<EvpnRouteEntry *>
                (evpn_table->GetTablePartition(0)->GetNext(c_entry));
        else
            break;
    }
}

void VxlanRoutingManager::PrintInetTable(const VrfEntry* const_vrf) {
    if (const_vrf == NULL) {
        std::cout<< "VxlanRoutingManager::PrintInetTable"
                 << ", NULL vrf ptr"
                 << std::endl;
        return;
    }
    VrfEntry* routing_vrf = const_cast<VrfEntry*>(const_vrf);
    InetUnicastAgentRouteTable *inet_table =
        routing_vrf->GetInet4UnicastRouteTable();
    if (inet_table == NULL) {
        std::cout<< "VxlanRoutingManager::PrintInetTable"
                 << ", NULL Inet tbl ptr"
                 << std::endl;
        return;
    }
    InetUnicastRouteEntry *c_entry = dynamic_cast<InetUnicastRouteEntry *>
        (inet_table->GetTablePartition(0)->GetFirst());
    if (c_entry) {
            std::cout << "Inet table:" << std::endl;
    }
    while (c_entry) {
        const Route::PathList & path_list = c_entry->GetPathList();
        std::cout<< "  IP:" << c_entry->prefix_address()
                 << ", path count = " << path_list.size() << std::endl;
        for (Route::PathList::const_iterator it = path_list.begin();
            it != path_list.end(); ++it) {
            const AgentPath* path =
                dynamic_cast<const AgentPath*>(it.operator->());
            if (!path)
                continue;
            std::cout<< "    NH: "
                        << (path->nexthop() ? path->nexthop()->ToString() :
                        "NULL")
                        << ", " << "Peer:"
                        << (path->peer() ? path->peer()->GetName() : "NULL")
                        << ", nh ptr = " << path->nexthop()
                        << ", pt ptr = " << path
                        << std::endl;
            // if (path->nexthop()->GetType() == NextHop::COMPOSITE) {
            //     CompositeNH *comp_nh = dynamic_cast<CompositeNH *>
            //         (path->nexthop());
            //     std::cout<< "    n components="
            //                 << comp_nh->ComponentNHCount()
            //                 << std::endl;
            // }
        }
        if (inet_table && inet_table->GetTablePartition(0))
            c_entry = dynamic_cast<InetUnicastRouteEntry *>
                (inet_table->GetTablePartition(0)->GetNext(c_entry));
        else
            break;
    }
}

void VxlanRoutingManager::ListAttachedVns() {
    Agent *agent = Agent::GetInstance();
    if (agent == NULL) {
        std::cout<<"VxlanRoutingManager::ListAttachedVns agent == NULL"<<std::endl;
        return;
    }
    VxlanRoutingManager *vxlan_rt_mgr = agent->oper_db()->vxlan_routing_manager();
    if (vxlan_rt_mgr == NULL) {
        std::cout<<"VxlanRoutingManager::ListAttachedVns rt mgr = NULL"<<std::endl;
        return;
    }
    VxlanRoutingVrfMapper& vrf_mapper = vxlan_rt_mgr->vrf_mapper_;
    VxlanRoutingVrfMapper::LrVrfInfoMap& lr_vrf_info_map = vrf_mapper.lr_vrf_info_map_;
    for(VxlanRoutingVrfMapper::LrVrfInfoMap::iterator it = lr_vrf_info_map.begin();
        it != lr_vrf_info_map.end(); it++) {
            std::cout << "VxlanRoutingManager::ListAttachedVns, "
                      << "rt VRF = "
                      << (*it).second.routing_vrf_->GetName()
                      << std::endl;
            std::cout << "VxlanRoutingManager::ListAttachedVns, "
                      << "size = "
                      << (*it).second.bridge_vn_list_.size()
                      << std::endl;
            VxlanRoutingVrfMapper::RoutedVrfInfo::BridgeVnList &reg_br =
                (*it).second.bridge_vn_list_;
            for(VxlanRoutingVrfMapper::RoutedVrfInfo::BridgeVnList::iterator it_br = reg_br.begin();
                it_br != reg_br.end(); it_br++) {
                if ((*it_br))
                std::cout<< "VxlanRoutingManager::ListAttachedVns, "
                         << "br VN = " << (*it_br)->GetName()
                         << std::endl;
            }
    }
}

//
//END-OF-FILE
//
