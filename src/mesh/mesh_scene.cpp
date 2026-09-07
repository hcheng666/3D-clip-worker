#include "clip_worker/mesh/mesh_scene.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace clip_worker::mesh {
namespace {

using formats::FormatError;
using formats::FormatErrorCode;

[[noreturn]] void invalidScene(const std::string& message) {
    throw FormatError(FormatErrorCode::invalid_accessor, message);
}

void validateFiniteStream(const std::vector<float>& values, const char* semantic) {
    if (std::any_of(values.begin(), values.end(), [](float value) {
            return !std::isfinite(value);
        })) {
        invalidScene(std::string(semantic) + " contains a non-finite value");
    }
}

void validateOptionalStream(const std::vector<float>& values,
                            std::size_t vertex_count,
                            std::size_t components,
                            const char* semantic) {
    if (!values.empty() && values.size() != vertex_count * components) {
        invalidScene(std::string(semantic) + " count differs from POSITION");
    }
    validateFiniteStream(values, semantic);
}

Bounds3 boundsForPositions(const std::vector<float>& positions) {
    Bounds3 result;
    if (positions.empty()) {
        return result;
    }
    result.minimum = {positions[0U], positions[1U], positions[2U]};
    result.maximum = result.minimum;
    for (std::size_t offset = 0U; offset < positions.size(); offset += 3U) {
        for (std::size_t component = 0U; component < 3U; ++component) {
            const double value = positions[offset + component];
            result.minimum[component] = std::min(result.minimum[component], value);
            result.maximum[component] = std::max(result.maximum[component], value);
        }
    }
    result.valid = true;
    return result;
}

void mergeBounds(Bounds3& target, const Bounds3& source) {
    if (!source.valid) {
        return;
    }
    if (!target.valid) {
        target = source;
        return;
    }
    for (std::size_t component = 0U; component < 3U; ++component) {
        target.minimum[component] = std::min(target.minimum[component],
                                             source.minimum[component]);
        target.maximum[component] = std::max(target.maximum[component],
                                             source.maximum[component]);
    }
}

}  // namespace

std::size_t MeshPrimitive::vertexCount() const noexcept {
    return positions.size() / 3U;
}

std::size_t MeshPrimitive::triangleCount() const noexcept {
    return indices.empty() ? vertexCount() / 3U : indices.size() / 3U;
}

MeshResourceAccountant::MeshResourceAccountant(MeshResourceLimits limits)
    : limits_(limits) {}

void MeshResourceAccountant::reserve(
        std::uint64_t count, std::uint64_t limit, std::uint64_t& current,
        const char* resource) {
    if (count > limit || current > limit - count) {
        throw FormatError(FormatErrorCode::unsupported_content,
                          std::string("Mesh resource limit exceeded: ") + resource);
    }
    current += count;
}

void MeshResourceAccountant::reserveVertices(std::uint64_t count) {
    reserve(count, limits_.maximum_vertices, vertices_, "vertices");
}

void MeshResourceAccountant::reserveIndices(std::uint64_t count) {
    reserve(count, limits_.maximum_indices, indices_, "indices");
}

void MeshResourceAccountant::reserveDecodedBytes(std::uint64_t count) {
    reserve(count, limits_.maximum_decoded_bytes, decoded_bytes_, "decoded bytes");
}

void MeshResourceAccountant::reserveTexturePixels(std::uint64_t count) {
    reserve(count, limits_.maximum_texture_pixels, texture_pixels_, "texture pixels");
}

