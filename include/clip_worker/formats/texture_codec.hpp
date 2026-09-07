#pragma once

#include "clip_worker/mesh/mesh_scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::formats {

inline constexpr const char* kCanonicalTexturePolicyVersion =
        "canonical-png-rgba8-v1";

enum class TextureFormat {
    png,
    jpeg,
    webp,
    ktx2_basis,
};

struct TextureDecodeLimits {
    std::uint32_t maximum_width = 8192U;
    std::uint32_t maximum_height = 8192U;
    std::uint64_t maximum_pixels = 67108864ULL;
    std::uint64_t maximum_rgba_bytes = 268435456ULL;
};

/** Bounded texture decoder and deterministic canonical PNG encoder. */
class TextureCodec final {
public:
    [[nodiscard]] static TextureFormat formatForMimeType(
            const std::string& mime_type);
    [[nodiscard]] static mesh::RgbaImage decode(
            const std::vector<std::uint8_t>& encoded,
            TextureFormat format,
            const TextureDecodeLimits& limits = {});
    [[nodiscard]] static std::vector<std::uint8_t> encodeDeterministicPng(
            const mesh::RgbaImage& image);
};

}  // namespace clip_worker::formats
