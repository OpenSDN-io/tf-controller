/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "base/os.h"
#include "testing/gunit.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <boost/scoped_array.hpp>
#include <base/logging.h>

#include <io/event_manager.h>
#include <cmn/agent_cmn.h>
#include <oper/operdb_init.h>
#include <controller/controller_init.h>
#include <controller/controller_vrf_export.h>
#include <pkt/pkt_init.h>
#include <services/services_init.h>
#include <vrouter/ksync/ksync_init.h>
#include <oper/vrf.h>
#include <pugixml/pugixml.hpp>
#include <services/icmp_proto.h>
#include <vr_interface.h>
#include <test/test_cmn_util.h>
#include <test/pkt_gen.h>
#include <services/services_sandesh.h>
#include "vr_types.h"

#define MAX_WAIT_COUNT 60
#define BUF_SIZE 8192
char src_mac[ETHER_ADDR_LEN] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
char dest_mac[ETHER_ADDR_LEN] = { 0x00, 0x11, 0x12, 0x13, 0x14, 0x15 };

class IcmpTest : public ::testing::Test {
public:
    enum IcmpError {
        NO_ERROR,
        TYPE_ERROR,
        CHECKSUM_ERROR
    };

    IcmpTest() : itf_count_(0), icmp_seq_(0) {
        rid_ = Agent::GetInstance()->interface_table()->Register(
                boost::bind(&IcmpTest::ItfUpdate, this, _2));
    }

    ~IcmpTest() {
        Agent::GetInstance()->interface_table()->Unregister(rid_);
    }

    void ItfUpdate(DBEntryBase *entry) {
        Interface *itf = static_cast<Interface *>(entry);
        tbb::mutex::scoped_lock lock(mutex_);
        unsigned int i;
        for (i = 0; i < itf_id_.size(); ++i)
            if (itf_id_[i] == itf->id())
                break;
        if (entry->IsDeleted()) {
            if (itf_count_ && i < itf_id_.size()) {
                itf_count_--;
                LOG(DEBUG, "Icmp test : interface deleted " << itf_id_[0]);
                itf_id_.erase(itf_id_.begin()); // we delete in create order
            }
        } else {
            if (i == itf_id_.size()) {
                itf_count_++;
                itf_id_.push_back(itf->id());
                LOG(DEBUG, "Icmp test : interface added " << itf->id());
            }
        }
    }

    uint32_t GetItfCount() {
        tbb::mutex::scoped_lock lock(mutex_);
        return itf_count_;
    }

    std::size_t GetItfId(int index) {
        tbb::mutex::scoped_lock lock(mutex_);
        return itf_id_[index];
    }

    void CheckSandeshResponse(Sandesh *sandesh, int count) {
        if (memcmp(sandesh->Name(), "IcmpStats", strlen("IcmpStats")) == 0) {
            IcmpStats *icmp = (IcmpStats *)sandesh;
            EXPECT_EQ(icmp->get_icmp_gw_ping(), count);
        } else if (memcmp(sandesh->Name(), "IcmpPktSandesh",
                          strlen("IcmpPktSandesh")) == 0) {
            IcmpPktSandesh *icmp = (IcmpPktSandesh *)sandesh;
            EXPECT_EQ(icmp->get_pkt_list().size(),
                      std::min((int)PktTrace::kPktNumBuffers, 2 * count));
        }
    }

    void SendIcmp(short ifindex, uint32_t dest_ip, IcmpError error = NO_ERROR, bool layer3fwd = true) {
        int len = 512;
        uint8_t *buf = new uint8_t[len];
        memset(buf, 0, len);

        struct ether_header *eth = (struct ether_header *)buf;
        eth->ether_dhost[5] = 1;
        eth->ether_shost[5] = 2;
        eth->ether_type = htons(0x800);

        agent_hdr *agent = (agent_hdr *)(eth + 1);
        agent->hdr_ifindex = htons(ifindex);
        if(layer3fwd != true) {
            Interface *itf = Agent::GetInstance()->interface_table()->FindInterface(ifindex);
             VmInterface *vm_itf = static_cast<VmInterface *>(itf);
             vm_itf->set_layer3_forwarding(false);
        }
        agent->hdr_vrf = htons(0);
        agent->hdr_cmd = htons(AgentHdr::TRAP_NEXTHOP);

        eth = (struct ether_header *) (agent + 1);
        memcpy(eth->ether_dhost, dest_mac, ETHER_ADDR_LEN);
        memcpy(eth->ether_shost, src_mac, ETHER_ADDR_LEN);
        eth->ether_type = htons(0x800);

        struct ip *ip = (struct ip *) (eth + 1);
        ip->ip_hl = 5;
        ip->ip_v = 4;
        ip->ip_tos = 0;
        ip->ip_id = 0;
        ip->ip_off = 0;
        ip->ip_ttl = 16;
        ip->ip_p = IPPROTO_ICMP;
        ip->ip_sum = 0;
        ip->ip_src.s_addr = 0;
        ip->ip_dst.s_addr = htonl(dest_ip);

        struct icmp *icmp = (struct icmp *) (ip + 1);
        if (error == TYPE_ERROR)
            icmp->icmp_type = ICMP_ECHOREPLY;
        else
            icmp->icmp_type = ICMP_ECHO;
        icmp->icmp_code = 0;
        icmp->icmp_cksum = 0;
        icmp->icmp_id = 0x1234;
        icmp->icmp_seq = icmp_seq_++;
        if (error == CHECKSUM_ERROR)
            icmp->icmp_cksum = 0;
        else
            icmp->icmp_cksum = IpUtils::IPChecksum((uint16_t *)icmp, 64);
        len = 64;

        ip->ip_len = htons(len + sizeof(struct ip));
        len += sizeof(struct ip) + sizeof(struct ether_header) +
            Agent::GetInstance()->pkt()->pkt_handler()->EncapHeaderLen();
        TestPkt0Interface *tap = (TestPkt0Interface *)
                (Agent::GetInstance()->pkt()->control_interface());
        tap->TxPacket(buf, len);
    }

private:
    DBTableBase::ListenerId rid_;
    uint32_t itf_count_;
    std::vector<std::size_t> itf_id_;
    tbb::mutex mutex_;
    int icmp_seq_;
};

