/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <csignal>
#include <fstream>

#include <pthread.h>
#include <boost/program_options.hpp>
#include "nodeinfo_types.h"
#include "base/connection_info.h"
#include <base/logging.h>
#include <base/misc_utils.h>
#include <base/contrail_ports.h>
#include <base/task.h>
#include <io/process_signal.h>
#include <config_client_manager.h>
#include <db/db_graph.h>
#include <ifmap/client/config_json_parser.h>
#include <ifmap/ifmap_link_table.h>
#include "ifmap/ifmap_sandesh_context.h"
#include <ifmap/ifmap_server.h>
#include <ifmap/ifmap_xmpp.h>
#include <io/event_manager.h>
#include <vnc_cfg_types.h>
#include <cmn/dns.h>
#include <bind/bind_util.h>
#include <mgr/dns_mgr.h>
#include <mgr/dns_oper.h>
#include <bind/named_config.h>
#include <cfg/dns_config.h>
#include <cfg/dns_config_parser.h>
#include <agent/agent_xmpp_init.h>
#include "cmn/dns_options.h"
#include <sandesh/common/vns_types.h>
#include <sandesh/common/vns_constants.h>
#include <uve/uve.h>
#include "xmpp/xmpp_sandesh.h"
#include <config-client-mgr/config_amqp_client.h>

namespace opt = boost::program_options;
using namespace boost::asio::ip;
using namespace std;
using process::ConnectionInfo;
using process::ConnectionStateManager;
using process::GetProcessStateCb;
using process::ProcessState;
using process::ConnectionType;
using process::ConnectionTypeName;
using process::g_process_info_constants;
using process::Signal;

uint64_t start_time;
TaskTrigger *dns_info_trigger;
Timer *dns_info_log_timer;
static Options options;

static void WaitForIdle(long wait_seconds = 1200, bool running_only = false);
static void WaitForIdle(long wait_seconds, bool running_only) {
    static const long kTimeoutUsecs = 1000;
    static long envWaitTime;

    if (!envWaitTime) {
        if (getenv("WAIT_FOR_IDLE")) {
            envWaitTime = atoi(getenv("WAIT_FOR_IDLE"));
        } else {
            envWaitTime = wait_seconds;
        }
    }

    if (envWaitTime > wait_seconds) wait_seconds = envWaitTime;

    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    for (long i = 0; i < ((wait_seconds * 1000000)/kTimeoutUsecs); i++) {
        if (scheduler->IsEmpty(running_only)) {
            return;
        }
        usleep(kTimeoutUsecs);
    }
}

