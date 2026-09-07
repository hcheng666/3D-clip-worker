#include "clip_worker/normalization/mesh_canonical_writer.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/glb.hpp"
#include "clip_worker/formats/texture_codec.hpp"
#include "clip_worker/normalization/metadata_canonical_writer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kArrayBufferTarget = 34962U;
constexpr std::uint32_t kElementArrayBufferTarget = 34963U;
constexpr std::uint32_t kComponentUnsignedShort = 5123U;
constexpr std::uint32_t kComponentUnsignedInt = 5125U;
constexpr std::uint32_t kComponentFloat = 5126U;
constexpr std::uint32_t kTrianglesMode = 4U;
constexpr const char* kUnlitExtension = "KHR_materials_unlit";
constexpr const char* kMeshFeaturesExtension = "EXT_mesh_features";
constexpr const char* kGpuInstancingExtension = "EXT_mesh_gpu_instancing";
constexpr const char* kInstanceFeaturesExtension = "EXT_instance_features";
constexpr const char* kStructuralMetadataExtension = "EXT_structural_metadata";
constexpr const char* kLegacyClassName = "legacyFeature";

[[noreturn]] void invalidOutput(const std::string& message) {
    throw formats::FormatError(formats::FormatErrorCode::invalid_accessor,
                               message);
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        output.push_back(static_cast<std::uint8_t>(
                (value >> (byte * 8U)) & 0xffU));
    }
}

class BinaryBuilder final {
public:
    std::size_t addFloatAccessor(const std::vector<float>& values,
                                 std::size_t components,
                                 const char* type,
                                 bool include_bounds) {
        if (values.empty() || values.size() % components != 0U) {
            invalidOutput("Canonical float stream is empty or misaligned");
        }
        align();
        const std::size_t offset = bytes_.size();
        const auto* raw = reinterpret_cast<const std::uint8_t*>(values.data());
        bytes_.insert(bytes_.end(), raw,
                      raw + values.size() * sizeof(float));
        const std::size_t view = views_.size();
        views_.push_back({{"buffer", 0U},
                          {"byteLength", values.size() * sizeof(float)},
                          {"byteOffset", offset},
                          {"target", kArrayBufferTarget}});
        Json accessor{{"bufferView", view},
                      {"componentType", kComponentFloat},
                      {"count", values.size() / components},
                      {"type", type}};
        if (include_bounds) {
            std::vector<float> minimum(components,
                                       std::numeric_limits<float>::infinity());
            std::vector<float> maximum(components,
                                       -std::numeric_limits<float>::infinity());
            for (std::size_t offset_value = 0U; offset_value < values.size();
                 offset_value += components) {
                for (std::size_t component = 0U; component < components;
                     ++component) {
                    minimum[component] = std::min(
                            minimum[component], values[offset_value + component]);
                    maximum[component] = std::max(
                            maximum[component], values[offset_value + component]);
                }
            }
            accessor["max"] = maximum;
            accessor["min"] = minimum;
        }
        const std::size_t index = accessors_.size();
        accessors_.push_back(std::move(accessor));
        return index;
    }

    std::size_t addIndexAccessor(const std::vector<std::uint32_t>& indices) {
        if (indices.empty()) invalidOutput("Canonical index stream is empty");
        const std::uint32_t maximum = *std::max_element(indices.begin(),
                                                         indices.end());
        align();
        const std::size_t offset = bytes_.size();
        std::uint32_t component_type = kComponentUnsignedInt;
        std::size_t byte_length = indices.size() * sizeof(std::uint32_t);
        if (maximum <= std::numeric_limits<std::uint16_t>::max()) {
            component_type = kComponentUnsignedShort;
            byte_length = indices.size() * sizeof(std::uint16_t);
            for (const std::uint32_t value : indices) {
                const auto narrowed = static_cast<std::uint16_t>(value);
                const auto* raw = reinterpret_cast<const std::uint8_t*>(&narrowed);
                bytes_.insert(bytes_.end(), raw, raw + sizeof(narrowed));
            }
        } else {
            const auto* raw = reinterpret_cast<const std::uint8_t*>(indices.data());
            bytes_.insert(bytes_.end(), raw, raw + byte_length);
        }
        const std::size_t view = views_.size();
        views_.push_back({{"buffer", 0U}, {"byteLength", byte_length},
                          {"byteOffset", offset},
                          {"target", kElementArrayBufferTarget}});
        const std::size_t accessor = accessors_.size();
        accessors_.push_back({{"bufferView", view},
                              {"componentType", component_type},
                              {"count", indices.size()},
                              {"type", "SCALAR"}});
        return accessor;
    }

