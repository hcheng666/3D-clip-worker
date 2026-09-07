#pragma once

#include "clip_worker/geometry/authorization_scope.hpp"
#include "clip_worker/geometry/matrix4.hpp"
#include "clip_worker/point/point_scene.hpp"
#include "clip_worker/metadata/metadata_resource_limits.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace clip_worker::clip {

struct PointClipResult {
    std::optional<point::PointScene> scene;
    std::vector<std::uint32_t> retained_point_ordinals;
    std::vector<std::uint32_t> retained_source_feature_ids;
};

/** Exact point-membership clipper with aligned stream and property compaction. */
class PointSceneClipper final {
public:
    [[nodiscard]] static PointClipResult clip(
            point::PointScene source,
            const geometry::Matrix4& tile_to_ecef,
            const geometry::AuthorizationScope& authorization,
            std::optional<metadata::MetadataResourceLimits> metadata_limits =
                    std::nullopt);
};

}  // namespace clip_worker::clip
