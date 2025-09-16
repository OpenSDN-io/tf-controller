/*
 * Copyright (c) 2018-2024 Juniper Networks, Inc. All rights reserved.
 * Copyright (c) 2024 Elena Zizganova
 */

#include "base/os.h"
#include "testing/gunit.h"
#include "sandesh/library/cpp/sandesh_trace.h"

#include <base/logging.h>
#include <io/event_manager.h>
#include <io/test/event_manager_test.h>
#include <tbb/task.h>
#include <base/task.h>
#include "net/bgp_af.h"
#include <cmn/agent_cmn.h>

#include "cfg/cfg_init.h"
#include "pkt/pkt_init.h"
#include "services/services_init.h"
#include "vrouter/ksync/ksync_init.h"
#include "oper/interface_common.h"
#include "oper/nexthop.h"
#include "oper/tunnel_nh.h"
#include "route/route.h"
#include "oper/vrf.h"
#include "oper/mpls.h"
#include "oper/vm.h"
#include "oper/vn.h"
#include "oper/physical_device_vn.h"
#include "filter/acl.h"
#include "test_cmn_util.h"
#include "vr_types.h"

#include "xmpp/xmpp_init.h"
#include "xmpp/test/xmpp_test_util.h"
#include "vr_types.h"
#include "control_node_mock.h"
#include "xml/xml_pugi.h"
#include "oper/vxlan_routing_manager.h"
#include "controller/controller_init.h"
#include <controller/controller_export.h>

using namespace pugi;
#define L3_VRF_OFFSET 100

#define NULL_VRF ""
#define ZERO_IP "0.0.0.0"
#define ZERO_MAC "00:00:00:00:00:00"
MacAddress zero_mac;

struct PortInfo input1[] = {
    {"vnet10", 10, "1.1.1.10", "00:00:01:01:01:10", 1, 10},
    {"vnet11", 11, "1.1.1.11", "00:00:01:01:01:11", 1, 11},
};

struct PortInfo input2[] = {
    {"vnet20", 20, "2.2.2.20", "00:00:02:02:02:20", 2, 20},
};

IpamInfo ipam_1[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
};

IpamInfo ipam_2[] = {
    {"2.2.2.0", 24, "2.2.2.200", true},
};

void RouterIdDepInit(Agent *agent) {
}

#define INPUT_SIZE(x) (sizeof(x) / sizeof(PortInfo))

static void ValidateSandeshResponse(Sandesh *sandesh, vector<int> &result) {
    //TBD
    //Validate the response by the expectation
}

void DoInterfaceSandesh(std::string name) {
    ItfReq *itf_req = new ItfReq();
    std::vector<int> result = boost::assign::list_of(1);
    Sandesh::set_response_callback(boost::bind(ValidateSandeshResponse, _1,
                                               result));
    if (name != "") {
        itf_req->set_name(name);
    }
    itf_req->HandleRequest();
    client->WaitForIdle();
    itf_req->Release();
    client->WaitForIdle();
}

class VxlanRoutingTest : public ::testing::Test {
protected:
    VxlanRoutingTest() {
    }

    virtual void SetUp() {
        client->Reset();
        agent_ = Agent::GetInstance();
        bgp_peer_ = nullptr;
    }

    virtual void TearDown() {
    }

    void SetupEnvironment() {
        bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
        client->WaitForIdle();
        AddIPAM("vn1", ipam_1, 1);
        AddIPAM("vn2", ipam_2, 1);
        CreateVmportEnv(input1, INPUT_SIZE(input1));
        CreateVmportEnv(input2, INPUT_SIZE(input2));
        AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                    "instance_ip_1", 1);
        AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                    "instance_ip_2", 2);
        client->WaitForIdle();
    }

    void DeleteEnvironment(bool vxlan_enabled) {
        client->WaitForIdle();
        DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                    "instance_ip_1", 1);
        DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                    "instance_ip_2", 2);
        DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
        DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
        DelIPAM("vn1");
        DelIPAM("vn2");
        client->WaitForIdle();
        // Verify subnet route is deleted on vn detach as lr vmi port is
        // deleted
        InetUnicastRouteEntry *subnet_rt_vn1 =
            RouteGet("vrf2", Ip4Address::from_string("1.1.1.0"), 24);
        EXPECT_TRUE(subnet_rt_vn1 == nullptr);
        InetUnicastRouteEntry *subnet_rt_vn2 =
            RouteGet("vrf2", Ip4Address::from_string("2.2.2.0"), 24);
        EXPECT_TRUE(subnet_rt_vn2 == nullptr);
        DeleteBgpPeer(bgp_peer_);
        DelNode("project", "admin");
        client->WaitForIdle(5);
        EXPECT_TRUE(VrfGet("vrf1") == nullptr);
        EXPECT_TRUE(VrfGet("vrf2") == nullptr);
        EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                    IsEmpty());
        EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                    IsEmpty());
        client->WaitForIdle();
    }

    void ValidateBridge(const std::string &bridge_vrf,
                        const std::string &routing_vrf,
                        const Ip4Address &addr,
                        uint8_t plen,
                        bool participate) {
        InetUnicastRouteEntry *rt =
            RouteGet(bridge_vrf, addr, plen);
        if (participate) {
            InetUnicastRouteEntry *default_rt =
            RouteGet(bridge_vrf, Ip4Address::from_string("0.0.0.0"), 0);
            EXPECT_TRUE(default_rt == nullptr);
            EXPECT_TRUE(rt->GetActivePath()->peer()->GetType() ==
                        Peer::EVPN_ROUTING_PEER);
            const VrfNH *nh = dynamic_cast<const VrfNH *>
                (rt->GetActiveNextHop());
            EXPECT_TRUE(nh->GetVrf()->GetName() == routing_vrf);
        } else {
            if (rt == nullptr)
                return;

            EXPECT_TRUE(rt->GetActivePath()->peer()->GetType() !=
                        Peer::EVPN_ROUTING_PEER);
            const VrfNH *nh = dynamic_cast<const VrfNH *>
                (rt->GetActiveNextHop());
            EXPECT_TRUE(nh == nullptr);
        }
    }

    void ValidateBridgeRemote(const std::string &bridge_vrf,
                              const std::string &routing_vrf,
                              const Ip4Address &addr,
                              uint8_t plen,
                              bool participate) {
        InetUnicastRouteEntry *rt =
            RouteGet(bridge_vrf, addr, plen);
        if (participate) {
            EXPECT_TRUE(rt->GetActivePath()->peer()->GetType() ==
                        Peer::BGP_PEER);
            const VrfNH *nh = dynamic_cast<const VrfNH *>
                (rt->GetActiveNextHop());
            EXPECT_TRUE(nh->GetVrf()->GetName() == routing_vrf);
        } else {
            if (rt == nullptr)
                return;

            EXPECT_TRUE(rt->GetActivePath()->peer()->GetType() !=
                        Peer::BGP_PEER);
            const VrfNH *nh = dynamic_cast<const VrfNH *>
                (rt->GetActiveNextHop());
            EXPECT_TRUE(nh == nullptr);
        }
    }

    void ValidateRouting(const std::string &routing_vrf,
                         const Ip4Address &addr,
                         uint8_t plen,
                         const std::string &dest_name,
                         bool present,
                         const std::string &origin_vn = "") {
        InetUnicastRouteEntry *rt =
            RouteGet(routing_vrf, addr, plen);
        if (present) {
            ASSERT_TRUE(rt != nullptr);
            const auto *rt_path = rt->GetActivePath();
            ASSERT_TRUE(rt_path != nullptr);
            const auto &path_preference = rt_path->path_preference();
            const InterfaceNH *intf_nh =
                dynamic_cast<const InterfaceNH *>(rt->GetActiveNextHop());
            if (intf_nh) {
                EXPECT_TRUE(intf_nh->GetInterface()->name() == dest_name);
                EXPECT_TRUE(intf_nh->IsVxlanRouting());
                if (agent_->local_vm_export_peer()->GetType() ==
                    rt_path->peer()->GetType()) {
                    EXPECT_TRUE(path_preference.loc_sequence() == 0);
                }
            }
            const CompositeNH *comp_nh =
                dynamic_cast<const CompositeNH *>(rt->GetActiveNextHop());
            if (comp_nh) {
                EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
                if (agent_->local_vm_export_peer()->GetType() ==
                    rt_path->peer()->GetType()) {
                    EXPECT_TRUE(path_preference.loc_sequence() > 0);
                }
            }
            const TunnelNH *tunnel_nh =
                dynamic_cast<const TunnelNH *>(rt->GetActiveNextHop());
            if (tunnel_nh) {
                EXPECT_TRUE(tunnel_nh->GetDip()->to_string() == dest_name);
            }
            EXPECT_TRUE(rt_path->origin_vn() == origin_vn);
        } else {
            EXPECT_TRUE(rt == nullptr);
        }
    }

    static void ProcessBufferItem(SandeshTrace *buffer_item, bool not_last) {
        std::string msg_str = buffer_item->ToString();
        std::string::size_type vn_name_pos = msg_str.find("name = l3evpn_1");
        if (vn_name_pos != std::string::npos) {
            std::string::size_type vxlan_id_pos = msg_str.find(
                "vxlan_id = 201");
            std::cout << "Oper db.vxlan.0 item buffer_item->ToString() = "
                    << buffer_item->ToString() << std::endl;
            if (vxlan_id_pos == std::string::npos) {
                vxlan_id_was_not_found_ = true;
            }
        }
    }

    ///
    BgpPeer *bgp_peer_;

    ///
    Agent *agent_;

    /// Stores the result of Oper DB trace scanning for a proper VxLAN ID value
    /// of a routing VN
    static bool vxlan_id_was_not_found_;
};

bool VxlanRoutingTest::vxlan_id_was_not_found_;

TEST_F(VxlanRoutingTest, CheckVnVxlanId) {
    SandeshTraceBufferPtr oper_db_trace_buff_ptr =
        SandeshTraceBufferGet("Oper db.vxlan.0");
    const std::string context = "oper_db_vxlan_context";
    SetupEnvironment();
    AddLrRoutingVrf(1);

    client->WaitForIdle();
    vxlan_id_was_not_found_ = false;
    SandeshTraceBufferRead(oper_db_trace_buff_ptr,
                           context,
                           0,
                           VxlanRoutingTest::ProcessBufferItem);
    client->WaitForIdle();
    EXPECT_FALSE(vxlan_id_was_not_found_);
    DelLrRoutingVrf(1);
    DeleteEnvironment(true);
}

TEST_F(VxlanRoutingTest, Basic) {
    using boost::uuids::nil_uuid;

    SetupEnvironment();
    AddLrRoutingVrf(1);
    EXPECT_TRUE(VmInterfaceGet(10)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(11)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", false);
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", false);
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", false);
    // No subnet route is added into the bridge vrf
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);
    client->WaitForIdle();
    DelLrRoutingVrf(1);
    DeleteEnvironment(true);
}

TEST_F(VxlanRoutingTest, Route_1) {
    using boost::uuids::nil_uuid;

    SetupEnvironment();
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);

    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(10)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(11)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", false);
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);

    // Trigger vxlan routing manager's walker
    VxlanRoutingRouteWalker *walker = dynamic_cast<VxlanRoutingRouteWalker*>(
        agent_->oper_db()->vxlan_routing_manager()->walker());
    if (walker) {
        VrfEntry *vrf = Agent::GetInstance()->vrf_table()->FindVrfFromName(
            "vrf1");
        if (vrf != nullptr) {
            walker->StartRouteWalk(vrf);
        }
    }
    client->WaitForIdle();
    // Validate the routes again
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);

    // Get routing vrf
    VrfEntry *routing_vrf= VrfGet("l3evpn_1");
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // Delete routing vrf
    DelLrRoutingVrf(1);
    client->WaitForIdle();
    // route update
    autogen::EnetItemType item;
    SecurityGroupList sg;
    item.entry.nlri.af = BgpAf::L2Vpn;
    item.entry.nlri.safi = BgpAf::Enet;
    item.entry.nlri.address = "10.10.10.10";
    item.entry.nlri.ethernet_tag = 0;
    autogen::EnetNextHopType nh;
    nh.af = Address::INET;
    nh.address = "10.10.10.11";
    nh.label = routing_vrf->vxlan_id();;
    item.entry.next_hops.next_hop.push_back(nh);
    item.entry.med = 0;

    // send route add in deleted l3evpn_1 vrf
    bgp_peer_->GetAgentXmppChannel()->AddEvpnRoute("l3evpn_1",
            "00:00:00:00:00:00",
            Ip4Address::from_string("4.4.4.0"),
            24, &item);
    client->WaitForIdle();

    // since routing vrf was deleted, route is not found in bridge vrf
    InetUnicastRouteEntry *rt_del =
        RouteGet("vrf1", Ip4Address::from_string("4.4.4.0"), 24);
    EXPECT_TRUE( rt_del == nullptr);

    // cleanup
    client->WaitForIdle();
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DeleteEnvironment(true);
    client->WaitForIdle();
    EXPECT_TRUE (VrfGet("l3evpn_1") == nullptr);
}

TEST_F(VxlanRoutingTest, Route_2) {
    using boost::uuids::nil_uuid;

    SetupEnvironment();
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    EXPECT_TRUE(VmInterfaceGet(10)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(11)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", false);
   // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);

    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DeleteEnvironment(true);
}


TEST_F(VxlanRoutingTest, Route_3) {
    using boost::uuids::nil_uuid;

    SetupEnvironment();
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    EXPECT_TRUE(VmInterfaceGet(10)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(11)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", true, "vn2");
    // validate subnet route for vn2 gets added to vrf1 inet table
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.0"), 24, true);
    // validate subnet route for vn1 gets added to vrf2 inet table
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, true);
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);

    DelLrBridgeVrf("vn1", 1);
    DelLrBridgeVrf("vn2", 1);
    DelLrRoutingVrf(1);
    DeleteEnvironment(true);
}

