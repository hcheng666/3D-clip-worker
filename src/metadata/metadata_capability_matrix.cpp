#include "clip_worker/metadata/metadata_capability_matrix.hpp"

#include "clip_worker/client/object_transfer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace clip_worker::metadata {
namespace {

using Json = nlohmann::json;

template <typename Enum, std::size_t Size>
Enum parseEnum(const std::string& value,
               const std::array<std::pair<const char*, Enum>, Size>& values,
               const char* field) {
    const auto match = std::find_if(
            values.begin(), values.end(), [&value](const auto& candidate) {
                return value == candidate.first;
            });
    if (match == values.end()) {
        throw std::invalid_argument(std::string("Metadata matrix ") + field
                                    + " is invalid");
    }
    return match->second;
}

constexpr std::array<std::pair<const char*, MetadataSourceModel>, 15U>
        kSourceModels{{
                {"NO_FEATURE_METADATA",
                 MetadataSourceModel::no_feature_metadata},
                {"LEGACY_BATCH_TABLE_JSON",
                 MetadataSourceModel::legacy_batch_table_json},
                {"LEGACY_BATCH_TABLE_BINARY",
                 MetadataSourceModel::legacy_batch_table_binary},
                {"LEGACY_BATCH_TABLE_HIERARCHY",
                 MetadataSourceModel::legacy_batch_table_hierarchy},
                {"ATTRIBUTE_FEATURE_ID_PROPERTY_TABLE",
                 MetadataSourceModel::attribute_feature_id_property_table},
                {"IMPLICIT_FEATURE_ID_PROPERTY_TABLE",
                 MetadataSourceModel::implicit_feature_id_property_table},
                {"MULTIPLE_ATTRIBUTE_OR_IMPLICIT_FEATURE_ID_SETS",
                 MetadataSourceModel::multiple_attribute_or_implicit_feature_id_sets},
                {"FEATURE_ID_TEXTURE", MetadataSourceModel::feature_id_texture},
                {"PROPERTY_TEXTURE", MetadataSourceModel::property_texture},
                {"PROPERTY_ATTRIBUTE", MetadataSourceModel::property_attribute},
                {"INT64_UINT64_OR_MATRIX_PROPERTY",
                 MetadataSourceModel::int64_uint64_or_matrix_property},
                {"UNKNOWN_MODERN_HIERARCHY_OR_RELATIONSHIP",
                 MetadataSourceModel::unknown_modern_hierarchy_or_relationship},
                {"ROOT_STATISTICS_OR_CUSTOM_AGGREGATE",
                 MetadataSourceModel::root_statistics_or_custom_aggregate},
                {"UNKNOWN_REQUIRED_METADATA_EXTENSION",
                 MetadataSourceModel::unknown_required_metadata_extension},
                {"RECOGNIZED_BUT_UNSAFE_TO_RECONSTRUCT",
                 MetadataSourceModel::recognized_but_unsafe_to_reconstruct},
        }};

constexpr std::array<std::pair<const char*, MetadataNormalizationSupport>, 2U>
        kNormalizationSupport{{
                {"SUPPORTED", MetadataNormalizationSupport::supported},
                {"DEFERRED", MetadataNormalizationSupport::deferred},
        }};

constexpr std::array<std::pair<const char*, MetadataBoundarySupport>, 2U>
        kBoundarySupport{{
                {"SUPPORTED", MetadataBoundarySupport::supported},
                {"GLOBAL_PREVIEW_ONLY",
                 MetadataBoundarySupport::global_preview_only},
        }};

}  // namespace

MetadataCapabilityMatrix MetadataCapabilityMatrix::load(
        const std::filesystem::path& document,
        const std::string& expected_sha256) {
    std::ifstream input(document, std::ios::binary);
    if (!input) {
        throw std::invalid_argument("Metadata capability matrix is unreadable");
    }
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    if (client::sha256Hex(bytes) != expected_sha256) {
        throw std::invalid_argument(
                "Metadata capability matrix SHA-256 mismatch");
    }
    const Json root = Json::parse(bytes.begin(), bytes.end());
    if (root.value("schemaVersion", 0U) != 1U
            || root.value("matrixVersion", std::string())
                    != kMetadataCapabilityMatrixVersion
            || !root.contains("entries") || !root.at("entries").is_array()) {
        throw std::invalid_argument("Metadata capability matrix is invalid");
    }
    MetadataCapabilityMatrix result;
    for (const auto& value : root.at("entries")) {
        if (!value.is_object() || !value.contains("sourceModel")
                || !value.contains("normalization")
                || !value.contains("boundaryRebuild")
                || !value.contains("reasonCode")) {
            throw std::invalid_argument(
                    "Metadata capability matrix entry is incomplete");
        }
        MetadataCapabilityEntry entry;
        entry.source_model = parseEnum(
                value.at("sourceModel").get<std::string>(), kSourceModels,
                "source model");
        entry.normalization = parseEnum(
                value.at("normalization").get<std::string>(),
                kNormalizationSupport, "normalization support");
        entry.boundary_rebuild = parseEnum(
                value.at("boundaryRebuild").get<std::string>(),
                kBoundarySupport, "boundary support");
        entry.reason_code = value.at("reasonCode").get<std::string>();
        if (entry.reason_code.empty()
                || std::any_of(result.entries_.begin(), result.entries_.end(),
                               [&entry](const auto& existing) {
                                   return existing.source_model
                                           == entry.source_model;
                               })) {
            throw std::invalid_argument(
                    "Metadata capability matrix entry is ambiguous");
        }
        result.entries_.push_back(std::move(entry));
    }
    if (result.entries_.size() != kSourceModels.size()) {
        throw std::invalid_argument(
                "Metadata capability matrix does not cover every source model");
    }
    return result;
}

const MetadataCapabilityEntry& MetadataCapabilityMatrix::require(
        MetadataSourceModel model) const {
    const auto match = std::find_if(
            entries_.begin(), entries_.end(), [model](const auto& entry) {
                return entry.source_model == model;
            });
    if (match == entries_.end()) {
        throw std::invalid_argument("Metadata source model is absent");
    }
    return *match;
}

const std::vector<MetadataCapabilityEntry>& MetadataCapabilityMatrix::entries()
        const noexcept {
    return entries_;
}

}  // namespace clip_worker::metadata
