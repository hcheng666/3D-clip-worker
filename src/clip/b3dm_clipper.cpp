#include "clip_worker/clip/b3dm_clipper.hpp"

#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/geometry/authorization_scope.hpp"
#include "clip_worker/geometry/clip_geometry.hpp"
#include "clip_worker/geometry/matrix4.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <webp/decode.h>
#include <webp/encode.h>

namespace clip_worker::clip {
namespace {

using Json = nlohmann::json;
using formats::FormatError;
using formats::FormatErrorCode;
using geometry::AuthorizationScope;
using geometry::ClipVertex;
using geometry::ClippedTriangle;
using geometry::Matrix4;
using geometry::Point2;

constexpr std::uint32_t kGlTriangles = 4U;
constexpr std::uint32_t kComponentUnsignedByte = 5121U;
constexpr std::uint32_t kComponentUnsignedShort = 5123U;
constexpr std::uint32_t kComponentUnsignedInt = 5125U;
constexpr std::uint32_t kComponentFloat = 5126U;
constexpr std::uint32_t kArrayBufferTarget = 34962U;
constexpr std::uint32_t kWrapClampToEdge = 33071U;
constexpr std::uint32_t kWrapMirroredRepeat = 33648U;
constexpr std::uint32_t kWrapRepeat = 10497U;
constexpr std::uint32_t kDefaultSamplerWrap = kWrapRepeat;
constexpr std::size_t kMaximumAccessorElements = 100000000U;
constexpr std::size_t kMaximumTextureDimension = 32768U;
constexpr std::size_t kMaximumUvRepeatSpan = 128U;
constexpr double kUvMaskTolerancePixels = 1.0;
constexpr const char* kUnlitExtension = "KHR_materials_unlit";
constexpr const char* kWebpExtension = "EXT_texture_webp";
constexpr const char* kSamplerWrapRField = "wrapR";

[[noreturn]] void unsupported(const std::string& message) {
    throw FormatError(FormatErrorCode::unsupported_content, message);
}

[[noreturn]] void invalidAccessor(const std::string& message) {
    throw FormatError(FormatErrorCode::invalid_accessor, message);
}

std::uint32_t readUint32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
           | (static_cast<std::uint32_t>(bytes[1]) << 8U)
           | (static_cast<std::uint32_t>(bytes[2]) << 16U)
           | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::size_t checkedSize(const Json& value, const char* field) {
    if (!value.contains(field) || !value.at(field).is_number_unsigned()) {
        invalidAccessor(std::string("glTF field must be unsigned: ") + field);
    }
    const auto result = value.at(field).get<std::uint64_t>();
    if (result > std::numeric_limits<std::size_t>::max()) {
        invalidAccessor(std::string("glTF field is too large: ") + field);
    }
    return static_cast<std::size_t>(result);
}

std::size_t optionalSize(const Json& value, const char* field, std::size_t fallback = 0U) {
    return value.contains(field) ? checkedSize(value, field) : fallback;
}

void requireAllowedKeys(const Json& object, const std::set<std::string>& allowed,
                        const char* description) {
    if (!object.is_object()) {
        unsupported(std::string(description) + " must be an object");
    }
    for (auto item = object.begin(); item != object.end(); ++item) {
        if (allowed.count(item.key()) == 0U) {
            unsupported(std::string(description) + " contains unsupported field: "
                        + item.key());
        }
    }
}

bool extensionArrayContains(const Json& document, const char* field,
                            const char* extension) {
    if (!document.contains(field)) {
        return false;
    }
    const auto& values = document.at(field);
    if (!values.is_array()) {
        unsupported(std::string("glTF ") + field + " must be an array");
    }
    bool found = false;
    for (const auto& value : values) {
        if (!value.is_string()) {
            unsupported(std::string("glTF ") + field + " contains a non-string");
        }
        const std::string name = value.get<std::string>();
        if (name != kUnlitExtension && name != kWebpExtension) {
            unsupported(std::string("glTF declares an unsupported extension: ") + name);
        }
        found = found || name == extension;
    }
    return found;
}

void validateTopLevelDocument(const Json& document) {
    requireAllowedKeys(document,
                       {"asset", "scene", "scenes", "nodes", "meshes", "materials",
                        "textures", "images", "samplers", "accessors", "bufferViews",
                        "buffers", "extensionsUsed", "extensionsRequired"},
                       "glTF document");
    if (!document.contains("asset") || !document.at("asset").is_object()
        || document.at("asset").value("version", std::string()) != "2.0") {
        unsupported("glTF asset.version must be 2.0");
    }
    static_cast<void>(extensionArrayContains(document, "extensionsUsed", kUnlitExtension));
    static_cast<void>(extensionArrayContains(document, "extensionsRequired",
                                             kUnlitExtension));
}

class GlbSource final {
public:
    GlbSource(const std::vector<std::uint8_t>& b3dm,
              const formats::B3dmDocument& document)
        : b3dm_(b3dm), document_(document) {
        try {
            json_ = Json::parse(document.glb.json_text);
        } catch (const Json::exception&) {
            throw FormatError(FormatErrorCode::invalid_json, "GLB JSON is invalid");
        }
        const auto& buffers = requiredArray("buffers");
        if (buffers.size() != 1U || buffers.at(0).contains("uri")) {
            unsupported("Only one embedded GLB buffer is supported");
        }
        const std::size_t declared_length = checkedSize(buffers.at(0), "byteLength");
        if (declared_length > document.glb.binary_length) {
            invalidAccessor("GLB buffer byteLength exceeds BIN chunk");
        }
        binary_ = b3dm.data() + document.glb_section.offset
                  + document.glb.binary_offset;
        binary_length_ = declared_length;
    }

    const Json& json() const noexcept { return json_; }

    const Json& requiredArray(const char* name) const {
        if (!json_.contains(name) || !json_.at(name).is_array()) {
            unsupported(std::string("GLB is missing array: ") + name);
        }
        return json_.at(name);
    }

    const Json& optionalArray(const char* name) const {
        static const Json empty = Json::array();
        if (!json_.contains(name)) { return empty; }
        if (!json_.at(name).is_array()) {
            unsupported(std::string("GLB field must be an array: ") + name);
        }
        return json_.at(name);
    }

    std::vector<double> readFloatAccessor(std::size_t accessor_index,
                                          std::size_t components,
                                          const char* expected_type) const {
        const AccessorRange range = accessorRange(accessor_index, components,
                                                  expected_type, kComponentFloat);
        std::vector<double> values(range.count * components);
        for (std::size_t element = 0; element < range.count; ++element) {
            const std::uint8_t* source = range.data + element * range.stride;
            for (std::size_t component = 0; component < components; ++component) {
                float value = 0.0F;
                std::memcpy(&value, source + component * sizeof(float), sizeof(float));
                if (!std::isfinite(value)) {
                    invalidAccessor("Float accessor contains a non-finite value");
                }
                values[element * components + component] = value;
            }
        }
        return values;
    }

