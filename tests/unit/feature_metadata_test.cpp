#include "clip_worker/metadata/feature_metadata.hpp"

#include "clip_worker/metadata/property_lookup.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <gtest/gtest.h>

namespace clip_worker::metadata {
namespace {

MetadataResourceLimits limits() {
    MetadataResourceLimits value;
    value.maximum_property_tables = 16U;
    value.maximum_feature_rows = 1024U;
    value.maximum_properties = 16U;
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

TEST(FeatureMetadataTest, DenseMappingExcludesNullAndRewritesIds) {
    const auto mapping = buildDenseFeatureMapping(
            8U, {7U, 2U, 7U, 4U, 99U}, 99U, limits());
    EXPECT_EQ(mapping.retained_source_ids,
              (std::vector<std::uint32_t>{2U, 4U, 7U}));
    EXPECT_EQ(rewriteFeatureIds({7U, 99U, 2U}, mapping, 99U, 3U),
              (std::vector<std::uint32_t>{2U, 3U, 0U}));
}

TEST(FeatureMetadataTest, CompactsVariableStringArraysWithoutRemovedCanary) {
    PropertyColumn labels;
    labels.name = "labels";
    labels.scalar_type = PropertyScalarType::string;
    labels.array_offsets = {0U, 2U, 3U, 5U};
    labels.values = StringValues{{"keep-a", "keep-b", "removed-canary",
                                  "keep-c", "keep-d"}};
    PropertyTable table{"Feature", 3U, {labels}};
    const auto compacted = compactPropertyTable(table, {0U, 2U}, limits());
    EXPECT_EQ(compacted.row_count, 2U);
    EXPECT_EQ(compacted.columns.front().array_offsets,
              (std::vector<std::uint32_t>{0U, 2U, 4U}));
    EXPECT_EQ(std::get<StringValues>(compacted.columns.front().values).values,
              (std::vector<std::string>{"keep-a", "keep-b", "keep-c",
                                        "keep-d"}));
}

TEST(FeatureMetadataTest, RetainsHierarchyAncestorClosureAndRewritesRows) {
    PropertyColumn names;
    names.name = "name";
    names.scalar_type = PropertyScalarType::string;
    names.values = StringValues{{"root", "removed-parent", "kept-child"}};
    PropertyTable table{"Node", 3U, {names}};
    PropertyHierarchy hierarchy;
    hierarchy.nodes = {{0U, 0U, {}}, {0U, 1U, {0U}}, {0U, 2U, {0U}}};
    hierarchy.feature_nodes = {1U, 2U};
    const auto compacted = compactPropertyHierarchy(
            {table}, hierarchy, {1U}, limits());
    ASSERT_EQ(compacted.hierarchy.nodes.size(), 2U);
    EXPECT_EQ(compacted.hierarchy.feature_nodes,
              (std::vector<std::uint32_t>{0U}));
    EXPECT_EQ(std::get<StringValues>(
                      compacted.property_tables.front().columns.front().values)
                      .values,
              (std::vector<std::string>{"kept-child", "root"}));
}

TEST(FeatureMetadataTest, RejectsHierarchyCycle) {
    PropertyColumn values;
    values.name = "id";
    values.scalar_type = PropertyScalarType::uint32;
    values.values = std::vector<std::uint32_t>{0U, 1U};
    PropertyTable table{"Node", 2U, {values}};
    PropertyHierarchy hierarchy{{{0U, 0U, {1U}}, {0U, 1U, {0U}}}, {0U}};
    try {
        static_cast<void>(compactPropertyHierarchy(
                {table}, hierarchy, {0U}, limits()));
        FAIL() << "cyclic hierarchy must fail closed";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(),
                  formats::FormatErrorCode::metadata_hierarchy_invalid);
    }
}

TEST(FeatureMetadataTest, SelectsSpecLegalFeatureIdComponents) {
    EXPECT_EQ(selectFeatureIdEncoding(255U, true).component_type,
              FeatureIdComponentType::unsigned_byte);
    EXPECT_EQ(selectFeatureIdEncoding(256U, true).component_type,
              FeatureIdComponentType::unsigned_short);
    EXPECT_EQ(selectFeatureIdEncoding(65536U, true).component_type,
              FeatureIdComponentType::float32);
}

TEST(FeatureMetadataTest, PropertyLookupMatchesDenseRowsAndRejectsNullIds) {
    PropertyColumn names;
    names.name = "name";
    names.scalar_type = PropertyScalarType::string;
    names.values = StringValues{{"kept-a", "removed-canary", "kept-b"}};
    FeatureMetadata source;
    source.property_tables = {{"Feature", 3U, {names}}};

    const auto expected = lookupPropertyMaterial(source, 0U, 2U);
    const auto removed = lookupPropertyMaterial(source, 0U, 1U);
    const auto compacted = compactPropertyTable(
            source.property_tables.front(), {0U, 2U}, limits());
    FeatureMetadata output;
    output.property_tables = {compacted};

    ASSERT_TRUE(expected.has_value());
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(lookupPropertyMaterial(output, 0U, 1U), expected);
    EXPECT_NE(lookupPropertyMaterial(output, 0U, 1U), removed);
    EXPECT_FALSE(lookupPropertyMaterial(output, 0U, 2U, 2U).has_value());
    EXPECT_FALSE(lookupPropertyMaterial(output, 0U, 99U).has_value());
    EXPECT_FALSE(lookupPropertyMaterial(output, std::nullopt, 0U).has_value());
}

TEST(FeatureMetadataTest, PropertyLookupPreservesHierarchyAncestorClosure) {
    PropertyColumn direct;
    direct.name = "direct";
    direct.scalar_type = PropertyScalarType::string;
    direct.values = StringValues{{"removed", "kept"}};
    PropertyColumn leaf;
    leaf.name = "leaf";
    leaf.scalar_type = PropertyScalarType::string;
    leaf.values = StringValues{{"removed-leaf", "kept-leaf"}};
    PropertyColumn parent;
    parent.name = "parent";
    parent.scalar_type = PropertyScalarType::string;
    parent.values = StringValues{{"shared-parent"}};
    FeatureMetadata source;
    source.property_tables = {
            {"Feature", 2U, {direct}}, {"Leaf", 2U, {leaf}},
            {"Parent", 1U, {parent}}};
    source.primary_property_table = 0U;
    source.hierarchy = PropertyHierarchy{
            {{1U, 0U, {2U}}, {1U, 1U, {2U}}, {2U, 0U, {}}},
            {0U, 1U}};
    const auto expected = lookupPropertyMaterial(source, 0U, 1U);

    const auto compacted = compactPropertyHierarchy(
            source.property_tables, *source.hierarchy, {1U}, limits());
    FeatureMetadata output;
    output.property_tables.push_back(compactPropertyTable(
            source.property_tables.front(), {1U}, limits()));
    output.property_tables.insert(output.property_tables.end(),
                                  compacted.property_tables.begin(),
                                  compacted.property_tables.end());
    output.primary_property_table = 0U;
    output.hierarchy = compacted.hierarchy;
    for (auto& node : output.hierarchy->nodes) ++node.property_table;

    ASSERT_TRUE(expected.has_value());
    EXPECT_EQ(lookupPropertyMaterial(output, 0U, 0U), expected);
    EXPECT_FALSE(lookupPropertyMaterial(output, 0U, 1U).has_value());
}

}  // namespace
}  // namespace clip_worker::metadata
