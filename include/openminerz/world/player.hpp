#pragma once

#include "openminerz/world/block.hpp"
#include "openminerz/world/player_animation.hpp"
#include "openminerz/world/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace openminerz::world
{

// Player.cs derives both environment volumes from the player's feet. Lava is a
// global two-block transition beginning at Y=-60, independent of the rendered
// lava voxel type. Water is measured from BlockTerrain.WaterLevel, but only in
// a water world, and the shipped game never is one (IsWaterWorld is never
// set): `water_world` is true only while the default-off Lagoon water
// enhancement is in effect (retail_parity.hpp). Callers pass false otherwise.
[[nodiscard]] float water_depth(float feet_y) noexcept;
[[nodiscard]] bool is_underwater(bool water_world, float feet_y) noexcept;
[[nodiscard]] float lava_submersion(float feet_y) noexcept;
[[nodiscard]] bool is_under_lava(float feet_y) noexcept;
inline constexpr float oxygen_decay_per_second = 0.1F;
inline constexpr float drowning_health_per_second = 20.0F;
inline constexpr float lava_damage_period_seconds = 0.5F;
inline constexpr float lava_damage_health = 25.0F;

struct Vec3 final
{
    float x{};
    float y{};
    float z{};
};

// WorldInfo.DefaultStartLocation in the shipped client. Teleports first move
// here, then FindTopmostGroundLocation keeps X/Z and chooses the surface Y.
inline constexpr Vec3 default_start_location{8.0F, 128.0F, -8.0F};

// DNA.Timers.OneShotTimer, as every InventoryItem's CoolDownTimer uses it: a
// TimeSpan that Update advances by the frame's elapsed ticks, Reset zeroes, and
// that is Expired at `Elapsed >= MaxTime`. One 60 Hz XNA frame is 166667 ticks,
// a hair OVER 1/60 s, so a cooldown that is a whole number of frames (0.1 s,
// 0.25 s, 0.5 s, 1 s) expires ON that frame: six frames for 0.1 s, not seven.
//
// The timers here count the remaining seconds down in float from the period.
// Landing "exactly" on zero is then a rounding coin toss, and the losing side
// swung, dug and fired one frame late on every repeat (14% slower at 0.1 s).
// CoolDownTimes are whole milliseconds and a frame is 1/60 s, so the nearest a
// frame can come to a period without reaching it is a third of a millisecond;
// anything within this slack is the frame the original expires on.
inline constexpr float one_shot_timer_slack = 1.0e-4F;
[[nodiscard]] constexpr bool one_shot_expired(float seconds_left) noexcept
{
    return seconds_left <= one_shot_timer_slack;
}

// Whether the fly key does anything, by the shipped game's own rule (executed
// in both generations by SimulationParityProbe's fly_* scenarios):
//   1.9.9.8  Player.ProcessInput: `if (GameMode == Creative &&
//            mapping.FlyMode.Pressed) FlyMode = !FlyMode;`
//   1.6.3.8  binds the same key (controller function 13) and has the same
//            `_flyMode` field, but nothing reads the one or sets the other:
//            no world of that generation can fly, Creative included.
[[nodiscard]] constexpr bool retail_fly_key_toggles(bool modern, bool creative) noexcept
{
    return modern && creative;
}

// What the fly key does in OpenMinerZ. A DELIBERATE DIFFERENCE from retail,
// asked for by the project's owner: in Creative it toggles flight in BOTH
// generations, offline and online (the player's position is the client's own
// and simply replicates, so no world state can diverge). `free_flight` is the
// Free flight extra being in effect: flight in any other unrestricted world,
// offline only.
[[nodiscard]] constexpr bool fly_key_toggles(bool creative, bool free_flight) noexcept
{
    return creative || free_flight;
}
// Which model flies: 1.9.9.8 Creative keeps retail's own (24 along the camera,
// 48 sprinting, +15 up while Jump is held); 1.6.3.8 Creative, where retail has
// nothing to copy, and the Free flight extra use this project's free flight
// at the player's flight-speed setting.
[[nodiscard]] constexpr bool flight_is_free_model(bool modern, bool creative,
                                                  bool free_flight) noexcept
{
    return free_flight || (creative && !modern);
}

struct PlayerInput final
{
    float strafe{};
    float forward{};
    bool jump{};
    // Modern core profile only. The shipped Player requires movement plus a
    // held sprint trigger and a successful normalized stamina spend.
    bool sprint{};
    // Creative flight and swimming: descend while Shift/Ctrl (or the pad's
    // left-stick click) is held; jump/Space ascends.
    bool descend{};
};

struct VoxelRaycastHit final
{
    int x{};
    int y{};
    int z{};
    int place_x{};
    int place_y{};
    int place_z{};
    int normal_x{};
    int normal_y{};
    int normal_z{};
    float distance{};
    BlockType block{};
};

enum class ViewmodelCue : std::uint8_t
{
    idle,
    primary_use,
    shoulder,
    shoulder_idle,
    shoulder_use,
    unshoulder,
    reload,
    grenade_cook,
    grenade_throw,
};

struct PlayerState final
{
    Vec3 position{};
    Vec3 velocity{};
    float yaw{};
    float pitch{};
    float step_view_offset{};
    float health{100.0F};
    float stamina{100.0F};
    bool sprinting{};
    bool grounded{};
    // Player._flyMode: gravity off and the velocity taken from the input every
    // frame. How it flies is PlayerSimulation's FlightModel.
    bool fly_mode{};
    bool using_tool{};
    float break_progress{};
    float tool_swing_progress{};
    ViewmodelCue viewmodel_cue{ViewmodelCue::idle};
    float viewmodel_cue_progress{};
    // Render-only: every animation channel as Player.UpdateAnimation and the
    // avatar's clip clocks left it (clip, position, cross-fade), by the game's
    // registered clip names. `viewmodel_cue` above is the same state reduced
    // for the first-person viewmodel that predates this list.
    PlayerAnimationPose animation{};
    bool animation_valid{};
    // Render-only global clock phase used by the held ClockEntity model.
    float clock_time_of_day{};
    // Render-only death state. The world camera follows the animated head
    // while the local body plays Faint; the head itself is hidden.
    bool dead{};
    float death_animation_seconds{};
    // Render-only payload for the held GPSEntity. Each locator instance owns
    // its own target, so this follows the selected stack rather than the item id.
    Vec3 selected_waypoint{};
    std::uint16_t selected_item_id{23U};
    BlockType selected_block{BlockType::rock};
    std::optional<VoxelRaycastHit> target;
};

struct BlockBreakUpdate final
{
    int x{};
    int y{};
    int z{};
    BlockType block{};
    Vec3 intersection{};
    int normal_x{};
    int normal_y{};
    int normal_z{};
    bool impact{};
    bool completed{};
    std::vector<BlockRemovalEffect> removed_blocks;
};

class PlayerSimulation final
{
  public:
    static constexpr float body_half_width = 0.35F;
    static constexpr float body_height = 1.7F;
    static constexpr float eye_height = 1.4F;
    // FPSRig.Speed. It is only what the player moves at while AIRBORNE in
    // 1.6.3.8: on the ground Player.UpdateAnimation replaces it with the two
    // gait speeds below, so a player runs at 4, not 5.
    static constexpr float movement_speed = 5.0F;
    static constexpr float ground_run_speed = 4.0F;
    static constexpr float ground_stroll_speed = 0.947F;
    static constexpr float sprint_multiplier = 2.0F;
    static constexpr float sprint_stamina_cost_per_second = 60.0F;
    static constexpr float stamina_recovery_per_second = 25.0F;
    static constexpr float stamina_recovery_delay_seconds = 1.0F;
    static constexpr float stamina_exhaustion_block_seconds = 2.0F;
    static constexpr float underwater_speed = 1.894F;
    static constexpr float underwater_up_acceleration = 4.0F;
    static constexpr float underwater_control_acceleration = 12.0F;
    static constexpr float underwater_vertical_speed = 4.0F;
    static constexpr float underwater_drag = 3.0F;
    static constexpr float jump_impulse = 10.0F;
    // Shipped Modern Player.GetJumpCost returns 0 for jump zero and 0.25 of
    // the normalized stamina meter thereafter.  This simulation stores
    // stamina as 0..100, so only the airborne second jump costs 25 points.
    static constexpr float modern_air_jump_stamina_cost = 25.0F;
    static constexpr float gravity = -20.0F;
    // 1.9.9.8 Player.UpdateAnimation, `_flyMode` branch: the ground gait speed
    // (4, stroll 0.947, doubled while sprinting) times 6 along the camera, and
    // JumpImpulse * 1.5 added upward while Jump is held.
    static constexpr float retail_fly_multiplier = 6.0F;
    static constexpr float retail_fly_rise_speed = jump_impulse * 1.5F;
    // The Free flight extra's speeds: horizontal cruise (the default of the
    // "Creative flight speed" setting) and its vertical share.
    static constexpr float fly_speed = 8.0F;
    static constexpr float fly_vertical_speed = 6.0F;
    static constexpr float auto_climb_visual_speed = 5.0F;
    static constexpr float interaction_reach = 5.0F;
    static constexpr float gameplay_fov_degrees = 73.0F;

    [[nodiscard]] const PlayerState &state() const noexcept;
    [[nodiscard]] PlayerState &state() noexcept;

    void spawn(const WorldRegion &world, int near_x, int near_z);
    void update(const WorldRegion &world, const PlayerInput &input, float fixed_delta_seconds,
                bool auto_climb, bool water_world = false, float creative_flight_speed = fly_speed);
    void update(const WorldRegion &world, const PlayerInput &input, float fixed_delta_seconds,
                bool auto_climb, bool water_world, float creative_flight_speed,
                bool modern_double_jump);
    // EnemyManager.AttentuateVelocity as Player.ResolveCollsion applies it
    // before moving: given the player's normalized horizontal velocity and
    // position, how much the blocking enemies around it scale its speed.
    using VelocityAttenuation = std::function<float(Vec3 forward, Vec3 position)>;
    void set_velocity_attenuation(VelocityAttenuation attenuation)
    {
        velocity_attenuation_ = std::move(attenuation);
    }
    // 1.9.9.8 InGameHUD.CanUseStamina refuses every spend in Endurance and
    // Dragon Endurance: no sprint and no second jump there.
    void set_stamina_usable(bool usable) noexcept
    {
        stamina_usable_ = usable;
    }
    // How `state().fly_mode` moves the player.
    //   retail: 1.9.9.8's own flight (executed, SimulationParityProbe fly_*):
    //           the velocity is the camera's Forward and Right times the gait
    //           speed times 6 -- 24 at a full stick, 48 sprinting (which
    //           spends stamina as on the ground), zero with no input -- plus
    //           15 upward while Jump is held. It collides like walking does.
    //   free:   the Free flight extra (retail_parity.hpp): the heading alone
    //           steers at `creative_flight_speed`, Jump rises, descend sinks.
    enum class FlightModel : std::uint8_t
    {
        retail,
        free
    };
    void set_flight_model(FlightModel model) noexcept
    {
        flight_model_ = model;
    }
    [[nodiscard]] FlightModel flight_model() const noexcept
    {
        return flight_model_;
    }
    // Vertical velocity after the last update's collision minus the velocity
    // it entered the collision with (after gravity): what a landing absorbed.
    // The original deals (change - 15) / 15 of full health when this exceeds
    // 15 (18 in 1.9.9.8) and the player has stopped rising.
    [[nodiscard]] float landing_speed_change() const noexcept
    {
        return landing_speed_change_;
    }
    // (mod-liquid) Whether the last update started with the feet in a mod
    // liquid or ended with the body in one (openminerz/world/liquid.hpp):
    // such a landing is no fall. Always false where no liquids exist.
    [[nodiscard]] bool touched_liquid() const noexcept
    {
        return touched_liquid_;
    }
    // (mod-liquid) Climbing out of a liquid: Jump held against a bank while
    // the feet are less than this deep lifts the player at this speed.
    static constexpr float liquid_climb_depth = 1.3F;
    static constexpr float liquid_climb_speed = 5.5F;
    void look(float horizontal_radians, float vertical_radians) noexcept;
    void refresh_target(const WorldRegion &world);
    [[nodiscard]] bool mine_target(WorldRegion &world);
    [[nodiscard]] bool place_target(WorldRegion &world);
    // dig_seconds < 0 falls back to the bare-hand table; the app passes the
    // held tool's TimeToDig for the current target instead.
    [[nodiscard]] std::optional<BlockBreakUpdate> update_breaking(
        WorldRegion &world, bool held, float delta_seconds, float impact_period_seconds = 0.5F,
        std::uint8_t action_key = 0U, float dig_seconds = -1.0F);
    void cancel_breaking() noexcept;
    [[nodiscard]] Vec3 view_direction() const noexcept;

    [[nodiscard]] static std::optional<VoxelRaycastHit> raycast(const WorldRegion &world,
                                                                Vec3 origin, Vec3 direction,
                                                                float reach);
    // Projectile/physics trace that skips pass-through rendered voxels such as
    // leaves, lava, torches, and open doors.
    [[nodiscard]] static std::optional<VoxelRaycastHit> raycast_blocking(const WorldRegion &world,
                                                                         Vec3 origin,
                                                                         Vec3 direction,
                                                                         float reach);
    [[nodiscard]] static bool touches_solid(const WorldRegion &world, Vec3 feet_position) noexcept;

  private:
    [[nodiscard]] static std::optional<VoxelRaycastHit> raycast_internal(
        const WorldRegion &world, Vec3 origin, Vec3 direction, float reach, bool blocking_only,
        bool skip_embedded);

    struct BlockCoordinate final
    {
        int x{};
        int y{};
        int z{};

        bool operator==(const BlockCoordinate &) const noexcept = default;
    };

    void move_and_collide(const WorldRegion &world, float delta_seconds, bool auto_climb);

    PlayerState state_{};
    VelocityAttenuation velocity_attenuation_{};
    FlightModel flight_model_{FlightModel::retail};
    float landing_speed_change_{};
    // (mod-liquid) The last move ran into a wall; the last update touched a
    // liquid.
    bool side_contact_{};
    bool touched_liquid_{};
    bool jump_was_down_{};
    int jump_count_{};
    [[nodiscard]] bool spend_stamina(float amount) noexcept;

  public:
    // InGameHUD.StaminaBlockTimer is still running (two seconds after stamina
    // ran out). Read by the HUD only: 1.9.9.8 tints its stamina meter while it is.
    [[nodiscard]] bool stamina_blocked() const noexcept
    {
        return stamina_block_ticks_ < stamina_block_timer_ticks;
    }

  private:
    // InGameHUD.StaminaRecoverTimer (one second) and StaminaBlockTimer (two),
    // as elapsed 100 ns ticks; both start run out.
    static constexpr std::int64_t stamina_recover_timer_ticks = 10'000'000;
    static constexpr std::int64_t stamina_block_timer_ticks = 20'000'000;
    static constexpr std::int64_t stamina_timer_ceiling = 100'000'000;
    std::int64_t stamina_recover_ticks_{stamina_timer_ceiling};
    std::int64_t stamina_block_ticks_{stamina_timer_ceiling};
    bool stamina_usable_{true};
    std::optional<BlockCoordinate> breaking_target_;
    float breaking_seconds_{};
    float action_animation_seconds_{};
    std::array<float, 9> impact_cooldowns_{};
};

} // namespace openminerz::world
