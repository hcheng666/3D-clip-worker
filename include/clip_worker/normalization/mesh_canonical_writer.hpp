#pragma once

#include "clip_worker/mesh/mesh_scene.hpp"

#include <cstdint>
#include <vector>

namespace clip_worker::normalization {

inline constexpr const char* kMeshCanonicalWriterVersion =
        "mesh-canonical-writer-v1";

struct MeshCanonicalWriteResult {
    std::vector<std::uint8_t> glb;
    std::uint64_t vertex_count = 0U;
    std::uint64_t triangle_count = 0U;
    std::uint64_t texture_bytes = 0U;
};

/** Writes a typed scene as deterministic, embedded-resource MESH_GLTF2 GLB. */
class MeshCanonicalWriter final {
public:
    [[nodiscard]] static MeshCanonicalWriteResult write(mesh::MeshScene scene);
};

}  // namespace clip_worker::normalization
