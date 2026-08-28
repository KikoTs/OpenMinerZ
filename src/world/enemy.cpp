#include "openminerz/world/enemy.hpp"

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

[[nodiscard]] float horizontal_length(Vec3 value) noexcept
{
    return std::sqrt(value.x * value.x + value.z * value.z);
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

// Lightweight axis-separated voxel collision for a standing capsule the size the
// original enemies use.
constexpr float enemy_half_width = 0.35F;
constexpr float enemy_height = 1.65F;
constexpr float enemy_gravity = -20.0F;

[[nodiscard]] bool box_intersects_solid(const WorldRegion &region, Vec3 feet) noexcept
{
    const int min_x = static_cast<int>(std::floor(feet.x - enemy_half_width));
    const int max_x = static_cast<int>(std::floor(feet.x + enemy_half_width));
    const int min_y = static_cast<int>(std::floor(feet.y));
    const int max_y = static_cast<int>(std::floor(feet.y + enemy_height - 0.01F));
    const int min_z = static_cast<int>(std::floor(feet.z - enemy_half_width));
    const int max_z = static_cast<int>(std::floor(feet.z + enemy_half_width));
    for (int y = min_y; y <= max_y; ++y)
        for (int z = min_z; z <= max_z; ++z)
            for (int x = min_x; x <= max_x; ++x)
                if (region.contains(x, y, z) && blocks_player(region.block(x, y, z)))
                    return true;
    return false;
}

void move_enemy(const WorldRegion &region, Enemy &enemy, float delta_seconds)
{
    enemy.velocity.y = std::max(enemy.velocity.y + enemy_gravity * delta_seconds, -40.0F);
    Vec3 next = enemy.position;
    next.x += enemy.velocity.x * delta_seconds;
    if (box_intersects_solid(region, next))
    {
        next.x = enemy.position.x;
        enemy.velocity.x = 0.0F;
    }
    next.z += enemy.velocity.z * delta_seconds;
    if (box_intersects_solid(region, next))
    {
        next.z = enemy.position.z;
        enemy.velocity.z = 0.0F;
    }
    next.y += enemy.velocity.y * delta_seconds;
    enemy.on_ground = false;
    if (box_intersects_solid(region, next))
    {
        if (enemy.velocity.y <= 0.0F)
        {
            enemy.on_ground = true;
            next.y = std::floor(next.y) + 1.0F;
            if (box_intersects_solid(region, next))
                next.y = enemy.position.y;
        }
        else
        {
            next.y = enemy.position.y;
        }
        enemy.velocity.y = 0.0F;
    }
    enemy.position = next;
}

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
    static const EnemyAttackSet zombie{
        zombie_attack_list.data(), static_cast<int>(zombie_attack_list.size()), "attack5", 1.0F};
    static const EnemyAttackSet skeleton{skeleton_attack_list.data(),
                                         static_cast<int>(skeleton_attack_list.size()), "attack3",
                                         1.0F};
    static const EnemyAttackSet axes{axes_attack_list.data(),
                                     static_cast<int>(axes_attack_list.size()), "attack3", 1.0F};
    static const EnemyAttackSet alien{alien_attack_list.data(),
                                      static_cast<int>(alien_attack_list.size()), "Attack2", 1.0F};
    static const EnemyAttackSet felguard{felguard_attack_list.data(),
                                         static_cast<int>(felguard_attack_list.size()), "atack_3",
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
    const float amount = std::clamp(distance / 3450.0F, 0.0F, 1.0F);
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
    if (stats.habitat == EnemyHabitat::hell)
    {
        // Felguards always pay out explosive powder.
        const std::uint16_t count = roll < 0.5F ? 1U : (roll < 0.8F ? 2U : 3U);
        return {108U, count}; // ExplosivePowder
    }
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
    active_ = true;
}

void DragonFlight::apply_network_waypoint(Vec3 position, Vec3 velocity, float host_time,
                                          float target_roll, std::uint8_t animation,
                                          std::uint8_t sound) noexcept
{
    static_cast<void>(
        add_network_waypoint({position, velocity, host_time, target_roll, animation, sound}));
}

bool DragonFlight::apply_network_attack_waypoint(Vec3 position, Vec3 velocity, float host_time,
                                                 float target_roll, std::uint8_t animation,
                                                 std::uint8_t sound, Vec3 target,
                                                 std::int32_t fireball_id, bool animated) noexcept
{
    return add_network_waypoint({position, velocity, host_time, target_roll, animation, sound,
                                 static_cast<std::uint8_t>(animated ? 1U : 2U), target,
                                 fireball_id});
}

bool DragonFlight::add_network_waypoint(NetworkWaypoint incoming) noexcept
{
    constexpr std::size_t maximum_buffered_waypoints = 128U;
    if (!active_ || !network_replica_ || network_waypoints_.size() >= maximum_buffered_waypoints)
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
        network_waypoints_.push_back({dragon_.position,
                                      {forward.x * dragon_.velocity, forward.y * dragon_.velocity,
                                       forward.z * dragon_.velocity},
                                      network_interpolation_time_,
                                      dragon_.roll,
                                      incoming.animation});
    }
    network_waypoints_.push_back(incoming);
    return true;
}

