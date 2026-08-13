#include "clip_worker/geometry/matrix4.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace clip_worker::geometry {

Matrix4::Matrix4(std::array<double, 16> values) : values_(std::move(values)) {
}

Matrix4 Matrix4::identity() {
    return Matrix4({1.0, 0.0, 0.0, 0.0,
                    0.0, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0,
                    0.0, 0.0, 0.0, 1.0});
}

Matrix4 Matrix4::fromColumnMajor(const std::array<double, 16>& values) {
    for (const double value : values) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Transform matrix contains a non-finite value");
        }
    }
    return Matrix4(values);
}

Matrix4 Matrix4::translation(const std::array<double, 3>& values) {
    Matrix4 result = identity();
    result.values_[12] = values[0];
    result.values_[13] = values[1];
    result.values_[14] = values[2];
    return result;
}

Matrix4 Matrix4::scale(const std::array<double, 3>& values) {
    Matrix4 result = identity();
    result.values_[0] = values[0];
    result.values_[5] = values[1];
    result.values_[10] = values[2];
    return result;
}

Matrix4 Matrix4::quaternion(const std::array<double, 4>& xyzw) {
    const double length = std::sqrt(xyzw[0] * xyzw[0] + xyzw[1] * xyzw[1]
                                    + xyzw[2] * xyzw[2] + xyzw[3] * xyzw[3]);
    if (!std::isfinite(length) || length <= 0.0) {
        throw std::invalid_argument("Node quaternion is invalid");
    }
    const double x = xyzw[0] / length;
    const double y = xyzw[1] / length;
    const double z = xyzw[2] / length;
    const double w = xyzw[3] / length;
    return Matrix4({
            1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + z * w),
            2.0 * (x * z - y * w), 0.0,
            2.0 * (x * y - z * w), 1.0 - 2.0 * (x * x + z * z),
            2.0 * (y * z + x * w), 0.0,
            2.0 * (x * z + y * w), 2.0 * (y * z - x * w),
            1.0 - 2.0 * (x * x + y * y), 0.0,
            0.0, 0.0, 0.0, 1.0});
}

Matrix4 Matrix4::operator*(const Matrix4& other) const {
    std::array<double, 16> result{};
    for (std::size_t column = 0; column < 4U; ++column) {
        for (std::size_t row = 0; row < 4U; ++row) {
            double value = 0.0;
            for (std::size_t inner = 0; inner < 4U; ++inner) {
                value += at(row, inner) * other.at(inner, column);
            }
            result[column * 4U + row] = value;
        }
    }
    return Matrix4(result);
}

std::array<double, 3> Matrix4::transformPoint(
        const std::array<double, 3>& point) const {
    const double x = at(0U, 0U) * point[0] + at(0U, 1U) * point[1]
                     + at(0U, 2U) * point[2] + at(0U, 3U);
    const double y = at(1U, 0U) * point[0] + at(1U, 1U) * point[1]
                     + at(1U, 2U) * point[2] + at(1U, 3U);
    const double z = at(2U, 0U) * point[0] + at(2U, 1U) * point[1]
                     + at(2U, 2U) * point[2] + at(2U, 3U);
    const double w = at(3U, 0U) * point[0] + at(3U, 1U) * point[1]
                     + at(3U, 2U) * point[2] + at(3U, 3U);
    if (!std::isfinite(w) || std::abs(w) <= 1.0e-15) {
        throw std::invalid_argument("Transform produced an invalid homogeneous coordinate");
    }
    return {x / w, y / w, z / w};
}

const std::array<double, 16>& Matrix4::values() const noexcept {
    return values_;
}

double Matrix4::at(std::size_t row, std::size_t column) const {
    return values_[column * 4U + row];
}

}  // namespace clip_worker::geometry
