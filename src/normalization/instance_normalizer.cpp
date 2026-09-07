#include "clip_worker/normalization/instance_normalizer.hpp"

#include "clip_worker/metadata/legacy_feature_metadata.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/i3dm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;
using Matrix3 = std::array<double, 9U>;

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::i3dm_feature_table_invalid, message);
}

[[noreturn]] void unsupported(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::i3dm_semantic_unsupported, message);
}

Json parseFeatureTable(const std::string& text) {
    try {
        const Json table = Json::parse(text);
        if (!table.is_object()) invalid("I3DM Feature Table is not an object");
        return table;
    } catch (const formats::FormatError&) {
        throw;
    } catch (const Json::exception&) {
        invalid("I3DM Feature Table contains invalid JSON");
    }
}

void validateKnownSemantics(const Json& table) {
    static const std::set<std::string> kKnown{
            "INSTANCES_LENGTH", "POSITION", "POSITION_QUANTIZED",
            "QUANTIZED_VOLUME_OFFSET", "QUANTIZED_VOLUME_SCALE", "NORMAL_UP",
            "NORMAL_RIGHT", "NORMAL_UP_OCT32P", "NORMAL_RIGHT_OCT32P",
            "SCALE", "SCALE_NON_UNIFORM", "BATCH_ID", "BATCH_LENGTH",
            "RTC_CENTER", "EAST_NORTH_UP"};
    for (const auto& item : table.items()) {
        if (kKnown.find(item.key()) == kKnown.end()) {
            unsupported("I3DM Feature Table contains an unsupported semantic");
        }
    }
}

std::uint32_t unsignedValue(const Json& table, const char* name, bool required) {
    const auto item = table.find(name);
    if (item == table.end()) {
        if (required) invalid(std::string("I3DM requires ") + name);
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
            invalid(std::string(name) + " contains an unknown descriptor field");
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
        if (required) invalid(std::string("I3DM requires ") + name);
        return {};
    }
    std::array<double, 3U> result{};
    if (item->is_array()) {
        if (item->size() != 3U) invalid(std::string(name) + " must contain VEC3");
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            if (!item->at(axis).is_number()) invalid(std::string(name) + " contains non-number");
            result[axis] = item->at(axis).get<double>();
        }
    } else {
        const auto values = readValues<float>(binary, byteOffset(*item, name), 3U, name);
        std::copy(values.begin(), values.end(), result.begin());
    }
    if (std::any_of(result.begin(), result.end(),
                    [](double value) { return !std::isfinite(value); })) {
        invalid(std::string(name) + " contains non-finite value");
    }
    return result;
}

std::array<double, 3U> normalizeVector(std::array<double, 3U> value,
                                       const char* semantic) {
    const double length = std::sqrt(value[0] * value[0] + value[1] * value[1]
                                    + value[2] * value[2]);
    if (!(length > 0.0) || !std::isfinite(length)) {
        throw formats::FormatError(
                formats::FormatErrorCode::i3dm_orientation_invalid,
                std::string(semantic) + " is zero or non-finite");
    }
    for (double& component : value) component /= length;
    return value;
}

std::array<double, 3U> cross(const std::array<double, 3U>& left,
                             const std::array<double, 3U>& right) {
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

double dot(const std::array<double, 3U>& left,
           const std::array<double, 3U>& right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

std::array<double, 3U> decodeOct(std::uint16_t encoded_x,
                                 std::uint16_t encoded_y) {
    double x = static_cast<double>(encoded_x) / 65535.0 * 2.0 - 1.0;
    double y = static_cast<double>(encoded_y) / 65535.0 * 2.0 - 1.0;
    double z = 1.0 - std::abs(x) - std::abs(y);
    if (z < 0.0) {
        const double previous_x = x;
        x = (1.0 - std::abs(y)) * (previous_x < 0.0 ? -1.0 : 1.0);
        y = (1.0 - std::abs(previous_x)) * (y < 0.0 ? -1.0 : 1.0);
    }
    return normalizeVector({x, y, z}, "I3DM oct orientation");
}

Matrix3 multiply(const Matrix3& left, const Matrix3& right) {
    Matrix3 result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            for (std::size_t inner = 0U; inner < 3U; ++inner) {
                result[row * 3U + column] +=
                        left[row * 3U + inner] * right[inner * 3U + column];
            }
        }
    }
    return result;
}

