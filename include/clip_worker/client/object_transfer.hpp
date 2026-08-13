#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace clip_worker::client {

struct DownloadedObject {
    std::vector<std::uint8_t> bytes;
    std::string etag;
};

class ObjectTransferError final : public std::runtime_error {
public:
    explicit ObjectTransferError(std::string message);
};

/** 只访问控制面下发的短期预签名URL，不持有MinIO长期凭据。 */
class ObjectTransfer final {
public:
    static constexpr long kDefaultConnectTimeoutSeconds = 10;
    static constexpr long kDefaultRequestTimeoutSeconds = 300;

    ObjectTransfer(long connect_timeout_seconds = kDefaultConnectTimeoutSeconds,
                   long request_timeout_seconds = kDefaultRequestTimeoutSeconds);

    [[nodiscard]] DownloadedObject download(const std::string& presigned_url,
                                             std::uint64_t maximum_bytes) const;
    [[nodiscard]] std::string upload(const std::string& presigned_url,
                                     const std::vector<std::uint8_t>& bytes) const;

private:
    long connect_timeout_seconds_;
    long request_timeout_seconds_;
};

[[nodiscard]] std::string sha256Hex(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] std::string normalizeEtag(const std::string& value);

}  // namespace clip_worker::client
