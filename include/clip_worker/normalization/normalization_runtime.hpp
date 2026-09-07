#pragma once

#include "clip_worker/client/normalization_api_client.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::normalization {

enum class SessionState {
    active,
    cancelled,
    lease_lost,
    completed,
    failed
};

/** Lease-bound Normalizer runtime seam. Production claiming remains disabled. */
class NormalizationLeaseSession final {
public:
    NormalizationLeaseSession(const client::NormalizationApiClient& api_client,
                              std::string worker_id, ClaimTask task,
                              std::string validation_time_utc,
                              std::uint64_t maximum_input_bytes =
                                      ProtocolLimits::kMaximumArtifactBytes,
                              std::uint64_t maximum_output_bytes =
                                      ProtocolLimits::kMaximumArtifactBytes);

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const ClaimTask& task() const noexcept;
    [[nodiscard]] const std::vector<ResourceManifestPage>& manifestPages() const;

    [[nodiscard]] HeartbeatResponse heartbeat(TaskPhase phase,
                                              std::uint64_t processed_resources);
    [[nodiscard]] ResourceManifestPage resourceManifestPage(
            std::uint32_t page_number);
    void validateManifest() const;
    [[nodiscard]] UploadGrant prepareUpload(
            const UploadDeclaration& declaration);
    void reportUpload(const UploadReport& report);
    void complete();
    void fail(const Failure& failure);

private:
    void requireActive() const;
    void handleApiFailure(const client::NormalizationApiError& error,
                          bool terminal_operation);

    const client::NormalizationApiClient& api_client_;
    std::string worker_id_;
    ClaimTask task_;
    std::string validation_time_utc_;
    std::uint64_t maximum_input_bytes_;
    std::uint64_t maximum_output_bytes_;
    SessionState state_ = SessionState::active;
    TaskPhase phase_ = TaskPhase::claimed;
    std::uint64_t processed_resources_ = 0U;
    std::vector<ResourceManifestPage> manifest_pages_;
    std::optional<UploadDeclaration> upload_declaration_;
    bool upload_reported_ = false;
};

class NormalizationTaskRuntime final {
public:
    using Executor = std::function<void(NormalizationLeaseSession&)>;

    NormalizationTaskRuntime(ClaimRequest capabilities,
                             client::NormalizationApiClient api_client);

    /** Claims at most one task and invokes the supplied isolated executor. */
    [[nodiscard]] bool runOnce(const std::string& now_utc,
                               const Executor& executor) const;

private:
    ClaimRequest capabilities_;
    client::NormalizationApiClient api_client_;
};

}  // namespace clip_worker::normalization
