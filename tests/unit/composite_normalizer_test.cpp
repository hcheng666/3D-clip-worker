#include "clip_worker/normalization/composite_normalizer.hpp"

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "support/synthetic_cmpt.hpp"
#include "support/synthetic_pnts.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

mesh::MeshScene triangleScene() {
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

MeshResourceProfile profile() {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/resource-limit-profiles-v2.json";
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    return MeshResourceProfile::load(
            path, kMeshResourceProfileVersion, client::sha256Hex(bytes));
}

ToolVersion validator(const std::string& family) {
    return {family + "_CANONICAL_VALIDATOR", "1.0.0", std::string(64U, 'd')};
}

TEST(CompositeNormalizerTest, NormalizesSupportedLeavesAndMarksMixedParentPreviewOnly) {
    const auto glb = MeshCanonicalWriter::write(triangleScene()).glb;
    CompositeNormalizationInput input;
    input.root_package_relative_path = "tiles/root.cmpt";
    input.source_bytes = tests::makeCompositeFixture(
            glb, tests::makeQuantizedPntsFixture().bytes);

    const auto first = CompositeNormalizer::normalize(
            input, profile(), {}, {}, validator("MESH"), validator("POINT"),
            validator("INSTANCE"));
    const auto second = CompositeNormalizer::normalize(
            input, profile(), {}, {}, validator("MESH"), validator("POINT"),
            validator("INSTANCE"));

    ASSERT_EQ(first.leaves.size(), 3U);
    EXPECT_EQ(first.leaves[0U].status, CompositeLeafStatus::success);
    EXPECT_EQ(first.leaves[0U].canonical_family, CanonicalFamily::mesh_gltf2);
    EXPECT_EQ(first.leaves[1U].status, CompositeLeafStatus::success);
    EXPECT_EQ(first.leaves[1U].canonical_family, CanonicalFamily::point_gltf2);
    EXPECT_EQ(first.leaves[2U].status, CompositeLeafStatus::unsupported);
    EXPECT_TRUE(first.global_preview_only);
    EXPECT_EQ(first.parent_manifest, second.parent_manifest);
    EXPECT_EQ(first.parent_manifest_sha256, second.parent_manifest_sha256);
    EXPECT_TRUE(std::all_of(first.parent_manifest.begin(),
                            first.parent_manifest.end(),
                            [](std::uint8_t value) { return value != '\0'; }));
}

}  // namespace
}  // namespace clip_worker::normalization
