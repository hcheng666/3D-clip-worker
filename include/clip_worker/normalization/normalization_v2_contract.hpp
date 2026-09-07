#pragma once

#include "clip_worker/normalization/canonical_artifact.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::normalization::v2 {

inline constexpr const char* kProtocolVersion = "THREE_D_TILES_NORMALIZER_V2";
inline constexpr const char* kSchemaSha256 =
        "4926bba1c29b47eb148fe3abdfafd22c67adabbb645e102321d6affb8c8219a6";
inline constexpr const char* kManifestVersion =
        "NORMALIZATION_RESOURCE_MANIFEST_V2";
inline constexpr const char* kResourceProfileVersion =
        "STANDARD_4CPU_8GIB_SINGLE_TASK_V3";
inline constexpr const char* kMeshNormalizationVersion = "normalization-v2";
inline constexpr const char* kPointNormalizationVersion =
        "normalization-point-v1";
inline constexpr const char* kInstanceNormalizationVersion =
        "normalization-instance-v1";
inline constexpr const char* kCompositeNormalizationVersion =
        "normalization-composite-v1";
inline constexpr const char* kMeshCanonicalContractVersion =
        "canonical-gltf2-v1";
inline constexpr const char* kPointCanonicalContractVersion =
        "canonical-point-gltf2-v1";
inline constexpr const char* kInstanceCanonicalContractVersion =
        "canonical-instance-gltf2-v1";
inline constexpr const char* kCompositeCanonicalContractVersion =
        "canonical-composite-children-v1";
inline constexpr const char* kCompositeParentManifestVersion =
        "CANONICAL_COMPOSITE_CHILDREN_V1";

struct Limits {
    static constexpr std::size_t kMaximumOrdinalDepth = 8U;
    static constexpr std::uint64_t kMaximumOrdinalValue = 4294967295ULL;
    static constexpr std::uint64_t kMaximumOutputs = 1025U;
};

enum class CanonicalFamily {
    mesh_gltf2,
    point_gltf2,
    instance_gltf2,
    composite_children
};

enum class OutputKind { canonical_content, parent_manifest };

struct FamilyCapability {
    CanonicalFamily canonical_family = CanonicalFamily::point_gltf2;
    std::string normalization_version;
    std::string canonical_contract_version;
    std::string validator_name;
    std::string validator_version;
    std::string validator_build_sha256;
    std::uint64_t maximum_input_bytes = 0U;
    std::uint64_t maximum_output_bytes = 0U;
};

struct ClaimRequest {
    std::string worker_id;
    std::string protocol_version = kProtocolVersion;
    std::string schema_sha256 = kSchemaSha256;
    std::vector<std::string> decoder_capabilities;
    std::vector<FamilyCapability> family_capabilities;
    std::string resource_profile_version = kResourceProfileVersion;
    std::string resource_profile_sha256;
    std::vector<ToolVersion> tool_versions;
};

struct ClaimTask {
    std::string task_id;
    std::string attempt_id;
    std::string request_id;
    std::string lease_token;
    std::string lease_expire_time;
    std::string hard_deadline_time;
    std::string tile_content_id;
    std::string root_object_id;
    std::string source_closure_hash;
    std::string resource_closure_version;
    std::string normalization_version;
    CanonicalFamily canonical_family = CanonicalFamily::point_gltf2;
    std::string canonical_contract_version;
    std::string resource_profile_version;
    std::string resource_profile_sha256;
    std::vector<std::string> required_decoders;
    std::uint64_t maximum_outputs = 0U;
    ResourceManifestDescriptor resource_manifest;
};

/** Optional fields serialize as JSON null for the legacy common summary. */
struct ValidationSummary {
    std::optional<std::string> coordinate_basis;
    std::optional<std::uint64_t> scene_count;
    std::optional<std::uint64_t> node_count;
    std::optional<std::uint64_t> primitive_count;
    std::optional<std::uint64_t> accessor_count;
    std::optional<std::uint64_t> buffer_count;
    std::optional<std::uint64_t> image_count;
    std::optional<std::uint64_t> feature_count;
    std::optional<std::uint64_t> metadata_property_count;
    std::optional<std::uint64_t> point_count;
    std::optional<std::uint64_t> instance_count;
    std::optional<std::uint64_t> composite_child_count;
    std::optional<std::uint64_t> boundary_expanded_instance_count;
    std::optional<std::string> feature_identity_model;
    std::vector<std::string> required_extensions;
    std::vector<std::string> used_extensions;
    std::string validator_name;
    std::string validator_version;
    std::string validator_build_sha256;
    std::string validation_hash;
};

