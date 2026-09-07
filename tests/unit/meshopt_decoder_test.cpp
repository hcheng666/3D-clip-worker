#include "clip_worker/formats/meshopt_decoder.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <array>
#include <cstring>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>
#include <meshoptimizer.h>

namespace clip_worker::formats {
namespace {

TEST(MeshoptDecoderTest, DecodesBoundedVertexAndIndexPayloads) {
    const std::array<float, 9U> positions{
            0.0F, 0.0F, 0.0F,
            1.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F};
    std::vector<std::uint8_t> encoded_vertices(
            meshopt_encodeVertexBufferBound(3U, sizeof(float) * 3U));
    const std::size_t vertex_bytes = meshopt_encodeVertexBuffer(
            encoded_vertices.data(), encoded_vertices.size(), positions.data(),
            3U, sizeof(float) * 3U);
    encoded_vertices.resize(vertex_bytes);

    const auto decoded_vertices = MeshoptDecoder::decode(
            ByteView(encoded_vertices), 3U, sizeof(float) * 3U,
            MeshoptMode::attributes, MeshoptFilter::none);
    EXPECT_EQ(decoded_vertices.size(), sizeof(positions));
    EXPECT_EQ(std::memcmp(decoded_vertices.data(), positions.data(),
                          sizeof(positions)), 0);

    const std::array<std::uint32_t, 3U> indices{0U, 1U, 2U};
    std::vector<std::uint8_t> encoded_indices(
            meshopt_encodeIndexBufferBound(indices.size(), 3U));
    const std::size_t index_bytes = meshopt_encodeIndexBuffer(
            encoded_indices.data(), encoded_indices.size(), indices.data(),
            indices.size());
    encoded_indices.resize(index_bytes);
    const auto decoded_indices = MeshoptDecoder::decode(
            ByteView(encoded_indices), indices.size(), sizeof(std::uint32_t),
            MeshoptMode::triangles, MeshoptFilter::none);
    EXPECT_EQ(std::memcmp(decoded_indices.data(), indices.data(),
                          sizeof(indices)), 0);
}

TEST(MeshoptDecoderTest, RejectsCorruptionUnsupportedCombinationsAndLimits) {
    EXPECT_THROW(MeshoptDecoder::decode(
                         ByteView(std::vector<std::uint8_t>{1U, 2U, 3U}),
                         3U, 4U, MeshoptMode::triangles,
                         MeshoptFilter::octahedral),
                 FormatError);

    MeshoptDecodeLimits limits;
    limits.maximum_decoded_bytes = 4U;
    EXPECT_THROW(MeshoptDecoder::decode(
                         ByteView(std::vector<std::uint8_t>{1U}),
                         3U, 4U, MeshoptMode::attributes,
                         MeshoptFilter::none, limits),
                 FormatError);
}

TEST(MeshoptDecoderTest, DecodesAllSupportedAttributeFilters) {
    const std::array<float, 4U> normal{0.0F, 0.0F, 1.0F, 1.0F};
    std::array<std::uint8_t, 4U> oct{};
    meshopt_encodeFilterOct(oct.data(), 1U, oct.size(), 8, normal.data());
    std::vector<std::uint8_t> encoded_oct(
            meshopt_encodeVertexBufferBound(1U, oct.size()));
    encoded_oct.resize(meshopt_encodeVertexBuffer(
            encoded_oct.data(), encoded_oct.size(), oct.data(), 1U, oct.size()));
    EXPECT_NO_THROW(MeshoptDecoder::decode(
            ByteView(encoded_oct), 1U, oct.size(), MeshoptMode::attributes,
            MeshoptFilter::octahedral));

    const std::array<float, 4U> quaternion{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<std::uint8_t, 8U> quat{};
    meshopt_encodeFilterQuat(quat.data(), 1U, quat.size(), 12,
                             quaternion.data());
    std::vector<std::uint8_t> encoded_quat(
            meshopt_encodeVertexBufferBound(1U, quat.size()));
    encoded_quat.resize(meshopt_encodeVertexBuffer(
            encoded_quat.data(), encoded_quat.size(), quat.data(), 1U,
            quat.size()));
    EXPECT_NO_THROW(MeshoptDecoder::decode(
            ByteView(encoded_quat), 1U, quat.size(), MeshoptMode::attributes,
            MeshoptFilter::quaternion));

    const std::array<float, 4U> exponential{1.0F, -2.0F, 0.5F, 8.0F};
    std::array<std::uint8_t, sizeof(exponential)> exp{};
    meshopt_encodeFilterExp(exp.data(), 1U, exp.size(), 20,
                            exponential.data(), meshopt_EncodeExpSeparate);
    std::vector<std::uint8_t> encoded_exp(
            meshopt_encodeVertexBufferBound(1U, exp.size()));
    encoded_exp.resize(meshopt_encodeVertexBuffer(
            encoded_exp.data(), encoded_exp.size(), exp.data(), 1U, exp.size()));
    EXPECT_NO_THROW(MeshoptDecoder::decode(
            ByteView(encoded_exp), 1U, exp.size(), MeshoptMode::attributes,
            MeshoptFilter::exponential));
}

}  // namespace
}  // namespace clip_worker::formats
