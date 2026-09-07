#include "clip_worker/clip/texture_masker.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::clip {
namespace {

constexpr std::uint32_t kImageWidth = 8U;
constexpr std::uint32_t kImageHeight = 8U;
constexpr std::uint8_t kRed = 17U;
constexpr std::uint8_t kGreen = 34U;
constexpr std::uint8_t kBlue = 51U;
constexpr std::uint8_t kAlpha = 255U;

mesh::MeshScene texturedScene(mesh::SamplerWrap wrap,
                              const std::vector<float>& texcoords_0,
                              const std::vector<float>& texcoords_1 = {}) {
    mesh::RgbaImage image;
    image.width = kImageWidth;
    image.height = kImageHeight;
    image.pixels.resize(static_cast<std::size_t>(kImageWidth)
                        * kImageHeight * 4U);
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(kImageWidth) * kImageHeight;
         ++pixel) {
        image.pixels[pixel * 4U] = kRed;
        image.pixels[pixel * 4U + 1U] = kGreen;
        image.pixels[pixel * 4U + 2U] = kBlue;
        image.pixels[pixel * 4U + 3U] = kAlpha;
    }

    mesh::MeshPrimitive primitive;
    primitive.positions = {
            0.0F, 0.0F, 0.0F,
            1.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F};
    primitive.texcoords_0 = texcoords_0;
    primitive.texcoords_1 = texcoords_1;
    primitive.indices = {0U, 1U, 2U};
    primitive.material = 0U;

    mesh::MeshMaterial material;
    material.base_color_texture = mesh::TextureBinding{0U, 0U};
    if (!texcoords_1.empty()) {
        material.emissive_texture = mesh::TextureBinding{0U, 1U};
    }

    mesh::MeshScene scene;
    scene.default_scene = 0U;
    scene.scenes = {{0U}};
    scene.nodes = {{0U, geometry::Matrix4::identity(), 0U, {}}};
    scene.meshes = {{{std::move(primitive)}, {}}};
    scene.materials = {std::move(material)};
    scene.samplers = {{wrap, wrap, {}, {}}};
    scene.textures = {{0U, 0U}};
    scene.images = {std::move(image)};
    return scene;
}

std::vector<std::uint8_t> retainedMask(const mesh::RgbaImage& image) {
    std::vector<std::uint8_t> result(
            static_cast<std::size_t>(image.width) * image.height, 0U);
    for (std::size_t pixel = 0U; pixel < result.size(); ++pixel) {
        if (image.pixels[pixel * 4U + 3U] != 0U) result[pixel] = 1U;
    }
    return result;
}

void expectClearedPixelsAreTransparentBlack(const mesh::RgbaImage& image) {
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(image.width) * image.height;
         ++pixel) {
        const std::size_t offset = pixel * 4U;
        if (image.pixels[offset + 3U] == 0U) {
            EXPECT_EQ(image.pixels[offset], 0U);
            EXPECT_EQ(image.pixels[offset + 1U], 0U);
            EXPECT_EQ(image.pixels[offset + 2U], 0U);
        }
    }
}

TEST(TextureMaskerTest, ClearsAllRgbaChannelsOutsideRetainedUvCoverage) {
    auto scene = texturedScene(
            mesh::SamplerWrap::clamp_to_edge,
            {0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.5F});

    const auto statistics = TextureMasker::mask(scene);

    EXPECT_GT(statistics.retained_pixels, 0U);
    EXPECT_GT(statistics.cleared_pixels, 0U);
    EXPECT_EQ(statistics.retained_pixels + statistics.cleared_pixels,
              static_cast<std::uint64_t>(kImageWidth) * kImageHeight);
    expectClearedPixelsAreTransparentBlack(scene.images.front());
}

TEST(TextureMaskerTest, RepeatAndMirroredRepeatUseDeterministicInverseWrapping) {
    const std::vector<float> base_uv{
            0.0F, 0.0F, 0.75F, 0.0F, 0.0F, 0.5F};
    auto repeated_base = texturedScene(mesh::SamplerWrap::repeat, base_uv);
    auto repeated_shift = texturedScene(
            mesh::SamplerWrap::repeat,
            {1.0F, 0.0F, 1.75F, 0.0F, 1.0F, 0.5F});
    static_cast<void>(TextureMasker::mask(repeated_base));
    static_cast<void>(TextureMasker::mask(repeated_shift));
    EXPECT_EQ(retainedMask(repeated_base.images.front()),
              retainedMask(repeated_shift.images.front()));

    auto mirrored_base = texturedScene(
            mesh::SamplerWrap::mirrored_repeat, base_uv);
    auto mirrored_shift = texturedScene(
            mesh::SamplerWrap::mirrored_repeat,
            {1.0F, 0.0F, 1.75F, 0.0F, 1.0F, 0.5F});
    static_cast<void>(TextureMasker::mask(mirrored_base));
    static_cast<void>(TextureMasker::mask(mirrored_shift));
    const auto base_mask = retainedMask(mirrored_base.images.front());
    const auto shifted_mask = retainedMask(mirrored_shift.images.front());
    for (std::uint32_t row = 0U; row < kImageHeight; ++row) {
        for (std::uint32_t column = 0U; column < kImageWidth; ++column) {
            EXPECT_EQ(base_mask[static_cast<std::size_t>(row) * kImageWidth
                                + column],
                      shifted_mask[static_cast<std::size_t>(row) * kImageWidth
                                   + (kImageWidth - column - 1U)]);
        }
    }
}

TEST(TextureMaskerTest, UnionsAllMaterialSlotsAndTextureCoordinateSets) {
    auto base_only = texturedScene(
            mesh::SamplerWrap::clamp_to_edge,
            {0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.5F});
    const auto base_statistics = TextureMasker::mask(base_only);

    auto union_scene = texturedScene(
            mesh::SamplerWrap::clamp_to_edge,
            {0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.5F},
            {1.0F, 1.0F, 0.5F, 1.0F, 1.0F, 0.5F});
    const auto union_statistics = TextureMasker::mask(union_scene);

    EXPECT_GT(union_statistics.retained_pixels,
              base_statistics.retained_pixels);
    expectClearedPixelsAreTransparentBlack(union_scene.images.front());
}

TEST(TextureMaskerTest, RejectsRepeatSpansAndRasterWorkOverConfiguredLimits) {
    auto repeat_scene = texturedScene(
            mesh::SamplerWrap::repeat,
            {-100.0F, 0.0F, 100.0F, 0.0F, 0.0F, 1.0F});
    TextureMaskLimits repeat_limits;
    repeat_limits.maximum_repeat_span = 16U;
    EXPECT_THROW(static_cast<void>(TextureMasker::mask(
                         repeat_scene, repeat_limits)),
                 formats::FormatError);

    auto raster_scene = texturedScene(
            mesh::SamplerWrap::clamp_to_edge,
            {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F});
    TextureMaskLimits raster_limits;
    raster_limits.maximum_raster_tests = 1U;
    EXPECT_THROW(static_cast<void>(TextureMasker::mask(
                         raster_scene, raster_limits)),
                 formats::FormatError);
}

}  // namespace
}  // namespace clip_worker::clip
