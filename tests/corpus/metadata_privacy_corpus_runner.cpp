#include "clip_worker/formats/b3dm_mesh_adapter.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/formats/gltf_mesh_reader.hpp"
#include "clip_worker/metadata/legacy_feature_metadata.hpp"
#include "clip_worker/metadata/property_lookup.hpp"
#include "clip_worker/normalization/broad_resource_profile.hpp"
#include "clip_worker/normalization/mesh_normalizer.hpp"
#include "clip_worker/normalization/mesh_resource_profile.hpp"
#include "clip_worker/normalization/metadata_canonical_writer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clip_worker::corpus {
namespace {

using Json = nlohmann::json;

constexpr int kSkippedReturnCode = 77;
constexpr const char* kExpectationsRelativePath =
        "tests/corpus/metadata-privacy-phase-corpus-expectations.json";
constexpr const char* kDownloadedRelativePath = "tests/corpus/downloaded";
constexpr const char* kSourceRecordName = "SOURCE.json";
constexpr const char* kResultRecordName = "RESULTS.metadata-v1.json";
constexpr const char* kMeshProfileRelativePath =
        "config/resource-limit-profiles-v2.json";
constexpr const char* kMetadataProfileRelativePath =
        "config/resource-limit-profiles-v4.json";
constexpr const char* kMeshProfileSha256 =
        "bb326727f6b29a6cdd3532d85e2043c3c0ff212056b837d3d4a2eca7df3d349c";
constexpr const char* kMetadataProfileSha256 =
        "b1a631c3f776210362ff795737603af88578a10634a04713dc04adff69880999";
constexpr const char* kValidatorBuildSha256 =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

constexpr const char* kSupportedOutcome = "SUPPORTED";
constexpr const char* kUnsupportedOutcome = "UNSUPPORTED";
constexpr const char* kRelationshipUnsupported =
        "METADATA_RELATIONSHIP_UNSUPPORTED";
constexpr const char* kStatisticsUnsupported =
        "METADATA_STATISTICS_UNSUPPORTED";

constexpr const char* kLegacyVariant = "legacy-b3dm-metadata-types";
constexpr const char* kExternalSchemaVariant =
        "external-tileset-schema-metadata";
constexpr const char* kPropertyAttributesVariant =
        "property-attributes-point-cloud";
constexpr const char* kImplicitSubtreeVariant =
        "implicit-subtree-metadata";
constexpr const char* kFullMetadataVariant =
        "tileset-statistics-and-relationships";
constexpr const char* kInvalidFeatureIdVariant =
        "feature-id-out-of-property-table-range";

const std::vector<std::string> kRequiredAssertions{
        "PARSER", "NORMALIZER", "PROPERTY_LOOKUP", "REMOVED_ID_REJECTION",
        "LEAKAGE_SCAN", "DETERMINISM"};

struct ExecutionOutcome {
    std::string outcome;
    std::optional<std::string> reason;
};

struct SerializedMetadata {
    Json structural_metadata;
    std::optional<Json> legacy_hierarchy;
    std::vector<std::vector<std::uint8_t>> buffer_views;
    std::vector<std::size_t> alignments;

    bool operator==(const SerializedMetadata& other) const {
        return structural_metadata == other.structural_metadata
                && legacy_hierarchy == other.legacy_hierarchy
                && buffer_views == other.buffer_views
                && alignments == other.alignments;
    }
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0) {
        fail("Corpus file is unreadable: " + path.generic_string());
    }
    std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(input.tellg()));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) fail("Corpus file read failed: " + path.generic_string());
    return bytes;
}

Json readJson(const std::filesystem::path& path) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) fail("Corpus JSON is unreadable: " + path.generic_string());
        Json result;
        input >> result;
        return result;
    } catch (const Json::exception&) {
        fail("Corpus JSON is invalid: " + path.generic_string());
    }
}

void writeJsonAtomically(const std::filesystem::path& path, const Json& value) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) fail("Corpus result cannot be written");
        output << value.dump(2) << '\n';
        if (!output) fail("Corpus result write failed");
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) fail("Corpus result publish failed");
}

std::string regexEscape(char value) {
    static constexpr std::string_view kSpecial = R"(\.^$|()[]{}+)";
    if (kSpecial.find(value) != std::string_view::npos) {
        return std::string{"\\"} + value;
    }
    return std::string(1U, value);
}

