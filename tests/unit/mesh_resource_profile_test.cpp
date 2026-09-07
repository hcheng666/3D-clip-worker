#include "clip_worker/normalization/mesh_resource_profile.hpp"

#include "clip_worker/client/object_transfer.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

std::filesystem::path profilePath() {
    return std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/resource-limit-profiles-v2.json";
}

std::string profileSha256() {
    std::ifstream input(profilePath(), std::ios::binary);
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    return client::sha256Hex(bytes);
}

TEST(MeshResourceProfileTest, LoadsHashClosedTaskSixLimits) {
    const auto profile = MeshResourceProfile::load(
            profilePath(), kMeshResourceProfileVersion, profileSha256());

    EXPECT_EQ(profile.profile_id, kMeshResourceProfileVersion);
    EXPECT_EQ(profile.b3dm.gltf.mesh.maximum_vertices, 5000000U);
    EXPECT_EQ(profile.b3dm.gltf.mesh.maximum_indices, 6000000U);
    EXPECT_EQ(profile.b3dm.gltf.texture.maximum_width, 8192U);
    EXPECT_EQ(profile.b3dm.gltf.draco.maximum_faces, 2000000U);
    EXPECT_EQ(profile.b3dm.gltf.meshopt.maximum_count, 6000000U);
    EXPECT_EQ(profile.b3dm.metadata.maximum_feature_rows, 10000000U);
    EXPECT_EQ(profile.texture_mask.maximum_repeat_span, 256U);
    EXPECT_EQ(profile.authorization.densify_segment_meters, 10.0);
}

TEST(MeshResourceProfileTest, RejectsProfileHashOrIdentityDrift) {
    EXPECT_THROW(MeshResourceProfile::load(
                         profilePath(), kMeshResourceProfileVersion,
                         std::string(64U, '0')),
                 std::invalid_argument);
    EXPECT_THROW(MeshResourceProfile::load(
                         profilePath(), "UNKNOWN_PROFILE", profileSha256()),
                 std::invalid_argument);
}

}  // namespace
}  // namespace clip_worker::normalization
