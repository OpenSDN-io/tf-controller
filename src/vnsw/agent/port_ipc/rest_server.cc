/*
 * Copyright (c) 2015 Juniper Networks, Inc. All rights reserved.
 */

#include <vector>
#include <string>

#include <boost/bind/bind.hpp>
#include <boost/assign.hpp>
#include <boost/foreach.hpp>

#include "base/regex.h"
#include "http/http_server.h"
#include "http/http_request.h"
#include "http/http_session.h"
#include "base/contrail_ports.h"
#include "port_ipc/port_ipc_handler.h"
#include "cmn/agent.h"
#include "rest_server.h"
#include "rest_common.h"
#include "init/agent_param.h"

using namespace boost::placeholders;

using contrail::regex;
using contrail::regex_match;
using contrail::regex_search;

class RestServerGetVmCfgTask : public Task {
public:
    RestServerGetVmCfgTask(PortIpcHandler *pih,
                            const struct RestServer::RESTData& data):
        Task((TaskScheduler::GetInstance()->GetTaskId("Agent::RestApi")), 0),
        pih_(pih),
        vm_name_((*data.match)[1]),
        data_(data),
        context_(data_.session->get_context()) {
    }
    virtual ~RestServerGetVmCfgTask() { }
    virtual bool Run() {
        std::string info;
        const std::string client_ctx = data_.session->get_client_context(context_);
        // check session is not deleted
        if ((!data_.session->set_client_context(context_, client_ctx)) ||
            (context_.empty()))
            return true;
        if (pih_->GetVmVnCfgPort(vm_name_, info)) {
            REST::SendResponse(data_.session, info, 200, context_);
        } else {
            REST::SendErrorResponse(data_.session, "{ Not Found }", 404, context_);
        }
        return true;
    }
    std::string Description() const { return "RestServerGetVmCfgTask"; }
private:
    const PortIpcHandler *pih_;
    std::string vm_name_;
    const struct RestServer::RESTData& data_;
    const std::string context_;
    DISALLOW_COPY_AND_ASSIGN(RestServerGetVmCfgTask);
};

