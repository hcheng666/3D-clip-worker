#include "clip_worker/authorization/clipper_v2_runtime.hpp"

#include "clip_worker/normalization/canonical_artifact.hpp"

#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace clip_worker::authorization::v2 {
namespace {

using Json = nlohmann::json;
constexpr const char* kHash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

class FakeTransport final : public client::INormalizationHttpTransport {
public:
    mutable std::deque<client::NormalizationHttpResponse> responses;
    mutable std::vector<client::NormalizationHttpRequest> requests;

    [[nodiscard]] client::NormalizationHttpResponse execute(
            const client::NormalizationHttpRequest& request) const override {
        requests.push_back(request);
        if (responses.empty()) throw std::logic_error("No response queued");
        auto response = responses.front();
        responses.pop_front();
        return response;
    }
};

ClaimTask task() {
    ClaimTask value;
    value.work_item_id = "work-v2";
    value.attempt_id = "attempt-v2";
    value.request_id = "request-v2";
    value.lease_token = "lease-v2";
    value.canonical_family = CanonicalFamily::mesh_gltf2;
    value.canonical_contract_version =
            normalization::v3::kMeshCanonicalContractVersion;
    value.clip_strategy_version = kMeshClipStrategyVersion;
    value.input_size = 256U;
    value.input_sha256 = kHash;
    value.input_semantic_hash = kHash;
    value.input_validation_manifest_sha256 = kHash;
    value.scope_hash = kHash;
    value.transform_hash = kHash;
    value.limits.maximum_aggregate_output_bytes = 8192U;
    value.limits.maximum_output_bytes = 4096U;
    value.limits.maximum_outputs = 2U;
    return value;
}

ClaimRequest capabilities() {
    ClaimRequest value;
    value.worker_id = "worker-v2";
    return value;
}

std::vector<std::uint8_t> readFixture() {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "tests/fixtures/normalization/generated/mesh-positive.glb";
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

OutputDeclaration output(const ClaimTask& value) {
    auto evidence = normalization::validateCanonicalGlb(
            readFixture(), normalization::CanonicalFamily::mesh_gltf2,
            {"mesh-validator", "1.0.0", kHash});
    evidence.semantic_hash = normalization::metadataSemanticHash(evidence);
    OutputDeclaration result;
    result.output_id = clippedOutputId(value.work_item_id, 0U);
    result.canonical_family = CanonicalFamily::mesh_gltf2;
    result.canonical_contract_version = value.canonical_contract_version;
    result.evidence = std::move(evidence);
    return result;
}

AuthorizationClipProof proof(const ClaimTask& value) {
    AuthorizationClipProof result;
    result.input_sha256 = value.input_sha256;
    result.input_semantic_hash = value.input_semantic_hash;
    result.input_validation_manifest_sha256 =
            value.input_validation_manifest_sha256;
    result.scope_hash = value.scope_hash;
    result.transform_hash = value.transform_hash;
    result.canonical_family = canonicalFamilyName(value.canonical_family);
    result.canonical_contract_version = value.canonical_contract_version;
    result.clip_strategy_version = value.clip_strategy_version;
    result.classifier.relation =
            clip::AuthorizationContentRelation::boundary;
    result.classifier.classifier_name = "EXACT_TRIANGLE_UNION_COVERAGE";
    result.classifier.classifier_version =
            "exact-triangle-union-coverage-v1";
    result.classifier.input_element_count = 1U;
    result.classifier.boundary_element_count = 1U;
    return buildAuthorizationClipProof(std::move(result));
}

client::ClipperV2ApiClient api(
        const std::shared_ptr<FakeTransport>& transport) {
    client::NormalizationApiClientConfig config;
    config.base_url = "https://metadata.invalid";
    return client::ClipperV2ApiClient(config, transport);
}

TEST(ClipperV2RuntimeTest, CompletesOnlyAfterExactOutputReportClosure) {
    const auto claimed = task();
    const auto declared = output(claimed);
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back({204, {}});
    transport->responses.push_back({200, Json{
            {"messageType", "CLIPPER_V2_OUTPUT_PREPARE_RESPONSE"},
            {"outputId", declared.output_id},
            {"uploadGrantId", "grant-v2"}, {"httpMethod", "PUT"},
            {"uploadUrl", "https://broker.invalid/output"},
            {"expiresAt", "2026-08-24T01:01:00Z"}}.dump()});
    transport->responses.push_back({204, {}});
    transport->responses.push_back({204, {}});
    auto client = api(transport);
    ClipperV2LeaseSession session(
            client, capabilities(), claimed, "2026-08-24T01:00:00Z");

    session.heartbeat(TaskPhase::validating_output, claimed.input_size);
    static_cast<void>(session.prepareOutput(declared));
    EXPECT_THROW(session.complete(
                         CompletionOutcome::clipped, proof(claimed),
                         {declared}),
                 std::logic_error);
    session.reportOutput({declared.output_id, "etag-v2",
                          declared.evidence.output_size,
                          declared.evidence.output_sha256});
    session.complete(CompletionOutcome::clipped, proof(claimed), {declared});

    EXPECT_EQ(session.state(), SessionState::completed);
    const Json completion = Json::parse(transport->requests.back().body);
    EXPECT_EQ(completion.at("orderedOutputIds"),
              Json::array({declared.output_id}));
    EXPECT_EQ(completion.at("outputManifestSha256"),
              outputManifestSha256({declared}));
}

TEST(ClipperV2RuntimeTest, ConflictHeartbeatProvesLeaseLoss) {
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back({409, {}});
    auto client = api(transport);
    ClipperV2LeaseSession session(
            client, capabilities(), task(), "2026-08-24T01:00:00Z");

    EXPECT_THROW(session.heartbeat(TaskPhase::downloading, 0U),
                 client::NormalizationApiError);
    EXPECT_EQ(session.state(), SessionState::lease_lost);
    EXPECT_FALSE(session.active());
}

}  // namespace
}  // namespace clip_worker::authorization::v2
