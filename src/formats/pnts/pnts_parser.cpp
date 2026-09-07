#include "clip_worker/formats/pnts.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <array>

#include <nlohmann/json.hpp>

namespace clip_worker::formats {
namespace {

constexpr std::array<std::uint8_t, 4U> kMagic = {'p', 'n', 't', 's'};

[[noreturn]] void invalid(const std::string& message) {
    throw FormatError(FormatErrorCode::pnts_header_invalid, message);
}

std::uint32_t readU32(ByteView bytes, std::size_t offset) {
    if (!bytes.contains(offset, sizeof(std::uint32_t))) {
        invalid("PNTS ended while reading its header");
    }
    const auto* value = bytes.data() + offset;
    return static_cast<std::uint32_t>(value[0])
           | (static_cast<std::uint32_t>(value[1]) << 8U)
           | (static_cast<std::uint32_t>(value[2]) << 16U)
           | (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::size_t advance(std::size_t offset, std::uint32_t length,
                    std::size_t limit) {
    const std::size_t value = length;
    if (offset > limit || value > limit - offset) {
        invalid("PNTS table section exceeds byteLength");
    }
    return offset + value;
}

std::string readText(ByteView bytes, const PntsSection& section) {
    return section.byte_length == 0U
            ? std::string()
            : std::string(reinterpret_cast<const char*>(
                                  bytes.data() + section.offset),
                          section.byte_length);
}

void validateObjectJson(const std::string& value, const char* description) {
    if (value.empty()) invalid(std::string(description) + " is empty");
    try {
        const auto json = nlohmann::json::parse(value);
        if (!json.is_object()) invalid(std::string(description) + " is not an object");
    } catch (const FormatError&) {
        throw;
    } catch (const nlohmann::json::exception&) {
        invalid(std::string(description) + " is invalid JSON");
    }
}

}  // namespace

PntsDocument PntsParser::parse(ByteView bytes) {
    if (bytes.size() < kHeaderSize) invalid("PNTS is shorter than its header");
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.data())) {
        invalid("PNTS magic is not pnts");
    }
    PntsDocument document;
    document.version = readU32(bytes, 4U);
    document.byte_length = readU32(bytes, 8U);
    const std::uint32_t feature_json = readU32(bytes, 12U);
    const std::uint32_t feature_binary = readU32(bytes, 16U);
    const std::uint32_t batch_json = readU32(bytes, 20U);
    const std::uint32_t batch_binary = readU32(bytes, 24U);
    if (document.version != kSupportedVersion) invalid("PNTS version is unsupported");
    if (document.byte_length != bytes.size()) invalid("PNTS byteLength mismatch");

    std::size_t offset = kHeaderSize;
    document.feature_table_json = {offset, feature_json};
    offset = advance(offset, feature_json, bytes.size());
    document.feature_table_binary = {offset, feature_binary};
    offset = advance(offset, feature_binary, bytes.size());
    document.batch_table_json = {offset, batch_json};
    offset = advance(offset, batch_json, bytes.size());
    document.batch_table_binary = {offset, batch_binary};
    offset = advance(offset, batch_binary, bytes.size());
    if (offset != bytes.size()) invalid("PNTS contains undeclared trailing bytes");
    if (document.feature_table_json.offset % 4U != 0U
        || document.feature_table_binary.offset % 4U != 0U
        || document.batch_table_json.offset % 4U != 0U
        || document.batch_table_binary.offset % 4U != 0U) {
        invalid("PNTS table sections are not four-byte aligned");
    }
    document.feature_table_json_text = readText(bytes, document.feature_table_json);
    document.batch_table_json_text = readText(bytes, document.batch_table_json);
    validateObjectJson(document.feature_table_json_text, "PNTS Feature Table");
    if (!document.batch_table_json_text.empty()) {
        validateObjectJson(document.batch_table_json_text, "PNTS Batch Table");
    }
    return document;
}

}  // namespace clip_worker::formats
