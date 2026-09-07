#include "clip_worker/normalization/broad_resource_profile.hpp"

#include "clip_worker/client/object_transfer.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

TEST(BroadResourceProfileTest, LoadsPinnedV3Limits) {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/resource-limit-profiles-v3.json";
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    const std::string digest = client::sha256Hex(bytes);
    EXPECT_EQ(digest,
              "d6097a29380375180e2cf9374eae354a9672e0589f3fb04b2f85930f16d5af78");
    const auto profile = BroadResourceProfile::load(
            path, kBroadResourceProfileVersion, digest);
    EXPECT_EQ(profile.point.maximum_points, 10000000U);
    EXPECT_EQ(profile.instance.maximum_instances, 250000U);
    EXPECT_EQ(profile.composite.maximum_depth, 8U);
    EXPECT_EQ(profile.composite.maximum_children, 1024U);
    EXPECT_EQ(profile.maximum_outputs, 1025U);
}

TEST(BroadResourceProfileTest, LoadsMetadataV4Limits) {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/resource-limit-profiles-v4.json";
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    const std::string digest = client::sha256Hex(bytes);
    const auto profile = BroadResourceProfile::load(
            path, kMetadataResourceProfileVersion, digest);
    EXPECT_EQ(profile.metadata.maximum_schema_bytes, 4194304U);
    EXPECT_EQ(profile.metadata.maximum_property_tables, 1024U);
    EXPECT_EQ(profile.metadata.maximum_hierarchy_depth, 256U);
    EXPECT_EQ(profile.metadata.maximum_validation_operations, 100000000U);
}

}  // namespace
}  // namespace clip_worker::normalization
