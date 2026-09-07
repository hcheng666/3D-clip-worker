#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace clip_worker::client {

struct DownloadedObject {
    std::vector<std::uint8_t> bytes;
    std::string etag;
};

struct StreamedObjectMetadata {
    std::uint64_t observed_size = 0U;
    std::string etag;
    std::string sha256;
    bool has_zip_magic = false;
};

struct StreamedUploadMetadata {
    std::uint64_t observed_size = 0U;
    std::string etag;
    std::string sha256;
};

class ObjectTransferError final : public std::runtime_error {
public:
    explicit ObjectTransferError(std::string message);
};

class ObjectTransferCancelledError final : public std::runtime_error {
public:
    explicit ObjectTransferCancelledError(std::string message);
};

using TransferContinuePredicate = std::function<bool()>;
using UploadStreamReader =
        std::function<std::size_t(std::uint8_t* buffer, std::size_t capacity)>;

/** 只访问控制面下发的短期预签名URL，不持有MinIO长期凭据。 */
class ObjectTransfer final {
public:
    static constexpr long kDefaultConnectTimeoutSeconds = 10;
    static constexpr long kDefaultRequestTimeoutSeconds = 300;

    ObjectTransfer(long connect_timeout_seconds = kDefaultConnectTimeoutSeconds,
                   long request_timeout_seconds = kDefaultRequestTimeoutSeconds);

    [[nodiscard]] DownloadedObject download(const std::string& presigned_url,
                                             std::uint64_t maximum_bytes) const;
    [[nodiscard]] StreamedObjectMetadata inspect(
            const std::string& presigned_url, std::uint64_t maximum_bytes,
            const TransferContinuePredicate& should_continue = {}) const;
    [[nodiscard]] StreamedObjectMetadata downloadToFile(
            const std::string& presigned_url, const std::filesystem::path& destination,
            std::uint64_t maximum_bytes,
            const TransferContinuePredicate& should_continue = {}) const;
    [[nodiscard]] std::string upload(const std::string& presigned_url,
                                     const std::vector<std::uint8_t>& bytes) const;
    [[nodiscard]] StreamedUploadMetadata uploadStream(
            const std::string& presigned_url, std::uint64_t declared_size,
            const std::string& expected_sha256, const UploadStreamReader& reader,
            const TransferContinuePredicate& should_continue = {}) const;

private:
    long connect_timeout_seconds_;
    long request_timeout_seconds_;
};

[[nodiscard]] std::string sha256Hex(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] std::string normalizeEtag(const std::string& value);

}  // namespace clip_worker::client
