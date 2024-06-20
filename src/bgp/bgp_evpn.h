/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_BGP_EVPN_H_
#define SRC_BGP_BGP_EVPN_H_

#include <boost/ptr_container/ptr_map.hpp>
#include <boost/scoped_ptr.hpp>
#include <tbb/spin_rw_mutex.h>

#include <list>
#include <set>
#include <vector>

#include "base/lifetime.h"
#include "base/task_trigger.h"
#include "base/address.h"
#include "bgp/bgp_attr.h"
#include "bgp/bgp_path.h"
#include "db/db_entry.h"

class BgpPath;
class DBTablePartition;
class DBTablePartBase;
class EvpnRoute;
class ErmVpnRoute;
class EvpnState;
class EvpnTable;
class ErmVpnTable;
class EvpnManagerPartition;
class EvpnManager;
class ShowEvpnTable;
struct UpdateInfo;

typedef boost::intrusive_ptr<EvpnState> EvpnStatePtr;

/// @brief This is the base class for a multicast node in an EVPN instance. 
/// The node could either represent a local vRouter that's connected to a 
/// control node via XMPP or a remote vRouter/PE that's discovered via BGP.
///
/// In normal operation, the EvpnMcastNodes corresponding to vRouters (local
/// or remote) support edge replication, while those corresponding to EVPN PEs
/// do not.  However, for test purposes, it's possible to have vRouters that
/// no not support edge replication.
class EvpnMcastNode : public DBState {
public:
    enum Type {
        LocalNode,
        RemoteNode
    };
    /// @brief Constructor for EvpnMcastNode. The type indicates whether this is a local
    /// or remote EvpnMcastNode.
    EvpnMcastNode(EvpnManagerPartition *partition, EvpnRoute *route,
        uint8_t type);
    EvpnMcastNode(EvpnManagerPartition *partition, EvpnRoute *route,
        uint8_t type, EvpnStatePtr state);
    ~EvpnMcastNode();

    /// @brief Update the label and attributes for a EvpnMcastNode.
    /// Return true if either of them changed.
    bool UpdateAttributes(EvpnRoute *route);

    /// @brief Handle update of EvpnLocalMcastNode.
    ///
    /// We delete and add the Inclusive Multicast route to ensure that all the
    /// attributes are updated. An in-place update is not always possible since
    /// the vRouter address is part of the key for the Inclusive Multicast route.
    virtual void TriggerUpdate() = 0;

    EvpnStatePtr state() { return state_; };
    void set_state(EvpnStatePtr state) { state_ = state; };
    EvpnRoute *route() { return route_; }
    uint8_t type() const { return type_; }
    const BgpAttr *attr() const { return attr_.get(); }
    uint32_t label() const { return label_; }
    Ip4Address address() const { return address_; }
    Ip4Address replicator_address() const { return replicator_address_; }
    bool edge_replication_not_supported() const {
        return edge_replication_not_supported_;
    }
    bool assisted_replication_supported() const {
        return assisted_replication_supported_;
    }
    bool assisted_replication_leaf() const {
        return assisted_replication_leaf_;
    }

protected:
    EvpnManagerPartition *partition_;
    EvpnStatePtr state_;
    EvpnRoute *route_;
    uint8_t type_;
    BgpAttrPtr attr_;
    uint32_t label_;
    Ip4Address address_;
    Ip4Address replicator_address_;
    bool edge_replication_not_supported_;
    bool assisted_replication_supported_;
    bool assisted_replication_leaf_;

private:
    DISALLOW_COPY_AND_ASSIGN(EvpnMcastNode);
};


/// @brief This class represents (in the context of an EVPN instance) a local 
/// vRouter that's connected to a control node via XMPP.  An EvpnLocalMcastNode
/// gets created and associated as DBState with the broadcast MAC route advertised
/// by the vRouter.
///
/// The EvpnLocalMcastNode mainly exists to translate the broadcast MAC route
/// advertised by the vRouter into an EVPN Inclusive Multicast route.  In the
/// other direction, EvpnLocalMcastNode serves as the anchor point to build a
/// vRouter specific ingress replication olist so that the vRouter can send
/// multicast traffic to EVPN PEs (and possibly vRouters in test environment)
/// that do not support edge replication.
///
/// An Inclusive Multicast route is added for each EvpnLocalMcastNode. The
/// attributes of the Inclusive Multicast route are based on the broadcast
/// MAC route corresponding to the EvpnLocalMcastNode.  The label for the
/// broadcast MAC route is advertised as the label for ingress replication
/// in the PmsiTunnel attribute.
class EvpnLocalMcastNode : public EvpnMcastNode {
public:

