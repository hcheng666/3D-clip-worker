#include "clip_worker/normalization/normalization_contract.hpp"

#include "clip_worker/inspection/inspection_contract.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

constexpr const char* kManifestHashDomain =
        "THREE_D_NORMALIZATION_MANIFEST_HASH_V1";
constexpr std::size_t kSha256Bytes = 32U;
constexpr std::array<std::string_view, 16U> kSensitiveMarkers = {
        "http://", "https://", "file://", "x-amz-", "signature=", "token=",
        "access_key", "secret_key", "session_token", "authorization:", "bucket",
        "object key", "object-key", "object_key", ":\\", "/tmp/"};

template <typename Enum>
using EnumEntry = std::pair<std::string_view, Enum>;

constexpr std::array<EnumEntry<CanonicalFamily>, 3U> kCanonicalFamilies = {{
        {"MESH_GLTF2", CanonicalFamily::mesh_gltf2},
        {"POINT_GLTF2", CanonicalFamily::point_gltf2},
        {"INSTANCE_GLTF2", CanonicalFamily::instance_gltf2}}};
constexpr std::array<EnumEntry<FeatureIdentityModel>, 5U> kIdentityModels = {{
        {"NONE", FeatureIdentityModel::none},
        {"LEGACY_BATCH_TABLE_MAPPED",
         FeatureIdentityModel::legacy_batch_table_mapped},
        {"ATTRIBUTE_FEATURE_ID_PROPERTY_TABLE",
         FeatureIdentityModel::attribute_feature_id_property_table},
        {"POINT_FEATURE_ID", FeatureIdentityModel::point_feature_id},
        {"INSTANCE_FEATURE_ID", FeatureIdentityModel::instance_feature_id}}};
constexpr std::array<EnumEntry<TaskPhase>, 9U> kTaskPhases = {{
        {"CLAIMED", TaskPhase::claimed},
        {"MANIFEST_FETCH", TaskPhase::manifest_fetch},
        {"RESOURCE_DOWNLOAD", TaskPhase::resource_download},
        {"DECODE", TaskPhase::decode},
        {"NORMALIZE", TaskPhase::normalize},
        {"VALIDATE", TaskPhase::validate},
        {"UPLOAD_PREPARE", TaskPhase::upload_prepare},
        {"UPLOAD", TaskPhase::upload},
        {"COMPLETE", TaskPhase::complete}}};
constexpr std::array<std::string_view, 6U> kDecoders = {
        "DRACO", "JPEG", "KTX2", "MESHOPT", "PNG", "WEBP"};

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

Json parseObject(const std::string& text) {
    try {
        Json value = Json::parse(text);
        require(value.is_object(), "Normalizer message must be an object");
        return value;
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Normalizer JSON is invalid");
    }
}

void requireOnly(const Json& value, std::initializer_list<const char*> allowed) {
    std::set<std::string> names;
    for (const char* name : allowed) names.emplace(name);
    for (const auto& item : value.items()) {
        require(names.find(item.key()) != names.end(),
                "Normalizer message contains an unknown field");
    }
}

template <typename Value>
Value required(const Json& value, const char* field) {
    const auto item = value.find(field);
    require(item != value.end() && !item->is_null(),
            "Normalizer message is missing a field");
    try {
        return item->get<Value>();
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Normalizer field has an invalid type");
    }
}

template <typename Enum, std::size_t Size>
Enum parseEnum(const std::string& value,
               const std::array<EnumEntry<Enum>, Size>& entries) {
    for (const auto& entry : entries) {
        if (entry.first == value) return entry.second;
    }
    throw std::invalid_argument("Normalizer enum value is invalid");
}

template <typename Enum, std::size_t Size>
std::string enumName(Enum value,
                     const std::array<EnumEntry<Enum>, Size>& entries) {
    for (const auto& entry : entries) {
        if (entry.second == value) return std::string(entry.first);
    }
    throw std::invalid_argument("Normalizer enum value is invalid");
}

void requireText(const std::string& value, const char* message,
                 std::size_t maximum = ProtocolLimits::kMaximumIdentifierBytes) {
    require(!value.empty() && value.size() <= maximum
                    && std::any_of(value.begin(), value.end(), [](unsigned char current) {
                           return std::isspace(current) == 0;
                       }),
            message);
}

