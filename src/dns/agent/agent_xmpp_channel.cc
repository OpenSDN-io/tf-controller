/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "base/logging.h"
#include "base/task_trigger.h"
#include <pugixml/pugixml.hpp>
#include "agent/agent_xmpp_channel.h"

#include "xml/xml_pugi.h"
#include "xmpp/xmpp_connection.h"
#include "xmpp/xmpp_server.h"
#include "cmn/dns.h"
#include "mgr/dns_mgr.h"
#include "mgr/dns_oper.h"
#include "bind/bind_util.h"
#include "bind/bind_resolver.h"
#include "bind/named_config.h"
#include "bind/xmpp_dns_agent.h"
#include "sandesh/sandesh_trace.h"
#include "cmn/dns_types.h"

DnsAgentXmppChannel:: DnsAgentXmppChannel(XmppChannel *channel,
                                          DnsAgentXmppChannelManager *mgr)
                                        : channel_(channel), mgr_(mgr) {
    channel_->RegisterReceive(xmps::DNS,
                     boost::bind(&DnsAgentXmppChannel::ReceiveReq, this, _1));

}

DnsAgentXmppChannel::~DnsAgentXmppChannel() {
    channel_->UnRegisterWriteReady(xmps::DNS);
    channel_->UnRegisterReceive(xmps::DNS);
}

void DnsAgentXmppChannel::Close() {
    UpdateDnsRecords(BindUtil::DELETE_UPDATE);
    if (mgr_)
        mgr_->RemoveChannel(channel_);
}

void DnsAgentXmppChannel::ReceiveReq(const XmppStanza::XmppMessage *msg) {
    if (msg && msg->type == XmppStanza::IQ_STANZA) {
        XmlBase *impl = msg->dom.get();
        XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl);
        pugi::xml_node node = pugi->FindNode("dns");
        DnsAgentXmpp::XmppType type;
        std::string source_name = msg->from;
        uint32_t xid;
        uint16_t code;
        std::unique_ptr<DnsUpdateData> rcv_data(new DnsUpdateData());
        DnsUpdateData *data = rcv_data.get();
        if (DnsAgentXmpp::DnsAgentXmppDecode(node, type, xid, code, data, source_name)) {
            if (type == DnsAgentXmpp::Update) {
                HandleAgentUpdate(std::move(rcv_data));
            }
        }
    }
}

void DnsAgentXmppChannel::HandleAgentUpdate(
    std::unique_ptr<DnsUpdateData> rcv_data) {
    DnsUpdateData *data = rcv_data.get();
    if (data->virtual_dns.empty() || data->zone.empty()) {
        // if key is not present, ignore the update
        return;
    }
    DataSet::iterator it = update_data_.find(data);
    if (it != update_data_.end()) {
        DnsUpdateData *xmpp_data = *it;
        for (DnsItems::iterator iter = data->items.begin();
             iter != data->items.end();) {
            bool change;
            BindUtil::Operation op = BindUtil::ADD_UPDATE;
            if ((*iter).IsDelete()) {
                change = xmpp_data->DelItem(*iter);
                op = BindUtil::DELETE_UPDATE;
            } else {
                change = xmpp_data->AddItem(*iter, true);
                if (!change) {
                    op = BindUtil::CHANGE_UPDATE;
                    change = true;
                }
            }

            if (!change) {
                data->items.erase(iter++);
            } else {
                boost::shared_ptr<DnsAgentXmppChannelManager::RecordRequest>
                    req(new DnsAgentXmppChannelManager::RecordRequest(
                            op, GetDnsRecordName(data->virtual_dns, *iter),
                            data->virtual_dns, *iter));
                mgr_->EnqueueRecord(req);
                ++iter;
            }
        }
        if (!xmpp_data->items.size()) {
            update_data_.erase(xmpp_data);
            delete xmpp_data;
        }
    } else {
        for (DnsItems::iterator iter = data->items.begin();
             iter != data->items.end();) {
            if ((*iter).IsDelete())
                data->items.erase(iter++);
            else {
                boost::shared_ptr<DnsAgentXmppChannelManager::RecordRequest>
                    req(new DnsAgentXmppChannelManager::RecordRequest(
                            BindUtil::ADD_UPDATE,
                            GetDnsRecordName(data->virtual_dns, *iter),
                            data->virtual_dns, *iter));
                mgr_->EnqueueRecord(req);
                ++iter;
            }
        }
        update_data_.insert(data);
        rcv_data.release();
    }
}

