#pragma once

#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/metadata/legacy_property_table.hpp"
#include "clip_worker/metadata/metadata_resource_limits.hpp"
#include "clip_worker/point/point_scene.hpp"

#include <cstdint>

namespace clip_worker::normalization {

struct PointResourceLimits {
    std::uint64_t maximum_points = 10000000ULL;
    std::uint64_t maximum_decoded_bytes = 1073741824ULL;
    metadata::LegacyPropertyLimits metadata;
    bool enable_feature_metadata = false;
    metadata::MetadataResourceLimits feature_metadata;
};

/** Strict PNTS-to-typed-point normalization. */
class PointNormalizer final {
public:
    explicit PointNormalizer(PointResourceLimits limits = {});

    [[nodiscard]] point::PointScene normalize(formats::ByteView source) const;

private:
    PointResourceLimits limits_;
};

}  // namespace clip_worker::normalization
