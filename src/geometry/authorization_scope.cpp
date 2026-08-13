#include "clip_worker/geometry/authorization_scope.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

#include <mapbox/earcut.hpp>
#include <proj.h>

namespace clip_worker::geometry {
namespace {

constexpr std::uint32_t kWkbPolygon = 3U;
constexpr std::uint32_t kWkbMultiPolygon = 6U;
constexpr std::int32_t kSupportedScopeSrid = 4490;
constexpr std::size_t kMaximumPolygons = 100000U;
constexpr std::size_t kMaximumRings = 100000U;
constexpr std::size_t kMaximumPoints = 2000000U;
constexpr std::size_t kMaximumDensifiedPointsPerSegment = 100000U;
constexpr double kApproximateMetersPerDegree = 111319.49079327358;

using Ring = std::vector<Point2>;
using Polygon = std::vector<Ring>;

class WkbReader final {
public:
    explicit WkbReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {
    }

    std::vector<Polygon> read() {
        auto polygons = readGeometry();
        if (offset_ != bytes_.size()) {
            throw std::invalid_argument("Authorization WKB contains trailing bytes");
        }
        return polygons;
    }

private:
    std::vector<Polygon> readGeometry(bool require_polygon = false) {
        const bool little_endian = readByteOrder();
        const std::uint32_t type = readUint32(little_endian);
        if (type == kWkbPolygon) {
            return {readPolygon(little_endian)};
        }
        if (require_polygon) {
            throw std::invalid_argument("MultiPolygon child must be a Polygon");
        }
        if (type != kWkbMultiPolygon) {
            throw std::invalid_argument("Authorization WKB must be Polygon or MultiPolygon");
        }
        const std::uint32_t count = readUint32(little_endian);
        requireCount(count, kMaximumPolygons, "polygon");
        std::vector<Polygon> polygons;
        polygons.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            auto child = readGeometry(true);
            if (child.size() != 1U) {
                throw std::invalid_argument("MultiPolygon child must be a Polygon");
            }
            polygons.push_back(std::move(child.front()));
        }
        return polygons;
    }

    Polygon readPolygon(bool little_endian) {
        const std::uint32_t ring_count = readUint32(little_endian);
        requireCount(ring_count, kMaximumRings, "ring");
        if (ring_count == 0U) {
            throw std::invalid_argument("Authorization Polygon has no rings");
        }
        Polygon polygon;
        polygon.reserve(ring_count);
        for (std::uint32_t ring_index = 0; ring_index < ring_count; ++ring_index) {
            const std::uint32_t point_count = readUint32(little_endian);
            requireCount(point_count, kMaximumPoints, "point");
            if (point_count < 4U) {
                throw std::invalid_argument("Authorization ring has fewer than four WKB points");
            }
            Ring ring;
            ring.reserve(point_count);
            for (std::uint32_t point_index = 0; point_index < point_count; ++point_index) {
                const double x = readDouble(little_endian);
                const double y = readDouble(little_endian);
                if (!std::isfinite(x) || !std::isfinite(y)) {
                    throw std::invalid_argument("Authorization WKB contains non-finite coordinates");
                }
                ring.push_back({x, y});
            }
            if (ring.front().x != ring.back().x || ring.front().y != ring.back().y) {
                throw std::invalid_argument("Authorization WKB ring is not closed");
            }
            ring.pop_back();
            polygon.push_back(std::move(ring));
        }
        return polygon;
    }

    bool readByteOrder() {
        const std::uint8_t value = readByte();
        if (value > 1U) {
            throw std::invalid_argument("Authorization WKB byte order is invalid");
        }
        return value == 1U;
    }

    std::uint8_t readByte() {
        requireBytes(1U);
        return bytes_[offset_++];
    }

    std::uint32_t readUint32(bool little_endian) {
        requireBytes(sizeof(std::uint32_t));
        std::uint32_t value = 0U;
        if (little_endian) {
            for (std::size_t index = 0; index < sizeof(value); ++index) {
                value |= static_cast<std::uint32_t>(bytes_[offset_ + index])
                         << (index * 8U);
            }
        } else {
            for (std::size_t index = 0; index < sizeof(value); ++index) {
                value = (value << 8U) | bytes_[offset_ + index];
            }
        }
        offset_ += sizeof(value);
        return value;
    }

    double readDouble(bool little_endian) {
        requireBytes(sizeof(double));
        std::array<std::uint8_t, sizeof(double)> ordered{};
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const std::size_t source = little_endian == nativeLittleEndian()
                    ? index : ordered.size() - 1U - index;
            ordered[index] = bytes_[offset_ + source];
        }
        offset_ += ordered.size();
        double value = 0.0;
        std::memcpy(&value, ordered.data(), sizeof(value));
        return value;
    }

