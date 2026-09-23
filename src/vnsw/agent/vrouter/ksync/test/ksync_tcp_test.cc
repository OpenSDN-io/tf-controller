/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 *
 * ksync_tcp_test.cc  (true round-trip)
 * ------------------------------------
 * Covers ksync_sock_tcp.cc end to end. A small in-process TCP "vrouter" accepts
 * the connection, reads each netlink-framed request, and replies with a VALID
 * netlink-framed vr_response carrying the same seqno. That makes the agent-side
 * KSyncSockTcp actually decode a response and drive the entry to IN_SYNC.
 *
 * Covered (verified against ksync_sock_tcp.cc):
 *   send    : KSyncSockTcp::{ctor,Init,AllocSession,SendTo,AsyncSendTo,
 *             OnSessionEvent(CONNECT_COMPLETE)}.
 *   receive : KSyncSockTcpSession::{ctor,OnRead}, KSyncSockTcpSessionReader::
 *             {ctor,MsgLength}, KSyncSockTcp::ReceiveMsg -> ProcessDataInline ->
 *             {Validate,GetSeqno,IsMoreData,Decoder,BulkDecoder}.
 *   (KSyncSockTcp::Run/Receive/AsyncReadStart are the alternative dpdk recv-loop
 *    path and are not used when KSyncSockTcp runs as a TcpServer client; they
 *    stay uncovered here.)
 *
 * Not a false positive: KSyncNetlinkDBEntry::Add() returns false (async), so the
 * entry can reach IN_SYNC only after the server's vr_response is received and
 * decoded over TCP. If the transport were broken, IN_SYNC would never happen and
 * the WaitFor below would time out and fail.
 *
 * Earlier fixes retained: corrected KSyncSockTcp::Init() signature; acceptor is
 * bound on the main thread (no server_port race); ephemeral port (no hard-coded
 * 9001).
 *
 * Build note: lives under the agent ksync test tree
 * (controller/src/vnsw/agent/vrouter/ksync/test) and is built with AgentEnv via
 * MakeTestCmd. The agent test env already links the vrouter sandesh (vr_types +
 * the vr_*::Process bodies, from vnswksync) and 'vrutil' (nl_* helpers), so no
 * extra linker wiring is needed.
 */

#include <atomic>
#include <iostream>
#include <unistd.h>

#include <boost/asio.hpp>
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
using namespace boost::asio;
using namespace boost::asio::ip;
using boost::asio::ip::tcp;
using namespace boost::placeholders;

static uint32_t server_port;

class TcpTest : public ::testing::Test {
public:
    virtual void SetUp() {
        VlanKSyncEntry::Reset();
        itbl_ = static_cast<VlanTable *>(db_.CreateTable("db.test.vlan.0"));
        VlanKSyncObject::Init(itbl_);

        KSyncSockTcp *tcp = static_cast<KSyncSockTcp *>(KSyncSock::Get(0));
        ASSERT_TRUE(WaitFor(5000, [tcp] { return tcp->connect_complete(); }))
            << "KSyncSockTcp did not connect";
        tcp->AsyncReadStart();
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

TEST_F(TcpTest, RoundTrip) {
    EnqueueVlan(itbl_, 10, DBRequest::DB_ENTRY_ADD_CHANGE);
    ASSERT_TRUE(WaitFor(5000, [] {
        return VlanKSyncObject::last() != nullptr &&
               VlanKSyncEntry::AddCount() >= 1;
    })) << "DB notification did not reach VlanKSyncObject";
    EXPECT_EQ(VlanKSyncEntry::AddCount(), 1);
    ASSERT_TRUE(WaitFor(5000, [] {
        return VlanKSyncObject::last()->GetState() == KSyncEntry::IN_SYNC;
    })) << "no vr_response decoded over TCP -> entry stuck in SYNC_WAIT";

    EnqueueVlan(itbl_, 10, DBRequest::DB_ENTRY_DELETE);
    EXPECT_TRUE(WaitFor(5000, [] { return VlanKSyncEntry::DelCount() == 1; }));
}

TEST_F(TcpTest, Burst) {
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

static TcpTestVrouter *g_vrouter = nullptr;


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
    io_context &io = *evm.io_service();

    g_vrouter = new TcpTestVrouter(io, tcp::endpoint(tcp::v4(), 0));
    g_vrouter->Bind();
    server_port = g_vrouter->local_endpoint().port();
    g_vrouter->Start();

    ServerThread evm_thread(&evm);
    evm_thread.Start();

    boost::system::error_code ec;
    boost::asio::ip::address ip = boost::asio::ip::address::from_string("127.0.0.1", ec);
    assert(!ec);
    KSyncSockTcp::Init(&evm, ip, server_port, "disabled");
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
