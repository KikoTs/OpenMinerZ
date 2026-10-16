// mod-world: the native evaluator of mod world generators
// (openminerz/world/custom_terrain.hpp).
//
// Everything below is a pure function of (world seed, definition, cell): no
// state, no cache, no order dependence, so any thread may generate any chunk
// or far-terrain column at any time and every peer gets the same blocks.
// Arithmetic is IEEE double without contraction (the pragma below) and uses
// only +, -, *, / , sqrt and floor, which are exactly rounded, so one build
// of the game gives the same answer on every machine. Integers are hashed
// with fixed 32/64-bit mixers, never std::hash or a library RNG.

#include "openminerz/world/custom_terrain.hpp"
#include "openminerz/world/far_terrain.hpp" // (fix) far_mod_block_count

#include "openminerz/net/mod_catalog.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace openminerz::world
{
namespace
{

#if defined(_MSC_VER)
#pragma fp_contract(off)
#else
#pragma STDC FP_CONTRACT OFF
#endif

// --- Hashing ------------------------------------------------------------------

[[nodiscard]] constexpr std::uint32_t mix32(std::uint32_t h) noexcept
{
    h ^= h >> 16U;
    h *= 0x7FEB352DU;
    h ^= h >> 15U;
    h *= 0x846CA68BU;
    h ^= h >> 16U;
    return h;
}

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t z) noexcept
{
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] std::uint32_t lattice2(std::uint32_t seed, std::int64_t x, std::int64_t z) noexcept
{
    const std::uint32_t h = mix32(seed ^ (static_cast<std::uint32_t>(x) * 0x9E3779B1U));
    return mix32(h ^ (static_cast<std::uint32_t>(z) * 0x85EBCA77U));
}

[[nodiscard]] std::uint32_t lattice3(std::uint32_t seed, std::int64_t x, std::int64_t y,
                                     std::int64_t z) noexcept
{
    return mix32(lattice2(seed, x, z) ^ (static_cast<std::uint32_t>(y) * 0xC2B2AE3DU));
}

// A cell's (or column's) own random number in [0, 1), one stream per `salt`.
[[nodiscard]] double unit_hash(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t z,
                               std::uint64_t salt) noexcept
{
    std::uint64_t h = mix64(seed ^ salt);
    h = mix64(h ^ static_cast<std::uint64_t>(x) * 0x632BE59BD9B4E019ULL);
    h = mix64(h ^ static_cast<std::uint64_t>(y) * 0x8CB92BA72F3D8DD7ULL);
    h = mix64(h ^ static_cast<std::uint64_t>(z) * 0x9E3779B97F4A7C15ULL);
    return static_cast<double>(h >> 11U) * (1.0 / 9007199254740992.0);
}

// --- Gradient and cellular noise ------------------------------------------------

[[nodiscard]] double fade(double t) noexcept
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] double gradient2(std::uint32_t seed, double x, double z) noexcept
{
    constexpr double diagonal = 0.70710678118654752;
    const double fx = std::floor(x);
    const double fz = std::floor(z);
    const auto ix = static_cast<std::int64_t>(fx);
    const auto iz = static_cast<std::int64_t>(fz);
    const double tx = x - fx;
    const double tz = z - fz;
    const auto dot = [seed](std::int64_t cx, std::int64_t cz, double dx, double dz) noexcept {
        switch (lattice2(seed, cx, cz) & 7U)
        {
        case 0U:
            return dx;
        case 1U:
            return -dx;
        case 2U:
            return dz;
        case 3U:
            return -dz;
        case 4U:
            return (dx + dz) * diagonal;
        case 5U:
            return (dz - dx) * diagonal;
        case 6U:
            return (dx - dz) * diagonal;
        default:
            return (-dx - dz) * diagonal;
        }
    };
    const double n00 = dot(ix, iz, tx, tz);
    const double n10 = dot(ix + 1, iz, tx - 1.0, tz);
    const double n01 = dot(ix, iz + 1, tx, tz - 1.0);
    const double n11 = dot(ix + 1, iz + 1, tx - 1.0, tz - 1.0);
    const double u = fade(tx);
    const double v = fade(tz);
    const double near = n00 + u * (n10 - n00);
    const double far = n01 + u * (n11 - n01);
    return (near + v * (far - near)) * 1.4142135623730951;
}

[[nodiscard]] double gradient3(std::uint32_t seed, double x, double y, double z) noexcept
{
    const double fx = std::floor(x);
    const double fy = std::floor(y);
    const double fz = std::floor(z);
    const auto ix = static_cast<std::int64_t>(fx);
    const auto iy = static_cast<std::int64_t>(fy);
    const auto iz = static_cast<std::int64_t>(fz);
    const double tx = x - fx;
    const double ty = y - fy;
    const double tz = z - fz;
    // Perlin's twelve edge gradients.
    const auto dot = [seed](std::int64_t cx, std::int64_t cy, std::int64_t cz, double dx, double dy,
                            double dz) noexcept {
        switch (lattice3(seed, cx, cy, cz) % 12U)
        {
        case 0U:
            return dx + dy;
        case 1U:
            return dy - dx;
        case 2U:
            return dx - dy;
        case 3U:
            return -dx - dy;
        case 4U:
            return dx + dz;
        case 5U:
            return dz - dx;
        case 6U:
            return dx - dz;
        case 7U:
            return -dx - dz;
        case 8U:
            return dy + dz;
        case 9U:
            return dz - dy;
        case 10U:
            return dy - dz;
        default:
            return -dy - dz;
        }
    };
    const double u = fade(tx);
    const double v = fade(ty);
    const double w = fade(tz);
    const double c000 = dot(ix, iy, iz, tx, ty, tz);
    const double c100 = dot(ix + 1, iy, iz, tx - 1.0, ty, tz);
    const double c010 = dot(ix, iy + 1, iz, tx, ty - 1.0, tz);
    const double c110 = dot(ix + 1, iy + 1, iz, tx - 1.0, ty - 1.0, tz);
    const double c001 = dot(ix, iy, iz + 1, tx, ty, tz - 1.0);
    const double c101 = dot(ix + 1, iy, iz + 1, tx - 1.0, ty, tz - 1.0);
    const double c011 = dot(ix, iy + 1, iz + 1, tx, ty - 1.0, tz - 1.0);
    const double c111 = dot(ix + 1, iy + 1, iz + 1, tx - 1.0, ty - 1.0, tz - 1.0);
    const double x00 = c000 + u * (c100 - c000);
    const double x10 = c010 + u * (c110 - c010);
    const double x01 = c001 + u * (c101 - c001);
    const double x11 = c011 + u * (c111 - c011);
    const double y0 = x00 + v * (x10 - x00);
    const double y1 = x01 + v * (x11 - x01);
    return y0 + w * (y1 - y0);
}

struct CellularResult final
{
    double first{};
    double second{};
    std::uint32_t cell{};
};

[[nodiscard]] CellularResult cellular2(std::uint32_t seed, double x, double z,
                                       double jitter) noexcept
{
    const auto ix = static_cast<std::int64_t>(std::floor(x));
    const auto iz = static_cast<std::int64_t>(std::floor(z));
    CellularResult result{8.0, 8.0, 0U};
    for (std::int64_t dz = -1; dz <= 1; ++dz)
        for (std::int64_t dx = -1; dx <= 1; ++dx)
        {
            const std::uint32_t h = lattice2(seed, ix + dx, iz + dz);
            const double px = static_cast<double>(ix + dx) + 0.5 +
                              jitter * (static_cast<double>(h & 0xFFFFU) / 65535.0 - 0.5);
            const double pz = static_cast<double>(iz + dz) + 0.5 +
                              jitter * (static_cast<double>(h >> 16U) / 65535.0 - 0.5);
            const double distance = std::sqrt((px - x) * (px - x) + (pz - z) * (pz - z));
            if (distance < result.first)
            {
                result.second = result.first;
                result.first = distance;
                result.cell = h;
            }
            else if (distance < result.second)
                result.second = distance;
        }
    return result;
}

[[nodiscard]] CellularResult cellular3(std::uint32_t seed, double x, double y, double z,
                                       double jitter) noexcept
{
    const auto ix = static_cast<std::int64_t>(std::floor(x));
    const auto iy = static_cast<std::int64_t>(std::floor(y));
    const auto iz = static_cast<std::int64_t>(std::floor(z));
    CellularResult result{8.0, 8.0, 0U};
    for (std::int64_t dy = -1; dy <= 1; ++dy)
        for (std::int64_t dz = -1; dz <= 1; ++dz)
            for (std::int64_t dx = -1; dx <= 1; ++dx)
            {
                const std::uint32_t h = lattice3(seed, ix + dx, iy + dy, iz + dz);
                const std::uint32_t h2 = mix32(h);
                const double px = static_cast<double>(ix + dx) + 0.5 +
                                  jitter * (static_cast<double>(h & 0xFFFFU) / 65535.0 - 0.5);
                const double py = static_cast<double>(iy + dy) + 0.5 +
                                  jitter * (static_cast<double>(h >> 16U) / 65535.0 - 0.5);
                const double pz = static_cast<double>(iz + dz) + 0.5 +
                                  jitter * (static_cast<double>(h2 & 0xFFFFU) / 65535.0 - 0.5);
                const double distance = std::sqrt((px - x) * (px - x) + (py - y) * (py - y) +
                                                  (pz - z) * (pz - z));
                if (distance < result.first)
                {
                    result.second = result.first;
                    result.first = distance;
                    result.cell = h;
                }
                else if (distance < result.second)
                    result.second = distance;
            }
    return result;
}

[[nodiscard]] double smoothstep(double t) noexcept
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// --- Retail approximations for blend zones ---------------------------------------

struct RetailLook final
{
    std::array<TerrainLayer, 2> surface;
    std::size_t surface_count{};
    TerrainLayer underwater;
};

