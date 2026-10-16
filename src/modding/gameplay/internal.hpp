#pragma once

// GameplayService's private state, shared by the files of
// src/modding/gameplay/. Nothing outside that folder includes this.
//
//   service.cpp      lifecycle, hooks, tick, replication, protocol
//   api.cpp          every script-visible function and event
//   registry.cpp     blocks, items, recipes, loot, creatures, stats, effects
//   world_edits.cpp  edit jobs, schematics, the overlay, world generation
//   entities.cpp     mod creatures/NPCs: physics, behaviours, paths, spawning
//   quests.cpp       quests and dialogue
//   persistence.cpp  the world sidecar and its value codec
//   retail_names.cpp retail block/item names

#include "openminerz/modding/gameplay.hpp"

#include "ai.hpp" // (mod-ai)

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::modding::gameplay
{

// ---------------------------------------------------------------- values

[[nodiscard]] inline const Value &arg(std::span<const Value> args, std::size_t index) noexcept
{
    static const Value nil;
    return index < args.size() ? args[index] : nil;
}

[[nodiscard]] Value make_map(std::initializer_list<std::pair<std::string, Value>> fields);
[[nodiscard]] Value vec3_value(Vec3 value);
[[nodiscard]] Value cell_value(BlockPos cell);
// A position argument: a vector, {x=,y=,z=} or {x, y, z}.
[[nodiscard]] Vec3 to_vec3(const Value &value, std::string_view what);
[[nodiscard]] std::optional<Vec3> try_vec3(const Value &value) noexcept;
[[nodiscard]] BlockPos to_cell(const Value &value, std::string_view what);
[[nodiscard]] double number_or(const Value &table, std::string_view key, double fallback);
[[nodiscard]] std::string string_or(const Value &table, std::string_view key,
                                    std::string_view fallback);
[[nodiscard]] bool bool_or(const Value &table, std::string_view key, bool fallback);
[[nodiscard]] Callback callback_or_none(const Value &table, std::string_view key);
[[nodiscard]] BlockPos floor_cell(Vec3 position) noexcept;
[[nodiscard]] double distance_squared(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] bool finite(Vec3 value) noexcept;
// (mod-liquid, liquids.cpp) A creature body in a liquid rises toward its
// surface: the vertical speed after `dt` from `vertical` (a jump out of it
// keeps its speed and falls back as usual).
[[nodiscard]] double wading_vertical_speed(double vertical, double dt) noexcept;

// --------------------------------------------------------------- definitions

struct ItemRef final
{
    std::uint16_t item{};    // retail InventoryItemId (the fallback for a mod item)
    std::uint32_t mod_item{}; // session id, 0 for retail
    [[nodiscard]] bool valid() const noexcept
    {
        return item != 0U;
    }
    auto operator<=>(const ItemRef &) const = default;
};

struct BlockDef final
{
    std::uint16_t id{};
    std::string key;
    std::string mod;
    std::string name;
    std::uint8_t fallback{};
    std::string texture;
    std::array<std::string, 3> faces;
    std::string model;
    std::uint8_t light{};
    // What breaking it gives: a loot table key, an item, or the mod item of
    // the same key when nothing is named and one exists.
    std::string drops;
    std::uint16_t drop_count{1U};
    // (mod-liquid) A liquid (openminerz/world/liquid.hpp): stops nothing, is
    // swum in, passed by the crosshair and drawn translucent.
    bool liquid{};
};

struct ItemDef final
{
    std::uint32_t id{};
    std::string key;
    std::string mod;
    std::string name;
    std::string description;
    std::uint16_t fallback{};
    std::string icon;
    std::string model;
    std::uint16_t stack_limit{};
    // A block this item places (its mod block or a retail one); 0 = none.
    std::uint16_t places_block{};
    Callback on_use;
    float cooldown{0.25F};
    // (Items) items.define hold, scale and weapon (items.cpp).
    HoldStyle hold{HoldStyle::fallback};
    float scale{1.0F};
    std::optional<ItemWeapon> weapon;
};

struct RecipeDef final
{
    std::uint32_t id{};
    std::string mod;
    std::string category;
    ItemRef result;
    std::uint16_t count{1U};
    std::vector<std::pair<ItemRef, std::uint16_t>> ingredients;
};

struct LootEntry final
{
    ItemRef item;
    int weight{1};
    int minimum{1};
    int maximum{1};
};

struct LootTable final
{
    std::string key;
    int rolls_min{1};
    int rolls_max{1};
    std::vector<LootEntry> entries;
};

enum class Brain : std::uint8_t
{
    passive, // wanders, flees when hurt
    neutral, // wanders, fights back
    hostile, // hunts players in range
    npc      // stands, faces players, can be talked to
};

enum class SpawnTime : std::uint8_t
{
    any,
    day,
    night
};

struct SpawnRule final
{
    bool enabled{};
    std::vector<int> biomes; // empty = every biome
    float light_min{};
    float light_max{1.0F};
    SpawnTime time{SpawnTime::any};
    bool surface{true};
    float chance{0.05F}; // per attempt (one attempt per player per second)
    int max_near_player{4};
    int group_min{1};
    int group_max{1};
    int y_min{-64};
    int y_max{63};
};

struct CreatureDef final
{
    std::uint32_t id{};
    std::string key;
    std::string mod;
    std::string name;
    std::string model;
    std::string texture;
    float health{20.0F};
    float speed{2.0F};
    float run_speed{4.0F};
    float damage{10.0F};
    float attack_range{1.6F};
    float attack_cooldown{1.0F};
    float view_range{16.0F};
    float width{0.6F};
    float height{1.8F};
    Brain brain{Brain::passive};
    std::string drops; // loot table key or item
    bool interactable{};
    std::string dialogue;
    bool show_name{};
    SpawnRule spawn;
    // (mod-ai) Brain, senses, team, attacks, movement and boss; null for a
    // creature that keeps the built-in behaviours above.
    std::shared_ptr<const CreatureAi> ai;
};

struct StatDef final
{
    std::string name;
    std::string mod;
    double minimum{};
    double maximum{100.0};
    double initial{100.0};
    double decay{}; // per second, toward minimum (negative regenerates)
    bool is_public{};
    bool persist{true};
    bool hud{};
    std::string label;
    std::string color;
    std::string icon;
};

struct EffectDef final
{
    std::string name;
    std::string mod;
    Callback on_start;
    Callback on_tick;
    Callback on_end;
    float interval{1.0F};
    std::string icon;
};

enum class ObjectiveKind : std::uint8_t
{
    kill,
    collect,
    reach,
    talk,
    custom
};

struct ObjectiveDef final
{
    ObjectiveKind kind{ObjectiveKind::custom};
    std::string id;
    std::string text;
    std::string target; // enemy type/class name, creature key, item, NPC
    std::uint32_t count{1U};
    Vec3 position;
    float radius{4.0F};
};

struct StageDef final
{
    std::string id;
    std::string text;
    std::vector<ObjectiveDef> objectives;
};

struct QuestDef final
{
    std::string id;
    std::string mod;
    std::string title;
    std::string description;
    std::vector<StageDef> stages;
    std::vector<std::pair<ItemRef, std::uint16_t>> reward_items;
    Callback on_stage;    // (player, quest, stage)
    Callback on_complete; // (player, quest)
    bool repeatable{};
};

struct DialogueChoice final
{
    std::string text;
    std::string next; // "" closes
    Callback condition;
    Callback action;
    std::string start_quest;
};

struct DialogueNode final
{
    std::string id;
    std::string speaker;
    std::string text;
    std::string portrait;
    std::vector<DialogueChoice> choices;
    std::string next;
    Callback on_enter;
};

struct DialogueDef final
{
    std::string id;
    std::string mod;
    std::string title;
    std::string start;
    std::map<std::string, DialogueNode, std::less<>> nodes;
};

struct CommandDef final
{
    std::string name;
    std::string help;
    std::string mod;
    Callback function;
};

// One side's definitions. The server's is authoritative and is what the
// registry snapshot carries; a client VM's own define() calls land in its own
// copy (presentation and queries only).
struct Registry final
{
    std::vector<BlockDef> blocks;
    std::vector<ItemDef> items;
    std::vector<RecipeDef> recipes;
    std::set<std::size_t> disabled_retail_recipes;
    std::map<std::string, LootTable, std::less<>> loot;
    std::multimap<int, std::string> enemy_loot; // retail enemy type -> table
    std::vector<CreatureDef> creatures;
    std::map<std::string, StatDef, std::less<>> stats;
    std::map<std::string, EffectDef, std::less<>> effects;
    std::map<std::string, QuestDef, std::less<>> quests;
    std::map<std::string, DialogueDef, std::less<>> dialogues;
    std::map<std::string, Schematic, std::less<>> structures;
    std::map<std::string, CommandDef, std::less<>> commands;
    // (mod-ai) Brains and projectiles, by full key ("mod:name").
    std::map<std::string, BrainDef, std::less<>> brains;
    std::map<std::string, ProjectileDef, std::less<>> projectiles;

    [[nodiscard]] const BlockDef *block(std::string_view key) const noexcept;
    [[nodiscard]] const BlockDef *block(std::uint16_t id) const noexcept;
    [[nodiscard]] const ItemDef *item(std::string_view key) const noexcept;
    [[nodiscard]] const ItemDef *item(std::uint32_t id) const noexcept;
    [[nodiscard]] const CreatureDef *creature(std::string_view key) const noexcept;
    [[nodiscard]] const CreatureDef *creature(std::uint32_t id) const noexcept;
};

// ------------------------------------------------------------ runtime state

struct ActiveEffect final
{
    std::string kind;
    float remaining{-1.0F}; // < 0: until removed
    float strength{1.0F};
    float interval_left{};
    std::string stat; // the "stat" effect: which stat it moves
};

struct ObjectiveProgress final
{
    std::uint32_t progress{};
    bool done{};
};

struct QuestProgress final
{
    std::size_t stage{};
    std::vector<ObjectiveProgress> objectives;
    bool completed{};
};

struct DialogueSession final
{
    std::uint64_t id{};
    std::string dialogue;
    std::string node;
    std::uint64_t entity{};
    // Indices into the node's choices that passed their conditions, in the
    // order the client was shown them.
    std::vector<std::size_t> shown;
};

// What persists for a player between sessions, by gamer tag.
struct PersistedPlayer final
{
    std::map<std::string, double, std::less<>> stats;
    std::map<std::string, QuestProgress, std::less<>> quests;
    std::set<std::string, std::less<>> completed_quests;
    std::vector<std::pair<std::uint16_t, std::string>> inventory_tags;
};

struct PlayerRecord final
{
    std::uint64_t serial{};
    std::uint8_t gid{};
    std::string name;
    bool local{};
    bool alive{true};
    bool present{true};
    bool spawned{};
    bool restored{};  // saved data applied (see Impl::restore)
    bool announced{}; // player.joined reached the scripts
    // The app admitted this player to the session (its mod channel or this
    // process's own player): it stays until remote_player_left, whatever the
    // game's own player list says meanwhile, so its handle never changes.
    bool admitted{};
    // When the last full state went out (server_time), to answer a repeated
    // "ready" at most every few seconds.
    double full_state_at{-1.0e9};
    Vec3 position;
    float yaw{};
    float health{100.0F};
    // Server-owned mod state.
    std::map<std::string, double, std::less<>> stats;
    std::map<std::string, double, std::less<>> stats_sent;
    bool stats_full_resend{true};
    std::vector<ActiveEffect> effects;
    bool effects_dirty{true};
    std::map<std::string, QuestProgress, std::less<>> quests;
    std::set<std::string, std::less<>> completed_quests;
    bool quests_dirty{true};
    std::optional<DialogueSession> dialogue;
    // The owner's last reports (remote players; the local one is read live).
    std::map<std::string, std::uint32_t, std::less<>> inventory_counts;
    std::vector<std::pair<std::uint16_t, std::string>> inventory_tags;
    // Rate limit for client messages.
    float tokens{};
    float item_cooldown{};
    float attack_tokens{};
    // (Items) The mod item the player's client says is in its hand (server
    // session id, 0 for none), and its swings with a mod weapon: a bucket
    // refilled at the weapon's own rate.
    std::uint32_t held_item{};
    float weapon_tokens{};
    double weapon_tokens_at{-1.0};
    // (mod-ai) The entities this player's client was told about, and what it
    // was last sent of each (interest management, entities.cpp).
    struct SentEntity final
    {
        Vec3 position;
        float yaw{};
        std::string animation;
        float health{-1.0F};
        std::string name;
    };
    std::unordered_map<std::uint64_t, SentEntity> known_entities;
};

enum class BehaviourKind : std::uint8_t
{
    idle,
    wander,
    flee,
    follow,
    attack,
    path,
    look
};

struct EntityTarget final
{
    std::uint64_t player{}; // player serial
    std::uint64_t entity{};
    std::optional<Vec3> position;
    [[nodiscard]] bool any() const noexcept
    {
        return player != 0U || entity != 0U || position.has_value();
    }
};

struct Behaviour final
{
    BehaviourKind kind{BehaviourKind::idle};
    EntityTarget target;
    float distance{2.0F};
    float timer{};     // seconds left (scripted behaviours end at 0 when > 0)
    bool scripted{};   // set by a script: the brain does not override it
    Vec3 goal;
    bool has_goal{};
};

struct Entity final
{
    std::uint64_t id{};
    std::uint32_t creature{};
    Vec3 position;
    Vec3 velocity;
    float yaw{};
    float health{};
    float max_health{};
    bool grounded{};
    bool persistent{};
    bool from_spawn_rule{};
    bool removed{};
    std::string name;
    std::string animation{"idle"};
    std::string animation_override;
    Behaviour behaviour;
    std::vector<BlockPos> path;
    std::size_t path_index{};
    float attack_timer{};
    float hurt_timer{};
    float think_timer{};
    float far_timer{};
    float stuck_timer{};
    std::uint64_t last_attacker{}; // player serial
    Value data;
    // Last replicated state.
    Vec3 sent_position;
    float sent_yaw{};
    std::string sent_animation;
    float sent_health{-1.0F};
    std::string sent_name;
    EntityMind mind; // (mod-ai)
};

struct EditCell final
{
    BlockPos cell;
    std::uint8_t block{};
    std::uint16_t mod_block{};
    // Replace jobs: only cells holding this (retail id, mod block) change.
    bool conditional{};
    std::uint8_t from_block{};
    std::uint16_t from_mod_block{};
};

struct EditJob final
{
    std::uint64_t id{};
    std::string mod;
    std::vector<EditCell> cells;
    std::size_t next{};
    std::size_t changed{};
    Callback on_done;
};

// ----------------------------------------------------------------- the Impl

class GameplayService::Impl final
{
  public:
    Impl(GameplayService &owner, ScriptHost &host, GameplayBackend &backend,
         GameplayLimits limits);

    GameplayService &owner;
    ScriptHost &host;
    GameplayBackend &backend;
    GameplayLimits limits;
    LoopbackGameplaySync loopback;
    GameplaySync *sync{};

    Registry server_registry;
    Registry client_registry;
    // Every callback a definition holds, per side, released on teardown.
    std::vector<Callback> held_callbacks[2];

    // --- server state ---
    std::uint64_t next_player_serial{1U};
    std::vector<PlayerRecord> players; // present and departed this session
    std::map<std::string, PersistedPlayer, std::less<>> persisted_players;
    std::map<BlockPos, std::uint16_t> overlay;
    std::vector<std::pair<BlockPos, std::uint16_t>> overlay_outbox;
    std::set<std::pair<int, int>> decorated_chunks;
    std::deque<std::pair<int, int>> worldgen_queue;
    std::set<std::pair<int, int>> worldgen_queued;
    // World generation is opt-in (worldgen.enable): only then are chunks
    // offered and remembered as decorated.
    bool worldgen_enabled{};
    BlockPos overlay_cursor{std::numeric_limits<int>::min(), 0, 0};
    std::vector<Entity> entities;
    std::uint64_t next_entity_id{1U};
    float entity_step_accumulator{};
    std::deque<EditJob> edit_jobs;
    std::uint64_t next_job_id{1U};
    std::uint64_t next_dialogue_id{1U};
    std::uint32_t next_recipe_id{1U};
    DifficultyScaling difficulty;
    bool difficulty_dirty{};
    double server_time{};
    float replicate_timer{};
    bool replicate_due{};
    float spawn_timer{};
    float sweep_timer{};
    float quest_timer{};
    int last_day{-1};
    int last_phase{-1}; // 0 day, 1 night
    std::uint64_t tick_count{};
    // players.set_damage target while player.damaged handlers run.
    std::vector<float *> damage_overrides;
    std::uint32_t random_state{0x9E3779B9U};
    bool loaded_sidecar{};
    // The sidecar and players are in (start_server, or the first tick after
    // a deferred start).
    bool activated{};
    void activate();
    bool refreshing_players{};

    // --- client state ---
    std::vector<ClientStat> client_stats;
    std::vector<ClientEffect> client_effects;
    std::vector<ClientEntity> client_entities;
    std::optional<ClientDialogue> client_dialogue;
    std::vector<ClientQuest> client_quests;
    DifficultyScaling client_difficulty;
    float client_speed{1.0F};
    std::vector<ClientBlockType> client_blocks;
    std::vector<ClientItemType> client_items;
    std::vector<ClientRecipe> client_recipes;
    std::map<std::string, std::uint16_t, std::less<>> client_block_ids;
    std::map<std::string, std::uint32_t, std::less<>> client_item_ids;
    std::map<BlockPos, std::uint16_t> client_overlay;
    std::set<std::size_t> client_disabled_recipes;
    std::function<void(ClientChange)> client_listener;
    // (Visuals) notify() counts per ClientChange; the last entities that died.
    std::array<std::uint64_t, 8> client_revisions{};
    std::deque<std::uint64_t> client_deaths;
    float client_report_timer{};
    float client_reported_health{-1.0F};
    std::map<std::string, std::uint32_t, std::less<>> client_reported_counts;
    std::vector<std::pair<std::uint16_t, std::string>> client_reported_tags;
    std::vector<std::pair<std::uint16_t, std::string>> client_pending_tags;
    float client_tags_seconds{};

    // (Integration) Mod item identities of crate slots and network pickups
    // (containers.cpp): the server's (crates persist in the sidecar) and the
    // copy a client was sent.
    using CrateSlot = std::pair<BlockPos, std::uint8_t>;
    using PickupKey = std::pair<std::uint8_t, std::int32_t>; // spawner gid, pickup id
    std::map<CrateSlot, std::uint32_t> crate_tags;
    std::map<PickupKey, std::uint32_t> pickup_tags;
    std::deque<PickupKey> pickup_order;
    std::map<CrateSlot, std::uint32_t> client_crate_tags;
    std::map<PickupKey, std::uint32_t> client_pickup_tags;
    std::deque<PickupKey> client_pickup_order;
    bool set_crate_tag(BlockPos crate, std::uint8_t slot, std::uint32_t mod_item);
    void record_pickup(std::uint8_t spawner, std::int32_t pickup, std::uint32_t mod_item);
    void server_crate_message(PlayerRecord &player, const Value &message);
    void server_pickup_message(PlayerRecord &player, const Value &message);
    void send_crate_tags(std::uint8_t gid);
    // This peer's crates get the identities the sidecar brought back.
    void apply_crate_tags();
    void client_crate_message(const Value &message);
    void client_pickup_message(const Value &message);

    // (Items) Held mod items and mod weapons (items.cpp). The server's
    // definition fields, the "held" messages both ways, and the checks of a
    // swing made with a mod weapon.
    void define_item_presentation(ItemDef &item, const Value &def);
    void server_held_message(PlayerRecord &player, const Value &message);
    void send_held_items(std::uint8_t gid);
    void forget_held_item(const PlayerRecord &player);
    void client_held_message(const Value &message);
    void client_report_held();
    // The weapon `player` swings with its stock hand on retail `item`, by
    // the server's own definitions (Impl side of held_weapon).
    [[nodiscard]] const ItemDef *held_weapon_def(const PlayerRecord &player, std::uint16_t item) const;
    // A claimed swing of `weapon` by `player`: within the weapon's rate.
    [[nodiscard]] bool take_weapon_swing(PlayerRecord &player, const ItemWeapon &weapon);
    std::map<std::uint8_t, std::uint32_t> client_held; // gid -> client item id
    std::uint32_t client_held_sent{};
    bool client_held_known{};

    // ------------------------------------------------------------ helpers
    [[nodiscard]] Registry &registry(Side side) noexcept
    {
        return side == Side::server ? server_registry : client_registry;
    }
    void hold(Side side, const Callback &callback);
    void release_side(Side side);
    [[nodiscard]] GameplaySync &out() noexcept
    {
        return sync != nullptr ? *sync : loopback;
    }
    bool emit(Side side, std::string_view event, Value payload);
    void log(std::string_view mod, LogLevel level, std::string_view message);
    [[nodiscard]] float random01() noexcept;
    [[nodiscard]] int random_int(int minimum, int maximum_inclusive) noexcept;

    // Players (service.cpp)
    void refresh_players();
    [[nodiscard]] PlayerRecord *player_by_gid(std::uint8_t gid) noexcept;
    [[nodiscard]] PlayerRecord *player_by_serial(std::uint64_t serial) noexcept;
    [[nodiscard]] PlayerRecord *player_by_handle(Handle handle) noexcept;
    [[nodiscard]] PlayerRecord &player_arg(const Value &value);
    [[nodiscard]] PlayerRecord *local_player() noexcept;
    [[nodiscard]] Handle handle_of(const PlayerRecord &player) const noexcept;
    // A new record (or the present one for `gid`); player.joined reaches the
    // scripts at once when `announce` and the server runs, else at activation
    // or through announce().
    PlayerRecord &join(std::uint8_t gid, std::string_view name, bool local, bool announce = true);
    void announce(PlayerRecord &player);
    // Departed records past the newest few are forgotten (their data was
    // stored by leave()), so a long session's joins cannot grow without end.
    void prune_departed();
    void restore(PlayerRecord &player);
    void ensure_players();
    void leave(PlayerRecord &player);
    void store_persisted(const PlayerRecord &player);
    // Player operations that reach a remote player through a directive.
    void set_health(PlayerRecord &player, float health);
    void damage(PlayerRecord &player, float amount, DamageCause cause, Vec3 source);
    void heal(PlayerRecord &player, float amount);
    void teleport(PlayerRecord &player, Vec3 position);
    std::uint32_t give(PlayerRecord &player, ItemRef item, std::uint32_t count);
    std::uint32_t take(PlayerRecord &player, ItemRef item, std::uint32_t count);
    [[nodiscard]] std::uint32_t count(PlayerRecord &player, ItemRef item);
    void clear_inventory(PlayerRecord &player);
    void message(std::optional<std::uint8_t> gid, std::string_view text);
    void directive(PlayerRecord &player, Value message);
    // Damage of the player this process simulates, through player.damaged.
    [[nodiscard]] float filter_local_damage(PlayerRecord &player, float amount,
                                            DamageCause cause, Vec3 source);
    [[nodiscard]] ChatVerdict chat(PlayerRecord &player, std::string_view text);
    bool run_command(PlayerRecord &player, std::string_view text);
    void handle_client_message(PlayerRecord &player, const Value &message);
    // `aim` is the aimed-block table of item.used ({position, block, face,
    // look}) or nil.
    void use_item(PlayerRecord &player, ItemRef item, const Value &aim = {});
    // (Integration) That table for a cell the player aims at, read from the
    // authoritative world; nil without a cell.
    // (mod-liquid) `liquid`: the first liquid cell on the look, when the
    // server agrees it holds one (the table's `liquid = {position, block}`).
    [[nodiscard]] Value aim_value(std::optional<BlockPos> cell, std::optional<BlockPos> face,
                                  std::optional<Vec3> look, std::optional<BlockPos> liquid = {});
    void craft(PlayerRecord &player, std::uint32_t recipe_id);

    // Registry (registry.cpp)
    [[nodiscard]] std::string qualify(std::string_view mod, std::string_view name) const;
    // Releases every callback in a definition table that is not in `kept`
    // (the runtime holds the callbacks it passes in for the service).
    void release_unused(const Value &definition, std::span<const Callback> kept);
    // A script's item: a retail name or id, a mod key ("mod:name"), or a short
    // name of the calling mod's own item. Throws ScriptError when unknown.
    [[nodiscard]] ItemRef resolve_item(const Value &value, std::string_view mod,
                                       std::string_view what, Side side = Side::server) const;
    [[nodiscard]] std::string item_label(ItemRef item) const;
    [[nodiscard]] std::string item_key(ItemRef item) const;
    struct BlockSpec final
    {
        std::uint8_t block{};
        std::uint16_t mod_block{};
    };
    [[nodiscard]] BlockSpec resolve_block(const Value &value, std::string_view mod,
                                          std::string_view what) const;
    [[nodiscard]] std::string block_label(std::uint8_t block, std::uint16_t mod_block) const;
    [[nodiscard]] bool retail_block_valid(std::uint8_t block) const;
    [[nodiscard]] bool retail_item_valid(std::uint16_t item) const;
    void define_block(Side side, std::string_view mod, std::string_view name, const Value &def);
    void define_item(Side side, std::string_view mod, std::string_view name, const Value &def);
    std::uint32_t add_recipe(Side side, std::string_view mod, const Value &def);
    bool remove_recipe(Side side, std::string_view mod, const Value &what);
    void add_enemy_loot(Side side, std::string_view mod, const Value &enemy, const Value &table);
    void define_loot(Side side, std::string_view mod, std::string_view name, const Value &def);
    void define_creature(Side side, std::string_view mod, std::string_view name,
                         const Value &def);
    void define_stat(Side side, std::string_view mod, std::string_view name, const Value &def);
    void define_effect(Side side, std::string_view mod, std::string_view name, const Value &def);
    [[nodiscard]] std::vector<std::pair<ItemRef, std::uint16_t>> roll_loot(
        std::string_view table);
    [[nodiscard]] Value stacks_value(const std::vector<std::pair<ItemRef, std::uint16_t>> &stacks)
        const;
    [[nodiscard]] Value registry_snapshot() const;
    void apply_registry(const Value &snapshot);

    // Stats and effects (registry.cpp)
    [[nodiscard]] double stat(PlayerRecord &player, std::string_view name);
    double set_stat(PlayerRecord &player, std::string_view name, double value, bool notify);
    void apply_effect(PlayerRecord &player, std::string_view kind, const Value &options);
    bool remove_effect(PlayerRecord &player, std::string_view kind);
    void tick_stats_and_effects(PlayerRecord &player, float seconds);
    [[nodiscard]] float speed_multiplier(const PlayerRecord &player) const;
    void replicate_player(PlayerRecord &player, bool force);

    // World (world_edits.cpp)
    [[nodiscard]] std::optional<std::uint8_t> block_at(BlockPos cell) const;
    [[nodiscard]] std::uint16_t mod_block_at(BlockPos cell, std::uint8_t current);
    [[nodiscard]] std::uint16_t generated_block_at(BlockPos cell, std::uint8_t current,
                                                   bool changed) const; // mod-world
    bool set_cell(BlockPos cell, std::uint8_t block, std::uint16_t mod_block);
    void forget_overlay(BlockPos cell);
    std::uint64_t queue_job(std::string_view mod, std::vector<EditCell> cells, Callback on_done);
    void run_jobs();
    std::uint64_t place_structure(std::string_view mod, const Schematic &schematic, BlockPos at,
                                  int quarter_turns, bool include_air, Callback on_done);
    // structures.define's table form: {palette = {["#"] = "rock"}, layers =
    // {{"###", "#.#"}, ...}, origin = {x, y, z}}.
    [[nodiscard]] Schematic schematic_from_table(const Value &table) const;
    [[nodiscard]] std::optional<std::pair<BlockPos, BlockPos>> raycast(Vec3 origin,
                                                                       Vec3 direction,
                                                                       float reach) const;
    [[nodiscard]] std::optional<int> surface_y(int x, int z) const;
    void queue_worldgen();
    void run_worldgen();
    void sweep_overlay();
    void flush_overlay();
    [[nodiscard]] Value overlay_value(const std::vector<std::pair<BlockPos, std::uint16_t>> &cells)
        const;

    // Entities (entities.cpp)
    Entity *spawn_entity(const CreatureDef &creature, Vec3 position, const Value &options);
    [[nodiscard]] Entity *entity(std::uint64_t id) noexcept;
    [[nodiscard]] Entity &entity_arg(const Value &value);
    [[nodiscard]] Handle entity_handle(const Entity &entity) const noexcept;
    void remove_entity(Entity &entity, bool died);
    void damage_entity(Entity &entity, float amount, std::uint64_t attacker_player,
                       std::uint64_t attacker_entity);
    void set_behaviour(Entity &entity, std::string_view kind, const Value &options);
    void tick_entities(float seconds);
    void spawn_rules();
    void replicate_entities(bool force);
    [[nodiscard]] Value entity_spawn_value(const Entity &entity) const;
    [[nodiscard]] Value entity_info(const Entity &entity) const;
    bool interact(PlayerRecord &player, Entity &entity);
    [[nodiscard]] std::optional<std::pair<Entity *, float>> trace_entities(Vec3 origin,
                                                                           Vec3 direction,
                                                                           float reach,
                                                                           bool interactable_only);
    [[nodiscard]] bool solid(int x, int y, int z) const;
    // (mod-liquid) Whether a cell holds a liquid mod block (`liquid = true`)
    // as the server knows it: its fallback is there, and the overlay or the
    // world generator says which mod block that is. False at once while the
    // session defines no liquid.
    [[nodiscard]] bool liquid_cell(BlockPos cell) const;
    // The first liquid cell on a ray within reach, stopping at anything else
    // solid (item.used's target.liquid).
    [[nodiscard]] std::optional<BlockPos> liquid_on_ray(Vec3 origin, Vec3 direction, float reach) const;
    // How deep a point is under the surface of the liquid it is in (0: none).
    [[nodiscard]] double liquid_depth_at(Vec3 point) const;
    // A creature body whose middle is in a liquid floats and wades; one on
    // land does not step where its feet, or the ground under them, would be
    // a liquid.
    [[nodiscard]] bool wading_in_liquid(Vec3 feet, double height) const;
    [[nodiscard]] bool steps_into_liquid(Vec3 feet) const;
    std::array<bool, 256> liquid_fallbacks{};
    bool liquids_defined{};
    void refresh_liquids();

    // (mod-ai) Creature AI (ai.cpp, ai_navigation.cpp, ai_projectiles.cpp,
    // ai_retail.cpp, ai_api.cpp; state in ai.hpp).
    AiWorld ai;
    AiClientState ai_client;
    // Definitions.
    void define_creature_ai(Side side, std::string_view mod, CreatureDef &creature, const Value &def,
                            std::vector<Callback> &kept);
    void define_brain(Side side, std::string_view mod, std::string_view name, const Value &def);
    void define_projectile(Side side, std::string_view mod, std::string_view name, const Value &def);
    [[nodiscard]] const BrainDef *brain(std::string_view key, std::string_view mod) const noexcept;
    [[nodiscard]] const ProjectileDef *projectile_def(std::string_view key, std::string_view mod) const noexcept;
    // Lifecycle of a mind.
    void ai_entity_spawned(Entity &entity, const CreatureDef &creature, const Value &options);
    void ai_entity_removed(Entity &entity, bool died);
    void join_pack(Entity &entity, Entity &other);
    // After damage_entity applied `amount` (the entity still lives).
    void ai_entity_damaged(std::uint64_t id, float amount, std::uint64_t attacker_player,
                           std::uint64_t attacker_entity);
    // One 20 Hz step of an AI creature (from tick_entities).
    void tick_ai_entity(std::size_t index, float dt, double nearest_squared);
    // Once per step before the creatures (budgets, the spatial grid) and once
    // per server tick after them (projectiles, bosses, noise, rules).
    void ai_begin_step();
    void tick_ai_world(float seconds);
    void ai_stop_server();
    void ai_stop_client();
    void ai_send_full_state(PlayerRecord &player);
    [[nodiscard]] bool ai_client_receive(std::string_view type, const Value &message);
    void ai_client_tick(float seconds);
    void ai_block_changed(BlockPos cell);
    // A cell dug, blasted or built (`player` = its serial, 0 for none): plans
    // through it are made again, and creatures may hear it.
    void ai_world_edit(BlockPos cell, std::uint64_t player, float loudness);
    // A client's claim that it struck a creature ("attack"), checked.
    // (Items) `reach`: blocks a melee swing reaches (a mod weapon's own).
    [[nodiscard]] bool ai_attack_believable(const PlayerRecord &player, const Entity &target,
                                            std::uint16_t weapon, const Value &message,
                                            float reach = 5.0F);
    // Brains and minds (ai.cpp).
    void think(std::size_t index);
    void perceive(Entity &body, const CreatureDef &creature);
    void set_state(Entity &body, std::string_view state);
    void run_state_change(std::size_t index);
    void set_target(Entity &body, std::uint64_t player, std::uint64_t entity, std::string_view why);
    void hunt(Entity &body, const CreatureDef &creature);
    [[nodiscard]] Value mind_info(const Entity &body, const CreatureDef &creature, float dt);
    bool invoke_brain(std::size_t index, const Callback &callback, Value info, bool takes_state);
    [[nodiscard]] std::string team_of_entity(const Entity &body) const;
    [[nodiscard]] std::string team_of_player(std::uint64_t serial) const;
    [[nodiscard]] Relation relation(std::string_view from_team, std::string_view to_team,
                                    const CreatureAi *from) const;
    [[nodiscard]] bool hostile_to_player(const Entity &body, const CreatureDef &creature,
                                         std::uint64_t serial) const;
    [[nodiscard]] bool hostile_to_entity(const Entity &body, const CreatureDef &creature,
                                         const Entity &other) const;
    [[nodiscard]] std::optional<Vec3> target_point(std::uint64_t player, std::uint64_t entity) const;
    [[nodiscard]] Vec3 eye_of(const Entity &body) const;
    [[nodiscard]] bool line_of_sight(Vec3 from, Vec3 to);
    MemoryEntry &remember(EntityMind &mind, std::uint64_t player, std::uint64_t entity);
    // Attacks (ai.cpp).
    bool start_attack(std::size_t index, std::string_view name, std::uint64_t player,
                      std::uint64_t entity, bool ability);
    void advance_attack(std::size_t index, float dt);
    // Movement (ai_navigation.cpp).
    void move_ai_entity(std::size_t index, float dt);
    [[nodiscard]] bool ai_solid(int x, int y, int z);
    [[nodiscard]] bool water(int x, int y, int z);
    [[nodiscard]] bool standable(const CreatureDef &creature, BlockPos cell);
    [[nodiscard]] std::optional<BlockPos> next_step_toward(Entity &body, const CreatureDef &creature,
                                                           BlockPos goal, bool follow_field,
                                                           std::uint64_t target_key);
    [[nodiscard]] std::optional<Vec3> surround_slot(const Entity &body, Vec3 around, float radius);
    // Projectiles (ai_projectiles.cpp).
    Projectile *launch_projectile(const ProjectileDef &def, Vec3 from, Vec3 velocity,
                                  std::uint64_t owner_entity, std::uint64_t owner_player,
                                  std::uint64_t target_player, std::uint64_t target_entity,
                                  float damage_scale);
    [[nodiscard]] std::optional<Vec3> aim_velocity(Vec3 from, Vec3 target, Vec3 target_velocity,
                                                   const ProjectileDef &def, bool lead);
    void tick_projectiles(float dt);
    void explode(const ProjectileDef &def, Vec3 at, std::uint64_t owner_entity,
                 std::uint64_t owner_player, float damage_scale);
    // Whether a projectile of this kind may dig this cell out (fix: blocks /
    // breaks_blocks, spawn protection).
    [[nodiscard]] bool projectile_may_break(const ProjectileDef &def, BlockPos cell) const;
    [[nodiscard]] Value projectile_value(const Projectile &projectile) const;
    // Bosses (ai.cpp).
    void tick_bosses();
    // Replication of interest (entities.cpp).
    void forget_entity_for_all(std::uint64_t id, bool died);
    // Retail enemy rules (ai_retail.cpp).
    [[nodiscard]] RetailEnemyRules parse_retail_rules(const Value &rules) const;
    [[nodiscard]] Value retail_rules_value(const RetailEnemyRules &rules) const;
    [[nodiscard]] RetailEnemyRules retail_rules_from_value(const Value &value) const;
    [[nodiscard]] std::string biome_name(int x, int z) const;
    [[nodiscard]] const RetailEnemyRules &effective_retail_rules() const noexcept;

    // Quests and dialogue (quests.cpp)
    void define_quest(Side side, std::string_view mod, std::string_view id, const Value &def);
    void define_dialogue(Side side, std::string_view mod, std::string_view id, const Value &def);
    bool start_quest(PlayerRecord &player, std::string_view id);
    void advance_objective(PlayerRecord &player, std::string_view quest, std::string_view objective,
                           std::uint32_t amount);
    void complete_quest(PlayerRecord &player, std::string_view id);
    bool abandon_quest(PlayerRecord &player, std::string_view id);
    void quest_event(PlayerRecord &player, ObjectiveKind kind, std::string_view target,
                     std::uint32_t amount);
    void tick_quests();
    [[nodiscard]] Value quest_status(const PlayerRecord &player, std::string_view id) const;
    void replicate_quests(PlayerRecord &player);
    bool open_dialogue(PlayerRecord &player, std::string_view id, std::string_view node,
                       std::uint64_t entity);
    void show_dialogue_node(PlayerRecord &player);
    void choose(PlayerRecord &player, std::uint64_t session, std::size_t choice);
    void close_dialogue(PlayerRecord &player, bool notify);

    // Persistence (persistence.cpp)
    void load_sidecar();
    void save_sidecar();

    // Client half (service.cpp)
    void client_receive(const Value &message);
    void client_tick(float seconds);
    void notify(ClientChange change);
};

// (Items) What a client needs of an item's hold, size, use rate and weapon
// stats: the "hand" field of a registry item, written and read back
// (items.cpp). An absent or malformed field reads as a plain item.
[[nodiscard]] Value item_hand_value(const ItemDef &item);
void read_item_hand(ClientItemType &item, const Value *hand);

// (mod-ai) Registers ai, brains, projectiles, teams, enemies, entities.find
// and the AI events (ai_api.cpp); called from GameplayService::register_api.
void register_ai_api(ApiRegistry &registry, GameplayService &service, GameplayService::Impl &impl);

// The sidecar's binary value encoding (persistence.cpp; also used by tests).
[[nodiscard]] std::string encode_sidecar_value(const Value &value);
[[nodiscard]] bool decode_sidecar_value(std::string_view bytes, Value &value, std::string &error);

} // namespace openminerz::modding::gameplay
