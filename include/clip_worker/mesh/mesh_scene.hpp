#pragma once

#include "clip_worker/geometry/matrix4.hpp"
#include "clip_worker/metadata/feature_metadata.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clip_worker::mesh {

/** Supported glTF primitive topologies before canonical triangle expansion. */
enum class PrimitiveMode {
    triangles,
    triangle_strip,
};

/** Core glTF sampler wrapping modes. */
enum class SamplerWrap : std::uint32_t {
    clamp_to_edge = 33071U,
    mirrored_repeat = 33648U,
    repeat = 10497U,
};

/** Source up-axis used when evaluating a 3D Tiles content transform. */
enum class UpAxis {
    x,
    y,
    z,
};

/** Inclusive finite axis-aligned bounds in mesh-local coordinates. */
struct Bounds3 {
    std::array<double, 3> minimum{};
    std::array<double, 3> maximum{};
    bool valid = false;
};

/** Decoded RGBA8 image owned by the typed scene. */
struct RgbaImage {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> pixels;
};

struct MeshSampler {
    SamplerWrap wrap_s = SamplerWrap::repeat;
    SamplerWrap wrap_t = SamplerWrap::repeat;
    std::optional<std::uint32_t> mag_filter;
    std::optional<std::uint32_t> min_filter;
};

struct MeshTexture {
    std::size_t image = 0U;
    std::optional<std::size_t> sampler;
};

struct TextureBinding {
    std::size_t texture = 0U;
    std::uint32_t texcoord_set = 0U;
};

/** Core PBR material fields that affect retained pixels or vertex semantics. */
struct MeshMaterial {
    std::array<float, 4> base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    float metallic_factor = 1.0F;
    float roughness_factor = 1.0F;
    std::optional<TextureBinding> base_color_texture;
    std::optional<TextureBinding> metallic_roughness_texture;
    std::optional<TextureBinding> normal_texture;
    std::optional<TextureBinding> occlusion_texture;
    std::optional<TextureBinding> emissive_texture;
    std::array<float, 3> emissive_factor{0.0F, 0.0F, 0.0F};
    float normal_scale = 1.0F;
    float occlusion_strength = 1.0F;
    std::string alpha_mode = "OPAQUE";
    float alpha_cutoff = 0.5F;
    bool unlit = false;
    bool double_sided = false;
};

/**
 * Structure-of-arrays primitive shared by normalization, clipping, and writers.
 * Continuous streams are tightly packed and use the component counts in the
 * field names. Feature IDs are discrete and are never interpolated.
 */
struct MeshPrimitive {
    PrimitiveMode source_mode = PrimitiveMode::triangles;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> tangents;
    std::vector<float> texcoords_0;
    std::vector<float> texcoords_1;
    std::vector<float> colors;
    std::uint32_t color_components = 0U;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> feature_ids;
    std::vector<metadata::FeatureIdSet> feature_id_sets;
    std::optional<std::size_t> material;
    Bounds3 bounds;

    [[nodiscard]] std::size_t vertexCount() const noexcept;
    [[nodiscard]] std::size_t triangleCount() const noexcept;
};

struct Mesh {
    std::vector<MeshPrimitive> primitives;
    Bounds3 bounds;
};

/** Aligned glTF instance attributes attached to a mesh-bearing node. */
struct MeshNodeInstancing {
    std::vector<float> translations;
    std::vector<float> rotations;
    std::vector<float> scales;
    std::vector<std::uint32_t> feature_ids;

    [[nodiscard]] std::size_t instanceCount() const noexcept {
        return translations.size() / 3U;
    }
};

struct MeshNode {
    MeshNode() = default;
    MeshNode(std::size_t source_ordinal_value,
             geometry::Matrix4 local_transform_value,
             std::optional<std::size_t> mesh_value,
             std::vector<std::size_t> children_value)
        : source_ordinal(source_ordinal_value),
          local_transform(std::move(local_transform_value)),
          mesh(mesh_value),
          children(std::move(children_value)) {
    }

    std::size_t source_ordinal = 0U;
    geometry::Matrix4 local_transform = geometry::Matrix4::identity();
    std::optional<std::size_t> mesh;
    std::vector<std::size_t> children;
    std::optional<MeshNodeInstancing> instancing;
};

/** Canonical value family for the supported legacy B3DM property subset. */
enum class LegacyPropertyKind {
    numeric,
    boolean,
    string,
};

struct LegacyPropertyColumn {
    std::string name;
    LegacyPropertyKind kind = LegacyPropertyKind::numeric;
    std::uint32_t components = 1U;
    std::vector<double> numeric_values;
    std::vector<std::uint8_t> boolean_values;
    std::vector<std::string> string_values;
};

struct LegacyPropertyTable {
    std::uint32_t feature_count = 0U;
    std::vector<LegacyPropertyColumn> columns;
};

/** Canonical typed mesh scene; source JSON and bufferView identities never leak in. */
struct MeshScene {
    std::size_t default_scene = 0U;
    std::vector<std::vector<std::size_t>> scenes;
    std::vector<MeshNode> nodes;
    std::vector<Mesh> meshes;
    std::vector<MeshMaterial> materials;
    std::vector<MeshSampler> samplers;
    std::vector<MeshTexture> textures;
    std::vector<RgbaImage> images;
    std::optional<LegacyPropertyTable> legacy_properties;
    std::optional<metadata::FeatureMetadata> feature_metadata;
    bool require_unlit = false;
};

/** Named limits used before every typed mesh allocation. */
struct MeshResourceLimits {
    std::uint64_t maximum_vertices = 100000000ULL;
    std::uint64_t maximum_indices = 300000000ULL;
    std::uint64_t maximum_decoded_bytes = 1073741824ULL;
    std::uint64_t maximum_texture_pixels = 268435456ULL;
};

class MeshResourceAccountant final {
public:
    explicit MeshResourceAccountant(MeshResourceLimits limits);

    void reserveVertices(std::uint64_t count);
    void reserveIndices(std::uint64_t count);
    void reserveDecodedBytes(std::uint64_t count);
    void reserveTexturePixels(std::uint64_t count);

    [[nodiscard]] std::uint64_t vertices() const noexcept { return vertices_; }
    [[nodiscard]] std::uint64_t indices() const noexcept { return indices_; }
    [[nodiscard]] std::uint64_t decodedBytes() const noexcept { return decoded_bytes_; }
    [[nodiscard]] std::uint64_t texturePixels() const noexcept { return texture_pixels_; }

private:
    static void reserve(std::uint64_t count, std::uint64_t limit,
                        std::uint64_t& current, const char* resource);

    MeshResourceLimits limits_;
    std::uint64_t vertices_ = 0U;
    std::uint64_t indices_ = 0U;
    std::uint64_t decoded_bytes_ = 0U;
    std::uint64_t texture_pixels_ = 0U;
};

/** Recomputes bounds and validates stream cardinality, indices, references, and finiteness. */
void validateMeshPrimitive(MeshPrimitive& primitive);

/** Recomputes bounds and validates stream cardinality, indices, references, and finiteness. */
void validateMeshScene(MeshScene& scene);

/** Converts a source content up-axis into the 3D Tiles Z-up basis. */
[[nodiscard]] geometry::Matrix4 upAxisToZTransform(UpAxis axis);

}  // namespace clip_worker::mesh