    std::vector<std::uint32_t> readUnsignedScalarAccessor(
            std::size_t accessor_index) const {
        const auto& accessor = indexed(requiredArray("accessors"), accessor_index,
                                       "accessor");
        if (accessor.value("type", std::string()) != "SCALAR") {
            invalidAccessor("Index accessor must have SCALAR type");
        }
        if (!accessor.contains("componentType")
            || !accessor.at("componentType").is_number_unsigned()) {
            invalidAccessor("Index accessor componentType is missing");
        }
        const std::uint32_t component_type = accessor.at("componentType").get<std::uint32_t>();
        std::size_t component_size = 0U;
        if (component_type == kComponentUnsignedByte) { component_size = 1U; }
        else if (component_type == kComponentUnsignedShort) { component_size = 2U; }
        else if (component_type == kComponentUnsignedInt) { component_size = 4U; }
        else { invalidAccessor("Index accessor uses an unsupported componentType"); }
        const AccessorRange range = accessorRange(accessor_index, 1U, "SCALAR",
                                                  component_type, component_size);
        std::vector<std::uint32_t> values(range.count);
        for (std::size_t element = 0; element < range.count; ++element) {
            const std::uint8_t* source = range.data + element * range.stride;
            if (component_size == 1U) { values[element] = source[0]; }
            else if (component_size == 2U) {
                values[element] = static_cast<std::uint32_t>(source[0])
                                  | (static_cast<std::uint32_t>(source[1]) << 8U);
            } else { values[element] = readUint32(source); }
        }
        return values;
    }

    std::vector<std::uint8_t> bufferViewBytes(std::size_t view_index) const {
        const auto& view = indexed(requiredArray("bufferViews"), view_index, "bufferView");
        if (optionalSize(view, "buffer", 0U) != 0U) { unsupported("Only buffer zero is supported"); }
        const std::size_t offset = optionalSize(view, "byteOffset");
        const std::size_t length = checkedSize(view, "byteLength");
        requireBinaryRange(offset, length);
        return std::vector<std::uint8_t>(binary_ + offset, binary_ + offset + length);
    }

private:
    struct AccessorRange { const std::uint8_t* data; std::size_t count; std::size_t stride; };

    AccessorRange accessorRange(std::size_t accessor_index, std::size_t components,
                                const char* expected_type,
                                std::uint32_t expected_component_type,
                                std::size_t component_size = sizeof(float)) const {
        const auto& accessor = indexed(requiredArray("accessors"), accessor_index, "accessor");
        if (accessor.contains("sparse") || accessor.value("normalized", false)) {
            unsupported("Sparse or normalized accessors are not supported");
        }
        if (accessor.value("type", std::string()) != expected_type
            || accessor.value("componentType", 0U) != expected_component_type) {
            invalidAccessor("Accessor type or componentType does not match its semantic");
        }
        const std::size_t count = checkedSize(accessor, "count");
        if (count == 0U || count > kMaximumAccessorElements) {
            invalidAccessor("Accessor count is outside the supported range");
        }
        const auto& view = indexed(requiredArray("bufferViews"),
                                   checkedSize(accessor, "bufferView"), "bufferView");
        if (optionalSize(view, "buffer", 0U) != 0U) { unsupported("Only buffer zero is supported"); }
        const std::size_t element_size = components * component_size;
        const std::size_t stride = optionalSize(view, "byteStride", element_size);
        if (stride < element_size || stride > 252U || stride % component_size != 0U) {
            invalidAccessor("Accessor byteStride is invalid");
        }
        const std::size_t view_offset = optionalSize(view, "byteOffset");
        const std::size_t view_length = checkedSize(view, "byteLength");
        const std::size_t accessor_offset = optionalSize(accessor, "byteOffset");
        if (accessor_offset > view_length || (count > 1U
            && stride > (std::numeric_limits<std::size_t>::max() - element_size) / (count - 1U))) {
            invalidAccessor("Accessor byte range overflows");
        }
        const std::size_t required_length = (count - 1U) * stride + element_size;
        if (required_length > view_length - accessor_offset) {
            invalidAccessor("Accessor exceeds its bufferView");
        }
        if (view_offset > std::numeric_limits<std::size_t>::max() - accessor_offset) {
            invalidAccessor("Accessor absolute byte offset overflows");
        }
        requireBinaryRange(view_offset + accessor_offset, required_length);
        return {binary_ + view_offset + accessor_offset, count, stride};
    }

    const Json& indexed(const Json& array, std::size_t index, const char* name) const {
        if (index >= array.size() || !array.at(index).is_object()) {
            invalidAccessor(std::string("Invalid glTF ") + name + " index");
        }
        return array.at(index);
    }

    void requireBinaryRange(std::size_t offset, std::size_t length) const {
        if (offset > binary_length_ || length > binary_length_ - offset) {
            invalidAccessor("GLB binary reference is out of range");
        }
    }

