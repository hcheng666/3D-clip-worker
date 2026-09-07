#include "clip_worker/metadata/legacy_feature_metadata.hpp"

#include <cstring>

#include <gtest/gtest.h>

namespace clip_worker::metadata {
namespace {

MetadataResourceLimits limits() {
    MetadataResourceLimits value;
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

TEST(LegacyFeatureMetadataTest, PreservesBinaryScalarTypes) {
    std::vector<std::uint8_t> binary(sizeof(std::uint32_t) * 2U);
    const std::uint32_t values[] = {7U, 4000000000U};
    std::memcpy(binary.data(), values, sizeof(values));
    const auto metadata = readLegacyFeatureMetadata(
            R"({"code":{"byteOffset":0,"componentType":"UNSIGNED_INT","type":"SCALAR"}})",
            formats::ByteView(binary), 2U, limits());
    const auto& column = metadata.property_tables.front().columns.front();
    EXPECT_EQ(column.scalar_type, PropertyScalarType::uint32);
    EXPECT_EQ(std::get<std::vector<std::uint32_t>>(column.values),
              (std::vector<std::uint32_t>{7U, 4000000000U}));
}

TEST(LegacyFeatureMetadataTest, CompactsHierarchyAncestorWithoutSiblingCanary) {
    const std::string json = R"({
      "featureName":["kept","removed"],
      "extensions":{"3DTILES_batch_table_hierarchy":{
        "instancesLength":4,
        "classIds":[0,0,1,1],
        "parentIds":[2,3,2,3],
        "classes":[
          {"name":"Leaf","length":2,"instances":{"secret":["kept-leaf","removed-leaf-canary"]}},
          {"name":"Parent","length":2,"instances":{"name":["kept-parent","removed-parent-canary"]}}
        ]
      }}
    })";
    const auto source = readLegacyFeatureMetadata(
            json, formats::ByteView(), 2U, limits());
    const auto compacted = compactLegacyFeatureMetadata(source, {0U}, limits());
    ASSERT_TRUE(compacted.hierarchy.has_value());
    EXPECT_EQ(compacted.hierarchy->nodes.size(), 2U);
    EXPECT_EQ(std::get<StringValues>(
                      compacted.property_tables.at(1).columns.front().values)
                      .values,
              (std::vector<std::string>{"kept-leaf"}));
    EXPECT_EQ(std::get<StringValues>(
                      compacted.property_tables.at(2).columns.front().values)
                      .values,
              (std::vector<std::string>{"kept-parent"}));
}

}  // namespace
}  // namespace clip_worker::metadata
