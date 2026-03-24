/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "ifmap/ifmap_graph_walker.h"

#include "base/logging.h"
#include "base/task_trigger.h"
#include "db/db_graph.h"
#include "db/db_table.h"
#include "ifmap/ifmap_client.h"
#include "ifmap/ifmap_exporter.h"
#include "ifmap/ifmap_link.h"
#include "ifmap/ifmap_log.h"
#include "ifmap/ifmap_table.h"
#include "ifmap/ifmap_server.h"
#include "ifmap/ifmap_log_types.h"
#include "ifmap/ifmap_update.h"
#include "ifmap/ifmap_util.h"
#include "schema/vnc_cfg_types.h"

class GraphPropagateFilter : public DBGraph::VisitorFilter {
public:
    GraphPropagateFilter(IFMapExporter *exporter,
                         const IFMapTypenameWhiteList *type_filter,
                         const BitSet &bitset)
            : exporter_(exporter),
              type_filter_(type_filter),
              bset_(bitset) {
    }

    bool VertexFilter(const DBGraphVertex *vertex) const {
        return type_filter_->VertexFilter(vertex);
    }

    bool EdgeFilter(const DBGraphVertex *source, const DBGraphVertex *target,
                    const DBGraphEdge *edge) const {
        const IFMapNode *tgt = static_cast<const IFMapNode *>(target);
        const IFMapNodeState *state = NodeStateLookup(tgt);
        if (state != NULL && state->interest().Contains(bset_)) {
            return false;
        }

        return true;
    }

    const IFMapNodeState *NodeStateLookup(const IFMapNode *node) const {
        const DBTable *table = node->table();
        const DBState *state =
                node->GetState(table, exporter_->TableListenerId(table));
        return static_cast<const IFMapNodeState *>(state);
    }

    DBGraph::VisitorFilter::AllowedEdgeRetVal AllowedEdges(
                                       const DBGraphVertex *vertex) const {
        return type_filter_->AllowedEdges(vertex);
    }
private:
    IFMapExporter *exporter_;
    const IFMapTypenameWhiteList *type_filter_;
    const BitSet &bset_;
};

IFMapGraphWalker::IFMapGraphWalker(DBGraph *graph, IFMapExporter *exporter)
    : graph_(graph),
      exporter_(exporter),
      link_delete_walk_trigger_(new TaskTrigger(
          [this](){ return LinkDeleteWalk(); },
          TaskScheduler::GetInstance()->GetTaskId("db::IFMapTable"), 0)),
      walk_client_index_(BitSet::npos) {
    traversal_white_list_.reset(new IFMapTypenameWhiteList());
    AddNodesToWhitelist();
}

IFMapGraphWalker::~IFMapGraphWalker() {
}

void IFMapGraphWalker::NotifyEdge(DBGraphEdge *edge, const BitSet &bset) {
    DBTable *table = exporter_->link_table();
    table->Change(edge);
}

void IFMapGraphWalker::JoinVertex(DBGraphVertex *vertex, const BitSet &bset) {
    IFMapNode *node = static_cast<IFMapNode *>(vertex);
    IFMapNodeState *state = exporter_->NodeStateLocate(node);
    IFMAP_DEBUG(JoinVertex, vertex->ToString(), state->interest().ToString(),
               bset.ToString());
    exporter_->StateInterestOr(state, bset);
    node->table()->Change(node);
}

void IFMapGraphWalker::ProcessLinkAdd(IFMapNode *lnode, IFMapNode *rnode,
                                      const BitSet &bset) {
    GraphPropagateFilter filter(exporter_, traversal_white_list_.get(), bset);
    graph_->Visit(rnode,
                  [this, &bset](DBGraphVertex *v) { JoinVertex(v, bset); },
                  [this, &bset](DBGraphEdge *e)   { NotifyEdge(e, bset); },
                  filter);
}

