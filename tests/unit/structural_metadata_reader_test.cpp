#include "clip_worker/metadata/structural_metadata_reader.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <cstring>

#include <gtest/gtest.h>

namespace clip_worker::metadata {
namespace {

MetadataResourceLimits limits() {
    MetadataResourceLimits value;
    value.maximum_schema_bytes = 1024U * 1024U;
    value.maximum_classes = 16U;
    value.maximum_enums = 16U;
    value.maximum_property_tables = 16U;
    value.maximum_feature_rows = 1024U;
    value.maximum_properties = 32U;
    value.maximum_values_bytes = 1024U * 1024U;
    value.maximum_array_offset_bytes = 1024U * 1024U;
    value.maximum_string_offset_bytes = 1024U * 1024U;
    value.maximum_decoded_string_bytes = 1024U * 1024U;
    value.maximum_array_elements = 4096U;
    return value;
}

template <typename Value>
MetadataBufferView bytes(std::vector<std::vector<std::uint8_t>>& storage,
                         std::initializer_list<Value> values) {
    storage.emplace_back(values.size() * sizeof(Value));
    std::memcpy(storage.back().data(), values.begin(), storage.back().size());
    return {&storage.back(), 0U};
}

TEST(StructuralMetadataReaderTest, ReadsNumericStringBooleanAndEnumTables) {
    const std::string json = R"({
      "extensions":{"EXT_structural_metadata":{
        "schema":{"classes":{"Feature":{"properties":{
          "height":{"type":"SCALAR","componentType":"FLOAT32","noData":-1,"default":0},
          "labels":{"type":"STRING","array":true},
          "visible":{"type":"BOOLEAN"},
          "kind":{"type":"ENUM","enumType":"Kind"}
        }}},"enums":{"Kind":{"valueType":"UINT8","values":[
          {"name":"A","value":1},{"name":"B","value":2}
        ]}}},
        "propertyTables":[{"class":"Feature","count":2,"properties":{
          "height":{"values":0},
          "labels":{"values":1,"arrayOffsets":2,"stringOffsets":3},
          "visible":{"values":4},
          "kind":{"values":5}
        }}]
      }}
    })";
    std::vector<std::vector<std::uint8_t>> storage;
    storage.reserve(6U);
    std::vector<MetadataBufferView> views;
    views.push_back(bytes<float>(storage, {12.5F, 25.0F}));
    views.push_back(bytes<std::uint8_t>(storage, {'a', 'b', 'c'}));
    views.push_back(bytes<std::uint32_t>(storage, {0U, 2U, 3U}));
    views.push_back(bytes<std::uint32_t>(storage, {0U, 1U, 2U, 3U}));
    views.push_back(bytes<std::uint8_t>(storage, {0x01U}));
    views.push_back(bytes<std::uint8_t>(storage, {1U, 2U}));
    const auto metadata = readStructuralMetadata(json, views, limits());
    ASSERT_EQ(metadata.property_tables.size(), 1U);
    ASSERT_EQ(metadata.enums.size(), 1U);
    const auto& columns = metadata.property_tables.front().columns;
    EXPECT_EQ(std::get<std::vector<float>>(columns.at(0).values),
              (std::vector<float>{12.5F, 25.0F}));
    EXPECT_EQ(std::get<EnumValues>(columns.at(1).values).values,
              (std::vector<std::int64_t>{1, 2}));
    EXPECT_EQ(std::get<StringValues>(columns.at(2).values).values,
              (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(std::get<BooleanValues>(columns.at(3).values).values,
              (std::vector<std::uint8_t>{1U, 0U}));
}

TEST(StructuralMetadataReaderTest, RejectsPropertyTextureExplicitly) {
    const std::string json = R"({"extensions":{"EXT_structural_metadata":{
      "schema":{"classes":{}},"propertyTextures":[{"pixel":"texture-canary"}]
    }}})";
    try {
        static_cast<void>(readStructuralMetadata(json, {}, limits()));
        FAIL() << "property textures must fail closed";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(),
                  formats::FormatErrorCode::metadata_property_texture_unsupported);
        EXPECT_EQ(std::string(error.what()).find("texture-canary"),
                  std::string::npos);
    }
}

TEST(StructuralMetadataReaderTest, RejectsSchemaExtrasWithoutEchoingCanary) {
    const std::string json = R"({"extensions":{"EXT_structural_metadata":{
      "schema":{"classes":{},"extras":{"secret":"schema-canary"}}
    }}})";
    try {
        static_cast<void>(readStructuralMetadata(json, {}, limits()));
        FAIL() << "schema extras must fail closed";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(),
                  formats::FormatErrorCode::metadata_reconstruction_unsafe);
        EXPECT_EQ(std::string(error.what()).find("schema-canary"),
                  std::string::npos);
    }
}

TEST(StructuralMetadataReaderTest, RejectsEnumValueOutsideDeclaredType) {
    const std::string json = R"({"extensions":{"EXT_structural_metadata":{
      "schema":{"classes":{},"enums":{"Kind":{"valueType":"UINT8",
        "values":[{"name":"TOO_LARGE","value":256}]}}}
    }}})";
    try {
        static_cast<void>(readStructuralMetadata(json, {}, limits()));
        FAIL() << "enum values must fit their declared valueType";
    } catch (const formats::FormatError& error) {
        EXPECT_EQ(error.code(),
                  formats::FormatErrorCode::metadata_property_table_invalid);
    }
}

}  // namespace
}  // namespace clip_worker::metadata
