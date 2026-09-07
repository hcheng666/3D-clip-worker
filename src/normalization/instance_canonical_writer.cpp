#include "clip_worker/normalization/instance_canonical_writer.hpp"

namespace clip_worker::normalization {

InstanceCanonicalWriteResult InstanceCanonicalWriter::write(
        instance::InstanceScene scene, const ToolVersion& validator) {
    InstanceCanonicalWriteResult result;
    result.instance_count = scene.instanceCount();
    result.canonical = MeshCanonicalWriter::write(
            instance::toInstancedMeshScene(std::move(scene)));
    result.evidence = validateCanonicalGlb(
            result.canonical.glb, CanonicalFamily::instance_gltf2, validator);
    return result;
}

}  // namespace clip_worker::normalization