    static bool nativeLittleEndian() {
        const std::uint16_t marker = 1U;
        return *reinterpret_cast<const std::uint8_t*>(&marker) == 1U;
    }

    void requireBytes(std::size_t count) const {
        if (offset_ > bytes_.size() || count > bytes_.size() - offset_) {
            throw std::invalid_argument("Authorization WKB ended unexpectedly");
        }
    }

    static void requireCount(std::uint32_t count, std::size_t maximum,
                             const char* name) {
        if (count == 0U || count > maximum) {
            throw std::invalid_argument(std::string("Authorization WKB ") + name
                                        + " count is invalid");
        }
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0U;
};

std::vector<std::uint8_t> decodeBase64(const std::string& value) {
    static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> lookup{};
    lookup.fill(-1);
    for (int index = 0; index < 64; ++index) {
        lookup[static_cast<unsigned char>(kAlphabet[index])] = index;
    }
    std::vector<std::uint8_t> output;
    output.reserve(value.size() * 3U / 4U);
    std::uint32_t accumulator = 0U;
    int bits = -8;
    bool padding = false;
    for (const unsigned char character : value) {
        if (character == '=') {
            padding = true;
            continue;
        }
        if (std::isspace(character) != 0) {
            continue;
        }
        if (padding || lookup[character] < 0) {
            throw std::invalid_argument("Authorization scope Base64 is invalid");
        }
        accumulator = (accumulator << 6U)
                      | static_cast<std::uint32_t>(lookup[character]);
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
            bits -= 8;
        }
    }
    if (output.empty()) {
        throw std::invalid_argument("Authorization scope Base64 is empty");
    }
    return output;
}

double longitudeDelta(double first, double second) {
    double delta = second - first;
    while (delta > 180.0) {
        delta -= 360.0;
    }
    while (delta < -180.0) {
        delta += 360.0;
    }
    return delta;
}

Ring densify(const Ring& ring) {
    Ring result;
    for (std::size_t index = 0; index < ring.size(); ++index) {
        const Point2& first = ring[index];
        const Point2& second = ring[(index + 1U) % ring.size()];
        const double delta_lon = longitudeDelta(first.x, second.x);
        const double delta_lat = second.y - first.y;
        const double mean_latitude = (first.y + second.y) * 0.5
                                     * 3.14159265358979323846 / 180.0;
        const double east = delta_lon * std::cos(mean_latitude)
                            * kApproximateMetersPerDegree;
        const double north = delta_lat * kApproximateMetersPerDegree;
        const double length = std::hypot(east, north);
        const auto segment_count = static_cast<std::size_t>(std::max(
                1.0, std::ceil(length / ClipTolerances::kScopeDensifySegmentMeters)));
        if (segment_count > kMaximumDensifiedPointsPerSegment) {
            throw std::invalid_argument("Authorization ring segment is too long to densify safely");
        }
        for (std::size_t segment = 0; segment < segment_count; ++segment) {
            const double t = static_cast<double>(segment)
                             / static_cast<double>(segment_count);
            result.push_back({first.x + delta_lon * t, first.y + delta_lat * t});
        }
    }
    return result;
}

std::pair<double, double> scopeCenter(const std::vector<Polygon>& polygons) {
    double longitude = 0.0;
    double latitude = 0.0;
    std::size_t count = 0U;
    for (const auto& polygon : polygons) {
        for (const auto& point : polygon.front()) {
            longitude += point.x;
            latitude += point.y;
            ++count;
        }
    }
    if (count == 0U) {
        throw std::invalid_argument("Authorization scope has no coordinates");
    }
    return {longitude / static_cast<double>(count),
            latitude / static_cast<double>(count)};
}

PJ* normalizedTransform(PJ_CONTEXT* context, const char* source,
                        const std::string& target) {
    PJ* transform = proj_create_crs_to_crs(context, source, target.c_str(), nullptr);
    if (transform == nullptr) {
        throw std::runtime_error("Unable to create authorization projection");
    }
    PJ* normalized = proj_normalize_for_visualization(context, transform);
    proj_destroy(transform);
    if (normalized == nullptr) {
        throw std::runtime_error("Unable to normalize authorization projection");
    }
    return normalized;
}

}  // namespace

