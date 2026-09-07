#include "clip_worker/metadata/feature_metadata.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <type_traits>

namespace clip_worker::metadata {
namespace {

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_property_table_invalid,
            message);
}

[[noreturn]] void hierarchyInvalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_hierarchy_invalid, message);
}

[[noreturn]] void limitExceeded(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::metadata_reconstruction_unsafe, message);
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

bool valuesMatchType(const PropertyValues& values, PropertyScalarType type) {
    switch (type) {
        case PropertyScalarType::int8:
            return std::holds_alternative<std::vector<std::int8_t>>(values);
        case PropertyScalarType::uint8:
            return std::holds_alternative<std::vector<std::uint8_t>>(values);
        case PropertyScalarType::int16:
            return std::holds_alternative<std::vector<std::int16_t>>(values);
        case PropertyScalarType::uint16:
            return std::holds_alternative<std::vector<std::uint16_t>>(values);
        case PropertyScalarType::int32:
            return std::holds_alternative<std::vector<std::int32_t>>(values);
        case PropertyScalarType::uint32:
            return std::holds_alternative<std::vector<std::uint32_t>>(values);
        case PropertyScalarType::float32:
            return std::holds_alternative<std::vector<float>>(values);
        case PropertyScalarType::float64:
            return std::holds_alternative<std::vector<double>>(values);
        case PropertyScalarType::boolean:
            return std::holds_alternative<BooleanValues>(values);
        case PropertyScalarType::string:
            return std::holds_alternative<StringValues>(values);
        case PropertyScalarType::enumeration:
            return std::holds_alternative<EnumValues>(values);
    }
    return false;
}

std::uint64_t propertyBytes(const PropertyValues& values) {
    return std::visit(
            [](const auto& typed) -> std::uint64_t {
                using Value = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Value, StringValues>) {
                    return std::accumulate(
                            typed.values.begin(), typed.values.end(), 0ULL,
                            [](std::uint64_t total, const std::string& value) {
                                if (total > std::numeric_limits<std::uint64_t>::max()
                                                    - value.size()) {
                                    limitExceeded(
                                            "Metadata string byte count overflows");
                                }
                                return total + value.size();
                            });
                } else if constexpr (std::is_same_v<Value, BooleanValues>) {
                    return typed.values.size();
                } else if constexpr (std::is_same_v<Value, EnumValues>) {
                    if (typed.values.size()
                            > std::numeric_limits<std::uint64_t>::max()
                                    / sizeof(std::int64_t)) {
                        limitExceeded("Metadata enum byte count overflows");
                    }
                    return typed.values.size() * sizeof(std::int64_t);
                } else {
                    if (typed.size()
                            > std::numeric_limits<std::uint64_t>::max()
                                    / sizeof(typename Value::value_type)) {
                        limitExceeded("Metadata value byte count overflows");
                    }
                    return typed.size() * sizeof(typename Value::value_type);
                }
            },
            values);
}

std::pair<std::uint32_t, std::uint32_t> rowElementRange(
        const PropertyColumn& column, std::uint32_t row) {
    if (!column.array_offsets.empty()) {
        return {column.array_offsets.at(row), column.array_offsets.at(row + 1U)};
    }
    const std::uint32_t count = column.fixed_array_count.value_or(1U);
    return {row * count, (row + 1U) * count};
}

void appendScalarRange(PropertyValues& target, const PropertyValues& source,
                       std::size_t begin, std::size_t end) {
    if (target.index() != source.index() || begin > end
            || end > valueCount(source)) {
        invalid("Metadata property value range is invalid");
    }
    std::visit(
            [begin, end](auto& output, const auto& input) {
                using Output = std::decay_t<decltype(output)>;
                using Input = std::decay_t<decltype(input)>;
                if constexpr (std::is_same_v<Output, Input>) {
                    if constexpr (std::is_same_v<Output, BooleanValues>
                                  || std::is_same_v<Output, StringValues>
                                  || std::is_same_v<Output, EnumValues>) {
                        output.values.insert(
                                output.values.end(),
                                input.values.begin()
                                        + static_cast<std::ptrdiff_t>(begin),
                                input.values.begin()
                                        + static_cast<std::ptrdiff_t>(end));
                    } else {
                        output.insert(
                                output.end(),
                                input.begin() + static_cast<std::ptrdiff_t>(begin),
                                input.begin() + static_cast<std::ptrdiff_t>(end));
                    }
                }
            },
            target, source);
}

