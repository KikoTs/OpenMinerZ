#pragma once

// The Enhanced presentation's one atmosphere: what stands between the eye and
// any point of the world above ground. The tested copy of the arithmetic that
// src/atmosphere_shader.hpp carries in HLSL.
//
// PRESENTATION ONLY. A density field over the world, integrated in closed form
// along a view ray:
//
//   density(p) = air(p.y) + first_biome(p.xz) * (mist(p.y) + haze(p.y))
//
// Every layer is full strength at and below its floor and thins exponentially
// above it, so mist lies low and a summit stands in clearer air.
//
//   * AIR is everywhere. Its density is set by how far the view reaches: it
//     leaves a tenth of the light at the distance where the far landscape
//     ends, so a ridge a kilometre off is visibly paler than one at 300
//     blocks and the last of the land melts into the horizon.
//   * With far terrain off the view ends at the edge of the detailed window
//     instead. The air near the eye stays what it is at the default far
//     distance, and one more term of the same function closes the view: an
//     optical depth that grows with the fifth power of the horizontal
//     distance (edge_path), 3% of it at half the visible range and all of it
//     - a twentieth of the light left - at its end.
//   * MIST and a taller HAZE belong to the first biome only, the plains round
//     the world's origin (and wherever the generator's rings repeat them).
//     `first_biome` falls smoothly from 1 to 0 between 170 and 320 blocks
//     out, so from the plains the hills soften into mist while peaks further
//     out stand above it, and from the mountains or the arctic the air is
//     clear with the plains as a bank of mist below.
//
// The field is never negative, so transmittance can only fall along a ray:
// nothing is ever clearer than what stands in front of it. And because it is a
// field over world positions, a point is fogged the same whichever renderer
// path draws it; the paths differ only in how they find the point's distance
// (near_view_depth / far_view_depth below).

#include <algorithm>
#include <array>
#include <cmath>

