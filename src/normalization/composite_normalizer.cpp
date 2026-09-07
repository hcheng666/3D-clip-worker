#include "clip_worker/normalization/composite_normalizer.hpp"

#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/instance_canonical_writer.hpp"
#include "clip_worker/normalization/mesh_normalizer.hpp"
#include "clip_worker/normalization/point_canonical_writer.hpp"

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

bool unsupportedCode(formats::FormatErrorCode code) {
    switch (code) {
        case formats::FormatErrorCode::unsupported_version:
        case formats::FormatErrorCode::unsupported_chunk:
        case formats::FormatErrorCode::unsupported_content:
        case formats::FormatErrorCode::content_primitive_mode_unsupported:
        case formats::FormatErrorCode::content_gltf_feature_unsupported:
        case formats::FormatErrorCode::metadata_batch_table_unsupported:
        case formats::FormatErrorCode::metadata_feature_id_texture_unsupported:
        case formats::FormatErrorCode::metadata_feature_id_triangle_ambiguous:
        case formats::FormatErrorCode::metadata_property_type_unsupported:
        case formats::FormatErrorCode::metadata_property_attribute_unsupported:
        case formats::FormatErrorCode::metadata_property_texture_unsupported:
        case formats::FormatErrorCode::metadata_hierarchy_unsupported:
        case formats::FormatErrorCode::metadata_relationship_unsupported:
        case formats::FormatErrorCode::metadata_statistics_unsupported:
        case formats::FormatErrorCode::metadata_unknown_required_extension:
        case formats::FormatErrorCode::metadata_reconstruction_unsafe:
        case formats::FormatErrorCode::pnts_semantic_unsupported:
        case formats::FormatErrorCode::i3dm_semantic_unsupported:
        case formats::FormatErrorCode::i3dm_enu_unsupported:
        case formats::FormatErrorCode::cmpt_child_unsupported:
            return true;
        default:
            return false;
    }
}

std::string reasonCode(formats::FormatErrorCode code) {
    switch (code) {
        case formats::FormatErrorCode::pnts_semantic_unsupported:
            return "CONTENT_PNTS_ENCODING_UNSUPPORTED";
        case formats::FormatErrorCode::i3dm_semantic_unsupported:
            return "CONTENT_I3DM_ENCODING_UNSUPPORTED";
        case formats::FormatErrorCode::i3dm_enu_unsupported:
            return "CONTENT_I3DM_ENCODING_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_batch_table_unsupported:
            return "METADATA_BATCH_TABLE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_feature_id_texture_unsupported:
            return "METADATA_FEATURE_ID_TEXTURE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_feature_id_triangle_ambiguous:
            return "METADATA_FEATURE_ID_TRIANGLE_AMBIGUOUS";
        case formats::FormatErrorCode::metadata_property_type_unsupported:
            return "METADATA_PROPERTY_TYPE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_property_attribute_unsupported:
            return "METADATA_PROPERTY_ATTRIBUTE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_property_texture_unsupported:
            return "METADATA_PROPERTY_TEXTURE_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_hierarchy_unsupported:
            return "METADATA_HIERARCHY_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_relationship_unsupported:
            return "METADATA_RELATIONSHIP_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_statistics_unsupported:
            return "METADATA_STATISTICS_UNSUPPORTED";
        case formats::FormatErrorCode::metadata_unknown_required_extension:
            return "METADATA_UNKNOWN_REQUIRED_EXTENSION";
        case formats::FormatErrorCode::metadata_reconstruction_unsafe:
            return "METADATA_RECONSTRUCTION_UNSAFE";
        case formats::FormatErrorCode::metadata_feature_id_invalid:
            return "METADATA_FEATURE_ID_INVALID";
        case formats::FormatErrorCode::metadata_property_table_invalid:
            return "METADATA_PROPERTY_TABLE_INVALID";
        case formats::FormatErrorCode::metadata_hierarchy_invalid:
            return "METADATA_HIERARCHY_INVALID";
        case formats::FormatErrorCode::metadata_leakage_verification_failed:
            return "METADATA_LEAKAGE_VERIFICATION_FAILED";
        case formats::FormatErrorCode::unsupported_content:
            return "CONTENT_CMPT_CHILD_UNSUPPORTED";
        default:
            return unsupportedCode(code) ? "CONTENT_CMPT_CHILD_UNSUPPORTED"
                                         : "NORMALIZATION_OUTPUT_INVALID";
    }
}

void appendLeaves(const std::vector<formats::CompositeChild>& children,
                  std::vector<const formats::CompositeChild*>& leaves) {
    for (const auto& child : children) {
        if (child.kind == formats::CompositeChildKind::cmpt) {
            appendLeaves(child.children, leaves);
        } else {
            leaves.push_back(&child);
        }
    }
}

Json manifestJson(const CompositeNormalizationResult& result) {
    Json leaves = Json::array();
    for (const auto& leaf : result.leaves) {
        Json item{{"childId", leaf.child_id},
                  {"depth", leaf.depth},
                  {"ordinalPath", leaf.ordinal_path},
                  {"reasonCode", leaf.reason_code},
                  {"sourceKind", leaf.source_kind},
                  {"sourceSize", leaf.source_size},
                  {"sourceSha256", leaf.source_sha256},
                  {"sourceVersion", leaf.source_version},
                  {"status", compositeLeafStatusName(leaf.status)}};
        if (leaf.canonical_family.has_value()) {
            item["canonicalFamily"] = canonicalFamilyName(*leaf.canonical_family);
        }
        if (leaf.evidence.has_value()) {
            item["outputSha256"] = leaf.evidence->output_sha256;
            item["semanticHash"] = leaf.evidence->semantic_hash;
            item["validationManifestSha256"] =
                    leaf.evidence->validation_manifest_sha256;
        }
        leaves.push_back(std::move(item));
    }
    return {{"globalPreviewOnly", result.global_preview_only},
            {"manifestVersion", "CANONICAL_COMPOSITE_CHILDREN_V1"},
            {"leaves", std::move(leaves)}};
}

}  // namespace

