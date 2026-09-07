#include "clip_worker/inspection/pipeline/inspection_pipeline.hpp"

#include "clip_worker/client/object_transfer.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::inspection::pipeline {
namespace {

using package::PackageEntryEvidence;
using package::PackageEntryKind;
using package::PackageEnumerationResult;
using package::PackagePathAnalyzer;

std::vector<std::uint8_t> bytes(const std::string& value) {
    return {value.begin(), value.end()};
}

void appendLe32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::uint32_t byte = 0U; byte < 4U; ++byte) {
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

void appendLe64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

std::vector<std::uint8_t> binarySubtree(std::string json,
                                        std::vector<std::uint8_t> binary) {
    constexpr std::size_t kSubtreeChunkAlignment = 8U;
    while (json.size() % kSubtreeChunkAlignment != 0U) json.push_back(' ');
    while (binary.size() % kSubtreeChunkAlignment != 0U) binary.push_back(0U);
    std::vector<std::uint8_t> output{'s', 'u', 'b', 't'};
    appendLe32(output, 1U);
    appendLe64(output, json.size());
    appendLe64(output, binary.size());
    output.insert(output.end(), json.begin(), json.end());
    output.insert(output.end(), binary.begin(), binary.end());
    return output;
}

PackageEntryEvidence entry(const std::string& id, const std::string& path,
                           const std::vector<std::uint8_t>& content) {
    PackageEntryEvidence result;
    result.object_id = id;
    result.path = PackagePathAnalyzer{}.analyze(
            path, ProtocolLimits::kMaximumPackagePathUtf8Bytes);
    result.entry_kind = PackageEntryKind::file;
    result.observed_expanded_bytes = content.size();
    result.observed_sha256 = client::sha256Hex(content);
    result.source_etag = "etag-" + id;
    return result;
}

InspectionRequest request() {
    InspectionRequest result;
    result.protocol_version = kProtocolVersion;
    result.inspection_id = "inspection-pipeline";
    result.request_id = "request-pipeline";
    result.source_generation = "generation-pipeline";
    result.inspector_version = "inspector-v1";
    return result;
}

std::vector<ToolVersion> tools() {
    ToolVersion inspector;
    inspector.name = ToolName::inspector;
    inspector.version = "1.0.0";
    return {inspector};
}

PipelineOutput inspect(
        InspectionRequest inspection_request,
        const std::map<std::string, std::vector<std::uint8_t>>& content,
        PipelineLimits limits = PipelineLimits{}) {
    PackageEnumerationResult package;
    for (const auto& item : content) {
        package.entries.push_back(entry("id-" + std::to_string(package.entries.size()),
                                        item.first, item.second));
    }
    return InspectionPipeline{}.inspect(
            inspection_request, package, limits, tools(),
            [&content](const PackageEntryEvidence& evidence,
                       std::uint64_t maximum_bytes) {
                const auto& source = content.at(evidence.path.normalized_candidate);
                const std::size_t count = static_cast<std::size_t>(
                        std::min<std::uint64_t>(source.size(), maximum_bytes));
                return std::vector<std::uint8_t>(source.begin(),
                        source.begin() + static_cast<std::ptrdiff_t>(count));
            });
}

const ResourceEvidence& evidence(const PipelineOutput& output,
                                 const std::string& path) {
    for (const auto& page : output.result_pages) {
        for (const auto& record : page.records) {
            if (record.package_relative_path == path) return record;
        }
    }
    throw std::invalid_argument("Fixture evidence is absent");
}

std::vector<const HierarchyRecord*> hierarchyRecords(
        const PipelineOutput& output, HierarchyRecordType type) {
    std::vector<const HierarchyRecord*> records;
    for (const auto& page : output.hierarchy_pages) {
        for (const auto& record : page.records) {
            if (record.record_type == type) records.push_back(&record);
        }
    }
    return records;
}

TEST(InspectionPipelineTest, BuildsHashClosedGraphWithoutUsingSuffixes) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"image", {0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU}},
            {"model", bytes(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,AQID"}],"images":[{"uri":"image"}],"extensionsRequired":["VENDOR_unknown"],"extensionsUsed":["VENDOR_unknown"]})")},
            {"root", bytes(R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0,"content":{"uri":"model"}}})")},
            {"unused", {1U, 2U, 3U}}};
    const PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.outcome, Outcome::succeeded);
    ASSERT_TRUE(output.result.source_closure_hash.has_value());
    EXPECT_EQ(output.result.root_selection.method,
              RootSelectionMethod::unique_top_level);
    EXPECT_EQ(output.result.root_selection.selected_root_path, "root");
    EXPECT_EQ(output.result.total_resources, 5U);
    EXPECT_TRUE(evidence(output, "root").required);
    EXPECT_TRUE(evidence(output, "model").required);
    EXPECT_TRUE(evidence(output, "image").required);
    EXPECT_FALSE(evidence(output, "unused").required);
    EXPECT_EQ(evidence(output, "model").detected_kind, DetectedKind::gltf_json);
    EXPECT_EQ(evidence(output, "image").detected_kind, DetectedKind::image);
    EXPECT_EQ(evidence(output, "model").required_extensions,
              std::vector<std::string>({"VENDOR_unknown"}));
    EXPECT_EQ(evidence(output, "model").used_extensions,
              std::vector<std::string>({"VENDOR_unknown"}));
    ASSERT_FALSE(output.result.diagnostics.empty());
    EXPECT_EQ(canonicalResultManifestSha256(output.result_pages),
              output.result.result_manifest.manifest_sha256);
}

