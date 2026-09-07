#include "clip_worker/clip/canonical_instance_clip_strategy.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/metadata/legacy_feature_metadata.hpp"
#include "clip_worker/metadata/legacy_property_table.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace clip_worker::clip {
namespace {

geometry::Matrix4 instanceTransform(const mesh::MeshNodeInstancing& instances,
                                    std::size_t index) {
    return geometry::Matrix4::translation({
                   instances.translations[index * 3U],
                   instances.translations[index * 3U + 1U],
                   instances.translations[index * 3U + 2U]})
            * geometry::Matrix4::quaternion({
                   instances.rotations[index * 4U],
                   instances.rotations[index * 4U + 1U],
                   instances.rotations[index * 4U + 2U],
                   instances.rotations[index * 4U + 3U]})
            * geometry::Matrix4::scale({
                   instances.scales[index * 3U],
                   instances.scales[index * 3U + 1U],
                   instances.scales[index * 3U + 2U]});
}

instance::InstanceScene compactWhole(
        const instance::InstanceScene& source,
        const std::vector<std::uint32_t>& ordinals,
        const std::optional<metadata::MetadataResourceLimits>& metadata_limits) {
    instance::InstanceScene result = source;
    auto& output = *result.model.nodes.front().instancing;
    const auto& input = *source.model.nodes.front().instancing;
    output = {};
    output.translations.reserve(ordinals.size() * 3U);
    output.rotations.reserve(ordinals.size() * 4U);
    output.scales.reserve(ordinals.size() * 3U);
    std::vector<std::uint32_t> source_features;
    source_features.reserve(ordinals.size());
    for (const std::uint32_t ordinal : ordinals) {
        output.translations.insert(output.translations.end(),
                input.translations.begin() + static_cast<std::ptrdiff_t>(ordinal * 3U),
                input.translations.begin() + static_cast<std::ptrdiff_t>(ordinal * 3U + 3U));
        output.rotations.insert(output.rotations.end(),
                input.rotations.begin() + static_cast<std::ptrdiff_t>(ordinal * 4U),
                input.rotations.begin() + static_cast<std::ptrdiff_t>(ordinal * 4U + 4U));
        output.scales.insert(output.scales.end(),
                input.scales.begin() + static_cast<std::ptrdiff_t>(ordinal * 3U),
                input.scales.begin() + static_cast<std::ptrdiff_t>(ordinal * 3U + 3U));
        source_features.push_back(input.feature_ids.at(ordinal));
    }
    std::set<std::uint32_t> unique(source_features.begin(), source_features.end());
    std::vector<std::uint32_t> retained(unique.begin(), unique.end());
    std::map<std::uint32_t, std::uint32_t> remap;
    for (const std::uint32_t id : retained) {
        remap[id] = static_cast<std::uint32_t>(remap.size());
    }
    for (const std::uint32_t id : source_features) output.feature_ids.push_back(remap.at(id));
    if (source.model.legacy_properties.has_value()) {
        result.model.legacy_properties = metadata::compactLegacyPropertyTable(
                *source.model.legacy_properties, retained);
    }
    if (source.model.feature_metadata.has_value()) {
        if (!metadata_limits.has_value()) {
            throw formats::FormatError(
                    formats::FormatErrorCode::metadata_reconstruction_unsafe,
                    "Instance metadata compaction limits are absent");
        }
        result.model.feature_metadata = metadata::compactLegacyFeatureMetadata(
                *source.model.feature_metadata, retained, *metadata_limits);
    }
    instance::validateInstanceScene(result);
    return result;
}

mesh::MeshScene boundaryScene(
        const instance::InstanceScene& source,
        const std::vector<std::uint32_t>& ordinals,
        const InstanceExpansionLimits& limits) {
    const auto& base_mesh = source.model.meshes.at(0U);
    std::uint64_t vertices = 0U;
    std::uint64_t indices = 0U;
    for (const auto& primitive : base_mesh.primitives) {
        vertices += primitive.vertexCount();
        indices += primitive.indices.size();
    }
    if (ordinals.size() > limits.maximum_boundary_instances
        || (vertices > 0U
            && ordinals.size() > limits.maximum_expanded_vertices / vertices)
        || (indices > 0U
            && ordinals.size() > limits.maximum_expanded_indices / indices)) {
        throw formats::FormatError(
                formats::FormatErrorCode::instance_expansion_limit_exceeded,
                "I3DM boundary expansion exceeds configured limits");
    }
    mesh::MeshScene result;
    result.default_scene = 0U;
    result.scenes.resize(1U);
    result.materials = source.model.materials;
    result.samplers = source.model.samplers;
    result.textures = source.model.textures;
    result.images = source.model.images;
    result.legacy_properties = source.model.legacy_properties;
    result.feature_metadata = source.model.feature_metadata;
    result.require_unlit = source.model.require_unlit;
    const auto& instances = *source.model.nodes.front().instancing;
    for (const std::uint32_t ordinal : ordinals) {
        mesh::Mesh clone = base_mesh;
        const std::uint32_t feature_id = instances.feature_ids.at(ordinal);
        for (auto& primitive : clone.primitives) {
            primitive.feature_ids.assign(primitive.vertexCount(), feature_id);
        }
        const std::size_t mesh_index = result.meshes.size();
        result.meshes.push_back(std::move(clone));
        const std::size_t node_index = result.nodes.size();
        mesh::MeshNode node;
        node.source_ordinal = ordinal;
        node.local_transform = source.root_transform
                * instanceTransform(instances, ordinal);
        node.mesh = mesh_index;
        result.nodes.push_back(std::move(node));
        result.scenes.front().push_back(node_index);
    }
    mesh::validateMeshScene(result);
    return result;
}

}  // namespace

CanonicalInstanceClipResult CanonicalInstanceClipStrategy::clip(
        instance::InstanceScene source, MeshSceneClipRequest request,
        const normalization::MeshResourceProfile& mesh_profile,
        const normalization::ToolVersion& instance_validator,
        const normalization::ToolVersion& mesh_validator,
        const InstanceExpansionLimits& limits) {
    instance::validateInstanceScene(source);
    const auto authorization = geometry::AuthorizationScope::fromWkb(
            request.scope_wkb, request.scope_srid, mesh_profile.authorization);
    CanonicalInstanceClipResult result;
    result.relations = InstanceBoundsClassifier::classify(
            source, request.tileset_transform, authorization);
    std::vector<std::uint32_t> whole;
    std::vector<std::uint32_t> boundary;
    for (std::size_t index = 0U; index < result.relations.size(); ++index) {
        if (result.relations[index] == InstanceBoundsRelation::whole) {
            whole.push_back(static_cast<std::uint32_t>(index));
        } else if (result.relations[index] == InstanceBoundsRelation::boundary) {
            boundary.push_back(static_cast<std::uint32_t>(index));
        }
    }
    if (!boundary.empty()) {
        auto expanded = boundaryScene(source, boundary, limits);
        request.gltf_up_axis = mesh::UpAxis::y;
        auto clipped = CanonicalMeshClipStrategy::clip(
                std::move(expanded), request, mesh_profile, mesh_validator);
        if (!clipped.empty) result.boundary_mesh = std::move(clipped);
    }
    if (!whole.empty()) {
        result.whole_instances = normalization::InstanceCanonicalWriter::write(
                compactWhole(source, whole, request.metadata_limits),
                instance_validator);
    }
    return result;
}

}  // namespace clip_worker::clip
