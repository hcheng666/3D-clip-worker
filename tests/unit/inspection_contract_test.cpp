#include "clip_worker/inspection/inspection_contract.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace clip_worker::inspection {
namespace {

constexpr const char* kFixtureNow = "2026-08-17T08:00:00Z";

std::filesystem::path sourcePath(const std::string& relative) {
#ifdef CLIP_WORKER_SOURCE_DIR
    return std::filesystem::path(CLIP_WORKER_SOURCE_DIR) / relative;
#else
    return std::filesystem::current_path() / relative;
#endif
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open Inspector contract fixture");
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

nlohmann::json readFixture(const char* name) {
    return nlohmann::json::parse(readText(sourcePath(
            std::string("tests/fixtures/protocol/inspector-v1/") + name)));
}

std::string sha256Hex(const std::vector<std::uint8_t>& bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0U;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1
        || EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1
        || EVP_DigestFinal_ex(context, digest.data(), &digest_length) != 1) {
        if (context != nullptr) EVP_MD_CTX_free(context);
        throw std::runtime_error("Cannot hash Inspector schema");
    }
    EVP_MD_CTX_free(context);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(digest_length * 2U, '0');
    for (std::size_t index = 0U; index < digest_length; ++index) {
        result[index * 2U] = kHex[digest[index] >> 4U];
        result[index * 2U + 1U] = kHex[digest[index] & 0x0FU];
    }
    return result;
}

TEST(InspectionContractTest, ValidatesCompletePositiveExchangeAndCanonicalHashes) {
    const auto fixture = readFixture("positive-contract.json");
    const auto request = parseInspectionRequest(fixture.at("inspectionRequest").dump());
    const auto source_page = parseSourceManifestPage(fixture.at("sourceManifestPage").dump());
    const auto result = parseInspectionResult(fixture.at("inspectionResult").dump());
    const auto result_page = parseResultPage(fixture.at("resultPage").dump());
    const auto hierarchy_page = parseHierarchyPage(
            fixture.at("hierarchyPage").dump());

    EXPECT_NO_THROW(validateCompletedExchange(request, {source_page}, result,
                                              {result_page}, {hierarchy_page},
                                              kFixtureNow));
    EXPECT_EQ(source_page.page_sha256, canonicalPageSha256(source_page.records));
    EXPECT_EQ(result_page.page_sha256, canonicalPageSha256(result_page.records));
    EXPECT_EQ(hierarchy_page.page_sha256,
              canonicalPageSha256(hierarchy_page.records));
}

TEST(InspectionContractTest, RejectsVersionHashMismatchAndSensitiveDiagnostic) {
    const auto negative = readFixture("negative-contract.json");
    const auto positive = readFixture("positive-contract.json");
    const auto unsupported = parseInspectionRequest(
            negative.at("unsupportedProtocolRequest").dump());
    EXPECT_THROW(validateRequest(unsupported, kFixtureNow), std::invalid_argument);

    const auto request = parseInspectionRequest(positive.at("inspectionRequest").dump());
    const auto hash_mismatch = parseSourceManifestPage(
            negative.at("hashMismatchManifestPage").dump());
    EXPECT_THROW(validateSourceManifestPage(hash_mismatch, request, kFixtureNow),
                 std::invalid_argument);

    const auto sensitive = parseInspectionResult(
            negative.at("sensitiveDiagnosticResult").dump());
    EXPECT_THROW(validateResult(sensitive, request), std::invalid_argument);
}

TEST(InspectionContractTest, RejectsUnknownFieldsAndNonNormalizedRootHint) {
    auto fixture = readFixture("positive-contract.json");
    fixture["inspectionRequest"]["bucketName"] = "must-not-be-accepted";
    EXPECT_THROW(static_cast<void>(parseInspectionRequest(
                         fixture.at("inspectionRequest").dump())),
                 std::invalid_argument);

    fixture = readFixture("positive-contract.json");
    fixture["inspectionRequest"]["selectedRootHint"] = "../tileset.json";
    const auto request = parseInspectionRequest(fixture.at("inspectionRequest").dump());
    EXPECT_THROW(validateRequest(request, kFixtureNow), std::invalid_argument);
}

TEST(InspectionContractTest, RejectsResultFromDifferentToolBuild) {
    const auto fixture = readFixture("positive-contract.json");
    const auto request = parseInspectionRequest(fixture.at("inspectionRequest").dump());
    auto result = parseInspectionResult(fixture.at("inspectionResult").dump());
    result.tool_versions.front().version = "different-build";

    EXPECT_THROW(validateResult(result, request), std::invalid_argument);
}

TEST(InspectionContractTest, SchemaDigestAndCredentialBoundaryAreStable) {
    std::string schema = readText(sourcePath("config/inspector-protocol-v1.schema.json"));
    std::string normalized;
    normalized.reserve(schema.size());
    for (std::size_t index = 0U; index < schema.size(); ++index) {
        if (schema[index] == '\r') {
            if (index + 1U < schema.size() && schema[index + 1U] == '\n') continue;
            normalized.push_back('\n');
        } else {
            normalized.push_back(schema[index]);
        }
    }
    const std::vector<std::uint8_t> bytes(normalized.begin(), normalized.end());

    EXPECT_EQ(kSchemaSha256, sha256Hex(bytes));
    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    EXPECT_EQ(std::string::npos, lower.find("accesskey"));
    EXPECT_EQ(std::string::npos, lower.find("secretkey"));
    EXPECT_EQ(std::string::npos, lower.find("sessioncredential"));
    EXPECT_EQ(std::string::npos, normalized.find("bucketName"));
}

}  // namespace
}  // namespace clip_worker::inspection
