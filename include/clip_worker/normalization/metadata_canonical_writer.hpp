#pragma once

#include "clip_worker/metadata/feature_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {

inline constexpr const char* kCanonicalLegacyHierarchyExtension =
        "JUSTAI_legacy_batch_table_hierarchy";

using CanonicalBufferViewWriter = std::function<std::size_t(
        const std::vector<std::uint8_t>&, std::size_t)>;

struct CanonicalMetadataJson {
    nlohmann::json structural_metadata;
    std::optional<nlohmann::json> legacy_hierarchy;
};

struct CanonicalFeatureIdBytes {
    std::uint32_t component_type = 0U;
    std::vector<std::uint8_t> bytes;
};

/** Serializes only retained typed metadata into deterministic GLB structures. */
[[nodiscard]] CanonicalMetadataJson writeCanonicalMetadata(
        const metadata::FeatureMetadata& metadata,
        const CanonicalBufferViewWriter& add_buffer_view);

/** Encodes a dense feature stream using legal glTF vertex attribute types. */
[[nodiscard]] CanonicalFeatureIdBytes encodeCanonicalFeatureIds(
        const metadata::FeatureIdSet& feature_id_set);

}  // namespace clip_worker::normalization
