#include "clip_worker/inspection/inspection_contract.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace clip_worker::inspection {
namespace {

using Json = nlohmann::json;

constexpr const char* kEmptySha256 =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr std::size_t kSha256Bytes = 32U;
constexpr std::array<std::string_view, 18U> kSensitiveMarkers = {
        "http://", "https://", "file://", "x-amz-", "signature=", "token=",
        "access_key", "secret_key", "session_token", "authorization:", "bucket",
        "object key", "object-key", "object_key", "objectkey", ":\\", ":/", "/tmp/"};

template <typename Enum>
using EnumEntry = std::pair<std::string_view, Enum>;

constexpr std::array<EnumEntry<MessageType>, 5U> kMessageTypes = {{
        {"INSPECTION_REQUEST", MessageType::inspection_request},
        {"SOURCE_MANIFEST_PAGE", MessageType::source_manifest_page},
        {"INSPECTION_RESULT", MessageType::inspection_result},
        {"RESULT_PAGE", MessageType::result_page},
        {"HIERARCHY_RESULT_PAGE", MessageType::hierarchy_result_page}}};
constexpr std::array<EnumEntry<PackageKind>, 3U> kPackageKinds = {{
        {"DIRECTORY", PackageKind::directory}, {"ZIP", PackageKind::zip},
        {"THREE_TZ", PackageKind::three_tz}}};
constexpr std::array<EnumEntry<GrantPurpose>, 2U> kGrantPurposes = {{
        {"READ_MANIFEST_PAGE", GrantPurpose::read_manifest_page},
        {"READ_SOURCE_OBJECT", GrantPurpose::read_source_object}}};
constexpr std::array<EnumEntry<HttpMethod>, 1U> kHttpMethods = {{{"GET", HttpMethod::get}}};
constexpr std::array<EnumEntry<ToolName>, 4U> kToolNames = {{
        {"INSPECTOR", ToolName::inspector}, {"ARCHIVE_READER", ToolName::archive_reader},
        {"THREE_D_TILES_VALIDATOR", ToolName::three_d_tiles_validator},
        {"GLTF_VALIDATOR", ToolName::gltf_validator}}};
constexpr std::array<EnumEntry<Outcome>, 5U> kOutcomes = {{
        {"SUCCEEDED", Outcome::succeeded}, {"REJECTED", Outcome::rejected},
        {"LIMIT_EXCEEDED", Outcome::limit_exceeded}, {"FAILED", Outcome::failed},
        {"CANCELLED", Outcome::cancelled}}};
constexpr std::array<EnumEntry<RootSelectionMethod>, 5U> kRootMethods = {{
        {"EXPLICIT", RootSelectionMethod::explicit_root},
        {"UNIQUE_TOP_LEVEL", RootSelectionMethod::unique_top_level},
        {"UNIQUE_GRAPH_ROOT", RootSelectionMethod::unique_graph_root},
        {"NONE", RootSelectionMethod::none}, {"AMBIGUOUS", RootSelectionMethod::ambiguous}}};
constexpr std::array<EnumEntry<DiagnosticSeverity>, 3U> kDiagnosticSeverities = {{
        {"INFO", DiagnosticSeverity::info}, {"WARNING", DiagnosticSeverity::warning},
        {"ERROR", DiagnosticSeverity::error}}};
constexpr std::array<EnumEntry<InspectorStage>, 7U> kInspectorStages = {{
        {"REQUEST_VALIDATION", InspectorStage::request_validation},
        {"MANIFEST_FETCH", InspectorStage::manifest_fetch},
        {"PACKAGE_ENUMERATION", InspectorStage::package_enumeration},
        {"RESOURCE_RESOLUTION", InspectorStage::resource_resolution},
        {"STRUCTURE_CLASSIFICATION", InspectorStage::structure_classification},
        {"CLOSURE_VALIDATION", InspectorStage::closure_validation},
        {"RESULT_PUBLICATION", InspectorStage::result_publication}}};
constexpr std::array<EnumEntry<DiagnosticCode>, 20U> kDiagnosticCodes = {{
        {"REQUEST_INVALID", DiagnosticCode::request_invalid},
        {"PROTOCOL_VERSION_UNSUPPORTED", DiagnosticCode::protocol_version_unsupported},
        {"RESOURCE_PROFILE_MISMATCH", DiagnosticCode::resource_profile_mismatch},
        {"DEADLINE_EXPIRED", DiagnosticCode::deadline_expired},
        {"MANIFEST_INVALID", DiagnosticCode::manifest_invalid},
        {"MANIFEST_PAGE_INVALID", DiagnosticCode::manifest_page_invalid},
        {"MANIFEST_HASH_MISMATCH", DiagnosticCode::manifest_hash_mismatch},
        {"GRANT_INVALID", DiagnosticCode::grant_invalid},
        {"GRANT_EXPIRED", DiagnosticCode::grant_expired},
        {"OBJECT_IDENTITY_MISMATCH", DiagnosticCode::object_identity_mismatch},
        {"PACKAGE_INVALID", DiagnosticCode::package_invalid},
        {"PACKAGE_LIMIT_EXCEEDED", DiagnosticCode::package_limit_exceeded},
        {"RESOURCE_INVALID", DiagnosticCode::resource_invalid},
        {"RESOURCE_LIMIT_EXCEEDED", DiagnosticCode::resource_limit_exceeded},
        {"ROOT_SELECTION_REQUIRED", DiagnosticCode::root_selection_required},
        {"CONTENT_INVALID", DiagnosticCode::content_invalid},
        {"CLOSURE_INVALID", DiagnosticCode::closure_invalid},
        {"RESULT_MANIFEST_INVALID", DiagnosticCode::result_manifest_invalid},
        {"RESULT_PUBLICATION_FAILED", DiagnosticCode::result_publication_failed},
        {"INSPECTOR_INTERNAL_FAILURE", DiagnosticCode::inspector_internal_failure}}};
constexpr std::array<EnumEntry<ResourceRole>, 9U> kResourceRoles = {{
        {"PACKAGE_ARCHIVE", ResourceRole::package_archive},
        {"PACKAGE_OBJECT", ResourceRole::package_object}, {"TILESET", ResourceRole::tileset},
        {"CONTENT", ResourceRole::content}, {"SUBTREE", ResourceRole::subtree},
        {"BUFFER", ResourceRole::buffer}, {"IMAGE", ResourceRole::image},
        {"SCHEMA", ResourceRole::schema}, {"OTHER", ResourceRole::other}}};
constexpr std::array<EnumEntry<DetectedKind>, 12U> kDetectedKinds = {{
        {"UNKNOWN", DetectedKind::unknown}, {"TILESET_JSON", DetectedKind::tileset_json},
        {"GLTF_JSON", DetectedKind::gltf_json}, {"SUBTREE_JSON", DetectedKind::subtree_json},
        {"GLB", DetectedKind::glb}, {"B3DM", DetectedKind::b3dm},
        {"I3DM", DetectedKind::i3dm}, {"PNTS", DetectedKind::pnts},
        {"CMPT", DetectedKind::cmpt}, {"SUBTREE_BINARY", DetectedKind::subtree_binary},
        {"BINARY", DetectedKind::binary}, {"IMAGE", DetectedKind::image}}};
constexpr std::array<EnumEntry<HierarchyRecordType>, 5U> kHierarchyRecordTypes = {{
        {"DOCUMENT", HierarchyRecordType::document},
        {"EXPLICIT_TILE", HierarchyRecordType::explicit_tile},
        {"IMPLICIT_SUBTREE", HierarchyRecordType::implicit_subtree},
        {"IMPLICIT_TILE", HierarchyRecordType::implicit_tile},
        {"CONTENT", HierarchyRecordType::content}}};
constexpr std::array<EnumEntry<BoundingVolumeType>, 3U> kBoundingVolumeTypes = {{
        {"REGION", BoundingVolumeType::region}, {"BOX", BoundingVolumeType::box},
        {"SPHERE", BoundingVolumeType::sphere}}};
constexpr std::array<EnumEntry<RefineMode>, 2U> kRefineModes = {{
        {"ADD", RefineMode::add}, {"REPLACE", RefineMode::replace}}};
constexpr std::array<EnumEntry<SubdivisionScheme>, 2U> kSubdivisionSchemes = {{
        {"QUADTREE", SubdivisionScheme::quadtree},
        {"OCTREE", SubdivisionScheme::octree}}};
constexpr std::array<EnumEntry<ContentKind>, 7U> kContentKinds = {{
        {"MESH", ContentKind::mesh}, {"POINT", ContentKind::point},
        {"INSTANCE", ContentKind::instance}, {"COMPOSITE", ContentKind::composite},
        {"EXTERNAL_TILESET", ContentKind::external_tileset},
        {"UNSUPPORTED", ContentKind::unsupported}, {"UNKNOWN", ContentKind::unknown}}};
constexpr std::array<EnumEntry<ContentFormat>, 8U> kContentFormats = {{
        {"B3DM", ContentFormat::b3dm}, {"GLB", ContentFormat::glb},
        {"GLTF", ContentFormat::gltf}, {"PNTS", ContentFormat::pnts},
        {"I3DM", ContentFormat::i3dm}, {"CMPT", ContentFormat::cmpt},
        {"JSON", ContentFormat::json}, {"UNKNOWN", ContentFormat::unknown}}};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

Json parseObject(const std::string& text) {
    try {
        auto value = Json::parse(text);
        require(value.is_object(), "Inspector message must be a JSON object");
        return value;
    } catch (const nlohmann::json::exception&) {
        // JSON library diagnostics can echo a presigned URL from the payload.
        throw std::invalid_argument("Inspector message JSON is invalid");
    }
}

void requireOnly(const Json& value, std::initializer_list<const char*> allowed) {
    std::set<std::string> names;
    for (const char* name : allowed) {
        names.emplace(name);
    }
    for (const auto& item : value.items()) {
        require(names.find(item.key()) != names.end(),
                "Inspector message contains an unknown field");
    }
}

template <typename Value>
Value required(const Json& value, const char* field) {
    const auto item = value.find(field);
    require(item != value.end() && !item->is_null(), "Inspector message is missing a field");
    try {
        return item->get<Value>();
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Inspector message field has an invalid type");
    }
}

template <typename Value>
std::optional<Value> optional(const Json& value, const char* field) {
    const auto item = value.find(field);
    if (item == value.end()) {
        return std::nullopt;
    }
    require(!item->is_null(), "Inspector optional field must not be null");
    try {
        return item->get<Value>();
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Inspector optional field has an invalid type");
    }
}

template <typename Enum, std::size_t Size>
Enum parseEnum(const std::string& value, const std::array<EnumEntry<Enum>, Size>& entries) {
    for (const auto& entry : entries) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    throw std::invalid_argument("Inspector message contains an unsupported enum value");
}

template <typename Enum, std::size_t Size>
std::string enumName(Enum value, const std::array<EnumEntry<Enum>, Size>& entries) {
    for (const auto& entry : entries) {
        if (entry.second == value) {
            return std::string(entry.first);
        }
    }
    throw std::invalid_argument("Inspector enum cannot be serialized");
}

AccessGrant parseGrant(const Json& value) {
    require(value.is_object(), "Access grant must be an object");
    requireOnly(value, {"grantId", "purpose", "httpMethod", "url", "expiresAt"});
    AccessGrant result;
    result.grant_id = required<std::string>(value, "grantId");
    result.purpose = parseEnum(required<std::string>(value, "purpose"), kGrantPurposes);
    result.http_method = parseEnum(required<std::string>(value, "httpMethod"), kHttpMethods);
    result.url = required<std::string>(value, "url");
    result.expires_at = required<std::string>(value, "expiresAt");
    return result;
}

ToolVersion parseTool(const Json& value) {
    require(value.is_object(), "Tool version must be an object");
    requireOnly(value, {"name", "version", "buildSha256"});
    ToolVersion result;
    result.name = parseEnum(required<std::string>(value, "name"), kToolNames);
    result.version = required<std::string>(value, "version");
    result.build_sha256 = optional<std::string>(value, "buildSha256");
    return result;
}

std::vector<ToolVersion> parseTools(const Json& value, const char* field) {
    const auto tools = required<Json>(value, field);
    require(tools.is_array(), "Tool versions must be an array");
    std::vector<ToolVersion> result;
    result.reserve(tools.size());
    for (const auto& tool : tools) {
        result.push_back(parseTool(tool));
    }
    return result;
}

ManifestDescriptor parseManifestDescriptor(const Json& value) {
    require(value.is_object(), "Manifest descriptor must be an object");
    requireOnly(value, {"manifestId", "objectCount", "pageCount", "pageSize",
                        "manifestSha256", "pageGrant"});
    ManifestDescriptor result;
    result.manifest_id = required<std::string>(value, "manifestId");
    result.object_count = required<std::uint64_t>(value, "objectCount");
    result.page_count = required<std::uint32_t>(value, "pageCount");
    result.page_size = required<std::uint32_t>(value, "pageSize");
    result.manifest_sha256 = required<std::string>(value, "manifestSha256");
    result.page_grant = parseGrant(required<Json>(value, "pageGrant"));
    return result;
}

SourceObject parseSourceObject(const Json& value) {
    require(value.is_object(), "Source object must be an object");
    requireOnly(value, {"objectId", "packageRelativePath", "sizeBytes", "etag", "sha256",
                        "readGrant"});
    SourceObject result;
    result.object_id = required<std::string>(value, "objectId");
    result.package_relative_path = required<std::string>(value, "packageRelativePath");
    result.size_bytes = required<std::uint64_t>(value, "sizeBytes");
    result.etag = required<std::string>(value, "etag");
    result.sha256 = optional<std::string>(value, "sha256");
    result.read_grant = parseGrant(required<Json>(value, "readGrant"));
    return result;
}

ResultManifestDescriptor parseResultDescriptor(const Json& value) {
    require(value.is_object(), "Result manifest descriptor must be an object");
    requireOnly(value, {"manifestId", "recordCount", "pageCount", "pageSize",
                        "manifestSha256"});
    ResultManifestDescriptor result;
    result.manifest_id = required<std::string>(value, "manifestId");
    result.record_count = required<std::uint64_t>(value, "recordCount");
    result.page_count = required<std::uint32_t>(value, "pageCount");
    result.page_size = required<std::uint32_t>(value, "pageSize");
    result.manifest_sha256 = required<std::string>(value, "manifestSha256");
    return result;
}

RootSelectionEvidence parseRootSelection(const Json& value) {
    require(value.is_object(), "Root selection must be an object");
    requireOnly(value, {"method", "selectedRootPath", "candidateCount"});
    RootSelectionEvidence result;
    result.method = parseEnum(required<std::string>(value, "method"), kRootMethods);
    result.selected_root_path = optional<std::string>(value, "selectedRootPath");
    result.candidate_count = required<std::uint32_t>(value, "candidateCount");
    return result;
}

Diagnostic parseDiagnostic(const Json& value) {
    require(value.is_object(), "Diagnostic must be an object");
    requireOnly(value, {"code", "severity", "stage", "safeMessage", "retryable",
                        "occurrenceCount", "objectId", "packageRelativePath", "jsonPointer",
                        "byteOffset"});
    Diagnostic result;
    result.code = parseEnum(required<std::string>(value, "code"), kDiagnosticCodes);
    result.severity = parseEnum(required<std::string>(value, "severity"),
                                kDiagnosticSeverities);
    result.stage = parseEnum(required<std::string>(value, "stage"), kInspectorStages);
    result.safe_message = required<std::string>(value, "safeMessage");
    result.retryable = required<bool>(value, "retryable");
    result.occurrence_count = required<std::uint64_t>(value, "occurrenceCount");
    result.object_id = optional<std::string>(value, "objectId");
    result.package_relative_path = optional<std::string>(value, "packageRelativePath");
    result.json_pointer = optional<std::string>(value, "jsonPointer");
    result.byte_offset = optional<std::uint64_t>(value, "byteOffset");
    return result;
}

ResourceEvidence parseResourceEvidence(const Json& value) {
    require(value.is_object(), "Resource evidence must be an object");
    requireOnly(value, {"objectId", "packageRelativePath", "role", "mediaType",
                        "detectedKind", "detectedVersion", "observedSize", "observedEtag",
                        "observedSha256", "required", "dependencyIds",
                        "requiredExtensions", "usedExtensions"});
    ResourceEvidence result;
    result.object_id = required<std::string>(value, "objectId");
    result.package_relative_path = required<std::string>(value, "packageRelativePath");
    result.role = parseEnum(required<std::string>(value, "role"), kResourceRoles);
    result.media_type = optional<std::string>(value, "mediaType");
    result.detected_kind = parseEnum(required<std::string>(value, "detectedKind"),
                                     kDetectedKinds);
    result.detected_version = optional<std::string>(value, "detectedVersion");
    result.observed_size = required<std::uint64_t>(value, "observedSize");
    result.observed_etag = required<std::string>(value, "observedEtag");
    result.observed_sha256 = required<std::string>(value, "observedSha256");
    result.required = required<bool>(value, "required");
    result.dependency_ids = required<std::vector<std::string>>(value, "dependencyIds");
    result.required_extensions =
            required<std::vector<std::string>>(value, "requiredExtensions");
    result.used_extensions =
            required<std::vector<std::string>>(value, "usedExtensions");
    return result;
}

HierarchyRecord parseHierarchyRecord(const Json& value) {
    require(value.is_object(), "Hierarchy record must be an object");
    requireOnly(value, {"recordType", "recordId", "documentId", "resourceObjectId",
                        "parentDocumentId", "parentContentId", "parentContentOrdinal",
                        "documentDepth", "assetVersion", "documentSha256", "rootTileId",
                        "gltfUpAxis", "tileId", "parentTileId", "tileJsonPointer",
                        "tileLevel", "implicitX", "implicitY", "implicitZ", "transform",
                        "boundingVolumeType", "boundingVolume", "geometricError", "refine",
                        "contentCount", "hasChildren", "implicitRoot", "subtreeId",
                        "subdivisionScheme", "subtreeLevels", "availableLevels",
                        "availableTileCount", "availableContentCount",
                        "availableChildSubtreeCount", "contentStreamCount", "contentId",
                        "contentOrdinal", "contentSubdivisionScheme", "sourceUri",
                        "sourceResourceObjectId", "contentKind", "contentFormat",
                        "resourceClosureHash", "resourceClosureVersion", "groupId",
                        "contentBoundingVolumeType", "contentBoundingVolume",
                        "requiredExtensions", "usedExtensions"});
    HierarchyRecord result;
    result.record_type = parseEnum(required<std::string>(value, "recordType"),
                                   kHierarchyRecordTypes);
    result.record_id = required<std::string>(value, "recordId");
    result.document_id = optional<std::string>(value, "documentId");
    result.resource_object_id = optional<std::string>(value, "resourceObjectId");
    result.parent_document_id = optional<std::string>(value, "parentDocumentId");
    result.parent_content_id = optional<std::string>(value, "parentContentId");
    result.parent_content_ordinal = optional<std::uint32_t>(value, "parentContentOrdinal");
    result.document_depth = optional<std::uint32_t>(value, "documentDepth");
    result.asset_version = optional<std::string>(value, "assetVersion");
    result.document_sha256 = optional<std::string>(value, "documentSha256");
    result.root_tile_id = optional<std::string>(value, "rootTileId");
    result.gltf_up_axis = optional<std::string>(value, "gltfUpAxis");
    result.tile_id = optional<std::string>(value, "tileId");
    result.parent_tile_id = optional<std::string>(value, "parentTileId");
    result.tile_json_pointer = optional<std::string>(value, "tileJsonPointer");
    result.tile_level = optional<std::uint64_t>(value, "tileLevel");
    result.implicit_x = optional<std::uint64_t>(value, "implicitX");
    result.implicit_y = optional<std::uint64_t>(value, "implicitY");
    result.implicit_z = optional<std::uint64_t>(value, "implicitZ");
    result.transform = optional<std::vector<double>>(value, "transform");
    if (const auto item = optional<std::string>(value, "boundingVolumeType")) {
        result.bounding_volume_type = parseEnum(*item, kBoundingVolumeTypes);
    }
    result.bounding_volume = optional<std::vector<double>>(value, "boundingVolume");
    result.geometric_error = optional<double>(value, "geometricError");
    if (const auto item = optional<std::string>(value, "refine")) {
        result.refine = parseEnum(*item, kRefineModes);
    }
    result.content_count = optional<std::uint32_t>(value, "contentCount");
    result.has_children = optional<bool>(value, "hasChildren");
    result.implicit_root = optional<bool>(value, "implicitRoot");
    result.subtree_id = optional<std::string>(value, "subtreeId");
    if (const auto item = optional<std::string>(value, "subdivisionScheme")) {
        result.subdivision_scheme = parseEnum(*item, kSubdivisionSchemes);
    }
    result.subtree_levels = optional<std::uint32_t>(value, "subtreeLevels");
    result.available_levels = optional<std::uint64_t>(value, "availableLevels");
    result.available_tile_count = optional<std::uint64_t>(value, "availableTileCount");
    result.available_content_count = optional<std::uint64_t>(value, "availableContentCount");
    result.available_child_subtree_count =
            optional<std::uint64_t>(value, "availableChildSubtreeCount");
    result.content_stream_count = optional<std::uint32_t>(value, "contentStreamCount");
    result.content_id = optional<std::string>(value, "contentId");
    result.content_ordinal = optional<std::uint32_t>(value, "contentOrdinal");
    if (const auto item = optional<std::string>(value, "contentSubdivisionScheme")) {
        result.content_subdivision_scheme = parseEnum(*item, kSubdivisionSchemes);
    }
    result.source_uri = optional<std::string>(value, "sourceUri");
    result.source_resource_object_id = optional<std::string>(value, "sourceResourceObjectId");
    if (const auto item = optional<std::string>(value, "contentKind")) {
        result.content_kind = parseEnum(*item, kContentKinds);
    }
    if (const auto item = optional<std::string>(value, "contentFormat")) {
        result.content_format = parseEnum(*item, kContentFormats);
    }
    result.resource_closure_hash = optional<std::string>(value, "resourceClosureHash");
    result.resource_closure_version = optional<std::string>(value, "resourceClosureVersion");
    result.group_id = optional<std::string>(value, "groupId");
    if (const auto item = optional<std::string>(value, "contentBoundingVolumeType")) {
        result.content_bounding_volume_type = parseEnum(*item, kBoundingVolumeTypes);
    }
    result.content_bounding_volume =
            optional<std::vector<double>>(value, "contentBoundingVolume");
    result.required_extensions =
            required<std::vector<std::string>>(value, "requiredExtensions");
    result.used_extensions = required<std::vector<std::string>>(value, "usedExtensions");
    return result;
}

Json sourceObjectJson(const SourceObject& value) {
    Json result = {{"etag", value.etag},
                   {"objectId", value.object_id},
                   {"packageRelativePath", value.package_relative_path},
                   {"readGrant",
                    {{"expiresAt", value.read_grant.expires_at},
                     {"grantId", value.read_grant.grant_id},
                     {"httpMethod", enumName(value.read_grant.http_method, kHttpMethods)},
                     {"purpose", enumName(value.read_grant.purpose, kGrantPurposes)},
                     {"url", value.read_grant.url}}},
                   {"sizeBytes", value.size_bytes}};
    if (value.sha256.has_value()) {
        result["sha256"] = *value.sha256;
    }
    return result;
}

Json resourceEvidenceJson(const ResourceEvidence& value) {
    Json result = {{"dependencyIds", value.dependency_ids},
                   {"detectedKind", enumName(value.detected_kind, kDetectedKinds)},
                   {"objectId", value.object_id},
                   {"observedEtag", value.observed_etag},
                   {"observedSha256", value.observed_sha256},
                   {"observedSize", value.observed_size},
                   {"packageRelativePath", value.package_relative_path},
                    {"required", value.required},
                    {"requiredExtensions", value.required_extensions},
                    {"usedExtensions", value.used_extensions},
                    {"role", enumName(value.role, kResourceRoles)}};
    if (value.media_type.has_value()) {
        result["mediaType"] = *value.media_type;
    }
    if (value.detected_version.has_value()) {
        result["detectedVersion"] = *value.detected_version;
    }
    return result;
}

template <typename Value>
void putOptional(Json& output, const char* field,
                 const std::optional<Value>& value) {
    if (value.has_value()) output[field] = *value;
}

template <typename Enum, std::size_t Size>
void putOptionalEnum(Json& output, const char* field,
                     const std::optional<Enum>& value,
                     const std::array<EnumEntry<Enum>, Size>& entries) {
    if (value.has_value()) output[field] = enumName(*value, entries);
}

Json hierarchyRecordJson(const HierarchyRecord& value) {
    Json result = {{"recordType", enumName(value.record_type, kHierarchyRecordTypes)},
                   {"recordId", value.record_id},
                   {"requiredExtensions", value.required_extensions},
                   {"usedExtensions", value.used_extensions}};
    putOptional(result, "documentId", value.document_id);
    putOptional(result, "resourceObjectId", value.resource_object_id);
    putOptional(result, "parentDocumentId", value.parent_document_id);
    putOptional(result, "parentContentId", value.parent_content_id);
    putOptional(result, "parentContentOrdinal", value.parent_content_ordinal);
    putOptional(result, "documentDepth", value.document_depth);
    putOptional(result, "assetVersion", value.asset_version);
    putOptional(result, "documentSha256", value.document_sha256);
    putOptional(result, "rootTileId", value.root_tile_id);
    putOptional(result, "gltfUpAxis", value.gltf_up_axis);
    putOptional(result, "tileId", value.tile_id);
    putOptional(result, "parentTileId", value.parent_tile_id);
    putOptional(result, "tileJsonPointer", value.tile_json_pointer);
    putOptional(result, "tileLevel", value.tile_level);
    putOptional(result, "implicitX", value.implicit_x);
    putOptional(result, "implicitY", value.implicit_y);
    putOptional(result, "implicitZ", value.implicit_z);
    putOptional(result, "transform", value.transform);
    putOptionalEnum(result, "boundingVolumeType", value.bounding_volume_type,
                    kBoundingVolumeTypes);
    putOptional(result, "boundingVolume", value.bounding_volume);
    putOptional(result, "geometricError", value.geometric_error);
    putOptionalEnum(result, "refine", value.refine, kRefineModes);
    putOptional(result, "contentCount", value.content_count);
    putOptional(result, "hasChildren", value.has_children);
    putOptional(result, "implicitRoot", value.implicit_root);
    putOptional(result, "subtreeId", value.subtree_id);
    putOptionalEnum(result, "subdivisionScheme", value.subdivision_scheme,
                    kSubdivisionSchemes);
    putOptional(result, "subtreeLevels", value.subtree_levels);
    putOptional(result, "availableLevels", value.available_levels);
    putOptional(result, "availableTileCount", value.available_tile_count);
    putOptional(result, "availableContentCount", value.available_content_count);
    putOptional(result, "availableChildSubtreeCount",
                value.available_child_subtree_count);
    putOptional(result, "contentStreamCount", value.content_stream_count);
    putOptional(result, "contentId", value.content_id);
    putOptional(result, "contentOrdinal", value.content_ordinal);
    putOptionalEnum(result, "contentSubdivisionScheme",
                    value.content_subdivision_scheme, kSubdivisionSchemes);
    putOptional(result, "sourceUri", value.source_uri);
    putOptional(result, "sourceResourceObjectId", value.source_resource_object_id);
    putOptionalEnum(result, "contentKind", value.content_kind, kContentKinds);
    putOptionalEnum(result, "contentFormat", value.content_format, kContentFormats);
    putOptional(result, "resourceClosureHash", value.resource_closure_hash);
    putOptional(result, "resourceClosureVersion", value.resource_closure_version);
    putOptional(result, "groupId", value.group_id);
    putOptionalEnum(result, "contentBoundingVolumeType",
                    value.content_bounding_volume_type, kBoundingVolumeTypes);
    putOptional(result, "contentBoundingVolume", value.content_bounding_volume);
    return result;
}

Json diagnosticJson(const Diagnostic& value) {
    Json result = {{"code", enumName(value.code, kDiagnosticCodes)},
                   {"occurrenceCount", value.occurrence_count},
                   {"retryable", value.retryable},
                   {"safeMessage", value.safe_message},
                   {"severity", enumName(value.severity, kDiagnosticSeverities)},
                   {"stage", enumName(value.stage, kInspectorStages)}};
    if (value.object_id.has_value()) result["objectId"] = *value.object_id;
    if (value.package_relative_path.has_value()) {
        result["packageRelativePath"] = *value.package_relative_path;
    }
    if (value.json_pointer.has_value()) result["jsonPointer"] = *value.json_pointer;
    if (value.byte_offset.has_value()) result["byteOffset"] = *value.byte_offset;
    return result;
}

Json toolJson(const ToolVersion& value) {
    Json result = {{"name", enumName(value.name, kToolNames)},
                   {"version", value.version}};
    if (value.build_sha256.has_value()) result["buildSha256"] = *value.build_sha256;
    return result;
}

Json rootSelectionJson(const RootSelectionEvidence& value) {
    Json result = {{"method", enumName(value.method, kRootMethods)},
                   {"candidateCount", value.candidate_count}};
    if (value.selected_root_path.has_value()) {
        result["selectedRootPath"] = *value.selected_root_path;
    }
    return result;
}

Json resultDescriptorJson(const ResultManifestDescriptor& value) {
    return {{"manifestId", value.manifest_id},
            {"recordCount", value.record_count},
            {"pageCount", value.page_count},
            {"pageSize", value.page_size},
            {"manifestSha256", value.manifest_sha256}};
}

std::string sha256Hex(const std::string& value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0U;
    EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
    require(raw_context != nullptr, "Failed to allocate SHA-256 context");
    const auto free_context = [](EVP_MD_CTX* context) { EVP_MD_CTX_free(context); };
    std::unique_ptr<EVP_MD_CTX, decltype(free_context)> context(raw_context, free_context);
    require(EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1
                    && EVP_DigestUpdate(context.get(), value.data(), value.size()) == 1
                    && EVP_DigestFinal_ex(context.get(), digest.data(), &digest_length) == 1
                    && digest_length == kSha256Bytes,
            "SHA-256 calculation failed");
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(digest_length * 2U, '0');
    for (std::size_t index = 0; index < digest_length; ++index) {
        result[index * 2U] = kHex[digest[index] >> 4U];
        result[index * 2U + 1U] = kHex[digest[index] & 0x0FU];
    }
    return result;
}

bool hasVisibleText(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char current) {
        return std::isspace(current) == 0;
    });
}