    const std::vector<std::uint8_t>& b3dm_;
    const formats::B3dmDocument& document_;
    Json json_;
    const std::uint8_t* binary_ = nullptr;
    std::size_t binary_length_ = 0U;
};

std::uint32_t compatibleWrapR(const Json& sampler) {
    const auto& value = sampler.at(kSamplerWrapRField);
    if (!value.is_number_unsigned()) {
        unsupported("sampler wrapR must be an unsigned integer");
    }
    const std::uint64_t numeric_value = value.get<std::uint64_t>();
    if (numeric_value > std::numeric_limits<std::uint32_t>::max()) {
        unsupported("sampler wrapR exceeds the supported enum range");
    }
    const auto wrap_r = static_cast<std::uint32_t>(numeric_value);
    if (wrap_r != kWrapClampToEdge
        && wrap_r != kWrapMirroredRepeat
        && wrap_r != kWrapRepeat) {
        unsupported("sampler wrapR uses an unknown wrap mode");
    }
    return wrap_r;
}

SamplerCompatibilityDiagnostics inspectSamplerCompatibility(
        const GlbSource& source) {
    SamplerCompatibilityDiagnostics diagnostics;
    std::set<std::uint32_t> distinct_values;
    for (const auto& sampler : source.optionalArray("samplers")) {
        if (!sampler.is_object()) {
            unsupported("sampler must be an object");
        }
        if (!sampler.contains(kSamplerWrapRField)) {
            continue;
        }
        ++diagnostics.affected_sampler_count;
        distinct_values.insert(compatibleWrapR(sampler));
    }
    diagnostics.wrap_r_values.assign(distinct_values.begin(), distinct_values.end());
    return diagnostics;
}

template <std::size_t Count>
std::array<double, Count> numberArray(const Json& value, const char* description) {
    if (!value.is_array() || value.size() != Count) {
        unsupported(std::string(description) + " has an invalid element count");
    }
    std::array<double, Count> result{};
    for (std::size_t index = 0; index < Count; ++index) {
        if (!value.at(index).is_number()) { unsupported(std::string(description) + " contains a non-number"); }
        result[index] = value.at(index).get<double>();
        if (!std::isfinite(result[index])) {
            unsupported(std::string(description) + " contains a non-finite number");
        }
    }
    return result;
}

Matrix4 nodeTransform(const Json& node) {
    if (node.contains("matrix")) {
        if (node.contains("translation") || node.contains("rotation") || node.contains("scale")) {
            unsupported("glTF node cannot combine matrix and TRS");
        }
        return Matrix4::fromColumnMajor(numberArray<16U>(node.at("matrix"), "node matrix"));
    }
    const auto translation = node.contains("translation")
            ? numberArray<3U>(node.at("translation"), "node translation")
            : std::array<double, 3>{0.0, 0.0, 0.0};
    const auto rotation = node.contains("rotation")
            ? numberArray<4U>(node.at("rotation"), "node rotation")
            : std::array<double, 4>{0.0, 0.0, 0.0, 1.0};
    const auto scale = node.contains("scale")
            ? numberArray<3U>(node.at("scale"), "node scale")
            : std::array<double, 3>{1.0, 1.0, 1.0};
    return Matrix4::translation(translation) * Matrix4::quaternion(rotation) * Matrix4::scale(scale);
}

struct SceneTransforms {
    std::vector<std::optional<Matrix4>> mesh_transforms;
    std::vector<bool> reachable_nodes;
};

void visitNode(const Json& nodes, std::size_t node_index, const Matrix4& parent,
               std::vector<int>& state, SceneTransforms& result) {
    if (node_index >= nodes.size() || !nodes.at(node_index).is_object()) { unsupported("Scene references an invalid node"); }
    if (state[node_index] == 1) { unsupported("glTF node graph contains a cycle"); }
    if (state[node_index] == 2) { unsupported("glTF node is referenced by multiple parents"); }
    state[node_index] = 1;
    result.reachable_nodes[node_index] = true;
    const Json& node = nodes.at(node_index);
    if (node.contains("camera") || node.contains("skin") || node.contains("weights")) {
        unsupported("Camera, skin and morph weights are not supported");
    }
    const Matrix4 global = parent * nodeTransform(node);
    if (node.contains("mesh")) {
        const std::size_t mesh_index = checkedSize(node, "mesh");
        if (mesh_index >= result.mesh_transforms.size()) { unsupported("Node references an invalid mesh"); }
        if (result.mesh_transforms[mesh_index].has_value()) { unsupported("A mesh referenced by multiple nodes is not supported"); }
        result.mesh_transforms[mesh_index] = global;
    }
    if (node.contains("children")) {
        if (!node.at("children").is_array()) { unsupported("Node children must be an array"); }
        for (const auto& child : node.at("children")) {
            if (!child.is_number_unsigned()) { unsupported("Node child index must be unsigned"); }
            visitNode(nodes, child.get<std::size_t>(), global, state, result);
        }
    }
    state[node_index] = 2;
}

SceneTransforms sceneTransforms(const GlbSource& source) {
    const auto& meshes = source.requiredArray("meshes");
    const auto& nodes = source.requiredArray("nodes");
    const auto& scenes = source.requiredArray("scenes");
    const std::size_t scene_index = source.json().contains("scene") ? checkedSize(source.json(), "scene") : 0U;
    if (scene_index >= scenes.size() || !scenes.at(scene_index).is_object()
        || !scenes.at(scene_index).contains("nodes") || !scenes.at(scene_index).at("nodes").is_array()) {
        unsupported("Active glTF scene is invalid");
    }
    SceneTransforms result;
    result.mesh_transforms.resize(meshes.size());
    result.reachable_nodes.resize(nodes.size(), false);
    std::vector<int> state(nodes.size(), 0);
    for (const auto& root : scenes.at(scene_index).at("nodes")) {
        if (!root.is_number_unsigned()) { unsupported("Scene root node index must be unsigned"); }
        visitNode(nodes, root.get<std::size_t>(), Matrix4::identity(), state, result);
    }
    return result;
}

struct SamplerInfo { std::uint32_t wrap_s = kDefaultSamplerWrap; std::uint32_t wrap_t = kDefaultSamplerWrap; Json output = Json::object(); };
struct TextureInfo { std::size_t image = 0U; std::optional<std::size_t> sampler; };
struct MaterialInfo {
    Json output = Json::object();
    std::optional<std::size_t> texture;
    bool unlit = false;
};

class MaterialCatalog final {
public:
    explicit MaterialCatalog(const GlbSource& source) : source_(source) {}

    const MaterialInfo& material(std::size_t index) {
        const auto found = materials_.find(index); if (found != materials_.end()) { return found->second; }
        const auto& source_material = at(source_.optionalArray("materials"), index, "material");
        requireAllowedKeys(source_material, {"pbrMetallicRoughness", "extensions", "alphaMode", "alphaCutoff", "doubleSided"}, "material");
        MaterialInfo info;
        for (const char* field : {"alphaMode", "alphaCutoff", "doubleSided"}) { if (source_material.contains(field)) { info.output[field] = source_material.at(field); } }
        if (source_material.contains("extensions")) {
            const auto& extensions = source_material.at("extensions");
            requireAllowedKeys(extensions, {kUnlitExtension}, "material extensions");
            if (!extensions.contains(kUnlitExtension)
                || !extensions.at(kUnlitExtension).is_object()
                || !extensions.at(kUnlitExtension).empty()) {
                unsupported("KHR_materials_unlit payload must be an empty object");
            }
            info.output["extensions"] = {{kUnlitExtension, Json::object()}};
            info.unlit = true;
        }
        if (source_material.contains("pbrMetallicRoughness")) {
            const auto& pbr = source_material.at("pbrMetallicRoughness");
            requireAllowedKeys(pbr, {"baseColorFactor", "baseColorTexture", "metallicFactor", "roughnessFactor"}, "pbrMetallicRoughness");
            info.output["pbrMetallicRoughness"] = pbr;
            if (pbr.contains("baseColorTexture")) {
                const auto& texture = pbr.at("baseColorTexture");
                requireAllowedKeys(texture, {"index", "texCoord"}, "baseColorTexture");
                if (texture.value("texCoord", 0U) != 0U) { unsupported("Only TEXCOORD_0 is supported"); }
                info.texture = checkedSize(texture, "index");
            }
        }
        return materials_.emplace(index, std::move(info)).first->second;
    }

    const TextureInfo& texture(std::size_t index) {
        const auto found = textures_.find(index); if (found != textures_.end()) { return found->second; }
        const auto& source_texture = at(source_.optionalArray("textures"), index, "texture");
        requireAllowedKeys(source_texture, {"sampler", "source", "extensions"}, "texture");
        TextureInfo info;
        if (source_texture.contains("extensions")) {
            const auto& extensions = source_texture.at("extensions");
            requireAllowedKeys(extensions, {kWebpExtension}, "texture extensions");
            if (!extensions.contains(kWebpExtension)) {
                unsupported("Texture extensions do not contain EXT_texture_webp");
            }
            const auto& webp = extensions.at(kWebpExtension);
            requireAllowedKeys(webp, {"source"}, "EXT_texture_webp");
            info.image = checkedSize(webp, "source");
        } else {
            // Some production producers use image/webp directly as texture.source.
            // Normalize that legacy representation to EXT_texture_webp in the output.
            info.image = checkedSize(source_texture, "source");
        }
        if (source_texture.contains("sampler")) { info.sampler = checkedSize(source_texture, "sampler"); }
        return textures_.emplace(index, std::move(info)).first->second;
    }

    const SamplerInfo& sampler(std::optional<std::size_t> index) {
        if (!index.has_value()) { return default_sampler_; }
        const auto found = samplers_.find(*index); if (found != samplers_.end()) { return found->second; }
        const auto& source_sampler = at(source_.optionalArray("samplers"), *index, "sampler");
        requireAllowedKeys(source_sampler,
                           {"magFilter", "minFilter", "wrapS", "wrapT",
                            kSamplerWrapRField},
                           "sampler");
        SamplerInfo info; info.wrap_s = source_sampler.value("wrapS", kDefaultSamplerWrap); info.wrap_t = source_sampler.value("wrapT", kDefaultSamplerWrap);
        validateWrap(info.wrap_s); validateWrap(info.wrap_t); info.output = source_sampler;
        if (source_sampler.contains(kSamplerWrapRField)) {
            static_cast<void>(compatibleWrapR(source_sampler));
            info.output.erase(kSamplerWrapRField);
        }
        return samplers_.emplace(*index, std::move(info)).first->second;
    }

