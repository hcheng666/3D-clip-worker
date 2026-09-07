#include "clip_worker/normalization/broad_normalization_executor.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/composite_normalizer.hpp"
#include "clip_worker/normalization/instance_canonical_writer.hpp"
#include "clip_worker/normalization/mesh_normalization_executor.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"
#include "clip_worker/normalization/point_canonical_writer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization::v2 {
namespace {

using Json = nlohmann::json;

constexpr const char* kCompositeSemanticDomain =
        "THREE_D_COMPOSITE_PARENT_SEMANTIC_V1";
constexpr const char* kBroadScratchPrefix = "broad-normalizer-";

struct PreparedOutput {
    OutputDeclaration declaration;
    std::vector<std::uint8_t> bytes;
};

class AttemptScratch final {
public:
    AttemptScratch(const std::filesystem::path& root,
                   const ClaimTask& task) {
        if (root.empty()) {
            throw std::invalid_argument(
                    "Broad normalizer scratch root is unavailable");
        }
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error) {
            throw std::runtime_error(
                    "Broad normalizer scratch root cannot be created");
        }
        root_ = std::filesystem::weakly_canonical(root, error);
        if (error) {
            throw std::runtime_error(
                    "Broad normalizer scratch root cannot be resolved");
        }
        const std::string identity = sha256Hex(
                task.task_id + "\n" + task.attempt_id);
        directory_ = (root_ / (std::string(kBroadScratchPrefix) + identity))
                             .lexically_normal();
        if (directory_.parent_path() != root_) {
            throw std::invalid_argument(
                    "Broad normalizer scratch identity escapes its root");
        }
        if (!std::filesystem::create_directory(directory_, error) || error) {
            throw std::runtime_error(
                    "Broad normalizer scratch attempt directory is unavailable");
        }
    }

    ~AttemptScratch() {
        std::error_code error;
        if (!directory_.empty() && directory_.parent_path() == root_) {
            std::filesystem::remove_all(directory_, error);
        }
    }

    AttemptScratch(const AttemptScratch&) = delete;
    AttemptScratch& operator=(const AttemptScratch&) = delete;

    [[nodiscard]] std::filesystem::path resourcePath(
            std::size_t ordinal) const {
        return directory_ / ("resource-" + std::to_string(ordinal) + ".bin");
    }

private:
    std::filesystem::path root_;
    std::filesystem::path directory_;
};

std::vector<std::uint8_t> readFile(const std::filesystem::path& path,
                                   std::uint64_t expected_size) {
    if (expected_size > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(
                "Broad normalizer resource exceeds addressable size");
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0
            || static_cast<std::uint64_t>(input.tellg()) != expected_size) {
        throw std::runtime_error(
                "Broad normalizer downloaded resource size is inconsistent");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(expected_size));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        throw std::runtime_error(
                "Broad normalizer downloaded resource cannot be read");
    }
    return bytes;
}

void heartbeat(NormalizationV2LeaseSession& session, TaskPhase phase,
               std::uint64_t processed_resources) {
    const auto response = session.heartbeat(phase, processed_resources);
    if (response.cancel_requested || !session.active()) {
        throw client::ObjectTransferCancelledError(
                "Broad normalizer task was cancelled");
    }
}