void requireText(const std::string& value, std::size_t maximum, const char* message) {
    require(!value.empty() && value.size() <= maximum && hasVisibleText(value), message);
}

void requireIdentifier(const std::string& value) {
    requireText(value, ProtocolLimits::kMaximumIdentifierUtf8Bytes,
                "Inspector identifier is blank or too long");
}

void requireSha256(const std::string& value) {
    require(value.size() == 64U
                    && std::all_of(value.begin(), value.end(), [](char current) {
                           return (current >= '0' && current <= '9')
                                   || (current >= 'a' && current <= 'f');
                       }),
            "Inspector hash must be lowercase SHA-256");
}

void requirePackagePath(const std::string& value) {
    requireText(value, ProtocolLimits::kMaximumPackagePathUtf8Bytes,
                "Package-relative path is blank or too long");
    require(value.front() != '/' && value.find('\\') == std::string::npos
                    && value.find('?') == std::string::npos
                    && value.find('#') == std::string::npos
                    && value.find("://") == std::string::npos,
            "Package-relative path has a forbidden form");
    std::size_t offset = 0U;
    while (offset <= value.size()) {
        const auto separator = value.find('/', offset);
        const auto length = (separator == std::string::npos ? value.size() : separator) - offset;
        const auto segment = value.substr(offset, length);
        require(!segment.empty() && segment != "." && segment != "..",
                "Package-relative path is not normalized");
        if (separator == std::string::npos) break;
        offset = separator + 1U;
    }
}

