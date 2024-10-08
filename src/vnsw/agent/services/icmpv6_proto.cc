/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "base/os.h"
#include <init/agent_init.h>
#include <pkt/pkt_handler.h>
#include "pkt/pkt_init.h"
#include <oper/route_common.h>
#include <services/icmpv6_proto.h>
#include "mac_learning/mac_learning_proto.h"

Icmpv6Proto::Icmpv6Proto(Agent *agent, boost::asio::io_context &io) :
    Proto(agent, "Agent::Services", PktHandler::ICMPV6, io) {
    // limit the number of entries in the workqueue
    work_queue_.SetSize(agent->params()->services_queue_limit());
    work_queue_.SetBounded(true);

    vn_table_listener_id_ = agent->vn_table()->Register(
                             boost::bind(&Icmpv6Proto::VnNotify, this, _2));
    vrf_table_listener_id_ = agent->vrf_table()->Register(
                             boost::bind(&Icmpv6Proto::VrfNotify, this, _1, _2));
    interface_listener_id_ = agent->interface_table()->Register(
                             boost::bind(&Icmpv6Proto::InterfaceNotify,
                                         this, _2));
    nexthop_listener_id_ = agent->nexthop_table()->Register(
                           boost::bind(&Icmpv6Proto::NexthopNotify, this, _2));

    boost::shared_ptr<PktInfo> pkt_info(new PktInfo(PktHandler::ICMPV6, NULL));
    icmpv6_handler_.reset(new Icmpv6Handler(agent, pkt_info, io));

    timer_ = TimerManager::CreateTimer(io, "Icmpv6Timer",
             TaskScheduler::GetInstance()->GetTaskId("Agent::Services"),
             PktHandler::ICMPV6);
    timer_->Start(kRouterAdvertTimeout,
                  boost::bind(&Icmpv6Handler::RouterAdvertisement,
                  icmpv6_handler_.get(), this));
}

Icmpv6Proto::~Icmpv6Proto() {
}

void Icmpv6Proto::Shutdown() {
    agent_->vn_table()->Unregister(vn_table_listener_id_);
    agent_->vrf_table()->Unregister(vrf_table_listener_id_);
    agent_->interface_table()->Unregister(interface_listener_id_);
    timer_->Cancel();
    TimerManager::DeleteTimer(timer_);
}

ProtoHandler *Icmpv6Proto::AllocProtoHandler(boost::shared_ptr<PktInfo> info,
                                             boost::asio::io_context &io) {
    return new Icmpv6Handler(agent(), info, io);
}

Icmpv6VrfState *Icmpv6Proto::CreateAndSetVrfState(VrfEntry *vrf) {
    Icmpv6VrfState *state = new Icmpv6VrfState(agent_, this, vrf,
                                               vrf->GetInet6UnicastRouteTable(),
                                               vrf->GetEvpnRouteTable());
    state->set_route_table_listener_id(vrf->GetInet6UnicastRouteTable()->
        Register(boost::bind(&Icmpv6VrfState::RouteUpdate, state, _1, _2)));
    state->set_evpn_route_table_listener_id(vrf->GetEvpnRouteTable()->
        Register(boost::bind(&Icmpv6VrfState::EvpnRouteUpdate, state,  _1, _2)));
    vrf->SetState(vrf->get_table_partition()->parent(),
                  vrf_table_listener_id_, state);
    return state;
}

