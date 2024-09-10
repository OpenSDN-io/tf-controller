/*
 *
 */

#ifndef vnsw_agent_factory_hpp
#define vnsw_agent_factory_hpp

#include <type_traits>
#include <boost/function.hpp>
#include <base/factory.h>
#include <cmn/agent_cmn.h>

class Agent;
class AgentUveBase;
class KSync;
class DB;
class DBGraph;
class IFMapDependencyManager;
class InstanceManager;
class FlowStatsCollector;
class FlowStatsCollectorObject;
class SessionStatsCollector;
class SessionStatsCollectorObject;
class NexthopManager;
class FlowStatsManager;
struct FlowAgingTableKey;

struct AgentStaticObjectFactory : public StaticObjectFactory {

// The overload for FlowStatsCollector (because of references)
template <class Base, class T1, class T2, class T3, class T4, class T5>
static typename FactoryTypes<Base, T1&, int, uint32_t, T2*, uint32_t, T3*, T4*, T5*>::BasePointer
CreateRef(T1 &arg1, int arg2, uint32_t arg3, T2* arg4, uint32_t arg5, T3 *arg6, T4 *arg7, T5 *arg8) {
    return FactoryRecord<Base, T1&, int, uint32_t, T2*, uint32_t, T3*, T4*, T5*>::
        create_func_(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
}

// The overload for SessionStatsCollector (because of references)
template <class Base, class T1, class T2, class T3, class T4>
static typename FactoryTypes<Base, T1&, T2*, uint32_t, T3*, T4*>::BasePointer
CreateRef(T1 &arg1, T2* arg2, uint32_t arg3, T3 *arg4, T4 *arg5) {
    return FactoryRecord<Base, T1&, T2*, uint32_t, T3*, T4*>::
        create_func_(arg1, arg2, arg3, arg4, arg5);
}

};

using AgentSignalRec =
    AgentStaticObjectFactory::FactoryRecord<AgentSignal,
        EventManager*>;
using IFMapDependencyManagerRec =
    AgentStaticObjectFactory::FactoryRecord<IFMapDependencyManager,
        DB *, DBGraph *>;

using InstanceManagerRec =
    AgentStaticObjectFactory::FactoryRecord<InstanceManager,
        Agent*>;
using NexthopManagerRec =
    AgentStaticObjectFactory::FactoryRecord<NexthopManager,
        EventManager *, std::string>;
using KSyncRec =
    AgentStaticObjectFactory::FactoryRecord<KSync,
        Agent*>;

using FlowStatsCollectorRec =
    AgentStaticObjectFactory::FactoryRecord<FlowStatsCollector,
        boost::asio::io_service &, int, uint32_t, AgentUveBase *, uint32_t, FlowAgingTableKey *, FlowStatsManager *, FlowStatsCollectorObject *>;
using SessionStatsCollectorRec =
    AgentStaticObjectFactory::FactoryRecord<SessionStatsCollector,
        boost::asio::io_service&, AgentUveBase *, uint32_t, FlowStatsManager *, SessionStatsCollectorObject *>;
using AgentUveBaseRec =
    AgentStaticObjectFactory::FactoryRecord<AgentUveBase,
        Agent *, uint64_t, uint32_t, uint32_t>;

#endif // vnsw_agent_factory_hpp