bool isLeapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(int year, int month) {
    static constexpr std::array<int, 12U> kDays =
            {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && isLeapYear(year)) return 29;
    return kDays[static_cast<std::size_t>(month - 1)];
}

int parseDigits(const std::string& value, std::size_t offset, std::size_t count) {
    int result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char current = value[offset + index];
        require(current >= '0' && current <= '9', "UTC time contains a non-digit");
        result = result * 10 + (current - '0');
    }
    return result;
}

std::int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2U ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year =
            (153U * adjusted_month + 2U) / 5U
            + day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U
            - year_of_era / 100U + day_of_year;
    return static_cast<std::int64_t>(era) * 146097L
            + static_cast<std::int64_t>(day_of_era) - 719468L;
}

std::int64_t parseUtcEpochSeconds(const std::string& value) {
    require(value.size() == 20U && value[4] == '-' && value[7] == '-'
                    && value[10] == 'T' && value[13] == ':' && value[16] == ':'
                    && value[19] == 'Z',
            "UTC time must use second precision and Z");
    const int year = parseDigits(value, 0U, 4U);
    const int month = parseDigits(value, 5U, 2U);
    const int day = parseDigits(value, 8U, 2U);
    const int hour = parseDigits(value, 11U, 2U);
    const int minute = parseDigits(value, 14U, 2U);
    const int second = parseDigits(value, 17U, 2U);
    require(month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month)
                    && hour <= 23 && minute <= 59 && second <= 59,
            "UTC time is outside calendar bounds");
    return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day))
            * 86400L + hour * 3600L + minute * 60L + second;
}