TEST(InspectionPipelineTest, KeepsContentClosureStableAcrossInspectionRequests) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"model", bytes(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,AQID","byteLength":3}]})")},
            {"root", bytes(R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0,"content":{"uri":"model"}}})")}};
    const PipelineOutput first = inspect(request(), content);
    InspectionRequest replay_request = request();
    replay_request.inspection_id = "inspection-pipeline-replay";
    replay_request.request_id = "request-pipeline-replay";
    const PipelineOutput replay = inspect(replay_request, content);

    const auto first_contents = hierarchyRecords(first, HierarchyRecordType::content);
    const auto replay_contents = hierarchyRecords(replay, HierarchyRecordType::content);
    ASSERT_EQ(first_contents.size(), 1U);
    ASSERT_EQ(replay_contents.size(), 1U);
    EXPECT_NE(first_contents[0]->record_id, replay_contents[0]->record_id);
    EXPECT_EQ(first_contents[0]->resource_closure_hash,
              replay_contents[0]->resource_closure_hash);
}

TEST(InspectionPipelineTest, PublishesFactualPagesForAmbiguousRoots) {
    const auto tileset = bytes(
            R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0}})");
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"a/root", tileset}, {"b/root", tileset}};
    PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.outcome, Outcome::rejected);
    EXPECT_EQ(output.result.root_selection.method, RootSelectionMethod::ambiguous);
    EXPECT_FALSE(output.result.source_closure_hash.has_value());
    EXPECT_EQ(output.result_pages.front().records.size(), 2U);

    InspectionRequest explicit_request = request();
    explicit_request.selected_root_hint = "b/root";
    output = inspect(explicit_request, content);
    EXPECT_EQ(output.result.outcome, Outcome::succeeded);
    EXPECT_EQ(output.result.root_selection.method, RootSelectionMethod::explicit_root);
    EXPECT_EQ(output.result.root_selection.selected_root_path, "b/root");
}

TEST(InspectionPipelineTest, SelectsUniqueExternalTilesetGraphRoot) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"graph/child", bytes(R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0}})")},
            {"graph/parent", bytes(R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0,"content":{"uri":"child"}}})")}};
    const PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.root_selection.method,
              RootSelectionMethod::unique_graph_root);
    EXPECT_EQ(output.result.root_selection.selected_root_path, "graph/parent");
    EXPECT_TRUE(evidence(output, "graph/child").required);
}

TEST(InspectionPipelineTest, InventoriesTransformsRefineAndOrderedContents) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"a.gltf", bytes(R"({"asset":{"version":"2.0"},"meshes":[]})")},
            {"b.gltf", bytes(R"({"asset":{"version":"2.0"},"meshes":[]})")},
            {"root", bytes(R"({"asset":{"version":"1.1"},"geometricError":8,"root":{"boundingVolume":{"box":[0,0,0,1,0,0,0,1,0,0,0,1]},"geometricError":4,"refine":"ADD","transform":[1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1],"contents":[{"uri":"a.gltf","group":7,"boundingVolume":{"sphere":[0,0,0,1]}},{"uri":"b.gltf"}],"children":[{"boundingVolume":{"region":[0,0,1,1,0,10]},"geometricError":0}]}})")}};

    const PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.total_documents, 1U);
    EXPECT_EQ(output.result.total_tiles, 2U);
    EXPECT_EQ(output.result.total_contents, 2U);
    const auto tiles = hierarchyRecords(output, HierarchyRecordType::explicit_tile);
    const auto contents = hierarchyRecords(output, HierarchyRecordType::content);
    ASSERT_EQ(tiles.size(), 2U);
    ASSERT_EQ(contents.size(), 2U);
    ASSERT_TRUE(tiles[0]->transform.has_value());
    EXPECT_EQ(tiles[0]->transform->at(12U), 10.0);
    EXPECT_EQ(tiles[1]->refine, RefineMode::add);
    EXPECT_EQ(contents[0]->content_ordinal, 0U);
    EXPECT_EQ(contents[1]->content_ordinal, 1U);
    EXPECT_NE(contents[0]->record_id, contents[1]->record_id);
    EXPECT_EQ(contents[0]->group_id, std::optional<std::string>("7"));
    EXPECT_EQ(contents[0]->content_bounding_volume_type,
              BoundingVolumeType::sphere);
    EXPECT_EQ(contents[0]->resource_closure_version,
              std::optional<std::string>(kContentResourceClosureVersion));
}

