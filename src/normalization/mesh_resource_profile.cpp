#include "clip_worker/normalization/mesh_resource_profile.hpp"

#include "clip_worker/client/object_transfer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

namespace clip_worker::normalization {
namespace {

using Json = nlohmann::json;

std::string lowercaseAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::vector<std::uint8_t> readDocument(
        const std::filesystem::path& document) {
    std::ifstream input(document, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::invalid_argument("Mesh resource profile is unavailable");
    }
    const std::streampos end = input.tellg();
    if (end <= 0
        || static_cast<std::uintmax_t>(end)
                > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("Mesh resource profile size is invalid");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::invalid_argument("Mesh resource profile cannot be read");
    }
    return bytes;
}

template <typename Value>
Value positive(const Json& object, const char* field) {
    const Value value = object.at(field).get<Value>();
    if (value <= Value{}) {
        throw std::invalid_argument("Mesh resource profile limit is invalid");
    }
    return value;
}

}  // namespace

MeshResourceProfile MeshResourceProfile::load(
        const std::filesystem::path& document,
        const std::string& profile_id,
        const std::string& expected_document_sha256) {
    if (profile_id.empty() || expected_document_sha256.size() != 64U) {
        throw std::invalid_argument("Mesh resource profile identity is invalid");
    }
    const auto bytes = readDocument(document);
    const std::string actual_sha256 = client::sha256Hex(bytes);
    if (lowercaseAscii(expected_document_sha256) != actual_sha256) {
        throw std::invalid_argument("Mesh resource profile hash mismatch");
    }
    Json root;
    try {
        root = Json::parse(bytes.begin(), bytes.end());
    } catch (const std::exception&) {
        throw std::invalid_argument("Mesh resource profile JSON is invalid");
    }
    if (!root.is_object() || root.value("schemaVersion", 0) != 2
        || !root.contains("profiles") || !root.at("profiles").is_array()) {
        throw std::invalid_argument("Mesh resource profile document is invalid");
    }
    const Json* selected = nullptr;
    for (const auto& profile : root.at("profiles")) {
        if (profile.is_object()
            && profile.value("id", std::string{}) == profile_id) {
            if (selected != nullptr) {
                throw std::invalid_argument(
                        "Mesh resource profile identity is duplicated");
            }
            selected = &profile;
        }
    }
    if (selected == nullptr) {
        throw std::invalid_argument("Mesh resource profile is unavailable");
    }

    MeshResourceProfile result;
    result.profile_id = profile_id;
    result.document_sha256 = actual_sha256;
    try {
        const auto& closure = selected->at("resourceClosure");
        const auto& gltf = selected->at("gltf");
        const auto& geometry = selected->at("geometry");
        const auto& texture = selected->at("texture");
        const auto& metadata = selected->at("metadata");
        const auto& authorization = selected->at("authorization");
        const auto& decoder = selected->at("decoder");
        result.maximum_input_bytes = positive<std::uint64_t>(
                closure, "maximumInputBytes");
        result.maximum_output_bytes = positive<std::uint64_t>(
                closure, "maximumOutputBytes");
        auto& limits = result.b3dm.gltf;
        limits.maximum_data_uri_bytes = positive<std::uint64_t>(
                closure, "maximumDecodedDataUriBytes");
        limits.maximum_buffers = positive<std::size_t>(gltf, "maximumBuffers");
        limits.maximum_buffer_views = positive<std::size_t>(
                gltf, "maximumBufferViews");
        limits.maximum_accessors = positive<std::size_t>(
                gltf, "maximumAccessors");
        limits.maximum_nodes = positive<std::size_t>(gltf, "maximumNodes");
        limits.maximum_meshes = positive<std::size_t>(gltf, "maximumMeshes");
        limits.maximum_primitives = positive<std::size_t>(
                gltf, "maximumPrimitives");
        limits.maximum_images = positive<std::size_t>(gltf, "maximumImages");
        limits.mesh.maximum_vertices = positive<std::uint64_t>(
                geometry, "maximumVerticesPerContent");
        const std::uint64_t triangles = positive<std::uint64_t>(
                geometry, "maximumTrianglesPerContent");
        if (triangles > std::numeric_limits<std::uint64_t>::max() / 3U) {
            throw std::invalid_argument(
                    "Mesh resource profile triangle limit overflows");
        }
        limits.mesh.maximum_indices = triangles * 3U;
        limits.mesh.maximum_decoded_bytes = positive<std::uint64_t>(
                geometry, "maximumDecodedGeometryBytes");
        limits.mesh.maximum_texture_pixels = positive<std::uint64_t>(
                texture, "maximumAggregateTexturePixels");
        limits.texture.maximum_width = positive<std::uint32_t>(
                texture, "maximumWidthPixels");
        limits.texture.maximum_height = positive<std::uint32_t>(
                texture, "maximumHeightPixels");
        limits.texture.maximum_pixels = positive<std::uint64_t>(
                texture, "maximumPixelsPerImage");
        limits.texture.maximum_rgba_bytes = positive<std::uint64_t>(
                texture, "maximumDecodedRgbaBytesPerImage");
        limits.draco.maximum_points = positive<std::size_t>(
                decoder, "maximumDracoPoints");
        limits.draco.maximum_faces = positive<std::size_t>(
                decoder, "maximumDracoFaces");
        limits.draco.maximum_decoded_bytes = positive<std::size_t>(
                decoder, "maximumSingleDecoderScratchBytes");
        limits.draco.maximum_attribute_components = positive<std::size_t>(
                decoder, "maximumDracoAttributeComponents");
        limits.meshopt.maximum_count = positive<std::size_t>(
                decoder, "maximumMeshoptCount");
        limits.meshopt.maximum_decoded_bytes = positive<std::size_t>(
                decoder, "maximumSingleDecoderScratchBytes");
        limits.meshopt.maximum_stride = positive<std::size_t>(
                decoder, "maximumMeshoptStride");
        result.b3dm.metadata.maximum_feature_rows = positive<std::uint64_t>(
                metadata, "maximumFeatureRows");
        result.b3dm.metadata.maximum_properties = positive<std::size_t>(
                metadata, "maximumProperties");
        result.b3dm.metadata.maximum_string_bytes = positive<std::uint64_t>(
                metadata, "maximumStringBytes");
        result.b3dm.metadata.maximum_binary_bytes = positive<std::uint64_t>(
                metadata, "maximumBinaryBytes");
        result.texture_mask.maximum_repeat_span = positive<std::uint32_t>(
                texture, "maximumUvRepeatSpan");
        result.texture_mask.maximum_raster_tests = positive<std::uint64_t>(
                texture, "maximumMaskRasterTests");
        result.authorization.maximum_polygons = positive<std::size_t>(
                authorization, "maximumPolygons");
        result.authorization.maximum_rings = positive<std::size_t>(
                authorization, "maximumRings");
        result.authorization.maximum_points = positive<std::size_t>(
                authorization, "maximumPoints");
        result.authorization.maximum_densified_points_per_segment =
                positive<std::size_t>(
                        authorization,
                        "maximumDensifiedPointsPerSegment");
        result.authorization.maximum_triangles = positive<std::size_t>(
                authorization, "maximumTriangles");
        result.authorization.densify_segment_meters = positive<double>(
                authorization, "densifySegmentMeters");

        const auto& policy = selected->at("policy");
        if (policy.at("limitFailureMode") != "FAIL_CLOSED"
            || policy.at("canonicalTextureEncoding") != "PNG"
            || policy.at("normalizationVersion") != kMeshNormalizationVersion
            || policy.at("canonicalContractVersion")
                    != kCanonicalContractVersion) {
            throw std::invalid_argument(
                    "Mesh resource profile policy is incompatible");
        }
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception&) {
        throw std::invalid_argument("Mesh resource profile limits are invalid");
    }
    if (result.maximum_output_bytes > ProtocolLimits::kMaximumArtifactBytes
        || result.b3dm.gltf.texture.maximum_rgba_bytes
                > result.b3dm.gltf.mesh.maximum_decoded_bytes
        || result.b3dm.gltf.draco.maximum_decoded_bytes
                > result.b3dm.gltf.mesh.maximum_decoded_bytes
        || result.b3dm.gltf.meshopt.maximum_decoded_bytes
                > result.b3dm.gltf.mesh.maximum_decoded_bytes) {
        throw std::invalid_argument(
                "Mesh resource profile limits are inconsistent");
    }
    return result;
}

}  // namespace clip_worker::normalization
