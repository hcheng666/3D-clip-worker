#include "clip_worker/authorization/clipper_v2_executor.hpp"

#include "clip_worker/authorization/authorization_clip_proof.hpp"
#include "clip_worker/clip/authorization_content_classifier.hpp"
#include "clip_worker/clip/canonical_instance_clip_strategy.hpp"
#include "clip_worker/clip/canonical_mesh_clip_strategy.hpp"
#include "clip_worker/clip/canonical_point_clip_strategy.hpp"
#include "clip_worker/geometry/authorization_scope.hpp"
#include "clip_worker/geometry/matrix4.hpp"
#include "clip_worker/normalization/canonical_input_reader.hpp"
#include "clip_worker/normalization/normalization_v3_contract.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace clip_worker::authorization::v2 {
namespace {

constexpr std::size_t kMaximumSafeFailureMessageBytes = 512U;

struct PreparedOutput {
    OutputDeclaration declaration;
    const std::vector<std::uint8_t>* bytes = nullptr;
};

void validateTool(const normalization::ToolVersion& tool,
                  const char* family) {
    if (tool.name.empty() || tool.version.empty()
            || tool.build_sha256.size() != 64U
            || tool.build_sha256.find_first_not_of("0123456789abcdef")
                    != std::string::npos) {
        throw std::invalid_argument(
                std::string("Clipper V2 validator identity is invalid: ")
                + family);
    }
}

normalization::CanonicalFamily inputFamily(CanonicalFamily family) {
    switch (family) {
        case CanonicalFamily::mesh_gltf2:
            return normalization::CanonicalFamily::mesh_gltf2;
        case CanonicalFamily::point_gltf2:
            return normalization::CanonicalFamily::point_gltf2;
        case CanonicalFamily::instance_gltf2:
            return normalization::CanonicalFamily::instance_gltf2;
    }
    throw std::invalid_argument("Clipper V2 canonical family is invalid");
}

const normalization::ToolVersion& inputValidator(
        const ClipperV2ExecutorConfig& config, CanonicalFamily family) {
    switch (family) {
        case CanonicalFamily::mesh_gltf2: return config.mesh_validator;
        case CanonicalFamily::point_gltf2: return config.point_validator;
        case CanonicalFamily::instance_gltf2:
            return config.instance_validator;
    }
    throw std::invalid_argument("Clipper V2 validator family is invalid");
}

normalization::CanonicalInputExpectation expectation(
        const ClaimTask& task, const ClipperV2ExecutorConfig& config) {
    normalization::CanonicalInputExpectation result;
    result.family = inputFamily(task.canonical_family);
    result.canonical_contract_version = task.canonical_contract_version;
    result.input_size = task.input_size;
    result.input_sha256 = task.input_sha256;
    result.semantic_hash = task.input_semantic_hash;
    result.validation_manifest_sha256 =
            task.input_validation_manifest_sha256;
    result.validator = inputValidator(config, task.canonical_family);
    return result;
}

geometry::Matrix4 taskTransform(const ClaimTask& task) {
    return geometry::Matrix4::fromColumnMajor(task.accumulated_transform);
}

geometry::AuthorizationScope taskScope(
        const ClaimTask& task,
        const normalization::MeshResourceProfile& mesh_profile) {
    return geometry::AuthorizationScope::fromWkb(
            geometry::decodeAuthorizationWkbBase64(task.scope_wkb_base64),
            task.scope_srid, mesh_profile.authorization);
}

AuthorizationClipProof proof(
        const ClaimTask& task,
        const clip::ExactClassifierSummary& classifier) {
    AuthorizationClipProof result;
    result.input_sha256 = task.input_sha256;
    result.input_semantic_hash = task.input_semantic_hash;
    result.input_validation_manifest_sha256 =
            task.input_validation_manifest_sha256;
    result.scope_hash = task.scope_hash;
    result.transform_hash = task.transform_hash;
    result.canonical_family = canonicalFamilyName(task.canonical_family);
    result.canonical_contract_version = task.canonical_contract_version;
    result.clip_strategy_version = task.clip_strategy_version;
    result.source_ordinal_path = task.source_ordinal_path;
    result.classifier = classifier;
    return buildAuthorizationClipProof(std::move(result));
}

normalization::CanonicalArtifactEvidence metadataEvidence(
        normalization::CanonicalArtifactEvidence evidence) {
    // Clipper V2 only consumes V3 metadata-safe canonical artifacts. Bind the
    // semantic identity to the complete validation summary just like V3.
    evidence.semantic_hash = normalization::metadataSemanticHash(evidence);
    return evidence;
}

OutputDeclaration declaration(
        const ClaimTask& task, std::uint32_t ordinal,
        CanonicalFamily family, const std::string& contract,
        normalization::CanonicalArtifactEvidence evidence) {
    OutputDeclaration result;
    result.output_id = clippedOutputId(task.work_item_id, ordinal);
    result.output_ordinal = ordinal;
    result.canonical_family = family;
    result.canonical_contract_version = contract;
    result.evidence = metadataEvidence(std::move(evidence));
    return result;
}

std::string boundedMessage(const std::string& value) {
    return value.substr(0U, kMaximumSafeFailureMessageBytes);
}

std::uint64_t saturatingAdd(std::uint64_t left, std::uint64_t right) {
    return left > std::numeric_limits<std::uint64_t>::max() - right
            ? std::numeric_limits<std::uint64_t>::max()
            : left + right;
}

}  // namespace

