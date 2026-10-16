#pragma once

// The gameplay modding service: what a mod uses to build a game on top of
// OpenMinerZ - survival rules (stats such as hunger and thirst, status
// effects), world edits and structures (dungeons), world-generation hooks, new
// blocks, items, recipes and loot, creatures and NPCs, quests and dialogue,
// and explicit difficulty scaling.
//
// Read docs/modding/architecture.md first. The rules this service keeps:
//
//  * Server-authoritative. Everything that changes the game runs on the
//    server VM (capability `gameplay`): offline worlds and listen hosts run it,
//    a joined client does not. Client VMs get `query` functions and see the
//    results through replication (GameplayClient below).
//  * CastleMiner Z is peer-owned: each peer simulates its own player (health,
//    movement, inventory) and the enemies that target it. The server changes
//    a REMOTE player through directives its owning OpenMinerZ client applies
//    (GameplaySync), and learns about that player's damage, death and
//    inventory from the owner's reports. So `player.damaged` is cancellable
//    and modifiable for the players this process simulates (single-player and
//    the host's own player) and an after-the-fact notification for the
//    others; difficulty scaling is replicated to every peer and applied by
//    each one to its own simulation. (Authority) Except where the host is
//    authoritative - every session with a server mod, which only OpenMinerZ
//    players can join (net/session_authority.hpp): there the host owns
//    every player's health, `player.damaged` is cancellable and modifiable
//    for remote players too, and their clients' own reports are ignored
//    (set_authority, authority_player_damaging, GameplayBackend::authority_*).
//  * Retail parity. With no server VM running every hook below returns at
//    once (one inline flag test, no allocation, no virtual call), so packets,
//    saves and simulation are exactly what they are without mods. Mod content
//    never puts an unknown id into a stock message or the retail save: a mod
//    block or item names a retail `fallback`; the world holds the fallback
//    and the mod identity lives in this service's sidecar
//    (<world>/mods/gameplay.omzg) and in the registry snapshot the session
//    sends to clients. Removing the mod leaves a world retail can open.
//  * Mod structures and world-generation output are ordinary authoritative
//    block edits (GameplayBackend::set_block -> the host's chunk snapshot,
//    AlterBlockMessage and the world save), never "generated" state: a stock
//    or joining peer receives them as the modified-chunk data the existing
//    chunk sync already sends, so peers never need to re-run mod generation
//    and cannot disagree about it.
//
// Nothing here includes app internals. The app implements GameplayBackend
// (src/app_mod_gameplay.inc); the Network area implements GameplaySync over
// the mod channel; UI and Graphics read GameplayClient.

#include "openminerz/modding/api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
namespace gameplay
{

// ---------------------------------------------------------------------------
// Engine-facing values
// ---------------------------------------------------------------------------

struct BlockPos final
{
    int x{};
    int y{};
    int z{};
    auto operator<=>(const BlockPos &) const = default;
};

// What a player looks like to the service. `gid` is the session gamer id; the
// script-visible identity is a Handle{HandleKind::player, serial} that is never
// reused within a session (gids are).
struct PlayerSnapshot final
{
    std::uint8_t gid{};
    std::string name;   // gamer tag: the stable key for persisted player data
    bool local{};       // simulated by this process
    bool alive{true};
    Vec3 position;      // feet
    float yaw{};
    float health{100.0F};
};

// A stack as the service sees it: retail item id (InventoryItemId), count and
// the mod item's session id (0 for a plain retail stack).
struct StackView final
{
    std::uint16_t item{};
    std::uint16_t count{};
    std::uint32_t mod_item{};
};

enum class DamageCause : std::uint8_t
{
    unknown,
    enemy,
    fall,
    lava,
    drowning,
    player,
    explosion,
    creature, // a mod entity
    effect,   // a status effect
    script    // players.damage
};

[[nodiscard]] const char *damage_cause_name(DamageCause cause) noexcept;

// (Integration) Where the player this process simulates looks: for
// players.me() on either VM and the aimed block of item.used.
struct LocalView final
{
    Vec3 eye;
    Vec3 look; // unit direction
    float yaw{};
    float pitch{};
    // The block the game's own selection targets (what digging would act
    // on) and the empty cell before the face it looks at, when there is one.
    std::optional<BlockPos> target;
    std::optional<BlockPos> target_adjacent;
    // (mod-liquid) The first liquid cell the look passes through within
    // reach (the selection passes through liquids), and whether the feet /
    // the eyes are in one.
    std::optional<BlockPos> liquid;
    bool in_liquid{};
    bool underwater{};
};

// The app's narrow interface onto the game (src/app_mod_gameplay.inc). Main
// thread only. "Local" means the player this process simulates; the service
// never asks the backend to change another peer's player directly.
class GameplayBackend
{
  public:
    virtual ~GameplayBackend() = default;

    // (Integration) nullopt outside a world (and by default).
    [[nodiscard]] virtual std::optional<LocalView> local_view() const
    {
        return std::nullopt;
    }

    // --- session -----------------------------------------------------------
    [[nodiscard]] virtual std::int32_t world_seed() const = 0;
    // 1.9.9.8 rules (Modern item set) rather than 1.6.3.8.
    [[nodiscard]] virtual bool modern_rules() const = 0;
    // The gid of the player this process simulates; nullopt before a world.
    [[nodiscard]] virtual std::optional<std::uint8_t> local_player() const = 0;
    // (fix) Whether that player already stands in the world as it will be
    // played (false while the world still loads: player.spawned then waits
    // for the spawn, local_player_spawned, instead of announcing a position
    // the loading world has not settled).
    [[nodiscard]] virtual bool local_player_placed() const
    {
        return true;
    }
    // Every player in the session, local one first.
    virtual void players(std::vector<PlayerSnapshot> &out) const = 0;

