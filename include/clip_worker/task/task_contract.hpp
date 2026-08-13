#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::task {

enum class ContentFormat {
    b3dm_gltf2
};

enum class GltfUpAxis {
    z
};

struct ClaimRequest {
    std::string worker_id;
    std::vector<ContentFormat> supported_formats;
    std::string algorithm_version;
    std::uint64_t max_input_bytes = 0;
};

struct ClipOptions {
    bool mask_textures = true;
    bool compact_feature_metadata = true;
    bool cap_surface = false;
};

struct ClaimTask {
    static constexpr std::size_t kWorldTransformElementCount = 16;

    std::string asset_id;
    std::string lease_token;
    std::string lease_expire_time;
    std::string source_download_url;
    std::string source_etag;
    std::string output_upload_url;
    std::string scope_wkb_base64;
    std::int32_t scope_srid = 0;
    std::array<double, kWorldTransformElementCount> world_transform{};
    GltfUpAxis gltf_up_axis = GltfUpAxis::z;
    ContentFormat content_format = ContentFormat::b3dm_gltf2;
    ClipOptions clip_options;
};

struct LeaseRequest {
    std::string worker_id;
    std::string lease_token;
};

struct CompleteStatistics {
    std::uint64_t vertex_count_before = 0;
    std::uint64_t vertex_count_after = 0;
    std::uint64_t triangle_count_before = 0;
    std::uint64_t triangle_count_after = 0;
    std::uint64_t texture_bytes_before = 0;
    std::uint64_t texture_bytes_after = 0;
    std::uint64_t cost_ms = 0;
};

enum class CompletionResult {
    ready,
    empty
};

struct CompleteRequest {
    std::string worker_id;
    std::string lease_token;
    CompletionResult result = CompletionResult::ready;
    std::string output_etag;
    std::string output_sha256;
    std::uint64_t output_size = 0;
    CompleteStatistics statistics;
};

struct FailRequest {
    std::string worker_id;
    std::string lease_token;
    std::string error_code;
    std::string error_message;
    bool retryable = false;
};

[[nodiscard]] std::string contentFormatName(ContentFormat value);
[[nodiscard]] ContentFormat parseContentFormat(const std::string& value);
[[nodiscard]] std::string gltfUpAxisName(GltfUpAxis value);
[[nodiscard]] GltfUpAxis parseGltfUpAxis(const std::string& value);
[[nodiscard]] std::string completionResultName(CompletionResult value);
[[nodiscard]] std::string serializeClaimRequest(const ClaimRequest& request);
[[nodiscard]] ClaimTask parseClaimTask(const std::string& response_json);
[[nodiscard]] std::string serializeLeaseRequest(const LeaseRequest& request);
[[nodiscard]] std::string serializeCompleteRequest(const CompleteRequest& request);
[[nodiscard]] std::string serializeFailRequest(const FailRequest& request);

}  // namespace clip_worker::task
