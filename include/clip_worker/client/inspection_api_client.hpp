#pragma once

#include "clip_worker/inspection/inspection_scheduling.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace clip_worker::client {

enum class InspectionHttpMethod { get, post };

struct InspectionHttpRequest {
    InspectionHttpMethod method = InspectionHttpMethod::post;
    std::string url;
    std::vector<std::string> headers;
    std::string body;
};

struct InspectionHttpResponse {
    long status_code = 0;
    std::string body;
};

class IInspectionHttpTransport {
public:
    virtual ~IInspectionHttpTransport() = default;
    [[nodiscard]] virtual InspectionHttpResponse execute(
            const InspectionHttpRequest& request) const = 0;
};

struct InspectionApiClientConfig {
    static constexpr long kDefaultConnectTimeoutSeconds = 10;
    static constexpr long kDefaultRequestTimeoutSeconds = 30;

    std::string base_url;
    std::string authorization_header;
    long connect_timeout_seconds = kDefaultConnectTimeoutSeconds;
    long request_timeout_seconds = kDefaultRequestTimeoutSeconds;
};

class InspectionApiError final : public std::runtime_error {
public:
    InspectionApiError(long status_code, std::string operation);
    [[nodiscard]] long statusCode() const noexcept;

private:
    long status_code_;
};

class CurlInspectionHttpTransport final : public IInspectionHttpTransport {
public:
    CurlInspectionHttpTransport(long connect_timeout_seconds,
                                long request_timeout_seconds);
    [[nodiscard]] InspectionHttpResponse execute(
            const InspectionHttpRequest& request) const override;

private:
    long connect_timeout_seconds_;
    long request_timeout_seconds_;
};

/** Dedicated Inspector scheduling client. It never shares clip-task JSON. */
class InspectionApiClient final {
public:
    explicit InspectionApiClient(InspectionApiClientConfig config);
    InspectionApiClient(InspectionApiClientConfig config,
                        std::shared_ptr<const IInspectionHttpTransport> transport);

    [[nodiscard]] std::optional<inspection::InspectionClaimTask> claim(
            const inspection::InspectionClaimRequest& request) const;
    [[nodiscard]] inspection::SourceManifestPage sourceManifestPage(
            const inspection::InspectionClaimTask& task,
            const std::string& worker_id, std::uint32_t page_number) const;
    [[nodiscard]] inspection::InspectionHeartbeatResponse heartbeat(
            const inspection::InspectionClaimTask& task,
            const std::string& worker_id, inspection::TaskPhase phase,
            const inspection::InspectionProgress& progress) const;
    void submitResultPage(const inspection::InspectionClaimTask& task,
                          const std::string& worker_id,
                          const inspection::ResultPage& page) const;
    void submitHierarchyPage(const inspection::InspectionClaimTask& task,
                             const std::string& worker_id,
                             const inspection::HierarchyPage& page) const;
    [[nodiscard]] inspection::ExpansionPlanDescriptor prepareExpansion(
            const inspection::InspectionClaimTask& task,
            const std::string& worker_id,
            const inspection::InspectionResult& result,
            const std::string& result_envelope_sha256) const;
    [[nodiscard]] inspection::ExpansionPlanPage expansionPlanPage(
            const inspection::InspectionClaimTask& task,
            const std::string& worker_id, const std::string& plan_id,
            std::uint32_t page_number) const;
    void reportExpandedUpload(
            const inspection::InspectionClaimTask& task,
            const std::string& worker_id, const std::string& plan_id,
            const inspection::ExpandedUploadReport& report) const;
    void complete(const inspection::InspectionClaimTask& task,
                  const std::string& worker_id,
                  const inspection::InspectionResult& result) const;

private:
    [[nodiscard]] InspectionHttpRequest request(
            InspectionHttpMethod method, const std::string& path,
            std::string body = {}) const;
    [[nodiscard]] std::string taskPath(const std::string& inspection_id,
                                       const std::string& suffix) const;
    static void requireSuccess(const InspectionHttpResponse& response,
                               const char* operation);

    InspectionApiClientConfig config_;
    std::shared_ptr<const IInspectionHttpTransport> transport_;
};

}  // namespace clip_worker::client
