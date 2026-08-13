#include "clip_worker/client/object_transfer.hpp"

#include "clip_worker/client/curl_runtime.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include <curl/curl.h>
#include <openssl/evp.h>

namespace clip_worker::client {
namespace {

constexpr long kHttpOk = 200;
constexpr const char* kEtagHeader = "etag:";
constexpr std::size_t kSha256Bytes = 32U;

struct DownloadContext {
    std::vector<std::uint8_t> bytes;
    std::uint64_t maximum_bytes = 0U;
    bool exceeded_limit = false;
    std::string etag;
};

struct UploadContext {
    const std::vector<std::uint8_t>* bytes = nullptr;
    std::size_t offset = 0U;
    std::string etag;
};

using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::size_t receiveBytes(char* data, std::size_t size, std::size_t count, void* context) {
    if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) {
        return 0U;
    }
    const std::size_t byte_count = size * count;
    auto* download = static_cast<DownloadContext*>(context);
    if (byte_count > download->maximum_bytes
        || download->bytes.size() > download->maximum_bytes - byte_count) {
        download->exceeded_limit = true;
        return 0U;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    download->bytes.insert(download->bytes.end(), bytes, bytes + byte_count);
    return byte_count;
}

std::size_t sendBytes(char* data, std::size_t size, std::size_t count, void* context) {
    auto* upload = static_cast<UploadContext*>(context);
    if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) {
        return CURL_READFUNC_ABORT;
    }
    const std::size_t capacity = size * count;
    const std::size_t remaining = upload->bytes->size() - upload->offset;
    const std::size_t byte_count = std::min(capacity, remaining);
    if (byte_count > 0U) {
        std::memcpy(data, upload->bytes->data() + upload->offset, byte_count);
        upload->offset += byte_count;
    }
    return byte_count;
}

std::size_t receiveHeader(char* data, std::size_t size, std::size_t count, void* context) {
    if (count != 0U && size > std::numeric_limits<std::size_t>::max() / count) {
        return 0U;
    }
    const std::size_t byte_count = size * count;
    std::string line(data, byte_count);
    std::string lowercase = line;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (lowercase.rfind(kEtagHeader, 0U) == 0U) {
        auto* etag = static_cast<std::string*>(context);
        *etag = trim(line.substr(std::strlen(kEtagHeader)));
    }
    return byte_count;
}

CurlHandle requestHandle(const std::string& url, long connect_timeout,
                         long request_timeout) {
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        throw ObjectTransferError("Failed to allocate object transfer request");
    }
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, connect_timeout);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, request_timeout);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    return curl;
}

void requireHttpOk(CURL* curl, CURLcode result, const char* operation) {
    if (result != CURLE_OK) {
        // Preserve the public CURL diagnosis without exposing the presigned request URL.
        throw ObjectTransferError(std::string(operation) + " transfer failed: "
                                  + curl_easy_strerror(result));
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != kHttpOk) {
        throw ObjectTransferError(std::string(operation) + " returned HTTP "
                                  + std::to_string(status));
    }
}

}  // namespace

ObjectTransferError::ObjectTransferError(std::string message)
    : std::runtime_error(std::move(message)) {
}

ObjectTransfer::ObjectTransfer(long connect_timeout_seconds,
                               long request_timeout_seconds)
    : connect_timeout_seconds_(connect_timeout_seconds),
      request_timeout_seconds_(request_timeout_seconds) {
    if (connect_timeout_seconds_ <= 0 || request_timeout_seconds_ <= 0) {
        throw std::invalid_argument("Object transfer timeouts must be greater than zero");
    }
    ensureCurlInitialized();
}

DownloadedObject ObjectTransfer::download(const std::string& presigned_url,
                                           std::uint64_t maximum_bytes) const {
    if (presigned_url.empty() || maximum_bytes == 0U
        || maximum_bytes > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("Object download parameters are invalid");
    }
    auto curl = requestHandle(presigned_url, connect_timeout_seconds_,
                              request_timeout_seconds_);
    DownloadContext context;
    context.maximum_bytes = maximum_bytes;
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &receiveBytes);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &receiveHeader);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &context.etag);
    const CURLcode result = curl_easy_perform(curl.get());
    if (context.exceeded_limit) {
        throw ObjectTransferError("Object download exceeded the configured size limit");
    }
    requireHttpOk(curl.get(), result, "Object download");
    if (context.bytes.empty() || normalizeEtag(context.etag).empty()) {
        throw ObjectTransferError("Object download is empty or missing ETag");
    }
    return {std::move(context.bytes), normalizeEtag(context.etag)};
}

std::string ObjectTransfer::upload(const std::string& presigned_url,
                                   const std::vector<std::uint8_t>& bytes) const {
    if (presigned_url.empty() || bytes.empty()) {
        throw std::invalid_argument("Object upload parameters are invalid");
    }
    auto curl = requestHandle(presigned_url, connect_timeout_seconds_,
                              request_timeout_seconds_);
    UploadContext context{&bytes, 0U, {}};
    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/octet-stream");
    HeaderList headers(raw_headers, &curl_slist_free_all);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, &sendBytes);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &context);
    curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE,
                     static_cast<curl_off_t>(bytes.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
                     +[](char*, std::size_t size, std::size_t count, void*) {
                         return size * count;
                     });
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &receiveHeader);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &context.etag);
    const CURLcode result = curl_easy_perform(curl.get());
    requireHttpOk(curl.get(), result, "Object upload");
    const std::string etag = normalizeEtag(context.etag);
    if (context.offset != bytes.size() || etag.empty()) {
        throw ObjectTransferError("Object upload did not consume all bytes or return ETag");
    }
    return etag;
}

std::string sha256Hex(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        throw std::invalid_argument("Cannot hash an empty object");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0U;
    EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
    if (raw_context == nullptr) {
        throw std::runtime_error("Failed to allocate SHA-256 context");
    }
    using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(raw_context, &EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1
        || EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1
        || EVP_DigestFinal_ex(context.get(), digest.data(), &digest_length) != 1
        || digest_length != kSha256Bytes) {
        throw std::runtime_error("SHA-256 calculation failed");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(digest_length * 2U, '0');
    for (std::size_t index = 0; index < digest_length; ++index) {
        result[index * 2U] = kHex[digest[index] >> 4U];
        result[index * 2U + 1U] = kHex[digest[index] & 0x0FU];
    }
    return result;
}

std::string normalizeEtag(const std::string& value) {
    std::string result = trim(value);
    if (result.size() >= 2U && (result[0] == 'W' || result[0] == 'w')
        && result[1] == '/') {
        result = trim(result.substr(2U));
    }
    if (result.size() >= 2U && result.front() == '"' && result.back() == '"') {
        result = result.substr(1U, result.size() - 2U);
    }
    return result;
}

}  // namespace clip_worker::client
