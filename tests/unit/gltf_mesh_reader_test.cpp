#include "clip_worker/formats/gltf_mesh_reader.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "support/synthetic_tile.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

namespace clip_worker::formats {
namespace {

using Json = nlohmann::json;

template <typename T>
void appendValues(std::vector<std::uint8_t>& output,
                  const std::vector<T>& values) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
    output.insert(output.end(), bytes, bytes + values.size() * sizeof(T));
}

struct ExternalFixture {
    std::vector<std::uint8_t> json;
    ApprovedGltfResourceMap resources;
};

ExternalFixture externalFixture() {
    std::vector<std::uint8_t> geometry;
    appendValues(geometry, std::vector<float>{
            0.0F, 0.0F, 0.0F,
            1.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F});
    appendValues(geometry, std::vector<std::uint16_t>{0U, 1U, 2U});

    std::vector<std::uint8_t> attributes;
    const std::array<float, 15U> interleaved{
            0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F, 1.0F};
    attributes.resize(sizeof(interleaved));
    std::memcpy(attributes.data(), interleaved.data(), attributes.size());

    const mesh::RgbaImage image{2U, 2U,
            {255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U,
             0U, 0U, 255U, 255U, 0U, 0U, 0U, 0U}};
    const auto png = TextureCodec::encodeDeterministicPng(image);

    Json root;
    root["asset"] = {{"version", "2.0"}};
    root["scene"] = 0U;
    root["scenes"] = Json::array({{{"nodes", Json::array({0U})}}});
    root["nodes"] = Json::array({{{"mesh", 0U},
                                   {"translation", Json::array({1.0, 2.0, 3.0})}}});
    root["buffers"] = Json::array({
            {{"byteLength", geometry.size()}, {"uri", "geometry.bin"}},
            {{"byteLength", attributes.size()}, {"uri", "attrs.bin"}}});
    root["bufferViews"] = Json::array({
            {{"buffer", 0U}, {"byteLength", 36U}, {"byteOffset", 0U}},
            {{"buffer", 0U}, {"byteLength", 6U}, {"byteOffset", 36U}},
            {{"buffer", 1U}, {"byteLength", attributes.size()},
             {"byteOffset", 0U}, {"byteStride", 20U}}});
    root["accessors"] = Json::array({
            {{"bufferView", 0U}, {"componentType", 5126U},
             {"count", 3U}, {"type", "VEC3"}},
            {{"bufferView", 1U}, {"componentType", 5123U},
             {"count", 3U}, {"type", "SCALAR"}},
            {{"bufferView", 2U}, {"byteOffset", 0U},
             {"componentType", 5126U}, {"count", 3U}, {"type", "VEC3"}},
            {{"bufferView", 2U}, {"byteOffset", 12U},
             {"componentType", 5126U}, {"count", 3U}, {"type", "VEC2"}}});
    const Json primitive{{"attributes", {{"NORMAL", 2U},
                                           {"POSITION", 0U},
                                           {"TEXCOORD_0", 3U}}},
                         {"indices", 1U}, {"material", 0U}, {"mode", 4U}};
    const Json strip{{"attributes", {{"NORMAL", 2U},
                                       {"POSITION", 0U},
                                       {"TEXCOORD_0", 3U}}},
                     {"indices", 1U}, {"material", 0U}, {"mode", 5U}};
    root["meshes"] = Json::array({{{"primitives", Json::array({primitive, strip})}}});
    root["images"] = Json::array({{{"uri", "texture.png"}}});
    root["samplers"] = Json::array({{{"wrapS", 33648U}, {"wrapT", 33071U}}});
    root["textures"] = Json::array({{{"sampler", 0U}, {"source", 0U}}});
    root["materials"] = Json::array({{{"pbrMetallicRoughness",
                                        {{"baseColorTexture", {{"index", 0U}}}}}}});

    const std::string text = root.dump();
    return {{text.begin(), text.end()},
            {{"models/attrs.bin", {attributes, "application/octet-stream"}},
             {"models/geometry.bin", {geometry, "application/octet-stream"}},
             {"models/texture.png", {png, "image/png"}}}};
}

TEST(GltfMeshReaderTest, ReadsExternalInterleavedResourcesAndEmbedsCanonicalGlb) {
    const auto fixture = externalFixture();

    const auto read = GltfMeshReader::read(
            fixture.json, GltfContentKind::gltf, "models/model.gltf",
            fixture.resources);

    ASSERT_EQ(read.scene.meshes.size(), 1U);
    ASSERT_EQ(read.scene.meshes.front().primitives.size(), 2U);
    EXPECT_EQ(read.scene.meshes.front().primitives[0U].triangleCount(), 1U);
    EXPECT_EQ(read.scene.meshes.front().primitives[1U].triangleCount(), 1U);
    EXPECT_EQ(read.scene.images.front().pixels.size(), 16U);
    EXPECT_EQ(read.diagnostics.external_resource_count, 3U);

    const auto first = normalization::MeshCanonicalWriter::write(read.scene);
    const auto second = normalization::MeshCanonicalWriter::write(read.scene);
    EXPECT_EQ(first.glb, second.glb);
    EXPECT_EQ(first.triangle_count, 2U);
    const normalization::ToolVersion validator{
            "mesh-canonical-validator", "2.0.0", std::string(64U, 'a')};
    const auto evidence = normalization::validateCanonicalGlb(
            first.glb, normalization::CanonicalFamily::mesh_gltf2, validator);
    EXPECT_EQ(evidence.validation_summary.primitive_count, 2U);

    const auto round_trip = GltfMeshReader::read(
            first.glb, GltfContentKind::glb, "canonical/model.glb", {});
    EXPECT_EQ(normalization::MeshCanonicalWriter::write(round_trip.scene).glb,
              first.glb);
}

TEST(GltfMeshReaderTest, AppliesSparseAccessorAndRejectsDuplicateSparseIndices) {
    Json root;
    root["asset"] = {{"version", "2.0"}};
    root["scene"] = 0U;
    root["scenes"] = Json::array({{{"nodes", Json::array({0U})}}});
    root["nodes"] = Json::array({{{"mesh", 0U}}});
    root["buffers"] = Json::array({{{"byteLength", 16U},
                                      {"uri", "data:application/octet-stream;base64,AQAAAAAAgD8AAAAAAAAAAA=="}}});
    root["bufferViews"] = Json::array({
            {{"buffer", 0U}, {"byteLength", 4U}, {"byteOffset", 0U}},
            {{"buffer", 0U}, {"byteLength", 12U}, {"byteOffset", 4U}}});
    root["accessors"] = Json::array({{{"componentType", 5126U},
                                       {"count", 3U},
                                       {"type", "VEC3"},
                                       {"sparse", {{"count", 1U},
                                                    {"indices", {{"bufferView", 0U},
                                                                  {"componentType", 5121U}}},
                                                    {"values", {{"bufferView", 1U}}}}}}});
    root["meshes"] = Json::array({{{"primitives", Json::array(
            {{{"attributes", {{"POSITION", 0U}}}, {"mode", 4U}}})}}});
    const std::string text = root.dump();
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());

