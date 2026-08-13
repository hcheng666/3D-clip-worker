#include "clip_worker/geometry/authorization_scope.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::geometry {
namespace {

constexpr std::uint32_t kWkbPolygon = 3U;
constexpr std::uint32_t kWkbMultiPolygon = 6U;

using Coordinate = std::array<double, 2>;
using WkbRing = std::vector<Coordinate>;
using WkbPolygon = std::vector<WkbRing>;

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
    }
}

void appendDouble(std::vector<std::uint8_t>& bytes, double value) {
    std::array<std::uint8_t, sizeof(double)> encoded{};
    std::memcpy(encoded.data(), &value, sizeof(value));
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

void appendPolygonBody(std::vector<std::uint8_t>& bytes, const WkbPolygon& polygon) {
    appendUint32(bytes, static_cast<std::uint32_t>(polygon.size()));
    for (const WkbRing& ring : polygon) {
        appendUint32(bytes, static_cast<std::uint32_t>(ring.size()));
        for (const Coordinate& point : ring) {
            appendDouble(bytes, point[0]);
            appendDouble(bytes, point[1]);
        }
    }
}

std::vector<std::uint8_t> polygonWkb(const WkbPolygon& polygon) {
    std::vector<std::uint8_t> bytes{1U};
    appendUint32(bytes, kWkbPolygon);
    appendPolygonBody(bytes, polygon);
    return bytes;
}

std::vector<std::uint8_t> multiPolygonWkb(
        const std::vector<WkbPolygon>& polygons) {
    std::vector<std::uint8_t> bytes{1U};
    appendUint32(bytes, kWkbMultiPolygon);
    appendUint32(bytes, static_cast<std::uint32_t>(polygons.size()));
    for (const WkbPolygon& polygon : polygons) {
        bytes.push_back(1U);
        appendUint32(bytes, kWkbPolygon);
        appendPolygonBody(bytes, polygon);
    }
    return bytes;
}

WkbRing square(double minimum_x, double minimum_y,
               double maximum_x, double maximum_y) {
    return {{minimum_x, minimum_y}, {maximum_x, minimum_y},
            {maximum_x, maximum_y}, {minimum_x, maximum_y},
            {minimum_x, minimum_y}};
}

ClipVertex vertex(double x, double y) {
    ClipVertex value;
    value.projected = {x, y};
    value.local_position = {x, y, x + y};
    return value;
}

void expectSameFragments(const std::vector<ClippedTriangle>& expected,
                         const std::vector<ClippedTriangle>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t triangle = 0U; triangle < expected.size(); ++triangle) {
        for (std::size_t vertex_index = 0U; vertex_index < 3U; ++vertex_index) {
            EXPECT_DOUBLE_EQ(expected[triangle][vertex_index].projected.x,
                             actual[triangle][vertex_index].projected.x);
            EXPECT_DOUBLE_EQ(expected[triangle][vertex_index].projected.y,
                             actual[triangle][vertex_index].projected.y);
            EXPECT_EQ(expected[triangle][vertex_index].local_position,
                      actual[triangle][vertex_index].local_position);
        }
    }
}

void expectIndexedClipMatchesExhaustive(const AuthorizationScope& scope) {
    double minimum_x = std::numeric_limits<double>::infinity();
    double minimum_y = std::numeric_limits<double>::infinity();
    double maximum_x = -std::numeric_limits<double>::infinity();
    double maximum_y = -std::numeric_limits<double>::infinity();
    for (const ScopeTriangle& triangle : scope.triangles()) {
        for (const Point2& point : triangle) {
            minimum_x = std::min(minimum_x, point.x);
            minimum_y = std::min(minimum_y, point.y);
            maximum_x = std::max(maximum_x, point.x);
            maximum_y = std::max(maximum_y, point.y);
        }
    }
    const double padding = std::max(maximum_x - minimum_x, maximum_y - minimum_y) * 0.2;
    std::mt19937_64 random(0x4490AABBULL);
    std::uniform_real_distribution<double> x_coordinate(
            minimum_x - padding, maximum_x + padding);
    std::uniform_real_distribution<double> y_coordinate(
            minimum_y - padding, maximum_y + padding);
    AuthorizationTriangleIndex::QueryWorkspace workspace;
    for (std::size_t sample = 0U; sample < 500U; ++sample) {
        const ClippedTriangle source{
                vertex(x_coordinate(random), y_coordinate(random)),
                vertex(x_coordinate(random), y_coordinate(random)),
                vertex(x_coordinate(random), y_coordinate(random))};
        scope.queryTriangles(source, workspace);
        expectSameFragments(TriangleClipper::clip(source, scope.triangles()),
                            TriangleClipper::clip(source, workspace.triangles));
    }
}

std::vector<std::uint8_t> squareWkb() {
    return polygonWkb({square(120.0, 30.0, 120.0001, 30.0001)});
}

TEST(AuthorizationScopeTest, ParsesProjectsAndTriangulatesEpsg4490Polygon) {
    const auto scope = AuthorizationScope::fromWkb(squareWkb(), 4490);

    EXPECT_FALSE(scope.triangles().empty());
}

TEST(AuthorizationScopeTest, RejectsUnexpectedSrid) {
    EXPECT_THROW(static_cast<void>(AuthorizationScope::fromWkb(squareWkb(), 4326)),
                 std::invalid_argument);
}

TEST(AuthorizationScopeTest, IndexedClipMatchesExhaustiveForPolygonWithHole) {
    const auto scope = AuthorizationScope::fromWkb(polygonWkb({
            square(120.0, 30.0, 120.001, 30.001),
            square(120.0003, 30.0003, 120.0007, 30.0007)}), 4490);

    expectIndexedClipMatchesExhaustive(scope);
}

TEST(AuthorizationScopeTest, IndexedClipMatchesExhaustiveForMultiPolygon) {
    const auto scope = AuthorizationScope::fromWkb(multiPolygonWkb({
            {square(120.0, 30.0, 120.0004, 30.0004)},
            {square(120.001, 30.001, 120.0014, 30.0014)}}), 4490);

    expectIndexedClipMatchesExhaustive(scope);
}

}  // namespace
}  // namespace clip_worker::geometry