void requireSha256(const std::string& value) {
    require(value.size() == 64U
                    && std::all_of(value.begin(), value.end(), [](char current) {
                           return (current >= '0' && current <= '9')
                                   || (current >= 'a' && current <= 'f');
                       }),
            "Normalizer hash must be lowercase SHA-256");
}

void requireUtc(const std::string& value) {
    require(value.size() == 20U && value[4] == '-' && value[7] == '-'
                    && value[10] == 'T' && value[13] == ':' && value[16] == ':'
                    && value[19] == 'Z',
            "Normalizer time must use UTC seconds");
    static_cast<void>(inspection::utcEpochSeconds(value));
}

void requireSortedUnique(const std::vector<std::string>& values,
                         std::size_t maximum, const char* message) {
    require(values.size() <= maximum && std::is_sorted(values.begin(), values.end())
                    && std::adjacent_find(values.begin(), values.end()) == values.end(),
            message);
    for (const auto& value : values) requireText(value, message);
}

void requireDecoders(const std::vector<std::string>& values,
                     const char* message) {
    requireSortedUnique(values, ProtocolLimits::kMaximumDecoders, message);
    for (const auto& value : values) {
        require(std::find(kDecoders.begin(), kDecoders.end(), value)
                        != kDecoders.end(),
                message);
    }
}

void requireSafeMessage(const std::optional<std::string>& message) {
    if (!message.has_value()) return;
    require(message->size() <= ProtocolLimits::kMaximumSafeMessageBytes,
            "Normalizer safe message is too long");
    std::string lower = *message;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char current) {
        return static_cast<char>(std::tolower(current));
    });
    for (const auto marker : kSensitiveMarkers) {
        require(lower.find(marker) == std::string::npos,
                "Normalizer safe message contains sensitive material");
    }
}

Json toolJson(const ToolVersion& tool) {
    return Json{{"name", tool.name}, {"version", tool.version},
                {"buildSha256", tool.build_sha256}};
}

Json familyJson(const FamilyCapability& family) {
    return Json{{"canonicalFamily", canonicalFamilyName(family.canonical_family)},
                {"canonicalContractVersion", family.canonical_contract_version},
                {"validatorName", family.validator_name},
                {"validatorVersion", family.validator_version},
                {"validatorBuildSha256", family.validator_build_sha256},
                {"maximumInputBytes", family.maximum_input_bytes},
                {"maximumOutputBytes", family.maximum_output_bytes}};
}

AccessGrant parseGrant(const Json& json) {
    require(json.is_object(), "Normalizer access grant must be an object");
    requireOnly(json, {"grantId", "httpMethod", "url", "expiresAt"});
    AccessGrant grant;
    grant.grant_id = required<std::string>(json, "grantId");
    grant.http_method = required<std::string>(json, "httpMethod");
    grant.url = required<std::string>(json, "url");
    grant.expires_at = required<std::string>(json, "expiresAt");
    requireSha256(grant.grant_id);
    require(grant.http_method == "GET", "Normalizer access grant must use GET");
    requireText(grant.url, "Normalizer access grant URL is invalid",
                ProtocolLimits::kMaximumGrantUrlBytes);
    require(grant.url.rfind("http://", 0U) == 0U
                    || grant.url.rfind("https://", 0U) == 0U,
            "Normalizer access grant URL must use HTTP(S)");
    requireUtc(grant.expires_at);
    return grant;
}