Matrix3 transpose(const Matrix3& source) {
    return {source[0], source[3], source[6],
            source[1], source[4], source[7],
            source[2], source[5], source[8]};
}

std::array<float, 4U> quaternion(Matrix3 matrix) {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
    const double trace = matrix[0] + matrix[4] + matrix[8];
    if (trace > 0.0) {
        const double scale = std::sqrt(trace + 1.0) * 2.0;
        w = 0.25 * scale;
        x = (matrix[7] - matrix[5]) / scale;
        y = (matrix[2] - matrix[6]) / scale;
        z = (matrix[3] - matrix[1]) / scale;
    } else if (matrix[0] > matrix[4] && matrix[0] > matrix[8]) {
        const double scale = std::sqrt(1.0 + matrix[0] - matrix[4] - matrix[8]) * 2.0;
        w = (matrix[7] - matrix[5]) / scale;
        x = 0.25 * scale;
        y = (matrix[1] + matrix[3]) / scale;
        z = (matrix[2] + matrix[6]) / scale;
    } else if (matrix[4] > matrix[8]) {
        const double scale = std::sqrt(1.0 + matrix[4] - matrix[0] - matrix[8]) * 2.0;
        w = (matrix[2] - matrix[6]) / scale;
        x = (matrix[1] + matrix[3]) / scale;
        y = 0.25 * scale;
        z = (matrix[5] + matrix[7]) / scale;
    } else {
        const double scale = std::sqrt(1.0 + matrix[8] - matrix[0] - matrix[4]) * 2.0;
        w = (matrix[3] - matrix[1]) / scale;
        x = (matrix[2] + matrix[6]) / scale;
        y = (matrix[5] + matrix[7]) / scale;
        z = 0.25 * scale;
    }
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(length > 0.0) || !std::isfinite(length)) {
        throw formats::FormatError(
                formats::FormatErrorCode::i3dm_orientation_invalid,
                "I3DM orientation cannot be converted to quaternion");
    }
    x /= length;
    y /= length;
    z /= length;
    w /= length;
    if (w < 0.0 || (w == 0.0 && (z < 0.0 || (z == 0.0
            && (y < 0.0 || (y == 0.0 && x < 0.0)))))) {
        x = -x;
        y = -y;
        z = -z;
        w = -w;
    }
    return {static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(z), static_cast<float>(w)};
}

std::array<float, 4U> canonicalOrientation(
        const std::array<double, 3U>& source_right,
        const std::array<double, 3U>& source_up) {
    const auto right = normalizeVector(source_right, "NORMAL_RIGHT");
    const auto up = normalizeVector(source_up, "NORMAL_UP");
    if (std::abs(dot(right, up)) > 1.0e-3) {
        throw formats::FormatError(
                formats::FormatErrorCode::i3dm_orientation_invalid,
                "I3DM NORMAL_RIGHT and NORMAL_UP are not orthogonal");
    }
    const auto forward = normalizeVector(cross(up, right), "I3DM forward axis");
    const auto corrected_up = normalizeVector(cross(right, forward), "I3DM up axis");
    const Matrix3 source_rotation{
            right[0], forward[0], corrected_up[0],
            right[1], forward[1], corrected_up[1],
            right[2], forward[2], corrected_up[2]};
    const Matrix3 z_to_y{1.0, 0.0, 0.0,
                         0.0, 0.0, 1.0,
                         0.0, -1.0, 0.0};
    return quaternion(multiply(multiply(z_to_y, source_rotation),
                               transpose(z_to_y)));
}

