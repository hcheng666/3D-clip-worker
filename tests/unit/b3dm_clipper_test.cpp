#include "clip_worker/clip/b3dm_clipper.hpp"

#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "support/synthetic_tile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <webp/decode.h>

namespace clip_worker::clip {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kB3dmByteLengthOffset = 8U;
constexpr std::size_t kFeatureTableBinaryLengthOffset = 16U;
constexpr std::size_t kBatchTableBinaryLengthOffset = 24U;
constexpr std::size_t kSyntheticBinaryTableLength = 8U;

struct ParsedOutput {
    formats::B3dmDocument document;
    Json json;
    const std::uint8_t* binary = nullptr;
};

ParsedOutput parseOutput(const std::vector<std::uint8_t>& bytes) {
    ParsedOutput result;
    result.document = formats::B3dmParser::parse(formats::ByteView(bytes));
    result.json = Json::parse(result.document.glb.json_text);
    result.binary = bytes.data() + result.document.glb_section.offset
                    + result.document.glb.binary_offset;
    return result;
}

std::vector<std::uint8_t> makeB3dmWithBinaryTable(
        std::size_t binary_length_header_offset) {
    auto bytes = tests::makeMinimalB3dm();
    const auto document = formats::B3dmParser::parse(formats::ByteView(bytes));
    bytes.insert(bytes.begin()
                         + static_cast<std::ptrdiff_t>(document.glb_section.offset),
                 kSyntheticBinaryTableLength, 0U);
    tests::writeUint32(bytes, binary_length_header_offset,
                       static_cast<std::uint32_t>(kSyntheticBinaryTableLength));
    tests::writeUint32(bytes, kB3dmByteLengthOffset,
                       static_cast<std::uint32_t>(bytes.size()));
    return bytes;
}

std::vector<float> positionValues(const ParsedOutput& output) {
    const std::size_t accessor_index = output.json.at("meshes").at(0U)
            .at("primitives").at(0U).at("attributes").at("POSITION")
            .get<std::size_t>();
    const auto& accessor = output.json.at("accessors").at(accessor_index);
    const auto& view = output.json.at("bufferViews").at(
            accessor.at("bufferView").get<std::size_t>());
    const std::size_t offset = view.value("byteOffset", 0U)
                               + accessor.value("byteOffset", 0U);
    const std::size_t value_count = accessor.at("count").get<std::size_t>() * 3U;
    std::vector<float> values(value_count);
    std::memcpy(values.data(), output.binary + offset,
                values.size() * sizeof(float));
    return values;
}

std::vector<std::uint8_t> imageBytes(const ParsedOutput& output) {
    const auto& image = output.json.at("images").at(0U);
    const auto& view = output.json.at("bufferViews").at(
            image.at("bufferView").get<std::size_t>());
    const std::size_t offset = view.value("byteOffset", 0U);
    const std::size_t length = view.at("byteLength").get<std::size_t>();
    return std::vector<std::uint8_t>(output.binary + offset,
                                     output.binary + offset + length);
}

TEST(B3dmClipperTest, ClipsGeometryMasksWebpAndRemovesUnusedMetadata) {
    const auto fixture = tests::makeTexturedMeshFixture();

    const B3dmClipResult clipped = B3dmClipper::clip(fixture.b3dm, fixture.task);

    ASSERT_FALSE(clipped.empty);
    EXPECT_EQ(clipped.statistics.vertex_count_before, 3U);
    EXPECT_EQ(clipped.statistics.triangle_count_before, 1U);
    EXPECT_GT(clipped.statistics.triangle_count_after, 0U);
    EXPECT_GT(clipped.statistics.texture_bytes_before, 0U);
    EXPECT_GT(clipped.statistics.texture_bytes_after, 0U);

    const ParsedOutput output = parseOutput(clipped.bytes);
    ASSERT_EQ(output.json.at("scene"), 0U);
    ASSERT_EQ(output.json.at("scenes").size(), 1U);
    EXPECT_FALSE(output.json.at("scenes").at(0U).contains("name"));
    ASSERT_EQ(output.json.at("nodes").size(), 1U);
    EXPECT_FALSE(output.json.at("nodes").at(0U).contains("name"));
    EXPECT_EQ(output.json.at("extensionsRequired"),
              Json::array({"EXT_texture_webp"}));

    const auto batch_table = Json::parse(output.document.batch_table_json_text);
    ASSERT_EQ(batch_table.size(), 1U);
    EXPECT_EQ(batch_table.at("name"), Json::array({"synthetic-feature"}));

    const auto positions = positionValues(output);
    ASSERT_FALSE(positions.empty());
    for (std::size_t offset = 0; offset < positions.size(); offset += 3U) {
        EXPECT_LE(std::abs(positions[offset]), 5.0F);
        EXPECT_LE(std::abs(positions[offset + 1U]), 5.7F);
    }

    const auto encoded_image = imageBytes(output);
    int width = 0;
    int height = 0;
    std::uint8_t* decoded = WebPDecodeRGBA(encoded_image.data(), encoded_image.size(),
                                            &width, &height);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(width, 8);
    ASSERT_EQ(height, 8);
    bool found_cleared_pixel = false;
    bool found_retained_pixel = false;
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width * height);
         ++pixel) {
        const std::size_t offset = pixel * 4U;
        const bool cleared = decoded[offset] == 0U && decoded[offset + 1U] == 0U
                             && decoded[offset + 2U] == 0U
                             && decoded[offset + 3U] == 0U;
        found_cleared_pixel = found_cleared_pixel || cleared;
        found_retained_pixel = found_retained_pixel || decoded[offset + 3U] != 0U;
        if (decoded[offset + 3U] == 0U) {
            EXPECT_EQ(decoded[offset], 0U);
            EXPECT_EQ(decoded[offset + 1U], 0U);
            EXPECT_EQ(decoded[offset + 2U], 0U);
        }
    }
    WebPFree(decoded);
    EXPECT_TRUE(found_cleared_pixel);
    EXPECT_TRUE(found_retained_pixel);
}

