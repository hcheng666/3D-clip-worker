#include "clip_worker/formats/gltf_mesh_reader.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/glb.hpp"
#include "clip_worker/metadata/structural_metadata_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace clip_worker::formats {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kComponentByte = 5120U;
constexpr std::uint32_t kComponentUnsignedByte = 5121U;
constexpr std::uint32_t kComponentShort = 5122U;
constexpr std::uint32_t kComponentUnsignedShort = 5123U;
constexpr std::uint32_t kComponentUnsignedInt = 5125U;
constexpr std::uint32_t kComponentFloat = 5126U;
constexpr std::uint32_t kTrianglesMode = 4U;
constexpr std::uint32_t kTriangleStripMode = 5U;
constexpr const char* kDracoExtension = "KHR_draco_mesh_compression";
constexpr const char* kMeshoptExtension = "EXT_meshopt_compression";
constexpr const char* kWebpExtension = "EXT_texture_webp";
constexpr const char* kBasisExtension = "KHR_texture_basisu";
constexpr const char* kUnlitExtension = "KHR_materials_unlit";
constexpr const char* kStructuralMetadataExtension =
        "EXT_structural_metadata";
constexpr const char* kMeshFeaturesExtension = "EXT_mesh_features";
constexpr const char* kGpuInstancingExtension = "EXT_mesh_gpu_instancing";
constexpr const char* kInstanceFeaturesExtension = "EXT_instance_features";
constexpr const char* kCanonicalLegacyHierarchyExtension =
        "JUSTAI_legacy_batch_table_hierarchy";
constexpr double kMaximumExactFloatFeatureId = 16777216.0;

const std::set<std::string> kSupportedExtensions = {
        kBasisExtension,
        kDracoExtension,
        kMeshoptExtension,
        kUnlitExtension,
        kWebpExtension,
};

[[noreturn]] void invalid(const std::string& message) {
    throw FormatError(FormatErrorCode::invalid_accessor, message);
}

[[noreturn]] void unsupported(const std::string& message) {
    throw FormatError(FormatErrorCode::unsupported_content, message);
}

std::size_t unsignedSize(const Json& object, const char* field,
                         std::optional<std::size_t> fallback = std::nullopt) {
    const auto item = object.find(field);
    if (item == object.end()) {
        if (fallback.has_value()) return *fallback;
        invalid(std::string("glTF integer field is missing: ") + field);
    }
    if (!item->is_number_unsigned()) {
        invalid(std::string("glTF integer field is invalid: ") + field);
    }
    const std::uint64_t value = item->get<std::uint64_t>();
    if (value > std::numeric_limits<std::size_t>::max()) {
        invalid(std::string("glTF integer field exceeds size_t: ") + field);
    }
    return static_cast<std::size_t>(value);
}

const Json& arrayField(const Json& root, const char* field, bool required) {
    static const Json empty = Json::array();
    const auto item = root.find(field);
    if (item == root.end()) {
        if (!required) return empty;
        unsupported(std::string("glTF array is missing: ") + field);
    }
    if (!item->is_array()) {
        invalid(std::string("glTF field must be an array: ") + field);
    }
    return *item;
}

const Json& indexed(const Json& array, std::size_t index, const char* kind) {
    if (index >= array.size() || !array.at(index).is_object()) {
        invalid(std::string("glTF ") + kind + " index is out of range");
    }
    return array.at(index);
}

void allowedKeys(const Json& object, const std::set<std::string>& keys,
                 const char* description) {
    if (!object.is_object()) {
        invalid(std::string(description) + " must be an object");
    }
    for (const auto& item : object.items()) {
        if (keys.count(item.key()) == 0U) {
            unsupported(std::string(description) + " contains unsupported field: "
                        + item.key());
        }
    }
}

bool featureIdSemantic(const std::string& name) {
    constexpr const char* kPrefix = "_FEATURE_ID_";
    if (name.rfind(kPrefix, 0U) != 0U || name.size() <= 12U) return false;
    return std::all_of(name.begin() + 12, name.end(), [](unsigned char value) {
        return value >= '0' && value <= '9';
    });
}

std::size_t componentSize(std::uint32_t type) {
    switch (type) {
        case kComponentByte:
        case kComponentUnsignedByte: return 1U;
        case kComponentShort:
        case kComponentUnsignedShort: return 2U;
        case kComponentUnsignedInt:
        case kComponentFloat: return 4U;
        default: unsupported("Accessor componentType is unsupported");
    }
}

std::size_t componentCount(const std::string& type) {
    if (type == "SCALAR") return 1U;
    if (type == "VEC2") return 2U;
    if (type == "VEC3") return 3U;
    if (type == "VEC4") return 4U;
    unsupported("Accessor type is unsupported for a mesh attribute");
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string parentDirectory(const std::string& path) {
    const std::size_t separator = path.rfind('/');
    return separator == std::string::npos ? std::string()
                                           : path.substr(0U, separator + 1U);
}

std::string resolveCanonicalPath(const std::string& root_path,
                                 const std::string& uri) {
    if (uri.empty() || uri.front() == '/' || uri.rfind("//", 0U) == 0U
        || uri.find('\\') != std::string::npos
        || uri.find(':') != std::string::npos
        || uri.find('?') != std::string::npos
        || uri.find('#') != std::string::npos) {
        unsupported("Resource URI is outside the approved package namespace");
    }
    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t index = 0U; index < uri.size(); ++index) {
        const unsigned char current = static_cast<unsigned char>(uri[index]);
        if (current < 0x20U || current == 0x7fU) {
            unsupported("Resource URI contains a control character");
        }
        if (uri[index] != '%') {
            decoded.push_back(uri[index]);
            continue;
        }
        if (uri.size() - index < 3U) {
            unsupported("Resource URI percent escape is truncated");
        }
        const int high = hexValue(uri[index + 1U]);
        const int low = hexValue(uri[index + 2U]);
        if (high < 0 || low < 0) {
            unsupported("Resource URI percent escape is invalid");
        }
        const char byte = static_cast<char>((high << 4) | low);
        if (byte == '\0' || byte == '/' || byte == '\\') {
            unsupported("Resource URI encodes a forbidden separator");
        }
        // Approved package paths are canonical, so aliases remain fail closed.
        unsupported("Percent-encoded resource URI aliases are unsupported");
    }
    const std::string candidate = parentDirectory(root_path) + decoded;
    std::size_t start = 0U;
    while (start <= candidate.size()) {
        const std::size_t end = candidate.find('/', start);
        const std::string segment = candidate.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            unsupported("Resource URI does not resolve to a canonical package path");
        }
        if (end == std::string::npos) break;
        start = end + 1U;
    }
    return candidate;
}

struct ResolvedResource {
    std::vector<std::uint8_t> bytes;
    std::string media_type;
    bool data_uri = false;
};

std::vector<std::uint8_t> decodePercentData(
        const std::string& value, std::uint64_t maximum_bytes) {
    std::vector<std::uint8_t> result;
    result.reserve(std::min<std::uint64_t>(value.size(), maximum_bytes));
    for (std::size_t index = 0U; index < value.size(); ++index) {
        std::uint8_t byte = static_cast<std::uint8_t>(value[index]);
        if (value[index] == '%') {
            if (value.size() - index < 3U) invalid("Data URI escape is truncated");
            const int high = hexValue(value[index + 1U]);
            const int low = hexValue(value[index + 2U]);
            if (high < 0 || low < 0) invalid("Data URI escape is invalid");
            byte = static_cast<std::uint8_t>((high << 4) | low);
            index += 2U;
        }
        if (result.size() >= maximum_bytes) {
            unsupported("Data URI exceeds the configured byte limit");
        }
        result.push_back(byte);
    }
    return result;
}

std::vector<std::uint8_t> decodeBase64Data(
        const std::string& value, std::uint64_t maximum_bytes) {
    if (value.empty() || value.size() % 4U != 0U
        || std::any_of(value.begin(), value.end(), [](unsigned char byte) {
               return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
           })) {
        invalid("Data URI base64 is not canonical");
    }
    const std::uint64_t upper = value.size() / 4U * 3U;
    if (upper > maximum_bytes || upper > std::numeric_limits<int>::max()) {
        unsupported("Data URI exceeds the configured byte limit");
    }
    std::vector<std::uint8_t> result(static_cast<std::size_t>(upper));
    const int decoded = EVP_DecodeBlock(
            result.data(),
            reinterpret_cast<const unsigned char*>(value.data()),
            static_cast<int>(value.size()));
    if (decoded < 0) invalid("Data URI base64 is invalid");
    std::size_t length = static_cast<std::size_t>(decoded);
    if (!value.empty() && value.back() == '=') --length;
    if (value.size() > 1U && value[value.size() - 2U] == '=') --length;
    result.resize(length);
    return result;
}