void IFMapGraphWalker::LinkAdd(IFMapLink *link, IFMapNode *lnode, const BitSet &lhs,
                               IFMapNode *rnode, const BitSet &rhs) {
    IFMAP_DEBUG(LinkOper, "LinkAdd", lnode->ToString(), rnode->ToString(),
                lhs.ToString(), rhs.ToString());

    // Ensure that nodes are passed are indeed nodes and not links.
    assert(dynamic_cast<IFMapNode *>(lnode));
    assert(dynamic_cast<IFMapNode *>(rnode));

    assert(!dynamic_cast<IFMapLink *>(lnode));
    assert(!dynamic_cast<IFMapLink *>(rnode));

    if (!lhs.empty() && !rhs.Contains(lhs) &&
        traversal_white_list_->VertexFilter(rnode) &&
        traversal_white_list_->EdgeFilter(lnode, rnode, link))  {
        ProcessLinkAdd(lnode, rnode, lhs);
    }
    if (!rhs.empty() && !lhs.Contains(rhs) &&
        traversal_white_list_->VertexFilter(lnode) &&
        traversal_white_list_->EdgeFilter(rnode, lnode, link)) {
        ProcessLinkAdd(rnode, lnode, rhs);
    }
}

void IFMapGraphWalker::LinkRemove(const BitSet &bset) {
    OrLinkDeleteClients(bset);          // link_delete_clients_ | bset
    link_delete_walk_trigger_->Set();
}

// Check if the neighbor or link to neighbor should be filtered. Returns true
// if rnode or link to rnode should be filtered.
bool IFMapGraphWalker::FilterNeighbor(IFMapNode *lnode, IFMapLink *link) {
    IFMapNode *rnode = link->left();
    if (rnode == lnode)
        rnode = link->right();
    if (!traversal_white_list_->VertexFilter(rnode) ||
        !traversal_white_list_->EdgeFilter(lnode, NULL, link)) {
        return true;
    }
    return false;
}

void IFMapGraphWalker::RecomputeInterest(DBGraphVertex *vertex, int bit) {
    IFMapNode *node = static_cast<IFMapNode *>(vertex);
    IFMapNodeState *state = exporter_->NodeStateLocate(node);
    state->nmask_set(bit);
    UpdateNewReachableNodesTracker(bit, state);
}

bool IFMapGraphWalker::LinkDeleteWalk() {
    if (link_delete_clients_.empty()) {
        walk_client_index_ = BitSet::npos;
        return true;
    }

    IFMapServer *server = exporter_->server();
    size_t i;

    // Get the index of the client we want to start with.
    if (walk_client_index_ == BitSet::npos) {
        i = link_delete_clients_.find_first();
    } else {
        // walk_client_index_ was the last client that we finished processing.
        i = link_delete_clients_.find_next(walk_client_index_);
    }
    int count = 0;
    BitSet done_set;
    while (i != BitSet::npos) {
        IFMapClient *client = server->GetClient(i);
        assert(client);
        AddNewReachableNodesTracker(client->index());

        IFMapTable *table = IFMapTable::FindTable(server->database(),
                                                  "virtual-router");
        IFMapNode *node = table->FindNode(client->identifier());
        if ((node != NULL) && node->IsVertexValid()) {
            graph_->Visit(node,
                [this, i](DBGraphVertex *v) { RecomputeInterest(v, i); },
                0, *traversal_white_list_.get());
        }
        done_set.set(i);
        if (++count == kMaxLinkDeleteWalks) {
            // client 'i' has been processed. If 'i' is the last bit set, we
            // will return true below. Else we will return false and there
            // is atleast one more bit left to process.
            break;
        }

        i = link_delete_clients_.find_next(i);
    }
    // Remove the subset of clients that we have finished processing.
    ResetLinkDeleteClients(done_set);

    LinkDeleteWalkBatchEnd(done_set);

    if (link_delete_clients_.empty()) {
        walk_client_index_ = BitSet::npos;
        return true;
    } else {
        walk_client_index_ = i;
        return false;
    }
}

void IFMapGraphWalker::OrLinkDeleteClients(const BitSet &bset) {
    link_delete_clients_.Set(bset);     // link_delete_clients_ | bset
}

void IFMapGraphWalker::ResetLinkDeleteClients(const BitSet &bset) {
    link_delete_clients_.Reset(bset);
}

