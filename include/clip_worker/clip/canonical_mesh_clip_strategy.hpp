#pragma once

#include "clip_worker/clip/mesh_scene_clipper.hpp"
#include "clip_worker/clip/texture_masker.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "clip_worker/normalization/mesh_resource_profile.hpp"

namespace clip_worker::clip {

struct CanonicalMeshClipResult {
    bool empty = false;
    MeshSceneClipStatistics geometry;
    TextureMaskStatistics texture;
    normalization::MeshCanonicalWriteResult canonical;
    normalization::CanonicalArtifactEvidence evidence;
};

/** Typed canonical Mesh strategy exposed for the later hierarchy integration. */
class CanonicalMeshClipStrategy final {
public:
    [[nodiscard]] static CanonicalMeshClipResult clip(
            mesh::MeshScene source,
            MeshSceneClipRequest request,
            const normalization::MeshResourceProfile& profile,
            const normalization::ToolVersion& validator);
};

}  // namespace clip_worker::clip
