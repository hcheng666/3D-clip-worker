#include "clip_worker/formats/b3dm.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include <nlohmann/json.hpp>

namespace clip_worker::formats {
namespace {

constexpr std::array<std::uint8_t, 4> kB3dmMagic = {'b', '3', 'd', 'm'};

std::uint32_t readUint32(ByteView bytes, std::size_t offset) {
    if (!bytes.contains(offset, sizeof(std::uint32_t))) {
        throw FormatError(FormatErrorCode::unexpected_end,
                          "B3DM ended while reading a 32-bit field");
    }
    const auto* value = bytes.data() + offset;
    return static_cast<std::uint32_t>(value[0])
           | (static_cast<std::uint32_t>(value[1]) << 8U)
           | (static_cast<std::uint32_t>(value[2]) << 16U)
           | (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::size_t checkedAdvance(std::size_t offset, std::uint32_t length, std::size_t limit) {
    const auto value = static_cast<std::size_t>(length);
    if (offset > limit || value > limit - offset) {
        throw FormatError(FormatErrorCode::unexpected_end,
                          "B3DM table section exceeds the declared object length");
    }
    return offset + value;
}

std::string readText(ByteView bytes, const B3dmSection& section) {
    if (section.byte_length == 0U) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data() + section.offset),
                       section.byte_length);
}

void requireMinimumAlignment(std::size_t value, const char* description) {
    if (value % B3dmParser::kCompatibleAlignment != 0U) {
        throw FormatError(FormatErrorCode::invalid_alignment,
                          std::string(description)
                                  + " is not aligned to a four-byte boundary");
    }
}

void validateTrailingPadding(ByteView bytes, std::size_t offset) {
    const std::size_t length = bytes.size() - offset;
    if (length > B3dmParser::kMaximumTrailingPadding) {
        throw FormatError(FormatErrorCode::length_mismatch,
                          "B3DM contains excessive data after the embedded GLB");
    }
    if (!std::all_of(bytes.data() + offset, bytes.data() + bytes.size(),
                     [](std::uint8_t value) { return value == 0U; })) {
        throw FormatError(FormatErrorCode::length_mismatch,
                          "B3DM contains non-zero data after the embedded GLB");
    }
}

nlohmann::json parseTableJson(const std::string& value, const char* table_name) {
    if (value.empty()) {
        return nlohmann::json::object();
    }
    try {
        const auto parsed = nlohmann::json::parse(value);
        if (!parsed.is_object()) {
            throw FormatError(FormatErrorCode::invalid_json,
                              std::string(table_name) + " must contain a JSON object");
        }
        return parsed;
    } catch (const FormatError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw FormatError(FormatErrorCode::invalid_json,
                          std::string(table_name) + " is invalid: " + error.what());
    }
}

}  // namespace

B3dmDocument B3dmParser::parse(ByteView bytes) {
    if (bytes.size() < kHeaderSize) {
        throw FormatError(FormatErrorCode::unexpected_end,
                          "B3DM is shorter than its fixed header");
    }
    if (!std::equal(kB3dmMagic.begin(), kB3dmMagic.end(), bytes.data())) {
        throw FormatError(FormatErrorCode::invalid_magic, "B3DM magic is not b3dm");
    }

    B3dmDocument document;
    document.header.version = readUint32(bytes, 4);
    document.header.byte_length = readUint32(bytes, 8);
    document.header.feature_table_json_length = readUint32(bytes, 12);
    document.header.feature_table_binary_length = readUint32(bytes, 16);
    document.header.batch_table_json_length = readUint32(bytes, 20);
    document.header.batch_table_binary_length = readUint32(bytes, 24);

    if (document.header.version != kSupportedVersion) {
        throw FormatError(FormatErrorCode::unsupported_version,
                          "Only B3DM version 1 is supported");
    }
    if (document.header.byte_length != bytes.size()) {
        throw FormatError(FormatErrorCode::length_mismatch,
                          "B3DM header byteLength does not match the object length");
    }

    std::size_t offset = kHeaderSize;
    document.feature_table_json = {offset, document.header.feature_table_json_length};
    offset = checkedAdvance(offset, document.header.feature_table_json_length, bytes.size());
    document.feature_table_binary = {offset, document.header.feature_table_binary_length};
    offset = checkedAdvance(offset, document.header.feature_table_binary_length, bytes.size());
    document.batch_table_json = {offset, document.header.batch_table_json_length};
    offset = checkedAdvance(offset, document.header.batch_table_json_length, bytes.size());
    document.batch_table_binary = {offset, document.header.batch_table_binary_length};
    offset = checkedAdvance(offset, document.header.batch_table_binary_length, bytes.size());

    requireMinimumAlignment(offset, "Embedded GLB");
    requireMinimumAlignment(bytes.size(), "B3DM byteLength");
    if (offset >= bytes.size()) {
        throw FormatError(FormatErrorCode::unexpected_end,
                          "B3DM does not contain an embedded GLB");
    }

    const std::size_t glb_length_offset = offset + 2U * sizeof(std::uint32_t);
    const std::uint32_t declared_glb_length = readUint32(bytes, glb_length_offset);
    const std::size_t glb_end = checkedAdvance(
            offset, declared_glb_length, bytes.size());
    validateTrailingPadding(bytes, glb_end);

    document.feature_table_json_text = readText(bytes, document.feature_table_json);
    document.batch_table_json_text = readText(bytes, document.batch_table_json);
    const auto feature_table = parseTableJson(document.feature_table_json_text,
                                              "B3DM feature table JSON");
    parseTableJson(document.batch_table_json_text, "B3DM batch table JSON");
    const auto batch_length = feature_table.find("BATCH_LENGTH");
    if (batch_length == feature_table.end() || !batch_length->is_number_unsigned()
        || *batch_length > std::numeric_limits<std::uint32_t>::max()) {
        throw FormatError(FormatErrorCode::invalid_feature_table,
                          "B3DM feature table requires an unsigned BATCH_LENGTH");
    }
    document.batch_length = batch_length->get<std::uint32_t>();

    document.glb_section = {offset, declared_glb_length};
    document.layout.glb_offset = offset;
    document.layout.glb_byte_length = declared_glb_length;
    document.layout.trailing_padding_bytes = bytes.size() - glb_end;
    document.layout.glb_offset_standard_aligned = offset % kGlbAlignment == 0U;
    document.layout.tile_length_standard_aligned =
            bytes.size() % kGlbAlignment == 0U;
    document.glb = GlbParser::parse(bytes.subview(offset, document.glb_section.byte_length));
    return document;
}

}  // namespace clip_worker::formats
