#include "clip_worker/client/clipper_v2_api_client.hpp"

#include "clip_worker/client/curl_runtime.hpp"

#include <memory>
#include <utility>

#include <curl/curl.h>

namespace clip_worker::client {
namespace {

constexpr const char* kTaskBasePath =
        "/api/internal/three-d-clip/v2/tasks";
constexpr const char* kInnerSourceHeader = "from-source: inner";
constexpr long kHttpOk = 200;
constexpr long kHttpNoContent = 204;

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string escapePathSegment(const std::string& value) {
    if (value.empty()) {
        throw std::invalid_argument("Clipper V2 work item ID is blank");
    }
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw NormalizationApiError(0, "Clipper V2 path encoding");
    using Escaped = std::unique_ptr<char, decltype(&curl_free)>;
    Escaped escaped(curl_easy_escape(curl.get(), value.c_str(),
                                     static_cast<int>(value.size())),
                    &curl_free);
    if (!escaped) {
        throw NormalizationApiError(0, "Clipper V2 path encoding");
    }
    return escaped.get();
}

}  // namespace

ClipperV2ApiClient::ClipperV2ApiClient(
        NormalizationApiClientConfig config)
    : ClipperV2ApiClient(
              config,
              std::make_shared<CurlNormalizationHttpTransport>(
                      config.connect_timeout_seconds,
                      config.request_timeout_seconds)) {
}

ClipperV2ApiClient::ClipperV2ApiClient(
        NormalizationApiClientConfig config,
        std::shared_ptr<const INormalizationHttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    config_.base_url = trimTrailingSlash(config_.base_url);
    if (config_.base_url.empty() || !transport_) {
        throw std::invalid_argument(
                "Clipper V2 API configuration is incomplete");
    }
    ensureCurlInitialized();
}

std::optional<authorization::v2::ClaimTask> ClipperV2ApiClient::claim(
        const authorization::v2::ClaimRequest& claim_request) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post, std::string(kTaskBasePath) + "/claim",
            authorization::v2::serializeClaimRequest(claim_request)));
    if (response.status_code == kHttpNoContent) return std::nullopt;
    requireSuccess(response, "Clipper V2 claim");
    return authorization::v2::parseClaimTask(response.body);
}

void ClipperV2ApiClient::heartbeat(
        const authorization::v2::ClaimTask& task,
        const std::string& worker_id,
        authorization::v2::TaskPhase phase,
        std::uint64_t processed_bytes) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.work_item_id, "heartbeat"),
            authorization::v2::serializeHeartbeatRequest(
                    worker_id, task, phase, processed_bytes)));
    requireSuccess(response, "Clipper V2 heartbeat");
}

authorization::v2::OutputGrant ClipperV2ApiClient::prepareOutput(
        const authorization::v2::ClaimTask& task,
        const std::string& worker_id,
        const authorization::v2::OutputDeclaration& declaration) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.work_item_id, "outputs/prepare"),
            authorization::v2::serializeOutputPrepareRequest(
                    worker_id, task, declaration)));
    requireSuccess(response, "Clipper V2 output preparation");
    return authorization::v2::parseOutputGrant(response.body);
}

void ClipperV2ApiClient::reportOutput(
        const authorization::v2::ClaimTask& task,
        const std::string& worker_id,
        const authorization::v2::OutputReport& report) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.work_item_id, "outputs/report"),
            authorization::v2::serializeOutputReportRequest(
                    worker_id, task, report)));
    requireSuccess(response, "Clipper V2 output report");
}

void ClipperV2ApiClient::complete(
        const authorization::v2::ClaimTask& task,
        const std::string& worker_id,
        const authorization::v2::Completion& completion) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.work_item_id, "complete"),
            authorization::v2::serializeCompleteRequest(
                    worker_id, task, completion)));
    requireSuccess(response, "Clipper V2 completion");
}

void ClipperV2ApiClient::fail(
        const authorization::v2::ClaimTask& task,
        const std::string& worker_id,
        const authorization::v2::Failure& failure) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.work_item_id, "fail"),
            authorization::v2::serializeFailRequest(
                    worker_id, task, failure)));
    requireSuccess(response, "Clipper V2 failure report");
}

NormalizationHttpRequest ClipperV2ApiClient::request(
        NormalizationHttpMethod method, const std::string& path,
        std::string body) const {
    NormalizationHttpRequest result;
    result.method = method;
    result.url = config_.base_url + path;
    result.headers.emplace_back(kInnerSourceHeader);
    if (method == NormalizationHttpMethod::post) {
        result.headers.emplace_back("Content-Type: application/json");
    }
    if (!config_.authorization_header.empty()) {
        result.headers.push_back(config_.authorization_header);
    }
    result.body = std::move(body);
    return result;
}

std::string ClipperV2ApiClient::taskPath(
        const std::string& work_item_id,
        const std::string& suffix) const {
    return std::string(kTaskBasePath) + "/"
            + escapePathSegment(work_item_id) + "/" + suffix;
}

void ClipperV2ApiClient::requireSuccess(
        const NormalizationHttpResponse& response, const char* operation) {
    if (response.status_code == kHttpOk
            || response.status_code == kHttpNoContent) {
        return;
    }
    throw NormalizationApiError(response.status_code, operation);
}

}  // namespace clip_worker::client
