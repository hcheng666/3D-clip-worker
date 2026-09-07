#include "clip_worker/normalization/metadata_canonical_writer.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kUnsignedByteComponentType = 5121U;
constexpr std::uint32_t kUnsignedShortComponentType = 5123U;
constexpr std::uint32_t kFloatComponentType = 5126U;

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_reconstruction_unsafe,
            message);
}

std::string propertyType(std::uint32_t components) {
    if (components == 1U) return "SCALAR";
    if (components == 2U) return "VEC2";
    if (components == 3U) return "VEC3";
    if (components == 4U) return "VEC4";
    invalid("Canonical metadata component count is unsupported");
}

std::string componentType(metadata::PropertyScalarType type) {
    switch (type) {
        case metadata::PropertyScalarType::int8: return "INT8";
        case metadata::PropertyScalarType::uint8: return "UINT8";
        case metadata::PropertyScalarType::int16: return "INT16";
        case metadata::PropertyScalarType::uint16: return "UINT16";
        case metadata::PropertyScalarType::int32: return "INT32";
        case metadata::PropertyScalarType::uint32: return "UINT32";
        case metadata::PropertyScalarType::float32: return "FLOAT32";
        case metadata::PropertyScalarType::float64: return "FLOAT64";
        case metadata::PropertyScalarType::boolean:
        case metadata::PropertyScalarType::string:
        case metadata::PropertyScalarType::enumeration: break;
    }
    invalid("Canonical metadata component type is unavailable");
}

std::size_t componentAlignment(metadata::PropertyScalarType type) {
    switch (type) {
        case metadata::PropertyScalarType::int8:
        case metadata::PropertyScalarType::uint8:
        case metadata::PropertyScalarType::boolean:
        case metadata::PropertyScalarType::string: return 1U;
        case metadata::PropertyScalarType::int16:
        case metadata::PropertyScalarType::uint16: return 2U;
        case metadata::PropertyScalarType::int32:
        case metadata::PropertyScalarType::uint32:
        case metadata::PropertyScalarType::float32: return 4U;
        case metadata::PropertyScalarType::float64: return 8U;
        case metadata::PropertyScalarType::enumeration: break;
    }
    invalid("Canonical enum alignment requires its value type");
}

template <typename Value>
std::vector<std::uint8_t> rawBytes(const std::vector<Value>& values) {
    if (values.empty()) invalid("Canonical metadata values are empty");
    if (values.size() > std::numeric_limits<std::size_t>::max()
                                / sizeof(Value)) {
        invalid("Canonical metadata byte count overflows");
    }
    std::vector<std::uint8_t> result(values.size() * sizeof(Value));
    std::memcpy(result.data(), values.data(), result.size());
    return result;
}

Json literalJson(const metadata::PropertyValues& values) {
    Json result = Json::array();
    std::visit(
            [&result](const auto& typed) {
                using Value = std::decay_t<decltype(typed)>;
                const auto append = [&result](const auto& source) {
                    for (const auto& value : source) result.push_back(value);
                };
                if constexpr (std::is_same_v<Value, metadata::BooleanValues>) {
                    for (const std::uint8_t value : typed.values) {
                        result.push_back(value != 0U);
                    }
                } else if constexpr (
                        std::is_same_v<Value, metadata::StringValues>
                        || std::is_same_v<Value, metadata::EnumValues>) {
                    append(typed.values);
                } else {
                    append(typed);
                }
            },
            values);
    if (result.size() == 1U) return result.at(0U);
    return result;
}

void addLiteral(Json& definition, const char* name,
                const std::optional<metadata::PropertyValues>& values) {
    if (values.has_value()) definition[name] = literalJson(*values);
}

struct OffsetBytes {
    std::string type;
    std::size_t alignment = 1U;
    std::vector<std::uint8_t> bytes;
};

