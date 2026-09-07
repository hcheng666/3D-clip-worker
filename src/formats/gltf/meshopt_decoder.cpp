#include "clip_worker/formats/meshopt_decoder.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <limits>
#include <stdexcept>
#include <string>

#include <meshoptimizer.h>

namespace clip_worker::formats {
namespace {

[[noreturn]] void invalidMeshopt(const char* message) {
    throw FormatError(FormatErrorCode::compression_meshopt_invalid, message);
}

[[noreturn]] void unsupportedMeshopt(const char* message) {
    throw FormatError(FormatErrorCode::compression_meshopt_unsupported,
                      message);
}

[[noreturn]] void limitMeshopt(const char* message) {
    throw FormatError(FormatErrorCode::compression_meshopt_limit_exceeded,
                      message);
}

void validateFilter(MeshoptMode mode, MeshoptFilter filter,
                    std::size_t stride) {
    if (mode != MeshoptMode::attributes && filter != MeshoptFilter::none) {
        unsupportedMeshopt("Meshopt index modes cannot use an attribute filter");
    }
    switch (filter) {
        case MeshoptFilter::none:
            return;
        case MeshoptFilter::octahedral:
            if (stride != 4U && stride != 8U) {
                unsupportedMeshopt("Meshopt octahedral filter stride is unsupported");
            }
            return;
        case MeshoptFilter::quaternion:
            if (stride != 8U) {
                unsupportedMeshopt("Meshopt quaternion filter stride is unsupported");
            }
            return;
        case MeshoptFilter::exponential:
            if (stride % 4U != 0U) {
                unsupportedMeshopt("Meshopt exponential filter stride is unsupported");
            }
            return;
    }
    throw std::invalid_argument("Unknown Meshopt filter enum value");
}

}  // namespace

std::vector<std::uint8_t> MeshoptDecoder::decode(
        ByteView compressed, std::size_t count, std::size_t byte_stride,
        MeshoptMode mode, MeshoptFilter filter,
        const MeshoptDecodeLimits& limits) {
    if (compressed.data() == nullptr || compressed.size() == 0U) {
        invalidMeshopt("Meshopt bufferView is empty");
    }
    if (count == 0U || byte_stride == 0U) {
        invalidMeshopt("Meshopt count or stride is zero");
    }
    if (count > limits.maximum_count
        || byte_stride > limits.maximum_stride) {
        limitMeshopt("Meshopt count or stride exceeds the configured limit");
    }
    if (count > std::numeric_limits<std::size_t>::max() / byte_stride) {
        invalidMeshopt("Meshopt decoded byte length overflows");
    }
    const std::size_t decoded_bytes = count * byte_stride;
    if (decoded_bytes > limits.maximum_decoded_bytes) {
        limitMeshopt("Meshopt decoded bytes exceed the configured limit");
    }
    validateFilter(mode, filter, byte_stride);

    std::vector<std::uint8_t> output(decoded_bytes);
    int status = -1;
    switch (mode) {
        case MeshoptMode::attributes:
            status = meshopt_decodeVertexBuffer(
                    output.data(), count, byte_stride,
                    compressed.data(), compressed.size());
            break;
        case MeshoptMode::triangles:
            if (byte_stride != 2U && byte_stride != 4U) {
                unsupportedMeshopt("Meshopt triangle index stride is unsupported");
            }
            status = meshopt_decodeIndexBuffer(
                    output.data(), count, byte_stride,
                    compressed.data(), compressed.size());
            break;
        case MeshoptMode::indices:
            if (byte_stride != 2U && byte_stride != 4U) {
                unsupportedMeshopt("Meshopt index sequence stride is unsupported");
            }
            status = meshopt_decodeIndexSequence(
                    output.data(), count, byte_stride,
                    compressed.data(), compressed.size());
            break;
    }
    if (status != 0) {
        invalidMeshopt("Meshopt payload decoding failed");
    }

    switch (filter) {
        case MeshoptFilter::none: break;
        case MeshoptFilter::octahedral:
            meshopt_decodeFilterOct(output.data(), count, byte_stride);
            break;
        case MeshoptFilter::quaternion:
            meshopt_decodeFilterQuat(output.data(), count, byte_stride);
            break;
        case MeshoptFilter::exponential:
            meshopt_decodeFilterExp(output.data(), count, byte_stride);
            break;
    }
    return output;
}

}  // namespace clip_worker::formats
