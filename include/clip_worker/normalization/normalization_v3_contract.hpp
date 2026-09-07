#pragma once

#include "clip_worker/normalization/normalization_v2_contract.hpp"

#include <stdexcept>
#include <string>

namespace clip_worker::normalization::v3 {

inline constexpr const char* kProtocolVersion =
        "THREE_D_TILES_NORMALIZER_V3";
inline constexpr const char* kSchemaSha256 =
        "bc8a587c664b0d12df53ae5ef5568873a932951d2738b04ef242b702dac92f17";
inline constexpr const char* kResourceProfileVersion =
        "STANDARD_4CPU_8GIB_SINGLE_TASK_V4";
inline constexpr const char* kTaskBasePath =
        "/api/internal/three-d-tiles/normalization-v3-tasks";

inline constexpr const char* kMeshNormalizationVersion =
        "normalization-mesh-metadata-v1";
inline constexpr const char* kPointNormalizationVersion =
        "normalization-point-metadata-v1";
inline constexpr const char* kInstanceNormalizationVersion =
        "normalization-instance-metadata-v1";
inline constexpr const char* kCompositeNormalizationVersion =
        "normalization-composite-metadata-v1";
inline constexpr const char* kMeshCanonicalContractVersion =
        "canonical-mesh-metadata-gltf2-v1";
inline constexpr const char* kPointCanonicalContractVersion =
        "canonical-point-metadata-gltf2-v1";
inline constexpr const char* kInstanceCanonicalContractVersion =
        "canonical-instance-metadata-gltf2-v1";
inline constexpr const char* kCompositeCanonicalContractVersion =
        "canonical-composite-children-metadata-v1";

using CanonicalFamily = v2::CanonicalFamily;
using FamilyCapability = v2::FamilyCapability;
using ClaimRequest = v2::ClaimRequest;

[[nodiscard]] inline std::string normalizationVersion(
        CanonicalFamily family) {
    switch (family) {
        case CanonicalFamily::mesh_gltf2: return kMeshNormalizationVersion;
        case CanonicalFamily::point_gltf2: return kPointNormalizationVersion;
        case CanonicalFamily::instance_gltf2:
            return kInstanceNormalizationVersion;
        case CanonicalFamily::composite_children:
            return kCompositeNormalizationVersion;
    }
    throw std::invalid_argument("Normalizer V3 family is invalid");
}

[[nodiscard]] inline std::string canonicalContractVersion(
        CanonicalFamily family) {
    switch (family) {
        case CanonicalFamily::mesh_gltf2:
            return kMeshCanonicalContractVersion;
        case CanonicalFamily::point_gltf2:
            return kPointCanonicalContractVersion;
        case CanonicalFamily::instance_gltf2:
            return kInstanceCanonicalContractVersion;
        case CanonicalFamily::composite_children:
            return kCompositeCanonicalContractVersion;
    }
    throw std::invalid_argument("Normalizer V3 family is invalid");
}

[[nodiscard]] inline bool isMetadataTuple(
        const FamilyCapability& capability) {
    return capability.normalization_version
                    == clip_worker::normalization::v3::normalizationVersion(
                            capability.canonical_family)
            && capability.canonical_contract_version
                    == clip_worker::normalization::v3::canonicalContractVersion(
                            capability.canonical_family);
}

inline void configureClaim(ClaimRequest& request) {
    request.protocol_version = kProtocolVersion;
    request.schema_sha256 = kSchemaSha256;
    request.resource_profile_version = kResourceProfileVersion;
}

}  // namespace clip_worker::normalization::v3
