#pragma once

#include "clip_worker/geometry/clip_geometry.hpp"

#include <cstddef>
#include <vector>

namespace clip_worker::geometry {

/** 授权三角形的确定性保守AABB层次索引。 */
class AuthorizationTriangleIndex final {
public:
    struct Bounds {
        double minimum_x = 0.0;
        double minimum_y = 0.0;
        double maximum_x = 0.0;
        double maximum_y = 0.0;
    };

    /** Reusable query buffers avoid per-model-triangle heap allocation. */
    struct QueryWorkspace {
        std::vector<std::size_t> triangle_indices;
        std::vector<ScopeTriangle> triangles;
    };

    explicit AuthorizationTriangleIndex(std::vector<ScopeTriangle> triangles);

    [[nodiscard]] const std::vector<ScopeTriangle>& triangles() const noexcept;

    /** Returns conservative candidates in original triangulation order. */
    void query(const ClippedTriangle& source, QueryWorkspace& workspace) const;

private:
    struct Entry {
        Bounds bounds;
        std::size_t triangle_index = 0U;
    };

    struct Node {
        Bounds bounds;
        std::size_t begin = 0U;
        std::size_t end = 0U;
        std::size_t left = 0U;
        std::size_t right = 0U;
        bool leaf = true;
    };

    [[nodiscard]] std::size_t build(std::size_t begin, std::size_t end);
    void queryNode(std::size_t node_index, const Bounds& bounds,
                   std::vector<std::size_t>& candidate_indices) const;

    std::vector<ScopeTriangle> triangles_;
    std::vector<Entry> entries_;
    std::vector<Node> nodes_;
};

}  // namespace clip_worker::geometry
