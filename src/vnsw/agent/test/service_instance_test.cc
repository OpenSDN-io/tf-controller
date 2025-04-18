/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

#include "base/os.h"
#include "service_instance.h"

#include <boost/uuid/random_generator.hpp>
#include <pugixml/pugixml.hpp>
#include "schema/vnc_cfg_types.h"
#include "testing/gunit.h"
#include <test/test_cmn_util.h>

#include "base/test/task_test_util.h"
#include "db/test/db_test_util.h"
#include "db/db_entry.h"
#include "cfg/cfg_init.h"
#include "oper/operdb_init.h"

/*
 * TODO: remove these includes (used by for AgentTable::Clear())
 */
#include "filter/acl.h"
#include "oper/interface.h"
#include "oper/mirror_table.h"
#include "oper/mpls.h"
#include "oper/nexthop.h"
#include "oper/sg.h"
#include "oper/vm.h"
#include "oper/vn.h"
#include "oper/vrf.h"
#include "oper/vrf_assign.h"
#include "oper/vxlan.h"


using boost::uuids::uuid;

static std::string uuid_to_string(const uuid &uuid) {
    std::stringstream sgen;
    sgen << uuid;
    return sgen.str();
}

static void UuidTypeSet(const uuid &uuid, autogen::UuidType *idpair) {
    idpair->uuid_lslong = 0;
    idpair->uuid_mslong = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t value = uuid.data[16 - (i + 1)];
        idpair->uuid_lslong |= value << (8 * i);
    }
    for (int i = 0; i < 8; i++) {
        uint64_t value = uuid.data[8 - (i + 1)];
        idpair->uuid_mslong |= value << (8 * i);
    }
}

class ServiceInstanceState : public DBState {
    public:
        ServiceInstanceState() {
            seen = 0;
        }
        int seen;
};

class ServiceInstanceIntegrationTest : public ::testing::Test {
    public:
    void ServiceInstanceObserver(
                DBTablePartBase *db_part, DBEntryBase *entry) {
        if (!entry->IsDeleted()) {
            DBState *s =
                entry->GetState(agent_->service_instance_table(),
                        si_listener_);
            if (!s) {
                s = new ServiceInstanceState();
                entry->SetState(agent_->service_instance_table(),
                        si_listener_, s);
            }
        }
    }

  protected:
    ServiceInstanceIntegrationTest() {
    }

    virtual void SetUp() {
        agent_ = Agent::GetInstance();
    }



    /*
     * TODO: Move code to OperDB::Shutdown.
     */
    void DBClear() {
    }

    virtual void TearDown() {
        IFMapAgentStaleCleaner *cl = new IFMapAgentStaleCleaner(agent_->db(),
                    agent_->cfg()->cfg_graph());
        cl->StaleTimeout(1);
        task_util::WaitForIdle();
        delete cl;

    }

    void MessageInit() {
        doc_.reset();
        config_ = doc_.append_child("config");
    }

    std::string GetRandomIp() {
        std::stringstream ss;
        for(int i = 0; i != 4; i++) {
            ss << (rand() % 255) + 1 << ".";
        }
        std::string tmp = ss.str();
        return tmp.substr(0, tmp.size() - 1);
    }

    std::string GetRandomMac() {
        std::stringstream ss;
        for(int i = 0; i != 6; i++) {
            ss << std::hex << (rand() % 255) + 1 << ":";
        }
        std::string tmp = ss.str();
        return tmp.substr(0, tmp.size() - 1);
    }

    void EncodeNode(pugi::xml_node *parent, const std::string &obj_typename,
                    const std::string &obj_name, const IFMapObject *object) {
        pugi::xml_node node = parent->append_child("node");
        node.append_attribute("type") = obj_typename.c_str();
        node.append_child("name").text().set(obj_name.c_str());
        object->EncodeUpdate(&node);
    }

    void EncodeNodeDelete(pugi::xml_node *parent,
                          const std::string &obj_typename,
                          const std::string &obj_name) {
        pugi::xml_node node = parent->append_child("node");
        node.append_attribute("type") = obj_typename.c_str();
        node.append_child("name").text().set(obj_name.c_str());
    }

