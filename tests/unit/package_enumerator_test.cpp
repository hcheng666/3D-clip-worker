#include "clip_worker/inspection/package/package_enumerator.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <zip.h>

namespace clip_worker::inspection::package {
namespace {

constexpr const char* kFixtureNow = "2026-08-17T08:00:00Z";
constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50U;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50U;
constexpr std::uint32_t kEocdSignature = 0x06054b50U;
constexpr std::size_t kLocalHeaderBytes = 30U;
constexpr std::size_t kCentralHeaderBytes = 46U;
constexpr std::size_t kEocdBytes = 22U;

std::filesystem::path sourcePath(const std::string& relative) {
#ifdef CLIP_WORKER_SOURCE_DIR
    return std::filesystem::path(CLIP_WORKER_SOURCE_DIR) / relative;
#else
    return std::filesystem::current_path() / relative;
#endif
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto identity = std::chrono::steady_clock::now()
                .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
                / ("clip-worker-package-test-" + std::to_string(identity));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read package fixture");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot write package fixture");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Cannot finalize package fixture");
}

std::uint16_t readLe16(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
    return static_cast<std::uint16_t>(bytes.at(offset))
            | static_cast<std::uint16_t>(bytes.at(offset + 1U)) << 8U;
}

std::uint32_t readLe32(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
    return static_cast<std::uint32_t>(bytes.at(offset))
            | static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 8U
            | static_cast<std::uint32_t>(bytes.at(offset + 2U)) << 16U
            | static_cast<std::uint32_t>(bytes.at(offset + 3U)) << 24U;
}

std::uint64_t readLe64(const std::uint8_t* bytes) {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

void writeLe16(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value & 0xffU);
    bytes.at(offset + 1U) = static_cast<std::uint8_t>(value >> 8U);
}

void writeLe32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes.at(offset + index) = static_cast<std::uint8_t>(
                value >> (index * 8U));
    }
}

void appendLe16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendLe32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void appendLe64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::size_t eocdOffset(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kEocdBytes) {
        throw std::runtime_error("ZIP fixture has no EOCD");
    }
    for (std::size_t offset = bytes.size() - kEocdBytes;; --offset) {
        if (readLe32(bytes, offset) == kEocdSignature) return offset;
        if (offset == 0U) break;
    }
    throw std::runtime_error("ZIP fixture has no EOCD");
}

std::size_t centralOffset(const std::vector<std::uint8_t>& bytes) {
    return readLe32(bytes, eocdOffset(bytes) + 16U);
}

void mutateFirstHeaders(
        const std::filesystem::path& path,
        const std::function<void(std::vector<std::uint8_t>&,
                                 std::size_t, std::size_t)>& mutation) {
    auto bytes = readBytes(path);
    const std::size_t central = centralOffset(bytes);
    if (readLe32(bytes, central) != kCentralHeaderSignature) {
        throw std::runtime_error("ZIP fixture central header is invalid");
    }
    const std::size_t local = readLe32(bytes, central + 42U);
    if (readLe32(bytes, local) != kLocalHeaderSignature) {
        throw std::runtime_error("ZIP fixture local header is invalid");
    }
    mutation(bytes, local, central);
    writeBytes(path, bytes);
}

std::string sha256(const std::vector<std::uint8_t>& bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0U;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
            EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context
        || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1
        || EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1
        || EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1) {
        throw std::runtime_error("Fixture SHA-256 failed");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(static_cast<std::size_t>(length) * 2U, '0');
    for (std::size_t index = 0U; index < length; ++index) {
        result[index * 2U] = kHex[digest[index] >> 4U];
        result[index * 2U + 1U] = kHex[digest[index] & 0x0fU];
    }
    return result;
}

std::array<std::uint8_t, 16U> md5(const std::string& value) {
    std::array<std::uint8_t, 16U> digest{};
    unsigned int length = 0U;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
            EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context
        || EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr) != 1
        || EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1
        || EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1
        || length != digest.size()) {
        throw std::runtime_error("Fixture MD5 failed");
    }
    return digest;
}

PackageLimits standardLimits() {
    return PackageLimits::load(
            sourcePath("config/resource-limit-profiles-v1.json"),
            "STANDARD_4CPU_8GIB_SINGLE_TASK_V1",
            "a65be19950a48f15c9a0275dda5a0b8b8e9e309feab84cbc10584dd70713c093");
}

struct FixtureEntry {
    std::string name;
    std::vector<std::uint8_t> bytes;
    zip_int32_t method = ZIP_CM_STORE;
};

