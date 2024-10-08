/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */
#include <cmn/agent_cmn.h>

#include <ksync/ksync_index.h>
#include <ksync/ksync_entry.h>
#include <ksync/ksync_object.h>

#include <vnc_cfg_types.h>
#include <bgp_schema_types.h>
#include <agent_types.h>

#include <oper/peer.h>
#include <oper/vrf.h>
#include <oper/interface_common.h>
#include <oper/nexthop.h>
#include <oper/multicast.h>
#include <oper/vn.h>
#include <oper/mirror_table.h>
#include <oper/vxlan.h>
#include <oper/mpls.h>
#include <oper/route_common.h>
#include <oper/bridge_route.h>

#include <vxlan_agent/ksync_vxlan.h>
#include <vxlan_agent/ksync_vxlan_bridge.h>
#include <vxlan_agent/ksync_vxlan_route.h>
#include <vxlan_agent/ksync_vxlan_port.h>

#include "linux_vxlan.h"
#include "linux_bridge.h"
#include "linux_port.h"
#include "linux_fdb.h"

KSyncLinuxVxlan::KSyncLinuxVxlan(Agent *agent) : KSyncVxlan(agent) {
    set_bridge_obj(new KSyncLinuxBridgeObject(this));
    set_port_obj(new KSyncLinuxPortObject(this));
    set_vrf_obj(new KSyncLinuxVrfObject(this));
}

void KSyncLinuxVxlan::Init() {
}

static void Execute(const string &str) {
    std::cout << str << std::endl;
    if (system(str.c_str()) < 0) {
        LOG(ERROR, "Error executing command : " << str);
    }
}

/****************************************************************************
 * Implementation of KSyncLinuxBridgeEntry
 ****************************************************************************/
KSyncLinuxBridgeEntry::KSyncLinuxBridgeEntry(KSyncLinuxBridgeObject *obj,
                                             const VxLanId *vxlan) :
    KSyncVxlanBridgeEntry(obj, vxlan) {
    std::stringstream s;
    s << "br-" << vxlan_id();
    name_ = s.str();

    std::stringstream s1;
    s1 << "vxlan-" << vxlan_id();
    vxlan_port_name_ = s1.str();
}

KSyncLinuxBridgeEntry::KSyncLinuxBridgeEntry(KSyncLinuxBridgeObject *obj,
                                         const KSyncLinuxBridgeEntry *entry) :
    KSyncVxlanBridgeEntry(obj, entry) {
    std::stringstream s;
    s << "br-" << vxlan_id();
    name_ = s.str();

    std::stringstream s1;
    s1 << "vxlan-" << vxlan_id();
    vxlan_port_name_ = s1.str();
}

bool KSyncLinuxBridgeEntry::Add() {
    std::stringstream s;
    s << "brctl addbr " << name_;
    Execute(s.str());

    s.str("");
    s << "ip link add " << vxlan_port_name_ << " type vxlan id " << vxlan_id()
        << " group 239.1.1.1 dstport 0";
    Execute(s.str());

    s.str("");
    s << "brctl addif " << name_ << " " << vxlan_port_name_;
    Execute(s.str());

    s.str("");
    s << "ifconfig " << name_ << " up";
    Execute(s.str());

    s.str("");
    s << "ifconfig " << vxlan_port_name_ << " up";
    Execute(s.str());

    return true;
}

bool KSyncLinuxBridgeEntry::Delete() {
    std::stringstream s;
    s << "brctl delif " << name_ << " " << vxlan_port_name_;
    Execute(s.str());

    s.str("");
    s << "ip link del " << vxlan_port_name_ << " type vxlan id " << vxlan_id();
    Execute(s.str());

    s.str("");
    s << " brctl delbr " << name_;
    Execute(s.str());
    return true;
}

/****************************************************************************
 * Implementation of KSyncLinuxBridgeObject
 ****************************************************************************/
KSyncLinuxBridgeObject::KSyncLinuxBridgeObject(KSyncLinuxVxlan *ksync) :
    KSyncVxlanBridgeObject(ksync) {
}

KSyncEntry *KSyncLinuxBridgeObject::Alloc(const KSyncEntry *entry,
                                          uint32_t index) {
    const KSyncLinuxBridgeEntry *bridge =
        static_cast<const KSyncLinuxBridgeEntry *>(entry);
    return new KSyncLinuxBridgeEntry(this, bridge);
}

KSyncEntry *KSyncLinuxBridgeObject::DBToKSyncEntry(const DBEntry *e) {
    const VxLanId *vxlan = static_cast<const VxLanId *>(e);
    return static_cast<KSyncEntry *>(new KSyncLinuxBridgeEntry(this, vxlan));
}

/****************************************************************************
 * Implementation of KSyncLinuxPortEntry
 ****************************************************************************/
KSyncLinuxPortEntry::KSyncLinuxPortEntry(KSyncLinuxPortObject *obj,
                                         const Interface *intrface) :
    KSyncVxlanPortEntry(obj, intrface), old_bridge_(nullptr) {
}

KSyncLinuxPortEntry::KSyncLinuxPortEntry(KSyncVxlanPortObject *obj,
                                         const KSyncLinuxPortEntry *entry) :
    KSyncVxlanPortEntry(obj, entry), old_bridge_(nullptr) {
}