PropertyValues emptyValuesLike(const PropertyValues& source) {
    return std::visit(
            [](const auto& value) -> PropertyValues {
                using Value = std::decay_t<decltype(value)>;
                return Value{};
            },
            source);
}

void recomputeRetainedBounds(PropertyColumn& column) {
    if (!column.minimum.has_value() && !column.maximum.has_value()) return;
    std::visit(
            [&column](const auto& values) {
                using Value = std::decay_t<decltype(values)>;
                if constexpr (std::is_same_v<Value, BooleanValues>
                              || std::is_same_v<Value, StringValues>
                              || std::is_same_v<Value, EnumValues>) {
                    column.minimum.reset();
                    column.maximum.reset();
                } else {
                    using Scalar = typename Value::value_type;
                    const Value* no_data = nullptr;
                    if (column.no_data.has_value()
                            && std::holds_alternative<Value>(
                                    *column.no_data)) {
                        no_data = &std::get<Value>(*column.no_data);
                    }
                    if (no_data != nullptr
                            && no_data->size() != column.components) {
                        invalid("Metadata noData component count is invalid");
                    }
                    Value minimum(column.components);
                    Value maximum(column.components);
                    bool found = false;
                    for (std::size_t offset = 0U; offset < values.size();
                         offset += column.components) {
                        bool is_no_data = no_data != nullptr;
                        if (no_data != nullptr) {
                            for (std::size_t component = 0U;
                                 component < column.components; ++component) {
                                if (values[offset + component]
                                        != no_data->at(component)) {
                                    is_no_data = false;
                                    break;
                                }
                            }
                        }
                        if (is_no_data) continue;
                        if (!found) {
                            for (std::size_t component = 0U;
                                 component < column.components; ++component) {
                                minimum[component] = values[offset + component];
                                maximum[component] = values[offset + component];
                            }
                            found = true;
                            continue;
                        }
                        for (std::size_t component = 0U;
                             component < column.components; ++component) {
                            const Scalar value = values[offset + component];
                            minimum[component] = std::min(
                                    minimum[component], value);
                            maximum[component] = std::max(
                                    maximum[component], value);
                        }
                    }
                    if (!found) {
                        column.minimum.reset();
                        column.maximum.reset();
                    } else {
                        if (column.minimum.has_value()) column.minimum = minimum;
                        if (column.maximum.has_value()) column.maximum = maximum;
                    }
                }
            },
            column.values);
}

void validateHierarchy(const std::vector<PropertyTable>& tables,
                       const PropertyHierarchy& hierarchy,
                       const MetadataResourceLimits& limits) {
    if (hierarchy.nodes.size() > limits.maximum_hierarchy_instances
            || hierarchy.feature_nodes.size() > limits.maximum_feature_rows) {
        limitExceeded("Metadata hierarchy instance limit is exceeded");
    }
    std::uint64_t edge_count = 0U;
    for (const auto& node : hierarchy.nodes) {
        if (node.property_table >= tables.size()
                || node.property_row
                        >= tables.at(node.property_table).row_count) {
            hierarchyInvalid("Metadata hierarchy property row is invalid");
        }
        edge_count += node.parents.size();
        if (edge_count > limits.maximum_hierarchy_edges) {
            limitExceeded("Metadata hierarchy edge limit is exceeded");
        }
        for (const std::uint32_t parent : node.parents) {
            if (parent >= hierarchy.nodes.size()) {
                hierarchyInvalid("Metadata hierarchy parent is invalid");
            }
        }
    }
    std::set<std::uint32_t> feature_nodes;
    for (const std::uint32_t node : hierarchy.feature_nodes) {
        if (node >= hierarchy.nodes.size()) {
            hierarchyInvalid("Metadata hierarchy feature node is invalid");
        }
        if (!feature_nodes.insert(node).second) {
            hierarchyInvalid("Metadata hierarchy feature node is duplicated");
        }
    }
    std::vector<std::uint8_t> state(hierarchy.nodes.size(), 0U);
    std::function<void(std::uint32_t, std::uint64_t)> visit =
            [&](std::uint32_t node, std::uint64_t depth) {
                if (depth > limits.maximum_hierarchy_depth) {
                    limitExceeded("Metadata hierarchy depth limit is exceeded");
                }
                if (state.at(node) == 1U) {
                    hierarchyInvalid("Metadata hierarchy contains a cycle");
                }
                if (state.at(node) == 2U) return;
                state.at(node) = 1U;
                for (const std::uint32_t parent : hierarchy.nodes.at(node).parents) {
                    visit(parent, depth + 1U);
                }
                state.at(node) = 2U;
            };
    for (std::uint32_t node = 0U; node < hierarchy.nodes.size(); ++node) {
        visit(node, 1U);
    }
}

}  // namespace

