/*
 * Copyright (c) 2018 Juniper Networks, Inc. All rights reserved.
 * Copyright (c) 2022 - 2026 Matvey Kraposhin.
 * Copyright (c) 2024 - 2026 Elena Zizganova.
 */
#ifndef __AGENT_OPER_VXLAN_ROUTING_H
#define __AGENT_OPER_VXLAN_ROUTING_H

#include <cmn/agent_cmn.h>
#include <cmn/agent.h>
#include <oper/oper_db.h>
#include <base/logging.h>

class BgpPeer;
namespace autogen {
    struct EnetNextHopType;
    struct NextHopType;
}


/// @brief A structure to hold path parameters during
/// the transfer (routes leaking) of data between VRF instances and tables.
/// The structure is designed to use references where it is possible.
struct RouteParameters {

    /// @brief Constructs RouteParameters object from components
    RouteParameters(const IpAddress& nh_addr,
        const MacAddress& mac,
        const VnListType& vns,
        const SecurityGroupList& sgs,
        const CommunityList& comms,
        const TagList& tags,
        const PathPreference& ppref,
        const EcmpLoadBalance& ecmp,
        uint64_t seq_n):
        nh_addresses_(1, nh_addr),
        nh_addr_(nh_addresses_.at(0)),
        nh_mac_(mac),
        vn_list_(vns), sg_list_(sgs),
        communities_(comms), tag_list_(tags),
        path_preference_(ppref),
        ecmp_load_balance_(ecmp),
        sequence_number_(seq_n) {
    }

    /// @brief Copy constructor
    RouteParameters(const RouteParameters &rp):
        nh_addresses_(rp.nh_addresses_),
        nh_addr_(rp.nh_addr_),
        nh_mac_(rp.nh_mac_),
        vn_list_(rp.vn_list_), sg_list_(rp.sg_list_),
        communities_(rp.communities_), tag_list_(rp.tag_list_),
        path_preference_(rp.path_preference_),
        ecmp_load_balance_(rp.ecmp_load_balance_),
        sequence_number_(rp.sequence_number_) {
    }

    // template <class ITEM_T
    // static std::vector<typename ITEM_T> ItemToVector(const ITEM_T& item) {
    //     return std::vector<ITEM_T>();
    // }

    // template<class ITEM_T>
    // RouteParameters(const ITEM_T& item,
    //     const VnListType& vns,
    //     const SecurityGroupList& sgs,
    //     const TagList& tags,
    //     const PathPreference& ppref,
    //     const EcmpLoadBalance& ecmp,
    //     uint64_t seq_n):
    //     nh_addr_(nh_addr),
    //     vn_list_(vns), sg_list_(sgs),
    //     tag_list_(tags), path_preference_(ppref),
    //     ecmp_load_balance_(ecmp),
    //     sequence_number_(seq_n) {
    // }

    /// @brief A list of nexthops IP addreses of a composite tunnel.
    const std::vector<IpAddress> nh_addresses_;

    /// @brief A nexthop IP address of the tunnel. Contains first IP address
    /// of nh_addresses_ in case of a composite tunnel.
    const IpAddress& nh_addr_;

    /// @brief A nexthop MAC address (usually it is a MAC of the router).
    const MacAddress& nh_mac_;

    /// @brief A list of path destination virtual networks used in policy
    /// lookups.
    const VnListType& vn_list_;

    /// @brief A list of security groups.
    const SecurityGroupList& sg_list_;

    /// @brief A list of communities.
    const CommunityList& communities_;

    /// @brief A list of tags.
    const TagList& tag_list_;

    /// @brief A reference to the PathPreference of the path
    const PathPreference& path_preference_;

    /// @brief A reference to EcmpLoadBalance of the path
    const EcmpLoadBalance& ecmp_load_balance_;

    /// @brief An ID of sequence
    uint64_t sequence_number_;

private:

    /// @brief Disallow default constructor
    RouteParameters();
};

/// @brief This state tracks inet and EVPN table listeners.
/// It establishes link between inet tables of a bridge VRF instance
/// from where routes leak and the EVPN table of a routing VRF instance
/// into which the routes leak, see VxlanRoutingManager and 
/// VxlanRoutingVrfMapper classes for more information.
struct VxlanRoutingState : public DBState {

    /// @brief Construct new instance using the given VxlanRoutingManager and
    /// VRF instance (VrfEntry).
    VxlanRoutingState(VxlanRoutingManager *mgr,
                      VrfEntry *vrf);

    /// @brief Destroys an instance.
    virtual ~VxlanRoutingState();

    /// @brief ID of a listener that tracks changes in the IPv4 Inet
    /// table of a bridge VRF instance
    DBTable::ListenerId inet4_id_;

    /// @brief ID of a listener that tracks changes in the IPv6 Inet
    /// table of a bridge VRF instance
    DBTable::ListenerId inet6_id_;

    /// @brief ID of a listener that tracks changes in the EVPN
    /// table of a routing VRF instance
    DBTable::ListenerId evpn_id_;

    /// @brief A pointer to the IPv4 Inet table of a bridge VRF instance
    AgentRouteTable *inet4_table_;

    /// @brief A pointer to the IPv6 Inet table of a bridge VRF instance
    AgentRouteTable *inet6_table_;

    /// @brief A pointer to the EVPN table of a routing VRF instance
    AgentRouteTable *evpn_table_;
};

/// @brief This state tracks all virtual machine interfaces (VmInterface)
/// attached to a Logical Router (LR). It establishes links
/// between VmInterfaces connected to a LR as
/// router's ports on behalf of bridge virtual networks (VnEntry),
/// see VxlanRoutingManager and
/// VxlanRoutingVrfMapper classes for more information.
struct VxlanRoutingVnState : public DBState {

