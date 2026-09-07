#include "clip_worker/normalization/point_normalizer.hpp"

#include "clip_worker/metadata/legacy_feature_metadata.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/pnts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::pnts_feature_table_invalid, message);
}

[[noreturn]] void unsupported(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::pnts_semantic_unsupported, message);
}

Json parseFeatureTable(const std::string& text) {
    try {
        const Json value = Json::parse(text);
        if (!value.is_object()) invalid("PNTS Feature Table is not an object");
        return value;
    } catch (const formats::FormatError&) {
        throw;
    } catch (const Json::exception&) {
        invalid("PNTS Feature Table contains invalid JSON");
    }
}

void validateKnownSemantics(const Json& table) {
    static const std::set<std::string> kKnown{
            "POINTS_LENGTH", "POSITION", "POSITION_QUANTIZED",
            "QUANTIZED_VOLUME_OFFSET", "QUANTIZED_VOLUME_SCALE", "RTC_CENTER",
            "RGBA", "RGB", "RGB565", "CONSTANT_RGBA", "NORMAL",
            "NORMAL_OCT16P", "BATCH_ID", "BATCH_LENGTH"};
    for (const auto& item : table.items()) {
        if (kKnown.find(item.key()) == kKnown.end()) {
            unsupported("PNTS Feature Table contains an unsupported semantic");
        }
    }
}

std::uint32_t unsignedValue(const Json& table, const char* name, bool required) {
    const auto item = table.find(name);
    if (item == table.end()) {
        if (required) invalid(std::string("PNTS requires ") + name);
        return 0U;
    }
    if (!item->is_number_unsigned()
        || *item > std::numeric_limits<std::uint32_t>::max()) {
        invalid(std::string(name) + " must be an unsigned integer");
    }
    return item->get<std::uint32_t>();
}

std::size_t byteOffset(const Json& semantic, const char* name) {
    if (!semantic.is_object()) invalid(std::string(name) + " must be a binary reference");
    for (const auto& item : semantic.items()) {
        if (item.key() != "byteOffset" && item.key() != "componentType") {
            invalid(std::string(name) + " binary reference contains an unknown field");
        }
    }
    const auto offset = semantic.find("byteOffset");
    if (offset == semantic.end() || !offset->is_number_unsigned()
        || *offset > std::numeric_limits<std::size_t>::max()) {
        invalid(std::string(name) + " byteOffset is invalid");
    }
    return offset->get<std::size_t>();
}

template <typename T>
T readScalar(formats::ByteView bytes, std::size_t offset, const char* name) {
    if (offset % alignof(T) != 0U || !bytes.contains(offset, sizeof(T))) {
        invalid(std::string(name) + " is misaligned or out of range");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
std::vector<T> readValues(formats::ByteView bytes, std::size_t offset,
                          std::uint64_t count, const char* name) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
        invalid(std::string(name) + " byte count overflows");
    }
    const std::size_t byte_count = static_cast<std::size_t>(count) * sizeof(T);
    if (offset % alignof(T) != 0U || !bytes.contains(offset, byte_count)) {
        invalid(std::string(name) + " is misaligned or out of range");
    }
    std::vector<T> result(static_cast<std::size_t>(count));
    if (byte_count > 0U) std::memcpy(result.data(), bytes.data() + offset, byte_count);
    return result;
}

std::array<double, 3U> vec3Value(const Json& table, const char* name,
                                 formats::ByteView binary, bool required) {
    const auto item = table.find(name);
    if (item == table.end()) {
        if (required) invalid(std::string("PNTS requires ") + name);
        return {};
    }
    std::array<double, 3U> result{};
    if (item->is_array()) {
        if (item->size() != result.size()) invalid(std::string(name) + " must be VEC3");
        for (std::size_t axis = 0U; axis < result.size(); ++axis) {
            if (!item->at(axis).is_number()) invalid(std::string(name) + " contains non-number");
            result[axis] = item->at(axis).get<double>();
        }
    } else {
        const std::size_t offset = byteOffset(*item, name);
        for (std::size_t axis = 0U; axis < result.size(); ++axis) {
            result[axis] = readScalar<float>(
                    binary, offset + axis * sizeof(float), name);
        }
    }
    if (std::any_of(result.begin(), result.end(),
                    [](double value) { return !std::isfinite(value); })) {
        invalid(std::string(name) + " contains non-finite value");
    }
    return result;
}

