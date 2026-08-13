#include "support/synthetic_tile.hpp"

#include "clip_worker/formats/b3dm.hpp"
#include "clip_worker/formats/glb.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <webp/encode.h>

namespace clip_worker::tests {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kTextureWidth = 8U;
constexpr std::size_t kTextureHeight = 8U;
constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kGlTriangles = 4U;
constexpr std::uint32_t kGlFloat = 5126U;
constexpr std::uint32_t kGlArrayBuffer = 34962U;
constexpr std::uint32_t kGlClampToEdge = 33071U;

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void appendText(std::vector<std::uint8_t>& bytes, const std::string& value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void appendDouble(std::vector<std::uint8_t>& bytes, double value) {
    std::array<std::uint8_t, sizeof(value)> encoded{};
    std::memcpy(encoded.data(), &value, sizeof(value));
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

void appendFloats(std::vector<std::uint8_t>& bytes,
                  const std::vector<float>& values) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(values.data());
    bytes.insert(bytes.end(), first, first + values.size() * sizeof(float));
}

void pad(std::vector<std::uint8_t>& bytes, std::size_t alignment, std::uint8_t value) {
    while (bytes.size() % alignment != 0U) {
        bytes.push_back(value);
    }
}

std::vector<std::uint8_t> makeB3dm(const std::vector<std::uint8_t>& glb,
                                   const std::string& feature_table_json,
                                   const std::string& batch_table_json = "") {
    constexpr std::array<std::uint8_t, 4> magic = {'b', '3', 'd', 'm'};
    std::vector<std::uint8_t> bytes(clip_worker::formats::B3dmParser::kHeaderSize, 0U);
    std::copy(magic.begin(), magic.end(), bytes.begin());

    appendText(bytes, feature_table_json);
    pad(bytes, clip_worker::formats::B3dmParser::kGlbAlignment,
        static_cast<std::uint8_t>(' '));
    const std::size_t feature_table_length = bytes.size()
            - clip_worker::formats::B3dmParser::kHeaderSize;

    const std::size_t batch_table_start = bytes.size();
    appendText(bytes, batch_table_json);
    if (!batch_table_json.empty()) {
        pad(bytes, clip_worker::formats::B3dmParser::kGlbAlignment,
            static_cast<std::uint8_t>(' '));
    }
    const std::size_t batch_table_length = bytes.size() - batch_table_start;
    bytes.insert(bytes.end(), glb.begin(), glb.end());
    pad(bytes, clip_worker::formats::B3dmParser::kGlbAlignment, 0U);

    writeUint32(bytes, 4, clip_worker::formats::B3dmParser::kSupportedVersion);
    writeUint32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    writeUint32(bytes, 12, static_cast<std::uint32_t>(feature_table_length));
    writeUint32(bytes, 20, static_cast<std::uint32_t>(batch_table_length));
    return bytes;
}

std::vector<std::uint8_t> encodeTexture() {
    std::vector<std::uint8_t> rgba(kTextureWidth * kTextureHeight * 4U);
    for (std::size_t pixel = 0; pixel < kTextureWidth * kTextureHeight; ++pixel) {
        rgba[pixel * 4U] = static_cast<std::uint8_t>(32U + pixel);
        rgba[pixel * 4U + 1U] = 96U;
        rgba[pixel * 4U + 2U] = 192U;
        rgba[pixel * 4U + 3U] = 255U;
    }
    std::uint8_t* encoded = nullptr;
    const std::size_t size = WebPEncodeLosslessRGBA(
            rgba.data(), static_cast<int>(kTextureWidth),
            static_cast<int>(kTextureHeight), static_cast<int>(kTextureWidth * 4U),
            &encoded);
    if (size == 0U || encoded == nullptr) {
        throw std::runtime_error("Unable to encode synthetic WebP fixture");
    }
    std::vector<std::uint8_t> result(encoded, encoded + size);
    WebPFree(encoded);
    return result;
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
    static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
        const std::uint32_t first = bytes[offset];
        const std::uint32_t second = offset + 1U < bytes.size() ? bytes[offset + 1U] : 0U;
        const std::uint32_t third = offset + 2U < bytes.size() ? bytes[offset + 2U] : 0U;
        const std::uint32_t value = (first << 16U) | (second << 8U) | third;
        result.push_back(alphabet[(value >> 18U) & 0x3FU]);
        result.push_back(alphabet[(value >> 12U) & 0x3FU]);
        result.push_back(offset + 1U < bytes.size()
                                 ? alphabet[(value >> 6U) & 0x3FU] : '=');
        result.push_back(offset + 2U < bytes.size() ? alphabet[value & 0x3FU] : '=');
    }
    return result;
}

std::vector<std::uint8_t> squareScopeWkb() {
    constexpr double center_longitude = 120.0;
    constexpr double center_latitude = 30.0;
    constexpr double half_span_degrees = 0.00005;
    std::vector<std::uint8_t> bytes;
    bytes.push_back(1U);
    appendUint32(bytes, 3U);
    appendUint32(bytes, 1U);
    appendUint32(bytes, 5U);
    for (const std::array<double, 2> point : {
                 std::array<double, 2>{center_longitude - half_span_degrees,
                                       center_latitude - half_span_degrees},
                 std::array<double, 2>{center_longitude + half_span_degrees,
                                       center_latitude - half_span_degrees},
                 std::array<double, 2>{center_longitude + half_span_degrees,
                                       center_latitude + half_span_degrees},
                 std::array<double, 2>{center_longitude - half_span_degrees,
                                       center_latitude + half_span_degrees},
                 std::array<double, 2>{center_longitude - half_span_degrees,
                                       center_latitude - half_span_degrees}}) {
        appendDouble(bytes, point[0]);
        appendDouble(bytes, point[1]);
    }
    return bytes;
}

std::array<double, task::ClaimTask::kWorldTransformElementCount> localEnuTransform() {
    constexpr double longitude_degrees = 120.0;
    constexpr double latitude_degrees = 30.0;
    constexpr double semi_major_axis = 6378137.0;
    constexpr double inverse_flattening = 298.257222101;
    const double longitude = longitude_degrees * kPi / 180.0;
    const double latitude = latitude_degrees * kPi / 180.0;
    const double flattening = 1.0 / inverse_flattening;
    const double eccentricity_squared = flattening * (2.0 - flattening);
    const double sin_latitude = std::sin(latitude);
    const double cos_latitude = std::cos(latitude);
    const double sin_longitude = std::sin(longitude);
    const double cos_longitude = std::cos(longitude);
    const double prime_vertical = semi_major_axis
            / std::sqrt(1.0 - eccentricity_squared * sin_latitude * sin_latitude);
    const double ecef_x = prime_vertical * cos_latitude * cos_longitude;
    const double ecef_y = prime_vertical * cos_latitude * sin_longitude;
    const double ecef_z = prime_vertical * (1.0 - eccentricity_squared) * sin_latitude;

    // Column-major matrix mapping local east/north/up meters to CGCS2000 ECEF.
    return {
            -sin_longitude, cos_longitude, 0.0, 0.0,
            -sin_latitude * cos_longitude, -sin_latitude * sin_longitude,
            cos_latitude, 0.0,
            cos_latitude * cos_longitude, cos_latitude * sin_longitude,
            sin_latitude, 0.0,
            ecef_x, ecef_y, ecef_z, 1.0};
}

std::vector<std::uint8_t> makeTexturedMeshGlb(
        const std::string& additional_extension,
        const std::string& sampler_fields_json) {
    const std::vector<float> positions = {
            -10.0F, -4.0F, 0.0F,
            10.0F, -4.0F, 0.0F,
            0.0F, 10.0F, 0.0F};
    const std::vector<float> texture_coordinates = {
            0.0F, 0.0F,
            1.0F, 0.0F,
            0.5F, 1.0F};
    std::vector<std::uint8_t> binary;
    appendFloats(binary, positions);
    const std::size_t texture_coordinate_offset = binary.size();
    appendFloats(binary, texture_coordinates);
    const std::size_t image_offset = binary.size();
    const auto image = encodeTexture();
    binary.insert(binary.end(), image.begin(), image.end());
    const std::size_t buffer_length = binary.size();
    pad(binary, clip_worker::formats::GlbParser::kChunkAlignment, 0U);

    Json extensions_used = Json::array({"KHR_materials_unlit", "EXT_texture_webp"});
    if (!additional_extension.empty()) {
        extensions_used.push_back(additional_extension);
    }
    Json document;
    document["asset"] = {{"version", "2.0"}, {"generator", "synthetic"}};
    document["scene"] = 0U;
    document["scenes"] = Json::array({
            {{"nodes", Json::array({0U})}, {"name", "must-not-survive"}}});
    document["nodes"] = Json::array({
            {{"mesh", 0U}, {"name", "must-not-survive"}}});
    document["meshes"] = Json::array({
            {{"primitives", Json::array({
                    {{"attributes", {{"POSITION", 0U}, {"TEXCOORD_0", 1U}}},
                     {"material", 0U}, {"mode", kGlTriangles}}})}}});
    document["materials"] = Json::array({{
            {"pbrMetallicRoughness",
             {{"baseColorTexture", {{"index", 0U}}}}},
            {"extensions", {{"KHR_materials_unlit", Json::object()}}}}});
    document["textures"] = Json::array({{
            {"extensions", {{"EXT_texture_webp", {{"source", 0U}}}}},
            {"sampler", 0U}}});
    Json sampler = {{"wrapS", kGlClampToEdge}, {"wrapT", kGlClampToEdge}};
    if (!sampler_fields_json.empty()) {
        const Json additional_sampler_fields = Json::parse(sampler_fields_json);
        if (!additional_sampler_fields.is_object()) {
            throw std::invalid_argument("sampler_fields_json must contain an object");
        }
        for (auto field = additional_sampler_fields.cbegin();
             field != additional_sampler_fields.cend(); ++field) {
            sampler[field.key()] = field.value();
        }
    }
    document["samplers"] = Json::array({std::move(sampler)});
    document["images"] = Json::array({
            {{"bufferView", 2U}, {"mimeType", "image/webp"}}});
    document["bufferViews"] = Json::array({
            {{"buffer", 0U}, {"byteOffset", 0U},
             {"byteLength", positions.size() * sizeof(float)},
             {"target", kGlArrayBuffer}},
            {{"buffer", 0U}, {"byteOffset", texture_coordinate_offset},
             {"byteLength", texture_coordinates.size() * sizeof(float)},
             {"target", kGlArrayBuffer}},
            {{"buffer", 0U}, {"byteOffset", image_offset},
             {"byteLength", image.size()}}});
    document["accessors"] = Json::array({
            {{"bufferView", 0U}, {"componentType", kGlFloat},
             {"count", 3U}, {"type", "VEC3"}},
            {{"bufferView", 1U}, {"componentType", kGlFloat},
             {"count", 3U}, {"type", "VEC2"}}});
    document["buffers"] = Json::array({{{"byteLength", buffer_length}}});
    document["extensionsUsed"] = std::move(extensions_used);
    document["extensionsRequired"] = Json::array({"EXT_texture_webp"});

    std::string json_text = document.dump();
    while (json_text.size() % clip_worker::formats::GlbParser::kChunkAlignment != 0U) {
        json_text.push_back(' ');
    }
    std::vector<std::uint8_t> glb = {'g', 'l', 'T', 'F'};
    appendUint32(glb, clip_worker::formats::GlbParser::kSupportedVersion);
    appendUint32(glb, 0U);
    appendUint32(glb, static_cast<std::uint32_t>(json_text.size()));
    appendUint32(glb, clip_worker::formats::GlbParser::kJsonChunkType);
    appendText(glb, json_text);
    appendUint32(glb, static_cast<std::uint32_t>(binary.size()));
    appendUint32(glb, clip_worker::formats::GlbParser::kBinaryChunkType);
    glb.insert(glb.end(), binary.begin(), binary.end());
    writeUint32(glb, 8U, static_cast<std::uint32_t>(glb.size()));
    return glb;
}

}  // namespace