class AsioRunEvent : public Task {
public:
    AsioRunEvent() : Task(75) { };
    virtual  ~AsioRunEvent() { };
    bool Run() {
        Agent::GetInstance()->event_manager()->Run();
        return true;
    }
    std::string Description() const { return "AsioRunEvent"; }
};

TEST_F(IcmpTest, IcmpPingTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "7.8.9.2", "00:00:00:02:02:02", 1, 2},
    };
    IcmpProto::IcmpStats stats;

    IpamInfo ipam_info[] = {
        {"1.2.3.128", 27, "1.2.3.129", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
        {"1.1.1.0", 24, "1.1.1.200", true},
    };

    IpamInfo ipam_updated_info[] = {
        {"4.2.3.128", 24, "4.2.3.254", true},
        {"1.1.1.0", 24, "1.1.1.254", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
    };

    CreateVmportEnv(input, 2, 0);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 3);
    client->WaitForIdle();

    ClearAllInfo *clear_req1 = new ClearAllInfo();
    clear_req1->HandleRequest();
    client->WaitForIdle();
    clear_req1->Release();

    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[0].gw)));
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[1].gw)));
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[2].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_info[0].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_info[1].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_info[2].gw)));
    int count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        stats = Agent::GetInstance()->GetIcmpProto()->GetStats();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats.icmp_gw_ping < 6);
    client->WaitForIdle();
    EXPECT_EQ(6U, stats.icmp_gw_ping);
    EXPECT_EQ(0U, stats.icmp_drop);

    IcmpInfo *sand1 = new IcmpInfo();
    Sandesh::set_response_callback(
        boost::bind(&IcmpTest::CheckSandeshResponse, this, _1, 6));
    sand1->HandleRequest();
    client->WaitForIdle();
    sand1->Release();

    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_updated_info[0].gw)));
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_updated_info[1].gw)));
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_updated_info[2].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[0].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[1].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[2].gw)));
    count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        stats = Agent::GetInstance()->GetIcmpProto()->GetStats();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats.icmp_gw_ping < 8);
    client->WaitForIdle();
    EXPECT_EQ(8U, stats.icmp_gw_ping);
    EXPECT_EQ(0U, stats.icmp_drop);

    IcmpInfo *sand2 = new IcmpInfo();
    Sandesh::set_response_callback(
        boost::bind(&IcmpTest::CheckSandeshResponse, this, _1, 8));
    sand2->HandleRequest();
    client->WaitForIdle();
    sand2->Release();

    // Send updated Ipam
    char buf[BUF_SIZE];
    int len = 0;

    memset(buf, 0, BUF_SIZE);
    AddXmlHdr(buf, len);
    AddNodeString(buf, len, "virtual-network", "vn1", 1);
    AddNodeString(buf, len, "virtual-network-network-ipam", "default-network-ipam,vn1", ipam_updated_info, 3);
    LinkString(buf, len, "virtual-network", "vn1", "virtual-network-network-ipam", "default-network-ipam,vn1");
    AddXmlTail(buf, len);
    ApplyXmlString(buf);
    client->WaitForIdle();

    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[0].gw)));
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[1].gw)));
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[2].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_info[0].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_info[1].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_info[2].gw)));
    count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        stats = Agent::GetInstance()->GetIcmpProto()->GetStats();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats.icmp_gw_ping < 10);
    client->WaitForIdle();
    EXPECT_EQ(10U, stats.icmp_gw_ping);
    EXPECT_EQ(0U, stats.icmp_drop);

    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_updated_info[0].gw)));
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_updated_info[1].gw)));
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_updated_info[2].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[0].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[1].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[2].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[2].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[2].gw)));
    SendIcmp(GetItfId(1), ntohl(inet_addr(ipam_updated_info[2].gw)));
    count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        stats = Agent::GetInstance()->GetIcmpProto()->GetStats();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats.icmp_gw_ping < 19);
    client->WaitForIdle();
    EXPECT_EQ(19U, stats.icmp_gw_ping);
    EXPECT_EQ(0U, stats.icmp_drop);

    IcmpInfo *sand3 = new IcmpInfo();
    Sandesh::set_response_callback(
        boost::bind(&IcmpTest::CheckSandeshResponse, this, _1, 19));
    sand3->HandleRequest();
    client->WaitForIdle();
    sand3->Release();

    ClearAllInfo *clear_req2 = new ClearAllInfo();
    clear_req2->HandleRequest();
    client->WaitForIdle();
    clear_req2->Release();

    IcmpInfo *sand4 = new IcmpInfo();
    Sandesh::set_response_callback(
        boost::bind(&IcmpTest::CheckSandeshResponse, this, _1, 0));
    sand4->HandleRequest();
    client->WaitForIdle();
    sand4->Release();

    Agent::GetInstance()->GetIcmpProto()->ClearStats();

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 2, 1, 0);
    client->WaitForIdle();
}