    void EncodeLink(pugi::xml_node *parent,
                    const std::string &lhs_type,
                    const std::string &lhs_name,
                    const std::string &rhs_type,
                    const std::string &rhs_name,
                    const std::string &metadata) {
        pugi::xml_node link = parent->append_child("link");
        pugi::xml_node left = link.append_child("node");
        left.append_attribute("type") = lhs_type.c_str();
        left.append_child("name").text().set(lhs_name.c_str());
        pugi::xml_node right = link.append_child("node");
        right.append_attribute("type") = rhs_type.c_str();
        right.append_child("name").text().set(rhs_name.c_str());
        pugi::xml_node meta = link.append_child("metadata");
        meta.append_attribute("type") = metadata.c_str();
    }

    void EncodeServiceInstance(const uuid &uuid, const std::string &name,
                               bool has_inside_interface) {
        autogen::IdPermsType id;
        id.Clear();
        UuidTypeSet(uuid, &id.uuid);
        autogen::ServiceInstance svc_instance;
        svc_instance.SetProperty("id-perms", &id);

        autogen::ServiceInstanceType svc_properties;
        svc_properties.Clear();
        if (has_inside_interface) {
            svc_properties.left_virtual_network = "vnet-left";
        }
        svc_properties.right_virtual_network = "vnet-right";
        svc_instance.SetProperty("service-instance-properties",
                                 &svc_properties);

        pugi::xml_node update = config_.append_child("update");
        EncodeNode(&update, "service-instance", name, &svc_instance);
    }

    void ConnectServiceTemplate(const std::string &si_name,
                                const std::string &tmpl_name,
                                const std::string &service_type) {
        boost::uuids::random_generator gen;
        autogen::IdPermsType id;
        id.Clear();
        UuidTypeSet(gen(), &id.uuid);

        autogen::ServiceTemplate svc_template;
        svc_template.SetProperty("id-perms", &id);

        autogen::ServiceTemplateType props;
        props.Clear();
        props.service_type = service_type;
        props.service_virtualization_type = "network-namespace";
        svc_template.SetProperty("service-template-properties", &props);

        pugi::xml_node update = config_.append_child("update");
        EncodeNode(&update, "service-template", tmpl_name, &svc_template);

        update = config_.append_child("update");
        EncodeLink(&update,
                   "service-instance", si_name,
                   "service-template", tmpl_name,
                   "service-instance-service-template");
    }

    uuid ConnectVirtualMachine(const std::string &name) {
        boost::uuids::random_generator gen;
        autogen::IdPermsType id;
        id.Clear();
        uuid vm_id = gen();
        UuidTypeSet(vm_id, &id.uuid);

        autogen::VirtualMachine virtual_machine;
        virtual_machine.SetProperty("id-perms", &id);

        pugi::xml_node update = config_.append_child("update");
        EncodeNode(&update, "virtual-machine", name, &virtual_machine);

        update = config_.append_child("update");
        EncodeLink(&update,
                   "virtual-machine", name,
                   "service-instance", name,
                   "virtual-machine-service-instance");
        return vm_id;
    }

    void ConnectNetworkIpam(const std::string &vnet_name,
                            const std::string &ip_addr) {
        boost::uuids::random_generator gen;
        autogen::IdPermsType id;
        id.Clear();
        UuidTypeSet(gen(), &id.uuid);

        autogen::NetworkIpam ipam;
        ipam.SetProperty("id-perms", &id);

        std::string ipam_name("ipam-");
        ipam_name.append(vnet_name);

        pugi::xml_node update = config_.append_child("update");
        EncodeNode(&update, "network-ipam", ipam_name, &ipam);


        autogen::VirtualNetworkNetworkIpam attr;
        autogen::VnSubnetsType subnet_list;
        subnet_list.Clear();
        autogen::IpamSubnetType subnet;
        subnet.Clear();
        subnet.subnet.ip_prefix = ip_addr;
        subnet.subnet.ip_prefix_len = 24;
        subnet.default_gateway = ip_addr;
        subnet_list.ipam_subnets.push_back(subnet);
        attr.SetData(&subnet_list);

        update = config_.append_child("update");
        std::stringstream attrname_gen;
        attrname_gen << "attr(" << ipam_name << "," << vnet_name << ")";
        EncodeNode(&update, "virtual-network-network-ipam", attrname_gen.str(),
                   &attr);

        update = config_.append_child("update");
        EncodeLink(&update,
                   "virtual-network", vnet_name,
                   "virtual-network-network-ipam", attrname_gen.str(),
                   "virtual-network-network-ipam");

        update = config_.append_child("update");
        EncodeLink(&update,
                   "virtual-network-network-ipam", attrname_gen.str(),
                   "network-ipam", ipam_name,
                   "virtual-network-network-ipam");
    }