ResolvedResource resolveResource(
        const std::string& root_path, const std::string& uri,
        const ApprovedGltfResourceMap& resources,
        const GltfMeshReaderLimits& limits,
        GltfMeshReadDiagnostics& diagnostics,
        std::set<std::string>& observed_external) {
    if (uri.rfind("data:", 0U) == 0U) {
        const std::size_t comma = uri.find(',');
        if (comma == std::string::npos) invalid("Data URI has no payload delimiter");
        std::string declaration = uri.substr(5U, comma - 5U);
        bool base64 = false;
        constexpr const char* marker = ";base64";
        if (declaration.size() >= 7U
            && declaration.compare(declaration.size() - 7U, 7U, marker) == 0) {
            declaration.resize(declaration.size() - 7U);
            base64 = true;
        }
        if (declaration.find(';') != std::string::npos) {
            unsupported("Data URI parameters are unsupported");
        }
        ++diagnostics.data_uri_count;
        return {base64
                        ? decodeBase64Data(uri.substr(comma + 1U),
                                           limits.maximum_data_uri_bytes)
                        : decodePercentData(uri.substr(comma + 1U),
                                            limits.maximum_data_uri_bytes),
                declaration.empty() ? "application/octet-stream" : declaration,
                true};
    }
    const std::string path = resolveCanonicalPath(root_path, uri);
    const auto resource = resources.find(path);
    if (resource == resources.end()) {
        unsupported("Resource URI is absent from the approved closure");
    }
    if (observed_external.insert(path).second) {
        ++diagnostics.external_resource_count;
    }
    return {resource->second.bytes, resource->second.media_type, false};
}

std::string inferImageMimeType(const std::string& uri) {
    std::string lower = uri;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (lower.size() >= 4U && lower.compare(lower.size() - 4U, 4U, ".png") == 0)
        return "image/png";
    if ((lower.size() >= 4U && lower.compare(lower.size() - 4U, 4U, ".jpg") == 0)
        || (lower.size() >= 5U
            && lower.compare(lower.size() - 5U, 5U, ".jpeg") == 0))
        return "image/jpeg";
    if (lower.size() >= 5U && lower.compare(lower.size() - 5U, 5U, ".webp") == 0)
        return "image/webp";
    if (lower.size() >= 5U && lower.compare(lower.size() - 5U, 5U, ".ktx2") == 0)
        return "image/ktx2";
    unsupported("External image URI has no supported media type");
}

struct DecodedView {
    std::vector<std::uint8_t> bytes;
    std::size_t byte_stride = 0U;
    std::size_t absolute_offset = 0U;
    bool meshopt = false;
};

struct AccessorInfo {
    std::optional<std::size_t> view;
    std::size_t byte_offset = 0U;
    std::size_t count = 0U;
    std::uint32_t component_type = 0U;
    std::size_t components = 0U;
    std::string type;
    bool normalized = false;
    const Json* sparse = nullptr;
};

class DocumentReader final {
public:
    DocumentReader(Json root, std::vector<std::uint8_t> glb_binary,
                   std::string root_path,
                   const ApprovedGltfResourceMap& resources,
                   const GltfMeshReaderLimits& limits)
        : root_(std::move(root)), glb_binary_(std::move(glb_binary)),
          root_path_(std::move(root_path)), resources_(resources), limits_(limits),
          accountant_(limits.mesh) {}

    GltfMeshReadResult read() {
        validateRoot();
        scene_.require_unlit = requiredExtension(kUnlitExtension);
        loadBuffers();
        loadViews();
        loadStructuralMetadata();
        loadAccessors();
        loadSamplers();
        loadImages();
        loadTextures();
        loadMaterials();
        loadMeshes();
        loadNodesAndScene();
        mesh::validateMeshScene(scene_);
        return {std::move(scene_), diagnostics_};
    }

private:
    bool requiredExtension(const char* name) const {
        const auto extensions = root_.find("extensionsRequired");
        return extensions != root_.end()
                && std::find(extensions->begin(), extensions->end(), name)
                        != extensions->end();
    }

    void validateRoot() const {
        allowedKeys(root_, {"accessors", "asset", "bufferViews", "buffers",
                            "extensions",
                            "extensionsRequired", "extensionsUsed", "images",
                            "materials", "meshes", "nodes", "samplers", "scene",
                            "scenes", "textures"}, "glTF document");
        const auto asset = root_.find("asset");
        if (asset == root_.end() || !asset->is_object()
            || asset->value("version", std::string()) != "2.0") {
            unsupported("glTF asset.version must be 2.0");
        }
        for (const char* field : {"extensionsUsed", "extensionsRequired"}) {
            const auto extensions = root_.find(field);
            if (extensions == root_.end()) continue;
            if (!extensions->is_array()) invalid("glTF extension list is invalid");
            for (const auto& extension : *extensions) {
                if (!extension.is_string()) {
                    invalid("glTF extension name is invalid");
                }
                const std::string name = extension.get<std::string>();
                const bool metadata_extension =
                        name == kStructuralMetadataExtension
                        || name == kMeshFeaturesExtension
                        || name == kCanonicalLegacyHierarchyExtension;
                if (metadata_extension && limits_.enable_feature_metadata) {
                    continue;
                }
                const bool instance_extension =
                        name == kGpuInstancingExtension
                        || name == kInstanceFeaturesExtension;
                if (instance_extension && limits_.enable_gpu_instancing
                        && (name != kInstanceFeaturesExtension
                            || limits_.enable_feature_metadata)) {
                    continue;
                }
                if (kSupportedExtensions.count(name) == 0U) {
                    if (limits_.enable_feature_metadata
                            && field == std::string("extensionsRequired")) {
                        throw FormatError(
                                FormatErrorCode::metadata_unknown_required_extension,
                                "glTF declares an unknown required metadata extension");
                    }
                    throw FormatError(
                            FormatErrorCode::content_gltf_feature_unsupported,
                            "glTF declares an unsupported extension");
                }
            }
        }
        const auto root_extensions = root_.find("extensions");
        if (root_extensions != root_.end()) {
            if (!root_extensions->is_object()) {
                invalid("glTF root extensions are invalid");
            }
            for (const auto& extension : root_extensions->items()) {
                if (extension.key() != kStructuralMetadataExtension
                        && !(limits_.enable_feature_metadata
                             && extension.key()
                                    == kCanonicalLegacyHierarchyExtension)) {
                    throw FormatError(
                            limits_.enable_feature_metadata
                                    ? FormatErrorCode::metadata_unknown_required_extension
                                    : FormatErrorCode::content_gltf_feature_unsupported,
                            "glTF root metadata extension is unsupported");
                }
            }
        }
    }

    void loadStructuralMetadata() {
        const auto extensions = root_.find("extensions");
        if (extensions == root_.end()
                || !extensions->contains(kStructuralMetadataExtension)) {
            return;
        }
        if (!limits_.enable_feature_metadata) {
            throw FormatError(
                    FormatErrorCode::content_gltf_feature_unsupported,
                    "Structural metadata requires the metadata contract");
        }
        std::optional<std::string> external_schema;
        const Json& extension = extensions->at(kStructuralMetadataExtension);
        if (extension.is_object() && extension.contains("schemaUri")) {
            if (!extension.at("schemaUri").is_string()) {
                invalid("Structural metadata schemaUri is invalid");
            }
            ResolvedResource resource = resolveResource(
                    root_path_, extension.at("schemaUri").get<std::string>(),
                    resources_, limits_, diagnostics_, observed_external_);
            if (resource.bytes.size() > limits_.metadata.maximum_schema_bytes) {
                throw FormatError(
                        FormatErrorCode::metadata_reconstruction_unsafe,
                        "Structural metadata schema exceeds the configured limit");
            }
            external_schema = std::string(
                    reinterpret_cast<const char*>(resource.bytes.data()),
                    resource.bytes.size());
        }
        std::vector<metadata::MetadataBufferView> metadata_views;
        metadata_views.reserve(views_.size());
        for (const auto& source : views_) {
            metadata_views.push_back({&source.bytes, source.absolute_offset});
        }
        scene_.feature_metadata = metadata::readStructuralMetadata(
                root_.dump(), metadata_views, limits_.metadata,
                external_schema);
    }

    void loadBuffers() {
        const Json& buffers = arrayField(root_, "buffers", true);
        if (buffers.empty() || buffers.size() > limits_.maximum_buffers) {
            unsupported("glTF buffer count exceeds the configured limit");
        }
        buffers_.reserve(buffers.size());
        for (std::size_t index = 0U; index < buffers.size(); ++index) {
            const Json& buffer = indexed(buffers, index, "buffer");
            allowedKeys(buffer, {"byteLength", "name", "uri"}, "glTF buffer");
            const std::size_t length = unsignedSize(buffer, "byteLength");
            std::vector<std::uint8_t> bytes;
            if (!buffer.contains("uri")) {
                if (index != 0U || glb_binary_.empty()) {
                    invalid("Only GLB buffer zero may omit uri");
                }
                if (length > glb_binary_.size()
                    || glb_binary_.size() - length > 3U) {
                    invalid("GLB BIN length differs from buffer.byteLength");
                }
                bytes.assign(glb_binary_.begin(), glb_binary_.begin()
                             + static_cast<std::ptrdiff_t>(length));
            } else {
                if (!buffer.at("uri").is_string()) invalid("Buffer uri is invalid");
                ResolvedResource resource = resolveResource(
                        root_path_, buffer.at("uri").get<std::string>(), resources_,
                        limits_, diagnostics_, observed_external_);
                if (length > resource.bytes.size()) {
                    invalid("External buffer is shorter than buffer.byteLength");
                }
                bytes.assign(resource.bytes.begin(), resource.bytes.begin()
                             + static_cast<std::ptrdiff_t>(length));
            }
            accountant_.reserveDecodedBytes(bytes.size());
            buffers_.push_back(std::move(bytes));
        }
    }

