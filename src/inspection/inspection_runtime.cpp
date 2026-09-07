#include "clip_worker/inspection/inspection_runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace clip_worker::inspection {
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

template <typename Page>
auto findPage(std::vector<Page>& pages, std::uint32_t page_number) {
    return std::find_if(pages.begin(), pages.end(), [page_number](const Page& page) {
        return page.page_number == page_number;
    });
}

}  // namespace

InspectionLeaseSession::InspectionLeaseSession(
        const client::InspectionApiClient& api_client,
        std::string worker_id, InspectionClaimTask task)
    : api_client_(api_client), worker_id_(std::move(worker_id)), task_(std::move(task)) {
    if (worker_id_.empty()) throw std::invalid_argument("Inspection worker ID is required");
}

InspectionSessionState InspectionLeaseSession::state() const noexcept {
    return state_;
}

bool InspectionLeaseSession::active() const noexcept {
    return state_ == InspectionSessionState::active;
}

const InspectionClaimTask& InspectionLeaseSession::task() const noexcept {
    return task_;
}

InspectionHeartbeatResponse InspectionLeaseSession::heartbeat(
        TaskPhase phase, const InspectionProgress& progress) {
    requireActive();
    try {
        auto response = api_client_.heartbeat(task_, worker_id_, phase, progress);
        if (response.hard_deadline_time != task_.hard_deadline_time
            || response.lease_expire_time > response.hard_deadline_time) {
            state_ = InspectionSessionState::lease_lost;
            throw std::invalid_argument("Inspection heartbeat changed the hard deadline");
        }
        task_.lease_expire_time = response.lease_expire_time;
        if (response.cancel_requested) {
            state_ = InspectionSessionState::cancelled;
        }
        return response;
    } catch (const client::InspectionApiError& error) {
        handleApiFailure(error, true);
        throw;
    } catch (...) {
        state_ = InspectionSessionState::lease_lost;
        throw;
    }
}

SourceManifestPage InspectionLeaseSession::sourceManifestPage(
        std::uint32_t page_number, const std::string& now_utc) {
    requireActive();
    try {
        auto page = api_client_.sourceManifestPage(task_, worker_id_, page_number);
        validateSourceManifestPage(page, task_.inspection_request, now_utc);
        if (!source_validation_time_.has_value()) source_validation_time_ = now_utc;
        rememberSourcePage(page);
        return page;
    } catch (const client::InspectionApiError& error) {
        handleApiFailure(error, false);
        throw;
    } catch (...) {
        state_ = InspectionSessionState::lease_lost;
        throw;
    }
}

void InspectionLeaseSession::submitResultPage(const ResultPage& page) {
    requireActive();
    if (page.inspection_id != task_.inspection_id || page.request_id != task_.request_id
        || page.protocol_version != kProtocolVersion
        || page.record_count != page.records.size()
        || page.page_sha256 != canonicalPageSha256(page.records)) {
        throw std::invalid_argument("Inspection result page is inconsistent");
    }
    try {
        api_client_.submitResultPage(task_, worker_id_, page);
        rememberResultPage(page);
    } catch (const client::InspectionApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void InspectionLeaseSession::submitHierarchyPage(const HierarchyPage& page) {
    requireActive();
    if (page.inspection_id != task_.inspection_id
        || page.request_id != task_.request_id
        || page.protocol_version != kProtocolVersion
        || page.record_count != page.records.size()
        || page.page_sha256 != canonicalPageSha256(page.records)) {
        throw std::invalid_argument("Inspection hierarchy page is inconsistent");
    }
    try {
        api_client_.submitHierarchyPage(task_, worker_id_, page);
        rememberHierarchyPage(page);
    } catch (const client::InspectionApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

ExpansionPlanDescriptor InspectionLeaseSession::prepareExpansion(
        const InspectionResult& result,
        const std::string& result_envelope_sha256) {
    requireActive();
    try {
        return api_client_.prepareExpansion(task_, worker_id_, result,
                                            result_envelope_sha256);
    } catch (const client::InspectionApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

ExpansionPlanPage InspectionLeaseSession::expansionPlanPage(
        const std::string& plan_id, std::uint32_t page_number) {
    requireActive();
    try {
        return api_client_.expansionPlanPage(task_, worker_id_, plan_id,
                                             page_number);
    } catch (const client::InspectionApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void InspectionLeaseSession::reportExpandedUpload(
        const std::string& plan_id, const ExpandedUploadReport& report) {
    requireActive();
    try {
        api_client_.reportExpandedUpload(task_, worker_id_, plan_id, report);
    } catch (const client::InspectionApiError& error) {
        handleApiFailure(error, false);
        throw;
    }
}

void InspectionLeaseSession::complete(
        const InspectionResult& result, const std::string& now_utc) {
    requireActive();
    validateCompletedExchange(task_.inspection_request, source_pages_, result,
                              result_pages_, hierarchy_pages_,
                              source_validation_time_.value_or(now_utc));
    try {
        api_client_.complete(task_, worker_id_, result);
        state_ = InspectionSessionState::completed;
    } catch (const client::InspectionApiError& error) {
        handleApiFailure(error, true);
        throw;
    } catch (...) {
        state_ = InspectionSessionState::lease_lost;
        throw;
    }
}

void InspectionLeaseSession::requireActive() const {
    if (!active()) {
        throw std::logic_error("Inspection session is no longer active");
    }
}

void InspectionLeaseSession::rememberSourcePage(const SourceManifestPage& page) {
    auto existing = findPage(source_pages_, page.page_number);
    if (existing == source_pages_.end()) {
        source_pages_.push_back(page);
    } else if (existing->page_sha256 != page.page_sha256) {
        state_ = InspectionSessionState::lease_lost;
        throw std::invalid_argument("Inspection source page changed during replay");
    }
}

void InspectionLeaseSession::rememberResultPage(const ResultPage& page) {
    auto existing = findPage(result_pages_, page.page_number);
    if (existing == result_pages_.end()) {
        result_pages_.push_back(page);
    } else if (existing->page_sha256 != page.page_sha256) {
        throw std::invalid_argument("Inspection result page changed during replay");
    }
}

void InspectionLeaseSession::rememberHierarchyPage(const HierarchyPage& page) {
    auto existing = findPage(hierarchy_pages_, page.page_number);
    if (existing == hierarchy_pages_.end()) {
        hierarchy_pages_.push_back(page);
    } else if (existing->page_sha256 != page.page_sha256) {
        throw std::invalid_argument("Inspection hierarchy page changed during replay");
    }
}

void InspectionLeaseSession::handleApiFailure(
        const client::InspectionApiError& error, bool terminal_operation) {
    if (terminal_operation || provesLeaseLoss(error.statusCode())) {
        state_ = InspectionSessionState::lease_lost;
    }
}

InspectionTaskRuntime::InspectionTaskRuntime(
        InspectionClaimRequest capabilities,
        client::InspectionApiClient api_client)
    : capabilities_(std::move(capabilities)), api_client_(std::move(api_client)) {
}

bool InspectionTaskRuntime::runOnce(
        const std::string& now_utc, const Executor& executor) const {
    if (!executor) throw std::invalid_argument("Inspection executor callback is required");
    const auto claimed = api_client_.claim(capabilities_);
    if (!claimed.has_value()) return false;
    validateInspectionClaimTask(*claimed, capabilities_, now_utc);
    InspectionLeaseSession session(api_client_, capabilities_.worker_id, *claimed);
    executor(session);
    return true;
}

}  // namespace clip_worker::inspection
