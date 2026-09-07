#include "clip_worker/normalization/normalization_v2_runtime.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization::v2 {
namespace {

using Json = nlohmann::json;

constexpr const char* kHash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kPointValidatorHash =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* kCompositeValidatorHash =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

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

ResourceRecord resource() {
    ResourceRecord record;
    record.object_id = "composite-root";
    record.package_relative_path = "composite.cmpt";
    record.resource_role = "CONTENT";
    record.detected_kind = "CMPT";
    record.detected_version = "1";
    record.size = 32U;
    record.sha256 = kHash;
    record.access_grant = {kHash, "GET", "https://broker.invalid/resource",
                           "2026-08-23T01:05:00Z"};
    return record;
}

ResourceManifestPage manifestPage() {
    ResourceManifestPage page;
    page.manifest_id = kHash;
    page.page_number = 1U;
    page.record_count = 1U;
    page.records = {resource()};
    page.page_sha256 = canonicalPageSha256(page.records);
    return page;
}

ClaimTask task() {
    const auto page = manifestPage();
    ClaimTask value;
    value.task_id = "task-v2";
    value.attempt_id = "attempt-v2";
    value.request_id = "request-v2";
    value.lease_token = "lease-v2";
    value.lease_expire_time = "2026-08-23T01:01:00Z";
    value.hard_deadline_time = "2026-08-23T01:10:00Z";
    value.tile_content_id = "content-v2";
    value.root_object_id = "composite-root";
    value.source_closure_hash = kHash;
    value.resource_closure_version = "CONTENT_RESOURCE_CLOSURE_V1";
    value.normalization_version = kCompositeNormalizationVersion;
    value.canonical_family = CanonicalFamily::composite_children;
    value.canonical_contract_version = kCompositeCanonicalContractVersion;
    value.resource_profile_version = kResourceProfileVersion;
    value.resource_profile_sha256 = kHash;
    value.maximum_outputs = 2U;
    value.resource_manifest = {kManifestVersion, kHash, 1U, 1000U, 1U,
                               canonicalManifestSha256({page})};
    return value;
}

ClaimRequest capabilities() {
    ClaimRequest request;
    request.worker_id = "worker-v2";
    request.resource_profile_sha256 = kHash;
    request.family_capabilities = {
            {CanonicalFamily::point_gltf2, kPointNormalizationVersion,
             kPointCanonicalContractVersion, "point-validator", "1.0.0",
             kPointValidatorHash, 4096U, 4096U},
            {CanonicalFamily::composite_children,
             kCompositeNormalizationVersion,
             kCompositeCanonicalContractVersion, "composite-validator",
             "1.0.0", kCompositeValidatorHash, 4096U, 4096U}};
    request.tool_versions = {
            {"point-validator", "1.0.0", kPointValidatorHash},
            {"composite-validator", "1.0.0", kCompositeValidatorHash}};
    return request;
}

Json resourceJson(const ResourceRecord& record) {
    return {{"objectId", record.object_id},
            {"packageRelativePath", record.package_relative_path},
            {"resourceRole", record.resource_role},
            {"detectedKind", record.detected_kind},
            {"detectedVersion", record.detected_version},
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
    for (const auto& record : page.records) records.push_back(resourceJson(record));
    return Json{{"messageType", "RESOURCE_MANIFEST_PAGE"},
                {"manifestId", page.manifest_id},
                {"pageNumber", page.page_number},
                {"recordCount", page.record_count},
                {"pageSha256", page.page_sha256},
                {"records", std::move(records)}}
            .dump();
}

OutputDeclaration pointOutput(const ClaimTask& value) {
    CanonicalArtifactEvidence evidence;
    evidence.output_size = 16U;
    evidence.output_sha256 = sha256Hex("point-output");
    evidence.semantic_hash = sha256Hex("point-semantic");
    evidence.validation_summary.scene_count = 1U;
    evidence.validation_summary.node_count = 1U;
    evidence.validation_summary.primitive_count = 1U;
    evidence.validation_summary.accessor_count = 1U;
    evidence.validation_summary.buffer_count = 1U;
    evidence.validation_summary.feature_count = 2U;
    evidence.validation_summary.metadata_property_count = 1U;
    evidence.validation_summary.feature_identity_model =
            FeatureIdentityModel::point_feature_id;
    evidence.validation_summary.validator_name = "point-validator";
    evidence.validation_summary.validator_version = "1.0.0";
    evidence.validation_summary.validator_build_sha256 = kPointValidatorHash;

    OutputDeclaration output;
    output.ordinal_path = {0U};
    output.canonical_family = CanonicalFamily::point_gltf2;
    output.canonical_contract_version = kPointCanonicalContractVersion;
    output.output_size = evidence.output_size;
    output.output_sha256 = evidence.output_sha256;
    output.semantic_hash = evidence.semantic_hash;
    output.validation_summary = contentValidationSummary(
            evidence, output.canonical_family, 2U);
    output.validation_manifest_sha256 = validationManifestSha256(
            output.validation_summary);
    output.output_id = outputId(value, output);
    return output;
}

OutputDeclaration parentOutput(const ClaimTask& value) {
    OutputDeclaration output;
    output.output_kind = OutputKind::parent_manifest;
    output.canonical_family = CanonicalFamily::composite_children;
    output.canonical_contract_version = kCompositeCanonicalContractVersion;
    output.output_size = 32U;
    output.output_sha256 = sha256Hex("parent-output");
    output.semantic_hash = sha256Hex("parent-semantic");
    output.validation_summary = parentValidationSummary(
            1U, {"composite-validator", "1.0.0", kCompositeValidatorHash});
    output.validation_manifest_sha256 = validationManifestSha256(
            output.validation_summary);
    output.output_id = outputId(value, output);
    return output;
}

std::string grantJson(const OutputDeclaration& output) {
    return Json{{"messageType", "OUTPUT_PREPARE_RESPONSE"},
                {"outputId", output.output_id},
                {"uploadGrantId", sha256Hex("grant-" + output.output_id)},
                {"httpMethod", "PUT"},
                {"uploadUrl", "https://broker.invalid/upload"},
                {"expiresAt", "2026-08-23T01:05:00Z"}}
            .dump();
}

client::NormalizationV2ApiClient clientFor(
        const std::shared_ptr<FakeTransport>& transport) {
    client::NormalizationApiClientConfig config;
    config.base_url = "https://metadata.invalid";
    return client::NormalizationV2ApiClient(config, transport);
}

TEST(NormalizationV2RuntimeTest, CompletesExactOrderedMultiOutputClosure) {
    const auto claimed_task = task();
    const auto point = pointOutput(claimed_task);
    const auto parent = parentOutput(claimed_task);
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back({200, pageJson(manifestPage())});
    transport->responses.push_back({200, grantJson(point)});
    transport->responses.push_back({204, {}});
    transport->responses.push_back({200, grantJson(parent)});
    transport->responses.push_back({204, {}});
    transport->responses.push_back({204, {}});
    auto api = clientFor(transport);
    NormalizationV2LeaseSession session(
            api, capabilities(), claimed_task, "2026-08-23T01:00:00Z");

    static_cast<void>(session.resourceManifestPage(1U));
    static_cast<void>(session.prepareOutput(point));
    session.reportOutput({point.output_id, "etag-point", point.output_size,
                          point.output_sha256});
    static_cast<void>(session.prepareOutput(parent));
    session.reportOutput({parent.output_id, "etag-parent", parent.output_size,
                          parent.output_sha256});
    session.complete({point, parent}, true);

    ASSERT_EQ(SessionState::completed, session.state());
    ASSERT_EQ(6U, transport->requests.size());
    const Json completion = Json::parse(transport->requests.back().body);
    EXPECT_EQ(completion.at("orderedOutputIds"),
              Json::array({point.output_id, parent.output_id}));
    EXPECT_EQ(completion.at("outputManifestSha256"),
              outputManifestSha256({point, parent}));
    EXPECT_TRUE(completion.at("globalPreviewOnly").get<bool>());
}

TEST(NormalizationV2RuntimeTest, RejectsCompletionBeforeEveryOutputIsReported) {
    const auto claimed_task = task();
    const auto point = pointOutput(claimed_task);
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back({200, pageJson(manifestPage())});
    transport->responses.push_back({200, grantJson(point)});
    auto api = clientFor(transport);
    NormalizationV2LeaseSession session(
            api, capabilities(), claimed_task, "2026-08-23T01:00:00Z");

    static_cast<void>(session.resourceManifestPage(1U));
    static_cast<void>(session.prepareOutput(point));
    EXPECT_THROW(session.complete({point}, false), std::logic_error);
    EXPECT_EQ(SessionState::active, session.state());
}

TEST(NormalizationV2RuntimeTest, ClaimRequiresEveryFamilyValidatorTool) {
    auto request = capabilities();
    request.tool_versions.pop_back();
    EXPECT_THROW(serializeClaimRequest(request), std::invalid_argument);
}

}  // namespace
}  // namespace clip_worker::normalization::v2
