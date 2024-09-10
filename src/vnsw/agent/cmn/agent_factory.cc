/*
 *
 */

#include "cmn/agent_factory.h"

#include "cmn/agent_signal.h"
template<> AgentSignalRec::FunctionType
    AgentSignalRec::create_func_ = nullptr;
template<> AgentSignalRec::DefaultLinkType
    AgentSignalRec::default_link_{};

#include "oper/ifmap_dependency_manager.h"
template<> IFMapDependencyManagerRec::FunctionType
    IFMapDependencyManagerRec::create_func_ = nullptr;
template<> IFMapDependencyManagerRec::DefaultLinkType
    IFMapDependencyManagerRec::default_link_{};

#include "oper/instance_manager.h"
template<> InstanceManagerRec::FunctionType
    InstanceManagerRec::create_func_ = nullptr;
template<> InstanceManagerRec::DefaultLinkType
    InstanceManagerRec::default_link_{};

#include "nexthop_server/nexthop_manager.h"
template<> NexthopManagerRec::FunctionType
    NexthopManagerRec::create_func_ = nullptr;
template<> NexthopManagerRec::DefaultLinkType
    NexthopManagerRec::default_link_{};

#include "vnsw/agent/uve/agent_uve_base.h"
template<> AgentUveBaseRec::FunctionType
    AgentUveBaseRec::create_func_ = nullptr;

#include "vrouter/ksync/ksync_init.h"
template<> KSyncRec::FunctionType
    KSyncRec::create_func_ = nullptr;

#include "vrouter/flow_stats/flow_stats_collector.h"
template<> FlowStatsCollectorRec::FunctionType
    FlowStatsCollectorRec::create_func_ = nullptr;

#include "vrouter/flow_stats/session_stats_collector.h"
template<> SessionStatsCollectorRec::FunctionType
    SessionStatsCollectorRec::create_func_ = nullptr;

//
//END-OF-FILE
//
