#include "clip_worker/formats/b3dm_mesh_adapter.hpp"

#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/glb.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace clip_worker::formats {
namespace {

using Json = nlohmann::json;

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        output.push_back(static_cast<std::uint8_t>(
                (value >> (byte * 8U)) & 0xffU));
    }
}

void writeU32(std::vector<std::uint8_t>& output, std::size_t offset,
              std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        output.at(offset + byte) = static_cast<std::uint8_t>(
                (value >> (byte * 8U)) & 0xffU);
    }
}

template <typename T>
void appendValues(std::vector<std::uint8_t>& output,
                  const std::vector<T>& values) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(values.data());
    output.insert(output.end(), first, first + values.size() * sizeof(T));
}

void align(std::vector<std::uint8_t>& output, std::size_t boundary,
           std::uint8_t padding) {
    while (output.size() % boundary != 0U) output.push_back(padding);
}

std::vector<std::uint8_t> featureGlb() {
    std::vector<std::uint8_t> binary;
    appendValues(binary, std::vector<float>{
            0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
            2.0F, 0.0F, 0.0F, 3.0F, 0.0F, 0.0F, 2.0F, 1.0F, 0.0F});
    const std::size_t batch_offset = binary.size();
    binary.insert(binary.end(), {2U, 2U, 2U, 0U, 0U, 0U});
    align(binary, 4U, 0U);

    Json root;
    root["asset"] = {{"version", "2.0"}};
    root["scene"] = 0U;
    root["scenes"] = Json::array({{{"nodes", Json::array({0U})}}});
    root["nodes"] = Json::array({{{"mesh", 0U}}});
    root["buffers"] = Json::array({{{"byteLength", batch_offset + 6U}}});
    root["bufferViews"] = Json::array({
            {{"buffer", 0U}, {"byteLength", batch_offset}, {"byteOffset", 0U}},
            {{"buffer", 0U}, {"byteLength", 6U}, {"byteOffset", batch_offset}}});
    root["accessors"] = Json::array({
            {{"bufferView", 0U}, {"byteOffset", 0U}, {"componentType", 5126U},
             {"count", 3U}, {"type", "VEC3"}},
            {{"bufferView", 0U}, {"byteOffset", 36U}, {"componentType", 5126U},
             {"count", 3U}, {"type", "VEC3"}},
            {{"bufferView", 1U}, {"byteOffset", 0U}, {"componentType", 5121U},
             {"count", 3U}, {"type", "SCALAR"}},
            {{"bufferView", 1U}, {"byteOffset", 3U}, {"componentType", 5121U},
             {"count", 3U}, {"type", "SCALAR"}}});
    root["meshes"] = Json::array({{{"primitives", Json::array({
            {{"attributes", {{"POSITION", 0U}, {"_BATCHID", 2U}}}, {"mode", 4U}},
            {{"attributes", {{"POSITION", 1U}, {"_BATCHID", 3U}}}, {"mode", 4U}}})}}});

    std::string json = root.dump();
    while (json.size() % 4U != 0U) json.push_back(' ');
    std::vector<std::uint8_t> output{'g', 'l', 'T', 'F'};
    appendU32(output, 2U);
    appendU32(output, 0U);
    appendU32(output, static_cast<std::uint32_t>(json.size()));
    appendU32(output, GlbParser::kJsonChunkType);
    output.insert(output.end(), json.begin(), json.end());
    appendU32(output, static_cast<std::uint32_t>(binary.size()));
    appendU32(output, GlbParser::kBinaryChunkType);
    output.insert(output.end(), binary.begin(), binary.end());
    writeU32(output, 8U, static_cast<std::uint32_t>(output.size()));
    return output;
}