TEST(B3dmClipperTest, NormalizesCompatibleSourceToEightByteAlignedOutput) {
    auto fixture = tests::makeTexturedMeshFixture();
    tests::makeB3dmLayoutCompatibleButNonconforming(fixture.b3dm);

    const B3dmClipResult clipped = B3dmClipper::clip(fixture.b3dm, fixture.task);

    ASSERT_FALSE(clipped.empty);
    EXPECT_TRUE(clipped.source_layout.requiresCompatibility());
    const auto output = formats::B3dmParser::parse(formats::ByteView(clipped.bytes));
    EXPECT_FALSE(output.layout.requiresCompatibility());
    EXPECT_EQ(output.layout.glb_offset % formats::B3dmParser::kGlbAlignment, 0U);
    EXPECT_EQ(clipped.bytes.size() % formats::B3dmParser::kGlbAlignment, 0U);
    EXPECT_EQ(output.glb_section.byte_length, output.glb.header.byte_length);
}

TEST(B3dmClipperTest, ProducesDeterministicBytesForRepeatedV8Input) {
    const auto fixture = tests::makeTexturedMeshFixture();

    const B3dmClipResult first = B3dmClipper::clip(fixture.b3dm, fixture.task);
    const B3dmClipResult second = B3dmClipper::clip(fixture.b3dm, fixture.task);

    EXPECT_EQ(first.empty, second.empty);
    EXPECT_EQ(first.bytes, second.bytes);
    EXPECT_EQ(first.statistics.triangle_count_after,
              second.statistics.triangle_count_after);
    EXPECT_EQ(first.statistics.texture_bytes_after,
              second.statistics.texture_bytes_after);
}