void validateGrant(const AccessGrant& grant, GrantPurpose expected,
                   const std::string& now_utc) {
    requireIdentifier(grant.grant_id);
    require(grant.purpose == expected && grant.http_method == HttpMethod::get,
            "Access grant scope is invalid");
    requireText(grant.url, ProtocolLimits::kMaximumGrantUrlUtf8Bytes,
                "Access grant URL is blank or too long");
    const bool http = grant.url.rfind("http://", 0U) == 0U;
    const bool https = grant.url.rfind("https://", 0U) == 0U;
    require(http || https, "Access grant URL is invalid");
    const std::size_t authority_start = https ? 8U : 7U;
    const auto authority_end = grant.url.find_first_of("/?", authority_start);
    const auto authority = grant.url.substr(authority_start, authority_end - authority_start);
    require(!authority.empty() && authority.find('@') == std::string::npos
                    && grant.url.find('#') == std::string::npos,
            "Access grant URL is invalid");
    const auto now = parseUtcEpochSeconds(now_utc);
    const auto expiry = parseUtcEpochSeconds(grant.expires_at);
    const auto lifetime = expiry - now;
    require(lifetime >= ProtocolLimits::kMinimumGrantLifetimeSeconds,
            "Access grant is expired or too close to expiry");
    require(lifetime <= ProtocolLimits::kMaximumGrantLifetimeSeconds,
            "Access grant is not short-lived");
}

void validateTools(const std::vector<ToolVersion>& tools) {
    require(!tools.empty() && tools.size() <= ProtocolLimits::kMaximumToolRecords,
            "Tool version list is outside bounds");
    std::set<ToolName> names;
    for (const auto& tool : tools) {
        require(names.insert(tool.name).second, "Tool version list contains duplicates");
        requireIdentifier(tool.version);
        if (tool.build_sha256.has_value()) requireSha256(*tool.build_sha256);
    }
    require(names.find(ToolName::inspector) != names.end(),
            "Tool version list does not contain INSPECTOR");
}

void validateEnvelope(MessageType actual, MessageType expected, const std::string& protocol,
                      const std::string& inspection_id, const std::string& request_id) {
    require(actual == expected, "Inspector messageType is invalid");
    require(protocol == kProtocolVersion, "Inspector protocolVersion is unsupported");
    requireIdentifier(inspection_id);
    requireIdentifier(request_id);
}

std::uint64_t divideRoundUp(std::uint64_t value, std::uint32_t divisor) {
    return (value + divisor - 1U) / divisor;
}

struct PageDigest {
    std::uint32_t page_number;
    std::string page_sha256;
    std::uint32_t record_count;
};

