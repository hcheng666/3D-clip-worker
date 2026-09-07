#include "clip_worker/authorization/clipper_v2_runtime.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace clip_worker::authorization::v2 {
namespace {

constexpr long kBadRequest = 400;
constexpr long kUnauthorized = 401;
constexpr long kForbidden = 403;
constexpr long kNotFound = 404;
constexpr long kConflict = 409;

bool provesLeaseLoss(long status_code) {
    return status_code == kBadRequest || status_code == kUnauthorized
            || status_code == kForbidden || status_code == kNotFound
            || status_code == kConflict;
}

}  // namespace

ClipperV2LeaseSession::ClipperV2LeaseSession(
        const client::ClipperV2ApiClient& api_client,
        ClaimRequest capabilities, ClaimTask task,
        std::string validation_time_utc)
    : api_client_(api_client), capabilities_(std::move(capabilities)),
      task_(std::move(task)),
      validation_time_utc_(std::move(validation_time_utc)) {
    if (capabilities_.worker_id.empty()) {
        throw std::invalid_argument("Clipper V2 worker ID is required");
    }
}

SessionState ClipperV2LeaseSession::state() const noexcept {
    return state_;
}

bool ClipperV2LeaseSession::active() const noexcept {
    return state_ == SessionState::active;
}

TaskPhase ClipperV2LeaseSession::phase() const noexcept {
    return phase_;
}

const ClaimTask& ClipperV2LeaseSession::task() const noexcept {
    return task_;
}

const ClaimRequest& ClipperV2LeaseSession::capabilities() const noexcept {
    return capabilities_;
}

void ClipperV2LeaseSession::heartbeat(
        TaskPhase phase, std::uint64_t processed_bytes) {
    requireActive();
    const std::uint64_t maximum_progress = task_.input_size
            > std::numeric_limits<std::uint64_t>::max()
                    - task_.limits.maximum_aggregate_output_bytes
            ? std::numeric_limits<std::uint64_t>::max()
            : task_.input_size
                    + task_.limits.maximum_aggregate_output_bytes;
    if (static_cast<int>(phase) < static_cast<int>(phase_)
            || processed_bytes < processed_bytes_
            || processed_bytes > maximum_progress) {
        throw std::invalid_argument(
                "Clipper V2 progress must be monotonic");
    }
    try {
        api_client_.heartbeat(task_, capabilities_.worker_id,
                              phase, processed_bytes);
        phase_ = phase;
        processed_bytes_ = processed_bytes;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

OutputGrant ClipperV2LeaseSession::prepareOutput(
        const OutputDeclaration& declaration) {
    requireActive();
    validateOutputDeclaration(declaration, task_);
    const auto existing = declarations_.find(declaration.output_id);
    if (existing != declarations_.end()) {
        if (existing->second.evidence.output_sha256
                    != declaration.evidence.output_sha256
                || existing->second.evidence.output_size
                    != declaration.evidence.output_size) {
            throw std::invalid_argument(
                    "Clipper V2 repeated output declaration differs");
        }
    } else {
        if (declarations_.size() >= task_.limits.maximum_outputs
                || declaration.evidence.output_size
                        > task_.limits.maximum_aggregate_output_bytes
                                - declared_output_bytes_) {
            throw std::invalid_argument(
                    "Clipper V2 output set exceeds the task limit");
        }
        declarations_.emplace(declaration.output_id, declaration);
        declared_output_bytes_ += declaration.evidence.output_size;
    }
    try {
        auto grant = api_client_.prepareOutput(
                task_, capabilities_.worker_id, declaration);
        if (grant.output_id != declaration.output_id) {
            state_ = SessionState::lease_lost;
            throw std::invalid_argument(
                    "Clipper V2 grant output identity changed");
        }
        return grant;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void ClipperV2LeaseSession::reportOutput(const OutputReport& report) {
    requireActive();
    const auto declaration = declarations_.find(report.output_id);
    if (declaration == declarations_.end()
            || report.output_size
                    != declaration->second.evidence.output_size
            || report.output_sha256
                    != declaration->second.evidence.output_sha256) {
        throw std::invalid_argument(
                "Clipper V2 output report differs from declaration");
    }
    try {
        api_client_.reportOutput(task_, capabilities_.worker_id, report);
        reported_outputs_.insert(report.output_id);
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void ClipperV2LeaseSession::complete(
        CompletionOutcome outcome, const AuthorizationClipProof& proof,
        const std::vector<OutputDeclaration>& ordered_outputs) {
    requireActive();
    Completion completion;
    completion.outcome = outcome;
    completion.proof = proof;
    if (outcome == CompletionOutcome::clipped) {
        if (ordered_outputs.empty()
                || ordered_outputs.size() != declarations_.size()
                || reported_outputs_.size() != declarations_.size()) {
            throw std::logic_error(
                    "Clipper V2 cannot complete an open output set");
        }
        std::set<std::string> seen;
        for (const auto& output : ordered_outputs) {
            const auto declaration = declarations_.find(output.output_id);
            if (declaration == declarations_.end()
                    || declaration->second.evidence.output_sha256
                            != output.evidence.output_sha256
                    || !seen.insert(output.output_id).second
                    || reported_outputs_.count(output.output_id) == 0U) {
                throw std::invalid_argument(
                        "Clipper V2 completion output closure is inconsistent");
            }
            completion.ordered_output_ids.push_back(output.output_id);
        }
        completion.output_manifest_sha256 =
                outputManifestSha256(ordered_outputs);
    } else if (!ordered_outputs.empty() || !declarations_.empty()
               || !reported_outputs_.empty()) {
        throw std::logic_error(
                "Clipper V2 whole/empty outcome contains outputs");
    }
    try {
        api_client_.complete(task_, capabilities_.worker_id, completion);
        state_ = SessionState::completed;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, true);
        throw;
    } catch (...) {
        state_ = SessionState::lease_lost;
        throw;
    }
}

void ClipperV2LeaseSession::fail(const Failure& failure) {
    requireActive();
    try {
        api_client_.fail(task_, capabilities_.worker_id, failure);
        state_ = SessionState::failed;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, true);
        throw;
    } catch (...) {
        state_ = SessionState::lease_lost;
        throw;
    }
}

void ClipperV2LeaseSession::requireActive() const {
    if (!active()) {
        throw std::logic_error("Clipper V2 session is no longer active");
    }
}

void ClipperV2LeaseSession::handleApiFailure(
        const client::NormalizationApiError& error,
        bool terminal_operation) {
    if (terminal_operation || provesLeaseLoss(error.statusCode())) {
        state_ = SessionState::lease_lost;
    }
}

ClipperV2TaskRuntime::ClipperV2TaskRuntime(
        ClaimRequest capabilities,
        client::ClipperV2ApiClient api_client)
    : capabilities_(std::move(capabilities)),
      api_client_(std::move(api_client)) {
}

bool ClipperV2TaskRuntime::runOnce(
        const std::string& now_utc, const Executor& executor) const {
    if (!executor) {
        throw std::invalid_argument(
                "Clipper V2 executor callback is required");
    }
    const auto claimed = api_client_.claim(capabilities_);
    if (!claimed.has_value()) return false;
    validateClaimTask(*claimed, capabilities_, now_utc);
    ClipperV2LeaseSession session(
            api_client_, capabilities_, *claimed, now_utc);
    executor(session);
    return true;
}

}  // namespace clip_worker::authorization::v2
