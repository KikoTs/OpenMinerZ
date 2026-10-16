#include "enemy_simulation.hpp"

#include "openminerz/world/liquid.hpp" // mod-liquid
#include "openminerz/world/movement_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace openminerz::world::enemy_simulation
{
namespace
{

constexpr float pi = 3.14159265358979323846F;
constexpr float two_pi = 6.2831855F;
// BaseZombie.PlayerAABB and BasicPhysics.Gravity.
constexpr Vec3 enemy_box_minimum{-0.35F, 0.0F, -0.35F};
constexpr Vec3 enemy_box_maximum{0.35F, 1.65F, 0.35F};
constexpr float gravity = -20.0F;
constexpr std::int64_t ticks_per_second = 10'000'000;

struct StockClip final
{
    std::string_view name;
    std::int64_t ticks;
};

// AnimationClip.Duration of every clip a state waits on, read from the shipped
// models by tools/OriginalClientInterop/SimulationParityProbe. The four
// skeleton rigs share one clip set.
constexpr std::array<StockClip, 21> zombie_clips{{
    {"arise_1", 23000000}, {"arise_2", 35666666}, {"arise_3", 83000000}, {"arise_4", 83000000},
    {"walk", 16333333},    {"walk2", 13000000},   {"run", 13000000},     {"run_fast", 6333333},
    {"attack1", 16333333}, {"attack2", 23000000}, {"attack3", 19333333}, {"attack4", 19666666},
    {"attack5", 24333333}, {"enraged", 36333333}, {"hit_reaction1", 16333333},
    {"hit_reaction3", 16333333}, {"death1", 19666666}, {"death2", 23000000},
    {"death3", 26333333},  {"eat_start", 13000000}, {"jump", 19666666},
}};
constexpr std::array<StockClip, 25> skeleton_clips{{
    {"walk", 13000000},        {"walk2", 19666666},       {"run", 6333333},
    {"attack1", 19333333},     {"attack2", 19666666},     {"attack3", 24333333},
    {"axes_atack1", 13000000}, {"axes_atack2", 13000000}, {"enraged", 36333333},
    {"gethit1", 9666666},      {"gethit2", 26333333},     {"gethit3", 8000000},
    {"death1", 18000000},      {"death2", 18000000},      {"death3", 18000000},
    {"death4", 19666666},      {"death5", 23000000},      {"death6", 29666666},
    {"death7", 26333333},      {"standup", 21333333},     {"standup2", 34666666},
    {"walk_archer1", 13000000}, {"idle_archer1", 33000000}, {"idle-archer1", 33000000},
    {"atack_archer1", 16333333},
}};
constexpr std::array<StockClip, 7> felguard_clips{{
    {"Idle", 33000000}, {"Attack1", 16333333}, {"atack_3", 29666666}, {"walk", 13000000},
    {"run", 11333333},  {"death1", 13000000},  {"death2", 59666666},
}};
constexpr std::array<StockClip, 7> alien_clips{{
    {"MoveLoop", 26333333}, {"Attack1", 19666666}, {"Attack2", 19666666}, {"Jump", 13000000},
    {"Damage1", 6333333},   {"Damage2", 6333333},  {"Death", 26333333},
}};

template <std::size_t Count>
[[nodiscard]] std::int64_t find_clip(const std::array<StockClip, Count> &clips,
                                     std::string_view clip) noexcept
{
    for (const StockClip &entry : clips)
        if (entry.name == clip)
            return entry.ticks;
    return 0;
}

[[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] float length_squared(Vec3 value) noexcept
{
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

[[nodiscard]] float dot(Vec3 left, Vec3 right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

// Our yaw of a horizontal direction: forward is (sin yaw, -cos yaw). The
// original stores atan2(-z, x) + pi/2 + EnemyType.Facing as a quaternion whose
// LocalToWorld.Forward is this same direction.
[[nodiscard]] float heading(float x, float z) noexcept
{
    return std::atan2(x, -z);
}

[[nodiscard]] bool is_skeleton_melee(EnemyClass enemy_class) noexcept
{
    return enemy_class == EnemyClass::skeleton || enemy_class == EnemyClass::skeleton_sword ||
           enemy_class == EnemyClass::skeleton_axes;
}

// EnemyType.GetDigState: only zombies dig, everything else gives up instead.
[[nodiscard]] EnemyState dig_state(EnemyClass enemy_class) noexcept
{
    return enemy_class == EnemyClass::zombie ? EnemyState::dig : EnemyState::give_up;
}

struct Machine final
{
    Enemy &enemy;
    const EnemyStats &stats;
    const ClipDurationLookup *clip_durations;
    // Null between frames (message handlers and weapon hits).
    Context *context;

    [[nodiscard]] std::int64_t duration(std::string_view clip) const
    {
        if (clip_durations != nullptr && *clip_durations)
        {
            const float seconds = (*clip_durations)(stats.model, clip);
            if (seconds > 0.0F)
            {
                // A float of seconds cannot carry a 100 ns tick (it is off by
                // up to five of them for an eight-second clip), and one tick
                // decides which frame IsNearAnimationEnd fires on whenever the
                // threshold falls on the millisecond clock. Recover the
                // authored length: 1.9.9.8 clips are whole milliseconds,
                // 1.6.3.8 clips whole 30 Hz frames truncated to ticks.
                const std::int64_t ticks = std::llround(static_cast<double>(seconds) * 1e7);
                const std::int64_t milliseconds = (ticks + 5'000) / 10'000 * 10'000;
                if (std::llabs(ticks - milliseconds) <= 6)
                    return milliseconds;
                const std::int64_t frames =
                    std::llround(static_cast<double>(seconds) * 30.0) * 10'000'000 / 30;
                if (std::llabs(ticks - frames) <= 6)
                    return frames;
                return ticks;
            }
        }
        if (const std::int64_t ticks = stock_clip_ticks(stats.model, clip); ticks > 0)
            return ticks;
        return 14'000'000;
    }

    // ModelEntity.PlayClip: a fresh AnimationPlayer at speed 1, time zero.
    void play(std::string_view clip, bool loops)
    {
        enemy.clip = clip;
        enemy.clip_loops = loops;
        enemy.clip_speed = 1.0F;
        enemy.clip_ticks = 0;
        enemy.clip_time = 0.0F;
        enemy.clip_duration_ticks = duration(clip);
    }

    // BaseZombie.IsNearAnimationEnd.
    [[nodiscard]] bool near_end() const noexcept
    {
        return static_cast<double>(enemy.clip_duration_ticks - enemy.clip_ticks) * 1e-7 < 0.25;
    }

    // AnimationPlayer.Finished.
    [[nodiscard]] bool finished() const noexcept
    {
        return !enemy.clip_loops && enemy.clip_ticks >= enemy.clip_duration_ticks;
    }

    [[nodiscard]] double clip_seconds() const noexcept
    {
        return static_cast<double>(enemy.clip_ticks) * 1e-7;
    }

    void zero_velocity() noexcept
    {
        enemy.velocity.x = 0.0F;
        enemy.velocity.z = 0.0F;
    }

    void reduce_velocity() noexcept
    {
        enemy.velocity.x *= 0.99F;
        enemy.velocity.z *= 0.99F;
    }

    void remove() noexcept
    {
        enemy.state = EnemyState::despawn;
    }

    // ZombieChase.StartMoveAnimation and its Alien / Felguard overrides.
    void start_move_animation()
    {
        const float speed = enemy.current_speed;
        if (stats.enemy_class == EnemyClass::alien)
        {
            play("MoveLoop", true);
            enemy.clip_speed = std::min(speed / 1.0F, 1.0F);
            return;
        }
        if (stats.enemy_class == EnemyClass::felguard)
        {
            if (speed < 2.7F)
            {
                play("walk", true);
                enemy.clip_speed = std::min(speed / 1.0F, 1.0F);
            }
            else if (speed < 5.0F || !stats.has_run_fast)
            {
                play("run", true);
                enemy.clip_speed = std::min(speed / 3.0F, 1.0F);
            }
            return;
        }
        if (speed < 2.7F)
        {
            play("walk", true);
            enemy.clip_speed = std::min(speed / 1.0F, 1.0F);
        }
        else if (speed < 3.7F)
        {
            play("walk2", true);
            enemy.clip_speed = std::min(speed / 1.0F, 1.0F);
        }
        else if (speed < 5.0F || !stats.has_run_fast)
        {
            play("run", true);
            enemy.clip_speed = std::min(speed / 3.0F, 1.0F);
        }
        else
        {
            play("run_fast", true);
            enemy.clip_speed = std::min(speed / 4.0F, 1.0F);
        }
    }

    // BaseTryAttack.GetRandomAttack.
    [[nodiscard]] std::string_view random_attack()
    {
        const EnemyAttackSet &attacks = enemy_attacks(stats.enemy_class);
        enemy.attack_index = enemy.instance_rng.next_int(0, attacks.attack_count);
        return attacks.attacks[enemy.attack_index].clip;
    }

    void enter_emerge()
    {
        enemy.blocking = true;
        enemy.hittable = false;
        std::string_view clip = "Idle";
        switch (stats.enemy_class)
        {
        case EnemyClass::zombie: {
            constexpr std::array<std::string_view, 4> arises{"arise_1", "arise_2", "arise_3",
                                                             "arise_4"};
            clip = arises[static_cast<std::size_t>(enemy.instance_rng.next_int(0, 4))];
            break;
        }
        case EnemyClass::archer:
            // ArcherSkeletonEmerge rolls its 5..9-shot budget before the clip.
            enemy.swing_count = 5 + enemy.instance_rng.next_int(0, 5);
            clip = enemy.instance_rng.next_int(0, 2) != 0 ? "standup2" : "standup";
            break;
        case EnemyClass::alien:
            clip = "Jump";
            break;
        default:
            break;
        }
        play(clip, false);
        enemy.clip_speed = enemy.emerge_speed;
        // A random facing while clawing out of the ground.
        enemy.yaw = pi - enemy.instance_rng.next01() * two_pi;
    }

    void enter_chase()
    {
        enemy.blocking = true;
        enemy.hittable = true;
        if (stats.enemy_class == EnemyClass::archer)
        {
            play("walk_archer1", true);
            enemy.clip_speed = enemy.current_speed / 1.0F;
            enemy.frustration = 2.5F;
            enemy.state_timer = 0.0F;
            return;
        }
        start_move_animation();
        enemy.frustration = 2.5F;
    }

    void enter_attack()
    {
        if (context != nullptr && context->events != nullptr)
            ++context->events->attacks_started;
        if (stats.enemy_class == EnemyClass::archer)
        {
            zero_velocity();
            enemy.hit_count = 0;
            play("atack_archer1", false);
            --enemy.swing_count;
            return;
        }
        if (enemy.on_ground)
            zero_velocity();
        play(random_attack(), false);
        enemy.swing_count = 2 + enemy.instance_rng.next_int(0, 3);
        enemy.hit_count = 0;
        enemy.miss_count = enemy.instance_rng.next_int(3, 5);
        enemy.clip_speed = stats.attack_animation_speed;
    }

    void enter_idle()
    {
        zero_velocity();
        // 1.6.3.8 plays "idle_archer1" and its rig has that clip; 1.9.9.8 plays
        // "idle-archer1" and renamed the clip to match. Present whichever the
        // loaded rig carries, so the archer never idles in its bind pose.
        const bool underscore = clip_durations != nullptr && *clip_durations &&
                                (*clip_durations)(stats.model, "idle_archer1") > 0.0F;
        play(underscore ? "idle_archer1" : "idle-archer1", true);
        enemy.state_timer = 1.0F + enemy.instance_rng.next01() * 2.0F;
        enemy.state_duration = enemy.state_timer;
    }

    void enter_hit()
    {
        zero_velocity();
        std::string_view clip = "Idle";
        switch (stats.enemy_class)
        {
        case EnemyClass::zombie:
            clip = enemy.instance_rng.next_int(0, 2) == 0 ? "hit_reaction1" : "hit_reaction3";
            break;
        case EnemyClass::alien:
            clip = enemy.instance_rng.next_int(0, 2) == 0 ? "Damage1" : "Damage2";
            break;
        case EnemyClass::felguard:
            break;
        default: {
            constexpr std::array<std::string_view, 3> hits{"gethit1", "gethit2", "gethit3"};
            clip = hits[static_cast<std::size_t>(enemy.instance_rng.next_int(0, 3))];
            break;
        }
        }
        play(clip, false);
        enemy.clip_speed = stats.hit_animation_speed;
    }

    void enter_die()
    {
        zero_velocity();
        enemy.blocking = false;
        enemy.hittable = false;
        std::string_view clip = "Death";
        switch (stats.enemy_class)
        {
        case EnemyClass::zombie: {
            constexpr std::array<std::string_view, 3> deaths{"death1", "death2", "death3"};
            clip = deaths[static_cast<std::size_t>(enemy.instance_rng.next_int(0, 3))];
            break;
        }
        case EnemyClass::alien:
            break;
        case EnemyClass::felguard:
            clip = enemy.instance_rng.next_int(0, 2) != 0 ? "death2" : "death1";
            break;
        default: {
            constexpr std::array<std::string_view, 7> deaths{"death1", "death2", "death3", "death4",
                                                             "death5", "death6", "death7"};
            clip = deaths[static_cast<std::size_t>(enemy.instance_rng.next_int(0, 7))];
            break;
        }
        }
        play(clip, false);
        // The corpse timer. ResolveCollsion keeps draining it from this frame
        // on, so the body lasts about three seconds, not clip + five.
        enemy.frustration = 5.0F;
        enemy.clip_speed = stats.die_animation_speed;
    }

    void enter_give_up()
    {
        zero_velocity();
        // 1.6.3.8: only ZombieGiveUp announces itself; its skeleton, alien and
        // felguard overrides send nothing, so those enemies give up on each
        // peer's own measurement. 1.9.9.8 added the send to all three.
        if (context != nullptr && context->local_target && context->events != nullptr &&
            (stats.enemy_class == EnemyClass::zombie || context->modern_rules))
            context->events->give_up_requests.push_back({enemy.target_gid, enemy.id});
        switch (stats.enemy_class)
        {
        case EnemyClass::zombie:
            play("eat_start", false);
            break;
        case EnemyClass::alien:
            play("Jump", false);
            break;
        case EnemyClass::felguard:
            play("Idle", false);
            break;
        default:
            play("enraged", false);
            break;
        }
    }

    void enter_dig()
    {
        // Only reached from inside a frame, so the target is known.
        const Vec3 offset = subtract(context->target.position, enemy.position);
        if (context->local_target && length_squared(offset) > 256.0F)
        {
            change(EnemyState::give_up);
            return;
        }
        if (enemy.on_ground)
            zero_velocity();
        play(random_attack(), false);
        enemy.hit_count = 0;
        enemy.swing_count = 0;
        enemy.miss_count = 0;
        enemy.clip_speed = stats.attack_animation_speed;
    }

    // StateMachine.ChangeState.
    void change(EnemyState state)
    {
        enemy.previous_state = enemy.state;
        enemy.state = state;
        enemy.state_time = 0.0F;
        switch (state)
        {
        case EnemyState::emerge:
            enter_emerge();
            break;
        case EnemyState::chase:
            enter_chase();
            break;
        case EnemyState::attack:
            enter_attack();
            break;
        case EnemyState::idle:
            enter_idle();
            break;
        case EnemyState::hit:
            enter_hit();
            break;
        case EnemyState::die:
            enter_die();
            break;
        case EnemyState::give_up:
            enter_give_up();
            break;
        case EnemyState::dig:
            enter_dig();
            break;
        case EnemyState::despawn:
            break;
        }
    }

    // BaseZombie.TimeToIntercept.
    [[nodiscard]] float time_to_intercept() const noexcept
    {
        Vec3 offset = subtract(enemy.position, context->target.position);
        offset.y = 0.0F;
        const float distance_squared = length_squared(offset);
        if (distance_squared < 1.0F)
            return 0.0F;
        Vec3 closing = subtract(context->target.velocity, enemy.velocity);
        closing.y = 0.0F;
        if (dot(offset, closing) < 0.0F)
            return std::numeric_limits<float>::max();
        float speed = length_squared(closing);
        if (speed < 0.001F)
            return std::numeric_limits<float>::max();
        speed = std::sqrt(speed);
        const float inverse_speed = 1.0F / speed;
        closing = {closing.x * inverse_speed, closing.y * inverse_speed, closing.z * inverse_speed};
        const float distance = std::sqrt(distance_squared);
        const float inverse_distance = 1.0F / distance;
        offset = {offset.x * inverse_distance, offset.y * inverse_distance,
                  offset.z * inverse_distance};
        const float alignment = dot(closing, offset);
        if (alignment < 0.01F)
            return std::numeric_limits<float>::max();
        return distance / alignment / speed;
    }

    // How far the target may get before a walking enemy stops chasing.
    // 1.6.3.8: 40 for the Alien, 25 for everything else. 1.9.9.8:
    // BaseZombie.PlayerDistanceLimit, i.e. EnemyType.StartingDistanceLimit
    // (Alien and Felguard 40, otherwise 25) unless a spawner block made it.
    [[nodiscard]] float chase_distance_limit() const noexcept
    {
        if (context->modern_rules)
        {
            if (enemy.spawner_sourced)
                return enemy.hell_spawner ? 300.0F : 100.0F;
            if (stats.enemy_class == EnemyClass::felguard)
                return 40.0F;
        }
        return stats.enemy_class == EnemyClass::alien ? 40.0F : 25.0F;
    }

    void request_speed_up()
    {
        if (context->events != nullptr)
            context->events->speed_up_requests.push_back({enemy.target_gid, enemy.id});
    }

    // ZombieChase.Update (also the Alien, Felguard and cave-skeleton chase).
    void update_chase()
    {
        const float delta = context->delta_seconds;
        const PlayerState &target = context->target;
        Vec3 velocity = enemy.velocity;
        velocity.x = 0.0F;
        velocity.z = 0.0F;
        Vec3 offset = subtract(target.position, enemy.position);
        const float height = offset.y;
        offset.y = 0.0F;
        const float distance = std::sqrt(length_squared(offset));
        if (distance < 5.0F)
        {
            if (time_to_intercept() < 1.0F / enemy.current_speed)
            {
                // Returns without touching the velocity: an airborne enemy
                // carries its speed into the swing.
                if (std::abs(height) > 4.0F && enemy.on_ground && target.grounded)
                    change(dig_state(stats.enemy_class));
                else
                    change(EnemyState::attack);
                return;
            }
        }
        if (enemy.frustration <= 0.0F)
        {
            change(dig_state(stats.enemy_class));
            enemy.velocity = velocity;
            return;
        }
        if (stats.has_run_fast && !enemy.running && context->local_target)
        {
            // One SpeedUpEnemyMessage per frame and per expired timer until
            // the echo arrives, exactly as the original floods them.
            enemy.normal_activation_time -= delta;
            if (enemy.normal_activation_time <= 0.0F || context->zombie_fest)
                request_speed_up();
            Vec3 target_velocity = target.velocity;
            target_velocity.y = 0.0F;
            if (length_squared(target_velocity) > 3.5F)
            {
                enemy.run_activation_time -= delta;
                if (enemy.run_activation_time < 0.0F)
                    request_speed_up();
            }
        }
        if (length_squared(offset) < 0.001F)
            offset = {};
        else
        {
            const float inverse = 1.0F / std::sqrt(length_squared(offset));
            offset = {offset.x * inverse, 0.0F, offset.z * inverse};
        }
        float speed = enemy.current_speed;
        if (!enemy.on_ground)
            speed *= enemy.running ? 1.0F : 0.5F;
        else if (enemy.touching_wall)
            velocity.y += enemy.running ? stats.fast_jump_speed : 10.0F;
        velocity.x = offset.x * speed;
        velocity.z = offset.z * speed;
        enemy.velocity = velocity;
        velocity.y = 0.0F;
        if (length_squared(velocity) > 0.2F)
            enemy.yaw = heading(velocity.x, velocity.z);
        if (!(distance >= 5.0F))
            return;
        // Both exits store the FLATTENED velocity: a leap is cut short.
        if (enemy.running)
        {
            if (time_to_intercept() > 8.0F)
            {
                change(dig_state(stats.enemy_class));
                enemy.velocity = velocity;
            }
        }
        else if (distance > chase_distance_limit())
        {
            change(dig_state(stats.enemy_class));
            enemy.velocity = velocity;
        }
    }

    // ArcherSkeletonChase.Update.
    void update_archer_chase()
    {
        const float delta = context->delta_seconds;
        const PlayerState &target = context->target;
        enemy.state_timer -= delta;
        Vec3 velocity = enemy.velocity;
        velocity.x = 0.0F;
        velocity.z = 0.0F;
        Vec3 offset = subtract(target.position, enemy.position);
        offset.y = 0.0F;
        const float distance = std::sqrt(length_squared(offset));
        if (distance < 35.0F && enemy.state_timer <= 0.0F)
        {
            enemy.state_timer = 0.5F;
            const Vec3 eye{enemy.position.x, enemy.position.y + 1.5F, enemy.position.z};
            const Vec3 sight{target.position.x - enemy.position.x,
                             target.position.y - enemy.position.y,
                             target.position.z - enemy.position.z};
            const float reach = std::sqrt(length_squared(sight));
            if (reach <= 0.0F ||
                !PlayerSimulation::raycast(context->region, eye, sight, reach).has_value())
            {
                if (length_squared(offset) > 0.2F)
                    enemy.yaw = heading(offset.x, offset.z);
                change(EnemyState::attack);
                return;
            }
        }
        if (enemy.frustration < 0.0F)
        {
            change(EnemyState::give_up);
            enemy.velocity = velocity;
            return;
        }
        if (length_squared(offset) < 0.001F)
            offset = {};
        else
        {
            const float inverse = 1.0F / std::sqrt(length_squared(offset));
            offset = {offset.x * inverse, 0.0F, offset.z * inverse};
        }
        float speed = enemy.current_speed;
        if (!enemy.on_ground)
            speed *= 0.5F;
        else if (enemy.touching_wall)
            velocity.y += 10.0F;
        velocity.x = offset.x * speed;
        velocity.z = offset.z * speed;
        enemy.velocity = velocity;
        velocity.y = 0.0F;
        if (length_squared(velocity) > 0.2F)
            enemy.yaw = heading(velocity.x, velocity.z);
    }

    // BaseTryAttack.Update.
    void update_attack()
    {
        const PlayerState &target = context->target;
        if (enemy.on_ground)
            zero_velocity();
        else
            reduce_velocity();
        const EnemyAttackSet &attacks = enemy_attacks(stats.enemy_class);
        if (near_end())
        {
            // Every completed swing has reset HitCount to zero by now, so each
            // one counts as a "miss" and the rage clip follows the budget.
            if (enemy.hit_count == 0)
                --enemy.miss_count;
            Vec3 offset = subtract(target.position, enemy.position);
            const float height = offset.y;
            offset.y = 0.0F;
            if (enemy.miss_count <= 0)
            {
                if (std::abs(height) > 1.5F && target.grounded && enemy.on_ground)
                {
                    change(dig_state(stats.enemy_class));
                    return;
                }
                enemy.miss_count = enemy.instance_rng.next_int(1, 3);
                enemy.hit_count = 1;
                enemy.attack_index = -1;
                play(attacks.rage_clip, false);
                return;
            }
            const float distance = std::sqrt(length_squared(offset));
            if (distance >= 1.0F)
            {
                change(EnemyState::chase);
                return;
            }
            if (distance > 0.1F)
                enemy.yaw = heading(offset.x, offset.z);
            // Only Enter applies AttackAnimationSpeed: every follow-up swing
            // plays at the new AnimationPlayer's speed of one.
            play(random_attack(), false);
            enemy.hit_count = 0;
            enemy.swing_count = 2 + enemy.instance_rng.next_int(0, 3);
            return;
        }
        if (enemy.attack_index == -1)
            return;
        if (!context->local_target)
        {
            observe_remote_swing(attacks);
            return;
        }
        const EnemyAttack &attack = attacks.attacks[enemy.attack_index];
        // The authored hit moment is divided by the playback speed although
        // the clip clock already runs at that speed.
        const float moment =
            attack.hit_times[static_cast<std::size_t>(enemy.hit_count)] / enemy.clip_speed;
        if (!(clip_seconds() >= static_cast<double>(moment)))
            return;
        if (swing_lands(attack, attacks) && context->events != nullptr)
        {
            context->events->player_damage += attack.damage;
            context->events->player_damage_sources.push_back(enemy.position);
            context->events->strikes.push_back(
                {enemy.target_gid, enemy.id, attack.damage, enemy.position}); // (Authority)
        }
        ++enemy.hit_count;
        if (enemy.hit_count == attack.hit_count)
        {
            enemy.attack_index = -1;
            enemy.hit_count = 0;
        }
    }

    // BaseTryAttack's hit test against the target as this peer has it:
    // within 1.2 vertically, inside the swing's range and facing it.
    [[nodiscard]] bool swing_lands(const EnemyAttack &attack, const EnemyAttackSet &attacks) const
    {
        Vec3 reach = subtract(context->target.position, enemy.position);
        if (!(std::abs(reach.y) < 1.2F))
            return false;
        reach.y = 0.0F;
        const float squared = length_squared(reach);
        if (static_cast<double>(squared) < 0.05)
            return true;
        if (!(squared < attack.range * attack.range))
            return false;
        const float inverse = 1.0F / std::sqrt(squared);
        const float facing = (reach.x * inverse * std::sin(enemy.yaw) +
                              reach.z * inverse * -std::cos(enemy.yaw)) *
                             attacks.hit_dot_multiplier;
        return facing > 0.7F;
    }

    // (Authority) A remote target's swing as its own peer would test it, at
    // the hit moments this frame's clock crossed. Nothing of the enemy is
    // touched: on this peer HitCount stays where retail leaves it.
    void observe_remote_swing(const EnemyAttackSet &attacks)
    {
        if (!context->observe_remote || context->events == nullptr)
            return;
        const EnemyAttack &attack = attacks.attacks[enemy.attack_index];
        const double now = clip_seconds();
        const double before = now - static_cast<double>(context->delta_seconds);
        for (int hit = 0; hit < attack.hit_count; ++hit)
        {
            const double moment = static_cast<double>(
                attack.hit_times[static_cast<std::size_t>(hit)] / enemy.clip_speed);
            if (before < moment && now >= moment && swing_lands(attack, attacks))
                context->events->remote_strikes.push_back(
                    {enemy.target_gid, enemy.id, attack.damage, enemy.position});
        }
    }

    // ArcherSkeletonTryAttack.Update.
    void update_archer_attack()
    {
        const PlayerState &target = context->target;
        if (near_end())
        {
            change(enemy.swing_count <= 0 ? EnemyState::give_up : EnemyState::idle);
            return;
        }
        Vec3 offset = subtract(target.position, enemy.position);
        offset.y = 0.0F;
        if (length_squared(offset) > 0.2F)
            enemy.yaw = heading(offset.x, offset.z);
        if (!(clip_seconds() > 1.100000023841858) || enemy.hit_count != 0)
            return;
        enemy.hit_count = 1;
        Vec3 arrow_origin = enemy.position;
        arrow_origin.y += 1.5F;
        Vec3 aim = target.position;
        aim.y += 0.5F;
        Vec3 flight{};
        if (const auto ballistic = initial_ballistic_vector(arrow_origin, aim, 25.0F, -10.0F);
            ballistic.has_value())
        {
            flight = *ballistic;
        }
        else
        {
            // The unreachable-target fallback uses the unadjusted origins.
            flight = subtract(target.position, enemy.position);
            const float squared = length_squared(flight);
            if (squared < 0.001F)
                flight = {0.0F, 25.0F, 0.0F};
            else
            {
                const float scale = 25.0F / std::sqrt(squared);
                flight = {flight.x * scale, flight.y * scale, flight.z * scale};
            }
        }
        if (context->events != nullptr)
        {
            context->events->arrow_origins.push_back(arrow_origin);
            context->events->arrow_velocities.push_back(flight);
            context->events->arrow_target_gids.push_back(enemy.target_gid);
            ++context->events->arrows_fired;
        }
    }

    enum class BreakResult : std::uint8_t
    {
        will_break,
        will_not_break,
        broken,
        region_is_empty,
    };

    // Explosive.EnemyBreakBlocks. Every peer flips the coin for every swing;
    // only the target's own peer turns a break into a RemoveBlocksMessage, so
    // watchers legitimately disagree about which swing "broke" something.
    [[nodiscard]] BreakResult break_blocks(int minimum_x, int minimum_y, int minimum_z,
                                           int maximum_x, int maximum_y, int maximum_z, int hits)
    {
        const WorldRegion &region = context->region;
        minimum_x = std::max(minimum_x, region.minimum_x());
        minimum_z = std::max(minimum_z, region.minimum_z());
        minimum_y = std::max(minimum_y, world_min_y);
        maximum_x = std::min(maximum_x, region.maximum_x());
        maximum_z = std::min(maximum_z, region.maximum_z());
        maximum_y = std::min(maximum_y, world_min_y + chunk_height - 1);
        bool diggable{};
        bool broken{};
        bool touched{};
        for (int z = minimum_z; z <= maximum_z; ++z)
            for (int x = minimum_x; x <= maximum_x; ++x)
                for (int y = minimum_y; y <= maximum_y; ++y)
                {
                    const BlockType block = region.block(x, y, z);
                    // (mod-liquid) Nor can a liquid be dug out.
                    if (block == BlockType::empty || block == BlockType::uninitialized ||
                        liquid_at(region, x, y, z))
                        continue;
                    if (blocks_player(block))
                        touched = true;
                    const int hardness = enemy_dig_hardness(block);
                    if (hardness > stats.hardest_block_dug)
                        continue;
                    diggable = true;
                    if (hits > hardness && context->dig_random != nullptr &&
                        context->dig_random->next_int(0, 2) == 0)
                    {
                        broken = true;
                        // Upper door halves count as broken but are removed
                        // with their lower half.
                        if (context->local_target && !is_upper_door(block) &&
                            context->events != nullptr)
                            context->events->dug_blocks.push_back({x, y, z});
                    }
                    else if (context->local_target && context->events != nullptr &&
                             (block == BlockType::tnt || block == BlockType::c4))
                    {
                        context->events->fuse_blocks.push_back({x, y, z});
                    }
                }
        if (broken)
            return BreakResult::broken;
        if (!touched)
            return BreakResult::region_is_empty;
        return diggable ? BreakResult::will_break : BreakResult::will_not_break;
    }

    // ZombieDig.Update.
    void update_dig()
    {
        const PlayerState &target = context->target;
        if (enemy.on_ground)
            zero_velocity();
        else
            reduce_velocity();
        if (near_end())
        {
            if (enemy.miss_count == 4)
            {
                change(EnemyState::give_up);
                return;
            }
            if (enemy.miss_count == 0 || (enemy.miss_count & 2) != 0)
            {
                change(EnemyState::chase);
                return;
            }
            Vec3 offset = subtract(target.position, enemy.position);
            const float height = offset.y;
            offset.y = 0.0F;
            const float distance = std::sqrt(length_squared(offset));
            if (distance < 1.0F && std::abs(height) < 1.5F)
            {
                change(EnemyState::attack);
                return;
            }
            const bool from_spawner = context->modern_rules && enemy.spawner_sourced;
            if (context->local_target && (distance > (from_spawner ? 48.0F : 16.0F) ||
                                          std::abs(height) > (from_spawner ? 24.0F : 8.0F)))
            {
                change(EnemyState::give_up);
                return;
            }
            enemy.yaw = heading(offset.x, offset.z);
            play(random_attack(), false);
            enemy.hit_count = 0;
            enemy.miss_count = 0;
            return;
        }
        if (enemy.attack_index == -1)
            return;
        const EnemyAttackSet &attacks = enemy_attacks(stats.enemy_class);
        const EnemyAttack &attack = attacks.attacks[enemy.attack_index];
        const float moment =
            attack.hit_times[static_cast<std::size_t>(enemy.hit_count)] / enemy.clip_speed;
        if (!(clip_seconds() >= static_cast<double>(moment)))
            return;
        // A 2x3x2 volume leaning toward the target.
        int cell_x = static_cast<int>(std::floor(enemy.position.x));
        int cell_y = static_cast<int>(std::floor(enemy.position.y));
        int cell_z = static_cast<int>(std::floor(enemy.position.z));
        if (target.position.y >= enemy.position.y + 1.0F)
            ++cell_y;
        else if (target.position.y <= enemy.position.y - 1.0F)
            --cell_y;
        int minimum_x = cell_x;
        int maximum_x = cell_x;
        int minimum_z = cell_z;
        int maximum_z = cell_z;
        if (target.position.x > enemy.position.x)
            ++maximum_x;
        else
            --minimum_x;
        if (target.position.z >= enemy.position.z)
            ++maximum_z;
        else
            --minimum_z;
        ++enemy.swing_count;
        const int hits =
            static_cast<int>(static_cast<float>(enemy.swing_count) * stats.digging_multiplier);
        const BreakResult result =
            break_blocks(minimum_x, cell_y, minimum_z, maximum_x, cell_y + 2, maximum_z, hits);
        switch (result)
        {
        case BreakResult::broken:
            enemy.miss_count |= 2;
            break;
        case BreakResult::will_break:
            enemy.miss_count |= 1;
            break;
        case BreakResult::will_not_break:
            enemy.miss_count |= 4;
            break;
        case BreakResult::region_is_empty:
            break;
        }
        // The loud "ZombieDig" scrape on every swing that met something.
        if (result != BreakResult::region_is_empty && context->events != nullptr)
            context->events->dig_swings.push_back(enemy.position);
        ++enemy.hit_count;
        if (enemy.hit_count == attack.hit_count)
        {
            enemy.attack_index = -1;
            enemy.hit_count = 0;
        }
    }

    void update_state()
    {
        switch (enemy.state)
        {
        case EnemyState::emerge:
            if (near_end())
                change(EnemyState::chase);
            break;
        case EnemyState::chase:
            if (stats.enemy_class == EnemyClass::archer)
                update_archer_chase();
            else
                update_chase();
            break;
        case EnemyState::attack:
            if (stats.enemy_class == EnemyClass::archer)
                update_archer_attack();
            else
                update_attack();
            break;
        case EnemyState::idle:
            enemy.state_timer -= context->delta_seconds;
            if (enemy.state_timer <= 0.0F)
                change(EnemyState::chase);
            break;
        case EnemyState::hit:
            // ZombieHit is never restartable, so Restart always goes to the
            // chase state, whatever the enemy was doing when it was struck.
            if (near_end())
                change(EnemyState::chase);
            break;
        case EnemyState::die:
            if (finished())
            {
                enemy.frustration -= context->delta_seconds;
                if (enemy.frustration < 0.0F)
                {
                    if (context->events != nullptr)
                        ++context->events->died;
                    remove();
                }
            }
            break;
        case EnemyState::give_up:
            if (finished())
                remove();
            break;
        case EnemyState::dig:
            update_dig();
            break;
        case EnemyState::despawn:
            break;
        }
    }

    // AnimationPlayer.Update: the step is TimeSpan.FromSeconds(dt * Speed),
    // which rounds to whole milliseconds. At 60 Hz a speed-1 clip therefore
    // advances 17 ms per frame, a speed-2 clip 33 ms and a speed-3 clip 50 ms.
    void advance_clip(float delta_seconds) noexcept
    {
        const double step_seconds =
            static_cast<double>(delta_seconds) * static_cast<double>(enemy.clip_speed);
        const double milliseconds = step_seconds * 1000.0 + (step_seconds >= 0.0 ? 0.5 : -0.5);
        enemy.clip_ticks += static_cast<std::int64_t>(milliseconds) * 10'000;
        const std::int64_t duration = enemy.clip_duration_ticks;
        if (enemy.clip_ticks > duration)
        {
            if (enemy.clip_loops && duration > 0)
            {
                while (enemy.clip_ticks > duration)
                    enemy.clip_ticks -= duration;
            }
            else
            {
                enemy.clip_ticks = duration;
            }
        }
        else if (enemy.clip_ticks < 0)
        {
            enemy.clip_ticks = 0;
        }
        enemy.clip_time = static_cast<float>(static_cast<double>(enemy.clip_ticks) /
                                             static_cast<double>(ticks_per_second));
    }
};

} // namespace

std::int64_t stock_clip_ticks(EnemyModelKind model, std::string_view clip) noexcept
{
    switch (model)
    {
    case EnemyModelKind::zombie:
        return find_clip(zombie_clips, clip);
    case EnemyModelKind::felguard:
        return find_clip(felguard_clips, clip);
    case EnemyModelKind::alien:
        return find_clip(alien_clips, clip);
    case EnemyModelKind::skeleton_zombie:
    case EnemyModelKind::skeleton_archer:
    case EnemyModelKind::skeleton_axes:
    case EnemyModelKind::skeleton_sword:
        return find_clip(skeleton_clips, clip);
    case EnemyModelKind::count:
        break;
    }
    return 0;
}

void spawn(Enemy &enemy, const ClipDurationLookup *clip_durations)
{
    const EnemyStats &stats = enemy_stats(enemy.type);
    enemy.velocity = {};
    enemy.yaw = 0.0F;
    enemy.on_ground = false;
    enemy.touching_wall = false;
    enemy.running = false;
    enemy.frustration = 15.0F;
    enemy.current_speed = enemy.slow_speed;
    enemy.state_timer = 0.0F;
    enemy.blocking = false;
    enemy.hittable = false;
    Machine machine{enemy, stats, clip_durations, nullptr};
    // SkeletonEnemyType.GetEmergeState is the chase state itself: cave
    // skeletons have no stand-up clip and consume no random draw.
    machine.change(is_skeleton_melee(stats.enemy_class) ? EnemyState::chase : EnemyState::emerge);
    enemy.previous_state = enemy.state;
}

void advance_physics(const WorldRegion &region, Enemy &enemy, float delta_seconds)
{
    // BasicPhysics.Accelerate: WorldAcceleration is plain gravity, with no
    // terminal velocity.
    enemy.velocity.y += gravity * delta_seconds;

    // BaseZombie.ResolveCollsion: up to four slide iterations of the swept box.
    float remaining = delta_seconds;
    Vec3 position = enemy.position;
    Vec3 velocity = enemy.velocity;
    const Vec3 original_velocity = velocity;
    enemy.on_ground = false;
    enemy.touching_wall = false;
    int iterations{};
    bool collides{};
    do
    {
        const Vec3 start = position;
        const Vec3 step{velocity.x * remaining, velocity.y * remaining, velocity.z * remaining};
        position = {start.x + step.x, start.y + step.y, start.z + step.z};
        const MovementTrace trace = trace_movement_box(region, start, position, enemy_box_minimum,
                                                       enemy_box_maximum, false);
        collides = trace.collides;
        if (collides)
        {
            if (trace.in_face == ProbeFace::positive_y)
                enemy.on_ground = true;
            else
                enemy.touching_wall = true;
            const float travelled = std::max(trace.in_t - 0.001F, 0.0F);
            position = {start.x + step.x * travelled, start.y + step.y * travelled,
                        start.z + step.z * travelled};
            const float into = dot(trace.in_normal, velocity);
            velocity = {velocity.x - trace.in_normal.x * into, velocity.y - trace.in_normal.y * into,
                        velocity.z - trace.in_normal.z * into};
            remaining *= 1.0F - travelled;
            if (remaining <= 1e-07F)
                break;
            if (length_squared(velocity) <= 1e-06F || dot(original_velocity, velocity) <= 1e-06F)
            {
                velocity = {};
                break;
            }
        }
        ++iterations;
    } while (collides && iterations < 4);
    if (iterations == 4)
        velocity = {};
    enemy.position = position;
    enemy.velocity = velocity;
    // Frustration: standing still (under half a block per second) drains it,
    // any real movement resets it to two and a half seconds.
    if (velocity.x * velocity.x + velocity.z * velocity.z < 0.25F)
        enemy.frustration -= delta_seconds;
    else
        enemy.frustration = 2.5F;
}

void update(Enemy &enemy, Context &context)
{
    Machine machine{enemy, enemy_stats(enemy.type), context.clip_durations, &context};
    // `!Target.ValidGamer || Target.Dead` -> GiveUp(), on every peer.
    if (!context.target_alive && enemy.state != EnemyState::die &&
        enemy.state != EnemyState::give_up)
        machine.change(EnemyState::give_up);
    machine.update_state();
    if (enemy.state != EnemyState::despawn)
        machine.advance_clip(context.delta_seconds);
}

void speed_up(Enemy &enemy, const ClipDurationLookup *clip_durations)
{
    const EnemyStats &stats = enemy_stats(enemy.type);
    if (!stats.has_run_fast)
        return;
    enemy.running = true;
    enemy.current_speed = enemy.fast_speed;
    // Only the chase state reacts (HandleSpeedUp restarts the gait clip).
    if (enemy.state == EnemyState::chase && stats.enemy_class != EnemyClass::archer)
    {
        Machine machine{enemy, stats, clip_durations, nullptr};
        machine.start_move_animation();
    }
}

void give_up(Enemy &enemy, const ClipDurationLookup *clip_durations)
{
    if (enemy.state == EnemyState::die || enemy.state == EnemyState::give_up)
        return;
    Machine machine{enemy, enemy_stats(enemy.type), clip_durations, nullptr};
    machine.change(EnemyState::give_up);
}

void kill(Enemy &enemy, const ClipDurationLookup *clip_durations)
{
    if (enemy.state == EnemyState::die)
        return;
    Machine machine{enemy, enemy_stats(enemy.type), clip_durations, nullptr};
    machine.change(EnemyState::die);
}

void react_to_hit(Enemy &enemy, const ClipDurationLookup *clip_durations)
{
    if (enemy.state == EnemyState::hit)
        return;
    Machine machine{enemy, enemy_stats(enemy.type), clip_durations, nullptr};
    machine.change(EnemyState::hit);
}

} // namespace openminerz::world::enemy_simulation
