#include "clip_worker/instance/instance_scene.hpp"

#include "clip_worker/formats/format_error.hpp"

namespace clip_worker::instance {

std::size_t InstanceScene::instanceCount() const noexcept {
    if (model.nodes.empty() || !model.nodes.front().instancing.has_value()) return 0U;
    return model.nodes.front().instancing->instanceCount();
}

void validateInstanceScene(InstanceScene& scene) {
    mesh::validateMeshScene(scene.model);
    if (scene.model.scenes.size() != 1U || scene.model.default_scene != 0U
        || scene.model.scenes.front() != std::vector<std::size_t>{0U}
        || scene.model.nodes.size() != 1U
        || !scene.model.nodes.front().mesh.has_value()
        || *scene.model.nodes.front().mesh != 0U
        || !scene.model.nodes.front().children.empty()
        || !scene.model.nodes.front().instancing.has_value()) {
        throw formats::FormatError(
                formats::FormatErrorCode::i3dm_semantic_unsupported,
                "Instance model is not a single flattened mesh node");
    }
}

mesh::MeshScene toInstancedMeshScene(InstanceScene scene) {
    validateInstanceScene(scene);
    scene.model.nodes.front().local_transform = scene.root_transform;
    return std::move(scene.model);
}

}  // namespace clip_worker::instance