void createZip(const std::filesystem::path& path,
               std::vector<FixtureEntry> entries,
               bool overwrite_names = true) {
    int error = 0;
    std::unique_ptr<zip_t, decltype(&zip_discard)> archive(
            zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error),
            &zip_discard);
    if (!archive) throw std::runtime_error("Cannot create ZIP fixture");
    for (auto& entry : entries) {
        zip_source_t* source = zip_source_buffer(
                archive.get(), entry.bytes.data(), entry.bytes.size(), 0);
        if (source == nullptr) throw std::runtime_error("Cannot create ZIP source");
        const zip_int64_t index = zip_file_add(
                archive.get(), entry.name.c_str(), source,
                ZIP_FL_ENC_UTF_8
                        | (overwrite_names ? ZIP_FL_OVERWRITE : 0U));
        if (index < 0) {
            zip_source_free(source);
            throw std::runtime_error("Cannot add ZIP fixture entry");
        }
        if (zip_set_file_compression(
                    archive.get(), static_cast<zip_uint64_t>(index),
                    entry.method, 0U) != 0) {
            throw std::runtime_error("Cannot configure ZIP fixture compression");
        }
    }
    zip_t* raw = archive.release();
    if (zip_close(raw) != 0) {
        zip_discard(raw);
        throw std::runtime_error("Cannot finalize ZIP fixture");
    }
}

struct FixtureCentralEntry {
    std::string name;
    std::uint64_t local_offset = 0U;
};

std::vector<FixtureCentralEntry> fixtureCentralEntries(
        const std::filesystem::path& path) {
    const auto bytes = readBytes(path);
    const std::size_t eocd = eocdOffset(bytes);
    const std::uint16_t count = readLe16(bytes, eocd + 10U);
    std::size_t cursor = centralOffset(bytes);
    std::vector<FixtureCentralEntry> entries;
    for (std::uint16_t index = 0U; index < count; ++index) {
        if (readLe32(bytes, cursor) != kCentralHeaderSignature) {
            throw std::runtime_error("ZIP fixture central header is invalid");
        }
        const std::uint16_t name_size = readLe16(bytes, cursor + 28U);
        const std::uint16_t extra_size = readLe16(bytes, cursor + 30U);
        const std::uint16_t comment_size = readLe16(bytes, cursor + 32U);
        entries.push_back({
                std::string(reinterpret_cast<const char*>(
                                    bytes.data() + cursor + kCentralHeaderBytes),
                            name_size),
                readLe32(bytes, cursor + 42U)});
        cursor += kCentralHeaderBytes + name_size + extra_size + comment_size;
    }
    return entries;
}

bool digestLess(const std::array<std::uint8_t, 16U>& left,
                const std::array<std::uint8_t, 16U>& right) {
    const std::uint64_t left_first = readLe64(left.data());
    const std::uint64_t right_first = readLe64(right.data());
    if (left_first != right_first) return left_first < right_first;
    return readLe64(left.data() + 8U) < readLe64(right.data() + 8U);
}

void replaceThreeTzIndex(const std::filesystem::path& path,
                         std::vector<std::uint8_t> index_bytes) {
    int error = 0;
    std::unique_ptr<zip_t, decltype(&zip_discard)> archive(
            zip_open(path.string().c_str(), 0, &error), &zip_discard);
    if (!archive) throw std::runtime_error("Cannot reopen 3TZ fixture");
    const zip_int64_t index = zip_name_locate(
            archive.get(), kThreeTzIndexPath, ZIP_FL_ENC_UTF_8);
    if (index < 0) throw std::runtime_error("Cannot locate 3TZ fixture index");
    zip_source_t* source = zip_source_buffer(
            archive.get(), index_bytes.data(), index_bytes.size(), 0);
    if (source == nullptr
        || zip_file_replace(archive.get(), static_cast<zip_uint64_t>(index),
                            source, ZIP_FL_ENC_UTF_8) != 0) {
        if (source != nullptr) zip_source_free(source);
        throw std::runtime_error("Cannot replace 3TZ fixture index");
    }
    if (zip_set_file_compression(
                archive.get(), static_cast<zip_uint64_t>(index),
                ZIP_CM_STORE, 0U) != 0) {
        throw std::runtime_error("Cannot store 3TZ fixture index");
    }
    zip_t* raw = archive.release();
    if (zip_close(raw) != 0) {
        zip_discard(raw);
        throw std::runtime_error("Cannot finalize 3TZ fixture index");
    }
}

