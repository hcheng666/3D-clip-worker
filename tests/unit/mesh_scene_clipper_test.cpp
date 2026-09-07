#include "clip_worker/clip/mesh_scene_clipper.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::clip {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCenterLongitudeDegrees = 120.0;
constexpr double kCenterLatitudeDegrees = 30.0;
constexpr double kScopeHalfSpanDegrees = 0.00005;

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
        bytes.push_back(static_cast<std::uint8_t>(
                (value >> (byte * 8U)) & 0xFFU));
    }
}

void appendDouble(std::vector<std::uint8_t>& bytes, double value) {
    std::array<std::uint8_t, sizeof(value)> encoded{};
    std::memcpy(encoded.data(), &value, sizeof(value));
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

std::vector<std::uint8_t> squareScopeWkb(
        double longitude = kCenterLongitudeDegrees,
        double latitude = kCenterLatitudeDegrees) {
    constexpr std::uint32_t kWkbPolygon = 3U;
    constexpr std::uint32_t kRingPointCount = 5U;
    std::vector<std::uint8_t> bytes{1U};
    appendUint32(bytes, kWkbPolygon);
    appendUint32(bytes, 1U);
    appendUint32(bytes, kRingPointCount);
    for (const std::array<double, 2>& point : {
                 std::array<double, 2>{longitude - kScopeHalfSpanDegrees,
                                       latitude - kScopeHalfSpanDegrees},
                 std::array<double, 2>{longitude + kScopeHalfSpanDegrees,
                                       latitude - kScopeHalfSpanDegrees},
                 std::array<double, 2>{longitude + kScopeHalfSpanDegrees,
                                       latitude + kScopeHalfSpanDegrees},
                 std::array<double, 2>{longitude - kScopeHalfSpanDegrees,
                                       latitude + kScopeHalfSpanDegrees},
                 std::array<double, 2>{longitude - kScopeHalfSpanDegrees,
                                       latitude - kScopeHalfSpanDegrees}}) {
        appendDouble(bytes, point[0U]);
        appendDouble(bytes, point[1U]);
    }
    return bytes;
}

geometry::Matrix4 localEnuTransform() {
    constexpr double kSemiMajorAxis = 6378137.0;
    constexpr double kInverseFlattening = 298.257222101;
    const double longitude = kCenterLongitudeDegrees * kPi / 180.0;
    const double latitude = kCenterLatitudeDegrees * kPi / 180.0;
    const double flattening = 1.0 / kInverseFlattening;
    const double eccentricity_squared = flattening * (2.0 - flattening);
    const double sin_latitude = std::sin(latitude);
    const double cos_latitude = std::cos(latitude);
    const double sin_longitude = std::sin(longitude);
    const double cos_longitude = std::cos(longitude);
    const double prime_vertical = kSemiMajorAxis
            / std::sqrt(1.0 - eccentricity_squared
                                * sin_latitude * sin_latitude);
    const double ecef_x = prime_vertical * cos_latitude * cos_longitude;
    const double ecef_y = prime_vertical * cos_latitude * sin_longitude;
    const double ecef_z = prime_vertical * (1.0 - eccentricity_squared)
                          * sin_latitude;
    return geometry::Matrix4::fromColumnMajor({
            -sin_longitude, cos_longitude, 0.0, 0.0,
            -sin_latitude * cos_longitude,
            -sin_latitude * sin_longitude, cos_latitude, 0.0,
            cos_latitude * cos_longitude,
            cos_latitude * sin_longitude, sin_latitude, 0.0,
            ecef_x, ecef_y, ecef_z, 1.0});
}

mesh::MeshScene crossingScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {
            -10.0F, -4.0F, 0.0F,
            10.0F, -4.0F, 0.0F,
            0.0F, 10.0F, 0.0F};
    primitive.normals = {
            0.0F, 0.0F, 2.0F,
            0.0F, 0.0F, 2.0F,
            0.0F, 0.0F, 2.0F};
    primitive.tangents = {
            2.0F, 0.0F, 0.0F, 1.0F,
            2.0F, 0.0F, 0.0F, 1.0F,
            2.0F, 0.0F, 0.0F, 1.0F};
    primitive.texcoords_0 = {0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F};
    primitive.texcoords_1 = {0.1F, 0.2F, 0.9F, 0.2F, 0.5F, 0.8F};
    primitive.colors = {
            1.0F, 0.0F, 0.0F, 1.0F,
            0.0F, 1.0F, 0.0F, 1.0F,
            0.0F, 0.0F, 1.0F, 1.0F};
    primitive.color_components = 4U;
    primitive.indices = {0U, 1U, 2U};
    primitive.feature_ids = {2U, 2U, 2U};

    mesh::MeshScene scene;
    scene.default_scene = 0U;
    scene.scenes = {{0U}};
    scene.nodes = {{0U, geometry::Matrix4::identity(), 0U, {}}};
    scene.meshes = {{{std::move(primitive)}, {}}};
    return scene;
}