PreparedOutput contentOutput(
        const ClaimTask& task, CanonicalFamily family,
        std::vector<std::uint32_t> ordinal_path,
        std::vector<std::uint8_t> bytes,
        const CanonicalArtifactEvidence& evidence,
        std::optional<std::uint64_t> point_count = std::nullopt,
        std::optional<std::uint64_t> instance_count = std::nullopt) {
    PreparedOutput result;
    result.bytes = std::move(bytes);
    auto& declaration = result.declaration;
    declaration.ordinal_path = std::move(ordinal_path);
    declaration.output_kind = OutputKind::canonical_content;
    declaration.canonical_family = family;
    const bool metadata_task = task.normalization_version
                    == v3::normalizationVersion(task.canonical_family)
            && task.canonical_contract_version
                    == v3::canonicalContractVersion(task.canonical_family);
    declaration.canonical_contract_version = metadata_task
            ? v3::canonicalContractVersion(family)
            : canonicalContractVersion(family);
    declaration.output_size = evidence.output_size;
    declaration.output_sha256 = evidence.output_sha256;
    declaration.semantic_hash = metadata_task
            ? normalization::metadataSemanticHash(evidence)
            : evidence.semantic_hash;
    declaration.validation_summary = contentValidationSummary(
            evidence, family, point_count, instance_count, 0U);
    declaration.validation_manifest_sha256 = validationManifestSha256(
            declaration.validation_summary);
    declaration.output_id = outputId(task, declaration);
    return result;
}

formats::ApprovedGltfResourceMap approvedResources(
        const std::vector<ResourceRecord>& records,
        const std::map<std::string, std::vector<std::uint8_t>>& bytes_by_id,
        const std::string& root_object_id) {
    formats::ApprovedGltfResourceMap result;
    for (const auto& record : records) {
        const auto bytes = bytes_by_id.find(record.object_id);
        if (bytes == bytes_by_id.end()
                || bytes->second.size() != record.size
                || client::sha256Hex(bytes->second) != record.sha256) {
            throw std::invalid_argument(
                    "Broad normalizer resource differs from manifest");
        }
        if (record.object_id == root_object_id) continue;
        const bool inserted = result.emplace(
                record.package_relative_path,
                formats::ApprovedGltfResource{
                        bytes->second,
                        record.media_type.value_or(std::string{})})
                                      .second;
        if (!inserted) {
            throw std::invalid_argument(
                    "Broad normalizer manifest has duplicate paths");
        }
    }
    return result;
}

const ResourceRecord& rootRecord(const std::vector<ResourceRecord>& records,
                                 const ClaimTask& task) {
    const auto root = std::find_if(
            records.begin(), records.end(), [&task](const ResourceRecord& record) {
                return record.object_id == task.root_object_id;
            });
    if (root == records.end()) {
        throw std::invalid_argument(
                "Broad normalizer root object is absent");
    }
    return *root;
}

PreparedOutput normalizePoint(
        const ClaimTask& task, const ResourceRecord& root,
        const std::map<std::string, std::vector<std::uint8_t>>& bytes_by_id,
        const BroadNormalizationExecutorConfig& config) {
    if (root.detected_kind != "PNTS") {
        throw formats::FormatError(
                formats::FormatErrorCode::pnts_header_invalid,
                "POINT task root is not PNTS");
    }
    auto scene = PointNormalizer(config.broad_profile.point).normalize(
            formats::ByteView(bytes_by_id.at(root.object_id)));
    auto canonical = PointCanonicalWriter::write(std::move(scene));
    const auto evidence = validateCanonicalGlb(
            canonical.glb, normalization::CanonicalFamily::point_gltf2,
            config.point_validator);
    return contentOutput(task, CanonicalFamily::point_gltf2, {0U},
                         std::move(canonical.glb), evidence,
                         canonical.point_count);
}

PreparedOutput normalizeMesh(
        const ClaimTask& task, const std::vector<ResourceRecord>& records,
        const std::map<std::string, std::vector<std::uint8_t>>& bytes_by_id,
        const BroadNormalizationExecutorConfig& config) {
    const auto input = MeshNormalizationExecutor::buildInput(
            records, bytes_by_id);
    auto normalized = MeshNormalizer::normalize(
            input, config.mesh_profile, config.mesh_validator);
    return contentOutput(task, CanonicalFamily::mesh_gltf2, {0U},
                         std::move(normalized.canonical.glb),
                         normalized.evidence);
}

