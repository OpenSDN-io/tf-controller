/*
 * Copyright (c) 2024 Juniper Networks, Inc. All rights reserved.
 */

#include "ksync_test_util.h"

static int g_encode_gen = 1000;

int UTSandeshContext::VrResponseMsgHandler(vr_response *resp) {
    response_code_ = resp->get_resp_code();
    if (response_code_ < 0) {
        SetErrno(-response_code_);
        return -response_code_;
    }
    SetErrno(0);
    return 0;
}

int KSyncTestEncodeIf(uint16_t idx, sandesh_op::type op, char *buf, int len,
                      int *stamped_gen) {
    vr_interface_req encoder;
    encoder.set_h_op(op);
    encoder.set_vifr_idx(idx);
    encoder.set_vifr_type(0);
    int gen = ++g_encode_gen;
    encoder.set_vifr_mtu(gen);
    if (stamped_gen != nullptr) {
        *stamped_gen = gen;
    }
    int error = 0;
    int elen = encoder.WriteBinary((uint8_t *)buf, len, &error);
    assert(error == 0);
    assert(elen > 0 && elen <= len);
    return elen;
}

DBEntry *VlanTable::Add(const DBRequest *req) {
    const Vlan::VlanKey *key = static_cast<Vlan::VlanKey *>(req->key.get());
    return new Vlan(key->tag_);
}

DBTableBase *VlanTable::CreateTable(DB *db, const std::string &name) {
    VlanTable *table = new VlanTable(db, name);
    table->Init();
    return table;
}

int VlanKSyncEntry::add_count_ = 0;
int VlanKSyncEntry::change_count_ = 0;
int VlanKSyncEntry::del_count_ = 0;
int VlanKSyncEntry::last_gen_ = 0;

KSyncDBObject *VlanKSyncEntry::GetObject() const {
    return VlanKSyncObject::Get();
}

VlanKSyncObject *VlanKSyncObject::singleton_ = nullptr;
VlanKSyncEntry *VlanKSyncObject::last_ = nullptr;

void VlanKSyncObject::Init(VlanTable *t) {
    assert(singleton_ == nullptr);
    singleton_ = new VlanKSyncObject(t);
}

void VlanKSyncObject::Shutdown() {
    delete singleton_;
    singleton_ = nullptr;
    last_ = nullptr;
}

void EnqueueVlan(VlanTable *table, uint16_t tag, DBRequest::DBOperation op) {
    DBRequest req;
    req.oper = op;
    req.key.reset(new Vlan::VlanKey(tag));
    req.data.reset(nullptr);
    table->Enqueue(&req);
}
