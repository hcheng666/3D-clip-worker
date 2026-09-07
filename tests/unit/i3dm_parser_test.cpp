#include "clip_worker/formats/i3dm.hpp"
#include "clip_worker/formats/format_error.hpp"

#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "support/synthetic_i3dm.hpp"

#include <gtest/gtest.h>

namespace clip_worker::formats {
namespace {

mesh::MeshScene modelScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {0.0F, 0.0F, 0.0F,
                           1.0F, 0.0F, 0.0F,
                           0.0F, 1.0F, 0.0F};
    primitive.indices = {0U, 1U, 2U};
    mesh::MeshScene scene;
    scene.default_scene = 0U;
    scene.scenes = {{0U}};
    scene.nodes = {{0U, geometry::Matrix4::identity(), 0U, {}}};
    scene.meshes = {{{std::move(primitive)}, {}}};
    return scene;
}

TEST(I3dmParserTest, ParsesEmbeddedAndExternalPayloadForms) {
    const auto model = normalization::MeshCanonicalWriter::write(modelScene()).glb;
    const auto embedded_bytes = tests::makeEmbeddedI3dmFixture(model);
    const auto embedded = I3dmParser::parse(ByteView(embedded_bytes));
    EXPECT_EQ(embedded.gltf_format, I3dmGltfFormat::embedded_glb);
    EXPECT_EQ(embedded.gltf_payload.byte_length, model.size());

    const auto external_bytes = tests::makeExternalI3dmFixture("models/tree.glb");
    const auto external = I3dmParser::parse(ByteView(external_bytes));
    EXPECT_EQ(external.gltf_format, I3dmGltfFormat::uri);
    EXPECT_EQ(external.external_gltf_uri, "models/tree.glb");
}

TEST(I3dmParserTest, AcceptsBoundedZeroPaddingAfterEmbeddedGlb) {
    const auto model = normalization::MeshCanonicalWriter::write(modelScene()).glb;
    auto bytes = tests::makeOctOrientedI3dmFixture(model);
    bytes.insert(bytes.end(), 4U, 0U);
    const std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
    for (std::size_t byte = 0U; byte < sizeof(std::uint32_t); ++byte) {
        bytes[8U + byte] = static_cast<std::uint8_t>(
                (length >> (byte * 8U)) & 0xffU);
    }

    const auto document = formats::I3dmParser::parse(
            formats::ByteView(bytes));
    EXPECT_EQ(document.gltf_payload.byte_length, model.size());

    bytes.back() = 1U;
    EXPECT_THROW(formats::I3dmParser::parse(formats::ByteView(bytes)),
                 formats::FormatError);
}

}  // namespace
}  // namespace clip_worker::formats
