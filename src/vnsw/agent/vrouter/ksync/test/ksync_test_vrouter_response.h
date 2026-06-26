/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */
#ifndef KSYNC_TEST_VROUTER_RESPONSE_H_
#define KSYNC_TEST_VROUTER_RESPONSE_H_

#include <string>
#include <stdint.h>
#include <string.h>
#if defined(__linux__)
#include <linux/netlink.h>
#endif

#include "nl_util.h"
#include "vr_genetlink.h"
#include "vr_types.h"
#include "ksync/ksync_sock.h"

static inline int TestEncodeVrResponse(uint8_t *buf, int buf_len, int code) {
    vr_response encoder;
    int error = 0;
    encoder.set_h_op(sandesh_op::RESPONSE);
    encoder.set_resp_code(code);
    return encoder.WriteBinary(buf, buf_len, &error);
}


static inline uint32_t TestMsgLenOf(const char *hdr) {
    struct nlmsghdr h;
    memcpy(&h, hdr, sizeof(h));
    return h.nlmsg_len;
}

static inline uint32_t TestSeqnoOf(const char *hdr) {
    struct nlmsghdr h;
    memcpy(&h, hdr, sizeof(h));
    return h.nlmsg_seq;
}

class TestCountingSandeshContext : public AgentSandeshContext {
public:
    TestCountingSandeshContext() : count_(0) {}
    uint32_t count() const { return count_; }
    virtual int VrResponseMsgHandler(vr_response *) { count_++; return 0; }
    virtual void IfMsgHandler(vr_interface_req *) { count_++; }
    virtual void NHMsgHandler(vr_nexthop_req *) { count_++; }
    virtual void RouteMsgHandler(vr_route_req *) { count_++; }
    virtual void MplsMsgHandler(vr_mpls_req *) { count_++; }
    virtual void MirrorMsgHandler(vr_mirror_req *) { count_++; }
    virtual void FlowMsgHandler(vr_flow_req *) { count_++; }
    virtual void VrfAssignMsgHandler(vr_vrf_assign_req *) { count_++; }
    virtual void VrfMsgHandler(vr_vrf_req *) { count_++; }
    virtual void VrfStatsMsgHandler(vr_vrf_stats_req *) { count_++; }
    virtual void DropStatsMsgHandler(vr_drop_stats_req *) { count_++; }
    virtual void VxLanMsgHandler(vr_vxlan_req *) { count_++; }
    virtual void VrouterOpsMsgHandler(vrouter_ops *) { count_++; }
    virtual void QosConfigMsgHandler(vr_qos_map_req *) { count_++; }
    virtual void ForwardingClassMsgHandler(vr_fc_map_req *) { count_++; }
private:
    uint32_t count_;
};

static inline std::string TestBuildVrResponseFrameN(uint32_t seqno, int code,
                                                    uint32_t n) {
    std::string out;
    for (uint32_t i = 0; i < n; i++) {
        struct nl_client cl;
        uint8_t *buf = NULL;
        uint32_t buf_len = 0;
        nl_init_generic_client_req(&cl, KSyncSock::GetNetlinkFamilyId());
        if (nl_build_header(&cl, &buf, &buf_len) < 0) {
            nl_free(&cl);
            return std::string();
        }
        struct nlmsghdr *nlh = (struct nlmsghdr *)cl.cl_buf;
        nlh->nlmsg_seq = seqno;
        if (n > 1)
            nlh->nlmsg_flags |= NLM_F_MULTI;
        int el = TestEncodeVrResponse(buf, buf_len, code);
        if (el <= 0) { nl_free(&cl); return std::string(); }
        nl_update_header(&cl, el);
        out.append(reinterpret_cast<const char *>(cl.cl_buf), cl.cl_msg_len);
        nl_free(&cl);
    }
    if (n > 1) {
        struct nlmsghdr done;
        memset(&done, 0, sizeof(done));
        done.nlmsg_seq = seqno;
        done.nlmsg_type = NLMSG_DONE;
        done.nlmsg_len = NLMSG_HDRLEN;
        done.nlmsg_flags = 0;
        out.append(reinterpret_cast<const char *>(&done), NLMSG_HDRLEN);
    }
    return out;
}

static inline std::string TestBuildVrResponseFrame(uint32_t seqno, int code) {
    return TestBuildVrResponseFrameN(seqno, code, 1);
}

static inline uint32_t TestCountSandeshMsgs(char *payload, size_t len,
                                            uint32_t skip = 8) {
    if (len <= skip) return 0;
    TestCountingSandeshContext ctx;
    uint8_t *buf = reinterpret_cast<uint8_t *>(payload) + skip;
    int buf_len = static_cast<int>(len - skip);
    while (buf_len > 0) {
        int err = 0;
        int decode_len = Sandesh::ReceiveBinaryMsgOne(buf, buf_len, &err, &ctx);
        if (decode_len <= 0)
            break;
        buf += decode_len;
        buf_len -= decode_len;
    }
    return ctx.count();
}

#endif
