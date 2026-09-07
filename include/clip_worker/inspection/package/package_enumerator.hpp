#pragma once

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/inspection/inspection_contract.hpp"
#include "clip_worker/inspection/inspection_runtime.hpp"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace clip_worker::inspection::package {

inline constexpr const char* kArchiveContainerPath =
        "__inspection_package__/source.archive";
inline constexpr const char* kThreeTzIndexPath = "@3dtilesIndex1@";
inline constexpr std::uint16_t kZipMethodStore = 0U;
inline constexpr std::uint16_t kZipMethodDeflate = 8U;
inline constexpr std::uint16_t kZipMethodZstandard = 93U;

enum class PackageFailureKind { invalid, limit_exceeded, cancelled };

class PackageEnumerationError final : public std::runtime_error {
public:
    PackageEnumerationError(PackageFailureKind kind, std::string message);
    [[nodiscard]] PackageFailureKind kind() const noexcept;
    [[nodiscard]] DiagnosticCode diagnosticCode() const noexcept;

private:
    PackageFailureKind kind_;
};

struct PackageLimits {
    std::string profile_id;
    std::string document_sha256;
    std::uint64_t maximum_package_bytes = 0U;
    std::uint64_t maximum_expanded_bytes = 0U;
    std::uint64_t maximum_entry_bytes = 0U;
    std::uint64_t maximum_entry_count = 0U;
    std::uint64_t maximum_resource_count = 0U;
    std::uint64_t maximum_path_utf8_bytes = 0U;
    std::uint32_t maximum_nesting_depth = 0U;
    std::uint64_t maximum_compression_ratio = 0U;

    [[nodiscard]] static PackageLimits load(
            const std::filesystem::path& document, const std::string& profile_id,
            const std::string& expected_document_sha256);
};

enum class PathAnalysisFlag : std::uint32_t {
    none = 0U,
    absolute = 1U << 0U,
    drive_prefix = 1U << 1U,
    backslash = 1U << 2U,
    dot_segment = 1U << 3U,
    percent_encoded_alias = 1U << 4U,
    unsupported_scheme = 1U << 5U,
    reserved_namespace = 1U << 6U,
    symlink_like = 1U << 7U,
    empty_segment = 1U << 8U,
    non_nfc = 1U << 9U
};

[[nodiscard]] PathAnalysisFlag operator|(PathAnalysisFlag left,
                                         PathAnalysisFlag right) noexcept;
PathAnalysisFlag& operator|=(PathAnalysisFlag& left,
                            PathAnalysisFlag right) noexcept;
[[nodiscard]] bool hasFlag(PathAnalysisFlag value,
                           PathAnalysisFlag flag) noexcept;

struct PackagePathAnalysis {
    std::string raw_name;
    std::string normalized_candidate;
    PathAnalysisFlag flags = PathAnalysisFlag::none;
};

class PackagePathAnalyzer final {
public:
    [[nodiscard]] PackagePathAnalysis analyze(
            const std::string& raw_name,
            std::uint64_t maximum_path_utf8_bytes) const;
};

enum class PackageEntryKind {
    archive_container,
    file,
    directory_marker,
    three_tz_index
};

enum class PackageCompressionMethod { none, store, deflate, zstandard, unknown };

struct ObservedSourceObject {
    std::uint64_t observed_size = 0U;
    std::string observed_etag;
    std::string observed_sha256;
    bool has_zip_magic = false;
};

struct PackageEntryEvidence {
    std::string object_id;
    std::optional<std::string> archive_parent_id;
    PackagePathAnalysis path;
    PackageEntryKind entry_kind = PackageEntryKind::file;
    PackageCompressionMethod compression_method =
            PackageCompressionMethod::none;
    std::uint32_t archive_depth = 0U;
    std::uint64_t compressed_bytes = 0U;
    std::uint64_t declared_expanded_bytes = 0U;
    std::uint64_t observed_expanded_bytes = 0U;
    std::string source_etag;
    std::string observed_sha256;
    std::uint8_t zip_creator_system = 0U;
    std::uint32_t zip_external_attributes = 0U;
    std::uint64_t local_header_offset = 0U;
    std::uint64_t stable_ordinal = 0U;
};

struct PackageEnumerationResult {
    PackageKind package_kind = PackageKind::directory;
    std::uint64_t package_bytes = 0U;
    std::uint64_t expanded_bytes = 0U;
    std::uint64_t archive_record_count = 0U;
    std::vector<PackageEntryEvidence> entries;
};

/** Validates aliases and collisions across the complete immutable namespace. */
class PackageNamespaceValidator final {
public:
    void validate(const std::vector<PackageEntryEvidence>& entries) const;
};

using ContinuePredicate = std::function<bool()>;
using DirectoryObjectReader =
        std::function<ObservedSourceObject(const SourceObject&)>;

class DirectoryPackageEnumerator final {
public:
    [[nodiscard]] PackageEnumerationResult enumerate(
            const InspectionRequest& request,
            const std::vector<SourceManifestPage>& pages,
            const PackageLimits& limits,
            const DirectoryObjectReader& object_reader,
            const std::string& now_utc,
            const ContinuePredicate& should_continue = {}) const;
};

class ZipPackageEnumerator final {
public:
    [[nodiscard]] PackageEnumerationResult enumerate(
            PackageKind package_kind, const SourceObject& source,
            const ObservedSourceObject& observed_archive,
            const std::filesystem::path& archive_path,
            const PackageLimits& limits,
            const ContinuePredicate& should_continue = {}) const;
};

/** Request-scoped scratch directory with unconditional RAII cleanup. */
class InspectionScratch final {
public:
    static constexpr std::chrono::seconds kDefaultOrphanMaximumAge{
            24 * 60 * 60};

    InspectionScratch(const std::filesystem::path& scratch_root,
                      const std::string& inspection_id,
                      const std::string& request_id);
    ~InspectionScratch();

    InspectionScratch(const InspectionScratch&) = delete;
    InspectionScratch& operator=(const InspectionScratch&) = delete;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept;
    [[nodiscard]] std::filesystem::path archivePath() const;

    /** Removes only expired, non-symlink request directories created by this type. */
    [[nodiscard]] static std::uint64_t cleanupOrphans(
            const std::filesystem::path& scratch_root,
            std::chrono::seconds maximum_age = kDefaultOrphanMaximumAge);

private:
    std::filesystem::path directory_;
};

/**
 * Real Task 3.3 executor seam. It validates every source-manifest page before
 * dispatching to the bounded directory or archive enumerator. Production claim
 * remains disabled until the later inspection stages are complete.
 */
class PackageInspectionExecutor final {
public:
    PackageInspectionExecutor(const client::ObjectTransfer& object_transfer,
                              std::filesystem::path scratch_root);

    [[nodiscard]] PackageEnumerationResult enumerate(
            InspectionLeaseSession& session, const PackageLimits& limits,
            const std::string& now_utc,
            const ContinuePredicate& should_continue = {}) const;

private:
    const client::ObjectTransfer& object_transfer_;
    std::filesystem::path scratch_root_;
};

}  // namespace clip_worker::inspection::package