    /// @brief Constructor for EvpnLocalMcastNode.
    ///
    /// Add an Inclusive Multicast route corresponding to Broadcast MAC route.
    ///
    /// Need to Notify the Broadcast MAC route so that the table Export method
    /// can run and build the OList. OList is not built till EvpnLocalMcastNode
    /// has been created.
    EvpnLocalMcastNode(EvpnManagerPartition *partition, EvpnRoute *route);
    EvpnLocalMcastNode(EvpnManagerPartition *partition, EvpnRoute *route,
                       EvpnStatePtr state);
    virtual ~EvpnLocalMcastNode();

    /// @brief Handle update of EvpnLocalMcastNode.
    ///
    /// We delete and add the Inclusive Multicast route to ensure that all the
    /// attributes are updated. An in-place update is not always possible since
    /// the vRouter address is part of the key for the Inclusive Multicast route.
    virtual void TriggerUpdate();

    UpdateInfo *GetUpdateInfo(EvpnRoute *route);
    EvpnRoute *inclusive_mcast_route() { return inclusive_mcast_route_; }

private:
    /// @brief Add Inclusive Multicast route for this EvpnLocalMcastNode.
    /// The attributes are based on the Broadcast MAC route.
    void AddInclusiveMulticastRoute();

    /// @brief Delete Inclusive Multicast route for this EvpnLocalMcastNode.
    void DeleteInclusiveMulticastRoute();

    EvpnRoute *inclusive_mcast_route_;

    DISALLOW_COPY_AND_ASSIGN(EvpnLocalMcastNode);
};


/// @brief This class represents (in the context of an EVPN instance) a remote
/// vRouter or PE discovered via BGP.  An EvpnRemoteMcastNode is created and
/// associated as DBState with the Inclusive Multicast route in question.
///
/// An EvpnRemoteMcastNode also gets created for the Inclusive Multicast route
/// that's added for each EvpnLocalMcastNode. This is required only to support
/// test mode where vRouter acts like PE that doesn't support edge replication.
class EvpnRemoteMcastNode : public EvpnMcastNode {
public:
    EvpnRemoteMcastNode(EvpnManagerPartition *partition, EvpnRoute *route);
    EvpnRemoteMcastNode(EvpnManagerPartition *partition, EvpnRoute *route,
                        EvpnStatePtr state);
    virtual ~EvpnRemoteMcastNode();

    /// @brief Handle update of EvpnRemoteMcastNode.
    virtual void TriggerUpdate();

private:
    DISALLOW_COPY_AND_ASSIGN(EvpnRemoteMcastNode);
};


/// @brief This class represents a remote EVPN segment that has 2 or more PEs 
/// that are multi-homed to it. An EvpnSegment is created when we see a MAC 
/// route with a non-NULL ESI or when we see an AD route for the ESI in question.
///
/// An EvpnSegment contains a vector of lists of MAC routes that are dependent
/// on it. There's a list entry in the vector for each DB partition.  All the
/// MAC routes in a given partition that are associated with the EvpnSegment are
/// in inserted in the list for that partition. The lists are updated as and
/// when the EvpnSegment for MAC routes is updated.
///
/// An EvpnSegment contains a list of Remote PEs that have advertised per-ESI
/// AD routes for the EVPN segment in question. The list is updated when paths
/// are added/deleted from the AD route. A change in the contents of the list
/// triggers an update of all dependent MAC routes, so that their aliased paths
/// can be updated. The single-active state of the EvpnSegment is also updated
/// when the PE list is updated. The PE list is updated from the context of the
/// bgp::EvpnSegment task.
class EvpnSegment : public DBState {
public:
    EvpnSegment(EvpnManager *evpn_manager, const EthernetSegmentId &esi);
    ~EvpnSegment();

    class RemotePe {
    public:
        RemotePe(const BgpPath *path);

