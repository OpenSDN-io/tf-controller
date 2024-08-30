/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __ctrlplane__xmpp_factory__
#define __ctrlplane__xmpp_factory__

#include "base/factory.h"

class TcpServer;
class XmppChannelConfig;
class XmppChannelMux;
class XmppConnection;
class XmppClient;
class XmppClientConnection;
class XmppLifetimeManager;
class XmppServer;
class XmppServerConnection;
class XmppStateMachine;

struct XmppStaticObjectFactory : public StaticObjectFactory {
};

using XmppServerConnectionRec =
    XmppStaticObjectFactory::FactoryRecord<XmppServerConnection,
        XmppServer *,
        const XmppChannelConfig *>;

using XmppClientConnectionRec =
    XmppStaticObjectFactory::FactoryRecord<XmppClientConnection,
        XmppClient *,
        const XmppChannelConfig *>;

using XmppStateMachineRec =
    XmppStaticObjectFactory::FactoryRecord<XmppStateMachine,
        XmppConnection *,
        bool,
        bool,
        int>;

using XmppChannelMuxRec =
    XmppStaticObjectFactory::FactoryRecord<XmppChannelMux,
        XmppConnection*>;

using XmppLifetimeManagerRec =
    XmppStaticObjectFactory::FactoryRecord<XmppLifetimeManager,
        int>;

#endif /* defined(__ctrlplane__xmpp_factory__) */