TEST_F(VxlanRoutingTest, Route_4) {
    using boost::uuids::nil_uuid;

    SetupEnvironment();
    AddLrRoutingVrf(1);
    AddLrRoutingVrf(2);
    AddLrBridgeVrf("vn1", 2);
    AddLrBridgeVrf("vn2", 1);
    EXPECT_TRUE(VmInterfaceGet(10)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(11)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", false);
    ValidateRouting("l3evpn_2", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", true, "vn1");
    ValidateRouting("l3evpn_2", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", true, "vn2");
    // only one bridge vn added in Lr 2, verify no subnet route
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.0"), 24, false);
    // only one bridge vn added in Lr 1, verify no subnet route
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, false);
    ValidateBridge("vrf1", "l3evpn_2",
                    Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", "l3evpn_2",
                    Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);

    DelLrBridgeVrf("vn1", 2);
    DelLrBridgeVrf("vn2", 1);
    DelLrRoutingVrf(1);
    DelLrRoutingVrf(2);
    DeleteEnvironment(true);
}

TEST_F(VxlanRoutingTest, Route_5) {
    using boost::uuids::nil_uuid;

    SetupEnvironment();
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    EXPECT_TRUE(VmInterfaceGet(10)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(11)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    MacAddress dummy_mac;
    BridgeTunnelRouteAdd(bgp_peer_, "l3evpn_1", TunnelType::VxlanType(),
                        Ip4Address::from_string("100.1.1.11"),
                        101, dummy_mac,
                        Ip4Address::from_string("1.1.1.20"),
                        32, "00:00:99:99:99:99");
    client->WaitForIdle();
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.20"), 32,
                    "100.1.1.11", true);
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", false);
    // only one bridge vn added , verify no subnet route
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);
    EvpnAgentRouteTable::DeleteReq(bgp_peer_, "l3evpn_1", MacAddress(),
                                   Ip4Address::from_string("1.1.1.20"), 32,
                                   0, nullptr);
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DeleteEnvironment(true);
}

TEST_F(VxlanRoutingTest, Route_6) {
    using boost::uuids::nil_uuid;

    SetupEnvironment();
    AddLrRoutingVrf(1);
    AddLrRoutingVrf(2);
    AddLrBridgeVrf("vn1", 1);
    EXPECT_TRUE(VmInterfaceGet(10)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(11)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
#if 0
    InetUnicastRouteEntry *rt1 =
        RouteGet("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32);
    InetUnicastRouteEntry *rt2 =
        RouteGet("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32);
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", (rt2 == nullptr));
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", (rt1 == nullptr));
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("0.0.0.0"), 0, true);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, (rt2 == nullptr));
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.11"), 32, (rt1 == nullptr));
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);
#endif
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrRoutingVrf(2);
    DeleteEnvironment(true);
}

TEST_F(VxlanRoutingTest, Route_7) {
    using boost::uuids::nil_uuid;

    SetupEnvironment();

    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1, "snat-routing");
    EXPECT_TRUE(VmInterfaceGet(91) == nullptr);
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DeleteEnvironment(true);
}

// Adding EVPN Type5 route to LR evpn table and verify it gets replicated
// to bridge vrf tables
TEST_F(VxlanRoutingTest, Lrvrf_Evpn_Type5_RouteAdd) {
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 20, "2.2.2.20", "00:00:02:02:02:20", 2, 20},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    struct PortInfo input3[] = {
        {"vnet3", 30, "3.3.3.30", "00:00:03:03:03:30", 3, 30},
    };
    IpamInfo ipam_info_3[] = {
        {"3.3.3.0", 24, "3.3.3.200", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);
    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", false);

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);

    // since vn2 is not included in the LR,
    // check to see no route add by peer:EVPN_ROUTING_PEER
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // Test Type 5 route add/del in LR vrf
    stringstream ss_node;
    autogen::EnetItemType item;
    SecurityGroupList sg;
    item.entry.nlri.af = BgpAf::L2Vpn;
    item.entry.nlri.safi = BgpAf::Enet;
    item.entry.nlri.address = "10.10.10.10";
    item.entry.nlri.ethernet_tag = 0;
    autogen::EnetNextHopType nh;
    nh.af = Address::INET;
    nh.address = "10.10.10.11";
    nh.label = routing_vrf->vxlan_id();
    nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    item.entry.next_hops.next_hop.push_back(nh);
    item.entry.med = 0;

    // Add type5 route 10.10.10.10/32 to lr evpn table
    bgp_peer_->GetAgentXmppChannel()->AddEvpnRoute(routing_vrf_name,
            "00:00:00:00:00:00",
            Ip4Address::from_string("10.10.10.10"),
            32, &item);
    client->WaitForIdle();
    // Validate route is copied to bridge vrf
    InetUnicastRouteEntry *rt1 =
        RouteGet(routing_vrf_name, Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE( rt1 != nullptr);

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet(routing_vrf_name, Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE( lr_vrf_rt != nullptr);
    EXPECT_TRUE(lr_vrf_rt->GetActivePath()->vxlan_id() == routing_vrf->
                                                          vxlan_id());

    // Change label in external rt and verify rt path vxlan id is set to new
    // label
    nh.label = 8282;
    item.entry.next_hops.next_hop[0] = nh;

    bgp_peer_->GetAgentXmppChannel()->AddEvpnRoute(routing_vrf_name,
            "00:00:00:00:00:00",
            Ip4Address::from_string("10.10.10.10"),
            32, &item);
    client->WaitForIdle();
    InetUnicastRouteEntry *lr_vrf_rt1 =
        RouteGet(routing_vrf_name, Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE( lr_vrf_rt1 != nullptr);
    EXPECT_TRUE(lr_vrf_rt1->GetActivePath()->vxlan_id() == 8282);

    // Verify route for local vm port is still present
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);

    AddIPAM("vn3", ipam_info_3, 1);
    CreateVmportEnv(input3, INPUT_SIZE(input3));
    AddLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);
    client->WaitForIdle();
    AddLrBridgeVrf("vn3", 1);
    client->WaitForIdle();

    // check to see if the subnet route for vn3 added to the bridge vrf1 inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);

    // check to see if the subnet route for vn1 added to the bridge vrf3 inet
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);

    // Verify type5 route added to Lr evpn table is copied to bridge vrf3 inet
    InetUnicastRouteEntry *static_rt_vrf3 =
        RouteGet("vrf3", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(static_rt_vrf3 != nullptr);

    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    EvpnAgentRouteTable *rt_table1 = static_cast<EvpnAgentRouteTable *>
        (agent_->vrf_table()->GetEvpnRouteTable(routing_vrf_name));
    rt_table1->DeleteReq(bgp_peer_,
        routing_vrf_name,
        MacAddress::FromString("00:00:00:00:00:00"),
        Ip4Address::from_string("10.10.10.10"),
        32, 0,
        new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    InetUnicastRouteEntry *rt_del =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del == nullptr);
    InetUnicastRouteEntry *rt_del_vrf3 =
        RouteGet("vrf3", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del_vrf3 == nullptr);

    // Clean up
    client->WaitForIdle();

    // Bridge VN3
    DelLrBridgeVrf("vn3", 1);
    DelLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);
    DeleteVmportEnv(input3, INPUT_SIZE(input3), true);
    DelIPAM("vn3");

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    DelIPAM("vn1");
    DelIPAM("vn2");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(VrfGet("vrf2") == nullptr);
    EXPECT_TRUE(VrfGet("vrf3") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
            IsEmpty());
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
            IsEmpty());
    client->WaitForIdle();
}

TEST_F(VxlanRoutingTest, LrRoutingVrfDeletion) {
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    CreateVmportEnv(input1, INPUT_SIZE(input1));
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    const char *routing_vrf_name = "l3evpn_1";
    AddLrBridgeVrf("vn1", 1);
    client->WaitForIdle();
    AddLrRoutingVrf(1);

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // Test Type 5 route add/del in LR vrf
    stringstream ss_node;
    autogen::EnetItemType item;
    SecurityGroupList sg;
    item.entry.nlri.af = BgpAf::L2Vpn;
    item.entry.nlri.safi = BgpAf::Enet;
    item.entry.nlri.address = "10.10.10.10";
    item.entry.nlri.ethernet_tag = 0;
    autogen::EnetNextHopType nh;
    nh.af = Address::INET;
    nh.address = "10.10.10.11";
    nh.label = routing_vrf->vxlan_id();
    nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    item.entry.next_hops.next_hop.push_back(nh);
    item.entry.med = 0;

    // Add type5 route 10.10.10.10/32 to lr evpn table
    bgp_peer_->GetAgentXmppChannel()->AddEvpnRoute(routing_vrf_name,
                                                   "00:00:00:00:00:00",
                                                   Ip4Address::from_string(
                                                      "10.10.10.10"),
                                                   32, &item);
    client->WaitForIdle();
    // Validate route is copied to bridge vrf
    InetUnicastRouteEntry *rt1 =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE( rt1 != NULL);

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet(routing_vrf_name, Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE( lr_vrf_rt != NULL);
    EXPECT_TRUE(lr_vrf_rt->GetActivePath()->vxlan_id() == routing_vrf->vxlan_id());

    {
        std::stringstream name_ss;
        name_ss << "l3evpn_" << int(1);
        DelLink("virtual-network",
                name_ss.str().c_str(),
                "routing-instance",
                name_ss.str().c_str());
    }
    client->WaitForIdle();
    InetUnicastRouteEntry *rt_del_rtvrf =
        RouteGet(routing_vrf_name, Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del_rtvrf == NULL);

    // Clean up
    DelLrRoutingVrf(1);
    DelLrBridgeVrf("vn1", 1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DelIPAM("vn1");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == NULL);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    client->WaitForIdle();
}

TEST_F(VxlanRoutingTest, SubnetRoute) {
    using boost::uuids::nil_uuid;
    SetupEnvironment();
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    EXPECT_TRUE(VmInterfaceGet(10)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(11)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(20)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());

    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet10", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet11", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet20", true, "vn2");
    // validate subnet route for vn2 gets added to vrf1 inet table
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.0"), 24, true);
    // validate subnet route for vn1 gets added to vrf2 inet table
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("2.2.2.20"), 32, false);

    // Add one subnet to existing ipam default-network-ipam,vn1
    IpamInfo ipam_update[] = {
        {"1.1.1.0", 24, "1.1.1.200", true},
        {"3.3.3.0", 24, "3.3.3.233", true},
    };
    AddIPAM("vn1", ipam_update, 2);
    client->WaitForIdle();

    // Validate both subnet routes for vn1 in vn2
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("3.3.3.0"), 24, true);

    // Delete 3.3.3.0/24 subnet from ipam default-network-ipam,vn1
    AddIPAM("vn1", ipam_1, 1);
    client->WaitForIdle();
    // Validate vn2 has subnet route only for 1.1.10/24 and not 3.3.3.0/24
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, true);
    InetUnicastRouteEntry *rt =
        RouteGet("vrf2", Ip4Address::from_string("3.3.3.0"), 24);
    EXPECT_TRUE(rt == nullptr);

    DelIPAM("vn1");
    client->WaitForIdle();

    // Verify both subnet routes for vn1 get deleted in vrf2 inet table
    InetUnicastRouteEntry *rt_sub_1 =
        RouteGet("vrf2", Ip4Address::from_string("1.1.1.0"), 24);
    EXPECT_TRUE(rt_sub_1 == nullptr);
    InetUnicastRouteEntry *rt_sub_2 =
        RouteGet("vrf2", Ip4Address::from_string("3.3.3.0"), 24);
    EXPECT_TRUE(rt_sub_2 == nullptr);
    client->WaitForIdle();
    DelLrBridgeVrf("vn2", 1);
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DeleteEnvironment(true);
}

TEST_F(VxlanRoutingTest, Basic_IRT_32) {
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:00:01:01:01", 1, 1},
    };

    client->Reset();
    AddIPAM("vn1", ipam, 1);
    CreateVmportEnv(input, INPUT_SIZE(input));
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    client->WaitForIdle();
    EXPECT_TRUE(VmPortActive(input, 0));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);

    //Add a static route
    struct TestIp4Prefix static_route[] = {
       { Ip4Address::from_string("24.1.1.1"), 32},
       { Ip4Address::from_string("2.1.1.20"), 32},
    };

    AddInterfaceRouteTable("static_route", 1, static_route, 2);

    AddLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");

    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);

    EXPECT_TRUE(RouteFind("vrf1", static_route[0].addr_,
                        static_route[0].plen_));
    EXPECT_TRUE(RouteFind("vrf1", static_route[1].addr_,
                        static_route[1].plen_));
    InetUnicastRouteEntry *rt =
        RouteGet("vrf1", static_route[0].addr_, static_route[0].plen_);

    EXPECT_TRUE(
        rt->intf_route_type().compare(VmInterface::kInterfaceStatic) == 0);


    ValidateRouting("l3evpn_1", static_route[0].addr_, static_route[0].plen_,
                    "vnet1",  true, "vn1");
    ValidateRouting("l3evpn_1", static_route[1].addr_, static_route[1].plen_,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    static_route[0].addr_, static_route[0].plen_, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    static_route[1].addr_, static_route[1].plen_, false);
    client->WaitForIdle();

    //Delete the link between interface and route table
    DelLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");
    client->WaitForIdle();
    EXPECT_FALSE(RouteFind("vrf1", static_route[0].addr_,
                            static_route[0].plen_));
    EXPECT_FALSE(RouteFind("vrf1", static_route[1].addr_,
                            static_route[1].plen_));
    DoInterfaceSandesh("vnet1");
    client->WaitForIdle();

    DelLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

