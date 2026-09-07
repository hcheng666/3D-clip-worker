#include "clip_worker/normalization/point_canonical_writer.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/glb.hpp"
#include "clip_worker/normalization/metadata_canonical_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kArrayBufferTarget = 34962U;
constexpr std::uint32_t kUnsignedByte = 5121U;
constexpr std::uint32_t kUnsignedShort = 5123U;
constexpr std::uint32_t kUnsignedInt = 5125U;
constexpr std::uint32_t kFloat = 5126U;
constexpr std::uint32_t kPointsMode = 0U;
constexpr const char* kMeshFeatures = "EXT_mesh_features";
constexpr const char* kStructuralMetadata = "EXT_structural_metadata";
constexpr const char* kLegacyClassName = "legacyPointFeature";

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(formats::FormatErrorCode::point_output_invalid,
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
    std::size_t addFloat(const std::vector<float>& values,
                         std::size_t components, const char* type,
                         bool bounds) {
        if (values.empty() || values.size() % components != 0U) {
            invalid("Canonical point FLOAT accessor is empty or misaligned");
        }
        const std::size_t view = addView(values, true);
        Json accessor{{"bufferView", view}, {"componentType", kFloat},
                      {"count", values.size() / components}, {"type", type}};
        if (bounds) {
            std::vector<float> minimum(components,
                    std::numeric_limits<float>::infinity());
            std::vector<float> maximum(components,
                    -std::numeric_limits<float>::infinity());
            for (std::size_t offset = 0U; offset < values.size();
                 offset += components) {
                for (std::size_t component = 0U; component < components;
                     ++component) {
                    minimum[component] = std::min(
                            minimum[component], values[offset + component]);
                    maximum[component] = std::max(
                            maximum[component], values[offset + component]);
                }
            }
            accessor["min"] = minimum;
            accessor["max"] = maximum;
        }
        return addAccessor(std::move(accessor));
    }

    std::size_t addColor(const std::vector<std::uint8_t>& values) {
        if (values.empty() || values.size() % 4U != 0U) {
            invalid("Canonical point color accessor is empty or misaligned");
        }
        return addAccessor({{"bufferView", addView(values, true)},
                            {"componentType", kUnsignedByte},
                            {"normalized", true},
                            {"count", values.size() / 4U}, {"type", "VEC4"}});
    }

    std::size_t addFeatureIds(const std::vector<std::uint32_t>& values) {
        if (values.empty()) invalid("Canonical point feature IDs are empty");
        return addAccessor({{"bufferView", addView(values, true)},
                            {"componentType", kUnsignedInt},
                            {"count", values.size()}, {"type", "SCALAR"}});
    }

    std::size_t addCanonicalFeatureIds(
            const metadata::FeatureIdSet& feature_id_set) {
        const auto encoded = encodeCanonicalFeatureIds(feature_id_set);
        const std::size_t alignment = encoded.component_type == kUnsignedShort
                ? alignof(std::uint16_t)
                : encoded.component_type == kFloat
                        ? alignof(float) : alignof(std::uint8_t);
        return addAccessor({{"bufferView", addRawView(
                                     encoded.bytes, true, alignment)},
                            {"componentType", encoded.component_type},
                            {"count", feature_id_set.ids.size()},
                            {"type", "SCALAR"}});
    }

    std::size_t addMetadataBytes(const std::vector<std::uint8_t>& values,
                                 std::size_t alignment) {
        return addRawView(values, false, alignment);
    }

    std::size_t addBytes(const std::vector<std::uint8_t>& values) {
        if (values.empty()) invalid("Canonical metadata bufferView is empty");
        return addView(values, false);
    }

    template <typename T>
    std::size_t addValues(const std::vector<T>& values) {
        if (values.empty()) invalid("Canonical metadata values are empty");
        const auto* begin = reinterpret_cast<const std::uint8_t*>(values.data());
        return addBytes(std::vector<std::uint8_t>(
                begin, begin + values.size() * sizeof(T)));
    }

    void finish() { align(); }
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    const Json& views() const noexcept { return views_; }
    const Json& accessors() const noexcept { return accessors_; }

private:
    std::size_t addRawView(const std::vector<std::uint8_t>& values,
                           bool array_target, std::size_t alignment) {
        if (values.empty()) invalid("Canonical metadata bufferView is empty");
        align(alignment);
        const std::size_t offset = bytes_.size();
        bytes_.insert(bytes_.end(), values.begin(), values.end());
        Json view{{"buffer", 0U}, {"byteOffset", offset},
                  {"byteLength", values.size()}};
        if (array_target) view["target"] = kArrayBufferTarget;
        const std::size_t index = views_.size();
        views_.push_back(std::move(view));
        return index;
    }

    template <typename T>
    std::size_t addView(const std::vector<T>& values, bool array_target) {
        align();
        const std::size_t offset = bytes_.size();
        const auto* begin = reinterpret_cast<const std::uint8_t*>(values.data());
        const std::size_t byte_length = values.size() * sizeof(T);
        bytes_.insert(bytes_.end(), begin, begin + byte_length);
        Json view{{"buffer", 0U}, {"byteOffset", offset},
                  {"byteLength", byte_length}};
        if (array_target) view["target"] = kArrayBufferTarget;
        const std::size_t index = views_.size();
        views_.push_back(std::move(view));
        return index;
    }

    std::size_t addAccessor(Json accessor) {
        const std::size_t index = accessors_.size();
        accessors_.push_back(std::move(accessor));
        return index;
    }

    void align(std::size_t alignment = 4U) {
        if (alignment == 0U) invalid("Canonical point alignment is zero");
        while (bytes_.size() % alignment != 0U) bytes_.push_back(0U);
    }

    std::vector<std::uint8_t> bytes_;
    Json views_ = Json::array();
    Json accessors_ = Json::array();
};

