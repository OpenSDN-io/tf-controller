/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_icmpv6_handler_h_
#define vnsw_agent_icmpv6_handler_h_

#include "pkt/proto_handler.h"

#define IPV6_ADDR_SIZE_BYTES  16
#define IPV6_ICMP_NEXT_HEADER 58

struct NdpKey;
class NdpEntry;

// ICMPv6 protocol handler
class Icmpv6Handler : public ProtoHandler {
public:
    static const Ip6Address::bytes_type kPrefix;
    static const Ip6Address::bytes_type kSuffix;
    static const Ip6Address kSolicitedNodeIpPrefix;
    static const Ip6Address kSolicitedNodeIpSuffixMask;
    static const uint8_t kIPv6AddrUnspecifiedBytes[IPV6_ADDR_SIZE_BYTES];
    Icmpv6Handler(Agent *agent, boost::shared_ptr<PktInfo> info,
                  boost::asio::io_context &io);
    virtual ~Icmpv6Handler();

    bool Run();
    bool RouterAdvertisement(Icmpv6Proto *proto);
    void SendNeighborAdvert(const Ip6Address &sip, const Ip6Address &dip,
                            const MacAddress &smac, const MacAddress &dmac,
                            uint32_t itf, uint32_t vrf, bool solicited);
    void SendNeighborSolicit(const Ip6Address &sip, const Ip6Address &dip,
                             const VmInterface *vmi, uint32_t vrf,
                             bool send_unicast=false);
    friend void intrusive_ptr_add_ref(const Icmpv6Handler *p);
    friend void intrusive_ptr_release(const Icmpv6Handler *p);

private:
    bool CheckPacket();
    bool HandlePacket();
    bool HandleMessage();
    void EntryDelete(NdpKey &key);
    uint16_t FillRouterAdvertisement(uint8_t *buf, uint32_t ifindex,
                                     uint8_t *src, uint8_t *dest,
                                     const Ip6Address &prefix, uint8_t plen);
    uint16_t FillNeighborAdvertisement(uint8_t *buf, uint8_t *dip,
                                       uint8_t *sip, const Ip6Address &target,
                                       const MacAddress &dmac, bool solicited);
    void SendRAResponse(uint32_t ifindex, uint32_t vrfindex,
                        uint8_t *src_ip, uint8_t *dest_ip,
                        const MacAddress &dest_mac,
                        const Ip6Address &prefix, uint8_t plen);
    void SendPingResponse();
    void SendIcmpv6Response(uint32_t ifindex, uint32_t vrfindex,
                            uint8_t *src_ip, uint8_t *dest_ip,
                            const MacAddress &dest_mac, uint16_t len);
    void SolicitedMulticastIpAndMac(const Ip6Address &dip, uint8_t *ip,
                                    MacAddress &mac);
    uint16_t FillNeighborSolicit(uint8_t *buf, const Ip6Address &target,
                                 uint8_t *sip, uint8_t *dip);
    void Ipv6Lower24BitsExtract(uint8_t *dst, uint8_t *src);
    void Ipv6AddressBitwiseOr(uint8_t *dst, uint8_t *src);
    bool IsDefaultGatewayConfigured(uint32_t ifindex, const Ip6Address &addr);
    bool IsIPv6AddrUnspecifiedBytes(const uint8_t *ip);

    icmp6_hdr *icmp_;
    uint16_t icmp_len_;
    mutable tbb::atomic<uint32_t> refcount_;
    DISALLOW_COPY_AND_ASSIGN(Icmpv6Handler);
};

#endif // vnsw_agent_icmpv6_handler_h_
