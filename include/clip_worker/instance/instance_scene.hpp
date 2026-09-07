#pragma once

#include "clip_worker/mesh/mesh_scene.hpp"

#include <cstddef>
#include <cstdint>

namespace clip_worker::instance {

/** Canonical shared model plus aligned instance attributes. */
struct InstanceScene {
    mesh::MeshScene model;
    geometry::Matrix4 root_transform = geometry::Matrix4::identity();

    [[nodiscard]] std::size_t instanceCount() const noexcept;
};

/** Validates the single flattened model node and all instance streams. */
void validateInstanceScene(InstanceScene& scene);

/** Returns a MeshScene ready for deterministic canonical instance writing. */
[[nodiscard]] mesh::MeshScene toInstancedMeshScene(InstanceScene scene);

}  // namespace clip_worker::instance
