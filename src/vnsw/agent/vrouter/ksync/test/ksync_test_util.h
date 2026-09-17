/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_ksync_test_util_h
#define vnsw_agent_ksync_test_util_h

#include <unistd.h>
#include <memory>
#include <string>

#include "db/db.h"
#include "db/db_table.h"
#include "db/db_entry.h"
#include "db/db_partition.h"

#include "base/util.h"

#include "ksync/ksync_index.h"
#include "ksync/ksync_entry.h"
#include "ksync/ksync_object.h"
#include "ksync/ksync_netlink.h"
#include "ksync/ksync_sock.h"

#include "vr_types.h"

template <typename Cond>
bool WaitFor(int max_ms, Cond cond) {
    for (int i = 0; i < max_ms; i += 10) {
        if (cond()) {
            return true;
        }
        usleep(10 * 1000);
    }
    return cond();
}

class UTSandeshContext : public AgentSandeshContext {
public:
    UTSandeshContext() : AgentSandeshContext(), response_code_(0) {}
    virtual ~UTSandeshContext() {}

    int response_code() const { return response_code_; }

    virtual int VrResponseMsgHandler(vr_response *resp);

    virtual void IfMsgHandler(vr_interface_req *) {}
    virtual void NHMsgHandler(vr_nexthop_req *) {}
    virtual void RouteMsgHandler(vr_route_req *) {}
    virtual void MplsMsgHandler(vr_mpls_req *) {}
    virtual void MirrorMsgHandler(vr_mirror_req *) {}
    virtual void FlowMsgHandler(vr_flow_req *) {}
    virtual void VrfAssignMsgHandler(vr_vrf_assign_req *) {}
    virtual void VrfMsgHandler(vr_vrf_req *) {}
    virtual void VrfStatsMsgHandler(vr_vrf_stats_req *) {}
    virtual void DropStatsMsgHandler(vr_drop_stats_req *) {}
    virtual void VxLanMsgHandler(vr_vxlan_req *) {}
    virtual void VrouterOpsMsgHandler(vrouter_ops *) {}
    virtual void QosConfigMsgHandler(vr_qos_map_req *) {}
    virtual void ForwardingClassMsgHandler(vr_fc_map_req *) {}

private:
    int response_code_;
};

int KSyncTestEncodeIf(uint16_t idx, sandesh_op::type op, char *buf, int len,
                      int *stamped_gen = nullptr);

class VlanTable;

class Vlan : public DBEntry {
public:
    struct VlanKey : public DBRequestKey {
        VlanKey(uint16_t tag) : DBRequestKey(), tag_(tag) {}
        virtual ~VlanKey() {}
        uint16_t tag_;
    };

    Vlan(uint16_t tag) : DBEntry(), tag_(tag) {}
    virtual ~Vlan() {}

    bool IsLess(const DBEntry &rhs) const {
        return tag_ < static_cast<const Vlan &>(rhs).tag_;
    }
    virtual std::string ToString() const { return "Vlan"; }
    virtual void SetKey(const DBRequestKey *k) {
        tag_ = static_cast<const VlanKey *>(k)->tag_;
    }
    virtual KeyPtr GetDBRequestKey() const { return KeyPtr(new VlanKey(tag_)); }
    uint16_t GetTag() const { return tag_; }

private:
    uint16_t tag_;
    friend class VlanTable;
    DISALLOW_COPY_AND_ASSIGN(Vlan);
};

class VlanTable : public DBTable {
public:
    VlanTable(DB *db, const std::string &name) : DBTable(db, name) {}
    virtual ~VlanTable() {}

    virtual std::unique_ptr<DBEntry> AllocEntry(const DBRequestKey *k) const {
        const Vlan::VlanKey *key = static_cast<const Vlan::VlanKey *>(k);
        return std::unique_ptr<DBEntry>(new Vlan(key->tag_));
    }
    virtual size_t Hash(const DBEntry *entry) const { return 0; }
    virtual size_t Hash(const DBRequestKey *key) const { return 0; }
    virtual DBEntry *Add(const DBRequest *req);
    virtual bool OnChange(DBEntry *entry, const DBRequest *req) { return true; }
    virtual bool Delete(DBEntry *entry, const DBRequest *req) { return true; }

    static DBTableBase *CreateTable(DB *db, const std::string &name);

private:
    DISALLOW_COPY_AND_ASSIGN(VlanTable);
};

class VlanKSyncObject;

class VlanKSyncEntry : public KSyncNetlinkDBEntry {
public:
    explicit VlanKSyncEntry(const VlanKSyncEntry *e)
        : KSyncNetlinkDBEntry(), tag_(e->tag_) {}
    explicit VlanKSyncEntry(const Vlan *v)
        : KSyncNetlinkDBEntry(), tag_(v->GetTag()) {}
    virtual ~VlanKSyncEntry() {}

    virtual bool IsLess(const KSyncEntry &rhs) const {
        return tag_ < static_cast<const VlanKSyncEntry &>(rhs).tag_;
    }
    virtual std::string ToString() const { return "VlanKSync"; }
    virtual KSyncEntry *UnresolvedReference() { return nullptr; }
    virtual bool Sync(DBEntry *e) { return true; }
    virtual int MsgLen() { return KSYNC_DEFAULT_MSG_SIZE; }

    virtual int AddMsg(char *buf, int len) {
        add_count_++;
        return KSyncTestEncodeIf(tag_, sandesh_op::ADD, buf, len, &last_gen_);
    }
    virtual int ChangeMsg(char *buf, int len) {
        change_count_++;
        return KSyncTestEncodeIf(tag_, sandesh_op::ADD, buf, len, &last_gen_);
    }
    virtual int DeleteMsg(char *buf, int len) {
        del_count_++;
        return KSyncTestEncodeIf(tag_, sandesh_op::DEL, buf, len, nullptr);
    }

    KSyncDBObject *GetObject() const;
    uint16_t GetTag() const { return tag_; }

    static void Reset() {
        add_count_ = change_count_ = del_count_ = 0;
        last_gen_ = 0;
    }
    static int AddCount()    { return add_count_; }
    static int ChangeCount() { return change_count_; }
    static int DelCount()    { return del_count_; }
    static int LastGen()     { return last_gen_; }

private:
    uint16_t tag_;
    static int add_count_, change_count_, del_count_, last_gen_;
    DISALLOW_COPY_AND_ASSIGN(VlanKSyncEntry);
};

class VlanKSyncObject : public KSyncDBObject {
public:
    explicit VlanKSyncObject(DBTableBase *t) : KSyncDBObject("Vlan KSync", t) {}
    virtual ~VlanKSyncObject() {}

    virtual KSyncEntry *Alloc(const KSyncEntry *e, uint32_t index) {
        VlanKSyncEntry *k =
            new VlanKSyncEntry(static_cast<const VlanKSyncEntry *>(e));
        last_ = k;
        return static_cast<KSyncEntry *>(k);
    }
    virtual KSyncEntry *DBToKSyncEntry(const DBEntry *e) {
        return static_cast<KSyncEntry *>(
            new VlanKSyncEntry(static_cast<const Vlan *>(e)));
    }

    static void Init(VlanTable *t);
    static void Shutdown();
    static VlanKSyncObject *Get() { return singleton_; }
    static VlanKSyncEntry *last() { return last_; }

private:
    static VlanKSyncObject *singleton_;
    static VlanKSyncEntry *last_;
    DISALLOW_COPY_AND_ASSIGN(VlanKSyncObject);
};

void EnqueueVlan(VlanTable *table, uint16_t tag, DBRequest::DBOperation op);

#endif  // vnsw_agent_ksync_test_util_h