ResourceRecord parseRecord(const Json& json) {
    require(json.is_object(), "Normalizer resource record must be an object");
    requireOnly(json, {"objectId", "packageRelativePath", "resourceRole",
                       "detectedKind", "detectedVersion", "mediaType", "size",
                       "sha256", "requiredExtensions", "usedExtensions",
                       "dependencyIds", "accessGrant"});
    ResourceRecord record;
    record.object_id = required<std::string>(json, "objectId");
    record.package_relative_path = required<std::string>(json, "packageRelativePath");
    record.resource_role = required<std::string>(json, "resourceRole");
    record.detected_kind = required<std::string>(json, "detectedKind");
    if (json.contains("detectedVersion") && !json.at("detectedVersion").is_null()) {
        record.detected_version = required<std::string>(json, "detectedVersion");
    }
    if (json.contains("mediaType") && !json.at("mediaType").is_null()) {
        record.media_type = required<std::string>(json, "mediaType");
    }
    record.size = required<std::uint64_t>(json, "size");
    record.sha256 = required<std::string>(json, "sha256");
    record.required_extensions = required<std::vector<std::string>>(
            json, "requiredExtensions");
    record.used_extensions = required<std::vector<std::string>>(
            json, "usedExtensions");
    record.dependency_ids = required<std::vector<std::string>>(json, "dependencyIds");
    record.access_grant = parseGrant(required<Json>(json, "accessGrant"));
    requireText(record.object_id, "Normalizer resource object ID is invalid");
    requireText(record.package_relative_path, "Normalizer resource path is invalid",
                ProtocolLimits::kMaximumPathBytes);
    require(record.package_relative_path.front() != '/'
                    && record.package_relative_path.find("..") == std::string::npos
                    && record.package_relative_path.find('\\') == std::string::npos
                    && record.package_relative_path.find(':') == std::string::npos,
            "Normalizer resource path escapes the approved namespace");
    requireText(record.resource_role, "Normalizer resource role is invalid");
    requireText(record.detected_kind, "Normalizer detected kind is invalid");
    requireSha256(record.sha256);
    requireSortedUnique(record.required_extensions, ProtocolLimits::kMaximumExtensions,
                        "Normalizer required extensions are invalid");
    requireSortedUnique(record.used_extensions, ProtocolLimits::kMaximumExtensions,
                        "Normalizer used extensions are invalid");
    requireSortedUnique(record.dependency_ids, ProtocolLimits::kMaximumResources,
                        "Normalizer dependency IDs are invalid");
    require(std::includes(record.used_extensions.begin(), record.used_extensions.end(),
                          record.required_extensions.begin(),
                          record.required_extensions.end()),
            "Normalizer required extensions are not a subset of used extensions");
    return record;
}

Json recordIdentityJson(const ResourceRecord& record) {
    Json json = {{"accessGrant", nullptr},
                 {"dependencyIds", record.dependency_ids},
                 {"detectedKind", record.detected_kind},
                 {"detectedVersion", record.detected_version.has_value()
                         ? Json(*record.detected_version) : Json(nullptr)},
                 {"mediaType", record.media_type.has_value()
                         ? Json(*record.media_type) : Json(nullptr)},
                 {"objectId", record.object_id},
                 {"packageRelativePath", record.package_relative_path},
                 {"requiredExtensions", record.required_extensions},
                 {"resourceRole", record.resource_role},
                 {"sha256", record.sha256},
                 {"size", record.size},
                 {"usedExtensions", record.used_extensions}};
    return json;
}

Json validationSummaryJson(const ValidationSummary& summary,
                           bool include_validation_hash) {
    Json json = {{"accessorCount", summary.accessor_count},
                 {"bufferCount", summary.buffer_count},
                 {"coordinateBasis", summary.coordinate_basis},
                 {"featureCount", summary.feature_count},
                 {"featureIdentityModel", enumName(summary.feature_identity_model,
                                                    kIdentityModels)},
                 {"imageCount", summary.image_count},
                 {"metadataPropertyCount", summary.metadata_property_count},
                 {"nodeCount", summary.node_count},
                 {"primitiveCount", summary.primitive_count},
                 {"requiredExtensions", summary.required_extensions},
                 {"sceneCount", summary.scene_count},
                 {"usedExtensions", summary.used_extensions},
                 {"validatorBuildSha256", summary.validator_build_sha256},
                 {"validatorName", summary.validator_name},
                 {"validatorVersion", summary.validator_version}};
    if (include_validation_hash) json["validationHash"] = summary.validation_hash;
    return json;
}

