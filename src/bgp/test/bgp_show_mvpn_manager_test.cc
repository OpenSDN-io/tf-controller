/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/test/bgp_show_instance_or_table_test.h"
#include "bgp/bgp_mvpn.h"

typedef TypeDefinition<
    ShowMvpnManagerReq,
    ShowMvpnManagerReqIterate,
    ShowMvpnManagerResp> MvpnReq;

// Specialization of AddInstanceOrTableName.
template<>
void BgpShowInstanceOrTableTest<MvpnReq>::AddInstanceOrTableName(
    vector<string> *names, const string &name) {
    string table_name;
    if (name == BgpConfigManager::kMasterInstance) {
        table_name = "bgp.mvpn.0";
    } else {
        table_name = name + ".mvpn.0";
    }
    names->push_back(table_name);
}

// Specialization of ValidateResponse.
template<>
void BgpShowInstanceOrTableTest<MvpnReq>::ValidateResponse(
    Sandesh *sandesh, vector<string> &result, const string &next_batch) {
    MvpnReq::RespT *resp = dynamic_cast<MvpnReq::RespT *>(sandesh);
    TASK_UTIL_EXPECT_TRUE(resp != NULL);
    TASK_UTIL_EXPECT_EQ(result.size(), resp->get_managers().size());
    TASK_UTIL_EXPECT_EQ(next_batch, resp->get_next_batch());
    for (size_t i = 0; i < resp->get_managers().size(); ++i) {
        TASK_UTIL_EXPECT_EQ(result[i], resp->get_managers()[i].get_name());
        cout << resp->get_managers()[i].log() << endl;
    }
    validate_done_ = true;
}

// Instantiate all test patterns for ShowMvpnManagerReq.
INSTANTIATE_TYPED_TEST_CASE_P(Config, BgpShowInstanceOrTableTest, MvpnReq);

class TestEnvironment : public ::testing::Environment {
    virtual ~TestEnvironment() { }
};

static void SetUp() {
    ControlNode::SetDefaultSchedulingPolicy();
    BgpServerTest::GlobalSetUp();
    XmppStaticObjectFactory::LinkImpl<XmppStateMachine,
        XmppStateMachineTest,XmppConnection*,bool,bool,int>();
    BgpStaticObjectFactory::LinkImpl<BgpXmppMessageBuilder,
        BgpXmppMessageBuilder>();
}

static void TearDown() {
    task_util::WaitForIdle();
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Terminate();
}

int main(int argc, char **argv) {
    bgp_log_test::init();
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new TestEnvironment());
    SetUp();
    int result = RUN_ALL_TESTS();
    TearDown();
    return result;
}