    void ConnectVirtualNetwork(const std::string &vmi_name,
                               const std::string &ip_addr) {
        boost::uuids::random_generator gen;
        autogen::IdPermsType id;
        id.Clear();
        UuidTypeSet(gen(), &id.uuid);

        autogen::VirtualNetwork vnet;
        vnet.SetProperty("id-perms", &id);

        pugi::xml_node update = config_.append_child("update");
        std::string vnet_name("vnet-");
        size_t loc = vmi_name.find(':');
        if (loc != std::string::npos) {
            vnet_name.append(vmi_name.substr(loc + 1));
        } else {
            vnet_name.append(vmi_name);
        }
        EncodeNode(&update, "virtual-network", vnet_name, &vnet);

        ConnectNetworkIpam(vnet_name, ip_addr);

        update = config_.append_child("update");
        EncodeLink(&update,
                   "virtual-machine-interface", vmi_name,
                   "virtual-network", vnet_name,
                   "virtual-machine-interface-virtual-network");
    }

    void ConnectInstanceIp(const std::string &vmi_name,
                           const std::string &ip_addr) {
        boost::uuids::random_generator gen;
        autogen::IdPermsType id;
        id.Clear();
        uuid ip_id = gen();
        UuidTypeSet(ip_id, &id.uuid);

        autogen::InstanceIp ip;
        ip.SetProperty("id-perms", &id);

        autogen::InstanceIp::StringProperty ip_prop;
        ip_prop.data = ip_addr;
        ip.SetProperty("instance-ip-address", &ip_prop);

        pugi::xml_node update = config_.append_child("update");
        std::string ip_name("ip-");
        ip_name.append(vmi_name);
        EncodeNode(&update, "instance-ip", ip_name, &ip);

        update = config_.append_child("update");
        EncodeLink(&update,
                   "instance-ip", ip_name,
                   "virtual-machine-interface", vmi_name,
                   "instance-ip-virtual-machine-interface");
    }

    uuid ConnectVirtualMachineInterface(const std::string &vm_name,
                                        const std::string &vmi_name,
                                        const std::string &vmi_mac_addr,
                                        const std::string &vmi_ip_addr,
                                        bool is_left) {
        boost::uuids::random_generator gen;
        autogen::IdPermsType id;
        id.Clear();
        uuid vmi_id = gen();
        UuidTypeSet(vmi_id, &id.uuid);

        autogen::VirtualMachineInterface vmi;
        vmi.SetProperty("id-perms", &id);

        autogen::MacAddressesType mac_addresses;
        mac_addresses.mac_address.push_back(vmi_mac_addr);
        vmi.SetProperty("virtual-machine-interface-mac-addresses",
                        &mac_addresses);

        autogen::VirtualMachineInterfacePropertiesType properties;
        if (is_left)
            properties.service_interface_type = "left";
        else
            properties.service_interface_type = "right";

        vmi.SetProperty("virtual-machine-interface-properties",
                        &properties);

        pugi::xml_node update = config_.append_child("update");
        EncodeNode(&update, "virtual-machine-interface", vmi_name, &vmi);

        ConnectVirtualNetwork(vmi_name, vmi_ip_addr);
        ConnectInstanceIp(vmi_name, vmi_ip_addr);

        update = config_.append_child("update");
        EncodeLink(&update,
                   "virtual-machine-interface", vmi_name,
                   "virtual-machine", vm_name,
                   "virtual-machine-interface-virtual-machine");

        return vmi_id;
    }

    uuid ConnectLoadbalancerPool(const std::string &si_name,
                                 const std::string &pool_name) {
        boost::uuids::random_generator gen;
        uuid pool_id = gen();
        autogen::IdPermsType id;
        id.Clear();
        UuidTypeSet(pool_id, &id.uuid);

        autogen::LoadbalancerPool pool;
        pool.SetProperty("id-perms", &id);

        pugi::xml_node update = config_.append_child("update");
        EncodeNode(&update, "loadbalancer-pool", pool_name, &pool);

        update = config_.append_child("update");
        EncodeLink(&update,
                   "loadbalancer-pool", pool_name,
                   "service-instance", si_name,
                   "loadbalancer-pool-service-instance");
        return pool_id;
    }