TEST(InspectionPipelineTest, ReusesExternalTilesetAsIndependentDocumentInstances) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"root", bytes(R"({"asset":{"version":"1.1"},"geometricError":8,"root":{"boundingVolume":{"region":[0,0,1,1,0,10]},"geometricError":4,"children":[{"boundingVolume":{"region":[0,0,0.5,0.5,0,10]},"geometricError":2,"content":{"uri":"tiles/child.json"}},{"boundingVolume":{"region":[0.5,0.5,1,1,0,10]},"geometricError":2,"content":{"uri":"tiles/child.json"}}]}})")},
            {"tiles/child.json", bytes(R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"sphere":[0,0,0,1]},"geometricError":0}})")}};

    const PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.outcome, Outcome::succeeded);
    EXPECT_EQ(output.result.total_documents, 3U);
    EXPECT_EQ(output.result.total_tiles, 5U);
    EXPECT_EQ(output.result.total_contents, 2U);
    const auto documents = hierarchyRecords(output, HierarchyRecordType::document);
    ASSERT_EQ(documents.size(), 3U);
    EXPECT_NE(documents[1]->record_id, documents[2]->record_id);
    EXPECT_EQ(documents[1]->resource_object_id,
              documents[2]->resource_object_id);
    EXPECT_EQ(documents[1]->parent_content_ordinal, 0U);
    EXPECT_EQ(documents[2]->parent_content_ordinal, 0U);
}

TEST(InspectionPipelineTest, SupportsLegacyMultipleContentsAdapter) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"a.gltf", bytes(R"({"asset":{"version":"2.0"},"meshes":[]})")},
            {"b.gltf", bytes(R"({"asset":{"version":"2.0"},"meshes":[]})")},
            {"root", bytes(R"({"asset":{"version":"1.0"},"extensionsUsed":["3DTILES_multiple_contents"],"geometricError":1,"root":{"boundingVolume":{"region":[0,0,1,1,0,1]},"geometricError":0,"extensions":{"3DTILES_multiple_contents":{"contents":[{"uri":"a.gltf"},{"uri":"b.gltf"}]}}}})")}};

    const PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.total_contents, 2U);
    EXPECT_EQ(output.result.used_extensions,
              std::vector<std::string>({"3DTILES_multiple_contents"}));
    const auto contents = hierarchyRecords(output, HierarchyRecordType::content);
    ASSERT_EQ(contents.size(), 2U);
    EXPECT_EQ(contents[0]->content_ordinal, 0U);
    EXPECT_EQ(contents[1]->content_ordinal, 1U);
}

