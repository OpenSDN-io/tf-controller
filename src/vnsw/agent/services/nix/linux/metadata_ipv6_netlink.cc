/*
 * Copyright (c) 2023 Matvey Kraposhin
 */
extern "C" {
    #include <errno.h>
    #include <net/if.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #include <linux/netlink.h>
    #include <linux/rtnetlink.h>
    #include <linux/neighbour.h>
    #include <linux/if_addr.h>
    #include <arpa/inet.h>
}

#ifndef  NLM_F_DUMP_FILTERED
#define  NLM_F_DUMP_FILTERED 0x20
#endif

#include <cstring>
#include <cmn/agent_cmn.h>
#include <oper/interface_common.h>
#include "services/metadata_proxy.h"
#include "services/services_init.h"

namespace aux {

/// @brief A wrapper function to insert an attribute
/// into a netlink message
void insert_attr(nlmsghdr* nl_msg,
    unsigned short attr_type, int attr_len, const void* attr_data)  // T-L-V
{
    if (nl_msg == NULL) {
        LOG(ERROR, "NULL pointer in nl_msg in"  // INFO | DEBUG
        " void insert_attr, metadata_ipv6_netlink.cc");
        return;
    }
    if (attr_data == NULL) {
        LOG(ERROR, "NULL pointer in attr_data in"
        " void insert_attr, metadata_ipv6_netlink.cc");
        return;
    }

    struct rtattr *payload = 
        (struct rtattr*)(((char*)nl_msg) + NLMSG_ALIGN(nl_msg->nlmsg_len));
    payload->rta_type = attr_type;
    payload->rta_len =  RTA_LENGTH(attr_len);
    std::memcpy
    (
        RTA_DATA(payload),
        attr_data, attr_len);
    nl_msg->nlmsg_len = NLMSG_ALIGN(nl_msg->nlmsg_len) + payload->rta_len;
}


/// @brief A class template to store Netlink request data
template <class T>
struct NetlinkRequest
{
    /// @brief A structure carrying the main netlink header
    nlmsghdr nl_message_hdr;

    /// @brief A structure carrying the request-specific header
    T nl_req_hdr;

    /// @brief A block of memory of memory carrying attributes
    /// of this request
    char payload[16384];

    /// @brief A reference to the total length of this Netlink request
    unsigned int& msg_len;

    /// @brief Returns a pointer to the Netlink message, generated
    /// using this class
    msghdr* message (sockaddr_nl& s_nl) {
        if (this->msg_len < NLMSG_LENGTH(sizeof(T))) {
            LOG(ERROR, " too short msg_len, "
            "NetlinkRequest::message, metadata_ipv6_netlink.cc, "<<
            "this->msg_len = " << this->msg_len <<
            ", NLMSG_LENGTH(sizeof(T)) = " << NLMSG_LENGTH(sizeof(T)));
            return NULL;
        }

        iov_.iov_len = this->msg_len;
        msg_ptr_.reset(new msghdr);
        std::memset(msg_ptr_.get(), 0, sizeof(msghdr));
        msg_ptr_->msg_name    = &s_nl;
        msg_ptr_->msg_namelen = sizeof(s_nl);
        msg_ptr_->msg_iov     = &iov_;
        msg_ptr_->msg_iovlen  = 1;  // 1 iov entry

        return msg_ptr_.get();
    }

    /// @brief Inserts an attribute stored in attr_data into this
    /// Netlink reequest
    void insert_attr(unsigned short attr_type,
        int attr_len, const void* attr_data) {
        aux::insert_attr(&nl_message_hdr, attr_type, attr_len, attr_data);
    }

    /// @brief Creates new blank request
    NetlinkRequest() : msg_len(nl_message_hdr.nlmsg_len) {
        std::memset(&nl_message_hdr, 0, sizeof(nl_message_hdr));
        std::memset(&nl_req_hdr, 0, sizeof(nl_req_hdr));
        std::memset(&payload, 0, sizeof(payload));

        iov_.iov_base = &nl_message_hdr;
    }

    /// @brief NetlinkRequest dtor
    ~NetlinkRequest() {}

private:

