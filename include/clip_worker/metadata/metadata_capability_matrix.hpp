#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace clip_worker::metadata {

inline constexpr const char* kMetadataCapabilityMatrixVersion =
        "THREE_D_TILES_METADATA_CAPABILITY_MATRIX_V1";
inline constexpr const char* kMetadataCapabilityMatrixSha256 =
        "3be3a6b7431fd1345ce09b27bacefc4c9095fb307a6eb5fa79496cc450d65090";

enum class MetadataSourceModel {
    no_feature_metadata,
    legacy_batch_table_json,
    legacy_batch_table_binary,
    legacy_batch_table_hierarchy,
    attribute_feature_id_property_table,
    implicit_feature_id_property_table,
    multiple_attribute_or_implicit_feature_id_sets,
    feature_id_texture,
    property_texture,
    property_attribute,
    int64_uint64_or_matrix_property,
    unknown_modern_hierarchy_or_relationship,
    root_statistics_or_custom_aggregate,
    unknown_required_metadata_extension,
    recognized_but_unsafe_to_reconstruct
};

enum class MetadataNormalizationSupport { supported, deferred };
enum class MetadataBoundarySupport { supported, global_preview_only };

struct MetadataCapabilityEntry {
    MetadataSourceModel source_model = MetadataSourceModel::no_feature_metadata;
    MetadataNormalizationSupport normalization =
            MetadataNormalizationSupport::deferred;
    MetadataBoundarySupport boundary_rebuild =
            MetadataBoundarySupport::global_preview_only;
    std::string reason_code;
};

class MetadataCapabilityMatrix final {
public:
    [[nodiscard]] static MetadataCapabilityMatrix load(
            const std::filesystem::path& document,
            const std::string& expected_sha256 =
                    kMetadataCapabilityMatrixSha256);

    [[nodiscard]] const MetadataCapabilityEntry& require(
            MetadataSourceModel model) const;
    [[nodiscard]] const std::vector<MetadataCapabilityEntry>& entries() const
            noexcept;

private:
    std::vector<MetadataCapabilityEntry> entries_;
};

}  // namespace clip_worker::metadata
