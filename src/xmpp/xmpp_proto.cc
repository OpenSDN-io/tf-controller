/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "xmpp/xmpp_proto.h"
#include <iostream>
#include <string>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "xmpp/xmpp_connection.h"
#include "xmpp/xmpp_log.h"
#include "xmpp/xmpp_session.h"
#include "xmpp/xmpp_str.h"

#include "sandesh/sandesh_trace.h"
#include "sandesh/common/vns_types.h"
#include "sandesh/common/vns_constants.h"
#include "sandesh/xmpp_message_sandesh_types.h"
#include "sandesh/xmpp_trace_sandesh_types.h"

using namespace std;

unique_ptr<XmlBase> XmppProto::open_doc_(AllocXmppXmlImpl(sXMPP_STREAM_OPEN));

XmppStanza::XmppStanza() {
}

XmppProto::XmppProto() {
}

XmppProto::~XmppProto() {
}

int XmppProto::EncodeStream(const XmppStreamMessage &str, string &to,
                            string &from, const string &xmlns, uint8_t *buf,
                            size_t size) {
    int len = 0;

    switch (str.strmtype) {
        case (XmppStanza::XmppStreamMessage::INIT_STREAM_HEADER):
            len = EncodeOpen(buf, to, from, xmlns, size);
            break;
        case (XmppStanza::XmppStreamMessage::INIT_STREAM_HEADER_RESP):
            len = EncodeOpenResp(buf, to, from, size);
            break;
        case (XmppStanza::XmppStreamMessage::FEATURE_TLS):
            switch (str.strmtlstype) {
                case (XmppStanza::XmppStreamMessage::TLS_FEATURE_REQUEST):
                    len = EncodeFeatureTlsRequest(buf);
                    break;
                case (XmppStanza::XmppStreamMessage::TLS_START):
                    len = EncodeFeatureTlsStart(buf);
                    break;
                case (XmppStanza::XmppStreamMessage::TLS_PROCEED):
                    len = EncodeFeatureTlsProceed(buf);
                    break;
            }
            break;
        default:
            break;
    }

    return len;
}

int XmppProto::EncodeStream(const XmppStanza::XmppMessage &str, uint8_t *buf,
                            size_t size) {
    int ret = 0;

    if (str.type == XmppStanza::WHITESPACE_MESSAGE_STANZA) {
        return EncodeWhitespace(buf);
    }

    return ret;
}

int XmppProto::EncodeMessage(XmlBase *dom, uint8_t *buf, size_t size) {
    int len = dom->WriteDoc(buf);

    return len;
}

int XmppProto::EncodePresence(uint8_t *buf, size_t size) {
    return 0;
}

int XmppProto::EncodeIq(const XmppStanza::XmppMessageIq *iq,
                        XmlBase *doc, uint8_t *buf, size_t size) {
    unique_ptr<XmlBase> send_doc_(AllocXmppXmlImpl());

    // create
    send_doc_->LoadDoc("");
    send_doc_->AddNode("iq", "");

    switch(iq->stype) {
        case XmppStanza::XmppMessageIq::GET:
            send_doc_->AddAttribute("type", "get");
            break;
        case XmppStanza::XmppMessageIq::SET:
            send_doc_->AddAttribute("type", "set");
            break;
        case XmppStanza::XmppMessageIq::RESULT:
            send_doc_->AddAttribute("type", "result");
            break;
        case XmppStanza::XmppMessageIq::ERROR:
            send_doc_->AddAttribute("type", "error");
            break;
        default:
            break;
    }
    send_doc_->AddAttribute("from", iq->from);
    send_doc_->AddAttribute("to", iq->to);
    send_doc_->AddAttribute("id", "id1");

    send_doc_->AddChildNode("pubsub", "");
    send_doc_->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");

    send_doc_->AppendDoc("pubsub", doc);

    //Returns byte encoded in the doc
    int len = send_doc_->WriteDoc(buf);

    return len;
}

int XmppProto::EncodeWhitespace(uint8_t *buf) {
    string str(sXMPP_WHITESPACE);

    int len = str.size();
    if (len > 0) {
        memcpy(buf, str.data(), len);
    }

    return len;
}