OffsetBytes encodeOffsets(const std::vector<std::uint32_t>& offsets) {
    if (offsets.empty()) invalid("Canonical metadata offsets are empty");
    const std::uint32_t maximum = *std::max_element(
            offsets.begin(), offsets.end());
    if (maximum <= std::numeric_limits<std::uint8_t>::max()) {
        std::vector<std::uint8_t> values;
        values.reserve(offsets.size());
        for (const auto value : offsets) {
            values.push_back(static_cast<std::uint8_t>(value));
        }
        return {"UINT8", alignof(std::uint8_t), rawBytes(values)};
    }
    if (maximum <= std::numeric_limits<std::uint16_t>::max()) {
        std::vector<std::uint16_t> values;
        values.reserve(offsets.size());
        for (const auto value : offsets) {
            values.push_back(static_cast<std::uint16_t>(value));
        }
        return {"UINT16", alignof(std::uint16_t), rawBytes(values)};
    }
    return {"UINT32", alignof(std::uint32_t), rawBytes(offsets)};
}

std::vector<std::uint8_t> encodeValues(
        const metadata::PropertyColumn& column, std::size_t& alignment) {
    return std::visit(
            [&column, &alignment](const auto& typed)
                    -> std::vector<std::uint8_t> {
                using Value = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Value,
                                             metadata::BooleanValues>) {
                    alignment = 1U;
                    std::vector<std::uint8_t> packed(
                            (typed.values.size() + 7U) / 8U, 0U);
                    for (std::size_t index = 0U;
                         index < typed.values.size(); ++index) {
                        if (typed.values[index] > 1U) {
                            invalid("Canonical boolean value is invalid");
                        }
                        if (typed.values[index] != 0U) {
                            packed[index / 8U] |= static_cast<std::uint8_t>(
                                    1U << (index % 8U));
                        }
                    }
                    if (packed.empty()) invalid("Canonical boolean values are empty");
                    return packed;
                } else if constexpr (std::is_same_v<Value,
                                                    metadata::StringValues>) {
                    invalid("String values require offset-aware encoding");
                } else if constexpr (std::is_same_v<Value,
                                                    metadata::EnumValues>) {
                    if (!column.enum_value_type.has_value()) {
                        invalid("Canonical enum value type is absent");
                    }
                    alignment = componentAlignment(*column.enum_value_type);
                    switch (*column.enum_value_type) {
                        case metadata::PropertyScalarType::int8: {
                            std::vector<std::int8_t> values;
                            for (const auto value : typed.values) {
                                if (value < std::numeric_limits<std::int8_t>::min()
                                        || value > std::numeric_limits<std::int8_t>::max()) invalid("Canonical enum value is out of range");
                                values.push_back(static_cast<std::int8_t>(value));
                            }
                            return rawBytes(values);
                        }
                        case metadata::PropertyScalarType::uint8: {
                            std::vector<std::uint8_t> values;
                            for (const auto value : typed.values) {
                                if (value < 0 || value > std::numeric_limits<std::uint8_t>::max()) invalid("Canonical enum value is out of range");
                                values.push_back(static_cast<std::uint8_t>(value));
                            }
                            return rawBytes(values);
                        }
                        case metadata::PropertyScalarType::int16: {
                            std::vector<std::int16_t> values;
                            for (const auto value : typed.values) {
                                if (value < std::numeric_limits<std::int16_t>::min()
                                        || value > std::numeric_limits<std::int16_t>::max()) invalid("Canonical enum value is out of range");
                                values.push_back(static_cast<std::int16_t>(value));
                            }
                            return rawBytes(values);
                        }
                        case metadata::PropertyScalarType::uint16: {
                            std::vector<std::uint16_t> values;
                            for (const auto value : typed.values) {
                                if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) invalid("Canonical enum value is out of range");
                                values.push_back(static_cast<std::uint16_t>(value));
                            }
                            return rawBytes(values);
                        }
                        case metadata::PropertyScalarType::int32: {
                            std::vector<std::int32_t> values;
                            for (const auto value : typed.values) {
                                if (value < std::numeric_limits<std::int32_t>::min()
                                        || value > std::numeric_limits<std::int32_t>::max()) invalid("Canonical enum value is out of range");
                                values.push_back(static_cast<std::int32_t>(value));
                            }
                            return rawBytes(values);
                        }
                        case metadata::PropertyScalarType::uint32: {
                            std::vector<std::uint32_t> values;
                            for (const auto value : typed.values) {
                                if (value < 0) invalid("Canonical enum value is negative");
                                values.push_back(static_cast<std::uint32_t>(value));
                            }
                            return rawBytes(values);
                        }
                        default: invalid("Canonical enum value type is invalid");
                    }
                } else {
                    alignment = alignof(typename Value::value_type);
                    return rawBytes(typed);
                }
            },
            column.values);
}

