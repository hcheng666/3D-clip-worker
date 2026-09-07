#include "clip_worker/inspection/pipeline/inspection_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <uriparser/Uri.h>

namespace clip_worker::inspection::pipeline {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kSha256Bytes = 32U;
constexpr std::size_t kBinaryHeaderReadBytes = 64U;
constexpr std::uint32_t kGlbJsonChunkType = 0x4e4f534aU;
constexpr const char* kClosureHashDomain = "three-d-tiles-source-closure-v1";
constexpr const char* kInlineIdentityDomain = "three-d-tiles-inline-resource-v1";
constexpr const char* kResultManifestDomain = "three-d-tiles-result-manifest-v1";
constexpr const char* kInlinePathPrefix = "__inspection_inline__/sha256/";
constexpr const char* kInlineEtagPrefix = "INLINE_SHA256:";
constexpr const char* kArchiveEntryEtagPrefix = "ARCHIVE_ENTRY_SHA256:";
constexpr const char* kMetadataExtension = "3DTILES_metadata";

enum class ReferenceKind {
    tileset_content,
    implicit_content_template,
    implicit_subtree_template,
    gltf_buffer,
    gltf_image,
    metadata_schema,
    subtree_buffer,
    i3dm_gltf
};

struct RawReference {
    std::string uri;
    ReferenceKind kind = ReferenceKind::tileset_content;
};

struct Node {
    ResourceEvidence evidence;
    std::optional<Json> document;
    std::vector<RawReference> raw_references;
    std::vector<std::size_t> dependencies;
    bool has_implicit_template = false;
};

struct DecodedDataUri {
    std::string media_type;
    std::vector<std::uint8_t> bytes;
};

bool containsUnsupportedSubtreeMetadata(const Json& document) {
    static constexpr std::array<const char*, 9U> kUnsupportedFields = {
            "subtreeMetadata", "tileMetadata", "contentMetadata",
            "propertyTables", "propertyTextures", "propertyAttributes",
            "statistics", "tileBoundingVolumes", "contentBoundingVolumes"};
    return std::any_of(kUnsupportedFields.begin(), kUnsupportedFields.end(),
            [&document](const char* field) { return document.contains(field); });
}

[[noreturn]] void fail(PipelineFailureKind kind, DiagnosticCode code,
                       InspectorStage stage, const char* message) {
    throw InspectionPipelineError(kind, code, stage, message);
}

[[noreturn]] void invalidContent(const char* message) {
    fail(PipelineFailureKind::invalid, DiagnosticCode::content_invalid,
         InspectorStage::structure_classification, message);
}

[[noreturn]] void invalidClosure(const char* message) {
    fail(PipelineFailureKind::invalid, DiagnosticCode::closure_invalid,
         InspectorStage::closure_validation, message);
}

[[noreturn]] void invalidResource(const char* message) {
    fail(PipelineFailureKind::invalid, DiagnosticCode::resource_invalid,
         InspectorStage::resource_resolution, message);
}

[[noreturn]] void resourceLimit(const char* message) {
    fail(PipelineFailureKind::limit_exceeded,
         DiagnosticCode::resource_limit_exceeded,
         InspectorStage::resource_resolution, message);
}

void requireContinue(const package::ContinuePredicate& should_continue) {
    if (should_continue && !should_continue()) {
        fail(PipelineFailureKind::cancelled, DiagnosticCode::deadline_expired,
             InspectorStage::resource_resolution,
             "Inspection pipeline was cancelled");
    }
}

std::uint32_t readLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
            | static_cast<std::uint32_t>(bytes[1]) << 8U
            | static_cast<std::uint32_t>(bytes[2]) << 16U
            | static_cast<std::uint32_t>(bytes[3]) << 24U;
}

std::uint64_t readLe64(const std::uint8_t* bytes) {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

std::string encodeHex(const unsigned char* digest, std::size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(length * 2U, '0');
    for (std::size_t index = 0U; index < length; ++index) {
        output[index * 2U] = kHex[digest[index] >> 4U];
        output[index * 2U + 1U] = kHex[digest[index] & 0x0fU];
    }
    return output;
}

std::string sha256(const std::uint8_t* bytes, std::size_t size) {
    using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Digest digest(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
    unsigned int length = 0U;
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1
        || (size != 0U
            && EVP_DigestUpdate(digest.get(), bytes, size) != 1)
        || EVP_DigestFinal_ex(digest.get(), result.data(), &length) != 1
        || length != kSha256Bytes) {
        throw std::runtime_error("SHA-256 calculation failed");
    }
    return encodeHex(result.data(), length);
}

std::string sha256(const std::string& value) {
    return sha256(reinterpret_cast<const std::uint8_t*>(value.data()),
                  value.size());
}

std::string sha256(const std::vector<std::uint8_t>& value) {
    return sha256(value.data(), value.size());
}

bool startsWithAsciiCaseInsensitive(const std::string& value,
                                    const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    if (value.size() < length) return false;
    for (std::size_t index = 0U; index < length; ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index]))
            != std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

bool rangePresent(const UriTextRangeA& range) {
    return range.first != nullptr;
}

struct UriDeleter {
    void operator()(UriUriA* uri) const noexcept {
        if (uri != nullptr) uriFreeUriMembersA(uri);
    }
};

void validateUriGrammar(const std::string& value) {
    UriUriA uri{};
    const char* error_position = nullptr;
    const int status = uriParseSingleUriA(&uri, value.c_str(), &error_position);
    std::unique_ptr<UriUriA, UriDeleter> cleanup(&uri);
    if (status != URI_SUCCESS || rangePresent(uri.scheme)
        || rangePresent(uri.hostText) || rangePresent(uri.userInfo)
        || rangePresent(uri.portText) || rangePresent(uri.query)
        || rangePresent(uri.fragment) || uri.absolutePath == URI_TRUE) {
        invalidResource("Resource URI is outside the approved package namespace");
    }
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string strictPercentDecodePath(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const unsigned char current = static_cast<unsigned char>(value[index]);
        if (current < 0x20U || current == 0x7fU) {
            invalidResource("Resource URI contains a control character");
        }
        if (value[index] != '%') {
            decoded.push_back(value[index]);
            continue;
        }
        if (value.size() - index < 3U) {
            invalidResource("Resource URI percent escape is truncated");
        }
        const int high = hexValue(value[index + 1U]);
        const int low = hexValue(value[index + 2U]);
        if (high < 0 || low < 0) {
            invalidResource("Resource URI percent escape is invalid");
        }
        const char byte = static_cast<char>((high << 4) | low);
        if (byte == '\0' || byte == '/' || byte == '\\') {
            invalidResource("Resource URI encodes a forbidden separator");
        }
        decoded.push_back(byte);
        index += 2U;
    }
    return decoded;
}

std::string parentDirectory(const std::string& path) {
    const std::size_t separator = path.rfind('/');
    return separator == std::string::npos ? std::string{}
                                           : path.substr(0U, separator + 1U);
}

std::string resolveRelativePath(const std::string& base_path,
                                const std::string& raw_uri) {
    if (raw_uri.empty() || raw_uri.find('?') != std::string::npos
        || raw_uri.find('#') != std::string::npos
        || raw_uri.rfind("//", 0U) == 0U) {
        invalidResource("Resource URI has a forbidden form");
    }
    validateUriGrammar(raw_uri);
    const std::string decoded = strictPercentDecodePath(raw_uri);
    const std::string candidate = parentDirectory(base_path) + decoded;
    package::PackagePathAnalyzer analyzer;
    const package::PackagePathAnalysis analysis = analyzer.analyze(
            candidate, ProtocolLimits::kMaximumPackagePathUtf8Bytes);
    const package::PathAnalysisFlag forbidden = package::PathAnalysisFlag::absolute
            | package::PathAnalysisFlag::drive_prefix
            | package::PathAnalysisFlag::backslash
            | package::PathAnalysisFlag::dot_segment
            | package::PathAnalysisFlag::percent_encoded_alias
            | package::PathAnalysisFlag::unsupported_scheme
            | package::PathAnalysisFlag::reserved_namespace
            | package::PathAnalysisFlag::empty_segment
            | package::PathAnalysisFlag::non_nfc;
    if (package::hasFlag(analysis.flags, forbidden)) {
        invalidResource("Resource URI does not resolve to a canonical package path");
    }
    return analysis.normalized_candidate;
}

std::vector<std::uint8_t> strictPercentDecodeData(const std::string& value,
                                                  std::uint64_t maximum_bytes) {
    std::vector<std::uint8_t> decoded;
    decoded.reserve(std::min<std::uint64_t>(value.size(), maximum_bytes));
    for (std::size_t index = 0U; index < value.size(); ++index) {
        std::uint8_t byte = static_cast<std::uint8_t>(value[index]);
        if (value[index] == '%') {
            if (value.size() - index < 3U) invalidResource("Data URI escape is truncated");
            const int high = hexValue(value[index + 1U]);
            const int low = hexValue(value[index + 2U]);
            if (high < 0 || low < 0) invalidResource("Data URI escape is invalid");
            byte = static_cast<std::uint8_t>((high << 4) | low);
            index += 2U;
        }
        if (decoded.size() >= maximum_bytes) resourceLimit("Data URI exceeds its byte limit");
        decoded.push_back(byte);
    }
    return decoded;
}

std::vector<std::uint8_t> strictBase64Decode(const std::string& value,
                                             std::uint64_t maximum_bytes) {
    if (value.size() % 4U != 0U
        || std::any_of(value.begin(), value.end(), [](unsigned char current) {
               return std::isspace(current) != 0;
           })) {
        invalidResource("Data URI base64 is not canonical");
    }
    std::size_t padding = 0U;
    if (!value.empty() && value.back() == '=') ++padding;
    if (value.size() > 1U && value[value.size() - 2U] == '=') ++padding;
    for (std::size_t index = 0U; index + padding < value.size(); ++index) {
        const unsigned char current = static_cast<unsigned char>(value[index]);
        if (!(std::isalnum(current) != 0 || current == '+' || current == '/')) {
            invalidResource("Data URI base64 alphabet is invalid");
        }
    }
    const std::uint64_t decoded_size = value.empty()
            ? 0U : static_cast<std::uint64_t>(value.size() / 4U * 3U - padding);
    if (decoded_size > maximum_bytes
        || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        resourceLimit("Data URI exceeds its byte limit");
    }
    std::vector<std::uint8_t> decoded(value.size() / 4U * 3U);
    if (!value.empty()) {
        const int count = EVP_DecodeBlock(decoded.data(),
                reinterpret_cast<const unsigned char*>(value.data()),
                static_cast<int>(value.size()));
        if (count < 0 || static_cast<std::uint64_t>(count) < padding
            || static_cast<std::uint64_t>(count) - padding != decoded_size) {
            invalidResource("Data URI base64 payload is invalid");
        }
    }
    decoded.resize(static_cast<std::size_t>(decoded_size));
    return decoded;
}

DecodedDataUri decodeDataUri(const std::string& uri,
                             std::uint64_t maximum_bytes) {
    if (!startsWithAsciiCaseInsensitive(uri, "data:")) {
        throw std::invalid_argument("Not a data URI");
    }
    const std::size_t comma = uri.find(',');
    if (comma == std::string::npos || comma > 1024U) {
        invalidResource("Data URI metadata is invalid");
    }
    std::string metadata = uri.substr(5U, comma - 5U);
    if (std::any_of(metadata.begin(), metadata.end(), [](unsigned char current) {
            return current < 0x20U || current > 0x7eU;
        })) {
        invalidResource("Data URI metadata is not bounded ASCII");
    }
    bool base64 = false;
    constexpr const char* kBase64Marker = ";base64";
    if (metadata.size() >= std::strlen(kBase64Marker)
        && metadata.compare(metadata.size() - std::strlen(kBase64Marker),
                            std::strlen(kBase64Marker), kBase64Marker) == 0) {
        base64 = true;
        metadata.erase(metadata.size() - std::strlen(kBase64Marker));
    }
    const std::size_t parameter = metadata.find(';');
    const std::string media_type = parameter == std::string::npos
            ? metadata : metadata.substr(0U, parameter);
    if (media_type.find('/') == std::string::npos && !media_type.empty()) {
        invalidResource("Data URI media type is invalid");
    }
    const std::string payload = uri.substr(comma + 1U);
    return {media_type.empty() ? "text/plain" : media_type,
            base64 ? strictBase64Decode(payload, maximum_bytes)
                   : strictPercentDecodeData(payload, maximum_bytes)};
}

Json parseJson(const std::vector<std::uint8_t>& bytes,
               const PipelineLimits& limits) {
    if (bytes.size() > limits.maximum_json_bytes) {
        resourceLimit("JSON resource exceeds its byte limit");
    }
    std::vector<std::set<std::string>> object_keys;
    auto callback = [&object_keys, &limits](int depth, Json::parse_event_t event,
                                            Json& parsed) {
        if (depth < 0
            || static_cast<std::uint32_t>(depth) > limits.maximum_json_nesting) {
            resourceLimit("JSON resource exceeds its nesting limit");
        }
        if (event == Json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == Json::parse_event_t::key) {
            if (object_keys.empty()) invalidContent("JSON parser object stack is invalid");
            const std::string key = parsed.get<std::string>();
            if (!object_keys.back().insert(key).second) {
                invalidContent("JSON resource contains a duplicate key");
            }
        } else if (event == Json::parse_event_t::object_end) {
            if (object_keys.empty()) invalidContent("JSON parser object stack is invalid");
            object_keys.pop_back();
        }
        return true;
    };
    try {
        const std::string text(reinterpret_cast<const char*>(bytes.data()),
                               bytes.size());
        Json result = Json::parse(text, callback, true, false);
        if (!result.is_object()) invalidContent("JSON resource is not an object");
        return result;
    } catch (const InspectionPipelineError&) {
        throw;
    } catch (const std::exception&) {
        invalidContent("JSON resource is malformed");
    }
}

bool isNonNegativeNumber(const Json& value) {
    return value.is_number() && std::isfinite(value.get<double>())
            && value.get<double>() >= 0.0;
}

