#include "clip_worker/authorization/clipper_v2_contract.hpp"

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace clip_worker::authorization::v2 {
namespace {

using Json = nlohmann::json;

constexpr const char* kHashA =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kHashB =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* kHashC =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Clipper V2 test fixture is absent");
    return {std::istreambuf_iterator<char>(input), {}};
}

ClaimTask meshTask() {
    ClaimTask task;
    task.work_item_id = "work-item-v2";
    task.attempt_id = "attempt-v2";
    task.request_id = "request-v2";
    task.lease_token = "lease-v2";
    task.lease_expire_time = "2026-08-24T01:01:00Z";
    task.hard_deadline_time = "2026-08-24T01:10:00Z";
    task.tile_content_id = "content-v2";
    task.source_artifact_kind = "NORMALIZATION_ARTIFACT";
    task.source_artifact_id = "artifact-v2";
    task.canonical_family = CanonicalFamily::mesh_gltf2;
    task.canonical_contract_version =
            normalization::v3::kMeshCanonicalContractVersion;
    task.clip_strategy_version = kMeshClipStrategyVersion;
    task.input_size = 256U;
    task.input_sha256 = kHashA;
    task.input_semantic_hash = kHashB;
    task.input_validation_manifest_sha256 = kHashC;
    task.input_grant = {"grant-v2", "GET", "https://broker.invalid/input",
                        "2026-08-24T01:01:00Z"};
    task.scope_wkb_base64 = "AQ==";
    task.scope_srid = 4490;
    task.scope_hash = kHashA;
    task.accumulated_transform = {1.0, 0.0, 0.0, 0.0,
                                  0.0, 1.0, 0.0, 0.0,
                                  0.0, 0.0, 1.0, 0.0,
                                  0.0, 0.0, 0.0, 1.0};
    task.transform_hash = kHashB;
    task.canonical_coordinate_basis = kCoordinateBasis;
    task.resource_profile_version = kResourceProfileVersion;
    task.resource_profile_sha256 = kHashC;
    task.limits = {4096U, 4096U, 8192U, 100U, 100U,
                   100U, 1000U, 1000U, 2U};
    return task;
}

ClaimRequest capabilities() {
    ClaimRequest request;
    request.worker_id = "clipper-v2-worker";
    request.resource_profile_sha256 = kHashC;
    request.family_capabilities = {{
            kProtocolVersion, CanonicalFamily::mesh_gltf2,
            normalization::v3::kMeshCanonicalContractVersion,
            kMeshClipStrategyVersion, "mesh-validator", "1.0.0", kHashA,
            4096U, 4096U}};
    return request;
}

normalization::CanonicalArtifactEvidence meshEvidence() {
    const auto bytes = readFile(
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "tests/fixtures/normalization/generated/mesh-positive.glb");
    auto evidence = normalization::validateCanonicalGlb(
            bytes, normalization::CanonicalFamily::mesh_gltf2,
            {"mesh-validator", "1.0.0", kHashA});
    evidence.semantic_hash = normalization::metadataSemanticHash(evidence);
    return evidence;
}

AuthorizationClipProof boundaryProof(const ClaimTask& task) {
    AuthorizationClipProof proof;
    proof.input_sha256 = task.input_sha256;
    proof.input_semantic_hash = task.input_semantic_hash;
    proof.input_validation_manifest_sha256 =
            task.input_validation_manifest_sha256;
    proof.scope_hash = task.scope_hash;
    proof.transform_hash = task.transform_hash;
    proof.canonical_family = canonicalFamilyName(task.canonical_family);
    proof.canonical_contract_version = task.canonical_contract_version;
    proof.clip_strategy_version = task.clip_strategy_version;
    proof.source_ordinal_path = {1U, 0U};
    proof.classifier.relation =
            clip::AuthorizationContentRelation::boundary;
    proof.classifier.classifier_name = "EXACT_TRIANGLE_UNION_COVERAGE";
    proof.classifier.classifier_version =
            "exact-triangle-union-coverage-v1";
    proof.classifier.input_element_count = 2U;
    proof.classifier.whole_element_count = 1U;
    proof.classifier.boundary_element_count = 1U;
    return buildAuthorizationClipProof(std::move(proof));
}

TEST(ClipperV2ContractTest, SchemaDigestAndClaimIdentityArePinned) {
    const auto schema = readFile(
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/clipper-protocol-v2.schema.json");
    EXPECT_EQ(client::sha256Hex(schema), kSchemaSha256);
    const Json claim = Json::parse(serializeClaimRequest(capabilities()));
    EXPECT_EQ(claim.at("protocolVersion"), kProtocolVersion);
    EXPECT_EQ(claim.at("schemaSha256"), kSchemaSha256);
    EXPECT_EQ(claim.at("familyCapabilities").front()
                      .at("clipStrategyVersion"),
              kMeshClipStrategyVersion);
}

TEST(ClipperV2ContractTest, OutputIdentityManifestAndProofAreHashClosed) {
    const auto task = meshTask();
    OutputDeclaration output;
    output.output_id = clippedOutputId(task.work_item_id, 0U);
    output.canonical_family = CanonicalFamily::mesh_gltf2;
    output.canonical_contract_version = task.canonical_contract_version;
    output.evidence = meshEvidence();
    EXPECT_NO_THROW(validateOutputDeclaration(output, task));

    const std::string manifest = outputManifestSha256({output});
    EXPECT_EQ(manifest.size(), 64U);
    auto drifted = output;
    drifted.evidence.semantic_hash = kHashB;
    EXPECT_NE(outputManifestSha256({drifted}), manifest);

    Completion completion;
    completion.outcome = CompletionOutcome::clipped;
    completion.proof = boundaryProof(task);
    completion.ordered_output_ids = {output.output_id};
    completion.output_manifest_sha256 = manifest;
    const Json completed = Json::parse(serializeCompleteRequest(
            "clipper-v2-worker", task, completion));
    EXPECT_EQ(completed.at("proof").at("proofHash"),
              completion.proof.proof_hash);
    EXPECT_EQ(completed.at("outputManifestSha256"), manifest);
}

TEST(ClipperV2ContractTest, InstanceBoundaryUsesOnlyThePinnedMeshSlot) {
    auto task = meshTask();
    task.canonical_family = CanonicalFamily::instance_gltf2;
    task.canonical_contract_version =
            normalization::v3::kInstanceCanonicalContractVersion;
    task.clip_strategy_version = kInstanceClipStrategyVersion;
    OutputDeclaration boundary;
    boundary.output_ordinal = 1U;
    boundary.output_id = clippedOutputId(task.work_item_id, 1U);
    boundary.canonical_family = CanonicalFamily::mesh_gltf2;
    boundary.canonical_contract_version =
            normalization::v3::kMeshCanonicalContractVersion;
    boundary.evidence = meshEvidence();
    EXPECT_NO_THROW(validateOutputDeclaration(boundary, task));
    boundary.output_ordinal = 0U;
    boundary.output_id = clippedOutputId(task.work_item_id, 0U);
    EXPECT_THROW(validateOutputDeclaration(boundary, task),
                 std::invalid_argument);
}

}  // namespace
}  // namespace clip_worker::authorization::v2