  protected:
    Agent *agent_;
    pugi::xml_document doc_;
    pugi::xml_node config_;
    DBTableBase::ListenerId si_listener_;
};

/*
 * Verifies that we can flatten the graph into the operational structure
 * properties that contains the elements necessary to start the netns.
 */

TEST_F(ServiceInstanceIntegrationTest, Config) {
    boost::uuids::random_generator gen;
    uuid svc_id = gen();

    const std::string ip_left = GetRandomIp();
    const std::string ip_right = GetRandomIp();

    const std::string mac_left = GetRandomMac();
    const std::string mac_right = GetRandomMac();

    MessageInit();
    EncodeServiceInstance(svc_id, "test-1", true);
    IFMapAgentParser *parser = agent_->ifmap_parser();
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    ServiceInstanceTable *si_table = agent_->service_instance_table();
    EXPECT_EQ(1, si_table->Size());

    ServiceInstanceKey key(svc_id);
    ServiceInstance *svc_instance =
            static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance != NULL);

    MessageInit();
    ConnectServiceTemplate("test-1", "tmpl-1", "source-nat");
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    MessageInit();
    uuid vm_id = ConnectVirtualMachine("test-1");

    uuid vmi1 = ConnectVirtualMachineInterface("test-1", "left", mac_left,
                                               ip_left, true);
    uuid vmi2 = ConnectVirtualMachineInterface("test-1", "right", mac_right,
                                               ip_right, false);
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    EXPECT_EQ(vm_id, svc_instance->properties().instance_id);
    EXPECT_EQ(vmi1, svc_instance->properties().vmi_inside);
    EXPECT_EQ(vmi2, svc_instance->properties().vmi_outside);

    EXPECT_EQ(ip_left, svc_instance->properties().ip_addr_inside);
    EXPECT_EQ(ip_right, svc_instance->properties().ip_addr_outside);

    EXPECT_EQ(24, svc_instance->properties().ip_prefix_len_inside);
    EXPECT_EQ(24, svc_instance->properties().ip_prefix_len_outside);

    EXPECT_EQ(mac_left, svc_instance->properties().mac_addr_inside);
    EXPECT_EQ(mac_right, svc_instance->properties().mac_addr_outside);

    pugi::xml_node msg = config_.append_child("delete");
    EncodeLink(&msg,
               "service-instance", "test-1",
               "service-template", "tmpl-1",
               "service-instance-service-template");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine", "test-1",
               "virtual-machine-service-instance");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine-interface", "left",
               "virtual-machine-interface-virtual-machine");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine-interface", "right",
               "virtual-machine-interface-virtual-machine");

    EncodeNodeDelete(&msg, "service-instance", "test-1");
    EncodeNodeDelete(&msg, "virtual-machine", "test-1");
    EncodeNodeDelete(&msg, "virtual-machine-interface", "left");
    EncodeNodeDelete(&msg, "virtual-machine-interface", "right");

    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();
}


