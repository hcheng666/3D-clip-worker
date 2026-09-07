#include "clip_worker/authorization/clipper_v2_executor.hpp"
#include "clip_worker/client/clipper_v2_api_client.hpp"
#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/client/worker_api_client.hpp"
#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/logging/logger.hpp"
#include "clip_worker/normalization/broad_normalization_executor.hpp"
#include "clip_worker/normalization/mesh_normalization_executor.hpp"
#include "clip_worker/normalization/mesh_resource_profile.hpp"
#include "clip_worker/normalization/normalization_runtime.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"
#include "clip_worker/task/worker_runtime.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr std::uintmax_t kMebibyte = 1024U * 1024U;
constexpr std::uintmax_t kMaxInspectInputBytes = 512U * kMebibyte;
constexpr const char* kVersion = CLIP_WORKER_VERSION;
constexpr const char* kControlPlaneUrlVariable = "CLIP_WORKER_CONTROL_PLANE_URL";
constexpr const char* kWorkerIdVariable = "CLIP_WORKER_ID";
constexpr const char* kHostnameVariable = "HOSTNAME";
constexpr const char* kComputerNameVariable = "COMPUTERNAME";
constexpr const char* kAuthorizationHeaderVariable = "CLIP_WORKER_AUTHORIZATION_HEADER";
constexpr const char* kAlgorithmVersionVariable = "CLIP_WORKER_ALGORITHM_VERSION";
constexpr const char* kMaximumInputBytesVariable = "CLIP_WORKER_MAX_INPUT_BYTES";
constexpr const char* kMaximumOutputBytesVariable = "CLIP_WORKER_MAX_OUTPUT_BYTES";
constexpr const char* kPollIntervalVariable = "CLIP_WORKER_POLL_INTERVAL_SECONDS";
constexpr const char* kHeartbeatIntervalVariable = "CLIP_WORKER_HEARTBEAT_INTERVAL_SECONDS";
constexpr const char* kApiConnectTimeoutVariable = "CLIP_WORKER_API_CONNECT_TIMEOUT_SECONDS";
constexpr const char* kApiRequestTimeoutVariable = "CLIP_WORKER_API_REQUEST_TIMEOUT_SECONDS";
constexpr const char* kTransferConnectTimeoutVariable =
        "CLIP_WORKER_TRANSFER_CONNECT_TIMEOUT_SECONDS";
constexpr const char* kTransferRequestTimeoutVariable =
        "CLIP_WORKER_TRANSFER_REQUEST_TIMEOUT_SECONDS";
constexpr const char* kLogLevelVariable = "CLIP_WORKER_LOG_LEVEL";
constexpr const char* kNormalizerControlPlaneUrlVariable =
        "CLIP_WORKER_NORMALIZER_CONTROL_PLANE_URL";
constexpr const char* kNormalizerAuthorizationHeaderVariable =
        "CLIP_WORKER_NORMALIZER_AUTHORIZATION_HEADER";
constexpr const char* kNormalizerWorkerIdVariable =
        "CLIP_WORKER_NORMALIZER_ID";
constexpr const char* kBroadNormalizerWorkerIdVariable =
        "CLIP_WORKER_BROAD_NORMALIZER_ID";
constexpr const char* kMetadataNormalizerWorkerIdVariable =
        "CLIP_WORKER_METADATA_NORMALIZER_ID";
constexpr const char* kNormalizerProfilePathVariable =
        "CLIP_WORKER_NORMALIZER_RESOURCE_PROFILE_PATH";
constexpr const char* kNormalizerProfileSha256Variable =
        "CLIP_WORKER_NORMALIZER_RESOURCE_PROFILE_SHA256";
constexpr const char* kNormalizerValidatorNameVariable =
        "CLIP_WORKER_NORMALIZER_MESH_VALIDATOR_NAME";
constexpr const char* kNormalizerValidatorVersionVariable =
        "CLIP_WORKER_NORMALIZER_MESH_VALIDATOR_VERSION";
constexpr const char* kNormalizerValidatorBuildSha256Variable =
        "CLIP_WORKER_NORMALIZER_MESH_VALIDATOR_BUILD_SHA256";
constexpr const char* kPointValidatorNameVariable =
        "CLIP_WORKER_NORMALIZER_POINT_VALIDATOR_NAME";
constexpr const char* kPointValidatorVersionVariable =
        "CLIP_WORKER_NORMALIZER_POINT_VALIDATOR_VERSION";
constexpr const char* kPointValidatorBuildSha256Variable =
        "CLIP_WORKER_NORMALIZER_POINT_VALIDATOR_BUILD_SHA256";
constexpr const char* kInstanceValidatorNameVariable =
        "CLIP_WORKER_NORMALIZER_INSTANCE_VALIDATOR_NAME";
constexpr const char* kInstanceValidatorVersionVariable =
        "CLIP_WORKER_NORMALIZER_INSTANCE_VALIDATOR_VERSION";
constexpr const char* kInstanceValidatorBuildSha256Variable =
        "CLIP_WORKER_NORMALIZER_INSTANCE_VALIDATOR_BUILD_SHA256";
constexpr const char* kCompositeValidatorNameVariable =
        "CLIP_WORKER_NORMALIZER_COMPOSITE_VALIDATOR_NAME";
constexpr const char* kCompositeValidatorVersionVariable =
        "CLIP_WORKER_NORMALIZER_COMPOSITE_VALIDATOR_VERSION";
constexpr const char* kCompositeValidatorBuildSha256Variable =
        "CLIP_WORKER_NORMALIZER_COMPOSITE_VALIDATOR_BUILD_SHA256";
constexpr const char* kBroadProfilePathVariable =
        "CLIP_WORKER_BROAD_RESOURCE_PROFILE_PATH";
