#include <base/os.h>
#include <iostream>
#include <boost/program_options.hpp>
#include <testing/gunit.h>
#include <pkt/flow_mgmt.h>
#include <pkt/flow_proto.h>
#include <pkt/flow_table.h>
#include <pkt/flow_entry.h>
#include <test/test_cmn_util.h>
#include "test-xml/test_xml.h"
#include "test-xml/test_xml_oper.h"
#include "oper/test/test_xml_physical_device.h"
#include "test_xml_flow_agent_init.h"
#include "test_pkt_util.h"

using namespace std;
namespace opt = boost::program_options;

IpamInfo ipam_info[] = {
    {"1.1.1.0", 24, "1.1.1.10"},
};

static void GetArgs(char *test_file, int argc, char *argv[]) {
    test_file[0] = '\0';
    opt::options_description desc("Options");
    opt::variables_map vars;
    desc.add_options()
        ("help", "Print help message")
        ("test-data", opt::value<string>(), "Specify test data file");

    opt::store(opt::parse_command_line(argc, argv, desc), vars);
    opt::notify(vars);
    if (vars.count("test-data")) {
        strcpy(test_file, vars["test-data"].as<string>().c_str());
    }
    return;
}

class TestPkt : public ::testing::Test {
public:
    virtual void SetUp() {
        agent_ = Agent::GetInstance();
        proto_ = agent_->pkt()->get_flow_proto();
        interface_count_ = agent_->interface_table()->Size();
        flow_stats_collector_ = agent_->flow_stats_manager()->
            default_flow_stats_collector_obj();
        FlowStatsTimerStartStop(agent_, true);
        AddIPAM("vn1", ipam_info, 1);
        client->WaitForIdle();
    }

    void DumpLeftovers(const char *where) {
        std::cout << "LEFTOVERS after " << where
            << ": flows=" << agent_->pkt()->get_flow_proto()->FlowCount()
            << " vns=" << agent_->vn_table()->Size()
            << " intfs=" << agent_->interface_table()->Size()
            << " (baseline " << interface_count_ << ")" << std::endl;

        DBTablePartition *vp = static_cast<DBTablePartition *>
            (agent_->vn_table()->GetTablePartition(0));
        for (DBEntry *e = vp->GetFirst(); e != NULL; e = vp->GetNext(e)) {
            VnEntry *vn = static_cast<VnEntry *>(e);
            std::cout << "  VN left: " << vn->GetName()
                << " deleted=" << vn->IsDeleted() << std::endl;
        }

        DBTablePartition *ip = static_cast<DBTablePartition *>
            (agent_->interface_table()->GetTablePartition(0));
        for (DBEntry *e = ip->GetFirst(); e != NULL; e = ip->GetNext(e)) {
            Interface *intf = static_cast<Interface *>(e);
            std::cout << "  INTF left: " << intf->name()
                << " deleted=" << intf->IsDeleted() << std::endl;
        }

        FlowProto *proto = agent_->pkt()->get_flow_proto();
        for (uint32_t i = 0; i < proto->flow_table_count(); i++) {
            FlowTable *ftable = proto->GetTable(i);
            if (ftable == NULL) {
                continue;
            }
            for (FlowTable::FlowEntryMap::iterator it = ftable->begin();
                 it != ftable->end(); ++it) {
                FlowEntry *fe = it->second;
                const FlowKey &k = fe->key();
                std::cout << "  FLOW left: nh=" << k.nh
                    << " " << k.src_addr.to_string()
                    << " -> " << k.dst_addr.to_string()
                    << " proto=" << (int)k.protocol
                    << " " << k.src_port << "/" << k.dst_port
                    << " deleted=" << fe->deleted()
                    << " short=" << fe->IsShortFlow()
                    << " short_reason=" << fe->short_flow_reason()
                    << " handle=" << fe->flow_handle()
                    << " rflow=" << (fe->reverse_flow_entry() ? 1 : 0)
                    << std::endl;
            }
        }
    }

    virtual void TearDown() {
        client->agent()->flow_stats_manager()->set_delete_short_flow(true);
        client->EnqueueFlowAge();
        client->WaitForIdle();
        client->agent()->flow_stats_manager()->set_delete_short_flow(false);

        WAIT_FOR(3000, 1000,
                  (0U == agent_->pkt()->get_flow_proto()->FlowCount()));
        WAIT_FOR(3000, 1000, (0U == agent_->vn_table()->Size()));
        WAIT_FOR(3000, 1000,
                 (interface_count_ == agent_->interface_table()->Size()));

        bool dirty = (agent_->pkt()->get_flow_proto()->FlowCount() != 0U) ||
                     (agent_->vn_table()->Size() != 0U) ||
                     (agent_->interface_table()->Size() != interface_count_);
        if (dirty) {
            DumpLeftovers("test body");
        }

        FlowStatsTimerStartStop(agent_, false);
        DelIPAM("vn1");
        client->WaitForIdle();
        if (dirty) {
            client->EnqueueFlowFlush();
            client->WaitForIdle();
            WAIT_FOR(1000, 1000,
                     (0U == agent_->pkt()->get_flow_proto()->FlowCount()));
            DumpLeftovers("cleanup attempt");
        }
    }

