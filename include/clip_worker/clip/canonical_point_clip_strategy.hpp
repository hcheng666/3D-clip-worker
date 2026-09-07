#pragma once

#include "clip_worker/clip/point_scene_clipper.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/point_canonical_writer.hpp"

#include <cstdint>

namespace clip_worker::clip {

struct CanonicalPointClipResult {
    bool empty = false;
    std::uint64_t input_point_count = 0U;
    std::uint64_t output_point_count = 0U;
    normalization::PointCanonicalWriteResult canonical;
    normalization::CanonicalArtifactEvidence evidence;
};

/** Exact point clip followed by deterministic canonical GLB validation. */
class CanonicalPointClipStrategy final {
public:
    [[nodiscard]] static CanonicalPointClipResult clip(
            point::PointScene source,
            const geometry::Matrix4& tile_to_ecef,
            const geometry::AuthorizationScope& authorization,
            const metadata::MetadataResourceLimits& metadata_limits,
            const normalization::ToolVersion& validator,
            std::uint64_t maximum_output_bytes);
};

}  // namespace clip_worker::clip
