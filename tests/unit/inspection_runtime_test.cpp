#include "clip_worker/client/inspection_api_client.hpp"
#include "clip_worker/inspection/inspection_runtime.hpp"

#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace clip_worker::inspection {
namespace {

constexpr const char* kFixtureNow = "2026-08-17T08:00:00Z";
constexpr const char* kLeaseTime = "2026-08-17T08:00:45Z";
constexpr const char* kSecretLease = "lease-secret-must-not-leak";

std::filesystem::path sourcePath(const std::string& relative) {
#ifdef CLIP_WORKER_SOURCE_DIR
    return std::filesystem::path(CLIP_WORKER_SOURCE_DIR) / relative;
#else
    return std::filesystem::current_path() / relative;
#endif
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open Inspector runtime fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

nlohmann::json positiveFixture() {
    return nlohmann::json::parse(readText(sourcePath(
            "tests/fixtures/protocol/inspector-v1/positive-contract.json")));
}

InspectionClaimRequest capabilities(const InspectionRequest& request) {
    InspectionClaimRequest result;
    result.worker_id = "inspector-worker-1";
    result.supported_inspector_versions = {request.inspector_version};
    result.resource_profile_version = request.resource_profile_version;
    result.resource_profile_sha256 = request.resource_profile_sha256;
    result.tool_versions = request.required_tools;
    return result;
}

nlohmann::json claimJson(const nlohmann::json& fixture) {
    const auto& request = fixture.at("inspectionRequest");
    return {{"inspectionId", request.at("inspectionId")},
            {"requestId", request.at("requestId")},
            {"leaseToken", kSecretLease},
            {"leaseExpireTime", kLeaseTime},
            {"hardDeadlineTime", request.at("deadlineTime")},
            {"inspectionRequest", request}};
}

class FakeTransport final : public client::IInspectionHttpTransport {
public:
    mutable std::vector<client::InspectionHttpRequest> requests;
    mutable std::deque<client::InspectionHttpResponse> responses;

    client::InspectionHttpResponse execute(
            const client::InspectionHttpRequest& request) const override {
        requests.push_back(request);
        if (responses.empty()) throw std::logic_error("Fake response queue is empty");
        auto response = responses.front();
        responses.pop_front();
        return response;
    }
};

client::InspectionApiClient apiClient(const std::shared_ptr<FakeTransport>& transport) {
    client::InspectionApiClientConfig config;
    config.base_url = "https://control.invalid";
    config.authorization_header = "Authorization: internal-secret";
    return client::InspectionApiClient(std::move(config), transport);
}

InspectionClaimTask validTask(const nlohmann::json& fixture) {
    return parseInspectionClaimTask(claimJson(fixture).dump());
}

TEST(InspectionRuntimeTest, ClaimsOnlyAnExactlyCompatibleSchedulingEnvelope) {
    const auto fixture = positiveFixture();
    const auto request = parseInspectionRequest(fixture.at("inspectionRequest").dump());
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back({200, claimJson(fixture).dump()});
    InspectionTaskRuntime runtime(capabilities(request), apiClient(transport));
    bool invoked = false;

    EXPECT_TRUE(runtime.runOnce(kFixtureNow, [&](InspectionLeaseSession& session) {
        invoked = true;
        EXPECT_EQ(session.task().request_id, request.request_id);
        EXPECT_TRUE(session.active());
    }));
    EXPECT_TRUE(invoked);
    ASSERT_EQ(transport->requests.size(), 1U);
    EXPECT_NE(transport->requests.front().url.find("/inspection-tasks/claim"),
              std::string::npos);

    auto incompatible = capabilities(request);
    incompatible.resource_profile_sha256 = std::string(64U, 'f');
    auto mismatch_transport = std::make_shared<FakeTransport>();
    mismatch_transport->responses.push_back({200, claimJson(fixture).dump()});
    InspectionTaskRuntime mismatch_runtime(
            std::move(incompatible), apiClient(mismatch_transport));
    EXPECT_THROW(static_cast<void>(mismatch_runtime.runOnce(
                         kFixtureNow, [](InspectionLeaseSession&) {})),
                 std::invalid_argument);
}

TEST(InspectionRuntimeTest, HeartbeatCancellationStopsAllFurtherIo) {
    const auto fixture = positiveFixture();
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back(
            {200, nlohmann::json({{"leaseExpireTime", kLeaseTime},
                                  {"hardDeadlineTime",
                                   fixture["inspectionRequest"]["deadlineTime"]},
                                  {"cancelRequested", true}})
                          .dump()});
    auto api = apiClient(transport);
    InspectionLeaseSession session(api, "inspector-worker-1", validTask(fixture));

    const auto heartbeat = session.heartbeat(
            TaskPhase::manifest_fetch, InspectionProgress{});

    EXPECT_TRUE(heartbeat.cancel_requested);
    EXPECT_EQ(session.state(), InspectionSessionState::cancelled);
    EXPECT_THROW(static_cast<void>(session.sourceManifestPage(0U, kFixtureNow)),
                 std::logic_error);
    EXPECT_EQ(transport->requests.size(), 1U);
}

TEST(InspectionRuntimeTest, LeaseConflictIsTerminalAndDoesNotExposeResponseBody) {
    const auto fixture = positiveFixture();
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back(
            {409, std::string("https://storage.invalid/object?token=") + kSecretLease});
    auto api = apiClient(transport);
    InspectionLeaseSession session(api, "inspector-worker-1", validTask(fixture));

    try {
        static_cast<void>(session.heartbeat(
                TaskPhase::manifest_fetch, InspectionProgress{}));
        FAIL() << "heartbeat should reject a stale lease";
    } catch (const client::InspectionApiError& error) {
        EXPECT_EQ(error.statusCode(), 409);
        EXPECT_EQ(std::string(error.what()).find(kSecretLease), std::string::npos);
        EXPECT_EQ(std::string(error.what()).find("storage.invalid"), std::string::npos);
    }
    EXPECT_EQ(session.state(), InspectionSessionState::lease_lost);
    EXPECT_THROW(static_cast<void>(session.heartbeat(
                         TaskPhase::manifest_fetch, InspectionProgress{})),
                 std::logic_error);
}

TEST(InspectionRuntimeTest, ReplaysResultPageButPublishesTerminalEnvelopeOnce) {
    const auto fixture = positiveFixture();
    const auto source_page = parseSourceManifestPage(
            fixture.at("sourceManifestPage").dump());
    const auto result_page = parseResultPage(fixture.at("resultPage").dump());
    const auto hierarchy_page = parseHierarchyPage(
            fixture.at("hierarchyPage").dump());
    const auto result = parseInspectionResult(fixture.at("inspectionResult").dump());
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back({200, fixture.at("sourceManifestPage").dump()});
    transport->responses.push_back({500, "temporary failure with token=secret"});
    transport->responses.push_back({204, ""});
    transport->responses.push_back({204, ""});
    transport->responses.push_back({204, ""});
    transport->responses.push_back({204, ""});
    auto api = apiClient(transport);
    InspectionLeaseSession session(api, "inspector-worker-1", validTask(fixture));

    EXPECT_EQ(session.sourceManifestPage(0U, kFixtureNow).page_sha256,
              source_page.page_sha256);
    EXPECT_THROW(session.submitResultPage(result_page), client::InspectionApiError);
    EXPECT_TRUE(session.active());
    EXPECT_NO_THROW(session.submitResultPage(result_page));
    EXPECT_NO_THROW(session.submitResultPage(result_page));
    EXPECT_NO_THROW(session.submitHierarchyPage(hierarchy_page));
    EXPECT_NO_THROW(session.complete(result, kFixtureNow));
    EXPECT_EQ(session.state(), InspectionSessionState::completed);
    EXPECT_THROW(session.complete(result, kFixtureNow), std::logic_error);
    EXPECT_EQ(transport->requests.size(), 6U);
}

TEST(InspectionRuntimeTest, SchedulingParserRejectsUnknownOuterFields) {
    auto json = claimJson(positiveFixture());
    json["sourceBucket"] = "must-not-be-accepted";

    EXPECT_THROW(static_cast<void>(parseInspectionClaimTask(json.dump())),
                 std::invalid_argument);
}

}  // namespace
}  // namespace clip_worker::inspection
