#include "clip_worker/formats/cmpt.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/normalization_contract.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace clip_worker::formats {
namespace {

constexpr std::array<std::uint8_t, 4U> kCmptMagic = {'c', 'm', 'p', 't'};

[[noreturn]] void invalid(const std::string& message) {
    throw FormatError(FormatErrorCode::cmpt_header_invalid, message);
}

std::uint32_t readU32(ByteView bytes, std::size_t offset) {
    if (!bytes.contains(offset, sizeof(std::uint32_t))) {
        invalid("CMPT ended while reading a common header");
    }
    const auto* value = bytes.data() + offset;
    return static_cast<std::uint32_t>(value[0])
           | (static_cast<std::uint32_t>(value[1]) << 8U)
           | (static_cast<std::uint32_t>(value[2]) << 16U)
           | (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::string magic(ByteView bytes, std::size_t offset) {
    if (!bytes.contains(offset, 4U)) invalid("CMPT child magic is truncated");
    return std::string(reinterpret_cast<const char*>(bytes.data() + offset), 4U);
}

CompositeChildKind kind(const std::string& value) {
    if (value == "b3dm") return CompositeChildKind::b3dm;
    if (value == "pnts") return CompositeChildKind::pnts;
    if (value == "i3dm") return CompositeChildKind::i3dm;
    if (value == "glTF") return CompositeChildKind::glb;
    if (value == "cmpt") return CompositeChildKind::cmpt;
    return CompositeChildKind::unsupported;
}

void appendU32(std::string& value, std::uint32_t number) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        value.push_back(static_cast<char>((number >> (byte * 8U)) & 0xffU));
    }
}

std::string childIdentity(const CompositeChild& child) {
    std::string canonical("CMPT_CHILD_V1\0", 14U);
    appendU32(canonical, static_cast<std::uint32_t>(child.ordinal_path.size()));
    for (const std::uint32_t ordinal : child.ordinal_path) appendU32(canonical, ordinal);
    canonical.append(child.magic);
    appendU32(canonical, child.version);
    canonical.append(child.source_sha256);
    return normalization::sha256Hex(canonical);
}

std::vector<CompositeChild> parseChildren(
        ByteView outer, std::size_t base_offset,
        const std::vector<std::uint32_t>& parent_path,
        std::uint32_t depth, const CmptLimits& limits,
        std::uint64_t& total_children) {
    if (depth > limits.maximum_depth) {
        throw FormatError(FormatErrorCode::cmpt_recursion_limit_exceeded,
                          "CMPT nesting depth exceeds the configured limit");
    }
    if (outer.size() < CmptParser::kHeaderSize
        || !std::equal(kCmptMagic.begin(), kCmptMagic.end(), outer.data())) {
        invalid("Nested CMPT header is invalid");
    }
    const std::uint32_t version = readU32(outer, 4U);
    const std::uint32_t byte_length = readU32(outer, 8U);
    const std::uint32_t child_count = readU32(outer, 12U);
    if (version != CmptParser::kSupportedVersion || byte_length != outer.size()) {
        invalid("CMPT version or byteLength is invalid");
    }
    if (child_count == 0U
        || total_children > limits.maximum_children - child_count) {
        throw FormatError(FormatErrorCode::cmpt_recursion_limit_exceeded,
                          "CMPT child count is zero or exceeds the configured limit");
    }
    total_children += child_count;
    std::vector<CompositeChild> children;
    children.reserve(child_count);
    std::size_t offset = CmptParser::kHeaderSize;
    for (std::uint32_t ordinal = 0U; ordinal < child_count; ++ordinal) {
        if (offset % 4U != 0U || !outer.contains(offset, 12U)) {
            invalid("CMPT child header is misaligned or truncated");
        }
        CompositeChild child;
        child.ordinal_path = parent_path;
        child.ordinal_path.push_back(ordinal);
        child.depth = depth;
        child.source_offset = base_offset + offset;
        child.magic = magic(outer, offset);
        child.kind = kind(child.magic);
        child.version = readU32(outer, offset + 4U);
        child.source_length = readU32(outer, offset + 8U);
        if (child.source_length < 12U
            || !outer.contains(offset, child.source_length)) {
            invalid("CMPT child byteLength is invalid");
        }
        const ByteView child_bytes = outer.subview(offset, child.source_length);
        child.source_sha256 = normalization::sha256Hex(std::string(
                reinterpret_cast<const char*>(child_bytes.data()),
                child_bytes.size()));
        child.child_id = childIdentity(child);
        if (child.kind == CompositeChildKind::cmpt) {
            child.children = parseChildren(
                    child_bytes, child.source_offset, child.ordinal_path,
                    depth + 1U, limits, total_children);
        }
        children.push_back(std::move(child));
        offset += children.back().source_length;
    }
    if (offset != outer.size()) invalid("CMPT contains undeclared trailing bytes");
    return children;
}

}  // namespace

CmptDocument CmptParser::parse(ByteView bytes, const CmptLimits& limits) {
    if (limits.maximum_depth == 0U || limits.maximum_children == 0U) {
        throw FormatError(FormatErrorCode::cmpt_recursion_limit_exceeded,
                          "CMPT limits must be positive");
    }
    std::uint64_t total_children = 0U;
    CmptDocument document;
    document.version = readU32(bytes, 4U);
    document.byte_length = readU32(bytes, 8U);
    document.children = parseChildren(
            bytes, 0U, {}, 1U, limits, total_children);
    return document;
}

std::string compositeChildKindName(CompositeChildKind kind_value) {
    switch (kind_value) {
        case CompositeChildKind::b3dm: return "B3DM";
        case CompositeChildKind::pnts: return "PNTS";
        case CompositeChildKind::i3dm: return "I3DM";
        case CompositeChildKind::glb: return "GLB";
        case CompositeChildKind::cmpt: return "CMPT";
        case CompositeChildKind::unsupported: return "UNSUPPORTED";
    }
    return "UNSUPPORTED";
}

}  // namespace clip_worker::formats