    /// A typedef for a set of pointers to VmInterface.
    typedef std::set<const VmInterface *> VmiList;

    /// A typedef for the iterator of VxlanRoutingVnState::VmiList.
    typedef VmiList::iterator VmiListIter;

    /// Constructs new instance using VxlanRoutingManager.
    VxlanRoutingVnState(VxlanRoutingManager *mgr);

    /// @brief Destroys a VxlanRoutingVnState object.
    virtual ~VxlanRoutingVnState();

    /// @brief Adds a VmInterface (LR port) to a Logical Router and connects
    /// the given VirtualNetwork (to which the VmInterface belongs to) to the
    /// LR.
    void AddVmi(const VnEntry *vn, const VmInterface *vmi);

    /// @brief Deletes the VmInterface from set of connected interfaces
    ///  and disconnects the given VirtualNetwork from the Logical Router.
    void DeleteVmi(const VnEntry *vn, const VmInterface *vmi);

    /// @brief Returns the UUID of the Logical Router.
    boost::uuids::uuid logical_router_uuid() const;

    /// @brief A list of VmInterface (router's ports) connected to a Logical
    /// Router (LR)
    std::set<const VmInterface *> vmi_list_;

    /// @brief Returns true when state is associated with a routing
    /// VirtualNetwork
    bool is_routing_vn_;

    /// @brief A  UUID of the Logical Router.
    boost::uuids::uuid logical_router_uuid_;

    /// @brief Holds a reference to a VrfEntry when VirtualNetwork's
    /// reference stored in VrfGet() is null
    VrfEntryRef vrf_ref_;

    /// @brief A pointer to the instance of VxlanRoutingManager
    VxlanRoutingManager *mgr_;
};

/// @brief Tracks movement of a VmInterface among LRs. This is used
/// to associate VmInterface with a LR and a VN, see VxlanRoutingManager and
/// VxlanRoutingVrfMapper classes for more information.
struct VxlanRoutingVmiState : public DBState {

    /// @brief Constructs new instance of VxlanRoutingVmiState
    VxlanRoutingVmiState();

    /// @brief Destroys an instance of VxlanRoutingVmiState
    virtual ~VxlanRoutingVmiState();

    /// @brief Reference (smart pointer) to the virtual network
    /// (VirtualNetwork) to which VmInterface belongs to
    VnEntryRef vn_entry_;

    /// @brief UUID of the LR to which this VmInterface is connected.
    boost::uuids::uuid logical_router_uuid_;
};

/**
 * VxlanRoutingRouteWalker
 * Incarnation of AgentRouteWalker. 
 * 
 */
/// @brief Listens to Inet routes in a bridge VRF instance.
/// Started when l3vrf is added/deleted or
/// when a bridge VRF is attached / detached to/from the LR.
class VxlanRoutingRouteWalker : public AgentRouteWalker {
public:

    /// @brief Constructs a new instance using the given name, pointer to
    /// the VxlanRoutingManager and pointer to the Agent.
    VxlanRoutingRouteWalker(const std::string &name,
        VxlanRoutingManager *mgr, Agent *agent);

    /// @brief Destructs an instance of VxlanRoutingRouteWalker.
    virtual ~VxlanRoutingRouteWalker();

    /// @brief Runs route leaking process when L3 VRF instance is added/deleted
    /// or when a bridge VRF is attached / detached to/from the LR
    virtual bool RouteWalkNotify(DBTablePartBase *partition, DBEntryBase *e);

private:

    /// @brief A pointer to the VxlanRoutingManager instance.
    VxlanRoutingManager *mgr_;

    DISALLOW_COPY_AND_ASSIGN(VxlanRoutingRouteWalker);
};


/// @brief This class is a storage for operative state of VxLAN logical
/// routers (LRs) defined via Config logical-router element. It stores:
/// - a map between a virtual network (VnEntry*) and an LR's UUID, vn_lr_set_;
/// - a map between an LR's UUID and virtual networks and VRF tables
/// associated with the LR, lr_vrf_info_map_.
/// Along with these it has a walker for traversing EVPN tables due to
/// modification of an LR or associated elements configurative or operative
/// state.
/// (If multiple walks get scheduled for an EVPN table then they are
/// collapsed and only one walk is done).
/// For more information, refer to VxlanRoutingManager class.
class VxlanRoutingVrfMapper {
public:

    /// @brief The structure holds information about virtual networks
    /// connected to a logical router (LR)
    struct RoutedVrfInfo {

        /// @brief A typedef to store the list of bridge virtual networks
        /// connected to a LR.
        typedef std::set<const VnEntry *> BridgeVnList;

        /// @brief A typedef to store the correspondence between bridge virtual
        /// network (VirtualNetwork) pointer and a name of it's VRF instance.
        typedef std::map<const VnEntry *, std::string> BridgeVrfNamesList;

        /// @brief A type for iterator of the list of bridge virtual networks
        /// connected to a LR.
        typedef BridgeVnList::iterator BridgeVnListIter;

        /// @brief Constructs an instance of RoutedVrfInfo.
        RoutedVrfInfo() : routing_vn_(),
        routing_vrf_(NULL), bridge_vn_list_() {
        }

        /// @brief Destroys an instance of RoutedVrfInfo.
        virtual ~RoutedVrfInfo() {
        }

        /// @brief A pointer to the routing virtual network (VirtualNetwork)
        /// connected to a LR.
        const VnEntry *routing_vn_;

        /// @brief A pointer to the routing VRF instance (L3 VRF)
        /// connected to a LR.
        VrfEntry *routing_vrf_;

