#include "clip_worker/geometry/authorization_triangle_index.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::geometry {
namespace {

ClipVertex vertex(double x, double y, double payload) {
    ClipVertex value;
    value.projected = {x, y};
    value.projected_height = payload * 0.25;
    value.local_position = {x, y, payload};
    value.attributes = {payload, x + y};
    return value;
}

std::vector<ScopeTriangle> gridTriangles(std::size_t width, std::size_t height) {
    std::vector<ScopeTriangle> triangles;
    triangles.reserve(width * height * 2U);
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            const double left = static_cast<double>(x) * 2.0;
            const double bottom = static_cast<double>(y) * 2.0;
            triangles.push_back({Point2{left, bottom}, Point2{left + 1.0, bottom},
                                 Point2{left, bottom + 1.0}});
            triangles.push_back({Point2{left + 1.0, bottom},
                                 Point2{left + 1.0, bottom + 1.0},
                                 Point2{left, bottom + 1.0}});
        }
    }
    return triangles;
}

bool sameScopeTriangle(const ScopeTriangle& first, const ScopeTriangle& second) {
    for (std::size_t vertex_index = 0U; vertex_index < first.size(); ++vertex_index) {
        if (first[vertex_index].x != second[vertex_index].x
                || first[vertex_index].y != second[vertex_index].y) {
            return false;
        }
    }
    return true;
}

void expectSameFragments(const std::vector<ClippedTriangle>& expected,
                         const std::vector<ClippedTriangle>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t triangle = 0U; triangle < expected.size(); ++triangle) {
        for (std::size_t vertex_index = 0U; vertex_index < 3U; ++vertex_index) {
            const ClipVertex& first = expected[triangle][vertex_index];
            const ClipVertex& second = actual[triangle][vertex_index];
            EXPECT_DOUBLE_EQ(first.projected.x, second.projected.x);
            EXPECT_DOUBLE_EQ(first.projected.y, second.projected.y);
            EXPECT_DOUBLE_EQ(first.projected_height, second.projected_height);
            EXPECT_EQ(first.local_position, second.local_position);
            EXPECT_EQ(first.attributes, second.attributes);
        }
    }
}

TEST(AuthorizationTriangleIndexTest, MatchesExhaustiveClipForDeterministicRandomTriangles) {
    const std::vector<ScopeTriangle> scope = gridTriangles(20U, 15U);
    AuthorizationTriangleIndex index(scope);
    std::mt19937_64 random(0x3D71E5ULL);
    std::uniform_real_distribution<double> coordinate(-3.0, 43.0);
    AuthorizationTriangleIndex::QueryWorkspace workspace;

    for (std::size_t sample = 0U; sample < 1000U; ++sample) {
        const ClippedTriangle source{
                vertex(coordinate(random), coordinate(random), 1.0),
                vertex(coordinate(random), coordinate(random), 2.0),
                vertex(coordinate(random), coordinate(random), 3.0)};
        index.query(source, workspace);
        expectSameFragments(TriangleClipper::clip(source, scope),
                            TriangleClipper::clip(source, workspace.triangles));
    }
}

TEST(AuthorizationTriangleIndexTest, KeepsCandidatesInsidePlaneTolerance) {
    const std::vector<ScopeTriangle> scope{{
            Point2{0.0, 0.0}, Point2{1.0, 0.0}, Point2{0.0, 1.0}}};
    AuthorizationTriangleIndex index(scope);
    const double offset = ClipTolerances::kPlaneEpsilonMeters * 0.5;
    const ClippedTriangle source{
            vertex(0.25, -offset, 1.0),
            vertex(0.75, -offset, 2.0),
            vertex(0.50, -offset * 0.5, 3.0)};
    AuthorizationTriangleIndex::QueryWorkspace workspace;

    index.query(source, workspace);

    ASSERT_EQ(1U, workspace.triangles.size());
    expectSameFragments(TriangleClipper::clip(source, scope),
                        TriangleClipper::clip(source, workspace.triangles));
}

TEST(AuthorizationTriangleIndexTest, MatchesExhaustiveForTouchingAndTinyTriangles) {
    const std::vector<ScopeTriangle> scope{{
            Point2{0.0, 0.0}, Point2{1.0, 0.0}, Point2{0.0, 1.0}}};
    AuthorizationTriangleIndex index(scope);
    AuthorizationTriangleIndex::QueryWorkspace workspace;
    const std::vector<ClippedTriangle> sources{
            {vertex(0.0, 0.0, 1.0), vertex(-1.0, 0.0, 2.0),
             vertex(0.0, -1.0, 3.0)},
            {vertex(0.25, 0.25, 1.0), vertex(0.2500001, 0.25, 2.0),
             vertex(0.25, 0.2500001, 3.0)},
            {vertex(2.0, 2.0, 1.0), vertex(3.0, 2.0, 2.0),
             vertex(2.0, 3.0, 3.0)}};

    for (const ClippedTriangle& source : sources) {
        index.query(source, workspace);
        expectSameFragments(TriangleClipper::clip(source, scope),
                            TriangleClipper::clip(source, workspace.triangles));
    }
}

TEST(AuthorizationTriangleIndexTest, ReturnsCandidatesInOriginalTriangulationOrder) {
    std::vector<ScopeTriangle> scope = gridTriangles(8U, 8U);
    AuthorizationTriangleIndex index(scope);
    const ClippedTriangle source{
            vertex(-1.0, -1.0, 1.0),
            vertex(20.0, -1.0, 2.0),
            vertex(-1.0, 20.0, 3.0)};
    AuthorizationTriangleIndex::QueryWorkspace workspace;

    index.query(source, workspace);

    std::size_t previous = 0U;
    bool first = true;
    for (const ScopeTriangle& candidate : workspace.triangles) {
        const auto found = std::find_if(scope.begin(), scope.end(),
                [&candidate](const ScopeTriangle& triangle) {
                    return sameScopeTriangle(triangle, candidate);
                });
        ASSERT_NE(scope.end(), found);
        const std::size_t current = static_cast<std::size_t>(found - scope.begin());
        if (!first) {
            EXPECT_LT(previous, current);
        }
        previous = current;
        first = false;
    }
}

TEST(AuthorizationTriangleIndexTest, ReducesCandidatesForLocalizedSourceTriangle) {
    const std::vector<ScopeTriangle> scope = gridTriangles(100U, 100U);
    AuthorizationTriangleIndex index(scope);
    const ClippedTriangle source{
            vertex(100.1, 100.1, 1.0),
            vertex(100.8, 100.1, 2.0),
            vertex(100.1, 100.8, 3.0)};
    AuthorizationTriangleIndex::QueryWorkspace workspace;

    index.query(source, workspace);

    EXPECT_FALSE(workspace.triangles.empty());
    EXPECT_LT(workspace.triangles.size(), scope.size() / 100U);
}

}  // namespace
}  // namespace clip_worker::geometry
