#pragma once

#include "openminerz/world/block.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace openminerz::world
{

// Numeric ids follow the original EnemyTypeEnum exactly: 18 zombies, 8 archer
// skeletons, 24 cave skeletons (zombie/sword/axes classes interleaved), the
// Felguard demon (50) and the crash-site Alien (51).
constexpr int enemy_type_count = 52;
constexpr int enemy_felguard = 50;
constexpr int enemy_alien = 51;

enum class EnemyClass : std::uint8_t
{
    zombie,
    archer,
    skeleton,
    skeleton_sword,
    skeleton_axes,
    felguard,
    alien,
};

enum class EnemyModelKind : std::uint8_t
{
    zombie,
    skeleton_zombie,
    skeleton_archer,
    skeleton_axes,
    skeleton_sword,
    felguard,
    alien,
    count,
};

enum class EnemyHabitat : std::uint8_t
{
    aboveground,
    caves,
    hell,
    crash_site,
};

[[nodiscard]] constexpr bool enemy_uses_cave_spawn_budget(EnemyHabitat habitat) noexcept
{
    return habitat == EnemyHabitat::caves || habitat == EnemyHabitat::hell;
}

// Original DamageType flags.
enum class DamageFlags : std::uint8_t
{
    none = 0,
    blunt = 1,
    piercing = 2,
    blade = 4,
    bullet = 8,
    shotgun = 16,
};

struct EnemyStats final
{
    EnemyClass enemy_class{};
    EnemyModelKind model{};
    std::uint8_t texture_index{}; // Into the original 12 zombie/skeleton skins.
    EnemyHabitat habitat{};
    float starting_health{1.0F};
    float chance_of_bullet_strike{1.0F};
    int spawn_radius{15};
    float attack_animation_speed{1.0F};
    float die_animation_speed{1.0F};
    float hit_animation_speed{1.0F};
    float spawn_animation_speed{1.0F};
    bool has_run_fast{};
    float base_slow_speed{2.0F};
    float random_slow_speed{3.5F};
    float base_fast_speed{6.0F};
    float base_run_activation_time{1000.0F};
    float random_run_activation_time{};
    float fast_jump_speed{10.0F};
    float digging_multiplier{1.0F};
    int hardest_block_dug{2};
};

[[nodiscard]] const EnemyStats &enemy_stats(int type) noexcept;

// Per-class GetDamageTypeMultiplier.
[[nodiscard]] float enemy_damage_multiplier(int type, DamageFlags damage, bool headshot) noexcept;

struct EnemyAttack final
{
    std::string_view clip;
    std::array<float, 4> hit_times{}; // Zero entries beyond hit_count are unused.
    int hit_count{1};
    float damage{};
    float range{};
};

struct EnemyAttackSet final
{
    const EnemyAttack *attacks{};
    int attack_count{};
    std::string_view rage_clip;
    float hit_dot_multiplier{1.0F};
};

[[nodiscard]] const EnemyAttackSet &enemy_attacks(EnemyClass enemy_class) noexcept;

// Exact MathTools.CalculateInitialBallisticVector port used by archer skeletons.
// Returns no result when the target is outside the requested speed's ballistic envelope.
[[nodiscard]] std::optional<Vec3> initial_ballistic_vector(Vec3 origin, Vec3 target, float velocity,
                                                           float gravity) noexcept;

enum class FireballSweepKind : std::uint8_t
{
    none,
    player,
    terrain,
};

struct FireballSweepHit final
{
    FireballSweepKind kind{FireballSweepKind::none};
    Vec3 position{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return kind != FireballSweepKind::none;
    }
};

// FireballEntity's AABBTraceProbe: a +/-0.57 box swept from start to end.
// The original tests the local player's 0.7x1.7 AABB first, then terrain.
[[nodiscard]] FireballSweepHit fireball_sweep(const WorldRegion &region, const PlayerState &player,
                                              Vec3 start, Vec3 end) noexcept;

enum class ArrowSweepKind : std::uint8_t
{
    none,
    player,
    terrain,
};

struct ArrowSweepHit final
{
    ArrowSweepKind kind{ArrowSweepKind::none};
    Vec3 position{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return kind != ArrowSweepKind::none;
    }
};

// TracerManager's arrow path traces the complete visible tail-to-head segment.
// The target's exact 0.7x1.7 PlayerAABB is considered first, but nearer blocking
// terrain wins; an invalid/dead target is omitted from the player test.
[[nodiscard]] ArrowSweepHit arrow_sweep(const WorldRegion &region, const PlayerState &player,
                                        bool player_valid, Vec3 tail, Vec3 head) noexcept;

// BlockType.Hardness as consumed by Explosive.EnemyBreakBlocks. Keep this
// separate from player tool dig times and explosive blast resistance: enemy
// tiers compare their HardestBlockThatCanBeDug directly with this 1..5 value.
[[nodiscard]] int enemy_dig_hardness(BlockType type) noexcept;

// Tiny deterministic generator so spawn selection and loot stay testable.
struct EnemyRng final
{
    std::uint64_t state{88172645463325252ULL};

    [[nodiscard]] std::uint64_t next() noexcept
    {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    }

    [[nodiscard]] float next01() noexcept
    {
        return static_cast<float>(next() >> 40U) / 16777216.0F;
    }

    [[nodiscard]] int next_int(int minimum, int maximum_exclusive) noexcept
    {
        if (maximum_exclusive <= minimum)
            return minimum;
        return minimum +
               static_cast<int>(next() % static_cast<std::uint64_t>(maximum_exclusive - minimum));
    }
};

// System.Random as shipped with the .NET Framework build used by CastleMiner Z.
// SpawnEnemyMessage carries this constructor seed so every peer makes the same
// per-enemy animation and attack choices without position-update messages.
struct EnemyInstanceRng final
{
    EnemyInstanceRng() noexcept;
    explicit EnemyInstanceRng(std::int32_t seed) noexcept;

    void reset(std::int32_t seed) noexcept;
    [[nodiscard]] std::int32_t next_sample() noexcept;
    [[nodiscard]] float next01() noexcept;
    [[nodiscard]] int next_int(int minimum, int maximum_exclusive) noexcept;

  private:
    std::array<std::int32_t, 56> seed_array_{};
    int inext_{};
    int inextp_{21};
};

// EnemyType.FindEnemy: distance-band selection over [first, last] with the original
// three-band sine weighting; roll01 in [0, 1).
[[nodiscard]] int find_enemy(float distance_step, float distance, int first, int last,
                             float roll01) noexcept;

[[nodiscard]] int pick_aboveground_enemy(float percent_midnight, float distance,
                                         bool felguard_ready, bool felguard_spawned,
                                         EnemyRng &rng) noexcept;
[[nodiscard]] int pick_belowground_enemy(float depth, float distance, bool felguard_ready,
                                         bool felguard_spawned, EnemyRng &rng) noexcept;

// EnemyManager.CalculateMidnight surface curve (0.85 fixed underground).
[[nodiscard]] float enemy_midnight_factor(float percent_midnight, float distance,
                                          float player_y) noexcept;
// EnemyManager.GetMinEnemySpawnTime for the normal (non-hard) difficulty.
[[nodiscard]] float minimum_enemy_spawn_seconds(float distance, bool hard_branch) noexcept;

// BaseZombie.CreatePickup vanilla loot roll. Returns item_count 0 when nothing drops.
struct EnemyLoot final
{
    std::uint16_t item_id{};
    std::uint16_t item_count{};
};
[[nodiscard]] EnemyLoot enemy_loot(int type, float distance_from_origin, float enemy_y,
                                   EnemyRng &rng) noexcept;
// EnemyManager.SpawnDragonPickups baseline drops. Only the local killer rolls
// this list; CreatePickupMessage then distributes those authoritative pickups.
[[nodiscard]] std::vector<EnemyLoot> dragon_loot(Vec3 location, EnemyRng &rng);

// Original timed-dragon schedule: FIRE -> FOREST -> LIZARD(SAND) -> ICE -> SKELETON.
enum class DragonKind : std::uint8_t
{
    fire,
    forest,
    sand,
    ice,
    undead,
    count,
};

struct DragonStats final
{
    float starting_health{};
    float fireball_velocity{};
    float fireball_damage{};
    float speed{20.0F};
    float strafe_fire_seconds{2.0F};
    float hover_fire_seconds{1.0F};
    float hunting_altitude{70.0F};
    float loiter_altitude{90.0F};
    float min_attack_distance{50.0F};
    float max_attack_distance{200.0F};
};

[[nodiscard]] const DragonStats &dragon_stats(DragonKind kind) noexcept;
// Minutes until the given timed dragon spawns while the player stays in one area
// (RecalculateDragonBox: 32/48/64/80/80 with a random +-4 minute jitter applied by
// the caller).
[[nodiscard]] float dragon_schedule_minutes(DragonKind kind) noexcept;
// DragonType.BreakLookup: which blocks this dragon's fireballs can destroy. Weaker
// dragons protect progressively more block families.
[[nodiscard]] bool dragon_breaks_block(DragonKind kind, BlockType type) noexcept;
// DamageLOSProbe: protected blocks stop a dragon blast, while breakable blocks
// attenuate it by DamageTransmision for the path length inside each voxel.
[[nodiscard]] float dragon_fireball_damage_multiplier(const WorldRegion &region, DragonKind kind,
                                                      Vec3 impact, Vec3 target) noexcept;

enum class DragonState : std::uint8_t
{
    approach,
    loiter,
    strafe,
    hover,
    leave,
    dying,
    despawn,
};

struct Dragon final
{
    DragonKind kind{DragonKind::fire};
    DragonState state{DragonState::approach};
    Vec3 position{};
    float yaw{};
    // Rate-limited attitude, like DragonEntity's yaw/pitch/roll Euler flight:
    // pitch climbs toward the target altitude, roll banks into turns.
    float pitch{};
    float roll{};
    // DragonEntity eases the scalar flight velocity independently from its
    // attitude. In particular, hover attacks decelerate from 20 to 0.25 m/s
    // instead of snapping to a stop.
    float velocity{20.0F};
    float health{};
    float state_time{};
    float fire_timer{};
    float loiter_seconds{4.0F};
    int loiter_direction{1};
    int loiters{};
    int hover_shots{};
    std::string_view clip{"fly_forward"};
    float clip_time{};
};

// Rows of the XNA Quaternion.CreateFromYawPitchRoll attitude used by the
// original dragon. Local -Z is forward; exposing the basis keeps simulation,
// hit volumes, and rendering on the same handedness.
struct DragonBasis final
{
    Vec3 right{};
    Vec3 up{};
    Vec3 forward{};
};

[[nodiscard]] DragonBasis dragon_attitude_basis(float yaw, float pitch, float roll) noexcept;

struct DragonRayHit final
{
    float distance{};
    Vec3 position{};
    bool head{};
    // DragonClientEntity.IsHeadshot is not the selected-volume flag: it is
    // BodyHitVolume[1], the plane just in front of local Z=-4.2.
    bool explosive_headshot{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return distance >= 0.0F;
    }
};

// DragonClientEntity.Trace: the authored, attitude-oriented head/body hit
// volumes. Direction is expected to be normalized, as it is for weapon rays.
[[nodiscard]] std::optional<DragonRayHit> trace_dragon(const Dragon &dragon, Vec3 origin,
                                                       Vec3 direction, float max_distance) noexcept;

struct DragonEvents final
{
    std::vector<Vec3> fireball_origins;
    std::vector<Vec3> fireball_velocities;
    bool died{};
    bool left{};
};

enum class DragonReplicaEventKind : std::uint8_t
{
    cry,
    fireball,
    cancel_fireball,
};

struct DragonReplicaEvent final
{
    DragonReplicaEventKind kind{};
    Vec3 position{};
    Vec3 target{};
    std::int32_t fireball_id{};
};

// A faithful-enough port of the dragon flight brain: approach from afar, loiter in
// circles, and alternate strafe/hover fireball runs. Original MaxLoiters counts
// targetless search circuits; it is reset whenever a living visible target is
// reacquired. A sustained loss of visibility or a genuine long-distance escape
// eventually releases the encounter, but never after an arbitrary attack count.
class DragonFlight final
{
  public:
    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }
    [[nodiscard]] bool visible() const noexcept
    {
        return active_ &&
               (!network_replica_ || network_replica_ready_ || dragon_.state == DragonState::dying);
    }
    [[nodiscard]] const Dragon &dragon() const noexcept
    {
        return dragon_;
    }
    // Dragon Endurance injects escalating wave health after spawn, the same
    // way SpawnDragonMessage carries an explicit health override.
    void set_health(float health) noexcept
    {
        dragon_.health = health;
    }
    // NOENEMIES (and world resets) remove the dragon outright.
    void deactivate() noexcept
    {
        active_ = false;
        network_waypoints_.clear();
        network_events_.clear();
        network_replica_ready_ = false;
        network_waiting_to_shoot_ = false;
    }
    void spawn(DragonKind kind, Vec3 player_position, EnemyRng &rng);
    // DragonClientEntity exists on every peer while DragonEntity's flight
    // brain exists only on the designated SpawnerID. These two entry points
    // install and advance that network-only client replica without running a
    // second local flight brain.
    void install_replica(DragonKind kind, float health = -1.0F) noexcept;
    void apply_network_waypoint(Vec3 position, Vec3 velocity, float host_time, float target_roll,
                                std::uint8_t animation, std::uint8_t sound = 0U) noexcept;
    [[nodiscard]] bool apply_network_attack_waypoint(Vec3 position, Vec3 velocity, float host_time,
                                                     float target_roll, std::uint8_t animation,
                                                     std::uint8_t sound, Vec3 target,
                                                     std::int32_t fireball_id,
                                                     bool animated) noexcept;
    void reset_network_replica(Vec3 position, Vec3 velocity, float host_time, float target_roll,
                               std::uint8_t animation) noexcept;
    void promote_network_replica_to_brain() noexcept;
    void extrapolate_network_replica(float delta_seconds) noexcept;
    [[nodiscard]] std::vector<DragonReplicaEvent> take_network_replica_events() noexcept;
    void damage(float amount);
    [[nodiscard]] DragonEvents update(const PlayerState &player, float delta_seconds, EnemyRng &rng,
                                      bool target_visible = true);

  private:
    struct NetworkWaypoint final
    {
        Vec3 position{};
        Vec3 velocity{};
        float host_time{};
        float target_roll{};
        std::uint8_t animation{};
        std::uint8_t sound{};
        std::uint8_t action{};
        Vec3 action_position{};
        std::int32_t fireball_id{};
    };

    [[nodiscard]] bool add_network_waypoint(NetworkWaypoint incoming) noexcept;
    void process_network_waypoint(const NetworkWaypoint &waypoint) noexcept;
    void emit_network_fireball(Vec3 target, std::int32_t fireball_id) noexcept;
    void apply_network_replica_sample(Vec3 position, Vec3 velocity, float target_roll,
                                      std::uint8_t animation, float delta_seconds) noexcept;

    Dragon dragon_{};
    bool active_{};
    float engagement_seconds_{};
    float lost_target_seconds_{};
    std::vector<NetworkWaypoint> network_waypoints_;
    float network_interpolation_time_{};
    bool network_replica_{};
    bool network_replica_ready_{};
    bool network_waiting_to_shoot_{};
    Vec3 network_shoot_target_{};
    std::int32_t network_shoot_fireball_id_{};
    std::vector<DragonReplicaEvent> network_events_;
};

