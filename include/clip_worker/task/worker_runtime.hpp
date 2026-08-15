#pragma once

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/client/worker_api_client.hpp"
#include "clip_worker/clip/b3dm_clipper.hpp"
#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/logging/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace clip_worker::task {

/** Emits one redacted warning when a source uses the bounded legacy B3DM layout. */
void logSourceCompatibilityWarning(
        const logging::Logger& logger, const std::string& worker_id,
        const std::string& asset_id,
        const formats::B3dmLayoutDiagnostics& diagnostics) noexcept;

/** Emits one redacted warning for accepted non-standard sampler wrapR fields. */
void logSamplerCompatibilityWarning(
        const logging::Logger& logger, const std::string& worker_id,
        const std::string& asset_id,
        const clip::SamplerCompatibilityDiagnostics& diagnostics) noexcept;

/** Emits one redacted warning for accepted stale Draco accessor counts. */
void logDracoCompatibilityWarning(
        const logging::Logger& logger, const std::string& worker_id,
        const std::string& asset_id,
        const clip::DracoCompatibilityDiagnostics& diagnostics) noexcept;

struct WorkerRuntimeConfig {
    static constexpr const char* kDefaultAlgorithmVersion = "v8";
    static constexpr std::uint64_t kDefaultMaximumInputBytes = 64U * 1024U * 1024U;
    static constexpr std::uint64_t kDefaultMaximumOutputBytes = 64U * 1024U * 1024U;
    static constexpr long kDefaultPollIntervalSeconds = 5;
    static constexpr long kDefaultHeartbeatIntervalSeconds = 30;

    std::string worker_id;
    std::string algorithm_version = kDefaultAlgorithmVersion;
    std::uint64_t maximum_input_bytes = kDefaultMaximumInputBytes;
    std::uint64_t maximum_output_bytes = kDefaultMaximumOutputBytes;
    long poll_interval_seconds = kDefaultPollIntervalSeconds;
    long heartbeat_interval_seconds = kDefaultHeartbeatIntervalSeconds;
};

class WorkerRuntime final {
public:
    WorkerRuntime(WorkerRuntimeConfig config,
                  client::WorkerApiClient api_client,
                  client::ObjectTransfer object_transfer = client::ObjectTransfer(),
                  logging::Logger logger = logging::Logger());

    void run(const std::atomic_bool& stop_requested) const;
    [[nodiscard]] bool runOnce() const;

private:
    void process(const ClaimTask& task) const;

    WorkerRuntimeConfig config_;
    client::WorkerApiClient api_client_;
    client::ObjectTransfer object_transfer_;
    logging::Logger logger_;
};

}  // namespace clip_worker::task