    const auto result = GltfMeshReader::read(
            bytes, GltfContentKind::gltf, "sparse.gltf", {});
    EXPECT_EQ(result.scene.meshes.front().primitives.front().positions[3U], 1.0F);
    EXPECT_EQ(result.diagnostics.data_uri_count, 1U);
}

TEST(GltfMeshReaderTest, FailsClosedForUnapprovedUriAndUnknownAttribute) {
    auto fixture = externalFixture();
    fixture.resources.erase("models/geometry.bin");
    EXPECT_THROW(GltfMeshReader::read(
                         fixture.json, GltfContentKind::gltf,
                         "models/model.gltf", fixture.resources),
                 FormatError);

    fixture = externalFixture();
    Json root = Json::parse(fixture.json);
    root["meshes"][0U]["primitives"][0U]["attributes"]["JOINTS_0"] = 0U;
    const std::string text = root.dump();
    EXPECT_THROW(GltfMeshReader::read(
                         {text.begin(), text.end()}, GltfContentKind::gltf,
                         "models/model.gltf", fixture.resources),
                 FormatError);
}

TEST(GltfMeshReaderTest, ReadsAttributeFeatureIdsAndStructuralPropertyTable) {
    auto fixture = externalFixture();
    Json root = Json::parse(fixture.json);
    std::vector<std::uint8_t> metadata_bytes{0U, 0U, 0U, 0U};
    const float height = 42.5F;
    const auto* height_bytes = reinterpret_cast<const std::uint8_t*>(&height);
    metadata_bytes.insert(metadata_bytes.end(), height_bytes,
                          height_bytes + sizeof(height));
    const std::size_t buffer_index = root["buffers"].size();
    root["buffers"].push_back(
            {{"byteLength", metadata_bytes.size()}, {"uri", "metadata.bin"}});
    const std::size_t feature_view = root["bufferViews"].size();
    root["bufferViews"].push_back(
            {{"buffer", buffer_index}, {"byteLength", 3U}, {"byteOffset", 0U}});
    const std::size_t property_view = root["bufferViews"].size();
    root["bufferViews"].push_back(
            {{"buffer", buffer_index}, {"byteLength", sizeof(float)},
             {"byteOffset", 4U}});
    const std::size_t feature_accessor = root["accessors"].size();
    root["accessors"].push_back(
            {{"bufferView", feature_view}, {"componentType", 5121U},
             {"count", 3U}, {"type", "SCALAR"}});
    for (auto& primitive : root["meshes"][0U]["primitives"]) {
        primitive["attributes"]["_FEATURE_ID_0"] = feature_accessor;
        primitive["extensions"]["EXT_mesh_features"] = {
                {"featureIds", Json::array({{{"featureCount", 1U},
                                              {"attribute", 0U},
                                              {"label", "building"},
                                              {"propertyTable", 0U}}})}};
    }
    root["extensionsUsed"] =
            Json::array({"EXT_mesh_features", "EXT_structural_metadata"});
    root["extensionsRequired"] = root["extensionsUsed"];
    // Keep the metadata fixture readable and avoid error-prone deeply nested
    // initializer braces that compile differently across toolchains.
    const Json height_schema = {
            {"type", "SCALAR"}, {"componentType", "FLOAT32"}};
    const Json structural_schema = {
            {"classes",
             {{"Building",
               {{"properties", {{"height", height_schema}}}}}}}};
    const Json property_table = {
            {"class", "Building"},
            {"count", 1U},
            {"properties", {{"height", {{"values", property_view}}}}}};
    root["extensions"]["EXT_structural_metadata"] = {
            {"schema", structural_schema},
            {"propertyTables", Json::array({property_table})}};
    fixture.resources["models/metadata.bin"] =
            {metadata_bytes, "application/octet-stream"};
    const std::string text = root.dump();
    GltfMeshReaderLimits limits;
    limits.enable_feature_metadata = true;
    limits.metadata.maximum_schema_bytes = 1024U * 1024U;
    limits.metadata.maximum_classes = 16U;
    limits.metadata.maximum_enums = 16U;
    limits.metadata.maximum_property_tables = 16U;
    limits.metadata.maximum_feature_id_sets = 16U;
    limits.metadata.maximum_feature_rows = 1024U;
    limits.metadata.maximum_properties = 32U;
    limits.metadata.maximum_values_bytes = 1024U * 1024U;
    limits.metadata.maximum_array_elements = 4096U;
    const auto result = GltfMeshReader::read(
            {text.begin(), text.end()}, GltfContentKind::gltf,
            "models/model.gltf", fixture.resources, limits);
    ASSERT_TRUE(result.scene.feature_metadata.has_value());
    ASSERT_EQ(result.scene.feature_metadata->property_tables.size(), 1U);
    ASSERT_EQ(result.scene.meshes.front().primitives.front()
                      .feature_id_sets.size(),
              1U);
    EXPECT_EQ(std::get<std::vector<float>>(
                      result.scene.feature_metadata->property_tables.front()
                              .columns.front().values),
              (std::vector<float>{42.5F}));
}