        /// @brief The list of bridge virtual networks (VirtualNetwork)
        /// connected to a LR.
        BridgeVnList bridge_vn_list_;

        /// @brief The list of bridge virtual networks (VirtualNetwork) names
        /// connected to a LR.
        BridgeVrfNamesList bridge_vrf_names_list_;
    };

    /// @brief A typedef to store map between Logical router UUID and
    /// RoutedVrfInfo
    typedef std::map<boost::uuids::uuid, RoutedVrfInfo> LrVrfInfoMap;

    /// @brief A typedef for iterator of LrVrfInfoMap
    typedef LrVrfInfoMap::iterator LrVrfInfoMapIter;

    /// @brief A typedef to store map between pointer to VirtualNetwork (a
    /// bridge or routing virtual network connected to some LR) and
    /// the LR's UUID.
    typedef std::map<const VnEntry *, boost::uuids::uuid> VnLrSet;

    /// @brief A typedef for iterator of VnLrSet.
    typedef VnLrSet::iterator VnLrSetIter;

    /// @brief A typedef for a storage of all walkers on Inet tables,
    /// if needed the walk can be restarted
    /// instead of spawning new one for a table.
    typedef std::map<const InetUnicastAgentRouteTable *, DBTable::DBTableWalkRef>
        InetTableWalker;

    /// @brief Constructs a new instance of VxlanRoutingVrfMapper using
    /// the given pointer to VxlanRoutingManager.
    VxlanRoutingVrfMapper(VxlanRoutingManager *mgr);

    /// @brief Destroys an instance of VxlanRoutingVrfMapper().
    virtual ~VxlanRoutingVrfMapper();

    /// @brief Handles completion of route walk in the Inet IPv4 table
    /// of a bridge VRF instance.
    void BridgeInet4RouteWalkDone(DBTable::DBTableWalkRef walk_ref,
                       DBTableBase *partition);

    /// @brief Handles completion of route walk in an Inet IPv6 table
    /// of a bridge VRF instance.
    void BridgeInet6RouteWalkDone(DBTable::DBTableWalkRef walk_ref,
                       DBTableBase *partition);

    /// @brief Handles completion of route walk in the EVPN table
    /// of a routing VRF instance.
    void RoutingVrfRouteWalkDone(DBTable::DBTableWalkRef walk_ref,
                                          DBTableBase *partition);

    /// @brief Attempts to delete the given LR.
    /// @todo better way to release logical router from lr_vrf_info_map_
    /// Easier way will be to add logical router in db and trigger delete of this
    ///via LR delete in same.
    void TryDeleteLogicalRouter(LrVrfInfoMapIter &it);

    /// @brief Determines whether object is empty or not.
    bool IsEmpty() const {
        return ((vn_lr_set_.size() == 0) &&
                (lr_vrf_info_map_.size() == 0));
    }

private:

    /// @brief Allows access to private members for VxlanRoutingManager class.
    friend class VxlanRoutingManager;

    /// @brief Walks Inet tables of all bridge VRF instances connected to
    /// a LR (given in routing_vrf_info parameter).
    void WalkBridgeVrfs(const RoutedVrfInfo &routing_vrf_info);

    /// @brief Walks the EVPN table of the routing VRF instance of a given
    /// LR.
    void WalkRoutingVrf(const boost::uuids::uuid &lr_uuid,
        const VnEntry *vn, bool update, bool withdraw);

    /// @brief Walks given Inet tables (IPv4 and IPv6).
    void WalkBridgeInetTables(InetUnicastAgentRouteTable *inet4,
        InetUnicastAgentRouteTable *inet6);

    /// @brief Find the routing VRF instance using a given virtual network.
    const VrfEntry *GetRoutingVrfUsingVn(const VnEntry *vn);

    /// @brief Find the routing VRF instance using a given route
    /// (AgentRoute).
    const VrfEntry *GetRoutingVrfUsingAgentRoute(const AgentRoute *rt);

    /// @brief Find the routing VRF instance using a given LR UUID.
    const VrfEntry *GetRoutingVrfUsingUuid(const boost::uuids::uuid &lr_uuid);

    /// @brief Find the UUID of the LR using a given route (AgentRoute).
    const boost::uuids::uuid GetLogicalRouterUuidUsingRoute(const AgentRoute *rt);

    /// @brief A pointer to the VxlanRoutingManager instance.
    VxlanRoutingManager *mgr_;

    /// @brief  The map between Logical router UUID and
    /// RoutedVrfInfo
    LrVrfInfoMap lr_vrf_info_map_;

    /// @brief The map between pointer to VirtualNetwork (a
    /// bridge or routing virtual network connected to some LR) and
    /// the LR's UUID.
    VnLrSet vn_lr_set_;

    /// @brief The set of walkers for Inet IPv4 tables of bridge VRF instances.
    InetTableWalker inet4_table_walker_;

    /// @brief The set of walkers for Inet IPv6 tables of bridge VRF instances.
    InetTableWalker inet6_table_walker_;

    DISALLOW_COPY_AND_ASSIGN(VxlanRoutingVrfMapper);
};

