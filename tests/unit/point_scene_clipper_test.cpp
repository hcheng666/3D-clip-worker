#include "clip_worker/clip/point_scene_clipper.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
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
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

std::vector<std::uint8_t> scopeWkb() {
    const std::array<std::array<double, 2U>, 5U> ring{{
            {120.0, 30.0}, {120.001, 30.0}, {120.001, 30.001},
            {120.0, 30.001}, {120.0, 30.0}}};
    std::vector<std::uint8_t> bytes{1U};
    appendU32(bytes, 3U);
    appendU32(bytes, 1U);
    appendU32(bytes, static_cast<std::uint32_t>(ring.size()));
    for (const auto& coordinate : ring) {
        appendDouble(bytes, coordinate[0]);
        appendDouble(bytes, coordinate[1]);
    }
    return bytes;
}

geometry::Matrix4 enuToEcef(double longitude_degrees,
                            double latitude_degrees) {
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

point::PointScene scene() {
    point::PointScene value;
    value.positions = {0.0F, 0.0F, 0.0F,
                       70.0F, 0.0F, 0.0F,
                       5.0F, 0.0F, 0.0F};
    value.colors_rgba = {255U, 0U, 0U, 255U,
                         0U, 255U, 0U, 255U,
                         0U, 0U, 255U, 255U};
    value.feature_ids = {2U, 5U, 2U};
    mesh::LegacyPropertyTable properties;
    properties.feature_count = 6U;
    mesh::LegacyPropertyColumn names;
    names.name = "name";
    names.kind = mesh::LegacyPropertyKind::string;
    names.string_values = {"zero", "one", "two", "three", "four", "five"};
    properties.columns.push_back(std::move(names));
    value.legacy_properties = std::move(properties);
    point::validatePointScene(value);
    return value;
}

TEST(PointSceneClipperTest, CompactsAlignedStreamsAndDenseFeatureRows) {
    const auto scope = geometry::AuthorizationScope::fromWkb(scopeWkb(), 4490);
    const auto result = PointSceneClipper::clip(
            scene(), enuToEcef(120.0005, 30.0005), scope);

    ASSERT_TRUE(result.scene.has_value());
    EXPECT_EQ(result.retained_point_ordinals,
              (std::vector<std::uint32_t>{0U, 2U}));
    EXPECT_EQ(result.scene->pointCount(), 2U);
    EXPECT_EQ(result.scene->feature_ids,
              (std::vector<std::uint32_t>{0U, 0U}));
    ASSERT_TRUE(result.scene->legacy_properties.has_value());
    EXPECT_EQ(result.scene->legacy_properties->feature_count, 1U);
    EXPECT_EQ(result.scene->legacy_properties->columns[0U].string_values,
              (std::vector<std::string>{"two"}));
    EXPECT_TRUE(result.scene->bounds.valid);
}

TEST(PointSceneClipperTest, ReturnsExplicitEmptyWithoutWritingArtifact) {
    auto source = scene();
    source.positions = {1000.0F, 0.0F, 0.0F};
    source.colors_rgba = {255U, 255U, 255U, 255U};
    source.feature_ids = {5U};
    const auto scope = geometry::AuthorizationScope::fromWkb(scopeWkb(), 4490);
    const auto result = PointSceneClipper::clip(
            std::move(source), enuToEcef(120.0005, 30.0005), scope);
    EXPECT_FALSE(result.scene.has_value());
}

}  // namespace
}  // namespace clip_worker::clip
