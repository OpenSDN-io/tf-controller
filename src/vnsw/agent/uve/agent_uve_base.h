/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_uve_base_h
#define vnsw_agent_uve_base_h

#include "nodeinfo_types.h"
#include <base/connection_info.h>
#include <uve/vn_uve_table_base.h>
#include <uve/vm_uve_table_base.h>
#include <uve/vrouter_uve_entry_base.h>
#include <uve/prouter_uve_table.h>
#include <uve/interface_uve_table.h>
#include <boost/scoped_ptr.hpp>

class VrouterStatsCollector;

/* Structure used to build and carry tags of different types across APIs */
struct UveTagData {
    enum FillType {
        STRING,
        SET,
        VECTOR
    };
    std::string application;
    std::string tier;
    std::string site;
    std::string deployment;
    /* Semi-colon separated list of labels. This format is imposed by analytics
     * module as it expects labels in semi-colon separated string */
    std::string labels;
    std::set<string> label_set;
    std::vector<string> label_vector;
    /* Semi-colon separated list of custom-tags. This format is imposed by
     * analytics module as it expects custom-tags in semi-colon separated
     * string */
    std::string custom_tags;
    std::set<string> custom_tag_set;
    std::vector<string> custom_tag_vector;
    FillType fill_type;
    void Reset() {
        application = tier = site = deployment = labels = custom_tags = "";
        if (fill_type == SET) {
            label_set.clear();
            custom_tag_set.clear();
        } else if (fill_type == VECTOR) {
            label_vector.clear();
            custom_tag_vector.clear();
        }
    }
    UveTagData() : fill_type(STRING) { Reset(); }
    UveTagData(FillType type) : fill_type(type) { Reset(); }
};

//The class to drive UVE module initialization for agent.
//Defines objects required for statistics collection from vrouter and
//objects required for sending UVE information to collector.
class AgentUveBase {
public:
    /* Number of UVEs to be sent each time, the timer callback is Run */
    static const uint32_t kUveCountPerTimer = 64;
    /* The interval at which timer is fired */
    static const uint32_t kDefaultInterval = (30 * 1000); // time in millisecs
    /* When timer is Run, we send atmost 'kUveCountPerTimer' UVEs. If there
     * are more UVEs to be sent, we reschedule timer at the following shorter
     * interval*/
    static const uint32_t kIncrementalInterval = (1000); // time in millisecs

    static const uint64_t kBandwidthInterval = (1000000); // time in microseconds

    AgentUveBase(Agent *agent, uint64_t intvl,
                 uint32_t default_intvl, uint32_t incremental_intvl);
    virtual ~AgentUveBase();

    virtual void Shutdown();
    uint64_t bandwidth_intvl() const { return bandwidth_intvl_; }
    VnUveTableBase* vn_uve_table() const { return vn_uve_table_.get(); }
    VmUveTableBase* vm_uve_table() const { return vm_uve_table_.get(); }
    Agent* agent() const { return agent_; }
    VrouterUveEntryBase* vrouter_uve_entry() const {
        return vrouter_uve_entry_.get();
    }
    ProuterUveTable* prouter_uve_table() const {
        return prouter_uve_table_.get();
    }
    InterfaceUveTable* interface_uve_table() const {
        return interface_uve_table_.get();
    }
    VrouterStatsCollector *vrouter_stats_collector() const {
        return vrouter_stats_collector_.get();
    }

    void Init();
    virtual void InitDone();
    virtual void RegisterDBClients();
    static AgentUveBase *GetInstance() {return singleton_;}
    uint8_t ExpectedConnections(uint8_t &num_c_nodes, uint8_t &num_d_servers);
    uint32_t default_interval() const { return default_interval_; }
    uint32_t incremental_interval() const { return incremental_interval_; }
    void BuildTagNamesFromList(const TagList &tl, UveTagData *info) const;
    void BuildTagIdsFromList(const TagList &tl, UveTagData *info) const;
protected:
    boost::scoped_ptr<VnUveTableBase> vn_uve_table_;
    boost::scoped_ptr<VmUveTableBase> vm_uve_table_;
    boost::scoped_ptr<VrouterUveEntryBase> vrouter_uve_entry_;
    boost::scoped_ptr<ProuterUveTable> prouter_uve_table_;
    boost::scoped_ptr<InterfaceUveTable> interface_uve_table_;
    uint32_t default_interval_;
    uint32_t incremental_interval_;
    static AgentUveBase *singleton_;

private:
    std::string IntegerToHexString(uint32_t value) const;
    friend class UveTest;
    void VrouterAgentProcessState(
        const std::vector<process::ConnectionInfo> &c,
        process::ProcessState::type &state, std::string &message);
    void UpdateMessage(const process::ConnectionInfo &info,
                       std::string &message);
    bool HasSelfConfiguration() const;

    Agent *agent_;
    uint64_t bandwidth_intvl_; //in microseconds
    process::ConnectionStateManager
        *connection_state_manager_;
    DISALLOW_COPY_AND_ASSIGN(AgentUveBase);
protected:
    boost::scoped_ptr<VrouterStatsCollector> vrouter_stats_collector_;
};

#endif //vnsw_agent_uve_base_h