    std::size_t addFeatureAccessor(const std::vector<std::uint32_t>& ids) {
        if (ids.empty()) invalidOutput("Canonical feature ID stream is empty");
        align();
        const std::size_t offset = bytes_.size();
        const auto* raw = reinterpret_cast<const std::uint8_t*>(ids.data());
        const std::size_t byte_length = ids.size() * sizeof(std::uint32_t);
        bytes_.insert(bytes_.end(), raw, raw + byte_length);
        const std::size_t view = views_.size();
        views_.push_back({{"buffer", 0U}, {"byteLength", byte_length},
                          {"byteOffset", offset},
                          {"target", kArrayBufferTarget}});
        const std::size_t accessor = accessors_.size();
        accessors_.push_back({{"bufferView", view},
                              {"componentType", kComponentUnsignedInt},
                              {"count", ids.size()},
                              {"type", "SCALAR"}});
        return accessor;
    }

    std::size_t addCanonicalFeatureAccessor(
            const metadata::FeatureIdSet& feature_id_set) {
        const auto encoded = encodeCanonicalFeatureIds(feature_id_set);
        const std::size_t alignment = encoded.component_type
                        == kComponentUnsignedShort
                ? alignof(std::uint16_t)
                : encoded.component_type == kComponentFloat
                        ? alignof(float) : alignof(std::uint8_t);
        align(alignment);
        const std::size_t offset = bytes_.size();
        bytes_.insert(bytes_.end(), encoded.bytes.begin(), encoded.bytes.end());
        const std::size_t view = views_.size();
        views_.push_back({{"buffer", 0U}, {"byteLength", encoded.bytes.size()},
                          {"byteOffset", offset},
                          {"target", kArrayBufferTarget}});
        const std::size_t accessor = accessors_.size();
        accessors_.push_back({{"bufferView", view},
                              {"componentType", encoded.component_type},
                              {"count", feature_id_set.ids.size()},
                              {"type", "SCALAR"}});
        return accessor;
    }

    std::size_t addMetadataBytes(const std::vector<std::uint8_t>& values,
                                 std::size_t alignment) {
        if (values.empty()) invalidOutput("Canonical metadata stream is empty");
        align(alignment);
        const std::size_t offset = bytes_.size();
        bytes_.insert(bytes_.end(), values.begin(), values.end());
        const std::size_t view = views_.size();
        views_.push_back({{"buffer", 0U}, {"byteLength", values.size()},
                          {"byteOffset", offset}});
        return view;
    }

    std::size_t addBytes(const std::vector<std::uint8_t>& values) {
        if (values.empty()) invalidOutput("Canonical embedded resource is empty");
        align();
        const std::size_t offset = bytes_.size();
        bytes_.insert(bytes_.end(), values.begin(), values.end());
        const std::size_t view = views_.size();
        views_.push_back({{"buffer", 0U}, {"byteLength", values.size()},
                          {"byteOffset", offset}});
        return view;
    }

    template <typename T>
    std::size_t addValues(const std::vector<T>& values) {
        if (values.empty()) invalidOutput("Canonical metadata stream is empty");
        const auto* first = reinterpret_cast<const std::uint8_t*>(values.data());
        return addBytes(std::vector<std::uint8_t>(
                first, first + values.size() * sizeof(T)));
    }

    void finish() { align(); }
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    const Json& views() const noexcept { return views_; }
    const Json& accessors() const noexcept { return accessors_; }

private:
    void align(std::size_t alignment = 4U) {
        if (alignment == 0U) invalidOutput("Canonical alignment is zero");
        while (bytes_.size() % alignment != 0U) bytes_.push_back(0U);
    }

    std::vector<std::uint8_t> bytes_;
    Json views_ = Json::array();
    Json accessors_ = Json::array();
};