static void PrintBulkSyncStats(DB *database) {
    DNS_OPERATIONAL_LOG(
        g_vns_constants.CategoryNames.find(Category::DNSAGENT)->second,
        SandeshLevel::SYS_NOTICE, "EndOfRib point was reached, "
        "IF-MAP objects have been created");
    uint32_t ntypes = DnsConfigManager::config_types_size / sizeof(const char *);
    for (uint32_t i=0; i<ntypes; i++) {
        const char *type_name = DnsConfigManager::config_types[i];
        IFMapTable *table = IFMapTable::FindTable(database, type_name);
        std::stringstream msg_str;
        msg_str << "The IF-MAP database table has been updated: "
                << table->name()
                << ", counting " << table->Size() << " elements";

        DNS_OPERATIONAL_LOG(
            g_vns_constants.CategoryNames.find(Category::DNSAGENT)->second,
            SandeshLevel::SYS_NOTICE, msg_str.str().c_str());
    }

    {
        std::stringstream msg_str;
        msg_str << "VnniConfig has been updated after"
                << " the bulk sync, the number of elements: "
                << VnniConfig::vnni_config_.size();
        DNS_OPERATIONAL_LOG(
            g_vns_constants.CategoryNames.find(Category::DNSAGENT)->second,
            SandeshLevel::SYS_NOTICE,
            msg_str.str().c_str());
    }

    {
        std::stringstream msg_str;
        msg_str << "IpamConfig has been updated after"
                << " the bulk sync, the number of elements: "
                << IpamConfig::ipam_config_.size();
        DNS_OPERATIONAL_LOG(
            g_vns_constants.CategoryNames.find(Category::DNSAGENT)->second,
            SandeshLevel::SYS_NOTICE,
            msg_str.str().c_str());
    }

    {
        std::stringstream msg_str;
        msg_str << "VirtualDnsConfig has been updated after"
                << " the bulk sync, the number of elements: "
                << VirtualDnsConfig::virt_dns_config_.size();
        DNS_OPERATIONAL_LOG(
            g_vns_constants.CategoryNames.find(Category::DNSAGENT)->second,
            SandeshLevel::SYS_NOTICE,
            msg_str.str().c_str());
    }

    {
        std::stringstream msg_str;
        msg_str << "VirtualDnsRecordConfig has been updated after"
                << " the bulk sync, the number of elements: "
                << VirtualDnsRecordConfig::virt_dns_rec_config_.size();
        DNS_OPERATIONAL_LOG(
            g_vns_constants.CategoryNames.find(Category::DNSAGENT)->second,
            SandeshLevel::SYS_NOTICE,
            msg_str.str().c_str());
    }

    {
        std::stringstream msg_str;
        msg_str << "GlobalQosConfig has been updated after"
                << " the bulk sync";
        DNS_OPERATIONAL_LOG(
            g_vns_constants.CategoryNames.find(Category::DNSAGENT)->second,
            SandeshLevel::SYS_NOTICE,
            msg_str.str().c_str());
    }
}

bool DnsInfoLogTimer() {
    dns_info_trigger->Set();
    return false;
}

bool DnsInfoLogger() {
    DnsUveClient::SendDnsUve(start_time);

    dns_info_log_timer->Cancel();
    dns_info_log_timer->Start(60*1000, boost::bind(&DnsInfoLogTimer),NULL);
    return true;
}

static void DnsServerGetProcessStateCb(
   const ConfigClientManager *config_client_manager,
   const std::vector<ConnectionInfo> &cinfos,
   ProcessState::type &state, std::string &message,
   std::vector<ConnectionTypeName> expected_connections) {
    GetProcessStateCb(cinfos, state, message, expected_connections);
    if (state == ProcessState::NON_FUNCTIONAL)
        return;
    if (!config_client_manager->GetEndOfRibComputed()) {
        state = ProcessState::NON_FUNCTIONAL;
        message = "IFMap Server End-Of-RIB not computed";
    }
}

static string FileRead(const string &filename) {
    ifstream file(filename.c_str());
    string content((istreambuf_iterator<char>(file)),
                   istreambuf_iterator<char>());
    return content;
}

static void IFMap_Initialize(IFMapServer *server, ConfigJsonParser *json_parser) {
    IFMapLinkTable_Init(server->database(), server->graph());
    vnc_cfg_JsonParserInit(json_parser);
    vnc_cfg_Server_ModuleInit(server->database(), server->graph());
    server->Initialize();
}

static bool OptionsParse(Options &options, int argc, char *argv[]) {
    try {
        options.Parse(*Dns::GetEventManager(), argc, argv);
        return true;
    } catch (boost::program_options::error &e) {
        cout << "Error " << e.what() << endl;
    } catch (...) {
        cout << "Options Parser: Caught fatal unknown exception" << endl;
    }

    return false;
}

void ReConfigSignalHandler(const boost::system::error_code &error, int sig) {
    if (error) {
        LOG(ERROR, "SIGHUP handler ERROR: " << error);
        return;
    }
    options.ParseReConfig();
}

