/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#include <atomic>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/bind/bind.hpp>

#include "db/db.h"
#include "db/db_table.h"
#include "db/db_entry.h"
#include "db/db_partition.h"

#include "base/logging.h"
#include "base/test/task_test_util.h"
#include "testing/gunit.h"

#include "io/event_manager.h"

#include "ksync/ksync_index.h"
#include "ksync/ksync_entry.h"
#include "ksync/ksync_object.h"
#include "ksync/ksync_netlink.h"
#include "ksync/ksync_sock.h"

#include "vr_types.h"
#include "ksync_test_vrouter_response.h"

using namespace std;
using boost::asio::local::stream_protocol;
using namespace boost::placeholders;

static const char *kSockPath = "/tmp/ksync_uds_test.sock";

class UdsVrouter {
public:
    explicit UdsVrouter(boost::asio::io_context &io)
        : io_(io), acceptor_(nullptr), socket_(nullptr), stop_(false) {}

    void Bind() {
        ::unlink(kSockPath);
        acceptor_ = new stream_protocol::acceptor(
            io_, stream_protocol::endpoint(kSockPath));
    }
    void Start() { assert(pthread_create(&tid_, nullptr, &Run, this) == 0); }
    void Stop()  { stop_ = true; }
    void Join()  { pthread_join(tid_, nullptr); }

private:
    bool ReadN(char *buf, size_t n) {
        size_t got = 0; boost::system::error_code ec;
        while (got < n && !stop_) {
            got += socket_->read_some(boost::asio::buffer(buf + got, n - got), ec);
            if (ec) return false;
        }
        return got == n;
    }
    void Serve() {
        socket_ = new stream_protocol::socket(io_);
        boost::system::error_code ec;
        acceptor_->accept(*socket_, ec);
        if (ec) return;

        char hdr[sizeof(struct nlmsghdr)];
        std::vector<char> msg;
        while (!stop_) {
            if (!ReadN(hdr, sizeof(hdr))) break;
            uint32_t total = TestMsgLenOf(hdr);
            uint32_t seqno = TestSeqnoOf(hdr);
            if (total > sizeof(hdr)) {
                msg.resize(total - sizeof(hdr));
                if (!ReadN(msg.data(), msg.size())) break;
            }
            uint32_t nreq = msg.empty() ? 1 :
                TestCountSandeshMsgs(msg.data(), msg.size());
            if (nreq == 0) nreq = 1;
            std::string resp = TestBuildVrResponseFrameN(seqno, 0, nreq);
            boost::asio::write(*socket_, boost::asio::buffer(resp), ec);
            if (ec) break;
        }
        Cleanup();
    }
    void Cleanup() {
        boost::system::error_code e;
        if (socket_)   { socket_->close(e);   delete socket_;   socket_ = nullptr; }
        if (acceptor_) { acceptor_->close(e); delete acceptor_; acceptor_ = nullptr; }
        ::unlink(kSockPath);
    }
    static void *Run(void *o) { static_cast<UdsVrouter *>(o)->Serve(); return nullptr; }

    boost::asio::io_context &io_;
    stream_protocol::acceptor *acceptor_;
    stream_protocol::socket *socket_;
    std::atomic<bool> stop_;
    pthread_t tid_;
};

class Vlan : public DBEntry {
public:
    struct VlanKey : public DBRequestKey {
        VlanKey(uint16_t tag) : DBRequestKey(), tag_(tag) {}
        uint16_t tag_;
    };
    Vlan(uint16_t tag) : DBEntry(), tag_(tag) {}
    bool IsLess(const DBEntry &rhs) const { return tag_ < static_cast<const Vlan &>(rhs).tag_; }
    virtual string ToString() const { return "Vlan"; }
    virtual void SetKey(const DBRequestKey *k) { tag_ = static_cast<const VlanKey *>(k)->tag_; }
    virtual KeyPtr GetDBRequestKey() const { return KeyPtr(new VlanKey(tag_)); }
    uint16_t GetTag() const { return tag_; }
private:
    uint16_t tag_;
    DISALLOW_COPY_AND_ASSIGN(Vlan);
};
class VlanTable : public DBTable {
public:
    VlanTable(DB *db, const string &name) : DBTable(db, name) {}
    virtual unique_ptr<DBEntry> AllocEntry(const DBRequestKey *k) const {
        return unique_ptr<DBEntry>(new Vlan(static_cast<const Vlan::VlanKey *>(k)->tag_));
    }
    virtual DBEntry *Add(const DBRequest *req) {
        return new Vlan(static_cast<Vlan::VlanKey *>(req->key.get())->tag_);
    }
    virtual bool OnChange(DBEntry *e, const DBRequest *req) { return true; }
    virtual bool Delete(DBEntry *e, const DBRequest *req) { return true; }
    static VlanTable *CreateTable(DB *db, const string &name) {
        VlanTable *t = new VlanTable(db, name); t->Init(); return t;
    }
private:
    DISALLOW_COPY_AND_ASSIGN(VlanTable);
};

