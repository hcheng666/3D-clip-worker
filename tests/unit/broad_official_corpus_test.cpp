#include "clip_worker/normalization/broad_resource_profile.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/composite_normalizer.hpp"
#include "clip_worker/normalization/instance_canonical_writer.hpp"
#include "clip_worker/normalization/instance_normalizer.hpp"
#include "clip_worker/normalization/mesh_resource_profile.hpp"
#include "clip_worker/normalization/point_canonical_writer.hpp"
#include "clip_worker/normalization/point_normalizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace clip_worker::normalization {
namespace {

constexpr const char* kOfficialCorpusRootVariable =
        "CLIP_WORKER_OFFICIAL_CORPUS_ROOT";
constexpr const char* kMeshProfileSha256 =
        "bb326727f6b29a6cdd3532d85e2043c3c0ff212056b837d3d4a2eca7df3d349c";
constexpr const char* kBroadProfileSha256 =
        "d6097a29380375180e2cf9374eae354a9672e0589f3fb04b2f85930f16d5af78";
constexpr const char* kValidatorBuildSha256 =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

std::filesystem::path corpusRoot() {
    const char* configured = std::getenv(kOfficialCorpusRootVariable);
    if (configured != nullptr && *configured != '\0') {
        return configured;
    }
    return std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "tests/corpus/downloaded";
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0) {
        throw std::runtime_error("Official corpus file is unreadable");
    }
    std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(input.tellg()));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) throw std::runtime_error("Official corpus file read failed");
    return bytes;
}

formats::ApprovedGltfResourceMap approvedResources(
        const std::filesystem::path& payload_root,
        const std::filesystem::path& root_path) {
    formats::ApprovedGltfResourceMap resources;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(payload_root)) {
        if (!entry.is_regular_file() || entry.path() == root_path) continue;
        const std::string relative = std::filesystem::relative(
                entry.path(), payload_root).generic_string();
        resources.emplace(relative, formats::ApprovedGltfResource{
                readBytes(entry.path()), std::string{}});
    }
    return resources;
}

ToolVersion validator(const char* name) {
    return {name, "official-corpus-v1", kValidatorBuildSha256};
}

TEST(BroadOfficialCorpusTest, NormalizesRepresentativePointCloudVariants) {
    const auto entry_root = corpusRoot() / "cesium-point-cloud/payload";
    if (!std::filesystem::exists(entry_root)) {
        GTEST_SKIP() << "Task 7 official corpus is absent";
    }
    const auto profile = BroadResourceProfile::load(
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
                    / "config/resource-limit-profiles-v3.json",
            kBroadResourceProfileVersion, kBroadProfileSha256);
    const auto point_validator = validator("point-official-validator");
    constexpr std::array<const char*, 6U> kPointCases = {
            "PointCloudQuantized/pointCloudQuantized.pnts",
            "PointCloudNormalsOctEncoded/pointCloudNormalsOctEncoded.pnts",
            "PointCloudRGB565/pointCloudRGB565.pnts",
            "PointCloudBatched/pointCloudBatched.pnts",
            "PointCloudWithPerPointProperties/pointCloudWithPerPointProperties.pnts",
            "PointCloudWGS84/pointCloudWGS84.pnts"};

    for (const char* relative : kPointCases) {
        SCOPED_TRACE(relative);
        try {
            auto scene = PointNormalizer(profile.point).normalize(
                    formats::ByteView(readBytes(entry_root / relative)));
            const auto canonical = PointCanonicalWriter::write(std::move(scene));
            const auto evidence = validateCanonicalGlb(
                    canonical.glb, CanonicalFamily::point_gltf2,
                    point_validator);
            EXPECT_GT(canonical.point_count, 0U);
            EXPECT_EQ(evidence.output_size, canonical.glb.size());
        } catch (const std::exception& error) {
            ADD_FAILURE() << relative << ": " << error.what();
        }
    }
}

