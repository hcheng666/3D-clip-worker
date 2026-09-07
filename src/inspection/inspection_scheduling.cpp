#include "clip_worker/inspection/inspection_scheduling.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace clip_worker::inspection {
namespace {

using Json = nlohmann::json;

template <typename Enum>
using EnumEntry = std::pair<std::string_view, Enum>;

constexpr std::array<EnumEntry<TaskPhase>, 7U> kTaskPhases = {{
        {"CLAIMED", TaskPhase::claimed},
        {"MANIFEST_FETCH", TaskPhase::manifest_fetch},
        {"PACKAGE_ENUMERATION", TaskPhase::package_enumeration},
        {"RESOURCE_RESOLUTION", TaskPhase::resource_resolution},
        {"STRUCTURE_CLASSIFICATION", TaskPhase::structure_classification},
        {"CLOSURE_VALIDATION", TaskPhase::closure_validation},
        {"RESULT_PUBLICATION", TaskPhase::result_publication}}};

constexpr std::array<EnumEntry<ExpansionUploadMode>, 4U> kExpansionModes = {{
        {"SERVER_COPY_SOURCE", ExpansionUploadMode::server_copy_source},
        {"WORKER_PUT_ARCHIVE_ENTRY", ExpansionUploadMode::worker_put_archive_entry},
        {"WORKER_PUT_INLINE_DATA", ExpansionUploadMode::worker_put_inline_data},
        {"ALREADY_STAGED", ExpansionUploadMode::already_staged}}};

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

Json parseObject(const std::string& text) {
    try {
        Json value = Json::parse(text);
        require(value.is_object(), "Inspection scheduling message must be an object");
        return value;
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Inspection scheduling JSON is invalid");
    }
}

void requireOnly(const Json& value, std::initializer_list<const char*> allowed) {
    std::set<std::string> names;
    for (const char* name : allowed) names.emplace(name);
    for (const auto& item : value.items()) {
        require(names.find(item.key()) != names.end(),
                "Inspection scheduling message contains an unknown field");
    }
}

template <typename Value>
Value required(const Json& value, const char* field) {
    const auto item = value.find(field);
    require(item != value.end() && !item->is_null(),
            "Inspection scheduling message is missing a field");
    try {
        return item->get<Value>();
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Inspection scheduling field has an invalid type");
    }
}

template <typename Enum, std::size_t Size>
Enum parseEnum(const std::string& value,
               const std::array<EnumEntry<Enum>, Size>& values) {
    for (const auto& entry : values) {
        if (entry.first == value) return entry.second;
    }
    throw std::invalid_argument("Inspection scheduling enum is invalid");
}

std::string taskPhaseName(TaskPhase phase) {
    for (const auto& entry : kTaskPhases) {
        if (entry.second == phase) return std::string(entry.first);
    }
    throw std::invalid_argument("Inspection task phase is invalid");
}

std::string toolName(ToolName name) {
    switch (name) {
        case ToolName::inspector: return "INSPECTOR";
        case ToolName::archive_reader: return "ARCHIVE_READER";
        case ToolName::three_d_tiles_validator: return "THREE_D_TILES_VALIDATOR";
        case ToolName::gltf_validator: return "GLTF_VALIDATOR";
    }
    throw std::invalid_argument("Inspection tool name is invalid");
}

Json toolJson(const ToolVersion& tool) {
    Json json = {{"name", toolName(tool.name)}, {"version", tool.version}};
    if (tool.build_sha256.has_value()) json["buildSha256"] = *tool.build_sha256;
    return json;
}

void requireText(const std::string& value, const char* message) {
    require(!value.empty() && std::any_of(value.begin(), value.end(), [](unsigned char ch) {
                return std::isspace(ch) == 0;
            }), message);
}

void requireSha256(const std::string& value) {
    require(value.size() == 64U
                    && std::all_of(value.begin(), value.end(), [](char current) {
                           return (current >= '0' && current <= '9')
                                   || (current >= 'a' && current <= 'f');
                       }),
            "Inspection scheduling hash must be lowercase SHA-256");
}

bool sameTool(const ToolVersion& left, const ToolVersion& right) {
    return left.name == right.name && left.version == right.version
            && left.build_sha256 == right.build_sha256;
}

void validateFixedUtc(const std::string& value) {
    require(value.size() == 20U && value[4] == '-' && value[7] == '-'
                    && value[10] == 'T' && value[13] == ':' && value[16] == ':'
                    && value[19] == 'Z',
            "Inspection scheduling time must be UTC seconds");
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (index == 4U || index == 7U || index == 10U || index == 13U
            || index == 16U || index == 19U) {
            continue;
        }
        require(value[index] >= '0' && value[index] <= '9',
                "Inspection scheduling time contains an invalid digit");
    }
    static_cast<void>(utcEpochSeconds(value));
}

}  // namespace

