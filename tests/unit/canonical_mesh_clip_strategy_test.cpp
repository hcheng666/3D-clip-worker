#include "clip_worker/clip/canonical_mesh_clip_strategy.hpp"

#include "clip_worker/client/object_transfer.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::clip {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLongitudeDegrees = 120.0;
constexpr double kLatitudeDegrees = 30.0;
constexpr double kHalfSpanDegrees = 0.00005;

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
        bytes.push_back(static_cast<std::uint8_t>(
                (value >> (byte * 8U)) & 0xFFU));
    }
}

void appendF64(std::vector<std::uint8_t>& bytes, double value) {
    std::array<std::uint8_t, sizeof(value)> encoded{};
    std::memcpy(encoded.data(), &value, sizeof(value));
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

std::vector<std::uint8_t> scopeWkb() {
    std::vector<std::uint8_t> bytes{1U};
    appendU32(bytes, 3U);
    appendU32(bytes, 1U);
    appendU32(bytes, 5U);
    for (const std::array<double, 2>& point : {
                 std::array<double, 2>{kLongitudeDegrees - kHalfSpanDegrees,
                                       kLatitudeDegrees - kHalfSpanDegrees},
                 std::array<double, 2>{kLongitudeDegrees + kHalfSpanDegrees,
                                       kLatitudeDegrees - kHalfSpanDegrees},
                 std::array<double, 2>{kLongitudeDegrees + kHalfSpanDegrees,
                                       kLatitudeDegrees + kHalfSpanDegrees},
                 std::array<double, 2>{kLongitudeDegrees - kHalfSpanDegrees,
                                       kLatitudeDegrees + kHalfSpanDegrees},
                 std::array<double, 2>{kLongitudeDegrees - kHalfSpanDegrees,
                                       kLatitudeDegrees - kHalfSpanDegrees}}) {
        appendF64(bytes, point[0U]);
        appendF64(bytes, point[1U]);
    }
    return bytes;
}

geometry::Matrix4 enuToEcef() {
    constexpr double kSemiMajorAxis = 6378137.0;
    constexpr double kInverseFlattening = 298.257222101;
    const double longitude = kLongitudeDegrees * kPi / 180.0;
    const double latitude = kLatitudeDegrees * kPi / 180.0;
    const double flattening = 1.0 / kInverseFlattening;
    const double eccentricity_squared = flattening * (2.0 - flattening);
    const double sin_latitude = std::sin(latitude);
    const double cos_latitude = std::cos(latitude);
    const double sin_longitude = std::sin(longitude);
    const double cos_longitude = std::cos(longitude);
    const double prime_vertical = kSemiMajorAxis
            / std::sqrt(1.0 - eccentricity_squared
                                * sin_latitude * sin_latitude);
    return geometry::Matrix4::fromColumnMajor({
            -sin_longitude, cos_longitude, 0.0, 0.0,
            -sin_latitude * cos_longitude,
            -sin_latitude * sin_longitude, cos_latitude, 0.0,
            cos_latitude * cos_longitude,
            cos_latitude * sin_longitude, sin_latitude, 0.0,
            prime_vertical * cos_latitude * cos_longitude,
            prime_vertical * cos_latitude * sin_longitude,
            prime_vertical * (1.0 - eccentricity_squared) * sin_latitude,
            1.0});
}

normalization::MeshResourceProfile profile() {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/resource-limit-profiles-v2.json";
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    return normalization::MeshResourceProfile::load(
            path, normalization::kMeshResourceProfileVersion,
            client::sha256Hex(bytes));
}

normalization::ToolVersion validator() {
    return {"MESH_CANONICAL_VALIDATOR", "1.0.0", std::string(64U, 'b')};
}

mesh::MeshScene texturedFeatureScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {
            -10.0F, -4.0F, 0.0F,
            10.0F, -4.0F, 0.0F,
            0.0F, 10.0F, 0.0F};
    primitive.normals = {
            0.0F, 0.0F, 1.0F,
            0.0F, 0.0F, 1.0F,
            0.0F, 0.0F, 1.0F};
    primitive.texcoords_0 = {0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F};
    primitive.indices = {0U, 1U, 2U};
    primitive.feature_ids = {2U, 2U, 2U};
    primitive.material = 0U;

    mesh::MeshMaterial material;
    material.base_color_texture = mesh::TextureBinding{0U, 0U};
    mesh::RgbaImage image;
    image.width = 8U;
    image.height = 8U;
    image.pixels.assign(8U * 8U * 4U, 255U);
    mesh::LegacyPropertyColumn property;
    property.name = "name";
    property.kind = mesh::LegacyPropertyKind::string;
    property.string_values = {"removed-0", "removed-1", "retained"};

    mesh::MeshScene scene;
    scene.default_scene = 0U;
    scene.scenes = {{0U}};
    scene.nodes = {{0U, geometry::Matrix4::identity(), 0U, {}}};
    scene.meshes = {{{std::move(primitive)}, {}}};
    scene.materials = {std::move(material)};
    scene.textures = {{0U, {}}};
    scene.images = {std::move(image)};
    scene.legacy_properties = mesh::LegacyPropertyTable{
            3U, {std::move(property)}};
    return scene;
}

MeshSceneClipRequest request() {
    MeshSceneClipRequest result;
    result.scope_wkb = scopeWkb();
    result.tileset_transform = enuToEcef();
    result.gltf_up_axis = mesh::UpAxis::z;
    return result;
}

TEST(CanonicalMeshClipStrategyTest, RebuildsMaskedValidatedDeterministicOutput) {
    const auto resource_profile = profile();
    const auto first = CanonicalMeshClipStrategy::clip(
            texturedFeatureScene(), request(), resource_profile, validator());
    const auto second = CanonicalMeshClipStrategy::clip(
            texturedFeatureScene(), request(), resource_profile, validator());

    ASSERT_FALSE(first.empty);
    EXPECT_GT(first.geometry.output_triangles, 0U);
    EXPECT_GT(first.texture.retained_pixels, 0U);
    EXPECT_GT(first.texture.cleared_pixels, 0U);
    EXPECT_EQ(first.evidence.validation_summary.feature_count, 1U);
    EXPECT_EQ(first.evidence.validation_summary.metadata_property_count, 1U);
    EXPECT_EQ(first.canonical.glb, second.canonical.glb);
    EXPECT_EQ(first.evidence.semantic_hash, second.evidence.semantic_hash);
    EXPECT_EQ(first.evidence.validation_manifest_sha256,
              second.evidence.validation_manifest_sha256);
}

TEST(CanonicalMeshClipStrategyTest, DoesNotCreateGlbForEmptyResult) {
    auto clip_request = request();
    clip_request.tileset_transform = enuToEcef()
            * geometry::Matrix4::translation({1000.0, 0.0, 0.0});

    const auto result = CanonicalMeshClipStrategy::clip(
            texturedFeatureScene(), clip_request, profile(), validator());

    EXPECT_TRUE(result.empty);
    EXPECT_TRUE(result.canonical.glb.empty());
    EXPECT_TRUE(result.evidence.output_sha256.empty());
}

}  // namespace
}  // namespace clip_worker::clip
