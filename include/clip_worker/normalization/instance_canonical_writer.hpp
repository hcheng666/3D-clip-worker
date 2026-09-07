#pragma once

#include "clip_worker/instance/instance_scene.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"

namespace clip_worker::normalization {

struct InstanceCanonicalWriteResult {
    MeshCanonicalWriteResult canonical;
    CanonicalArtifactEvidence evidence;
    std::uint64_t instance_count = 0U;
};

/** Writes and validates canonical EXT_mesh_gpu_instancing output. */
class InstanceCanonicalWriter final {
public:
    [[nodiscard]] static InstanceCanonicalWriteResult write(
            instance::InstanceScene scene, const ToolVersion& validator);
};

}  // namespace clip_worker::normalization
