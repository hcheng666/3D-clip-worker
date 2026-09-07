#include "clip_worker/formats/cmpt.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "support/synthetic_cmpt.hpp"
#include "support/synthetic_pnts.hpp"

#include <gtest/gtest.h>

namespace clip_worker::formats {
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

TEST(CmptParserTest, PreservesNestedDepthFirstOrdinalIdentity) {
    const auto glb = normalization::MeshCanonicalWriter::write(
            triangleScene()).glb;
    const auto pnts = tests::makeQuantizedPntsFixture().bytes;
    const auto bytes = tests::makeCompositeFixture(glb, pnts);
    const auto first = CmptParser::parse(ByteView(bytes));
    const auto second = CmptParser::parse(ByteView(bytes));

    ASSERT_EQ(first.children.size(), 2U);
    ASSERT_EQ(first.children[1U].children.size(), 2U);
    EXPECT_EQ(first.children[0U].ordinal_path,
              (std::vector<std::uint32_t>{0U}));
    EXPECT_EQ(first.children[1U].children[0U].ordinal_path,
              (std::vector<std::uint32_t>{1U, 0U}));
    EXPECT_EQ(first.children[1U].children[1U].kind,
              CompositeChildKind::unsupported);
    EXPECT_EQ(first.children[1U].children[0U].child_id,
              second.children[1U].children[0U].child_id);
}

TEST(CmptParserTest, EnforcesRecursiveDepthLimit) {
    const auto glb = normalization::MeshCanonicalWriter::write(
            triangleScene()).glb;
    const auto bytes = tests::makeCompositeFixture(
            glb, tests::makeQuantizedPntsFixture().bytes);
    CmptLimits limits;
    limits.maximum_depth = 1U;
    EXPECT_THROW(CmptParser::parse(ByteView(bytes), limits),
                 FormatError);
}

}  // namespace
}  // namespace clip_worker::formats
