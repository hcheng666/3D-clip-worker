#include "clip_worker/client/normalization_api_client.hpp"

#include "clip_worker/client/curl_runtime.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <curl/curl.h>

namespace clip_worker::client {
namespace {

constexpr const char* kTaskBasePath =
        "/api/internal/three-d-tiles/normalization-tasks";
constexpr const char* kInnerSourceHeader = "from-source: inner";
constexpr const char* kWorkerIdHeader = "X-Normalizer-Worker-Id: ";
constexpr const char* kLeaseTokenHeader = "X-Normalizer-Lease-Token: ";
constexpr const char* kRequestIdHeader = "X-Normalizer-Request-Id: ";
constexpr long kHttpOk = 200;
constexpr long kHttpNoContent = 204;
constexpr std::size_t kMaximumControlResponseBytes = 2U * 1024U * 1024U;

struct BoundedResponse {
    std::string body;
    bool exceeded = false;
};

std::size_t appendResponse(char* data, std::size_t size, std::size_t count,
                           void* context) {
    if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) {
        return 0U;
    }
    const std::size_t byte_count = size * count;
    auto* response = static_cast<BoundedResponse*>(context);
    if (byte_count > kMaximumControlResponseBytes - response->body.size()) {
        response->exceeded = true;
        return 0U;
    }
    response->body.append(data, byte_count);
    return byte_count;
}

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string escapePathSegment(const std::string& value) {
    if (value.empty()) throw std::invalid_argument("Normalizer task ID is blank");
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw NormalizationApiError(0, "path encoding");
    using Escaped = std::unique_ptr<char, decltype(&curl_free)>;
    Escaped escaped(curl_easy_escape(curl.get(), value.c_str(),
                                     static_cast<int>(value.size())), &curl_free);
    if (!escaped) throw NormalizationApiError(0, "path encoding");
    return escaped.get();
}

}  // namespace

NormalizationApiError::NormalizationApiError(long status_code,
                                               std::string operation)
    : std::runtime_error("Normalization API " + std::move(operation)
                         + " failed with HTTP " + std::to_string(status_code)),
      status_code_(status_code) {
}

long NormalizationApiError::statusCode() const noexcept {
    return status_code_;
}

CurlNormalizationHttpTransport::CurlNormalizationHttpTransport(
        long connect_timeout_seconds, long request_timeout_seconds)
    : connect_timeout_seconds_(connect_timeout_seconds),
      request_timeout_seconds_(request_timeout_seconds) {
    if (connect_timeout_seconds_ <= 0 || request_timeout_seconds_ <= 0) {
        throw std::invalid_argument(
                "Normalization API timeouts must be greater than zero");
    }
    ensureCurlInitialized();
}

NormalizationHttpResponse CurlNormalizationHttpTransport::execute(
        const NormalizationHttpRequest& request) const {
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw NormalizationApiError(0, "transport allocation");

    curl_slist* raw_headers = nullptr;
    for (const auto& header : request.headers) {
        raw_headers = curl_slist_append(raw_headers, header.c_str());
    }
    using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
    HeaderList headers(raw_headers, &curl_slist_free_all);
    BoundedResponse response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    if (request.method == NormalizationHttpMethod::get) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(request.body.size()));
    }
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds_);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, request_timeout_seconds_);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &appendResponse);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        throw NormalizationApiError(
                0, response.exceeded ? "response limit" : "transport");
    }
    NormalizationHttpResponse output;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &output.status_code);
    output.body = std::move(response.body);
    return output;
}

NormalizationApiClient::NormalizationApiClient(
        NormalizationApiClientConfig config)
    : NormalizationApiClient(
              config,
              std::make_shared<CurlNormalizationHttpTransport>(
                      config.connect_timeout_seconds,
                      config.request_timeout_seconds)) {
}

NormalizationApiClient::NormalizationApiClient(
        NormalizationApiClientConfig config,
        std::shared_ptr<const INormalizationHttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    config_.base_url = trimTrailingSlash(config_.base_url);
    if (config_.base_url.empty()) {
        throw std::invalid_argument("Normalization API base URL must not be blank");
    }
    if (!transport_) {
        throw std::invalid_argument("Normalization HTTP transport is required");
    }
    ensureCurlInitialized();
}

std::optional<normalization::ClaimTask> NormalizationApiClient::claim(
        const normalization::ClaimRequest& claim_request) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post, std::string(kTaskBasePath) + "/claim",
            normalization::serializeClaimRequest(claim_request)));
    if (response.status_code == kHttpNoContent) return std::nullopt;
    requireSuccess(response, "claim");
    return normalization::parseClaimTask(response.body);
}

normalization::ResourceManifestPage
NormalizationApiClient::resourceManifestPage(
        const normalization::ClaimTask& task, const std::string& worker_id,
        std::uint32_t page_number) const {
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
    requireSuccess(response, "resource manifest page");
    return normalization::parseResourceManifestPage(response.body);
}

normalization::HeartbeatResponse NormalizationApiClient::heartbeat(
        const normalization::ClaimTask& task, const std::string& worker_id,
        normalization::TaskPhase phase, std::uint64_t total_resources,
        std::uint64_t processed_resources) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post, taskPath(task.task_id, "heartbeat"),
            normalization::serializeHeartbeatRequest(
                    worker_id, task, phase, total_resources,
                    processed_resources)));
    requireSuccess(response, "heartbeat");
    return normalization::parseHeartbeatResponse(response.body);
}

normalization::UploadGrant NormalizationApiClient::prepareUpload(
        const normalization::ClaimTask& task, const std::string& worker_id,
        const normalization::UploadDeclaration& declaration) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.task_id, "prepare-upload"),
            normalization::serializeUploadPrepareRequest(
                    worker_id, task, declaration)));
    requireSuccess(response, "upload preparation");
    return normalization::parseUploadGrant(response.body);
}

void NormalizationApiClient::reportUpload(
        const normalization::ClaimTask& task, const std::string& worker_id,
        const normalization::UploadReport& report) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post,
            taskPath(task.task_id, "upload-report"),
            normalization::serializeUploadReportRequest(
                    worker_id, task, report)));
    requireSuccess(response, "upload report");
}

void NormalizationApiClient::complete(
        const normalization::ClaimTask& task,
        const std::string& worker_id) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post, taskPath(task.task_id, "complete"),
            normalization::serializeCompleteRequest(worker_id, task)));
    requireSuccess(response, "completion");
}

void NormalizationApiClient::fail(
        const normalization::ClaimTask& task, const std::string& worker_id,
        const normalization::Failure& failure) const {
    const auto response = transport_->execute(request(
            NormalizationHttpMethod::post, taskPath(task.task_id, "fail"),
            normalization::serializeFailRequest(worker_id, task, failure)));
    requireSuccess(response, "failure report");
}

NormalizationHttpRequest NormalizationApiClient::request(
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

std::string NormalizationApiClient::taskPath(
        const std::string& task_id, const std::string& suffix) const {
    return std::string(kTaskBasePath) + "/" + escapePathSegment(task_id)
            + "/" + suffix;
}

void NormalizationApiClient::requireSuccess(
        const NormalizationHttpResponse& response, const char* operation) {
    if (response.status_code == kHttpOk
            || response.status_code == kHttpNoContent) {
        return;
    }
    // Bodies and headers may contain short-lived grants and are never echoed.
    throw NormalizationApiError(response.status_code, operation);
}

}  // namespace clip_worker::client