void Icmpv6Proto::VnNotify(DBEntryBase *entry) {
    if (entry->IsDeleted()) return;

    VnEntry *vn = static_cast<VnEntry *>(entry);
    VrfEntry *vrf = vn->GetVrf();
    if (!vrf || vrf->IsDeleted()) return;

    if (vrf->GetName() == agent_->fabric_vrf_name())
        return;

    if (vn->layer3_forwarding()) {
        Icmpv6VrfState *state = static_cast<Icmpv6VrfState *>(vrf->GetState(
                             vrf->get_table_partition()->parent(),
                             vrf_table_listener_id_));
        if (state == NULL) {
            state = CreateAndSetVrfState(vrf);
        }
        if (state->default_routes_added()) {
            return;
        }

        boost::system::error_code ec;
        Ip6Address addr = Ip6Address::from_string(IPV6_ALL_ROUTERS_ADDRESS, ec);
        static_cast<InetUnicastAgentRouteTable *>
            (vrf->GetInet6UnicastRouteTable())->AddHostRoute(vrf->GetName(),
                                                             addr, 128,
                                                             vn->GetName(), false);
        addr = Ip6Address::from_string(IPV6_ALL_NODES_ADDRESS, ec);
        static_cast<InetUnicastAgentRouteTable *>
            (vrf->GetInet6UnicastRouteTable())->AddHostRoute(vrf->GetName(),
                                                             addr, 128,
                                                             vn->GetName(), false);
        /* We need route for PKT0_LINKLOCAL_ADDRESS so that vrouter can respond
         * to NDP requests for PKT0_LINKLOCAL_ADDRESS. Even though the nexthop
         * for this route is pkt0, vrouter never sends pkts pointing to this
         * route on pkt0.
         */
        addr = Ip6Address::from_string(PKT0_LINKLOCAL_ADDRESS, ec);
        static_cast<InetUnicastAgentRouteTable *>
            (vrf->GetInet6UnicastRouteTable())->AddHostRoute(vrf->GetName(),
                                                             addr, 128,
                                                             vn->GetName(), false);
        state->set_default_routes_added(true);
    }
}

void Icmpv6Proto::VrfNotify(DBTablePartBase *part, DBEntryBase *entry) {
    VrfEntry *vrf = static_cast<VrfEntry *>(entry);

    Icmpv6VrfState *state = static_cast<Icmpv6VrfState *>(vrf->GetState(
                             vrf->get_table_partition()->parent(),
                             vrf_table_listener_id_));
    if (entry->IsDeleted()) {
        if (state) {
            boost::system::error_code ec;
            Ip6Address addr =
                Ip6Address::from_string(IPV6_ALL_ROUTERS_ADDRESS, ec);
            // enqueue delete request on fabric VRF
            agent_->fabric_inet4_unicast_table()->DeleteReq(
                    agent_->local_peer(), vrf->GetName(), addr, 128, NULL);
            addr = Ip6Address::from_string(IPV6_ALL_NODES_ADDRESS, ec);
            agent_->fabric_inet4_unicast_table()->DeleteReq(
                    agent_->local_peer(), vrf->GetName(), addr, 128, NULL);
            addr = Ip6Address::from_string(PKT0_LINKLOCAL_ADDRESS, ec);
            agent_->fabric_inet4_unicast_table()->DeleteReq(
                    agent_->local_peer(), vrf->GetName(), addr, 128, NULL);
            state->set_default_routes_added(false);
            state->Delete();
        }
        return;
    }
    if (!state) {
        CreateAndSetVrfState(vrf);
    }
}

void Icmpv6Proto::InterfaceNotify(DBEntryBase *entry) {
    Interface *intrface = static_cast<Interface *>(entry);
    if (intrface->type() != Interface::VM_INTERFACE)
        return;

    Icmpv6Stats stats;
    VmInterface *vm_interface = static_cast<VmInterface *>(entry);
    VmInterfaceMap::iterator it = vm_interfaces_.find(vm_interface);
    if (intrface->IsDeleted()) {
        if (it != vm_interfaces_.end()) {
            vm_interfaces_.erase(it);
        }
        if (vm_interface->vmi_type() == VmInterface::VHOST) {
            set_ip_fabric_interface(NULL);
            set_ip_fabric_interface_index(-1);
        }
    } else {
        if (it == vm_interfaces_.end()) {
            vm_interfaces_.insert(VmInterfacePair(vm_interface, stats));
        }
        if (vm_interface->vmi_type() == VmInterface::VHOST) {
            set_ip_fabric_interface(intrface);
            set_ip_fabric_interface_index(intrface->id());
            set_ip_fabric_interface_mac(intrface->mac());
        }
    }
}

void Icmpv6Proto::SendIcmpv6Ipc(Icmpv6Proto::Icmpv6MsgType type, Ip6Address ip,
                                const VrfEntry *vrf, InterfaceConstRef itf) {
    Icmpv6Ipc *ipc = new Icmpv6Ipc(type, ip, vrf, itf);
    agent_->pkt()->pkt_handler()->SendMessage(PktHandler::ICMPV6, ipc);
}

void Icmpv6Proto::SendIcmpv6Ipc(Icmpv6Proto::Icmpv6MsgType type, NdpKey &key,
                                InterfaceConstRef itf) {
    Icmpv6Ipc *ipc = new Icmpv6Ipc(type, key, itf);
    agent_->pkt()->pkt_handler()->SendMessage(PktHandler::ICMPV6, ipc);
}

