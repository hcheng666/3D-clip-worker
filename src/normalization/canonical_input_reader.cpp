#include "clip_worker/normalization/canonical_input_reader.hpp"

#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/glb.hpp"
#include "clip_worker/metadata/structural_metadata_reader.hpp"
#include "clip_worker/normalization/normalization_contract.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kUnsignedByte = 5121U;
constexpr std::uint32_t kUnsignedShort = 5123U;
constexpr std::uint32_t kUnsignedInt = 5125U;
constexpr std::uint32_t kFloat = 5126U;
constexpr std::uint32_t kPointsMode = 0U;
constexpr const char* kMeshFeaturesExtension = "EXT_mesh_features";
constexpr const char* kStructuralMetadataExtension = "EXT_structural_metadata";
constexpr double kMaximumExactFloatFeatureId = 16777216.0;

[[noreturn]] void invalid(const std::string& message) {
    throw formats::FormatError(
            formats::FormatErrorCode::normalization_output_invalid, message);
}

std::size_t unsignedSize(const Json& value, const char* field) {
    const auto item = value.find(field);
    if (item == value.end() || !item->is_number_unsigned()) {
        invalid(std::string("Canonical point integer field is invalid: ") + field);
    }
    const auto result = item->get<std::uint64_t>();
    if (result > std::numeric_limits<std::size_t>::max()) {
        invalid(std::string("Canonical point integer field is too large: ") + field);
    }
    return static_cast<std::size_t>(result);
}

void requireKeys(const Json& value, const std::set<std::string>& allowed,
                 const char* context) {
    if (!value.is_object()) invalid(std::string(context) + " is not an object");
    for (const auto& item : value.items()) {
        if (allowed.count(item.key()) == 0U) {
            invalid(std::string(context) + " contains a non-canonical field");
        }
    }
}

CanonicalArtifactEvidence validateExpectation(
        const std::vector<std::uint8_t>& bytes,
        const CanonicalInputExpectation& expectation,
        CanonicalFamily required_family) {
    if (expectation.family != required_family || expectation.input_size == 0U
            || expectation.input_size != bytes.size()
            || expectation.input_sha256.empty()
            || expectation.semantic_hash.empty()
            || expectation.validation_manifest_sha256.empty()
            || expectation.canonical_contract_version.empty()) {
        invalid("Canonical input expectation is incomplete");
    }
    const auto evidence = validateCanonicalGlb(
            bytes, required_family, expectation.validator);
    const auto v3_family = required_family == CanonicalFamily::mesh_gltf2
            ? v3::CanonicalFamily::mesh_gltf2
            : required_family == CanonicalFamily::point_gltf2
                    ? v3::CanonicalFamily::point_gltf2
                    : v3::CanonicalFamily::instance_gltf2;
    const std::string actual_semantic_hash =
            expectation.canonical_contract_version
                    == v3::canonicalContractVersion(v3_family)
            ? metadataSemanticHash(evidence) : evidence.semantic_hash;
    if (evidence.output_sha256 != expectation.input_sha256
            || actual_semantic_hash != expectation.semantic_hash
            || evidence.validation_manifest_sha256
                    != expectation.validation_manifest_sha256
            || evidence.output_size != expectation.input_size) {
        invalid("Canonical input immutable evidence mismatch");
    }
    return evidence;
}

struct PointAccessor {
    std::size_t buffer_view = 0U;
    std::size_t byte_offset = 0U;
    std::size_t count = 0U;
    std::size_t components = 0U;
    std::uint32_t component_type = 0U;
    bool normalized = false;
};

std::size_t components(const std::string& type) {
    if (type == "SCALAR") return 1U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4") return 4U;
    invalid("Canonical point accessor type is unsupported");
}

std::size_t componentBytes(std::uint32_t type) {
    if (type == kUnsignedByte) return 1U;
    if (type == kUnsignedShort) return 2U;
    if (type == kUnsignedInt || type == kFloat) return 4U;
    invalid("Canonical point accessor component type is unsupported");
}

std::uint32_t readUnsigned(const std::uint8_t* bytes,
                           std::uint32_t component_type) {
    if (component_type == kUnsignedByte) return bytes[0U];
    if (component_type == kUnsignedShort) {
        return static_cast<std::uint32_t>(bytes[0U])
                | (static_cast<std::uint32_t>(bytes[1U]) << 8U);
    }
    if (component_type == kUnsignedInt) {
        return static_cast<std::uint32_t>(bytes[0U])
                | (static_cast<std::uint32_t>(bytes[1U]) << 8U)
                | (static_cast<std::uint32_t>(bytes[2U]) << 16U)
                | (static_cast<std::uint32_t>(bytes[3U]) << 24U);
    }
    invalid("Canonical point unsigned component type is invalid");
}