TEST(B3dmClipperTest, ClipsDracoAndWritesUncompressedZeroAndSingleBatchOutput) {
    for (const std::uint32_t batch_length : {0U, 1U}) {
        const auto fixture = tests::makeDracoTexturedMeshFixture(batch_length);

        const B3dmClipResult clipped = B3dmClipper::clip(fixture.b3dm, fixture.task);

        ASSERT_FALSE(clipped.empty);
        EXPECT_EQ(clipped.statistics.vertex_count_before, 3U);
        EXPECT_EQ(clipped.statistics.triangle_count_before, 1U);
        const ParsedOutput output = parseOutput(clipped.bytes);
        EXPECT_EQ(output.document.batch_length, batch_length);
        const auto& primitive = output.json.at("meshes").at(0U)
                .at("primitives").at(0U);
        EXPECT_FALSE(primitive.contains("extensions"));
        for (const auto& accessor : output.json.at("accessors")) {
            EXPECT_TRUE(accessor.contains("bufferView"));
        }
        if (output.json.contains("extensionsUsed")) {
            EXPECT_EQ(std::find(output.json.at("extensionsUsed").begin(),
                                output.json.at("extensionsUsed").end(),
                                "KHR_draco_mesh_compression"),
                      output.json.at("extensionsUsed").end());
        }
        if (batch_length == 0U) {
            EXPECT_TRUE(output.document.batch_table_json_text.empty());
        } else {
            EXPECT_EQ(Json::parse(output.document.batch_table_json_text).at("name"),
                      Json::array({"synthetic-feature"}));
        }
    }
}

TEST(B3dmClipperTest, AcceptsStaleDracoAccessorCountsAndReportsCompatibility) {
    constexpr std::uint32_t stale_vertex_count = 2U;
    constexpr std::uint32_t stale_index_count = 6U;
    const auto fixture = tests::makeDracoTexturedMeshFixture(
            1U, stale_vertex_count, stale_index_count);
    DracoCompatibilityDiagnostics diagnostics;

    const B3dmClipResult clipped = B3dmClipper::clip(
            fixture.b3dm, fixture.task, {}, {},
            [&diagnostics](const DracoCompatibilityDiagnostics& observed) {
                diagnostics = observed;
            });

    EXPECT_FALSE(clipped.empty);
    EXPECT_EQ(diagnostics.affected_primitive_count, 1U);
    EXPECT_EQ(diagnostics.affected_accessor_count, 2U);
    EXPECT_EQ(diagnostics.affected_index_count, 1U);
    EXPECT_EQ(diagnostics.maximum_declared_vertex_count, stale_vertex_count);
    EXPECT_EQ(diagnostics.maximum_decoded_point_count, 3U);
    EXPECT_EQ(diagnostics.maximum_declared_index_count, stale_index_count);
    EXPECT_EQ(diagnostics.maximum_decoded_index_count, 3U);
}

TEST(B3dmClipperTest, RejectsMalformedDracoPayloadWithAccessorDiagnostic) {
    auto fixture = tests::makeDracoTexturedMeshFixture();
    const auto input = parseOutput(fixture.b3dm);
    const auto& compressed_view = input.json.at("bufferViews").at(0U);
    const std::size_t compressed_offset = input.document.glb_section.offset
            + input.document.glb.binary_offset
            + compressed_view.value("byteOffset", 0U);
    const std::size_t compressed_length =
            compressed_view.at("byteLength").get<std::size_t>();
    ASSERT_GE(compressed_length, 8U);
    std::fill_n(fixture.b3dm.begin()
                        + static_cast<std::ptrdiff_t>(compressed_offset),
                8U, 0U);

    try {
        static_cast<void>(B3dmClipper::clip(fixture.b3dm, fixture.task));
        FAIL() << "Expected FormatError";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::invalid_accessor);
        EXPECT_NE(std::string(error.what()).find("Draco"), std::string::npos);
    }
}

TEST(B3dmClipperTest, RejectsMoreThanOneBatch) {
    const auto source = tests::makeMinimalB3dm(R"({"BATCH_LENGTH":2})");
    task::ClaimTask task;

    try {
        static_cast<void>(B3dmClipper::clip(source, task));
        FAIL() << "Expected FormatError";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::unsupported_content);
        EXPECT_NE(std::string(error.what()).find("BATCH_LENGTH"), std::string::npos);
    }
}

TEST(B3dmClipperTest, DistinguishesUnsupportedFeatureTableBinary) {
    const auto source = makeB3dmWithBinaryTable(
            kFeatureTableBinaryLengthOffset);

    try {
        static_cast<void>(B3dmClipper::clip(source, {}));
        FAIL() << "Expected FormatError";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::unsupported_content);
        EXPECT_NE(std::string(error.what()).find("Feature Table Binary"),
                  std::string::npos);
    }
}

