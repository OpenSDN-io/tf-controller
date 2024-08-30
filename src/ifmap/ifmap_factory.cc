/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include <vector>

#include "ifmap/ifmap_factory.h"

#include "ifmap/ifmap_xmpp.h"
template<> IfmapXmppChannelRec::FunctionType
    IfmapXmppChannelRec::create_func_ = nullptr;
template<> IfmapXmppChannelRec::DefaultLinkType
    IfmapXmppChannelRec::default_link_{};

//
//END-OF-FILE
//