float readFloat(const std::uint8_t* bytes) {
    float value = 0.0F;
    std::memcpy(&value, bytes, sizeof(value));
    if (!std::isfinite(value)) invalid("Canonical point FLOAT value is non-finite");
    return value;
}

geometry::Matrix4 nodeTransform(const Json& node) {
    if (!node.contains("matrix")) return geometry::Matrix4::identity();
    if (!node.at("matrix").is_array() || node.at("matrix").size() != 16U) {
        invalid("Canonical point node matrix is invalid");
    }
    std::array<double, 16U> values{};
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (!node.at("matrix").at(index).is_number()) {
            invalid("Canonical point node matrix value is invalid");
        }
        values[index] = node.at("matrix").at(index).get<double>();
    }
    return geometry::Matrix4::fromColumnMajor(values);
}

class PointDocumentReader final {
public:
    PointDocumentReader(const std::vector<std::uint8_t>& bytes,
                        const CanonicalPointReaderLimits& limits)
        : bytes_(bytes), limits_(limits),
          document_(formats::GlbParser::parse(formats::ByteView(bytes))) {
        root_ = Json::parse(document_.json_text);
        binary_ = formats::ByteView(
                bytes_.data() + document_.binary_offset,
                document_.binary_length);
    }

    point::PointScene read() {
        if (!root_.contains("buffers") || !root_.at("buffers").is_array()
                || root_.at("buffers").size() != 1U
                || root_.at("buffers").front().contains("uri")) {
            invalid("Canonical point buffer closure is invalid");
        }
        loadViews();
        loadAccessors();
        const Json& scenes = root_.at("scenes");
        const Json& nodes = root_.at("nodes");
        const Json& meshes = root_.at("meshes");
        if (!scenes.is_array() || scenes.size() != 1U
                || !nodes.is_array() || nodes.size() != 1U
                || !meshes.is_array() || meshes.size() != 1U) {
            invalid("Canonical point scene structure is invalid");
        }
        const Json& node = nodes.front();
        requireKeys(node, {"matrix", "mesh"}, "Canonical point node");
        if (unsignedSize(node, "mesh") != 0U) {
            invalid("Canonical point node mesh is invalid");
        }
        const Json& primitives = meshes.front().at("primitives");
        if (!primitives.is_array() || primitives.size() != 1U) {
            invalid("Canonical point primitive count is invalid");
        }
        const Json& primitive = primitives.front();
        requireKeys(primitive, {"attributes", "extensions", "mode"},
                    "Canonical point primitive");
        if (primitive.value("mode", std::numeric_limits<std::uint32_t>::max())
                    != kPointsMode
                || primitive.contains("indices")
                || !primitive.contains("attributes")
                || !primitive.at("attributes").is_object()) {
            invalid("Canonical point topology is invalid");
        }
        const Json& attributes = primitive.at("attributes");
        requireKeys(attributes,
                    {"COLOR_0", "NORMAL", "POSITION", "_FEATURE_ID_0"},
                    "Canonical point attributes");
        if (!attributes.contains("POSITION")) {
            invalid("Canonical point POSITION is missing");
        }
        point::PointScene scene;
        scene.positions = floatValues(
                unsignedSize(attributes, "POSITION"), 3U, false);
        if (scene.pointCount() == 0U
                || scene.pointCount() > limits_.maximum_points) {
            invalid("Canonical point count exceeds the configured limit");
        }
        if (attributes.contains("NORMAL")) {
            scene.normals = floatValues(
                    unsignedSize(attributes, "NORMAL"), 3U, false);
        }
        if (attributes.contains("COLOR_0")) {
            scene.colors_rgba = colorValues(
                    unsignedSize(attributes, "COLOR_0"));
        }
        if (attributes.contains("_FEATURE_ID_0")) {
            scene.feature_ids = featureValues(
                    unsignedSize(attributes, "_FEATURE_ID_0"));
            validateFeatureContract(primitive, scene.feature_ids);
        } else if (primitive.contains("extensions")) {
            invalid("Canonical point feature extension lacks an accessor");
        }
        scene.root_transform = nodeTransform(node);
        loadMetadata(scene);
        point::validatePointScene(scene);
        return scene;
    }

private:
    void loadViews() {
        if (!root_.contains("bufferViews")
                || !root_.at("bufferViews").is_array()) {
            invalid("Canonical point bufferViews are missing");
        }
        for (const Json& value : root_.at("bufferViews")) {
            requireKeys(value, {"buffer", "byteLength", "byteOffset", "target"},
                        "Canonical point bufferView");
            if (unsignedSize(value, "buffer") != 0U) {
                invalid("Canonical point bufferView references another buffer");
            }
            const std::size_t offset = value.value("byteOffset", 0U);
            const std::size_t length = unsignedSize(value, "byteLength");
            if (!binary_.contains(offset, length)) {
                invalid("Canonical point bufferView is outside BIN");
            }
            views_.push_back({offset, length});
        }
    }

