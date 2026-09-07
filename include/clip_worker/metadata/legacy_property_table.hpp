#pragma once

#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/mesh/mesh_scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::metadata {

/** Named resource limits shared by legacy Batch Table readers. */
struct LegacyPropertyLimits {
    std::uint64_t maximum_feature_rows = 10000000ULL;
    std::uint64_t maximum_properties = 256ULL;
    std::uint64_t maximum_binary_bytes = 1073741824ULL;
    std::uint64_t maximum_string_bytes = 268435456ULL;
};

/**
 * Reads the supported legacy JSON scalar/string/boolean and numeric binary
 * property subset. Unsupported complex metadata fails closed.
 */
[[nodiscard]] mesh::LegacyPropertyTable readLegacyPropertyTable(
        const std::string& json_text, formats::ByteView binary,
        std::uint32_t feature_count,
        const LegacyPropertyLimits& limits = {});

/** Dense row compaction used by both point and instance clipping. */
[[nodiscard]] mesh::LegacyPropertyTable compactLegacyPropertyTable(
        const mesh::LegacyPropertyTable& source,
        const std::vector<std::uint32_t>& retained_source_ids);

}  // namespace clip_worker::metadata