void createIndexedThreeTz(
        const std::filesystem::path& path,
        std::vector<FixtureEntry> logical_entries,
        const std::function<void(std::vector<std::uint8_t>&)>& mutation = {}) {
    const std::size_t logical_count = logical_entries.size();
    logical_entries.push_back({kThreeTzIndexPath,
                               std::vector<std::uint8_t>(logical_count * 24U),
                               ZIP_CM_STORE});
    createZip(path, std::move(logical_entries));
    auto central_entries = fixtureCentralEntries(path);
    struct Record {
        std::array<std::uint8_t, 16U> digest{};
        std::uint64_t offset = 0U;
    };
    std::vector<Record> records;
    for (const auto& entry : central_entries) {
        if (entry.name != kThreeTzIndexPath) {
            records.push_back({md5(entry.name), entry.local_offset});
        }
    }
    std::sort(records.begin(), records.end(), [](const Record& left,
                                                  const Record& right) {
        return digestLess(left.digest, right.digest);
    });
    std::vector<std::uint8_t> index_bytes;
    for (const auto& record : records) {
        index_bytes.insert(index_bytes.end(), record.digest.begin(),
                           record.digest.end());
        appendLe64(index_bytes, record.offset);
    }
    if (mutation) mutation(index_bytes);
    replaceThreeTzIndex(path, std::move(index_bytes));
}

void createMinimalZip64(const std::filesystem::path& path) {
    constexpr std::uint32_t kCrc32OfX = 0x8cdc1683U;
    constexpr std::uint64_t kOneEntry = 1U;
    constexpr std::uint16_t kZip64Version = 45U;
    constexpr std::uint32_t kZip32Sentinel = 0xffffffffU;
    constexpr std::uint16_t kZip16Sentinel = 0xffffU;
    const std::string name = "a.bin";
    std::vector<std::uint8_t> bytes;
    appendLe32(bytes, kLocalHeaderSignature);
    appendLe16(bytes, kZip64Version);
    appendLe16(bytes, 0U);
    appendLe16(bytes, kZipMethodStore);
    appendLe16(bytes, 0U);
    appendLe16(bytes, 0U);
    appendLe32(bytes, kCrc32OfX);
    appendLe32(bytes, kZip32Sentinel);
    appendLe32(bytes, kZip32Sentinel);
    appendLe16(bytes, static_cast<std::uint16_t>(name.size()));
    appendLe16(bytes, 20U);
    bytes.insert(bytes.end(), name.begin(), name.end());
    appendLe16(bytes, 1U);
    appendLe16(bytes, 16U);
    appendLe64(bytes, 1U);
    appendLe64(bytes, 1U);
    bytes.push_back('x');
    const std::uint64_t central_offset = bytes.size();

    appendLe32(bytes, kCentralHeaderSignature);
    appendLe16(bytes, kZip64Version);
    appendLe16(bytes, kZip64Version);
    appendLe16(bytes, 0U);
    appendLe16(bytes, kZipMethodStore);
    appendLe16(bytes, 0U);
    appendLe16(bytes, 0U);
    appendLe32(bytes, kCrc32OfX);
    appendLe32(bytes, kZip32Sentinel);
    appendLe32(bytes, kZip32Sentinel);
    appendLe16(bytes, static_cast<std::uint16_t>(name.size()));
    appendLe16(bytes, 28U);
    appendLe16(bytes, 0U);
    appendLe16(bytes, 0U);
    appendLe16(bytes, 0U);
    appendLe32(bytes, 0U);
    appendLe32(bytes, kZip32Sentinel);
    bytes.insert(bytes.end(), name.begin(), name.end());
    appendLe16(bytes, 1U);
    appendLe16(bytes, 24U);
    appendLe64(bytes, 1U);
    appendLe64(bytes, 1U);
    appendLe64(bytes, 0U);
    const std::uint64_t central_size = bytes.size() - central_offset;
    const std::uint64_t zip64_eocd_offset = bytes.size();

    appendLe32(bytes, 0x06064b50U);
    appendLe64(bytes, 44U);
    appendLe16(bytes, kZip64Version);
    appendLe16(bytes, kZip64Version);
    appendLe32(bytes, 0U);
    appendLe32(bytes, 0U);
    appendLe64(bytes, kOneEntry);
    appendLe64(bytes, kOneEntry);
    appendLe64(bytes, central_size);
    appendLe64(bytes, central_offset);
    appendLe32(bytes, 0x07064b50U);
    appendLe32(bytes, 0U);
    appendLe64(bytes, zip64_eocd_offset);
    appendLe32(bytes, 1U);
    appendLe32(bytes, kEocdSignature);
    appendLe16(bytes, 0U);
    appendLe16(bytes, 0U);
    appendLe16(bytes, kZip16Sentinel);
    appendLe16(bytes, kZip16Sentinel);
    appendLe32(bytes, kZip32Sentinel);
    appendLe32(bytes, kZip32Sentinel);
    appendLe16(bytes, 0U);
    writeBytes(path, bytes);
}

