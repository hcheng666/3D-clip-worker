#pragma once

#include <cstdint>
#include <vector>

namespace clip_worker::tests {

[[nodiscard]] std::vector<std::uint8_t> makeCompositeFixture(
        const std::vector<std::uint8_t>& glb,
        const std::vector<std::uint8_t>& pnts);

}  // namespace clip_worker::tests