std::vector<std::uint8_t> makeMinimalGlb() {
    constexpr std::array<std::uint8_t, 4> magic = {'g', 'l', 'T', 'F'};
    const std::string asset_json = R"({"asset":{"version":"2.0"}})";

    std::vector<std::uint8_t> json_bytes(asset_json.begin(), asset_json.end());
    pad(json_bytes, clip_worker::formats::GlbParser::kChunkAlignment,
        static_cast<std::uint8_t>(' '));

    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    appendUint32(bytes, clip_worker::formats::GlbParser::kSupportedVersion);
    appendUint32(bytes, 0U);
    appendUint32(bytes, static_cast<std::uint32_t>(json_bytes.size()));
    appendUint32(bytes, clip_worker::formats::GlbParser::kJsonChunkType);
    bytes.insert(bytes.end(), json_bytes.begin(), json_bytes.end());
    writeUint32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    return bytes;
}

std::vector<std::uint8_t> makeMinimalB3dm(const std::string& feature_table_json) {
    return makeB3dm(makeMinimalGlb(), feature_table_json);
}

std::vector<std::uint8_t> makeCompatibleMisalignedB3dm() {
    auto bytes = makeMinimalB3dm();
    makeB3dmLayoutCompatibleButNonconforming(bytes);
    return bytes;
}

