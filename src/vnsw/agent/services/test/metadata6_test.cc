/*
 * Copyright (c) 2023 Matvey Kraposhin
 */

#include "base/os.h"
#include "testing/gunit.h"

#include <boost/scoped_array.hpp>
#include <base/logging.h>
#include <base/task_annotations.h>

#include <pugixml/pugixml.hpp>
#include <io/event_manager.h>
#include "http/http_request.h"
#include "http/http_session.h"
#include "http/http_server.h"
#include "http/client/http_client.h"
#include "http/client/http_curl.h"
#include <cmn/agent_cmn.h>
#include <oper/operdb_init.h>
#include <oper/interface_common.h>
#include <pkt/pkt_init.h>
#include <services/services_init.h>
#include <test/test_cmn_util.h>
#include <services/services_sandesh.h>
#include <services/metadata_proxy.h>
#include "pkt/test/test_pkt_util.h"

#include <cstdio>
#include <cstdlib>
extern "C"{
#include <errno.h>
}

#define DEFAULT_VNSW_METADATA_CONFIG_FILE \
"controller/src/vnsw/agent/test/vnswa_metadata_cfg.ini"
#define MAX_WAIT_COUNT 5000
#define BUF_SIZE 8192
#define md_ip6 "fe80::a1fe:a1fe"
#define vm1_ip "ee80::11"
#define vm1_ll_ip "fe80::11:11"
#define vm1_mac "00:00:00:01:01:01"
#define vhost_ll_ip "fe80::11:22:33"
#define nova_proxy_mock_port 8775

IpamInfo ipam_info[] = {
    {"ee80::", 32, "ee80::1"}, /* prefix, prefixlen, gw */
};

IpAddress md_ip6_address(Ip6Address::from_string(md_ip6));

std::string ExecCmd(const std::string& cmd)
{
    char buffer[BUF_SIZE];
    std::string result;
    FILE *pipe = popen(cmd.c_str(), "r");

    if (!pipe) throw std::runtime_error("popen() failed!");

    while (!feof(pipe)) {
        if (fgets(buffer, BUF_SIZE, pipe) != NULL)
            result += buffer;
    }

    int rc = pclose(pipe);

    if (rc == EXIT_SUCCESS) {
    } else {
        result = std::string("");
    }

    return result;
}

class Metadata6Test : public ::testing::Test {

    void RemoveVhost() {
        std::string vhost_nm =
            Agent::GetInstance()->vhost_interface_name();
        std::string delete_vhost = "ip link del dev " + vhost_nm;
        std::system(delete_vhost.c_str());

        const std::string ipa_cmd = "ip addr";
        const std::string grep_cmd = "grep";
        const std::string vhost_cmd = ipa_cmd + " | " + grep_cmd +
            " " + agent_->vhost_interface_name();
        std::string grep_str = ExecCmd(vhost_cmd);
        EXPECT_TRUE(grep_str.empty());
    }

public:
    void SetUp() {
        AddIPAM("vn1", ipam_info, 1);
        client->WaitForIdle();

        // Check for a vhost presence
        std::string vhost_nm = agent_->vhost_interface_name();
        std::string vhost_str =
            ExecCmd(std::string("ip a | grep " + vhost_nm));
        // try to create a vhost interface
        // if it is not present
        if (vhost_str.empty()) {
            std::string create_vhost = "ip link add " + vhost_nm +
                " type dummy";
            std::system(create_vhost.c_str());
            std::string add_vhost_ip = "ip addr add " vhost_ll_ip " dev "
                + vhost_nm;
            std::system(add_vhost_ip.c_str());
        }
    }

    void TearDown() {
        DelIPAM("vn1");
        client->WaitForIdle();
        RemoveVhost();
        DeleteGlobalVrouterConfig();
    }

    Metadata6Test() :
        agent_(Agent::GetInstance()) {
        Agent::GetInstance()->set_compute_node_ip(
            Ip4Address::from_string("127.0.0.1"));
    }

    ~Metadata6Test() {
    }

    void CheckSandeshResponse(Sandesh *sandesh) {
    }

