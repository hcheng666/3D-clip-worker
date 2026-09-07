#pragma once

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/broad_resource_profile.hpp"
#include "clip_worker/normalization/mesh_resource_profile.hpp"
#include "clip_worker/normalization/normalization_v2_runtime.hpp"

#include <filesystem>
#include <map>

namespace clip_worker::normalization::v2 {

struct BroadNormalizationExecutorConfig {
    BroadResourceProfile broad_profile;
    MeshResourceProfile mesh_profile;
    ToolVersion mesh_validator;
    ToolVersion point_validator;
    ToolVersion instance_validator;
    ToolVersion composite_validator;
    std::filesystem::path scratch_root;
};

/** Production V2 executor for POINT, INSTANCE, and COMPOSITE tasks. */
class BroadNormalizationExecutor final {
public:
    BroadNormalizationExecutor(
            BroadNormalizationExecutorConfig config,
            client::ObjectTransfer object_transfer);

    void execute(NormalizationV2LeaseSession& session) const;

private:
    BroadNormalizationExecutorConfig config_;
    client::ObjectTransfer object_transfer_;
};

[[nodiscard]] Failure broadNormalizationFailure(
        const formats::FormatError& error);

}  // namespace clip_worker::normalization::v2
