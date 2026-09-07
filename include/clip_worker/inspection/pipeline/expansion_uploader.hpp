#pragma once

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/inspection/inspection_runtime.hpp"
#include "clip_worker/inspection/package/package_enumerator.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace clip_worker::inspection::pipeline {

using UploadStreamFactory = std::function<client::UploadStreamReader()>;
using ExactStreamUploader = std::function<client::StreamedUploadMetadata(
        const std::string&, std::uint64_t, const std::string&,
        const client::UploadStreamReader&,
        const client::TransferContinuePredicate&)>;

struct ExpansionStreamSource {
    ExpansionUploadMode mode = ExpansionUploadMode::worker_put_archive_entry;
    std::uint64_t declared_size = 0U;
    std::string expected_sha256;
    UploadStreamFactory open_stream;
};

[[nodiscard]] UploadStreamFactory archiveEntryStreamFactory(
        const std::filesystem::path& archive_path,
        const package::PackageEntryEvidence& entry);

[[nodiscard]] UploadStreamFactory dataUriStreamFactory(
        std::string data_uri, std::uint64_t maximum_decoded_bytes);

/** Executes only exact Worker PUT grants and reports hash-closed observations. */
class ExpansionUploader final {
public:
    void upload(InspectionLeaseSession& session,
                const client::ObjectTransfer& object_transfer,
                const ExpansionPlanDescriptor& descriptor,
                const std::map<std::string, ExpansionStreamSource>& sources,
                const package::ContinuePredicate& should_continue = {}) const;

    /** Injectable exact-upload seam used for interruption and replay tests. */
    void upload(InspectionLeaseSession& session,
                const ExpansionPlanDescriptor& descriptor,
                const std::map<std::string, ExpansionStreamSource>& sources,
                const ExactStreamUploader& exact_upload,
                const package::ContinuePredicate& should_continue = {}) const;
};

}  // namespace clip_worker::inspection::pipeline
