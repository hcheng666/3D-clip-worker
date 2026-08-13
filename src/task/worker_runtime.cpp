#include "clip_worker/task/worker_runtime.hpp"

#include "clip_worker/clip/b3dm_clipper.hpp"
#include "clip_worker/formats/format_error.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace clip_worker::task {
namespace {

constexpr const char* kEventWorkerStarted = "worker.started";
constexpr const char* kEventWorkerStopRequested = "worker.stop_requested";
constexpr const char* kEventClaimEmpty = "claim.empty";
constexpr const char* kEventClaimFailed = "claim.failed";
constexpr const char* kEventClaimInvalidResponse = "claim.invalid_response";
constexpr const char* kEventTaskClaimed = "task.claimed";
constexpr const char* kEventHeartbeatSucceeded = "heartbeat.succeeded";
constexpr const char* kEventHeartbeatFailed = "heartbeat.failed";
constexpr const char* kEventDownloadStarted = "download.started";
constexpr const char* kEventDownloadCompleted = "download.completed";
constexpr const char* kEventClipStarted = "clip.started";
constexpr const char* kEventClipCompleted = "clip.completed";
constexpr const char* kEventUploadStarted = "upload.started";
constexpr const char* kEventUploadCompleted = "upload.completed";
constexpr const char* kEventTaskSucceeded = "task.succeeded";
constexpr const char* kEventTaskFailed = "task.failed";
constexpr const char* kEventSourceCompatibilityWarning =
        "source.compatibility_warning";

constexpr const char* kInvalidClaimResponseCode = "INVALID_CLAIM_RESPONSE";
constexpr const char* kTransferFailedCode = "TRANSFER_FAILED";
constexpr const char* kControlPlaneFailedCode = "CONTROL_PLANE_FAILED";
constexpr const char* kProcessingFailedCode = "PROCESSING_FAILED";
constexpr const char* kUnknownFailureMessage = "Unknown non-standard exception";
constexpr const char* kB3dmNonconformantAlignmentCode =
        "B3DM_NONCONFORMANT_ALIGNMENT";
constexpr const char* kGltfNonstandardSamplerWrapRCode =
        "GLTF_NONSTANDARD_SAMPLER_WRAP_R";

using SteadyClock = std::chrono::steady_clock;

enum class TaskStage {
    heartbeat,
    download,
    source_validation,
    clip,
    output_validation,
    upload,
    complete_report
};

enum class TaskErrorType {
    format,
    transfer,
    control_plane,
    processing
};

const char* taskStageName(TaskStage stage) noexcept {
    switch (stage) {
        case TaskStage::heartbeat: return "HEARTBEAT";
        case TaskStage::download: return "DOWNLOAD";
        case TaskStage::source_validation: return "SOURCE_VALIDATION";
        case TaskStage::clip: return "CLIP";
        case TaskStage::output_validation: return "OUTPUT_VALIDATION";
        case TaskStage::upload: return "UPLOAD";
        case TaskStage::complete_report: return "COMPLETE_REPORT";
    }
    return "PROCESSING";
}

const char* taskErrorTypeName(TaskErrorType type) noexcept {
    switch (type) {
        case TaskErrorType::format: return "FORMAT";
        case TaskErrorType::transfer: return "TRANSFER";
        case TaskErrorType::control_plane: return "CONTROL_PLANE";
        case TaskErrorType::processing: return "PROCESSING";
    }
    return "PROCESSING";
}

std::uint64_t elapsedMilliseconds(SteadyClock::time_point started) {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    SteadyClock::now() - started).count());
}

logging::LogFields workerFields(const std::string& worker_id) {
    return {{"workerId", worker_id}};
}

logging::LogFields taskFields(const std::string& worker_id,
                              const std::string& asset_id) {
    return {{"workerId", worker_id}, {"assetId", asset_id}};
}

std::size_t effectiveAlignment(std::size_t value) {
    return value % formats::B3dmParser::kGlbAlignment == 0U
            ? formats::B3dmParser::kGlbAlignment
            : formats::B3dmParser::kCompatibleAlignment;
}

