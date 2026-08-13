#include "clip_worker/geometry/authorization_triangle_index.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace clip_worker::geometry {
namespace {

constexpr std::size_t kLeafEntryLimit = 8U;
constexpr double kParallelLineThreshold = 1.0e-12;

double cross(const Point2& first, const Point2& second, const Point2& value) {
    return (second.x - first.x) * (value.y - first.y)
           - (second.y - first.y) * (value.x - first.x);
}

ScopeTriangle counterClockwise(ScopeTriangle triangle) {
    if (cross(triangle[0], triangle[1], triangle[2]) < 0.0) {
        std::swap(triangle[1], triangle[2]);
    }
    return triangle;
}

AuthorizationTriangleIndex::Bounds infiniteBounds() {
    const double infinity = std::numeric_limits<double>::infinity();
    return {-infinity, -infinity, infinity, infinity};
}

void includePoint(AuthorizationTriangleIndex::Bounds& bounds,
                  const Point2& point) {
    bounds.minimum_x = std::min(bounds.minimum_x, point.x);
    bounds.minimum_y = std::min(bounds.minimum_y, point.y);
    bounds.maximum_x = std::max(bounds.maximum_x, point.x);
    bounds.maximum_y = std::max(bounds.maximum_y, point.y);
}

AuthorizationTriangleIndex::Bounds sourceBounds(const ClippedTriangle& source) {
    AuthorizationTriangleIndex::Bounds bounds{
            source[0].projected.x, source[0].projected.y,
            source[0].projected.x, source[0].projected.y};
    includePoint(bounds, source[1].projected);
    includePoint(bounds, source[2].projected);
    return bounds;
}

AuthorizationTriangleIndex::Bounds clippingBounds(
        const ScopeTriangle& source_triangle) {
    const ScopeTriangle triangle = counterClockwise(source_triangle);
    std::array<Point2, 3> normals{};
    std::array<double, 3> offsets{};
    for (std::size_t edge = 0U; edge < triangle.size(); ++edge) {
        const Point2& first = triangle[edge];
        const Point2& second = triangle[(edge + 1U) % triangle.size()];
        const double delta_x = second.x - first.x;
        const double delta_y = second.y - first.y;
        const double length = std::hypot(delta_x, delta_y);
        if (!(length > 0.0) || !std::isfinite(length)) {
            return infiniteBounds();
        }
        normals[edge] = {-delta_y / length, delta_x / length};
        offsets[edge] = normals[edge].x * first.x + normals[edge].y * first.y
                        - ClipTolerances::kPlaneEpsilonMeters;
    }

    AuthorizationTriangleIndex::Bounds bounds{
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()};
    for (std::size_t edge = 0U; edge < triangle.size(); ++edge) {
        const std::size_t previous = (edge + triangle.size() - 1U) % triangle.size();
        const double determinant = normals[previous].x * normals[edge].y
                                   - normals[previous].y * normals[edge].x;
        if (std::abs(determinant) <= kParallelLineThreshold) {
            return infiniteBounds();
        }
        const Point2 expanded{
                (offsets[previous] * normals[edge].y
                 - normals[previous].y * offsets[edge]) / determinant,
                (normals[previous].x * offsets[edge]
                 - offsets[previous] * normals[edge].x) / determinant};
        if (!std::isfinite(expanded.x) || !std::isfinite(expanded.y)) {
            return infiniteBounds();
        }
        includePoint(bounds, expanded);
    }

    const double infinity = std::numeric_limits<double>::infinity();
    bounds.minimum_x = std::nextafter(bounds.minimum_x, -infinity);
    bounds.minimum_y = std::nextafter(bounds.minimum_y, -infinity);
    bounds.maximum_x = std::nextafter(bounds.maximum_x, infinity);
    bounds.maximum_y = std::nextafter(bounds.maximum_y, infinity);
    return bounds;
}

AuthorizationTriangleIndex::Bounds combine(
        const AuthorizationTriangleIndex::Bounds& first,
        const AuthorizationTriangleIndex::Bounds& second) {
    return {std::min(first.minimum_x, second.minimum_x),
            std::min(first.minimum_y, second.minimum_y),
            std::max(first.maximum_x, second.maximum_x),
            std::max(first.maximum_y, second.maximum_y)};
}

bool overlaps(const AuthorizationTriangleIndex::Bounds& first,
              const AuthorizationTriangleIndex::Bounds& second) {
    return first.minimum_x <= second.maximum_x
           && first.maximum_x >= second.minimum_x
           && first.minimum_y <= second.maximum_y
           && first.maximum_y >= second.minimum_y;
}

double center(const AuthorizationTriangleIndex::Bounds& bounds, bool x_axis) {
    const double minimum = x_axis ? bounds.minimum_x : bounds.minimum_y;
    const double maximum = x_axis ? bounds.maximum_x : bounds.maximum_y;
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        return 0.0;
    }
    return minimum + (maximum - minimum) * 0.5;
}

}  // namespace

