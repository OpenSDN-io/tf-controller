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

#include "vr_types.h"
#include "udp_util.h"

#include "ksync_test_vrouter_response.h"

using namespace std;
using namespace boost::placeholders;

class CountingContext : public UTSandeshContext {
public:
    CountingContext() : n_(0) {}
    uint32_t n() const { return n_; }
    virtual void IfMsgHandler(vr_interface_req *) { n_++; }
private:
    uint32_t n_;
};

static uint32_t CountUvrRequests(char *payload, size_t len) {
    if (len == 0) return 0;
    CountingContext ctx;
    uint8_t *buf = reinterpret_cast<uint8_t *>(payload);
    int buf_len = static_cast<int>(len);
    while (buf_len > 0) {
        int err = 0;
        int decode_len = Sandesh::ReceiveBinaryMsgOne(buf, buf_len, &err, &ctx);
        if (decode_len <= 0)
            break;
        buf += decode_len;
        buf_len -= decode_len;
    }
    return ctx.n();
}

class UdpVrouter {
public:
    UdpVrouter() : fd_(-1), port_(0), stop_(false) {}

    bool Start() {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) return false;
        socklen_t alen = sizeof(addr);
        getsockname(fd_, (struct sockaddr *)&addr, &alen);
        port_ = ntohs(addr.sin_port);
        struct timeval tv = {0, 200 * 1000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return pthread_create(&thread_, nullptr, &UdpVrouter::ThreadFn, this) == 0;
    }
    void Stop() { stop_ = true; }
    void Join() { pthread_join(thread_, nullptr); if (fd_ >= 0) close(fd_); }
    int port() const { return port_; }

private:
    static void *ThreadFn(void *arg) {
        static_cast<UdpVrouter *>(arg)->Serve();
        return nullptr;
    }

    void Serve() {
        std::vector<char> buf(KSYNC_DEFAULT_MSG_SIZE * 4);
        char resp[KSYNC_DEFAULT_MSG_SIZE];
        while (!stop_) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            ssize_t n = recvfrom(fd_, buf.data(), buf.size(), 0,
                                 (struct sockaddr *)&peer, &plen);
            if (n < 0) continue;                       // timeout -> poll stop_
            if (n < (ssize_t)sizeof(struct uvr_msg_hdr)) continue;

            struct uvr_msg_hdr req;
            memcpy(&req, buf.data(), sizeof(req));
            char *payload = buf.data() + sizeof(req);
            size_t payload_len = n - sizeof(req);
            if (req.msg_len < payload_len) payload_len = req.msg_len;

            uint32_t nreq = CountUvrRequests(payload, payload_len);
            if (nreq == 0) nreq = 1;

            for (uint32_t i = 0; i < nreq; i++) {
                struct uvr_msg_hdr rhdr;
                memset(&rhdr, 0, sizeof(rhdr));
                rhdr.seq_no = req.seq_no;              // echo the bulk seqno
                rhdr.flags = (i + 1 < nreq) ? UVR_MORE : 0;
                int el = TestEncodeVrResponse(
                    (uint8_t *)resp + sizeof(rhdr),
                    (int)(sizeof(resp) - sizeof(rhdr)), 0 /* success */);
                assert(el > 0);
                rhdr.msg_len = el;
                memcpy(resp, &rhdr, sizeof(rhdr));
                sendto(fd_, resp, sizeof(rhdr) + el, 0,
                       (struct sockaddr *)&peer, plen);
            }
        }
    }

    int fd_;
    int port_;
    pthread_t thread_;
    std::atomic<bool> stop_;
};

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

    UdpVrouter vr;
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
