#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace clip_worker::formats {

enum class FormatErrorCode {
    unexpected_end,
    invalid_magic,
    unsupported_version,
    length_mismatch,
    invalid_alignment,
    invalid_chunk_order,
    invalid_json,
    invalid_feature_table,
    unsupported_chunk,
    unsupported_content,
    invalid_accessor,
    compression_draco_invalid,
    compression_draco_unsupported,
    compression_draco_limit_exceeded,
    compression_meshopt_invalid,
    compression_meshopt_unsupported,
    compression_meshopt_limit_exceeded,
    texture_invalid,
    texture_format_unsupported,
    texture_ktx2_unsupported,
    texture_dimension_limit_exceeded,
    texture_decoded_bytes_limit_exceeded,
    content_primitive_mode_unsupported,
    content_gltf_feature_unsupported,
    metadata_batch_table_unsupported,
    metadata_feature_id_invalid,
    metadata_feature_id_texture_unsupported,
    metadata_feature_id_triangle_ambiguous,
    metadata_property_table_invalid,
    metadata_property_type_unsupported,
    metadata_property_attribute_unsupported,
    metadata_property_texture_unsupported,
    metadata_hierarchy_invalid,
    metadata_hierarchy_unsupported,
    metadata_relationship_unsupported,
    metadata_statistics_unsupported,
    metadata_unknown_required_extension,
    metadata_reconstruction_unsafe,
    metadata_leakage_verification_failed,
    pnts_header_invalid,
    pnts_feature_table_invalid,
    pnts_semantic_unsupported,
    pnts_quantization_invalid,
    pnts_normal_invalid,
    point_output_invalid,
    i3dm_header_invalid,
    i3dm_feature_table_invalid,
    i3dm_semantic_unsupported,
    i3dm_enu_unsupported,
    i3dm_orientation_invalid,
    i3dm_scale_invalid,
    instance_output_invalid,
    instance_expansion_limit_exceeded,
    cmpt_header_invalid,
    cmpt_recursion_limit_exceeded,
    cmpt_child_unsupported,
    normalization_output_invalid,
    clipping_output_invalid
};

class FormatError final : public std::runtime_error {
public:
    FormatError(FormatErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {
    }

    [[nodiscard]] FormatErrorCode code() const noexcept {
        return code_;
    }

private:
    FormatErrorCode code_;
};

}  // namespace clip_worker::formats
