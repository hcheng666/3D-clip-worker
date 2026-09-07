#pragma once

#include "clip_worker/authorization/clipper_v2_contract.hpp"
#include "clip_worker/client/clipper_v2_api_client.hpp"

#include <functional>
#include <map>
#include <set>

namespace clip_worker::authorization::v2 {

enum class SessionState { active, completed, failed, lease_lost };

/** Lease-bound Clipper V2 session with an exact multi-output closure. */
class ClipperV2LeaseSession final {
public:
    ClipperV2LeaseSession(
            const client::ClipperV2ApiClient& api_client,
            ClaimRequest capabilities, ClaimTask task,
            std::string validation_time_utc);

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] TaskPhase phase() const noexcept;
    [[nodiscard]] const ClaimTask& task() const noexcept;
    [[nodiscard]] const ClaimRequest& capabilities() const noexcept;

    void heartbeat(TaskPhase phase, std::uint64_t processed_bytes);
    [[nodiscard]] OutputGrant prepareOutput(
            const OutputDeclaration& declaration);
    void reportOutput(const OutputReport& report);
    void complete(CompletionOutcome outcome,
                  const AuthorizationClipProof& proof,
                  const std::vector<OutputDeclaration>& ordered_outputs = {});
    void fail(const Failure& failure);

private:
    void requireActive() const;
    void handleApiFailure(const client::NormalizationApiError& error,
                          bool terminal_operation);

    const client::ClipperV2ApiClient& api_client_;
    ClaimRequest capabilities_;
    ClaimTask task_;
    std::string validation_time_utc_;
    SessionState state_ = SessionState::active;
    TaskPhase phase_ = TaskPhase::claimed;
    std::uint64_t processed_bytes_ = 0U;
    std::uint64_t declared_output_bytes_ = 0U;
    std::map<std::string, OutputDeclaration> declarations_;
    std::set<std::string> reported_outputs_;
};

class ClipperV2TaskRuntime final {
public:
    using Executor = std::function<void(ClipperV2LeaseSession&)>;

    ClipperV2TaskRuntime(
            ClaimRequest capabilities,
            client::ClipperV2ApiClient api_client);

    [[nodiscard]] bool runOnce(const std::string& now_utc,
                               const Executor& executor) const;

private:
    ClaimRequest capabilities_;
    client::ClipperV2ApiClient api_client_;
};

}  // namespace clip_worker::authorization::v2