SourceObject archiveSource(const std::filesystem::path& path) {
    const auto bytes = readBytes(path);
    SourceObject source;
    source.object_id = "archive-object-1";
    source.package_relative_path = kArchiveContainerPath;
    source.size_bytes = bytes.size();
    source.etag = "archive-etag";
    source.sha256 = sha256(bytes);
    return source;
}

ObservedSourceObject observedArchive(const std::filesystem::path& path) {
    const auto bytes = readBytes(path);
    return {bytes.size(), "archive-etag", sha256(bytes), true};
}

nlohmann::json positiveFixture() {
    std::ifstream input(sourcePath(
            "tests/fixtures/protocol/inspector-v1/positive-contract.json"));
    if (!input) throw std::runtime_error("Cannot open protocol fixture");
    return nlohmann::json::parse(input);
}

std::pair<InspectionRequest, std::vector<SourceManifestPage>> directoryManifest() {
    const auto fixture = positiveFixture();
    InspectionRequest request = parseInspectionRequest(
            fixture.at("inspectionRequest").dump());
    SourceManifestPage page = parseSourceManifestPage(
            fixture.at("sourceManifestPage").dump());
    return {std::move(request), {std::move(page)}};
}

std::pair<InspectionRequest, std::vector<SourceManifestPage>>
twoPageDirectoryManifest() {
    auto [request, pages] = directoryManifest();
    SourceManifestPage second = pages.front();
    second.page_number = 1U;
    second.records.front().object_id = "object-2";
    second.records.front().package_relative_path = "content/tile.b3dm";
    second.records.front().etag = "etag-source-2";
    second.records.front().sha256 = std::string(64U, 'b');
    second.page_sha256 = canonicalPageSha256(second.records);
    request.source_manifest.object_count = 2U;
    request.source_manifest.page_count = 2U;
    request.source_manifest.page_size = 1U;
    pages.front().page_sha256 = canonicalPageSha256(pages.front().records);
    pages.push_back(std::move(second));
    std::ostringstream canonical_manifest;
    for (const auto& page : pages) {
        canonical_manifest << page.page_number << ':' << page.page_sha256
                           << ':' << page.record_count << '\n';
    }
    const std::string canonical = canonical_manifest.str();
    request.source_manifest.manifest_sha256 = sha256(
            std::vector<std::uint8_t>(canonical.begin(), canonical.end()));
    return {std::move(request), std::move(pages)};
}

std::vector<std::uint8_t> threeTzIndexForFirstEntry(const std::string& name) {
    std::vector<std::uint8_t> bytes(24U, 0U);
    const auto digest = md5(name);
    std::copy(digest.begin(), digest.end(), bytes.begin());
    // libzip writes the first local header at offset zero.
    return bytes;
}

TEST(PackageLimitsTest, LoadsOnlyTheHashClosedNamedProfile) {
    const auto limits = standardLimits();
    EXPECT_EQ(limits.maximum_package_bytes, 2147483648ULL);
    EXPECT_EQ(limits.maximum_expanded_bytes, 8589934592ULL);
    EXPECT_EQ(limits.maximum_entry_count, 100000U);
    EXPECT_THROW(static_cast<void>(PackageLimits::load(
                         sourcePath("config/resource-limit-profiles-v1.json"),
                         limits.profile_id, std::string(64U, '0'))),
                 std::invalid_argument);
}

TEST(PackagePathAnalyzerTest, PreservesUnsafeSpellingAndEmitsExplicitFlags) {
    const auto analysis = PackagePathAnalyzer{}.analyze(
            "C:\\..\\%2e%2e/tileset.json", 1024U);
    EXPECT_EQ(analysis.normalized_candidate,
              "C:\\..\\%2e%2e/tileset.json");
    EXPECT_TRUE(hasFlag(analysis.flags, PathAnalysisFlag::drive_prefix));
    EXPECT_TRUE(hasFlag(analysis.flags, PathAnalysisFlag::absolute));
    EXPECT_TRUE(hasFlag(analysis.flags, PathAnalysisFlag::backslash));
    EXPECT_TRUE(hasFlag(analysis.flags, PathAnalysisFlag::percent_encoded_alias));
    EXPECT_THROW(static_cast<void>(PackagePathAnalyzer{}.analyze(
                         "\xE4\xB8\xAD.json", 4U)),
                 PackageEnumerationError);
}

