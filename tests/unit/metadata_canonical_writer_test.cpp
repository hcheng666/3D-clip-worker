#include "clip_worker/normalization/metadata_canonical_writer.hpp"

#include "clip_worker/metadata/feature_metadata.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

metadata::MetadataResourceLimits limits() {
    metadata::MetadataResourceLimits value;
    value.maximum_property_tables = 16U;
    value.maximum_feature_rows = 1024U;
    value.maximum_properties = 32U;
    value.maximum_values_bytes = 1024U * 1024U;
    value.maximum_array_offset_bytes = 1024U * 1024U;
    value.maximum_string_offset_bytes = 1024U * 1024U;
    value.maximum_decoded_string_bytes = 1024U * 1024U;
    value.maximum_array_elements = 4096U;
    value.maximum_hierarchy_instances = 1024U;
    value.maximum_hierarchy_edges = 4096U;
    value.maximum_hierarchy_depth = 32U;
    value.maximum_ancestor_closure_entries = 4096U;
    value.maximum_feature_mapping_entries = 4096U;
    return value;
}

TEST(MetadataCanonicalWriterTest, WritesOnlyCompactedStringArrayBytes) {
    metadata::PropertyColumn labels;
    labels.name = "labels";
    labels.scalar_type = metadata::PropertyScalarType::string;
    labels.array_offsets = {0U, 1U, 2U, 3U};
    labels.values = metadata::StringValues{
            {"kept-a", "removed-canary", "kept-b"}};
    const auto compacted = metadata::compactPropertyTable(
            {"Feature", 3U, {labels}}, {0U, 2U}, limits());
    metadata::FeatureMetadata feature_metadata;
    feature_metadata.property_tables = {compacted};
    std::vector<std::vector<std::uint8_t>> views;
    const auto output = writeCanonicalMetadata(
            feature_metadata,
            [&views](const std::vector<std::uint8_t>& bytes,
                     std::size_t) {
                views.push_back(bytes);
                return views.size() - 1U;
            });

    EXPECT_EQ(output.structural_metadata.at("schema").at("id"),
              "justai-canonical-metadata-v1");
    std::vector<std::uint8_t> joined;
    for (const auto& view : views) {
        joined.insert(joined.end(), view.begin(), view.end());
    }
    const std::string binary(joined.begin(), joined.end());
    EXPECT_NE(binary.find("kept-a"), std::string::npos);
    EXPECT_NE(binary.find("kept-b"), std::string::npos);
    EXPECT_EQ(binary.find("removed-canary"), std::string::npos);
}

TEST(MetadataCanonicalWriterTest, SelectsLegalFeatureAccessorBytes) {
    metadata::FeatureIdSet byte_set;
    byte_set.feature_count = 255U;
    byte_set.null_feature_id = 255U;
    byte_set.ids = {0U, 254U, 255U};
    const auto byte_encoding = encodeCanonicalFeatureIds(byte_set);
    EXPECT_EQ(byte_encoding.component_type, 5121U);
    EXPECT_EQ(byte_encoding.bytes.size(), 3U);

    metadata::FeatureIdSet short_set;
    short_set.feature_count = 256U;
    short_set.null_feature_id = 256U;
    short_set.ids = {0U, 255U, 256U};
    const auto short_encoding = encodeCanonicalFeatureIds(short_set);
    EXPECT_EQ(short_encoding.component_type, 5123U);
    EXPECT_EQ(short_encoding.bytes.size(), 6U);
}

TEST(MetadataCanonicalWriterTest, ClearsBooleanPaddingAndDropsNumericCanary) {
    metadata::PropertyColumn visible;
    visible.name = "visible";
    visible.scalar_type = metadata::PropertyScalarType::boolean;
    visible.values = metadata::BooleanValues{
            {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U}};
    metadata::PropertyColumn codes;
    codes.name = "code";
    codes.scalar_type = metadata::PropertyScalarType::uint32;
    constexpr std::uint32_t kRemovedBinaryCanary = 0xa1b2c3d4U;
    codes.values = std::vector<std::uint32_t>{
            7U, kRemovedBinaryCanary, 11U, 13U, 17U, 19U, 23U, 29U, 31U,
            37U};
    const auto compacted = metadata::compactPropertyTable(
            {"Feature", 10U, {visible, codes}}, {0U, 9U}, limits());
    metadata::FeatureMetadata feature_metadata;
    feature_metadata.property_tables = {compacted};
    std::vector<std::vector<std::uint8_t>> views;
    static_cast<void>(writeCanonicalMetadata(
            feature_metadata,
            [&views](const std::vector<std::uint8_t>& bytes,
                     std::size_t) {
                views.push_back(bytes);
                return views.size() - 1U;
            }));

    ASSERT_EQ(views.size(), 2U);
    const auto boolean_view = std::find_if(
            views.begin(), views.end(), [](const auto& bytes) {
                return bytes.size() == 1U;
            });
    ASSERT_NE(boolean_view, views.end());
    EXPECT_EQ(boolean_view->front(), 0x03U);
    std::vector<std::uint8_t> joined;
    for (const auto& view : views) {
        joined.insert(joined.end(), view.begin(), view.end());
    }
    const std::vector<std::uint8_t> canary_bytes{0xd4U, 0xc3U, 0xb2U, 0xa1U};
    EXPECT_EQ(std::search(joined.begin(), joined.end(),
                          canary_bytes.begin(), canary_bytes.end()),
              joined.end());
}

}  // namespace
}  // namespace clip_worker::normalization
