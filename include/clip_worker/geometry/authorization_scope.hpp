#pragma once

#include "clip_worker/geometry/clip_geometry.hpp"
#include "clip_worker/geometry/authorization_triangle_index.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace clip_worker::geometry {

/** Strict RFC 4648 decoder shared by exact clipping request envelopes. */
[[nodiscard]] std::vector<std::uint8_t> decodeAuthorizationWkbBase64(
        const std::string& base64_wkb);

/** Profile-backed limits for WKB parsing, densification, and triangulation. */
struct AuthorizationScopeLimits {
    std::size_t maximum_polygons = 100000U;
    std::size_t maximum_rings = 100000U;
    std::size_t maximum_points = 2000000U;
    std::size_t maximum_densified_points_per_segment = 100000U;
    std::size_t maximum_triangles = 4000000U;
    double densify_segment_meters = ClipTolerances::kScopeDensifySegmentMeters;
};

struct ProjectedPoint {
    Point2 horizontal;
    double height = 0.0;
};

/** EPSG:4490 Polygon/MultiPolygon授权范围及其局部米制三角化。 */
class AuthorizationScope final {
public:
    [[nodiscard]] static AuthorizationScope fromWkb(
            const std::vector<std::uint8_t>& wkb, std::int32_t srid,
            const AuthorizationScopeLimits& limits = {});
    [[nodiscard]] static AuthorizationScope fromBase64Wkb(
            const std::string& base64_wkb, std::int32_t srid,
            const AuthorizationScopeLimits& limits = {});

    AuthorizationScope(AuthorizationScope&&) noexcept;
    AuthorizationScope& operator=(AuthorizationScope&&) noexcept;
    ~AuthorizationScope();

    AuthorizationScope(const AuthorizationScope&) = delete;
    AuthorizationScope& operator=(const AuthorizationScope&) = delete;

    [[nodiscard]] const std::vector<ScopeTriangle>& triangles() const noexcept;
    void queryTriangles(
            const ClippedTriangle& source,
            AuthorizationTriangleIndex::QueryWorkspace& workspace) const;
    [[nodiscard]] ProjectedPoint projectEcef(
            const std::array<double, 3>& ecef) const;
    /** Exact Polygon/MultiPolygon covers semantics; boundary points are retained. */
    [[nodiscard]] bool coversEcef(
            const std::array<double, 3>& ecef,
            AuthorizationTriangleIndex::QueryWorkspace& workspace) const;
    [[nodiscard]] bool coversProjected(
            const Point2& projected,
            AuthorizationTriangleIndex::QueryWorkspace& workspace) const;

private:
    class Impl;

    AuthorizationScope(std::unique_ptr<Impl> impl,
                       std::vector<ScopeTriangle> triangles);

    std::unique_ptr<Impl> impl_;
    AuthorizationTriangleIndex triangle_index_;
};

}  // namespace clip_worker::geometry