        /// @brief Equality operator for EvpnSegment::RemotePe.
        /// Do not compare esi_valid and single_active fields since they are
        /// derived.
        bool operator==(const RemotePe &rhs) const;

        bool esi_valid;
        bool single_active;
        const IPeer *peer;
        BgpAttrPtr attr;
        uint32_t flags;
        BgpPath::PathSource src;
    };

    typedef std::list<RemotePe> RemotePeList;
    typedef RemotePeList::const_iterator const_iterator;

    const_iterator begin() const { return pe_list_.begin(); }
    const_iterator end() const { return pe_list_.end(); }

    /// @brief Add the given MAC route as a dependent of this EvpnSegment.
    void AddMacRoute(size_t part_id, EvpnRoute *route);

    /// @brief Delete the given MAC route as a dependent of this EvpnSegment.
    /// Trigger deletion of the EvpnSegment if there are no dependent
    /// routes in the partition.
    void DeleteMacRoute(size_t part_id, EvpnRoute *route);

    /// @brief Trigger an update of all dependent MAC routes for this EvpnSegment.
    /// Note that the bgp::EvpnSegment task is mutually exclusive with the
    /// db::DBTable task.
    void TriggerMacRouteUpdate();

    /// @brief Update the PE list for this EvpnSegment. This should be called when
    /// the AutoDisocvery route is updated.
    /// Return true if there's a change in the PE list i.e. if an entry is
    /// added, deleted or updated.
    bool UpdatePeList();

    /// @brief Return true if it's safe to delete this EvpnSegment.
    bool MayDelete() const;

    const EthernetSegmentId &esi() const { return esi_; }
    EvpnRoute *esi_ad_route() { return esi_ad_route_; }
    void set_esi_ad_route(EvpnRoute *esi_ad_route) {
        esi_ad_route_ = esi_ad_route;
    }
    void clear_esi_ad_route() { esi_ad_route_ = NULL; }
    bool single_active() const { return single_active_; }

private:
    typedef std::set<EvpnRoute *> RouteList;
    typedef std::vector<RouteList> RouteListVector;

    EvpnManager *evpn_manager_;
    EthernetSegmentId esi_;
    EvpnRoute *esi_ad_route_;
    bool single_active_;
    RouteListVector route_lists_;
    RemotePeList pe_list_;

    DISALLOW_COPY_AND_ASSIGN(EvpnSegment);
};


/// @brief This class represents the EvpnManager state associated with a MAC route.
///
/// In the steady state, a EvpnMacState should exist only for MAC routes that
/// have a non-zero ESI. The segment_ field is a pointer to the EvpnSegment for
/// the ESI in question. A EvpnMacState is created from the route listener when
/// we see a MAC route with a non-zero ESI. It is deleted after processing the
/// MAC route if the route has been deleted or if it has a zero ESI.
//
/// The AliasedPathList keeps track of the aliased BgpPaths that we've added.
/// An aliased BgpPath is added for each remote PE for all-active EvpnSegment.
/// The contents of the AliasedPathList are updated when the ESI for the MAC
/// route changes or when the list of remote PEs for the EvpnSegment changes.
class EvpnMacState : public DBState {
public:
    EvpnMacState(EvpnManager *evpn_manager, EvpnRoute *route);
    ~EvpnMacState();

    /// @brief Update aliased BgpPaths for the EvpnRoute based on the remote PEs
    /// for the EvpnSegment.
    /// Return true if the list of aliased paths is modified.
    bool ProcessMacRouteAliasing();

    EvpnSegment *segment() { return segment_; }
    const EvpnSegment *segment() const { return segment_; }
    void set_segment(EvpnSegment *segment) { segment_ = segment; }
    void clear_segment() { segment_ = NULL; }

private:
    typedef std::set<BgpPath *> AliasedPathList;

    /// @brief Add the BgpPath specified by the iterator to the aliased path list.
    /// Also inserts the BgpPath to the BgpRoute.
    void AddAliasedPath(AliasedPathList::const_iterator it);

    /// @brief Delete the BgpPath specified by the iterator from the aliased path list.
    /// Also deletes the BgpPath from the BgpRoute.
    void DeleteAliasedPath(AliasedPathList::const_iterator it);