std::string metadataType(std::uint32_t components) {
    if (components == 1U) return "SCALAR";
    if (components == 2U) return "VEC2";
    if (components == 3U) return "VEC3";
    if (components == 4U) return "VEC4";
    invalid("Canonical point property component count is unsupported");
}

Json structuralMetadata(const mesh::LegacyPropertyTable& table,
                        BinaryBuilder& binary) {
    Json schema_properties = Json::object();
    Json table_properties = Json::object();
    for (const auto& column : table.columns) {
        Json schema_property;
        Json property;
        switch (column.kind) {
            case mesh::LegacyPropertyKind::numeric:
                schema_property = {{"componentType", "FLOAT64"},
                                   {"type", metadataType(column.components)}};
                property["values"] = binary.addValues(column.numeric_values);
                break;
            case mesh::LegacyPropertyKind::boolean: {
                schema_property = {{"type", "BOOLEAN"}};
                std::vector<std::uint8_t> values(
                        (column.boolean_values.size() + 7U) / 8U, 0U);
                for (std::size_t index = 0U;
                     index < column.boolean_values.size(); ++index) {
                    if (column.boolean_values[index] != 0U) {
                        values[index / 8U] |= static_cast<std::uint8_t>(
                                1U << (index % 8U));
                    }
                }
                if (values.empty()) values.push_back(0U);
                property["values"] = binary.addBytes(values);
                break;
            }
            case mesh::LegacyPropertyKind::string: {
                schema_property = {{"type", "STRING"}};
                std::vector<std::uint8_t> values;
                std::vector<std::uint32_t> offsets{0U};
                for (const auto& text : column.string_values) {
                    if (values.size() > std::numeric_limits<std::uint32_t>::max()
                                                - text.size()) {
                        invalid("Canonical point property string bytes overflow");
                    }
                    values.insert(values.end(), text.begin(), text.end());
                    offsets.push_back(static_cast<std::uint32_t>(values.size()));
                }
                if (values.empty()) values.push_back(0U);
                property = {{"stringOffsetType", "UINT32"},
                            {"stringOffsets", binary.addValues(offsets)},
                            {"values", binary.addBytes(values)}};
                break;
            }
        }
        schema_properties[column.name] = std::move(schema_property);
        table_properties[column.name] = std::move(property);
    }
    return {{"schema", {{"id", "legacy-point-batch-table"},
                         {"classes", {{kLegacyClassName,
                                       {{"properties", std::move(schema_properties)}}}}}}},
            {"propertyTables", Json::array({
                    {{"class", kLegacyClassName}, {"count", table.feature_count},
                     {"properties", std::move(table_properties)}}})}};
}

std::vector<std::uint8_t> buildGlb(Json root, BinaryBuilder& binary) {
    binary.finish();
    root["accessors"] = binary.accessors();
    root["bufferViews"] = binary.views();
    root["buffers"] = Json::array({{{"byteLength", binary.bytes().size()}}});
    std::string json = root.dump();
    while (json.size() % 4U != 0U) json.push_back(' ');
    if (json.size() > std::numeric_limits<std::uint32_t>::max()
        || binary.bytes().size() > std::numeric_limits<std::uint32_t>::max()) {
        invalid("Canonical point GLB exceeds format limits");
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
        invalid("Canonical point GLB exceeds uint32 byteLength");
    }
    const std::uint32_t length = static_cast<std::uint32_t>(output.size());
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        output[8U + byte] = static_cast<std::uint8_t>(
                (length >> (byte * 8U)) & 0xffU);
    }
    return output;
}