int XmppProto::EncodeOpenResp(uint8_t *buf, string &to, string &from,
                              size_t max_size) {

    unique_ptr<XmlBase> resp_doc(XmppStanza::AllocXmppXmlImpl(sXMPP_STREAM_RESP));

    if (resp_doc.get() == NULL) {
        return 0;
    }

    SetTo(to, resp_doc.get());
    SetFrom(from, resp_doc.get());

    std::stringstream ss;
    resp_doc->PrintDoc(ss);
    std::string msg;
    msg = ss.str();
    size_t len = msg.size();
    if (len > max_size) {
        LOG(ERROR, "\n (Open Confirm) size greater than max buffer size \n");
        return 0;
    } else {
        boost::algorithm::ireplace_last(msg, "/", " ");
        memcpy(buf, msg.c_str(), len);
        return len;
    }
}

int XmppProto::EncodeOpen(uint8_t *buf, string &to, string &from,
                          const string &xmlns, size_t max_size) {

    if (open_doc_.get() ==  NULL) {
        return 0;
    }

    SetTo(to, open_doc_.get());
    SetFrom(from, open_doc_.get());
    SetXmlns(xmlns, open_doc_.get());

    //Returns byte encoded in the doc
    std::stringstream ss;
    open_doc_->PrintDoc(ss);
    std::string msg;
    msg = ss.str();
    size_t len = msg.size();
    if (len > max_size) {
        LOG(ERROR, "\n (Open Message) size greater than max buffer size \n");
        return 0;
    } else {
        boost::algorithm::ireplace_last(msg, "/", " ");
        memcpy(buf, msg.c_str(), len);
        return len;
    }
}

int XmppProto::EncodeFeatureTlsRequest(uint8_t *buf) {
    unique_ptr<XmlBase> resp_doc(XmppStanza::AllocXmppXmlImpl(sXMPP_STREAM_FEATURE_TLS));
    //Returns byte encoded in the doc
    int len = resp_doc->WriteDoc(buf);
    return len;
}

int XmppProto::EncodeFeatureTlsStart(uint8_t *buf) {
    unique_ptr<XmlBase> resp_doc(XmppStanza::AllocXmppXmlImpl(sXMPP_STREAM_START_TLS));
    //Returns byte encoded in the doc
    int len = resp_doc->WriteDoc(buf);
    return len;
}

int XmppProto::EncodeFeatureTlsProceed(uint8_t *buf) {
    unique_ptr<XmlBase> resp_doc(XmppStanza::AllocXmppXmlImpl(sXMPP_STREAM_PROCEED_TLS));
    //Returns byte encoded in the doc
    int len = resp_doc->WriteDoc(buf);
    return len;
}

XmppStanza::XmppMessage *XmppProto::Decode(const XmppConnection *connection,
                                           const string &ts) {
    XmlBase *impl = XmppStanza::AllocXmppXmlImpl();
    if (impl == nullptr) {
        return nullptr;
    }

    XmppStanza::XmppMessage *msg = DecodeInternal(connection, ts, impl);
    if (!msg) {
        return nullptr;
    }

    // transfer ownership of the dom implementation
    msg->dom.reset(impl);

    return msg;
}