TEST_F(VxlanRoutingTest, Basic_IRT_24) {
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:00:01:01:01", 1, 1},
    };

    client->Reset();
    AddIPAM("vn1", ipam, 1);
    CreateVmportEnv(input, INPUT_SIZE(input));
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    client->WaitForIdle();
    EXPECT_TRUE(VmPortActive(input, 0));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);

    //Add a static route
    struct TestIp4Prefix static_route[] = {
       { Ip4Address::from_string("24.1.1.0"), 24},
       { Ip4Address::from_string("1.2.2.0"), 24},
    };

    AddInterfaceRouteTable("static_route", 1, static_route, 2);

    AddLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");


    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);

    EXPECT_TRUE(RouteFind("vrf1", static_route[0].addr_,
                        static_route[0].plen_));
    EXPECT_TRUE(RouteFind("vrf1", static_route[1].addr_,
                        static_route[1].plen_));
    InetUnicastRouteEntry *rt =
        RouteGet("vrf1", static_route[0].addr_, static_route[0].plen_);

    EXPECT_TRUE(
        rt->intf_route_type().compare(VmInterface::kInterfaceStatic) == 0);


    ValidateRouting("l3evpn_1", static_route[0].addr_, static_route[0].plen_,
                    "vnet1",  true, "vn1");
    ValidateRouting("l3evpn_1", static_route[1].addr_, static_route[1].plen_,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    static_route[0].addr_, static_route[0].plen_, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    static_route[1].addr_, static_route[1].plen_, false);
    client->WaitForIdle();

    //Delete the link between interface and route table
    DelLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");
    client->WaitForIdle();
    EXPECT_FALSE(RouteFind("vrf1", static_route[0].addr_,
                          static_route[0].plen_));
    EXPECT_FALSE(RouteFind("vrf1", static_route[1].addr_,
                          static_route[1].plen_));
    DoInterfaceSandesh("vnet1");
    client->WaitForIdle();

    DelLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

TEST_F(VxlanRoutingTest, Basic_IRT_16) {
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:00:01:01:01", 1, 1},
    };

    client->Reset();
    AddIPAM("vn1", ipam, 1);
    CreateVmportEnv(input, INPUT_SIZE(input));
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    client->WaitForIdle();
    EXPECT_TRUE(VmPortActive(input, 0));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                   Ip4Address::from_string("1.1.1.10"), 32, false);

    //Add a static route
    struct TestIp4Prefix static_route[] = {
       { Ip4Address::from_string("24.1.0.0"), 16},
       { Ip4Address::from_string("1.1.0.0"), 16},
    };

    AddInterfaceRouteTable("static_route", 1, static_route, 2);

    AddLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");


    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.10"), 32, false);

    EXPECT_TRUE(RouteFind("vrf1", static_route[0].addr_,
                        static_route[0].plen_));
    EXPECT_TRUE(RouteFind("vrf1", static_route[1].addr_,
                        static_route[1].plen_));
    InetUnicastRouteEntry *rt =
        RouteGet("vrf1", static_route[0].addr_, static_route[0].plen_);

    EXPECT_TRUE(
        rt->intf_route_type().compare(VmInterface::kInterfaceStatic) == 0);


    ValidateRouting("l3evpn_1", static_route[0].addr_, static_route[0].plen_,
                    "vnet1",  true, "vn1");
    ValidateRouting("l3evpn_1", static_route[1].addr_, static_route[1].plen_,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    static_route[0].addr_, static_route[0].plen_, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    static_route[1].addr_, static_route[1].plen_, false);
    client->WaitForIdle();

    //Delete the link between interface and route table
    DelLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");
    client->WaitForIdle();
    EXPECT_FALSE(RouteFind("vrf1", static_route[0].addr_,
                            static_route[0].plen_));
    EXPECT_FALSE(RouteFind("vrf1", static_route[1].addr_,
                            static_route[1].plen_));
    DoInterfaceSandesh("vnet1");
    client->WaitForIdle();

    DelLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

//test with "BOTH" direction
TEST_F(VxlanRoutingTest, fip_test_initial_conf_with_lr1) {

    IpamInfo ipam_info1[] = {
    {"1.1.1.0", 24, "1.1.1.1"},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.29", "00:00:00:01:01:01", 1, 1},
    };

    const char* domestic_addr = "1.1.1.2";
    client->WaitForIdle();
    client->Reset();

    AddIPAM("vn1", ipam_info1, 1);
    CreateVmportFIpEnv(input, 1, 0, "vn1", "vrf1");

    AddLink("virtual-network", "vn1", "instance-ip", "instance1");

    AddLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    client->WaitForIdle();

    AddFloatingIp("fip1", 1, domestic_addr,  input[0].addr, "BOTH", true, 80);
    AddLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    AddLink("floating-ip", "fip1", "instance-ip", "instance1");
    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.29"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string(domestic_addr), 32,
                    "vnet1", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.29"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string(domestic_addr), 32, false);

    //Check link with the first interface
    VmInterface *vmi1 = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(vmi1);
    const VmInterface::FloatingIpSet &fip_list1 =
        vmi1->floating_ip_list().list_;
    EXPECT_EQ(fip_list1.size(), 1);

    DelLink("floating-ip", "fip1", "instance-ip", "instance1");
    DelLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    DelFloatingIp("fip1");

    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);

    //Check that corresponding objects were deleted
    EXPECT_EQ(fip_list1.size(), 0);

    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
}

TEST_F(VxlanRoutingTest, fip_test_initial_conf_with_lr1_not_in_mask_vn) {

    IpamInfo ipam_info1[] = {
    {"1.1.1.0", 24, "1.1.1.1"},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.29", "00:00:00:01:01:01", 1, 1},
    };

    const char* domestic_addr = "1.1.1.2";
    const char* foreign_addr = "2.2.2.2";
    client->WaitForIdle();
    client->Reset();

    AddIPAM("vn1", ipam_info1, 1);
    CreateVmportFIpEnv(input, 1, 0, "vn1", "vrf1");

    AddLink("virtual-network", "vn1", "instance-ip", "instance1");

    AddLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    client->WaitForIdle();

    AddFloatingIp("fip1", 1, foreign_addr,  input[0].addr, "BOTH", true, 80);
    AddLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    AddLink("floating-ip", "fip1", "instance-ip", "instance1");
    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.29"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string(foreign_addr), 32,
                    "vnet1", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.29"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string(foreign_addr), 32, false);

    //Check link with the first interface
    VmInterface *vmi1 = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(vmi1);
    const VmInterface::FloatingIpSet &fip_list1 =
        vmi1->floating_ip_list().list_;
    EXPECT_EQ(fip_list1.size(), 1);

    DelLink("floating-ip", "fip1", "instance-ip", "instance1");
    DelLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    DelFloatingIp("fip1");

    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);

    //Check that corresponding objects were deleted
    EXPECT_EQ(fip_list1.size(), 0);

    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
}

//test with "INGRESS" direction
TEST_F(VxlanRoutingTest, fip_test_initial_conf_with_lr2) {

    IpamInfo ipam_info1[] = {
    {"1.1.1.0", 24, "1.1.1.1"},
    };

    IpamInfo ipam_info2[] = {
    {"2.2.2.0", 24, "2.2.2.1"},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.29", "00:00:00:01:01:01", 1, 1},
    };

    const char* domestic_addr = "1.1.1.2";
    const char* foreign_addr = "2.2.2.2";
    client->WaitForIdle();
    client->Reset();

    AddIPAM("vn1", ipam_info1, 1);
    CreateVmportFIpEnv(input, 1, 0, "vn1", "vrf1");

    AddLink("virtual-network", "vn1", "instance-ip", "instance1");
    client->WaitForIdle();

    AddLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    client->WaitForIdle();

    AddFloatingIp("fip1", 1, domestic_addr,  input[0].addr, "INGRESS",
                  true, 80);
    AddLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    AddLink("floating-ip", "fip1", "instance-ip", "instance1");
    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.29"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string(domestic_addr), 32,
                    "vnet1", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.29"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string(domestic_addr), 32, false);

    //Check link with the first interface
    VmInterface *vmi1 = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(vmi1);
    const VmInterface::FloatingIpSet &fip_list1 =
        vmi1->floating_ip_list().list_;
    EXPECT_EQ(fip_list1.size(), 1);

    //Check that Inet and Evpn tables of the l3vrf contain
    //routes for a given prefix "domestic_addr"

    DelLink("floating-ip", "fip1", "instance-ip", "instance1");
    DelLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    DelFloatingIp("fip1");

    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);

    //Check that corresponding objects were deleted

    EXPECT_EQ(fip_list1.size(), 0);

    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
}

//test with "EGRESS" direction
TEST_F(VxlanRoutingTest, fip_test_initial_conf_with_lr3) {

    IpamInfo ipam_info1[] = {
    {"1.1.1.0", 24, "1.1.1.1"},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.29", "00:00:00:01:01:01", 1, 1},
    };

    const char* domestic_addr = "1.1.1.2";
    const char* foreign_addr = "2.2.2.2";
    client->WaitForIdle();
    client->Reset();

    AddIPAM("vn1", ipam_info1, 1);
    CreateVmportFIpEnv(input, 1, 0, "vn1", "vrf1");

    AddLink("virtual-network", "vn1", "instance-ip", "instance1");
    client->WaitForIdle();

    AddLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    client->WaitForIdle();

    AddFloatingIp("fip1", 1, domestic_addr,  input[0].addr, "EGRESS",
                  true, 80);
    AddLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    AddLink("floating-ip", "fip1", "instance-ip", "instance1");
    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.29"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string(domestic_addr), 32,
                    "vnet1", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.29"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string(domestic_addr), 32, false);

    //Check link with the first interface
    VmInterface *vmi1 = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(vmi1);
    const VmInterface::FloatingIpSet &fip_list1 =
        vmi1->floating_ip_list().list_;
    EXPECT_EQ(fip_list1.size(), 1);

    DelLink("floating-ip", "fip1", "instance-ip", "instance1");
    DelLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    DelFloatingIp("fip1");

    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);

    //Check that corresponding objects were deleted
    EXPECT_EQ(fip_list1.size(), 0);

    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
}

//test with "BOTH" direction and 2 vrfs
TEST_F(VxlanRoutingTest, fip_test_2vrf_conf_with_lr1) {

    IpamInfo ipam_info1[] = {
    {"1.1.1.0", 24, "1.1.1.1"},
    };

    IpamInfo ipam_info2[] = {
    {"2.2.2.0", 24, "2.2.2.1"},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.29", "00:00:00:01:01:01", 1, 1},
    };

    const char* domestic_addr = "1.1.1.2";
    const char* foreign_addr = "2.2.2.2";
    client->WaitForIdle();
    client->Reset();

    AddIPAM("vn1", ipam_info1, 1);
    CreateVmportFIpEnv(input, 1, 0, "vn1", "vrf1");

    AddLink("virtual-network", "vn1", "instance-ip", "instance1");
    client->WaitForIdle();

    AddVrf("vrf2");
    AddVn("vn2", 2);
    AddIPAM("vn2", ipam_info2, 1);
    AddActiveActiveInstanceIp("instance2", 2, foreign_addr);

    AddLink("virtual-network", "vn2", "routing-instance", "vrf2");
    AddLink("virtual-network", "vn2", "instance-ip", "instance2");

    AddLrVmiPort("lr-vmi-vn2", 101, "2.2.2.101", "vrf2", "vn2",
                    "instance_ip_2", 2);
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn2", 1);
    client->WaitForIdle();

    AddFloatingIp("fip1", 1, foreign_addr,  input[0].addr, "BOTH", true, 80);
    AddLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    AddLink("floating-ip", "fip1", "instance-ip", "instance2");
    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.29"), 32,
                    "vnet1", false);
    ValidateRouting("l3evpn_1", Ip4Address::from_string(foreign_addr), 32,
                    "vnet1", true, "vn2");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.29"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string(foreign_addr), 32, false);

    //Check link with the first interface
    VmInterface *vmi1 = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(vmi1);
    const VmInterface::FloatingIpSet &fip_list1 =
        vmi1->floating_ip_list().list_;
    EXPECT_EQ(fip_list1.size(), 1);

    DelLink("floating-ip", "fip1", "instance-ip", "instance2");
    DelLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    DelFloatingIp("fip1");

    DelLrBridgeVrf("vn2", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn2", 101, "2.2.2.101", "vrf2", "vn2",
                    "instance_ip_2", 2);

    DelLink("virtual-network", "vn2", "routing-instance", "vrf2");
    DelLink("virtual-network", "vn2", "instance-ip", "instance2");

    DelInstanceIp("instance2");
    DelIPAM("vn2");
    DelVn("vn2");
    DelVrf("vrf2");

    //Check that corresponding objects were deleted
    EXPECT_EQ(fip_list1.size(), 0);

    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
}

//aap
TEST_F(VxlanRoutingTest, Aap_l3) {
    using boost::uuids::nil_uuid;
    struct PortInfo input[] = {
        {"intf1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1, "fd10::2"},
        {"intf2", 2, "1.1.1.2", "00:00:00:01:01:02", 1, 2, "fd10::3"},
    };
    IpamInfo ipam_info[] = {
        {"1.1.1.0", 24, "1.1.1.10", true},
    };

    client->WaitForIdle();

    AddIPAM("vn1", ipam_info, 1);
    CreateVmportEnv(input, INPUT_SIZE(input));
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    client->WaitForIdle();
    EXPECT_TRUE(VmPortActive(input, 0));
    EXPECT_TRUE(VmPortActive(1));
    EXPECT_TRUE(VmPortActive(2));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "intf1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.2"), 32,
                    "intf2", true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.1"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.2"), 32, false);

    Ip4Address ip = Ip4Address::from_string("10.10.10.10");
    std::vector<Ip4Address> v;
    v.push_back(ip);

    AddAap("intf1", 1, v);
    ValidateRouting("l3evpn_1", v[0], 32,
                    "intf1", true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                   v[0], 32, false);

    v.clear();
    AddAap("intf1", 1, v);
    EXPECT_FALSE(RouteFind("vrf1", ip, 32));

    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFindRetDel(1));
    EXPECT_FALSE(VrfFind("vrf1", true));
    client->WaitForIdle();
    DelIPAM("vn1");
    client->WaitForIdle();
}