std::string serializeInspectionClaimRequest(const InspectionClaimRequest& request) {
    requireText(request.worker_id, "Inspection worker ID must not be blank");
    require(request.protocol_version == kProtocolVersion,
            "Inspection claim protocol version is incompatible");
    require(request.schema_sha256 == kSchemaSha256,
            "Inspection claim schema digest is incompatible");
    require(!request.supported_inspector_versions.empty(),
            "Inspection claim requires an inspector version");
    requireText(request.resource_profile_version,
                "Inspection resource profile version must not be blank");
    requireSha256(request.resource_profile_sha256);
    Json tools = Json::array();
    for (const auto& tool : request.tool_versions) tools.push_back(toolJson(tool));
    return Json({{"workerId", request.worker_id},
                 {"protocolVersion", request.protocol_version},
                 {"schemaSha256", request.schema_sha256},
                 {"supportedInspectorVersions", request.supported_inspector_versions},
                 {"resourceProfileVersion", request.resource_profile_version},
                 {"resourceProfileSha256", request.resource_profile_sha256},
                 {"toolVersions", std::move(tools)}})
            .dump();
}

InspectionClaimTask parseInspectionClaimTask(const std::string& text) {
    const auto json = parseObject(text);
    requireOnly(json, {"inspectionId", "requestId", "leaseToken", "leaseExpireTime",
                       "hardDeadlineTime", "inspectionRequest"});
    InspectionClaimTask task;
    task.inspection_id = required<std::string>(json, "inspectionId");
    task.request_id = required<std::string>(json, "requestId");
    task.lease_token = required<std::string>(json, "leaseToken");
    task.lease_expire_time = required<std::string>(json, "leaseExpireTime");
    task.hard_deadline_time = required<std::string>(json, "hardDeadlineTime");
    task.inspection_request = parseInspectionRequest(
            required<Json>(json, "inspectionRequest").dump());
    return task;
}

std::string serializeInspectionHeartbeatRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, TaskPhase phase,
        const InspectionProgress& progress) {
    Json json = {{"workerId", worker_id},
                 {"leaseToken", lease_token},
                 {"requestId", request_id},
                 {"phase", taskPhaseName(phase)}};
    if (progress.total_resources.has_value()) {
        json["totalResources"] = *progress.total_resources;
    }
    if (progress.processed_resources.has_value()) {
        json["processedResources"] = *progress.processed_resources;
    }
    if (progress.total_contents.has_value()) json["totalContents"] = *progress.total_contents;
    if (progress.processed_contents.has_value()) {
        json["processedContents"] = *progress.processed_contents;
    }
    return json.dump();
}

InspectionHeartbeatResponse parseInspectionHeartbeatResponse(const std::string& text) {
    const auto json = parseObject(text);
    requireOnly(json, {"leaseExpireTime", "hardDeadlineTime", "cancelRequested"});
    InspectionHeartbeatResponse response;
    response.lease_expire_time = required<std::string>(json, "leaseExpireTime");
    response.hard_deadline_time = required<std::string>(json, "hardDeadlineTime");
    response.cancel_requested = required<bool>(json, "cancelRequested");
    validateFixedUtc(response.lease_expire_time);
    validateFixedUtc(response.hard_deadline_time);
    require(utcEpochSeconds(response.lease_expire_time)
                    <= utcEpochSeconds(response.hard_deadline_time),
            "Inspection lease exceeds its hard deadline");
    return response;
}