void validateSummary(const ValidationSummary& summary) {
    require(summary.coordinate_basis == kCoordinateBasis,
            "Canonical coordinate basis is incompatible");
    require(summary.scene_count > 0U && summary.primitive_count > 0U
                    && summary.accessor_count > 0U && summary.buffer_count > 0U,
            "Canonical validation counts are invalid");
    requireSortedUnique(summary.required_extensions, ProtocolLimits::kMaximumExtensions,
                        "Canonical required extensions are invalid");
    requireSortedUnique(summary.used_extensions, ProtocolLimits::kMaximumExtensions,
                        "Canonical used extensions are invalid");
    require(std::includes(summary.used_extensions.begin(), summary.used_extensions.end(),
                          summary.required_extensions.begin(),
                          summary.required_extensions.end()),
            "Canonical required extensions are not a subset of used extensions");
    requireText(summary.validator_name, "Canonical validator name is invalid");
    requireText(summary.validator_version, "Canonical validator version is invalid");
    requireSha256(summary.validator_build_sha256);
    requireSha256(summary.validation_hash);
    require(summary.validation_hash
                    == sha256Hex(validationSummaryJson(summary, false).dump()),
            "Canonical validation summary hash is inconsistent");
}

ResourceManifestDescriptor parseDescriptor(const Json& json) {
    require(json.is_object(), "Normalizer manifest descriptor must be an object");
    requireOnly(json, {"manifestVersion", "manifestId", "pageCount", "pageSize",
                       "recordCount", "manifestSha256"});
    ResourceManifestDescriptor descriptor;
    descriptor.manifest_version = required<std::string>(json, "manifestVersion");
    descriptor.manifest_id = required<std::string>(json, "manifestId");
    descriptor.page_count = required<std::uint32_t>(json, "pageCount");
    descriptor.page_size = required<std::uint32_t>(json, "pageSize");
    descriptor.record_count = required<std::uint64_t>(json, "recordCount");
    descriptor.manifest_sha256 = required<std::string>(json, "manifestSha256");
    require(descriptor.manifest_version == kManifestVersion,
            "Normalizer manifest version is incompatible");
    requireSha256(descriptor.manifest_id);
    requireSha256(descriptor.manifest_sha256);
    require(descriptor.page_count > 0U
                    && descriptor.page_count <= ProtocolLimits::kMaximumPages
                    && descriptor.page_size > 0U
                    && descriptor.page_size <= ProtocolLimits::kMaximumPageRecords
                    && descriptor.record_count > 0U
                    && descriptor.record_count <= ProtocolLimits::kMaximumResources
                    && descriptor.page_count
                            == (descriptor.record_count + descriptor.page_size - 1U)
                                    / descriptor.page_size,
            "Normalizer manifest descriptor counts are invalid");
    return descriptor;
}

bool sameFamily(const FamilyCapability& capability, const ClaimTask& task) {
    return capability.canonical_family == task.canonical_family
            && capability.canonical_contract_version
                    == task.canonical_contract_version;
}

}  // namespace

std::string canonicalFamilyName(CanonicalFamily family) {
    return enumName(family, kCanonicalFamilies);
}

std::string sha256Hex(const std::string& bytes) {
    std::array<unsigned char, kSha256Bytes> digest{};
    unsigned int digest_size = 0U;
    require(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digest_size,
                       EVP_sha256(), nullptr) == 1
                    && digest_size == digest.size(),
            "Normalizer SHA-256 computation failed");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : digest) output << std::setw(2) << static_cast<int>(value);
    return output.str();
}

