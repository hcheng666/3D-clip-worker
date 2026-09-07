#include "clip_worker/metadata/structural_metadata_reader.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace clip_worker::metadata {
namespace {

using Json = nlohmann::json;

constexpr const char* kStructuralMetadata = "EXT_structural_metadata";

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_property_table_invalid,
            message);
}

[[noreturn]] void unsupportedType(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_property_type_unsupported,
            message);
}

[[noreturn]] void unsafe(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_reconstruction_unsafe,
            message);
}

Json parseObject(const std::string& text, const char* description) {
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        unsafe(std::string(description) + " is too large");
    }
    try {
        Json value = Json::parse(text);
        if (!value.is_object()) invalid(std::string(description) + " is invalid");
        return value;
    } catch (const formats::FormatError&) {
        throw;
    } catch (const Json::exception&) {
        invalid(std::string(description) + " contains invalid JSON");
    }
}

void allowedKeys(const Json& object, const std::set<std::string>& keys,
                 const char* description) {
    if (!object.is_object()) invalid(std::string(description) + " is invalid");
    for (const auto& item : object.items()) {
        if (keys.count(item.key()) == 0U) {
            unsafe(std::string(description) + " contains an unsupported field");
        }
    }
}

std::uint32_t unsigned32(const Json& object, const char* field) {
    const auto item = object.find(field);
    if (item == object.end() || !item->is_number_unsigned()
            || item->get<std::uint64_t>()
                    > std::numeric_limits<std::uint32_t>::max()) {
        invalid(std::string("Structural metadata field is invalid: ") + field);
    }
    return item->get<std::uint32_t>();
}

PropertyScalarType integerType(const std::string& value) {
    if (value == "INT8") return PropertyScalarType::int8;
    if (value == "UINT8") return PropertyScalarType::uint8;
    if (value == "INT16") return PropertyScalarType::int16;
    if (value == "UINT16") return PropertyScalarType::uint16;
    if (value == "INT32") return PropertyScalarType::int32;
    if (value == "UINT32") return PropertyScalarType::uint32;
    unsupportedType("Structural metadata integer type is unsupported");
}

PropertyScalarType componentType(const std::string& value) {
    if (value == "FLOAT32") return PropertyScalarType::float32;
    if (value == "FLOAT64") return PropertyScalarType::float64;
    return integerType(value);
}

std::uint32_t components(const std::string& type) {
    if (type == "SCALAR" || type == "STRING" || type == "BOOLEAN"
            || type == "ENUM") {
        return 1U;
    }
    if (type == "VEC2") return 2U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4") return 4U;
    if (type == "MAT2" || type == "MAT3" || type == "MAT4") {
        unsupportedType("Structural metadata matrix property is unsupported");
    }
    unsupportedType("Structural metadata property type is unsupported");
}

const MetadataBufferView& view(
        const std::vector<MetadataBufferView>& views, std::uint32_t index) {
    if (index >= views.size()) invalid("Metadata bufferView is out of range");
    if (views.at(index).bytes == nullptr) {
        invalid("Metadata bufferView bytes are unavailable");
    }
    return views.at(index);
}

template <typename Value>
std::vector<Value> typedValues(const MetadataBufferView& source,
                               std::uint64_t count) {
    if (source.absolute_offset % alignof(Value) != 0U
            || count > std::numeric_limits<std::size_t>::max() / sizeof(Value)
            || source.bytes->size() < count * sizeof(Value)) {
        invalid("Metadata values bufferView is misaligned or too short");
    }
    std::vector<Value> result(static_cast<std::size_t>(count));
    if (!result.empty()) {
        std::memcpy(result.data(), source.bytes->data(),
                    result.size() * sizeof(Value));
    }
    if constexpr (std::is_floating_point_v<Value>) {
        if (std::any_of(result.begin(), result.end(), [](Value value) {
                return !std::isfinite(value);
            })) {
            invalid("Metadata values contain a non-finite number");
        }
    }
    return result;
}

