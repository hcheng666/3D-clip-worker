#pragma once

#include "clip_worker/formats/byte_view.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::formats {

struct GlbHeader {
    std::uint32_t version = 0;
    std::uint32_t byte_length = 0;
};

struct GlbChunk {
    std::uint32_t type = 0;
    std::size_t data_offset = 0;
    std::uint32_t byte_length = 0;
};

struct GlbDocument {
    GlbHeader header;
    std::string json_text;
    std::vector<GlbChunk> chunks;
    std::size_t binary_offset = 0;
    std::size_t binary_length = 0;
};

class GlbParser final {
public:
    static constexpr std::size_t kHeaderSize = 12;
    static constexpr std::size_t kChunkHeaderSize = 8;
    static constexpr std::uint32_t kSupportedVersion = 2;
    static constexpr std::uint32_t kJsonChunkType = 0x4E4F534A;
    static constexpr std::uint32_t kBinaryChunkType = 0x004E4942;
    static constexpr std::size_t kChunkAlignment = 4;

    [[nodiscard]] static GlbDocument parse(ByteView bytes);
};

}  // namespace clip_worker::formats

