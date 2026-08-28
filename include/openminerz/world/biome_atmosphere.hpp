#pragma once

#include "openminerz/world/sky.hpp"
#include "openminerz/world/world.hpp"

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
                0.33F,
                0.28F,
                0.10F,
                0.45F,
                0.18F,
                0.92F,
                0.38F};
    case BiomeKind::desert:
        return {{0.60F, 0.46F, 0.29F},
                {0.78F, 0.61F, 0.40F},
                {0.15F, 0.31F, 0.52F},
                {1.08F, 1.01F, 0.86F},
                0.46F,
                0.35F,
                0.10F,
                0.34F,
                0.26F,
                0.94F,
                0.08F};
    case BiomeKind::mountain:
        return {{0.40F, 0.47F, 0.55F},
                {0.56F, 0.68F, 0.77F},
                {0.10F, 0.27F, 0.50F},
                {0.95F, 1.01F, 1.08F},
                0.22F,
                0.18F,
                0.12F,
                0.50F,
                0.12F,
                0.90F,
                0.16F};
    case BiomeKind::arctic:
        return {{0.60F, 0.69F, 0.78F},
                {0.72F, 0.82F, 0.90F},
                {0.11F, 0.28F, 0.54F},
                {0.88F, 1.02F, 1.12F},
                0.50F,
                0.42F,
                0.18F,
                0.40F,
                0.25F,
                0.82F,
                0.12F};
    case BiomeKind::descent:
        return {{0.26F, 0.22F, 0.31F},
                {0.43F, 0.34F, 0.48F},
                {0.09F, 0.07F, 0.17F},
                {1.03F, 0.90F, 1.07F},
                0.34F,
                0.28F,
                0.22F,
                0.42F,
                0.20F,
                0.88F,
                0.08F};
    case BiomeKind::classic:
        // The origin biome is the game's storm front. Keep noon readable, but
        // never let the 12:00-16:00 sun wash it into a clear blue grassland.
        return {{0.27F, 0.31F, 0.33F},
                {0.34F, 0.39F, 0.41F},
                {0.09F, 0.13F, 0.16F},
                {0.90F, 0.94F, 0.96F},
                0.52F,
                0.46F,
                0.30F,
                0.36F,
                0.22F,
                0.84F,
                1.0F};
    }
    return {};
}

} // namespace openminerz::world
