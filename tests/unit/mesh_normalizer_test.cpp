#include "clip_worker/normalization/mesh_normalizer.hpp"

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/glb.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/metadata_canonical_writer.hpp"
#include "support/synthetic_tile.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

std::filesystem::path profilePath() {
    return std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/resource-limit-profiles-v2.json";
}

MeshResourceProfile profile() {
    std::ifstream input(profilePath(), std::ios::binary);
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    return MeshResourceProfile::load(
            profilePath(), kMeshResourceProfileVersion,
            client::sha256Hex(bytes));
}

ToolVersion validator() {
    return {"MESH_CANONICAL_VALIDATOR", "1.0.0", std::string(64U, 'b')};
}

mesh::MeshScene triangleScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {
            0.0F, 0.0F, 0.0F,
            1.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F};
    primitive.indices = {0U, 1U, 2U};
    mesh::MeshScene scene;
    scene.default_scene = 0U;
    scene.scenes = {{0U}};
    scene.nodes = {{0U, geometry::Matrix4::identity(), 0U, {}}};
    scene.meshes = {{{std::move(primitive)}, {}}};
    return scene;
}

TEST(MeshNormalizerTest, RewritesDirectCanonicalGlbDeterministically) {
    const auto source = MeshCanonicalWriter::write(triangleScene()).glb;
    MeshNormalizationInput input;
    input.source_kind = MeshSourceKind::glb;
    input.root_package_relative_path = "content.glb";
    input.source_bytes = source;

    const auto first = MeshNormalizer::normalize(input, profile(), validator());
    const auto second = MeshNormalizer::normalize(input, profile(), validator());

    EXPECT_EQ(first.canonical.glb, source);
    EXPECT_EQ(first.canonical.glb, second.canonical.glb);
    EXPECT_EQ(first.evidence.output_sha256, second.evidence.output_sha256);
    EXPECT_EQ(first.evidence.semantic_hash, second.evidence.semantic_hash);
    EXPECT_EQ(first.evidence.validation_manifest_sha256,
              second.evidence.validation_manifest_sha256);
}

TEST(MeshNormalizerTest, NormalizesMultiStageB3dmTextureAndLegacyMetadata) {
    const auto fixture = tests::makeTexturedMeshFixture();
    MeshNormalizationInput input;
    input.source_kind = MeshSourceKind::b3dm;
    input.root_package_relative_path = "content.b3dm";
    input.source_bytes = fixture.b3dm;

    const auto result = MeshNormalizer::normalize(input, profile(), validator());

    EXPECT_FALSE(result.canonical.glb.empty());
    EXPECT_EQ(result.evidence.validation_summary.image_count, 1U);
    EXPECT_EQ(result.evidence.validation_summary.feature_count, 1U);
    EXPECT_EQ(result.evidence.validation_summary.feature_identity_model,
              FeatureIdentityModel::attribute_feature_id_property_table);
    EXPECT_GT(result.canonical.texture_bytes, 0U);
}

TEST(MeshNormalizerTest, RejectsEmptyOrOverLimitSourceBeforeDecode) {
    MeshNormalizationInput input;
    input.source_kind = MeshSourceKind::glb;
    input.root_package_relative_path = "content.glb";
    EXPECT_THROW(MeshNormalizer::normalize(input, profile(), validator()),
                 formats::FormatError);
}

TEST(MeshNormalizerTest, ValidatesMultipleTypedFeatureSetsAndHierarchy) {
    auto scene = triangleScene();
    auto& primitive = scene.meshes.front().primitives.front();
    primitive.feature_id_sets = {
            {"building", 1U, 0U, std::nullopt, false, {0U, 0U, 0U}},
            {"category", 1U, std::nullopt, std::nullopt, false,
             {0U, 0U, 0U}}};
    metadata::PropertyColumn name;
    name.name = "name";
    name.scalar_type = metadata::PropertyScalarType::string;
    name.values = metadata::StringValues{{"retained"}};
    metadata::PropertyColumn owner;
    owner.name = "owner";
    owner.scalar_type = metadata::PropertyScalarType::string;
    owner.values = metadata::StringValues{{"authorized"}};
    metadata::FeatureMetadata feature_metadata;
    feature_metadata.property_tables = {
            {"Feature", 1U, {name}}, {"Owner", 1U, {owner}}};
    feature_metadata.hierarchy = metadata::PropertyHierarchy{
            {{1U, 0U, {}}}, {0U}};
    scene.feature_metadata = std::move(feature_metadata);

    const auto canonical = MeshCanonicalWriter::write(scene);
    const auto evidence = validateCanonicalGlb(
            canonical.glb, CanonicalFamily::mesh_gltf2, validator());
    EXPECT_EQ(evidence.validation_summary.feature_count, 2U);
    EXPECT_EQ(evidence.validation_summary.metadata_property_count, 2U);

    const auto document = formats::GlbParser::parse(
            formats::ByteView(canonical.glb));
    const auto root = nlohmann::json::parse(document.json_text);
    const auto& feature_ids = root.at("meshes").at(0U).at("primitives")
            .at(0U).at("extensions").at("EXT_mesh_features")
            .at("featureIds");
    ASSERT_EQ(feature_ids.size(), 2U);
    EXPECT_EQ(root.at("accessors").at(
                      root.at("meshes").at(0U).at("primitives").at(0U)
                              .at("attributes").at("_FEATURE_ID_0")
                              .get<std::size_t>())
                      .at("componentType"),
              5121U);
    EXPECT_EQ(root.at("extensionsRequired").at(0U),
              kCanonicalLegacyHierarchyExtension);
}

}  // namespace
}  // namespace clip_worker::normalization
