#pragma once

#include "openminerz/world/sky.hpp"

#include <algorithm>
#include <cmath>

namespace openminerz::world
{
// Hell has an authored red fill, independent of skylight and local emitters.
// Keep some green/blue energy so rock texture and usable paths stay legible.
inline constexpr SkyColor hell_ambient{1.10F, 0.20F, 0.075F};

[[nodiscard]] inline float hell_presence(float height) noexcept
{
    const float t = std::clamp((-24.0F - height) / 12.0F, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

inline void apply_hell_atmosphere(SkyState &sky, float height) noexcept
{
    const float amount = hell_presence(height);
    sky.hell = amount;
    const auto blend = [amount](SkyColor a, SkyColor b) {
        return SkyColor{std::lerp(a.red, b.red, amount), std::lerp(a.green, b.green, amount),
                        std::lerp(a.blue, b.blue, amount)};
    };
    sky.fog = blend(sky.fog, {0.32F, 0.052F, 0.018F});
    sky.horizon = blend(sky.horizon, {0.36F, 0.063F, 0.022F});
    sky.zenith = blend(sky.zenith, {0.055F, 0.006F, 0.004F});
    sky.ambient = blend(sky.ambient, hell_ambient);
    sky.direct = blend(sky.direct, {});
    sky.specular = blend(sky.specular, {});
    sky.daylight *= 1.0F - amount;
    if (amount > 0.0F)
    {
        sky.lightning = false;
        sky.lightning_strength = 0.0F;
    }
}

// ---------------------------------------------------------------------------
// Rebirth's deep underground: how lava lights the cavern, how the air glows
// and shimmers above it, and how far the eye may open down there.
//
// PRESENTATION ONLY. Nothing below reads or changes the voxel light field, the
// world clock or anything the simulation or the network sees; the Original
// ("Classic") presentation uses none of it and keeps its fade to black.
//
// The shaders in src/hell_atmosphere.cpp evaluate these same expressions with
// these same constants (they are written into the shader text at load), so the
// tests of this arithmetic are tests of what is drawn.
// ---------------------------------------------------------------------------

[[nodiscard]] inline float hell_ease(float value) noexcept
{
    const float t = std::clamp(value, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// --- Depth ------------------------------------------------------------------
// A SURFACE takes the cavern's glow between these heights. The band sits a
// little under the eye's own (hell_presence: -24 .. -36) and ends at Hell's
// roof, so a dry cave a few blocks above the roof stays cool and torch-lit
// while the roof itself is fully lit from below.
inline constexpr float hell_surface_fade_top = -27.0F;
inline constexpr float hell_surface_fade_bottom = -35.0F;

[[nodiscard]] inline float hell_surface_presence(float height) noexcept
{
    return hell_ease((hell_surface_fade_top - height) /
                     (hell_surface_fade_top - hell_surface_fade_bottom));
}

// --- Exposure ---------------------------------------------------------------
// Above ground and in caves auto exposure may only close down
// (post::auto_exposure), so a night or a cave is never brightened into day.
// Hell is the one place with no bright reference to protect: there the eye may
// open by this much (a stop and a third), which is what makes a dry passage
// with no lava in sight readable, and it still closes down over a lava lake.
inline constexpr float hell_exposure_headroom = 2.50F;
inline constexpr float hell_exposure_floor = 0.354F;
inline constexpr float hell_exposure_key = 0.20F;

[[nodiscard]] inline float hell_exposure_limit(float eye_presence) noexcept
{
    return std::lerp(1.0F, hell_exposure_headroom, std::clamp(eye_presence, 0.0F, 1.0F));
}

[[nodiscard]] inline float hell_auto_exposure(float average_luminance, float eye_presence) noexcept
{
    return std::clamp(hell_exposure_key / std::max(average_luminance, 0.0008F),
                      hell_exposure_floor, hell_exposure_limit(eye_presence));
}

// --- Lava as an area light ----------------------------------------------------
// `coverage` is the share of a surface's footprint on the lava map that is
// lava (LavaField), `height` how far the surface is above that lava.

// Share of an endless plane below that a surface turned this way can see: all
// of it for a roof, half for a wall, none for a floor.
[[nodiscard]] inline float lava_form_factor(float normal_y) noexcept
{
    return std::clamp(0.5F - 0.5F * normal_y, 0.0F, 1.0F);
}

// Which level of the lava map a surface this high above the lava reads: one
// whose texels are about as wide as the surface is high, the footprint of the
// cosine kernel of an area light.
inline constexpr float lava_footprint_scale = 1.1F;
inline constexpr float lava_far_top_level = 6.0F;

[[nodiscard]] inline float lava_footprint_level(float height) noexcept
{
    return std::clamp(std::log2(std::max(height, 0.0F) * lava_footprint_scale + 1.0F), 0.0F,
                      lava_far_top_level);
}

// A lake has an edge and a cavern has hills in the way: light from below
// fades with the square of the height, to half at this many blocks.
inline constexpr float lava_far_half_height = 20.0F;
// The voxel light field already carries the first blocks above lava, with real
// occlusion. The area light takes over between these heights instead of adding
// to it.
inline constexpr float lava_far_fade_in_start = 2.0F;
inline constexpr float lava_far_fade_in_end = 9.0F;

[[nodiscard]] inline float lava_height_falloff(float height) noexcept
{
    if (height <= -0.5F)
        return 0.0F;
    const float h = std::max(height, 0.0F);
    const float reach = lava_far_half_height * lava_far_half_height;
    return hell_ease((h - lava_far_fade_in_start) /
                     (lava_far_fade_in_end - lava_far_fade_in_start)) /
           (1.0F + h * h / reach);
}

// Irradiance, as a share of the lava's own, on a surface `height` above lava
// covering `coverage` of its footprint.
[[nodiscard]] inline float lava_far_light(float coverage, float height, float normal_y) noexcept
{
    return std::clamp(coverage, 0.0F, 1.0F) * lava_form_factor(normal_y) *
           lava_height_falloff(height);
}

// --- Air ----------------------------------------------------------------------
// The glow and the heat in the air above lava both thin out with height; heat
// stays lower than light. Nothing rises from under the lava.
inline constexpr float lava_glow_height = 7.0F;
inline constexpr float lava_heat_height = 4.5F;

[[nodiscard]] inline float lava_air_share(float height, float scale_height) noexcept
{
    if (height <= -1.5F)
        return 0.0F;
    return hell_ease((height + 1.5F) / 1.5F) *
           std::exp(-std::max(height, 0.0F) / std::max(scale_height, 0.01F));
}

// How much of the full shimmer a sight line shows after crossing `heat_path`
// blocks of air at the lava's own heat. It saturates: a long look across a
// lake shimmers fully, never more, and the block in front of the player hardly
// at all.
inline constexpr float heat_haze_saturation = 0.085F;

[[nodiscard]] inline float heat_haze_amount(float heat_path) noexcept
{
    return 1.0F - std::exp(-std::max(heat_path, 0.0F) * heat_haze_saturation);
}

// The largest shift of the picture the shimmer may make, as a share of its
// height (2.6 pixels at 1080p, 3.5 at 1440p): enough to read as hot air on
// the far shore of a lake, too little to move what the crosshair is on.
inline constexpr float heat_haze_reach = 0.0024F;

// Molten rock breathes: three slow, unrelated swells. Always within 13% of
// steady, never fast enough to read as a flicker.
[[nodiscard]] inline float lava_pulse(float seconds) noexcept
{
    return 1.0F + 0.060F * std::sin(seconds * 0.83F) + 0.040F * std::sin(seconds * 2.10F + 1.3F) +
           0.022F * std::sin(seconds * 5.30F + 4.0F);
}

// --- Colours (linear scene light) ---------------------------------------------
// The cavern's own glow where no lava is in view: what a floor receives from
// the dim roof above it, and what a roof or an overhang receives from below.
// Deliberately not pure red: with green and blue in it rock keeps its texture
// and unlit stone reads dark red-brown instead of black.
inline constexpr SkyColor hell_glow_from_above{0.440F, 0.152F, 0.097F};
inline constexpr SkyColor hell_glow_from_below{0.780F, 0.255F, 0.145F};
// Irradiance under a roof directly over an endless lake.
inline constexpr SkyColor hell_lava_light{1.40F, 0.44F, 0.075F};
// What the cavern fades into where nothing is drawn any more: its own dark,
// warm and dim, so the lava's glow in the air is what lights the distance.
// (The older flat fill fades into a saturated red instead: hell_presence's
// fog colours above.)
inline constexpr SkyColor hell_far_haze{0.045F, 0.0105F, 0.0062F};

// What the first-person body is lit by in the deep layer: a dim fill from all
// round and the light from below that gives hands and a held tool their shape.
struct HellBodyLight final
{
    SkyColor ambient{};
    SkyColor below{};
};

// `lava_light` is lava_far_light() plus the flood-filled lava light at the
// player, 0..1; `strength` is the "Deep underground light" setting.
[[nodiscard]] inline HellBodyLight hell_body_light(float presence, float lava_light,
                                                   float strength, float pulse) noexcept
{
    const float amount = std::clamp(presence, 0.0F, 1.0F) * std::clamp(strength, 0.0F, 2.0F);
    const float lava = std::clamp(lava_light, 0.0F, 1.0F) * std::clamp(strength, 0.0F, 2.0F) * pulse;
    // The same irradiance the terrain shader gives a floor (ambient) and the
    // extra a surface turned to the lava receives (below).
    HellBodyLight light;
    light.ambient = {hell_glow_from_above.red * amount, hell_glow_from_above.green * amount,
                     hell_glow_from_above.blue * amount};
    light.below = {(hell_glow_from_below.red - hell_glow_from_above.red) * amount +
                       hell_lava_light.red * lava,
                   (hell_glow_from_below.green - hell_glow_from_above.green) * amount +
                       hell_lava_light.green * lava,
                   (hell_glow_from_below.blue - hell_glow_from_above.blue) * amount +
                       hell_lava_light.blue * lava};
    return light;
}

} // namespace openminerz::world
