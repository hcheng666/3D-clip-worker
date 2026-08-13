#pragma once

#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/task/task_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace clip_worker::clip {

struct B3dmClipResult {
    bool empty = false;
    std::vector<std::uint8_t> bytes;
    task::CompleteStatistics statistics;
    formats::B3dmLayoutDiagnostics source_layout;
};

/** Aggregated diagnostics for accepted producer-specific sampler fields. */
struct SamplerCompatibilityDiagnostics {
    std::size_t affected_sampler_count = 0U;
    std::vector<std::uint32_t> wrap_r_values;

    [[nodiscard]] bool requiresCompatibility() const noexcept {
        return affected_sampler_count > 0U;
    }
};

/** 当前一期格式范围内的严格B3DM/GLB Mesh与WebP裁切重建器。 */
class B3dmClipper final {
public:
    using SourceLayoutObserver = std::function<void(
            const formats::B3dmLayoutDiagnostics&)>;
    using SamplerCompatibilityObserver = std::function<void(
            const SamplerCompatibilityDiagnostics&)>;

    /** The observer runs after source parsing and before semantic/geometry validation. */
    [[nodiscard]] static B3dmClipResult clip(
            const std::vector<std::uint8_t>& source,
            const task::ClaimTask& task,
            const SourceLayoutObserver& source_layout_observer = {},
            const SamplerCompatibilityObserver& sampler_compatibility_observer = {});
};

}  // namespace clip_worker::clip
