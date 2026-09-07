#include "clip_worker/metadata/metadata_capability_matrix.hpp"

#include <filesystem>

#include <gtest/gtest.h>

namespace clip_worker::metadata {
namespace {

TEST(MetadataCapabilityMatrixTest, LoadsPinnedPolicyAndFailsClosed) {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/metadata-capability-matrix-v1.json";
    const auto matrix = MetadataCapabilityMatrix::load(path);
    EXPECT_EQ(matrix.entries().size(), 15U);
    EXPECT_EQ(matrix.require(MetadataSourceModel::legacy_batch_table_hierarchy)
                      .boundary_rebuild,
              MetadataBoundarySupport::supported);
    EXPECT_EQ(matrix.require(MetadataSourceModel::property_texture)
                      .boundary_rebuild,
              MetadataBoundarySupport::global_preview_only);
    EXPECT_EQ(matrix.require(
                             MetadataSourceModel::unknown_required_metadata_extension)
                      .reason_code,
              "METADATA_UNKNOWN_REQUIRED_EXTENSION");
}

TEST(MetadataCapabilityMatrixTest, RejectsUnexpectedDigest) {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/metadata-capability-matrix-v1.json";
    EXPECT_THROW(MetadataCapabilityMatrix::load(path, std::string(64U, '0')),
                 std::invalid_argument);
}

}  // namespace
}  // namespace clip_worker::metadata
