/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_LARGE_COMMUNITY_TAG_H_
#define SRC_BGP_LARGE_COMMUNITY_TAG_H_

#include <boost/array.hpp>
#include <stdint.h>
#include <string>

#include "base/parse_object.h"
#include "bgp/bgp_common.h"

/// @brief Represents a single BGP Large Community tag.
class TagLC {
public:
    /// Fixed size (in bytes) of a BGP Large Community value.
    static const int kSize = 12;
    /// Minimum tag id to belong to a global community.
    static const int kMinGlobalId = 8000000;
    /// Raw 12-byte type representing the community on the wire.
    typedef boost::array<uint8_t, kSize> bytes_type;

    /// Construct from a raw 12-byte value.
    explicit TagLC(const bytes_type &data);
    /// Construct from AS number and tag value.
    explicit TagLC(as_t asn, uint64_t tag);
    /// Returns a string representation.
    std::string ToString() const;

    /// Returns true if this tag belongs to a global community.
    bool IsGlobal() const;
    /// Returns the AS number.
    as_t as_number() const;
    /// Returns the tag id.
    uint64_t tag() const;

    /// Returns the community value as a vector of 3 32-bit integers.
    const std::vector<uint32_t> GetLargeCommunityValue() const {
        std::vector<uint32_t> lc_val;
        for (int i =0; i < 3; i++) {
            uint32_t value = get_value(data_.begin() + 4*i, 4);
            lc_val.push_back(value);
        }
        return lc_val;
    }

private:
    /// Raw 12-byte encoded Large Community value.
    bytes_type data_;
};

#endif  // SRC_BGP_LARGE_COMMUNITY_TAG_H_
