#include "clip_worker/normalization/mesh_normalizer.hpp"

#include "clip_worker/formats/b3dm_mesh_adapter.hpp"
#include "clip_worker/formats/format_error.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace clip_worker::normalization {

MeshNormalizationResult MeshNormalizer::normalize(
        const MeshNormalizationInput& input,
        const MeshResourceProfile& profile,
        const ToolVersion& validator) {
    if (input.source_bytes.empty()
        || input.source_bytes.size() > profile.maximum_input_bytes
        || input.root_package_relative_path.empty()) {
        throw formats::FormatError(
                formats::FormatErrorCode::unsupported_content,
                "Mesh normalization source identity or size is invalid");
    }
    mesh::MeshScene scene;
    formats::GltfMeshReadDiagnostics diagnostics;
    switch (input.source_kind) {
        case MeshSourceKind::glb: {
            auto decoded = formats::GltfMeshReader::read(
                    input.source_bytes, formats::GltfContentKind::glb,
                    input.root_package_relative_path,
                    input.approved_resources, profile.b3dm.gltf);
            scene = std::move(decoded.scene);
            diagnostics = decoded.diagnostics;
            break;
        }
        case MeshSourceKind::gltf: {
            auto decoded = formats::GltfMeshReader::read(
                    input.source_bytes, formats::GltfContentKind::gltf,
                    input.root_package_relative_path,
                    input.approved_resources, profile.b3dm.gltf);
            scene = std::move(decoded.scene);
            diagnostics = decoded.diagnostics;
            break;
        }
        case MeshSourceKind::b3dm: {
            auto decoded = formats::B3dmMeshAdapter::read(
                    input.source_bytes, profile.b3dm);
            scene = std::move(decoded.scene);
            diagnostics = decoded.gltf_diagnostics;
            break;
        }
    }
    MeshNormalizationResult result;
    result.canonical = MeshCanonicalWriter::write(std::move(scene));
    if (result.canonical.glb.size() > profile.maximum_output_bytes) {
        throw formats::FormatError(
                formats::FormatErrorCode::unsupported_content,
                "Canonical mesh output exceeds the configured limit");
    }
    try {
        result.evidence = validateCanonicalGlb(
                result.canonical.glb, CanonicalFamily::mesh_gltf2, validator);
    } catch (const std::invalid_argument& error) {
        throw formats::FormatError(
                formats::FormatErrorCode::normalization_output_invalid,
                std::string("Canonical mesh validation failed: ")
                        + error.what());
    }
    result.diagnostics = diagnostics;
    return result;
}

}  // namespace clip_worker::normalization
