#pragma once

#include "clip_worker/task/task_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clip_worker::tests {

[[nodiscard]] std::vector<std::uint8_t> makeMinimalGlb();
[[nodiscard]] std::vector<std::uint8_t> makeMinimalB3dm(
        const std::string& feature_table_json = R"({"BATCH_LENGTH":1})");
[[nodiscard]] std::vector<std::uint8_t> makeCompatibleMisalignedB3dm();

struct TexturedMeshFixture {
    std::vector<std::uint8_t> b3dm;
    task::ClaimTask task;
};

/** Creates a small ECEF-located triangle crossing an EPSG:4490 square scope. */
[[nodiscard]] TexturedMeshFixture makeTexturedMeshFixture(
        const std::string& additional_extension = "",
        bool include_rtc_center = false,
        const std::string& sampler_fields_json = "");
void makeB3dmLayoutCompatibleButNonconforming(std::vector<std::uint8_t>& bytes);
void writeUint32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value);

}  // namespace clip_worker::tests
