#pragma once

#include "openminerz/world/block.hpp"
#include "openminerz/world/world.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace openminerz::world
{

// Player.cs derives both environment volumes from the player's feet. Lagoon
// water uses BlockTerrain.WaterLevel; lava is a global two-block transition
// beginning at Y=-60, independent of the rendered lava voxel type.
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
    // Creative fly mode (the original Player._flyMode): gravity off, jump
    // rises, descend key sinks.
    bool fly_mode{};
    bool using_tool{};
    float break_progress{};
    float tool_swing_progress{};
    ViewmodelCue viewmodel_cue{ViewmodelCue::idle};
    float viewmodel_cue_progress{};
    // Render-only global clock phase used by the held ClockEntity model.
    float clock_time_of_day{};
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
    static constexpr float movement_speed = 5.0F;
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
    // Creative flight speeds: brisk horizontal cruise, steady vertical.
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
        const WorldRegion &world, Vec3 origin, Vec3 direction, float reach, bool blocking_only);

    struct BlockCoordinate final
    {
        int x{};
        int y{};
        int z{};

        bool operator==(const BlockCoordinate &) const noexcept = default;
    };

    [[nodiscard]] bool can_slope_slide(const WorldRegion &world, Vec3 candidate) const noexcept;
    void move_and_collide(const WorldRegion &world, float delta_seconds, bool auto_climb);

    PlayerState state_{};
    bool jump_was_down_{};
    int jump_count_{};
    float stamina_recovery_delay_{};
    float stamina_block_time_{};
    std::optional<BlockCoordinate> breaking_target_;
    float breaking_seconds_{};
    float action_animation_seconds_{};
    std::array<float, 9> impact_cooldowns_{};
};

} // namespace openminerz::world