Json schemaProperty(const metadata::PropertyColumn& column) {
    Json result;
    switch (column.scalar_type) {
        case metadata::PropertyScalarType::boolean: result["type"] = "BOOLEAN"; break;
        case metadata::PropertyScalarType::string: result["type"] = "STRING"; break;
        case metadata::PropertyScalarType::enumeration:
            if (!column.enum_type.has_value()) invalid("Canonical enum type is absent");
            result["type"] = "ENUM";
            result["enumType"] = *column.enum_type;
            break;
        default:
            result["type"] = propertyType(column.components);
            result["componentType"] = componentType(column.scalar_type);
            break;
    }
    if (column.fixed_array_count.has_value()) {
        result["array"] = true;
        result["count"] = *column.fixed_array_count;
    } else if (!column.array_offsets.empty()) {
        result["array"] = true;
    }
    if (column.normalized) result["normalized"] = true;
    if (column.required) result["required"] = true;
    addLiteral(result, "noData", column.no_data);
    addLiteral(result, "default", column.default_value);
    addLiteral(result, "offset", column.offset);
    addLiteral(result, "scale", column.scale);
    addLiteral(result, "min", column.minimum);
    addLiteral(result, "max", column.maximum);
    return result;
}

Json tableProperty(const metadata::PropertyColumn& column,
                   const CanonicalBufferViewWriter& add_buffer_view) {
    Json result;
    if (!column.array_offsets.empty()) {
        const OffsetBytes offsets = encodeOffsets(column.array_offsets);
        result["arrayOffsetType"] = offsets.type;
        result["arrayOffsets"] = add_buffer_view(
                offsets.bytes, offsets.alignment);
    }
    if (column.scalar_type == metadata::PropertyScalarType::string) {
        const auto& strings = std::get<metadata::StringValues>(column.values).values;
        std::vector<std::uint8_t> values;
        std::vector<std::uint32_t> offsets{0U};
        for (const auto& value : strings) {
            if (values.size() > std::numeric_limits<std::uint32_t>::max()
                                        - value.size()) {
                invalid("Canonical string byte count overflows");
            }
            values.insert(values.end(), value.begin(), value.end());
            offsets.push_back(static_cast<std::uint32_t>(values.size()));
        }
        const OffsetBytes encoded_offsets = encodeOffsets(offsets);
        result["stringOffsetType"] = encoded_offsets.type;
        result["stringOffsets"] = add_buffer_view(
                encoded_offsets.bytes, encoded_offsets.alignment);
        // Empty strings reference no bytes; one unreferenced zero byte keeps the
        // GLB bufferView valid without carrying source padding.
        if (values.empty()) values.push_back(0U);
        result["values"] = add_buffer_view(values, 1U);
        return result;
    }
    std::size_t alignment = 1U;
    const auto bytes = encodeValues(column, alignment);
    result["values"] = add_buffer_view(bytes, alignment);
    return result;
}

Json hierarchyJson(const metadata::PropertyHierarchy& hierarchy) {
    Json nodes = Json::array();
    for (const auto& node : hierarchy.nodes) {
        nodes.push_back({{"parents", node.parents},
                         {"propertyRow", node.property_row},
                         {"propertyTable", node.property_table}});
    }
    return {{"featureNodes", hierarchy.feature_nodes},
            {"nodes", std::move(nodes)}, {"version", 1U}};
}

template <typename Value>
void appendFeatureValue(std::vector<std::uint8_t>& bytes, Value value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(Value));
}

}  // namespace

