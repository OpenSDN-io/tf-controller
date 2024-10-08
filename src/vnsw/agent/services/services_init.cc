/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <oper/mirror_table.h>
#include <io/event_manager.h>
#include <cmn/agent_cmn.h>
#include "sandesh/sandesh_types.h"
#include "sandesh/sandesh.h"
#include "sandesh/sandesh_trace.h"
#include "services/services_init.h"
#include "services/dhcp_proto.h"
#include "services/dhcpv6_proto.h"
#include "services/dns_proto.h"
#include "services/arp_proto.h"
#include "services/icmp_proto.h"
#include "services/icmpv6_proto.h"
#include "services/bfd_proto.h"
#include "services/igmp_proto.h"
#include "services/metadata_proxy.h"
#include "init/agent_param.h"


SandeshTraceBufferPtr DhcpTraceBuf(SandeshTraceBufferCreate("Dhcp", 1000));
SandeshTraceBufferPtr Dhcpv6TraceBuf(SandeshTraceBufferCreate("Dhcpv6", 1000));
SandeshTraceBufferPtr Icmpv6TraceBuf(SandeshTraceBufferCreate("Icmpv6", 500));
SandeshTraceBufferPtr ArpTraceBuf(SandeshTraceBufferCreate("Arp", 1000));
SandeshTraceBufferPtr MetadataTraceBuf(SandeshTraceBufferCreate("Metadata", 500));
SandeshTraceBufferPtr BfdTraceBuf(SandeshTraceBufferCreate("Bfd", 500));
SandeshTraceBufferPtr IgmpTraceBuf(SandeshTraceBufferCreate("Igmp", 500));

ServicesModule::ServicesModule(Agent *agent, const std::string &metadata_secret)
    : agent_(agent), metadata_secret_key_(metadata_secret), dhcp_proto_(NULL),
      dhcpv6_proto_(NULL), dns_proto_(NULL), arp_proto_(NULL), bfd_proto_(NULL),
      icmp_proto_(NULL), icmpv6_proto_(NULL), igmp_proto_(NULL), metadata_proxy_(NULL) {
}

ServicesModule::~ServicesModule() {
}

void ServicesModule::Init(bool run_with_vrouter) {
    EventManager *event = agent_->event_manager();
    boost::asio::io_context &io = *event->io_service();

    dhcp_proto_.reset(new DhcpProto(agent_, io, run_with_vrouter));
    agent_->SetDhcpProto(dhcp_proto_.get());

    dhcpv6_proto_.reset(new Dhcpv6Proto(agent_, io, run_with_vrouter));
    agent_->set_dhcpv6_proto(dhcpv6_proto_.get());

    dns_proto_.reset(new DnsProto(agent_, io));
    agent_->SetDnsProto(dns_proto_.get());

    arp_proto_.reset(new ArpProto(agent_, io, run_with_vrouter));
    agent_->SetArpProto(arp_proto_.get());

    bfd_proto_.reset(new BfdProto(agent_, io));
    agent_->SetBfdProto(bfd_proto_.get());

    icmp_proto_.reset(new IcmpProto(agent_, io));
    agent_->SetIcmpProto(icmp_proto_.get());

    icmpv6_proto_.reset(new Icmpv6Proto(agent_, io));
    agent_->set_icmpv6_proto(icmpv6_proto_.get());

    icmp_error_proto_.reset(new IcmpErrorProto(agent_, io));
    icmpv6_error_proto_.reset(new Icmpv6ErrorProto(agent_, io));

    igmp_proto_.reset(new IgmpProto(agent_, io));
    agent_->SetIgmpProto(igmp_proto_.get());

    metadata_proxy_.reset(new MetadataProxy(this, metadata_secret_key_));
    ReserveLocalPorts();
}

void ServicesModule::ConfigInit() {
    dns_proto_->ConfigInit();
}

void ServicesModule::IoShutdown() {
    dns_proto_->IoShutdown();
    metadata_proxy_->CloseSessions();
}

void ServicesModule::Shutdown() {
    dhcp_proto_->Shutdown();
    dhcp_proto_.reset(NULL);
    agent_->SetDhcpProto(NULL);

    dhcpv6_proto_->Shutdown();
    dhcpv6_proto_.reset(NULL);
    agent_->set_dhcpv6_proto(NULL);

    dns_proto_->Shutdown();
    dns_proto_.reset(NULL);
    agent_->SetDnsProto(NULL);

    arp_proto_->Shutdown();
    arp_proto_.reset(NULL);
    agent_->SetArpProto(NULL);

    bfd_proto_->Shutdown();
    bfd_proto_.reset(NULL);
    agent_->SetBfdProto(NULL);

    icmp_proto_->Shutdown();
    icmp_proto_.reset(NULL);
    agent_->SetIcmpProto(NULL);

    icmpv6_proto_->Shutdown();
    icmpv6_proto_.reset(NULL);
    agent_->set_icmpv6_proto(NULL);

    igmp_proto_->Shutdown();
    igmp_proto_.reset(NULL);
    agent_->SetIgmpProto(NULL);

    metadata_proxy_->Shutdown();
    metadata_proxy_.reset(NULL);
}

bool ServicesModule::AllocateFd(uint16_t port_number, uint8_t l3_proto) {
    int fd;

    // l3 proto can be TCP or UDP
    if (l3_proto == IPPROTO_TCP) {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    } else {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }

    if (fd < 0) {
        LOG(ERROR, "Failed to create socket, errno:" << strerror(errno));
        return false;
    }
    
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port_number);
    if (::bind(fd, (struct sockaddr*) &address, sizeof(address)) < 0) {
        LOG(ERROR, "failed to bind socket to port: " << port_number
                << "errno: " << strerror(errno));
        close(fd);
        return false;
    }

    reserved_port_fd_list_.push_back(fd);

    return true;
}
void ServicesModule::ReserveLocalPorts() {

    //reserve UDP dest ports used for tunneling protocos
    //so that these port numbers are not used for link local services.
    AllocateFd(MPLS_OVER_UDP_OLD_DEST_PORT, IPPROTO_UDP);
    AllocateFd(MPLS_OVER_UDP_NEW_DEST_PORT, IPPROTO_UDP);
    AllocateFd(VXLAN_UDP_DEST_PORT, IPPROTO_UDP);
}

void ServicesModule::FreeLocalPortBindings() {
    std::vector<int>::const_iterator it = reserved_port_fd_list_.begin();
    while ( it != reserved_port_fd_list_.end()) {
        // close socket
        close(*it);
        it++;
    }
}
