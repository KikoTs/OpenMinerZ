#pragma once

// (Authority) The host decides, in sessions only OpenMinerZ players can join.
//
// Retail CastleMiner Z (1.6.3.8 and 1.9.9.8, read from the shipped
// executables) has no combat authority at all. Every peer simulates every
// enemy, tracer and explosion; a peer only ever lowers its OWN player's health
// (InGameHUD.ApplyDamage): an enemy's swing is tested only where its target is
// local (BaseTryAttack.Update), a bullet only by the victim's tracer, an
// explosion or a fireball by each peer against its own player, a MeleePlayer
// message by the victim it was sent to; falls and lava are local; no message
// carries health; death is the PlayerUpdate Dead bit; respawn is local. The
// host relays what it is sent and stores the inventory it is given.
//
// A session that can hold a stock player keeps exactly that, byte for byte:
// a stock client owns its health and nothing may override it. A session whose
// manifest requires_extension() - server/core mods, or the host's "OpenMinerZ
// players only" (ModManifest::openminerz_only) - admits only OpenMinerZ
// clients, so there the host takes the decisions instead:
//
//   * Health, damage, death and respawn belong to the host. A client never
//     sets its own health; it reports what only it can see first (an enemy's
//     swing, an archer's arrow, a fall) and the host checks the report against
//     the game's tables before applying it. What the host can see itself it
//     decides alone: another player's melee, gunfire (traced on the host
//     against where the victim was over the shooter's lag), explosions and
//     fireballs (every player in range, behind the same line-of-sight probe),
//     lava, mods. A client that leaves its own hurts unreported is caught by
//     the host's copy of the same enemies and falls and charged the
//     difference. Clients keep their own instant feedback (hit cue,
//     indicator, camera kick).
//   * Movement must be possible: speed, unannounced teleports, hovering or
//     passing through solid blocks outside Creative are corrected (the
//     player is put back where the host last accepted it).
//   * Block edits must be in reach, at a possible rate, with a tool that can
//     dig the block, and outside spawn protection.
//   * Weapons fire at the gun's own rate; a launch starts where its shooter is.
//   * A pickup has one winner; a pickup a player creates must come from
//     something it broke, killed, opened or carried.
//
// A refused action has no effect on anybody else: the app does not relay it
// and corrects the sender. Each refusal of something an honest client cannot
// produce adds to the player's score, which decays; with the host's Anti-Cheat
// on STRICT a score past the limit is a kick with the reason in the host's log
// and chat. Thresholds sit above what 200 ms of lag with jitter produces.
//
// This file has no app, Luau or transport dependency: the app (and
// tests/test_mod_auth.cpp) feed it what the session's stock messages say, and
// AuthorityServerLink / AuthorityClient carry its own messages over the mod
// channel (engine namespace, names "auth.*").

#include "openminerz/game_mode.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/modding/api.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/host_session.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/explosive.hpp"
#include "openminerz/world/player.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::modding
{
class ModNetServer;
class ModNetClient;
} // namespace openminerz::modding