/// @brief This class manages an operative state of VxLAN logical routers (LR)
/// defined via Config logical-router element. The operative state include
/// bonds between LRs, virtual networks, VRFs and corresponding routes which
/// support connectivity between networks connected to a LR.
///
/// Given a logical router (LR), there is precisely 1 routing virtual network
/// (VN) and a routing VRF table associated with it and zero or more
/// bridge VNs (and corresponding VRF tables) whose states are manipulated
/// by VxlanRoutingManager using routes leaking process.
/// Routes leaking is a routing tables bi-directional manipulation process
/// during which:
/// - new VM interface routes arising in the bridge VRF tables are being
/// transferred into the routing VRF table associated with the LR (forward
/// propagation of routes);
/// - new routes arising in the routing VRF table are redistributed among
/// the bridge VRF tables associated with the LR (backward propagation of
/// routes);
/// - deletion of created routes in the routing or a bridge VRF leads to
/// the deletion of the corresponding created routes in other VRF tables
/// associated with the LR.
///
/// In a nutshell, routes leaking is a kind of data synchronization process
/// between several routing tables.
///
/// A bridge virtual network is a basic element in OpenSDN that manages
/// network connectivity between several VMs within a broadcast domain.
/// A routing virtual network is a technical virtual network associated
/// with a LR and providing connectivity between several bridge virtual
/// networks attached to (associated with) the LR.
///
/// The information about an LR is loaded into (or unloaded from) the Agent’s
/// operative DB when the processing of a corresponding IF-MAP node changes
/// triggers notifications for associated tables: the interfaces tables
/// (InterfaceTable class), the virtual networks table (VnTable class) and
/// the VRFs tables (VrfTable class).
/// The changes to the IF-MAP nodes arise due to such events as a
/// connection of a virtual network with an LR (or a disconnection of a VN
/// from an LR), appearance of a first VM connected to an LR via a bridge
/// network or a destruction of a last VM (connected to an LR) on a hypervisor.
/// VxlanRoutingManager links these notifications with its member functions:
/// VxlanRoutingManager::VmiNotify, VxlanRoutingManager::VnNotify
/// and VxlanRoutingManager::VrfNotify and they are called VmiNotify,
/// VnNotify and VrfNotify for brevity.
///
/// Due to concurrency mechanisms, these functions can run in various order.
/// However, only specific order guarantees lack of inconsistencies in the
/// VxlanRoutingManager data describing state of an LR:
/// 1. if an LR is being created or updated (e.g., a bridge network is being
/// attached to it), then the correct notifications processing order is
/// VmiNotify, VnNotify and VrfNotify, in this case routes leaking functions
/// which are called by RouteNotify member of VxlanRoutingManager will not
/// start execution until all information from IF-MAP is not correctly
/// transferred into the operative DB;
/// 2. if an LR is being destroyed (or a bridge network is being detached
/// from it), then the situation is not so obvious, because (a)
/// on the one hand, VrfNotify should be called first to prevent unexpected
/// routes leaking from the disconnected or destroyed bridge virtual network,
/// however this breaks normal event-based mechanism of routes update in
/// the LR's bridge networks due to changes in the LR's routing network
/// (since RouteNotify handler will be disconnected) therefore, (b)
/// the only second viable option is when VnNotify is called first.
///
/// The preferred order of execution for functions VrfNotify, VmiNotoify
/// and VnNotify cannot be specified explicitly from VxlanRoutingMananger,
/// since it is controlled by (a) arrival of IF-MAP information on a hypervisor
/// node from a controller and (b) by policies of Agent's operative DB.
/// For some cases, the correctness of execution is guaranteed by the order
/// of IF-MAP nodes processing, in some cases, it is guaranteed by additonal
/// calls inside VxlanRoutingManager.
///
/// VxlanRoutingManager acts as a kernel module in some sense: it has several
/// member functions acting as an entry point and providing service to
/// its caller: an information about the current state of the module,
/// modification of the internal state, modifiction of external state (routes
/// advertisement). These entry points are:
/// - VxlanRoutingManager::VxlanRoutingManager, initializes the module,
/// modifies only the internal state;
/// - VxlanRoutingManager::Register: sets/resets handlers for change events in
/// VmiTable, VrfTable and VnTable, modifies only the internal state;
/// - VxlanRoutingManager::VmiNotify: links a VN with a VMI connecting the
/// latter with an LR, modifies the external state (VnEntry and VmInterface
/// states declared in VxlanRoutingManager) and is called whenever a VMI is
/// being attached to an LR (or detached from an LR);
/// - VxlanRoutingManager::VnNotify: establishes (updates) a mapping between
/// an LR and VNs attached to the LR, initiates traversing of the routing
/// and bridge VRF tables to copy (leak) routes between them, modifies the
/// internal and external states and is called whenever a VN is modified;
/// - VxlanRoutingManager::VrfNotify: links VxlanRoutingManager member
/// functions with changes in a modified bridge VN and routing VN routing
/// tables, modifies only the internal state;
/// - VxlanRoutingManager::RouteNotify: propagates locally-originated routes
/// (interface or interface composites) from bridge VRF Inet tables into
///  the routing VRF Inet table associated previously with an LR;
/// - VxlanRoutingManager::XmppAdvertiseEvpnRoute: advertises an external
/// EVPN Type 5 route that came from controller via BGP/XMPP;
/// - VxlanRoutingManager::IsVxlanAvailable: returns true if VxLAN is
/// available;
/// - VxlanRoutingManager::IsRoutingVrf: returns true if the given VRF table
/// object is a VxLAN routing VRF table.
///
/// The internal state of VxlanRoutingManager is defined by a map between
/// an LR UUID and virtual networks / VRF tables, associated with this LR.
/// The map is defined in VxlanRoutingVrfMapper class and stored as
/// std::map for a pair of {boost::uuids::uuid,
/// VxlanRoutingVrfMapper::RoutedVrfInfo}, where
/// VxlanRoutingVrfMapper::RoutedVrfInfo stores:
/// - a pointer to a VnEntry object containing description of the routing VN
/// associated with the LR;
/// - a list of pointers to VnEntry objects containing description of
/// bridge VNs associated with the LR;
/// - an array of names of VrfEntry objects containing description of
/// bridge and routing VRF tables linked with the corresponding VNs.
/// In addition, VxlanRoutingVrfMapper contains a reverse map, that
/// determines an LR UUID by a pointer to an VnEntry object, associated
/// with an LR as a bridge or a routing VN.
///
/// The external state, which is modified by VxlanRoutingManager, is
/// characterized by:
/// - VRF tables (inet and EVPN) of bridge and routing virtual networks;
/// - operative DB entries states (VxlanRoutingVnState, VxlanRoutingVmiState,
/// VxlanRoutingState) which are associated with a VN, a VRF table or a VMI
/// to store additional information needed by VxlanRoutingManager.
///
/// Since the aforementioned VxlanRoutingManager entry points are executed
/// in tasks with the same task code and task data ID (see Task class), their
/// execution is mutually exclusive, i.e., they cannot run in parallel, but
/// the order of their execution is not predetermined.
class VxlanRoutingManager {
public:

