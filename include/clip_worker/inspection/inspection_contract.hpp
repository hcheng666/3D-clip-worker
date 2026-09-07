#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::inspection {

inline constexpr const char* kProtocolVersion = "THREE_D_TILES_INSPECTOR_V1";
inline constexpr const char* kSchemaSha256 =
        "c1f3428b8e6610bcb08a5cd6e615b0ca5174b3402634ea8744f9a23473590d9a";
inline constexpr const char* kContentResourceClosureVersion =
        "CONTENT_RESOURCE_CLOSURE_V1";

struct ProtocolLimits {
    static constexpr std::size_t kMaximumIdentifierUtf8Bytes = 256U;
    static constexpr std::size_t kMaximumPackagePathUtf8Bytes = 1024U;
    static constexpr std::size_t kMaximumEtagUtf8Bytes = 1024U;
    static constexpr std::size_t kMaximumGrantUrlUtf8Bytes = 8192U;
    static constexpr std::size_t kMaximumJsonPointerUtf8Bytes = 2048U;
    static constexpr std::size_t kMaximumManifestPageRecords = 1000U;
    static constexpr std::size_t kMaximumResultPageRecords = 1000U;
    static constexpr std::size_t kMaximumHierarchyPageRecords = 1000U;
    static constexpr std::size_t kMaximumPageJsonBytes = 1048576U;
    static constexpr std::size_t kMaximumDiagnostics = 1000U;
    static constexpr std::size_t kMaximumDiagnosticMessageUtf8Bytes = 1024U;
    static constexpr std::size_t kMaximumDiagnosticJsonBytes = 1048576U;
    static constexpr std::size_t kMaximumDependenciesPerResource = 4096U;
    static constexpr std::size_t kMaximumRequiredExtensionsPerResource = 256U;
    static constexpr std::size_t kMaximumExtensionUtf8Bytes = 256U;
    static constexpr std::size_t kMaximumToolRecords = 16U;
    static constexpr std::int64_t kMinimumGrantLifetimeSeconds = 30;
    static constexpr std::int64_t kMaximumGrantLifetimeSeconds = 900;
    static constexpr std::int64_t kMaximumInspectionDeadlineSeconds = 900;
    static constexpr std::uint64_t kMaximumSourceObjectBytes = 2147483648ULL;
    static constexpr std::uint64_t kMaximumExpandedResourceBytes = 8589934592ULL;
    static constexpr std::uint64_t kMaximumResourceCount = 100000ULL;
    static constexpr std::uint64_t kMaximumHierarchyRecordCount = 10000000ULL;
    static constexpr std::uint64_t kMaximumAvailableTileCount = 5000000ULL;
    static constexpr std::uint64_t kMaximumDocumentCount = 1000000ULL;
    static constexpr std::uint64_t kMaximumSubtreeCount = 100000ULL;
    static constexpr std::uint32_t kMaximumContentsPerTile = 64U;
    static constexpr std::uint32_t kMaximumTileDepth = 512U;
};

enum class MessageType {
    inspection_request,
    source_manifest_page,
    inspection_result,
    result_page,
    hierarchy_result_page
};

enum class PackageKind { directory, zip, three_tz };
enum class GrantPurpose { read_manifest_page, read_source_object };
enum class HttpMethod { get };
enum class ToolName { inspector, archive_reader, three_d_tiles_validator, gltf_validator };
enum class Outcome { succeeded, rejected, limit_exceeded, failed, cancelled };
enum class RootSelectionMethod { explicit_root, unique_top_level, unique_graph_root, none, ambiguous };
enum class DiagnosticSeverity { info, warning, error };
enum class InspectorStage {
    request_validation,
    manifest_fetch,
    package_enumeration,
    resource_resolution,
    structure_classification,
    closure_validation,
    result_publication
};
enum class DiagnosticCode {
    request_invalid,
    protocol_version_unsupported,
    resource_profile_mismatch,
    deadline_expired,
    manifest_invalid,
    manifest_page_invalid,
    manifest_hash_mismatch,
    grant_invalid,
    grant_expired,
    object_identity_mismatch,
    package_invalid,
    package_limit_exceeded,
    resource_invalid,
    resource_limit_exceeded,
    root_selection_required,
    content_invalid,
    closure_invalid,
    result_manifest_invalid,
    result_publication_failed,
    inspector_internal_failure
};
enum class ResourceRole {
    package_archive,
    package_object,
    tileset,
    content,
    subtree,
    buffer,
    image,
    schema,
    other
};
enum class DetectedKind {
    unknown,
    tileset_json,
    gltf_json,
    subtree_json,
    glb,
    b3dm,
    i3dm,
    pnts,
    cmpt,
    subtree_binary,
    binary,
    image
};
enum class HierarchyRecordType {
    document,
    explicit_tile,
    implicit_subtree,
    implicit_tile,
    content
};
enum class BoundingVolumeType { region, box, sphere };
enum class RefineMode { add, replace };
enum class SubdivisionScheme { quadtree, octree };
enum class ContentKind { mesh, point, instance, composite, external_tileset, unsupported, unknown };
enum class ContentFormat { b3dm, glb, gltf, pnts, i3dm, cmpt, json, unknown };