//Check if subnet gateway for allowed address pait route gets set properly
// aap l3l2
TEST_F(VxlanRoutingTest, Aap_l3l2) {
    using boost::uuids::nil_uuid;
    struct PortInfo input[] = {
        {"intf1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1, "fd10::2"},
        {"intf2", 2, "1.1.1.2", "00:00:00:01:01:02", 1, 2, "fd10::3"},
    };
    IpamInfo ipam_info[] = {
        {"1.1.1.0", 24, "1.1.1.10", true},
        {"2.2.2.0", 24, "2.2.2.10", true}
    };

    client->WaitForIdle();

    AddIPAM("vn1", ipam_info, 2);
    CreateVmportEnv(input, INPUT_SIZE(input));
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    client->WaitForIdle();
    EXPECT_TRUE(VmPortActive(input, 0));
    EXPECT_TRUE(VmPortActive(1));
    EXPECT_TRUE(VmPortActive(2));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "intf1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.2"), 32,
                    "intf2", true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.1"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.2"), 32, false);

    Ip4Address ip = Ip4Address::from_string("10.10.10.10");
    MacAddress mac("0a:0b:0c:0d:0e:0f");

    VmInterface *vm_intf = static_cast<VmInterface *>(VmPortGet(1));
    AddAap("intf1", 1, ip, mac.ToString());
    ValidateRouting("l3evpn_1", ip, 32, "intf1", true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1", ip, 32, false);

    EXPECT_TRUE(vm_intf->allowed_address_pair_list().list_.size() == 1);

    InetUnicastRouteEntry *rt = RouteGet("vrf1", ip, 32);
    uint32_t label = rt->GetActivePath()->label();
    EXPECT_TRUE(label != vm_intf->label());
    const InterfaceNH *intf_nh =
        dynamic_cast<const InterfaceNH *>(rt->GetActiveNextHop());
    EXPECT_TRUE(intf_nh->GetDMac() == mac);

    AddAap("intf1", 1, Ip4Address(0), zero_mac.ToString());
    ValidateRouting("l3evpn_1", ip, 32, "intf1", false);
    ValidateBridge("vrf1", "l3evpn_1", ip, 32, false);

    EXPECT_TRUE(vm_intf->allowed_address_pair_list().list_.size() == 0);
    EXPECT_TRUE(agent_->mpls_table()->FindMplsLabel(label) == nullptr);

    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFindRetDel(1));
    EXPECT_FALSE(VrfFind("vrf1", true));
    client->WaitForIdle();
    DelIPAM("vn1");
    client->WaitForIdle();
}

//Composite interface routes
TEST_F(VxlanRoutingTest, Composite_two_interfaces) {
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };

     struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.1", "00:00:00:02:02:01", 1, 2},
    };

    AddIPAM("vn1", ipam, 1);
    CreateVmportWithEcmp(input, 2);
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    EXPECT_TRUE(VmPortActive(input, 0));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);

    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet2", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.1"), 32, false);

    InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32);
    const CompositeNH *comp_nh =
        dynamic_cast<const CompositeNH *>
            (lr_vrf_rt->GetActiveNextHop());
    EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
    EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
    const InterfaceNH *nh_nh = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[0]->nh());
    EXPECT_TRUE(nh_nh != nullptr);
    const InterfaceNH *nh_nh2 = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[1]->nh());
     EXPECT_TRUE(nh_nh2 != nullptr);

    // cleanup
    client->WaitForIdle();
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    //DeleteEnvironment(true);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_TRUE (VrfGet("l3evpn_1") == nullptr);
}

// Composite tunnel routes
TEST_F(VxlanRoutingTest, Composite_two_tunnels) {
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam, 1);
    CreateVmportEnv(input, INPUT_SIZE(input));
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.1"), 32,
            "vnet1", true, "vn1");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.1"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // Test Type 5 route add/del in LR vrf
    stringstream ss_node;
    autogen::EnetItemType item;
    SecurityGroupList sg;
    item.entry.nlri.af = BgpAf::L2Vpn;
    item.entry.nlri.safi = BgpAf::Enet;
    item.entry.nlri.address = "10.10.10.10";
    item.entry.nlri.ethernet_tag = 0;
    autogen::EnetNextHopType nh, nh2;
    nh.af = Address::INET;
    nh.address = "3.3.3.3";
    nh.label = routing_vrf->vxlan_id();
    nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    item.entry.next_hops.next_hop.push_back(nh);
    nh2.af = Address::INET;
    nh2.address = "2.2.2.2";
    nh2.label = routing_vrf->vxlan_id();
    nh2.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    item.entry.next_hops.next_hop.push_back(nh2);
    item.entry.med = 0;

    // Add type5 route 10.10.10.10/32 to lr evpn table
    VnListType vn_list;
    vn_list.insert(item.entry.virtual_network);
    bgp_peer_->GetAgentXmppChannel()->AddEvpnEcmpRoute(
        routing_vrf_name, MacAddress::FromString("00:00:00:00:00:00"),
        Ip4Address::from_string("10.10.10.10"), 32, &item, vn_list);
    client->WaitForIdle();

    // Validate route is copied to bridge vrf
    InetUnicastRouteEntry *rt1 =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE( rt1 != nullptr);

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet(routing_vrf_name, Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE( lr_vrf_rt != nullptr);
    EXPECT_TRUE(
        lr_vrf_rt->GetActivePath()->vxlan_id() == routing_vrf->vxlan_id());
    const CompositeNH *comp_nh =
        dynamic_cast<const CompositeNH *>
            (lr_vrf_rt->GetActiveNextHop());
    EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
    EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
    const TunnelNH *nh_nh = dynamic_cast<const TunnelNH *>
        (comp_nh->component_nh_list()[0]->nh());
    EXPECT_TRUE(nh_nh != nullptr);
    const TunnelNH *nh_nh2 = dynamic_cast<const TunnelNH *>
        (comp_nh->component_nh_list()[1]->nh());
    EXPECT_TRUE(nh_nh2 != nullptr);

    // Verify route for local vm port is still present
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.1"), 32, false);

    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    EvpnAgentRouteTable *rt_table1 = static_cast<EvpnAgentRouteTable *>
        (agent_->vrf_table()->GetEvpnRouteTable(routing_vrf_name));
    rt_table1->DeleteReq(bgp_peer_,
        routing_vrf_name,
        MacAddress::FromString("00:00:00:00:00:00"),
        Ip4Address::from_string("10.10.10.10"),
        32, 0,
        new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    // Clean up
    client->WaitForIdle();

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);

    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
            IsEmpty());
    client->WaitForIdle();
}

// Composite tunnel and interface routes
TEST_F(VxlanRoutingTest, Composite_tunnels_and_interfaces) {
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam, 1);
    CreateVmportWithEcmp(input, 1);
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.1"), 32,
            "vnet1", true, "vn1");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.1"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // Test Type 5 route add/del in LR vrf
    stringstream ss_node;
    autogen::EnetItemType item;
    SecurityGroupList sg;
    item.entry.nlri.af = BgpAf::L2Vpn;
    item.entry.nlri.safi = BgpAf::Enet;
    item.entry.nlri.address = "1.1.1.1/32";
    item.entry.nlri.ethernet_tag = 0;
    autogen::EnetNextHopType nh, nh2;
    nh.af = Address::INET;
    nh.address = "3.3.3.3";
    nh.label = routing_vrf->vxlan_id();
    nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    item.entry.next_hops.next_hop.push_back(nh);
    nh2.af = Address::INET;
    nh2.address = "10.1.1.1";
    nh2.label = routing_vrf->vxlan_id();
    nh2.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    item.entry.next_hops.next_hop.push_back(nh2);
    item.entry.med = 0;

    // Add type5 route 1.1.1.1/32 to lr evpn table
    VnListType vn_list;
    vn_list.insert(item.entry.virtual_network);
    bgp_peer_->GetAgentXmppChannel()->AddEvpnEcmpRoute(
        routing_vrf_name, MacAddress::FromString("00:00:00:00:00:00"),
        Ip4Address::from_string("1.1.1.1"), 32, &item, vn_list);
    client->WaitForIdle();

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet(routing_vrf_name, Ip4Address::from_string("1.1.1.1"), 32);
    EXPECT_TRUE( lr_vrf_rt != nullptr);
    if (lr_vrf_rt) {
        const Route::PathList & path_list = lr_vrf_rt->GetPathList();
        for (const auto& path_it : path_list) {
            const AgentPath* path =
                dynamic_cast<const AgentPath*>(&path_it);
            if (!path)
                continue;
            if ((path->nexthop()) &&
                (path->nexthop()->GetType() == NextHop::COMPOSITE)) {
                CompositeNH *comp_nh = dynamic_cast<CompositeNH *>
                     (path->nexthop());
                EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
                EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
                EXPECT_TRUE(comp_nh->PolicyEnabled() == true);
                if (comp_nh->component_nh_list()[0] != nullptr)
                {
                    const InterfaceNH *nh_nh =
                        dynamic_cast<const InterfaceNH *>
                            (comp_nh->component_nh_list()[0]->nh());
                    EXPECT_TRUE(comp_nh->component_nh_list()[1] == nullptr);
                }
                else
                {
                    const InterfaceNH *nh_nh2 =
                        dynamic_cast<const InterfaceNH *>
                            (comp_nh->component_nh_list()[1]->nh());
                    EXPECT_TRUE(nh_nh2 != nullptr);
                }
            }
        }
    }
    // Verify route for local vm port is still present
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.1"), 32,
            "vnet1", true);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.1"), 32, false);

    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    EvpnAgentRouteTable *rt_table1 = static_cast<EvpnAgentRouteTable *>
            (agent_->vrf_table()->GetEvpnRouteTable(routing_vrf_name));
    rt_table1->DeleteReq(bgp_peer_,
        routing_vrf_name,
        MacAddress::FromString("00:00:00:00:00:00"),
        Ip4Address::from_string("1.1.1.1"),
        32, 0,
        new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    // Clean up
    client->WaitForIdle();

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);

    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
            IsEmpty());
    client->WaitForIdle();
}

//Composite aap interface routes
TEST_F(VxlanRoutingTest, Composite_two_interfaces_aap_l3l2) {
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };

     struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.1", "00:00:00:02:02:01", 1, 2},
    };

    AddIPAM("vn1", ipam, 1);
    CreateVmportWithEcmp(input, 2);
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    EXPECT_TRUE(VmPortActive(input, 0));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);

    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet2", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.1"), 32, false);

    Ip4Address ip1 = Ip4Address::from_string("10.10.10.10");
    Ip4Address ip2 = Ip4Address::from_string("10.10.10.11");
    std::vector<Ip4Address> v;
    v.push_back(ip1);

    AddEcmpAap("vnet1", 1, ip1, "00:00:00:01:01:01");
    AddEcmpAap("vnet2", 2, ip2, "00:00:00:01:01:02");
    EXPECT_TRUE(RouteFind("vrf1", ip1, 32));
    EXPECT_TRUE(RouteFind("vrf1", ip2, 32));

     InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32);
    const CompositeNH *comp_nh =
        dynamic_cast<const CompositeNH *>
            (lr_vrf_rt->GetActiveNextHop());
    EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
    EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
    const InterfaceNH *nh_nh = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[0]->nh());
    EXPECT_TRUE(nh_nh != nullptr);
    const InterfaceNH *nh_nh2 = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[1]->nh());
    EXPECT_TRUE(nh_nh2 != nullptr);

    v.clear();
    AddAap("vnet1", 1, v);
    AddAap("vnet2", 2, v);

    // cleanup
    client->WaitForIdle();
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    //DeleteEnvironment(true);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_TRUE (VrfGet("l3evpn_1") == nullptr);
}

TEST_F(VxlanRoutingTest, Composite_two_interfaces_aap_l3) {
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };

     struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.1", "00:00:00:02:02:01", 1, 2},
    };

    AddIPAM("vn1", ipam, 1);
    CreateVmportWithEcmp(input, 2);
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    EXPECT_TRUE(VmPortActive(input, 0));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);

    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet2", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.1"), 32, false);

    Ip4Address ip1 = Ip4Address::from_string("1.1.1.1");
    Ip4Address ip2 = Ip4Address::from_string("1.1.1.1");
    std::vector<Ip4Address> v;
    v.push_back(ip1);

    AddEcmpAap("vnet1", 1, ip1, "00:00:00:00:00:00");
    AddEcmpAap("vnet2", 2, ip2, "00:00:00:00:00:00");
    EXPECT_TRUE(RouteFind("vrf1", ip1, 32));
    EXPECT_TRUE(RouteFind("vrf1", ip2, 32));

     InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32);
    const CompositeNH *comp_nh =
        dynamic_cast<const CompositeNH *>
            (lr_vrf_rt->GetActiveNextHop());
    EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
    EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
    const InterfaceNH *nh_nh = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[0]->nh());
    EXPECT_TRUE(nh_nh != nullptr);
    const InterfaceNH *nh_nh2 = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[1]->nh());
    EXPECT_TRUE(nh_nh2 != nullptr);

    v.clear();
    AddAap("vnet1", 1, v);
    AddAap("vnet2", 2, v);

    // cleanup
    client->WaitForIdle();
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_TRUE (VrfGet("l3evpn_1") == nullptr);
}

