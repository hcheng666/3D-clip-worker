#include "clip_worker/clip/texture_masker.hpp"

#include "clip_worker/formats/format_error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace clip_worker::clip {
namespace {

struct UvPoint {
    double u = 0.0;
    double v = 0.0;
};

using UvTriangle = std::array<UvPoint, 3>;

[[noreturn]] void invalid(const char* message) {
    throw formats::FormatError(formats::FormatErrorCode::invalid_accessor,
                               message);
}

double cross(const UvPoint& first, const UvPoint& second,
             const UvPoint& point) {
    return (second.u - first.u) * (point.v - first.v)
           - (second.v - first.v) * (point.u - first.u);
}

bool contains(const UvTriangle& triangle, const UvPoint& point) {
    constexpr double kUvEpsilon = 1.0e-12;
    const double first = cross(triangle[0U], triangle[1U], point);
    const double second = cross(triangle[1U], triangle[2U], point);
    const double third = cross(triangle[2U], triangle[0U], point);
    const bool negative = first < -kUvEpsilon || second < -kUvEpsilon
                          || third < -kUvEpsilon;
    const bool positive = first > kUvEpsilon || second > kUvEpsilon
                          || third > kUvEpsilon;
    return !(negative && positive);
}

std::int64_t floorTile(double value) {
    if (!std::isfinite(value)
        || value < static_cast<double>(std::numeric_limits<std::int64_t>::min())
        || value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        invalid("Texture coordinate cannot be converted to a repeat tile");
    }
    return static_cast<std::int64_t>(std::floor(value));
}

bool odd(std::int64_t value) {
    const std::int64_t remainder = value % 2;
    return remainder == 1 || remainder == -1;
}

std::vector<double> candidates(double normalized, double minimum,
                               double maximum, mesh::SamplerWrap wrap,
                               bool edge_pixel,
                               const TextureMaskLimits& limits) {
    if (wrap == mesh::SamplerWrap::clamp_to_edge) {
        std::vector<double> result{normalized};
        if (edge_pixel && minimum < 0.0) result.push_back(0.0);
        if (edge_pixel && maximum > 1.0) result.push_back(1.0);
        return result;
    }
    const std::int64_t first = floorTile(minimum) - 1;
    const std::int64_t last = floorTile(maximum) + 1;
    const long double span = static_cast<long double>(last)
                             - static_cast<long double>(first) + 1.0L;
    if (last < first || span > limits.maximum_repeat_span) {
        throw formats::FormatError(formats::FormatErrorCode::unsupported_content,
                                   "Texture UV repeat span exceeds the configured limit");
    }
    std::vector<double> result;
    result.reserve(static_cast<std::size_t>(span));
    for (std::int64_t tile = first; tile <= last; ++tile) {
        const double value = wrap == mesh::SamplerWrap::repeat || !odd(tile)
                ? static_cast<double>(tile) + normalized
                : static_cast<double>(tile) + (1.0 - normalized);
        if (value >= minimum - 1.0e-12 && value <= maximum + 1.0e-12)
            result.push_back(value);
    }
    return result;
}

void rasterize(const UvTriangle& triangle, const mesh::MeshSampler& sampler,
               mesh::RgbaImage& image, std::vector<std::uint8_t>& retained,
               TextureMaskStatistics& statistics,
               const TextureMaskLimits& limits) {
    double minimum_u = triangle[0U].u;
    double maximum_u = minimum_u;
    double minimum_v = triangle[0U].v;
    double maximum_v = minimum_v;
    for (const auto& point : triangle) {
        if (!std::isfinite(point.u) || !std::isfinite(point.v))
            invalid("Texture coordinate is non-finite");
        minimum_u = std::min(minimum_u, point.u);
        maximum_u = std::max(maximum_u, point.u);
        minimum_v = std::min(minimum_v, point.v);
        maximum_v = std::max(maximum_v, point.v);
    }
    for (std::uint32_t row = 0U; row < image.height; ++row) {
        const double v = 1.0 - (static_cast<double>(row) + 0.5)
                                 / static_cast<double>(image.height);
        const auto source_v = candidates(
                v, minimum_v, maximum_v, sampler.wrap_t,
                row == 0U || row + 1U == image.height, limits);
        for (std::uint32_t column = 0U; column < image.width; ++column) {
            const std::size_t pixel = static_cast<std::size_t>(row) * image.width
                                      + column;
            if (retained[pixel] != 0U) continue;
            const double u = (static_cast<double>(column) + 0.5)
                             / static_cast<double>(image.width);
            const auto source_u = candidates(
                    u, minimum_u, maximum_u, sampler.wrap_s,
                    column == 0U || column + 1U == image.width, limits);
            bool keep = false;
            for (const double candidate_v : source_v) {
                for (const double candidate_u : source_u) {
                    if (++statistics.raster_tests > limits.maximum_raster_tests) {
                        throw formats::FormatError(
                                formats::FormatErrorCode::unsupported_content,
                                "Texture masking exceeds the configured raster limit");
                    }
                    if (contains(triangle, {candidate_u, candidate_v})) {
                        keep = true;
                        break;
                    }
                }
                if (keep) break;
            }
            if (keep) retained[pixel] = 1U;
        }
    }
}

std::vector<mesh::TextureBinding> bindings(const mesh::MeshMaterial& material) {
    std::vector<mesh::TextureBinding> result;
    for (const auto* binding : {&material.base_color_texture,
                                &material.metallic_roughness_texture,
                                &material.normal_texture,
                                &material.occlusion_texture,
                                &material.emissive_texture}) {
        if (binding->has_value()) result.push_back(**binding);
    }
    return result;
}

const std::vector<float>& coordinates(const mesh::MeshPrimitive& primitive,
                                      std::uint32_t set) {
    if (set == 0U && !primitive.texcoords_0.empty()) return primitive.texcoords_0;
    if (set == 1U && !primitive.texcoords_1.empty()) return primitive.texcoords_1;
    invalid("Material references a missing texture coordinate set");
}

}  // namespace

