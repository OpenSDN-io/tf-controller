/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 * Copyright Contributors to the OpenSDN project
 */
#include "base/logging.h"
#include "bfd/contrail_bfd.h"

int main(int argc, char *argv[]) {
    LoggingInit();
    ContrailBfd contrail_bfd;

    // Event loop.
    contrail_bfd.evm()->Run();
    return 0;
}
