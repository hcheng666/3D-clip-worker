#include "clip_worker/mesh/mesh_scene.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <gtest/gtest.h>

namespace clip_worker::mesh {
namespace {

MeshScene triangleScene() {
    MeshPrimitive primitive;
    primitive.positions = {0.0F, 0.0F, 0.0F,
                           1.0F, 0.0F, 0.0F,
                           0.0F, 1.0F, 0.0F};
    primitive.texcoords_0 = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    primitive.indices = {0U, 1U, 2U};
    MeshScene scene;
    scene.scenes = {{0U}};
    scene.nodes.push_back({0U, geometry::Matrix4::identity(), 0U, {}});
    scene.meshes.push_back({{primitive}, {}});
    return scene;
}

TEST(MeshSceneTest, ValidatesCardinalityAndRecomputesBounds) {
    MeshScene scene = triangleScene();

    validateMeshScene(scene);

    ASSERT_TRUE(scene.meshes.front().bounds.valid);
    EXPECT_EQ(scene.meshes.front().bounds.minimum,
              (std::array<double, 3>{0.0, 0.0, 0.0}));
    EXPECT_EQ(scene.meshes.front().bounds.maximum,
              (std::array<double, 3>{1.0, 1.0, 0.0}));
}

TEST(MeshSceneTest, RejectsAttributeAndIndexDrift) {
    MeshScene scene = triangleScene();
    scene.meshes.front().primitives.front().texcoords_0.pop_back();
    EXPECT_THROW(validateMeshScene(scene), formats::FormatError);

    scene = triangleScene();
    scene.meshes.front().primitives.front().indices.back() = 3U;
    EXPECT_THROW(validateMeshScene(scene), formats::FormatError);
}

TEST(MeshSceneTest, AccountsResourcesBeforeAllocation) {
    MeshResourceLimits limits;
    limits.maximum_vertices = 3U;
    limits.maximum_indices = 3U;
    MeshResourceAccountant accountant(limits);
    accountant.reserveVertices(3U);
    accountant.reserveIndices(3U);

    EXPECT_EQ(accountant.vertices(), 3U);
    EXPECT_THROW(accountant.reserveVertices(1U), formats::FormatError);
}

TEST(MeshSceneTest, PreservesLegacyAxisTransforms) {
    const std::array<double, 3> point{1.0, 2.0, 3.0};
    EXPECT_EQ(upAxisToZTransform(UpAxis::z).transformPoint(point), point);
    EXPECT_EQ(upAxisToZTransform(UpAxis::y).transformPoint(point),
              (std::array<double, 3>{1.0, -3.0, 2.0}));
    EXPECT_EQ(upAxisToZTransform(UpAxis::x).transformPoint(point),
              (std::array<double, 3>{-3.0, 2.0, 1.0}));
}

}  // namespace
}  // namespace clip_worker::mesh
