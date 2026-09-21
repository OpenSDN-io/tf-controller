/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#include <atomic>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>

#include "base/logging.h"
#include "testing/gunit.h"

#include "io/event_manager.h"

#include "ksync/ksync_index.h"
#include "ksync/ksync_entry.h"
#include "ksync/ksync_object.h"
#include "ksync/ksync_netlink.h"
#include "ksync/ksync_sock.h"
#include "ksync_test_util.h"
#include "ksync_test_vrouter.h"

#include "vr_types.h"
#include "udp_util.h"

#include "ksync_test_vrouter_response.h"

using namespace std;
using namespace boost::placeholders;

class UdpKSyncObject;
class UdpKSyncEntry : public KSyncNetlinkEntry {
public:
    explicit UdpKSyncEntry(uint16_t tag) : KSyncNetlinkEntry(), tag_(tag) {}
    explicit UdpKSyncEntry(const UdpKSyncEntry *e)
        : KSyncNetlinkEntry(), tag_(e->tag_) {}
    virtual bool IsLess(const KSyncEntry &rhs) const {
        return tag_ < static_cast<const UdpKSyncEntry &>(rhs).tag_;
    }
    virtual string ToString() const { return "UdpKSync"; }
    virtual KSyncEntry *UnresolvedReference() { return nullptr; }
    virtual bool Sync() { return true; }
    virtual int MsgLen() { return KSYNC_DEFAULT_MSG_SIZE; }
    virtual int AddMsg(char *b, int l)    { return KSyncTestEncodeIf(tag_, sandesh_op::ADD, b, l); }
    virtual int ChangeMsg(char *b, int l) { return KSyncTestEncodeIf(tag_, sandesh_op::ADD, b, l); }
    virtual int DeleteMsg(char *b, int l) { return KSyncTestEncodeIf(tag_, sandesh_op::DEL, b, l); }
    KSyncObject *GetObject() const;
private:
    uint16_t tag_;
    DISALLOW_COPY_AND_ASSIGN(UdpKSyncEntry);
};

class UdpKSyncObject : public KSyncObject {
public:
    UdpKSyncObject() : KSyncObject("Udp KSync") {}
    virtual KSyncEntry *Alloc(const KSyncEntry *e, uint32_t index) {
        return static_cast<KSyncEntry *>(
            new UdpKSyncEntry(static_cast<const UdpKSyncEntry *>(e)));
    }
    static void Init() { assert(singleton_ == nullptr); singleton_ = new UdpKSyncObject(); }
    static void Shutdown() { delete singleton_; singleton_ = nullptr; }
    static UdpKSyncObject *Get() { return singleton_; }
private:
    static UdpKSyncObject *singleton_;
    DISALLOW_COPY_AND_ASSIGN(UdpKSyncObject);
};

UdpKSyncObject *UdpKSyncObject::singleton_;
KSyncObject *UdpKSyncEntry::GetObject() const { return UdpKSyncObject::Get(); }

class UdpTest : public ::testing::Test {
public:
    virtual void SetUp() { UdpKSyncObject::Init(); }
    virtual void TearDown() {
        UdpKSyncObject *obj = UdpKSyncObject::Get();
        for (size_t i = 0; i < entries_.size(); i++) {
            KSyncEntry *e = entries_[i];
            if (e->GetState() == KSyncEntry::SYNC_WAIT)
                obj->NotifyEvent(e, KSyncEntry::ADD_ACK);
        }
        for (size_t i = 0; i < entries_.size(); i++)
            obj->Delete(entries_[i]);
        WaitFor(5000, [obj] { return obj->Size() == 0; });
        for (size_t i = 0; i < entries_.size(); i++) {
            KSyncEntry *e = entries_[i];
            if (obj->Size() != 0 && e->GetState() == KSyncEntry::DEL_ACK_WAIT)
                obj->NotifyEvent(e, KSyncEntry::DEL_ACK);
        }
        entries_.clear();
        EXPECT_EQ(obj->Size(), 0u) << "leaked ksync entries";
        UdpKSyncObject::Shutdown();
    }

    std::vector<KSyncEntry *> entries_;
};

TEST_F(UdpTest, WaitTreeRoundTrip) {
    UdpKSyncObject *obj = UdpKSyncObject::Get();
    UdpKSyncEntry key(200);
    KSyncEntry *e = obj->Create(&key);
    entries_.push_back(e);

    ASSERT_TRUE(WaitFor(5000, [e] {
        return e->GetState() == KSyncEntry::IN_SYNC;
    })) << "no uvr reply decoded through the wait-tree path";
    EXPECT_EQ(KSyncSock::Get(0)->WaitTreeSize(), 0u);   // erased on final msg
}

TEST_F(UdpTest, BurstMultiDatagram) {
    UdpKSyncObject *obj = UdpKSyncObject::Get();
    for (uint16_t tag = 210; tag < 215; tag++) {
        UdpKSyncEntry key(tag);
        entries_.push_back(obj->Create(&key));
    }
    ASSERT_TRUE(WaitFor(5000, [this] {
        for (size_t i = 0; i < entries_.size(); i++)
            if (entries_[i]->GetState() != KSyncEntry::IN_SYNC) return false;
        return true;
    })) << "not all entries completed the multi-datagram (UVR_MORE) reply";
    EXPECT_EQ(KSyncSock::Get(0)->WaitTreeSize(), 0u);
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

static void *AsioRun(void *arg) {
    static_cast<EventManager *>(arg)->Run();
    return nullptr;
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    LoggingInit();

    UdpTestVrouter vr;
    assert(vr.Bind());
    assert(vr.Start());

    EventManager evm;
    KSyncSockUdp::Init(*evm.io_service(), vr.port(), "disabled");
    for (int i = 0; i < KSyncSock::kRxWorkQueueCount; i++)
        KSyncSock::SetAgentSandeshContext(new UTSandeshContext(), i);
    KSyncSock::Start(false);

    KSyncObjectManager::Init();

    pthread_t asio_thread;
    assert(pthread_create(&asio_thread, nullptr, &AsioRun, &evm) == 0);

    int ret = RUN_ALL_TESTS();
    FlushCoverage();

    KSyncSock::Shutdown();
    KSyncObjectManager::Shutdown();
    for (int i = 0; i < KSyncSock::kRxWorkQueueCount; i++) {
        delete KSyncSock::GetAgentSandeshContext(i);
        KSyncSock::SetAgentSandeshContext(nullptr, i);
    }
    vr.Stop();
    vr.Join();
    evm.Shutdown();
    pthread_join(asio_thread, nullptr);
    return ret;
}