    // --- the local player --------------------------------------------------
    virtual void local_set_health(float health) = 0;
    // Through the game's own damage path (indicator, death, drops).
    virtual void local_damage(float amount, Vec3 source, DamageCause cause) = 0;
    virtual void local_teleport(Vec3 feet) = 0;
    // Returns how many could NOT be given (they are dropped at the player by
    // the caller's choice: `drop_rest`).
    virtual std::uint16_t local_give(StackView stack, bool drop_rest) = 0;
    // Removes up to `count`; returns how many were removed.
    virtual std::uint32_t local_take(std::uint16_t item, std::uint32_t mod_item,
                                     std::uint32_t count) = 0;
    [[nodiscard]] virtual std::uint32_t local_count(std::uint16_t item,
                                                    std::uint32_t mod_item) const = 0;
    virtual void local_clear_inventory() = 0;
    [[nodiscard]] virtual std::optional<StackView> local_held() const = 0;
    // Every occupied slot, as (linear slot index, stack); tray slots follow
    // the backpack. Used to persist and report mod item identities.
    virtual void local_slots(std::vector<std::pair<std::uint16_t, StackView>> &out) const = 0;
    // Tags the stack in a slot as a mod item when it still holds `item`.
    virtual bool local_tag_slot(std::uint16_t slot, std::uint16_t item, std::uint32_t mod_item) = 0;
    // Multiplies the local player's movement (status effects); 1 = unchanged.
    virtual void local_set_speed(float multiplier) = 0;

    // --- world -------------------------------------------------------------
    // The cell as the authority knows it: loaded terrain, remembered edits or
    // the generated baseline. nullopt outside the world.
    [[nodiscard]] virtual std::optional<std::uint8_t> block(BlockPos cell) const = 0;
    // One authoritative edit, exactly as a player's own edit travels: the
    // host's chunk snapshot, AlterBlockMessage to every peer, the remembered
    // edit the world save writes, and the remesh. `block` is a retail id.
    virtual bool set_block(BlockPos cell, std::uint8_t block) = 0;
    // world::BiomeKind as an integer; (mod-world) where a mod world
    // generator runs, a mod biome's world::biome_id.
    [[nodiscard]] virtual int biome(int x, int z) const = 0;
    // (mod-world) The mod block ("mod:name") a world generator put in a cell
    // while the cell still holds it - or, `before_change`, what the cell held
    // right before its last change (a dig the hooks hear of afterwards). ""
    // for none, and by default: only the app knows generated terrain.
    [[nodiscard]] virtual std::string generated_block(BlockPos cell, bool before_change) const
    {
        static_cast<void>(cell);
        static_cast<void>(before_change);
        return {};
    }
    // Sunlight 0..1 (-1 inside an opaque block) and torchlight 0..1 where the
    // terrain is loaded; nullopt elsewhere.
    [[nodiscard]] virtual std::optional<std::array<float, 2>> light(BlockPos cell) const = 0;

    // --- containers and pickups ---------------------------------------------
    virtual bool set_crate_slot(BlockPos crate, std::size_t index,
                                std::optional<StackView> stack) = 0;
    virtual bool spawn_pickup(Vec3 position, StackView stack) = 0;
    // (Integration) This peer's copy of a crate slot gets a mod item identity
    // (`mod_item`, 0 clears it) when it still holds `item`, the mod item's
    // retail fallback. Nothing is sent: the identity travels on the mod
    // channel, the stock ItemCrateMessage carries the fallback as always.
    virtual bool tag_crate_slot(BlockPos crate, std::size_t index, std::uint16_t item,
                                std::uint32_t mod_item)
    {
        static_cast<void>(crate);
        static_cast<void>(index);
        static_cast<void>(item);
        static_cast<void>(mod_item);
        return false;
    }

    // --- time --------------------------------------------------------------
    // GameScreen.Day: the day counter with the time of day as its fraction.
    [[nodiscard]] virtual double day() const = 0;
    virtual void set_time_of_day(float time_of_day) = 0;

    // --- retail enemies (the stock path, as if EnemyManager spawned it) ----
    virtual bool spawn_enemy(int type, Vec3 position) = 0;

    // --- chat --------------------------------------------------------------
    // A system line: to one player (nullopt: everyone). Offline and for the
    // local player it shows locally; the app decides how a remote line goes.
    virtual void chat(std::optional<std::uint8_t> gid, std::string_view text) = 0;

    // --- persistence -------------------------------------------------------
    // <world>/mods for the open world; empty when nothing is persisted (a
    // joined client, a preview).
    [[nodiscard]] virtual std::filesystem::path sidecar_directory() const = 0;

    // --- (Authority) another player, when the host is authoritative ---------
    // In a session only OpenMinerZ players can join, the host owns every
    // player's health and checks every move (net/session_authority.hpp).
    // These hand a server mod's change to another player to that authority
    // instead of a directive to the player's own client. False (the
    // default, and every session that may hold a stock player): the service
    // sends its directive as before.
    virtual bool authority_damage(std::uint8_t gid, float amount, DamageCause cause, Vec3 source)
    {
        static_cast<void>(gid);
        static_cast<void>(amount);
        static_cast<void>(cause);
        static_cast<void>(source);
        return false;
    }
    virtual bool authority_heal(std::uint8_t gid, float amount)
    {
        static_cast<void>(gid);
        static_cast<void>(amount);
        return false;
    }
    virtual bool authority_set_health(std::uint8_t gid, float health)
    {
        static_cast<void>(gid);
        static_cast<void>(health);
        return false;
    }
    // The directive still moves the player or gives it the items; the
    // authority is told so it accepts the jump and knows the items.
    virtual void authority_teleport(std::uint8_t gid, Vec3 feet)
    {
        static_cast<void>(gid);
        static_cast<void>(feet);
    }
    virtual void authority_give(std::uint8_t gid, std::uint16_t item, std::uint32_t count)
    {
        static_cast<void>(gid);
        static_cast<void>(item);
        static_cast<void>(count);
    }

