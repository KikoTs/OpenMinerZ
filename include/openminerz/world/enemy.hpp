#pragma once

#include "openminerz/world/block.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/terrain_window.hpp"
#include "openminerz/world/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

// EnemyTypeEnum as SpawnEnemyMessage carries it. Classic's 52 values are this
// table's ids. Modern inserts HELL_LORD at 51 and moves ALIEN to 52, so the
// same byte names a different enemy in each generation: untranslated, this
// client's Alien arrived on a stock Modern peer as the 300-health Hell Lord,
// a stock Hell Lord was simulated here as an Alien, and a stock Alien was
// refused outright. Modern's TREASURE_ZOMBIE, ANTLER_BEAST and REAPER enum
// values (53..55) have no EnemyType.Types entry and are never sent. The Hell
// Lord is a boss Felguard and has no native type yet, so it is simulated as
// the Felguard it is modelled on.
[[nodiscard]] constexpr std::optional<int> enemy_type_from_wire(std::uint8_t wire,
                                                                bool modern) noexcept
{
    if (!modern)
        return wire < enemy_type_count ? std::optional<int>{wire} : std::nullopt;
    if (wire <= enemy_felguard)
        return wire;
    if (wire == 51U)
        return enemy_felguard;
    if (wire == 52U)
        return enemy_alien;
    return std::nullopt;
}

[[nodiscard]] constexpr std::uint8_t enemy_type_to_wire(int type, bool modern) noexcept
{
    return static_cast<std::uint8_t>(modern && type == enemy_alien ? 52 : type);
}

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
// The whole of CalculateMidnight: below y -40 it is 0.85 whatever the hour;
// otherwise the midnight frenzy (EnemyManager.ZombieFestIsOn) pins it to 1.
[[nodiscard]] inline float enemy_midnight_factor(float percent_midnight, float distance,
                                                 float player_y, bool zombie_fest) noexcept
{
    if (player_y < -40.0F)
        return 0.85F;
    return zombie_fest ? 1.0F : enemy_midnight_factor(percent_midnight, distance, player_y);
}
// EnemyManager.GetMinEnemySpawnTime: 5 s falling to 1 s at 3450 blocks on the
// hard branch (Endurance, Dragon Endurance, HARD, HARDCORE), 45 s falling to
// 20 s otherwise - and still falling beyond, because MathHelper.Lerp does not
// clamp.
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
    // Valid with `died`: EnemyManager.SpawnDragonPickups is handed the dead
    // body box's position, seven units behind where the corpse came to rest.
    Vec3 loot_position{};
    // From an owner's brain: EnemyManager.RemoveDragon() was called (send
    // RemoveDragonMessage), and DoMigration picked the next owner.
    bool remove_requested{};
    std::optional<std::uint8_t> migrate_to;
};

// DragonBaseState.FindBestTarget, over every living player in the session.
struct DragonTargetCandidate final
{
    std::uint8_t gid{};
    Vec3 position{}; // feet (Player.WorldPosition)
    bool alive{true};
    // BlockTerrain.RegionIsLoaded / GetSimpleSunlightAtPoint / a clear trace
    // from the dragon's head, each evaluated at the player's position + 1.5.
    bool region_loaded{true};
    float sunlight{1.0F};
    bool line_of_sight{true};
};

struct DragonTargetChoice final
{
    std::optional<std::uint8_t> gid;
    // Nobody within MaxViewDistance or 1.25 * SpawnDistance: the original
    // sets entity.Removed and broadcasts RemoveDragonMessage.
    bool remove_dragon{};
};

// The lowest distance^2 / sunlight^2 among players inside the 300-unit view
// range, the 0.17 view cone and a clear line of sight, measured from seven
// units ahead of the dragon. Players in unloaded terrain or with no sunlight
// are skipped (but still keep the dragon from being removed).
[[nodiscard]] DragonTargetChoice dragon_find_best_target(
    Vec3 dragon_position, Vec3 forward, std::span<const DragonTargetCandidate> players) noexcept;
