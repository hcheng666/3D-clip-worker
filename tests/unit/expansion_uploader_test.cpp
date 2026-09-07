#include "clip_worker/inspection/pipeline/expansion_uploader.hpp"

#include "clip_worker/client/inspection_api_client.hpp"
#include "clip_worker/client/object_transfer.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zip.h>

namespace clip_worker::inspection::pipeline {
namespace {

constexpr const char* kWorkerId = "inspector-worker-1";
constexpr const char* kLeaseToken = "lease-secret-must-not-leak";
constexpr const char* kPlanId = "plan-1";

std::filesystem::path sourcePath(const std::string& relative) {
#ifdef CLIP_WORKER_SOURCE_DIR
    return std::filesystem::path(CLIP_WORKER_SOURCE_DIR) / relative;
#else
    return std::filesystem::current_path() / relative;
#endif
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open expansion fixture");
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

nlohmann::json positiveFixture() {
    return nlohmann::json::parse(readText(sourcePath(
            "tests/fixtures/protocol/inspector-v1/positive-contract.json")));
}

InspectionClaimTask validTask() {
    const auto fixture = positiveFixture();
    const auto& request = fixture.at("inspectionRequest");
    return parseInspectionClaimTask(nlohmann::json({
            {"inspectionId", request.at("inspectionId")},
            {"requestId", request.at("requestId")},
            {"leaseToken", kLeaseToken},
            {"leaseExpireTime", "2026-08-17T08:00:45Z"},
            {"hardDeadlineTime", request.at("deadlineTime")},
            {"inspectionRequest", request}}).dump());
}

class FakeTransport final : public client::IInspectionHttpTransport {
public:
    mutable std::vector<client::InspectionHttpRequest> requests;
    mutable std::deque<client::InspectionHttpResponse> responses;

    client::InspectionHttpResponse execute(
            const client::InspectionHttpRequest& request) const override {
        requests.push_back(request);
        if (responses.empty()) throw std::logic_error("Fake response queue is empty");
        auto response = responses.front();
        responses.pop_front();
        return response;
    }
};

client::InspectionApiClient apiClient(
        const std::shared_ptr<FakeTransport>& transport) {
    client::InspectionApiClientConfig config;
    config.base_url = "https://control.invalid";
    return client::InspectionApiClient(std::move(config), transport);
}

std::vector<std::uint8_t> readAll(const UploadStreamFactory& factory) {
    auto reader = factory();
    std::vector<std::uint8_t> result;
    std::array<std::uint8_t, 2U> buffer{};
    for (;;) {
        const std::size_t count = reader(buffer.data(), buffer.size());
        if (count == 0U) break;
        result.insert(result.end(), buffer.begin(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return result;
}

class TemporaryZip final {
public:
    explicit TemporaryZip(const std::vector<std::uint8_t>& bytes) {
        path_ = std::filesystem::temp_directory_path()
                / "clip-worker-expansion-uploader-test.zip";
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        int error = 0;
        zip_t* archive = zip_open(path_.string().c_str(),
                                  ZIP_CREATE | ZIP_TRUNCATE, &error);
        if (archive == nullptr) throw std::runtime_error("Cannot create test ZIP");
        zip_source_t* source = zip_source_buffer(archive, bytes.data(),
                                                 bytes.size(), 0);
        if (source == nullptr) {
            zip_discard(archive);
            throw std::runtime_error("Cannot populate test ZIP");
        }
        if (zip_file_add(archive, "entry.bin", source, ZIP_FL_ENC_UTF_8) < 0) {
            zip_source_free(source);
            zip_discard(archive);
            throw std::runtime_error("Cannot populate test ZIP");
        }
        if (zip_set_file_compression(archive, 0U, ZIP_CM_STORE, 0U) != 0
            || zip_close(archive) != 0) {
            zip_discard(archive);
            throw std::runtime_error("Cannot populate test ZIP");
        }
    }

    ~TemporaryZip() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

ExpansionPlanDescriptor descriptor() {
    ExpansionPlanDescriptor value;
    value.plan_id = kPlanId;
    value.record_count = 1U;
    value.page_count = 1U;
    value.page_size = 1U;
    return value;
}

nlohmann::json planPage(const std::string& object_id,
                        ExpansionUploadMode mode,
                        std::uint64_t size, const std::string& sha256) {
    const char* mode_name = mode == ExpansionUploadMode::already_staged
            ? "ALREADY_STAGED" : "WORKER_PUT_INLINE_DATA";
    nlohmann::json item = {{"objectId", object_id},
                           {"mode", mode_name},
                           {"declaredSize", size},
                           {"expectedSha256", sha256}};
    if (mode != ExpansionUploadMode::already_staged) {
        item["putUrl"] = "https://storage.invalid/exact?signature=secret";
    }
    return {{"planId", kPlanId}, {"pageNumber", 0}, {"recordCount", 1},
            {"records", nlohmann::json::array({item})}};
}

TEST(ExpansionStreamFactoryTest, ReplaysStrictDataUriWithoutBufferingWholePlan) {
    const std::vector<std::uint8_t> expected = {1U, 2U, 3U};
    const auto factory = dataUriStreamFactory(
            "data:application/octet-stream;base64,AQID", 3U);
    EXPECT_EQ(readAll(factory), expected);
    EXPECT_EQ(readAll(factory), expected);
    EXPECT_EQ(readAll(dataUriStreamFactory("data:text/plain,a%20b", 3U)),
              std::vector<std::uint8_t>({'a', ' ', 'b'}));
    EXPECT_THROW(dataUriStreamFactory("data:textplain,abc", 3U),
                 std::invalid_argument);
    EXPECT_THROW(dataUriStreamFactory("data:text/plain;base64,AQI", 3U),
                 std::invalid_argument);
    EXPECT_THROW(dataUriStreamFactory("data:text/plain;base64,AQ=I", 3U),
                 std::invalid_argument);
    EXPECT_THROW(dataUriStreamFactory("data:text/plain,abcd", 3U),
                 std::invalid_argument);
}

TEST(ExpansionStreamFactoryTest, ReopensValidatedArchiveOrdinalForReplay) {
    const std::vector<std::uint8_t> expected = {'t', 'i', 'l', 'e'};
    TemporaryZip archive(expected);
    package::PackageEntryEvidence entry;
    entry.stable_ordinal = 1U;
    entry.entry_kind = package::PackageEntryKind::file;
    entry.compression_method = package::PackageCompressionMethod::store;
    entry.observed_expanded_bytes = expected.size();
    const auto factory = archiveEntryStreamFactory(archive.path(), entry);
    EXPECT_EQ(readAll(factory), expected);
    EXPECT_EQ(readAll(factory), expected);

    entry.observed_expanded_bytes++;
    const auto drifted = archiveEntryStreamFactory(archive.path(), entry);
    EXPECT_THROW(static_cast<void>(drifted()), std::invalid_argument);
}

TEST(ExpansionUploaderTest, ReportsOnlyAfterHashClosedStreamUpload) {
    const std::vector<std::uint8_t> bytes = {1U, 2U, 3U};
    const std::string sha256 = client::sha256Hex(bytes);
    auto transport = std::make_shared<FakeTransport>();
    transport->responses.push_back(
            {200, planPage("inline-1", ExpansionUploadMode::worker_put_inline_data,
                           bytes.size(), sha256).dump()});
    transport->responses.push_back({204, ""});
    auto api = apiClient(transport);
    InspectionLeaseSession session(api, kWorkerId, validTask());
    ExpansionStreamSource source;
    source.mode = ExpansionUploadMode::worker_put_inline_data;
    source.declared_size = bytes.size();
    source.expected_sha256 = sha256;
    source.open_stream = dataUriStreamFactory(
            "data:application/octet-stream;base64,AQID", bytes.size());

    ExpansionUploader{}.upload(session, descriptor(), {{"inline-1", source}},
            [&bytes, &sha256](const std::string&, std::uint64_t declared_size,
                             const std::string& expected_sha256,
                             const client::UploadStreamReader& reader,
                             const client::TransferContinuePredicate& predicate) {
                EXPECT_TRUE(predicate());
                std::vector<std::uint8_t> observed;
                std::array<std::uint8_t, 2U> buffer{};
                for (;;) {
                    const std::size_t count = reader(buffer.data(), buffer.size());
                    if (count == 0U) break;
                    observed.insert(observed.end(), buffer.begin(),
                            buffer.begin() + static_cast<std::ptrdiff_t>(count));
                }
                EXPECT_EQ(observed, bytes);
                EXPECT_EQ(declared_size, bytes.size());
                EXPECT_EQ(expected_sha256, sha256);
                return client::StreamedUploadMetadata{
                        declared_size, "etag-1", expected_sha256};
            });

    ASSERT_EQ(transport->requests.size(), 2U);
    const std::string& report = transport->requests.back().body;
    EXPECT_NE(report.find("inline-1"), std::string::npos);
    EXPECT_EQ(report.find("signature=secret"), std::string::npos);
}

TEST(ExpansionUploaderTest, InterruptedUploadDoesNotReportAndStagedReplaySkipsPut) {
    const std::vector<std::uint8_t> bytes = {1U, 2U, 3U};
    const std::string sha256 = client::sha256Hex(bytes);
    ExpansionStreamSource source;
    source.mode = ExpansionUploadMode::worker_put_inline_data;
    source.declared_size = bytes.size();
    source.expected_sha256 = sha256;
    source.open_stream = dataUriStreamFactory(
            "data:application/octet-stream;base64,AQID", bytes.size());

    auto interrupted_transport = std::make_shared<FakeTransport>();
    interrupted_transport->responses.push_back(
            {200, planPage("inline-1", ExpansionUploadMode::worker_put_inline_data,
                           bytes.size(), sha256).dump()});
    auto interrupted_api = apiClient(interrupted_transport);
    InspectionLeaseSession interrupted_session(
            interrupted_api, kWorkerId, validTask());
    EXPECT_THROW(ExpansionUploader{}.upload(
                         interrupted_session, descriptor(), {{"inline-1", source}},
                         [](const std::string&, std::uint64_t,
                            const std::string&,
                            const client::UploadStreamReader& reader,
                            const client::TransferContinuePredicate&) {
                             std::array<std::uint8_t, 1U> byte{};
                             static_cast<void>(reader(byte.data(), byte.size()));
                             throw client::ObjectTransferCancelledError(
                                     "Expanded upload was cancelled");
                             return client::StreamedUploadMetadata{};
                         }),
                 client::ObjectTransferCancelledError);
    EXPECT_EQ(interrupted_transport->requests.size(), 1U);

    auto replay_transport = std::make_shared<FakeTransport>();
    replay_transport->responses.push_back(
            {200, planPage("inline-1", ExpansionUploadMode::already_staged,
                           bytes.size(), sha256).dump()});
    auto replay_api = apiClient(replay_transport);
    InspectionLeaseSession replay_session(replay_api, kWorkerId, validTask());
    bool upload_called = false;
    EXPECT_NO_THROW(ExpansionUploader{}.upload(
            replay_session, descriptor(), {},
            [&upload_called](const std::string&, std::uint64_t,
                             const std::string&,
                             const client::UploadStreamReader&,
                             const client::TransferContinuePredicate&) {
                upload_called = true;
                return client::StreamedUploadMetadata{};
            }));
    EXPECT_FALSE(upload_called);
    EXPECT_EQ(replay_transport->requests.size(), 1U);
}

}  // namespace
}  // namespace clip_worker::inspection::pipeline