TEST(DirectoryPackageEnumeratorTest, ValidatesIdentityAndReturnsStableEvidence) {
    auto [request, pages] = directoryManifest();
    const auto limits = standardLimits();
    DirectoryPackageEnumerator enumerator;
    const auto result = enumerator.enumerate(
            request, pages, limits,
            [](const SourceObject& source) {
                return ObservedSourceObject{
                        source.size_bytes, source.etag, *source.sha256, false};
            },
            kFixtureNow);
    ASSERT_EQ(result.entries.size(), 1U);
    EXPECT_EQ(result.entries.front().object_id, "object-1");
    EXPECT_EQ(result.entries.front().observed_expanded_bytes, 128U);
    EXPECT_EQ(result.package_bytes, 128U);

    EXPECT_THROW(static_cast<void>(enumerator.enumerate(
                         request, pages, limits,
                         [](const SourceObject& source) {
                             return ObservedSourceObject{
                                     source.size_bytes + 1U, source.etag,
                                     *source.sha256, false};
                         },
                         kFixtureNow)),
                 PackageEnumerationError);
}

TEST(DirectoryPackageEnumeratorTest, EnforcesEntryLimitAndCancellation) {
    auto [request, pages] = directoryManifest();
    auto limits = standardLimits();
    limits.maximum_entry_bytes = 64U;
    DirectoryPackageEnumerator enumerator;
    EXPECT_THROW(static_cast<void>(enumerator.enumerate(
                         request, pages, limits,
                         [](const SourceObject& source) {
                             return ObservedSourceObject{
                                     source.size_bytes, source.etag,
                                     *source.sha256, false};
                         },
                         kFixtureNow)),
                 PackageEnumerationError);

    bool reader_called = false;
    EXPECT_THROW(static_cast<void>(enumerator.enumerate(
                         request, pages, standardLimits(),
                         [&reader_called](const SourceObject&) {
                             reader_called = true;
                             return ObservedSourceObject{};
                         },
                         kFixtureNow, [] { return false; })),
                 PackageEnumerationError);
    EXPECT_FALSE(reader_called);
}

TEST(DirectoryPackageEnumeratorTest, ReplaysMultiplePagesStablyAndFailsBeforeIo) {
    auto [request, pages] = twoPageDirectoryManifest();
    DirectoryPackageEnumerator enumerator;
    const auto reader = [](const SourceObject& source) {
        return ObservedSourceObject{source.size_bytes, source.etag,
                                    *source.sha256, false};
    };
    const auto first = enumerator.enumerate(
            request, pages, standardLimits(), reader, kFixtureNow);
    const auto replay = enumerator.enumerate(
            request, pages, standardLimits(), reader, kFixtureNow);
    ASSERT_EQ(first.entries.size(), 2U);
    ASSERT_EQ(replay.entries.size(), first.entries.size());
    for (std::size_t index = 0U; index < first.entries.size(); ++index) {
        EXPECT_EQ(replay.entries[index].object_id, first.entries[index].object_id);
        EXPECT_EQ(replay.entries[index].observed_sha256,
                  first.entries[index].observed_sha256);
    }

    auto one_entry_limit = standardLimits();
    one_entry_limit.maximum_entry_count = 1U;
    std::uint64_t read_count = 0U;
    EXPECT_THROW(static_cast<void>(enumerator.enumerate(
                         request, pages, one_entry_limit,
                         [&read_count](const SourceObject&) {
                             ++read_count;
                             return ObservedSourceObject{};
                         },
                         kFixtureNow)),
                 PackageEnumerationError);
    EXPECT_EQ(read_count, 0U);
}

TEST(ZipPackageEnumeratorTest, StreamsStoreAndDeflateEntriesDeterministically) {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "ordinary.zip";
    createZip(path, {{"z.bin", {1U, 2U, 3U}, ZIP_CM_STORE},
                     {"a.json", {'{', '}'}, ZIP_CM_DEFLATE}});
    ZipPackageEnumerator enumerator;
    const auto result = enumerator.enumerate(
            PackageKind::zip, archiveSource(path), observedArchive(path),
            path, standardLimits());
    ASSERT_EQ(result.entries.size(), 3U);
    EXPECT_EQ(result.entries[0].path.normalized_candidate, kArchiveContainerPath);
    EXPECT_EQ(result.entries[1].path.normalized_candidate, "a.json");
    EXPECT_EQ(result.entries[2].path.normalized_candidate, "z.bin");
    EXPECT_EQ(result.archive_record_count, 2U);
    EXPECT_EQ(result.expanded_bytes, 5U);
}

TEST(ZipPackageEnumeratorTest, ReservesOneEntryForTheArchiveContainer) {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "container-reservation.zip";
    createZip(path, {{"only.bin", {}, ZIP_CM_STORE}});
    auto limits = standardLimits();
    limits.maximum_entry_count = 1U;
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::zip, archiveSource(path),
                         observedArchive(path), path, limits)),
                 PackageEnumerationError);
}

