#include "clip_worker/inspection/package/package_enumerator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <utf8proc.h>
#include <zip.h>

namespace clip_worker::inspection::package {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kDigestBytes = 32U;
constexpr std::size_t kMd5Bytes = 16U;
constexpr std::size_t kReadBufferBytes = 64U * 1024U;
constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50U;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50U;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054b50U;
constexpr std::uint32_t kZip64EndSignature = 0x06064b50U;
constexpr std::uint32_t kZip64LocatorSignature = 0x07064b50U;
constexpr std::uint16_t kZip64ExtraField = 0x0001U;
constexpr std::uint16_t kEncryptedFlag = 1U << 0U;
constexpr std::uint16_t kStrongEncryptionFlag = 1U << 6U;
constexpr std::uint16_t kUtf8NameFlag = 1U << 11U;
constexpr std::uint8_t kUnixCreatorSystem = 3U;
constexpr std::uint32_t kUnixTypeMask = 0170000U;
constexpr std::uint32_t kUnixSymlinkType = 0120000U;
constexpr std::uint32_t kUnixRegularType = 0100000U;
constexpr std::uint32_t kUnixDirectoryType = 0040000U;
constexpr std::uint64_t kThreeTzIndexRecordBytes = 24U;
constexpr const char* kEntryIdentityDomain = "three-d-tiles-package-entry-v1";
constexpr const char* kScratchIdentityDomain = "three-d-tiles-inspection-scratch-v1";
constexpr const char* kScratchDirectoryPrefix = "inspection-";

struct DigestContextDeleter {
    void operator()(EVP_MD_CTX* context) const noexcept {
        EVP_MD_CTX_free(context);
    }
};

using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;

struct ZipArchiveDeleter {
    void operator()(zip_t* archive) const noexcept {
        if (archive != nullptr) zip_discard(archive);
    }
};

using ZipArchive = std::unique_ptr<zip_t, ZipArchiveDeleter>;

struct ZipFileDeleter {
    void operator()(zip_file_t* file) const noexcept {
        if (file != nullptr) static_cast<void>(zip_fclose(file));
    }
};

using ZipFile = std::unique_ptr<zip_file_t, ZipFileDeleter>;

struct CentralEntry {
    std::uint64_t ordinal = 0U;
    std::string raw_name;
    std::uint16_t flags = 0U;
    std::uint16_t compression_method = 0U;
    std::uint64_t compressed_size = 0U;
    std::uint64_t expanded_size = 0U;
    std::uint64_t local_header_offset = 0U;
    std::uint8_t creator_system = 0U;
    std::uint32_t external_attributes = 0U;
};

struct ArchiveLayout {
    std::uint64_t central_offset = 0U;
    std::uint64_t central_size = 0U;
    std::vector<CentralEntry> entries;
};

struct StreamedEntry {
    std::uint64_t observed_size = 0U;
    std::string sha256;
    bool has_zip_magic = false;
    std::vector<std::uint8_t> retained_bytes;
};

struct ThreeTzIndexRecord {
    std::array<std::uint8_t, kMd5Bytes> digest{};
    std::uint64_t local_header_offset = 0U;
};

[[noreturn]] void invalid(const char* message) {
    throw PackageEnumerationError(PackageFailureKind::invalid, message);
}

[[noreturn]] void limitExceeded(const char* message) {
    throw PackageEnumerationError(PackageFailureKind::limit_exceeded, message);
}

void requireContinue(const ContinuePredicate& should_continue) {
    if (should_continue && !should_continue()) {
        throw PackageEnumerationError(PackageFailureKind::cancelled,
                                      "Package enumeration was cancelled");
    }
}

std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right,
                         const char* message) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        limitExceeded(message);
    }
    return left + right;
}

std::uint64_t checkedAddInvalid(std::uint64_t left, std::uint64_t right,
                                const char* message) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) invalid(message);
    return left + right;
}

std::uint64_t checkedMultiplyInvalid(std::uint64_t left,
                                     std::uint64_t right,
                                     const char* message) {
    if (left != 0U
        && right > std::numeric_limits<std::uint64_t>::max() / left) {
        invalid(message);
    }
    return left * right;
}

bool exceedsRatio(std::uint64_t expanded, std::uint64_t compressed,
                  std::uint64_t maximum_ratio) {
    if (expanded == 0U && compressed == 0U) return false;
    if (compressed == 0U) invalid("Archive entry has an impossible compressed size");
    const std::uint64_t quotient = expanded / compressed;
    const std::uint64_t remainder = expanded % compressed;
    return quotient > maximum_ratio
            || (quotient == maximum_ratio && remainder != 0U);
}

std::uint16_t readLe16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0])
            | static_cast<std::uint16_t>(bytes[1]) << 8U;
}

std::uint32_t readLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
            | static_cast<std::uint32_t>(bytes[1]) << 8U
            | static_cast<std::uint32_t>(bytes[2]) << 16U
            | static_cast<std::uint32_t>(bytes[3]) << 24U;
}

std::uint64_t readLe64(const std::uint8_t* bytes) {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

std::vector<std::uint8_t> readAt(std::ifstream& input, std::uint64_t offset,
                                 std::size_t length) {
    if (offset > static_cast<std::uint64_t>(
                         std::numeric_limits<std::streamoff>::max())) {
        invalid("Archive offset is outside the readable range");
    }
    std::vector<std::uint8_t> bytes(length);
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input || (length != 0U
            && !input.read(reinterpret_cast<char*>(bytes.data()),
                           static_cast<std::streamsize>(length)))) {
        invalid("Archive structure is truncated");
    }
    return bytes;
}

std::string encodeHex(const unsigned char* digest, std::size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(length * 2U, '0');
    for (std::size_t index = 0U; index < length; ++index) {
        output[index * 2U] = kHex[digest[index] >> 4U];
        output[index * 2U + 1U] = kHex[digest[index] & 0x0fU];
    }
    return output;
}

std::array<std::uint8_t, kMd5Bytes> md5(const std::string& value) {
    DigestContext context(EVP_MD_CTX_new());
    std::array<std::uint8_t, kMd5Bytes> digest{};
    unsigned int length = 0U;
    if (!context
        || EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr) != 1
        || EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1
        || EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1
        || length != digest.size()) {
        throw std::runtime_error("MD5 calculation failed");
    }
    return digest;
}

std::string sha256(const std::string& value) {
    DigestContext context(EVP_MD_CTX_new());
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0U;
    if (!context
        || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1
        || EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1
        || EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1
        || length != kDigestBytes) {
        throw std::runtime_error("SHA-256 calculation failed");
    }
    return encodeHex(digest.data(), length);
}

