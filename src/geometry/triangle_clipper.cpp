#include "clip_worker/geometry/clip_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace clip_worker::geometry {
namespace {

double cross(const Point2& a, const Point2& b, const Point2& value) {
    return (b.x - a.x) * (value.y - a.y)
           - (b.y - a.y) * (value.x - a.x);
}

double edgeLength(const Point2& a, const Point2& b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

double signedDistance(const Point2& a, const Point2& b, const Point2& value) {
    const double length = edgeLength(a, b);
    if (length <= 0.0) {
        throw std::invalid_argument("Authorization triangulation contains a zero-length edge");
    }
    return cross(a, b, value) / length;
}

ClipVertex interpolate(const ClipVertex& first, const ClipVertex& second, double t) {
    if (first.attributes.size() != second.attributes.size()) {
        throw std::invalid_argument("Clip vertices have different attribute layouts");
    }
    const double clamped = std::max(0.0, std::min(1.0, t));
    ClipVertex result;
    result.projected.x = first.projected.x
                         + (second.projected.x - first.projected.x) * clamped;
    result.projected.y = first.projected.y
                         + (second.projected.y - first.projected.y) * clamped;
    result.projected_height = first.projected_height
                              + (second.projected_height
                                 - first.projected_height) * clamped;
    for (std::size_t index = 0; index < result.local_position.size(); ++index) {
        result.local_position[index] = first.local_position[index]
                                       + (second.local_position[index]
                                          - first.local_position[index]) * clamped;
    }
    result.attributes.resize(first.attributes.size());
    for (std::size_t index = 0; index < result.attributes.size(); ++index) {
        result.attributes[index] = first.attributes[index]
                                   + (second.attributes[index]
                                      - first.attributes[index]) * clamped;
    }
    return result;
}

std::vector<ClipVertex> clipAgainstEdge(const std::vector<ClipVertex>& polygon,
                                        const Point2& edge_start,
                                        const Point2& edge_end) {
    std::vector<ClipVertex> output;
    if (polygon.empty()) {
        return output;
    }
    output.reserve(polygon.size() + 1U);
    ClipVertex previous = polygon.back();
    double previous_distance = signedDistance(edge_start, edge_end, previous.projected);
    bool previous_inside = previous_distance >= -ClipTolerances::kPlaneEpsilonMeters;
    for (const auto& current : polygon) {
        const double current_distance = signedDistance(
                edge_start, edge_end, current.projected);
        const bool current_inside =
                current_distance >= -ClipTolerances::kPlaneEpsilonMeters;
        if (previous_inside != current_inside) {
            const double denominator = previous_distance - current_distance;
            if (std::abs(denominator) > 1.0e-15) {
                output.push_back(interpolate(previous, current,
                                             previous_distance / denominator));
            }
        }
        if (current_inside) {
            output.push_back(current);
        }
        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }
    return output;
}

double triangleArea(const ClipVertex& first, const ClipVertex& second,
                    const ClipVertex& third) {
    const std::array<double, 3> first_edge{
            second.projected.x - first.projected.x,
            second.projected.y - first.projected.y,
            second.projected_height - first.projected_height};
    const std::array<double, 3> second_edge{
            third.projected.x - first.projected.x,
            third.projected.y - first.projected.y,
            third.projected_height - first.projected_height};
    const std::array<double, 3> area_vector{
            first_edge[1] * second_edge[2] - first_edge[2] * second_edge[1],
            first_edge[2] * second_edge[0] - first_edge[0] * second_edge[2],
            first_edge[0] * second_edge[1] - first_edge[1] * second_edge[0]};
    return std::sqrt(area_vector[0] * area_vector[0]
                     + area_vector[1] * area_vector[1]
                     + area_vector[2] * area_vector[2]) * 0.5;
}

ScopeTriangle counterClockwise(ScopeTriangle triangle) {
    if (cross(triangle[0], triangle[1], triangle[2]) < 0.0) {
        std::swap(triangle[1], triangle[2]);
    }
    return triangle;
}

}  // namespace

std::vector<ClippedTriangle> TriangleClipper::clip(
        const ClippedTriangle& triangle,
        const std::vector<ScopeTriangle>& authorized_triangles) {
    if (triangle[0].attributes.size() != triangle[1].attributes.size()
        || triangle[0].attributes.size() != triangle[2].attributes.size()) {
        throw std::invalid_argument("Triangle attributes use inconsistent layouts");
    }
    std::vector<ClippedTriangle> result;
    for (const auto& source_scope_triangle : authorized_triangles) {
        const auto scope_triangle = counterClockwise(source_scope_triangle);
        std::vector<ClipVertex> polygon(triangle.begin(), triangle.end());
        for (std::size_t edge = 0; edge < scope_triangle.size(); ++edge) {
            polygon = clipAgainstEdge(polygon, scope_triangle[edge],
                                      scope_triangle[(edge + 1U)
                                                     % scope_triangle.size()]);
            if (polygon.size() < 3U) {
                break;
            }
        }
        if (polygon.size() < 3U) {
            continue;
        }
        for (std::size_t index = 1U; index + 1U < polygon.size(); ++index) {
            ClippedTriangle fragment{polygon[0], polygon[index], polygon[index + 1U]};
            if (triangleArea(fragment[0], fragment[1], fragment[2])
                > ClipTolerances::kMinimumTriangleAreaSquareMeters) {
                result.push_back(std::move(fragment));
            }
        }
    }
    return result;
}

}  // namespace clip_worker::geometry
