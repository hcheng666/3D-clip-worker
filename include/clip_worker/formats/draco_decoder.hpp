#pragma once

#include "clip_worker/formats/byte_view.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace clip_worker::formats {

enum class DracoAttributeValueType {
    floating_point,
    unsigned_integer
};

struct DracoAttributeRequest {
    std::uint32_t unique_id = 0U;
    std::size_t component_count = 0U;
    DracoAttributeValueType value_type = DracoAttributeValueType::floating_point;
};

struct DecodedDracoMesh {
    std::size_t point_count = 0U;
    std::vector<std::uint32_t> indices;
    std::map<std::uint32_t, std::vector<double>> floating_attributes;
    std::map<std::uint32_t, std::vector<std::uint32_t>> unsigned_attributes;
};

/** Decodes the bounded subset of Draco mesh data used by glTF primitives. */
class DracoDecoder final {
public:
    [[nodiscard]] static DecodedDracoMesh decode(
            ByteView compressed,
            const std::vector<DracoAttributeRequest>& attributes);
};

}  // namespace clip_worker::formats
