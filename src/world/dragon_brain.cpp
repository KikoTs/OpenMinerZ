#include "openminerz/world/dragon_brain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// Retail DragonEntity, DragonBaseState and the seven dragon states. Comments
// name the original statement each block stands for; nothing here is tuned.

namespace openminerz::world
{

namespace
{

// DragonType's constructor (identical for every type but MaxLoiters).
constexpr float speed = 20.0F;
constexpr float max_accel = 10.0F;
constexpr float yaw_rate = 0.35F;
constexpr float roll_rate = 1.0F;
constexpr float max_roll = 1.0F;
constexpr float pitch_rate = 0.2F;
constexpr float max_pitch = 0.4F;
constexpr float slow_view_check_interval = 2.0F;
constexpr float fast_view_check_interval = 1.0F;
constexpr float max_view_distance = 300.0F;
constexpr float max_attack_distance = 200.0F;
constexpr float min_attack_distance = 50.0F;
constexpr float break_off_strafe_distance = 40.0F;
constexpr float hover_distance = 50.0F;
constexpr float loiter_distance = 200.0F;
constexpr float spawn_distance = 750.0F;
constexpr float cruising_altitude = 120.0F;
constexpr float loiter_altitude = 90.0F;
constexpr float hunting_altitude = 70.0F;
constexpr float strafe_fire_rate = 2.0F;
constexpr float hover_fire_rate = 1.0F;
constexpr float min_loiter_time = 2.0F;
constexpr float max_loiter_time = 6.0F;
constexpr int min_hover_shots = 4;
constexpr int max_hover_shots = 8;
constexpr float chance_of_hover_attack = 0.5F;
constexpr int chances_to_not_attack = 5;
constexpr float shot_hearing_interval = 5.0F;

constexpr std::uint8_t soar = 0U;
constexpr std::uint8_t flap = 1U;
constexpr std::uint8_t attack = 2U;
constexpr std::uint8_t hover = 3U;
constexpr std::uint8_t cry = 1U;

// flying_idle, fly_forward, gethit, Idle: AnimationClip.Duration in ticks.
// 1.6.3.8 and 1.9.9.8 ship different dragon clips (Modern's are a little longer).
constexpr std::int64_t clip_duration_ticks[2][4]{
    {23'000'000, 13'000'000, 16'000'000, 26'333'333},
    {23'330'000, 13'330'000, 16'170'000, 26'670'000},
};

[[nodiscard]] int max_loiters(DragonKind kind) noexcept
{
    return kind == DragonKind::undead ? 100 : 3;
}

// MathHelper.WrapAngle.
[[nodiscard]] float wrap_angle(float angle) noexcept
{
    angle = static_cast<float>(std::remainder(static_cast<double>(angle), 6.2831854820251465));
    if (angle <= -3.14159274F)
        angle += 6.28318548F;
    else if (angle > 3.14159274F)
        angle -= 6.28318548F;
    return angle;
}

// MathTools.MoveTowardTargetAngle.
[[nodiscard]] float move_toward_target_angle(float current, float target, float rate,
                                             float dt) noexcept
{
    if (target == current)
        return current;
    // x87 evaluation, as in dragon_move_toward_target: doubles until a store.
    double step = static_cast<double>(wrap_angle(target - current));
    const double magnitude = std::abs(step);
    if (magnitude > static_cast<double>(rate))
        step *= static_cast<double>(rate) / magnitude;
    const float moved =
        wrap_angle(static_cast<float>(static_cast<double>(current) + step * static_cast<double>(dt)));
    return std::abs(static_cast<double>(moved) - static_cast<double>(target)) <
                   static_cast<double>(0.01F)
               ? target
               : moved;
}

[[nodiscard]] float length(Vec3 value) noexcept
{
    const float squared = value.x * value.x + value.y * value.y + value.z * value.z;
    return static_cast<float>(std::sqrt(static_cast<double>(squared)));
}

[[nodiscard]] float dot(Vec3 left, Vec3 right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] float distance_squared(Vec3 left, Vec3 right) noexcept
{
    const float x = left.x - right.x;
    const float y = left.y - right.y;
    const float z = left.z - right.z;
    return x * x + y * y + z * z;
}

// Quaternion.CreateFromYawPitchRoll, then Matrix.CreateFromQuaternion's
// third row negated: Entity.LocalToWorld.Forward.
[[nodiscard]] Vec3 forward_of(float yaw, float pitch, float roll) noexcept
{
    const auto sine = [](float value) { return static_cast<float>(std::sin(static_cast<double>(value))); };
    const auto cosine = [](float value) { return static_cast<float>(std::cos(static_cast<double>(value))); };
    const float half_roll = roll * 0.5F;
    const float sr = sine(half_roll);
    const float cr = cosine(half_roll);
    const float half_pitch = pitch * 0.5F;
    const float sp = sine(half_pitch);
    const float cp = cosine(half_pitch);
    const float half_yaw = yaw * 0.5F;
    const float sy = sine(half_yaw);
    const float cy = cosine(half_yaw);
    const float x = cy * sp * cr + sy * cp * sr;
    const float y = sy * cp * cr - cy * sp * sr;
    const float z = cy * cp * sr - sy * sp * cr;
    const float w = cy * cp * cr + sy * sp * sr;
    const float m31 = 2.0F * (z * x + y * w);
    const float m32 = 2.0F * (y * z - x * w);
    const float m33 = 1.0F - 2.0F * (y * y + x * x);
    return {-m31, -m32, -m33};
}

// DragonBaseState.GetHeading / MakeYawVector.
[[nodiscard]] float get_heading(Vec3 forward, float default_heading) noexcept
{
    if (forward.x != 0.0F || forward.z != 0.0F)
        return static_cast<float>(std::atan2(static_cast<double>(0.0F - forward.x),
                                             static_cast<double>(0.0F - forward.z)));
    return default_heading;
}

[[nodiscard]] Vec3 make_yaw_vector(float yaw) noexcept
{
    return {static_cast<float>(0.0 - std::sin(static_cast<double>(yaw))), 0.0F,
            static_cast<float>(0.0 - std::cos(static_cast<double>(yaw)))};
}

[[nodiscard]] std::int64_t ticks_of(float seconds) noexcept
{
    return std::llround(static_cast<double>(seconds) * 10'000'000.0);
}

// The CurrentAnimation setter: PlaySingleClip starts a fresh AnimationPlayer.
void set_current_animation(DragonBrainState &entity, std::uint8_t animation) noexcept
{
    entity.current_animation = animation;
    entity.next_animation = animation;
    entity.clip_ticks = 0;
}

[[nodiscard]] Vec3 eye_of(const DragonBrainState &entity) noexcept
{
    return {entity.position.x + entity.forward.x * 7.0F, entity.position.y + entity.forward.y * 7.0F,
            entity.position.z + entity.forward.z * 7.0F};
}

// DragonBaseState.CanSeePosition.
[[nodiscard]] bool can_see_position(const DragonBrainState &entity, DragonBrainSession &session,
                                    Vec3 target)
{
    return session.can_see(eye_of(entity), target);
}

// DragonBaseState.DoViewCheck / CalculateNextCheckInterval.
[[nodiscard]] bool do_view_check(DragonBrainState &entity, DragonBrainSession &session, float dt,
                                 float interval)
{
    entity.time_left_before_next_view_check -= dt;
    if (entity.time_left_before_next_view_check <= 0.0F)
    {
        const float minimum = interval * 0.75F;
        const float maximum = interval * 1.25F;
        entity.time_left_before_next_view_check +=
            minimum + (maximum - minimum) * session.random_float();
        return true;
    }
    return false;
}

// DragonBaseState.SearchForNewTarget (with FindBestTarget inlined as
// dragon_find_best_target over the session's players).
[[nodiscard]] bool search_for_new_target(DragonBrainState &entity, DragonBrainSession &session,
                                         float dt, DragonBrainOutput &output)
{
    if (do_view_check(entity, session, dt, slow_view_check_interval))
    {
        const std::vector<DragonTargetCandidate> players = session.players(eye_of(entity));
        const DragonTargetChoice choice =
            dragon_find_best_target(entity.position, entity.forward, players);
        if (choice.remove_dragon)
        {
            entity.removed = true;
            ++output.remove_requests;
        }
        if (choice.gid.has_value())
        {
            entity.target = choice.gid;
            for (const DragonTargetCandidate &player : players)
                if (player.gid == *choice.gid)
                    entity.travel_target = player.position;
            if (entity.chances_to_not_attack != 0)
                entity.next_sound = cry;
            entity.chances_to_not_attack = 0;
            entity.time_left_til_shots_heard = 0.0F;
            if (*entity.target != session.local_gid() && session.local_valid() &&
                distance_squared(entity.travel_target, session.local_position()) > 22'500.0F)
            {
                entity.migrate_dragon_to = entity.target;
                entity.migrate_dragon = true;
            }
            return true;
        }
    }
    if (entity.chances_to_not_attack != 0)
    {
        entity.time_left_til_shots_heard -= dt;
        if (entity.time_left_til_shots_heard <= 0.0F)
        {
            const float reach = max_view_distance * max_view_distance * 2.25F;
            float nearest = std::numeric_limits<float>::max();
            Vec3 travel_target{};
            const Vec3 from = entity.chances_to_not_attack == chances_to_not_attack
                                  ? entity.position
                                  : entity.travel_target;
            for (const Vec3 gunshot : entity.gunshots)
            {
                const float squared = distance_squared(from, gunshot);
                if (squared < reach && squared < nearest && session.region_loaded(gunshot))
                {
                    nearest = squared;
                    travel_target = gunshot;
                }
            }
            if (nearest != std::numeric_limits<float>::max())
            {
                --entity.chances_to_not_attack;
                if (entity.chances_to_not_attack == 0)
                    entity.next_sound = cry;
                entity.travel_target = travel_target;
                entity.target.reset();
                entity.time_left_til_shots_heard = shot_hearing_interval;
                return true;
            }
        }
    }
    return false;
}

// DragonBaseState.SteerTowardTarget.
[[nodiscard]] float steer_toward_target(DragonBrainState &entity, DragonBrainSession &session,
                                        Vec3 &dest)
{
    if (entity.target.has_value())
        if (const auto target = session.living_position(*entity.target); target.has_value())
            if (can_see_position(entity, session, {target->x, target->y + 1.5F, target->z}))
                entity.travel_target = *target;
    dest = {entity.travel_target.x - entity.position.x, 0.0F,
            entity.travel_target.z - entity.position.z};
    const float distance = length(dest);
    if (distance > 10.0F)
        entity.target_yaw = wrap_angle(get_heading(dest, entity.target_yaw));
    return distance;
}

void update_state(DragonBrainState &entity, DragonBrainSession &session, float dt,
                  DragonBrainOutput &output);

// `Target != null && !Target.ValidLivingGamer`.
[[nodiscard]] bool target_is_gone(const DragonBrainState &entity, DragonBrainSession &session)
{
    return entity.target.has_value() && !session.living_position(*entity.target).has_value();
}

// DragonDefaultState.Enter (the only Enter that reads nothing outside the entity).
void enter_default_state(DragonBrainState &entity) noexcept
{
    entity.time_left_before_next_view_check = slow_view_check_interval;
    entity.target_velocity = speed;
    const Vec3 heading = make_yaw_vector(entity.default_heading);
    float distance = spawn_distance;
    if (entity.first_time_for_default_state)
        distance *= 2.0F;
    entity.first_time_for_default_state = false;
    entity.target.reset();
    entity.travel_target = {entity.position.x + heading.x * distance,
                            entity.position.y + heading.y * distance,
                            entity.position.z + heading.z * distance};
    entity.target_yaw = entity.default_heading;
    entity.target_altitude = cruising_altitude;
    entity.had_target_this_pass = false;
    entity.loiters_left = 3;
    entity.chances_to_not_attack = chances_to_not_attack;
}

void enter_state(DragonBrainState &entity, DragonBrainMode mode, DragonBrainSession &session)
{
    switch (mode)
    {
    case DragonBrainMode::default_state:
        enter_default_state(entity);
        break;
    case DragonBrainMode::loiter:
        if (!entity.target.has_value())
        {
            --entity.loiters_left;
            // The nested ChangeState returns here, so a dragon that gives up
            // still gets the two loiter assignments below.
            if (entity.loiters_left == 0)
                dragon_brain_change_state(entity, DragonBrainMode::default_state, session);
        }
        else
        {
            entity.target.reset();
            entity.loiters_left = max_loiters(entity.kind);
        }
        entity.target_altitude = loiter_altitude;
        entity.target_velocity = speed;
        break;
    case DragonBrainMode::loiter_left:
    case DragonBrainMode::loiter_right:
        entity.loiter_timer =
            min_loiter_time + (max_loiter_time - min_loiter_time) * session.random_float();
        if (entity.chances_to_not_attack != 0)
            entity.loiter_timer *= 1.5F;
        break;
    case DragonBrainMode::hover:
        entity.shots_left = session.random_int(min_hover_shots, max_hover_shots);
        entity.time_left_before_next_shot =
            hover_fire_rate + hover_fire_rate * session.random_float();
        entity.target_velocity = 0.25F;
        entity.next_animation = hover;
        break;
    case DragonBrainMode::hover_attack:
        entity.time_left_before_next_view_check = fast_view_check_interval;
        entity.target_altitude = hunting_altitude;
        entity.target_velocity = speed;
        break;
    case DragonBrainMode::strafe_attack:
        entity.time_left_before_next_view_check = fast_view_check_interval;
        entity.target_altitude = hunting_altitude;
        entity.time_left_before_next_shot = strafe_fire_rate;
        entity.target_velocity = speed;
        break;
    }
}

// DragonLoiterLeftState / DragonLoiterRightState.GetNewYaw.
[[nodiscard]] float loiter_yaw(const DragonBrainState &entity, Vec3 dest) noexcept
{
    const float distance = length(dest);
    const bool left = entity.mode == DragonBrainMode::loiter_left;
    float heading = get_heading(dest, 0.0F) + (left ? 1.5707964F : -1.5707964F);
    const float outside = std::min(1.5F, (distance - loiter_distance) / 30.0F);
    const float inside = std::min(1.5F, (loiter_distance - distance) / 20.0F);
    if (distance > loiter_distance)
        heading = left ? heading + outside : heading - outside;
    else
        heading = left ? heading - inside : heading + inside;
    return wrap_angle(heading);
}

void update_state(DragonBrainState &entity, DragonBrainSession &session, float dt,
                  DragonBrainOutput &output)
{
    switch (entity.mode)
    {
    case DragonBrainMode::default_state: {
        if (entity.removed)
            return;
        if (search_for_new_target(entity, session, dt, output))
        {
            dragon_brain_change_state(entity, dragon_brain_next_attack_type(entity, session),
                                      session);
            update_state(entity, session, dt, output);
            return;
        }
        Vec3 dest{};
        if (steer_toward_target(entity, session, dest) < 10.0F)
        {
            entity.removed = true;
            ++output.remove_requests;
        }
        return;
    }
    case DragonBrainMode::loiter: {
        const Vec3 away{entity.position.x - entity.travel_target.x, 0.0F,
                        entity.position.z - entity.travel_target.z};
        const Vec3 level{entity.forward.x, 0.0F, entity.forward.z};
        if (dot(level, away) < 0.0F)
            return;
        if (!entity.target.has_value() && search_for_new_target(entity, session, dt, output))
        {
            if (entity.target.has_value())
            {
                dragon_brain_change_state(entity, dragon_brain_next_attack_type(entity, session),
                                          session);
                update_state(entity, session, dt, output);
            }
        }
        else if (length(away) > break_off_strafe_distance)
            dragon_brain_change_state(entity,
                                      session.random_int(0, 2) == 0 ? DragonBrainMode::loiter_left
                                                                    : DragonBrainMode::loiter_right,
                                      session);
        return;
    }
    case DragonBrainMode::loiter_left:
    case DragonBrainMode::loiter_right: {
        if (!entity.target.has_value() && search_for_new_target(entity, session, dt, output))
        {
            dragon_brain_change_state(entity, dragon_brain_next_attack_type(entity, session),
                                      session);
            update_state(entity, session, dt, output);
            return;
        }
        const Vec3 dest{entity.position.x - entity.travel_target.x, 0.0F,
                        entity.position.z - entity.travel_target.z};
        if (entity.loiter_timer != std::numeric_limits<float>::max())
        {
            entity.loiter_timer -= dt;
            if (entity.loiter_timer < 0.0F)
            {
                dragon_brain_change_state(entity, dragon_brain_next_attack_type(entity, session),
                                          session);
                update_state(entity, session, dt, output);
                return;
            }
        }
        entity.target_yaw = loiter_yaw(entity, dest);
        return;
    }
    case DragonBrainMode::hover: {
        if (target_is_gone(entity, session))
            entity.target.reset();
        if (do_view_check(entity, session, dt, fast_view_check_interval) &&
            entity.target.has_value())
        {
            const Vec3 target = *session.living_position(*entity.target);
            if (can_see_position(entity, session, {target.x, target.y + 1.5F, target.z}))
                entity.travel_target = target;
        }
        entity.target_altitude = hunting_altitude;
        const Vec3 toward{entity.travel_target.x - entity.position.x,
                          entity.travel_target.y - entity.position.y,
                          entity.travel_target.z - entity.position.z};
        entity.target_yaw = wrap_angle(get_heading(toward, entity.target_yaw));
        // Fired at the last position the target was SEEN at, seen now or not.
        entity.shoot_target = entity.travel_target;
        if (entity.shot_pending)
            return;
        entity.time_left_before_next_shot -= dt;
        if (entity.time_left_before_next_shot < 0.0F)
        {
            if (entity.shots_left == 0)
            {
                dragon_brain_change_state(entity, DragonBrainMode::loiter, session);
                update_state(entity, session, dt, output);
            }
            else
            {
                entity.time_left_before_next_shot += hover_fire_rate;
                entity.shot_pending = true;
                --entity.shots_left;
            }
        }
        return;
    }
    case DragonBrainMode::hover_attack: {
        if (target_is_gone(entity, session))
        {
            entity.target.reset();
            entity.migrate_dragon_to.reset();
            entity.migrate_dragon = false;
        }
        if (!entity.target.has_value())
            static_cast<void>(search_for_new_target(entity, session, dt, output));
        Vec3 dest{};
        if (steer_toward_target(entity, session, dest) < hover_distance)
            dragon_brain_change_state(entity, DragonBrainMode::hover, session);
        return;
    }
    case DragonBrainMode::strafe_attack: {
        if (target_is_gone(entity, session))
        {
            entity.target.reset();
            entity.migrate_dragon = false;
            entity.migrate_dragon_to.reset();
        }
        if (!entity.target.has_value())
            static_cast<void>(search_for_new_target(entity, session, dt, output));
        Vec3 dest{};
        const float distance = steer_toward_target(entity, session, dest);
        if (distance < break_off_strafe_distance)
        {
            dragon_brain_change_state(entity, DragonBrainMode::loiter, session);
            return;
        }
        if (!(distance > min_attack_distance))
            return;
        Vec3 level{entity.forward.x, 0.0F, entity.forward.z};
        const float scale =
            1.0F / static_cast<float>(std::sqrt(static_cast<double>(level.x * level.x + level.z * level.z)));
        level = {level.x * scale, 0.0F, level.z * scale};
        if (!(distance < max_attack_distance) || !(dot(dest, level) / distance > 0.95F))
            return;
        entity.shoot_target = entity.travel_target;
        if (entity.shot_pending)
            return;
        entity.time_left_before_next_shot -= dt;
        if (entity.time_left_before_next_shot < 0.0F)
        {
            entity.time_left_before_next_shot += strafe_fire_rate;
            if (entity.chances_to_not_attack == 0)
                entity.shot_pending = true;
        }
        return;
    }
    }
}

} // namespace

const char *dragon_brain_mode_name(DragonBrainMode mode) noexcept
{
    switch (mode)
    {
    case DragonBrainMode::default_state:
        return "DragonDefaultState";
    case DragonBrainMode::loiter:
        return "DragonLoiterState";
    case DragonBrainMode::loiter_left:
        return "DragonLoiterLeftState";
    case DragonBrainMode::loiter_right:
        return "DragonLoiterRightState";
    case DragonBrainMode::hover:
        return "DragonHoverState";
    case DragonBrainMode::hover_attack:
        return "DragonHoverAttackState";
    case DragonBrainMode::strafe_attack:
        return "DragonStrafeAttackState";
    }
    return "";
}

void dragon_brain_change_state(DragonBrainState &entity, DragonBrainMode mode,
                               DragonBrainSession &session)
{
    // DragonHoverState.Exit is the only Exit that does anything.
    if (entity.mode == DragonBrainMode::hover)
    {
        entity.target_velocity = speed;
        entity.next_animation = flap;
    }
    entity.mode = mode;
    enter_state(entity, mode, session);
}

DragonBrainMode dragon_brain_next_attack_type(DragonBrainState &entity,
                                              DragonBrainSession &session)
{
    // The && short-circuits: no random number is drawn while the dragon is
    // still only investigating gunshots.
    if (entity.chances_to_not_attack == 0 && chance_of_hover_attack > session.random_float())
        return DragonBrainMode::hover_attack;
    return DragonBrainMode::strafe_attack;
}

float dragon_brain_spawn_heading(DragonBrainSession &session)
{
    // MathTools.RandomFloat(-PI / 2, PI / 2) = MathHelper.Lerp(min, max, NextDouble).
    const float minimum = -1.5707964F;
    const float maximum = 1.5707964F;
    return minimum + (maximum - minimum) * session.random_float();
}

void dragon_brain_spawn(DragonBrainState &entity, DragonKind kind, Vec3 local_player,
                        float heading) noexcept
{
    const std::uint32_t next_fireball_index = entity.next_fireball_index;
    const bool modern_content = entity.modern_content;
    entity = DragonBrainState{};
    entity.next_fireball_index = next_fireball_index;
    entity.modern_content = modern_content;
    entity.kind = kind;
    entity.velocity = entity.target_velocity = speed;
    entity.chances_to_not_attack = chances_to_not_attack;
    // InitSpawnState.
    const Vec3 direction = make_yaw_vector(heading);
    entity.position = {local_player.x - direction.x * spawn_distance, cruising_altitude,
                       local_player.z - direction.z * spawn_distance};
    entity.target_altitude = cruising_altitude;
    entity.yaw = entity.target_yaw = entity.default_heading = heading;
    entity.pitch = entity.target_pitch = 0.0F;
    set_current_animation(entity, flap);
    entity.forward = forward_of(heading, 0.0F, 0.0F);
    entity.mode = DragonBrainMode::default_state;
    enter_default_state(entity);
}

void dragon_brain_adopt(DragonBrainState &entity, DragonKind kind,
                        const DragonMigrationState &migration, DragonBrainSession &session)
{
    const int update_count = entity.update_count;
    const bool modern_content = entity.modern_content;
    entity = DragonBrainState{};
    entity.update_count = update_count; // a static: only a fresh spawn zeroes it
    entity.modern_content = modern_content;
    entity.kind = kind;
    // InitAfterHostChange.
    entity.dragon_time = migration.next_dragon_time;
    entity.position = migration.position;
    entity.yaw = migration.yaw;
    entity.target_yaw = migration.target_yaw;
    entity.roll = migration.roll;
    entity.target_roll = migration.target_roll;
    entity.pitch = migration.pitch;
    entity.target_pitch = migration.target_pitch;
    entity.default_heading = migration.default_heading;
    entity.velocity = migration.velocity;
    entity.target_velocity = migration.target_velocity;
    entity.next_update_time = migration.next_update_time;
    entity.next_fireball_index =
        static_cast<std::uint32_t>(std::max(0, migration.next_fireball_index));
    entity.target = session.local_gid();
    entity.travel_target = migration.target;
    entity.first_time_for_default_state = false;
    entity.chances_to_not_attack = 0;
    entity.loiters_left = 0;
    set_current_animation(entity, soar); // miginfo.Animation, which DoMigration never sets
    entity.flap_debt = 0.0F;
    // The constructor's LocalRotation is still the identity here.
    entity.forward = {0.0F, 0.0F, -1.0F};
    entity.mode = dragon_brain_next_attack_type(entity, session);
    enter_state(entity, entity.mode, session);
}

DragonMigrationState dragon_brain_migration(const DragonBrainState &entity) noexcept
{
    DragonMigrationState state;
    state.yaw = entity.yaw;
    state.target_yaw = entity.target_yaw;
    state.roll = entity.roll;
    state.target_roll = entity.target_roll;
    state.pitch = entity.pitch;
    state.target_pitch = entity.target_pitch;
    state.next_dragon_time = entity.dragon_time + 0.4F;
    state.next_update_time = entity.next_update_time + 0.4F;
    state.position = entity.position;
    state.velocity = entity.velocity;
    state.target_velocity = entity.target_velocity;
    state.default_heading = entity.default_heading;
    state.next_fireball_index = static_cast<std::int32_t>(entity.next_fireball_index);
    state.target = entity.travel_target;
    return state;
}

void dragon_brain_update(DragonBrainState &entity, DragonBrainSession &session, float delta_seconds,
                         std::uint8_t local_gid, DragonBrainOutput &output)
{
    if (entity.migrate_dragon)
    {
        if (entity.target.has_value())
            entity.migrate_dragon_to = entity.target;
        if (entity.migrate_dragon_to.has_value() &&
            session.living_position(*entity.migrate_dragon_to).has_value())
        {
            output.migrate_to = entity.migrate_dragon_to;
            return;
        }
        entity.migrate_dragon_to.reset();
        entity.migrate_dragon = false;
    }
    const float dt = delta_seconds;
    const std::int64_t elapsed_ticks = ticks_of(dt);
    entity.dragon_time += dt;
    update_state(entity, session, dt, output);
    entity.target_altitude = session.flight_altitude(entity.position, entity.target_altitude);
    // UpdateSounds.
    entity.cry_elapsed_ticks += elapsed_ticks;
    if (entity.next_sound == 0U && entity.cry_elapsed_ticks >= entity.cry_maximum_ticks)
    {
        entity.next_sound = cry;
        entity.cry_maximum_ticks = static_cast<std::int64_t>(session.random_int(6, 14)) * 10'000'000;
        entity.cry_elapsed_ticks = 0;
    }
    const float yaw_error = wrap_angle(entity.target_yaw - entity.yaw);
    entity.target_roll =
        entity.current_animation == hover ? 0.0F : std::clamp(yaw_error, 0.0F - max_roll, max_roll);
    const float height = entity.position.y;
    if (height > entity.target_altitude - 2.0F && height < entity.target_altitude + 2.0F)
        entity.target_pitch = 0.0F;
    else
    {
        const double climb = static_cast<double>(entity.target_altitude) - static_cast<double>(height);
        const double sign = climb > 0.0 ? 1.0 : (climb < 0.0 ? -1.0 : 0.0);
        entity.target_pitch = static_cast<float>(
            sign * static_cast<double>(std::min(static_cast<float>(climb * climb / 30.0), max_pitch)));
    }
    const float previous_yaw = entity.yaw;
    entity.yaw = move_toward_target_angle(entity.yaw, entity.target_yaw, yaw_rate, dt);
    entity.velocity = dragon_move_toward_target(entity.velocity, entity.target_velocity, max_accel, dt);
    entity.roll = dragon_move_toward_target(entity.roll, entity.target_roll, roll_rate, dt);
    entity.pitch = dragon_move_toward_target(entity.pitch, entity.target_pitch, pitch_rate, dt);
    if (entity.animation_change_update >= entity.flap_change_update &&
        entity.updates_sent > entity.animation_change_update)
    {
        const float weight = entity.pitch > 0.0F ? 2.0F : 0.5F;
        const auto add = [&entity](double amount) {
            entity.flap_debt = static_cast<float>(static_cast<double>(entity.flap_debt) + amount);
        };
        add(static_cast<double>(entity.pitch) * static_cast<double>(dt) * static_cast<double>(weight));
        add(static_cast<double>(std::abs(wrap_angle(previous_yaw - entity.yaw))) * 1.5);
        if (std::abs(entity.pitch) < 0.01F)
            add(static_cast<double>(0.2F) * static_cast<double>(dt));
        if (entity.current_animation != soar)
        {
            add(-0.5 * static_cast<double>(dt));
            if (entity.flap_debt < -1.0F && entity.next_animation == flap)
            {
                entity.next_animation = soar;
                entity.flap_change_update = entity.updates_sent;
            }
        }
        else if (entity.next_animation == soar && entity.flap_debt > 1.0F)
        {
            entity.next_animation = flap;
            entity.flap_change_update = entity.updates_sent;
        }
        entity.flap_debt = std::clamp(entity.flap_debt, -1.0F, 1.0F);
    }
    entity.forward = forward_of(entity.yaw, entity.pitch, entity.roll);
    const float travel = entity.velocity * dt;
    entity.position = {entity.position.x + entity.forward.x * travel,
                       entity.position.y + entity.forward.y * travel,
                       entity.position.z + entity.forward.z * travel};
    // The model child advances its (non-looping) clip, then ClipFinished.
    const std::int64_t duration =
        clip_duration_ticks[entity.modern_content ? 1 : 0][entity.current_animation & 3U];
    // AnimationPlayer.Update advances by TimeSpan.FromSeconds(elapsed * Speed),
    // and FromSeconds rounds to the nearest MILLISECOND: a 60 Hz frame moves
    // the clip 17 ms, so every clip ends about 2 % early in frames.
    const std::int64_t clip_step =
        static_cast<std::int64_t>(static_cast<double>(elapsed_ticks) / 10'000.0 + 0.5) * 10'000;
    entity.clip_ticks = std::min(duration, entity.clip_ticks + clip_step);
    if (entity.clip_ticks >= duration)
    {
        set_current_animation(entity, entity.next_animation);
        entity.animation_change_update = entity.updates_sent;
    }
    if (entity.dragon_time > entity.next_update_time)
    {
        if (entity.next_update_time == -1.0F)
            entity.update_count = 0;
        // DragonBaseState.SendRegularUpdate over GetBaseWaypoint.
        DragonOwnerMessage message;
        message.position = entity.position;
        message.velocity = {entity.forward.x * entity.velocity, entity.forward.y * entity.velocity,
                            entity.forward.z * entity.velocity};
        message.host_time = entity.dragon_time;
        message.animation = entity.current_animation;
        message.target_roll = entity.target_roll;
        message.sound = entity.next_sound;
        entity.next_sound = 0U;
        if (entity.current_animation != attack && entity.shot_pending)
        {
            message.attack = true;
            message.reliable = true;
            message.target = entity.shoot_target;
            entity.shot_pending = false;
            message.fireball_index = static_cast<std::int32_t>(
                (static_cast<std::uint32_t>(local_gid) << 23U) | entity.next_fireball_index++);
        }
        else
        {
            message.reliable = entity.update_count < 2;
            ++entity.update_count;
        }
        ++entity.updates_sent;
        output.messages.push_back(message);
        entity.next_update_time = entity.dragon_time + 0.2F;
    }
    entity.gunshots.clear();
}

} // namespace openminerz::world
