/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/large-community/tag.h"

#include <stdio.h>

#include <algorithm>
#include <string>

#include "base/parse_object.h"
#include "bgp/large-community/types.h"

using std::copy;
using std::string;

TagLC::TagLC(const bytes_type &data) {
    copy(data.begin(), data.end(), data_.begin());
}

TagLC::TagLC(as_t asn, uint64_t tag) {
    put_value(&data_[0], 4, asn); // ASN
    data_[4] = BgpLargeCommunityType::TagLC;
    put_value(&data_[5], 7, tag); // TagLC value
}

as_t TagLC::as_number() const {
    if (data_[4] == BgpLargeCommunityType::TagLC) {
        as_t as_number =  get_value(data_.data(), 4);
        return as_number;
    }
    return 0;
}

uint64_t TagLC::tag() const {
    if (data_[4] == BgpLargeCommunityType::TagLC) {
        uint64_t value = get_value(&data_[5], 7);
        return value;
    }
    return 0;
}

bool TagLC::IsGlobal() const {
    return (tag() >= kMinGlobalId);
}

string TagLC::ToString() const {
    char temp[50];
    snprintf(temp, sizeof(temp), "tagLC:%u:%lu", as_number(), tag());
    return string(temp);
}