PreparedOutput normalizeInstance(
        const ClaimTask& task, const ResourceRecord& root,
        const std::vector<ResourceRecord>& records,
        const std::map<std::string, std::vector<std::uint8_t>>& bytes_by_id,
        const BroadNormalizationExecutorConfig& config) {
    if (root.detected_kind != "I3DM") {
        throw formats::FormatError(
                formats::FormatErrorCode::i3dm_header_invalid,
                "INSTANCE task root is not I3DM");
    }
    InstanceNormalizationInput input;
    input.root_package_relative_path = root.package_relative_path;
    input.source_bytes = bytes_by_id.at(root.object_id);
    input.approved_resources = approvedResources(
            records, bytes_by_id, root.object_id);
    auto normalized = InstanceNormalizer(config.broad_profile.instance).normalize(
            input);
    auto canonical = InstanceCanonicalWriter::write(
            std::move(normalized.scene), config.instance_validator);
    return contentOutput(task, CanonicalFamily::instance_gltf2, {0U},
                         std::move(canonical.canonical.glb),
                         canonical.evidence, std::nullopt,
                         canonical.instance_count);
}

Json compositeChildJson(const ClaimTask& task,
                        const CompositeLeafResult& leaf,
                        const std::optional<std::string>& output_id) {
    const std::string child_id = compositeChildId(
            task, leaf.ordinal_path, leaf.source_kind, leaf.source_version,
            leaf.source_sha256);
    Json result{{"canonicalFamily", nullptr},
                {"childId", child_id},
                {"depth", leaf.depth},
                {"ordinalPath", leaf.ordinal_path},
                {"outputId", nullptr},
                {"reasonCode", nullptr},
                {"sourceKind", leaf.source_kind},
                {"sourceSha256", leaf.source_sha256},
                {"sourceSize", leaf.source_size},
                {"sourceVersion", leaf.source_version},
                {"status", compositeLeafStatusName(leaf.status)}};
    if (leaf.status == CompositeLeafStatus::success) {
        result["canonicalFamily"] = canonicalFamilyName(
                fromV1Family(*leaf.canonical_family));
        result["outputId"] = *output_id;
    } else {
        result["reasonCode"] = leaf.reason_code;
    }
    return result;
}

std::vector<PreparedOutput> normalizeComposite(
        const ClaimTask& task, const ResourceRecord& root,
        const std::vector<ResourceRecord>& records,
        const std::map<std::string, std::vector<std::uint8_t>>& bytes_by_id,
        const BroadNormalizationExecutorConfig& config,
        bool& global_preview_only) {
    if (root.detected_kind != "CMPT") {
        throw formats::FormatError(
                formats::FormatErrorCode::cmpt_header_invalid,
                "COMPOSITE task root is not CMPT");
    }
    CompositeNormalizationInput input;
    input.root_package_relative_path = root.package_relative_path;
    input.source_bytes = bytes_by_id.at(root.object_id);
    input.approved_resources = approvedResources(
            records, bytes_by_id, root.object_id);
    auto normalized = CompositeNormalizer::normalize(
            input, config.mesh_profile, config.broad_profile.point,
            config.broad_profile.instance, config.mesh_validator,
            config.point_validator, config.instance_validator,
            config.broad_profile.composite);
    if (normalized.leaves.empty()
            || normalized.leaves.size() + 1U
                    > config.broad_profile.maximum_outputs) {
        throw formats::FormatError(
                formats::FormatErrorCode::cmpt_recursion_limit_exceeded,
                "Composite output count exceeds the profile");
    }
    std::vector<PreparedOutput> outputs;
    outputs.reserve(normalized.leaves.size() + 1U);
    Json children = Json::array();
    global_preview_only = false;
    for (const auto& leaf : normalized.leaves) {
        if (leaf.status == CompositeLeafStatus::failure) {
            throw formats::FormatError(
                    formats::FormatErrorCode::normalization_output_invalid,
                    "Composite child normalization failed");
        }
        std::optional<std::string> output_id;
        if (leaf.status == CompositeLeafStatus::success) {
            const CanonicalFamily family = fromV1Family(*leaf.canonical_family);
            outputs.push_back(contentOutput(
                    task, family, leaf.ordinal_path, leaf.canonical_bytes,
                    *leaf.evidence, leaf.point_count, leaf.instance_count));
            output_id = outputs.back().declaration.output_id;
        } else {
            global_preview_only = true;
        }
        children.push_back(compositeChildJson(task, leaf, output_id));
    }
    const Json manifest{{"children", std::move(children)},
                        {"compositeContractVersion",
                         task.canonical_contract_version},
                        {"globalPreviewOnly", global_preview_only},
                        {"manifestVersion", kCompositeParentManifestVersion},
                        {"parentTileContentId", task.tile_content_id},
                        {"sourceClosureHash", task.source_closure_hash}};
    const std::string manifest_text = manifest.dump();
    PreparedOutput parent;
    parent.bytes.assign(manifest_text.begin(), manifest_text.end());
    parent.declaration.output_kind = OutputKind::parent_manifest;
    parent.declaration.canonical_family = CanonicalFamily::composite_children;
    parent.declaration.canonical_contract_version =
            task.canonical_contract_version;
    parent.declaration.output_size = parent.bytes.size();
    parent.declaration.output_sha256 = sha256Hex(manifest_text);
    parent.declaration.semantic_hash = sha256Hex(
            std::string(kCompositeSemanticDomain) + "\n" + manifest_text);
    parent.declaration.validation_summary = parentValidationSummary(
            normalized.leaves.size(), config.composite_validator);
    parent.declaration.validation_manifest_sha256 = validationManifestSha256(
            parent.declaration.validation_summary);
    parent.declaration.output_id = outputId(task, parent.declaration);
    outputs.push_back(std::move(parent));
    return outputs;
}

