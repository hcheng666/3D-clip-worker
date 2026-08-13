#include "clip_worker/task/task_contract.hpp"

#include <algorithm>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace clip_worker::task {
namespace {

constexpr const char* kB3dmGltf2 = "B3DM_GLTF2";
constexpr const char* kGltfUpAxisZ = "Z";
constexpr const char* kCompletionReady = "READY";
constexpr const char* kCompletionEmpty = "EMPTY";

template <typename Value>
Value required(const nlohmann::json& json, const char* field) {
    const auto item = json.find(field);
    if (item == json.end() || item->is_null()) {
        throw std::invalid_argument(std::string("Task response is missing field: ") + field);
    }
    try {
        return item->get<Value>();
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("Task response field has invalid type: ")
                                    + field + ": " + error.what());
    }
}

void requireNotBlank(const std::string& value, const char* field) {
    if (value.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::invalid_argument(std::string("Task response field is blank: ") + field);
    }
}

nlohmann::json leaseJson(const LeaseRequest& request) {
    requireNotBlank(request.worker_id, "workerId");
    requireNotBlank(request.lease_token, "leaseToken");
    return {{"workerId", request.worker_id}, {"leaseToken", request.lease_token}};
}

}  // namespace

std::string contentFormatName(ContentFormat value) {
    switch (value) {
        case ContentFormat::b3dm_gltf2:
            return kB3dmGltf2;
    }
    throw std::invalid_argument("Unknown content format enum value");
}

ContentFormat parseContentFormat(const std::string& value) {
    if (value == kB3dmGltf2) {
        return ContentFormat::b3dm_gltf2;
    }
    throw std::invalid_argument("Unsupported task content format: " + value);
}

std::string gltfUpAxisName(GltfUpAxis value) {
    switch (value) {
        case GltfUpAxis::z:
            return kGltfUpAxisZ;
    }
    throw std::invalid_argument("Unknown glTF up axis enum value");
}

GltfUpAxis parseGltfUpAxis(const std::string& value) {
    if (value == kGltfUpAxisZ) {
        return GltfUpAxis::z;
    }
    throw std::invalid_argument("Unsupported task glTF up axis: " + value);
}

std::string completionResultName(CompletionResult value) {
    switch (value) {
        case CompletionResult::ready:
            return kCompletionReady;
        case CompletionResult::empty:
            return kCompletionEmpty;
    }
    throw std::invalid_argument("Unknown completion result enum value");
}

std::string serializeClaimRequest(const ClaimRequest& request) {
    requireNotBlank(request.worker_id, "workerId");
    requireNotBlank(request.algorithm_version, "algorithmVersion");
    if (request.supported_formats.empty()) {
        throw std::invalid_argument("supportedFormats must not be empty");
    }
    if (request.max_input_bytes == 0U) {
        throw std::invalid_argument("maxInputBytes must be greater than zero");
    }

    nlohmann::json formats = nlohmann::json::array();
    for (const auto format : request.supported_formats) {
        formats.push_back(contentFormatName(format));
    }
    return nlohmann::json{{"workerId", request.worker_id},
                          {"supportedFormats", formats},
                          {"algorithmVersion", request.algorithm_version},
                          {"maxInputBytes", request.max_input_bytes}}
            .dump();
}

ClaimTask parseClaimTask(const std::string& response_json) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(response_json);
    } catch (const nlohmann::json::exception&) {
        // Parsing diagnostics may quote fragments of a presigned URL from the response.
        throw std::invalid_argument("Task response JSON is invalid");
    }
    if (!json.is_object()) {
        throw std::invalid_argument("Task response must contain a JSON object");
    }

    ClaimTask task;
    task.asset_id = required<std::string>(json, "assetId");
    task.lease_token = required<std::string>(json, "leaseToken");
    task.lease_expire_time = required<std::string>(json, "leaseExpireTime");
    task.source_download_url = required<std::string>(json, "sourceDownloadUrl");
    task.source_etag = required<std::string>(json, "sourceEtag");
    task.output_upload_url = required<std::string>(json, "outputUploadUrl");
    task.scope_wkb_base64 = required<std::string>(json, "scopeWkbBase64");
    task.scope_srid = required<std::int32_t>(json, "scopeSrid");
    task.gltf_up_axis = parseGltfUpAxis(required<std::string>(json, "gltfUpAxis"));
    task.content_format = parseContentFormat(required<std::string>(json, "contentFormat"));

    requireNotBlank(task.asset_id, "assetId");
    requireNotBlank(task.lease_token, "leaseToken");
    requireNotBlank(task.source_download_url, "sourceDownloadUrl");
    requireNotBlank(task.output_upload_url, "outputUploadUrl");
    requireNotBlank(task.scope_wkb_base64, "scopeWkbBase64");

    const auto transform = required<std::vector<double>>(json, "worldTransform");
    if (transform.size() != ClaimTask::kWorldTransformElementCount) {
        throw std::invalid_argument("worldTransform must contain exactly 16 values");
    }
    std::copy(transform.begin(), transform.end(), task.world_transform.begin());

    const auto options = required<nlohmann::json>(json, "clipOptions");
    if (!options.is_object()) {
        throw std::invalid_argument("clipOptions must contain a JSON object");
    }
    task.clip_options.mask_textures = required<bool>(options, "maskTextures");
    task.clip_options.compact_feature_metadata =
            required<bool>(options, "compactFeatureMetadata");
    task.clip_options.cap_surface = required<bool>(options, "capSurface");
    return task;
}

std::string serializeLeaseRequest(const LeaseRequest& request) {
    return leaseJson(request).dump();
}

std::string serializeCompleteRequest(const CompleteRequest& request) {
    auto json = leaseJson({request.worker_id, request.lease_token});
    json["result"] = completionResultName(request.result);
    if (request.result == CompletionResult::ready) {
        requireNotBlank(request.output_etag, "outputEtag");
        requireNotBlank(request.output_sha256, "outputSha256");
        if (request.output_size == 0U) {
            throw std::invalid_argument("outputSize must be greater than zero for READY");
        }
        json["outputEtag"] = request.output_etag;
        json["outputSha256"] = request.output_sha256;
        json["outputSize"] = request.output_size;
    } else if (!request.output_etag.empty() || !request.output_sha256.empty()
               || request.output_size != 0U) {
        throw std::invalid_argument("EMPTY completion must not contain output object fields");
    }
    json["statistics"] = {
            {"vertexCountBefore", request.statistics.vertex_count_before},
            {"vertexCountAfter", request.statistics.vertex_count_after},
            {"triangleCountBefore", request.statistics.triangle_count_before},
            {"triangleCountAfter", request.statistics.triangle_count_after},
            {"textureBytesBefore", request.statistics.texture_bytes_before},
            {"textureBytesAfter", request.statistics.texture_bytes_after},
            {"costMs", request.statistics.cost_ms}};
    return json.dump();
}

std::string serializeFailRequest(const FailRequest& request) {
    auto json = leaseJson({request.worker_id, request.lease_token});
    requireNotBlank(request.error_code, "errorCode");
    requireNotBlank(request.error_message, "errorMessage");
    json["errorCode"] = request.error_code;
    json["errorMessage"] = request.error_message;
    json["retryable"] = request.retryable;
    return json.dump();
}

}  // namespace clip_worker::task
