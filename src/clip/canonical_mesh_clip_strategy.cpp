#include "clip_worker/clip/canonical_mesh_clip_strategy.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace clip_worker::clip {

CanonicalMeshClipResult CanonicalMeshClipStrategy::clip(
        mesh::MeshScene source,
        MeshSceneClipRequest request,
        const normalization::MeshResourceProfile& profile,
        const normalization::ToolVersion& validator) {
    request.authorization_limits = profile.authorization;
    auto clipped = MeshSceneClipper::clip(std::move(source), request);
    CanonicalMeshClipResult result;
    result.empty = clipped.empty;
    result.geometry = clipped.statistics;
    if (result.empty) return result;

    result.texture = TextureMasker::mask(
            clipped.scene, profile.texture_mask);
    result.canonical = normalization::MeshCanonicalWriter::write(
            std::move(clipped.scene));
    if (result.canonical.glb.size() > profile.maximum_output_bytes) {
        throw formats::FormatError(
                formats::FormatErrorCode::unsupported_content,
                "Clipped canonical mesh exceeds the configured output limit");
    }
    try {
        result.evidence = normalization::validateCanonicalGlb(
                result.canonical.glb,
                normalization::CanonicalFamily::mesh_gltf2,
                validator);
    } catch (const std::invalid_argument& error) {
        throw formats::FormatError(
                formats::FormatErrorCode::clipping_output_invalid,
                std::string("Clipped canonical mesh validation failed: ")
                        + error.what());
    }
    return result;
}

}  // namespace clip_worker::clip
