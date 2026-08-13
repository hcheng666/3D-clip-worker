#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/glb.hpp"
#include "support/synthetic_tile.hpp"

#include <gtest/gtest.h>

namespace clip_worker::formats {
namespace {

TEST(GlbParserTest, ParsesMinimalVersionTwoDocument) {
    const auto bytes = clip_worker::tests::makeMinimalGlb();

    const auto document = GlbParser::parse(ByteView(bytes));

    EXPECT_EQ(document.header.version, GlbParser::kSupportedVersion);
    EXPECT_EQ(document.header.byte_length, bytes.size());
    ASSERT_EQ(document.chunks.size(), 1U);
    EXPECT_EQ(document.chunks.front().type, GlbParser::kJsonChunkType);
    EXPECT_EQ(document.binary_length, 0U);
}

TEST(GlbParserTest, RejectsDeclaredLengthMismatch) {
    auto bytes = clip_worker::tests::makeMinimalGlb();
    clip_worker::tests::writeUint32(bytes, 8, static_cast<std::uint32_t>(bytes.size() + 4U));

    try {
        static_cast<void>(GlbParser::parse(ByteView(bytes)));
        FAIL() << "Expected a FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::length_mismatch);
    }
}

TEST(GlbParserTest, RejectsNonJsonFirstChunk) {
    auto bytes = clip_worker::tests::makeMinimalGlb();
    clip_worker::tests::writeUint32(bytes, GlbParser::kHeaderSize + sizeof(std::uint32_t),
                                    GlbParser::kBinaryChunkType);

    try {
        static_cast<void>(GlbParser::parse(ByteView(bytes)));
        FAIL() << "Expected a FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::invalid_chunk_order);
    }
}

}  // namespace
}  // namespace clip_worker::formats

