#include "clip_worker/normalization/mesh_normalization_executor.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace clip_worker::normalization {
namespace {

constexpr std::size_t kMaximumFailureMessageBytes = 512U;

class AttemptScratch final {
public:
    AttemptScratch(const std::filesystem::path& root,
                   const ClaimTask& task) {
        if (root.empty()) {
            throw std::invalid_argument(
                    "Normalizer scratch root is unavailable");
        }
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error) {
            throw std::runtime_error(
                    "Normalizer scratch root cannot be created");
        }
        root_ = std::filesystem::weakly_canonical(root, error);
        if (error) {
            throw std::runtime_error(
                    "Normalizer scratch root cannot be resolved");
        }
        const std::string identity = sha256Hex(
                task.task_id + "\n" + task.attempt_id);
        directory_ = (root_ / ("mesh-normalizer-" + identity)).lexically_normal();
        if (directory_.parent_path() != root_) {
            throw std::invalid_argument(
                    "Normalizer scratch identity escapes its root");
        }
        if (!std::filesystem::create_directory(directory_, error) || error) {
            throw std::runtime_error(
                    "Normalizer scratch attempt directory is unavailable");
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

std::vector<std::uint8_t> readFile(
        const std::filesystem::path& path,
        std::uint64_t expected_size) {
    if (expected_size > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(
                "Normalizer resource exceeds the addressable size");
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0
        || static_cast<std::uint64_t>(input.tellg()) != expected_size) {
        throw std::runtime_error(
                "Normalizer downloaded resource size is inconsistent");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(expected_size));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        throw std::runtime_error(
                "Normalizer downloaded resource cannot be read");
    }
    return bytes;
}

MeshSourceKind sourceKind(const std::string& value) {
    if (value == "GLB") return MeshSourceKind::glb;
    if (value == "GLTF") return MeshSourceKind::gltf;
    if (value == "B3DM") return MeshSourceKind::b3dm;
    throw formats::FormatError(
            formats::FormatErrorCode::content_gltf_feature_unsupported,
            "Normalizer CONTENT kind is not a Task-6 mesh source");
}

void heartbeat(NormalizationLeaseSession& session,
               TaskPhase phase,
               std::uint64_t processed_resources) {
    const auto response = session.heartbeat(phase, processed_resources);
    if (response.cancel_requested || !session.active()) {
        throw client::ObjectTransferCancelledError(
                "Normalizer task was cancelled");
    }
}

std::string safeMessage(const std::string& message) {
    const std::string fallback = "Mesh normalization failed safely";
    const std::string& selected = message.empty() ? fallback : message;
    return selected.substr(0U, kMaximumFailureMessageBytes);
}

const char* failureCode(formats::FormatErrorCode code) {
    switch (code) {
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
        case formats::FormatErrorCode::normalization_output_invalid:
            return "NORMALIZATION_OUTPUT_INVALID";
        case formats::FormatErrorCode::clipping_output_invalid:
            return "CLIPPING_OUTPUT_INVALID";
        case formats::FormatErrorCode::unsupported_version:
            return "CONTENT_VERSION_UNSUPPORTED";
        case formats::FormatErrorCode::unsupported_content:
        case formats::FormatErrorCode::unsupported_chunk:
            return "CANONICAL_CONTRACT_UNSUPPORTED";
        default:
            return "CANONICAL_OUTPUT_INVALID";
    }
}

bool unsupportedFailure(formats::FormatErrorCode code) {
    return code != formats::FormatErrorCode::normalization_output_invalid
            && code != formats::FormatErrorCode::clipping_output_invalid
            && code != formats::FormatErrorCode::metadata_feature_id_invalid
            && code != formats::FormatErrorCode::metadata_property_table_invalid
            && code != formats::FormatErrorCode::metadata_hierarchy_invalid
            && code
                    != formats::FormatErrorCode::metadata_leakage_verification_failed;
}

}  // namespace

MeshNormalizationExecutor::MeshNormalizationExecutor(
        MeshNormalizationExecutorConfig config,
        client::ObjectTransfer object_transfer)
    : config_(std::move(config)),
      object_transfer_(std::move(object_transfer)) {
    if (config_.profile.profile_id != kMeshResourceProfileVersion
        || config_.validator.name.empty()
        || config_.validator.version.empty()
        || config_.validator.build_sha256.size() != 64U
        || config_.scratch_root.empty()) {
        throw std::invalid_argument(
                "Mesh normalization executor configuration is incomplete");
    }
}

MeshNormalizationInput MeshNormalizationExecutor::buildInput(
        const std::vector<ResourceRecord>& records,
        const std::map<std::string, std::vector<std::uint8_t>>& bytes_by_id) {
    const ResourceRecord* root = nullptr;
    for (const auto& record : records) {
        const auto bytes = bytes_by_id.find(record.object_id);
        if (bytes == bytes_by_id.end()
            || bytes->second.size() != record.size
            || client::sha256Hex(bytes->second) != record.sha256) {
            throw std::invalid_argument(
                    "Normalizer resource bytes differ from the manifest");
        }
        if (record.resource_role == "CONTENT") {
            if (root != nullptr) {
                throw std::invalid_argument(
                        "Normalizer manifest has multiple CONTENT roots");
            }
            root = &record;
        }
    }
    if (root == nullptr) {
        throw std::invalid_argument(
                "Normalizer manifest has no CONTENT root");
    }
    MeshNormalizationInput input;
    input.source_kind = sourceKind(root->detected_kind);
    input.root_package_relative_path = root->package_relative_path;
    input.source_bytes = bytes_by_id.at(root->object_id);
    for (const auto& record : records) {
        if (record.object_id == root->object_id) continue;
        const bool inserted = input.approved_resources.emplace(
                record.package_relative_path,
                formats::ApprovedGltfResource{
                        bytes_by_id.at(record.object_id),
                        record.media_type.value_or(std::string{})})
                                      .second;
        if (!inserted) {
            throw std::invalid_argument(
                    "Normalizer manifest has a duplicate resource path");
        }
    }
    return input;
}

void MeshNormalizationExecutor::execute(
        NormalizationLeaseSession& session) const {
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
                    "Downloaded resource identity differs from the manifest");
        }
        bytes_by_id.emplace(record.object_id, readFile(path, record.size));
        heartbeat(session, TaskPhase::resource_download,
                  static_cast<std::uint64_t>(index + 1U));
    }
    heartbeat(session, TaskPhase::decode, records.size());
    const auto input = buildInput(records, bytes_by_id);
    heartbeat(session, TaskPhase::normalize, records.size());
    const auto normalized = MeshNormalizer::normalize(
            input, config_.profile, config_.validator);
    heartbeat(session, TaskPhase::validate, records.size());
    const auto declaration = normalized.evidence.uploadDeclaration(
            CanonicalFamily::mesh_gltf2);
    heartbeat(session, TaskPhase::upload_prepare, records.size());
    const auto grant = session.prepareUpload(declaration);
    heartbeat(session, TaskPhase::upload, records.size());
    const std::string etag = object_transfer_.upload(
            grant.upload_url, normalized.canonical.glb);
    session.reportUpload({client::normalizeEtag(etag),
                          declaration.output_size,
                          declaration.output_sha256});
    heartbeat(session, TaskPhase::complete, records.size());
    session.complete();
}

Failure meshNormalizationFailure(const formats::FormatError& error) {
    Failure failure;
    failure.error_code = failureCode(error.code());
    failure.error_message = safeMessage(error.what());
    failure.retryable = false;
    failure.unsupported = unsupportedFailure(error.code());
    return failure;
}

}  // namespace clip_worker::normalization