constexpr const char* kBroadProfileSha256Variable =
        "CLIP_WORKER_BROAD_RESOURCE_PROFILE_SHA256";
constexpr const char* kMetadataProfilePathVariable =
        "CLIP_WORKER_METADATA_RESOURCE_PROFILE_PATH";
constexpr const char* kMetadataProfileSha256Variable =
        "CLIP_WORKER_METADATA_RESOURCE_PROFILE_SHA256";
constexpr const char* kBroadPointEnabledVariable =
        "CLIP_WORKER_BROAD_POINT_ENABLED";
constexpr const char* kBroadInstanceEnabledVariable =
        "CLIP_WORKER_BROAD_INSTANCE_ENABLED";
constexpr const char* kBroadCompositeEnabledVariable =
        "CLIP_WORKER_BROAD_COMPOSITE_ENABLED";
constexpr const char* kMetadataMeshEnabledVariable =
        "CLIP_WORKER_METADATA_MESH_ENABLED";
constexpr const char* kMetadataPointEnabledVariable =
        "CLIP_WORKER_METADATA_POINT_ENABLED";
constexpr const char* kMetadataInstanceEnabledVariable =
        "CLIP_WORKER_METADATA_INSTANCE_ENABLED";
constexpr const char* kMetadataCompositeEnabledVariable =
        "CLIP_WORKER_METADATA_COMPOSITE_ENABLED";
constexpr const char* kDracoDecoderEnabledVariable =
        "CLIP_WORKER_NORMALIZER_DRACO_ENABLED";
constexpr const char* kMeshoptDecoderEnabledVariable =
        "CLIP_WORKER_NORMALIZER_MESHOPT_ENABLED";
constexpr const char* kPngDecoderEnabledVariable =
        "CLIP_WORKER_NORMALIZER_PNG_ENABLED";
constexpr const char* kJpegDecoderEnabledVariable =
        "CLIP_WORKER_NORMALIZER_JPEG_ENABLED";
constexpr const char* kWebpDecoderEnabledVariable =
        "CLIP_WORKER_NORMALIZER_WEBP_ENABLED";
constexpr const char* kKtx2DecoderEnabledVariable =
        "CLIP_WORKER_NORMALIZER_KTX2_ENABLED";
constexpr const char* kClipperV2ControlPlaneUrlVariable =
        "CLIP_WORKER_CLIPPER_V2_CONTROL_PLANE_URL";
constexpr const char* kClipperV2AuthorizationHeaderVariable =
        "CLIP_WORKER_CLIPPER_V2_AUTHORIZATION_HEADER";
constexpr const char* kClipperV2WorkerIdVariable =
        "CLIP_WORKER_CLIPPER_V2_ID";
constexpr const char* kClipperV2MeshEnabledVariable =
        "CLIP_WORKER_CLIPPER_V2_MESH_ENABLED";
constexpr const char* kClipperV2PointEnabledVariable =
        "CLIP_WORKER_CLIPPER_V2_POINT_ENABLED";
constexpr const char* kClipperV2InstanceEnabledVariable =
        "CLIP_WORKER_CLIPPER_V2_INSTANCE_ENABLED";
constexpr const char* kNormalizerScratchRootVariable =
        "CLIP_WORKER_NORMALIZER_SCRATCH_ROOT";
constexpr const char* kNormalizerPollIntervalVariable =
        "CLIP_WORKER_NORMALIZER_POLL_INTERVAL_SECONDS";
constexpr const char* kNormalizerApiConnectTimeoutVariable =
        "CLIP_WORKER_NORMALIZER_API_CONNECT_TIMEOUT_SECONDS";
constexpr const char* kNormalizerApiRequestTimeoutVariable =
        "CLIP_WORKER_NORMALIZER_API_REQUEST_TIMEOUT_SECONDS";
constexpr const char* kNormalizerTransferConnectTimeoutVariable =
        "CLIP_WORKER_NORMALIZER_TRANSFER_CONNECT_TIMEOUT_SECONDS";
constexpr const char* kNormalizerTransferRequestTimeoutVariable =
        "CLIP_WORKER_NORMALIZER_TRANSFER_REQUEST_TIMEOUT_SECONDS";
constexpr const char* kDefaultNormalizerProfilePath =
        "/usr/local/share/clip-worker/resource-limit-profiles-v2.json";
constexpr const char* kDefaultBroadProfilePath =
        "/usr/local/share/clip-worker/resource-limit-profiles-v3.json";
constexpr const char* kDefaultMetadataProfilePath =
        "/usr/local/share/clip-worker/resource-limit-profiles-v4.json";
constexpr const char* kDefaultNormalizerScratchRoot =
        "/tmp/3d-tiles-normalizer";
constexpr long kDefaultNormalizerPollIntervalSeconds = 5;
constexpr const char* kDefaultLogLevel = "INFO";
constexpr const char* kEventApplicationStartFailed = "application.start_failed";
constexpr const char* kEventApplicationCommandFailed = "application.command_failed";

std::atomic_bool stop_requested{false};