enum class EnemyState : std::uint8_t
{
    emerge,
    chase,
    attack,
    idle,
    hit,
    die,
    give_up,
    dig,
    despawn,
};

struct EnemyInitialization final
{
    float slow_speed{};
    float fast_speed{};
    float emerge_speed{};
    float run_activation_time{};
    float normal_activation_time{};
};

struct EnemyIdentity final
{
    std::uint8_t target_gid{};
    std::uint32_t enemy_id{};

    auto operator<=>(const EnemyIdentity &) const = default;
};

struct Enemy final
{
    int type{};
    EnemyState state{EnemyState::emerge};
    Vec3 position{};
    Vec3 velocity{};
    float yaw{};
    float health{};
    bool on_ground{};
    // The clip the renderer should present, with the state-machine clock.
    std::string_view clip{"walk"};
    bool clip_loops{true};
    float clip_speed{1.0F};
    float clip_time{};
    float state_time{};
    float state_duration{};
    float lifetime{};
    // InitPackage values rolled at spawn.
    float slow_speed{2.0F};
    float fast_speed{};
    float run_activation_time{1000.0F};
    float normal_activation_time{1000.0F};
    float emerge_speed{1.0F};
    bool running{};
    int attack_index{-1};
    int hit_count{};
    int miss_count{};
    int swing_count{};
    float dig_progress{};
    int dig_x{}, dig_y{}, dig_z{};
    bool dig_valid{};
    bool loot_pending{};
    std::uint32_t id{};
    std::int32_t random_seed{};
    std::uint8_t target_gid{};
    EnemyInstanceRng instance_rng{};
};

