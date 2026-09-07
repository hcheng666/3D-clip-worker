#pragma once

#include "clip_worker/mesh/mesh_scene.hpp"

#include <cstdint>

namespace clip_worker::clip {

struct TextureMaskLimits {
    std::uint32_t maximum_repeat_span = 256U;
    std::uint64_t maximum_raster_tests = 1000000000ULL;
};

struct TextureMaskStatistics {
    std::uint64_t retained_pixels = 0U;
    std::uint64_t cleared_pixels = 0U;
    std::uint64_t raster_tests = 0U;
};

/** Clears all RGBA channels outside retained primitive UV coverage. */
class TextureMasker final {
public:
    [[nodiscard]] static TextureMaskStatistics mask(
            mesh::MeshScene& scene, const TextureMaskLimits& limits = {});
};

}  // namespace clip_worker::clip