    Agent *agent_;
    FlowProto *proto_;
    uint32_t interface_count_;
    FlowStatsCollectorObject* flow_stats_collector_;
};

TEST_F(TestPkt, parse_1) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/pkt-parse.xml");
    AgentUtXmlOperInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, ingress_flow_1) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/ingress-flow.xml");
    AgentUtXmlOperInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, egress_flow_1) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/egress-flow.xml");
    AgentUtXmlOperInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, l2_sg_flow_1) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/l2-sg-flow.xml");
    AgentUtXmlOperInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}

TEST_F(TestPkt, rpf_flow) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/rpf-flow.xml");
    AgentUtXmlOperInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
}


TEST_F(TestPkt, DISABLED_unknown_unicast_flood) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/unknown-unicast-flood.xml");
    AgentUtXmlOperInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
    client->WaitForIdle();
    client->agent()->flow_stats_manager()->set_delete_short_flow(true);
    client->EnqueueFlowAge();
    client->WaitForIdle();
    WAIT_FOR(1000, 1000, (0U == proto_->FlowCount()));
    client->agent()->flow_stats_manager()->set_delete_short_flow(false);
}

TEST_F(TestPkt, tcp) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/tcp_flow.xml");
    AgentUtXmlOperInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
    client->WaitForIdle();
    client->agent()->flow_stats_manager()->set_delete_short_flow(true);
    client->EnqueueFlowAge();
    client->WaitForIdle();
    WAIT_FOR(1000, 1000, (0U == proto_->FlowCount()));
    client->agent()->flow_stats_manager()->set_delete_short_flow(false);
}

TEST_F(TestPkt, DISABLED_flow_eviction) {
    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/flow-eviction.xml");
    AgentUtXmlOperInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }
    client->WaitForIdle();
    client->agent()->flow_stats_manager()->set_delete_short_flow(true);
    client->EnqueueFlowAge();
    client->WaitForIdle();
    WAIT_FOR(1000, 1000, (0U == proto_->FlowCount()));
    client->agent()->flow_stats_manager()->set_delete_short_flow(false);
}


TEST_F(TestPkt, flow_tsn_mode_1) {
    Agent *agent = Agent::GetInstance();
    //agent->set_tsn_enabled(true);
    IpamInfo ipam_info[] = {
        {"1.1.1.0", 24, "1.1.1.254", true}
    };
    AddVn("vn1", 1);
    AddVrf("vrf1", 1);
    AddIPAM("vn1", ipam_info, 1);
    client->WaitForIdle();

    boost::system::error_code ec;
    EcmpTunnelRouteAdd(agent, bgp_peer_, "vrf1", "1.1.1.0", 24,
                       "100.100.100.1", 1, "100.100.100.2", 2, "vn1");

    AgentUtXmlTest test("controller/src/vnsw/agent/pkt/test/tsn-flow.xml");
    AgentUtXmlOperInit(&test);
    AgentUtXmlPhysicalDeviceInit(&test);
    ASSERT_TRUE(test.Load()) << "Unable load test XML";
    {
        test.ReadXml();

        string str;
        test.ToString(&str);
        cout << str << endl;
        test.Run();
    }

    DeleteRoute("vrf1", "1.1.1.0", 24, bgp_peer_);
    client->WaitForIdle();
    DelIPAM("vn1");
    DelVn("vn1");
    client->WaitForIdle();
}

int main(int argc, char *argv[]) {
    GETUSERARGS();

    client = XmlPktParseTestInit(init_file, ksync_init);
    client->agent()->flow_stats_manager()->
        default_flow_stats_collector_obj()->SetExpiryTime(1000*1000);
    client->agent()->flow_stats_manager()->set_delete_short_flow(false);
    AddFlowExportRate(100);
    boost::system::error_code ec;
    bgp_peer_ = CreateBgpPeer(Ip4Address::from_string("0.0.0.1", ec),
                                          "xmpp channel");
    usleep(1000);
    client->WaitForIdle();
    int ret = RUN_ALL_TESTS();
    DeleteBgpPeer(bgp_peer_);
    TestShutdown();
    delete client;
    return ret;
}
