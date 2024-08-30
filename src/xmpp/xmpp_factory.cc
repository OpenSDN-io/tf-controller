/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "xmpp_factory.h"

#include "xmpp_connection.h"
template<> XmppServerConnectionRec::FunctionType
    XmppServerConnectionRec::create_func_ = nullptr;
template<> XmppServerConnectionRec::DefaultLinkType
    XmppServerConnectionRec::default_link_{};

template<> XmppClientConnectionRec::FunctionType
    XmppClientConnectionRec::create_func_ = nullptr;
template<> XmppClientConnectionRec::DefaultLinkType
    XmppClientConnectionRec::default_link_{};

template<> XmppStateMachineRec::FunctionType
    XmppStateMachineRec::create_func_ = nullptr;
template<> XmppStateMachineRec::DefaultLinkType
    XmppStateMachineRec::default_link_{};

template<> XmppChannelMuxRec::FunctionType
    XmppChannelMuxRec::create_func_ = nullptr;
template<> XmppChannelMuxRec::DefaultLinkType
    XmppChannelMuxRec::default_link_{};

#include "xmpp_lifetime.h"
template<> XmppLifetimeManagerRec::FunctionType
    XmppLifetimeManagerRec::create_func_ = nullptr;
template<> XmppLifetimeManagerRec::DefaultLinkType
    XmppLifetimeManagerRec::default_link_{};

//
//
//