void DragonFlight::emit_network_fireball(Vec3 target, std::int32_t fireball_id) noexcept
{
    const Vec3 forward = dragon_attitude_basis(dragon_.yaw, dragon_.pitch, dragon_.roll).forward;
    const Vec3 origin{dragon_.position.x + forward.x * 4.0F, dragon_.position.y - 1.0F,
                      dragon_.position.z + forward.z * 4.0F};
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
    const float maximum_roll_change = std::max(0.0F, delta_seconds);
    dragon.roll += std::clamp(target_roll - dragon.roll, -maximum_roll_change, maximum_roll_change);
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
    if (!active_ || !network_replica_ || delta_seconds < 0.0F)
        return;
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
        dragon_.state = DragonState::dying;
        dragon_.state_time = 0.0F;
        dragon_.clip = "death_air_1";
        dragon_.clip_time = 0.0F;
    }
}

DragonEvents DragonFlight::update(const PlayerState &player, float delta_seconds, EnemyRng &rng,
                                  bool target_visible)
{
    DragonEvents events;
    if (!active_)
        return events;
    const DragonStats &stats = dragon_stats(dragon_.kind);
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
        dragon.yaw = wrap_angle(dragon.yaw + std::clamp(yaw_error, -yaw_rate * delta_seconds,
                                                        yaw_rate * delta_seconds));
        dragon.roll += std::clamp(desired_roll - dragon.roll, -roll_rate * delta_seconds,
                                  roll_rate * delta_seconds);
        dragon.pitch += std::clamp(desired_pitch - dragon.pitch, -pitch_rate * delta_seconds,
                                   pitch_rate * delta_seconds);
        dragon.velocity +=
            std::clamp(target_speed - dragon.velocity, -max_acceleration * delta_seconds,
                       max_acceleration * delta_seconds);
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
        // ShootFireball: from the head, straight at the target's feet at
        // FireballVelocity — no gravity, no lead.
        const DragonBasis basis = dragon_attitude_basis(dragon.yaw, dragon.pitch, dragon.roll);
        Vec3 origin = dragon.position;
        origin.x += basis.forward.x * 4.0F;
        origin.z += basis.forward.z * 4.0F;
        origin.y -= 1.0F;
        Vec3 flight{player.position.x - origin.x, player.position.y - origin.y,
                    player.position.z - origin.z};
        const float length = std::max(
            0.001F, std::sqrt(flight.x * flight.x + flight.y * flight.y + flight.z * flight.z));
        flight.x = flight.x / length * stats.fireball_velocity;
        flight.y = flight.y / length * stats.fireball_velocity;
        flight.z = flight.z / length * stats.fireball_velocity;
        events.fireball_origins.push_back(origin);
        events.fireball_velocities.push_back(flight);
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
    case DragonState::dying:
        // Death: roll and pitch collapse, the body drops under gravity while
        // the death clip plays (DragonClientEntity.UpdateWhileDead).
        dragon.roll *= std::max(0.0F, 1.0F - 3.0F * delta_seconds);
        dragon.pitch *= std::max(0.0F, 1.0F - 3.0F * delta_seconds);
        dragon.position.y -= 14.0F * delta_seconds;
        if (dragon.position.y < world_min_y || dragon.state_time > 6.0F)
        {
            active_ = false;
            events.died = true;
        }
        break;
    case DragonState::despawn:
    default:
        active_ = false;
        break;
    }
    return events;
}

