/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#include "ksync_test_vrouter.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <cassert>
#include <cstring>

#include "udp_util.h"

TestVrouter::TestVrouter() : stop_(false), started_(false) {
}

TestVrouter::~TestVrouter() {
}

bool TestVrouter::Start() {
    started_ = (pthread_create(&thread_, nullptr, &TestVrouter::ThreadMain,
                               this) == 0);
    return started_;
}

void TestVrouter::Stop() {
    stop_ = true;
}

void TestVrouter::Join() {
    if (started_) {
        pthread_join(thread_, nullptr);
        started_ = false;
    }
    Close();
}

void *TestVrouter::ThreadMain(void *arg) {
    static_cast<TestVrouter *>(arg)->Serve();
    return nullptr;
}

UdsTestVrouter::UdsTestVrouter(boost::asio::io_context &io,
                               const std::string &path)
    : NetlinkStreamVrouter<boost::asio::local::stream_protocol>(
          io, boost::asio::local::stream_protocol::endpoint(path)),
      path_(path) {
}

bool UdsTestVrouter::Bind() {
    ::unlink(path_.c_str());
    return NetlinkStreamVrouter<boost::asio::local::stream_protocol>::Bind();
}

void UdsTestVrouter::Close() {
    NetlinkStreamVrouter<boost::asio::local::stream_protocol>::Close();
    ::unlink(path_.c_str());
}

UdpTestVrouter::UdpTestVrouter() : TestVrouter(), fd_(-1), port_(0) {
}

bool UdpTestVrouter::Bind() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        return false;
    }
    socklen_t alen = sizeof(addr);
    ::getsockname(fd_, (struct sockaddr *)&addr, &alen);
    port_ = ntohs(addr.sin_port);
    struct timeval tv = {0, 200 * 1000};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

void UdpTestVrouter::Serve() {
    std::vector<char> buf(KSYNC_DEFAULT_MSG_SIZE * 4);
    char resp[KSYNC_DEFAULT_MSG_SIZE];
    while (!stopping()) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0,
                             (struct sockaddr *)&peer, &plen);
        if (n < 0) {
            continue;
        }
        if (n < (ssize_t)sizeof(struct uvr_msg_hdr)) {
            continue;
        }

        struct uvr_msg_hdr req;
        memcpy(&req, buf.data(), sizeof(req));
        char *payload = buf.data() + sizeof(req);
        size_t payload_len = n - sizeof(req);
        if (req.msg_len < payload_len) {
            payload_len = req.msg_len;
        }

        uint32_t nreq = TestCountSandeshMsgs(payload, payload_len, 0);
        if (nreq == 0) {
            nreq = 1;
        }

        for (uint32_t i = 0; i < nreq; i++) {
            struct uvr_msg_hdr rhdr;
            memset(&rhdr, 0, sizeof(rhdr));
            rhdr.seq_no = req.seq_no;
            rhdr.flags = (i + 1 < nreq) ? UVR_MORE : 0;
            int el = TestEncodeVrResponse(
                (uint8_t *)resp + sizeof(rhdr),
                (int)(sizeof(resp) - sizeof(rhdr)), 0);
            assert(el > 0);
            rhdr.msg_len = el;
            memcpy(resp, &rhdr, sizeof(rhdr));
            ::sendto(fd_, resp, sizeof(rhdr) + el, 0,
                   (struct sockaddr *)&peer, plen);
        }
    }
}

void UdpTestVrouter::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}