PropertyValues numericValues(PropertyScalarType type,
                             const MetadataBufferView& source,
                             std::uint64_t count) {
    switch (type) {
        case PropertyScalarType::int8:
            return typedValues<std::int8_t>(source, count);
        case PropertyScalarType::uint8:
            return typedValues<std::uint8_t>(source, count);
        case PropertyScalarType::int16:
            return typedValues<std::int16_t>(source, count);
        case PropertyScalarType::uint16:
            return typedValues<std::uint16_t>(source, count);
        case PropertyScalarType::int32:
            return typedValues<std::int32_t>(source, count);
        case PropertyScalarType::uint32:
            return typedValues<std::uint32_t>(source, count);
        case PropertyScalarType::float32:
            return typedValues<float>(source, count);
        case PropertyScalarType::float64:
            return typedValues<double>(source, count);
        case PropertyScalarType::boolean:
        case PropertyScalarType::string:
        case PropertyScalarType::enumeration: break;
    }
    invalid("Metadata numeric values type is invalid");
}

std::vector<std::uint32_t> offsets(
        const Json& property, const char* view_field, const char* type_field,
        std::uint32_t expected_count,
        const std::vector<MetadataBufferView>& views,
        std::uint64_t maximum_encoded_bytes) {
    if (!property.contains(view_field)) {
        invalid(std::string("Metadata offsets are missing: ") + view_field);
    }
    const PropertyScalarType type = property.contains(type_field)
            ? integerType(property.at(type_field).get<std::string>())
            : PropertyScalarType::uint32;
    const auto& source = view(views, unsigned32(property, view_field));
    if (source.bytes->size() > maximum_encoded_bytes) {
        unsafe(std::string("Metadata offset byte limit is exceeded: ")
               + view_field);
    }
    std::vector<std::uint32_t> result;
    result.reserve(expected_count);
    auto append = [&result](const auto& values) {
        for (const auto value : values) {
            if constexpr (std::is_signed_v<decltype(value)>) {
                if (value < 0) invalid("Metadata offset is negative");
            }
            const std::uint64_t converted = static_cast<std::uint64_t>(value);
            if (converted > std::numeric_limits<std::uint32_t>::max()) {
                unsupportedType("64-bit metadata offsets are unsupported");
            }
            result.push_back(static_cast<std::uint32_t>(converted));
        }
    };
    switch (type) {
        case PropertyScalarType::uint8:
            append(typedValues<std::uint8_t>(source, expected_count)); break;
        case PropertyScalarType::uint16:
            append(typedValues<std::uint16_t>(source, expected_count)); break;
        case PropertyScalarType::uint32:
            append(typedValues<std::uint32_t>(source, expected_count)); break;
        default: invalid("Metadata offset type must be unsigned");
    }
    if (result.empty() || result.front() != 0U
            || !std::is_sorted(result.begin(), result.end())) {
        invalid("Metadata offsets are invalid");
    }
    return result;
}

bool validUtf8(const std::string& value) {
    try {
        static_cast<void>(Json(value).dump());
        return true;
    } catch (const Json::type_error&) {
        return false;
    }
}

