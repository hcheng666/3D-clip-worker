#pragma once

#include "clip_worker/formats/gltf_mesh_reader.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "clip_worker/normalization/mesh_resource_profile.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::normalization {

enum class MeshSourceKind { glb, gltf, b3dm };

struct MeshNormalizationInput {
    MeshSourceKind source_kind = MeshSourceKind::glb;
    std::string root_package_relative_path;
    std::vector<std::uint8_t> source_bytes;
    formats::ApprovedGltfResourceMap approved_resources;
};

struct MeshNormalizationResult {
    MeshCanonicalWriteResult canonical;
    CanonicalArtifactEvidence evidence;
    formats::GltfMeshReadDiagnostics diagnostics;
};

/** Converts one approved mesh source closure into verified normalization-v2 GLB. */
class MeshNormalizer final {
public:
    [[nodiscard]] static MeshNormalizationResult normalize(
            const MeshNormalizationInput& input,
            const MeshResourceProfile& profile,
            const ToolVersion& validator);
};

}  // namespace clip_worker::normalization