    std::vector<std::uint8_t> imageBytes(std::size_t index) const {
        const auto& image = at(source_.optionalArray("images"), index, "image");
        requireAllowedKeys(image, {"bufferView", "mimeType"}, "image");
        if (image.value("mimeType", std::string()) != "image/webp") { unsupported("Only embedded image/webp is supported"); }
        return source_.bufferViewBytes(checkedSize(image, "bufferView"));
    }

private:
    static const Json& at(const Json& array, std::size_t index, const char* name) {
        if (index >= array.size() || !array.at(index).is_object()) { unsupported(std::string("Invalid ") + name + " index"); }
        return array.at(index);
    }
    static void validateWrap(std::uint32_t value) {
        if (value == kWrapMirroredRepeat) { unsupported("MIRRORED_REPEAT is not supported"); }
        if (value != kWrapRepeat && value != kWrapClampToEdge) { unsupported("Sampler uses an unknown wrap mode"); }
    }
    const GlbSource& source_;
    std::map<std::size_t, MaterialInfo> materials_;
    std::map<std::size_t, TextureInfo> textures_;
    std::map<std::size_t, SamplerInfo> samplers_;
    SamplerInfo default_sampler_;
};

struct MaskUse { std::array<Point2, 3> triangle; SamplerInfo sampler; };

class BufferBuilder final {
public:
    std::size_t addFloatAccessor(const std::vector<float>& values, std::size_t components,
                                 const char* type, bool include_min_max) {
        if (values.empty() || values.size() % components != 0U) { throw std::invalid_argument("Output accessor values are invalid"); }
        align(4U, 0U); const std::size_t offset = bytes_.size();
        const auto* raw = reinterpret_cast<const std::uint8_t*>(values.data());
        bytes_.insert(bytes_.end(), raw, raw + values.size() * sizeof(float));
        const std::size_t view_index = views_.size();
        views_.push_back({{"buffer", 0U}, {"byteOffset", offset}, {"byteLength", values.size() * sizeof(float)}, {"target", kArrayBufferTarget}});
        Json accessor{{"bufferView", view_index}, {"componentType", kComponentFloat}, {"count", values.size() / components}, {"type", type}};
        if (include_min_max) {
            std::vector<float> minimum(components, std::numeric_limits<float>::infinity());
            std::vector<float> maximum(components, -std::numeric_limits<float>::infinity());
            for (std::size_t element = 0; element < values.size() / components; ++element) {
                for (std::size_t component = 0; component < components; ++component) {
                    const float value = values[element * components + component];
                    minimum[component] = std::min(minimum[component], value); maximum[component] = std::max(maximum[component], value);
                }
            }
            accessor["min"] = minimum; accessor["max"] = maximum;
        }
        const std::size_t accessor_index = accessors_.size(); accessors_.push_back(std::move(accessor)); return accessor_index;
    }
    std::size_t addBytes(const std::vector<std::uint8_t>& values) {
        if (values.empty()) { throw std::invalid_argument("Output image is empty"); }
        align(4U, 0U); const std::size_t offset = bytes_.size(); bytes_.insert(bytes_.end(), values.begin(), values.end());
        const std::size_t view_index = views_.size(); views_.push_back({{"buffer", 0U}, {"byteOffset", offset}, {"byteLength", values.size()}}); return view_index;
    }
    void finish() { align(4U, 0U); }
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    const Json& views() const noexcept { return views_; }
    const Json& accessors() const noexcept { return accessors_; }
private:
    void align(std::size_t alignment, std::uint8_t padding) { while (bytes_.size() % alignment != 0U) { bytes_.push_back(padding); } }
    std::vector<std::uint8_t> bytes_; Json views_ = Json::array(); Json accessors_ = Json::array();
};

struct VertexLayout {
    bool has_uv = false; bool has_normal = false; bool has_color = false;
    std::size_t color_components = 0U; std::size_t uv_offset = 0U; std::size_t normal_offset = 0U;
    std::size_t color_offset = 0U; std::size_t payload_size = 0U;
};
struct PrimitiveOutput {
    Json json; std::optional<std::size_t> source_material; std::map<std::size_t, std::vector<MaskUse>> image_uses;
    std::uint64_t input_vertices = 0U; std::uint64_t input_triangles = 0U; std::uint64_t output_vertices = 0U; std::uint64_t output_triangles = 0U;
};

ClipVertex makeVertex(std::uint32_t index, const std::vector<double>& positions,
                      const std::vector<double>& uv, const std::vector<double>& normals,
                      const std::vector<double>& colors, const VertexLayout& layout,
                      const Matrix4& world_transform, const AuthorizationScope& scope) {
    const std::size_t vertex = index; ClipVertex result;
    result.local_position = {positions.at(vertex * 3U), positions.at(vertex * 3U + 1U), positions.at(vertex * 3U + 2U)};
    const auto projected = scope.projectEcef(world_transform.transformPoint(result.local_position));
    result.projected = projected.horizontal; result.projected_height = projected.height; result.attributes.resize(layout.payload_size);
    if (layout.has_uv) { result.attributes[layout.uv_offset] = uv.at(vertex * 2U); result.attributes[layout.uv_offset + 1U] = uv.at(vertex * 2U + 1U); }
    if (layout.has_normal) { for (std::size_t component = 0; component < 3U; ++component) { result.attributes[layout.normal_offset + component] = normals.at(vertex * 3U + component); } }
    if (layout.has_color) { for (std::size_t component = 0; component < layout.color_components; ++component) { result.attributes[layout.color_offset + component] = colors.at(vertex * layout.color_components + component); } }
    return result;
}

void appendFragment(const ClippedTriangle& fragment, const VertexLayout& layout,
                    std::vector<float>& positions, std::vector<float>& uv,
                    std::vector<float>& normals, std::vector<float>& colors) {
    for (const auto& vertex : fragment) {
        for (const double value : vertex.local_position) { positions.push_back(static_cast<float>(value)); }
        if (layout.has_uv) { uv.push_back(static_cast<float>(vertex.attributes[layout.uv_offset])); uv.push_back(static_cast<float>(vertex.attributes[layout.uv_offset + 1U])); }
        if (layout.has_normal) {
            const double x = vertex.attributes[layout.normal_offset], y = vertex.attributes[layout.normal_offset + 1U], z = vertex.attributes[layout.normal_offset + 2U];
            const double length = std::sqrt(x * x + y * y + z * z); if (length <= 1.0e-15) { unsupported("Interpolated normal has zero length"); }
            normals.push_back(static_cast<float>(x / length)); normals.push_back(static_cast<float>(y / length)); normals.push_back(static_cast<float>(z / length));
        }
        if (layout.has_color) { for (std::size_t component = 0; component < layout.color_components; ++component) { colors.push_back(static_cast<float>(vertex.attributes[layout.color_offset + component])); } }
    }
}

PrimitiveOutput processPrimitive(const GlbSource& source, const Json& primitive,
                                 const Matrix4& world_transform, const AuthorizationScope& scope,
                                 MaterialCatalog& materials, BufferBuilder& output_buffer) {
    requireAllowedKeys(primitive, {"attributes", "indices", "material", "mode"}, "primitive");
    if (primitive.value("mode", kGlTriangles) != kGlTriangles) { unsupported("Only TRIANGLES primitives are supported"); }
    if (!primitive.contains("attributes") || !primitive.at("attributes").is_object()) { invalidAccessor("Primitive attributes are missing"); }
    const Json& attributes = primitive.at("attributes");
    requireAllowedKeys(attributes, {"POSITION", "TEXCOORD_0", "NORMAL", "COLOR_0", "_BATCHID"}, "primitive attributes");
    if (!attributes.contains("POSITION")) { invalidAccessor("Primitive POSITION is missing"); }
    const auto positions = source.readFloatAccessor(checkedSize(attributes, "POSITION"), 3U, "VEC3");
    const std::size_t vertex_count = positions.size() / 3U; VertexLayout layout;
    std::vector<double> uv, normals, colors;
    if (attributes.contains("TEXCOORD_0")) { layout.has_uv = true; layout.uv_offset = layout.payload_size; layout.payload_size += 2U; uv = source.readFloatAccessor(checkedSize(attributes, "TEXCOORD_0"), 2U, "VEC2"); if (uv.size() / 2U != vertex_count) { invalidAccessor("TEXCOORD_0 count differs from POSITION"); } }
    if (attributes.contains("NORMAL")) { layout.has_normal = true; layout.normal_offset = layout.payload_size; layout.payload_size += 3U; normals = source.readFloatAccessor(checkedSize(attributes, "NORMAL"), 3U, "VEC3"); if (normals.size() / 3U != vertex_count) { invalidAccessor("NORMAL count differs from POSITION"); } }
    if (attributes.contains("COLOR_0")) {
        const std::size_t accessor_index = checkedSize(attributes, "COLOR_0");
        const auto& accessor = source.requiredArray("accessors").at(accessor_index);
        const std::string type = accessor.value("type", std::string()); layout.color_components = type == "VEC4" ? 4U : type == "VEC3" ? 3U : 0U;
        if (layout.color_components == 0U) { invalidAccessor("COLOR_0 must be VEC3 or VEC4"); }
        colors = source.readFloatAccessor(accessor_index, layout.color_components, type.c_str());
        layout.has_color = true; layout.color_offset = layout.payload_size; layout.payload_size += layout.color_components;
        if (colors.size() / layout.color_components != vertex_count) { invalidAccessor("COLOR_0 count differs from POSITION"); }
    }
    if (attributes.contains("_BATCHID")) { const auto ids = source.readUnsignedScalarAccessor(checkedSize(attributes, "_BATCHID")); if (ids.size() != vertex_count || std::any_of(ids.begin(), ids.end(), [](std::uint32_t value) { return value != 0U; })) { unsupported("Only single-feature _BATCHID=0 is supported"); } }
    std::vector<std::uint32_t> indices;
    if (primitive.contains("indices")) { indices = source.readUnsignedScalarAccessor(checkedSize(primitive, "indices")); }
    else { indices.resize(vertex_count); for (std::size_t index = 0; index < vertex_count; ++index) { indices[index] = static_cast<std::uint32_t>(index); } }
    if (indices.empty() || indices.size() % 3U != 0U || std::any_of(indices.begin(), indices.end(), [vertex_count](std::uint32_t value) { return value >= vertex_count; })) { invalidAccessor("Primitive triangle indices are invalid"); }
    PrimitiveOutput result; result.input_vertices = vertex_count; result.input_triangles = indices.size() / 3U;
    std::optional<std::size_t> texture_index;
    if (primitive.contains("material")) { result.source_material = checkedSize(primitive, "material"); texture_index = materials.material(*result.source_material).texture; if (texture_index.has_value() && !layout.has_uv) { unsupported("Textured primitive is missing TEXCOORD_0"); } }
    std::vector<float> output_positions, output_uv, output_normals, output_colors; std::vector<std::array<Point2, 3>> retained_uv; geometry::AuthorizationTriangleIndex::QueryWorkspace scope_query_workspace;
    for (std::size_t index = 0; index < indices.size(); index += 3U) {
        const ClippedTriangle triangle{makeVertex(indices[index], positions, uv, normals, colors, layout, world_transform, scope), makeVertex(indices[index + 1U], positions, uv, normals, colors, layout, world_transform, scope), makeVertex(indices[index + 2U], positions, uv, normals, colors, layout, world_transform, scope)};
        scope.queryTriangles(triangle, scope_query_workspace);
        for (const auto& fragment : geometry::TriangleClipper::clip(triangle, scope_query_workspace.triangles)) {
            appendFragment(fragment, layout, output_positions, output_uv, output_normals, output_colors);
            if (texture_index.has_value()) { retained_uv.push_back({Point2{fragment[0].attributes[layout.uv_offset], fragment[0].attributes[layout.uv_offset + 1U]}, Point2{fragment[1].attributes[layout.uv_offset], fragment[1].attributes[layout.uv_offset + 1U]}, Point2{fragment[2].attributes[layout.uv_offset], fragment[2].attributes[layout.uv_offset + 1U]}}); }
        }
    }
    if (output_positions.empty()) { return result; }
    result.output_vertices = output_positions.size() / 3U; result.output_triangles = result.output_vertices / 3U; result.json["mode"] = kGlTriangles;
    result.json["attributes"]["POSITION"] = output_buffer.addFloatAccessor(output_positions, 3U, "VEC3", true);
    if (layout.has_uv) { result.json["attributes"]["TEXCOORD_0"] = output_buffer.addFloatAccessor(output_uv, 2U, "VEC2", false); }
    if (layout.has_normal) { result.json["attributes"]["NORMAL"] = output_buffer.addFloatAccessor(output_normals, 3U, "VEC3", false); }
    if (layout.has_color) { result.json["attributes"]["COLOR_0"] = output_buffer.addFloatAccessor(output_colors, layout.color_components, layout.color_components == 4U ? "VEC4" : "VEC3", false); }
    if (texture_index.has_value()) { const auto& texture = materials.texture(*texture_index); const auto& sampler = materials.sampler(texture.sampler); auto& uses = result.image_uses[texture.image]; for (const auto& triangle : retained_uv) { uses.push_back({triangle, sampler}); } }
    return result;
}

double edgeDistance(const Point2& a, const Point2& b, const Point2& p) {
    const double dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
    if (length2 <= 1.0e-15) { return std::hypot(p.x - a.x, p.y - a.y); }
    const double t = std::max(0.0, std::min(1.0, ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2));
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}
bool insideOrNear(const std::array<Point2, 3>& t, const Point2& p) {
    auto sign = [](const Point2& p1, const Point2& p2, const Point2& p3) { return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y); };
    const double a = sign(p, t[0], t[1]), b = sign(p, t[1], t[2]), c = sign(p, t[2], t[0]);
    if (!((a < 0.0 || b < 0.0 || c < 0.0) && (a > 0.0 || b > 0.0 || c > 0.0))) { return true; }
    return edgeDistance(t[0], t[1], p) <= kUvMaskTolerancePixels || edgeDistance(t[1], t[2], p) <= kUvMaskTolerancePixels || edgeDistance(t[2], t[0], p) <= kUvMaskTolerancePixels;
}
std::vector<int> wrapShifts(double minimum, double maximum, std::uint32_t mode) {
    if (mode == kWrapClampToEdge) { return {0}; }
    if (std::ceil(maximum) - std::floor(minimum) + 1.0 > static_cast<double>(kMaximumUvRepeatSpan)) { unsupported("UV repeat span exceeds the safety limit"); }
    std::vector<int> result; for (int shift = static_cast<int>(std::floor(minimum)) - 1; shift <= static_cast<int>(std::ceil(maximum)) + 1; ++shift) { result.push_back(shift); } return result;
}
void rasterize(const MaskUse& use, std::size_t width, std::size_t height, std::vector<std::uint8_t>& mask) {
    const double texture_width = static_cast<double>(width);
    const double texture_height = static_cast<double>(height);
    double min_u = use.triangle[0].x, max_u = min_u, min_v = use.triangle[0].y, max_v = min_v;
    for (const auto& p : use.triangle) { min_u = std::min(min_u, p.x); max_u = std::max(max_u, p.x); min_v = std::min(min_v, p.y); max_v = std::max(max_v, p.y); }
    for (const int su : wrapShifts(min_u, max_u, use.sampler.wrap_s)) for (const int sv : wrapShifts(min_v, max_v, use.sampler.wrap_t)) {
        std::array<Point2, 3> pixels{};
        for (std::size_t i = 0; i < 3U; ++i) { double u = use.triangle[i].x - su, v = use.triangle[i].y - sv; if (use.sampler.wrap_s == kWrapClampToEdge) { u = std::max(0.0, std::min(1.0, u)); } if (use.sampler.wrap_t == kWrapClampToEdge) { v = std::max(0.0, std::min(1.0, v)); } pixels[i] = {u * texture_width, v * texture_height}; }
        double min_x = pixels[0].x, max_x = min_x, min_y = pixels[0].y, max_y = min_y; for (const auto& p : pixels) { min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x); min_y = std::min(min_y, p.y); max_y = std::max(max_y, p.y); }
        const int start_x = std::max(0, static_cast<int>(std::floor(min_x - kUvMaskTolerancePixels))), end_x = std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(max_x + kUvMaskTolerancePixels)));
        const int start_y = std::max(0, static_cast<int>(std::floor(min_y - kUvMaskTolerancePixels))), end_y = std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(max_y + kUvMaskTolerancePixels)));
        for (int y = start_y; y <= end_y; ++y) for (int x = start_x; x <= end_x; ++x) if (insideOrNear(pixels, {x + 0.5, y + 0.5})) { mask[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = 1U; }
    }
}

