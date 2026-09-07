#include "clip_worker/normalization/canonical_artifact.hpp"

#include "clip_worker/formats/texture_codec.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/geometry/matrix4.hpp"
#include "clip_worker/normalization/mesh_canonical_writer.hpp"
#include "clip_worker/metadata/structural_metadata_reader.hpp"
#include "clip_worker/metadata/property_lookup.hpp"
#include "clip_worker/normalization/metadata_canonical_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kGlbMagic = 0x46546c67U;
constexpr std::uint32_t kGlbVersion = 2U;
constexpr std::uint32_t kJsonChunkType = 0x4e4f534aU;
constexpr std::uint32_t kBinChunkType = 0x004e4942U;
constexpr std::size_t kGlbHeaderBytes = 12U;
constexpr std::size_t kChunkHeaderBytes = 8U;
constexpr int kPointPrimitiveMode = 0;
constexpr int kTrianglePrimitiveMode = 4;
constexpr std::uint32_t kFloatComponentType = 5126U;
constexpr std::uint32_t kUnsignedByteComponentType = 5121U;
constexpr std::uint32_t kUnsignedShortComponentType = 5123U;
constexpr std::uint32_t kUnsignedIntComponentType = 5125U;
constexpr const char* kGpuInstancingExtension = "EXT_mesh_gpu_instancing";
constexpr const char* kMeshFeaturesExtension = "EXT_mesh_features";
constexpr const char* kInstanceFeaturesExtension = "EXT_instance_features";
constexpr const char* kStructuralMetadataExtension = "EXT_structural_metadata";
const std::set<std::string> kApprovedRequiredExtensions = {
        kGpuInstancingExtension,
        kMeshFeaturesExtension,
        kInstanceFeaturesExtension,
        kStructuralMetadataExtension,
        kCanonicalLegacyHierarchyExtension,
        "KHR_materials_unlit"};

struct BufferViewInfo {
    std::uint64_t byte_offset = 0U;
    std::uint64_t byte_length = 0U;
    std::uint64_t byte_stride = 0U;
};

struct AccessorInfo {
    std::size_t buffer_view = 0U;
    std::uint64_t byte_offset = 0U;
    std::uint64_t count = 0U;
    std::uint64_t component_size = 0U;
    std::uint64_t component_count = 0U;
    std::uint64_t element_size = 0U;
    std::uint64_t stride = 0U;
    std::uint32_t component_type = 0U;
    bool normalized = false;
    std::string type;
};

struct Bounds3D {
    std::array<double, 3> minimum{};
    std::array<double, 3> maximum{};
    bool valid = false;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4U,
            "Canonical GLB is truncated");
    return static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
            | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
            | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 2U,
            "Canonical GLB is truncated");
    return static_cast<std::uint16_t>(bytes[offset])
            | static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
}

