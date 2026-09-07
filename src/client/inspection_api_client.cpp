#include "clip_worker/client/inspection_api_client.hpp"

#include "clip_worker/client/curl_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include <curl/curl.h>

namespace clip_worker::client {
namespace {

constexpr const char* kTaskBasePath = "/api/internal/three-d-tiles/inspection-tasks";
constexpr const char* kInnerSourceHeader = "from-source: inner";
constexpr const char* kWorkerIdHeader = "X-Inspection-Worker-Id: ";
constexpr const char* kLeaseTokenHeader = "X-Inspection-Lease-Token: ";
constexpr const char* kRequestIdHeader = "X-Inspection-Request-Id: ";
constexpr long kHttpOk = 200;
constexpr long kHttpNoContent = 204;
constexpr std::size_t kMaximumControlResponseBytes = 16U * 1024U * 1024U;

struct BoundedResponse {
    std::string body;
    bool exceeded = false;
};

std::size_t appendResponse(char* data, std::size_t size, std::size_t count, void* context) {
    const auto byte_count = size * count;
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
    if (value.empty()) throw std::invalid_argument("inspectionId must not be blank");
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw InspectionApiError(0, "path encoding");
    using EscapedValue = std::unique_ptr<char, decltype(&curl_free)>;
    EscapedValue escaped(curl_easy_escape(curl.get(), value.c_str(),
                                          static_cast<int>(value.size())), &curl_free);
    if (!escaped) throw InspectionApiError(0, "path encoding");
    return escaped.get();
}

}  // namespace

InspectionApiError::InspectionApiError(long status_code, std::string operation)
    : std::runtime_error("Inspection API " + std::move(operation)
                         + " failed with HTTP " + std::to_string(status_code)),
      status_code_(status_code) {
}

long InspectionApiError::statusCode() const noexcept {
    return status_code_;
}

CurlInspectionHttpTransport::CurlInspectionHttpTransport(
        long connect_timeout_seconds, long request_timeout_seconds)
    : connect_timeout_seconds_(connect_timeout_seconds),
      request_timeout_seconds_(request_timeout_seconds) {
    if (connect_timeout_seconds_ <= 0 || request_timeout_seconds_ <= 0) {
        throw std::invalid_argument("Inspection API timeouts must be greater than zero");
    }
    ensureCurlInitialized();
}

InspectionHttpResponse CurlInspectionHttpTransport::execute(
        const InspectionHttpRequest& request) const {
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw InspectionApiError(0, "transport allocation");

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
    if (request.method == InspectionHttpMethod::get) {
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
        throw InspectionApiError(0, response.exceeded ? "response limit" : "transport");
    }
    InspectionHttpResponse output;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &output.status_code);
    output.body = std::move(response.body);
    return output;
}

InspectionApiClient::InspectionApiClient(InspectionApiClientConfig config)
    : InspectionApiClient(
              config,
              std::make_shared<CurlInspectionHttpTransport>(
                      config.connect_timeout_seconds, config.request_timeout_seconds)) {
}

InspectionApiClient::InspectionApiClient(
        InspectionApiClientConfig config,
        std::shared_ptr<const IInspectionHttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    config_.base_url = trimTrailingSlash(config_.base_url);
    if (config_.base_url.empty()) {
        throw std::invalid_argument("Inspection API base URL must not be blank");
    }
    if (!transport_) throw std::invalid_argument("Inspection HTTP transport is required");
    ensureCurlInitialized();
}

std::optional<inspection::InspectionClaimTask> InspectionApiClient::claim(
        const inspection::InspectionClaimRequest& claim_request) const {
    const auto response = transport_->execute(request(
            InspectionHttpMethod::post, std::string(kTaskBasePath) + "/claim",
            inspection::serializeInspectionClaimRequest(claim_request)));
    if (response.status_code == kHttpNoContent) return std::nullopt;
    requireSuccess(response, "claim");
    return inspection::parseInspectionClaimTask(response.body);
}

inspection::SourceManifestPage InspectionApiClient::sourceManifestPage(
        const inspection::InspectionClaimTask& task,
        const std::string& worker_id, std::uint32_t page_number) const {
    auto http_request = request(
            InspectionHttpMethod::get,
            taskPath(task.inspection_id, "source-manifest/pages/"
                    + std::to_string(page_number)));
    http_request.headers.push_back(std::string(kWorkerIdHeader) + worker_id);
    http_request.headers.push_back(std::string(kLeaseTokenHeader) + task.lease_token);
    http_request.headers.push_back(std::string(kRequestIdHeader) + task.request_id);
    const auto response = transport_->execute(http_request);
    requireSuccess(response, "source manifest page");
    return inspection::parseSourceManifestPage(response.body);
}

