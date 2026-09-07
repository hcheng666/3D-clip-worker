#include "clip_worker/normalization/canonical_artifact.hpp"

#include "clip_worker/normalization/normalization_contract.hpp"

#include <gtest/gtest.h>

namespace clip_worker::normalization {
namespace {

TEST(MetadataSemanticHashTest, BindsValidationSummary) {
    CanonicalArtifactEvidence first;
    first.semantic_hash = sha256Hex("canonical-bytes");
    first.validation_summary.validator_name = "validator";
    first.validation_summary.validator_version = "1";
    first.validation_summary.validator_build_sha256 = sha256Hex("validator");
    first.validation_summary.validation_hash = sha256Hex("summary-a");
    CanonicalArtifactEvidence second = first;
    second.validation_summary.metadata_property_count = 1U;
    second.validation_summary.validation_hash = sha256Hex("summary-b");

    EXPECT_EQ(metadataSemanticHash(first), metadataSemanticHash(first));
    EXPECT_NE(metadataSemanticHash(first), metadataSemanticHash(second));
}

}  // namespace
}  // namespace clip_worker::normalization
