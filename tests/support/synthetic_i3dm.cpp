#include "support/synthetic_i3dm.hpp"

#include <utility>

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
    while (result.size() % 8U != 0U) result.push_back(' ');
    return result;
}

std::vector<std::uint8_t> featureBinary() {
    std::vector<std::uint8_t> bytes;
    for (const float value : {0.0F, 0.0F, 0.0F,
                              10.0F, 20.0F, 30.0F}) appendValue(bytes, value);
    for (const float value : {0.0F, 0.0F, 1.0F,
                              0.0F, 0.0F, 1.0F}) appendValue(bytes, value);
    for (const float value : {1.0F, 0.0F, 0.0F,
                              1.0F, 0.0F, 0.0F}) appendValue(bytes, value);
    for (const float value : {2.0F, 0.5F}) appendValue(bytes, value);
    for (const float value : {1.0F, 2.0F, 3.0F,
                              4.0F, 5.0F, 6.0F}) appendValue(bytes, value);
    bytes.insert(bytes.end(), {0U, 1U});
    while (bytes.size() % 8U != 0U) bytes.push_back(0U);
    return bytes;
}

std::string featureJson() {
    return paddedJson({
            {"INSTANCES_LENGTH", 2U},
            {"POSITION", {{"byteOffset", 0U}}},
            {"NORMAL_UP", {{"byteOffset", 24U}}},
            {"NORMAL_RIGHT", {{"byteOffset", 48U}}},
            {"SCALE", {{"byteOffset", 72U}}},
            {"SCALE_NON_UNIFORM", {{"byteOffset", 80U}}},
            {"BATCH_ID", {{"byteOffset", 104U},
                          {"componentType", "UNSIGNED_BYTE"}}},
            {"BATCH_LENGTH", 2U},
            {"RTC_CENTER", {100.0, 200.0, 300.0}}});
}

std::string batchJson() {
    return paddedJson({{"name", {"oak", "pine"}},
                       {"rank", {7.0, 9.0}}});
}

std::vector<std::uint8_t> buildWithTables(
        std::uint32_t gltf_format, const std::vector<std::uint8_t>& payload,
        std::string feature_json, std::vector<std::uint8_t> feature_binary,
        std::string batch_json) {
    std::vector<std::uint8_t> bytes{'i', '3', 'd', 'm'};
    appendU32(bytes, 1U);
    appendU32(bytes, 0U);
    appendU32(bytes, static_cast<std::uint32_t>(feature_json.size()));
    appendU32(bytes, static_cast<std::uint32_t>(feature_binary.size()));
    appendU32(bytes, static_cast<std::uint32_t>(batch_json.size()));
    appendU32(bytes, 0U);
    appendU32(bytes, gltf_format);
    bytes.insert(bytes.end(), feature_json.begin(), feature_json.end());
    bytes.insert(bytes.end(), feature_binary.begin(), feature_binary.end());
    bytes.insert(bytes.end(), batch_json.begin(), batch_json.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        bytes[8U + byte] = static_cast<std::uint8_t>(
                (length >> (byte * 8U)) & 0xffU);
    }
    return bytes;
}

std::vector<std::uint8_t> build(std::uint32_t gltf_format,
                                const std::vector<std::uint8_t>& payload) {
    return buildWithTables(gltf_format, payload, featureJson(),
                           featureBinary(), batchJson());
}

}  // namespace

std::vector<std::uint8_t> makeEmbeddedI3dmFixture(
        const std::vector<std::uint8_t>& model_glb) {
    return build(1U, model_glb);
}

std::vector<std::uint8_t> makeExternalI3dmFixture(
        const std::string& model_uri) {
    std::vector<std::uint8_t> payload(model_uri.begin(), model_uri.end());
    while (payload.size() % 8U != 0U) payload.push_back(0U);
    return build(0U, payload);
}

std::vector<std::uint8_t> makeEnuOnlyI3dmFixture(
        const std::vector<std::uint8_t>& model_glb) {
    std::vector<std::uint8_t> binary;
    for (const float value : {1.0F, 2.0F, 3.0F}) appendValue(binary, value);
    while (binary.size() % 8U != 0U) binary.push_back(0U);
    return buildWithTables(
            1U, model_glb,
            paddedJson({{"EAST_NORTH_UP", true},
                        {"INSTANCES_LENGTH", 1U},
                        {"POSITION", {{"byteOffset", 0U}}}}),
            std::move(binary), {});
}

std::vector<std::uint8_t> makeOctOrientedI3dmFixture(
        const std::vector<std::uint8_t>& model_glb) {
    std::vector<std::uint8_t> binary;
    for (const float value : {1.0F, 2.0F, 3.0F}) appendValue(binary, value);
    for (const std::uint16_t value : {32768U, 32768U}) {
        appendValue(binary, value);
    }
    for (const std::uint16_t value : {65535U, 32768U}) {
        appendValue(binary, value);
    }
    while (binary.size() % 8U != 0U) binary.push_back(0U);
    return buildWithTables(
            1U, model_glb,
            paddedJson({{"INSTANCES_LENGTH", 1U},
                        {"NORMAL_RIGHT_OCT32P", {{"byteOffset", 16U}}},
                        {"NORMAL_UP_OCT32P", {{"byteOffset", 12U}}},
                        {"POSITION", {{"byteOffset", 0U}}}}),
            std::move(binary), {});
}

std::vector<std::uint8_t> makePerInstanceMetadataI3dmFixture(
        const std::vector<std::uint8_t>& model_glb) {
    return buildWithTables(
            1U, model_glb,
            paddedJson({{"INSTANCES_LENGTH", 2U},
                        {"NORMAL_RIGHT", {{"byteOffset", 48U}}},
                        {"NORMAL_UP", {{"byteOffset", 24U}}},
                        {"POSITION", {{"byteOffset", 0U}}}}),
            featureBinary(), batchJson());
}

}  // namespace clip_worker::tests