std::vector<std::uint8_t> encodeExactLosslessWebp(
        const std::vector<std::uint8_t>& rgba, int width, int height) {
    WebPConfig config;
    if (WebPConfigInit(&config) == 0) {
        unsupported("Masked WebP encoder configuration failed");
    }
    config.lossless = 1;
    config.quality = 100.0F;
    config.method = 6;
    config.exact = 1;
    if (WebPValidateConfig(&config) == 0) {
        unsupported("Masked WebP encoder configuration is invalid");
    }

    WebPPicture picture;
    if (WebPPictureInit(&picture) == 0) {
        unsupported("Masked WebP picture initialization failed");
    }
    picture.use_argb = 1;
    picture.width = width;
    picture.height = height;

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;

    const int stride = width * 4;
    if (WebPPictureImportRGBA(&picture, rgba.data(), stride) == 0) {
        WebPPictureFree(&picture);
        WebPMemoryWriterClear(&writer);
        unsupported("Masked WebP pixel import failed");
    }
    const int encoded = WebPEncode(&config, &picture);
    WebPPictureFree(&picture);
    if (encoded == 0 || writer.mem == nullptr || writer.size == 0U) {
        WebPMemoryWriterClear(&writer);
        unsupported("Masked WebP lossless encoding failed");
    }

    std::vector<std::uint8_t> result(writer.mem, writer.mem + writer.size);
    WebPMemoryWriterClear(&writer);
    return result;
}

