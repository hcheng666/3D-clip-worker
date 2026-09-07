#include "clip_worker/normalization/normalization_v2_runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace clip_worker::normalization::v2 {
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

NormalizationV2LeaseSession::NormalizationV2LeaseSession(
        const client::NormalizationV2ApiClient& api_client,
        ClaimRequest capabilities, ClaimTask task,
        std::string validation_time_utc)
    : api_client_(api_client),
      capabilities_(std::move(capabilities)),
      task_(std::move(task)),
      validation_time_utc_(std::move(validation_time_utc)) {
    if (capabilities_.worker_id.empty()) {
        throw std::invalid_argument("Normalizer V2 worker ID is required");
    }
}

SessionState NormalizationV2LeaseSession::state() const noexcept {
    return state_;
}

bool NormalizationV2LeaseSession::active() const noexcept {
    return state_ == SessionState::active;
}

const ClaimTask& NormalizationV2LeaseSession::task() const noexcept {
    return task_;
}

const ClaimRequest& NormalizationV2LeaseSession::capabilities() const noexcept {
    return capabilities_;
}

const std::vector<ResourceManifestPage>&
NormalizationV2LeaseSession::manifestPages() const {
    return manifest_pages_;
}

HeartbeatResponse NormalizationV2LeaseSession::heartbeat(
        TaskPhase phase, std::uint64_t processed_resources) {
    requireActive();
    if (static_cast<int>(phase) < static_cast<int>(phase_)
            || processed_resources < processed_resources_
            || processed_resources > task_.resource_manifest.record_count) {
        throw std::invalid_argument(
                "Normalizer V2 progress must be monotonic");
    }
    try {
        auto response = api_client_.heartbeat(
                task_, capabilities_.worker_id, phase,
                task_.resource_manifest.record_count, processed_resources);
        if (response.hard_deadline_time != task_.hard_deadline_time
                || response.lease_expire_time > response.hard_deadline_time) {
            state_ = SessionState::lease_lost;
            throw std::invalid_argument(
                    "Normalizer V2 heartbeat changed the hard deadline");
        }
        task_.lease_expire_time = response.lease_expire_time;
        phase_ = phase;
        processed_resources_ = processed_resources;
        if (response.cancel_requested) state_ = SessionState::cancelled;
        return response;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, true);
        throw;
    } catch (...) {
        state_ = SessionState::lease_lost;
        throw;
    }
}

ResourceManifestPage NormalizationV2LeaseSession::resourceManifestPage(
        std::uint32_t page_number) {
    requireActive();
    try {
        auto page = api_client_.resourceManifestPage(
                task_, capabilities_.worker_id, page_number);
        validateResourceManifestPage(page, task_, validation_time_utc_);
        auto existing = std::find_if(
                manifest_pages_.begin(), manifest_pages_.end(),
                [page_number](const ResourceManifestPage& candidate) {
                    return candidate.page_number == page_number;
                });
        if (existing == manifest_pages_.end()) {
            manifest_pages_.push_back(page);
        } else if (existing->page_sha256 != page.page_sha256) {
            state_ = SessionState::lease_lost;
            throw std::invalid_argument(
                    "Normalizer V2 manifest page changed during replay");
        }
        return page;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, false);
        throw;
    } catch (...) {
        state_ = SessionState::lease_lost;
        throw;
    }
}

void NormalizationV2LeaseSession::validateManifest() const {
    requireActive();
    validateResourceManifest(task_, manifest_pages_, validation_time_utc_,
                             taskCapability().maximum_input_bytes);
}