void RestServer::VmPortPostHandler(const struct RESTData& data) {
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        std::string err_msg;
        if (pih->AddPortFromJson(data.request->Body(), false, err_msg, true)) {
            REST::SendResponse(data.session, "{}");
        } else {
            REST::SendErrorResponse(data.session, "{ " + err_msg + " }");
        }
    } else {
       REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmPortSyncHandler(const struct RESTData& data) {
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        pih->SyncHandler();
        REST::SendResponse(data.session, "{}");
    } else {
       REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmPortDeleteHandler(const struct RESTData& data) {
    std::string error;
    const std::string& port_id = (*data.match)[1];
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        if (pih->DeletePort(port_id, error)) {
            REST::SendResponse(data.session, "{}");
        } else {
            REST::SendErrorResponse(data.session, "{" + error + "}");
        }
    } else {
        REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmPortGetHandler(const struct RESTData& data) {
    const std::string& port_id = (*data.match)[1];
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        std::string info;
        if (pih->GetPortInfo(port_id, info)) {
            REST::SendResponse(data.session, info);
        } else {
            REST::SendErrorResponse(data.session, "{ Not Found }", 404);
        }
    } else {
        REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::GatewayPostHandler(const struct RESTData& data) {
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        std::string err_msg;
        if (pih->AddVgwFromJson(data.request->Body(), err_msg)) {
            REST::SendResponse(data.session, "{}");
        } else {
            REST::SendErrorResponse(data.session, "{ " + err_msg + " }");
        }
    } else {
       REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::GatewayDeleteHandler(const struct RESTData& data) {
    std::string error;
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        std::string err_msg;
        if (pih->DelVgwFromJson(data.request->Body(), err_msg)) {
            REST::SendResponse(data.session, "{}");
        } else {
            REST::SendErrorResponse(data.session, "{" + error + "}");
        }
    } else {
        REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmVnPortPostHandler(const struct RESTData& data) {
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        std::string err_msg;
        if (pih->AddVmVnPort(data.request->Body(), false, err_msg, true)) {
            REST::SendResponse(data.session, "{}");
        } else {
            REST::SendErrorResponse(data.session, "{ " + err_msg + " }");
        }
    } else {
       REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmVnPortDeleteHandler(const struct RESTData& data) {
    std::string error;
    const std::string &vm_uuid = (*data.match)[1];
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        if (pih->DeleteVmVnPort(data.request->Body(), vm_uuid, error)) {
            REST::SendResponse(data.session, "{}");
        } else {
            REST::SendErrorResponse(data.session, "{" + error + "}");
        }
    } else {
        REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmVnPortGetHandler(const struct RESTData& data) {
    std::string url = (*data.match)[0];
    bool vmi_uuid_presence = false;
    std::size_t pos = 4; // skip "/vm/"
    std::size_t vmi_uuid_pos = url.find("/", pos);
    if (vmi_uuid_pos != std::string::npos) {
        vmi_uuid_presence = true;
    }
    std::string vm_uuid = url.substr(pos, vmi_uuid_pos);
    std::string vmi_uuid = "";
    if (vmi_uuid_presence) {
        vmi_uuid = url.substr(vmi_uuid_pos+1, std::string::npos);
    }

    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        std::string info;
        if (pih->GetVmVnPort(vm_uuid, vmi_uuid, info)) {
            REST::SendResponse(data.session, info);
        } else {
            REST::SendErrorResponse(data.session, "{ Not Found }", 404);
        }
    } else {
        REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmVnPortCfgGetHandler(const struct RESTData& data) {
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        RestServerGetVmCfgTask *t =
            new RestServerGetVmCfgTask(pih, data);
        TaskScheduler *scheduler = TaskScheduler::GetInstance();
        scheduler->Enqueue(t);
    } else {
        REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmPortEnableHandler(const struct RESTData& data) {
    std::string error;
    const std::string& port_id = (*data.match)[1];
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        if (pih->EnablePort(port_id, error)) {
            REST::SendResponse(data.session, "{}");
        } else {
            REST::SendErrorResponse(data.session, "{" + error + "}");
        }
    } else {
        REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

void RestServer::VmPortDisableHandler(const struct RESTData& data) {
    std::string error;
    const std::string& port_id = (*data.match)[1];
    PortIpcHandler *pih = agent_->port_ipc_handler();
    if (pih) {
        if (pih->DisablePort(port_id, error)) {
            REST::SendResponse(data.session, "{}");
        } else {
            REST::SendErrorResponse(data.session, "{" + error + "}");
        }
    } else {
        REST::SendErrorResponse(data.session, "{ Operation Not Supported }");
    }
}

const std::vector<RestServer::HandlerSpecifier> RestServer::RESTHandlers_ =
    boost::assign::list_of
    (HandlerSpecifier(
        regex("/port"),
        HTTP_POST,
        &RestServer::VmPortPostHandler))
    (HandlerSpecifier(
        regex("/syncports"),
        HTTP_POST,
        &RestServer::VmPortSyncHandler))
    (HandlerSpecifier(
        regex("/port/"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"),
        HTTP_DELETE,
        &RestServer::VmPortDeleteHandler))
    (HandlerSpecifier(
        regex("/port/"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"),
        HTTP_GET,
        &RestServer::VmPortGetHandler))
    (HandlerSpecifier(
        regex("/gateway"),
        HTTP_POST,
        &RestServer::GatewayPostHandler))
    (HandlerSpecifier(
        regex("/gateway"),
        HTTP_DELETE,
        &RestServer::GatewayDeleteHandler))
    (HandlerSpecifier(
        regex("/vm"),
        HTTP_POST,
        &RestServer::VmVnPortPostHandler))
    (HandlerSpecifier(
        regex("/vm/"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"),
        HTTP_DELETE,
        &RestServer::VmVnPortDeleteHandler))
    (HandlerSpecifier(
        regex("/vm/"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"),
        HTTP_GET,
        &RestServer::VmVnPortGetHandler))
    (HandlerSpecifier(
        regex("/vm/"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})/"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"),
        HTTP_GET,
        &RestServer::VmVnPortGetHandler))
    (HandlerSpecifier(
        regex("/vm-cfg/(.*)"),
        HTTP_GET,
        &RestServer::VmVnPortCfgGetHandler))
    (HandlerSpecifier(
        regex("/enable-port/"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"),
        HTTP_PUT,
        &RestServer::VmPortEnableHandler))
    (HandlerSpecifier(
        regex("/disable-port/"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"),
        HTTP_PUT,
        &RestServer::VmPortDisableHandler));

RestServer::RestServer(Agent *agent)
    : agent_(agent), http_server_(new HttpServer(agent->event_manager())) {
    http_server_->RegisterHandler(HTTP_WILDCARD_ENTRY,
        boost::bind(&RestServer::HandleRequest, this, _1, _2));
}

void RestServer::InitDone() {
     if (agent_ && agent_->params() && agent_->params()->cat_is_agent_mocked())
         //only if RestServer is used via mocked agent
         http_server_->Initialize(agent_->params()->rest_port());
     else //otherwise , use the hardcoded value
         http_server_->Initialize(ContrailPorts::PortIpcVrouterAgentPort());
}

void RestServer::HandleRequest(HttpSession* session,
                               const HttpRequest* request) {
    std::string path = request->UrlPath();
    BOOST_FOREACH(const RestServer::HandlerSpecifier& hs, RESTHandlers_) {
        boost::smatch match;
        if (regex_match(path, match, hs.request_regex) &&
            request->GetMethod() == hs.method) {
            struct RESTData data = {
                &match,
                request->GetMethod(),
                session,
                request
            };
            (this->*hs.handler_func)(data);
            // Though IMHO it contradicts most programmers' intuition,
            // currently handlers are responsible for freeing handled
            // requests.
            delete request;
            return;
        }
    }
    REST::SendErrorResponse(session, "Unknown Request", 404);
}

void RestServer::Shutdown() {
    if (http_server_) {
        http_server_->Shutdown();
        TcpServerManager::DeleteServer(http_server_);
        http_server_ = NULL;
    }
}

RestServer::~RestServer() {
}