bool isTileset(const Json& value) {
    return value.contains("asset") && value["asset"].is_object()
            && value["asset"].contains("version")
            && value["asset"]["version"].is_string()
            && value.contains("geometricError")
            && isNonNegativeNumber(value["geometricError"])
            && value.contains("root") && value["root"].is_object();
}

bool isGltf(const Json& value) {
    if (!value.contains("asset") || !value["asset"].is_object()
        || !value["asset"].contains("version")
        || !value["asset"]["version"].is_string()) {
        return false;
    }
    const std::string version = value["asset"]["version"].get<std::string>();
    const bool gltf_shape = value.contains("scenes") || value.contains("nodes")
            || value.contains("meshes") || value.contains("buffers")
            || value.contains("images") || value.contains("accessors");
    return version.rfind("2.", 0U) == 0U && gltf_shape
            && !value.contains("root") && !value.contains("geometricError");
}

bool isSubtree(const Json& value) {
    return value.contains("tileAvailability")
            && value["tileAvailability"].is_object()
            && value.contains("childSubtreeAvailability")
            && value["childSubtreeAvailability"].is_object()
            && (!value.contains("contentAvailability")
                || value["contentAvailability"].is_object()
                || value["contentAvailability"].is_array());
}

std::vector<std::string> requiredExtensions(const Json& value) {
    std::vector<std::string> extensions;
    if (!value.contains("extensionsRequired")) return extensions;
    if (!value["extensionsRequired"].is_array()) {
        invalidContent("extensionsRequired is not an array");
    }
    for (const auto& extension : value["extensionsRequired"]) {
        if (!extension.is_string()) invalidContent("Required extension name is invalid");
        const std::string name = extension.get<std::string>();
        if (name.empty()
            || name.size() > ProtocolLimits::kMaximumExtensionUtf8Bytes) {
            invalidContent("Required extension name is outside bounds");
        }
        extensions.push_back(name);
    }
    std::sort(extensions.begin(), extensions.end());
    extensions.erase(std::unique(extensions.begin(), extensions.end()),
                     extensions.end());
    if (extensions.size()
        > ProtocolLimits::kMaximumRequiredExtensionsPerResource) {
        resourceLimit("Required extension list exceeds its limit");
    }
    return extensions;
}

std::vector<std::string> usedExtensions(const Json& value,
                                        const std::vector<std::string>& required) {
    std::vector<std::string> extensions;
    if (value.contains("extensionsUsed")) {
        if (!value["extensionsUsed"].is_array()) {
            invalidContent("extensionsUsed is not an array");
        }
        for (const auto& extension : value["extensionsUsed"]) {
            if (!extension.is_string()) invalidContent("Used extension name is invalid");
            extensions.push_back(extension.get<std::string>());
        }
    }
    if (value.contains("extensions")) {
        if (!value["extensions"].is_object()) {
            invalidContent("extensions is not an object");
        }
        for (const auto& extension : value["extensions"].items()) {
            extensions.push_back(extension.key());
        }
    }
    std::sort(extensions.begin(), extensions.end());
    extensions.erase(std::unique(extensions.begin(), extensions.end()),
                     extensions.end());
    if (extensions.size()
        > ProtocolLimits::kMaximumRequiredExtensionsPerResource) {
        resourceLimit("Used extension list exceeds its limit");
    }
    if (!std::includes(extensions.begin(), extensions.end(),
                       required.begin(), required.end())) {
        invalidContent("extensionsRequired is not a subset of extensionsUsed");
    }
    return extensions;
}

void addStringReference(const Json& object, const char* field,
                        ReferenceKind kind,
                        std::vector<RawReference>& output) {
    if (!object.contains(field)) return;
    if (!object[field].is_string()) invalidContent("Resource URI field is invalid");
    output.push_back({object[field].get<std::string>(), kind});
}

void extractTilesetReferences(const Json& document,
                              std::vector<RawReference>& output) {
    addStringReference(document, "schemaUri", ReferenceKind::metadata_schema,
                       output);
    if (document.contains("extensions") && document["extensions"].is_object()) {
        const auto extension = document["extensions"].find("3DTILES_metadata");
        if (extension != document["extensions"].end() && extension->is_object()) {
            addStringReference(*extension, "schemaUri",
                               ReferenceKind::metadata_schema, output);
        }
    }
    std::vector<const Json*> pending{&document.at("root")};
    while (!pending.empty()) {
        const Json* tile = pending.back();
        pending.pop_back();
        if (!tile->is_object()) invalidContent("Tileset tile is not an object");
        const bool implicit_tile = tile->contains("implicitTiling")
                || (tile->contains("extensions")
                    && tile->at("extensions").is_object()
                    && tile->at("extensions").contains(
                            "3DTILES_implicit_tiling"));
        if (tile->contains("content")) {
            const Json& content = tile->at("content");
            if (!content.is_object()) invalidContent("Tileset content is invalid");
            const bool uri = content.contains("uri");
            const bool url = content.contains("url");
            if (uri == url) invalidContent("Tileset content URI is ambiguous");
            addStringReference(content, uri ? "uri" : "url",
                    implicit_tile ? ReferenceKind::implicit_content_template
                                  : ReferenceKind::tileset_content,
                    output);
        }
        if (tile->contains("contents")) {
            if (!tile->at("contents").is_array()) {
                invalidContent("Tileset contents is not an array");
            }
            for (const auto& content : tile->at("contents")) {
                if (!content.is_object() || !content.contains("uri")) {
                    invalidContent("Tileset contents entry is invalid");
                }
                addStringReference(content, "uri",
                        implicit_tile ? ReferenceKind::implicit_content_template
                                      : ReferenceKind::tileset_content,
                        output);
            }
        }
        if (tile->contains("extensions") && tile->at("extensions").is_object()) {
            const auto multiple = tile->at("extensions").find(
                    "3DTILES_multiple_contents");
            if (multiple != tile->at("extensions").end()) {
                if (!multiple->is_object() || !multiple->contains("contents")
                    || !multiple->at("contents").is_array()) {
                    invalidContent("Legacy multiple contents extension is invalid");
                }
                for (const auto& content : multiple->at("contents")) {
                    if (!content.is_object() || !content.contains("uri")) {
                        invalidContent("Legacy multiple contents entry is invalid");
                    }
                    addStringReference(content, "uri",
                            implicit_tile
                                    ? ReferenceKind::implicit_content_template
                                    : ReferenceKind::tileset_content,
                            output);
                }
            }
        }
        if (tile->contains("implicitTiling")) {
            const Json& implicit = tile->at("implicitTiling");
            if (!implicit.is_object() || !implicit.contains("subtrees")
                || !implicit.at("subtrees").is_object()) {
                invalidContent("Implicit tiling subtree template is invalid");
            }
            addStringReference(implicit.at("subtrees"), "uri",
                               ReferenceKind::implicit_subtree_template, output);
        }
        if (tile->contains("children")) {
            if (!tile->at("children").is_array()) {
                invalidContent("Tileset children is not an array");
            }
            for (const auto& child : tile->at("children")) pending.push_back(&child);
        }
    }
}

void extractGltfReferences(const Json& document,
                           std::vector<RawReference>& output) {
    if (document.contains("buffers")) {
        if (!document.at("buffers").is_array()) invalidContent("glTF buffers is invalid");
        for (const auto& buffer : document.at("buffers")) {
            if (!buffer.is_object()) invalidContent("glTF buffer is invalid");
            addStringReference(buffer, "uri", ReferenceKind::gltf_buffer, output);
        }
    }
    if (document.contains("images")) {
        if (!document.at("images").is_array()) invalidContent("glTF images is invalid");
        for (const auto& image : document.at("images")) {
            if (!image.is_object()) invalidContent("glTF image is invalid");
            addStringReference(image, "uri", ReferenceKind::gltf_image, output);
        }
    }
    if (document.contains("extensions") && document.at("extensions").is_object()) {
        const auto extension = document.at("extensions").find("EXT_structural_metadata");
        if (extension != document.at("extensions").end() && extension->is_object()) {
            addStringReference(*extension, "schemaUri",
                               ReferenceKind::metadata_schema, output);
        }
    }
}

void extractSubtreeReferences(const Json& document,
                              std::vector<RawReference>& output) {
    if (!document.contains("buffers")) return;
    if (!document.at("buffers").is_array()) invalidContent("Subtree buffers is invalid");
    for (const auto& buffer : document.at("buffers")) {
        if (!buffer.is_object()) invalidContent("Subtree buffer is invalid");
        addStringReference(buffer, "uri", ReferenceKind::subtree_buffer, output);
    }
}

std::vector<std::uint8_t> jsonChunk(const std::vector<std::uint8_t>& bytes,
                                    std::uint64_t observed_size,
                                    std::size_t offset,
                                    std::uint64_t length) {
    if (length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        || offset > bytes.size() || length > bytes.size() - offset
        || length > observed_size - std::min<std::uint64_t>(observed_size, offset)) {
        invalidContent("Binary JSON chunk is truncated or outside bounds");
    }
    std::vector<std::uint8_t> result(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset
                    + static_cast<std::size_t>(length)));
    while (!result.empty()
           && (result.back() == 0U || result.back() == 0x20U)) {
        result.pop_back();
    }
    return result;
}

Json parseGlbJson(const std::vector<std::uint8_t>& bytes,
                  std::uint64_t observed_size, std::size_t offset,
                  const PipelineLimits& limits) {
    if (observed_size < offset + 20U || bytes.size() < offset + 20U
        || std::memcmp(bytes.data() + offset, "glTF", 4U) != 0
        || readLe32(bytes.data() + offset + 4U) != 2U
        || readLe32(bytes.data() + offset + 8U) != observed_size - offset) {
        invalidContent("GLB header is invalid");
    }
    const std::uint32_t json_length = readLe32(bytes.data() + offset + 12U);
    const std::uint32_t chunk_type = readLe32(bytes.data() + offset + 16U);
    if (chunk_type != kGlbJsonChunkType || json_length > limits.maximum_json_bytes) {
        invalidContent("GLB JSON chunk is invalid");
    }
    return parseJson(jsonChunk(bytes, observed_size, offset + 20U, json_length),
                     limits);
}

void validateComposite(const std::vector<std::uint8_t>& bytes,
                       std::uint64_t observed_size) {
    if (bytes.size() < 16U || observed_size < 16U
        || readLe32(bytes.data() + 4U) != 1U
        || readLe32(bytes.data() + 8U) != observed_size
        || readLe32(bytes.data() + 12U) == 0U) {
        invalidContent("CMPT header is invalid");
    }
    std::uint64_t cursor = 16U;
    const std::uint32_t count = readLe32(bytes.data() + 12U);
    for (std::uint32_t index = 0U; index < count; ++index) {
        if (cursor + 12U > observed_size || cursor + 12U > bytes.size()) {
            invalidContent("CMPT child header is truncated");
        }
        const std::uint32_t child_length = readLe32(
                bytes.data() + static_cast<std::size_t>(cursor) + 8U);
        if (child_length < 12U || child_length > observed_size - cursor) {
            invalidContent("CMPT child length is invalid");
        }
        cursor += child_length;
    }
    if (cursor != observed_size) invalidContent("CMPT length is inconsistent");
}

void classifyJson(Node& node, Json document) {
    const bool tileset = isTileset(document);
    const bool gltf = isGltf(document);
    const bool subtree = isSubtree(document);
    if (static_cast<int>(tileset) + static_cast<int>(gltf)
            + static_cast<int>(subtree) != 1) {
        invalidContent("JSON resource does not have one supported structure");
    }
    node.evidence.required_extensions = requiredExtensions(document);
    node.evidence.used_extensions = usedExtensions(
            document, node.evidence.required_extensions);
    node.document = std::move(document);
    if (tileset) {
        node.evidence.detected_kind = DetectedKind::tileset_json;
        node.evidence.detected_version =
                node.document->at("asset").at("version").get<std::string>();
        node.evidence.role = ResourceRole::tileset;
        extractTilesetReferences(*node.document, node.raw_references);
    } else if (gltf) {
        node.evidence.detected_kind = DetectedKind::gltf_json;
        node.evidence.detected_version =
                node.document->at("asset").at("version").get<std::string>();
        extractGltfReferences(*node.document, node.raw_references);
    } else {
        node.evidence.detected_kind = DetectedKind::subtree_json;
        node.evidence.detected_version = "1.0";
        node.evidence.role = ResourceRole::subtree;
        extractSubtreeReferences(*node.document, node.raw_references);
    }
}

bool hasMagic(const std::vector<std::uint8_t>& bytes, const char* magic) {
    return bytes.size() >= 4U && std::memcmp(bytes.data(), magic, 4U) == 0;
}