std::regex selectorRegex(const std::string& selector) {
    require(!selector.empty()
                    && !std::filesystem::path(selector).is_absolute()
                    && selector.find("..") == std::string::npos
                    && selector.find('\\') == std::string::npos,
            "Corpus selector is unsafe");
    std::string expression{"^"};
    for (const char value : selector) {
        if (value == '*') {
            expression += "[^/]*";
        } else if (value == '?') {
            expression += "[^/]";
        } else {
            expression += regexEscape(value);
        }
    }
    expression += '$';
    return std::regex(expression, std::regex::ECMAScript);
}

std::vector<std::filesystem::path> selectFiles(
        const std::filesystem::path& payload_root,
        const std::vector<std::string>& selectors) {
    require(std::filesystem::is_directory(payload_root),
            "Corpus payload directory is absent");
    std::vector<std::regex> patterns;
    patterns.reserve(selectors.size());
    for (const auto& selector : selectors) {
        patterns.push_back(selectorRegex(selector));
    }
    std::vector<bool> matched(selectors.size(), false);
    std::vector<std::filesystem::path> result;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(payload_root)) {
        if (!entry.is_regular_file()) continue;
        const std::string relative = std::filesystem::relative(
                entry.path(), payload_root).generic_string();
        bool selected = false;
        for (std::size_t index = 0U; index < patterns.size(); ++index) {
            if (std::regex_match(relative, patterns.at(index))) {
                matched.at(index) = true;
                selected = true;
            }
        }
        if (selected) result.push_back(entry.path());
    }
    require(std::all_of(matched.begin(), matched.end(),
                        [](bool value) { return value; }),
            "One or more corpus selectors matched no files");
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    require(!result.empty(), "Corpus variant selected no files");
    return result;
}

formats::ApprovedGltfResourceMap approvedResources(
        const std::filesystem::path& payload_root,
        const std::filesystem::path& root_path) {
    formats::ApprovedGltfResourceMap resources;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(payload_root)) {
        if (!entry.is_regular_file() || entry.path() == root_path) continue;
        const std::string relative = std::filesystem::relative(
                entry.path(), payload_root).generic_string();
        resources.emplace(relative, formats::ApprovedGltfResource{
                readBytes(entry.path()), std::string{}});
    }
    return resources;
}

normalization::ToolVersion corpusValidator() {
    return {"metadata-official-validator", "official-corpus-v1",
            kValidatorBuildSha256};
}

normalization::MeshResourceProfile metadataMeshProfile(
        const std::filesystem::path& project_root,
        const normalization::BroadResourceProfile& metadata_profile) {
    auto result = normalization::MeshResourceProfile::load(
            project_root / kMeshProfileRelativePath,
            normalization::kMeshResourceProfileVersion, kMeshProfileSha256);
    result.b3dm.enable_feature_metadata = true;
    result.b3dm.feature_metadata = metadata_profile.metadata;
    result.b3dm.gltf.enable_feature_metadata = true;
    result.b3dm.gltf.metadata = metadata_profile.metadata;
    return result;
}

SerializedMetadata serializeMetadata(
        const metadata::FeatureMetadata& feature_metadata) {
    SerializedMetadata result;
    const auto output = normalization::writeCanonicalMetadata(
            feature_metadata,
            [&result](const std::vector<std::uint8_t>& values,
                      std::size_t alignment) {
                require(alignment > 0U,
                        "Canonical metadata requested zero alignment");
                result.buffer_views.push_back(values);
                result.alignments.push_back(alignment);
                return result.buffer_views.size() - 1U;
            });
    result.structural_metadata = output.structural_metadata;
    result.legacy_hierarchy = output.legacy_hierarchy;
    return result;
}

std::size_t serializedByteCount(const SerializedMetadata& value) {
    std::size_t result = 0U;
    for (const auto& view : value.buffer_views) result += view.size();
    return result;
}

bool containsSequence(const std::vector<std::uint8_t>& haystack,
                      const std::vector<std::uint8_t>& needle) {
    return !needle.empty()
            && std::search(haystack.begin(), haystack.end(), needle.begin(),
                           needle.end()) != haystack.end();
}

std::optional<std::string> stableReason(
        formats::FormatErrorCode code) {
    switch (code) {
        case formats::FormatErrorCode::metadata_feature_id_invalid:
            return "METADATA_FEATURE_ID_INVALID";
        case formats::FormatErrorCode::metadata_property_attribute_unsupported:
            return "METADATA_PROPERTY_ATTRIBUTE_UNSUPPORTED";
        default:
            return std::nullopt;
    }
}

