/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __AGENT_ACL_N_H__
#define __AGENT_ACL_N_H__

#include <boost/intrusive/list.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/intrusive_ptr.hpp>
#include <tbb/atomic.h>

#include <oper/oper_db.h>
#include <filter/traffic_action.h>
#include <filter/acl_entry_match.h>
#include <filter/acl_entry_spec.h>
#include <filter/acl_entry.h>
#include <filter/packet_header.h>

struct FlowKey;
class VnEntry;
class Interface;

struct FlowPolicyInfo {
    std::string uuid;
    bool drop;
    bool terminal;
    bool other;
    std::string src_match_vn;  // source VN that matched
    std::string dst_match_vn;  // destination VN that matched
    std::string acl_name;
    FlowPolicyInfo(const std::string &u);
};

struct FlowAction {
    FlowAction():
        action(0), mirror_l() {};
    ~FlowAction() { };

    void Clear() {
        action = 0;
        mirror_l.clear();
        qos_config_action_.clear();
    };

    uint32_t action;
    std::vector<MirrorActionSpec> mirror_l;
    VrfTranslateActionSpec vrf_translate_action_;
    QosConfigActionSpec qos_config_action_;
};

struct MatchAclParams {
    MatchAclParams(): acl(NULL), ace_id_list(), terminal_rule(false) {}
    ~MatchAclParams() { };

    AclDBEntryConstRef acl;
    AclEntryIDList ace_id_list;
    FlowAction action_info;
    bool terminal_rule;
};

struct AclKey : public AgentOperDBKey {
    AclKey(const boost::uuids::uuid &id) : AgentOperDBKey(), uuid_(id) {} ;
    virtual ~AclKey() {};

    boost::uuids::uuid uuid_;
};

struct AclData: public AgentOperDBData {
    AclData(Agent *agent, IFMapNode *node, AclSpec &aclspec) :
        AgentOperDBData(agent, node), ace_id_to_del_(0), ace_add(false),
        acl_spec_(aclspec) {
    }
    AclData(Agent *agent, IFMapNode *node, int ace_id_to_del) :
        AgentOperDBData(agent, node), ace_id_to_del_(ace_id_to_del) {
    }
    virtual ~AclData() { }

    // Delete a particular ace
    int ace_id_to_del_;
    // true: add to existing aces, false:replace existing aces with specified in the spec
    bool ace_add;
    std::string cfg_name_;
    AclSpec acl_spec_;
};

struct AclResyncQosConfigData : public AgentOperDBData {
    AclResyncQosConfigData(Agent *agent, IFMapNode *node) :
        AgentOperDBData(agent, node) {}
};

class AclDBEntry : AgentRefCount<AclDBEntry>, public AgentOperDBEntry {
public:
    typedef boost::intrusive::member_hook<AclEntry,
            boost::intrusive::list_member_hook<>,
            &AclEntry::acl_list_node> AclEntryNode;
    typedef boost::intrusive::list<AclEntry, AclEntryNode> AclEntries;

    AclDBEntry(const boost::uuids::uuid &id) :
        AgentOperDBEntry(), uuid_(id), dynamic_acl_(false) {
    }
    ~AclDBEntry() {
    }

    bool IsLess(const DBEntry &rhs) const;
    KeyPtr GetDBRequestKey() const;
    void SetKey(const DBRequestKey *key);
    std::string ToString() const;
    uint32_t GetRefCount() const {
        return AgentRefCount<AclDBEntry>::GetRefCount();
    }
    const boost::uuids::uuid &GetUuid() const {return uuid_;};
    const std::string &GetName() const {return name_;};
    void SetName(const std::string name) {name_ = name;};
    bool DBEntrySandesh(Sandesh *resp, std::string &name) const;
    void SetAclSandeshData(AclSandeshData &data) const;

    // ACL methods
    //AclEntry *AddAclEntry(const AclEntrySpec &acl_entry_spec);
    AclEntry *AddAclEntry(const AclEntrySpec &acl_entry_spec, AclEntries &entries);
    bool DeleteAclEntry(const uint32_t acl_entry_id);
    void DeleteAllAclEntries();
    uint32_t Size() const {return acl_entries_.size();};
    void SetAclEntries(AclEntries &entries);
    void SetDynamicAcl(bool dyn) {dynamic_acl_ = dyn;};
    bool GetDynamicAcl () const {return dynamic_acl_;};