void appendLiteralScalar(PropertyValues& output, PropertyScalarType type,
                         const Json& value) {
    auto number = [&value]() -> double {
        if (!value.is_number()) invalid("Metadata literal is not numeric");
        const double result = value.get<double>();
        if (!std::isfinite(result)) invalid("Metadata literal is non-finite");
        return result;
    };
    auto signedInteger = [&value](std::int64_t minimum,
                                  std::int64_t maximum) -> std::int64_t {
        if (!value.is_number_integer()) {
            invalid("Metadata signed integer literal is invalid");
        }
        if (value.is_number_unsigned()) {
            const std::uint64_t unsigned_result = value.get<std::uint64_t>();
            if (unsigned_result > static_cast<std::uint64_t>(maximum)) {
                invalid("Metadata signed integer literal is out of range");
            }
            return static_cast<std::int64_t>(unsigned_result);
        }
        const std::int64_t result = value.get<std::int64_t>();
        if (result < minimum || result > maximum) {
            invalid("Metadata signed integer literal is out of range");
        }
        return result;
    };
    auto unsignedInteger = [&value](std::uint64_t maximum) -> std::uint64_t {
        if (!value.is_number_integer()) {
            invalid("Metadata unsigned integer literal is invalid");
        }
        std::uint64_t result = 0U;
        if (value.is_number_unsigned()) {
            result = value.get<std::uint64_t>();
        } else {
            const std::int64_t signed_result = value.get<std::int64_t>();
            if (signed_result < 0) {
                invalid("Metadata unsigned integer literal is negative");
            }
            result = static_cast<std::uint64_t>(signed_result);
        }
        if (result > maximum) {
            invalid("Metadata unsigned integer literal is out of range");
        }
        return result;
    };
    switch (type) {
        case PropertyScalarType::int8:
            std::get<std::vector<std::int8_t>>(output).push_back(
                    static_cast<std::int8_t>(signedInteger(
                            std::numeric_limits<std::int8_t>::min(),
                            std::numeric_limits<std::int8_t>::max()))); return;
        case PropertyScalarType::uint8:
            std::get<std::vector<std::uint8_t>>(output).push_back(
                    static_cast<std::uint8_t>(unsignedInteger(
                            std::numeric_limits<std::uint8_t>::max()))); return;
        case PropertyScalarType::int16:
            std::get<std::vector<std::int16_t>>(output).push_back(
                    static_cast<std::int16_t>(signedInteger(
                            std::numeric_limits<std::int16_t>::min(),
                            std::numeric_limits<std::int16_t>::max()))); return;
        case PropertyScalarType::uint16:
            std::get<std::vector<std::uint16_t>>(output).push_back(
                    static_cast<std::uint16_t>(unsignedInteger(
                            std::numeric_limits<std::uint16_t>::max()))); return;
        case PropertyScalarType::int32:
            std::get<std::vector<std::int32_t>>(output).push_back(
                    static_cast<std::int32_t>(signedInteger(
                            std::numeric_limits<std::int32_t>::min(),
                            std::numeric_limits<std::int32_t>::max()))); return;
        case PropertyScalarType::uint32:
            std::get<std::vector<std::uint32_t>>(output).push_back(
                    static_cast<std::uint32_t>(unsignedInteger(
                            std::numeric_limits<std::uint32_t>::max()))); return;
        case PropertyScalarType::float32:
            std::get<std::vector<float>>(output).push_back(
                    static_cast<float>(number())); return;
        case PropertyScalarType::float64:
            std::get<std::vector<double>>(output).push_back(number()); return;
        case PropertyScalarType::boolean:
            if (!value.is_boolean()) invalid("Metadata boolean literal is invalid");
            std::get<BooleanValues>(output).values.push_back(
                    value.get<bool>() ? 1U : 0U); return;
        case PropertyScalarType::string:
            if (!value.is_string()) invalid("Metadata string literal is invalid");
            std::get<StringValues>(output).values.push_back(
                    value.get<std::string>()); return;
        case PropertyScalarType::enumeration:
            if (!value.is_number_integer()) {
                invalid("Metadata enum literal is invalid");
            }
            if (value.is_number_unsigned()) {
                const std::uint64_t enum_value = value.get<std::uint64_t>();
                if (enum_value
                        > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())) {
                    invalid("Metadata enum literal is out of range");
                }
                std::get<EnumValues>(output).values.push_back(
                        static_cast<std::int64_t>(enum_value));
            } else {
                std::get<EnumValues>(output).values.push_back(
                        value.get<std::int64_t>());
            }
            return;
    }
}

PropertyValues emptyValues(PropertyScalarType type) {
    switch (type) {
        case PropertyScalarType::int8: return std::vector<std::int8_t>{};
        case PropertyScalarType::uint8: return std::vector<std::uint8_t>{};
        case PropertyScalarType::int16: return std::vector<std::int16_t>{};
        case PropertyScalarType::uint16: return std::vector<std::uint16_t>{};
        case PropertyScalarType::int32: return std::vector<std::int32_t>{};
        case PropertyScalarType::uint32: return std::vector<std::uint32_t>{};
        case PropertyScalarType::float32: return std::vector<float>{};
        case PropertyScalarType::float64: return std::vector<double>{};
        case PropertyScalarType::boolean: return BooleanValues{};
        case PropertyScalarType::string: return StringValues{};
        case PropertyScalarType::enumeration: return EnumValues{};
    }
    invalid("Metadata literal type is invalid");
}

void flattenLiteral(PropertyValues& output, PropertyScalarType type,
                    const Json& value) {
    if (value.is_array()) {
        for (const auto& item : value) flattenLiteral(output, type, item);
    } else {
        appendLiteralScalar(output, type, value);
    }
}