void validatePropertyTable(
        const PropertyTable& table,
        const MetadataResourceLimits& limits) {
    if (table.class_name.empty() || table.row_count > limits.maximum_feature_rows
            || table.columns.size() > limits.maximum_properties) {
        limitExceeded("Metadata property table limit is exceeded");
    }
    std::set<std::string> names;
    std::uint64_t total_bytes = 0U;
    std::uint64_t total_elements = 0U;
    for (const auto& column : table.columns) {
        if (column.name.empty() || !names.insert(column.name).second
                || column.components == 0U || column.components > 4U
                || (column.scalar_type == PropertyScalarType::string
                    && column.components != 1U)
                || !valuesMatchType(column.values, column.scalar_type)
                || (column.no_data.has_value()
                    && !valuesMatchType(*column.no_data, column.scalar_type))
                || (column.default_value.has_value()
                    && !valuesMatchType(*column.default_value,
                                        column.scalar_type))
                || (column.offset.has_value()
                    && !valuesMatchType(*column.offset, column.scalar_type))
                || (column.scale.has_value()
                    && !valuesMatchType(*column.scale, column.scalar_type))
                || (column.minimum.has_value()
                    && !valuesMatchType(*column.minimum, column.scalar_type))
                || (column.maximum.has_value()
                    && !valuesMatchType(*column.maximum, column.scalar_type))) {
            invalid("Metadata property definition is invalid");
        }
        if (column.fixed_array_count.has_value()
                && (!column.array_offsets.empty()
                    || *column.fixed_array_count == 0U)) {
            invalid("Metadata array definition is ambiguous");
        }
        const auto hasCount = [&column](
                                      const std::optional<PropertyValues>& value,
                                      std::uint64_t expected) {
            return !value.has_value() || valueCount(*value) == expected;
        };
        const std::uint64_t element_components = column.components;
        const std::uint64_t default_components =
                column.fixed_array_count.has_value()
                ? element_components * *column.fixed_array_count
                : element_components;
        if (!hasCount(column.no_data, element_components)
                || !hasCount(column.offset, element_components)
                || !hasCount(column.scale, element_components)
                || !hasCount(column.minimum, element_components)
                || !hasCount(column.maximum, element_components)
                || (!column.array_offsets.empty()
                    ? (column.default_value.has_value()
                       && valueCount(*column.default_value)
                                % element_components != 0U)
                    : !hasCount(column.default_value, default_components))) {
            invalid("Metadata property literal component count is invalid");
        }
        if (column.normalized
                && (column.scalar_type == PropertyScalarType::float32
                    || column.scalar_type == PropertyScalarType::float64
                    || column.scalar_type == PropertyScalarType::boolean
                    || column.scalar_type == PropertyScalarType::string
                    || column.scalar_type == PropertyScalarType::enumeration)) {
            invalid("Metadata normalized flag is invalid for its type");
        }
        std::uint64_t element_count = table.row_count;
        if (!column.array_offsets.empty()) {
            if (column.array_offsets.size()
                            != static_cast<std::size_t>(table.row_count) + 1U
                    || column.array_offsets.front() != 0U
                    || !std::is_sorted(column.array_offsets.begin(),
                                       column.array_offsets.end())) {
                invalid("Metadata array offsets are invalid");
            }
            element_count = column.array_offsets.back();
        } else if (column.fixed_array_count.has_value()) {
            element_count *= *column.fixed_array_count;
        }
        if (element_count
                    > std::numeric_limits<std::uint64_t>::max()
                            / column.components
                || valueCount(column.values)
                        != element_count * column.components) {
            invalid("Metadata property value count is invalid");
        }
        if (total_elements > std::numeric_limits<std::uint64_t>::max()
                                     - element_count) {
            limitExceeded("Metadata element count overflows");
        }
        total_elements += element_count;
        const std::uint64_t column_bytes = propertyBytes(column.values);
        if (total_bytes > std::numeric_limits<std::uint64_t>::max()
                                  - column_bytes) {
            limitExceeded("Metadata value byte count overflows");
        }
        total_bytes += column_bytes;
        if (!column.array_offsets.empty()
                && column.array_offsets.size()
                        > limits.maximum_array_offset_bytes
                                / sizeof(std::uint32_t)) {
            limitExceeded("Metadata array offset byte limit is exceeded");
        }
        if (column.scalar_type == PropertyScalarType::string
                && column_bytes > limits.maximum_decoded_string_bytes) {
            limitExceeded("Metadata decoded string byte limit is exceeded");
        }
        if (total_elements > limits.maximum_array_elements
                || total_bytes > limits.maximum_values_bytes) {
            limitExceeded("Metadata property allocation limit is exceeded");
        }
    }
}

