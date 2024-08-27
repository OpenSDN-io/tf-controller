/*
 * Copyright (c) 2024 Elena Zizganova
 */

#include "bgp/routing-instance/routepath_replicator.h"

#include <boost/foreach.hpp>
#include <boost/assign/list_of.hpp>

#include "bgp/bgp_config_ifmap.h"
#include "bgp/bgp_config_parser.h"
#include "bgp/bgp_factory.h"
#include "bgp/inet/inet_table.h"
#include "bgp/l3vpn/inetvpn_table.h"
#include "bgp/origin-vn/origin_vn.h"
#include "bgp/routing-instance/rtarget_group_mgr.h"
#include "bgp/test/bgp_server_test_util.h"
#include "bgp/test/bgp_test_util.h"
#include "control-node/control_node.h"
#include "db/test/db_test_util.h"
#include "db/db_table_walk_mgr.h"
#include "ifmap/ifmap_link_table.h"
#include "ifmap/test/ifmap_test_util.h"
#include "schema/bgp_schema_types.h"
#include "schema/vnc_cfg_types.h"

using namespace std;
using boost::assign::list_of;
using boost::assign::map_list_of;

class BgpPeerMock : public IPeer {
public:
    BgpPeerMock(const Ip4Address &address)
        : address_(address),
          address_str_(address.to_string()) {
    }
    virtual ~BgpPeerMock() { }
    virtual const std::string &ToString() const { return address_str_; }
    virtual const std::string &ToUVEKey() const { return address_str_; }
    virtual bool SendUpdate(const uint8_t *msg, size_t msgsize) {
        return true;
    }
    virtual BgpServer *server() {
        return NULL;
    }
    virtual BgpServer *server() const { return NULL; }
    virtual IPeerClose *peer_close() {
        return NULL;
    }
    virtual IPeerClose *peer_close() const { return NULL; }
    virtual void UpdateCloseRouteStats(Address::Family family,
        const BgpPath *old_path, uint32_t path_flags) const {
    }
    virtual IPeerDebugStats *peer_stats() {
        return NULL;
    }
    virtual const IPeerDebugStats *peer_stats() const {
        return NULL;
    }
    virtual bool IsReady() const {
        return true;
    }
    virtual bool IsXmppPeer() const { return false; }
    virtual void Close(bool graceful) { }
    BgpProto::BgpPeerType PeerType() const {
        return BgpProto::IBGP;
    }
    virtual uint32_t bgp_identifier() const {
        return htonl(address_.to_ulong());
    }
    virtual const std::string GetStateName() const {
        return "";
    }
    virtual void UpdateTotalPathCount(int count) const { }
    virtual int GetTotalPathCount() const { return 0; }
    virtual bool IsAs4Supported() const { return false; }
    virtual void UpdatePrimaryPathCount(int count,
        Address::Family family) const { }
    virtual int GetPrimaryPathCount() const { return 0; }
    virtual void ProcessPathTunnelEncapsulation(const BgpPath *path,
        BgpAttr *attr, ExtCommunityDB *extcomm_db, const BgpTable *table)
        const {
    }
    virtual const std::vector<std::string> GetDefaultTunnelEncap(
        Address::Family family) const {
        return std::vector<std::string>();
    }
    virtual bool IsRegistrationRequired() const { return true; }
    virtual void MembershipRequestCallback(BgpTable *table) { }
    virtual bool MembershipPathCallback(DBTablePartBase *tpart,
        BgpRoute *route, BgpPath *path) { return false; }
    virtual bool CanUseMembershipManager() const { return true; }
    virtual bool IsInGRTimerWaitState() const { return false; }
    virtual bool IsRouterTypeBGPaaS() const { return true; }

private:
    Ip4Address address_;
    std::string address_str_;
};

#define VERIFY_EQ(expected, actual) \
    TASK_UTIL_EXPECT_EQ(expected, actual)

static const char *bgp_server_config = "\
<config>\
    <bgp-router name=\'localhost\'>\
        <identifier>192.168.0.100</identifier>\
        <address>192.168.0.100</address>\
        <autonomous-system>64496</autonomous-system>\
    </bgp-router>\
</config>\
";