ClipperV2Executor::ClipperV2Executor(
        ClipperV2ExecutorConfig config,
        client::ObjectTransfer object_transfer)
    : config_(std::move(config)),
      object_transfer_(std::move(object_transfer)) {
    if (config_.mesh_profile.profile_id
                    != normalization::kMeshResourceProfileVersion
            || config_.broad_profile.profile_id
                    != normalization::kMetadataResourceProfileVersion) {
        throw std::invalid_argument(
                "Clipper V2 resource profile configuration is invalid");
    }
    validateTool(config_.mesh_validator, "mesh");
    validateTool(config_.point_validator, "point");
    validateTool(config_.instance_validator, "instance");
    config_.mesh_profile.b3dm.enable_feature_metadata = true;
    config_.mesh_profile.b3dm.feature_metadata =
            config_.broad_profile.metadata;
    config_.mesh_profile.b3dm.gltf.enable_feature_metadata = true;
    config_.mesh_profile.b3dm.gltf.metadata =
            config_.broad_profile.metadata;
}

void ClipperV2Executor::execute(ClipperV2LeaseSession& session) const {
    const ClaimTask& task = session.task();
    session.heartbeat(TaskPhase::downloading, 0U);
    const auto downloaded = object_transfer_.download(
            task.input_grant.url, task.limits.maximum_input_bytes);
    if (downloaded.bytes.size() != task.input_size
            || client::sha256Hex(downloaded.bytes) != task.input_sha256) {
        throw formats::FormatError(
                formats::FormatErrorCode::normalization_output_invalid,
                "Canonical clipping input identity mismatch");
    }
    session.heartbeat(TaskPhase::verifying, task.input_size);
    const auto expected = expectation(task, config_);
    const geometry::Matrix4 transform = taskTransform(task);
    auto authorization = taskScope(task, config_.mesh_profile);
    clip::ExactClassifierSummary classifier;

    if (task.canonical_family == CanonicalFamily::mesh_gltf2) {
        auto limits = config_.mesh_profile.b3dm.gltf;
        limits.enable_feature_metadata = true;
        limits.metadata = config_.broad_profile.metadata;
        auto input = normalization::CanonicalMeshReader::read(
                downloaded.bytes, expected, limits);
        session.heartbeat(TaskPhase::classifying, task.input_size);
        classifier = clip::MeshAuthorizationClassifier::classify(
                input.scene, transform, authorization);
        const auto exact_proof = proof(task, classifier);
        if (classifier.relation
                == clip::AuthorizationContentRelation::empty) {
            session.heartbeat(TaskPhase::completing, task.input_size);
            session.complete(CompletionOutcome::empty, exact_proof);
            return;
        }
        if (classifier.relation
                == clip::AuthorizationContentRelation::safe_whole) {
            session.heartbeat(TaskPhase::completing, task.input_size);
            session.complete(CompletionOutcome::safe_whole, exact_proof);
            return;
        }
        session.heartbeat(TaskPhase::clipping, task.input_size);
        clip::MeshSceneClipRequest request;
        request.scope_wkb = geometry::decodeAuthorizationWkbBase64(
                task.scope_wkb_base64);
        request.scope_srid = task.scope_srid;
        request.tileset_transform = transform;
        request.metadata_limits = config_.broad_profile.metadata;
        auto clipped = clip::CanonicalMeshClipStrategy::clip(
                std::move(input.scene), std::move(request),
                config_.mesh_profile, config_.mesh_validator);
        if (clipped.empty) {
            throw formats::FormatError(
                    formats::FormatErrorCode::clipping_output_invalid,
                    "Exact mesh boundary classification clipped to empty");
        }
        std::vector<PreparedOutput> outputs{{
                declaration(task, 0U, CanonicalFamily::mesh_gltf2,
                            task.canonical_contract_version,
                            std::move(clipped.evidence)),
                &clipped.canonical.glb}};
        session.heartbeat(TaskPhase::validating_output, task.input_size);
        std::vector<OutputDeclaration> ordered;
        for (const auto& output : outputs) {
            const auto grant = session.prepareOutput(output.declaration);
            session.heartbeat(TaskPhase::uploading, task.input_size);
            const std::string etag = object_transfer_.upload(
                    grant.upload_url, *output.bytes);
            session.reportOutput({output.declaration.output_id,
                                  client::normalizeEtag(etag),
                                  output.declaration.evidence.output_size,
                                  output.declaration.evidence.output_sha256});
            ordered.push_back(output.declaration);
        }
        session.heartbeat(TaskPhase::completing,
                          saturatingAdd(
                                  task.input_size,
                                  outputs.front().declaration.evidence.output_size));
        session.complete(CompletionOutcome::clipped, exact_proof, ordered);
        return;
    }

    if (task.canonical_family == CanonicalFamily::point_gltf2) {
        normalization::CanonicalPointReaderLimits limits;
        limits.maximum_points = std::min(
                task.limits.maximum_points,
                config_.broad_profile.point.maximum_points);
        limits.metadata = config_.broad_profile.metadata;
        auto input = normalization::CanonicalPointReader::read(
                downloaded.bytes, expected, limits);
        session.heartbeat(TaskPhase::classifying, task.input_size);
        classifier = clip::PointAuthorizationClassifier::classify(
                input.scene, transform, authorization);
        const auto exact_proof = proof(task, classifier);
        if (classifier.relation
                == clip::AuthorizationContentRelation::empty) {
            session.heartbeat(TaskPhase::completing, task.input_size);
            session.complete(CompletionOutcome::empty, exact_proof);
            return;
        }
        if (classifier.relation
                == clip::AuthorizationContentRelation::safe_whole) {
            session.heartbeat(TaskPhase::completing, task.input_size);
            session.complete(CompletionOutcome::safe_whole, exact_proof);
            return;
        }
        session.heartbeat(TaskPhase::clipping, task.input_size);
        auto clipped = clip::CanonicalPointClipStrategy::clip(
                std::move(input.scene), transform, authorization,
                config_.broad_profile.metadata, config_.point_validator,
                task.limits.maximum_output_bytes);
        if (clipped.empty) {
            throw formats::FormatError(
                    formats::FormatErrorCode::point_output_invalid,
                    "Exact point boundary classification clipped to empty");
        }
        OutputDeclaration output = declaration(
                task, 0U, CanonicalFamily::point_gltf2,
                task.canonical_contract_version, std::move(clipped.evidence));
        session.heartbeat(TaskPhase::validating_output, task.input_size);
        const auto grant = session.prepareOutput(output);
        session.heartbeat(TaskPhase::uploading, task.input_size);
        const std::string etag = object_transfer_.upload(
                grant.upload_url, clipped.canonical.glb);
        session.reportOutput({output.output_id, client::normalizeEtag(etag),
                              output.evidence.output_size,
                              output.evidence.output_sha256});
        session.heartbeat(TaskPhase::completing,
                          saturatingAdd(task.input_size,
                                        output.evidence.output_size));
        session.complete(CompletionOutcome::clipped, exact_proof, {output});
        return;
    }

    auto limits = config_.broad_profile.instance.model;
    limits.enable_feature_metadata = true;
    limits.metadata = config_.broad_profile.metadata;
    auto input = normalization::CanonicalInstanceReader::read(
            downloaded.bytes, expected, limits);
    if (input.scene.instanceCount() > task.limits.maximum_instances
            || input.scene.instanceCount()
                    > config_.broad_profile.instance.maximum_instances) {
        throw formats::FormatError(
                formats::FormatErrorCode::instance_expansion_limit_exceeded,
                "Canonical instance count exceeds the task limit");
    }
    session.heartbeat(TaskPhase::classifying, task.input_size);
    classifier = clip::InstanceAuthorizationClassifier::classify(
            input.scene, transform, authorization);
    const auto exact_proof = proof(task, classifier);
    if (classifier.relation == clip::AuthorizationContentRelation::empty) {
        session.heartbeat(TaskPhase::completing, task.input_size);
        session.complete(CompletionOutcome::empty, exact_proof);
        return;
    }
    if (classifier.relation
            == clip::AuthorizationContentRelation::safe_whole) {
        session.heartbeat(TaskPhase::completing, task.input_size);
        session.complete(CompletionOutcome::safe_whole, exact_proof);
        return;
    }
    session.heartbeat(TaskPhase::clipping, task.input_size);
    clip::MeshSceneClipRequest request;
    request.scope_wkb = geometry::decodeAuthorizationWkbBase64(
            task.scope_wkb_base64);
    request.scope_srid = task.scope_srid;
    request.tileset_transform = transform;
    request.metadata_limits = config_.broad_profile.metadata;
    clip::InstanceExpansionLimits expansion = config_.broad_profile.expansion;
    expansion.maximum_boundary_instances = std::min(
            expansion.maximum_boundary_instances,
            task.limits.maximum_boundary_instances);
    expansion.maximum_expanded_vertices = std::min(
            expansion.maximum_expanded_vertices,
            task.limits.maximum_expanded_vertices);
    expansion.maximum_expanded_indices = std::min(
            expansion.maximum_expanded_indices,
            task.limits.maximum_expanded_indices);
    auto clipped = clip::CanonicalInstanceClipStrategy::clip(
            std::move(input.scene), std::move(request),
            config_.mesh_profile, config_.instance_validator,
            config_.mesh_validator, expansion);
    std::vector<PreparedOutput> outputs;
    if (clipped.whole_instances.has_value()) {
        outputs.push_back({
                declaration(task, 0U, CanonicalFamily::instance_gltf2,
                            task.canonical_contract_version,
                            std::move(clipped.whole_instances->evidence)),
                &clipped.whole_instances->canonical.glb});
    }
    if (clipped.boundary_mesh.has_value()) {
        outputs.push_back({
                declaration(task, 1U, CanonicalFamily::mesh_gltf2,
                            normalization::v3::kMeshCanonicalContractVersion,
                            std::move(clipped.boundary_mesh->evidence)),
                &clipped.boundary_mesh->canonical.glb});
    }
    if (outputs.empty()) {
        throw formats::FormatError(
                formats::FormatErrorCode::instance_output_invalid,
                "Exact instance boundary classification produced no output");
    }
    std::sort(outputs.begin(), outputs.end(),
              [](const PreparedOutput& left, const PreparedOutput& right) {
                  return left.declaration.output_ordinal
                          < right.declaration.output_ordinal;
              });
    session.heartbeat(TaskPhase::validating_output, task.input_size);
    std::vector<OutputDeclaration> ordered;
    std::uint64_t progress = task.input_size;
    for (const auto& output : outputs) {
        const auto grant = session.prepareOutput(output.declaration);
        session.heartbeat(TaskPhase::uploading, progress);
        const std::string etag = object_transfer_.upload(
                grant.upload_url, *output.bytes);
        session.reportOutput({output.declaration.output_id,
                              client::normalizeEtag(etag),
                              output.declaration.evidence.output_size,
                              output.declaration.evidence.output_sha256});
        progress = saturatingAdd(
                progress, output.declaration.evidence.output_size);
        ordered.push_back(output.declaration);
    }
    session.heartbeat(TaskPhase::completing, progress);
    session.complete(CompletionOutcome::clipped, exact_proof, ordered);
}

Failure clipperV2Failure(const formats::FormatError& error) {
    Failure result;
    result.retryable = false;
    result.error_message = boundedMessage(error.what());
    switch (error.code()) {
        case formats::FormatErrorCode::instance_expansion_limit_exceeded:
        case formats::FormatErrorCode::compression_draco_limit_exceeded:
        case formats::FormatErrorCode::compression_meshopt_limit_exceeded:
        case formats::FormatErrorCode::texture_dimension_limit_exceeded:
        case formats::FormatErrorCode::texture_decoded_bytes_limit_exceeded:
            result.error_code = FailureCode::resource_limit_exceeded;
            break;
        case formats::FormatErrorCode::clipping_output_invalid:
        case formats::FormatErrorCode::point_output_invalid:
        case formats::FormatErrorCode::instance_output_invalid:
            result.error_code = FailureCode::output_validation_failed;
            break;
        case formats::FormatErrorCode::normalization_output_invalid:
            result.error_code = FailureCode::input_identity_mismatch;
            break;
        default:
            result.error_code = FailureCode::input_validation_failed;
            break;
    }
    return result;
}

}  // namespace clip_worker::authorization::v2
