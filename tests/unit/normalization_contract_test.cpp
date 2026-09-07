#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/normalization_contract.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef CLIP_WORKER_SOURCE_DIR
#define CLIP_WORKER_SOURCE_DIR "."
#endif

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

constexpr const char* kHash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kValidatorHash =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::uint32_t value) {
    ASSERT_LE(offset + 4U, bytes.size());
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

Json canonicalRoot(CanonicalFamily family) {
    const int mode = family == CanonicalFamily::point_gltf2 ? 0 : 4;
    const std::uint64_t position_count =
            family == CanonicalFamily::point_gltf2 ? 1U : 3U;
    const std::uint64_t buffer_length = position_count * 12U;
    Json root = {
            {"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
            {"nodes", Json::array({{{"mesh", 0}}})},
            {"meshes", Json::array({{{"primitives", Json::array({
                    {{"attributes", {{"POSITION", 0}}}, {"mode", mode}}})}}})},
            {"accessors", Json::array({{{"bufferView", 0},
                                         {"componentType", 5126},
                                         {"count", position_count},
                                         {"type", "VEC3"},
                                         {"min", Json::array({0.0, 0.0, 0.0})},
                                         {"max", Json::array({0.0, 0.0, 0.0})}}})},
            {"bufferViews", Json::array({{{"buffer", 0},
                                           {"byteLength", buffer_length}}})},
            {"buffers", Json::array({{{"byteLength", buffer_length}}})}};
    if (family == CanonicalFamily::instance_gltf2) {
        root["extensionsUsed"] = Json::array({"EXT_mesh_gpu_instancing"});
        root["extensionsRequired"] = Json::array({"EXT_mesh_gpu_instancing"});
        root["nodes"][0]["extensions"] = {
                {"EXT_mesh_gpu_instancing", {{"attributes", {{"TRANSLATION", 0}}}}}};
    }
    return root;
}

std::vector<std::uint8_t> buildGlb(Json root) {
    const std::size_t bin_length = static_cast<std::size_t>(
            root.at("buffers").at(0U).at("byteLength").get<std::uint64_t>());
    std::string json = root.dump();
    while (json.size() % 4U != 0U) json.push_back(' ');
    std::vector<std::uint8_t> bin(bin_length, 0U);
    std::vector<std::uint8_t> bytes;
    appendU32(bytes, 0x46546c67U);
    appendU32(bytes, 2U);
    appendU32(bytes, static_cast<std::uint32_t>(
            12U + 8U + json.size() + 8U + bin.size()));
    appendU32(bytes, static_cast<std::uint32_t>(json.size()));
    appendU32(bytes, 0x4e4f534aU);
    bytes.insert(bytes.end(), json.begin(), json.end());
    appendU32(bytes, static_cast<std::uint32_t>(bin.size()));
    appendU32(bytes, 0x004e4942U);
    bytes.insert(bytes.end(), bin.begin(), bin.end());
    return bytes;
}

std::vector<std::uint8_t> canonicalGlb(CanonicalFamily family,
                                       bool external_uri = false) {
    Json root = canonicalRoot(family);
    if (external_uri) root["buffers"][0]["uri"] = "outside.bin";
    return buildGlb(std::move(root));
}

ToolVersion validator() {
    return {"SYNTHETIC_GLTF_VALIDATOR", "1.0.0", kValidatorHash};
}

std::vector<std::uint8_t> fixtureBytes(const char* name) {
    const std::filesystem::path path =
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR) / "tests" / "fixtures"
            / "normalization" / "generated" / name;
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) throw std::runtime_error("Normalizer fixture is missing");
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

Json protocolFixture(const char* name) {
    const std::filesystem::path path =
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR) / "tests" / "fixtures"
            / "protocol" / "normalizer-v1" / name;
    std::ifstream input(path);
    if (!input.good()) throw std::runtime_error("Normalizer protocol fixture is missing");
    return Json::parse(input);
}