TEST(B3dmClipperTest, DistinguishesUnsupportedBatchTableBinary) {
    const auto source = makeB3dmWithBinaryTable(
            kBatchTableBinaryLengthOffset);

    try {
        static_cast<void>(B3dmClipper::clip(source, {}));
        FAIL() << "Expected FormatError";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::unsupported_content);
        EXPECT_NE(std::string(error.what()).find("Batch Table Binary"),
                  std::string::npos);
    }
}

TEST(B3dmClipperTest, ClipsEquivalentYAndZUpWorldGeometry) {
    const auto z_fixture = tests::makeTexturedMeshFixture(
            "", false, "", task::GltfUpAxis::z);
    const auto y_fixture = tests::makeTexturedMeshFixture(
            "", false, "", task::GltfUpAxis::y);

    const auto z_clipped = B3dmClipper::clip(z_fixture.b3dm, z_fixture.task);
    const auto y_clipped = B3dmClipper::clip(y_fixture.b3dm, y_fixture.task);

    ASSERT_FALSE(z_clipped.empty);
    ASSERT_FALSE(y_clipped.empty);
    EXPECT_EQ(y_clipped.statistics.vertex_count_after,
              z_clipped.statistics.vertex_count_after);
    EXPECT_EQ(y_clipped.statistics.triangle_count_after,
              z_clipped.statistics.triangle_count_after);
    const ParsedOutput y_output = parseOutput(y_clipped.bytes);
    const ParsedOutput z_output = parseOutput(z_clipped.bytes);
    EXPECT_EQ(imageBytes(y_output), imageBytes(z_output));

    const auto y_positions = positionValues(y_output);
    const auto z_positions = positionValues(z_output);
    ASSERT_EQ(y_positions.size(), z_positions.size());
    for (std::size_t offset = 0U; offset < y_positions.size(); offset += 3U) {
        // Output remains in each source's local axis while representing equal world geometry.
        EXPECT_NEAR(y_positions[offset], z_positions[offset], 1.0e-5F);
        EXPECT_NEAR(y_positions[offset + 1U], z_positions[offset + 2U], 1.0e-5F);
        EXPECT_NEAR(y_positions[offset + 2U], -z_positions[offset + 1U], 1.0e-5F);
    }
}

TEST(B3dmClipperTest, RejectsUnknownDeclaredExtension) {
    const auto fixture = tests::makeTexturedMeshFixture("VENDOR_unknown_extension");

    try {
        static_cast<void>(B3dmClipper::clip(fixture.b3dm, fixture.task));
        FAIL() << "Expected FormatError";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::unsupported_content);
    }
}

TEST(B3dmClipperTest, ReportsCompatibleLayoutBeforeLaterSemanticFailure) {
    auto fixture = tests::makeTexturedMeshFixture("VENDOR_unknown_extension");
    tests::makeB3dmLayoutCompatibleButNonconforming(fixture.b3dm);
    std::size_t observer_calls = 0U;
    formats::B3dmLayoutDiagnostics observed;

    try {
        static_cast<void>(B3dmClipper::clip(
                fixture.b3dm, fixture.task,
                [&observer_calls, &observed](
                        const formats::B3dmLayoutDiagnostics& diagnostics) {
                    ++observer_calls;
                    observed = diagnostics;
                }));
        FAIL() << "Expected FormatError";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::unsupported_content);
    }

    EXPECT_EQ(observer_calls, 1U);
    EXPECT_TRUE(observed.requiresCompatibility());
}