static int EncodeIf(uint16_t idx, sandesh_op::type op, char *buf, int len) {
    vr_interface_req e;
    e.set_h_op(op); e.set_vifr_idx(idx); e.set_vifr_type(0);
    int error = 0;
    int elen = e.WriteBinary((uint8_t *)buf, len, &error);
    assert(error == 0);
    assert(elen > 0 && elen <= len);
    return elen;
}

class VlanKSyncObject;
class VlanKSyncEntry : public KSyncNetlinkDBEntry {
public:
    explicit VlanKSyncEntry(const VlanKSyncEntry *e) : KSyncNetlinkDBEntry(), tag_(e->tag_) {}
    explicit VlanKSyncEntry(const Vlan *v) : KSyncNetlinkDBEntry(), tag_(v->GetTag()) {}
    virtual bool IsLess(const KSyncEntry &rhs) const {
        return tag_ < static_cast<const VlanKSyncEntry &>(rhs).tag_;
    }
    virtual string ToString() const { return "VlanKSync"; }
    virtual KSyncEntry *UnresolvedReference() { return nullptr; }
    virtual bool Sync(DBEntry *e) { return true; }
    virtual int MsgLen() { return KSYNC_DEFAULT_MSG_SIZE; }
    virtual int AddMsg(char *b, int l)    { add_++; return EncodeIf(tag_, sandesh_op::ADD, b, l); }
    virtual int ChangeMsg(char *b, int l) { return EncodeIf(tag_, sandesh_op::ADD, b, l); }
    virtual int DeleteMsg(char *b, int l) { del_++; return EncodeIf(tag_, sandesh_op::DEL, b, l); }
    KSyncDBObject *GetObject() const;
    static void Reset() { add_ = del_ = 0; }
    static int AddCount() { return add_; }
    static int DelCount() { return del_; }
private:
    uint16_t tag_;
    static int add_, del_;
    DISALLOW_COPY_AND_ASSIGN(VlanKSyncEntry);
};
int VlanKSyncEntry::add_;
int VlanKSyncEntry::del_;