[[nodiscard]] RetailLook retail_look(BiomeKind kind) noexcept
{
    switch (kind)
    {
    case BiomeKind::desert:
        return {{TerrainLayer{{BlockType::sand}, 3}, {}}, 1U, {{BlockType::sand}, 3}};
    case BiomeKind::mountain:
    case BiomeKind::descent:
        return {{TerrainLayer{{BlockType::rock}, 1}, {}}, 1U, {{BlockType::sand}, 2}};
    case BiomeKind::arctic:
        return {{TerrainLayer{{BlockType::snow}, 1}, TerrainLayer{{BlockType::ice}, 1}}, 2U,
                {{BlockType::ice}, 1}};
    case BiomeKind::classic:
    case BiomeKind::lagoon:
        break;
    }
    return {{TerrainLayer{{BlockType::grass}, 1}, TerrainLayer{{BlockType::dirt}, 3}}, 2U,
            {{BlockType::sand}, 3}};
}

[[nodiscard]] BiomeKind reuse_kind(RetailReuse reuse) noexcept
{
    switch (reuse)
    {
    case RetailReuse::lagoon:
        return BiomeKind::lagoon;
    case RetailReuse::desert:
        return BiomeKind::desert;
    case RetailReuse::mountain:
        return BiomeKind::mountain;
    case RetailReuse::arctic:
        return BiomeKind::arctic;
    case RetailReuse::descent:
        return BiomeKind::descent;
    default:
        return BiomeKind::classic;
    }
}

[[nodiscard]] std::string double_bits(double value)
{
    return std::format("{:016x}", std::bit_cast<std::uint64_t>(value));
}

constexpr std::uint64_t dither_salt = 0xD17E5A17ULL;
constexpr std::uint64_t tree_salt = 0x7EE5A17ULL;
constexpr std::uint64_t bedrock_salt = 0xBEDB0C4ULL;
constexpr int maximum_tree_reach = 8;

} // namespace

// ---------------------------------------------------------------------------
// The compiled terrain
// ---------------------------------------------------------------------------

class CustomTerrain::Impl final
{
  public:
    explicit Impl(TerrainDefinition source) : definition(std::move(source))
    {
        validate();
        for (const TerrainBiome &biome : definition.biomes)
        {
            reuses_retail = reuses_retail || biome.retail != RetailReuse::none;
            has_fluid = has_fluid ||
                        (biome.retail == RetailReuse::none && biome.fluid.block != BlockType::empty);
            double chance{};
            for (const TerrainTree &tree : biome.trees)
            {
                chance += tree.chance;
                tree_reach = std::max(tree_reach, reach_of(tree));
            }
            maximum_tree_chance = std::max(maximum_tree_chance, std::min(chance, 1.0));
        }
        build_far_profiles();
        digest = net::digest_hex(net::sha256_digest(std::span{
            reinterpret_cast<const std::uint8_t *>(canonical.data()), canonical.size()}));
    }

    TerrainDefinition definition;
    std::string canonical;
    std::string digest;
    int tree_reach{};
    double maximum_tree_chance{};
    bool reuses_retail{};
    bool has_fluid{}; // (wave 5) some biome fills its hollows: caves keep away
    // (fix, wave 5) The far landscape's side profiles, and per biome the one of
    // its surface and of its underwater layers (1-based; 0: none).
    std::vector<FarSurfaceProfile> far_profiles;
    std::vector<std::array<std::uint8_t, 2>> biome_profiles;

    // How the side of a run of `layers` over the biome's filler reads: the
    // second cell's block, and rock from where the layers end (if the filler
    // is rock). Byte values above the palette's are shared with it
    // (far_terrain.hpp), so the list stops where they would meet.
    void build_far_profiles()
    {
        constexpr auto byte_room = static_cast<std::size_t>(far_mod_block_count);
        const std::size_t room =
            byte_room > definition.palette.size() ? byte_room - definition.palette.size() : 0U;
        const auto profile_of = [&](const TerrainBiome &biome,
                                    const std::vector<TerrainLayer> &layers) -> std::uint8_t {
            if (layers.empty())
                return 0U;
            FarSurfaceProfile profile;
            profile.top = layers.front().block;
            profile.under = layers.front().depth > 1 ? layers.front().block
                            : layers.size() > 1U     ? layers[1].block
                                                     : biome.filler;
            int depth{};
            for (const TerrainLayer &layer : layers)
                depth += layer.depth;
            profile.soil = biome.filler == TerrainBlock{BlockType::rock}
                               ? static_cast<std::uint8_t>(std::min(depth, 255))
                               : std::uint8_t{};
            const auto found = std::find(far_profiles.begin(), far_profiles.end(), profile);
            if (found != far_profiles.end())
                return static_cast<std::uint8_t>(found - far_profiles.begin() + 1);
            if (far_profiles.size() >= room)
                return 0U;
            far_profiles.push_back(profile);
            return static_cast<std::uint8_t>(far_profiles.size());
        };
        biome_profiles.reserve(definition.biomes.size());
        for (const TerrainBiome &biome : definition.biomes)
        {
            std::array<std::uint8_t, 2> entry{};
            if (biome.retail == RetailReuse::none)
            {
                entry[0] = profile_of(biome, biome.surface);
                entry[1] = biome.underwater.empty() ? entry[0] : profile_of(biome, biome.underwater);
            }
            biome_profiles.push_back(entry);
        }
    }

    // --- expressions -------------------------------------------------------------

    struct Point final
    {
        double x{};
        double y{};
        double z{};
        std::uint32_t seed{};
    };

    [[nodiscard]] static std::uint32_t node_seed(const NoiseNode &node, std::uint32_t world,
                                                 int octave) noexcept
    {
        return mix32(world * 0x9E3779B1U ^ mix32(node.seed + 0x51ED270BU) ^
                     static_cast<std::uint32_t>(octave) * 0x632BE5ABU);
    }

    [[nodiscard]] double octaves(const NoiseNode &node, const Point &point) const noexcept
    {
        const double horizontal = 1.0 / node.scale;
        const double vertical =
            1.0 / (node.vertical_scale > 0.0 ? node.vertical_scale : node.scale);
        double frequency = 1.0;
        double amplitude = 1.0;
        double sum{};
        double norm{};
        for (int octave = 0; octave < node.octaves; ++octave)
        {
            const std::uint32_t seed = node_seed(node, point.seed, octave);
            const double n =
                node.three_d ? gradient3(seed, point.x * horizontal * frequency,
                                         point.y * vertical * frequency,
                                         point.z * horizontal * frequency)
                             : gradient2(seed, point.x * horizontal * frequency,
                                         point.z * horizontal * frequency);
            double term = n;
            if (node.op == NoiseOp::ridged)
            {
                term = 1.0 - std::abs(n);
                term *= term;
            }
            else if (node.op == NoiseOp::billow)
                term = std::abs(n) * 2.0 - 1.0;
            sum += term * amplitude;
            norm += amplitude;
            amplitude *= node.gain;
            frequency *= node.lacunarity;
        }
        return norm > 0.0 ? sum / norm : 0.0;
    }

    [[nodiscard]] double evaluate(NoiseRef reference, const Point &point) const noexcept
    {
        if (reference >= definition.nodes.size())
            return 0.0;
        const NoiseNode &node = definition.nodes[reference];
        const auto input = [&](std::size_t index) noexcept {
            return index < node.inputs.size() ? evaluate(node.inputs[index], point) : 0.0;
        };
        switch (node.op)
        {
        case NoiseOp::constant:
            return node.value;
        case NoiseOp::coord_x:
            return point.x;
        case NoiseOp::coord_y:
            return point.y;
        case NoiseOp::coord_z:
            return point.z;
        case NoiseOp::distance:
            return std::sqrt(point.x * point.x + point.z * point.z);
        case NoiseOp::fbm:
        case NoiseOp::ridged:
        case NoiseOp::billow:
            return octaves(node, point);
        case NoiseOp::cellular: {
            const double horizontal = 1.0 / node.scale;
            const double vertical =
                1.0 / (node.vertical_scale > 0.0 ? node.vertical_scale : node.scale);
            const std::uint32_t seed = node_seed(node, point.seed, 0);
            const CellularResult cell =
                node.three_d ? cellular3(seed, point.x * horizontal, point.y * vertical,
                                         point.z * horizontal, node.jitter)
                             : cellular2(seed, point.x * horizontal, point.z * horizontal,
                                         node.jitter);
            if (node.cellular == CellularMode::edge)
                return cell.second - cell.first;
            if (node.cellular == CellularMode::cell)
                return static_cast<double>(mix32(cell.cell) >> 8U) * (1.0 / 16777216.0);
            return cell.first;
        }
        case NoiseOp::add: {
            double sum{};
            for (const NoiseRef child : node.inputs)
                sum += evaluate(child, point);
            return sum;
        }
        case NoiseOp::multiply: {
            double product = 1.0;
            for (const NoiseRef child : node.inputs)
                product *= evaluate(child, point);
            return product;
        }
        case NoiseOp::subtract:
            return input(0) - input(1);
        case NoiseOp::minimum: {
            double result = std::numeric_limits<double>::infinity();
            for (const NoiseRef child : node.inputs)
                result = std::min(result, evaluate(child, point));
            return node.inputs.empty() ? 0.0 : result;
        }
        case NoiseOp::maximum: {
            double result = -std::numeric_limits<double>::infinity();
            for (const NoiseRef child : node.inputs)
                result = std::max(result, evaluate(child, point));
            return node.inputs.empty() ? 0.0 : result;
        }
        case NoiseOp::absolute:
            return std::abs(input(0));
        case NoiseOp::clamp:
            return std::clamp(input(0), node.low, node.high);
        case NoiseOp::lerp: {
            const double from = input(0);
            return from + (input(1) - from) * input(2);
        }
        case NoiseOp::curve:
            return curve(node, input(0));
        case NoiseOp::select: {
            const double selector = input(0);
            if (selector <= node.low)
                return input(1);
            if (selector >= node.high)
                return input(2);
            const double t = smoothstep((selector - node.low) / (node.high - node.low));
            const double below = input(1);
            return below + (input(2) - below) * t;
        }
        }
        return 0.0;
    }