std::string bytesString(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool isNonNegativeInteger(const Json& value) {
    return value.is_number_unsigned()
            || (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

std::uint64_t requiredUnsigned(const Json& object, const char* name,
                               std::uint64_t default_value,
                               bool required_field) {
    const auto item = object.find(name);
    if (item == object.end()) {
        require(!required_field, "Canonical GLB integer field is missing");
        return default_value;
    }
    require(isNonNegativeInteger(*item),
            "Canonical GLB integer field is invalid");
    return item->get<std::uint64_t>();
}

bool rangeFits(std::uint64_t offset, std::uint64_t length,
               std::uint64_t limit) {
    return offset <= limit && length <= limit - offset;
}

std::uint64_t componentSize(std::uint32_t component_type) {
    switch (component_type) {
        case 5120U:
        case kUnsignedByteComponentType: return 1U;
        case 5122U:
        case kUnsignedShortComponentType: return 2U;
        case kUnsignedIntComponentType:
        case kFloatComponentType: return 4U;
        default:
            throw std::invalid_argument(
                    "Canonical GLB accessor component type is unsupported");
    }
}

std::uint64_t componentCount(const std::string& type) {
    if (type == "SCALAR") return 1U;
    if (type == "VEC2") return 2U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4" || type == "MAT2") return 4U;
    if (type == "MAT3") return 9U;
    if (type == "MAT4") return 16U;
    throw std::invalid_argument("Canonical GLB accessor type is unsupported");
}

std::uint64_t elementSize(const std::string& type,
                          std::uint64_t component_size) {
    if (type == "MAT2" || type == "MAT3" || type == "MAT4") {
        const std::uint64_t dimension = type == "MAT2" ? 2U
                : type == "MAT3" ? 3U : 4U;
        const std::uint64_t column_bytes = dimension * component_size;
        const std::uint64_t aligned_column_bytes =
                (column_bytes + 3U) & ~std::uint64_t{3U};
        return dimension * aligned_column_bytes;
    }
    return componentCount(type) * component_size;
}

void validateNumericArray(const Json& object, const char* name,
                          std::size_t expected_size) {
    const auto item = object.find(name);
    if (item == object.end()) return;
    require(item->is_array() && item->size() == expected_size,
            "Canonical GLB transform array is invalid");
    for (const auto& value : *item) {
        require(value.is_number()
                        && std::isfinite(value.get<double>()),
                "Canonical GLB transform contains a non-finite number");
    }
}

void rejectExternalUris(const Json& value) {
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            if ((item.key() == "uri" || item.key() == "schemaUri")
                    && item.value().is_string()
                    && !item.value().get<std::string>().empty()) {
                throw std::invalid_argument(
                        "Canonical GLB contains an external resource URI");
            }
            rejectExternalUris(item.value());
        }
    } else if (value.is_array()) {
        for (const auto& item : value) rejectExternalUris(item);
    } else if (value.is_number_float()) {
        require(std::isfinite(value.get<double>()),
                "Canonical GLB contains a non-finite number");
    }
}

std::vector<std::string> sortedStrings(const Json& root, const char* name) {
    std::vector<std::string> values;
    const auto item = root.find(name);
    if (item == root.end()) return values;
    require(item->is_array(), "Canonical GLB extension list is invalid");
    for (const auto& value : *item) {
        require(value.is_string() && !value.get_ref<const std::string&>().empty(),
                "Canonical GLB extension name is invalid");
        values.push_back(value.get<std::string>());
    }
    require(std::is_sorted(values.begin(), values.end())
                    && std::adjacent_find(values.begin(), values.end())
                            == values.end(),
            "Canonical GLB extension list is not canonical");
    return values;
}

bool contains(const std::vector<std::string>& values, const char* value) {
    return std::binary_search(values.begin(), values.end(), value);
}

std::uint64_t validateBuffer(const Json& root, std::uint64_t bin_length) {
    const auto buffers = root.find("buffers");
    require(buffers != root.end() && buffers->is_array()
                    && buffers->size() == 1U && buffers->at(0U).is_object(),
            "Canonical GLB must contain exactly one embedded buffer");
    const auto& buffer = buffers->at(0U);
    require(!buffer.contains("uri"),
            "Canonical GLB embedded buffer must not contain a URI");
    const std::uint64_t declared = requiredUnsigned(
            buffer, "byteLength", 0U, true);
    require(declared > 0U && declared <= bin_length
                    && bin_length - declared <= 3U,
            "Canonical GLB buffer length differs from the BIN chunk");
    return declared;
}

std::vector<BufferViewInfo> validateBufferViews(
        const Json& root, std::uint64_t buffer_length) {
    const auto views = root.find("bufferViews");
    require(views != root.end() && views->is_array() && !views->empty(),
            "Canonical GLB has no buffer views");
    std::vector<BufferViewInfo> result;
    result.reserve(views->size());
    for (const auto& view : *views) {
        require(view.is_object()
                        && requiredUnsigned(view, "buffer", 0U, true) == 0U,
                "Canonical GLB buffer view references an invalid buffer");
        BufferViewInfo info;
        info.byte_offset = requiredUnsigned(view, "byteOffset", 0U, false);
        info.byte_length = requiredUnsigned(view, "byteLength", 0U, true);
        info.byte_stride = requiredUnsigned(view, "byteStride", 0U, false);
        require(info.byte_length > 0U
                        && rangeFits(info.byte_offset, info.byte_length,
                                     buffer_length),
                "Canonical GLB buffer view is out of range");
        if (info.byte_stride != 0U) {
            require(info.byte_stride >= 4U && info.byte_stride <= 252U
                            && info.byte_stride % 4U == 0U,
                    "Canonical GLB buffer view stride is invalid");
        }
        result.push_back(info);
    }
    return result;
}

void validateDeclaredBounds(const Json& accessor,
                            std::uint64_t component_count) {
    const bool has_minimum = accessor.contains("min");
    const bool has_maximum = accessor.contains("max");
    require(has_minimum == has_maximum,
            "Canonical GLB accessor bounds are incomplete");
    if (!has_minimum) return;
    const auto& minimum = accessor.at("min");
    const auto& maximum = accessor.at("max");
    require(minimum.is_array() && maximum.is_array()
                    && minimum.size() == component_count
                    && maximum.size() == component_count,
            "Canonical GLB accessor bounds have invalid dimensions");
    for (std::size_t index = 0U; index < minimum.size(); ++index) {
        require(minimum.at(index).is_number() && maximum.at(index).is_number(),
                "Canonical GLB accessor bounds are not numeric");
        const double lower = minimum.at(index).get<double>();
        const double upper = maximum.at(index).get<double>();
        require(std::isfinite(lower) && std::isfinite(upper) && lower <= upper,
                "Canonical GLB accessor bounds are invalid");
    }
}

std::vector<AccessorInfo> validateAccessors(
        const Json& root, const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes, std::size_t bin_offset) {
    const auto accessors = root.find("accessors");
    require(accessors != root.end() && accessors->is_array()
                    && !accessors->empty(),
            "Canonical GLB has no accessors");
    std::vector<AccessorInfo> result;
    result.reserve(accessors->size());
    for (const auto& accessor : *accessors) {
        require(accessor.is_object() && !accessor.contains("sparse")
                        && accessor.contains("bufferView")
                        && accessor.contains("componentType")
                        && accessor.contains("count")
                        && accessor.contains("type"),
                "Canonical GLB accessor is incomplete or sparse");
        AccessorInfo info;
        const std::uint64_t view_index = requiredUnsigned(
                accessor, "bufferView", 0U, true);
        require(view_index < views.size(),
                "Canonical GLB accessor buffer view is out of range");
        info.buffer_view = static_cast<std::size_t>(view_index);
        const std::uint64_t component_type = requiredUnsigned(
                accessor, "componentType", 0U, true);
        require(component_type <= std::numeric_limits<std::uint32_t>::max(),
                "Canonical GLB accessor component type is invalid");
        info.component_type = static_cast<std::uint32_t>(component_type);
        const auto normalized = accessor.find("normalized");
        require(normalized == accessor.end() || normalized->is_boolean(),
                "Canonical GLB accessor normalized flag is invalid");
        info.normalized = normalized != accessor.end()
                && normalized->get<bool>();
        info.component_size = componentSize(info.component_type);
        require(accessor.at("type").is_string(),
                "Canonical GLB accessor type is invalid");
        info.type = accessor.at("type").get<std::string>();
        info.component_count = componentCount(info.type);
        info.element_size = elementSize(info.type, info.component_size);
        info.count = requiredUnsigned(accessor, "count", 0U, true);
        info.byte_offset = requiredUnsigned(accessor, "byteOffset", 0U, false);
        require(info.count > 0U,
                "Canonical GLB accessor count must be positive");
        const auto& view = views.at(info.buffer_view);
        info.stride = view.byte_stride == 0U
                ? info.element_size : view.byte_stride;
        require(info.stride >= info.element_size
                        && (view.byte_offset + info.byte_offset)
                                % info.component_size == 0U,
                "Canonical GLB accessor stride or alignment is invalid");
        require(info.count - 1U
                        <= (std::numeric_limits<std::uint64_t>::max()
                                    - info.element_size) / info.stride,
                "Canonical GLB accessor byte range overflows");
        const std::uint64_t span = (info.count - 1U) * info.stride
                + info.element_size;
        require(rangeFits(info.byte_offset, span, view.byte_length),
                "Canonical GLB accessor exceeds its buffer view");
        validateDeclaredBounds(accessor, info.component_count);

        if (info.component_type == kFloatComponentType) {
            for (std::uint64_t element = 0U; element < info.count; ++element) {
                const std::uint64_t element_offset = view.byte_offset
                        + info.byte_offset + element * info.stride;
                for (std::uint64_t component = 0U;
                     component < info.component_count; ++component) {
                    const std::uint64_t relative = element_offset
                            + component * info.component_size;
                    require(relative <= std::numeric_limits<std::size_t>::max()
                                    - bin_offset,
                            "Canonical GLB float accessor offset overflows");
                    const std::uint32_t bits = readU32(
                            bytes, bin_offset + static_cast<std::size_t>(relative));
                    float value = 0.0F;
                    std::memcpy(&value, &bits, sizeof(value));
                    require(std::isfinite(value),
                            "Canonical GLB binary accessor contains a non-finite value");
                }
            }
        }
        result.push_back(info);
    }
    return result;
}

std::size_t componentOffset(
        const AccessorInfo& accessor,
        const std::vector<BufferViewInfo>& views,
        std::size_t bin_offset,
        std::uint64_t element,
        std::uint64_t component) {
    require(element < accessor.count
                    && component < accessor.component_count,
            "Canonical GLB accessor element is out of range");
    const auto& view = views.at(accessor.buffer_view);
    const std::uint64_t relative = view.byte_offset + accessor.byte_offset
            + element * accessor.stride
            + component * accessor.component_size;
    require(relative <= std::numeric_limits<std::size_t>::max() - bin_offset,
            "Canonical GLB accessor offset overflows");
    return bin_offset + static_cast<std::size_t>(relative);
}

std::uint32_t unsignedValue(
        const AccessorInfo& accessor,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset,
        std::uint64_t element) {
    const std::size_t offset = componentOffset(
            accessor, views, bin_offset, element, 0U);
    switch (accessor.component_type) {
        case kUnsignedByteComponentType: return bytes.at(offset);
        case kUnsignedShortComponentType: return readU16(bytes, offset);
        case kUnsignedIntComponentType: return readU32(bytes, offset);
        case kFloatComponentType: {
            const std::uint32_t bits = readU32(bytes, offset);
            float value = 0.0F;
            std::memcpy(&value, &bits, sizeof(value));
            require(value >= 0.0F && value <= 16777216.0F
                            && std::isfinite(value)
                            && std::floor(value) == value,
                    "Canonical GLB float feature ID is not an exact integer");
            return static_cast<std::uint32_t>(value);
        }
        default:
            throw std::invalid_argument(
                    "Canonical GLB unsigned accessor type is invalid");
    }
}

std::optional<std::uint32_t> featureSemanticIndex(
        const std::string& semantic) {
    constexpr const char* kPrefix = "_FEATURE_ID_";
    constexpr std::size_t kPrefixLength = 12U;
    if (semantic.size() <= kPrefixLength
            || semantic.compare(0U, kPrefixLength, kPrefix) != 0) {
        return std::nullopt;
    }
    std::uint64_t value = 0U;
    for (std::size_t index = kPrefixLength; index < semantic.size(); ++index) {
        const char character = semantic[index];
        if (character < '0' || character > '9') return std::nullopt;
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

float floatValue(
        const AccessorInfo& accessor,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset,
        std::uint64_t element,
        std::uint64_t component) {
    require(accessor.component_type == kFloatComponentType,
            "Canonical GLB accessor is not float");
    const std::uint32_t bits = readU32(
            bytes, componentOffset(accessor, views, bin_offset,
                                   element, component));
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    require(std::isfinite(value),
            "Canonical GLB float accessor contains a non-finite value");
    return value;
}

void mergeBounds(Bounds3D& destination, const Bounds3D& source) {
    if (!source.valid) return;
    if (!destination.valid) {
        destination = source;
        return;
    }
    for (std::size_t component = 0U; component < 3U; ++component) {
        destination.minimum[component] = std::min(
                destination.minimum[component], source.minimum[component]);
        destination.maximum[component] = std::max(
                destination.maximum[component], source.maximum[component]);
    }
}

Bounds3D validatePositionBounds(
        const Json& accessor_json,
        const AccessorInfo& accessor,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset) {
    require(accessor_json.contains("min")
                    && accessor_json.contains("max"),
            "Canonical GLB POSITION bounds are missing");
    Bounds3D actual;
    for (std::uint64_t element = 0U; element < accessor.count; ++element) {
        std::array<double, 3> point{};
        for (std::size_t component = 0U; component < 3U; ++component) {
            point[component] = floatValue(
                    accessor, views, bytes, bin_offset, element, component);
        }
        if (!actual.valid) {
            actual.minimum = point;
            actual.maximum = point;
            actual.valid = true;
        } else {
            for (std::size_t component = 0U; component < 3U; ++component) {
                actual.minimum[component] = std::min(
                        actual.minimum[component], point[component]);
                actual.maximum[component] = std::max(
                        actual.maximum[component], point[component]);
            }
        }
    }
    constexpr double kBoundsTolerance = 1.0e-6;
    for (std::size_t component = 0U; component < 3U; ++component) {
        const double declared_minimum = accessor_json.at("min")
                .at(component).get<double>();
        const double declared_maximum = accessor_json.at("max")
                .at(component).get<double>();
        const double scale = std::max(
                {1.0, std::abs(actual.minimum[component]),
                 std::abs(actual.maximum[component])});
        require(std::abs(actual.minimum[component] - declared_minimum)
                                <= kBoundsTolerance * scale
                        && std::abs(actual.maximum[component]
                                    - declared_maximum)
                                <= kBoundsTolerance * scale,
                "Canonical GLB POSITION bounds differ from binary values");
    }
    return actual;
}

void validateUnitDirections(
        const AccessorInfo& accessor,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset,
        bool tangent) {
    require(accessor.component_type == kFloatComponentType
                    && accessor.type == (tangent ? "VEC4" : "VEC3"),
            tangent ? "Canonical GLB TANGENT accessor is invalid"
                    : "Canonical GLB NORMAL accessor is invalid");
    constexpr double kUnitTolerance = 1.0e-4;
    for (std::uint64_t element = 0U; element < accessor.count; ++element) {
        double squared_length = 0.0;
        for (std::uint64_t component = 0U; component < 3U; ++component) {
            const double value = floatValue(
                    accessor, views, bytes, bin_offset, element, component);
            squared_length += value * value;
        }
        require(std::abs(std::sqrt(squared_length) - 1.0)
                        <= kUnitTolerance,
                tangent ? "Canonical GLB TANGENT is not normalized"
                        : "Canonical GLB NORMAL is not normalized");
        if (tangent) {
            const float handedness = floatValue(
                    accessor, views, bytes, bin_offset, element, 3U);
            require(handedness == -1.0F || handedness == 1.0F,
                    "Canonical GLB tangent handedness is invalid");
        }
    }
}

std::size_t accessorIndex(const Json& value,
                          const std::vector<AccessorInfo>& accessors) {
    require(isNonNegativeInteger(value),
            "Canonical GLB accessor reference is invalid");
    const std::uint64_t index = value.get<std::uint64_t>();
    require(index < accessors.size(),
            "Canonical GLB accessor reference is out of range");
    return static_cast<std::size_t>(index);
}

std::uint64_t countPrimitives(
        const Json& root, CanonicalFamily family,
        const std::vector<AccessorInfo>& accessors,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset,
        std::vector<Bounds3D>& mesh_bounds) {
    const auto meshes = root.find("meshes");
    require(meshes != root.end() && meshes->is_array() && !meshes->empty(),
            "Canonical GLB has no mesh payload");
    mesh_bounds.assign(meshes->size(), {});
    std::uint64_t count = 0U;
    for (std::size_t mesh_index = 0U; mesh_index < meshes->size();
         ++mesh_index) {
        const auto& mesh = meshes->at(mesh_index);
        require(mesh.is_object() && mesh.contains("primitives")
                        && mesh.at("primitives").is_array()
                        && !mesh.at("primitives").empty(),
                "Canonical GLB mesh has no primitives");
        for (const auto& primitive : mesh.at("primitives")) {
            require(primitive.is_object(), "Canonical GLB primitive is invalid");
            const int expected_mode = family == CanonicalFamily::point_gltf2
                    ? kPointPrimitiveMode : kTrianglePrimitiveMode;
            const int mode = primitive.value("mode", kTrianglePrimitiveMode);
            require(mode == expected_mode,
                    "Canonical GLB primitive mode does not match its family");
            require(primitive.contains("attributes")
                            && primitive.at("attributes").is_object()
                            && primitive.at("attributes").contains("POSITION"),
                    "Canonical GLB primitive has no POSITION accessor");
            const auto& attributes = primitive.at("attributes");
            const std::size_t position_index = accessorIndex(
                    attributes.at("POSITION"), accessors);
            const auto& position = accessors.at(position_index);
            require(position.component_type == kFloatComponentType
                            && position.type == "VEC3",
                    "Canonical GLB POSITION accessor is not float VEC3");
            const auto& position_json = root.at("accessors").at(position_index);
            mergeBounds(mesh_bounds[mesh_index], validatePositionBounds(
                    position_json, position, views, bytes, bin_offset));
            for (const auto& attribute : attributes.items()) {
                const auto& info = accessors.at(
                        accessorIndex(attribute.value(), accessors));
                require(info.count == position.count,
                        "Canonical GLB primitive attributes are not aligned");
                if (attribute.key() == "POSITION") {
                    continue;
                }
                if (attribute.key() == "NORMAL") {
                    validateUnitDirections(
                            info, views, bytes, bin_offset, false);
                } else if (attribute.key() == "TANGENT") {
                    validateUnitDirections(
                            info, views, bytes, bin_offset, true);
                } else if (attribute.key() == "TEXCOORD_0"
                           || attribute.key() == "TEXCOORD_1") {
                    require(info.component_type == kFloatComponentType
                                    && info.type == "VEC2",
                            "Canonical GLB texture coordinate accessor is invalid");
                } else if (attribute.key() == "COLOR_0") {
                    const bool normalized_integer = info.normalized
                            && (info.component_type == kUnsignedByteComponentType
                                || info.component_type
                                           == kUnsignedShortComponentType);
                    require((info.component_type == kFloatComponentType
                             || normalized_integer)
                                    && (info.type == "VEC3" || info.type == "VEC4"),
                            "Canonical GLB color accessor is invalid");
                } else if (featureSemanticIndex(attribute.key()).has_value()) {
                    require(!info.normalized && info.type == "SCALAR"
                                    && (info.component_type
                                                == kUnsignedByteComponentType
                                        || info.component_type
                                                == kUnsignedShortComponentType
                                        || info.component_type
                                                == kUnsignedIntComponentType
                                        || info.component_type
                                                == kFloatComponentType),
                            "Canonical GLB feature ID accessor is invalid");
                } else {
                    throw std::invalid_argument(
                            "Canonical GLB primitive semantic is unsupported");
                }
            }
            std::uint64_t element_count = position.count;
            if (primitive.contains("indices")) {
                const auto& indices = accessors.at(
                        accessorIndex(primitive.at("indices"), accessors));
                require(indices.type == "SCALAR"
                                && (indices.component_type
                                            == kUnsignedByteComponentType
                                    || indices.component_type
                                            == kUnsignedShortComponentType
                                    || indices.component_type
                                            == kUnsignedIntComponentType),
                        "Canonical GLB index accessor is invalid");
                element_count = indices.count;
                for (std::uint64_t element = 0U;
                     element < indices.count; ++element) {
                    require(unsignedValue(indices, views, bytes,
                                          bin_offset, element)
                                    < position.count,
                            "Canonical GLB index is out of range");
                }
            }
            if (expected_mode == kTrianglePrimitiveMode) {
                require(element_count % 3U == 0U,
                        "Canonical GLB triangle primitive count is invalid");
            }
            if (primitive.contains("material")) {
                const auto materials = root.find("materials");
                const auto& material = primitive.at("material");
                require(materials != root.end() && materials->is_array()
                                && isNonNegativeInteger(material)
                                && material.get<std::uint64_t>()
                                        < materials->size(),
                        "Canonical GLB material reference is invalid");
            }
            ++count;
        }
    }
    return count;
}

std::vector<std::uint8_t> bufferViewBytes(
        std::size_t view_index,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset) {
    require(view_index < views.size(),
            "Canonical GLB buffer view reference is out of range");
    const auto& view = views[view_index];
    require(view.byte_offset <= std::numeric_limits<std::size_t>::max()
                                - bin_offset
                    && view.byte_length
                            <= std::numeric_limits<std::size_t>::max()
                                    - bin_offset
                                    - static_cast<std::size_t>(view.byte_offset),
            "Canonical GLB buffer view slice overflows");
    const std::size_t first = bin_offset
            + static_cast<std::size_t>(view.byte_offset);
    const std::size_t length = static_cast<std::size_t>(view.byte_length);
    require(first <= bytes.size() && length <= bytes.size() - first,
            "Canonical GLB buffer view slice is truncated");
    return {bytes.begin() + static_cast<std::ptrdiff_t>(first),
            bytes.begin() + static_cast<std::ptrdiff_t>(first + length)};
}

void validateTextureBinding(
        const Json& binding, std::size_t texture_count,
        std::vector<bool>& used_textures) {
    require(binding.is_object() && binding.contains("index")
                    && isNonNegativeInteger(binding.at("index")),
            "Canonical GLB texture binding is invalid");
    const auto texture = binding.at("index").get<std::uint64_t>();
    require(texture < texture_count,
            "Canonical GLB texture binding is out of range");
    used_textures[static_cast<std::size_t>(texture)] = true;
    if (binding.contains("texCoord")) {
        require(isNonNegativeInteger(binding.at("texCoord"))
                        && binding.at("texCoord").get<std::uint64_t>() <= 1U,
                "Canonical GLB texture coordinate set is unsupported");
    }
}

void validateTexturesAndMaterials(
        const Json& root,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset) {
    const auto images = root.find("images");
    const auto textures = root.find("textures");
    const auto samplers = root.find("samplers");
    const auto materials = root.find("materials");
    const std::size_t image_count = images == root.end() ? 0U : images->size();
    const std::size_t texture_count = textures == root.end() ? 0U : textures->size();
    const std::size_t sampler_count = samplers == root.end() ? 0U : samplers->size();
    const std::size_t material_count = materials == root.end() ? 0U : materials->size();
    require((images == root.end() || images->is_array())
                    && (textures == root.end() || textures->is_array())
                    && (samplers == root.end() || samplers->is_array())
                    && (materials == root.end() || materials->is_array()),
            "Canonical GLB material arrays are invalid");
    require((image_count == 0U) == (texture_count == 0U),
            "Canonical GLB images and textures are inconsistent");

    std::vector<bool> used_images(image_count, false);
    std::vector<bool> used_samplers(sampler_count, false);
    if (images != root.end()) {
        for (const auto& image : *images) {
            require(image.is_object() && !image.contains("uri")
                            && image.value("mimeType", std::string{})
                                    == "image/png"
                            && image.contains("bufferView")
                            && isNonNegativeInteger(image.at("bufferView")),
                    "Canonical GLB image is not embedded PNG");
            const auto encoded = bufferViewBytes(
                    static_cast<std::size_t>(
                            image.at("bufferView").get<std::uint64_t>()),
                    views, bytes, bin_offset);
            const auto decoded = formats::TextureCodec::decode(
                    encoded, formats::TextureFormat::png);
            require(formats::TextureCodec::encodeDeterministicPng(decoded)
                            == encoded,
                    "Canonical GLB PNG bytes are not deterministic canonical output");
        }
    }
    if (samplers != root.end()) {
        const std::set<std::uint32_t> allowed_wrap{
                33071U, 33648U, 10497U};
        const std::set<std::uint32_t> allowed_mag{9728U, 9729U};
        const std::set<std::uint32_t> allowed_min{
                9728U, 9729U, 9984U, 9985U, 9986U, 9987U};
        for (const auto& sampler : *samplers) {
            require(sampler.is_object(),
                    "Canonical GLB sampler is invalid");
            for (const auto& item : sampler.items()) {
                require(item.key() == "wrapS" || item.key() == "wrapT"
                                || item.key() == "magFilter"
                                || item.key() == "minFilter",
                        "Canonical GLB sampler field is unsupported");
                require(isNonNegativeInteger(item.value()),
                        "Canonical GLB sampler value is invalid");
            }
            require(!sampler.contains("wrapS")
                            || allowed_wrap.count(
                                       sampler.at("wrapS").get<std::uint32_t>())
                                    != 0U,
                    "Canonical GLB sampler wrapS is invalid");
            require(!sampler.contains("wrapT")
                            || allowed_wrap.count(
                                       sampler.at("wrapT").get<std::uint32_t>())
                                    != 0U,
                    "Canonical GLB sampler wrapT is invalid");
            require(!sampler.contains("magFilter")
                            || allowed_mag.count(sampler.at("magFilter")
                                                         .get<std::uint32_t>())
                                    != 0U,
                    "Canonical GLB sampler magFilter is invalid");
            require(!sampler.contains("minFilter")
                            || allowed_min.count(sampler.at("minFilter")
                                                         .get<std::uint32_t>())
                                    != 0U,
                    "Canonical GLB sampler minFilter is invalid");
        }
    }
    if (textures != root.end()) {
        for (const auto& texture : *textures) {
            require(texture.is_object() && texture.contains("source")
                            && isNonNegativeInteger(texture.at("source")),
                    "Canonical GLB texture is invalid");
            const auto image = texture.at("source").get<std::uint64_t>();
            require(image < image_count,
                    "Canonical GLB texture image is out of range");
            used_images[static_cast<std::size_t>(image)] = true;
            if (texture.contains("sampler")) {
                require(isNonNegativeInteger(texture.at("sampler"))
                                && texture.at("sampler").get<std::uint64_t>()
                                        < sampler_count,
                        "Canonical GLB texture sampler is out of range");
                used_samplers[static_cast<std::size_t>(
                        texture.at("sampler").get<std::uint64_t>())] = true;
            }
        }
    }

    std::vector<bool> used_materials(material_count, false);
    for (const auto& mesh : root.at("meshes")) {
        for (const auto& primitive : mesh.at("primitives")) {
            if (!primitive.contains("material")) continue;
            require(isNonNegativeInteger(primitive.at("material"))
                            && primitive.at("material").get<std::uint64_t>()
                                    < material_count,
                    "Canonical GLB primitive material is out of range");
            used_materials[static_cast<std::size_t>(
                    primitive.at("material").get<std::uint64_t>())] = true;
        }
    }
    std::vector<bool> used_textures(texture_count, false);
    if (materials != root.end()) {
        for (const auto& material : *materials) {
            require(material.is_object(),
                    "Canonical GLB material is invalid");
            if (material.contains("pbrMetallicRoughness")) {
                const auto& pbr = material.at("pbrMetallicRoughness");
                require(pbr.is_object(),
                        "Canonical GLB PBR material is invalid");
                for (const char* field : {"baseColorTexture",
                                          "metallicRoughnessTexture"}) {
                    if (pbr.contains(field)) {
                        validateTextureBinding(
                                pbr.at(field), texture_count, used_textures);
                    }
                }
            }
            for (const char* field : {"normalTexture", "occlusionTexture",
                                      "emissiveTexture"}) {
                if (material.contains(field)) {
                    validateTextureBinding(
                            material.at(field), texture_count, used_textures);
                }
            }
        }
    }
    require(std::all_of(used_materials.begin(), used_materials.end(),
                        [](bool used) { return used; })
                    && std::all_of(used_textures.begin(), used_textures.end(),
                                   [](bool used) { return used; })
                    && std::all_of(used_images.begin(), used_images.end(),
                                   [](bool used) { return used; })
                    && std::all_of(used_samplers.begin(), used_samplers.end(),
                                   [](bool used) { return used; }),
            "Canonical GLB contains an unreachable material resource");
}

std::uint64_t metadataComponentCount(const std::string& type) {
    if (type == "SCALAR") return 1U;
    if (type == "VEC2") return 2U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4") return 4U;
    throw std::invalid_argument(
            "Canonical structural metadata type is unsupported");
}

struct StructuralMetadataValidation {
    std::vector<std::uint64_t> table_counts;
    std::optional<clip_worker::metadata::FeatureMetadata> typed_metadata;
};

StructuralMetadataValidation validateStructuralMetadata(
        const Json& root,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset) {
    const auto extensions = root.find("extensions");
    if (extensions == root.end()
        || !extensions->contains(kStructuralMetadataExtension)) {
        return {};
    }
    const auto& metadata = extensions->at(kStructuralMetadataExtension);
    require(metadata.is_object() && metadata.contains("schema")
                    && metadata.contains("propertyTables")
                    && metadata.at("propertyTables").is_array()
                    && !metadata.at("propertyTables").empty(),
            "Canonical structural metadata root is invalid");
    const auto& schema = metadata.at("schema");
    require(schema.is_object() && schema.contains("classes")
                    && schema.at("classes").is_object(),
            "Canonical structural metadata schema is invalid");
    if (schema.value("id", std::string{})
            == "justai-canonical-metadata-v1") {
        std::vector<std::vector<std::uint8_t>> storage;
        std::vector<clip_worker::metadata::MetadataBufferView> metadata_views;
        storage.reserve(views.size());
        metadata_views.reserve(views.size());
        for (std::size_t index = 0U; index < views.size(); ++index) {
            storage.push_back(bufferViewBytes(
                    index, views, bytes, bin_offset));
            metadata_views.push_back(
                    {&storage.back(),
                     static_cast<std::size_t>(views[index].byte_offset)});
        }
        clip_worker::metadata::MetadataResourceLimits limits;
        const std::uint64_t maximum =
                std::numeric_limits<std::uint64_t>::max();
        limits.maximum_schema_bytes = maximum;
        limits.maximum_classes = maximum;
        limits.maximum_enums = maximum;
        limits.maximum_property_tables = maximum;
        limits.maximum_feature_id_sets = maximum;
        limits.maximum_feature_rows = maximum;
        limits.maximum_properties = maximum;
        limits.maximum_values_bytes = maximum;
        limits.maximum_array_offset_bytes = maximum;
        limits.maximum_string_offset_bytes = maximum;
        limits.maximum_decoded_string_bytes = maximum;
        limits.maximum_array_elements = maximum;
        limits.maximum_hierarchy_instances = maximum;
        limits.maximum_hierarchy_edges = maximum;
        limits.maximum_hierarchy_depth = maximum;
        limits.maximum_ancestor_closure_entries = maximum;
        limits.maximum_feature_mapping_entries = maximum;
        limits.maximum_validation_operations = maximum;
        try {
            const auto parsed = clip_worker::metadata::readStructuralMetadata(
                    root.dump(), metadata_views, limits);
            require(parsed.property_tables.size()
                            == metadata.at("propertyTables").size(),
                    "Canonical typed metadata table count differs after reparse");
            StructuralMetadataValidation result;
            result.table_counts.reserve(parsed.property_tables.size());
            for (const auto& table : parsed.property_tables) {
                require(table.row_count > 0U,
                        "Canonical typed metadata table is empty");
                result.table_counts.push_back(table.row_count);
            }
            result.typed_metadata = parsed;
            return result;
        } catch (const formats::FormatError&) {
            throw std::invalid_argument(
                    "Canonical typed metadata failed strict reparse");
        }
    }
    StructuralMetadataValidation result;
    for (const auto& table : metadata.at("propertyTables")) {
        require(table.is_object() && table.contains("class")
                        && table.at("class").is_string()
                        && table.contains("count")
                        && isNonNegativeInteger(table.at("count"))
                        && table.at("count").get<std::uint64_t>() > 0U
                        && table.contains("properties")
                        && table.at("properties").is_object(),
                "Canonical structural metadata table is invalid");
        const std::uint64_t row_count = table.at("count").get<std::uint64_t>();
        result.table_counts.push_back(row_count);
        const std::string class_name = table.at("class").get<std::string>();
        require(schema.at("classes").contains(class_name),
                "Canonical property table class is unavailable");
        const auto& class_definition = schema.at("classes").at(class_name);
        require(class_definition.is_object()
                        && class_definition.contains("properties")
                        && class_definition.at("properties").is_object(),
                "Canonical metadata class properties are invalid");
        const auto& schema_properties = class_definition.at("properties");
        const auto& table_properties = table.at("properties");
        require(schema_properties.size() == table_properties.size(),
                "Canonical property table columns differ from the schema");
        for (const auto& property : table_properties.items()) {
            require(schema_properties.contains(property.key())
                            && property.value().is_object(),
                    "Canonical property table column is invalid");
            const auto& definition = schema_properties.at(property.key());
            require(definition.is_object() && definition.contains("type")
                            && definition.at("type").is_string(),
                    "Canonical metadata property definition is invalid");
            const std::string type = definition.at("type").get<std::string>();
            if (type == "BOOLEAN") {
                require(property.value().contains("values")
                                && isNonNegativeInteger(
                                        property.value().at("values")),
                        "Canonical BOOLEAN property storage is invalid");
                const auto values = bufferViewBytes(
                        static_cast<std::size_t>(property.value().at("values")
                                                         .get<std::uint64_t>()),
                        views, bytes, bin_offset);
                require(values.size() == (row_count + 7U) / 8U,
                        "Canonical BOOLEAN property row count is inconsistent");
            } else if (type == "STRING") {
                require(property.value().value("stringOffsetType", std::string{})
                                        == "UINT32"
                                && property.value().contains("stringOffsets")
                                && property.value().contains("values")
                                && isNonNegativeInteger(
                                        property.value().at("stringOffsets"))
                                && isNonNegativeInteger(
                                        property.value().at("values")),
                        "Canonical STRING property storage is invalid");
                const auto offsets = bufferViewBytes(
                        static_cast<std::size_t>(
                                property.value().at("stringOffsets")
                                        .get<std::uint64_t>()),
                        views, bytes, bin_offset);
                const auto values = bufferViewBytes(
                        static_cast<std::size_t>(property.value().at("values")
                                                         .get<std::uint64_t>()),
                        views, bytes, bin_offset);
                require(row_count <= (std::numeric_limits<std::size_t>::max()
                                                / sizeof(std::uint32_t))
                                        - 1U
                                && offsets.size()
                                        == static_cast<std::size_t>(row_count + 1U)
                                                * sizeof(std::uint32_t),
                        "Canonical STRING offsets do not match the row count");
                std::uint32_t previous = 0U;
                for (std::uint64_t row = 0U; row <= row_count; ++row) {
                    const std::uint32_t current = readU32(
                            offsets, static_cast<std::size_t>(row)
                                             * sizeof(std::uint32_t));
                    require(current >= previous && current <= values.size(),
                            "Canonical STRING offsets are invalid");
                    previous = current;
                }
            } else {
                require(definition.value("componentType", std::string{})
                                        == "FLOAT64"
                                && property.value().contains("values")
                                && isNonNegativeInteger(
                                        property.value().at("values")),
                        "Canonical numeric property storage is invalid");
                const std::uint64_t components = metadataComponentCount(type);
                require(row_count
                                        <= std::numeric_limits<std::uint64_t>::max()
                                                / components
                                && row_count * components
                                        <= std::numeric_limits<std::uint64_t>::max()
                                                / sizeof(double),
                        "Canonical numeric property byte count overflows");
                const auto values = bufferViewBytes(
                        static_cast<std::size_t>(property.value().at("values")
                                                         .get<std::uint64_t>()),
                        views, bytes, bin_offset);
                require(values.size() == row_count * components * sizeof(double),
                        "Canonical numeric property row count is inconsistent");
            }
        }
    }
    return result;
}

void validateCanonicalHierarchy(
        const Json& root,
        const std::vector<std::uint64_t>& table_counts) {
    const auto extensions = root.find("extensions");
    const bool present = extensions != root.end()
            && extensions->is_object()
            && extensions->contains(kCanonicalLegacyHierarchyExtension);
    const auto used_extensions = sortedStrings(root, "extensionsUsed");
    const auto required_extensions = sortedStrings(root, "extensionsRequired");
    require(contains(used_extensions, kCanonicalLegacyHierarchyExtension)
                    == present
                    && contains(required_extensions,
                                kCanonicalLegacyHierarchyExtension)
                            == present,
            "Canonical hierarchy extension declaration is inconsistent");
    if (!present) return;
    require(!table_counts.empty(),
            "Canonical hierarchy has no structural metadata tables");
    const auto& hierarchy = extensions->at(
            kCanonicalLegacyHierarchyExtension);
    require(hierarchy.is_object()
                    && hierarchy.value("version", 0U) == 1U
                    && hierarchy.contains("nodes")
                    && hierarchy.at("nodes").is_array()
                    && !hierarchy.at("nodes").empty()
                    && hierarchy.contains("featureNodes")
                    && hierarchy.at("featureNodes").is_array(),
            "Canonical hierarchy root is invalid");
    const auto& nodes = hierarchy.at("nodes");
    std::vector<std::vector<std::uint32_t>> parents(nodes.size());
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        const auto& node = nodes.at(index);
        require(node.is_object()
                        && node.contains("propertyTable")
                        && isNonNegativeInteger(node.at("propertyTable"))
                        && node.contains("propertyRow")
                        && isNonNegativeInteger(node.at("propertyRow"))
                        && node.contains("parents")
                        && node.at("parents").is_array(),
                "Canonical hierarchy node is invalid");
        const std::uint64_t table = node.at("propertyTable")
                .get<std::uint64_t>();
        const std::uint64_t row = node.at("propertyRow")
                .get<std::uint64_t>();
        require(table < table_counts.size()
                        && row < table_counts.at(
                                static_cast<std::size_t>(table)),
                "Canonical hierarchy property row is invalid");
        std::set<std::uint32_t> unique_parents;
        for (const auto& parent : node.at("parents")) {
            require(isNonNegativeInteger(parent)
                            && parent.get<std::uint64_t>() < nodes.size(),
                    "Canonical hierarchy parent is invalid");
            const auto parent_index = parent.get<std::uint32_t>();
            require(parent_index != index
                            && unique_parents.insert(parent_index).second,
                    "Canonical hierarchy parent is duplicated or self-referential");
            parents[index].push_back(parent_index);
        }
        require(std::is_sorted(parents[index].begin(), parents[index].end()),
                "Canonical hierarchy parents are not deterministic");
    }
    std::set<std::uint32_t> feature_nodes;
    std::size_t feature_ordinal = 0U;
    for (const auto& node : hierarchy.at("featureNodes")) {
        require(isNonNegativeInteger(node)
                        && node.get<std::uint64_t>() < nodes.size(),
                "Canonical hierarchy feature node is invalid");
        const std::uint32_t node_index = node.get<std::uint32_t>();
        require(feature_nodes.insert(node_index).second
                        && node_index == feature_ordinal,
                "Canonical hierarchy physical feature order is invalid");
        ++feature_ordinal;
    }
    std::vector<std::uint8_t> state(nodes.size(), 0U);
    std::function<void(std::uint32_t)> visit = [&](std::uint32_t node) {
        require(state.at(node) != 1U,
                "Canonical hierarchy contains a cycle");
        if (state.at(node) == 2U) return;
        state.at(node) = 1U;
        for (const std::uint32_t parent : parents.at(node)) visit(parent);
        state.at(node) = 2U;
    };
    for (std::uint32_t node = 0U; node < nodes.size(); ++node) visit(node);
}

bool isTypedCanonicalMetadata(const Json& root) {
    return root.contains("extensions")
            && root.at("extensions").is_object()
            && root.at("extensions").contains(kStructuralMetadataExtension)
            && root.at("extensions").at(kStructuralMetadataExtension)
                       .is_object()
            && root.at("extensions").at(kStructuralMetadataExtension)
                       .contains("schema")
            && root.at("extensions").at(kStructuralMetadataExtension)
                       .at("schema").is_object()
            && root.at("extensions").at(kStructuralMetadataExtension)
                       .at("schema").value("id", std::string{})
                    == "justai-canonical-metadata-v1";
}

void validateFeatureMetadata(
        const Json& root,
        const std::vector<AccessorInfo>& accessors,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes,
        std::size_t bin_offset) {
    const auto metadata_validation = validateStructuralMetadata(
            root, views, bytes, bin_offset);
    const auto& table_counts = metadata_validation.table_counts;
    validateCanonicalHierarchy(root, table_counts);
    const bool typed_metadata = isTypedCanonicalMetadata(root);
    bool found_features = false;
    std::set<std::size_t> referenced_tables;
    for (const auto& mesh : root.at("meshes")) {
        for (const auto& primitive : mesh.at("primitives")) {
            const auto& attributes = primitive.at("attributes");
            std::map<std::uint32_t, std::size_t> feature_accessors;
            for (const auto& attribute : attributes.items()) {
                const auto feature_index = featureSemanticIndex(attribute.key());
                if (!feature_index.has_value()) continue;
                require(feature_accessors.emplace(
                                *feature_index,
                                accessorIndex(attribute.value(), accessors))
                                .second,
                        "Canonical mesh feature semantic is duplicated");
            }
            std::uint32_t expected_feature_index = 0U;
            for (const auto& feature : feature_accessors) {
                require(feature.first == expected_feature_index++,
                        "Canonical mesh feature semantics are not contiguous");
            }
            const bool has_attribute = !feature_accessors.empty();
            const bool has_extension = primitive.contains("extensions")
                    && primitive.at("extensions").is_object()
                    && primitive.at("extensions").contains(
                            kMeshFeaturesExtension);
            require(has_attribute == has_extension,
                    "Canonical mesh feature attribute and extension disagree");
            if (!has_attribute) continue;
            found_features = true;
            const auto& extension = primitive.at("extensions").at(
                    kMeshFeaturesExtension);
            require(extension.is_object() && extension.contains("featureIds")
                            && extension.at("featureIds").is_array()
                            && extension.at("featureIds").size()
                                    == feature_accessors.size(),
                    "Canonical mesh feature declaration is invalid");
            std::set<std::string> labels;
            for (std::size_t set_index = 0U;
                 set_index < feature_accessors.size(); ++set_index) {
                const auto& declaration =
                        extension.at("featureIds").at(set_index);
                require(declaration.is_object()
                                && declaration.value(
                                           "attribute",
                                           std::numeric_limits<std::uint64_t>::max())
                                        == set_index
                                && declaration.contains("featureCount")
                                && isNonNegativeInteger(
                                        declaration.at("featureCount"))
                                && declaration.at("featureCount")
                                               .get<std::uint64_t>() > 0U,
                        "Canonical mesh feature identity is invalid");
                if (declaration.contains("label")) {
                    require(declaration.at("label").is_string()
                                    && !declaration.at("label")
                                                .get_ref<const std::string&>()
                                                .empty()
                                    && labels.insert(declaration.at("label")
                                                             .get<std::string>())
                                               .second,
                            "Canonical mesh feature label is invalid");
                }
                const std::uint64_t feature_count =
                        declaration.at("featureCount").get<std::uint64_t>();
                std::optional<std::uint32_t> null_feature_id;
                if (declaration.contains("nullFeatureId")) {
                    require(isNonNegativeInteger(
                                    declaration.at("nullFeatureId"))
                                    && declaration.at("nullFeatureId")
                                               .get<std::uint64_t>()
                                            <= std::numeric_limits<std::uint32_t>::max()
                                    && declaration.at("nullFeatureId")
                                               .get<std::uint64_t>()
                                            >= feature_count,
                            "Canonical mesh null feature ID is invalid");
                    null_feature_id = declaration.at("nullFeatureId")
                            .get<std::uint32_t>();
                }
                const auto& accessor = accessors.at(
                        feature_accessors.at(static_cast<std::uint32_t>(set_index)));
                const bool legal_component = accessor.component_type
                                == kUnsignedByteComponentType
                        || accessor.component_type
                                == kUnsignedShortComponentType
                        || accessor.component_type == kFloatComponentType
                        || (!typed_metadata
                            && accessor.component_type
                                    == kUnsignedIntComponentType);
                require(!accessor.normalized && accessor.type == "SCALAR"
                                && legal_component,
                        "Canonical mesh feature accessor is invalid");
                for (std::uint64_t element = 0U;
                     element < accessor.count; ++element) {
                    const std::uint32_t value = unsignedValue(
                            accessor, views, bytes, bin_offset, element);
                    require(value < feature_count
                                    || (null_feature_id.has_value()
                                        && value == *null_feature_id),
                            "Canonical mesh feature ID is out of range");
                }
                if (declaration.contains("propertyTable")) {
                    require(isNonNegativeInteger(
                                    declaration.at("propertyTable"))
                                    && declaration.at("propertyTable")
                                               .get<std::uint64_t>()
                                            < table_counts.size(),
                            "Canonical mesh property table reference is invalid");
                    const std::size_t table = declaration.at("propertyTable")
                            .get<std::size_t>();
                    require(table_counts.at(table) == feature_count,
                            "Canonical feature and property row counts differ");
                    if (metadata_validation.typed_metadata.has_value()) {
                        for (std::uint64_t element = 0U;
                             element < accessor.count; ++element) {
                            const std::uint32_t value = unsignedValue(
                                    accessor, views, bytes, bin_offset, element);
                            if (null_feature_id.has_value()
                                    && value == *null_feature_id) {
                                require(!clip_worker::metadata::lookupPropertyMaterial(
                                                *metadata_validation.typed_metadata,
                                                static_cast<std::uint32_t>(table),
                                                value, null_feature_id)
                                                .has_value(),
                                        "Canonical null feature ID resolved properties");
                            } else {
                                require(clip_worker::metadata::lookupPropertyMaterial(
                                                *metadata_validation.typed_metadata,
                                                static_cast<std::uint32_t>(table),
                                                value, null_feature_id)
                                                .has_value(),
                                        "Canonical feature property lookup failed");
                            }
                        }
                    }
                    referenced_tables.insert(table);
                }
            }
        }
    }
    const auto used_extensions = sortedStrings(root, "extensionsUsed");
    require(contains(used_extensions, kMeshFeaturesExtension)
                    == found_features,
            "Canonical mesh feature extension usage is inconsistent");
    require(table_counts.empty()
                    || (found_features
                        && contains(used_extensions,
                                    kStructuralMetadataExtension)),
            "Canonical structural metadata extension usage is inconsistent");
    if (!root.contains("extensions")
            || !root.at("extensions").contains(
                    kCanonicalLegacyHierarchyExtension)) {
        require(referenced_tables.size() == table_counts.size(),
                "Canonical structural metadata table is unreferenced");
    }
}

void validateInstanceFeatureMetadata(
        const Json& root, const std::vector<AccessorInfo>& accessors,
        const std::vector<BufferViewInfo>& views,
        const std::vector<std::uint8_t>& bytes, std::size_t bin_offset) {
    const auto metadata_validation = validateStructuralMetadata(
            root, views, bytes, bin_offset);
    const auto& table_counts = metadata_validation.table_counts;
    validateCanonicalHierarchy(root, table_counts);
    const bool typed_metadata = isTypedCanonicalMetadata(root);
    bool found_features = false;
    for (const auto& node : root.at("nodes")) {
        if (!node.contains("extensions") || !node.at("extensions").is_object()
            || !node.at("extensions").contains(kGpuInstancingExtension)) {
            continue;
        }
        const auto& attributes = node.at("extensions").at(
                kGpuInstancingExtension).at("attributes");
        const bool has_attribute = attributes.contains("_FEATURE_ID_0");
        const bool has_extension = node.at("extensions").contains(
                kInstanceFeaturesExtension);
        require(has_attribute == has_extension,
                "Canonical instance feature attribute and extension disagree");
        if (!has_attribute) continue;
        found_features = true;
        const auto& extension = node.at("extensions").at(
                kInstanceFeaturesExtension);
        require(extension.is_object() && extension.contains("featureIds")
                        && extension.at("featureIds").is_array()
                        && extension.at("featureIds").size() == 1U,
                "Canonical instance feature declaration is invalid");
        const auto& declaration = extension.at("featureIds").at(0U);
        require(declaration.is_object()
                        && declaration.value("attribute",
                                std::numeric_limits<std::uint64_t>::max()) == 0U
                        && declaration.contains("featureCount")
                        && isNonNegativeInteger(declaration.at("featureCount"))
                        && declaration.at("featureCount").get<std::uint64_t>() > 0U,
                "Canonical instance feature identity is invalid");
        const std::uint64_t feature_count = declaration.at(
                "featureCount").get<std::uint64_t>();
        std::optional<std::uint32_t> null_feature_id;
        if (declaration.contains("nullFeatureId")) {
            require(isNonNegativeInteger(declaration.at("nullFeatureId"))
                            && declaration.at("nullFeatureId")
                                       .get<std::uint64_t>()
                                    <= std::numeric_limits<std::uint32_t>::max()
                            && declaration.at("nullFeatureId")
                                       .get<std::uint64_t>() >= feature_count,
                    "Canonical instance null feature ID is invalid");
            null_feature_id = declaration.at("nullFeatureId")
                    .get<std::uint32_t>();
        }
        const auto& accessor = accessors.at(accessorIndex(
                attributes.at("_FEATURE_ID_0"), accessors));
        require(!accessor.normalized && accessor.type == "SCALAR"
                        && (accessor.component_type == kUnsignedByteComponentType
                            || accessor.component_type
                                   == kUnsignedShortComponentType
                            || accessor.component_type == kFloatComponentType
                            || (!typed_metadata
                                && accessor.component_type
                                        == kUnsignedIntComponentType)),
                "Canonical instance feature accessor is invalid");
        for (std::uint64_t element = 0U; element < accessor.count; ++element) {
            const std::uint32_t value = unsignedValue(
                    accessor, views, bytes, bin_offset, element);
            require(value < feature_count
                            || (null_feature_id.has_value()
                                && value == *null_feature_id),
                    "Canonical instance feature ID is out of range");
        }
        if (declaration.contains("propertyTable")) {
            require(isNonNegativeInteger(declaration.at("propertyTable"))
                            && declaration.at("propertyTable").get<std::uint64_t>()
                                    < table_counts.size(),
                    "Canonical instance property table reference is invalid");
            const std::size_t table = static_cast<std::size_t>(
                    declaration.at("propertyTable").get<std::uint64_t>());
            require(table_counts.at(table)
                            == feature_count,
                    "Canonical instance feature and property row counts differ");
            if (metadata_validation.typed_metadata.has_value()) {
                for (std::uint64_t element = 0U;
                     element < accessor.count; ++element) {
                    const std::uint32_t value = unsignedValue(
                            accessor, views, bytes, bin_offset, element);
                    if (null_feature_id.has_value()
                            && value == *null_feature_id) {
                        require(!clip_worker::metadata::lookupPropertyMaterial(
                                        *metadata_validation.typed_metadata,
                                        static_cast<std::uint32_t>(table), value,
                                        null_feature_id)
                                        .has_value(),
                                "Canonical null instance ID resolved properties");
                    } else {
                        require(clip_worker::metadata::lookupPropertyMaterial(
                                        *metadata_validation.typed_metadata,
                                        static_cast<std::uint32_t>(table), value,
                                        null_feature_id)
                                        .has_value(),
                                "Canonical instance property lookup failed");
                    }
                }
            }
        } else {
            require(table_counts.empty(),
                    "Canonical instance property table is unreferenced");
        }
    }
    const auto used_extensions = sortedStrings(root, "extensionsUsed");
    require(contains(used_extensions, kInstanceFeaturesExtension)
                    == found_features,
            "Canonical instance feature extension usage is inconsistent");
    require(table_counts.empty()
                    || (found_features
                        && contains(used_extensions,
                                    kStructuralMetadataExtension)),
            "Canonical instance structural metadata usage is inconsistent");
}

geometry::Matrix4 nodeTransform(const Json& node) {
    if (node.contains("matrix")) {
        std::array<double, 16> values{};
        for (std::size_t index = 0U; index < values.size(); ++index) {
            values[index] = node.at("matrix").at(index).get<double>();
        }
        return geometry::Matrix4::fromColumnMajor(values);
    }
    std::array<double, 3> translation{0.0, 0.0, 0.0};
    std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> scale{1.0, 1.0, 1.0};
    if (node.contains("translation")) {
        for (std::size_t index = 0U; index < translation.size(); ++index) {
            translation[index] = node.at("translation").at(index).get<double>();
        }
    }
    if (node.contains("rotation")) {
        for (std::size_t index = 0U; index < rotation.size(); ++index) {
            rotation[index] = node.at("rotation").at(index).get<double>();
        }
    }
    if (node.contains("scale")) {
        for (std::size_t index = 0U; index < scale.size(); ++index) {
            scale[index] = node.at("scale").at(index).get<double>();
        }
    }
    return geometry::Matrix4::translation(translation)
            * geometry::Matrix4::quaternion(rotation)
            * geometry::Matrix4::scale(scale);
}

void validateTransformedBounds(
        const Json& root,
        const std::vector<Bounds3D>& mesh_bounds) {
    const auto& nodes = root.at("nodes");
    const auto& scene = root.at("scenes").at(
            static_cast<std::size_t>(root.at("scene").get<std::uint64_t>()));
    std::function<void(std::size_t, const geometry::Matrix4&)> visit =
            [&](std::size_t node_index, const geometry::Matrix4& parent) {
        const auto& node = nodes.at(node_index);
        const auto world = parent * nodeTransform(node);
        if (node.contains("mesh")) {
            const auto& bounds = mesh_bounds.at(static_cast<std::size_t>(
                    node.at("mesh").get<std::uint64_t>()));
            require(bounds.valid,
                    "Canonical mesh has no derived local bounds");
            for (std::uint32_t corner = 0U; corner < 8U; ++corner) {
                const std::array<double, 3> point{
                        (corner & 1U) == 0U ? bounds.minimum[0U]
                                            : bounds.maximum[0U],
                        (corner & 2U) == 0U ? bounds.minimum[1U]
                                            : bounds.maximum[1U],
                        (corner & 4U) == 0U ? bounds.minimum[2U]
                                            : bounds.maximum[2U]};
                const auto transformed = world.transformPoint(point);
                require(std::all_of(transformed.begin(), transformed.end(),
                                    [](double value) {
                                        return std::isfinite(value);
                                    }),
                        "Canonical transformed mesh bounds are non-finite");
            }
        }
        if (node.contains("children")) {
            for (const auto& child : node.at("children")) {
                visit(static_cast<std::size_t>(child.get<std::uint64_t>()),
                      world);
            }
        }
    };
    for (const auto& root_node : scene.at("nodes")) {
        visit(static_cast<std::size_t>(root_node.get<std::uint64_t>()),
              geometry::Matrix4::identity());
    }
}

void validateNodeGraph(const Json& root) {
    const auto nodes = root.find("nodes");
    require(nodes != root.end() && nodes->is_array() && !nodes->empty(),
            "Canonical GLB nodes are invalid");
    std::vector<int> colors(nodes->size(), 0);
    std::vector<std::uint32_t> parent_counts(nodes->size(), 0U);
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        require(index < nodes->size(), "Canonical GLB node index is out of range");
        if (colors[index] == 2) return;
        require(colors[index] != 1, "Canonical GLB node graph contains a cycle");
        colors[index] = 1;
        const auto& node = nodes->at(index);
        require(node.is_object(), "Canonical GLB node is invalid");
        validateNumericArray(node, "matrix", 16U);
        validateNumericArray(node, "translation", 3U);
        validateNumericArray(node, "rotation", 4U);
        validateNumericArray(node, "scale", 3U);
        require(!node.contains("matrix")
                        || (!node.contains("translation")
                            && !node.contains("rotation")
                            && !node.contains("scale")),
                "Canonical GLB node mixes matrix and TRS transforms");
        if (node.contains("children")) {
            require(node.at("children").is_array(),
                    "Canonical GLB node children are invalid");
            for (const auto& child : node.at("children")) {
                require(child.is_number_unsigned() || child.is_number_integer(),
                        "Canonical GLB child index is invalid");
                const auto signed_child = child.get<std::int64_t>();
                require(signed_child >= 0,
                        "Canonical GLB child index is negative");
                const auto child_index = static_cast<std::size_t>(signed_child);
                require(child_index < nodes->size(),
                        "Canonical GLB child index is out of range");
                require(++parent_counts[child_index] == 1U,
                        "Canonical GLB node has multiple parents");
                visit(child_index);
            }
        }
        colors[index] = 2;
    };
    for (std::size_t index = 0U; index < nodes->size(); ++index) visit(index);
}

void validateScenesAndReachability(const Json& root) {
    require(root.contains("scenes") && root.at("scenes").is_array()
                    && !root.at("scenes").empty(),
            "Canonical GLB has no scenes");
    require(root.contains("scene") && root.at("scene").is_number_integer(),
            "Canonical GLB has no default scene");
    const auto scene = root.at("scene").get<std::int64_t>();
    require(scene >= 0 && static_cast<std::size_t>(scene) < root.at("scenes").size(),
            "Canonical GLB default scene is invalid");
    const auto& default_scene = root.at("scenes").at(
            static_cast<std::size_t>(scene));
    require(default_scene.is_object() && default_scene.contains("nodes")
                    && default_scene.at("nodes").is_array()
                    && !default_scene.at("nodes").empty(),
            "Canonical GLB default scene has no root nodes");
    const auto& nodes = root.at("nodes");
    const auto& meshes = root.at("meshes");
    std::vector<bool> reachable_nodes(nodes.size(), false);
    std::vector<bool> reachable_meshes(meshes.size(), false);
    std::function<void(std::size_t)> visit = [&](std::size_t node_index) {
        require(node_index < nodes.size(),
                "Canonical GLB scene node is out of range");
        if (reachable_nodes[node_index]) return;
        reachable_nodes[node_index] = true;
        const auto& node = nodes.at(node_index);
        if (node.contains("mesh")) {
            require(isNonNegativeInteger(node.at("mesh"))
                            && node.at("mesh").get<std::uint64_t>()
                                    < meshes.size(),
                    "Canonical GLB node mesh is out of range");
            reachable_meshes.at(static_cast<std::size_t>(
                    node.at("mesh").get<std::uint64_t>())) = true;
        }
        if (node.contains("children")) {
            for (const auto& child : node.at("children")) {
                require(isNonNegativeInteger(child),
                        "Canonical GLB child index is invalid");
                visit(static_cast<std::size_t>(child.get<std::uint64_t>()));
            }
        }
    };
    std::set<std::size_t> root_nodes;
    for (const auto& node : default_scene.at("nodes")) {
        require(isNonNegativeInteger(node),
                "Canonical GLB scene root index is invalid");
        const auto node_index = static_cast<std::size_t>(
                node.get<std::uint64_t>());
        require(root_nodes.insert(node_index).second,
                "Canonical GLB scene contains duplicate roots");
        visit(node_index);
    }
    require(std::all_of(reachable_nodes.begin(), reachable_nodes.end(),
                        [](bool value) { return value; })
                    && std::all_of(reachable_meshes.begin(),
                                   reachable_meshes.end(),
                                   [](bool value) { return value; }),
            "Canonical GLB contains unreachable node or mesh data");
}

void validateInstancing(const Json& root, CanonicalFamily family,
                        const std::vector<AccessorInfo>& accessors,
                        const std::vector<BufferViewInfo>& views,
                        const std::vector<std::uint8_t>& bytes,
                        std::size_t bin_offset) {
    bool found = false;
    for (const auto& node : root.at("nodes")) {
        if (!node.contains("extensions")
                || !node.at("extensions").is_object()
                || !node.at("extensions").contains(kGpuInstancingExtension)) {
            continue;
        }
        found = true;
        require(node.contains("mesh") && isNonNegativeInteger(node.at("mesh")),
                "Canonical GLB instancing node has no mesh");
        const auto& extension = node.at("extensions").at(
                kGpuInstancingExtension);
        require(extension.is_object() && extension.contains("attributes")
                        && extension.at("attributes").is_object()
                        && !extension.at("attributes").empty(),
                "Canonical GLB instance attributes are missing");
        std::optional<std::uint64_t> instance_count;
        for (const auto& attribute : extension.at("attributes").items()) {
            const auto& accessor = accessors.at(
                    accessorIndex(attribute.value(), accessors));
            require(!instance_count.has_value()
                            || *instance_count == accessor.count,
                    "Canonical GLB instance attributes are not aligned");
            instance_count = accessor.count;
            if (attribute.key() == "TRANSLATION"
                    || attribute.key() == "SCALE") {
                require(accessor.component_type == kFloatComponentType
                                && accessor.type == "VEC3",
                        "Canonical GLB instance translation/scale is invalid");
                if (attribute.key() == "SCALE") {
                    for (std::uint64_t element = 0U; element < accessor.count;
                         ++element) {
                        for (std::uint64_t component = 0U; component < 3U;
                             ++component) {
                            require(floatValue(accessor, views, bytes, bin_offset,
                                               element, component) > 0.0F,
                                    "Canonical GLB instance scale is not positive");
                        }
                    }
                }
            } else if (attribute.key() == "ROTATION") {
                require(accessor.component_type == kFloatComponentType
                                && accessor.type == "VEC4",
                        "Canonical GLB instance rotation is invalid");
                for (std::uint64_t element = 0U; element < accessor.count;
                     ++element) {
                    std::array<double, 4U> value{};
                    double squared_length = 0.0;
                    for (std::uint64_t component = 0U; component < 4U;
                         ++component) {
                        value[component] = floatValue(
                                accessor, views, bytes, bin_offset,
                                element, component);
                        squared_length += value[component] * value[component];
                    }
                    require(std::abs(std::sqrt(squared_length) - 1.0) <= 1.0e-4,
                            "Canonical GLB instance quaternion is not unit length");
                    require(value[3] > 0.0 || (value[3] == 0.0
                                    && (value[2] > 0.0 || (value[2] == 0.0
                                    && (value[1] > 0.0 || (value[1] == 0.0
                                    && value[0] >= 0.0))))),
                            "Canonical GLB instance quaternion sign is not canonical");
                }
            } else {
                require(featureSemanticIndex(attribute.key()).has_value(),
                        "Canonical GLB instance semantic is unsupported");
                require(!accessor.normalized && accessor.type == "SCALAR"
                                && (accessor.component_type
                                            == kUnsignedByteComponentType
                                    || accessor.component_type
                                            == kUnsignedShortComponentType
                                    || accessor.component_type
                                            == kUnsignedIntComponentType
                                    || accessor.component_type
                                            == kFloatComponentType),
                        "Canonical GLB instance feature accessor is invalid");
            }
        }
    }
    require((family == CanonicalFamily::instance_gltf2) == found,
            "Canonical GLB instancing structure does not match its family");
}

FeatureIdentityModel identityModel(CanonicalFamily family,
                                   const std::vector<std::string>& used) {
    if (family == CanonicalFamily::point_gltf2
            && contains(used, kMeshFeaturesExtension)) {
        return FeatureIdentityModel::point_feature_id;
    }
    if (family == CanonicalFamily::instance_gltf2
            && (contains(used, kMeshFeaturesExtension)
                || contains(used, kInstanceFeaturesExtension))) {
        return FeatureIdentityModel::instance_feature_id;
    }
    if (contains(used, kMeshFeaturesExtension)) {
        return FeatureIdentityModel::attribute_feature_id_property_table;
    }
    return FeatureIdentityModel::none;
}

std::uint64_t propertyCount(const Json& root) {
    const auto extensions = root.find("extensions");
    if (extensions == root.end() || !extensions->is_object()
            || !extensions->contains(kStructuralMetadataExtension)) {
        return 0U;
    }
    const auto& metadata = extensions->at(kStructuralMetadataExtension);
    if (!metadata.is_object() || !metadata.contains("propertyTables")
            || !metadata.at("propertyTables").is_array()) {
        return 0U;
    }
    std::uint64_t total = 0U;
    for (const auto& table : metadata.at("propertyTables")) {
        if (table.is_object() && table.contains("properties")
                && table.at("properties").is_object()) {
            total += table.at("properties").size();
        }
    }
    return total;
}

std::uint64_t featureCount(const Json& root) {
    const auto extensions = root.find("extensions");
    if (extensions == root.end() || !extensions->is_object()
            || !extensions->contains(kStructuralMetadataExtension)) {
        return 0U;
    }
    const auto& metadata = extensions->at(kStructuralMetadataExtension);
    if (!metadata.is_object() || !metadata.contains("propertyTables")
            || !metadata.at("propertyTables").is_array()) {
        return 0U;
    }
    std::uint64_t total = 0U;
    for (const auto& table : metadata.at("propertyTables")) {
        if (table.is_object() && table.contains("count")
                && table.at("count").is_number_unsigned()) {
            total += table.at("count").get<std::uint64_t>();
        }
    }
    return total;
}

Json summaryJson(const ValidationSummary& summary, bool include_hash) {
    const auto identity_name = [&summary]() {
        switch (summary.feature_identity_model) {
            case FeatureIdentityModel::none: return "NONE";
            case FeatureIdentityModel::legacy_batch_table_mapped:
                return "LEGACY_BATCH_TABLE_MAPPED";
            case FeatureIdentityModel::attribute_feature_id_property_table:
                return "ATTRIBUTE_FEATURE_ID_PROPERTY_TABLE";
            case FeatureIdentityModel::point_feature_id: return "POINT_FEATURE_ID";
            case FeatureIdentityModel::instance_feature_id:
                return "INSTANCE_FEATURE_ID";
        }
        throw std::invalid_argument("Canonical feature identity is invalid");
    }();
    Json json = {{"accessorCount", summary.accessor_count},
                 {"bufferCount", summary.buffer_count},
                 {"coordinateBasis", summary.coordinate_basis},
                 {"featureCount", summary.feature_count},
                 {"featureIdentityModel", identity_name},
                 {"imageCount", summary.image_count},
                 {"metadataPropertyCount", summary.metadata_property_count},
                 {"nodeCount", summary.node_count},
                 {"primitiveCount", summary.primitive_count},
                 {"requiredExtensions", summary.required_extensions},
                 {"sceneCount", summary.scene_count},
                 {"usedExtensions", summary.used_extensions},
                 {"validatorBuildSha256", summary.validator_build_sha256},
                 {"validatorName", summary.validator_name},
                 {"validatorVersion", summary.validator_version}};
    if (include_hash) json["validationHash"] = summary.validation_hash;
    return json;
}

std::uint64_t arraySize(const Json& root, const char* name) {
    const auto value = root.find(name);
    if (value == root.end()) return 0U;
    require(value->is_array(), "Canonical GLB summary array is invalid");
    return value->size();
}

}  // namespace