ResourceRecord resource(const std::string& object_id,
                        std::vector<std::string> dependencies = {}) {
    ResourceRecord record;
    record.object_id = object_id;
    record.package_relative_path = object_id + ".glb";
    record.resource_role = "CONTENT";
    record.detected_kind = "GLB";
    record.size = 128U;
    record.sha256 = kHash;
    record.dependency_ids = std::move(dependencies);
    record.access_grant = {kHash, "GET", "https://broker.invalid/resource",
                           "2026-08-22T01:05:00Z"};
    return record;
}

ClaimTask taskFor(const std::vector<ResourceRecord>& records) {
    ResourceManifestPage page;
    page.manifest_id = kHash;
    page.page_number = 1U;
    page.record_count = static_cast<std::uint32_t>(records.size());
    page.records = records;
    page.page_sha256 = canonicalPageSha256(page.records);
    ClaimTask task;
    task.task_id = "task-1";
    task.attempt_id = "attempt-1";
    task.request_id = "request-1";
    task.lease_token = "lease-1";
    task.lease_expire_time = "2026-08-22T01:01:00Z";
    task.hard_deadline_time = "2026-08-22T01:10:00Z";
    task.tile_content_id = "content-1";
    task.source_closure_hash = kHash;
    task.resource_closure_version = "CONTENT_RESOURCE_CLOSURE_V1";
    task.normalization_version = kNormalizationVersion;
    task.canonical_contract_version = kCanonicalContractVersion;
    task.resource_profile_version = "profile-v1";
    task.resource_profile_sha256 = kHash;
    task.resource_manifest = {kManifestVersion, kHash, 1U, 1000U,
                              records.size(), canonicalManifestSha256({page})};
    return task;
}

TEST(NormalizationContractTest, SchemaDigestIsMirroredByteForByte) {
    const std::filesystem::path path =
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR) / "config"
            / "normalizer-protocol-v1.schema.json";
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.good());
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    EXPECT_EQ(kSchemaSha256, sha256Hex(bytes));
    EXPECT_EQ(std::string::npos, bytes.find("accessKey"));
    EXPECT_EQ(std::string::npos, bytes.find("secretKey"));
    EXPECT_EQ(std::string::npos, bytes.find("bucketName"));
}

TEST(NormalizationContractTest, SharedProtocolFixtureRoundTripsCanonicalEvidence) {
    const Json fixture = protocolFixture("positive-contract.json");
    const ClaimTask claimed = parseClaimTask(fixture.at("claimResponse").dump());
    const ResourceManifestPage page = parseResourceManifestPage(
            fixture.at("resourceManifestPage").dump());
    ClaimRequest request;
    request.worker_id = "worker-1";
    request.decoder_capabilities = {
            "DRACO", "JPEG", "KTX2", "MESHOPT", "PNG", "WEBP"};
    request.supported_normalization_versions = {kNormalizationVersion};
    request.family_capabilities = {{CanonicalFamily::mesh_gltf2,
                                    kCanonicalContractVersion,
                                    "SYNTHETIC_GLTF_VALIDATOR", "1.0.0",
                                    kValidatorHash, 4294967296ULL,
                                    4294967296ULL}};
    request.resource_profile_version = "STANDARD_4CPU_8GIB_SINGLE_TASK_V1";
    request.resource_profile_sha256 =
            "a65be19950a48f15c9a0275dda5a0b8b8e9e309feab84cbc10584dd70713c093";
    request.tool_versions = {validator()};
    validateClaimTask(claimed, request, "2026-08-22T01:00:00Z");
    validateResourceManifest(claimed, {page}, "2026-08-22T01:00:00Z",
                             request.family_capabilities.at(0U)
                                     .maximum_input_bytes);

    UploadDeclaration declaration;
    declaration.canonical_family = CanonicalFamily::mesh_gltf2;
    declaration.canonical_contract_version = kCanonicalContractVersion;
    declaration.output_size = 396U;
    declaration.output_sha256 =
            "446bfc4170db358bac35ce93548e7da735557f74ec9792797225f6935455094d";
    declaration.semantic_hash =
            "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    declaration.validation_manifest_sha256 =
            "e685490ad007476aba03b5f4b38295bf71d8f2bde4fe7834411bafe666e70bff";
    auto& summary = declaration.validation_summary;
    summary.scene_count = 1U;
    summary.node_count = 1U;
    summary.primitive_count = 1U;
    summary.accessor_count = 1U;
    summary.buffer_count = 1U;
    summary.feature_identity_model = FeatureIdentityModel::none;
    summary.validator_name = "SYNTHETIC_GLTF_VALIDATOR";
    summary.validator_version = "1.0.0";
    summary.validator_build_sha256 = kValidatorHash;
    summary.validation_hash =
            "6b1f679ccd1fbd81cea51d572213fe0d87e1ceedfb94df9c65d4fdc67d07847c";
    EXPECT_EQ(fixture.at("uploadPrepareRequest"),
              Json::parse(serializeUploadPrepareRequest(
                      request.worker_id, claimed, declaration)));

    const auto output = fixtureBytes("mesh-positive.glb");
    EXPECT_EQ(declaration.output_size, output.size());
    EXPECT_EQ(declaration.output_sha256,
              sha256Hex(std::string(reinterpret_cast<const char*>(output.data()),
                                    output.size())));
}

