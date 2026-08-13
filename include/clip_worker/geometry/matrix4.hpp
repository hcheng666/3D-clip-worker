#pragma once

#include <array>
#include <cstddef>

namespace clip_worker::geometry {

/** glTF/3D Tiles列主序4x4仿射矩阵。 */
class Matrix4 final {
public:
    [[nodiscard]] static Matrix4 identity();
    [[nodiscard]] static Matrix4 fromColumnMajor(const std::array<double, 16>& values);
    [[nodiscard]] static Matrix4 translation(const std::array<double, 3>& values);
    [[nodiscard]] static Matrix4 scale(const std::array<double, 3>& values);
    [[nodiscard]] static Matrix4 quaternion(const std::array<double, 4>& xyzw);

    [[nodiscard]] Matrix4 operator*(const Matrix4& other) const;
    [[nodiscard]] std::array<double, 3> transformPoint(
            const std::array<double, 3>& point) const;
    [[nodiscard]] const std::array<double, 16>& values() const noexcept;

private:
    explicit Matrix4(std::array<double, 16> values);
    [[nodiscard]] double at(std::size_t row, std::size_t column) const;

    std::array<double, 16> values_{};
};

}  // namespace clip_worker::geometry