//Composite irt interface routes
TEST_F(VxlanRoutingTest, Composite_two_interfaces_irt) {
    using boost::uuids::nil_uuid;

    IpamInfo ipam[] = {
    {"1.1.1.0", 24, "1.1.1.200", true},
    };

     struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.1", "00:00:00:02:02:01", 1, 2},
    };

    AddIPAM("vn1", ipam, 1);
    CreateVmportWithEcmp(input, 2);
    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    EXPECT_TRUE(VmPortActive(input, 0));
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);

    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet2", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.1"), 32, false);

    //Add a static route
    struct TestIp4Prefix static_route[] = {
       { Ip4Address::from_string("24.1.1.0"), 24},
       { Ip4Address::from_string("16.1.1.0"), 16},
    };

    AddInterfaceRouteTable("static_route", 1, static_route, 2);

    AddLink("virtual-machine-interface", "vnet1",
           "interface-route-table", "static_route");
    AddLink("virtual-machine-interface", "vnet2",
           "interface-route-table", "static_route");

    client->WaitForIdle();
    EXPECT_TRUE(RouteFind("vrf1", static_route[0].addr_,
                         static_route[0].plen_));
    EXPECT_TRUE(RouteFind("vrf1", static_route[1].addr_,
                         static_route[1].plen_));
    InetUnicastRouteEntry *rt =
        RouteGet("vrf1", static_route[0].addr_, static_route[0].plen_);

    EXPECT_TRUE(
        rt->intf_route_type().compare(VmInterface::kInterfaceStatic) == 0);

    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32,
                    "vnet1",  true, "vn1");
    ValidateBridge("vrf1", "l3evpn_1",
                   Ip4Address::from_string("1.1.1.1"), 32, false);

     InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet("l3evpn_1", Ip4Address::from_string("1.1.1.1"), 32);
   const CompositeNH *comp_nh =
        dynamic_cast<const CompositeNH *>
            (lr_vrf_rt->GetActiveNextHop());
    EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
    EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
    const InterfaceNH *nh_nh = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[0]->nh());
    EXPECT_TRUE(nh_nh != nullptr);
    const InterfaceNH *nh_nh2 = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[1]->nh());
     EXPECT_TRUE(nh_nh2 != nullptr);

    // cleanup
    client->WaitForIdle();
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                "instance_ip_1", 1);
    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_TRUE (VrfGet("l3evpn_1") == nullptr);
}

//composite fip route
TEST_F(VxlanRoutingTest, Composite_two_interfaces_fip) {

    IpamInfo ipam_info1[] = {
    {"1.1.1.0", 24, "1.1.1.1"},
    };

    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.19", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.29", "00:00:00:01:01:01", 1, 2}
    };

    const char* domestic_addr = "1.1.1.2";
    const char* foreign_addr = "2.2.2.2";
    client->WaitForIdle();
    client->Reset();

    AddIPAM("vn1", ipam_info1, 1);
    CreateVmportFIpEnv(input, 2, 0, "vn1", "vrf1");

    AddLink("virtual-network", "vn1", "instance-ip", "instance1");

    AddLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    client->WaitForIdle();

    AddFloatingIp("fip1", 1, domestic_addr,  input[0].addr, "BOTH", true, 80);
    AddLink("virtual-machine-interface", input[0].name, "floating-ip", "fip1");
    AddLink("floating-ip", "fip1", "instance-ip", "instance1");
    AddFloatingIp("fip2", 2, domestic_addr,  input[1].addr, "BOTH", true, 80);
    AddLink("virtual-machine-interface", input[1].name, "floating-ip", "fip2");
    AddLink("floating-ip", "fip2", "instance-ip", "instance1");
    client->WaitForIdle();

    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.19"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string("1.1.1.29"), 32,
                    "vnet2", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string(domestic_addr), 32,
                    "vnet1", true, "vn1");
    ValidateRouting("l3evpn_1", Ip4Address::from_string(domestic_addr), 32,
                    "vnet2", true, "vn1");
    // VrfNH paths are not added anymore for bridge host routes
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.19"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.29"), 32, false);
    ValidateBridge("vrf1", "l3evpn_1",
                    Ip4Address::from_string(domestic_addr), 32, false);

    //Check link with the first interface
    VmInterface *vmi1 = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(vmi1);
    const VmInterface::FloatingIpSet &fip_list1 =
        vmi1->floating_ip_list().list_;
    EXPECT_EQ(fip_list1.size(), 1);

    //Check link with the second interface
    VmInterface *vmi2 = static_cast<VmInterface *>(VmPortGet(2));
    EXPECT_TRUE(vmi2);
    const VmInterface::FloatingIpSet &fip_list2 =
        vmi2->floating_ip_list().list_;
    EXPECT_EQ(fip_list2.size(), 1);

     InetUnicastRouteEntry *lr_vrf_rt =
        RouteGet("l3evpn_1", Ip4Address::from_string("1.1.1.2"), 32);
    const CompositeNH *comp_nh =
        dynamic_cast<const CompositeNH *>
            (lr_vrf_rt->GetActiveNextHop());
    EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
    EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
    const InterfaceNH *nh_nh = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[0]->nh());
    EXPECT_TRUE(nh_nh != nullptr);
    const InterfaceNH *nh_nh2 = dynamic_cast<const InterfaceNH *>
        (comp_nh->component_nh_list()[1]->nh());
     EXPECT_TRUE(nh_nh2 != nullptr);

    DelLink("floating-ip", "fip1", "instance-ip", "instance1");
    DelLink("floating-ip", "fip2", "instance-ip", "instance1");
    DelLink("virtual-machine-interface", input[1].name, "floating-ip", "fip1");
    DelLink("virtual-machine-interface", input[0].name, "floating-ip", "fip2");
    DelFloatingIp("fip1");

    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 101, "1.1.1.101", "vrf1", "vn1",
        "instance_ip_1", 1);

    //Check that corresponding objects were deleted
    EXPECT_EQ(fip_list1.size(), 0);
    EXPECT_EQ(fip_list2.size(), 0);

    DeleteVmportEnv(input, INPUT_SIZE(input), true);
    DelIPAM("vn1");
}

TEST_F(VxlanRoutingTest, Add_del_network) {
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 2, "2.2.2.20", "00:00:02:02:02:20", 2, 2},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    struct PortInfo input3[] = {
        {"vnet3", 3, "3.3.3.30", "00:00:03:03:03:30", 3, 3},
    };
    IpamInfo ipam_info_3[] = {
        {"3.3.3.0", 24, "3.3.3.200", true},
    };

    struct PortInfo input4[] = {
        {"vnet4", 4, "4.4.4.40", "00:00:04:04:04:40", 4, 4},
    };
    IpamInfo ipam_info_4[] = {
        {"4.4.4.0", 24, "4.4.4.200", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);
    AddIPAM("vn3", ipam_info_3, 1);

    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));
    CreateVmportEnv(input3, INPUT_SIZE(input3));

    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    AddLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    AddLrBridgeVrf("vn3", 1);
    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(3)->logical_router_uuid() == nil_uuid());

    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(93)->logical_router_uuid() != nil_uuid());

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("3.3.3.30"), 32,
            "vnet3", true, "vn3");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("3.3.3.30"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // check to see if the subnet route for vn3 added to the bridge vrf1 inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, true);
    client->WaitForIdle();

    DelLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    client->WaitForIdle();
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("3.3.3.30"), 32,
            "vnet3", false);
    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("3.3.3.30"), 32, false);
    // check to see if the subnet route for vn added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, false);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, false);

    AddIPAM("vn4", ipam_info_4, 1);
    CreateVmportEnv(input4, INPUT_SIZE(input4));
    AddLrVmiPort("lr-vmi-vn4", 94, "4.4.4.99", "vrf4", "vn4",
            "instance_ip_4", 4);
    AddLrBridgeVrf("vn4", 1);
    AddLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);
    EXPECT_TRUE(VmInterfaceGet(4)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(94)->logical_router_uuid() != nil_uuid());
    client->WaitForIdle();

     ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("3.3.3.30"), 32,
            "vnet3", true, "vn3");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("4.4.4.40"), 32,
            "vnet4", true, "vn4");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("3.3.3.30"), 32, false);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("4.4.4.40"), 32, false);

    // check to see if the subnet route for vn3 added to the bridge vrf1 inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("4.4.4.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("4.4.4.0"), 24, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("4.4.4.0"), 24, true);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, false);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);
    client->WaitForIdle();

    // Bridge VN3
    DelLrBridgeVrf("vn3", 1);
    DelLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);
    DeleteVmportEnv(input3, INPUT_SIZE(input3), true);
    DelIPAM("vn3");

    DelLrBridgeVrf("vn4", 1);
    DelLrVmiPort("lr-vmi-vn4", 94, "4.4.4.99", "vrf4", "vn4",
            "instance_ip_4", 4);
    DeleteVmportEnv(input4, INPUT_SIZE(input4), true);
    DelIPAM("vn4");

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    DelIPAM("vn1");
    DelIPAM("vn2");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(VrfGet("vrf2") == nullptr);
    EXPECT_TRUE(VrfGet("vrf3") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
            IsEmpty());
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
            IsEmpty());
    client->WaitForIdle();
}

TEST_F(VxlanRoutingTest, AddDelIPAM) {
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 2, "2.2.2.20", "00:00:02:02:02:20", 2, 2},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    struct PortInfo input3[] = {
        {"vnet3", 3, "3.3.3.30", "00:00:03:03:03:30", 3, 3},
    };
    IpamInfo ipam_info_3[] = {
        {"3.3.3.0", 24, "3.3.3.200", true},
    };
    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);
    AddIPAM("vn3", ipam_info_3, 1);

    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));
    CreateVmportEnv(input3, INPUT_SIZE(input3));

    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    AddLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    AddLrBridgeVrf("vn3", 1);

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(3)->logical_router_uuid() == nil_uuid());

    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(93)->logical_router_uuid() != nil_uuid());

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("3.3.3.30"), 32,
            "vnet3", true, "vn3");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("3.3.3.30"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // check to see if the subnet route for vn3 added to the bridge vrf1 inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, true);
    client->WaitForIdle();

    // Add one subnet to existing ipam default-network-ipam,vn1
    IpamInfo ipam_update[] = {
        {"1.1.1.0", 24, "1.1.1.200", true},
        {"5.5.5.0", 24, "5.5.5.200", true},
    };
    AddIPAM("vn1", ipam_update, 2);
    client->WaitForIdle();

    // Validate both subnet routes for vn1 in vn2
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("5.5.5.0"), 24, true);
    ValidateBridge("vrf3", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf3", "l3evpn_1",
                    Ip4Address::from_string("5.5.5.0"), 24, true);

    // Delete 5.5.5.0/24 subnet from ipam default-network-ipam,vn1
    AddIPAM("vn1", ipam_info_1, 1);
    client->WaitForIdle();
    // Validate vn2 has subnet route only for 1.1.1.0/24 and not 5.5.5.0/24
    ValidateBridge("vrf2", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, true);
    InetUnicastRouteEntry *rt =
        RouteGet("vrf2", Ip4Address::from_string("5.5.5.0"), 24);
    EXPECT_TRUE(rt == nullptr);
    ValidateBridge("vrf3", "l3evpn_1",
                    Ip4Address::from_string("1.1.1.0"), 24, true);
    InetUnicastRouteEntry *rt2 =
        RouteGet("vrf3", Ip4Address::from_string("5.5.5.0"), 24);
    EXPECT_TRUE(rt == nullptr);

    DelIPAM("vn1");
    client->WaitForIdle();

    // Verify both subnet routes for vn1 get deleted in vrf2 inet table
    InetUnicastRouteEntry *rt_sub_1 =
        RouteGet("vrf2", Ip4Address::from_string("1.1.1.0"), 24);
    EXPECT_TRUE(rt_sub_1 == nullptr);
    InetUnicastRouteEntry *rt_sub_2 =
        RouteGet("vrf2", Ip4Address::from_string("5.5.5.0"), 24);
    EXPECT_TRUE(rt_sub_2 == nullptr);
    InetUnicastRouteEntry *rt_sub_21 =
        RouteGet("vrf3", Ip4Address::from_string("1.1.1.0"), 24);
    EXPECT_TRUE(rt_sub_21 == nullptr);
    InetUnicastRouteEntry *rt_sub_22 =
        RouteGet("vrf3", Ip4Address::from_string("5.5.5.0"), 24);
    EXPECT_TRUE(rt_sub_22 == nullptr);
    client->WaitForIdle();
    DelLrBridgeVrf("vn3", 1);
    DelLrBridgeVrf("vn2", 1);
    DelLrBridgeVrf("vn1", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    DeleteVmportEnv(input3, INPUT_SIZE(input3), true);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    DelIPAM("vn1");
    DelIPAM("vn2");
    DelIPAM("vn3");
    DeleteEnvironment(true);
}

TEST_F(VxlanRoutingTest, Add_del_network_policy_2_networks) {
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 2, "2.2.2.20", "00:00:02:02:02:20", 2, 2},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);

    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));

    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());

    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", true, "vn2");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // check to see if the subnet route added to the bridge vrf
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);

    // AddLocalVmRouteReq
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    VnListType vn_list;
    vn_list.insert("vn1");
    PathPreference path_preference;
    EcmpLoadBalance ecmp_load_balance;
    intf->SetPathPreference(&path_preference, false, Ip4Address(0));
    InetUnicastAgentRouteTable *table2 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf2"));
    table2->AddLocalVmRouteReq(
        bgp_peer_, "vrf2", Ip4Address::from_string("1.1.1.10"), 32,
        intf->GetUuid(), vn_list, intf->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf->name());
    client->WaitForIdle();

    // Verify type5 route added to Lr evpn table is copied to bridge vrf
    InetUnicastRouteEntry *rt_vrf2 =
        RouteGet("vrf2", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_vrf2 != nullptr);
    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", true, "vn2");

    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    table2->DeleteReq(bgp_peer_, "vrf2",
        Ip4Address::from_string("1.1.1.10"), 32,
        new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    InetUnicastRouteEntry *rt_del =
        RouteGet("vrf2", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_del == nullptr);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", true, "vn2");

    // Clean up
    client->WaitForIdle();

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrBridgeVrf("vn2", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    DelIPAM("vn1");
    DelIPAM("vn2");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(VrfGet("vrf2") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
            IsEmpty());
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
            IsEmpty());
    client->WaitForIdle();
}