    // --- (mod-ai) creature AI and retail enemy rules ---------------------------
    // The biome key of a column for rules and water ("" = the retail name of
    // biome()); a mod biome's key when the world generator has mod biomes.
    [[nodiscard]] virtual std::string biome_key(int x, int z) const
    {
        static_cast<void>(x);
        static_cast<void>(z);
        return {};
    }
    // Whether a creature's explosion may dig this cell out: the session's
    // rules let monsters break blocks (retail's dragon fireball rule) and the
    // cell is outside spawn protection. False by default.
    [[nodiscard]] virtual bool creature_block_damage_allowed(BlockPos cell) const
    {
        static_cast<void>(cell);
        return false;
    }
    // (fix) Whether a mod projectile that opted in to breaking blocks
    // (breaks_blocks) may dig this cell out: anywhere outside the host's
    // spawn protection, whatever the difficulty. False by default.
    [[nodiscard]] virtual bool projectile_block_breaking_allowed(BlockPos cell) const
    {
        static_cast<void>(cell);
        return false;
    }
    // The host's own test of a projectile's segment against a player (where
    // the player was over its lag): the distance along the segment of the
    // first touch, nullopt for a miss. Unanswered (the outer nullopt): the
    // service tests the player's last known box itself.
    [[nodiscard]] virtual std::optional<std::optional<float>> projectile_hits_player(
        std::uint8_t gid, Vec3 tail, Vec3 head) const
    {
        static_cast<void>(gid);
        static_cast<void>(tail);
        static_cast<void>(head);
        return std::nullopt;
    }
    // Whether the host lets this player act (alive, not kicked): a dead
    // player's claims to have struck a creature are refused.
    [[nodiscard]] virtual bool player_may_act(std::uint8_t gid) const
    {
        static_cast<void>(gid);
        return true;
    }
};

// ---------------------------------------------------------------------------
// (mod-ai) Retail enemy rules, projectiles and boss bars
// ---------------------------------------------------------------------------

// The retail enemy families a server mod can switch off for the whole session
// (enemies.rules). Every peer spawns retail enemies for its own player, so
// every peer applies the rules the host replicates; they exist only in
// sessions with a server mod, which no stock player can join.
enum class RetailEnemyClass : std::uint8_t
{
    zombie,
    skeleton,
    archer,
    felguard,
    alien,
    dragon,
    spawner, // enemies from 1.9.9.8 spawner blocks (and the block's activation)
    hell,    // anything spawning below y -40, and hell spawners
    count
};

[[nodiscard]] const char *retail_enemy_class_name(RetailEnemyClass type) noexcept;
[[nodiscard]] std::optional<RetailEnemyClass> retail_enemy_class_from_name(std::string_view name) noexcept;
// The family of an EnemyTypeEnum value (zombie, skeleton, archer, felguard, alien).
[[nodiscard]] RetailEnemyClass retail_enemy_class_of(int type) noexcept;

// One region rule: the columns it covers (a box and/or a biome; neither =
// everywhere) and what it decides there. Later regions win.
struct RetailEnemyRegion final
{
    bool has_box{};
    double min_x{};
    double min_z{};
    double max_x{};
    double max_z{};
    bool has_heights{};
    double min_y{};
    double max_y{};
    std::string biome;          // a biome key ("desert", "mymod:ashlands") or ""
    std::optional<bool> enabled; // every family on/off here
    std::uint16_t classes_set{}; // families this region decides (bit per class)
    std::uint16_t classes_on{};
    std::optional<float> tier_distance; // retail distance used for tiers here
};

struct RetailEnemyRules final
{
    bool active{};   // false: retail, exactly
    bool enabled{true};
    std::uint16_t classes{0xFFFFU};
    // The distance retail picks tiers by (zombie strength, skeleton kinds,
    // dragons, spawn pace): distance * scale + offset, then at most `maximum`.
    float distance_scale{1.0F};
    float distance_offset{};
    float distance_maximum{-1.0F}; // < 0: none
    std::vector<RetailEnemyRegion> regions;
    // Retail enemies already about that these rules switch off leave as
    // retail's own give-up does (no drops) on every peer; false: the rules
    // only hold for what spawns from now on.
    bool clear_existing{};