TEST(NormalizationContractTest, SharedNegativeFixtureFailsClosed) {
    const Json fixture = protocolFixture("negative-contract.json");
    EXPECT_THROW(static_cast<void>(parseClaimTask(
                         fixture.at("credentialBearingClaimResponse").dump())),
                 std::invalid_argument);
    Failure failure;
    failure.error_code = "INTERNAL_FAILURE";
    failure.error_message =
            fixture.at("unsafeFailure").at("errorMessage").get<std::string>();
    failure.retryable = true;
    EXPECT_THROW(static_cast<void>(serializeFailRequest(
                         "worker-1", taskFor({resource("content")}), failure)),
                 std::invalid_argument);
}

TEST(NormalizationContractTest, ManifestRejectsDependencyOutsideApprovedClosure) {
    ResourceManifestPage page;
    page.manifest_id = kHash;
    page.page_number = 1U;
    page.records = {resource("content", {"missing"})};
    page.record_count = 1U;
    page.page_sha256 = canonicalPageSha256(page.records);
    ClaimTask task = taskFor(page.records);

    EXPECT_THROW(validateResourceManifest(
                         task, {page}, "2026-08-22T01:00:00Z"),
                 std::invalid_argument);
}

TEST(NormalizationContractTest, CanonicalFamiliesAreValidatedDeterministically) {
    for (const CanonicalFamily family : {CanonicalFamily::mesh_gltf2,
                                         CanonicalFamily::point_gltf2,
                                         CanonicalFamily::instance_gltf2}) {
        const auto bytes = canonicalGlb(family);
        const auto first = validateCanonicalGlb(bytes, family, validator());
        const auto second = validateCanonicalGlb(bytes, family, validator());
        EXPECT_EQ(first.output_sha256, second.output_sha256);
        EXPECT_EQ(first.semantic_hash, second.semantic_hash);
        EXPECT_EQ(first.validation_manifest_sha256,
                  second.validation_manifest_sha256);
        EXPECT_EQ(kCoordinateBasis,
                  first.validation_summary.coordinate_basis);
        EXPECT_EQ(1U, first.validation_summary.primitive_count);
    }
}

TEST(NormalizationContractTest, CanonicalArtifactRejectsExternalResources) {
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         canonicalGlb(CanonicalFamily::mesh_gltf2, true),
                         CanonicalFamily::mesh_gltf2, validator())),
                 std::invalid_argument);
}