std::vector<std::uint8_t> maskWebp(const std::vector<std::uint8_t>& source,
                                   const std::vector<MaskUse>& uses) {
    int width_value = 0, height_value = 0;
    if (uses.empty() || WebPGetInfo(source.data(), source.size(), &width_value, &height_value) == 0 || width_value <= 0 || height_value <= 0 || static_cast<std::size_t>(width_value) > kMaximumTextureDimension || static_cast<std::size_t>(height_value) > kMaximumTextureDimension) { unsupported("Embedded WebP dimensions or mask are invalid"); }
    const std::size_t width = static_cast<std::size_t>(width_value), height = static_cast<std::size_t>(height_value);
    if (width > std::numeric_limits<std::size_t>::max() / height || width * height > std::numeric_limits<std::size_t>::max() / 4U) { unsupported("Embedded WebP pixel count is too large"); }
    std::uint8_t* decoded = WebPDecodeRGBA(source.data(), source.size(), &width_value, &height_value); if (decoded == nullptr) { unsupported("Embedded WebP decoding failed"); }
    const std::size_t rgba_size = width * height * 4U; std::vector<std::uint8_t> rgba(decoded, decoded + rgba_size); WebPFree(decoded);
    std::vector<std::uint8_t> mask(width * height, 0U); for (const auto& use : uses) { rasterize(use, width, height, mask); }
    for (std::size_t pixel = 0; pixel < mask.size(); ++pixel) if (mask[pixel] == 0U) { std::fill_n(rgba.begin() + static_cast<std::ptrdiff_t>(pixel * 4U), 4U, 0U); }
    std::vector<std::uint8_t> result = encodeExactLosslessWebp(
            rgba, width_value, height_value);

    // Decode the exact emitted bytes so a codec or buffer error can never publish
    // non-zero pixels outside the authorized UV mask.
    int verified_width = 0;
    int verified_height = 0;
    std::uint8_t* verified = WebPDecodeRGBA(result.data(), result.size(),
                                             &verified_width, &verified_height);
    if (verified == nullptr || verified_width != width_value
        || verified_height != height_value) {
        WebPFree(verified);
        unsupported("Masked WebP verification failed");
    }
    for (std::size_t pixel = 0; pixel < mask.size(); ++pixel) {
        if (mask[pixel] == 0U) {
            const std::size_t offset = pixel * 4U;
            if (verified[offset] != 0U || verified[offset + 1U] != 0U
                || verified[offset + 2U] != 0U || verified[offset + 3U] != 0U) {
                WebPFree(verified);
                unsupported("Masked WebP contains a non-zero pixel outside the UV mask");
            }
        }
    }
    WebPFree(verified);
    return result;
}

struct SanitizedScene {
    Json scenes = Json::array();
    Json nodes = Json::array();
};

std::optional<std::size_t> appendSanitizedNode(
        const Json& source_nodes, std::size_t source_index,
        const std::vector<std::optional<std::size_t>>& mesh_mapping,
        Json& output_nodes) {
    const auto& original = source_nodes.at(source_index);
    Json children = Json::array();
    if (original.contains("children")) {
        for (const auto& child : original.at("children")) {
            const auto mapped = appendSanitizedNode(
                    source_nodes, child.get<std::size_t>(), mesh_mapping, output_nodes);
            if (mapped.has_value()) {
                children.push_back(*mapped);
            }
        }
    }

    std::optional<std::size_t> mesh;
    if (original.contains("mesh")) {
        const std::size_t source_mesh = checkedSize(original, "mesh");
        if (source_mesh < mesh_mapping.size()) {
            mesh = mesh_mapping[source_mesh];
        }
    }
    if (!mesh.has_value() && children.empty()) {
        return std::nullopt;
    }

    Json output = Json::object();
    for (const char* field : {"matrix", "translation", "rotation", "scale"}) {
        if (original.contains(field)) {
            output[field] = original.at(field);
        }
    }
    if (mesh.has_value()) {
        output["mesh"] = *mesh;
    }
    if (!children.empty()) {
        output["children"] = std::move(children);
    }
    const std::size_t output_index = output_nodes.size();
    output_nodes.push_back(std::move(output));
    return output_index;
}