CompositeNormalizationResult CompositeNormalizer::normalize(
        const CompositeNormalizationInput& input,
        const MeshResourceProfile& mesh_profile,
        const PointResourceLimits& point_limits,
        const InstanceResourceLimits& instance_limits,
        const ToolVersion& mesh_validator,
        const ToolVersion& point_validator,
        const ToolVersion& instance_validator,
        const formats::CmptLimits& composite_limits) {
    const auto document = formats::CmptParser::parse(
            formats::ByteView(input.source_bytes), composite_limits);
    std::vector<const formats::CompositeChild*> leaves;
    appendLeaves(document.children, leaves);
    CompositeNormalizationResult result;
    result.leaves.reserve(leaves.size());
    for (const auto* child : leaves) {
        CompositeLeafResult leaf;
        leaf.ordinal_path = child->ordinal_path;
        leaf.depth = child->depth;
        leaf.child_id = child->child_id;
        leaf.source_sha256 = child->source_sha256;
        leaf.source_kind = formats::compositeChildKindName(child->kind);
        leaf.source_version = std::to_string(child->version);
        leaf.source_size = child->source_length;
        const auto begin = input.source_bytes.begin()
                + static_cast<std::ptrdiff_t>(child->source_offset);
        const std::vector<std::uint8_t> source(
                begin, begin + static_cast<std::ptrdiff_t>(child->source_length));
        try {
            if (child->kind == formats::CompositeChildKind::b3dm
                || child->kind == formats::CompositeChildKind::glb) {
                MeshNormalizationInput mesh_input;
                mesh_input.source_kind = child->kind
                                == formats::CompositeChildKind::b3dm
                        ? MeshSourceKind::b3dm : MeshSourceKind::glb;
                mesh_input.root_package_relative_path =
                        input.root_package_relative_path;
                mesh_input.source_bytes = source;
                mesh_input.approved_resources = input.approved_resources;
                auto normalized = MeshNormalizer::normalize(
                        mesh_input, mesh_profile, mesh_validator);
                leaf.canonical_family = CanonicalFamily::mesh_gltf2;
                leaf.canonical_bytes = std::move(normalized.canonical.glb);
                leaf.evidence = std::move(normalized.evidence);
                leaf.status = CompositeLeafStatus::success;
            } else if (child->kind == formats::CompositeChildKind::pnts) {
                auto scene = PointNormalizer(point_limits).normalize(
                        formats::ByteView(source));
                auto canonical = PointCanonicalWriter::write(std::move(scene));
                leaf.canonical_family = CanonicalFamily::point_gltf2;
                leaf.evidence = validateCanonicalGlb(
                        canonical.glb, *leaf.canonical_family, point_validator);
                leaf.point_count = canonical.point_count;
                leaf.canonical_bytes = std::move(canonical.glb);
                leaf.status = CompositeLeafStatus::success;
            } else if (child->kind == formats::CompositeChildKind::i3dm) {
                InstanceNormalizationInput instance_input;
                instance_input.root_package_relative_path =
                        input.root_package_relative_path;
                instance_input.source_bytes = source;
                instance_input.approved_resources = input.approved_resources;
                auto scene = InstanceNormalizer(instance_limits).normalize(
                        instance_input);
                auto canonical = InstanceCanonicalWriter::write(
                        std::move(scene.scene), instance_validator);
                leaf.canonical_family = CanonicalFamily::instance_gltf2;
                leaf.canonical_bytes = std::move(canonical.canonical.glb);
                leaf.evidence = std::move(canonical.evidence);
                leaf.instance_count = canonical.instance_count;
                leaf.status = CompositeLeafStatus::success;
            } else {
                leaf.status = CompositeLeafStatus::unsupported;
                leaf.reason_code = "CONTENT_CMPT_CHILD_UNSUPPORTED";
            }
        } catch (const formats::FormatError& error) {
            leaf.status = unsupportedCode(error.code())
                    ? CompositeLeafStatus::unsupported
                    : CompositeLeafStatus::failure;
            leaf.reason_code = reasonCode(error.code());
        } catch (const std::exception&) {
            leaf.status = CompositeLeafStatus::failure;
            leaf.reason_code = "NORMALIZATION_OUTPUT_INVALID";
        }
        if (leaf.status != CompositeLeafStatus::success) {
            result.global_preview_only = true;
        }
        result.leaves.push_back(std::move(leaf));
    }
    const std::string manifest = manifestJson(result).dump();
    result.parent_manifest.assign(manifest.begin(), manifest.end());
    result.parent_manifest_sha256 = sha256Hex(manifest);
    return result;
}

std::string compositeLeafStatusName(CompositeLeafStatus status) {
    switch (status) {
        case CompositeLeafStatus::success: return "READY";
        case CompositeLeafStatus::unsupported: return "UNSUPPORTED";
        case CompositeLeafStatus::failure: return "FAILED";
    }
    return "FAILED";
}

}  // namespace clip_worker::normalization