    static MeshoptMode meshoptMode(const std::string& value) {
        if (value == "ATTRIBUTES") return MeshoptMode::attributes;
        if (value == "TRIANGLES") return MeshoptMode::triangles;
        if (value == "INDICES") return MeshoptMode::indices;
        unsupported("Meshopt mode is unsupported");
    }

    static MeshoptFilter meshoptFilter(const std::string& value) {
        if (value == "NONE") return MeshoptFilter::none;
        if (value == "OCTAHEDRAL") return MeshoptFilter::octahedral;
        if (value == "QUATERNION") return MeshoptFilter::quaternion;
        if (value == "EXPONENTIAL") return MeshoptFilter::exponential;
        unsupported("Meshopt filter is unsupported");
    }

    std::vector<std::uint8_t> bufferSlice(std::size_t buffer_index,
                                          std::size_t offset,
                                          std::size_t length) const {
        if (buffer_index >= buffers_.size() || offset > buffers_[buffer_index].size()
            || length > buffers_[buffer_index].size() - offset) {
            invalid("BufferView byte range is out of range");
        }
        return std::vector<std::uint8_t>(
                buffers_[buffer_index].begin() + static_cast<std::ptrdiff_t>(offset),
                buffers_[buffer_index].begin()
                        + static_cast<std::ptrdiff_t>(offset + length));
    }

    void loadViews() {
        const Json& views = arrayField(root_, "bufferViews", true);
        if (views.size() > limits_.maximum_buffer_views) {
            unsupported("glTF bufferView count exceeds the configured limit");
        }
        views_.reserve(views.size());
        for (const auto& view : views) {
            allowedKeys(view, {"buffer", "byteLength", "byteOffset", "byteStride",
                               "extensions", "name", "target"},
                        "glTF bufferView");
            DecodedView output;
            const auto extensions = view.find("extensions");
            if (extensions != view.end()) {
                allowedKeys(*extensions, {kMeshoptExtension},
                            "bufferView extensions");
            }
            if (extensions != view.end() && extensions->contains(kMeshoptExtension)) {
                const Json& extension = extensions->at(kMeshoptExtension);
                allowedKeys(extension, {"buffer", "byteLength", "byteOffset",
                                        "byteStride", "count", "filter", "mode"},
                            "EXT_meshopt_compression");
                const std::size_t buffer_index = unsignedSize(extension, "buffer");
                const std::size_t offset = unsignedSize(extension, "byteOffset", 0U);
                const std::size_t length = unsignedSize(extension, "byteLength");
                const std::size_t count = unsignedSize(extension, "count");
                const std::size_t stride = unsignedSize(extension, "byteStride");
                if (!extension.at("mode").is_string()
                    || (extension.contains("filter")
                        && !extension.at("filter").is_string())) {
                    invalid("Meshopt mode or filter is invalid");
                }
                const auto compressed = bufferSlice(buffer_index, offset, length);
                output.bytes = MeshoptDecoder::decode(
                        ByteView(compressed), count, stride,
                        meshoptMode(extension.at("mode").get<std::string>()),
                        meshoptFilter(extension.value("filter", std::string("NONE"))),
                        limits_.meshopt);
                output.byte_stride = stride;
                output.meshopt = true;
                ++diagnostics_.meshopt_buffer_view_count;
            } else {
                const std::size_t buffer_index = unsignedSize(view, "buffer");
                const std::size_t offset = unsignedSize(view, "byteOffset", 0U);
                const std::size_t length = unsignedSize(view, "byteLength");
                output.bytes = bufferSlice(buffer_index, offset, length);
                output.byte_stride = unsignedSize(view, "byteStride", 0U);
                output.absolute_offset = offset;
            }
            accountant_.reserveDecodedBytes(output.bytes.size());
            views_.push_back(std::move(output));
        }
    }

    AccessorInfo parseAccessor(const Json& accessor) const {
        allowedKeys(accessor, {"bufferView", "byteOffset", "componentType", "count",
                               "max", "min", "name", "normalized", "sparse", "type"},
                    "glTF accessor");
        AccessorInfo result;
        if (accessor.contains("bufferView")) {
            result.view = unsignedSize(accessor, "bufferView");
            if (*result.view >= views_.size()) invalid("Accessor bufferView is out of range");
        }
        result.byte_offset = unsignedSize(accessor, "byteOffset", 0U);
        result.count = unsignedSize(accessor, "count");
        if (result.count == 0U || result.count > limits_.mesh.maximum_vertices) {
            unsupported("Accessor count exceeds the configured limit");
        }
        const std::size_t component_type = unsignedSize(accessor, "componentType");
        if (component_type > std::numeric_limits<std::uint32_t>::max()) {
            invalid("Accessor componentType exceeds uint32");
        }
        result.component_type = static_cast<std::uint32_t>(component_type);
        static_cast<void>(componentSize(result.component_type));
        if (!accessor.contains("type") || !accessor.at("type").is_string()) {
            invalid("Accessor type is invalid");
        }
        result.type = accessor.at("type").get<std::string>();
        result.components = componentCount(result.type);
        result.normalized = accessor.value("normalized", false);
        if (result.normalized && result.component_type == kComponentFloat) {
            invalid("Float accessor cannot be normalized");
        }
        if (accessor.contains("sparse")) result.sparse = &accessor.at("sparse");
        if (!result.view.has_value() && result.sparse == nullptr) {
            invalid("Accessor has neither bufferView nor sparse data");
        }
        return result;
    }

    void loadAccessors() {
        const Json& accessors = arrayField(root_, "accessors", true);
        if (accessors.empty() || accessors.size() > limits_.maximum_accessors) {
            unsupported("glTF accessor count exceeds the configured limit");
        }
        accessors_.reserve(accessors.size());
        for (const auto& accessor : accessors) {
            accessors_.push_back(parseAccessor(accessor));
        }
    }

    static std::int64_t signedComponent(const std::uint8_t* source,
                                        std::uint32_t type) {
        if (type == kComponentByte) {
            std::int8_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }
        if (type == kComponentShort) {
            std::int16_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }
        invalid("Accessor component is not signed integer");
    }

    static std::uint64_t unsignedComponent(const std::uint8_t* source,
                                           std::uint32_t type) {
        if (type == kComponentUnsignedByte) return source[0U];
        if (type == kComponentUnsignedShort) {
            std::uint16_t value = 0U;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }
        if (type == kComponentUnsignedInt) {
            std::uint32_t value = 0U;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }
        invalid("Accessor component is not unsigned integer");
    }

    static double componentValue(const std::uint8_t* source,
                                 std::uint32_t type, bool normalized) {
        if (type == kComponentFloat) {
            float value = 0.0F;
            std::memcpy(&value, source, sizeof(value));
            if (!std::isfinite(value)) invalid("Accessor contains a non-finite float");
            return value;
        }
        if (type == kComponentByte || type == kComponentShort) {
            const std::int64_t value = signedComponent(source, type);
            if (!normalized) return static_cast<double>(value);
            const double maximum = type == kComponentByte ? 127.0 : 32767.0;
            return std::max(-1.0, static_cast<double>(value) / maximum);
        }
        const std::uint64_t value = unsignedComponent(source, type);
        if (!normalized) return static_cast<double>(value);
        const double maximum = type == kComponentUnsignedByte ? 255.0
                : type == kComponentUnsignedShort ? 65535.0
                                                  : 4294967295.0;
        return static_cast<double>(value) / maximum;
    }

    const std::uint8_t* elementPointer(const AccessorInfo& accessor,
                                       std::size_t element,
                                       std::size_t expected_components) const {
        if (!accessor.view.has_value()) invalid("Sparse-only accessor has no base pointer");
        const DecodedView& view = views_.at(*accessor.view);
        const std::size_t element_size = componentSize(accessor.component_type)
                                         * expected_components;
        const std::size_t stride = view.byte_stride == 0U
                ? element_size : view.byte_stride;
        if (stride < element_size
            || accessor.byte_offset > view.bytes.size()
            || element > (std::numeric_limits<std::size_t>::max()
                           - accessor.byte_offset) / stride) {
            invalid("Accessor stride or byte offset is invalid");
        }
        const std::size_t offset = accessor.byte_offset + element * stride;
        if (offset > view.bytes.size() || element_size > view.bytes.size() - offset
            || (view.absolute_offset + offset)
                    % componentSize(accessor.component_type) != 0U) {
            invalid("Accessor byte range or alignment is invalid");
        }
        return view.bytes.data() + offset;
    }

