#include "bgp/bgp_factory.h"


template<> BgpConfigManagerRec::FunctionType
BgpConfigManagerRec::create_func_ = nullptr;
//template<> BgpConfigManagerRec::DefaultLinkType //Is abstract
//BgpConfigManagerRec::default_link_{};

#include "bgp/bgp_membership.h"
template<> BgpMembershipManagerRec::FunctionType
BgpMembershipManagerRec::create_func_ = nullptr;
template<> BgpMembershipManagerRec::DefaultLinkType
BgpMembershipManagerRec::default_link_{};

#include "bgp/bgp_export.h"
template<> BgpExportRec::FunctionType
BgpExportRec::create_func_ = nullptr;
template<> BgpExportRec::DefaultLinkType
BgpExportRec::default_link_{};

#include "bgp/bgp_evpn.h"
template<> EvpnManagerRec::FunctionType
EvpnManagerRec::create_func_ = nullptr;
template<> EvpnManagerRec::DefaultLinkType
EvpnManagerRec::default_link_{};

#include "bgp/bgp_multicast.h"
template<> McastTreeManagerRec::FunctionType
McastTreeManagerRec::create_func_ = nullptr;
template<> McastTreeManagerRec::DefaultLinkType
McastTreeManagerRec::default_link_{};

#include "bgp/bgp_mvpn.h"
template<> MvpnProjectManagerRec::FunctionType
MvpnProjectManagerRec::create_func_ = nullptr;
template<> MvpnProjectManagerRec::DefaultLinkType
MvpnProjectManagerRec::default_link_{};
template<> MvpnManagerRec::FunctionType
MvpnManagerRec::create_func_ = nullptr;
template<> MvpnManagerRec::DefaultLinkType
MvpnManagerRec::default_link_{};

#include "bgp/peer_close_manager.h"
template<> PeerCloseManagerRec::FunctionType
PeerCloseManagerRec::create_func_ = nullptr;
template<> PeerCloseManagerRec::DefaultLinkType
PeerCloseManagerRec::default_link_{};

#include "bgp/routing-instance/peer_manager.h"
template<> PeerManagerRec::FunctionType
PeerManagerRec::create_func_ = nullptr;
template<> PeerManagerRec::DefaultLinkType
PeerManagerRec::default_link_{};

#include "bgp/routing-policy/routing_policy.h"
template<> RoutingPolicyMgrRec::FunctionType
RoutingPolicyMgrRec::create_func_ = nullptr;
template<> RoutingPolicyMgrRec::DefaultLinkType
RoutingPolicyMgrRec::default_link_{};
template<> RoutingPolicyRec::FunctionType
RoutingPolicyRec::create_func_ = nullptr;
template<> RoutingPolicyRec::DefaultLinkType
RoutingPolicyRec::default_link_{};

#include "bgp/routing-instance/rtarget_group_mgr.h"
template<> RTargetGroupMgrRec::FunctionType
RTargetGroupMgrRec::create_func_ = nullptr;
template<> RTargetGroupMgrRec::DefaultLinkType
RTargetGroupMgrRec::default_link_{};

#include "bgp/bgp_peer_close.h"
template<> BgpPeerCloseRec::FunctionType
BgpPeerCloseRec::create_func_ = nullptr;
template<> BgpPeerCloseRec::DefaultLinkType
BgpPeerCloseRec::default_link_{};

#include "bgp/state_machine.h"
template<> StateMachineRec::FunctionType
StateMachineRec::create_func_ = nullptr;
template<> StateMachineRec::DefaultLinkType
StateMachineRec::default_link_{};

#include "bgp/bgp_lifetime.h"
template<> BgpLifetimeManagerRec::FunctionType
BgpLifetimeManagerRec::create_func_ = nullptr;
template<> BgpLifetimeManagerRec::DefaultLinkType
BgpLifetimeManagerRec::default_link_{};

#include "bgp/bgp_session_manager.h"
template<> BgpSessionManagerRec::FunctionType
BgpSessionManagerRec::create_func_ = nullptr;
template<> BgpSessionManagerRec::DefaultLinkType
BgpSessionManagerRec::default_link_{};

#include "bgp/bgp_ribout_updates.h"
template<> RibOutUpdatesRec::FunctionType
RibOutUpdatesRec::create_func_ = nullptr;
template<> RibOutUpdatesRec::DefaultLinkType
RibOutUpdatesRec::default_link_{};

#include "bgp/bgp_peer.h"
template<> BgpPeerRec::FunctionType
BgpPeerRec::create_func_ = nullptr;
template<> BgpPeerRec::DefaultLinkType
BgpPeerRec::default_link_{};

#include "bgp/routing-instance/routing_instance.h"
template<> RoutingInstanceRec::FunctionType
RoutingInstanceRec::create_func_ = nullptr;
template<> RoutingInstanceRec::DefaultLinkType
RoutingInstanceRec::default_link_{};
template<> RoutingInstanceMgrRec::FunctionType
RoutingInstanceMgrRec::create_func_ = nullptr;
template<> RoutingInstanceMgrRec::DefaultLinkType
RoutingInstanceMgrRec::default_link_{};

#include "bgp/bgp_message_builder.h"
template<> BgpMessageBuilderRec::FunctionType
BgpMessageBuilderRec::create_func_ = nullptr;
template<> BgpMessageBuilderRec::DefaultLinkType
BgpMessageBuilderRec::default_link_{};

#include "bgp/xmpp_message_builder.h"
template<> BgpXmppMessageBuilderRec::FunctionType
BgpXmppMessageBuilderRec::create_func_ = nullptr;
template<> BgpXmppMessageBuilderRec::DefaultLinkType
BgpXmppMessageBuilderRec::default_link_{};

//
//
//
