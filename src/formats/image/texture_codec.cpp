#include "clip_worker/formats/texture_codec.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <ktx.h>
#include <png.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <zlib.h>

namespace clip_worker::formats {
namespace {

constexpr std::uint64_t kRgbaChannelCount = 4U;
constexpr int kCanonicalPngCompressionLevel = 9;

[[noreturn]] void invalidTexture(const char* message) {
    throw FormatError(FormatErrorCode::texture_invalid, message);
}

[[noreturn]] void unsupportedTextureFormat(const char* message) {
    throw FormatError(FormatErrorCode::texture_format_unsupported, message);
}

[[noreturn]] void unsupportedKtx2(const char* message) {
    throw FormatError(FormatErrorCode::texture_ktx2_unsupported, message);
}

[[noreturn]] void limitTextureDimensions(const char* message) {
    throw FormatError(
            FormatErrorCode::texture_dimension_limit_exceeded, message);
}

[[noreturn]] void limitTextureBytes(const char* message) {
    throw FormatError(
            FormatErrorCode::texture_decoded_bytes_limit_exceeded, message);
}

std::size_t checkedRgbaBytes(std::uint32_t width, std::uint32_t height,
                             const TextureDecodeLimits& limits) {
    if (width == 0U || height == 0U
        || width > limits.maximum_width || height > limits.maximum_height) {
        limitTextureDimensions("Texture dimensions exceed the configured limit");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > limits.maximum_pixels
        || pixels > std::numeric_limits<std::uint64_t>::max()
                        / kRgbaChannelCount) {
        limitTextureDimensions("Texture pixel count exceeds the configured limit");
    }
    const std::uint64_t bytes = pixels * kRgbaChannelCount;
    if (bytes > limits.maximum_rgba_bytes
        || bytes > std::numeric_limits<std::size_t>::max()) {
        limitTextureBytes("Texture RGBA bytes exceed the configured limit");
    }
    return static_cast<std::size_t>(bytes);
}

mesh::RgbaImage decodePng(const std::vector<std::uint8_t>& encoded,
                          const TextureDecodeLimits& limits) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (encoded.empty()
        || png_image_begin_read_from_memory(
                &image, encoded.data(), encoded.size()) == 0) {
        invalidTexture("PNG header is invalid");
    }
    struct ImageCleanup {
        png_image* value;
        ~ImageCleanup() { png_image_free(value); }
    } cleanup{&image};
    const auto width = static_cast<std::uint32_t>(image.width);
    const auto height = static_cast<std::uint32_t>(image.height);
    const std::size_t byte_count = checkedRgbaBytes(width, height, limits);
    image.format = PNG_FORMAT_RGBA;
    mesh::RgbaImage result{width, height, std::vector<std::uint8_t>(byte_count)};
    if (png_image_finish_read(&image, nullptr, result.pixels.data(), 0, nullptr)
        == 0) {
        invalidTexture("PNG pixel decoding failed");
    }
    return result;
}

mesh::RgbaImage decodeJpeg(const std::vector<std::uint8_t>& encoded,
                           const TextureDecodeLimits& limits) {
    using TurboHandle = std::unique_ptr<void, decltype(&tj3Destroy)>;
    TurboHandle handle(tj3Init(TJINIT_DECOMPRESS), &tj3Destroy);
    if (handle == nullptr || encoded.empty()
        || tj3DecompressHeader(handle.get(), encoded.data(), encoded.size()) != 0) {
        invalidTexture("JPEG header is invalid");
    }
    const int raw_width = tj3Get(handle.get(), TJPARAM_JPEGWIDTH);
    const int raw_height = tj3Get(handle.get(), TJPARAM_JPEGHEIGHT);
    if (raw_width <= 0 || raw_height <= 0) {
        invalidTexture("JPEG dimensions are invalid");
    }
    const auto width = static_cast<std::uint32_t>(raw_width);
    const auto height = static_cast<std::uint32_t>(raw_height);
    const std::size_t byte_count = checkedRgbaBytes(width, height, limits);
    mesh::RgbaImage result{width, height, std::vector<std::uint8_t>(byte_count)};
    if (tj3Decompress8(handle.get(), encoded.data(), encoded.size(),
                      result.pixels.data(), 0, TJPF_RGBA) != 0) {
        invalidTexture("JPEG pixel decoding failed");
    }
    return result;
}

mesh::RgbaImage decodeWebp(const std::vector<std::uint8_t>& encoded,
                           const TextureDecodeLimits& limits) {
    int raw_width = 0;
    int raw_height = 0;
    if (encoded.empty()
        || WebPGetInfo(encoded.data(), encoded.size(), &raw_width, &raw_height)
                == 0
        || raw_width <= 0 || raw_height <= 0) {
        invalidTexture("WebP header is invalid");
    }
    const auto width = static_cast<std::uint32_t>(raw_width);
    const auto height = static_cast<std::uint32_t>(raw_height);
    const std::size_t byte_count = checkedRgbaBytes(width, height, limits);
    mesh::RgbaImage result{width, height, std::vector<std::uint8_t>(byte_count)};
    if (WebPDecodeRGBAInto(encoded.data(), encoded.size(), result.pixels.data(),
                           result.pixels.size(), width * 4U) == nullptr) {
        invalidTexture("WebP pixel decoding failed");
    }
    return result;
}

struct KtxTextureDeleter {
    void operator()(ktxTexture2* texture) const noexcept {
        if (texture != nullptr) {
            ktxTexture_Destroy(ktxTexture(texture));
        }
    }
};

mesh::RgbaImage decodeKtx2(const std::vector<std::uint8_t>& encoded,
                           const TextureDecodeLimits& limits) {
    ktxTexture2* raw_texture = nullptr;
    const KTX_error_code create_status = encoded.empty()
            ? KTX_INVALID_VALUE
            : ktxTexture2_CreateFromMemory(
                    encoded.data(), encoded.size(),
                    KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw_texture);
    std::unique_ptr<ktxTexture2, KtxTextureDeleter> texture(raw_texture);
    if (create_status != KTX_SUCCESS || texture == nullptr) {
        invalidTexture("KTX2 header or level data is invalid");
    }
    if (texture->numDimensions != 2U || texture->isArray == KTX_TRUE
        || texture->isCubemap == KTX_TRUE || texture->baseDepth != 1U
        || texture->numLayers != 1U || texture->numFaces != 1U
        || texture->isVideo == KTX_TRUE) {
        unsupportedKtx2("Only a single 2D KTX2 texture is supported");
    }
    if (texture->orientation.x != KTX_ORIENT_X_RIGHT
        || texture->orientation.y != KTX_ORIENT_Y_DOWN) {
        unsupportedKtx2("KTX2 orientation is unsupported");
    }
    const khr_df_model_e color_model = ktxTexture2_GetColorModel_e(texture.get());
    if (color_model != KHR_DF_MODEL_ETC1S
        && color_model != KHR_DF_MODEL_UASTC) {
        unsupportedKtx2("KTX2 is not Basis ETC1S or UASTC");
    }
    const khr_df_transfer_e transfer = ktxTexture2_GetOETF_e(texture.get());
    if (transfer != KHR_DF_TRANSFER_LINEAR
        && transfer != KHR_DF_TRANSFER_SRGB) {
        unsupportedKtx2("KTX2 transfer function is unsupported");
    }
    if (ktxTexture2_GetPremultipliedAlpha(texture.get()) == KTX_TRUE) {
        unsupportedKtx2("Premultiplied-alpha KTX2 is unsupported");
    }
    const auto width = static_cast<std::uint32_t>(texture->baseWidth);
    const auto height = static_cast<std::uint32_t>(texture->baseHeight);
    const std::size_t byte_count = checkedRgbaBytes(width, height, limits);
    if (ktxTexture2_NeedsTranscoding(texture.get()) != KTX_TRUE
        || ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_RGBA32, 0U)
                != KTX_SUCCESS) {
        invalidTexture("KTX2 Basis transcoding failed");
    }
    ktx_size_t image_offset = 0U;
    if (ktxTexture_GetImageOffset(ktxTexture(texture.get()), 0U, 0U, 0U,
                                  &image_offset) != KTX_SUCCESS
        || texture->pData == nullptr || image_offset > texture->dataSize
        || byte_count > texture->dataSize - image_offset) {
        invalidTexture("KTX2 transcoded image range is invalid");
    }
    mesh::RgbaImage result{width, height, std::vector<std::uint8_t>(byte_count)};
    std::memcpy(result.pixels.data(), texture->pData + image_offset, byte_count);
    return result;
}