TEST_F(IcmpTest, IcmpErrorTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
    };
    IcmpProto::IcmpStats stats;

    IpamInfo ipam_info[] = {
        {"1.1.1.0", 24, "1.1.1.200", true},
    };

    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 1);
    client->WaitForIdle();

    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[0].gw)), TYPE_ERROR);
    int count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        stats = Agent::GetInstance()->GetIcmpProto()->GetStats();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats.icmp_drop < 1);
    client->WaitForIdle();
    EXPECT_EQ(0U, stats.icmp_gw_ping);
    EXPECT_EQ(1U, stats.icmp_drop);
    EXPECT_EQ(0U, stats.icmp_gw_ping_err);

    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[0].gw)), CHECKSUM_ERROR);
    count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        stats = Agent::GetInstance()->GetIcmpProto()->GetStats();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats.icmp_gw_ping_err < 1);
    client->WaitForIdle();
    EXPECT_EQ(0U, stats.icmp_gw_ping);
    EXPECT_EQ(1U, stats.icmp_drop);
    EXPECT_EQ(1U, stats.icmp_gw_ping_err);
    Agent::GetInstance()->GetIcmpProto()->ClearStats();

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();
}

TEST_F(IcmpTest, IcmpIntfTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
    };
    Agent *agent = Agent::GetInstance();
    AgentStats *stats = agent->stats();

    IpamInfo ipam_info[] = {
        {"1.1.1.0", 24, "1.1.1.200", true},
    };

    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 1);
    client->WaitForIdle();

    // Provide a non-existing inteface to trigger the icmp_intf_not_inst counter incr
    SendIcmp(989, ntohl(inet_addr(ipam_info[0].gw)));
    int count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats->pkt_invalid_interface() < 1);
    client->WaitForIdle();
    EXPECT_EQ(1U, stats->pkt_invalid_interface());

    // Provide a non-existing inteface to trigger the icmp_intf_not_inst counter incr
    SendIcmp(1989, ntohl(inet_addr(ipam_info[0].gw)));
    count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats->pkt_invalid_interface() < 2);
    client->WaitForIdle();
    EXPECT_EQ(2U, stats->pkt_invalid_interface());

    Agent::GetInstance()->GetIcmpProto()->ClearStats();

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();
}

TEST_F(IcmpTest, IcmpFwdTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
    };
    IcmpProto::IcmpStats stats;

    IpamInfo ipam_info[] = {
        {"1.1.1.0", 24, "1.1.1.200", true},
    };

    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 1);
    client->WaitForIdle();

    // Provide a boolean to set the VM intf attribute via utility function
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[0].gw)), NO_ERROR, false);
    int count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        stats = Agent::GetInstance()->GetIcmpProto()->GetStats();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats.icmp_no_l3fwd < 1);
    client->WaitForIdle();
    EXPECT_EQ(1U, stats.icmp_no_l3fwd);

    // Provide a boolean to set the VM intf attribute via utility function
    SendIcmp(GetItfId(0), ntohl(inet_addr(ipam_info[0].gw)), NO_ERROR, false);
    count = 0;
    do {
        usleep(1000);
        client->WaitForIdle();
        stats = Agent::GetInstance()->GetIcmpProto()->GetStats();
        if (++count == MAX_WAIT_COUNT)
            assert(0);
    } while (stats.icmp_no_l3fwd < 2);
    client->WaitForIdle();
    EXPECT_EQ(2U, stats.icmp_no_l3fwd);

    // Clean up for the VM interface L3 Fwd setting
    Interface *itf = Agent::GetInstance()->interface_table()->FindInterface(GetItfId(0));
    VmInterface *vm_itf = static_cast<VmInterface *>(itf);
    vm_itf->set_layer3_forwarding(true);

    Agent::GetInstance()->GetIcmpProto()->ClearStats();

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();
}

void RouterIdDepInit(Agent *agent) {
}

int main(int argc, char *argv[]) {
    GETUSERARGS();

    client = TestInit(init_file, ksync_init, true, true);
    usleep(100000);
    client->WaitForIdle();

    int ret = RUN_ALL_TESTS();
    client->WaitForIdle();
    TestShutdown();
    delete client;
    return ret;
}