std::string canonicalManifestSha256(std::vector<PageDigest> digests) {
    std::sort(digests.begin(), digests.end(), [](const auto& left, const auto& right) {
        return left.page_number < right.page_number;
    });
    std::ostringstream value;
    for (const auto& digest : digests) {
        value << digest.page_number << ':' << digest.page_sha256 << ':'
              << digest.record_count << '\n';
    }
    return sha256Hex(value.str());
}

void validateRootSelection(const RootSelectionEvidence& root) {
    require(root.candidate_count <= ProtocolLimits::kMaximumResourceCount,
            "Root candidate count is outside bounds");
    const bool selected = root.method == RootSelectionMethod::explicit_root
            || root.method == RootSelectionMethod::unique_top_level
            || root.method == RootSelectionMethod::unique_graph_root;
    if (selected) {
        require(root.selected_root_path.has_value() && root.candidate_count > 0U,
                "Selected root evidence is incomplete");
        requirePackagePath(*root.selected_root_path);
    } else {
        require(!root.selected_root_path.has_value(),
                "Unselected root contains a selected path");
    }
}

void validateExtensions(const std::vector<std::string>& extensions,
                        const char* message) {
    require(extensions.size()
                    <= ProtocolLimits::kMaximumRequiredExtensionsPerResource,
            message);
    std::string previous;
    for (const auto& extension : extensions) {
        requireText(extension, ProtocolLimits::kMaximumExtensionUtf8Bytes, message);
        require(previous.empty() || previous < extension, message);
        previous = extension;
    }
}

void validateResourceEvidence(const ResourceEvidence& evidence) {
    requireIdentifier(evidence.object_id);
    requirePackagePath(evidence.package_relative_path);
    require(evidence.observed_size <= ProtocolLimits::kMaximumExpandedResourceBytes,
            "Observed resource size is outside bounds");
    requireText(evidence.observed_etag, ProtocolLimits::kMaximumEtagUtf8Bytes,
                "Observed ETag is blank or too long");
    requireSha256(evidence.observed_sha256);
    require(evidence.dependency_ids.size()
                    <= ProtocolLimits::kMaximumDependenciesPerResource,
            "Resource dependency list is outside bounds");
    std::set<std::string> dependencies;
    for (const auto& dependency : evidence.dependency_ids) {
        requireIdentifier(dependency);
        require(dependencies.insert(dependency).second,
                "Resource dependency list contains duplicates");
    }
    validateExtensions(evidence.required_extensions,
                       "Required extension list is invalid");
    validateExtensions(evidence.used_extensions,
                       "Used extension list is invalid");
    require(std::includes(evidence.used_extensions.begin(), evidence.used_extensions.end(),
                          evidence.required_extensions.begin(),
                          evidence.required_extensions.end()),
            "Required extensions are not a subset of used extensions");
}

void validateBoundingVolume(BoundingVolumeType type,
                            const std::vector<double>& values,
                            const char* message) {
    std::size_t expected = 0U;
    switch (type) {
        case BoundingVolumeType::region: expected = 6U; break;
        case BoundingVolumeType::box: expected = 12U; break;
        case BoundingVolumeType::sphere: expected = 4U; break;
    }
    require(values.size() == expected
                    && std::all_of(values.begin(), values.end(),
                                   [](double value) { return std::isfinite(value); }),
            message);
}

void validateHierarchyRecord(const HierarchyRecord& record) {
    requireIdentifier(record.record_id);
    validateExtensions(record.required_extensions,
                       "Hierarchy required extension list is invalid");
    validateExtensions(record.used_extensions,
                       "Hierarchy used extension list is invalid");
    require(std::includes(record.used_extensions.begin(), record.used_extensions.end(),
                          record.required_extensions.begin(),
                          record.required_extensions.end()),
            "Hierarchy required extensions are not a subset of used extensions");
    switch (record.record_type) {
        case HierarchyRecordType::document:
            require(record.document_id == std::optional<std::string>(record.record_id)
                            && record.resource_object_id.has_value()
                            && record.document_depth.has_value()
                            && *record.document_depth <= ProtocolLimits::kMaximumTileDepth
                            && record.asset_version.has_value()
                            && record.document_sha256.has_value()
                            && record.root_tile_id.has_value(),
                    "Document hierarchy record is incomplete");
            require(record.parent_document_id.has_value()
                            == record.parent_content_id.has_value()
                            && record.parent_content_id.has_value()
                                    == record.parent_content_ordinal.has_value(),
                    "Document parent identity is inconsistent");
            requireSha256(*record.document_sha256);
            break;
        case HierarchyRecordType::explicit_tile:
        case HierarchyRecordType::implicit_tile:
            require(record.tile_id == std::optional<std::string>(record.record_id)
                            && record.document_id.has_value()
                            && record.tile_level.has_value()
                            && *record.tile_level <= ProtocolLimits::kMaximumTileDepth
                            && record.transform.has_value()
                            && record.transform->size() == 16U
                            && record.bounding_volume_type.has_value()
                            && record.bounding_volume.has_value()
                            && record.geometric_error.has_value()
                            && std::isfinite(*record.geometric_error)
                            && *record.geometric_error >= 0.0
                            && record.refine.has_value()
                            && record.content_count.has_value()
                            && *record.content_count
                                    <= ProtocolLimits::kMaximumContentsPerTile
                            && record.has_children.has_value()
                            && record.implicit_root.has_value(),
                    "Tile hierarchy record is incomplete");
            require(std::all_of(record.transform->begin(), record.transform->end(),
                                [](double value) { return std::isfinite(value); }),
                    "Tile transform contains a non-finite value");
            validateBoundingVolume(*record.bounding_volume_type,
                                   *record.bounding_volume,
                                   "Tile bounding volume is invalid");
            if (record.record_type == HierarchyRecordType::explicit_tile) {
                require(record.tile_json_pointer.has_value(),
                        "Explicit tile JSON pointer is missing");
            } else {
                require(record.implicit_x.has_value()
                                && record.implicit_y.has_value(),
                        "Implicit tile coordinates are incomplete");
            }
            break;
        case HierarchyRecordType::implicit_subtree:
            require(record.subtree_id == std::optional<std::string>(record.record_id)
                            && record.document_id.has_value()
                            && record.resource_object_id.has_value()
                            && record.tile_id.has_value()
                            && record.subdivision_scheme.has_value()
                            && record.subtree_levels.has_value()
                            && record.available_levels.has_value()
                            && record.available_tile_count.has_value()
                            && record.available_content_count.has_value()
                            && record.available_child_subtree_count.has_value()
                            && record.content_stream_count.has_value(),
                    "Implicit subtree record is incomplete");
            require(record.tile_level.has_value()
                            && record.implicit_x.has_value()
                            && record.implicit_y.has_value()
                            && (record.subdivision_scheme
                                        != std::optional<SubdivisionScheme>(
                                                SubdivisionScheme::octree)
                                || record.implicit_z.has_value()),
                    "Implicit subtree coordinates are incomplete");
            break;
        case HierarchyRecordType::content:
            require(record.content_id == std::optional<std::string>(record.record_id)
                            && record.document_id.has_value()
                            && record.tile_id.has_value()
                            && record.content_ordinal.has_value()
                            && *record.content_ordinal
                                    < ProtocolLimits::kMaximumContentsPerTile
                            && record.source_uri.has_value()
                            && record.source_resource_object_id.has_value()
                            && record.content_kind.has_value()
                            && record.content_format.has_value()
                            && record.resource_closure_hash.has_value()
                            && record.resource_closure_version
                                    == std::optional<std::string>(
                                            kContentResourceClosureVersion),
                    "Content hierarchy record is incomplete");
            require(record.tile_json_pointer.has_value()
                            || (record.content_subdivision_scheme.has_value()
                                && record.tile_level.has_value()
                                && record.implicit_x.has_value()
                                && record.implicit_y.has_value()),
                    "Content location is incomplete");
            require(record.content_bounding_volume_type.has_value()
                            == record.content_bounding_volume.has_value(),
                    "Content bounding volume is incomplete");
            if (record.content_bounding_volume_type.has_value()) {
                validateBoundingVolume(*record.content_bounding_volume_type,
                                       *record.content_bounding_volume,
                                       "Content bounding volume is invalid");
            }
            requireSha256(*record.resource_closure_hash);
            break;
    }
}

void validateDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    require(diagnostics.size() <= ProtocolLimits::kMaximumDiagnostics,
            "Diagnostic count is outside bounds");
    Json canonical = Json::array();
    for (const auto& diagnostic : diagnostics) {
        requireText(diagnostic.safe_message,
                    ProtocolLimits::kMaximumDiagnosticMessageUtf8Bytes,
                    "Diagnostic safeMessage is blank or too long");
        std::string normalized = diagnostic.safe_message;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char current) { return static_cast<char>(std::tolower(current)); });
        for (const auto marker : kSensitiveMarkers) {
            require(normalized.find(marker) == std::string::npos,
                    "Diagnostic safeMessage contains sensitive data");
        }
        require(diagnostic.occurrence_count > 0U,
                "Diagnostic occurrenceCount must be positive");
        if (diagnostic.object_id.has_value()) requireIdentifier(*diagnostic.object_id);
        if (diagnostic.package_relative_path.has_value()) {
            requirePackagePath(*diagnostic.package_relative_path);
        }
        if (diagnostic.json_pointer.has_value()) {
            require(diagnostic.json_pointer->size()
                            <= ProtocolLimits::kMaximumJsonPointerUtf8Bytes
                            && (diagnostic.json_pointer->empty()
                                || diagnostic.json_pointer->front() == '/'),
                    "Diagnostic JSON pointer is invalid");
        }
        canonical.push_back(diagnosticJson(diagnostic));
    }
    require(canonical.dump().size() <= ProtocolLimits::kMaximumDiagnosticJsonBytes,
            "Diagnostic JSON is outside bounds");
}

}  // namespace

