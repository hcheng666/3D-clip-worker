#pragma once

#include "clip_worker/metadata/metadata_resource_limits.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace clip_worker::metadata {

enum class PropertyScalarType {
    int8,
    uint8,
    int16,
    uint16,
    int32,
    uint32,
    float32,
    float64,
    boolean,
    string,
    enumeration
};

struct BooleanValues {
    std::vector<std::uint8_t> values;
};

struct StringValues {
    std::vector<std::string> values;
};

struct EnumValues {
    std::vector<std::int64_t> values;
};

using PropertyValues = std::variant<
        std::vector<std::int8_t>, std::vector<std::uint8_t>,
        std::vector<std::int16_t>, std::vector<std::uint16_t>,
        std::vector<std::int32_t>, std::vector<std::uint32_t>,
        std::vector<float>, std::vector<double>, BooleanValues, StringValues,
        EnumValues>;

/** Typed property column retaining its source scalar representation. */
struct PropertyColumn {
    std::string name;
    PropertyScalarType scalar_type = PropertyScalarType::float64;
    std::uint32_t components = 1U;
    std::optional<std::uint32_t> fixed_array_count;
    /** Variable-array offsets count logical values, not scalar components. */
    std::vector<std::uint32_t> array_offsets;
    PropertyValues values = std::vector<double>{};
    std::optional<PropertyValues> no_data;
    std::optional<PropertyValues> default_value;
    std::optional<PropertyValues> offset;
    std::optional<PropertyValues> scale;
    std::optional<PropertyValues> minimum;
    std::optional<PropertyValues> maximum;
    std::optional<PropertyScalarType> enum_value_type;
    std::optional<std::string> enum_type;
    bool normalized = false;
    bool required = false;
};

struct PropertyTable {
    std::string class_name;
    std::uint32_t row_count = 0U;
    std::vector<PropertyColumn> columns;
};

struct DenseFeatureMapping {
    static constexpr std::uint32_t kUnmapped =
            std::numeric_limits<std::uint32_t>::max();

    std::vector<std::uint32_t> retained_source_ids;
    std::vector<std::uint32_t> old_to_new;
};

struct HierarchyNode {
    std::uint32_t property_table = 0U;
    std::uint32_t property_row = 0U;
    std::vector<std::uint32_t> parents;
};

/** Legacy hierarchy expressed as a bounded DAG over typed class rows. */
struct PropertyHierarchy {
    std::vector<HierarchyNode> nodes;
    std::vector<std::uint32_t> feature_nodes;
};

struct EnumEntry {
    std::string name;
    std::int64_t value = 0;
};

struct EnumDefinition {
    std::string name;
    PropertyScalarType value_type = PropertyScalarType::uint16;
    std::vector<EnumEntry> values;
};

/** Shared metadata payload referenced by Mesh, Point, or Instance features. */
struct FeatureMetadata {
    std::vector<PropertyTable> property_tables;
    std::vector<EnumDefinition> enums;
    std::optional<std::uint32_t> primary_property_table;
    std::optional<PropertyHierarchy> hierarchy;
};

struct FeatureIdSet {
    std::string label;
    std::uint32_t feature_count = 0U;
    std::optional<std::uint32_t> property_table;
    std::optional<std::uint32_t> null_feature_id;
    bool implicit = false;
    std::vector<std::uint32_t> ids;
};

struct FeatureMetadataCompactionResult {
    FeatureMetadata metadata;
    std::vector<FeatureIdSet> feature_id_sets;
};

struct HierarchyCompactionResult {
    std::vector<PropertyTable> property_tables;
    PropertyHierarchy hierarchy;
    std::vector<std::uint32_t> retained_source_feature_ids;
    std::vector<std::uint32_t> old_to_new_nodes;
};

enum class FeatureIdComponentType {
    unsigned_byte,
    unsigned_short,
    float32
};

struct FeatureIdEncoding {
    FeatureIdComponentType component_type =
            FeatureIdComponentType::unsigned_byte;
    std::optional<std::uint32_t> null_feature_id;
};

[[nodiscard]] DenseFeatureMapping buildDenseFeatureMapping(
        std::uint32_t source_row_count,
        const std::vector<std::uint32_t>& retained_source_ids,
        std::optional<std::uint32_t> source_null_feature_id,
        const MetadataResourceLimits& limits);

[[nodiscard]] std::vector<std::uint32_t> rewriteFeatureIds(
        const std::vector<std::uint32_t>& source_ids,
        const DenseFeatureMapping& mapping,
        std::optional<std::uint32_t> source_null_feature_id,
        std::optional<std::uint32_t> target_null_feature_id);

[[nodiscard]] PropertyTable compactPropertyTable(
        const PropertyTable& source,
        const std::vector<std::uint32_t>& retained_source_rows,
        const MetadataResourceLimits& limits);

[[nodiscard]] HierarchyCompactionResult compactPropertyHierarchy(
        const std::vector<PropertyTable>& source_tables,
        const PropertyHierarchy& source_hierarchy,
        const std::vector<std::uint32_t>& retained_source_feature_ids,
        const MetadataResourceLimits& limits);

/** Compacts all referenced modern property tables and rewrites every ID set. */
[[nodiscard]] FeatureMetadataCompactionResult compactFeatureMetadata(
        const FeatureMetadata& source,
        const std::vector<FeatureIdSet>& retained_feature_id_sets,
        const MetadataResourceLimits& limits);

[[nodiscard]] FeatureIdEncoding selectFeatureIdEncoding(
        std::uint32_t feature_count, bool requires_null_feature_id);

void validatePropertyTable(
        const PropertyTable& table,
        const MetadataResourceLimits& limits);

}  // namespace clip_worker::metadata