CanonicalMetadataJson writeCanonicalMetadata(
        const metadata::FeatureMetadata& metadata,
        const CanonicalBufferViewWriter& add_buffer_view) {
    if (metadata.property_tables.empty()) {
        invalid("Canonical metadata contains no property tables");
    }
    std::map<std::string, metadata::PropertyScalarType> enum_types;
    for (const auto& definition : metadata.enums) {
        if (definition.name.empty()
                || !enum_types.emplace(definition.name,
                                       definition.value_type).second) {
            invalid("Canonical metadata enum definition is duplicated");
        }
    }
    Json classes = Json::object();
    Json property_tables = Json::array();
    for (const auto& table : metadata.property_tables) {
        if (table.row_count == 0U) {
            invalid("Canonical metadata contains an empty property table");
        }
        Json properties = Json::object();
        Json schema_properties = Json::object();
        for (const auto& column : table.columns) {
            if (column.scalar_type == metadata::PropertyScalarType::enumeration
                    && (!column.enum_type.has_value()
                        || !column.enum_value_type.has_value()
                        || enum_types.count(*column.enum_type) == 0U
                        || enum_types.at(*column.enum_type)
                                != *column.enum_value_type)) {
                invalid("Canonical metadata enum property is unresolved");
            }
            schema_properties[column.name] = schemaProperty(column);
            properties[column.name] = tableProperty(column, add_buffer_view);
        }
        Json class_value{{"properties", std::move(schema_properties)}};
        if (classes.contains(table.class_name)
                && classes.at(table.class_name) != class_value) {
            invalid("Canonical metadata class definitions conflict");
        }
        classes[table.class_name] = std::move(class_value);
        property_tables.push_back({{"class", table.class_name},
                                   {"count", table.row_count},
                                   {"properties", std::move(properties)}});
    }
    Json schema{{"classes", std::move(classes)},
                {"id", "justai-canonical-metadata-v1"}};
    if (!metadata.enums.empty()) {
        Json enums = Json::object();
        for (const auto& definition : metadata.enums) {
            Json values = Json::array();
            for (const auto& entry : definition.values) {
                values.push_back({{"name", entry.name}, {"value", entry.value}});
            }
            enums[definition.name] = {
                    {"valueType", componentType(definition.value_type)},
                    {"values", std::move(values)}};
        }
        schema["enums"] = std::move(enums);
    }
    CanonicalMetadataJson result;
    result.structural_metadata = {{"propertyTables", std::move(property_tables)},
                                  {"schema", std::move(schema)}};
    if (metadata.hierarchy.has_value()) {
        result.legacy_hierarchy = hierarchyJson(*metadata.hierarchy);
    }
    return result;
}

CanonicalFeatureIdBytes encodeCanonicalFeatureIds(
        const metadata::FeatureIdSet& feature_id_set) {
    if (feature_id_set.feature_count == 0U || feature_id_set.ids.empty()) {
        invalid("Canonical feature ID set is empty");
    }
    const auto encoding = metadata::selectFeatureIdEncoding(
            feature_id_set.feature_count,
            feature_id_set.null_feature_id.has_value());
    if (encoding.null_feature_id != feature_id_set.null_feature_id) {
        invalid("Canonical feature ID null sentinel is not deterministic");
    }
    CanonicalFeatureIdBytes result;
    switch (encoding.component_type) {
        case metadata::FeatureIdComponentType::unsigned_byte:
            result.component_type = kUnsignedByteComponentType; break;
        case metadata::FeatureIdComponentType::unsigned_short:
            result.component_type = kUnsignedShortComponentType; break;
        case metadata::FeatureIdComponentType::float32:
            result.component_type = kFloatComponentType; break;
    }
    for (const std::uint32_t id : feature_id_set.ids) {
        const bool valid_feature = id < feature_id_set.feature_count;
        const bool valid_null = feature_id_set.null_feature_id.has_value()
                && id == *feature_id_set.null_feature_id;
        if (!valid_feature && !valid_null) {
            invalid("Canonical feature ID is outside its declared range");
        }
        if (result.component_type == kUnsignedByteComponentType) {
            appendFeatureValue(result.bytes, static_cast<std::uint8_t>(id));
        } else if (result.component_type == kUnsignedShortComponentType) {
            appendFeatureValue(result.bytes, static_cast<std::uint16_t>(id));
        } else {
            appendFeatureValue(result.bytes, static_cast<float>(id));
        }
    }
    return result;
}

}  // namespace clip_worker::normalization
