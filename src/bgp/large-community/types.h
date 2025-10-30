#ifndef SRC_BGP_LARGE_COMMUNITY_TYPES_H_
#define SRC_BGP_LARGE_COMMUNITY_TYPES_H_

/// Defines constants for BGP Large Community attribute subtypes.
struct BgpLargeCommunityType {
    /// Large Community subtype identifiers.
    enum type {
        /// Large Community Tag.
        TagLC = 0x01,
    };
};

#endif // SRC_BGP_LARGE_COMMUNITY_TYPES_H_