    /// @brief Find or create the matching aliased BgpPath.
    BgpPath *LocateAliasedPath(const EvpnSegment::RemotePe *remote_pe,
        uint32_t label);

    EvpnManager *evpn_manager_;
    EvpnRoute *route_;
    EvpnSegment *segment_;
    AliasedPathList aliased_path_list_;

    DISALLOW_COPY_AND_ASSIGN(EvpnMacState);
};

/// @brief This class holds Evpn state for a particular <S,G> at any given time.
///
/// In Evpn state machinery, different types of routes are sent and received at
/// different phases of processing. This class holds all relevant information
/// associated with an <S,G>.
///
/// This is a refcounted class which is referred by DB States of different
/// routes. When the refcount reaches 0, (last referring db state is deleted),
/// this object is deleted from the container map and then destroyed.
///
/// global_ermvpn_tree_rt_
///     This is a reference to GlobalErmVpnRoute associated with the ErmVpnTree
///     used in the data plane for this <S,G>. This route is created/updated
///     when ErmVpn notifies changes to ermvpn routes.
///
/// states_
///     This is the parent map that holds 'this' EvpnState pointer as the value
///     for the associated SG key. When the refcount reaches zero, it indicates
///     that there is no reference to this state from of the DB States associated
///     with any Evpn route. Hence at that time, this state is removed this map
///     states_ and destroyed. This map actually sits inside the associated
///     EvpnProjectManagerParitition object.
class EvpnState {
public:
    typedef std::set<EvpnRoute *> RoutesSet;
    typedef std::map<EvpnRoute *, BgpAttrPtr> RoutesMap;

    /// @brief Simple structure to hold <S,G>. Source as "0.0.0.0" can be used to encode
    /// <*,G> as well.
    struct SG {
        SG(const Ip4Address &source, const Ip4Address &group);
        SG(const IpAddress &source, const IpAddress &group);
        explicit SG(const ErmVpnRoute *route);
        explicit SG(const EvpnRoute *route);
        bool operator<(const SG &other) const;

        IpAddress source;
        IpAddress group;
    };

    typedef std::map<SG, EvpnState *> StatesMap;

    /// @brief A global MVPN state for a given <S.G> within a EvpnProjectManager.
    EvpnState(const SG &sg, StatesMap *states, EvpnManager* manager);

    virtual ~EvpnState();
    const SG &sg() const;
    ErmVpnRoute *global_ermvpn_tree_rt();
    const ErmVpnRoute *global_ermvpn_tree_rt() const;
    void set_global_ermvpn_tree_rt(ErmVpnRoute *global_ermvpn_tree_rt);
    RoutesSet &smet_routes() { return smet_routes_; }
    const RoutesSet &smet_routes() const { return smet_routes_; }
    const StatesMap *states() const { return states_; }
    StatesMap *states() { return states_; }
    EvpnManager *manager() { return manager_; }
    const EvpnManager *manager() const { return manager_; }
    int refcount() const { return refcount_; }

private:
    friend class EvpnMcastNode;
    friend class EvpnManagerPartition;

    /// @brief Increment refcont atomically.
    friend void intrusive_ptr_add_ref(EvpnState *evpn_state);

    /// @brief Decrement refcount of an evpn_state. If the refcount falls to 1, it implies
    /// that there is no more reference to this particular state from any other data
    /// structure. Hence, it can be deleted from the container map and destroyed as
    /// well.
    friend void intrusive_ptr_release(EvpnState *evpn_state);

    const ErmVpnTable *table() const;

    SG sg_;
    ErmVpnRoute *global_ermvpn_tree_rt_;
    RoutesSet smet_routes_;
    StatesMap *states_;
    EvpnManager *manager_;
    tbb::atomic<int> refcount_;

    DISALLOW_COPY_AND_ASSIGN(EvpnState);
};


/// @brief This class represents a partition in the EvpnManager.
///
/// It is used to keep track of local and remote EvpnMcastNodes that belong to
/// the partition. The partition is determined on the ethernet tag in the IM
/// route.
///
/// An EvpnManagerPartition contains a set of MAC routes whose alias paths need
/// to be updated. Entries are added to the list using the TriggerMacRouteUpdate
/// method.
class EvpnManagerPartition {
public:
    typedef EvpnState::SG SG;
    typedef std::map<SG, std::set<EvpnMcastNode *> > EvpnMcastNodeList;

