#include "clip_worker/normalization/normalization_runtime.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

constexpr const char* kHash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kValidatorHash =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

class FakeTransport final : public client::INormalizationHttpTransport {
public:
    mutable std::deque<client::NormalizationHttpResponse> responses;
    mutable std::vector<client::NormalizationHttpRequest> requests;

    [[nodiscard]] client::NormalizationHttpResponse execute(
            const client::NormalizationHttpRequest& request) const override {
        requests.push_back(request);
        if (responses.empty()) throw std::logic_error("No fake response queued");
        auto response = responses.front();
        responses.pop_front();
        return response;
    }
};

ResourceRecord resource(std::string object_id) {
    ResourceRecord record;
    record.object_id = std::move(object_id);
    record.package_relative_path = record.object_id + ".glb";
    record.resource_role = "CONTENT";
    record.detected_kind = "GLB";
    record.size = 12U;
    record.sha256 = kHash;
    record.access_grant = {kHash, "GET", "https://broker.invalid/resource",
                           "2026-08-22T01:05:00Z"};
    return record;
}

ResourceManifestPage manifestPage(const ResourceRecord& record) {
    ResourceManifestPage page;
    page.manifest_id = kHash;
    page.page_number = 1U;
    page.record_count = 1U;
    page.records = {record};
    page.page_sha256 = canonicalPageSha256(page.records);
    return page;
}

ClaimTask task(const std::string& attempt_id = "attempt-1") {
    const auto page = manifestPage(resource("content-1"));
    ClaimTask value;
    value.task_id = "task-1";
    value.attempt_id = attempt_id;
    value.request_id = "request-" + attempt_id;
    value.lease_token = "lease-" + attempt_id;
    value.lease_expire_time = "2026-08-22T01:01:00Z";
    value.hard_deadline_time = "2026-08-22T01:10:00Z";
    value.tile_content_id = "content-1";
    value.source_closure_hash = kHash;
    value.resource_closure_version = "CONTENT_RESOURCE_CLOSURE_V1";
    value.normalization_version = kNormalizationVersion;
    value.canonical_family = CanonicalFamily::mesh_gltf2;
    value.canonical_contract_version = kCanonicalContractVersion;
    value.resource_profile_version = "profile-v1";
    value.resource_profile_sha256 = kHash;
    value.resource_manifest = {kManifestVersion, kHash, 1U, 1000U, 1U,
                               canonicalManifestSha256({page})};
    return value;
}

Json recordJson(const ResourceRecord& record) {
    return {{"objectId", record.object_id},
            {"packageRelativePath", record.package_relative_path},
            {"resourceRole", record.resource_role},
            {"detectedKind", record.detected_kind},
            {"detectedVersion", nullptr},
            {"mediaType", nullptr},
            {"size", record.size},
            {"sha256", record.sha256},
            {"requiredExtensions", record.required_extensions},
            {"usedExtensions", record.used_extensions},
            {"dependencyIds", record.dependency_ids},
            {"accessGrant", {{"grantId", record.access_grant.grant_id},
                              {"httpMethod", "GET"},
                              {"url", record.access_grant.url},
                              {"expiresAt", record.access_grant.expires_at}}}};
}

std::string pageJson(const ResourceManifestPage& page) {
    Json records = Json::array();
    for (const auto& record : page.records) records.push_back(recordJson(record));
    return Json{{"messageType", "RESOURCE_MANIFEST_PAGE"},
                {"manifestId", page.manifest_id},
                {"pageNumber", page.page_number},
                {"recordCount", page.record_count},
                {"pageSha256", page.page_sha256},
                {"records", std::move(records)}}
            .dump();
}

std::string claimJson(const ClaimTask& value) {
    return Json{{"messageType", "CLAIM_RESPONSE"},
                {"taskId", value.task_id},
                {"attemptId", value.attempt_id},
                {"requestId", value.request_id},
                {"leaseToken", value.lease_token},
                {"leaseExpireTime", value.lease_expire_time},
                {"hardDeadlineTime", value.hard_deadline_time},
                {"tileContentId", value.tile_content_id},
                {"sourceClosureHash", value.source_closure_hash},
                {"resourceClosureVersion", value.resource_closure_version},
                {"normalizationVersion", value.normalization_version},
                {"canonicalFamily", "MESH_GLTF2"},
                {"canonicalContractVersion", value.canonical_contract_version},
                {"requiredDecoders", value.required_decoders},
                {"resourceProfileVersion", value.resource_profile_version},
                {"resourceProfileSha256", value.resource_profile_sha256},
                {"resourceManifest",
                 {{"manifestVersion", value.resource_manifest.manifest_version},
                  {"manifestId", value.resource_manifest.manifest_id},
                  {"pageCount", value.resource_manifest.page_count},
                  {"pageSize", value.resource_manifest.page_size},
                  {"recordCount", value.resource_manifest.record_count},
                  {"manifestSha256", value.resource_manifest.manifest_sha256}}}}
            .dump();
}