class BGPaaSRDTest : public ::testing::Test {
protected:
    BGPaaSRDTest()
      : config_db_(TaskScheduler::GetInstance()->GetTaskId("db::IFMapTable")),
        bgp_server_(new BgpServer(&evm_)) {
        IFMapLinkTable_Init(&config_db_, &config_graph_);
        vnc_cfg_Server_ModuleInit(&config_db_, &config_graph_);
        bgp_schema_Server_ModuleInit(&config_db_, &config_graph_);
    }
    ~BGPaaSRDTest() {
        STLDeleteValues(&peers_);
    }

    virtual void SetUp() {
        BgpIfmapConfigManager *config_manager =
                static_cast<BgpIfmapConfigManager *>(
                    bgp_server_->config_manager());
        config_manager->Initialize(&config_db_, &config_graph_, "localhost");
        bgp_server_->rtarget_group_mgr()->Initialize();
        BgpConfigParser bgp_parser(&config_db_);
        bgp_parser.Parse(bgp_server_config);
        task_util::WaitForIdle();
    }

    virtual void TearDown() {
        task_util::WaitForIdle();
        bgp_server_->Shutdown();
        task_util::WaitForIdle();
        db_util::Clear(&config_db_);
    }

    void NetworkConfig(const vector<string> &instance_names,
                       const multimap<string, string> &connections) {
        bgp_util::NetworkConfigGenerate(&config_db_, instance_names,
                                        connections);
    }


    void DeleteRoutingInstance(const string &instance_name, const string &rt_name) {
        ifmap_test_util::IFMapMsgUnlink(&config_db_, "routing-instance", instance_name,
            "virtual-network", instance_name, "virtual-network-routing-instance");
        ifmap_test_util::IFMapMsgUnlink(&config_db_, "routing-instance", instance_name,
            "route-target", rt_name, "instance-target");
        ifmap_test_util::IFMapMsgNodeDelete(
            &config_db_, "virtual-network", instance_name);
        ifmap_test_util::IFMapMsgNodeDelete(
            &config_db_, "routing-instance", instance_name);
        ifmap_test_util::IFMapMsgNodeDelete(
            &config_db_, "route-target", rt_name);
        task_util::WaitForIdle();
    }

    void VerifyTableNoExists(const string &table_name) {
        TASK_UTIL_EXPECT_TRUE(
            bgp_server_->database()->FindTable(table_name) == NULL);
    }