std::optional<PropertyValues> literal(
        const Json& object, const char* field, PropertyScalarType type) {
    const auto value = object.find(field);
    if (value == object.end()) return std::nullopt;
    PropertyValues result = emptyValues(type);
    flattenLiteral(result, type, *value);
    return result;
}

struct SchemaProperty {
    PropertyScalarType scalar_type = PropertyScalarType::float64;
    std::uint32_t components = 1U;
    std::optional<std::uint32_t> fixed_array_count;
    bool variable_array = false;
    bool normalized = false;
    bool required = false;
    std::optional<std::string> enum_type;
    std::optional<PropertyScalarType> enum_value_type;
    std::optional<PropertyValues> no_data;
    std::optional<PropertyValues> default_value;
    std::optional<PropertyValues> offset;
    std::optional<PropertyValues> scale;
    std::optional<PropertyValues> minimum;
    std::optional<PropertyValues> maximum;
};

using SchemaClasses = std::map<std::string, std::map<std::string, SchemaProperty>>;

std::map<std::string, EnumDefinition> parseEnums(
        const Json& schema, const MetadataResourceLimits& limits) {
    std::map<std::string, EnumDefinition> result;
    const auto enums = schema.find("enums");
    if (enums == schema.end()) return result;
    if (!enums->is_object() || enums->size() > limits.maximum_enums) {
        unsafe("Structural metadata enum limit is exceeded");
    }
    for (const auto& item : enums->items()) {
        const Json& value = item.value();
        allowedKeys(value, {"name", "description", "valueType", "values"},
                    "metadata enum");
        if (!value.contains("valueType") || !value.at("valueType").is_string()
                || !value.contains("values") || !value.at("values").is_array()) {
            invalid("Metadata enum is incomplete");
        }
        EnumDefinition definition;
        definition.name = item.key();
        definition.value_type = integerType(
                value.at("valueType").get<std::string>());
        std::set<std::string> names;
        std::set<std::int64_t> values;
        for (const auto& entry : value.at("values")) {
            allowedKeys(entry, {"name", "description", "value"},
                        "metadata enum value");
            if (!entry.contains("name") || !entry.at("name").is_string()
                    || !entry.contains("value")
                    || !entry.at("value").is_number_integer()) {
                invalid("Metadata enum value is incomplete");
            }
            std::int64_t enum_value = 0;
            if (entry.at("value").is_number_unsigned()) {
                const std::uint64_t unsigned_value =
                        entry.at("value").get<std::uint64_t>();
                if (unsigned_value
                        > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())) {
                    invalid("Metadata enum value is out of range");
                }
                enum_value = static_cast<std::int64_t>(unsigned_value);
            } else {
                enum_value = entry.at("value").get<std::int64_t>();
            }
            const bool value_in_range = [&definition, enum_value]() {
                switch (definition.value_type) {
                    case PropertyScalarType::int8:
                        return enum_value >= std::numeric_limits<std::int8_t>::min()
                                && enum_value
                                        <= std::numeric_limits<std::int8_t>::max();
                    case PropertyScalarType::uint8:
                        return enum_value >= 0
                                && enum_value
                                        <= std::numeric_limits<std::uint8_t>::max();
                    case PropertyScalarType::int16:
                        return enum_value
                                        >= std::numeric_limits<std::int16_t>::min()
                                && enum_value
                                        <= std::numeric_limits<std::int16_t>::max();
                    case PropertyScalarType::uint16:
                        return enum_value >= 0
                                && enum_value
                                        <= std::numeric_limits<std::uint16_t>::max();
                    case PropertyScalarType::int32:
                        return enum_value
                                        >= std::numeric_limits<std::int32_t>::min()
                                && enum_value
                                        <= std::numeric_limits<std::int32_t>::max();
                    case PropertyScalarType::uint32:
                        return enum_value >= 0;
                    default: return false;
                }
            }();
            if (!value_in_range) {
                invalid("Metadata enum value exceeds its valueType");
            }
            EnumEntry output{entry.at("name").get<std::string>(), enum_value};
            if (output.name.empty() || !names.insert(output.name).second
                    || !values.insert(output.value).second) {
                invalid("Metadata enum value is duplicated");
            }
            definition.values.push_back(std::move(output));
        }
        result.emplace(item.key(), std::move(definition));
    }
    return result;
}