OutputGrant NormalizationV2LeaseSession::prepareOutput(
        const OutputDeclaration& declaration) {
    requireActive();
    validateManifest();
    validateOutputDeclaration(declaration, task_, capabilities_);
    const auto existing = declarations_.find(declaration.output_id);
    if (existing != declarations_.end()) {
        if (existing->second.output_sha256 != declaration.output_sha256
                || existing->second.output_size != declaration.output_size) {
            throw std::invalid_argument(
                    "Normalizer V2 repeated output declaration differs");
        }
    } else {
        if (declarations_.size() >= task_.maximum_outputs
                || declaration.output_size
                        > taskCapability().maximum_output_bytes
                                - declared_output_bytes_) {
            throw std::invalid_argument(
                    "Normalizer V2 output set exceeds the task limit");
        }
        declarations_.emplace(declaration.output_id, declaration);
        declared_output_bytes_ += declaration.output_size;
    }
    try {
        auto grant = api_client_.prepareOutput(
                task_, capabilities_.worker_id, declaration);
        if (grant.output_id != declaration.output_id) {
            state_ = SessionState::lease_lost;
            throw std::invalid_argument(
                    "Normalizer V2 grant output identity changed");
        }
        return grant;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void NormalizationV2LeaseSession::reportOutput(const OutputReport& report) {
    requireActive();
    const auto declaration = declarations_.find(report.output_id);
    if (declaration == declarations_.end()
            || report.output_size != declaration->second.output_size
            || report.output_sha256 != declaration->second.output_sha256) {
        throw std::invalid_argument(
                "Normalizer V2 output report differs from declaration");
    }
    try {
        api_client_.reportOutput(task_, capabilities_.worker_id, report);
        reported_outputs_.insert(report.output_id);
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void NormalizationV2LeaseSession::complete(
        const std::vector<OutputDeclaration>& ordered_outputs,
        bool global_preview_only) {
    requireActive();
    if (ordered_outputs.empty()
            || ordered_outputs.size() != declarations_.size()
            || reported_outputs_.size() != declarations_.size()) {
        throw std::logic_error(
                "Normalizer V2 cannot complete an open output set");
    }
    Completion completion;
    std::set<std::string> seen;
    for (const auto& output : ordered_outputs) {
        const auto declaration = declarations_.find(output.output_id);
        if (declaration == declarations_.end()
                || declaration->second.output_sha256 != output.output_sha256
                || !seen.insert(output.output_id).second
                || reported_outputs_.find(output.output_id)
                        == reported_outputs_.end()) {
            throw std::invalid_argument(
                    "Normalizer V2 completion output closure is inconsistent");
        }
        completion.ordered_output_ids.push_back(output.output_id);
    }
    completion.output_manifest_sha256 =
            outputManifestSha256(ordered_outputs);
    completion.global_preview_only = global_preview_only;
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

void NormalizationV2LeaseSession::fail(const Failure& failure) {
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

void NormalizationV2LeaseSession::requireActive() const {
    if (!active()) {
        throw std::logic_error("Normalizer V2 session is no longer active");
    }
}

void NormalizationV2LeaseSession::handleApiFailure(
        const client::NormalizationApiError& error,
        bool terminal_operation) {
    if (terminal_operation || provesLeaseLoss(error.statusCode())) {
        state_ = SessionState::lease_lost;
    }
}

const FamilyCapability& NormalizationV2LeaseSession::taskCapability() const {
    const auto found = std::find_if(
            capabilities_.family_capabilities.begin(),
            capabilities_.family_capabilities.end(),
            [this](const FamilyCapability& capability) {
                return capability.canonical_family == task_.canonical_family
                        && capability.normalization_version
                                == task_.normalization_version
                        && capability.canonical_contract_version
                                == task_.canonical_contract_version;
            });
    if (found == capabilities_.family_capabilities.end()) {
        throw std::logic_error(
                "Normalizer V2 task capability is unavailable");
    }
    return *found;
}

NormalizationV2TaskRuntime::NormalizationV2TaskRuntime(
        ClaimRequest capabilities,
        client::NormalizationV2ApiClient api_client)
    : capabilities_(std::move(capabilities)),
      api_client_(std::move(api_client)) {
}

bool NormalizationV2TaskRuntime::runOnce(
        const std::string& now_utc, const Executor& executor) const {
    if (!executor) {
        throw std::invalid_argument(
                "Normalizer V2 executor callback is required");
    }
    const auto claimed = api_client_.claim(capabilities_);
    if (!claimed.has_value()) return false;
    validateClaimTask(*claimed, capabilities_, now_utc);
    NormalizationV2LeaseSession session(
            api_client_, capabilities_, *claimed, now_utc);
    executor(session);
    return true;
}

}  // namespace clip_worker::normalization::v2
