#pragma once

#include "clip_worker/clip/canonical_mesh_clip_strategy.hpp"
#include "clip_worker/clip/instance_bounds_classifier.hpp"
#include "clip_worker/instance/instance_scene.hpp"
#include "clip_worker/normalization/instance_canonical_writer.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace clip_worker::clip {

struct InstanceExpansionLimits {
    std::uint64_t maximum_expanded_vertices = 5000000ULL;
    std::uint64_t maximum_expanded_indices = 6000000ULL;
    std::uint64_t maximum_boundary_instances = 250000ULL;
};

struct CanonicalInstanceClipResult {
    std::optional<normalization::InstanceCanonicalWriteResult> whole_instances;
    std::optional<CanonicalMeshClipResult> boundary_mesh;
    std::vector<InstanceBoundsRelation> relations;
};

/** Conservative whole/drop/expand strategy with ordered typed outputs. */
class CanonicalInstanceClipStrategy final {
public:
    [[nodiscard]] static CanonicalInstanceClipResult clip(
            instance::InstanceScene source,
            MeshSceneClipRequest request,
            const normalization::MeshResourceProfile& mesh_profile,
            const normalization::ToolVersion& instance_validator,
            const normalization::ToolVersion& mesh_validator,
            const InstanceExpansionLimits& limits = {});
};

}  // namespace clip_worker::clip