    [[nodiscard]] bool allows(RetailEnemyClass type, Vec3 position, std::string_view biome) const;
    [[nodiscard]] float tier_distance(float distance, Vec3 position, std::string_view biome) const;
    // Whether any region names a biome (callers look the biome up only then).
    [[nodiscard]] bool needs_biome() const noexcept;
};

// A mod projectile as a client draws it: launched with a velocity and
// gravity and flown here between the host's corrections.
struct ClientProjectile final
{
    std::uint64_t id{};
    std::string kind;
    std::string model;
    std::string texture;
    std::string fx;
    float scale{1.0F};
    float radius{0.15F};
    Vec3 position;
    Vec3 velocity;
    float gravity{};
    float yaw{};   // 0 faces -Z, like entities
    float pitch{}; // radians, up positive
    float age{};
    std::string impact_fx;
    float explosion{}; // radius of the blast where it lands (0: none)
};

// Where a projectile ended, for the client's impact effect and sound.
struct ClientImpact final
{
    Vec3 position;
    std::string kind;
    std::string hit;  // "block", "player", "entity", "expired"
    std::string fx;   // the projectile's impact_fx
    float explosion{};
};

// Script handles of projectiles (projectiles.launch, projectile.hit).
inline constexpr HandleKind projectile_handle_kind = static_cast<HandleKind>(120U);

// A boss's health bar. Every client is told about every boss; the UI shows
// the bars of bosses within `range` of the local player.
struct ClientBoss final
{
    std::uint64_t entity{};
    std::string mod;
    std::string title;
    std::string color; // "#rrggbb"
    float fraction{1.0F};
    std::string phase;
    std::string music; // a music/ asset of `mod`, "" for none
    Vec3 position;
    float range{64.0F};
};

// ---------------------------------------------------------------------------
// Replication transport
// ---------------------------------------------------------------------------

// Every message is a Value map whose "t" field names it (the list is at the
// top of src/modding/gameplay/service.cpp). The Network area
// carries them opaque over the mod channel, only to peers that negotiated the
// same server mods; payload sizes are bounded by the service.
class GameplaySync
{
  public:
    virtual ~GameplaySync() = default;
    // Server -> the client that simulates player `gid`.
    virtual void to_player(std::uint8_t gid, const Value &message, bool reliable) = 0;
    // Server -> every client in the session, the local one included.
    virtual void to_all(const Value &message, bool reliable) = 0;
    // Client -> server. The receiving host fills in the sender from the
    // transport, never from the message.
    virtual void to_server(const Value &message) = 0;
};

class GameplayService;

// Joins the server and client halves inside one process (offline worlds and a
// listen host's own player), and forwards everything else to `remote` (the
// Network area's mod channel; null drops it, which is right offline).
class LoopbackGameplaySync final : public GameplaySync
{
  public:
    explicit LoopbackGameplaySync(GameplayService &service) : service_{service}
    {
    }
    void set_remote(GameplaySync *remote) noexcept
    {
        remote_ = remote;
    }
    void to_player(std::uint8_t gid, const Value &message, bool reliable) override;
    void to_all(const Value &message, bool reliable) override;
    void to_server(const Value &message) override;

  private:
    GameplayService &service_;
    GameplaySync *remote_{};
};

// ---------------------------------------------------------------------------
// What clients see (read by UI and Graphics; filled by replication)
// ---------------------------------------------------------------------------

// One stat of the local player (hunger, thirst, ...), with the presentation
// hints its definition gave. UI draws `hud` stats as bars.
struct ClientStat final
{
    std::string name;
    double value{};
    double minimum{};
    double maximum{100.0};
    bool hud{};
    std::string label;
    std::string color; // "#rrggbb" or ""
    std::string icon;  // asset ref, resolved by Graphics/UI
};

struct ClientEffect final
{
    std::string kind;
    float remaining{}; // seconds; < 0 = until removed
    float strength{};
    std::string icon;
};

// A mod creature or NPC as a client draws it. Graphics resolves `model`
// (and `texture`) and plays `animation`; positions arrive at 10 Hz and are
// interpolated here (`position` is the presented one).
struct ClientEntity final
{
    std::uint64_t id{};
    std::string kind;
    std::string model;
    std::string texture;
    std::string animation{"idle"};
    std::string name; // shown as a nametag when not empty
    Vec3 position;
    float yaw{};
    float health_fraction{1.0F};
    float width{0.6F};
    float height{1.8F};
    bool interactable{};
    // Interpolation state (owned by GameplayClient).
    Vec3 from;
    Vec3 to;
    float from_yaw{};
    float to_yaw{};
    float blend{1.0F};
};

// A dialogue the server opened for the local player. The UI agent's dialogue
// screen draws it and answers with GameplayService::client_choose().
struct ClientDialogue final
{
    std::uint64_t session{};
    std::string title;   // dialogue/NPC name
    std::string speaker;
    std::string text;
    std::string portrait; // asset ref or ""
    std::vector<std::string> choices; // empty: one "Continue"/close
    std::uint64_t entity{};          // the NPC entity id, 0 for none
};

struct ClientQuestObjective final
{
    std::string text;
    std::uint32_t progress{};
    std::uint32_t required{1U};
    bool done{};
};

// One entry of the local player's quest log.
struct ClientQuest final
{
    std::string id;
    std::string title;
    std::string description;
    std::string stage;      // current stage text
    std::vector<ClientQuestObjective> objectives;
    bool completed{};
    bool tracked{true};
};

// Difficulty scaling a server mod set; replicated to every peer, which applies
// it to the enemies and damage it simulates itself. `active` false = retail.
struct DifficultyScaling final
{
    bool active{};
    float enemy_health{1.0F};
    float enemy_damage{1.0F};
    float spawn_rate{1.0F};
    // Per retail enemy type (EnemyTypeEnum) overrides of the two scales.
    std::map<int, std::array<float, 2>> per_type;