std::vector<std::uint8_t> multiFeatureB3dm() {
    const auto glb = featureGlb();
    Json feature_table{{"BATCH_LENGTH", 3U},
                       {"RTC_CENTER", {{"byteOffset", 0U}}}};
    Json batch_table{
            {"active", Json::array({true, false, true})},
            {"height", {{"byteOffset", 0U}, {"componentType", "FLOAT"},
                        {"type", "SCALAR"}}},
            {"name", Json::array({"zero", "one", "two"})},
            {"vector", {{"byteOffset", 12U}, {"componentType", "SHORT"},
                        {"type", "VEC2"}}}};
    std::string feature_json = feature_table.dump();
    std::string batch_json = batch_table.dump();
    std::vector<std::uint8_t> feature_binary;
    appendValues(feature_binary, std::vector<float>{10.0F, 20.0F, 30.0F});
    std::vector<std::uint8_t> batch_binary;
    appendValues(batch_binary, std::vector<float>{100.0F, 200.0F, 300.0F});
    appendValues(batch_binary, std::vector<std::int16_t>{1, 2, 3, 4, 5, 6});

    std::vector<std::uint8_t> output(B3dmParser::kHeaderSize, 0U);
    output[0U] = 'b'; output[1U] = '3'; output[2U] = 'd'; output[3U] = 'm';
    const std::size_t feature_json_start = output.size();
    output.insert(output.end(), feature_json.begin(), feature_json.end());
    align(output, 8U, static_cast<std::uint8_t>(' '));
    const std::size_t feature_json_length = output.size() - feature_json_start;
    const std::size_t feature_binary_start = output.size();
    output.insert(output.end(), feature_binary.begin(), feature_binary.end());
    align(output, 8U, 0U);
    const std::size_t feature_binary_length = output.size() - feature_binary_start;
    const std::size_t batch_json_start = output.size();
    output.insert(output.end(), batch_json.begin(), batch_json.end());
    align(output, 8U, static_cast<std::uint8_t>(' '));
    const std::size_t batch_json_length = output.size() - batch_json_start;
    const std::size_t batch_binary_start = output.size();
    output.insert(output.end(), batch_binary.begin(), batch_binary.end());
    align(output, 8U, 0U);
    const std::size_t batch_binary_length = output.size() - batch_binary_start;
    output.insert(output.end(), glb.begin(), glb.end());
    align(output, 8U, 0U);
    writeU32(output, 4U, 1U);
    writeU32(output, 8U, static_cast<std::uint32_t>(output.size()));
    writeU32(output, 12U, static_cast<std::uint32_t>(feature_json_length));
    writeU32(output, 16U, static_cast<std::uint32_t>(feature_binary_length));
    writeU32(output, 20U, static_cast<std::uint32_t>(batch_json_length));
    writeU32(output, 24U, static_cast<std::uint32_t>(batch_binary_length));
    return output;
}

TEST(B3dmMeshAdapterTest, MapsMultipleFeaturesAndLegacyPropertiesDeterministically) {
    const auto decoded = B3dmMeshAdapter::read(multiFeatureB3dm());

    ASSERT_TRUE(decoded.scene.legacy_properties.has_value());
    EXPECT_EQ(decoded.scene.legacy_properties->feature_count, 3U);
    ASSERT_EQ(decoded.scene.legacy_properties->columns.size(), 4U);
    EXPECT_EQ(decoded.scene.nodes.size(), 2U);
    EXPECT_DOUBLE_EQ(decoded.scene.nodes.back().local_transform.values()[12U], 10.0);
    EXPECT_EQ(decoded.scene.meshes.front().primitives[0U].feature_ids[0U], 2U);
    EXPECT_EQ(decoded.scene.meshes.front().primitives[1U].feature_ids[0U], 0U);

    const auto first = normalization::MeshCanonicalWriter::write(decoded.scene);
    const auto second = normalization::MeshCanonicalWriter::write(decoded.scene);
    EXPECT_EQ(first.glb, second.glb);
    const auto document = GlbParser::parse(ByteView(first.glb));
    const Json root = Json::parse(document.json_text);
    EXPECT_EQ(root.at("extensions").at("EXT_structural_metadata")
                      .at("propertyTables").at(0U).at("count"), 2U);
    EXPECT_EQ(root.at("meshes").at(0U).at("primitives").at(0U)
                      .at("extensions").at("EXT_mesh_features")
                      .at("featureIds").at(0U).at("featureCount"), 2U);
    EXPECT_EQ(root.at("meshes").at(0U).at("primitives").at(0U)
                      .at("extensions").at("EXT_mesh_features")
                      .at("featureIds").at(0U).at("propertyTable"), 0U);

    const normalization::ToolVersion validator{
            "mesh-canonical-validator", "2.0.0", std::string(64U, 'b')};
    const auto evidence = normalization::validateCanonicalGlb(
            first.glb, normalization::CanonicalFamily::mesh_gltf2, validator);
    EXPECT_EQ(evidence.validation_summary.feature_count, 2U);
    EXPECT_EQ(evidence.validation_summary.metadata_property_count, 4U);
}

TEST(B3dmMeshAdapterTest, RejectsMixedFeatureIdentityInsideTriangle) {
    auto bytes = multiFeatureB3dm();
    const auto document = B3dmParser::parse(ByteView(bytes));
    const Json root = Json::parse(document.glb.json_text);
    const std::size_t batch_view_offset = root.at("bufferViews").at(1U)
                                                  .at("byteOffset").get<std::size_t>();
    const std::size_t batch_byte = document.glb_section.offset
                                   + document.glb.binary_offset
                                   + batch_view_offset + 1U;
    bytes.at(batch_byte) = 0U;
    EXPECT_THROW(B3dmMeshAdapter::read(bytes), FormatError);
}

}  // namespace
}  // namespace clip_worker::formats
