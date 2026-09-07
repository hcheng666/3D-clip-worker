#include "clip_worker/normalization/canonical_input_reader.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/instance_canonical_writer.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"
#include "clip_worker/normalization/point_canonical_writer.hpp"

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

constexpr const char* kValidatorHash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

ToolVersion validator(const char* family) {
    return {std::string(family) + "-validator", "1.0.0", kValidatorHash};
}

CanonicalInputExpectation expectation(
        CanonicalFamily family, const char* contract,
        const CanonicalArtifactEvidence& evidence,
        const ToolVersion& tool) {
    CanonicalInputExpectation result;
    result.family = family;
    result.canonical_contract_version = contract;
    result.input_size = evidence.output_size;
    result.input_sha256 = evidence.output_sha256;
    result.semantic_hash = metadataSemanticHash(evidence);
    result.validation_manifest_sha256 =
            evidence.validation_manifest_sha256;
    result.validator = tool;
    return result;
}

mesh::MeshScene meshScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {-1.0F, 0.0F, 1.0F,
                           1.0F, 0.0F, 1.0F,
                           0.0F, 0.0F, -1.0F};
    primitive.indices = {0U, 1U, 2U};
    mesh::MeshScene scene;
    scene.default_scene = 0U;
    scene.scenes = {{0U}};
    scene.nodes = {{0U, geometry::Matrix4::identity(), 0U, {}}};
    scene.meshes = {{{std::move(primitive)}, {}}};
    mesh::validateMeshScene(scene);
    return scene;
}

point::PointScene pointScene() {
    point::PointScene scene;
    scene.positions = {0.0F, 0.0F, 0.0F,
                       1.0F, 0.0F, 0.0F};
    scene.colors_rgba = {255U, 0U, 0U, 255U,
                         0U, 255U, 0U, 255U};
    scene.feature_ids = {0U, 1U};
    point::validatePointScene(scene);
    return scene;
}

instance::InstanceScene instanceScene() {
    auto model = meshScene();
    mesh::MeshNodeInstancing instances;
    instances.translations = {0.0F, 0.0F, 0.0F,
                              2.0F, 0.0F, 0.0F};
    instances.rotations = {0.0F, 0.0F, 0.0F, 1.0F,
                           0.0F, 0.0F, 0.0F, 1.0F};
    instances.scales = {1.0F, 1.0F, 1.0F,
                        1.0F, 1.0F, 1.0F};
    instances.feature_ids = {0U, 1U};
    model.nodes.front().instancing = std::move(instances);
    instance::InstanceScene scene;
    scene.model = std::move(model);
    instance::validateInstanceScene(scene);
    return scene;
}

TEST(CanonicalInputReaderTest, ReopensAllMetadataCanonicalFamilies) {
    const auto mesh_tool = validator("mesh");
    const auto mesh_written = MeshCanonicalWriter::write(meshScene());
    const auto mesh_evidence = validateCanonicalGlb(
            mesh_written.glb, CanonicalFamily::mesh_gltf2, mesh_tool);
    const auto mesh_read = CanonicalMeshReader::read(
            mesh_written.glb,
            expectation(CanonicalFamily::mesh_gltf2,
                        v3::kMeshCanonicalContractVersion,
                        mesh_evidence, mesh_tool));
    EXPECT_EQ(mesh_read.scene.meshes.front().primitives.front().triangleCount(),
              1U);

    const auto point_tool = validator("point");
    const auto point_written = PointCanonicalWriter::write(pointScene());
    const auto point_evidence = validateCanonicalGlb(
            point_written.glb, CanonicalFamily::point_gltf2, point_tool);
    const auto point_read = CanonicalPointReader::read(
            point_written.glb,
            expectation(CanonicalFamily::point_gltf2,
                        v3::kPointCanonicalContractVersion,
                        point_evidence, point_tool));
    EXPECT_EQ(point_read.scene.pointCount(), 2U);

    const auto instance_tool = validator("instance");
    const auto instance_written = InstanceCanonicalWriter::write(
            instanceScene(), instance_tool);
    const auto instance_read = CanonicalInstanceReader::read(
            instance_written.canonical.glb,
            expectation(CanonicalFamily::instance_gltf2,
                        v3::kInstanceCanonicalContractVersion,
                        instance_written.evidence, instance_tool));
    EXPECT_EQ(instance_read.scene.instanceCount(), 2U);
}

TEST(CanonicalInputReaderTest, RejectsSemanticIdentityDriftBeforeClipping) {
    const auto tool = validator("mesh");
    const auto written = MeshCanonicalWriter::write(meshScene());
    const auto evidence = validateCanonicalGlb(
            written.glb, CanonicalFamily::mesh_gltf2, tool);
    auto expected = expectation(
            CanonicalFamily::mesh_gltf2,
            v3::kMeshCanonicalContractVersion, evidence, tool);
    expected.semantic_hash = std::string(64U, 'b');

    EXPECT_THROW(CanonicalMeshReader::read(written.glb, expected),
                 formats::FormatError);
}

}  // namespace
}  // namespace clip_worker::normalization