void DnsAgentXmppChannel::UpdateDnsRecords(BindUtil::Operation op) {
    for (DataSet::iterator it = update_data_.begin();
         it != update_data_.end(); ++it) {
        for (DnsItems::const_iterator item = (*it)->items.begin();
             item != (*it)->items.end(); ++item) {
            boost::shared_ptr<DnsAgentXmppChannelManager::RecordRequest>
                req(new DnsAgentXmppChannelManager::RecordRequest(
                        op, GetDnsRecordName((*it)->virtual_dns, *item),
                        (*it)->virtual_dns, *item));
            mgr_->EnqueueRecord(req);
        }
    }
}

std::string DnsAgentXmppChannel::GetDnsRecordName(std::string &vdns_name,
                                                  const DnsItem &item) {
    std::stringstream str;
    str << vdns_name << ":" << item.type << ":" << item.name << ":" << item.data;
    return str.str();
}

void DnsAgentXmppChannel::GetAgentDnsData(AgentDnsData &data) {
    data.set_agent(channel_->connection()->endpoint().address().to_string());
    std::vector<AgentDnsDataItem> items;
    for (DataSet::iterator it = update_data_.begin();
         it != update_data_.end(); ++it) {
        AgentDnsDataItem item;
        item.set_virtual_dns((*it)->virtual_dns);
        item.set_zone((*it)->zone);
        std::vector<VirtualDnsRecordTraceData> records;
        for (DnsItems::iterator iter = (*it)->items.begin();
             iter != (*it)->items.end(); ++iter) {
            VirtualDnsRecordTraceData record;
            record.rec_name = (*iter).name;
            record.rec_type = BindUtil::DnsType((*iter).type);
            record.rec_class = "IN";
            record.rec_data = (*iter).data;
            record.rec_ttl = (*iter).ttl;
            record.source = "Agent";
            record.installed = "yes";
            record.rec_source_name = (*iter).source_name;
            records.push_back(record);
        }
        item.set_records(records);
        items.push_back(item);
    }
    data.set_agent_data(items);
}

DnsAgentXmppChannelManager::DnsAgentXmppChannelManager(
                            XmppServer *server)
    : server_(server),
      work_queue_(TaskScheduler::GetInstance()->GetTaskId("dns::Config"), 0,
                  boost::bind(&DnsAgentXmppChannelManager::ProcessRecord,
                              this, _1)) {
    if (server_) {
        server_->RegisterConnectionEvent(xmps::DNS,
        boost::bind(&DnsAgentXmppChannelManager::HandleXmppChannelEvent,
                    this, _1, _2));
    }
}

DnsAgentXmppChannelManager::~DnsAgentXmppChannelManager() {
    if (server_)
        server_->UnRegisterConnectionEvent(xmps::DNS);
    channel_map_.clear();
    work_queue_.Shutdown();
}

bool DnsAgentXmppChannelManager::ProcessRecord(
                                 boost::shared_ptr<RecordRequest> req) {
    Dns::GetDnsManager()->ProcessAgentUpdate(req->op, req->record_name,
                                             req->vdns_name, req->item);
    return true;
}

void DnsAgentXmppChannelManager::EnqueueRecord(
                                 boost::shared_ptr<RecordRequest> req) {
    work_queue_.Enqueue(req);
}

void DnsAgentXmppChannelManager::RemoveChannel(XmppChannel *ch) {
    channel_map_.erase(ch);
}