TEST_F(VxlanRoutingTest, Add_del_network_policy_4_networks) {
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 2, "2.2.2.20", "00:00:02:02:02:20", 2, 2},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    struct PortInfo input3[] = {
        {"vnet3", 3, "3.3.3.30", "00:00:03:03:03:30", 3, 3},
    };
    IpamInfo ipam_info_3[] = {
        {"3.3.3.0", 24, "3.3.3.200", true},
    };

    struct PortInfo input4[] = {
        {"vnet4", 4, "4.4.4.40", "00:00:04:04:04:40", 4, 4},
    };
    IpamInfo ipam_info_4[] = {
        {"4.4.4.0", 24, "4.4.4.200", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);
    AddIPAM("vn3", ipam_info_3, 1);
    AddIPAM("vn4", ipam_info_4, 1);

    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));
    CreateVmportEnv(input3, INPUT_SIZE(input3));
    CreateVmportEnv(input4, INPUT_SIZE(input4));

    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    AddLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);
    AddLrVmiPort("lr-vmi-vn4", 94, "4.4.4.99", "vrf4", "vn4",
            "instance_ip_4", 4);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    AddLrBridgeVrf("vn3", 1);
    AddLrBridgeVrf("vn4", 1);
    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(3)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(4)->logical_router_uuid() == nil_uuid());

    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(93)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(94)->logical_router_uuid() != nil_uuid());

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("3.3.3.30"), 32,
            "vnet3", true, "vn3");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("4.4.4.40"), 32,
            "vnet4", true, "vn4");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("3.3.3.30"), 32, false);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("4.4.4.40"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // check to see if the subnet route for vn3 added to the bridge vrf1 inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("4.4.4.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("4.4.4.0"), 24, true);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("4.4.4.0"), 24, true);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("1.1.1.0"), 24, true);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("3.3.3.0"), 24, true);

    // AddLocalVmRouteReq
    VmInterface *intf1 = static_cast<VmInterface *>(VmPortGet(1));
    VnListType vn_list1;
    vn_list1.insert("vn1");
    VmInterface *intf2 = static_cast<VmInterface *>(VmPortGet(2));
    VnListType vn_list2;
    vn_list2.insert("vn2");
    VmInterface *intf3 = static_cast<VmInterface *>(VmPortGet(3));
    VnListType vn_list3;
    vn_list3.insert("vn3");
    VmInterface *intf4 = static_cast<VmInterface *>(VmPortGet(4));
    VnListType vn_list4;
    vn_list4.insert("vn4");
    PathPreference path_preference;
    EcmpLoadBalance ecmp_load_balance;
    intf1->SetPathPreference(&path_preference, false, Ip4Address(0));
    intf2->SetPathPreference(&path_preference, false, Ip4Address(0));
    intf3->SetPathPreference(&path_preference, false, Ip4Address(0));
    intf4->SetPathPreference(&path_preference, false, Ip4Address(0));
    InetUnicastAgentRouteTable *table1 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf1"));
    InetUnicastAgentRouteTable *table2 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf2"));
    InetUnicastAgentRouteTable *table3 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf3"));
    InetUnicastAgentRouteTable *table4 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf4"));
    table1->AddLocalVmRouteReq(
        bgp_peer_, "vrf1", Ip4Address::from_string("2.2.2.20"), 32,
        intf2->GetUuid(), vn_list2, intf2->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf2->name());
    table1->AddLocalVmRouteReq(
        bgp_peer_, "vrf1", Ip4Address::from_string("3.3.3.30"), 32,
        intf3->GetUuid(), vn_list3, intf3->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf3->name());
    table1->AddLocalVmRouteReq(
        bgp_peer_, "vrf1", Ip4Address::from_string("4.4.4.40"), 32,
        intf4->GetUuid(), vn_list4, intf4->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf4->name());
    table2->AddLocalVmRouteReq(
        bgp_peer_, "vrf2", Ip4Address::from_string("1.1.1.10"), 32,
        intf1->GetUuid(), vn_list1, intf1->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf1->name());
    table2->AddLocalVmRouteReq(
        bgp_peer_, "vrf2", Ip4Address::from_string("3.3.3.30"), 32,
        intf3->GetUuid(), vn_list3, intf3->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf3->name());
    table2->AddLocalVmRouteReq(
        bgp_peer_, "vrf2", Ip4Address::from_string("4.4.4.40"), 32,
        intf4->GetUuid(), vn_list4, intf4->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf4->name());
    table3->AddLocalVmRouteReq(
        bgp_peer_, "vrf3", Ip4Address::from_string("1.1.1.10"), 32,
        intf1->GetUuid(), vn_list1, intf1->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf1->name());
    table3->AddLocalVmRouteReq(
        bgp_peer_, "vrf3", Ip4Address::from_string("2.2.2.20"), 32,
        intf2->GetUuid(), vn_list2, intf2->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf2->name());
    table3->AddLocalVmRouteReq(
        bgp_peer_, "vrf3", Ip4Address::from_string("4.4.4.40"), 32,
        intf4->GetUuid(), vn_list4, intf4->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf4->name());
    table4->AddLocalVmRouteReq(
        bgp_peer_, "vrf4", Ip4Address::from_string("1.1.1.10"), 32,
        intf1->GetUuid(), vn_list1, intf1->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf1->name());
    table4->AddLocalVmRouteReq(
        bgp_peer_, "vrf4", Ip4Address::from_string("2.2.2.20"), 32,
        intf2->GetUuid(), vn_list2, intf2->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf2->name());
    table4->AddLocalVmRouteReq(
        bgp_peer_, "vrf4", Ip4Address::from_string("3.3.3.30"), 32,
        intf3->GetUuid(), vn_list3, intf3->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf3->name());
    client->WaitForIdle();

    // Verify type5 route added to Lr evpn table is copied to bridge vrf
    InetUnicastRouteEntry *rt_vrf12 =
        RouteGet("vrf1", Ip4Address::from_string("2.2.2.20"), 32);
    EXPECT_TRUE(rt_vrf12 != nullptr);
    InetUnicastRouteEntry *rt_vrf13 =
        RouteGet("vrf1", Ip4Address::from_string("3.3.3.30"), 32);
    EXPECT_TRUE(rt_vrf13 != nullptr);
    InetUnicastRouteEntry *rt_vrf14 =
        RouteGet("vrf1", Ip4Address::from_string("4.4.4.40"), 32);
    EXPECT_TRUE(rt_vrf14 != nullptr);
    InetUnicastRouteEntry *rt_vrf21 =
        RouteGet("vrf2", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_vrf21 != nullptr);
    InetUnicastRouteEntry *rt_vrf23 =
        RouteGet("vrf2", Ip4Address::from_string("3.3.3.30"), 32);
    EXPECT_TRUE(rt_vrf23 != nullptr);
    InetUnicastRouteEntry *rt_vrf24 =
        RouteGet("vrf2", Ip4Address::from_string("4.4.4.40"), 32);
    EXPECT_TRUE(rt_vrf24 != nullptr);
    InetUnicastRouteEntry *rt_vrf31 =
        RouteGet("vrf3", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_vrf31 != nullptr);
    InetUnicastRouteEntry *rt_vrf32 =
        RouteGet("vrf3", Ip4Address::from_string("2.2.2.20"), 32);
    EXPECT_TRUE(rt_vrf32 != nullptr);
    InetUnicastRouteEntry *rt_vrf34 =
        RouteGet("vrf3", Ip4Address::from_string("4.4.4.40"), 32);
    EXPECT_TRUE(rt_vrf34 != nullptr);
    InetUnicastRouteEntry *rt_vrf41 =
        RouteGet("vrf4", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_vrf41 != nullptr);
    InetUnicastRouteEntry *rt_vrf42 =
        RouteGet("vrf4", Ip4Address::from_string("2.2.2.20"), 32);
    EXPECT_TRUE(rt_vrf42 != nullptr);
    InetUnicastRouteEntry *rt_vrf43 =
        RouteGet("vrf4", Ip4Address::from_string("3.3.3.30"), 32);
    EXPECT_TRUE(rt_vrf43 != nullptr);
    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("3.3.3.30"), 32,
            "vnet3", true, "vn3");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("4.4.4.40"), 32,
            "vnet4", true, "vn4");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("3.3.3.30"), 32, false);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("4.4.4.40"), 32, false);

    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    table1->DeleteReq(bgp_peer_, "vrf1",
        Ip4Address::from_string("2.2.2.20"), 32,
        new ControllerVmRoute(bgp_peer_));
    table1->DeleteReq(bgp_peer_, "vrf1",
        Ip4Address::from_string("3.3.3.30"), 32,
        new ControllerVmRoute(bgp_peer_));
    table1->DeleteReq(bgp_peer_, "vrf1",
        Ip4Address::from_string("4.4.4.40"), 32,
        new ControllerVmRoute(bgp_peer_));
    table2->DeleteReq(bgp_peer_, "vrf2",
        Ip4Address::from_string("1.1.1.10"), 32,
        new ControllerVmRoute(bgp_peer_));
    table2->DeleteReq(bgp_peer_, "vrf2",
        Ip4Address::from_string("3.3.3.30"), 32,
        new ControllerVmRoute(bgp_peer_));
    table2->DeleteReq(bgp_peer_, "vrf2",
        Ip4Address::from_string("4.4.4.40"), 32,
        new ControllerVmRoute(bgp_peer_));
    table3->DeleteReq(bgp_peer_, "vrf3",
        Ip4Address::from_string("1.1.1.10"), 32,
        new ControllerVmRoute(bgp_peer_));
    table3->DeleteReq(bgp_peer_, "vrf3",
        Ip4Address::from_string("2.2.2.20"), 32,
        new ControllerVmRoute(bgp_peer_));
    table3->DeleteReq(bgp_peer_, "vrf3",
        Ip4Address::from_string("4.4.4.40"), 32,
        new ControllerVmRoute(bgp_peer_));
    table4->DeleteReq(bgp_peer_, "vrf4",
        Ip4Address::from_string("1.1.1.10"), 32,
        new ControllerVmRoute(bgp_peer_));
    table4->DeleteReq(bgp_peer_, "vrf4",
        Ip4Address::from_string("2.2.2.20"), 32,
        new ControllerVmRoute(bgp_peer_));
    table4->DeleteReq(bgp_peer_, "vrf4",
        Ip4Address::from_string("3.3.3.30"), 32,
        new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    InetUnicastRouteEntry *rt_del_vrf12 =
        RouteGet("vrf1", Ip4Address::from_string("2.2.2.20"), 32);
    EXPECT_TRUE(rt_del_vrf12 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf13 =
        RouteGet("vrf1", Ip4Address::from_string("3.3.3.30"), 32);
    EXPECT_TRUE(rt_del_vrf13 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf14 =
        RouteGet("vrf1", Ip4Address::from_string("4.4.4.40"), 32);
    EXPECT_TRUE(rt_del_vrf14 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf21 =
        RouteGet("vrf2", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_del_vrf21 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf23 =
        RouteGet("vrf2", Ip4Address::from_string("3.3.3.30"), 32);
    EXPECT_TRUE(rt_del_vrf23 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf24 =
        RouteGet("vrf2", Ip4Address::from_string("4.4.4.40"), 32);
    EXPECT_TRUE(rt_del_vrf24 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf31 =
        RouteGet("vrf3", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_del_vrf31 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf32 =
        RouteGet("vrf3", Ip4Address::from_string("2.2.2.20"), 32);
    EXPECT_TRUE(rt_del_vrf32 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf34 =
        RouteGet("vrf3", Ip4Address::from_string("4.4.4.40"), 32);
    EXPECT_TRUE(rt_del_vrf34 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf41 =
        RouteGet("vrf4", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_del_vrf41 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf42 =
        RouteGet("vrf4", Ip4Address::from_string("2.2.2.20"), 32);
    EXPECT_TRUE(rt_del_vrf42 == nullptr);
    InetUnicastRouteEntry *rt_del_vrf43 =
        RouteGet("vrf4", Ip4Address::from_string("3.3.3.30"), 32);
    EXPECT_TRUE(rt_del_vrf43 == nullptr);

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
            "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
            "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("3.3.3.30"), 32,
            "vnet3", true, "vn3");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("4.4.4.40"), 32,
            "vnet4", true, "vn4");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
            Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
            Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf3", routing_vrf_name,
            Ip4Address::from_string("3.3.3.30"), 32, false);
    ValidateBridge("vrf4", routing_vrf_name,
            Ip4Address::from_string("4.4.4.40"), 32, false);

    // Clean up
    client->WaitForIdle();

    // Bridge
    DelLrBridgeVrf("vn1", 1);
    DelLrBridgeVrf("vn2", 1);
    DelLrBridgeVrf("vn3", 1);
    DelLrBridgeVrf("vn4", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
            "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
            "instance_ip_2", 2);
    DelLrVmiPort("lr-vmi-vn3", 93, "3.3.3.99", "vrf3", "vn3",
            "instance_ip_3", 3);
    DelLrVmiPort("lr-vmi-vn4", 94, "4.4.4.99", "vrf4", "vn4",
            "instance_ip_4", 4);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    DeleteVmportEnv(input3, INPUT_SIZE(input3), true);
    DeleteVmportEnv(input4, INPUT_SIZE(input4), true);
    DelIPAM("vn1");
    DelIPAM("vn2");
    DelIPAM("vn3");
    DelIPAM("vn4");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(VrfGet("vrf2") == nullptr);
    EXPECT_TRUE(VrfGet("vrf3") == nullptr);
    EXPECT_TRUE(VrfGet("vrf4") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    client->WaitForIdle();
}

TEST_F(VxlanRoutingTest, Add_del_bgpaas_route_to_bridge_vrf) {
     agent_ = Agent::GetInstance();
    AgentParam *params = agent_->params();
    params->set_bgpaas_max_shared_sessions(4);
    AddBgpaasPortRange(50000, 50512);
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 2, "2.2.2.20", "00:00:02:02:02:20", 2, 2},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);

    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));

    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                 "instance_ip_2", 2);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    client->WaitForIdle();

    AddBgpRouterConfig("127.0.0.1", 0, 179,
                       64512, "ip-fabric", "control-node");
    std::string bgpaas = AddBgpServiceConfig("1.1.1.10", 50000, 179, 1,
                                             "vnet1", "vrf1",
                                             "bgpaas-client", true);
    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());

    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // check to see if the subnet route for vn added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.0"), 24, true);

    // AddLocalVmRouteReq
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    VnListType vn_list;
    vn_list.insert("vn1");
    PathPreference path_preference;
    EcmpLoadBalance ecmp_load_balance;
    intf->SetPathPreference(&path_preference, false, Ip4Address(0));
    InetUnicastAgentRouteTable *table1 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf1"));
    table1->AddLocalVmRouteReq(
        bgp_peer_, "vrf1", Ip4Address::from_string("10.10.10.10"), 32,
        intf->GetUuid(), vn_list, intf->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf->name());
    client->WaitForIdle();

    InetUnicastRouteEntry *bgpaas_rt_vrf1 =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(bgpaas_rt_vrf1 != nullptr);

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("10.10.10.10"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("10.10.10.10"),
                    32, "vnet1", false, "vn1");

    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    table1->DeleteReq(bgp_peer_, "vrf1",
                      Ip4Address::from_string("10.10.10.10"), 32,
                      new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    InetUnicastRouteEntry *rt_del =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del == nullptr);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");

    // Clean up
    client->WaitForIdle();

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrBridgeVrf("vn2", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                 "instance_ip_2", 2);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->vm_table()->Size()));
    DelBgpaasPortRange();
    DeleteBgpRouterConfig("127.0.0.1", 0, "ip-fabric");
    DeleteBgpServiceConfig("1.1.1.10", 50000, "vnet1", "vrf1");
    DelIPAM("vn1");
    DelIPAM("vn2");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(VrfGet("vrf2") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    client->WaitForIdle();
}

TEST_F(VxlanRoutingTest, Add_del_bgpaas_route_to_bridge_and_routing_vrf) {
    agent_ = Agent::GetInstance();
    AgentParam *params = agent_->params();
    params->set_bgpaas_max_shared_sessions(4);
    AddBgpaasPortRange(50000, 50512);
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 2, "2.2.2.20", "00:00:02:02:02:20", 2, 2},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);

    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));

    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                 "instance_ip_2", 2);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    client->WaitForIdle();

    AddBgpRouterConfig("127.0.0.1", 0, 179,
                       64512, "ip-fabric", "control-node");
    std::string bgpaas = AddBgpServiceConfig("1.1.1.10", 50000, 179, 1,
                                             "vnet1", "vrf1",
                                             "bgpaas-client", true);
    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());

    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // check to see if the subnet route for vn added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.0"), 24, true);

    // AddLocalVmRouteReq
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    VnListType vn_list;
    vn_list.insert("vn1");
    PathPreference path_preference;
    EcmpLoadBalance ecmp_load_balance;
    intf->SetPathPreference(&path_preference, false, Ip4Address(0));
    InetUnicastAgentRouteTable *table1 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf1"));
    table1->AddLocalVmRouteReq(
        bgp_peer_, "vrf1", Ip4Address::from_string("10.10.10.10"), 32,
        intf->GetUuid(), vn_list, intf->label(), SecurityGroupList(),
        TagList(), CommunityList(), true, path_preference,
        Ip4Address(0), ecmp_load_balance, false, false, false,
        intf->name());
    client->WaitForIdle();

    InetUnicastRouteEntry *bgpaas_rt_vrf1 =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(bgpaas_rt_vrf1 != nullptr);

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("10.10.10.10"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("10.10.10.10"),
                    32, "vnet1", false, "vn1");

    //Add bgpaas route to evpn routing
    std::vector<std::string> peer_sources;
    peer_sources.push_back("1.1.1.10");
    MacAddress dummy_mac;

    // Add Tunnel BGPaaS
    BgpaasIntfRouteAdd(bgp_peer_, "l3evpn_1", TunnelType::VxlanType(),
                       agent_->router_id(),
                       routing_vrf->vxlan_id(), vn_list,
                       Ip4Address::from_string("10.10.10.10"), 32,
                       peer_sources);

    client->WaitForIdle();

    // Verify type5 route added to Lr evpn table is copied to bridge vrf
    InetUnicastRouteEntry *rt_l3evpn_1 =
        RouteGet("l3evpn_1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_l3evpn_1 != nullptr);

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("10.10.10.10"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("10.10.10.10"),
                    32, "vnet1", true);

    EvpnAgentRouteTable *rt_table =
        static_cast<EvpnAgentRouteTable *>
            (agent_->vrf_table()->GetEvpnRouteTable("l3evpn_1"));
    rt_table->DeleteReq(bgp_peer_, "l3evpn_1", MacAddress(),
                        Ip4Address::from_string("10.10.10.10"), 32, 0,
                        new ControllerVmRoute(bgp_peer_));

    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    table1->DeleteReq(bgp_peer_, "vrf1",
                      Ip4Address::from_string("10.10.10.10"), 32,
                      new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    InetUnicastRouteEntry *rt_del =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del == nullptr);
    InetUnicastRouteEntry *rt_del_l3evpn_1 =
        RouteGet("l3evpn_1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del_l3evpn_1 == nullptr);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");

    // Clean up
    client->WaitForIdle();

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrBridgeVrf("vn2", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                 "instance_ip_2", 2);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->vm_table()->Size()));
    DelBgpaasPortRange();
    DeleteBgpRouterConfig("127.0.0.1", 0, "ip-fabric");
    DeleteBgpServiceConfig("1.1.1.10", 50000, "vnet1", "vrf1");
    DelIPAM("vn1");
    DelIPAM("vn2");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(VrfGet("vrf2") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    client->WaitForIdle();
}