TEST(B3dmClipperTest, AcceptsKnownWrapRValuesAndRemovesThemFromOutput) {
    const std::vector<std::uint32_t> supported_values = {33071U, 33648U, 10497U};
    const auto baseline_fixture = tests::makeTexturedMeshFixture();
    const auto baseline = B3dmClipper::clip(
            baseline_fixture.b3dm, baseline_fixture.task);
    const auto baseline_image = imageBytes(parseOutput(baseline.bytes));

    for (const std::uint32_t value : supported_values) {
        const auto fixture = tests::makeTexturedMeshFixture(
                "", false,
                "{\"magFilter\":9729,\"minFilter\":9729,\"wrapR\":"
                        + std::to_string(value) + "}");
        std::size_t observer_calls = 0U;
        SamplerCompatibilityDiagnostics observed;

        const auto clipped = B3dmClipper::clip(
                fixture.b3dm, fixture.task, {},
                [&observer_calls, &observed](
                        const SamplerCompatibilityDiagnostics& diagnostics) {
                    ++observer_calls;
                    observed = diagnostics;
                });

        const auto output = parseOutput(clipped.bytes);
        ASSERT_EQ(output.json.at("samplers").size(), 1U);
        EXPECT_FALSE(output.json.at("samplers").at(0U).contains("wrapR"));
        EXPECT_EQ(imageBytes(output), baseline_image);
        EXPECT_EQ(observer_calls, 1U);
        EXPECT_EQ(observed.affected_sampler_count, 1U);
        EXPECT_EQ(observed.wrap_r_values, std::vector<std::uint32_t>({value}));
    }
}

class InvalidWrapRTest : public testing::TestWithParam<std::string> {};

TEST_P(InvalidWrapRTest, RejectsInvalidWrapR) {
    const auto fixture = tests::makeTexturedMeshFixture(
            "", false, "{\"wrapR\":" + GetParam() + "}");

    try {
        static_cast<void>(B3dmClipper::clip(fixture.b3dm, fixture.task));
        FAIL() << "Expected FormatError";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::unsupported_content);
    }
}

INSTANTIATE_TEST_SUITE_P(
        InvalidValues, InvalidWrapRTest,
        testing::Values("-1", "10497.5", "\"10497\"", "true", "null",
                        "{}", "[]", "1"));

TEST(B3dmClipperTest, KeepsWrapSTSafetyRulesWhileAcceptingWrapR) {
    const auto fixture = tests::makeTexturedMeshFixture(
            "", false,
            R"({"wrapR":33648,"wrapS":33648,"wrapT":33071})");

    EXPECT_THROW(static_cast<void>(B3dmClipper::clip(fixture.b3dm, fixture.task)),
                 formats::FormatError);
}

TEST(B3dmClipperTest, StillRejectsOtherUnknownSamplerFields) {
    const auto fixture = tests::makeTexturedMeshFixture(
            "", false, R"({"wrapR":10497,"vendorWrap":10497})");

    EXPECT_THROW(static_cast<void>(B3dmClipper::clip(fixture.b3dm, fixture.task)),
                 formats::FormatError);
}

TEST(B3dmClipperTest, ReportsWrapRBeforeLaterSemanticFailure) {
    const auto fixture = tests::makeTexturedMeshFixture(
            "VENDOR_unknown_extension", false, R"({"wrapR":10497})");
    std::size_t observer_calls = 0U;

    try {
        static_cast<void>(B3dmClipper::clip(
                fixture.b3dm, fixture.task, {},
                [&observer_calls](const SamplerCompatibilityDiagnostics&) {
                    ++observer_calls;
                }));
        FAIL() << "Expected FormatError";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(), formats::FormatErrorCode::unsupported_content);
    }

    EXPECT_EQ(observer_calls, 1U);
}

TEST(B3dmClipperTest, PreservesSupportedRtcCenter) {
    const auto fixture = tests::makeTexturedMeshFixture("", true);

    const auto clipped = B3dmClipper::clip(fixture.b3dm, fixture.task);

    const auto output = parseOutput(clipped.bytes);
    const auto feature_table = Json::parse(output.document.feature_table_json_text);
    EXPECT_EQ(feature_table.at("RTC_CENTER"), Json::array({0.0, 0.0, 0.0}));
}

TEST(B3dmClipperTest, RejectsUnsafeTaskOptions) {
    auto fixture = tests::makeTexturedMeshFixture();
    fixture.task.clip_options.mask_textures = false;

    EXPECT_THROW(static_cast<void>(B3dmClipper::clip(fixture.b3dm, fixture.task)),
                 formats::FormatError);
}

}  // namespace
}  // namespace clip_worker::clip
