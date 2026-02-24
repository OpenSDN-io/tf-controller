/*
 * Copyright (c) 2018 Juniper Networks, Inc. All rights reserved.
 * Copyright (c) 2022 - 2026 Matvey Kraposhin.
 * Copyright (c) 2024 - 2026 Elena Zizganova.
 */

#include <oper/bgp_as_service.h>

template <class ItType>
std::vector<IpAddress> VxlanRoutingManager::ItemNexthopsToVector(ItType *item) {
    std::vector<IpAddress> nh_addr;
    const uint32_t n_items = item->entry.next_hops.next_hop.size();
    for (uint32_t i_nh=0; i_nh < n_items; i_nh++) {
        const IpAddress nh_ip = IpAddress::from_string(
            item->entry.next_hops.next_hop[i_nh].address);
        nh_addr.insert(nh_addr.end(), nh_ip);
    }

    return nh_addr;
}

// A nexthop is counted as BGPaaS when it has MPLS label and this
// label points to the VmInterface linked with the BGPaaS object
static bool IsBgpaasInterfaceNexthop(const Agent* agent, const NextHop* nh) {
    if (nh->GetType() != NextHop::INTERFACE ||
        nh->mpls_label() == NULL) {
        return false;
    }

    const InterfaceNH *intf_nh = static_cast<const InterfaceNH *>(nh);
    const Interface *interface = intf_nh->GetInterface();
    if (interface->type() != Interface::VM_INTERFACE)
        return false;

    const VmInterface *vm_intf =
        dynamic_cast<const VmInterface*>(interface);
    const Ip4Address& intf_ip4 = vm_intf->primary_ip_addr();
    const Ip6Address& intf_ip6 = vm_intf->primary_ip6_addr();

    if (agent->oper_db()->bgp_as_a_service()->
        IsBgpService(vm_intf, IpAddress(intf_ip4), IpAddress(intf_ip4))) {
        return true;
    }
    if (agent->oper_db()->bgp_as_a_service()->
        IsBgpService(vm_intf, IpAddress(intf_ip6), IpAddress(intf_ip6))) {
        return true;
    }
    return false;
}

// A composite nexthop is counted as BGPaaS when at least one component is
// an interface
static bool IsBgpaasCompositeNexthop(const Agent* agent, const NextHop* nh) {
    if (nh->GetType() != NextHop::COMPOSITE)
    return false;
    const CompositeNH *composite_nh = dynamic_cast<const CompositeNH*>(nh);
    uint32_t n_comps = composite_nh->ComponentNHCount();
    for (uint32_t i=0; i < n_comps; i++) {
        const NextHop *c_nh = composite_nh->GetNH(i);
        if (c_nh != nullptr &&
            IsBgpaasInterfaceNexthop(agent, c_nh) == true) {
            return true;
        }
    }
    return false;
}

template<typename NhType>
void VxlanRoutingManager::AddBgpaasInterfaceComponentToList(
    const std::string& vrf_name,
    const NhType &nh_item,
    ComponentNHKeyList& comp_nh_list,
    std::vector<std::string> &peer_sources) {

    for (const auto &nexthop_addr : peer_sources) {
        const Agent *agent = Agent::GetInstance();
        IpAddress nh_ip;
        uint32_t prefix_len;
        boost::system::error_code ec;

        if (is_ipv4_string(nexthop_addr)) {
            nh_ip = Ip4Address::from_string(ipv4_prefix(nexthop_addr), ec);
            prefix_len = ipv4_prefix_len(nexthop_addr);
        } else if (is_ipv6_string(nexthop_addr)) {
            nh_ip = Ip6Address::from_string(ipv6_prefix(nexthop_addr), ec);
            prefix_len = ipv6_prefix_len(nexthop_addr);
        } else {
            LOG(ERROR, "Error in VxlanRoutingManager"
                << "::AddBgpaasInterfaceComponentToList"
                << ", nexthop_addr = " << nexthop_addr
                << " is not an IPv4 or IPv6 prefix");
            return;
        }
        if (ec) {
            continue;
        }
        const AgentRoute *intf_rt =
            FindEvpnOrInetRoute(agent, vrf_name, nh_ip, prefix_len, nh_item);
        const AgentPath *loc_path =
            intf_rt->FindIntfOrCompLocalVmPortPath();
        if (loc_path == nullptr) {
            continue;
        }
        if (loc_path->nexthop() == nullptr) {
            continue;
        }
        if (IsBgpaasInterfaceNexthop(agent, loc_path->nexthop())) {
            DBEntryBase::KeyPtr key_ptr =
                loc_path->nexthop()->GetDBRequestKey();
            NextHopKey *nh_key =
                static_cast<NextHopKey *>(key_ptr.release());
            std::unique_ptr<const NextHopKey> nh_key_ptr(nh_key);
            ComponentNHKeyPtr component_nh_key(new
                                ComponentNHKey(MplsTable::kInvalidLabel,
                                               std::move(nh_key_ptr)));
            comp_nh_list.push_back(component_nh_key);
        } else if (IsBgpaasCompositeNexthop(agent, loc_path->nexthop())) {
            CompositeNH *loc_comp_nh = dynamic_cast<CompositeNH*>
                (loc_path->nexthop());

            DBEntryBase::KeyPtr key_ptr =
                loc_comp_nh->GetDBRequestKey();
            CompositeNHKey *nh_key =
                static_cast<CompositeNHKey *>(key_ptr.release());
            std::unique_ptr<const NextHopKey> nh_key_ptr(nh_key);

            if (nh_key == nullptr){
                LOG(ERROR, "Error in VxlanRoutingManager::"
                << "::AddBgpaasInterfaceComponentToList"
                << ", null nh key");
            }

            const ComponentNHList& component_nh_list =
                loc_comp_nh->component_nh_list();
            for (auto &component_nh : component_nh_list) {
                std::unique_ptr<const NextHopKey> nh_key_ptr;
                ComponentNHKeyPtr component_nh_key;
                if (component_nh.get() == nullptr) {
                    // component_nh_key.reset(NULL);
                } else {
                    DBEntryBase::KeyPtr key =
                        component_nh.get()->nh()->GetDBRequestKey();
                    NextHopKey *nh_key =
                        static_cast<NextHopKey *>(key.release());
                    nh_key_ptr.reset(nh_key);
                    component_nh_key.reset(
                        new ComponentNHKey(MplsTable::kInvalidLabel,
                                           std::move(nh_key_ptr)));
                }
                comp_nh_list.push_back(component_nh_key);
            }
        }
    }
}