std::string sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("Resource profile document is unavailable");
    DigestContext context(EVP_MD_CTX_new());
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("SHA-256 calculation failed");
    }
    std::array<char, kReadBufferBytes> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0
            && EVP_DigestUpdate(context.get(), buffer.data(),
                                static_cast<std::size_t>(count)) != 1) {
            throw std::runtime_error("SHA-256 calculation failed");
        }
    }
    if (!input.eof()) throw std::invalid_argument("Resource profile document is unreadable");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0U;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1
        || length != kDigestBytes) {
        throw std::runtime_error("SHA-256 calculation failed");
    }
    return encodeHex(digest.data(), length);
}

bool isValidUtf8(const std::string& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    std::size_t index = 0U;
    while (index < value.size()) {
        const std::uint8_t first = bytes[index];
        std::size_t remaining = 0U;
        std::uint32_t code_point = 0U;
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if ((first & 0xe0U) == 0xc0U) {
            remaining = 1U;
            code_point = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            remaining = 2U;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            remaining = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (remaining > value.size() - index - 1U) return false;
        for (std::size_t part = 0U; part < remaining; ++part) {
            const std::uint8_t next = bytes[index + part + 1U];
            if ((next & 0xc0U) != 0x80U) return false;
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        if ((remaining == 1U && code_point < 0x80U)
            || (remaining == 2U && code_point < 0x800U)
            || (remaining == 3U && code_point < 0x10000U)
            || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += remaining + 1U;
    }
    return true;
}

bool containsNonAscii(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char value_byte) {
        return value_byte > 0x7fU;
    });
}

std::string unicodeNfc(const std::string& value) {
    utf8proc_uint8_t* normalized = utf8proc_NFC(
            reinterpret_cast<const utf8proc_uint8_t*>(value.c_str()));
    if (normalized == nullptr) invalid("Package path Unicode normalization failed");
    std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(
            normalized, &std::free);
    return reinterpret_cast<const char*>(owned.get());
}

std::string unicodeCaseFoldKey(const std::string& value) {
    utf8proc_uint8_t* mapped = nullptr;
    const auto options = static_cast<utf8proc_option_t>(
            UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
    const utf8proc_ssize_t length = utf8proc_map(
            reinterpret_cast<const utf8proc_uint8_t*>(value.data()),
            static_cast<utf8proc_ssize_t>(value.size()), &mapped, options);
    if (length < 0 || mapped == nullptr) {
        invalid("Package path Unicode case folding failed");
    }
    std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(mapped,
                                                                 &std::free);
    return std::string(reinterpret_cast<const char*>(owned.get()),
                       static_cast<std::size_t>(length));
}

std::string lowercaseAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char current) {
                       return static_cast<char>(std::tolower(current));
                   });
    return value;
}

std::uint64_t zip64Value(const std::vector<std::uint8_t>& extra,
                         std::size_t& cursor) {
    if (cursor > extra.size() || extra.size() - cursor < sizeof(std::uint64_t)) {
        invalid("ZIP64 extra field is truncated");
    }
    const std::uint64_t value = readLe64(extra.data() + cursor);
    cursor += sizeof(std::uint64_t);
    return value;
}

void applyZip64Extra(CentralEntry& entry, std::uint16_t disk_start,
                     const std::vector<std::uint8_t>& extra,
                     std::uint32_t compressed32, std::uint32_t expanded32,
                     std::uint32_t offset32) {
    std::size_t field = 0U;
    bool found = false;
    while (field < extra.size()) {
        if (extra.size() - field < 4U) invalid("ZIP extra field is truncated");
        const std::uint16_t tag = readLe16(extra.data() + field);
        const std::uint16_t size = readLe16(extra.data() + field + 2U);
        field += 4U;
        if (size > extra.size() - field) invalid("ZIP extra field is truncated");
        if (tag == kZip64ExtraField) {
            found = true;
            std::vector<std::uint8_t> values(
                    extra.begin() + static_cast<std::ptrdiff_t>(field),
                    extra.begin() + static_cast<std::ptrdiff_t>(field + size));
            std::size_t cursor = 0U;
            if (expanded32 == std::numeric_limits<std::uint32_t>::max()) {
                entry.expanded_size = zip64Value(values, cursor);
            }
            if (compressed32 == std::numeric_limits<std::uint32_t>::max()) {
                entry.compressed_size = zip64Value(values, cursor);
            }
            if (offset32 == std::numeric_limits<std::uint32_t>::max()) {
                entry.local_header_offset = zip64Value(values, cursor);
            }
            if (disk_start == std::numeric_limits<std::uint16_t>::max()) {
                if (cursor > values.size() || values.size() - cursor < 4U
                    || readLe32(values.data() + cursor) != 0U) {
                    invalid("Multi-volume ZIP archives are unsupported");
                }
            } else if (disk_start != 0U) {
                invalid("Multi-volume ZIP archives are unsupported");
            }
            break;
        }
        field += size;
    }
    if (!found && (compressed32 == std::numeric_limits<std::uint32_t>::max()
            || expanded32 == std::numeric_limits<std::uint32_t>::max()
            || offset32 == std::numeric_limits<std::uint32_t>::max()
            || disk_start == std::numeric_limits<std::uint16_t>::max())) {
        invalid("ZIP64 extra field is missing");
    }
    if (disk_start != 0U
        && disk_start != std::numeric_limits<std::uint16_t>::max()) {
        invalid("Multi-volume ZIP archives are unsupported");
    }
}

