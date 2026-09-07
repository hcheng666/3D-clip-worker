#pragma once

#include "clip_worker/authorization/authorization_clip_proof.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/normalization_contract.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::authorization::v2 {

inline constexpr const char* kProtocolVersion = "THREE_D_TILES_CLIPPER_V2";
inline constexpr const char* kSchemaSha256 =
        "35cfe39f12c2e3a8d6ce11259b7333261a61707cca06538f3e9482d8cb533c6d";
inline constexpr const char* kResourceProfileVersion =
        "STANDARD_4CPU_8GIB_SINGLE_TASK_V4";
inline constexpr const char* kMeshClipStrategyVersion =
        "clip-mesh-canonical-v1";
inline constexpr const char* kPointClipStrategyVersion =
        "clip-point-canonical-v1";
inline constexpr const char* kInstanceClipStrategyVersion =
        "clip-instance-canonical-v1";
inline constexpr const char* kCoordinateBasis =
        normalization::kCoordinateBasis;
inline constexpr const char* kClippedOutputIdDomain =
        "three-d-authorization-clipped-output-v1";
inline constexpr const char* kOutputManifestHashDomain =
        "three-d-authorization-clipped-output-manifest-v1";

struct Limits {
    static constexpr std::size_t kTransformElements = 16U;
    static constexpr std::size_t kMaximumOrdinalDepth = 8U;
    static constexpr std::size_t kMaximumFamilyCapabilities = 16U;
    static constexpr std::size_t kMaximumOutputs = 2U;
};

enum class CanonicalFamily { mesh_gltf2, point_gltf2, instance_gltf2 };
enum class CompletionOutcome { empty, safe_whole, clipped };
enum class OutputKind { clipped_canonical };
enum class FailureCode {
    download_failed,
    input_identity_mismatch,
    input_validation_failed,
    classification_failed,
    clip_failed,
    output_validation_failed,
    output_upload_failed,
    resource_limit_exceeded,
    lease_expired,
    attempts_exhausted,
    publication_identity_mismatch,
    internal_error
};
enum class TaskPhase {
    claimed,
    downloading,
    verifying,
    classifying,
    clipping,
    validating_output,
    uploading,
    completing
};

struct FamilyCapability {
    std::string clip_contract_version = kProtocolVersion;
    CanonicalFamily canonical_family = CanonicalFamily::mesh_gltf2;
    std::string canonical_contract_version;
    std::string clip_strategy_version;
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
    std::string resource_profile_version = kResourceProfileVersion;
    std::string resource_profile_sha256;
    std::vector<FamilyCapability> family_capabilities;
};

struct AccessGrant {
    std::string grant_id;
    std::string http_method;
    std::string url;
    std::string expires_at;
};

struct TaskLimits {
    std::uint64_t maximum_input_bytes = 0U;
    std::uint64_t maximum_output_bytes = 0U;
    std::uint64_t maximum_aggregate_output_bytes = 0U;
    std::uint64_t maximum_points = 0U;
    std::uint64_t maximum_instances = 0U;
    std::uint64_t maximum_boundary_instances = 0U;
    std::uint64_t maximum_expanded_vertices = 0U;
    std::uint64_t maximum_expanded_indices = 0U;
    std::uint32_t maximum_outputs = 0U;
};

struct ClaimTask {
    std::string work_item_id;
    std::string attempt_id;
    std::string request_id;
    std::string lease_token;
    std::string lease_expire_time;
    std::string hard_deadline_time;
    std::string tile_content_id;
    std::vector<std::uint32_t> source_ordinal_path;
    std::string source_artifact_kind;
    std::string source_artifact_id;
    CanonicalFamily canonical_family = CanonicalFamily::mesh_gltf2;
    std::string canonical_contract_version;
    std::string clip_strategy_version;
    std::uint64_t input_size = 0U;
    std::string input_sha256;
    std::string input_semantic_hash;
    std::string input_validation_manifest_sha256;
    AccessGrant input_grant;
    std::string scope_wkb_base64;
    std::int32_t scope_srid = 0;
    std::string scope_hash;
    std::array<double, Limits::kTransformElements> accumulated_transform{};
    std::string transform_hash;
    std::string canonical_coordinate_basis;
    std::string resource_profile_version;
    std::string resource_profile_sha256;
    TaskLimits limits;
};

struct OutputDeclaration {
    std::string output_id;
    std::uint32_t output_ordinal = 0U;
    OutputKind output_kind = OutputKind::clipped_canonical;
    CanonicalFamily canonical_family = CanonicalFamily::mesh_gltf2;
    std::string canonical_contract_version;
    normalization::CanonicalArtifactEvidence evidence;
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
    CompletionOutcome outcome = CompletionOutcome::empty;
    AuthorizationClipProof proof;
    std::vector<std::string> ordered_output_ids;
    std::optional<std::string> output_manifest_sha256;
};

struct Failure {
    FailureCode error_code = FailureCode::internal_error;
    std::string error_message;
    bool retryable = false;
};

[[nodiscard]] const char* canonicalFamilyName(CanonicalFamily family) noexcept;
[[nodiscard]] const char* completionOutcomeName(
        CompletionOutcome outcome) noexcept;
[[nodiscard]] const char* taskPhaseName(TaskPhase phase) noexcept;
[[nodiscard]] const char* failureCodeName(FailureCode code) noexcept;
[[nodiscard]] std::string clippedOutputId(
        const std::string& work_item_id, std::uint32_t output_ordinal);
[[nodiscard]] std::string outputManifestSha256(
        const std::vector<OutputDeclaration>& ordered_outputs);

[[nodiscard]] std::string serializeClaimRequest(const ClaimRequest& request);
[[nodiscard]] ClaimTask parseClaimTask(const std::string& json);
[[nodiscard]] std::string serializeHeartbeatRequest(
        const std::string& worker_id, const ClaimTask& task, TaskPhase phase,
        std::uint64_t processed_bytes);
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

void validateClaimTask(const ClaimTask& task, const ClaimRequest& capability,
                       const std::string& now_utc);
void validateOutputDeclaration(const OutputDeclaration& declaration,
                               const ClaimTask& task);

}  // namespace clip_worker::authorization::v2