void validateValidator(const ToolVersion& validator) {
    if (validator.name.empty() || validator.version.empty()
            || validator.build_sha256.size() != 64U) {
        throw std::invalid_argument(
                "Broad normalizer validator identity is incomplete");
    }
}

std::string failureCode(formats::FormatErrorCode code) {
    switch (code) {
        case formats::FormatErrorCode::pnts_header_invalid:
            return "PNTS_HEADER_INVALID";
        case formats::FormatErrorCode::pnts_feature_table_invalid:
            return "PNTS_FEATURE_TABLE_INVALID";
        case formats::FormatErrorCode::pnts_semantic_unsupported:
            return "PNTS_SEMANTIC_UNSUPPORTED";
        case formats::FormatErrorCode::pnts_quantization_invalid:
            return "PNTS_QUANTIZATION_INVALID";
        case formats::FormatErrorCode::pnts_normal_invalid:
            return "PNTS_NORMAL_INVALID";
        case formats::FormatErrorCode::point_output_invalid:
            return "POINT_CANONICAL_OUTPUT_INVALID";
        case formats::FormatErrorCode::i3dm_header_invalid:
            return "I3DM_HEADER_INVALID";
        case formats::FormatErrorCode::i3dm_feature_table_invalid:
            return "I3DM_FEATURE_TABLE_INVALID";
        case formats::FormatErrorCode::i3dm_semantic_unsupported:
            return "I3DM_SEMANTIC_UNSUPPORTED";
        case formats::FormatErrorCode::i3dm_enu_unsupported:
            return "I3DM_ENU_UNSUPPORTED";
        case formats::FormatErrorCode::i3dm_orientation_invalid:
            return "I3DM_ORIENTATION_INVALID";
        case formats::FormatErrorCode::i3dm_scale_invalid:
            return "I3DM_SCALE_INVALID";
        case formats::FormatErrorCode::instance_output_invalid:
            return "INSTANCE_CANONICAL_OUTPUT_INVALID";
        case formats::FormatErrorCode::instance_expansion_limit_exceeded:
            return "INSTANCE_EXPANSION_LIMIT_EXCEEDED";
        case formats::FormatErrorCode::cmpt_header_invalid:
            return "CMPT_HEADER_INVALID";
        case formats::FormatErrorCode::cmpt_recursion_limit_exceeded:
            return "CMPT_RECURSION_LIMIT_EXCEEDED";
        case formats::FormatErrorCode::cmpt_child_unsupported:
            return "CMPT_CHILD_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_batch_table_unsupported:
            return "METADATA_BATCH_TABLE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_feature_id_invalid:
            return "METADATA_FEATURE_ID_INVALID";
        case formats::FormatErrorCode::metadata_feature_id_texture_unsupported:
            return "METADATA_FEATURE_ID_TEXTURE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_feature_id_triangle_ambiguous:
            return "METADATA_FEATURE_ID_TRIANGLE_AMBIGUOUS";
        case formats::FormatErrorCode::metadata_property_table_invalid:
            return "METADATA_PROPERTY_TABLE_INVALID";
        case formats::FormatErrorCode::metadata_property_type_unsupported:
            return "METADATA_PROPERTY_TYPE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_property_attribute_unsupported:
            return "METADATA_PROPERTY_ATTRIBUTE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_property_texture_unsupported:
            return "METADATA_PROPERTY_TEXTURE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_hierarchy_invalid:
            return "METADATA_HIERARCHY_INVALID";
        case formats::FormatErrorCode::metadata_hierarchy_unsupported:
            return "METADATA_HIERARCHY_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_relationship_unsupported:
            return "METADATA_RELATIONSHIP_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_statistics_unsupported:
            return "METADATA_STATISTICS_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_unknown_required_extension:
            return "METADATA_UNKNOWN_REQUIRED_EXTENSION";
        case formats::FormatErrorCode::metadata_reconstruction_unsafe:
            return "METADATA_RECONSTRUCTION_UNSAFE";
        case formats::FormatErrorCode::metadata_leakage_verification_failed:
            return "METADATA_LEAKAGE_VERIFICATION_FAILED";
        case formats::FormatErrorCode::compression_draco_invalid:
            return "COMPRESSION_DRACO_INVALID";
        case formats::FormatErrorCode::compression_draco_unsupported:
            return "COMPRESSION_DRACO_UNSUPPORTED";
        case formats::FormatErrorCode::compression_draco_limit_exceeded:
            return "COMPRESSION_DRACO_LIMIT_EXCEEDED";
        case formats::FormatErrorCode::compression_meshopt_invalid:
            return "COMPRESSION_MESHOPT_INVALID";
        case formats::FormatErrorCode::compression_meshopt_unsupported:
            return "COMPRESSION_MESHOPT_UNSUPPORTED";
        case formats::FormatErrorCode::compression_meshopt_limit_exceeded:
            return "COMPRESSION_MESHOPT_LIMIT_EXCEEDED";
        case formats::FormatErrorCode::texture_invalid:
            return "TEXTURE_DECODE_INVALID";
        case formats::FormatErrorCode::texture_format_unsupported:
            return "TEXTURE_FORMAT_UNSUPPORTED";
        case formats::FormatErrorCode::texture_ktx2_unsupported:
            return "TEXTURE_KTX2_UNSUPPORTED";
        case formats::FormatErrorCode::texture_dimension_limit_exceeded:
            return "TEXTURE_DIMENSION_LIMIT_EXCEEDED";
        case formats::FormatErrorCode::texture_decoded_bytes_limit_exceeded:
            return "TEXTURE_DECODED_BYTES_LIMIT_EXCEEDED";
        case formats::FormatErrorCode::content_primitive_mode_unsupported:
            return "CONTENT_PRIMITIVE_MODE_UNSUPPORTED";
        case formats::FormatErrorCode::content_gltf_feature_unsupported:
            return "CONTENT_GLTF_FEATURE_UNSUPPORTED";
        case formats::FormatErrorCode::unsupported_version:
            return "CONTENT_VERSION_UNSUPPORTED";
        case formats::FormatErrorCode::unsupported_content:
        case formats::FormatErrorCode::unsupported_chunk:
            return "CANONICAL_CONTRACT_UNSUPPORTED";
        case formats::FormatErrorCode::clipping_output_invalid:
            return "CLIPPING_OUTPUT_INVALID";
        default:
            return "NORMALIZATION_OUTPUT_INVALID";
    }
}