ArchiveLayout parseArchiveLayout(const std::filesystem::path& path,
                                 std::uint64_t archive_size,
                                 const PackageLimits& limits) {
    std::ifstream input(path, std::ios::binary);
    if (!input) invalid("Archive scratch file is unavailable");
    constexpr std::uint64_t kMaximumEocdSearch = 65557U;
    const std::uint64_t tail_size = std::min(archive_size, kMaximumEocdSearch);
    if (tail_size < 22U) invalid("Archive end record is missing");
    const auto tail = readAt(input, archive_size - tail_size,
                             static_cast<std::size_t>(tail_size));
    std::optional<std::size_t> eocd_index;
    for (std::size_t cursor = tail.size() - 22U;; --cursor) {
        if (readLe32(tail.data() + cursor) == kEndOfCentralDirectorySignature
            && cursor + 22U + readLe16(tail.data() + cursor + 20U)
                    == tail.size()) {
            eocd_index = cursor;
            break;
        }
        if (cursor == 0U) break;
    }
    if (!eocd_index.has_value()) invalid("Archive end record is invalid");
    const auto* eocd = tail.data() + *eocd_index;
    const std::uint16_t disk = readLe16(eocd + 4U);
    const std::uint16_t central_disk = readLe16(eocd + 6U);
    const std::uint16_t entries_disk = readLe16(eocd + 8U);
    const std::uint16_t entries_total = readLe16(eocd + 10U);
    std::uint64_t entry_count = entries_total;
    std::uint64_t central_size = readLe32(eocd + 12U);
    std::uint64_t central_offset = readLe32(eocd + 16U);
    if (disk != 0U || central_disk != 0U || entries_disk != entries_total) {
        invalid("Multi-volume ZIP archives are unsupported");
    }
    const bool zip64 = entries_total == std::numeric_limits<std::uint16_t>::max()
            || central_size == std::numeric_limits<std::uint32_t>::max()
            || central_offset == std::numeric_limits<std::uint32_t>::max();
    const std::uint64_t eocd_offset = archive_size - tail_size + *eocd_index;
    if (zip64) {
        if (eocd_offset < 20U) invalid("ZIP64 locator is missing");
        const auto locator = readAt(input, eocd_offset - 20U, 20U);
        if (readLe32(locator.data()) != kZip64LocatorSignature
            || readLe32(locator.data() + 4U) != 0U
            || readLe32(locator.data() + 16U) != 1U) {
            invalid("ZIP64 locator is invalid");
        }
        const std::uint64_t zip64_offset = readLe64(locator.data() + 8U);
        const auto zip64_end = readAt(input, zip64_offset, 56U);
        if (readLe32(zip64_end.data()) != kZip64EndSignature
            || readLe64(zip64_end.data() + 4U) < 44U
            || readLe32(zip64_end.data() + 16U) != 0U
            || readLe32(zip64_end.data() + 20U) != 0U
            || readLe64(zip64_end.data() + 24U)
                    != readLe64(zip64_end.data() + 32U)) {
            invalid("ZIP64 end record is invalid");
        }
        entry_count = readLe64(zip64_end.data() + 32U);
        central_size = readLe64(zip64_end.data() + 40U);
        central_offset = readLe64(zip64_end.data() + 48U);
    }
    if (entry_count == 0U) invalid("Archive contains no records");
    if (entry_count > limits.maximum_entry_count) {
        limitExceeded("Archive record count exceeds the configured limit");
    }
    const std::uint64_t central_end = checkedAddInvalid(
            central_offset, central_size, "Archive central directory overflows");
    if (central_end > eocd_offset) invalid("Archive central directory is outside bounds");

    ArchiveLayout layout;
    layout.central_offset = central_offset;
    layout.central_size = central_size;
    layout.entries.reserve(static_cast<std::size_t>(entry_count));
    std::uint64_t cursor = central_offset;
    for (std::uint64_t ordinal = 0U; ordinal < entry_count; ++ordinal) {
        const auto fixed = readAt(input, cursor, 46U);
        if (readLe32(fixed.data()) != kCentralHeaderSignature) {
            invalid("Archive central directory is invalid");
        }
        const std::uint16_t name_length = readLe16(fixed.data() + 28U);
        const std::uint16_t extra_length = readLe16(fixed.data() + 30U);
        const std::uint16_t comment_length = readLe16(fixed.data() + 32U);
        const std::uint64_t variable_length = static_cast<std::uint64_t>(name_length)
                + extra_length + comment_length;
        const auto variable = readAt(
                input, checkedAddInvalid(cursor, 46U,
                         "Archive central record overflows"),
                static_cast<std::size_t>(variable_length));
        CentralEntry entry;
        entry.ordinal = ordinal;
        entry.creator_system = static_cast<std::uint8_t>(
                readLe16(fixed.data() + 4U) >> 8U);
        entry.flags = readLe16(fixed.data() + 8U);
        entry.compression_method = readLe16(fixed.data() + 10U);
        const std::uint32_t compressed32 = readLe32(fixed.data() + 20U);
        const std::uint32_t expanded32 = readLe32(fixed.data() + 24U);
        entry.compressed_size = compressed32;
        entry.expanded_size = expanded32;
        entry.external_attributes = readLe32(fixed.data() + 38U);
        const std::uint32_t offset32 = readLe32(fixed.data() + 42U);
        entry.local_header_offset = offset32;
        entry.raw_name.assign(reinterpret_cast<const char*>(variable.data()),
                              name_length);
        std::vector<std::uint8_t> extra(
                variable.begin() + static_cast<std::ptrdiff_t>(name_length),
                variable.begin() + static_cast<std::ptrdiff_t>(
                        static_cast<std::size_t>(name_length) + extra_length));
        applyZip64Extra(entry, readLe16(fixed.data() + 34U), extra,
                        compressed32, expanded32, offset32);
        if ((entry.flags & (kEncryptedFlag | kStrongEncryptionFlag)) != 0U) {
            invalid("Encrypted archive entries are unsupported");
        }
        if (entry.raw_name.empty() || entry.raw_name.find('\0') != std::string::npos
            || !isValidUtf8(entry.raw_name)
            || ((entry.flags & kUtf8NameFlag) == 0U
                    && containsNonAscii(entry.raw_name))) {
            invalid("Archive entry name is not unambiguous UTF-8");
        }

        const auto local = readAt(input, entry.local_header_offset, 30U);
        if (readLe32(local.data()) != kLocalHeaderSignature
            || readLe16(local.data() + 6U) != entry.flags
            || readLe16(local.data() + 8U) != entry.compression_method) {
            invalid("Archive local header differs from central directory");
        }
        const std::uint16_t local_name_length = readLe16(local.data() + 26U);
        const std::uint16_t local_extra_length = readLe16(local.data() + 28U);
        const auto local_name = readAt(
                input, checkedAddInvalid(entry.local_header_offset, 30U,
                                         "Archive local header overflows"),
                local_name_length);
        if (local_name_length != name_length
            || !std::equal(local_name.begin(), local_name.end(),
                           variable.begin())) {
            invalid("Archive local and central names differ");
        }
        std::uint64_t data_offset = checkedAddInvalid(
                entry.local_header_offset, 30U,
                "Archive local data offset overflows");
        data_offset = checkedAddInvalid(data_offset, local_name_length,
                                       "Archive local data offset overflows");
        data_offset = checkedAddInvalid(data_offset, local_extra_length,
                                       "Archive local data offset overflows");
        if (checkedAddInvalid(data_offset, entry.compressed_size,
                              "Archive entry range overflows") > central_offset) {
            invalid("Archive entry data overlaps the central directory");
        }
        cursor = checkedAddInvalid(cursor, 46U + variable_length,
                                   "Archive central record overflows");
        if (cursor > central_end) invalid("Archive central directory is truncated");
        layout.entries.push_back(std::move(entry));
    }
    if (cursor != central_end) invalid("Archive central directory size is inconsistent");
    return layout;
}

PackageCompressionMethod compressionMethod(std::uint16_t method) {
    switch (method) {
        case kZipMethodStore: return PackageCompressionMethod::store;
        case kZipMethodDeflate: return PackageCompressionMethod::deflate;
        case kZipMethodZstandard: return PackageCompressionMethod::zstandard;
        default: return PackageCompressionMethod::unknown;
    }
}

bool isDirectoryMarker(const CentralEntry& entry) {
    return !entry.raw_name.empty() && entry.raw_name.back() == '/';
}

