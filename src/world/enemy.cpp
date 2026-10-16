#include "openminerz/world/enemy.hpp"

#include "enemy_simulation.hpp"

#include "openminerz/world/dragon_brain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openminerz::world
{
namespace
{

constexpr float pi = 3.14159265358979323846F;

// Distances the ramps divide by, straight from EnemyType.Init.
constexpr float zombie_distance_step = 188.88889F;
constexpr float archer_distance_step = 425.0F;
constexpr float cave_distance_step = 141.66667F;
constexpr float felguard_probability = 0.1F;

[[nodiscard]] std::optional<float> segment_box_time(Vec3 start, Vec3 end, Vec3 minimum,
                                                    Vec3 maximum) noexcept
{
    const Vec3 delta{end.x - start.x, end.y - start.y, end.z - start.z};
    float entry{};
    float exit = 1.0F;
    const auto slab = [&](float origin, float direction, float low, float high) {
        if (std::abs(direction) < 0.000001F)
            return origin >= low && origin <= high;
        float first = (low - origin) / direction;
        float second = (high - origin) / direction;
        if (first > second)
            std::swap(first, second);
        entry = std::max(entry, first);
        exit = std::min(exit, second);
        return entry <= exit;
    };
    if (!slab(start.x, delta.x, minimum.x, maximum.x) ||
        !slab(start.y, delta.y, minimum.y, maximum.y) ||
        !slab(start.z, delta.z, minimum.z, maximum.z) || exit < 0.0F || entry > 1.0F)
        return std::nullopt;
    return std::clamp(entry, 0.0F, 1.0F);
}

[[nodiscard]] std::optional<std::array<float, 2>> segment_box_interval(Vec3 start, Vec3 end,
                                                                       Vec3 minimum,
                                                                       Vec3 maximum) noexcept
{
    const Vec3 delta{end.x - start.x, end.y - start.y, end.z - start.z};
    float entry{};
    float exit = 1.0F;
    const auto slab = [&](float origin, float direction, float low, float high) {
        if (std::abs(direction) < 0.000001F)
            return origin >= low && origin <= high;
        float first = (low - origin) / direction;
        float second = (high - origin) / direction;
        if (first > second)
            std::swap(first, second);
        entry = std::max(entry, first);
        exit = std::min(exit, second);
        return entry <= exit;
    };
    if (!slab(start.x, delta.x, minimum.x, maximum.x) ||
        !slab(start.y, delta.y, minimum.y, maximum.y) ||
        !slab(start.z, delta.z, minimum.z, maximum.z) || exit < 0.0F || entry > 1.0F)
        return std::nullopt;
    return std::array{std::clamp(entry, 0.0F, 1.0F), std::clamp(exit, 0.0F, 1.0F)};
}

std::array<EnemyStats, enemy_type_count> build_enemy_stats() noexcept
{
    std::array<EnemyStats, enemy_type_count> table{};
    // Zombie tiers 0..17: ZombieEnemyType(model ZOMBIE, texture cycling 0..6,
    // maxDigHardness 2 for the first eight then 3, digMultiplier authored per tier).
    constexpr std::array<float, 18> zombie_dig{0.1F, 0.1F, 0.2F, 0.2F, 0.3F, 0.3F,
                                               0.4F, 0.4F, 0.5F, 0.5F, 0.6F, 0.6F,
                                               0.7F, 0.7F, 0.8F, 0.8F, 0.9F, 1.0F};
    constexpr std::array<std::uint8_t, 18> zombie_texture{0U, 1U, 2U, 3U, 4U, 5U, 6U, 0U, 1U,
                                                          2U, 3U, 4U, 5U, 6U, 0U, 1U, 2U, 3U};
    for (int type = 0; type < 18; ++type)
    {
        EnemyStats &stats = table[static_cast<std::size_t>(type)];
        stats.enemy_class = EnemyClass::zombie;
        stats.model = EnemyModelKind::zombie;
        stats.texture_index = zombie_texture[static_cast<std::size_t>(type)];
        stats.habitat = EnemyHabitat::aboveground;
        stats.chance_of_bullet_strike = 1.0F;
        stats.spawn_radius = 20;
        stats.attack_animation_speed = 2.0F;
        stats.die_animation_speed = 2.0F;
        stats.hit_animation_speed = 2.0F;
        stats.spawn_animation_speed = 3.0F;
        stats.has_run_fast = true;
        stats.random_slow_speed = 0.5F;
        stats.digging_multiplier = zombie_dig[static_cast<std::size_t>(type)];
        stats.hardest_block_dug = type < 8 ? 2 : 3;
        // EnemyType.Init ramp: health 1..18 (the last tier x1.5), speeds keyed to type/16.
        const float ramp = static_cast<float>(type) / 16.0F;
        stats.starting_health =
            type == 17 ? static_cast<float>(type + 1) * 1.5F : static_cast<float>(type + 1);
        stats.base_slow_speed = 0.5F + ramp * 1.5F;
        stats.base_fast_speed = 6.5F + ramp * 1.5F;
        stats.base_run_activation_time = 4.0F - ramp;
        stats.random_run_activation_time = 1.0F;
        stats.fast_jump_speed = 13.0F + 3.0F * ramp;
    }
    // Archers 18..25: aboveground dusk/dawn skeletons, health 1 + 2.5 per tier.
    constexpr std::array<std::uint8_t, 8> archer_texture{7U, 8U, 9U, 10U, 11U, 7U, 8U, 9U};
    for (int index = 0; index < 8; ++index)
    {
        EnemyStats &stats = table[static_cast<std::size_t>(18 + index)];
        stats.enemy_class = EnemyClass::archer;
        stats.model = EnemyModelKind::skeleton_archer;
        stats.texture_index = archer_texture[static_cast<std::size_t>(index)];
        stats.habitat = EnemyHabitat::aboveground;
        stats.chance_of_bullet_strike = 0.6F;
        stats.spawn_radius = 40;
        stats.starting_health = 1.0F + 2.5F * static_cast<float>(index);
    }
    // Cave skeletons 26..49 exactly as EnemyType.Init interleaves the zombie, sword
    // and axes classes; health 1 + 0.9 per step through the whole run.
    struct SkeletonEntry final
    {
        EnemyClass enemy_class;
        EnemyModelKind model;
        std::uint8_t texture;
    };
    constexpr std::array<SkeletonEntry, 24> skeletons{{
        {EnemyClass::skeleton, EnemyModelKind::skeleton_zombie, 7U},
        {EnemyClass::skeleton, EnemyModelKind::skeleton_zombie, 8U},
        {EnemyClass::skeleton, EnemyModelKind::skeleton_zombie, 9U},
        {EnemyClass::skeleton, EnemyModelKind::skeleton_zombie, 10U},
        {EnemyClass::skeleton, EnemyModelKind::skeleton_zombie, 11U},
        {EnemyClass::skeleton_sword, EnemyModelKind::skeleton_sword, 7U},
        {EnemyClass::skeleton, EnemyModelKind::skeleton_zombie, 8U},
        {EnemyClass::skeleton_sword, EnemyModelKind::skeleton_sword, 9U},
        {EnemyClass::skeleton, EnemyModelKind::skeleton_zombie, 10U},
        {EnemyClass::skeleton_sword, EnemyModelKind::skeleton_sword, 11U},
        {EnemyClass::skeleton, EnemyModelKind::skeleton_zombie, 7U},
        {EnemyClass::skeleton_sword, EnemyModelKind::skeleton_sword, 8U},
        {EnemyClass::skeleton_axes, EnemyModelKind::skeleton_axes, 9U},
        {EnemyClass::skeleton_sword, EnemyModelKind::skeleton_sword, 10U},
        {EnemyClass::skeleton_axes, EnemyModelKind::skeleton_axes, 11U},
        {EnemyClass::skeleton_sword, EnemyModelKind::skeleton_sword, 7U},
        {EnemyClass::skeleton_axes, EnemyModelKind::skeleton_axes, 8U},
        {EnemyClass::skeleton_sword, EnemyModelKind::skeleton_sword, 9U},
        {EnemyClass::skeleton_axes, EnemyModelKind::skeleton_axes, 10U},
        {EnemyClass::skeleton_sword, EnemyModelKind::skeleton_sword, 11U},
        {EnemyClass::skeleton_axes, EnemyModelKind::skeleton_axes, 7U},
        {EnemyClass::skeleton_axes, EnemyModelKind::skeleton_axes, 8U},
        {EnemyClass::skeleton_axes, EnemyModelKind::skeleton_axes, 9U},
        {EnemyClass::skeleton_axes, EnemyModelKind::skeleton_axes, 10U},
    }};
    for (int index = 0; index < 24; ++index)
    {
        EnemyStats &stats = table[static_cast<std::size_t>(26 + index)];
        const SkeletonEntry &entry = skeletons[static_cast<std::size_t>(index)];
        stats.enemy_class = entry.enemy_class;
        stats.model = entry.model;
        stats.texture_index = entry.texture;
        stats.habitat = EnemyHabitat::caves;
        stats.chance_of_bullet_strike = 0.6F;
        stats.spawn_radius = 10;
        stats.starting_health = 1.0F + 0.9F * static_cast<float>(index);
    }
    EnemyStats &felguard = table[enemy_felguard];
    felguard.enemy_class = EnemyClass::felguard;
    felguard.model = EnemyModelKind::felguard;
    felguard.texture_index = 12U;
    felguard.habitat = EnemyHabitat::hell;
    felguard.chance_of_bullet_strike = 0.6F;
    felguard.spawn_radius = 10;
    felguard.starting_health = 140.0F;
    EnemyStats &alien = table[enemy_alien];
    alien.enemy_class = EnemyClass::alien;
    alien.model = EnemyModelKind::alien;
    alien.texture_index = 13U;
    alien.habitat = EnemyHabitat::crash_site;
    alien.chance_of_bullet_strike = 0.6F;
    alien.spawn_radius = 10;
    alien.starting_health = 70.0F;
    return table;
}

[[nodiscard]] bool has_flag(DamageFlags value, DamageFlags flag) noexcept
{
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0U;
}

// Attack tables from the *TryAttack states.
constexpr std::array<EnemyAttack, 5> zombie_attack_list{{
    {"attack1", {0.8333F}, 1, 0.4F, 1.6F},
    {"attack2", {0.8333F, 1.466667F}, 2, 0.3F, 1.8F},
    {"attack3", {0.7333F}, 1, 0.4F, 1.6F},
    {"attack4", {0.9333F}, 1, 0.4F, 2.1F},
    {"attack5", {1.5F}, 1, 0.6F, 2.1F},
}};
constexpr std::array<EnemyAttack, 5> skeleton_attack_list{{
    {"attack1", {0.7F}, 1, 0.4F, 1.6F},
    {"attack2", {0.9333F}, 1, 0.4F, 2.0F},
    {"attack1", {0.7F}, 1, 0.4F, 1.6F},
    {"attack2", {0.9333F}, 1, 0.4F, 2.0F},
    {"attack3", {1.5667F}, 1, 0.6F, 2.1F},
}};
constexpr std::array<EnemyAttack, 11> axes_attack_list{{
    {"attack1", {0.7F}, 1, 0.4F, 1.6F},
    {"attack2", {0.9333F}, 1, 0.4F, 2.0F},
    {"attack1", {0.7F}, 1, 0.4F, 1.6F},
    {"attack2", {0.9333F}, 1, 0.4F, 2.0F},
    {"attack1", {0.7F}, 1, 0.4F, 1.6F},
    {"attack2", {0.9333F}, 1, 0.4F, 2.0F},
    {"attack3", {1.5667F}, 1, 0.6F, 2.1F},
    {"axes_atack1", {0.2667F, 0.5667F, 0.933F, 1.2667F}, 4, 0.2F, 1.6F},
    {"axes_atack2", {0.3333F, 0.6667F, 1.033F, 1.3667F}, 4, 0.2F, 1.6F},
    {"axes_atack1", {0.2667F, 0.5667F, 0.933F, 1.2667F}, 4, 0.2F, 1.6F},
    {"axes_atack2", {0.3333F, 0.6667F, 1.033F, 1.3667F}, 4, 0.2F, 1.6F},
}};
constexpr std::array<EnemyAttack, 2> alien_attack_list{{
    {"Attack1", {0.7F}, 1, 0.6F, 1.6F},
    {"Attack2", {0.7F}, 1, 0.8F, 2.1F},
}};
constexpr std::array<EnemyAttack, 2> felguard_attack_list{{
    {"Attack1", {0.7F}, 1, 0.4F, 1.6F},
    {"atack_3", {1.5667F}, 1, 0.6F, 2.1F},
}};

[[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] int block_dig_hardness(BlockType type) noexcept
{
    switch (type)
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
        return 1;
    case BlockType::dirt:
    case BlockType::grass:
    case BlockType::log:
    case BlockType::wood:
    case BlockType::crate:
    case BlockType::crate_stone:
    case BlockType::crate_copper:
    case BlockType::ice:
    case BlockType::lantern:
        return 2;
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
        return 3;
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
        return 4;
    case BlockType::fixed_lantern:
    case BlockType::bedrock:
    case BlockType::crate_safe:
        return 5;
    case BlockType::empty:
    case BlockType::uninitialized:
        return 0;
    }
    return 5;
}

// BaseZombie.PlayerAABB, the box weapons trace against. Movement itself is the
// original swept box in enemy_simulation.cpp.
constexpr float enemy_half_width = 0.35F;
constexpr float enemy_height = 1.65F;

} // namespace

int enemy_dig_hardness(BlockType type) noexcept
{
    return block_dig_hardness(type);
}

const EnemyStats &enemy_stats(int type) noexcept
{
    static const auto table = build_enemy_stats();
    const int clamped = std::clamp(type, 0, enemy_type_count - 1);
    return table[static_cast<std::size_t>(clamped)];
}

float enemy_damage_multiplier(int type, DamageFlags damage, bool headshot) noexcept
{
    const EnemyClass enemy_class = enemy_stats(type).enemy_class;
    float multiplier = 1.0F;
    switch (enemy_class)
    {
    case EnemyClass::zombie:
        if (has_flag(damage, DamageFlags::blunt))
            multiplier *= 0.5F;
        else if (has_flag(damage, DamageFlags::blade))
            multiplier *= 0.75F;
        if (headshot)
            multiplier *= 2.5F;
        break;
    case EnemyClass::alien:
        if (has_flag(damage, DamageFlags::piercing))
            multiplier *= 0.25F;
        else if (has_flag(damage, DamageFlags::shotgun))
            multiplier *= 1.25F;
        else if (has_flag(damage, DamageFlags::blade))
            multiplier *= 0.75F;
        if (headshot)
            multiplier *= 2.0F;
        break;
    default:
        // Skeleton family and the Felguard share the same table.
        if (has_flag(damage, DamageFlags::piercing))
            multiplier *= 0.5F;
        else if (has_flag(damage, DamageFlags::shotgun))
            multiplier *= 1.5F;
        else if (has_flag(damage, DamageFlags::blade))
            multiplier *= 0.75F;
        if (headshot)
            multiplier *= 2.0F;
        break;
    }
    return multiplier;
}

const EnemyAttackSet &enemy_attacks(EnemyClass enemy_class) noexcept
{
    // RageAnimation of each *TryAttack state: the clip played when the miss
    // budget runs out. Zombies and every skeleton play "enraged"; the Alien
    // and the Felguard replay "Attack1".
    static const EnemyAttackSet zombie{
        zombie_attack_list.data(), static_cast<int>(zombie_attack_list.size()), "enraged", 1.0F};
    static const EnemyAttackSet skeleton{skeleton_attack_list.data(),
                                         static_cast<int>(skeleton_attack_list.size()), "enraged",
                                         1.0F};
    static const EnemyAttackSet axes{axes_attack_list.data(),
                                     static_cast<int>(axes_attack_list.size()), "enraged", 1.0F};
    static const EnemyAttackSet alien{alien_attack_list.data(),
                                      static_cast<int>(alien_attack_list.size()), "Attack1", 1.0F};
    static const EnemyAttackSet felguard{felguard_attack_list.data(),
                                         static_cast<int>(felguard_attack_list.size()), "Attack1",
                                         1.0F};
    switch (enemy_class)
    {
    case EnemyClass::zombie:
        return zombie;
    case EnemyClass::skeleton_axes:
        return axes;
    case EnemyClass::alien:
        return alien;
    case EnemyClass::felguard:
        return felguard;
    case EnemyClass::archer:
    case EnemyClass::skeleton:
    case EnemyClass::skeleton_sword:
    default:
        return skeleton;
    }
}

std::optional<Vec3> initial_ballistic_vector(Vec3 origin, Vec3 target, float velocity,
                                             float gravity) noexcept
{
    const float gravity_squared = gravity * gravity;
    const float vertical = target.y - origin.y;
    const float horizontal_x = origin.x - target.x;
    const float horizontal_z = origin.z - target.z;
    const float horizontal = std::sqrt(horizontal_x * horizontal_x + horizontal_z * horizontal_z);
    if (horizontal == 0.0F)
        return Vec3{0.0F, vertical >= 0.0F ? velocity : -velocity, 0.0F};

    const float vertical_squared = vertical * vertical;
    const float horizontal_squared = horizontal * horizontal;
    float denominator = 2.0F * vertical_squared + 2.0F * horizontal_squared;
    if (denominator == 0.0F)
        return std::nullopt;
    const float velocity_squared = velocity * velocity;
    float discriminant = 2.0F * gravity * vertical * velocity_squared +
                         velocity_squared * velocity_squared - gravity_squared * horizontal_squared;
    if (discriminant < 0.0F)
        return std::nullopt;
    discriminant = std::sqrt(discriminant);

    const float base = gravity * vertical + velocity_squared;
    float first = (base - discriminant) * (horizontal_squared + vertical_squared);
    float second = (base + discriminant) * (horizontal_squared + vertical_squared);
    denominator = 1.414213F * horizontal / denominator;
    bool solved = false;
    float horizontal_velocity{};
    if (first >= 0.0F)
    {
        horizontal_velocity = std::sqrt(first) * denominator;
        solved = true;
    }
    if (second >= 0.0F)
    {
        const float candidate = std::sqrt(second) * denominator;
        if (!solved || candidate > horizontal_velocity)
        {
            horizontal_velocity = candidate;
            solved = true;
        }
    }
    if (!solved || horizontal_velocity <= 0.0F)
        return std::nullopt;
    const float horizontal_velocity_squared = horizontal_velocity * horizontal_velocity;
    if (horizontal_velocity_squared > velocity_squared)
        return std::nullopt;

    const float flight_time = horizontal / horizontal_velocity;
    float vertical_velocity =
        std::sqrt(std::max(0.0F, velocity_squared - horizontal_velocity_squared));
    const float positive_error = std::abs(
        vertical - (vertical_velocity * flight_time + 0.5F * gravity * flight_time * flight_time));
    const float negative_error = std::abs(
        vertical - (-vertical_velocity * flight_time + 0.5F * gravity * flight_time * flight_time));
    if (positive_error > negative_error)
        vertical_velocity = -vertical_velocity;

    const float direction_x = (target.x - origin.x) / horizontal;
    const float direction_z = (target.z - origin.z) / horizontal;
    return Vec3{horizontal_velocity * direction_x, vertical_velocity,
                horizontal_velocity * direction_z};
}

FireballSweepHit fireball_sweep(const WorldRegion &region, const PlayerState &player, Vec3 start,
                                Vec3 end) noexcept
{
    constexpr float radius = 0.57F;
    // AABBTraceProbe against Player.PlayerAABB expanded by the moving fireball box.
    const Vec3 player_minimum{player.position.x - 0.35F - radius, player.position.y - radius,
                              player.position.z - 0.35F - radius};
    const Vec3 player_maximum{player.position.x + 0.35F + radius, player.position.y + 1.7F + radius,
                              player.position.z + 0.35F + radius};
    if (const auto time = segment_box_time(start, end, player_minimum, player_maximum);
        time.has_value())
    {
        return {FireballSweepKind::player,
                {std::lerp(start.x, end.x, *time), std::lerp(start.y, end.y, *time),
                 std::lerp(start.z, end.z, *time)}};
    }

    const int minimum_x = static_cast<int>(std::floor(std::min(start.x, end.x) - radius));
    const int minimum_y = static_cast<int>(std::floor(std::min(start.y, end.y) - radius));
    const int minimum_z = static_cast<int>(std::floor(std::min(start.z, end.z) - radius));
    const int maximum_x = static_cast<int>(std::floor(std::max(start.x, end.x) + radius));
    const int maximum_y = static_cast<int>(std::floor(std::max(start.y, end.y) + radius));
    const int maximum_z = static_cast<int>(std::floor(std::max(start.z, end.z) + radius));
    std::optional<float> earliest;
    for (int z = minimum_z; z <= maximum_z; ++z)
        for (int y = minimum_y; y <= maximum_y; ++y)
            for (int x = minimum_x; x <= maximum_x; ++x)
            {
                if (!region.contains(x, y, z) || !blocks_player(region.block(x, y, z)))
                    continue;
                const auto time = segment_box_time(
                    start, end,
                    {static_cast<float>(x) - radius, static_cast<float>(y) - radius,
                     static_cast<float>(z) - radius},
                    {static_cast<float>(x + 1) + radius, static_cast<float>(y + 1) + radius,
                     static_cast<float>(z + 1) + radius});
                if (time.has_value() && (!earliest.has_value() || *time < *earliest))
                    earliest = *time;
            }
    if (!earliest.has_value())
        return {};
    return {FireballSweepKind::terrain,
            {std::lerp(start.x, end.x, *earliest), std::lerp(start.y, end.y, *earliest),
             std::lerp(start.z, end.z, *earliest)}};
}

ArrowSweepHit arrow_sweep(const WorldRegion &region, const PlayerState &player, bool player_valid,
                          Vec3 tail, Vec3 head) noexcept
{
    const Vec3 segment{head.x - tail.x, head.y - tail.y, head.z - tail.z};
    const float length =
        std::sqrt(segment.x * segment.x + segment.y * segment.y + segment.z * segment.z);
    if (length <= 0.000001F)
        return {};

    std::optional<float> player_time;
    if (player_valid)
    {
        player_time = segment_box_time(
            tail, head, {player.position.x - 0.35F, player.position.y, player.position.z - 0.35F},
            {player.position.x + 0.35F, player.position.y + 1.7F, player.position.z + 0.35F});
    }

    const Vec3 direction{segment.x / length, segment.y / length, segment.z / length};
    const auto terrain = PlayerSimulation::raycast_blocking(region, tail, direction, length);
    const std::optional<float> terrain_time =
        terrain.has_value() ? std::optional{terrain->distance / length} : std::nullopt;

    // Tracer.Update replaces a target hit only when terrain's _inT is strictly
    // smaller, so the target wins an exact tie.
    if (terrain_time.has_value() && (!player_time.has_value() || *terrain_time < *player_time))
    {
        return {ArrowSweepKind::terrain,
                {std::lerp(tail.x, head.x, *terrain_time), std::lerp(tail.y, head.y, *terrain_time),
                 std::lerp(tail.z, head.z, *terrain_time)}};
    }
    if (player_time.has_value())
    {
        return {ArrowSweepKind::player,
                {std::lerp(tail.x, head.x, *player_time), std::lerp(tail.y, head.y, *player_time),
                 std::lerp(tail.z, head.z, *player_time)}};
    }
    return {};
}

int find_enemy(float distance_step, float distance, int first, int last, float roll01) noexcept
{
    const int span = last - first;
    const float scaled = distance / distance_step;
    int band0 = static_cast<int>(std::floor(scaled));
    int band1 = band0 - 1;
    int band2 = band1 - 1;
    const float fraction = scaled - static_cast<float>(band0);
    float weight0;
    float weight1 = 0.0F;
    float weight2 = 0.0F;
    if (band0 > span)
    {
        band0 = span;
        band1 = span - 1;
        band2 = span - 2;
        weight0 = 1.0F;
        weight1 = 1.0F;
        weight2 = 0.5F;
    }
    else
    {
        weight0 = std::sin(fraction * (pi * 0.5F) / 3.0F);
        if (band1 >= 0)
            weight1 = std::sin((1.0F + fraction) * (pi * 0.5F) / 3.0F);
        if (band2 >= 0)
            weight2 = std::sin((2.0F + fraction) * (pi * 0.5F) / 3.0F);
    }
    const float pick = roll01 * (weight0 + weight1 + weight2);
    const int band = pick <= weight0 ? band0 : (pick > weight0 + weight1 ? band2 : band1);
    return std::clamp(band, 0, span) + first;
}

int pick_aboveground_enemy(float percent_midnight, float distance, bool felguard_ready,
                           bool felguard_spawned, EnemyRng &rng) noexcept
{
    // Archers own dusk and dawn: chance (1 - midnight)^4.
    const float archer_chance = std::pow(1.0F - percent_midnight, 4.0F);
    if (rng.next01() < archer_chance)
        return find_enemy(archer_distance_step, distance, 18, 25, rng.next01());
    // GetAbovegroundEnemy: the surface only rolls a Felguard AFTER the first
    // one was picked underground (EnemyManager.FelguardSpawned latch) — hell
    // demons never lead on the surface.
    if (felguard_spawned && felguard_ready && rng.next01() < felguard_probability)
        return enemy_felguard;
    return find_enemy(zombie_distance_step, distance, 0, 17, rng.next01());
}

int pick_belowground_enemy(float depth, float distance, bool felguard_ready, bool felguard_spawned,
                           EnemyRng &rng) noexcept
{
    const float adjusted = distance + depth * 2.0F * cave_distance_step / 50.0F;
    const float roll = rng.next01();
    if (felguard_ready && (roll < felguard_probability || !felguard_spawned))
    {
        // The original lets the search run one past the skeleton table into FELGUARD.
        const int result =
            find_enemy(cave_distance_step, adjusted, 26, enemy_felguard, rng.next01());
        return result;
    }
    return find_enemy(cave_distance_step, adjusted, 26, 49, rng.next01());
}

float enemy_midnight_factor(float percent_midnight, float distance, float player_y) noexcept
{
    if (player_y < -40.0F)
        return 0.85F;
    float near_origin = 1.0F - std::min(distance / 5000.0F, 1.0F);
    near_origin *= 0.5F;
    float value = std::max(0.0F, percent_midnight - near_origin);
    value /= 1.0F - near_origin;
    return std::clamp(value, 0.0F, 0.79F);
}

float minimum_enemy_spawn_seconds(float distance, bool hard_branch) noexcept
{
    // GetMinEnemySpawnTime: Endurance/Normal/Hardcore run 5s -> 1s; Easy runs
    // 45s -> 20s.  The gap is the difference between a quiet night and the
    // original's storm.
    //
    // It is `MathHelper.Lerp(5f, 1f, d / 3450f)`, and XNA's Lerp does not
    // clamp: past 3450 blocks the time keeps falling (zero at 4312 on the
    // hard branch, negative beyond), which is what makes the far lands -
    // and a long Endurance run, whose distance grows by 120 a day - swarm.
    // Clamping it held the hard branch at one second and the easy one at
    // twenty for good. (Executed: SemanticsParityProbe facts spawn.min_time.*.)
    const float amount = distance / 3450.0F;
    if (hard_branch)
    {
        return 5.0F + (1.0F - 5.0F) * amount;
    }
    return 45.0F + (20.0F - 45.0F) * amount;
}

EnemyLoot enemy_loot(int type, float distance_from_origin, float enemy_y, EnemyRng &rng) noexcept
{
    const EnemyStats &stats = enemy_stats(type);
    const float floor = std::clamp(distance_from_origin / 5000.0F, 0.0F, 1.0F);
    const float roll = floor + rng.next01() * (1.0F - floor);
    // BaseZombie.CreatePickup does pick Explosive Powder for a HELL enemy, but
    // the very next statement is `if (CRASHSITE) {...} else {...}` with no
    // `else` in front of it, so a hell enemy runs the ordinary branch below and
    // the powder is thrown away: nothing on a roll under 0.5, otherwise the
    // cave or surface table. Both shipped generations read this way; powder
    // comes from dragons (and, in 1.9.9.8, from its recipe).
    if (stats.habitat == EnemyHabitat::crash_site)
    {
        if (roll < 0.5F)
            return {21U, 1U}; // Copper bars
        if (roll >= 0.8F)
            return {19U, 1U}; // Diamond
        return {20U, 1U};     // Iron bars
    }
    if (roll < 0.5F)
        return {0U, 0U};
    if (enemy_y < -40.0F)
    {
        if (roll < 0.7F)
            return {16U, 1U}; // Copper ore
        if (roll < 0.8F)
            return {21U, 1U}; // Copper bars
        if (roll < 0.85F)
            return {20U, 1U}; // Iron bars
        if (roll >= 0.9F)
            return {19U, 1U}; // Diamond
        return {18U, 1U};     // Gold ore
    }
    if (roll < 0.7F)
        return {5U, 1U}; // Wood
    if (roll < 0.8F)
        return {15U, 1U}; // Coal
    if (roll < 0.85F)
        return {16U, 1U}; // Copper ore
    if (roll >= 0.9F)
        return {17U, 1U}; // Iron ore
    return {21U, 1U};     // Copper bars
}

std::vector<EnemyLoot> dragon_loot(Vec3 location, EnemyRng &rng)
{
    const int material_count = rng.next_int(1, 4) + rng.next_int(1, 5);
    const float location_length =
        std::sqrt(location.x * location.x + location.y * location.y + location.z * location.z);
    const float distance_floor = std::clamp(location_length / 5000.0F, 0.0F, 1.0F);
    const int powder_maximum_exclusive = 3 + static_cast<int>(distance_floor * 5.0F);
    const int powder_count =
        rng.next_int(1, powder_maximum_exclusive) + rng.next_int(1, powder_maximum_exclusive);
    std::vector<EnemyLoot> result;
    result.reserve(static_cast<std::size_t>(powder_count + material_count));
    for (int index{}; index < powder_count; ++index)
        result.push_back({108U, 1U}); // ExplosivePowder
    for (int index{}; index < material_count; ++index)
    {
        const float roll = distance_floor + rng.next01() * (1.0F - distance_floor);
        std::uint16_t item = 19U; // Diamond
        if (roll < 0.5F)
            item = 21U; // Copper bars
        else if (roll < 0.6F)
            item = 20U; // Iron bars
        else if (roll < 0.8F)
            item = 22U; // Gold bars
        else if (roll >= 0.9F)
            item = 7U; // BloodStoneBlock
        result.push_back({item, 1U});
    }
    return result;
}

const DragonStats &dragon_stats(DragonKind kind) noexcept
{
    // DragonType.Init: health / fireball velocity / fireball damage per type; the
    // shared tuning constants come from the DragonType constructor.
    static const std::array<DragonStats, 5> table{{
        {20.0F, 30.0F, 0.4F, 20.0F, 2.0F, 1.0F, 70.0F, 90.0F, 50.0F, 200.0F},
        {100.0F, 40.0F, 0.4F, 20.0F, 2.0F, 1.0F, 70.0F, 90.0F, 50.0F, 200.0F},
        {300.0F, 40.0F, 0.4F, 20.0F, 2.0F, 1.0F, 70.0F, 90.0F, 50.0F, 200.0F},
        {600.0F, 60.0F, 0.4F, 20.0F, 2.0F, 1.0F, 70.0F, 90.0F, 50.0F, 200.0F},
        {1000.0F, 60.0F, 0.4F, 20.0F, 2.0F, 1.0F, 70.0F, 90.0F, 50.0F, 200.0F},
    }};
    return table[std::min<std::size_t>(static_cast<std::size_t>(kind), table.size() - 1U)];
}

DragonBasis dragon_attitude_basis(float yaw, float pitch, float roll) noexcept
{
    // Microsoft.Xna.Framework.Quaternion.CreateFromYawPitchRoll followed by
    // Matrix.CreateFromQuaternion, in XNA's row-vector convention.
    const float half_yaw = yaw * 0.5F;
    const float half_pitch = pitch * 0.5F;
    const float half_roll = roll * 0.5F;
    const float sy = std::sin(half_yaw);
    const float cy = std::cos(half_yaw);
    const float sp = std::sin(half_pitch);
    const float cp = std::cos(half_pitch);
    const float sr = std::sin(half_roll);
    const float cr = std::cos(half_roll);
    const float qx = cy * sp * cr + sy * cp * sr;
    const float qy = sy * cp * cr - cy * sp * sr;
    const float qz = cy * cp * sr - sy * sp * cr;
    const float qw = cy * cp * cr + sy * sp * sr;
    const float xx = qx * qx * 2.0F;
    const float yy = qy * qy * 2.0F;
    const float zz = qz * qz * 2.0F;
    const float xy = qx * qy * 2.0F;
    const float xz = qx * qz * 2.0F;
    const float yz = qy * qz * 2.0F;
    const float wx = qw * qx * 2.0F;
    const float wy = qw * qy * 2.0F;
    const float wz = qw * qz * 2.0F;
    const Vec3 right{1.0F - yy - zz, xy + wz, xz - wy};
    const Vec3 up{xy - wz, 1.0F - xx - zz, yz + wx};
    const Vec3 backward{xz + wy, yz - wx, 1.0F - xx - yy};
    return {right, up, {-backward.x, -backward.y, -backward.z}};
}

bool dragon_blast_reaches(Vec3 impact, int x, int y, int z) noexcept
{
    const float dx = static_cast<float>(x) + 0.5F - impact.x;
    const float dy = static_cast<float>(y) + 0.5F - impact.y;
    const float dz = static_cast<float>(z) + 0.5F - impact.z;
    return dx * dx + dy * dy + dz * dz <= 25.0F;
}

Vec3 dragon_fireball_velocity(DragonKind kind, Vec3 origin, Vec3 target) noexcept
{
    const Vec3 delta{target.x - origin.x, target.y - origin.y, target.z - origin.z};
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (length < 0.000001F)
        return {};
    const float scale = dragon_stats(kind).fireball_velocity / length;
    return {delta.x * scale, delta.y * scale, delta.z * scale};
}

Vec3 dragon_muzzle_offset(std::uint8_t animation) noexcept
{
    // Mean "Bip01_Ponytail1" translation in DragonClientEntity space over a
    // 180-second original flight (identical in 1.6.3.8 and 1.9.9.8). The bone
    // rides the wing beat, so an individual shot may sit up to ~2 units away.
    switch (animation)
    {
    case 1U: // FLAP, "fly_forward"
        return {0.014F, 0.764F, -8.266F};
    case 3U: // HOVER, "Idle"
        return {-0.395F, 3.340F, -6.232F};
    default: // SOAR, "flying_idle" (and the ATTACK clip, which blends from it)
        return {0.021F, 0.453F, -6.833F};
    }
}

std::uint8_t dragon_animation_index(std::string_view clip) noexcept
{
    // DragonEntity.AnimNames: flying_idle, fly_forward, gethit, Idle.
    if (clip == "Idle")
        return 3U;
    if (clip == "attack")
        return 2U;
    if (clip == "fly_forward")
        return 1U;
    return 0U;
}

float dragon_move_toward_target(float current, float target, float rate,
                                float delta_seconds) noexcept
{
    if (target == current)
        return current;
    // The original is 32-bit .NET: a float expression is evaluated in the x87
    // FPU at double precision and rounded once, when it is stored. Doing the
    // same keeps the 0.01 snap (and everything downstream of it) on the frame
    // the original takes it.
    double step = static_cast<double>(target) - static_cast<double>(current);
    const double magnitude = std::abs(step);
    if (magnitude > static_cast<double>(rate))
        step *= static_cast<double>(rate) / magnitude;
    const double moved = static_cast<double>(current) + step * static_cast<double>(delta_seconds);
    return std::abs(moved - static_cast<double>(target)) < static_cast<double>(0.01F)
               ? target
               : static_cast<float>(moved);
}

std::optional<DragonRayHit> trace_dragon(const Dragon &dragon, Vec3 origin, Vec3 direction,
                                         float max_distance) noexcept
{
    if (max_distance <= 0.0F)
        return std::nullopt;

    // DragonClientEntity builds its oriented volumes from LocalToWorld.  Local
    // -Z is forward; roll rotates the right/up rows around that forward axis.
    const DragonBasis basis = dragon_attitude_basis(dragon.yaw, dragon.pitch, dragon.roll);
    const Vec3 backward{-basis.forward.x, -basis.forward.y, -basis.forward.z};
    const auto to_local = [&](Vec3 point) {
        const Vec3 offset{point.x - dragon.position.x, point.y - dragon.position.y,
                          point.z - dragon.position.z};
        return Vec3{offset.x * basis.right.x + offset.y * basis.right.y + offset.z * basis.right.z,
                    offset.x * basis.up.x + offset.y * basis.up.y + offset.z * basis.up.z,
                    offset.x * backward.x + offset.y * backward.y + offset.z * backward.z};
    };

    const Vec3 end{origin.x + direction.x * max_distance, origin.y + direction.y * max_distance,
                   origin.z + direction.z * max_distance};
    const Vec3 local_start = to_local(origin);
    const Vec3 local_end = to_local(end);
    const auto head_time =
        segment_box_time(local_start, local_end, {-1.5F, 0.0F, -9.625F}, {1.5F, 2.5F, -4.125F});
    const auto body_time =
        segment_box_time(local_start, local_end, {-1.75F, -1.05F, -4.2F}, {1.75F, 2.45F, 4.8F});
    if (!head_time.has_value() && !body_time.has_value())
        return std::nullopt;

    const bool head = head_time.has_value() && (!body_time.has_value() || *head_time <= *body_time);
    const float time = head ? *head_time : *body_time;
    const float distance = time * max_distance;
    const Vec3 local_impact{local_start.x + (local_end.x - local_start.x) * time,
                            local_start.y + (local_end.y - local_start.y) * time,
                            local_start.z + (local_end.z - local_start.z) * time};
    return DragonRayHit{distance,
                        {origin.x + direction.x * distance, origin.y + direction.y * distance,
                         origin.z + direction.z * distance},
                        head,
                        local_impact.z < -4.2F};
}

float dragon_schedule_minutes(DragonKind kind) noexcept
{
    switch (kind)
    {
    case DragonKind::fire:
        return 32.0F;
    case DragonKind::forest:
        return 48.0F;
    case DragonKind::sand:
        return 64.0F;
    case DragonKind::ice:
    case DragonKind::undead:
    default:
        return 80.0F;
    }
}

bool dragon_breaks_block(DragonKind kind, BlockType type) noexcept
{
    // The always-protected set from DragonType.Init.
    switch (type)
    {
    case BlockType::empty:
    case BlockType::uninitialized:
    case BlockType::fixed_lantern:
    case BlockType::bedrock:
    case BlockType::blood_stone:
    case BlockType::space_rock:
    case BlockType::space_rock_inventory:
    case BlockType::slime:
        return false;
    default:
        break;
    }
    // Weaker dragons protect additional families, strongest to weakest.
    const auto protects = [type](std::initializer_list<BlockType> list) {
        for (const BlockType entry : list)
            if (entry == type)
                return true;
        return false;
    };
    switch (kind)
    {
    case DragonKind::fire:
        return !protects({BlockType::rock, BlockType::lantern, BlockType::gold_ore,
                          BlockType::iron_ore, BlockType::copper_ore, BlockType::coal_ore,
                          BlockType::diamond_ore, BlockType::iron_wall, BlockType::copper_wall,
                          BlockType::golden_wall, BlockType::diamond_wall});
    case DragonKind::forest:
        return !protects({BlockType::lantern, BlockType::iron_wall, BlockType::copper_wall,
                          BlockType::golden_wall, BlockType::diamond_wall});
    case DragonKind::sand:
        return !protects({BlockType::lantern, BlockType::iron_wall, BlockType::golden_wall,
                          BlockType::diamond_wall});
    case DragonKind::ice:
        return !protects({BlockType::golden_wall, BlockType::diamond_wall});
    case DragonKind::undead:
    default:
        return type != BlockType::diamond_wall;
    }
}

float dragon_fireball_damage_multiplier(const WorldRegion &region, DragonKind kind, Vec3 impact,
                                        Vec3 target) noexcept
{
    const Vec3 delta{target.x - impact.x, target.y - impact.y, target.z - impact.z};
    const float total_distance =
        std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (total_distance <= 0.000001F)
        return 1.0F;
    const int minimum_x = static_cast<int>(std::floor(std::min(impact.x, target.x)));
    const int maximum_x = static_cast<int>(std::floor(std::max(impact.x, target.x)));
    const int minimum_y = static_cast<int>(std::floor(std::min(impact.y, target.y)));
    const int maximum_y = static_cast<int>(std::floor(std::max(impact.y, target.y)));
    const int minimum_z = static_cast<int>(std::floor(std::min(impact.z, target.z)));
    const int maximum_z = static_cast<int>(std::floor(std::max(impact.z, target.z)));
    float multiplier = 1.0F;
    for (int z = minimum_z; z <= maximum_z; ++z)
        for (int y = minimum_y; y <= maximum_y; ++y)
            for (int x = minimum_x; x <= maximum_x; ++x)
            {
                if (!region.contains(x, y, z))
                    continue;
                const BlockType block = region.block(x, y, z);
                if (!blocks_player(block))
                    continue;
                const auto interval = segment_box_interval(
                    impact, target,
                    {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
                    {static_cast<float>(x + 1), static_cast<float>(y + 1),
                     static_cast<float>(z + 1)});
                if (!interval.has_value())
                    continue;
                const float path_length = ((*interval)[1] - (*interval)[0]) * total_distance;
                if (path_length <= 0.0F)
                    continue;
                // BreakLookup is the original protected-block table despite
                // its name: protected material zeroes LOS damage immediately.
                if (!dragon_breaks_block(kind, block))
                    return 0.0F;
                const float absorption = 1.0F - damage_transmission(block);
                multiplier *= std::clamp(1.0F - absorption * path_length, 0.0F, 1.0F);
                if (multiplier <= 0.0F)
                    return 0.0F;
            }
    return multiplier;
}

void DragonFlight::spawn(DragonKind kind, Vec3 player_position, EnemyRng &rng)
{
    const DragonStats &stats = dragon_stats(kind);
    dragon_ = Dragon{};
    dragon_.kind = kind;
    dragon_.health = stats.starting_health;
    // DragonEntity.InitSpawnState: 750 m out at cruising altitude 120 — the
    // long visible approach is the original's dragon telegraph.
    // InitSpawnState chooses the forward half-plane, then places the root 750
    // metres behind that forward vector. The initial heading therefore points
    // directly into the long approach instead of crossing it sideways.
    const float angle = std::lerp(-pi * 0.5F, pi * 0.5F, rng.next01());
    dragon_.position = {player_position.x + std::sin(angle) * 750.0F, 120.0F,
                        player_position.z + std::cos(angle) * 750.0F};
    dragon_.yaw = angle;
    dragon_.velocity = stats.speed;
    dragon_.state = DragonState::approach;
    dragon_.clip = "fly_forward";
    dragon_.loiter_seconds = 2.0F + rng.next01() * 4.0F;
    engagement_seconds_ = 0.0F;
    lost_target_seconds_ = 0.0F;
    network_waypoints_.clear();
    network_interpolation_time_ = 0.0F;
    network_replica_ = false;
    network_replica_ready_ = true;
    network_waiting_to_shoot_ = false;
    network_events_.clear();
    network_owner_ = false;
    network_timeout_ = 0.0F;
    owner_messages_.clear();
    owner_echo_.clear();
    // InitSpawnState: Yaw = DefaultHeading = the random approach heading.
    default_heading_ = angle;
    brain_target_yaw_ = angle;
    brain_target_pitch_ = 0.0F;
    brain_target_roll_ = 0.0F;
    brain_target_velocity_ = stats.speed;
    active_ = true;
}

void DragonFlight::install_replica(DragonKind kind, float health) noexcept
{
    dragon_ = Dragon{};
    dragon_.kind = kind;
    dragon_.health = health >= 0.0F ? health : dragon_stats(kind).starting_health;
    dragon_.state = DragonState::approach;
    dragon_.clip = "fly_forward";
    engagement_seconds_ = 0.0F;
    lost_target_seconds_ = 0.0F;
    network_waypoints_.clear();
    network_interpolation_time_ = 0.0F;
    network_replica_ = true;
    network_replica_ready_ = false;
    network_waiting_to_shoot_ = false;
    network_events_.clear();
    network_owner_ = false;
    network_timeout_ = 0.0F;
    owner_messages_.clear();
    owner_echo_.clear();
    active_ = true;
}

void DragonFlight::begin_network_ownership(std::uint8_t local_gid,
                                           std::uint32_t next_fireball_index) noexcept
{
    if (!active_)
        return;
    // `new DragonEntity(type, forBiome, null)` beside `new DragonClientEntity`.
    const DragonKind kind = dragon_.kind;
    const float health = dragon_.health;
    const Vec3 spawned = dragon_.position;
    retail_ = std::make_shared<DragonBrainState>();
    retail_->next_fireball_index = next_fireball_index;
    // spawn() already drew InitSpawnState's heading and placed the dragon.
    dragon_brain_spawn(*retail_, kind,
                       {spawned.x - std::sin(default_heading_) * 750.0F, 0.0F,
                        spawned.z - std::cos(default_heading_) * 750.0F},
                       default_heading_);
    retail_->position = spawned;
    dragon_ = Dragon{};
    dragon_.kind = kind;
    dragon_.health = health;
    dragon_.clip = "fly_forward";
    network_waypoints_.clear();
    network_events_.clear();
    network_interpolation_time_ = 0.0F;
    network_replica_ = false;
    network_replica_ready_ = false;
    network_waiting_to_shoot_ = false;
    network_timeout_ = 0.0F;
    network_owner_ = true;
    owner_gid_ = local_gid;
    owner_messages_.clear();
    owner_echo_.clear();
    sync_brain_view();
}

void DragonFlight::adopt_network_ownership(std::uint8_t local_gid,
                                           const DragonMigrationState &migration,
                                           DragonBrainSession &session)
{
    if (!active_)
        return;
    // `new DragonEntity(type, forBiome, miginfo)`: InitAfterHostChange. The
    // replica this peer is already presenting is not touched.
    if (retail_ == nullptr)
        retail_ = std::make_shared<DragonBrainState>();
    dragon_brain_adopt(*retail_, dragon_.kind, migration, session);
    network_owner_ = true;
    owner_gid_ = local_gid;
    owner_messages_.clear();
    owner_echo_.clear();
    sync_brain_view();
}

void DragonFlight::end_network_ownership() noexcept
{
    network_owner_ = false;
    // What remains is an ordinary DragonClientEntity.
    network_replica_ = active_;
    owner_messages_.clear();
    owner_echo_.clear();
}

DragonMigrationState DragonFlight::migration_state() const noexcept
{
    return retail_ != nullptr ? dragon_brain_migration(*retail_) : DragonMigrationState{};
}

float DragonFlight::owner_time() const noexcept
{
    return retail_ != nullptr ? retail_->dragon_time : 0.0F;
}

std::uint32_t DragonFlight::next_fireball_index() const noexcept
{
    return retail_ != nullptr ? retail_->next_fireball_index : 0U;
}

DragonBrainState &DragonFlight::brain_state() noexcept
{
    if (retail_ == nullptr)
        retail_ = std::make_shared<DragonBrainState>();
    return *retail_;
}

void DragonFlight::register_gunshot(Vec3 position)
{
    if (network_owner_ && retail_ != nullptr)
        retail_->gunshots.push_back(position);
}

void DragonFlight::note_hit() noexcept
{
    if (network_owner_ && retail_ != nullptr)
        dragon_brain_hit(*retail_);
}

// The legacy Dragon view of the retail brain, for brain() readers.
void DragonFlight::sync_brain_view() noexcept
{
    if (retail_ == nullptr)
        return;
    brain_.kind = retail_->kind;
    brain_.position = retail_->position;
    brain_.yaw = retail_->yaw;
    brain_.pitch = retail_->pitch;
    brain_.roll = retail_->roll;
    brain_.velocity = retail_->velocity;
    static constexpr std::string_view clips[4]{"flying_idle", "fly_forward", "attack", "Idle"};
    brain_.clip = clips[retail_->current_animation & 3U];
    switch (retail_->mode)
    {
    case DragonBrainMode::default_state:
        brain_.state = DragonState::approach;
        break;
    case DragonBrainMode::strafe_attack:
        brain_.state = DragonState::strafe;
        break;
    case DragonBrainMode::hover:
    case DragonBrainMode::hover_attack:
        brain_.state = DragonState::hover;
        break;
    default:
        brain_.state = DragonState::loiter;
        break;
    }
}

DragonEvents DragonFlight::update_owner(DragonBrainSession &session, float delta_seconds)
{
    DragonEvents events;
    if (!active_ || !network_owner_ || retail_ == nullptr || dragon_.state == DragonState::dying)
        return events;
    // Message.Echo: what the brain sent last frame reaches this peer's own
    // DragonClientEntity on this frame's message pump.
    for (const DragonOwnerMessage &echo : owner_echo_)
        static_cast<void>(add_network_waypoint(
            {echo.position, echo.velocity, echo.host_time, echo.target_roll, echo.animation,
             echo.sound, static_cast<std::uint8_t>(echo.attack ? 2U : 0U), echo.target,
             echo.fireball_index}));
    owner_echo_.clear();
    retail_->modern_content = modern_content_;
    DragonBrainOutput output;
    dragon_brain_update(*retail_, session, delta_seconds, owner_gid_, output);
    sync_brain_view();
    events.remove_requested = output.remove_requests > 0;
    events.migrate_to = output.migrate_to;
    for (const DragonOwnerMessage &message : output.messages)
    {
        owner_messages_.push_back(message);
        owner_echo_.push_back(message);
    }
    extrapolate_network_replica(delta_seconds);
    return events;
}

std::vector<DragonOwnerMessage> DragonFlight::take_owner_messages() noexcept
{
    std::vector<DragonOwnerMessage> messages;
    messages.swap(owner_messages_);
    return messages;
}

void DragonFlight::apply_network_waypoint(Vec3 position, Vec3 velocity, float host_time,
                                          float target_roll, std::uint8_t animation,
                                          std::uint8_t sound) noexcept
{
    // An owner's replica is fed only by its own echo (update()); the same
    // waypoint coming back through the session would be a duplicate.
    if (network_owner_)
        return;
    static_cast<void>(
        add_network_waypoint({position, velocity, host_time, target_roll, animation, sound}));
}

bool DragonFlight::apply_network_attack_waypoint(Vec3 position, Vec3 velocity, float host_time,
                                                 float target_roll, std::uint8_t animation,
                                                 std::uint8_t sound, Vec3 target,
                                                 std::int32_t fireball_id, bool animated) noexcept
{
    if (network_owner_)
        return false;
    return add_network_waypoint({position, velocity, host_time, target_roll, animation, sound,
                                 static_cast<std::uint8_t>(animated ? 1U : 2U), target,
                                 fireball_id});
}

bool DragonFlight::add_network_waypoint(NetworkWaypoint incoming) noexcept
{
    constexpr std::size_t maximum_buffered_waypoints = 128U;
    if (!active_ || !presents_replica())
        return false;
    // AddActionWaypoint zeroes TimeoutTimer before it decides anything else,
    // so even a sample it then discards keeps the dragon alive.
    network_timeout_ = 0.0F;
    if (network_waypoints_.size() >= maximum_buffered_waypoints)
        return false;
    for (std::size_t index{}; index < network_waypoints_.size(); ++index)
    {
        if (network_waypoints_[index].host_time <= incoming.host_time)
            continue;
        // AddActionWaypoint never rewinds either of the two samples currently
        // bracketing the interpolation cursor. Older unreliable updates are
        // therefore harmless, while a slightly reordered future sample can
        // still be inserted.
        if (index < 2U)
            return false;
        network_waypoints_.insert(network_waypoints_.begin() + static_cast<std::ptrdiff_t>(index),
                                  incoming);
        return true;
    }
    if (!network_waypoints_.empty() &&
        network_interpolation_time_ > network_waypoints_.back().host_time)
    {
        network_waypoints_.clear();
        const Vec3 forward =
            dragon_attitude_basis(dragon_.yaw, dragon_.pitch, dragon_.roll).forward;
        // The bridging waypoint is a default struct apart from position,
        // CurrentVelocity, time and CurrentAnimation: its TargetRoll is zero.
        network_waypoints_.push_back({dragon_.position,
                                      {forward.x * dragon_.velocity, forward.y * dragon_.velocity,
                                       forward.z * dragon_.velocity},
                                      network_interpolation_time_,
                                      0.0F,
                                      dragon_animation_index(dragon_.clip)});
    }
    network_waypoints_.push_back(incoming);
    return true;
}

void DragonFlight::emit_network_fireball(Vec3 target, std::int32_t fireball_id) noexcept
{
    // ShootFireball reads the ponytail bone out of the pose the replica held
    // BEFORE this frame's move, which is exactly what dragon_ still is here.
    const DragonBasis basis = dragon_attitude_basis(dragon_.yaw, dragon_.pitch, dragon_.roll);
    const Vec3 muzzle = dragon_muzzle_offset(dragon_animation_index(dragon_.clip));
    const Vec3 origin{
        dragon_.position.x + basis.right.x * muzzle.x + basis.up.x * muzzle.y -
            basis.forward.x * muzzle.z,
        dragon_.position.y + basis.right.y * muzzle.x + basis.up.y * muzzle.y -
            basis.forward.y * muzzle.z,
        dragon_.position.z + basis.right.z * muzzle.x + basis.up.z * muzzle.y -
            basis.forward.z * muzzle.z};
    network_events_.push_back({DragonReplicaEventKind::fireball, origin, target, fireball_id});
}

void DragonFlight::process_network_waypoint(const NetworkWaypoint &waypoint) noexcept
{
    if (waypoint.action == 2U) // QUICKSHOOT
        emit_network_fireball(waypoint.action_position, waypoint.fireball_id);
    else if (waypoint.action == 1U) // ANIMSHOOT
    {
        if (network_waiting_to_shoot_)
            network_events_.push_back(
                {DragonReplicaEventKind::cancel_fireball, {}, {}, network_shoot_fireball_id_});
        network_shoot_target_ = waypoint.action_position;
        network_shoot_fireball_id_ = waypoint.fireball_id;
        network_waiting_to_shoot_ = true;
        dragon_.clip = "attack";
        dragon_.clip_time = 0.0F;
    }
    if (waypoint.sound == 1U) // CRY
        network_events_.push_back({DragonReplicaEventKind::cry, dragon_.position, {}, 0});
}

std::vector<DragonReplicaEvent> DragonFlight::take_network_replica_events() noexcept
{
    std::vector<DragonReplicaEvent> events;
    events.swap(network_events_);
    return events;
}

void DragonFlight::reset_network_replica(Vec3 position, Vec3 velocity, float host_time,
                                         float target_roll, std::uint8_t animation) noexcept
{
    if (!active_)
        return;
    network_replica_ = true;
    network_replica_ready_ = true;
    network_interpolation_time_ = host_time;
    network_waypoints_.clear();
    network_events_.clear();
    network_waiting_to_shoot_ = false;
    network_waypoints_.push_back({position, velocity, host_time, target_roll, animation});
    apply_network_replica_sample(position, velocity, target_roll, animation, 0.0F);
}

void DragonFlight::promote_network_replica_to_brain() noexcept
{
    if (!active_)
        return;
    network_replica_ = false;
    network_replica_ready_ = true;
    network_waypoints_.clear();
    network_events_.clear();
    network_waiting_to_shoot_ = false;
    network_interpolation_time_ = 0.0F;
}

void DragonFlight::apply_network_replica_sample(Vec3 position, Vec3 velocity, float target_roll,
                                                std::uint8_t animation,
                                                float delta_seconds) noexcept
{
    Dragon &dragon = dragon_;
    dragon.position = position;
    const float horizontal = std::hypot(velocity.x, velocity.z);
    dragon.velocity =
        std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
    if (dragon.velocity > 0.001F)
    {
        dragon.yaw = std::atan2(-velocity.x, -velocity.z);
        dragon.pitch = std::atan2(velocity.y, std::max(0.001F, horizontal));
    }
    // Roll = MathTools.MoveTowardTarget(Roll, TargetRoll, EType.RollRate, dt).
    dragon.roll =
        dragon_move_toward_target(dragon.roll, target_roll, 1.0F, std::max(0.0F, delta_seconds));
    if (!network_waiting_to_shoot_)
    {
        const char *next_clip{};
        switch (animation)
        {
        case 0U: // SOAR
            next_clip = "flying_idle";
            break;
        case 1U: // FLAP
            next_clip = "fly_forward";
            break;
        case 2U: // ATTACK
            next_clip = "attack";
            break;
        case 3U: // HOVER
            next_clip = "Idle";
            break;
        default:
            return;
        }
        if (dragon.clip != next_clip)
        {
            dragon.clip = next_clip;
            dragon.clip_time = 0.0F;
        }
    }
}

void DragonFlight::extrapolate_network_replica(float delta_seconds) noexcept
{
    if (!active_ || !presents_replica() || delta_seconds < 0.0F)
        return;
    // DragonClientEntity.Update: the watchdog runs before anything else, even
    // while the replica is still waiting for its first three waypoints.
    network_timeout_ += delta_seconds;
    if (network_timeout_ > 10.0F)
    {
        network_timeout_ = -3600.0F;
        network_events_.push_back({DragonReplicaEventKind::timeout, dragon_.position, {}, 0});
    }
    if (!network_replica_ready_)
    {
        if (network_waypoints_.size() < 3U)
            return;
        network_replica_ready_ = true;
        dragon_.clip = "flying_idle";
        network_interpolation_time_ = network_waypoints_.front().host_time;
    }
    else if (!network_waypoints_.empty())
    {
        const float rate =
            network_waypoints_.back().host_time - network_interpolation_time_ < 0.1F ? 0.8F : 1.0F;
        network_interpolation_time_ += delta_seconds * rate;
    }
    while (network_waypoints_.size() > 1U &&
           network_interpolation_time_ >= network_waypoints_[1].host_time)
    {
        network_waypoints_.erase(network_waypoints_.begin());
        process_network_waypoint(network_waypoints_.front());
    }
    if (network_waypoints_.empty())
        return;

    const NetworkWaypoint &first = network_waypoints_[0];
    Vec3 position{};
    Vec3 velocity{};
    if (network_waypoints_.size() < 2U || network_interpolation_time_ <= first.host_time)
    {
        const float elapsed = network_interpolation_time_ - first.host_time;
        position = {first.position.x + first.velocity.x * elapsed,
                    first.position.y + first.velocity.y * elapsed,
                    first.position.z + first.velocity.z * elapsed};
        velocity = first.velocity;
    }
    else
    {
        const NetworkWaypoint &second = network_waypoints_[1];
        const float duration = second.host_time - first.host_time;
        if (duration <= 0.0F || network_interpolation_time_ >= second.host_time)
        {
            const float elapsed = network_interpolation_time_ - second.host_time;
            position = {second.position.x + second.velocity.x * elapsed,
                        second.position.y + second.velocity.y * elapsed,
                        second.position.z + second.velocity.z * elapsed};
            velocity = second.velocity;
        }
        else
        {
            const float amount = (network_interpolation_time_ - first.host_time) / duration;
            const float amount2 = amount * amount;
            const float amount3 = amount2 * amount;
            const float position1_weight = 2.0F * amount3 - 3.0F * amount2 + 1.0F;
            const float tangent1_weight = amount3 - 2.0F * amount2 + amount;
            const float position2_weight = -2.0F * amount3 + 3.0F * amount2;
            const float tangent2_weight = amount3 - amount2;
            const float derivative1_weight = 6.0F * amount2 - 6.0F * amount;
            const float derivative_tangent1_weight = 3.0F * amount2 - 4.0F * amount + 1.0F;
            const float derivative2_weight = -6.0F * amount2 + 6.0F * amount;
            const float derivative_tangent2_weight = 3.0F * amount2 - 2.0F * amount;
            const auto interpolate_component = [&](float first_position, float first_velocity,
                                                   float second_position, float second_velocity) {
                return first_position * position1_weight +
                       first_velocity * duration * tangent1_weight +
                       second_position * position2_weight +
                       second_velocity * duration * tangent2_weight;
            };
            const auto derivative_component = [&](float first_position, float first_velocity,
                                                  float second_position, float second_velocity) {
                return (first_position * derivative1_weight +
                        first_velocity * duration * derivative_tangent1_weight +
                        second_position * derivative2_weight +
                        second_velocity * duration * derivative_tangent2_weight) /
                       duration;
            };
            position = {interpolate_component(first.position.x, first.velocity.x, second.position.x,
                                              second.velocity.x),
                        interpolate_component(first.position.y, first.velocity.y, second.position.y,
                                              second.velocity.y),
                        interpolate_component(first.position.z, first.velocity.z, second.position.z,
                                              second.velocity.z)};
            velocity = {derivative_component(first.position.x, first.velocity.x, second.position.x,
                                             second.velocity.x),
                        derivative_component(first.position.y, first.velocity.y, second.position.y,
                                             second.velocity.y),
                        derivative_component(first.position.z, first.velocity.z, second.position.z,
                                             second.velocity.z)};
        }
    }
    apply_network_replica_sample(position, velocity, first.target_roll, first.animation,
                                 delta_seconds);
    dragon_.clip_time += delta_seconds;
    if (network_waiting_to_shoot_ && dragon_.clip == "attack" &&
        dragon_.clip_time > 1.1333333333333333F)
    {
        emit_network_fireball(network_shoot_target_, network_shoot_fireball_id_);
        network_waiting_to_shoot_ = false;
    }
}

void DragonFlight::damage(float amount)
{
    if (!active_ || dragon_.state == DragonState::dying)
        return;
    dragon_.health -= amount;
    if (dragon_.health <= 0.0F)
    {
        // DragonClientEntity.Kill: the body keeps CurrentVelocity, levels out
        // on its heading, and starts death_air_1.
        const Vec3 forward = dragon_attitude_basis(dragon_.yaw, dragon_.pitch, 0.0F).forward;
        death_velocity_ = {forward.x * dragon_.velocity, forward.y * dragon_.velocity,
                           forward.z * dragon_.velocity};
        dragon_.pitch = 0.0F;
        dragon_.roll = 0.0F;
        death_phase_ = 0U;
        death_reaction_time_ = 0.0F;
        death_clip_time_ = 0.0F;
        death_clip_playing_ = true;
        death_timer_ = 0.0F;
        dragon_.state = DragonState::dying;
        dragon_.state_time = 0.0F;
        dragon_.clip = "death_air_1";
        dragon_.clip_time = 0.0F;
    }
}

namespace
{

struct DeadBodySweep final
{
    bool hit{};
    float time{1.0F};
    Vec3 normal{};
};

// AABBTraceProbe with DragonClientEntity.DeadBodyBox, SkipEmbedded, against
// DragonTraceProbe.TestThisType blocks: whatever blocks a player, except logs.
[[nodiscard]] DeadBodySweep sweep_dead_body(const WorldRegion &region, Vec3 start, Vec3 end) noexcept
{
    constexpr Vec3 box_minimum{-2.5F, 0.0F, -2.5F};
    constexpr Vec3 box_maximum{2.5F, 10.0F, 2.5F};
    DeadBodySweep result;
    const std::array<float, 3> origin{start.x, start.y, start.z};
    const std::array<float, 3> delta{end.x - start.x, end.y - start.y, end.z - start.z};
    const std::array<float, 3> low{box_minimum.x, box_minimum.y, box_minimum.z};
    const std::array<float, 3> high{box_maximum.x, box_maximum.y, box_maximum.z};
    std::array<int, 3> first{};
    std::array<int, 3> last{};
    for (std::size_t axis{}; axis < 3U; ++axis)
    {
        first[axis] = static_cast<int>(
            std::floor(std::min(origin[axis], origin[axis] + delta[axis]) + low[axis]));
        last[axis] = static_cast<int>(
            std::floor(std::max(origin[axis], origin[axis] + delta[axis]) + high[axis]));
    }
    for (int z = first[2]; z <= last[2]; ++z)
        for (int y = first[1]; y <= last[1]; ++y)
            for (int x = first[0]; x <= last[0]; ++x)
            {
                if (!region.contains(x, y, z))
                    continue;
                const BlockType block = region.block(x, y, z);
                if (block == BlockType::log || !blocks_player(block))
                    continue;
                // The voxel grown by the body box turns the sweep into a segment test.
                const std::array<int, 3> cell{x, y, z};
                float enter = -std::numeric_limits<float>::infinity();
                float exit = std::numeric_limits<float>::infinity();
                std::size_t enter_axis{};
                bool miss{};
                for (std::size_t axis{}; axis < 3U && !miss; ++axis)
                {
                    const float minimum = static_cast<float>(cell[axis]) - high[axis];
                    const float maximum = static_cast<float>(cell[axis]) + 1.0F - low[axis];
                    if (std::abs(delta[axis]) < 1.0e-9F)
                    {
                        miss = origin[axis] <= minimum || origin[axis] >= maximum;
                        continue;
                    }
                    float near_time = (minimum - origin[axis]) / delta[axis];
                    float far_time = (maximum - origin[axis]) / delta[axis];
                    if (near_time > far_time)
                        std::swap(near_time, far_time);
                    if (near_time > enter)
                    {
                        enter = near_time;
                        enter_axis = axis;
                    }
                    exit = std::min(exit, far_time);
                }
                // enter < 0: the body already overlaps this block (SkipEmbedded).
                if (miss || enter > exit || enter < 0.0F || enter > 1.0F || enter >= result.time)
                    continue;
                result.hit = true;
                result.time = enter;
                result.normal = {};
                const float sign = delta[enter_axis] > 0.0F ? -1.0F : 1.0F;
                (enter_axis == 0U   ? result.normal.x
                 : enter_axis == 1U ? result.normal.y
                                    : result.normal.z) = sign;
            }
    return result;
}

} // namespace

// DragonClientEntity.MoveDragonWithCollision.
bool DragonFlight::move_dead_dragon(const WorldRegion *terrain, float delta_seconds) noexcept
{
    // DeathAnimationPackages[0].BoxOffset (0, 0, 7) through the yaw-only
    // LocalToWorld Kill left behind: seven units behind the body.
    const Vec3 offset{std::sin(dragon_.yaw) * 7.0F, 0.0F, std::cos(dragon_.yaw) * 7.0F};
    float remaining = delta_seconds;
    Vec3 position{dragon_.position.x + offset.x, dragon_.position.y + offset.y,
                  dragon_.position.z + offset.z};
    Vec3 velocity = death_velocity_;
    bool on_ground{};
    bool collided{};
    int iterations{};
    do
    {
        const Vec3 start = position;
        const Vec3 step{velocity.x * remaining, velocity.y * remaining, velocity.z * remaining};
        position = {start.x + step.x, start.y + step.y, start.z + step.z};
        collided = false;
        if (terrain != nullptr)
        {
            const DeadBodySweep sweep = sweep_dead_body(*terrain, start, position);
            if (sweep.hit)
            {
                collided = true;
                on_ground = on_ground || sweep.normal.y > 0.5F;
                const float travelled = std::max(sweep.time - 0.001F, 0.0F);
                position = {start.x + step.x * travelled, start.y + step.y * travelled,
                            start.z + step.z * travelled};
                const float into = sweep.normal.x * velocity.x + sweep.normal.y * velocity.y +
                                   sweep.normal.z * velocity.z;
                velocity = {velocity.x - sweep.normal.x * into, velocity.y - sweep.normal.y * into,
                            velocity.z - sweep.normal.z * into};
                remaining *= 1.0F - travelled;
                if (remaining <= 1.0e-7F)
                    break;
                const float speed_squared =
                    velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
                const float along = death_velocity_.x * velocity.x +
                                    death_velocity_.y * velocity.y + death_velocity_.z * velocity.z;
                if (speed_squared <= 1.0e-6F || along <= 1.0e-6F)
                {
                    velocity = {};
                    break;
                }
            }
        }
        ++iterations;
    } while (collided && iterations < 4);
    if (iterations == 4)
        velocity = {};
    position = {position.x - offset.x, position.y - offset.y, position.z - offset.z};
    if (position.y < -64.0F)
    {
        position.y = -64.0F;
        velocity.y = 0.0F;
        on_ground = true;
    }
    dragon_.position = position;
    death_velocity_ = velocity;
    return on_ground;
}

DragonTargetChoice dragon_find_best_target(Vec3 dragon_position, Vec3 forward,
                                           std::span<const DragonTargetCandidate> players) noexcept
{
    constexpr float maximum_view_distance = 300.0F;
    constexpr float keep_alive_distance = 750.0F * 1.25F;
    DragonTargetChoice choice;
    choice.remove_dragon = true;
    float best = std::numeric_limits<float>::max();
    const Vec3 eye{dragon_position.x + forward.x * 7.0F, dragon_position.y + forward.y * 7.0F,
                   dragon_position.z + forward.z * 7.0F};
    for (const DragonTargetCandidate &player : players)
    {
        if (!player.alive)
            continue;
        const Vec3 toward{player.position.x - eye.x, player.position.y + 1.5F - eye.y,
                          player.position.z - eye.z};
        const float distance_squared =
            toward.x * toward.x + toward.y * toward.y + toward.z * toward.z;
        if (distance_squared < maximum_view_distance * maximum_view_distance &&
            distance_squared > 0.001F)
        {
            choice.remove_dragon = false;
            if (!player.region_loaded || !(player.sunlight > 0.0F))
                continue;
            const float score = distance_squared / (player.sunlight * player.sunlight);
            if (!(score < best))
                continue;
            const float distance = std::sqrt(distance_squared);
            const float facing =
                (toward.x * forward.x + toward.y * forward.y + toward.z * forward.z) / distance;
            if (facing > 0.17F && player.line_of_sight)
            {
                choice.gid = player.gid;
                best = score;
            }
        }
        else if (distance_squared < keep_alive_distance * keep_alive_distance)
            choice.remove_dragon = false;
    }
    return choice;
}

bool dragon_target_requires_migration(Vec3 target_position, Vec3 owner_position) noexcept
{
    const float dx = target_position.x - owner_position.x;
    const float dy = target_position.y - owner_position.y;
    const float dz = target_position.z - owner_position.z;
    return dx * dx + dy * dy + dz * dz > 22'500.0F;
}

std::optional<Vec3> cavern_dragon_approach(const WorldRegion &region, Vec3 player, float yaw)
{
    // Admit only a visible, loaded pocket large enough for the whole dragon.
    // The encounter remains due in a narrow tunnel until the player reaches
    // an arena. This is deliberately independent of sunlight in Hell.
    for (const float height : {18.0F, 12.0F, 24.0F})
        for (const float radius : {60.0F, 40.0F, 80.0F})
            for (int bearing = 0; bearing < 8; ++bearing)
            {
                const float angle = yaw + static_cast<float>(bearing) * pi * 0.25F;
                const Vec3 point{player.x + std::sin(angle) * radius, player.y + height,
                                  player.z - std::cos(angle) * radius};
                bool clear = true;
                for (int z = -8; z <= 8 && clear; ++z)
                    for (int x = -8; x <= 8 && clear; ++x)
                        for (int y = -4; y <= 4; ++y)
                        {
                            const int bx = static_cast<int>(std::floor(point.x)) + x;
                            const int by = static_cast<int>(std::floor(point.y)) + y;
                            const int bz = static_cast<int>(std::floor(point.z)) + z;
                            if (!region.contains(bx, by, bz) ||
                                region.block(bx, by, bz) == BlockType::uninitialized ||
                                blocks_player(region.block(bx, by, bz)))
                            {
                                clear = false;
                                break;
                            }
                        }
                if (!clear)
                    continue;
                Vec3 toward{player.x - point.x, player.y + 1.5F - point.y, player.z - point.z};
                const float length = std::sqrt(toward.x*toward.x + toward.y*toward.y + toward.z*toward.z);
                toward = {toward.x / length, toward.y / length, toward.z / length};
                if (!PlayerSimulation::raycast_blocking(region, point, toward, length))
                    return point;
            }
    return std::nullopt;
}

void DragonSchedule::recalculate(Vec3 player, EnemyRng &rng) noexcept
{
    center_x_ = player.x;
    center_z_ = player.z;
    // RecalculateDragonBox: 32/48/64/80/80/80 minutes by NextTimedDragonType,
    // plus 16 * RandomFloat(-0.25, 0.25).
    const float minutes =
        dragon_schedule_minutes(static_cast<DragonKind>(std::clamp(next_type_, 0, 4)));
    next_spawn_seconds_ = (minutes + 16.0F * (rng.next01() * 0.5F - 0.25F)) * 60.0F;
}

std::optional<DragonKind> DragonSchedule::check_box(Vec3 player, float delta_seconds,
                                                    bool dragon_present, EnemyRng &rng) noexcept
{
    allowed_delay_ = std::max(0.0F, allowed_delay_ - delta_seconds);
    next_spawn_seconds_ -= delta_seconds;
    // DragonBox is +-500 in X and Z (and +-100 in Y, which a player cannot leave).
    const bool outside = std::abs(player.x - center_x_) > 500.0F ||
                         std::abs(player.z - center_z_) > 500.0F || std::abs(player.y) > 100.0F;
    if (initialize_ || outside)
    {
        next_type_ = 0;
        recalculate(player, rng);
        if (initialize_)
            allowed_delay_ = 5.0F;
        initialize_ = false;
        return std::nullopt;
    }
    if (!(next_spawn_seconds_ < 0.0F))
        return std::nullopt;
    // `NextSpawnDragonTime += 1 minute`: it asks again every minute.
    next_spawn_seconds_ += 60.0F;
    const bool far_out = player.x * player.x + player.z * player.z > 12'960'000.0F;
    if (!(next_type_ < 5 || far_out))
        return std::nullopt;
    const int asked = next_type_;
    next_type_ = std::min(next_type_ + 1, 5);
    // AskForDragon.
    if (allowed_delay_ > 0.0F || dragon_present)
        return std::nullopt;
    const int type = far_out ? 4 : asked;
    if (type >= 5)
        return std::nullopt;
    return static_cast<DragonKind>(type);
}

void DragonSchedule::dragon_installed(DragonKind kind, bool for_biome, Vec3 player,
                                      EnemyRng &rng) noexcept
{
    // SetCurrentTimedDragonType: a timed dragon moves EVERY peer's ladder on.
    if (!for_biome && kind != DragonKind::count)
        next_type_ = static_cast<int>(kind) + 1;
    recalculate(player, rng);
}

void DragonSchedule::dragon_removed(Vec3 player, EnemyRng &rng) noexcept
{
    allowed_delay_ = 8.0F;
    recalculate(player, rng);
}

DragonEvents DragonFlight::update(const PlayerState &player, float delta_seconds, EnemyRng &rng,
                                  bool target_visible, const WorldRegion *terrain)
{
    DragonEvents events;
    if (!active_)
        return events;
    const DragonStats &stats = dragon_stats(dragon_.kind);
    // An online owner flies the retail DragonEntity (world/dragon_brain.hpp)
    // and presents the replica its own waypoints feed. This overload gives
    // that brain a one-player session: `player` is the only gamer and
    // `target_visible` is every terrain trace. A dying dragon has no brain
    // left at all (HandleKillDragonMessage removes it) and falls as the replica.
    if (network_owner_ && dragon_.state != DragonState::dying)
    {
        class OnePlayerSession final : public DragonBrainSession
        {
          public:
            OnePlayerSession(const PlayerState &player, bool visible, EnemyRng &rng,
                             std::uint8_t gid) noexcept
                : player_{player}, visible_{visible}, rng_{rng}, gid_{gid}
            {
            }
            std::vector<DragonTargetCandidate> players(Vec3) override
            {
                return {{gid_, player_.position, true, true, 1.0F, visible_}};
            }
            std::optional<Vec3> living_position(std::uint8_t gid) override
            {
                return gid == gid_ ? std::optional{player_.position} : std::nullopt;
            }
            bool can_see(Vec3, Vec3) override
            {
                return visible_;
            }
            bool region_loaded(Vec3) override
            {
                return true;
            }
            std::uint8_t local_gid() override
            {
                return gid_;
            }
            bool local_valid() override
            {
                return true;
            }
            Vec3 local_position() override
            {
                return player_.position;
            }
            float random_float() override
            {
                return rng_.next01();
            }
            int random_int(int minimum, int maximum_exclusive) override
            {
                return rng_.next_int(minimum, maximum_exclusive);
            }

          private:
            const PlayerState &player_;
            bool visible_;
            EnemyRng &rng_;
            std::uint8_t gid_;
        };
        OnePlayerSession session{player, target_visible, rng, owner_gid_};
        return update_owner(session, delta_seconds);
    }
    Dragon &dragon = dragon_;
    dragon.state_time += delta_seconds;
    dragon.clip_time += delta_seconds;
    dragon.fire_timer += delta_seconds;
    const float dx = player.position.x - dragon.position.x;
    const float dz = player.position.z - dragon.position.z;
    const float flat_distance = std::sqrt(dx * dx + dz * dz);
    constexpr float max_view_distance = 300.0F;
    constexpr float escape_distance = 420.0F;
    constexpr float escape_grace_seconds = 30.0F;
    constexpr float hidden_grace_seconds = 35.0F;
    constexpr float minimum_engagement_seconds = 120.0F;
    const bool engaged = dragon.state == DragonState::loiter ||
                         dragon.state == DragonState::strafe || dragon.state == DragonState::hover;
    const bool target_available = target_visible && flat_distance <= max_view_distance;
    if (engaged)
    {
        engagement_seconds_ += delta_seconds;
        if (target_available)
            lost_target_seconds_ = std::max(0.0F, lost_target_seconds_ - delta_seconds * 3.0F);
        else
            lost_target_seconds_ += delta_seconds;

        const bool escaped =
            flat_distance > escape_distance && lost_target_seconds_ >= escape_grace_seconds;
        const bool hidden_long_enough = engagement_seconds_ >= minimum_engagement_seconds &&
                                        lost_target_seconds_ >= hidden_grace_seconds;
        if (escaped || hidden_long_enough)
        {
            dragon.state = DragonState::leave;
            dragon.state_time = 0.0F;
            dragon.fire_timer = 0.0F;
        }
        else if (!target_available &&
                 (dragon.state == DragonState::strafe || dragon.state == DragonState::hover))
        {
            // Break the firing pass and circle while searching. This keeps
            // cover meaningful instead of letting a remembered target receive
            // fireballs through terrain during the grace period.
            dragon.state = DragonState::loiter;
            dragon.state_time = 0.0F;
            dragon.loiter_direction = rng.next01() < 0.5F ? 1 : -1;
            dragon.loiter_seconds = (2.0F + rng.next01() * 4.0F) * 1.5F;
        }
    }
    // DragonEntity's Euler flight model: each state chooses TargetYaw,
    // TargetAltitude and TargetVelocity, then this common integrator rate-limits
    // all four channels before moving along LocalToWorld.Forward.
    constexpr float yaw_rate = 0.35F;
    constexpr float pitch_rate = 0.2F;
    constexpr float max_pitch = 0.4F;
    constexpr float roll_rate = 1.0F;
    constexpr float max_roll = 1.0F;
    constexpr float max_acceleration = 10.0F;
    const auto wrap_angle = [](float angle) {
        while (angle > pi)
            angle -= 2.0F * pi;
        while (angle < -pi)
            angle += 2.0F * pi;
        return angle;
    };
    const auto fly_heading = [&](float desired_yaw, float altitude, float target_speed,
                                 bool force_level_roll = false) {
        desired_yaw = wrap_angle(desired_yaw);
        const float yaw_error = wrap_angle(desired_yaw - dragon.yaw);
        const float desired_roll =
            force_level_roll ? 0.0F : std::clamp(yaw_error, -max_roll, max_roll);
        const float altitude_error = altitude - dragon.position.y;
        const float desired_pitch =
            std::abs(altitude_error) < 2.0F
                ? 0.0F
                : std::copysign(std::min(altitude_error * altitude_error / 30.0F, max_pitch),
                                altitude_error);
        brain_target_yaw_ = desired_yaw;
        brain_target_roll_ = desired_roll;
        brain_target_pitch_ = desired_pitch;
        brain_target_velocity_ = target_speed;
        // MathTools.MoveTowardTargetAngle / MoveTowardTarget: each channel
        // closes at min(|error|, rate) per second, not at a constant rate.
        if (desired_yaw != dragon.yaw)
        {
            const float yaw_step = std::clamp(yaw_error, -yaw_rate, yaw_rate);
            const float moved = wrap_angle(dragon.yaw + yaw_step * delta_seconds);
            dragon.yaw = std::abs(moved - desired_yaw) < 0.01F ? desired_yaw : moved;
        }
        dragon.velocity = dragon_move_toward_target(dragon.velocity, target_speed,
                                                    max_acceleration, delta_seconds);
        dragon.roll =
            dragon_move_toward_target(dragon.roll, desired_roll, roll_rate, delta_seconds);
        dragon.pitch =
            dragon_move_toward_target(dragon.pitch, desired_pitch, pitch_rate, delta_seconds);
        const DragonBasis basis = dragon_attitude_basis(dragon.yaw, dragon.pitch, dragon.roll);
        dragon.position.x += basis.forward.x * dragon.velocity * delta_seconds;
        dragon.position.y += basis.forward.y * dragon.velocity * delta_seconds;
        dragon.position.z += basis.forward.z * dragon.velocity * delta_seconds;
        dragon.clip =
            (dragon.pitch > 0.03F || std::abs(dragon.roll) > 0.18F) ? "fly_forward" : "flying_idle";
        return yaw_error;
    };
    const auto steer_toward = [&](float target_x, float target_z, float altitude,
                                  float target_speed, bool force_level_roll = false) {
        const float to_x = target_x - dragon.position.x;
        const float to_z = target_z - dragon.position.z;
        const float desired_yaw =
            std::hypot(to_x, to_z) > 10.0F ? std::atan2(-to_x, -to_z) : dragon.yaw;
        return fly_heading(desired_yaw, altitude, target_speed, force_level_roll);
    };
    const auto enter_loiter = [&](bool has_target) {
        dragon.state = DragonState::loiter;
        dragon.state_time = 0.0F;
        dragon.fire_timer = 0.0F;
        dragon.loiter_direction = rng.next01() < 0.5F ? 1 : -1;
        dragon.loiter_seconds = (2.0F + rng.next01() * 4.0F) * (has_target ? 1.0F : 1.5F);
    };
    const auto enter_attack = [&]() {
        dragon.state_time = 0.0F;
        dragon.fire_timer = 0.0F;
        if (rng.next01() < 0.5F)
        {
            dragon.state = DragonState::hover;
            // Zero means HoverAttack is still closing to HoverDistance. Shots
            // and the initial 1..2 s delay begin only once actual hover starts.
            dragon.hover_shots = 0;
        }
        else
        {
            dragon.state = DragonState::strafe;
        }
    };
    const auto launch_fireball = [&]() {
        // ShootFireball: from the ponytail bone, straight at the target's
        // feet at FireballVelocity — no gravity, no lead.
        const DragonBasis basis = dragon_attitude_basis(dragon.yaw, dragon.pitch, dragon.roll);
        const Vec3 muzzle = dragon_muzzle_offset(dragon_animation_index(dragon.clip));
        const Vec3 origin{dragon.position.x + basis.right.x * muzzle.x + basis.up.x * muzzle.y -
                              basis.forward.x * muzzle.z,
                          dragon.position.y + basis.right.y * muzzle.x + basis.up.y * muzzle.y -
                              basis.forward.y * muzzle.z,
                          dragon.position.z + basis.right.z * muzzle.x + basis.up.z * muzzle.y -
                              basis.forward.z * muzzle.z};
        events.fireball_origins.push_back(origin);
        events.fireball_velocities.push_back(
            dragon_fireball_velocity(dragon.kind, origin, player.position));
    };
    switch (dragon.state)
    {
    case DragonState::approach:
        steer_toward(player.position.x, player.position.z, 120.0F, stats.speed);
        // DefaultState acquires within MaxViewDistance and immediately picks
        // an attack type; it does not force an artificial 80 m setup circle.
        if (target_available)
        {
            enter_attack();
        }
        break;
    case DragonState::loiter: {
        // DragonLoiterLeft/RightState: follow the tangent of the original
        // 200 m circle and bias the heading inward/outward to restore radius.
        const float from_target_x = dragon.position.x - player.position.x;
        const float from_target_z = dragon.position.z - player.position.z;
        const float radius = std::hypot(from_target_x, from_target_z);
        constexpr float loiter_radius = 200.0F;
        const float radial_heading = std::atan2(-from_target_x, -from_target_z);
        const float radial_error = radius <= loiter_radius
                                       ? -std::min(1.5F, (loiter_radius - radius) / 20.0F)
                                       : std::min(1.5F, (radius - loiter_radius) / 30.0F);
        const float direction = static_cast<float>(dragon.loiter_direction);
        const float loiter_heading = radial_heading + direction * (pi * 0.5F + radial_error);
        fly_heading(loiter_heading, stats.loiter_altitude, stats.speed);
        if (dragon.state_time >= dragon.loiter_seconds)
        {
            if (!target_available)
            {
                ++dragon.loiters;
                enter_loiter(false);
                break;
            }
            // Original DragonLoiterState resets LoitersLeft to MaxLoiters every
            // time it enters with a valid target, then immediately reacquires
            // that player. MaxLoiters counts only targetless search circuits;
            // it is not a cap on attacks against a visible living player. Our
            // single-player port always has that engaged target here, so keep
            // scheduling attack passes instead of leaving after three volleys.
            dragon.loiters = 0;
            enter_attack();
        }
        break;
    }
    case DragonState::strafe: {
        const float alignment_error =
            steer_toward(player.position.x, player.position.z, stats.hunting_altitude, stats.speed);
        // DragonStrafeAttackState: fire every StrafeFireRate seconds while
        // between the attack distances and lined up on the target.
        if (dragon.fire_timer >= stats.strafe_fire_seconds &&
            flat_distance > stats.min_attack_distance &&
            flat_distance < stats.max_attack_distance && std::abs(alignment_error) < 0.32F)
        {
            dragon.fire_timer = 0.0F;
            launch_fireball();
        }
        // BreakOffStrafeDistance (40): peel away and set up another pass.
        if (flat_distance < 40.0F || dragon.state_time > 20.0F)
        {
            enter_loiter(target_available);
        }
        break;
    }
    case DragonState::hover: {
        // HoverAttack closes at full speed. DragonHoverState begins only inside
        // HoverDistance, eases TargetVelocity to exactly 0.25, levels the bank,
        // and fires 4-8 rounds at HoverFireRate.
        const bool hovering = flat_distance <= 50.0F;
        steer_toward(player.position.x, player.position.z, stats.hunting_altitude,
                     hovering ? 0.25F : stats.speed, hovering);
        if (hovering && dragon.hover_shots == 0)
        {
            dragon.hover_shots = 4 + rng.next_int(0, 5);
            dragon.fire_timer = -stats.hover_fire_seconds * rng.next01();
        }
        if (hovering)
            dragon.clip = "Idle";
        if (hovering && dragon.fire_timer >= stats.hover_fire_seconds)
        {
            dragon.fire_timer = 0.0F;
            launch_fireball();
            if (--dragon.hover_shots <= 0)
            {
                enter_loiter(target_available);
            }
        }
        if (dragon.state_time > 24.0F)
        {
            enter_loiter(target_available);
        }
        break;
    }
    case DragonState::leave:
        fly_heading(dragon.yaw, 140.0F, stats.speed);
        if (flat_distance > 600.0F)
        {
            active_ = false;
            events.left = true;
        }
        break;
    case DragonState::dying: {
        // DragonClientEntity.UpdateWhileDead. The corpse is a projectile: it
        // keeps the velocity it died with under 10 m/s^2 until death_air_1
        // reaches its pause frame, then falls with collision, skids to a stop
        // on the ground, and is removed five seconds after the clip has played
        // out. Loot appears there, not under the point where it was killed.
        constexpr float pause_time = 1.3666667F; // DeathAnimationPackages[0].PauseTime
        // death_air_1 in the owned content: 2.9667 s in 1.6.3.8, 3 s in 1.9.9.8.
        const float clip_length = modern_content_ ? 3.0F : 2.9666667F;
        if (death_phase_ < 2U)
        {
            death_velocity_.y -= 10.0F * delta_seconds;
            // Collision starts on the first death frame. In a low cavern the
            // 1.37-second reaction animation can reach the floor before its
            // pause marker; the old free-flight branch then embedded the body,
            // and SkipEmbedded allowed the corpse to fall through the floor.
            {
                if (move_dead_dragon(terrain, delta_seconds))
                {
                    if (death_phase_ == 0U)
                        death_phase_ = 1U;
                    death_clip_playing_ = true; // "DragonFall", and the clip resumes
                    death_velocity_.x *= 0.7F;
                    death_velocity_.z *= 0.7F;
                    if (death_velocity_.x * death_velocity_.x +
                            death_velocity_.y * death_velocity_.y +
                            death_velocity_.z * death_velocity_.z <
                        0.5F)
                    {
                        death_phase_ = 2U;
                        death_timer_ = 5.0F; // TimeToWaitAfterStop
                    }
                }
            }
        }
        // `ClipCurrentTime >= PauseTime` is read BEFORE the model advances its
        // clip, and AnimationPlayer advances by TimeSpan.FromSeconds(dt), which
        // rounds to the millisecond: 17 ms a frame at 60 Hz. That is the
        // original's 82 ballistic frames.
        if (death_phase_ == 0U)
        {
            if (death_clip_time_ >= pause_time)
            {
                death_phase_ = 1U;
                death_clip_playing_ = false;
            }
        }
        else if (death_phase_ == 2U && clip_length - death_clip_time_ < 0.5F)
            death_timer_ -= delta_seconds;
        if (death_clip_playing_)
            death_clip_time_ = std::min(
                clip_length,
                death_clip_time_ + std::floor(delta_seconds * 1000.0F + 0.5F) / 1000.0F);
        if (death_phase_ == 2U && death_timer_ < 0.0F)
        {
            events.loot_position = {dragon.position.x + std::sin(dragon.yaw) * 7.0F,
                                    dragon.position.y,
                                    dragon.position.z + std::cos(dragon.yaw) * 7.0F};
            active_ = false;
            events.died = true;
        }
        break;
    }
    case DragonState::despawn:
    default:
        active_ = false;
        break;
    }
    return events;
}

EnemyInstanceRng::EnemyInstanceRng() noexcept
{
    reset(0);
}

EnemyInstanceRng::EnemyInstanceRng(std::int32_t seed) noexcept
{
    reset(seed);
}

void EnemyInstanceRng::reset(std::int32_t seed) noexcept
{
    constexpr std::int32_t maximum = 2'147'483'647;
    const std::int32_t subtraction =
        seed == std::numeric_limits<std::int32_t>::min() ? maximum : std::abs(seed);
    std::int32_t value = 161'803'398 - subtraction;
    seed_array_.fill(0);
    seed_array_[55] = value;
    std::int32_t next = 1;
    for (int index = 1; index < 55; ++index)
    {
        const int slot = (21 * index) % 55;
        seed_array_[static_cast<std::size_t>(slot)] = next;
        next = value - next;
        if (next < 0)
            next += maximum;
        value = seed_array_[static_cast<std::size_t>(slot)];
    }
    for (int pass = 1; pass < 5; ++pass)
        for (int slot = 1; slot < 56; ++slot)
        {
            seed_array_[static_cast<std::size_t>(slot)] -=
                seed_array_[static_cast<std::size_t>(1 + (slot + 30) % 55)];
            if (seed_array_[static_cast<std::size_t>(slot)] < 0)
                seed_array_[static_cast<std::size_t>(slot)] += maximum;
        }
    inext_ = 0;
    inextp_ = 21;
}

std::int32_t EnemyInstanceRng::next_sample() noexcept
{
    constexpr std::int32_t maximum = 2'147'483'647;
    if (++inext_ >= 56)
        inext_ = 1;
    if (++inextp_ >= 56)
        inextp_ = 1;
    std::int32_t result = seed_array_[static_cast<std::size_t>(inext_)] -
                          seed_array_[static_cast<std::size_t>(inextp_)];
    if (result == maximum)
        --result;
    if (result < 0)
        result += maximum;
    seed_array_[static_cast<std::size_t>(inext_)] = result;
    return result;
}

float EnemyInstanceRng::next01() noexcept
{
    return static_cast<float>(static_cast<double>(next_sample()) / 2'147'483'647.0);
}

int EnemyInstanceRng::next_int(int minimum, int maximum_exclusive) noexcept
{
    if (maximum_exclusive <= minimum)
        return minimum;
    const double range = static_cast<double>(maximum_exclusive) - static_cast<double>(minimum);
    return minimum + static_cast<int>(static_cast<double>(next_sample()) / 2'147'483'647.0 * range);
}

bool EnemyHorde::spawn(int type, Vec3 position, float midnight, std::uint32_t id,
                       std::uint8_t target_gid)
{
    const EnemyStats &stats = enemy_stats(type);
    EnemyInitialization initialization;
    // EnemyType.CreateInitPackage.
    initialization.slow_speed = stats.base_slow_speed + rng_.next01() * stats.random_slow_speed;
    if (stats.has_run_fast)
    {
        initialization.fast_speed = stats.base_fast_speed - 0.25F + rng_.next01() * 0.5F;
        initialization.run_activation_time =
            stats.base_run_activation_time -
            stats.base_run_activation_time * 0.5F * (rng_.next01() * midnight);
        initialization.normal_activation_time = 45.0F;
        initialization.emerge_speed =
            midnight > 0.8F || stats.spawn_animation_speed == 1.0F
                ? stats.spawn_animation_speed
                : stats.spawn_animation_speed * 0.5F +
                      stats.spawn_animation_speed * 0.5F * std::min(1.0F, midnight * 2.0F);
    }
    else
    {
        initialization.fast_speed = 0.0F;
        initialization.run_activation_time = 0.0F;
        initialization.normal_activation_time = 0.0F;
        initialization.emerge_speed = stats.spawn_animation_speed;
    }
    // The original never builds an enemy from these floats: it sends them as
    // millisecond integers (InitPackage.Write truncates `1000f * value`) and
    // creates the enemy, its own copy included, from the echoed message
    // (InitPackage.Read, `(float)n / 1000f`). Every peer must start from the
    // same quantized speeds or the local copy outruns the others.
    const auto wire = [](float value) noexcept {
        return static_cast<float>(static_cast<std::int32_t>(1000.0F * value)) / 1000.0F;
    };
    initialization.slow_speed = wire(initialization.slow_speed);
    initialization.fast_speed = wire(initialization.fast_speed);
    initialization.emerge_speed = wire(initialization.emerge_speed);
    initialization.run_activation_time = wire(initialization.run_activation_time);
    initialization.normal_activation_time = wire(initialization.normal_activation_time);
    const std::int32_t random_seed = static_cast<std::int32_t>(rng_.next() & 0x7FFF'FFFFULL);
    return spawn_networked(type, position, initialization, id, random_seed, target_gid);
}

bool EnemyHorde::spawn_networked(int type, Vec3 position, EnemyInitialization initialization,
                                 std::uint32_t id, std::int32_t random_seed,
                                 std::uint8_t target_gid)
{
    if (type < 0 || type >= enemy_type_count || random_seed < 0 ||
        find_enemy({target_gid, id}).has_value() || enemies_.size() >= 500U)
        return false;
    const EnemyStats &stats = enemy_stats(type);
    Enemy enemy;
    enemy.type = type;
    enemy.position = position;
    enemy.health = stats.starting_health;
    enemy.id = id;
    enemy.random_seed = random_seed;
    enemy.target_gid = target_gid;
    enemy.instance_rng.reset(random_seed);
    enemy.slow_speed = initialization.slow_speed;
    enemy.fast_speed = initialization.fast_speed;
    enemy.run_activation_time = initialization.run_activation_time;
    enemy.normal_activation_time = initialization.normal_activation_time;
    enemy.emerge_speed = initialization.emerge_speed;
    // BaseZombie..ctor ends with ChangeState(EType.GetEmergeState(this)).
    enemy_simulation::spawn(enemy, &clip_durations_);
    enemies_.push_back(enemy);
    return true;
}

bool EnemyHorde::accept_remote_spawn() noexcept
{
    if (enemies_.size() < 45U)
        return true;
    if (enemies_.size() >= 50U)
        return false;
    ++remote_spawn_counter_;
    return (remote_spawn_counter_ & 3) == 1;
}

std::size_t EnemyHorde::count_for_target(std::uint8_t target_gid) const noexcept
{
    std::size_t total{};
    for (const Enemy &enemy : enemies_)
        if (enemy.target_gid == target_gid && enemy.state != EnemyState::despawn)
            ++total;
    return total;
}

std::optional<std::size_t> EnemyHorde::find_enemy(EnemyIdentity identity) const noexcept
{
    for (std::size_t index{}; index < enemies_.size(); ++index)
        if (enemies_[index].target_gid == identity.target_gid &&
            enemies_[index].id == identity.enemy_id)
            return index;
    return std::nullopt;
}

bool EnemyHorde::speed_up_enemy(EnemyIdentity identity)
{
    const auto found = find_enemy(identity);
    if (!found.has_value())
        return false;
    Enemy &enemy = enemies_[*found];
    // HandleSpeedUpEnemyMessage: `!IsMovingFast && !IsDead`. A repeat (the
    // original sends one per frame until the enraged state arrives, and an
    // impaired link duplicates them) must not restart the walk clip.
    if (enemy.running || enemy.state == EnemyState::die || enemy.state == EnemyState::despawn)
        return false;
    enemy_simulation::speed_up(enemy, &clip_durations_);
    return true;
}

bool EnemyHorde::give_up_enemy(EnemyIdentity identity)
{
    const auto found = find_enemy(identity);
    if (!found.has_value())
        return false;
    Enemy &enemy = enemies_[*found];
    // BaseZombie.GiveUp is idempotent (`_currentState != GetGiveUpState`) and
    // HandleEnemyGiveUpMessage skips dead enemies entirely.
    if (enemy.state == EnemyState::give_up || enemy.state == EnemyState::die ||
        enemy.state == EnemyState::despawn)
        return false;
    enemy_simulation::give_up(enemy, &clip_durations_);
    return true;
}

bool EnemyHorde::kill_enemy(EnemyIdentity identity, bool create_loot)
{
    const auto found = find_enemy(identity);
    if (!found.has_value())
        return false;
    Enemy &enemy = enemies_[*found];
    // EnemyManager.HandleKillEnemyMessage searches its list with `!IsDead`, so a
    // kill naming an enemy that is already dying matches nothing and the whole
    // handler — pickup roll, statistics and Kill() — is skipped. Re-arming the
    // roll here let a duplicated or reordered KillEnemyMessage drop a second
    // loot stack for the same corpse.
    if (enemy.state == EnemyState::die || enemy.state == EnemyState::despawn)
        return false;
    enemy.health = 0.0F;
    enemy_simulation::kill(enemy, &clip_durations_);
    enemy.loot_pending = create_loot;
    return true;
}

template <typename AcceptHit>
[[nodiscard]] EnemyHorde::RayHit trace_enemy_boxes(const std::vector<Enemy> &enemies, Vec3 origin,
                                                   Vec3 direction, float max_distance,
                                                   int ignored_index, AcceptHit &&accept_hit)
{
    EnemyHorde::RayHit result;
    result.distance = max_distance;
    for (std::size_t index = 0; index < enemies.size(); ++index)
    {
        if (static_cast<int>(index) == ignored_index)
            continue;
        const Enemy &enemy = enemies[index];
        if (enemy.state == EnemyState::emerge || enemy.state == EnemyState::die ||
            enemy.state == EnemyState::despawn)
            continue;
        // Slab test against the enemy's standing box.
        const Vec3 minimum{enemy.position.x - enemy_half_width, enemy.position.y,
                           enemy.position.z - enemy_half_width};
        const Vec3 maximum{enemy.position.x + enemy_half_width, enemy.position.y + enemy_height,
                           enemy.position.z + enemy_half_width};
        float enter = 0.0F;
        float exit = result.distance;
        Vec3 entry_normal{};
        bool miss = false;
        const std::array<std::array<float, 3>, 1> unused{};
        (void)unused;
        const std::array<float, 3> origins{origin.x, origin.y, origin.z};
        const std::array<float, 3> directions{direction.x, direction.y, direction.z};
        const std::array<float, 3> minimums{minimum.x, minimum.y, minimum.z};
        const std::array<float, 3> maximums{maximum.x, maximum.y, maximum.z};
        for (int axis = 0; axis < 3 && !miss; ++axis)
        {
            const auto a = static_cast<std::size_t>(axis);
            if (std::abs(directions[a]) < 0.000001F)
            {
                if (origins[a] < minimums[a] || origins[a] > maximums[a])
                    miss = true;
                continue;
            }
            float t0 = (minimums[a] - origins[a]) / directions[a];
            float t1 = (maximums[a] - origins[a]) / directions[a];
            float normal_sign = -1.0F;
            if (t0 > t1)
            {
                std::swap(t0, t1);
                normal_sign = 1.0F;
            }
            if (t0 > enter)
            {
                enter = t0;
                entry_normal = {};
                if (axis == 0)
                    entry_normal.x = normal_sign;
                else if (axis == 1)
                    entry_normal.y = normal_sign;
                else
                    entry_normal.z = normal_sign;
            }
            exit = std::min(exit, t1);
            if (enter > exit)
                miss = true;
        }
        if (miss || enter >= result.distance)
            continue;
        const float hit_y = origin.y + direction.y * enter;
        // BaseZombie.IsHeadshot uses a strict 1.5-unit offset from the feet,
        // independent of the 1.65-unit standing AABB height.
        const bool headshot = hit_y - enemy.position.y > 1.5F;
        if (!accept_hit(enemy, headshot))
            continue;
        result.distance = enter;
        result.index = static_cast<int>(index);
        result.normal = entry_normal;
        result.headshot = headshot;
    }
    return result;
}

EnemyHorde::RayHit EnemyHorde::trace_along_ray(Vec3 origin, Vec3 direction, float max_distance,
                                               int ignored_index) const
{
    return trace_enemy_boxes(enemies_, origin, direction, max_distance, ignored_index,
                             [](const Enemy &, bool) { return true; });
}

EnemyHorde::RayHit EnemyHorde::trace_projectile_along_ray(Vec3 origin, Vec3 direction,
                                                          float max_distance)
{
    return trace_enemy_boxes(
        enemies_, origin, direction, max_distance, -1, [&](const Enemy &enemy, bool headshot) {
            const float chance = enemy_stats(enemy.type).chance_of_bullet_strike;
            return chance >= 1.0F || headshot || rng_.next01() <= chance;
        });
}

int EnemyHorde::damage_along_ray(Vec3 origin, Vec3 direction, float max_distance, float damage,
                                 DamageFlags flags)
{
    const RayHit hit = trace_projectile_along_ray(origin, direction, max_distance);
    if (hit)
        damage_enemy(static_cast<std::size_t>(hit.index), damage, flags, hit.headshot);
    return hit.index;
}

void EnemyHorde::damage_enemy(std::size_t index, float damage, DamageFlags flags, bool headshot,
                              bool create_loot)
{
    if (index >= enemies_.size())
        return;
    Enemy &enemy = enemies_[index];
    if (enemy.state == EnemyState::die || enemy.state == EnemyState::despawn)
        return;
    enemy.health -= damage * enemy_damage_multiplier(enemy.type, flags, headshot);
    if (enemy.health <= 0.0F)
    {
        enemy_simulation::kill(enemy, &clip_durations_);
        enemy.loot_pending = create_loot;
    }
    else
    {
        // BaseZombie.TakeDamage: every state but the hit reaction itself is
        // interrupted by one, the give-up walk-off included.
        enemy_simulation::react_to_hit(enemy, &clip_durations_);
    }
}

std::vector<std::size_t> EnemyHorde::apply_explosive_damage(Vec3 location, float damage_range,
                                                            bool create_loot)
{
    std::vector<std::size_t> killed;
    for (std::size_t index = 0; index < enemies_.size(); ++index)
    {
        const Vec3 offset = subtract(enemies_[index].position, location);
        const float distance =
            std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
        if (distance >= damage_range)
            continue;
        const float half = damage_range * 0.5F;
        const float damage =
            distance < half ? 12.0F : 12.0F * (1.0F - (distance - half) / (damage_range - half));
        const bool was_alive = enemies_[index].state != EnemyState::die &&
                               enemies_[index].state != EnemyState::despawn;
        damage_enemy(index, damage, DamageFlags::blunt, false, create_loot);
        if (was_alive && enemies_[index].state == EnemyState::die)
            killed.push_back(index);
    }
    return killed;
}

float EnemyHorde::attenuate_velocity(std::uint8_t target_gid, Vec3 forward,
                                     Vec3 player_position) const noexcept
{
    float scale = 1.0F;
    for (const Enemy &enemy : enemies_)
    {
        if (enemy.target_gid != target_gid || !enemy.blocking ||
            enemy.state == EnemyState::despawn)
            continue;
        const Vec3 offset = subtract(enemy.position, player_position);
        const float squared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
        float factor = 1.0F;
        if (squared < 4.0F && std::abs(offset.y) < 1.5F)
        {
            factor = 0.5F;
            if (squared > 0.0001F)
            {
                const float inverse = 1.0F / std::sqrt(squared);
                const float toward = (offset.x * forward.x + offset.y * forward.y +
                                      offset.z * forward.z) * inverse;
                if (toward > 0.0F)
                    factor *= std::min(1.0F, 2.0F * (1.0F - toward));
            }
        }
        scale *= factor;
    }
    return scale;
}

EnemyEvents EnemyHorde::update(const WorldRegion &region, const PlayerState &player,
                               float delta_seconds)
{
    return update(region, player, 0U, {}, delta_seconds);
}

namespace
{
// Whether this client holds terrain for the column an entity stands in. Only
// X and Z matter, as in BlockTerrain.RegionIsLoaded: height never unloads.
bool region_holds_column(const WorldRegion &region, Vec3 position) noexcept
{
    const int x = static_cast<int>(std::floor(position.x));
    const int z = static_cast<int>(std::floor(position.z));
    return x >= region.minimum_x() && x <= region.maximum_x() && z >= region.minimum_z() &&
           z <= region.maximum_z();
}
} // namespace

EnemyEvents EnemyHorde::update(const WorldRegion &region, const PlayerState &local_player,
                               std::uint8_t local_gid, const TargetLookup &target_lookup,
                               float delta_seconds)
{
    EnemyEvents events;
    // BlockTerrain scrolls with the LOCAL player, whoever an enemy is chasing.
    if (!terrain_window_external_)
        terrain_window_.follow(local_player.position.x, local_player.position.z);
    // A destination outside the window is not a scroll but BlockTerrain.Teleport:
    // every chunk is discarded and rebuilt around the new centre, and until a
    // chunk has geometry again RegionIsLoaded answers false for it. Every
    // BaseZombie therefore removes itself on its next update, wherever it
    // stands -- including the ones near where the player arrives.
    const bool terrain_reset = terrain_resets_seen_.has_value() &&
                               *terrain_resets_seen_ != terrain_window_.resets();
    terrain_resets_seen_ = terrain_window_.resets();
    if (terrain_reset)
    {
        enemies_.clear();
        return events;
    }
    for (auto &enemy : enemies_)
    {
        // BaseZombie.OnUpdate's first statement, in both shipped generations:
        //     if (!BlockTerrain.Instance.RegionIsLoaded(LocalPosition)) { Remove(); return; }
        // The enemy is dropped silently, on this peer only, in whatever state
        // it is in, when it is outside the terrain THIS peer has loaded. The
        // original has no distance-to-target rule: the flat 100-block radius
        // that stood here removed enemies a stock peer was still fighting.
        // "Loaded" is the original's 24-chunk window around the local player
        // and, because this client streams a smaller window than that, also
        // the region it actually holds: an enemy past either edge has no
        // ground to stand on here.
        if (!terrain_window_.contains(enemy.position.x, enemy.position.z) ||
            !region_holds_column(region, enemy.position))
        {
            enemy.state = EnemyState::despawn;
            continue;
        }
        std::optional<PlayerState> remote_target;
        // A target this peer cannot resolve any more: the player left the
        // session, or its updates stopped. BaseZombie.OnUpdate treats that as
        // `!Target.ValidGamer` and gives up on EVERY peer, owner or not.
        // Orphaned enemies are therefore released rather than frozen in place,
        // and the synthetic stand-in keeps every distance rule inert while the
        // give-up clip plays out.
        PlayerState orphan;
        bool target_resolved = true;
        if (enemy.target_gid != local_gid)
        {
            remote_target = target_lookup ? target_lookup(enemy.target_gid)
                                          : std::optional<PlayerState>{};
            if (!remote_target.has_value())
            {
                target_resolved = false;
                orphan.position = enemy.position;
                orphan.health = 0.0F;
            }
        }
        const PlayerState &player = !target_resolved ? orphan
                                    : remote_target.has_value() ? *remote_target
                                                                : local_player;
        const bool local_target = enemy.target_gid == local_gid;
        // Scene.OnUpdate runs every entity's physics before any state machine:
        // gravity, then BaseZombie.ResolveCollsion with the velocity the state
        // chose on the previous frame.
        enemy_simulation::advance_physics(region, enemy, delta_seconds);
        enemy.state_time += delta_seconds;
        enemy.lifetime += delta_seconds;
        // EnemyManager handles the kill message (and therefore the killer's
        // loot roll) at once, before the corpse has played a frame.
        if (enemy.state == EnemyState::die && enemy.loot_pending)
        {
            enemy.loot_pending = false;
            const float origin_distance = std::sqrt(enemy.position.x * enemy.position.x +
                                                    enemy.position.z * enemy.position.z);
            const EnemyLoot loot = enemy_loot(enemy.type, origin_distance, enemy.position.y, rng_);
            if (loot.item_count > 0U)
            {
                events.drops.push_back(loot);
                events.drop_positions.push_back(
                    {enemy.position.x, enemy.position.y + 1.0F, enemy.position.z});
            }
        }
        // BaseZombie.OnUpdate: `!Target.ValidGamer || Target.Dead` -> GiveUp()
        // on every peer, owner or not (both stock generations read the same
        // PlayerUpdate death flag, so this agrees without a message of its
        // own), then the state machine, then the animation clock.
        enemy_simulation::Context context{region, player};
        context.target_alive = target_resolved && player.health > 0.0F;
        context.local_target = local_target;
        context.observe_remote = observe_remote_strikes_; // (Authority)
        context.zombie_fest = zombie_fest_;
        context.modern_rules = modern_rules_;
        context.delta_seconds = delta_seconds;
        context.clip_durations = &clip_durations_;
        context.dig_random = &dig_random_;
        context.events = &events;
        enemy_simulation::update(enemy, context);
    }
    std::erase_if(enemies_, [](const Enemy &enemy) { return enemy.state == EnemyState::despawn; });
    return events;
}

} // namespace openminerz::world
