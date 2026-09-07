#include "clip_worker/formats/b3dm_mesh_adapter.hpp"

#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/metadata/legacy_feature_metadata.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace clip_worker::formats {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(const std::string& message) {
    throw FormatError(FormatErrorCode::invalid_feature_table, message);
}

[[noreturn]] void unsupported(const std::string& message) {
    throw FormatError(FormatErrorCode::metadata_batch_table_unsupported,
                      message);
}

Json parseObject(const std::string& text, const char* description) {
    if (text.empty()) return Json::object();
    try {
        Json value = Json::parse(text);
        if (!value.is_object()) invalid(std::string(description) + " is not an object");
        return value;
    } catch (const FormatError&) {
        throw;
    } catch (const Json::exception&) {
        invalid(std::string(description) + " is invalid JSON");
    }
}

void allowedKeys(const Json& object, const std::set<std::string>& names,
                 const char* description) {
    for (const auto& item : object.items()) {
        if (names.find(item.key()) == names.end()) {
            unsupported(std::string(description) + " contains an unsupported field");
        }
    }
}

std::size_t unsignedOffset(const Json& object, const char* field) {
    const auto item = object.find(field);
    if (item == object.end() || !item->is_number_unsigned()
        || *item > std::numeric_limits<std::size_t>::max()) {
        invalid(std::string(field) + " is not an unsigned offset");
    }
    return item->get<std::size_t>();
}

