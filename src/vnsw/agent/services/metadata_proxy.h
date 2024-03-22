/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_metadata_proxy_h_
#define vnsw_agent_metadata_proxy_h_

#include "http/client/http_client.h"
#include "http/http_session.h"
#include "oper/vm_interface.h"
#include <map>

class MetadataServer;
class MetadataClient;

class MetadataProxy {
public:
    struct SessionData {
        SessionData(HttpConnection *c, bool conn_close)
            : conn(c), content_len(0), data_sent(0),
              close_req(conn_close), header_end(false) {}

        HttpConnection *conn;
        uint32_t content_len;
        uint32_t data_sent;
        bool close_req;
        bool header_end;
    };

    struct MetadataStats {
        MetadataStats() { Reset(); }
        void Reset() { requests = responses = proxy_sessions = internal_errors = 0; }

        uint32_t requests;
        uint32_t responses;
        uint32_t proxy_sessions;
        uint32_t internal_errors;
    };

    typedef std::map<HttpSession *, SessionData> SessionMap;
    typedef std::pair<HttpSession *, SessionData> SessionPair;
    typedef std::map<HttpConnection *, HttpSession *> ConnectionSessionMap;
    typedef std::pair<HttpConnection *, HttpSession *> ConnectionSessionPair;
    typedef boost::intrusive_ptr<HttpSession> HttpSessionPtr;

    MetadataProxy(ServicesModule *module, const std::string &secret);
    virtual ~MetadataProxy();
    void CloseSessions();
    void Shutdown();

    void HandleMetadataRequest(HttpSession *session, const HttpRequest *request);
    void HandleMetadataResponse(HttpConnection *conn, HttpSessionPtr session,
                                std::string &msg, boost::system::error_code &ec);

    void OnServerSessionEvent(HttpSession *session, TcpSession::Event event);
    void OnClientSessionEvent(HttpClientSession *session, TcpSession::Event event);

    const MetadataStats &metadatastats() const { return metadata_stats_; }
    void ClearStats() { metadata_stats_.Reset(); }

    /// @brief Returns index of a network device that is specified by vhost_name
    /// @param vhost_name is a name of a network device
    /// @return integer index of a network device 
    int VhostIndex(const std::string& vhost_name);

    /// @brief Adds an IP address specified in vhost_ll_ip to vhost0 interface
    /// inet6 addresses
    /// @param vhost_ll_ip is a new IP address for vhost0 interface
    /// @return true if address has been successfully bound to the
    /// interface and false otherwise
    bool NetlinkGetVhostIp(Ip6Address& vhost_ll_ip);

    /// @brief Adds a new neighbour (an arp entry) with given IP and MAC
    /// addresses
    /// @param nb_ip - the IP address of a neighbour
    /// @param via_mac - the MAC address of a neighbour
    void NetlinkAddVhostNb(const IpAddress& nb_ip, const MacAddress& via_mac);

    /// @brief Adds routes to internal addresses of VM's interfaces
    /// @param intf_addr - the internal IP address assigned to an interface
    /// (metadata IP)
    void NetlinkAddInterfaceRoute(const IpAddress& intf_addr);

    /// @brief Advertises routes to a given vm-interface via a given IPv6
    /// address. Routes are announced in the fabric VRF entry and in a given
    /// VRF entry.
    /// @param vm is a pointer to the vm interface 
    /// @param ll_ip is an IPv6 address (prefix) to announce route to
    /// @param intf_vrf is a pointer to the VRF entry to which the 
    /// vm interface belongs to
    void AdvertiseMetaDataLinkLocalRoutes(const VmInterface* vm,
        const Ip6Address& ll_ip, const VrfEntry* intf_vrf);
    
    /// @brief Deletes an announced earlier route to a vm interface via
    /// a given IPv6 address
    /// @param vm_intf is a pointer to the vm interface for which route
    /// should be deleted
    void DeleteMetaDataLinkLocalRoute(const VmInterface* vm_intf);
    
    /// @brief Returns an IPv6 address of the Metadata TF link local service
    /// @return Ip6Address structure containing the IPv6 address
    const Ip6Address& Ipv6ServiceAddress() const;

    /// @brief Initializes an HTTP server for IPv6 requests
    void InitializeHttp6Server(const VmInterface *vhost0);

