#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::tests {

struct SyntheticPnts {
    std::vector<std::uint8_t> bytes;
    std::string feature_json;
};

[[nodiscard]] SyntheticPnts makeQuantizedPntsFixture();
[[nodiscard]] SyntheticPnts makePerPointPropertyPntsFixture();

}  // namespace clip_worker::tests