std::string serializeClaimRequest(const ClaimRequest& request) {
    requireText(request.worker_id, "Normalizer worker ID is invalid");
    require(request.protocol_version == kProtocolVersion
                    && request.schema_sha256 == kSchemaSha256,
            "Normalizer protocol/schema is incompatible");
    requireDecoders(request.decoder_capabilities,
                    "Normalizer decoder capabilities are invalid");
    require(!request.supported_normalization_versions.empty()
                    && request.supported_normalization_versions.size()
                            <= ProtocolLimits::kMaximumFamilies,
            "Normalizer versions are invalid");
    requireSortedUnique(request.supported_normalization_versions,
                        ProtocolLimits::kMaximumFamilies,
                        "Normalizer versions are invalid");
    require(!request.family_capabilities.empty()
                    && request.family_capabilities.size()
                            <= ProtocolLimits::kMaximumFamilies,
            "Normalizer family capabilities are invalid");
    requireText(request.resource_profile_version,
                "Normalizer resource profile version is invalid");
    requireSha256(request.resource_profile_sha256);
    require(!request.tool_versions.empty()
                    && request.tool_versions.size() <= ProtocolLimits::kMaximumTools,
            "Normalizer tool versions are invalid");
    Json tools = Json::array();
    std::set<std::string> tool_identities;
    for (const auto& tool : request.tool_versions) {
        requireText(tool.name, "Normalizer tool name is invalid");
        requireText(tool.version, "Normalizer tool version is invalid");
        requireSha256(tool.build_sha256);
        const std::string identity = tool.name + '\0' + tool.version + '\0'
                + tool.build_sha256;
        require(tool_identities.insert(identity).second,
                "Normalizer tool version is duplicated");
        tools.push_back(toolJson(tool));
    }
    Json families = Json::array();
    std::set<CanonicalFamily> seen_families;
    for (const auto& family : request.family_capabilities) {
        require(seen_families.insert(family.canonical_family).second,
                "Normalizer family capability is duplicated");
        requireText(family.canonical_contract_version,
                    "Normalizer canonical contract version is invalid");
        requireText(family.validator_name, "Normalizer validator name is invalid");
        requireText(family.validator_version,
                    "Normalizer validator version is invalid");
        requireSha256(family.validator_build_sha256);
        require(family.maximum_input_bytes > 0U
                        && family.maximum_output_bytes > 0U,
                "Normalizer family limits are invalid");
        const std::string validator_identity = family.validator_name + '\0'
                + family.validator_version + '\0'
                + family.validator_build_sha256;
        require(tool_identities.find(validator_identity) != tool_identities.end(),
                "Normalizer family validator is not registered as a tool");
        families.push_back(familyJson(family));
    }
    return Json{{"messageType", "CLAIM_REQUEST"},
                {"workerId", request.worker_id},
                {"protocolVersion", request.protocol_version},
                {"schemaSha256", request.schema_sha256},
                {"decoderCapabilities", request.decoder_capabilities},
                {"supportedNormalizationVersions",
                 request.supported_normalization_versions},
                {"familyCapabilities", std::move(families)},
                {"resourceProfileVersion", request.resource_profile_version},
                {"resourceProfileSha256", request.resource_profile_sha256},
                {"toolVersions", std::move(tools)}}
            .dump();
}

ClaimTask parseClaimTask(const std::string& text) {
    const Json json = parseObject(text);
    requireOnly(json, {"messageType", "taskId", "attemptId", "requestId",
                       "leaseToken", "leaseExpireTime", "hardDeadlineTime",
                       "tileContentId", "sourceClosureHash",
                       "resourceClosureVersion", "normalizationVersion",
                       "canonicalFamily", "canonicalContractVersion",
                       "resourceProfileVersion", "resourceProfileSha256",
                       "requiredDecoders",
                       "resourceManifest"});
    require(required<std::string>(json, "messageType") == "CLAIM_RESPONSE",
            "Normalizer claim response message type is invalid");
    ClaimTask task;
    task.task_id = required<std::string>(json, "taskId");
    task.attempt_id = required<std::string>(json, "attemptId");
    task.request_id = required<std::string>(json, "requestId");
    task.lease_token = required<std::string>(json, "leaseToken");
    task.lease_expire_time = required<std::string>(json, "leaseExpireTime");
    task.hard_deadline_time = required<std::string>(json, "hardDeadlineTime");
    task.tile_content_id = required<std::string>(json, "tileContentId");
    task.source_closure_hash = required<std::string>(json, "sourceClosureHash");
    task.resource_closure_version = required<std::string>(
            json, "resourceClosureVersion");
    task.normalization_version = required<std::string>(json, "normalizationVersion");
    task.canonical_family = parseEnum(required<std::string>(json, "canonicalFamily"),
                                      kCanonicalFamilies);
    task.canonical_contract_version = required<std::string>(
            json, "canonicalContractVersion");
    task.resource_profile_version = required<std::string>(
            json, "resourceProfileVersion");
    task.resource_profile_sha256 = required<std::string>(
            json, "resourceProfileSha256");
    task.required_decoders = required<std::vector<std::string>>(
            json, "requiredDecoders");
    requireDecoders(task.required_decoders,
                    "Normalizer required decoders are invalid");
    task.resource_manifest = parseDescriptor(required<Json>(json, "resourceManifest"));
    return task;
}