TEST_F(ServiceInstanceIntegrationTest, AddDeleteAdd) {
    boost::uuids::random_generator gen;
    uuid svc_id = gen();

    const std::string ip_left = GetRandomIp();
    const std::string ip_right = GetRandomIp();

    const std::string mac_left = GetRandomMac();
    const std::string mac_right = GetRandomMac();

    MessageInit();
    EncodeServiceInstance(svc_id, "test-1", true);
    IFMapAgentParser *parser = agent_->ifmap_parser();
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    ServiceInstanceTable *si_table = agent_->service_instance_table();
    si_listener_ = si_table->Register(boost::bind(&ServiceInstanceIntegrationTest::ServiceInstanceObserver, this,
                        _1, _2));
    EXPECT_EQ(1, si_table->Size());

    ServiceInstanceKey key(svc_id);
    ServiceInstance *svc_instance =
            static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance != NULL);

    MessageInit();
    ConnectServiceTemplate("test-1", "tmpl-1", "source-nat");
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    MessageInit();
    uuid vm_id = ConnectVirtualMachine("test-1");

    uuid vmi1 = ConnectVirtualMachineInterface("test-1", "left", mac_left,
                                               ip_left, true);
    uuid vmi2 = ConnectVirtualMachineInterface("test-1", "right", mac_right,
                                               ip_right, false);
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    EXPECT_EQ(vm_id, svc_instance->properties().instance_id);
    EXPECT_EQ(vmi1, svc_instance->properties().vmi_inside);
    EXPECT_EQ(vmi2, svc_instance->properties().vmi_outside);

    EXPECT_EQ(ip_left, svc_instance->properties().ip_addr_inside);
    EXPECT_EQ(ip_right, svc_instance->properties().ip_addr_outside);

    EXPECT_EQ(24, svc_instance->properties().ip_prefix_len_inside);
    EXPECT_EQ(24, svc_instance->properties().ip_prefix_len_outside);

    EXPECT_EQ(mac_left, svc_instance->properties().mac_addr_inside);
    EXPECT_EQ(mac_right, svc_instance->properties().mac_addr_outside);
    ASSERT_TRUE(svc_instance->IsUsable());

    pugi::xml_node msg = config_.append_child("delete");
    EncodeNodeDelete(&msg, "service-instance", "test-1");
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();
    EXPECT_EQ(1, si_table->Size());
    svc_instance = static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance != NULL);
    ASSERT_TRUE(svc_instance->IsDeleted());

    MessageInit();
    EncodeServiceInstance(svc_id, "test-1", true);
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();
    svc_instance = static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance != NULL);
    ASSERT_FALSE(svc_instance->IsDeleted());


    DBState *s = svc_instance->GetState(agent_->service_instance_table(),
                        si_listener_);
    svc_instance->ClearState(agent_->service_instance_table(), si_listener_);
    delete s;

    MessageInit();
    msg = config_.append_child("delete");
    EncodeLink(&msg,
               "service-instance", "test-1",
               "service-template", "tmpl-1",
               "service-instance-service-template");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine", "test-1",
               "virtual-machine-service-instance");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine-interface", "left",
               "virtual-machine-interface-virtual-machine");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine-interface", "right",
               "virtual-machine-interface-virtual-machine");

    EncodeNodeDelete(&msg, "service-instance", "test-1");
    EncodeNodeDelete(&msg, "virtual-machine", "test-1");
    EncodeNodeDelete(&msg, "virtual-machine-interface", "left");
    EncodeNodeDelete(&msg, "virtual-machine-interface", "right");

    agent_->service_instance_table()->Unregister(si_listener_);
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();
}

/*
 * Ensure that the code can deal with multiple instances.
 * In this case, the same template is used for all instances.
 */
