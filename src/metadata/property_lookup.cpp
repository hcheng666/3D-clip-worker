#include "clip_worker/metadata/property_lookup.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace clip_worker::metadata {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_property_table_invalid,
            message);
}

[[noreturn]] void hierarchyInvalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_hierarchy_invalid, message);
}

std::size_t valueCount(const PropertyValues& values) {
    return std::visit(
            [](const auto& typed) -> std::size_t {
                using Value = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Value, BooleanValues>
                              || std::is_same_v<Value, StringValues>
                              || std::is_same_v<Value, EnumValues>) {
                    return typed.values.size();
                } else {
                    return typed.size();
                }
            },
            values);
}

const char* scalarTypeName(PropertyScalarType type) {
    switch (type) {
        case PropertyScalarType::int8: return "INT8";
        case PropertyScalarType::uint8: return "UINT8";
        case PropertyScalarType::int16: return "INT16";
        case PropertyScalarType::uint16: return "UINT16";
        case PropertyScalarType::int32: return "INT32";
        case PropertyScalarType::uint32: return "UINT32";
        case PropertyScalarType::float32: return "FLOAT32";
        case PropertyScalarType::float64: return "FLOAT64";
        case PropertyScalarType::boolean: return "BOOLEAN";
        case PropertyScalarType::string: return "STRING";
        case PropertyScalarType::enumeration: return "ENUM";
    }
    invalid("Metadata lookup scalar type is invalid");
}

Json valuesJson(const PropertyValues& values,
                std::size_t begin, std::size_t end) {
    if (begin > end || end > valueCount(values)) {
        invalid("Metadata lookup value range is invalid");
    }
    Json output = Json::array();
    std::visit(
            [&output, begin, end](const auto& typed) {
                using Value = std::decay_t<decltype(typed)>;
                const auto append = [&output, begin, end](const auto& source) {
                    for (std::size_t index = begin; index < end; ++index) {
                        output.push_back(source.at(index));
                    }
                };
                if constexpr (std::is_same_v<Value, BooleanValues>) {
                    for (std::size_t index = begin; index < end; ++index) {
                        output.push_back(typed.values.at(index) != 0U);
                    }
                } else if constexpr (std::is_same_v<Value, StringValues>
                                     || std::is_same_v<Value, EnumValues>) {
                    append(typed.values);
                } else {
                    append(typed);
                }
            },
            values);
    return output;
}

Json propertyJson(const PropertyColumn& column, std::uint32_t row) {
    std::uint64_t logical_begin = row;
    std::uint64_t logical_end = static_cast<std::uint64_t>(row) + 1U;
    if (!column.array_offsets.empty()) {
        if (static_cast<std::size_t>(row) + 1U >= column.array_offsets.size()) {
            invalid("Metadata lookup array row is invalid");
        }
        logical_begin = column.array_offsets.at(row);
        logical_end = column.array_offsets.at(row + 1U);
    } else if (column.fixed_array_count.has_value()) {
        logical_begin *= *column.fixed_array_count;
        logical_end *= *column.fixed_array_count;
    }
    const std::uint64_t scalar_begin = logical_begin * column.components;
    const std::uint64_t scalar_end = logical_end * column.components;
    if (scalar_end > static_cast<std::uint64_t>(valueCount(column.values))) {
        invalid("Metadata lookup property row is out of range");
    }
    Json result{{"components", column.components},
                {"scalarType", scalarTypeName(column.scalar_type)},
                {"values", valuesJson(
                        column.values,
                        static_cast<std::size_t>(scalar_begin),
                        static_cast<std::size_t>(scalar_end))}};
    if (column.fixed_array_count.has_value()) {
        result["fixedArrayCount"] = *column.fixed_array_count;
    } else if (!column.array_offsets.empty()) {
        result["variableArray"] = true;
    }
    if (column.enum_type.has_value()) result["enumType"] = *column.enum_type;
    return result;
}

void addRowProperties(const PropertyTable& table, std::uint32_t row,
                      std::map<std::string, Json>& properties) {
    if (row >= table.row_count) {
        invalid("Metadata lookup feature ID exceeds its property table");
    }
    for (const auto& column : table.columns) {
        if (!properties.emplace(column.name, propertyJson(column, row)).second) {
            hierarchyInvalid(
                    "Metadata hierarchy property overload is ambiguous");
        }
    }
}

}  // namespace

std::optional<std::string> lookupPropertyMaterial(
        const FeatureMetadata& metadata,
        std::optional<std::uint32_t> property_table,
        std::uint32_t feature_id,
        std::optional<std::uint32_t> null_feature_id) {
    if (!property_table.has_value()
            || (null_feature_id.has_value()
                && feature_id == *null_feature_id)
            || *property_table >= metadata.property_tables.size()
            || feature_id
                    >= metadata.property_tables.at(*property_table).row_count) {
        return std::nullopt;
    }

    std::map<std::string, Json> properties;
    addRowProperties(metadata.property_tables.at(*property_table), feature_id,
                     properties);
    if (metadata.hierarchy.has_value()) {
        const auto& hierarchy = *metadata.hierarchy;
        if (feature_id >= hierarchy.feature_nodes.size()) {
            return std::nullopt;
        }
        std::set<std::uint32_t> visited;
        std::set<std::uint32_t> active;
        std::function<void(std::uint32_t)> visit = [&](std::uint32_t node_id) {
            if (node_id >= hierarchy.nodes.size()) {
                hierarchyInvalid("Metadata hierarchy lookup node is invalid");
            }
            if (!active.insert(node_id).second) {
                hierarchyInvalid("Metadata hierarchy lookup contains a cycle");
            }
            if (visited.insert(node_id).second) {
                const auto& node = hierarchy.nodes.at(node_id);
                if (node.property_table >= metadata.property_tables.size()) {
                    hierarchyInvalid(
                            "Metadata hierarchy lookup table is invalid");
                }
                addRowProperties(
                        metadata.property_tables.at(node.property_table),
                        node.property_row, properties);
                std::vector<std::uint32_t> parents = node.parents;
                std::sort(parents.begin(), parents.end());
                for (const std::uint32_t parent : parents) visit(parent);
            }
            active.erase(node_id);
        };
        visit(hierarchy.feature_nodes.at(feature_id));
    }

    Json material = Json::object();
    for (auto& property : properties) {
        material[property.first] = std::move(property.second);
    }
    return material.dump();
}

}  // namespace clip_worker::metadata