    EvpnManagerPartition(EvpnManager *evpn_manager, size_t part_id);
    ~EvpnManagerPartition();

    /// @brief Get the DBTablePartition for the EvpnTable for our partition id.
    DBTablePartition *GetTablePartition();

    /// @brief Notify the Broadcast MAC route for the given EvpnMcastNode.
    void NotifyNodeRoute(EvpnMcastNode *node);

    /// @brief Go through all replicator EvpnMcastNodes and notify associated Broadcast
    /// MAC route.
    void NotifyReplicatorNodeRoutes();

    /// @brief Go through all ingress replication client EvpnMcastNodes and notify the
    /// associated Broadcast MAC route.
    void NotifyIrClientNodeRoutes(bool exclude_edge_replication_supported);

    /// @brief Add an EvpnMcastNode to the EvpnManagerPartition.
    void AddMcastNode(EvpnMcastNode *node, EvpnRoute *route);

    /// @brief Delete an EvpnMcastNode from the EvpnManagerPartition.
    void DeleteMcastNode(EvpnMcastNode *node, EvpnRoute *route);

    /// @brief Update an EvpnMcastNode in the EvpnManagerPartition.
    /// Need to remove/add EvpnMcastNode from the replicator, leaf and ir client
    /// lists as appropriate.
    void UpdateMcastNode(EvpnMcastNode *node, EvpnRoute *route);

    /// @brief Delete an EvpnMcastNode from the EvpnManagerPartition.
    bool RemoveMcastNodeFromList(EvpnState::SG &sg, EvpnMcastNode *node,
                                 EvpnMcastNodeList *list);

    /// @brief Add the given MAC route to the update list.
    /// This method gets called either when the MAC route itself changes or when
    /// the remote PE list for the EvpnSegment of the MAC route gets updated.                             
    void TriggerMacRouteUpdate(EvpnRoute *route);

    /// @brief Return true if the EvpnManagerPartition is empty i.e. it has no local
    /// or remote EvpnMcastNodes and no MAC routes that need to be updated.
    bool empty() const;

    const EvpnMcastNodeList &remote_mcast_node_list() const {
        return remote_mcast_node_list_;
    }
    const EvpnMcastNodeList &local_mcast_node_list() const {
        return local_mcast_node_list_;
    }
    const EvpnMcastNodeList &leaf_node_list() const {
        return leaf_node_list_;
    }
    EvpnMcastNodeList *remote_mcast_node_list() {
        return &remote_mcast_node_list_;
    }
    EvpnMcastNodeList *local_mcast_node_list() {
        return &local_mcast_node_list_;
    }
    EvpnMcastNodeList *leaf_node_list() {
        return &leaf_node_list_;
    }

    /// @brief Return the BgpServer for the EvpnManagerPartition.
    BgpServer *server();

    /// @brief Return the EvpnTable for the EvpnManagerPartition.
    const EvpnTable *table() const;

    size_t part_id() const { return part_id_; }

private:
    friend class EvpnManager;
    friend class BgpEvpnManagerTest;

    typedef std::set<EvpnRoute *> EvpnRouteList;

    /// @brief Process the MAC route update list for this EvpnManagerPartition.
    bool ProcessMacUpdateList();

    /// @brief Disable processing of the update list.
    /// For testing only.
    void DisableMacUpdateProcessing();

    /// @brief Enable processing of the update list.
    /// For testing only.
    void EnableMacUpdateProcessing();

    EvpnStatePtr GetState(const SG &sg);
    EvpnStatePtr GetState(const SG &sg) const;
    EvpnStatePtr GetState(EvpnRoute *route);
    EvpnStatePtr LocateState(EvpnRoute *route);
    EvpnStatePtr LocateState(const SG &sg);
    EvpnStatePtr CreateState(const SG &sg);
    const EvpnState::StatesMap &states() const { return states_; }
    EvpnState::StatesMap &states() { return states_; }
    bool GetForestNodeAddress(ErmVpnRoute *rt, Ip4Address *address) const;
    void NotifyForestNode(EvpnRoute *route, ErmVpnTable *table);