struct AccessGrant {
    std::string grant_id;
    GrantPurpose purpose = GrantPurpose::read_source_object;
    HttpMethod http_method = HttpMethod::get;
    std::string url;
    std::string expires_at;
};

struct ToolVersion {
    ToolName name = ToolName::inspector;
    std::string version;
    std::optional<std::string> build_sha256;
};

struct ManifestDescriptor {
    std::string manifest_id;
    std::uint64_t object_count = 0U;
    std::uint32_t page_count = 0U;
    std::uint32_t page_size = 0U;
    std::string manifest_sha256;
    AccessGrant page_grant;
};

struct SourceObject {
    std::string object_id;
    std::string package_relative_path;
    std::uint64_t size_bytes = 0U;
    std::string etag;
    std::optional<std::string> sha256;
    AccessGrant read_grant;
};

struct InspectionRequest {
    MessageType message_type = MessageType::inspection_request;
    std::string protocol_version;
    std::string inspection_id;
    std::string request_id;
    std::string source_generation;
    std::string inspector_version;
    std::string resource_profile_version;
    std::string resource_profile_sha256;
    std::string deadline_time;
    PackageKind package_kind = PackageKind::directory;
    std::optional<std::string> selected_root_hint;
    std::vector<ToolVersion> required_tools;
    ManifestDescriptor source_manifest;
};

struct SourceManifestPage {
    MessageType message_type = MessageType::source_manifest_page;
    std::string protocol_version;
    std::string inspection_id;
    std::string request_id;
    std::string manifest_id;
    std::uint32_t page_number = 0U;
    std::uint32_t record_count = 0U;
    std::string page_sha256;
    std::vector<SourceObject> records;
};

struct ResultManifestDescriptor {
    std::string manifest_id;
    std::uint64_t record_count = 0U;
    std::uint32_t page_count = 0U;
    std::uint32_t page_size = 0U;
    std::string manifest_sha256;
};

struct RootSelectionEvidence {
    RootSelectionMethod method = RootSelectionMethod::none;
    std::optional<std::string> selected_root_path;
    std::uint32_t candidate_count = 0U;
};

struct Diagnostic {
    DiagnosticCode code = DiagnosticCode::request_invalid;
    DiagnosticSeverity severity = DiagnosticSeverity::error;
    InspectorStage stage = InspectorStage::request_validation;
    std::string safe_message;
    bool retryable = false;
    std::uint64_t occurrence_count = 0U;
    std::optional<std::string> object_id;
    std::optional<std::string> package_relative_path;
    std::optional<std::string> json_pointer;
    std::optional<std::uint64_t> byte_offset;
};

struct InspectionResult {
    MessageType message_type = MessageType::inspection_result;
    std::string protocol_version;
    std::string inspection_id;
    std::string request_id;
    std::string source_generation;
    std::string inspector_version;
    Outcome outcome = Outcome::failed;
    std::vector<ToolVersion> tool_versions;
    std::optional<std::string> source_closure_hash;
    RootSelectionEvidence root_selection;
    std::uint64_t total_resources = 0U;
    std::uint64_t total_documents = 0U;
    std::uint64_t total_tiles = 0U;
    std::uint64_t total_subtrees = 0U;
    std::uint64_t total_contents = 0U;
    ResultManifestDescriptor result_manifest;
    ResultManifestDescriptor hierarchy_manifest;
    std::vector<std::string> required_extensions;
    std::vector<std::string> used_extensions;
    std::vector<Diagnostic> diagnostics;
};

struct ResourceEvidence {
    std::string object_id;
    std::string package_relative_path;
    ResourceRole role = ResourceRole::other;
    std::optional<std::string> media_type;
    DetectedKind detected_kind = DetectedKind::unknown;
    std::optional<std::string> detected_version;
    std::uint64_t observed_size = 0U;
    std::string observed_etag;
    std::string observed_sha256;
    bool required = false;
    std::vector<std::string> dependency_ids;
    std::vector<std::string> required_extensions;
    std::vector<std::string> used_extensions;
};

struct ResultPage {
    MessageType message_type = MessageType::result_page;
    std::string protocol_version;
    std::string inspection_id;
    std::string request_id;
    std::string manifest_id;
    std::uint32_t page_number = 0U;
    std::uint32_t record_count = 0U;
    std::string page_sha256;
    std::vector<ResourceEvidence> records;
};