void classifyBinary(Node& node, const std::vector<std::uint8_t>& bytes,
                    const PipelineLimits& limits) {
    const std::uint64_t size = node.evidence.observed_size;
    if (hasMagic(bytes, "glTF")) {
        Json document = parseGlbJson(bytes, size, 0U, limits);
        if (!isGltf(document)) invalidContent("GLB JSON is not glTF 2.x");
        node.evidence.detected_kind = DetectedKind::glb;
        node.evidence.detected_version = "2";
        node.evidence.required_extensions = requiredExtensions(document);
        node.evidence.used_extensions = usedExtensions(
                document, node.evidence.required_extensions);
        extractGltfReferences(document, node.raw_references);
        return;
    }
    if (hasMagic(bytes, "b3dm") || hasMagic(bytes, "i3dm")
        || hasMagic(bytes, "pnts")) {
        const bool i3dm = hasMagic(bytes, "i3dm");
        const std::size_t header_size = i3dm ? 32U : 28U;
        if (bytes.size() < header_size || size < header_size
            || readLe32(bytes.data() + 4U) != 1U
            || readLe32(bytes.data() + 8U) != size) {
            invalidContent("Legacy tile header is invalid");
        }
        const std::uint64_t tables = static_cast<std::uint64_t>(
                readLe32(bytes.data() + 12U)) + readLe32(bytes.data() + 16U)
                + readLe32(bytes.data() + 20U) + readLe32(bytes.data() + 24U);
        if (tables > size - header_size) invalidContent("Legacy tile tables are invalid");
        const std::uint64_t payload = header_size + tables;
        node.evidence.detected_version = "1";
        if (hasMagic(bytes, "b3dm")) {
            node.evidence.detected_kind = DetectedKind::b3dm;
            Json document = parseGlbJson(bytes, size, static_cast<std::size_t>(payload), limits);
            if (!isGltf(document)) invalidContent("B3DM payload is not glTF 2.x");
            node.evidence.required_extensions = requiredExtensions(document);
            node.evidence.used_extensions = usedExtensions(
                    document, node.evidence.required_extensions);
            extractGltfReferences(document, node.raw_references);
        } else if (i3dm) {
            node.evidence.detected_kind = DetectedKind::i3dm;
            const std::uint32_t gltf_format = readLe32(bytes.data() + 28U);
            if (gltf_format == 1U) {
                Json document = parseGlbJson(
                        bytes, size, static_cast<std::size_t>(payload), limits);
                if (!isGltf(document)) invalidContent("I3DM payload is not glTF 2.x");
                node.evidence.required_extensions = requiredExtensions(document);
                node.evidence.used_extensions = usedExtensions(
                        document, node.evidence.required_extensions);
                extractGltfReferences(document, node.raw_references);
            } else if (gltf_format == 0U) {
                if (payload >= bytes.size()) invalidContent("I3DM glTF URI is missing");
                const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(payload);
                const auto end = std::find(begin, bytes.end(), 0U);
                const std::string uri(begin, end);
                if (uri.empty()) invalidContent("I3DM glTF URI is empty");
                node.raw_references.push_back({uri, ReferenceKind::i3dm_gltf});
            } else {
                invalidContent("I3DM glTF format is invalid");
            }
        } else {
            node.evidence.detected_kind = DetectedKind::pnts;
        }
        return;
    }
    if (hasMagic(bytes, "cmpt")) {
        validateComposite(bytes, size);
        node.evidence.detected_kind = DetectedKind::cmpt;
        node.evidence.detected_version = "1";
        return;
    }
    if (hasMagic(bytes, "subt")) {
        if (bytes.size() < 24U || size < 24U
            || readLe32(bytes.data() + 4U) != 1U) {
            invalidContent("Binary subtree header is invalid");
        }
        const std::uint64_t json_length = readLe64(bytes.data() + 8U);
        const std::uint64_t binary_length = readLe64(bytes.data() + 16U);
        if (json_length > limits.maximum_json_bytes
            || json_length > size - 24U
            || binary_length != size - 24U - json_length) {
            invalidContent("Binary subtree lengths are invalid");
        }
        Json document = parseJson(jsonChunk(bytes, size, 24U, json_length), limits);
        if (!isSubtree(document)) invalidContent("Binary subtree JSON is invalid");
        node.evidence.detected_kind = DetectedKind::subtree_binary;
        node.evidence.detected_version = "1";
        node.evidence.role = ResourceRole::subtree;
        node.evidence.required_extensions = requiredExtensions(document);
        node.evidence.used_extensions = usedExtensions(
                document, node.evidence.required_extensions);
        node.document = document;
        extractSubtreeReferences(document, node.raw_references);
        return;
    }
    static constexpr std::array<std::uint8_t, 8U> kPng =
            {0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    static constexpr std::array<std::uint8_t, 12U> kKtx2 =
            {0xabU, 0x4bU, 0x54U, 0x58U, 0x20U, 0x32U, 0x30U, 0xbbU,
             0x0dU, 0x0aU, 0x1aU, 0x0aU};
    const bool png = bytes.size() >= kPng.size()
            && std::equal(kPng.begin(), kPng.end(), bytes.begin());
    const bool jpeg = bytes.size() >= 3U && bytes[0] == 0xffU
            && bytes[1] == 0xd8U && bytes[2] == 0xffU;
    const bool webp = bytes.size() >= 12U
            && std::memcmp(bytes.data(), "RIFF", 4U) == 0
            && std::memcmp(bytes.data() + 8U, "WEBP", 4U) == 0;
    const bool ktx2 = bytes.size() >= kKtx2.size()
            && std::equal(kKtx2.begin(), kKtx2.end(), bytes.begin());
    node.evidence.detected_kind = png || jpeg || webp || ktx2
            ? DetectedKind::image : DetectedKind::binary;
}

std::vector<std::uint8_t> readClassificationBytes(
        const package::PackageEntryEvidence& entry,
        const PipelineLimits& limits, const ResourcePrefixReader& reader) {
    const std::uint64_t allowance = limits.maximum_json_bytes
            > std::numeric_limits<std::uint64_t>::max() - kBinaryHeaderReadBytes
            ? limits.maximum_json_bytes
            : limits.maximum_json_bytes + kBinaryHeaderReadBytes;
    const std::uint64_t maximum = std::min(entry.observed_expanded_bytes,
                                           allowance);
    std::vector<std::uint8_t> bytes = reader(entry, maximum);
    if (bytes.size() != static_cast<std::size_t>(maximum)) {
        invalidContent("Resource prefix reader returned an inconsistent length");
    }
    return bytes;
}

void classify(Node& node, const package::PackageEntryEvidence& entry,
              const PipelineLimits& limits, const ResourcePrefixReader& reader) {
    if (entry.entry_kind == package::PackageEntryKind::archive_container) {
        node.evidence.role = ResourceRole::package_archive;
        node.evidence.detected_kind = DetectedKind::binary;
        return;
    }
    std::vector<std::uint8_t> bytes = readClassificationBytes(entry, limits, reader);
    const auto first = std::find_if(bytes.begin(), bytes.end(), [](std::uint8_t value) {
        return std::isspace(value) == 0;
    });
    if (first != bytes.end() && *first == static_cast<std::uint8_t>('{')) {
        if (entry.observed_expanded_bytes > limits.maximum_json_bytes) {
            resourceLimit("JSON resource exceeds its byte limit");
        }
        classifyJson(node, parseJson(bytes, limits));
    } else {
        classifyBinary(node, bytes, limits);
    }
}

ResourceRole targetRole(ReferenceKind kind) {
    switch (kind) {
        case ReferenceKind::tileset_content:
        case ReferenceKind::i3dm_gltf: return ResourceRole::content;
        case ReferenceKind::implicit_content_template: return ResourceRole::content;
        case ReferenceKind::gltf_buffer:
        case ReferenceKind::subtree_buffer: return ResourceRole::buffer;
        case ReferenceKind::gltf_image: return ResourceRole::image;
        case ReferenceKind::metadata_schema: return ResourceRole::schema;
        case ReferenceKind::implicit_subtree_template: return ResourceRole::subtree;
    }
    return ResourceRole::other;
}

void assignRole(Node& node, ResourceRole role) {
    if (node.evidence.detected_kind == DetectedKind::tileset_json) {
        node.evidence.role = ResourceRole::tileset;
        return;
    }
    if (node.evidence.detected_kind == DetectedKind::subtree_json
        || node.evidence.detected_kind == DetectedKind::subtree_binary) {
        if (role != ResourceRole::subtree && role != ResourceRole::package_object) {
            invalidClosure("Resource is referenced with an incompatible role");
        }
        node.evidence.role = ResourceRole::subtree;
        return;
    }
    if (node.evidence.role == ResourceRole::package_object
        || node.evidence.role == ResourceRole::other) {
        node.evidence.role = role;
    } else if (node.evidence.role != role) {
        invalidClosure("Resource is referenced with incompatible roles");
    }
}

bool validateImplicitTemplate(const std::string& base_path,
                              const std::string& raw_uri) {
    static const std::set<std::string> kAllowed =
            {"{level}", "{x}", "{y}", "{z}"};
    std::string concrete;
    std::set<std::string> found;
    for (std::size_t index = 0U; index < raw_uri.size();) {
        if (raw_uri[index] != '{') {
            if (raw_uri[index] == '}') invalidResource("Implicit URI template is invalid");
            concrete.push_back(raw_uri[index++]);
            continue;
        }
        const std::size_t end = raw_uri.find('}', index);
        if (end == std::string::npos) invalidResource("Implicit URI template is invalid");
        const std::string token = raw_uri.substr(index, end - index + 1U);
        if (kAllowed.count(token) == 0U || !found.insert(token).second) {
            invalidResource("Implicit URI template placeholder is invalid");
        }
        concrete.push_back('0');
        index = end + 1U;
    }
    if (found.count("{level}") == 0U || found.count("{x}") == 0U
        || found.count("{y}") == 0U) {
        invalidResource("Implicit URI template is incomplete");
    }
    static_cast<void>(resolveRelativePath(base_path, concrete));
    return true;
}

std::string detectedKindName(DetectedKind kind) {
    switch (kind) {
        case DetectedKind::unknown: return "UNKNOWN";
        case DetectedKind::tileset_json: return "TILESET_JSON";
        case DetectedKind::gltf_json: return "GLTF_JSON";
        case DetectedKind::subtree_json: return "SUBTREE_JSON";
        case DetectedKind::glb: return "GLB";
        case DetectedKind::b3dm: return "B3DM";
        case DetectedKind::i3dm: return "I3DM";
        case DetectedKind::pnts: return "PNTS";
        case DetectedKind::cmpt: return "CMPT";
        case DetectedKind::subtree_binary: return "SUBTREE_BINARY";
        case DetectedKind::binary: return "BINARY";
        case DetectedKind::image: return "IMAGE";
    }
    return "UNKNOWN";
}

std::vector<std::string> mergeExtensions(
        const std::vector<std::string>& left,
        const std::vector<std::string>& right) {
    std::vector<std::string> result = left;
    result.insert(result.end(), right.begin(), right.end());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    if (result.size() > ProtocolLimits::kMaximumRequiredExtensionsPerResource) {
        resourceLimit("Merged extension evidence exceeds its limit");
    }
    return result;
}

std::vector<std::string> objectExtensionNames(const Json& value) {
    std::vector<std::string> result;
    if (!value.contains("extensions")) return result;
    if (!value.at("extensions").is_object()) {
        invalidContent("extensions is not an object");
    }
    for (const auto& extension : value.at("extensions").items()) {
        result.push_back(extension.key());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<double> identityTransform() {
    return {1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0};
}

std::vector<double> tileTransform(const Json& tile) {
    if (!tile.contains("transform")) return identityTransform();
    const Json& value = tile.at("transform");
    if (!value.is_array() || value.size() != 16U) {
        invalidContent("Tile transform must contain 16 numbers");
    }
    std::vector<double> result;
    result.reserve(16U);
    for (const auto& component : value) {
        if (!component.is_number() || !std::isfinite(component.get<double>())) {
            invalidContent("Tile transform contains a non-finite number");
        }
        result.push_back(component.get<double>());
    }
    return result;
}

std::vector<double> multiplyTransforms(const std::vector<double>& left,
                                       const std::vector<double>& right) {
    std::vector<double> result(16U, 0.0);
    for (std::size_t column = 0U; column < 4U; ++column) {
        for (std::size_t row = 0U; row < 4U; ++row) {
            for (std::size_t index = 0U; index < 4U; ++index) {
                result[column * 4U + row] += left[index * 4U + row]
                        * right[column * 4U + index];
            }
        }
    }
    return result;
}

struct ParsedBoundingVolume {
    BoundingVolumeType type = BoundingVolumeType::region;
    std::vector<double> values;
};

ParsedBoundingVolume boundingVolume(const Json& owner, const char* field) {
    if (!owner.contains(field) || !owner.at(field).is_object()) {
        invalidContent("Tile bounding volume is missing or invalid");
    }
    const Json& value = owner.at(field);
    const std::array<std::pair<const char*, std::size_t>, 3U> forms = {{
            {"region", 6U}, {"box", 12U}, {"sphere", 4U}}};
    std::size_t matches = 0U;
    ParsedBoundingVolume result;
    for (std::size_t index = 0U; index < forms.size(); ++index) {
        if (!value.contains(forms[index].first)) continue;
        ++matches;
        const Json& values = value.at(forms[index].first);
        if (!values.is_array() || values.size() != forms[index].second) {
            invalidContent("Bounding volume element count is invalid");
        }
        result.type = index == 0U ? BoundingVolumeType::region
                : index == 1U ? BoundingVolumeType::box
                              : BoundingVolumeType::sphere;
        result.values.clear();
        for (const auto& component : values) {
            if (!component.is_number() || !std::isfinite(component.get<double>())) {
                invalidContent("Bounding volume contains a non-finite number");
            }
            result.values.push_back(component.get<double>());
        }
    }
    if (matches != 1U) invalidContent("Bounding volume must use one core form");
    return result;
}

RefineMode refineMode(const Json& tile, RefineMode inherited) {
    if (!tile.contains("refine")) return inherited;
    if (!tile.at("refine").is_string()) invalidContent("Tile refine is invalid");
    std::string value = tile.at("refine").get<std::string>();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char item) { return static_cast<char>(std::toupper(item)); });
    if (value == "ADD") return RefineMode::add;
    if (value == "REPLACE") return RefineMode::replace;
    invalidContent("Tile refine is unsupported");
}

std::string contentUri(const Json& content) {
    if (!content.is_object()) invalidContent("Tile content entry is invalid");
    const bool uri = content.contains("uri");
    const bool url = content.contains("url");
    if (uri == url || !(uri ? content.at("uri") : content.at("url")).is_string()) {
        invalidContent("Tile content URI is ambiguous or invalid");
    }
    return (uri ? content.at("uri") : content.at("url")).get<std::string>();
}

std::vector<const Json*> tileContents(const Json& tile) {
    const Json* multiple = nullptr;
    if (tile.contains("contents")) multiple = &tile.at("contents");
    if (tile.contains("extensions") && tile.at("extensions").is_object()) {
        const auto legacy = tile.at("extensions").find("3DTILES_multiple_contents");
        if (legacy != tile.at("extensions").end()) {
            if (!legacy->is_object() || !legacy->contains("contents")) {
                invalidContent("Legacy multiple contents extension is invalid");
            }
            if (multiple != nullptr) {
                invalidContent("Core and legacy multiple contents are both present");
            }
            multiple = &legacy->at("contents");
        }
    }
    if (tile.contains("content") && multiple != nullptr) {
        invalidContent("Tile content and contents are mutually exclusive");
    }
    std::vector<const Json*> result;
    if (tile.contains("content")) result.push_back(&tile.at("content"));
    if (multiple != nullptr) {
        if (!multiple->is_array()) invalidContent("Tile contents is not an array");
        for (const auto& content : *multiple) result.push_back(&content);
    }
    return result;
}

ContentKind contentKind(DetectedKind kind) {
    switch (kind) {
        case DetectedKind::tileset_json: return ContentKind::external_tileset;
        case DetectedKind::gltf_json:
        case DetectedKind::glb:
        case DetectedKind::b3dm: return ContentKind::mesh;
        case DetectedKind::pnts: return ContentKind::point;
        case DetectedKind::i3dm: return ContentKind::instance;
        case DetectedKind::cmpt: return ContentKind::composite;
        case DetectedKind::unknown:
        case DetectedKind::binary:
        case DetectedKind::image: return ContentKind::unknown;
        default: return ContentKind::unsupported;
    }
}

ContentFormat contentFormat(DetectedKind kind) {
    switch (kind) {
        case DetectedKind::tileset_json: return ContentFormat::json;
        case DetectedKind::gltf_json: return ContentFormat::gltf;
        case DetectedKind::glb: return ContentFormat::glb;
        case DetectedKind::b3dm: return ContentFormat::b3dm;
        case DetectedKind::pnts: return ContentFormat::pnts;
        case DetectedKind::i3dm: return ContentFormat::i3dm;
        case DetectedKind::cmpt: return ContentFormat::cmpt;
        default: return ContentFormat::unknown;
    }
}

void appendLengthPrefixed(std::ostringstream& output, const std::string& value) {
    output << value.size() << ':' << value << '\n';
}

std::string contentClosureHash(const std::vector<Node>& nodes,
                               std::size_t root_index) {
    std::set<std::size_t> closure;
    std::vector<std::size_t> pending{root_index};
    while (!pending.empty()) {
        const std::size_t current = pending.back();
        pending.pop_back();
        if (!closure.insert(current).second) continue;
        pending.insert(pending.end(), nodes[current].dependencies.begin(),
                       nodes[current].dependencies.end());
    }
    std::vector<std::size_t> ordered(closure.begin(), closure.end());
    std::sort(ordered.begin(), ordered.end(), [&nodes](std::size_t left,
                                                       std::size_t right) {
        return nodes[left].evidence.object_id < nodes[right].evidence.object_id;
    });
    std::ostringstream canonical;
    canonical << kContentResourceClosureVersion << '\n';
    for (const std::size_t index : ordered) {
        const auto& evidence = nodes[index].evidence;
        appendLengthPrefixed(canonical, evidence.object_id);
        appendLengthPrefixed(canonical, evidence.observed_sha256);
        canonical << evidence.observed_size << '\n';
        appendLengthPrefixed(canonical, detectedKindName(evidence.detected_kind));
        appendLengthPrefixed(canonical,
                             evidence.detected_version.value_or(std::string{}));
        std::vector<std::string> dependency_ids;
        dependency_ids.reserve(nodes[index].dependencies.size());
        for (const std::size_t dependency : nodes[index].dependencies) {
            dependency_ids.push_back(nodes[dependency].evidence.object_id);
        }
        std::sort(dependency_ids.begin(), dependency_ids.end());
        canonical << dependency_ids.size() << '\n';
        for (const auto& dependency : dependency_ids) {
            appendLengthPrefixed(canonical, dependency);
        }
    }
    return sha256(canonical.str());
}

struct HierarchyInventory {
    std::vector<HierarchyRecord> records;
    std::uint64_t documents = 0U;
    std::uint64_t tiles = 0U;
    std::uint64_t subtrees = 0U;
    std::uint64_t contents = 0U;
    std::set<std::string> required_extensions;
    std::set<std::string> used_extensions;
};

struct InventoryContext {
    const InspectionRequest& request;
    std::vector<Node>& nodes;
    const std::map<std::string, std::size_t>& catalog;
    const std::map<std::string, const package::PackageEntryEvidence*>& entries;
    const PipelineLimits& limits;
    const ResourcePrefixReader& reader;
    const package::ContinuePredicate& should_continue;
    HierarchyInventory inventory;
};

void requireHierarchyCapacity(InventoryContext& context) {
    const std::uint64_t count = context.inventory.records.size();
    if (count >= context.limits.maximum_hierarchy_records) {
        resourceLimit("Hierarchy record count exceeds its limit");
    }
}

std::string stableId(const char* domain, const std::string& value) {
    return sha256(std::string(domain) + '\0' + value);
}

std::string hierarchyStableId(const InventoryContext& context,
                              const char* domain,
                              const std::string& value) {
    return stableId(domain, context.request.inspection_id + '\0'
            + context.request.request_id + '\0' + value);
}

void collectExtensions(HierarchyInventory& inventory,
                       const std::vector<std::string>& required,
                       const std::vector<std::string>& used) {
    inventory.required_extensions.insert(required.begin(), required.end());
    inventory.used_extensions.insert(used.begin(), used.end());
}

void inventoryDocument(InventoryContext& context, std::size_t resource_index,
                       const std::optional<std::string>& parent_document_id,
                       const std::optional<std::string>& parent_content_id,
                       const std::optional<std::uint32_t>& parent_content_ordinal,
                       std::uint32_t document_depth,
                       std::vector<std::size_t> ancestor_resources);
void inventoryImplicitHierarchy(
        InventoryContext& context, std::size_t document_resource,
        const std::string& document_id, const Json& tile,
        const std::string& pointer,
        const std::optional<std::string>& parent_tile_id,
        const std::vector<double>& parent_transform,
        RefineMode inherited_refine, std::uint32_t document_depth,
        const std::vector<std::size_t>& ancestor_resources);

void inventoryContent(InventoryContext& context, std::size_t document_resource,
                      const std::string& document_id, const std::string& tile_id,
                      const std::string& tile_pointer,
                      const Json& content, std::uint32_t ordinal,
                      std::uint32_t document_depth,
                      const std::vector<std::size_t>& ancestor_resources) {
    if (ordinal >= context.limits.maximum_contents_per_tile) {
        resourceLimit("Tile content count exceeds its limit");
    }
    const std::string raw_uri = contentUri(content);
    if (startsWithAsciiCaseInsensitive(raw_uri, "data:")) {
        invalidContent("A tile content data URI is not a hierarchy-addressable object");
    }
    const std::string resolved = resolveRelativePath(
            context.nodes[document_resource].evidence.package_relative_path, raw_uri);
    const auto found = context.catalog.find(resolved);
    if (found == context.catalog.end()) invalidClosure("Tile content resource is missing");
    const std::size_t source_index = found->second;
    const Node& source = context.nodes[source_index];
    const std::string content_id = hierarchyStableId(
            context, "three-d-tiles-content-v1",
            context.request.source_generation + '\0' + document_id + '\0'
            + tile_id + '\0' + std::to_string(ordinal));
    HierarchyRecord record;
    record.record_type = HierarchyRecordType::content;
    record.record_id = content_id;
    record.document_id = document_id;
    record.tile_id = tile_id;
    record.tile_json_pointer = tile_pointer;
    record.content_id = content_id;
    record.content_ordinal = ordinal;
    record.source_uri = source.evidence.package_relative_path;
    record.source_resource_object_id = source.evidence.object_id;
    record.content_kind = contentKind(source.evidence.detected_kind);
    record.content_format = contentFormat(source.evidence.detected_kind);
    record.resource_closure_version = kContentResourceClosureVersion;
    if (content.contains("group")) {
        if (!(content.at("group").is_string()
              || content.at("group").is_number_unsigned())) {
            invalidContent("Tile content group is invalid");
        }
        record.group_id = content.at("group").is_string()
                ? content.at("group").get<std::string>()
                : std::to_string(content.at("group").get<std::uint64_t>());
    }
    if (content.contains("boundingVolume")) {
        const auto volume = boundingVolume(content, "boundingVolume");
        record.content_bounding_volume_type = volume.type;
        record.content_bounding_volume = volume.values;
    }
    record.required_extensions = source.evidence.required_extensions;
    record.used_extensions = mergeExtensions(source.evidence.used_extensions,
                                              objectExtensionNames(content));
    if (source.evidence.detected_kind == DetectedKind::tileset_json) {
        inventoryDocument(context, source_index, document_id, content_id, ordinal,
                          document_depth + 1U, ancestor_resources);
    }
    // External Tilesets can discover implicit subtree dependencies recursively.
    // Hash only after that document instance has completed its inventory.
    record.resource_closure_hash = contentClosureHash(context.nodes, source_index);
    collectExtensions(context.inventory, record.required_extensions,
                      record.used_extensions);
    requireHierarchyCapacity(context);
    context.inventory.records.push_back(std::move(record));
    ++context.inventory.contents;
}

void inventoryExplicitTile(InventoryContext& context,
                           std::size_t document_resource,
                           const std::string& document_id,
                           const Json& tile, const std::string& pointer,
                           const std::optional<std::string>& parent_tile_id,
                           std::uint64_t level,
                           const std::vector<double>& parent_transform,
                           RefineMode inherited_refine,
                           std::uint32_t document_depth,
                           const std::vector<std::size_t>& ancestor_resources) {
    requireContinue(context.should_continue);
    if (!tile.is_object() || level > context.limits.maximum_tile_depth) {
        resourceLimit("Explicit tile depth exceeds its limit");
    }
    if (tile.contains("implicitTiling")
        || (tile.contains("extensions") && tile.at("extensions").is_object()
            && tile.at("extensions").contains("3DTILES_implicit_tiling"))) {
        inventoryImplicitHierarchy(context, document_resource, document_id,
                tile, pointer, parent_tile_id, parent_transform,
                inherited_refine, document_depth, ancestor_resources);
        return;
    }
    if (!tile.contains("geometricError")
        || !isNonNegativeNumber(tile.at("geometricError"))) {
        invalidContent("Tile geometricError is missing or invalid");
    }
    const ParsedBoundingVolume volume = boundingVolume(tile, "boundingVolume");
    const std::vector<double> effective_transform = multiplyTransforms(
            parent_transform, tileTransform(tile));
    const RefineMode effective_refine = refineMode(tile, inherited_refine);
    const std::string tile_id = hierarchyStableId(
            context, "three-d-tiles-explicit-tile-v1",
            context.request.source_generation + '\0' + document_id + '\0' + pointer);
    const std::vector<const Json*> contents = tileContents(tile);
    if (contents.size() > context.limits.maximum_contents_per_tile) {
        resourceLimit("Tile content count exceeds its limit");
    }
    bool has_children = false;
    if (tile.contains("children")) {
        if (!tile.at("children").is_array()) invalidContent("Tile children is invalid");
        has_children = !tile.at("children").empty();
    }
    HierarchyRecord record;
    record.record_type = HierarchyRecordType::explicit_tile;
    record.record_id = tile_id;
    record.document_id = document_id;
    record.tile_id = tile_id;
    record.parent_tile_id = parent_tile_id;
    record.tile_json_pointer = pointer;
    record.tile_level = level;
    record.transform = effective_transform;
    record.bounding_volume_type = volume.type;
    record.bounding_volume = volume.values;
    record.geometric_error = tile.at("geometricError").get<double>();
    record.refine = effective_refine;
    record.content_count = static_cast<std::uint32_t>(contents.size());
    record.has_children = has_children;
    record.implicit_root = false;
    record.required_extensions = {};
    record.used_extensions = objectExtensionNames(tile);
    collectExtensions(context.inventory, record.required_extensions,
                      record.used_extensions);
    requireHierarchyCapacity(context);
    context.inventory.records.push_back(std::move(record));
    ++context.inventory.tiles;
    for (std::size_t ordinal = 0U; ordinal < contents.size(); ++ordinal) {
        inventoryContent(context, document_resource, document_id, tile_id,
                         pointer, *contents[ordinal],
                         static_cast<std::uint32_t>(ordinal), document_depth,
                         ancestor_resources);
    }
    if (has_children) {
        for (std::size_t index = 0U; index < tile.at("children").size(); ++index) {
            inventoryExplicitTile(context, document_resource, document_id,
                    tile.at("children").at(index),
                    pointer + "/children/" + std::to_string(index), tile_id,
                    level + 1U, effective_transform, effective_refine,
                    document_depth, ancestor_resources);
        }
    }
}

constexpr std::uint64_t kMaximumAddressableImplicitLevel = 62U;

const Json& implicitTiling(const Json& tile) {
    if (tile.contains("implicitTiling")) {
        if (!tile.at("implicitTiling").is_object()) {
            invalidContent("Core implicitTiling is invalid");
        }
        return tile.at("implicitTiling");
    }
    if (!tile.contains("extensions") || !tile.at("extensions").is_object()) {
        invalidContent("Implicit tiling configuration is missing");
    }
    const auto found = tile.at("extensions").find("3DTILES_implicit_tiling");
    if (found == tile.at("extensions").end() || !found->is_object()) {
        invalidContent("Legacy implicit tiling extension is invalid");
    }
    return *found;
}

std::string templateUri(const Json& owner) {
    if (!owner.is_object()) invalidContent("Implicit URI template owner is invalid");
    const bool uri = owner.contains("uri");
    const bool url = owner.contains("url");
    if (uri == url || !(uri ? owner.at("uri") : owner.at("url")).is_string()) {
        invalidContent("Implicit URI template is invalid");
    }
    return (uri ? owner.at("uri") : owner.at("url")).get<std::string>();
}

void replaceToken(std::string& value, const char* token,
                  std::uint64_t replacement) {
    const std::string needle(token);
    const std::string text = std::to_string(replacement);
    std::size_t position = 0U;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), text);
        position += text.size();
    }
}

