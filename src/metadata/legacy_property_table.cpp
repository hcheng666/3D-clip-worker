#include "clip_worker/metadata/legacy_property_table.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>

#include <nlohmann/json.hpp>

namespace clip_worker::metadata {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(formats::FormatErrorCode::invalid_feature_table,
                               message);
}

[[noreturn]] void unsupported(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_batch_table_unsupported,
            message);
}

Json parseObject(const std::string& text) {
    if (text.empty()) return Json::object();
    try {
        Json value = Json::parse(text);
        if (!value.is_object()) invalid("Batch Table must be a JSON object");
        return value;
    } catch (const formats::FormatError&) {
        throw;
    } catch (const Json::exception&) {
        invalid("Batch Table contains invalid JSON");
    }
}

void allowedKeys(const Json& object, const std::set<std::string>& names) {
    for (const auto& item : object.items()) {
        if (names.find(item.key()) == names.end()) {
            unsupported("Batch Table binary property contains an unsupported field");
        }
    }
}

std::size_t unsignedOffset(const Json& object) {
    const auto item = object.find("byteOffset");
    if (item == object.end() || !item->is_number_unsigned()
        || *item > std::numeric_limits<std::size_t>::max()) {
        invalid("Batch Table binary byteOffset is invalid");
    }
    return item->get<std::size_t>();
}

template <typename T>
T readScalar(formats::ByteView bytes, std::size_t offset) {
    if (!bytes.contains(offset, sizeof(T))) {
        invalid("Batch Table binary property exceeds its section");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::uint32_t componentCount(const std::string& type) {
    if (type == "SCALAR") return 1U;
    if (type == "VEC2") return 2U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4") return 4U;
    unsupported("Batch Table binary property type is unsupported");
}

struct NumericComponent {
    std::size_t size = 0U;
    std::size_t alignment = 0U;
    double (*read)(formats::ByteView, std::size_t) = nullptr;
};

template <typename T>
double readNumeric(formats::ByteView bytes, std::size_t offset) {
    return static_cast<double>(readScalar<T>(bytes, offset));
}

NumericComponent numericComponent(const std::string& type) {
    if (type == "BYTE")
        return {sizeof(std::int8_t), alignof(std::int8_t), readNumeric<std::int8_t>};
    if (type == "UNSIGNED_BYTE")
        return {sizeof(std::uint8_t), alignof(std::uint8_t), readNumeric<std::uint8_t>};
    if (type == "SHORT")
        return {sizeof(std::int16_t), alignof(std::int16_t), readNumeric<std::int16_t>};
    if (type == "UNSIGNED_SHORT")
        return {sizeof(std::uint16_t), alignof(std::uint16_t), readNumeric<std::uint16_t>};
    if (type == "INT")
        return {sizeof(std::int32_t), alignof(std::int32_t), readNumeric<std::int32_t>};
    if (type == "UNSIGNED_INT")
        return {sizeof(std::uint32_t), alignof(std::uint32_t), readNumeric<std::uint32_t>};
    if (type == "FLOAT")
        return {sizeof(float), alignof(float), readNumeric<float>};
    if (type == "DOUBLE")
        return {sizeof(double), alignof(double), readNumeric<double>};
    unsupported("Batch Table binary componentType is unsupported");
}

mesh::LegacyPropertyColumn jsonColumn(
        const std::string& name, const Json& values,
        std::uint32_t feature_count, std::uint64_t& string_bytes) {
    if (!values.is_array() || values.size() != feature_count) {
        invalid("Batch Table JSON row count differs from feature count");
    }
    mesh::LegacyPropertyColumn column;
    column.name = name;
    if (values.empty()) return column;
    if (values.front().is_boolean()) {
        column.kind = mesh::LegacyPropertyKind::boolean;
        for (const auto& value : values) {
            if (!value.is_boolean()) invalid("Batch Table JSON types are mixed");
            column.boolean_values.push_back(value.get<bool>() ? 1U : 0U);
        }
    } else if (values.front().is_number()) {
        column.kind = mesh::LegacyPropertyKind::numeric;
        for (const auto& value : values) {
            if (!value.is_number()) invalid("Batch Table JSON types are mixed");
            const double number = value.get<double>();
            if (!std::isfinite(number)) invalid("Batch Table JSON number is non-finite");
            column.numeric_values.push_back(number);
        }
    } else if (values.front().is_string()) {
        column.kind = mesh::LegacyPropertyKind::string;
        for (const auto& value : values) {
            if (!value.is_string()) invalid("Batch Table JSON types are mixed");
            const std::string text = value.get<std::string>();
            if (string_bytes > std::numeric_limits<std::uint64_t>::max()
                                       - text.size()) {
                unsupported("Batch Table string byte count overflows");
            }
            string_bytes += text.size();
            column.string_values.push_back(text);
        }
    } else {
        unsupported("Batch Table JSON property type is unsupported");
    }
    return column;
}

mesh::LegacyPropertyColumn binaryColumn(
        const std::string& name, const Json& reference,
        formats::ByteView bytes, std::uint32_t feature_count) {
    allowedKeys(reference, {"byteOffset", "componentType", "type"});
    if (!reference.contains("componentType")
        || !reference.at("componentType").is_string()
        || !reference.contains("type") || !reference.at("type").is_string()) {
        invalid("Batch Table binary descriptor is incomplete");
    }
    const std::size_t offset = unsignedOffset(reference);
    const NumericComponent component = numericComponent(
            reference.at("componentType").get<std::string>());
    const std::uint32_t components = componentCount(
            reference.at("type").get<std::string>());
    const std::uint64_t value_count =
            static_cast<std::uint64_t>(feature_count) * components;
    if (offset % component.alignment != 0U
        || value_count > std::numeric_limits<std::size_t>::max() / component.size
        || !bytes.contains(offset, static_cast<std::size_t>(value_count)
                                   * component.size)) {
        invalid("Batch Table binary property is misaligned or out of range");
    }
    mesh::LegacyPropertyColumn column;
    column.name = name;
    column.kind = mesh::LegacyPropertyKind::numeric;
    column.components = components;
    column.numeric_values.reserve(static_cast<std::size_t>(value_count));
    for (std::size_t index = 0U; index < value_count; ++index) {
        const double value = component.read(bytes, offset + index * component.size);
        if (!std::isfinite(value)) invalid("Batch Table binary value is non-finite");
        column.numeric_values.push_back(value);
    }
    return column;
}

}  // namespace

mesh::LegacyPropertyTable readLegacyPropertyTable(
        const std::string& json_text, formats::ByteView binary,
        std::uint32_t feature_count, const LegacyPropertyLimits& limits) {
    const Json table_json = parseObject(json_text);
    if (feature_count > limits.maximum_feature_rows
        || table_json.size() > limits.maximum_properties
        || binary.size() > limits.maximum_binary_bytes) {
        unsupported("Batch Table exceeds the configured property limits");
    }
    mesh::LegacyPropertyTable table;
    table.feature_count = feature_count;
    std::uint64_t string_bytes = 0U;
    for (const auto& item : table_json.items()) {
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
            unsupported("Batch Table strings exceed the configured limit");
        }
    }
    std::sort(table.columns.begin(), table.columns.end(),
              [](const auto& left, const auto& right) {
                  return left.name < right.name;
              });
    return table;
}