class AuthorizationScope::Impl final {
public:
    Impl(double center_longitude, double center_latitude) {
        context_ = proj_context_create();
        if (context_ == nullptr) {
            throw std::runtime_error("Unable to create PROJ context");
        }
        std::ostringstream target;
        target.imbue(std::locale::classic());
        target << std::setprecision(15)
               << "+proj=aeqd +lat_0=" << center_latitude
               << " +lon_0=" << center_longitude
               << " +ellps=GRS80 +units=m +no_defs +type=crs";
        try {
            geographic_to_local_ = normalizedTransform(context_, "EPSG:4490", target.str());
            ecef_to_local_ = normalizedTransform(context_, "EPSG:4479", target.str());
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() {
        cleanup();
    }

    Point2 projectGeographic(const Point2& value) const {
        const PJ_COORD result = project(geographic_to_local_, value.x, value.y, 0.0);
        return {result.xyz.x, result.xyz.y};
    }

    ProjectedPoint projectEcef(const std::array<double, 3>& value) const {
        const PJ_COORD result = project(ecef_to_local_, value[0], value[1], value[2]);
        return {{result.xyz.x, result.xyz.y}, result.xyz.z};
    }

private:
    PJ_COORD project(PJ* transform, double x, double y, double z) const {
        proj_errno_reset(transform);
        const PJ_COORD result = proj_trans(transform, PJ_FWD, proj_coord(x, y, z, 0.0));
        if (proj_errno(transform) != 0 || !std::isfinite(result.xyz.x)
            || !std::isfinite(result.xyz.y) || !std::isfinite(result.xyz.z)) {
            throw std::runtime_error("Coordinate projection failed");
        }
        return result;
    }

    void cleanup() noexcept {
        proj_destroy(geographic_to_local_);
        proj_destroy(ecef_to_local_);
        proj_context_destroy(context_);
        geographic_to_local_ = nullptr;
        ecef_to_local_ = nullptr;
        context_ = nullptr;
    }

    PJ_CONTEXT* context_ = nullptr;
    PJ* geographic_to_local_ = nullptr;
    PJ* ecef_to_local_ = nullptr;
};

AuthorizationScope AuthorizationScope::fromWkb(
        const std::vector<std::uint8_t>& wkb, std::int32_t srid) {
    if (srid != kSupportedScopeSrid) {
        throw std::invalid_argument("Only EPSG:4490 authorization scopes are supported");
    }
    auto polygons = WkbReader(wkb).read();
    const auto center = scopeCenter(polygons);
    auto impl = std::make_unique<Impl>(center.first, center.second);
    std::vector<ScopeTriangle> triangles;
    for (const auto& polygon : polygons) {
        std::vector<std::vector<std::array<double, 2>>> projected_polygon;
        std::vector<Point2> flattened;
        projected_polygon.reserve(polygon.size());
        for (const auto& source_ring : polygon) {
            const auto dense_ring = densify(source_ring);
            std::vector<std::array<double, 2>> projected_ring;
            projected_ring.reserve(dense_ring.size());
            for (const auto& geographic : dense_ring) {
                const Point2 projected = impl->projectGeographic(geographic);
                projected_ring.push_back({projected.x, projected.y});
                flattened.push_back(projected);
            }
            projected_polygon.push_back(std::move(projected_ring));
        }
        const auto indices = mapbox::earcut<std::uint32_t>(projected_polygon);
        if (indices.size() % 3U != 0U) {
            throw std::runtime_error("Authorization triangulation returned invalid indices");
        }
        for (std::size_t index = 0; index < indices.size(); index += 3U) {
            triangles.push_back({flattened.at(indices[index]),
                                 flattened.at(indices[index + 1U]),
                                 flattened.at(indices[index + 2U])});
        }
    }
    if (triangles.empty()) {
        throw std::invalid_argument("Authorization scope triangulation is empty");
    }
    return AuthorizationScope(std::move(impl), std::move(triangles));
}

AuthorizationScope AuthorizationScope::fromBase64Wkb(
        const std::string& base64_wkb, std::int32_t srid) {
    return fromWkb(decodeBase64(base64_wkb), srid);
}

AuthorizationScope::AuthorizationScope(std::unique_ptr<Impl> impl,
                                       std::vector<ScopeTriangle> triangles)
    : impl_(std::move(impl)), triangle_index_(std::move(triangles)) {
}

AuthorizationScope::AuthorizationScope(AuthorizationScope&&) noexcept = default;
AuthorizationScope& AuthorizationScope::operator=(AuthorizationScope&&) noexcept = default;
AuthorizationScope::~AuthorizationScope() = default;

const std::vector<ScopeTriangle>& AuthorizationScope::triangles() const noexcept {
    return triangle_index_.triangles();
}

void AuthorizationScope::queryTriangles(
        const ClippedTriangle& source,
        AuthorizationTriangleIndex::QueryWorkspace& workspace) const {
    triangle_index_.query(source, workspace);
}

ProjectedPoint AuthorizationScope::projectEcef(
        const std::array<double, 3>& ecef) const {
    return impl_->projectEcef(ecef);
}

}  // namespace clip_worker::geometry