std::string instantiateTemplate(const std::string& raw,
                                std::uint64_t level, std::uint64_t x,
                                std::uint64_t y,
                                const std::optional<std::uint64_t>& z) {
    std::string result = raw;
    replaceToken(result, "{level}", level);
    replaceToken(result, "{x}", x);
    replaceToken(result, "{y}", y);
    if (z.has_value()) replaceToken(result, "{z}", *z);
    if (result.find('{') != std::string::npos
        || result.find('}') != std::string::npos) {
        invalidContent("Implicit URI template has unresolved placeholders");
    }
    return result;
}

std::string implicitTileId(const InventoryContext& context,
                           const std::string& document_id,
                           const std::string& pointer,
                           std::uint64_t level, std::uint64_t x,
                           std::uint64_t y,
                           const std::optional<std::uint64_t>& z) {
    return hierarchyStableId(context, "three-d-tiles-implicit-tile-v1",
            context.request.source_generation + '\0' + document_id + '\0'
            + pointer + '\0' + std::to_string(level) + '\0'
            + std::to_string(x) + '\0' + std::to_string(y) + '\0'
            + (z.has_value() ? std::to_string(*z) : std::string{}));
}

void addDependency(InventoryContext& context, std::size_t source,
                   std::size_t target) {
    auto& dependencies = context.nodes[source].dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), target)
        == dependencies.end()) {
        if (dependencies.size()
            >= ProtocolLimits::kMaximumDependenciesPerResource) {
            resourceLimit("Implicit subtree dependency count exceeds its limit");
        }
        dependencies.push_back(target);
    }
}

