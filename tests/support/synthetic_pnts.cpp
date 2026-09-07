#include "support/synthetic_pnts.hpp"

#include <nlohmann/json.hpp>

namespace clip_worker::tests {
namespace {

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        bytes.push_back(static_cast<std::uint8_t>(
                (value >> (byte * 8U)) & 0xffU));
    }
}

template <typename T>
void appendValue(std::vector<std::uint8_t>& bytes, T value) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

std::string paddedJson(const nlohmann::json& value) {
    std::string result = value.dump();
    while (result.size() % 4U != 0U) result.push_back(' ');
    return result;
}

}  // namespace

SyntheticPnts makeQuantizedPntsFixture() {
    std::vector<std::uint8_t> feature_binary;
    for (const std::uint16_t value : {0U, 0U, 0U, 65535U, 32768U, 65535U}) {
        appendValue(feature_binary, value);
    }
    feature_binary.insert(feature_binary.end(),
                          {255U, 0U, 0U, 0U, 255U, 0U});
    feature_binary.insert(feature_binary.end(), {128U, 128U, 255U, 128U});
    feature_binary.insert(feature_binary.end(), {0U, 1U});

    nlohmann::json feature{
            {"POINTS_LENGTH", 2U},
            {"POSITION_QUANTIZED", {{"byteOffset", 0U}}},
            {"QUANTIZED_VOLUME_OFFSET", {10.0, 20.0, 30.0}},
            {"QUANTIZED_VOLUME_SCALE", {100.0, 200.0, 300.0}},
            {"RGB", {{"byteOffset", 12U}}},
            {"NORMAL_OCT16P", {{"byteOffset", 18U}}},
            {"BATCH_ID", {{"byteOffset", 22U},
                          {"componentType", "UNSIGNED_BYTE"}}},
            {"BATCH_LENGTH", 2U},
            {"RTC_CENTER", {1.0, 2.0, 3.0}}};
    const std::string feature_json = paddedJson(feature);
    const std::string batch_json = paddedJson(
            {{"name", {"first", "second"}}, {"height", {5.0, 9.0}}});

    std::vector<std::uint8_t> bytes{'p', 'n', 't', 's'};
    appendU32(bytes, 1U);
    appendU32(bytes, 0U);
    appendU32(bytes, static_cast<std::uint32_t>(feature_json.size()));
    appendU32(bytes, static_cast<std::uint32_t>(feature_binary.size()));
    appendU32(bytes, static_cast<std::uint32_t>(batch_json.size()));
    appendU32(bytes, 0U);
    bytes.insert(bytes.end(), feature_json.begin(), feature_json.end());
    bytes.insert(bytes.end(), feature_binary.begin(), feature_binary.end());
    bytes.insert(bytes.end(), batch_json.begin(), batch_json.end());
    const std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        bytes[8U + byte] = static_cast<std::uint8_t>(
                (length >> (byte * 8U)) & 0xffU);
    }
    return {std::move(bytes), feature_json};
}

SyntheticPnts makePerPointPropertyPntsFixture() {
    std::vector<std::uint8_t> feature_binary;
    for (const float value : {1.0F, 2.0F, 3.0F,
                              4.0F, 5.0F, 6.0F}) {
        appendValue(feature_binary, value);
    }
    const std::string feature_json = paddedJson(
            {{"POINTS_LENGTH", 2U},
             {"POSITION", {{"byteOffset", 0U}}}});
    const std::string batch_json = paddedJson(
            {{"name", {"first", "second"}},
             {"height", {5.0, 9.0}}});
    std::vector<std::uint8_t> bytes{'p', 'n', 't', 's'};
    appendU32(bytes, 1U);
    appendU32(bytes, 0U);
    appendU32(bytes, static_cast<std::uint32_t>(feature_json.size()));
    appendU32(bytes, static_cast<std::uint32_t>(feature_binary.size()));
    appendU32(bytes, static_cast<std::uint32_t>(batch_json.size()));
    appendU32(bytes, 0U);
    bytes.insert(bytes.end(), feature_json.begin(), feature_json.end());
    bytes.insert(bytes.end(), feature_binary.begin(), feature_binary.end());
    bytes.insert(bytes.end(), batch_json.begin(), batch_json.end());
    const std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        bytes[8U + byte] = static_cast<std::uint8_t>(
                (length >> (byte * 8U)) & 0xffU);
    }
    return {std::move(bytes), feature_json};
}

}  // namespace clip_worker::tests
