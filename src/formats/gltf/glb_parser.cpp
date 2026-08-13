#include "clip_worker/formats/glb.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <array>

#include <nlohmann/json.hpp>

namespace clip_worker::formats {
namespace {

constexpr std::array<std::uint8_t, 4> kGlbMagic = {'g', 'l', 'T', 'F'};

std::uint32_t readUint32(ByteView bytes, std::size_t offset) {
    if (!bytes.contains(offset, sizeof(std::uint32_t))) {
        throw FormatError(FormatErrorCode::unexpected_end,
                          "GLB ended while reading a 32-bit field");
    }
    const auto* value = bytes.data() + offset;
    return static_cast<std::uint32_t>(value[0])
           | (static_cast<std::uint32_t>(value[1]) << 8U)
           | (static_cast<std::uint32_t>(value[2]) << 16U)
           | (static_cast<std::uint32_t>(value[3]) << 24U);
}

void validateJson(const std::string& json_text) {
    try {
        const auto parsed = nlohmann::json::parse(json_text);
        if (!parsed.is_object()) {
            throw FormatError(FormatErrorCode::invalid_json,
                              "GLB JSON chunk must contain an object");
        }
    } catch (const FormatError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw FormatError(FormatErrorCode::invalid_json,
                          std::string("GLB JSON chunk is invalid: ") + error.what());
    }
}

}  // namespace

GlbDocument GlbParser::parse(ByteView bytes) {
    if (bytes.size() < kHeaderSize) {
        throw FormatError(FormatErrorCode::unexpected_end,
                          "GLB is shorter than its fixed header");
    }
    if (!std::equal(kGlbMagic.begin(), kGlbMagic.end(), bytes.data())) {
        throw FormatError(FormatErrorCode::invalid_magic, "GLB magic is not glTF");
    }

    GlbDocument document;
    document.header.version = readUint32(bytes, 4);
    document.header.byte_length = readUint32(bytes, 8);
    if (document.header.version != kSupportedVersion) {
        throw FormatError(FormatErrorCode::unsupported_version,
                          "Only GLB version 2 is supported");
    }
    if (document.header.byte_length != bytes.size()) {
        throw FormatError(FormatErrorCode::length_mismatch,
                          "GLB header byteLength does not match the object length");
    }

    std::size_t offset = kHeaderSize;
    bool found_json = false;
    bool found_binary = false;
    while (offset < bytes.size()) {
        if (!bytes.contains(offset, kChunkHeaderSize)) {
            throw FormatError(FormatErrorCode::unexpected_end,
                              "GLB ended inside a chunk header");
        }
        const auto chunk_length = readUint32(bytes, offset);
        const auto chunk_type = readUint32(bytes, offset + sizeof(std::uint32_t));
        const auto data_offset = offset + kChunkHeaderSize;
        if (chunk_length % kChunkAlignment != 0U) {
            throw FormatError(FormatErrorCode::invalid_alignment,
                              "GLB chunk length is not aligned to four bytes");
        }
        if (!bytes.contains(data_offset, chunk_length)) {
            throw FormatError(FormatErrorCode::unexpected_end,
                              "GLB chunk exceeds the declared object length");
        }

        if (!found_json) {
            if (chunk_type != kJsonChunkType) {
                throw FormatError(FormatErrorCode::invalid_chunk_order,
                                  "The first GLB chunk must be JSON");
            }
            document.json_text.assign(
                    reinterpret_cast<const char*>(bytes.data() + data_offset), chunk_length);
            validateJson(document.json_text);
            found_json = true;
        } else if (chunk_type == kBinaryChunkType && !found_binary) {
            document.binary_offset = data_offset;
            document.binary_length = chunk_length;
            found_binary = true;
        } else {
            throw FormatError(FormatErrorCode::unsupported_chunk,
                              "GLB contains an unsupported or duplicate chunk");
        }

        document.chunks.push_back({chunk_type, data_offset, chunk_length});
        offset = data_offset + chunk_length;
    }

    if (!found_json) {
        throw FormatError(FormatErrorCode::invalid_chunk_order,
                          "GLB does not contain a JSON chunk");
    }
    return document;
}

}  // namespace clip_worker::formats

