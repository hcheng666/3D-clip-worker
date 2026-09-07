#include "clip_worker/clip/authorization_content_classifier.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::clip {
namespace {

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(
                (value >> (index * 8U)) & 0xffU));
    }
}

void appendDouble(std::vector<std::uint8_t>& bytes, double value) {
    std::array<std::uint8_t, sizeof(value)> encoded{};
    std::memcpy(encoded.data(), &value, sizeof(value));
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

using Ring = std::vector<std::array<double, 2U>>;

std::vector<std::uint8_t> polygonWkb(const std::vector<Ring>& rings) {
    std::vector<std::uint8_t> bytes{1U};
    appendU32(bytes, 3U);
    appendU32(bytes, static_cast<std::uint32_t>(rings.size()));
    for (const auto& ring : rings) {
        appendU32(bytes, static_cast<std::uint32_t>(ring.size()));
        for (const auto& coordinate : ring) {
            appendDouble(bytes, coordinate[0U]);
            appendDouble(bytes, coordinate[1U]);
        }
    }
    return bytes;
}

std::vector<std::uint8_t> scopeWkb(bool with_hole = false) {
    std::vector<Ring> rings{{
            {120.0, 30.0}, {120.001, 30.0}, {120.001, 30.001},
            {120.0, 30.001}, {120.0, 30.0}}};
    if (with_hole) {
        rings.push_back({
                {120.0004, 30.0004}, {120.0004, 30.0006},
                {120.0006, 30.0006}, {120.0006, 30.0004},
                {120.0004, 30.0004}});
    }
    return polygonWkb(rings);
}

geometry::Matrix4 enuToEcef(double longitude_degrees = 120.0005,
                            double latitude_degrees = 30.0005) {
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    constexpr double kSemiMajor = 6378137.0;
    constexpr double kEccentricitySquared = 6.69437999014e-3;
    const double longitude = longitude_degrees * kRadians;
    const double latitude = latitude_degrees * kRadians;
    const double sin_lon = std::sin(longitude);
    const double cos_lon = std::cos(longitude);
    const double sin_lat = std::sin(latitude);
    const double cos_lat = std::cos(latitude);
    const double prime_vertical = kSemiMajor
            / std::sqrt(1.0 - kEccentricitySquared * sin_lat * sin_lat);
    const std::array<double, 3U> origin{
            prime_vertical * cos_lat * cos_lon,
            prime_vertical * cos_lat * sin_lon,
            prime_vertical * (1.0 - kEccentricitySquared) * sin_lat};
    return geometry::Matrix4::fromColumnMajor({
            -sin_lon, cos_lon, 0.0, 0.0,
            -sin_lat * cos_lon, -sin_lat * sin_lon, cos_lat, 0.0,
            cos_lat * cos_lon, cos_lat * sin_lon, sin_lat, 0.0,
            origin[0], origin[1], origin[2], 1.0});
}

mesh::MeshScene meshScene(std::vector<float> positions,
                          geometry::Matrix4 local =
                                  geometry::Matrix4::identity()) {
    mesh::MeshPrimitive primitive;
    primitive.positions = std::move(positions);
    primitive.indices = {0U, 1U, 2U};
    mesh::MeshScene scene;
    scene.default_scene = 0U;
    scene.scenes = {{0U}};
    scene.nodes = {{0U, std::move(local), 0U, {}}};
    scene.meshes = {{{std::move(primitive)}, {}}};
    mesh::validateMeshScene(scene);
    return scene;
}

point::PointScene pointScene(std::vector<float> positions) {
    point::PointScene scene;
    scene.positions = std::move(positions);
    point::validatePointScene(scene);
    return scene;
}

instance::InstanceScene instanceScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {-10.0F, 0.0F, 10.0F,
                           10.0F, 0.0F, 10.0F,
                           0.0F, 0.0F, -10.0F};
    primitive.indices = {0U, 1U, 2U};
    mesh::MeshNodeInstancing instances;
    instances.translations = {0.0F, 0.0F, 0.0F,
                              48.0F, 0.0F, 0.0F,
                              120.0F, 0.0F, 0.0F};
    instances.rotations = {0.0F, 0.0F, 0.0F, 1.0F,
                           0.0F, 0.0F, 0.0F, 1.0F,
                           0.0F, 0.0F, 0.0F, 1.0F};
    instances.scales = {1.0F, 1.0F, 1.0F,
                        1.0F, 1.0F, 1.0F,
                        1.0F, 1.0F, 1.0F};
    instances.feature_ids = {0U, 1U, 2U};
    instance::InstanceScene scene;
    scene.model.default_scene = 0U;
    scene.model.scenes = {{0U}};
    mesh::MeshNode node;
    node.mesh = 0U;
    node.instancing = std::move(instances);
    scene.model.nodes.push_back(std::move(node));
    scene.model.meshes = {{{std::move(primitive)}, {}}};
    instance::validateInstanceScene(scene);
    return scene;
}

