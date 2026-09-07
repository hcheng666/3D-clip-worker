#pragma once

#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/gltf_mesh_reader.hpp"
#include "clip_worker/instance/instance_scene.hpp"
#include "clip_worker/metadata/legacy_property_table.hpp"
#include "clip_worker/metadata/metadata_resource_limits.hpp"

#include <cstdint>
#include <string>

namespace clip_worker::normalization {

struct InstanceResourceLimits {
    std::uint64_t maximum_instances = 250000ULL;
    std::uint64_t maximum_decoded_bytes = 1073741824ULL;
    formats::GltfMeshReaderLimits model;
    metadata::LegacyPropertyLimits metadata;
    bool enable_feature_metadata = false;
    metadata::MetadataResourceLimits feature_metadata;
};

struct InstanceNormalizationInput {
    std::string root_package_relative_path;
    std::vector<std::uint8_t> source_bytes;
    formats::ApprovedGltfResourceMap approved_resources;
};

struct InstanceNormalizationResult {
    instance::InstanceScene scene;
    formats::GltfMeshReadDiagnostics model_diagnostics;
    bool external_model = false;
};

/** Strict I3DM decoder and deterministic shared-model flattener. */
class InstanceNormalizer final {
public:
    explicit InstanceNormalizer(InstanceResourceLimits limits = {});

    [[nodiscard]] InstanceNormalizationResult normalize(
            const InstanceNormalizationInput& input) const;

private:
    InstanceResourceLimits limits_;
};

}  // namespace clip_worker::normalization
