#pragma once

#include "clip_worker/clip/texture_masker.hpp"
#include "clip_worker/formats/b3dm_mesh_adapter.hpp"
#include "clip_worker/geometry/authorization_scope.hpp"
#include "clip_worker/normalization/normalization_contract.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace clip_worker::normalization {

inline constexpr const char* kMeshResourceProfileVersion =
        "STANDARD_4CPU_8GIB_SINGLE_TASK_V2";
inline constexpr const char* kMeshNormalizationVersion = "normalization-v2";

/** Hash-closed Task-6 limits translated into the typed mesh adapters. */
struct MeshResourceProfile {
    std::string profile_id;
    std::string document_sha256;
    std::uint64_t maximum_input_bytes = 0U;
    std::uint64_t maximum_output_bytes = 0U;
    formats::B3dmMeshAdapterLimits b3dm;
    clip::TextureMaskLimits texture_mask;
    geometry::AuthorizationScopeLimits authorization;

    [[nodiscard]] static MeshResourceProfile load(
            const std::filesystem::path& document,
            const std::string& profile_id,
            const std::string& expected_document_sha256);
};

}  // namespace clip_worker::normalization
