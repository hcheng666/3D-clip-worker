#pragma once

#include "clip_worker/clip/instance_bounds_classifier.hpp"
#include "clip_worker/geometry/authorization_scope.hpp"
#include "clip_worker/geometry/matrix4.hpp"
#include "clip_worker/instance/instance_scene.hpp"
#include "clip_worker/mesh/mesh_scene.hpp"
#include "clip_worker/point/point_scene.hpp"

#include <cstdint>
#include <string>

namespace clip_worker::clip {

enum class AuthorizationContentRelation { empty, safe_whole, boundary };

struct ExactClassifierSummary {
    AuthorizationContentRelation relation =
            AuthorizationContentRelation::empty;
    std::string classifier_name;
    std::string classifier_version;
    std::uint64_t input_element_count = 0U;
    std::uint64_t whole_element_count = 0U;
    std::uint64_t disjoint_element_count = 0U;
    std::uint64_t boundary_element_count = 0U;
};

/** Exact triangle-union coverage classifier; bounds are never sufficient. */
class MeshAuthorizationClassifier final {
public:
    [[nodiscard]] static ExactClassifierSummary classify(
            const mesh::MeshScene& scene,
            const geometry::Matrix4& tile_to_ecef,
            const geometry::AuthorizationScope& authorization);
};

/** Exhaustive transformed point-membership classifier. */
class PointAuthorizationClassifier final {
public:
    [[nodiscard]] static ExactClassifierSummary classify(
            const point::PointScene& scene,
            const geometry::Matrix4& tile_to_ecef,
            const geometry::AuthorizationScope& authorization);
};

/** Conservative model-hull classifier shared with boundary expansion. */
class InstanceAuthorizationClassifier final {
public:
    [[nodiscard]] static ExactClassifierSummary classify(
            const instance::InstanceScene& scene,
            const geometry::Matrix4& tile_to_ecef,
            const geometry::AuthorizationScope& authorization);
};

[[nodiscard]] const char* authorizationContentRelationName(
        AuthorizationContentRelation relation) noexcept;

}  // namespace clip_worker::clip
