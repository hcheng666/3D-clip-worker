#include "clip_worker/formats/i3dm.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/glb.hpp"

#include <algorithm>
#include <array>

#include <nlohmann/json.hpp>

namespace clip_worker::formats {
namespace {

constexpr std::array<std::uint8_t, 4U> kMagic = {'i', '3', 'd', 'm'};
constexpr std::size_t kGlbByteLengthOffset = 2U * sizeof(std::uint32_t);

[[noreturn]] void invalid(const std::string& message) {
    throw FormatError(FormatErrorCode::i3dm_header_invalid, message);
}

std::uint32_t readU32(ByteView bytes, std::size_t offset) {
    if (!bytes.contains(offset, sizeof(std::uint32_t))) {
        invalid("I3DM ended while reading its header");
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
        invalid("I3DM table section exceeds byteLength");
    }
    return offset + value;
}

std::string readText(ByteView bytes, const I3dmSection& section) {
    return section.byte_length == 0U
            ? std::string()
            : std::string(reinterpret_cast<const char*>(
                                  bytes.data() + section.offset),
                          section.byte_length);
}

void validateObjectJson(const std::string& value, const char* description,
                        bool required) {
    if (value.empty()) {
        if (required) invalid(std::string(description) + " is empty");
        return;
    }
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

I3dmDocument I3dmParser::parse(ByteView bytes) {
    if (bytes.size() < kHeaderSize) invalid("I3DM is shorter than its header");
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.data())) {
        invalid("I3DM magic is not i3dm");
    }
    I3dmDocument document;
    document.version = readU32(bytes, 4U);
    document.byte_length = readU32(bytes, 8U);
    const std::uint32_t feature_json = readU32(bytes, 12U);
    const std::uint32_t feature_binary = readU32(bytes, 16U);
    const std::uint32_t batch_json = readU32(bytes, 20U);
    const std::uint32_t batch_binary = readU32(bytes, 24U);
    const std::uint32_t gltf_format = readU32(bytes, 28U);
    if (document.version != kSupportedVersion) invalid("I3DM version is unsupported");
    if (document.byte_length != bytes.size()) invalid("I3DM byteLength mismatch");
    if (gltf_format > static_cast<std::uint32_t>(I3dmGltfFormat::embedded_glb)) {
        invalid("I3DM gltfFormat is unsupported");
    }
    document.gltf_format = static_cast<I3dmGltfFormat>(gltf_format);
    std::size_t offset = kHeaderSize;
    document.feature_table_json = {offset, feature_json};
    offset = advance(offset, feature_json, bytes.size());
    document.feature_table_binary = {offset, feature_binary};
    offset = advance(offset, feature_binary, bytes.size());
    document.batch_table_json = {offset, batch_json};
    offset = advance(offset, batch_json, bytes.size());
    document.batch_table_binary = {offset, batch_binary};
    offset = advance(offset, batch_binary, bytes.size());
    if (offset >= bytes.size()) invalid("I3DM has no glTF payload");
    document.gltf_payload = {offset, bytes.size() - offset};
    document.feature_table_json_text = readText(bytes, document.feature_table_json);
    document.batch_table_json_text = readText(bytes, document.batch_table_json);
    validateObjectJson(document.feature_table_json_text, "I3DM Feature Table", true);
    validateObjectJson(document.batch_table_json_text, "I3DM Batch Table", false);
    if (document.gltf_format == I3dmGltfFormat::embedded_glb) {
        if (document.gltf_payload.byte_length < GlbParser::kHeaderSize) {
            invalid("I3DM embedded GLB is shorter than its header");
        }
        const std::uint32_t declared_glb_length = readU32(
                bytes, document.gltf_payload.offset + kGlbByteLengthOffset);
        const std::size_t glb_end = advance(
                document.gltf_payload.offset, declared_glb_length,
                bytes.size());
        const std::size_t trailing_padding = bytes.size() - glb_end;
        if (trailing_padding > I3dmParser::kMaximumTrailingPadding
                || std::any_of(bytes.data() + glb_end,
                               bytes.data() + bytes.size(),
                               [](std::uint8_t value) { return value != 0U; })) {
            invalid("I3DM embedded GLB has invalid trailing padding");
        }
        document.gltf_payload.byte_length = declared_glb_length;
        static_cast<void>(GlbParser::parse(bytes.subview(
                document.gltf_payload.offset,
                document.gltf_payload.byte_length)));
    } else {
        document.external_gltf_uri = readText(bytes, document.gltf_payload);
        while (!document.external_gltf_uri.empty()
               && (document.external_gltf_uri.back() == '\0'
                   || document.external_gltf_uri.back() == ' ')) {
            document.external_gltf_uri.pop_back();
        }
        if (document.external_gltf_uri.empty()) invalid("I3DM external glTF URI is empty");
    }
    return document;
}

}  // namespace clip_worker::formats