    void AddInetRoute(IPeer *peer, const string &instance_name,
                      const string &prefix, int localpref, string rd = "",
                      const vector<string> &rtarget_list = vector<string>(),
                      uint32_t flags = 0, uint32_t label = 0) {
        boost::system::error_code error;
        Ip4Prefix nlri = Ip4Prefix::FromString(prefix, &error);
        EXPECT_FALSE(error);
        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        request.key.reset(new InetTable::RequestKey(nlri, peer));
        BgpAttrSpec attr_spec;
        boost::scoped_ptr<BgpAttrLocalPref> local_pref(
                                new BgpAttrLocalPref(localpref));
        attr_spec.push_back(local_pref.get());
        BgpAttrSourceRd rd_spec(RouteDistinguisher::FromString(rd));
        if (!rd.empty()) {
            attr_spec.push_back(&rd_spec);
        }
        ExtCommunitySpec spec;
        if (!rtarget_list.empty()) {
            BOOST_FOREACH(string tgt, rtarget_list) {
                RouteTarget rt(RouteTarget::FromString(tgt));
                spec.communities.push_back(get_value(rt.GetExtCommunity().begin(), 8));
            }
            attr_spec.push_back(&spec);
        }

        BgpAttrPtr attr = bgp_server_->attr_db()->Locate(attr_spec);
        request.data.reset(new BgpTable::RequestData(attr, flags, label));
        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable(instance_name + ".inet.0"));
        ASSERT_TRUE(table != NULL);
        table->Enqueue(&request);
        task_util::WaitForIdle();
    }

    void AddVPNRouteCommon(IPeer *peer, const string &prefix,
                           const BgpAttrSpec &attr_spec) {
        boost::system::error_code error;
        InetVpnPrefix nlri = InetVpnPrefix::FromString(prefix, &error);
        EXPECT_FALSE(error);
        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        request.key.reset(new InetVpnTable::RequestKey(nlri, peer));
        BgpAttrPtr attr = bgp_server_->attr_db()->Locate(attr_spec);
        request.data.reset(new BgpTable::RequestData(attr, 0, 0));
        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable("bgp.l3vpn.0"));
        ASSERT_TRUE(table != NULL);
        table->Enqueue(&request);
        task_util::WaitForIdle();
    }

    void AddVPNRoute(IPeer *peer, const string &prefix, int localpref,
                     const vector<string> &instance_names) {
        BgpAttrSpec attr_spec;
        boost::scoped_ptr<BgpAttrLocalPref> local_pref(
                                new BgpAttrLocalPref(localpref));
        attr_spec.push_back(local_pref.get());

        boost::scoped_ptr<ExtCommunitySpec> commspec;
        commspec.reset(BuildInstanceListTargets(instance_names, &attr_spec));
        AddVPNRouteCommon(peer, prefix, attr_spec);
        task_util::WaitForIdle();
    }

    void AddVPNRouteWithTarget(IPeer *peer, const string &prefix, int localpref,
                               const string &target,
                               string origin_vn_str = string()) {
        BgpAttrSpec attr_spec;
        boost::scoped_ptr<BgpAttrLocalPref> local_pref(
                                new BgpAttrLocalPref(localpref));
        attr_spec.push_back(local_pref.get());

        boost::scoped_ptr<ExtCommunitySpec> commspec(new ExtCommunitySpec());
        RouteTarget tgt = RouteTarget::FromString(target);
        const ExtCommunity::ExtCommunityValue &extcomm =
            tgt.GetExtCommunity();
        uint64_t value = get_value(extcomm.data(), extcomm.size());
        commspec->communities.push_back(value);

        if (!origin_vn_str.empty()) {
            OriginVn origin_vn = OriginVn::FromString(origin_vn_str);
            commspec->communities.push_back(origin_vn.GetExtCommunityValue());
        }
        attr_spec.push_back(commspec.get());
        AddVPNRouteCommon(peer, prefix, attr_spec);
        task_util::WaitForIdle();
    }

    void DeleteVPNRoute(IPeer *peer, const string &prefix) {
        boost::system::error_code error;
        InetVpnPrefix nlri = InetVpnPrefix::FromString(prefix, &error);
        EXPECT_FALSE(error);
        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_DELETE;
        request.key.reset(new InetVpnTable::RequestKey(nlri, peer));
        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable("bgp.l3vpn.0"));
        ASSERT_TRUE(table != NULL);
        table->Enqueue(&request);
    }

    void DeleteInetRoute(IPeer *peer, const string &instance_name,
                         const string &prefix) {
        boost::system::error_code error;
        Ip4Prefix nlri = Ip4Prefix::FromString(prefix, &error);
        EXPECT_FALSE(error);

        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_DELETE;
        request.key.reset(new InetTable::RequestKey(nlri, peer));

        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable(instance_name + ".inet.0"));
        ASSERT_TRUE(table != NULL);

        table->Enqueue(&request);
    }


    ExtCommunitySpec *BuildInstanceListTargets(
        const vector<string> &instance_names, BgpAttrSpec *attr_spec) {
        ExtCommunitySpec *commspec(new ExtCommunitySpec());
        for (vector<string>::const_iterator iter = instance_names.begin();
             iter != instance_names.end(); ++iter) {
            RoutingInstance *rti =
                bgp_server_->routing_instance_mgr()->GetRoutingInstance(*iter);
            BOOST_FOREACH(RouteTarget tgt, rti->GetExportList()) {
                const ExtCommunity::ExtCommunityValue &extcomm =
                        tgt.GetExtCommunity();
                uint64_t value = get_value(extcomm.data(), extcomm.size());
                commspec->communities.push_back(value);
            }
        }
        if (!commspec->communities.empty()) {
            attr_spec->push_back(commspec);
        }

        return commspec;
    }

    int RouteCount(const string &instance_name) const {
        string tablename(instance_name);
        tablename.append(".inet.0");
        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable(tablename));
        EXPECT_TRUE(table != NULL);
        if (table == NULL) {
            return 0;
        }
        return table->Size();
    }

    BgpRoute *VPNRouteLookup(const string &prefix) {
        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable("bgp.l3vpn.0"));
        EXPECT_TRUE(table != NULL);
        if (table == NULL) {
            return NULL;
        }
        boost::system::error_code error;
        InetVpnPrefix nlri = InetVpnPrefix::FromString(prefix, &error);
        EXPECT_FALSE(error);
        InetVpnTable::RequestKey key(nlri, NULL);
        BgpRoute *rt = static_cast<BgpRoute *>(table->Find(&key));
        return rt;
    }

    void VerifyVPNPathRouteTargets(const BgpPath *path,
        const vector<string> &expected_targets) {
        const BgpAttr *attr = path->GetAttr();
        const ExtCommunity *ext_community = attr->ext_community();
        set<string> actual_targets;
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                      ext_community->communities()) {
            if (!ExtCommunity::is_route_target(comm))
                continue;
            RouteTarget rtarget(comm);
            actual_targets.insert(rtarget.ToString());
        }

        EXPECT_EQ(actual_targets.size(), expected_targets.size());
        BOOST_FOREACH(const string &target, expected_targets) {
            EXPECT_TRUE(actual_targets.find(target) != actual_targets.end());
        }
    }

    BgpRoute *InetRouteLookup(const string &instance_name, const string &prefix) {
        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable(instance_name + ".inet.0"));
        EXPECT_TRUE(table != NULL);
        if (table == NULL) {
            return NULL;
        }
        boost::system::error_code error;
        Ip4Prefix nlri = Ip4Prefix::FromString(prefix, &error);
        EXPECT_FALSE(error);
        InetTable::RequestKey key(nlri, NULL);
        BgpRoute *rt = static_cast<BgpRoute *>(table->Find(&key));
        return rt;
    }

    const RtReplicated *InetRouteReplicationState(const string &instance_name,
                                                  BgpRoute *rt) {
        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable(instance_name + ".inet.0"));
        RoutePathReplicator *replicator =
            bgp_server_->replicator(Address::INETVPN);
        return replicator->GetReplicationState(table, rt);
    }

    const BgpRoute *GetVPNSecondary(const RtReplicated *rts) {
        BOOST_FOREACH(const RtReplicated::SecondaryRouteInfo rinfo,
                      rts->GetList()) {
            if (BgpAf::FamilyToSafi(rinfo.table_->family()) == BgpAf::Vpn) {
                return rinfo.rt_;
            }
        }
        return NULL;
    }

    string GetInstanceRD(const string &instance) {
        TASK_UTIL_EXPECT_NE(static_cast<RoutingInstance *>(NULL),
            bgp_server_->routing_instance_mgr()->GetRoutingInstance(instance));
        const RoutingInstance *rti =
            bgp_server_->routing_instance_mgr()->GetRoutingInstance(instance);
        return rti->GetRD()->ToString();
    }

    vector<string> GetInstanceRouteTargetList(const string &instance,
        bool import = false) {
        TASK_UTIL_EXPECT_NE(static_cast<RoutingInstance *>(NULL),
            bgp_server_->routing_instance_mgr()->GetRoutingInstance(instance));
        RoutingInstance *rti =
            bgp_server_->routing_instance_mgr()->GetRoutingInstance(instance);
        vector<string> target_list;
        if (import) {
            BOOST_FOREACH(RouteTarget tgt, rti->GetImportList()) {
                target_list.push_back(tgt.ToString());
            }
        } else {
            BOOST_FOREACH(RouteTarget tgt, rti->GetExportList()) {
                target_list.push_back(tgt.ToString());
            }
        }
        sort(target_list.begin(), target_list.end());
        return target_list;
    }

    vector<string> GetInstanceImportRouteTargetList(const string &instance) {
        return GetInstanceRouteTargetList(instance, true);
    }

    vector<string> GetInstanceExportRouteTargetList(const string &instance) {
        return GetInstanceRouteTargetList(instance, false);
    }

    int GetInstanceOriginVnIndex(const string &instance) {
        TASK_UTIL_EXPECT_NE(static_cast<RoutingInstance *>(NULL),
            bgp_server_->routing_instance_mgr()->GetRoutingInstance(instance));
        RoutingInstance *rti =
            bgp_server_->routing_instance_mgr()->GetRoutingInstance(instance);
        TASK_UTIL_EXPECT_NE(0, rti->virtual_network_index());
        return rti->virtual_network_index();
    }

    void AddInstanceRouteTarget(const string &instance, const string &target,
        bool do_import = true, bool do_export = true) {
        assert(do_import || do_export);
        autogen::InstanceTargetType *tgt_type = new autogen::InstanceTargetType;
        if (do_import && do_export) {
            tgt_type->import_export = "";
        } else if (do_import) {
            tgt_type->import_export = "import";
        } else if (do_export) {
            tgt_type->import_export = "export";
        }
        TASK_UTIL_EXPECT_NE(static_cast<RoutingInstance *>(NULL),
            bgp_server_->routing_instance_mgr()->GetRoutingInstance(instance));
        ifmap_test_util::IFMapMsgLink(&config_db_,
            "routing-instance", instance,
            "route-target", target, "instance-target", 0, tgt_type);
        task_util::WaitForIdle();
    }

    void AddInstanceImportRouteTarget(const string &instance,
        const string &target) {
        AddInstanceRouteTarget(instance, target, true, false);
    }

    void AddInstanceExportRouteTarget(const string &instance,
        const string &target) {
        AddInstanceRouteTarget(instance, target, false, true);
    }

    void RemoveInstanceRouteTarget(const string instance, const string &target) {
        TASK_UTIL_EXPECT_NE(static_cast<RoutingInstance *>(NULL),
            bgp_server_->routing_instance_mgr()->GetRoutingInstance(instance));
        ifmap_test_util::IFMapMsgUnlink(&config_db_,
            "routing-instance", instance,
            "route-target", target, "instance-target");
        task_util::WaitForIdle();
    }

    int GetOriginVnIndexFromRoute(const BgpPath *path) {
        const ExtCommunity *ext_comm = path->GetAttr()->ext_community();
        assert(ext_comm);
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                      ext_comm->communities()) {
            if (!ExtCommunity::is_origin_vn(comm))
                continue;
            OriginVn origin_vn(comm);
            if (origin_vn.as_number() != 64496)
                continue;
            return origin_vn.vn_index();
        }
        return 0;
    }

    const TableState *LookupVpnTableState() {
        RoutePathReplicator *replicator =
            bgp_server_->replicator(Address::INETVPN);
        RoutingInstanceMgr *mgr = bgp_server_->routing_instance_mgr();
        RoutingInstance *master =
                mgr->GetRoutingInstance(BgpConfigManager::kMasterInstance);
        assert(master);
        BgpTable *vpn_table = master->GetTable(Address::INETVPN);
        const TableState *vpn_ts = replicator->FindTableState(vpn_table);
        return vpn_ts;
    }

    void VerifyVpnTableStateExists(bool exists) {
        if (exists)
            TASK_UTIL_EXPECT_TRUE(LookupVpnTableState() != NULL);
        else
            TASK_UTIL_EXPECT_TRUE(LookupVpnTableState() == NULL);
    }

    const TableState *LookupTableState(const string &instance_name) {
        RoutePathReplicator *replicator =
            bgp_server_->replicator(Address::INETVPN);

        BgpTable *table = static_cast<BgpTable *>(
            bgp_server_->database()->FindTable(instance_name + ".inet.0"));
        return replicator->FindTableState(table);
    }

    const TableState *VerifyVRFTableStateExists(const string &instance_name, bool exists) {
        const TableState *ts = NULL;
        if (exists)
            TASK_UTIL_EXPECT_TRUE((ts = LookupTableState(instance_name)) != NULL);
        else
            TASK_UTIL_EXPECT_TRUE((ts = LookupTableState(instance_name)) == NULL);
        return ts;
    }

    void VerifyVpnTableStateRouteCount(uint32_t count) {
        const TableState *vpn_ts = LookupVpnTableState();
        TASK_UTIL_EXPECT_EQ(count, (vpn_ts ? vpn_ts->route_count() : 0));
    }

    void DisableBulkSync() {
        DBTableWalkMgr *walk_mgr = bgp_server_->database()->GetWalkMgr();
        task_util::TaskFire(
            boost::bind(&DBTableWalkMgr::DisableWalkProcessing, walk_mgr),
            "bgp::Config");
    }

    void EnableBulkSync() {
        DBTableWalkMgr *walk_mgr = bgp_server_->database()->GetWalkMgr();
        task_util::TaskFire(
            boost::bind(&DBTableWalkMgr::EnableWalkProcessing, walk_mgr),
            "bgp::Config");
    }

    EventManager evm_;
    DB config_db_;
    DBGraph config_graph_;
    boost::scoped_ptr<BgpServer> bgp_server_;
    vector<BgpPeerMock *> peers_;
};