    void loadAccessors() {
        if (!root_.contains("accessors") || !root_.at("accessors").is_array()) {
            invalid("Canonical point accessors are missing");
        }
        for (const Json& value : root_.at("accessors")) {
            requireKeys(value,
                        {"bufferView", "byteOffset", "componentType", "count",
                         "max", "min", "normalized", "type"},
                        "Canonical point accessor");
            PointAccessor accessor;
            accessor.buffer_view = unsignedSize(value, "bufferView");
            accessor.byte_offset = value.value("byteOffset", 0U);
            accessor.component_type = static_cast<std::uint32_t>(
                    unsignedSize(value, "componentType"));
            accessor.count = unsignedSize(value, "count");
            accessor.components = components(value.at("type").get<std::string>());
            accessor.normalized = value.value("normalized", false);
            if (accessor.buffer_view >= views_.size() || accessor.count == 0U) {
                invalid("Canonical point accessor range is invalid");
            }
            const std::size_t element_bytes = accessor.components
                    * componentBytes(accessor.component_type);
            const auto& view = views_.at(accessor.buffer_view);
            if (accessor.byte_offset > view.length
                    || accessor.count > (view.length - accessor.byte_offset)
                                            / element_bytes) {
                invalid("Canonical point accessor exceeds its bufferView");
            }
            accessors_.push_back(accessor);
        }
    }

    const std::uint8_t* accessorBytes(const PointAccessor& accessor) const {
        return binary_.data() + views_.at(accessor.buffer_view).offset
                + accessor.byte_offset;
    }

    std::vector<float> floatValues(std::size_t index, std::size_t components,
                                   bool normalized) const {
        if (index >= accessors_.size()) invalid("Canonical point accessor is absent");
        const auto& accessor = accessors_.at(index);
        if (accessor.component_type != kFloat
                || accessor.components != components
                || accessor.normalized != normalized) {
            invalid("Canonical point FLOAT accessor contract mismatch");
        }
        std::vector<float> result;
        result.reserve(accessor.count * components);
        const auto* source = accessorBytes(accessor);
        for (std::size_t value = 0U;
             value < accessor.count * components; ++value) {
            result.push_back(readFloat(source + value * sizeof(float)));
        }
        return result;
    }

    std::vector<std::uint8_t> colorValues(std::size_t index) const {
        if (index >= accessors_.size()) invalid("Canonical point color is absent");
        const auto& accessor = accessors_.at(index);
        if (accessor.component_type != kUnsignedByte
                || accessor.components != 4U || !accessor.normalized) {
            invalid("Canonical point color accessor contract mismatch");
        }
        const std::size_t count = accessor.count * accessor.components;
        const auto* source = accessorBytes(accessor);
        return {source, source + count};
    }

    std::vector<std::uint32_t> featureValues(std::size_t index) const {
        if (index >= accessors_.size()) invalid("Canonical point feature ID is absent");
        const auto& accessor = accessors_.at(index);
        if (accessor.components != 1U || accessor.normalized
                || (accessor.component_type != kUnsignedByte
                    && accessor.component_type != kUnsignedShort
                    && accessor.component_type != kUnsignedInt
                    && accessor.component_type != kFloat)) {
            invalid("Canonical point feature accessor contract mismatch");
        }
        std::vector<std::uint32_t> result;
        result.reserve(accessor.count);
        const auto* source = accessorBytes(accessor);
        const std::size_t width = componentBytes(accessor.component_type);
        for (std::size_t item = 0U; item < accessor.count; ++item) {
            if (accessor.component_type == kFloat) {
                const double value = readFloat(source + item * width);
                if (value < 0.0 || value > kMaximumExactFloatFeatureId
                        || std::floor(value) != value) {
                    invalid("Canonical point FLOAT feature ID is not exact");
                }
                result.push_back(static_cast<std::uint32_t>(value));
            } else {
                result.push_back(readUnsigned(
                        source + item * width, accessor.component_type));
            }
        }
        return result;
    }

