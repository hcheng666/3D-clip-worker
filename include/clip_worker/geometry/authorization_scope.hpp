#pragma once

#include "clip_worker/geometry/clip_geometry.hpp"
#include "clip_worker/geometry/authorization_triangle_index.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace clip_worker::geometry {

struct ProjectedPoint {
    Point2 horizontal;
    double height = 0.0;
};

/** EPSG:4490 Polygon/MultiPolygon授权范围及其局部米制三角化。 */
class AuthorizationScope final {
public:
    [[nodiscard]] static AuthorizationScope fromWkb(
            const std::vector<std::uint8_t>& wkb, std::int32_t srid);
    [[nodiscard]] static AuthorizationScope fromBase64Wkb(
            const std::string& base64_wkb, std::int32_t srid);

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

private:
    class Impl;

    AuthorizationScope(std::unique_ptr<Impl> impl,
                       std::vector<ScopeTriangle> triangles);

    std::unique_ptr<Impl> impl_;
    AuthorizationTriangleIndex triangle_index_;
};

}  // namespace clip_worker::geometry
