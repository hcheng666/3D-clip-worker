#pragma once

#include "clip_worker/geometry/authorization_scope.hpp"
#include "clip_worker/geometry/matrix4.hpp"
#include "clip_worker/mesh/mesh_scene.hpp"
#include "clip_worker/metadata/metadata_resource_limits.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace clip_worker::clip {

struct MeshSceneClipRequest {
    std::vector<std::uint8_t> scope_wkb;
    std::int32_t scope_srid = 4490;
    geometry::Matrix4 tileset_transform = geometry::Matrix4::identity();
    mesh::UpAxis gltf_up_axis = mesh::UpAxis::y;
    geometry::AuthorizationScopeLimits authorization_limits;
    std::optional<metadata::MetadataResourceLimits> metadata_limits;
};

struct MeshSceneClipStatistics {
    std::uint64_t input_vertices = 0U;
    std::uint64_t input_triangles = 0U;
    std::uint64_t output_vertices = 0U;
    std::uint64_t output_triangles = 0U;
    std::uint64_t clipped_mesh_instances = 0U;
    std::uint64_t reused_mesh_instances = 0U;
};

struct MeshSceneClipResult {
    mesh::MeshScene scene;
    MeshSceneClipStatistics statistics;
    bool empty = false;
};

/** Clips a typed mesh scene against the vertically unbounded authorization prism. */
class MeshSceneClipper final {
public:
    [[nodiscard]] static MeshSceneClipResult clip(
            mesh::MeshScene source, const MeshSceneClipRequest& request);
};

}  // namespace clip_worker::clip