DenseFeatureMapping buildDenseFeatureMapping(
        std::uint32_t source_row_count,
        const std::vector<std::uint32_t>& retained_source_ids,
        std::optional<std::uint32_t> source_null_feature_id,
        const MetadataResourceLimits& limits) {
    if (source_row_count > limits.maximum_feature_rows
            || retained_source_ids.size()
                    > limits.maximum_feature_mapping_entries) {
        limitExceeded("Metadata feature mapping limit is exceeded");
    }
    DenseFeatureMapping result;
    result.old_to_new.assign(source_row_count, DenseFeatureMapping::kUnmapped);
    result.retained_source_ids.reserve(retained_source_ids.size());
    for (const std::uint32_t source_id : retained_source_ids) {
        if (source_null_feature_id.has_value()
                && source_id == *source_null_feature_id) {
            continue;
        }
        if (source_id >= source_row_count) {
            throw formats::FormatError(
                    formats::FormatErrorCode::metadata_feature_id_invalid,
                    "Metadata feature ID exceeds its property table");
        }
        result.retained_source_ids.push_back(source_id);
    }
    std::sort(result.retained_source_ids.begin(),
              result.retained_source_ids.end());
    result.retained_source_ids.erase(
            std::unique(result.retained_source_ids.begin(),
                        result.retained_source_ids.end()),
            result.retained_source_ids.end());
    for (std::uint32_t target_id = 0U;
         target_id < result.retained_source_ids.size(); ++target_id) {
        result.old_to_new.at(result.retained_source_ids.at(target_id)) = target_id;
    }
    return result;
}

std::vector<std::uint32_t> rewriteFeatureIds(
        const std::vector<std::uint32_t>& source_ids,
        const DenseFeatureMapping& mapping,
        std::optional<std::uint32_t> source_null_feature_id,
        std::optional<std::uint32_t> target_null_feature_id) {
    std::vector<std::uint32_t> result;
    result.reserve(source_ids.size());
    for (const std::uint32_t source_id : source_ids) {
        if (source_null_feature_id.has_value()
                && source_id == *source_null_feature_id) {
            if (!target_null_feature_id.has_value()) {
                throw formats::FormatError(
                        formats::FormatErrorCode::metadata_feature_id_invalid,
                        "Metadata null feature ID has no canonical sentinel");
            }
            result.push_back(*target_null_feature_id);
        } else if (source_id >= mapping.old_to_new.size()
                   || mapping.old_to_new.at(source_id)
                           == DenseFeatureMapping::kUnmapped) {
            throw formats::FormatError(
                    formats::FormatErrorCode::metadata_feature_id_invalid,
                    "Metadata feature ID was not retained");
        } else {
            result.push_back(mapping.old_to_new.at(source_id));
        }
    }
    return result;
}

