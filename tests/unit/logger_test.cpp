#include "clip_worker/logging/logger.hpp"
#include "clip_worker/task/worker_runtime.hpp"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace clip_worker::logging {
namespace {

constexpr int kThreadCount = 4;
constexpr int kEventsPerThread = 25;

TEST(LoggerTest, ParsesSupportedLevelsCaseInsensitively) {
    EXPECT_EQ(parseLogLevel("debug"), LogLevel::debug);
    EXPECT_EQ(parseLogLevel("INFO"), LogLevel::info);
    EXPECT_EQ(parseLogLevel("Warn"), LogLevel::warning);
    EXPECT_EQ(parseLogLevel("error"), LogLevel::error);
    EXPECT_THROW(static_cast<void>(parseLogLevel("trace")), std::invalid_argument);
}

TEST(LoggerTest, WritesStructuredJsonWithUtcMillisecondTimestamp) {
    std::ostringstream output;
    const Logger logger(LogLevel::info, output);

    logger.info("task.succeeded", "Clip task completed",
                {{"workerId", "worker-1"}, {"assetId", "asset-1"},
                 {"durationMs", 42}});

    std::string line = output.str();
    ASSERT_EQ(std::count(line.begin(), line.end(), '\n'), 1);
    line.pop_back();
    const auto json = nlohmann::json::parse(line);
    EXPECT_EQ(json.at("level"), "INFO");
    EXPECT_EQ(json.at("event"), "task.succeeded");
    EXPECT_EQ(json.at("message"), "Clip task completed");
    EXPECT_EQ(json.at("workerId"), "worker-1");
    EXPECT_EQ(json.at("assetId"), "asset-1");
    EXPECT_EQ(json.at("durationMs"), 42);
    EXPECT_TRUE(std::regex_match(
            json.at("timestamp").get<std::string>(),
            std::regex(
                    R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$)")));
}

TEST(LoggerTest, FiltersEventsBelowConfiguredLevel) {
    std::ostringstream output;
    const Logger logger(LogLevel::warning, output);

    logger.debug("claim.empty", "No task");
    logger.info("worker.started", "Started");
    logger.warning("claim.failed", "Temporary failure");
    logger.error("task.failed", "Permanent failure");

    std::istringstream lines(output.str());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(lines, line)));
    EXPECT_EQ(nlohmann::json::parse(line).at("event"), "claim.failed");
    ASSERT_TRUE(static_cast<bool>(std::getline(lines, line)));
    EXPECT_EQ(nlohmann::json::parse(line).at("event"), "task.failed");
    EXPECT_FALSE(static_cast<bool>(std::getline(lines, line)));
}

TEST(LoggerTest, EscapesUntrustedMessagesIntoOnePhysicalLine) {
    std::ostringstream output;
    const Logger logger(LogLevel::info, output);
    const std::string message = "failed: \"quoted\"\nnext\tcolumn";

    logger.error("task.failed", message, {{"errorMessage", message}});

    const std::string encoded = output.str();
    EXPECT_EQ(std::count(encoded.begin(), encoded.end(), '\n'), 1);
    const auto json = nlohmann::json::parse(encoded);
    EXPECT_EQ(json.at("message"), message);
    EXPECT_EQ(json.at("errorMessage"), message);
}

TEST(LoggerTest, PreservesCompleteJsonEventsDuringConcurrentWrites) {
    std::ostringstream output;
    const Logger logger(LogLevel::debug, output);
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
        threads.emplace_back([&logger, thread_index] {
            for (int event_index = 0; event_index < kEventsPerThread; ++event_index) {
                logger.debug("heartbeat.succeeded", "Heartbeat succeeded",
                             {{"thread", thread_index}, {"eventIndex", event_index}});
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::istringstream lines(output.str());
    std::string line;
    int event_count = 0;
    while (std::getline(lines, line)) {
        ASSERT_FALSE(line.empty());
        EXPECT_EQ(nlohmann::json::parse(line).at("event"), "heartbeat.succeeded");
        ++event_count;
    }
    EXPECT_EQ(event_count, kThreadCount * kEventsPerThread);
}

TEST(LoggerTest, WritesOneRedactedB3dmCompatibilityWarning) {
    std::ostringstream output;
    const Logger logger(LogLevel::info, output);
    formats::B3dmLayoutDiagnostics diagnostics;
    diagnostics.glb_offset = 92U;
    diagnostics.glb_byte_length = 250044U;
    diagnostics.trailing_padding_bytes = 0U;
    diagnostics.glb_offset_standard_aligned = false;
    diagnostics.tile_length_standard_aligned = true;

    task::logSourceCompatibilityWarning(
            logger, "worker-1", "asset-1", diagnostics);

    std::istringstream lines(output.str());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(lines, line)));
    const auto json = nlohmann::json::parse(line);
    EXPECT_EQ(json.at("level"), "WARN");
    EXPECT_EQ(json.at("event"), "source.compatibility_warning");
    EXPECT_EQ(json.at("compatibilityCode"), "B3DM_NONCONFORMANT_ALIGNMENT");
    EXPECT_EQ(json.at("workerId"), "worker-1");
    EXPECT_EQ(json.at("assetId"), "asset-1");
    EXPECT_EQ(json.at("glbOffset"), 92U);
    EXPECT_EQ(json.at("glbOffsetAlignment"), 4U);
    EXPECT_EQ(json.at("tileLength"), 250136U);
    EXPECT_EQ(json.at("tileLengthAlignment"), 8U);
    EXPECT_FALSE(json.contains("sourceDownloadUrl"));
    EXPECT_FALSE(json.contains("leaseToken"));
    EXPECT_FALSE(json.contains("sourceEtag"));
    EXPECT_FALSE(json.contains("scopeWkbBase64"));
    std::string extra_line;
    EXPECT_FALSE(static_cast<bool>(std::getline(lines, extra_line)));
}

TEST(LoggerTest, OmitsCompatibilityWarningForStandardLayout) {
    std::ostringstream output;
    const Logger logger(LogLevel::debug, output);
    formats::B3dmLayoutDiagnostics diagnostics;
    diagnostics.glb_offset = 96U;
    diagnostics.glb_byte_length = 128U;

    task::logSourceCompatibilityWarning(
            logger, "worker-1", "asset-1", diagnostics);

    EXPECT_TRUE(output.str().empty());
}

TEST(LoggerTest, WritesOneRedactedSamplerWrapRCompatibilityWarning) {
    std::ostringstream output;
    const Logger logger(LogLevel::info, output);
    clip::SamplerCompatibilityDiagnostics diagnostics;
    diagnostics.affected_sampler_count = 3U;
    diagnostics.wrap_r_values = {33071U, 10497U};

    task::logSamplerCompatibilityWarning(
            logger, "worker-1", "asset-1", diagnostics);

    std::istringstream lines(output.str());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(lines, line)));
    const auto json = nlohmann::json::parse(line);
    EXPECT_EQ(json.at("level"), "WARN");
    EXPECT_EQ(json.at("event"), "source.compatibility_warning");
    EXPECT_EQ(json.at("compatibilityCode"),
              "GLTF_NONSTANDARD_SAMPLER_WRAP_R");
    EXPECT_EQ(json.at("affectedSamplerCount"), 3U);
    EXPECT_EQ(json.at("wrapRValues"),
              nlohmann::json::array({33071U, 10497U}));
    EXPECT_FALSE(json.contains("sourceDownloadUrl"));
    EXPECT_FALSE(json.contains("leaseToken"));
    EXPECT_FALSE(json.contains("sourceEtag"));
    EXPECT_FALSE(json.contains("scopeWkbBase64"));
    std::string extra_line;
    EXPECT_FALSE(static_cast<bool>(std::getline(lines, extra_line)));
}