namespace openminerz::net
{

// Whether a session's host is authoritative: exactly when only OpenMinerZ
// players can be in it.
[[nodiscard]] bool session_authoritative(const ModManifest &manifest) noexcept;

// What hurt a player. Values travel in "auth.*" messages; never renumber.
enum class HarmCause : std::uint8_t
{
    unknown = 0,
    enemy = 1,        // an enemy's swing (BaseTryAttack)
    arrow = 2,        // an archer skeleton's arrow
    fall = 3,         // Player.ResolveCollsion
    lava = 4,         // InGameHUD's lava timer
    player_melee = 5, // MeleePlayerMessage
    player_gun = 6,   // another player's tracer or blaster bolt
    explosion = 7,    // TNT, C4, rockets, HE grenades
    fireball = 8,     // a dragon's fire or ice ball
    mod = 9,          // a server mod: players.damage, status effects, creatures
};
inline constexpr std::uint8_t harm_cause_count = 10U;
[[nodiscard]] const char *harm_cause_name(HarmCause cause) noexcept;

// How a player is about to move itself somewhere far, announced before it
// does so (auth.tp). Values travel on the wire.
enum class TeleportKind : std::uint8_t
{
    surface = 0,  // menu Teleport > Surface: the same column
    start = 1,    // menu Teleport > Start, a level restart
    player = 2,   // menu Teleport > Player, Endurance respawn beside one
    locator = 3,  // the GPS / teleport item
    station = 4,  // a 1.9.9.8 teleport station or spawn block
    respawn = 5,  // after a respawn the host granted
};

// The session's rules, fixed when hosting starts.
struct AuthorityRules final
{
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    GameMode mode{GameMode::survival};
    GameDifficulty difficulty{GameDifficulty::easy};
    bool infinite_resources{};
    PlayerVersusPlayer pvp{PlayerVersusPlayer::off};
    // The host's Anti-Cheat row: STRICT kicks a player whose score passes the
    // limit; OFF still refuses and corrects but never kicks.
    bool strict{true};
    // Blocks around `spawn_point` (horizontal) that only the host may change.
    int spawn_protection_radius{};
    world::Vec3 spawn_point{};
};

// Lag tolerance and limits. The defaults keep an honest client at 200 ms with
// jitter free of corrections; tests/test_mod_auth.cpp holds them to that.
struct AuthorityTuning final
{
    // Health messages to one client at most this often (damage and death go
    // out at once).
    double health_interval_seconds{0.1};
    // How long the host keeps where each player was, for lag compensation.
    double position_history_seconds{1.0};
    // A shot is tested against where the victim was over this much of the
    // past (the shooter's own lag plus the host's view of the victim).
    double shot_compensation_seconds{0.35};
    // Movement, measured from an anchor 1-2 s old (so reordered and bunched
    // updates do not add up): top horizontal speed (1.9.9.8 sprints at 8,
    // Creative flight at 48), top rise, how late a sample may have arrived,
    // and a fixed allowance on top.
    float walk_speed_limit{10.0F};
    float fly_speed_limit{60.0F};
    float rise_speed_limit{12.0F};
    float movement_jitter_seconds{0.3F};
    float movement_slack_meters{4.0F};
    // A move longer than this in one update is a teleport.
    float teleport_distance{24.0F};
    // A teleport notice opens this long a window for its jump.
    double teleport_window_seconds{4.0};
    // A jump that came before its notice waits this long for it.
    double teleport_grace_seconds{1.5};
    // Airborne this long without falling at least `hover_drop_meters` from
    // the highest point is flying.
    double hover_seconds{2.5};
    float hover_drop_meters{2.0F};
    // Feet and head inside player-blocking blocks this long is no-clip.
    double inside_solid_seconds{1.5};
    // After a correction, updates from where the client was are expected for
    // this long before the correction counts as ignored.
    double correction_grace_seconds{2.0};
    // Reach: the game's 5-unit probe plus how far the host's picture may lag.
    float reach{5.0F};
    float reach_slack{3.5F};
    // Melee: the attacker's probe hits within 2.5 units.
    float melee_reach{2.5F};
    float melee_slack{3.0F};
    // An enemy's swing is believed within this distance of the host's copy.
    float enemy_reach_slack{8.0F};
    // A victim's report and the host's own sighting of the same enemy's swing
    // meet within this window; an unreported sighting older than it counts.
    double hit_match_seconds{1.5};
    // Unreported enemy damage over `deficit_window_seconds` above this many
    // health points, with fewer than half of the host's sightings reported, is
    // a player ignoring its damage.
    float deficit_points{60.0F};
    double deficit_window_seconds{12.0};
    // The host's fall estimate must exceed the reported damage by this much
    // before it charges the rest.
    float fall_slack_points{12.0F};
    // Rates: rounds, swings and edits may arrive this many at once, or this
    // many seconds' worth (a lag spike), whichever is more.
    float burst_actions{4.0F};
    float rate_burst_seconds{1.0F};
    // Score: each refusal adds its weight; the score falls by this much per
    // second; STRICT kicks at `kick_score`.
    float score_decay_per_second{0.25F};
    float kick_score{24.0F};
};

// One enemy as the host's own simulation has it (EnemyHorde).
struct AuthorityEnemy final
{
    int type{};
    world::Vec3 position{};
    std::uint8_t target_gid{};
    bool alive{true};
};

// What the host knows of the world. The app answers from its terrain and
// enemy horde; a test from a fake. Main thread only.
class AuthorityWorld
{
  public:
    virtual ~AuthorityWorld() = default;
    // The block at a cell, or nullopt where the host cannot tell.
    [[nodiscard]] virtual std::optional<world::BlockType> block(int x, int y, int z) const = 0;
    // An enemy by its stock identity (target gid, enemy id), or nullopt.
    [[nodiscard]] virtual std::optional<AuthorityEnemy> enemy(std::uint8_t target_gid,
                                                              std::uint32_t enemy_id) const = 0;
    // DamageLOSProbe from an impact to a target: 1 in the open, less behind
    // blocks, 0 behind what this blast cannot break (dragon kind 0 for
    // explosives, the dragon's kind for its fireballs).
    [[nodiscard]] virtual float blast_line_of_sight(world::Vec3 impact, world::Vec3 target,
                                                    world::DragonKind kind) const = 0;
    // Whether nothing that stops a player lies on the straight segment.
    [[nodiscard]] virtual bool clear_line(world::Vec3 from, world::Vec3 to) const = 0;
    // (mod-liquid) Whether a cell holds a mod liquid (openminerz/world/
    // liquid.hpp): it stops nobody, a player swims in it without support,
    // and building into it is building into an empty cell. False by default.
    [[nodiscard]] virtual bool liquid(int x, int y, int z) const
    {
        static_cast<void>(x);
        static_cast<void>(y);
        static_cast<void>(z);
        return false;
    }
};

// A client's own report of damage only it could see first (auth.hurt).
struct HarmReport final
{
    HarmCause cause{HarmCause::unknown};
    float amount{};                       // health points, 0..100 scale
    std::optional<std::uint32_t> enemy;   // the enemy (target is the reporter)
    std::optional<world::Vec3> source;    // where it came from
};

enum class AuthorityVerdict : std::uint8_t
{
    accept,
    // No effect on anybody: not applied, not relayed. The app corrects the
    // sender where the action changed its own picture (a block, a pickup).
    refuse,
};

enum class AuthorityEventKind : std::uint8_t
{
    health,     // tell the player its health (value) and whether it lives
    died,       // the host decided the player died (cause, source, other = attacker)
    respawned,  // the host granted a respawn (value = health)
    correction, // put the player back at `position` (reason)
    violation,  // the player tried something refused (reason, value = score)
    kick,       // STRICT: the score passed the limit (reason)
};

struct AuthorityEvent final
{
    AuthorityEventKind kind{AuthorityEventKind::health};
    std::uint8_t gid{};
    HarmCause cause{HarmCause::unknown};
    float value{};
    bool alive{true};
    world::Vec3 position{};
    std::optional<std::uint8_t> other;
    std::string reason;
};

// What a player's record says, for the app and tests.
struct AuthorityPlayerView final
{
    std::uint8_t gid{};
    bool local{};
    float health{100.0F};
    bool alive{true};
    float score{};
    std::uint32_t violations{};
    std::uint32_t corrections{};
    bool kicked{};
    std::optional<world::Vec3> position;
    world::Vec3 accepted{};
};

class AuthorityHost final
{
  public:
    explicit AuthorityHost(AuthorityRules rules, AuthorityTuning tuning = {});
    ~AuthorityHost();
    AuthorityHost(const AuthorityHost &) = delete;
    AuthorityHost &operator=(const AuthorityHost &) = delete;

