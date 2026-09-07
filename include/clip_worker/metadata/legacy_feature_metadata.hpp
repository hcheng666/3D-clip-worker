#pragma once

#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/metadata/feature_metadata.hpp"

#include <cstdint>
#include <string>

namespace clip_worker::metadata {

/**
 * Reads legacy Batch Table JSON/binary while preserving exact scalar types.
 * The supported hierarchy extension is converted to the shared bounded DAG.
 */
[[nodiscard]] FeatureMetadata readLegacyFeatureMetadata(
        const std::string& json_text, formats::ByteView binary,
        std::uint32_t feature_count, const MetadataResourceLimits& limits);

/** Compacts the primary feature table and its complete hierarchy ancestry. */
[[nodiscard]] FeatureMetadata compactLegacyFeatureMetadata(
        const FeatureMetadata& source,
        const std::vector<std::uint32_t>& retained_source_feature_ids,
        const MetadataResourceLimits& limits);

}  // namespace clip_worker::metadata