    void validateFeatureContract(
            const Json& primitive,
            const std::vector<std::uint32_t>& feature_ids) const {
        if (!primitive.contains("extensions")
                || !primitive.at("extensions").is_object()) {
            invalid("Canonical point feature extension is missing");
        }
        const Json& extensions = primitive.at("extensions");
        requireKeys(extensions, {kMeshFeaturesExtension},
                    "Canonical point primitive extensions");
        const Json& mesh_features = extensions.at(kMeshFeaturesExtension);
        requireKeys(mesh_features, {"featureIds"}, "EXT_mesh_features");
        if (!mesh_features.at("featureIds").is_array()
                || mesh_features.at("featureIds").size() != 1U) {
            invalid("Canonical point feature set is invalid");
        }
        const Json& feature = mesh_features.at("featureIds").front();
        requireKeys(feature,
                    {"attribute", "featureCount", "propertyTable"},
                    "Canonical point feature ID set");
        if (unsignedSize(feature, "attribute") != 0U) {
            invalid("Canonical point feature attribute is not zero");
        }
        const std::size_t feature_count = unsignedSize(feature, "featureCount");
        if (feature_count == 0U
                || std::any_of(feature_ids.begin(), feature_ids.end(),
                               [feature_count](std::uint32_t value) {
                                   return value >= feature_count;
                               })) {
            invalid("Canonical point feature ID is outside featureCount");
        }
        property_table_ = feature.contains("propertyTable")
                ? std::optional<std::size_t>(
                          unsignedSize(feature, "propertyTable"))
                : std::nullopt;
    }

    void loadMetadata(point::PointScene& scene) const {
        const auto extensions = root_.find("extensions");
        if (extensions == root_.end()
                || !extensions->is_object()
                || !extensions->contains(kStructuralMetadataExtension)) {
            if (property_table_.has_value()) {
                invalid("Canonical point propertyTable lacks metadata");
            }
            return;
        }
        std::vector<metadata::MetadataBufferView> metadata_views;
        metadata_views.reserve(views_.size());
        for (const auto& view : views_) {
            metadata_views.push_back(
                    {&bytes_, document_.binary_offset + view.offset});
        }
        scene.feature_metadata = metadata::readStructuralMetadata(
                root_.dump(), metadata_views, limits_.metadata);
        if (property_table_.has_value()) {
            if (*property_table_
                    >= scene.feature_metadata->property_tables.size()) {
                invalid("Canonical point propertyTable is out of range");
            }
            scene.feature_metadata->primary_property_table = *property_table_;
        }
    }

    struct View {
        std::size_t offset = 0U;
        std::size_t length = 0U;
    };

    const std::vector<std::uint8_t>& bytes_;
    const CanonicalPointReaderLimits& limits_;
    formats::GlbDocument document_;
    Json root_;
    formats::ByteView binary_;
    std::vector<View> views_;
    std::vector<PointAccessor> accessors_;
    mutable std::optional<std::size_t> property_table_;
};

}  // namespace

CanonicalMeshReadResult CanonicalMeshReader::read(
        const std::vector<std::uint8_t>& bytes,
        const CanonicalInputExpectation& expectation,
        formats::GltfMeshReaderLimits limits) {
    CanonicalMeshReadResult result;
    result.evidence = validateExpectation(
            bytes, expectation, CanonicalFamily::mesh_gltf2);
    limits.enable_feature_metadata = true;
    limits.enable_gpu_instancing = false;
    result.scene = formats::GltfMeshReader::read(
            bytes, formats::GltfContentKind::glb,
            "canonical/input.glb", {}, limits).scene;
    return result;
}

CanonicalPointReadResult CanonicalPointReader::read(
        const std::vector<std::uint8_t>& bytes,
        const CanonicalInputExpectation& expectation,
        const CanonicalPointReaderLimits& limits) {
    CanonicalPointReadResult result;
    result.evidence = validateExpectation(
            bytes, expectation, CanonicalFamily::point_gltf2);
    result.scene = PointDocumentReader(bytes, limits).read();
    return result;
}

CanonicalInstanceReadResult CanonicalInstanceReader::read(
        const std::vector<std::uint8_t>& bytes,
        const CanonicalInputExpectation& expectation,
        formats::GltfMeshReaderLimits limits) {
    CanonicalInstanceReadResult result;
    result.evidence = validateExpectation(
            bytes, expectation, CanonicalFamily::instance_gltf2);
    limits.enable_feature_metadata = true;
    limits.enable_gpu_instancing = true;
    result.scene.model = formats::GltfMeshReader::read(
            bytes, formats::GltfContentKind::glb,
            "canonical/input.glb", {}, limits).scene;
    if (result.scene.model.nodes.size() != 1U
            || !result.scene.model.nodes.front().instancing.has_value()) {
        invalid("Canonical instance scene structure is invalid");
    }
    result.scene.root_transform =
            result.scene.model.nodes.front().local_transform;
    result.scene.model.nodes.front().local_transform =
            geometry::Matrix4::identity();
    instance::validateInstanceScene(result.scene);
    return result;
}

}  // namespace clip_worker::normalization
