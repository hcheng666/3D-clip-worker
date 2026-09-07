#include "support/synthetic_cmpt.hpp"

namespace clip_worker::tests {
namespace {

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        bytes.push_back(static_cast<std::uint8_t>(
                (value >> (byte * 8U)) & 0xffU));
    }
}

std::vector<std::uint8_t> unsupportedChild() {
    std::vector<std::uint8_t> bytes{'z', 'z', 'z', 'z'};
    appendU32(bytes, 1U);
    appendU32(bytes, 12U);
    return bytes;
}

std::vector<std::uint8_t> composite(
        const std::vector<std::vector<std::uint8_t>>& children) {
    std::vector<std::uint8_t> bytes{'c', 'm', 'p', 't'};
    appendU32(bytes, 1U);
    appendU32(bytes, 0U);
    appendU32(bytes, static_cast<std::uint32_t>(children.size()));
    for (const auto& child : children) {
        bytes.insert(bytes.end(), child.begin(), child.end());
    }
    const std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        bytes[8U + byte] = static_cast<std::uint8_t>(
                (length >> (byte * 8U)) & 0xffU);
    }
    return bytes;
}

}  // namespace

std::vector<std::uint8_t> makeCompositeFixture(
        const std::vector<std::uint8_t>& glb,
        const std::vector<std::uint8_t>& pnts) {
    return composite({glb, composite({pnts, unsupportedChild()})});
}

}  // namespace clip_worker::tests
