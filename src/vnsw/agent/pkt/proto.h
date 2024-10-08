/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_proto_hpp
#define vnsw_agent_proto_hpp

#include "pkt_handler.h"

class Agent;
class ProtoHandler;
class TokenPool;

// Protocol task (work queue for each protocol)
class Proto {
public:
    typedef WorkQueue<boost::shared_ptr<PktInfo> > ProtoWorkQueue;

    Proto(Agent *agent, const char *task_name, PktHandler::PktModuleName mod,
          boost::asio::io_context &io);
    virtual ~Proto();

    virtual bool Validate(PktInfo *msg) { return true; }
    virtual bool Enqueue(boost::shared_ptr<PktInfo> msg);
    virtual void ProcessStats(PktStatsType::Type type) { return; }
    virtual ProtoHandler *AllocProtoHandler(boost::shared_ptr<PktInfo> info,
                                            boost::asio::io_context &io) = 0;

    void FreeBuffer(PktInfo *msg);
    bool ProcessProto(boost::shared_ptr<PktInfo> msg_info);
    bool RunProtoHandler(ProtoHandler *handler);
    void set_trace(bool val) { trace_ = val; }
    void set_free_buffer(bool val) { free_buffer_ = val; }
    boost::asio::io_context &get_io_service() const { return io_; }
    Agent *agent() const { return agent_; }
    const ProtoWorkQueue *work_queue() const { return &work_queue_; }
    virtual void TokenAvailable(TokenPool *pool) {assert(0);}
protected:
    Agent *agent_;
    PktHandler::PktModuleName module_;
    bool trace_;
    bool free_buffer_;
    boost::asio::io_context &io_;
    ProtoWorkQueue work_queue_;

private:
    DISALLOW_COPY_AND_ASSIGN(Proto);
};

#endif // vnsw_agent_proto_hpp
