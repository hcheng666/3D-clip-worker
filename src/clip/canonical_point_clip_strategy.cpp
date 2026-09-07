#include "clip_worker/clip/canonical_point_clip_strategy.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace clip_worker::clip {

CanonicalPointClipResult CanonicalPointClipStrategy::clip(
        point::PointScene source,
        const geometry::Matrix4& tile_to_ecef,
        const geometry::AuthorizationScope& authorization,
        const metadata::MetadataResourceLimits& metadata_limits,
        const normalization::ToolVersion& validator,
        std::uint64_t maximum_output_bytes) {
    CanonicalPointClipResult result;
    result.input_point_count = source.pointCount();
    auto clipped = PointSceneClipper::clip(
            std::move(source), tile_to_ecef, authorization,
            metadata_limits);
    if (!clipped.scene.has_value()) {
        result.empty = true;
        return result;
    }
    result.output_point_count = clipped.scene->pointCount();
    result.canonical = normalization::PointCanonicalWriter::write(
            std::move(*clipped.scene));
    if (maximum_output_bytes == 0U
            || result.canonical.glb.size() > maximum_output_bytes) {
        throw formats::FormatError(
                formats::FormatErrorCode::point_output_invalid,
                "Clipped canonical point output exceeds the configured limit");
    }
    try {
        result.evidence = normalization::validateCanonicalGlb(
                result.canonical.glb,
                normalization::CanonicalFamily::point_gltf2,
                validator);
    } catch (const std::invalid_argument& error) {
        throw formats::FormatError(
                formats::FormatErrorCode::point_output_invalid,
                std::string("Clipped canonical point validation failed: ")
                        + error.what());
    }
    return result;
}

}  // namespace clip_worker::clip