void IFMapGraphWalker::CleanupInterest(int client_index, IFMapNode *node,
                                       IFMapNodeState *state) {
    BitSet rm_mask;
    rm_mask.set(client_index);

    // interest = interest - rm_mask + nmask

    if (!state->interest().empty() && !state->nmask().empty()) {
        IFMAP_DEBUG(CleanupInterest, node->ToString(),
                    state->interest().ToString(), rm_mask.ToString(),
                    state->nmask().ToString());
    }
    BitSet ninterest;
    ninterest.BuildComplement(state->interest(), rm_mask);
    ninterest |= state->nmask();
    state->nmask_clear();
    if (state->interest() == ninterest) {
        return;
    }

    exporter_->StateInterestSet(state, ninterest);
    node->table()->Change(node);

    // Mark all dependent links as potentially modified.
    for (IFMapNodeState::iterator iter = state->begin();
         iter != state->end(); ++iter) {
        DBTable *table = exporter_->link_table();
        table->Change(iter.operator->());
    }
}

// Cleanup all the graph nodes that were reachable before this link delete.
// After this link delete, these nodes may still be reachable. But, its
// also possible that the link delete has made them unreachable.
void IFMapGraphWalker::OldReachableNodesCleanupInterest(int client_index) {
    IFMapState *state = NULL;
    IFMapNode *node = NULL;
    IFMapExporter::Cs_citer iter = exporter_->ClientConfigTrackerBegin(
        IFMapExporter::INTEREST, client_index);
    IFMapExporter::Cs_citer end_iter = exporter_->ClientConfigTrackerEnd(
        IFMapExporter::INTEREST, client_index);

    while (iter != end_iter) {
        state = *iter;
        // Get the iterator to the next element before calling
        // CleanupInterest() since the state might be removed from the
        // client's config-tracker, thereby invalidating the iterator of the
        // container we are iterating over.
        ++iter;
        if (state->IsNode()) {
            node = state->GetIFMapNode();
            assert(node);
            IFMapNodeState *nstate = exporter_->NodeStateLookup(node);
            assert(state == nstate);
            CleanupInterest(client_index, node, nstate);
        }
    }
}

// Cleanup all the graph nodes that were not reachable before the link delete
// but are reachable now. Note, we store nodes in new_reachable_nodes_tracker_
// only if we visited them during the graph-walk via RecomputeInterest() and if
// their interest bit was not set i.e. they were not reachable before we
// started the walk.
void IFMapGraphWalker::NewReachableNodesCleanupInterest(int client_index) {
    IFMapState *state = NULL;
    IFMapNode *node = NULL;
    ReachableNodesSet *rnset = new_reachable_nodes_tracker_.at(client_index);

    for (Rns_citer iter = rnset->begin(); iter != rnset->end(); ++iter) {
        state = *iter;
        assert(state->IsNode());
        node = state->GetIFMapNode();
        assert(node);
        IFMapNodeState *nstate = exporter_->NodeStateLookup(node);
        assert(state == nstate);
        CleanupInterest(client_index, node, nstate);
    }
    DeleteNewReachableNodesTracker(client_index);
}

void IFMapGraphWalker::LinkDeleteWalkBatchEnd(const BitSet &done_set) {
    for (size_t i = done_set.find_first(); i != BitSet::npos;
            i = done_set.find_next(i)) {
        // Examine all the nodes that were reachable before the link delete.
        OldReachableNodesCleanupInterest(i);
        // Examine all the nodes that were not reachable before the link
        // delete but are now reachable.
        NewReachableNodesCleanupInterest(i);
    }
}

void IFMapGraphWalker::AddNewReachableNodesTracker(int client_index) {
    if (client_index >= (int)new_reachable_nodes_tracker_.size()) {
        new_reachable_nodes_tracker_.resize(client_index + 1, NULL);
    }
    assert(new_reachable_nodes_tracker_[client_index] == NULL);
    ReachableNodesSet *rnset = new ReachableNodesSet();
    new_reachable_nodes_tracker_[client_index] = rnset;
}

void IFMapGraphWalker::DeleteNewReachableNodesTracker(int client_index) {
    ReachableNodesSet *rnset = new_reachable_nodes_tracker_.at(client_index);
    assert(rnset);
    delete rnset;
    new_reachable_nodes_tracker_[client_index] = NULL;
}