TextureMaskStatistics TextureMasker::mask(
        mesh::MeshScene& scene, const TextureMaskLimits& limits) {
    mesh::validateMeshScene(scene);
    if (limits.maximum_repeat_span == 0U
        || limits.maximum_raster_tests == 0U) {
        throw std::invalid_argument("Texture mask limits are invalid");
    }
    std::vector<std::vector<std::uint8_t>> retained(scene.images.size());
    for (std::size_t image = 0U; image < scene.images.size(); ++image) {
        retained[image].assign(static_cast<std::size_t>(scene.images[image].width)
                                       * scene.images[image].height, 0U);
    }
    TextureMaskStatistics statistics;
    const mesh::MeshSampler default_sampler;
    for (const auto& source_mesh : scene.meshes) {
        for (const auto& primitive : source_mesh.primitives) {
            if (!primitive.material.has_value()) continue;
            const auto& material = scene.materials.at(*primitive.material);
            for (const auto& binding : bindings(material)) {
                const auto& texture = scene.textures.at(binding.texture);
                auto& image = scene.images.at(texture.image);
                const auto& sampler = texture.sampler.has_value()
                        ? scene.samplers.at(*texture.sampler) : default_sampler;
                const auto& uv = coordinates(primitive, binding.texcoord_set);
                for (std::size_t offset = 0U; offset < primitive.indices.size();
                     offset += 3U) {
                    UvTriangle triangle{};
                    for (std::size_t vertex = 0U; vertex < 3U; ++vertex) {
                        const std::size_t index = primitive.indices[offset + vertex];
                        triangle[vertex] = {uv.at(index * 2U),
                                            uv.at(index * 2U + 1U)};
                    }
                    rasterize(triangle, sampler, image, retained[texture.image],
                              statistics, limits);
                }
            }
        }
    }
    for (std::size_t image_index = 0U; image_index < scene.images.size();
         ++image_index) {
        auto& image = scene.images[image_index];
        for (std::size_t pixel = 0U; pixel < retained[image_index].size(); ++pixel) {
            if (retained[image_index][pixel] != 0U) {
                ++statistics.retained_pixels;
                continue;
            }
            std::fill_n(image.pixels.begin()
                                + static_cast<std::ptrdiff_t>(pixel * 4U),
                        4U, 0U);
            ++statistics.cleared_pixels;
        }
    }
    return statistics;
}

}  // namespace clip_worker::clip
