#pragma once

#include "clip_worker/normalization/normalization_contract.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace clip_worker::client {

enum class NormalizationHttpMethod { get, post };

struct NormalizationHttpRequest {
    NormalizationHttpMethod method = NormalizationHttpMethod::post;
    std::string url;
    std::vector<std::string> headers;
    std::string body;
};

struct NormalizationHttpResponse {
    long status_code = 0;
    std::string body;
};

class INormalizationHttpTransport {
public:
    virtual ~INormalizationHttpTransport() = default;
    [[nodiscard]] virtual NormalizationHttpResponse execute(
            const NormalizationHttpRequest& request) const = 0;
};

struct NormalizationApiClientConfig {
    static constexpr long kDefaultConnectTimeoutSeconds = 10;
    static constexpr long kDefaultRequestTimeoutSeconds = 30;

    std::string base_url;
    std::string authorization_header;
    long connect_timeout_seconds = kDefaultConnectTimeoutSeconds;
    long request_timeout_seconds = kDefaultRequestTimeoutSeconds;
};

class NormalizationApiError final : public std::runtime_error {
public:
    NormalizationApiError(long status_code, std::string operation);
    [[nodiscard]] long statusCode() const noexcept;

private:
    long status_code_;
};

class CurlNormalizationHttpTransport final
        : public INormalizationHttpTransport {
public:
    CurlNormalizationHttpTransport(long connect_timeout_seconds,
                                   long request_timeout_seconds);
    [[nodiscard]] NormalizationHttpResponse execute(
            const NormalizationHttpRequest& request) const override;

private:
    long connect_timeout_seconds_;
    long request_timeout_seconds_;
};

/** Dedicated Normalizer V1 client; error text never includes bodies or grants. */
class NormalizationApiClient final {
public:
    explicit NormalizationApiClient(NormalizationApiClientConfig config);
    NormalizationApiClient(
            NormalizationApiClientConfig config,
            std::shared_ptr<const INormalizationHttpTransport> transport);

    [[nodiscard]] std::optional<normalization::ClaimTask> claim(
            const normalization::ClaimRequest& request) const;
    [[nodiscard]] normalization::ResourceManifestPage resourceManifestPage(
            const normalization::ClaimTask& task, const std::string& worker_id,
            std::uint32_t page_number) const;
    [[nodiscard]] normalization::HeartbeatResponse heartbeat(
            const normalization::ClaimTask& task, const std::string& worker_id,
            normalization::TaskPhase phase, std::uint64_t total_resources,
            std::uint64_t processed_resources) const;
    [[nodiscard]] normalization::UploadGrant prepareUpload(
            const normalization::ClaimTask& task, const std::string& worker_id,
            const normalization::UploadDeclaration& declaration) const;
    void reportUpload(const normalization::ClaimTask& task,
                      const std::string& worker_id,
                      const normalization::UploadReport& report) const;
    void complete(const normalization::ClaimTask& task,
                  const std::string& worker_id) const;
    void fail(const normalization::ClaimTask& task,
              const std::string& worker_id,
              const normalization::Failure& failure) const;

private:
    [[nodiscard]] NormalizationHttpRequest request(
            NormalizationHttpMethod method, const std::string& path,
            std::string body = {}) const;
    [[nodiscard]] std::string taskPath(const std::string& task_id,
                                       const std::string& suffix) const;
    static void requireSuccess(const NormalizationHttpResponse& response,
                               const char* operation);

    NormalizationApiClientConfig config_;
    std::shared_ptr<const INormalizationHttpTransport> transport_;
};

}  // namespace clip_worker::client
