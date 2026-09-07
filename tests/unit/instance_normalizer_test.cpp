#include "clip_worker/normalization/instance_normalizer.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/instance_canonical_writer.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "support/synthetic_i3dm.hpp"

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

mesh::MeshScene modelScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {0.0F, 0.0F, 0.0F,
                           1.0F, 0.0F, 0.0F,
                           0.0F, 1.0F, 0.0F};
    primitive.normals = {0.0F, 0.0F, 1.0F,
                         0.0F, 0.0F, 1.0F,
                         0.0F, 0.0F, 1.0F};
    primitive.indices = {0U, 1U, 2U};
    mesh::MeshScene scene;
    scene.default_scene = 0U;
    scene.scenes = {{0U}};
    scene.nodes = {{0U, geometry::Matrix4::identity(), 0U, {}}};
    scene.meshes = {{{std::move(primitive)}, {}}};
    return scene;
}

ToolVersion validator() {
    return {"INSTANCE_CANONICAL_VALIDATOR", "1.0.0", std::string(64U, 'b')};
}

TEST(InstanceNormalizerTest, NormalizesEmbeddedModelTransformsAndMetadata) {
    const auto model = MeshCanonicalWriter::write(modelScene()).glb;
    InstanceNormalizationInput input;
    input.root_package_relative_path = "tiles/tile.i3dm";
    input.source_bytes = tests::makeEmbeddedI3dmFixture(model);

    const auto normalized = InstanceNormalizer().normalize(input);
    ASSERT_EQ(normalized.scene.instanceCount(), 2U);
    const auto& instances = *normalized.scene.model.nodes.front().instancing;
    EXPECT_EQ(instances.translations,
              (std::vector<float>{0.0F, 0.0F, -0.0F,
                                  10.0F, 30.0F, -20.0F}));
    EXPECT_EQ(instances.rotations,
              (std::vector<float>{0.0F, 0.0F, 0.0F, 1.0F,
                                  0.0F, 0.0F, 0.0F, 1.0F}));
    EXPECT_EQ(instances.scales,
              (std::vector<float>{2.0F, 6.0F, 4.0F,
                                  2.0F, 3.0F, 2.5F}));
    ASSERT_TRUE(normalized.scene.model.legacy_properties.has_value());

    const auto first = InstanceCanonicalWriter::write(normalized.scene, validator());
    const auto second = InstanceCanonicalWriter::write(normalized.scene, validator());
    EXPECT_EQ(first.canonical.glb, second.canonical.glb);
    EXPECT_EQ(first.instance_count, 2U);
    EXPECT_EQ(first.evidence.validation_summary.feature_identity_model,
              FeatureIdentityModel::instance_feature_id);
    EXPECT_EQ(first.evidence.validation_summary.feature_count, 2U);
}

TEST(InstanceNormalizerTest, ResolvesApprovedExternalModelRelativeToI3dm) {
    const auto model = MeshCanonicalWriter::write(modelScene()).glb;
    InstanceNormalizationInput input;
    input.root_package_relative_path = "tiles/tile.i3dm";
    input.source_bytes = tests::makeExternalI3dmFixture("models/tree.glb");
    input.approved_resources["tiles/models/tree.glb"] = {model, "model/gltf-binary"};

    const auto normalized = InstanceNormalizer().normalize(input);
    EXPECT_TRUE(normalized.external_model);
    EXPECT_EQ(normalized.scene.instanceCount(), 2U);
}

TEST(InstanceNormalizerTest, RejectsEnuOnlyOrientationWithStableCode) {
    const auto model = MeshCanonicalWriter::write(modelScene()).glb;
    try {
        static_cast<void>(InstanceNormalizer().normalize(
                {"tiles/tile.i3dm",
                 tests::makeEnuOnlyI3dmFixture(model), {}}));
        FAIL() << "ENU-only fixture must be unsupported";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::i3dm_enu_unsupported);
    }
}

TEST(InstanceNormalizerTest, DecodesOctOrientationDeterministically) {
    const auto model = MeshCanonicalWriter::write(modelScene()).glb;
    const auto first = InstanceNormalizer().normalize(
            {"tiles/tile.i3dm",
             tests::makeOctOrientedI3dmFixture(model), {}});
    const auto second = InstanceNormalizer().normalize(
            {"tiles/tile.i3dm",
             tests::makeOctOrientedI3dmFixture(model), {}});
    ASSERT_EQ(first.scene.instanceCount(), 1U);
    EXPECT_EQ(first.scene.model.nodes.front().instancing->rotations,
              second.scene.model.nodes.front().instancing->rotations);
}

TEST(InstanceNormalizerTest, UsesInstanceOrdinalsForPerInstanceProperties) {
    const auto glb = MeshCanonicalWriter::write(modelScene()).glb;
    InstanceNormalizationInput input;
    input.root_package_relative_path = "tiles/instances.i3dm";
    input.source_bytes = tests::makePerInstanceMetadataI3dmFixture(glb);

    const auto result = InstanceNormalizer().normalize(input);
    const auto& instancing = *result.scene.model.nodes.front().instancing;
    EXPECT_EQ(instancing.feature_ids,
              (std::vector<std::uint32_t>{0U, 1U}));
    ASSERT_TRUE(result.scene.model.legacy_properties.has_value());
    EXPECT_EQ(result.scene.model.legacy_properties->feature_count, 2U);
}

}  // namespace
}  // namespace clip_worker::normalization
