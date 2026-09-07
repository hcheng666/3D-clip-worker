#pragma once

#include "clip_worker/authorization/clipper_v2_contract.hpp"
#include "clip_worker/client/normalization_api_client.hpp"

namespace clip_worker::client {

/** Independent Clipper V2 client; legacy clipping routes remain unchanged. */
class ClipperV2ApiClient final {
public:
    explicit ClipperV2ApiClient(NormalizationApiClientConfig config);
    ClipperV2ApiClient(
            NormalizationApiClientConfig config,
            std::shared_ptr<const INormalizationHttpTransport> transport);

    [[nodiscard]] std::optional<authorization::v2::ClaimTask> claim(
            const authorization::v2::ClaimRequest& request) const;
    void heartbeat(const authorization::v2::ClaimTask& task,
                   const std::string& worker_id,
                   authorization::v2::TaskPhase phase,
                   std::uint64_t processed_bytes) const;
    [[nodiscard]] authorization::v2::OutputGrant prepareOutput(
            const authorization::v2::ClaimTask& task,
            const std::string& worker_id,
            const authorization::v2::OutputDeclaration& declaration) const;
    void reportOutput(const authorization::v2::ClaimTask& task,
                      const std::string& worker_id,
                      const authorization::v2::OutputReport& report) const;
    void complete(const authorization::v2::ClaimTask& task,
                  const std::string& worker_id,
                  const authorization::v2::Completion& completion) const;
    void fail(const authorization::v2::ClaimTask& task,
              const std::string& worker_id,
              const authorization::v2::Failure& failure) const;

private:
    [[nodiscard]] NormalizationHttpRequest request(
            NormalizationHttpMethod method, const std::string& path,
            std::string body = {}) const;
    [[nodiscard]] std::string taskPath(
            const std::string& work_item_id,
            const std::string& suffix) const;
    static void requireSuccess(const NormalizationHttpResponse& response,
                               const char* operation);

    NormalizationApiClientConfig config_;
    std::shared_ptr<const INormalizationHttpTransport> transport_;
};

}  // namespace clip_worker::client