bool unsupportedFailure(formats::FormatErrorCode code) {
    switch (code) {
        case formats::FormatErrorCode::pnts_semantic_unsupported:
        case formats::FormatErrorCode::i3dm_semantic_unsupported:
        case formats::FormatErrorCode::i3dm_enu_unsupported:
        case formats::FormatErrorCode::instance_expansion_limit_exceeded:
        case formats::FormatErrorCode::cmpt_recursion_limit_exceeded:
        case formats::FormatErrorCode::cmpt_child_unsupported:
        case formats::FormatErrorCode::metadata_batch_table_unsupported:
        case formats::FormatErrorCode::metadata_feature_id_texture_unsupported:
        case formats::FormatErrorCode::metadata_feature_id_triangle_ambiguous:
        case formats::FormatErrorCode::metadata_property_type_unsupported:
        case formats::FormatErrorCode::metadata_property_attribute_unsupported:
        case formats::FormatErrorCode::metadata_property_texture_unsupported:
        case formats::FormatErrorCode::metadata_hierarchy_unsupported:
        case formats::FormatErrorCode::metadata_relationship_unsupported:
        case formats::FormatErrorCode::metadata_statistics_unsupported:
        case formats::FormatErrorCode::metadata_unknown_required_extension:
        case formats::FormatErrorCode::metadata_reconstruction_unsafe:
        case formats::FormatErrorCode::compression_draco_unsupported:
        case formats::FormatErrorCode::compression_draco_limit_exceeded:
        case formats::FormatErrorCode::compression_meshopt_unsupported:
        case formats::FormatErrorCode::compression_meshopt_limit_exceeded:
        case formats::FormatErrorCode::texture_format_unsupported:
        case formats::FormatErrorCode::texture_ktx2_unsupported:
        case formats::FormatErrorCode::texture_dimension_limit_exceeded:
        case formats::FormatErrorCode::texture_decoded_bytes_limit_exceeded:
        case formats::FormatErrorCode::content_primitive_mode_unsupported:
        case formats::FormatErrorCode::content_gltf_feature_unsupported:
        case formats::FormatErrorCode::unsupported_version:
        case formats::FormatErrorCode::unsupported_content:
        case formats::FormatErrorCode::unsupported_chunk:
            return true;
        default: return false;
    }
}

}  // namespace

