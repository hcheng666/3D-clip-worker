#pragma once

#include "clip_worker/formats/gltf_mesh_reader.hpp"
#include "clip_worker/instance/instance_scene.hpp"
#include "clip_worker/normalization/canonical_artifact.hpp"
#include "clip_worker/point/point_scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::normalization {

/** Immutable identity advertised by the Clipper V2 control plane. */
struct CanonicalInputExpectation {
    CanonicalFamily family = CanonicalFamily::mesh_gltf2;
    std::string canonical_contract_version;
    std::uint64_t input_size = 0U;
    std::string input_sha256;
    std::string semantic_hash;
    std::string validation_manifest_sha256;
    ToolVersion validator;
};

struct CanonicalMeshReadResult {
    mesh::MeshScene scene;
    CanonicalArtifactEvidence evidence;
};

struct CanonicalPointReaderLimits {
    std::uint64_t maximum_points = 10000000ULL;
    metadata::MetadataResourceLimits metadata;
};

struct CanonicalPointReadResult {
    point::PointScene scene;
    CanonicalArtifactEvidence evidence;
};

struct CanonicalInstanceReadResult {
    instance::InstanceScene scene;
    CanonicalArtifactEvidence evidence;
};

/** Reopens a canonical mesh GLB and cross-checks all immutable evidence. */
class CanonicalMeshReader final {
public:
    [[nodiscard]] static CanonicalMeshReadResult read(
            const std::vector<std::uint8_t>& bytes,
            const CanonicalInputExpectation& expectation,
            formats::GltfMeshReaderLimits limits = {});
};

/** Strict reader for the deterministic POINT_GLTF2 writer contract. */
class CanonicalPointReader final {
public:
    [[nodiscard]] static CanonicalPointReadResult read(
            const std::vector<std::uint8_t>& bytes,
            const CanonicalInputExpectation& expectation,
            const CanonicalPointReaderLimits& limits = {});
};

/** Strict reader for the deterministic INSTANCE_GLTF2 writer contract. */
class CanonicalInstanceReader final {
public:
    [[nodiscard]] static CanonicalInstanceReadResult read(
            const std::vector<std::uint8_t>& bytes,
            const CanonicalInputExpectation& expectation,
            formats::GltfMeshReaderLimits limits = {});
};

}  // namespace clip_worker::normalization
