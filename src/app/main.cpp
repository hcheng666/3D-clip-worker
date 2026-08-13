#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/client/worker_api_client.hpp"
#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/formats/byte_view.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/logging/logger.hpp"
#include "clip_worker/task/worker_runtime.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr std::uintmax_t kMebibyte = 1024U * 1024U;
constexpr std::uintmax_t kMaxInspectInputBytes = 512U * kMebibyte;
constexpr const char* kVersion = "0.1.3";
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

void printUsage(const char* executable) {
    std::cerr << "Usage:\n"
              << "  " << executable << " --version\n"
              << "  " << executable << " inspect <tile.b3dm>\n"
              << "  " << executable << " run\n";
}

std::string commandName(int argc, char* argv[]) {
    if (argc < 2) {
        return "unknown";
    }
    return argv[1];
}

const char* failureEvent(int argc, char* argv[]) {
    return argc >= 2 && std::string(argv[1]) == "run"
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
