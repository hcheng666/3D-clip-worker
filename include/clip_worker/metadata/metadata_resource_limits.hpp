#pragma once

#include <cstdint>

namespace clip_worker::metadata {

/** Bounded allocations and graph work permitted for metadata reconstruction. */
struct MetadataResourceLimits {
    std::uint64_t maximum_schema_bytes = 0U;
    std::uint64_t maximum_classes = 0U;
    std::uint64_t maximum_enums = 0U;
    std::uint64_t maximum_property_tables = 0U;
    std::uint64_t maximum_feature_id_sets = 0U;
    std::uint64_t maximum_feature_rows = 0U;
    std::uint64_t maximum_properties = 0U;
    std::uint64_t maximum_values_bytes = 0U;
    std::uint64_t maximum_array_offset_bytes = 0U;
    std::uint64_t maximum_string_offset_bytes = 0U;
    std::uint64_t maximum_decoded_string_bytes = 0U;
    std::uint64_t maximum_array_elements = 0U;
    std::uint64_t maximum_hierarchy_instances = 0U;
    std::uint64_t maximum_hierarchy_edges = 0U;
    std::uint64_t maximum_hierarchy_depth = 0U;
    std::uint64_t maximum_ancestor_closure_entries = 0U;
    std::uint64_t maximum_feature_mapping_entries = 0U;
    std::uint64_t maximum_validation_operations = 0U;
};

}  // namespace clip_worker::metadata