void Icmpv6Proto::NexthopNotify(DBEntryBase *entry) {
    NextHop *nh = static_cast<NextHop *>(entry);

    switch(nh->GetType()) {
    case NextHop::NDP: {
        NdpNH *ndp_nh = (static_cast<NdpNH *>(nh));
        if (ndp_nh->IsDeleted()) {
            SendIcmpv6Ipc(Icmpv6Proto::NDP_DELETE, ndp_nh->GetIp()->to_v6(),
                          ndp_nh->GetVrf(), ndp_nh->GetInterface());
        } else if (ndp_nh->IsValid() == false && ndp_nh->GetInterface()) {
            SendIcmpv6Ipc(Icmpv6Proto::NDP_RESOLVE, ndp_nh->GetIp()->to_v6(),
                          ndp_nh->GetVrf(), ndp_nh->GetInterface());
        }
        break;
    }

    default:
        break;
    }
}

bool Icmpv6Proto::ValidateAndClearVrfState(VrfEntry *vrf,
                                           Icmpv6VrfState *vrf_state) {
    if (!vrf->IsDeleted()) {
        return false;
    }

    if (vrf_state->l3_walk_completed() == false) {
        return false;
    }

    if (vrf_state->evpn_walk_completed() == false) {
        return false;
    }

    if (vrf_state->managed_delete_walk_ref().get() != NULL ||
        vrf_state->evpn_walk_ref().get() != NULL) {
        return false;
    }

    DBState *state = static_cast<DBState *>
        (vrf->GetState(vrf->get_table_partition()->parent(),
                       vrf_table_listener_id_));
    if (state) {
        vrf->ClearState(vrf->get_table_partition()->parent(),
                        vrf_table_listener_id_);
    }
    return true;
}

void Icmpv6VrfState::RouteUpdate(DBTablePartBase *part, DBEntryBase *entry) {
    InetUnicastRouteEntry *route = static_cast<InetUnicastRouteEntry *>(entry);

    Icmpv6RouteState *state = static_cast<Icmpv6RouteState *>
        (entry->GetState(part->parent(), route_table_listener_id_));

#if 0
    // This is the code for sending unsolicited NA for vhost0 but it should
    // be taken care of by linux itself
    const InterfaceNH *intf_nh = dynamic_cast<const InterfaceNH *>(
            route->GetActiveNextHop());
    const Interface *intf = (intf_nh) ?
        static_cast<const Interface *>(intf_nh->GetInterface()) : NULL;

    NdpKey key(route->prefix_address().to_v6(), route->vrf());
    NdpEntry *ndpentry = icmp_proto_->UnsolNaEntry(key, intf);
    if (route->vrf()->GetName() == agent_->fabric_vrf_name()) {
        ndpentry = icmp_proto_->UnsolNaEntry(key, icmp_proto_->ip_fabric_interface());
    }
#endif
    if (entry->IsDeleted() || deleted_) {
        if (state) {
            //icmp_proto_->DeleteUnsolNaEntry(ndpentry);
            entry->ClearState(part->parent(), route_table_listener_id_);
            delete state;
        }
        return;
    }

    if (!state) {
        state = new Icmpv6RouteState(this, route->vrf_id(), route->prefix_address(),
                                     route->prefix_length());
        entry->SetState(part->parent(), route_table_listener_id_, state);
    }

#if 0
    // May not be needed since kernel can take care of sending unsolicited NA
    if (route->vrf()->GetName() == agent_->fabric_vrf_name() &&
        route->GetActiveNextHop()->GetType() == NextHop::RECEIVE &&
        icmp_proto_->agent()->router_id6() == route->prefix_address().to_v6()) {
        //Send unsolicited NA
        icmp_proto_->AddUnsolNaEntry(key);
        icmp_proto_->SendIcmpv6Ipc(Icmpv6Proto::NDP_SEND_UNSOL_NA,
                              route->prefix_address().to_v6(), route->vrf(),
                              icmp_proto_->ip_fabric_interface());
    }
#endif

    //Check if there is a local VM path, if yes send a
    //Neighbor Solicit request, to trigger route preference state machine
    if (state && route->GetTableType() == Agent::INET6_UNICAST &&
        route->vrf()->GetName() != agent_->fabric_vrf_name()) {
        state->SendNeighborSolicitForAllIntf(route);
    }
}

