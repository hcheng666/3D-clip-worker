#pragma once

#include "clip_worker/geometry/matrix4.hpp"
#include "clip_worker/mesh/mesh_scene.hpp"
#include "clip_worker/metadata/feature_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace clip_worker::point {

struct PointScene {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<std::uint8_t> colors_rgba;
    std::vector<std::uint32_t> feature_ids;
    std::optional<mesh::LegacyPropertyTable> legacy_properties;
    std::optional<metadata::FeatureMetadata> feature_metadata;
    geometry::Matrix4 root_transform = geometry::Matrix4::identity();
    mesh::Bounds3 bounds;

    [[nodiscard]] std::size_t pointCount() const noexcept {
        return positions.size() / 3U;
    }
};

/** Validates aligned streams, finite/unit values, feature rows, and bounds. */
void validatePointScene(PointScene& scene);

}  // namespace clip_worker::point
