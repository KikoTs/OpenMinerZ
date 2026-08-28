#include "openminerz/world/dynamic_radiance.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace openminerz::world
{
namespace
{

constexpr std::array<std::array<int, 3>, 6> face_offsets{{
    {{1, 0, 0}}, {{-1, 0, 0}}, {{0, 1, 0}},
    {{0, -1, 0}}, {{0, 0, 1}}, {{0, 0, -1}},
}};

std::size_t volume_index(int x, int y, int z) noexcept
{
    return static_cast<std::size_t>(x + y * dynamic_radiance_extent +
                                    z * dynamic_radiance_extent * dynamic_radiance_extent);
}

std::array<float, 3> linear_reflectance(BlockType type) noexcept
{
    const BlockColor color = fallback_color(type);
    const auto channel = [](std::uint8_t value) {
        const float display = static_cast<float>(value) / 255.0F;
        return display * display;
    };
    return {channel(color.red), channel(color.green), channel(color.blue)};
}

std::uint8_t encode_radiance(float value) noexcept
{
    return static_cast<std::uint8_t>(
        std::lround(std::sqrt(std::clamp(value, 0.0F, 1.0F)) * 255.0F));
}

} // namespace

std::array<float, 3> DynamicRadianceVolume::radiance_at(int x, int y, int z) const noexcept
{
    const int local_x = x - origin_x;
    const int local_y = y - origin_y;
    const int local_z = z - origin_z;
    const std::size_t expected = static_cast<std::size_t>(dynamic_radiance_extent) *
                                 dynamic_radiance_extent * dynamic_radiance_extent;
    if (!active || local_x < 0 || local_x >= dynamic_radiance_extent || local_y < 0 ||
        local_y >= dynamic_radiance_extent || local_z < 0 ||
        local_z >= dynamic_radiance_extent || texels.size() != expected)
    {
        return {};
    }
    const auto &packed = texels[volume_index(local_x, local_y, local_z)];
    const auto decode = [](std::uint8_t value) {
        const float companded = static_cast<float>(value) / 255.0F;
        return companded * companded;
    };
    return {decode(packed[0]), decode(packed[1]), decode(packed[2])};
}

DynamicRadianceVolume build_dynamic_radiance(const WorldRegion &region, Vec3 light_position,
                                             bool enabled)
{
    if (!enabled)
        return build_dynamic_radiance(region, light_position,
                                      std::span<const DynamicRadianceSource>{});
    constexpr std::array<float, 3> torch_linear{
        1.0F, (235.0F / 255.0F) * (235.0F / 255.0F),
        (190.0F / 255.0F) * (190.0F / 255.0F)};
    const DynamicRadianceSource held_torch{light_position, torch_linear,
                                           static_cast<float>(dynamic_radiance_radius), 1.0F};
    return build_dynamic_radiance(region, light_position,
                                  std::span<const DynamicRadianceSource>{&held_torch, 1U});
}

float dynamic_radiance_lifetime_fade(float age, float lifetime) noexcept
{
    if (!std::isfinite(age) || !std::isfinite(lifetime) || lifetime <= 0.0F)
        return 0.0F;
    const float remaining = std::clamp(1.0F - age / lifetime, 0.0F, 1.0F);
    return remaining * remaining;
}

DynamicRadianceVolume build_dynamic_radiance(
    const WorldRegion &region, Vec3 volume_center,
    std::span<const DynamicRadianceSource> sources)
{
    DynamicRadianceVolume volume;
    const int center_cell_x = static_cast<int>(std::floor(volume_center.x));
    const int center_cell_y = static_cast<int>(std::floor(volume_center.y));
    const int center_cell_z = static_cast<int>(std::floor(volume_center.z));
    volume.origin_x = center_cell_x - dynamic_radiance_radius;
    volume.origin_y = center_cell_y - dynamic_radiance_radius;
    volume.origin_z = center_cell_z - dynamic_radiance_radius;

    std::array<DynamicRadianceSource, dynamic_radiance_max_sources> effective_sources{};
    std::size_t source_count{};
    for (const DynamicRadianceSource &source : sources)
    {
        if (source_count >= effective_sources.size())
            break;
        if (!std::isfinite(source.position.x) || !std::isfinite(source.position.y) ||
            !std::isfinite(source.position.z) || !std::isfinite(source.radius) ||
            !std::isfinite(source.intensity) || !std::isfinite(source.transient_direct) ||
            source.radius <= 0.05F ||
            source.intensity <= 0.0F)
        {
            continue;
        }
        DynamicRadianceSource sanitized = source;
        sanitized.radius =
            std::clamp(sanitized.radius, 0.05F, static_cast<float>(dynamic_radiance_radius));
        sanitized.intensity = std::max(0.0F, sanitized.intensity);
        sanitized.transient_direct = std::clamp(sanitized.transient_direct, 0.0F, 1.0F);
        for (float &channel : sanitized.color)
            channel = std::isfinite(channel) ? std::max(0.0F, channel) : 0.0F;
        if (sanitized.color[0] <= 0.0F && sanitized.color[1] <= 0.0F &&
            sanitized.color[2] <= 0.0F)
        {
            continue;
        }
        effective_sources[source_count++] = sanitized;
    }
    if (source_count == 0U)
        return volume;

    const std::size_t cell_count = static_cast<std::size_t>(dynamic_radiance_extent) *
                                   dynamic_radiance_extent * dynamic_radiance_extent;
    using LinearCell = std::array<float, 4>;
    std::vector<LinearCell> linear_field(cell_count);
    std::vector<std::uint8_t> transports_radiance(cell_count, 0U);
    volume.active = true;

    for (int local_z = 0; local_z < dynamic_radiance_extent; ++local_z)
    {
        const int z = volume.origin_z + local_z;
        for (int local_y = 0; local_y < dynamic_radiance_extent; ++local_y)
        {
            const int y = volume.origin_y + local_y;
            for (int local_x = 0; local_x < dynamic_radiance_extent; ++local_x)
            {
                const int x = volume.origin_x + local_x;
                if (!region.contains(x, y, z))
                    continue;
                const std::size_t cell = volume_index(local_x, local_y, local_z);
                const BlockType block = region.block(x, y, z);
                transports_radiance[cell] = occludes(block) ? 0U : 1U;
                if (!occludes(block))
                    continue;
                const Vec3 center{static_cast<float>(x) + 0.5F,
                                  static_cast<float>(y) + 0.5F,
                                  static_cast<float>(z) + 0.5F};
                const auto reflectance = linear_reflectance(block);
                auto &seed = linear_field[cell];
                for (std::size_t source_index = 0U; source_index < source_count;
                     ++source_index)
                {
                    const DynamicRadianceSource &source = effective_sources[source_index];
                    const float to_light_x = source.position.x - center.x;
                    const float to_light_y = source.position.y - center.y;
                    const float to_light_z = source.position.z - center.z;
                    const float distance_squared = to_light_x * to_light_x +
                                                   to_light_y * to_light_y +
                                                   to_light_z * to_light_z;
                    const float radius_squared = source.radius * source.radius;
                    if (distance_squared <= 0.01F || distance_squared >= radius_squared)
                        continue;
                    const float distance = std::sqrt(distance_squared);
                    const float inverse_distance = 1.0F / distance;
                    float best_facing{};
                    for (const auto &normal : face_offsets)
                    {
                        const int neighbor_x = x + normal[0];
                        const int neighbor_y = y + normal[1];
                        const int neighbor_z = z + normal[2];
                        if (!region.contains(neighbor_x, neighbor_y, neighbor_z) ||
                            occludes(region.block(neighbor_x, neighbor_y, neighbor_z)))
                        {
                            continue;
                        }
                        const float facing =
                            (to_light_x * static_cast<float>(normal[0]) +
                             to_light_y * static_cast<float>(normal[1]) +
                             to_light_z * static_cast<float>(normal[2])) *
                            inverse_distance;
                        best_facing = std::max(best_facing, facing);
                    }
                    if (best_facing <= 0.02F)
                        continue;

                    const Vec3 toward_surface{-to_light_x * inverse_distance,
                                               -to_light_y * inverse_distance,
                                               -to_light_z * inverse_distance};
                    const auto hit = PlayerSimulation::raycast_blocking(
                        region, source.position, toward_surface, distance + 0.75F);
                    if (!hit.has_value() || hit->x != x || hit->y != y || hit->z != z)
                        continue;

                    const float falloff = 1.0F - distance / source.radius;
                    const float strength =
                        falloff * falloff * best_facing * source.intensity;
                    for (std::size_t channel = 0; channel < reflectance.size(); ++channel)
                    {
                        seed[channel] = std::min(
                            1.0F, seed[channel] + reflectance[channel] *
                                                       source.color[channel] * strength);
                    }
                    if (source.transient_direct > 0.0F)
                        seed[3] = std::max(seed[3], source.transient_direct);
                }
            }
        }
    }

    // Emissive projectiles and flashes also occupy transparent space. Seeding
    // that cell lets nearby enemies and particles receive a compact colored
    // glow; the same transparent-cell propagation rule prevents wall leakage.
    for (std::size_t source_index = 0U; source_index < source_count; ++source_index)
    {
        const DynamicRadianceSource &source = effective_sources[source_index];
        if (source.transient_direct <= 0.0F)
            continue;
        const int local_x = static_cast<int>(std::floor(source.position.x)) - volume.origin_x;
        const int local_y = static_cast<int>(std::floor(source.position.y)) - volume.origin_y;
        const int local_z = static_cast<int>(std::floor(source.position.z)) - volume.origin_z;
        if (local_x < 0 || local_x >= dynamic_radiance_extent || local_y < 0 ||
            local_y >= dynamic_radiance_extent || local_z < 0 ||
            local_z >= dynamic_radiance_extent)
        {
            continue;
        }
        const std::size_t cell = volume_index(local_x, local_y, local_z);
        if (transports_radiance[cell] == 0U)
            continue;
        for (std::size_t channel = 0; channel < 3U; ++channel)
        {
            linear_field[cell][channel] =
                std::max(linear_field[cell][channel],
                         std::min(1.0F, source.color[channel] * source.intensity));
        }
    }

    // Three Jacobi-style max-propagation steps turn isolated surface seeds into
    // a compact radiance field. Transport is forbidden inside opaque cells, so
    // an occluder still blocks the field while open corners receive a restrained
    // diffuse wrap. World-aligned cells avoid temporal noise or camera history.
    constexpr float transport = 0.68F;
    constexpr int propagation_steps = 3;
    for (int iteration = 0; iteration < propagation_steps; ++iteration)
    {
        auto next = linear_field;
        for (int local_z = 0; local_z < dynamic_radiance_extent; ++local_z)
            for (int local_y = 0; local_y < dynamic_radiance_extent; ++local_y)
                for (int local_x = 0; local_x < dynamic_radiance_extent; ++local_x)
                {
                    const std::size_t cell = volume_index(local_x, local_y, local_z);
                    if (transports_radiance[cell] == 0U)
                        continue;
                    LinearCell strongest{};
                    for (const auto &offset : face_offsets)
                    {
                        const int neighbor_x = local_x + offset[0];
                        const int neighbor_y = local_y + offset[1];
                        const int neighbor_z = local_z + offset[2];
                        if (neighbor_x < 0 || neighbor_x >= dynamic_radiance_extent ||
                            neighbor_y < 0 || neighbor_y >= dynamic_radiance_extent ||
                            neighbor_z < 0 || neighbor_z >= dynamic_radiance_extent)
                        {
                            continue;
                        }
                        const auto &neighbor =
                            linear_field[volume_index(neighbor_x, neighbor_y, neighbor_z)];
                        for (std::size_t channel = 0; channel < 3U; ++channel)
                            strongest[channel] = std::max(strongest[channel], neighbor[channel]);
                    }
                    for (std::size_t channel = 0; channel < 3U; ++channel)
                        next[cell][channel] =
                            std::max(next[cell][channel], strongest[channel] * transport);
                }
        linear_field = std::move(next);
    }

    volume.texels.resize(cell_count);
    for (std::size_t cell = 0; cell < cell_count; ++cell)
        for (std::size_t channel = 0; channel < volume.texels[cell].size(); ++channel)
            volume.texels[cell][channel] = encode_radiance(linear_field[cell][channel]);
    return volume;
}

} // namespace openminerz::world