void Icmpv6VrfState::EvpnRouteUpdate(DBTablePartBase *part, DBEntryBase *entry) {
    EvpnRouteEntry *route = static_cast<EvpnRouteEntry *>(entry);

    Icmpv6RouteState *state = static_cast<Icmpv6RouteState *>
        (entry->GetState(part->parent(), evpn_route_table_listener_id_));

    if (entry->IsDeleted() || deleted_) {
        if (state) {
            entry->ClearState(part->parent(), evpn_route_table_listener_id_);
            delete state;
        }
        return;
    }

    if (!state) {
        state = new Icmpv6RouteState(this, route->vrf_id(), route->prefix_address(),
                                     route->prefix_length());
        entry->SetState(part->parent(), evpn_route_table_listener_id_, state);
    }

    //Check if there is a local VM path, if yes send a
    //Neighbor Solicit request, to trigger route preference state machine
    if (state && route->vrf()->GetName() != agent_->fabric_vrf_name()) {
        state->SendNeighborSolicitForAllIntf(route);
    }
}

bool Icmpv6VrfState::DeleteRouteState(DBTablePartBase *part, DBEntryBase *ent) {
    RouteUpdate(part, ent);
    return true;
}

bool Icmpv6VrfState::DeleteEvpnRouteState(DBTablePartBase *part,
                                          DBEntryBase *ent) {
    EvpnRouteUpdate(part, ent);
    return true;
}

void Icmpv6VrfState::Delete() {
    if (managed_delete_walk_ref_.get() == NULL)
        return;

    rt_table_->WalkAgain(managed_delete_walk_ref_);
    if (evpn_walk_ref_.get())
        evpn_rt_table_->WalkAgain(evpn_walk_ref_);
    deleted_ = true;
}

bool Icmpv6VrfState::PreWalkDone(DBTableBase *partition) {
    if (icmp_proto_->ValidateAndClearVrfState(vrf_, this) == false) {
        return false;
    }

    rt_table_->Unregister(route_table_listener_id_);
    table_delete_ref_.Reset(NULL);

    evpn_rt_table_->Unregister(evpn_route_table_listener_id_);
    evpn_table_delete_ref_.Reset(NULL);
    return true;
}

void Icmpv6VrfState::WalkDone(DBTableBase *partition, Icmpv6VrfState *state) {
    if (partition == state->rt_table_) {
        state->rt_table_->ReleaseWalker(state->managed_delete_walk_ref_);
        state->managed_delete_walk_ref_ = NULL;
        state->l3_walk_completed_ = true;
    } else {
        state->evpn_rt_table_->ReleaseWalker(state->evpn_walk_ref_);
        state->evpn_walk_ref_ = NULL;
        state->evpn_walk_completed_ = true;
    }

    if (state->PreWalkDone(partition)) {
        delete state;
    }
}

Icmpv6PathPreferenceState* Icmpv6VrfState::Locate(const IpAddress &ip) {
    Icmpv6PathPreferenceState* ptr = icmpv6_path_preference_map_[ip];
    if (ptr == NULL) {
        ptr = new Icmpv6PathPreferenceState(this, vrf_->vrf_id(), ip, 128);
        icmpv6_path_preference_map_[ip] = ptr;
    }
    return ptr;
}

void Icmpv6PathPreferenceState::HandleNA(uint32_t itf) {
    WaitForTrafficIntfMap::iterator it = l3_wait_for_traffic_map_.find(itf);
    if (it == l3_wait_for_traffic_map_.end()) {
        return;
    }
    InterfaceIcmpv6PathPreferenceInfo &data = it->second;

    // resetting ns_try_count as interface sent NA
    data.ns_try_count = 0;

}

void Icmpv6Proto::HandlePathPreferenceNA(const VrfEntry *vrf, uint32_t itf,
                                            IpAddress sip) {
    if (!vrf) {
        return;
    }
    InetUnicastRouteEntry *rt = vrf->GetUcRoute(sip);
    if (!rt) {
        return;
    }

    Icmpv6VrfState *state = static_cast<Icmpv6VrfState *>
        (vrf->GetState(vrf->get_table_partition()->parent(),
                       vrf_table_listener_id_));
    if (!state) {
        return;
    }
    Icmpv6PathPreferenceState *pstate = state->Get(sip);
    if (!pstate) {
        return;
    }
    pstate->HandleNA(itf);
}