template <typename Callable>
std::string expectFormatError(Callable&& callable,
                              formats::FormatErrorCode expected) {
    try {
        callable();
    } catch (const formats::FormatError& error) {
        require(error.code() == expected,
                "Corpus parser returned a different stable error code");
        const auto reason = stableReason(error.code());
        require(reason.has_value(), "Corpus error has no stable reason mapping");
        return *reason;
    }
    fail("Corpus parser unexpectedly accepted an unsupported variant");
}

ExecutionOutcome executeLegacyB3dm(
        const std::filesystem::path& project_root,
        const std::filesystem::path& payload_root,
        const std::vector<std::filesystem::path>& files) {
    const auto metadata_profile = normalization::BroadResourceProfile::load(
            project_root / kMetadataProfileRelativePath,
            normalization::kMetadataResourceProfileVersion,
            kMetadataProfileSha256);
    const auto mesh_profile = metadataMeshProfile(project_root, metadata_profile);
    for (const auto& path : files) {
        const auto source_bytes = readBytes(path);
        const auto parsed = formats::B3dmMeshAdapter::read(
                source_bytes, mesh_profile.b3dm);
        require(parsed.scene.feature_metadata.has_value(),
                "Official B3DM metadata was not parsed");
        const auto& source_metadata = *parsed.scene.feature_metadata;
        require(source_metadata.primary_property_table == 0U
                        && !source_metadata.property_tables.empty()
                        && source_metadata.property_tables.front().row_count > 1U,
                "Official B3DM metadata table is not multi-feature");
        const auto retained_material = metadata::lookupPropertyMaterial(
                source_metadata, source_metadata.primary_property_table, 0U);
        require(retained_material.has_value(),
                "Official B3DM retained property lookup failed");
        for (std::uint32_t feature_id = 0U;
             feature_id < source_metadata.property_tables.front().row_count;
             ++feature_id) {
            require(metadata::lookupPropertyMaterial(
                            source_metadata,
                            source_metadata.primary_property_table,
                            feature_id)
                            .has_value(),
                    "Official B3DM source property lookup failed");
        }

        normalization::MeshNormalizationInput input;
        input.source_kind = normalization::MeshSourceKind::b3dm;
        input.root_package_relative_path = std::filesystem::relative(
                path, payload_root).generic_string();
        input.source_bytes = source_bytes;
        const auto first = normalization::MeshNormalizer::normalize(
                input, mesh_profile, corpusValidator());
        const auto second = normalization::MeshNormalizer::normalize(
                input, mesh_profile, corpusValidator());
        require(first.canonical.glb == second.canonical.glb
                        && first.evidence.semantic_hash
                                == second.evidence.semantic_hash,
                "Official B3DM normalization is not deterministic");
        require(!containsSequence(first.canonical.glb, source_bytes),
                "Canonical GLB copied the original B3DM boundary payload");

        const auto compacted = metadata::compactLegacyFeatureMetadata(
                source_metadata, {0U}, metadata_profile.metadata);
        require(compacted.property_tables.front().row_count == 1U,
                "Official B3DM property table was not compacted");
        require(metadata::lookupPropertyMaterial(
                        compacted, compacted.primary_property_table, 0U)
                        == retained_material,
                "Official B3DM retained property changed during compaction");
        require(!metadata::lookupPropertyMaterial(
                         compacted, compacted.primary_property_table, 1U)
                         .has_value(),
                "Official B3DM removed feature ID still resolves");

        const auto source_serialized = serializeMetadata(source_metadata);
        const auto compacted_first = serializeMetadata(compacted);
        const auto compacted_second = serializeMetadata(compacted);
        require(compacted_first == compacted_second,
                "Official B3DM compacted metadata is not deterministic");
        require(serializedByteCount(compacted_first)
                        < serializedByteCount(source_serialized),
                "Official B3DM compacted metadata retained unused row bytes");
    }
    return {kSupportedOutcome, std::nullopt};
}

ExecutionOutcome executePropertyAttributes(
        const std::filesystem::path& payload_root,
        const std::vector<std::filesystem::path>& files,
        const normalization::BroadResourceProfile& metadata_profile) {
    std::optional<std::string> stable_reason;
    for (const auto& path : files) {
        formats::GltfMeshReaderLimits limits;
        limits.enable_feature_metadata = true;
        limits.metadata = metadata_profile.metadata;
        const auto resources = approvedResources(payload_root, path);
        const auto bytes = readBytes(path);
        const std::string relative = std::filesystem::relative(
                path, payload_root).generic_string();
        const std::string reason = expectFormatError(
                [&] {
                    static_cast<void>(formats::GltfMeshReader::read(
                            bytes, formats::GltfContentKind::gltf, relative,
                            resources, limits));
                },
                formats::FormatErrorCode::metadata_property_attribute_unsupported);
        if (stable_reason.has_value()) {
            require(*stable_reason == reason,
                    "Property attribute reason changed between files");
        }
        stable_reason = reason;
    }
    return {kUnsupportedOutcome, stable_reason};
}