    [[nodiscard]] static double curve(const NoiseNode &node, double value) noexcept
    {
        const auto &points = node.points;
        if (points.empty())
            return value;
        if (value <= points.front()[0])
            return points.front()[1];
        if (value >= points.back()[0])
            return points.back()[1];
        std::size_t upper = 1U;
        while (upper < points.size() && points[upper][0] < value)
            ++upper;
        const std::size_t lower = upper - 1U;
        const double span = points[upper][0] - points[lower][0];
        const double t = span > 0.0 ? (value - points[lower][0]) / span : 0.0;
        if (!node.smooth)
            return points[lower][1] + (points[upper][1] - points[lower][1]) * t;
        // Cubic Hermite with Catmull-Rom tangents (one-sided at the ends).
        const auto slope = [&](std::size_t index) noexcept {
            const std::size_t before = index == 0U ? 0U : index - 1U;
            const std::size_t after = std::min(index + 1U, points.size() - 1U);
            const double run = points[after][0] - points[before][0];
            return run > 0.0 ? (points[after][1] - points[before][1]) / run : 0.0;
        };
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
        const double h10 = t3 - 2.0 * t2 + t;
        const double h01 = -2.0 * t3 + 3.0 * t2;
        const double h11 = t3 - t2;
        return h00 * points[lower][1] + h10 * span * slope(lower) + h01 * points[upper][1] +
               h11 * span * slope(upper);
    }

    // --- the layout ------------------------------------------------------------------

    struct Weights final
    {
        std::array<BiomeWeight, maximum_biome_weights> entries{};
        std::size_t count{};

        void add(std::uint16_t biome, double weight) noexcept
        {
            if (!(weight > 1e-6))
                return;
            for (std::size_t index = 0U; index < count; ++index)
                if (entries[index].biome == biome)
                {
                    entries[index].weight += static_cast<float>(weight);
                    return;
                }
            if (count < entries.size())
            {
                entries[count++] = {biome, static_cast<float>(weight)};
                return;
            }
            // Full: the lightest entry gives way to a heavier one.
            std::size_t lightest{};
            for (std::size_t index = 1U; index < count; ++index)
                if (entries[index].weight < entries[lightest].weight)
                    lightest = index;
            if (entries[lightest].weight < static_cast<float>(weight))
                entries[lightest] = {biome, static_cast<float>(weight)};
        }

        void finish() noexcept
        {
            float total{};
            for (std::size_t index = 0U; index < count; ++index)
                total += entries[index].weight;
            if (total > 0.0F)
                for (std::size_t index = 0U; index < count; ++index)
                    entries[index].weight /= total;
            std::sort(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(count),
                      [](const BiomeWeight &a, const BiomeWeight &b) {
                          return a.weight != b.weight ? a.weight > b.weight : a.biome < b.biome;
                      });
        }
    };

    [[nodiscard]] double layout_input(const TerrainLayout &layout, const Point &point) const noexcept
    {
        if (layout.input == LayoutInput::noise)
            return evaluate(layout.select, point);
        double radius = std::sqrt(point.x * point.x + point.z * point.z);
        if (layout.warp != no_noise)
            radius = std::max(0.0, radius + evaluate(layout.warp, point));
        const double period = layout_period(layout);
        if (layout.beyond == LayoutBeyond::extend || !(period > 0.0) || radius <= period)
            return radius;
        const double rings = std::floor(radius / period);
        double remainder = radius - rings * period;
        if (layout.beyond == LayoutBeyond::mirror &&
            (static_cast<std::int64_t>(rings) & 1) != 0)
            remainder = period - remainder;
        return remainder;
    }

    [[nodiscard]] static double layout_period(const TerrainLayout &layout) noexcept
    {
        if (layout.period > 0.0)
            return layout.period;
        return layout.bands.empty() ? 0.0 : layout.bands.back().to;
    }

    void layout_weights(std::size_t index, const Point &point, double factor, Weights &weights,
                        int depth) const noexcept
    {
        if (index >= definition.layouts.size() || depth > 8)
            return;
        const TerrainLayout &layout = definition.layouts[index];
        const std::size_t count = layout.bands.size();
        if (count == 0U)
            return;
        const double value = layout_input(layout, point);
        // c[i]: how far past the boundary into band i the value is (1 before
        // band 0); band i's weight is c[i] - c[i + 1].
        const auto past = [&](std::size_t band) noexcept {
            if (band == 0U)
                return 1.0;
            if (band >= count)
                return 0.0;
            const double boundary = layout.bands[band - 1U].to;
            const double width = layout.bands[band].blend;
            if (!(width > 0.0))
                return value >= boundary ? 1.0 : 0.0;
            return smoothstep((value - (boundary - width * 0.5)) / width);
        };
        double here = past(0U);
        for (std::size_t band = 0U; band < count; ++band)
        {
            const double next = past(band + 1U);
            const double weight = std::max(0.0, here - next) * factor;
            here = next;
            if (!(weight > 1e-6))
                continue;
            const LayoutBand &entry = layout.bands[band];
            if (entry.layout >= 0)
                layout_weights(static_cast<std::size_t>(entry.layout), point, weight, weights,
                               depth + 1);
            else if (entry.biome >= 0)
                weights.add(static_cast<std::uint16_t>(entry.biome), weight);
        }
    }

    // --- a column's plan -------------------------------------------------------------

    struct Plan final
    {
        Weights weights;
        std::uint16_t pick{};
        bool pure_retail{};
    };

    [[nodiscard]] Plan plan(std::uint32_t seed, std::int64_t x, std::int64_t z) const noexcept
    {
        Plan result;
        const Point point{static_cast<double>(x), 0.0, static_cast<double>(z), seed};
        layout_weights(0U, point, 1.0, result.weights, 0);
        result.weights.finish();
        if (result.weights.count == 0U)
        {
            result.weights.entries[0] = {0U, 1.0F};
            result.weights.count = 1U;
        }
        // Materials, trees, caves and ores follow one biome per column, picked
        // with the weights as odds: a blend reads as a ragged edge, not a
        // stripe. The roll is a smooth field (patches a few blocks across),
        // not one coin per column, so a wide blend does not turn to static.
        result.pick = result.weights.entries[0].biome;
        if (result.weights.count > 1U)
        {
            const double field = gradient2(mix32(seed ^ static_cast<std::uint32_t>(dither_salt)),
                                           static_cast<double>(x) / 7.0, static_cast<double>(z) / 7.0) +
                                 0.35 * gradient2(mix32(seed + static_cast<std::uint32_t>(dither_salt)),
                                                  static_cast<double>(x) / 2.5, static_cast<double>(z) / 2.5);
            const double roll = std::clamp(0.5 + field * 0.5, 0.0, 0.999999);
            double cumulative{};
            for (std::size_t index = 0U; index < result.weights.count; ++index)
            {
                cumulative += result.weights.entries[index].weight;
                if (roll < cumulative)
                {
                    result.pick = result.weights.entries[index].biome;
                    break;
                }
            }
        }
        result.pure_retail = result.weights.count == 1U &&
                             definition.biomes[result.pick].retail != RetailReuse::none;
        return result;
    }

    // --- shapes ------------------------------------------------------------------------

    [[nodiscard]] double biome_height(const WorldGenerator &retail, const TerrainBiome &biome,
                                      const Point &point) const noexcept
    {
        const auto x = static_cast<int>(point.x);
        const auto z = static_cast<int>(point.z);
        if (biome.retail == RetailReuse::world)
            return static_cast<double>(retail.retail_surface_column(x, z, false).top_local_y +
                                       world_min_y);
        if (biome.retail != RetailReuse::none)
        {
            std::array<BlockType, chunk_height> cells{};
            retail.retail_biome_column(reuse_kind(biome.retail), x, z, cells);
            int top = chunk_height - 1;
            while (top > 0 && (cells[static_cast<std::size_t>(top)] == BlockType::empty ||
                               cells[static_cast<std::size_t>(top)] == BlockType::uninitialized))
                --top;
            return static_cast<double>(top + world_min_y);
        }
        return evaluate(biome.height, point);
    }

    struct Shape final
    {
        double height{};
        double reach{}; // how far density can move the surface either way
        std::array<std::pair<const TerrainBiome *, double>, maximum_biome_weights> density{};
        std::size_t density_count{};
    };

    [[nodiscard]] Shape shape(const WorldGenerator &retail, const Plan &plan,
                              const Point &point) const noexcept
    {
        Shape result;
        for (std::size_t index = 0U; index < plan.weights.count; ++index)
        {
            const TerrainBiome &biome = definition.biomes[plan.weights.entries[index].biome];
            const double weight = plan.weights.entries[index].weight;
            result.height += weight * biome_height(retail, biome, point);
            if (biome.density != no_noise && biome.density_strength > 0.0)
            {
                result.density[result.density_count++] = {&biome, weight * biome.density_strength};
                result.reach += weight * biome.density_strength;
            }
        }
        return result;
    }

    [[nodiscard]] bool solid(const Shape &shape, const Point &column, int world_y) const noexcept
    {
        const double y = static_cast<double>(world_y);
        if (shape.density_count == 0U)
            return y <= shape.height;
        if (y <= shape.height - shape.reach)
            return true;
        if (y > shape.height + shape.reach)
            return false;
        double offset{};
        const Point point{column.x, y, column.z, column.seed};
        for (std::size_t index = 0U; index < shape.density_count; ++index)
            offset += shape.density[index].second *
                      std::clamp(evaluate(shape.density[index].first->density, point), -1.0, 1.0);
        return y <= shape.height + offset;
    }

    // (wave 5) The lowest cell a column's fluid fills (world Y), or no_fluid:
    // the cells above its top that are under the sea level, when its biome
    // has a fluid (build fills exactly those).
    static constexpr int no_fluid = std::numeric_limits<int>::max();
    [[nodiscard]] int fluid_floor(const Plan &plan, const Shape &form, const Point &point) const noexcept
    {
        if (plan.pure_retail || definition.biomes[plan.pick].fluid.block == BlockType::empty)
            return no_fluid;
        int top = chunk_height - 1;
        while (top >= 0 && !solid(form, point, top + world_min_y))
            --top;
        const int top_y = top + world_min_y;
        return top_y < definition.sea_level ? top_y + 1 : no_fluid;
    }

