#include "clip_worker/normalization/normalization_v2_contract.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace clip_worker::normalization::v2 {
namespace {

using Json = nlohmann::json;

Json fixture(const char* name) {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "tests/fixtures/protocol/normalizer-v2" / name;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("V2 protocol fixture is unreadable");
    return Json::parse(input);
}

ClaimRequest capabilities() {
    ClaimRequest result;
    result.worker_id = "normalizer-v2-worker";
    result.decoder_capabilities = {
            "DRACO", "JPEG", "KTX2", "MESHOPT", "PNG", "WEBP"};
    result.resource_profile_sha256 =
            "d6097a29380375180e2cf9374eae354a9672e0589f3fb04b2f85930f16d5af78";
    result.family_capabilities = {{
            CanonicalFamily::point_gltf2,
            kPointNormalizationVersion,
            kPointCanonicalContractVersion,
            "point-validator",
            "validator-v1",
            std::string(64U, 'c'),
            ProtocolLimits::kMaximumArtifactBytes,
            ProtocolLimits::kMaximumArtifactBytes}};
    result.tool_versions = {{"point-validator", "validator-v1",
                             std::string(64U, 'c')}};
    return result;
}

TEST(NormalizationV2ContractTest, MatchesSharedClaimFixtureAndExplicitRoot) {
    const auto positive = fixture("positive-contract.json");
    const auto request = capabilities();
    EXPECT_EQ(Json::parse(serializeClaimRequest(request)),
              positive.at("claimRequest"));

    const auto task = parseClaimTask(positive.at("claimResponse").dump());
    EXPECT_EQ(task.root_object_id, "root-object-v2");
    EXPECT_EQ(task.resource_manifest.manifest_version, kManifestVersion);
    EXPECT_EQ(task.maximum_outputs, 1U);
    EXPECT_NO_THROW(validateClaimTask(
            task, request, "2026-08-23T00:00:00Z"));
}

TEST(NormalizationV2ContractTest, ClosesPointOutputIdentityAndTypedSummary) {
    const auto task = parseClaimTask(
            fixture("positive-contract.json").at("claimResponse").dump());
    CanonicalArtifactEvidence evidence;
    evidence.output_size = 16U;
    evidence.output_sha256 = sha256Hex("point-output");
    evidence.semantic_hash = sha256Hex("point-semantic");
    evidence.validation_summary.scene_count = 1U;
    evidence.validation_summary.node_count = 1U;
    evidence.validation_summary.primitive_count = 1U;
    evidence.validation_summary.accessor_count = 1U;
    evidence.validation_summary.buffer_count = 1U;
    evidence.validation_summary.image_count = 0U;
    evidence.validation_summary.feature_count = 2U;
    evidence.validation_summary.metadata_property_count = 1U;
    evidence.validation_summary.feature_identity_model =
            FeatureIdentityModel::point_feature_id;
    evidence.validation_summary.validator_name = "point-validator";
    evidence.validation_summary.validator_version = "validator-v1";
    evidence.validation_summary.validator_build_sha256 =
            std::string(64U, 'c');

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
    output.output_id = outputId(task, output);

    EXPECT_NO_THROW(validateOutputDeclaration(
            output, task, capabilities()));
    const Json request = Json::parse(serializeOutputPrepareRequest(
            "normalizer-v2-worker", task, output));
    EXPECT_EQ(request.at("ordinalPath"), Json::array({0U}));
    EXPECT_EQ(request.at("validationSummary").at("pointCount"), 2U);
    EXPECT_EQ(outputManifestSha256({output}).size(), 64U);
}

TEST(NormalizationV2ContractTest, ParentSummaryUsesNullCommonFields) {
    ClaimTask task;
    task.attempt_id = "attempt-v2";
    task.lease_token = "lease-v2";
    task.request_id = "request-v2";
    task.canonical_family = CanonicalFamily::composite_children;
    task.canonical_contract_version = kCompositeCanonicalContractVersion;
    OutputDeclaration output;
    output.output_kind = OutputKind::parent_manifest;
    output.canonical_family = CanonicalFamily::composite_children;
    output.canonical_contract_version = kCompositeCanonicalContractVersion;
    output.output_size = 2U;
    output.output_sha256 = sha256Hex("{}");
    output.semantic_hash = sha256Hex("parent");
    output.validation_summary = parentValidationSummary(
            3U, {"composite-validator", "validator-v1",
                 std::string(64U, 'd')});
    output.validation_manifest_sha256 = validationManifestSha256(
            output.validation_summary);
    output.output_id = outputId(task, output);
    const Json summary = Json::parse(serializeOutputPrepareRequest(
            "normalizer-v2-worker", task, output))
                                 .at("validationSummary");
    EXPECT_TRUE(summary.at("coordinateBasis").is_null());
    EXPECT_EQ(summary.at("compositeChildCount"), 3U);
    EXPECT_FALSE(summary.contains("pointCount"));
}

TEST(NormalizationV2ContractTest, RejectsUnknownCredentialBearingField) {
    const auto negative = fixture("negative-contract.json");
    EXPECT_THROW(parseClaimTask(
                         negative.at("credentialBearingClaimResponse").dump()),
                 std::invalid_argument);
}

TEST(NormalizationV2ContractTest,
     V3AllowsLegacyAndMetadataTuplesForTheSameFamily) {
    ClaimRequest request = capabilities();
    v3::configureClaim(request);
    request.resource_profile_sha256 = std::string(64U, 'd');
    request.family_capabilities.push_back({
            CanonicalFamily::point_gltf2,
            v3::kPointNormalizationVersion,
            v3::kPointCanonicalContractVersion,
            "point-validator",
            "validator-v1",
            std::string(64U, 'c'),
            ProtocolLimits::kMaximumArtifactBytes,
            ProtocolLimits::kMaximumArtifactBytes});

    const Json json = Json::parse(serializeClaimRequest(request));
    EXPECT_EQ(json.at("protocolVersion"), v3::kProtocolVersion);
    EXPECT_EQ(json.at("schemaSha256"), v3::kSchemaSha256);
    EXPECT_EQ(json.at("resourceProfileVersion"),
              v3::kResourceProfileVersion);
    EXPECT_EQ(json.at("familyCapabilities").size(), 2U);
}

TEST(NormalizationV2ContractTest,
     V3MatchesClaimAndOutputByCompleteMetadataTuple) {
    ClaimRequest request = capabilities();
    v3::configureClaim(request);
    request.resource_profile_sha256 = std::string(64U, 'd');
    request.family_capabilities.push_back({
            CanonicalFamily::point_gltf2,
            v3::kPointNormalizationVersion,
            v3::kPointCanonicalContractVersion,
            "point-validator",
            "validator-v1",
            std::string(64U, 'c'),
            ProtocolLimits::kMaximumArtifactBytes,
            ProtocolLimits::kMaximumArtifactBytes});

    ClaimTask task;
    task.task_id = "metadata-task";
    task.attempt_id = "metadata-attempt";
    task.request_id = "metadata-request";
    task.lease_token = "metadata-lease";
    task.lease_expire_time = "2026-08-23T00:01:00Z";
    task.hard_deadline_time = "2026-08-23T00:10:00Z";
    task.tile_content_id = "metadata-content";
    task.root_object_id = "metadata-root";
    task.source_closure_hash = std::string(64U, 'a');
    task.resource_closure_version = "CONTENT_RESOURCE_CLOSURE_V1";
    task.normalization_version = v3::kPointNormalizationVersion;
    task.canonical_family = CanonicalFamily::point_gltf2;
    task.canonical_contract_version = v3::kPointCanonicalContractVersion;
    task.resource_profile_version = v3::kResourceProfileVersion;
    task.resource_profile_sha256 = request.resource_profile_sha256;
    task.maximum_outputs = 1U;
    EXPECT_NO_THROW(validateClaimTask(
            task, request, "2026-08-23T00:00:00Z"));

    CanonicalArtifactEvidence evidence;
    evidence.output_size = 16U;
    evidence.output_sha256 = sha256Hex("metadata-point-output");
    evidence.semantic_hash = sha256Hex("metadata-point-semantic");
    evidence.validation_summary.scene_count = 1U;
    evidence.validation_summary.node_count = 1U;
    evidence.validation_summary.primitive_count = 1U;
    evidence.validation_summary.accessor_count = 1U;
    evidence.validation_summary.buffer_count = 1U;
    evidence.validation_summary.image_count = 0U;
    evidence.validation_summary.feature_count = 1U;
    evidence.validation_summary.metadata_property_count = 1U;
    evidence.validation_summary.feature_identity_model =
            FeatureIdentityModel::point_feature_id;
    evidence.validation_summary.validator_name = "point-validator";
    evidence.validation_summary.validator_version = "validator-v1";
    evidence.validation_summary.validator_build_sha256 =
            std::string(64U, 'c');
    OutputDeclaration output;
    output.ordinal_path = {0U};
    output.canonical_family = CanonicalFamily::point_gltf2;
    output.canonical_contract_version =
            v3::kPointCanonicalContractVersion;
    output.output_size = evidence.output_size;
    output.output_sha256 = evidence.output_sha256;
    output.semantic_hash = evidence.semantic_hash;
    output.validation_summary = contentValidationSummary(
            evidence, output.canonical_family, 1U);
    output.validation_manifest_sha256 = validationManifestSha256(
            output.validation_summary);
    output.output_id = outputId(task, output);
    EXPECT_NO_THROW(validateOutputDeclaration(output, task, request));
}

}  // namespace
}  // namespace clip_worker::normalization::v2
