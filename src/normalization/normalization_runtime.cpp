#include "clip_worker/normalization/normalization_runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace clip_worker::normalization {
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

NormalizationLeaseSession::NormalizationLeaseSession(
        const client::NormalizationApiClient& api_client,
        std::string worker_id, ClaimTask task, std::string validation_time_utc,
        std::uint64_t maximum_input_bytes,
        std::uint64_t maximum_output_bytes)
    : api_client_(api_client),
      worker_id_(std::move(worker_id)),
      task_(std::move(task)),
      validation_time_utc_(std::move(validation_time_utc)),
      maximum_input_bytes_(maximum_input_bytes),
      maximum_output_bytes_(maximum_output_bytes) {
    if (worker_id_.empty() || maximum_input_bytes_ == 0U
            || maximum_output_bytes_ == 0U) {
        throw std::invalid_argument("Normalizer worker ID is required");
    }
}

SessionState NormalizationLeaseSession::state() const noexcept {
    return state_;
}

bool NormalizationLeaseSession::active() const noexcept {
    return state_ == SessionState::active;
}

const ClaimTask& NormalizationLeaseSession::task() const noexcept {
    return task_;
}

const std::vector<ResourceManifestPage>&
NormalizationLeaseSession::manifestPages() const {
    return manifest_pages_;
}

HeartbeatResponse NormalizationLeaseSession::heartbeat(
        TaskPhase phase, std::uint64_t processed_resources) {
    requireActive();
    if (static_cast<int>(phase) < static_cast<int>(phase_)
            || processed_resources < processed_resources_
            || processed_resources > task_.resource_manifest.record_count) {
        throw std::invalid_argument("Normalizer progress must be monotonic");
    }
    try {
        auto response = api_client_.heartbeat(
                task_, worker_id_, phase, task_.resource_manifest.record_count,
                processed_resources);
        if (response.hard_deadline_time != task_.hard_deadline_time
                || response.lease_expire_time > response.hard_deadline_time) {
            state_ = SessionState::lease_lost;
            throw std::invalid_argument(
                    "Normalizer heartbeat changed the hard deadline");
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

ResourceManifestPage NormalizationLeaseSession::resourceManifestPage(
        std::uint32_t page_number) {
    requireActive();
    try {
        auto page = api_client_.resourceManifestPage(task_, worker_id_, page_number);
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
                    "Normalizer manifest page changed during replay");
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

void NormalizationLeaseSession::validateManifest() const {
    requireActive();
    validateResourceManifest(task_, manifest_pages_, validation_time_utc_,
                             maximum_input_bytes_);
}

UploadGrant NormalizationLeaseSession::prepareUpload(
        const UploadDeclaration& declaration) {
    requireActive();
    validateManifest();
    if (declaration.output_size > maximum_output_bytes_) {
        throw std::invalid_argument(
                "Normalizer output exceeds the advertised family limit");
    }
    try {
        auto grant = api_client_.prepareUpload(
                task_, worker_id_, declaration);
        upload_declaration_ = declaration;
        return grant;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void NormalizationLeaseSession::reportUpload(const UploadReport& report) {
    requireActive();
    if (!upload_declaration_.has_value()
            || report.output_size != upload_declaration_->output_size
            || report.output_sha256 != upload_declaration_->output_sha256) {
        throw std::invalid_argument(
                "Normalizer upload report differs from its declaration");
    }
    try {
        api_client_.reportUpload(task_, worker_id_, report);
        upload_reported_ = true;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void NormalizationLeaseSession::complete() {
    requireActive();
    if (!upload_reported_) {
        throw std::logic_error(
                "Normalizer cannot complete before a verified upload report");
    }
    try {
        api_client_.complete(task_, worker_id_);
        state_ = SessionState::completed;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, true);
        throw;
    } catch (...) {
        state_ = SessionState::lease_lost;
        throw;
    }
}

void NormalizationLeaseSession::fail(const Failure& failure) {
    requireActive();
    try {
        api_client_.fail(task_, worker_id_, failure);
        state_ = SessionState::failed;
    } catch (const client::NormalizationApiError& error) {
        handleApiFailure(error, true);
        throw;
    } catch (...) {
        state_ = SessionState::lease_lost;
        throw;
    }
}

void NormalizationLeaseSession::requireActive() const {
    if (!active()) {
        throw std::logic_error("Normalizer session is no longer active");
    }
}

void NormalizationLeaseSession::handleApiFailure(
        const client::NormalizationApiError& error, bool terminal_operation) {
    if (terminal_operation || provesLeaseLoss(error.statusCode())) {
        state_ = SessionState::lease_lost;
    }
}

NormalizationTaskRuntime::NormalizationTaskRuntime(
        ClaimRequest capabilities, client::NormalizationApiClient api_client)
    : capabilities_(std::move(capabilities)),
      api_client_(std::move(api_client)) {
}

bool NormalizationTaskRuntime::runOnce(
        const std::string& now_utc, const Executor& executor) const {
    if (!executor) {
        throw std::invalid_argument("Normalizer executor callback is required");
    }
    const auto claimed = api_client_.claim(capabilities_);
    if (!claimed.has_value()) return false;
    validateClaimTask(*claimed, capabilities_, now_utc);
    const auto family = std::find_if(
            capabilities_.family_capabilities.begin(),
            capabilities_.family_capabilities.end(),
            [&claimed](const FamilyCapability& capability) {
                return capability.canonical_family == claimed->canonical_family
                        && capability.canonical_contract_version
                                == claimed->canonical_contract_version;
            });
    if (family == capabilities_.family_capabilities.end()) {
        throw std::invalid_argument(
                "Normalizer claimed family capability is unavailable");
    }
    NormalizationLeaseSession session(
            api_client_, capabilities_.worker_id, *claimed, now_utc,
            family->maximum_input_bytes, family->maximum_output_bytes);
    executor(session);
    return true;
}

}  // namespace clip_worker::normalization
