/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/l3vpn/inetvpn_table.h"

#include "bgp/ipeer.h"
#include "bgp/bgp_server.h"
#include "bgp/bgp_update.h"
#include "bgp/inet/inet_table.h"
#include "bgp/bgp_peer.h"
#include "bgp/routing-instance/routing_instance.h"
#include "net/rd.h"

using std::unique_ptr;
using std::string;

InetVpnTable::InetVpnTable(DB *db, const string &name)
        : BgpTable(db, name) {
}

unique_ptr<DBEntry> InetVpnTable::AllocEntry(const DBRequestKey *key) const {
    const RequestKey *pfxkey = static_cast<const RequestKey *>(key);
    return unique_ptr<DBEntry> (new InetVpnRoute(pfxkey->prefix));
}


unique_ptr<DBEntry> InetVpnTable::AllocEntryStr(const string &key_str) const {
    InetVpnPrefix prefix = InetVpnPrefix::FromString(key_str);
    return unique_ptr<DBEntry> (new InetVpnRoute(prefix));
}

size_t InetVpnTable::Hash(const DBEntry *entry) const {
    const InetVpnRoute *rt_entry = static_cast<const InetVpnRoute *>(entry);
    const InetVpnPrefix &inetvpnprefix = rt_entry->GetPrefix();
    Ip4Prefix prefix(inetvpnprefix.addr(), inetvpnprefix.prefixlen());
    size_t value = InetTable::HashFunction(prefix);
    return value % DB::PartitionCount();
}

size_t InetVpnTable::Hash(const DBRequestKey *key) const {
    const RequestKey *rkey = static_cast<const RequestKey *>(key);
    Ip4Prefix prefix(rkey->prefix.addr(), rkey->prefix.prefixlen());
    size_t value = InetTable::HashFunction(prefix);
    return value % DB::PartitionCount();
}

BgpRoute *InetVpnTable::TableFind(DBTablePartition *rtp,
                                  const DBRequestKey *prefix) {
    const RequestKey *pfxkey = static_cast<const RequestKey *>(prefix);
    InetVpnRoute rt_key(pfxkey->prefix);
    return static_cast<BgpRoute *>(rtp->Find(&rt_key));
}

DBTableBase *InetVpnTable::CreateTable(DB *db, const string &name) {
    InetVpnTable *table = new InetVpnTable(db, name);
    table->Init();
    return table;
}

static RouteDistinguisher GenerateDistinguisher(
        const BgpTable *src_table, const BgpPath *src_path) {
    const RouteDistinguisher &source_rd = src_path->GetAttr()->source_rd();
    if (!src_path->GetPeer() || !(src_path->GetPeer()->IsRouterTypeBGPaaS())){
        if (!source_rd.IsZero())
        return source_rd;

        assert(!src_path->GetPeer() || !src_path->GetPeer()->IsXmppPeer());
        const RoutingInstance *src_instance = src_table->routing_instance();
        return *src_instance->GetRD();
    }
    boost::system::error_code ec;
    Ip4Address addr = Ip4Address::from_string(src_path->GetPeer()->ToString(), ec);
    if ((ec.value() != 0)) {
        if (!source_rd.IsZero())
        return source_rd;

        assert(!src_path->GetPeer() || !src_path->GetPeer()->IsXmppPeer());
        const RoutingInstance *src_instance = src_table->routing_instance();
        return *src_instance->GetRD();
    } else {
        int vrf_id = 0;
        RouteDistinguisher new_source_rd = RouteDistinguisher(addr.to_ulong(), vrf_id);
        return new_source_rd;
    }
}

BgpRoute *InetVpnTable::RouteReplicate(BgpServer *server,
        BgpTable *src_table, BgpRoute *src_rt, const BgpPath *src_path,
        ExtCommunityPtr community) {
    assert(src_table->family() == Address::INET);

    InetRoute *inet = dynamic_cast<InetRoute *> (src_rt);
    assert(inet);

    const RouteDistinguisher &rd = GenerateDistinguisher(src_table, src_path);

    InetVpnPrefix vpn(rd, inet->GetPrefix().ip4_addr(),
                      inet->GetPrefix().prefixlen());

    InetVpnRoute rt_key(vpn);

    DBTablePartition *rtp =
        static_cast<DBTablePartition *>(GetTablePartition(&rt_key));
    BgpRoute *dest_route = static_cast<BgpRoute *>(rtp->Find(&rt_key));
    if (dest_route == NULL) {
        dest_route = new InetVpnRoute(vpn);
        rtp->Add(dest_route);
    } else {
        dest_route->ClearDelete();
    }

    BgpAttrDB *attr_db = server->attr_db();

    BgpAttrPtr new_attr =
        server->attr_db()->ReplaceExtCommunityAndLocate(src_path->GetAttr(),
                                                        community);
    new_attr = attr_db->ReplaceSourceRdAndLocate(new_attr.get(), rd);
    // Check whether there's already a path with the given peer and path id.
    BgpPath *dest_path =
        dest_route->FindSecondaryPath(src_rt, src_path->GetSource(),
                                      src_path->GetPeer(),
                                      src_path->GetPathId());
    if (dest_path != NULL) {
        if ((new_attr != dest_path->GetOriginalAttr()) ||
            (src_path->GetFlags() != dest_path->GetFlags()) ||
            (src_path->GetLabel() != dest_path->GetLabel())) {
            // Update Attributes and notify (if needed)
            assert(dest_route->RemoveSecondaryPath(src_rt,
                                src_path->GetSource(), src_path->GetPeer(),
                                src_path->GetPathId()));
        } else {
            return dest_route;
        }
    }

    // Create replicated path and insert it on the route
    BgpSecondaryPath *replicated_path =
        new BgpSecondaryPath(src_path->GetPeer(), src_path->GetPathId(),
                            src_path->GetSource(), new_attr,
                            src_path->GetFlags(), src_path->GetLabel());
    replicated_path->SetReplicateInfo(src_table, src_rt);
    dest_route->InsertPath(replicated_path);

    // Always trigger notification.
    rtp->Notify(dest_route);

    // Update corresponding route's extended communities in inet.0 table.
    InetTable *inet_table =
        dynamic_cast<InetTable *>(routing_instance()->GetTable(Address::INET));
    InetVpnRoute *inetvpn_route = dynamic_cast<InetVpnRoute *>(dest_route);
    inet_table->UpdateRoute(inetvpn_route->GetPrefix(), src_path->GetPeer(),
                            new_attr);
    return dest_route;
}

bool InetVpnTable::Export(RibOut *ribout, Route *route,
        const RibPeerSet &peerset, UpdateInfoSList &uinfo_slist) {
    BgpRoute *bgp_route = static_cast<BgpRoute *> (route);
    UpdateInfo *uinfo = GetUpdateInfo(ribout, bgp_route, peerset);
    if (!uinfo) return false;
    uinfo_slist->push_front(*uinfo);

    return true;
}

static void RegisterFactory() {
    DB::RegisterFactory("bgp.l3vpn.0", &InetVpnTable::CreateTable);
}
MODULE_INITIALIZER(RegisterFactory);
