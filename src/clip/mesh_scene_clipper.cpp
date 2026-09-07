#include "clip_worker/clip/mesh_scene_clipper.hpp"

#include "clip_worker/metadata/legacy_feature_metadata.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/geometry/authorization_triangle_index.hpp"
#include "clip_worker/geometry/clip_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace clip_worker::clip {
namespace {

using geometry::ClippedTriangle;
using geometry::ClipVertex;

[[noreturn]] void invalid(const char* message) {
    throw formats::FormatError(formats::FormatErrorCode::invalid_accessor,
                               message);
}

struct AttributeLayout {
    std::optional<std::size_t> normal;
    std::optional<std::size_t> tangent;
    std::optional<std::size_t> texcoord_0;
    std::optional<std::size_t> texcoord_1;
    std::optional<std::size_t> color;
    std::size_t color_components = 0U;
    std::size_t values = 0U;
};

AttributeLayout attributeLayout(const mesh::MeshPrimitive& primitive) {
    AttributeLayout result;
    if (!primitive.normals.empty()) {
        result.normal = result.values;
        result.values += 3U;
    }
    if (!primitive.tangents.empty()) {
        result.tangent = result.values;
        result.values += 4U;
    }
    if (!primitive.texcoords_0.empty()) {
        result.texcoord_0 = result.values;
        result.values += 2U;
    }
    if (!primitive.texcoords_1.empty()) {
        result.texcoord_1 = result.values;
        result.values += 2U;
    }
    if (!primitive.colors.empty()) {
        result.color = result.values;
        result.color_components = primitive.color_components;
        result.values += primitive.color_components;
    }
    return result;
}

void appendValues(std::vector<double>& destination,
                  const std::vector<float>& source,
                  std::size_t vertex, std::size_t components) {
    const std::size_t offset = vertex * components;
    for (std::size_t component = 0U; component < components; ++component) {
        destination.push_back(source.at(offset + component));
    }
}

ClipVertex sourceVertex(const mesh::MeshPrimitive& primitive,
                        const AttributeLayout& layout,
                        std::uint32_t index,
                        const geometry::Matrix4& world_transform,
                        const geometry::AuthorizationScope& scope) {
    ClipVertex result;
    const std::size_t position = static_cast<std::size_t>(index) * 3U;
    result.local_position = {primitive.positions.at(position),
                             primitive.positions.at(position + 1U),
                             primitive.positions.at(position + 2U)};
    const auto projected = scope.projectEcef(
            world_transform.transformPoint(result.local_position));
    result.projected = projected.horizontal;
    result.projected_height = projected.height;
    result.attributes.reserve(layout.values);
    if (layout.normal.has_value()) appendValues(
            result.attributes, primitive.normals, index, 3U);
    if (layout.tangent.has_value()) appendValues(
            result.attributes, primitive.tangents, index, 4U);
    if (layout.texcoord_0.has_value()) appendValues(
            result.attributes, primitive.texcoords_0, index, 2U);
    if (layout.texcoord_1.has_value()) appendValues(
            result.attributes, primitive.texcoords_1, index, 2U);
    if (layout.color.has_value()) appendValues(
            result.attributes, primitive.colors, index,
            layout.color_components);
    return result;
}

void normalizeVector(std::array<float, 3>& values, const char* semantic) {
    const double length = std::sqrt(
            static_cast<double>(values[0]) * values[0]
            + static_cast<double>(values[1]) * values[1]
            + static_cast<double>(values[2]) * values[2]);
    if (!std::isfinite(length) || length <= 1.0e-12) {
        throw formats::FormatError(formats::FormatErrorCode::invalid_accessor,
                                   std::string(semantic)
                                           + " cannot be normalized after clipping");
    }
    for (float& value : values) value = static_cast<float>(value / length);
}

std::uint32_t floatBits(float value) {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct OutputVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 4> tangent{};
    std::array<float, 2> texcoord_0{};
    std::array<float, 2> texcoord_1{};
    std::array<float, 4> color{};
    std::uint32_t feature_id = 0U;
    std::vector<std::uint32_t> feature_ids;
};

OutputVertex outputVertex(const ClipVertex& source,
                          const AttributeLayout& layout,
                          std::uint32_t feature_id,
                          std::vector<std::uint32_t> feature_ids) {
    OutputVertex result;
    for (std::size_t component = 0U; component < 3U; ++component) {
        result.position[component] = static_cast<float>(
                source.local_position[component]);
    }
    if (layout.normal.has_value()) {
        for (std::size_t component = 0U; component < 3U; ++component)
            result.normal[component] = static_cast<float>(
                    source.attributes.at(*layout.normal + component));
        normalizeVector(result.normal, "NORMAL");
    }
    if (layout.tangent.has_value()) {
        std::array<float, 3> direction{};
        for (std::size_t component = 0U; component < 3U; ++component)
            direction[component] = static_cast<float>(
                    source.attributes.at(*layout.tangent + component));
        normalizeVector(direction, "TANGENT");
        std::copy(direction.begin(), direction.end(), result.tangent.begin());
        result.tangent[3U] = source.attributes.at(*layout.tangent + 3U) < 0.0
                ? -1.0F : 1.0F;
    }
    if (layout.texcoord_0.has_value()) {
        for (std::size_t component = 0U; component < 2U; ++component)
            result.texcoord_0[component] = static_cast<float>(
                    source.attributes.at(*layout.texcoord_0 + component));
    }
    if (layout.texcoord_1.has_value()) {
        for (std::size_t component = 0U; component < 2U; ++component)
            result.texcoord_1[component] = static_cast<float>(
                    source.attributes.at(*layout.texcoord_1 + component));
    }
    if (layout.color.has_value()) {
        for (std::size_t component = 0U; component < layout.color_components;
             ++component) {
            result.color[component] = static_cast<float>(
                    source.attributes.at(*layout.color + component));
        }
    }
    result.feature_id = feature_id;
    result.feature_ids = std::move(feature_ids);
    return result;
}

std::vector<std::uint32_t> vertexKey(const OutputVertex& vertex,
                                     const AttributeLayout& layout,
                                     bool has_feature,
                                     std::size_t feature_set_count) {
    std::vector<std::uint32_t> result;
    result.reserve(3U + layout.values + (has_feature ? 1U : 0U)
                   + feature_set_count);
    for (const float value : vertex.position) result.push_back(floatBits(value));
    if (layout.normal.has_value())
        for (const float value : vertex.normal) result.push_back(floatBits(value));
    if (layout.tangent.has_value())
        for (const float value : vertex.tangent) result.push_back(floatBits(value));
    if (layout.texcoord_0.has_value())
        for (const float value : vertex.texcoord_0) result.push_back(floatBits(value));
    if (layout.texcoord_1.has_value())
        for (const float value : vertex.texcoord_1) result.push_back(floatBits(value));
    if (layout.color.has_value()) {
        for (std::size_t component = 0U; component < layout.color_components;
             ++component) result.push_back(floatBits(vertex.color[component]));
    }
    if (has_feature) result.push_back(vertex.feature_id);
    result.insert(result.end(), vertex.feature_ids.begin(),
                  vertex.feature_ids.end());
    return result;
}

void appendOutputVertex(mesh::MeshPrimitive& output,
                        const OutputVertex& vertex,
                        const AttributeLayout& layout,
                        bool has_feature) {
    output.positions.insert(output.positions.end(),
                            vertex.position.begin(), vertex.position.end());
    if (layout.normal.has_value()) output.normals.insert(
            output.normals.end(), vertex.normal.begin(), vertex.normal.end());
    if (layout.tangent.has_value()) output.tangents.insert(
            output.tangents.end(), vertex.tangent.begin(), vertex.tangent.end());
    if (layout.texcoord_0.has_value()) output.texcoords_0.insert(
            output.texcoords_0.end(), vertex.texcoord_0.begin(),
            vertex.texcoord_0.end());
    if (layout.texcoord_1.has_value()) output.texcoords_1.insert(
            output.texcoords_1.end(), vertex.texcoord_1.begin(),
            vertex.texcoord_1.end());
    if (layout.color.has_value()) output.colors.insert(
            output.colors.end(), vertex.color.begin(),
            vertex.color.begin()
                    + static_cast<std::ptrdiff_t>(layout.color_components));
    if (has_feature) output.feature_ids.push_back(vertex.feature_id);
    if (vertex.feature_ids.size() != output.feature_id_sets.size()) {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_feature_id_invalid,
                "Clipped metadata feature set count changed");
    }
    for (std::size_t set = 0U; set < vertex.feature_ids.size(); ++set) {
        output.feature_id_sets.at(set).ids.push_back(vertex.feature_ids.at(set));
    }
}

