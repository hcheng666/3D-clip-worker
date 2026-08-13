#include "clip_worker/geometry/matrix4.hpp"

#include <cmath>

#include <gtest/gtest.h>

namespace clip_worker::geometry {
namespace {

TEST(Matrix4Test, AppliesColumnMajorTrsInGltfOrder) {
    const Matrix4 transform = Matrix4::translation({10.0, 20.0, 30.0})
                              * Matrix4::scale({2.0, 3.0, 4.0});

    const auto result = transform.transformPoint({1.0, 2.0, 3.0});

    EXPECT_DOUBLE_EQ(result[0], 12.0);
    EXPECT_DOUBLE_EQ(result[1], 26.0);
    EXPECT_DOUBLE_EQ(result[2], 42.0);
}

TEST(Matrix4Test, RotatesUsingXyzwQuaternion) {
    const double half_sqrt = std::sqrt(0.5);
    const Matrix4 rotation = Matrix4::quaternion({0.0, 0.0, half_sqrt, half_sqrt});

    const auto result = rotation.transformPoint({1.0, 0.0, 0.0});

    EXPECT_NEAR(result[0], 0.0, 1.0e-12);
    EXPECT_NEAR(result[1], 1.0, 1.0e-12);
}

}  // namespace
}  // namespace clip_worker::geometry
