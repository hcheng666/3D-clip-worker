#include "clip_worker/client/normalization_v2_api_client.hpp"

#include "clip_worker/client/curl_runtime.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"

#include <memory>
#include <utility>

#include <curl/curl.h>

namespace clip_worker::client {
namespace {

constexpr const char* kTaskBasePath =
        "/api/internal/three-d-tiles/normalization-v2-tasks";
constexpr const char* kInnerSourceHeader = "from-source: inner";
constexpr const char* kWorkerIdHeader = "X-Normalizer-Worker-Id: ";
constexpr const char* kLeaseTokenHeader = "X-Normalizer-Lease-Token: ";
constexpr const char* kRequestIdHeader = "X-Normalizer-Request-Id: ";
constexpr long kHttpOk = 200;
constexpr long kHttpNoContent = 204;

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string escapePathSegment(const std::string& value) {
    if (value.empty()) throw std::invalid_argument("Normalizer V2 task ID is blank");
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw NormalizationApiError(0, "V2 path encoding");
    using Escaped = std::unique_ptr<char, decltype(&curl_free)>;
    Escaped escaped(curl_easy_escape(curl.get(), value.c_str(),
                                     static_cast<int>(value.size())),
                    &curl_free);
    if (!escaped) throw NormalizationApiError(0, "V2 path encoding");
    return escaped.get();
}

}  // namespace

NormalizationV2ApiClient::NormalizationV2ApiClient(
        NormalizationApiClientConfig config)
    : NormalizationV2ApiClient(
              config,
              std::make_shared<CurlNormalizationHttpTransport>(
                      config.connect_timeout_seconds,
                      config.request_timeout_seconds),
              kTaskBasePath) {
}

NormalizationV2ApiClient::NormalizationV2ApiClient(
        NormalizationApiClientConfig config, std::string task_base_path)
    : NormalizationV2ApiClient(
              config,
              std::make_shared<CurlNormalizationHttpTransport>(
                      config.connect_timeout_seconds,
                      config.request_timeout_seconds),
              std::move(task_base_path)) {
}

NormalizationV2ApiClient::NormalizationV2ApiClient(
        NormalizationApiClientConfig config,
        std::shared_ptr<const INormalizationHttpTransport> transport)
    : NormalizationV2ApiClient(std::move(config), std::move(transport),
                               kTaskBasePath) {
}

NormalizationV2ApiClient::NormalizationV2ApiClient(
        NormalizationApiClientConfig config,
        std::shared_ptr<const INormalizationHttpTransport> transport,
        std::string task_base_path)
    : config_(std::move(config)), transport_(std::move(transport)),
      task_base_path_(std::move(task_base_path)) {
    config_.base_url = trimTrailingSlash(config_.base_url);
    if (config_.base_url.empty() || !transport_
            || (task_base_path_ != kTaskBasePath
                && task_base_path_ != normalization::v3::kTaskBasePath)) {
        throw std::invalid_argument(
                "Normalizer V2 API configuration is incomplete");
    }
    ensureCurlInitialized();
}

std::optional<normalization::v2::ClaimTask>
NormalizationV2ApiClient::claim(
        const normalization::v2::ClaimRequest& claim_request) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            task_base_path_ + "/claim",
            normalization::v2::serializeClaimRequest(claim_request)));
    if (response.status_code == kHttpNoContent) return std::nullopt;
    requireSuccess(response, "V2 claim");
    return normalization::v2::parseClaimTask(response.body);
}

normalization::ResourceManifestPage
NormalizationV2ApiClient::resourceManifestPage(
        const normalization::v2::ClaimTask& task,
        const std::string& worker_id, std::uint32_t page_number) const {
    auto http_request = request(
            NormalizationHttpMethod::get,
            taskPath(task.task_id, "resource-manifest/pages/"
                    + std::to_string(page_number)));
    http_request.headers.push_back(std::string(kWorkerIdHeader) + worker_id);
    http_request.headers.push_back(
            std::string(kLeaseTokenHeader) + task.lease_token);
    http_request.headers.push_back(
            std::string(kRequestIdHeader) + task.request_id);
    const auto response = transport_->execute(http_request);
    requireSuccess(response, "V2 resource manifest page");
    return normalization::parseResourceManifestPage(response.body);
}

normalization::HeartbeatResponse NormalizationV2ApiClient::heartbeat(
        const normalization::v2::ClaimTask& task,
        const std::string& worker_id, normalization::TaskPhase phase,
        std::uint64_t total_resources,
        std::uint64_t processed_resources) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.task_id, "heartbeat"),
            normalization::v2::serializeHeartbeatRequest(
                    worker_id, task, phase, total_resources,
                    processed_resources)));
    requireSuccess(response, "V2 heartbeat");
    return normalization::parseHeartbeatResponse(response.body);
}

normalization::v2::OutputGrant NormalizationV2ApiClient::prepareOutput(
        const normalization::v2::ClaimTask& task,
        const std::string& worker_id,
        const normalization::v2::OutputDeclaration& declaration) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.task_id, "prepare-output"),
            normalization::v2::serializeOutputPrepareRequest(
                    worker_id, task, declaration)));
    requireSuccess(response, "V2 output preparation");
    return normalization::v2::parseOutputGrant(response.body);
}

void NormalizationV2ApiClient::reportOutput(
        const normalization::v2::ClaimTask& task,
        const std::string& worker_id,
        const normalization::v2::OutputReport& report) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.task_id, "output-report"),
            normalization::v2::serializeOutputReportRequest(
                    worker_id, task, report)));
    requireSuccess(response, "V2 output report");
}

void NormalizationV2ApiClient::complete(
        const normalization::v2::ClaimTask& task,
        const std::string& worker_id,
        const normalization::v2::Completion& completion) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.task_id, "complete"),
            normalization::v2::serializeCompleteRequest(
                    worker_id, task, completion)));
    requireSuccess(response, "V2 completion");
}

void NormalizationV2ApiClient::fail(
        const normalization::v2::ClaimTask& task,
        const std::string& worker_id,
        const normalization::Failure& failure) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.task_id, "fail"),
            normalization::v2::serializeFailRequest(
                    worker_id, task, failure)));
    requireSuccess(response, "V2 failure report");
}

NormalizationHttpRequest NormalizationV2ApiClient::request(
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

std::string NormalizationV2ApiClient::taskPath(
        const std::string& task_id, const std::string& suffix) const {
    return task_base_path_ + "/" + escapePathSegment(task_id) + "/" + suffix;
}

void NormalizationV2ApiClient::requireSuccess(
        const NormalizationHttpResponse& response, const char* operation) {
    if (response.status_code == kHttpOk
        || response.status_code == kHttpNoContent) {
        return;
    }
    throw NormalizationApiError(response.status_code, operation);
}

}  // namespace clip_worker::client