DnsAgentXmppChannel *
DnsAgentXmppChannelManager::DnsAgentXmppChannelManager::FindChannel(
    const XmppChannel *ch) {
    tbb::mutex::scoped_lock lock(mutex_);
    ChannelMap::iterator it = channel_map_.find(ch);
    if (it == channel_map_.end())
        return NULL;
    return it->second;
}

void
DnsAgentXmppChannelManager::HandleXmppChannelEvent(XmppChannel *channel,
                                                   xmps::PeerState state) {
    tbb::mutex::scoped_lock lock(mutex_);
    ChannelMap::iterator it = channel_map_.find(channel);

    if (state == xmps::READY) {
        if (it == channel_map_.end()) {
            DnsAgentXmppChannel *agent_xmpp_channel =
                            new DnsAgentXmppChannel(channel, this);
            channel_map_.insert(std::make_pair(channel, agent_xmpp_channel));
            ConfigClientManager *config_manager =
                Dns::GetDnsManager()->get_config_manager();
            if (config_manager && !config_manager->GetEndOfRibComputed()) {
                DNS_XMPP_TRACE(DnsXmppTrace, "Peer:" + channel->PeerAddress() +
                               " Close due to EndofRib not computed");
                channel->Close();
            }
        }
    } else if (state == xmps::NOT_READY) {
        if (it != channel_map_.end()) {
            DnsAgentXmppChannel *agent_xmpp_channel = (*it).second;
            agent_xmpp_channel->Close();
            delete agent_xmpp_channel;
        } else {
            DNS_XMPP_TRACE(DnsXmppTrace,
                           "Peer not found on channel not ready event");
        }
    }
}

uint8_t DnsAgentXmppChannelManager::ChannelToDscp(const XmppChannel *xc) const {
    const XmppConnection *conn = xc->connection();
    if (conn) {
        const XmppSession *sess = conn->session();
        if (sess) {
            return sess->GetDscpValue();
        }
    }
    return 0;
}

void DnsAgentXmppChannelManager::GetAgentData(std::vector<AgentData> &list) {
    tbb::mutex::scoped_lock lock(mutex_);
    for (ChannelMap::iterator iter = channel_map_.begin();
         iter != channel_map_.end(); ++iter) {
        const XmppChannel *channel = iter->first;
        AgentData agent_data;
        agent_data.set_peer(channel->ToString());
        agent_data.set_peer_address(
            channel->connection()->endpoint().address().to_string());
        agent_data.set_local_address(
            channel->connection()->local_endpoint().address().to_string());
        agent_data.set_state(channel->StateName());
        agent_data.set_last_event(channel->LastEvent());
        agent_data.set_last_state(channel->LastStateName());
        agent_data.set_last_state_at(channel->LastStateChangeAt());
        agent_data.set_dscp(ChannelToDscp(channel));
        list.push_back(agent_data);
    }
}

void DnsAgentXmppChannelManager::GetAgentDnsData(std::vector<AgentDnsData> &dt) {
    tbb::mutex::scoped_lock lock(mutex_);
    for (ChannelMap::iterator iter = channel_map_.begin();
         iter != channel_map_.end(); ++iter) {
        DnsAgentXmppChannel *ch = iter->second;
        AgentDnsData data;
        ch->GetAgentDnsData(data);
        dt.push_back(data);
    }
}

void ShowAgentList::HandleRequest() const {
    DnsAgentListResponse *resp = new DnsAgentListResponse();
    resp->set_context(context());

    std::vector<AgentData> agent_list_sandesh;
    Dns::GetAgentXmppChannelManager()->GetAgentData(agent_list_sandesh);

    resp->set_agent(agent_list_sandesh);
    resp->Response();
}

void ShowAgentXmppDnsData::HandleRequest() const {
    AgentXmppDnsDataResponse *resp = new AgentXmppDnsDataResponse();
    resp->set_context(context());

    std::vector<AgentDnsData> agent_data;
    Dns::GetAgentXmppChannelManager()->GetAgentDnsData(agent_data);

    resp->set_data(agent_data);
    resp->Response();
}
