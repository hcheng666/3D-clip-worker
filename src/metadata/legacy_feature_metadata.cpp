#include "clip_worker/metadata/legacy_feature_metadata.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace clip_worker::metadata {
namespace {

using Json = nlohmann::json;

constexpr const char* kExtensions = "extensions";
constexpr const char* kHierarchyExtension =
        "3DTILES_batch_table_hierarchy";

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_property_table_invalid,
            message);
}

[[noreturn]] void hierarchyInvalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_hierarchy_invalid, message);
}

[[noreturn]] void unsupported(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_property_type_unsupported,
            message);
}

Json parseObject(const std::string& text) {
    try {
        Json value = text.empty() ? Json::object() : Json::parse(text);
        if (!value.is_object()) invalid("Batch Table must be a JSON object");
        return value;
    } catch (const formats::FormatError&) {
        throw;
    } catch (const Json::exception&) {
        invalid("Batch Table contains invalid JSON");
    }
}

std::size_t unsignedOffset(const Json& object) {
    const auto value = object.find("byteOffset");
    if (value == object.end() || !value->is_number_unsigned()
            || value->get<std::uint64_t>()
                    > std::numeric_limits<std::size_t>::max()) {
        invalid("Batch Table binary byteOffset is invalid");
    }
    return value->get<std::size_t>();
}

std::uint32_t componentCount(const std::string& type) {
    if (type == "SCALAR") return 1U;
    if (type == "VEC2") return 2U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4") return 4U;
    unsupported("Batch Table property vector type is unsupported");
}

template <typename Value>
std::vector<Value> readBinaryValues(formats::ByteView binary,
                                    std::size_t offset,
                                    std::uint64_t value_count) {
    if (offset % alignof(Value) != 0U
            || value_count > std::numeric_limits<std::size_t>::max()
                                     / sizeof(Value)
            || !binary.contains(offset,
                                static_cast<std::size_t>(value_count)
                                        * sizeof(Value))) {
        invalid("Batch Table binary property is misaligned or out of range");
    }
    std::vector<Value> result(static_cast<std::size_t>(value_count));
    if (!result.empty()) {
        std::memcpy(result.data(), binary.data() + offset,
                    result.size() * sizeof(Value));
    }
    if constexpr (std::is_floating_point_v<Value>) {
        if (std::any_of(result.begin(), result.end(), [](Value value) {
                return !std::isfinite(value);
            })) {
            invalid("Batch Table binary property contains non-finite values");
        }
    }
    return result;
}

PropertyColumn binaryColumn(const std::string& name, const Json& descriptor,
                            formats::ByteView binary, std::uint32_t row_count) {
    static const std::set<std::string> kAllowed{
            "byteOffset", "componentType", "type"};
    if (!descriptor.is_object()) invalid("Batch Table binary property is invalid");
    for (const auto& item : descriptor.items()) {
        if (kAllowed.count(item.key()) == 0U) {
            unsupported("Batch Table binary descriptor field is unsupported");
        }
    }
    if (!descriptor.contains("componentType")
            || !descriptor.at("componentType").is_string()
            || !descriptor.contains("type")
            || !descriptor.at("type").is_string()) {
        invalid("Batch Table binary descriptor is incomplete");
    }
    PropertyColumn result;
    result.name = name;
    result.components = componentCount(descriptor.at("type").get<std::string>());
    const std::uint64_t value_count =
            static_cast<std::uint64_t>(row_count) * result.components;
    const std::size_t offset = unsignedOffset(descriptor);
    const std::string component =
            descriptor.at("componentType").get<std::string>();
    if (component == "BYTE") {
        result.scalar_type = PropertyScalarType::int8;
        result.values = readBinaryValues<std::int8_t>(binary, offset, value_count);
    } else if (component == "UNSIGNED_BYTE") {
        result.scalar_type = PropertyScalarType::uint8;
        result.values = readBinaryValues<std::uint8_t>(binary, offset, value_count);
    } else if (component == "SHORT") {
        result.scalar_type = PropertyScalarType::int16;
        result.values = readBinaryValues<std::int16_t>(binary, offset, value_count);
    } else if (component == "UNSIGNED_SHORT") {
        result.scalar_type = PropertyScalarType::uint16;
        result.values = readBinaryValues<std::uint16_t>(binary, offset, value_count);
    } else if (component == "INT") {
        result.scalar_type = PropertyScalarType::int32;
        result.values = readBinaryValues<std::int32_t>(binary, offset, value_count);
    } else if (component == "UNSIGNED_INT") {
        result.scalar_type = PropertyScalarType::uint32;
        result.values = readBinaryValues<std::uint32_t>(binary, offset, value_count);
    } else if (component == "FLOAT") {
        result.scalar_type = PropertyScalarType::float32;
        result.values = readBinaryValues<float>(binary, offset, value_count);
    } else if (component == "DOUBLE") {
        result.scalar_type = PropertyScalarType::float64;
        result.values = readBinaryValues<double>(binary, offset, value_count);
    } else {
        unsupported("Batch Table binary component type is unsupported");
    }
    return result;
}

