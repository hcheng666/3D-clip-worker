#pragma once

#include <iosfwd>
#include <string>

#include <nlohmann/json.hpp>

namespace clip_worker::logging {

enum class LogLevel {
    debug = 0,
    info = 1,
    warning = 2,
    error = 3
};

using LogFields = nlohmann::ordered_json;

[[nodiscard]] const char* logLevelName(LogLevel level) noexcept;
[[nodiscard]] LogLevel parseLogLevel(const std::string& value);

class Logger final {
public:
    Logger();
    explicit Logger(LogLevel minimum_level);
    Logger(LogLevel minimum_level, std::ostream& output);

    [[nodiscard]] LogLevel minimumLevel() const noexcept;

    void log(LogLevel level, const std::string& event, const std::string& message,
             const LogFields& fields = LogFields::object()) const noexcept;
    void debug(const std::string& event, const std::string& message,
               const LogFields& fields = LogFields::object()) const noexcept;
    void info(const std::string& event, const std::string& message,
              const LogFields& fields = LogFields::object()) const noexcept;
    void warning(const std::string& event, const std::string& message,
                 const LogFields& fields = LogFields::object()) const noexcept;
    void error(const std::string& event, const std::string& message,
               const LogFields& fields = LogFields::object()) const noexcept;

private:
    LogLevel minimum_level_;
    std::ostream* output_;
};

}  // namespace clip_worker::logging
