#pragma once

#include "clip_worker/inspection/inspection_contract.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::inspection {

enum class TaskPhase {
    claimed,
    manifest_fetch,
    package_enumeration,
    resource_resolution,
    structure_classification,
    closure_validation,
    result_publication
};

enum class ExpansionUploadMode {
    server_copy_source,
    worker_put_archive_entry,
    worker_put_inline_data,
    already_staged
};

struct InspectionClaimRequest {
    std::string worker_id;
    std::string protocol_version = kProtocolVersion;
    std::string schema_sha256 = kSchemaSha256;
    std::vector<std::string> supported_inspector_versions;
    std::string resource_profile_version;
    std::string resource_profile_sha256;
    std::vector<ToolVersion> tool_versions;
};

struct InspectionClaimTask {
    std::string inspection_id;
    std::string request_id;
    std::string lease_token;
    std::string lease_expire_time;
    std::string hard_deadline_time;
    InspectionRequest inspection_request;
};

struct InspectionProgress {
    std::optional<std::uint64_t> total_resources;
    std::optional<std::uint64_t> processed_resources;
    std::optional<std::uint64_t> total_contents;
    std::optional<std::uint64_t> processed_contents;
};

struct InspectionHeartbeatResponse {
    std::string lease_expire_time;
    std::string hard_deadline_time;
    bool cancel_requested = false;
};

struct ExpansionPlanDescriptor {
    std::string plan_id;
    std::uint64_t record_count = 0U;
    std::uint32_t page_count = 0U;
    std::uint32_t page_size = 0U;
};

struct ExpansionPlanItem {
    std::string object_id;
    ExpansionUploadMode mode = ExpansionUploadMode::server_copy_source;
    std::uint64_t declared_size = 0U;
    std::string expected_sha256;
    std::optional<std::string> put_url;
};

struct ExpansionPlanPage {
    std::string plan_id;
    std::uint32_t page_number = 0U;
    std::uint32_t record_count = 0U;
    std::vector<ExpansionPlanItem> records;
};

struct ExpandedUploadReport {
    std::string object_id;
    std::uint64_t observed_size = 0U;
    std::string observed_sha256;
    std::string observed_etag;
};

[[nodiscard]] std::string serializeInspectionClaimRequest(
        const InspectionClaimRequest& request);
[[nodiscard]] InspectionClaimTask parseInspectionClaimTask(const std::string& json);
[[nodiscard]] std::string serializeInspectionHeartbeatRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, TaskPhase phase,
        const InspectionProgress& progress);
[[nodiscard]] InspectionHeartbeatResponse parseInspectionHeartbeatResponse(
        const std::string& json);
[[nodiscard]] std::string serializeInspectionResultPageRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const ResultPage& page);
[[nodiscard]] std::string serializeInspectionHierarchyPageRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const HierarchyPage& page);
[[nodiscard]] std::string serializeInspectionCompleteRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const InspectionResult& result);
[[nodiscard]] std::string serializeExpansionPrepareRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const InspectionResult& result,
        const std::string& result_envelope_sha256);
[[nodiscard]] ExpansionPlanDescriptor parseExpansionPlanDescriptor(
        const std::string& json);
[[nodiscard]] ExpansionPlanPage parseExpansionPlanPage(const std::string& json);
[[nodiscard]] std::string serializeExpandedUploadReport(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const std::string& plan_id,
        const ExpandedUploadReport& report);

void validateInspectionClaimTask(const InspectionClaimTask& task,
                                 const InspectionClaimRequest& capabilities,
                                 const std::string& now_utc);

}  // namespace clip_worker::inspection
