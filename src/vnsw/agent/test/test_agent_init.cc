/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

// in Boost this macro defaults to 6 but we're using FACTORY_TYPE_N8,
// so we need to define it manually
#define BOOST_FUNCTIONAL_FORWARD_ADAPTER_MAX_ARITY 8

#include <boost/functional/forward_adapter.hpp>
#include <base/test/task_test_util.h>

#include <cmn/agent_cmn.h>

#include <init/agent_param.h>
#include <cfg/cfg_init.h>

#include <oper/operdb_init.h>
#include <vrouter/ksync/ksync_init.h>
#include <vrouter/ksync/test/ksync_test.h>
#include <uve/agent_uve.h>
#include <uve/test/agent_uve_test.h>
#include <vrouter/flow_stats/session_stats_collector.h>

#include "test_agent_init.h"
#include <cmn/agent_factory.h>

const uint32_t TestAgentInit::kDefaultInterval;
const uint32_t TestAgentInit::kIncrementalInterval;


TestAgentInit::TestAgentInit() : ContrailInitCommon() {
}

TestAgentInit::~TestAgentInit() {
    ksync_.reset();
    uve_.reset();
    flow_stats_manager_.reset();
}

void TestAgentInit::ProcessOptions
    (const std::string &config_file, const std::string &program_name) {

    ContrailInitCommon::ProcessOptions(config_file, program_name);

    boost::program_options::variables_map var_map = agent_param()->var_map();
    if (var_map.count("disable-vhost")) {
        set_create_vhost(false);
    }

    if (var_map.count("disable-ksync")) {
        set_ksync_enable(false);
    }

    if (var_map.count("disable-services")) {
        set_services_enable(false);
    }

    if (var_map.count("disable-packet")) {
        set_packet_enable(false);
    }
}

void TestAgentInit::ProcessComputeAddress(AgentParam *param) {
    ContrailInitCommon::ProcessComputeAddress(param);
}

/****************************************************************************
 * Initialization routines
 ***************************************************************************/

void TestAgentInit::FactoryInit() {
    AgentStaticObjectFactory::LinkImpl<AgentUveBase,
        AgentUveBaseTest, Agent *, uint64_t, uint32_t, uint32_t>();

    AgentStaticObjectFactory::LinkImpl<KSync,
        KSyncTest, Agent*>();

    AgentStaticObjectFactory::LinkImpl<FlowStatsCollector,
        FlowStatsCollector, boost::asio::io_service &,
        int, uint32_t, AgentUveBase *, uint32_t,
        FlowAgingTableKey *, FlowStatsManager *, FlowStatsCollectorObject *>();

    AgentStaticObjectFactory::LinkImpl<SessionStatsCollector,
        SessionStatsCollector, boost::asio::io_service&,
        AgentUveBase*, uint32_t, FlowStatsManager *,
        SessionStatsCollectorObject *>();
}

// Create the basic modules for agent operation.
// Optional modules or modules that have different implementation are created
// by init module
void TestAgentInit::CreateModules() {
    /* Set timeout to high value so that it doesn't get fired */
    agent()->stats()->set_flow_stats_update_timeout(0x7FFFFFFF);
    ContrailInitCommon::CreateModules();
    pkt0_.reset(new TestPkt0Interface(agent(), "pkt0",
                *agent()->event_manager()->io_service()));
    agent()->pkt()->set_control_interface(pkt0_.get());

    uve_.reset(AgentStaticObjectFactory::Create<AgentUveBase>
               (agent(), AgentUveBase::kBandwidthInterval,
                TestAgentInit::kDefaultInterval,
                TestAgentInit::kIncrementalInterval));
    agent()->set_uve(uve_.get());

    if (agent()->tsn_enabled() == false) {
        stats_collector_.reset(new AgentStatsCollectorTest(
                                   *(agent()->event_manager()->io_service()),
                                   agent()));
        agent()->set_stats_collector(stats_collector_.get());
    }

    flow_stats_manager_.reset(new FlowStatsManager(agent()));
    flow_stats_manager_->Init(agent()->params()->flow_stats_interval(),
            agent()->params()->flow_cache_timeout());
    agent()->set_flow_stats_manager(flow_stats_manager_.get());

    ksync_.reset(AgentStaticObjectFactory::Create<KSync>(agent()));
    agent()->set_ksync(ksync_.get());

    pih_.reset(new PortIpcHandler(agent(), "/tmp"));
    agent()->set_port_ipc_handler(pih_.get());
}

void TestAgentInit::InitDone() {
    ContrailInitCommon::InitDone();
    if (agent()->port_ipc_handler()) {
        agent()->port_ipc_handler()->InitDone();
    }
}

/****************************************************************************
 * Shutdown routines
 ***************************************************************************/
void TestAgentInit::KSyncShutdown() {
    if (agent()->ksync()) {
        KSyncTest *ksync = static_cast<KSyncTest *>(agent()->ksync());
        ksync->Shutdown();
    }
}

void TestAgentInit::UveShutdown() {
    if (agent()->port_ipc_handler()) {
        agent()->port_ipc_handler()->Shutdown();
    }
    if (agent()->uve()) {
        agent()->uve()->Shutdown();
    }
}

void TestAgentInit::StatsCollectorShutdown() {
    if (agent()->stats_collector()) {
        agent()->stats_collector()->Shutdown();
    }
}

void TestAgentInit::FlowStatsCollectorShutdown() {
    if (agent()->flow_stats_manager()) {
        agent()->flow_stats_manager()->Shutdown();
    }
}

void TestAgentInit::WaitForIdle() {
    task_util::WaitForIdle(3);
}
