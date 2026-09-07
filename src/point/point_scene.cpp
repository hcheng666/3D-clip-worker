#include "clip_worker/point/point_scene.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace clip_worker::point {
namespace {

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(formats::FormatErrorCode::pnts_feature_table_invalid,
                               message);
}

}  // namespace

void validatePointScene(PointScene& scene) {
    if (scene.positions.empty() || scene.positions.size() % 3U != 0U) {
        invalid("Point positions must contain one or more VEC3 values");
    }
    const std::size_t count = scene.pointCount();
    if (!scene.normals.empty() && scene.normals.size() != count * 3U) {
        invalid("Point normal count differs from POSITION");
    }
    if (!scene.colors_rgba.empty() && scene.colors_rgba.size() != count * 4U) {
        invalid("Point color count differs from POSITION");
    }
    if (!scene.feature_ids.empty() && scene.feature_ids.size() != count) {
        invalid("Point feature ID count differs from POSITION");
    }
    scene.bounds = {};
    for (std::size_t point = 0U; point < count; ++point) {
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            const double value = scene.positions[point * 3U + axis];
            if (!std::isfinite(value)) invalid("Point position is non-finite");
            if (!scene.bounds.valid) {
                scene.bounds.minimum[axis] = value;
                scene.bounds.maximum[axis] = value;
            } else {
                scene.bounds.minimum[axis] =
                        std::min(scene.bounds.minimum[axis], value);
                scene.bounds.maximum[axis] =
                        std::max(scene.bounds.maximum[axis], value);
            }
        }
        scene.bounds.valid = true;
        if (!scene.normals.empty()) {
            const double x = scene.normals[point * 3U];
            const double y = scene.normals[point * 3U + 1U];
            const double z = scene.normals[point * 3U + 2U];
            const double length = std::sqrt(x * x + y * y + z * z);
            if (!std::isfinite(length) || std::abs(length - 1.0) > 1.0e-4) {
                throw formats::FormatError(
                        formats::FormatErrorCode::pnts_normal_invalid,
                        "Point normal is not finite and unit length");
            }
        }
    }
    if (scene.legacy_properties.has_value()) {
        const auto feature_count = scene.legacy_properties->feature_count;
        if (scene.feature_ids.empty() && feature_count > 1U) {
            invalid("Multiple property rows require BATCH_ID");
        }
        for (const std::uint32_t id : scene.feature_ids) {
            if (id >= feature_count) invalid("Point feature ID exceeds property rows");
        }
    }
    if (scene.feature_metadata.has_value()) {
        if (!scene.feature_metadata->primary_property_table.has_value()
                || *scene.feature_metadata->primary_property_table
                        >= scene.feature_metadata->property_tables.size()) {
            throw formats::FormatError(
                    formats::FormatErrorCode::metadata_property_table_invalid,
                    "Point metadata primary property table is invalid");
        }
        const std::uint32_t row_count = scene.feature_metadata
                ->property_tables.at(
                        *scene.feature_metadata->primary_property_table)
                .row_count;
        if (scene.feature_ids.empty() && row_count > 1U) {
            throw formats::FormatError(
                    formats::FormatErrorCode::metadata_feature_id_invalid,
                    "Multiple point metadata rows require feature IDs");
        }
        for (const std::uint32_t id : scene.feature_ids) {
            if (id >= row_count) {
                throw formats::FormatError(
                        formats::FormatErrorCode::metadata_feature_id_invalid,
                        "Point feature ID exceeds typed metadata rows");
            }
        }
    }
}

}  // namespace clip_worker::point