void requestStop(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

std::optional<std::string> environmentValue(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::string requiredEnvironmentValue(const char* name) {
    const auto value = environmentValue(name);
    if (!value.has_value()) {
        throw std::invalid_argument(std::string("Required environment variable is missing: ")
                                    + name);
    }
    return *value;
}

template <typename Integer>
Integer positiveEnvironmentValue(const char* name, Integer fallback) {
    const auto value = environmentValue(name);
    if (!value.has_value()) {
        return fallback;
    }
    Integer parsed{};
    const auto converted = std::from_chars(value->data(), value->data() + value->size(),
                                           parsed);
    if (converted.ec != std::errc() || converted.ptr != value->data() + value->size()
        || parsed <= 0) {
        throw std::invalid_argument(std::string("Environment variable must be a positive integer: ")
                                    + name);
    }
    return parsed;
}

std::string workerId() {
    for (const char* name : {kWorkerIdVariable, kHostnameVariable, kComputerNameVariable}) {
        const auto value = environmentValue(name);
        if (value.has_value()) {
            return *value;
        }
    }
    throw std::invalid_argument(
            "CLIP_WORKER_ID is required when no host name environment variable is available");
}

std::string normalizerWorkerId() {
    const auto explicit_id = environmentValue(kNormalizerWorkerIdVariable);
    if (explicit_id.has_value()) return *explicit_id;
    return workerId() + "-normalizer";
}

std::string broadNormalizerWorkerId() {
    const auto explicit_id = environmentValue(kBroadNormalizerWorkerIdVariable);
    if (explicit_id.has_value()) return *explicit_id;
    return normalizerWorkerId() + "-broad";
}

std::string metadataNormalizerWorkerId() {
    const auto explicit_id = environmentValue(kMetadataNormalizerWorkerIdVariable);
    if (explicit_id.has_value()) return *explicit_id;
    return normalizerWorkerId() + "-metadata";
}

std::string clipperV2WorkerId() {
    const auto explicit_id = environmentValue(kClipperV2WorkerIdVariable);
    if (explicit_id.has_value()) return *explicit_id;
    return workerId() + "-clipper-v2";
}

bool enabledEnvironmentValue(const char* name) {
    const auto value = environmentValue(name);
    if (!value.has_value()) return false;
    if (*value == "true" || *value == "TRUE" || *value == "1") return true;
    if (*value == "false" || *value == "FALSE" || *value == "0") return false;
    throw std::invalid_argument(
            std::string("Environment variable must be boolean: ") + name);
}

std::vector<std::string> decoderCapabilities() {
    std::vector<std::string> decoders;
    if (enabledEnvironmentValue(kDracoDecoderEnabledVariable)) {
        decoders.emplace_back("DRACO");
    }
    if (enabledEnvironmentValue(kJpegDecoderEnabledVariable)) {
        decoders.emplace_back("JPEG");
    }
    if (enabledEnvironmentValue(kKtx2DecoderEnabledVariable)) {
        decoders.emplace_back("KTX2");
    }
    if (enabledEnvironmentValue(kMeshoptDecoderEnabledVariable)) {
        decoders.emplace_back("MESHOPT");
    }
    if (enabledEnvironmentValue(kPngDecoderEnabledVariable)) {
        decoders.emplace_back("PNG");
    }
    if (enabledEnvironmentValue(kWebpDecoderEnabledVariable)) {
        decoders.emplace_back("WEBP");
    }
    return decoders;
}

std::string utcNowSeconds() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &value) != 0) {
#else
    if (gmtime_r(&value, &utc) == nullptr) {
#endif
        throw std::runtime_error("Unable to compute normalizer UTC time");
    }
    std::array<char, 21U> text{};
    if (std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc)
        == 0U) {
        throw std::runtime_error("Unable to format normalizer UTC time");
    }
    return text.data();
}

std::vector<std::uint8_t> readBinary(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > kMaxInspectInputBytes) {
        throw std::invalid_argument("Input exceeds the inspect command size limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Unable to open input file: " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw std::runtime_error("Unable to read complete input file: " + path.string());
        }
    }
    return bytes;
}

int inspect(const std::filesystem::path& path) {
    const auto bytes = readBinary(path);
    const auto document = clip_worker::formats::B3dmParser::parse(
            clip_worker::formats::ByteView(bytes));
    const nlohmann::json report = {
            {"format", "B3DM"},
            {"version", document.header.version},
            {"byteLength", document.header.byte_length},
            {"batchLength", document.batch_length},
            {"featureTableJsonLength", document.header.feature_table_json_length},
            {"featureTableBinaryLength", document.header.feature_table_binary_length},
            {"batchTableJsonLength", document.header.batch_table_json_length},
            {"batchTableBinaryLength", document.header.batch_table_binary_length},
            {"layout",
             {{"glbOffset", document.layout.glb_offset},
              {"glbByteLength", document.layout.glb_byte_length},
              {"trailingPaddingBytes", document.layout.trailing_padding_bytes},
              {"glbOffsetStandardAligned",
               document.layout.glb_offset_standard_aligned},
              {"tileLengthStandardAligned",
               document.layout.tile_length_standard_aligned},
              {"requiresCompatibility", document.layout.requiresCompatibility()}}},
            {"glb",
             {{"version", document.glb.header.version},
              {"byteLength", document.glb.header.byte_length},
              {"chunkCount", document.glb.chunks.size()},
              {"binaryLength", document.glb.binary_length}}}};
    std::cout << report.dump(2) << '\n';
    return 0;
}