ResourceManifestPage parseResourceManifestPage(const std::string& text) {
    const Json json = parseObject(text);
    requireOnly(json, {"messageType", "manifestId", "pageNumber", "recordCount",
                       "pageSha256", "records"});
    require(required<std::string>(json, "messageType") == "RESOURCE_MANIFEST_PAGE",
            "Normalizer manifest page message type is invalid");
    ResourceManifestPage page;
    page.manifest_id = required<std::string>(json, "manifestId");
    page.page_number = required<std::uint32_t>(json, "pageNumber");
    page.record_count = required<std::uint32_t>(json, "recordCount");
    page.page_sha256 = required<std::string>(json, "pageSha256");
    const Json records = required<Json>(json, "records");
    require(records.is_array(), "Normalizer manifest records must be an array");
    for (const auto& record : records) page.records.push_back(parseRecord(record));
    return page;
}

std::string serializeHeartbeatRequest(
        const std::string& worker_id, const ClaimTask& task, TaskPhase phase,
        std::uint64_t total_resources, std::uint64_t processed_resources) {
    require(processed_resources <= total_resources,
            "Normalizer heartbeat progress is invalid");
    return Json{{"messageType", "HEARTBEAT_REQUEST"},
                {"workerId", worker_id},
                {"leaseToken", task.lease_token},
                {"requestId", task.request_id},
                {"phase", enumName(phase, kTaskPhases)},
                {"totalResources", total_resources},
                {"processedResources", processed_resources}}
            .dump();
}

HeartbeatResponse parseHeartbeatResponse(const std::string& text) {
    const Json json = parseObject(text);
    requireOnly(json, {"messageType", "leaseExpireTime", "hardDeadlineTime",
                       "cancelRequested"});
    require(required<std::string>(json, "messageType") == "HEARTBEAT_RESPONSE",
            "Normalizer heartbeat response message type is invalid");
    HeartbeatResponse response;
    response.lease_expire_time = required<std::string>(json, "leaseExpireTime");
    response.hard_deadline_time = required<std::string>(json, "hardDeadlineTime");
    response.cancel_requested = required<bool>(json, "cancelRequested");
    requireUtc(response.lease_expire_time);
    requireUtc(response.hard_deadline_time);
    require(inspection::utcEpochSeconds(response.lease_expire_time)
                    <= inspection::utcEpochSeconds(response.hard_deadline_time),
            "Normalizer lease exceeds hard deadline");
    return response;
}

std::string serializeUploadPrepareRequest(
        const std::string& worker_id, const ClaimTask& task,
        const UploadDeclaration& declaration) {
    validateUploadDeclaration(declaration, task);
    return Json{{"messageType", "UPLOAD_PREPARE_REQUEST"},
                {"workerId", worker_id},
                {"leaseToken", task.lease_token},
                {"requestId", task.request_id},
                {"canonicalFamily", canonicalFamilyName(declaration.canonical_family)},
                {"canonicalContractVersion",
                 declaration.canonical_contract_version},
                {"outputSize", declaration.output_size},
                {"outputSha256", declaration.output_sha256},
                {"semanticHash", declaration.semantic_hash},
                {"validationManifestSha256",
                 declaration.validation_manifest_sha256},
                {"validationSummary",
                 validationSummaryJson(declaration.validation_summary, true)}}
            .dump();
}

UploadGrant parseUploadGrant(const std::string& text) {
    const Json json = parseObject(text);
    requireOnly(json, {"messageType", "uploadGrantId", "httpMethod", "uploadUrl",
                       "expiresAt"});
    require(required<std::string>(json, "messageType") == "UPLOAD_PREPARE_RESPONSE",
            "Normalizer upload grant message type is invalid");
    UploadGrant grant;
    grant.upload_grant_id = required<std::string>(json, "uploadGrantId");
    grant.http_method = required<std::string>(json, "httpMethod");
    grant.upload_url = required<std::string>(json, "uploadUrl");
    grant.expires_at = required<std::string>(json, "expiresAt");
    requireSha256(grant.upload_grant_id);
    require(grant.http_method == "PUT", "Normalizer upload grant must use PUT");
    requireText(grant.upload_url, "Normalizer upload URL is invalid",
                ProtocolLimits::kMaximumGrantUrlBytes);
    require(grant.upload_url.rfind("http://", 0U) == 0U
                    || grant.upload_url.rfind("https://", 0U) == 0U,
            "Normalizer upload URL must use HTTP(S)");
    requireUtc(grant.expires_at);
    return grant;
}