template<typename NhType>
void VxlanRoutingManager::AddInterfaceComponentToList(
    const std::string& prefix_str,
    const std::string& vrf_name,
    const NhType &nh_item,
    ComponentNHKeyList& comp_nh_list,
    std::vector<std::string> &peer_sources) {
    const Agent *agent = Agent::GetInstance();
    IpAddress ip_addr;
    uint32_t prefix_len;
    boost::system::error_code ec;

    if (is_ipv4_string(prefix_str)) {
        ip_addr = Ip4Address::from_string(ipv4_prefix(prefix_str), ec);
        prefix_len = ipv4_prefix_len(prefix_str);
    } else if (is_ipv6_string(prefix_str)) {
        std::string addr_str = ipv6_prefix(prefix_str);
        prefix_len = ipv6_prefix_len(prefix_str);
        ip_addr = Ip6Address::from_string(addr_str, ec);
    } else {
        LOG(ERROR, "Error in VxlanRoutingManager::AddInterfaceComponentToList"
            << ", prefix_str = " << prefix_str
            << " is not an IPv4 or IPv6 prefix");
        return;
    }

    if (ec) {
        LOG(ERROR, "Possible error in "
            << "VxlanRoutingManager::AddInterfaceComponentToList"
            << ", cannot convert prefix_str = " << prefix_str
            << " to IPv4 or IPv6 address");
        return;
    }

    const AgentRoute *intf_rt =
        FindEvpnOrInetRoute(agent, vrf_name, ip_addr, prefix_len, nh_item);
    if (intf_rt == NULL) {
        AddBgpaasInterfaceComponentToList(vrf_name, nh_item, comp_nh_list,
                                          peer_sources);
        return;
    }

    const AgentPath *loc_path =
        intf_rt->FindIntfOrCompLocalVmPortPath();
    if (loc_path == NULL) {
        AddBgpaasInterfaceComponentToList(vrf_name, nh_item, comp_nh_list,
                                          peer_sources);
        return;
    }
    if (loc_path->nexthop() == NULL) {
        AddBgpaasInterfaceComponentToList(vrf_name, nh_item, comp_nh_list,
                                          peer_sources);
        return;
    }

    // Case 1. NextHop is an interface
    if (loc_path->nexthop()->GetType() == NextHop::INTERFACE) {
        DBEntryBase::KeyPtr key_ptr =
            loc_path->nexthop()->GetDBRequestKey();
        NextHopKey *nh_key =
            static_cast<NextHopKey *>(key_ptr.release());
        std::unique_ptr<const NextHopKey> nh_key_ptr(nh_key);
        ComponentNHKeyPtr component_nh_key(new ComponentNHKey(MplsTable::kInvalidLabel,  // label
                                        std::move(nh_key_ptr)));
        comp_nh_list.push_back(component_nh_key);
        return;
    }

    // Case 2. NextHop is a composite of interfaces
    // Copy all interfaces from this composite
    // into the components list
    if (loc_path->nexthop()->GetType() == NextHop::COMPOSITE) {
        CompositeNH *loc_comp_nh = dynamic_cast<CompositeNH*>
            (loc_path->nexthop());

        DBEntryBase::KeyPtr key_ptr =
            loc_comp_nh->GetDBRequestKey();
        CompositeNHKey *nh_key =
            static_cast<CompositeNHKey *>(key_ptr.release());
        std::unique_ptr<const NextHopKey> nh_key_ptr(nh_key);

        if (nh_key == NULL){
            LOG(ERROR, "Error in VxlanRoutingManager::AddInterfaceComponentToList"
            << ", null nh key");
            assert(nh_key != NULL);
        }

        // Refresh on path_preference.sequence change
        const ComponentNHList& component_nh_list =
            loc_comp_nh->component_nh_list();
        for (ComponentNHList::const_iterator
            it_nh = component_nh_list.begin();
            it_nh != component_nh_list.end(); it_nh++) {
            // nullptr means deleted component, which
            // can be reused later
            std::unique_ptr<const NextHopKey> nh_key_ptr;
            ComponentNHKeyPtr component_nh_key;
            if (it_nh->get() == NULL) {
                // component_nh_key.reset(NULL);
            } else {
                DBEntryBase::KeyPtr key =
                    it_nh->get()->nh()->GetDBRequestKey();
                NextHopKey *nh_key =
                    static_cast<NextHopKey *>(key.release());
                nh_key_ptr.reset(nh_key);
                component_nh_key.reset(
                    new ComponentNHKey(MplsTable::kInvalidLabel, std::move(nh_key_ptr)));
            }
            comp_nh_list.push_back(component_nh_key);
        }
    }
}  // AddInterfaceComponentToList func

//
// END-OF-FILE
//