bool isSymlinkLike(const CentralEntry& entry) {
    if (entry.creator_system != kUnixCreatorSystem) return false;
    const std::uint32_t unix_mode = entry.external_attributes >> 16U;
    const std::uint32_t type = unix_mode & kUnixTypeMask;
    return type == kUnixSymlinkType
            || (type != 0U && type != kUnixRegularType
                    && type != kUnixDirectoryType);
}

bool isZipMagic(const std::array<std::uint8_t, 4U>& prefix,
                std::size_t prefix_size) {
    if (prefix_size != prefix.size() || prefix[0] != 0x50U
        || prefix[1] != 0x4bU) {
        return false;
    }
    const std::uint16_t signature = static_cast<std::uint16_t>(prefix[2])
            | static_cast<std::uint16_t>(prefix[3]) << 8U;
    return signature == 0x0403U || signature == 0x0605U
            || signature == 0x0807U;
}

StreamedEntry readEntry(zip_t* archive, std::uint64_t index,
                        const CentralEntry& entry, const PackageLimits& limits,
                        bool retain_bytes,
                        const ContinuePredicate& should_continue) {
    ZipFile file(zip_fopen_index(archive, index, ZIP_FL_UNCHANGED));
    if (!file) invalid("Archive entry cannot be opened");
    DigestContext digest(EVP_MD_CTX_new());
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("SHA-256 calculation failed");
    }
    StreamedEntry result;
    if (retain_bytes) {
        const std::uint64_t retain_limit = checkedMultiplyInvalid(
                checkedAddInvalid(limits.maximum_entry_count, 1U,
                                  "3TZ index retention limit overflows"),
                kThreeTzIndexRecordBytes,
                "3TZ index retention limit overflows");
        if (entry.expanded_size > retain_limit) {
            limitExceeded("3TZ index exceeds the configured record limit");
        }
        result.retained_bytes.reserve(static_cast<std::size_t>(entry.expanded_size));
    }
    std::array<std::uint8_t, kReadBufferBytes> buffer{};
    std::array<std::uint8_t, 4U> prefix{};
    std::size_t prefix_size = 0U;
    while (true) {
        requireContinue(should_continue);
        const zip_int64_t count = zip_fread(file.get(), buffer.data(), buffer.size());
        if (count < 0) invalid("Archive entry data is invalid");
        if (count == 0) break;
        const auto byte_count = static_cast<std::uint64_t>(count);
        result.observed_size = checkedAdd(
                result.observed_size, byte_count,
                "Archive entry expanded size overflows");
        if (result.observed_size > limits.maximum_entry_bytes) {
            limitExceeded("Archive entry exceeds the configured expanded limit");
        }
        const auto size_count = static_cast<std::size_t>(count);
        const std::size_t prefix_count = std::min(
                size_count, prefix.size() - prefix_size);
        std::copy_n(buffer.begin(), prefix_count,
                    prefix.begin() + static_cast<std::ptrdiff_t>(prefix_size));
        prefix_size += prefix_count;
        if (EVP_DigestUpdate(digest.get(), buffer.data(), size_count) != 1) {
            throw std::runtime_error("SHA-256 calculation failed");
        }
        if (retain_bytes) {
            result.retained_bytes.insert(result.retained_bytes.end(),
                                         buffer.begin(),
                                         buffer.begin()
                                                 + static_cast<std::ptrdiff_t>(size_count));
        }
    }
    if (result.observed_size != entry.expanded_size) {
        invalid("Archive entry expanded size differs from its declaration");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest_bytes{};
    unsigned int digest_length = 0U;
    if (EVP_DigestFinal_ex(digest.get(), digest_bytes.data(), &digest_length) != 1
        || digest_length != kDigestBytes) {
        throw std::runtime_error("SHA-256 calculation failed");
    }
    result.sha256 = encodeHex(digest_bytes.data(), digest_length);
    result.has_zip_magic = isZipMagic(prefix, prefix_size);
    return result;
}

std::string compatibleThreeTzPath(const std::string& raw_name) {
    std::string path = raw_name;
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    std::vector<std::string> segments;
    std::size_t start = 0U;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string segment = path.substr(start, end - start);
        if (!segment.empty() && segment != ".") segments.push_back(segment);
        if (slash == std::string::npos) break;
        start = slash + 1U;
    }
    std::ostringstream normalized;
    for (std::size_t index = 0U; index < segments.size(); ++index) {
        if (index != 0U) normalized << '/';
        normalized << segments[index];
    }
    return normalized.str();
}

bool digestLess(const std::array<std::uint8_t, kMd5Bytes>& left,
                const std::array<std::uint8_t, kMd5Bytes>& right) {
    const std::uint64_t left_first = readLe64(left.data());
    const std::uint64_t right_first = readLe64(right.data());
    if (left_first != right_first) return left_first < right_first;
    return readLe64(left.data() + 8U) < readLe64(right.data() + 8U);
}

void validateThreeTzIndex(const std::vector<std::uint8_t>& bytes,
                          const std::vector<CentralEntry>& entries,
                          std::size_t index_ordinal) {
    if (bytes.size() % kThreeTzIndexRecordBytes != 0U) {
        invalid("3TZ index length is invalid");
    }
    std::vector<const CentralEntry*> logical_files;
    for (const auto& entry : entries) {
        if (entry.ordinal != index_ordinal && !isDirectoryMarker(entry)) {
            logical_files.push_back(&entry);
        }
    }
    if (bytes.size() / kThreeTzIndexRecordBytes != logical_files.size()) {
        invalid("3TZ index does not cover every logical file");
    }
    std::map<std::uint64_t, const CentralEntry*> by_offset;
    for (const auto* entry : logical_files) {
        if (!by_offset.emplace(entry->local_header_offset, entry).second) {
            invalid("3TZ archive contains duplicate local offsets");
        }
    }
    std::set<std::uint64_t> covered_offsets;
    std::optional<std::array<std::uint8_t, kMd5Bytes>> previous;
    for (std::size_t cursor = 0U; cursor < bytes.size();
         cursor += kThreeTzIndexRecordBytes) {
        ThreeTzIndexRecord record;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                    kMd5Bytes, record.digest.begin());
        record.local_header_offset = readLe64(bytes.data() + cursor + kMd5Bytes);
        if (previous.has_value() && digestLess(record.digest, *previous)) {
            invalid("3TZ index records are not in the required order");
        }
        previous = record.digest;
        const auto entry = by_offset.find(record.local_header_offset);
        if (entry == by_offset.end()
            || md5(compatibleThreeTzPath(entry->second->raw_name)) != record.digest
            || !covered_offsets.insert(record.local_header_offset).second) {
            invalid("3TZ index identity does not match archive entries");
        }
    }
    if (covered_offsets.size() != logical_files.size()) {
        invalid("3TZ index coverage is incomplete");
    }
}

void validateObservedSource(const SourceObject& source,
                            const ObservedSourceObject& observed) {
    if (source.size_bytes != observed.observed_size
        || client::normalizeEtag(source.etag)
                != client::normalizeEtag(observed.observed_etag)
        || (source.sha256.has_value()
                && lowercaseAscii(*source.sha256)
                        != lowercaseAscii(observed.observed_sha256))) {
        invalid("Approved source object identity changed during enumeration");
    }
}