bool isIdentity(const geometry::Matrix4& matrix) {
    return matrix.values() == geometry::Matrix4::identity().values();
}

template <std::size_t Count>
bool equals(const std::array<float, Count>& left,
            const std::array<float, Count>& right) {
    return left == right;
}

Json bindingJson(const mesh::TextureBinding& binding,
                 const std::map<std::size_t, std::size_t>& texture_mapping) {
    Json result{{"index", texture_mapping.at(binding.texture)}};
    if (binding.texcoord_set != 0U) result["texCoord"] = binding.texcoord_set;
    return result;
}

void addTextureBindingSet(const mesh::MeshMaterial& material,
                          std::set<std::size_t>& textures) {
    for (const auto* binding : {&material.base_color_texture,
                                &material.metallic_roughness_texture,
                                &material.normal_texture,
                                &material.occlusion_texture,
                                &material.emissive_texture}) {
        if (binding->has_value()) textures.insert((*binding)->texture);
    }
}

std::string metadataType(std::uint32_t components) {
    if (components == 1U) return "SCALAR";
    if (components == 2U) return "VEC2";
    if (components == 3U) return "VEC3";
    if (components == 4U) return "VEC4";
    invalidOutput("Legacy metadata component count is unsupported");
}

Json structuralMetadata(
        const mesh::LegacyPropertyTable& table,
        const std::vector<std::uint32_t>& retained_ids,
        BinaryBuilder& binary) {
    Json schema_properties = Json::object();
    Json table_properties = Json::object();
    for (const auto& column : table.columns) {
        Json schema_property;
        Json table_property;
        switch (column.kind) {
            case mesh::LegacyPropertyKind::numeric: {
                schema_property = {{"componentType", "FLOAT64"},
                                   {"type", metadataType(column.components)}};
                std::vector<double> values;
                values.reserve(retained_ids.size() * column.components);
                for (const std::uint32_t id : retained_ids) {
                    const std::size_t offset = static_cast<std::size_t>(id)
                                               * column.components;
                    values.insert(values.end(),
                                  column.numeric_values.begin()
                                          + static_cast<std::ptrdiff_t>(offset),
                                  column.numeric_values.begin()
                                          + static_cast<std::ptrdiff_t>(
                                                  offset + column.components));
                }
                table_property["values"] = binary.addValues(values);
                break;
            }
            case mesh::LegacyPropertyKind::boolean: {
                schema_property = {{"type", "BOOLEAN"}};
                std::vector<std::uint8_t> values(
                        (retained_ids.size() + 7U) / 8U, 0U);
                for (std::size_t index = 0U; index < retained_ids.size(); ++index) {
                    if (column.boolean_values.at(retained_ids[index]) != 0U) {
                        values[index / 8U] |= static_cast<std::uint8_t>(
                                1U << (index % 8U));
                    }
                }
                table_property["values"] = binary.addBytes(values);
                break;
            }
            case mesh::LegacyPropertyKind::string: {
                schema_property = {{"type", "STRING"}};
                std::vector<std::uint8_t> values;
                std::vector<std::uint32_t> offsets{0U};
                for (const std::uint32_t id : retained_ids) {
                    const auto& value = column.string_values.at(id);
                    if (values.size() > std::numeric_limits<std::uint32_t>::max()
                                                - value.size()) {
                        invalidOutput("Canonical metadata string bytes overflow");
                    }
                    values.insert(values.end(), value.begin(), value.end());
                    offsets.push_back(static_cast<std::uint32_t>(values.size()));
                }
                // glTF bufferViews cannot be empty. The byte is not referenced by
                // any string offset when every retained string is empty.
                if (values.empty()) values.push_back(0U);
                table_property = {{"stringOffsetType", "UINT32"},
                                  {"stringOffsets", binary.addValues(offsets)},
                                  {"values", binary.addBytes(values)}};
                break;
            }
        }
        schema_properties[column.name] = std::move(schema_property);
        table_properties[column.name] = std::move(table_property);
    }
    Json schema = {{"classes",
                    {{kLegacyClassName,
                      {{"properties", std::move(schema_properties)}}}}},
                   {"id", "legacy-batch-table"}};
    Json property_table = {{"class", kLegacyClassName},
                           {"count", retained_ids.size()},
                           {"properties", std::move(table_properties)}};
    return {{"propertyTables", Json::array({std::move(property_table)})},
            {"schema", std::move(schema)}};
}

