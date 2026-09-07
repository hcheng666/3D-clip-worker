#include "clip_worker/authorization/authorization_clip_proof.hpp"

#include "clip_worker/normalization/normalization_contract.hpp"

#include <stdexcept>

namespace clip_worker::authorization {
namespace {

constexpr const char* kProofHashDomain =
        "three-d-authorization-safe-whole-proof-v1";
constexpr std::size_t kMaximumOrdinalDepth = 8U;

void append(std::string& target, const std::string& value) {
    target.push_back('\0');
    target.append(value);
}

void requireHash(const std::string& value, const char* field) {
    if (value.size() != 64U
            || value.find_first_not_of("0123456789abcdef")
                    != std::string::npos) {
        throw std::invalid_argument(
                std::string("Authorization proof hash field is invalid: ")
                + field);
    }
}

std::string calculatedHash(const AuthorizationClipProof& proof) {
    std::string identity(kProofHashDomain);
    append(identity, proof.proof_contract_version);
    append(identity, proof.input_sha256);
    append(identity, proof.input_semantic_hash);
    append(identity, proof.input_validation_manifest_sha256);
    append(identity, proof.scope_hash);
    append(identity, proof.transform_hash);
    append(identity, proof.canonical_family);
    append(identity, proof.canonical_contract_version);
    append(identity, proof.clip_strategy_version);
    append(identity, clip::authorizationContentRelationName(
            proof.classifier.relation));
    append(identity, proof.classifier.classifier_name);
    append(identity, proof.classifier.classifier_version);
    append(identity, std::to_string(proof.classifier.input_element_count));
    append(identity, std::to_string(proof.classifier.whole_element_count));
    append(identity, std::to_string(proof.classifier.disjoint_element_count));
    append(identity, std::to_string(proof.classifier.boundary_element_count));
    append(identity, std::to_string(proof.source_ordinal_path.size()));
    for (const std::uint32_t ordinal : proof.source_ordinal_path) {
        append(identity, std::to_string(ordinal));
    }
    return normalization::sha256Hex(identity);
}

void validateFields(const AuthorizationClipProof& proof) {
    if (proof.proof_contract_version != kProofContractVersion
            || proof.canonical_family.empty()
            || proof.canonical_contract_version.empty()
            || proof.clip_strategy_version.empty()
            || proof.classifier.classifier_name.empty()
            || proof.classifier.classifier_version.empty()
            || proof.source_ordinal_path.size() > kMaximumOrdinalDepth) {
        throw std::invalid_argument("Authorization proof identity is incomplete");
    }
    requireHash(proof.input_sha256, "inputSha256");
    requireHash(proof.input_semantic_hash, "inputSemanticHash");
    requireHash(proof.input_validation_manifest_sha256,
                "inputValidationManifestSha256");
    requireHash(proof.scope_hash, "scopeHash");
    requireHash(proof.transform_hash, "transformHash");
    const std::uint64_t classified = proof.classifier.whole_element_count
            + proof.classifier.disjoint_element_count
            + proof.classifier.boundary_element_count;
    if (proof.classifier.input_element_count == 0U
            || classified != proof.classifier.input_element_count) {
        throw std::invalid_argument(
                "Authorization proof classifier counts are inconsistent");
    }
    if ((proof.classifier.relation
                    == clip::AuthorizationContentRelation::empty
         && proof.classifier.disjoint_element_count
                    != proof.classifier.input_element_count)
            || (proof.classifier.relation
                        == clip::AuthorizationContentRelation::safe_whole
                && proof.classifier.whole_element_count
                        != proof.classifier.input_element_count)
            || (proof.classifier.relation
                        == clip::AuthorizationContentRelation::boundary
                && (proof.classifier.whole_element_count
                            == proof.classifier.input_element_count
                    || proof.classifier.disjoint_element_count
                            == proof.classifier.input_element_count))) {
        throw std::invalid_argument(
                "Authorization proof relation does not match exact counts");
    }
}

}  // namespace

AuthorizationClipProof buildAuthorizationClipProof(
        AuthorizationClipProof proof) {
    validateFields(proof);
    proof.proof_hash = calculatedHash(proof);
    return proof;
}

void validateAuthorizationClipProof(const AuthorizationClipProof& proof) {
    validateFields(proof);
    requireHash(proof.proof_hash, "proofHash");
    if (proof.proof_hash != calculatedHash(proof)) {
        throw std::invalid_argument("Authorization proof SHA-256 mismatch");
    }
}

}  // namespace clip_worker::authorization