InspectionRequest parseInspectionRequest(const std::string& text) {
    const auto json = parseObject(text);
    requireOnly(json, {"messageType", "protocolVersion", "inspectionId", "requestId",
                       "sourceGeneration", "inspectorVersion", "resourceProfileVersion",
                       "resourceProfileSha256", "deadlineTime", "packageKind",
                       "selectedRootHint", "requiredTools", "sourceManifest"});
    InspectionRequest result;
    result.message_type = parseEnum(required<std::string>(json, "messageType"), kMessageTypes);
    result.protocol_version = required<std::string>(json, "protocolVersion");
    result.inspection_id = required<std::string>(json, "inspectionId");
    result.request_id = required<std::string>(json, "requestId");
    result.source_generation = required<std::string>(json, "sourceGeneration");
    result.inspector_version = required<std::string>(json, "inspectorVersion");
    result.resource_profile_version = required<std::string>(json, "resourceProfileVersion");
    result.resource_profile_sha256 = required<std::string>(json, "resourceProfileSha256");
    result.deadline_time = required<std::string>(json, "deadlineTime");
    result.package_kind = parseEnum(required<std::string>(json, "packageKind"), kPackageKinds);
    result.selected_root_hint = optional<std::string>(json, "selectedRootHint");
    result.required_tools = parseTools(json, "requiredTools");
    result.source_manifest = parseManifestDescriptor(required<Json>(json, "sourceManifest"));
    return result;
}

SourceManifestPage parseSourceManifestPage(const std::string& text) {
    const auto json = parseObject(text);
    requireOnly(json, {"messageType", "protocolVersion", "inspectionId", "requestId",
                       "manifestId", "pageNumber", "recordCount", "pageSha256", "records"});
    SourceManifestPage result;
    result.message_type = parseEnum(required<std::string>(json, "messageType"), kMessageTypes);
    result.protocol_version = required<std::string>(json, "protocolVersion");
    result.inspection_id = required<std::string>(json, "inspectionId");
    result.request_id = required<std::string>(json, "requestId");
    result.manifest_id = required<std::string>(json, "manifestId");
    result.page_number = required<std::uint32_t>(json, "pageNumber");
    result.record_count = required<std::uint32_t>(json, "recordCount");
    result.page_sha256 = required<std::string>(json, "pageSha256");
    const auto records = required<Json>(json, "records");
    require(records.is_array(), "Source manifest records must be an array");
    result.records.reserve(records.size());
    for (const auto& record : records) result.records.push_back(parseSourceObject(record));
    return result;
}

InspectionResult parseInspectionResult(const std::string& text) {
    const auto json = parseObject(text);
    requireOnly(json, {"messageType", "protocolVersion", "inspectionId", "requestId",
                       "sourceGeneration", "inspectorVersion", "outcome", "toolVersions",
                       "sourceClosureHash", "rootSelection", "totalResources",
                       "totalDocuments", "totalTiles", "totalSubtrees", "totalContents",
                       "resultManifest", "hierarchyManifest", "requiredExtensions",
                       "usedExtensions", "diagnostics"});
    InspectionResult result;
    result.message_type = parseEnum(required<std::string>(json, "messageType"), kMessageTypes);
    result.protocol_version = required<std::string>(json, "protocolVersion");
    result.inspection_id = required<std::string>(json, "inspectionId");
    result.request_id = required<std::string>(json, "requestId");
    result.source_generation = required<std::string>(json, "sourceGeneration");
    result.inspector_version = required<std::string>(json, "inspectorVersion");
    result.outcome = parseEnum(required<std::string>(json, "outcome"), kOutcomes);
    result.tool_versions = parseTools(json, "toolVersions");
    result.source_closure_hash = optional<std::string>(json, "sourceClosureHash");
    result.root_selection = parseRootSelection(required<Json>(json, "rootSelection"));
    result.total_resources = required<std::uint64_t>(json, "totalResources");
    result.total_documents = required<std::uint64_t>(json, "totalDocuments");
    result.total_tiles = required<std::uint64_t>(json, "totalTiles");
    result.total_subtrees = required<std::uint64_t>(json, "totalSubtrees");
    result.total_contents = required<std::uint64_t>(json, "totalContents");
    result.result_manifest = parseResultDescriptor(required<Json>(json, "resultManifest"));
    result.hierarchy_manifest = parseResultDescriptor(
            required<Json>(json, "hierarchyManifest"));
    result.required_extensions =
            required<std::vector<std::string>>(json, "requiredExtensions");
    result.used_extensions =
            required<std::vector<std::string>>(json, "usedExtensions");
    const auto diagnostics = required<Json>(json, "diagnostics");
    require(diagnostics.is_array(), "Diagnostics must be an array");
    result.diagnostics.reserve(diagnostics.size());
    for (const auto& item : diagnostics) result.diagnostics.push_back(parseDiagnostic(item));
    return result;
}

ResultPage parseResultPage(const std::string& text) {
    const auto json = parseObject(text);
    requireOnly(json, {"messageType", "protocolVersion", "inspectionId", "requestId",
                       "manifestId", "pageNumber", "recordCount", "pageSha256", "records"});
    ResultPage result;
    result.message_type = parseEnum(required<std::string>(json, "messageType"), kMessageTypes);
    result.protocol_version = required<std::string>(json, "protocolVersion");
    result.inspection_id = required<std::string>(json, "inspectionId");
    result.request_id = required<std::string>(json, "requestId");
    result.manifest_id = required<std::string>(json, "manifestId");
    result.page_number = required<std::uint32_t>(json, "pageNumber");
    result.record_count = required<std::uint32_t>(json, "recordCount");
    result.page_sha256 = required<std::string>(json, "pageSha256");
    const auto records = required<Json>(json, "records");
    require(records.is_array(), "Result records must be an array");
    result.records.reserve(records.size());
    for (const auto& record : records) result.records.push_back(parseResourceEvidence(record));
    return result;
}

HierarchyPage parseHierarchyPage(const std::string& text) {
    const auto json = parseObject(text);
    requireOnly(json, {"messageType", "protocolVersion", "inspectionId", "requestId",
                       "manifestId", "pageNumber", "recordCount", "pageSha256", "records"});
    HierarchyPage result;
    result.message_type = parseEnum(required<std::string>(json, "messageType"), kMessageTypes);
    result.protocol_version = required<std::string>(json, "protocolVersion");
    result.inspection_id = required<std::string>(json, "inspectionId");
    result.request_id = required<std::string>(json, "requestId");
    result.manifest_id = required<std::string>(json, "manifestId");
    result.page_number = required<std::uint32_t>(json, "pageNumber");
    result.record_count = required<std::uint32_t>(json, "recordCount");
    result.page_sha256 = required<std::string>(json, "pageSha256");
    const auto records = required<Json>(json, "records");
    require(records.is_array(), "Hierarchy records must be an array");
    result.records.reserve(records.size());
    for (const auto& record : records) {
        result.records.push_back(parseHierarchyRecord(record));
    }
    return result;
}

std::string serializeInspectionResult(const InspectionResult& result) {
    Json tools = Json::array();
    for (const auto& tool : result.tool_versions) tools.push_back(toolJson(tool));
    Json diagnostics = Json::array();
    for (const auto& diagnostic : result.diagnostics) {
        diagnostics.push_back(diagnosticJson(diagnostic));
    }
    Json json = {{"messageType", enumName(result.message_type, kMessageTypes)},
                 {"protocolVersion", result.protocol_version},
                 {"inspectionId", result.inspection_id},
                 {"requestId", result.request_id},
                 {"sourceGeneration", result.source_generation},
                 {"inspectorVersion", result.inspector_version},
                 {"outcome", enumName(result.outcome, kOutcomes)},
                 {"toolVersions", std::move(tools)},
                 {"rootSelection", rootSelectionJson(result.root_selection)},
                 {"totalResources", result.total_resources},
                 {"totalDocuments", result.total_documents},
                 {"totalTiles", result.total_tiles},
                 {"totalSubtrees", result.total_subtrees},
                 {"totalContents", result.total_contents},
                 {"resultManifest", resultDescriptorJson(result.result_manifest)},
                 {"hierarchyManifest", resultDescriptorJson(result.hierarchy_manifest)},
                 {"requiredExtensions", result.required_extensions},
                 {"usedExtensions", result.used_extensions},
                 {"diagnostics", std::move(diagnostics)}};
    if (result.source_closure_hash.has_value()) {
        json["sourceClosureHash"] = *result.source_closure_hash;
    }
    return json.dump();
}