void addStatistics(logging::LogFields& fields, const CompleteStatistics& statistics) {
    fields["vertexCountBefore"] = statistics.vertex_count_before;
    fields["vertexCountAfter"] = statistics.vertex_count_after;
    fields["triangleCountBefore"] = statistics.triangle_count_before;
    fields["triangleCountAfter"] = statistics.triangle_count_after;
    fields["textureBytesBefore"] = statistics.texture_bytes_before;
    fields["textureBytesAfter"] = statistics.texture_bytes_after;
    fields["processingCostMs"] = statistics.cost_ms;
}

std::string formatErrorCode(formats::FormatErrorCode code) {
    switch (code) {
        case formats::FormatErrorCode::unsupported_version: return "UNSUPPORTED_VERSION";
        case formats::FormatErrorCode::unsupported_chunk: return "UNSUPPORTED_GLB_CHUNK";
        case formats::FormatErrorCode::unsupported_content: return "UNSUPPORTED_CONTENT";
        case formats::FormatErrorCode::invalid_accessor: return "UNSUPPORTED_ACCESSOR";
        default: return "UNSUPPORTED_INVALID_CONTENT";
    }
}

class LeaseHeartbeat final {
public:
    LeaseHeartbeat(const client::WorkerApiClient& api, std::string asset_id,
                   LeaseRequest request, long interval_seconds,
                   const logging::Logger& logger)
        : api_(api), asset_id_(std::move(asset_id)), request_(std::move(request)),
          interval_(interval_seconds), logger_(logger), thread_([this] { loop(); }) {
    }

    ~LeaseHeartbeat() {
        stop();
    }

    void stopAndRequireHealthy() {
        stop();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    void stop() noexcept {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void loop() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!condition_.wait_for(lock, interval_, [this] { return stopped_; })) {
            lock.unlock();
            try {
                api_.heartbeat(asset_id_, request_);
                logger_.debug(kEventHeartbeatSucceeded, "Worker lease heartbeat succeeded",
                              taskFields(request_.worker_id, asset_id_));
            } catch (const client::WorkerApiError& error) {
                failure_ = std::current_exception();
                auto fields = taskFields(request_.worker_id, asset_id_);
                if (error.statusCode() != 0) {
                    fields["httpStatus"] = error.statusCode();
                }
                fields["errorType"] = taskErrorTypeName(TaskErrorType::control_plane);
                fields["errorMessage"] = error.what();
                logger_.warning(kEventHeartbeatFailed, error.what(), fields);
                return;
            } catch (const std::exception& error) {
                failure_ = std::current_exception();
                auto fields = taskFields(request_.worker_id, asset_id_);
                fields["errorType"] = taskErrorTypeName(TaskErrorType::processing);
                fields["errorMessage"] = error.what();
                logger_.warning(kEventHeartbeatFailed, error.what(), fields);
                return;
            } catch (...) {
                failure_ = std::current_exception();
                auto fields = taskFields(request_.worker_id, asset_id_);
                fields["errorType"] = taskErrorTypeName(TaskErrorType::processing);
                fields["errorMessage"] = kUnknownFailureMessage;
                logger_.warning(kEventHeartbeatFailed, kUnknownFailureMessage, fields);
                return;
            }
            lock.lock();
        }
    }

    const client::WorkerApiClient& api_;
    std::string asset_id_;
    LeaseRequest request_;
    std::chrono::seconds interval_;
    const logging::Logger& logger_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopped_ = false;
    std::exception_ptr failure_;
};