    [[nodiscard]] const AuthorityRules &rules() const noexcept;
    [[nodiscard]] const AuthorityTuning &tuning() const noexcept;
    void set_world(const AuthorityWorld *world) noexcept;
    // Whether a remote victim is a Steam friend of the host (PVP "not
    // friends" asks the host's friendship, as retail's victim does).
    void set_friend_of_host(std::function<bool(std::uint8_t gid)> query);
    // A server mod's say on every player's damage (player.damaged): returns
    // the amount to apply, 0 to cancel. Unset: unchanged.
    void set_damage_filter(
        std::function<float(std::uint8_t gid, float amount, HarmCause cause, world::Vec3 source)>
            filter);
    // Whether a recipe makes the item. Crafting happens on the client and is
    // not announced, so the host believes any count of a craftable item and
    // only checks the rest against what it gave. Unset: nothing is craftable.
    void set_craftable(std::function<bool(InventoryItemId item)> query);
    // How much faster than the game a player may move (a server mod's speed
    // effect); at least 1. Unset: 1.
    void set_speed_scale(std::function<float(std::uint8_t gid)> query);
    // (Items) A server mod's melee weapon in the attacker's hand when its
    // stock hand holds `item` (the mod item it announced; GameplayService::
    // held_weapon): a MeleePlayerMessage is then judged by the weapon's reach
    // (half of it reaches players, as retail's probe does) and rate, and
    // hurts by its player damage. Unset or nullopt: the retail item's.
    struct MeleeWeapon final
    {
        float player_damage{};
        float reach{5.0F};
        float cooldown_seconds{0.5F};
    };
    void set_melee_weapon(
        std::function<std::optional<MeleeWeapon>(std::uint8_t attacker, InventoryItemId item)> query);

