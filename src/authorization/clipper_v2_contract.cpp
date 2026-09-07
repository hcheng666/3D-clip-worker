#include "clip_worker/authorization/clipper_v2_contract.hpp"

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/inspection/inspection_contract.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace clip_worker::authorization::v2 {
namespace {

using Json = nlohmann::json;

constexpr const char* kClaimRequest = "CLIPPER_V2_CLAIM_REQUEST";
constexpr const char* kClaimResponse = "CLIPPER_V2_CLAIM_RESPONSE";
constexpr const char* kHeartbeatRequest = "CLIPPER_V2_HEARTBEAT_REQUEST";
constexpr const char* kOutputPrepareRequest =
        "CLIPPER_V2_OUTPUT_PREPARE_REQUEST";
constexpr const char* kOutputPrepareResponse =
        "CLIPPER_V2_OUTPUT_PREPARE_RESPONSE";
constexpr const char* kOutputReportRequest =
        "CLIPPER_V2_OUTPUT_REPORT_REQUEST";
constexpr const char* kCompleteRequest = "CLIPPER_V2_COMPLETE_REQUEST";
constexpr const char* kFailRequest = "CLIPPER_V2_FAIL_REQUEST";
constexpr const char* kOutputKind = "CLIPPED_CANONICAL";

template <typename T>
T required(const Json& json, const char* field) {
    const auto item = json.find(field);
    if (item == json.end() || item->is_null()) {
        throw std::invalid_argument(
                std::string("Clipper V2 field is missing: ") + field);
    }
    try {
        return item->get<T>();
    } catch (const Json::exception&) {
        throw std::invalid_argument(
                std::string("Clipper V2 field type is invalid: ") + field);
    }
}

void requireNotBlank(const std::string& value, const char* field) {
    if (value.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::invalid_argument(
                std::string("Clipper V2 field is blank: ") + field);
    }
}

void requireHash(const std::string& value, const char* field) {
    if (value.size() != 64U
            || value.find_first_not_of("0123456789abcdef")
                    != std::string::npos) {
        throw std::invalid_argument(
                std::string("Clipper V2 SHA-256 is invalid: ") + field);
    }
}

CanonicalFamily parseFamily(const std::string& value) {
    if (value == "MESH_GLTF2") return CanonicalFamily::mesh_gltf2;
    if (value == "POINT_GLTF2") return CanonicalFamily::point_gltf2;
    if (value == "INSTANCE_GLTF2") return CanonicalFamily::instance_gltf2;
    throw std::invalid_argument("Clipper V2 canonical family is unsupported");
}

Json leaseJson(const std::string& message_type,
               const std::string& worker_id, const ClaimTask& task) {
    requireNotBlank(worker_id, "workerId");
    requireNotBlank(task.work_item_id, "workItemId");
    requireNotBlank(task.attempt_id, "attemptId");
    requireNotBlank(task.request_id, "requestId");
    requireNotBlank(task.lease_token, "leaseToken");
    return {{"messageType", message_type},
            {"workerId", worker_id},
            {"attemptId", task.attempt_id},
            {"requestId", task.request_id},
            {"leaseToken", task.lease_token}};
}

Json proofJson(const AuthorizationClipProof& proof) {
    validateAuthorizationClipProof(proof);
    return {{"proofContractVersion", proof.proof_contract_version},
            {"inputSha256", proof.input_sha256},
            {"inputSemanticHash", proof.input_semantic_hash},
            {"inputValidationManifestSha256",
             proof.input_validation_manifest_sha256},
            {"scopeHash", proof.scope_hash},
            {"transformHash", proof.transform_hash},
            {"canonicalFamily", proof.canonical_family},
            {"canonicalContractVersion",
             proof.canonical_contract_version},
            {"clipStrategyVersion", proof.clip_strategy_version},
            {"sourceOrdinalPath", proof.source_ordinal_path},
            {"exactClassifier",
             {{"relation", clip::authorizationContentRelationName(
                                       proof.classifier.relation)},
              {"classifierName", proof.classifier.classifier_name},
              {"classifierVersion", proof.classifier.classifier_version},
              {"inputElementCount",
               proof.classifier.input_element_count},
              {"wholeElementCount",
               proof.classifier.whole_element_count},
              {"disjointElementCount",
               proof.classifier.disjoint_element_count},
              {"boundaryElementCount",
               proof.classifier.boundary_element_count}}},
            {"proofHash", proof.proof_hash}};
}

const char* featureIdentityModelName(
        normalization::FeatureIdentityModel model) noexcept {
    switch (model) {
        case normalization::FeatureIdentityModel::none: return "NONE";
        case normalization::FeatureIdentityModel::legacy_batch_table_mapped:
            return "LEGACY_BATCH_TABLE_MAPPED";
        case normalization::FeatureIdentityModel::attribute_feature_id_property_table:
            return "ATTRIBUTE_FEATURE_ID_PROPERTY_TABLE";
        case normalization::FeatureIdentityModel::point_feature_id:
            return "POINT_FEATURE_ID";
        case normalization::FeatureIdentityModel::instance_feature_id:
            return "INSTANCE_FEATURE_ID";
    }
    return "NONE";
}

Json validationSummaryJson(
        const normalization::ValidationSummary& summary,
        bool include_validation_hash = true) {
    Json json = {{"coordinateBasis", summary.coordinate_basis},
            {"sceneCount", summary.scene_count},
            {"nodeCount", summary.node_count},
            {"primitiveCount", summary.primitive_count},
            {"accessorCount", summary.accessor_count},
            {"bufferCount", summary.buffer_count},
            {"imageCount", summary.image_count},
            {"featureCount", summary.feature_count},
            {"metadataPropertyCount", summary.metadata_property_count},
            {"featureIdentityModel",
             featureIdentityModelName(summary.feature_identity_model)},
            {"requiredExtensions", summary.required_extensions},
            {"usedExtensions", summary.used_extensions},
            {"validatorName", summary.validator_name},
            {"validatorVersion", summary.validator_version},
            {"validatorBuildSha256", summary.validator_build_sha256}};
    if (include_validation_hash) {
        json["validationHash"] = summary.validation_hash;
    }
    return json;
}

void validateValidationSummary(
        const normalization::CanonicalArtifactEvidence& evidence) {
    const auto& summary = evidence.validation_summary;
    requireNotBlank(summary.validator_name, "validatorName");
    requireNotBlank(summary.validator_version, "validatorVersion");
    requireHash(summary.validator_build_sha256, "validatorBuildSha256");
    requireHash(summary.validation_hash, "validationHash");
    requireHash(evidence.validation_manifest_sha256,
                "validationManifestSha256");
    if (summary.coordinate_basis != kCoordinateBasis
            || summary.scene_count == 0U || summary.node_count == 0U
            || summary.primitive_count == 0U
            || summary.accessor_count == 0U
            || summary.buffer_count == 0U
            || !std::is_sorted(summary.required_extensions.begin(),
                               summary.required_extensions.end())
            || std::adjacent_find(summary.required_extensions.begin(),
                                  summary.required_extensions.end())
                    != summary.required_extensions.end()
            || !std::is_sorted(summary.used_extensions.begin(),
                               summary.used_extensions.end())
            || std::adjacent_find(summary.used_extensions.begin(),
                                  summary.used_extensions.end())
                    != summary.used_extensions.end()
            || !std::includes(summary.used_extensions.begin(),
                              summary.used_extensions.end(),
                              summary.required_extensions.begin(),
                              summary.required_extensions.end())
            || summary.validation_hash
                    != normalization::sha256Hex(
                            validationSummaryJson(summary, false).dump())
            || evidence.validation_manifest_sha256
                    != normalization::sha256Hex(
                            validationSummaryJson(summary, true).dump())) {
        throw std::invalid_argument(
                "Clipper V2 validation summary is invalid");
    }
}

const FamilyCapability* matchingCapability(
        const ClaimRequest& request, const ClaimTask& task) {
    const auto found = std::find_if(
            request.family_capabilities.begin(),
            request.family_capabilities.end(),
            [&task](const FamilyCapability& capability) {
                return capability.canonical_family == task.canonical_family
                        && capability.clip_contract_version
                                == kProtocolVersion
                        && capability.canonical_contract_version
                                == task.canonical_contract_version
                        && capability.clip_strategy_version
                                == task.clip_strategy_version;
            });
    return found == request.family_capabilities.end() ? nullptr : &*found;
}

}  // namespace

const char* canonicalFamilyName(CanonicalFamily family) noexcept {
    switch (family) {
        case CanonicalFamily::mesh_gltf2: return "MESH_GLTF2";
        case CanonicalFamily::point_gltf2: return "POINT_GLTF2";
        case CanonicalFamily::instance_gltf2: return "INSTANCE_GLTF2";
    }
    return "MESH_GLTF2";
}

const char* completionOutcomeName(CompletionOutcome outcome) noexcept {
    switch (outcome) {
        case CompletionOutcome::empty: return "EMPTY";
        case CompletionOutcome::safe_whole: return "SAFE_WHOLE";
        case CompletionOutcome::clipped: return "CLIPPED";
    }
    return "EMPTY";
}

const char* taskPhaseName(TaskPhase phase) noexcept {
    switch (phase) {
        case TaskPhase::claimed: return "CLAIMED";
        case TaskPhase::downloading: return "DOWNLOADING";
        case TaskPhase::verifying: return "VERIFYING";
        case TaskPhase::classifying: return "CLASSIFYING";
        case TaskPhase::clipping: return "CLIPPING";
        case TaskPhase::validating_output: return "VALIDATING_OUTPUT";
        case TaskPhase::uploading: return "UPLOADING";
        case TaskPhase::completing: return "COMPLETING";
    }
    return "CLAIMED";
}

const char* failureCodeName(FailureCode code) noexcept {
    switch (code) {
        case FailureCode::download_failed: return "DOWNLOAD_FAILED";
        case FailureCode::input_identity_mismatch:
            return "INPUT_IDENTITY_MISMATCH";
        case FailureCode::input_validation_failed:
            return "INPUT_VALIDATION_FAILED";
        case FailureCode::classification_failed:
            return "CLASSIFICATION_FAILED";
        case FailureCode::clip_failed: return "CLIP_FAILED";
        case FailureCode::output_validation_failed:
            return "OUTPUT_VALIDATION_FAILED";
        case FailureCode::output_upload_failed:
            return "OUTPUT_UPLOAD_FAILED";
        case FailureCode::resource_limit_exceeded:
            return "RESOURCE_LIMIT_EXCEEDED";
        case FailureCode::lease_expired: return "LEASE_EXPIRED";
        case FailureCode::attempts_exhausted: return "ATTEMPTS_EXHAUSTED";
        case FailureCode::publication_identity_mismatch:
            return "PUBLICATION_IDENTITY_MISMATCH";
        case FailureCode::internal_error: return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

std::string clippedOutputId(
        const std::string& work_item_id, std::uint32_t output_ordinal) {
    requireNotBlank(work_item_id, "workItemId");
    return normalization::sha256Hex(
            std::string(kClippedOutputIdDomain) + '\0' + work_item_id
            + '\0' + "CLIPPED" + '\0' + std::to_string(output_ordinal));
}

std::string outputManifestSha256(
        const std::vector<OutputDeclaration>& ordered_outputs) {
    if (ordered_outputs.empty()
            || ordered_outputs.size() > Limits::kMaximumOutputs) {
        throw std::invalid_argument(
                "Clipper V2 output manifest size is invalid");
    }
    std::string material = std::string(kOutputManifestHashDomain) + '\n';
    std::set<std::string> ids;
    std::optional<std::uint32_t> previous_ordinal;
    for (const auto& output : ordered_outputs) {
        requireNotBlank(output.output_id, "outputId");
        requireNotBlank(output.canonical_contract_version,
                        "canonicalContractVersion");
        requireHash(output.evidence.output_sha256, "outputSha256");
        requireHash(output.evidence.semantic_hash, "semanticHash");
        requireHash(output.evidence.validation_manifest_sha256,
                    "validationManifestSha256");
        validateValidationSummary(output.evidence);
        if (!ids.insert(output.output_id).second
                || (previous_ordinal.has_value()
                    && output.output_ordinal <= *previous_ordinal)) {
            throw std::invalid_argument(
                    "Clipper V2 output manifest ordering is invalid");
        }
        previous_ordinal = output.output_ordinal;
        material += output.output_id + ':'
                + std::to_string(output.output_ordinal) + ':' + kOutputKind
                + ':' + canonicalFamilyName(output.canonical_family) + ':'
                + output.canonical_contract_version + ':'
                + std::to_string(output.evidence.output_size) + ':'
                + output.evidence.output_sha256 + ':'
                + output.evidence.semantic_hash + ':'
                + output.evidence.validation_manifest_sha256 + '\n';
    }
    return normalization::sha256Hex(material);
}

std::string serializeClaimRequest(const ClaimRequest& request) {
    requireNotBlank(request.worker_id, "workerId");
    if (request.protocol_version != kProtocolVersion
            || request.schema_sha256 != kSchemaSha256
            || request.resource_profile_version != kResourceProfileVersion
            || request.family_capabilities.empty()
            || request.family_capabilities.size()
                    > Limits::kMaximumFamilyCapabilities) {
        throw std::invalid_argument("Clipper V2 claim identity is invalid");
    }
    requireHash(request.schema_sha256, "schemaSha256");
    requireHash(request.resource_profile_sha256, "resourceProfileSha256");
    Json capabilities = Json::array();
    std::set<std::string> identities;
    for (const auto& capability : request.family_capabilities) {
        requireNotBlank(capability.canonical_contract_version,
                        "canonicalContractVersion");
        requireNotBlank(capability.clip_strategy_version,
                        "clipStrategyVersion");
        requireNotBlank(capability.validator_name, "validatorName");
        requireNotBlank(capability.validator_version, "validatorVersion");
        requireHash(capability.validator_build_sha256,
                    "validatorBuildSha256");
        if (capability.clip_contract_version != kProtocolVersion
                || capability.maximum_input_bytes == 0U
                || capability.maximum_output_bytes == 0U) {
            throw std::invalid_argument(
                    "Clipper V2 family capability is invalid");
        }
        const std::string identity =
                std::string(canonicalFamilyName(capability.canonical_family))
                + '\0' + capability.canonical_contract_version + '\0'
                + capability.clip_strategy_version;
        if (!identities.insert(identity).second) {
            throw std::invalid_argument(
                    "Clipper V2 family capability is duplicated");
        }
        capabilities.push_back(
                {{"clipContractVersion",
                  capability.clip_contract_version},
                 {"canonicalFamily",
                  canonicalFamilyName(capability.canonical_family)},
                 {"canonicalContractVersion",
                  capability.canonical_contract_version},
                 {"clipStrategyVersion",
                  capability.clip_strategy_version},
                 {"validatorName", capability.validator_name},
                 {"validatorVersion", capability.validator_version},
                 {"validatorBuildSha256",
                  capability.validator_build_sha256},
                 {"maximumInputBytes",
                  capability.maximum_input_bytes},
                 {"maximumOutputBytes",
                  capability.maximum_output_bytes}});
    }
    return Json{{"messageType", kClaimRequest},
                {"workerId", request.worker_id},
                {"protocolVersion", request.protocol_version},
                {"schemaSha256", request.schema_sha256},
                {"resourceProfileVersion",
                 request.resource_profile_version},
                {"resourceProfileSha256",
                 request.resource_profile_sha256},
                {"familyCapabilities", std::move(capabilities)}}
            .dump();
}

ClaimTask parseClaimTask(const std::string& text) {
    Json json;
    try {
        json = Json::parse(text);
    } catch (const Json::exception&) {
        throw std::invalid_argument("Clipper V2 claim response JSON is invalid");
    }
    if (!json.is_object()
            || required<std::string>(json, "messageType") != kClaimResponse) {
        throw std::invalid_argument("Clipper V2 claim response type is invalid");
    }
    ClaimTask task;
    task.work_item_id = required<std::string>(json, "workItemId");
    task.attempt_id = required<std::string>(json, "attemptId");
    task.request_id = required<std::string>(json, "requestId");
    task.lease_token = required<std::string>(json, "leaseToken");
    task.lease_expire_time = required<std::string>(json, "leaseExpireTime");
    task.hard_deadline_time = required<std::string>(json, "hardDeadlineTime");
    task.tile_content_id = required<std::string>(json, "tileContentId");
    task.source_ordinal_path =
            required<std::vector<std::uint32_t>>(json, "sourceOrdinalPath");
    task.source_artifact_kind =
            required<std::string>(json, "sourceArtifactKind");
    task.source_artifact_id =
            required<std::string>(json, "sourceArtifactId");
    task.canonical_family = parseFamily(
            required<std::string>(json, "canonicalFamily"));
    task.canonical_contract_version =
            required<std::string>(json, "canonicalContractVersion");
    task.clip_strategy_version =
            required<std::string>(json, "clipStrategyVersion");
    task.input_size = required<std::uint64_t>(json, "inputSize");
    task.input_sha256 = required<std::string>(json, "inputSha256");
    task.input_semantic_hash =
            required<std::string>(json, "inputSemanticHash");
    task.input_validation_manifest_sha256 = required<std::string>(
            json, "inputValidationManifestSha256");
    const Json grant = required<Json>(json, "inputGrant");
    task.input_grant.grant_id = required<std::string>(grant, "grantId");
    task.input_grant.http_method = required<std::string>(grant, "httpMethod");
    task.input_grant.url = required<std::string>(grant, "url");
    task.input_grant.expires_at = required<std::string>(grant, "expiresAt");
    task.scope_wkb_base64 = required<std::string>(json, "scopeWkbBase64");
    task.scope_srid = required<std::int32_t>(json, "scopeSrid");
    task.scope_hash = required<std::string>(json, "scopeHash");
    const auto transform =
            required<std::vector<double>>(json, "accumulatedTransform");
    if (transform.size() != Limits::kTransformElements
            || std::any_of(transform.begin(), transform.end(),
                           [](double value) { return !std::isfinite(value); })) {
        throw std::invalid_argument(
                "Clipper V2 accumulatedTransform is invalid");
    }
    std::copy(transform.begin(), transform.end(),
              task.accumulated_transform.begin());
    task.transform_hash = required<std::string>(json, "transformHash");
    task.canonical_coordinate_basis =
            required<std::string>(json, "canonicalCoordinateBasis");
    task.resource_profile_version =
            required<std::string>(json, "resourceProfileVersion");
    task.resource_profile_sha256 =
            required<std::string>(json, "resourceProfileSha256");
    const Json limits = required<Json>(json, "limits");
    task.limits.maximum_input_bytes =
            required<std::uint64_t>(limits, "maximumInputBytes");
    task.limits.maximum_output_bytes =
            required<std::uint64_t>(limits, "maximumOutputBytes");
    task.limits.maximum_aggregate_output_bytes =
            required<std::uint64_t>(limits, "maximumAggregateOutputBytes");
    task.limits.maximum_points =
            required<std::uint64_t>(limits, "maximumPoints");
    task.limits.maximum_instances =
            required<std::uint64_t>(limits, "maximumInstances");
    task.limits.maximum_boundary_instances =
            required<std::uint64_t>(limits, "maximumBoundaryInstances");
    task.limits.maximum_expanded_vertices =
            required<std::uint64_t>(limits, "maximumExpandedVertices");
    task.limits.maximum_expanded_indices =
            required<std::uint64_t>(limits, "maximumExpandedIndices");
    task.limits.maximum_outputs =
            required<std::uint32_t>(limits, "maximumOutputs");
    return task;
}

std::string serializeHeartbeatRequest(
        const std::string& worker_id, const ClaimTask& task, TaskPhase phase,
        std::uint64_t processed_bytes) {
    Json json = leaseJson(kHeartbeatRequest, worker_id, task);
    json["phase"] = taskPhaseName(phase);
    json["processedBytes"] = processed_bytes;
    return json.dump();
}

std::string serializeOutputPrepareRequest(
        const std::string& worker_id, const ClaimTask& task,
        const OutputDeclaration& declaration) {
    validateOutputDeclaration(declaration, task);
    Json json = leaseJson(kOutputPrepareRequest, worker_id, task);
    json["output"] =
            {{"outputId", declaration.output_id},
             {"outputOrdinal", declaration.output_ordinal},
             {"outputKind", kOutputKind},
             {"canonicalFamily",
              canonicalFamilyName(declaration.canonical_family)},
             {"canonicalContractVersion",
              declaration.canonical_contract_version},
             {"outputSize", declaration.evidence.output_size},
             {"outputSha256", declaration.evidence.output_sha256},
             {"semanticHash", declaration.evidence.semantic_hash},
             {"validationManifestSha256",
              declaration.evidence.validation_manifest_sha256},
             {"validationSummary",
              validationSummaryJson(
                      declaration.evidence.validation_summary)}};
    return json.dump();
}

OutputGrant parseOutputGrant(const std::string& text) {
    Json json;
    try {
        json = Json::parse(text);
    } catch (const Json::exception&) {
        throw std::invalid_argument("Clipper V2 output grant JSON is invalid");
    }
    if (required<std::string>(json, "messageType")
            != kOutputPrepareResponse) {
        throw std::invalid_argument("Clipper V2 output grant type is invalid");
    }
    OutputGrant result;
    result.output_id = required<std::string>(json, "outputId");
    result.upload_grant_id =
            required<std::string>(json, "uploadGrantId");
    result.http_method = required<std::string>(json, "httpMethod");
    result.upload_url = required<std::string>(json, "uploadUrl");
    result.expires_at = required<std::string>(json, "expiresAt");
    if (result.http_method != "PUT") {
        throw std::invalid_argument("Clipper V2 output grant method is not PUT");
    }
    return result;
}

std::string serializeOutputReportRequest(
        const std::string& worker_id, const ClaimTask& task,
        const OutputReport& report) {
    Json json = leaseJson(kOutputReportRequest, worker_id, task);
    requireNotBlank(report.output_id, "outputId");
    requireNotBlank(report.output_etag, "outputEtag");
    requireHash(report.output_sha256, "outputSha256");
    if (report.output_size == 0U) {
        throw std::invalid_argument("Clipper V2 outputSize is zero");
    }
    json["outputId"] = report.output_id;
    json["outputEtag"] = report.output_etag;
    json["outputSize"] = report.output_size;
    json["outputSha256"] = report.output_sha256;
    return json.dump();
}

std::string serializeCompleteRequest(
        const std::string& worker_id, const ClaimTask& task,
        const Completion& completion) {
    validateAuthorizationClipProof(completion.proof);
    const auto expected_relation = completion.outcome == CompletionOutcome::empty
            ? clip::AuthorizationContentRelation::empty
            : completion.outcome == CompletionOutcome::safe_whole
                    ? clip::AuthorizationContentRelation::safe_whole
                    : clip::AuthorizationContentRelation::boundary;
    if (completion.proof.classifier.relation != expected_relation) {
        throw std::invalid_argument(
                "Clipper V2 completion outcome differs from exact proof");
    }
    if (completion.outcome == CompletionOutcome::clipped) {
        if (completion.ordered_output_ids.empty()
                || completion.ordered_output_ids.size()
                        > task.limits.maximum_outputs
                || !completion.output_manifest_sha256.has_value()) {
            throw std::invalid_argument(
                    "Clipper V2 CLIPPED completion output set is invalid");
        }
        requireHash(*completion.output_manifest_sha256,
                    "outputManifestSha256");
    } else if (!completion.ordered_output_ids.empty()
               || completion.output_manifest_sha256.has_value()) {
        throw std::invalid_argument(
                "Clipper V2 whole/empty completion contains outputs");
    }
    Json json = leaseJson(kCompleteRequest, worker_id, task);
    json["outcome"] = completionOutcomeName(completion.outcome);
    json["proof"] = proofJson(completion.proof);
    json["orderedOutputIds"] = completion.ordered_output_ids;
    json["outputManifestSha256"] =
            completion.output_manifest_sha256.has_value()
            ? Json(*completion.output_manifest_sha256) : Json(nullptr);
    return json.dump();
}

std::string serializeFailRequest(
        const std::string& worker_id, const ClaimTask& task,
        const Failure& failure) {
    Json json = leaseJson(kFailRequest, worker_id, task);
    requireNotBlank(failure.error_message, "errorMessage");
    json["errorCode"] = failureCodeName(failure.error_code);
    json["errorMessage"] = failure.error_message;
    json["retryable"] = failure.retryable;
    return json.dump();
}

void validateClaimTask(const ClaimTask& task, const ClaimRequest& capability,
                       const std::string& now_utc) {
    requireNotBlank(now_utc, "nowUtc");
    for (const auto* value : {&task.work_item_id, &task.attempt_id,
                              &task.request_id, &task.lease_token,
                              &task.lease_expire_time,
                              &task.hard_deadline_time,
                              &task.tile_content_id,
                              &task.source_artifact_kind,
                              &task.source_artifact_id,
                              &task.canonical_contract_version,
                              &task.clip_strategy_version,
                              &task.scope_wkb_base64,
                              &task.canonical_coordinate_basis}) {
        requireNotBlank(*value, "claimTaskIdentity");
    }
    requireHash(task.input_sha256, "inputSha256");
    requireHash(task.input_semantic_hash, "inputSemanticHash");
    requireHash(task.input_validation_manifest_sha256,
                "inputValidationManifestSha256");
    requireHash(task.scope_hash, "scopeHash");
    requireHash(task.transform_hash, "transformHash");
    requireHash(task.resource_profile_sha256, "resourceProfileSha256");
    const auto now = inspection::utcEpochSeconds(now_utc);
    const auto lease_expiry =
            inspection::utcEpochSeconds(task.lease_expire_time);
    const auto hard_deadline =
            inspection::utcEpochSeconds(task.hard_deadline_time);
    const auto grant_expiry =
            inspection::utcEpochSeconds(task.input_grant.expires_at);
    if (task.source_ordinal_path.size() > Limits::kMaximumOrdinalDepth
            || task.scope_srid != 4490
            || task.canonical_coordinate_basis != kCoordinateBasis
            || task.resource_profile_version
                    != capability.resource_profile_version
            || task.resource_profile_sha256
                    != capability.resource_profile_sha256
            || task.input_grant.http_method != "GET"
            || task.input_grant.url.empty()
            || now >= lease_expiry || lease_expiry > hard_deadline
            || grant_expiry <= now || grant_expiry > lease_expiry
            || task.input_size == 0U
            || task.limits.maximum_input_bytes < task.input_size
            || task.limits.maximum_output_bytes == 0U
            || task.limits.maximum_aggregate_output_bytes == 0U
            || task.limits.maximum_outputs == 0U
            || task.limits.maximum_outputs > Limits::kMaximumOutputs) {
        throw std::invalid_argument("Clipper V2 task envelope is invalid");
    }
    const auto* matched = matchingCapability(capability, task);
    if (matched == nullptr
            || matched->maximum_input_bytes < task.input_size
            || matched->maximum_output_bytes
                    < task.limits.maximum_output_bytes) {
        throw std::invalid_argument(
                "Clipper V2 task is outside advertised capability");
    }
}

void validateOutputDeclaration(const OutputDeclaration& declaration,
                               const ClaimTask& task) {
    requireNotBlank(declaration.output_id, "outputId");
    requireNotBlank(declaration.canonical_contract_version,
                    "canonicalContractVersion");
    requireHash(declaration.evidence.output_sha256, "outputSha256");
    requireHash(declaration.evidence.semantic_hash, "semanticHash");
    requireHash(declaration.evidence.validation_manifest_sha256,
                "validationManifestSha256");
    validateValidationSummary(declaration.evidence);
    const bool direct_slot = declaration.output_ordinal == 0U
            && declaration.canonical_family == task.canonical_family
            && declaration.canonical_contract_version
                    == task.canonical_contract_version;
    const bool instance_boundary_slot =
            task.canonical_family == CanonicalFamily::instance_gltf2
            && declaration.output_ordinal == 1U
            && declaration.canonical_family == CanonicalFamily::mesh_gltf2
            && declaration.canonical_contract_version
                    == normalization::v3::kMeshCanonicalContractVersion;
    if (declaration.output_ordinal >= task.limits.maximum_outputs
            || declaration.evidence.output_size == 0U
            || declaration.evidence.output_size
                    > task.limits.maximum_output_bytes
            || declaration.output_id
                    != clippedOutputId(task.work_item_id,
                                       declaration.output_ordinal)
            || (!direct_slot && !instance_boundary_slot)) {
        throw std::invalid_argument(
                "Clipper V2 output declaration exceeds task limits");
    }
}

}  // namespace clip_worker::authorization::v2
