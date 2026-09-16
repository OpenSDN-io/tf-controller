/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#include <atomic>
#include <iostream>
#include <pthread.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>

#include "base/logging.h"
#include "base/test/task_test_util.h"
#include "testing/gunit.h"

#include "db/db.h"
#include "db/db_table.h"
#include "db/db_entry.h"
#include "db/db_partition.h"

#include "io/event_manager.h"

#include "ksync/ksync_index.h"
#include "ksync/ksync_entry.h"
#include "ksync/ksync_object.h"
#include "ksync/ksync_netlink.h"
#include "ksync/ksync_sock.h"
#include "ksync/ksync_sock_user.h"

#include "vr_types.h"

using namespace std;
using namespace boost::placeholders;

// ---------------------------------------------------------------------------
// Minimal AgentSandeshContext: records the vrouter result code on response.
// ---------------------------------------------------------------------------
class UTSandeshContext : public AgentSandeshContext {
public:
    UTSandeshContext() : AgentSandeshContext(), response_code_(0) {}
    virtual ~UTSandeshContext() {}
    virtual int VrResponseMsgHandler(vr_response *resp) {
        response_code_ = resp->get_resp_code();
        if (response_code_ < 0) { SetErrno(-response_code_); return -response_code_; }
        SetErrno(0);
        return 0;
    }
    virtual void IfMsgHandler(vr_interface_req *req) {}
    virtual void NHMsgHandler(vr_nexthop_req *req) {}
    virtual void RouteMsgHandler(vr_route_req *req) {}
    virtual void MplsMsgHandler(vr_mpls_req *req) {}
    virtual void MirrorMsgHandler(vr_mirror_req *req) {}
    virtual void FlowMsgHandler(vr_flow_req *req) {}
    virtual void VrfAssignMsgHandler(vr_vrf_assign_req *req) {}
    virtual void VrfMsgHandler(vr_vrf_req *req) {}
    virtual void VrfStatsMsgHandler(vr_vrf_stats_req *req) {}
    virtual void DropStatsMsgHandler(vr_drop_stats_req *req) {}
    virtual void VxLanMsgHandler(vr_vxlan_req *req) {}
    virtual void VrouterOpsMsgHandler(vrouter_ops *req) {}
    virtual void QosConfigMsgHandler(vr_qos_map_req *req) {}
    virtual void ForwardingClassMsgHandler(vr_fc_map_req *req) {}
    int response_code() const { return response_code_; }
private:
    int response_code_;
};

// Shorthand to read what the mock vrouter currently stores for an interface idx.
static bool MockHasIf(int idx) {
    return KSyncSockTypeMap::GetKSyncSockTypeMap()->if_map.count(idx) != 0;
}
static int MockIfMtu(int idx) {
    return KSyncSockTypeMap::GetKSyncSockTypeMap()->if_map[idx].get_vifr_mtu();
}

// Encode a vr_interface_req. A monotonically increasing generation is stamped
// into vifr_mtu so a test can prove that a *specific* (e.g. the change) message
// reached the datapath, rather than relying on residual entry state.
static int g_encode_gen = 1000;
static int EncodeIf(uint16_t idx, sandesh_op::type op, char *buf, int len,
                    int *stamped_gen) {
    vr_interface_req encoder;
    encoder.set_h_op(op);
    encoder.set_vifr_idx(idx);
    encoder.set_vifr_type(0);
    int gen = ++g_encode_gen;
    encoder.set_vifr_mtu(gen);
    if (stamped_gen) *stamped_gen = gen;
    int error = 0;
    int elen = encoder.WriteBinary((uint8_t *)buf, len, &error);
    assert(error == 0);
    assert(elen <= len);
    return elen;
}

// ---------------------------------------------------------------------------
// Oper DB side
// ---------------------------------------------------------------------------
class VlanTable;
static KSyncObjectManager *object_manager;

class Vlan : public DBEntry {
public:
    struct VlanKey : public DBRequestKey {
        VlanKey(uint16_t tag) : DBRequestKey(), tag_(tag) {}
        virtual ~VlanKey() {}
        uint16_t tag_;
    };
    Vlan(uint16_t tag) : DBEntry(), tag_(tag) {}
    virtual ~Vlan() {}
    bool IsLess(const DBEntry &rhs) const {
        return tag_ < static_cast<const Vlan &>(rhs).tag_;
    }
    virtual string ToString() const { return "Vlan"; }
    virtual void SetKey(const DBRequestKey *k) {
        tag_ = static_cast<const VlanKey *>(k)->tag_;
    }
    virtual KeyPtr GetDBRequestKey() const { return KeyPtr(new VlanKey(tag_)); }
    uint16_t GetTag() const { return tag_; }
private:
    uint16_t tag_;
    friend class VlanTable;
    DISALLOW_COPY_AND_ASSIGN(Vlan);
};