    std::vector<std::uint32_t> sparseIndices(const Json& sparse,
                                             std::size_t sparse_count,
                                             std::size_t accessor_count) const {
        allowedKeys(sparse, {"count", "indices", "values"}, "sparse accessor");
        const Json& indices = sparse.at("indices");
        allowedKeys(indices, {"bufferView", "byteOffset", "componentType"},
                    "sparse indices");
        const std::size_t view_index = unsignedSize(indices, "bufferView");
        if (view_index >= views_.size()) invalid("Sparse index bufferView is out of range");
        const std::size_t offset = unsignedSize(indices, "byteOffset", 0U);
        const std::size_t raw_type = unsignedSize(indices, "componentType");
        if (raw_type != kComponentUnsignedByte
            && raw_type != kComponentUnsignedShort
            && raw_type != kComponentUnsignedInt) {
            invalid("Sparse index componentType is invalid");
        }
        const auto type = static_cast<std::uint32_t>(raw_type);
        const std::size_t size = componentSize(type);
        const DecodedView& view = views_.at(view_index);
        if (offset > view.bytes.size()
            || sparse_count > (view.bytes.size() - offset) / size) {
            invalid("Sparse index byte range is invalid");
        }
        std::vector<std::uint32_t> result(sparse_count);
        std::uint32_t previous = 0U;
        for (std::size_t index = 0U; index < sparse_count; ++index) {
            const std::uint64_t value = unsignedComponent(
                    view.bytes.data() + offset + index * size, type);
            if (value >= accessor_count
                || (index > 0U && value <= previous)) {
                invalid("Sparse indices are duplicated, unsorted, or out of range");
            }
            result[index] = static_cast<std::uint32_t>(value);
            previous = result[index];
        }
        return result;
    }

    std::vector<double> numericAccessor(std::size_t accessor_index) const {
        if (accessor_index >= accessors_.size()) invalid("Accessor index is out of range");
        const AccessorInfo& accessor = accessors_.at(accessor_index);
        if (accessor.count > std::numeric_limits<std::size_t>::max()
                             / accessor.components) {
            invalid("Accessor value count overflows");
        }
        std::vector<double> result(accessor.count * accessor.components, 0.0);
        if (accessor.view.has_value()) {
            for (std::size_t element = 0U; element < accessor.count; ++element) {
                const std::uint8_t* source = elementPointer(
                        accessor, element, accessor.components);
                for (std::size_t component = 0U;
                     component < accessor.components; ++component) {
                    result[element * accessor.components + component] =
                            componentValue(source + component
                                    * componentSize(accessor.component_type),
                                           accessor.component_type,
                                           accessor.normalized);
                }
            }
        }
        if (accessor.sparse != nullptr) {
            const Json& sparse = *accessor.sparse;
            const std::size_t count = unsignedSize(sparse, "count");
            if (count == 0U || count > accessor.count) {
                invalid("Sparse accessor count is invalid");
            }
            const auto indices = sparseIndices(sparse, count, accessor.count);
            const Json& values = sparse.at("values");
            allowedKeys(values, {"bufferView", "byteOffset"}, "sparse values");
            const std::size_t view_index = unsignedSize(values, "bufferView");
            if (view_index >= views_.size()) invalid("Sparse value bufferView is out of range");
            const std::size_t offset = unsignedSize(values, "byteOffset", 0U);
            const std::size_t component_size = componentSize(accessor.component_type);
            const std::size_t element_size = component_size * accessor.components;
            const DecodedView& view = views_.at(view_index);
            if (offset > view.bytes.size()
                || count > (view.bytes.size() - offset) / element_size) {
                invalid("Sparse value byte range is invalid");
            }
            for (std::size_t sparse_index = 0U; sparse_index < count;
                 ++sparse_index) {
                const std::uint8_t* source = view.bytes.data() + offset
                        + sparse_index * element_size;
                const std::size_t target = indices[sparse_index]
                                           * accessor.components;
                for (std::size_t component = 0U;
                     component < accessor.components; ++component) {
                    result[target + component] = componentValue(
                            source + component * component_size,
                            accessor.component_type, accessor.normalized);
                }
            }
        }
        return result;
    }

    std::vector<float> floatAttribute(std::size_t accessor_index,
                                      const char* semantic,
                                      std::size_t components,
                                      const std::set<std::uint32_t>& types,
                                      bool require_normalized_integer) const {
        const AccessorInfo& accessor = accessors_.at(accessor_index);
        if (accessor.components != components
            || types.count(accessor.component_type) == 0U
            || (accessor.component_type != kComponentFloat
                && require_normalized_integer && !accessor.normalized)) {
            invalid(std::string(semantic) + " accessor type is invalid");
        }
        const auto values = numericAccessor(accessor_index);
        std::vector<float> result;
        result.reserve(values.size());
        for (const double value : values) {
            if (!std::isfinite(value)
                || value < -std::numeric_limits<float>::max()
                || value > std::numeric_limits<float>::max()) {
                invalid(std::string(semantic) + " value is outside float range");
            }
            result.push_back(static_cast<float>(value));
        }
        return result;
    }

    std::vector<std::uint32_t> unsignedAccessor(
            std::size_t accessor_index, bool allow_integral_float) const {
        const AccessorInfo& accessor = accessors_.at(accessor_index);
        if (accessor.components != 1U || accessor.normalized
            || (accessor.component_type != kComponentUnsignedByte
                && accessor.component_type != kComponentUnsignedShort
                && accessor.component_type != kComponentUnsignedInt
                && !(allow_integral_float
                     && accessor.component_type == kComponentFloat))) {
            invalid("Unsigned scalar accessor type is invalid");
        }
        const auto values = numericAccessor(accessor_index);
        std::vector<std::uint32_t> result;
        result.reserve(values.size());
        for (const double value : values) {
            if (value < 0.0 || value > std::numeric_limits<std::uint32_t>::max()
                || (accessor.component_type == kComponentFloat
                    && value > kMaximumExactFloatFeatureId)
                || std::floor(value) != value) {
                invalid("Unsigned scalar accessor contains a non-integral value");
            }
            result.push_back(static_cast<std::uint32_t>(value));
        }
        return result;
    }

    static mesh::SamplerWrap samplerWrap(std::uint32_t value) {
        if (value == static_cast<std::uint32_t>(mesh::SamplerWrap::repeat))
            return mesh::SamplerWrap::repeat;
        if (value == static_cast<std::uint32_t>(mesh::SamplerWrap::clamp_to_edge))
            return mesh::SamplerWrap::clamp_to_edge;
        if (value == static_cast<std::uint32_t>(mesh::SamplerWrap::mirrored_repeat))
            return mesh::SamplerWrap::mirrored_repeat;
        unsupported("Sampler wrap mode is unsupported");
    }

    void loadSamplers() {
        const Json& samplers = arrayField(root_, "samplers", false);
        scene_.samplers.reserve(samplers.size());
        for (const auto& sampler : samplers) {
            allowedKeys(sampler, {"magFilter", "minFilter", "name", "wrapS", "wrapT"},
                        "glTF sampler");
            mesh::MeshSampler output;
            output.wrap_s = samplerWrap(static_cast<std::uint32_t>(
                    unsignedSize(sampler, "wrapS", 10497U)));
            output.wrap_t = samplerWrap(static_cast<std::uint32_t>(
                    unsignedSize(sampler, "wrapT", 10497U)));
            if (sampler.contains("magFilter")) {
                output.mag_filter = static_cast<std::uint32_t>(
                        unsignedSize(sampler, "magFilter"));
                if (*output.mag_filter != 9728U && *output.mag_filter != 9729U)
                    unsupported("Sampler magFilter is unsupported");
            }
            if (sampler.contains("minFilter")) {
                output.min_filter = static_cast<std::uint32_t>(
                        unsignedSize(sampler, "minFilter"));
                if (*output.min_filter < 9728U || *output.min_filter > 9987U)
                    unsupported("Sampler minFilter is unsupported");
            }
            scene_.samplers.push_back(output);
        }
    }

    std::vector<std::uint8_t> imageBytes(const Json& image,
                                         std::string& mime_type) {
        allowedKeys(image, {"bufferView", "mimeType", "name", "uri"},
                    "glTF image");
        if (image.contains("bufferView") == image.contains("uri")) {
            invalid("Image must have exactly one of bufferView or uri");
        }
        if (image.contains("mimeType")) {
            if (!image.at("mimeType").is_string()) invalid("Image mimeType is invalid");
            mime_type = image.at("mimeType").get<std::string>();
        }
        if (image.contains("bufferView")) {
            const std::size_t view = unsignedSize(image, "bufferView");
            if (view >= views_.size() || mime_type.empty()) {
                invalid("Embedded image bufferView or mimeType is invalid");
            }
            return views_.at(view).bytes;
        }
        if (!image.at("uri").is_string()) invalid("Image uri is invalid");
        const std::string uri = image.at("uri").get<std::string>();
        ResolvedResource resource = resolveResource(
                root_path_, uri, resources_, limits_, diagnostics_, observed_external_);
        if (mime_type.empty()) {
            mime_type = resource.media_type.empty()
                    ? inferImageMimeType(uri) : resource.media_type;
        } else if (!resource.media_type.empty()
                   && resource.media_type != "application/octet-stream"
                   && resource.media_type != mime_type) {
            invalid("Image MIME type differs from approved resource metadata");
        }
        return std::move(resource.bytes);
    }

