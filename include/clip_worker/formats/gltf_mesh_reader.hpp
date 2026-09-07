#pragma once

#include "clip_worker/formats/draco_decoder.hpp"
#include "clip_worker/formats/meshopt_decoder.hpp"
#include "clip_worker/formats/texture_codec.hpp"
#include "clip_worker/mesh/mesh_scene.hpp"
#include "clip_worker/metadata/metadata_resource_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace clip_worker::formats {

enum class GltfContentKind {
    glb,
    gltf,
};

struct ApprovedGltfResource {
    std::vector<std::uint8_t> bytes;
    std::string media_type;
};

using ApprovedGltfResourceMap = std::map<std::string, ApprovedGltfResource>;

struct GltfMeshReaderLimits {
    mesh::MeshResourceLimits mesh;
    TextureDecodeLimits texture;
    DracoDecodeLimits draco;
    MeshoptDecodeLimits meshopt;
    std::uint64_t maximum_data_uri_bytes = 67108864ULL;
    std::size_t maximum_buffers = 4096U;
    std::size_t maximum_buffer_views = 100000U;
    std::size_t maximum_accessors = 100000U;
    std::size_t maximum_nodes = 1000000U;
    std::size_t maximum_meshes = 100000U;
    std::size_t maximum_primitives = 1000000U;
    std::size_t maximum_images = 100000U;
    bool enable_feature_metadata = false;
    /** Only canonical INSTANCE_GLTF2 readers may enable this extension set. */
    bool enable_gpu_instancing = false;
    metadata::MetadataResourceLimits metadata;
};

struct GltfMeshReadDiagnostics {
    std::size_t draco_primitive_count = 0U;
    std::size_t draco_count_compatibility_count = 0U;
    std::size_t meshopt_buffer_view_count = 0U;
    std::size_t external_resource_count = 0U;
    std::size_t data_uri_count = 0U;
};

struct GltfMeshReadResult {
    mesh::MeshScene scene;
    GltfMeshReadDiagnostics diagnostics;
};

/**
 * Decodes direct GLB/glTF into the typed scene using only the approved resource
 * map. URI resolution is canonical and never performs filesystem or network I/O.
 */
class GltfMeshReader final {
public:
    [[nodiscard]] static GltfMeshReadResult read(
            const std::vector<std::uint8_t>& root_bytes,
            GltfContentKind kind,
            const std::string& root_package_relative_path,
            const ApprovedGltfResourceMap& approved_resources,
            const GltfMeshReaderLimits& limits = {});
};

}  // namespace clip_worker::formats
