#include "clip_worker/clip/authorization_content_classifier.hpp"

#include "clip_worker/geometry/clip_geometry.hpp"
#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace clip_worker::clip {
namespace {

constexpr const char* kMeshClassifierName =
        "EXACT_TRIANGLE_UNION_COVERAGE";
constexpr const char* kMeshClassifierVersion =
        "exact-triangle-union-coverage-v1";
constexpr const char* kPointClassifierName =
        "EXHAUSTIVE_POINT_COVERS";
constexpr const char* kPointClassifierVersion =
        "exhaustive-point-covers-v1";
constexpr const char* kInstanceClassifierName =
        "MODEL_HULL_UNION_COVERAGE";
constexpr const char* kInstanceClassifierVersion =
        "model-hull-union-coverage-v1";

double triangleArea(const geometry::ClippedTriangle& triangle) {
    const std::array<double, 3U> first{
            triangle[1U].projected.x - triangle[0U].projected.x,
            triangle[1U].projected.y - triangle[0U].projected.y,
            triangle[1U].projected_height
                    - triangle[0U].projected_height};
    const std::array<double, 3U> second{
            triangle[2U].projected.x - triangle[0U].projected.x,
            triangle[2U].projected.y - triangle[0U].projected.y,
            triangle[2U].projected_height
                    - triangle[0U].projected_height};
    const std::array<double, 3U> cross{
            first[1U] * second[2U] - first[2U] * second[1U],
            first[2U] * second[0U] - first[0U] * second[2U],
            first[0U] * second[1U] - first[1U] * second[0U]};
    return std::sqrt(cross[0U] * cross[0U]
                     + cross[1U] * cross[1U]
                     + cross[2U] * cross[2U]) * 0.5;
}

geometry::ClipVertex projectedVertex(
        const mesh::MeshPrimitive& primitive, std::uint32_t index,
        const geometry::Matrix4& world,
        const geometry::AuthorizationScope& authorization) {
    const std::array<double, 3U> local{
            primitive.positions.at(index * 3U),
            primitive.positions.at(index * 3U + 1U),
            primitive.positions.at(index * 3U + 2U)};
    const auto projected = authorization.projectEcef(world.transformPoint(local));
    geometry::ClipVertex result;
    result.local_position = local;
    result.projected = projected.horizontal;
    result.projected_height = projected.height;
    return result;
}

AuthorizationContentRelation classifyTriangle(
        const geometry::ClippedTriangle& triangle,
        const geometry::AuthorizationScope& authorization,
        geometry::AuthorizationTriangleIndex::QueryWorkspace& workspace) {
    const double source_area = triangleArea(triangle);
    if (source_area <= geometry::ClipTolerances::kMinimumTriangleAreaSquareMeters) {
        throw formats::FormatError(
                formats::FormatErrorCode::clipping_output_invalid,
                "Canonical mesh contains a degenerate transformed triangle");
    }
    authorization.queryTriangles(triangle, workspace);
    double retained_area = 0.0;
    for (const auto& fragment : geometry::TriangleClipper::clip(
                 triangle, workspace.triangles)) {
        retained_area += triangleArea(fragment);
    }
    const double tolerance = std::max(
            geometry::ClipTolerances::kMinimumTriangleAreaSquareMeters,
            source_area * 1.0e-10);
    if (retained_area <= tolerance) return AuthorizationContentRelation::empty;
    if (retained_area + tolerance >= source_area) {
        return AuthorizationContentRelation::safe_whole;
    }
    return AuthorizationContentRelation::boundary;
}

void addRelation(ExactClassifierSummary& summary,
                 AuthorizationContentRelation relation) {
    ++summary.input_element_count;
    switch (relation) {
        case AuthorizationContentRelation::empty:
            ++summary.disjoint_element_count;
            break;
        case AuthorizationContentRelation::safe_whole:
            ++summary.whole_element_count;
            break;
        case AuthorizationContentRelation::boundary:
            ++summary.boundary_element_count;
            break;
    }
}

void finish(ExactClassifierSummary& summary) {
    if (summary.input_element_count == 0U) {
        throw formats::FormatError(
                formats::FormatErrorCode::clipping_output_invalid,
                "Canonical input has no classifiable geometry");
    }
    if (summary.disjoint_element_count == summary.input_element_count) {
        summary.relation = AuthorizationContentRelation::empty;
    } else if (summary.whole_element_count == summary.input_element_count) {
        summary.relation = AuthorizationContentRelation::safe_whole;
    } else {
        summary.relation = AuthorizationContentRelation::boundary;
    }
}

}  // namespace

