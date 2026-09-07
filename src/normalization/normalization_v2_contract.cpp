#include "clip_worker/normalization/normalization_v2_contract.hpp"

#include "clip_worker/inspection/inspection_contract.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization::v2 {
namespace {

using Json = nlohmann::json;

constexpr const char* kOutputIdDomain = "THREE_D_NORMALIZATION_OUTPUT_ID_V2";
constexpr const char* kOutputManifestDomain =
        "THREE_D_NORMALIZATION_OUTPUT_MANIFEST_V2";
constexpr const char* kCompositeChildIdDomain =
        "THREE_D_COMPOSITE_CHILD_ID_V1";

template <typename Enum>
using EnumEntry = std::pair<std::string_view, Enum>;

constexpr std::array<EnumEntry<CanonicalFamily>, 4U> kFamilies{{
        {"MESH_GLTF2", CanonicalFamily::mesh_gltf2},
        {"POINT_GLTF2", CanonicalFamily::point_gltf2},
        {"INSTANCE_GLTF2", CanonicalFamily::instance_gltf2},
        {"COMPOSITE_CHILDREN", CanonicalFamily::composite_children}}};
constexpr std::array<EnumEntry<OutputKind>, 2U> kOutputKinds{{
        {"CANONICAL_CONTENT", OutputKind::canonical_content},
        {"PARENT_MANIFEST", OutputKind::parent_manifest}}};
constexpr std::array<EnumEntry<TaskPhase>, 9U> kTaskPhases{{
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

void requireOnly(const Json& value,
                 std::initializer_list<const char*> allowed) {
    std::set<std::string> names;
    for (const char* name : allowed) names.emplace(name);
    for (const auto& item : value.items()) {
        require(names.find(item.key()) != names.end(),
                "Normalizer V2 message contains an unknown field");
    }
}

Json parseObject(const std::string& text) {
    try {
        Json value = Json::parse(text);
        require(value.is_object(), "Normalizer V2 message must be an object");
        return value;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const Json::exception&) {
        throw std::invalid_argument("Normalizer V2 JSON is invalid");
    }
}

template <typename Value>
Value required(const Json& value, const char* name) {
    const auto item = value.find(name);
    require(item != value.end() && !item->is_null(),
            "Normalizer V2 message is missing a field");
    try {
        return item->get<Value>();
    } catch (const Json::exception&) {
        throw std::invalid_argument("Normalizer V2 field has an invalid type");
    }
}

template <typename Enum, std::size_t Size>
Enum parseEnum(const std::string& value,
               const std::array<EnumEntry<Enum>, Size>& entries) {
    for (const auto& entry : entries) {
        if (entry.first == value) return entry.second;
    }
    throw std::invalid_argument("Normalizer V2 enum value is invalid");
}

template <typename Enum, std::size_t Size>
std::string enumName(Enum value,
                     const std::array<EnumEntry<Enum>, Size>& entries) {
    for (const auto& entry : entries) {
        if (entry.second == value) return std::string(entry.first);
    }
    throw std::invalid_argument("Normalizer V2 enum value is invalid");
}

void requireText(const std::string& value, const char* message,
                 std::size_t maximum = ProtocolLimits::kMaximumIdentifierBytes) {
    require(!value.empty() && value.size() <= maximum
                    && std::any_of(value.begin(), value.end(),
                                   [](unsigned char current) {
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
            "Normalizer V2 hash must be lowercase SHA-256");
}

void requireUtc(const std::string& value) {
    require(value.size() == 20U && value[4] == '-' && value[7] == '-'
                    && value[10] == 'T' && value[13] == ':' && value[16] == ':'
                    && value[19] == 'Z',
            "Normalizer V2 time must use UTC seconds");
    static_cast<void>(inspection::utcEpochSeconds(value));
}

void requireSortedUnique(const std::vector<std::string>& values,
                         std::size_t maximum, const char* message) {
    require(values.size() <= maximum && std::is_sorted(values.begin(), values.end())
                    && std::adjacent_find(values.begin(), values.end())
                            == values.end(),
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

Json toolJson(const ToolVersion& tool) {
    return {{"name", tool.name}, {"version", tool.version},
            {"buildSha256", tool.build_sha256}};
}

Json familyJson(const FamilyCapability& family) {
    return {{"canonicalFamily", canonicalFamilyName(family.canonical_family)},
            {"normalizationVersion", family.normalization_version},
            {"canonicalContractVersion", family.canonical_contract_version},
            {"validatorName", family.validator_name},
            {"validatorVersion", family.validator_version},
            {"validatorBuildSha256", family.validator_build_sha256},
            {"maximumInputBytes", family.maximum_input_bytes},
            {"maximumOutputBytes", family.maximum_output_bytes}};
}

ResourceManifestDescriptor parseDescriptor(const Json& json) {
    require(json.is_object(),
            "Normalizer V2 manifest descriptor must be an object");
    requireOnly(json, {"manifestVersion", "manifestId", "pageCount",
                       "pageSize", "recordCount", "manifestSha256"});
    ResourceManifestDescriptor result;
    result.manifest_version = required<std::string>(json, "manifestVersion");
    result.manifest_id = required<std::string>(json, "manifestId");
    result.page_count = required<std::uint32_t>(json, "pageCount");
    result.page_size = required<std::uint32_t>(json, "pageSize");
    result.record_count = required<std::uint64_t>(json, "recordCount");
    result.manifest_sha256 = required<std::string>(json, "manifestSha256");
    require(result.manifest_version == kManifestVersion,
            "Normalizer V2 manifest version is incompatible");
    requireSha256(result.manifest_id);
    requireSha256(result.manifest_sha256);
    require(result.page_count > 0U
                    && result.page_count <= ProtocolLimits::kMaximumPages
                    && result.page_size > 0U
                    && result.page_size <= ProtocolLimits::kMaximumPageRecords
                    && result.record_count > 0U
                    && result.record_count <= ProtocolLimits::kMaximumResources
                    && result.page_count
                            == (result.record_count + result.page_size - 1U)
                                    / result.page_size,
            "Normalizer V2 manifest descriptor counts are invalid");
    return result;
}

std::string identityName(FeatureIdentityModel model) {
    switch (model) {
        case FeatureIdentityModel::none: return "NONE";
        case FeatureIdentityModel::legacy_batch_table_mapped:
            return "LEGACY_BATCH_TABLE_MAPPED";
        case FeatureIdentityModel::attribute_feature_id_property_table:
            return "ATTRIBUTE_FEATURE_ID_PROPERTY_TABLE";
        case FeatureIdentityModel::point_feature_id: return "POINT_FEATURE_ID";
        case FeatureIdentityModel::instance_feature_id:
            return "INSTANCE_FEATURE_ID";
    }
    throw std::invalid_argument("Normalizer V2 feature identity is invalid");
}

Json nullable(const std::optional<std::uint64_t>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

Json nullable(const std::optional<std::string>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

Json validationSummaryJson(const ValidationSummary& summary,
                           bool include_hash) {
    Json json{{"accessorCount", nullable(summary.accessor_count)},
              {"bufferCount", nullable(summary.buffer_count)},
              {"coordinateBasis", nullable(summary.coordinate_basis)},
              {"featureCount", nullable(summary.feature_count)},
              {"featureIdentityModel", nullable(summary.feature_identity_model)},
              {"imageCount", nullable(summary.image_count)},
              {"metadataPropertyCount", nullable(summary.metadata_property_count)},
              {"nodeCount", nullable(summary.node_count)},
              {"primitiveCount", nullable(summary.primitive_count)},
              {"requiredExtensions", summary.required_extensions},
              {"sceneCount", nullable(summary.scene_count)},
              {"usedExtensions", summary.used_extensions},
              {"validatorBuildSha256", summary.validator_build_sha256},
              {"validatorName", summary.validator_name},
              {"validatorVersion", summary.validator_version}};
    if (summary.point_count.has_value()) json["pointCount"] = *summary.point_count;
    if (summary.instance_count.has_value()) {
        json["instanceCount"] = *summary.instance_count;
    }
    if (summary.composite_child_count.has_value()) {
        json["compositeChildCount"] = *summary.composite_child_count;
    }
    if (summary.boundary_expanded_instance_count.has_value()) {
        json["boundaryExpandedInstanceCount"] =
                *summary.boundary_expanded_instance_count;
    }
    if (include_hash) json["validationHash"] = summary.validation_hash;
    return json;
}

const FamilyCapability* findCapability(const ClaimRequest& capabilities,
                                       CanonicalFamily family,
                                       const std::string& normalization_version,
                                       const std::string& contract_version) {
    const auto found = std::find_if(
            capabilities.family_capabilities.begin(),
            capabilities.family_capabilities.end(),
            [family, &normalization_version,
             &contract_version](const FamilyCapability& capability) {
                return capability.canonical_family == family
                        && capability.normalization_version
                                == normalization_version
                        && capability.canonical_contract_version
                                == contract_version;
            });
    return found == capabilities.family_capabilities.end() ? nullptr : &*found;
}

const FamilyCapability* findOutputCapability(
        const ClaimRequest& capabilities, CanonicalFamily family,
        const std::string& contract_version) {
    const auto found = std::find_if(
            capabilities.family_capabilities.begin(),
            capabilities.family_capabilities.end(),
            [family, &contract_version](const FamilyCapability& capability) {
                return capability.canonical_family == family
                        && capability.canonical_contract_version
                                == contract_version;
            });
    return found == capabilities.family_capabilities.end() ? nullptr : &*found;
}

bool metadataTask(const ClaimTask& task) {
    return task.normalization_version
                    == v3::normalizationVersion(task.canonical_family)
            && task.canonical_contract_version
                    == v3::canonicalContractVersion(task.canonical_family);
}

std::string outputContractVersion(const ClaimTask& task,
                                  CanonicalFamily family) {
    return metadataTask(task) ? v3::canonicalContractVersion(family)
                              : canonicalContractVersion(family);
}

normalization::ClaimTask legacyTask(const ClaimTask& task) {
    normalization::ClaimTask result;
    result.task_id = task.task_id;
    result.attempt_id = task.attempt_id;
    result.request_id = task.request_id;
    result.lease_token = task.lease_token;
    result.lease_expire_time = task.lease_expire_time;
    result.hard_deadline_time = task.hard_deadline_time;
    result.tile_content_id = task.tile_content_id;
    result.source_closure_hash = task.source_closure_hash;
    result.resource_closure_version = task.resource_closure_version;
    result.normalization_version = task.normalization_version;
    result.canonical_contract_version = task.canonical_contract_version;
    result.resource_profile_version = task.resource_profile_version;
    result.resource_profile_sha256 = task.resource_profile_sha256;
    result.required_decoders = task.required_decoders;
    result.resource_manifest = task.resource_manifest;
    return result;
}

void validateSummary(const ValidationSummary& summary, CanonicalFamily family,
                     const FamilyCapability& capability,
                     OutputKind output_kind) {
    require(summary.validator_name == capability.validator_name
                    && summary.validator_version == capability.validator_version
                    && summary.validator_build_sha256
                            == capability.validator_build_sha256,
            "Normalizer V2 validator identity is inconsistent");
    requireSha256(summary.validator_build_sha256);
    requireSha256(summary.validation_hash);
    requireSortedUnique(summary.required_extensions,
                        ProtocolLimits::kMaximumExtensions,
                        "Normalizer V2 required extensions are invalid");
    requireSortedUnique(summary.used_extensions,
                        ProtocolLimits::kMaximumExtensions,
                        "Normalizer V2 used extensions are invalid");
    require(std::includes(summary.used_extensions.begin(),
                          summary.used_extensions.end(),
                          summary.required_extensions.begin(),
                          summary.required_extensions.end()),
            "Normalizer V2 required extensions are not a used subset");
    require(summary.validation_hash
                    == sha256Hex(validationSummaryJson(summary, false).dump()),
            "Normalizer V2 validation summary hash is inconsistent");
    if (output_kind == OutputKind::parent_manifest) {
        require(family == CanonicalFamily::composite_children
                        && summary.composite_child_count.value_or(0U) > 0U,
                "Normalizer V2 parent summary is invalid");
        return;
    }
    require(summary.coordinate_basis == kCoordinateBasis
                    && summary.scene_count.value_or(0U) > 0U
                    && summary.node_count.value_or(0U) > 0U
                    && summary.primitive_count.value_or(0U) > 0U
                    && summary.accessor_count.value_or(0U) > 0U
                    && summary.buffer_count.value_or(0U) > 0U
                    && summary.image_count.has_value()
                    && summary.feature_count.has_value()
                    && summary.metadata_property_count.has_value()
                    && summary.feature_identity_model.has_value(),
            "Normalizer V2 content summary is invalid");
    if (family == CanonicalFamily::point_gltf2) {
        require(summary.point_count.value_or(0U) > 0U
                        && *summary.feature_identity_model
                                != "INSTANCE_FEATURE_ID",
                "Normalizer V2 point summary is invalid");
    }
    if (family == CanonicalFamily::instance_gltf2) {
        require(summary.instance_count.value_or(0U) > 0U
                        && summary.boundary_expanded_instance_count.has_value(),
                "Normalizer V2 instance summary is invalid");
    }
}

}  // namespace

std::string canonicalFamilyName(CanonicalFamily family) {
    return enumName(family, kFamilies);
}

std::string outputKindName(OutputKind kind) {
    return enumName(kind, kOutputKinds);
}

std::string canonicalContractVersion(CanonicalFamily family) {
    switch (family) {
        case CanonicalFamily::mesh_gltf2:
            return kMeshCanonicalContractVersion;
        case CanonicalFamily::point_gltf2:
            return kPointCanonicalContractVersion;
        case CanonicalFamily::instance_gltf2:
            return kInstanceCanonicalContractVersion;
        case CanonicalFamily::composite_children:
            return kCompositeCanonicalContractVersion;
    }
    throw std::invalid_argument("Normalizer V2 family is invalid");
}

std::string normalizationVersion(CanonicalFamily family) {
    switch (family) {
        case CanonicalFamily::mesh_gltf2: return kMeshNormalizationVersion;
        case CanonicalFamily::point_gltf2: return kPointNormalizationVersion;
        case CanonicalFamily::instance_gltf2:
            return kInstanceNormalizationVersion;
        case CanonicalFamily::composite_children:
            return kCompositeNormalizationVersion;
    }
    throw std::invalid_argument("Normalizer V2 family is invalid");
}

CanonicalFamily fromV1Family(normalization::CanonicalFamily family) {
    switch (family) {
        case normalization::CanonicalFamily::mesh_gltf2:
            return CanonicalFamily::mesh_gltf2;
        case normalization::CanonicalFamily::point_gltf2:
            return CanonicalFamily::point_gltf2;
        case normalization::CanonicalFamily::instance_gltf2:
            return CanonicalFamily::instance_gltf2;
    }
    throw std::invalid_argument("Normalizer canonical family is invalid");
}

std::string serializeClaimRequest(const ClaimRequest& request) {
    requireText(request.worker_id, "Normalizer V2 worker ID is invalid");
    const bool v2_claim = request.protocol_version == kProtocolVersion
            && request.schema_sha256 == kSchemaSha256
            && request.resource_profile_version == kResourceProfileVersion;
    const bool v3_claim = request.protocol_version == v3::kProtocolVersion
            && request.schema_sha256 == v3::kSchemaSha256
            && request.resource_profile_version
                    == v3::kResourceProfileVersion;
    require(v2_claim || v3_claim,
            "Normalizer V2 protocol/schema/profile is incompatible");
    requireDecoders(request.decoder_capabilities,
                    "Normalizer V2 decoder capabilities are invalid");
    requireSha256(request.schema_sha256);
    requireSha256(request.resource_profile_sha256);
    require(!request.family_capabilities.empty()
                    && request.family_capabilities.size()
                            <= ProtocolLimits::kMaximumFamilies,
            "Normalizer V2 family capabilities are invalid");
    require(!request.tool_versions.empty()
                    && request.tool_versions.size()
                            <= ProtocolLimits::kMaximumTools,
            "Normalizer V2 tools are invalid");
    Json families = Json::array();
    std::set<std::string> family_ids;
    bool metadata_tuple_declared = false;
    for (const auto& family : request.family_capabilities) {
        const std::string family_key = v3_claim
                ? canonicalFamilyName(family.canonical_family) + '\0'
                        + family.normalization_version + '\0'
                        + family.canonical_contract_version
                : canonicalFamilyName(family.canonical_family);
        require(family_ids.insert(family_key).second,
                "Normalizer V2 family capability is duplicated");
        const bool legacy_tuple = family.normalization_version
                             == normalizationVersion(family.canonical_family)
                && family.canonical_contract_version
                                 == canonicalContractVersion(
                                         family.canonical_family);
        const bool metadata_tuple = v3::isMetadataTuple(family);
        require(v2_claim ? legacy_tuple : legacy_tuple || metadata_tuple,
                "Normalizer V2 family version is incompatible");
        metadata_tuple_declared = metadata_tuple_declared || metadata_tuple;
        requireText(family.validator_name,
                    "Normalizer V2 validator name is invalid");
        requireText(family.validator_version,
                    "Normalizer V2 validator version is invalid");
        requireSha256(family.validator_build_sha256);
        require(family.maximum_input_bytes > 0U
                        && family.maximum_output_bytes > 0U,
                "Normalizer V2 family limits are invalid");
        families.push_back(familyJson(family));
    }
    require(!v3_claim || metadata_tuple_declared,
            "Normalizer V3 claim requires a metadata contract tuple");
    Json tools = Json::array();
    std::set<std::string> tool_ids;
    for (const auto& tool : request.tool_versions) {
        requireText(tool.name, "Normalizer V2 tool name is invalid");
        requireText(tool.version, "Normalizer V2 tool version is invalid");
        requireSha256(tool.build_sha256);
        require(tool_ids.insert(tool.name + '\0' + tool.version + '\0'
                                + tool.build_sha256)
                        .second,
                "Normalizer V2 tool is duplicated");
        tools.push_back(toolJson(tool));
    }
    for (const auto& family : request.family_capabilities) {
        const bool validator_declared = std::any_of(
                request.tool_versions.begin(), request.tool_versions.end(),
                [&family](const ToolVersion& tool) {
                    return tool.name == family.validator_name
                            && tool.version == family.validator_version
                            && tool.build_sha256
                                    == family.validator_build_sha256;
                });
        require(validator_declared,
                "Normalizer V2 family validator is absent from tool versions");
    }
    return Json{{"messageType", "CLAIM_REQUEST"},
                {"workerId", request.worker_id},
                {"protocolVersion", request.protocol_version},
                {"schemaSha256", request.schema_sha256},
                {"decoderCapabilities", request.decoder_capabilities},
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
                       "tileContentId", "rootObjectId", "sourceClosureHash",
                       "resourceClosureVersion", "normalizationVersion",
                       "canonicalFamily", "canonicalContractVersion",
                       "resourceProfileVersion", "resourceProfileSha256",
                       "requiredDecoders",
                       "maximumOutputs", "resourceManifest"});
    require(required<std::string>(json, "messageType") == "CLAIM_RESPONSE",
            "Normalizer V2 claim message type is invalid");
    ClaimTask result;
    result.task_id = required<std::string>(json, "taskId");
    result.attempt_id = required<std::string>(json, "attemptId");
    result.request_id = required<std::string>(json, "requestId");
    result.lease_token = required<std::string>(json, "leaseToken");
    result.lease_expire_time = required<std::string>(json, "leaseExpireTime");
    result.hard_deadline_time = required<std::string>(json, "hardDeadlineTime");
    result.tile_content_id = required<std::string>(json, "tileContentId");
    result.root_object_id = required<std::string>(json, "rootObjectId");
    result.source_closure_hash = required<std::string>(json, "sourceClosureHash");
    result.resource_closure_version =
            required<std::string>(json, "resourceClosureVersion");
    result.normalization_version =
            required<std::string>(json, "normalizationVersion");
    result.canonical_family = parseEnum(
            required<std::string>(json, "canonicalFamily"), kFamilies);
    result.canonical_contract_version =
            required<std::string>(json, "canonicalContractVersion");
    result.resource_profile_version =
            required<std::string>(json, "resourceProfileVersion");
    result.resource_profile_sha256 =
            required<std::string>(json, "resourceProfileSha256");
    result.required_decoders = required<std::vector<std::string>>(
            json, "requiredDecoders");
    requireDecoders(result.required_decoders,
                    "Normalizer V2 required decoders are invalid");
    result.maximum_outputs = required<std::uint64_t>(json, "maximumOutputs");
    result.resource_manifest = parseDescriptor(
            required<Json>(json, "resourceManifest"));
    return result;
}

std::string serializeHeartbeatRequest(
        const std::string& worker_id, const ClaimTask& task, TaskPhase phase,
        std::uint64_t total_resources, std::uint64_t processed_resources) {
    require(processed_resources <= total_resources && total_resources > 0U,
            "Normalizer V2 heartbeat progress is invalid");
    return Json{{"messageType", "HEARTBEAT_REQUEST"},
                {"workerId", worker_id},
                {"leaseToken", task.lease_token},
                {"requestId", task.request_id},
                {"phase", enumName(phase, kTaskPhases)},
                {"processedResources", processed_resources},
                {"totalResources", total_resources}}
            .dump();
}

std::string serializeOutputPrepareRequest(
        const std::string& worker_id, const ClaimTask& task,
        const OutputDeclaration& declaration) {
    return Json{{"messageType", "OUTPUT_PREPARE_REQUEST"},
                {"workerId", worker_id},
                {"leaseToken", task.lease_token},
                {"requestId", task.request_id},
                {"outputId", declaration.output_id},
                {"ordinalPath", declaration.ordinal_path},
                {"outputKind", outputKindName(declaration.output_kind)},
                {"canonicalFamily",
                 canonicalFamilyName(declaration.canonical_family)},
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

OutputGrant parseOutputGrant(const std::string& text) {
    const Json json = parseObject(text);
    requireOnly(json, {"messageType", "outputId", "uploadGrantId",
                       "httpMethod", "uploadUrl", "expiresAt"});
    require(required<std::string>(json, "messageType")
                    == "OUTPUT_PREPARE_RESPONSE",
            "Normalizer V2 output grant message type is invalid");
    OutputGrant result;
    result.output_id = required<std::string>(json, "outputId");
    result.upload_grant_id = required<std::string>(json, "uploadGrantId");
    result.http_method = required<std::string>(json, "httpMethod");
    result.upload_url = required<std::string>(json, "uploadUrl");
    result.expires_at = required<std::string>(json, "expiresAt");
    requireSha256(result.output_id);
    requireSha256(result.upload_grant_id);
    require(result.http_method == "PUT", "Normalizer V2 grant must use PUT");
    requireText(result.upload_url, "Normalizer V2 upload URL is invalid",
                ProtocolLimits::kMaximumGrantUrlBytes);
    require(result.upload_url.rfind("http://", 0U) == 0U
                    || result.upload_url.rfind("https://", 0U) == 0U,
            "Normalizer V2 upload URL must use HTTP(S)");
    requireUtc(result.expires_at);
    return result;
}

std::string serializeOutputReportRequest(
        const std::string& worker_id, const ClaimTask& task,
        const OutputReport& report) {
    return Json{{"messageType", "OUTPUT_REPORT_REQUEST"},
                {"workerId", worker_id},
                {"leaseToken", task.lease_token},
                {"requestId", task.request_id},
                {"outputId", report.output_id},
                {"outputEtag", report.output_etag},
                {"outputSize", report.output_size},
                {"outputSha256", report.output_sha256}}
            .dump();
}

std::string serializeCompleteRequest(
        const std::string& worker_id, const ClaimTask& task,
        const Completion& completion) {
    return Json{{"messageType", "COMPLETE_REQUEST"},
                {"workerId", worker_id},
                {"leaseToken", task.lease_token},
                {"requestId", task.request_id},
                {"orderedOutputIds", completion.ordered_output_ids},
                {"outputManifestSha256", completion.output_manifest_sha256},
                {"globalPreviewOnly", completion.global_preview_only}}
            .dump();
}

std::string serializeFailRequest(const std::string& worker_id,
                                 const ClaimTask& task,
                                 const Failure& failure) {
    normalization::ClaimTask legacy = legacyTask(task);
    return normalization::serializeFailRequest(worker_id, legacy, failure);
}

void validateClaimTask(const ClaimTask& task,
                       const ClaimRequest& capabilities,
                       const std::string& now_utc) {
    requireText(task.task_id, "Normalizer V2 task ID is invalid");
    requireText(task.attempt_id, "Normalizer V2 attempt ID is invalid");
    requireText(task.request_id, "Normalizer V2 request ID is invalid");
    requireText(task.lease_token, "Normalizer V2 lease token is invalid");
    requireText(task.tile_content_id, "Normalizer V2 content ID is invalid");
    requireText(task.root_object_id, "Normalizer V2 root object ID is invalid");
    requireSha256(task.source_closure_hash);
    requireSha256(task.resource_profile_sha256);
    requireUtc(now_utc);
    requireUtc(task.lease_expire_time);
    requireUtc(task.hard_deadline_time);
    require(inspection::utcEpochSeconds(now_utc)
                            < inspection::utcEpochSeconds(task.lease_expire_time)
                    && inspection::utcEpochSeconds(task.lease_expire_time)
                            <= inspection::utcEpochSeconds(
                                    task.hard_deadline_time),
            "Normalizer V2 claim lease window is invalid");
    const FamilyCapability* capability = findCapability(
            capabilities, task.canonical_family,
            task.normalization_version, task.canonical_contract_version);
    require(capability != nullptr
                    && task.normalization_version
                            == capability->normalization_version
                    && task.canonical_contract_version
                            == capability->canonical_contract_version
                    && task.resource_profile_version
                            == capabilities.resource_profile_version
                    && task.resource_profile_sha256
                            == capabilities.resource_profile_sha256
                    && std::includes(capabilities.decoder_capabilities.begin(),
                                     capabilities.decoder_capabilities.end(),
                                     task.required_decoders.begin(),
                                     task.required_decoders.end())
                    && task.maximum_outputs > 0U
                    && task.maximum_outputs <= Limits::kMaximumOutputs,
            "Normalizer V2 claim is incompatible with capabilities");
}

void validateResourceManifestPage(const ResourceManifestPage& page,
                                  const ClaimTask& task,
                                  const std::string& now_utc) {
    normalization::validateResourceManifestPage(
            page, legacyTask(task), now_utc);
}

void validateResourceManifest(const ClaimTask& task,
                              const std::vector<ResourceManifestPage>& pages,
                              const std::string& now_utc,
                              std::uint64_t maximum_input_bytes) {
    normalization::validateResourceManifest(
            legacyTask(task), pages, now_utc, maximum_input_bytes);
    const bool root_present = std::any_of(
            pages.begin(), pages.end(), [&task](const auto& page) {
                return std::any_of(page.records.begin(), page.records.end(),
                                   [&task](const ResourceRecord& record) {
                                       return record.object_id
                                               == task.root_object_id;
                                   });
            });
    require(root_present, "Normalizer V2 root object is absent from manifest");
}

void validateOutputDeclaration(const OutputDeclaration& declaration,
                               const ClaimTask& task,
                               const ClaimRequest& capabilities) {
    require(declaration.ordinal_path.size() <= Limits::kMaximumOrdinalDepth
                    && std::all_of(declaration.ordinal_path.begin(),
                                   declaration.ordinal_path.end(),
                                   [](std::uint32_t) { return true; }),
            "Normalizer V2 ordinal path is invalid");
    require(declaration.output_size > 0U,
            "Normalizer V2 output size is invalid");
    requireSha256(declaration.output_id);
    requireSha256(declaration.output_sha256);
    requireSha256(declaration.semantic_hash);
    requireSha256(declaration.validation_manifest_sha256);
    require(declaration.canonical_contract_version
                    == outputContractVersion(task,
                                             declaration.canonical_family),
            "Normalizer V2 output contract is incompatible");
    if (declaration.output_kind == OutputKind::parent_manifest) {
        require(task.canonical_family == CanonicalFamily::composite_children
                        && declaration.canonical_family
                                == CanonicalFamily::composite_children
                        && declaration.ordinal_path.empty(),
                "Normalizer V2 parent output is invalid");
    } else {
        require(!declaration.ordinal_path.empty()
                        && declaration.canonical_family
                                != CanonicalFamily::composite_children,
                "Normalizer V2 content output is invalid");
        if (task.canonical_family == CanonicalFamily::mesh_gltf2) {
            require(declaration.canonical_family
                            == CanonicalFamily::mesh_gltf2,
                    "Normalizer V2 mesh output family is invalid");
        } else if (task.canonical_family == CanonicalFamily::point_gltf2) {
            require(declaration.canonical_family
                            == CanonicalFamily::point_gltf2,
                    "Normalizer V2 point output family is invalid");
        } else if (task.canonical_family == CanonicalFamily::instance_gltf2) {
            require(declaration.canonical_family
                                    == CanonicalFamily::instance_gltf2
                            || declaration.canonical_family
                                    == CanonicalFamily::mesh_gltf2,
                    "Normalizer V2 instance output family is invalid");
        } else {
            require(task.canonical_family
                            == CanonicalFamily::composite_children,
                    "Normalizer V2 task family cannot emit this output");
        }
    }
    const FamilyCapability* capability = findOutputCapability(
            capabilities, declaration.canonical_family,
            declaration.canonical_contract_version);
    require(capability != nullptr
                    && declaration.output_size
                            <= capability->maximum_output_bytes,
            "Normalizer V2 output exceeds family capability");
    validateSummary(declaration.validation_summary,
                    declaration.canonical_family, *capability,
                    declaration.output_kind);
    require(declaration.validation_manifest_sha256
                    == validationManifestSha256(
                            declaration.validation_summary),
            "Normalizer V2 validation manifest hash is inconsistent");
    require(declaration.output_id == outputId(task, declaration),
            "Normalizer V2 output ID is inconsistent");
}

ValidationSummary contentValidationSummary(
        const CanonicalArtifactEvidence& evidence, CanonicalFamily family,
        std::optional<std::uint64_t> point_count,
        std::optional<std::uint64_t> instance_count,
        std::optional<std::uint64_t> boundary_expanded_instance_count) {
    ValidationSummary result;
    result.coordinate_basis = evidence.validation_summary.coordinate_basis;
    result.scene_count = evidence.validation_summary.scene_count;
    result.node_count = evidence.validation_summary.node_count;
    result.primitive_count = evidence.validation_summary.primitive_count;
    result.accessor_count = evidence.validation_summary.accessor_count;
    result.buffer_count = evidence.validation_summary.buffer_count;
    result.image_count = evidence.validation_summary.image_count;
    result.feature_count = evidence.validation_summary.feature_count;
    result.metadata_property_count =
            evidence.validation_summary.metadata_property_count;
    result.feature_identity_model = identityName(
            evidence.validation_summary.feature_identity_model);
    result.required_extensions = evidence.validation_summary.required_extensions;
    result.used_extensions = evidence.validation_summary.used_extensions;
    result.validator_name = evidence.validation_summary.validator_name;
    result.validator_version = evidence.validation_summary.validator_version;
    result.validator_build_sha256 =
            evidence.validation_summary.validator_build_sha256;
    if (family == CanonicalFamily::point_gltf2) {
        result.point_count = point_count;
    }
    if (family == CanonicalFamily::instance_gltf2) {
        result.instance_count = instance_count;
        result.boundary_expanded_instance_count =
                boundary_expanded_instance_count.value_or(0U);
    }
    result.validation_hash = sha256Hex(
            validationSummaryJson(result, false).dump());
    return result;
}

ValidationSummary parentValidationSummary(std::uint64_t child_count,
                                          const ToolVersion& validator) {
    require(child_count > 0U, "Composite child count must be positive");
    ValidationSummary result;
    result.composite_child_count = child_count;
    result.validator_name = validator.name;
    result.validator_version = validator.version;
    result.validator_build_sha256 = validator.build_sha256;
    result.validation_hash = sha256Hex(
            validationSummaryJson(result, false).dump());
    return result;
}

std::string validationManifestSha256(const ValidationSummary& summary) {
    return sha256Hex(validationSummaryJson(summary, true).dump());
}

std::string outputId(const ClaimTask& task,
                     const OutputDeclaration& declaration) {
    const std::string ordinal = Json(declaration.ordinal_path).dump();
    return sha256Hex(std::string(kOutputIdDomain) + '\0' + task.attempt_id
                     + '\0' + ordinal + '\0'
                     + outputKindName(declaration.output_kind) + '\0'
                     + canonicalFamilyName(declaration.canonical_family) + '\0'
                     + declaration.canonical_contract_version + '\0'
                     + std::to_string(declaration.output_size) + '\0'
                     + declaration.output_sha256 + '\0'
                     + declaration.semantic_hash + '\0'
                     + declaration.validation_manifest_sha256);
}

std::string outputManifestSha256(
        const std::vector<OutputDeclaration>& ordered_outputs) {
    require(!ordered_outputs.empty()
                    && ordered_outputs.size() <= Limits::kMaximumOutputs,
            "Normalizer V2 output set is invalid");
    std::ostringstream material;
    material << kOutputManifestDomain << '\n';
    for (const auto& output : ordered_outputs) {
        material << output.output_id << ':'
                 << Json(output.ordinal_path).dump() << ':'
                 << outputKindName(output.output_kind) << ':'
                 << canonicalFamilyName(output.canonical_family) << ':'
                 << output.canonical_contract_version << ':'
                 << output.output_size << ':' << output.output_sha256 << ':'
                 << output.semantic_hash << ':'
                 << output.validation_manifest_sha256 << '\n';
    }
    return sha256Hex(material.str());
}

std::string compositeChildId(
        const ClaimTask& task, const std::vector<std::uint32_t>& ordinal_path,
        const std::string& source_kind, const std::string& source_version,
        const std::string& source_sha256) {
    require(!ordinal_path.empty()
                    && ordinal_path.size() <= Limits::kMaximumOrdinalDepth,
            "Composite child ordinal path is invalid");
    requireText(source_kind, "Composite source kind is invalid", 128U);
    requireText(source_version, "Composite source version is invalid", 128U);
    requireSha256(source_sha256);
    return sha256Hex(std::string(kCompositeChildIdDomain) + '\0'
                     + task.tile_content_id + '\0' + task.source_closure_hash
                     + '\0' + Json(ordinal_path).dump() + '\0' + source_kind
                     + '\0' + source_version + '\0' + source_sha256 + '\0'
                     + task.canonical_contract_version);
}

}  // namespace clip_worker::normalization::v2