    /// @brief Disallow copy ctor
    NetlinkRequest(const NetlinkRequest<T>&);

    /// @brief Disallow assignment operator
    const NetlinkRequest& operator = (const NetlinkRequest<T>&);

    /// @brief An iovec struct to keep headers with data
    iovec iov_;

    /// @brief A pointer to the Netlink message header
    std::auto_ptr<msghdr> msg_ptr_;
};


/// @brief A storage for the Netlink socket descriptor
/// @brief and for the Netlink socket sockaddr structure
class NetlinkSocket {

private:

    /// @brief Forbid the copy ctor
    NetlinkSocket(const NetlinkSocket& nl);

    /// @brief Forbid the copy assignment
    const NetlinkSocket& operator = (const NetlinkSocket& nl);

private:

    /// @brief A Netlink socket ID (descriptor)
    int socket_fd;

    /// @brief A Netlink socket sockaddr structure
    sockaddr_nl local_sock_addr;

public:

    /// @brief Creates a socket and binds it with AF_NETLINK family
    /// @brief and the kernel process
    NetlinkSocket() {
        socket_fd = -1;
        // open a socket
        socket_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (socket_fd < 0) {
            LOG(ERROR, "An error has occured during opening the socket"
            "NetlinkSocket::NetlinkSocket, metadata_ipv6_netlink.cc, "
            "errno = " << errno << std::endl);
            return;
        }

        // bind the socket to an address
        local_sock_addr.nl_family = AF_NETLINK;
        local_sock_addr.nl_groups = 0;  // no multicast
        local_sock_addr.nl_pad    = 0;  // not used here
        local_sock_addr.nl_pid    = 0;
        if (bind(socket_fd, 
            (sockaddr*)(&local_sock_addr), sizeof(local_sock_addr)) < 0) {
            LOG(ERROR, "An error has occured during binding the socket"
            "NetlinkSocket::NetlinkSocket, metadata_ipv6_netlink.cc, "
            "errno = " << errno << std::endl);
            close(socket_fd);
        }
    }

    /// @brief Closes the Netlink connection
    ~NetlinkSocket() {
        int close_res = close(socket_fd);
        if (close_res) {
            LOG(ERROR, "An error has occured during closing the socket"
            "NetlinkSocket::NetlinkSocket, metadata_ipv6_netlink.cc, "
            "errno = " << errno << std::endl);
        }
    }

    /// @brief Returns a reference to the Netlink socket descriptor
    const int& fd() const {
        return socket_fd;
    }

    /// @brief Returns a reference to the Netlink socket sockaddr struct
    sockaddr_nl& netlink_socket() {
        return local_sock_addr;
    }
};

struct NetlinkResponse {

    /// @brief A pointer to the begin of the response
    struct nlmsghdr *resp_nh_;

    /// @brief The number of received bytes in the response
    int n_recv_;

    /// @brief contains message with response
    struct msghdr msg_;

    /// @brief Contains response data
    struct iovec iov_;

    /// @brief A buffer for response data
    char buffer_[16384];

private:

    /// @brief Forbid default ctor for NetlinkResponse
    NetlinkResponse();

public:

    /// @brief Construct a NetlinkResponse object from the request
    /// message and the Netlink socket
    NetlinkResponse(msghdr* req_msg, NetlinkSocket& nl_sock) {
        resp_nh_ = NULL;
        memset(buffer_, 0, sizeof(buffer_));
        memset(&iov_, 0, sizeof(iov_));

        msg_.msg_name = req_msg->msg_name;
        msg_.msg_namelen = req_msg->msg_namelen;
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;
        iov_.iov_base = buffer_;
        iov_.iov_len = sizeof(buffer_);
        n_recv_ = recvmsg(nl_sock.fd(), &msg_, 0);
        if (n_recv_ < 0) {
            LOG(ERROR, "Error receiving message from netlink: "
                << strerror(errno) << std::endl);
        } else {
            resp_nh_ = reinterpret_cast<struct nlmsghdr *>(&buffer_[0]);
        }
    }

    /// @brief NetlinkResponse dtor
    ~NetlinkResponse() {}

