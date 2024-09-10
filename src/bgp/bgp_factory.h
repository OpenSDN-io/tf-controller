/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_BGP_FACTORY_H_
#define SRC_BGP_BGP_FACTORY_H_

#include <boost/function.hpp>

#include <string>

#include "base/factory.h"
#include "base/address.h"
#include "bgp/routing-instance/iservice_chain_mgr.h"

class BgpConfigListener;
class BgpConfigManager;
class BgpIfmapConfigManager;
class BgpExport;
class BgpInstanceConfig;
class BgpLifetimeManager;
class BgpMembershipManager;
class BgpMessageBuilder;
class BgpNeighborConfig;
class BgpPeer;
class BgpPeerClose;
class BgpRoutingPolicyConfig;
class BgpServer;
class BgpSessionManager;
class BgpXmppMessageBuilder;
class EventManager;
class EvpnManager;
class EvpnTable;
class ErmVpnTable;
class IPeerClose;
class IRouteAggregator;
class IServiceChainMgr;
class IStaticRouteMgr;
class McastTreeManager;
class MvpnProjectManager;
class MvpnManager;
class MvpnTable;
class PeerManager;
class PeerCloseManager;
class RibOut;
class RibOutUpdates;
class RoutingInstance;
class RoutingInstanceMgr;
class RoutingPolicy;
class RoutingPolicyMgr;
class RTargetGroupMgr;
class StateMachine;


struct BgpStaticObjectFactory : public StaticObjectFactory {
};

#include "bgp/bgp_config_ifmap.h"

using BgpConfigManagerRec =
    BgpStaticObjectFactory::FactoryRecord<BgpConfigManager,
        BgpServer*>;
using BgpMembershipManagerRec =
    BgpStaticObjectFactory::FactoryRecord<BgpMembershipManager,
        BgpServer*>;
using BgpExportRec =
    BgpStaticObjectFactory::FactoryRecord<BgpExport,
        RibOut*>;
using EvpnManagerRec =
    BgpStaticObjectFactory::FactoryRecord<EvpnManager,
        EvpnTable*>;
using McastTreeManagerRec =
    BgpStaticObjectFactory::FactoryRecord<McastTreeManager,
        ErmVpnTable*>;
using MvpnProjectManagerRec =
    BgpStaticObjectFactory::FactoryRecord<MvpnProjectManager,
        ErmVpnTable*>;
using PeerCloseManagerRec =
    BgpStaticObjectFactory::FactoryRecord<PeerCloseManager,
        IPeerClose*>;
using PeerManagerRec =
    BgpStaticObjectFactory::FactoryRecord<PeerManager,
        RoutingInstance*>;
using RoutingInstanceMgrRec =
    BgpStaticObjectFactory::FactoryRecord<RoutingInstanceMgr,
       BgpServer *>;
using RoutingPolicyMgrRec =
    BgpStaticObjectFactory::FactoryRecord<RoutingPolicyMgr,
        BgpServer*>;
using RTargetGroupMgrRec =
    BgpStaticObjectFactory::FactoryRecord<RTargetGroupMgr,
        BgpServer *>;
using StateMachineRec =
    BgpStaticObjectFactory::FactoryRecord<StateMachine,
        BgpPeer *>;
using BgpPeerCloseRec =
    BgpStaticObjectFactory::FactoryRecord<BgpPeerClose,
        BgpPeer *>;
using MvpnManagerRec =
    BgpStaticObjectFactory::FactoryRecord<MvpnManager,
        MvpnTable *, ErmVpnTable *>;
using BgpLifetimeManagerRec =
    BgpStaticObjectFactory::FactoryRecord<BgpLifetimeManager,
        BgpServer *, int>;
using BgpSessionManagerRec =
    BgpStaticObjectFactory::FactoryRecord<BgpSessionManager,
        EventManager *, BgpServer *>;
using RibOutUpdatesRec =
    BgpStaticObjectFactory::FactoryRecord<RibOutUpdates,
        RibOut *, int>;
using BgpPeerRec =
    BgpStaticObjectFactory::FactoryRecord<BgpPeer,
        BgpServer *, RoutingInstance *, const BgpNeighborConfig *>;
using RoutingInstanceRec =
    BgpStaticObjectFactory::FactoryRecord<RoutingInstance,
        std::string, BgpServer *,
        RoutingInstanceMgr *, const BgpInstanceConfig *>;
using RoutingPolicyRec =
    BgpStaticObjectFactory::FactoryRecord<RoutingPolicy,
        std::string, BgpServer *,
        RoutingPolicyMgr *, const BgpRoutingPolicyConfig *>;
using BgpMessageBuilderRec =
    BgpStaticObjectFactory::FactoryRecord<BgpMessageBuilder>;
using BgpXmppMessageBuilderRec =
    BgpStaticObjectFactory::FactoryRecord<BgpXmppMessageBuilder>;


#include "bgp/routing-instance/service_chaining.h"
template<> struct BgpStaticObjectFactory::ParameterCastTo<IServiceChainMgr,SCAddress::INET>
    {using ImplType = ServiceChainMgrInet;};
template<> struct BgpStaticObjectFactory::ParameterCastTo<IServiceChainMgr,SCAddress::INET6>
    {using ImplType = ServiceChainMgrInet6;};
template<> struct BgpStaticObjectFactory::ParameterCastTo<IServiceChainMgr,SCAddress::EVPN>
    {using ImplType = ServiceChainMgrEvpn;};
template<> struct BgpStaticObjectFactory::ParameterCastTo<IServiceChainMgr,SCAddress::EVPN6>
    {using ImplType = ServiceChainMgrEvpn6;};

#include "bgp/routing-instance/static_route.h"
template<> struct BgpStaticObjectFactory::ParameterCastTo<IStaticRouteMgr,Address::INET>
    {using ImplType = StaticRouteMgrInet;};
template<> struct BgpStaticObjectFactory::ParameterCastTo<IStaticRouteMgr,Address::INET6>
    {using ImplType = StaticRouteMgrInet6;};

#include "bgp/routing-instance/route_aggregator.h"
template<> struct BgpStaticObjectFactory::ParameterCastTo<IRouteAggregator,Address::INET>
    {using ImplType = RouteAggregatorInet;};
template<> struct BgpStaticObjectFactory::ParameterCastTo<IRouteAggregator,Address::INET6>
    {using ImplType = RouteAggregatorInet6;};
#endif  // SRC_BGP_BGP_FACTORY_H_