    // --- Players ------------------------------------------------------------

    // `local` is the host's own player: the app keeps simulating it and
    // mirrors its health here (set_local_state); nothing below corrects it.
    void add_player(std::uint8_t gid, double now, bool local = false);
    void remove_player(std::uint8_t gid);
    [[nodiscard]] bool has_player(std::uint8_t gid) const noexcept;
    [[nodiscard]] std::optional<AuthorityPlayerView> player(std::uint8_t gid) const;
    [[nodiscard]] std::vector<std::uint8_t> players() const;
    void set_local_state(std::uint8_t gid, float health, bool alive, world::Vec3 position,
                         double now);
    // Whether the host lets this player act (alive, not kicked). A dead
    // player's edits, swings, shots and pickups are refused.
    [[nodiscard]] bool may_act(std::uint8_t gid) const noexcept;

    // --- Health -------------------------------------------------------------

    // Damage the host decided (its own detection, a mod). Returns what it
    // applied. `attacker` is the player behind it, if any.
    float harm(std::uint8_t gid, float amount, HarmCause cause, world::Vec3 source, double now,
               std::optional<std::uint8_t> attacker = std::nullopt);
    void heal(std::uint8_t gid, float amount, double now);
    void set_health(std::uint8_t gid, float health, double now);
    // The victim's own report (auth.hurt): checked against the enemy tables,
    // the host's copy of the enemy, and the fall the host saw.
    AuthorityVerdict report_harm(std::uint8_t gid, const HarmReport &report, double now);
    // The client asks to come back (auth.respawn).
    AuthorityVerdict request_respawn(std::uint8_t gid, double now);
    // The host's own copy of an enemy (or arrow) struck this remote player:
    // not applied, but held against the player's own reports.
    void observe_enemy_hit(std::uint8_t gid, std::optional<std::uint32_t> enemy, float amount,
                           world::Vec3 source, double now, HarmCause cause = HarmCause::enemy);

    // --- What the session's stock messages say -------------------------------

    // One gameplay message the session already accepted from `sender`
    // (`recipient` 0 for the host or everyone), by name and payload: the
    // whole stock-message side of the authority in one call, so the app and
    // tests/test_mod_auth.cpp take the same path. Refuse: do not apply, do
    // not relay, correct the sender. Messages it has no rule for: accept.
    AuthorityVerdict observe_stock_message(std::uint8_t sender, std::uint8_t recipient,
                                           std::string_view name,
                                           std::span<const std::uint8_t> payload, double now);

