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
#include "ksync_test_util.h"

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
