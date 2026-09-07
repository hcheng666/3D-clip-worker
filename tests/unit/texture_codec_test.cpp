#include "clip_worker/formats/texture_codec.hpp"

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/formats/format_error.hpp"

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include <ktx.h>
#include <turbojpeg.h>
#include <webp/encode.h>

namespace clip_worker::formats {
namespace {

// Vulkan 1.0 VkFormat value used by KTX2 for linear RGBA8 source pixels.
constexpr std::uint32_t kVkFormatR8G8B8A8Unorm = 37U;

mesh::RgbaImage testImage() {
    return {2U, 2U,
            {255U, 0U, 0U, 255U, 0U, 255U, 0U, 128U,
             0U, 0U, 255U, 64U, 255U, 255U, 255U, 0U}};
}

std::vector<std::uint8_t> basisKtx2() {
    constexpr std::uint32_t kWidth = 4U;
    constexpr std::uint32_t kHeight = 4U;
    std::vector<std::uint8_t> rgba(kWidth * kHeight * 4U, 255U);
    for (std::size_t pixel = 0U; pixel < kWidth * kHeight; ++pixel) {
        rgba[pixel * 4U] = static_cast<std::uint8_t>(pixel * 11U);
        rgba[pixel * 4U + 1U] = static_cast<std::uint8_t>(255U - pixel * 7U);
        rgba[pixel * 4U + 2U] = 96U;
    }
    ktxTextureCreateInfo info{};
    info.vkFormat = kVkFormatR8G8B8A8Unorm;
    info.baseWidth = kWidth;
    info.baseHeight = kHeight;
    info.baseDepth = 1U;
    info.numDimensions = 2U;
    info.numLevels = 1U;
    info.numLayers = 1U;
    info.numFaces = 1U;
    ktxTexture2* raw = nullptr;
    if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &raw)
        != KTX_SUCCESS || raw == nullptr) {
        throw std::runtime_error("Unable to create KTX2 fixture");
    }
    struct Cleanup {
        ktxTexture2* value;
        ~Cleanup() { ktxTexture_Destroy(ktxTexture(value)); }
    } cleanup{raw};
    if (ktxTexture_SetImageFromMemory(ktxTexture(raw), 0U, 0U, 0U,
                                      rgba.data(), rgba.size()) != KTX_SUCCESS
        || ktxTexture2_CompressBasis(raw, 128U) != KTX_SUCCESS) {
        throw std::runtime_error("Unable to encode BasisU KTX2 fixture");
    }
    ktx_uint8_t* encoded = nullptr;
    ktx_size_t encoded_size = 0U;
    if (ktxTexture_WriteToMemory(ktxTexture(raw), &encoded, &encoded_size)
        != KTX_SUCCESS || encoded == nullptr || encoded_size == 0U) {
        throw std::runtime_error("Unable to serialize BasisU KTX2 fixture");
    }
    std::unique_ptr<ktx_uint8_t, decltype(&std::free)> memory(encoded, &std::free);
    return {memory.get(), memory.get() + encoded_size};
}

TEST(TextureCodecTest, EncodesDeterministicPngAndRoundTripsExactRgba) {
    const auto image = testImage();

    const auto first = TextureCodec::encodeDeterministicPng(image);
    const auto second = TextureCodec::encodeDeterministicPng(image);

    EXPECT_EQ(first, second);
    EXPECT_EQ(TextureCodec::decode(first, TextureFormat::png).pixels,
              image.pixels);
    EXPECT_EQ(client::sha256Hex(first),
              "639f6f8d8fb03448feda2f0e8e291a09f710b543ec07f1c5e0103451aa4bea37");
}

TEST(TextureCodecTest, DecodesLosslessWebpToTheSharedRgbaModel) {
    const auto image = testImage();
    std::uint8_t* raw = nullptr;
    const std::size_t size = WebPEncodeLosslessRGBA(
            image.pixels.data(), static_cast<int>(image.width),
            static_cast<int>(image.height), static_cast<int>(image.width * 4U),
            &raw);
    ASSERT_GT(size, 0U);
    std::unique_ptr<std::uint8_t, decltype(&WebPFree)> encoded(raw, &WebPFree);
    const std::vector<std::uint8_t> bytes(encoded.get(), encoded.get() + size);

    auto expected = image.pixels;
    expected[12U] = 0U;
    expected[13U] = 0U;
    expected[14U] = 0U;
    EXPECT_EQ(TextureCodec::decode(bytes, TextureFormat::webp).pixels,
              expected);
}

TEST(TextureCodecTest, DecodesJpegAndAppliesDimensionLimitsBeforeRgbaAllocation) {
    const auto image = testImage();
    using TurboHandle = std::unique_ptr<void, decltype(&tj3Destroy)>;
    TurboHandle compressor(tj3Init(TJINIT_COMPRESS), &tj3Destroy);
    ASSERT_NE(compressor, nullptr);
    ASSERT_EQ(tj3Set(compressor.get(), TJPARAM_QUALITY, 100), 0);
    ASSERT_EQ(tj3Set(compressor.get(), TJPARAM_SUBSAMP, TJSAMP_444), 0);
    unsigned char* raw = nullptr;
    std::size_t size = 0U;
    ASSERT_EQ(tj3Compress8(compressor.get(), image.pixels.data(),
                          static_cast<int>(image.width), 0,
                          static_cast<int>(image.height), TJPF_RGBA,
                          &raw, &size), 0);
    std::unique_ptr<unsigned char, decltype(&tj3Free)> encoded(raw, &tj3Free);
    const std::vector<std::uint8_t> bytes(encoded.get(), encoded.get() + size);

    const auto decoded = TextureCodec::decode(bytes, TextureFormat::jpeg);
    EXPECT_EQ(decoded.width, image.width);
    EXPECT_EQ(decoded.height, image.height);

    TextureDecodeLimits limits;
    limits.maximum_width = 1U;
    try {
        static_cast<void>(TextureCodec::decode(
                bytes, TextureFormat::jpeg, limits));
        FAIL() << "Expected FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(),
                  FormatErrorCode::texture_dimension_limit_exceeded);
    }

    limits.maximum_width = image.width;
    limits.maximum_rgba_bytes = 1U;
    try {
        static_cast<void>(TextureCodec::decode(
                bytes, TextureFormat::jpeg, limits));
        FAIL() << "Expected FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(),
                  FormatErrorCode::texture_decoded_bytes_limit_exceeded);
    }
}

TEST(TextureCodecTest, RejectsInvalidAndUnsupportedTextureInputs) {
    try {
        static_cast<void>(TextureCodec::decode(
                {1U, 2U, 3U}, TextureFormat::png));
        FAIL() << "Expected FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::texture_invalid);
    }
    try {
        static_cast<void>(TextureCodec::formatForMimeType("image/gif"));
        FAIL() << "Expected FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(),
                  FormatErrorCode::texture_format_unsupported);
    }
}

TEST(TextureCodecTest, DecodesSingleTwoDimensionalBasisKtx2) {
    const auto decoded = TextureCodec::decode(
            basisKtx2(), TextureFormat::ktx2_basis);
    EXPECT_EQ(decoded.width, 4U);
    EXPECT_EQ(decoded.height, 4U);
    EXPECT_EQ(decoded.pixels.size(), 4U * 4U * 4U);
}

}  // namespace
}  // namespace clip_worker::formats
