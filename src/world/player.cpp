#include "openminerz/world/player.hpp"

#include "openminerz/world/liquid.hpp" // mod-liquid
#include "openminerz/world/movement_probe.hpp"

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
    // (mod-liquid) A column under a lake is no place to start while a dry one
    // is in reach; with none, the lake bed is (the player floats up).
    const bool liquids = active_liquids() != nullptr;
    for (int pass = liquids ? 0 : 1; pass < 2 && !found; ++pass)
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
                    if (pass == 0 && liquid_at(world, x, y, z))
                        break;
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
    stamina_recover_ticks_ = stamina_timer_ceiling;
    stamina_block_ticks_ = stamina_timer_ceiling;
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
    // (mod-liquid) A mod liquid is swum in exactly like the Lagoon's water,
    // measured from its own surface instead of the sea plane.
    const float liquid =
        liquid_depth(world, state_.position.x, state_.position.y, state_.position.z);
    const float depth = std::max(water_world ? water_depth(state_.position.y) : 0.0F, liquid);
    const float submerged = std::clamp(depth, 0.0F, 1.0F);
    const bool in_water = depth > 0.0F;
    touched_liquid_ = liquid > 0.0F;
    state_.sprinting = false;
    const bool moving = input.strafe != 0.0F || input.forward != 0.0F;
    // 1.9.9.8 Player.ProcessInput: a movement key and the sprint key held, and
    // InGameHUD.CheckAndUseStamina(GetSprintCost() * dt) granted.
    // The same test runs in `_flyMode`: 1.9.9.8 sprints in the air too, and
    // pays for it. Only the Free flight extra has no sprint.
    const bool retail_flight = state_.fly_mode && flight_model_ == FlightModel::retail;
    if ((!state_.fly_mode || retail_flight) && moving && input.sprint &&
        spend_stamina(sprint_stamina_cost_per_second * fixed_delta_seconds))
        state_.sprinting = true;
    if (retail_flight)
    {
        // 1.9.9.8 Player.UpdateAnimation, executed in the shipped client by
        // SimulationParityProbe (fly_creative, _sprint, _dive):
        //     v  = camera.Right   * (strafe * 4 * 6)
        //     v += camera.Forward * (-num4 * 6)
        //     if (Jump.Held) v.Y += JumpImpulse * 1.5
        // where num4 is the SAME gait speed the ground uses: 4 at a full
        // stick (8 sprinting), and below 0.8 the stroll formula, which is
        // evaluated for a zero stick too -- a pure strafe drifts 1.42 back
        // along the camera. No input is no velocity; gravity is off (the
        // caller's Player.OnUpdate zeroes WorldAcceleration); and Player.Jump's
        // own impulse is overwritten by this in the same frame.
        const float walk = input.forward;
        const float strafe = input.strafe;
        const float amount = std::abs(walk);
        float along{};
        float across{};
        if (std::max(amount, std::abs(strafe)) >= 0.1F)
        {
            const float gait = state_.sprinting ? sprint_multiplier : 1.0F;
            const float forward_speed = amount < 0.8F
                                            ? (amount - 0.1F) / 0.4F * (ground_stroll_speed * gait)
                                            : amount * (ground_run_speed * gait);
            along = walk < 0.0F ? -forward_speed : forward_speed;
            across = strafe * ground_run_speed;
        }
        const Vec3 forward = view_direction();
        state_.velocity.x =
            (cosine * across + forward.x * along) * retail_fly_multiplier;
        state_.velocity.y = forward.y * along * retail_fly_multiplier;
        state_.velocity.z = (sine * across + forward.z * along) * retail_fly_multiplier;
        if (input.jump)
            state_.velocity.y += retail_fly_rise_speed;
        jump_was_down_ = input.jump;
    }
    else if (state_.fly_mode)
    {
        // The Free flight extra: gravity off, jump climbs, the descend key
        // sinks, and the heading alone steers at the configured speed.
        const float configured_speed = std::clamp(creative_flight_speed, 4.0F, 32.0F);
        state_.velocity.x = (movement.x * cosine + movement.z * sine) * configured_speed;
        state_.velocity.z = (movement.x * sine - movement.z * cosine) * configured_speed;
        const float vertical = (input.jump ? 1.0F : 0.0F) - (input.descend ? 1.0F : 0.0F);
        state_.velocity.y = vertical * configured_speed * (fly_vertical_speed / fly_speed);
        jump_was_down_ = input.jump;
    }
    else
    {
        if (in_water)
        {
            const float surface_speed =
                state_.sprinting ? movement_speed * sprint_multiplier : movement_speed;
            const float speed = std::lerp(surface_speed, underwater_speed, submerged);
            state_.velocity.x = (movement.x * cosine + movement.z * sine) * speed;
            state_.velocity.z = (movement.x * sine - movement.z * cosine) * speed;
        }
        else
        {
            // Player.ProcessInput, executed in the shipped client by
            // tools/OriginalClientInterop/SimulationParityProbe. Two writers
            // run every frame:
            //   1. FPSRig.UpdateVelocity: Speed (5) on each axis.
            //   2. Player.UpdateAnimation, which OVERWRITES that whenever the
            //      player was in contact with the ground last frame: strafe at
            //      4, forward or back at 4 (a stick under 0.8 strolls at up to
            //      1.66; 0.947 scaled by (stick - 0.1) / 0.4).
            // So a player RUNS at 4 and its diagonal is 5.66, not 5 and 7.07.
            // In 1.6.3.8 the override has no airborne branch, so a jump
            // carries the rig's 5; 1.9.9.8 added one (4, or 8 sprinting).
            // The stroll formula is evaluated for a zero stick too: a pure
            // strafe drifts BACKWARD at 0.237. Sprinting doubles 4 and 0.947.
            const float walk = input.forward;
            const float strafe = input.strafe;
            const float amount = std::abs(walk);
            float local_x = strafe * movement_speed;
            float local_z = -walk * movement_speed;
            if (std::max(amount, std::abs(strafe)) < 0.1F)
            {
                local_x = 0.0F;
                local_z = 0.0F;
            }
            else if (state_.grounded || modern_double_jump)
            {
                const float gait = state_.sprinting ? sprint_multiplier : 1.0F;
                const float forward_speed =
                    state_.grounded && amount < 0.8F
                        ? (amount - 0.1F) / 0.4F * (ground_stroll_speed * gait)
                        : amount * (ground_run_speed * gait);
                local_x = strafe * ground_run_speed;
                local_z = walk < 0.0F ? forward_speed : -forward_speed;
            }
            // The rig's local axes (right, back) turned to the heading.
            state_.velocity.x = local_x * cosine - local_z * sine;
            state_.velocity.z = local_x * sine + local_z * cosine;
        }
        if (state_.grounded)
            jump_count_ = 0;
        const bool jump_pressed = input.jump && !jump_was_down_;
        // FPSRig.CanJump: in contact, or (1.9.9.8) fewer than JumpCountLimit
        // jumps since the last contact. Player.CanJump then charges stamina:
        // nothing for the first jump of a flight, a quarter of the bar for
        // the next, refused like any other spend while exhausted.
        const bool airborne_jump = !state_.grounded && modern_double_jump && jump_count_ < 2;
        if (jump_pressed && !in_water && (state_.grounded || airborne_jump) &&
            spend_stamina(jump_count_ < 1 ? 0.0F : modern_air_jump_stamina_cost))
        {
            state_.velocity.y += jump_impulse;
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
        // (mod-liquid) Out of a lake: Jump held against its bank near the
        // surface lifts the player over the edge, as in Minecraft. The
        // Lagoon's open sea never needed it, so only mod liquids do this.
        if (liquid > 0.0F && liquid < liquid_climb_depth && input.jump && side_contact_)
            state_.velocity.y = std::max(state_.velocity.y, liquid_climb_speed);
    }
    move_and_collide(world, fixed_delta_seconds, auto_climb);
    // (mod-liquid) Whether the body ended this tick in a liquid: a fall that
    // lands in one, however shallow, deals no damage (the app asks).
    if (!touched_liquid_ && active_liquids() != nullptr)
    {
        const int cell_x = floor_to_int(state_.position.x);
        const int cell_z = floor_to_int(state_.position.z);
        for (int y = floor_to_int(state_.position.y); y <= floor_to_int(state_.position.y + body_height);
             ++y)
            touched_liquid_ = touched_liquid_ || liquid_at(world, cell_x, y, cell_z);
    }
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
    // InGameHUD.OnUpdate, which runs after the player has moved: both
    // OneShotTimers advance (in 100 ns ticks, as TimeSpan does), then stamina
    // refills at a quarter of the bar a second once a full second has passed
    // since it was last spent.
    const auto tick_step =
        static_cast<std::int64_t>(std::llround(static_cast<double>(fixed_delta_seconds) * 1e7));
    stamina_block_ticks_ = std::min(stamina_block_ticks_ + tick_step, stamina_timer_ceiling);
    stamina_recover_ticks_ = std::min(stamina_recover_ticks_ + tick_step, stamina_timer_ceiling);
    if (state_.stamina < 100.0F && stamina_recover_ticks_ >= stamina_recover_timer_ticks)
        state_.stamina =
            std::min(100.0F, state_.stamina + fixed_delta_seconds * stamina_recovery_per_second);
    refresh_target(world);
}