std::string entryIdentity(const std::string& parent_id,
                          const CentralEntry& entry) {
    return sha256(std::string(kEntryIdentityDomain) + '\0' + parent_id + '\0'
                  + entry.raw_name + '\0'
                  + std::to_string(entry.local_header_offset));
}

void stableSort(std::vector<PackageEntryEvidence>& entries) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const PackageEntryEvidence& left,
                        const PackageEntryEvidence& right) {
                         if (left.path.normalized_candidate
                             != right.path.normalized_candidate) {
                             return left.path.normalized_candidate
                                     < right.path.normalized_candidate;
                         }
                         return left.stable_ordinal < right.stable_ordinal;
                     });
}

}  // namespace

PackageEnumerationError::PackageEnumerationError(
        PackageFailureKind kind, std::string message)
    : std::runtime_error(std::move(message)), kind_(kind) {
}

PackageFailureKind PackageEnumerationError::kind() const noexcept {
    return kind_;
}

DiagnosticCode PackageEnumerationError::diagnosticCode() const noexcept {
    switch (kind_) {
        case PackageFailureKind::invalid: return DiagnosticCode::package_invalid;
        case PackageFailureKind::limit_exceeded:
            return DiagnosticCode::package_limit_exceeded;
        case PackageFailureKind::cancelled: return DiagnosticCode::deadline_expired;
    }
    return DiagnosticCode::inspector_internal_failure;
}

PackageLimits PackageLimits::load(
        const std::filesystem::path& document, const std::string& profile_id,
        const std::string& expected_document_sha256) {
    if (profile_id.empty() || expected_document_sha256.size() != 64U) {
        throw std::invalid_argument("Resource profile identity is invalid");
    }
    const std::string actual_sha256 = sha256File(document);
    if (lowercaseAscii(expected_document_sha256) != actual_sha256) {
        throw std::invalid_argument("Resource profile document hash mismatch");
    }
    std::ifstream input(document, std::ios::binary);
    Json root;
    try {
        input >> root;
    } catch (const std::exception&) {
        throw std::invalid_argument("Resource profile document is invalid");
    }
    if (!root.is_object() || root.value("schemaVersion", 0) != 1
        || !root.contains("profiles") || !root["profiles"].is_array()) {
        throw std::invalid_argument("Resource profile document is invalid");
    }
    const Json* selected = nullptr;
    for (const auto& profile : root["profiles"]) {
        if (profile.is_object() && profile.value("id", std::string{}) == profile_id) {
            if (selected != nullptr) {
                throw std::invalid_argument("Resource profile identity is duplicated");
            }
            selected = &profile;
        }
    }
    if (selected == nullptr) throw std::invalid_argument("Resource profile is unavailable");
    PackageLimits limits;
    limits.profile_id = profile_id;
    limits.document_sha256 = actual_sha256;
    try {
        const auto& archive = selected->at("archive");
        const auto& closure = selected->at("resourceClosure");
        limits.maximum_package_bytes =
                archive.at("maximumPackageBytes").get<std::uint64_t>();
        limits.maximum_expanded_bytes =
                archive.at("maximumExpandedBytes").get<std::uint64_t>();
        limits.maximum_entry_bytes =
                archive.at("maximumEntryBytes").get<std::uint64_t>();
        limits.maximum_entry_count =
                archive.at("maximumEntryCount").get<std::uint64_t>();
        limits.maximum_path_utf8_bytes =
                archive.at("maximumPathUtf8Bytes").get<std::uint64_t>();
        limits.maximum_nesting_depth =
                archive.at("maximumNestingDepth").get<std::uint32_t>();
        limits.maximum_compression_ratio =
                archive.at("maximumCompressionRatio").get<std::uint64_t>();
        limits.maximum_resource_count =
                closure.at("maximumResourceCount").get<std::uint64_t>();
    } catch (const std::exception&) {
        throw std::invalid_argument("Resource profile limits are invalid");
    }
    if (limits.maximum_package_bytes == 0U
        || limits.maximum_expanded_bytes == 0U
        || limits.maximum_entry_bytes == 0U
        || limits.maximum_entry_count == 0U
        || limits.maximum_resource_count < 2U
        || limits.maximum_path_utf8_bytes == 0U
        || limits.maximum_nesting_depth == 0U
        || limits.maximum_compression_ratio == 0U
        || limits.maximum_entry_bytes > limits.maximum_expanded_bytes
        || limits.maximum_package_bytes
                > ProtocolLimits::kMaximumSourceObjectBytes
        || limits.maximum_expanded_bytes
                > ProtocolLimits::kMaximumExpandedResourceBytes
        || limits.maximum_entry_count > ProtocolLimits::kMaximumResourceCount
        || limits.maximum_resource_count > ProtocolLimits::kMaximumResourceCount
        || limits.maximum_path_utf8_bytes
                > ProtocolLimits::kMaximumPackagePathUtf8Bytes) {
        throw std::invalid_argument("Resource profile limits are inconsistent");
    }
    return limits;
}

PathAnalysisFlag operator|(PathAnalysisFlag left,
                           PathAnalysisFlag right) noexcept {
    return static_cast<PathAnalysisFlag>(static_cast<std::uint32_t>(left)
                                         | static_cast<std::uint32_t>(right));
}

PathAnalysisFlag& operator|=(PathAnalysisFlag& left,
                             PathAnalysisFlag right) noexcept {
    left = left | right;
    return left;
}

bool hasFlag(PathAnalysisFlag value, PathAnalysisFlag flag) noexcept {
    return (static_cast<std::uint32_t>(value)
            & static_cast<std::uint32_t>(flag)) != 0U;
}