class VlanTable : public DBTable {
public:
    VlanTable(DB *db, const string &name) : DBTable(db, name) {}
    virtual ~VlanTable() {}
    virtual unique_ptr<DBEntry> AllocEntry(const DBRequestKey *k) const {
        const Vlan::VlanKey *key = static_cast<const Vlan::VlanKey *>(k);
        return unique_ptr<DBEntry>(new Vlan(key->tag_));
    }
    virtual DBEntry *Add(const DBRequest *req) {
        return new Vlan(static_cast<Vlan::VlanKey *>(req->key.get())->tag_);
    }
    virtual bool OnChange(DBEntry *entry, const DBRequest *req) { return true; }
    virtual bool Delete(DBEntry *entry, const DBRequest *req) { return true; }
    static VlanTable *CreateTable(DB *db, const string &name) {
        VlanTable *t = new VlanTable(db, name); t->Init(); return t;
    }
private:
    DISALLOW_COPY_AND_ASSIGN(VlanTable);
};

// ---------------------------------------------------------------------------
// KSync side -- DB-driven netlink entry (covers KSyncNetlinkDBEntry).
// ---------------------------------------------------------------------------
class VlanKSyncEntry : public KSyncNetlinkDBEntry {
public:
    explicit VlanKSyncEntry(const VlanKSyncEntry *e)
        : KSyncNetlinkDBEntry(), tag_(e->tag_) {}
    explicit VlanKSyncEntry(const Vlan *v)
        : KSyncNetlinkDBEntry(), tag_(v->GetTag()) {}
    virtual ~VlanKSyncEntry() {}
    virtual bool IsLess(const KSyncEntry &rhs) const {
        return tag_ < static_cast<const VlanKSyncEntry &>(rhs).tag_;
    }
    virtual string ToString() const { return "VlanKSync"; }
    virtual KSyncEntry *UnresolvedReference() { return nullptr; }
    virtual bool Sync(DBEntry *e) { return true; }
    // vr_interface_req is a wide sandesh struct (100+ fields incl. lists): its
    // binary encoding does not fit the base-class default MsgLen() of
    // KSyncEntry::kDefaultMsgSize (512). With 512 the encoder runs out of buffer
    // ("ensureCanWrite: Insufficient space ... Available 0") and AddMsg returns
    // the full needed length > len => assert(msg_len <= len) in
    // KSyncNetlink*Entry::Add() aborts. Mirror the agent, which overrides
    // MsgLen() for interface messages to KSYNC_DEFAULT_MSG_SIZE (4096) --
    // see interface_ksync.h (kDefaultInterfaceMsgSize).
    virtual int MsgLen() { return KSYNC_DEFAULT_MSG_SIZE; }
    virtual int AddMsg(char *buf, int len) {
        add_count_++;
        return EncodeIf(tag_, sandesh_op::ADD, buf, len, &last_gen_);
    }
    virtual int ChangeMsg(char *buf, int len) {
        change_count_++;
        return EncodeIf(tag_, sandesh_op::ADD, buf, len, &last_gen_);
    }
    virtual int DeleteMsg(char *buf, int len) {
        del_count_++;
        return EncodeIf(tag_, sandesh_op::DEL, buf, len, nullptr);
    }
    KSyncDBObject *GetObject() const;
    uint16_t GetTag() const { return tag_; }
    static void Reset() { add_count_ = change_count_ = del_count_ = 0; }
    static int AddCount()    { return add_count_; }
    static int ChangeCount() { return change_count_; }
    static int DelCount()    { return del_count_; }
    static int LastGen()     { return last_gen_; }   // gen of most recent encode
private:
    uint16_t tag_;
    static int add_count_, change_count_, del_count_, last_gen_;
    DISALLOW_COPY_AND_ASSIGN(VlanKSyncEntry);
};
int VlanKSyncEntry::add_count_;
int VlanKSyncEntry::change_count_;
int VlanKSyncEntry::del_count_;
int VlanKSyncEntry::last_gen_;