TEST(ZipPackageEnumeratorTest, RejectsRatioBombAndNestedArchiveByStableKind) {
    TemporaryDirectory temporary;
    const auto bomb_path = temporary.path() / "bomb.zip";
    createZip(bomb_path, {{"bomb.bin", std::vector<std::uint8_t>(65536U, 0U),
                           ZIP_CM_DEFLATE}});
    auto strict_ratio = standardLimits();
    strict_ratio.maximum_compression_ratio = 2U;
    try {
        static_cast<void>(ZipPackageEnumerator{}.enumerate(
                PackageKind::zip, archiveSource(bomb_path),
                observedArchive(bomb_path), bomb_path, strict_ratio));
        FAIL() << "Expected a compression-ratio limit";
    } catch (const PackageEnumerationError& error) {
        EXPECT_EQ(error.kind(), PackageFailureKind::limit_exceeded);
        EXPECT_EQ(error.diagnosticCode(), DiagnosticCode::package_limit_exceeded);
    }

    const auto nested_path = temporary.path() / "nested.zip";
    createZip(nested_path, {{"nested.zip", {0x50U, 0x4bU, 0x03U, 0x04U},
                             ZIP_CM_STORE}});
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::zip, archiveSource(nested_path),
                         observedArchive(nested_path), nested_path,
                         standardLimits())),
                 PackageEnumerationError);
}

TEST(ZipPackageEnumeratorTest, RejectsLocalAndCentralNameMismatch) {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "mismatched-name.zip";
    createZip(path, {{"a.json", {'{', '}'}, ZIP_CM_STORE}});
    {
        std::fstream archive(path, std::ios::binary | std::ios::in
                                      | std::ios::out);
        ASSERT_TRUE(archive.good());
        constexpr std::streamoff kFirstLocalNameOffset = 30;
        archive.seekp(kFirstLocalNameOffset);
        archive.put('b');
    }
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::zip, archiveSource(path),
                         observedArchive(path), path, standardLimits())),
                 PackageEnumerationError);
}

TEST(ZipPackageEnumeratorTest, AcceptsEmptyStoreEntryAndMinimalZip64) {
    TemporaryDirectory temporary;
    const auto empty_path = temporary.path() / "empty.zip";
    createZip(empty_path, {{"empty.bin", {}, ZIP_CM_STORE}});
    const auto empty_result = ZipPackageEnumerator{}.enumerate(
            PackageKind::zip, archiveSource(empty_path),
            observedArchive(empty_path), empty_path, standardLimits());
    EXPECT_EQ(empty_result.expanded_bytes, 0U);
    ASSERT_EQ(empty_result.entries.size(), 2U);

    const auto zip64_path = temporary.path() / "minimal-zip64.zip";
    createMinimalZip64(zip64_path);
    const auto zip64_result = ZipPackageEnumerator{}.enumerate(
            PackageKind::zip, archiveSource(zip64_path),
            observedArchive(zip64_path), zip64_path, standardLimits());
    EXPECT_EQ(zip64_result.expanded_bytes, 1U);
    ASSERT_EQ(zip64_result.entries.size(), 2U);
    EXPECT_EQ(zip64_result.entries.back().path.normalized_candidate, "a.bin");
}

TEST(ZipPackageEnumeratorTest, RejectsCorruptEncryptedAndUnknownMethodArchives) {
    TemporaryDirectory temporary;
    const auto corrupt = temporary.path() / "corrupt-eocd.zip";
    createZip(corrupt, {{"a.bin", {'x'}, ZIP_CM_STORE}});
    auto corrupt_bytes = readBytes(corrupt);
    writeLe32(corrupt_bytes, eocdOffset(corrupt_bytes), 0U);
    writeBytes(corrupt, corrupt_bytes);
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::zip, archiveSource(corrupt),
                         observedArchive(corrupt), corrupt, standardLimits())),
                 PackageEnumerationError);

    const auto encrypted = temporary.path() / "encrypted.zip";
    createZip(encrypted, {{"a.bin", {'x'}, ZIP_CM_STORE}});
    mutateFirstHeaders(encrypted, [](std::vector<std::uint8_t>& bytes,
                                     std::size_t local,
                                     std::size_t central) {
        constexpr std::uint16_t kEncryptedFlag = 1U;
        writeLe16(bytes, local + 6U,
                  readLe16(bytes, local + 6U) | kEncryptedFlag);
        writeLe16(bytes, central + 8U,
                  readLe16(bytes, central + 8U) | kEncryptedFlag);
    });
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::zip, archiveSource(encrypted),
                         observedArchive(encrypted), encrypted,
                         standardLimits())),
                 PackageEnumerationError);

    const auto unsupported = temporary.path() / "unsupported-method.zip";
    createZip(unsupported, {{"a.bin", {'x'}, ZIP_CM_STORE}});
    mutateFirstHeaders(unsupported, [](std::vector<std::uint8_t>& bytes,
                                       std::size_t local,
                                       std::size_t central) {
        constexpr std::uint16_t kUnsupportedMethod = 12U;
        writeLe16(bytes, local + 8U, kUnsupportedMethod);
        writeLe16(bytes, central + 10U, kUnsupportedMethod);
    });
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::zip, archiveSource(unsupported),
                         observedArchive(unsupported), unsupported,
                         standardLimits())),
                 PackageEnumerationError);
}