// SearchForNewTarget: a newly chosen target that is not the owner and stands
// more than 150 units from the owner takes the dragon over.
[[nodiscard]] bool dragon_target_requires_migration(Vec3 target_position,
                                                    Vec3 owner_position) noexcept;

// EnemyManager's timed-dragon ladder, run by EVERY peer outside Dragon
// Endurance: CheckDragonBox, RecalculateDragonBox, AskForDragon's gate and
// SetCurrentTimedDragonType. A non-host peer turns the result into
// RequestDragonMessage, the host into SpawnDragonMessage.
struct DistanceDragonEncounter final
{
    DragonKind kind{};
    int next_index{};
};

[[nodiscard]] std::optional<Vec3> cavern_dragon_approach(const WorldRegion &region,
                                                       Vec3 player, float yaw);

// Native encounter selection follows the terrain band, including after a
// delayed admission or fast travel. Endurance keeps its separate wave ladder.
[[nodiscard]] inline DragonKind dragon_for_distance(float distance) noexcept
{
    return distance >= 3600.0F ? DragonKind::undead
         : distance >= 2400.0F ? DragonKind::ice
         : distance >= 1000.0F ? DragonKind::sand
         : distance >= 300.0F ? DragonKind::forest : DragonKind::fire;
}

[[nodiscard]] inline std::optional<DistanceDragonEncounter>
pending_distance_dragon(float distance, int next_index) noexcept
{
    constexpr std::array<float, 5> thresholds{100, 500, 1000, 2600, 4000};
    int due = 0;
    while (due < 5 && distance > thresholds[static_cast<std::size_t>(due)])
        ++due;
    if (due <= next_index)
        return std::nullopt;
    return DistanceDragonEncounter{dragon_for_distance(distance), due};
}

class DragonSchedule final
{
  public:
    // One frame of CheckDragonBox. Returns the type to ask for, if any.
    [[nodiscard]] std::optional<DragonKind> check_box(Vec3 player, float delta_seconds,
                                                      bool dragon_present, EnemyRng &rng) noexcept;
    // HandleSpawnDragonMessage / HandleExistingDragonMessage, on every peer.
    void dragon_installed(DragonKind kind, bool for_biome, Vec3 player, EnemyRng &rng) noexcept;
    // RemoveDragonEntity: eight seconds before anything may be asked for again.
    void dragon_removed(Vec3 player, EnemyRng &rng) noexcept;
    // `!(now < NextDragonAllowedTime)`, AskForDragon's first condition.
    [[nodiscard]] bool allowed() const noexcept
    {
        return !initialize_ && allowed_delay_ <= 0.0F;
    }
    [[nodiscard]] int next_timed_type() const noexcept
    {
        return next_type_;
    }
    [[nodiscard]] float seconds_until_next() const noexcept
    {
        return next_spawn_seconds_;
    }

  private:
    void recalculate(Vec3 player, EnemyRng &rng) noexcept;

    bool initialize_{true};
    float center_x_{};
    float center_z_{};
    int next_type_{}; // DragonTypeEnum, 5 = COUNT
    float next_spawn_seconds_{};
    float allowed_delay_{};
};

// What a peer does with a DetonateFireballMessage. HandleDetonateFireball-
// Message itself checks nothing; these are the limits we keep on top of it.
enum class DragonDetonationVerdict : std::uint8_t
{
    apply,
    // Not applied here. A watcher still lets it pass; the host does not relay it.
    ignore,
    reject,
};

struct DragonDetonationFireball final
{
    std::uint8_t owner_gid{};
    DragonKind kind{};
    Vec3 position{};
};

[[nodiscard]] bool dragon_blast_reaches(Vec3 impact, int x, int y, int z) noexcept;

