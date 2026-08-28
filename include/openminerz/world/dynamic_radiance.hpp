#pragma once

#include "openminerz/world/player.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace openminerz::world
{

inline constexpr int dynamic_radiance_radius = 9;
inline constexpr int dynamic_radiance_extent = dynamic_radiance_radius * 2 + 1;
inline constexpr std::size_t dynamic_radiance_max_sources = 5U;

struct DynamicRadianceSource final
{
    Vec3 position{};
    // Linear-light source colour. Intensity is separate so short explosion
    // peaks can exceed one before the packed clipmap clamps the final result.
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    float radius{static_cast<float>(dynamic_radiance_radius)};
    float intensity{1.0F};
    // Transient emissive VFX seed their air cell and mark directly illuminated
    // voxel surfaces. Held torches leave this zero because their shadowed
    // direct term already exists in the terrain shader.
    float transient_direct{};
};

struct DynamicRadianceVolume final
{
    int origin_x{};
    int origin_y{};
    int origin_z{};
    bool active{};
    // Gamma-two companded linear RGB plus a transient-direct surface marker.
    // Visibility-tested surface seeds propagate only through transparent cells
    // before the GPU decodes them by squaring the R8G8B8A8_UNORM texel.
    std::vector<std::array<std::uint8_t, 4>> texels;

    [[nodiscard]] std::array<float, 3> radiance_at(int x, int y, int z) const noexcept;
};

// Builds a small, player-centred surface-radiance clipmap for bounded moving
// lights. Directly visible voxel faces become reflected-light seeds; receiving
// shaders gather those seeds in their outward hemisphere. No screen history or
// camera visibility is involved, so the result remains valid off-screen. At
// most dynamic_radiance_max_sources are consumed in caller-priority order.
[[nodiscard]] DynamicRadianceVolume build_dynamic_radiance(
    const WorldRegion &region, Vec3 volume_center,
    std::span<const DynamicRadianceSource> sources);

// Compatibility helper for the original single held-torch path.
[[nodiscard]] DynamicRadianceVolume build_dynamic_radiance(const WorldRegion &region,
                                                            Vec3 light_position,
                                                            bool enabled = true);

// Stable quadratic decay shared by transient VFX lights and unit tests.
[[nodiscard]] float dynamic_radiance_lifetime_fade(float age, float lifetime) noexcept;

} // namespace openminerz::world
