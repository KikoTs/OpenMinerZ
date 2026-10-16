#pragma once

// The dragon owner's flight brain: retail DragonEntity and its seven states
// (DragonDefaultState, DragonLoiterState, DragonLoiterLeft/RightState,
// DragonHoverAttackState, DragonHoverState, DragonStrafeAttackState) and
// DragonBaseState's helpers, ported statement for statement from the shipped
// 1.6.3.8 executable (identical in 1.9.9.8 apart from where FindBestTarget
// lives). tools/OriginalClientInterop/DragonParityProbe runs the original
// beside this and compares the state timeline and every emitted message.
//
// Field and function names follow the original so the two can be read side by
// side. Everything the original reads from the session or from BlockTerrain
// goes through DragonBrainSession.

#include "openminerz/world/enemy.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace openminerz::world
{

// DragonStates.*
enum class DragonBrainMode : std::uint8_t
{
    default_state,
    loiter,
    loiter_left,
    loiter_right,
    hover,
    hover_attack,
    strafe_attack,
};

[[nodiscard]] const char *dragon_brain_mode_name(DragonBrainMode mode) noexcept;

// What DragonEntity reads from outside itself.
class DragonBrainSession
{
  public:
    virtual ~DragonBrainSession() = default;
    // CurrentNetworkSession.AllGamers with a Player, as FindBestTarget sees
    // them from `eye` (the dragon's position + Forward * 7): alive is
    // ValidLivingGamer, the other three are evaluated at position + 1.5.
    [[nodiscard]] virtual std::vector<DragonTargetCandidate> players(Vec3 eye) = 0;
    // Target.ValidLivingGamer ? Target.WorldPosition : nothing.
    [[nodiscard]] virtual std::optional<Vec3> living_position(std::uint8_t gid) = 0;
    // CanSeePosition: BlockTerrain.Trace from `eye` to `point` does not collide.
    [[nodiscard]] virtual bool can_see(Vec3 eye, Vec3 point) = 0;
    // BlockTerrain.RegionIsLoaded.
    [[nodiscard]] virtual bool region_loaded(Vec3 point) = 0;
    // CastleMinerZGame.LocalPlayer: its gamer id, ValidGamer and WorldPosition.
    [[nodiscard]] virtual std::uint8_t local_gid() = 0;
    [[nodiscard]] virtual bool local_valid() = 0;
    [[nodiscard]] virtual Vec3 local_position() = 0;
    // Native cavern encounters use their available air band. Retail/reference
    // sessions retain the authored surface altitudes by default.
    [[nodiscard]] virtual float flight_altitude(Vec3, float requested) { return requested; }
    // MathTools.Rnd.NextDouble() narrowed to float, and Rnd.Next(min, max).
    [[nodiscard]] virtual float random_float() = 0;
    [[nodiscard]] virtual int random_int(int minimum, int maximum_exclusive) = 0;
};

// DragonEntity's own fields.
struct DragonBrainState final
{
    DragonKind kind{DragonKind::fire};
    DragonBrainMode mode{DragonBrainMode::default_state};
    Vec3 position{}; // LocalPosition
    Vec3 forward{0.0F, 0.0F, -1.0F}; // LocalToWorld.Forward of the last LocalRotation
    float velocity{20.0F};
    float target_velocity{20.0F};
    float yaw{};
    float target_yaw{};
    float roll{};
    float target_roll{};
    float pitch{};
    float target_pitch{};
    bool first_time_for_default_state{true};
    bool had_target_this_pass{};
    float loiter_timer{};
    float next_update_time{-1.0F};
    std::optional<std::uint8_t> target; // Player Target
    Vec3 travel_target{};
    bool shot_pending{};
    Vec3 shoot_target{};
    std::uint8_t current_animation{1U}; // DragonAnimEnum
    std::uint8_t next_animation{1U};
    float flap_debt{};
    int shots_left{};
    float time_left_before_next_shot{};
    float time_left_til_shots_heard{};
    float time_left_before_next_view_check{};
    float default_heading{};
    float target_altitude{};
    int updates_sent{};
    int animation_change_update{};
    int flap_change_update{};
    int loiters_left{};
    bool removed{};
    std::uint8_t next_sound{}; // DragonSoundEnum
    int chances_to_not_attack{5};
    float dragon_time{};
    bool migrate_dragon{};
    std::optional<std::uint8_t> migrate_dragon_to;
    std::vector<Vec3> gunshots;
    // OneShotTimer CryTimer and the playing AnimationPlayer, in TimeSpan ticks.
    std::int64_t cry_elapsed_ticks{};
    std::int64_t cry_maximum_ticks{50'000'000};
    std::int64_t clip_ticks{};
    // Which game's dragon clips are installed (their lengths differ).
    bool modern_content{};
    // Statics the original keeps outside the entity.
    int update_count{}; // UpdateDragonMessage.UpdateCount
    std::uint32_t next_fireball_index{}; // DragonEntity.NextFireballIndex
};

struct DragonBrainOutput final
{
    // At most one per update: SendRegularUpdate.
    std::vector<DragonOwnerMessage> messages;
    // EnemyManager.RemoveDragon() calls (each one is a RemoveDragonMessage).
    int remove_requests{};
    // DoMigration: hand the dragon to this player. Nothing else ran this frame.
    std::optional<std::uint8_t> migrate_to;
};

// DragonEntity.InitSpawnState: the random approach heading, then the spawn
// 750 units behind it at cruising altitude, in DragonDefaultState.
[[nodiscard]] float dragon_brain_spawn_heading(DragonBrainSession &session);
void dragon_brain_spawn(DragonBrainState &entity, DragonKind kind, Vec3 local_player,
                        float heading) noexcept;
// DragonEntity.InitAfterHostChange.
void dragon_brain_adopt(DragonBrainState &entity, DragonKind kind,
                        const DragonMigrationState &migration, DragonBrainSession &session);
// DragonEntity.DoMigration's DragonHostMigrationInfo.
[[nodiscard]] DragonMigrationState dragon_brain_migration(const DragonBrainState &entity) noexcept;
// StateMachine.ChangeState and DragonBaseState.GetNextAttackType.
void dragon_brain_change_state(DragonBrainState &entity, DragonBrainMode mode,
                               DragonBrainSession &session);
[[nodiscard]] DragonBrainMode dragon_brain_next_attack_type(DragonBrainState &entity,
                                                           DragonBrainSession &session);
// DragonEntity.Update.
void dragon_brain_update(DragonBrainState &entity, DragonBrainSession &session, float delta_seconds,
                         std::uint8_t local_gid, DragonBrainOutput &output);
// EnemyManager.DragonHasBeenHit.
inline void dragon_brain_hit(DragonBrainState &entity) noexcept
{
    entity.chances_to_not_attack = 0;
}

} // namespace openminerz::world