std::string serializeResultPage(const ResultPage& page) {
    Json records = Json::array();
    for (const auto& record : page.records) records.push_back(resourceEvidenceJson(record));
    return Json({{"messageType", enumName(page.message_type, kMessageTypes)},
                 {"protocolVersion", page.protocol_version},
                 {"inspectionId", page.inspection_id},
                 {"requestId", page.request_id},
                 {"manifestId", page.manifest_id},
                 {"pageNumber", page.page_number},
                 {"recordCount", page.record_count},
                 {"pageSha256", page.page_sha256},
                 {"records", std::move(records)}})
            .dump();
}

std::string serializeHierarchyPage(const HierarchyPage& page) {
    Json records = Json::array();
    for (const auto& record : page.records) records.push_back(hierarchyRecordJson(record));
    return Json({{"messageType", enumName(page.message_type, kMessageTypes)},
                 {"protocolVersion", page.protocol_version},
                 {"inspectionId", page.inspection_id},
                 {"requestId", page.request_id},
                 {"manifestId", page.manifest_id},
                 {"pageNumber", page.page_number},
                 {"recordCount", page.record_count},
                 {"pageSha256", page.page_sha256},
                 {"records", std::move(records)}})
            .dump();
}

void validateRequest(const InspectionRequest& request, const std::string& now_utc) {
    validateEnvelope(request.message_type, MessageType::inspection_request,
                     request.protocol_version, request.inspection_id, request.request_id);
    requireIdentifier(request.source_generation);
    requireIdentifier(request.inspector_version);
    requireIdentifier(request.resource_profile_version);
    requireSha256(request.resource_profile_sha256);
    if (request.selected_root_hint.has_value()) requirePackagePath(*request.selected_root_hint);
    validateTools(request.required_tools);
    const auto now = parseUtcEpochSeconds(now_utc);
    const auto deadline = parseUtcEpochSeconds(request.deadline_time);
    require(deadline > now && deadline - now <= ProtocolLimits::kMaximumInspectionDeadlineSeconds,
            "Inspection deadline is outside bounds");
    const auto& descriptor = request.source_manifest;
    requireIdentifier(descriptor.manifest_id);
    require(descriptor.object_count > 0U
                    && descriptor.object_count <= ProtocolLimits::kMaximumResourceCount,
            "Source manifest objectCount is outside bounds");
    require(descriptor.page_size > 0U
                    && descriptor.page_size <= ProtocolLimits::kMaximumManifestPageRecords,
            "Source manifest pageSize is outside bounds");
    require(descriptor.page_count == divideRoundUp(descriptor.object_count,
                                                   descriptor.page_size),
            "Source manifest pageCount is inconsistent");
    requireSha256(descriptor.manifest_sha256);
    validateGrant(descriptor.page_grant, GrantPurpose::read_manifest_page, now_utc);
}

void validateSourceManifestPage(const SourceManifestPage& page,
                                const InspectionRequest& request,
                                const std::string& now_utc) {
    validateRequest(request, now_utc);
    validateEnvelope(page.message_type, MessageType::source_manifest_page,
                     page.protocol_version, page.inspection_id, page.request_id);
    require(page.inspection_id == request.inspection_id && page.request_id == request.request_id
                    && page.manifest_id == request.source_manifest.manifest_id,
            "Source manifest page identity differs from request");
    require(page.page_number < request.source_manifest.page_count,
            "Source manifest pageNumber is outside bounds");
    require(!page.records.empty()
                    && page.records.size() <= ProtocolLimits::kMaximumManifestPageRecords
                    && page.record_count == page.records.size(),
            "Source manifest page records are outside bounds");
    std::set<std::string> object_ids;
    std::set<std::string> paths;
    for (const auto& source : page.records) {
        requireIdentifier(source.object_id);
        requirePackagePath(source.package_relative_path);
        require(source.size_bytes <= ProtocolLimits::kMaximumSourceObjectBytes,
                "Source object size is outside bounds");
        requireText(source.etag, ProtocolLimits::kMaximumEtagUtf8Bytes,
                    "Source object ETag is blank or too long");
        if (source.sha256.has_value()) requireSha256(*source.sha256);
        validateGrant(source.read_grant, GrantPurpose::read_source_object, now_utc);
        require(object_ids.insert(source.object_id).second,
                "Source manifest page contains duplicate objectId");
        require(paths.insert(source.package_relative_path).second,
                "Source manifest page contains duplicate path");
    }
    requireSha256(page.page_sha256);
    require(canonicalPageSha256(page.records) == page.page_sha256,
            "Source manifest page hash mismatch");
}

void validateSourceManifest(const InspectionRequest& request,
                            const std::vector<SourceManifestPage>& pages,
                            const std::string& now_utc) {
    validateRequest(request, now_utc);
    require(pages.size() == request.source_manifest.page_count,
            "Source manifest page count is incomplete");
    std::set<std::uint32_t> page_numbers;
    std::uint64_t record_count = 0U;
    std::vector<PageDigest> digests;
    for (const auto& page : pages) {
        validateSourceManifestPage(page, request, now_utc);
        require(page_numbers.insert(page.page_number).second,
                "Source manifest contains a duplicate page");
        record_count += page.record_count;
        digests.push_back({page.page_number, page.page_sha256, page.record_count});
    }
    for (std::uint32_t page = 0U; page < request.source_manifest.page_count; ++page) {
        require(page_numbers.find(page) != page_numbers.end(),
                "Source manifest contains a missing page");
    }
    require(record_count == request.source_manifest.object_count,
            "Source manifest objectCount differs from pages");
    require(canonicalManifestSha256(digests) == request.source_manifest.manifest_sha256,
            "Source manifest hash mismatch");
}

void validateResult(const InspectionResult& result, const InspectionRequest& request) {
    validateEnvelope(result.message_type, MessageType::inspection_result,
                     result.protocol_version, result.inspection_id, result.request_id);
    require(result.inspection_id == request.inspection_id && result.request_id == request.request_id
                    && result.source_generation == request.source_generation
                    && result.inspector_version == request.inspector_version,
            "Inspection result identity differs from request");
    validateTools(result.tool_versions);
    for (const auto& required_tool : request.required_tools) {
        require(std::any_of(result.tool_versions.begin(), result.tool_versions.end(),
                            [&required_tool](const ToolVersion& actual_tool) {
                                return required_tool.name == actual_tool.name
                                        && required_tool.version == actual_tool.version
                                        && required_tool.build_sha256
                                                == actual_tool.build_sha256;
                            }),
                "Inspection result tool versions differ from request");
    }
    validateRootSelection(result.root_selection);
    require(result.total_resources <= ProtocolLimits::kMaximumResourceCount
                    && result.total_documents <= ProtocolLimits::kMaximumDocumentCount
                    && result.total_tiles <= ProtocolLimits::kMaximumAvailableTileCount
                    && result.total_subtrees <= ProtocolLimits::kMaximumSubtreeCount
                    && result.total_contents
                            <= ProtocolLimits::kMaximumHierarchyRecordCount,
            "Inspection result counts are outside bounds");
    const auto& descriptor = result.result_manifest;
    requireIdentifier(descriptor.manifest_id);
    require(descriptor.record_count == result.total_resources
                    && descriptor.record_count <= ProtocolLimits::kMaximumResourceCount,
            "Result manifest recordCount is inconsistent");
    require(descriptor.page_size > 0U
                    && descriptor.page_size <= ProtocolLimits::kMaximumResultPageRecords,
            "Result manifest pageSize is outside bounds");
    const auto expected_pages = descriptor.record_count == 0U ? 0U
            : divideRoundUp(descriptor.record_count, descriptor.page_size);
    require(descriptor.page_count == expected_pages,
            "Result manifest pageCount is inconsistent");
    requireSha256(descriptor.manifest_sha256);
    const auto& hierarchy = result.hierarchy_manifest;
    const std::uint64_t hierarchy_count = result.total_documents
            + result.total_tiles + result.total_subtrees + result.total_contents;
    requireIdentifier(hierarchy.manifest_id);
    require(hierarchy.record_count == hierarchy_count
                    && hierarchy.record_count
                            <= ProtocolLimits::kMaximumHierarchyRecordCount,
            "Hierarchy manifest recordCount is inconsistent");
    require(hierarchy.page_size > 0U
                    && hierarchy.page_size
                            <= ProtocolLimits::kMaximumHierarchyPageRecords,
            "Hierarchy manifest pageSize is outside bounds");
    const auto expected_hierarchy_pages = hierarchy.record_count == 0U ? 0U
            : divideRoundUp(hierarchy.record_count, hierarchy.page_size);
    require(hierarchy.page_count == expected_hierarchy_pages,
            "Hierarchy manifest pageCount is inconsistent");
    requireSha256(hierarchy.manifest_sha256);
    validateExtensions(result.required_extensions,
                       "Dataset required extension list is invalid");
    validateExtensions(result.used_extensions,
                       "Dataset used extension list is invalid");
    require(std::includes(result.used_extensions.begin(), result.used_extensions.end(),
                          result.required_extensions.begin(),
                          result.required_extensions.end()),
            "Dataset required extensions are not a subset of used extensions");
    if (result.outcome == Outcome::succeeded) {
        require(result.source_closure_hash.has_value(),
                "Successful result has no source closure hash");
        requireSha256(*result.source_closure_hash);
        require(result.root_selection.method != RootSelectionMethod::none
                        && result.root_selection.method != RootSelectionMethod::ambiguous,
                "Successful result has no selected root");
    } else {
        require(!result.source_closure_hash.has_value(),
                "Non-success result contains a source closure hash");
        require(!result.diagnostics.empty(), "Non-success result has no diagnostic");
    }
    validateDiagnostics(result.diagnostics);
}