mesh::LegacyPropertyTable compactLegacyPropertyTable(
        const mesh::LegacyPropertyTable& source,
        const std::vector<std::uint32_t>& retained_source_ids) {
    mesh::LegacyPropertyTable result;
    result.feature_count = static_cast<std::uint32_t>(retained_source_ids.size());
    result.columns.reserve(source.columns.size());
    for (const auto& source_column : source.columns) {
        mesh::LegacyPropertyColumn column;
        column.name = source_column.name;
        column.kind = source_column.kind;
        column.components = source_column.components;
        for (const std::uint32_t id : retained_source_ids) {
            if (id >= source.feature_count) {
                invalid("Retained feature ID exceeds the source property table");
            }
            switch (column.kind) {
                case mesh::LegacyPropertyKind::numeric: {
                    const std::size_t begin = static_cast<std::size_t>(id)
                                              * column.components;
                    column.numeric_values.insert(
                            column.numeric_values.end(),
                            source_column.numeric_values.begin()
                                    + static_cast<std::ptrdiff_t>(begin),
                            source_column.numeric_values.begin()
                                    + static_cast<std::ptrdiff_t>(
                                            begin + column.components));
                    break;
                }
                case mesh::LegacyPropertyKind::boolean:
                    column.boolean_values.push_back(
                            source_column.boolean_values.at(id));
                    break;
                case mesh::LegacyPropertyKind::string:
                    column.string_values.push_back(
                            source_column.string_values.at(id));
                    break;
            }
        }
        result.columns.push_back(std::move(column));
    }
    return result;
}

}  // namespace clip_worker::metadata