TEST(ZipPackageEnumeratorTest, RejectsDeclaredSizeAndConfiguredPathDrift) {
    TemporaryDirectory temporary;
    const auto size_drift = temporary.path() / "size-drift.zip";
    createZip(size_drift, {{"a.bin", {'x'}, ZIP_CM_STORE}});
    mutateFirstHeaders(size_drift, [](std::vector<std::uint8_t>& bytes,
                                      std::size_t local,
                                      std::size_t central) {
        writeLe32(bytes, local + 22U, 2U);
        writeLe32(bytes, central + 24U, 2U);
    });
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::zip, archiveSource(size_drift),
                         observedArchive(size_drift), size_drift,
                         standardLimits())),
                 PackageEnumerationError);

    const auto long_path = temporary.path() / "long-path.zip";
    createZip(long_path, {{"long-name.bin", {'x'}, ZIP_CM_STORE}});
    auto limits = standardLimits();
    limits.maximum_path_utf8_bytes = 4U;
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::zip, archiveSource(long_path),
                         observedArchive(long_path), long_path, limits)),
                 PackageEnumerationError);
}

TEST(ThreeTzPackageEnumeratorTest, ValidatesIndexCoverageAndZstandardMethod93) {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "valid.3tz";
    createZip(path, {{"tileset.json", {'{', '}'}, kZipMethodZstandard},
                     {kThreeTzIndexPath,
                      threeTzIndexForFirstEntry("tileset.json"), ZIP_CM_STORE}});
    const auto result = ZipPackageEnumerator{}.enumerate(
            PackageKind::three_tz, archiveSource(path), observedArchive(path),
            path, standardLimits());
    ASSERT_EQ(result.entries.size(), 2U);
    EXPECT_EQ(result.entries.back().compression_method,
              PackageCompressionMethod::zstandard);
    EXPECT_EQ(result.archive_record_count, 2U);
}

TEST(ThreeTzPackageEnumeratorTest, RejectsMissingAndNonLastIndexes) {
    TemporaryDirectory temporary;
    const auto missing = temporary.path() / "missing.3tz";
    createZip(missing, {{"tileset.json", {'{', '}'}, ZIP_CM_STORE}});
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::three_tz, archiveSource(missing),
                         observedArchive(missing), missing, standardLimits())),
                 PackageEnumerationError);

    const auto non_last = temporary.path() / "non-last.3tz";
    createZip(non_last,
              {{kThreeTzIndexPath,
                threeTzIndexForFirstEntry("tileset.json"), ZIP_CM_STORE},
               {"tileset.json", {'{', '}'}, ZIP_CM_STORE}});
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::three_tz, archiveSource(non_last),
                         observedArchive(non_last), non_last, standardLimits())),
                 PackageEnumerationError);
}