    void loadImages() {
        const Json& images = arrayField(root_, "images", false);
        if (images.size() > limits_.maximum_images) {
            unsupported("glTF image count exceeds the configured limit");
        }
        scene_.images.reserve(images.size());
        for (const auto& image : images) {
            std::string mime_type;
            const auto bytes = imageBytes(image, mime_type);
            const TextureFormat format = TextureCodec::formatForMimeType(mime_type);
            mesh::RgbaImage decoded = TextureCodec::decode(
                    bytes, format, limits_.texture);
            const std::uint64_t pixels = static_cast<std::uint64_t>(decoded.width)
                                         * decoded.height;
            accountant_.reserveTexturePixels(pixels);
            accountant_.reserveDecodedBytes(decoded.pixels.size());
            scene_.images.push_back(std::move(decoded));
            image_formats_.push_back(format);
        }
    }

    void loadTextures() {
        const Json& textures = arrayField(root_, "textures", false);
        scene_.textures.reserve(textures.size());
        for (const auto& texture : textures) {
            allowedKeys(texture, {"extensions", "name", "sampler", "source"},
                        "glTF texture");
            mesh::MeshTexture output;
            bool extension_source = false;
            if (texture.contains("extensions")) {
                const Json& extensions = texture.at("extensions");
                allowedKeys(extensions, {kBasisExtension, kWebpExtension},
                            "texture extensions");
                if (extensions.size() != 1U) {
                    unsupported("Texture has an ambiguous extension source");
                }
                const auto extension_iterator = extensions.begin();
                const Json& extension = extension_iterator.value();
                allowedKeys(extension, {"source"}, "texture source extension");
                output.image = unsignedSize(extension, "source");
                if ((extensions.contains(kBasisExtension)
                     && image_formats_.at(output.image)
                                != TextureFormat::ktx2_basis)
                    || (extensions.contains(kWebpExtension)
                        && image_formats_.at(output.image)
                                   != TextureFormat::webp)) {
                    invalid("Texture extension source MIME type is inconsistent");
                }
                extension_source = true;
            }
            if (!extension_source) output.image = unsignedSize(texture, "source");
            if (output.image >= scene_.images.size()) invalid("Texture image is out of range");
            if (texture.contains("sampler")) {
                output.sampler = unsignedSize(texture, "sampler");
                if (*output.sampler >= scene_.samplers.size())
                    invalid("Texture sampler is out of range");
            }
            scene_.textures.push_back(output);
        }
    }

    static std::array<float, 4> floatArray4(const Json& value,
                                            const char* description) {
        if (!value.is_array() || value.size() != 4U) {
            invalid(std::string(description) + " must have four components");
        }
        std::array<float, 4> result{};
        for (std::size_t index = 0U; index < result.size(); ++index) {
            if (!value.at(index).is_number()) invalid("Material factor is not numeric");
            const double item = value.at(index).get<double>();
            if (!std::isfinite(item)) invalid("Material factor is non-finite");
            result[index] = static_cast<float>(item);
        }
        return result;
    }

    static std::array<float, 3> floatArray3(const Json& value,
                                            const char* description) {
        if (!value.is_array() || value.size() != 3U) {
            invalid(std::string(description) + " must have three components");
        }
        std::array<float, 3> result{};
        for (std::size_t index = 0U; index < result.size(); ++index) {
            if (!value.at(index).is_number()) invalid("Material factor is not numeric");
            const double item = value.at(index).get<double>();
            if (!std::isfinite(item)) invalid("Material factor is non-finite");
            result[index] = static_cast<float>(item);
        }
        return result;
    }

    mesh::TextureBinding textureBinding(
            const Json& value, const char* description,
            const std::optional<std::string>& scalar_field = std::nullopt) const {
        std::set<std::string> keys{"index", "texCoord"};
        if (scalar_field.has_value()) keys.insert(*scalar_field);
        allowedKeys(value, keys, description);
        mesh::TextureBinding binding;
        binding.texture = unsignedSize(value, "index");
        binding.texcoord_set = static_cast<std::uint32_t>(
                unsignedSize(value, "texCoord", 0U));
        if (binding.texture >= scene_.textures.size()
            || binding.texcoord_set > 1U) {
            unsupported("Material texture or texCoord set is unsupported");
        }
        return binding;
    }

    void loadMaterials() {
        const Json& materials = arrayField(root_, "materials", false);
        scene_.materials.reserve(materials.size());
        for (const auto& material : materials) {
            allowedKeys(material, {"alphaCutoff", "alphaMode", "doubleSided",
                                   "emissiveFactor", "emissiveTexture", "extensions",
                                   "name", "normalTexture", "occlusionTexture",
                                   "pbrMetallicRoughness"},
                        "glTF material");
            mesh::MeshMaterial output;
            output.double_sided = material.value("doubleSided", false);
            output.alpha_mode = material.value("alphaMode", std::string("OPAQUE"));
            if (output.alpha_mode != "OPAQUE" && output.alpha_mode != "MASK"
                && output.alpha_mode != "BLEND") {
                unsupported("Material alphaMode is unsupported");
            }
            output.alpha_cutoff = material.value("alphaCutoff", 0.5F);
            if (!std::isfinite(output.alpha_cutoff)) invalid("alphaCutoff is non-finite");
            if (material.contains("emissiveFactor")) {
                output.emissive_factor = floatArray3(
                        material.at("emissiveFactor"), "emissiveFactor");
            }
            if (material.contains("extensions")) {
                const Json& extensions = material.at("extensions");
                allowedKeys(extensions, {kUnlitExtension}, "material extensions");
                if (!extensions.contains(kUnlitExtension)
                    || !extensions.at(kUnlitExtension).is_object()
                    || !extensions.at(kUnlitExtension).empty()) {
                    unsupported("KHR_materials_unlit payload is unsupported");
                }
                output.unlit = true;
            }
            if (material.contains("pbrMetallicRoughness")) {
                const Json& pbr = material.at("pbrMetallicRoughness");
                allowedKeys(pbr, {"baseColorFactor", "baseColorTexture",
                                  "metallicFactor", "metallicRoughnessTexture",
                                  "roughnessFactor"},
                            "pbrMetallicRoughness");
                if (pbr.contains("baseColorFactor")) {
                    output.base_color_factor = floatArray4(
                            pbr.at("baseColorFactor"), "baseColorFactor");
                }
                output.metallic_factor = pbr.value("metallicFactor", 1.0F);
                output.roughness_factor = pbr.value("roughnessFactor", 1.0F);
                if (!std::isfinite(output.metallic_factor)
                    || !std::isfinite(output.roughness_factor)) {
                    invalid("Metallic or roughness factor is non-finite");
                }
                if (pbr.contains("baseColorTexture")) {
                    output.base_color_texture = textureBinding(
                            pbr.at("baseColorTexture"), "baseColorTexture");
                }
                if (pbr.contains("metallicRoughnessTexture")) {
                    output.metallic_roughness_texture = textureBinding(
                            pbr.at("metallicRoughnessTexture"),
                            "metallicRoughnessTexture");
                }
            }
            if (material.contains("normalTexture")) {
                const Json& value = material.at("normalTexture");
                allowedKeys(value, {"index", "scale", "texCoord"}, "normalTexture");
                output.normal_texture = textureBinding(
                        value, "normalTexture", std::string("scale"));
                output.normal_scale = value.value("scale", 1.0F);
            }
            if (material.contains("occlusionTexture")) {
                const Json& value = material.at("occlusionTexture");
                allowedKeys(value, {"index", "strength", "texCoord"},
                            "occlusionTexture");
                output.occlusion_texture = textureBinding(
                        value, "occlusionTexture", std::string("strength"));
                output.occlusion_strength = value.value("strength", 1.0F);
            }
            if (material.contains("emissiveTexture")) {
                output.emissive_texture = textureBinding(
                        material.at("emissiveTexture"), "emissiveTexture");
            }
            if (!std::isfinite(output.normal_scale)
                || !std::isfinite(output.occlusion_strength)) {
                invalid("Material texture scalar is non-finite");
            }
            scene_.materials.push_back(std::move(output));
        }
    }

    static std::uint32_t attributeIndex(const Json& attributes,
                                        const char* semantic) {
        const std::size_t index = unsignedSize(attributes, semantic);
        if (index > std::numeric_limits<std::uint32_t>::max()) {
            invalid("Attribute accessor index exceeds uint32");
        }
        return static_cast<std::uint32_t>(index);
    }

