#pragma once

#include "clip_worker/clip/canonical_instance_clip_strategy.hpp"
#include "clip_worker/formats/cmpt.hpp"
#include "clip_worker/metadata/metadata_resource_limits.hpp"
#include "clip_worker/normalization/instance_normalizer.hpp"
#include "clip_worker/normalization/point_normalizer.hpp"

#include <filesystem>
#include <cstdint>
#include <string>

namespace clip_worker::normalization {

inline constexpr const char* kBroadResourceProfileVersion =
        "STANDARD_4CPU_8GIB_SINGLE_TASK_V3";
inline constexpr const char* kMetadataResourceProfileVersion =
        "STANDARD_4CPU_8GIB_SINGLE_TASK_V4";

struct BroadResourceProfile {
    std::string profile_id;
    std::string document_sha256;
    PointResourceLimits point;
    InstanceResourceLimits instance;
    clip::InstanceExpansionLimits expansion;
    formats::CmptLimits composite;
    metadata::MetadataResourceLimits metadata;
    std::uint64_t maximum_outputs = 0U;

    [[nodiscard]] static BroadResourceProfile load(
            const std::filesystem::path& document,
            const std::string& profile_id,
            const std::string& expected_document_sha256);
};

}  // namespace clip_worker::normalization