std::vector<std::uint8_t> readWholeResource(InventoryContext& context,
                                            std::size_t node_index,
                                            std::uint64_t maximum_bytes) {
    const Node& node = context.nodes[node_index];
    if (node.evidence.observed_size > maximum_bytes) {
        resourceLimit("Implicit subtree resource exceeds its byte limit");
    }
    const auto found = context.entries.find(node.evidence.package_relative_path);
    if (found == context.entries.end()) {
        invalidClosure("Implicit subtree buffer is not a package object");
    }
    auto bytes = context.reader(*found->second, node.evidence.observed_size);
    if (bytes.size() != node.evidence.observed_size) {
        invalidContent("Implicit subtree reader returned an inconsistent length");
    }
    return bytes;
}

struct SubtreePayload {
    Json document;
    std::vector<std::vector<std::uint8_t>> buffers;
};

SubtreePayload loadSubtreePayload(InventoryContext& context,
                                  std::size_t subtree_index) {
    Node& node = context.nodes[subtree_index];
    if ((node.evidence.detected_kind != DetectedKind::subtree_json
         && node.evidence.detected_kind != DetectedKind::subtree_binary)
        || !node.document.has_value()) {
        invalidContent("Implicit subtree resource has an invalid type");
    }
    SubtreePayload payload;
    payload.document = *node.document;
    std::vector<std::uint8_t> internal_binary;
    if (node.evidence.detected_kind == DetectedKind::subtree_binary) {
        const auto bytes = readWholeResource(context, subtree_index,
                                             context.limits.maximum_subtree_bytes);
        const std::uint64_t json_length = readLe64(bytes.data() + 8U);
        const std::uint64_t binary_length = readLe64(bytes.data() + 16U);
        const std::size_t binary_offset = 24U + static_cast<std::size_t>(json_length);
        internal_binary.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(binary_offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(binary_offset
                        + static_cast<std::size_t>(binary_length)));
    }
    if (!payload.document.contains("buffers")) {
        if (!internal_binary.empty()) {
            invalidContent("Binary subtree payload has no buffer declaration");
        }
        return payload;
    }
    const Json& buffers = payload.document.at("buffers");
    if (!buffers.is_array()) invalidContent("Subtree buffers is invalid");
    payload.buffers.reserve(buffers.size());
    bool internal_consumed = false;
    for (const auto& buffer : buffers) {
        if (!buffer.is_object() || !buffer.contains("byteLength")
            || !buffer.at("byteLength").is_number_unsigned()) {
            invalidContent("Subtree buffer declaration is invalid");
        }
        const std::uint64_t declared = buffer.at("byteLength").get<std::uint64_t>();
        std::vector<std::uint8_t> bytes;
        if (buffer.contains("uri")) {
            if (!buffer.at("uri").is_string()) {
                invalidContent("Subtree buffer URI is invalid");
            }
            const std::string uri = buffer.at("uri").get<std::string>();
            if (startsWithAsciiCaseInsensitive(uri, "data:")) {
                bytes = decodeDataUri(uri, context.limits.maximum_data_uri_bytes).bytes;
            } else {
                const std::string resolved = resolveRelativePath(
                        node.evidence.package_relative_path, uri);
                const auto target = context.catalog.find(resolved);
                if (target == context.catalog.end()) {
                    invalidClosure("Subtree buffer resource is missing");
                }
                addDependency(context, subtree_index, target->second);
                bytes = readWholeResource(context, target->second,
                                          context.limits.maximum_subtree_bytes);
            }
        } else {
            if (node.evidence.detected_kind != DetectedKind::subtree_binary
                || internal_consumed) {
                invalidContent("Subtree internal buffer declaration is invalid");
            }
            bytes = internal_binary;
            internal_consumed = true;
        }
        if (bytes.size() < declared) {
            invalidContent("Subtree buffer is shorter than byteLength");
        }
        bytes.resize(static_cast<std::size_t>(declared));
        payload.buffers.push_back(std::move(bytes));
    }
    return payload;
}

std::vector<std::uint8_t> bufferViewBytes(const SubtreePayload& payload,
                                          std::uint32_t view_index) {
    if (!payload.document.contains("bufferViews")
        || !payload.document.at("bufferViews").is_array()
        || view_index >= payload.document.at("bufferViews").size()) {
        invalidContent("Subtree availability bufferView is missing");
    }
    const Json& view = payload.document.at("bufferViews").at(view_index);
    if (!view.is_object() || !view.contains("buffer")
        || !view.at("buffer").is_number_unsigned()
        || !view.contains("byteLength")
        || !view.at("byteLength").is_number_unsigned()) {
        invalidContent("Subtree bufferView is invalid");
    }
    const std::uint64_t buffer = view.at("buffer").get<std::uint64_t>();
    const std::uint64_t offset = view.value("byteOffset", 0ULL);
    const std::uint64_t length = view.at("byteLength").get<std::uint64_t>();
    if (buffer >= payload.buffers.size()
        || offset > payload.buffers[static_cast<std::size_t>(buffer)].size()
        || length > payload.buffers[static_cast<std::size_t>(buffer)].size() - offset) {
        invalidContent("Subtree bufferView is outside its buffer");
    }
    const auto& source = payload.buffers[static_cast<std::size_t>(buffer)];
    return std::vector<std::uint8_t>(
            source.begin() + static_cast<std::ptrdiff_t>(offset),
            source.begin() + static_cast<std::ptrdiff_t>(offset + length));
}

struct Availability {
    std::optional<bool> constant;
    std::vector<std::uint8_t> bits;

    [[nodiscard]] bool at(std::uint64_t index) const {
        if (constant.has_value()) return *constant;
        return (bits[static_cast<std::size_t>(index / 8U)]
                & static_cast<std::uint8_t>(1U << (index % 8U))) != 0U;
    }
};

Availability parseAvailability(const Json& value,
                               const SubtreePayload& payload,
                               std::uint64_t expected_bits) {
    if (!value.is_object()) invalidContent("Subtree availability is invalid");
    const bool has_constant = value.contains("constant");
    const bool has_bitstream = value.contains("bitstream");
    if (has_constant == has_bitstream) {
        invalidContent("Subtree availability must use constant or bitstream");
    }
    Availability result;
    if (has_constant) {
        if (!value.at("constant").is_number_unsigned()) {
            invalidContent("Subtree availability constant is invalid");
        }
        const auto constant = value.at("constant").get<std::uint32_t>();
        if (constant > 1U) invalidContent("Subtree availability constant is invalid");
        result.constant = constant == 1U;
    } else {
        if (!value.at("bitstream").is_number_unsigned()) {
            invalidContent("Subtree availability bitstream is invalid");
        }
        result.bits = bufferViewBytes(
                payload, value.at("bitstream").get<std::uint32_t>());
        const std::uint64_t required_bytes = (expected_bits + 7U) / 8U;
        if (result.bits.size() < required_bytes) {
            invalidContent("Subtree availability bitstream is truncated");
        }
    }
    std::uint64_t observed = 0U;
    for (std::uint64_t index = 0U; index < expected_bits; ++index) {
        if (result.at(index)) ++observed;
    }
    if (value.contains("availableCount")) {
        if (!value.at("availableCount").is_number_unsigned()
            || value.at("availableCount").get<std::uint64_t>() != observed) {
            invalidContent("Subtree availableCount is inconsistent");
        }
    }
    return result;
}

std::uint64_t checkedPower(std::uint64_t base, std::uint32_t exponent,
                           std::uint64_t maximum) {
    std::uint64_t value = 1U;
    for (std::uint32_t index = 0U; index < exponent; ++index) {
        if (value > maximum / base) {
            resourceLimit("Implicit subtree fanout exceeds its limit");
        }
        value *= base;
    }
    return value;
}

std::uint64_t subtreeTileCount(std::uint64_t branching,
                               std::uint32_t levels,
                               std::uint64_t maximum) {
    std::uint64_t total = 0U;
    std::uint64_t level_count = 1U;
    for (std::uint32_t level = 0U; level < levels; ++level) {
        if (total > maximum - level_count) {
            resourceLimit("Implicit subtree tile count exceeds its limit");
        }
        total += level_count;
        if (level + 1U < levels) {
            if (level_count > maximum / branching) {
                resourceLimit("Implicit subtree tile count exceeds its limit");
            }
            level_count *= branching;
        }
    }
    return total;
}

std::uint64_t levelOffset(std::uint64_t branching, std::uint32_t level) {
    return subtreeTileCount(branching, level,
                            ProtocolLimits::kMaximumAvailableTileCount);
}

struct LocalCoordinates {
    std::uint64_t x = 0U;
    std::uint64_t y = 0U;
    std::uint64_t z = 0U;
};

LocalCoordinates decodeMorton(std::uint64_t morton,
                              std::uint32_t level,
                              SubdivisionScheme scheme) {
    LocalCoordinates result;
    const std::uint32_t dimensions = scheme == SubdivisionScheme::octree ? 3U : 2U;
    for (std::uint32_t bit = 0U; bit < level; ++bit) {
        result.x |= ((morton >> (dimensions * bit)) & 1U) << bit;
        result.y |= ((morton >> (dimensions * bit + 1U)) & 1U) << bit;
        if (dimensions == 3U) {
            result.z |= ((morton >> (dimensions * bit + 2U)) & 1U) << bit;
        }
    }
    return result;
}

std::uint64_t mortonChild(std::uint64_t parent, std::uint64_t child,
                          std::uint32_t parent_level,
                          SubdivisionScheme scheme) {
    const std::uint32_t dimensions = scheme == SubdivisionScheme::octree ? 3U : 2U;
    return parent | (child << (dimensions * parent_level));
}

std::uint64_t mortonParent(std::uint64_t child, std::uint32_t child_level,
                           SubdivisionScheme scheme) {
    if (child_level == 0U) return 0U;
    const std::uint32_t dimensions = scheme == SubdivisionScheme::octree ? 3U : 2U;
    const std::uint32_t bits = dimensions * (child_level - 1U);
    return bits == 0U ? 0U : child & ((1ULL << bits) - 1ULL);
}

ParsedBoundingVolume subdivideBoundingVolume(
        const ParsedBoundingVolume& root, SubdivisionScheme scheme,
        std::uint64_t level, std::uint64_t x, std::uint64_t y,
        const std::optional<std::uint64_t>& z) {
    if (root.type == BoundingVolumeType::sphere) {
        invalidContent("Implicit tiling with a sphere bounding volume is unsupported");
    }
    const double scale = std::ldexp(1.0, -static_cast<int>(level));
    ParsedBoundingVolume result = root;
    if (root.type == BoundingVolumeType::region) {
        const double west = root.values[0U];
        const double south = root.values[1U];
        const double east = root.values[2U];
        const double north = root.values[3U];
        result.values[0U] = west + (east - west) * static_cast<double>(x) * scale;
        result.values[2U] = west + (east - west) * static_cast<double>(x + 1U) * scale;
        result.values[1U] = south + (north - south) * static_cast<double>(y) * scale;
        result.values[3U] = south + (north - south) * static_cast<double>(y + 1U) * scale;
        if (scheme == SubdivisionScheme::octree) {
            const double minimum = root.values[4U];
            const double maximum = root.values[5U];
            result.values[4U] = minimum + (maximum - minimum)
                    * static_cast<double>(*z) * scale;
            result.values[5U] = minimum + (maximum - minimum)
                    * static_cast<double>(*z + 1U) * scale;
        }
        return result;
    }
    const std::array<std::uint64_t, 3U> coordinates = {x, y, z.value_or(0U)};
    result.values.assign(root.values.begin(), root.values.end());
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const bool split = axis < 2U || scheme == SubdivisionScheme::octree;
        if (!split) continue;
        const double center_offset = (2.0 * static_cast<double>(coordinates[axis]) + 1.0)
                * scale - 1.0;
        for (std::size_t component = 0U; component < 3U; ++component) {
            result.values[component] += root.values[3U + axis * 3U + component]
                    * center_offset;
            result.values[3U + axis * 3U + component] =
                    root.values[3U + axis * 3U + component] * scale;
        }
    }
    return result;
}

struct SubtreeCoordinates {
    std::uint64_t level = 0U;
    std::uint64_t x = 0U;
    std::uint64_t y = 0U;
    std::optional<std::uint64_t> z;
    std::optional<std::string> parent_tile_id;
    std::size_t parent_resource = 0U;
};