ExactClassifierSummary MeshAuthorizationClassifier::classify(
        const mesh::MeshScene& scene,
        const geometry::Matrix4& tile_to_ecef,
        const geometry::AuthorizationScope& authorization) {
    mesh::MeshScene validated = scene;
    mesh::validateMeshScene(validated);
    ExactClassifierSummary summary;
    summary.classifier_name = kMeshClassifierName;
    summary.classifier_version = kMeshClassifierVersion;
    const geometry::Matrix4 content_to_ecef = tile_to_ecef
            * mesh::upAxisToZTransform(mesh::UpAxis::y);
    std::vector<std::uint8_t> state(scene.nodes.size(), 0U);
    geometry::AuthorizationTriangleIndex::QueryWorkspace workspace;
    std::function<void(std::size_t, const geometry::Matrix4&)> visit =
            [&](std::size_t node_index, const geometry::Matrix4& parent) {
        if (state.at(node_index) == 1U) {
            throw formats::FormatError(
                    formats::FormatErrorCode::clipping_output_invalid,
                    "Canonical mesh node graph contains a cycle");
        }
        if (state.at(node_index) == 2U) {
            throw formats::FormatError(
                    formats::FormatErrorCode::clipping_output_invalid,
                    "Canonical mesh node has multiple parents");
        }
        state.at(node_index) = 1U;
        const auto& node = scene.nodes.at(node_index);
        const geometry::Matrix4 accumulated = parent * node.local_transform;
        if (node.mesh.has_value()) {
            const geometry::Matrix4 world = content_to_ecef * accumulated;
            for (const auto& primitive
                    : scene.meshes.at(*node.mesh).primitives) {
                for (std::size_t offset = 0U;
                     offset < primitive.indices.size(); offset += 3U) {
                    const geometry::ClippedTriangle triangle{
                            projectedVertex(primitive,
                                            primitive.indices[offset],
                                            world, authorization),
                            projectedVertex(primitive,
                                            primitive.indices[offset + 1U],
                                            world, authorization),
                            projectedVertex(primitive,
                                            primitive.indices[offset + 2U],
                                            world, authorization)};
                    addRelation(summary, classifyTriangle(
                            triangle, authorization, workspace));
                }
            }
        }
        for (const std::size_t child : node.children) visit(child, accumulated);
        state.at(node_index) = 2U;
    };
    for (const std::size_t root : scene.scenes.at(scene.default_scene)) {
        visit(root, geometry::Matrix4::identity());
    }
    finish(summary);
    return summary;
}

ExactClassifierSummary PointAuthorizationClassifier::classify(
        const point::PointScene& scene,
        const geometry::Matrix4& tile_to_ecef,
        const geometry::AuthorizationScope& authorization) {
    point::PointScene validated = scene;
    point::validatePointScene(validated);
    ExactClassifierSummary summary;
    summary.classifier_name = kPointClassifierName;
    summary.classifier_version = kPointClassifierVersion;
    const geometry::Matrix4 canonical_to_ecef = tile_to_ecef
            * mesh::upAxisToZTransform(mesh::UpAxis::y)
            * scene.root_transform;
    geometry::AuthorizationTriangleIndex::QueryWorkspace workspace;
    for (std::size_t ordinal = 0U; ordinal < scene.pointCount(); ++ordinal) {
        const std::array<double, 3U> local{
                scene.positions[ordinal * 3U],
                scene.positions[ordinal * 3U + 1U],
                scene.positions[ordinal * 3U + 2U]};
        addRelation(summary,
                    authorization.coversEcef(
                            canonical_to_ecef.transformPoint(local), workspace)
                            ? AuthorizationContentRelation::safe_whole
                            : AuthorizationContentRelation::empty);
    }
    finish(summary);
    return summary;
}

ExactClassifierSummary InstanceAuthorizationClassifier::classify(
        const instance::InstanceScene& scene,
        const geometry::Matrix4& tile_to_ecef,
        const geometry::AuthorizationScope& authorization) {
    instance::InstanceScene validated = scene;
    instance::validateInstanceScene(validated);
    ExactClassifierSummary summary;
    summary.classifier_name = kInstanceClassifierName;
    summary.classifier_version = kInstanceClassifierVersion;
    for (const auto relation : InstanceBoundsClassifier::classify(
                 scene, tile_to_ecef, authorization)) {
        switch (relation) {
            case InstanceBoundsRelation::whole:
                addRelation(summary,
                            AuthorizationContentRelation::safe_whole);
                break;
            case InstanceBoundsRelation::disjoint:
                addRelation(summary, AuthorizationContentRelation::empty);
                break;
            case InstanceBoundsRelation::boundary:
                addRelation(summary,
                            AuthorizationContentRelation::boundary);
                break;
        }
    }
    finish(summary);
    return summary;
}

const char* authorizationContentRelationName(
        AuthorizationContentRelation relation) noexcept {
    switch (relation) {
        case AuthorizationContentRelation::empty: return "EMPTY";
        case AuthorizationContentRelation::safe_whole: return "SAFE_WHOLE";
        case AuthorizationContentRelation::boundary: return "BOUNDARY";
    }
    return "BOUNDARY";
}

}  // namespace clip_worker::clip