XmppStanza::XmppMessage *XmppProto::DecodeInternal(
        const XmppConnection *connection, const string &ts, XmlBase *impl) {
    XmppStanza::XmppMessage *ret = nullptr;

    string ns(sXMPP_STREAM_O);
    string ws(sXMPP_WHITESPACE);
    string iq(sXMPP_IQ_KEY);

    if (ts.find(sXMPP_IQ) != string::npos) {
        string ts_tmp = ts;

        if (impl->LoadDoc(ts) == -1) {
            XMPP_WARNING(XmppIqMessageParseFail, connection->ToUVEKey(),
                         XMPP_PEER_DIR_IN);
            assert(false);
            goto done;
        }

        XmppStanza::XmppMessageIq *msg = new XmppStanza::XmppMessageIq;
        impl->ReadNode(iq);
        msg->to = XmppProto::GetTo(impl);
        msg->from = XmppProto::GetFrom(impl);
        msg->id = XmppProto::GetId(impl);
        msg->iq_type = XmppProto::GetType(impl);
        // action is subscribe,publish,collection
        const char *action = XmppProto::GetAction(impl, msg->iq_type);
        if (action) {
            msg->action = action;
        }
        if (XmppProto::GetNode(impl, msg->action)) {
            msg->node = XmppProto::GetNode(impl, msg->action);
        }
        //associate or dissociate collection node
        if (msg->action.compare("collection") == 0) {
            if (XmppProto::GetAsNode(impl)) {
                msg->as_node = XmppProto::GetAsNode(impl);
                msg->is_as_node = true;
            } else if (XmppProto::GetDsNode(impl)) {
                msg->as_node = XmppProto::GetDsNode(impl);
                msg->is_as_node = false;
            }
        }

        //msg->dom.reset(impl);

        ret = msg;

        XMPP_UTDEBUG(XmppIqMessageProcess, connection->ToUVEKey(),
                     XMPP_PEER_DIR_IN, msg->node, msg->action, msg->from,
                     msg->to, msg->id, msg->iq_type);
        goto done;

    } else if (ts.find(sXMPP_MESSAGE) != string::npos) {

        if (impl->LoadDoc(ts) == -1) {
            XMPP_WARNING(XmppChatMessageParseFail, connection->ToUVEKey(),
                         XMPP_PEER_DIR_IN);
            goto done;
        }
        XmppStanza::XmppMessage *msg = new XmppStanza::XmppChatMessage(
                                           STATE_NONE);
        impl->ReadNode(sXMPP_MESSAGE_KEY);

        msg->to = XmppProto::GetTo(impl);
        msg->from = XmppProto::GetFrom(impl);
        ret = msg;

        XMPP_UTDEBUG(XmppChatMessageProcess, connection->ToUVEKey(),
                     XMPP_PEER_DIR_IN, msg->type, msg->from, msg->to);
        goto done;

    } else if (ts.find(sXMPP_STREAM_O) != string::npos) {

        // ensusre stream open is at the beginning of the message
        string ts_tmp = ts;
        ts_tmp.erase(std::remove(ts_tmp.begin(), ts_tmp.end(), '\n'), ts_tmp.end());

        if ((ts_tmp.compare(0, strlen(sXMPP_STREAM_START),
             sXMPP_STREAM_START) != 0) &&
            (ts_tmp.compare(0, strlen(sXMPP_STREAM_START_S),
             sXMPP_STREAM_START_S) != 0)) {
            XMPP_WARNING(XmppBadMessage, connection->ToUVEKey(),
                         XMPP_PEER_DIR_IN,
                         "Open message not at the beginning.", ts);
            goto done;
        }

        // check if the buf is xmpp open or response message
        // As end tag will be missing we need to modify the
        // string for stream open, else dom decoder will fail
        boost::algorithm::replace_last(ts_tmp, ">", "/>");
        if (impl->LoadDoc(ts_tmp) == -1) {
            XMPP_WARNING(XmppBadMessage, connection->ToUVEKey(),
                         XMPP_PEER_DIR_IN, "Open message parse failed.", ts);
            goto done;
        }

        XmppStanza::XmppStreamMessage *strm =
            new XmppStanza::XmppStreamMessage();
        strm->strmtype = XmppStanza::XmppStreamMessage::INIT_STREAM_HEADER;
        impl->ReadNode(ns);
        strm->to = XmppProto::GetTo(impl);
        strm->from = XmppProto::GetFrom(impl);
        strm->xmlns = XmppProto::GetXmlns(impl);

        ret = strm;

        XMPP_UTDEBUG(XmppRxOpenMessage, connection->ToUVEKey(),
                     XMPP_PEER_DIR_IN, strm->from, strm->to);

    } else if (ts.find(sXMPP_STREAM_NS_TLS) != string::npos) {

        if (impl->LoadDoc(ts) == -1) {
            XMPP_WARNING(XmppBadMessage, connection->ToUVEKey(),
                         XMPP_PEER_DIR_IN, "Stream TLS parse failed.", ts);
            goto done;
        }

        // find stream:features tls required
        if ((ts.find(sXMPP_STREAM_FEATURES_O) != string::npos) &&
            (ts.find(sXMPP_STREAM_STARTTLS_O) != string::npos) &&
            (ts.find(sXMPP_REQUIRED_O) != string::npos)) {

            XmppStanza::XmppStreamMessage *strm =
                new XmppStanza::XmppStreamMessage();
            strm->strmtype = XmppStanza::XmppStreamMessage::FEATURE_TLS;
            strm->strmtlstype = XmppStanza::XmppStreamMessage::TLS_FEATURE_REQUEST;

            ret = strm;

            XMPP_UTDEBUG(XmppRxStreamTlsRequired, connection->ToUVEKey(),
                         XMPP_PEER_DIR_IN);

        } else if (ts.find(sXMPP_STREAM_STARTTLS_O) != string::npos) {
            XmppStanza::XmppStreamMessage *strm =
                new XmppStanza::XmppStreamMessage();
            strm->strmtype = XmppStanza::XmppStreamMessage::FEATURE_TLS;
            strm->strmtlstype = XmppStanza::XmppStreamMessage::TLS_START;
            ret = strm;

            XMPP_UTDEBUG(XmppRxStreamStartTls, connection->ToUVEKey(),
                         XMPP_PEER_DIR_IN);

        } else if (ts.find(sXMPP_STREAM_PROCEED_O) != string::npos) {
            XmppStanza::XmppStreamMessage *strm =
                new XmppStanza::XmppStreamMessage();
            strm->strmtype = XmppStanza::XmppStreamMessage::FEATURE_TLS;
            strm->strmtlstype = XmppStanza::XmppStreamMessage::TLS_PROCEED;

            ret = strm;

            XMPP_UTDEBUG(XmppRxStreamProceed, connection->ToUVEKey(),
                         XMPP_PEER_DIR_IN);
        }
        goto done;

    } else if (ts.find_first_of(sXMPP_VALIDWS) != string::npos) {

        XmppStanza::XmppMessage *msg =
            new XmppStanza::XmppMessage(WHITESPACE_MESSAGE_STANZA);
        return msg;
    } else {
        XMPP_WARNING(XmppBadMessage, connection->ToUVEKey(),
                     XMPP_PEER_DIR_IN, "Message not supported", ts);
    }

done:

    return ret;
}