    // PlayerUpdateMessage: movement, the dead bit. Refused = do not relay; a
    // correction event says where the player goes back to.
    AuthorityVerdict observe_player_update(std::uint8_t gid, const PlayerUpdatePayload &update,
                                           double now);
    // The client's notice before it moves itself far (auth.tp).
    AuthorityVerdict observe_teleport_notice(std::uint8_t gid, TeleportKind kind,
                                             world::Vec3 target, double now);
    // The host itself moves a player (a server mod's players.teleport): its
    // next jump, near `target` when given, is accepted.
    void allow_teleport(std::uint8_t gid, std::optional<world::Vec3> target, double now);
    // RestartLevelMessage: everyone returns to the start.
    void observe_level_restart(double now);
    // ChangeCarriedItemMessage.
    void observe_carried_item(std::uint8_t gid, InventoryItemId item, double now);
    // MeleePlayerMessage from `attacker` to `victim`. Accepted: the host has
    // applied the damage to a remote victim; for the host's own player
    // (`victim` local) the app applies it as before.
    AuthorityVerdict observe_melee(std::uint8_t attacker, std::uint8_t victim,
                                   const MeleePlayerPayload &melee, double now);
    // GunshotMessage / ShotgunShotMessage: rate, ammunition and aim. Accepted
    // shots are traced by the app, which calls tracer_hits_player and harm.
    AuthorityVerdict observe_gunshot(std::uint8_t shooter, InventoryItemId item,
                                     std::span<const Vector3> directions, double now);
    // GrenadeMessage / FireRocketMessage: the projectile starts at the shooter.
    AuthorityVerdict observe_launch(std::uint8_t shooter, world::Vec3 position,
                                    world::Vec3 direction, double now);
    // A detonation the host accepted (anyone's, its own included): every
    // player in range takes its damage, behind the line-of-sight probe.
    // Retail has no PVP rule for explosives. Returns the players it hurt.
    std::vector<std::uint8_t> apply_explosion(world::Vec3 location,
                                              world::ExplosiveDamageBands bands,
                                              std::optional<std::uint8_t> by, double now);
    // DetonateFireballMessage: 0.4 of health inside 5 units, as retail.
    std::vector<std::uint8_t> apply_fireball(world::Vec3 location, world::DragonKind kind,
                                             double now);
    // A tracer segment from `shooter` (nullopt: an enemy's) against where
    // `victim` was over the compensation window. Returns the distance along
    // the segment of the first touch, or nullopt.
    [[nodiscard]] std::optional<float> tracer_hits_player(std::uint8_t victim, world::Vec3 tail,
                                                          world::Vec3 head, double now) const;
    // Whether PVP lets `attacker` hurt `victim` (retail's victim-side rule).
    [[nodiscard]] bool pvp_allows(std::uint8_t attacker, std::uint8_t victim) const;

    // AlterBlockMessage from `gid`: reach, rate, tool, held item, spawn
    // protection. `previous` is the block the host has there.
    AuthorityVerdict observe_block_edit(std::uint8_t gid, IntVector3 cell,
                                        world::BlockType previous, world::BlockType block,
                                        double now);
    // RemoveBlocksMessage from `gid`: an explosion it set off, or an enemy
    // chasing it digging.
    AuthorityVerdict observe_block_removal(std::uint8_t gid, std::span<const IntVector3> cells,
                                           double now);
    // DetonateExplosive/Grenade/Rocket from `gid`, before apply_explosion.
    AuthorityVerdict observe_detonation(std::uint8_t gid, world::Vec3 location, double now);
    // KillEnemyMessage / KillDragonMessage crediting `gid`: its loot may
    // follow as pickups. No position: the killer's own.
    void observe_enemy_kill(std::uint8_t gid, std::optional<world::Vec3> position, double now);
    // DestroyCrateMessage from `gid`: the crate's stacks may follow as pickups.
    void observe_crate_opened(std::uint8_t gid, world::Vec3 position,
                              std::span<const InventoryStack> contents, double now);
    // ItemCrateMessage the host applied: a crate slot went from `before` to
    // `after` by this player's hand (the difference moved to or from it).
    void observe_crate_slot(std::uint8_t gid, const InventoryStack &before,
                            const InventoryStack &after, double now);
    // Whether the player can have put what `after` adds to the slot.
    [[nodiscard]] bool may_put_in_crate(std::uint8_t gid, const InventoryStack &before,
                                        const InventoryStack &after) const;
    // Items the host gave the player outside a pickup (a server mod's give).
    void grant_items(std::uint8_t gid, const InventoryStack &stack, double now);
    // InventoryStoreOnServerMessage: what the client says it carries. The
    // first one is believed (and, in Hardcore, the first after a respawn);
    // after that an item may not grow past what the host handed over plus
    // what the cookbook makes of what the player holds. Refused: the host
    // keeps the inventory it stored before.
    AuthorityVerdict observe_inventory(std::uint8_t gid, std::span<const InventoryStack> stacks,
                                       double now);

