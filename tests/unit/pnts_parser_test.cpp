#include "clip_worker/formats/pnts.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "support/synthetic_pnts.hpp"

#include <gtest/gtest.h>

namespace clip_worker::formats {
namespace {

TEST(PntsParserTest, ParsesStrictVersionOneSections) {
    const auto fixture = tests::makeQuantizedPntsFixture();
    const auto document = PntsParser::parse(ByteView(fixture.bytes));

    EXPECT_EQ(document.version, 1U);
    EXPECT_EQ(document.byte_length, fixture.bytes.size());
    EXPECT_EQ(document.feature_table_json_text, fixture.feature_json);
    EXPECT_GT(document.feature_table_binary.byte_length, 0U);
    EXPECT_GT(document.batch_table_json.byte_length, 0U);
}

TEST(PntsParserTest, RejectsDeclaredLengthMismatch) {
    auto fixture = tests::makeQuantizedPntsFixture();
    fixture.bytes[8U] = 0U;
    EXPECT_THROW(PntsParser::parse(ByteView(fixture.bytes)), FormatError);
}

}  // namespace
}  // namespace clip_worker::formats