TEST(BroadOfficialCorpusTest, NormalizesRepresentativeInstancedVariants) {
    const auto entry_root = corpusRoot() / "cesium-instanced/payload";
    if (!std::filesystem::exists(entry_root)) {
        GTEST_SKIP() << "Task 7 official corpus is absent";
    }
    const auto profile = BroadResourceProfile::load(
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
                    / "config/resource-limit-profiles-v3.json",
            kBroadResourceProfileVersion, kBroadProfileSha256);
    const auto instance_validator = validator("instance-official-validator");
    constexpr std::array<const char*, 4U> kSupportedInstanceCases = {
            "InstancedOct32POrientation/instancedOct32POrientation.i3dm",
            "InstancedOrientation/instancedOrientation.i3dm",
            "InstancedQuantizedOct32POrientation/instancedQuantizedOct32POrientation.i3dm",
            "InstancedWithTransform/instancedWithTransform.i3dm"};
    constexpr std::array<const char*, 4U> kEnuOnlyInstanceCases = {
            "InstancedGltfExternal/instancedGltfExternal.i3dm",
            "InstancedRTC/instancedRTC.i3dm",
            "InstancedScaleNonUniform/instancedScaleNonUniform.i3dm",
            "InstancedWithBatchTableBinary/instancedWithBatchTableBinary.i3dm"};

    for (const char* relative : kSupportedInstanceCases) {
        SCOPED_TRACE(relative);
        try {
            const auto root_path = entry_root / relative;
            InstanceNormalizationInput input;
            input.root_package_relative_path = relative;
            input.source_bytes = readBytes(root_path);
            input.approved_resources = approvedResources(entry_root, root_path);
            auto normalized = InstanceNormalizer(profile.instance).normalize(input);
            const auto canonical = InstanceCanonicalWriter::write(
                    std::move(normalized.scene), instance_validator);
            EXPECT_GT(canonical.instance_count, 0U);
            EXPECT_EQ(canonical.evidence.output_size,
                      canonical.canonical.glb.size());
        } catch (const std::exception& error) {
            ADD_FAILURE() << relative << ": " << error.what();
        }
    }
    for (const char* relative : kEnuOnlyInstanceCases) {
        SCOPED_TRACE(relative);
        const auto root_path = entry_root / relative;
        InstanceNormalizationInput input;
        input.root_package_relative_path = relative;
        input.source_bytes = readBytes(root_path);
        input.approved_resources = approvedResources(entry_root, root_path);
        try {
            static_cast<void>(InstanceNormalizer(profile.instance).normalize(input));
            ADD_FAILURE() << relative << " unexpectedly normalized";
        } catch (const formats::FormatError& error) {
            EXPECT_EQ(formats::FormatErrorCode::i3dm_enu_unsupported,
                      error.code());
        }
    }
}

TEST(BroadOfficialCorpusTest, MarksOfficialEnuCompositeVariantsPreviewOnly) {
    const auto entry_root = corpusRoot() / "cesium-composite/payload";
    if (!std::filesystem::exists(entry_root)) {
        GTEST_SKIP() << "Task 7 official corpus is absent";
    }
    const auto broad_profile = BroadResourceProfile::load(
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
                    / "config/resource-limit-profiles-v3.json",
            kBroadResourceProfileVersion, kBroadProfileSha256);
    const auto mesh_profile = MeshResourceProfile::load(
            std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
                    / "config/resource-limit-profiles-v2.json",
            kMeshResourceProfileVersion, kMeshProfileSha256);
    constexpr std::array<const char*, 3U> kCompositeCases = {
            "Composite/composite.cmpt",
            "CompositeOfComposite/compositeOfComposite.cmpt",
            "CompositeOfInstanced/compositeOfInstanced.cmpt"};

    for (const char* relative : kCompositeCases) {
        SCOPED_TRACE(relative);
        const auto root_path = entry_root / relative;
        CompositeNormalizationInput input;
        input.root_package_relative_path = relative;
        input.source_bytes = readBytes(root_path);
        input.approved_resources = approvedResources(entry_root, root_path);
        const auto result = CompositeNormalizer::normalize(
                input, mesh_profile, broad_profile.point,
                broad_profile.instance, validator("mesh-official-validator"),
                validator("point-official-validator"),
                validator("instance-official-validator"),
                broad_profile.composite);
        ASSERT_FALSE(result.leaves.empty());
        EXPECT_TRUE(result.global_preview_only);
        EXPECT_TRUE(std::any_of(
                result.leaves.begin(), result.leaves.end(),
                [](const CompositeLeafResult& leaf) {
                    return leaf.status == CompositeLeafStatus::unsupported;
                }));
        EXPECT_TRUE(std::none_of(
                result.leaves.begin(), result.leaves.end(),
                [](const CompositeLeafResult& leaf) {
                    return leaf.status == CompositeLeafStatus::failure;
                }));
        EXPECT_FALSE(result.parent_manifest.empty());
    }
}

}  // namespace
}  // namespace clip_worker::normalization