template <typename Blocks>
[[nodiscard]] DragonDetonationVerdict dragon_assess_detonation(
    std::uint8_t sender_gid, std::int32_t fireball_index, std::uint8_t dragon_type, Vec3 location,
    const Blocks &blocks, bool already_applied, const DragonDetonationFireball *local_fireball,
    std::optional<std::uint8_t> dragon_owner_gid) noexcept
{
    // DragonEntity.GetNextFireballIndex puts the firing peer's id in bits 23+.
    const auto encoded_owner =
        static_cast<std::uint8_t>((static_cast<std::uint32_t>(fireball_index) >> 23U) & 0xFFU);
    if (encoded_owner != sender_gid || dragon_type >= static_cast<std::uint8_t>(DragonKind::count) ||
        !(location.x >= -1'000'000.0F && location.x <= 1'000'000.0F) ||
        !(location.y >= -128.0F && location.y <= 128.0F) ||
        !(location.z >= -1'000'000.0F && location.z <= 1'000'000.0F))
        return DragonDetonationVerdict::reject;
    if (already_applied)
        return DragonDetonationVerdict::ignore;
    if (local_fireball == nullptr)
    {
        // A peer whose replica discarded a late attack waypoint never fired
        // this fireball, yet the blast still lands. Only the dragon's current
        // owner is taken at its word for one we never saw.
        if (!dragon_owner_gid.has_value() || *dragon_owner_gid != sender_gid)
            return DragonDetonationVerdict::ignore;
    }
    else
    {
        const float dx = location.x - local_fireball->position.x;
        const float dy = location.y - local_fireball->position.y;
        const float dz = location.z - local_fireball->position.z;
        const float maximum_error =
            dragon_stats(local_fireball->kind).fireball_velocity * 2.0F + 5.0F;
        if (local_fireball->owner_gid != sender_gid ||
            static_cast<std::uint8_t>(local_fireball->kind) != dragon_type ||
            dx * dx + dy * dy + dz * dz > maximum_error * maximum_error)
            return DragonDetonationVerdict::reject;
    }
    // The block list is the fireball owner's (built from ITS loaded terrain);
    // a receiver can only bound it to the blast.
    for (const auto &block : blocks)
        if (!dragon_blast_reaches(location, block.x, block.y, block.z))
            return DragonDetonationVerdict::reject;
    return DragonDetonationVerdict::apply;
}

enum class DragonReplicaEventKind : std::uint8_t
{
    cry,
    fireball,
    cancel_fireball,
    // DragonClientEntity.TIMEOUT_MAX_TIME: ten seconds without a waypoint
    // makes ANY peer broadcast RemoveDragonMessage.
    timeout,
};

// What the dragon's owner publishes. DragonBaseState.SendRegularUpdate sends
// exactly one UpdateDragonMessage or one DragonAttackMessage per 0.2-second
// slot, and the owner's own DragonClientEntity consumes the same message.
struct DragonOwnerMessage final
{
    bool attack{};
    // UpdateDragonMessage.SendDataOptions: the first two updates after a
    // spawn are ReliableInOrder, every later one is unreliable.
    bool reliable{};
    Vec3 position{};
    Vec3 velocity{};
    float host_time{};
    float target_roll{};
    std::uint8_t animation{};
    std::uint8_t sound{};
    Vec3 target{};
    std::int32_t fireball_index{};
};

// DragonHostMigrationInfo exactly as DragonEntity.DoMigration fills it. The
// original never assigns Animation or FlapDebt there, so both travel as zero.
struct DragonMigrationState final
{
    float next_dragon_time{};
    float roll{};
    float target_roll{};
    float pitch{};
    float target_pitch{};
    float yaw{};
    float target_yaw{};
    float velocity{};
    float target_velocity{};
    float default_heading{};
    float next_update_time{};
    std::int32_t next_fireball_index{};
    Vec3 position{};
    Vec3 target{};
};

