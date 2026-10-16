#pragma once

#include "openminerz/world/enemy.hpp"

#include <cstdint>
#include <functional>
#include <string_view>

// The ordinary enemies' simulation: BaseZombie, its states, its collision and
// its animation clock, ported statement for statement from the shipped
// client. No peer replicates an enemy's position, so this code IS the
// replication: every difference from the original moves an enemy somewhere a
// stock player does not see it.
namespace openminerz::world::enemy_simulation
{

using ClipDurationLookup = std::function<float(EnemyModelKind, std::string_view)>;

struct Context final
{
    const WorldRegion &region;
    // Player.WorldPosition, PlayerPhysics.WorldVelocity and InContact of the
    // enemy's Target. `target_alive` is `Target.ValidGamer && !Target.Dead`.
    const PlayerState &target;
    bool target_alive{true};
    // Target.IsLocal: only the targeted player's own peer deals the damage,
    // asks for the speed-up and announces the give-up.
    bool local_target{};
    // (Authority) For a remote target, also test the swing as its own peer
    // would and record a landing in events->remote_strikes. Observation only.
    bool observe_remote{};
    // EnemyManager.ZombieFestIsOn.
    bool zombie_fest{};
    // 1.9.9.8 instead of 1.6.3.8. The two generations differ in three places:
    // every give-up state announces itself (only ZombieGiveUp does in 1.6.3.8),
    // the chase gives up at BaseZombie.PlayerDistanceLimit (Felguards at 40
    // like Aliens, spawner-block enemies at 100, hell-block ones at 300), and a
    // spawner-block zombie digs on until its target is 48 away or 24 above.
    bool modern_rules{};
    float delta_seconds{};
    const ClipDurationLookup *clip_durations{};
    // MathTools.Rnd, which Explosive.EnemyBreakBlocks flips a coin on.
    EnemyInstanceRng *dig_random{};
    EnemyEvents *events{};
};

// The pinned length of a clip the state machine waits on, in 100 ns ticks as
// the shipped models carry them; 0 when the clip is not one of those.
[[nodiscard]] std::int64_t stock_clip_ticks(EnemyModelKind model, std::string_view clip) noexcept;

// BaseZombie..ctor after the fields are filled: ChangeState(GetEmergeState).
void spawn(Enemy &enemy, const ClipDurationLookup *clip_durations);
// Scene.OnUpdate's physics pass for one enemy: BasicPhysics.Accelerate, then
// BaseZombie.ResolveCollsion.
void advance_physics(const WorldRegion &region, Enemy &enemy, float delta_seconds);
// BaseZombie.OnUpdate: the dead-target give-up, the state machine, then the
// animation clock (ModelEntity.OnUpdate).
void update(Enemy &enemy, Context &context);

// BaseZombie.SpeedUp / GiveUp / Kill and the tail of TakeDamage, as the
// message handlers and weapons call them between frames.
void speed_up(Enemy &enemy, const ClipDurationLookup *clip_durations);
void give_up(Enemy &enemy, const ClipDurationLookup *clip_durations);
void kill(Enemy &enemy, const ClipDurationLookup *clip_durations);
void react_to_hit(Enemy &enemy, const ClipDurationLookup *clip_durations);

} // namespace openminerz::world::enemy_simulation
