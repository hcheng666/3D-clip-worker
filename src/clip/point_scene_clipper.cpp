#include "clip_worker/clip/point_scene_clipper.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/metadata/legacy_feature_metadata.hpp"
#include "clip_worker/metadata/legacy_property_table.hpp"
#include "clip_worker/mesh/mesh_scene.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace clip_worker::clip {
namespace {

template <typename T>
std::vector<T> compactStream(const std::vector<T>& source,
                             std::size_t components,
                             const std::vector<std::uint32_t>& ordinals) {
    if (source.empty()) return {};
    std::vector<T> result;
    result.reserve(ordinals.size() * components);
    for (const std::uint32_t ordinal : ordinals) {
        const std::size_t begin = static_cast<std::size_t>(ordinal) * components;
        result.insert(result.end(),
                      source.begin() + static_cast<std::ptrdiff_t>(begin),
                      source.begin() + static_cast<std::ptrdiff_t>(
                              begin + components));
    }
    return result;
}

}  // namespace

PointClipResult PointSceneClipper::clip(
        point::PointScene source, const geometry::Matrix4& tile_to_ecef,
        const geometry::AuthorizationScope& authorization,
        std::optional<metadata::MetadataResourceLimits> metadata_limits) {
    point::validatePointScene(source);
    PointClipResult result;
    const geometry::Matrix4 canonical_to_ecef =
            tile_to_ecef * mesh::upAxisToZTransform(mesh::UpAxis::y)
            * source.root_transform;
    geometry::AuthorizationTriangleIndex::QueryWorkspace workspace;
    result.retained_point_ordinals.reserve(source.pointCount());
    for (std::size_t ordinal = 0U; ordinal < source.pointCount(); ++ordinal) {
        const std::array<double, 3U> local{
                source.positions[ordinal * 3U],
                source.positions[ordinal * 3U + 1U],
                source.positions[ordinal * 3U + 2U]};
        if (authorization.coversEcef(
                    canonical_to_ecef.transformPoint(local), workspace)) {
            result.retained_point_ordinals.push_back(
                    static_cast<std::uint32_t>(ordinal));
        }
    }
    if (result.retained_point_ordinals.empty()) return result;

    point::PointScene clipped;
    clipped.positions = compactStream(
            source.positions, 3U, result.retained_point_ordinals);
    clipped.normals = compactStream(
            source.normals, 3U, result.retained_point_ordinals);
    clipped.colors_rgba = compactStream(
            source.colors_rgba, 4U, result.retained_point_ordinals);
    clipped.root_transform = source.root_transform;

    const auto retained_features = compactStream(
            source.feature_ids, 1U, result.retained_point_ordinals);
    if (!retained_features.empty()) {
        std::set<std::uint32_t> unique(
                retained_features.begin(), retained_features.end());
        result.retained_source_feature_ids.assign(unique.begin(), unique.end());
        std::map<std::uint32_t, std::uint32_t> remap;
        for (const std::uint32_t source_id : result.retained_source_feature_ids) {
            remap[source_id] = static_cast<std::uint32_t>(remap.size());
        }
        clipped.feature_ids.reserve(retained_features.size());
        for (const std::uint32_t source_id : retained_features) {
            clipped.feature_ids.push_back(remap.at(source_id));
        }
        if (source.legacy_properties.has_value()) {
            clipped.legacy_properties = metadata::compactLegacyPropertyTable(
                    *source.legacy_properties,
                    result.retained_source_feature_ids);
        }
        if (source.feature_metadata.has_value()) {
            if (!metadata_limits.has_value()) {
                throw formats::FormatError(
                        formats::FormatErrorCode::metadata_reconstruction_unsafe,
                        "Point metadata compaction limits are absent");
            }
            clipped.feature_metadata = metadata::compactLegacyFeatureMetadata(
                    *source.feature_metadata,
                    result.retained_source_feature_ids, *metadata_limits);
        }
    }
    point::validatePointScene(clipped);
    result.scene = std::move(clipped);
    return result;
}

}  // namespace clip_worker::clip