mesh::MeshPrimitive clipPrimitive(
        const mesh::MeshPrimitive& source,
        const geometry::Matrix4& world_transform,
        const geometry::AuthorizationScope& scope,
        MeshSceneClipStatistics& statistics) {
    const AttributeLayout layout = attributeLayout(source);
    mesh::MeshPrimitive output;
    output.source_mode = mesh::PrimitiveMode::triangles;
    output.material = source.material;
    output.color_components = source.color_components;
    output.feature_id_sets = source.feature_id_sets;
    for (auto& set : output.feature_id_sets) set.ids.clear();
    const bool has_feature = !source.feature_ids.empty();
    std::map<std::vector<std::uint32_t>, std::uint32_t> vertices;
    geometry::AuthorizationTriangleIndex::QueryWorkspace workspace;
    statistics.input_vertices += source.vertexCount();
    statistics.input_triangles += source.triangleCount();
    for (std::size_t offset = 0U; offset < source.indices.size(); offset += 3U) {
        const std::array<std::uint32_t, 3> indices{
                source.indices[offset], source.indices[offset + 1U],
                source.indices[offset + 2U]};
        std::uint32_t feature_id = 0U;
        if (has_feature) {
            feature_id = source.feature_ids.at(indices[0U]);
            if (source.feature_ids.at(indices[1U]) != feature_id
                || source.feature_ids.at(indices[2U]) != feature_id) {
                invalid("A source triangle references multiple feature IDs");
            }
        }
        std::vector<std::uint32_t> feature_ids;
        feature_ids.reserve(source.feature_id_sets.size());
        for (const auto& set : source.feature_id_sets) {
            const std::uint32_t id = set.ids.at(indices[0U]);
            if (set.ids.at(indices[1U]) != id
                    || set.ids.at(indices[2U]) != id) {
                throw formats::FormatError(
                        formats::FormatErrorCode::metadata_feature_id_triangle_ambiguous,
                        "A source triangle references multiple modern feature IDs");
            }
            feature_ids.push_back(id);
        }
        const ClippedTriangle triangle{
                sourceVertex(source, layout, indices[0U], world_transform, scope),
                sourceVertex(source, layout, indices[1U], world_transform, scope),
                sourceVertex(source, layout, indices[2U], world_transform, scope)};
        scope.queryTriangles(triangle, workspace);
        const auto fragments = geometry::TriangleClipper::clip(
                triangle, workspace.triangles);
        for (const auto& fragment : fragments) {
            for (const auto& clipped : fragment) {
                const OutputVertex vertex = outputVertex(
                        clipped, layout, feature_id, feature_ids);
                auto key = vertexKey(vertex, layout, has_feature,
                                     source.feature_id_sets.size());
                const auto existing = vertices.find(key);
                if (existing != vertices.end()) {
                    output.indices.push_back(existing->second);
                    continue;
                }
                if (output.vertexCount()
                    >= std::numeric_limits<std::uint32_t>::max()) {
                    invalid("Clipped primitive exceeds uint32 vertex identity");
                }
                const auto index = static_cast<std::uint32_t>(output.vertexCount());
                appendOutputVertex(output, vertex, layout, has_feature);
                vertices.emplace(std::move(key), index);
                output.indices.push_back(index);
            }
        }
    }
    if (!output.positions.empty()) {
        mesh::validateMeshPrimitive(output);
        statistics.output_vertices += output.vertexCount();
        statistics.output_triangles += output.triangleCount();
    }
    return output;
}

