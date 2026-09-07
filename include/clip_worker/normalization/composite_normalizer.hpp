#pragma once

#include "clip_worker/formats/cmpt.hpp"
#include "clip_worker/formats/gltf_mesh_reader.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/instance_normalizer.hpp"
#include "clip_worker/normalization/mesh_resource_profile.hpp"
#include "clip_worker/normalization/point_normalizer.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::normalization {

enum class CompositeLeafStatus { success, unsupported, failure };

struct CompositeLeafResult {
    std::vector<std::uint32_t> ordinal_path;
    std::uint32_t depth = 0U;
    std::string child_id;
    std::string source_sha256;
    std::string source_kind;
    std::string source_version;
    std::uint64_t source_size = 0U;
    std::optional<CanonicalFamily> canonical_family;
    CompositeLeafStatus status = CompositeLeafStatus::failure;
    std::string reason_code;
    std::vector<std::uint8_t> canonical_bytes;
    std::optional<CanonicalArtifactEvidence> evidence;
    std::optional<std::uint64_t> point_count;
    std::optional<std::uint64_t> instance_count;
};

struct CompositeNormalizationInput {
    std::string root_package_relative_path;
    std::vector<std::uint8_t> source_bytes;
    formats::ApprovedGltfResourceMap approved_resources;
};

struct CompositeNormalizationResult {
    std::vector<CompositeLeafResult> leaves;
    std::vector<std::uint8_t> parent_manifest;
    std::string parent_manifest_sha256;
    bool global_preview_only = false;
};

/** Depth-first independent child normalization with fail-closed parent status. */
class CompositeNormalizer final {
public:
    [[nodiscard]] static CompositeNormalizationResult normalize(
            const CompositeNormalizationInput& input,
            const MeshResourceProfile& mesh_profile,
            const PointResourceLimits& point_limits,
            const InstanceResourceLimits& instance_limits,
            const ToolVersion& mesh_validator,
            const ToolVersion& point_validator,
            const ToolVersion& instance_validator,
            const formats::CmptLimits& composite_limits = {});
};

[[nodiscard]] std::string compositeLeafStatusName(CompositeLeafStatus status);

}  // namespace clip_worker::normalization
