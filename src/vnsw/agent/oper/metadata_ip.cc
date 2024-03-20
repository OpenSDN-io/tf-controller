/*
 * Copyright (c) 2016 Juniper Networks, Inc. All rights reserved.
 */

#include <oper/route_common.h>
#include <oper/ecmp_load_balance.h>
#include <oper/interface_common.h>
#include <oper/vm_interface.h>
#include <oper/inet_unicast_route.h>
#include <oper/metadata_ip.h>

const IpAddress MetaDataIp::kDefaultIp;

MetaDataIp::MetaDataIp(MetaDataIpAllocator *allocator, VmInterface *intf,
                       MetaDataIpType type, bool insert_metadata_ip, bool ipv4) :
    allocator_(allocator), index_(-1), intf_(intf),
    intf_label_(MplsTable::kInvalidLabel), service_ip_(), destination_ip_(),
    active_(false), type_(type), insert_metadata_ip_(insert_metadata_ip),
    ipv4_(ipv4) {
    if (insert_metadata_ip_) {
        index_ = allocator_->AllocateIndex(this);
        intf->InsertMetaDataIpInfo(this);
    }
}

MetaDataIp::MetaDataIp(MetaDataIpAllocator *allocator, VmInterface *intf,
                       uint16_t index, bool ipv4) :
    allocator_(allocator), index_(index), intf_(intf),
    intf_label_(MplsTable::kInvalidLabel), service_ip_(), destination_ip_(),
    active_(false), type_(LINKLOCAL), ipv4_(ipv4) {
    allocator_->AllocateIndex(this, index_);
    intf_->InsertMetaDataIpInfo(this);
}

MetaDataIp::~MetaDataIp() {
    if (type_ == HEALTH_CHECK) {
        if (insert_metadata_ip_) {
            intf_->DeleteMetaDataIpInfo(this);
            allocator_->ReleaseIndex(this);
        }
    } else {
        intf_->DeleteMetaDataIpInfo(this);
        allocator_->ReleaseIndex(this);
    }
    set_active(false);
}

template<>
Ip4Address MetaDataIp::IndexToIpAddress<Ip4Address>(uint32_t idx) {
    uint32_t ip = METADATA_IP_ADDR & 0xFFFF0000;
    ip += (((uint32_t)idx) & 0xFFFF);
    return Ip4Address(ip);
}

template<>
Ip6Address MetaDataIp::IndexToIpAddress<Ip6Address>(uint32_t idx) {
    Ip6Address::bytes_type ip_bytes =
        {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        (uint32_t)idx & 0xFF00,
        (uint32_t)idx & 0x00FF};
    return Ip6Address(ip_bytes);
}

template<>
uint32_t MetaDataIp::IpAddressToIndex<Ip4Address>(const Ip4Address& ip) {
    uint32_t idx = ip.to_ulong();
    if ((idx & 0xFFFF0000) != (METADATA_IP_ADDR & 0xFFFF0000))
        return (0xFFFF + 1);
    idx &= 0xFFFF;
    return idx;
}

template<>
uint32_t MetaDataIp::IpAddressToIndex<Ip6Address>(const Ip6Address& ip) {
    uint32_t idx = ip.to_bytes()[15] + (ip.to_bytes()[14] << 8);
    return idx;
}

IpAddress MetaDataIp::GetLinkLocalIp() const {
    if (ipv4_) {
        return GetLinkLocalIp4();
    }
    return GetLinkLocalIp6();
}

Ip4Address MetaDataIp::GetLinkLocalIp4() const {
    return IndexToIpAddress<Ip4Address>(index_);
}

Ip6Address MetaDataIp::GetLinkLocalIp6() const {
    return IndexToIpAddress<Ip6Address>(index_);
}

IpAddress MetaDataIp::service_ip() const {
    // check if explicit configuration of service ip is present for
    // this metadata ip
    if (service_ip_ == kDefaultIp) {
        IpAddress service_ip;
        if (type_ == HEALTH_CHECK) {
            // for metadata IP type Health Check first verify
            // if service health check ip is available on interface
            service_ip =  intf_->service_health_check_ip();
            if (service_ip != kDefaultIp) {
                return service_ip;
            }
        }
        // check if service ip on the primary ip addr of interface
        // is available
        service_ip = intf_->GetServiceIp(intf_->primary_ip_addr());
        if (service_ip != kDefaultIp) {
            return service_ip;
        }
        // if service IP is not available fallback to MetaData IP
        return Ip4Address(METADATA_IP_ADDR);
    }
    return service_ip_;
}