std::string serializeInspectionResultPageRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const ResultPage& page) {
    return Json({{"workerId", worker_id},
                 {"leaseToken", lease_token},
                 {"requestId", request_id},
                 {"resultPage", Json::parse(serializeResultPage(page))}})
            .dump();
}

std::string serializeInspectionHierarchyPageRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const HierarchyPage& page) {
    return Json({{"workerId", worker_id},
                 {"leaseToken", lease_token},
                 {"requestId", request_id},
                 {"hierarchyPage", Json::parse(serializeHierarchyPage(page))}})
            .dump();
}

std::string serializeInspectionCompleteRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const InspectionResult& result) {
    return Json({{"workerId", worker_id},
                 {"leaseToken", lease_token},
                 {"requestId", request_id},
                 {"result", Json::parse(serializeInspectionResult(result))}})
            .dump();
}

std::string serializeExpansionPrepareRequest(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id,
        const InspectionResult& result,
        const std::string& result_envelope_sha256) {
    requireText(worker_id, "Inspection worker ID must not be blank");
    requireText(lease_token, "Inspection lease token must not be blank");
    requireText(request_id, "Inspection request ID must not be blank");
    requireSha256(result_envelope_sha256);
    return Json({{"workerId", worker_id},
                 {"leaseToken", lease_token},
                 {"requestId", request_id},
                 {"result", Json::parse(serializeInspectionResult(result))},
                 {"resultEnvelopeSha256", result_envelope_sha256}})
            .dump();
}

ExpansionPlanDescriptor parseExpansionPlanDescriptor(const std::string& text) {
    const Json json = parseObject(text);
    requireOnly(json, {"planId", "recordCount", "pageCount", "pageSize"});
    ExpansionPlanDescriptor descriptor;
    descriptor.plan_id = required<std::string>(json, "planId");
    descriptor.record_count = required<std::uint64_t>(json, "recordCount");
    descriptor.page_count = required<std::uint32_t>(json, "pageCount");
    descriptor.page_size = required<std::uint32_t>(json, "pageSize");
    requireText(descriptor.plan_id, "Expansion plan ID must not be blank");
    require(descriptor.record_count <= ProtocolLimits::kMaximumResourceCount
                    && descriptor.page_size > 0U
                    && descriptor.page_size
                            <= ProtocolLimits::kMaximumResultPageRecords,
            "Expansion plan descriptor is outside bounds");
    const std::uint64_t expected_pages = descriptor.record_count == 0U
            ? 0U : (descriptor.record_count + descriptor.page_size - 1U)
                    / descriptor.page_size;
    require(expected_pages == descriptor.page_count,
            "Expansion plan descriptor counts are inconsistent");
    return descriptor;
}

ExpansionPlanPage parseExpansionPlanPage(const std::string& text) {
    const Json json = parseObject(text);
    requireOnly(json, {"planId", "pageNumber", "recordCount", "records"});
    ExpansionPlanPage page;
    page.plan_id = required<std::string>(json, "planId");
    page.page_number = required<std::uint32_t>(json, "pageNumber");
    page.record_count = required<std::uint32_t>(json, "recordCount");
    const Json records = required<Json>(json, "records");
    requireText(page.plan_id, "Expansion plan ID must not be blank");
    require(records.is_array() && !records.empty()
                    && records.size() <= ProtocolLimits::kMaximumResultPageRecords
                    && records.size() == page.record_count,
            "Expansion plan page records are outside bounds");
    std::set<std::string> object_ids;
    for (const auto& record : records) {
        require(record.is_object(), "Expansion plan item must be an object");
        requireOnly(record, {"objectId", "mode", "declaredSize",
                             "expectedSha256", "putUrl"});
        ExpansionPlanItem item;
        item.object_id = required<std::string>(record, "objectId");
        item.mode = parseEnum(required<std::string>(record, "mode"),
                              kExpansionModes);
        item.declared_size = required<std::uint64_t>(record, "declaredSize");
        item.expected_sha256 = required<std::string>(record, "expectedSha256");
        if (record.contains("putUrl")) {
            item.put_url = required<std::string>(record, "putUrl");
        }
        requireText(item.object_id, "Expansion plan object ID must not be blank");
        requireSha256(item.expected_sha256);
        require(item.declared_size <= ProtocolLimits::kMaximumExpandedResourceBytes,
                "Expansion plan item size is outside bounds");
        const bool worker_put = item.mode
                        == ExpansionUploadMode::worker_put_archive_entry
                || item.mode == ExpansionUploadMode::worker_put_inline_data;
        require(worker_put == item.put_url.has_value(),
                "Expansion plan PUT grant scope is invalid");
        if (item.put_url.has_value()) {
            require(item.put_url->size() <= ProtocolLimits::kMaximumGrantUrlUtf8Bytes
                            && (item.put_url->rfind("http://", 0U) == 0U
                                || item.put_url->rfind("https://", 0U) == 0U),
                    "Expansion plan PUT grant URL is invalid");
        }
        require(object_ids.insert(item.object_id).second,
                "Expansion plan page contains duplicate object IDs");
        page.records.push_back(std::move(item));
    }
    return page;
}