// InGameHUD.CheckAndUseStamina. A zero cost always passes; anything else needs
// stamina to be usable at all (never in Endurance), the two-second exhaustion
// block to have run out, and the bar to cover it. Spending restarts the
// recovery timer, and a bar left at one percent or less restarts the block.
bool PlayerSimulation::spend_stamina(float amount) noexcept
{
    if (amount <= 0.0F)
        return true;
    if (!stamina_usable_ || stamina_block_ticks_ < stamina_block_timer_ticks ||
        amount > state_.stamina)
        return false;
    state_.stamina -= amount;
    stamina_recover_ticks_ = 0;
    if (state_.stamina <= 1.0F)
        stamina_block_ticks_ = 0;
    return true;
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
    // InGameHUD.DoConstructionModeUpdate: five units along the camera, with
    // ConstructionProbe.SkipEmbedded set, so the cell the eye is INSIDE (leaves,
    // an open door, a torch) is never the target; the ray goes on past it.
    state_.target = raycast_internal(world, eye, view_direction(), interaction_reach, false, true);
    // InGameHUD.Dig and Build both need BlockTerrain.OkayToBuildHere: nothing
    // in the world's bottom or top layer (world Y -64 and 63) can be dug.
    if (state_.target.has_value() &&
        (state_.target->y <= world_min_y || state_.target->y >= world_min_y + chunk_height - 1))
        state_.target.reset();
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
    if (!state_.target.has_value())
        return false;
    // (mod-liquid) A block placed into a liquid replaces it. Unless the block
    // is the liquid's own fallback (the cell would not change at all) or the
    // body is in the way, which is tested before: the cell's generated
    // identity goes with the liquid, so it could not be put back.
    const int cell_x = state_.target->place_x;
    const int cell_y = state_.target->place_y;
    const int cell_z = state_.target->place_z;
    if (liquid_at(world, cell_x, cell_y, cell_z))
    {
        const Vec3 feet = state_.position;
        const bool in_the_way =
            feet.x + body_half_width > static_cast<float>(cell_x) &&
            feet.x - body_half_width < static_cast<float>(cell_x + 1) &&
            feet.y + body_height > static_cast<float>(cell_y) &&
            feet.y < static_cast<float>(cell_y + 1) &&
            feet.z + body_half_width > static_cast<float>(cell_z) &&
            feet.z - body_half_width < static_cast<float>(cell_z + 1);
        if (in_the_way || world.block(cell_x, cell_y, cell_z) == state_.selected_block ||
            !blocks_player(state_.selected_block))
            return false;
        world.set_block(cell_x, cell_y, cell_z, state_.selected_block);
        refresh_target(world);
        return true;
    }
    if (world.block(state_.target->place_x, state_.target->place_y, state_.target->place_z) !=
        BlockType::empty)
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
    // `if (CoolDownTimer.Expired)`, on the frame the original's tick timer
    // expires (see one_shot_expired).
    if (one_shot_expired(impact_cooldown))
    {
        update.impact = true;
        impact_cooldown = impact_period;
    }
    // InventoryItem.ProcessInput only reaches hud.Dig inside
    // `if (CoolDownTimer.Expired)`: DigTime keeps running between swings, but a
    // block breaks on the first SWING at or after its TimeToDig, never between
    // swings. A 0.01 s block therefore falls once per tool cooldown (0.3 s for
    // picks and laser swords), a 0.5 s block on the swing at 0.6 s, and even a
    // zero-second block (torch, surface lava) needs a swing. Completing the
    // instant the time elapsed let fast tools clear one block per tick.
    if (update.impact && std::isfinite(duration) && breaking_seconds_ >= duration)
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
    return raycast_internal(world, origin, direction, reach, false, false);
}

