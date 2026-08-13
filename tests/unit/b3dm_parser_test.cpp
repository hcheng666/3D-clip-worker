#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "support/synthetic_tile.hpp"

#include <gtest/gtest.h>

namespace clip_worker::formats {
namespace {

TEST(B3dmParserTest, ParsesMinimalEmbeddedGlb) {
    const auto bytes = clip_worker::tests::makeMinimalB3dm();

    const auto document = B3dmParser::parse(ByteView(bytes));

    EXPECT_EQ(document.header.version, B3dmParser::kSupportedVersion);
    EXPECT_EQ(document.header.byte_length, bytes.size());
    EXPECT_EQ(document.batch_length, 1U);
    EXPECT_EQ(document.glb.header.version, GlbParser::kSupportedVersion);
    EXPECT_EQ(document.glb_section.offset % B3dmParser::kGlbAlignment, 0U);
    EXPECT_FALSE(document.layout.requiresCompatibility());
    EXPECT_EQ(document.layout.trailing_padding_bytes, 0U);
}

TEST(B3dmParserTest, AcceptsFourByteAlignedEmbeddedGlbWithDiagnostics) {
    const auto bytes = clip_worker::tests::makeCompatibleMisalignedB3dm();

    const auto document = B3dmParser::parse(ByteView(bytes));

    EXPECT_EQ(document.glb_section.offset % B3dmParser::kGlbAlignment,
              B3dmParser::kCompatibleAlignment);
    EXPECT_TRUE(document.layout.requiresCompatibility());
    EXPECT_FALSE(document.layout.glb_offset_standard_aligned);
    EXPECT_FALSE(document.layout.tile_length_standard_aligned);
    EXPECT_EQ(document.layout.glb_byte_length, document.glb.header.byte_length);
}

TEST(B3dmParserTest, ParsesExactGlbRangeBeforeBoundedZeroPadding) {
    auto bytes = clip_worker::tests::makeCompatibleMisalignedB3dm();
    bytes.insert(bytes.end(), B3dmParser::kCompatibleAlignment, 0U);
    clip_worker::tests::writeUint32(
            bytes, 8, static_cast<std::uint32_t>(bytes.size()));

    const auto document = B3dmParser::parse(ByteView(bytes));

    EXPECT_EQ(document.layout.trailing_padding_bytes,
              B3dmParser::kCompatibleAlignment);
    EXPECT_EQ(document.glb_section.byte_length, document.glb.header.byte_length);
    EXPECT_TRUE(document.layout.tile_length_standard_aligned);
}

TEST(B3dmParserTest, RejectsMissingBatchLength) {
    const auto bytes = clip_worker::tests::makeMinimalB3dm(R"({"RTC_CENTER":[0,0,0]})");

    try {
        static_cast<void>(B3dmParser::parse(ByteView(bytes)));
        FAIL() << "Expected a FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::invalid_feature_table);
    }
}

TEST(B3dmParserTest, RejectsMisalignedEmbeddedGlb) {
    auto bytes = clip_worker::tests::makeMinimalB3dm();
    const auto feature_length = bytes[12]
                                | (static_cast<std::uint32_t>(bytes[13]) << 8U)
                                | (static_cast<std::uint32_t>(bytes[14]) << 16U)
                                | (static_cast<std::uint32_t>(bytes[15]) << 24U);
    clip_worker::tests::writeUint32(bytes, 12, feature_length - 1U);

    try {
        static_cast<void>(B3dmParser::parse(ByteView(bytes)));
        FAIL() << "Expected a FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::invalid_alignment);
    }
}

TEST(B3dmParserTest, RejectsObjectLengthMismatch) {
    auto bytes = clip_worker::tests::makeMinimalB3dm();
    clip_worker::tests::writeUint32(bytes, 8, static_cast<std::uint32_t>(bytes.size() - 1U));

    try {
        static_cast<void>(B3dmParser::parse(ByteView(bytes)));
        FAIL() << "Expected a FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::length_mismatch);
    }
}

TEST(B3dmParserTest, RejectsNonZeroDataAfterDeclaredGlb) {
    auto bytes = clip_worker::tests::makeCompatibleMisalignedB3dm();
    bytes.insert(bytes.end(), B3dmParser::kCompatibleAlignment, 0U);
    bytes.back() = 1U;
    clip_worker::tests::writeUint32(
            bytes, 8, static_cast<std::uint32_t>(bytes.size()));

    try {
        static_cast<void>(B3dmParser::parse(ByteView(bytes)));
        FAIL() << "Expected a FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::length_mismatch);
    }
}

TEST(B3dmParserTest, RejectsExcessiveDataAfterDeclaredGlb) {
    auto bytes = clip_worker::tests::makeCompatibleMisalignedB3dm();
    bytes.insert(bytes.end(), B3dmParser::kGlbAlignment, 0U);
    clip_worker::tests::writeUint32(
            bytes, 8, static_cast<std::uint32_t>(bytes.size()));

    try {
        static_cast<void>(B3dmParser::parse(ByteView(bytes)));
        FAIL() << "Expected a FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::length_mismatch);
    }
}

TEST(B3dmParserTest, RejectsEmbeddedGlbLengthBeyondB3dm) {
    auto bytes = clip_worker::tests::makeCompatibleMisalignedB3dm();
    const auto document = B3dmParser::parse(ByteView(bytes));
    clip_worker::tests::writeUint32(
            bytes, document.glb_section.offset + 8U,
            static_cast<std::uint32_t>(document.glb_section.byte_length
                                       + B3dmParser::kCompatibleAlignment));

    try {
        static_cast<void>(B3dmParser::parse(ByteView(bytes)));
        FAIL() << "Expected a FormatError";
    } catch (const FormatError& error) {
        EXPECT_EQ(error.code(), FormatErrorCode::unexpected_end);
    }
}

}  // namespace
}  // namespace clip_worker::formats