IpAddress MetaDataIp::destination_ip() const {
    if (destination_ip_.to_v4() == kDefaultIp) {
        return intf_->primary_ip_addr();
    }
    return destination_ip_;
}

void MetaDataIp::set_destination_ip(const IpAddress &dst_ip) {
    destination_ip_ = dst_ip;
}

void MetaDataIp::set_active(bool active) {
    if (active_ == active ||
        (active & !allocator_->CanAddRoute(this))) {
        return;
    }
    active_ = active;
    UpdateRoute();
}

void MetaDataIp::UpdateInterfaceCb() {
    if (intf_label_ != intf_->label()) {
        intf_label_ = intf_->label();
        UpdateRoute();
    }
}

const Interface *MetaDataIp::GetInterface() const {
    return intf_;
}

void MetaDataIp::UpdateRoute() {
    if (active_ && intf_->label() != MplsTable::kInvalidLabel) {
        intf_label_ = intf_->label();
        allocator_->AddFabricRoute(this);
    } else {
        allocator_->DelFabricRoute(this);
        intf_label_ = MplsTable::kInvalidLabel;
    }
}

MetaDataIpAllocator::MetaDataIpAllocator(Agent *agent, uint16_t start,
                                         uint16_t end) :
    agent_(agent), index_table_(), start_(start), end_(end) {
}

MetaDataIpAllocator::~MetaDataIpAllocator() {
}

MetaDataIp *MetaDataIpAllocator::FindIndex(uint16_t id) {
    uint16_t index = end_ - id;
    return index_table_.At(index);
}

uint16_t MetaDataIpAllocator::AllocateIndex(MetaDataIp *ip) {
    uint16_t index = index_table_.Insert(ip);
    assert(index <= end_ && (end_ - index) >= start_);
    return (end_ - index);
}

void MetaDataIpAllocator::AllocateIndex(MetaDataIp *ip, uint16_t id) {
    assert(id <= end_ && id >= start_);
    uint16_t index = end_ - id;
    assert(index == index_table_.InsertAtIndex(index, ip));
}

void MetaDataIpAllocator::ReleaseIndex(MetaDataIp *ip) {
    uint16_t index = end_ - ip->index_;
    index_table_.Remove(index);
}

bool MetaDataIpAllocator::CanAddRoute(MetaDataIp* ip) {
    if (ip->intf_->vmi_type() == VmInterface::VHOST ||
        ip->intf_->vn() == NULL || ip->intf_->vrf() == NULL) {
        return false;
    }

    return true;
}

void MetaDataIpAllocator::AddFabricRoute(MetaDataIp *ip) {
    if (!CanAddRoute(ip)) {
        return;
    }
    PathPreference path_preference;
    EcmpLoadBalance ecmp_load_balance;
    ip->intf_->SetPathPreference(&path_preference, false, Ip4Address(0));

    VnListType vn_list;
    vn_list.insert(ip->intf_->vn()->GetName());

    if (ip->ipv4_) {
        InetUnicastAgentRouteTable *table =
            static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable(
                                            agent_->fabric_vrf_name()));

        table->AddLocalVmRouteReq(
            agent_->link_local_peer(), agent_->fabric_vrf_name(),
            ip->GetLinkLocalIp(), 32, ip->intf_->GetUuid(),
            vn_list, ip->intf_->label(), SecurityGroupList(),
            TagList(), CommunityList(), true, path_preference,
            Ip4Address(0), ecmp_load_balance, false, false, false,
            ip->intf_->name());
    } else {
        InetUnicastAgentRouteTable *table =
            static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet6UnicastRouteTable(
                                            agent_->fabric_vrf_name()));

        table->AddLocalVmRouteReq(
            agent_->link_local_peer(), agent_->fabric_vrf_name(),
            ip->GetLinkLocalIp(), 128, ip->intf_->GetUuid(),
            vn_list, ip->intf_->label(), SecurityGroupList(),
            TagList(), CommunityList(), true, path_preference,
            Ip6Address(), ecmp_load_balance, false, false, false,
            ip->intf_->name());
    }
}

void MetaDataIpAllocator::DelFabricRoute(MetaDataIp *ip) {
    if (ip->ipv4_) {
        InetUnicastAgentRouteTable::Delete(agent_->link_local_peer(),
            agent_->fabric_vrf_name(),
            ip->GetLinkLocalIp(), 32);
    } else {
        InetUnicastAgentRouteTable::Delete(agent_->link_local_peer(),
            agent_->fabric_vrf_name(),
            ip->GetLinkLocalIp(), 128);
    }
}