class VlanKSyncObject : public KSyncDBObject {
public:
    explicit VlanKSyncObject(DBTableBase *table)
        : KSyncDBObject("Vlan KSync", table) {}
    virtual KSyncEntry *Alloc(const KSyncEntry *entry, uint32_t index) {
        VlanKSyncEntry *k =
            new VlanKSyncEntry(static_cast<const VlanKSyncEntry *>(entry));
        last_ = k;
        return static_cast<KSyncEntry *>(k);
    }
    virtual KSyncEntry *DBToKSyncEntry(const DBEntry *e) {
        return static_cast<KSyncEntry *>(
            new VlanKSyncEntry(static_cast<const Vlan *>(e)));
    }
    static void Init(VlanTable *t) { assert(singleton_ == nullptr);
                                     singleton_ = new VlanKSyncObject(t); }
    static void Shutdown() { delete singleton_; singleton_ = nullptr; last_ = nullptr; }
    static VlanKSyncObject *Get() { return singleton_; }
    static VlanKSyncEntry *last() { return last_; }
private:
    static VlanKSyncObject *singleton_;
    static VlanKSyncEntry *last_;
    DISALLOW_COPY_AND_ASSIGN(VlanKSyncObject);
};
VlanKSyncObject *VlanKSyncObject::singleton_;
VlanKSyncEntry  *VlanKSyncObject::last_;
KSyncDBObject *VlanKSyncEntry::GetObject() const { return VlanKSyncObject::Get(); }

// ---------------------------------------------------------------------------
// KSync side -- non-DB netlink entry (covers KSyncNetlinkEntry + msg_len==0).
// Driven manually via KSyncObject::Create/Change/Delete (no DBTable).
// ---------------------------------------------------------------------------
class RawKSyncObject;

class RawKSyncEntry : public KSyncNetlinkEntry {
public:
    explicit RawKSyncEntry(uint16_t tag) : KSyncNetlinkEntry(), tag_(tag) {}
    explicit RawKSyncEntry(const RawKSyncEntry *e)
        : KSyncNetlinkEntry(), tag_(e->tag_) {}
    virtual ~RawKSyncEntry() {}
    virtual bool IsLess(const KSyncEntry &rhs) const {
        return tag_ < static_cast<const RawKSyncEntry &>(rhs).tag_;
    }
    virtual string ToString() const { return "RawKSync"; }
    virtual KSyncEntry *UnresolvedReference() { return nullptr; }
    virtual bool Sync() { return true; }
    // vr_interface_req is a wide sandesh struct (100+ fields incl. lists): its
    // binary encoding does not fit the base-class default MsgLen() of
    // KSyncEntry::kDefaultMsgSize (512). With 512 the encoder runs out of buffer
    // ("ensureCanWrite: Insufficient space ... Available 0") and AddMsg returns
    // the full needed length > len => assert(msg_len <= len) in
    // KSyncNetlink*Entry::Add() aborts. Mirror the agent, which overrides
    // MsgLen() for interface messages to KSYNC_DEFAULT_MSG_SIZE (4096) --
    // see interface_ksync.h (kDefaultInterfaceMsgSize).
    virtual int MsgLen() { return KSYNC_DEFAULT_MSG_SIZE; }
    virtual int AddMsg(char *buf, int len) {
        if (no_send_) return 0;     // exercise the "msg_len==0" no-send branch
        return EncodeIf(tag_, sandesh_op::ADD, buf, len, nullptr);
    }
    virtual int ChangeMsg(char *buf, int len) {
        if (no_send_) return 0;
        return EncodeIf(tag_, sandesh_op::ADD, buf, len, nullptr);
    }
    virtual int DeleteMsg(char *buf, int len) {
        if (no_send_) return 0;
        return EncodeIf(tag_, sandesh_op::DEL, buf, len, nullptr);
    }
    KSyncObject *GetObject() const;
    uint16_t GetTag() const { return tag_; }
    static void set_no_send(bool v) { no_send_ = v; }
private:
    uint16_t tag_;
    static bool no_send_;
    DISALLOW_COPY_AND_ASSIGN(RawKSyncEntry);
};
bool RawKSyncEntry::no_send_;