/** Conditionally typed hierarchy fact; absent fields are omitted from JSON. */
struct HierarchyRecord {
    HierarchyRecordType record_type = HierarchyRecordType::document;
    std::string record_id;
    std::optional<std::string> document_id;
    std::optional<std::string> resource_object_id;
    std::optional<std::string> parent_document_id;
    std::optional<std::string> parent_content_id;
    std::optional<std::uint32_t> parent_content_ordinal;
    std::optional<std::uint32_t> document_depth;
    std::optional<std::string> asset_version;
    std::optional<std::string> document_sha256;
    std::optional<std::string> root_tile_id;
    std::optional<std::string> gltf_up_axis;
    std::optional<std::string> tile_id;
    std::optional<std::string> parent_tile_id;
    std::optional<std::string> tile_json_pointer;
    std::optional<std::uint64_t> tile_level;
    std::optional<std::uint64_t> implicit_x;
    std::optional<std::uint64_t> implicit_y;
    std::optional<std::uint64_t> implicit_z;
    std::optional<std::vector<double>> transform;
    std::optional<BoundingVolumeType> bounding_volume_type;
    std::optional<std::vector<double>> bounding_volume;
    std::optional<double> geometric_error;
    std::optional<RefineMode> refine;
    std::optional<std::uint32_t> content_count;
    std::optional<bool> has_children;
    std::optional<bool> implicit_root;
    std::optional<std::string> subtree_id;
    std::optional<SubdivisionScheme> subdivision_scheme;
    std::optional<std::uint32_t> subtree_levels;
    std::optional<std::uint64_t> available_levels;
    std::optional<std::uint64_t> available_tile_count;
    std::optional<std::uint64_t> available_content_count;
    std::optional<std::uint64_t> available_child_subtree_count;
    std::optional<std::uint32_t> content_stream_count;
    std::optional<std::string> content_id;
    std::optional<std::uint32_t> content_ordinal;
    std::optional<SubdivisionScheme> content_subdivision_scheme;
    std::optional<std::string> source_uri;
    std::optional<std::string> source_resource_object_id;
    std::optional<ContentKind> content_kind;
    std::optional<ContentFormat> content_format;
    std::optional<std::string> resource_closure_hash;
    std::optional<std::string> resource_closure_version;
    std::optional<std::string> group_id;
    std::optional<BoundingVolumeType> content_bounding_volume_type;
    std::optional<std::vector<double>> content_bounding_volume;
    std::vector<std::string> required_extensions;
    std::vector<std::string> used_extensions;
};

struct HierarchyPage {
    MessageType message_type = MessageType::hierarchy_result_page;
    std::string protocol_version;
    std::string inspection_id;
    std::string request_id;
    std::string manifest_id;
    std::uint32_t page_number = 0U;
    std::uint32_t record_count = 0U;
    std::string page_sha256;
    std::vector<HierarchyRecord> records;
};

[[nodiscard]] InspectionRequest parseInspectionRequest(const std::string& json);
[[nodiscard]] SourceManifestPage parseSourceManifestPage(const std::string& json);
[[nodiscard]] InspectionResult parseInspectionResult(const std::string& json);
[[nodiscard]] ResultPage parseResultPage(const std::string& json);
[[nodiscard]] HierarchyPage parseHierarchyPage(const std::string& json);
[[nodiscard]] std::string serializeInspectionResult(const InspectionResult& result);
[[nodiscard]] std::string serializeResultPage(const ResultPage& page);
[[nodiscard]] std::string serializeHierarchyPage(const HierarchyPage& page);
[[nodiscard]] std::int64_t utcEpochSeconds(const std::string& utc_seconds);

void validateRequest(const InspectionRequest& request, const std::string& now_utc);
void validateSourceManifestPage(const SourceManifestPage& page,
                                const InspectionRequest& request,
                                const std::string& now_utc);
void validateSourceManifest(const InspectionRequest& request,
                            const std::vector<SourceManifestPage>& pages,
                            const std::string& now_utc);
void validateResult(const InspectionResult& result, const InspectionRequest& request);
void validateResultPages(const InspectionResult& result,
                         const std::vector<ResultPage>& pages);
void validateHierarchyPages(const InspectionResult& result,
                            const std::vector<HierarchyPage>& pages);
void validateCompletedExchange(const InspectionRequest& request,
                               const std::vector<SourceManifestPage>& source_pages,
                               const InspectionResult& result,
                               const std::vector<ResultPage>& result_pages,
                               const std::vector<HierarchyPage>& hierarchy_pages,
                               const std::string& now_utc);

[[nodiscard]] std::string canonicalPageSha256(const std::vector<SourceObject>& records);
[[nodiscard]] std::string canonicalPageSha256(const std::vector<ResourceEvidence>& records);
[[nodiscard]] std::string canonicalPageSha256(const std::vector<HierarchyRecord>& records);
[[nodiscard]] std::string canonicalResultManifestSha256(
        const std::vector<ResultPage>& pages);
[[nodiscard]] std::string canonicalHierarchyManifestSha256(
        const std::vector<HierarchyPage>& pages);

}  // namespace clip_worker::inspection