    void SetupLinkLocalConfig() {
        std::stringstream global_config;
        global_config << "<linklocal-services>\n"
            << "<linklocal-service-entry>\n"
            << "<linklocal-service-name>metadata6</linklocal-service-name>\n"
            << "<linklocal-service-ip>"<< md_ip6_address.to_string()
            <<"</linklocal-service-ip>\n"
            << "<linklocal-service-port>80</linklocal-service-port>\n"
            << "<ip-fabric-DNS-service-name></ip-fabric-DNS-service-name>\n"
            << "<ip-fabric-service-port>"
                << nova_proxy_mock_port
                << "</ip-fabric-service-port>\n"
            << "<ip-fabric-service-ip>127.0.0.1</ip-fabric-service-ip>\n"
            << "</linklocal-service-entry>\n"
            << "</linklocal-services>";

        char buf[BUF_SIZE];
        int len = 0;
        memset(buf, 0, BUF_SIZE);
        AddXmlHdr(buf, len);
        AddNodeString(buf, len, "global-vrouter-config",
            "default-global-system-config:default-global-vrouter-config",
            1024, global_config.str().c_str());
        AddXmlTail(buf, len);
        ApplyXmlString(buf);
    }

    void ClearLinkLocalConfig() {
        char buf[BUF_SIZE];
        int len = 0;
        memset(buf, 0, BUF_SIZE);
        AddXmlHdr(buf, len);
        AddNodeString(buf, len, "global-vrouter-config",
            "default-global-system-config:default-global-vrouter-config",
            1024, "");
        AddXmlTail(buf, len);
        ApplyXmlString(buf);
    }

    Agent *agent_;
};

TEST_F(Metadata6Test, Metadata6RouteFuncsTest) {
    struct PortInfo input[] = {
        /*  name  intf_id ip_addr mac_addr vn_id vm_id ip6addr*/
        {"vnet1", 1,      vm1_ip, vm1_mac, 1,    1},
    };
    SetupLinkLocalConfig();

    CreateV6VmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();

    VnEntry *vn1 = VnGet(1);
    ASSERT_TRUE(vn1 != NULL);

    VrfEntry *vrf1 = vn1->GetVrf();
    ASSERT_TRUE(vrf1 != NULL);

    InetUnicastRouteEntry *ucrt_vm_ip = vrf1->GetUcRoute
        (boost::asio::ip::address::from_string(vm1_ip));
    InetUnicastRouteEntry *ucrt_md_ip = vrf1->GetUcRoute
        (boost::asio::ip::address::from_string(md_ip6));
    // AdvertiseVhostRoute, OnAVrfChange
    ASSERT_TRUE(ucrt_vm_ip != NULL);
    ASSERT_TRUE(ucrt_md_ip != NULL);
    EXPECT_TRUE(ucrt_vm_ip->prefix_address().to_string() == std::string(vm1_ip));
    EXPECT_TRUE(ucrt_md_ip->prefix_address().to_string() == std::string(md_ip6));

    VrfEntry *fabric_vrf = agent_->fabric_vrf();
    EXPECT_TRUE(fabric_vrf != NULL);

    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    const VmInterface *cvhost0 = dynamic_cast<const VmInterface*>(
        agent_->vhost_interface());
    agent_->services()->metadataproxy()->InitializeHttp6Server(cvhost0);

    Ip6Address vhost_addr = agent_->services()->
        metadataproxy()->Ipv6ServiceAddress();
    ASSERT_EQ(vhost_addr,
        Ip6Address::from_string(vhost_ll_ip));

    TxTcp6Packet(intf->id(), vm1_ip, md_ip6, 1000, 80, false);
    client->WaitForIdle();
    FlowEntry *flow = FlowGet(0, vm1_ip, md_ip6, 6, 1000, 80,
                              intf->flow_key_nh()->id());
    ASSERT_TRUE(flow != NULL);
    FlowEntry *rflow = flow->reverse_flow_entry();
    ASSERT_TRUE(rflow != NULL);

    EXPECT_EQ(Ip6Address::from_string(vm1_ip),
                flow->key().src_addr.to_v6());
    EXPECT_EQ(Ip6Address::from_string(md_ip6),
                flow->key().dst_addr.to_v6());
    EXPECT_EQ(vhost_addr,
        rflow->key().src_addr.to_v6());
    EXPECT_EQ(intf->mdata_ip6_addr(),
        rflow->key().dst_addr.to_v6());

    // // Routes are added manually (the imitation of a packet
    // // interception by the PktFlowInfo::IngressProcess)
    // IpAddress ll_ip(Ip6Address::from_string(vm1_ll_ip));
    // agent_->services()->metadataproxy()->
    //     AdvertiseMetaDataLinkLocalRoutes(intf, ll_ip.to_v6(), vrf1);
    // InetUnicastRouteEntry *ucrt_ll2 = vrf1->GetUcRoute(ll_ip);
    // ASSERT_TRUE(ucrt_ll2 != NULL);
    // EXPECT_TRUE(ucrt_ll2->addr() == ll_ip);  // AdvertiseMetaDataLinkLocalRoutes

    // const Ip6Address tmp_ip = intf->mdata_ip6_addr();
    // const Interface *intf_new = agent_->interface_table()->
    //     FindInterfaceFromMetadataIp(tmp_ip);
    // EXPECT_TRUE(intf_new != NULL);  // FindInterfaceFromMetadataIp
    // client->WaitForIdle();

    // // Checks: OnAnInterfaceChange, DeleteMetaDataLinkLocalRoute,
    // DeleteVmportEnv(input, 1, 1, 0);
    // client->WaitForIdle();
    // ASSERT_TRUE(agent_->interface_table() != NULL);
    // EXPECT_TRUE(agent_->interface_table()->
    //     FindInterfaceFromMetadataIp(tmp_ip) == NULL);
    // ucrt_ll2 = vrf1->GetUcRoute(ll_ip);
    // EXPECT_TRUE(ucrt_ll2 == NULL);
    // ucrt_md_ip = vrf1->GetUcRoute
    //     (Ip6Address::from_string(md_ip6));
    // EXPECT_TRUE(ucrt_md_ip == NULL);  // OnAVrfChange

    client->WaitForIdle();
    ClearLinkLocalConfig();
    client->WaitForIdle();
}