PackagePathAnalysis PackagePathAnalyzer::analyze(
        const std::string& raw_name,
        std::uint64_t maximum_path_utf8_bytes) const {
    if (raw_name.empty() || raw_name.find('\0') != std::string::npos
        || !isValidUtf8(raw_name)) {
        invalid("Package path is not valid UTF-8");
    }
    if (raw_name.size() > maximum_path_utf8_bytes) {
        limitExceeded("Package path exceeds the configured UTF-8 byte limit");
    }
    PackagePathAnalysis result{raw_name, raw_name, PathAnalysisFlag::none};
    if (unicodeNfc(raw_name) != raw_name) {
        result.flags |= PathAnalysisFlag::non_nfc;
    }
    if (raw_name.front() == '/') result.flags |= PathAnalysisFlag::absolute;
    if (raw_name.size() >= 2U
        && std::isalpha(static_cast<unsigned char>(raw_name[0])) != 0
        && raw_name[1] == ':') {
        result.flags |= PathAnalysisFlag::absolute;
        result.flags |= PathAnalysisFlag::drive_prefix;
    }
    if (raw_name.find('\\') != std::string::npos) {
        result.flags |= PathAnalysisFlag::backslash;
    }
    std::size_t start = 0U;
    while (start <= raw_name.size()) {
        const std::size_t slash = raw_name.find('/', start);
        const std::size_t end = slash == std::string::npos
                ? raw_name.size() : slash;
        const std::string segment = raw_name.substr(start, end - start);
        if (segment.empty()) {
            result.flags |= PathAnalysisFlag::empty_segment;
        }
        if (segment == "." || segment == "..") {
            result.flags |= PathAnalysisFlag::dot_segment;
        }
        if (slash == std::string::npos) break;
        start = slash + 1U;
    }
    const std::string lowercase = lowercaseAscii(raw_name);
    if (lowercase.find("%2e") != std::string::npos
        || lowercase.find("%2f") != std::string::npos
        || lowercase.find("%5c") != std::string::npos) {
        result.flags |= PathAnalysisFlag::percent_encoded_alias;
    }
    const std::size_t colon = raw_name.find(':');
    const std::size_t slash = raw_name.find('/');
    if (colon != std::string::npos
        && (slash == std::string::npos || colon < slash)
        && !hasFlag(result.flags, PathAnalysisFlag::drive_prefix)) {
        result.flags |= PathAnalysisFlag::unsupported_scheme;
    }
    if (raw_name == kArchiveContainerPath
        || raw_name.rfind("__inspection_package__/", 0U) == 0U) {
        result.flags |= PathAnalysisFlag::reserved_namespace;
    }
    return result;
}

void PackageNamespaceValidator::validate(
        const std::vector<PackageEntryEvidence>& entries) const {
    std::set<std::string> exact_paths;
    std::set<std::string> folded_paths;
    std::set<std::string> file_paths;
    std::set<std::string> directory_paths;
    for (const auto& entry : entries) {
        if (entry.entry_kind == PackageEntryKind::archive_container) continue;
        const PathAnalysisFlag forbidden = PathAnalysisFlag::absolute
                | PathAnalysisFlag::drive_prefix | PathAnalysisFlag::backslash
                | PathAnalysisFlag::dot_segment
                | PathAnalysisFlag::percent_encoded_alias
                | PathAnalysisFlag::unsupported_scheme
                | PathAnalysisFlag::reserved_namespace
                | PathAnalysisFlag::symlink_like
                | PathAnalysisFlag::empty_segment | PathAnalysisFlag::non_nfc;
        if (hasFlag(entry.path.flags, forbidden)) {
            invalid("Package namespace contains an unsafe path");
        }
        const std::string& path = entry.path.normalized_candidate;
        if (!exact_paths.insert(path).second
            || !folded_paths.insert(unicodeCaseFoldKey(path)).second) {
            invalid("Package namespace contains a duplicate or case-colliding path");
        }
        if (entry.entry_kind == PackageEntryKind::directory_marker) {
            directory_paths.insert(path);
        } else {
            file_paths.insert(path);
        }
    }
    for (const auto& file : file_paths) {
        if (directory_paths.count(file) != 0U) {
            invalid("Package namespace contains a file-directory collision");
        }
        std::size_t separator = file.find('/');
        while (separator != std::string::npos) {
            if (file_paths.count(file.substr(0U, separator)) != 0U) {
                invalid("Package namespace contains a file-directory collision");
            }
            separator = file.find('/', separator + 1U);
        }
    }
}

PackageEnumerationResult DirectoryPackageEnumerator::enumerate(
        const InspectionRequest& request,
        const std::vector<SourceManifestPage>& pages,
        const PackageLimits& limits,
        const DirectoryObjectReader& object_reader,
        const std::string& now_utc,
        const ContinuePredicate& should_continue) const {
    if (request.package_kind != PackageKind::directory || !object_reader) {
        throw std::invalid_argument("Directory enumeration input is invalid");
    }
    validateSourceManifest(request, pages, now_utc);
    std::vector<std::pair<std::uint64_t, const SourceObject*>> sources;
    std::set<std::string> object_ids;
    std::uint64_t ordinal = 0U;
    for (const auto& page : pages) {
        for (const auto& source : page.records) {
            if (!object_ids.insert(source.object_id).second) {
                invalid("Source manifest contains duplicate identities");
            }
            sources.emplace_back(ordinal++, &source);
        }
    }
    if (sources.size() > limits.maximum_entry_count
        || sources.size() > limits.maximum_resource_count) {
        limitExceeded("Directory resource count exceeds the configured limit");
    }
    PackageEnumerationResult result;
    result.package_kind = PackageKind::directory;
    PackagePathAnalyzer analyzer;
    std::vector<PackageEntryEvidence> namespace_entries;
    namespace_entries.reserve(sources.size());
    for (const auto& source_with_ordinal : sources) {
        PackageEntryEvidence evidence;
        evidence.object_id = source_with_ordinal.second->object_id;
        evidence.path = analyzer.analyze(
                source_with_ordinal.second->package_relative_path,
                limits.maximum_path_utf8_bytes);
        evidence.entry_kind = PackageEntryKind::file;
        namespace_entries.push_back(std::move(evidence));
    }
    PackageNamespaceValidator{}.validate(namespace_entries);
    for (const auto& source_with_ordinal : sources) {
        requireContinue(should_continue);
        const SourceObject& source = *source_with_ordinal.second;
        result.package_bytes = checkedAdd(
                result.package_bytes, source.size_bytes,
                "Directory package size overflows");
        if (result.package_bytes > limits.maximum_package_bytes) {
            limitExceeded("Directory package exceeds the configured size limit");
        }
        const ObservedSourceObject observed = object_reader(source);
        validateObservedSource(source, observed);
        if (observed.observed_size > limits.maximum_entry_bytes) {
            limitExceeded("Directory object exceeds the configured entry limit");
        }
        result.expanded_bytes = checkedAdd(
                result.expanded_bytes, observed.observed_size,
                "Directory expanded size overflows");
        if (result.expanded_bytes > limits.maximum_expanded_bytes) {
            limitExceeded("Directory expanded size exceeds the configured limit");
        }
        PackageEntryEvidence evidence;
        evidence.object_id = source.object_id;
        evidence.path = namespace_entries[static_cast<std::size_t>(
                source_with_ordinal.first)].path;
        evidence.entry_kind = PackageEntryKind::file;
        evidence.archive_depth = 0U;
        evidence.compressed_bytes = observed.observed_size;
        evidence.declared_expanded_bytes = source.size_bytes;
        evidence.observed_expanded_bytes = observed.observed_size;
        evidence.source_etag = observed.observed_etag;
        evidence.observed_sha256 = observed.observed_sha256;
        evidence.stable_ordinal = source_with_ordinal.first;
        result.entries.push_back(std::move(evidence));
    }
    stableSort(result.entries);
    return result;
}

