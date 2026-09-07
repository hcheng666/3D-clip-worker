#pragma once

#include "clip_worker/metadata/feature_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clip_worker::metadata {

struct MetadataBufferView {
    const std::vector<std::uint8_t>* bytes = nullptr;
    std::size_t absolute_offset = 0U;
};

/**
 * Reads the supported EXT_structural_metadata property-table subset. External
 * schema bytes must already be closed by the approved resource manifest.
 */
[[nodiscard]] FeatureMetadata readStructuralMetadata(
        const std::string& gltf_json,
        const std::vector<MetadataBufferView>& buffer_views,
        const MetadataResourceLimits& limits,
        const std::optional<std::string>& external_schema_json = std::nullopt);

}  // namespace clip_worker::metadata
