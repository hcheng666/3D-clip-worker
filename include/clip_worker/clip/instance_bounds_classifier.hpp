#pragma once

#include "clip_worker/geometry/authorization_scope.hpp"
#include "clip_worker/geometry/matrix4.hpp"
#include "clip_worker/instance/instance_scene.hpp"

#include <vector>

namespace clip_worker::clip {

enum class InstanceBoundsRelation { whole, disjoint, boundary };

/** Conservative projected convex-hull classifier for transformed model AABBs. */
class InstanceBoundsClassifier final {
public:
    [[nodiscard]] static std::vector<InstanceBoundsRelation> classify(
            const instance::InstanceScene& scene,
            const geometry::Matrix4& tile_to_ecef,
            const geometry::AuthorizationScope& authorization);
};

}  // namespace clip_worker::clip