int runWorker() {
    const clip_worker::logging::Logger logger(
            clip_worker::logging::parseLogLevel(
                    environmentValue(kLogLevelVariable).value_or(kDefaultLogLevel)));
    clip_worker::client::WorkerApiClientConfig api_config;
    api_config.base_url = requiredEnvironmentValue(kControlPlaneUrlVariable);
    api_config.authorization_header =
            environmentValue(kAuthorizationHeaderVariable).value_or("");
    api_config.connect_timeout_seconds = positiveEnvironmentValue<long>(
            kApiConnectTimeoutVariable, api_config.connect_timeout_seconds);
    api_config.request_timeout_seconds = positiveEnvironmentValue<long>(
            kApiRequestTimeoutVariable, api_config.request_timeout_seconds);

    clip_worker::task::WorkerRuntimeConfig runtime_config;
    runtime_config.worker_id = workerId();
    runtime_config.algorithm_version = environmentValue(kAlgorithmVersionVariable)
            .value_or(clip_worker::task::WorkerRuntimeConfig::kDefaultAlgorithmVersion);
    runtime_config.maximum_input_bytes = positiveEnvironmentValue<std::uint64_t>(
            kMaximumInputBytesVariable, runtime_config.maximum_input_bytes);
    runtime_config.maximum_output_bytes = positiveEnvironmentValue<std::uint64_t>(
            kMaximumOutputBytesVariable, runtime_config.maximum_output_bytes);
    runtime_config.poll_interval_seconds = positiveEnvironmentValue<long>(
            kPollIntervalVariable, runtime_config.poll_interval_seconds);
    runtime_config.heartbeat_interval_seconds = positiveEnvironmentValue<long>(
            kHeartbeatIntervalVariable, runtime_config.heartbeat_interval_seconds);

    const long transfer_connect_timeout = positiveEnvironmentValue<long>(
            kTransferConnectTimeoutVariable,
            clip_worker::client::ObjectTransfer::kDefaultConnectTimeoutSeconds);
    const long transfer_request_timeout = positiveEnvironmentValue<long>(
            kTransferRequestTimeoutVariable,
            clip_worker::client::ObjectTransfer::kDefaultRequestTimeoutSeconds);

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    clip_worker::task::WorkerRuntime runtime(
            std::move(runtime_config),
             clip_worker::client::WorkerApiClient(std::move(api_config)),
             clip_worker::client::ObjectTransfer(transfer_connect_timeout,
                                                 transfer_request_timeout),
             logger);
    runtime.run(stop_requested);
    return 0;
}

void failNormalizerSession(
        clip_worker::normalization::NormalizationLeaseSession& session,
        const clip_worker::normalization::Failure& failure) {
    if (session.active()) session.fail(failure);
}

void failBroadNormalizerSession(
        clip_worker::normalization::v2::NormalizationV2LeaseSession& session,
        const clip_worker::normalization::Failure& failure) {
    if (session.active()) session.fail(failure);
}

void failClipperV2Session(
        clip_worker::authorization::v2::ClipperV2LeaseSession& session,
        const clip_worker::authorization::v2::Failure& failure) {
    if (session.active()) session.fail(failure);
}