TEST(NormalizationContractTest, CanonicalArtifactRejectsMalformedCommonEvidence) {
    auto misaligned = canonicalGlb(CanonicalFamily::mesh_gltf2);
    const std::uint32_t json_length = static_cast<std::uint32_t>(misaligned[12U])
            | (static_cast<std::uint32_t>(misaligned[13U]) << 8U)
            | (static_cast<std::uint32_t>(misaligned[14U]) << 16U)
            | (static_cast<std::uint32_t>(misaligned[15U]) << 24U);
    writeU32(misaligned, 12U, json_length - 1U);
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         misaligned, CanonicalFamily::mesh_gltf2, validator())),
                 std::invalid_argument);

    auto non_finite = canonicalGlb(CanonicalFamily::mesh_gltf2);
    writeU32(non_finite, non_finite.size() - 36U, 0x7fc00000U);
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         non_finite, CanonicalFamily::mesh_gltf2, validator())),
                 std::invalid_argument);

    Json cycle = canonicalRoot(CanonicalFamily::mesh_gltf2);
    cycle["nodes"][0]["children"] = Json::array({0});
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         buildGlb(cycle), CanonicalFamily::mesh_gltf2,
                         validator())),
                 std::invalid_argument);

    Json bad_bounds = canonicalRoot(CanonicalFamily::mesh_gltf2);
    bad_bounds["accessors"][0]["min"] = Json::array({1.0, 0.0, 0.0});
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         buildGlb(bad_bounds), CanonicalFamily::mesh_gltf2,
                         validator())),
                 std::invalid_argument);

    Json bad_range = canonicalRoot(CanonicalFamily::mesh_gltf2);
    bad_range["bufferViews"][0]["byteLength"] = 8U;
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         buildGlb(bad_range), CanonicalFamily::mesh_gltf2,
                         validator())),
                 std::invalid_argument);
}

TEST(NormalizationContractTest, CommittedCanonicalFixturesCoverPositiveAndNegativeCases) {
    EXPECT_NO_THROW(static_cast<void>(validateCanonicalGlb(
            fixtureBytes("mesh-positive.glb"), CanonicalFamily::mesh_gltf2,
            validator())));
    EXPECT_NO_THROW(static_cast<void>(validateCanonicalGlb(
            fixtureBytes("point-positive.glb"), CanonicalFamily::point_gltf2,
            validator())));
    EXPECT_NO_THROW(static_cast<void>(validateCanonicalGlb(
            fixtureBytes("instance-positive.glb"),
            CanonicalFamily::instance_gltf2, validator())));
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         fixtureBytes("external-uri-negative.glb"),
                         CanonicalFamily::mesh_gltf2, validator())),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         fixtureBytes("unknown-required-extension-negative.glb"),
                         CanonicalFamily::mesh_gltf2, validator())),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateCanonicalGlb(
                         fixtureBytes("bad-length-negative.glb"),
                         CanonicalFamily::mesh_gltf2, validator())),
                 std::invalid_argument);
}

TEST(NormalizationContractTest, VersionUpgradeCannotReuseOldWorkerCapability) {
    const auto records = std::vector<ResourceRecord>{resource("content")};
    ClaimTask task = taskFor(records);
    task.normalization_version = "normalization-v2";
    ClaimRequest request;
    request.worker_id = "worker-1";
    request.supported_normalization_versions = {kNormalizationVersion};
    request.family_capabilities = {{CanonicalFamily::mesh_gltf2,
                                    kCanonicalContractVersion,
                                    "validator", "1", kValidatorHash,
                                    1024U, 1024U}};
    request.resource_profile_version = "profile-v1";
    request.resource_profile_sha256 = kHash;
    request.tool_versions = {validator()};

    EXPECT_THROW(validateClaimTask(task, request, "2026-08-22T01:00:00Z"),
                 std::invalid_argument);
}

}  // namespace
}  // namespace clip_worker::normalization