    static std::uint32_t unsigned32Value(const Json& object,
                                         const char* field) {
        const std::size_t value = unsignedSize(object, field);
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            invalid(std::string("glTF uint32 field is too large: ") + field);
        }
        return static_cast<std::uint32_t>(value);
    }

    std::vector<float> primitiveFloatAttribute(
            const Json& attributes, const char* semantic,
            const std::optional<DecodedDracoMesh>& draco,
            const std::map<std::string, std::uint32_t>& draco_ids,
            std::size_t components, const std::set<std::uint32_t>& types,
            bool normalized_integer) const {
        const auto compressed = draco_ids.find(semantic);
        if (compressed != draco_ids.end()) {
            const auto& values = draco->floating_attributes.at(compressed->second);
            std::vector<float> result;
            result.reserve(values.size());
            for (const double value : values) {
                if (!std::isfinite(value)
                        || value < -std::numeric_limits<float>::max()
                        || value > std::numeric_limits<float>::max()) {
                    invalid(std::string(semantic)
                            + " Draco value is outside float range");
                }
                result.push_back(static_cast<float>(value));
            }
            return result;
        }
        return floatAttribute(attributeIndex(attributes, semantic), semantic,
                              components, types, normalized_integer);
    }

    std::vector<std::uint32_t> primitiveFeatureIds(
            const Json& attributes, const std::optional<DecodedDracoMesh>& draco,
            const std::map<std::string, std::uint32_t>& draco_ids,
            const std::string& semantic = "_BATCHID") const {
        const auto compressed = draco_ids.find(semantic);
        if (compressed != draco_ids.end()) {
            return draco->unsigned_attributes.at(compressed->second);
        }
        return unsignedAccessor(
                attributeIndex(attributes, semantic.c_str()), true);
    }

    void readMeshFeatureSets(
            const Json& primitive, const Json& attributes,
            const std::optional<DecodedDracoMesh>& draco,
            const std::map<std::string, std::uint32_t>& draco_ids,
            std::size_t vertex_count, mesh::MeshPrimitive& output) const {
        if (!primitive.contains("extensions")
                || !primitive.at("extensions").contains(
                        kMeshFeaturesExtension)) {
            return;
        }
        if (!limits_.enable_feature_metadata) {
            throw FormatError(
                    FormatErrorCode::content_gltf_feature_unsupported,
                    "Mesh features require the metadata contract");
        }
        const Json& extension =
                primitive.at("extensions").at(kMeshFeaturesExtension);
        allowedKeys(extension, {"featureIds"}, "EXT_mesh_features");
        if (!extension.contains("featureIds")
                || !extension.at("featureIds").is_array()
                || extension.at("featureIds").empty()
                || extension.at("featureIds").size()
                        > limits_.metadata.maximum_feature_id_sets) {
            throw FormatError(
                    FormatErrorCode::metadata_feature_id_invalid,
                    "Mesh feature ID sets are invalid");
        }
        std::set<std::string> labels;
        std::uint32_t set_index = 0U;
        for (const auto& source : extension.at("featureIds")) {
            allowedKeys(source,
                        {"featureCount", "nullFeatureId", "label",
                         "attribute", "texture", "propertyTable"},
                        "mesh feature ID set");
            if (source.contains("texture")) {
                throw FormatError(
                        FormatErrorCode::metadata_feature_id_texture_unsupported,
                        "Mesh feature ID textures are unsupported");
            }
            metadata::FeatureIdSet set;
            set.feature_count = unsigned32Value(source, "featureCount");
            if (set.feature_count == 0U) {
                throw FormatError(
                        FormatErrorCode::metadata_feature_id_invalid,
                        "Mesh feature count is zero");
            }
            if (source.contains("label")) {
                if (!source.at("label").is_string()) {
                    throw FormatError(
                            FormatErrorCode::metadata_feature_id_invalid,
                            "Mesh feature label is invalid");
                }
                set.label = source.at("label").get<std::string>();
                if (set.label.empty() || !labels.insert(set.label).second) {
                    throw FormatError(
                            FormatErrorCode::metadata_feature_id_invalid,
                            "Mesh feature label is duplicated");
                }
            }
            if (source.contains("propertyTable")) {
                set.property_table = unsigned32Value(source, "propertyTable");
                if (!scene_.feature_metadata.has_value()
                        || *set.property_table
                                >= scene_.feature_metadata->property_tables.size()
                        || set.feature_count
                                > scene_.feature_metadata->property_tables
                                          .at(*set.property_table)
                                          .row_count) {
                    throw FormatError(
                            FormatErrorCode::metadata_feature_id_invalid,
                            "Mesh feature property table is invalid");
                }
            }
            if (source.contains("nullFeatureId")) {
                set.null_feature_id = unsigned32Value(source, "nullFeatureId");
                if (*set.null_feature_id < set.feature_count) {
                    throw FormatError(
                            FormatErrorCode::metadata_feature_id_invalid,
                            "Mesh null feature ID identifies a real feature");
                }
            }
            if (source.contains("attribute")) {
                const std::uint32_t attribute =
                        unsigned32Value(source, "attribute");
                if (attribute != set_index) {
                    throw FormatError(
                            FormatErrorCode::metadata_feature_id_invalid,
                            "Mesh feature attributes are not contiguous");
                }
                const std::string semantic =
                        "_FEATURE_ID_" + std::to_string(attribute);
                if (!attributes.contains(semantic)) {
                    throw FormatError(
                            FormatErrorCode::metadata_feature_id_invalid,
                            "Mesh feature attribute is absent");
                }
                const auto accessor_index = attributeIndex(
                        attributes, semantic.c_str());
                const AccessorInfo& accessor = accessors_.at(accessor_index);
                if (accessor.components != 1U || accessor.normalized
                        || (accessor.component_type
                                    != kComponentUnsignedByte
                            && accessor.component_type
                                    != kComponentUnsignedShort
                            && accessor.component_type != kComponentFloat)) {
                    throw FormatError(
                            FormatErrorCode::metadata_feature_id_invalid,
                            "Mesh feature accessor type is invalid");
                }
                set.ids = primitiveFeatureIds(
                        attributes, draco, draco_ids, semantic);
            } else {
                throw FormatError(
                        FormatErrorCode::metadata_feature_id_invalid,
                        "Mesh feature IDs require an attribute in this contract");
            }
            if (set.ids.size() != vertex_count
                    || std::any_of(set.ids.begin(), set.ids.end(),
                                   [&set](std::uint32_t id) {
                                       return id >= set.feature_count
                                               && (!set.null_feature_id.has_value()
                                                   || id
                                                           != *set.null_feature_id);
                                   })) {
                throw FormatError(
                        FormatErrorCode::metadata_feature_id_invalid,
                        "Mesh feature IDs are outside featureCount");
            }
            output.feature_id_sets.push_back(std::move(set));
            ++set_index;
        }
    }

    std::vector<std::uint32_t> expandTopology(
            std::vector<std::uint32_t> indices, std::uint32_t mode) const {
        if (mode == kTrianglesMode) {
            if (indices.empty() || indices.size() % 3U != 0U) {
                invalid("TRIANGLES index count is invalid");
            }
            return indices;
        }
        if (mode != kTriangleStripMode) {
            throw FormatError(
                    FormatErrorCode::content_primitive_mode_unsupported,
                    "Primitive mode is unsupported");
        }
        std::vector<std::uint32_t> triangles;
        if (indices.size() < 3U) return triangles;
        triangles.reserve((indices.size() - 2U) * 3U);
        for (std::size_t index = 2U; index < indices.size(); ++index) {
            std::uint32_t first = indices[index - 2U];
            std::uint32_t second = indices[index - 1U];
            const std::uint32_t third = indices[index];
            if ((index & 1U) != 0U) std::swap(first, second);
            if (first == second || second == third || first == third) continue;
            triangles.push_back(first);
            triangles.push_back(second);
            triangles.push_back(third);
        }
        return triangles;
    }

    mesh::MeshPrimitive readPrimitive(const Json& primitive) {
        allowedKeys(primitive, {"attributes", "extensions", "indices", "material",
                                "mode"},
                    "glTF primitive");
        if (!primitive.contains("attributes")
            || !primitive.at("attributes").is_object()) {
            invalid("Primitive attributes are missing");
        }
        const Json& attributes = primitive.at("attributes");
        const std::set<std::string> core_attributes{
                "COLOR_0", "NORMAL", "POSITION", "TANGENT", "TEXCOORD_0",
                "TEXCOORD_1", "_BATCHID"};
        for (const auto& attribute : attributes.items()) {
            if (core_attributes.count(attribute.key()) == 0U
                    && !(limits_.enable_feature_metadata
                         && featureIdSemantic(attribute.key()))) {
                unsupported("Primitive attribute semantic is unsupported");
            }
        }
        if (!attributes.contains("POSITION")) invalid("Primitive POSITION is missing");

        std::optional<DecodedDracoMesh> draco;
        std::map<std::string, std::uint32_t> draco_ids;
        if (primitive.contains("extensions")) {
            const Json& extensions = primitive.at("extensions");
            allowedKeys(extensions,
                        {kDracoExtension, kMeshFeaturesExtension},
                        "primitive extensions");
            if (extensions.contains(kDracoExtension)) {
                const Json& extension = extensions.at(kDracoExtension);
                allowedKeys(extension, {"attributes", "bufferView"},
                            "KHR_draco_mesh_compression");
                const std::size_t view_index = unsignedSize(extension, "bufferView");
                if (view_index >= views_.size() || views_[view_index].meshopt) {
                    unsupported("Draco bufferView cannot also use Meshopt");
                }
                if (!extension.contains("attributes")
                    || !extension.at("attributes").is_object()) {
                    invalid("Draco attribute mapping is missing");
                }
                const Json& mapping = extension.at("attributes");
                for (const auto& mapped : mapping.items()) {
                    if (core_attributes.count(mapped.key()) == 0U
                            && !(limits_.enable_feature_metadata
                                 && featureIdSemantic(mapped.key()))) {
                        unsupported("Draco attribute semantic is unsupported");
                    }
                }
                std::vector<DracoAttributeRequest> requests;
                for (const auto& item : mapping.items()) {
                    if (!attributes.contains(item.key())) {
                        invalid("Draco mapping semantic is absent from primitive attributes");
                    }
                    const std::size_t accessor_index = unsignedSize(attributes,
                                                                     item.key().c_str());
                    if (accessor_index >= accessors_.size()) invalid("Draco accessor is out of range");
                    const AccessorInfo& accessor = accessors_[accessor_index];
                    const std::size_t id_value = unsignedSize(mapping, item.key().c_str());
                    if (id_value > std::numeric_limits<std::uint32_t>::max())
                        invalid("Draco unique id exceeds uint32");
                    const auto id = static_cast<std::uint32_t>(id_value);
                    draco_ids[item.key()] = id;
                    requests.push_back({id, accessor.components,
                            (item.key() == "_BATCHID"
                             || featureIdSemantic(item.key()))
                                    ? DracoAttributeValueType::unsigned_integer
                                    : DracoAttributeValueType::floating_point});
                }
                draco = DracoDecoder::decode(ByteView(views_[view_index].bytes),
                                             requests, limits_.draco);
                ++diagnostics_.draco_primitive_count;
                for (const auto& item : mapping.items()) {
                    const AccessorInfo& accessor = accessors_.at(
                            unsignedSize(attributes, item.key().c_str()));
                    if (accessor.count != draco->point_count) {
                        ++diagnostics_.draco_count_compatibility_count;
                    }
                }
            }
        }

        mesh::MeshPrimitive output;
        output.positions = primitiveFloatAttribute(
                attributes, "POSITION", draco, draco_ids, 3U,
                {kComponentFloat}, false);
        const std::size_t vertex_count = output.vertexCount();
        if (attributes.contains("NORMAL")) {
            output.normals = primitiveFloatAttribute(
                    attributes, "NORMAL", draco, draco_ids, 3U,
                    {kComponentByte, kComponentShort, kComponentFloat}, true);
        }
        if (attributes.contains("TANGENT")) {
            output.tangents = primitiveFloatAttribute(
                    attributes, "TANGENT", draco, draco_ids, 4U,
                    {kComponentByte, kComponentShort, kComponentFloat}, true);
        }
        if (attributes.contains("TEXCOORD_0")) {
            output.texcoords_0 = primitiveFloatAttribute(
                    attributes, "TEXCOORD_0", draco, draco_ids, 2U,
                    {kComponentUnsignedByte, kComponentUnsignedShort,
                     kComponentFloat}, true);
        }
        if (attributes.contains("TEXCOORD_1")) {
            output.texcoords_1 = primitiveFloatAttribute(
                    attributes, "TEXCOORD_1", draco, draco_ids, 2U,
                    {kComponentUnsignedByte, kComponentUnsignedShort,
                     kComponentFloat}, true);
        }
        if (attributes.contains("COLOR_0")) {
            const AccessorInfo& accessor = accessors_.at(
                    attributeIndex(attributes, "COLOR_0"));
            if (accessor.components != 3U && accessor.components != 4U)
                invalid("COLOR_0 must be VEC3 or VEC4");
            output.color_components = static_cast<std::uint32_t>(accessor.components);
            output.colors = primitiveFloatAttribute(
                    attributes, "COLOR_0", draco, draco_ids,
                    accessor.components,
                    {kComponentUnsignedByte, kComponentUnsignedShort,
                     kComponentFloat}, true);
        }
        if (attributes.contains("_BATCHID")) {
            output.feature_ids = primitiveFeatureIds(attributes, draco, draco_ids);
        }
        readMeshFeatureSets(primitive, attributes, draco, draco_ids,
                            vertex_count, output);

        std::vector<std::uint32_t> indices;
        if (draco.has_value()) {
            indices = draco->indices;
            if (primitive.contains("indices")) {
                const AccessorInfo& declared = accessors_.at(
                        unsignedSize(primitive, "indices"));
                if (declared.count != indices.size()) {
                    ++diagnostics_.draco_count_compatibility_count;
                }
            }
        } else if (primitive.contains("indices")) {
            indices = unsignedAccessor(unsignedSize(primitive, "indices"), false);
        } else {
            if (vertex_count > std::numeric_limits<std::uint32_t>::max())
                unsupported("Non-indexed primitive has too many vertices");
            indices.resize(vertex_count);
            for (std::size_t index = 0U; index < vertex_count; ++index)
                indices[index] = static_cast<std::uint32_t>(index);
        }
        output.source_mode = primitive.value("mode", kTrianglesMode)
                == kTriangleStripMode ? mesh::PrimitiveMode::triangle_strip
                                      : mesh::PrimitiveMode::triangles;
        output.indices = expandTopology(
                std::move(indices), primitive.value("mode", kTrianglesMode));
        if (output.indices.empty()) invalid("Primitive contains no non-degenerate triangles");
        if (std::any_of(output.indices.begin(), output.indices.end(),
                        [vertex_count](std::uint32_t index) {
                            return index >= vertex_count;
                        })) {
            invalid("Primitive index is out of range");
        }
        if (primitive.contains("material")) {
            output.material = unsignedSize(primitive, "material");
            if (*output.material >= scene_.materials.size())
                invalid("Primitive material is out of range");
        }
        accountant_.reserveVertices(vertex_count);
        accountant_.reserveIndices(output.indices.size());
        mesh::validateMeshPrimitive(output);
        return output;
    }

    void loadMeshes() {
        const Json& meshes = arrayField(root_, "meshes", true);
        if (meshes.empty() || meshes.size() > limits_.maximum_meshes) {
            unsupported("glTF mesh count exceeds the configured limit");
        }
        scene_.meshes.reserve(meshes.size());
        std::size_t primitive_count = 0U;
        for (const auto& source_mesh : meshes) {
            allowedKeys(source_mesh, {"name", "primitives"}, "glTF mesh");
            if (!source_mesh.contains("primitives")
                || !source_mesh.at("primitives").is_array()
                || source_mesh.at("primitives").empty()) {
                invalid("Mesh primitives are missing");
            }
            primitive_count += source_mesh.at("primitives").size();
            if (primitive_count > limits_.maximum_primitives) {
                unsupported("Primitive count exceeds the configured limit");
            }
            mesh::Mesh output;
            output.primitives.reserve(source_mesh.at("primitives").size());
            for (const auto& primitive : source_mesh.at("primitives")) {
                output.primitives.push_back(readPrimitive(primitive));
            }
            scene_.meshes.push_back(std::move(output));
        }
    }

    template <std::size_t Count>
    static std::array<double, Count> numberArray(const Json& value,
                                                  const char* description) {
        if (!value.is_array() || value.size() != Count) {
            invalid(std::string(description) + " has an invalid component count");
        }
        std::array<double, Count> result{};
        for (std::size_t index = 0U; index < Count; ++index) {
            if (!value.at(index).is_number())
                invalid(std::string(description) + " contains a non-number");
            result[index] = value.at(index).get<double>();
            if (!std::isfinite(result[index]))
                invalid(std::string(description) + " contains a non-finite value");
        }
        return result;
    }

    static geometry::Matrix4 nodeTransform(const Json& node) {
        if (node.contains("matrix")) {
            if (node.contains("translation") || node.contains("rotation")
                || node.contains("scale")) {
                invalid("Node cannot combine matrix and TRS");
            }
            return geometry::Matrix4::fromColumnMajor(
                    numberArray<16U>(node.at("matrix"), "node matrix"));
        }
        geometry::Matrix4 result = geometry::Matrix4::identity();
        if (node.contains("translation")) {
            result = result * geometry::Matrix4::translation(
                    numberArray<3U>(node.at("translation"), "node translation"));
        }
        if (node.contains("rotation")) {
            result = result * geometry::Matrix4::quaternion(
                    numberArray<4U>(node.at("rotation"), "node rotation"));
        }
        if (node.contains("scale")) {
            result = result * geometry::Matrix4::scale(
                    numberArray<3U>(node.at("scale"), "node scale"));
        }
        return result;
    }

    void visitNode(std::size_t index, std::vector<std::uint8_t>& state) const {
        if (index >= scene_.nodes.size()) invalid("Scene node index is out of range");
        if (state[index] == 1U) invalid("Node graph contains a cycle");
        if (state[index] == 2U) return;
        state[index] = 1U;
        for (const std::size_t child : scene_.nodes[index].children)
            visitNode(child, state);
        state[index] = 2U;
    }

    void loadNodesAndScene() {
        const Json& nodes = arrayField(root_, "nodes", true);
        if (nodes.empty() || nodes.size() > limits_.maximum_nodes) {
            unsupported("glTF node count exceeds the configured limit");
        }
        scene_.nodes.reserve(nodes.size());
        for (std::size_t index = 0U; index < nodes.size(); ++index) {
            const Json& node = indexed(nodes, index, "node");
            allowedKeys(node, {"children", "extensions", "matrix", "mesh", "name",
                               "rotation", "scale", "translation"},
                        "glTF node");
            mesh::MeshNode output;
            output.source_ordinal = index;
            output.local_transform = nodeTransform(node);
            if (node.contains("mesh")) {
                output.mesh = unsignedSize(node, "mesh");
                if (*output.mesh >= scene_.meshes.size())
                    invalid("Node mesh is out of range");
            }
            if (node.contains("children")) {
                if (!node.at("children").is_array()) invalid("Node children are invalid");
                for (const auto& child : node.at("children")) {
                    if (!child.is_number_unsigned()) invalid("Node child is invalid");
                    output.children.push_back(child.get<std::size_t>());
                }
            }
            if (node.contains("extensions")) {
                if (!limits_.enable_gpu_instancing
                        || !node.at("extensions").is_object()) {
                    unsupported("Node extension is unsupported");
                }
                const Json& extensions = node.at("extensions");
                allowedKeys(extensions,
                            {kGpuInstancingExtension,
                             kInstanceFeaturesExtension},
                            "glTF node extensions");
                if (!extensions.contains(kGpuInstancingExtension)) {
                    unsupported("Instance feature metadata lacks GPU instancing");
                }
                const Json& gpu = extensions.at(kGpuInstancingExtension);
                allowedKeys(gpu, {"attributes"},
                            "EXT_mesh_gpu_instancing");
                if (!gpu.contains("attributes")
                        || !gpu.at("attributes").is_object()) {
                    invalid("GPU instance attributes are missing");
                }
                const Json& attributes = gpu.at("attributes");
                allowedKeys(attributes,
                            {"ROTATION", "SCALE", "TRANSLATION",
                             "_FEATURE_ID_0"},
                            "GPU instance attributes");
                if (!attributes.contains("TRANSLATION")
                        || !attributes.contains("ROTATION")
                        || !attributes.contains("SCALE")) {
                    invalid("Canonical GPU instance TRS is incomplete");
                }
                mesh::MeshNodeInstancing instancing;
                instancing.translations = floatAttribute(
                        unsignedSize(attributes, "TRANSLATION"),
                        "TRANSLATION", 3U, {kComponentFloat}, false);
                instancing.rotations = floatAttribute(
                        unsignedSize(attributes, "ROTATION"),
                        "ROTATION", 4U, {kComponentFloat}, false);
                instancing.scales = floatAttribute(
                        unsignedSize(attributes, "SCALE"),
                        "SCALE", 3U, {kComponentFloat}, false);
                const std::size_t count = instancing.instanceCount();
                if (count == 0U || instancing.rotations.size() != count * 4U
                        || instancing.scales.size() != count * 3U) {
                    invalid("Canonical GPU instance streams are not aligned");
                }
                if (attributes.contains("_FEATURE_ID_0")) {
                    if (!limits_.enable_feature_metadata
                            || !extensions.contains(kInstanceFeaturesExtension)) {
                        throw FormatError(
                                FormatErrorCode::metadata_feature_id_invalid,
                                "Instance feature accessor lacks typed metadata");
                    }
                    instancing.feature_ids = unsignedAccessor(
                            unsignedSize(attributes, "_FEATURE_ID_0"), true);
                    if (instancing.feature_ids.size() != count) {
                        throw FormatError(
                                FormatErrorCode::metadata_feature_id_invalid,
                                "Instance feature IDs are not aligned");
                    }
                }
                if (extensions.contains(kInstanceFeaturesExtension)) {
                    if (!limits_.enable_feature_metadata
                            || instancing.feature_ids.empty()) {
                        throw FormatError(
                                FormatErrorCode::metadata_feature_id_invalid,
                                "Instance feature metadata has no accessor");
                    }
                    const Json& features =
                            extensions.at(kInstanceFeaturesExtension);
                    allowedKeys(features, {"featureIds"},
                                "EXT_instance_features");
                    if (!features.contains("featureIds")
                            || !features.at("featureIds").is_array()
                            || features.at("featureIds").size() != 1U) {
                        throw FormatError(
                                FormatErrorCode::metadata_feature_id_invalid,
                                "Canonical instance feature set is invalid");
                    }
                    const Json& feature = features.at("featureIds").front();
                    allowedKeys(feature,
                                {"attribute", "featureCount", "propertyTable"},
                                "instance feature ID set");
                    if (unsignedSize(feature, "attribute") != 0U) {
                        throw FormatError(
                                FormatErrorCode::metadata_feature_id_invalid,
                                "Canonical instance feature attribute is not zero");
                    }
                    const std::uint32_t feature_count =
                            unsigned32Value(feature, "featureCount");
                    if (feature_count == 0U
                            || std::any_of(instancing.feature_ids.begin(),
                                           instancing.feature_ids.end(),
                                           [feature_count](std::uint32_t id) {
                                               return id >= feature_count;
                                           })) {
                        throw FormatError(
                                FormatErrorCode::metadata_feature_id_invalid,
                                "Canonical instance feature ID is outside featureCount");
                    }
                    if (feature.contains("propertyTable")) {
                        const std::size_t property_table =
                                unsignedSize(feature, "propertyTable");
                        if (!scene_.feature_metadata.has_value()
                                || property_table
                                        >= scene_.feature_metadata
                                                   ->property_tables.size()) {
                            throw FormatError(
                                    FormatErrorCode::metadata_property_table_invalid,
                                    "Canonical instance property table is invalid");
                        }
                        scene_.feature_metadata->primary_property_table =
                                property_table;
                    }
                }
                output.instancing = std::move(instancing);
            }
            scene_.nodes.push_back(std::move(output));
        }
        const Json& scenes = arrayField(root_, "scenes", true);
        if (scenes.empty()) invalid("glTF has no scene");
        const std::size_t active = unsignedSize(root_, "scene", 0U);
        const Json& source_scene = indexed(scenes, active, "scene");
        allowedKeys(source_scene, {"name", "nodes"}, "glTF scene");
        if (!source_scene.contains("nodes") || !source_scene.at("nodes").is_array())
            invalid("glTF scene roots are invalid");
        std::vector<std::size_t> roots;
        for (const auto& root : source_scene.at("nodes")) {
            if (!root.is_number_unsigned()) invalid("Scene root is invalid");
            roots.push_back(root.get<std::size_t>());
        }
        if (roots.empty()) invalid("Default scene has no roots");
        scene_.scenes = {roots};
        scene_.default_scene = 0U;
        std::vector<std::uint8_t> state(scene_.nodes.size(), 0U);
        for (const std::size_t root : roots) visitNode(root, state);
    }

    Json root_;
    std::vector<std::uint8_t> glb_binary_;
    std::string root_path_;
    const ApprovedGltfResourceMap& resources_;
    const GltfMeshReaderLimits& limits_;
    mesh::MeshResourceAccountant accountant_;
    mesh::MeshScene scene_;
    GltfMeshReadDiagnostics diagnostics_;
    std::set<std::string> observed_external_;
    std::vector<std::vector<std::uint8_t>> buffers_;
    std::vector<DecodedView> views_;
    std::vector<AccessorInfo> accessors_;
    std::vector<TextureFormat> image_formats_;
};

