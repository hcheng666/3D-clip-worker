#pragma once

#include "clip_worker/client/inspection_api_client.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::inspection {

enum class InspectionSessionState { active, cancelled, lease_lost, completed };

/**
 * Lease-bound seam used by the future package executor. It keeps pagination
 * bounded and prevents source/result activity after cancellation or lease loss.
 */
class InspectionLeaseSession final {
public:
    InspectionLeaseSession(const client::InspectionApiClient& api_client,
                           std::string worker_id,
                           InspectionClaimTask task);

    [[nodiscard]] InspectionSessionState state() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const InspectionClaimTask& task() const noexcept;

    [[nodiscard]] InspectionHeartbeatResponse heartbeat(
            TaskPhase phase, const InspectionProgress& progress);
    [[nodiscard]] SourceManifestPage sourceManifestPage(
            std::uint32_t page_number, const std::string& now_utc);
    void submitResultPage(const ResultPage& page);
    void submitHierarchyPage(const HierarchyPage& page);
    [[nodiscard]] ExpansionPlanDescriptor prepareExpansion(
            const InspectionResult& result,
            const std::string& result_envelope_sha256);
    [[nodiscard]] ExpansionPlanPage expansionPlanPage(
            const std::string& plan_id, std::uint32_t page_number);
    void reportExpandedUpload(const std::string& plan_id,
                              const ExpandedUploadReport& report);
    void complete(const InspectionResult& result, const std::string& now_utc);

private:
    void requireActive() const;
    void rememberSourcePage(const SourceManifestPage& page);
    void rememberResultPage(const ResultPage& page);
    void rememberHierarchyPage(const HierarchyPage& page);
    void handleApiFailure(const client::InspectionApiError& error,
                          bool terminal_operation);

    const client::InspectionApiClient& api_client_;
    std::string worker_id_;
    InspectionClaimTask task_;
    InspectionSessionState state_ = InspectionSessionState::active;
    std::vector<SourceManifestPage> source_pages_;
    std::vector<ResultPage> result_pages_;
    std::vector<HierarchyPage> hierarchy_pages_;
    std::optional<std::string> source_validation_time_;
};

class InspectionTaskRuntime final {
public:
    using Executor = std::function<void(InspectionLeaseSession&)>;

    InspectionTaskRuntime(InspectionClaimRequest capabilities,
                          client::InspectionApiClient api_client);

    /** Claims and invokes one executor callback; no production loop is wired yet. */
    [[nodiscard]] bool runOnce(const std::string& now_utc,
                               const Executor& executor) const;

private:
    InspectionClaimRequest capabilities_;
    client::InspectionApiClient api_client_;
};

}  // namespace clip_worker::inspection