void validateMeshPrimitive(MeshPrimitive& primitive) {
    if (primitive.positions.empty()
        || primitive.positions.size() % 3U != 0U) {
        invalidScene("POSITION stream is empty or misaligned");
    }
    validateFiniteStream(primitive.positions, "POSITION");
    const std::size_t vertex_count = primitive.vertexCount();
    validateOptionalStream(primitive.normals, vertex_count, 3U, "NORMAL");
    validateOptionalStream(primitive.tangents, vertex_count, 4U, "TANGENT");
    validateOptionalStream(primitive.texcoords_0, vertex_count, 2U,
                           "TEXCOORD_0");
    validateOptionalStream(primitive.texcoords_1, vertex_count, 2U,
                           "TEXCOORD_1");
    if (!primitive.colors.empty()
        && primitive.color_components != 3U
        && primitive.color_components != 4U) {
        invalidScene("COLOR_0 component count must be three or four");
    }
    validateOptionalStream(primitive.colors, vertex_count,
                           primitive.color_components == 0U
                                   ? 1U : primitive.color_components,
                           "COLOR_0");
    if (!primitive.feature_ids.empty()
        && primitive.feature_ids.size() != vertex_count) {
        invalidScene("Feature ID count differs from POSITION");
    }
    if (!primitive.feature_ids.empty() && !primitive.feature_id_sets.empty()) {
        invalidScene("Legacy and modern feature ID streams are mixed");
    }
    std::set<std::string> feature_labels;
    for (const auto& set : primitive.feature_id_sets) {
        if ((!set.label.empty() && !feature_labels.insert(set.label).second)
                || set.feature_count == 0U || set.ids.size() != vertex_count
                || std::any_of(set.ids.begin(), set.ids.end(),
                               [&set](std::uint32_t id) {
                                   return id >= set.feature_count
                                           && (!set.null_feature_id.has_value()
                                               || id
                                                       != *set.null_feature_id);
                               })) {
            throw FormatError(
                    FormatErrorCode::metadata_feature_id_invalid,
                    "Modern feature ID stream is invalid");
        }
        for (std::size_t index = 0U; index < primitive.indices.size();
             index += 3U) {
            const std::uint32_t first = set.ids.at(primitive.indices[index]);
            if (set.ids.at(primitive.indices[index + 1U]) != first
                    || set.ids.at(primitive.indices[index + 2U]) != first) {
                throw FormatError(
                        FormatErrorCode::metadata_feature_id_triangle_ambiguous,
                        "Triangle vertices use different feature IDs");
            }
        }
    }
    if (primitive.indices.empty()
        || primitive.indices.size() % 3U != 0U) {
        invalidScene("Canonical primitive indices are empty or misaligned");
    }
    if (std::any_of(primitive.indices.begin(), primitive.indices.end(),
                    [vertex_count](std::uint32_t index) {
                        return index >= vertex_count;
                    })) {
        invalidScene("Canonical primitive index is out of range");
    }
    primitive.bounds = boundsForPositions(primitive.positions);
}