namespace openminerz::world
{

struct AtmosphereLayer final
{
    // Extinction per block at and below `floor`, falling by e every `height`
    // blocks above it.
    float density{};
    float floor{};
    float height{1.0F};
};

struct MistDisc final
{
    float radius{};
    // Share of the first biome's layers inside this disc; negative for the
    // hole of a ring.
    float weight{};
};

struct SurfaceAtmosphere final
{
    AtmosphereLayer air{};
    AtmosphereLayer mist{0.0060F, 12.0F, 10.0F};
    AtmosphereLayer haze{0.0012F, 20.0F, 36.0F};
    std::array<MistDisc, 4> discs{};
    // The end of sight when there is no far landscape: the optical depth the
    // closing air has reached `edge_reach` blocks out (zero with far terrain
    // drawn), growing with the fifth power of the horizontal distance and
    // gathered only within `edge_ceiling` blocks above the eye.
    float edge_depth{};
    float edge_reach{128.0F};
    float edge_ceiling{28.0F};
};

// How far above an eye at world height `eye_y` the closing air reaches: to
// about the height of the tallest land, so every hilltop at the end of sight
// is inside it from wherever it is seen, and the sky above that is clear. An
// eye that stands higher still looks down into it, with a little of it left
// above the horizon so its top is not a line.
[[nodiscard]] constexpr float closing_air_ceiling(float eye_y) noexcept
{
    return std::max(64.0F - eye_y, 12.0F);
}

// How much light is left at the end of the view: where the far landscape
// dissolves, and at the edge of the detailed window when there is none.
inline constexpr float air_left_at_far_edge = 0.10F;
inline constexpr float air_left_at_window_edge = 0.05F;

// The clear air for a view that ends `window_reach` blocks away (the detailed
// window) or, as far as `far_presence` (0 .. 1) says the far landscape is
// drawn, `far_reach` blocks away.
[[nodiscard]] inline AtmosphereLayer clear_air(float window_reach, float far_reach,
                                               float far_presence) noexcept
{
    // Without a far landscape the air near the eye is the air of the default
    // far distance, so what is close by looks the same either way; the end of
    // sight is then closed by SurfaceAtmosphere::edge_depth, not by this.
    (void)window_reach;
    const float standard = -std::log(air_left_at_far_edge) / 4000.0F;
    const float open = -std::log(air_left_at_far_edge) / std::max(far_reach, 500.0F);
    AtmosphereLayer layer;
    layer.density = far_reach > 0.0F
                        ? std::lerp(standard, open, std::clamp(far_presence, 0.0F, 1.0F))
                        : standard;
    layer.floor = 40.0F;
    layer.height = 90.0F;
    return layer;
}

// How much of the first biome's mist stands over a point `radius` blocks from
// the world's origin. The generator folds the distance every 4400 blocks and
// mirrors odd rings, so the plains come round again at 8800.
[[nodiscard]] inline float biome_mist_weight(float radius) noexcept
{
    float folded = std::fmod(std::abs(radius), 8800.0F);
    if (folded > 4400.0F)
        folded = 8800.0F - folded;
    return std::clamp((320.0F - folded) / 150.0F, 0.0F, 1.0F);
}

// The discs that reproduce biome_mist_weight around an eye `eye_radius` blocks
// from the origin: the plains themselves, or the nearest repeat of them.
[[nodiscard]] inline std::array<MistDisc, 4> mist_discs(float eye_radius) noexcept
{
    std::array<MistDisc, 4> discs{};
    const float ring = std::round(std::abs(eye_radius) / 8800.0F) * 8800.0F;
    if (ring < 1.0F)
    {
        for (std::size_t index = 0U; index < discs.size(); ++index)
            discs[index] = {170.0F + (static_cast<float>(index) + 0.5F) * 37.5F, 0.25F};
        return discs;
    }
    for (std::size_t index = 0U; index < 2U; ++index)
    {
        const float offset = 170.0F + (static_cast<float>(index) + 0.5F) * 75.0F;
        discs[index] = {ring + offset, 0.5F};
        discs[index + 2U] = {ring - offset, -0.5F};
    }
    return discs;
}

// The atmosphere round an eye `eye_radius` blocks from the world's origin.
[[nodiscard]] inline SurfaceAtmosphere surface_atmosphere(float eye_radius, float window_reach,
                                                          float far_reach,
                                                          float far_presence) noexcept
{
    SurfaceAtmosphere atmosphere;
    atmosphere.air = clear_air(window_reach, far_reach, far_presence);
    atmosphere.discs = mist_discs(eye_radius);
    // As the far landscape arrives the closing air lifts with it.
    atmosphere.edge_depth = -std::log(air_left_at_window_edge) *
                            (far_reach > 0.0F ? 1.0F - std::clamp(far_presence, 0.0F, 1.0F) : 1.0F);
    atmosphere.edge_reach = std::max(window_reach, 32.0F);
    return atmosphere;
}

// What the closing air of SurfaceAtmosphere::edge_depth has gathered along
// `length` blocks in unit direction `direction`: nothing to speak of over the
// first half of the view, all of it by its end. A path only ever adds to it,
// and one that climbs out of its reach stops adding, which leaves the sky
// overhead a sky.
[[nodiscard]] inline float edge_path(const SurfaceAtmosphere &air,
                                     const std::array<float, 3> &direction, float length) noexcept
{
    if (air.edge_depth <= 0.0F || length <= 0.0F)
        return 0.0F;
    const float level = std::hypot(direction[0], direction[2]);
    float reached = length * level;
    if (direction[1] > 0.0F)
        reached = std::min(reached, air.edge_ceiling * level / direction[1]);
    const float share = reached / air.edge_reach;
    return air.edge_depth * share * share * share * share * share;
}

// How much of a path counts as open air: all of it unless both ends are
// underground (`eye_sky` and `surface_sky` are baked skylight, 0 .. 1). Every
// kind of surface under the sky - ground, leaves, a creature - gets the same.
[[nodiscard]] inline float atmosphere_exposure(float eye_sky, float surface_sky) noexcept
{
    const float t = std::clamp(surface_sky / 0.25F, 0.0F, 1.0F);
    return std::max(eye_sky, t * t * (3.0F - 2.0F * t));
}

// Integral over a straight path of exp(-max(y - floor, 0) / height): the
// length of the path as a layer weighs it. `y0` is the height where the path
// starts and `dy` how much it climbs per block travelled.
[[nodiscard]] inline float layer_path(float y0, float dy, float length, float floor,
                                      float height) noexcept
{
    if (length <= 0.0F)
        return 0.0F;
    const float a = (y0 - floor) / height;
    const float b = dy / height;
    if (std::abs(b) < 1.0e-5F)
        return length * std::exp(-std::max(a, 0.0F));
    // Where the path crosses the floor, clamped to the path.
    const float crossing = std::clamp(-a / b, 0.0F, length);
    const float above_start = b > 0.0F ? crossing : 0.0F;
    const float above_end = b > 0.0F ? length : crossing;
    float result = length - (above_end - above_start);
    if (above_end > above_start)
        result += (std::exp(-std::max(a + b * above_start, 0.0F)) -
                   std::exp(-std::max(a + b * above_end, 0.0F))) /
                  b;
    return result;
}

// The part of a path that lies inside a vertical cylinder of `radius` round
// the origin. (ex, ez) is the start relative to the origin and (dx, dz) the
// horizontal part of the unit direction. Returns false when there is none.
[[nodiscard]] inline bool disc_span(float ex, float ez, float dx, float dz, float radius,
                                    float length, float &start, float &end) noexcept
{
    const float a = dx * dx + dz * dz;
    const float c = ex * ex + ez * ez - radius * radius;
    if (a < 1.0e-8F)
    {
        if (c >= 0.0F)
            return false;
        start = 0.0F;
        end = length;
        return length > 0.0F;
    }
    const float b = ex * dx + ez * dz;
    const float discriminant = b * b - a * c;
    if (discriminant <= 0.0F)
        return false;
    const float root = std::sqrt(discriminant);
    start = std::max((-b - root) / a, 0.0F);
    end = std::min((-b + root) / a, length);
    return end > start;
}

// The field at one point: x the clear air's density there, y the mist's.
[[nodiscard]] inline std::array<float, 2> atmosphere_density(const SurfaceAtmosphere &air,
                                                             float x, float y, float z) noexcept
{
    const auto layer = [y](const AtmosphereLayer &part) {
        return part.density * std::exp(-std::max(y - part.floor, 0.0F) / part.height);
    };
    float weight{};
    for (const MistDisc &disc : air.discs)
        if (x * x + z * z < disc.radius * disc.radius)
            weight += disc.weight;
    return {layer(air.air), std::max(weight, 0.0F) * (layer(air.mist) + layer(air.haze))};
}

// Optical depth straight up from a point: what sunlight has to cross on its
// way down to it, per unit of 1 / sin(the sun's elevation). In thin air it is
// next to nothing; in thick fog it is why the sun is a pale disc and the fog
// round the eye does not blaze toward it.
[[nodiscard]] inline float overhead_depth(const SurfaceAtmosphere &air, float x, float y,
                                          float z) noexcept
{
    const auto above = [y](const AtmosphereLayer &part) {
        return part.density *
               (std::max(part.floor - y, 0.0F) +
                part.height * std::exp(-std::max(y - part.floor, 0.0F) / part.height));
    };
    float weight{};
    for (const MistDisc &disc : air.discs)
        if (x * x + z * z < disc.radius * disc.radius)
            weight += disc.weight;
    return above(air.air) + std::max(weight, 0.0F) * (above(air.mist) + above(air.haze));
}

struct AirPath final
{
    // Optical depths: the light surviving the path is exp(-(air + mist)).
    // (`air` includes the closing air at the end of sight, edge_path.)
    float air{};
    float mist{};

