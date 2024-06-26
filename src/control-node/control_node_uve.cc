/*
 * Copyright (c) 2016 Juniper Networks, Inc. All rights reserved.
 */

#include "control_node.h"

#include "base/misc_utils.h"
#include "base/task.h"
#include "base/task_annotations.h"
#include "base/task_trigger.h"
#include "base/timer.h"
#include "bgp/bgp_server.h"
#include "bgp/bgp_xmpp_channel.h"
#include "control-node/sandesh/control_node_types.h"
#include "ifmap/ifmap_server.h"

std::string ControlNode::hostname_;
std::string ControlNode::prog_name_;
std::string ControlNode::self_ip_;
bool ControlNode::test_mode_;

static std::unique_ptr<Timer> node_info_log_timer;
static std::unique_ptr<TaskTrigger> node_info_trigger;

bool ControlNode::ControlNodeInfoLogger(const BgpServer *server,
        const BgpXmppChannelManager *xmpp_channel_mgr,
        const IFMapServer *ifmap_server, const string &build_info) {
    CHECK_CONCURRENCY("bgp::ShowCommand");
    static bool first = true;
    static BgpRouterState state;
    bool change = false;

    state.set_name(server->localname());

    // Send self information.
    if (first) {
        state.set_uptime(UTCTimestampUsec());
        change = true;
    }

    vector<string> ip_list;
    ip_list.push_back(ControlNode::GetSelfIp());
    if (first || state.get_bgp_router_ip_list() != ip_list) {
        state.set_bgp_router_ip_list(ip_list);
        change = true;
    }

    // Send Build information.
    if (first || build_info != state.get_build_info()) {
        state.set_build_info(build_info);
        change = true;
    }

    change |= server->CollectStats(&state, first);
    change |= xmpp_channel_mgr->CollectStats(&state, first);
    change |= ifmap_server->CollectStats(&state, first);

    if (change) {
        assert(!state.get_name().empty());
        BGPRouterInfo::Send(state);

        // Reset changed flags in the uve structure.
        memset(static_cast<void*>(&state.__isset), 0, sizeof(state.__isset));
    }

    /* Release process free memory to back to system */
    int rc = malloc_trim(0);
    if (!rc) {
        LOG(ERROR, "Control-node free memory is not released to system,"
            " rc: " << rc);
    }

    first = false;
    return true;
}

bool ControlNode::ControlNodeInfoLogTimer(TaskTrigger *node_info_trigger) {
    node_info_trigger->Set();
    // Periodic timer. Restart
    return true;
}

void ControlNode::StartControlNodeInfoLogger(
        EventManager &evm, uint64_t period_msecs,
        const BgpServer *bgp_server,
        const BgpXmppChannelManager *xmpp_channel_mgr,
        const IFMapServer *ifmap_server, const string &build_info) {
    node_info_trigger.reset(
        new TaskTrigger(
            boost::bind(&ControlNode::ControlNodeInfoLogger, bgp_server,
                        xmpp_channel_mgr, ifmap_server, build_info),
            TaskScheduler::GetInstance()->GetTaskId("bgp::ShowCommand"), 0));

    node_info_log_timer.reset(TimerManager::CreateTimer(*evm.io_service(),
                                  "ControlNode Info log timer"));

    // Start periodic timer to send BGPRouterInfo UVE.
    node_info_log_timer->Start(period_msecs,
        boost::bind(&ControlNode::ControlNodeInfoLogTimer,
                    node_info_trigger.get()), NULL);
}

void ControlNode::Shutdown() {
    node_info_trigger->set_disable();
    while (node_info_trigger->IsSet())
        usleep(100);
    node_info_trigger.reset();
    if (node_info_log_timer.get()) {
        node_info_log_timer->Cancel();
        while (node_info_log_timer->running() || node_info_log_timer->fired())
            usleep(100);
        TimerManager::DeleteTimer(node_info_log_timer.get());
        node_info_log_timer.release();
    }
}
