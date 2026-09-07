#pragma once

#include "clip_worker/inspection/inspection_contract.hpp"
#include "clip_worker/inspection/package/package_enumerator.hpp"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace clip_worker::inspection::pipeline {

enum class PipelineFailureKind { invalid, limit_exceeded, cancelled };

class InspectionPipelineError final : public std::runtime_error {
public:
    InspectionPipelineError(PipelineFailureKind kind, DiagnosticCode code,
                            InspectorStage stage, std::string message);

    [[nodiscard]] PipelineFailureKind kind() const noexcept;
    [[nodiscard]] DiagnosticCode diagnosticCode() const noexcept;
    [[nodiscard]] InspectorStage stage() const noexcept;

private:
    PipelineFailureKind kind_;
    DiagnosticCode diagnostic_code_;
    InspectorStage stage_;
};

struct PipelineLimits {
    static constexpr std::uint64_t kDefaultMaximumJsonBytes = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::uint32_t kDefaultMaximumJsonNesting = 128U;
    static constexpr std::uint64_t kDefaultMaximumDataUriBytes = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::uint32_t kDefaultMaximumUriDepth = 32U;
    static constexpr std::uint64_t kDefaultMaximumHierarchySpoolBytes =
            4ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kDefaultMaximumSubtreeBytes =
            64ULL * 1024ULL * 1024ULL;

    std::uint64_t maximum_json_bytes = kDefaultMaximumJsonBytes;
    std::uint32_t maximum_json_nesting = kDefaultMaximumJsonNesting;
    std::uint64_t maximum_data_uri_bytes = kDefaultMaximumDataUriBytes;
    std::uint64_t maximum_expanded_bytes =
            ProtocolLimits::kMaximumExpandedResourceBytes;
    std::uint64_t maximum_resource_count = ProtocolLimits::kMaximumResourceCount;
    std::uint32_t maximum_uri_depth = kDefaultMaximumUriDepth;
    std::uint64_t maximum_available_tiles =
            ProtocolLimits::kMaximumAvailableTileCount;
    std::uint64_t maximum_hierarchy_records =
            ProtocolLimits::kMaximumHierarchyRecordCount;
    std::uint64_t maximum_documents = ProtocolLimits::kMaximumDocumentCount;
    std::uint64_t maximum_subtrees = ProtocolLimits::kMaximumSubtreeCount;
    std::uint64_t maximum_hierarchy_spool_bytes =
            kDefaultMaximumHierarchySpoolBytes;
    std::uint64_t maximum_subtree_bytes = kDefaultMaximumSubtreeBytes;
    std::uint32_t maximum_contents_per_tile =
            ProtocolLimits::kMaximumContentsPerTile;
    std::uint32_t maximum_tile_depth = ProtocolLimits::kMaximumTileDepth;
    std::uint32_t result_page_size =
            static_cast<std::uint32_t>(ProtocolLimits::kMaximumResultPageRecords);
    std::uint32_t hierarchy_page_size =
            static_cast<std::uint32_t>(ProtocolLimits::kMaximumHierarchyPageRecords);
};

/**
 * Returns at most maximum_bytes from the beginning of one already-enumerated
 * logical resource. The callback must not return bytes from another object.
 */
using ResourcePrefixReader = std::function<std::vector<std::uint8_t>(
        const package::PackageEntryEvidence&, std::uint64_t maximum_bytes)>;

struct PipelineOutput {
    InspectionResult result;
    std::vector<ResultPage> result_pages;
    std::vector<HierarchyPage> hierarchy_pages;
};

/** Bounded structure classifier, URI resolver, root selector, and closure builder. */
class InspectionPipeline final {
public:
    [[nodiscard]] PipelineOutput inspect(
            const InspectionRequest& request,
            const package::PackageEnumerationResult& package,
            const PipelineLimits& limits,
            const std::vector<ToolVersion>& tool_versions,
            const ResourcePrefixReader& reader,
            const package::ContinuePredicate& should_continue = {}) const;
};

}  // namespace clip_worker::inspection::pipeline