    [[nodiscard]] int fluid_floor_at(const WorldGenerator &retail, std::uint32_t seed, int x,
                                     int z) const noexcept
    {
        if (!has_fluid)
            return no_fluid;
        const Plan column_plan = plan(seed, x, z);
        if (column_plan.pure_retail)
            return no_fluid;
        const Point point{static_cast<double>(x), 0.0, static_cast<double>(z), seed};
        return fluid_floor(column_plan, shape(retail, column_plan, point), point);
    }

    // A column's neighbours' fluid floors, worked out the first time a cave
    // asks (build's `floors` for a column outside a chunk: far columns).
    struct LazyFloors final
    {
        const Impl &impl;
        const WorldGenerator &retail;
        std::uint32_t seed{};
        int x{};
        int z{};
        std::array<int, 4> floors{};
        std::array<bool, 4> known{};

        int operator()(int direction) noexcept
        {
            static constexpr std::array<std::array<int, 2>, 4> steps{
                std::array{1, 0}, std::array{-1, 0}, std::array{0, 1}, std::array{0, -1}};
            const auto index = static_cast<std::size_t>(direction);
            if (!known[index])
            {
                floors[index] =
                    impl.fluid_floor_at(retail, seed, x + steps[index][0], z + steps[index][1]);
                known[index] = true;
            }
            return floors[index];
        }
    };

    // --- caves and ores ------------------------------------------------------------------

    [[nodiscard]] bool carved(const TerrainCaves &caves, std::uint32_t seed, int x, int y,
                              int z) const noexcept
    {
        if (!caves.enabled || y < caves.y_min || y > caves.y_max)
            return false;
        const std::uint32_t base = mix32(seed * 0x9E3779B1U ^ mix32(caves.seed + 0xCA7E5U));
        const double scale = 1.0 / caves.scale;
        const double fx = static_cast<double>(x) * scale;
        const double fy = static_cast<double>(y) * scale * 1.6;
        const double fz = static_cast<double>(z) * scale;
        // Caverns: two octaves under the threshold.
        const double cavern = gradient3(base, fx, fy, fz) * 0.66 +
                              gradient3(mix32(base + 1U), fx * 2.0, fy * 2.0, fz * 2.0) * 0.34;
        if (cavern < caves.threshold)
            return true;
        if (!caves.tunnels)
            return false;
        // Tunnels: where two independent fields both cross zero.
        const double first = gradient3(mix32(base + 2U), fx * 1.5, fy * 1.5, fz * 1.5);
        if (std::abs(first) > caves.width)
            return false;
        const double second = gradient3(mix32(base + 3U), fx * 1.5, fy * 1.5, fz * 1.5);
        return std::abs(second) <= caves.width;
    }

    [[nodiscard]] static bool ore_in(const TerrainOre &ore, const TerrainBiome &biome,
                                     TerrainBlock block) noexcept
    {
        if (ore.in.empty())
            return block == biome.filler;
        return std::find(ore.in.begin(), ore.in.end(), block) != ore.in.end();
    }

    // The blob of one lattice cube of an ore: each cube of `spacing` blocks
    // holds one with probability `chance`, wholly inside it.
    struct OreBlob final
    {
        int cube_y{std::numeric_limits<int>::min()};
        bool present{};
        double x{};
        double y{};
        double z{};
    };

    [[nodiscard]] static int ore_cube(int value, int spacing) noexcept
    {
        return value >= 0 ? value / spacing : (value - spacing + 1) / spacing;
    }

    [[nodiscard]] static OreBlob ore_blob(const TerrainOre &ore, std::uint32_t seed, int cx, int cy,
                                          int cz) noexcept
    {
        OreBlob blob;
        blob.cube_y = cy;
        const int spacing = ore.spacing;
        const std::uint64_t ore_seed = mix64(static_cast<std::uint64_t>(seed) << 32U ^ ore.seed);
        if (unit_hash(ore_seed, cx, cy, cz, 0x0AE5ULL) >= ore.chance)
            return blob;
        const double room = std::max(0.0, static_cast<double>(spacing) - 2.0 * ore.size - 1.0);
        blob.present = true;
        blob.x = cx * spacing + ore.size + 0.5 + room * unit_hash(ore_seed, cx, cy, cz, 0x0AE6ULL);
        blob.y = cy * spacing + ore.size + 0.5 + room * unit_hash(ore_seed, cx, cy, cz, 0x0AE7ULL);
        blob.z = cz * spacing + ore.size + 0.5 + room * unit_hash(ore_seed, cx, cy, cz, 0x0AE8ULL);
        return blob;
    }

    // Whether the cell is in its cube's blob. `cache` holds the blob of the
    // cube the column last asked about: a column walks its cells bottom up,
    // so each cube is worked out once, not once per cell (wave 5: ores were
    // a third of a mountain chunk's time).
    [[nodiscard]] static bool ore_at(const TerrainOre &ore, std::uint32_t seed, int x, int y, int z,
                                     OreBlob &cache) noexcept
    {
        if (y < ore.y_min || y > ore.y_max)
            return false;
        const int cy = ore_cube(y, ore.spacing);
        if (cache.cube_y != cy)
            cache = ore_blob(ore, seed, ore_cube(x, ore.spacing), cy, ore_cube(z, ore.spacing));
        if (!cache.present)
            return false;
        const double dx = x + 0.5 - cache.x;
        const double dy = (y + 0.5 - cache.y) * 1.4;
        const double dz = z + 0.5 - cache.z;
        const double radius = ore.size + 0.35;
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }

    // --- one column ----------------------------------------------------------------------

    struct Column final
    {
        std::array<BlockType, chunk_height> blocks{};
        std::array<std::uint8_t, chunk_height> mods{};
        int top{-1};          // local Y of the highest cell of the shape
        int under{-1};        // the lowest cell of the top span when air lies below it
        int lower{-1};        // the top of the ground under that air
    };

    // (fix, wave 5) The far side profile of the solid run that starts at
    // `local_y` (build() layers every run from its own top), while that cell
    // still holds the run's top layer - an ore may have taken it.
    [[nodiscard]] std::uint8_t profile_at(const Plan &plan, const Column &column,
                                          int local_y) const noexcept
    {
        const std::array<std::uint8_t, 2> &entry = biome_profiles[plan.pick];
        const std::uint8_t profile = entry[local_y + world_min_y <= definition.sea_level ? 1U : 0U];
        if (profile == 0U)
            return 0U;
        const TerrainBlock &top = far_profiles[profile - 1U].top;
        const auto index = static_cast<std::size_t>(local_y);
        return column.blocks[index] == top.block && column.mods[index] == top.mod ? profile
                                                                                  : std::uint8_t{};
    }

    void set(Column &column, int local_y, TerrainBlock block) const noexcept
    {
        column.blocks[static_cast<std::size_t>(local_y)] = block.block;
        column.mods[static_cast<std::size_t>(local_y)] = block.mod;
    }

    [[nodiscard]] TerrainBlock layer_block(const TerrainBiome *biome, BiomeKind kind, int depth,
                                           bool underwater) const noexcept
    {
        if (biome != nullptr && biome->retail == RetailReuse::none)
        {
            const auto &layers =
                underwater && !biome->underwater.empty() ? biome->underwater : biome->surface;
            for (const TerrainLayer &layer : layers)
            {
                if (depth < layer.depth)
                    return layer.block;
                depth -= layer.depth;
            }
            return biome->filler;
        }
        const RetailLook look = retail_look(kind);
        if (underwater)
            return depth < look.underwater.depth ? look.underwater.block
                                                 : TerrainBlock{BlockType::rock};
        for (std::size_t index = 0U; index < look.surface_count; ++index)
        {
            if (depth < look.surface[index].depth)
                return look.surface[index].block;
            depth -= look.surface[index].depth;
        }
        return {BlockType::rock};
    }

    [[nodiscard]] BiomeKind pick_kind(const WorldGenerator &retail, const TerrainBiome &biome,
                                      int x, int z) const noexcept
    {
        if (biome.retail == RetailReuse::world)
            return retail.retail_biome_at(x, z);
        if (biome.retail != RetailReuse::none)
            return reuse_kind(biome.retail);
        return biome.like;
    }