TEST_F(VxlanRoutingTest, Add_del_bgpaas_composite_interfaces) {
    agent_ = Agent::GetInstance();
    AgentParam *params = agent_->params();
    params->set_bgpaas_max_shared_sessions(4);
    AddBgpaasPortRange(50000, 50512);
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
        {"vnet3", 3, "1.1.1.11", "00:00:01:01:01:11", 1, 3},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 2, "2.2.2.20", "00:00:02:02:02:20", 2, 2},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);

    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));

    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                 "instance_ip_2", 2);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    client->WaitForIdle();

    AddBgpRouterConfig("127.0.0.1", 0, 179,
                       64512, "ip-fabric", "control-node");
    std::string bgpaas1 = AddBgpServiceConfig("1.1.1.10", 50000, 179, 1,
                                              "vnet1", "vrf1",
                                              "bgpaas-client", true);
    std::string bgpaas2 = AddBgpServiceConfig("1.1.1.11", 50000, 179, 1,
                                              "vnet3", "vrf1",
                                              "bgpaas-client", true);
    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(3)->logical_router_uuid() == nil_uuid());

    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet3", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);

    // checking routing vrf have valid VXLAN ID
    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    // check to see if the subnet route for vn added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.0"), 24, true);

    ComponentNHKeyList comp_nh_list;
    InetUnicastRouteEntry *rt_1 =
        RouteGet("vrf1", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_1 != nullptr);
    const NextHop *nh_1 = rt_1->GetActiveNextHop();
    EXPECT_TRUE(nh_1->GetType() == NextHop::INTERFACE);

    DBEntryBase::KeyPtr db_nh_key_1 = nh_1->GetDBRequestKey();
    NextHopKey *nh_key_1 = static_cast<NextHopKey *>(db_nh_key_1.release());
    std::unique_ptr<const NextHopKey> nh_key_ptr_1(nh_key_1);
    ComponentNHKeyPtr nh_data1(new ComponentNHKey(rt_1->GetActiveLabel(),
                                                  std::move(nh_key_ptr_1)));

    InetUnicastRouteEntry *rt_2 =
        RouteGet("vrf1", Ip4Address::from_string("1.1.1.11"), 32);
    EXPECT_TRUE(rt_2 != nullptr);
    const NextHop *nh_2 = rt_2->GetActiveNextHop();
    EXPECT_TRUE(nh_2->GetType() == NextHop::INTERFACE);

    DBEntryBase::KeyPtr db_nh_key_2 = nh_2->GetDBRequestKey();
    NextHopKey *nh_key_2 = static_cast<NextHopKey *>(db_nh_key_2.release());
    std::unique_ptr<const NextHopKey> nh_key_ptr_2(nh_key_2);
    ComponentNHKeyPtr nh_data2(new ComponentNHKey(rt_2->GetActiveLabel(),
                                                  std::move(nh_key_ptr_2)));

    comp_nh_list.push_back(nh_data1);
    comp_nh_list.push_back(nh_data2);
    DBRequest nh_req(DBRequest::DB_ENTRY_ADD_CHANGE);
    nh_req.key.reset(new CompositeNHKey(Composite::ECMP, true,
                                        comp_nh_list, "vrf1"));
    nh_req.data.reset(new CompositeNHData());

    VnListType vn_list;
    vn_list.insert("vn1");
    PathPreference path_preference;
    EcmpLoadBalance ecmp_load_balance;
    InetUnicastAgentRouteTable *table1 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf1"));

    ControllerEcmpRoute *data = new ControllerEcmpRoute(
        bgp_peer_, vn_list, ecmp_load_balance, TagList(), SecurityGroupList(),
        path_preference, (1 << TunnelType::MPLS_GRE), nh_req,
        "10.10.10.10/32");
    ControllerEcmpRoute::ClonedLocalPathListIter iter =
        data->cloned_local_path_list().begin();
    while (iter != data->cloned_local_path_list().end()) {
        table1->AddClonedLocalPathReq(bgp_peer_, "vrf1",
                                      Ip4Address::from_string("10.10.10.10"),
                                      32, (*iter));
        iter++;
    }
    // ECMP create component NH
    table1->AddRemoteVmRouteReq(bgp_peer_, "vrf1",
                                Ip4Address::from_string("10.10.10.10"), 32,
                                data);
    client->WaitForIdle();

    InetUnicastRouteEntry *bgpaas_rt_vrf1 =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(bgpaas_rt_vrf1 != nullptr);

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("10.10.10.10"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet3", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("10.10.10.10"),
                    32, "vnet1", false, "vn1");

    //Add bgpaas route to evpn routing
    std::vector<std::string> peer_sources;
    peer_sources.push_back("1.1.1.10");
    peer_sources.push_back("1.1.1.11");
    MacAddress dummy_mac;

    // Add Tunnel BGPaaS
    BgpaasIntfRouteAdd(bgp_peer_, "l3evpn_1", TunnelType::VxlanType(),
                       agent_->router_id(),
                       routing_vrf->vxlan_id(), vn_list,
                       Ip4Address::from_string("10.10.10.10"), 32,
                       peer_sources);

    client->WaitForIdle();

    // Verify type5 route added to Lr evpn table is copied to bridge vrf
    InetUnicastRouteEntry *rt_l3evpn_1 =
        RouteGet("l3evpn_1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_l3evpn_1 != nullptr);

    if (rt_l3evpn_1) {
        const Route::PathList & path_list = rt_l3evpn_1->GetPathList();
        for (const auto& path_it : path_list) {
            const AgentPath* path =
                dynamic_cast<const AgentPath*>(&path_it);
            if (path != nullptr)
                continue;
            if ((path->nexthop()) &&
                (path->nexthop()->GetType() == NextHop::COMPOSITE)) {
               CompositeNH *comp_nh = dynamic_cast<CompositeNH *>
                    (path->nexthop());
                EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
                EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
                EXPECT_TRUE(comp_nh->PolicyEnabled()==true);
                const InterfaceNH *nh_nh = dynamic_cast<const InterfaceNH *>
                    (comp_nh->component_nh_list()[0]->nh());
                EXPECT_TRUE(nh_nh != nullptr);
                const InterfaceNH *nh_nh2 = dynamic_cast<const InterfaceNH *>
                    (comp_nh->component_nh_list()[1]->nh());
                EXPECT_TRUE(nh_nh2 != nullptr);
            }
        }
    }

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("10.10.10.10"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet3", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("10.10.10.10"),
                    32, "vnet1", true);

    EvpnAgentRouteTable *rt_table =
        static_cast<EvpnAgentRouteTable *>
            (agent_->vrf_table()->GetEvpnRouteTable("l3evpn_1"));
    rt_table->DeleteReq(bgp_peer_, "l3evpn_1", MacAddress(),
                        Ip4Address::from_string("10.10.10.10"), 32, 0,
                        new ControllerVmRoute(bgp_peer_));

    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    table1->DeleteReq(bgp_peer_, "vrf1",
                      Ip4Address::from_string("10.10.10.10"), 32,
                      new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    InetUnicastRouteEntry *rt_del =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del == nullptr);
    InetUnicastRouteEntry *rt_del_l3evpn_1 =
        RouteGet("l3evpn_1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del_l3evpn_1 == nullptr);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.11"), 32,
                    "vnet3", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");

    // Clean up
    client->WaitForIdle();

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrBridgeVrf("vn2", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                 "instance_ip_2", 2);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->vm_table()->Size()));
    DelBgpaasPortRange();
    DeleteBgpRouterConfig("127.0.0.1", 0, "ip-fabric");
    DeleteBgpServiceConfig("1.1.1.10", 50000, "vnet1", "vrf1");
    DeleteBgpServiceConfig("1.1.1.11", 50000, "vnet3", "vrf1");
    DelIPAM("vn1");
    DelIPAM("vn2");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(VrfGet("vrf2") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    client->WaitForIdle();
}