TEST(LoggerTest, OmitsSamplerWarningWhenNoWrapRWasObserved) {
    std::ostringstream output;
    const Logger logger(LogLevel::debug, output);

    task::logSamplerCompatibilityWarning(
            logger, "worker-1", "asset-1", {});

    EXPECT_TRUE(output.str().empty());
}

TEST(LoggerTest, WritesOneRedactedDracoAccessorCompatibilityWarning) {
    std::ostringstream output;
    const Logger logger(LogLevel::info, output);
    clip::DracoCompatibilityDiagnostics diagnostics;
    diagnostics.affected_primitive_count = 1U;
    diagnostics.affected_accessor_count = 2U;
    diagnostics.affected_index_count = 1U;
    diagnostics.maximum_declared_vertex_count = 5996U;
    diagnostics.maximum_decoded_point_count = 6251U;
    diagnostics.maximum_declared_index_count = 12606U;
    diagnostics.maximum_decoded_index_count = 12603U;

    task::logDracoCompatibilityWarning(
            logger, "worker-1", "asset-1", diagnostics);

    std::istringstream lines(output.str());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(lines, line)));
    const auto json = nlohmann::json::parse(line);
    EXPECT_EQ(json.at("level"), "WARN");
    EXPECT_EQ(json.at("event"), "source.compatibility_warning");
    EXPECT_EQ(json.at("compatibilityCode"),
              "GLTF_STALE_DRACO_ACCESSOR_COUNT");
    EXPECT_EQ(json.at("affectedPrimitiveCount"), 1U);
    EXPECT_EQ(json.at("affectedAccessorCount"), 2U);
    EXPECT_EQ(json.at("affectedIndexCount"), 1U);
    EXPECT_EQ(json.at("maximumDeclaredVertexCount"), 5996U);
    EXPECT_EQ(json.at("maximumDecodedPointCount"), 6251U);
    EXPECT_EQ(json.at("maximumDeclaredIndexCount"), 12606U);
    EXPECT_EQ(json.at("maximumDecodedIndexCount"), 12603U);
    EXPECT_FALSE(json.contains("sourceDownloadUrl"));
    EXPECT_FALSE(json.contains("leaseToken"));
    EXPECT_FALSE(json.contains("sourceEtag"));
    EXPECT_FALSE(json.contains("scopeWkbBase64"));
    std::string extra_line;
    EXPECT_FALSE(static_cast<bool>(std::getline(lines, extra_line)));
}

TEST(LoggerTest, OmitsDracoWarningWhenCountsMatch) {
    std::ostringstream output;
    const Logger logger(LogLevel::debug, output);

    task::logDracoCompatibilityWarning(
            logger, "worker-1", "asset-1", {});

    EXPECT_TRUE(output.str().empty());
}

}  // namespace
}  // namespace clip_worker::logging