void inventoryImplicitContent(
        InventoryContext& context, std::size_t document_resource,
        const std::string& document_id, const std::string& tile_id,
        const Json& content_template, std::uint32_t ordinal,
        std::size_t subtree_resource,
        SubdivisionScheme scheme, std::uint64_t level,
        std::uint64_t x, std::uint64_t y,
        const std::optional<std::uint64_t>& z,
        std::uint32_t document_depth,
        const std::vector<std::size_t>& ancestor_resources) {
    const std::string raw = contentUri(content_template);
    static_cast<void>(validateImplicitTemplate(
            context.nodes[document_resource].evidence.package_relative_path, raw));
    const std::string concrete = instantiateTemplate(raw, level, x, y, z);
    const std::string resolved = resolveRelativePath(
            context.nodes[document_resource].evidence.package_relative_path,
            concrete);
    const auto found = context.catalog.find(resolved);
    if (found == context.catalog.end()) {
        invalidClosure("Available implicit content resource is missing");
    }
    const std::size_t source_index = found->second;
    addDependency(context, subtree_resource, source_index);
    const Node& source = context.nodes[source_index];
    const std::string content_id = hierarchyStableId(
            context, "three-d-tiles-content-v1",
            context.request.source_generation + '\0' + document_id + '\0'
            + tile_id + '\0' + std::to_string(ordinal));
    HierarchyRecord record;
    record.record_type = HierarchyRecordType::content;
    record.record_id = content_id;
    record.document_id = document_id;
    record.tile_id = tile_id;
    record.tile_level = level;
    record.implicit_x = x;
    record.implicit_y = y;
    record.implicit_z = z;
    record.content_id = content_id;
    record.content_ordinal = ordinal;
    record.content_subdivision_scheme = scheme;
    record.source_uri = source.evidence.package_relative_path;
    record.source_resource_object_id = source.evidence.object_id;
    record.content_kind = contentKind(source.evidence.detected_kind);
    record.content_format = contentFormat(source.evidence.detected_kind);
    record.resource_closure_version = kContentResourceClosureVersion;
    if (content_template.contains("group")) {
        if (!(content_template.at("group").is_string()
              || content_template.at("group").is_number_unsigned())) {
            invalidContent("Implicit content group is invalid");
        }
        record.group_id = content_template.at("group").is_string()
                ? content_template.at("group").get<std::string>()
                : std::to_string(content_template.at("group").get<std::uint64_t>());
    }
    record.required_extensions = source.evidence.required_extensions;
    record.used_extensions = mergeExtensions(source.evidence.used_extensions,
            objectExtensionNames(content_template));
    if (source.evidence.detected_kind == DetectedKind::tileset_json) {
        inventoryDocument(context, source_index, document_id, content_id, ordinal,
                          document_depth + 1U, ancestor_resources);
    }
    // Recursion may add implicit dependencies to an external Tileset resource.
    record.resource_closure_hash = contentClosureHash(context.nodes, source_index);
    collectExtensions(context.inventory, record.required_extensions,
                      record.used_extensions);
    requireHierarchyCapacity(context);
    context.inventory.records.push_back(std::move(record));
    ++context.inventory.contents;
}

void inventoryImplicitHierarchy(
        InventoryContext& context, std::size_t document_resource,
        const std::string& document_id, const Json& tile,
        const std::string& pointer,
        const std::optional<std::string>& parent_tile_id,
        const std::vector<double>& parent_transform,
        RefineMode inherited_refine, std::uint32_t document_depth,
        const std::vector<std::size_t>& ancestor_resources) {
    const Json& implicit = implicitTiling(tile);
    if (!implicit.contains("subdivisionScheme")
        || !implicit.at("subdivisionScheme").is_string()
        || !implicit.contains("subtreeLevels")
        || !implicit.at("subtreeLevels").is_number_unsigned()
        || !implicit.contains("availableLevels")
        || !implicit.at("availableLevels").is_number_unsigned()
        || !implicit.contains("subtrees")) {
        invalidContent("Implicit tiling configuration is incomplete");
    }
    const std::string scheme_name = implicit.at("subdivisionScheme").get<std::string>();
    const SubdivisionScheme scheme = scheme_name == "QUADTREE"
            ? SubdivisionScheme::quadtree
            : scheme_name == "OCTREE" ? SubdivisionScheme::octree
                                        : throw InspectionPipelineError(
                                                PipelineFailureKind::invalid,
                                                DiagnosticCode::content_invalid,
                                                InspectorStage::structure_classification,
                                                "Implicit subdivision scheme is invalid");
    const std::uint32_t subtree_levels =
            implicit.at("subtreeLevels").get<std::uint32_t>();
    const std::uint64_t available_levels =
            implicit.at("availableLevels").get<std::uint64_t>();
    if (subtree_levels == 0U || available_levels == 0U
        || available_levels > kMaximumAddressableImplicitLevel
        || subtree_levels > available_levels) {
        resourceLimit("Implicit level configuration exceeds addressable bounds");
    }
    const std::string subtree_template = templateUri(implicit.at("subtrees"));
    static_cast<void>(validateImplicitTemplate(
            context.nodes[document_resource].evidence.package_relative_path,
            subtree_template));
    if (scheme == SubdivisionScheme::octree
        && subtree_template.find("{z}") == std::string::npos) {
        invalidContent("Octree subtree template is missing {z}");
    }
    const std::vector<const Json*> content_templates = tileContents(tile);
    if (content_templates.size() > context.limits.maximum_contents_per_tile) {
        resourceLimit("Implicit content stream count exceeds its limit");
    }
    for (const Json* content : content_templates) {
        static_cast<void>(validateImplicitTemplate(
                context.nodes[document_resource].evidence.package_relative_path,
                contentUri(*content)));
        if (scheme == SubdivisionScheme::octree
            && contentUri(*content).find("{z}") == std::string::npos) {
            invalidContent("Octree content template is missing {z}");
        }
    }
    if (!tile.contains("geometricError")
        || !isNonNegativeNumber(tile.at("geometricError"))) {
        invalidContent("Implicit root geometricError is invalid");
    }
    const ParsedBoundingVolume root_volume = boundingVolume(tile, "boundingVolume");
    const std::vector<double> effective_transform = multiplyTransforms(
            parent_transform, tileTransform(tile));
    const RefineMode effective_refine = refineMode(tile, inherited_refine);
    const std::uint64_t branching = scheme == SubdivisionScheme::octree ? 8U : 4U;
    const std::uint64_t tile_slots = subtreeTileCount(
            branching, subtree_levels, context.limits.maximum_available_tiles);
    const std::uint64_t child_slots = checkedPower(
            branching, subtree_levels, context.limits.maximum_available_tiles);
    std::queue<SubtreeCoordinates> pending;
    pending.push({0U, 0U, 0U,
                  scheme == SubdivisionScheme::octree
                          ? std::optional<std::uint64_t>(0U) : std::nullopt,
                  parent_tile_id, document_resource});
    std::set<std::tuple<std::uint64_t, std::uint64_t,
                        std::uint64_t, std::uint64_t>> visited;
    while (!pending.empty()) {
        requireContinue(context.should_continue);
        const SubtreeCoordinates coordinates = pending.front();
        pending.pop();
        const auto key = std::make_tuple(coordinates.level, coordinates.x,
                coordinates.y, coordinates.z.value_or(0U));
        if (!visited.insert(key).second) {
            invalidClosure("Implicit subtree coordinate cycle detected");
        }
        if (context.inventory.subtrees >= context.limits.maximum_subtrees) {
            resourceLimit("Implicit subtree count exceeds its limit");
        }
        const std::string concrete_subtree = instantiateTemplate(
                subtree_template, coordinates.level, coordinates.x,
                coordinates.y, coordinates.z);
        const std::string subtree_path = resolveRelativePath(
                context.nodes[document_resource].evidence.package_relative_path,
                concrete_subtree);
        const auto subtree_found = context.catalog.find(subtree_path);
        if (subtree_found == context.catalog.end()) {
            invalidClosure("Available implicit subtree resource is missing");
        }
        const std::size_t subtree_index = subtree_found->second;
        addDependency(context, coordinates.parent_resource, subtree_index);
        const SubtreePayload payload = loadSubtreePayload(context, subtree_index);
        const Availability tile_availability = parseAvailability(
                payload.document.at("tileAvailability"), payload, tile_slots);
        if (!tile_availability.at(0U)) {
            invalidContent("Implicit subtree root tile is unavailable");
        }
        std::vector<Availability> content_availability;
        if (payload.document.contains("contentAvailability")) {
            const Json& content_value = payload.document.at("contentAvailability");
            if (content_value.is_array()) {
                for (const auto& availability : content_value) {
                    content_availability.push_back(parseAvailability(
                            availability, payload, tile_slots));
                }
            } else {
                content_availability.push_back(parseAvailability(
                        content_value, payload, tile_slots));
            }
        }
        if (content_availability.size() != content_templates.size()) {
            invalidContent("Implicit content availability stream count is inconsistent");
        }
        for (const auto& availability : content_availability) {
            for (std::uint64_t index = 0U; index < tile_slots; ++index) {
                if (availability.at(index) && !tile_availability.at(index)) {
                    invalidContent("Available implicit content has an unavailable tile");
                }
            }
        }
        const Availability child_availability = parseAvailability(
                payload.document.at("childSubtreeAvailability"), payload,
                child_slots);
        std::uint64_t available_tile_count = 0U;
        std::uint64_t available_content_count = 0U;
        std::uint64_t available_child_count = 0U;
        for (std::uint64_t child = 0U; child < child_slots; ++child) {
            if (child_availability.at(child)) ++available_child_count;
        }
        const std::string subtree_root_tile_id = implicitTileId(
                context, document_id, pointer, coordinates.level,
                coordinates.x, coordinates.y, coordinates.z);
        for (std::uint32_t local_level = 0U;
             local_level < subtree_levels; ++local_level) {
            const std::uint64_t level_count = checkedPower(
                    branching, local_level,
                    context.limits.maximum_available_tiles);
            const std::uint64_t offset = levelOffset(branching, local_level);
            for (std::uint64_t morton = 0U; morton < level_count; ++morton) {
                const std::uint64_t availability_index = offset + morton;
                if (!tile_availability.at(availability_index)) continue;
                if (coordinates.level + local_level >= available_levels) {
                    invalidContent("Tile availability exceeds availableLevels");
                }
                if (context.inventory.tiles >= context.limits.maximum_available_tiles) {
                    resourceLimit("Available implicit tile count exceeds its limit");
                }
                const LocalCoordinates local = decodeMorton(morton, local_level, scheme);
                const std::uint64_t level = coordinates.level + local_level;
                const std::uint64_t x = (coordinates.x << local_level) | local.x;
                const std::uint64_t y = (coordinates.y << local_level) | local.y;
                const std::optional<std::uint64_t> z = scheme == SubdivisionScheme::octree
                        ? std::optional<std::uint64_t>(
                                (*coordinates.z << local_level) | local.z)
                        : std::nullopt;
                const std::string tile_id = implicitTileId(
                        context, document_id, pointer, level, x, y, z);
                std::optional<std::string> parent;
                if (local_level == 0U) {
                    parent = coordinates.parent_tile_id;
                } else {
                    parent = implicitTileId(context, document_id, pointer,
                            level - 1U, x >> 1U, y >> 1U,
                            z.has_value()
                                    ? std::optional<std::uint64_t>(*z >> 1U)
                                    : std::nullopt);
                    if (!tile_availability.at(levelOffset(branching, local_level - 1U)
                            + mortonParent(morton, local_level, scheme))) {
                        invalidContent("Available implicit tile has an unavailable parent");
                    }
                }
                std::uint32_t content_count = 0U;
                for (const auto& availability : content_availability) {
                    if (availability.at(availability_index)) ++content_count;
                }
                bool has_children = false;
                if (local_level + 1U < subtree_levels) {
                    for (std::uint64_t child = 0U; child < branching; ++child) {
                        const std::uint64_t child_morton = mortonChild(
                                morton, child, local_level, scheme);
                        if (tile_availability.at(levelOffset(branching, local_level + 1U)
                                + child_morton)) {
                            has_children = true;
                        }
                    }
                } else {
                    for (std::uint64_t child = 0U; child < branching; ++child) {
                        if (child_availability.at(mortonChild(
                                    morton, child, local_level, scheme))) {
                            has_children = true;
                        }
                    }
                }
                const ParsedBoundingVolume volume = subdivideBoundingVolume(
                        root_volume, scheme, level, x, y, z);
                HierarchyRecord record;
                record.record_type = HierarchyRecordType::implicit_tile;
                record.record_id = tile_id;
                record.document_id = document_id;
                record.tile_id = tile_id;
                record.parent_tile_id = parent;
                record.tile_level = level;
                record.implicit_x = x;
                record.implicit_y = y;
                record.implicit_z = z;
                record.transform = effective_transform;
                record.bounding_volume_type = volume.type;
                record.bounding_volume = volume.values;
                record.geometric_error = tile.at("geometricError").get<double>()
                        * std::ldexp(1.0, -static_cast<int>(level));
                record.refine = effective_refine;
                record.content_count = content_count;
                record.has_children = has_children;
                record.implicit_root = level == 0U;
                record.required_extensions = {};
                record.used_extensions = mergeExtensions(
                        objectExtensionNames(tile),
                        {"3DTILES_implicit_tiling"});
                collectExtensions(context.inventory, record.required_extensions,
                                  record.used_extensions);
                requireHierarchyCapacity(context);
                context.inventory.records.push_back(std::move(record));
                ++context.inventory.tiles;
                ++available_tile_count;
                for (std::size_t ordinal = 0U;
                     ordinal < content_availability.size(); ++ordinal) {
                    if (!content_availability[ordinal].at(availability_index)) continue;
                    inventoryImplicitContent(context, document_resource, document_id,
                            tile_id, *content_templates[ordinal],
                            static_cast<std::uint32_t>(ordinal), subtree_index,
                            scheme,
                            level, x, y, z, document_depth, ancestor_resources);
                    ++available_content_count;
                }
            }
        }
        HierarchyRecord subtree_record;
        subtree_record.record_type = HierarchyRecordType::implicit_subtree;
        subtree_record.record_id = hierarchyStableId(
                context, "three-d-tiles-implicit-subtree-v1",
                context.request.source_generation + '\0' + document_id + '\0'
                + pointer + '\0' + std::to_string(coordinates.level) + '\0'
                + std::to_string(coordinates.x) + '\0'
                + std::to_string(coordinates.y) + '\0'
                + (coordinates.z.has_value()
                        ? std::to_string(*coordinates.z) : std::string{}));
        subtree_record.subtree_id = subtree_record.record_id;
        subtree_record.document_id = document_id;
        subtree_record.tile_id = subtree_root_tile_id;
        subtree_record.resource_object_id =
                context.nodes[subtree_index].evidence.object_id;
        subtree_record.subdivision_scheme = scheme;
        subtree_record.subtree_levels = subtree_levels;
        subtree_record.available_levels = available_levels;
        subtree_record.tile_level = coordinates.level;
        subtree_record.implicit_x = coordinates.x;
        subtree_record.implicit_y = coordinates.y;
        subtree_record.implicit_z = coordinates.z;
        subtree_record.available_tile_count = available_tile_count;
        subtree_record.available_content_count = available_content_count;
        subtree_record.available_child_subtree_count = available_child_count;
        subtree_record.content_stream_count =
                static_cast<std::uint32_t>(content_templates.size());
        subtree_record.required_extensions =
                context.nodes[subtree_index].evidence.required_extensions;
        subtree_record.used_extensions =
                context.nodes[subtree_index].evidence.used_extensions;
        if (containsUnsupportedSubtreeMetadata(payload.document)) {
            subtree_record.used_extensions = mergeExtensions(
                    subtree_record.used_extensions, {kMetadataExtension});
        }
        collectExtensions(context.inventory, subtree_record.required_extensions,
                          subtree_record.used_extensions);
        requireHierarchyCapacity(context);
        context.inventory.records.push_back(std::move(subtree_record));
        ++context.inventory.subtrees;
        if (coordinates.level + subtree_levels < available_levels) {
            for (std::uint64_t child = 0U; child < child_slots; ++child) {
                if (!child_availability.at(child)) continue;
                const std::uint64_t parent_morton = mortonParent(
                        child, subtree_levels, scheme);
                if (!tile_availability.at(levelOffset(
                            branching, subtree_levels - 1U) + parent_morton)) {
                    invalidContent("Available child subtree has an unavailable parent tile");
                }
                const LocalCoordinates local = decodeMorton(
                        child, subtree_levels, scheme);
                const std::uint64_t next_level = coordinates.level + subtree_levels;
                const std::uint64_t next_x =
                        (coordinates.x << subtree_levels) | local.x;
                const std::uint64_t next_y =
                        (coordinates.y << subtree_levels) | local.y;
                const std::optional<std::uint64_t> next_z =
                        scheme == SubdivisionScheme::octree
                        ? std::optional<std::uint64_t>(
                                (*coordinates.z << subtree_levels) | local.z)
                        : std::nullopt;
                pending.push({next_level, next_x, next_y, next_z,
                              implicitTileId(context, document_id, pointer,
                                      next_level - 1U, next_x >> 1U,
                                      next_y >> 1U,
                                      next_z.has_value()
                                              ? std::optional<std::uint64_t>(
                                                        *next_z >> 1U)
                                              : std::nullopt),
                              subtree_index});
            }
        } else if (available_child_count != 0U) {
            invalidContent("Child subtree availability exceeds availableLevels");
        }
    }
}