TEST(AuthorizationContentClassifierTest,
     MeshHoleIntersectionCannotBecomeSafeWholeFromOuterBounds) {
    const auto authorization = geometry::AuthorizationScope::fromWkb(
            scopeWkb(true), 4490);
    const auto summary = MeshAuthorizationClassifier::classify(
            meshScene({-30.0F, 0.0F, 20.0F,
                       30.0F, 0.0F, 20.0F,
                       0.0F, 0.0F, -30.0F}),
            enuToEcef(), authorization);

    EXPECT_EQ(summary.relation, AuthorizationContentRelation::boundary);
    EXPECT_EQ(summary.input_element_count, 1U);
    EXPECT_EQ(summary.boundary_element_count, 1U);
}

TEST(AuthorizationContentClassifierTest,
     MeshAndPointClassifiersDistinguishWholeEmptyAndBoundary) {
    const auto authorization = geometry::AuthorizationScope::fromWkb(
            scopeWkb(), 4490);
    EXPECT_EQ(MeshAuthorizationClassifier::classify(
                      meshScene({-10.0F, 0.0F, 10.0F,
                                 10.0F, 0.0F, 10.0F,
                                 0.0F, 0.0F, -10.0F}),
                      enuToEcef(), authorization).relation,
              AuthorizationContentRelation::safe_whole);
    EXPECT_EQ(MeshAuthorizationClassifier::classify(
                      meshScene({-10.0F, 0.0F, 10.0F,
                                 10.0F, 0.0F, 10.0F,
                                 0.0F, 0.0F, -10.0F},
                                geometry::Matrix4::translation(
                                        {1000.0, 0.0, 0.0})),
                      enuToEcef(), authorization).relation,
              AuthorizationContentRelation::empty);
    EXPECT_EQ(PointAuthorizationClassifier::classify(
                      pointScene({0.0F, 0.0F, 0.0F,
                                  70.0F, 0.0F, 0.0F}),
                      enuToEcef(), authorization).relation,
              AuthorizationContentRelation::boundary);
}

TEST(AuthorizationContentClassifierTest,
     InstanceClassifierUsesWholeBoundaryAndDisjointHullCounts) {
    const auto authorization = geometry::AuthorizationScope::fromWkb(
            scopeWkb(), 4490);
    const auto summary = InstanceAuthorizationClassifier::classify(
            instanceScene(), enuToEcef(), authorization);

    EXPECT_EQ(summary.relation, AuthorizationContentRelation::boundary);
    EXPECT_EQ(summary.input_element_count, 3U);
    EXPECT_EQ(summary.whole_element_count, 1U);
    EXPECT_EQ(summary.boundary_element_count, 1U);
    EXPECT_EQ(summary.disjoint_element_count, 1U);
}

}  // namespace
}  // namespace clip_worker::clip