// Can a DetonateFireballMessage at `impact` name this block at all?
// EnemyManager.DetonateFireball lists voxels whose centre is within 3 of the
// impact, then anything attached to a face of one of those, then the upper
// half of any door among them: never further than 5 from the impact. The
// list itself is the fireball owner's to decide (it is computed from the
// owner's own loaded terrain and nobody else can reproduce it).
[[nodiscard]] bool dragon_blast_reaches(Vec3 impact, int x, int y, int z) noexcept;
// FireballEntity..ctor: straight at the target at FireballVelocity.
[[nodiscard]] Vec3 dragon_fireball_velocity(DragonKind kind, Vec3 origin, Vec3 target) noexcept;
// DragonClientEntity.ShootFireball spawns at the "Bip01_Ponytail1" bone. These
// are that bone's mean positions in the replica's own frame (right, up,
// backward) per presented animation, measured by running the original
// skeleton in tools/OriginalClientInterop/DragonParityProbe.
[[nodiscard]] Vec3 dragon_muzzle_offset(std::uint8_t animation) noexcept;
// DragonAnimEnum of a presented clip: SOAR 0, FLAP 1, ATTACK 2, HOVER 3.
[[nodiscard]] std::uint8_t dragon_animation_index(std::string_view clip) noexcept;
// MathTools.MoveTowardTarget: the step is min(|error|, rate) * dt, so the
// approach is linear above `rate` and exponential below it, snapping within 0.01.
[[nodiscard]] float dragon_move_toward_target(float current, float target, float rate,
                                              float delta_seconds) noexcept;

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
struct DragonBrainState;
class DragonBrainSession;