TEST(ThreeTzPackageEnumeratorTest, RejectsMalformedIndexEvidence) {
    TemporaryDirectory temporary;
    const auto unaligned = temporary.path() / "unaligned.3tz";
    createZip(unaligned,
              {{"tileset.json", {'{', '}'}, ZIP_CM_STORE},
               {kThreeTzIndexPath, std::vector<std::uint8_t>(23U),
                ZIP_CM_STORE}});
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::three_tz, archiveSource(unaligned),
                         observedArchive(unaligned), unaligned,
                         standardLimits())),
                 PackageEnumerationError);

    const auto bad_hash = temporary.path() / "bad-hash.3tz";
    createIndexedThreeTz(
            bad_hash,
            {{"tileset.json", {'{', '}'}, ZIP_CM_STORE}},
            [](std::vector<std::uint8_t>& index) { index.front() ^= 0xffU; });
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::three_tz, archiveSource(bad_hash),
                         observedArchive(bad_hash), bad_hash,
                         standardLimits())),
                 PackageEnumerationError);

    const auto bad_offset = temporary.path() / "bad-offset.3tz";
    createIndexedThreeTz(
            bad_offset,
            {{"tileset.json", {'{', '}'}, ZIP_CM_STORE}},
            [](std::vector<std::uint8_t>& index) {
                std::fill(index.end() - 8, index.end(), 0xffU);
            });
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::three_tz, archiveSource(bad_offset),
                         observedArchive(bad_offset), bad_offset,
                         standardLimits())),
                 PackageEnumerationError);

    const auto unsorted = temporary.path() / "unsorted.3tz";
    createIndexedThreeTz(
            unsorted,
            {{"a.bin", {'a'}, ZIP_CM_STORE},
             {"b.bin", {'b'}, ZIP_CM_STORE}},
            [](std::vector<std::uint8_t>& index) {
                constexpr std::size_t kRecordBytes = 24U;
                std::rotate(index.begin(), index.begin() + kRecordBytes,
                            index.end());
            });
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::three_tz, archiveSource(unsorted),
                         observedArchive(unsorted), unsorted,
                         standardLimits())),
                 PackageEnumerationError);
}

TEST(ThreeTzPackageEnumeratorTest, RejectsDuplicateReservedIndexName) {
    TemporaryDirectory temporary;
    const auto duplicate = temporary.path() / "duplicate-index.3tz";
    const std::string placeholder(std::string(kThreeTzIndexPath).size(), 'x');
    createZip(duplicate,
              {{placeholder, std::vector<std::uint8_t>(24U), ZIP_CM_STORE},
               {kThreeTzIndexPath, std::vector<std::uint8_t>(24U),
                ZIP_CM_STORE}});
    mutateFirstHeaders(duplicate,
                       [](std::vector<std::uint8_t>& bytes,
                          std::size_t local, std::size_t central) {
        const std::string reserved = kThreeTzIndexPath;
        std::copy(reserved.begin(), reserved.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(
                          local + kLocalHeaderBytes));
        std::copy(reserved.begin(), reserved.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(
                          central + kCentralHeaderBytes));
    });
    EXPECT_THROW(static_cast<void>(ZipPackageEnumerator{}.enumerate(
                         PackageKind::three_tz, archiveSource(duplicate),
                         observedArchive(duplicate), duplicate,
                         standardLimits())),
                 PackageEnumerationError);
}

TEST(ThreeTzPackageEnumeratorTest, AcceptsPinnedCesiumValidatorCorpusPackage) {
    const auto path = sourcePath(
            "tests/corpus/downloaded/validator-valid-3tz/payload");
    if (!std::filesystem::is_regular_file(path)) {
        GTEST_SKIP() << "Pinned official 3TZ corpus entry is not materialized";
    }
    const auto result = ZipPackageEnumerator{}.enumerate(
            PackageKind::three_tz, archiveSource(path), observedArchive(path),
            path, standardLimits());
    EXPECT_GT(result.archive_record_count, 1U);
    EXPECT_FALSE(result.entries.empty());
}

TEST(InspectionScratchTest, CleansRequestDirectoryOnEveryExit) {
    TemporaryDirectory temporary;
    std::filesystem::path request_directory;
    {
        InspectionScratch scratch(temporary.path(), "inspection-1", "request-1");
        request_directory = scratch.directory();
        EXPECT_TRUE(std::filesystem::exists(request_directory));
        std::ofstream(scratch.archivePath()) << "fixture";
    }
    EXPECT_FALSE(std::filesystem::exists(request_directory));
}

TEST(InspectionScratchTest, StartupCleanupRemovesOnlyExpiredOwnedDirectories) {
    TemporaryDirectory temporary;
    const auto expired = temporary.path() / "inspection-expired";
    const auto current = temporary.path() / "inspection-current";
    const auto foreign = temporary.path() / "foreign-expired";
    std::filesystem::create_directory(expired);
    std::filesystem::create_directory(current);
    std::filesystem::create_directory(foreign);
    const auto old_time = std::filesystem::file_time_type::clock::now()
            - std::chrono::hours(2);
    std::filesystem::last_write_time(expired, old_time);
    std::filesystem::last_write_time(foreign, old_time);

    EXPECT_EQ(InspectionScratch::cleanupOrphans(
                      temporary.path(), std::chrono::hours(1)),
              1U);
    EXPECT_FALSE(std::filesystem::exists(expired));
    EXPECT_TRUE(std::filesystem::exists(current));
    EXPECT_TRUE(std::filesystem::exists(foreign));
}

}  // namespace
}  // namespace clip_worker::inspection::package