void Icmpv6VrfState::Erase(const IpAddress &ip) {
    icmpv6_path_preference_map_.erase(ip);
}

Icmpv6VrfState::Icmpv6VrfState(Agent *agent_ptr, Icmpv6Proto *proto,
                               VrfEntry *vrf_entry, AgentRouteTable *table,
                               AgentRouteTable *evpn_rt_table):
    agent_(agent_ptr), icmp_proto_(proto), vrf_(vrf_entry), rt_table_(table),
    evpn_rt_table_(evpn_rt_table),
    route_table_listener_id_(DBTableBase::kInvalidId),
    evpn_route_table_listener_id_(DBTableBase::kInvalidId),
    table_delete_ref_(this, table->deleter()),
    evpn_table_delete_ref_(this, evpn_rt_table_->deleter()),
    deleted_(false),
    default_routes_added_(false), l3_walk_completed_(false),
    evpn_walk_completed_(false) {
    evpn_walk_ref_ = evpn_rt_table_->AllocWalker(
            boost::bind(&Icmpv6VrfState::DeleteEvpnRouteState, this, _1, _2),
            boost::bind(&Icmpv6VrfState::WalkDone, _2, this));
    managed_delete_walk_ref_ = table->AllocWalker(
            boost::bind(&Icmpv6VrfState::DeleteRouteState, this, _1, _2),
            boost::bind(&Icmpv6VrfState::WalkDone, _2, this));
}

Icmpv6VrfState::~Icmpv6VrfState() {
    assert(icmpv6_path_preference_map_.size() == 0);
}

void intrusive_ptr_add_ref(Icmpv6PathPreferenceState *ps) {
    ps->refcount_.fetch_and_increment();
}

void intrusive_ptr_release(Icmpv6PathPreferenceState *ps) {
    Icmpv6VrfState *state = ps->vrf_state();
    int prev = ps->refcount_.fetch_and_decrement();
    if (prev == 1) {
        state->Erase(ps->ip());
        delete ps;
    }
}

Icmpv6PathPreferenceState::Icmpv6PathPreferenceState(
        Icmpv6VrfState *vrf_state, uint32_t vrf_id,
        IpAddress ip, uint8_t plen) :
    vrf_state_(vrf_state), ns_req_timer_(NULL), vrf_id_(vrf_id), vm_ip_(ip),
    plen_(plen), svc_ip_(Ip6Address()) {
    refcount_ = 0;
}

Icmpv6PathPreferenceState::~Icmpv6PathPreferenceState() {
    if (ns_req_timer_) {
        ns_req_timer_->Cancel();
        TimerManager::DeleteTimer(ns_req_timer_);
    }
    assert(refcount_ == 0);
}

bool Icmpv6PathPreferenceState::SendNeighborSolicit(WaitForTrafficIntfMap
                                                    &wait_for_traffic_map,
                                                    NDTransmittedIntfMap
                                                    &nd_transmitted_map) {
    bool ret = false;
    boost::shared_ptr<PktInfo> pkt(new PktInfo(vrf_state_->agent(),
                                               ICMP_PKT_SIZE,
                                               PktHandler::ICMPV6, 0));
    Icmpv6Handler handler(vrf_state_->agent(), pkt,
                         *(vrf_state_->agent()->event_manager()->io_service()));

    WaitForTrafficIntfMap::iterator it = wait_for_traffic_map.begin();
    for (;it != wait_for_traffic_map.end(); it++) {

        VmInterface *vm_intf = static_cast<VmInterface *>(
             vrf_state_->agent()->interface_table()->FindInterface(it->first));
        if (!vm_intf) {
            continue;
        }
        InterfaceIcmpv6PathPreferenceInfo &data = it->second;
        bool inserted = nd_transmitted_map.insert(it->first).second;
        ++data.ns_retry_count;
        if (inserted == false) {
            continue;
        }

        MacAddress mil_mac = vrf_state_->agent()->mac_learning_proto()->
                        GetMacIpLearningTable()->GetPairedMacAddress(
                                vm_intf->vrf_id(), ip());
        if (mil_mac != MacAddress()) {
            ++data.ns_try_count;
            if (data.ns_try_count == kNSTryCount) {
                IpAddress ip = vm_ip_;
                MacAddress mac = mil_mac;
                vrf_state_->agent()->mac_learning_proto()->
                        GetMacIpLearningTable()->MacIpEntryUnreachable(
                                vm_intf->vrf_id(), ip, mac);
                return true;
            }
        }

        Ip6Address src_addr;
        if (svc_ip_.is_unspecified() == false && svc_ip_.is_v6())
            src_addr = svc_ip_.to_v6();
        handler.SendNeighborSolicit(src_addr, vm_ip_.to_v6(), vm_intf, vrf_id_);

        vrf_state_->icmp_proto()->IncrementStatsNeighborSolicit(vm_intf);
        ++data.ns_send_count;

        // reduce the frequency of NS requests after some tries
        if (data.ns_send_count >= kMaxRetry) {
            // change frequency only if not in gateway mode with remote VMIs and
            // learnt mac is not present
            if (vm_intf->vmi_type() != VmInterface::REMOTE_VM
                 && mil_mac == MacAddress()) {
                ns_req_timer_->Reschedule(kTimeout * kTimeoutMultiplier);
            }
        }

        ret = true;
    }
    return ret;
}