    // Packet Match
    bool PacketMatch(const PacketHeader &packet_header, MatchAclParams &m_acl,
                     FlowPolicyInfo *info) const;
    bool Changed(const AclEntries &new_acl_entries) const;
    uint32_t ace_count() const { return acl_entries_.size();}
    bool IsRulePresent(const std::string &uuid) const;
    bool ResyncQosConfigEntries();
    bool IsQosConfigResolved();
    bool Isresolved();
    const AclEntry* GetAclEntryAtIndex(uint32_t) const;
private:
    friend class AclTable;
    boost::uuids::uuid uuid_;
    bool dynamic_acl_;
    std::string name_;
    AclEntries acl_entries_;
    DISALLOW_COPY_AND_ASSIGN(AclDBEntry);
};

class AclTable : public AgentOperDBTable {
public:
    typedef std::map<std::string, TrafficAction::Action> TrafficActionMap;
    typedef std::set<AclDBEntry*> UnResolvedAclEntries;

    // Packet module is optional. Callback function to update the flow stats
    // for ACL. The callback is defined to avoid linking error
    // when flow is not enabled
    typedef boost::function<void(const AclDBEntry *acl, AclFlowCountResp &data,
                                 const std::string &ace_id)> FlowAceSandeshDataFn;
    typedef boost::function<void(const AclDBEntry *acl, AclFlowResp &data,
                                 const int last_count)> FlowAclSandeshDataFn;

    AclTable(DB *db, const std::string &name) : AgentOperDBTable(db, name) { }
    virtual ~AclTable() { }
    void GetTables(DB *db) { };

    virtual std::unique_ptr<DBEntry> AllocEntry(const DBRequestKey *k) const;
    virtual size_t Hash(const DBEntry *entry) const {return 0;};
    virtual size_t Hash(const DBRequestKey *key) const {return 0;};

    virtual DBEntry *OperDBAdd(const DBRequest *req);
    virtual bool OperDBOnChange(DBEntry *entry, const DBRequest *req);
    virtual bool OperDBDelete(DBEntry *entry, const DBRequest *req);
    virtual bool OperDBResync(DBEntry *entry, const DBRequest *req);

    virtual bool IFNodeToReq(IFMapNode *node, DBRequest &req,
            const boost::uuids::uuid &u);
    void FirewallPolicyIFNodeToReq(IFMapNode *node, DBRequest &req,
                                   const boost::uuids::uuid &u,
                                   AclSpec &acl_spec);
    void AclIFNodeToReq(IFMapNode *node, DBRequest &req,
                        const boost::uuids::uuid &u,
                        AclSpec &acl_spec);
    virtual bool IFNodeToUuid(IFMapNode *node, boost::uuids::uuid &u);
    virtual AgentSandeshPtr GetAgentSandesh(const AgentSandeshArguments *args,
                                            const std::string &context);

    static DBTableBase *CreateTable(DB *db, const std::string &name);
    TrafficAction::Action ConvertActionString(std::string action) const;
    static void AclFlowResponse(const std::string acl_uuid_str,
                                const std::string ctx, const int last_count);
    static void AclFlowCountResponse(const std::string acl_uuid_str,
                                     const std::string ctx,
                                     const std::string &ace_id);
    void set_ace_flow_sandesh_data_cb(FlowAceSandeshDataFn fn);
    void set_acl_flow_sandesh_data_cb(FlowAclSandeshDataFn fn);
    void ListenerInit();
    void Notify(DBTablePartBase *partition, DBEntryBase *e);
    void AddUnresolvedEntry(AclDBEntry *entry);
    void DeleteUnresolvedEntry(AclDBEntry *entry);
private:
    bool SubnetTypeEqual(const autogen::SubnetType &lhs,
                         const autogen::SubnetType &rhs) const;
    bool AddressTypeEqual(const autogen::AddressType &lhs,
                          const autogen::AddressType &rhs) const;
    bool PortTypeEqual(const autogen::PortType &src,
                       const autogen::PortType &dst) const;
    static const AclDBEntry* GetAclDBEntry(const std::string uuid_str,
                                           const std::string ctx,
                                           SandeshResponse *resp);
    void AddImplicitRule(AclSpec &acl_spec, AclEntrySpec &ace_spec,
                         const autogen::FirewallRule *rule);
    void PopulateServicePort(AclEntrySpec &ace_spec, IFMapNode *node);
    IFMapNode *GetFirewallRule(IFMapNode *node);
    void ActionInit();
    DBTableBase::ListenerId qos_config_listener_id_;
    UnResolvedAclEntries unresolved_acl_entries_;
    TrafficActionMap ta_map_;
    FlowAceSandeshDataFn flow_ace_sandesh_data_cb_;
    FlowAclSandeshDataFn flow_acl_sandesh_data_cb_;
    DISALLOW_COPY_AND_ASSIGN(AclTable);
};

extern SandeshTraceBufferPtr AclTraceBuf;

#define ACL_TRACE(obj, ...)\
do {\
    Acl##obj::TraceMsg(AclTraceBuf, __FILE__, __LINE__, ##__VA_ARGS__);\
} while (false)

#endif