PropertyColumn jsonColumn(const std::string& name, const Json& values,
                          std::uint32_t row_count) {
    if (!values.is_array() || values.size() != row_count) {
        invalid("Batch Table JSON row count differs from its table");
    }
    PropertyColumn result;
    result.name = name;
    if (values.empty()) {
        result.scalar_type = PropertyScalarType::float64;
        result.values = std::vector<double>{};
        return result;
    }
    if (values.front().is_boolean()) {
        result.scalar_type = PropertyScalarType::boolean;
        BooleanValues output;
        output.values.reserve(values.size());
        for (const auto& value : values) {
            if (!value.is_boolean()) invalid("Batch Table JSON types are mixed");
            output.values.push_back(value.get<bool>() ? 1U : 0U);
        }
        result.values = std::move(output);
        return result;
    }
    if (values.front().is_string()) {
        result.scalar_type = PropertyScalarType::string;
        StringValues output;
        output.values.reserve(values.size());
        for (const auto& value : values) {
            if (!value.is_string()) invalid("Batch Table JSON types are mixed");
            output.values.push_back(value.get<std::string>());
        }
        result.values = std::move(output);
        return result;
    }
    const bool all_signed = std::all_of(values.begin(), values.end(),
            [](const Json& value) {
                return value.is_number_integer()
                        && !value.is_number_unsigned()
                        && value.get<std::int64_t>()
                                >= std::numeric_limits<std::int32_t>::min()
                        && value.get<std::int64_t>()
                                <= std::numeric_limits<std::int32_t>::max();
            });
    if (all_signed) {
        result.scalar_type = PropertyScalarType::int32;
        std::vector<std::int32_t> output;
        output.reserve(values.size());
        for (const auto& value : values) {
            output.push_back(value.get<std::int32_t>());
        }
        result.values = std::move(output);
        return result;
    }
    const bool all_unsigned = std::all_of(values.begin(), values.end(),
            [](const Json& value) {
                return value.is_number_unsigned()
                        && value.get<std::uint64_t>()
                                <= std::numeric_limits<std::uint32_t>::max();
            });
    if (all_unsigned) {
        result.scalar_type = PropertyScalarType::uint32;
        std::vector<std::uint32_t> output;
        output.reserve(values.size());
        for (const auto& value : values) {
            output.push_back(value.get<std::uint32_t>());
        }
        result.values = std::move(output);
        return result;
    }
    result.scalar_type = PropertyScalarType::float64;
    std::vector<double> output;
    output.reserve(values.size());
    for (const auto& value : values) {
        if (!value.is_number()) unsupported("Batch Table JSON type is unsupported");
        const double number = value.get<double>();
        if (!std::isfinite(number)) invalid("Batch Table JSON number is non-finite");
        output.push_back(number);
    }
    result.values = std::move(output);
    return result;
}

PropertyTable readTable(const Json& object, const std::string& class_name,
                        std::uint32_t row_count, formats::ByteView binary,
                        bool skip_extensions) {
    if (!object.is_object()) invalid("Batch Table property set is invalid");
    PropertyTable result;
    result.class_name = class_name;
    result.row_count = row_count;
    for (const auto& item : object.items()) {
        if (skip_extensions && item.key() == kExtensions) continue;
        if (item.value().is_array()) {
            result.columns.push_back(
                    jsonColumn(item.key(), item.value(), row_count));
        } else if (item.value().is_object()) {
            result.columns.push_back(
                    binaryColumn(item.key(), item.value(), binary, row_count));
        } else {
            unsupported("Batch Table property representation is unsupported");
        }
    }
    std::sort(result.columns.begin(), result.columns.end(),
              [](const auto& left, const auto& right) {
                  return left.name < right.name;
              });
    return result;
}

