#pragma once

#include "clip_worker/authorization/clipper_v2_runtime.hpp"
#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/broad_resource_profile.hpp"
#include "clip_worker/normalization/mesh_resource_profile.hpp"

namespace clip_worker::authorization::v2 {

struct ClipperV2ExecutorConfig {
    normalization::MeshResourceProfile mesh_profile;
    normalization::BroadResourceProfile broad_profile;
    normalization::ToolVersion mesh_validator;
    normalization::ToolVersion point_validator;
    normalization::ToolVersion instance_validator;
};

/** Executes one exact canonical clipping task without using legacy routes. */
class ClipperV2Executor final {
public:
    ClipperV2Executor(ClipperV2ExecutorConfig config,
                      client::ObjectTransfer object_transfer);

    void execute(ClipperV2LeaseSession& session) const;

private:
    ClipperV2ExecutorConfig config_;
    client::ObjectTransfer object_transfer_;
};

/** Stable fail-closed mapping for typed parser, classifier, and clip errors. */
[[nodiscard]] Failure clipperV2Failure(const formats::FormatError& error);

}  // namespace clip_worker::authorization::v2
