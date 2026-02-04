/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */
#include <base/misc_utils.h>
#include <cmn/buildinfo.h>
#include <cmn/agent_cmn.h>
#include <init/agent_param.h>
#include <controller/controller_init.h>
#include "linux_vxlan_agent_init.h"

namespace opt = boost::program_options;
using namespace std;

void RouterIdDepInit(Agent *agent) {
    // Parse config and then connect
    Agent::GetInstance()->controller()->Connect();
    LOG(DEBUG, "Router ID Dependent modules (Nova and BGP) INITIALIZED");
}

int main(int argc, char *argv[]) {
    AgentParam params(false, false, false, false);
    Logging logging;

    const opt::variables_map &var_map = params.var_map();
    try {
        params.ParseArguments(argc, argv);
    } catch (...) {
        cout << "Invalid arguments. ";
        cout << params.options() << endl;
        exit(0);
    }

    if (var_map.count("help")) {
        cout << params.options() << endl;
        exit(0);
    }

    if (var_map.count("version")) {
        cout << BuildInfo << endl;
        exit(0);
    }

    string init_file = "";
    if (var_map.count("config_file")) {
        init_file = var_map["config_file"].as<string>();
        struct stat s;
        if (stat(init_file.c_str(), &s) != 0) {
            LOG(ERROR, "Error opening config file <" << init_file
                << ">. Error number <" << errno << ">");
            exit(EINVAL);
        }
    }

    // Read agent parameters from config file and arguments
    params.Init(init_file, argv[0]);

    // Initialize TBB
    // Call to GetScheduler::GetInstance() will also create Task Scheduler
    TaskScheduler::Initialize(params.tbb_thread_count());

    // Initialize the agent-init control class
    LinuxVxlanAgentInit init;
    Agent *agent = init.agent();

    MiscUtils::LogVersionInfo(BuildInfo, Category::VROUTER);

    init.set_agent_param(&params);

    // kick start initialization
    int ret = 0;
    if ((ret = init.Start(logging)) != 0) {
        return ret;
    }

    agent->event_manager()->Run();

    return 0;
}