    EvpnManager *evpn_manager_;
    size_t part_id_;
    EvpnState::StatesMap states_;;
    DBTablePartition *table_partition_;
    EvpnMcastNodeList local_mcast_node_list_;
    EvpnMcastNodeList remote_mcast_node_list_;
    EvpnMcastNodeList replicator_node_list_;
    EvpnMcastNodeList leaf_node_list_;
    EvpnMcastNodeList regular_node_list_;
    EvpnMcastNodeList ir_client_node_list_;
    EvpnRouteList mac_update_list_;
    boost::scoped_ptr<TaskTrigger> mac_update_trigger_;

    DISALLOW_COPY_AND_ASSIGN(EvpnManagerPartition);
};


/// @brief This class represents the EVPN manager for an EvpnTable in a VRF.
///
/// It is responsible for listening to route notifications on the associated
/// EvpnTable and implementing glue logic to massage routes so that vRouters
/// can communicate properly with EVPN PEs.
///
/// It currently implements glue logic for multicast connectivity between the
/// vRouters and EVPN PEs.  This is achieved by keeping track of local/remote
/// EvpnMcastNodes and constructing ingress replication OList for any given
/// EvpnLocalMcastNode when requested.
///
/// It also provides the EvpnTable class with an API to get the UpdateInfo for
/// a route in EvpnTable.  This is used by the table's Export method to build
/// the RibOutAttr for the broadcast MAC routes.  This is how we send ingress
/// replication OList information for an EVPN instance to the XMPP peers.
///
/// An EvpnManager keeps a vector of pointers to EvpnManagerPartitions.  The
/// number of partitions is the same as the DB partition count.  A partition
/// contains a subset of EvpnMcastNodes that are created based on EvpnRoutes
/// in the EvpnTable.
///
/// The concurrency model is that each EvpnManagerPartition can be updated and
/// can build the ingress replication OLists independently of other partitions.
///
/// The EvpnManager is also used to implement glue logic for EVPN aliasing when
/// remote PEs have multi-homed segments.
///
/// An EvpnManager maintains a map of EvpnSegments keyed by EthernetSegmentId.
/// It also keeps sets of EvpnSegments that need to be updated or evaluated for
/// deletion.
///
/// An EvpnSegment gets added to the segment update list in the EvpnManager when
/// there's a change in the AD route for the EvpnSegment. The update list gets
/// processed in the context of the bgp::EvpnSegment task.
///
/// An EvpnSegment gets added to the segment delete list in the EvpnManager when
/// the PE list becomes empty (bgp::EvpnSegment task) or when the MAC route list
/// for a given partition becomes empty (db::DBTable task).  The actual call to
/// MayDelete and subsequent destroy, if appropriate, happens in in the context
/// of bgp::EvpnSegment task.
///
/// The bgp::EvpnSegment task is mutually exclusive with the db::DBTable task.
class EvpnManager {
public:
    explicit EvpnManager(EvpnTable *table);
    virtual ~EvpnManager();

    /// @brief Initialize the EvpnManager. We allocate the EvpnManagerPartitions
    /// and register a DBListener for the EvpnTable. 
    virtual void Initialize();

    /// @brief Terminate the EvpnManager. We free the EvpnManagerPartitions
    /// and unregister from the EvpnTable.
    virtual void Terminate();

    /// @brief Construct export state for the given EvpnRoute. Note that the route
    /// only needs to be exported to the IPeer from which it was learnt.
    virtual UpdateInfo *GetUpdateInfo(EvpnRoute *route);

    /// @brief Get the EvpnManagerPartition for the given partition id.
    EvpnManagerPartition *GetPartition(size_t part_id);

    /// @brief Get the DBTablePartition for the EvpnTable for given partition id.
    DBTablePartition *GetTablePartition(size_t part_id);

    /// @brief Fill information for introspect command.
    /// Note that all IM routes are always in partition 0.
    void FillShowInfo(ShowEvpnTable *sevt) const;

    BgpServer *server();
    EvpnTable *table() { return table_; }
    const EvpnTable *table() const { return table_; }
    ErmVpnTable *ermvpn_table() { return ermvpn_table_; }
    const ErmVpnTable *ermvpn_table() const { return ermvpn_table_; }
    int listener_id() const { return listener_id_; }
    int ermvpn_listener_id() const { return ermvpn_listener_id_; }

