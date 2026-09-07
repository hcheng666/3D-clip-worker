#include "clip_worker/normalization/mesh_normalization_executor.hpp"

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

mesh::MeshScene triangleScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {
            0.0F, 0.0F, 0.0F,
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

ResourceRecord record(const std::string& id,
                      const std::string& path,
                      const std::string& role,
                      const std::string& kind,
                      const std::vector<std::uint8_t>& bytes) {
    ResourceRecord result;
    result.object_id = id;
    result.package_relative_path = path;
    result.resource_role = role;
    result.detected_kind = kind;
    result.size = bytes.size();
    result.sha256 = client::sha256Hex(bytes);
    return result;
}

TEST(MeshNormalizationExecutorTest, SelectsExactlyOneManifestContentRoot) {
    const auto source = MeshCanonicalWriter::write(triangleScene()).glb;
    const std::vector<std::uint8_t> image{1U, 2U, 3U};
    const std::vector<ResourceRecord> records{
            record("root", "content.glb", "CONTENT", "GLB", source),
            record("image", "textures/a.png", "IMAGE", "PNG", image)};
    const std::map<std::string, std::vector<std::uint8_t>> bytes{
            {"root", source}, {"image", image}};

    const auto input = MeshNormalizationExecutor::buildInput(records, bytes);

    EXPECT_EQ(input.source_kind, MeshSourceKind::glb);
    EXPECT_EQ(input.source_bytes, source);
    ASSERT_EQ(input.approved_resources.size(), 1U);
    EXPECT_EQ(input.approved_resources.begin()->first, "textures/a.png");
}

TEST(MeshNormalizationExecutorTest, RejectsMissingMultipleOrDriftedRoots) {
    const auto source = MeshCanonicalWriter::write(triangleScene()).glb;
    auto first = record("first", "first.glb", "CONTENT", "GLB", source);
    auto second = record("second", "second.glb", "CONTENT", "GLB", source);
    const std::map<std::string, std::vector<std::uint8_t>> bytes{
            {"first", source}, {"second", source}};
    EXPECT_THROW(MeshNormalizationExecutor::buildInput(
                         {first, second}, bytes),
                 std::invalid_argument);
    first.resource_role = "BUFFER";
    EXPECT_THROW(MeshNormalizationExecutor::buildInput(
                         {first}, {{"first", source}}),
                 std::invalid_argument);
    first.resource_role = "CONTENT";
    first.sha256 = std::string(64U, '0');
    EXPECT_THROW(MeshNormalizationExecutor::buildInput(
                         {first}, {{"first", source}}),
                 std::invalid_argument);
}

TEST(MeshNormalizationExecutorTest, MapsTypedCodecFailuresToStableApiCodes) {
    const std::vector<std::pair<formats::FormatErrorCode, std::string>> cases{
            {formats::FormatErrorCode::unsupported_version,
             "CONTENT_VERSION_UNSUPPORTED"},
            {formats::FormatErrorCode::content_primitive_mode_unsupported,
             "CONTENT_PRIMITIVE_MODE_UNSUPPORTED"},
            {formats::FormatErrorCode::compression_draco_invalid,
             "COMPRESSION_DRACO_INVALID"},
            {formats::FormatErrorCode::compression_meshopt_limit_exceeded,
             "COMPRESSION_MESHOPT_LIMIT_EXCEEDED"},
            {formats::FormatErrorCode::texture_invalid,
             "TEXTURE_DECODE_INVALID"},
            {formats::FormatErrorCode::texture_dimension_limit_exceeded,
             "TEXTURE_DIMENSION_LIMIT_EXCEEDED"},
            {formats::FormatErrorCode::texture_decoded_bytes_limit_exceeded,
             "TEXTURE_DECODED_BYTES_LIMIT_EXCEEDED"},
            {formats::FormatErrorCode::metadata_batch_table_unsupported,
             "METADATA_BATCH_TABLE_UNSUPPORTED"},
            {formats::FormatErrorCode::normalization_output_invalid,
             "NORMALIZATION_OUTPUT_INVALID"},
            {formats::FormatErrorCode::clipping_output_invalid,
             "CLIPPING_OUTPUT_INVALID"}};
    for (const auto& test_case : cases) {
        const formats::FormatError error(test_case.first, "bounded failure");
        const auto failure = meshNormalizationFailure(error);
        EXPECT_EQ(failure.error_code, test_case.second);
        EXPECT_EQ(failure.unsupported,
                  test_case.first
                          != formats::FormatErrorCode::normalization_output_invalid
                      && test_case.first
                          != formats::FormatErrorCode::clipping_output_invalid);
        EXPECT_FALSE(failure.retryable);
    }
}

}  // namespace
}  // namespace clip_worker::normalization