class RawKSyncObject : public KSyncObject {
public:
    RawKSyncObject() : KSyncObject("Raw KSync") {}
    virtual KSyncEntry *Alloc(const KSyncEntry *entry, uint32_t index) {
        return static_cast<KSyncEntry *>(
            new RawKSyncEntry(static_cast<const RawKSyncEntry *>(entry)));
    }
    static void Init() { assert(singleton_ == nullptr); singleton_ = new RawKSyncObject(); }
    static void Shutdown() { delete singleton_; singleton_ = nullptr; }
    static RawKSyncObject *Get() { return singleton_; }
private:
    static RawKSyncObject *singleton_;
    DISALLOW_COPY_AND_ASSIGN(RawKSyncObject);
};
RawKSyncObject *RawKSyncObject::singleton_;
KSyncObject *RawKSyncEntry::GetObject() const { return RawKSyncObject::Get(); }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
template <typename Cond>
static bool WaitFor(int max_ms, Cond cond) {
    for (int i = 0; i < max_ms / 10; i++) {
        task_util::WaitForIdle();
        if (cond()) return true;
        usleep(10 * 1000);
    }
    task_util::WaitForIdle();
    return cond();
}

static void EnqueueVlan(VlanTable *t, uint16_t tag, DBRequest::DBOperation op) {
    DBRequest req;
    req.oper = op;
    req.key.reset(new Vlan::VlanKey(tag));
    req.data.reset(nullptr);
    t->Enqueue(&req);
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class SockTest : public ::testing::Test {
public:
    virtual void SetUp() {
        VlanKSyncEntry::Reset();
        RawKSyncEntry::set_no_send(false);
        itbl_ = static_cast<VlanTable *>(db_.CreateTable("db.test.vlan.0"));
        VlanKSyncObject::Init(itbl_);
        RawKSyncObject::Init();
    }
    virtual void TearDown() {
        WaitFor(2000, [] { return KSyncSock::Get(0)->WaitTreeSize() == 0; });
        RawKSyncObject::Shutdown();
        db_.RemoveTable(itbl_);
        VlanKSyncObject::Shutdown();
        delete itbl_;
    }
    DB db_;
    VlanTable *itbl_;
};

// (1) DB add -> ACK -> IN_SYNC, then delete -> ACK, observed in the mock.
TEST_F(SockTest, RoundTripAddDelete) {
    ASSERT_FALSE(MockHasIf(10));
    EnqueueVlan(itbl_, 10, DBRequest::DB_ENTRY_ADD_CHANGE);
    ASSERT_TRUE(WaitFor(2000, [] { return MockHasIf(10); }))
        << "interface add never reached the mock vrouter";
    EXPECT_EQ(VlanKSyncEntry::AddCount(), 1);
    ASSERT_TRUE(VlanKSyncObject::last() != nullptr);
    ASSERT_TRUE(WaitFor(2000, [] {
        return VlanKSyncObject::last()->GetState() == KSyncEntry::IN_SYNC;
    })) << "entry stuck in SYNC_WAIT (vr_response not processed)";

    EnqueueVlan(itbl_, 10, DBRequest::DB_ENTRY_DELETE);
    ASSERT_TRUE(WaitFor(2000, [] { return !MockHasIf(10); }))
        << "interface delete never reached the mock vrouter";
    EXPECT_EQ(VlanKSyncEntry::DelCount(), 1);
}

// (2) Change after add. We prove the CHANGE message (not residual add state)
// reached the datapath: the mock's stored vifr_mtu must advance to the value the
// change encoded. IfCount alone or entry-state alone would NOT distinguish this.
TEST_F(SockTest, RoundTripChange) {
    EnqueueVlan(itbl_, 20, DBRequest::DB_ENTRY_ADD_CHANGE);
    ASSERT_TRUE(WaitFor(2000, [] { return MockHasIf(20); }));
    int mtu_after_add = MockIfMtu(20);
    EXPECT_EQ(mtu_after_add, VlanKSyncEntry::LastGen());

    EnqueueVlan(itbl_, 20, DBRequest::DB_ENTRY_ADD_CHANGE);  // -> OnChange -> Change
    ASSERT_TRUE(WaitFor(2000, [&] {
        return VlanKSyncEntry::ChangeCount() >= 1 && MockIfMtu(20) != mtu_after_add;
    })) << "change message did not overwrite the datapath entry";
    EXPECT_EQ(MockIfMtu(20), VlanKSyncEntry::LastGen());     // == the change's gen
    EXPECT_TRUE(WaitFor(2000, [] {
        return VlanKSyncObject::last()->GetState() == KSyncEntry::IN_SYNC;
    }));

    EnqueueVlan(itbl_, 20, DBRequest::DB_ENTRY_DELETE);
    ASSERT_TRUE(WaitFor(2000, [] { return !MockHasIf(20); }));
}

// (3) Sequence numbers allocated; WaitTree drains after every ACK.
TEST_F(SockTest, WaitTreeDrains) {
    for (uint16_t tag = 30; tag < 35; tag++)
        EnqueueVlan(itbl_, tag, DBRequest::DB_ENTRY_ADD_CHANGE);
    ASSERT_TRUE(WaitFor(3000, [] {
        for (int t = 30; t < 35; t++) if (!MockHasIf(t)) return false;
        return true;
    }));
    EXPECT_TRUE(WaitFor(2000, [] { return KSyncSock::Get(0)->WaitTreeSize() == 0; }));
    for (uint16_t tag = 30; tag < 35; tag++)
        EnqueueVlan(itbl_, tag, DBRequest::DB_ENTRY_DELETE);
    ASSERT_TRUE(WaitFor(3000, [] {
        for (int t = 30; t < 35; t++) if (MockHasIf(t)) return false;
        return true;
    }));
}

// (4) Non-DB KSyncNetlinkEntry round trip (covers KSyncNetlinkEntry::*).
TEST_F(SockTest, RawNetlinkEntryRoundTrip) {
    RawKSyncObject *obj = RawKSyncObject::Get();
    RawKSyncEntry key(40);
    KSyncEntry *e = obj->Create(&key);
    ASSERT_TRUE(e != nullptr);
    ASSERT_TRUE(WaitFor(2000, [] { return MockHasIf(40); }));
    EXPECT_TRUE(WaitFor(2000, [e] { return e->GetState() == KSyncEntry::IN_SYNC; }));

    obj->Delete(e);
    ASSERT_TRUE(WaitFor(2000, [] { return !MockHasIf(40); }));
}

// (5) msg_len==0 branch: Add() returns true (synchronous), nothing is sent, the
// entry becomes IN_SYNC WITHOUT any mock interaction. Here immediate IN_SYNC is
// the correct behaviour being tested -- not a false positive.
TEST_F(SockTest, SyncNoSend) {
    RawKSyncObject *obj = RawKSyncObject::Get();
    RawKSyncEntry::set_no_send(true);
    uint32_t before = KSyncSock::Get(0)->WaitTreeSize();

    RawKSyncEntry key(50);
    KSyncEntry *e = obj->Create(&key);
    ASSERT_TRUE(e != nullptr);
    task_util::WaitForIdle();
    EXPECT_EQ(e->GetState(), KSyncEntry::IN_SYNC);   // synchronous success
    EXPECT_FALSE(MockHasIf(50));                      // nothing was sent
    EXPECT_EQ(KSyncSock::Get(0)->WaitTreeSize(), before);  // no outstanding req

    obj->Delete(e);
    task_util::WaitForIdle();
    RawKSyncEntry::set_no_send(false);
}

// ---------------------------------------------------------------------------
static void *AsioRun(void *arg) { static_cast<EventManager *>(arg)->Run(); return nullptr; }

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    LoggingInit();

    EventManager evm;
    boost::asio::io_context &io = *evm.io_service();

    KSyncSockTypeMap::Init(io);
    KSyncSock::SetNetlinkFamilyId(24);
    for (int i = 0; i < KSyncSock::kRxWorkQueueCount; i++)
        KSyncSock::SetAgentSandeshContext(new UTSandeshContext(), i);
    KSyncSock::Start(false);

    object_manager = KSyncObjectManager::Init();
    DB::RegisterFactory("db.test.vlan.0", &VlanTable::CreateTable);

    pthread_t asio_thread;
    assert(pthread_create(&asio_thread, nullptr, &AsioRun, &evm) == 0);

    int ret = RUN_ALL_TESTS();

    KSyncSock::Shutdown();
    KSyncObjectManager::Shutdown();
    for (int i = 0; i < KSyncSock::kRxWorkQueueCount; i++) {
        delete KSyncSock::GetAgentSandeshContext(i);
        KSyncSock::SetAgentSandeshContext(nullptr, i);
    }
    KSyncSockTypeMap::Shutdown();
    evm.Shutdown();
    assert(pthread_join(asio_thread, nullptr) == 0);
    return ret;
}