void inventoryDocument(InventoryContext& context, std::size_t resource_index,
                       const std::optional<std::string>& parent_document_id,
                       const std::optional<std::string>& parent_content_id,
                       const std::optional<std::uint32_t>& parent_content_ordinal,
                       std::uint32_t document_depth,
                       std::vector<std::size_t> ancestor_resources) {
    requireContinue(context.should_continue);
    if (document_depth > context.limits.maximum_tile_depth
        || context.inventory.documents >= context.limits.maximum_documents) {
        resourceLimit("Tileset document instance count or depth exceeds its limit");
    }
    if (std::find(ancestor_resources.begin(), ancestor_resources.end(), resource_index)
        != ancestor_resources.end()) {
        invalidClosure("External Tileset ancestor cycle detected");
    }
    ancestor_resources.push_back(resource_index);
    Node& node = context.nodes[resource_index];
    if (node.evidence.detected_kind != DetectedKind::tileset_json
        || !node.document.has_value()) {
        invalidContent("External Tileset resource is not a Tileset document");
    }
    const Json& document = *node.document;
    const std::string instance_key = parent_document_id.value_or("ROOT") + '\0'
            + parent_content_id.value_or("ROOT") + '\0'
            + node.evidence.object_id;
    const std::string document_id = hierarchyStableId(
            context, "three-d-tiles-document-instance-v1",
            context.request.source_generation + '\0' + instance_key);
    const Json& root_tile = document.at("root");
    const bool implicit_root = root_tile.contains("implicitTiling")
            || (root_tile.contains("extensions")
                && root_tile.at("extensions").is_object()
                && root_tile.at("extensions").contains(
                        "3DTILES_implicit_tiling"));
    std::string root_tile_id;
    if (implicit_root) {
        const Json& configuration = implicitTiling(root_tile);
        const bool octree = configuration.contains("subdivisionScheme")
                && configuration.at("subdivisionScheme") == "OCTREE";
        root_tile_id = implicitTileId(context, document_id, "", 0U, 0U, 0U,
                octree ? std::optional<std::uint64_t>(0U) : std::nullopt);
    } else {
        root_tile_id = hierarchyStableId(
                context, "three-d-tiles-explicit-tile-v1",
                context.request.source_generation + '\0' + document_id + '\0');
    }
    HierarchyRecord record;
    record.record_type = HierarchyRecordType::document;
    record.record_id = document_id;
    record.document_id = document_id;
    record.resource_object_id = node.evidence.object_id;
    record.parent_document_id = parent_document_id;
    record.parent_content_id = parent_content_id;
    record.parent_content_ordinal = parent_content_ordinal;
    record.document_depth = document_depth;
    record.asset_version = document.at("asset").at("version").get<std::string>();
    record.document_sha256 = node.evidence.observed_sha256;
    record.root_tile_id = root_tile_id;
    if (document.at("asset").contains("gltfUpAxis")) {
        if (!document.at("asset").at("gltfUpAxis").is_string()) {
            invalidContent("Tileset gltfUpAxis is invalid");
        }
        record.gltf_up_axis = document.at("asset").at("gltfUpAxis").get<std::string>();
    }
    record.required_extensions = node.evidence.required_extensions;
    record.used_extensions = node.evidence.used_extensions;
    collectExtensions(context.inventory, record.required_extensions,
                      record.used_extensions);
    requireHierarchyCapacity(context);
    context.inventory.records.push_back(std::move(record));
    ++context.inventory.documents;
    inventoryExplicitTile(context, resource_index, document_id, document.at("root"),
                          "", std::nullopt, 0U, identityTransform(),
                          RefineMode::replace, document_depth, ancestor_resources);
}

std::string closureHash(const std::vector<Node>& nodes,
                        const std::string& root_path) {
    std::vector<const Node*> ordered;
    ordered.reserve(nodes.size());
    for (const auto& node : nodes) ordered.push_back(&node);
    std::sort(ordered.begin(), ordered.end(), [](const Node* left, const Node* right) {
        return std::tie(left->evidence.package_relative_path, left->evidence.object_id)
                < std::tie(right->evidence.package_relative_path,
                           right->evidence.object_id);
    });
    std::ostringstream canonical;
    canonical << kClosureHashDomain << '\0' << root_path << '\n';
    for (const Node* node : ordered) {
        canonical << node->evidence.object_id << '\0'
                  << node->evidence.package_relative_path << '\0'
                  << node->evidence.observed_size << '\0'
                  << node->evidence.observed_sha256 << '\0'
                  << detectedKindName(node->evidence.detected_kind) << '\0'
                  << (node->evidence.required ? '1' : '0') << '\0';
        for (const auto& dependency : node->evidence.dependency_ids) {
            canonical << dependency << '\0';
        }
        canonical << '\n';
    }
    return sha256(canonical.str());
}

std::vector<ResultPage> buildPages(const InspectionRequest& request,
                                   const std::vector<Node>& nodes,
                                   std::uint32_t requested_page_size,
                                   std::string& manifest_id) {
    std::vector<ResourceEvidence> records;
    records.reserve(nodes.size());
    for (const auto& node : nodes) records.push_back(node.evidence);
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return std::tie(left.package_relative_path, left.object_id)
                < std::tie(right.package_relative_path, right.object_id);
    });
    manifest_id = "result-" + sha256(std::string(kResultManifestDomain) + '\0'
            + request.inspection_id + '\0' + request.request_id).substr(0U, 32U);
    std::vector<ResultPage> pages;
    std::size_t offset = 0U;
    while (offset < records.size()) {
        std::size_t count = std::min<std::size_t>(requested_page_size,
                                                  records.size() - offset);
        ResultPage page;
        page.message_type = MessageType::result_page;
        page.protocol_version = kProtocolVersion;
        page.inspection_id = request.inspection_id;
        page.request_id = request.request_id;
        page.manifest_id = manifest_id;
        page.page_number = static_cast<std::uint32_t>(pages.size());
        while (count > 0U) {
            page.records.assign(records.begin() + static_cast<std::ptrdiff_t>(offset),
                                records.begin() + static_cast<std::ptrdiff_t>(offset + count));
            try {
                page.page_sha256 = canonicalPageSha256(page.records);
                break;
            } catch (const std::invalid_argument&) {
                --count;
            }
        }
        if (count == 0U) resourceLimit("One result evidence record exceeds page limits");
        page.record_count = static_cast<std::uint32_t>(count);
        pages.push_back(std::move(page));
        offset += count;
    }
    return pages;
}

std::vector<HierarchyPage> buildHierarchyPages(
        const InspectionRequest& request,
        const std::vector<HierarchyRecord>& records,
        const PipelineLimits& limits,
        std::string& manifest_id) {
    manifest_id = "hierarchy-" + sha256(
            std::string("three-d-tiles-hierarchy-manifest-v1") + '\0'
            + request.inspection_id + '\0' + request.request_id).substr(0U, 32U);
    std::vector<HierarchyPage> pages;
    std::size_t offset = 0U;
    std::uint64_t spool_bytes = 0U;
    while (offset < records.size()) {
        std::size_t count = std::min<std::size_t>(limits.hierarchy_page_size,
                                                  records.size() - offset);
        HierarchyPage page;
        page.message_type = MessageType::hierarchy_result_page;
        page.protocol_version = kProtocolVersion;
        page.inspection_id = request.inspection_id;
        page.request_id = request.request_id;
        page.manifest_id = manifest_id;
        page.page_number = static_cast<std::uint32_t>(pages.size());
        while (count > 0U) {
            page.records.assign(
                    records.begin() + static_cast<std::ptrdiff_t>(offset),
                    records.begin() + static_cast<std::ptrdiff_t>(offset + count));
            try {
                page.page_sha256 = canonicalPageSha256(page.records);
                break;
            } catch (const std::invalid_argument&) {
                --count;
            }
        }
        if (count == 0U) {
            resourceLimit("One hierarchy record exceeds page limits");
        }
        page.record_count = static_cast<std::uint32_t>(count);
        const std::uint64_t page_bytes = serializeHierarchyPage(page).size();
        if (page_bytes > limits.maximum_hierarchy_spool_bytes - std::min(
                    spool_bytes, limits.maximum_hierarchy_spool_bytes)) {
            resourceLimit("Hierarchy spool exceeds its byte limit");
        }
        spool_bytes += page_bytes;
        pages.push_back(std::move(page));
        offset += count;
    }
    return pages;
}

Diagnostic warningDiagnostic(const char* message, std::uint64_t occurrences) {
    Diagnostic diagnostic;
    diagnostic.code = DiagnosticCode::content_invalid;
    diagnostic.severity = DiagnosticSeverity::warning;
    diagnostic.stage = InspectorStage::structure_classification;
    diagnostic.safe_message = message;
    diagnostic.retryable = false;
    diagnostic.occurrence_count = occurrences;
    return diagnostic;
}

}  // namespace

InspectionPipelineError::InspectionPipelineError(
        PipelineFailureKind kind, DiagnosticCode code, InspectorStage stage,
        std::string message)
    : std::runtime_error(std::move(message)), kind_(kind),
      diagnostic_code_(code), stage_(stage) {
}

PipelineFailureKind InspectionPipelineError::kind() const noexcept {
    return kind_;
}

DiagnosticCode InspectionPipelineError::diagnosticCode() const noexcept {
    return diagnostic_code_;
}

InspectorStage InspectionPipelineError::stage() const noexcept {
    return stage_;
}

