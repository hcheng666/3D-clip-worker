#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::normalization {

inline constexpr const char* kProtocolVersion = "THREE_D_TILES_NORMALIZER_V1";
inline constexpr const char* kSchemaSha256 =
        "f9adef70706c010979e0bc65306cee9692af338941e13a0a554fe6879d6319c6";
inline constexpr const char* kNormalizationVersion = "normalization-v1";
inline constexpr const char* kCanonicalContractVersion = "canonical-gltf2-v1";
inline constexpr const char* kManifestVersion =
        "NORMALIZATION_RESOURCE_MANIFEST_V1";
inline constexpr const char* kCoordinateBasis =
        "GLTF_2_0_CONTENT_LOCAL_RIGHT_HANDED";

struct ProtocolLimits {
    static constexpr std::size_t kMaximumIdentifierBytes = 2048U;
    static constexpr std::size_t kMaximumPathBytes = 2048U;
    static constexpr std::size_t kMaximumGrantUrlBytes = 8192U;
    static constexpr std::size_t kMaximumPageRecords = 1000U;
    static constexpr std::size_t kMaximumPages = 100000U;
    static constexpr std::size_t kMaximumResources = 1000000U;
    static constexpr std::size_t kMaximumExtensions = 256U;
    static constexpr std::size_t kMaximumTools = 32U;
    static constexpr std::size_t kMaximumFamilies = 16U;
    static constexpr std::size_t kMaximumDecoders = 6U;
    static constexpr std::size_t kMaximumSafeMessageBytes = 512U;
    static constexpr std::uint64_t kMaximumArtifactBytes = 4294967296ULL;
};

enum class CanonicalFamily { mesh_gltf2, point_gltf2, instance_gltf2 };
enum class FeatureIdentityModel {
    none,
    legacy_batch_table_mapped,
    attribute_feature_id_property_table,
    point_feature_id,
    instance_feature_id
};
enum class TaskPhase {
    claimed,
    manifest_fetch,
    resource_download,
    decode,
    normalize,
    validate,
    upload_prepare,
    upload,
    complete
};

struct ToolVersion {
    std::string name;
    std::string version;
    std::string build_sha256;
};

struct FamilyCapability {
    CanonicalFamily canonical_family = CanonicalFamily::mesh_gltf2;
    std::string canonical_contract_version = kCanonicalContractVersion;
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
    std::vector<std::string> supported_normalization_versions;
    std::vector<FamilyCapability> family_capabilities;
    std::string resource_profile_version;
    std::string resource_profile_sha256;
    std::vector<ToolVersion> tool_versions;
};

struct ResourceManifestDescriptor {
    std::string manifest_version;
    std::string manifest_id;
    std::uint32_t page_count = 0U;
    std::uint32_t page_size = 0U;
    std::uint64_t record_count = 0U;
    std::string manifest_sha256;
};

struct ClaimTask {
    std::string task_id;
    std::string attempt_id;
    std::string request_id;
    std::string lease_token;
    std::string lease_expire_time;
    std::string hard_deadline_time;
    std::string tile_content_id;
    std::string source_closure_hash;
    std::string resource_closure_version;
    std::string normalization_version;
    CanonicalFamily canonical_family = CanonicalFamily::mesh_gltf2;
    std::string canonical_contract_version;
    std::string resource_profile_version;
    std::string resource_profile_sha256;
    std::vector<std::string> required_decoders;
    ResourceManifestDescriptor resource_manifest;
};

struct AccessGrant {
    std::string grant_id;
    std::string http_method;
    std::string url;
    std::string expires_at;
};

struct ResourceRecord {
    std::string object_id;
    std::string package_relative_path;
    std::string resource_role;
    std::string detected_kind;
    std::optional<std::string> detected_version;
    std::optional<std::string> media_type;
    std::uint64_t size = 0U;
    std::string sha256;
    std::vector<std::string> required_extensions;
    std::vector<std::string> used_extensions;
    std::vector<std::string> dependency_ids;
    AccessGrant access_grant;
};