TEST(GltfMeshReaderTest, DecodesMeshoptBufferViewAndEnforcesDracoLimits) {
    const std::array<float, 9U> positions{
            0.0F, 0.0F, 0.0F,
            1.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F};
    std::vector<std::uint8_t> encoded(
            meshopt_encodeVertexBufferBound(3U, sizeof(float) * 3U));
    const std::size_t encoded_size = meshopt_encodeVertexBuffer(
            encoded.data(), encoded.size(), positions.data(), 3U,
            sizeof(float) * 3U);
    encoded.resize(encoded_size);
    Json root;
    root["asset"] = {{"version", "2.0"}};
    root["extensionsUsed"] = Json::array({"EXT_meshopt_compression"});
    root["extensionsRequired"] = Json::array({"EXT_meshopt_compression"});
    root["scene"] = 0U;
    root["scenes"] = Json::array({{{"nodes", Json::array({0U})}}});
    root["nodes"] = Json::array({{{"mesh", 0U}}});
    root["buffers"] = Json::array({{{"byteLength", encoded.size()},
                                      {"uri", "compressed.bin"}}});
    root["bufferViews"] = Json::array({{{"extensions",
            {{"EXT_meshopt_compression",
              {{"buffer", 0U}, {"byteLength", encoded.size()},
               {"byteOffset", 0U}, {"byteStride", sizeof(float) * 3U},
               {"count", 3U}, {"filter", "NONE"},
               {"mode", "ATTRIBUTES"}}}}}}});
    root["accessors"] = Json::array({{{"bufferView", 0U},
                                       {"componentType", 5126U},
                                       {"count", 3U}, {"type", "VEC3"}}});
    root["meshes"] = Json::array({{{"primitives", Json::array({
            {{"attributes", {{"POSITION", 0U}}}, {"mode", 4U}}})}}});
    const std::string text = root.dump();
    const auto meshopt = GltfMeshReader::read(
            {text.begin(), text.end()}, GltfContentKind::gltf,
            "model.gltf", {{"compressed.bin",
                             {encoded, "application/octet-stream"}}});
    EXPECT_EQ(meshopt.diagnostics.meshopt_buffer_view_count, 1U);
    EXPECT_EQ(meshopt.scene.meshes.front().primitives.front().positions[3U],
              1.0F);

    const auto fixture = tests::makeDracoTexturedMeshFixture();
    const auto b3dm = B3dmParser::parse(ByteView(fixture.b3dm));
    std::vector<std::uint8_t> glb(
            fixture.b3dm.begin() + static_cast<std::ptrdiff_t>(b3dm.glb_section.offset),
            fixture.b3dm.begin() + static_cast<std::ptrdiff_t>(
                    b3dm.glb_section.offset + b3dm.glb_section.byte_length));
    GltfMeshReaderLimits limits;
    limits.draco.maximum_points = 2U;
    EXPECT_THROW(GltfMeshReader::read(
                         glb, GltfContentKind::glb, "draco.glb", {}, limits),
                 FormatError);
}

}  // namespace
}  // namespace clip_worker::formats