std::array<float, 3U> zUpToYUp(double x, double y, double z) {
    const std::array<double, 3U> value{x, z, -y};
    std::array<float, 3U> result{};
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        if (!std::isfinite(value[axis])
            || value[axis] < -std::numeric_limits<float>::max()
            || value[axis] > std::numeric_limits<float>::max()) {
            invalid("I3DM transform cannot be represented as FLOAT");
        }
        result[axis] = static_cast<float>(value[axis]);
    }
    return result;
}

std::string resolveExternalPath(const std::string& root_path,
                                const std::string& uri) {
    if (uri.empty() || uri.front() == '/' || uri.find('\\') != std::string::npos
        || uri.find(':') != std::string::npos || uri.find('?') != std::string::npos
        || uri.find('#') != std::string::npos || uri.find('%') != std::string::npos) {
        unsupported("I3DM external model URI is outside the approved package namespace");
    }
    const std::size_t separator = root_path.rfind('/');
    const std::string parent = separator == std::string::npos
            ? std::string() : root_path.substr(0U, separator + 1U);
    const std::string candidate = parent + uri;
    std::size_t start = 0U;
    while (start <= candidate.size()) {
        const std::size_t end = candidate.find('/', start);
        const std::string segment = candidate.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            unsupported("I3DM external model URI is not canonical");
        }
        if (end == std::string::npos) break;
        start = end + 1U;
    }
    return candidate;
}

std::array<double, 9U> linear(const geometry::Matrix4& matrix) {
    const auto& value = matrix.values();
    return {value[0], value[4], value[8],
            value[1], value[5], value[9],
            value[2], value[6], value[10]};
}

std::array<double, 9U> inverseTranspose(const Matrix3& matrix) {
    const double determinant =
            matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
            - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
            + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) {
        unsupported("I3DM model node transform is singular");
    }
    const double inverse = 1.0 / determinant;
    return {(matrix[4] * matrix[8] - matrix[5] * matrix[7]) * inverse,
            (matrix[5] * matrix[6] - matrix[3] * matrix[8]) * inverse,
            (matrix[3] * matrix[7] - matrix[4] * matrix[6]) * inverse,
            (matrix[2] * matrix[7] - matrix[1] * matrix[8]) * inverse,
            (matrix[0] * matrix[8] - matrix[2] * matrix[6]) * inverse,
            (matrix[1] * matrix[6] - matrix[0] * matrix[7]) * inverse,
            (matrix[1] * matrix[5] - matrix[2] * matrix[4]) * inverse,
            (matrix[2] * matrix[3] - matrix[0] * matrix[5]) * inverse,
            (matrix[0] * matrix[4] - matrix[1] * matrix[3]) * inverse};
}

std::array<float, 3U> transformDirection(
        const Matrix3& transform, const std::array<float, 3U>& source) {
    std::array<double, 3U> value{
            transform[0] * source[0] + transform[1] * source[1]
                    + transform[2] * source[2],
            transform[3] * source[0] + transform[4] * source[1]
                    + transform[5] * source[2],
            transform[6] * source[0] + transform[7] * source[1]
                    + transform[8] * source[2]};
    value = normalizeVector(value, "I3DM model direction");
    return {static_cast<float>(value[0]), static_cast<float>(value[1]),
            static_cast<float>(value[2])};
}

