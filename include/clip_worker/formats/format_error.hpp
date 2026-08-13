#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace clip_worker::formats {

enum class FormatErrorCode {
    unexpected_end,
    invalid_magic,
    unsupported_version,
    length_mismatch,
    invalid_alignment,
    invalid_chunk_order,
    invalid_json,
    invalid_feature_table,
    unsupported_chunk,
    unsupported_content,
    invalid_accessor
};

class FormatError final : public std::runtime_error {
public:
    FormatError(FormatErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {
    }

    [[nodiscard]] FormatErrorCode code() const noexcept {
        return code_;
    }

private:
    FormatErrorCode code_;
};

}  // namespace clip_worker::formats