std::string serializeUploadReportRequest(
        const std::string& worker_id, const ClaimTask& task,
        const UploadReport& report) {
    requireText(report.output_etag, "Normalizer upload ETag is invalid");
    require(report.output_size > 0U, "Normalizer upload size is invalid");
    requireSha256(report.output_sha256);
    return Json{{"messageType", "UPLOAD_REPORT_REQUEST"},
                {"workerId", worker_id},
                {"leaseToken", task.lease_token},
                {"requestId", task.request_id},
                {"outputEtag", report.output_etag},
                {"outputSize", report.output_size},
                {"outputSha256", report.output_sha256}}
            .dump();
}

std::string serializeCompleteRequest(const std::string& worker_id,
                                     const ClaimTask& task) {
    return Json{{"messageType", "COMPLETE_REQUEST"},
                {"workerId", worker_id},
                {"leaseToken", task.lease_token},
                {"requestId", task.request_id}}
            .dump();
}

std::string serializeFailRequest(const std::string& worker_id,
                                 const ClaimTask& task,
                                 const Failure& failure) {
    requireText(failure.error_code, "Normalizer failure code is invalid");
    requireSafeMessage(failure.error_message);
    Json json = {{"messageType", "FAIL_REQUEST"},
                 {"workerId", worker_id},
                 {"leaseToken", task.lease_token},
                 {"requestId", task.request_id},
                 {"errorCode", failure.error_code},
                 {"errorMessage", failure.error_message.has_value()
                         ? Json(*failure.error_message) : Json(nullptr)},
                 {"retryable", failure.retryable},
                 {"unsupported", failure.unsupported}};
    return json.dump();
}

void validateClaimTask(const ClaimTask& task, const ClaimRequest& capabilities,
                       const std::string& now_utc) {
    requireText(task.task_id, "Normalizer claim task ID is invalid");
    requireText(task.attempt_id, "Normalizer claim attempt ID is invalid");
    requireText(task.request_id, "Normalizer claim request ID is invalid");
    requireText(task.lease_token, "Normalizer claim lease token is invalid");
    requireText(task.tile_content_id, "Normalizer content ID is invalid");
    requireSha256(task.source_closure_hash);
    requireSha256(task.resource_profile_sha256);
    requireUtc(now_utc);
    requireUtc(task.lease_expire_time);
    requireUtc(task.hard_deadline_time);
    require(inspection::utcEpochSeconds(now_utc)
                            < inspection::utcEpochSeconds(task.lease_expire_time)
                    && inspection::utcEpochSeconds(task.lease_expire_time)
                            <= inspection::utcEpochSeconds(task.hard_deadline_time),
            "Normalizer claim lease window is invalid");
    require(task.resource_profile_version == capabilities.resource_profile_version
                    && task.resource_profile_sha256
                            == capabilities.resource_profile_sha256
                    && std::find(capabilities.supported_normalization_versions.begin(),
                                 capabilities.supported_normalization_versions.end(),
                                 task.normalization_version)
                            != capabilities.supported_normalization_versions.end()
                    && std::any_of(capabilities.family_capabilities.begin(),
                                   capabilities.family_capabilities.end(),
                                   [&task](const FamilyCapability& capability) {
                                       return sameFamily(capability, task);
                                   })
                    && std::includes(capabilities.decoder_capabilities.begin(),
                                     capabilities.decoder_capabilities.end(),
                                     task.required_decoders.begin(),
                                     task.required_decoders.end()),
            "Normalizer claim is incompatible with worker capabilities");
}