std::string serializeExpandedUploadReport(
        const std::string& worker_id, const std::string& lease_token,
        const std::string& request_id, const std::string& plan_id,
        const ExpandedUploadReport& report) {
    requireText(worker_id, "Inspection worker ID must not be blank");
    requireText(lease_token, "Inspection lease token must not be blank");
    requireText(request_id, "Inspection request ID must not be blank");
    requireText(plan_id, "Expansion plan ID must not be blank");
    requireText(report.object_id, "Expanded upload object ID must not be blank");
    requireText(report.observed_etag, "Expanded upload ETag must not be blank");
    requireSha256(report.observed_sha256);
    require(report.observed_size <= ProtocolLimits::kMaximumExpandedResourceBytes,
            "Expanded upload size is outside bounds");
    return Json({{"workerId", worker_id},
                 {"leaseToken", lease_token},
                 {"requestId", request_id},
                 {"planId", plan_id},
                 {"objectId", report.object_id},
                 {"observedSize", report.observed_size},
                 {"observedSha256", report.observed_sha256},
                 {"observedEtag", report.observed_etag}})
            .dump();
}

void validateInspectionClaimTask(const InspectionClaimTask& task,
                                 const InspectionClaimRequest& capabilities,
                                 const std::string& now_utc) {
    requireText(task.inspection_id, "Inspection claim has no inspection ID");
    requireText(task.request_id, "Inspection claim has no request ID");
    requireText(task.lease_token, "Inspection claim has no lease token");
    validateFixedUtc(now_utc);
    validateFixedUtc(task.lease_expire_time);
    validateFixedUtc(task.hard_deadline_time);
    require(task.inspection_id == task.inspection_request.inspection_id
                    && task.request_id == task.inspection_request.request_id,
            "Inspection scheduling and protocol identities differ");
    require(task.hard_deadline_time == task.inspection_request.deadline_time
                    && utcEpochSeconds(now_utc) < utcEpochSeconds(task.lease_expire_time)
                    && utcEpochSeconds(task.lease_expire_time)
                            <= utcEpochSeconds(task.hard_deadline_time),
            "Inspection lease window is invalid");
    require(task.inspection_request.protocol_version == capabilities.protocol_version
                    && task.inspection_request.resource_profile_version
                            == capabilities.resource_profile_version
                    && task.inspection_request.resource_profile_sha256
                            == capabilities.resource_profile_sha256
                    && std::find(capabilities.supported_inspector_versions.begin(),
                                 capabilities.supported_inspector_versions.end(),
                                 task.inspection_request.inspector_version)
                            != capabilities.supported_inspector_versions.end(),
            "Inspection claim is incompatible with worker capabilities");
    for (const auto& required_tool : task.inspection_request.required_tools) {
        require(std::any_of(capabilities.tool_versions.begin(),
                            capabilities.tool_versions.end(),
                            [&required_tool](const ToolVersion& local_tool) {
                                return sameTool(required_tool, local_tool);
                            }),
                "Inspection claim requires an unavailable tool version");
    }
    validateRequest(task.inspection_request, now_utc);
}

}  // namespace clip_worker::inspection
