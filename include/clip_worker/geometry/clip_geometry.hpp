#pragma once

#include <array>
#include <vector>

namespace clip_worker::geometry {

struct ClipTolerances {
    static constexpr double kPlaneEpsilonMeters = 0.01;
    static constexpr double kMinimumTriangleAreaSquareMeters = 1.0e-8;
    static constexpr double kScopeDensifySegmentMeters = 10.0;
};

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

struct ClipVertex {
    Point2 projected;
    double projected_height = 0.0;
    std::array<double, 3> local_position{};
    std::vector<double> attributes;
};

using ScopeTriangle = std::array<Point2, 3>;
using ClippedTriangle = std::array<ClipVertex, 3>;

class TriangleClipper final {
public:
    [[nodiscard]] static std::vector<ClippedTriangle> clip(
            const ClippedTriangle& triangle,
            const std::vector<ScopeTriangle>& authorized_triangles);
};

}  // namespace clip_worker::geometry