    // Builds a custom column (not a pure retail one). With `surface_only`
    // caves are resolved only from the top down to the first cell that
    // survives (and again under an overhang), which is all a far-terrain
    // summary reads; ores only for those cells.
    // `floors(direction)` gives the fluid_floor of the neighbour column at
    // +x, -x, +z or -z (directions 0 to 3); it is asked only when a cave
    // would open a cell at or under the sea level.
    template <typename Floors>
    void build(const WorldGenerator &retail, const Plan &plan, const Shape &form, std::uint32_t seed,
               int x, int z, Column &column, bool surface_only, bool tower, Floors &&floors) const noexcept
    {
        const Point point{static_cast<double>(x), 0.0, static_cast<double>(z), seed};
        const TerrainBiome &picked = definition.biomes[plan.pick];
        const TerrainBiome *materials = picked.retail == RetailReuse::none ? &picked : nullptr;
        const BiomeKind kind = pick_kind(retail, picked, x, z);
        // The shape, top down, with each solid run's material by its depth.
        bool previous_solid{};
        int depth{};
        bool run_underwater{};
        int highest = -1;
        int first_gap_bottom = -1; // the lowest cell of the top span, if air follows
        int ground_top = -1;       // the next span's top under that air
        for (int local_y = chunk_height - 1; local_y >= 0; --local_y)
        {
            const int world_y = local_y + world_min_y;
            const bool here = solid(form, point, world_y);
            if (!here)
            {
                if (previous_solid && highest >= 0 && first_gap_bottom < 0)
                    first_gap_bottom = local_y + 1;
                previous_solid = false;
                if (highest < 0 && world_y <= definition.sea_level && picked.fluid.block != BlockType::empty)
                    set(column, local_y, picked.fluid);
                continue;
            }
            if (!previous_solid)
            {
                depth = 0;
                run_underwater = world_y <= definition.sea_level;
                if (highest < 0)
                    highest = local_y;
                else if (first_gap_bottom >= 0 && ground_top < 0)
                    ground_top = local_y;
            }
            set(column, local_y, layer_block(materials, kind, depth, run_underwater));
            ++depth;
            previous_solid = true;
        }
        column.top = highest;
        if (first_gap_bottom >= 0 && ground_top >= 0)
        {
            column.under = first_gap_bottom;
            column.lower = ground_top;
        }
        // Bedrock and the origin tower.
        const int bedrock = 1 + static_cast<int>(unit_hash(seed, x, 0, z, bedrock_salt) *
                                                 std::max(definition.bedrock_depth, 1));
        if (definition.bedrock_depth > 0)
            for (int local_y = 0; local_y < bedrock && local_y < chunk_height; ++local_y)
                set(column, local_y, {BlockType::bedrock});
        // Caves.
        const TerrainCaves &caves = picked.retail == RetailReuse::none ? picked.caves : default_caves;
        // Caves open only the shape: never the fluid above it, the bedrock,
        // or (without breach_surface) a four-cell lid under the top.
        int lid = caves.breach_surface ? highest + 1 : highest - 3;
        // (wave 5) Nor next to a fluid, which would hang over or beside the
        // air (nothing flows): a column's own fluid keeps a floor two cells
        // thick, and no cell beside a neighbour's fluid opens.
        if (highest >= 0 && picked.fluid.block != BlockType::empty &&
            highest + world_min_y < definition.sea_level)
            lid = std::min(lid, highest - 1);
        const auto cave_cell = [&](int local_y) noexcept {
            const std::size_t index = static_cast<std::size_t>(local_y);
            const BlockType block = column.blocks[index];
            if (local_y > highest || local_y >= lid || block == BlockType::empty ||
                block == BlockType::bedrock)
                return false;
            const int world_y = local_y + world_min_y;
            if (!carved(caves, seed, x, world_y, z))
                return false;
            if (world_y <= definition.sea_level)
                for (int direction = 0; direction < 4; ++direction)
                    if (floors(direction) <= world_y)
                        return false;
            column.blocks[index] = BlockType::empty;
            column.mods[index] = 0U;
            return true;
        };
        std::array<OreBlob, 32> ore_blobs{}; // biomes have at most 32 ores
        const auto ore_cell = [&](int local_y) noexcept {
            if (materials == nullptr)
                return;
            const std::size_t index = static_cast<std::size_t>(local_y);
            const TerrainBlock current{column.blocks[index], column.mods[index]};
            if (current.block == BlockType::empty)
                return;
            for (std::size_t which = 0U; which < materials->ores.size(); ++which)
            {
                const TerrainOre &ore = materials->ores[which];
                if (ore_in(ore, *materials, current) &&
                    ore_at(ore, seed, x, local_y + world_min_y, z, ore_blobs[which]))
                {
                    set(column, local_y, ore.block);
                    return;
                }
            }
        };
        if (!surface_only)
        {
            for (int local_y = 0; local_y < chunk_height; ++local_y)
                if (!cave_cell(local_y))
                    ore_cell(local_y);
        }
        else
        {
            // The top, then (under an overhang) the ground beneath.
            const auto settle = [&](int from) noexcept {
                int local_y = from;
                while (local_y >= 0 && (column.blocks[static_cast<std::size_t>(local_y)] ==
                                            BlockType::empty ||
                                        cave_cell(local_y)))
                    --local_y;
                if (local_y >= 0)
                    ore_cell(local_y);
                return local_y;
            };
            column.top = settle(chunk_height - 1);
            if (column.under >= 0 && column.lower >= 0)
            {
                column.lower = settle(column.lower);
                if (column.lower < 0 || column.lower >= column.under)
                    column.under = column.lower = -1;
            }
        }
        if (tower && definition.origin_tower)
            origin_tower(column, x, z);
    }

    // Retail's OriginBiome: a bedrock tower stepping down from 125 cells,
    // with a lantern on the middle columns.
    static void origin_tower(Column &column, int x, int z) noexcept
    {
        const std::int64_t squared = static_cast<std::int64_t>(x) * x + static_cast<std::int64_t>(z) * z;
        if (squared >= 25)
            return;
        const int radius = static_cast<int>(std::nearbyint(std::sqrt(static_cast<double>(squared))));
        if (radius > 4)
            return;
        const int height = 125 - radius * 8;
        for (int local_y = 0; local_y < height; ++local_y)
        {
            column.blocks[static_cast<std::size_t>(local_y)] =
                local_y == height - 1 && radius < 2 ? BlockType::fixed_lantern : BlockType::bedrock;
            column.mods[static_cast<std::size_t>(local_y)] = 0U;
        }
        column.top = std::max(column.top, height - 1);
    }

    // --- trees -----------------------------------------------------------------------------

    struct TreePlan final
    {
        const TerrainTree *tree{};
        int x{};
        int z{};
        int ground{}; // local Y of the block it stands on
        int height{};
        std::uint64_t variant{};
    };

    [[nodiscard]] static int reach_of(const TerrainTree &tree) noexcept
    {
        int reach{};
        switch (tree.shape)
        {
        case TreeShape::retail:
            reach = 3;
            break;
        case TreeShape::round:
        case TreeShape::cone:
        case TreeShape::boulder:
            reach = tree.radius;
            break;
        case TreeShape::dead:
            reach = 2;
            break;
        case TreeShape::column:
            reach = 0;
            break;
        }
        return std::min(reach + (tree.thick ? 1 : 0), maximum_tree_reach);
    }

    // Whether a tree grows at (x, z), and where. The same answer whichever
    // chunk asks: it reads only the column's plan and shape.
    [[nodiscard]] std::optional<TreePlan> tree_at(const WorldGenerator &retail, std::uint32_t seed,
                                                  int x, int z) const noexcept
    {
        if (!(maximum_tree_chance > 0.0))
            return std::nullopt;
        const double roll = unit_hash(seed, x, 1, z, tree_salt);
        if (roll >= maximum_tree_chance)
            return std::nullopt;
        if (definition.origin_tower && std::abs(x) <= 6 && std::abs(z) <= 6)
            return std::nullopt;
        const Plan column_plan = plan(seed, x, z);
        const TerrainBiome &biome = definition.biomes[column_plan.pick];
        if (biome.retail != RetailReuse::none || biome.trees.empty())
            return std::nullopt;
        const TerrainTree *chosen{};
        double cumulative{};
        for (const TerrainTree &tree : biome.trees)
        {
            cumulative += tree.chance;
            if (roll < cumulative)
            {
                chosen = &tree;
                break;
            }
        }
        if (chosen == nullptr)
            return std::nullopt;
        // The ground: the top of the shape, which must be the biome's top
        // surface layer (or a block the tree names) and survive the caves.
        const Point point{static_cast<double>(x), 0.0, static_cast<double>(z), seed};
        const Shape form = shape(retail, column_plan, point);
        int top = chunk_height - 1;
        while (top >= 0 && !solid(form, point, top + world_min_y))
            --top;
        if (top < 1 || top >= chunk_height - 4)
            return std::nullopt;
        const bool underwater = top + world_min_y <= definition.sea_level;
        const TerrainBlock ground = layer_block(&biome, biome.like, 0, underwater);
        if (chosen->on.empty() ? (underwater || biome.surface.empty() ||
                                  ground != biome.surface.front().block)
                               : std::find(chosen->on.begin(), chosen->on.end(), ground) ==
                                     chosen->on.end())
            return std::nullopt;
        if (biome.caves.breach_surface && carved(biome.caves, seed, x, top + world_min_y, z))
            return std::nullopt;
        TreePlan result;
        result.tree = chosen;
        result.x = x;
        result.z = z;
        result.ground = top;
        result.variant = mix64(static_cast<std::uint64_t>(roll * 9007199254740992.0) ^
                               static_cast<std::uint64_t>(seed));
        const int span = std::max(chosen->height_max - chosen->height_min, 0) + 1;
        result.height = chosen->height_min + static_cast<int>(result.variant % static_cast<std::uint64_t>(span));
        return result;
    }