bool Icmpv6PathPreferenceState::SendNeighborSolicit() {
    if (l3_wait_for_traffic_map_.size() == 0 &&
            evpn_wait_for_traffic_map_.size() == 0) {
        return false;
    }

    bool ret = false;
    NDTransmittedIntfMap nd_transmitted_map;
    if (SendNeighborSolicit(l3_wait_for_traffic_map_, nd_transmitted_map)) {
        ret = true;
    }

    if (SendNeighborSolicit(evpn_wait_for_traffic_map_, nd_transmitted_map)) {
        ret = true;
    }
    return ret;
}

void Icmpv6PathPreferenceState::StartTimer() {
    if (ns_req_timer_ == NULL) {
        ns_req_timer_ = TimerManager::CreateTimer(
                *(vrf_state_->agent()->event_manager()->io_service()),
                "Neighbor Solicit Request timer for VM",
                TaskScheduler::GetInstance()->GetTaskId("Agent::Services"),
                PktHandler::ICMPV6);
    }
    ns_req_timer_->Start(kTimeout,
                         boost::bind(&Icmpv6PathPreferenceState::
                                      SendNeighborSolicit,
                                     this));
}

Icmpv6RouteState::Icmpv6RouteState(Icmpv6VrfState *vrf_state,
                                   uint32_t vrf_id, IpAddress ip,
                                   uint8_t plen) {
    if (plen == Address::kMaxV6PrefixLen) {
        icmpv6_path_preference_state_ = vrf_state->Locate(ip);
    }
}

Icmpv6RouteState::~Icmpv6RouteState() {
    icmpv6_path_preference_state_.reset(NULL);
}

void Icmpv6RouteState::SendNeighborSolicitForAllIntf(const AgentRoute *route) {
    if (icmpv6_path_preference_state_.get()) {
        icmpv6_path_preference_state_->SendNeighborSolicitForAllIntf(route);
    }
}