std::vector<std::uint32_t> unsignedVector(
        const Json& source, std::uint32_t count, formats::ByteView binary,
        const char* description) {
    if (source.is_array()) {
        if (source.size() != count) hierarchyInvalid(
                std::string(description) + " count is invalid");
        std::vector<std::uint32_t> result;
        result.reserve(source.size());
        for (const auto& value : source) {
            if (!value.is_number_unsigned()
                    || value.get<std::uint64_t>()
                            > std::numeric_limits<std::uint32_t>::max()) {
                hierarchyInvalid(std::string(description) + " value is invalid");
            }
            result.push_back(value.get<std::uint32_t>());
        }
        return result;
    }
    PropertyColumn column = binaryColumn(description, source, binary, count);
    if (column.components != 1U) {
        hierarchyInvalid(std::string(description) + " must be SCALAR");
    }
    return std::visit(
            [description](const auto& values) -> std::vector<std::uint32_t> {
                using Value = std::decay_t<decltype(values)>;
                if constexpr (std::is_same_v<Value, std::vector<std::uint8_t>>
                              || std::is_same_v<Value,
                                                std::vector<std::uint16_t>>
                              || std::is_same_v<Value,
                                                std::vector<std::uint32_t>>) {
                    return std::vector<std::uint32_t>(values.begin(), values.end());
                } else {
                    hierarchyInvalid(std::string(description)
                                     + " component type is invalid");
                }
            },
            column.values);
}

PropertyHierarchy readHierarchy(const Json& extension,
                                std::vector<PropertyTable>& tables,
                                formats::ByteView binary,
                                std::uint32_t feature_count) {
    static const std::set<std::string> kAllowed{
            "classes", "classIds", "instancesLength", "parentCounts",
            "parentIds"};
    if (!extension.is_object()) hierarchyInvalid("Batch hierarchy is invalid");
    for (const auto& item : extension.items()) {
        if (kAllowed.count(item.key()) == 0U) {
            throw formats::FormatError(
                    formats::FormatErrorCode::metadata_relationship_unsupported,
                    "Batch hierarchy field is unsupported");
        }
    }
    if (!extension.contains("instancesLength")
            || !extension.at("instancesLength").is_number_unsigned()
            || !extension.contains("classes")
            || !extension.at("classes").is_array()
            || !extension.contains("classIds")) {
        hierarchyInvalid("Batch hierarchy fields are incomplete");
    }
    const std::uint64_t raw_instances =
            extension.at("instancesLength").get<std::uint64_t>();
    if (raw_instances > std::numeric_limits<std::uint32_t>::max()
            || feature_count > raw_instances) {
        hierarchyInvalid("Batch hierarchy instance count is invalid");
    }
    const auto instances = static_cast<std::uint32_t>(raw_instances);
    const Json& classes = extension.at("classes");
    std::vector<std::uint32_t> class_lengths;
    class_lengths.reserve(classes.size());
    for (std::size_t index = 0U; index < classes.size(); ++index) {
        const Json& source_class = classes.at(index);
        if (!source_class.is_object() || !source_class.contains("name")
                || !source_class.at("name").is_string()
                || !source_class.contains("length")
                || !source_class.at("length").is_number_unsigned()
                || !source_class.contains("instances")) {
            hierarchyInvalid("Batch hierarchy class is incomplete");
        }
        const std::uint64_t raw_length =
                source_class.at("length").get<std::uint64_t>();
        if (raw_length > std::numeric_limits<std::uint32_t>::max()) {
            hierarchyInvalid("Batch hierarchy class length is invalid");
        }
        const auto length = static_cast<std::uint32_t>(raw_length);
        class_lengths.push_back(length);
        tables.push_back(readTable(
                source_class.at("instances"),
                source_class.at("name").get<std::string>(), length, binary,
                false));
    }
    const auto class_ids = unsignedVector(
            extension.at("classIds"), instances, binary, "classIds");
    std::vector<std::uint32_t> class_rows(classes.size(), 0U);
    PropertyHierarchy result;
    result.nodes.reserve(instances);
    for (const std::uint32_t class_id : class_ids) {
        if (class_id >= classes.size()
                || class_rows.at(class_id) >= class_lengths.at(class_id)) {
            hierarchyInvalid("Batch hierarchy class ID is invalid");
        }
        result.nodes.push_back(
                {class_id + 1U, class_rows.at(class_id)++, {}});
    }
    if (class_rows != class_lengths) {
        hierarchyInvalid("Batch hierarchy class lengths do not close");
    }
    if (extension.contains("parentCounts")) {
        if (!extension.contains("parentIds")) {
            hierarchyInvalid("Batch hierarchy parent IDs are missing");
        }
        const auto counts = unsignedVector(
                extension.at("parentCounts"), instances, binary,
                "parentCounts");
        const std::uint64_t parent_total = std::accumulate(
                counts.begin(), counts.end(), 0ULL);
        if (parent_total > std::numeric_limits<std::uint32_t>::max()) {
            hierarchyInvalid("Batch hierarchy parent count overflows");
        }
        const auto parents = unsignedVector(
                extension.at("parentIds"),
                static_cast<std::uint32_t>(parent_total), binary,
                "parentIds");
        std::size_t cursor = 0U;
        for (std::size_t node = 0U; node < counts.size(); ++node) {
            for (std::uint32_t index = 0U; index < counts.at(node); ++index) {
                const std::uint32_t parent = parents.at(cursor++);
                if (parent == node || parent >= instances) {
                    hierarchyInvalid("Batch hierarchy parent is invalid");
                }
                result.nodes.at(node).parents.push_back(parent);
            }
        }
    } else if (extension.contains("parentIds")) {
        const auto parents = unsignedVector(
                extension.at("parentIds"), instances, binary, "parentIds");
        for (std::uint32_t node = 0U; node < instances; ++node) {
            if (parents.at(node) >= instances) {
                hierarchyInvalid("Batch hierarchy parent is invalid");
            }
            // The legacy single-parent encoding uses self as the root sentinel.
            if (parents.at(node) != node) {
                result.nodes.at(node).parents.push_back(parents.at(node));
            }
        }
    }
    result.feature_nodes.resize(feature_count);
    std::iota(result.feature_nodes.begin(), result.feature_nodes.end(), 0U);
    return result;
}

}  // namespace