    /// @brief Constructs instance of the class and links to the Agent class
    /// instance. Since only one agent class instance works per system process,
    /// this implies that only one instance of VxlanRoutingManager exists.
    VxlanRoutingManager(Agent *agent);

    /// @brief Destroys the VxlanRoutingManager instance.
    virtual ~VxlanRoutingManager();

    /// @brief Registers handlers for events associated with changes in
    /// virtual networks (VnTable class) and VRF instances (VrfTable class).
    void Register();

    /// @brief Unregisters handlers for events associated with changes in
    /// virtual networks (VnTable class) and VRF instances (VrfTable class).
    void Shutdown();

    /// @brief A handler for changes (new/update/delete) in a virtual network
    /// (VnEntry class).
    void VnNotify(DBTablePartBase *partition, DBEntryBase *e);

    /// @brief A handler for changes (new/update/delete) in the virtual network
    /// from a bridge VRF.
    void BridgeVnNotify(const VnEntry *vn, VxlanRoutingVnState *vn_state);

    /// @brief A handler for changes (new/update/delete) in the virtual network
    /// from a routing VRF.
    void RoutingVnNotify(const VnEntry *vn, VxlanRoutingVnState *vn_state);

    /// @brief A handler for changes (new/update/delete) in a VRF instance
    /// (VrfEntry class).
    void VrfNotify(DBTablePartBase *partition, DBEntryBase *e);

    /// @brief Handler for changes (new/update/delete) in
    /// a VMI (VmInterface class).
    void VmiNotify(DBTablePartBase *partition, DBEntryBase *e);

    /// @brief Handler for changes (new/update/delete) in a route
    /// (EVPN or Inet). Main entry point for routes leaking.
    bool RouteNotify(DBTablePartBase *partition, DBEntryBase *e);

    /// Routes leaking functions

private:

    /// @brief Performs routes leaking between the Inet table of a bridge VRF
    /// instance and the EVPN table of the routing VRF instance.
    bool InetRouteNotify(DBTablePartBase *partition, DBEntryBase *e);

    /// @brief Performs routes leaking between the EVPN table of the routing
    /// VRF instance and the Inet table of the routing VRF instance.
    bool EvpnRouteNotify(DBTablePartBase *partition, DBEntryBase *e);

    /// @brief Removes redundant VrfNH path from a given route. These routes
    /// might arise with small chance in a bridge VRF inet tables when
    /// tunnels in the routing VRF instance arrive later then in the bridge VRF
    /// instance.
    void ClearRedundantVrfPath(DBEntryBase *e);

    /// @brief Handles deletion of a route in the EVPN table of the routing
    /// VRF instance.
    void WhenBridgeInetIntfWasDeleted(const InetUnicastRouteEntry *inet_rt,
        const VrfEntry *routing_vrf);

    /// @brief Handles deletion of a route in the Inet table of the routing
    /// VRF instance.
    void WhenRoutingEvpnRouteWasDeleted(const EvpnRouteEntry *routing_evpn_rt,
        const Peer* delete_from_peer);

public:

    /// @brief Deletes a given EVPN route from EVPN table of the routing
    /// VRF instance
    bool WithdrawEvpnRouteFromRoutingVrf(const VrfEntry *routing_vrf,
                                         DBTablePartBase *partition,
                                         DBEntryBase *e);

    /// @brief Performs advertisement and deletion of routing routes
    /// (with VrfNH) in bridge VRF instances. External tunnels and routes
    /// with a prefix that is not present in bridge VRF instance are
    /// selected for leaking
    bool LeakRoutesIntoBridgeTables(DBTablePartBase *partition,
                                    DBEntryBase *e,
                                    const boost::uuids::uuid &uuid,
                                    const VnEntry *vn,
                                    bool update = false);

    /// @brief Handles routing routes (with VrfNH) update in the routing VRF
    /// instance.
    void HandleSubnetRoute(const VrfEntry *vrf, bool bridge_vrf = false);

private:

    /// @brief deletes all routes in EVPN table of routing VRF
    void RoutingVrfDeleteAllRoutes(VrfEntry* rt_vrf);

    /// @brief Deletes subnet routes (actually, paths with VrfNH) in
    /// the given bridge VRF. This function is demanded at vn.c:618
    void DeleteSubnetRoute(const VrfEntry *vrf);
    // void DeleteSubnetRoute(const VrfEntry *vrf, VnIpam *ipam = NULL);

    /// @brief Deletes subnet routes from a specified virtual network
    /// (VirtualNetwork)
    void DeleteSubnetRoute(const VnEntry *vn,
        const std::string& vrf_name);

    /// @brief Delete routes to IPAM, specified by IP prefix and prefix
    /// length
    void DeleteIpamRoutes(const VnEntry *vn,
                          const std::string& vrf_name,
                          const IpAddress& ipam_prefix,
                          const uint32_t plen);

