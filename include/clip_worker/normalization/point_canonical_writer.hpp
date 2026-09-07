#pragma once

#include "clip_worker/point/point_scene.hpp"

#include <cstdint>
#include <vector>

namespace clip_worker::normalization {

struct PointCanonicalWriteResult {
    std::vector<std::uint8_t> glb;
    std::uint64_t point_count = 0U;
    std::uint64_t feature_count = 0U;
};

/** Deterministic, self-contained glTF 2.0 POINTS writer. */
class PointCanonicalWriter final {
public:
    [[nodiscard]] static PointCanonicalWriteResult write(
            point::PointScene scene);
};

}  // namespace clip_worker::normalization