FeatureMetadata readLegacyFeatureMetadata(
        const std::string& json_text, formats::ByteView binary,
        std::uint32_t feature_count, const MetadataResourceLimits& limits) {
    if (feature_count > limits.maximum_feature_rows
            || binary.size() > limits.maximum_values_bytes) {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_reconstruction_unsafe,
                "Batch Table exceeds metadata limits");
    }
    const Json root = parseObject(json_text);
    FeatureMetadata result;
    result.property_tables.push_back(readTable(
            root, "LegacyFeature", feature_count, binary, true));
    result.primary_property_table = 0U;
    if (root.contains(kExtensions)) {
        const Json& extensions = root.at(kExtensions);
        if (!extensions.is_object()) invalid("Batch Table extensions are invalid");
        for (const auto& item : extensions.items()) {
            if (item.key() != kHierarchyExtension) {
                throw formats::FormatError(
                        formats::FormatErrorCode::metadata_relationship_unsupported,
                        "Batch Table metadata extension is unsupported");
            }
        }
        if (extensions.contains(kHierarchyExtension)) {
            result.hierarchy = readHierarchy(
                    extensions.at(kHierarchyExtension), result.property_tables,
                    binary, feature_count);
        }
    }
    if (result.property_tables.size() > limits.maximum_property_tables) {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_reconstruction_unsafe,
                "Batch Table property table count exceeds metadata limits");
    }
    for (const auto& table : result.property_tables) {
        validatePropertyTable(table, limits);
    }
    if (result.hierarchy.has_value()) {
        static_cast<void>(compactPropertyHierarchy(
                std::vector<PropertyTable>(result.property_tables.begin() + 1,
                                           result.property_tables.end()),
                [&result]() {
                    PropertyHierarchy hierarchy = *result.hierarchy;
                    for (auto& node : hierarchy.nodes) --node.property_table;
                    return hierarchy;
                }(),
                {}, limits));
    }
    return result;
}

FeatureMetadata compactLegacyFeatureMetadata(
        const FeatureMetadata& source,
        const std::vector<std::uint32_t>& retained_source_feature_ids,
        const MetadataResourceLimits& limits) {
    if (source.primary_property_table != 0U
            || source.property_tables.empty()) {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_reconstruction_unsafe,
                "Legacy metadata primary table is invalid");
    }
    FeatureMetadata result;
    result.primary_property_table = 0U;
    result.property_tables.push_back(compactPropertyTable(
            source.property_tables.front(), retained_source_feature_ids,
            limits));
    if (!source.hierarchy.has_value()) return result;
    PropertyHierarchy hierarchy = *source.hierarchy;
    for (auto& node : hierarchy.nodes) {
        if (node.property_table == 0U) hierarchyInvalid(
                "Legacy hierarchy references the primary property table");
        --node.property_table;
    }
    const auto compacted = compactPropertyHierarchy(
            std::vector<PropertyTable>(source.property_tables.begin() + 1,
                                       source.property_tables.end()),
            hierarchy, retained_source_feature_ids, limits);
    result.property_tables.insert(result.property_tables.end(),
                                  compacted.property_tables.begin(),
                                  compacted.property_tables.end());
    result.hierarchy = compacted.hierarchy;
    for (auto& node : result.hierarchy->nodes) ++node.property_table;
    return result;
}

}  // namespace clip_worker::metadata
