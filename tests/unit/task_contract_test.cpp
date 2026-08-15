#include "clip_worker/task/task_contract.hpp"
#include "clip_worker/task/worker_runtime.hpp"

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace clip_worker::task {
namespace {

nlohmann::json validClaimResponse() {
    return {{"assetId", "asset-1"},
            {"leaseToken", "lease-1"},
            {"leaseExpireTime", "2026-08-07T09:00:00Z"},
            {"sourceDownloadUrl", "https://minio/source"},
            {"sourceEtag", "etag-1"},
            {"outputUploadUrl", "https://minio/output"},
            {"scopeWkbBase64", "AQYAAA=="},
            {"scopeSrid", 4490},
            {"worldTransform", {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1}},
            {"gltfUpAxis", "Z"},
            {"contentFormat", "B3DM_GLTF2"},
            {"clipOptions",
             {{"maskTextures", true},
              {"compactFeatureMetadata", true},
              {"capSurface", false}}}};
}

TEST(TaskContractTest, SerializesClaimCapabilities) {
    const ClaimRequest request{"worker-a", {ContentFormat::b3dm_gltf2}, "v8", 16U * 1024U * 1024U};

    const auto json = nlohmann::json::parse(serializeClaimRequest(request));

    EXPECT_EQ(json.at("workerId"), "worker-a");
    EXPECT_EQ(json.at("supportedFormats").at(0), "B3DM_GLTF2");
    EXPECT_EQ(json.at("algorithmVersion"), "v8");
}

TEST(TaskContractTest, DefaultsWorkerRuntimeToCurrentAlgorithmVersion) {
    const WorkerRuntimeConfig config;

    EXPECT_EQ(config.algorithm_version, "v8");
}

TEST(TaskContractTest, ParsesCompleteClaimResponse) {
    const auto response = validClaimResponse();

    const auto task = parseClaimTask(response.dump());

    EXPECT_EQ(task.asset_id, "asset-1");
    EXPECT_EQ(task.scope_srid, 4490);
    EXPECT_EQ(task.gltf_up_axis, GltfUpAxis::z);
    EXPECT_EQ(task.world_transform.front(), 1.0);
    EXPECT_TRUE(task.clip_options.mask_textures);
    EXPECT_FALSE(task.clip_options.cap_surface);
}

TEST(TaskContractTest, ParsesProductionYUpClaimResponse) {
    auto response = validClaimResponse();
    response["gltfUpAxis"] = "Y";

    EXPECT_EQ(parseClaimTask(response.dump()).gltf_up_axis, GltfUpAxis::y);
}

TEST(TaskContractTest, RejectsTransformWithWrongElementCount) {
    auto response = validClaimResponse();
    response["worldTransform"] = {1, 0, 0};

    EXPECT_THROW(static_cast<void>(parseClaimTask(response.dump())), std::invalid_argument);
}

TEST(TaskContractTest, RejectsMissingOrUnsupportedGltfUpAxis) {
    auto response = validClaimResponse();
    response.erase("gltfUpAxis");
    EXPECT_THROW(static_cast<void>(parseClaimTask(response.dump())), std::invalid_argument);

    for (const auto* axis : {"X", "z", ""}) {
        response = validClaimResponse();
        response["gltfUpAxis"] = axis;
        EXPECT_THROW(static_cast<void>(parseClaimTask(response.dump())), std::invalid_argument);
    }

    response = validClaimResponse();
    response["gltfUpAxis"] = 1;
    EXPECT_THROW(static_cast<void>(parseClaimTask(response.dump())), std::invalid_argument);
}

TEST(TaskContractTest, SerializesEmptyCompletionWithoutObjectFields) {
    CompleteRequest request;
    request.worker_id = "worker-a";
    request.lease_token = "lease-a";
    request.result = CompletionResult::empty;

    const auto json = nlohmann::json::parse(serializeCompleteRequest(request));

    EXPECT_EQ(json.at("result"), "EMPTY");
    EXPECT_FALSE(json.contains("outputEtag"));
    EXPECT_FALSE(json.contains("outputSha256"));
    EXPECT_FALSE(json.contains("outputSize"));
}

TEST(TaskContractTest, RejectsReadyCompletionWithoutObjectDigest) {
    CompleteRequest request;
    request.worker_id = "worker-a";
    request.lease_token = "lease-a";
    request.result = CompletionResult::ready;

    EXPECT_THROW(static_cast<void>(serializeCompleteRequest(request)), std::invalid_argument);
}

}  // namespace
}  // namespace clip_worker::task