TEST_F(BGPaaSRDTest, UpdatePathData)  {
    vector<string> instance_names = list_of("blue");
    multimap<string, string> connections;
    NetworkConfig(instance_names, connections);
    task_util::WaitForIdle();

    boost::system::error_code ec;
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.10", ec)));
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.20", ec)));
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.30", ec)));

    AddInetRoute(peers_[0], "blue", "10.9.9.9/32", 100);
    task_util::WaitForIdle();

    // Replicated to both green and red.
    VERIFY_EQ(1, RouteCount("blue"));

    BgpRoute *rt_blue = InetRouteLookup("blue", "10.9.9.9/32");

    VERIFY_EQ(1, rt_blue->count());

    EXPECT_EQ(0, rt_blue->BestPath()->GetFlags());

    EXPECT_EQ(0, rt_blue->BestPath()->GetLabel());

    // Update primary path's label.
    AddInetRoute(peers_[0], "blue", "10.9.9.9/32", 100, "", vector<string>(),
                 0, 200);
    task_util::WaitForIdle();

    rt_blue = InetRouteLookup("blue", "10.9.9.9/32");

    VERIFY_EQ(1, rt_blue->count());

    EXPECT_EQ(200, rt_blue->BestPath()->GetLabel());

    // Reset path's label and flag
    AddInetRoute(peers_[0], "blue", "10.9.9.9/32", 100, "", vector<string>(),
                 0, 0);
    task_util::WaitForIdle();

    rt_blue = InetRouteLookup("blue", "10.9.9.9/32");

    VERIFY_EQ(1, rt_blue->count());

    EXPECT_EQ(0, rt_blue->BestPath()->GetFlags());

    EXPECT_EQ(0, rt_blue->BestPath()->GetLabel());

    DeleteInetRoute(peers_[0], "blue", "10.9.9.9/32");
    task_util::WaitForIdle();

    VERIFY_EQ(0, RouteCount("blue"));
}