ExecutionOutcome executeInvalidFeatureId(
        const std::filesystem::path& payload_root,
        const std::vector<std::filesystem::path>& files,
        const normalization::BroadResourceProfile& metadata_profile) {
    require(files.size() == 1U,
            "Invalid Feature ID corpus selector is not unique");
    const auto& path = files.front();
    formats::GltfMeshReaderLimits limits;
    limits.enable_feature_metadata = true;
    limits.metadata = metadata_profile.metadata;
    const auto bytes = readBytes(path);
    const std::string relative = std::filesystem::relative(
            path, payload_root).generic_string();
    const std::string reason = expectFormatError(
            [&] {
                static_cast<void>(formats::GltfMeshReader::read(
                        bytes, formats::GltfContentKind::gltf, relative, {},
                        limits));
            },
            formats::FormatErrorCode::metadata_feature_id_invalid);
    return {kUnsupportedOutcome, reason};
}

void verifyUnsupportedLookupBoundary() {
    const metadata::FeatureMetadata empty;
    require(!metadata::lookupPropertyMaterial(empty, std::nullopt, 0U)
                     .has_value(),
            "Unsupported metadata unexpectedly exposed a property row");
}

ExecutionOutcome executeExternalSchemaRelationship(
        const std::vector<std::filesystem::path>& files) {
    const auto tileset = std::find_if(
            files.begin(), files.end(), [](const auto& path) {
                return path.filename() == "tileset_1.1.json";
            });
    const auto schema = std::find_if(
            files.begin(), files.end(), [](const auto& path) {
                return path.filename() == "schema.json";
            });
    require(tileset != files.end() && schema != files.end(),
            "External schema corpus files are incomplete");
    const Json tileset_json = readJson(*tileset);
    const Json schema_json = readJson(*schema);
    require(tileset_json.contains("schemaUri")
                    && tileset_json.at("schemaUri") == "schema.json"
                    && tileset_json.contains("metadata")
                    && tileset_json.at("metadata").is_object()
                    && schema_json.contains("classes")
                    && schema_json.at("classes").is_object(),
            "External tileset metadata relationship changed");
    verifyUnsupportedLookupBoundary();
    return {kUnsupportedOutcome, kRelationshipUnsupported};
}

ExecutionOutcome executeImplicitSubtreeRelationship(
        const std::vector<std::filesystem::path>& files) {
    for (const auto& path : files) {
        const Json document = readJson(path);
        require(document.contains("subtreeMetadata")
                        && document.at("subtreeMetadata").is_object(),
                "Implicit subtree metadata relationship changed");
    }
    verifyUnsupportedLookupBoundary();
    return {kUnsupportedOutcome, kRelationshipUnsupported};
}

ExecutionOutcome executeFullTilesetMetadata(
        const std::vector<std::filesystem::path>& files) {
    require(files.size() == 1U,
            "Full metadata corpus selector is not unique");
    const Json document = readJson(files.front());
    require(document.contains("schema") && document.at("schema").is_object()
                    && document.contains("metadata")
                    && document.at("metadata").is_object(),
            "Full tileset metadata aggregate changed");
    verifyUnsupportedLookupBoundary();
    return {kUnsupportedOutcome, kStatisticsUnsupported};
}

ExecutionOutcome executeVariant(
        const std::string& variant_id,
        const std::filesystem::path& project_root,
        const std::filesystem::path& payload_root,
        const std::vector<std::filesystem::path>& files,
        const normalization::BroadResourceProfile& metadata_profile) {
    if (variant_id == kLegacyVariant) {
        return executeLegacyB3dm(project_root, payload_root, files);
    }
    if (variant_id == kExternalSchemaVariant) {
        return executeExternalSchemaRelationship(files);
    }
    if (variant_id == kPropertyAttributesVariant) {
        return executePropertyAttributes(payload_root, files, metadata_profile);
    }
    if (variant_id == kImplicitSubtreeVariant) {
        return executeImplicitSubtreeRelationship(files);
    }
    if (variant_id == kFullMetadataVariant) {
        return executeFullTilesetMetadata(files);
    }
    if (variant_id == kInvalidFeatureIdVariant) {
        return executeInvalidFeatureId(payload_root, files, metadata_profile);
    }
    fail("Unknown metadata corpus variant: " + variant_id);
}