struct OutputDeclaration {
    std::string output_id;
    std::vector<std::uint32_t> ordinal_path;
    OutputKind output_kind = OutputKind::canonical_content;
    CanonicalFamily canonical_family = CanonicalFamily::point_gltf2;
    std::string canonical_contract_version;
    std::uint64_t output_size = 0U;
    std::string output_sha256;
    std::string semantic_hash;
    std::string validation_manifest_sha256;
    ValidationSummary validation_summary;
};

struct OutputGrant {
    std::string output_id;
    std::string upload_grant_id;
    std::string http_method;
    std::string upload_url;
    std::string expires_at;
};

struct OutputReport {
    std::string output_id;
    std::string output_etag;
    std::uint64_t output_size = 0U;
    std::string output_sha256;
};

struct Completion {
    std::vector<std::string> ordered_output_ids;
    std::string output_manifest_sha256;
    bool global_preview_only = false;
};

[[nodiscard]] std::string canonicalFamilyName(CanonicalFamily family);
[[nodiscard]] std::string outputKindName(OutputKind kind);
[[nodiscard]] std::string canonicalContractVersion(CanonicalFamily family);
[[nodiscard]] std::string normalizationVersion(CanonicalFamily family);
[[nodiscard]] CanonicalFamily fromV1Family(
        normalization::CanonicalFamily family);

[[nodiscard]] std::string serializeClaimRequest(const ClaimRequest& request);
[[nodiscard]] ClaimTask parseClaimTask(const std::string& json);
[[nodiscard]] std::string serializeHeartbeatRequest(
        const std::string& worker_id, const ClaimTask& task, TaskPhase phase,
        std::uint64_t total_resources, std::uint64_t processed_resources);
[[nodiscard]] std::string serializeOutputPrepareRequest(
        const std::string& worker_id, const ClaimTask& task,
        const OutputDeclaration& declaration);
[[nodiscard]] OutputGrant parseOutputGrant(const std::string& json);
[[nodiscard]] std::string serializeOutputReportRequest(
        const std::string& worker_id, const ClaimTask& task,
        const OutputReport& report);
[[nodiscard]] std::string serializeCompleteRequest(
        const std::string& worker_id, const ClaimTask& task,
        const Completion& completion);
[[nodiscard]] std::string serializeFailRequest(
        const std::string& worker_id, const ClaimTask& task,
        const Failure& failure);

void validateClaimTask(const ClaimTask& task,
                       const ClaimRequest& capabilities,
                       const std::string& now_utc);
void validateResourceManifestPage(const ResourceManifestPage& page,
                                  const ClaimTask& task,
                                  const std::string& now_utc);
void validateResourceManifest(const ClaimTask& task,
                              const std::vector<ResourceManifestPage>& pages,
                              const std::string& now_utc,
                              std::uint64_t maximum_input_bytes);
void validateOutputDeclaration(const OutputDeclaration& declaration,
                               const ClaimTask& task,
                               const ClaimRequest& capabilities);

[[nodiscard]] ValidationSummary contentValidationSummary(
        const CanonicalArtifactEvidence& evidence,
        CanonicalFamily family,
        std::optional<std::uint64_t> point_count = std::nullopt,
        std::optional<std::uint64_t> instance_count = std::nullopt,
        std::optional<std::uint64_t> boundary_expanded_instance_count =
                std::nullopt);
[[nodiscard]] ValidationSummary parentValidationSummary(
        std::uint64_t child_count, const ToolVersion& validator);
[[nodiscard]] std::string validationManifestSha256(
        const ValidationSummary& summary);
[[nodiscard]] std::string outputId(const ClaimTask& task,
                                   const OutputDeclaration& declaration);
[[nodiscard]] std::string outputManifestSha256(
        const std::vector<OutputDeclaration>& ordered_outputs);
[[nodiscard]] std::string compositeChildId(
        const ClaimTask& task, const std::vector<std::uint32_t>& ordinal_path,
        const std::string& source_kind, const std::string& source_version,
        const std::string& source_sha256);

}  // namespace clip_worker::normalization::v2