struct ResourceManifestPage {
    std::string manifest_id;
    std::uint32_t page_number = 0U;
    std::uint32_t record_count = 0U;
    std::string page_sha256;
    std::vector<ResourceRecord> records;
};

struct HeartbeatResponse {
    std::string lease_expire_time;
    std::string hard_deadline_time;
    bool cancel_requested = false;
};

struct ValidationSummary {
    std::string coordinate_basis = kCoordinateBasis;
    std::uint64_t scene_count = 0U;
    std::uint64_t node_count = 0U;
    std::uint64_t primitive_count = 0U;
    std::uint64_t accessor_count = 0U;
    std::uint64_t buffer_count = 0U;
    std::uint64_t image_count = 0U;
    std::uint64_t feature_count = 0U;
    std::uint64_t metadata_property_count = 0U;
    FeatureIdentityModel feature_identity_model = FeatureIdentityModel::none;
    std::vector<std::string> required_extensions;
    std::vector<std::string> used_extensions;
    std::string validator_name;
    std::string validator_version;
    std::string validator_build_sha256;
    std::string validation_hash;
};

struct UploadDeclaration {
    CanonicalFamily canonical_family = CanonicalFamily::mesh_gltf2;
    std::string canonical_contract_version = kCanonicalContractVersion;
    std::uint64_t output_size = 0U;
    std::string output_sha256;
    std::string semantic_hash;
    std::string validation_manifest_sha256;
    ValidationSummary validation_summary;
};

struct UploadGrant {
    std::string upload_grant_id;
    std::string http_method;
    std::string upload_url;
    std::string expires_at;
};

struct UploadReport {
    std::string output_etag;
    std::uint64_t output_size = 0U;
    std::string output_sha256;
};

struct Failure {
    std::string error_code;
    std::optional<std::string> error_message;
    bool retryable = false;
    bool unsupported = false;
};

[[nodiscard]] std::string serializeClaimRequest(const ClaimRequest& request);
[[nodiscard]] ClaimTask parseClaimTask(const std::string& json);
[[nodiscard]] ResourceManifestPage parseResourceManifestPage(
        const std::string& json);
[[nodiscard]] std::string serializeHeartbeatRequest(
        const std::string& worker_id, const ClaimTask& task, TaskPhase phase,
        std::uint64_t total_resources, std::uint64_t processed_resources);
[[nodiscard]] HeartbeatResponse parseHeartbeatResponse(const std::string& json);
[[nodiscard]] std::string serializeUploadPrepareRequest(
        const std::string& worker_id, const ClaimTask& task,
        const UploadDeclaration& declaration);
[[nodiscard]] UploadGrant parseUploadGrant(const std::string& json);
[[nodiscard]] std::string serializeUploadReportRequest(
        const std::string& worker_id, const ClaimTask& task,
        const UploadReport& report);
[[nodiscard]] std::string serializeCompleteRequest(
        const std::string& worker_id, const ClaimTask& task);
[[nodiscard]] std::string serializeFailRequest(
        const std::string& worker_id, const ClaimTask& task,
        const Failure& failure);

void validateClaimTask(const ClaimTask& task, const ClaimRequest& capabilities,
                       const std::string& now_utc);
void validateResourceManifestPage(const ResourceManifestPage& page,
                                  const ClaimTask& task,
                                  const std::string& now_utc);
void validateResourceManifest(const ClaimTask& task,
                              const std::vector<ResourceManifestPage>& pages,
                              const std::string& now_utc,
                              std::uint64_t maximum_input_bytes =
                                      ProtocolLimits::kMaximumArtifactBytes);
void validateUploadDeclaration(const UploadDeclaration& declaration,
                               const ClaimTask& task);

[[nodiscard]] std::string canonicalPageSha256(
        const std::vector<ResourceRecord>& records);
[[nodiscard]] std::string canonicalManifestSha256(
        const std::vector<ResourceManifestPage>& pages);
[[nodiscard]] std::string sha256Hex(const std::string& bytes);
[[nodiscard]] std::string canonicalFamilyName(CanonicalFamily family);

}  // namespace clip_worker::normalization