TEST_F(ServiceInstanceIntegrationTest, MultipleInstances) {
    static const int kNumTestInstances = 16;
    typedef std::vector<uuid> UuidList;
    typedef std::vector<std::string> StrList;
    UuidList svc_ids;
    UuidList vm_ids;
    UuidList vmi_inside_ids;
    UuidList vmi_outside_ids;
    StrList vmi_inside_macs;
    StrList vmi_outside_macs;
    StrList vmi_inside_ips;
    StrList vmi_outside_ips;
    StrList names;

    IFMapAgentParser *parser = agent_->ifmap_parser();
    for (int i = 0; i < kNumTestInstances; ++i) {
        boost::uuids::random_generator gen;
        uuid svc_id = gen();
        svc_ids.push_back(svc_id);

        MessageInit();
        std::stringstream name_gen;
        name_gen << "instance-" << i;
        names.push_back(name_gen.str());
        EncodeServiceInstance(svc_id, name_gen.str(), true);
        ConnectServiceTemplate(name_gen.str(), "nat-netns-template",
                               "source-nat");
//
        vm_ids.push_back(
            ConnectVirtualMachine(name_gen.str()));

        std::string left_ip = GetRandomIp();
        vmi_inside_ips.push_back(left_ip);

        std::string right_ip = GetRandomIp();
        vmi_outside_ips.push_back(right_ip);

        std::string left_mac = GetRandomMac();
        vmi_inside_macs.push_back(left_mac);

        std::string right_mac = GetRandomMac();
        vmi_outside_macs.push_back(right_mac);

        std::string left_id = name_gen.str();
        left_id.append(":left");
        vmi_inside_ids.push_back(
            ConnectVirtualMachineInterface(name_gen.str(), left_id, left_mac,
                                           left_ip, true));

        std::string right_id = name_gen.str();
        right_id.append(":right");
        vmi_outside_ids.push_back(
            ConnectVirtualMachineInterface(name_gen.str(), right_id, right_mac,
                                           right_ip, false));
        parser->ConfigParse(config_, 0);
    }
    task_util::WaitForIdle();

    for (int i = 0; i < kNumTestInstances; ++i) {
       ServiceInstanceTable *si_table = agent_->service_instance_table();

        ServiceInstanceKey key(svc_ids.at(i));
        ServiceInstance *svc_instance =
                static_cast<ServiceInstance *>(si_table->Find(&key, true));
        ASSERT_TRUE(svc_instance != NULL);

        EXPECT_EQ(vm_ids.at(i), svc_instance->properties().instance_id);
        EXPECT_EQ(vmi_inside_ids.at(i), svc_instance->properties().vmi_inside);
        EXPECT_EQ(vmi_outside_ids.at(i),
                  svc_instance->properties().vmi_outside);
        EXPECT_EQ(vmi_inside_macs.at(i),
                  svc_instance->properties().mac_addr_inside);
        EXPECT_EQ(vmi_outside_macs.at(i),
                  svc_instance->properties().mac_addr_outside);
        EXPECT_EQ(vmi_inside_ips.at(i),
                  svc_instance->properties().ip_addr_inside);
        EXPECT_EQ(vmi_outside_ips.at(i),
                  svc_instance->properties().ip_addr_outside);
    }
    pugi::xml_node msg = config_.append_child("delete");

    for (int i = 0; i < kNumTestInstances; ++i) {
        EncodeLink(&msg,
               "service-instance", names.at(i),
               "service-template", "nat-netns-template",
               "service-instance-service-template");

        EncodeLink(&msg,
               "service-instance", names.at(i),
               "virtual-machine", names.at(i),
               "virtual-machine-service-instance");

        EncodeLink(&msg,
               "service-instance", names.at(i),
               "virtual-machine-interface", names.at(i) + ":left",
               "virtual-machine-interface-virtual-machine");

        EncodeLink(&msg,
               "service-instance", names.at(i),
               "virtual-machine-interface", names.at(i) + "right",
               "virtual-machine-interface-virtual-machine");

        EncodeNodeDelete(&msg, "service-instance", names.at(i));
        EncodeNodeDelete(&msg, "virtual-machine", names.at(i));
        EncodeNodeDelete(&msg, "virtual-machine-interface", names.at(i));
        EncodeNodeDelete(&msg, "virtual-machine-interface", names.at(i));
    }
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();
}

/*
 * Remove some of the links and ensure that the depedency tracking is
 * doing the right thing.
 */

TEST_F(ServiceInstanceIntegrationTest, RemoveLinks) {
    boost::uuids::random_generator gen;
    uuid svc_id = gen();

    MessageInit();
    EncodeServiceInstance(svc_id, "test-1", true);

    ConnectServiceTemplate("test-1", "tmpl-1", "source-nat");

    const std::string ip_left = GetRandomIp();
    const std::string ip_right = GetRandomIp();

    const std::string mac_left = GetRandomMac();
    const std::string mac_right = GetRandomMac();

    uuid vm_id = ConnectVirtualMachine("test-1");
    uuid vmi1 = ConnectVirtualMachineInterface("test-1", "left", mac_left,
                                               ip_left, true);
    uuid vmi2 = ConnectVirtualMachineInterface("test-1", "right", mac_right,
                                               ip_right, false);

    IFMapAgentParser *parser = agent_->ifmap_parser();
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    ServiceInstanceTable *si_table = agent_->service_instance_table();
    ServiceInstanceKey key(svc_id);
    ServiceInstance *svc_instance =
            static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance != NULL);

    EXPECT_EQ(vm_id, svc_instance->properties().instance_id);
    EXPECT_EQ(vmi1, svc_instance->properties().vmi_inside);
    EXPECT_EQ(vmi2, svc_instance->properties().vmi_outside);

    /*
     * Remove the link between the vmi and the network.
     */
    MessageInit();
    pugi::xml_node msg = config_.append_child("delete");
    EncodeLink(&msg,
               "virtual-machine-interface", "left",
               "virtual-network", "vnet-left",
               "virtual-machine-interface-virtual-network");
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    EXPECT_TRUE(svc_instance->properties().vmi_inside.is_nil());
    EXPECT_EQ(vmi2, svc_instance->properties().vmi_outside);

    /*
     * Removethe link between the instance and the virtual-machine object.
     */
    MessageInit();
    msg = config_.append_child("delete");
    EncodeLink(&msg,
               "virtual-machine", "test-1",
               "service-instance", "test-1",
               "virtual-machine-service-instance");
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    EXPECT_TRUE(svc_instance->properties().instance_id.is_nil());
    EXPECT_TRUE(svc_instance->properties().vmi_inside.is_nil());
    EXPECT_TRUE(svc_instance->properties().vmi_outside.is_nil());

    msg = config_.append_child("delete");
    EncodeLink(&msg,
               "service-instance", "test-1",
               "service-template", "tmpl-1",
               "service-instance-service-template");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine", "test-1",
               "virtual-machine-service-instance");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine-interface", "left",
               "virtual-machine-interface-virtual-machine");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine-interface", "right",
               "virtual-machine-interface-virtual-machine");

    EncodeNodeDelete(&msg, "service-instance", "test-1");
    EncodeNodeDelete(&msg, "virtual-machine", "test-1");
    EncodeNodeDelete(&msg, "virtual-machine-interface", "left");
    EncodeNodeDelete(&msg, "virtual-machine-interface", "right");

    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();
}