    // Writes one tree's cells that fall inside `chunk` (trunk and leaves only
    // into empty cells).
    void plant(Chunk &chunk, const TreePlan &tree, std::uint32_t seed) const
    {
        const int base_x = chunk.chunk_x() * chunk_width;
        const int base_z = chunk.chunk_z() * chunk_depth;
        const auto put = [&](int x, int local_y, int z, TerrainBlock block, bool replace_leaves) {
            const int lx = x - base_x;
            const int lz = z - base_z;
            if (lx < 0 || lx >= chunk_width || lz < 0 || lz >= chunk_depth || local_y < 0 ||
                local_y >= chunk_height)
                return;
            const BlockType current = chunk.block(lx, local_y, lz);
            if (current != BlockType::empty &&
                !(replace_leaves && current == tree.tree->leaves.block))
                return;
            chunk.set_block(lx, local_y, lz, block.block);
            if (block.mod != 0U)
                chunk.set_mod_cell(lx, local_y, lz, block.mod);
        };
        const TerrainTree &spec = *tree.tree;
        const int bottom = tree.ground + 1;
        const int top = bottom + tree.height; // first cell above the trunk
        const int thickness = spec.thick ? 2 : 1;
        const auto trunk = [&](int height) {
            for (int dy = 0; dy < height; ++dy)
                for (int dz = 0; dz < thickness; ++dz)
                    for (int dx = 0; dx < thickness; ++dx)
                        put(tree.x + dx, bottom + dy, tree.z + dz, spec.trunk, true);
        };
        const double centre_x = tree.x + (spec.thick ? 1.0 : 0.5);
        const double centre_z = tree.z + (spec.thick ? 1.0 : 0.5);
        switch (spec.shape)
        {
        case TreeShape::retail:
        case TreeShape::round: {
            const int radius = spec.shape == TreeShape::retail ? 3 : std::max(spec.radius, 1);
            const int crown_y = spec.shape == TreeShape::retail ? top : top - 1;
            const std::uint32_t leaf_seed = mix32(seed ^ 0x1EAF5EEDU);
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dz = -radius; dz <= radius + thickness - 1; ++dz)
                    for (int dx = -radius; dx <= radius + thickness - 1; ++dx)
                    {
                        const double px = tree.x + dx + 0.5 - centre_x;
                        const double pz = tree.z + dz + 0.5 - centre_z;
                        const double distance =
                            std::sqrt(px * px + static_cast<double>(dy * dy) + pz * pz);
                        const double ragged =
                            gradient3(leaf_seed, (tree.x + dx) * 0.5, (crown_y + dy) * 0.5,
                                      (tree.z + dz) * 0.5);
                        if (1.0 - distance / radius + ragged * 0.45 > 0.25)
                            put(tree.x + dx, crown_y + dy, tree.z + dz, spec.leaves, false);
                    }
            trunk(tree.height);
            break;
        }
        case TreeShape::cone: {
            const int radius = std::max(spec.radius, 1);
            const int crown_bottom = bottom + std::max(2, tree.height / 3);
            const int crown_top = top + 1;
            for (int y = crown_bottom; y <= crown_top; ++y)
            {
                const double along = static_cast<double>(crown_top - y) /
                                     static_cast<double>(std::max(crown_top - crown_bottom, 1));
                // Rings shrink upwards, every other one tucked in.
                double ring = 0.6 + along * radius;
                if (((crown_top - y) & 1) == 1)
                    ring *= 0.7;
                for (int dz = -radius; dz <= radius + thickness - 1; ++dz)
                    for (int dx = -radius; dx <= radius + thickness - 1; ++dx)
                    {
                        const double px = tree.x + dx + 0.5 - centre_x;
                        const double pz = tree.z + dz + 0.5 - centre_z;
                        if (px * px + pz * pz <= ring * ring)
                            put(tree.x + dx, y, tree.z + dz, spec.leaves, false);
                    }
            }
            trunk(tree.height);
            break;
        }
        case TreeShape::column:
            trunk(tree.height);
            break;
        case TreeShape::dead: {
            trunk(tree.height);
            // A couple of stubs near the top.
            for (int branch = 0; branch < 2; ++branch)
            {
                const std::uint64_t bits = tree.variant >> (branch * 8U);
                const int y = bottom + tree.height - 1 - static_cast<int>(bits % 3U);
                const int direction = static_cast<int>((bits >> 2U) % 4U);
                const int dx = direction == 0 ? 1 : direction == 1 ? -1 : 0;
                const int dz = direction == 2 ? 1 : direction == 3 ? -1 : 0;
                put(tree.x + dx * thickness, y, tree.z + dz * thickness, spec.trunk, false);
                if (spec.radius > 1)
                    put(tree.x + dx * (thickness + 1), y + 1, tree.z + dz * (thickness + 1),
                        spec.trunk, false);
            }
            break;
        }
        case TreeShape::boulder: {
            const int radius = std::max(spec.radius, 1);
            const double squash = 0.75;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dz = -radius; dz <= radius; ++dz)
                    for (int dx = -radius; dx <= radius; ++dx)
                    {
                        const double distance =
                            std::sqrt(static_cast<double>(dx * dx + dz * dz) +
                                      (dy / squash) * (dy / squash));
                        const double ragged =
                            unit_hash(seed, tree.x + dx, tree.ground + dy, tree.z + dz, 0xB0D1ULL);
                        if (distance <= radius - 0.2 + ragged * 0.6)
                            put(tree.x + dx, tree.ground + dy + 1, tree.z + dz, spec.trunk, false);
                    }
            break;
        }
        }
    }

    // --- validation and the canonical form ------------------------------------------------------

    void fail(const std::string &reason) const
    {
        throw std::invalid_argument{"world generator: " + reason};
    }

    void check_noise(NoiseRef reference, std::size_t owner, std::string_view where) const
    {
        if (reference == no_noise)
            return;
        if (reference >= definition.nodes.size() || reference >= owner)
            fail(std::format("{} names a noise expression that does not exist", where));
    }

    void validate()
    {
        if (definition.nodes.size() > 65536U)
            fail("more than 65,536 noise nodes");
        for (std::size_t index = 0U; index < definition.nodes.size(); ++index)
        {
            NoiseNode &node = definition.nodes[index];
            for (const NoiseRef input : node.inputs)
                check_noise(input, index, "an expression");
            if (!(node.scale >= 0.001) || !(node.scale <= 1.0e7) || !std::isfinite(node.value) ||
                node.vertical_scale < 0.0 || !std::isfinite(node.vertical_scale) ||
                !std::isfinite(node.gain) || !std::isfinite(node.lacunarity) ||
                !std::isfinite(node.low) || !std::isfinite(node.high))
                fail(std::format("noise node {} has a value out of range", index));
            node.octaves = std::clamp(node.octaves, 1, 10);
            const std::size_t needed = node.op == NoiseOp::subtract ? 2U
                                       : node.op == NoiseOp::lerp || node.op == NoiseOp::select
                                           ? 3U
                                       : node.op == NoiseOp::absolute || node.op == NoiseOp::clamp ||
                                               node.op == NoiseOp::curve
                                           ? 1U
                                           : 0U;
            if (node.inputs.size() < needed)
                fail(std::format("noise node {} needs {} inputs", index, needed));
            if (node.op == NoiseOp::select && !(node.high > node.low))
                fail("noise.select needs high > low");
            for (std::size_t point = 1U; point < node.points.size(); ++point)
                if (!(node.points[point][0] > node.points[point - 1U][0]))
                    fail("curve points must be in increasing order of their input");
            for (const auto &pair : node.points)
                if (!std::isfinite(pair[0]) || !std::isfinite(pair[1]))
                    fail("curve points must be finite");
        }
        if (definition.biomes.empty())
            fail("no biomes");
        if (definition.biomes.size() > 4096U)
            fail("more than 4,096 biomes");
        if (definition.palette.size() > 255U || definition.palette.size() != definition.palette_fallback.size())
            fail("more than 255 mod blocks");
        const auto check_block = [&](const TerrainBlock &block, std::string_view where) {
            if (block.mod > definition.palette.size())
                fail(std::format("{} names a mod block that is not in the palette", where));
            if (block.mod != 0U && definition.palette_fallback[block.mod - 1U] != block.block)
                fail(std::format("{} does not hold its mod block's fallback", where));
        };
        const std::size_t nodes = definition.nodes.size();
        for (TerrainBiome &biome : definition.biomes)
        {
            const std::string where = "biome " + biome.key;
            if (biome.retail == RetailReuse::none && biome.height == no_noise)
                fail(where + " has no height");
            check_noise(biome.height, nodes, where + " height");
            check_noise(biome.density, nodes, where + " density");
            if (!std::isfinite(biome.density_strength) || biome.density_strength < 0.0 ||
                biome.density_strength > 64.0)
                fail(where + ": density strength must be 0..64");
            for (const TerrainLayer &layer : biome.surface)
                check_block(layer.block, where + " surface");
            for (const TerrainLayer &layer : biome.underwater)
                check_block(layer.block, where + " underwater");
            check_block(biome.filler, where + " filler");
            check_block(biome.fluid, where + " fluid");
            for (TerrainLayer &layer : biome.surface)
                layer.depth = std::clamp(layer.depth, 1, chunk_height);
            for (TerrainLayer &layer : biome.underwater)
                layer.depth = std::clamp(layer.depth, 1, chunk_height);
            biome.caves.scale = std::clamp(biome.caves.scale, 4.0, 1024.0);
            if (!std::isfinite(biome.caves.threshold) || !std::isfinite(biome.caves.width))
                fail(where + ": cave values must be finite");
            if (biome.ores.size() > 32U || biome.trees.size() > 32U)
                fail(where + ": at most 32 ores and 32 trees");
            for (TerrainOre &ore : biome.ores)
            {
                check_block(ore.block, where + " ore");
                for (const TerrainBlock &block : ore.in)
                    check_block(block, where + " ore host");
                ore.size = std::clamp(ore.size, 0, 6);
                ore.spacing = std::clamp(ore.spacing, 2 * ore.size + 2, 64);
                if (!(ore.chance >= 0.0 && ore.chance <= 1.0))
                    fail(where + ": ore chance must be 0..1");
            }
            for (TerrainTree &tree : biome.trees)
            {
                check_block(tree.trunk, where + " tree trunk");
                check_block(tree.leaves, where + " tree leaves");
                for (const TerrainBlock &block : tree.on)
                    check_block(block, where + " tree ground");
                tree.height_min = std::clamp(tree.height_min, 1, 48);
                tree.height_max = std::clamp(tree.height_max, tree.height_min, 48);
                tree.radius = std::clamp(tree.radius, 0, maximum_tree_reach - 1);
                if (!(tree.chance >= 0.0 && tree.chance <= 1.0))
                    fail(where + ": tree chance must be 0..1");
            }
        }
        if (definition.layouts.empty())
            fail("no layout");
        for (std::size_t index = 0U; index < definition.layouts.size(); ++index)
        {
            const TerrainLayout &layout = definition.layouts[index];
            if (layout.bands.empty())
                fail("a layout has no bands");
            check_noise(layout.select, nodes, "a layout's select");
            check_noise(layout.warp, nodes, "a layout's warp");
            if (layout.input == LayoutInput::noise && layout.select == no_noise)
                fail("a noise layout needs a select expression");
            for (std::size_t band = 0U; band < layout.bands.size(); ++band)
            {
                const LayoutBand &entry = layout.bands[band];
                if ((entry.biome < 0) == (entry.layout < 0))
                    fail("each band names exactly one biome or one layout");
                if (entry.biome >= static_cast<std::int32_t>(definition.biomes.size()))
                    fail("a band names a biome that does not exist");
                // Nested layouts come after their parent: no cycles.
                if (entry.layout >= 0 &&
                    (entry.layout >= static_cast<std::int32_t>(definition.layouts.size()) ||
                     entry.layout <= static_cast<std::int32_t>(index)))
                    fail("a band names a layout that does not exist");
                if (!std::isfinite(entry.to) || !std::isfinite(entry.blend) || entry.blend < 0.0)
                    fail("band bounds and blends must be finite");
                if (band > 0U && band + 1U < layout.bands.size() &&
                    !(entry.to > layout.bands[band - 1U].to))
                    fail("band bounds must increase");
            }
        }
        definition.sea_level = std::clamp(definition.sea_level, world_min_y, world_min_y + chunk_height - 1);
        definition.bedrock_depth = std::clamp(definition.bedrock_depth, 0, 8);
        canonical = canonical_terrain_bytes(definition);
    }

    TerrainCaves default_caves{};
};