void validateMeshScene(MeshScene& scene) {
    if (scene.scenes.empty() || scene.default_scene >= scene.scenes.size()) {
        invalidScene("Mesh scene has no valid default scene");
    }
    for (const auto& roots : scene.scenes) {
        for (const std::size_t root : roots) {
            if (root >= scene.nodes.size()) {
                invalidScene("Mesh scene root node is out of range");
            }
        }
    }
    for (const auto& node : scene.nodes) {
        if (node.mesh.has_value() && *node.mesh >= scene.meshes.size()) {
            invalidScene("Mesh node references an out-of-range mesh");
        }
        for (const std::size_t child : node.children) {
            if (child >= scene.nodes.size()) {
                invalidScene("Mesh node child is out of range");
            }
        }
        if (node.instancing.has_value()) {
            const auto& instances = *node.instancing;
            const std::size_t count = instances.instanceCount();
            if (!node.mesh.has_value() || count == 0U
                || instances.translations.size() != count * 3U
                || instances.rotations.size() != count * 4U
                || instances.scales.size() != count * 3U
                || (!instances.feature_ids.empty()
                    && instances.feature_ids.size() != count)) {
                invalidScene("Mesh node instancing streams are invalid");
            }
            for (std::size_t index = 0U; index < count; ++index) {
                const double qx = instances.rotations[index * 4U];
                const double qy = instances.rotations[index * 4U + 1U];
                const double qz = instances.rotations[index * 4U + 2U];
                const double qw = instances.rotations[index * 4U + 3U];
                const double length = std::sqrt(
                        qx * qx + qy * qy + qz * qz + qw * qw);
                if (!std::isfinite(length) || std::abs(length - 1.0) > 1.0e-4
                    || instances.scales[index * 3U] <= 0.0F
                    || instances.scales[index * 3U + 1U] <= 0.0F
                    || instances.scales[index * 3U + 2U] <= 0.0F) {
                    invalidScene("Mesh node instance transform is invalid");
                }
            }
            if (std::any_of(instances.translations.begin(),
                            instances.translations.end(),
                            [](float value) { return !std::isfinite(value); })
                || std::any_of(instances.scales.begin(), instances.scales.end(),
                               [](float value) { return !std::isfinite(value); })) {
                invalidScene("Mesh node instance transform is non-finite");
            }
        }
    }
    for (auto& mesh : scene.meshes) {
        mesh.bounds = {};
        for (auto& primitive : mesh.primitives) {
            validateMeshPrimitive(primitive);
            if (primitive.material.has_value()
                && *primitive.material >= scene.materials.size()) {
                invalidScene("Primitive material reference is out of range");
            }
            mergeBounds(mesh.bounds, primitive.bounds);
        }
    }
    for (const auto& texture : scene.textures) {
        if (texture.image >= scene.images.size()
            || (texture.sampler.has_value()
                && *texture.sampler >= scene.samplers.size())) {
            invalidScene("Texture reference is out of range");
        }
    }
    for (const auto& image : scene.images) {
        const std::uint64_t pixels = static_cast<std::uint64_t>(image.width)
                                     * image.height;
        if (image.width == 0U || image.height == 0U
            || pixels > std::numeric_limits<std::size_t>::max() / 4U
            || image.pixels.size() != static_cast<std::size_t>(pixels * 4U)) {
            invalidScene("RGBA image dimensions do not match its pixel stream");
        }
    }
    if (scene.legacy_properties.has_value()) {
        const auto& table = *scene.legacy_properties;
        for (const auto& column : table.columns) {
            if (column.name.empty() || column.components == 0U
                || column.components > 4U) {
                invalidScene("Legacy property column identity is invalid");
            }
            const std::size_t value_count = static_cast<std::size_t>(
                    table.feature_count) * column.components;
            switch (column.kind) {
                case LegacyPropertyKind::numeric:
                    if (column.numeric_values.size() != value_count
                        || !column.boolean_values.empty()
                        || !column.string_values.empty()
                        || std::any_of(column.numeric_values.begin(),
                                       column.numeric_values.end(),
                                       [](double value) {
                                           return !std::isfinite(value);
                                       })) {
                        invalidScene("Legacy numeric property values are invalid");
                    }
                    break;
                case LegacyPropertyKind::boolean:
                    if (column.components != 1U
                        || column.boolean_values.size() != table.feature_count
                        || !column.numeric_values.empty()
                        || !column.string_values.empty()
                        || std::any_of(column.boolean_values.begin(),
                                       column.boolean_values.end(),
                                       [](std::uint8_t value) {
                                           return value > 1U;
                                       })) {
                        invalidScene("Legacy boolean property values are invalid");
                    }
                    break;
                case LegacyPropertyKind::string:
                    if (column.components != 1U
                        || column.string_values.size() != table.feature_count
                        || !column.numeric_values.empty()
                        || !column.boolean_values.empty()) {
                        invalidScene("Legacy string property values are invalid");
                    }
                    break;
            }
        }
    }
    if (scene.feature_metadata.has_value()) {
        if (scene.feature_metadata->primary_property_table.has_value()
                && *scene.feature_metadata->primary_property_table
                        >= scene.feature_metadata->property_tables.size()) {
            throw FormatError(
                    FormatErrorCode::metadata_property_table_invalid,
                    "Primary metadata property table is invalid");
        }
        if (scene.feature_metadata->primary_property_table.has_value()) {
            const std::uint32_t row_count = scene.feature_metadata
                    ->property_tables.at(
                            *scene.feature_metadata->primary_property_table)
                    .row_count;
            for (const auto& mesh : scene.meshes) {
                for (const auto& primitive : mesh.primitives) {
                    for (const std::uint32_t id : primitive.feature_ids) {
                        if (id >= row_count) {
                            throw FormatError(
                                    FormatErrorCode::metadata_feature_id_invalid,
                                    "Feature ID exceeds primary metadata rows");
                        }
                    }
                }
            }
            for (const auto& node : scene.nodes) {
                if (!node.instancing.has_value()) continue;
                for (const std::uint32_t id
                        : node.instancing->feature_ids) {
                    if (id >= row_count) {
                        throw FormatError(
                                FormatErrorCode::metadata_feature_id_invalid,
                                "Instance feature ID exceeds metadata rows");
                    }
                }
            }
        }
        for (const auto& mesh : scene.meshes) {
            for (const auto& primitive : mesh.primitives) {
                for (const auto& set : primitive.feature_id_sets) {
                    if (set.property_table.has_value()
                            && (*set.property_table
                                        >= scene.feature_metadata
                                                   ->property_tables.size()
                                || set.feature_count
                                        > scene.feature_metadata
                                                  ->property_tables
                                                  .at(*set.property_table)
                                                  .row_count)) {
                        throw FormatError(
                                FormatErrorCode::metadata_feature_id_invalid,
                                "Modern feature ID property table is invalid");
                    }
                }
            }
        }
    }
}

geometry::Matrix4 upAxisToZTransform(UpAxis axis) {
    switch (axis) {
        case UpAxis::x:
            // X-up to Z-up: (x, y, z) -> (-z, y, x).
            return geometry::Matrix4::fromColumnMajor({
                    0.0, 0.0, 1.0, 0.0,
                    0.0, 1.0, 0.0, 0.0,
                    -1.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0, 1.0});
        case UpAxis::y:
            // glTF Y-up to 3D Tiles Z-up: (x, y, z) -> (x, -z, y).
            return geometry::Matrix4::fromColumnMajor({
                    1.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0,
                    0.0, -1.0, 0.0, 0.0,
                    0.0, 0.0, 0.0, 1.0});
        case UpAxis::z:
            return geometry::Matrix4::identity();
    }
    throw std::invalid_argument("Unknown mesh up-axis enum value");
}

}  // namespace clip_worker::mesh