mesh::MeshPrimitive transformedPrimitive(
        const mesh::MeshPrimitive& source,
        const geometry::Matrix4& transform) {
    if (!source.feature_ids.empty()) {
        unsupported("I3DM shared model feature IDs are not supported in this tranche");
    }
    mesh::MeshPrimitive result = source;
    for (std::size_t vertex = 0U; vertex < source.vertexCount(); ++vertex) {
        const auto position = transform.transformPoint({
                source.positions[vertex * 3U],
                source.positions[vertex * 3U + 1U],
                source.positions[vertex * 3U + 2U]});
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            if (!std::isfinite(position[axis])
                || position[axis] < -std::numeric_limits<float>::max()
                || position[axis] > std::numeric_limits<float>::max()) {
                unsupported("I3DM flattened model position exceeds FLOAT");
            }
            result.positions[vertex * 3U + axis] =
                    static_cast<float>(position[axis]);
        }
    }
    const Matrix3 normal_transform = inverseTranspose(linear(transform));
    for (std::size_t vertex = 0U; vertex < source.normals.size() / 3U; ++vertex) {
        const auto value = transformDirection(normal_transform,
                {source.normals[vertex * 3U], source.normals[vertex * 3U + 1U],
                 source.normals[vertex * 3U + 2U]});
        std::copy(value.begin(), value.end(),
                  result.normals.begin() + static_cast<std::ptrdiff_t>(vertex * 3U));
    }
    for (std::size_t vertex = 0U; vertex < source.tangents.size() / 4U; ++vertex) {
        const auto value = transformDirection(normal_transform,
                {source.tangents[vertex * 4U], source.tangents[vertex * 4U + 1U],
                 source.tangents[vertex * 4U + 2U]});
        std::copy(value.begin(), value.end(),
                  result.tangents.begin() + static_cast<std::ptrdiff_t>(vertex * 4U));
    }
    mesh::validateMeshPrimitive(result);
    return result;
}

mesh::MeshScene flattenModel(mesh::MeshScene source) {
    mesh::validateMeshScene(source);
    if (source.legacy_properties.has_value()) {
        unsupported("I3DM shared model metadata is not supported in this tranche");
    }
    mesh::Mesh flattened;
    std::vector<bool> visiting(source.nodes.size(), false);
    std::function<void(std::size_t, const geometry::Matrix4&)> visit =
            [&](std::size_t node_index, const geometry::Matrix4& parent) {
        if (visiting.at(node_index)) unsupported("I3DM model node graph is cyclic");
        visiting[node_index] = true;
        const auto& node = source.nodes.at(node_index);
        const geometry::Matrix4 world = parent * node.local_transform;
        if (node.mesh.has_value()) {
            for (const auto& primitive : source.meshes.at(*node.mesh).primitives) {
                flattened.primitives.push_back(
                        transformedPrimitive(primitive, world));
            }
        }
        for (const std::size_t child : node.children) visit(child, world);
        visiting[node_index] = false;
    };
    for (const std::size_t root : source.scenes.at(source.default_scene)) {
        visit(root, geometry::Matrix4::identity());
    }
    if (flattened.primitives.empty()) unsupported("I3DM model default scene has no mesh");
    mesh::MeshScene result;
    result.default_scene = 0U;
    result.scenes = {{0U}};
    result.nodes = {{0U, geometry::Matrix4::identity(), 0U, {}}};
    result.meshes = {std::move(flattened)};
    result.materials = std::move(source.materials);
    result.samplers = std::move(source.samplers);
    result.textures = std::move(source.textures);
    result.images = std::move(source.images);
    result.require_unlit = source.require_unlit;
    mesh::validateMeshScene(result);
    return result;
}

}  // namespace

InstanceNormalizer::InstanceNormalizer(InstanceResourceLimits limits)
    : limits_(limits) {
}

