#pragma once

#include "openminerz/world/block.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/world.hpp"

#include <cstdint>
#include <optional>

namespace openminerz::world
{

struct ExplosiveDamageBands final
{
    float kill_range{};
    float damage_range{};
    float enemy_range{};
};

enum class ExplosiveKind : std::uint8_t
{
    tnt,
    c4,
    rocket,
    laser,
    he_grenade,
    harvest,
};

// TracerManager arms neither a fuse nor a mining action when a bullet strikes
// one of these blocks; it sends an immediate DetonateExplosiveMessage instead.
[[nodiscard]] constexpr std::optional<ExplosiveKind> placed_explosive_kind(BlockType block) noexcept
{
    if (block == BlockType::tnt)
        return ExplosiveKind::tnt;
    if (block == BlockType::c4)
        return ExplosiveKind::c4;
    return std::nullopt;
}

struct ExplosiveDefinition final
{
    int destruction_range{};
    ExplosiveDamageBands damage{};
};

inline constexpr float placed_explosive_fuse_seconds = 4.0F;
inline constexpr float grenade_total_fuse_seconds = 5.0F;
inline constexpr float grenade_max_cook_seconds = 4.0F;
inline constexpr float grenade_throw_speed = 15.0F;
inline constexpr std::uint8_t sticky_grenade_type = 3U;

// GrenadeProjectile gives Sticky only one collision pass, so its first terrain
// or enemy contact stops it instead of reflecting through the remaining budget.
[[nodiscard]] constexpr int grenade_collision_budget(std::uint8_t grenade_type) noexcept
{
    return grenade_type == sticky_grenade_type ? 1 : 10;
}

// GrenadeProjectile.Create uses HEGrenadeProjectile for both HE and Sticky;
// HEGrenadeProjectile.Explode hard-codes the outgoing detonation type to HE.
[[nodiscard]] constexpr std::uint8_t grenade_detonation_type(std::uint8_t grenade_type) noexcept
{
    return grenade_type == sticky_grenade_type ? 0U : grenade_type;
}

[[nodiscard]] constexpr bool grenade_remains_stuck(std::uint8_t grenade_type) noexcept
{
    return grenade_type == sticky_grenade_type;
}

inline constexpr ExplosiveDamageBands tnt_damage_bands{3.0F, 6.0F, 12.0F};
inline constexpr ExplosiveDamageBands c4_damage_bands{6.0F, 12.0F, 24.0F};
inline constexpr ExplosiveDamageBands rocket_damage_bands{3.0F, 6.0F, 12.0F};
inline constexpr ExplosiveDamageBands he_grenade_damage_bands{2.5F, 5.0F, 8.0F};

[[nodiscard]] constexpr ExplosiveDefinition explosive_definition(ExplosiveKind kind) noexcept
{
    switch (kind)
    {
    case ExplosiveKind::tnt:
        return {2, tnt_damage_bands};
    case ExplosiveKind::c4:
        return {3, c4_damage_bands};
    case ExplosiveKind::rocket:
        return {1, rocket_damage_bands};
    case ExplosiveKind::laser:
    case ExplosiveKind::harvest:
        return {0, {1.0F, 1.0F, 1.0F}};
    case ExplosiveKind::he_grenade:
        return {1, he_grenade_damage_bands};
    }
    return {};
}

// Full damage inside kill_range, then linear falloff to damage_range.
[[nodiscard]] float explosive_damage_fraction(float distance, ExplosiveDamageBands bands) noexcept;

// Original BlockType.Hardness table. Values 1..2 are Level1Hardness, 3..4
// are Level2Hardness, and 5 is blast-proof.
[[nodiscard]] std::uint8_t explosive_block_hardness(BlockType block) noexcept;

// Exact Explosive.BreakLookup + BlockWithinLevelBlastRange decision. TNT and
// C4 themselves are handled as chain sources before this predicate is used.
[[nodiscard]] bool explosive_breaks_block(ExplosiveKind kind, BlockType block, int offset_x,
                                          int offset_y, int offset_z) noexcept;

// Compatibility helper for the already-ported rocket path.
[[nodiscard]] bool rocket_breaks_block(BlockType block) noexcept;

// Harvest lasers recover ores; other destroyed terrain is cleared without a drop.
[[nodiscard]] std::optional<BlockType> harvested_block(BlockType block) noexcept;

// Original per-material BounceRestitution used by the grenade trace.
[[nodiscard]] float grenade_bounce_restitution(BlockType block) noexcept;

struct GrenadeTerrainHit final
{
    float time{};
    Vec3 position{};
    Vec3 normal{};
    BlockType block{BlockType::empty};
};

// GrenadeTraceProbe is a +/-0.1 AABB and only accepts blocking terrain. The
// returned time is normalized over start..end.
[[nodiscard]] std::optional<GrenadeTerrainHit> grenade_terrain_sweep(const WorldRegion &region,
                                                                     Vec3 start, Vec3 end) noexcept;

// GrenadeProjectile.ReflectVectorWithRestitution. Upward-facing contacts retain
// more tangential velocity than walls and ceilings.
[[nodiscard]] Vec3 grenade_reflect_vector(Vec3 input, Vec3 normal, float restitution) noexcept;

// GrenadeItem sends 5 - cookTime, with cookTime capped at four seconds.
[[nodiscard]] float grenade_seconds_left(float cook_seconds) noexcept;

} // namespace openminerz::world