struct EnemyEvents final
{
    float player_damage{};
    // One entry per landed hit: the attacker's position, feeding the HUD's
    // directional damage indicator.
    std::vector<Vec3> player_damage_sources;
    std::vector<EnemyLoot> drops;
    std::vector<Vec3> drop_positions;
    std::vector<std::array<int, 3>> dug_blocks;
    // TNT/C4 touched by a locally-owned dig swing but not broken on that roll
    // starts its normal four-second fuse in the original game.
    std::vector<std::array<int, 3>> fuse_blocks;
    // One entry per dig swing that connected with solid blocks: the original
    // plays the loud "ZombieDig" cue on every such swing.
    std::vector<Vec3> dig_swings;
    int died{};
    int attacks_started{};
    int arrows_fired{};
    std::vector<Vec3> arrow_origins;
    std::vector<Vec3> arrow_velocities;
    std::vector<std::uint8_t> arrow_target_gids;
    std::vector<EnemyIdentity> speed_up_requests;
    std::vector<EnemyIdentity> give_up_requests;
};

class EnemyHorde final
{
  public:
    struct RayHit final
    {
        int index{-1};
        float distance{};
        Vec3 normal{};
        bool headshot{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return index >= 0;
        }
    };

    using ClipDurationLookup = std::function<float(EnemyModelKind, std::string_view)>;