int runAuthorizedClipper() {
    using namespace clip_worker;
    namespace clipper = authorization::v2;
    const bool mesh_enabled = enabledEnvironmentValue(
            kClipperV2MeshEnabledVariable);
    const bool point_enabled = enabledEnvironmentValue(
            kClipperV2PointEnabledVariable);
    const bool instance_enabled = enabledEnvironmentValue(
            kClipperV2InstanceEnabledVariable);
    if (!mesh_enabled && !point_enabled && !instance_enabled) {
        throw std::invalid_argument(
                "All Clipper V2 canonical family flags are disabled");
    }

    const logging::Logger logger(
            logging::parseLogLevel(
                    environmentValue(kLogLevelVariable)
                            .value_or(kDefaultLogLevel)));
    const auto mesh_profile = normalization::MeshResourceProfile::load(
            environmentValue(kNormalizerProfilePathVariable)
                    .value_or(kDefaultNormalizerProfilePath),
            normalization::kMeshResourceProfileVersion,
            requiredEnvironmentValue(kNormalizerProfileSha256Variable));
    const auto broad_profile = normalization::BroadResourceProfile::load(
            environmentValue(kMetadataProfilePathVariable)
                    .value_or(kDefaultMetadataProfilePath),
            normalization::kMetadataResourceProfileVersion,
            requiredEnvironmentValue(kMetadataProfileSha256Variable));
    normalization::ToolVersion mesh_validator;
    normalization::ToolVersion point_validator;
    normalization::ToolVersion instance_validator;
    if (mesh_enabled || instance_enabled) {
        // Instance boundary expansion emits mesh output and therefore also
        // requires the pinned mesh validator identity.
        mesh_validator = {
                requiredEnvironmentValue(kNormalizerValidatorNameVariable),
                requiredEnvironmentValue(kNormalizerValidatorVersionVariable),
                requiredEnvironmentValue(
                        kNormalizerValidatorBuildSha256Variable)};
    }
    if (point_enabled) {
        point_validator = {
                requiredEnvironmentValue(kPointValidatorNameVariable),
                requiredEnvironmentValue(kPointValidatorVersionVariable),
                requiredEnvironmentValue(
                        kPointValidatorBuildSha256Variable)};
    }
    if (instance_enabled) {
        instance_validator = {
                requiredEnvironmentValue(kInstanceValidatorNameVariable),
                requiredEnvironmentValue(kInstanceValidatorVersionVariable),
                requiredEnvironmentValue(
                        kInstanceValidatorBuildSha256Variable)};
    }
    const normalization::ToolVersion fallback = !mesh_validator.name.empty()
            ? mesh_validator
            : !point_validator.name.empty() ? point_validator
                                            : instance_validator;
    if (mesh_validator.name.empty()) mesh_validator = fallback;
    if (point_validator.name.empty()) point_validator = fallback;
    if (instance_validator.name.empty()) instance_validator = fallback;

    client::NormalizationApiClientConfig api_config;
    api_config.base_url = requiredEnvironmentValue(
            kClipperV2ControlPlaneUrlVariable);
    api_config.authorization_header = environmentValue(
            kClipperV2AuthorizationHeaderVariable).value_or("");
    api_config.connect_timeout_seconds = positiveEnvironmentValue<long>(
            kNormalizerApiConnectTimeoutVariable,
            api_config.connect_timeout_seconds);
    api_config.request_timeout_seconds = positiveEnvironmentValue<long>(
            kNormalizerApiRequestTimeoutVariable,
            api_config.request_timeout_seconds);
    const long transfer_connect_timeout = positiveEnvironmentValue<long>(
            kNormalizerTransferConnectTimeoutVariable,
            client::ObjectTransfer::kDefaultConnectTimeoutSeconds);
    const long transfer_request_timeout = positiveEnvironmentValue<long>(
            kNormalizerTransferRequestTimeoutVariable,
            client::ObjectTransfer::kDefaultRequestTimeoutSeconds);
    const long poll_interval = positiveEnvironmentValue<long>(
            kNormalizerPollIntervalVariable,
            kDefaultNormalizerPollIntervalSeconds);

    clipper::ClaimRequest capabilities;
    capabilities.worker_id = clipperV2WorkerId();
    capabilities.resource_profile_sha256 = broad_profile.document_sha256;
    const auto add_family = [&capabilities, &mesh_profile](
            clipper::CanonicalFamily family,
            const normalization::ToolVersion& validator,
            const char* canonical_contract_version,
            const char* clip_strategy_version) {
        capabilities.family_capabilities.push_back({
                clipper::kProtocolVersion, family,
                canonical_contract_version, clip_strategy_version,
                validator.name, validator.version, validator.build_sha256,
                mesh_profile.maximum_input_bytes,
                mesh_profile.maximum_output_bytes});
    };
    if (mesh_enabled) {
        add_family(clipper::CanonicalFamily::mesh_gltf2, mesh_validator,
                   normalization::v3::kMeshCanonicalContractVersion,
                   clipper::kMeshClipStrategyVersion);
    }
    if (point_enabled) {
        add_family(clipper::CanonicalFamily::point_gltf2, point_validator,
                   normalization::v3::kPointCanonicalContractVersion,
                   clipper::kPointClipStrategyVersion);
    }
    if (instance_enabled) {
        add_family(clipper::CanonicalFamily::instance_gltf2,
                   instance_validator,
                   normalization::v3::kInstanceCanonicalContractVersion,
                   clipper::kInstanceClipStrategyVersion);
    }

    clipper::ClipperV2TaskRuntime runtime(
            capabilities,
            client::ClipperV2ApiClient(std::move(api_config)));
    clipper::ClipperV2Executor executor(
            {mesh_profile, broad_profile, mesh_validator,
             point_validator, instance_validator},
            client::ObjectTransfer(transfer_connect_timeout,
                                   transfer_request_timeout));
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    logger.info("clipper_v2.started", "Authorized Clipper V2 started",
                {{"workerId", capabilities.worker_id},
                 {"resourceProfileVersion", broad_profile.profile_id},
                 {"meshEnabled", mesh_enabled},
                 {"pointEnabled", point_enabled},
                 {"instanceEnabled", instance_enabled}});
    while (!stop_requested.load(std::memory_order_relaxed)) {
        bool claimed = false;
        try {
            claimed = runtime.runOnce(
                    utcNowSeconds(),
                    [&executor](clipper::ClipperV2LeaseSession& session) {
                try {
                    executor.execute(session);
                } catch (const formats::FormatError& error) {
                    auto failure = clipper::clipperV2Failure(error);
                    if (session.phase() == clipper::TaskPhase::classifying) {
                        failure.error_code =
                                clipper::FailureCode::classification_failed;
                    } else if (session.phase()
                            == clipper::TaskPhase::clipping) {
                        failure.error_code = clipper::FailureCode::clip_failed;
                    }
                    failClipperV2Session(session, failure);
                } catch (const client::ObjectTransferCancelledError&) {
                    // Lease loss or shutdown intentionally stops remote I/O.
                } catch (const client::ObjectTransferError&) {
                    const bool downloading = session.phase()
                            == clipper::TaskPhase::downloading;
                    failClipperV2Session(
                            session,
                            {downloading
                                     ? clipper::FailureCode::download_failed
                                     : clipper::FailureCode::output_upload_failed,
                             downloading
                                     ? "Clipper V2 canonical input download failed"
                                     : "Clipper V2 canonical output upload failed",
                             true});
                } catch (const std::invalid_argument&) {
                    failClipperV2Session(
                            session,
                            {clipper::FailureCode::input_validation_failed,
                             "Clipper V2 rejected a bounded task envelope",
                             false});
                } catch (const std::exception&) {
                    failClipperV2Session(
                            session,
                            {clipper::FailureCode::internal_error,
                             "Clipper V2 encountered a bounded internal failure",
                             true});
                }
            });
        } catch (const std::exception& error) {
            logger.warning("clipper_v2.claim_failed", error.what(),
                           {{"workerId", capabilities.worker_id}});
        }
        if (!claimed) {
            for (long elapsed = 0;
                 elapsed < poll_interval
                 && !stop_requested.load(std::memory_order_relaxed);
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    logger.info("clipper_v2.stopped", "Authorized Clipper V2 stopped",
                {{"workerId", capabilities.worker_id}});
    return 0;
}

int runNormalizer() {
    using namespace clip_worker;
    const logging::Logger logger(
            logging::parseLogLevel(
                    environmentValue(kLogLevelVariable)
                            .value_or(kDefaultLogLevel)));
    const std::filesystem::path profile_path = environmentValue(
            kNormalizerProfilePathVariable)
            .value_or(kDefaultNormalizerProfilePath);
    const auto profile = normalization::MeshResourceProfile::load(
            profile_path, normalization::kMeshResourceProfileVersion,
            requiredEnvironmentValue(kNormalizerProfileSha256Variable));
    normalization::ToolVersion validator{
            requiredEnvironmentValue(kNormalizerValidatorNameVariable),
            requiredEnvironmentValue(kNormalizerValidatorVersionVariable),
            requiredEnvironmentValue(kNormalizerValidatorBuildSha256Variable)};

    client::NormalizationApiClientConfig api_config;
    api_config.base_url = requiredEnvironmentValue(
            kNormalizerControlPlaneUrlVariable);
    api_config.authorization_header = environmentValue(
            kNormalizerAuthorizationHeaderVariable).value_or("");
    api_config.connect_timeout_seconds = positiveEnvironmentValue<long>(
            kNormalizerApiConnectTimeoutVariable,
            api_config.connect_timeout_seconds);
    api_config.request_timeout_seconds = positiveEnvironmentValue<long>(
            kNormalizerApiRequestTimeoutVariable,
            api_config.request_timeout_seconds);
    const long transfer_connect_timeout = positiveEnvironmentValue<long>(
            kNormalizerTransferConnectTimeoutVariable,
            client::ObjectTransfer::kDefaultConnectTimeoutSeconds);
    const long transfer_request_timeout = positiveEnvironmentValue<long>(
            kNormalizerTransferRequestTimeoutVariable,
            client::ObjectTransfer::kDefaultRequestTimeoutSeconds);
    const long poll_interval = positiveEnvironmentValue<long>(
            kNormalizerPollIntervalVariable,
            kDefaultNormalizerPollIntervalSeconds);

    normalization::ClaimRequest capabilities;
    capabilities.worker_id = normalizerWorkerId();
    capabilities.decoder_capabilities = decoderCapabilities();
    capabilities.supported_normalization_versions = {
            normalization::kMeshNormalizationVersion};
    capabilities.family_capabilities = {{
            normalization::CanonicalFamily::mesh_gltf2,
            normalization::kCanonicalContractVersion,
            validator.name,
            validator.version,
            validator.build_sha256,
            profile.maximum_input_bytes,
            profile.maximum_output_bytes}};
    capabilities.resource_profile_version = profile.profile_id;
    capabilities.resource_profile_sha256 = profile.document_sha256;
    capabilities.tool_versions = {validator};

    normalization::NormalizationTaskRuntime runtime(
            capabilities,
            client::NormalizationApiClient(std::move(api_config)));
    normalization::MeshNormalizationExecutor executor(
            {profile, validator,
             environmentValue(kNormalizerScratchRootVariable)
                     .value_or(kDefaultNormalizerScratchRoot)},
            client::ObjectTransfer(transfer_connect_timeout,
                                   transfer_request_timeout));
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    logger.info("normalizer.started", "Mesh normalizer started",
                {{"workerId", capabilities.worker_id},
                 {"normalizationVersion",
                  normalization::kMeshNormalizationVersion},
                 {"resourceProfileVersion", profile.profile_id},
                 {"decoderCapabilities", capabilities.decoder_capabilities}});
    while (!stop_requested.load(std::memory_order_relaxed)) {
        bool claimed = false;
        try {
            claimed = runtime.runOnce(
                    utcNowSeconds(),
                    [&executor](
                            normalization::NormalizationLeaseSession& session) {
                try {
                    executor.execute(session);
                } catch (const formats::FormatError& error) {
                    failNormalizerSession(
                            session,
                            normalization::meshNormalizationFailure(error));
                } catch (const client::ObjectTransferCancelledError&) {
                    // Cancellation or lease loss intentionally stops all I/O.
                } catch (const std::invalid_argument& error) {
                    failNormalizerSession(
                            session,
                            {"MANIFEST_INVALID",
                             std::string(error.what()).substr(0U, 512U),
                             false, false});
                } catch (const std::exception&) {
                    failNormalizerSession(
                            session,
                            {"INTERNAL_FAILURE",
                             "Mesh normalizer encountered a bounded internal failure",
                             true, false});
                }
            });
        } catch (const std::exception& error) {
            logger.warning("normalizer.claim_failed", error.what(),
                           {{"workerId", capabilities.worker_id}});
        }
        if (!claimed) {
            for (long elapsed = 0;
                 elapsed < poll_interval
                 && !stop_requested.load(std::memory_order_relaxed);
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    logger.info("normalizer.stopped", "Mesh normalizer stopped",
                {{"workerId", capabilities.worker_id}});
    return 0;
}

int runVersionedNormalizer(bool metadata_mode) {
    using namespace clip_worker;
    namespace broad = normalization::v2;
    const bool mesh_enabled = metadata_mode && enabledEnvironmentValue(
            kMetadataMeshEnabledVariable);
    const bool point_enabled = enabledEnvironmentValue(metadata_mode
            ? kMetadataPointEnabledVariable : kBroadPointEnabledVariable);
    const bool instance_enabled = enabledEnvironmentValue(metadata_mode
            ? kMetadataInstanceEnabledVariable : kBroadInstanceEnabledVariable);
    const bool composite_enabled = enabledEnvironmentValue(metadata_mode
            ? kMetadataCompositeEnabledVariable : kBroadCompositeEnabledVariable);
    if (!mesh_enabled && !point_enabled && !instance_enabled
            && !composite_enabled) {
        throw std::invalid_argument(
                "All versioned normalizer family flags are disabled");
    }
    const logging::Logger logger(
            logging::parseLogLevel(
                    environmentValue(kLogLevelVariable)
                            .value_or(kDefaultLogLevel)));
    const auto mesh_profile = normalization::MeshResourceProfile::load(
            environmentValue(kNormalizerProfilePathVariable)
                    .value_or(kDefaultNormalizerProfilePath),
            normalization::kMeshResourceProfileVersion,
            requiredEnvironmentValue(kNormalizerProfileSha256Variable));
    const auto broad_profile = normalization::BroadResourceProfile::load(
            environmentValue(metadata_mode ? kMetadataProfilePathVariable
                                           : kBroadProfilePathVariable)
                    .value_or(metadata_mode ? kDefaultMetadataProfilePath
                                            : kDefaultBroadProfilePath),
            metadata_mode ? normalization::kMetadataResourceProfileVersion
                          : normalization::kBroadResourceProfileVersion,
            requiredEnvironmentValue(metadata_mode
                    ? kMetadataProfileSha256Variable
                    : kBroadProfileSha256Variable));
    normalization::ToolVersion point_validator;
    normalization::ToolVersion instance_validator;
    normalization::ToolVersion mesh_validator;
    normalization::ToolVersion composite_validator;
    if (point_enabled || composite_enabled) {
        point_validator = {
                requiredEnvironmentValue(kPointValidatorNameVariable),
                requiredEnvironmentValue(kPointValidatorVersionVariable),
                requiredEnvironmentValue(kPointValidatorBuildSha256Variable)};
    }
    if (instance_enabled || composite_enabled) {
        instance_validator = {
                requiredEnvironmentValue(kInstanceValidatorNameVariable),
                requiredEnvironmentValue(kInstanceValidatorVersionVariable),
                requiredEnvironmentValue(kInstanceValidatorBuildSha256Variable)};
    }
    if (mesh_enabled || composite_enabled) {
        mesh_validator = {
                requiredEnvironmentValue(kNormalizerValidatorNameVariable),
                requiredEnvironmentValue(kNormalizerValidatorVersionVariable),
                requiredEnvironmentValue(kNormalizerValidatorBuildSha256Variable)};
        composite_validator = {
                requiredEnvironmentValue(kCompositeValidatorNameVariable),
                requiredEnvironmentValue(kCompositeValidatorVersionVariable),
                requiredEnvironmentValue(kCompositeValidatorBuildSha256Variable)};
    }
    const normalization::ToolVersion fallback = !mesh_validator.name.empty()
            ? mesh_validator
            : !point_validator.name.empty() ? point_validator
                                            : instance_validator;
    if (mesh_validator.name.empty()) mesh_validator = fallback;
    if (point_validator.name.empty()) point_validator = fallback;
    if (instance_validator.name.empty()) instance_validator = fallback;
    if (composite_validator.name.empty()) composite_validator = fallback;

    client::NormalizationApiClientConfig api_config;
    api_config.base_url = requiredEnvironmentValue(
            kNormalizerControlPlaneUrlVariable);
    api_config.authorization_header = environmentValue(
            kNormalizerAuthorizationHeaderVariable).value_or("");
    api_config.connect_timeout_seconds = positiveEnvironmentValue<long>(
            kNormalizerApiConnectTimeoutVariable,
            api_config.connect_timeout_seconds);
    api_config.request_timeout_seconds = positiveEnvironmentValue<long>(
            kNormalizerApiRequestTimeoutVariable,
            api_config.request_timeout_seconds);
    const long transfer_connect_timeout = positiveEnvironmentValue<long>(
            kNormalizerTransferConnectTimeoutVariable,
            client::ObjectTransfer::kDefaultConnectTimeoutSeconds);
    const long transfer_request_timeout = positiveEnvironmentValue<long>(
            kNormalizerTransferRequestTimeoutVariable,
            client::ObjectTransfer::kDefaultRequestTimeoutSeconds);
    const long poll_interval = positiveEnvironmentValue<long>(
            kNormalizerPollIntervalVariable,
            kDefaultNormalizerPollIntervalSeconds);

    broad::ClaimRequest capabilities;
    capabilities.worker_id = metadata_mode ? metadataNormalizerWorkerId()
                                           : broadNormalizerWorkerId();
    capabilities.decoder_capabilities = decoderCapabilities();
    capabilities.resource_profile_sha256 = broad_profile.document_sha256;
    if (metadata_mode) {
        normalization::v3::configureClaim(capabilities);
    }
    const auto add_family_version = [&capabilities, &mesh_profile](
            broad::CanonicalFamily family,
            const normalization::ToolVersion& validator,
            std::string normalization_version,
            std::string canonical_contract_version) {
        capabilities.family_capabilities.push_back({
                family, std::move(normalization_version),
                std::move(canonical_contract_version), validator.name,
                validator.version, validator.build_sha256,
                mesh_profile.maximum_input_bytes,
                mesh_profile.maximum_output_bytes});
        capabilities.tool_versions.push_back(validator);
    };
    const auto add_family = [&add_family_version, metadata_mode](
            broad::CanonicalFamily family,
            const normalization::ToolVersion& validator) {
        add_family_version(family, validator,
                           broad::normalizationVersion(family),
                           broad::canonicalContractVersion(family));
        if (metadata_mode) {
            add_family_version(family, validator,
                               normalization::v3::normalizationVersion(family),
                               normalization::v3::canonicalContractVersion(family));
        }
    };
    if (mesh_enabled || composite_enabled) {
        add_family(broad::CanonicalFamily::mesh_gltf2, mesh_validator);
    }
    if (point_enabled || composite_enabled) {
        add_family(broad::CanonicalFamily::point_gltf2, point_validator);
    }
    if (instance_enabled || composite_enabled) {
        add_family(broad::CanonicalFamily::instance_gltf2, instance_validator);
    }
    if (composite_enabled) {
        add_family(broad::CanonicalFamily::composite_children,
                   composite_validator);
    }
    std::sort(capabilities.tool_versions.begin(),
              capabilities.tool_versions.end(),
              [](const normalization::ToolVersion& left,
                 const normalization::ToolVersion& right) {
                  return std::tie(left.name, left.version, left.build_sha256)
                          < std::tie(right.name, right.version,
                                     right.build_sha256);
              });
    capabilities.tool_versions.erase(
            std::unique(capabilities.tool_versions.begin(),
                        capabilities.tool_versions.end(),
                        [](const normalization::ToolVersion& left,
                           const normalization::ToolVersion& right) {
                            return left.name == right.name
                                    && left.version == right.version
                                    && left.build_sha256
                                            == right.build_sha256;
                        }),
            capabilities.tool_versions.end());

    broad::NormalizationV2TaskRuntime runtime(
            capabilities,
            metadata_mode
                    ? client::NormalizationV2ApiClient(
                            std::move(api_config),
                            normalization::v3::kTaskBasePath)
                    : client::NormalizationV2ApiClient(
                            std::move(api_config)));
    broad::BroadNormalizationExecutor executor(
            {broad_profile, mesh_profile, mesh_validator, point_validator,
             instance_validator, composite_validator,
             environmentValue(kNormalizerScratchRootVariable)
                     .value_or(kDefaultNormalizerScratchRoot)},
            client::ObjectTransfer(transfer_connect_timeout,
                                   transfer_request_timeout));
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    logger.info(metadata_mode ? "metadata_normalizer.started"
                              : "broad_normalizer.started",
                metadata_mode ? "Metadata normalizer started"
                              : "Broad normalizer started",
                {{"workerId", capabilities.worker_id},
                 {"resourceProfileVersion", broad_profile.profile_id},
                 {"meshEnabled", mesh_enabled},
                 {"pointEnabled", point_enabled},
                 {"instanceEnabled", instance_enabled},
                 {"compositeEnabled", composite_enabled},
                 {"decoderCapabilities", capabilities.decoder_capabilities}});
    while (!stop_requested.load(std::memory_order_relaxed)) {
        bool claimed = false;
        try {
            claimed = runtime.runOnce(
                    utcNowSeconds(),
                    [&executor](broad::NormalizationV2LeaseSession& session) {
                try {
                    executor.execute(session);
                } catch (const formats::FormatError& error) {
                    failBroadNormalizerSession(
                            session, broad::broadNormalizationFailure(error));
                } catch (const client::ObjectTransferCancelledError&) {
                    // Cancellation or lease loss intentionally stops all I/O.
                } catch (const std::invalid_argument&) {
                    failBroadNormalizerSession(
                            session,
                            {"MANIFEST_INVALID",
                             "Broad normalizer rejected the resource manifest",
                             false, false});
                } catch (const std::exception&) {
                    failBroadNormalizerSession(
                            session,
                            {"INTERNAL_FAILURE",
                             "Broad normalizer encountered a bounded internal failure",
                             true, false});
                }
            });
        } catch (const std::exception& error) {
            logger.warning(metadata_mode ? "metadata_normalizer.claim_failed"
                                         : "broad_normalizer.claim_failed",
                           error.what(),
                           {{"workerId", capabilities.worker_id}});
        }
        if (!claimed) {
            for (long elapsed = 0;
                 elapsed < poll_interval
                 && !stop_requested.load(std::memory_order_relaxed);
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    logger.info(metadata_mode ? "metadata_normalizer.stopped"
                              : "broad_normalizer.stopped",
                metadata_mode ? "Metadata normalizer stopped"
                              : "Broad normalizer stopped",
                {{"workerId", capabilities.worker_id}});
    return 0;
}

int runBroadNormalizer() {
    return runVersionedNormalizer(false);
}

int runMetadataNormalizer() {
    return runVersionedNormalizer(true);
}

void printUsage(const char* executable) {
    std::cerr << "Usage:\n"
              << "  " << executable << " --version\n"
              << "  " << executable << " inspect <tile.b3dm>\n"
              << "  " << executable << " run\n"
              << "  " << executable << " run-normalizer\n"
              << "  " << executable << " run-broad-normalizer\n"
              << "  " << executable << " run-metadata-normalizer\n"
              << "  " << executable << " run-authorized-clipper\n";
}

std::string commandName(int argc, char* argv[]) {
    if (argc < 2) {
        return "unknown";
    }
    return argv[1];
}

const char* failureEvent(int argc, char* argv[]) {
    return argc >= 2 && (std::string(argv[1]) == "run"
                         || std::string(argv[1]) == "run-normalizer"
                         || std::string(argv[1]) == "run-broad-normalizer"
                         || std::string(argv[1]) == "run-metadata-normalizer"
                         || std::string(argv[1]) == "run-authorized-clipper")
            ? kEventApplicationStartFailed : kEventApplicationCommandFailed;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << kVersion << '\n';
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "inspect") {
            return inspect(argv[2]);
        }
        if (argc == 2 && std::string(argv[1]) == "run") {
            return runWorker();
        }
        if (argc == 2 && std::string(argv[1]) == "run-normalizer") {
            return runNormalizer();
        }
        if (argc == 2 && std::string(argv[1]) == "run-broad-normalizer") {
            return runBroadNormalizer();
        }
        if (argc == 2 && std::string(argv[1]) == "run-metadata-normalizer") {
            return runMetadataNormalizer();
        }
        if (argc == 2 && std::string(argv[1]) == "run-authorized-clipper") {
            return runAuthorizedClipper();
        }
        printUsage(argv[0]);
        return 2;
    } catch (const clip_worker::formats::FormatError& error) {
        clip_worker::logging::Logger().error(
                failureEvent(argc, argv), error.what(),
                {{"command", commandName(argc, argv)},
                 {"errorType", "FORMAT"},
                 {"errorMessage", error.what()}});
        return 3;
    } catch (const std::exception& error) {
        clip_worker::logging::Logger().error(
                failureEvent(argc, argv), error.what(),
                {{"command", commandName(argc, argv)},
                 {"errorType", "APPLICATION"},
                 {"errorMessage", error.what()}});
        return 1;
    } catch (...) {
        constexpr const char* kUnknownFailure = "Unknown non-standard exception";
        clip_worker::logging::Logger().error(
                failureEvent(argc, argv), kUnknownFailure,
                {{"command", commandName(argc, argv)},
                 {"errorType", "APPLICATION"},
                 {"errorMessage", kUnknownFailure}});
        return 1;
    }
}