ClaimRequest capabilities() {
    ClaimRequest request;
    request.worker_id = "worker-1";
    request.supported_normalization_versions = {kNormalizationVersion};
    request.family_capabilities = {{CanonicalFamily::mesh_gltf2,
                                    kCanonicalContractVersion,
                                    "validator", "1.0.0", kValidatorHash,
                                    1024U, 1024U}};
    request.resource_profile_version = "profile-v1";
    request.resource_profile_sha256 = kHash;
    request.tool_versions = {{"validator", "1.0.0", kValidatorHash}};
    return request;
}

client::NormalizationApiClient clientFor(
        const std::shared_ptr<FakeTransport>& transport) {
    client::NormalizationApiClientConfig config;
    config.base_url = "https://metadata.invalid";
    return client::NormalizationApiClient(config, transport);
}

TEST(NormalizationRuntimeTest, StaleLeaseConflictStopsFurtherWork) {
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back({409, "sensitive response is ignored"});
    auto api = clientFor(transport);
    NormalizationLeaseSession session(
            api, "worker-1", task(), "2026-08-22T01:00:00Z");

    EXPECT_THROW(static_cast<void>(session.heartbeat(
                         TaskPhase::manifest_fetch, 0U)),
                 client::NormalizationApiError);
    EXPECT_EQ(SessionState::lease_lost, session.state());
    EXPECT_THROW(static_cast<void>(session.resourceManifestPage(1U)),
                 std::logic_error);
}

TEST(NormalizationRuntimeTest, DuplicatePageReplayIsIdempotentButDriftLosesLease) {
    auto transport = std::make_shared<FakeTransport>();
    const auto first = manifestPage(resource("content-1"));
    auto changed = manifestPage(resource("content-2"));
    transport->responses.push_back({200, pageJson(first)});
    transport->responses.push_back({200, pageJson(first)});
    transport->responses.push_back({200, pageJson(changed)});
    auto api = clientFor(transport);
    NormalizationLeaseSession session(
            api, "worker-1", task(), "2026-08-22T01:00:00Z");

    static_cast<void>(session.resourceManifestPage(1U));
    static_cast<void>(session.resourceManifestPage(1U));
    EXPECT_EQ(1U, session.manifestPages().size());
    EXPECT_THROW(static_cast<void>(session.resourceManifestPage(1U)),
                 std::invalid_argument);
    EXPECT_EQ(SessionState::lease_lost, session.state());
}

TEST(NormalizationRuntimeTest, PartialUploadCannotComplete) {
    auto transport = std::make_shared<FakeTransport>();
    const auto page = manifestPage(resource("content-1"));
    transport->responses.push_back({200, pageJson(page)});
    auto api = clientFor(transport);
    NormalizationLeaseSession session(
            api, "worker-1", task(), "2026-08-22T01:00:00Z");
    static_cast<void>(session.resourceManifestPage(1U));

    EXPECT_THROW(session.complete(), std::logic_error);
    EXPECT_EQ(SessionState::active, session.state());
}

TEST(NormalizationRuntimeTest, CrashRetryUsesANewImmutableAttempt) {
    auto first_transport = std::make_shared<FakeTransport>();
    first_transport->responses.push_back({200, claimJson(task("attempt-1"))});
    NormalizationTaskRuntime first(capabilities(), clientFor(first_transport));
    EXPECT_THROW(static_cast<void>(first.runOnce("2026-08-22T01:00:00Z",
                               [](NormalizationLeaseSession& session) {
                                   EXPECT_EQ("attempt-1", session.task().attempt_id);
                                   throw std::runtime_error("synthetic crash");
                               })),
                 std::runtime_error);

    auto retry_transport = std::make_shared<FakeTransport>();
    retry_transport->responses.push_back({200, claimJson(task("attempt-2"))});
    NormalizationTaskRuntime retry(capabilities(), clientFor(retry_transport));
    EXPECT_TRUE(retry.runOnce("2026-08-22T01:00:00Z",
                              [](NormalizationLeaseSession& session) {
                                  EXPECT_EQ("attempt-2", session.task().attempt_id);
                              }));
}

}  // namespace
}  // namespace clip_worker::normalization