AuthorizationTriangleIndex::AuthorizationTriangleIndex(
        std::vector<ScopeTriangle> triangles)
    : triangles_(std::move(triangles)) {
    if (triangles_.empty()) {
        throw std::invalid_argument("Authorization triangle index cannot be empty");
    }
    entries_.reserve(triangles_.size());
    for (std::size_t index = 0U; index < triangles_.size(); ++index) {
        entries_.push_back({clippingBounds(triangles_[index]), index});
    }
    nodes_.reserve(triangles_.size() * 2U);
    static_cast<void>(build(0U, entries_.size()));
}

const std::vector<ScopeTriangle>& AuthorizationTriangleIndex::triangles() const noexcept {
    return triangles_;
}

void AuthorizationTriangleIndex::query(
        const ClippedTriangle& source,
        QueryWorkspace& workspace) const {
    workspace.triangle_indices.clear();
    workspace.triangles.clear();
    queryNode(0U, sourceBounds(source), workspace.triangle_indices);
    std::sort(workspace.triangle_indices.begin(), workspace.triangle_indices.end());
    workspace.triangles.reserve(workspace.triangle_indices.size());
    for (const std::size_t index : workspace.triangle_indices) {
        workspace.triangles.push_back(triangles_.at(index));
    }
}

std::size_t AuthorizationTriangleIndex::build(std::size_t begin, std::size_t end) {
    const std::size_t node_index = nodes_.size();
    nodes_.push_back({});
    Bounds bounds = entries_.at(begin).bounds;
    for (std::size_t index = begin + 1U; index < end; ++index) {
        bounds = combine(bounds, entries_[index].bounds);
    }
    nodes_[node_index].bounds = bounds;
    nodes_[node_index].begin = begin;
    nodes_[node_index].end = end;
    if (end - begin <= kLeafEntryLimit) {
        return node_index;
    }

    const bool x_axis = bounds.maximum_x - bounds.minimum_x
                        >= bounds.maximum_y - bounds.minimum_y;
    std::stable_sort(entries_.begin() + static_cast<std::ptrdiff_t>(begin),
                     entries_.begin() + static_cast<std::ptrdiff_t>(end),
                     [x_axis](const Entry& first, const Entry& second) {
                         const double first_center = center(first.bounds, x_axis);
                         const double second_center = center(second.bounds, x_axis);
                         if (first_center == second_center) {
                             return first.triangle_index < second.triangle_index;
                         }
                         return first_center < second_center;
                     });
    const std::size_t middle = begin + (end - begin) / 2U;
    const std::size_t left = build(begin, middle);
    const std::size_t right = build(middle, end);
    nodes_[node_index].leaf = false;
    nodes_[node_index].left = left;
    nodes_[node_index].right = right;
    return node_index;
}

void AuthorizationTriangleIndex::queryNode(
        std::size_t node_index, const Bounds& bounds,
        std::vector<std::size_t>& candidate_indices) const {
    const Node& node = nodes_.at(node_index);
    if (!overlaps(node.bounds, bounds)) {
        return;
    }
    if (!node.leaf) {
        queryNode(node.left, bounds, candidate_indices);
        queryNode(node.right, bounds, candidate_indices);
        return;
    }
    for (std::size_t index = node.begin; index < node.end; ++index) {
        if (overlaps(entries_[index].bounds, bounds)) {
            candidate_indices.push_back(entries_[index].triangle_index);
        }
    }
}

}  // namespace clip_worker::geometry