TEST(InspectionPipelineTest, MaterializesSparseJsonSubtreeAvailability) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"root", bytes(R"({"asset":{"version":"1.1"},"extensionsUsed":["3DTILES_implicit_tiling"],"geometricError":8,"root":{"boundingVolume":{"region":[0,0,4,4,0,8]},"geometricError":4,"content":{"uri":"tiles/{level}/{x}/{y}.gltf"},"implicitTiling":{"subdivisionScheme":"QUADTREE","subtreeLevels":2,"availableLevels":2,"subtrees":{"uri":"subtrees/{level}/{x}/{y}.subtree"}}}})")},
            {"subtrees/0/0/0.subtree", bytes(R"({"buffers":[{"byteLength":2,"uri":"data:application/octet-stream;base64,FwU="}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":1},{"buffer":0,"byteOffset":1,"byteLength":1}],"tileAvailability":{"bitstream":0,"availableCount":4},"contentAvailability":{"bitstream":1,"availableCount":2},"childSubtreeAvailability":{"constant":0,"availableCount":0}})")},
            {"tiles/0/0/0.gltf", bytes(R"({"asset":{"version":"2.0"},"meshes":[]})")},
            {"tiles/1/1/0.gltf", bytes(R"({"asset":{"version":"2.0"},"meshes":[]})")}};

    const PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.total_subtrees, 1U);
    EXPECT_EQ(output.result.total_tiles, 4U);
    EXPECT_EQ(output.result.total_contents, 2U);
    const auto subtrees = hierarchyRecords(
            output, HierarchyRecordType::implicit_subtree);
    ASSERT_EQ(subtrees.size(), 1U);
    EXPECT_EQ(subtrees[0]->available_tile_count, 4U);
    EXPECT_EQ(subtrees[0]->available_content_count, 2U);
    EXPECT_EQ(output.result.used_extensions,
              std::vector<std::string>({"3DTILES_implicit_tiling"}));
}

TEST(InspectionPipelineTest, ReadsBinarySubtreeAndSkipsUnavailableContent) {
    const std::string subtree_json =
            R"({"buffers":[{"byteLength":1}],"bufferViews":[{"buffer":0,"byteLength":1}],"tileAvailability":{"constant":1,"availableCount":1},"contentAvailability":{"bitstream":0,"availableCount":0},"childSubtreeAvailability":{"constant":0,"availableCount":0}})";
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"root", bytes(R"({"asset":{"version":"1.1"},"extensionsUsed":["3DTILES_implicit_tiling"],"geometricError":1,"root":{"boundingVolume":{"box":[0,0,0,1,0,0,0,1,0,0,0,1]},"geometricError":0,"content":{"uri":"missing/{level}/{x}/{y}.gltf"},"implicitTiling":{"subdivisionScheme":"QUADTREE","subtreeLevels":1,"availableLevels":1,"subtrees":{"uri":"subtrees/{level}/{x}/{y}.subtree"}}}})")},
            {"subtrees/0/0/0.subtree", binarySubtree(subtree_json, {0U})}};

    const PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.total_subtrees, 1U);
    EXPECT_EQ(output.result.total_tiles, 1U);
    EXPECT_EQ(output.result.total_contents, 0U);
    EXPECT_EQ(evidence(output, "subtrees/0/0/0.subtree").detected_kind,
              DetectedKind::subtree_binary);
}

TEST(InspectionPipelineTest, MarksImplicitSubtreeMetadataAsUnsupportedEvidence) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"root", bytes(R"({"asset":{"version":"1.1"},"extensionsUsed":["3DTILES_implicit_tiling"],"geometricError":1,"root":{"boundingVolume":{"box":[0,0,0,1,0,0,0,1,0,0,0,1]},"geometricError":0,"implicitTiling":{"subdivisionScheme":"QUADTREE","subtreeLevels":1,"availableLevels":1,"subtrees":{"uri":"subtrees/{level}/{x}/{y}.subtree"}}}})")},
            {"subtrees/0/0/0.subtree", bytes(R"({"tileAvailability":{"constant":1,"availableCount":1},"childSubtreeAvailability":{"constant":0,"availableCount":0},"subtreeMetadata":{"class":"SensitiveHierarchy"}})")}};

    const PipelineOutput output = inspect(request(), content);
    const auto subtrees = hierarchyRecords(
            output, HierarchyRecordType::implicit_subtree);
    ASSERT_EQ(subtrees.size(), 1U);
    EXPECT_EQ(subtrees[0]->used_extensions,
              std::vector<std::string>({"3DTILES_metadata"}));
    EXPECT_EQ(output.result.used_extensions,
              std::vector<std::string>({"3DTILES_implicit_tiling",
                                        "3DTILES_metadata"}));
}