void reportFailure(const WorkerRuntimeConfig& config,
                   const client::WorkerApiClient& api_client,
                   const logging::Logger& logger, const ClaimTask& task,
                   TaskStage stage, TaskErrorType error_type,
                   const std::string& code, const std::string& message,
                   bool retryable, std::uint64_t duration_ms,
                   long error_http_status = 0) noexcept {
    const std::string safe_message = message.empty()
            ? std::string(kUnknownFailureMessage) : message;
    bool failure_reported = false;
    long report_http_status = 0;
    std::string report_error_message;
    try {
        api_client.fail(task.asset_id,
                        {config.worker_id, task.lease_token,
                         code, safe_message, retryable});
        failure_reported = true;
    } catch (const client::WorkerApiError& error) {
        report_http_status = error.statusCode();
        report_error_message = error.what();
    } catch (const std::exception& error) {
        report_error_message = error.what();
    } catch (...) {
        report_error_message = kUnknownFailureMessage;
    }

    auto fields = taskFields(config.worker_id, task.asset_id);
    fields["stage"] = taskStageName(stage);
    fields["errorType"] = taskErrorTypeName(error_type);
    fields["errorCode"] = code;
    fields["errorMessage"] = safe_message;
    fields["retryable"] = retryable;
    fields["durationMs"] = duration_ms;
    fields["failureReported"] = failure_reported;
    if (error_http_status != 0) {
        fields["httpStatus"] = error_http_status;
    }
    if (!failure_reported) {
        if (report_http_status != 0) {
            fields["reportHttpStatus"] = report_http_status;
        }
        fields["reportErrorMessage"] = report_error_message;
    }
    logger.error(kEventTaskFailed, safe_message, fields);
}

}  // namespace

void logSourceCompatibilityWarning(
        const logging::Logger& logger, const std::string& worker_id,
        const std::string& asset_id,
        const formats::B3dmLayoutDiagnostics& diagnostics) noexcept {
    try {
        if (!diagnostics.requiresCompatibility()) {
            return;
        }
        auto fields = taskFields(worker_id, asset_id);
        fields["compatibilityCode"] = kB3dmNonconformantAlignmentCode;
        fields["glbOffset"] = diagnostics.glb_offset;
        fields["glbOffsetAlignment"] = effectiveAlignment(diagnostics.glb_offset);
        const std::size_t tile_length = diagnostics.glb_offset
                                        + diagnostics.glb_byte_length
                                        + diagnostics.trailing_padding_bytes;
        fields["tileLength"] = tile_length;
        fields["tileLengthAlignment"] = effectiveAlignment(tile_length);
        fields["trailingPaddingBytes"] = diagnostics.trailing_padding_bytes;
        logger.warning(kEventSourceCompatibilityWarning,
                       "Accepted a nonconforming but structurally valid B3DM alignment",
                       fields);
    } catch (...) {
        // Diagnostic logging must never change the task outcome.
    }
}

void logSamplerCompatibilityWarning(
        const logging::Logger& logger, const std::string& worker_id,
        const std::string& asset_id,
        const clip::SamplerCompatibilityDiagnostics& diagnostics) noexcept {
    try {
        if (!diagnostics.requiresCompatibility()) {
            return;
        }
        auto fields = taskFields(worker_id, asset_id);
        fields["compatibilityCode"] = kGltfNonstandardSamplerWrapRCode;
        fields["affectedSamplerCount"] = diagnostics.affected_sampler_count;
        fields["wrapRValues"] = diagnostics.wrap_r_values;
        logger.warning(kEventSourceCompatibilityWarning,
                       "Accepted and normalized non-standard glTF sampler wrapR",
                       fields);
    } catch (...) {
        // Diagnostic logging must never change the task outcome.
    }
}

WorkerRuntime::WorkerRuntime(WorkerRuntimeConfig config,
                             client::WorkerApiClient api_client,
                             client::ObjectTransfer object_transfer,
                             logging::Logger logger)
    : config_(std::move(config)), api_client_(std::move(api_client)),
      object_transfer_(std::move(object_transfer)), logger_(logger) {
    if (config_.worker_id.empty() || config_.algorithm_version.empty()
        || config_.maximum_input_bytes == 0U || config_.maximum_output_bytes == 0U
        || config_.poll_interval_seconds <= 0
        || config_.heartbeat_interval_seconds <= 0) {
        throw std::invalid_argument("Worker runtime configuration is incomplete");
    }
}

