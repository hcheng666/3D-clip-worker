#include "clip_worker/clip/canonical_instance_clip_strategy.hpp"

#include "clip_worker/client/object_transfer.hpp"
#include "clip_worker/formats/format_error.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace clip_worker::clip {
namespace {

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(
                (value >> (index * 8U)) & 0xffU));
    }
}

void appendDouble(std::vector<std::uint8_t>& bytes, double value) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

std::vector<std::uint8_t> scopeWkb() {
    const std::array<std::array<double, 2U>, 5U> ring{{
            {120.0, 30.0}, {120.001, 30.0}, {120.001, 30.001},
            {120.0, 30.001}, {120.0, 30.0}}};
    std::vector<std::uint8_t> bytes{1U};
    appendU32(bytes, 3U);
    appendU32(bytes, 1U);
    appendU32(bytes, static_cast<std::uint32_t>(ring.size()));
    for (const auto& coordinate : ring) {
        appendDouble(bytes, coordinate[0]);
        appendDouble(bytes, coordinate[1]);
    }
    return bytes;
}

geometry::Matrix4 enuToEcef(double longitude_degrees,
                            double latitude_degrees) {
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    constexpr double kSemiMajor = 6378137.0;
    constexpr double kEccentricitySquared = 6.69437999014e-3;
    const double longitude = longitude_degrees * kRadians;
    const double latitude = latitude_degrees * kRadians;
    const double sin_lon = std::sin(longitude);
    const double cos_lon = std::cos(longitude);
    const double sin_lat = std::sin(latitude);
    const double cos_lat = std::cos(latitude);
    const double prime_vertical = kSemiMajor
            / std::sqrt(1.0 - kEccentricitySquared * sin_lat * sin_lat);
    const std::array<double, 3U> origin{
            prime_vertical * cos_lat * cos_lon,
            prime_vertical * cos_lat * sin_lon,
            prime_vertical * (1.0 - kEccentricitySquared) * sin_lat};
    return geometry::Matrix4::fromColumnMajor({
            -sin_lon, cos_lon, 0.0, 0.0,
            -sin_lat * cos_lon, -sin_lat * sin_lon, cos_lat, 0.0,
            cos_lat * cos_lon, cos_lat * sin_lon, sin_lat, 0.0,
            origin[0], origin[1], origin[2], 1.0});
}

normalization::MeshResourceProfile profile() {
    const auto path = std::filesystem::path(CLIP_WORKER_SOURCE_DIR)
            / "config/resource-limit-profiles-v2.json";
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes(
            std::istreambuf_iterator<char>(input), {});
    return normalization::MeshResourceProfile::load(
            path, normalization::kMeshResourceProfileVersion,
            client::sha256Hex(bytes));
}

normalization::ToolVersion validator(const std::string& family) {
    return {family + "_CANONICAL_VALIDATOR", "1.0.0", std::string(64U, 'c')};
}

instance::InstanceScene sourceScene() {
    mesh::MeshPrimitive primitive;
    primitive.positions = {-10.0F, 0.0F, 10.0F,
                           10.0F, 0.0F, 10.0F,
                           0.0F, 0.0F, -10.0F};
    primitive.indices = {0U, 1U, 2U};
    mesh::MeshNodeInstancing instances;
    instances.translations = {0.0F, 0.0F, 0.0F,
                              48.0F, 0.0F, 0.0F,
                              120.0F, 0.0F, 0.0F};
    instances.rotations = {0.0F, 0.0F, 0.0F, 1.0F,
                           0.0F, 0.0F, 0.0F, 1.0F,
                           0.0F, 0.0F, 0.0F, 1.0F};
    instances.scales = {1.0F, 1.0F, 1.0F,
                        1.0F, 1.0F, 1.0F,
                        1.0F, 1.0F, 1.0F};
    instances.feature_ids = {0U, 1U, 2U};
    instance::InstanceScene scene;
    scene.model.default_scene = 0U;
    scene.model.scenes = {{0U}};
    mesh::MeshNode node;
    node.mesh = 0U;
    node.instancing = std::move(instances);
    scene.model.nodes.push_back(std::move(node));
    scene.model.meshes = {{{std::move(primitive)}, {}}};
    mesh::LegacyPropertyColumn names;
    names.name = "name";
    names.kind = mesh::LegacyPropertyKind::string;
    names.string_values = {"whole", "boundary", "outside"};
    scene.model.legacy_properties = mesh::LegacyPropertyTable{
            3U, {std::move(names)}};
    instance::validateInstanceScene(scene);
    return scene;
}

TEST(CanonicalInstanceClipStrategyTest, EmitsWholeThenBoundaryAndDropsDisjoint) {
    MeshSceneClipRequest request;
    request.scope_wkb = scopeWkb();
    request.scope_srid = 4490;
    request.tileset_transform = enuToEcef(120.0005, 30.0005);

    const auto result = CanonicalInstanceClipStrategy::clip(
            sourceScene(), request, profile(), validator("INSTANCE"),
            validator("MESH"));

    ASSERT_EQ(result.relations.size(), 3U);
    EXPECT_EQ(result.relations[0U], InstanceBoundsRelation::whole);
    EXPECT_EQ(result.relations[1U], InstanceBoundsRelation::boundary);
    EXPECT_EQ(result.relations[2U], InstanceBoundsRelation::disjoint);
    ASSERT_TRUE(result.whole_instances.has_value());
    ASSERT_TRUE(result.boundary_mesh.has_value());
    EXPECT_EQ(result.whole_instances->instance_count, 1U);
    EXPECT_FALSE(result.boundary_mesh->empty);
}

TEST(CanonicalInstanceClipStrategyTest, FailsWholeContentWhenExpansionLimitIsExceeded) {
    MeshSceneClipRequest request;
    request.scope_wkb = scopeWkb();
    request.tileset_transform = enuToEcef(120.0005, 30.0005);
    InstanceExpansionLimits limits;
    limits.maximum_boundary_instances = 0U;
    EXPECT_THROW(CanonicalInstanceClipStrategy::clip(
                         sourceScene(), request, profile(), validator("INSTANCE"),
                         validator("MESH"), limits),
                 formats::FormatError);
}

}  // namespace
}  // namespace clip_worker::clip