    /// @brief Advertises the LL route to vhost0 interface in the fabric VRF
    /// instance
    void AdvertiseVhostRoute();


    /// @brief Deletes the LL route to vhost0 interface in the fabric VRF
    /// instance
    void DeleteVhostRoute();

    /// @brief A callback that is invoked each time when a VRF entry is
    /// modified: added, changed or deleted
    /// @param part is a pointer to a table partititon containing corresponding
    /// VRF entry
    /// @param e is a pointer to the modified VrfEntry
    void OnAVrfChange(DBTablePartBase *part, DBEntryBase *e);

    /// @brief A callback that is invoked each time when a route is modified
    /// in the fabric policy VRF inet4 unicast table. The callback is used
    /// to update IPv6 address of the metadata link local service. IPv6 address
    /// of the service is calculated as: FE80 + IPv4 address, e.g. for 
    /// IPv4 169.254.169.254, the IPv6 address is FE80::A9FE:A9FE
    /// @param part is a pointer to a table partititon containing corresponding
    /// modified route
    /// @param e is a pointer to the modified InetUnicastRouteEntry
    void OnAFabricRouteChange(DBTablePartBase *part, DBEntryBase *e);

    /// @brief A callback which is invoked everytime any vm interface is 
    /// changed.
    /// Handles deletion of routes associated with a deleted vm interface.
    /// @param part is a pointer to a table partititon containing corresponding
    /// vm interface 
    /// @param e is a pointer to the vm interface
    void OnAnInterfaceChange(DBTablePartBase *part, DBEntryBase *e);

    /// @brief Registers callbacks to intercept Agent's events emerging when a
    /// VRF entry is modified or an Interface is modified
    void RegisterListeners();

    /// @brief Unregisters all callbacks that were registered earlier to
    /// intercept Agent's events: MetadataProxy::OnAVrfChange,
    /// MetadataProxy::OnAFabricPolicyRouteChange,
    /// MetadataProxy::OnAnInterfaceChange
    void UnregisterListeners();

private:
    HttpConnection *GetProxyConnection(HttpSession *session,
        bool conn_close,
        std::string *nova_hostname);
    void CloseServerSession(HttpSession *session);
    void CloseClientSession(HttpConnection *conn);
    void ErrorClose(HttpSession *sesion, uint16_t error);

    ServicesModule *services_;
    std::string shared_secret_;

    /// @brief A pointer to a HTTP server listening on a IPv4 socket
    /// for Metadata requests from tenants / virtual machines
    MetadataServer *http_server_;

    /// @brief A pointer to a HTTP server listening on a IPv6 socket
    /// for Metadata requests from tenants / virtual machines
    MetadataServer *http_server6_;

    MetadataClient *http_client_;
    SessionMap metadata_sessions_;
    ConnectionSessionMap metadata_proxy_sessions_;
    MetadataStats metadata_stats_;

    /// @brief An IPv6 address on which Metadata6 link local service listens on.
    /// We use it instead of IPv4 compute IP.
    Ip6Address ipv6_service_address_;

    /// @brief an ID of a listener (callback) that acts when the VRF Table is
    /// modified
    DBTableBase::ListenerId vrf_table_notify_id_;

    /// @brief an ID of a listener (callback) that acts when a Fabric Policy
    /// VRF entry is modified
    DBTableBase::ListenerId fabric_notify_id_;

    /// @brief an ID of a listener (callback) that acts when an InterfaceTable
    ///entry is modified
    DBTableBase::ListenerId intf_table_notify_id_;

    /// @brief A correspondence table between a name of a vm interface, which
    /// requests a data from the service and the vm interface link local IPv6
    /// address. The table is needed to obtain a link local address of vm
    /// interface in the MetadataProxy::DeleteMetaDataLinkLocalRoute function.
    std::map<std::string, Ip6Address> ll_ipv6_addresses_;

    /// @brief A mutex which prevents simultaneous access to
    /// MetadataProxy::ll_ipv6_addresses_ table and member functions:
    /// MetadataProxy::DeleteMetaDataLinkLocalRoute,
    /// MetadataProxy::AdvertiseMetaDataLinkLocalRoutes
    tbb::mutex ll_ipv6_addr_mutex_;

    DISALLOW_COPY_AND_ASSIGN(MetadataProxy);
};

#endif // vnsw_agent_metadata_proxy_h_