std::vector<std::uint8_t> buildGlb(Json root, BinaryBuilder& binary) {
    binary.finish();
    root["accessors"] = binary.accessors();
    root["bufferViews"] = binary.views();
    root["buffers"] = Json::array(
            {{{"byteLength", binary.bytes().size()}}});
    std::string json = root.dump();
    while (json.size() % 4U != 0U) json.push_back(' ');
    if (json.size() > std::numeric_limits<std::uint32_t>::max()
        || binary.bytes().size() > std::numeric_limits<std::uint32_t>::max()) {
        invalidOutput("Canonical GLB exceeds uint32 format limits");
    }
    std::vector<std::uint8_t> output;
    output.insert(output.end(), {'g', 'l', 'T', 'F'});
    appendU32(output, formats::GlbParser::kSupportedVersion);
    appendU32(output, 0U);
    appendU32(output, static_cast<std::uint32_t>(json.size()));
    appendU32(output, formats::GlbParser::kJsonChunkType);
    output.insert(output.end(), json.begin(), json.end());
    appendU32(output, static_cast<std::uint32_t>(binary.bytes().size()));
    appendU32(output, formats::GlbParser::kBinaryChunkType);
    output.insert(output.end(), binary.bytes().begin(), binary.bytes().end());
    if (output.size() > std::numeric_limits<std::uint32_t>::max()) {
        invalidOutput("Canonical GLB exceeds uint32 format limits");
    }
    const std::uint32_t length = static_cast<std::uint32_t>(output.size());
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        output[8U + byte] = static_cast<std::uint8_t>(
                (length >> (byte * 8U)) & 0xffU);
    }
    return output;
}

}  // namespace