//Send Neighbor Solicit request on interface in Active-BackUp mode
//So that preference of route can be incremented if the VM replies with
//Neighbor Advertisement
void Icmpv6PathPreferenceState::SendNeighborSolicitForAllIntf
    (const AgentRoute *route) {

    WaitForTrafficIntfMap wait_for_traffic_map = evpn_wait_for_traffic_map_;
    if (dynamic_cast<const InetUnicastRouteEntry *>(route)) {
        wait_for_traffic_map = l3_wait_for_traffic_map_;
    }

    WaitForTrafficIntfMap new_wait_for_traffic_map;
    for (Route::PathList::const_iterator it = route->GetPathList().begin();
            it != route->GetPathList().end(); it++) {
        const AgentPath *path = static_cast<const AgentPath *>(it.operator->());
        if (path->peer() &&
            path->peer()->GetType() == Peer::LOCAL_VM_PORT_PEER) {
            const NextHop *nh = path->ComputeNextHop(vrf_state_->agent());
            if (nh->GetType() != NextHop::INTERFACE) {
                continue;
            }

            const InterfaceNH *intf_nh =
                static_cast<const  InterfaceNH *>(nh);
            const Interface *intf =
                static_cast<const Interface *>(intf_nh->GetInterface());
            if (intf->type() != Interface::VM_INTERFACE) {
                //Ignore non vm interface nexthop
                continue;
            }
            if (dynamic_cast<const InetUnicastRouteEntry *>(route)) {
                const VmInterface *vm_intf =
                    static_cast<const VmInterface *>(intf);
                if (vm_intf->primary_ip6_addr().is_unspecified() == false) {
                    svc_ip_ = vm_intf->GetServiceIp(vm_intf->primary_ip6_addr());
                }
            }

            if (path->path_preference().IsDependentRt() == true) {
                continue;
            }

            uint32_t intf_id = intf->id();
            WaitForTrafficIntfMap::const_iterator wait_for_traffic_it =
                wait_for_traffic_map.find(intf_id);
            if (wait_for_traffic_it == wait_for_traffic_map.end()) {
                InterfaceIcmpv6PathPreferenceInfo data;
                new_wait_for_traffic_map.insert(std::make_pair(intf_id, data));
            } else {
                new_wait_for_traffic_map.insert(std::make_pair(intf_id,
                    wait_for_traffic_it->second));
            }
        }
    }


    if (dynamic_cast<const InetUnicastRouteEntry *>(route)) {
        l3_wait_for_traffic_map_ = new_wait_for_traffic_map;
    } else {
        evpn_wait_for_traffic_map_ = new_wait_for_traffic_map;
    }
    if (new_wait_for_traffic_map.size() > 0) {
        SendNeighborSolicit();
        StartTimer();
    }
}

Icmpv6Proto::Icmpv6Stats *Icmpv6Proto::VmiToIcmpv6Stats(VmInterface *i) {
    VmInterfaceMap::iterator it = vm_interfaces_.find(i);
    if (it == vm_interfaces_.end()) {
        return NULL;
    }
    return &it->second;
}

void Icmpv6Proto::IncrementStatsRouterSolicit(VmInterface *vmi) {
    stats_.icmpv6_router_solicit_++;
    Icmpv6Stats *stats = VmiToIcmpv6Stats(vmi);
    if (stats) {
        stats->icmpv6_router_solicit_++;
    }
}

void Icmpv6Proto::IncrementStatsRouterAdvert(VmInterface *vmi) {
    stats_.icmpv6_router_advert_++;
    Icmpv6Stats *stats = VmiToIcmpv6Stats(vmi);
    if (stats) {
        stats->icmpv6_router_advert_++;
    }
}

void Icmpv6Proto::IncrementStatsPingRequest(VmInterface *vmi) {
    stats_.icmpv6_ping_request_++;
    Icmpv6Stats *stats = VmiToIcmpv6Stats(vmi);
    if (stats) {
        stats->icmpv6_ping_request_++;
    }
}

void Icmpv6Proto::IncrementStatsPingResponse(VmInterface *vmi) {
    stats_.icmpv6_ping_response_++;
    Icmpv6Stats *stats = VmiToIcmpv6Stats(vmi);
    if (stats) {
        stats->icmpv6_ping_response_++;
    }
}

void Icmpv6Proto::IncrementStatsNeighborSolicit(VmInterface *vmi) {
    stats_.icmpv6_neighbor_solicit_++;
    Icmpv6Stats *stats = VmiToIcmpv6Stats(vmi);
    if (stats) {
        stats->icmpv6_neighbor_solicit_++;
    }
}

void Icmpv6Proto::IncrementStatsNeighborSolicited(VmInterface *vmi) {
    stats_.icmpv6_neighbor_solicited_++;
    Icmpv6Stats *stats = VmiToIcmpv6Stats(vmi);
    if (stats) {
        stats->icmpv6_neighbor_solicited_++;
    }
}

void Icmpv6Proto::IncrementStatsNeighborAdvertSolicited(VmInterface *vmi) {
    stats_.icmpv6_neighbor_advert_solicited_++;
    Icmpv6Stats *stats = VmiToIcmpv6Stats(vmi);
    if (stats) {
        stats->icmpv6_neighbor_advert_solicited_++;
    }
}

void Icmpv6Proto::IncrementStatsNeighborAdvertUnSolicited(VmInterface *vmi) {
    stats_.icmpv6_neighbor_advert_unsolicited_++;
    Icmpv6Stats *stats = VmiToIcmpv6Stats(vmi);
    if (stats) {
        stats->icmpv6_neighbor_advert_unsolicited_++;
    }
}