void pngWrite(png_structp png, png_bytep data, png_size_t length) {
    auto* output = static_cast<std::vector<std::uint8_t>*>(
            png_get_io_ptr(png));
    output->insert(output->end(), data, data + length);
}

void pngFlush(png_structp) {}

}  // namespace

TextureFormat TextureCodec::formatForMimeType(const std::string& mime_type) {
    if (mime_type == "image/png") return TextureFormat::png;
    if (mime_type == "image/jpeg") return TextureFormat::jpeg;
    if (mime_type == "image/webp") return TextureFormat::webp;
    if (mime_type == "image/ktx2") return TextureFormat::ktx2_basis;
    unsupportedTextureFormat("Texture MIME type is unsupported");
}

mesh::RgbaImage TextureCodec::decode(
        const std::vector<std::uint8_t>& encoded, TextureFormat format,
        const TextureDecodeLimits& limits) {
    switch (format) {
        case TextureFormat::png: return decodePng(encoded, limits);
        case TextureFormat::jpeg: return decodeJpeg(encoded, limits);
        case TextureFormat::webp: return decodeWebp(encoded, limits);
        case TextureFormat::ktx2_basis: return decodeKtx2(encoded, limits);
    }
    throw std::invalid_argument("Unknown texture format enum value");
}

std::vector<std::uint8_t> TextureCodec::encodeDeterministicPng(
        const mesh::RgbaImage& image) {
    const TextureDecodeLimits exact_limits{
            image.width, image.height,
            static_cast<std::uint64_t>(image.width) * image.height,
            static_cast<std::uint64_t>(image.width) * image.height * 4U};
    const std::size_t expected_bytes = checkedRgbaBytes(
            image.width, image.height, exact_limits);
    if (image.pixels.size() != expected_bytes) {
        invalidTexture("RGBA image dimensions do not match its pixels");
    }

    png_structp png = png_create_write_struct(
            PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        throw std::runtime_error("Unable to initialize deterministic PNG encoder");
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        throw std::runtime_error("Unable to initialize deterministic PNG metadata");
    }
    std::vector<std::uint8_t> output;
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        invalidTexture("Deterministic PNG encoding failed");
    }
    png_set_write_fn(png, &output, pngWrite, pngFlush);
    png_set_IHDR(png, info, image.width, image.height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_set_compression_level(png, kCanonicalPngCompressionLevel);
    png_set_compression_strategy(png, Z_DEFAULT_STRATEGY);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);
    png_write_info(png, info);
    std::vector<png_bytep> rows(image.height);
    for (std::size_t row = 0U; row < rows.size(); ++row) {
        rows[row] = const_cast<png_bytep>(
                image.pixels.data() + row * image.width * 4U);
    }
    png_write_image(png, rows.data());
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);

    const mesh::RgbaImage verified = decodePng(output, exact_limits);
    if (verified.width != image.width || verified.height != image.height
        || verified.pixels != image.pixels) {
        invalidTexture("Deterministic PNG round-trip differs from RGBA input");
    }
    return output;
}

}  // namespace clip_worker::formats