    [[nodiscard]] float health_for(int type) const noexcept;
    [[nodiscard]] float damage_for(int type) const noexcept;
};

// A mod block/item/creature as the session registry describes it (what a
// client needs to present it). Graphics resolves the refs.
struct ClientBlockType final
{
    std::uint16_t id{};
    std::string key;  // "mod:name"
    std::string name;
    std::uint8_t fallback{};
    std::string texture;               // all faces, or the side faces
    std::array<std::string, 3> faces;  // top, side, bottom ("" = texture)
    std::string model;
    std::uint8_t light{};
    bool liquid{}; // (mod-liquid) openminerz/world/liquid.hpp
};

// (Items) How a body holds a mod item (items.define `hold`): which retail arm
// clips play while it is in hand, and where in the hand it sits
// (docs/modding/authoring.md, "Held items"). Scripts and the
// registry message name a style; nothing stores the number.
enum class HoldStyle : std::uint8_t
{
    fallback, // as its fallback item is held (the default)
    item,     // GenericIdle / GenericUse: a small thing in the palm
    swing,    // PickIdle / PickUse: a melee weapon (the default with `weapon`)
    tool,     // PickIdle / PickUse: a pick, axe or spade
    block,    // BlockIdle / BlockUse: held out in front
    fist,     // FistIdle / FistUse: in a fist
    eat,      // BlockIdle / BlockUse, and on use it bobs at the mouth
    drink,    // BlockIdle / BlockUse, and on use it tips up at the mouth
};
inline constexpr std::array<std::string_view, 8> hold_style_names{
    "fallback", "item", "swing", "tool", "block", "fist", "eat", "drink"};
[[nodiscard]] std::optional<HoldStyle> hold_style_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view hold_style_name(HoldStyle style) noexcept;

// (Items) A mod item's melee stats (items.define `weapon`). They replace the
// fallback's wherever the item strikes - retail enemies, mod creatures and,
// as PVP allows, players - and the host checks every swing against its own
// copy of them, never against what a client claims.
struct ItemWeapon final
{
    float damage{};        // enemies and creatures, retail enemy units (a knife: 0.5)
    float player_damage{}; // players, health points (every retail melee item: 21)
    float reach{5.0F};     // blocks from the eye along the look (retail: 5)
    float cooldown{0.5F};  // seconds between swings
};

struct ClientItemType final
{
    std::uint32_t id{};
    std::string key;
    std::string name;
    std::string description;
    std::uint16_t fallback{};
    std::string icon;
    std::string model;
    std::uint16_t stack_limit{};
    bool usable{};
    std::string places; // the mod block it places ("" for none)
    // (Items) How it is held, its size in the hand and on the ground, the
    // time between uses, and its melee stats when it is a weapon.
    HoldStyle hold{HoldStyle::fallback};
    float scale{1.0F};
    float cooldown{0.25F};
    std::optional<ItemWeapon> weapon;
};

struct ClientRecipe final
{
    std::uint32_t id{};
    std::string category;
    std::string result; // retail item name or mod item key
    std::uint16_t count{1U};
    std::vector<std::pair<std::string, std::uint16_t>> ingredients;
};

// What the request-driven UI pieces get told when something changes.
enum class ClientChange : std::uint8_t
{
    registry,
    stats,
    effects,
    entities,
    dialogue,
    quests,
    difficulty,
    overlay
};

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

struct GameplayLimits final
{
    // World cells a script may queue in one call (fill, replace, structure).
    std::size_t edit_cells_per_call{262'144U};
    // Authoritative block edits applied per server tick; the rest wait.
    std::size_t edits_per_tick{512U};
    // Region queries (find/count) visit at most this many cells.
    std::size_t query_cells{131'072U};
    std::size_t max_entities{1024U}; // (mod-ai) was 256: overhauls run hundreds of creatures
    // Chunks offered to world-generation handlers per tick.
    std::size_t worldgen_chunks_per_tick{2U};
    // Chunks within this many of a player get the world-generation pass;
    // retail peers hold +-12 chunks, so this decorates ahead of them.
    int worldgen_radius_chunks{14};
    // Client -> server messages per player per second (burst = twice this).
    float client_messages_per_second{30.0F};
    std::size_t max_overlay_cells{262'144U};
};

// ---------------------------------------------------------------------------
// The service
// ---------------------------------------------------------------------------

enum class ChatVerdict : std::uint8_t
{
    pass,     // send/show it as usual
    cancel,   // a handler refused it: drop it
    command   // it was a mod command and has been handled: do not send it
};

class GameplayService final
{
  public:
    GameplayService(ScriptHost &host, GameplayBackend &backend, GameplayLimits limits = {});
    ~GameplayService();
    GameplayService(const GameplayService &) = delete;
    GameplayService &operator=(const GameplayService &) = delete;

    // Registers every gameplay function, event and constant.
    void register_api(ApiRegistry &registry);

    // Where replication goes. Defaults to an internal LoopbackGameplaySync, so
    // offline play needs nothing; the Network area installs its channel with
    // loopback().set_remote(...).
    void set_sync(GameplaySync *sync) noexcept;
    [[nodiscard]] LoopbackGameplaySync &loopback() noexcept;

    // --- lifecycle ---------------------------------------------------------
    // The server VM started for the open world (offline or hosting): loads
    // the sidecar and starts simulating. Call after the server VM's scripts
    // ran (their definitions are in), before world.loaded.
    void start_server();
    // A server-side script used a gameplay function before start_server():
    // the server half runs from now on, and the sidecar and players are
    // brought in at the first tick, once every script has loaded. (The
    // registered functions call this themselves, so a session whose wiring
    // only ticks the service still works.)
    void start_server_deferred();
    // The client VM started (always, when a server mod runs in this session).
    void start_client();
    // The mod channel to the host became ready (a joined client): asks the
    // server for everything this client needs.
    void client_channel_ready();
    // Persists (server) and drops that half's state and every callback.
    void stop_server();
    void stop_client();
    // Writes the sidecar now (the app calls it when it saves the world).
    void save();
    // Restores the local player's mod item identities from the sidecar (after
    // the world's inventory is in place).
    void restore_local_inventory_tags();

    [[nodiscard]] bool server_running() const noexcept
    {
        return server_running_;
    }
    [[nodiscard]] bool client_running() const noexcept
    {
        return client_running_;
    }
    [[nodiscard]] bool running() const noexcept
    {
        return server_running_ || client_running_;
    }

    // Fixed simulation step (60 Hz). Server: stats, effects, entities, spawn
    // rules, queued edits, world generation, quests, time-of-day events and
    // replication. Client: entity interpolation and reports.
    void tick(float seconds);

    // --- hooks for the player this process simulates ------------------------
    // All of them are safe and do nothing when !running(); call sites still
    // test running() first so a game without mods pays one branch.
    //
    // The damage to apply: `amount` unchanged when nothing intervenes, 0 when
    // a handler cancelled. Before the game's own damage and death handling.
    [[nodiscard]] float local_player_damaging(float amount, DamageCause cause, Vec3 source);
    void local_player_died(Vec3 position, DamageCause cause);
    void local_player_spawned(Vec3 position);
    void local_player_respawned(Vec3 position);
    // False: refuse (the block stays). At the moment a dig completes.
    [[nodiscard]] bool local_block_breaking(BlockPos cell, std::uint8_t block);
    void local_block_broken(BlockPos cell, std::uint8_t block);
    // `drops` false: the mod block's own drops replaced the retail pickup.
    [[nodiscard]] bool local_block_drops(BlockPos cell, std::uint8_t block);
    // Cells this process's explosives removed (already gone; block.broken
    // for each, the previous blocks as the game knew them).
    void local_blocks_blasted(std::span<const BlockPos> cells, std::span<const std::uint8_t> previous);
    [[nodiscard]] bool local_block_placing(BlockPos cell, std::uint8_t block, StackView held);
    void local_block_placed(BlockPos cell, std::uint8_t block, StackView held);
    // The use button pressed with `held`. False: the game must not run the
    // item's own action (cancelled, or a mod item's use ran instead).
    [[nodiscard]] bool local_item_using(StackView held);
    void local_item_crafted(std::uint16_t item, std::uint32_t count, std::uint32_t mod_item);
    [[nodiscard]] ChatVerdict local_chat(std::string_view text);
    [[nodiscard]] bool local_enemy_spawning(int type, Vec3 position);
    void local_enemy_spawned(int type, std::uint32_t enemy_id, std::uint8_t target_gid,
                             Vec3 position);
    void local_enemy_killed(int type, std::uint32_t enemy_id, std::uint8_t target_gid,
                            Vec3 position);
    // A swing or shot from the local player against mod entities. Returns the
    // distance of the entity it struck (the caller keeps the nearer of this
    // and its own enemy/block hit), or nullopt.
    // (Items) `mod_item`: the held mod weapon the swing is made with (0 for
    // a retail item); the server judges the claim by that weapon's stats.
    [[nodiscard]] std::optional<float> local_attack(Vec3 origin, Vec3 direction, float reach,
                                                    float damage, std::uint16_t weapon,
                                                    std::uint32_t mod_item = 0U);
    // The interact button: the nearest interactable entity on the ray, if
    // any, is asked to interact. True when one took the press.
    [[nodiscard]] bool local_interact(Vec3 origin, Vec3 direction, float reach);

    // --- hooks for the host (other players' stock messages) ------------------
    // The one identity path (Integration): the app admits a player here when
    // its mod channel opens (the host's own player when the session starts)
    // and hands the returned handle to ModNetServer::add_peer, so net.*,
    // replicated.*, ui.*(player) and players.* name it the same way. The
    // record stays until remote_player_left, whatever the game's own player
    // list says meanwhile. No event yet: remote_player_joined (after
    // add_peer) tells the scripts, so a player.joined handler can already
    // reach the player over the channel.
    Handle admit_player(std::uint8_t gid, std::string_view name, bool local);
    void remote_player_joined(std::uint8_t gid, std::string_view name);
    void remote_player_left(std::uint8_t gid);
    void remote_player_dead(std::uint8_t gid, bool dead, Vec3 position);
    // An AlterBlockMessage from `gid`. False: refuse it (the host corrects the
    // sender and does not relay).
    [[nodiscard]] bool remote_block_edit(std::uint8_t gid, BlockPos cell, std::uint8_t previous,
                                         std::uint8_t block);
    // A RemoveBlocksMessage (dig/explosion) from `gid`. False: refuse it.
    [[nodiscard]] bool remote_blocks_removed(std::uint8_t gid, std::span<const BlockPos> cells);
    [[nodiscard]] ChatVerdict remote_chat(std::uint8_t gid, std::string_view text);
    void remote_enemy_spawned(std::uint8_t gid, int type, std::uint32_t enemy_id, Vec3 position);
    void remote_enemy_killed(std::uint8_t killer_gid, int type, std::uint32_t enemy_id,
                             std::uint8_t target_gid, Vec3 position);

    // --- (Authority) the host decides every player's health ---------------------
    // While on (a session only OpenMinerZ players can join), the host's
    // authority owns remote players' health: player.damaged is asked about
    // their damage first and may cancel or change it like the local
    // player's, and their clients' own health, hurt, death and respawn
    // reports are not believed. Off: everything as before.
    void set_authority(bool on) noexcept;
    [[nodiscard]] bool authority() const noexcept;
    // The amount to apply to another player after player.damaged handlers
    // ({remote = true}); 0 when one cancelled.
    [[nodiscard]] float authority_player_damaging(std::uint8_t gid, float amount, DamageCause cause,
                                                  Vec3 source);
    // What the authority decided: players.* reads it, and a change of life
    // fires player.died / player.respawned.
    void authority_player_health(std::uint8_t gid, float health, bool alive, Vec3 position,
                                 DamageCause cause);
    // The movement multiplier a player's "speed" effects give it (1 without
    // one): the authority's speed limit grows with it.
    [[nodiscard]] float player_speed_multiplier(std::uint8_t gid) const;

    // --- replication inbound --------------------------------------------------
    // A message from the client simulating `gid` (validated and rate limited
    // here before any script runs).
    void receive_from_client(std::uint8_t gid, const Value &message);
    // A message from the server.
    void receive_from_server(const Value &message);

    // The full registry a joining client needs before gameplay (what the
    // Network area's registry sync carries).
    [[nodiscard]] Value registry_snapshot() const;
    // Everything the server sends a client that just became ready: registry,
    // overlay, entities, difficulty, its stats/effects/quests, inventory tags.
    void send_full_state(std::uint8_t gid);

    // --- the client view --------------------------------------------------------
    [[nodiscard]] const std::vector<ClientStat> &client_stats() const noexcept;
    [[nodiscard]] const std::vector<ClientEffect> &client_effects() const noexcept;
    [[nodiscard]] const std::vector<ClientEntity> &client_entities() const noexcept;
    [[nodiscard]] const std::optional<ClientDialogue> &client_dialogue() const noexcept;
    [[nodiscard]] const std::vector<ClientQuest> &client_quests() const noexcept;
    [[nodiscard]] const DifficultyScaling &client_difficulty() const noexcept;
    [[nodiscard]] float client_speed_multiplier() const noexcept;
    [[nodiscard]] const std::vector<ClientBlockType> &client_block_types() const noexcept;
    // (mod-world) The retail fallback of a mod block as one side's own
    // definitions know it (blocks.define ran on that side's VM); nullopt for
    // an unknown key. What a world generator builds its palette from.
    [[nodiscard]] std::optional<std::uint8_t> block_fallback(Side side, std::string_view key) const;
    // (mod-world) A mod block as the client VM's own definitions describe it
    // (id 0: not the host's registry id). A world generator's blocks are drawn
    // from it while the world loads, before the host's registry arrives.
    [[nodiscard]] std::optional<ClientBlockType> client_defined_block(std::string_view key) const;
    [[nodiscard]] const std::vector<ClientItemType> &client_item_types() const noexcept;
    [[nodiscard]] const std::vector<ClientRecipe> &client_recipes() const noexcept;
    // The mod block a cell shows, when the cell still holds its fallback.
    [[nodiscard]] const ClientBlockType *client_block_at(BlockPos cell,
                                                         std::uint8_t current) const noexcept;
    [[nodiscard]] const ClientItemType *client_item(std::uint32_t mod_item) const noexcept;
    // A retail recipe the server switched off (recipes.remove), by its index
    // in crafting_recipes(profile).
    [[nodiscard]] bool client_recipe_disabled(std::size_t retail_index) const noexcept;
    // Notified after replication changed something (UI/Graphics refresh).
    void set_client_listener(std::function<void(ClientChange)> listener);
    // (Visuals) Bumped with every notification of `change`, so a renderer can
    // follow the client view by polling, without the single listener above.
    [[nodiscard]] std::uint64_t client_revision(ClientChange change) const noexcept;
    // (Visuals) Every cell the client knows a mod block in (registry id, 1-based
    // into client_block_types()); client_block_at() checks one cell.
    [[nodiscard]] const std::map<BlockPos, std::uint16_t> &client_overlay() const noexcept;
    // (Visuals) Whether a removed entity died rather than despawned (the last
    // 64 removals are remembered), so its death can still be played.
    [[nodiscard]] bool client_entity_died(std::uint64_t id) const noexcept;

    // Requests from the local UI, sent to the server.
    void client_choose(std::uint64_t dialogue_session, std::size_t choice);
    void client_craft(std::uint32_t recipe_id);

    // --- (Integration) mod item identity in crates and pickups ---------------
    // Stock messages carry a mod item's retail fallback; in a session with
    // server mods every peer is OpenMinerZ, so the identity rides the mod
    // channel beside them (src/modding/gameplay/containers.cpp). The server
    // keeps the identities (crates in the sidecar), every client a copy.
    //
    // The local player changed a crate slot to `stack` (`stack.mod_item` 0 for
    // a retail stack): call it for every slot whose old or new stack was a mod
    // item, whether or not a stock ItemCrateMessage went out.
    void local_crate_slot(BlockPos crate, std::size_t index, StackView stack);
    // The mod item a crate slot holding retail `item` is (0: a retail stack).
    [[nodiscard]] std::uint32_t crate_slot_identity(BlockPos crate, std::size_t index,
                                                    std::uint16_t item) const noexcept;
    // A crate is gone (destroyed here or by a peer): its identities go too.
    void crate_destroyed(BlockPos crate);
    // This peer published CreatePickupMessage `pickup_id` for `stack`.
    void local_pickup_published(std::uint8_t spawner_gid, std::int32_t pickup_id, StackView stack);
    // The mod item a consumed pickup (spawner, id) holding retail `item` is;
    // the identity is used up.
    [[nodiscard]] std::uint32_t take_pickup_identity(std::uint8_t spawner_gid, std::int32_t pickup_id,
                                                     std::uint16_t item);

    // --- (Items) the mod item in each player's hand (src/modding/gameplay/items.cpp)
    // Stock messages carry the fallback of what a player holds; the client
    // announces the mod item over the mod channel ("held") whenever its hand
    // changes, and the server tells every client.
    //
    // The mod item (client session id; 0: none or unknown) player `gid`
    // holds, as the server last said. Meaningful only while that player's
    // stock hand holds the item's fallback, which the caller checks.
    [[nodiscard]] std::uint32_t client_held_item(std::uint8_t gid) const noexcept;
    // Server: the mod weapon player `gid` strikes with when its stock hand
    // holds retail `item` - the mod item it announced, when that is a weapon
    // whose fallback is `item` and which the player has (the local player's
    // hand is read live). nullopt: a retail swing, judged as before.
    [[nodiscard]] std::optional<ItemWeapon> held_weapon(std::uint8_t gid, std::uint16_t item) const;

    // --- (mod-ai) retail enemy rules, noise, projectiles, bosses -------------
    // Whether a retail enemy of this family may appear here, on any peer
    // (the rules the host set; this process's own server rules offline and
    // when hosting). True whenever no rules are set (or known yet: another
    // peer's spawn was made by the rules that peer had).
    [[nodiscard]] bool retail_enemy_allowed(RetailEnemyClass type, Vec3 position) const;
    // Whether this peer may spawn a retail enemy of its own here: as above,
    // and a joined client spawns none at all until the host's rules are in
    // (they come with its full state, right after its mod channel opens).
    [[nodiscard]] bool retail_spawn_allowed(RetailEnemyClass type, Vec3 position) const;
    // Whether the host's rules have arrived (always, on the host or offline).
    [[nodiscard]] bool retail_rules_known() const noexcept;
    // Whether a spawner block whose waves can bring these enemy types (Modern
    // wire numbers, world::spawner_wave_enemy_types) stays silent: every one
    // of them is switched off where its wave appears (`position`). Such a
    // block does not start, gives no reward and stays dark; one with any
    // member left runs, its switched-off members counted as defeated.
    [[nodiscard]] bool retail_spawner_silent(std::span<const std::uint8_t> wave_types, Vec3 position) const;
    // True once after rules with clear_existing reached this peer: the app
    // sends the retail enemies they switch off away.
    [[nodiscard]] bool take_retail_clear() noexcept;
    // The distance retail picks enemy and dragon tiers by: `distance`
    // unchanged without rules.
    [[nodiscard]] float retail_tier_distance(float distance, Vec3 position) const;
    [[nodiscard]] bool retail_rules_active() const noexcept;
    // A sound creatures may hear: a gunshot, an explosion, digging. `gid`
    // is the player who made it, when one did. Server half only.
    void noise(Vec3 position, float loudness, std::string_view kind, std::optional<std::uint8_t> gid);
    [[nodiscard]] const std::vector<ClientProjectile> &client_projectiles() const noexcept;
    [[nodiscard]] const std::vector<ClientBoss> &client_bosses() const noexcept;
    // Impacts since the last call (the app plays their effects and sounds).
    [[nodiscard]] std::vector<ClientImpact> take_client_impacts();
    // One line of AI numbers (thinks, paths, step time) for logs and tests.
    [[nodiscard]] std::string ai_report() const;
    // What the server's definitions get wrong together (a creature's brain or
    // projectile that is not defined, a boss phase's state its brain lacks, a
    // retail: look that is not a retail enemy), for `openminerz_mod_sdk
    // validate`. Empty when all is well.
    [[nodiscard]] std::vector<std::string> ai_definition_problems() const;

    // --- identity ------------------------------------------------------------
    // Persistent key of a player handle (for ModRuntime::set_player_key).
    [[nodiscard]] std::optional<std::string> player_key(Handle player) const;
    // The handle the game uses for a player (what ModNetServer::add_peer is
    // given), invalid when the player is not in the session.
    [[nodiscard]] Handle player_handle(std::uint8_t gid) const;
    [[nodiscard]] Handle local_player_handle() const;
    [[nodiscard]] std::optional<std::uint8_t> player_gid(Handle player) const;

    class Impl;

  private:
    friend class LoopbackGameplaySync;
    std::unique_ptr<Impl> impl_;
    // Lets the runtime's teardown listener outlive this object harmlessly.
    std::shared_ptr<bool> alive_;
    bool server_running_{};
    bool client_running_{};
    bool authority_{}; // (Authority)
};

// Text schematic (structures.parse and mod files):
//
//   # comment              (a '#' line inside a layer is a row when '#' is
//                          in the palette)
//   origin 1 0 1           anchor cell, relative to the schematic's corner
//   palette # rock         one character -> a block (retail name or mod key)
//   palette . air          "air" clears the cell; a space leaves it as it
//   layer                  is; any other character must be in the palette
//   ###                    rows run along +Z, characters along +X;
//   #.#                    each "layer" is one Y level, bottom first
//   ###
struct Schematic final
{
    int size_x{};
    int size_y{};
    int size_z{};
    BlockPos origin;
    // Palette index per cell (x fastest, then z, then y); 0 = leave as is.
    std::vector<std::uint16_t> cells;
    // Palette entries 1..n: retail name, "air" or a mod block key.
    std::vector<std::string> palette;
};

[[nodiscard]] bool parse_schematic(std::string_view text, Schematic &schematic,
                                   std::string &error);
// The cell a schematic cell lands on, rotated `quarter_turns` x 90 degrees
// clockwise (seen from above) about the anchor placed at `anchor`.
[[nodiscard]] BlockPos schematic_cell(const Schematic &schematic, BlockPos anchor, int x, int y,
                                      int z, int quarter_turns) noexcept;

// Retail block / item names as scripts use them ("rock", "gold_ore",
// "iron_pickaxe") and back.
[[nodiscard]] std::optional<std::uint8_t> retail_block_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view retail_block_name(std::uint8_t block) noexcept;
[[nodiscard]] std::optional<std::uint16_t> retail_item_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view retail_item_name(std::uint16_t item) noexcept;

// The sidecar's file inside GameplayBackend::sidecar_directory().
inline constexpr std::string_view sidecar_file_name = "gameplay.omzg";

} // namespace gameplay

// The module entry point (see api.hpp): creates the service, registers its API
// and returns it; the app keeps it for the life of the registry.
[[nodiscard]] std::unique_ptr<gameplay::GameplayService> register_gameplay_api(
    ApiRegistry &registry, ScriptHost &host, gameplay::GameplayBackend &backend);

} // namespace openminerz::modding
