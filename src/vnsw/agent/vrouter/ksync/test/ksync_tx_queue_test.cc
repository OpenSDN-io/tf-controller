/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#include <atomic>
#include <pthread.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>

#include "base/logging.h"
#include "base/test/task_test_util.h"
#include "testing/gunit.h"

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

static int EncodeIf(uint16_t idx, sandesh_op::type op, char *buf, int len) {
    vr_interface_req encoder;
    encoder.set_h_op(op);
    encoder.set_vifr_idx(idx);
    encoder.set_vifr_type(0);
    int error = 0;
    int elen = encoder.WriteBinary((uint8_t *)buf, len, &error);
    assert(error == 0);
    assert(elen > 0 && elen <= len);
    return elen;
}

class TxKSyncObject;
class TxKSyncEntry : public KSyncNetlinkEntry {
public:
    explicit TxKSyncEntry(uint16_t tag) : KSyncNetlinkEntry(), tag_(tag) {}
    explicit TxKSyncEntry(const TxKSyncEntry *e) : KSyncNetlinkEntry(), tag_(e->tag_) {}
    virtual bool IsLess(const KSyncEntry &rhs) const {
        return tag_ < static_cast<const TxKSyncEntry &>(rhs).tag_;
    }
    virtual string ToString() const { return "TxKSync"; }
    virtual KSyncEntry *UnresolvedReference() { return NULL; }
    virtual bool Sync() { return true; }
    virtual int MsgLen() { return KSYNC_DEFAULT_MSG_SIZE; }
    virtual int AddMsg(char *b, int l)    { return EncodeIf(tag_, sandesh_op::ADD, b, l); }
    virtual int ChangeMsg(char *b, int l) { return EncodeIf(tag_, sandesh_op::ADD, b, l); }
    virtual int DeleteMsg(char *b, int l) { return EncodeIf(tag_, sandesh_op::DEL, b, l); }
    KSyncObject *GetObject() const;
private:
    uint16_t tag_;
    DISALLOW_COPY_AND_ASSIGN(TxKSyncEntry);
};

class TxKSyncObject : public KSyncObject {
public:
    TxKSyncObject() : KSyncObject("Tx KSync") {}
    virtual KSyncEntry *Alloc(const KSyncEntry *e, uint32_t index) {
        return static_cast<KSyncEntry *>(
            new TxKSyncEntry(static_cast<const TxKSyncEntry *>(e)));
    }
    static void Init() { assert(singleton_ == NULL); singleton_ = new TxKSyncObject(); }
    static void Shutdown() { delete singleton_; singleton_ = NULL; }
    static TxKSyncObject *Get() { return singleton_; }
private:
    static TxKSyncObject *singleton_;
    DISALLOW_COPY_AND_ASSIGN(TxKSyncObject);
};
TxKSyncObject *TxKSyncObject::singleton_;
KSyncObject *TxKSyncEntry::GetObject() const { return TxKSyncObject::Get(); }

template <typename Cond>
static bool WaitFor(int max_ms, Cond cond) {
    for (int i = 0; i < max_ms; i += 10) {
        if (cond()) return true;
        usleep(10 * 1000);
    }
    return cond();
}

class TxQueueTest : public ::testing::Test {
public:
    virtual void SetUp() {
        TxKSyncObject::Init();
        q_ = KSyncSock::Get(0)->send_queue();
        deq0_ = q_->dequeues();
    }
    virtual void TearDown() {
        TxKSyncObject *obj = TxKSyncObject::Get();
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i]->GetState() == KSyncEntry::SYNC_WAIT)
                obj->NotifyEvent(entries_[i], KSyncEntry::ADD_ACK);
        }
        for (size_t i = 0; i < entries_.size(); i++)
            obj->Delete(entries_[i]);
        EXPECT_TRUE(WaitFor(3000, [this] {
            return q_->dequeues() - deq0_ >= 2 * entries_.size() &&
                   q_->queue_len() == 0; }))
            << "tx task did not drain the DEL messages";
        for (size_t i = 0; i < entries_.size(); i++)
            obj->NotifyEvent(entries_[i], KSyncEntry::DEL_ACK);
        entries_.clear();
        EXPECT_EQ(obj->Size(), 0u) << "leaked ksync entries";
        TxKSyncObject::Shutdown();
    }

    const KSyncTxQueue *q_;
    size_t deq0_;
    std::vector<KSyncEntry *> entries_;
};

TEST_F(TxQueueTest, EventFdDrainsQueue) {
    TxKSyncObject *obj = TxKSyncObject::Get();
    for (uint16_t tag = 100; tag < 110; tag++) {
        TxKSyncEntry key(tag);
        entries_.push_back(obj->Create(&key));
    }

    ASSERT_TRUE(WaitFor(3000, [this] { return q_->dequeues() - deq0_ >= 10; }))
        << "event-fd Run() did not dequeue the enqueued requests";
    EXPECT_GE(q_->enqueues(), (size_t)10);
    EXPECT_GE(q_->read_events(), (uint32_t)1);
    EXPECT_GE(q_->write_events(), (uint32_t)1);
    EXPECT_EQ(q_->queue_len(), (size_t)0);
}

static void *AsioRun(void *arg) { static_cast<EventManager *>(arg)->Run(); return NULL; }

static boost::asio::ip::udp::socket *g_drain = NULL;
static int BindDrain(boost::asio::io_context &io) {
    g_drain = new boost::asio::ip::udp::socket(
        io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
    return g_drain->local_endpoint().port();
}

#if defined(__GNUC__) && !defined(__clang__)
extern "C" void __gcov_dump(void) __attribute__((weak));
extern "C" void __gcov_flush(void) __attribute__((weak));
static void FlushCoverage() {
    void (*dump)(void)  = __gcov_dump;
    void (*flush)(void) = __gcov_flush;
    if (dump)        dump();
    else if (flush)  flush();
}
#else
static void FlushCoverage() {}
#endif

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    LoggingInit();

    EventManager evm;
    boost::asio::io_context &io = *evm.io_service();

    int drain_port = BindDrain(io);

    KSyncSockUdp::Init(io, drain_port, "disabled");
    KSyncSock::SetNetlinkFamilyId(24);
    for (int i = 0; i < KSyncSock::kRxWorkQueueCount; i++)
        KSyncSock::SetAgentSandeshContext(new UTSandeshContext(), i);
    KSyncSock::Start(false);

    KSyncObjectManager::Init();

    pthread_t asio_thread;
    assert(pthread_create(&asio_thread, NULL, &AsioRun, &evm) == 0);

    int ret = RUN_ALL_TESTS();
    FlushCoverage();

    KSyncSock::Shutdown();
    KSyncObjectManager::Shutdown();
    for (int i = 0; i < KSyncSock::kRxWorkQueueCount; i++) {
        delete KSyncSock::GetAgentSandeshContext(i);
        KSyncSock::SetAgentSandeshContext(NULL, i);
    }
    evm.Shutdown();
    assert(pthread_join(asio_thread, NULL) == 0);
    delete g_drain;
    return ret;
}
