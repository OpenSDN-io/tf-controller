/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __AGENT_ACL_ENTRY_SPEC_H__
#define __AGENT_ACL_ENTRY_SPEC_H__

#include <vector>
#include <boost/uuid/uuid.hpp>

#include <base/address.h>
#include <cmn/agent_cmn.h>
#include <cmn/agent.h>
#include <vnc_cfg_types.h>
#include <filter/traffic_action.h>
#include <filter/acl_entry.h>
#include <filter/acl_entry_match.h>
#include <vnc_cfg_types.h>

struct RangeSpec {
    uint16_t min;
    uint16_t max;
};

struct StaticMirrorNhData {
    IpAddress vtep_dst_ip;
    uint32_t  vni;
    MacAddress vtep_dst_mac;
};

struct MirrorActionSpec {
    MirrorActionSpec() : analyzer_name(""), vrf_name(""), encap(""),
    juniper_header(true), nh_mode(""), nic_assisted_mirroring(false) { }
    bool operator == (const MirrorActionSpec &rhs) const {
        return analyzer_name == rhs.analyzer_name;
    }
    std::string analyzer_name;
    std::string vrf_name;
    IpAddress ip;
    MacAddress mac;
    uint16_t port;
    std::string encap;
    bool juniper_header;
    std::string nh_mode;
    StaticMirrorNhData staticnhdata;
    bool nic_assisted_mirroring;
    uint16_t nic_assisted_mirroring_vlan;
};

struct VrfTranslateActionSpec {
    VrfTranslateActionSpec() : vrf_name_(""), ignore_acl_(false) { }
    VrfTranslateActionSpec(std::string vrf_name, bool ignore_acl):
        vrf_name_(vrf_name), ignore_acl_(ignore_acl) { }
    const std::string& vrf_name() const { return vrf_name_;}
    bool ignore_acl() const { return ignore_acl_;}
    void set_vrf_name(const std::string &vrf_name) {
        vrf_name_ = vrf_name;
    }
    void set_ignore_acl(bool ignore_acl) {
        ignore_acl_ = ignore_acl;
    }
    std::string vrf_name_;
    bool ignore_acl_;
};

struct QosConfigActionSpec {
    QosConfigActionSpec() : name_(""), id_(-1) {}
    QosConfigActionSpec(const std::string &qos_config_name):
        name_(qos_config_name), id_(-1) {}
    void set_name(const std::string &name) {
        name_ = name;
    }
    const std::string& name() const {
        return name_;
    }

    void set_id(uint32_t id) {
        id_ = id;
    }

    uint32_t id() const {
        return id_;
    }

    void clear() {
        name_ = "";
        id_ = -1;
    }
    std::string name_;
    uint32_t id_;
};

struct ActionSpec {
    /* For actions log, alert and host_based_service we don't have any specific field.
     * Only ta_type of
     * TrafficAction::LOG_ACTION
     * TrafficAction::ALERT_ACTION and
     * TrafficAction::HBF_ACTION is enough
     */
    TrafficAction::Action simple_action;
    TrafficAction::TrafficActionType ta_type;
    MirrorActionSpec ma;
    VrfTranslateActionSpec vrf_translate;
    QosConfigActionSpec qos_config_action;
    ActionSpec() {}
    ActionSpec(TrafficAction::TrafficActionType type) : ta_type(type) {}
};

typedef enum AclTypeSpec {
    NOT_USED = 0,
    TERM = 1,
    NON_TERM = 2,
} AclTypeSpecT;

class AclEntrySpec {
public:
    //XXX Any field addition update Reverse API also
    //so that bidirectionaly ACL spec can be update
    AclEntrySpec(): type(NOT_USED), id(0), src_addr_type(AddressMatch::UNKNOWN_TYPE),
        dst_addr_type(AddressMatch::UNKNOWN_TYPE), terminal(true), family(Address::UNSPEC) { }
    //AclEntrySpec(const AclEntrySpec &rhs);
    typedef boost::uuids::uuid uuid;
    AclTypeSpecT type;
    AclEntryID id;

    // Address
    AddressMatch::AddressType src_addr_type;
    std::vector<AclAddressInfo> src_ip_list;
    uuid src_policy_id;
    std::string src_policy_id_str;
    int src_sg_id;

    AddressMatch::AddressType dst_addr_type;
    std::vector<AclAddressInfo> dst_ip_list;
    uuid dst_policy_id;
    std::string dst_policy_id_str;
    int dst_sg_id;

    // Protocol
    std::vector<RangeSpec> protocol;

    // Source port range
    std::vector<RangeSpec> src_port;

    // Destination port range
    std::vector<RangeSpec> dst_port;

    bool terminal;
    TagList src_tags;
    TagList dst_tags;

    ServiceGroupMatch::ServicePortList service_group;
    TagList match_tags;

    // Action
    std::vector<ActionSpec> action_l;

    // AddressFamily based on EtherType
    Address::Family family;

    //XXX Any field addition update Reverse API also
    //so that bidirectionaly ACL spec can be update

    // Rule-UUID
    std::string rule_uuid;
    bool Populate(const autogen::MatchConditionType *match_condition);
    bool Populate(Agent *agent, IFMapNode *node,
                  const autogen::FirewallRule *fw_rule);
    bool PopulateServiceGroup(const autogen::ServiceGroup *service_group);
    void PopulateAction(const AclTable *acl_table,
                        const autogen::ActionListType &action_list);
    void AddMirrorEntry(Agent *agent) const;
    void BuildAddressInfo(const std::string &prefix, int plen,
                          std::vector<AclAddressInfo> *list);
    void Reverse(AclEntrySpec *ace_spec, AclEntryID::Type type,
                 bool swap_address, bool swap_port);
    void ReverseAddress(AclEntrySpec *ace_spec);
    void ReversePort(AclEntrySpec *ace_spec);
    bool BuildAddressGroup(Agent *agent, IFMapNode *node,
                           const std::string &name, bool source);
    IFMapNode* GetAddressGroup(Agent *agent, IFMapNode *node,
                              const std::string &name);
    void PopulateServiceType(const autogen::FirewallServiceType *fst);
};

struct AclSpec {
  AclSpec() : dynamic_acl(false) { };
    typedef boost::uuids::uuid uuid;
    uuid acl_id;
    // Dynamic
    bool dynamic_acl;
    std::vector<AclEntrySpec> acl_entry_specs_;
};

#endif