std::vector<std::string> selectors(const Json& variant) {
    require(variant.contains("selectors")
                    && variant.at("selectors").is_array()
                    && !variant.at("selectors").empty(),
            "Metadata corpus variant selectors are missing");
    std::vector<std::string> result;
    for (const auto& value : variant.at("selectors")) {
        require(value.is_string(), "Metadata corpus selector is not a string");
        result.push_back(value.get<std::string>());
    }
    return result;
}

void validateOutcome(const Json& expected, const ExecutionOutcome& actual) {
    require(expected.contains("outcome") && expected.at("outcome").is_string(),
            "Expected metadata outcome is missing");
    require(actual.outcome == expected.at("outcome").get<std::string>(),
            "Executed metadata outcome differs from expectations");
    const std::optional<std::string> expected_reason =
            expected.contains("reason")
            ? std::optional<std::string>(
                      expected.at("reason").get<std::string>())
            : std::nullopt;
    require(actual.reason == expected_reason,
            "Executed metadata reason differs from expectations");
}

Json resultVariant(const Json& expected, const ExecutionOutcome& actual) {
    Json result{{"id", expected.at("id")},
                {"outcome", actual.outcome},
                {"assertions", kRequiredAssertions},
                {"passed", true}};
    if (actual.reason.has_value()) result["reason"] = *actual.reason;
    return result;
}

int run() {
    const std::filesystem::path project_root = CLIP_WORKER_SOURCE_DIR;
    const std::filesystem::path downloaded_root =
            project_root / kDownloadedRelativePath;
    const Json expectations =
            readJson(project_root / kExpectationsRelativePath);
    require(expectations.contains("entries")
                    && expectations.at("entries").is_array()
                    && !expectations.at("entries").empty(),
            "Metadata corpus expectations contain no entries");

    std::size_t present_entries = 0U;
    for (const auto& entry : expectations.at("entries")) {
        if (std::filesystem::is_directory(
                    downloaded_root / entry.at("entryId").get<std::string>())) {
            ++present_entries;
        }
    }
    if (present_entries == 0U) {
        std::cout << "Task 8 official corpus is not materialized; skipping\n";
        return kSkippedReturnCode;
    }
    require(present_entries == expectations.at("entries").size(),
            "Task 8 official corpus is only partially materialized");

    const auto metadata_profile = normalization::BroadResourceProfile::load(
            project_root / kMetadataProfileRelativePath,
            normalization::kMetadataResourceProfileVersion,
            kMetadataProfileSha256);
    std::vector<std::pair<std::filesystem::path, Json>> pending_results;
    for (const auto& entry : expectations.at("entries")) {
        const std::string entry_id = entry.at("entryId").get<std::string>();
        const std::filesystem::path entry_root = downloaded_root / entry_id;
        const std::filesystem::path payload_root = entry_root / "payload";
        const Json source = readJson(entry_root / kSourceRecordName);
        require(source.value("entryId", std::string{}) == entry_id
                        && source.contains("revision")
                        && source.at("revision").is_string(),
                "Materialized metadata source identity is invalid");

        Json variants = Json::array();
        for (const auto& variant : entry.at("variants")) {
            require(variant.contains("id") && variant.at("id").is_string(),
                    "Metadata corpus variant ID is missing");
            const auto files = selectFiles(payload_root, selectors(variant));
            const ExecutionOutcome actual = executeVariant(
                    variant.at("id").get<std::string>(), project_root,
                    payload_root, files, metadata_profile);
            validateOutcome(variant, actual);
            variants.push_back(resultVariant(variant, actual));
        }
        pending_results.emplace_back(
                entry_root / kResultRecordName,
                Json{{"entryId", entry_id},
                     {"revision", source.at("revision")},
                     {"variants", std::move(variants)}});
    }

    for (const auto& result : pending_results) {
        writeJsonAtomically(result.first, result.second);
    }
    std::cout << "Executed and published Task 8 official metadata corpus results"
              << '\n';
    return 0;
}

}  // namespace
}  // namespace clip_worker::corpus

int main() {
    try {
        return clip_worker::corpus::run();
    } catch (const std::exception& error) {
        std::cerr << "Metadata corpus execution failed: " << error.what()
                  << '\n';
        return 1;
    }
}