std::array<float, 3U> zUpToYUp(double x, double y, double z) {
    const std::array<double, 3U> converted{x, z, -y};
    std::array<float, 3U> result{};
    for (std::size_t axis = 0U; axis < result.size(); ++axis) {
        if (!std::isfinite(converted[axis])
            || converted[axis] < -std::numeric_limits<float>::max()
            || converted[axis] > std::numeric_limits<float>::max()) {
            throw formats::FormatError(
                    formats::FormatErrorCode::pnts_quantization_invalid,
                    "PNTS position cannot be represented as canonical FLOAT");
        }
        result[axis] = static_cast<float>(converted[axis]);
    }
    return result;
}

std::array<float, 3U> decodeOct(std::uint8_t encoded_x,
                                std::uint8_t encoded_y) {
    double x = static_cast<double>(encoded_x) / 255.0 * 2.0 - 1.0;
    double y = static_cast<double>(encoded_y) / 255.0 * 2.0 - 1.0;
    double z = 1.0 - std::abs(x) - std::abs(y);
    if (z < 0.0) {
        const double previous_x = x;
        x = (1.0 - std::abs(y)) * (previous_x < 0.0 ? -1.0 : 1.0);
        y = (1.0 - std::abs(previous_x)) * (y < 0.0 ? -1.0 : 1.0);
    }
    const double length = std::sqrt(x * x + y * y + z * z);
    if (!(length > 0.0) || !std::isfinite(length)) {
        throw formats::FormatError(formats::FormatErrorCode::pnts_normal_invalid,
                                   "PNTS oct-encoded normal is invalid");
    }
    return zUpToYUp(x / length, y / length, z / length);
}

std::uint8_t expand5(std::uint16_t value) {
    return static_cast<std::uint8_t>((value * 255U + 15U) / 31U);
}

std::uint8_t expand6(std::uint16_t value) {
    return static_cast<std::uint8_t>((value * 255U + 31U) / 63U);
}

void accountDecodedBytes(std::uint64_t point_count,
                         const point::PointScene& scene,
                         const PointResourceLimits& limits) {
    std::uint64_t bytes = point_count * 3U * sizeof(float);
    bytes += scene.normals.size() * sizeof(float);
    bytes += scene.colors_rgba.size() * sizeof(std::uint8_t);
    bytes += scene.feature_ids.size() * sizeof(std::uint32_t);
    if (bytes > limits.maximum_decoded_bytes) {
        unsupported("PNTS decoded streams exceed the configured byte limit");
    }
}

}  // namespace

PointNormalizer::PointNormalizer(PointResourceLimits limits)
    : limits_(limits) {
}