    /// @brief Updates subnet routes (actually, paths with VrfNH) in
    /// the given bridge VRF
    void UpdateSubnetRoute(const VrfEntry *vrf,
                           const VrfEntry *routing_vrf);
public:

    /// @brief Updates Sandesh response
    void FillSandeshInfo(VxlanRoutingResp *resp);

    /// @brief Returns the ID of the listener to changes in the VnTable
    DBTable::ListenerId vn_listener_id() const {
        return vn_listener_id_;
    }

    /// @brief Returns the ID of the listener to changes in the VrfTable
    DBTable::ListenerId vrf_listener_id() const {
        return vrf_listener_id_;
    }

    /// @brief Returns the ID of the listener to changes in the InterfaceTable
    DBTable::ListenerId vmi_listener_id() const {
        return vmi_listener_id_;
    }

    /// @brief Returns the map between LR uuids and associated bridge and
    /// routing VRF instances
    const VxlanRoutingVrfMapper &vrf_mapper() const {
        return vrf_mapper_;
    }

    /// @brief Returns a pointer to the AgentRouteWalkerPtr object
    AgentRouteWalker* walker() {
        return walker_.get();
    }

private:

    /// Internal data of this class

    /// @brief A pointer to the Peer where all interface / composite of
    /// interfaces routes in routing VRF are linked to.
    static const Peer *routing_vrf_interface_peer_;

    /// @brief A pointer to the Peer where all BGP routes are stored
    static const Peer *routing_vrf_vxlan_bgp_peer_;

    /// @brief A pointer to the Agent instance.
    Agent *agent_;

    /// @brief A pointer to the walker to loop over INET tables
    /// in bridge VRF instances.
    AgentRouteWalkerPtr walker_;

    /// @brief An ID of the listener to changes in VnTable.
    DBTable::ListenerId vn_listener_id_;

    /// @brief An ID of the listener to changes in VrfTable.
    DBTable::ListenerId vrf_listener_id_;

    /// @brief An ID of the listener to changes in InterfaceTable.
    DBTable::ListenerId vmi_listener_id_;

    /// @brief A map between LR uuids and associated bridge and
    /// routing VRF instances.
    VxlanRoutingVrfMapper vrf_mapper_;

    /// @brief An always increasing counter for new paths (used to signal
    /// controoler about new routes).
    static uint32_t loc_sequence_;

    /// Friends declarations

    /// @brief Allows access to private members for the VxlanRoutingRouteWalker
    /// class.
    friend class VxlanRoutingRouteWalker;

    /// @brief Allows ControllerEcmpRoute to use private members of this class.
    friend class ControllerEcmpRoute;

    /// @brief Allows AgentXmppChannel to use private members of this class.
    friend class AgentXmppChannel;

    /// @brief Allows access to Xmpp advertisement functions via external class
    friend class AgentXmppChannelVxlanInterface;

    /// @brief Allows VxlanRoutingVrfMapper to use private members of
    /// this class.
    friend class VxlanRoutingVrfMapper;

    /// @brief Allows MetadataProxy to use private members of this class.
    friend class MetadataProxy;

    /// Auxilliary functions

    /// @brief Returns new value of a local sequence. Thread safe version
    static uint32_t GetNewLocalSequence(const AgentPath*);

    /// @brief Determines whether the address string contains an IPv4 address
    ///  as substring or not.
    static bool is_ipv4_string(const std::string& prefix_str);

    /// @brief Determines whether the address string contains an IPv6 address
    ///  as substring or not.
    static bool is_ipv6_string(const std::string& prefix_str);

    /// @brief Extracts length of IPv4 subnet address from the prefix string.
    static uint32_t ipv4_prefix_len(const std::string& prefix_str);

    /// @brief Extracts an IPv4 address string from the prefix string.
    static std::string ipv4_prefix(const std::string& prefix_str);

    /// @brief Extracts length of IPv6 subnet address from the prefix string.
    static uint32_t ipv6_prefix_len(const std::string& prefix_str);

    /// @brief Extracts an IPv6 address string from the prefix string.
    static std::string ipv6_prefix(const std::string& prefix_str);

    /// @brief Checks whether VxLAN routing manager is enabled or not.
    static bool IsVxlanAvailable(const Agent* agent);

    /// @brief Finds first occurence of a route with the given prefix (IP
    /// address and length) in Inet tables of bridge VRF instances connected
    /// to the given routing VRF instance (LR).
    std::string GetOriginVn(const VrfEntry* routing_vrf,
        const IpAddress& ip_addr,
        const uint8_t& plen);

    /// @brief Determines whether route prefix in the EVPN route is equal
    /// to the given pair of prefix IP address and length
    static bool RoutePrefixIsEqualTo(const EvpnRouteEntry* route,
        const IpAddress& prefix_ip,
        const uint32_t prefix_len);

    /// @brief Determines whether route prefix of the Inet route is equal
    /// to the given pair of prefix IP address and length
    static bool RoutePrefixIsEqualTo(const InetUnicastRouteEntry* route,
        const IpAddress& prefix_ip,
        const uint32_t prefix_len);

    /// @brief Determines whether the prefix address and the prefix length
    /// point to a host route (/32 for IPv4, /128 for IPv6) or
    /// to a subnet route.
    static bool IsHostRoute(const IpAddress& prefix_ip, uint32_t prefix_len);

    /// @brief Determines whether the given EVPN route points to a host or
    /// a subnet.
    static bool IsHostRoute(const EvpnRouteEntry *rt);