    [[nodiscard]] float transmittance() const noexcept
    {
        return std::exp(-(air + mist));
    }
};

// The air along `length` blocks from `eye` in unit direction `direction`.
[[nodiscard]] inline AirPath air_path(const SurfaceAtmosphere &air,
                                      const std::array<float, 3> &eye,
                                      const std::array<float, 3> &direction,
                                      float length) noexcept
{
    AirPath path;
    if (length <= 0.0F)
        return path;
    path.air = air.air.density *
                   layer_path(eye[1], direction[1], length, air.air.floor, air.air.height) +
               edge_path(air, direction, length);
    for (const MistDisc &disc : air.discs)
    {
        float start{};
        float end{};
        if (disc.weight == 0.0F ||
            !disc_span(eye[0], eye[2], direction[0], direction[2], disc.radius, length, start, end))
            continue;
        const float height = eye[1] + direction[1] * start;
        path.mist += disc.weight *
                     (air.mist.density * layer_path(height, direction[1], end - start,
                                                    air.mist.floor, air.mist.height) +
                      air.haze.density * layer_path(height, direction[1], end - start,
                                                    air.haze.floor, air.haze.height));
    }
    path.mist = std::max(path.mist, 0.0F);
    return path;
}

// --- How each renderer path finds a pixel's distance. ---
//
// Both store depth for a view ray whose component along the camera's forward
// axis is one; the distance along the ray is that camera depth times the
// ray's length. The detailed pass uses the renderer's ordinary projection
// (Projection.z = far / (far - near), Projection.w = -near * that); the far
// pass an infinite, reversed one (depth = near / camera depth).
[[nodiscard]] constexpr float near_depth_value(float camera_depth, float near_plane,
                                               float far_plane) noexcept
{
    const float scale = far_plane / (far_plane - near_plane);
    return scale - near_plane * scale / camera_depth;
}
[[nodiscard]] constexpr float near_view_depth(float depth, float near_plane,
                                              float far_plane) noexcept
{
    const float scale = far_plane / (far_plane - near_plane);
    return -near_plane * scale / std::min(depth - scale, -0.0000001F);
}
[[nodiscard]] constexpr float far_depth_value(float camera_depth, float near_plane) noexcept
{
    return near_plane / camera_depth;
}
[[nodiscard]] constexpr float far_view_depth(float depth, float near_plane) noexcept
{
    return near_plane / depth;
}

} // namespace openminerz::world