SchemaClasses parseClasses(
        const Json& schema,
        const std::map<std::string, EnumDefinition>& enums,
        const MetadataResourceLimits& limits) {
    const auto classes = schema.find("classes");
    if (classes == schema.end() || !classes->is_object()
            || classes->size() > limits.maximum_classes) {
        invalid("Structural metadata classes are invalid");
    }
    SchemaClasses result;
    for (const auto& class_item : classes->items()) {
        const Json& source_class = class_item.value();
        allowedKeys(source_class, {"name", "description", "properties"},
                    "metadata class");
        if (!source_class.contains("properties")
                || !source_class.at("properties").is_object()
                || source_class.at("properties").size()
                        > limits.maximum_properties) {
            invalid("Metadata class properties are invalid");
        }
        auto& properties = result[class_item.key()];
        for (const auto& property_item
                : source_class.at("properties").items()) {
            const Json& source = property_item.value();
            allowedKeys(source,
                        {"name", "description", "semantic", "type",
                         "componentType", "enumType", "array", "count",
                         "normalized", "offset", "scale", "max", "min",
                         "required", "noData", "default"},
                        "metadata class property");
            if (!source.contains("type") || !source.at("type").is_string()) {
                invalid("Metadata class property type is missing");
            }
            const std::string type = source.at("type").get<std::string>();
            SchemaProperty property;
            property.components = components(type);
            if (type == "STRING") {
                property.scalar_type = PropertyScalarType::string;
            } else if (type == "BOOLEAN") {
                property.scalar_type = PropertyScalarType::boolean;
            } else if (type == "ENUM") {
                if (!source.contains("enumType")
                        || !source.at("enumType").is_string()
                        || enums.count(source.at("enumType").get<std::string>())
                                == 0U) {
                    invalid("Metadata enum property is invalid");
                }
                property.scalar_type = PropertyScalarType::enumeration;
                property.enum_type = source.at("enumType").get<std::string>();
                property.enum_value_type =
                        enums.at(*property.enum_type).value_type;
            } else {
                if (!source.contains("componentType")
                        || !source.at("componentType").is_string()) {
                    invalid("Metadata numeric component type is missing");
                }
                property.scalar_type = componentType(
                        source.at("componentType").get<std::string>());
            }
            const bool is_array = source.value("array", false);
            if (is_array && source.contains("count")) {
                property.fixed_array_count = unsigned32(source, "count");
                if (*property.fixed_array_count == 0U) {
                    invalid("Metadata fixed array count is zero");
                }
            } else if (is_array) {
                property.variable_array = true;
            } else if (source.contains("count")) {
                invalid("Metadata count requires array=true");
            }
            property.normalized = source.value("normalized", false);
            property.required = source.value("required", false);
            property.no_data = literal(source, "noData", property.scalar_type);
            property.default_value = literal(
                    source, "default", property.scalar_type);
            property.offset = literal(source, "offset", property.scalar_type);
            property.scale = literal(source, "scale", property.scalar_type);
            property.minimum = literal(source, "min", property.scalar_type);
            property.maximum = literal(source, "max", property.scalar_type);
            properties.emplace(property_item.key(), std::move(property));
        }
    }
    return result;
}

PropertyValues enumValues(PropertyScalarType value_type,
                          const MetadataBufferView& source,
                          std::uint64_t count) {
    EnumValues result;
    result.values.reserve(static_cast<std::size_t>(count));
    auto append = [&result](const auto& values) {
        for (const auto value : values) {
            result.values.push_back(static_cast<std::int64_t>(value));
        }
    };
    switch (value_type) {
        case PropertyScalarType::int8:
            append(typedValues<std::int8_t>(source, count)); break;
        case PropertyScalarType::uint8:
            append(typedValues<std::uint8_t>(source, count)); break;
        case PropertyScalarType::int16:
            append(typedValues<std::int16_t>(source, count)); break;
        case PropertyScalarType::uint16:
            append(typedValues<std::uint16_t>(source, count)); break;
        case PropertyScalarType::int32:
            append(typedValues<std::int32_t>(source, count)); break;
        case PropertyScalarType::uint32: {
            const auto values = typedValues<std::uint32_t>(source, count);
            for (const auto value : values) result.values.push_back(value);
            break;
        }
        default: invalid("Metadata enum value type is invalid");
    }
    return result;
}