bool KSyncLinuxPortEntry::Add() {
    const KSyncLinuxBridgeEntry *br =
        dynamic_cast<const KSyncLinuxBridgeEntry *>(bridge());

    if (br == old_bridge_) {
        return true;
    }

    std::stringstream s;
    if (old_bridge_) {
        s << "brctl delif " << old_bridge_->name() << " " << port_name();
        Execute(s.str());
    }

    if (br) {
        s.str("");
        old_bridge_ = br;
        s << "brctl addif " << br->name() << " " << port_name();
        Execute(s.str());
    }
    return true;
}

bool KSyncLinuxPortEntry::Change() {
    return Add();
}

bool KSyncLinuxPortEntry::Delete() {
    if (old_bridge_ == nullptr)
        return true;

    std::stringstream s;
    s << "brctl delif " << old_bridge_->name() << " " << port_name();
    Execute(s.str());
    return true;
}

/****************************************************************************
 * Implementation of KSyncLinuxPortObject
 ****************************************************************************/
KSyncLinuxPortObject::KSyncLinuxPortObject(KSyncLinuxVxlan *ksync) :
    KSyncVxlanPortObject(ksync) {
}

KSyncEntry *KSyncLinuxPortObject::Alloc(const KSyncEntry *entry,
                                        uint32_t index) {
    const KSyncLinuxPortEntry *intrface =
        static_cast<const KSyncLinuxPortEntry *>(entry);
    return new KSyncLinuxPortEntry(this, intrface);
}

KSyncEntry *KSyncLinuxPortObject::DBToKSyncEntry(const DBEntry *e) {
    const Interface *intrface = static_cast<const Interface *>(e);

    switch (intrface->type()) {
    case Interface::PHYSICAL:
    case Interface::VM_INTERFACE:
        return new KSyncLinuxPortEntry(this, intrface);
        break;

    default:
        assert(0);
        break;
    }
    return nullptr;
}

/****************************************************************************
 * Implementation of KSyncLinuxFdbEntry
 ****************************************************************************/
KSyncLinuxFdbEntry::KSyncLinuxFdbEntry(KSyncLinuxFdbObject *obj,
                                       const KSyncLinuxFdbEntry *entry) :
    KSyncVxlanFdbEntry(obj, entry) {
}

KSyncLinuxFdbEntry::KSyncLinuxFdbEntry(KSyncLinuxFdbObject *obj,
                                       const BridgeRouteEntry *route) :
    KSyncVxlanFdbEntry(obj, route) {
}

KSyncLinuxFdbEntry::~KSyncLinuxFdbEntry() {
}

static void MacToStr(char *buff, const MacAddress &mac) {
    sprintf(buff, "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]);
}

bool KSyncLinuxFdbEntry::Add() {
    char buff[64];
    MacToStr(buff, mac());

    if (port() != nullptr) {
        std::stringstream s;
        s << "bridge fdb add " << buff << " dev " << port()->port_name()
            << " master";
        //Execute(s.str());
    } else if (tunnel_dest().to_ulong() != 0) {
        const KSyncLinuxBridgeEntry *br =
            dynamic_cast<const KSyncLinuxBridgeEntry *>(bridge());

        std::stringstream s;
        s << "bridge fdb add " << buff << " dst " << tunnel_dest() << " dev "
            << br->vxlan_port_name();
        Execute(s.str());
    }

    return true;
}

bool KSyncLinuxFdbEntry::Change() {
    return Add();
}

bool KSyncLinuxFdbEntry::Delete() {
    char buff[64];
    MacToStr(buff, mac());

    if (port() != nullptr) {
        std::stringstream s;
        s << "bridge fdb del " << buff << " dev " << port()->port_name()
            << " master";
        //Execute(s.str());
    } else if (tunnel_dest().to_ulong() != 0) {
        const KSyncLinuxBridgeEntry *br =
            dynamic_cast<const KSyncLinuxBridgeEntry *>(bridge());

        std::stringstream s;
        s << "bridge fdb del " << buff << " dev " << br->vxlan_port_name();
        Execute(s.str());
    }

    return true;
}

/****************************************************************************
 * Implementation of KSyncLinuxFdbObject
 ****************************************************************************/
KSyncLinuxFdbObject::KSyncLinuxFdbObject(KSyncLinuxVrfObject *vrf,
                                         AgentRouteTable *rt_table) :
    KSyncVxlanRouteObject(vrf, rt_table) {
}

KSyncLinuxFdbObject::~KSyncLinuxFdbObject() {
}

KSyncEntry *KSyncLinuxFdbObject::Alloc(const KSyncEntry *entry,
                                       uint32_t index) {
    const KSyncLinuxFdbEntry *fdb =
        static_cast<const KSyncLinuxFdbEntry *>(entry);
    return new KSyncLinuxFdbEntry(this, fdb);
}

KSyncEntry *KSyncLinuxFdbObject::DBToKSyncEntry(const DBEntry *e) {
    const BridgeRouteEntry *fdb = static_cast<const BridgeRouteEntry *>(e);
    return new KSyncLinuxFdbEntry(this, fdb);
}

/****************************************************************************
 * Implementation of KSyncLinuxVrfObject
 ****************************************************************************/
KSyncLinuxVrfObject::KSyncLinuxVrfObject(KSyncLinuxVxlan *ksync) :
    KSyncVxlanVrfObject(ksync) {
}

KSyncLinuxVrfObject::~KSyncLinuxVrfObject() {
}

KSyncVxlanRouteObject *KSyncLinuxVrfObject::AllocBridgeRouteTable
(const VrfEntry *vrf) {
    return new KSyncLinuxFdbObject(this, vrf->GetBridgeRouteTable());
}