TEST_F(Metadata6Test, Metadata6ServerResetTest) {
    struct PortInfo input[] = {
        /*  name  intf_id ip_addr mac_addr vn_id vm_id ip6addr*/
        {"vnet1", 1,      vm1_ip, vm1_mac, 1,    1},
    };
    CreateV6VmportEnv(input, 1, 0);
    client->WaitForIdle();

    VnEntry *vn1 = VnGet(1);
    EXPECT_TRUE(vn1 != NULL);
    VrfEntry *vrf1 = vn1->GetVrf();
    EXPECT_TRUE(vrf1 != NULL);

    // Clear old service config
    ClearLinkLocalConfig();
    client->WaitForIdle();

    // Add config with default address + announce a corresponding route
    SetupLinkLocalConfig();
    client->WaitForIdle();

    const VmInterface *cvhost0 = dynamic_cast<const VmInterface*>(
    agent_->vhost_interface());
    agent_->services()->metadataproxy()->InitializeHttp6Server(cvhost0);

    // FindLinkLocalService might cause problems with memory
    // {
    //     // find metadata service and announce a route to vhost0 interface
    //     uint16_t nova_port;
    //     Ip4Address nova_server;
    //     uint16_t linklocal_port;
    //     IpAddress linklocal_server;
    //     std::string nova_hostname;

    //     EXPECT_TRUE(agent_->oper_db()->global_vrouter()->FindLinkLocalService(
    //         GlobalVrouter::kMetadataService, &linklocal_server, &linklocal_port,
    //         &nova_hostname, &nova_server, &nova_port));
    //     EXPECT_TRUE(linklocal_server.to_v6() == md_ip6_address);
    // }
    InetUnicastRouteEntry *vrf1_md_rt =
        vrf1->GetUcRoute(md_ip6_address);
    EXPECT_TRUE(vrf1_md_rt != NULL);
    EXPECT_EQ(md_ip6_address.to_string(), vrf1_md_rt->prefix_address().to_string());

    // Change address of Metadata service and update config
    md_ip6_address = Ip6Address::from_string("fe80::a9fe:fea9");
    ClearLinkLocalConfig();
    client->WaitForIdle();
    SetupLinkLocalConfig();
    client->WaitForIdle();

    // FindLinkLocalService might cause problems with memory
    // {
    //     // find metadata service
    //     uint16_t nova_port;
    //     Ip4Address nova_server;
    //     uint16_t linklocal_port;
    //     IpAddress linklocal_server;
    //     std::string nova_hostname;

    //     if (agent_->oper_db()->global_vrouter()->FindLinkLocalService(
    //         GlobalVrouter::kMetadataService, &linklocal_server, &linklocal_port,
    //         &nova_hostname, &nova_server, &nova_port)) {
    //         client->WaitForIdle();
    //     }
    // }
    vrf1_md_rt =
        vrf1->GetUcRoute(md_ip6_address);
    EXPECT_TRUE(vrf1_md_rt != NULL);
    EXPECT_EQ(md_ip6_address.to_string(), vrf1_md_rt->prefix_address().to_string());

    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();

    ClearLinkLocalConfig();
    client->WaitForIdle();
}

int main(int argc, char *argv[]) {
    GETUSERARGS();

    client = TestInit(DEFAULT_VNSW_METADATA_CONFIG_FILE, ksync_init, true,
                        true, false);
    usleep(100000);
    client->WaitForIdle();

    int ret = RUN_ALL_TESTS();
    TestShutdown();
    delete client;
    return ret;
}
