#include "clip_worker/clip/instance_bounds_classifier.hpp"

#include "clip_worker/geometry/clip_geometry.hpp"
#include "clip_worker/mesh/mesh_scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace clip_worker::clip {
namespace {

double cross(const geometry::Point2& origin,
             const geometry::Point2& first,
             const geometry::Point2& second) {
    return (first.x - origin.x) * (second.y - origin.y)
           - (first.y - origin.y) * (second.x - origin.x);
}

std::vector<geometry::Point2> convexHull(std::vector<geometry::Point2> points) {
    std::sort(points.begin(), points.end(), [](const auto& left, const auto& right) {
        return left.x < right.x || (left.x == right.x && left.y < right.y);
    });
    points.erase(std::unique(points.begin(), points.end(),
            [](const auto& left, const auto& right) {
                return left.x == right.x && left.y == right.y;
            }), points.end());
    if (points.size() <= 2U) return points;
    std::vector<geometry::Point2> hull;
    hull.reserve(points.size() * 2U);
    for (const auto& point : points) {
        while (hull.size() >= 2U
               && cross(hull[hull.size() - 2U], hull.back(), point) <= 0.0) {
            hull.pop_back();
        }
        hull.push_back(point);
    }
    const std::size_t lower_size = hull.size();
    for (auto iterator = points.rbegin() + 1U; iterator != points.rend(); ++iterator) {
        while (hull.size() > lower_size
               && cross(hull[hull.size() - 2U], hull.back(), *iterator) <= 0.0) {
            hull.pop_back();
        }
        hull.push_back(*iterator);
    }
    hull.pop_back();
    return hull;
}

double area(const geometry::ClippedTriangle& triangle) {
    return std::abs(cross(triangle[0].projected,
                          triangle[1].projected,
                          triangle[2].projected)) * 0.5;
}

geometry::Matrix4 instanceTransform(const mesh::MeshNodeInstancing& instances,
                                    std::size_t index) {
    return geometry::Matrix4::translation({
                   instances.translations[index * 3U],
                   instances.translations[index * 3U + 1U],
                   instances.translations[index * 3U + 2U]})
            * geometry::Matrix4::quaternion({
                   instances.rotations[index * 4U],
                   instances.rotations[index * 4U + 1U],
                   instances.rotations[index * 4U + 2U],
                   instances.rotations[index * 4U + 3U]})
            * geometry::Matrix4::scale({
                   instances.scales[index * 3U],
                   instances.scales[index * 3U + 1U],
                   instances.scales[index * 3U + 2U]});
}

InstanceBoundsRelation classifyHull(
        const std::vector<geometry::Point2>& hull,
        const geometry::AuthorizationScope& authorization) {
    geometry::AuthorizationTriangleIndex::QueryWorkspace workspace;
    if (hull.empty()) return InstanceBoundsRelation::disjoint;
    if (hull.size() < 3U) {
        const bool all_inside = std::all_of(
                hull.begin(), hull.end(),
                [&authorization, &workspace](const geometry::Point2& point) {
                    return authorization.coversProjected(point, workspace);
                });
        return all_inside ? InstanceBoundsRelation::whole
                          : InstanceBoundsRelation::boundary;
    }
    double source_area = 0.0;
    double retained_area = 0.0;
    for (std::size_t index = 1U; index + 1U < hull.size(); ++index) {
        geometry::ClippedTriangle triangle{};
        triangle[0].projected = hull[0U];
        triangle[1].projected = hull[index];
        triangle[2].projected = hull[index + 1U];
        const double triangle_area = area(triangle);
        source_area += triangle_area;
        authorization.queryTriangles(triangle, workspace);
        for (const auto& clipped : geometry::TriangleClipper::clip(
                     triangle, workspace.triangles)) {
            retained_area += area(clipped);
        }
    }
    const double tolerance = std::max(
            geometry::ClipTolerances::kMinimumTriangleAreaSquareMeters,
            source_area * 1.0e-10);
    if (retained_area <= tolerance) return InstanceBoundsRelation::disjoint;
    if (retained_area + tolerance >= source_area) return InstanceBoundsRelation::whole;
    return InstanceBoundsRelation::boundary;
}

}  // namespace

std::vector<InstanceBoundsRelation> InstanceBoundsClassifier::classify(
        const instance::InstanceScene& scene,
        const geometry::Matrix4& tile_to_ecef,
        const geometry::AuthorizationScope& authorization) {
    const auto& model = scene.model.meshes.at(0U);
    const auto& bounds = model.bounds;
    const auto& instances = *scene.model.nodes.front().instancing;
    std::vector<InstanceBoundsRelation> result;
    result.reserve(instances.instanceCount());
    const geometry::Matrix4 content_to_ecef = tile_to_ecef
            * mesh::upAxisToZTransform(mesh::UpAxis::y)
            * scene.root_transform;
    for (std::size_t index = 0U; index < instances.instanceCount(); ++index) {
        const geometry::Matrix4 world = content_to_ecef
                * instanceTransform(instances, index);
        std::vector<geometry::Point2> projected;
        projected.reserve(8U);
        for (std::uint32_t corner = 0U; corner < 8U; ++corner) {
            const std::array<double, 3U> local{
                    (corner & 1U) == 0U ? bounds.minimum[0U] : bounds.maximum[0U],
                    (corner & 2U) == 0U ? bounds.minimum[1U] : bounds.maximum[1U],
                    (corner & 4U) == 0U ? bounds.minimum[2U] : bounds.maximum[2U]};
            projected.push_back(authorization.projectEcef(
                    world.transformPoint(local)).horizontal);
        }
        result.push_back(classifyHull(convexHull(std::move(projected)),
                                      authorization));
    }
    return result;
}

}  // namespace clip_worker::clip
