#pragma once

#include "clip_worker/formats/byte_view.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace clip_worker::formats {

struct PntsSection {
    std::size_t offset = 0U;
    std::size_t byte_length = 0U;
};

struct PntsDocument {
    std::uint32_t version = 0U;
    std::uint32_t byte_length = 0U;
    PntsSection feature_table_json;
    PntsSection feature_table_binary;
    PntsSection batch_table_json;
    PntsSection batch_table_binary;
    std::string feature_table_json_text;
    std::string batch_table_json_text;
};

class PntsParser final {
public:
    static constexpr std::size_t kHeaderSize = 28U;
    static constexpr std::uint32_t kSupportedVersion = 1U;

    [[nodiscard]] static PntsDocument parse(ByteView bytes);
};

}  // namespace clip_worker::formats