mesh::Mesh clipMesh(const mesh::Mesh& source,
                    const geometry::Matrix4& world_transform,
                    const geometry::AuthorizationScope& scope,
                    MeshSceneClipStatistics& statistics) {
    mesh::Mesh output;
    for (const auto& primitive : source.primitives) {
        auto clipped = clipPrimitive(primitive, world_transform, scope, statistics);
        if (!clipped.positions.empty())
            output.primitives.push_back(std::move(clipped));
    }
    return output;
}

struct MeshInstanceKey {
    std::size_t mesh = 0U;
    std::array<double, 16> accumulated{};

    bool operator<(const MeshInstanceKey& other) const noexcept {
        if (mesh != other.mesh) return mesh < other.mesh;
        return accumulated < other.accumulated;
    }
};

}  // namespace

MeshSceneClipResult MeshSceneClipper::clip(
        mesh::MeshScene source, const MeshSceneClipRequest& request) {
    mesh::validateMeshScene(source);
    const auto scope = geometry::AuthorizationScope::fromWkb(
            request.scope_wkb, request.scope_srid,
            request.authorization_limits);
    MeshSceneClipResult result;
    result.scene = source;
    result.scene.meshes.clear();
    for (auto& node : result.scene.nodes) node.mesh.reset();
    std::map<MeshInstanceKey, std::optional<std::size_t>> cache;
    std::vector<std::uint8_t> state(source.nodes.size(), 0U);
    const geometry::Matrix4 content_transform = request.tileset_transform
            * mesh::upAxisToZTransform(request.gltf_up_axis);
    std::function<void(std::size_t, const geometry::Matrix4&)> visit =
            [&](std::size_t node_index, const geometry::Matrix4& parent) {
        if (state.at(node_index) == 1U) invalid("Mesh node graph contains a cycle");
        if (state.at(node_index) == 2U) invalid("Mesh node has multiple parents");
        state[node_index] = 1U;
        const auto& source_node = source.nodes.at(node_index);
        const geometry::Matrix4 accumulated = parent * source_node.local_transform;
        if (source_node.mesh.has_value()) {
            const MeshInstanceKey key{*source_node.mesh, accumulated.values()};
            const auto found = cache.find(key);
            if (found != cache.end()) {
                result.scene.nodes[node_index].mesh = found->second;
                ++result.statistics.reused_mesh_instances;
            } else {
                mesh::Mesh clipped = clipMesh(
                        source.meshes.at(*source_node.mesh),
                        content_transform * accumulated, scope,
                        result.statistics);
                std::optional<std::size_t> output_mesh;
                if (!clipped.primitives.empty()) {
                    output_mesh = result.scene.meshes.size();
                    result.scene.meshes.push_back(std::move(clipped));
                }
                cache.emplace(key, output_mesh);
                result.scene.nodes[node_index].mesh = output_mesh;
                ++result.statistics.clipped_mesh_instances;
            }
        }
        for (const std::size_t child : source_node.children)
            visit(child, accumulated);
        state[node_index] = 2U;
    };
    for (const std::size_t root : source.scenes.at(source.default_scene))
        visit(root, geometry::Matrix4::identity());
    result.empty = result.scene.meshes.empty();
    if (!result.empty && result.scene.feature_metadata.has_value()) {
        if (!request.metadata_limits.has_value()) {
            throw formats::FormatError(
                    formats::FormatErrorCode::metadata_reconstruction_unsafe,
                    "Mesh metadata compaction limits are absent");
        }
        std::vector<metadata::FeatureIdSet> modern_sets;
        for (const auto& output_mesh : result.scene.meshes) {
            for (const auto& primitive : output_mesh.primitives) {
                modern_sets.insert(modern_sets.end(),
                                   primitive.feature_id_sets.begin(),
                                   primitive.feature_id_sets.end());
            }
        }
        if (!modern_sets.empty()) {
            auto compacted = metadata::compactFeatureMetadata(
                    *result.scene.feature_metadata, modern_sets,
                    *request.metadata_limits);
            result.scene.feature_metadata = std::move(compacted.metadata);
            std::size_t set_index = 0U;
            for (auto& output_mesh : result.scene.meshes) {
                for (auto& primitive : output_mesh.primitives) {
                    for (auto& set : primitive.feature_id_sets) {
                        set = std::move(
                                compacted.feature_id_sets.at(set_index++));
                    }
                }
            }
        } else {
            std::set<std::uint32_t> retained;
            for (const auto& output_mesh : result.scene.meshes) {
                for (const auto& primitive : output_mesh.primitives) {
                    retained.insert(primitive.feature_ids.begin(),
                                    primitive.feature_ids.end());
                }
            }
            std::map<std::uint32_t, std::uint32_t> feature_mapping;
            for (const std::uint32_t source_id : retained) {
                feature_mapping[source_id] =
                        static_cast<std::uint32_t>(feature_mapping.size());
            }
            for (auto& output_mesh : result.scene.meshes) {
                for (auto& primitive : output_mesh.primitives) {
                    std::transform(
                            primitive.feature_ids.begin(),
                            primitive.feature_ids.end(),
                            primitive.feature_ids.begin(),
                            [&feature_mapping](std::uint32_t source_id) {
                                return feature_mapping.at(source_id);
                            });
                }
            }
            result.scene.feature_metadata =
                    metadata::compactLegacyFeatureMetadata(
                            *result.scene.feature_metadata,
                            std::vector<std::uint32_t>(retained.begin(),
                                                       retained.end()),
                            *request.metadata_limits);
        }
    }
    if (!result.empty) mesh::validateMeshScene(result.scene);
    return result;
}

}  // namespace clip_worker::clip