//Second test to add

TEST_F(BGPaaSRDTest, ECMPInetRoute)  {
    vector<string> instance_names = list_of("blue");
    multimap<string, string> connections;
    NetworkConfig(instance_names, connections);
    task_util::WaitForIdle();

    boost::system::error_code ec;
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.10", ec)));
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.20", ec)));
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.30", ec)));

    AddInetRoute(peers_[0], "blue", "10.9.9.9/32", 100);
    task_util::WaitForIdle();

    VERIFY_EQ(1, RouteCount("blue"));

    BgpRoute *rt_blue = InetRouteLookup("blue", "10.9.9.9/32");

    VERIFY_EQ(1, rt_blue->count());
    //
    // Add 2 more paths in blue
    //
    AddInetRoute(peers_[1], "blue", "10.9.9.9/32", 100);
    AddInetRoute(peers_[2], "blue", "10.9.9.9/32", 100);
    task_util::WaitForIdle();

    VERIFY_EQ(3, rt_blue->count());

    DeleteInetRoute(peers_[0], "blue", "10.9.9.9/32");
    task_util::WaitForIdle();
    // 2 replicatd  and 2 paths from peer
    VERIFY_EQ(2, rt_blue->count());

    DeleteInetRoute(peers_[1], "blue", "10.9.9.9/32");
    DeleteInetRoute(peers_[2], "blue", "10.9.9.9/32");
    task_util::WaitForIdle();

    VERIFY_EQ(0, RouteCount("blue"));

}