class VlanKSyncObject : public KSyncDBObject {
public:
    explicit VlanKSyncObject(DBTableBase *t) : KSyncDBObject("Vlan KSync", t) {}
    virtual KSyncEntry *Alloc(const KSyncEntry *e, uint32_t index) {
        VlanKSyncEntry *k = new VlanKSyncEntry(static_cast<const VlanKSyncEntry *>(e));
        last_ = k; return static_cast<KSyncEntry *>(k);
    }
    virtual KSyncEntry *DBToKSyncEntry(const DBEntry *e) {
        return static_cast<KSyncEntry *>(new VlanKSyncEntry(static_cast<const Vlan *>(e)));
    }
    static void Init(VlanTable *t) { assert(singleton_ == nullptr); singleton_ = new VlanKSyncObject(t); }
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

class UTSandeshContext : public AgentSandeshContext {
public:
    virtual int VrResponseMsgHandler(vr_response *resp) {
        return (resp->get_resp_code() < 0) ? -resp->get_resp_code() : 0;
    }
    virtual void IfMsgHandler(vr_interface_req *) {}
    virtual void NHMsgHandler(vr_nexthop_req *) {}
    virtual void RouteMsgHandler(vr_route_req *) {}
    virtual void MplsMsgHandler(vr_mpls_req *) {}
    virtual void MirrorMsgHandler(vr_mirror_req *) {}
    virtual void FlowMsgHandler(vr_flow_req *) {}
    virtual void VrfAssignMsgHandler(vr_vrf_assign_req *) {}
    virtual void VrfMsgHandler(vr_vrf_req *) {}
    virtual void VrfStatsMsgHandler(vr_vrf_stats_req *) {}
    virtual void DropStatsMsgHandler(vr_drop_stats_req *) {}
    virtual void VxLanMsgHandler(vr_vxlan_req *) {}
    virtual void VrouterOpsMsgHandler(vrouter_ops *) {}
    virtual void QosConfigMsgHandler(vr_qos_map_req *) {}
    virtual void ForwardingClassMsgHandler(vr_fc_map_req *) {}
};

template <typename Cond>
static bool WaitFor(int max_ms, Cond cond) {
    for (int i = 0; i < max_ms; i += 10) {
        if (cond()) return true;
        usleep(10 * 1000);
    }
    return cond();
}
static void EnqueueVlan(VlanTable *t, uint16_t tag, DBRequest::DBOperation op) {
    DBRequest req; req.oper = op;
    req.key.reset(new Vlan::VlanKey(tag)); req.data.reset(nullptr);
    t->Enqueue(&req);
}

class UdsTest : public ::testing::Test {
public:
    virtual void SetUp() {
        VlanKSyncEntry::Reset();
        itbl_ = static_cast<VlanTable *>(db_.CreateTable("db.test.vlan.0"));
        VlanKSyncObject::Init(itbl_);
    }
    virtual void TearDown() {
        WaitFor(5000, [] { return VlanKSyncObject::Get()->Size() == 0; });
        EXPECT_EQ(VlanKSyncObject::Get()->Size(), 0u)
            << "leaked ksync entries; ~KSyncObject would abort";
        db_.RemoveTable(itbl_);
        VlanKSyncObject::Shutdown();
        delete itbl_;
    }
    DB db_;
    VlanTable *itbl_;
};

TEST_F(UdsTest, RoundTrip) {
    EnqueueVlan(itbl_, 10, DBRequest::DB_ENTRY_ADD_CHANGE);
    ASSERT_TRUE(WaitFor(5000, [] {
        return VlanKSyncObject::last() != nullptr &&
               VlanKSyncEntry::AddCount() >= 1;
    })) << "DB notification did not reach VlanKSyncObject";
    EXPECT_EQ(VlanKSyncEntry::AddCount(), 1);
    ASSERT_TRUE(WaitFor(5000, [] {
        return VlanKSyncObject::last()->GetState() == KSyncEntry::IN_SYNC;
    })) << "no vr_response decoded over UDS -> entry stuck in SYNC_WAIT";

    EnqueueVlan(itbl_, 10, DBRequest::DB_ENTRY_DELETE);
    EXPECT_TRUE(WaitFor(5000, [] { return VlanKSyncEntry::DelCount() == 1; }));
}

TEST_F(UdsTest, Burst) {
    for (uint16_t tag = 20; tag < 25; tag++)
        EnqueueVlan(itbl_, tag, DBRequest::DB_ENTRY_ADD_CHANGE);
    ASSERT_TRUE(WaitFor(5000, [] {
        return VlanKSyncObject::last() &&
               VlanKSyncObject::last()->GetState() == KSyncEntry::IN_SYNC;
    }));
    for (uint16_t tag = 20; tag < 25; tag++)
        EnqueueVlan(itbl_, tag, DBRequest::DB_ENTRY_DELETE);
    EXPECT_TRUE(WaitFor(5000, [] { return VlanKSyncEntry::DelCount() == 5; }));
    EXPECT_TRUE(WaitFor(5000, [] {
        return VlanKSyncObject::Get()->Size() == 0; }));
}

static void *AsioRun(void *arg) { static_cast<EventManager *>(arg)->Run(); return nullptr; }
static UdsVrouter *g_vrouter = nullptr;


#ifdef KSYNC_TEST_GCOV_DUMP
extern "C" void __gcov_dump(void);
static void FlushCoverage() { __gcov_dump(); }
#else
static void FlushCoverage() {}
#endif

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    LoggingInit();

    EventManager evm;
    boost::asio::io_context &io = *evm.io_service();

    g_vrouter = new UdsVrouter(io);
    g_vrouter->Bind();
    g_vrouter->Start();

    pthread_t asio_thread;
    assert(pthread_create(&asio_thread, nullptr, &AsioRun, &evm) == 0);

    KSyncSockUds::Init(io, "disabled", kSockPath);
    for (int i = 0; i < KSyncSock::kRxWorkQueueCount; i++)
        KSyncSock::SetAgentSandeshContext(new UTSandeshContext(), i);
    KSyncSock::Start(false);

    KSyncObjectManager::Init();
    DB::RegisterFactory("db.test.vlan.0", &VlanTable::CreateTable);

    int ret = RUN_ALL_TESTS();
    FlushCoverage();
    fflush(nullptr);
    _exit(ret);
}