inspection::InspectionHeartbeatResponse InspectionApiClient::heartbeat(
        const inspection::InspectionClaimTask& task,
        const std::string& worker_id, inspection::TaskPhase phase,
        const inspection::InspectionProgress& progress) const {
    const auto response = transport_->execute(request(
            InspectionHttpMethod::post, taskPath(task.inspection_id, "heartbeat"),
            inspection::serializeInspectionHeartbeatRequest(
                    worker_id, task.lease_token, task.request_id, phase, progress)));
    requireSuccess(response, "heartbeat");
    return inspection::parseInspectionHeartbeatResponse(response.body);
}

void InspectionApiClient::submitResultPage(
        const inspection::InspectionClaimTask& task,
        const std::string& worker_id, const inspection::ResultPage& page) const {
    const auto response = transport_->execute(request(
            InspectionHttpMethod::post, taskPath(task.inspection_id, "result-pages"),
            inspection::serializeInspectionResultPageRequest(
                    worker_id, task.lease_token, task.request_id, page)));
    requireSuccess(response, "result page");
}

void InspectionApiClient::submitHierarchyPage(
        const inspection::InspectionClaimTask& task,
        const std::string& worker_id,
        const inspection::HierarchyPage& page) const {
    const auto response = transport_->execute(request(
            InspectionHttpMethod::post,
            taskPath(task.inspection_id, "hierarchy-pages"),
            inspection::serializeInspectionHierarchyPageRequest(
                    worker_id, task.lease_token, task.request_id, page)));
    requireSuccess(response, "hierarchy page");
}

inspection::ExpansionPlanDescriptor InspectionApiClient::prepareExpansion(
        const inspection::InspectionClaimTask& task,
        const std::string& worker_id,
        const inspection::InspectionResult& result,
        const std::string& result_envelope_sha256) const {
    const auto response = transport_->execute(request(
            InspectionHttpMethod::post,
            taskPath(task.inspection_id, "prepare-expansion"),
            inspection::serializeExpansionPrepareRequest(
                    worker_id, task.lease_token, task.request_id,
                    result,
                    result_envelope_sha256)));
    requireSuccess(response, "expansion preparation");
    return inspection::parseExpansionPlanDescriptor(response.body);
}

inspection::ExpansionPlanPage InspectionApiClient::expansionPlanPage(
        const inspection::InspectionClaimTask& task,
        const std::string& worker_id, const std::string& plan_id,
        std::uint32_t page_number) const {
    auto http_request = request(
            InspectionHttpMethod::get,
            taskPath(task.inspection_id, "expansion-plan/pages/"
                    + std::to_string(page_number) + "?planId="
                    + escapePathSegment(plan_id)));
    http_request.headers.push_back(std::string(kWorkerIdHeader) + worker_id);
    http_request.headers.push_back(std::string(kLeaseTokenHeader) + task.lease_token);
    http_request.headers.push_back(std::string(kRequestIdHeader) + task.request_id);
    const auto response = transport_->execute(http_request);
    requireSuccess(response, "expansion plan page");
    return inspection::parseExpansionPlanPage(response.body);
}

void InspectionApiClient::reportExpandedUpload(
        const inspection::InspectionClaimTask& task,
        const std::string& worker_id, const std::string& plan_id,
        const inspection::ExpandedUploadReport& report) const {
    const auto response = transport_->execute(request(
            InspectionHttpMethod::post,
            taskPath(task.inspection_id, "expanded-uploads"),
            inspection::serializeExpandedUploadReport(
                    worker_id, task.lease_token, task.request_id, plan_id,
                    report)));
    requireSuccess(response, "expanded upload report");
}

void InspectionApiClient::complete(
        const inspection::InspectionClaimTask& task,
        const std::string& worker_id,
        const inspection::InspectionResult& result) const {
    const auto response = transport_->execute(request(
            InspectionHttpMethod::post, taskPath(task.inspection_id, "complete"),
            inspection::serializeInspectionCompleteRequest(
                    worker_id, task.lease_token, task.request_id, result)));
    requireSuccess(response, "completion");
}

InspectionHttpRequest InspectionApiClient::request(
        InspectionHttpMethod method, const std::string& path, std::string body) const {
    InspectionHttpRequest result;
    result.method = method;
    result.url = config_.base_url + path;
    result.headers.push_back(kInnerSourceHeader);
    if (method == InspectionHttpMethod::post) {
        result.headers.emplace_back("Content-Type: application/json");
    }
    if (!config_.authorization_header.empty()) {
        result.headers.push_back(config_.authorization_header);
    }
    result.body = std::move(body);
    return result;
}

std::string InspectionApiClient::taskPath(
        const std::string& inspection_id, const std::string& suffix) const {
    return std::string(kTaskBasePath) + "/" + escapePathSegment(inspection_id)
            + "/" + suffix;
}

void InspectionApiClient::requireSuccess(
        const InspectionHttpResponse& response, const char* operation) {
    if (response.status_code == kHttpOk || response.status_code == kHttpNoContent) return;
    // Response bodies, URLs, and headers can contain reusable grants and are never echoed.
    throw InspectionApiError(response.status_code, operation);
}

}  // namespace clip_worker::client
