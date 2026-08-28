#include "openminerz/world/explosive.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openminerz::world
{

float explosive_damage_fraction(float distance, ExplosiveDamageBands bands) noexcept
{
    if (distance < 0.0F || bands.kill_range < 0.0F || bands.damage_range <= bands.kill_range ||
        distance >= bands.damage_range)
        return 0.0F;
    if (distance < bands.kill_range)
        return 1.0F;
    return std::clamp(
        1.0F - (distance - bands.kill_range) / (bands.damage_range - bands.kill_range), 0.0F, 1.0F);
}

bool rocket_breaks_block(BlockType block) noexcept
{
    return explosive_breaks_block(ExplosiveKind::rocket, block, 0, 0, 0);
}

std::uint8_t explosive_block_hardness(BlockType block) noexcept
{
    switch (block)
    {
    case BlockType::sand:
    case BlockType::snow:
    case BlockType::leaves:
    case BlockType::torch:
    case BlockType::torch_pos_x:
    case BlockType::torch_neg_z:
    case BlockType::torch_neg_x:
    case BlockType::torch_pos_z:
    case BlockType::torch_pos_y:
    case BlockType::torch_neg_y:
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::upper_door_open:
    case BlockType::tnt:
    case BlockType::c4:
        return 1U;
    case BlockType::dirt:
    case BlockType::grass:
    case BlockType::lantern:
    case BlockType::ice:
    case BlockType::log:
    case BlockType::wood:
    case BlockType::crate:
    case BlockType::crate_stone:
    case BlockType::crate_copper:
        return 2U;
    case BlockType::rock:
    case BlockType::gold_ore:
    case BlockType::iron_ore:
    case BlockType::copper_ore:
    case BlockType::coal_ore:
    case BlockType::diamond_ore:
    case BlockType::surface_lava:
    case BlockType::deep_lava:
    case BlockType::crate_iron:
    case BlockType::crate_gold:
        return 3U;
    case BlockType::blood_stone:
    case BlockType::space_rock:
    case BlockType::iron_wall:
    case BlockType::copper_wall:
    case BlockType::golden_wall:
    case BlockType::diamond_wall:
    case BlockType::slime:
    case BlockType::space_rock_inventory:
    case BlockType::crate_diamond:
    case BlockType::crate_bloodstone:
        return 4U;
    case BlockType::empty:
    case BlockType::fixed_lantern:
    case BlockType::bedrock:
    case BlockType::crate_safe:
    case BlockType::uninitialized:
        return 5U;
    }
    return 5U;
}

bool explosive_breaks_block(ExplosiveKind kind, BlockType block, int offset_x, int offset_y,
                            int offset_z) noexcept
{
    if (block == BlockType::empty || block == BlockType::fixed_lantern ||
        block == BlockType::bedrock || block == BlockType::space_rock ||
        block == BlockType::slime || block == BlockType::space_rock_inventory ||
        block == BlockType::uninitialized)
        return false;

    // TNT, laser and HE grenades preserve the valuable/resource subset. C4
    // and rockets are the two original exceptions.
    if (kind == ExplosiveKind::tnt || kind == ExplosiveKind::laser ||
        kind == ExplosiveKind::he_grenade)
    {
        switch (block)
        {
        case BlockType::lantern:
        case BlockType::gold_ore:
        case BlockType::iron_ore:
        case BlockType::copper_ore:
        case BlockType::coal_ore:
        case BlockType::diamond_ore:
        case BlockType::blood_stone:
        case BlockType::iron_wall:
        case BlockType::copper_wall:
        case BlockType::golden_wall:
        case BlockType::diamond_wall:
            return false;
        default:
            break;
        }
    }

    // Upper door halves are intentionally skipped by ProcessOneExplosion.
    if (block == BlockType::upper_door_closed || block == BlockType::upper_door_open)
        return false;

    const std::uint8_t hardness = explosive_block_hardness(block);
    int level_range{};
    if (hardness >= 1U && hardness <= 2U)
        level_range = kind == ExplosiveKind::tnt ? 1 : 2;
    else if (hardness >= 3U && hardness <= 4U)
        level_range = 1;
    else
        return false;

    const int scan_range = explosive_definition(kind).destruction_range;
    const int range = std::min(scan_range, level_range);
    return std::abs(offset_x) <= range && std::abs(offset_y) <= range &&
           std::abs(offset_z) <= range;
}

float grenade_bounce_restitution(BlockType block) noexcept
{
    switch (block)
    {
    case BlockType::sand:
    case BlockType::deep_lava:
    case BlockType::leaves:
    case BlockType::slime:
        return 0.1F;
    case BlockType::dirt:
    case BlockType::grass:
    case BlockType::snow:
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::upper_door_open:
    case BlockType::tnt:
        return 0.4F;
    default:
        return 0.6F;
    }
}

namespace
{

[[nodiscard]] std::optional<GrenadeTerrainHit> sweep_expanded_block(Vec3 start, Vec3 end, int x,
                                                                    int y, int z,
                                                                    BlockType block) noexcept
{
    constexpr float radius = 0.1F;
    const Vec3 minimum{static_cast<float>(x) - radius, static_cast<float>(y) - radius,
                       static_cast<float>(z) - radius};
    const Vec3 maximum{static_cast<float>(x + 1) + radius, static_cast<float>(y + 1) + radius,
                       static_cast<float>(z + 1) + radius};
    const Vec3 delta{end.x - start.x, end.y - start.y, end.z - start.z};
    float entry = -std::numeric_limits<float>::infinity();
    float exit = std::numeric_limits<float>::infinity();
    Vec3 entry_normal{};
    const auto slab = [&](float origin, float direction, float low, float high, Vec3 low_normal,
                          Vec3 high_normal) {
        if (std::abs(direction) < 0.000001F)
            return origin >= low && origin <= high;
        float near_time = (low - origin) / direction;
        float far_time = (high - origin) / direction;
        Vec3 near_normal = low_normal;
        if (near_time > far_time)
        {
            std::swap(near_time, far_time);
            near_normal = high_normal;
        }
        if (near_time > entry)
        {
            entry = near_time;
            entry_normal = near_normal;
        }
        exit = std::min(exit, far_time);
        return entry <= exit;
    };
    if (!slab(start.x, delta.x, minimum.x, maximum.x, {-1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}) ||
        !slab(start.y, delta.y, minimum.y, maximum.y, {0.0F, -1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}) ||
        !slab(start.z, delta.z, minimum.z, maximum.z, {0.0F, 0.0F, -1.0F}, {0.0F, 0.0F, 1.0F}) ||
        exit < 0.0F || entry > 1.0F)
        return std::nullopt;
    // A reflected segment begins exactly on the expanded face. Negative entry
    // means it is leaving an already-touched volume, so do not consume another
    // one of the original's ten collision iterations on that same face.
    if (entry < -0.000001F)
        return std::nullopt;
    const float time = std::clamp(entry, 0.0F, 1.0F);
    return GrenadeTerrainHit{time,
                             {std::lerp(start.x, end.x, time), std::lerp(start.y, end.y, time),
                              std::lerp(start.z, end.z, time)},
                             entry_normal,
                             block};
}

} // namespace

std::optional<GrenadeTerrainHit> grenade_terrain_sweep(const WorldRegion &region, Vec3 start,
                                                       Vec3 end) noexcept
{
    constexpr float radius = 0.1F;
    const int minimum_x = static_cast<int>(std::floor(std::min(start.x, end.x) - radius));
    const int maximum_x = static_cast<int>(std::floor(std::max(start.x, end.x) + radius));
    const int minimum_y = static_cast<int>(std::floor(std::min(start.y, end.y) - radius));
    const int maximum_y = static_cast<int>(std::floor(std::max(start.y, end.y) + radius));
    const int minimum_z = static_cast<int>(std::floor(std::min(start.z, end.z) - radius));
    const int maximum_z = static_cast<int>(std::floor(std::max(start.z, end.z) + radius));
    std::optional<GrenadeTerrainHit> earliest;
    for (int z = minimum_z; z <= maximum_z; ++z)
        for (int y = minimum_y; y <= maximum_y; ++y)
            for (int x = minimum_x; x <= maximum_x; ++x)
            {
                if (!region.contains(x, y, z))
                    continue;
                const BlockType block = region.block(x, y, z);
                if (!blocks_player(block))
                    continue;
                const auto hit = sweep_expanded_block(start, end, x, y, z, block);
                if (hit.has_value() && (!earliest.has_value() || hit->time < earliest->time))
                    earliest = hit;
            }
    return earliest;
}

Vec3 grenade_reflect_vector(Vec3 input, Vec3 normal, float restitution) noexcept
{
    const float dot = input.x * normal.x + input.y * normal.y + input.z * normal.z;
    const Vec3 reversed_normal{-normal.x * dot, -normal.y * dot, -normal.z * dot};
    const Vec3 tangent{input.x + reversed_normal.x, input.y + reversed_normal.y,
                       input.z + reversed_normal.z};
    const float tangent_scale = std::lerp(1.0F, restitution, std::max(normal.y, 0.0F));
    return {reversed_normal.x * restitution + tangent.x * tangent_scale,
            reversed_normal.y * restitution + tangent.y * tangent_scale,
            reversed_normal.z * restitution + tangent.z * tangent_scale};
}

float grenade_seconds_left(float cook_seconds) noexcept
{
    return grenade_total_fuse_seconds - std::clamp(cook_seconds, 0.0F, grenade_max_cook_seconds);
}

} // namespace openminerz::world