    /// @brief Determines whether the given EVPN route is a host one and
    /// belongs to a subnet of a local bridge VRF. During the search all
    /// subnets in all bridge VRF instances connected to the LR are traversed.
    bool IsHostRouteFromLocalSubnet(const EvpnRouteEntry *rt);

    /// @brief Determines if the given EVPN route has an interface NH or
    /// a composite of interfaces NH that belongs to the given
    /// bridge VRF instance.
    bool IsVrfLocalRoute(EvpnRouteEntry *routing_evpn_rt,
        VrfEntry *bridge_vrf);

    /// @brief Determines if the given EVPN route is already present
    /// in the given VRF
    bool IsLocalRoute(EvpnRouteEntry *routing_evpn_rt,
        VrfEntry *bridge_vrf);

    /// @brief Determines whether the given route has the path with
    /// a VRF nexthop (VrfNH)
    static bool HasVrfNexthop(const AgentRoute* rt);

    /// @brief Determines whether the given EVPN route has at least one path
    /// originating from BGP/XMPP (has Peer type BGP_PATH)
    bool HasBgpPeerPath(EvpnRouteEntry *evpn_rt);

    /// @brief Determines whether the pointer to the VRF instance is of routing
    ///  type.
    /// @return true if it is routing, otherwise return value is false.
    static bool IsRoutingVrf(const VrfEntry* vrf);

    /// @brief Determines whether the pointer to the VRF instance is of
    /// bridge type.
    /// @return true if it is routing, otherwise return value is false.
    static bool IsBridgeVrf(const VrfEntry* vrf);

    /// @brief Checks whether the VRF instance with the given name is routing
    /// or not.
    /// @return true if this VRF is routing, otherwise return value is false.
    static bool IsRoutingVrf(const std::string vrf_name, const Agent *agent);

    /// @brief Finds in the given route the path with a specified Peer type
    static const AgentPath* FindPathWithGivenPeer(
        const AgentRoute *inet_rt,
        const Peer::Type peer_type);

    /// @brief Finds in the given route the path with a specified Peer type
    /// and a specified nexthop type
    static const AgentPath* FindPathWithGivenPeerAndNexthop(
        const AgentRoute *inet_rt,
        const Peer::Type peer_type,
        const NextHop::Type nh_type,
        bool strict_match = true);

    /// @brief Finds in the given route the path with the given Peer type
    /// and interface nexthop (InterfaceNH).
    static const AgentPath* FindInterfacePathWithGivenPeer(
        const AgentRoute *inet_rt,
        const Peer::Type peer_type,
        bool strict_match = true);

    /// @brief Finds in the given route the path which has
    /// the BGP_PEER Peer type and the Interface nexthop type.
    /// Such path presumably points to BGPaaS advertised route.
    static const AgentPath *FindInterfacePathWithBgpPeer(
        const AgentRoute *inet_rt,
        bool strict_match = true);

    /// @brief Finds in the given route the path which has the LOCAL_VM_PEER
    /// peer type and the Interface nexthop type.
    static const AgentPath *FindInterfacePathWithLocalVmPeer(
        const AgentRoute *inet_rt,
        bool strict_match = true);

    /// @brief Returns the MAC address for the IP of a given
    /// neighbouring compute
    static MacAddress NbComputeMac(const Ip4Address& compute_ip,
        const Agent *agent);

    /// XMPP Advertising functions

    /// @brief Allocates and returns a new key for the VxLAN tunnel to
    /// the given router
    TunnelNHKey* AllocateTunnelNextHopKey(const IpAddress& dip,
        const MacAddress& dmac) const;

    /// @brief Advertises an EVPN route received using XMPP channel
    void XmppAdvertiseEvpnRoute(const IpAddress& prefix_ip,
        const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
        const RouteParameters& params, const Peer *bgp_peer,
        const std::vector<std::string> &peer_sources);

    /// @brief Advertises an Inet route received using XMPP channel
    void XmppAdvertiseInetRoute(const IpAddress& prefix_ip,
        const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
        const RouteParameters& params, const Peer *bgp_peer);

    /// @brief Advertises an Inet route received from EVPN table
    void XmppAdvertiseInetRoute(const IpAddress& prefix_ip,
        const int prefix_len, const std::string vrf_name,
        const AgentPath*);

    /// @brief Advertises in the EVPN table a tunnel route that arrived
    /// via XMPP channel. Must be called only from XmppAdvertiseInetRoute.
    void XmppAdvertiseEvpnTunnel(
        EvpnAgentRouteTable *inet_table, const IpAddress& prefix_ip,
        const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
        const RouteParameters& params, const Peer *bgp_peer);

    /// @brief Advertises in the EVPN table an interface route that arrived
    /// via XMPP channel. Must be called only from XmppAdvertiseInetRoute.
    void XmppAdvertiseEvpnInterface(
        EvpnAgentRouteTable *inet_table, const IpAddress& prefix_ip,
        const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
        const RouteParameters& params, const Peer *bgp_peer,
        const std::vector<std::string> &peer_sources);

    /// @brief Advertises in the Inet table a tunnel route that arrived
    /// via XMPP channel. Must be called only from XmppAdvertiseInetRoute.
    void XmppAdvertiseInetTunnel(
        InetUnicastAgentRouteTable *inet_table, const IpAddress& prefix_ip,
        const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
        const RouteParameters& params, const Peer *bgp_peer);

    /// @brief Advertises in the Inet table a tunnel route that arrived
    /// via XMPP channel. Must be called only from XmppAdvertiseInetRoute.
    void XmppAdvertiseInetTunnel(
        InetUnicastAgentRouteTable *inet_table, const IpAddress& prefix_ip,
        const int prefix_len, const std::string vrf_name,
        const AgentPath* path);

