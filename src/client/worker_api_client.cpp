#include "clip_worker/client/worker_api_client.hpp"

#include "clip_worker/client/curl_runtime.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include <curl/curl.h>

namespace clip_worker::client {
namespace {

constexpr const char* kClaimPath = "/api/internal/three-d-clip/tasks/claim";
constexpr const char* kTaskBasePath = "/api/internal/three-d-clip/tasks/";
constexpr const char* kInnerSourceHeader = "from-source: inner";
constexpr long kHttpOk = 200;
constexpr long kHttpNoContent = 204;

std::size_t appendResponse(char* data, std::size_t size, std::size_t count, void* context) {
    const auto byte_count = size * count;
    auto* response = static_cast<std::string*>(context);
    response->append(data, byte_count);
    return byte_count;
}

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

}  // namespace

WorkerApiError::WorkerApiError(long status_code, std::string message)
    : std::runtime_error(std::move(message)), status_code_(status_code) {
}

long WorkerApiError::statusCode() const noexcept {
    return status_code_;
}

WorkerApiClient::WorkerApiClient(WorkerApiClientConfig config)
    : config_(std::move(config)) {
    config_.base_url = trimTrailingSlash(config_.base_url);
    if (config_.base_url.empty()) {
        throw std::invalid_argument("Worker API base URL must not be blank");
    }
    if (config_.connect_timeout_seconds <= 0 || config_.request_timeout_seconds <= 0) {
        throw std::invalid_argument("Worker API timeouts must be greater than zero");
    }
    ensureCurlInitialized();
}

std::optional<task::ClaimTask> WorkerApiClient::claim(const task::ClaimRequest& request) const {
    const auto response = postJson(kClaimPath, task::serializeClaimRequest(request));
    if (response.status_code == kHttpNoContent) {
        return std::nullopt;
    }
    requireSuccess(response, "claim");
    return task::parseClaimTask(response.body);
}

void WorkerApiClient::heartbeat(const std::string& asset_id,
                                const task::LeaseRequest& request) const {
    const auto response = postJson(taskPath(asset_id, "heartbeat"),
                                   task::serializeLeaseRequest(request));
    requireSuccess(response, "heartbeat");
}

void WorkerApiClient::complete(const std::string& asset_id,
                               const task::CompleteRequest& request) const {
    const auto response = postJson(taskPath(asset_id, "complete"),
                                   task::serializeCompleteRequest(request));
    requireSuccess(response, "complete");
}

void WorkerApiClient::fail(const std::string& asset_id,
                           const task::FailRequest& request) const {
    const auto response = postJson(taskPath(asset_id, "fail"),
                                   task::serializeFailRequest(request));
    requireSuccess(response, "fail");
}

WorkerApiClient::Response WorkerApiClient::postJson(const std::string& path,
                                                     const std::string& body) const {
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        throw WorkerApiError(0, "Failed to allocate libcurl request handle");
    }

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
    raw_headers = curl_slist_append(raw_headers, kInnerSourceHeader);
    if (!config_.authorization_header.empty()) {
        raw_headers = curl_slist_append(raw_headers, config_.authorization_header.c_str());
    }
    using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
    HeaderList headers(raw_headers, &curl_slist_free_all);

    Response response;
    const std::string url = config_.base_url + path;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, config_.connect_timeout_seconds);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, config_.request_timeout_seconds);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &appendResponse);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        throw WorkerApiError(0, std::string("Worker API request failed: ")
                                    + curl_easy_strerror(result));
    }
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status_code);
    return response;
}

std::string WorkerApiClient::taskPath(const std::string& asset_id,
                                      const std::string& operation) const {
    if (asset_id.empty()) {
        throw std::invalid_argument("assetId must not be blank");
    }
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        throw WorkerApiError(0, "Failed to allocate libcurl escaping handle");
    }
    using EscapedValue = std::unique_ptr<char, decltype(&curl_free)>;
    EscapedValue escaped(curl_easy_escape(curl.get(), asset_id.c_str(),
                                          static_cast<int>(asset_id.size())), &curl_free);
    if (!escaped) {
        throw WorkerApiError(0, "Failed to encode assetId path segment");
    }
    return std::string(kTaskBasePath) + escaped.get() + "/" + operation;
}

void WorkerApiClient::requireSuccess(const Response& response, const char* operation) {
    if (response.status_code == kHttpOk || response.status_code == kHttpNoContent) {
        return;
    }
    // Do not include response bodies because they can accidentally contain signed URLs.
    throw WorkerApiError(response.status_code,
                         std::string("Worker API ") + operation + " returned HTTP "
                                 + std::to_string(response.status_code));
}

}  // namespace clip_worker::client