// Keep track of this node if it was unreachable earlier.
void IFMapGraphWalker::UpdateNewReachableNodesTracker(int client_index,
                                                      IFMapState *state) {
    ReachableNodesSet *rnset = new_reachable_nodes_tracker_.at(client_index);
    assert(rnset);
    // If the interest is not set, the node was not reachable earlier but is
    // reachable now.
    if (!state->interest().test(client_index)) {
        rnset->insert(state);
    }
}

const IFMapTypenameWhiteList &IFMapGraphWalker::get_traversal_white_list()
        const {
    return *traversal_white_list_.get();
}

// The nodes listed below and the nodes in
// IFMapGraphTraversalFilterCalculator::CreateNodeBlackList() are mutually
// exclusive
void IFMapGraphWalker::AddNodesToWhitelist() {
    traversal_white_list_->include_vertex = {
        {"virtual-router", {
            "physical-router-virtual-router",
            "virtual-router-virtual-machine",
            "virtual-router-network-ipam",
            "global-system-config-virtual-router",
            "provider-attachment-virtual-router",
            "virtual-router-virtual-machine-interface",
            "virtual-router-sub-cluster",
        }},
        {"virtual-router-network-ipam", {
            "virtual-router-network-ipam",
        }},
        {"virtual-machine", {
            "virtual-machine-service-instance",
            "virtual-machine-interface-virtual-machine",
            "virtual-machine-tag",
        }},
        {"control-node-zone", {}},
        {"sub-cluster", {
            "bgp-router-sub-cluster",
        }},
        {"bgp-router", {
            "instance-bgp-router",
            "physical-router-bgp-router",
            "bgp-router-control-node-zone",
        }},
        {"bgp-as-a-service", {
            "bgpaas-bgp-router",
            "bgpaas-health-check",
            "bgpaas-control-node-zone",
        }},
        {"bgpaas-control-node-zone", {
            "bgpaas-control-node-zone",
        }},
        {"global-system-config", {
            "global-system-config-global-vrouter-config",
            "global-system-config-global-qos-config",
            "global-system-config-bgp-router",
            "qos-config-global-system-config",
        }},
        {"provider-attachment", {}},
        {"service-instance", {
            "service-instance-service-template",
            "service-instance-port-tuple",
        }},
        {"global-vrouter-config", {
            "application-policy-set-global-vrouter-config",
            "global-vrouter-config-security-logging-object",
        }},
        {"virtual-machine-interface", {
            "virtual-machine-virtual-machine-interface",
            "virtual-machine-interface-sub-interface",
            "instance-ip-virtual-machine-interface",
            "virtual-machine-interface-virtual-network",
            "virtual-machine-interface-security-group",
            "floating-ip-virtual-machine-interface",
            "alias-ip-virtual-machine-interface",
            "customer-attachment-virtual-machine-interface",
            "virtual-machine-interface-routing-instance",
            "virtual-machine-interface-route-table",
            "subnet-virtual-machine-interface",
            "service-port-health-check",
            "bgpaas-virtual-machine-interface",
            "virtual-machine-interface-qos-config",
            "virtual-machine-interface-bridge-domain",
            "virtual-machine-interface-security-logging-object",
            "project-virtual-machine-interface",
            "port-tuple-interface",
            "virtual-machine-interface-tag",
            "virtual-machine-interface-bgp-router",
        }},
        {"virtual-machine-interface-bridge-domain", {
            "virtual-machine-interface-bridge-domain",
        }},
        {"security-group", {
            "security-group-access-control-list",
        }},
        {"physical-router", {
            "physical-router-physical-interface",
            "physical-router-logical-interface",
            "physical-router-virtual-network",
        }},
        {"service-template", {
            "domain-service-template",
        }},
        {"instance-ip", {
            "instance-ip-virtual-network",
        }},
        {"virtual-network", {
            "virtual-network-floating-ip-pool",
            "virtual-network-alias-ip-pool",
            "virtual-network-network-ipam",
            "virtual-network-access-control-list",
            "virtual-network-routing-instance",
            "virtual-network-qos-config",
            "virtual-network-bridge-domain",
            "virtual-network-security-logging-object",
            "virtual-network-tag",
            "virtual-network-provider-network",
            "virtual-network-multicast-policy",
            "vn-health-check",
            "host-based-service-virtual-network",
            "project-virtual-network",
        }},
        {"floating-ip", {
            "floating-ip-pool-floating-ip",
            "instance-ip-floating-ip",
        }},
        {"alias-ip", {
            "alias-ip-pool-alias-ip",
        }},
        {"customer-attachment", {}},
        {"virtual-machine-interface-routing-instance", {
            "virtual-machine-interface-routing-instance",
        }},
        {"physical-interface", {
            "physical-interface-logical-interface",
            "virtual-port-group-physical-interface",
        }},
        {"virtual-port-group-physical-interface", {
            "virtual-port-group-physical-interface",
        }},
        {"virtual-port-group", {
            "virtual-port-group-virtual-machine-interface",
            "virtual-port-group-physical-interface",
        }},
        {"domain", {
            "domain-namespace",
            "domain-virtual-DNS",
        }},
        {"floating-ip-pool", {
            "virtual-network-floating-ip-pool",
        }},
        {"alias-ip-pool", {
            "virtual-network-alias-ip-pool",
        }},
        {"logical-interface", {
            "logical-interface-virtual-machine-interface",
        }},
        {"logical-router-virtual-network", {
            "logical-router-virtual-network",
        }},
        {"logical-router", {
            "logical-router-virtual-network",
            "logical-router-interface",
        }},
        {"virtual-network-network-ipam", {
            "virtual-network-network-ipam",
        }},
        {"access-control-list", {}},
        {"routing-instance", {}},
        {"namespace", {}},
        {"virtual-DNS", {
            "virtual-DNS-virtual-DNS-record",
        }},
        {"network-ipam", {
            "network-ipam-virtual-DNS",
        }},
        {"virtual-DNS-record", {}},
        {"interface-route-table", {}},
        {"subnet", {}},
        {"service-health-check", {}},
        {"qos-config", {}},
        {"qos-queue", {}},
        {"forwarding-class", {
            "forwarding-class-qos-queue",
        }},
        {"global-qos-config", {
            "global-qos-config-forwarding-class",
            "global-qos-config-qos-queue",
            "global-qos-config-qos-config",
        }},
        {"bridge-domain", {}},
        {"security-logging-object", {
            "virtual-network-security-logging-object",
            "virtual-machine-interface-security-logging-object",
            "global-vrouter-config-security-logging-object",
            "security-logging-object-network-policy",
            "security-logging-object-security-group",
        }},
        {"tag", {
            "application-policy-set-tag",
        }},
        {"application-policy-set", {
            "application-policy-set-firewall-policy",
            "policy-management-application-policy-set",
        }},
        {"application-policy-set-firewall-policy", {
            "application-policy-set-firewall-policy",
        }},
        {"firewall-policy", {
            "firewall-policy-firewall-rule",
            "firewall-policy-security-logging-object",
        }},
        {"firewall-policy-firewall-rule", {
            "firewall-policy-firewall-rule",
        }},
        {"firewall-policy-security-logging-object", {
            "firewall-policy-security-logging-object",
        }},
        {"firewall-rule", {
            "firewall-rule-tag",
            "firewall-rule-service-group",
            "firewall-rule-address-group",
            "firewall-rule-security-logging-object",
        }},
        {"firewall-rule-security-logging-object", {
            "firewall-rule-security-logging-object",
        }},
        {"service-group", {}},
        {"address-group", {
            "address-group-tag",
        }},
        {"host-based-service", {
            "host-based-service-virtual-network",
        }},
        {"host-based-service-virtual-network", {
            "virtual-network",
        }},
        {"project", {
            "project-tag",
            "project-logical-router",
            "project-host-based-service",
        }},
        {"port-tuple", {
            "service-instance-port-tuple",
            "port-tuple-interface",
        }},
        {"policy-management", {}},
        {"multicast-policy", {
            "virtual-network-multicast-policy",
        }},
    };
}

