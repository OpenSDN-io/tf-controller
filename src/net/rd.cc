/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "net/rd.h"

#include <string>

#include "base/parse_object.h"
#include "base/address.h"

using std::string;

RouteDistinguisher RouteDistinguisher::kZeroRd;

RouteDistinguisher::RouteDistinguisher() {
    memset(data_, 0, kSize);
}

RouteDistinguisher::RouteDistinguisher(const uint8_t *data) {
    memcpy(data_, data, kSize);
}

RouteDistinguisher::RouteDistinguisher(uint32_t address, uint16_t vrf_id) {
    put_value(data_, 2, TypeIpAddressBased);
    put_value(data_ + 2, 4, address);
    put_value(data_ + 6, 2, vrf_id);
}

RouteDistinguisher::RouteDistinguisher(bool is_bgpaas, uint32_t asn, uint16_t vmi_index) {
    put_value(data_, 2, Type4ByteASBased);
    put_value(data_ + 2, 4, asn);
    put_value(data_ + 6, 2, vmi_index);
}

RouteDistinguisher::RouteDistinguisher(uint16_t cluster_seed, uint32_t address,
                                       uint16_t vrf_id) {
    put_value(data_, 2, TypeIpAddressBased);
    put_value(data_ + 2, 2, cluster_seed);
    put_value(data_ + 4, 2, (address & 0xFFFF));
    put_value(data_ + 6, 2, vrf_id);
}

uint16_t RouteDistinguisher::Type() const {
    return get_value(data_, 2);
}

uint32_t RouteDistinguisher::GetAddress() const {
    return (Type() == TypeIpAddressBased ? get_value(data_ + 2, 4) : 0);
}

uint16_t RouteDistinguisher::GetVrfId() const {
    return (Type() == TypeIpAddressBased ? get_value(data_ + 6, 2) : 0);
}

string RouteDistinguisher::ToString() const {
    char temp[32];

    uint16_t rd_type = get_value(data_, 2);
    if (rd_type == Type2ByteASBased) {
        uint16_t asn = get_value(data_ + 2, 2);
        uint32_t value = get_value(data_ + 4, 4);
        snprintf(temp, sizeof(temp), "%u:%u", asn, value);
        return string(temp);
    } else if (rd_type == TypeIpAddressBased) {
        Ip4Address ip(get_value(data_ + 2, 4));
        uint16_t value = get_value(data_ + 6, 2);
        snprintf(temp, sizeof(temp), ":%u", value);
        return ip.to_string() + temp;
    } else if (rd_type == Type4ByteASBased) {
        uint32_t asn = get_value(data_ + 2, 4);
        uint16_t value = get_value(data_ + 6, 2);
        snprintf(temp, sizeof(temp), "%u:%u", asn, value);
        return string(temp);
    } else {
        snprintf(temp, sizeof(temp), "%u:%02x:%02x:%02x:%02x:%02x:%02x",
            rd_type, data_[2], data_[3], data_[4], data_[5], data_[6],
            data_[7]);
        return string(temp);
    }
}

RouteDistinguisher RouteDistinguisher::FromString(
    const string &str, boost::system::error_code *errorp) {
    RouteDistinguisher rd;
    size_t pos = str.rfind(':');
    if (pos == string::npos) {
        if (errorp != NULL) {
            *errorp = make_error_code(boost::system::errc::invalid_argument);
        }
        return RouteDistinguisher::kZeroRd;
    }

    boost::system::error_code ec;
    string first(str.substr(0, pos));
    Ip4Address addr = Ip4Address::from_string(first, ec);
    int offset;
    char *endptr;
    int32_t asn = -1;
    if (ec.value() != 0) {
        // Not an IP address, try ASN.
        asn = strtol(first.c_str(), &endptr, 10);
        if (asn >= 65535 || *endptr != '\0') {
            if (errorp != NULL) {
                *errorp =
                    make_error_code(boost::system::errc::invalid_argument);
            }
            return RouteDistinguisher::kZeroRd;
        }

        put_value(rd.data_, 2, 0);
        put_value(rd.data_ + 2, 2, asn);
        offset = 4;
    } else {
        put_value(rd.data_, 2, 1);
        put_value(rd.data_ + 2, 4, addr.to_ulong());
        offset = 6;
    }

    string second(str, pos + 1);
    uint64_t value = strtol(second.c_str(), &endptr, 10);
    if (*endptr != '\0') {
        if (errorp != NULL) {
            *errorp = make_error_code(boost::system::errc::invalid_argument);
        }
        return RouteDistinguisher::kZeroRd;
    }

    // ASN 0 is not allowed if the assigned number is not 0.
    if (asn == 0 && value != 0) {
        if (errorp != NULL) {
            *errorp = make_error_code(boost::system::errc::invalid_argument);
        }
        return RouteDistinguisher::kZeroRd;
    }

    // Check assigned number for type 0.
    if (offset == 4 && value > 0xFFFFFFFF) {
        if (errorp != NULL) {
            *errorp = make_error_code(boost::system::errc::invalid_argument);
        }
        return RouteDistinguisher::kZeroRd;
    }

    // Check assigned number for type 1.
    if (offset == 6 && value > 0xFFFF) {
        if (errorp != NULL) {
            *errorp = make_error_code(boost::system::errc::invalid_argument);
        }
        return RouteDistinguisher::kZeroRd;
    }

    put_value(rd.data_ + offset, 8 - offset, value);
    return rd;
}

int RouteDistinguisher::CompareTo(const RouteDistinguisher &rhs) const {
    return memcmp(data_, rhs.data_, kSize);
}