/*
 * Delete the service-instance object.
 */
TEST_F(ServiceInstanceIntegrationTest, Delete) {
    boost::uuids::random_generator gen;
    uuid svc_id = gen();

    MessageInit();
    EncodeServiceInstance(svc_id, "test-1", true);

    ConnectServiceTemplate("test-1", "tmpl-1", "source-nat");

    const std::string ip_left = GetRandomIp();
    const std::string ip_right = GetRandomIp();

    const std::string mac_left = GetRandomMac();
    const std::string mac_right = GetRandomMac();


    uuid vm_id = ConnectVirtualMachine("test-1");
    uuid vmi1 = ConnectVirtualMachineInterface("test-1", "left",
            mac_left, ip_left, true);
    uuid vmi2 = ConnectVirtualMachineInterface("test-1", "right",
            mac_right, ip_right, false);

    IFMapAgentParser *parser = agent_->ifmap_parser();
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    ServiceInstanceTable *si_table = agent_->service_instance_table();
    ServiceInstanceKey key(svc_id);
    ServiceInstance *svc_instance =
            static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance != NULL);

    EXPECT_EQ(vm_id, svc_instance->properties().instance_id);
    EXPECT_EQ(vmi1, svc_instance->properties().vmi_inside);
    EXPECT_EQ(vmi2, svc_instance->properties().vmi_outside);

    pugi::xml_node msg = config_.append_child("delete");
    EncodeLink(&msg,
               "virtual-machine", "test-1",
               "service-instance", "test-1",
               "virtual-machine-service-instance");
    EncodeLink(&msg,
               "service-instance", "test-1",
               "service-template", "tmpl-1",
               "service-instance-service-template");
    EncodeNodeDelete(&msg, "service-instance", "test-1");
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    DBEntry *entry = si_table->Find(&key, true);
    EXPECT_TRUE(entry == NULL);
}