int main(int argc, char *argv[]) {
    // Initialize the task scheduler
    int num_threads_to_tbb = TaskScheduler::GetDefaultThreadCount() +
        ConfigClientManager::GetNumWorkers();
    TaskScheduler::Initialize(num_threads_to_tbb);

    // Create DB table and event manager
    Dns::Init();

    // Process options from command-line and configuration file.
    if (!OptionsParse(options, argc, argv)) {
        exit(-1);
    }

    srand(unsigned(time(NULL)));
    std::vector<Signal::SignalHandler> sighup_handlers = boost::assign::list_of
        (boost::bind(&ReConfigSignalHandler, _1, _2));
    Signal::SignalCallbackMap smap = boost::assign::map_list_of
        (SIGHUP, sighup_handlers);
    Signal signal(Dns::GetEventManager(), smap);

    Dns::SetProgramName(argv[0]);
    Module::type module = Module::DNS;
    string module_name = g_vns_constants.ModuleNames.find(module)->second;

    std::string log_property_file = options.log_property_file();
    if (log_property_file.size()) {
        LoggingInit(log_property_file);
    }
    else {
        LoggingInit(options.log_file(), options.log_file_size(),
                    options.log_files_count(), options.use_syslog(),
                    options.syslog_facility(), module_name,
                    SandeshLevelTolog4Level(
                        Sandesh::StringToLevel(options.log_level())));
    }

    string build_info_str;
    Dns::GetVersion(build_info_str);
    MiscUtils::LogVersionInfo(build_info_str, Category::DNSAGENT);

    if (options.collectors_configured()) {
        Dns::SetCollector(options.collector_server_list()[0]);
    }
    Dns::SetHttpPort(options.http_server_port());
    Dns::SetDnsPort(options.dns_server_port());

    string hostname = options.hostname();
    Dns::SetHostName(hostname);

    ConnectionStateManager::GetInstance();
    NodeType::type node_type =
        g_vns_constants.Module2NodeType.find(module)->second;
    bool success;
    if (options.collectors_configured()) {
        success = Sandesh::InitGenerator(
                    module_name,
                    options.hostname(),
                    g_vns_constants.NodeTypeNames.find(node_type)->second,
                    g_vns_constants.INSTANCE_ID_DEFAULT,
                    Dns::GetEventManager(),
                    options.http_server_port(),
                    options.randomized_collector_server_list(),
                    NULL,
                    Sandesh::DerivedStats(),
                    options.sandesh_config());
    } else {
        success = Sandesh::InitGenerator(
                    g_vns_constants.ModuleNames.find(module)->second,
                    options.hostname(),
                    g_vns_constants.NodeTypeNames.find(node_type)->second,
                    g_vns_constants.INSTANCE_ID_DEFAULT,
                    Dns::GetEventManager(),
                    options.http_server_port(),
                    NULL,
                    Sandesh::DerivedStats(),
                    options.sandesh_config());
    }
    if (!success) {
        LOG(ERROR, "SANDESH: Initialization FAILED ... exiting");
        Sandesh::Uninit();
        exit(1);
    }
    Sandesh::SetLoggingParams(options.log_local(), options.log_category(),
                              options.log_level());

    // XXX Disable logging -- for test purposes only
    if (options.log_disable()) {
        SetLoggingDisabled(true);
    }

    // DNS::SetTestMode(options.test_mode());

    DB config_db(TaskScheduler::GetInstance()->GetTaskId("db::IFMapTable"));
    DBGraph config_graph;
    IFMapServer ifmap_server(&config_db, &config_graph,
                             Dns::GetEventManager()->io_service());

    ConfigStaticObjectFactory::LinkImpl<ConfigJsonParserBase,
        ConfigJsonParser>();
    boost::scoped_ptr<ConfigClientManager> config_client_manager_ptr(
        new ConfigClientManager(Dns::GetEventManager(),
            ConfigStaticObjectFactory::Create<ConfigJsonParserBase>(),
            options.hostname(),
            module_name,
            options.configdb_options()));
    ConfigClientManager *config_client_manager = config_client_manager_ptr.get();
    ConfigJsonParser *json_parser = static_cast<ConfigJsonParser *>(
        config_client_manager->config_json_parser());
    json_parser->ifmap_server_set(&ifmap_server);
    IFMap_Initialize(&ifmap_server, json_parser);


    DnsManager dns_manager;
    Dns::SetDnsManager(&dns_manager);
    dns_manager.Initialize(&config_db, &config_graph,
                           options.named_config_dir(),
                           options.named_config_file(),
                           options.named_log_file(),
                           options.rndc_config_file(),
                           options.rndc_secret(),
                           options.named_max_cache_size(),
                           options.named_max_retransmissions(),
                           options.named_retransmission_interval());
    DnsConfigParser parser(&config_db);
    parser.Parse(FileRead(options.config_file()));

    Dns::SetSelfIp(options.host_ip());
    if (!DnsAgentXmppManager::Init(options.xmpp_auth_enabled(),
                                   options.xmpp_server_cert(),
                                   options.xmpp_server_key(),
                                   options.xmpp_ca_cert())) {
        LOG(ERROR, "Address already in use " << ContrailPorts::DnsXmpp());
        exit(1);
    }

    XmppSandeshContext xmpp_sandesh_context;
    xmpp_sandesh_context.xmpp_server = Dns::GetXmppServer();
    Sandesh::set_module_context("XMPP", &xmpp_sandesh_context);
    IFMapSandeshContext ifmap_sandesh_context(&ifmap_server);
    Sandesh::set_module_context("IFMap", &ifmap_sandesh_context);

    start_time = UTCTimestampUsec();
    dns_info_trigger =
            new TaskTrigger(boost::bind(&DnsInfoLogger),
                    TaskScheduler::GetInstance()->GetTaskId("dns::Config"), 0);

    dns_info_log_timer =
        TimerManager::CreateTimer(*(Dns::GetEventManager()->io_service()),
                                                   "Dns Info log timer");
    dns_info_log_timer->Start(60*1000, boost::bind(&DnsInfoLogTimer), NULL);
    Dns::SetSelfIp(options.host_ip());
    ifmap_server.set_config_manager(config_client_manager);

    //
    // Determine if the number of connections is as expected.
    // 1. Cassandra Server
    // 2. AMQP Server
    //
    std::vector<ConnectionTypeName> expected_connections;
    expected_connections = boost::assign::list_of
         (ConnectionTypeName(g_process_info_constants.ConnectionTypeNames.find(
                             ConnectionType::DATABASE)->second, "Cassandra"))
         (ConnectionTypeName(g_process_info_constants.ConnectionTypeNames.find(
                             ConnectionType::DATABASE)->second, "RabbitMQ"))
         (ConnectionTypeName(g_process_info_constants.ConnectionTypeNames.find(
                             ConnectionType::COLLECTOR)->second, ""))
                                 .convert_to_container\
                                  <vector<ConnectionTypeName> >();

    if (!options.collectors_configured()) {
        LOG(INFO, "DNS: Collectors are not configured!");
        expected_connections.pop_back();
    }

    ConnectionStateManager::GetInstance()->Init(
        *(Dns::GetEventManager()->io_service()), options.hostname(),
        module_name, g_vns_constants.INSTANCE_ID_DEFAULT,
        boost::bind(&DnsServerGetProcessStateCb, config_client_manager, _1, _2, _3,
                    expected_connections), "ObjectDns");

    dns_manager.set_config_manager(config_client_manager);
    options.set_config_client_manager(config_client_manager);
    DNS_OPERATIONAL_LOG(
        g_vns_constants.CategoryNames.find(Category::DNSAGENT)->second,
        SandeshLevel::SYS_NOTICE, "Bulk sync has been started");
    config_client_manager->Initialize();
    config_client_manager->config_amqp_client()->set_terminate(true);
    WaitForIdle();  // 20 mins
    PrintBulkSyncStats(&config_db);
    config_client_manager->config_amqp_client()->set_terminate(false);
    config_client_manager->config_amqp_client()->StartRabbitMQReader();
    dns_manager.StartEndofConfigTimer();
    Dns::GetEventManager()->Run();
    signal.Terminate();
    return 0;
}
