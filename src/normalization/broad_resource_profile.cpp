#include "clip_worker/normalization/broad_resource_profile.hpp"

#include "clip_worker/client/object_transfer.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

std::uint64_t positive(const Json& object, const char* name) {
    const auto item = object.find(name);
    if (item == object.end() || !item->is_number_unsigned()
        || item->get<std::uint64_t>() == 0U) {
        throw std::invalid_argument(
                std::string("Broad resource profile field is invalid: ") + name);
    }
    return item->get<std::uint64_t>();
}

}  // namespace

BroadResourceProfile BroadResourceProfile::load(
        const std::filesystem::path& document,
        const std::string& profile_id,
        const std::string& expected_document_sha256) {
    std::ifstream input(document, std::ios::binary);
    if (!input) throw std::invalid_argument("Broad resource profile is unreadable");
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    const std::string actual_sha = client::sha256Hex(bytes);
    if (actual_sha != expected_document_sha256) {
        throw std::invalid_argument("Broad resource profile SHA-256 mismatch");
    }
    const Json root = Json::parse(bytes.begin(), bytes.end());
    const std::uint32_t schema_version = root.value("schemaVersion", 0U);
    if ((schema_version != 3U && schema_version != 4U)
        || !root.contains("profiles") || !root.at("profiles").is_array()) {
        throw std::invalid_argument("Broad resource profile schema is invalid");
    }
    const auto profile = std::find_if(
            root.at("profiles").begin(), root.at("profiles").end(),
            [&profile_id](const Json& value) {
                return value.is_object() && value.value("id", std::string())
                        == profile_id;
            });
    if (profile == root.at("profiles").end()) {
        throw std::invalid_argument("Broad resource profile ID is absent");
    }
    BroadResourceProfile result;
    result.profile_id = profile_id;
    result.document_sha256 = actual_sha;
    const auto& point = profile->at("point");
    result.point.maximum_points = positive(point, "maximumPoints");
    result.point.maximum_decoded_bytes = positive(point, "maximumDecodedBytes");
    const auto& instance = profile->at("instance");
    result.instance.maximum_instances = positive(instance, "maximumInstances");
    result.instance.maximum_decoded_bytes = positive(instance, "maximumDecodedBytes");
    result.expansion.maximum_expanded_vertices =
            positive(instance, "maximumExpandedVertices");
    result.expansion.maximum_expanded_indices =
            positive(instance, "maximumExpandedIndices");
    result.expansion.maximum_boundary_instances =
            positive(instance, "maximumBoundaryInstances");
    const auto& composite = profile->at("composite");
    result.composite.maximum_depth = static_cast<std::uint32_t>(
            positive(composite, "maximumDepth"));
    result.composite.maximum_children = positive(composite, "maximumChildren");
    result.maximum_outputs = positive(composite, "maximumOutputs");
    const auto& metadata = profile->at("metadata");
    metadata::LegacyPropertyLimits metadata_limits;
    metadata_limits.maximum_feature_rows = positive(metadata, "maximumFeatureRows");
    metadata_limits.maximum_properties = positive(metadata, "maximumProperties");
    metadata_limits.maximum_string_bytes = positive(metadata, "maximumStringBytes");
    metadata_limits.maximum_binary_bytes = positive(metadata, "maximumBinaryBytes");
    result.point.metadata = metadata_limits;
    result.instance.metadata = metadata_limits;
    result.metadata.maximum_feature_rows = metadata_limits.maximum_feature_rows;
    result.metadata.maximum_properties = metadata_limits.maximum_properties;
    result.metadata.maximum_values_bytes = metadata_limits.maximum_binary_bytes;
    result.metadata.maximum_decoded_string_bytes =
            metadata_limits.maximum_string_bytes;
    if (schema_version == 4U) {
        result.point.enable_feature_metadata = true;
        result.instance.enable_feature_metadata = true;
        result.metadata.maximum_schema_bytes = positive(metadata, "maximumSchemaBytes");
        result.metadata.maximum_classes = positive(metadata, "maximumClasses");
        result.metadata.maximum_enums = positive(metadata, "maximumEnums");
        result.metadata.maximum_property_tables =
                positive(metadata, "maximumPropertyTables");
        result.metadata.maximum_feature_id_sets =
                positive(metadata, "maximumFeatureIdSets");
        result.metadata.maximum_values_bytes = positive(metadata, "maximumValuesBytes");
        result.metadata.maximum_array_offset_bytes =
                positive(metadata, "maximumArrayOffsetBytes");
        result.metadata.maximum_string_offset_bytes =
                positive(metadata, "maximumStringOffsetBytes");
        result.metadata.maximum_decoded_string_bytes =
                positive(metadata, "maximumDecodedStringBytes");
        result.metadata.maximum_array_elements =
                positive(metadata, "maximumArrayElements");
        result.metadata.maximum_hierarchy_instances =
                positive(metadata, "maximumHierarchyInstances");
        result.metadata.maximum_hierarchy_edges =
                positive(metadata, "maximumHierarchyEdges");
        result.metadata.maximum_hierarchy_depth =
                positive(metadata, "maximumHierarchyDepth");
        result.metadata.maximum_ancestor_closure_entries =
                positive(metadata, "maximumAncestorClosureEntries");
        result.metadata.maximum_feature_mapping_entries =
                positive(metadata, "maximumFeatureMappingEntries");
        result.metadata.maximum_validation_operations =
                positive(metadata, "maximumValidationOperations");
        result.point.feature_metadata = result.metadata;
        result.instance.feature_metadata = result.metadata;
    }
    return result;
}

}  // namespace clip_worker::normalization
