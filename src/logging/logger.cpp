#include "clip_worker/logging/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace clip_worker::logging {
namespace {

constexpr const char* kLevelDebug = "DEBUG";
constexpr const char* kLevelInfo = "INFO";
constexpr const char* kLevelWarning = "WARN";
constexpr const char* kLevelError = "ERROR";
constexpr const char* kTimestampField = "timestamp";
constexpr const char* kLevelField = "level";
constexpr const char* kEventField = "event";
constexpr const char* kMessageField = "message";
constexpr const char* kFallbackLog =
        "{\"level\":\"ERROR\",\"event\":\"logging.failed\","
        "\"message\":\"Unable to serialize log event\"}\n";

std::mutex& outputMutex() {
    static std::mutex mutex;
    return mutex;
}

bool isReservedField(const std::string& name) {
    return name == kTimestampField || name == kLevelField
            || name == kEventField || name == kMessageField;
}

std::string utcTimestamp() {
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now();
    const auto whole_seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millisecond_part = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - whole_seconds);
    const std::time_t time = Clock::to_time_t(whole_seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
              << std::setfill('0') << std::setw(3) << millisecond_part.count() << 'Z';
    return timestamp.str();
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

}  // namespace

const char* logLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug: return kLevelDebug;
        case LogLevel::info: return kLevelInfo;
        case LogLevel::warning: return kLevelWarning;
        case LogLevel::error: return kLevelError;
    }
    return kLevelError;
}

LogLevel parseLogLevel(const std::string& value) {
    const std::string normalized = uppercase(value);
    if (normalized == kLevelDebug) {
        return LogLevel::debug;
    }
    if (normalized == kLevelInfo) {
        return LogLevel::info;
    }
    if (normalized == kLevelWarning) {
        return LogLevel::warning;
    }
    if (normalized == kLevelError) {
        return LogLevel::error;
    }
    throw std::invalid_argument(
            "CLIP_WORKER_LOG_LEVEL must be DEBUG, INFO, WARN, or ERROR");
}

Logger::Logger() : Logger(LogLevel::info, std::cerr) {
}

Logger::Logger(LogLevel minimum_level) : Logger(minimum_level, std::cerr) {
}

Logger::Logger(LogLevel minimum_level, std::ostream& output)
    : minimum_level_(minimum_level), output_(&output) {
}

LogLevel Logger::minimumLevel() const noexcept {
    return minimum_level_;
}

void Logger::log(LogLevel level, const std::string& event, const std::string& message,
                 const LogFields& fields) const noexcept {
    if (level < minimum_level_) {
        return;
    }
    try {
        LogFields record = LogFields::object();
        record[kTimestampField] = utcTimestamp();
        record[kLevelField] = logLevelName(level);
        record[kEventField] = event;
        record[kMessageField] = message;
        if (fields.is_object()) {
            for (auto field = fields.cbegin(); field != fields.cend(); ++field) {
                if (!isReservedField(field.key())) {
                    record[field.key()] = field.value();
                }
            }
        }
        const std::string encoded = record.dump(
                -1, ' ', false, nlohmann::json::error_handler_t::replace);
        const std::lock_guard<std::mutex> lock(outputMutex());
        *output_ << encoded << '\n';
        output_->flush();
    } catch (...) {
        // Logging must never interrupt task processing or error reporting.
        const std::lock_guard<std::mutex> lock(outputMutex());
        *output_ << kFallbackLog;
        output_->flush();
    }
}

void Logger::debug(const std::string& event, const std::string& message,
                   const LogFields& fields) const noexcept {
    log(LogLevel::debug, event, message, fields);
}

void Logger::info(const std::string& event, const std::string& message,
                  const LogFields& fields) const noexcept {
    log(LogLevel::info, event, message, fields);
}

void Logger::warning(const std::string& event, const std::string& message,
                     const LogFields& fields) const noexcept {
    log(LogLevel::warning, event, message, fields);
}

void Logger::error(const std::string& event, const std::string& message,
                   const LogFields& fields) const noexcept {
    log(LogLevel::error, event, message, fields);
}

}  // namespace clip_worker::logging