PropertyColumn readProperty(
        const std::string& name, const SchemaProperty& definition,
        const Json& source, std::uint32_t row_count,
        const std::vector<MetadataBufferView>& views,
        const MetadataResourceLimits& limits) {
    allowedKeys(source,
                {"values", "arrayOffsets", "stringOffsets",
                 "arrayOffsetType", "stringOffsetType", "offset", "scale",
                 "max", "min"},
                "metadata property table property");
    if (!source.contains("values")) invalid("Metadata property values are missing");
    PropertyColumn result;
    result.name = name;
    result.scalar_type = definition.scalar_type;
    result.components = definition.components;
    result.fixed_array_count = definition.fixed_array_count;
    result.normalized = definition.normalized;
    result.required = definition.required;
    result.no_data = definition.no_data;
    result.default_value = definition.default_value;
    result.offset = source.contains("offset")
            ? literal(source, "offset", result.scalar_type) : definition.offset;
    result.scale = source.contains("scale")
            ? literal(source, "scale", result.scalar_type) : definition.scale;
    result.minimum = source.contains("min")
            ? literal(source, "min", result.scalar_type) : definition.minimum;
    result.maximum = source.contains("max")
            ? literal(source, "max", result.scalar_type) : definition.maximum;
    result.enum_type = definition.enum_type;
    result.enum_value_type = definition.enum_value_type;
    std::uint64_t logical_values = row_count;
    if (definition.variable_array) {
        result.array_offsets = offsets(
                source, "arrayOffsets", "arrayOffsetType", row_count + 1U,
                views, limits.maximum_array_offset_bytes);
        logical_values = result.array_offsets.back();
    } else if (definition.fixed_array_count.has_value()) {
        logical_values *= *definition.fixed_array_count;
    }
    if (logical_values > std::numeric_limits<std::uint64_t>::max()
                                 / result.components) {
        unsafe("Metadata property value count overflows");
    }
    const std::uint64_t scalar_count = logical_values * result.components;
    const auto& values_view = view(views, unsigned32(source, "values"));
    if (result.scalar_type == PropertyScalarType::boolean) {
        if (values_view.bytes->size() < (scalar_count + 7U) / 8U) {
            invalid("Metadata boolean values bufferView is too short");
        }
        BooleanValues values;
        values.values.reserve(static_cast<std::size_t>(scalar_count));
        for (std::uint64_t index = 0U; index < scalar_count; ++index) {
            values.values.push_back(static_cast<std::uint8_t>(
                    (values_view.bytes->at(static_cast<std::size_t>(index / 8U))
                     >> (index % 8U))
                    & 1U));
        }
        result.values = std::move(values);
    } else if (result.scalar_type == PropertyScalarType::string) {
        if (logical_values >= std::numeric_limits<std::uint32_t>::max()) {
            unsafe("Metadata string offset count exceeds uint32");
        }
        const auto string_offsets = offsets(
                source, "stringOffsets", "stringOffsetType",
                static_cast<std::uint32_t>(logical_values + 1U), views,
                limits.maximum_string_offset_bytes);
        if (string_offsets.back() > values_view.bytes->size()) {
            invalid("Metadata string offsets exceed values bufferView");
        }
        StringValues values;
        values.values.reserve(static_cast<std::size_t>(logical_values));
        for (std::size_t index = 0U; index + 1U < string_offsets.size(); ++index) {
            const std::string value(
                    reinterpret_cast<const char*>(values_view.bytes->data()
                                                   + string_offsets[index]),
                    string_offsets[index + 1U] - string_offsets[index]);
            if (!validUtf8(value)) invalid("Metadata string is not UTF-8");
            values.values.push_back(value);
        }
        result.values = std::move(values);
    } else if (result.scalar_type == PropertyScalarType::enumeration) {
        result.values = enumValues(*definition.enum_value_type, values_view,
                                   scalar_count);
    } else {
        result.values = numericValues(result.scalar_type, values_view,
                                      scalar_count);
    }
    return result;
}

}  // namespace