PipelineOutput InspectionPipeline::inspect(
        const InspectionRequest& request,
        const package::PackageEnumerationResult& package_result,
        const PipelineLimits& limits,
        const std::vector<ToolVersion>& tool_versions,
        const ResourcePrefixReader& reader,
        const package::ContinuePredicate& should_continue) const {
    if (!reader || tool_versions.empty() || limits.maximum_json_bytes == 0U
        || limits.maximum_json_nesting == 0U
        || limits.maximum_data_uri_bytes == 0U
         || limits.maximum_expanded_bytes == 0U
         || limits.maximum_resource_count == 0U
         || limits.maximum_uri_depth == 0U
         || limits.maximum_available_tiles == 0U
         || limits.maximum_hierarchy_records == 0U
         || limits.maximum_documents == 0U || limits.maximum_subtrees == 0U
         || limits.maximum_hierarchy_spool_bytes == 0U
         || limits.maximum_subtree_bytes == 0U
         || limits.maximum_contents_per_tile == 0U
         || limits.maximum_tile_depth == 0U
         || limits.result_page_size == 0U
         || limits.result_page_size > ProtocolLimits::kMaximumResultPageRecords
         || limits.hierarchy_page_size == 0U
         || limits.hierarchy_page_size
                 > ProtocolLimits::kMaximumHierarchyPageRecords) {
        throw std::invalid_argument("Inspection pipeline configuration is invalid");
    }
    package::PackageNamespaceValidator{}.validate(package_result.entries);
    if (package_result.entries.size() > limits.maximum_resource_count) {
        resourceLimit("Package resource count exceeds closure limits");
    }

    std::vector<Node> nodes;
    nodes.reserve(package_result.entries.size());
    std::map<std::string, std::size_t> catalog;
    std::map<std::string, const package::PackageEntryEvidence*> entries;
    for (const auto& entry : package_result.entries) {
        requireContinue(should_continue);
        Node node;
        node.evidence.object_id = entry.object_id;
        node.evidence.package_relative_path = entry.path.normalized_candidate;
        node.evidence.role = entry.entry_kind
                == package::PackageEntryKind::archive_container
                ? ResourceRole::package_archive : ResourceRole::package_object;
        node.evidence.observed_size = entry.observed_expanded_bytes;
        node.evidence.observed_etag = entry.source_etag.empty()
                ? std::string(kArchiveEntryEtagPrefix) + entry.observed_sha256
                : entry.source_etag;
        node.evidence.observed_sha256 = entry.observed_sha256;
        node.evidence.required = false;
        classify(node, entry, limits, reader);
        if (!catalog.emplace(node.evidence.package_relative_path,
                             nodes.size()).second) {
            invalidResource("Package catalog contains a duplicate path");
        }
        nodes.push_back(std::move(node));
        entries.emplace(entry.path.normalized_candidate, &entry);
    }

    std::uint64_t inline_bytes = 0U;
    std::map<std::string, std::size_t> inline_by_sha256;
    for (std::size_t source_index = 0U; source_index < nodes.size(); ++source_index) {
        requireContinue(should_continue);
        const std::vector<RawReference> references = nodes[source_index].raw_references;
        for (const auto& reference : references) {
            if (reference.kind == ReferenceKind::implicit_subtree_template
                || reference.kind == ReferenceKind::implicit_content_template) {
                nodes[source_index].has_implicit_template =
                        validateImplicitTemplate(
                                nodes[source_index].evidence.package_relative_path,
                                reference.uri);
                continue;
            }
            std::size_t target_index = 0U;
            if (startsWithAsciiCaseInsensitive(reference.uri, "data:")) {
                const DecodedDataUri decoded = decodeDataUri(
                        reference.uri, limits.maximum_data_uri_bytes);
                if (decoded.bytes.size()
                    > limits.maximum_expanded_bytes - std::min(
                            inline_bytes, limits.maximum_expanded_bytes)) {
                    resourceLimit("Data URI closure exceeds the expanded byte limit");
                }
                const std::string digest = sha256(decoded.bytes);
                const auto existing = inline_by_sha256.find(digest);
                if (existing != inline_by_sha256.end()) {
                    target_index = existing->second;
                    assignRole(nodes[target_index], targetRole(reference.kind));
                } else {
                    if (nodes.size() >= limits.maximum_resource_count) {
                        resourceLimit("Resource closure count exceeds its limit");
                    }
                    Node inline_node;
                    inline_node.evidence.object_id = sha256(
                            std::string(kInlineIdentityDomain) + '\0' + digest);
                    inline_node.evidence.package_relative_path =
                            std::string(kInlinePathPrefix) + digest;
                    inline_node.evidence.role = targetRole(reference.kind);
                    inline_node.evidence.media_type = decoded.media_type;
                    inline_node.evidence.detected_kind =
                            targetRole(reference.kind) == ResourceRole::image
                            ? DetectedKind::image : DetectedKind::binary;
                    inline_node.evidence.observed_size = decoded.bytes.size();
                    inline_node.evidence.observed_etag =
                            std::string(kInlineEtagPrefix) + digest;
                    inline_node.evidence.observed_sha256 = digest;
                    inline_node.evidence.required = false;
                    target_index = nodes.size();
                    inline_by_sha256.emplace(digest, target_index);
                    catalog.emplace(inline_node.evidence.package_relative_path,
                                    target_index);
                    nodes.push_back(std::move(inline_node));
                    inline_bytes += decoded.bytes.size();
                }
            } else {
                const std::string resolved = resolveRelativePath(
                        nodes[source_index].evidence.package_relative_path,
                        reference.uri);
                const auto target = catalog.find(resolved);
                if (target == catalog.end()) {
                    invalidClosure("Required package-relative resource is missing");
                }
                target_index = target->second;
                assignRole(nodes[target_index], targetRole(reference.kind));
            }
            nodes[source_index].dependencies.push_back(target_index);
            if (nodes[source_index].dependencies.size()
                > ProtocolLimits::kMaximumDependenciesPerResource) {
                resourceLimit("Resource dependency count exceeds its limit");
            }
        }
        std::sort(nodes[source_index].dependencies.begin(),
                  nodes[source_index].dependencies.end());
        nodes[source_index].dependencies.erase(
                std::unique(nodes[source_index].dependencies.begin(),
                            nodes[source_index].dependencies.end()),
                nodes[source_index].dependencies.end());
    }

    // Reject cycles in every understood reference component, including an
    // ambiguous Tileset candidate set for which no root has been selected yet.
    std::vector<std::uint8_t> graph_color(nodes.size(), 0U);
    for (std::size_t start = 0U; start < nodes.size(); ++start) {
        if (graph_color[start] != 0U) continue;
        std::vector<std::pair<std::size_t, std::size_t>> graph_stack;
        graph_color[start] = 1U;
        graph_stack.emplace_back(start, 0U);
        while (!graph_stack.empty()) {
            requireContinue(should_continue);
            auto& frame = graph_stack.back();
            if (frame.second == nodes[frame.first].dependencies.size()) {
                graph_color[frame.first] = 2U;
                graph_stack.pop_back();
                continue;
            }
            const std::size_t child =
                    nodes[frame.first].dependencies[frame.second++];
            if (graph_color[child] == 1U) {
                invalidClosure("Resource closure contains a cycle");
            }
            if (graph_color[child] == 0U) {
                graph_color[child] = 1U;
                graph_stack.emplace_back(child, 0U);
            }
        }
    }

    std::vector<std::size_t> candidates;
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        if (nodes[index].evidence.detected_kind == DetectedKind::tileset_json) {
            candidates.push_back(index);
        }
    }
    if (candidates.empty()) invalidContent("Package has no valid Tileset root candidate");

    RootSelectionEvidence root_selection;
    root_selection.candidate_count = static_cast<std::uint32_t>(candidates.size());
    std::optional<std::size_t> root;
    if (request.selected_root_hint.has_value()) {
        const std::string selected = resolveRelativePath(
                "__selection_base__", *request.selected_root_hint);
        const auto found = catalog.find(selected);
        if (found == catalog.end()
            || nodes[found->second].evidence.detected_kind
                    != DetectedKind::tileset_json) {
            invalidContent("Explicit root is absent or is not a Tileset");
        }
        root = found->second;
        root_selection.method = RootSelectionMethod::explicit_root;
    } else {
        std::vector<std::size_t> top_level;
        for (const std::size_t candidate : candidates) {
            if (nodes[candidate].evidence.package_relative_path.find('/')
                == std::string::npos) {
                top_level.push_back(candidate);
            }
        }
        if (top_level.size() == 1U) {
            root = top_level.front();
            root_selection.method = RootSelectionMethod::unique_top_level;
        } else {
            std::set<std::size_t> candidate_set(candidates.begin(), candidates.end());
            std::map<std::size_t, std::uint64_t> indegree;
            for (const auto candidate : candidates) indegree[candidate] = 0U;
            for (const auto candidate : candidates) {
                for (const auto dependency : nodes[candidate].dependencies) {
                    if (candidate_set.count(dependency) != 0U) ++indegree[dependency];
                }
            }
            std::vector<std::size_t> graph_roots;
            for (const auto& entry : indegree) {
                if (entry.second == 0U) graph_roots.push_back(entry.first);
            }
            if (graph_roots.size() == 1U) {
                root = graph_roots.front();
                root_selection.method = RootSelectionMethod::unique_graph_root;
            } else {
                root_selection.method = RootSelectionMethod::ambiguous;
            }
        }
    }

    HierarchyInventory hierarchy;
    if (root.has_value()) {
        InventoryContext inventory_context{
                request, nodes, catalog, entries, limits, reader,
                should_continue, {}};
        inventoryDocument(inventory_context, *root, std::nullopt, std::nullopt,
                          std::nullopt, 0U, {});
        hierarchy = std::move(inventory_context.inventory);
        root_selection.selected_root_path =
                nodes[*root].evidence.package_relative_path;
        std::vector<std::uint8_t> color(nodes.size(), 0U);
        std::vector<std::uint32_t> depth(nodes.size(), 0U);
        std::vector<std::pair<std::size_t, std::size_t>> stack;
        color[*root] = 1U;
        nodes[*root].evidence.required = true;
        stack.emplace_back(*root, 0U);
        while (!stack.empty()) {
            requireContinue(should_continue);
            auto& frame = stack.back();
            if (frame.second == nodes[frame.first].dependencies.size()) {
                color[frame.first] = 2U;
                stack.pop_back();
                continue;
            }
            const std::size_t child =
                    nodes[frame.first].dependencies[frame.second++];
            if (color[child] == 1U) invalidClosure("Resource closure contains a cycle");
            nodes[child].evidence.required = true;
            if (color[child] == 0U) {
                depth[child] = depth[frame.first] + 1U;
                if (depth[child] > limits.maximum_uri_depth) {
                    resourceLimit("Resource closure exceeds its URI depth limit");
                }
                color[child] = 1U;
                stack.emplace_back(child, 0U);
            }
        }
    }

    std::uint64_t unknown_extensions = 0U;
    std::uint64_t implicit_templates = 0U;
    static const std::set<std::string> kKnownRequiredExtensions = {
            "3DTILES_implicit_tiling", "3DTILES_metadata",
            "3DTILES_multiple_contents", "EXT_mesh_features",
            "EXT_meshopt_compression", "EXT_structural_metadata",
            "KHR_draco_mesh_compression", "KHR_texture_basisu"};
    for (auto& node : nodes) {
        node.evidence.dependency_ids.clear();
        node.evidence.dependency_ids.reserve(node.dependencies.size());
        for (const std::size_t dependency : node.dependencies) {
            node.evidence.dependency_ids.push_back(
                    nodes[dependency].evidence.object_id);
        }
        std::sort(node.evidence.dependency_ids.begin(),
                  node.evidence.dependency_ids.end());
        for (const auto& extension : node.evidence.required_extensions) {
            if (kKnownRequiredExtensions.count(extension) == 0U) {
                ++unknown_extensions;
            }
        }
        if (node.has_implicit_template) ++implicit_templates;
    }

    std::string manifest_id;
    std::vector<ResultPage> pages = buildPages(
            request, nodes, limits.result_page_size, manifest_id);
    std::string hierarchy_manifest_id;
    std::vector<HierarchyPage> hierarchy_pages = buildHierarchyPages(
            request, hierarchy.records, limits, hierarchy_manifest_id);
    InspectionResult result;
    result.message_type = MessageType::inspection_result;
    result.protocol_version = kProtocolVersion;
    result.inspection_id = request.inspection_id;
    result.request_id = request.request_id;
    result.source_generation = request.source_generation;
    result.inspector_version = request.inspector_version;
    result.tool_versions = tool_versions;
    result.root_selection = root_selection;
    result.total_resources = nodes.size();
    result.total_documents = hierarchy.documents;
    result.total_tiles = hierarchy.tiles;
    result.total_subtrees = hierarchy.subtrees;
    result.total_contents = hierarchy.contents;
    result.result_manifest.manifest_id = manifest_id;
    result.result_manifest.record_count = nodes.size();
    result.result_manifest.page_count = static_cast<std::uint32_t>(pages.size());
    result.result_manifest.page_size = limits.result_page_size;
    result.result_manifest.manifest_sha256 =
            canonicalResultManifestSha256(pages);
    result.hierarchy_manifest.manifest_id = hierarchy_manifest_id;
    result.hierarchy_manifest.record_count = hierarchy.records.size();
    result.hierarchy_manifest.page_count =
            static_cast<std::uint32_t>(hierarchy_pages.size());
    result.hierarchy_manifest.page_size = limits.hierarchy_page_size;
    result.hierarchy_manifest.manifest_sha256 =
            canonicalHierarchyManifestSha256(hierarchy_pages);
    result.required_extensions.assign(hierarchy.required_extensions.begin(),
                                      hierarchy.required_extensions.end());
    result.used_extensions.assign(hierarchy.used_extensions.begin(),
                                  hierarchy.used_extensions.end());
    if (root.has_value()) {
        result.outcome = Outcome::succeeded;
        result.source_closure_hash = closureHash(
                nodes, nodes[*root].evidence.package_relative_path);
    } else {
        result.outcome = Outcome::rejected;
        Diagnostic diagnostic;
        diagnostic.code = DiagnosticCode::root_selection_required;
        diagnostic.severity = DiagnosticSeverity::error;
        diagnostic.stage = InspectorStage::closure_validation;
        diagnostic.safe_message = "Multiple valid Tileset roots require explicit selection";
        diagnostic.retryable = false;
        diagnostic.occurrence_count = 1U;
        result.diagnostics.push_back(std::move(diagnostic));
    }
    if (unknown_extensions > 0U) {
        result.diagnostics.push_back(warningDiagnostic(
                "Unknown required extensions prevent limited authorization readiness",
                unknown_extensions));
    }
    if (implicit_templates > 0U) {
        result.diagnostics.push_back(warningDiagnostic(
                "Implicit hierarchy availability requires later inventory processing",
                implicit_templates));
    }
    validateResult(result, request);
    validateResultPages(result, pages);
    validateHierarchyPages(result, hierarchy_pages);
    return {std::move(result), std::move(pages), std::move(hierarchy_pages)};
}

}  // namespace clip_worker::inspection::pipeline