BroadNormalizationExecutor::BroadNormalizationExecutor(
        BroadNormalizationExecutorConfig config,
        client::ObjectTransfer object_transfer)
    : config_(std::move(config)),
      object_transfer_(std::move(object_transfer)) {
    if ((config_.broad_profile.profile_id != kBroadResourceProfileVersion
         && config_.broad_profile.profile_id != kMetadataResourceProfileVersion)
            || config_.mesh_profile.profile_id != kMeshResourceProfileVersion
            || config_.scratch_root.empty()) {
        throw std::invalid_argument(
                "Broad normalization executor configuration is incomplete");
    }
    validateValidator(config_.mesh_validator);
    validateValidator(config_.point_validator);
    validateValidator(config_.instance_validator);
    validateValidator(config_.composite_validator);
    if (config_.broad_profile.profile_id == kMetadataResourceProfileVersion) {
        config_.mesh_profile.b3dm.enable_feature_metadata = true;
        config_.mesh_profile.b3dm.feature_metadata =
                config_.broad_profile.metadata;
        config_.mesh_profile.b3dm.gltf.enable_feature_metadata = true;
        config_.mesh_profile.b3dm.gltf.metadata =
                config_.broad_profile.metadata;
    }
}

void BroadNormalizationExecutor::execute(
        NormalizationV2LeaseSession& session) const {
    heartbeat(session, TaskPhase::manifest_fetch, 0U);
    for (std::uint32_t page = 1U;
         page <= session.task().resource_manifest.page_count; ++page) {
        static_cast<void>(session.resourceManifestPage(page));
    }
    session.validateManifest();
    std::vector<ResourceRecord> records;
    for (const auto& page : session.manifestPages()) {
        records.insert(records.end(), page.records.begin(), page.records.end());
    }
    std::sort(records.begin(), records.end(),
              [](const ResourceRecord& left, const ResourceRecord& right) {
                  return left.object_id < right.object_id;
              });
    AttemptScratch scratch(config_.scratch_root, session.task());
    std::map<std::string, std::vector<std::uint8_t>> bytes_by_id;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const auto& record = records[index];
        const auto path = scratch.resourcePath(index);
        const auto observed = object_transfer_.downloadToFile(
                record.access_grant.url, path, record.size,
                [&session]() { return session.active(); });
        if (observed.observed_size != record.size
                || observed.sha256 != record.sha256) {
            throw std::invalid_argument(
                    "Broad normalizer resource identity mismatch");
        }
        bytes_by_id.emplace(record.object_id, readFile(path, record.size));
        heartbeat(session, TaskPhase::resource_download,
                  static_cast<std::uint64_t>(index + 1U));
    }
    const auto& root = rootRecord(records, session.task());
    heartbeat(session, TaskPhase::decode, records.size());
    std::vector<PreparedOutput> outputs;
    bool global_preview_only = false;
    switch (session.task().canonical_family) {
        case CanonicalFamily::point_gltf2:
            outputs.push_back(normalizePoint(
                    session.task(), root, bytes_by_id, config_));
            break;
        case CanonicalFamily::instance_gltf2:
            outputs.push_back(normalizeInstance(
                    session.task(), root, records, bytes_by_id, config_));
            break;
        case CanonicalFamily::composite_children:
            outputs = normalizeComposite(
                    session.task(), root, records, bytes_by_id, config_,
                    global_preview_only);
            break;
        case CanonicalFamily::mesh_gltf2:
            outputs.push_back(normalizeMesh(
                    session.task(), records, bytes_by_id, config_));
            break;
    }
    heartbeat(session, TaskPhase::normalize, records.size());
    if (outputs.empty() || outputs.size() > session.task().maximum_outputs) {
        throw formats::FormatError(
                formats::FormatErrorCode::normalization_output_invalid,
                "Broad normalizer produced an invalid output set");
    }
    std::vector<OutputDeclaration> ordered;
    ordered.reserve(outputs.size());
    heartbeat(session, TaskPhase::validate, records.size());
    heartbeat(session, TaskPhase::upload_prepare, records.size());
    for (auto& output : outputs) {
        const auto grant = session.prepareOutput(output.declaration);
        heartbeat(session, TaskPhase::upload, records.size());
        const std::string etag = object_transfer_.upload(
                grant.upload_url, output.bytes);
        session.reportOutput({output.declaration.output_id,
                              client::normalizeEtag(etag),
                              output.declaration.output_size,
                              output.declaration.output_sha256});
        ordered.push_back(output.declaration);
    }
    heartbeat(session, TaskPhase::complete, records.size());
    session.complete(ordered, global_preview_only);
}

Failure broadNormalizationFailure(const formats::FormatError& error) {
    Failure result;
    result.error_code = failureCode(error.code());
    // Public failure text stays bounded and does not echo parser payloads or URIs.
    result.error_message = "Broad 3D Tiles normalization rejected the source content";
    result.retryable = false;
    result.unsupported = unsupportedFailure(error.code());
    return result;
}

}  // namespace clip_worker::normalization::v2