float EnemyHorde::clip_duration(const Enemy &enemy, std::string_view clip) const
{
    if (clip_durations_)
    {
        const float duration = clip_durations_(enemy_stats(enemy.type).model, clip);
        if (duration > 0.0F)
            return duration;
    }
    // Fallbacks keep the state machine sane before the exported clips are wired:
    // attacks derive from their last authored hit moment.
    const EnemyAttackSet &attacks = enemy_attacks(enemy_stats(enemy.type).enemy_class);
    for (int index = 0; index < attacks.attack_count; ++index)
    {
        const EnemyAttack &attack = attacks.attacks[index];
        if (attack.clip == clip)
            return attack.hit_times[static_cast<std::size_t>(attack.hit_count - 1)] + 0.55F;
    }
    if (clip == "atack_archer1")
        return 1.8F;
    return 1.4F;
}

namespace
{

// Authored clip names per model family, from the exported skeleton manifests.
template <typename Random>
[[nodiscard]] std::string_view emerge_clip(EnemyClass enemy_class, Random &rng) noexcept
{
    switch (enemy_class)
    {
    case EnemyClass::zombie: {
        // ZombieEmerge.GetClipName: "arise_" + (Rnd.Next(4) + 1).
        constexpr std::array<std::string_view, 4> arises{"arise_1", "arise_2", "arise_3",
                                                         "arise_4"};
        return arises[static_cast<std::size_t>(rng.next_int(0, 4))];
    }
    case EnemyClass::archer:
        // ArcherSkeletonEmerge chooses between both authored stand-up clips.
        return rng.next_int(0, 2) == 0 ? "standup" : "standup2";
    case EnemyClass::alien:
        return "Jump";
    case EnemyClass::felguard:
        return "Idle";
    default:
        return "walk";
    }
}

template <typename Random>
[[nodiscard]] std::string_view hit_clip(EnemyClass enemy_class, Random &rng) noexcept
{
    switch (enemy_class)
    {
    case EnemyClass::zombie: {
        // ZombieHit deliberately uses only reactions 1 and 3.
        return rng.next_int(0, 2) == 0 ? "hit_reaction1" : "hit_reaction3";
    }
    case EnemyClass::alien:
        return rng.next_int(0, 2) == 0 ? "Damage1" : "Damage2";
    case EnemyClass::felguard:
        return "Idle";
    default: {
        constexpr std::array<std::string_view, 3> hits{"gethit1", "gethit2", "gethit3"};
        return hits[static_cast<std::size_t>(rng.next_int(0, 3))];
    }
    }
}

template <typename Random>
[[nodiscard]] std::string_view death_clip(EnemyClass enemy_class, Random &rng) noexcept
{
    switch (enemy_class)
    {
    case EnemyClass::zombie: {
        constexpr std::array<std::string_view, 3> deaths{"death1", "death2", "death3"};
        return deaths[static_cast<std::size_t>(rng.next_int(0, 3))];
    }
    case EnemyClass::alien:
        return "Death";
    case EnemyClass::felguard:
        return rng.next_int(0, 2) == 0 ? "death1" : "death2";
    default: {
        // SkeletonDie.GetAnimName covers all seven clips in every skeleton rig.
        constexpr std::array<std::string_view, 7> deaths{"death1", "death2", "death3", "death4",
                                                         "death5", "death6", "death7"};
        return deaths[static_cast<std::size_t>(rng.next_int(0, 7))];
    }
    }
}

// ZombieChase.StartMoveAnimation: locomotion clip picked by CurrentSpeed
// thresholds, playback rate = min(speed / reference, 1) — except the archer,
// whose walk rate is genuinely uncapped speed-scaled.
[[nodiscard]] std::string_view walk_clip(EnemyClass enemy_class, float speed, bool has_run_fast,
                                         float &clip_speed) noexcept
{
    clip_speed = 1.0F;
    switch (enemy_class)
    {
    case EnemyClass::alien:
        return "MoveLoop";
    case EnemyClass::archer:
        clip_speed = std::max(speed, 0.5F);
        return "walk_archer1";
    case EnemyClass::felguard:
        clip_speed = std::min(speed / (speed < 2.7F ? 1.0F : 3.0F), 1.0F);
        return speed < 2.7F ? "walk" : "run";
    default:
        if (speed < 2.7F)
        {
            clip_speed = std::min(speed, 1.0F);
            return "walk";
        }
        if (speed < 3.7F)
        {
            clip_speed = std::min(speed, 1.0F);
            return "walk2";
        }
        if (speed < 5.0F || !has_run_fast)
        {
            clip_speed = std::min(speed / 3.0F, 1.0F);
            return "run";
        }
        clip_speed = std::min(speed / 4.0F, 1.0F);
        return "run_fast";
    }
}

[[nodiscard]] std::string_view give_up_clip(EnemyClass enemy_class) noexcept
{
    switch (enemy_class)
    {
    case EnemyClass::zombie:
        return "eat_start";
    case EnemyClass::alien:
        return "Jump";
    case EnemyClass::felguard:
        return "Idle";
    default:
        return "enraged";
    }
}

} // namespace

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
    const bool skeleton_starts_chasing = stats.enemy_class == EnemyClass::skeleton ||
                                         stats.enemy_class == EnemyClass::skeleton_sword ||
                                         stats.enemy_class == EnemyClass::skeleton_axes;
    if (skeleton_starts_chasing)
    {
        // SkeletonEnemyType.GetEmergeState returns Chase directly. These models
        // have no stand-up clips, so do not expose one bind-pose frame first.
        enemy.state = EnemyState::chase;
        float clip_rate = 1.0F;
        enemy.clip = walk_clip(stats.enemy_class, enemy.slow_speed, stats.has_run_fast, clip_rate);
        enemy.clip_loops = true;
        enemy.clip_speed = clip_rate;
    }
    else
    {
        enemy.state = EnemyState::emerge;
        if (stats.enemy_class == EnemyClass::archer)
            enemy.swing_count = 5 + enemy.instance_rng.next_int(0, 5);
        enemy.clip = emerge_clip(stats.enemy_class, enemy.instance_rng);
        enemy.clip_loops = false;
        enemy.clip_speed = enemy.emerge_speed;
        // ZombieEmerge.Enter: a random facing while clawing out of the ground.
        enemy.yaw = enemy.instance_rng.next01() * 6.2831855F;
    }
    enemies_.push_back(enemy);
    return true;
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
    if (enemy.state == EnemyState::die || enemy.state == EnemyState::despawn)
        return false;
    enemy.running = true;
    if (enemy.state == EnemyState::chase)
        begin_state(enemy, EnemyState::chase, {});
    return true;
}