template <typename T>
T readScalar(ByteView bytes, std::size_t offset) {
    if (!bytes.contains(offset, sizeof(T))) {
        invalid("B3DM binary property exceeds its table section");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::optional<std::array<double, 3>> readRtcCenter(
        const Json& feature_table, ByteView binary) {
    const auto item = feature_table.find("RTC_CENTER");
    if (item == feature_table.end()) return std::nullopt;
    std::array<double, 3> result{};
    if (item->is_array()) {
        if (item->size() != result.size()) invalid("RTC_CENTER must contain three numbers");
        for (std::size_t index = 0U; index < result.size(); ++index) {
            if (!item->at(index).is_number()) invalid("RTC_CENTER contains a non-number");
            result[index] = item->at(index).get<double>();
        }
    } else if (item->is_object()) {
        allowedKeys(*item, {"byteOffset"}, "RTC_CENTER binary reference");
        const std::size_t offset = unsignedOffset(*item, "byteOffset");
        if (offset % alignof(float) != 0U
            || !binary.contains(offset, result.size() * sizeof(float))) {
            invalid("RTC_CENTER binary reference is misaligned or out of range");
        }
        for (std::size_t index = 0U; index < result.size(); ++index) {
            result[index] = readScalar<float>(binary,
                    offset + index * sizeof(float));
        }
    } else {
        invalid("RTC_CENTER has an unsupported representation");
    }
    if (std::any_of(result.begin(), result.end(),
                    [](double value) { return !std::isfinite(value); })) {
        invalid("RTC_CENTER contains a non-finite value");
    }
    return result;
}

std::uint32_t componentCount(const std::string& type) {
    if (type == "SCALAR") return 1U;
    if (type == "VEC2") return 2U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4") return 4U;
    unsupported("Batch Table Binary property type is unsupported");
}

struct NumericComponent {
    std::size_t size = 0U;
    std::size_t alignment = 0U;
    double (*read)(ByteView, std::size_t) = nullptr;
};

template <typename T>
double readNumeric(ByteView bytes, std::size_t offset) {
    return static_cast<double>(readScalar<T>(bytes, offset));
}

NumericComponent numericComponent(const std::string& component_type) {
    if (component_type == "BYTE")
        return {sizeof(std::int8_t), alignof(std::int8_t), readNumeric<std::int8_t>};
    if (component_type == "UNSIGNED_BYTE")
        return {sizeof(std::uint8_t), alignof(std::uint8_t), readNumeric<std::uint8_t>};
    if (component_type == "SHORT")
        return {sizeof(std::int16_t), alignof(std::int16_t), readNumeric<std::int16_t>};
    if (component_type == "UNSIGNED_SHORT")
        return {sizeof(std::uint16_t), alignof(std::uint16_t), readNumeric<std::uint16_t>};
    if (component_type == "INT")
        return {sizeof(std::int32_t), alignof(std::int32_t), readNumeric<std::int32_t>};
    if (component_type == "UNSIGNED_INT")
        return {sizeof(std::uint32_t), alignof(std::uint32_t), readNumeric<std::uint32_t>};
    if (component_type == "FLOAT")
        return {sizeof(float), alignof(float), readNumeric<float>};
    if (component_type == "DOUBLE")
        return {sizeof(double), alignof(double), readNumeric<double>};
    unsupported("Batch Table Binary componentType is unsupported");
}

mesh::LegacyPropertyColumn jsonColumn(const std::string& name,
                                      const Json& values,
                                      std::uint32_t feature_count,
                                      std::uint64_t& string_bytes) {
    if (!values.is_array() || values.size() != feature_count) {
        invalid("Batch Table JSON property row count differs from BATCH_LENGTH");
    }
    mesh::LegacyPropertyColumn column;
    column.name = name;
    if (values.empty()) return column;
    if (values.front().is_boolean()) {
        column.kind = mesh::LegacyPropertyKind::boolean;
        for (const auto& value : values) {
            if (!value.is_boolean()) invalid("Batch Table JSON property types are mixed");
            column.boolean_values.push_back(value.get<bool>() ? 1U : 0U);
        }
    } else if (values.front().is_number()) {
        column.kind = mesh::LegacyPropertyKind::numeric;
        for (const auto& value : values) {
            if (!value.is_number()) invalid("Batch Table JSON property types are mixed");
            const double number = value.get<double>();
            if (!std::isfinite(number)) invalid("Batch Table JSON number is non-finite");
            column.numeric_values.push_back(number);
        }
    } else if (values.front().is_string()) {
        column.kind = mesh::LegacyPropertyKind::string;
        for (const auto& value : values) {
            if (!value.is_string()) invalid("Batch Table JSON property types are mixed");
            const auto text = value.get<std::string>();
            if (string_bytes > std::numeric_limits<std::uint64_t>::max() - text.size())
                unsupported("Batch Table string byte count overflows");
            string_bytes += text.size();
            column.string_values.push_back(text);
        }
    } else {
        unsupported("Batch Table JSON property value type is unsupported");
    }
    return column;
}

mesh::LegacyPropertyColumn binaryColumn(const std::string& name,
                                        const Json& reference,
                                        ByteView bytes,
                                        std::uint32_t feature_count) {
    allowedKeys(reference, {"byteOffset", "componentType", "type"},
                "Batch Table Binary property");
    if (!reference.contains("componentType")
        || !reference.at("componentType").is_string()
        || !reference.contains("type") || !reference.at("type").is_string()) {
        invalid("Batch Table Binary property descriptor is incomplete");
    }
    const std::size_t offset = unsignedOffset(reference, "byteOffset");
    const NumericComponent component = numericComponent(
            reference.at("componentType").get<std::string>());
    const std::uint32_t components = componentCount(
            reference.at("type").get<std::string>());
    const std::uint64_t value_count = static_cast<std::uint64_t>(feature_count)
                                      * components;
    if (offset % component.alignment != 0U
        || value_count > std::numeric_limits<std::size_t>::max() / component.size
        || !bytes.contains(offset, static_cast<std::size_t>(value_count)
                                   * component.size)) {
        invalid("Batch Table Binary property is misaligned or out of range");
    }
    mesh::LegacyPropertyColumn column;
    column.name = name;
    column.kind = mesh::LegacyPropertyKind::numeric;
    column.components = components;
    column.numeric_values.reserve(static_cast<std::size_t>(value_count));
    for (std::size_t index = 0U; index < value_count; ++index) {
        const double value = component.read(bytes, offset + index * component.size);
        if (!std::isfinite(value)) invalid("Batch Table Binary contains non-finite data");
        column.numeric_values.push_back(value);
    }
    return column;
}

mesh::LegacyPropertyTable readProperties(
        const Json& batch_table, ByteView binary, std::uint32_t feature_count,
        const B3dmMetadataLimits& limits) {
    if (feature_count > limits.maximum_feature_rows
        || binary.size() > limits.maximum_binary_bytes
        || batch_table.size() > limits.maximum_properties) {
        unsupported("B3DM metadata exceeds the configured limit");
    }
    mesh::LegacyPropertyTable table;
    table.feature_count = feature_count;
    std::uint64_t string_bytes = 0U;
    for (const auto& item : batch_table.items()) {
        if (item.value().is_array()) {
            table.columns.push_back(jsonColumn(
                    item.key(), item.value(), feature_count, string_bytes));
        } else if (item.value().is_object()) {
            table.columns.push_back(binaryColumn(
                    item.key(), item.value(), binary, feature_count));
        } else {
            unsupported("Batch Table property representation is unsupported");
        }
        if (string_bytes > limits.maximum_string_bytes) {
            unsupported("B3DM metadata strings exceed the configured limit");
        }
    }
    std::sort(table.columns.begin(), table.columns.end(),
              [](const auto& left, const auto& right) {
                  return left.name < right.name;
              });
    return table;
}

void validateFeatureIds(mesh::MeshScene& scene, std::uint32_t feature_count) {
    for (auto& source_mesh : scene.meshes) {
        for (auto& primitive : source_mesh.primitives) {
            if (primitive.feature_ids.empty()) {
                if (feature_count == 1U) {
                    primitive.feature_ids.assign(primitive.vertexCount(), 0U);
                } else if (feature_count > 1U) {
                    invalid("BATCH_LENGTH greater than one requires _BATCHID");
                }
            }
            if (feature_count == 0U) {
                if (!primitive.feature_ids.empty())
                    invalid("BATCH_LENGTH zero cannot reference feature IDs");
                continue;
            }
            if (primitive.feature_ids.size() != primitive.vertexCount())
                invalid("_BATCHID count differs from POSITION");
            if (std::any_of(primitive.feature_ids.begin(), primitive.feature_ids.end(),
                            [feature_count](std::uint32_t id) {
                                return id >= feature_count;
                            })) {
                invalid("_BATCHID value exceeds BATCH_LENGTH");
            }
            for (std::size_t offset = 0U; offset < primitive.indices.size();
                 offset += 3U) {
                const std::uint32_t first = primitive.feature_ids.at(
                        primitive.indices[offset]);
                if (primitive.feature_ids.at(primitive.indices[offset + 1U]) != first
                    || primitive.feature_ids.at(primitive.indices[offset + 2U]) != first) {
                    invalid("A B3DM triangle references multiple feature IDs");
                }
            }
        }
    }
}

void applyRtcCenter(mesh::MeshScene& scene,
                    const std::optional<std::array<double, 3>>& rtc_center) {
    if (!rtc_center.has_value()) return;
    mesh::MeshNode container;
    container.source_ordinal = scene.nodes.size();
    container.local_transform = geometry::Matrix4::translation(*rtc_center);
    container.children = scene.scenes.at(scene.default_scene);
    scene.nodes.push_back(std::move(container));
    scene.scenes.at(scene.default_scene) = {scene.nodes.size() - 1U};
}

}  // namespace

B3dmMeshReadResult B3dmMeshAdapter::read(
        const std::vector<std::uint8_t>& source_bytes,
        const B3dmMeshAdapterLimits& limits) {
    const B3dmDocument document = B3dmParser::parse(ByteView(source_bytes));
    const Json feature_table = parseObject(
            document.feature_table_json_text, "B3DM Feature Table");
    allowedKeys(feature_table, {"BATCH_LENGTH", "RTC_CENTER"},
                "B3DM Feature Table");
    const Json batch_table = parseObject(
            document.batch_table_json_text, "B3DM Batch Table");
    const ByteView all(source_bytes);
    const auto rtc_center = readRtcCenter(
            feature_table, all.subview(document.feature_table_binary.offset,
                                       document.feature_table_binary.byte_length));
    std::vector<std::uint8_t> glb(
            source_bytes.begin() + static_cast<std::ptrdiff_t>(document.glb_section.offset),
            source_bytes.begin() + static_cast<std::ptrdiff_t>(
                    document.glb_section.offset + document.glb_section.byte_length));
    auto decoded = GltfMeshReader::read(
            glb, GltfContentKind::glb, "embedded.glb", {}, limits.gltf);
    validateFeatureIds(decoded.scene, document.batch_length);
    if (limits.enable_feature_metadata
            && (document.batch_length > 0U || !batch_table.empty())) {
        decoded.scene.feature_metadata = metadata::readLegacyFeatureMetadata(
                document.batch_table_json_text,
                all.subview(document.batch_table_binary.offset,
                            document.batch_table_binary.byte_length),
                document.batch_length, limits.feature_metadata);
    } else if (document.batch_length > 0U || !batch_table.empty()) {
        decoded.scene.legacy_properties = readProperties(
                batch_table,
                all.subview(document.batch_table_binary.offset,
                            document.batch_table_binary.byte_length),
                document.batch_length, limits.metadata);
    }
    applyRtcCenter(decoded.scene, rtc_center);
    mesh::validateMeshScene(decoded.scene);
    return {std::move(decoded.scene), document.layout,
            decoded.diagnostics, rtc_center};
}

}  // namespace clip_worker::formats
