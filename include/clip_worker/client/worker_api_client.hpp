#pragma once

#include "clip_worker/task/task_contract.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace clip_worker::client {

struct WorkerApiClientConfig {
    static constexpr long kDefaultConnectTimeoutSeconds = 10;
    static constexpr long kDefaultRequestTimeoutSeconds = 30;

    std::string base_url;
    std::string authorization_header;
    long connect_timeout_seconds = kDefaultConnectTimeoutSeconds;
    long request_timeout_seconds = kDefaultRequestTimeoutSeconds;
};

class WorkerApiError final : public std::runtime_error {
public:
    WorkerApiError(long status_code, std::string message);

    [[nodiscard]] long statusCode() const noexcept;

private:
    long status_code_;
};

class WorkerApiClient final {
public:
    explicit WorkerApiClient(WorkerApiClientConfig config);

    [[nodiscard]] std::optional<task::ClaimTask> claim(const task::ClaimRequest& request) const;
    void heartbeat(const std::string& asset_id, const task::LeaseRequest& request) const;
    void complete(const std::string& asset_id, const task::CompleteRequest& request) const;
    void fail(const std::string& asset_id, const task::FailRequest& request) const;

private:
    struct Response {
        long status_code = 0;
        std::string body;
    };

    [[nodiscard]] Response postJson(const std::string& path, const std::string& body) const;
    [[nodiscard]] std::string taskPath(const std::string& asset_id,
                                       const std::string& operation) const;
    static void requireSuccess(const Response& response, const char* operation);

    WorkerApiClientConfig config_;
};

}  // namespace clip_worker::client