TexturedMeshFixture makeTexturedMeshFixture(
        const std::string& additional_extension, bool include_rtc_center,
        const std::string& sampler_fields_json) {
    TexturedMeshFixture fixture;
    const std::string feature_table = include_rtc_center
            ? R"({"BATCH_LENGTH":1,"RTC_CENTER":[0,0,0]})"
            : R"({"BATCH_LENGTH":1})";
    fixture.b3dm = makeB3dm(
            makeTexturedMeshGlb(additional_extension, sampler_fields_json), feature_table,
            R"({"name":["synthetic-feature"]})");
    fixture.task.scope_wkb_base64 = base64Encode(squareScopeWkb());
    fixture.task.scope_srid = 4490;
    fixture.task.world_transform = localEnuTransform();
    return fixture;
}

void makeB3dmLayoutCompatibleButNonconforming(std::vector<std::uint8_t>& bytes) {
    const std::uint32_t feature_length =
            static_cast<std::uint32_t>(bytes[12])
            | (static_cast<std::uint32_t>(bytes[13]) << 8U)
            | (static_cast<std::uint32_t>(bytes[14]) << 16U)
            | (static_cast<std::uint32_t>(bytes[15]) << 24U);
    const std::uint32_t feature_binary_length =
            static_cast<std::uint32_t>(bytes[16])
            | (static_cast<std::uint32_t>(bytes[17]) << 8U)
            | (static_cast<std::uint32_t>(bytes[18]) << 16U)
            | (static_cast<std::uint32_t>(bytes[19]) << 24U);
    const std::uint32_t batch_length =
            static_cast<std::uint32_t>(bytes[20])
            | (static_cast<std::uint32_t>(bytes[21]) << 8U)
            | (static_cast<std::uint32_t>(bytes[22]) << 16U)
            | (static_cast<std::uint32_t>(bytes[23]) << 24U);
    const std::uint32_t batch_binary_length =
            static_cast<std::uint32_t>(bytes[24])
            | (static_cast<std::uint32_t>(bytes[25]) << 8U)
            | (static_cast<std::uint32_t>(bytes[26]) << 16U)
            | (static_cast<std::uint32_t>(bytes[27]) << 24U);
    const std::size_t glb_offset = clip_worker::formats::B3dmParser::kHeaderSize
                                   + feature_length + feature_binary_length
                                   + batch_length + batch_binary_length;
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(glb_offset),
                 clip_worker::formats::B3dmParser::kCompatibleAlignment,
                 static_cast<std::uint8_t>(' '));
    writeUint32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    const std::uint32_t compatibility_padding = static_cast<std::uint32_t>(
            clip_worker::formats::B3dmParser::kCompatibleAlignment);
    if (batch_length > 0U) {
        writeUint32(bytes, 20, batch_length + compatibility_padding);
    } else {
        writeUint32(bytes, 12, feature_length + compatibility_padding);
    }
}

void writeUint32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) {
        throw std::out_of_range("writeUint32 offset is outside the fixture buffer");
    }
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

}  // namespace clip_worker::tests