bool isIdentity(const geometry::Matrix4& matrix) {
    return matrix.values() == geometry::Matrix4::identity().values();
}

}  // namespace

PointCanonicalWriteResult PointCanonicalWriter::write(point::PointScene scene) {
    point::validatePointScene(scene);
    if (scene.legacy_properties.has_value()
            && scene.feature_metadata.has_value()) {
        invalid("Canonical point scene mixes legacy and typed metadata");
    }
    BinaryBuilder binary;
    Json primitive{{"mode", kPointsMode}};
    primitive["attributes"]["POSITION"] = binary.addFloat(
            scene.positions, 3U, "VEC3", true);
    if (!scene.normals.empty()) {
        primitive["attributes"]["NORMAL"] = binary.addFloat(
                scene.normals, 3U, "VEC3", false);
    }
    if (!scene.colors_rgba.empty()) {
        primitive["attributes"]["COLOR_0"] = binary.addColor(scene.colors_rgba);
    }

    PointCanonicalWriteResult result;
    result.point_count = scene.pointCount();
    std::vector<std::string> extensions;
    if (!scene.feature_ids.empty()) {
        const std::uint32_t feature_count = scene.feature_metadata.has_value()
                && scene.feature_metadata->primary_property_table.has_value()
                ? scene.feature_metadata->property_tables.at(
                          *scene.feature_metadata->primary_property_table)
                          .row_count
                : scene.legacy_properties.has_value()
                ? scene.legacy_properties->feature_count
                : *std::max_element(scene.feature_ids.begin(),
                                    scene.feature_ids.end()) + 1U;
        if (scene.feature_metadata.has_value()) {
            metadata::FeatureIdSet feature_set;
            feature_set.feature_count = feature_count;
            feature_set.property_table =
                    scene.feature_metadata->primary_property_table;
            feature_set.ids = scene.feature_ids;
            primitive["attributes"]["_FEATURE_ID_0"] =
                    binary.addCanonicalFeatureIds(feature_set);
        } else {
            primitive["attributes"]["_FEATURE_ID_0"] =
                    binary.addFeatureIds(scene.feature_ids);
        }
        Json feature{{"attribute", 0U}, {"featureCount", feature_count}};
        if (scene.legacy_properties.has_value()) {
            feature["propertyTable"] = 0U;
        } else if (scene.feature_metadata.has_value()
                   && scene.feature_metadata->primary_property_table.has_value()) {
            feature["propertyTable"] =
                    *scene.feature_metadata->primary_property_table;
        }
        primitive["extensions"][kMeshFeatures]["featureIds"] =
                Json::array({std::move(feature)});
        extensions.emplace_back(kMeshFeatures);
        result.feature_count = feature_count;
    }

    Json node{{"mesh", 0U}};
    if (!isIdentity(scene.root_transform)) node["matrix"] = scene.root_transform.values();
    Json root{{"asset", {{"generator", "3d-tiles-clip-worker-point-normalizer"},
                          {"version", "2.0"}}},
              {"scene", 0U}, {"scenes", Json::array({{{"nodes", {0U}}}})},
              {"nodes", Json::array({std::move(node)})},
              {"meshes", Json::array({{{"primitives", Json::array({std::move(primitive)})}}})}};
    if (scene.legacy_properties.has_value()) {
        root["extensions"][kStructuralMetadata] =
                structuralMetadata(*scene.legacy_properties, binary);
        extensions.emplace_back(kStructuralMetadata);
    } else if (scene.feature_metadata.has_value()) {
        const auto metadata_json = writeCanonicalMetadata(
                *scene.feature_metadata,
                [&binary](const std::vector<std::uint8_t>& bytes,
                          std::size_t alignment) {
                    return binary.addMetadataBytes(bytes, alignment);
                });
        root["extensions"][kStructuralMetadata] =
                metadata_json.structural_metadata;
        extensions.emplace_back(kStructuralMetadata);
        if (metadata_json.legacy_hierarchy.has_value()) {
            root["extensions"][kCanonicalLegacyHierarchyExtension] =
                    *metadata_json.legacy_hierarchy;
            extensions.emplace_back(kCanonicalLegacyHierarchyExtension);
            root["extensionsRequired"] =
                    Json::array({kCanonicalLegacyHierarchyExtension});
        }
    }
    std::sort(extensions.begin(), extensions.end());
    extensions.erase(std::unique(extensions.begin(), extensions.end()),
                     extensions.end());
    if (!extensions.empty()) root["extensionsUsed"] = extensions;
    result.glb = buildGlb(std::move(root), binary);
    return result;
}

}  // namespace clip_worker::normalization