TEST(InspectionPipelineTest, InventoriesMixedExplicitAndImplicitHierarchy) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"root", bytes(R"({"asset":{"version":"1.1"},"extensionsUsed":["3DTILES_implicit_tiling"],"geometricError":2,"root":{"boundingVolume":{"region":[0,0,2,2,0,2]},"geometricError":1,"children":[{"boundingVolume":{"region":[0,0,1,1,0,2]},"geometricError":0,"implicitTiling":{"subdivisionScheme":"QUADTREE","subtreeLevels":1,"availableLevels":1,"subtrees":{"uri":"subtrees/{level}/{x}/{y}.subtree"}}}]}})")},
            {"subtrees/0/0/0.subtree", bytes(R"({"tileAvailability":{"constant":1,"availableCount":1},"childSubtreeAvailability":{"constant":0,"availableCount":0}})")}};

    const PipelineOutput output = inspect(request(), content);
    EXPECT_EQ(output.result.total_tiles, 2U);
    EXPECT_EQ(output.result.total_subtrees, 1U);
    EXPECT_EQ(output.result.total_contents, 0U);
    const auto explicit_tiles = hierarchyRecords(
            output, HierarchyRecordType::explicit_tile);
    const auto implicit_tiles = hierarchyRecords(
            output, HierarchyRecordType::implicit_tile);
    ASSERT_EQ(explicit_tiles.size(), 1U);
    ASSERT_EQ(implicit_tiles.size(), 1U);
    EXPECT_EQ(implicit_tiles[0]->parent_tile_id,
              explicit_tiles[0]->tile_id);
}

TEST(InspectionPipelineTest, EnforcesAvailableTileTraversalLimit) {
    const std::map<std::string, std::vector<std::uint8_t>> content = {
            {"root", bytes(R"({"asset":{"version":"1.1"},"extensionsUsed":["3DTILES_implicit_tiling"],"geometricError":2,"root":{"boundingVolume":{"region":[0,0,2,2,0,2]},"geometricError":1,"implicitTiling":{"subdivisionScheme":"QUADTREE","subtreeLevels":2,"availableLevels":2,"subtrees":{"uri":"subtrees/{level}/{x}/{y}.subtree"}}}})")},
            {"subtrees/0/0/0.subtree", bytes(R"({"tileAvailability":{"constant":1,"availableCount":5},"childSubtreeAvailability":{"constant":0,"availableCount":0}})")}};
    PipelineLimits limits;
    limits.maximum_available_tiles = 4U;

    try {
        static_cast<void>(inspect(request(), content, limits));
        FAIL() << "availability traversal should be bounded";
    } catch (const InspectionPipelineError& error) {
        EXPECT_EQ(error.kind(), PipelineFailureKind::limit_exceeded);
    }
}

TEST(InspectionPipelineTest, RejectsExternalAndEncodedSeparatorUris) {
    const auto rootWithUri = [](const std::string& uri) {
        return bytes(std::string(
                R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0,"content":{"uri":")")
                + uri + R"("}}})");
    };
    for (const std::string& uri : {"https://example.invalid/tile", "a%2fb"}) {
        const std::map<std::string, std::vector<std::uint8_t>> content = {
                {"root", rootWithUri(uri)}};
        EXPECT_THROW(static_cast<void>(inspect(request(), content)),
                     InspectionPipelineError);
    }
}

TEST(InspectionPipelineTest, RejectsMissingResourcesAndCycles) {
    const std::map<std::string, std::vector<std::uint8_t>> missing = {
            {"root", bytes(R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0,"content":{"uri":"missing"}}})")}};
    EXPECT_THROW(static_cast<void>(inspect(request(), missing)),
                 InspectionPipelineError);

    const std::map<std::string, std::vector<std::uint8_t>> cycle = {
            {"a", bytes(R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0,"content":{"uri":"b"}}})")},
            {"b", bytes(R"({"asset":{"version":"1.1"},"geometricError":1,"root":{"boundingVolume":{"region":[0,0,0,0,0,0]},"geometricError":0,"content":{"uri":"a"}}})")}};
    EXPECT_THROW(static_cast<void>(inspect(request(), cycle)),
                 InspectionPipelineError);
}

TEST(PackageNamespaceValidatorTest, RejectsNfdAndUnicodeCaseFoldCollision) {
    const std::string nfc = std::string("caf") + "\xc3\xa9" + ".json";
    const std::string nfd = std::string("cafe") + "\xcc\x81" + ".json";
    PackagePathAnalyzer analyzer;
    EXPECT_FALSE(package::hasFlag(
            analyzer.analyze(nfc, 1024U).flags,
            package::PathAnalysisFlag::non_nfc));
    EXPECT_TRUE(package::hasFlag(
            analyzer.analyze(nfd, 1024U).flags,
            package::PathAnalysisFlag::non_nfc));

    PackageEntryEvidence first = entry("one", "strasse.bin", {1U});
    PackageEntryEvidence second = entry(
            "two", std::string("stra") + "\xc3\x9f" + "e.bin", {2U});
    EXPECT_THROW(package::PackageNamespaceValidator{}.validate({first, second}),
                 package::PackageEnumerationError);
}

}  // namespace
}  // namespace clip_worker::inspection::pipeline
