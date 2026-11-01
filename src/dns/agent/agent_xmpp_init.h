/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 * Copyright Contributors to the OpenSDN project
 */

#ifndef _dns_agent_xmpp_h_
#define _dns_agent_xmpp_h_

class DnsAgentXmppManager {
public:
    static bool Init(bool xmpp_auth_enabled,
                     const std::string &xmpp_server_cert,
                     const std::string &xmpp_server_key,
                     const std::string &xmpp_ca_cert);
    static void Shutdown();
};

#endif // _dns_agent_xmpp_h_
