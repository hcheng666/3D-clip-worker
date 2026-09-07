#pragma once

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/formats/format_error.hpp"
#include "clip_worker/normalization/mesh_normalizer.hpp"
#include "clip_worker/normalization/normalization_runtime.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace clip_worker::normalization {

struct MeshNormalizationExecutorConfig {
    MeshResourceProfile profile;
    ToolVersion validator;
    std::filesystem::path scratch_root;
};

/** Production Normalizer V1 executor for the Task-6 MESH_GLTF2 family. */
class MeshNormalizationExecutor final {
public:
    MeshNormalizationExecutor(
            MeshNormalizationExecutorConfig config,
            client::ObjectTransfer object_transfer);

    void execute(NormalizationLeaseSession& session) const;

    /** Pure closure-to-input adapter used by the executor and fail-closed tests. */
    [[nodiscard]] static MeshNormalizationInput buildInput(
            const std::vector<ResourceRecord>& records,
            const std::map<std::string, std::vector<std::uint8_t>>& bytes_by_id);

private:
    MeshNormalizationExecutorConfig config_;
    client::ObjectTransfer object_transfer_;
};

[[nodiscard]] Failure meshNormalizationFailure(
        const formats::FormatError& error);

}  // namespace clip_worker::normalization
