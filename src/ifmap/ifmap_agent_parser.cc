/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "ifmap/ifmap_agent_parser.h"

#include <vector>
#include <pugixml/pugixml.hpp>
#include "base/logging.h"
#include "db/db_entry.h"
#include "ifmap/ifmap_agent_table.h"

using namespace std;
using namespace pugi;

void IFMapAgentParser::NodeRegister(const string &node, NodeParseFn parser) {
    pair<NodeParseMap::iterator, bool> result =
            node_map_.insert(make_pair(node, parser));
    assert(result.second);
}

void IFMapAgentParser::NodeClear() {
    node_map_.clear();
}

void IFMapAgentParser::NodeParse(xml_node &node, DBRequest::DBOperation oper, uint64_t seq) {

    const char *name = node.attribute("type").value();
    int msg_type;
    if (oper == DBRequest::DB_ENTRY_ADD_CHANGE)
        msg_type = UPDATE;
    else
        msg_type = DEL;

    IFMapTable *table;
    table = IFMapTable::FindTable(db_, name);
    if(!table) {
        node_parse_errors_[msg_type]++;
        return;
    }

    // Locate the decode function using id_name
    NodeParseMap::const_iterator loc = node_map_.find(name);
    if (loc == node_map_.end()) {
        node_parse_errors_[msg_type]++;
        return;
    }

    IFMapObject *obj;
    IFMapTable::RequestKey *req_key = new IFMapTable::RequestKey;

    // Invoke the decode routine
    req_key->id_type = name;
    req_key->id_seq_num = seq;
    obj = loc->second(node, db_, &req_key->id_name);
    if (!obj) {
        node_parse_errors_[msg_type]++;
        delete req_key;
        return;
    }

    IFMapAgentTable::IFMapAgentData *req_data = new IFMapAgentTable::IFMapAgentData;
    req_data->content.reset(obj);

    unique_ptr<DBRequest> request(new DBRequest);
    request->oper = oper;
    request->data.reset(req_data);
    request->key.reset(req_key);
    table->Enqueue(request.get());
}

void IFMapAgentParser::LinkParse(xml_node &link, DBRequest::DBOperation oper, uint64_t seq) {

    xml_node first_node;
    xml_node second_node;
    xml_node name_node1;
    xml_node name_node2;
    const char *name1;
    const char *name2;
    IFMapTable *table;
    IFMapAgentLinkTable *link_table;

    int msg_type;
    if (oper == DBRequest::DB_ENTRY_ADD_CHANGE)
        msg_type = UPDATE;
    else
        msg_type = DEL;

    link_table = static_cast<IFMapAgentLinkTable *>(
        db_->FindTable(IFMAP_AGENT_LINK_DB_NAME));

    assert(link_table);

    // Get both first and second node and its corresponding tables
    first_node = link.first_child();
    if (!first_node) {
        link_parse_errors_[msg_type]++;
        return;
    }

    second_node = first_node.next_sibling();
    if (!second_node) {
        link_parse_errors_[msg_type]++;
        return;
    }

    name1 = first_node.attribute("type").value();
    table = IFMapTable::FindTable(db_, name1);
    if(!table) {
        link_parse_errors_[msg_type]++;
        return;
    }

    name2 = second_node.attribute("type").value();
    table = IFMapTable::FindTable(db_, name2);
    if(!table) {
        link_parse_errors_[msg_type]++;
        return;
    }

    // Get id_name of both the nodes
    name_node1 = first_node.first_child();
    if (!name_node1) {
        link_parse_errors_[msg_type]++;
        return;
    }

    if (strcmp(name_node1.name(), "name") != 0) {
        link_parse_errors_[msg_type]++;
        return;
    }

    name_node2 = second_node.first_child();
    if (!name_node2) {
        link_parse_errors_[msg_type]++;
        return;
    }

    if (strcmp(name_node2.name(), "name") != 0) {
        link_parse_errors_[msg_type]++;
        return;
    }

    // Create both the request keys
    unique_ptr <IFMapAgentLinkTable::RequestKey> req_key (new IFMapAgentLinkTable::RequestKey);
    req_key->left_key.id_name = name_node1.child_value();
    req_key->left_key.id_type = name1;
    req_key->left_key.id_seq_num = seq;

    req_key->right_key.id_name = name_node2.child_value();
    req_key->right_key.id_type = name2;
    req_key->right_key.id_seq_num = seq;

    xml_node metadata = link.child("metadata");
    if (metadata) {
        req_key->metadata = metadata.attribute("type").value();
    }

    unique_ptr <DBRequest> req (new DBRequest);
    req->oper = oper;
    req->key = std::move(req_key);

    link_table->Enqueue(req.get());
}

void IFMapAgentParser::ConfigParse(const xml_node config, const uint64_t seq) {

    DBRequest::DBOperation oper;

    for (xml_node node = config.first_child(); node;
         node = node.next_sibling()) {

        int msg_type;
        if (strcmp(node.name(), "update") == 0) {
            oper = DBRequest::DB_ENTRY_ADD_CHANGE;
            msg_type = UPDATE;
        } else if (strcmp(node.name(), "delete") == 0) {
            oper = DBRequest::DB_ENTRY_DELETE;
            msg_type = DEL;
        } else {
            continue;
        }

        for(xml_node chld = node.first_child(); chld; chld = chld.next_sibling()) {

            // Handle the links between the nodes
            if (strcmp(chld.name(), "link") == 0) {
                links_processed_[msg_type]++;
                LinkParse(chld, oper, seq);
                continue;
            }

            if (strcmp(chld.name(), "node") == 0) {
                nodes_processed_[msg_type]++;
                NodeParse(chld, oper, seq);
            }
        }
    }
}
