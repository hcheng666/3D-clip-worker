#pragma once

#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/glb.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace clip_worker::formats {

struct B3dmHeader {
    std::uint32_t version = 0;
    std::uint32_t byte_length = 0;
    std::uint32_t feature_table_json_length = 0;
    std::uint32_t feature_table_binary_length = 0;
    std::uint32_t batch_table_json_length = 0;
    std::uint32_t batch_table_binary_length = 0;
};

struct B3dmSection {
    std::size_t offset = 0;
    std::size_t byte_length = 0;
};

/** Describes accepted outer B3DM layout without exposing source-object identity. */
struct B3dmLayoutDiagnostics {
    std::size_t glb_offset = 0;
    std::size_t glb_byte_length = 0;
    std::size_t trailing_padding_bytes = 0;
    bool glb_offset_standard_aligned = true;
    bool tile_length_standard_aligned = true;

    [[nodiscard]] bool requiresCompatibility() const noexcept {
        return !glb_offset_standard_aligned || !tile_length_standard_aligned;
    }
};

struct B3dmDocument {
    B3dmHeader header;
    B3dmSection feature_table_json;
    B3dmSection feature_table_binary;
    B3dmSection batch_table_json;
    B3dmSection batch_table_binary;
    B3dmSection glb_section;
    std::string feature_table_json_text;
    std::string batch_table_json_text;
    std::uint32_t batch_length = 0;
    B3dmLayoutDiagnostics layout;
    GlbDocument glb;
};

class B3dmParser final {
public:
    static constexpr std::size_t kHeaderSize = 28;
    static constexpr std::uint32_t kSupportedVersion = 1;
    static constexpr std::size_t kGlbAlignment = 8;
    static constexpr std::size_t kCompatibleAlignment = GlbParser::kChunkAlignment;
    static constexpr std::size_t kMaximumTrailingPadding = kGlbAlignment - 1U;

    [[nodiscard]] static B3dmDocument parse(ByteView bytes);
};

}  // namespace clip_worker::formats