point::PointScene PointNormalizer::normalize(formats::ByteView source) const {
    const formats::PntsDocument document = formats::PntsParser::parse(source);
    const Json table = parseFeatureTable(document.feature_table_json_text);
    validateKnownSemantics(table);
    const std::uint32_t count = unsignedValue(table, "POINTS_LENGTH", true);
    if (count == 0U || count > limits_.maximum_points) {
        unsupported("PNTS POINTS_LENGTH is zero or exceeds the configured limit");
    }
    const formats::ByteView feature_binary = source.subview(
            document.feature_table_binary.offset,
            document.feature_table_binary.byte_length);
    point::PointScene scene;
    scene.positions.reserve(static_cast<std::size_t>(count) * 3U);

    const auto position = table.find("POSITION");
    const auto quantized = table.find("POSITION_QUANTIZED");
    if (position != table.end()) {
        const auto values = readValues<float>(
                feature_binary, byteOffset(*position, "POSITION"),
                static_cast<std::uint64_t>(count) * 3U, "POSITION");
        for (std::size_t index = 0U; index < values.size(); index += 3U) {
            const auto converted = zUpToYUp(
                    values[index], values[index + 1U], values[index + 2U]);
            scene.positions.insert(scene.positions.end(), converted.begin(), converted.end());
        }
    } else if (quantized != table.end()) {
        const auto offset = vec3Value(
                table, "QUANTIZED_VOLUME_OFFSET", feature_binary, true);
        const auto scale = vec3Value(
                table, "QUANTIZED_VOLUME_SCALE", feature_binary, true);
        if (std::any_of(scale.begin(), scale.end(),
                        [](double value) { return !(value > 0.0); })) {
            throw formats::FormatError(
                    formats::FormatErrorCode::pnts_quantization_invalid,
                    "PNTS quantized volume scale must be positive");
        }
        const auto values = readValues<std::uint16_t>(
                feature_binary, byteOffset(*quantized, "POSITION_QUANTIZED"),
                static_cast<std::uint64_t>(count) * 3U,
                "POSITION_QUANTIZED");
        constexpr double kMaximumQuantized = 65535.0;
        for (std::size_t index = 0U; index < values.size(); index += 3U) {
            const auto converted = zUpToYUp(
                    static_cast<double>(values[index]) / kMaximumQuantized
                            * scale[0] + offset[0],
                    static_cast<double>(values[index + 1U]) / kMaximumQuantized
                            * scale[1] + offset[1],
                    static_cast<double>(values[index + 2U]) / kMaximumQuantized
                            * scale[2] + offset[2]);
            scene.positions.insert(scene.positions.end(), converted.begin(), converted.end());
        }
    } else {
        invalid("PNTS requires POSITION or POSITION_QUANTIZED");
    }

    const auto rgba = table.find("RGBA");
    const auto rgb = table.find("RGB");
    const auto rgb565 = table.find("RGB565");
    const auto constant = table.find("CONSTANT_RGBA");
    scene.colors_rgba.reserve(static_cast<std::size_t>(count) * 4U);
    if (rgba != table.end()) {
        scene.colors_rgba = readValues<std::uint8_t>(
                feature_binary, byteOffset(*rgba, "RGBA"),
                static_cast<std::uint64_t>(count) * 4U, "RGBA");
    } else if (rgb != table.end()) {
        const auto values = readValues<std::uint8_t>(
                feature_binary, byteOffset(*rgb, "RGB"),
                static_cast<std::uint64_t>(count) * 3U, "RGB");
        for (std::size_t index = 0U; index < values.size(); index += 3U) {
            scene.colors_rgba.insert(scene.colors_rgba.end(),
                    {values[index], values[index + 1U], values[index + 2U], 255U});
        }
    } else if (rgb565 != table.end()) {
        const auto values = readValues<std::uint16_t>(
                feature_binary, byteOffset(*rgb565, "RGB565"), count, "RGB565");
        for (const std::uint16_t value : values) {
            scene.colors_rgba.insert(scene.colors_rgba.end(),
                    {expand5(static_cast<std::uint16_t>((value >> 11U) & 0x1fU)),
                     expand6(static_cast<std::uint16_t>((value >> 5U) & 0x3fU)),
                     expand5(static_cast<std::uint16_t>(value & 0x1fU)), 255U});
        }
    } else if (constant != table.end()) {
        if (!constant->is_array() || constant->size() != 4U) {
            invalid("CONSTANT_RGBA must contain four unsigned bytes");
        }
        std::array<std::uint8_t, 4U> value{};
        for (std::size_t channel = 0U; channel < value.size(); ++channel) {
            if (!constant->at(channel).is_number_unsigned()
                || constant->at(channel) > 255U) {
                invalid("CONSTANT_RGBA contains an invalid channel");
            }
            value[channel] = constant->at(channel).get<std::uint8_t>();
        }
        for (std::uint32_t point = 0U; point < count; ++point) {
            scene.colors_rgba.insert(scene.colors_rgba.end(), value.begin(), value.end());
        }
    }

    const auto normal = table.find("NORMAL");
    const auto oct = table.find("NORMAL_OCT16P");
    scene.normals.reserve(static_cast<std::size_t>(count) * 3U);
    if (normal != table.end()) {
        const auto values = readValues<float>(
                feature_binary, byteOffset(*normal, "NORMAL"),
                static_cast<std::uint64_t>(count) * 3U, "NORMAL");
        for (std::size_t index = 0U; index < values.size(); index += 3U) {
            const double length = std::sqrt(
                    static_cast<double>(values[index]) * values[index]
                    + static_cast<double>(values[index + 1U]) * values[index + 1U]
                    + static_cast<double>(values[index + 2U]) * values[index + 2U]);
            if (!(length > 0.0) || !std::isfinite(length)) {
                throw formats::FormatError(
                        formats::FormatErrorCode::pnts_normal_invalid,
                        "PNTS NORMAL contains a zero or non-finite vector");
            }
            const auto converted = zUpToYUp(
                    values[index] / length, values[index + 1U] / length,
                    values[index + 2U] / length);
            scene.normals.insert(scene.normals.end(), converted.begin(), converted.end());
        }
    } else if (oct != table.end()) {
        const auto values = readValues<std::uint8_t>(
                feature_binary, byteOffset(*oct, "NORMAL_OCT16P"),
                static_cast<std::uint64_t>(count) * 2U, "NORMAL_OCT16P");
        for (std::size_t index = 0U; index < values.size(); index += 2U) {
            const auto converted = decodeOct(values[index], values[index + 1U]);
            scene.normals.insert(scene.normals.end(), converted.begin(), converted.end());
        }
    }

    const std::uint32_t batch_length = unsignedValue(table, "BATCH_LENGTH", false);
    const auto batch_id = table.find("BATCH_ID");
    if (batch_id != table.end()) {
        if (batch_length == 0U) invalid("BATCH_ID requires positive BATCH_LENGTH");
        const std::size_t offset = byteOffset(*batch_id, "BATCH_ID");
        const std::string component_type = batch_id->value(
                "componentType", std::string("UNSIGNED_SHORT"));
        if (component_type == "UNSIGNED_BYTE") {
            const auto values = readValues<std::uint8_t>(feature_binary, offset, count, "BATCH_ID");
            scene.feature_ids.assign(values.begin(), values.end());
        } else if (component_type == "UNSIGNED_SHORT") {
            const auto values = readValues<std::uint16_t>(feature_binary, offset, count, "BATCH_ID");
            scene.feature_ids.assign(values.begin(), values.end());
        } else if (component_type == "UNSIGNED_INT") {
            scene.feature_ids = readValues<std::uint32_t>(feature_binary, offset, count, "BATCH_ID");
        } else {
            unsupported("PNTS BATCH_ID componentType is unsupported");
        }
        if (std::any_of(scene.feature_ids.begin(), scene.feature_ids.end(),
                        [batch_length](std::uint32_t id) {
                            return id >= batch_length;
                        })) {
            invalid("PNTS BATCH_ID exceeds BATCH_LENGTH");
        }
    } else if (batch_length == 1U) {
        scene.feature_ids.assign(count, 0U);
    } else if (batch_length > 1U) {
        invalid("PNTS BATCH_LENGTH greater than one requires BATCH_ID");
    }

    const bool has_batch_properties =
            (!document.batch_table_json_text.empty()
             && !Json::parse(document.batch_table_json_text).empty())
            || document.batch_table_binary.byte_length > 0U;
    if (batch_length > 0U || has_batch_properties) {
        const std::uint32_t property_row_count = batch_length > 0U
                ? batch_length : count;
        if (batch_length == 0U && scene.feature_ids.empty()) {
            // PNTS permits a per-point Batch Table without BATCH_ID/BATCH_LENGTH.
            scene.feature_ids.resize(count);
            for (std::uint32_t index = 0U; index < count; ++index) {
                scene.feature_ids[index] = index;
            }
        }
        const auto batch_binary = source.subview(
                document.batch_table_binary.offset,
                document.batch_table_binary.byte_length);
        if (limits_.enable_feature_metadata) {
            scene.feature_metadata = metadata::readLegacyFeatureMetadata(
                    document.batch_table_json_text, batch_binary,
                    property_row_count, limits_.feature_metadata);
        } else {
            scene.legacy_properties = metadata::readLegacyPropertyTable(
                    document.batch_table_json_text, batch_binary,
                    property_row_count, limits_.metadata);
        }
    }

    const auto rtc = table.find("RTC_CENTER");
    if (rtc != table.end()) {
        const auto source_rtc = vec3Value(table, "RTC_CENTER", feature_binary, true);
        const auto canonical = zUpToYUp(source_rtc[0], source_rtc[1], source_rtc[2]);
        scene.root_transform = geometry::Matrix4::translation(
                {canonical[0], canonical[1], canonical[2]});
    }
    accountDecodedBytes(count, scene, limits_);
    point::validatePointScene(scene);
    return scene;
}

}  // namespace clip_worker::normalization