MeshCanonicalWriteResult MeshCanonicalWriter::write(mesh::MeshScene scene) {
    mesh::validateMeshScene(scene);
    if (scene.legacy_properties.has_value()
            && scene.feature_metadata.has_value()) {
        invalidOutput("Canonical scene mixes legacy and typed metadata");
    }
    const auto& roots = scene.scenes.at(scene.default_scene);
    std::vector<bool> reachable_nodes(scene.nodes.size(), false);
    std::function<void(std::size_t)> visit = [&](std::size_t node) {
        if (reachable_nodes.at(node)) return;
        reachable_nodes[node] = true;
        for (const std::size_t child : scene.nodes[node].children) visit(child);
    };
    for (const std::size_t root : roots) visit(root);

    std::set<std::size_t> used_meshes;
    for (std::size_t index = 0U; index < scene.nodes.size(); ++index) {
        if (reachable_nodes[index] && scene.nodes[index].mesh.has_value())
            used_meshes.insert(*scene.nodes[index].mesh);
    }
    if (used_meshes.empty()) invalidOutput("Canonical default scene has no mesh");

    std::set<std::size_t> used_materials;
    for (const std::size_t mesh_index : used_meshes) {
        for (const auto& primitive : scene.meshes[mesh_index].primitives) {
            if (primitive.material.has_value()) used_materials.insert(*primitive.material);
        }
    }
    std::set<std::size_t> used_textures;
    for (const std::size_t material : used_materials)
        addTextureBindingSet(scene.materials[material], used_textures);
    std::set<std::size_t> used_images;
    std::set<std::size_t> used_samplers;
    for (const std::size_t texture : used_textures) {
        used_images.insert(scene.textures[texture].image);
        if (scene.textures[texture].sampler.has_value())
            used_samplers.insert(*scene.textures[texture].sampler);
    }

    auto mapping = [](const auto& values) {
        std::map<std::size_t, std::size_t> result;
        for (const std::size_t value : values) result[value] = result.size();
        return result;
    };
    const auto mesh_mapping = mapping(used_meshes);
    const auto material_mapping = mapping(used_materials);
    const auto texture_mapping = mapping(used_textures);
    const auto image_mapping = mapping(used_images);
    const auto sampler_mapping = mapping(used_samplers);
    std::map<std::size_t, std::size_t> node_mapping;
    for (std::size_t index = 0U; index < reachable_nodes.size(); ++index) {
        if (reachable_nodes[index]) node_mapping[index] = node_mapping.size();
    }

    BinaryBuilder binary;
    Json output_meshes = Json::array();
    MeshCanonicalWriteResult result;
    bool uses_features = false;
    std::set<std::uint32_t> retained_feature_id_set;
    for (const std::size_t mesh_index : used_meshes) {
        for (const auto& primitive : scene.meshes[mesh_index].primitives) {
            retained_feature_id_set.insert(primitive.feature_ids.begin(),
                                           primitive.feature_ids.end());
        }
    }
    for (const auto& node : scene.nodes) {
        if (node.instancing.has_value()) {
            retained_feature_id_set.insert(node.instancing->feature_ids.begin(),
                                           node.instancing->feature_ids.end());
        }
    }
    std::vector<std::uint32_t> retained_feature_ids(
            retained_feature_id_set.begin(), retained_feature_id_set.end());
    std::map<std::uint32_t, std::uint32_t> feature_mapping;
    for (const std::uint32_t id : retained_feature_ids) {
        feature_mapping[id] = static_cast<std::uint32_t>(feature_mapping.size());
    }
    if (scene.legacy_properties.has_value()) {
        const std::uint32_t feature_count = scene.legacy_properties->feature_count;
        if (std::any_of(retained_feature_ids.begin(), retained_feature_ids.end(),
                        [feature_count](std::uint32_t id) {
                            return id >= feature_count;
                        })) {
            invalidOutput("Feature ID exceeds the legacy property table");
        }
        if (feature_count > 0U && retained_feature_ids.empty()) {
            invalidOutput("Legacy property table has no referenced feature");
        }
    }
    const std::uint32_t canonical_feature_count = scene.legacy_properties.has_value()
            ? static_cast<std::uint32_t>(retained_feature_ids.size())
            : retained_feature_ids.empty()
                    ? 0U : *retained_feature_ids.rbegin() + 1U;
    for (const std::size_t mesh_index : used_meshes) {
        Json primitives = Json::array();
        for (const auto& primitive : scene.meshes[mesh_index].primitives) {
            Json output{{"mode", kTrianglesMode}};
            output["attributes"]["POSITION"] = binary.addFloatAccessor(
                    primitive.positions, 3U, "VEC3", true);
            if (!primitive.normals.empty())
                output["attributes"]["NORMAL"] = binary.addFloatAccessor(
                        primitive.normals, 3U, "VEC3", false);
            if (!primitive.tangents.empty())
                output["attributes"]["TANGENT"] = binary.addFloatAccessor(
                        primitive.tangents, 4U, "VEC4", false);
            if (!primitive.texcoords_0.empty())
                output["attributes"]["TEXCOORD_0"] = binary.addFloatAccessor(
                        primitive.texcoords_0, 2U, "VEC2", false);
            if (!primitive.texcoords_1.empty())
                output["attributes"]["TEXCOORD_1"] = binary.addFloatAccessor(
                        primitive.texcoords_1, 2U, "VEC2", false);
            if (!primitive.colors.empty())
                output["attributes"]["COLOR_0"] = binary.addFloatAccessor(
                        primitive.colors, primitive.color_components,
                        primitive.color_components == 4U ? "VEC4" : "VEC3",
                        false);
            if (!primitive.feature_id_sets.empty()) {
                uses_features = true;
                Json feature_ids = Json::array();
                for (std::size_t set_index = 0U;
                     set_index < primitive.feature_id_sets.size(); ++set_index) {
                    const auto& feature_set = primitive.feature_id_sets[set_index];
                    output["attributes"]["_FEATURE_ID_"
                            + std::to_string(set_index)] =
                            binary.addCanonicalFeatureAccessor(feature_set);
                    Json declaration{{"attribute", set_index},
                                     {"featureCount", feature_set.feature_count}};
                    if (!feature_set.label.empty()) {
                        declaration["label"] = feature_set.label;
                    }
                    if (feature_set.property_table.has_value()) {
                        declaration["propertyTable"] =
                                *feature_set.property_table;
                    }
                    if (feature_set.null_feature_id.has_value()) {
                        declaration["nullFeatureId"] =
                                *feature_set.null_feature_id;
                    }
                    feature_ids.push_back(std::move(declaration));
                }
                output["extensions"][kMeshFeaturesExtension]["featureIds"] =
                        std::move(feature_ids);
            } else if (!primitive.feature_ids.empty()) {
                uses_features = true;
                std::vector<std::uint32_t> feature_ids = primitive.feature_ids;
                if (scene.legacy_properties.has_value()) {
                    std::transform(feature_ids.begin(), feature_ids.end(),
                                   feature_ids.begin(),
                                   [&feature_mapping](std::uint32_t id) {
                                       return feature_mapping.at(id);
                                   });
                }
                if (scene.feature_metadata.has_value()) {
                    metadata::FeatureIdSet feature_set;
                    feature_set.feature_count = scene.feature_metadata
                            ->primary_property_table.has_value()
                            ? scene.feature_metadata->property_tables.at(
                                      *scene.feature_metadata
                                               ->primary_property_table)
                                      .row_count
                            : canonical_feature_count;
                    feature_set.property_table = scene.feature_metadata
                            ->primary_property_table;
                    feature_set.ids = feature_ids;
                    output["attributes"]["_FEATURE_ID_0"] =
                            binary.addCanonicalFeatureAccessor(feature_set);
                } else {
                    output["attributes"]["_FEATURE_ID_0"] =
                            binary.addFeatureAccessor(feature_ids);
                }
                Json feature_id{{"attribute", 0U},
                                {"featureCount", scene.feature_metadata.has_value()
                                        && scene.feature_metadata
                                                   ->primary_property_table
                                                   .has_value()
                                        ? scene.feature_metadata
                                                  ->property_tables.at(
                                                          *scene.feature_metadata
                                                                   ->primary_property_table)
                                                  .row_count
                                        : canonical_feature_count}};
                if (scene.legacy_properties.has_value()) {
                    feature_id["propertyTable"] = 0U;
                } else if (scene.feature_metadata.has_value()
                           && scene.feature_metadata
                                      ->primary_property_table.has_value()) {
                    feature_id["propertyTable"] = *scene.feature_metadata
                                                           ->primary_property_table;
                }
                output["extensions"][kMeshFeaturesExtension]["featureIds"] =
                        Json::array({std::move(feature_id)});
            }
            output["indices"] = binary.addIndexAccessor(primitive.indices);
            if (primitive.material.has_value())
                output["material"] = material_mapping.at(*primitive.material);
            result.vertex_count += primitive.vertexCount();
            result.triangle_count += primitive.triangleCount();
            primitives.push_back(std::move(output));
        }
        output_meshes.push_back({{"primitives", std::move(primitives)}});
    }

    Json output_images = Json::array();
    for (const std::size_t image : used_images) {
        const auto png = formats::TextureCodec::encodeDeterministicPng(
                scene.images[image]);
        result.texture_bytes += png.size();
        output_images.push_back({{"bufferView", binary.addBytes(png)},
                                 {"mimeType", "image/png"}});
    }
    Json output_samplers = Json::array();
    for (const std::size_t sampler : used_samplers) {
        const auto& source = scene.samplers[sampler];
        Json output = Json::object();
        if (source.mag_filter.has_value()) output["magFilter"] = *source.mag_filter;
        if (source.min_filter.has_value()) output["minFilter"] = *source.min_filter;
        if (source.wrap_s != mesh::SamplerWrap::repeat)
            output["wrapS"] = static_cast<std::uint32_t>(source.wrap_s);
        if (source.wrap_t != mesh::SamplerWrap::repeat)
            output["wrapT"] = static_cast<std::uint32_t>(source.wrap_t);
        output_samplers.push_back(std::move(output));
    }
    Json output_textures = Json::array();
    for (const std::size_t texture : used_textures) {
        Json output{{"source", image_mapping.at(scene.textures[texture].image)}};
        if (scene.textures[texture].sampler.has_value())
            output["sampler"] = sampler_mapping.at(
                    *scene.textures[texture].sampler);
        output_textures.push_back(std::move(output));
    }

    bool uses_unlit = false;
    Json output_materials = Json::array();
    for (const std::size_t material_index : used_materials) {
        const auto& material = scene.materials[material_index];
        Json output = Json::object();
        Json pbr = Json::object();
        if (!equals(material.base_color_factor,
                    std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F}))
            pbr["baseColorFactor"] = material.base_color_factor;
        if (material.base_color_texture.has_value())
            pbr["baseColorTexture"] = bindingJson(
                    *material.base_color_texture, texture_mapping);
        if (material.metallic_factor != 1.0F)
            pbr["metallicFactor"] = material.metallic_factor;
        if (material.metallic_roughness_texture.has_value())
            pbr["metallicRoughnessTexture"] = bindingJson(
                    *material.metallic_roughness_texture, texture_mapping);
        if (material.roughness_factor != 1.0F)
            pbr["roughnessFactor"] = material.roughness_factor;
        if (!pbr.empty()) output["pbrMetallicRoughness"] = std::move(pbr);
        if (material.normal_texture.has_value()) {
            output["normalTexture"] = bindingJson(
                    *material.normal_texture, texture_mapping);
            if (material.normal_scale != 1.0F)
                output["normalTexture"]["scale"] = material.normal_scale;
        }
        if (material.occlusion_texture.has_value()) {
            output["occlusionTexture"] = bindingJson(
                    *material.occlusion_texture, texture_mapping);
            if (material.occlusion_strength != 1.0F)
                output["occlusionTexture"]["strength"] =
                        material.occlusion_strength;
        }
        if (material.emissive_texture.has_value())
            output["emissiveTexture"] = bindingJson(
                    *material.emissive_texture, texture_mapping);
        if (!equals(material.emissive_factor,
                    std::array<float, 3>{0.0F, 0.0F, 0.0F}))
            output["emissiveFactor"] = material.emissive_factor;
        if (material.alpha_mode != "OPAQUE") output["alphaMode"] = material.alpha_mode;
        if (material.alpha_mode == "MASK" && material.alpha_cutoff != 0.5F)
            output["alphaCutoff"] = material.alpha_cutoff;
        if (material.double_sided) output["doubleSided"] = true;
        if (material.unlit) {
            uses_unlit = true;
            output["extensions"][kUnlitExtension] = Json::object();
        }
        output_materials.push_back(std::move(output));
    }

    Json output_nodes = Json::array();
    bool uses_gpu_instancing = false;
    bool uses_instance_features = false;
    for (const auto& item : node_mapping) {
        const auto& node = scene.nodes[item.first];
        Json output = Json::object();
        if (!isIdentity(node.local_transform)) output["matrix"] = node.local_transform.values();
        if (node.mesh.has_value()) output["mesh"] = mesh_mapping.at(*node.mesh);
        if (node.instancing.has_value()) {
            uses_gpu_instancing = true;
            const auto& instances = *node.instancing;
            Json attributes{
                    {"TRANSLATION", binary.addFloatAccessor(
                            instances.translations, 3U, "VEC3", false)},
                    {"ROTATION", binary.addFloatAccessor(
                            instances.rotations, 4U, "VEC4", false)},
                    {"SCALE", binary.addFloatAccessor(
                            instances.scales, 3U, "VEC3", false)}};
            if (!instances.feature_ids.empty()) {
                uses_instance_features = true;
                std::vector<std::uint32_t> ids = instances.feature_ids;
                if (scene.legacy_properties.has_value()) {
                    std::transform(ids.begin(), ids.end(), ids.begin(),
                                   [&feature_mapping](std::uint32_t id) {
                                       return feature_mapping.at(id);
                                   });
                }
                if (scene.feature_metadata.has_value()) {
                    metadata::FeatureIdSet feature_set;
                    feature_set.feature_count = scene.feature_metadata
                            ->primary_property_table.has_value()
                            ? scene.feature_metadata->property_tables.at(
                                      *scene.feature_metadata
                                               ->primary_property_table)
                                      .row_count
                            : canonical_feature_count;
                    feature_set.property_table = scene.feature_metadata
                            ->primary_property_table;
                    feature_set.ids = ids;
                    attributes["_FEATURE_ID_0"] =
                            binary.addCanonicalFeatureAccessor(feature_set);
                } else {
                    attributes["_FEATURE_ID_0"] =
                            binary.addFeatureAccessor(ids);
                }
                Json feature{{"attribute", 0U},
                             {"featureCount", scene.feature_metadata.has_value()
                                     && scene.feature_metadata
                                                ->primary_property_table
                                                .has_value()
                                     ? scene.feature_metadata
                                               ->property_tables.at(
                                                       *scene.feature_metadata
                                                                ->primary_property_table)
                                               .row_count
                                     : canonical_feature_count}};
                if (scene.legacy_properties.has_value()) {
                    feature["propertyTable"] = 0U;
                } else if (scene.feature_metadata.has_value()
                           && scene.feature_metadata
                                      ->primary_property_table.has_value()) {
                    feature["propertyTable"] = *scene.feature_metadata
                                                        ->primary_property_table;
                }
                output["extensions"][kInstanceFeaturesExtension]["featureIds"] =
                        Json::array({std::move(feature)});
            }
            output["extensions"][kGpuInstancingExtension]["attributes"] =
                    std::move(attributes);
        }
        Json children = Json::array();
        for (const std::size_t child : node.children) {
            const auto mapped = node_mapping.find(child);
            if (mapped != node_mapping.end()) children.push_back(mapped->second);
        }
        if (!children.empty()) output["children"] = std::move(children);
        output_nodes.push_back(std::move(output));
    }
    Json output_roots = Json::array();
    for (const std::size_t root : roots) output_roots.push_back(node_mapping.at(root));

    Json root;
    root["asset"] = {{"generator", "3d-tiles-clip-worker-normalizer"},
                     {"version", "2.0"}};
    root["scene"] = 0U;
    root["scenes"] = Json::array({{{"nodes", std::move(output_roots)}}});
    root["nodes"] = std::move(output_nodes);
    root["meshes"] = std::move(output_meshes);
    if (scene.legacy_properties.has_value()) {
        root["extensions"][kStructuralMetadataExtension] = structuralMetadata(
                *scene.legacy_properties, retained_feature_ids, binary);
    } else if (scene.feature_metadata.has_value()) {
        const auto metadata_json = writeCanonicalMetadata(
                *scene.feature_metadata,
                [&binary](const std::vector<std::uint8_t>& bytes,
                          std::size_t alignment) {
                    return binary.addMetadataBytes(bytes, alignment);
                });
        root["extensions"][kStructuralMetadataExtension] =
                metadata_json.structural_metadata;
        if (metadata_json.legacy_hierarchy.has_value()) {
            root["extensions"][kCanonicalLegacyHierarchyExtension] =
                    *metadata_json.legacy_hierarchy;
        }
    }
    if (!output_materials.empty()) root["materials"] = std::move(output_materials);
    if (!output_textures.empty()) root["textures"] = std::move(output_textures);
    if (!output_images.empty()) root["images"] = std::move(output_images);
    if (!output_samplers.empty()) root["samplers"] = std::move(output_samplers);
    std::vector<std::string> used_extensions;
    std::vector<std::string> required_extensions;
    if (uses_features) used_extensions.emplace_back(kMeshFeaturesExtension);
    if (uses_gpu_instancing) {
        used_extensions.emplace_back(kGpuInstancingExtension);
        required_extensions.emplace_back(kGpuInstancingExtension);
    }
    if (uses_instance_features)
        used_extensions.emplace_back(kInstanceFeaturesExtension);
    if (scene.legacy_properties.has_value()
            || scene.feature_metadata.has_value())
        used_extensions.emplace_back(kStructuralMetadataExtension);
    if (scene.feature_metadata.has_value()
            && scene.feature_metadata->hierarchy.has_value()) {
        used_extensions.emplace_back(kCanonicalLegacyHierarchyExtension);
        required_extensions.emplace_back(kCanonicalLegacyHierarchyExtension);
    }
    if (uses_unlit) {
        used_extensions.emplace_back(kUnlitExtension);
        if (scene.require_unlit) required_extensions.emplace_back(kUnlitExtension);
    }
    std::sort(used_extensions.begin(), used_extensions.end());
    std::sort(required_extensions.begin(), required_extensions.end());
    if (!used_extensions.empty()) root["extensionsUsed"] = used_extensions;
    if (!required_extensions.empty()) root["extensionsRequired"] = required_extensions;
    result.glb = buildGlb(std::move(root), binary);
    return result;
}

}  // namespace clip_worker::normalization