TEST_F(VxlanRoutingTest, Add_del_bgpaas_composite_interface_and_tunnel) {
    agent_ = Agent::GetInstance();
    AgentParam *params = agent_->params();
    params->set_bgpaas_max_shared_sessions(4);
    AddBgpaasPortRange(50000, 50512);
    bgp_peer_ = CreateBgpPeer("127.0.0.1", "remote");
    using boost::uuids::nil_uuid;
    struct PortInfo input1[] = {
        {"vnet1", 1, "1.1.1.10", "00:00:01:01:01:10", 1, 1},
    };
    IpamInfo ipam_info_1[] = {
        {"1.1.1.0", 24, "1.1.1.254", true},
    };

    struct PortInfo input2[] = {
        {"vnet2", 2, "2.2.2.20", "00:00:02:02:02:20", 2, 2},
    };
    IpamInfo ipam_info_2[] = {
        {"2.2.2.0", 24, "2.2.2.200", true},
    };

    // Bridge vrf
    AddIPAM("vn1", ipam_info_1, 1);
    AddIPAM("vn2", ipam_info_2, 1);

    CreateVmportEnv(input1, INPUT_SIZE(input1));
    CreateVmportEnv(input2, INPUT_SIZE(input2));

    AddLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    AddLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                 "instance_ip_2", 2);

    const char *routing_vrf_name = "l3evpn_1";
    AddLrRoutingVrf(1);
    AddLrBridgeVrf("vn1", 1);
    AddLrBridgeVrf("vn2", 1);
    client->WaitForIdle();

    AddBgpRouterConfig("127.0.0.1", 0, 179,
                       64512, "ip-fabric", "control-node");
    std::string bgpaas1 = AddBgpServiceConfig("1.1.1.10", 50000, 179, 1,
                                              "vnet1", "vrf1",
                                              "bgpaas-client", true);
    client->WaitForIdle();

    EXPECT_TRUE(VmInterfaceGet(1)->logical_router_uuid() == nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(2)->logical_router_uuid() == nil_uuid());

    EXPECT_TRUE(VmInterfaceGet(91)->logical_router_uuid() != nil_uuid());
    EXPECT_TRUE(VmInterfaceGet(92)->logical_router_uuid() != nil_uuid());

    VrfEntry *routing_vrf= VrfGet(routing_vrf_name);
    EXPECT_TRUE(routing_vrf->vxlan_id() != VxLanTable::kInvalidvxlan_id);

    InetUnicastAgentRouteTable *table1 =
        static_cast<InetUnicastAgentRouteTable*>
            (agent_->vrf_table()->GetInet4UnicastRouteTable("vrf1"));

    MacAddress dummy_mac;
    VnListType vn_list;
    vn_list.insert("vn1");
    PathPreference path_preference;
    EcmpLoadBalance ecmp_load_balance;

    ControllerVmRoute *data_tun = ControllerVmRoute::MakeControllerVmRoute(
        bgp_peer_, agent_->fabric_vrf_name(), agent_->router_id(), "vrf1",
        Ip4Address::from_string("100.1.1.11"), TunnelType::MPLS_GRE, 1001,
        dummy_mac, vn_list, SecurityGroupList(), TagList(), path_preference,
        false, ecmp_load_balance, false);
    table1->AddRemoteVmRouteReq(bgp_peer_, "vrf1",
                                Ip4Address::from_string("1.1.1.11"),
                                32, data_tun);

    BridgeTunnelRouteAdd(bgp_peer_, "l3evpn_1", TunnelType::VxlanType(),
                         Ip4Address::from_string("100.1.1.11"),
                         routing_vrf->vxlan_id(), dummy_mac,
                         Ip4Address::from_string("1.1.1.11"),
                         32, "00:00:99:99:99:99");
    client->WaitForIdle();

    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.11"), 32,
                    "100.1.1.11", true);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");

    // check to see if the local port route added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);

    // check to see if the subnet route for vn added to the bridge vrf inet
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.0"), 24, true);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.0"), 24, true);

    ComponentNHKeyList comp_nh_list;
    InetUnicastRouteEntry *rt_1 =
        RouteGet("vrf1", Ip4Address::from_string("1.1.1.10"), 32);
    EXPECT_TRUE(rt_1 != nullptr);
    const NextHop *nh_1 = rt_1->GetActiveNextHop();
    EXPECT_TRUE(nh_1->GetType() == NextHop::INTERFACE);

    DBEntryBase::KeyPtr db_nh_key_1 = nh_1->GetDBRequestKey();
    NextHopKey *nh_key_1 = static_cast<NextHopKey *>(db_nh_key_1.release());
    std::unique_ptr<const NextHopKey> nh_key_ptr_1(nh_key_1);
    ComponentNHKeyPtr nh_data1(new ComponentNHKey(rt_1->GetActiveLabel(),
                                                  std::move(nh_key_ptr_1)));

    TunnelNHKey *nh_key_2 = new TunnelNHKey(
        agent_->fabric_vrf_name(), agent_->router_id(),
        Ip4Address::from_string("100.1.1.11"), false, TunnelType::MPLS_GRE);
    std::unique_ptr<const NextHopKey> nh_key_ptr_2(nh_key_2);
    ComponentNHKeyPtr nh_data2(new ComponentNHKey(1001,
                                                  std::move(nh_key_ptr_2)));

    comp_nh_list.push_back(nh_data1);
    comp_nh_list.push_back(nh_data2);
    DBRequest nh_req(DBRequest::DB_ENTRY_ADD_CHANGE);
    nh_req.key.reset(new CompositeNHKey(Composite::ECMP, true,
                                        comp_nh_list, "vrf1"));
    nh_req.data.reset(new CompositeNHData());

    ControllerEcmpRoute *data = new ControllerEcmpRoute(
        bgp_peer_, vn_list, ecmp_load_balance, TagList(), SecurityGroupList(),
        path_preference, (1 << TunnelType::MPLS_GRE), nh_req,
        "10.10.10.10/32");
    ControllerEcmpRoute::ClonedLocalPathListIter iter =
        data->cloned_local_path_list().begin();
    while (iter != data->cloned_local_path_list().end()) {
        table1->AddClonedLocalPathReq(bgp_peer_, "vrf1",
                                      Ip4Address::from_string("10.10.10.10"),
                                      32, (*iter));
        iter++;
    }
    // ECMP create component NH
    table1->AddRemoteVmRouteReq(bgp_peer_, "vrf1",
                                Ip4Address::from_string("10.10.10.10"), 32,
                                data);
    client->WaitForIdle();

    InetUnicastRouteEntry *bgpaas_rt_vrf1 =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(bgpaas_rt_vrf1 != nullptr);

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("10.10.10.10"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.11"), 32,
                    "100.1.1.11", true);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("10.10.10.10"),
                    32, "vnet1", false, "vn1");

    //Add bgpaas route to evpn routing
    std::vector<std::string> peer_sources;
    peer_sources.push_back("1.1.1.10");
    peer_sources.push_back("1.1.1.11");

    stringstream ss_node;
    autogen::EnetItemType item;
    SecurityGroupList sg;
    item.entry.nlri.af = BgpAf::L2Vpn;
    item.entry.nlri.safi = BgpAf::Enet;
    item.entry.nlri.address = "10.10.10.10/32";
    item.entry.nlri.ethernet_tag = 0;
    autogen::EnetNextHopType nh, nh2;
    nh.af = Address::INET;
    nh.address = agent_->router_id().to_string();
    nh.label = routing_vrf->vxlan_id();
    nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    item.entry.next_hops.next_hop.push_back(nh);
    nh2.af = Address::INET;
    nh2.address = "100.1.1.11";
    nh2.label = routing_vrf->vxlan_id();
    nh2.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    item.entry.next_hops.next_hop.push_back(nh2);
    item.entry.med = 0;
    item.entry.peers.peer.push_back("1.1.1.10");
    item.entry.peers.peer.push_back("1.1.1.11");

    bgp_peer_->GetAgentXmppChannel()->AddEvpnEcmpRoute(
        routing_vrf_name, MacAddress::FromString("00:00:00:00:00:00"),
        Ip4Address::from_string("10.10.10.10"), 32, &item, vn_list);
    client->WaitForIdle();

    // Verify type5 route added to Lr evpn table is copied to inet table
    InetUnicastRouteEntry *rt_l3evpn_1 =
        RouteGet("l3evpn_1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_l3evpn_1 != nullptr);

    if (rt_l3evpn_1) {
        const Route::PathList & path_list = rt_l3evpn_1->GetPathList();
        for (const auto& path_it : path_list) {
            const AgentPath* path =
                dynamic_cast<const AgentPath*>(&path_it);
            if (path !=nullptr)
                continue;
            if ((path->nexthop()) &&
                (path->nexthop()->GetType() == NextHop::COMPOSITE)) {
                CompositeNH *comp_nh = dynamic_cast<CompositeNH *>
                    (path->nexthop());
                EXPECT_TRUE(comp_nh->GetType() == NextHop::COMPOSITE);
                EXPECT_TRUE(comp_nh->ComponentNHCount() == 2);
                EXPECT_TRUE(comp_nh->PolicyEnabled()==true);
                if (comp_nh->component_nh_list()[0] != nullptr)
                {
                    const InterfaceNH *nh_nh =
                        dynamic_cast<const InterfaceNH *>
                            (comp_nh->component_nh_list()[0]->nh());
                    EXPECT_TRUE(nh_nh != nullptr);
                    EXPECT_TRUE(comp_nh->component_nh_list()[1] == nullptr);
                }
                else
                {
                    const InterfaceNH *nh_nh2 =
                        dynamic_cast<const InterfaceNH *>
                            (comp_nh->component_nh_list()[1]->nh());
                    EXPECT_TRUE(nh_nh2 != nullptr);
                }
            }
        }
    }

    // Validate external rt gets installed in lr vrf with lr vn vxlan id
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("10.10.10.10"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.11"), 32,
                    "100.1.1.11", true);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("10.10.10.10"),
                    32, "vnet1", true);

    EvpnAgentRouteTable *rt_table =
        static_cast<EvpnAgentRouteTable *>
            (agent_->vrf_table()->GetEvpnRouteTable("l3evpn_1"));
    rt_table->DeleteReq(bgp_peer_, "l3evpn_1", MacAddress(),
                        Ip4Address::from_string("10.10.10.10"), 32, 0,
                        new ControllerVmRoute(bgp_peer_));


    // Send rt delete in lr vrf and see route gets deleted in bridge vrf
    table1->DeleteReq(bgp_peer_, "vrf1",
                      Ip4Address::from_string("10.10.10.10"), 32,
                      new ControllerVmRoute(bgp_peer_));
    client->WaitForIdle();

    InetUnicastRouteEntry *rt_del =
        RouteGet("vrf1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del == nullptr);
    InetUnicastRouteEntry *rt_del_l3evpn_1 =
        RouteGet("l3evpn_1", Ip4Address::from_string("10.10.10.10"), 32);
    EXPECT_TRUE(rt_del_l3evpn_1 == nullptr);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.10"), 32, false);
    ValidateBridge("vrf1", routing_vrf_name,
                   Ip4Address::from_string("1.1.1.11"), 32, false);
    ValidateBridge("vrf2", routing_vrf_name,
                   Ip4Address::from_string("2.2.2.20"), 32, false);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.10"), 32,
                    "vnet1", true, "vn1");
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("1.1.1.11"), 32,
                    "100.1.1.11", true);
    ValidateRouting(routing_vrf_name, Ip4Address::from_string("2.2.2.20"), 32,
                    "vnet2", true, "vn2");

    // Clean up
    client->WaitForIdle();

    // Bridge VN1 & VN2
    DelLrBridgeVrf("vn1", 1);
    DelLrBridgeVrf("vn2", 1);
    DelLrRoutingVrf(1);
    DelLrVmiPort("lr-vmi-vn1", 91, "1.1.1.99", "vrf1", "vn1",
                 "instance_ip_1", 1);
    DelLrVmiPort("lr-vmi-vn2", 92, "2.2.2.99", "vrf2", "vn2",
                 "instance_ip_2", 2);
    DeleteVmportEnv(input1, INPUT_SIZE(input1), true);
    DeleteVmportEnv(input2, INPUT_SIZE(input2), true);
    WAIT_FOR(1000, 1000, (0 == Agent::GetInstance()->vm_table()->Size()));
    DelBgpaasPortRange();
    DeleteBgpRouterConfig("127.0.0.1", 0, "ip-fabric");
    DeleteBgpServiceConfig("1.1.1.10", 50000, "vnet1", "vrf1");
    DelIPAM("vn1");
    DelIPAM("vn2");

    // Project
    DelNode("project", "admin");
    client->WaitForIdle();

    // Peer
    DeleteBgpPeer(bgp_peer_);
    client->WaitForIdle(5);

    // Checks
    EXPECT_TRUE(VrfGet("vrf1") == nullptr);
    EXPECT_TRUE(VrfGet("vrf2") == nullptr);
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    EXPECT_TRUE(agent_->oper_db()->vxlan_routing_manager()->vrf_mapper().
                IsEmpty());
    client->WaitForIdle();
}

int main(int argc, char *argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    GETUSERARGS();
    strcpy(init_file, DEFAULT_VNSW_CONFIG_FILE);
    client = TestInit(init_file, ksync_init, true, false);
    int ret = RUN_ALL_TESTS();
    TestShutdown();
    return ret;
}