    /// @brief Iterates over all response parts
    template <class T>
    int response_iterate(const T& handler_func) {
        for (;
            NLMSG_OK(resp_nh_, n_recv_);
            resp_nh_ = NLMSG_NEXT(resp_nh_, n_recv_)) {
                int handle_res = handler_func(resp_nh_);
                if (handle_res >= 0) {
                    return handle_res;
                }
        }
        return -1;
    }
};

struct IPv6Reader {

    explicit IPv6Reader(const unsigned int itf_idx)
    :
        vhost_idx_(itf_idx) {
    }

    unsigned int vhost_idx_;

    mutable Ip6Address vhost_addr_;

    int read_ipv6_address(const struct nlmsghdr *resp_nh) const {
        struct rtattr *resp_attr = NULL;
        struct ifaddrmsg *ifa_resp = NULL;
        int attr_len = 0;
        if (resp_nh->nlmsg_type != RTM_NEWADDR) {
            return -1;
        }
        ifa_resp = static_cast<struct ifaddrmsg *>(NLMSG_DATA(resp_nh));
        if (ifa_resp == NULL) {
            return -1;
        }
        if (ifa_resp->ifa_index != vhost_idx_) {
            return -1;
        }
        attr_len = resp_nh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa_resp));
        for (resp_attr = IFA_RTA(ifa_resp);
            RTA_OK(resp_attr, attr_len);
            resp_attr = RTA_NEXT(resp_attr, attr_len)) {
            if (resp_attr->rta_type == IFA_ADDRESS) {
                char addr_str[256];
                memset(addr_str, 0, 256);
                inet_ntop(AF_INET6,
                    RTA_DATA(resp_attr), addr_str, 256);
                if (addr_str[0] != 0) {
                    vhost_addr_ = Ip6Address::from_string(addr_str);
                    if (!vhost_addr_.is_unspecified() &&
                        vhost_addr_.is_link_local()) {
                        return 1;
                    }
                }
            }
        }
        return -1;
    }

    int operator() (const struct nlmsghdr *resp_nh) const {
        return read_ipv6_address(resp_nh);
    }

private:

    IPv6Reader();

    IPv6Reader(const IPv6Reader&);

};

} //namespace aux

bool MetadataProxy::NetlinkGetVhostIp(Ip6Address& vhost_ll_ip) {
    // set ip address
    //
    // set device index
    const int dev_idx =
        VhostIndex(services_->agent()->vhost_interface_name().c_str());

    // open a socket
    aux::NetlinkSocket nl_sock;
    int send_n = -1;
    {
        aux::NetlinkRequest<ifaddrmsg> addr_nl_req;
        ifaddrmsg &nl_req_param    = addr_nl_req.nl_req_hdr;
        nl_req_param.ifa_family    = AF_INET6;
        nl_req_param.ifa_prefixlen = 128;
        nl_req_param.ifa_index     = dev_idx;

        nlmsghdr &msg_hdr          = addr_nl_req.nl_message_hdr;
        msg_hdr.nlmsg_type         = RTM_GETADDR;
        msg_hdr.nlmsg_flags        = NLM_F_REQUEST | NLM_F_DUMP; // | NLM_F_DUMP_FILTERED;
        addr_nl_req.msg_len = NLMSG_LENGTH(sizeof(nl_req_param));

        msghdr *msg = addr_nl_req.message(nl_sock.netlink_socket());
        send_n = -1;
        if (msg == NULL) {
            return false;
        }

        // send a request to the kernel
        send_n = sendmsg(nl_sock.fd(), msg, 0);
        if (send_n <= 0) {
            LOG(ERROR, "sendmsg failed (address request), "
            "MetadataProxy::NetlinkAddVhostIp, metadata_ipv6_netlink.cc, "
            "errno = " << errno << std::endl);
            return false;
        }

        // read the response
        aux::NetlinkResponse addr_resp(msg, nl_sock);
        aux::IPv6Reader ipv6_reader(dev_idx);
        int ret_value = addr_resp.response_iterate(ipv6_reader);
        if (ret_value > 0) {
            vhost_ll_ip = ipv6_reader.vhost_addr_;
            return true;
        }
    }

    return false;
}



//
// END-OF-FILE
//