void validateResourceManifestPage(const ResourceManifestPage& page,
                                  const ClaimTask& task,
                                  const std::string& now_utc) {
    require(page.manifest_id == task.resource_manifest.manifest_id
                    && page.page_number > 0U
                    && page.page_number <= task.resource_manifest.page_count
                    && !page.records.empty()
                    && page.records.size() == page.record_count
                    && page.record_count <= task.resource_manifest.page_size,
            "Normalizer manifest page identity/count is invalid");
    requireSha256(page.page_sha256);
    require(page.page_sha256 == canonicalPageSha256(page.records),
            "Normalizer manifest page hash mismatch");
    std::set<std::string> object_ids;
    std::set<std::string> paths;
    for (const auto& record : page.records) {
        require(object_ids.insert(record.object_id).second,
                "Normalizer manifest page contains duplicate object IDs");
        require(paths.insert(record.package_relative_path).second,
                "Normalizer manifest page contains duplicate paths");
        require(inspection::utcEpochSeconds(now_utc)
                        < inspection::utcEpochSeconds(record.access_grant.expires_at),
                "Normalizer resource grant is expired");
        require(inspection::utcEpochSeconds(record.access_grant.expires_at)
                        <= inspection::utcEpochSeconds(task.hard_deadline_time),
                "Normalizer resource grant exceeds the hard deadline");
    }
}

void validateResourceManifest(const ClaimTask& task,
                              const std::vector<ResourceManifestPage>& pages,
                              const std::string& now_utc,
                              std::uint64_t maximum_input_bytes) {
    require(maximum_input_bytes > 0U,
            "Normalizer maximum input bytes is invalid");
    require(pages.size() == task.resource_manifest.page_count,
            "Normalizer manifest is incomplete");
    std::set<std::uint32_t> page_numbers;
    std::set<std::string> object_ids;
    std::set<std::string> paths;
    std::uint64_t record_count = 0U;
    std::uint64_t total_bytes = 0U;
    for (const auto& page : pages) {
        validateResourceManifestPage(page, task, now_utc);
        require(page_numbers.insert(page.page_number).second,
                "Normalizer manifest contains duplicate pages");
        record_count += page.record_count;
        for (const auto& record : page.records) {
            require(object_ids.insert(record.object_id).second,
                    "Normalizer manifest contains duplicate object IDs");
            require(paths.insert(record.package_relative_path).second,
                    "Normalizer manifest contains duplicate paths");
            require(record.size <= maximum_input_bytes - total_bytes,
                    "Normalizer manifest exceeds the input byte limit");
            total_bytes += record.size;
        }
    }
    require(record_count == task.resource_manifest.record_count
                    && canonicalManifestSha256(pages)
                            == task.resource_manifest.manifest_sha256,
            "Normalizer manifest count/hash is inconsistent");
    for (const auto& page : pages) {
        for (const auto& record : page.records) {
            for (const auto& dependency_id : record.dependency_ids) {
                require(object_ids.find(dependency_id) != object_ids.end(),
                        "Normalizer dependency escapes the approved manifest");
            }
        }
    }
}

void validateUploadDeclaration(const UploadDeclaration& declaration,
                               const ClaimTask& task) {
    require(declaration.canonical_family == task.canonical_family
                    && declaration.canonical_contract_version
                            == task.canonical_contract_version,
            "Normalizer upload contract differs from the claim");
    require(declaration.output_size > 0U
                    && declaration.output_size
                            <= ProtocolLimits::kMaximumArtifactBytes,
            "Normalizer output size is invalid");
    requireSha256(declaration.output_sha256);
    requireSha256(declaration.semantic_hash);
    requireSha256(declaration.validation_manifest_sha256);
    validateSummary(declaration.validation_summary);
    require(declaration.validation_manifest_sha256
                    == sha256Hex(validationSummaryJson(
                                         declaration.validation_summary, true)
                                         .dump()),
            "Normalizer validation manifest hash is inconsistent");
}

std::string canonicalPageSha256(const std::vector<ResourceRecord>& records) {
    Json array = Json::array();
    for (const auto& record : records) array.push_back(recordIdentityJson(record));
    return sha256Hex(array.dump());
}

std::string canonicalManifestSha256(
        const std::vector<ResourceManifestPage>& pages) {
    std::vector<const ResourceManifestPage*> sorted;
    sorted.reserve(pages.size());
    for (const auto& page : pages) sorted.push_back(&page);
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return left->page_number < right->page_number;
    });
    std::ostringstream material;
    material << kManifestHashDomain << '\n';
    for (const auto* page : sorted) {
        material << page->page_number << ':' << page->page_sha256 << ':'
                 << page->record_count << '\n';
    }
    return sha256Hex(material.str());
}

}  // namespace clip_worker::normalization