// Check how does it work
TEST_F(BGPaaSRDTest, ECMPInetRouteReplication) {
    vector<string> instance_names = list_of("blue");
    multimap<string, string> connections;
    NetworkConfig(instance_names, connections);
    task_util::WaitForIdle();

    boost::system::error_code ec;
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.10", ec)));
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.20", ec)));
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.30", ec)));

    AddInetRoute(peers_[0], "blue", "10.9.9.9/32", 100);
    AddInetRoute(peers_[1], "blue", "10.9.9.9/32", 100);
    AddInetRoute(peers_[2], "blue", "10.9.9.9/32", 100);
    task_util::WaitForIdle();

    BgpRoute *rt_blue = InetRouteLookup("blue", "10.9.9.9/32");
    VERIFY_EQ(3, rt_blue->count());
    TASK_UTIL_EXPECT_TRUE(VPNRouteLookup("10.10.10.10:0:10.9.9.9/32") != NULL);
    TASK_UTIL_EXPECT_TRUE(VPNRouteLookup("10.10.10.20:0:10.9.9.9/32") != NULL);
    TASK_UTIL_EXPECT_TRUE(VPNRouteLookup("10.10.10.30:0:10.9.9.9/32") != NULL);

    DeleteInetRoute(peers_[0], "blue", "10.9.9.9/32");
    DeleteInetRoute(peers_[1], "blue", "10.9.9.9/32");
    DeleteInetRoute(peers_[2], "blue", "10.9.9.9/32");
    task_util::WaitForIdle();

    VERIFY_EQ(0, RouteCount("blue"));
}