void validateResultPages(const InspectionResult& result,
                         const std::vector<ResultPage>& pages) {
    const auto& descriptor = result.result_manifest;
    if (descriptor.page_count == 0U) {
        require(pages.empty() && descriptor.manifest_sha256 == kEmptySha256,
                "Empty result manifest is inconsistent");
        return;
    }
    require(pages.size() == descriptor.page_count,
            "Result manifest page count is incomplete");
    std::set<std::uint32_t> page_numbers;
    std::set<std::string> object_ids;
    std::uint64_t record_count = 0U;
    std::vector<PageDigest> digests;
    for (const auto& page : pages) {
        validateEnvelope(page.message_type, MessageType::result_page, page.protocol_version,
                         page.inspection_id, page.request_id);
        require(page.inspection_id == result.inspection_id && page.request_id == result.request_id
                        && page.manifest_id == descriptor.manifest_id,
                "Result page identity differs from result");
        require(page.page_number < descriptor.page_count && !page.records.empty()
                        && page.records.size() <= ProtocolLimits::kMaximumResultPageRecords
                        && page.record_count == page.records.size(),
                "Result page records are outside bounds");
        require(page_numbers.insert(page.page_number).second,
                "Result manifest contains a duplicate page");
        for (const auto& evidence : page.records) {
            validateResourceEvidence(evidence);
            require(object_ids.insert(evidence.object_id).second,
                    "Result manifest contains duplicate objectId");
        }
        requireSha256(page.page_sha256);
        require(canonicalPageSha256(page.records) == page.page_sha256,
                "Result page hash mismatch");
        record_count += page.record_count;
        digests.push_back({page.page_number, page.page_sha256, page.record_count});
    }
    for (std::uint32_t page = 0U; page < descriptor.page_count; ++page) {
        require(page_numbers.find(page) != page_numbers.end(),
                "Result manifest contains a missing page");
    }
    require(record_count == descriptor.record_count,
            "Result manifest recordCount differs from pages");
    require(canonicalManifestSha256(digests) == descriptor.manifest_sha256,
            "Result manifest hash mismatch");
}

void validateHierarchyPages(const InspectionResult& result,
                            const std::vector<HierarchyPage>& pages) {
    const auto& descriptor = result.hierarchy_manifest;
    if (descriptor.page_count == 0U) {
        require(pages.empty() && descriptor.manifest_sha256 == kEmptySha256,
                "Empty hierarchy manifest is inconsistent");
        return;
    }
    require(pages.size() == descriptor.page_count,
            "Hierarchy manifest page count is incomplete");
    std::set<std::uint32_t> page_numbers;
    std::set<std::string> record_ids;
    std::uint64_t record_count = 0U;
    std::uint64_t documents = 0U;
    std::uint64_t tiles = 0U;
    std::uint64_t subtrees = 0U;
    std::uint64_t contents = 0U;
    std::set<std::string> required_extensions;
    std::set<std::string> used_extensions;
    std::vector<PageDigest> digests;
    for (const auto& page : pages) {
        validateEnvelope(page.message_type, MessageType::hierarchy_result_page,
                         page.protocol_version, page.inspection_id, page.request_id);
        require(page.inspection_id == result.inspection_id
                        && page.request_id == result.request_id
                        && page.manifest_id == descriptor.manifest_id,
                "Hierarchy page identity differs from result");
        require(page.page_number < descriptor.page_count && !page.records.empty()
                        && page.records.size()
                                <= ProtocolLimits::kMaximumHierarchyPageRecords
                        && page.record_count == page.records.size(),
                "Hierarchy page records are outside bounds");
        require(page_numbers.insert(page.page_number).second,
                "Hierarchy manifest contains a duplicate page");
        for (const auto& record : page.records) {
            validateHierarchyRecord(record);
            required_extensions.insert(record.required_extensions.begin(),
                                       record.required_extensions.end());
            used_extensions.insert(record.used_extensions.begin(),
                                   record.used_extensions.end());
            require(record_ids.insert(record.record_id).second,
                    "Hierarchy manifest contains duplicate recordId");
            switch (record.record_type) {
                case HierarchyRecordType::document: ++documents; break;
                case HierarchyRecordType::explicit_tile:
                case HierarchyRecordType::implicit_tile: ++tiles; break;
                case HierarchyRecordType::implicit_subtree: ++subtrees; break;
                case HierarchyRecordType::content: ++contents; break;
            }
        }
        requireSha256(page.page_sha256);
        require(canonicalPageSha256(page.records) == page.page_sha256,
                "Hierarchy page hash mismatch");
        record_count += page.record_count;
        digests.push_back({page.page_number, page.page_sha256, page.record_count});
    }
    for (std::uint32_t page = 0U; page < descriptor.page_count; ++page) {
        require(page_numbers.find(page) != page_numbers.end(),
                "Hierarchy manifest contains a missing page");
    }
    require(record_count == descriptor.record_count
                    && documents == result.total_documents
                    && tiles == result.total_tiles
                    && subtrees == result.total_subtrees
                    && contents == result.total_contents,
            "Hierarchy manifest counts differ from result totals");
    require(std::vector<std::string>(required_extensions.begin(),
                                     required_extensions.end())
                            == result.required_extensions
                    && std::vector<std::string>(used_extensions.begin(),
                                                used_extensions.end())
                            == result.used_extensions,
            "Hierarchy extension evidence differs from result totals");
    require(canonicalManifestSha256(digests) == descriptor.manifest_sha256,
            "Hierarchy manifest hash mismatch");
}

void validateCompletedExchange(const InspectionRequest& request,
                               const std::vector<SourceManifestPage>& source_pages,
                               const InspectionResult& result,
                               const std::vector<ResultPage>& result_pages,
                               const std::vector<HierarchyPage>& hierarchy_pages,
                               const std::string& now_utc) {
    validateSourceManifest(request, source_pages, now_utc);
    validateResult(result, request);
    validateResultPages(result, result_pages);
    validateHierarchyPages(result, hierarchy_pages);
    std::map<std::string, const ResourceEvidence*> evidence_by_id;
    for (const auto& page : result_pages) {
        for (const auto& evidence : page.records) evidence_by_id[evidence.object_id] = &evidence;
    }
    for (const auto& page : source_pages) {
        for (const auto& source : page.records) {
            const auto evidence = evidence_by_id.find(source.object_id);
            require(evidence != evidence_by_id.end(),
                    "Approved source object is absent from result evidence");
            require(source.package_relative_path == evidence->second->package_relative_path
                            && source.size_bytes == evidence->second->observed_size
                            && source.etag == evidence->second->observed_etag,
                    "Source object identity changed during inspection");
            if (source.sha256.has_value()) {
                require(*source.sha256 == evidence->second->observed_sha256,
                        "Source object hash changed during inspection");
            }
        }
    }
}

std::string canonicalPageSha256(const std::vector<SourceObject>& records) {
    Json canonical = Json::array();
    for (const auto& record : records) canonical.push_back(sourceObjectJson(record));
    const std::string bytes = canonical.dump();
    require(bytes.size() <= ProtocolLimits::kMaximumPageJsonBytes,
            "Canonical source page JSON is outside bounds");
    return sha256Hex(bytes);
}

std::string canonicalPageSha256(const std::vector<ResourceEvidence>& records) {
    Json canonical = Json::array();
    for (const auto& record : records) canonical.push_back(resourceEvidenceJson(record));
    const std::string bytes = canonical.dump();
    require(bytes.size() <= ProtocolLimits::kMaximumPageJsonBytes,
            "Canonical result page JSON is outside bounds");
    return sha256Hex(bytes);
}

std::string canonicalPageSha256(const std::vector<HierarchyRecord>& records) {
    Json canonical = Json::array();
    for (const auto& record : records) canonical.push_back(hierarchyRecordJson(record));
    const std::string bytes = canonical.dump();
    require(bytes.size() <= ProtocolLimits::kMaximumPageJsonBytes,
            "Canonical hierarchy page JSON is outside bounds");
    return sha256Hex(bytes);
}

std::string canonicalResultManifestSha256(
        const std::vector<ResultPage>& pages) {
    std::vector<PageDigest> digests;
    digests.reserve(pages.size());
    for (const auto& page : pages) {
        digests.push_back(
                {page.page_number, page.page_sha256, page.record_count});
    }
    return canonicalManifestSha256(std::move(digests));
}

std::string canonicalHierarchyManifestSha256(
        const std::vector<HierarchyPage>& pages) {
    std::vector<PageDigest> digests;
    digests.reserve(pages.size());
    for (const auto& page : pages) {
        digests.push_back(
                {page.page_number, page.page_sha256, page.record_count});
    }
    return canonicalManifestSha256(std::move(digests));
}

std::int64_t utcEpochSeconds(const std::string& utc_seconds) {
    return parseUtcEpochSeconds(utc_seconds);
}

}  // namespace clip_worker::inspection
