#pragma once

#include "openminerz/world/sky.hpp"
#include "openminerz/world/world.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace openminerz::world
{

// Rebirth atmosphere is authored per generated biome, but remains deliberately
// bounded: terrain lighting still comes from the sun, baked radiance and local
// emitters. These values only shape distant air and the final photographic
// response, so entering a desert cannot repaint nearby blocks as orange light.
struct BiomeAtmosphere final
{
    SkyColor fog_target{};
    SkyColor horizon_target{};
    SkyColor zenith_target{};
    SkyColor grade_multiplier{1.0F, 1.0F, 1.0F};
    float fog_mix{};
    float horizon_mix{};
    float zenith_mix{};
    float fog_start_ratio{0.55F};
    float grade_strength{};
    float saturation{1.0F};
    // 0 is an open sky; 1 preserves the dark authored storm deck throughout
    // daylight and softens the direct sun like dense overcast.
    float storminess{};
};

[[nodiscard]] constexpr BiomeAtmosphere biome_atmosphere(BiomeKind biome) noexcept
{
    switch (biome)
    {
    case BiomeKind::lagoon:
        return {{0.32F, 0.50F, 0.56F},
                {0.48F, 0.72F, 0.78F},
                {0.10F, 0.34F, 0.56F},
                {0.90F, 1.05F, 1.07F},
                0.22F,
                0.18F,
                0.10F,
                0.64F,
                0.10F,
                0.96F,
                0.38F};
    case BiomeKind::desert:
        return {{0.52F, 0.46F, 0.36F},
                {0.70F, 0.67F, 0.57F},
                {0.15F, 0.31F, 0.52F},
                {1.08F, 1.01F, 0.86F},
                0.26F,
                0.20F,
                0.10F,
                0.70F,
                0.12F,
                0.98F,
                0.08F};
    case BiomeKind::mountain:
        return {{0.40F, 0.47F, 0.55F},
                {0.56F, 0.68F, 0.77F},
                {0.10F, 0.27F, 0.50F},
                {0.95F, 1.01F, 1.08F},
                0.16F,
                0.14F,
                0.12F,
                0.76F,
                0.08F,
                0.98F,
                0.16F};
    case BiomeKind::arctic:
        return {{0.60F, 0.69F, 0.78F},
                {0.72F, 0.82F, 0.90F},
                {0.11F, 0.28F, 0.54F},
                {0.88F, 1.02F, 1.12F},
                0.32F,
                0.25F,
                0.18F,
                0.66F,
                0.12F,
                0.94F,
                0.12F};
    case BiomeKind::descent:
        return {{0.26F, 0.22F, 0.31F},
                {0.43F, 0.34F, 0.48F},
                {0.09F, 0.07F, 0.17F},
                {1.03F, 0.90F, 1.07F},
                0.24F,
                0.20F,
                0.22F,
                0.64F,
                0.10F,
                0.94F,
                0.08F};
    case BiomeKind::classic:
        // The origin biome is the game's storm front. Keep noon readable, but
        // never let the 12:00-16:00 sun wash it into a clear blue grassland.
        return {{0.27F, 0.31F, 0.33F},
                {0.34F, 0.39F, 0.41F},
                {0.09F, 0.13F, 0.16F},
                {0.90F, 0.94F, 0.96F},
                0.35F,
                0.32F,
                0.30F,
                0.62F,
                0.12F,
                0.94F,
                1.0F};
    }
    return {};
}

// Match the generator's radial transition bands, including the mirrored outer
// world. Sampling the position rather than the named biome avoids a palette pop
// when the player's feet cross an integer cell or a streamed chunk is replaced.
// mod-world: where a mod world generator runs, the app answers instead of the
// retail rings below (its biomes' `like`, blended); null is retail.
using BiomeAtmosphereHook = BiomeAtmosphere (*)(float x, float z) noexcept;
[[nodiscard]] inline std::atomic<BiomeAtmosphereHook> &biome_atmosphere_hook() noexcept
{
    static std::atomic<BiomeAtmosphereHook> hook{nullptr};
    return hook;
}

[[nodiscard]] inline BiomeAtmosphere biome_atmosphere_at(float x, float z) noexcept
{
    if (const BiomeAtmosphereHook hook = biome_atmosphere_hook().load(std::memory_order_relaxed))
        return hook(x, z); // mod-world
    float radius = std::fmod(std::hypot(x, z), 8800.0F);
    if (radius > 4400.0F)
        radius = 8800.0F - radius;
    constexpr std::array starts{200.0F, 900.0F, 1600.0F, 2300.0F, 2900.0F};
    constexpr std::array biomes{BiomeKind::classic,  BiomeKind::lagoon, BiomeKind::desert,
                                BiomeKind::mountain, BiomeKind::arctic, BiomeKind::descent};
    for (std::size_t index = 0; index < starts.size(); ++index)
    {
        if (radius >= starts[index] + 100.0F)
            continue;
        const auto a = biome_atmosphere(biomes[index]);
        const auto b = biome_atmosphere(biomes[index + 1]);
        float t = std::clamp((radius - starts[index]) / 100.0F, 0.0F, 1.0F);
        t = t * t * (3.0F - 2.0F * t);
        const auto color = [t](SkyColor left, SkyColor right) {
            return SkyColor{std::lerp(left.red, right.red, t),
                            std::lerp(left.green, right.green, t),
                            std::lerp(left.blue, right.blue, t)};
        };
        return {color(a.fog_target, b.fog_target),
                color(a.horizon_target, b.horizon_target),
                color(a.zenith_target, b.zenith_target),
                color(a.grade_multiplier, b.grade_multiplier),
                std::lerp(a.fog_mix, b.fog_mix, t),
                std::lerp(a.horizon_mix, b.horizon_mix, t),
                std::lerp(a.zenith_mix, b.zenith_mix, t),
                std::lerp(a.fog_start_ratio, b.fog_start_ratio, t),
                std::lerp(a.grade_strength, b.grade_strength, t),
                std::lerp(a.saturation, b.saturation, t),
                std::lerp(a.storminess, b.storminess, t)};
    }
    return biome_atmosphere(BiomeKind::descent);
}

inline void apply_biome_atmosphere(SkyState &sky, const BiomeAtmosphere &atmosphere) noexcept
{
    const float presence = 0.35F + 0.65F * sky.daylight;
    // These are daylight air colours, not light sources. Snow and desert dust
    // must not glow with their midday palette after the sun has gone down.
    const float air_light = 0.08F + 0.92F * sky.daylight;
    const auto blend = [presence, air_light](SkyColor source, SkyColor target, float mix) {
        const float amount = std::clamp(mix * presence, 0.0F, 1.0F);
        return SkyColor{std::lerp(source.red, target.red * air_light, amount),
                        std::lerp(source.green, target.green * air_light, amount),
                        std::lerp(source.blue, target.blue * air_light, amount)};
    };
    sky.fog = blend(sky.fog, atmosphere.fog_target, atmosphere.fog_mix);
    sky.horizon = blend(sky.horizon, atmosphere.horizon_target, atmosphere.horizon_mix);
    sky.zenith = blend(sky.zenith, atmosphere.zenith_target, atmosphere.zenith_mix);
    const float storm = std::clamp(atmosphere.storminess, 0.0F, 1.0F);
    const auto scale = [](SkyColor value, float amount) {
        return SkyColor{value.red * amount, value.green * amount, value.blue * amount};
    };
    sky.direct = scale(sky.direct, std::lerp(1.0F, 0.68F, storm));
    sky.specular = scale(sky.specular, std::lerp(1.0F, 0.68F, storm));
    sky.ambient = scale(sky.ambient, std::lerp(1.0F, 0.90F, storm));
}

} // namespace openminerz::world
