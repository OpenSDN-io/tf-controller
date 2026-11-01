/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 * Copyright Contributors to the OpenSDN project
 */
#define __SERVICE_CHAIN_TEST_WRAPPER_TEST_SUITE__
#include "service_chain_test.cc"

int main(int argc, char **argv) {
    const char *largv[] = {
        __FILE__,
        "--address-family=ip",
        "--service-type=non-transparent",
    };
    return service_chain_test_main(sizeof(largv)/sizeof(largv[0]), largv);
}
