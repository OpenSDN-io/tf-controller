/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#include <atomic>
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
#include "io/test/event_manager_test.h"
#include "ksync_test_vrouter.h"
#include "vr_types.h"
#include "ksync_test_vrouter_response.h"

using namespace std;
using boost::asio::local::stream_protocol;
using namespace boost::placeholders;

static const char *kSockPath = "/tmp/ksync_uds_test.sock";

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

static UdsTestVrouter *g_vrouter = nullptr;


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

    g_vrouter = new UdsTestVrouter(io, kSockPath);
    g_vrouter->Bind();
    g_vrouter->Start();

    ServerThread evm_thread(&evm);
    evm_thread.Start();

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
