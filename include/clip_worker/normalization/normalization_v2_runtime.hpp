#pragma once

#include "clip_worker/client/normalization_v2_api_client.hpp"
#include "clip_worker/normalization/normalization_runtime.hpp"

#include <functional>
#include <map>
#include <set>

namespace clip_worker::normalization::v2 {

/** Lease-bound V2 session with an exact, ordered multi-output closure. */
class NormalizationV2LeaseSession final {
public:
    NormalizationV2LeaseSession(
            const client::NormalizationV2ApiClient& api_client,
            ClaimRequest capabilities, ClaimTask task,
            std::string validation_time_utc);

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const ClaimTask& task() const noexcept;
    [[nodiscard]] const ClaimRequest& capabilities() const noexcept;
    [[nodiscard]] const std::vector<ResourceManifestPage>& manifestPages() const;

    [[nodiscard]] HeartbeatResponse heartbeat(TaskPhase phase,
                                              std::uint64_t processed_resources);
    [[nodiscard]] ResourceManifestPage resourceManifestPage(
            std::uint32_t page_number);
    void validateManifest() const;
    [[nodiscard]] OutputGrant prepareOutput(
            const OutputDeclaration& declaration);
    void reportOutput(const OutputReport& report);
    void complete(const std::vector<OutputDeclaration>& ordered_outputs,
                  bool global_preview_only);
    void fail(const Failure& failure);

private:
    void requireActive() const;
    void handleApiFailure(const client::NormalizationApiError& error,
                          bool terminal_operation);
    [[nodiscard]] const FamilyCapability& taskCapability() const;

    const client::NormalizationV2ApiClient& api_client_;
    ClaimRequest capabilities_;
    ClaimTask task_;
    std::string validation_time_utc_;
    SessionState state_ = SessionState::active;
    TaskPhase phase_ = TaskPhase::claimed;
    std::uint64_t processed_resources_ = 0U;
    std::uint64_t declared_output_bytes_ = 0U;
    std::vector<ResourceManifestPage> manifest_pages_;
    std::map<std::string, OutputDeclaration> declarations_;
    std::set<std::string> reported_outputs_;
};

class NormalizationV2TaskRuntime final {
public:
    using Executor = std::function<void(NormalizationV2LeaseSession&)>;

    NormalizationV2TaskRuntime(
            ClaimRequest capabilities,
            client::NormalizationV2ApiClient api_client);

    [[nodiscard]] bool runOnce(const std::string& now_utc,
                               const Executor& executor) const;

private:
    ClaimRequest capabilities_;
    client::NormalizationV2ApiClient api_client_;
};

}  // namespace clip_worker::normalization::v2