SanitizedScene sanitizeActiveScene(
        const GlbSource& source,
        const std::vector<std::optional<std::size_t>>& mesh_mapping) {
    const auto& source_scenes = source.requiredArray("scenes");
    const auto& source_nodes = source.requiredArray("nodes");
    const std::size_t active_index = source.json().contains("scene")
            ? checkedSize(source.json(), "scene") : 0U;
    const auto& active_scene = source_scenes.at(active_index);

    SanitizedScene result;
    Json roots = Json::array();
    for (const auto& source_root : active_scene.at("nodes")) {
        const auto root = appendSanitizedNode(source_nodes,
                                              source_root.get<std::size_t>(),
                                              mesh_mapping, result.nodes);
        if (root.has_value()) {
            roots.push_back(*root);
        }
    }
    if (roots.empty()) {
        unsupported("Clipped glTF has triangles but no reachable scene roots");
    }
    result.scenes.push_back({{"nodes", std::move(roots)}});
    return result;
}

std::string sanitizeBatchTable(const std::string& source_json) {
    if (source_json.empty()) {
        return {};
    }
    const Json source = Json::parse(source_json);
    if (!source.is_object()) {
        unsupported("B3DM batch table must be a JSON object");
    }
    Json output = Json::object();
    for (auto property = source.begin(); property != source.end(); ++property) {
        if (!property.value().is_array() || property.value().size() != 1U) {
            unsupported("B3DM batch table properties must contain exactly one feature");
        }
        const auto& value = property.value().at(0U);
        if (value.is_object()) {
            unsupported("Complex B3DM batch table properties are not supported");
        }
        output[property.key()] = property.value();
    }
    return output.empty() ? std::string() : output.dump();
}

std::optional<std::array<double, 3>> rtcCenter(const Json& feature_table) {
    requireAllowedKeys(feature_table, {"BATCH_LENGTH", "RTC_CENTER"},
                       "B3DM feature table");
    if (!feature_table.contains("RTC_CENTER")) {
        return std::nullopt;
    }
    return numberArray<3U>(feature_table.at("RTC_CENTER"), "RTC_CENTER");
}

std::vector<std::uint8_t> buildGlb(Json json, BufferBuilder& buffer) {
    buffer.finish(); json["buffers"] = Json::array({{{"byteLength", buffer.bytes().size()}}}); json["bufferViews"] = buffer.views(); json["accessors"] = buffer.accessors();
    std::string text = json.dump(); while (text.size() % 4U != 0U) { text.push_back(' '); }
    if (text.size() > std::numeric_limits<std::uint32_t>::max() || buffer.bytes().size() > std::numeric_limits<std::uint32_t>::max()) { unsupported("Output GLB exceeds the format limit"); }
    std::vector<std::uint8_t> output; output.insert(output.end(), {'g', 'l', 'T', 'F'}); appendUint32(output, formats::GlbParser::kSupportedVersion); appendUint32(output, 0U); appendUint32(output, static_cast<std::uint32_t>(text.size())); appendUint32(output, formats::GlbParser::kJsonChunkType); output.insert(output.end(), text.begin(), text.end()); appendUint32(output, static_cast<std::uint32_t>(buffer.bytes().size())); appendUint32(output, formats::GlbParser::kBinaryChunkType); output.insert(output.end(), buffer.bytes().begin(), buffer.bytes().end());
    if (output.size() > std::numeric_limits<std::uint32_t>::max()) { unsupported("Output GLB exceeds the format limit"); }
    const std::uint32_t length = static_cast<std::uint32_t>(output.size()); for (std::size_t i = 0; i < 4U; ++i) { output[8U + i] = static_cast<std::uint8_t>((length >> (i * 8U)) & 0xFFU); } return output;
}

std::vector<std::uint8_t> buildB3dm(
        const std::vector<std::uint8_t>& glb, const std::string& batch_json,
        const std::optional<std::array<double, 3>>& rtc_center) {
    Json feature_json = {{"BATCH_LENGTH", 1U}};
    if (rtc_center.has_value()) {
        feature_json["RTC_CENTER"] = *rtc_center;
    }
    const std::string feature = feature_json.dump(); std::vector<std::uint8_t> output(formats::B3dmParser::kHeaderSize, 0U); output[0] = 'b'; output[1] = '3'; output[2] = 'd'; output[3] = 'm';
    const std::size_t feature_start = output.size(); output.insert(output.end(), feature.begin(), feature.end()); while (output.size() % 8U != 0U) { output.push_back(' '); } const std::size_t feature_length = output.size() - feature_start;
    const std::size_t batch_start = output.size(); if (!batch_json.empty()) { const std::string normalized = Json::parse(batch_json).dump(); output.insert(output.end(), normalized.begin(), normalized.end()); while (output.size() % 8U != 0U) { output.push_back(' '); } } const std::size_t batch_length = output.size() - batch_start; output.insert(output.end(), glb.begin(), glb.end());
    while (output.size() % formats::B3dmParser::kGlbAlignment != 0U) {
        output.push_back(0U);
    }
    if (output.size() > std::numeric_limits<std::uint32_t>::max()) { unsupported("Output B3DM exceeds the format limit"); }
    auto write = [&output](std::size_t offset, std::uint32_t value) { for (std::size_t i = 0; i < 4U; ++i) { output[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU); } };
    write(4U, 1U); write(8U, static_cast<std::uint32_t>(output.size())); write(12U, static_cast<std::uint32_t>(feature_length)); write(20U, static_cast<std::uint32_t>(batch_length)); return output;
}

}  // namespace