int XmppProto::SetTo(string &to, XmlBase *doc) {
    if (!doc) return -1;

    string ns(sXMPP_STREAM_O);
    doc->ReadNode(ns);
    doc->ModifyAttribute("to", to);

    return 0;
}

int XmppProto::SetFrom(string &from, XmlBase *doc) {
    if (!doc) return -1;

    string ns(sXMPP_STREAM_O);
    doc->ReadNode(ns);
    return doc->ModifyAttribute("from", from);
}

int XmppProto::SetXmlns(const string &xmlns, XmlBase *doc) {
    if (!doc)
        return -1;

    string ns(sXMPP_STREAM_O);
    doc->ReadNode(ns);
    return doc->ModifyAttribute("xmlns", xmlns);
}

const char *XmppProto::GetTo(XmlBase *doc) {
    if (!doc) return NULL;

    string tmp("to");
    return doc->ReadAttrib(tmp);
}

const char *XmppProto::GetFrom(XmlBase *doc) {
    if (!doc) return NULL;

    string tmp("from");
    return doc->ReadAttrib(tmp);
}

const char *XmppProto::GetXmlns(XmlBase *doc) {
    if (!doc)
        return NULL;

    string tmp("xmlns");
    return doc->ReadAttrib(tmp);
}

const char *XmppProto::GetId(XmlBase *doc) {
    if (!doc) return NULL;

    string tmp("id");
    return doc->ReadAttrib(tmp);
}

const char *XmppProto::GetType(XmlBase *doc) {
    if (!doc) return NULL;

    string tmp("type");
    return doc->ReadAttrib(tmp);
}

const char *XmppProto::GetAction(XmlBase *doc, const string &str) {
    if (!doc) return NULL;

    if (str.compare("set") == 0) {
        doc->ReadNode("pubsub");
        return(doc->ReadChildNodeName());
    } else if (str.compare("get") == 0) {
    }

    return(NULL);
}

const char *XmppProto::GetNode(XmlBase *doc, const string &str) {
    if (!doc) return NULL;

    if (!str.empty()) {
        return(doc->ReadAttrib("node"));
    }

    return(NULL);
}

const char *XmppProto::GetAsNode(XmlBase *doc) {
    if (!doc) return NULL;

    const char *node = doc->ReadNode("associate");
    if (node != NULL) {
        return(doc->ReadAttrib("node"));
    }

    return(NULL);
}

const char *XmppProto::GetDsNode(XmlBase *doc) {
    if (!doc) return NULL;

    const char *node = doc->ReadNode("dissociate");
    if (node != NULL) {
        return(doc->ReadAttrib("node"));
    }

    return(NULL);
}