PropertyTable compactPropertyTable(
        const PropertyTable& source,
        const std::vector<std::uint32_t>& retained_source_rows,
        const MetadataResourceLimits& limits) {
    validatePropertyTable(source, limits);
    if (retained_source_rows.size() > limits.maximum_feature_rows) {
        limitExceeded("Metadata retained row limit is exceeded");
    }
    PropertyTable result;
    result.class_name = source.class_name;
    result.row_count = static_cast<std::uint32_t>(retained_source_rows.size());
    result.columns.reserve(source.columns.size());
    for (const auto& source_column : source.columns) {
        PropertyColumn column = source_column;
        column.values = emptyValuesLike(source_column.values);
        if (!source_column.array_offsets.empty()) {
            column.array_offsets.assign(1U, 0U);
        }
        for (const std::uint32_t source_row : retained_source_rows) {
            if (source_row >= source.row_count) {
                invalid("Metadata retained row exceeds its property table");
            }
            const auto range = rowElementRange(source_column, source_row);
            const std::size_t scalar_begin =
                    static_cast<std::size_t>(range.first)
                    * source_column.components;
            const std::size_t scalar_end =
                    static_cast<std::size_t>(range.second)
                    * source_column.components;
            appendScalarRange(column.values, source_column.values,
                              scalar_begin, scalar_end);
            if (!source_column.array_offsets.empty()) {
                const std::uint64_t next =
                        static_cast<std::uint64_t>(column.array_offsets.back())
                        + range.second - range.first;
                if (next > std::numeric_limits<std::uint32_t>::max()) {
                    limitExceeded("Metadata compacted array offsets overflow");
                }
                column.array_offsets.push_back(
                        static_cast<std::uint32_t>(next));
            }
        }
        recomputeRetainedBounds(column);
        result.columns.push_back(std::move(column));
    }
    validatePropertyTable(result, limits);
    return result;
}

HierarchyCompactionResult compactPropertyHierarchy(
        const std::vector<PropertyTable>& source_tables,
        const PropertyHierarchy& source_hierarchy,
        const std::vector<std::uint32_t>& retained_source_feature_ids,
        const MetadataResourceLimits& limits) {
    if (source_tables.size() > limits.maximum_property_tables) {
        limitExceeded("Metadata property table count limit is exceeded");
    }
    for (const auto& table : source_tables) validatePropertyTable(table, limits);
    validateHierarchy(source_tables, source_hierarchy, limits);
    std::set<std::uint32_t> closure;
    std::function<void(std::uint32_t)> retainNode = [&](std::uint32_t node) {
        if (!closure.insert(node).second) return;
        if (closure.size() > limits.maximum_ancestor_closure_entries) {
            limitExceeded("Metadata hierarchy ancestor closure limit is exceeded");
        }
        for (const std::uint32_t parent : source_hierarchy.nodes.at(node).parents) {
            retainNode(parent);
        }
    };
    HierarchyCompactionResult result;
    result.retained_source_feature_ids = retained_source_feature_ids;
    std::sort(result.retained_source_feature_ids.begin(),
              result.retained_source_feature_ids.end());
    result.retained_source_feature_ids.erase(
            std::unique(result.retained_source_feature_ids.begin(),
                        result.retained_source_feature_ids.end()),
            result.retained_source_feature_ids.end());
    for (const std::uint32_t feature : result.retained_source_feature_ids) {
        if (feature >= source_hierarchy.feature_nodes.size()) {
            hierarchyInvalid("Metadata hierarchy feature ID is invalid");
        }
        retainNode(source_hierarchy.feature_nodes.at(feature));
    }
    result.old_to_new_nodes.assign(source_hierarchy.nodes.size(),
                                   DenseFeatureMapping::kUnmapped);
    // Physical features are emitted in dense feature-ID order. Remaining
    // ancestors use a deterministic parent-first traversal.
    std::vector<std::uint32_t> retained_nodes;
    retained_nodes.reserve(closure.size());
    std::set<std::uint32_t> appended;
    for (const std::uint32_t feature : result.retained_source_feature_ids) {
        const std::uint32_t node = source_hierarchy.feature_nodes.at(feature);
        if (!appended.insert(node).second) {
            hierarchyInvalid("Retained hierarchy features share one node");
        }
        retained_nodes.push_back(node);
    }
    std::function<void(std::uint32_t)> appendAncestor =
            [&](std::uint32_t node) {
                if (appended.count(node) > 0U) return;
                for (const std::uint32_t parent
                        : source_hierarchy.nodes.at(node).parents) {
                    if (closure.count(parent) > 0U) appendAncestor(parent);
                }
                appended.insert(node);
                retained_nodes.push_back(node);
            };
    for (const std::uint32_t node : closure) appendAncestor(node);
    for (std::uint32_t target = 0U; target < retained_nodes.size(); ++target) {
        result.old_to_new_nodes.at(retained_nodes.at(target)) = target;
    }
    std::vector<std::vector<std::uint32_t>> retained_rows(source_tables.size());
    for (const std::uint32_t node : retained_nodes) {
        const auto& source_node = source_hierarchy.nodes.at(node);
        retained_rows.at(source_node.property_table).push_back(
                source_node.property_row);
    }
    std::vector<std::vector<std::uint32_t>> row_mappings(source_tables.size());
    std::vector<std::uint32_t> table_mappings(
            source_tables.size(), DenseFeatureMapping::kUnmapped);
    result.property_tables.reserve(source_tables.size());
    for (std::size_t table = 0U; table < source_tables.size(); ++table) {
        auto& rows = retained_rows.at(table);
        // Preserve the node emission contract: retained physical features come
        // first, followed by their deterministic ancestor closure. Sorting by
        // source row here would silently restore ancestor-first property rows.
        std::set<std::uint32_t> seen_rows;
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const std::uint32_t row) {
                                      return !seen_rows.insert(row).second;
                                  }),
                   rows.end());
        row_mappings.at(table).assign(source_tables.at(table).row_count,
                                      DenseFeatureMapping::kUnmapped);
        for (std::uint32_t target = 0U; target < rows.size(); ++target) {
            row_mappings.at(table).at(rows.at(target)) = target;
        }
        if (rows.empty()) continue;
        table_mappings.at(table) =
                static_cast<std::uint32_t>(result.property_tables.size());
        result.property_tables.push_back(
                compactPropertyTable(source_tables.at(table), rows, limits));
    }
    result.hierarchy.nodes.reserve(retained_nodes.size());
    for (const std::uint32_t node : retained_nodes) {
        const auto& source_node = source_hierarchy.nodes.at(node);
        HierarchyNode compacted;
        compacted.property_table = table_mappings.at(source_node.property_table);
        if (compacted.property_table == DenseFeatureMapping::kUnmapped) {
            hierarchyInvalid("Retained hierarchy node has no property table");
        }
        compacted.property_row = row_mappings.at(source_node.property_table)
                                         .at(source_node.property_row);
        compacted.parents.reserve(source_node.parents.size());
        for (const std::uint32_t parent : source_node.parents) {
            compacted.parents.push_back(result.old_to_new_nodes.at(parent));
        }
        std::sort(compacted.parents.begin(), compacted.parents.end());
        result.hierarchy.nodes.push_back(std::move(compacted));
    }
    result.hierarchy.feature_nodes.reserve(
            result.retained_source_feature_ids.size());
    for (const std::uint32_t feature : result.retained_source_feature_ids) {
        result.hierarchy.feature_nodes.push_back(result.old_to_new_nodes.at(
                source_hierarchy.feature_nodes.at(feature)));
    }
    validateHierarchy(result.property_tables, result.hierarchy, limits);
    return result;
}

