#pragma once

#include "clip_worker/clip/authorization_content_classifier.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::authorization {

inline constexpr const char* kProofContractVersion =
        "authorization-safe-whole-proof-v1";

struct AuthorizationClipProof {
    std::string proof_contract_version = kProofContractVersion;
    std::string input_sha256;
    std::string input_semantic_hash;
    std::string input_validation_manifest_sha256;
    std::string scope_hash;
    std::string transform_hash;
    std::string canonical_family;
    std::string canonical_contract_version;
    std::string clip_strategy_version;
    std::vector<std::uint32_t> source_ordinal_path;
    clip::ExactClassifierSummary classifier;
    std::string proof_hash;
};

/** Domain-separated deterministic proof; no source URI or grant participates. */
[[nodiscard]] AuthorizationClipProof buildAuthorizationClipProof(
        AuthorizationClipProof proof);

/** Recomputes and verifies the complete typed proof. */
void validateAuthorizationClipProof(const AuthorizationClipProof& proof);

}  // namespace clip_worker::authorization