PackageEnumerationResult ZipPackageEnumerator::enumerate(
        PackageKind package_kind, const SourceObject& source,
        const ObservedSourceObject& observed_archive,
        const std::filesystem::path& archive_path,
        const PackageLimits& limits,
        const ContinuePredicate& should_continue) const {
    if (package_kind != PackageKind::zip
        && package_kind != PackageKind::three_tz) {
        throw std::invalid_argument("Archive package kind is invalid");
    }
    validateObservedSource(source, observed_archive);
    if (observed_archive.observed_size > limits.maximum_package_bytes) {
        limitExceeded("Archive package exceeds the configured size limit");
    }
    const ArchiveLayout layout = parseArchiveLayout(
            archive_path, observed_archive.observed_size, limits);
    int open_error = 0;
    ZipArchive archive(zip_open(archive_path.string().c_str(),
                                ZIP_RDONLY | ZIP_CHECKCONS, &open_error));
    if (!archive) invalid("Archive structure is invalid");
    const zip_int64_t libzip_count = zip_get_num_entries(archive.get(), 0);
    if (libzip_count < 0
        || static_cast<std::uint64_t>(libzip_count) != layout.entries.size()) {
        invalid("Archive record count is inconsistent");
    }

    PackagePathAnalyzer analyzer;
    std::vector<std::optional<PackageEntryEvidence>> namespace_by_index(
            layout.entries.size());
    std::vector<PackageEntryEvidence> namespace_entries;
    std::uint64_t index_name_count = 0U;
    for (std::size_t index = 0U; index < layout.entries.size(); ++index) {
        const CentralEntry& entry = layout.entries[index];
        if (entry.raw_name == kThreeTzIndexPath) {
            ++index_name_count;
            if (isSymlinkLike(entry)) {
                invalid("3TZ index cannot be a link or special entry");
            }
            continue;
        }
        PackageEntryEvidence evidence;
        evidence.object_id = entryIdentity(source.object_id, entry);
        const bool directory = isDirectoryMarker(entry);
        const std::string namespace_name = directory
                ? entry.raw_name.substr(0U, entry.raw_name.size() - 1U)
                : entry.raw_name;
        evidence.path = analyzer.analyze(namespace_name,
                                         limits.maximum_path_utf8_bytes);
        if (isSymlinkLike(entry)) {
            evidence.path.flags |= PathAnalysisFlag::symlink_like;
        }
        evidence.entry_kind = directory
                ? PackageEntryKind::directory_marker
                : PackageEntryKind::file;
        namespace_by_index[index] = evidence;
        namespace_entries.push_back(std::move(evidence));
    }
    if (index_name_count > 1U) invalid("3TZ index identity is duplicated");
    PackageNamespaceValidator{}.validate(namespace_entries);

    PackageEnumerationResult result;
    result.package_kind = package_kind;
    result.package_bytes = observed_archive.observed_size;
    result.archive_record_count = layout.entries.size();
    PackageEntryEvidence container;
    container.object_id = source.object_id;
    container.path = {source.package_relative_path,
                      source.package_relative_path,
                      PathAnalysisFlag::none};
    container.entry_kind = PackageEntryKind::archive_container;
    container.compression_method = PackageCompressionMethod::none;
    container.archive_depth = 1U;
    container.compressed_bytes = observed_archive.observed_size;
    container.declared_expanded_bytes = source.size_bytes;
    container.observed_expanded_bytes = observed_archive.observed_size;
    container.source_etag = observed_archive.observed_etag;
    container.observed_sha256 = observed_archive.observed_sha256;
    container.stable_ordinal = 0U;
    result.entries.push_back(std::move(container));

    std::optional<std::size_t> index_ordinal;
    std::vector<std::uint8_t> index_bytes;
    std::uint64_t logical_file_count = 0U;
    for (std::size_t index = 0U; index < layout.entries.size(); ++index) {
        requireContinue(should_continue);
        const CentralEntry& entry = layout.entries[index];
        zip_stat_t status;
        zip_stat_init(&status);
        if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(index),
                           ZIP_FL_UNCHANGED, &status) != 0
            || (status.valid & ZIP_STAT_SIZE) == 0U
            || (status.valid & ZIP_STAT_COMP_SIZE) == 0U
            || (status.valid & ZIP_STAT_COMP_METHOD) == 0U
            || status.size != entry.expanded_size
            || status.comp_size != entry.compressed_size
            || status.comp_method != entry.compression_method) {
            invalid("Archive metadata is inconsistent");
        }
        if (entry.expanded_size > limits.maximum_entry_bytes) {
            limitExceeded("Archive entry exceeds the configured expanded limit");
        }
        const PackageCompressionMethod method =
                compressionMethod(entry.compression_method);
        const bool is_index = entry.raw_name == kThreeTzIndexPath;
        const bool is_directory = isDirectoryMarker(entry);
        if (method == PackageCompressionMethod::unknown
            || (method == PackageCompressionMethod::zstandard
                    && package_kind != PackageKind::three_tz)) {
            invalid("Archive compression method is unsupported");
        }
        if (is_index) {
            if (package_kind != PackageKind::three_tz
                || index_ordinal.has_value()
                || index + 1U != layout.entries.size()
                || method != PackageCompressionMethod::store) {
                invalid("3TZ index placement or compression is invalid");
            }
            index_ordinal = index;
        }
        if (exceedsRatio(entry.expanded_size, entry.compressed_size,
                         limits.maximum_compression_ratio)) {
            limitExceeded("Archive entry compression ratio exceeds the configured limit");
        }
        const StreamedEntry streamed = readEntry(
                archive.get(), index, entry, limits, is_index, should_continue);
        if (exceedsRatio(streamed.observed_size, entry.compressed_size,
                         limits.maximum_compression_ratio)) {
            limitExceeded("Archive entry compression ratio exceeds the configured limit");
        }
        result.expanded_bytes = checkedAdd(
                result.expanded_bytes, streamed.observed_size,
                "Archive expanded size overflows");
        if (result.expanded_bytes > limits.maximum_expanded_bytes) {
            limitExceeded("Archive expanded size exceeds the configured limit");
        }
        if (is_index) {
            index_bytes = streamed.retained_bytes;
            continue;
        }
        if (is_directory) continue;
        ++logical_file_count;
        const std::uint64_t evidence_count = checkedAdd(
                logical_file_count, 1U,
                "Archive resource count overflows");
        if (evidence_count > limits.maximum_entry_count
            || evidence_count > limits.maximum_resource_count) {
            limitExceeded("Archive resource count exceeds the configured limit");
        }
        if (streamed.has_zip_magic
            && 2U > limits.maximum_nesting_depth) {
            limitExceeded("Nested archive exceeds the configured depth limit");
        }
        PackageEntryEvidence evidence;
        evidence.object_id = entryIdentity(source.object_id, entry);
        evidence.archive_parent_id = source.object_id;
        evidence.path = namespace_by_index[index]->path;
        evidence.entry_kind = PackageEntryKind::file;
        evidence.compression_method = method;
        evidence.archive_depth = 1U;
        evidence.compressed_bytes = entry.compressed_size;
        evidence.declared_expanded_bytes = entry.expanded_size;
        evidence.observed_expanded_bytes = streamed.observed_size;
        evidence.observed_sha256 = streamed.sha256;
        evidence.zip_creator_system = entry.creator_system;
        evidence.zip_external_attributes = entry.external_attributes;
        evidence.local_header_offset = entry.local_header_offset;
        evidence.stable_ordinal = entry.ordinal + 1U;
        result.entries.push_back(std::move(evidence));
    }
    if (package_kind == PackageKind::three_tz) {
        if (!index_ordinal.has_value()) invalid("3TZ index is missing");
        validateThreeTzIndex(index_bytes, layout.entries, *index_ordinal);
    } else if (index_ordinal.has_value()) {
        invalid("Reserved 3TZ index is not valid in a ZIP package");
    }
    if (exceedsRatio(result.expanded_bytes, result.package_bytes,
                     limits.maximum_compression_ratio)) {
        limitExceeded("Archive aggregate compression ratio exceeds the configured limit");
    }
    PackageNamespaceValidator{}.validate(result.entries);
    stableSort(result.entries);
    return result;
}

