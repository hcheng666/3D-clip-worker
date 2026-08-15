#include "clip_worker/formats/draco_decoder.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <draco/compression/decode.h>
#include <draco/core/decoder_buffer.h>
#include <draco/mesh/mesh.h>

namespace clip_worker::formats {
namespace {

constexpr std::size_t kMaximumDecodedPoints = 5U * 1000U * 1000U;
constexpr std::size_t kMaximumDecodedFaces = 10U * 1000U * 1000U;
constexpr std::size_t kMaximumAttributeComponents = 4U;
constexpr std::size_t kMaximumDecodedBytes = 512U * 1024U * 1024U;
constexpr std::size_t kTriangleVertexCount = 3U;

[[noreturn]] void invalidDraco(const std::string& message) {
    throw FormatError(FormatErrorCode::invalid_accessor, message);
}

[[noreturn]] void unsupportedDraco(const std::string& message) {
    throw FormatError(FormatErrorCode::unsupported_content, message);
}

std::size_t checkedProduct(std::size_t left, std::size_t right,
                           const char* description) {
    if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
        invalidDraco(std::string("Draco ") + description + " size overflows");
    }
    return left * right;
}

void addDecodedBytes(std::size_t count, std::size_t element_size,
                     std::size_t& decoded_bytes) {
    const std::size_t additional = checkedProduct(count, element_size,
                                                  "decoded attribute");
    if (additional > kMaximumDecodedBytes - decoded_bytes) {
        unsupportedDraco("Draco decoded data exceeds the safety limit");
    }
    decoded_bytes += additional;
}

std::string decodeFailure(const char* prefix, const draco::Status& status) {
    const std::string detail = status.error_msg_string();
    return detail.empty() ? std::string(prefix)
                          : std::string(prefix) + ": " + detail;
}

}  // namespace

DecodedDracoMesh DracoDecoder::decode(
        ByteView compressed,
        const std::vector<DracoAttributeRequest>& attributes) {
    if (compressed.data() == nullptr || compressed.size() == 0U) {
        invalidDraco("Draco bufferView is empty");
    }

    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    const auto geometry_type = draco::Decoder::GetEncodedGeometryType(&buffer);
    if (!geometry_type.ok()) {
        invalidDraco(decodeFailure("Draco geometry header is invalid",
                                   geometry_type.status()));
    }
    if (geometry_type.value() != draco::TRIANGULAR_MESH) {
        unsupportedDraco("KHR_draco_mesh_compression requires a triangular mesh");
    }

    draco::Decoder decoder;
    auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
    if (!decoded.ok()) {
        invalidDraco(decodeFailure("Draco mesh decoding failed", decoded.status()));
    }
    std::unique_ptr<draco::Mesh> mesh = std::move(decoded).value();
    if (mesh == nullptr) {
        invalidDraco("Draco mesh decoding returned no geometry");
    }

    DecodedDracoMesh result;
    result.point_count = static_cast<std::size_t>(mesh->num_points());
    const std::size_t face_count = static_cast<std::size_t>(mesh->num_faces());
    if (result.point_count == 0U || result.point_count > kMaximumDecodedPoints
        || face_count == 0U || face_count > kMaximumDecodedFaces) {
        unsupportedDraco("Draco point or face count exceeds the supported range");
    }

    std::size_t decoded_bytes = 0U;
    const std::size_t index_count = checkedProduct(face_count, kTriangleVertexCount,
                                                   "index");
    addDecodedBytes(index_count, sizeof(std::uint32_t), decoded_bytes);
    result.indices.reserve(index_count);
    for (std::size_t face_index = 0U; face_index < face_count; ++face_index) {
        const auto& face = mesh->face(
                draco::FaceIndex(static_cast<std::uint32_t>(face_index)));
        for (const draco::PointIndex& point : face) {
            const auto value = static_cast<std::uint32_t>(point.value());
            if (value >= result.point_count) {
                invalidDraco("Draco face references an invalid point");
            }
            result.indices.push_back(value);
        }
    }

    std::set<std::uint32_t> requested_ids;
    for (const auto& request : attributes) {
        if (request.component_count == 0U
            || request.component_count > kMaximumAttributeComponents
            || !requested_ids.insert(request.unique_id).second) {
            invalidDraco("Draco attribute mapping is invalid or duplicated");
        }
        const draco::PointAttribute* attribute =
                mesh->GetAttributeByUniqueId(request.unique_id);
        if (attribute == nullptr) {
            invalidDraco("Draco attribute mapping references an unknown unique id");
        }
        if (static_cast<std::size_t>(attribute->num_components())
            != request.component_count) {
            invalidDraco("Draco attribute component count differs from its accessor");
        }

        const std::size_t value_count = checkedProduct(
                result.point_count, request.component_count, "attribute value");
        if (request.value_type == DracoAttributeValueType::unsigned_integer) {
            if (request.component_count != 1U) {
                invalidDraco("Draco unsigned attributes must be scalar");
            }
            addDecodedBytes(value_count, sizeof(std::uint32_t), decoded_bytes);
            auto& values = result.unsigned_attributes[request.unique_id];
            values.resize(value_count);
            for (std::size_t point = 0U; point < result.point_count; ++point) {
                const auto mapped = attribute->mapped_index(
                        draco::PointIndex(static_cast<std::uint32_t>(point)));
                if (!attribute->ConvertValue(mapped, values.data() + point)) {
                    invalidDraco("Draco unsigned attribute conversion failed");
                }
            }
            continue;
        }

        addDecodedBytes(value_count, sizeof(double), decoded_bytes);
        auto& values = result.floating_attributes[request.unique_id];
        values.resize(value_count);
        for (std::size_t point = 0U; point < result.point_count; ++point) {
            const auto mapped = attribute->mapped_index(
                    draco::PointIndex(static_cast<std::uint32_t>(point)));
            double* output = values.data() + point * request.component_count;
            if (!attribute->ConvertValue(
                        mapped, static_cast<std::int8_t>(request.component_count), output)) {
                invalidDraco("Draco floating-point attribute conversion failed");
            }
            for (std::size_t component = 0U;
                 component < request.component_count; ++component) {
                if (!std::isfinite(output[component])) {
                    invalidDraco("Draco attribute contains a non-finite value");
                }
            }
        }
    }

    return result;
}

}  // namespace clip_worker::formats
