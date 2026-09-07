#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::tests {

[[nodiscard]] std::vector<std::uint8_t> makeEmbeddedI3dmFixture(
        const std::vector<std::uint8_t>& model_glb);
[[nodiscard]] std::vector<std::uint8_t> makeExternalI3dmFixture(
        const std::string& model_uri);
[[nodiscard]] std::vector<std::uint8_t> makeEnuOnlyI3dmFixture(
        const std::vector<std::uint8_t>& model_glb);
[[nodiscard]] std::vector<std::uint8_t> makeOctOrientedI3dmFixture(
        const std::vector<std::uint8_t>& model_glb);
[[nodiscard]] std::vector<std::uint8_t> makePerInstanceMetadataI3dmFixture(
        const std::vector<std::uint8_t>& model_glb);

}  // namespace clip_worker::tests