UploadDeclaration CanonicalArtifactEvidence::uploadDeclaration(
        CanonicalFamily family) const {
    UploadDeclaration declaration;
    declaration.canonical_family = family;
    declaration.output_size = output_size;
    declaration.output_sha256 = output_sha256;
    declaration.semantic_hash = semantic_hash;
    declaration.validation_manifest_sha256 = validation_manifest_sha256;
    declaration.validation_summary = validation_summary;
    return declaration;
}

CanonicalArtifactEvidence validateCanonicalGlb(
        const std::vector<std::uint8_t>& bytes, CanonicalFamily family,
        const ToolVersion& validator) {
    require(bytes.size() >= kGlbHeaderBytes + kChunkHeaderBytes,
            "Canonical GLB is too small");
    require(bytes.size() <= ProtocolLimits::kMaximumArtifactBytes,
            "Canonical GLB exceeds the artifact limit");
    require(bytes.size() % 4U == 0U,
            "Canonical GLB length is not four-byte aligned");
    require(readU32(bytes, 0U) == kGlbMagic && readU32(bytes, 4U) == kGlbVersion
                    && readU32(bytes, 8U) == bytes.size(),
            "Canonical GLB header is invalid");

    std::size_t offset = kGlbHeaderBytes;
    require(readU32(bytes, offset + 4U) == kJsonChunkType,
            "Canonical GLB first chunk is not JSON");
    const std::uint32_t json_length = readU32(bytes, offset);
    require(json_length > 0U && json_length % 4U == 0U,
            "Canonical GLB JSON chunk alignment is invalid");
    offset += kChunkHeaderBytes;
    require(offset <= bytes.size() && json_length <= bytes.size() - offset,
            "Canonical GLB JSON chunk is truncated");
    std::string json_text(reinterpret_cast<const char*>(bytes.data() + offset),
                          json_length);
    while (!json_text.empty() && json_text.back() == ' ') {
        json_text.pop_back();
    }
    offset += json_length;
    require(offset + kChunkHeaderBytes <= bytes.size(),
            "Canonical GLB has no embedded BIN chunk");
    const std::uint32_t bin_length = readU32(bytes, offset);
    require(bin_length > 0U && bin_length % 4U == 0U,
            "Canonical GLB BIN chunk alignment is invalid");
    require(readU32(bytes, offset + 4U) == kBinChunkType,
            "Canonical GLB second chunk is not BIN");
    offset += kChunkHeaderBytes;
    require(bin_length <= bytes.size() - offset && offset + bin_length == bytes.size(),
            "Canonical GLB BIN chunk length is invalid");

    Json root;
    try {
        root = Json::parse(json_text);
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Canonical GLB JSON is invalid");
    }
    require(root.is_object() && root.contains("asset")
                    && root.at("asset").is_object()
                    && root.at("asset").value("version", std::string()) == "2.0",
            "Canonical GLB asset version is not 2.0");
    rejectExternalUris(root);
    const auto required_extensions = sortedStrings(root, "extensionsRequired");
    const auto used_extensions = sortedStrings(root, "extensionsUsed");
    require(std::includes(used_extensions.begin(), used_extensions.end(),
                          required_extensions.begin(), required_extensions.end()),
            "Canonical required extensions are not a subset of used extensions");
    require(std::all_of(required_extensions.begin(), required_extensions.end(),
                        [](const std::string& extension) {
                            return kApprovedRequiredExtensions.find(extension)
                                    != kApprovedRequiredExtensions.end();
                        }),
            "Canonical GLB requires an unapproved extension");
    require(std::all_of(used_extensions.begin(), used_extensions.end(),
                        [](const std::string& extension) {
                            return kApprovedRequiredExtensions.find(extension)
                                    != kApprovedRequiredExtensions.end();
                        }),
            "Canonical GLB uses an unapproved extension");
    if (family == CanonicalFamily::instance_gltf2) {
        require(contains(used_extensions, kGpuInstancingExtension),
                "INSTANCE_GLTF2 lacks EXT_mesh_gpu_instancing");
    } else {
        require(!contains(required_extensions, kGpuInstancingExtension),
                "Non-instance canonical artifact requires GPU instancing");
    }

    const std::uint64_t buffer_length = validateBuffer(root, bin_length);
    const auto buffer_views = validateBufferViews(root, buffer_length);
    const auto accessors = validateAccessors(
            root, buffer_views, bytes, offset);
    std::vector<Bounds3D> mesh_bounds;
    const std::uint64_t primitive_count = countPrimitives(
            root, family, accessors, buffer_views, bytes, offset,
            mesh_bounds);
    validateNodeGraph(root);
    validateScenesAndReachability(root);
    validateInstancing(root, family, accessors, buffer_views, bytes, offset);
    if (family == CanonicalFamily::mesh_gltf2
        || family == CanonicalFamily::point_gltf2) {
        validateTexturesAndMaterials(root, buffer_views, bytes, offset);
        validateFeatureMetadata(root, accessors, buffer_views, bytes, offset);
        validateTransformedBounds(root, mesh_bounds);
    } else if (family == CanonicalFamily::instance_gltf2) {
        validateTexturesAndMaterials(root, buffer_views, bytes, offset);
        validateInstanceFeatureMetadata(
                root, accessors, buffer_views, bytes, offset);
    }

    CanonicalArtifactEvidence evidence;
    evidence.output_size = bytes.size();
    evidence.output_sha256 = sha256Hex(bytesString(bytes));
    const std::string mesh_policy = family == CanonicalFamily::mesh_gltf2
            ? std::string(kMeshCanonicalWriterVersion) + "\n"
                    + formats::kCanonicalTexturePolicyVersion + "\n"
            : std::string();
    evidence.semantic_hash = sha256Hex(
            canonicalFamilyName(family) + "\n" + mesh_policy
            + root.dump() + "\n"
            + sha256Hex(std::string(
                    reinterpret_cast<const char*>(bytes.data() + offset), bin_length)));
    auto& summary = evidence.validation_summary;
    summary.scene_count = arraySize(root, "scenes");
    summary.node_count = arraySize(root, "nodes");
    summary.primitive_count = primitive_count;
    summary.accessor_count = arraySize(root, "accessors");
    summary.buffer_count = arraySize(root, "buffers");
    summary.image_count = arraySize(root, "images");
    summary.feature_count = featureCount(root);
    summary.metadata_property_count = propertyCount(root);
    summary.feature_identity_model = identityModel(family, used_extensions);
    summary.required_extensions = required_extensions;
    summary.used_extensions = used_extensions;
    summary.validator_name = validator.name;
    summary.validator_version = validator.version;
    summary.validator_build_sha256 = validator.build_sha256;
    require(summary.accessor_count > 0U && summary.buffer_count > 0U,
            "Canonical GLB has no accessor or buffer evidence");
    require(!summary.validator_name.empty() && !summary.validator_version.empty()
                    && summary.validator_build_sha256.size() == 64U
                    && std::all_of(summary.validator_build_sha256.begin(),
                                   summary.validator_build_sha256.end(),
                                   [](char value) {
                                       return (value >= '0' && value <= '9')
                                               || (value >= 'a' && value <= 'f');
                                   }),
            "Canonical validator identity is invalid");
    summary.validation_hash = sha256Hex(summaryJson(summary, false).dump());
    evidence.validation_manifest_sha256 =
            sha256Hex(summaryJson(summary, true).dump());
    return evidence;
}

std::string metadataSemanticHash(
        const CanonicalArtifactEvidence& evidence) {
    constexpr const char* kMetadataSemanticDomain =
            "THREE_D_TILES_METADATA_SEMANTIC_V1";
    return sha256Hex(std::string(kMetadataSemanticDomain) + '\n'
                     + evidence.semantic_hash + '\n'
                     + summaryJson(evidence.validation_summary, true).dump());
}

}  // namespace clip_worker::normalization
