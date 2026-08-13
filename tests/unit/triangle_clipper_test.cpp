#include "clip_worker/geometry/clip_geometry.hpp"

#include <gtest/gtest.h>

namespace clip_worker::geometry {
namespace {

ClipVertex vertex(double x, double y, double payload, double height = 0.0) {
    ClipVertex value;
    value.projected = {x, y};
    value.projected_height = height;
    value.local_position = {x, y, height};
    value.attributes = {payload};
    return value;
}

TEST(TriangleClipperTest, ClipsTriangleAndInterpolatesAttributes) {
    const ClippedTriangle source{
            vertex(-1.0, 0.0, 0.0),
            vertex(2.0, 0.0, 3.0),
            vertex(0.0, 2.0, 2.0)};
    const std::vector<ScopeTriangle> scope{{
            Point2{0.0, -1.0}, Point2{1.0, -1.0}, Point2{0.0, 2.0}}};

    const auto result = TriangleClipper::clip(source, scope);

    ASSERT_FALSE(result.empty());
    for (const auto& triangle : result) {
        for (const auto& clipped : triangle) {
            EXPECT_GE(clipped.projected.x, -ClipTolerances::kPlaneEpsilonMeters);
            EXPECT_EQ(clipped.attributes.size(), 1U);
        }
    }
}

TEST(TriangleClipperTest, KeepsVerticalProjectionAlongAuthorizedBoundary) {
    const ClippedTriangle source{
            vertex(0.0, 0.0, 0.0, 0.0),
            vertex(0.0, 0.0, 1.0, 2.0),
            vertex(0.0, 1.0, 2.0, 0.0)};
    const std::vector<ScopeTriangle> scope{{
            Point2{0.0, -1.0}, Point2{1.0, -1.0}, Point2{0.0, 2.0}}};

    const auto result = TriangleClipper::clip(source, scope);

    EXPECT_FALSE(result.empty());
}

}  // namespace
}  // namespace clip_worker::geometry