    /// @brief Find or create the EvpnSegment for the given EthernetSegmentId.
    EvpnSegment *LocateSegment(const EthernetSegmentId &esi);

    /// @brief Find the EvpnSegment for the given EthernetSegmentId.
    EvpnSegment *FindSegment(const EthernetSegmentId &esi);

    /// @brief Trigger deletion of the given EvpnSegment.
    /// The EvpnSegment is added to a set of EvpnSegments that can potentially
    /// be deleted. This method can be invoked from multiple db::DBTable tasks
    /// in parallel when a MAC routes are removed from the dependency list in an
    /// EvpnSegment. Hence we ensure exclusive access using a write lock.
    ///
    /// The list is processed from the context of bgp::EvpnSegment task which is
    /// mutually exclusive with db::DBTable task.
    void TriggerSegmentDelete(EvpnSegment *segment);

    /// @brief Trigger update of the given EvpnSegment.
    /// The EvpnSegment is added to a set of EvpnSegments for which updates
    /// need triggered. This method is called in the context of db::DBTable
    /// task and a task instance of 0 since all AutoDisocvery routes always
    /// get sharded to partition 0.
    ///
    /// The set is processed in the context of bgp::EvpnSegment task, which
    /// is mutually exclusive with db::DBTable task.
    void TriggerSegmentUpdate(EvpnSegment *segment);

    /// @brief Trigger deletion of the EvpnManager and propagate the delete to any
    /// dependents.
    void ManagedDelete();

    /// @brief Initiate shutdown for the EvpnManager.
    void Shutdown();

    /// @brief Trigger deletion of the EvpnManager and propagate the delete to any
    /// dependents.
    bool MayDelete() const;

    /// @brief Attempt to enqueue a delete for the EvpnManager.
    void RetryDelete();

    /// @brief Return the LifetimeActor for the EvpnManager.
    LifetimeActor *deleter();

private:
    friend class BgpEvpnManagerTest;
    friend class BgpEvpnAliasingTest;

    class DeleteActor;
    typedef std::vector<EvpnManagerPartition *> PartitionList;
    typedef boost::ptr_map<const EthernetSegmentId, EvpnSegment> SegmentMap;
    typedef std::set<EvpnSegment *> SegmentSet;

    /// @brief Allocate the EvpnManagerPartitions.
    void AllocPartitions();

    /// @brief Free the EvpnManagerPartitions.
    void FreePartitions();

    /// @brief DBListener callback handler for AutoDisocvery routes in the EvpnTable.
    void AutoDiscoveryRouteListener(EvpnRoute *route);

    /// @brief DBListener callback handler for MacAdvertisement routes in the EvpnTable.
    void MacAdvertisementRouteListener(EvpnManagerPartition *partition,
        EvpnRoute *route);

    /// @brief DBListener callback handler for InclusiveMulticast routes in the EvpnTable.
    void InclusiveMulticastRouteListener(EvpnManagerPartition *partition,
        EvpnRoute *route);
        
    /// @brief DBListener callback handler for SelectiveMulticast routes in the EvpnTable.
    void SelectiveMulticastRouteListener(EvpnManagerPartition *partition,
        EvpnRoute *route);

