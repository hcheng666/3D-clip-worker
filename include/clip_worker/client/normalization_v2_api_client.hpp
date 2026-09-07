#pragma once

#include "clip_worker/client/normalization_api_client.hpp"
#include "clip_worker/normalization/normalization_v2_contract.hpp"

namespace clip_worker::client {

/** Additive Normalizer V2 client; V1 routes and payloads remain unchanged. */
class NormalizationV2ApiClient final {
public:
    explicit NormalizationV2ApiClient(NormalizationApiClientConfig config);
    NormalizationV2ApiClient(NormalizationApiClientConfig config,
                             std::string task_base_path);
    NormalizationV2ApiClient(
            NormalizationApiClientConfig config,
            std::shared_ptr<const INormalizationHttpTransport> transport);
    NormalizationV2ApiClient(
            NormalizationApiClientConfig config,
            std::shared_ptr<const INormalizationHttpTransport> transport,
            std::string task_base_path);

    [[nodiscard]] std::optional<normalization::v2::ClaimTask> claim(
            const normalization::v2::ClaimRequest& request) const;
    [[nodiscard]] normalization::ResourceManifestPage resourceManifestPage(
            const normalization::v2::ClaimTask& task,
            const std::string& worker_id, std::uint32_t page_number) const;
    [[nodiscard]] normalization::HeartbeatResponse heartbeat(
            const normalization::v2::ClaimTask& task,
            const std::string& worker_id, normalization::TaskPhase phase,
            std::uint64_t total_resources,
            std::uint64_t processed_resources) const;
    [[nodiscard]] normalization::v2::OutputGrant prepareOutput(
            const normalization::v2::ClaimTask& task,
            const std::string& worker_id,
            const normalization::v2::OutputDeclaration& declaration) const;
    void reportOutput(const normalization::v2::ClaimTask& task,
                      const std::string& worker_id,
                      const normalization::v2::OutputReport& report) const;
    void complete(const normalization::v2::ClaimTask& task,
                  const std::string& worker_id,
                  const normalization::v2::Completion& completion) const;
    void fail(const normalization::v2::ClaimTask& task,
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
    std::string task_base_path_;
};

}  // namespace clip_worker::client
