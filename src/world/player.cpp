#include "openminerz/world/player.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace openminerz::world
{

float water_depth(float feet_y) noexcept
{
    return std::max(water_level - feet_y, 0.0F);
}

bool is_underwater(bool water_world, float feet_y) noexcept
{
    return water_world && water_depth(feet_y) >= 1.5F;
}

float lava_submersion(float feet_y) noexcept
{
    return std::clamp((-60.0F - feet_y) / 2.0F, 0.0F, 1.0F);
}

bool is_under_lava(float feet_y) noexcept
{
    return lava_submersion(feet_y) >= 1.0F;
}

namespace
{

constexpr float pi = 3.14159265358979323846F;
constexpr float collision_epsilon = 0.001F;

float length_squared(Vec3 value) noexcept
{
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

Vec3 normalize(Vec3 value) noexcept
{
    const float length = std::sqrt(length_squared(value));
    if (length <= 0.000001F)
    {
        return {};
    }
    return {value.x / length, value.y / length, value.z / length};
}

int floor_to_int(float value) noexcept
{
    return static_cast<int>(std::floor(value));
}

bool unbreakable(BlockType type) noexcept
{
    return type == BlockType::bedrock || type == BlockType::fixed_lantern;
}

} // namespace

const PlayerState &PlayerSimulation::state() const noexcept
{
    return state_;
}

PlayerState &PlayerSimulation::state() noexcept
{
    return state_;
}

void PlayerSimulation::spawn(const WorldRegion &world, int near_x, int near_z)
{
    int spawn_x = std::clamp(near_x, world.minimum_x() + 2, world.maximum_x() - 2);
    int spawn_z = std::clamp(near_z, world.minimum_z() + 2, world.maximum_z() - 2);
    int surface = world_min_y;
    bool found{};
    for (int radius = 0; radius <= 10 && !found; ++radius)
    {
        for (int dz = -radius; dz <= radius && !found; ++dz)
        {
            for (int dx = -radius; dx <= radius && !found; ++dx)
            {
                if (radius > 0 && std::abs(dx) != radius && std::abs(dz) != radius)
                {
                    continue;
                }
                const int x = std::clamp(near_x + dx, world.minimum_x() + 2, world.maximum_x() - 2);
                const int z = std::clamp(near_z + dz, world.minimum_z() + 2, world.maximum_z() - 2);
                for (int y = world_min_y + chunk_height - 2; y >= world_min_y; --y)
                {
                    const BlockType ground = world.block(x, y, z);
                    if (!blocks_player(ground) || ground == BlockType::fixed_lantern)
                    {
                        continue;
                    }
                    const Vec3 candidate{static_cast<float>(x) + 0.5F,
                                         static_cast<float>(y) + 1.001F,
                                         static_cast<float>(z) + 0.5F};
                    if (!touches_solid(world, candidate))
                    {
                        spawn_x = x;
                        spawn_z = z;
                        surface = y;
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    state_.position = {static_cast<float>(spawn_x) + 0.5F, static_cast<float>(surface) + 1.001F,
                       static_cast<float>(spawn_z) + 0.5F};
    state_.velocity = {};
    state_.yaw = 0.0F;
    state_.pitch = -0.08F;
    state_.step_view_offset = 0.0F;
    state_.health = 100.0F;
    state_.stamina = 100.0F;
    state_.sprinting = false;
    state_.grounded = true;
    jump_was_down_ = false;
    jump_count_ = 0;
    stamina_recovery_delay_ = 0.0F;
    stamina_block_time_ = 0.0F;
    cancel_breaking();
    refresh_target(world);
}

void PlayerSimulation::update(const WorldRegion &world, const PlayerInput &input,
                              float fixed_delta_seconds, bool auto_climb, bool water_world,
                              float creative_flight_speed)
{
    update(world, input, fixed_delta_seconds, auto_climb, water_world,
           creative_flight_speed, false);
}

void PlayerSimulation::update(const WorldRegion &world, const PlayerInput &input,
                              float fixed_delta_seconds, bool auto_climb, bool water_world,
                              float creative_flight_speed, bool modern_double_jump)
{
    const float step_recovery = auto_climb_visual_speed * fixed_delta_seconds;
    if (state_.step_view_offset < 0.0F)
    {
        state_.step_view_offset = std::min(0.0F, state_.step_view_offset + step_recovery);
    }
    else if (state_.step_view_offset > 0.0F)
    {
        state_.step_view_offset = std::max(0.0F, state_.step_view_offset - step_recovery);
    }
    Vec3 movement{input.strafe, 0.0F, input.forward};
    // FPSRig.UpdateVelocity writes each keyboard axis independently, so the
    // original preserves its faster diagonal. Player.ProcessInput blends the
    // five-unit surface Speed to 1.894 at full submersion.
    const float sine = std::sin(state_.yaw);
    const float cosine = std::cos(state_.yaw);
    const float depth = water_world ? water_depth(state_.position.y) : 0.0F;
    const float submerged = std::clamp(depth, 0.0F, 1.0F);
    const bool in_water = water_world && depth > 0.0F;
    stamina_recovery_delay_ = std::max(0.0F, stamina_recovery_delay_ - fixed_delta_seconds);
    stamina_block_time_ = std::max(0.0F, stamina_block_time_ - fixed_delta_seconds);
    state_.sprinting = false;
    const bool moving = input.strafe != 0.0F || input.forward != 0.0F;
    const float sprint_cost = sprint_stamina_cost_per_second * fixed_delta_seconds;
    if (!state_.fly_mode && moving && input.sprint && stamina_block_time_ <= 0.0F &&
        sprint_cost <= state_.stamina)
    {
        state_.stamina -= sprint_cost;
        stamina_recovery_delay_ = stamina_recovery_delay_seconds;
        if (state_.stamina <= 1.0F)
            stamina_block_time_ = stamina_exhaustion_block_seconds;
        state_.sprinting = true;
    }
    if (state_.fly_mode)
    {
        // Player._flyMode: gravity off, jump climbs, the descend key sinks,
        // and the cruise is brisker than walking.
        const float configured_speed = std::clamp(creative_flight_speed, 4.0F, 32.0F);
        state_.velocity.x = (movement.x * cosine + movement.z * sine) * configured_speed;
        state_.velocity.z = (movement.x * sine - movement.z * cosine) * configured_speed;
        const float vertical = (input.jump ? 1.0F : 0.0F) - (input.descend ? 1.0F : 0.0F);
        state_.velocity.y = vertical * configured_speed * (fly_vertical_speed / fly_speed);
        jump_was_down_ = input.jump;
    }
    else
    {
        const float surface_speed =
            state_.sprinting ? movement_speed * sprint_multiplier : movement_speed;
        const float speed = std::lerp(surface_speed, underwater_speed, submerged);
        state_.velocity.x = (movement.x * cosine + movement.z * sine) * speed;
        state_.velocity.z = (movement.x * sine - movement.z * cosine) * speed;
        if (state_.grounded)
            jump_count_ = 0;
        const bool jump_pressed = input.jump && !jump_was_down_;
        const bool airborne_jump = !state_.grounded && modern_double_jump && jump_count_ < 2;
        const float jump_cost = jump_count_ < 1 ? 0.0F : modern_air_jump_stamina_cost;
        if (jump_pressed && !in_water && (state_.grounded || airborne_jump) &&
            jump_cost <= state_.stamina)
        {
            state_.velocity.y += jump_impulse;
            state_.stamina -= jump_cost;
            if (jump_cost > 0.0F)
                stamina_recovery_delay_ = stamina_recovery_delay_seconds;
            ++jump_count_;
            state_.grounded = false;
            state_.step_view_offset = 0.0F;
        }
        jump_was_down_ = input.jump;
        float vertical_acceleration = std::lerp(gravity, underwater_up_acceleration, submerged);
        if (in_water)
        {
            // A held control is required for deliberate swimming. Buoyancy and
            // drag remain depth-scaled, while Space and descend add a symmetric
            // axis that works for as long as the player holds it.
            const float swim_axis = (input.jump ? 1.0F : 0.0F) - (input.descend ? 1.0F : 0.0F);
            vertical_acceleration += swim_axis * underwater_control_acceleration * submerged;
            if (swim_axis != 0.0F)
                state_.grounded = false;
        }
        state_.velocity.y += vertical_acceleration * fixed_delta_seconds;
        if (in_water)
        {
            state_.velocity.y = std::clamp(state_.velocity.y, -underwater_vertical_speed,
                                           underwater_vertical_speed);
        }
    }
    move_and_collide(world, fixed_delta_seconds, auto_climb);
    if (in_water)
    {
        // Player.OnUpdate applies depth-scaled drag after this frame's scene
        // move, so it shapes the next vertical step without changing the
        // distance already travelled this tick.
        const float damping =
            std::max(0.0F, 1.0F - underwater_drag * submerged * fixed_delta_seconds);
        state_.velocity.x *= damping;
        state_.velocity.y *= damping;
        state_.velocity.z *= damping;
    }
    if (stamina_recovery_delay_ <= 0.0F)
        state_.stamina =
            std::min(100.0F, state_.stamina + fixed_delta_seconds * stamina_recovery_per_second);
    refresh_target(world);
}

void PlayerSimulation::look(float horizontal_radians, float vertical_radians) noexcept
{
    state_.yaw += horizontal_radians;
    if (state_.yaw > pi)
    {
        state_.yaw -= pi * 2.0F;
    }
    else if (state_.yaw < -pi)
    {
        state_.yaw += pi * 2.0F;
    }
    state_.pitch = std::clamp(state_.pitch + vertical_radians, -pi * 0.494444F, pi * 0.494444F);
}

Vec3 PlayerSimulation::view_direction() const noexcept
{
    const float cosine_pitch = std::cos(state_.pitch);
    return {std::sin(state_.yaw) * cosine_pitch, std::sin(state_.pitch),
            -std::cos(state_.yaw) * cosine_pitch};
}

void PlayerSimulation::refresh_target(const WorldRegion &world)
{
    const Vec3 eye{state_.position.x, state_.position.y + eye_height, state_.position.z};
    state_.target = raycast(world, eye, view_direction(), interaction_reach);
}

bool PlayerSimulation::mine_target(WorldRegion &world)
{
    if (!state_.target.has_value() || unbreakable(state_.target->block))
    {
        return false;
    }
    for (const BlockRemovalEffect &effect :
         world.removal_effects(state_.target->x, state_.target->y, state_.target->z))
        world.set_block(effect.x, effect.y, effect.z, BlockType::empty);
    refresh_target(world);
    return true;
}

bool PlayerSimulation::place_target(WorldRegion &world)
{
    if (!state_.target.has_value() || world.block(state_.target->place_x, state_.target->place_y,
                                                  state_.target->place_z) != BlockType::empty)
    {
        return false;
    }
    world.set_block(state_.target->place_x, state_.target->place_y, state_.target->place_z,
                    state_.selected_block);
    if (touches_solid(world, state_.position))
    {
        world.set_block(state_.target->place_x, state_.target->place_y, state_.target->place_z,
                        BlockType::empty);
        return false;
    }
    refresh_target(world);
    return true;
}

std::optional<BlockBreakUpdate> PlayerSimulation::update_breaking(WorldRegion &world, bool held,
                                                                  float delta_seconds,
                                                                  float impact_period_seconds,
                                                                  std::uint8_t action_key,
                                                                  float dig_seconds)
{
    const float delta = std::max(0.0F, delta_seconds);
    // Vanilla stores this OneShotTimer on the equipped InventoryItem and updates it even while
    // the construction probe is not over a block. It is deliberately independent of DigLocation:
    // sweeping the crosshair across adjacent blocks must not manufacture a hit on every block.
    const std::size_t cooldown_index =
        std::min<std::size_t>(action_key, impact_cooldowns_.size() - 1U);
    float &impact_cooldown = impact_cooldowns_[cooldown_index];
    impact_cooldown = std::max(0.0F, impact_cooldown - delta);
    if (!held || !state_.target.has_value() || unbreakable(state_.target->block))
    {
        cancel_breaking();
        return std::nullopt;
    }

    const auto &target = *state_.target;
    const BlockCoordinate coordinate{target.x, target.y, target.z};
    if (!breaking_target_.has_value() || *breaking_target_ != coordinate)
    {
        breaking_target_ = coordinate;
        breaking_seconds_ = 0.0F;
    }

    const BlockType block = world.block(coordinate.x, coordinate.y, coordinate.z);
    if (block == BlockType::empty || block == BlockType::uninitialized)
    {
        cancel_breaking();
        return std::nullopt;
    }

    state_.using_tool = true;
    breaking_seconds_ += delta;
    action_animation_seconds_ += delta;
    const float impact_period = std::max(0.01F, impact_period_seconds);
    state_.tool_swing_progress =
        std::fmod(std::max(0.0F, action_animation_seconds_), impact_period) / impact_period;
    const float duration = dig_seconds >= 0.0F ? dig_seconds : bare_hand_break_seconds(block);
    state_.break_progress = std::isfinite(duration) && duration > 0.0F
                                ? std::clamp(breaking_seconds_ / duration, 0.0F, 1.0F)
                                : 0.0F;

    const Vec3 direction = view_direction();
    const Vec3 eye{state_.position.x, state_.position.y + eye_height, state_.position.z};
    BlockBreakUpdate update{coordinate.x,
                            coordinate.y,
                            coordinate.z,
                            block,
                            {eye.x + direction.x * target.distance,
                             eye.y + direction.y * target.distance,
                             eye.z + direction.z * target.distance},
                            target.normal_x,
                            target.normal_y,
                            target.normal_z};
    if (impact_cooldown <= 0.0F)
    {
        update.impact = true;
        impact_cooldown = impact_period;
    }
    if (std::isfinite(duration) && (duration <= 0.0F || breaking_seconds_ >= duration))
    {
        update.removed_blocks = world.removal_effects(coordinate.x, coordinate.y, coordinate.z);
        for (const BlockRemovalEffect &effect : update.removed_blocks)
            world.set_block(effect.x, effect.y, effect.z, BlockType::empty);
        update.completed = true;
        // Damage belongs to a voxel, but the held item's swing belongs to the input action.
        // Keep that visual phase across the newly exposed/aimed block just like vanilla's
        // InventoryItem OneShotTimer; only releasing the action restarts the animation.
        breaking_target_.reset();
        breaking_seconds_ = 0.0F;
        state_.break_progress = 0.0F;
        state_.using_tool = true;
        refresh_target(world);
    }
    return update.impact || update.completed ? std::optional{update} : std::nullopt;
}

void PlayerSimulation::cancel_breaking() noexcept
{
    breaking_target_.reset();
    breaking_seconds_ = 0.0F;
    action_animation_seconds_ = 0.0F;
    state_.using_tool = false;
    state_.break_progress = 0.0F;
    state_.tool_swing_progress = 0.0F;
}

std::optional<VoxelRaycastHit> PlayerSimulation::raycast(const WorldRegion &world, Vec3 origin,
                                                         Vec3 direction, float reach)
{
    return raycast_internal(world, origin, direction, reach, false);
}

std::optional<VoxelRaycastHit> PlayerSimulation::raycast_blocking(const WorldRegion &world,
                                                                  Vec3 origin, Vec3 direction,
                                                                  float reach)
{
    return raycast_internal(world, origin, direction, reach, true);
}

std::optional<VoxelRaycastHit> PlayerSimulation::raycast_internal(const WorldRegion &world,
                                                                  Vec3 origin, Vec3 direction,
                                                                  float reach, bool blocking_only)
{
    direction = normalize(direction);
    if (length_squared(direction) < 0.5F)
    {
        return std::nullopt;
    }
    int x = floor_to_int(origin.x);
    int y = floor_to_int(origin.y);
    int z = floor_to_int(origin.z);
    int previous_x = x;
    int previous_y = y;
    int previous_z = z;
    const int step_x = direction.x < 0.0F ? -1 : 1;
    const int step_y = direction.y < 0.0F ? -1 : 1;
    const int step_z = direction.z < 0.0F ? -1 : 1;
    const float infinity = std::numeric_limits<float>::infinity();
    const float delta_x =
        std::abs(direction.x) > 0.000001F ? std::abs(1.0F / direction.x) : infinity;
    const float delta_y =
        std::abs(direction.y) > 0.000001F ? std::abs(1.0F / direction.y) : infinity;
    const float delta_z =
        std::abs(direction.z) > 0.000001F ? std::abs(1.0F / direction.z) : infinity;
    float max_x = std::abs(direction.x) > 0.000001F
                      ? ((step_x > 0 ? static_cast<float>(x + 1) - origin.x
                                     : origin.x - static_cast<float>(x)) /
                         std::abs(direction.x))
                      : infinity;
    float max_y = std::abs(direction.y) > 0.000001F
                      ? ((step_y > 0 ? static_cast<float>(y + 1) - origin.y
                                     : origin.y - static_cast<float>(y)) /
                         std::abs(direction.y))
                      : infinity;
    float max_z = std::abs(direction.z) > 0.000001F
                      ? ((step_z > 0 ? static_cast<float>(z + 1) - origin.z
                                     : origin.z - static_cast<float>(z)) /
                         std::abs(direction.z))
                      : infinity;
    float distance{};
    int normal_x{};
    int normal_y{};
    int normal_z{};
    while (distance <= reach)
    {
        const BlockType type = world.block(x, y, z);
        if (is_solid(type) && (!blocking_only || blocks_player(type)))
        {
            return VoxelRaycastHit{x,        y,        z,        previous_x, previous_y, previous_z,
                                   normal_x, normal_y, normal_z, distance,   type};
        }
        previous_x = x;
        previous_y = y;
        previous_z = z;
        normal_x = 0;
        normal_y = 0;
        normal_z = 0;
        if (max_x <= max_y && max_x <= max_z)
        {
            x += step_x;
            distance = max_x;
            max_x += delta_x;
            normal_x = -step_x;
        }
        else if (max_y <= max_z)
        {
            y += step_y;
            distance = max_y;
            max_y += delta_y;
            normal_y = -step_y;
        }
        else
        {
            z += step_z;
            distance = max_z;
            max_z += delta_z;
            normal_z = -step_z;
        }
    }
    return std::nullopt;
}

bool PlayerSimulation::touches_solid(const WorldRegion &world, Vec3 feet_position) noexcept
{
    const int minimum_x = floor_to_int(feet_position.x - body_half_width + collision_epsilon);
    const int maximum_x = floor_to_int(feet_position.x + body_half_width - collision_epsilon);
    const int minimum_y = floor_to_int(feet_position.y + collision_epsilon);
    const int maximum_y = floor_to_int(feet_position.y + body_height - collision_epsilon);
    const int minimum_z = floor_to_int(feet_position.z - body_half_width + collision_epsilon);
    const int maximum_z = floor_to_int(feet_position.z + body_half_width - collision_epsilon);
    for (int y = minimum_y; y <= maximum_y; ++y)
    {
        for (int z = minimum_z; z <= maximum_z; ++z)
        {
            for (int x = minimum_x; x <= maximum_x; ++x)
            {
                if (blocks_player(world.block(x, y, z)))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// Port of the movement probe's SimulateSlopedSides gate: a horizontal
// collision counts as a climbable slope only when the obstruction sits at
// feet level (nothing solid blocks the candidate box above the feet band) and
// the cell above the player's own feet is open, exactly like BruteForceTrace's
// feet-band bookkeeping plus SlopingIsBlocked.
bool PlayerSimulation::can_slope_slide(const WorldRegion &world, Vec3 candidate) const noexcept
{
    const int above_feet_y = floor_to_int(state_.position.y + 0.01F) + 1;
    if (blocks_player(world.block(floor_to_int(state_.position.x), above_feet_y,
                                  floor_to_int(state_.position.z))))
    {
        return false;
    }
    const int band_top = floor_to_int(state_.position.y + 0.2F);
    const int minimum_x = floor_to_int(candidate.x - body_half_width + collision_epsilon);
    const int maximum_x = floor_to_int(candidate.x + body_half_width - collision_epsilon);
    const int maximum_y = floor_to_int(candidate.y + body_height - collision_epsilon);
    const int minimum_z = floor_to_int(candidate.z - body_half_width + collision_epsilon);
    const int maximum_z = floor_to_int(candidate.z + body_half_width - collision_epsilon);
    for (int y = band_top + 1; y <= maximum_y; ++y)
    {
        for (int z = minimum_z; z <= maximum_z; ++z)
        {
            for (int x = minimum_x; x <= maximum_x; ++x)
            {
                if (blocks_player(world.block(x, y, z)))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

void PlayerSimulation::move_and_collide(const WorldRegion &world, float delta_seconds,
                                        bool auto_climb)
{
    // Vanilla Player.cs: block sides act as slopes whenever AutoClimb is on
    // and vertical speed is below half the jump impulse. That is what carries
    // a 2.5-block jump onto a third block and walks you up single steps.
    const bool slide_allowed = auto_climb && state_.velocity.y < jump_impulse * 0.5F;
    bool sliding{};

    Vec3 candidate = state_.position;
    candidate.x += state_.velocity.x * delta_seconds;
    if (touches_solid(world, candidate))
    {
        if (slide_allowed && can_slope_slide(world, candidate))
        {
            sliding = true;
        }
        else
        {
            state_.velocity.x = 0.0F;
        }
        candidate.x = state_.position.x;
    }
    state_.position = candidate;

    candidate = state_.position;
    candidate.z += state_.velocity.z * delta_seconds;
    if (touches_solid(world, candidate))
    {
        if (slide_allowed && can_slope_slide(world, candidate))
        {
            sliding = true;
        }
        else
        {
            state_.velocity.z = 0.0F;
        }
        candidate.z = state_.position.z;
    }
    state_.position = candidate;

    if (sliding)
    {
        // The sloped block lifts at five units per second, clamps to its top
        // and counts as ground contact so a mid-slide jump works.
        const float band_target =
            static_cast<float>(floor_to_int(state_.position.y + 0.2F) + 1) + 0.001F;
        Vec3 lifted = state_.position;
        lifted.y = std::min(state_.position.y + auto_climb_visual_speed * delta_seconds,
                            std::max(state_.position.y, band_target));
        if (!touches_solid(world, lifted))
        {
            state_.position = lifted;
        }
        state_.velocity.y = 0.0F;
        state_.grounded = true;
        return;
    }

    state_.grounded = false;
    candidate = state_.position;
    candidate.y += state_.velocity.y * delta_seconds;
    if (touches_solid(world, candidate))
    {
        if (state_.velocity.y < 0.0F)
        {
            state_.grounded = true;
        }
        state_.velocity.y = 0.0F;
    }
    else
    {
        state_.position = candidate;
    }
    if (!state_.grounded)
    {
        Vec3 probe = state_.position;
        probe.y -= 0.025F;
        state_.grounded = touches_solid(world, probe);
    }
}

} // namespace openminerz::world