Json parseJson(const std::string& text) {
    try {
        const Json parsed = Json::parse(text);
        if (!parsed.is_object()) invalid("glTF JSON root must be an object");
        return parsed;
    } catch (const FormatError&) {
        throw;
    } catch (const Json::exception&) {
        throw FormatError(FormatErrorCode::invalid_json, "glTF JSON is invalid");
    }
}

}  // namespace

GltfMeshReadResult GltfMeshReader::read(
        const std::vector<std::uint8_t>& root_bytes, GltfContentKind kind,
        const std::string& root_package_relative_path,
        const ApprovedGltfResourceMap& approved_resources,
        const GltfMeshReaderLimits& limits) {
    try {
        if (root_bytes.empty() || root_package_relative_path.empty()) {
            invalid("glTF root content is empty or unnamed");
        }
        Json root;
        std::vector<std::uint8_t> binary;
        if (kind == GltfContentKind::glb) {
            const GlbDocument document = GlbParser::parse(ByteView(root_bytes));
            root = parseJson(document.json_text);
            if (document.binary_length > 0U) {
                binary.assign(root_bytes.begin()
                                      + static_cast<std::ptrdiff_t>(document.binary_offset),
                              root_bytes.begin()
                                      + static_cast<std::ptrdiff_t>(
                                              document.binary_offset
                                              + document.binary_length));
            }
        } else {
            root = parseJson(std::string(
                    reinterpret_cast<const char*>(root_bytes.data()),
                    root_bytes.size()));
        }
        return DocumentReader(std::move(root), std::move(binary),
                              root_package_relative_path, approved_resources,
                              limits).read();
    } catch (const FormatError&) {
        throw;
    } catch (const Json::exception&) {
        throw FormatError(FormatErrorCode::invalid_json,
                          "glTF JSON structure is invalid");
    } catch (const std::out_of_range&) {
        throw FormatError(FormatErrorCode::invalid_accessor,
                          "glTF reference is out of range");
    }
}

}  // namespace clip_worker::formats
