/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_ksync_test_vrouter_h
#define vnsw_agent_ksync_test_vrouter_h

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include "ksync_test_util.h"
#include "ksync_test_vrouter_response.h"

class TestVrouter {
public:
    virtual ~TestVrouter();

    virtual bool Bind() = 0;
    bool Start();
    void Stop();
    void Join();

protected:
    TestVrouter();

    virtual void Serve() = 0;
    virtual void Close() = 0;
    bool stopping() const { return stop_; }

private:
    std::atomic<bool> stop_;
    std::thread thread_;
};

template <typename Protocol>
class NetlinkStreamVrouter : public TestVrouter {
public:
    typedef typename Protocol::endpoint Endpoint;
    typedef typename Protocol::acceptor Acceptor;
    typedef typename Protocol::socket Socket;

    NetlinkStreamVrouter(boost::asio::io_context &io, const Endpoint &endpoint)
        : TestVrouter(), io_(io), endpoint_(endpoint),
          acceptor_(nullptr), socket_(nullptr) {}
    virtual ~NetlinkStreamVrouter() {}

    virtual bool Bind() {
        acceptor_ = new Acceptor(io_, endpoint_);
        return true;
    }

    Endpoint local_endpoint() const { return acceptor_->local_endpoint(); }

protected:
    virtual void Serve() {
        socket_ = new Socket(io_);
        boost::system::error_code ec;
        acceptor_->accept(*socket_, ec);
        if (ec) {
            return;
        }

        char hdr[sizeof(struct nlmsghdr)];
        std::vector<char> msg;
        while (!stopping()) {
            if (!ReadN(hdr, sizeof(hdr))) {
                break;
            }
            uint32_t total = TestMsgLenOf(hdr);
            uint32_t seqno = TestSeqnoOf(hdr);
            if (total > sizeof(hdr)) {
                msg.resize(total - sizeof(hdr));
                if (!ReadN(msg.data(), msg.size())) {
                    break;
                }
            }
            uint32_t nreq = msg.empty() ? 1 :
                TestCountSandeshMsgs(msg.data(), msg.size());
            if (nreq == 0) {
                nreq = 1;
            }
            std::string resp = TestBuildVrResponseFrameN(seqno, 0, nreq);
            boost::asio::write(*socket_, boost::asio::buffer(resp), ec);
            if (ec) {
                break;
            }
        }
        Close();
    }

    virtual void Close() {
        boost::system::error_code ec;
        if (socket_ != nullptr) {
            socket_->close(ec);
            delete socket_;
            socket_ = nullptr;
        }
        if (acceptor_ != nullptr) {
            acceptor_->close(ec);
            delete acceptor_;
            acceptor_ = nullptr;
        }
    }

private:
    bool ReadN(char *buf, size_t n) {
        size_t got = 0;
        boost::system::error_code ec;
        while (got < n && !stopping()) {
            got += socket_->read_some(boost::asio::buffer(buf + got, n - got),
                                      ec);
            if (ec) {
                return false;
            }
        }
        return got == n;
    }

    boost::asio::io_context &io_;
    Endpoint endpoint_;
    Acceptor *acceptor_;
    Socket *socket_;
};

typedef NetlinkStreamVrouter<boost::asio::ip::tcp> TcpTestVrouter;

class UdsTestVrouter
    : public NetlinkStreamVrouter<boost::asio::local::stream_protocol> {
public:
    UdsTestVrouter(boost::asio::io_context &io, const std::string &path);
    virtual ~UdsTestVrouter() {}

    virtual bool Bind();

protected:
    virtual void Close();

private:
    std::string path_;
};

class UdpTestVrouter : public TestVrouter {
public:
    UdpTestVrouter();
    virtual ~UdpTestVrouter() {}

    virtual bool Bind();
    int port() const { return port_; }

protected:
    virtual void Serve();
    virtual void Close();

private:
    int fd_;
    int port_;
};

#endif  // vnsw_agent_ksync_test_vrouter_h