MeshSceneClipRequest request() {
    MeshSceneClipRequest value;
    value.scope_wkb = squareScopeWkb();
    value.tileset_transform = localEnuTransform();
    value.gltf_up_axis = mesh::UpAxis::z;
    return value;
}

TEST(MeshSceneClipperTest, ClipsAndInterpolatesAllSupportedVertexStreams) {
    const auto result = MeshSceneClipper::clip(crossingScene(), request());

    ASSERT_FALSE(result.empty);
    ASSERT_EQ(result.scene.meshes.size(), 1U);
    ASSERT_EQ(result.scene.meshes.front().primitives.size(), 1U);
    const auto& primitive = result.scene.meshes.front().primitives.front();
    EXPECT_GT(primitive.triangleCount(), 0U);
    EXPECT_GT(primitive.vertexCount(), 3U);
    EXPECT_EQ(primitive.normals.size(), primitive.vertexCount() * 3U);
    EXPECT_EQ(primitive.tangents.size(), primitive.vertexCount() * 4U);
    EXPECT_EQ(primitive.texcoords_0.size(), primitive.vertexCount() * 2U);
    EXPECT_EQ(primitive.texcoords_1.size(), primitive.vertexCount() * 2U);
    EXPECT_EQ(primitive.colors.size(), primitive.vertexCount() * 4U);
    ASSERT_EQ(primitive.feature_ids.size(), primitive.vertexCount());
    for (const std::uint32_t feature_id : primitive.feature_ids) {
        EXPECT_EQ(feature_id, 2U);
    }
    for (std::size_t vertex = 0U; vertex < primitive.vertexCount(); ++vertex) {
        const std::size_t normal = vertex * 3U;
        const double normal_length = std::hypot(
                primitive.normals[normal],
                std::hypot(primitive.normals[normal + 1U],
                           primitive.normals[normal + 2U]));
        EXPECT_NEAR(normal_length, 1.0, 1.0e-6);
        const std::size_t tangent = vertex * 4U;
        const double tangent_length = std::hypot(
                primitive.tangents[tangent],
                std::hypot(primitive.tangents[tangent + 1U],
                           primitive.tangents[tangent + 2U]));
        EXPECT_NEAR(tangent_length, 1.0, 1.0e-6);
        EXPECT_EQ(primitive.tangents[tangent + 3U], 1.0F);
    }
    EXPECT_EQ(result.statistics.input_triangles, 1U);
    EXPECT_EQ(result.statistics.output_triangles, primitive.triangleCount());
}

TEST(MeshSceneClipperTest, ReusesIdenticalSharedMeshInstancesDeterministically) {
    auto scene = crossingScene();
    scene.scenes.front().push_back(1U);
    scene.nodes.push_back(
            {1U, geometry::Matrix4::identity(), 0U, {}});

    const auto result = MeshSceneClipper::clip(std::move(scene), request());

    ASSERT_FALSE(result.empty);
    ASSERT_EQ(result.scene.meshes.size(), 1U);
    ASSERT_TRUE(result.scene.nodes[0U].mesh.has_value());
    ASSERT_TRUE(result.scene.nodes[1U].mesh.has_value());
    EXPECT_EQ(result.scene.nodes[0U].mesh, result.scene.nodes[1U].mesh);
    EXPECT_EQ(result.statistics.clipped_mesh_instances, 1U);
    EXPECT_EQ(result.statistics.reused_mesh_instances, 1U);
}

TEST(MeshSceneClipperTest, ReturnsExplicitEmptyEvidenceForDisjointTransform) {
    auto clip_request = request();
    clip_request.tileset_transform = localEnuTransform()
            * geometry::Matrix4::translation({1000.0, 0.0, 0.0});

    const auto result = MeshSceneClipper::clip(
            crossingScene(), clip_request);

    EXPECT_TRUE(result.empty);
    EXPECT_TRUE(result.scene.meshes.empty());
    EXPECT_EQ(result.statistics.output_triangles, 0U);
}

TEST(MeshSceneClipperTest, RejectsTrianglesWithMixedFeatureIdentity) {
    auto scene = crossingScene();
    scene.meshes.front().primitives.front().feature_ids = {0U, 1U, 0U};

    EXPECT_THROW(MeshSceneClipper::clip(std::move(scene), request()),
                 formats::FormatError);
}

}  // namespace
}  // namespace clip_worker::clip