B3dmClipResult B3dmClipper::clip(
        const std::vector<std::uint8_t>& source_bytes,
        const task::ClaimTask& task,
        const SourceLayoutObserver& source_layout_observer,
        const SamplerCompatibilityObserver& sampler_compatibility_observer) {
    try {
        if (!task.clip_options.mask_textures
            || !task.clip_options.compact_feature_metadata
            || task.clip_options.cap_surface) {
            unsupported("Task requests unsupported or unsafe clip options");
        }

        const auto document = formats::B3dmParser::parse(
                formats::ByteView(source_bytes));
        if (source_layout_observer) {
            source_layout_observer(document.layout);
        }
        B3dmClipResult result;
        result.source_layout = document.layout;
        if (document.batch_length != 1U
            || document.header.feature_table_binary_length != 0U
            || document.header.batch_table_binary_length != 0U) {
            unsupported("Only BATCH_LENGTH=1 without binary tables is supported");
        }
        const Json feature_table = Json::parse(document.feature_table_json_text);
        if (!feature_table.is_object()
            || (feature_table.size() != 1U && feature_table.size() != 2U)
            || !feature_table.contains("BATCH_LENGTH")) {
            unsupported("Feature table semantics beyond BATCH_LENGTH/RTC_CENTER are not supported");
        }
        const auto rtc_center = rtcCenter(feature_table);

        GlbSource source(source_bytes, document);
        const auto sampler_diagnostics = inspectSamplerCompatibility(source);
        if (sampler_compatibility_observer
            && sampler_diagnostics.requiresCompatibility()) {
            sampler_compatibility_observer(sampler_diagnostics);
        }
        validateTopLevelDocument(source.json());
        const bool require_unlit = extensionArrayContains(
                source.json(), "extensionsRequired", kUnlitExtension);
        const AuthorizationScope scope = AuthorizationScope::fromBase64Wkb(
                task.scope_wkb_base64, task.scope_srid);
        const Matrix4 tile_transform = Matrix4::fromColumnMajor(task.world_transform);
        const Matrix4 content_transform = rtc_center.has_value()
                ? tile_transform * Matrix4::translation(*rtc_center)
                : tile_transform;
        const SceneTransforms transforms = sceneTransforms(source);
        MaterialCatalog materials(source);
        BufferBuilder output_buffer;

        Json output_meshes = Json::array();
        std::vector<std::optional<std::size_t>> mesh_mapping(
                source.requiredArray("meshes").size());
        std::vector<PrimitiveOutput> outputs;
        std::map<std::size_t, std::vector<MaskUse>> image_uses;
        for (std::size_t mesh_index = 0;
             mesh_index < source.requiredArray("meshes").size(); ++mesh_index) {
            if (!transforms.mesh_transforms[mesh_index].has_value()) {
                continue;
            }
            const auto& mesh = source.requiredArray("meshes").at(mesh_index);
            requireAllowedKeys(mesh, {"primitives"}, "mesh");
            if (!mesh.contains("primitives") || !mesh.at("primitives").is_array()) {
                unsupported("Mesh primitives are missing");
            }
            Json output_primitives = Json::array();
            const Matrix4 world = content_transform
                                  * *transforms.mesh_transforms[mesh_index];
            for (const auto& primitive : mesh.at("primitives")) {
                PrimitiveOutput processed = processPrimitive(
                        source, primitive, world, scope, materials, output_buffer);
                result.statistics.vertex_count_before += processed.input_vertices;
                result.statistics.triangle_count_before += processed.input_triangles;
                result.statistics.vertex_count_after += processed.output_vertices;
                result.statistics.triangle_count_after += processed.output_triangles;
                if (processed.output_triangles > 0U) {
                    output_primitives.push_back(processed.json);
                    for (auto& image : processed.image_uses) {
                        auto& uses = image_uses[image.first];
                        uses.insert(uses.end(), image.second.begin(), image.second.end());
                    }
                    outputs.push_back(std::move(processed));
                }
            }
            if (!output_primitives.empty()) {
                mesh_mapping[mesh_index] = output_meshes.size();
                output_meshes.push_back({{"primitives", std::move(output_primitives)}});
            }
        }
        if (result.statistics.triangle_count_after == 0U) {
            result.empty = true;
            return result;
        }

        std::set<std::size_t> used_materials;
        for (const auto& output : outputs) {
            if (output.source_material.has_value()) {
                used_materials.insert(*output.source_material);
            }
        }
        std::map<std::size_t, std::size_t> material_mapping;
        std::map<std::size_t, std::size_t> texture_mapping;
        std::map<std::size_t, std::size_t> image_mapping;
        std::map<std::size_t, std::size_t> sampler_mapping;
        Json output_materials = Json::array();
        Json output_textures = Json::array();
        Json output_images = Json::array();
        Json output_samplers = Json::array();
        bool uses_unlit = false;
        for (const std::size_t material_index : used_materials) {
            MaterialInfo material = materials.material(material_index);
            uses_unlit = uses_unlit || material.unlit;
            if (material.texture.has_value()) {
                const std::size_t texture_index = *material.texture;
                if (texture_mapping.count(texture_index) == 0U) {
                    const TextureInfo& texture = materials.texture(texture_index);
                    if (image_mapping.count(texture.image) == 0U) {
                        const auto original = materials.imageBytes(texture.image);
                        result.statistics.texture_bytes_before += original.size();
                        const auto masked = maskWebp(original,
                                                     image_uses.at(texture.image));
                        result.statistics.texture_bytes_after += masked.size();
                        const std::size_t view = output_buffer.addBytes(masked);
                        image_mapping[texture.image] = output_images.size();
                        output_images.push_back({{"bufferView", view},
                                                 {"mimeType", "image/webp"}});
                    }
                    Json output_texture = {
                            {"extensions",
                             {{kWebpExtension,
                               {{"source", image_mapping.at(texture.image)}}}}}};
                    if (texture.sampler.has_value()) {
                        if (sampler_mapping.count(*texture.sampler) == 0U) {
                            sampler_mapping[*texture.sampler] = output_samplers.size();
                            output_samplers.push_back(
                                    materials.sampler(texture.sampler).output);
                        }
                        output_texture["sampler"] = sampler_mapping.at(*texture.sampler);
                    }
                    texture_mapping[texture_index] = output_textures.size();
                    output_textures.push_back(std::move(output_texture));
                }
                material.output["pbrMetallicRoughness"]["baseColorTexture"]["index"] =
                        texture_mapping.at(texture_index);
            }
            material_mapping[material_index] = output_materials.size();
            output_materials.push_back(std::move(material.output));
        }

        std::size_t output_primitive_index = 0U;
        for (auto& mesh : output_meshes) {
            for (auto& primitive : mesh.at("primitives")) {
                const auto& processed = outputs.at(output_primitive_index++);
                if (processed.source_material.has_value()) {
                    primitive["material"] =
                            material_mapping.at(*processed.source_material);
                }
            }
        }

        const SanitizedScene scene = sanitizeActiveScene(source, mesh_mapping);
        const bool uses_webp = !output_textures.empty();
        Json output_json;
        output_json["asset"] = {{"version", "2.0"},
                                {"generator", "3d-tiles-clip-worker"}};
        output_json["scene"] = 0U;
        output_json["scenes"] = scene.scenes;
        output_json["nodes"] = scene.nodes;
        output_json["meshes"] = std::move(output_meshes);
        if (!output_materials.empty()) {
            output_json["materials"] = std::move(output_materials);
        }
        if (uses_webp) {
            output_json["textures"] = std::move(output_textures);
            output_json["images"] = std::move(output_images);
        }
        if (!output_samplers.empty()) {
            output_json["samplers"] = std::move(output_samplers);
        }

        Json extensions_used = Json::array();
        Json extensions_required = Json::array();
        if (uses_unlit) {
            extensions_used.push_back(kUnlitExtension);
            if (require_unlit) {
                extensions_required.push_back(kUnlitExtension);
            }
        }
        if (uses_webp) {
            extensions_used.push_back(kWebpExtension);
            extensions_required.push_back(kWebpExtension);
        }
        if (!extensions_used.empty()) {
            output_json["extensionsUsed"] = std::move(extensions_used);
        }
        if (!extensions_required.empty()) {
            output_json["extensionsRequired"] = std::move(extensions_required);
        }

        result.bytes = buildB3dm(
                buildGlb(std::move(output_json), output_buffer),
                sanitizeBatchTable(document.batch_table_json_text), rtc_center);
        const auto verified = formats::B3dmParser::parse(
                formats::ByteView(result.bytes));
        if (verified.layout.requiresCompatibility()) {
            throw FormatError(FormatErrorCode::invalid_alignment,
                              "Output B3DM is not aligned to an eight-byte boundary");
        }
        GlbSource verified_source(result.bytes, verified);
        validateTopLevelDocument(verified_source.json());
        return result;
    } catch (const FormatError&) {
        throw;
    } catch (const Json::exception& error) {
        throw FormatError(FormatErrorCode::invalid_json,
                          std::string("Invalid glTF JSON structure: ") + error.what());
    } catch (const std::out_of_range& error) {
        throw FormatError(FormatErrorCode::invalid_accessor,
                          std::string("glTF reference is out of range: ") + error.what());
    }
}

}  // namespace clip_worker::clip