/*
TEST_F(ServiceInstanceIntegrationTest, Loadbalancer) {
    boost::uuids::random_generator gen;
    uuid svc_id = gen();

    MessageInit();
    const std::string ip_right = GetRandomIp();
    const std::string mac_right = GetRandomMac();

    EncodeServiceInstance(svc_id, "loadbalancer-1", false);
    ConnectServiceTemplate("loadbalancer-1", "tmpl-1", "loadbalancer");

    ConnectVirtualMachine("loadbalancer-1");
    ConnectVirtualMachineInterface("loadbalancer-1", "right", mac_right,
                                   ip_right, false);
    ConnectLoadbalancerPool("loadbalancer-1", "pool-1");
    IFMapAgentParser *parser = agent_->ifmap_parser();
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    ServiceInstanceTable *si_table = agent_->service_instance_table();
    EXPECT_EQ(1, si_table->Size());

    ServiceInstanceKey key(svc_id);
    ServiceInstance *svc_instance =
            static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance != NULL);
    EXPECT_TRUE(svc_instance->IsUsable());

    pugi::xml_node msg = config_.append_child("delete");
    EncodeLink(&msg,
               "virtual-machine", "loadbalancer-1",
               "service-instance", "loadbalancer-1",
               "virtual-machine-service-instance");
    EncodeLink(&msg,
               "service-instance", "loadbalancer-1",
               "service-template", "tmpl-1",
               "service-instance-service-template");
    EncodeLink(&msg,
               "loadbalancer-pool", "pool-1",
               "service-instance", "loadbalancer-1",
               "loadbalancer-pool-service-instance");
    EncodeNodeDelete(&msg, "service-instance", "loadbalancer-1");
    EncodeNodeDelete(&msg, "virtual-machine", "loadbalancer-1");
    EncodeNodeDelete(&msg, "loadbalancer-pool", "pool-1");
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();
}
*/
TEST_F(ServiceInstanceIntegrationTest, ConfigUuidChange) {
    boost::uuids::random_generator gen;
    uuid svc_id = gen();

    const std::string ip_left = GetRandomIp();
    const std::string ip_right = GetRandomIp();

    const std::string mac_left = GetRandomMac();
    const std::string mac_right = GetRandomMac();

    MessageInit();
    EncodeServiceInstance(svc_id, "test-1", true);
    IFMapAgentParser *parser = agent_->ifmap_parser();
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    ServiceInstanceTable *si_table = agent_->service_instance_table();
    EXPECT_EQ(1, si_table->Size());

    ServiceInstanceKey key(svc_id);
    ServiceInstance *svc_instance =
            static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance != NULL);

    MessageInit();
    ConnectServiceTemplate("test-1", "tmpl-1", "source-nat");
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    MessageInit();
    uuid vm_id = ConnectVirtualMachine("test-1");

    uuid vmi1 = ConnectVirtualMachineInterface("test-1", "left", mac_left,
                                               ip_left, true);
    uuid vmi2 = ConnectVirtualMachineInterface("test-1", "right", mac_right,
                                               ip_right, false);
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    EXPECT_EQ(vm_id, svc_instance->properties().instance_id);
    EXPECT_EQ(vmi1, svc_instance->properties().vmi_inside);
    EXPECT_EQ(vmi2, svc_instance->properties().vmi_outside);

    EXPECT_EQ(ip_left, svc_instance->properties().ip_addr_inside);
    EXPECT_EQ(ip_right, svc_instance->properties().ip_addr_outside);

    EXPECT_EQ(24, svc_instance->properties().ip_prefix_len_inside);
    EXPECT_EQ(24, svc_instance->properties().ip_prefix_len_outside);

    EXPECT_EQ(mac_left, svc_instance->properties().mac_addr_inside);
    EXPECT_EQ(mac_right, svc_instance->properties().mac_addr_outside);


    //Change the service instance uuid with same name
    uuid new_svc_id = gen();
    MessageInit();
    EncodeServiceInstance(new_svc_id, "test-1", true);
    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();

    //Make sure old uuid oper entry is deleted
    svc_instance = static_cast<ServiceInstance *>(si_table->Find(&key, true));
    ASSERT_TRUE(svc_instance == NULL);

    //Make sre new uuid oper entry exists
    ServiceInstanceKey new_key(new_svc_id);
    svc_instance = static_cast<ServiceInstance *>(si_table->Find(&new_key, true));
    ASSERT_TRUE(svc_instance != NULL);

    //Start deleting
    MessageInit();
    pugi::xml_node msg = config_.append_child("delete");
    EncodeLink(&msg,
               "service-instance", "test-1",
               "service-template", "tmpl-1",
               "service-instance-service-template");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine", "test-1",
               "virtual-machine-service-instance");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine-interface", "left",
               "virtual-machine-interface-virtual-machine");

    EncodeLink(&msg,
               "service-instance", "test-1",
               "virtual-machine-interface", "right",
               "virtual-machine-interface-virtual-machine");

    EncodeNodeDelete(&msg, "service-instance", "test-1");
    EncodeNodeDelete(&msg, "virtual-machine", "test-1");
    EncodeNodeDelete(&msg, "virtual-machine-interface", "left");
    EncodeNodeDelete(&msg, "virtual-machine-interface", "right");

    parser->ConfigParse(config_, 0);
    task_util::WaitForIdle();
}

static void SetUp() {
}

static void TearDown() {
}

int main(int argc, char **argv) {

    GETUSERARGS();

    client = TestInit(init_file, ksync_init, false, false, false);
    usleep(100000);
    client->WaitForIdle();

    int ret = RUN_ALL_TESTS();
    TestShutdown();
    client->WaitForIdle();
    delete client;
    return ret;
}
