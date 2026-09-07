#include "clip_worker/inspection/pipeline/expansion_uploader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zip.h>

namespace clip_worker::inspection::pipeline {
namespace {

struct ZipArchiveDeleter {
    void operator()(zip_t* archive) const noexcept {
        if (archive != nullptr) zip_discard(archive);
    }
};

struct ZipFileDeleter {
    void operator()(zip_file_t* file) const noexcept {
        if (file != nullptr) static_cast<void>(zip_fclose(file));
    }
};

struct ArchiveStreamState {
    std::unique_ptr<zip_t, ZipArchiveDeleter> archive;
    std::unique_ptr<zip_file_t, ZipFileDeleter> file;
    std::uint64_t remaining = 0U;
};

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

int base64Value(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

class DataUriReader final {
public:
    DataUriReader(std::string uri, std::uint64_t maximum_decoded_bytes)
        : uri_(std::move(uri)) {
        if (uri_.size() < 5U
            || !std::equal(uri_.begin(), uri_.begin() + 5U, "data:",
                    [](char left, char right) {
                        return std::tolower(static_cast<unsigned char>(left))
                                == std::tolower(static_cast<unsigned char>(right));
                    })) {
            throw std::invalid_argument("Inline upload source is not a data URI");
        }
        const std::size_t comma = uri_.find(',');
        if (comma == std::string::npos || comma > 1024U) {
            throw std::invalid_argument("Inline upload data URI metadata is invalid");
        }
        std::string metadata = uri_.substr(5U, comma - 5U);
        if (std::any_of(metadata.begin(), metadata.end(), [](unsigned char value) {
                return value < 0x20U || value > 0x7eU;
            })) {
            throw std::invalid_argument("Inline upload data URI metadata is invalid");
        }
        constexpr const char* kMarker = ";base64";
        if (metadata.size() >= std::strlen(kMarker)
            && metadata.compare(metadata.size() - std::strlen(kMarker),
                                std::strlen(kMarker), kMarker) == 0) {
            base64_ = true;
            metadata.erase(metadata.size() - std::strlen(kMarker));
        }
        const std::size_t parameter = metadata.find(';');
        const std::string media_type = parameter == std::string::npos
                ? metadata : metadata.substr(0U, parameter);
        if (!media_type.empty() && media_type.find('/') == std::string::npos) {
            throw std::invalid_argument("Inline upload data URI media type is invalid");
        }
        payload_offset_ = comma + 1U;
        validate(maximum_decoded_bytes);
    }

    std::size_t read(std::uint8_t* output, std::size_t capacity) {
        std::size_t written = 0U;
        while (written < capacity && pending_offset_ < pending_size_) {
            output[written++] = pending_[pending_offset_++];
        }
        if (pending_offset_ == pending_size_) {
            pending_offset_ = 0U;
            pending_size_ = 0U;
        }
        while (written < capacity && cursor_ < uri_.size()) {
            if (base64_) {
                decodeBase64Group();
            } else {
                decodePercentByte();
            }
            while (written < capacity && pending_offset_ < pending_size_) {
                output[written++] = pending_[pending_offset_++];
            }
            if (pending_offset_ == pending_size_) {
                pending_offset_ = 0U;
                pending_size_ = 0U;
            }
        }
        return written;
    }

    [[nodiscard]] std::uint64_t decodedSize() const noexcept {
        return decoded_size_;
    }

private:
    void validate(std::uint64_t maximum_decoded_bytes) {
        cursor_ = payload_offset_;
        if (base64_) {
            const std::size_t length = uri_.size() - payload_offset_;
            if (length % 4U != 0U) {
                throw std::invalid_argument("Inline upload base64 length is invalid");
            }
            std::size_t padding = 0U;
            if (length > 0U && uri_.back() == '=') ++padding;
            if (length > 1U && uri_[uri_.size() - 2U] == '=') ++padding;
            for (std::size_t index = payload_offset_;
                 index + padding < uri_.size(); ++index) {
                if (base64Value(uri_[index]) < 0) {
                    throw std::invalid_argument("Inline upload base64 alphabet is invalid");
                }
            }
            for (std::size_t index = uri_.size() - padding;
                 index < uri_.size(); ++index) {
                if (uri_[index] != '=') {
                    throw std::invalid_argument("Inline upload base64 padding is invalid");
                }
            }
            decoded_size_ = length == 0U ? 0U
                    : static_cast<std::uint64_t>(length / 4U * 3U - padding);
        } else {
            std::size_t index = payload_offset_;
            while (index < uri_.size()) {
                if (uri_[index] == '%') {
                    if (uri_.size() - index < 3U
                        || hexValue(uri_[index + 1U]) < 0
                        || hexValue(uri_[index + 2U]) < 0) {
                        throw std::invalid_argument("Inline upload percent escape is invalid");
                    }
                    index += 3U;
                } else {
                    ++index;
                }
                ++decoded_size_;
            }
        }
        if (decoded_size_ > maximum_decoded_bytes) {
            throw std::invalid_argument("Inline upload exceeds its decoded byte limit");
        }
        cursor_ = payload_offset_;
    }

    void decodeBase64Group() {
        if (uri_.size() - cursor_ < 4U) {
            throw std::invalid_argument("Inline upload base64 group is truncated");
        }
        const char a = uri_[cursor_];
        const char b = uri_[cursor_ + 1U];
        const char c = uri_[cursor_ + 2U];
        const char d = uri_[cursor_ + 3U];
        const int av = base64Value(a);
        const int bv = base64Value(b);
        const int cv = c == '=' ? 0 : base64Value(c);
        const int dv = d == '=' ? 0 : base64Value(d);
        if (av < 0 || bv < 0 || cv < 0 || dv < 0
            || (c == '=' && d != '=')
            || (c == '=' && (bv & 0x0f) != 0)
            || (d == '=' && c != '=' && (cv & 0x03) != 0)
            || ((c == '=' || d == '=') && cursor_ + 4U != uri_.size())) {
            throw std::invalid_argument("Inline upload base64 group is invalid");
        }
        pending_[0] = static_cast<std::uint8_t>((av << 2) | (bv >> 4));
        pending_[1] = static_cast<std::uint8_t>((bv << 4) | (cv >> 2));
        pending_[2] = static_cast<std::uint8_t>((cv << 6) | dv);
        pending_size_ = c == '=' ? 1U : (d == '=' ? 2U : 3U);
        pending_offset_ = 0U;
        cursor_ += 4U;
    }

    void decodePercentByte() {
        if (uri_[cursor_] == '%') {
            pending_[0] = static_cast<std::uint8_t>(
                    (hexValue(uri_[cursor_ + 1U]) << 4)
                    | hexValue(uri_[cursor_ + 2U]));
            cursor_ += 3U;
        } else {
            pending_[0] = static_cast<std::uint8_t>(uri_[cursor_++]);
        }
        pending_size_ = 1U;
        pending_offset_ = 0U;
    }

    std::string uri_;
    std::size_t payload_offset_ = 0U;
    std::size_t cursor_ = 0U;
    bool base64_ = false;
    std::uint64_t decoded_size_ = 0U;
    std::array<std::uint8_t, 3U> pending_{};
    std::size_t pending_offset_ = 0U;
    std::size_t pending_size_ = 0U;
};

}  // namespace

UploadStreamFactory archiveEntryStreamFactory(
        const std::filesystem::path& archive_path,
        const package::PackageEntryEvidence& entry) {
    if (archive_path.empty() || entry.stable_ordinal == 0U
        || entry.entry_kind != package::PackageEntryKind::file) {
        throw std::invalid_argument("Archive expansion source is invalid");
    }
    return [archive_path, entry]() -> client::UploadStreamReader {
        int error = 0;
        auto state = std::make_shared<ArchiveStreamState>();
        state->archive.reset(zip_open(archive_path.string().c_str(),
                                      ZIP_RDONLY | ZIP_CHECKCONS, &error));
        if (!state->archive) {
            throw std::invalid_argument("Archive expansion source cannot be opened");
        }
        const zip_uint64_t index = static_cast<zip_uint64_t>(entry.stable_ordinal - 1U);
        zip_stat_t status;
        zip_stat_init(&status);
        const auto expected_method = [&entry]() -> zip_uint16_t {
            switch (entry.compression_method) {
                case package::PackageCompressionMethod::store:
                    return package::kZipMethodStore;
                case package::PackageCompressionMethod::deflate:
                    return package::kZipMethodDeflate;
                case package::PackageCompressionMethod::zstandard:
                    return package::kZipMethodZstandard;
                default:
                    throw std::invalid_argument(
                            "Archive expansion compression method is invalid");
            }
        }();
        if (zip_stat_index(state->archive.get(), index, ZIP_FL_UNCHANGED,
                           &status) != 0
            || (status.valid & ZIP_STAT_SIZE) == 0U
            || (status.valid & ZIP_STAT_COMP_METHOD) == 0U
            || status.size != entry.observed_expanded_bytes
            || status.comp_method != expected_method) {
            throw std::invalid_argument("Archive expansion entry identity changed");
        }
        state->file.reset(zip_fopen_index(state->archive.get(), index,
                                          ZIP_FL_UNCHANGED));
        if (!state->file) {
            throw std::invalid_argument("Archive expansion entry cannot be opened");
        }
        state->remaining = entry.observed_expanded_bytes;
        return [state](std::uint8_t* buffer, std::size_t capacity) {
            if (state->remaining == 0U) return std::size_t{0U};
            const std::size_t bounded = static_cast<std::size_t>(
                    std::min<std::uint64_t>(capacity, state->remaining));
            const zip_int64_t read = zip_fread(state->file.get(), buffer, bounded);
            if (read <= 0) {
                throw std::invalid_argument("Archive expansion stream is truncated");
            }
            state->remaining -= static_cast<std::uint64_t>(read);
            return static_cast<std::size_t>(read);
        };
    };
}

UploadStreamFactory dataUriStreamFactory(
        std::string data_uri, std::uint64_t maximum_decoded_bytes) {
    auto validated = std::make_shared<DataUriReader>(data_uri,
                                                     maximum_decoded_bytes);
    const std::uint64_t decoded_size = validated->decodedSize();
    return [data_uri = std::move(data_uri), maximum_decoded_bytes,
            decoded_size]() -> client::UploadStreamReader {
        auto reader = std::make_shared<DataUriReader>(data_uri,
                                                      maximum_decoded_bytes);
        if (reader->decodedSize() != decoded_size) {
            throw std::invalid_argument("Inline upload identity changed");
        }
        return [reader](std::uint8_t* buffer, std::size_t capacity) {
            return reader->read(buffer, capacity);
        };
    };
}

void ExpansionUploader::upload(
        InspectionLeaseSession& session,
        const client::ObjectTransfer& object_transfer,
        const ExpansionPlanDescriptor& descriptor,
        const std::map<std::string, ExpansionStreamSource>& sources,
        const package::ContinuePredicate& should_continue) const {
    upload(session, descriptor, sources,
           [&object_transfer](const std::string& url, std::uint64_t declared_size,
                              const std::string& expected_sha256,
                              const client::UploadStreamReader& reader,
                              const client::TransferContinuePredicate& predicate) {
               return object_transfer.uploadStream(url, declared_size,
                                                   expected_sha256, reader,
                                                   predicate);
           },
           should_continue);
}

void ExpansionUploader::upload(
        InspectionLeaseSession& session,
        const ExpansionPlanDescriptor& descriptor,
        const std::map<std::string, ExpansionStreamSource>& sources,
        const ExactStreamUploader& exact_upload,
        const package::ContinuePredicate& should_continue) const {
    if (!exact_upload) {
        throw std::invalid_argument("Expansion exact uploader is absent");
    }
    std::set<std::string> seen_objects;
    std::uint64_t observed_records = 0U;
    for (std::uint32_t page_number = 0U;
         page_number < descriptor.page_count; ++page_number) {
        if ((should_continue && !should_continue()) || !session.active()) {
            throw client::ObjectTransferCancelledError(
                    "Expanded upload was cancelled");
        }
        const ExpansionPlanPage page = session.expansionPlanPage(
                descriptor.plan_id, page_number);
        if (page.plan_id != descriptor.plan_id
            || page.page_number != page_number
            || page.record_count != page.records.size()) {
            throw std::invalid_argument("Expansion plan page identity is inconsistent");
        }
        observed_records += page.record_count;
        for (const auto& item : page.records) {
            if (!seen_objects.insert(item.object_id).second) {
                throw std::invalid_argument("Expansion plan repeats an object");
            }
            const bool worker_put = item.mode
                            == ExpansionUploadMode::worker_put_archive_entry
                    || item.mode == ExpansionUploadMode::worker_put_inline_data;
            if (!worker_put) continue;
            const auto source = sources.find(item.object_id);
            if (source == sources.end() || source->second.mode != item.mode
                || source->second.declared_size != item.declared_size
                || source->second.expected_sha256 != item.expected_sha256
                || !source->second.open_stream || !item.put_url.has_value()) {
                throw std::invalid_argument("Expansion stream source identity differs from plan");
            }
            const client::StreamedUploadMetadata uploaded =
                    exact_upload(*item.put_url, item.declared_size,
                                 item.expected_sha256,
                                 source->second.open_stream(),
                                 [&session, &should_continue] {
                                     return session.active()
                                             && (!should_continue
                                                 || should_continue());
                                 });
            ExpandedUploadReport report;
            report.object_id = item.object_id;
            report.observed_size = uploaded.observed_size;
            report.observed_sha256 = uploaded.sha256;
            report.observed_etag = uploaded.etag;
            session.reportExpandedUpload(descriptor.plan_id, report);
        }
    }
    if (observed_records != descriptor.record_count
        || seen_objects.size() != descriptor.record_count) {
        throw std::invalid_argument("Expansion plan record count is inconsistent");
    }
}

}  // namespace clip_worker::inspection::pipeline