bool EnemyHorde::give_up_enemy(EnemyIdentity identity)
{
    const auto found = find_enemy(identity);
    if (!found.has_value())
        return false;
    Enemy &enemy = enemies_[*found];
    if (enemy.state == EnemyState::die || enemy.state == EnemyState::despawn)
        return false;
    begin_state(enemy, EnemyState::give_up, {});
    return true;
}

bool EnemyHorde::kill_enemy(EnemyIdentity identity, bool create_loot)
{
    const auto found = find_enemy(identity);
    if (!found.has_value())
        return false;
    Enemy &enemy = enemies_[*found];
    if (enemy.state == EnemyState::despawn)
        return false;
    if (enemy.state != EnemyState::die)
    {
        enemy.health = 0.0F;
        enemy.state = EnemyState::die;
        enemy.state_time = 0.0F;
        enemy.clip = death_clip(enemy_stats(enemy.type).enemy_class, enemy.instance_rng);
        enemy.clip_loops = false;
        enemy.clip_speed = enemy_stats(enemy.type).die_animation_speed;
        enemy.clip_time = 0.0F;
        enemy.velocity.x = 0.0F;
        enemy.velocity.z = 0.0F;
    }
    enemy.loot_pending = enemy.loot_pending || create_loot;
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
        enemy.state = EnemyState::die;
        enemy.state_time = 0.0F;
        enemy.clip = death_clip(enemy_stats(enemy.type).enemy_class, enemy.instance_rng);
        enemy.clip_loops = false;
        enemy.clip_speed = enemy_stats(enemy.type).die_animation_speed;
        enemy.clip_time = 0.0F;
        enemy.loot_pending = create_loot;
        enemy.velocity.x = 0.0F;
        enemy.velocity.z = 0.0F;
    }
    else if (enemy.state != EnemyState::hit)
    {
        enemy.state = EnemyState::hit;
        enemy.state_time = 0.0F;
        enemy.clip = hit_clip(enemy_stats(enemy.type).enemy_class, enemy.instance_rng);
        enemy.clip_loops = false;
        enemy.clip_speed = enemy_stats(enemy.type).hit_animation_speed;
        enemy.clip_time = 0.0F;
        enemy.velocity.x = 0.0F;
        enemy.velocity.z = 0.0F;
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

void EnemyHorde::begin_state(Enemy &enemy, EnemyState state, const PlayerState &player)
{
    const EnemyStats &stats = enemy_stats(enemy.type);
    enemy.state = state;
    enemy.state_time = 0.0F;
    enemy.clip_time = 0.0F;
    switch (state)
    {
    case EnemyState::chase: {
        const float move_speed =
            enemy.running && enemy.fast_speed > 0.0F ? enemy.fast_speed : enemy.slow_speed;
        float clip_rate = 1.0F;
        enemy.clip = walk_clip(stats.enemy_class, move_speed, stats.has_run_fast, clip_rate);
        enemy.clip_loops = true;
        enemy.clip_speed = clip_rate;
        break;
    }
    case EnemyState::attack: {
        if (stats.enemy_class == EnemyClass::archer)
        {
            enemy.attack_index = -1;
            enemy.clip = "atack_archer1";
            // ArcherSkeletonTryAttack.Enter consumes the 5..9-shot budget
            // rolled by ArcherSkeletonEmerge; it never rerolls it per shot.
            --enemy.swing_count;
        }
        else
        {
            const EnemyAttackSet &attacks = enemy_attacks(stats.enemy_class);
            enemy.attack_index = enemy.instance_rng.next_int(0, attacks.attack_count);
            enemy.clip = attacks.attacks[enemy.attack_index].clip;
            enemy.swing_count = 2 + enemy.instance_rng.next_int(0, 3);
            enemy.miss_count = enemy.instance_rng.next_int(3, 5);
        }
        enemy.clip_loops = false;
        enemy.clip_speed = stats.attack_animation_speed;
        enemy.hit_count = 0;
        enemy.velocity.x = 0.0F;
        enemy.velocity.z = 0.0F;
        break;
    }
    case EnemyState::idle:
        // The installed SkeletonArcher content names this clip with a hyphen,
        // despite the source literal using an underscore.
        enemy.clip = "idle-archer1";
        enemy.clip_loops = true;
        enemy.clip_speed = 1.0F;
        enemy.state_duration = 1.0F + enemy.instance_rng.next01() * 2.0F;
        enemy.velocity.x = 0.0F;
        enemy.velocity.z = 0.0F;
        break;
    case EnemyState::give_up:
        enemy.clip = give_up_clip(stats.enemy_class);
        enemy.clip_loops = false;
        enemy.clip_speed = 1.0F;
        enemy.velocity.x = 0.0F;
        enemy.velocity.z = 0.0F;
        break;
    case EnemyState::dig:
        enemy.clip = "attack1";
        enemy.clip_loops = true;
        enemy.clip_speed = stats.attack_animation_speed;
        enemy.dig_progress = 0.0F;
        enemy.dig_valid = false;
        break;
    default:
        break;
    }
    (void)player;
}

EnemyEvents EnemyHorde::update(const WorldRegion &region, const PlayerState &player,
                               float delta_seconds)
{
    return update(region, player, 0U, {}, delta_seconds);
}

EnemyEvents EnemyHorde::update(const WorldRegion &region, const PlayerState &local_player,
                               std::uint8_t local_gid, const TargetLookup &target_lookup,
                               float delta_seconds)
{
    EnemyEvents events;
    for (auto &enemy : enemies_)
    {
        std::optional<PlayerState> remote_target;
        if (enemy.target_gid != local_gid)
        {
            if (!target_lookup)
                continue;
            remote_target = target_lookup(enemy.target_gid);
            if (!remote_target.has_value())
                continue;
        }
        const PlayerState &player = remote_target.has_value() ? *remote_target : local_player;
        const bool local_target = enemy.target_gid == local_gid;
        const EnemyState initial_state = enemy.state;
        const EnemyStats &stats = enemy_stats(enemy.type);
        enemy.state_time += delta_seconds;
        enemy.lifetime += delta_seconds;
        enemy.clip_time += delta_seconds * enemy.clip_speed;
        const Vec3 to_player = subtract(player.position, enemy.position);
        const float flat_distance = horizontal_length(to_player);
        if (flat_distance > despawn_distance && enemy.state != EnemyState::die)
        {
            enemy.state = EnemyState::despawn;
            continue;
        }
        switch (enemy.state)
        {
        case EnemyState::emerge: {
            // BaseZombie.IsNearAnimationEnd is based on the scaled clip clock,
            // not wall time: transition with 0.25 authored seconds remaining.
            const float duration = clip_duration(enemy, enemy.clip);
            if (enemy.clip_time >= std::max(0.0F, duration - 0.25F))
                begin_state(enemy, EnemyState::chase, player);
            break;
        }
        case EnemyState::chase: {
            // Only the targeted player's local simulation originates the
            // source SpeedUpEnemy message; every peer applies that message.
            if (stats.has_run_fast && !enemy.running && local_target)
            {
                enemy.normal_activation_time -= delta_seconds;
                const float target_speed_squared =
                    player.velocity.x * player.velocity.x + player.velocity.z * player.velocity.z;
                if (target_speed_squared > 3.5F)
                    enemy.run_activation_time -= delta_seconds;
                if (enemy.normal_activation_time <= 0.0F || enemy.run_activation_time <= 0.0F)
                    events.speed_up_requests.push_back({enemy.target_gid, enemy.id});
            }
            const float speed =
                enemy.running && enemy.fast_speed > 0.0F ? enemy.fast_speed : enemy.slow_speed;
            if (flat_distance > 0.05F)
            {
                enemy.velocity.x = to_player.x / flat_distance * speed;
                enemy.velocity.z = to_player.z / flat_distance * speed;
                enemy.yaw = std::atan2(enemy.velocity.x, -enemy.velocity.z);
            }
            // ZombieChase: OnGround && TouchingWall -> jump at the FULL original
            // impulse (10 walking, FastJumpSpeed 13-16 enraged, gravity -20).
            if (enemy.on_ground && std::abs(enemy.velocity.x) + std::abs(enemy.velocity.z) > 0.1F)
            {
                Vec3 probe = enemy.position;
                probe.x += enemy.velocity.x * 0.15F;
                probe.z += enemy.velocity.z * 0.15F;
                if (box_intersects_solid(region, probe))
                {
                    probe.y += 1.05F;
                    if (!box_intersects_solid(region, probe))
                        enemy.velocity.y = enemy.running ? stats.fast_jump_speed : 10.0F;
                    else if (stats.enemy_class == EnemyClass::zombie && enemy.on_ground)
                    {
                        begin_state(enemy, EnemyState::dig, player);
                        break;
                    }
                }
            }
            // ZombieChase: closing in on a player standing on a tower (or in a
            // pit) flips zombies into the dig state to chew through the base.
            if (stats.enemy_class == EnemyClass::zombie && enemy.on_ground &&
                flat_distance < 5.0F && std::abs(to_player.y) > 4.0F)
            {
                begin_state(enemy, EnemyState::dig, player);
                break;
            }
            // Archers stop and shoot from range instead of closing in.
            if (stats.enemy_class == EnemyClass::archer && flat_distance < 30.0F &&
                flat_distance > 4.0F)
            {
                begin_state(enemy, EnemyState::attack, player);
                ++events.attacks_started;
                break;
            }
            if (flat_distance <= attack_enter_distance && std::abs(to_player.y) < 1.5F)
            {
                begin_state(enemy, EnemyState::attack, player);
                ++events.attacks_started;
            }
            else if (flat_distance > give_up_distance && enemy.lifetime > 45.0F)
            {
                begin_state(enemy, EnemyState::give_up, player);
            }
            break;
        }
        case EnemyState::attack: {
            enemy.velocity.x = 0.0F;
            enemy.velocity.z = 0.0F;
            // BaseTryAttack: melee yaw is LOCKED for the whole swing and only
            // snaps to the target near the clip's end; archers track the
            // target continuously while drawing.
            const bool near_swing_end = enemy.clip_time >= clip_duration(enemy, enemy.clip) - 0.25F;
            if (flat_distance > 0.1F &&
                ((stats.enemy_class == EnemyClass::archer && !near_swing_end) ||
                 (stats.enemy_class != EnemyClass::archer && near_swing_end)))
                enemy.yaw = std::atan2(to_player.x, -to_player.z);
            if (stats.enemy_class == EnemyClass::archer)
            {
                // ArcherSkeletonTryAttack: loose the arrow 1.1 seconds into the draw.
                if (!near_swing_end && enemy.hit_count == 0 && enemy.clip_time >= 1.1F)
                {
                    enemy.hit_count = 1;
                    Vec3 arrow_origin = enemy.position;
                    arrow_origin.y += 1.5F;
                    Vec3 target = player.position;
                    target.y += 0.5F;
                    Vec3 flight{};
                    if (const auto ballistic =
                            initial_ballistic_vector(arrow_origin, target, 25.0F, -10.0F);
                        ballistic.has_value())
                    {
                        flight = *ballistic;
                    }
                    else
                    {
                        // ArcherSkeletonTryAttack's exact unreachable-target fallback
                        // uses the unadjusted entity origins, then normalizes to 25.
                        flight = subtract(player.position, enemy.position);
                        const float length = std::sqrt(flight.x * flight.x + flight.y * flight.y +
                                                       flight.z * flight.z);
                        if (length < 0.001F)
                            flight = {0.0F, 25.0F, 0.0F};
                        else
                        {
                            flight.x = flight.x / length * 25.0F;
                            flight.y = flight.y / length * 25.0F;
                            flight.z = flight.z / length * 25.0F;
                        }
                    }
                    events.arrow_origins.push_back(arrow_origin);
                    events.arrow_velocities.push_back(flight);
                    events.arrow_target_gids.push_back(enemy.target_gid);
                    ++events.arrows_fired;
                }
                if (near_swing_end)
                    begin_state(enemy,
                                enemy.swing_count <= 0 ? EnemyState::give_up : EnemyState::idle,
                                player);
                break;
            }
            const EnemyAttackSet &attacks = enemy_attacks(stats.enemy_class);
            const EnemyAttack &attack =
                attacks.attacks[std::clamp(enemy.attack_index, 0, attacks.attack_count - 1)];
            // BaseTryAttack timed hits: each authored hit moment tests height, range
            // and facing before applying player damage.
            while (enemy.hit_count < attack.hit_count &&
                   enemy.clip_time >= attack.hit_times[static_cast<std::size_t>(enemy.hit_count)])
            {
                if (std::abs(to_player.y) < 1.2F)
                {
                    const float squared = to_player.x * to_player.x + to_player.z * to_player.z;
                    bool landed = squared < 0.05F;
                    if (!landed && squared < attack.range * attack.range)
                    {
                        const float inverse_distance = 1.0F / std::sqrt(squared);
                        const float target_x = to_player.x * inverse_distance;
                        const float target_z = to_player.z * inverse_distance;
                        const float forward_x = std::sin(enemy.yaw);
                        const float forward_z = -std::cos(enemy.yaw);
                        const float facing = (target_x * forward_x + target_z * forward_z) *
                                             attacks.hit_dot_multiplier;
                        landed = facing > 0.7F;
                    }
                    if (landed && local_target)
                    {
                        events.player_damage += attack.damage;
                        events.player_damage_sources.push_back(enemy.position);
                    }
                }
                ++enemy.hit_count;
            }
            if (enemy.clip_time >= clip_duration(enemy, enemy.clip))
            {
                const float distance3 =
                    std::sqrt(to_player.x * to_player.x + to_player.y * to_player.y +
                              to_player.z * to_player.z);
                if (distance3 >= 1.0F)
                    begin_state(enemy, EnemyState::chase, player);
                else
                {
                    begin_state(enemy, EnemyState::attack, player);
                    ++events.attacks_started;
                }
            }
            break;
        }
        case EnemyState::idle:
            enemy.velocity.x = 0.0F;
            enemy.velocity.z = 0.0F;
            if (enemy.state_time >= enemy.state_duration)
                begin_state(enemy, EnemyState::chase, player);
            break;
        case EnemyState::hit:
            enemy.velocity.x = 0.0F;
            enemy.velocity.z = 0.0F;
            if (enemy.clip_time >= std::max(0.0F, clip_duration(enemy, enemy.clip) - 0.25F))
                begin_state(enemy, EnemyState::chase, player);
            break;
        case EnemyState::die:
            enemy.velocity.x = 0.0F;
            enemy.velocity.z = 0.0F;
            // EnemyManager handles the kill message (and therefore the local
            // loot roll) immediately; the nonblocking corpse remains for five
            // seconds after its death animation finishes.
            if (enemy.loot_pending)
            {
                enemy.loot_pending = false;
                const float origin_distance = std::sqrt(enemy.position.x * enemy.position.x +
                                                        enemy.position.z * enemy.position.z);
                const EnemyLoot loot =
                    enemy_loot(enemy.type, origin_distance, enemy.position.y, rng_);
                if (loot.item_count > 0U)
                {
                    events.drops.push_back(loot);
                    events.drop_positions.push_back(
                        {enemy.position.x, enemy.position.y + 1.0F, enemy.position.z});
                }
            }
            if (enemy.state_time >=
                clip_duration(enemy, enemy.clip) / std::max(0.001F, stats.die_animation_speed) +
                    5.0F)
            {
                ++events.died;
                enemy.state = EnemyState::despawn;
            }
            break;
        case EnemyState::give_up:
            enemy.velocity.x = 0.0F;
            enemy.velocity.z = 0.0F;
            // Every *GiveUp state inherits ZombieGiveUp.Update: play the
            // class-specific non-looping clip once, then remove the entity.
            if (enemy.clip_time >= clip_duration(enemy, enemy.clip))
                enemy.state = EnemyState::despawn;
            break;
        case EnemyState::dig: {
            // ZombieDig 1:1: swings of the melee clip chew a 2x3x2 block box
            // biased toward the player. Every swing that touches solid plays
            // the loud ZombieDig cue; a block breaks once the accumulated
            // swings exceed its hardness, with the original's 50% roll.
            if (!enemy.dig_valid)
            {
                enemy.dig_valid = true;
                enemy.swing_count = 0;
                enemy.dig_progress = 0.0F;
                // Enter: a target already 16+ blocks away means give up.
                const float target_range_squared = to_player.x * to_player.x +
                                                   to_player.y * to_player.y +
                                                   to_player.z * to_player.z;
                if (target_range_squared > 256.0F)
                {
                    begin_state(enemy, EnemyState::give_up, player);
                    break;
                }
            }
            enemy.velocity.x = 0.0F;
            enemy.velocity.z = 0.0F;
            if (flat_distance > 0.1F)
                enemy.yaw = std::atan2(to_player.x, -to_player.z);
            // attack clips land their dig hit ~0.83s in at animation speed 2.
            constexpr float swing_period = 0.8333F / 2.0F;
            enemy.dig_progress += delta_seconds;
            if (enemy.dig_progress < swing_period)
                break;
            enemy.dig_progress -= swing_period;
            ++enemy.swing_count;
            // The original's 2x3x2 target volume, biased toward the player.
            int base_x = static_cast<int>(std::floor(enemy.position.x));
            int base_y = static_cast<int>(std::floor(enemy.position.y));
            int base_z = static_cast<int>(std::floor(enemy.position.z));
            if (player.position.y >= enemy.position.y + 1.0F)
                ++base_y;
            else if (player.position.y <= enemy.position.y - 1.0F)
                --base_y;
            int min_x = base_x, max_x = base_x;
            int min_z = base_z, max_z = base_z;
            if (player.position.x > enemy.position.x)
                ++max_x;
            else
                --min_x;
            if (player.position.z >= enemy.position.z)
                ++max_z;
            else
                --min_z;
            const int hits =
                static_cast<int>(static_cast<float>(enemy.swing_count) * stats.digging_multiplier);
            bool touched_blocking = false;
            bool any_diggable = false;
            bool any_too_hard = false;
            for (int y = base_y; y <= base_y + 2; ++y)
                for (int z = min_z; z <= max_z; ++z)
                    for (int x = min_x; x <= max_x; ++x)
                    {
                        if (!region.contains(x, y, z))
                            continue;
                        const BlockType block = region.block(x, y, z);
                        const int hardness = enemy_dig_hardness(block);
                        if (hardness == 0)
                            continue;
                        if (blocks_player(block))
                            touched_blocking = true;
                        if (hardness > stats.hardest_block_dug)
                        {
                            any_too_hard = true;
                            continue;
                        }
                        any_diggable = true;
                        // Explosive.EnemyBreakBlocks: hits > hardness, then a
                        // coin flip per swing.
                        if (hits > hardness && rng_.next01() < 0.5F)
                        {
                            // Upper halves report a successful swing but are
                            // deliberately skipped; the lower half owns both.
                            if (local_target && !is_upper_door(block))
                                events.dug_blocks.push_back({x, y, z});
                        }
                        else if (local_target &&
                                 (block == BlockType::tnt || block == BlockType::c4))
                        {
                            events.fuse_blocks.push_back({x, y, z});
                        }
                    }
            if (touched_blocking)
                events.dig_swings.push_back(enemy.position);
            // ZombieDig exit table (checked at each swing end).
            if (!touched_blocking)
            {
                begin_state(enemy, EnemyState::chase, player);
            }
            else if (!any_diggable && any_too_hard)
            {
                begin_state(enemy, EnemyState::give_up, player);
            }
            else if (std::abs(to_player.y) > 8.0F || (flat_distance > 16.0F))
            {
                begin_state(enemy, EnemyState::give_up, player);
            }
            else if (flat_distance < 1.0F && std::abs(to_player.y) < 1.5F)
            {
                begin_state(enemy, EnemyState::attack, player);
                ++events.attacks_started;
            }
            break;
        }
        case EnemyState::despawn:
        default:
            break;
        }
        if (enemy.state != EnemyState::despawn)
            move_enemy(region, enemy, delta_seconds);
        if (local_target && initial_state != EnemyState::give_up &&
            enemy.state == EnemyState::give_up)
            events.give_up_requests.push_back({enemy.target_gid, enemy.id});
    }
    std::erase_if(enemies_, [](const Enemy &enemy) { return enemy.state == EnemyState::despawn; });
    return events;
}

} // namespace openminerz::world