    static constexpr float attack_enter_distance = 1.0F;
    static constexpr float give_up_distance = 45.0F;
    static constexpr float despawn_distance = 100.0F;

    [[nodiscard]] const std::vector<Enemy> &enemies() const noexcept
    {
        return enemies_;
    }
    [[nodiscard]] std::vector<Enemy> &enemies() noexcept
    {
        return enemies_;
    }
    [[nodiscard]] EnemyRng &rng() noexcept
    {
        return rng_;
    }

    void set_clip_durations(ClipDurationLookup lookup)
    {
        clip_durations_ = std::move(lookup);
    }

    bool spawn(int type, Vec3 position, float midnight, std::uint32_t id,
               std::uint8_t target_gid = 0U);
    [[nodiscard]] bool spawn_networked(int type, Vec3 position, EnemyInitialization initialization,
                                       std::uint32_t id, std::int32_t random_seed,
                                       std::uint8_t target_gid);
    [[nodiscard]] std::optional<std::size_t> find_enemy(EnemyIdentity identity) const noexcept;
    [[nodiscard]] bool speed_up_enemy(EnemyIdentity identity);
    [[nodiscard]] bool give_up_enemy(EnemyIdentity identity);
    [[nodiscard]] bool kill_enemy(EnemyIdentity identity, bool create_loot);
    // Finds the nearest hittable enemy without mutating it. max_distance is
    // normally the nearer of the construction reach and the first solid block.
    [[nodiscard]] RayHit trace_along_ray(Vec3 origin, Vec3 direction, float max_distance,
                                         int ignored_index = -1) const;
    // EnemyManager.Trace(..., false): body hits on several enemy classes are
    // probabilistically accepted; headshots and 100%-strike zombies always hit.
    [[nodiscard]] RayHit trace_projectile_along_ray(Vec3 origin, Vec3 direction,
                                                    float max_distance);
    // Applies weapon damage to the enemy nearest along the ray within max_distance.
    // Returns the damaged enemy index or -1. The caller resolves blocks separately.
    [[nodiscard]] int damage_along_ray(Vec3 origin, Vec3 direction, float max_distance,
                                       float damage, DamageFlags flags);
    void damage_enemy(std::size_t index, float damage, DamageFlags flags, bool headshot,
                      bool create_loot = true);
    [[nodiscard]] std::vector<std::size_t> apply_explosive_damage(Vec3 location, float damage_range,
                                                                  bool create_loot = true);

    // One fixed-step tick against the streamed region. Damage and drops surface as events.
    [[nodiscard]] EnemyEvents update(const WorldRegion &region, const PlayerState &player,
                                     float delta_seconds);
    using TargetLookup = std::function<std::optional<PlayerState>(std::uint8_t)>;
    [[nodiscard]] EnemyEvents update(const WorldRegion &region, const PlayerState &local_player,
                                     std::uint8_t local_gid, const TargetLookup &target_lookup,
                                     float delta_seconds);

  private:
    [[nodiscard]] float clip_duration(const Enemy &enemy, std::string_view clip) const;
    void begin_state(Enemy &enemy, EnemyState state, const PlayerState &player);

    std::vector<Enemy> enemies_;
    EnemyRng rng_{};
    ClipDurationLookup clip_durations_{};
};

} // namespace openminerz::world
