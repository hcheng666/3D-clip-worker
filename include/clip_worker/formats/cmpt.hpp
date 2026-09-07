#pragma once

#include "clip_worker/formats/byte_view.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::formats {

enum class CompositeChildKind { b3dm, pnts, i3dm, glb, cmpt, unsupported };

struct CompositeChild {
    std::vector<std::uint32_t> ordinal_path;
    std::uint32_t depth = 0U;
    std::size_t source_offset = 0U;
    std::size_t source_length = 0U;
    std::uint32_t version = 0U;
    CompositeChildKind kind = CompositeChildKind::unsupported;
    std::string magic;
    std::string source_sha256;
    std::string child_id;
    std::vector<CompositeChild> children;
};

struct CmptDocument {
    std::uint32_t version = 0U;
    std::uint32_t byte_length = 0U;
    std::vector<CompositeChild> children;
};

struct CmptLimits {
    std::uint32_t maximum_depth = 8U;
    std::uint64_t maximum_children = 1024U;
};

class CmptParser final {
public:
    static constexpr std::size_t kHeaderSize = 16U;
    static constexpr std::uint32_t kSupportedVersion = 1U;

    [[nodiscard]] static CmptDocument parse(
            ByteView bytes, const CmptLimits& limits = {});
};

[[nodiscard]] std::string compositeChildKindName(CompositeChildKind kind);

}  // namespace clip_worker::formats
