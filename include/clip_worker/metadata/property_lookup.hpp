#pragma once

#include "clip_worker/metadata/feature_metadata.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace clip_worker::metadata {

/**
 * Returns deterministic property material for one pickable feature row.
 * Null, out-of-range, or table-less identities deliberately do not resolve.
 * Legacy hierarchy ancestors are included and ambiguous property overloads
 * fail closed instead of selecting an arbitrary parent.
 */
[[nodiscard]] std::optional<std::string> lookupPropertyMaterial(
        const FeatureMetadata& metadata,
        std::optional<std::uint32_t> property_table,
        std::uint32_t feature_id,
        std::optional<std::uint32_t> null_feature_id = std::nullopt);

}  // namespace clip_worker::metadata