    /// @brief Advertises in the Inet table an interface route that arrived
    /// via XMPP channel. Must be called only from XmppAdvertiseInetRoute.
    void XmppAdvertiseInetInterfaceOrComposite(
        InetUnicastAgentRouteTable *inet_table, const IpAddress& prefix_ip,
        const int prefix_len, const std::string vrf_name,
        const AgentPath* path);

    /// Templates

    /// @brief Converts item's (EnetItemType for EVPN / ItemType for Inet)
    /// nexthops into the list of IP addresses (IpAddress)
    template <class ItType>
    static std::vector<IpAddress> ItemNexthopsToVector(ItType *item);

    /// @brief Adds an interface or a composite of interfaces nexthops to
    /// the list of components NH keys needed for construction of the
    /// a mixed composite.
    template<typename NhType>
    static void AddInterfaceComponentToList(
        const std::string& prefix_str,
        const std::string& vrf_name,
        const NhType &nh_item,
        ComponentNHKeyList& comp_nh_list,
        std::vector<std::string> &peer_sources);

    template<typename NhType>
    static void AddBgpaasInterfaceComponentToList(
        const std::string& vrf_name,
        const NhType &nh_item,
        ComponentNHKeyList& comp_nh_list,
        std::vector<std::string> &peer_sources);

    /// @brief Finds a route with the given prefix address and len
    /// in the EVPN table
    static AgentRoute *FindEvpnOrInetRoute(const Agent *agent,
        const std::string &vrf_name,
        const IpAddress &ip_addr,
        uint32_t prefix_len,
        const autogen::EnetNextHopType &nh_item);

    /// @brief Finds a route with the given prefix address and len
    /// in the Inet table
    static AgentRoute *FindEvpnOrInetRoute(const Agent *agent,
        const std::string &vrf_name,
        const IpAddress &ip_addr,
        uint32_t prefix_len,
        const autogen::NextHopType &nh_item);

    /// Routes copying functions

    /// @brief Deletes interface path specified with IP prefix, prefix
    /// length and Peer from the EVPN table.
    static void DeleteOldInterfacePath(const IpAddress &prefix_ip,
        const uint32_t plen,
        const Peer *peer,
        EvpnAgentRouteTable *evpn_table);

    /// @brief Copies the path to the prefix address into the EVPN table
    /// with the given Peer. The function is used during routes leaking
    /// between bridge VRF Inet and routing EVPN tables.
    static void CopyInterfacePathToEvpnTable(const AgentPath* path,
        const IpAddress &prefix_ip,
        const uint32_t plen,
        const Peer *peer,
        const RouteParameters &params,
        EvpnAgentRouteTable *evpn_table);

    /// @brief
    static void DeleteOldInterfacePath(const IpAddress &prefix_ip,
        const uint32_t plen,
        const Peer *peer,
        InetUnicastAgentRouteTable *inet_table);

    /// @brief Copies the path to the prefix address into the EVPN table
    /// with the given Peer. The function is used during routes leaking
    /// between routing VRF EVPN and Inet tables.
    void CopyPathToInetTable(const AgentPath* path,
        const IpAddress &prefix_ip,
        const uint32_t plen,
        const Peer *peer,
        const RouteParameters &params,
        InetUnicastAgentRouteTable *inet_table);

    /// @brief Advertises in an EVPN routing table a BGPaaS route that
    /// came from the controller (this routes leaking occurs in the
    /// controller).
    void XmppAdvertiseEvpnBgpaas(
    EvpnAgentRouteTable *evpn_table, const IpAddress& prefix_ip,
    const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
    const RouteParameters& params, const Peer *bgp_peer,
    const std::vector<std::string> &peer_sources);

    /// @brief Advertises in an EVPN routing table a BGPaaS route
    /// with interface path that
    /// came from the controller (this routes leaking occurs in the
    /// controller).
    void XmppAdvertiseEvpnBgpaasInterface(
    EvpnAgentRouteTable *evpn_table, const IpAddress& prefix_ip,
    const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
    const RouteParameters& params, const Peer *bgp_peer,  const NextHop *nh);

    /// @brief Advertises in an EVPN routing table a BGPaaS route
    /// with interface composite path that
    /// came from the controller (this routes leaking occurs in the
    /// controller).
    void XmppAdvertiseEvpnBgpaasComposite(
    EvpnAgentRouteTable *evpn_table, const IpAddress& prefix_ip,
    const int prefix_len, uint32_t vxlan_id, const std::string vrf_name,
    const RouteParameters& params, const Peer *bgp_peer,
    ComponentNHKeyList &comp_nh_list);

public:

    /// @brief Prints EVPN table of the given VRF instance.
    static void PrintEvpnTable(const VrfEntry* const_vrf);

    /// @brief Prints IPv4 Inet table of the given VRF instance.
    static void PrintInetTable(const VrfEntry* const_vrf);

    /// @brief Prints all virtual networks attached to logical routers.
    static void ListAttachedVns();

    /// @brief Finds a VRF table (VrfEntry) for the given virtual network (VN).
    /// Returns nullptr if no VRF table associated with this VN or there is no
    /// VrfEntry having the given name.
    static inline VrfEntry* VnVrf(const VnEntry *vn, const std::string &vrf_name) {
        VrfEntry* vrf = nullptr;
        vrf = vn->GetVrf();
        if (vrf != nullptr) {
            return vrf;
        }
        vrf = Agent::GetInstance()->vrf_table()->FindVrfFromName(vrf_name);
        return vrf;
    }

    DISALLOW_COPY_AND_ASSIGN(VxlanRoutingManager);
};

#include <oper/vxlan_templates.cc>

#endif