void WorkerRuntime::run(const std::atomic_bool& stop_requested) const {
    auto fields = workerFields(config_.worker_id);
    fields["algorithmVersion"] = config_.algorithm_version;
    fields["maximumInputBytes"] = config_.maximum_input_bytes;
    fields["maximumOutputBytes"] = config_.maximum_output_bytes;
    fields["pollIntervalSeconds"] = config_.poll_interval_seconds;
    fields["heartbeatIntervalSeconds"] = config_.heartbeat_interval_seconds;
    fields["logLevel"] = logging::logLevelName(logger_.minimumLevel());
    logger_.info(kEventWorkerStarted, "Clip worker started", fields);

    while (!stop_requested.load()) {
        bool claimed = false;
        try {
            claimed = runOnce();
        } catch (const client::WorkerApiError& error) {
            auto error_fields = workerFields(config_.worker_id);
            if (error.statusCode() != 0) {
                error_fields["httpStatus"] = error.statusCode();
            }
            error_fields["errorMessage"] = error.what();
            logger_.warning(kEventClaimFailed, error.what(), error_fields);
        } catch (const std::invalid_argument&) {
            auto error_fields = workerFields(config_.worker_id);
            error_fields["errorCode"] = kInvalidClaimResponseCode;
            logger_.error(kEventClaimInvalidResponse,
                          "Worker received an invalid claim task response", error_fields);
        }
        if (!claimed) {
            for (long elapsed = 0; elapsed < config_.poll_interval_seconds
                                   && !stop_requested.load(); ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    logger_.info(kEventWorkerStopRequested, "Clip worker stopped after stop request",
                 workerFields(config_.worker_id));
}

bool WorkerRuntime::runOnce() const {
    ClaimRequest request;
    request.worker_id = config_.worker_id;
    request.supported_formats = {ContentFormat::b3dm_gltf2};
    request.algorithm_version = config_.algorithm_version;
    request.max_input_bytes = config_.maximum_input_bytes;
    const auto claimed = api_client_.claim(request);
    if (!claimed.has_value()) {
        logger_.debug(kEventClaimEmpty, "No clip task is currently available",
                      workerFields(config_.worker_id));
        return false;
    }

    auto fields = taskFields(config_.worker_id, claimed->asset_id);
    fields["contentFormat"] = contentFormatName(claimed->content_format);
    fields["leaseExpireTime"] = claimed->lease_expire_time;
    logger_.info(kEventTaskClaimed, "Clip task claimed", fields);
    process(*claimed);
    return true;
}

void WorkerRuntime::process(const ClaimTask& task) const {
    const auto started = SteadyClock::now();
    TaskStage stage = TaskStage::heartbeat;
    try {
        LeaseRequest lease{config_.worker_id, task.lease_token};
        LeaseHeartbeat heartbeat(api_client_, task.asset_id, lease,
                                 config_.heartbeat_interval_seconds, logger_);

        stage = TaskStage::download;
        auto phase_started = SteadyClock::now();
        logger_.debug(kEventDownloadStarted, "Source object download started",
                      taskFields(config_.worker_id, task.asset_id));
        const auto source = object_transfer_.download(
                task.source_download_url, config_.maximum_input_bytes);
        auto fields = taskFields(config_.worker_id, task.asset_id);
        fields["durationMs"] = elapsedMilliseconds(phase_started);
        fields["inputBytes"] = source.bytes.size();
        logger_.info(kEventDownloadCompleted, "Source object download completed", fields);

        stage = TaskStage::source_validation;
        if (client::normalizeEtag(source.etag)
            != client::normalizeEtag(task.source_etag)) {
            throw client::ObjectTransferError("Downloaded source ETag does not match task");
        }

        stage = TaskStage::clip;
        phase_started = SteadyClock::now();
        logger_.debug(kEventClipStarted, "3D tile clipping started",
                      taskFields(config_.worker_id, task.asset_id));
        auto clipped = clip::B3dmClipper::clip(
                source.bytes, task,
                [this, &task](const formats::B3dmLayoutDiagnostics& diagnostics) {
                    logSourceCompatibilityWarning(
                            logger_, config_.worker_id, task.asset_id, diagnostics);
                },
                [this, &task](
                        const clip::SamplerCompatibilityDiagnostics& diagnostics) {
                    logSamplerCompatibilityWarning(
                            logger_, config_.worker_id, task.asset_id, diagnostics);
                });
        clipped.statistics.cost_ms = elapsedMilliseconds(started);
        CompleteRequest complete;
        complete.worker_id = config_.worker_id;
        complete.lease_token = task.lease_token;
        complete.statistics = clipped.statistics;
        complete.result = clipped.empty ? CompletionResult::empty : CompletionResult::ready;

        fields = taskFields(config_.worker_id, task.asset_id);
        fields["durationMs"] = elapsedMilliseconds(phase_started);
        fields["result"] = completionResultName(complete.result);
        fields["outputBytes"] = clipped.bytes.size();
        addStatistics(fields, clipped.statistics);
        logger_.info(kEventClipCompleted, "3D tile clipping completed", fields);

        if (!clipped.empty) {
            stage = TaskStage::output_validation;
            if (clipped.bytes.size() > config_.maximum_output_bytes) {
                throw std::length_error("Clipped output exceeds the configured size limit");
            }
            complete.output_sha256 = client::sha256Hex(clipped.bytes);
            complete.output_size = clipped.bytes.size();

            stage = TaskStage::upload;
            phase_started = SteadyClock::now();
            logger_.debug(kEventUploadStarted, "Clipped object upload started",
                          taskFields(config_.worker_id, task.asset_id));
            complete.output_etag = object_transfer_.upload(
                    task.output_upload_url, clipped.bytes);
            fields = taskFields(config_.worker_id, task.asset_id);
            fields["durationMs"] = elapsedMilliseconds(phase_started);
            fields["outputBytes"] = complete.output_size;
            logger_.info(kEventUploadCompleted, "Clipped object upload completed", fields);
        }

        stage = TaskStage::heartbeat;
        heartbeat.stopAndRequireHealthy();
        stage = TaskStage::complete_report;
        api_client_.complete(task.asset_id, complete);

        fields = taskFields(config_.worker_id, task.asset_id);
        fields["result"] = completionResultName(complete.result);
        fields["durationMs"] = elapsedMilliseconds(started);
        fields["inputBytes"] = source.bytes.size();
        fields["outputBytes"] = complete.output_size;
        addStatistics(fields, complete.statistics);
        logger_.info(kEventTaskSucceeded, "Clip task completed", fields);
    } catch (const formats::FormatError& error) {
        reportFailure(config_, api_client_, logger_, task, stage, TaskErrorType::format,
                      formatErrorCode(error.code()), error.what(), false,
                      elapsedMilliseconds(started));
    } catch (const client::ObjectTransferError& error) {
        reportFailure(config_, api_client_, logger_, task, stage, TaskErrorType::transfer,
                      kTransferFailedCode, error.what(), true,
                      elapsedMilliseconds(started));
    } catch (const client::WorkerApiError& error) {
        reportFailure(config_, api_client_, logger_, task, stage,
                      TaskErrorType::control_plane, kControlPlaneFailedCode,
                      error.what(), true, elapsedMilliseconds(started),
                      error.statusCode());
    } catch (const std::exception& error) {
        reportFailure(config_, api_client_, logger_, task, stage, TaskErrorType::processing,
                      kProcessingFailedCode, error.what(), false,
                      elapsedMilliseconds(started));
    } catch (...) {
        reportFailure(config_, api_client_, logger_, task, stage, TaskErrorType::processing,
                      kProcessingFailedCode, kUnknownFailureMessage, false,
                      elapsedMilliseconds(started));
    }
}

}  // namespace clip_worker::task