// ---------------------------------------------------------------------------
// CustomTerrain
// ---------------------------------------------------------------------------

CustomTerrain::CustomTerrain(TerrainDefinition definition)
    : impl_(std::make_unique<Impl>(std::move(definition)))
{
}

CustomTerrain::~CustomTerrain() = default;

const TerrainDefinition &CustomTerrain::definition() const noexcept
{
    return impl_->definition;
}

const std::string &CustomTerrain::digest() const noexcept
{
    return impl_->digest;
}

const std::vector<FarSurfaceProfile> &CustomTerrain::far_profiles() const noexcept
{
    return impl_->far_profiles;
}

namespace
{

[[nodiscard]] std::uint32_t terrain_seed(const WorldGenerator &retail) noexcept
{
    return mix32(static_cast<std::uint32_t>(retail.seed()) ^ 0x7E44A1A5U);
}

} // namespace

Chunk CustomTerrain::generate_chunk(const WorldGenerator &retail, int chunk_x, int chunk_z) const
{
    const Impl &impl = *impl_;
    const std::uint32_t seed = terrain_seed(retail);
    Chunk chunk{chunk_x, chunk_z};
    std::array<bool, chunk_width * chunk_depth> retail_columns{};
    bool any_retail{};
    Impl::Column column;
    // (wave 5) Every column's plan and shape first, with a ring of one more
    // around the chunk when a biome has a fluid: a cave asks its neighbours
    // whether their fluid comes down beside it.
    const int ring = impl.has_fluid ? 1 : 0;
    const int span_x = chunk_width + 2 * ring;
    const int span_z = chunk_depth + 2 * ring;
    struct Planned final
    {
        Impl::Plan plan;
        Impl::Shape form;
        int floor{Impl::no_fluid};
    };
    std::vector<Planned> planned(static_cast<std::size_t>(span_x * span_z));
    for (int grid_z = 0; grid_z < span_z; ++grid_z)
        for (int grid_x = 0; grid_x < span_x; ++grid_x)
        {
            const int x = chunk_x * chunk_width + grid_x - ring;
            const int z = chunk_z * chunk_depth + grid_z - ring;
            Planned &entry = planned[static_cast<std::size_t>(grid_x + grid_z * span_x)];
            entry.plan = impl.plan(seed, x, z);
            if (entry.plan.pure_retail)
                continue;
            const Impl::Point point{static_cast<double>(x), 0.0, static_cast<double>(z), seed};
            entry.form = impl.shape(retail, entry.plan, point);
            if (ring != 0)
                entry.floor = impl.fluid_floor(entry.plan, entry.form, point);
        }
    for (int local_z = 0; local_z < chunk_depth; ++local_z)
        for (int local_x = 0; local_x < chunk_width; ++local_x)
        {
            const int x = chunk_x * chunk_width + local_x;
            const int z = chunk_z * chunk_depth + local_z;
            const auto here = static_cast<std::ptrdiff_t>(local_x + ring + (local_z + ring) * span_x);
            const Impl::Plan &plan = planned[static_cast<std::size_t>(here)].plan;
            if (plan.pure_retail)
            {
                const TerrainBiome &biome = impl.definition.biomes[plan.pick];
                if (biome.retail == RetailReuse::world)
                    retail.retail_build_column(chunk, local_x, local_z);
                else
                    retail.retail_build_biome_column(chunk, reuse_kind(biome.retail), local_x,
                                                     local_z);
                retail_columns[static_cast<std::size_t>(local_x + local_z * chunk_width)] = true;
                any_retail = true;
                continue;
            }
            column = {};
            const auto floors = [&](int direction) noexcept {
                if (ring == 0)
                    return Impl::no_fluid;
                const std::ptrdiff_t step = direction == 0   ? 1
                                            : direction == 1 ? -1
                                            : direction == 2 ? span_x
                                                             : -span_x;
                return planned[static_cast<std::size_t>(here + step)].floor;
            };
            impl.build(retail, plan, planned[static_cast<std::size_t>(here)].form, seed, x, z, column,
                       false, true, floors);
            for (int local_y = 0; local_y < chunk_height; ++local_y)
            {
                const std::size_t index = static_cast<std::size_t>(local_y);
                if (column.blocks[index] == BlockType::empty)
                    continue;
                chunk.set_block(local_x, local_y, local_z, column.blocks[index]);
                if (column.mods[index] != 0U)
                    chunk.set_mod_cell(local_x, local_y, local_z, column.mods[index]);
            }
        }
    // Trees, in one order every chunk agrees on: those of the margin too, so
    // a crown that reaches over the border is the same crown on both sides.
    const int reach = impl.tree_reach;
    if (impl.maximum_tree_chance > 0.0)
        for (int z = chunk_z * chunk_depth - reach; z < (chunk_z + 1) * chunk_depth + reach; ++z)
            for (int x = chunk_x * chunk_width - reach; x < (chunk_x + 1) * chunk_width + reach; ++x)
                if (const auto tree = impl.tree_at(retail, seed, x, z))
                    impl.plant(chunk, *tree, seed);
    if (any_retail)
        retail.retail_trees(
            chunk,
            [](const void *context, int local_x, int local_z) {
                return (*static_cast<const std::array<bool, chunk_width * chunk_depth> *>(
                    context))[static_cast<std::size_t>(local_x + local_z * chunk_width)];
            },
            &retail_columns);
    return chunk;
}

void CustomTerrain::generate_column(const WorldGenerator &retail, int world_x, int world_z,
                                    std::span<BlockType, chunk_height> cells,
                                    std::span<std::uint8_t, chunk_height> *mods) const noexcept
{
    const Impl &impl = *impl_;
    const std::uint32_t seed = terrain_seed(retail);
    const Impl::Plan plan = impl.plan(seed, world_x, world_z);
    if (plan.pure_retail)
    {
        const TerrainBiome &biome = impl.definition.biomes[plan.pick];
        if (biome.retail == RetailReuse::world)
            retail.retail_column(world_x, world_z, cells);
        else
            retail.retail_biome_column(reuse_kind(biome.retail), world_x, world_z, cells);
        if (mods != nullptr)
            std::fill(mods->begin(), mods->end(), std::uint8_t{});
        return;
    }
    Impl::Column column;
    const Impl::Point point{static_cast<double>(world_x), 0.0, static_cast<double>(world_z), seed};
    impl.build(retail, plan, impl.shape(retail, plan, point), seed, world_x, world_z, column, false,
               true, Impl::LazyFloors{impl, retail, seed, world_x, world_z, {}, {}});
    std::copy(column.blocks.begin(), column.blocks.end(), cells.begin());
    if (mods != nullptr)
        std::copy(column.mods.begin(), column.mods.end(), mods->begin());
}

SurfaceColumn CustomTerrain::surface_column(const WorldGenerator &retail, int world_x,
                                            int world_z, bool origin_tower) const noexcept
{
    const Impl &impl = *impl_;
    const std::uint32_t seed = terrain_seed(retail);
    const Impl::Plan plan = impl.plan(seed, world_x, world_z);
    if (plan.pure_retail)
    {
        const TerrainBiome &biome = impl.definition.biomes[plan.pick];
        if (biome.retail == RetailReuse::world)
            return retail.retail_surface_column(world_x, world_z, origin_tower);
        std::array<BlockType, chunk_height> cells{};
        retail.retail_biome_column(reuse_kind(biome.retail), world_x, world_z, cells);
        SurfaceColumn surface;
        for (int y = chunk_height - 1; y >= 0; --y)
            if (cells[static_cast<std::size_t>(y)] != BlockType::empty &&
                cells[static_cast<std::size_t>(y)] != BlockType::uninitialized)
            {
                surface.top_local_y = static_cast<std::int16_t>(y);
                surface.top = cells[static_cast<std::size_t>(y)];
                break;
            }
        return surface;
    }
    Impl::Column column;
    // The far landscape draws the origin tower as its own object.
    const Impl::Point point{static_cast<double>(world_x), 0.0, static_cast<double>(world_z), seed};
    impl.build(retail, plan, impl.shape(retail, plan, point), seed, world_x, world_z, column, true,
               origin_tower, Impl::LazyFloors{impl, retail, seed, world_x, world_z, {}, {}});
    SurfaceColumn surface;
    if (column.top < 0)
        return surface;
    surface.top_local_y = static_cast<std::int16_t>(column.top);
    surface.top = column.blocks[static_cast<std::size_t>(column.top)];
    surface.top_mod = column.mods[static_cast<std::size_t>(column.top)];
    surface.top_profile = impl.profile_at(plan, column, column.top); // (fix)
    if (column.under > 0 && column.lower >= 0 && column.lower < column.under)
    {
        surface.under_local_y = static_cast<std::int16_t>(column.under);
        surface.lower_local_y = static_cast<std::int16_t>(column.lower);
        surface.lower = column.blocks[static_cast<std::size_t>(column.lower)];
        surface.lower_mod = column.mods[static_cast<std::size_t>(column.lower)];
        surface.lower_profile = impl.profile_at(plan, column, column.lower); // (fix)
    }
    return surface;
}

void CustomTerrain::tree_sites(const WorldGenerator &retail, int chunk_x, int chunk_z,
                               std::vector<TreeSite> &sites, bool top_grass_only) const
{
    const Impl &impl = *impl_;
    const std::uint32_t seed = terrain_seed(retail);
    for (int local_z = 0; local_z < chunk_depth; ++local_z)
        for (int local_x = 0; local_x < chunk_width; ++local_x)
        {
            const int x = chunk_x * chunk_width + local_x;
            const int z = chunk_z * chunk_depth + local_z;
            // Only trees with a crown: the far landscape draws each site as a
            // trunk and a ball of leaves (pillars, dead trees and boulders
            // are left to the detailed terrain).
            const auto tree = impl.tree_at(retail, seed, x, z);
            if (!tree.has_value() || tree->tree->shape == TreeShape::boulder ||
                tree->tree->shape == TreeShape::column || tree->tree->shape == TreeShape::dead)
                continue;
            // (wave 5) With its own crown and blocks, so a spruce is not drawn
            // as an oak on a pole far away.
            const TerrainTree &spec = *tree->tree;
            TreeSite site{x, z, static_cast<std::int16_t>(tree->ground),
                          static_cast<std::int16_t>(tree->height)};
            site.crown = spec.shape == TreeShape::cone    ? TreeCrown::cone
                         : spec.shape == TreeShape::round ? TreeCrown::round
                                                          : TreeCrown::retail;
            site.crown_radius = static_cast<std::uint8_t>(std::max(spec.radius, 1));
            site.thick = spec.thick;
            site.trunk = spec.trunk.block;
            site.trunk_mod = spec.trunk.mod;
            site.leaves = spec.leaves.block;
            site.leaves_mod = spec.leaves.mod;
            sites.push_back(site);
        }
    // Retail trees where the layout reuses retail terrain.
    if (!impl.reuses_retail)
        return;
    std::vector<TreeSite> retail_sites;
    retail.retail_tree_sites(chunk_x, chunk_z, retail_sites, top_grass_only);
    for (const TreeSite &site : retail_sites)
    {
        const Impl::Plan plan = impl.plan(seed, site.x, site.z);
        if (plan.pure_retail)
            sites.push_back(site);
    }
}

