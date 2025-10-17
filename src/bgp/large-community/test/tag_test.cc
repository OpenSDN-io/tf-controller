/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/large-community/tag.h"
#include "bgp/large-community/types.h"

#include "testing/gunit.h"

using namespace std;

class TagLCTest : public ::testing::Test {
};

TEST_F(TagLCTest, ByteArray_1) {
    TagLC::bytes_type data = { {
        0x00, 0x00, 0x00, 0x00,
        BgpLargeCommunityType::TagLC, 0x00, 0x00, 0x00,
        0x0, 0x0, 0x00, 0x00
    } };
    TagLC tag(data);
    EXPECT_EQ("tagLC:0:0", tag.ToString());
}

TEST_F(TagLCTest, ByteArray_2) {
    TagLC::bytes_type data = { {
        0x00, 0x00, 0x00, 0x00,
        BgpLargeCommunityType::TagLC, 0x00, 0x00, 0x00,
        0x0, 0x1, 0x0, 0x0
    } };
    TagLC tag(data);
    EXPECT_EQ("tagLC:0:65536", tag.ToString());
}

TEST_F(TagLCTest, ByteArray_3) {
    TagLC::bytes_type data = { {
        0x00, 0x00, 0x00, 0xff,
        BgpLargeCommunityType::TagLC, 0x00, 0x00, 0x00,
        0x80, 0x0, 0x0, 0x1
    } };
    TagLC tag(data);
    EXPECT_EQ("tagLC:255:2147483649", tag.ToString());
}


TEST_F(TagLCTest, Init) {
    TagLC tag(100, 100);
    EXPECT_EQ(tag.ToString(), "tagLC:100:100");
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    return result;
}
