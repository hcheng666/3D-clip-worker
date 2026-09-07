#pragma once

#include "clip_worker/formats/byte_view.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clip_worker::formats {

enum class MeshoptMode {
    attributes,
    triangles,
    indices,
};

enum class MeshoptFilter {
    none,
    octahedral,
    quaternion,
    exponential,
};

struct MeshoptDecodeLimits {
    std::size_t maximum_count = 100000000U;
    std::size_t maximum_decoded_bytes = 1073741824U;
    std::size_t maximum_stride = 256U;
};

/** Strict adapter for EXT_meshopt_compression buffer-view payloads. */
class MeshoptDecoder final {
public:
    [[nodiscard]] static std::vector<std::uint8_t> decode(
            ByteView compressed,
            std::size_t count,
            std::size_t byte_stride,
            MeshoptMode mode,
            MeshoptFilter filter,
            const MeshoptDecodeLimits& limits = {});
};

}  // namespace clip_worker::formats