    // CreatePickupMessage from `gid`. Accepted: the pickup is registered for
    // request_pickup.
    AuthorityVerdict observe_pickup_created(std::uint8_t gid, std::uint32_t pickup_id,
                                            InventoryStack stack, world::Vec3 position,
                                            double now);
    // A pickup the host itself (or a stock rule) made, e.g. its own drops.
    void register_pickup(std::uint8_t spawner, std::uint32_t pickup_id, InventoryStack stack,
                         world::Vec3 position);
    // RequestPickupMessage: one winner per pickup.
    AuthorityVerdict request_pickup(std::uint8_t gid, std::uint8_t spawner,
                                    std::uint32_t pickup_id, double now);
    // The host handed the player a pickup (ConsumePickupMessage). One the
    // authority registered was counted at request_pickup; any other (the
    // host's own drops) is counted here.
    void pickup_given(std::uint8_t gid, std::uint8_t spawner, std::uint32_t pickup_id,
                      const InventoryStack &stack, double now);

    // --- Time and output ------------------------------------------------------

    // Once per host step: regeneration, lava, falls, unreported damage,
    // respawn and correction timers, score decay, health messages.
    void update(double now);
    [[nodiscard]] std::vector<AuthorityEvent> take_events();

    class Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

// --- Over the mod channel ------------------------------------------------------

// The RPC names (engine namespace, mod id "").
inline constexpr std::string_view auth_hurt_message = "auth.hurt";       // client -> host
inline constexpr std::string_view auth_respawn_message = "auth.respawn"; // client -> host
inline constexpr std::string_view auth_teleport_message = "auth.tp";     // client -> host
inline constexpr std::string_view auth_health_message = "auth.hp";       // host -> client
inline constexpr std::string_view auth_died_message = "auth.died";       // host -> client
inline constexpr std::string_view auth_live_message = "auth.live";       // host -> client
inline constexpr std::string_view auth_fix_message = "auth.fix";         // host -> client
inline constexpr std::string_view auth_notice_message = "auth.note";     // host -> client

// The host's end: client messages into an AuthorityHost, its events out to
// the players they concern. Both objects outlive the link.
class AuthorityServerLink final
{
  public:
    using GidOf = std::function<std::optional<std::uint8_t>(const modding::Handle &player)>;
    using HandleOf = std::function<std::optional<modding::Handle>(std::uint8_t gid)>;

    AuthorityServerLink(AuthorityHost &authority, modding::ModNetServer &server, GidOf gid_of,
                        HandleOf handle_of, std::function<double()> clock);
    ~AuthorityServerLink();
    AuthorityServerLink(const AuthorityServerLink &) = delete;
    AuthorityServerLink &operator=(const AuthorityServerLink &) = delete;

    // Sends every pending event to its player (the host's own player is the
    // app's) and returns all of them for the app: deaths for the mod hooks,
    // kicks, log lines.
    std::vector<AuthorityEvent> flush();
    // A line every client shows (a kick's reason).
    void notice_all(std::string_view text);

  private:
    AuthorityHost &authority_;
    modding::ModNetServer &server_;
    GidOf gid_of_;
    HandleOf handle_of_;
    std::function<double()> clock_;
};

enum class AuthorityClientEventKind : std::uint8_t
{
    health,    // health (and whether alive) as the host has it
    died,      // the host says this player died: run the death
    respawned, // the host granted the respawn asked for
    correction,// go back to `position`
    notice,    // a line to show
};

struct AuthorityClientEvent final
{
    AuthorityClientEventKind kind{AuthorityClientEventKind::health};
    float health{100.0F};
    bool alive{true};
    HarmCause cause{HarmCause::unknown};
    std::optional<world::Vec3> source;
    world::Vec3 position{};
    std::string text;
};

// A joined client's end (and, offline or hosting, the in-process client half
// of a test). It never sets health on its own: it reports, and applies what
// the host answers.
class AuthorityClient final
{
  public:
    explicit AuthorityClient(modding::ModNetClient &client);
    ~AuthorityClient();
    AuthorityClient(const AuthorityClient &) = delete;
    AuthorityClient &operator=(const AuthorityClient &) = delete;

    void report_harm(const HarmReport &report);
    void request_respawn();
    void announce_teleport(TeleportKind kind, world::Vec3 target);

    [[nodiscard]] std::vector<AuthorityClientEvent> take_events();
    // The latest health and life the host sent (100 and alive before any).
    [[nodiscard]] float health() const noexcept;
    [[nodiscard]] bool alive() const noexcept;
    [[nodiscard]] bool respawn_pending() const noexcept;

  private:
    modding::ModNetClient &client_;
    std::vector<AuthorityClientEvent> events_;
    float health_{100.0F};
    bool alive_{true};
    bool respawn_pending_{};
};

} // namespace openminerz::net