InstanceNormalizationResult InstanceNormalizer::normalize(
        const InstanceNormalizationInput& input) const {
    const formats::I3dmDocument document = formats::I3dmParser::parse(
            formats::ByteView(input.source_bytes));
    const Json table = parseFeatureTable(document.feature_table_json_text);
    validateKnownSemantics(table);
    const std::uint32_t count = unsignedValue(table, "INSTANCES_LENGTH", true);
    if (count == 0U || count > limits_.maximum_instances) {
        unsupported("I3DM INSTANCES_LENGTH is zero or exceeds the configured limit");
    }
    const formats::ByteView feature_binary(input.source_bytes.data()
                    + document.feature_table_binary.offset,
            document.feature_table_binary.byte_length);
    mesh::MeshNodeInstancing instances;
    instances.translations.reserve(static_cast<std::size_t>(count) * 3U);
    const auto position = table.find("POSITION");
    const auto quantized = table.find("POSITION_QUANTIZED");
    if (position != table.end()) {
        const auto values = readValues<float>(feature_binary,
                byteOffset(*position, "POSITION"),
                static_cast<std::uint64_t>(count) * 3U, "POSITION");
        for (std::size_t index = 0U; index < values.size(); index += 3U) {
            const auto converted = zUpToYUp(
                    values[index], values[index + 1U], values[index + 2U]);
            instances.translations.insert(instances.translations.end(),
                                          converted.begin(), converted.end());
        }
    } else if (quantized != table.end()) {
        const auto offset = vec3Value(table, "QUANTIZED_VOLUME_OFFSET",
                                      feature_binary, true);
        const auto scale = vec3Value(table, "QUANTIZED_VOLUME_SCALE",
                                     feature_binary, true);
        if (std::any_of(scale.begin(), scale.end(),
                        [](double value) { return !(value > 0.0); })) {
            invalid("I3DM quantized volume scale must be positive");
        }
        const auto values = readValues<std::uint16_t>(feature_binary,
                byteOffset(*quantized, "POSITION_QUANTIZED"),
                static_cast<std::uint64_t>(count) * 3U,
                "POSITION_QUANTIZED");
        for (std::size_t index = 0U; index < values.size(); index += 3U) {
            const auto converted = zUpToYUp(
                    values[index] / 65535.0 * scale[0] + offset[0],
                    values[index + 1U] / 65535.0 * scale[1] + offset[1],
                    values[index + 2U] / 65535.0 * scale[2] + offset[2]);
            instances.translations.insert(instances.translations.end(),
                                          converted.begin(), converted.end());
        }
    } else {
        invalid("I3DM requires POSITION or POSITION_QUANTIZED");
    }

    const bool has_explicit = table.contains("NORMAL_UP")
            || table.contains("NORMAL_RIGHT");
    const bool has_oct = table.contains("NORMAL_UP_OCT32P")
            || table.contains("NORMAL_RIGHT_OCT32P");
    if ((table.contains("NORMAL_UP") != table.contains("NORMAL_RIGHT"))
        || (table.contains("NORMAL_UP_OCT32P")
            != table.contains("NORMAL_RIGHT_OCT32P"))
        || (has_explicit && has_oct)) {
        throw formats::FormatError(
                formats::FormatErrorCode::i3dm_orientation_invalid,
                "I3DM orientation semantics must form exactly one complete pair");
    }
    const bool east_north_up = table.value("EAST_NORTH_UP", false);
    if (east_north_up && !has_explicit && !has_oct) {
        throw formats::FormatError(formats::FormatErrorCode::i3dm_enu_unsupported,
                                   "I3DM ENU-only orientation is unsupported");
    }
    instances.rotations.reserve(static_cast<std::size_t>(count) * 4U);
    if (has_explicit) {
        const auto up = readValues<float>(feature_binary,
                byteOffset(table.at("NORMAL_UP"), "NORMAL_UP"),
                static_cast<std::uint64_t>(count) * 3U, "NORMAL_UP");
        const auto right = readValues<float>(feature_binary,
                byteOffset(table.at("NORMAL_RIGHT"), "NORMAL_RIGHT"),
                static_cast<std::uint64_t>(count) * 3U, "NORMAL_RIGHT");
        for (std::size_t index = 0U; index < count; ++index) {
            const auto value = canonicalOrientation(
                    {right[index * 3U], right[index * 3U + 1U],
                     right[index * 3U + 2U]},
                    {up[index * 3U], up[index * 3U + 1U],
                     up[index * 3U + 2U]});
            instances.rotations.insert(instances.rotations.end(),
                                       value.begin(), value.end());
        }
    } else if (has_oct) {
        const auto up = readValues<std::uint16_t>(feature_binary,
                byteOffset(table.at("NORMAL_UP_OCT32P"), "NORMAL_UP_OCT32P"),
                static_cast<std::uint64_t>(count) * 2U, "NORMAL_UP_OCT32P");
        const auto right = readValues<std::uint16_t>(feature_binary,
                byteOffset(table.at("NORMAL_RIGHT_OCT32P"), "NORMAL_RIGHT_OCT32P"),
                static_cast<std::uint64_t>(count) * 2U, "NORMAL_RIGHT_OCT32P");
        for (std::size_t index = 0U; index < count; ++index) {
            const auto value = canonicalOrientation(
                    decodeOct(right[index * 2U], right[index * 2U + 1U]),
                    decodeOct(up[index * 2U], up[index * 2U + 1U]));
            instances.rotations.insert(instances.rotations.end(),
                                       value.begin(), value.end());
        }
    } else {
        for (std::uint32_t index = 0U; index < count; ++index) {
            instances.rotations.insert(instances.rotations.end(),
                                       {0.0F, 0.0F, 0.0F, 1.0F});
        }
    }

    std::vector<float> scalar_scale(count, 1.0F);
    if (table.contains("SCALE")) {
        scalar_scale = readValues<float>(feature_binary,
                byteOffset(table.at("SCALE"), "SCALE"), count, "SCALE");
    }
    std::vector<float> non_uniform(static_cast<std::size_t>(count) * 3U, 1.0F);
    if (table.contains("SCALE_NON_UNIFORM")) {
        non_uniform = readValues<float>(feature_binary,
                byteOffset(table.at("SCALE_NON_UNIFORM"), "SCALE_NON_UNIFORM"),
                static_cast<std::uint64_t>(count) * 3U, "SCALE_NON_UNIFORM");
    }
    instances.scales.reserve(static_cast<std::size_t>(count) * 3U);
    for (std::size_t index = 0U; index < count; ++index) {
        const double x = scalar_scale[index] * non_uniform[index * 3U];
        const double y = scalar_scale[index] * non_uniform[index * 3U + 1U];
        const double z = scalar_scale[index] * non_uniform[index * 3U + 2U];
        if (!(x > 0.0) || !(y > 0.0) || !(z > 0.0)
            || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            throw formats::FormatError(formats::FormatErrorCode::i3dm_scale_invalid,
                                       "I3DM scale must be finite and positive");
        }
        instances.scales.insert(instances.scales.end(),
                {static_cast<float>(x), static_cast<float>(z),
                 static_cast<float>(y)});
    }

    const std::uint32_t batch_length = unsignedValue(table, "BATCH_LENGTH", false);
    if (table.contains("BATCH_ID")) {
        if (batch_length == 0U) invalid("I3DM BATCH_ID requires BATCH_LENGTH");
        const auto& descriptor = table.at("BATCH_ID");
        const std::string component_type = descriptor.value(
                "componentType", std::string("UNSIGNED_SHORT"));
        const std::size_t offset = byteOffset(descriptor, "BATCH_ID");
        if (component_type == "UNSIGNED_BYTE") {
            const auto values = readValues<std::uint8_t>(feature_binary, offset, count, "BATCH_ID");
            instances.feature_ids.assign(values.begin(), values.end());
        } else if (component_type == "UNSIGNED_SHORT") {
            const auto values = readValues<std::uint16_t>(feature_binary, offset, count, "BATCH_ID");
            instances.feature_ids.assign(values.begin(), values.end());
        } else if (component_type == "UNSIGNED_INT") {
            instances.feature_ids = readValues<std::uint32_t>(feature_binary, offset, count, "BATCH_ID");
        } else {
            unsupported("I3DM BATCH_ID componentType is unsupported");
        }
        if (std::any_of(instances.feature_ids.begin(), instances.feature_ids.end(),
                        [batch_length](std::uint32_t id) { return id >= batch_length; })) {
            invalid("I3DM BATCH_ID exceeds BATCH_LENGTH");
        }
    } else if (batch_length == 1U) {
        instances.feature_ids.assign(count, 0U);
    } else if (batch_length > 1U) {
        invalid("I3DM BATCH_LENGTH greater than one requires BATCH_ID");
    }
    if (instances.feature_ids.empty()) {
        instances.feature_ids.resize(count);
        for (std::uint32_t index = 0U; index < count; ++index) {
            instances.feature_ids[index] = index;
        }
    }

    std::vector<std::uint8_t> model_bytes;
    std::string model_path = input.root_package_relative_path;
    formats::GltfContentKind model_kind = formats::GltfContentKind::glb;
    InstanceNormalizationResult result;
    if (document.gltf_format == formats::I3dmGltfFormat::embedded_glb) {
        const auto model_begin = input.source_bytes.begin()
                + static_cast<std::ptrdiff_t>(document.gltf_payload.offset);
        const auto model_end = model_begin
                + static_cast<std::ptrdiff_t>(
                        document.gltf_payload.byte_length);
        model_bytes.assign(model_begin, model_end);
    } else {
        result.external_model = true;
        model_path = resolveExternalPath(input.root_package_relative_path,
                                         document.external_gltf_uri);
        const auto approved = input.approved_resources.find(model_path);
        if (approved == input.approved_resources.end()) {
            unsupported("I3DM external model is absent from the approved manifest");
        }
        model_bytes = approved->second.bytes;
        const bool glb_magic = model_bytes.size() >= 4U
                && std::equal(model_bytes.begin(), model_bytes.begin() + 4U,
                              std::array<std::uint8_t, 4U>{'g', 'l', 'T', 'F'}.begin());
        model_kind = glb_magic ? formats::GltfContentKind::glb
                               : formats::GltfContentKind::gltf;
    }
    auto model = formats::GltfMeshReader::read(
            model_bytes, model_kind, model_path,
            input.approved_resources, limits_.model);
    result.model_diagnostics = model.diagnostics;
    result.scene.model = flattenModel(std::move(model.scene));
    result.scene.model.nodes.front().instancing = std::move(instances);

    const bool has_batch_properties =
            (!document.batch_table_json_text.empty()
             && !Json::parse(document.batch_table_json_text).empty())
            || document.batch_table_binary.byte_length > 0U;
    if (batch_length > 0U || has_batch_properties) {
        // Without BATCH_ID/BATCH_LENGTH, I3DM properties are per-instance.
        const std::uint32_t property_row_count = batch_length > 0U
                ? batch_length : count;
        const formats::ByteView batch_binary(
                input.source_bytes.data() + document.batch_table_binary.offset,
                document.batch_table_binary.byte_length);
        if (limits_.enable_feature_metadata) {
            result.scene.model.feature_metadata =
                    metadata::readLegacyFeatureMetadata(
                            document.batch_table_json_text, batch_binary,
                            property_row_count, limits_.feature_metadata);
        } else {
            result.scene.model.legacy_properties =
                    metadata::readLegacyPropertyTable(
                            document.batch_table_json_text, batch_binary,
                            property_row_count, limits_.metadata);
        }
    }
    if (table.contains("RTC_CENTER")) {
        const auto source_rtc = vec3Value(table, "RTC_CENTER", feature_binary, true);
        const auto canonical = zUpToYUp(source_rtc[0], source_rtc[1], source_rtc[2]);
        result.scene.root_transform = geometry::Matrix4::translation(
                {canonical[0], canonical[1], canonical[2]});
    }
    instance::validateInstanceScene(result.scene);
    return result;
}

}  // namespace clip_worker::normalization