class DragonFlight final
{
  public:
    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }
    [[nodiscard]] bool visible() const noexcept
    {
        return active_ && (!presents_replica() || network_replica_ready_ ||
                           dragon_.state == DragonState::dying);
    }
    // What this peer PRESENTS and shoots at. Online that is always the
    // DragonClientEntity replica, on the owner too: the original owner never
    // shows its DragonEntity, only the replica fed by its own messages.
    [[nodiscard]] const Dragon &dragon() const noexcept
    {
        return dragon_;
    }
    // The flight brain (DragonEntity). Equal to dragon() unless this peer is a
    // network owner, where it runs ahead of the presented replica.
    [[nodiscard]] const Dragon &brain() const noexcept
    {
        return network_owner_ ? brain_ : dragon_;
    }
    [[nodiscard]] bool network_owner() const noexcept
    {
        return network_owner_;
    }
    // DragonEntity.DragonTime and DragonClientEntity.CurrentInterpolationTime.
    [[nodiscard]] float owner_time() const noexcept;
    [[nodiscard]] float replica_time() const noexcept
    {
        return network_interpolation_time_;
    }
    // DragonEntity.NextFireballIndex (a static that outlives each dragon).
    [[nodiscard]] std::uint32_t next_fireball_index() const noexcept;
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
        network_owner_ = false;
        owner_messages_.clear();
        owner_echo_.clear();
    }
    void spawn(DragonKind kind, Vec3 player_position, EnemyRng &rng);
    // HandleSpawnDragonMessage on the spawner: keep the freshly spawned state
    // as the hidden DragonEntity and start presenting a DragonClientEntity
    // that sees nothing until its own first three waypoints have echoed back.
    void begin_network_ownership(std::uint8_t local_gid, std::uint32_t next_fireball_index) noexcept;
    // DragonEntity.InitAfterHostChange: a brain appears beside the replica
    // this peer is already presenting; the replica and its queue are untouched.
    void adopt_network_ownership(std::uint8_t local_gid, const DragonMigrationState &migration,
                                 DragonBrainSession &session);
    // EnemyManager.MigrateDragon / HandleKillDragonMessage: the brain goes,
    // the replica stays exactly as it is.
    void end_network_ownership() noexcept;
    // DragonEntity.DoMigration's DragonHostMigrationInfo (Target = TravelTarget).
    [[nodiscard]] DragonMigrationState migration_state() const noexcept;
    // One DragonEntity.Update (world/dragon_brain.hpp is the retail brain) and
    // then the owner's own DragonClientEntity.Update on what has echoed back.
    [[nodiscard]] DragonEvents update_owner(DragonBrainSession &session, float delta_seconds);
    // The retail DragonEntity fields, for the differential test and migration.
    [[nodiscard]] DragonBrainState &brain_state() noexcept;
    // TracerManager.AddTracer -> EnemyManager.RegisterGunShot: every tracer,
    // local or remote, is heard by a dragon this peer owns.
    void register_gunshot(Vec3 position);
    // EnemyManager.DragonHasBeenHit: ChancesToNotAttack = 0.
    void note_hit() noexcept;
    // Which game's dragon clips are installed. 1.9.9.8's are slightly longer,
    // and both the brain's animation changes and the death sequence wait on them.
    void set_modern_content(bool modern) noexcept
    {
        modern_content_ = modern;
    }
    [[nodiscard]] std::vector<DragonOwnerMessage> take_owner_messages() noexcept;
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
    // `player` is the brain's current target (the session player it chose).
    // `terrain` is what a dead dragon falls onto (UpdateWhileDead); without
    // it the corpse only stops at the bottom of the world.
    [[nodiscard]] DragonEvents update(const PlayerState &player, float delta_seconds, EnemyRng &rng,
                                      bool target_visible = true,
                                      const WorldRegion *terrain = nullptr);

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
    void sync_brain_view() noexcept;
    [[nodiscard]] bool move_dead_dragon(const WorldRegion *terrain, float delta_seconds) noexcept;
    [[nodiscard]] bool presents_replica() const noexcept
    {
        return network_replica_ || network_owner_;
    }

    Dragon dragon_{};
    // DragonEntity state while this peer owns the dragon online.
    Dragon brain_{};
    std::shared_ptr<DragonBrainState> retail_;
    bool modern_content_{};
    bool network_owner_{};
    std::uint8_t owner_gid_{};
    float brain_target_yaw_{};
    float brain_target_pitch_{};
    float brain_target_roll_{};
    float brain_target_velocity_{20.0F};
    float default_heading_{};
    std::vector<DragonOwnerMessage> owner_messages_;
    std::vector<DragonOwnerMessage> owner_echo_;
    float network_timeout_{};
    // DragonClientEntity death: CurrentVelocity, DeathState (0 FIRST_REACTION,
    // 1 FALLING, 2 POSTCOLLISION), the paused death_air_1 clip and DeathTimer.
    Vec3 death_velocity_{};
    std::uint8_t death_phase_{};
    float death_reaction_time_{};
    float death_clip_time_{};
    bool death_clip_playing_{};
    float death_timer_{};
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
    // BaseZombie fields the original state machine and collision keep between
    // frames. Every peer evolves them from the SpawnEnemyMessage alone.
    bool touching_wall{};        // TouchingWall: a side contact last frame (the jump trigger)
    float frustration{15.0F};    // FrustrationCount; also the corpse timer while dying
    float state_timer{};         // StateTimer (archer line-of-sight poll and idle wait)
    float current_speed{};       // CurrentSpeed: SlowSpeed until SpeedUp, then FastSpeed
    EnemyState previous_state{EnemyState::emerge};
    bool blocking{};             // IsBlocking: slows a player walking into it
    bool hittable{};             // IsHittable: bullets and melee can strike it
    // 1.9.9.8 BaseZombie.SpawnSource: the enemy came out of a spawner block
    // (and whether that block is a hell spawner). It chases and digs further.
    bool spawner_sourced{};
    bool hell_spawner{};
    // AnimationPlayer.CurrentTime and the clip's Duration in 100 ns ticks. The
    // original advances the clock in whole milliseconds per frame, so the
    // state timings below are only reproducible with the same integer clock.
    std::int64_t clip_ticks{};
    std::int64_t clip_duration_ticks{};
};

// (Authority) One swing that landed: whose, on whom, how hard (a fraction of
// health, as EnemyAttack::damage).
struct EnemyStrike final
{
    std::uint8_t target_gid{};
    std::uint32_t enemy_id{};
    float damage{};
    Vec3 source{};
};