std::optional<VoxelRaycastHit> PlayerSimulation::raycast_blocking(const WorldRegion &world,
                                                                  Vec3 origin, Vec3 direction,
                                                                  float reach)
{
    return raycast_internal(world, origin, direction, reach, true, false);
}

std::optional<VoxelRaycastHit> PlayerSimulation::raycast_internal(const WorldRegion &world,
                                                                  Vec3 origin, Vec3 direction,
                                                                  float reach, bool blocking_only,
                                                                  bool skip_embedded)
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
    bool origin_cell = true;
    while (distance <= reach)
    {
        const BlockType type = world.block(x, y, z);
        // TraceProbe.SetIntersection: a probe that starts inside a shape and
        // has SkipEmbedded set reports nothing for that shape.
        const bool embedded = skip_embedded && origin_cell;
        origin_cell = false;
        // (mod-liquid) Every ray passes through liquids: the crosshair digs
        // and builds on what lies beyond the water.
        if (!embedded && is_solid(type) && (!blocking_only || blocks_player(type)) &&
            !liquid_at(world, x, y, z))
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
                if (cell_blocks_player(world, x, y, z)) // (mod-liquid) liquids stop nothing
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// Player.ResolveCollsion, statement for statement. A stock host believes the
// position a client reports, so every way this differs from the original is a
// player standing somewhere a stock player could not be.
//
//   * EnemyManager.AttentuateVelocity first: blocking enemies within two
//     blocks halve the horizontal speed (and any UPWARD speed, which is what
//     makes jumping next to a zombie so feeble), more when walking at them.
//   * Up to four slide iterations of the swept AABBTraceProbe. With AutoClimb
//     on and a vertical speed under half the jump impulse, the sides of a
//     block in the feet band are "sloped": instead of stopping the player they
//     lift it at five units a second to that block's top + 0.001.
//   * InContact (grounded) is ONLY a +Y face hit, or a slope, in this frame.
//     There is no tolerance probe below the feet: gravity makes a standing
//     player collide with the floor every frame.
void PlayerSimulation::move_and_collide(const WorldRegion &world, float delta_seconds,
                                        bool auto_climb)
{
    constexpr Vec3 box_minimum{-body_half_width, 0.0F, -body_half_width};
    constexpr Vec3 box_maximum{body_half_width, body_height, body_half_width};
    float remaining = delta_seconds;
    const Vec3 stored_velocity = state_.velocity;
    Vec3 next = state_.position;
    Vec3 velocity = state_.velocity;
    if (velocity_attenuation_)
    {
        // The original normalizes a possibly zero vector; the NaN it gets
        // fails `> 0` exactly as a zero direction does here.
        const float horizontal = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
        const Vec3 forward = horizontal > 0.0F
                                 ? Vec3{velocity.x / horizontal, 0.0F, velocity.z / horizontal}
                                 : Vec3{};
        const float attenuation = velocity_attenuation_(forward, state_.position);
        velocity.x *= attenuation;
        if (velocity.y > 0.0F)
            velocity.y *= attenuation;
        velocity.z *= attenuation;
    }
    const float entry_vertical = velocity.y;
    state_.grounded = false;
    side_contact_ = false; // (mod-liquid)
    int iterations{};
    bool collides{};
    do
    {
        const Vec3 start = next;
        const Vec3 step{velocity.x * remaining, velocity.y * remaining, velocity.z * remaining};
        next = {start.x + step.x, start.y + step.y, start.z + step.z};
        const bool slopes = auto_climb && velocity.y < jump_impulse * 0.5F;
        const MovementTrace trace =
            trace_movement_box(world, start, next, box_minimum, box_maximum, slopes);
        collides = trace.collides;
        // The sloped block carries the player up for `seconds` of this frame.
        const auto climb = [&](float seconds) {
            state_.grounded = true;
            velocity.y = 0.0F;
            next.y += auto_climb_visual_speed * seconds;
            const float top = static_cast<float>(trace.sloped_block_y) + 1.001F;
            if (next.y > top)
                next.y = top;
        };
        if (collides)
        {
            if (trace.in_face == ProbeFace::positive_y)
                state_.grounded = true;
            else if (trace.in_face != ProbeFace::negative_y)
                side_contact_ = true; // (mod-liquid)
            const float travelled = std::max(trace.in_t - 0.001F, 0.0F);
            next = {start.x + step.x * travelled, start.y + step.y * travelled,
                    start.z + step.z * travelled};
            const bool on_slope =
                trace.found_sloped_block && trace.sloped_block_t <= trace.in_t;
            if (on_slope)
                climb(travelled * remaining);
            const float into = trace.in_normal.x * velocity.x + trace.in_normal.y * velocity.y +
                               trace.in_normal.z * velocity.z;
            velocity = {velocity.x - trace.in_normal.x * into, velocity.y - trace.in_normal.y * into,
                        velocity.z - trace.in_normal.z * into};
            remaining *= 1.0F - travelled;
            if (remaining <= 1e-07F)
                break;
            if (length_squared(velocity) <= 1e-06F ||
                stored_velocity.x * velocity.x + stored_velocity.y * velocity.y +
                        stored_velocity.z * velocity.z <=
                    1e-06F)
            {
                velocity = {};
                if (on_slope)
                    climb(remaining);
                break;
            }
        }
        else if (trace.found_sloped_block)
        {
            climb(remaining);
        }
        ++iterations;
    } while (collides && iterations < 4);
    if (iterations == 4)
        velocity = {};
    // What the landing absorbed: Player.ResolveCollsion's fall damage input.
    landing_speed_change_ = velocity.y - entry_vertical;
    state_.position = next;
    state_.velocity = velocity;
}

} // namespace openminerz::world