FeatureIdEncoding selectFeatureIdEncoding(
        std::uint32_t feature_count, bool requires_null_feature_id) {
    constexpr std::uint32_t kUnsignedByteMaximum = 255U;
    constexpr std::uint32_t kUnsignedShortMaximum = 65535U;
    constexpr std::uint32_t kMaximumExactFloatInteger = 16777216U;
    const std::uint64_t maximum_id = feature_count == 0U
            ? 0U
            : static_cast<std::uint64_t>(feature_count)
                    - (requires_null_feature_id ? 0U : 1U);
    FeatureIdEncoding result;
    if (maximum_id <= kUnsignedByteMaximum) {
        result.component_type = FeatureIdComponentType::unsigned_byte;
    } else if (maximum_id <= kUnsignedShortMaximum) {
        result.component_type = FeatureIdComponentType::unsigned_short;
    } else if (maximum_id <= kMaximumExactFloatInteger) {
        result.component_type = FeatureIdComponentType::float32;
    } else {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_reconstruction_unsafe,
                "Metadata feature IDs exceed exact glTF attribute capacity");
    }
    if (requires_null_feature_id) result.null_feature_id = feature_count;
    return result;
}

FeatureMetadataCompactionResult compactFeatureMetadata(
        const FeatureMetadata& source,
        const std::vector<FeatureIdSet>& retained_feature_id_sets,
        const MetadataResourceLimits& limits) {
    if (source.hierarchy.has_value()) {
        throw formats::FormatError(
                formats::FormatErrorCode::metadata_relationship_unsupported,
                "Modern feature-set compaction cannot infer hierarchy semantics");
    }
    if (retained_feature_id_sets.size() > limits.maximum_feature_id_sets) {
        limitExceeded("Metadata feature ID set limit is exceeded");
    }
    std::map<std::uint32_t, std::vector<std::uint32_t>> retained_by_table;
    for (const auto& set : retained_feature_id_sets) {
        if (set.feature_count == 0U || set.ids.empty()) {
            throw formats::FormatError(
                    formats::FormatErrorCode::metadata_feature_id_invalid,
                    "Metadata feature ID set is empty");
        }
        if (set.property_table.has_value()) {
            if (*set.property_table >= source.property_tables.size()
                    || set.feature_count
                            > source.property_tables.at(*set.property_table)
                                      .row_count) {
                throw formats::FormatError(
                        formats::FormatErrorCode::metadata_feature_id_invalid,
                        "Metadata feature ID set property table is invalid");
            }
            auto& retained = retained_by_table[*set.property_table];
            retained.insert(retained.end(), set.ids.begin(), set.ids.end());
        }
    }
    FeatureMetadataCompactionResult result;
    std::map<std::uint32_t, std::uint32_t> table_mapping;
    std::map<std::uint32_t, DenseFeatureMapping> feature_mappings;
    for (auto& entry : retained_by_table) {
        const std::uint32_t source_table = entry.first;
        const auto source_count =
                source.property_tables.at(source_table).row_count;
        std::optional<std::uint32_t> null_id;
        for (const auto& set : retained_feature_id_sets) {
            if (set.property_table == source_table
                    && set.null_feature_id.has_value()) {
                if (null_id.has_value() && null_id != set.null_feature_id) {
                    throw formats::FormatError(
                            formats::FormatErrorCode::metadata_reconstruction_unsafe,
                            "Shared metadata table uses inconsistent null IDs");
                }
                null_id = set.null_feature_id;
            }
        }
        DenseFeatureMapping mapping = buildDenseFeatureMapping(
                source_count, entry.second, null_id, limits);
        table_mapping[source_table] =
                static_cast<std::uint32_t>(result.metadata.property_tables.size());
        result.metadata.property_tables.push_back(compactPropertyTable(
                source.property_tables.at(source_table),
                mapping.retained_source_ids, limits));
        feature_mappings.emplace(source_table, std::move(mapping));
    }
    result.feature_id_sets.reserve(retained_feature_id_sets.size());
    for (const auto& source_set : retained_feature_id_sets) {
        FeatureIdSet set = source_set;
        if (source_set.property_table.has_value()) {
            const auto& mapping = feature_mappings.at(*source_set.property_table);
            const std::uint32_t target_count =
                    static_cast<std::uint32_t>(mapping.retained_source_ids.size());
            set.ids = rewriteFeatureIds(
                    source_set.ids, mapping, source_set.null_feature_id,
                    source_set.null_feature_id.has_value()
                            ? std::optional<std::uint32_t>(target_count)
                            : std::nullopt);
            set.feature_count = target_count;
            set.null_feature_id = source_set.null_feature_id.has_value()
                    ? std::optional<std::uint32_t>(target_count)
                    : std::nullopt;
            set.property_table = table_mapping.at(*source_set.property_table);
        } else {
            const auto mapping = buildDenseFeatureMapping(
                    source_set.feature_count, source_set.ids,
                    source_set.null_feature_id, limits);
            const std::uint32_t target_count =
                    static_cast<std::uint32_t>(mapping.retained_source_ids.size());
            set.ids = rewriteFeatureIds(
                    source_set.ids, mapping, source_set.null_feature_id,
                    source_set.null_feature_id.has_value()
                            ? std::optional<std::uint32_t>(target_count)
                            : std::nullopt);
            set.feature_count = target_count;
            set.null_feature_id = source_set.null_feature_id.has_value()
                    ? std::optional<std::uint32_t>(target_count)
                    : std::nullopt;
        }
        result.feature_id_sets.push_back(std::move(set));
    }
    std::set<std::string> referenced_enums;
    for (const auto& table : result.metadata.property_tables) {
        for (const auto& column : table.columns) {
            if (column.enum_type.has_value()) {
                referenced_enums.insert(*column.enum_type);
            }
        }
    }
    for (const auto& definition : source.enums) {
        if (referenced_enums.count(definition.name) > 0U) {
            result.metadata.enums.push_back(definition);
        }
    }
    return result;
}

}  // namespace clip_worker::metadata