struct EnemyEvents final
{
    float player_damage{};
    // One entry per landed hit: the attacker's position, feeding the HUD's
    // directional damage indicator.
    std::vector<Vec3> player_damage_sources;
    // (Authority) The same hits one by one, with the enemy behind each: what
    // a client reports to an authoritative host.
    std::vector<EnemyStrike> strikes;
    // (Authority) Swings this peer's copy of an enemy landed on a REMOTE
    // target, which only that target's own peer applies (BaseTryAttack). Only
    // filled while EnemyHorde::set_observe_remote_strikes is on, on a host
    // that holds them against the target's own reports; nothing else changes.
    std::vector<EnemyStrike> remote_strikes;
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
    // There is no despawn distance: the original removes an enemy only when it
    // leaves the terrain its own peer has loaded (see StockTerrainWindow).

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
    // EnemyManager.ZombieFestIsOn: while it is set, every local-target zombie
    // in the chase state asks for its speed-up at once.
    void set_zombie_fest(bool active) noexcept
    {
        zombie_fest_ = active;
    }
    // Simulate as 1.9.9.8 rather than 1.6.3.8: who announces a give-up, and
    // how far a target may get before the chase or the dig is abandoned.
    void set_modern_rules(bool modern) noexcept
    {
        modern_rules_ = modern;
    }
    // (Authority) Also report the swings that land on remote targets
    // (EnemyEvents::remote_strikes). Observation only: no enemy state, random
    // draw or local damage changes.
    void set_observe_remote_strikes(bool observe) noexcept
    {
        observe_remote_strikes_ = observe;
    }
    // The terrain window this client actually holds (the renderer scrolls it
    // with the local player by the original's rule). Given one, update() tests
    // RegionIsLoaded against it instead of tracking a private copy, so the
    // enemy cull and the terrain can never disagree about where the edge is.
    void set_terrain_window(const StockTerrainWindow &window) noexcept
    {
        terrain_window_ = window;
        terrain_window_external_ = true;
    }
    // MathTools.Rnd as Explosive.EnemyBreakBlocks draws from it (one coin per
    // diggable block per swing). The original seeds it from the clock; a fixed
    // seed makes a dig reproducible for the parity replay.
    void seed_dig_random(std::int32_t seed) noexcept
    {
        dig_random_.reset(seed);
    }
    // EnemyManager.AttentuateVelocity: how much the IsBlocking enemies that
    // target this player slow it this frame. Each one within two blocks (and
    // 1.5 vertically) halves the speed, and scales it further toward zero the
    // more directly the player walks at it. `forward` is the player's
    // normalized horizontal velocity.
    [[nodiscard]] float attenuate_velocity(std::uint8_t target_gid, Vec3 forward,
                                           Vec3 player_position) const noexcept;

    bool spawn(int type, Vec3 position, float midnight, std::uint32_t id,
               std::uint8_t target_gid = 0U);
    [[nodiscard]] bool spawn_networked(int type, Vec3 position, EnemyInitialization initialization,
                                       std::uint32_t id, std::int32_t random_seed,
                                       std::uint8_t target_gid);
    // EnemyManager.HandleSpawnEnemyMessage's remote-admission quirk, shared by
    // both stock generations: below 45 total enemies every announced enemy is
    // accepted, between 45 and 49 only every fourth one is, and from 50 none
    // are. A refusal is not a protocol error — the original still relays the
    // message and simply never creates the entity, so peers legitimately
    // disagree about a crowded session's tail. Deviating from the rule is what
    // makes a stock peer and OpenMinerZ disagree about an uncrowded one.
    [[nodiscard]] bool accept_remote_spawn() noexcept;
    // How many live enemies target this GID. EnemyManager keeps the three
    // _localXxxEnemyCount tallies over its own target only, so another
    // player's horde must never throttle local spawning.
    [[nodiscard]] std::size_t count_for_target(std::uint8_t target_gid) const noexcept;
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
    std::vector<Enemy> enemies_;
    bool zombie_fest_{};
    bool modern_rules_{};
    bool observe_remote_strikes_{}; // (Authority)
    EnemyInstanceRng dig_random_{};
    // Where the shipped game's terrain window sits around the local player.
    StockTerrainWindow terrain_window_{};
    bool terrain_window_external_{};
    // StockTerrainWindow::resets() as of the last update, once one has run.
    std::optional<unsigned> terrain_resets_seen_{};
    EnemyRng rng_{};
    ClipDurationLookup clip_durations_{};
    int remote_spawn_counter_{};
};

} // namespace openminerz::world
