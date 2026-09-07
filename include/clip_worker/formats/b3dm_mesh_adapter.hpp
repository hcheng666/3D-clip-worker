#pragma once

#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/formats/gltf_mesh_reader.hpp"
#include "clip_worker/mesh/mesh_scene.hpp"
#include "clip_worker/metadata/metadata_resource_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::formats {

struct B3dmMetadataLimits {
    std::uint64_t maximum_feature_rows = 10000000ULL;
    std::size_t maximum_properties = 4096U;
    std::uint64_t maximum_string_bytes = 67108864ULL;
    std::uint64_t maximum_binary_bytes = 268435456ULL;
};

struct B3dmMeshAdapterLimits {
    GltfMeshReaderLimits gltf;
    B3dmMetadataLimits metadata;
    bool enable_feature_metadata = false;
    metadata::MetadataResourceLimits feature_metadata;
};

struct B3dmMeshReadResult {
    mesh::MeshScene scene;
    B3dmLayoutDiagnostics layout;
    GltfMeshReadDiagnostics gltf_diagnostics;
    std::optional<std::array<double, 3>> rtc_center;
};

/** Converts a bounded B3DM v1 payload into the shared typed mesh scene. */
class B3dmMeshAdapter final {
public:
    [[nodiscard]] static B3dmMeshReadResult read(
            const std::vector<std::uint8_t>& source_bytes,
            const B3dmMeshAdapterLimits& limits = {});
};

}  // namespace clip_worker::formats