FeatureMetadata readStructuralMetadata(
        const std::string& gltf_json,
        const std::vector<MetadataBufferView>& buffer_views,
        const MetadataResourceLimits& limits,
        const std::optional<std::string>& external_schema_json) {
    const Json root = parseObject(gltf_json, "glTF document");
    if (!root.contains("extensions") || !root.at("extensions").is_object()
            || !root.at("extensions").contains(kStructuralMetadata)) {
        return {};
    }
    const Json& extension = root.at("extensions").at(kStructuralMetadata);
    allowedKeys(extension,
                {"schema", "schemaUri", "propertyTables", "propertyTextures",
                 "propertyAttributes", "statistics"},
                "EXT_structural_metadata");
    if (extension.contains("propertyTextures")) {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_property_texture_unsupported,
                "Property textures are unsupported");
    }
    if (extension.contains("propertyAttributes")) {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_property_attribute_unsupported,
                "Property attributes are unsupported");
    }
    if (extension.contains("statistics")) {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_statistics_unsupported,
                "Structural metadata statistics are unsupported");
    }
    Json schema;
    if (extension.contains("schema")) {
        if (extension.contains("schemaUri")) {
            invalid("Structural metadata schema source is ambiguous");
        }
        schema = extension.at("schema");
    } else if (extension.contains("schemaUri")
               && external_schema_json.has_value()) {
        if (external_schema_json->size() > limits.maximum_schema_bytes) {
            unsafe("External structural metadata schema exceeds configured limits");
        }
        schema = parseObject(*external_schema_json, "external metadata schema");
    } else {
        unsafe("Structural metadata schema is unavailable");
    }
    const std::string schema_text = schema.dump();
    if (schema_text.size() > limits.maximum_schema_bytes) {
        unsafe("Structural metadata schema exceeds configured limits");
    }
    allowedKeys(schema, {"id", "name", "description", "version", "classes",
                         "enums"},
                "structural metadata schema");
    const auto enums = parseEnums(schema, limits);
    const auto classes = parseClasses(schema, enums, limits);
    FeatureMetadata result;
    const auto tables = extension.find("propertyTables");
    if (tables == extension.end()) return result;
    if (!tables->is_array() || tables->size() > limits.maximum_property_tables) {
        unsafe("Structural metadata property table limit is exceeded");
    }
    std::set<std::string> referenced_enums;
    for (const auto& source_table : *tables) {
        allowedKeys(source_table,
                    {"name", "class", "count", "properties"},
                    "metadata property table");
        if (!source_table.contains("class")
                || !source_table.at("class").is_string()
                || !source_table.contains("count")
                || !source_table.contains("properties")
                || !source_table.at("properties").is_object()) {
            invalid("Metadata property table is incomplete");
        }
        const std::string class_name =
                source_table.at("class").get<std::string>();
        const auto class_definition = classes.find(class_name);
        if (class_definition == classes.end()) {
            invalid("Metadata property table class is undefined");
        }
        PropertyTable table;
        table.class_name = class_name;
        table.row_count = unsigned32(source_table, "count");
        for (const auto& property
                : source_table.at("properties").items()) {
            const auto definition = class_definition->second.find(property.key());
            if (definition == class_definition->second.end()) {
                invalid("Metadata property is absent from its class");
            }
            table.columns.push_back(readProperty(
                    property.key(), definition->second, property.value(),
                    table.row_count, buffer_views, limits));
            if (definition->second.enum_type.has_value()) {
                referenced_enums.insert(*definition->second.enum_type);
            }
        }
        for (const auto& definition : class_definition->second) {
            if (definition.second.required
                    && !source_table.at("properties")
                                .contains(definition.first)) {
                invalid("Required metadata property is absent from its table");
            }
        }
        std::sort(table.columns.begin(), table.columns.end(),
                  [](const auto& left, const auto& right) {
                      return left.name < right.name;
                  });
        validatePropertyTable(table, limits);
        result.property_tables.push_back(std::move(table));
    }
    for (const auto& name : referenced_enums) result.enums.push_back(enums.at(name));
    std::sort(result.enums.begin(), result.enums.end(),
              [](const auto& left, const auto& right) {
                  return left.name < right.name;
              });
    return result;
}

}  // namespace clip_worker::metadata