TEST_F(BGPaaSRDTest, AnotherPathWithDifferentRD) {
    vector<string> instance_names = list_of("blue");
    multimap<string, string> connections;
    NetworkConfig(instance_names, connections);
    task_util::WaitForIdle();

    boost::system::error_code ec;
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.10", ec)));
    peers_.push_back(
        new BgpPeerMock(Ip4Address::from_string("10.10.10.20", ec)));

    AddInetRoute(peers_[0], "blue", "10.9.9.9/32", 100);
    task_util::WaitForIdle();

    BgpRoute *rt1 = InetRouteLookup("blue", "10.9.9.9/32");
    ASSERT_TRUE(rt1 != NULL);
    VERIFY_EQ(peers_[0], rt1->BestPath()->GetPeer());
    const RtReplicated *rts = InetRouteReplicationState("blue", rt1);
    ASSERT_TRUE(rts != NULL);
    ASSERT_TRUE(rts->GetList().size() == 1);
    const InetVpnRoute *rt1_vpn = (const InetVpnRoute *)GetVPNSecondary(rts);
    VERIFY_EQ("10.10.10.10:0", rt1_vpn->GetPrefix().route_distinguisher().ToString());

    DeleteInetRoute(peers_[0], "blue", "10.9.9.9/32");
    AddInetRoute(peers_[1], "blue", "10.9.9.9/32", 100);
    task_util::WaitForIdle();

    rt1 = InetRouteLookup("blue", "10.9.9.9/32");
    ASSERT_TRUE(rt1 != NULL);
    rts = InetRouteReplicationState("blue", rt1);
    ASSERT_TRUE(rts != NULL);
    ASSERT_TRUE(rts->GetList().size() == 1);
    rt1_vpn = (const InetVpnRoute *)GetVPNSecondary(rts);
    VERIFY_EQ("10.10.10.20:0", rt1_vpn->GetPrefix().route_distinguisher().ToString());

    DeleteInetRoute(peers_[1], "blue", "10.9.9.9/32");
    task_util::WaitForIdle();

    VERIFY_EQ(0, RouteCount("blue"));
}

class TestEnvironment : public ::testing::Environment {
    virtual ~TestEnvironment() { }
};

static void SetUp() {
    ControlNode::SetDefaultSchedulingPolicy();
    BgpObjectFactory::Register<BgpConfigManager>(
        boost::factory<BgpIfmapConfigManager *>());
}

static void TearDown() {
    task_util::WaitForIdle();
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Terminate();
}

int main(int argc, char **argv) {
    bgp_log_test::init();
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new TestEnvironment());
    SetUp();
    int result = RUN_ALL_TESTS();
    TearDown();
    return result;
}
