#pragma once

#include "clip_worker/formats/byte_view.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::formats {

enum class I3dmGltfFormat : std::uint32_t { uri = 0U, embedded_glb = 1U };

struct I3dmSection {
    std::size_t offset = 0U;
    std::size_t byte_length = 0U;
};

struct I3dmDocument {
    std::uint32_t version = 0U;
    std::uint32_t byte_length = 0U;
    I3dmGltfFormat gltf_format = I3dmGltfFormat::embedded_glb;
    I3dmSection feature_table_json;
    I3dmSection feature_table_binary;
    I3dmSection batch_table_json;
    I3dmSection batch_table_binary;
    I3dmSection gltf_payload;
    std::string feature_table_json_text;
    std::string batch_table_json_text;
    std::string external_gltf_uri;
};

class I3dmParser final {
public:
    static constexpr std::size_t kHeaderSize = 32U;
    static constexpr std::uint32_t kSupportedVersion = 1U;
    static constexpr std::size_t kEmbeddedGlbAlignment = 8U;
    static constexpr std::size_t kMaximumTrailingPadding =
            kEmbeddedGlbAlignment - 1U;

    [[nodiscard]] static I3dmDocument parse(ByteView bytes);
};

}  // namespace clip_worker::formats
