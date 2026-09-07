#pragma once

#include "clip_worker/normalization/normalization_contract.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::normalization {

struct CanonicalArtifactEvidence {
    std::uint64_t output_size = 0U;
    std::string output_sha256;
    std::string semantic_hash;
    std::string validation_manifest_sha256;
    ValidationSummary validation_summary;

    [[nodiscard]] UploadDeclaration uploadDeclaration(
            CanonicalFamily family) const;
};

/**
 * Validates the shared Task-5 canonical GLB seam. Source-format conversion is
 * intentionally deferred to the family-specific tasks that follow.
 */
[[nodiscard]] CanonicalArtifactEvidence validateCanonicalGlb(
        const std::vector<std::uint8_t>& bytes, CanonicalFamily family,
        const ToolVersion& validator);

/**
 * Binds the validated canonical bytes to the complete validation summary for
 * the V3 metadata contract. Legacy contracts keep their existing hash bytes.
 */
[[nodiscard]] std::string metadataSemanticHash(
        const CanonicalArtifactEvidence& evidence);

}  // namespace clip_worker::normalization