NdpEntry* Icmpv6Proto::FindUnsolNaEntry(NdpKey &key) {
    Icmpv6Proto::UnsolNaIterator iter = unsol_na_cache_.find(key);
    if (iter == unsol_na_cache_.end()) {
        return NULL;
    }
    return *iter->second.begin();
}

void Icmpv6Proto::AddUnsolNaEntry(NdpKey &key) {
     NdpEntrySet empty_set;
     unsol_na_cache_.insert(UnsolNaCachePair(key, empty_set));
}

void Icmpv6Proto::DeleteUnsolNaEntry(NdpEntry *entry) {
    if (!entry)
        return ;

    Icmpv6Proto::UnsolNaIterator iter = unsol_na_cache_.find(entry->key());
    if (iter == unsol_na_cache_.end()) {
        return;
    }

    iter->second.erase(entry);
    delete entry;
    if (iter->second.empty()) {
        unsol_na_cache_.erase(iter);
    }
}

NdpEntry *
Icmpv6Proto::UnsolNaEntry(const NdpKey &key, const Interface *intf) {
    Icmpv6Proto::UnsolNaIterator it = unsol_na_cache_.find(key);
    if (it == unsol_na_cache_.end())
        return NULL;

    for (NdpEntrySet::iterator sit = it->second.begin();
         sit != it->second.end(); sit++) {
        NdpEntry *entry = *sit;
        if (entry->get_interface() == intf)
            return *sit;
    }

    return NULL;
}

Icmpv6Proto::UnsolNaIterator
Icmpv6Proto::UnsolNaEntryIterator(const NdpKey &key, bool *key_valid) {
    Icmpv6Proto::UnsolNaIterator it = unsol_na_cache_.find(key);
    if (it == unsol_na_cache_.end())
        return it;
    const VrfEntry *vrf = key.vrf;
    if (!vrf)
        return it;
    const Icmpv6VrfState *state = static_cast<const Icmpv6VrfState *>
                         (vrf->GetState(vrf->get_table_partition()->parent(),
                          vrf_table_listener_id_));
    // If VRF is delete marked, do not add Ndp entries to cache
    if (state == NULL || state->deleted() == true)
        return it;
    *key_valid = true;
    return it;
}

bool Icmpv6Proto::AddNdpEntry(NdpEntry *entry) {
    const VrfEntry *vrf = entry->key().vrf;
    const Icmpv6VrfState *state = static_cast<const Icmpv6VrfState *>
                         (vrf->GetState(vrf->get_table_partition()->parent(),
                          vrf_table_listener_id_));
    // If VRF is delete marked, do not add Ndp entries to cache
    if (state == NULL || state->deleted() == true)
        return false;

    bool ret = ndp_cache_.insert(NdpCachePair(entry->key(), entry)).second;
    uint32_t intf_id = entry->get_interface()->id();
    InterfaceNdpMap::iterator it = interface_ndp_map_.find(intf_id);
    if (it == interface_ndp_map_.end()) {
        InterfaceNdpInfo intf_entry;
        intf_entry.ndp_key_list.insert(entry->key());
        interface_ndp_map_.insert(InterfaceNdpPair(intf_id, intf_entry));
    } else {
        InterfaceNdpInfo &intf_entry = it->second;
        NdpKeySet::iterator key_it = intf_entry.ndp_key_list.find(entry->key());
        if (key_it == intf_entry.ndp_key_list.end()) {
            intf_entry.ndp_key_list.insert(entry->key());
        }
    }
    return ret;
}

bool Icmpv6Proto::DeleteNdpEntry(NdpEntry *entry) {
    if (!entry)
        return false;

    Icmpv6Proto::NdpIterator iter = ndp_cache_.find(entry->key());
    if (iter == ndp_cache_.end()) {
        return false;
    }

    DeleteNdpEntry(iter);
    return true;
}

Icmpv6Proto::NdpIterator
Icmpv6Proto::DeleteNdpEntry(Icmpv6Proto::NdpIterator iter) {
    NdpEntry *entry = iter->second;
    ndp_cache_.erase(iter++);
    delete entry;
    return iter;
}

NdpEntry *Icmpv6Proto::FindNdpEntry(const NdpKey &key) {
    NdpIterator it = ndp_cache_.find(key);
    if (it == ndp_cache_.end())
        return NULL;
    return it->second;
}