std::size_t CustomTerrain::biome_weights(
    const WorldGenerator &retail, int world_x, int world_z,
    std::span<BiomeWeight, maximum_biome_weights> weights) const noexcept
{
    const Impl::Plan plan = impl_->plan(terrain_seed(retail), world_x, world_z);
    std::copy_n(plan.weights.entries.begin(), plan.weights.count, weights.begin());
    return plan.weights.count;
}

std::uint16_t CustomTerrain::biome_at(const WorldGenerator &retail, int world_x,
                                      int world_z) const noexcept
{
    const Impl::Plan plan = impl_->plan(terrain_seed(retail), world_x, world_z);
    return plan.weights.entries[0].biome;
}

BiomeKind CustomTerrain::retail_kind_at(const WorldGenerator &retail, int world_x,
                                        int world_z) const noexcept
{
    const Impl &impl = *impl_;
    const Impl::Plan plan = impl.plan(terrain_seed(retail), world_x, world_z);
    const TerrainBiome &biome = impl.definition.biomes[plan.weights.entries[0].biome];
    return impl.pick_kind(retail, biome, world_x, world_z);
}

int CustomTerrain::surface_height(const WorldGenerator &retail, int world_x,
                                  int world_z) const noexcept
{
    const Impl &impl = *impl_;
    const std::uint32_t seed = terrain_seed(retail);
    const Impl::Plan plan = impl.plan(seed, world_x, world_z);
    const Impl::Point point{static_cast<double>(world_x), 0.0, static_cast<double>(world_z), seed};
    const Impl::Shape form = impl.shape(retail, plan, point);
    int top = chunk_height - 1;
    while (top >= 0 && !impl.solid(form, point, top + world_min_y))
        --top;
    return top + world_min_y;
}

double CustomTerrain::sample(NoiseRef expression, std::int32_t seed, double x, double y,
                             double z) const noexcept
{
    return impl_->evaluate(expression,
                           {x, y, z, mix32(static_cast<std::uint32_t>(seed) ^ 0x7E44A1A5U)});
}

// ---------------------------------------------------------------------------
// Drawing generated mod blocks
// ---------------------------------------------------------------------------

std::size_t apply_mod_palette(const WorldRegion &region, ChunkFaceBatch &batch,
                              const ModPaletteLooks &looks)
{
    if (looks.empty())
        return 0U;
    const int region_x = batch.coordinate.x - region.origin_chunk_x();
    const int region_z = batch.coordinate.z - region.origin_chunk_z();
    if (region_x < 0 || region_x >= region.chunks_x() || region_z < 0 ||
        region_z >= region.chunks_z())
        return 0U;
    const Chunk &chunk =
        region.chunks()[static_cast<std::size_t>(region_x + region_z * region.chunks_x())];
    if (!chunk.has_mod_cells())
        return 0U;
    const int base_x = chunk.chunk_x() * chunk_width;
    const int base_z = chunk.chunk_z() * chunk_depth;
    std::size_t changed{};
    for (WorldFace &face : batch.faces)
    {
        const std::uint8_t palette =
            chunk.mod_cell(face.x - base_x, face.y - world_min_y, face.z - base_z);
        if (palette == 0U || palette > looks.size() || !looks[palette - 1U].valid)
            continue;
        const std::size_t slot = face.direction == FaceDirection::positive_y   ? 0U
                                 : face.direction == FaceDirection::negative_y ? 2U
                                                                               : 1U;
        face.tile = looks[palette - 1U].tiles[slot];
        ++changed;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// The canonical form
// ---------------------------------------------------------------------------

std::string canonical_terrain_bytes(const TerrainDefinition &definition)
{
    std::string out = "omz-terrain 1\n";
    const auto block = [](const TerrainBlock &value) {
        return std::format("{}:{}", static_cast<int>(value.block), static_cast<int>(value.mod));
    };
    out += std::format("sea {} spawn {} {} tower {} bedrock {}\n", definition.sea_level,
                       definition.spawn_x, definition.spawn_z, definition.origin_tower ? 1 : 0,
                       definition.bedrock_depth);
    for (std::size_t index = 0U; index < definition.palette.size(); ++index)
        out += std::format("palette {} {} {}\n", index + 1U, definition.palette[index],
                           static_cast<int>(definition.palette_fallback[index]));
    for (const NoiseNode &node : definition.nodes)
    {
        out += std::format("node {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}",
                           static_cast<int>(node.op), double_bits(node.value),
                           double_bits(node.scale), double_bits(node.vertical_scale), node.octaves,
                           double_bits(node.gain), double_bits(node.lacunarity), node.seed,
                           node.three_d ? 1 : 0, static_cast<int>(node.cellular),
                           double_bits(node.jitter), double_bits(node.low),
                           double_bits(node.high), node.smooth ? 1 : 0, node.inputs.size());
        for (const NoiseRef input : node.inputs)
            out += std::format(" {}", input);
        for (const auto &point : node.points)
            out += std::format(" {},{}", double_bits(point[0]), double_bits(point[1]));
        out += '\n';
    }
    for (const TerrainBiome &biome : definition.biomes)
    {
        out += std::format("biome {} retail {} height {} density {} {} filler {} fluid {}\n",
                           biome.key, static_cast<int>(biome.retail), biome.height, biome.density,
                           double_bits(biome.density_strength), block(biome.filler),
                           block(biome.fluid));
        for (const TerrainLayer &layer : biome.surface)
            out += std::format(" surface {} {}\n", block(layer.block), layer.depth);
        for (const TerrainLayer &layer : biome.underwater)
            out += std::format(" underwater {} {}\n", block(layer.block), layer.depth);
        const TerrainCaves &caves = biome.caves;
        out += std::format(" caves {} {} {} {} {} {} {} {} {}\n", caves.enabled ? 1 : 0,
                           caves.tunnels ? 1 : 0, double_bits(caves.scale),
                           double_bits(caves.threshold), double_bits(caves.width), caves.y_min,
                           caves.y_max, caves.breach_surface ? 1 : 0, caves.seed);
        for (const TerrainOre &ore : biome.ores)
        {
            out += std::format(" ore {} {} {} {} {} {} {}", block(ore.block),
                               double_bits(ore.chance), ore.size, ore.spacing, ore.y_min,
                               ore.y_max, ore.seed);
            for (const TerrainBlock &in : ore.in)
                out += " " + block(in);
            out += '\n';
        }
        for (const TerrainTree &tree : biome.trees)
        {
            out += std::format(" tree {} {} {} {} {} {} {} {} {}", static_cast<int>(tree.shape),
                               double_bits(tree.chance), tree.height_min, tree.height_max,
                               tree.radius, tree.thick ? 1 : 0, block(tree.trunk),
                               block(tree.leaves), tree.seed);
            for (const TerrainBlock &on : tree.on)
                out += " " + block(on);
            out += '\n';
        }
    }
    for (const TerrainLayout &layout : definition.layouts)
    {
        out += std::format("layout {} {} {} {} {}\n", static_cast<int>(layout.input), layout.select,
                           layout.warp, static_cast<int>(layout.beyond),
                           double_bits(layout.period));
        for (const LayoutBand &band : layout.bands)
            out += std::format(" band {} {} {} {}\n", double_bits(band.to), double_bits(band.blend),
                               band.biome, band.layout);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Biome ids
// ---------------------------------------------------------------------------

namespace
{

constexpr std::array<std::string_view, 6> retail_biome_names{"classic", "lagoon", "desert",
                                                             "mountain", "arctic", "descent"};

struct BiomeNames final
{
    std::mutex mutex;
    std::vector<std::string> names;
    std::map<std::string, int, std::less<>> ids;
};

BiomeNames &biome_names() noexcept
{
    static BiomeNames names;
    return names;
}

} // namespace

std::string_view retail_biome_name(BiomeKind kind) noexcept
{
    const auto index = static_cast<std::size_t>(kind);
    return index < retail_biome_names.size() ? retail_biome_names[index] : "classic";
}

std::optional<BiomeKind> retail_biome_from_name(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < retail_biome_names.size(); ++index)
        if (retail_biome_names[index] == name)
            return static_cast<BiomeKind>(index);
    return std::nullopt;
}

int biome_id(std::string_view name)
{
    if (const auto retail = retail_biome_from_name(name))
        return static_cast<int>(*retail);
    BiomeNames &table = biome_names();
    const std::lock_guard lock{table.mutex};
    if (const auto found = table.ids.find(name); found != table.ids.end())
        return found->second;
    const int id = first_custom_biome_id + static_cast<int>(table.names.size());
    table.names.emplace_back(name);
    table.ids.emplace(std::string{name}, id);
    return id;
}

std::string biome_name(int id)
{
    if (id >= 0 && id < static_cast<int>(retail_biome_names.size()))
        return std::string{retail_biome_names[static_cast<std::size_t>(id)]};
    BiomeNames &table = biome_names();
    const std::lock_guard lock{table.mutex};
    const int index = id - first_custom_biome_id;
    if (index >= 0 && index < static_cast<int>(table.names.size()))
        return table.names[static_cast<std::size_t>(index)];
    return "classic";
}

} // namespace openminerz::world