    /// @brief DBListener callback handler for the EvpnTable.
    void RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry);

    /// @brief ErmVpnTable route listener callback function.
    ///
    /// Process changes (create/update/delete) to GlobalErmVpnRoute in vrf.ermvpn.0
    void ErmVpnRouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry);

    /// @brief Process the set of EvpnSegments that can potentially be deleted.
    /// Remove the EvpnSegment from the map and destroy if it's fine to
    /// to delete the EvpnSegment.
    bool ProcessSegmentDeleteSet();

    /// @brief Process the set of EvpnSegments that need to be updated.
    ///
    /// Go through each EvpnSegment and update it's PE list. Trigger updates
    /// of all it's dependent MAC routes if there's a change in the PE list.
    bool ProcessSegmentUpdateSet();

    /// @brief Check whether an ErmVpnRoute is locally originated GlobalTreeRoute.
    bool IsUsableGlobalTreeRootRoute(ErmVpnRoute *ermvpn_route) const;

    /// @brief Disable processing of the update list.
    /// For testing only.
    void DisableSegmentUpdateProcessing();

    /// @brief Enable processing of the update list.
    /// For testing only.
    void EnableSegmentUpdateProcessing();

    /// @brief Disable processing of the delete list.
    /// For testing only.
    void DisableSegmentDeleteProcessing();

    /// @brief Enable processing of the delete list.
    /// For testing only.
    void EnableSegmentDeleteProcessing();

    /// @brief Disable processing of the update lists in all partitions.
    /// For testing only.
    void DisableMacUpdateProcessing();

    /// @brief Enable processing of the update lists in all partitions.
    /// For testing only.    
    void EnableMacUpdateProcessing();

    /// @brief Set DB State and update count.
    void SetDBState(EvpnRoute *route, EvpnMcastNode *dbstate);

    /// @brief Create DB State and update count. If there is no DB State associated in the
    /// table, resume table deletion if the deletion was pending.
    void ClearDBState(EvpnRoute *route);

    EvpnTable *table_;
    ErmVpnTable *ermvpn_table_;
    int listener_id_;
    int ermvpn_listener_id_;
    tbb::atomic<int> db_states_count_;
    PartitionList partitions_;
    tbb::spin_rw_mutex segment_rw_mutex_;
    SegmentMap segment_map_;
    SegmentSet segment_delete_set_;
    SegmentSet segment_update_set_;
    boost::scoped_ptr<TaskTrigger> segment_delete_trigger_;
    boost::scoped_ptr<TaskTrigger> segment_update_trigger_;

    boost::scoped_ptr<DeleteActor> deleter_;
    LifetimeRef<EvpnManager> table_delete_ref_;

    DISALLOW_COPY_AND_ASSIGN(EvpnManager);
};

/// @brief Increment refcont atomically.
inline void intrusive_ptr_add_ref(EvpnState *evpn_state) {
    evpn_state->refcount_.fetch_and_increment();
}

/// @brief Decrement refcount of an evpn_state. If the refcount falls to 1, it implies
/// that there is no more reference to this particular state from any other data
/// structure. Hence, it can be deleted from the container map and destroyed as
/// well.
inline void intrusive_ptr_release(EvpnState *evpn_state) {
    int prev = evpn_state->refcount_.fetch_and_decrement();
    if (prev > 1)
        return;
    if (evpn_state->states()) {
        EvpnState::StatesMap::iterator iter =
            evpn_state->states()->find(evpn_state->sg());
        if (iter != evpn_state->states()->end()) {
            assert(iter->second == evpn_state);
            evpn_state->states()->erase(iter);

            // Attempt project manager deletion as it could be held up due to
            // this map being non-empty so far..
            if (evpn_state->manager()->deleter()->IsDeleted())
                evpn_state->manager()->deleter()->RetryDelete();
        }
    }
    delete evpn_state;
}

#define EVPN_RT_LOG(rt, ...) \
    RTINSTANCE_LOG(EvpnRoute, this->table()->routing_instance(), \
                   SandeshLevel::UT_DEBUG, \
                   RTINSTANCE_LOG_FLAG_ALL, \
                   (rt)->GetPrefix().source().to_string(), \
                   (rt)->GetPrefix().group().to_string(), \
                   (rt)->ToString(), ##__VA_ARGS__)

#define EVPN_ERMVPN_RT_LOG(rt, ...) \
    RTINSTANCE_LOG(EvpnErmVpnRoute, this->table()->routing_instance(), \
                   SandeshLevel::UT_DEBUG, \
                   RTINSTANCE_LOG_FLAG_ALL, \
                   (rt)->GetPrefix().source().to_string(), \
                   (rt)->GetPrefix().group().to_string(), \
                   (rt)->ToString(), ##__VA_ARGS__)

#define EVPN_TRACE(type, ...) \
    RTINSTANCE_LOG(type, this->table()->routing_instance(), \
        SandeshLevel::UT_DEBUG, RTINSTANCE_LOG_FLAG_ALL, ##__VA_ARGS__)

#endif  // SRC_BGP_BGP_EVPN_H_