InspectionScratch::InspectionScratch(
        const std::filesystem::path& scratch_root,
        const std::string& inspection_id,
        const std::string& request_id) {
    if (scratch_root.empty() || inspection_id.empty() || request_id.empty()) {
        throw std::invalid_argument("Inspection scratch identity is invalid");
    }
    std::error_code error;
    const std::filesystem::path root = std::filesystem::absolute(
            scratch_root, error).lexically_normal();
    if (error) throw std::runtime_error("Inspection scratch root is invalid");
    std::filesystem::create_directories(root, error);
    if (error) throw std::runtime_error("Inspection scratch root is unavailable");
    const std::string identity = sha256(
            std::string(kScratchIdentityDomain) + '\0' + inspection_id + '\0'
            + request_id);
    directory_ = root / ("inspection-" + identity.substr(0U, 32U));
    if (!std::filesystem::create_directory(directory_, error) || error) {
        throw std::runtime_error("Inspection scratch directory is unavailable");
    }
}

InspectionScratch::~InspectionScratch() {
    std::error_code ignored;
    if (!directory_.empty()) std::filesystem::remove_all(directory_, ignored);
}

const std::filesystem::path& InspectionScratch::directory() const noexcept {
    return directory_;
}

std::filesystem::path InspectionScratch::archivePath() const {
    return directory_ / "source.archive";
}

std::uint64_t InspectionScratch::cleanupOrphans(
        const std::filesystem::path& scratch_root,
        std::chrono::seconds maximum_age) {
    if (scratch_root.empty() || maximum_age <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("Inspection scratch cleanup parameters are invalid");
    }
    std::error_code error;
    const std::filesystem::path root = std::filesystem::absolute(
            scratch_root, error).lexically_normal();
    if (error || !std::filesystem::exists(root, error)) return 0U;
    if (error || !std::filesystem::is_directory(root, error)) {
        throw std::runtime_error("Inspection scratch root is invalid");
    }
    const auto cutoff = std::filesystem::file_time_type::clock::now()
            - maximum_age;
    std::uint64_t removed = 0U;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        const std::string name = entry.path().filename().string();
        std::error_code entry_error;
        if (name.rfind(kScratchDirectoryPrefix, 0U) != 0U
            || entry.is_symlink(entry_error) || entry_error
            || !entry.is_directory(entry_error) || entry_error
            || entry.last_write_time(entry_error) >= cutoff || entry_error) {
            continue;
        }
        std::filesystem::remove_all(entry.path(), entry_error);
        if (!entry_error) ++removed;
    }
    if (error) throw std::runtime_error("Inspection scratch cleanup failed");
    return removed;
}

PackageInspectionExecutor::PackageInspectionExecutor(
        const client::ObjectTransfer& object_transfer,
        std::filesystem::path scratch_root)
    : object_transfer_(object_transfer), scratch_root_(std::move(scratch_root)) {
    if (scratch_root_.empty()) {
        throw std::invalid_argument("Inspection scratch root must not be empty");
    }
    static_cast<void>(InspectionScratch::cleanupOrphans(scratch_root_));
}

PackageEnumerationResult PackageInspectionExecutor::enumerate(
        InspectionLeaseSession& session, const PackageLimits& limits,
        const std::string& now_utc,
        const ContinuePredicate& should_continue) const {
    const auto continue_attempt = [&session, &should_continue]() {
        return session.active() && (!should_continue || should_continue());
    };
    const InspectionRequest& request = session.task().inspection_request;
    std::vector<SourceManifestPage> pages;
    pages.reserve(request.source_manifest.page_count);
    for (std::uint32_t page = 0U; page < request.source_manifest.page_count; ++page) {
        requireContinue(continue_attempt);
        pages.push_back(session.sourceManifestPage(page, now_utc));
    }
    validateSourceManifest(request, pages, now_utc);
    if (request.package_kind == PackageKind::directory) {
        DirectoryPackageEnumerator enumerator;
        return enumerator.enumerate(
                request, pages, limits,
                [this, &limits, &continue_attempt](const SourceObject& source) {
                    try {
                        const auto observed = object_transfer_.inspect(
                                source.read_grant.url,
                                limits.maximum_package_bytes,
                                continue_attempt);
                        return ObservedSourceObject{
                                observed.observed_size, observed.etag,
                                observed.sha256, observed.has_zip_magic};
                    } catch (const client::ObjectTransferCancelledError&) {
                        throw PackageEnumerationError(
                                PackageFailureKind::cancelled,
                                "Package inspection was cancelled");
                    }
                },
                now_utc, continue_attempt);
    }
    if (pages.size() != 1U || pages.front().records.size() != 1U) {
        invalid("Archive source manifest must contain exactly one object");
    }
    const SourceObject& source = pages.front().records.front();
    if (source.package_relative_path != kArchiveContainerPath) {
        invalid("Archive source manifest path is invalid");
    }
    InspectionScratch scratch(scratch_root_, request.inspection_id,
                              request.request_id);
    client::StreamedObjectMetadata observed;
    try {
        observed = object_transfer_.downloadToFile(
                source.read_grant.url, scratch.archivePath(),
                limits.maximum_package_bytes, continue_attempt);
    } catch (const client::ObjectTransferCancelledError&) {
        throw PackageEnumerationError(PackageFailureKind::cancelled,
                                      "Package inspection was cancelled");
    }
    if (!observed.has_zip_magic) invalid("Archive source magic is invalid");
    ZipPackageEnumerator enumerator;
    return enumerator.enumerate(
            request.package_kind, source,
            ObservedSourceObject{observed.observed_size, observed.etag,
                                 observed.sha256, observed.has_zip_magic},
            scratch.archivePath(), limits, continue_attempt);
}

}  // namespace clip_worker::inspection::package
