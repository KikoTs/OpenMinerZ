#pragma once

// (mod-ai) Creature AI, projectiles, bosses and the retail enemy rules:
// GameplayService's private state for them, shared by ai.cpp, ai_navigation.cpp,
// ai_projectiles.cpp, ai_retail.cpp and ai_api.cpp. Included by internal.hpp.
//
// Everything here runs on the server half only (the host, or an offline
// world); clients get presentation through the replication messages listed at
// the top of ai.cpp. A creature without any AI field in its definition keeps
// the original built-in brains of entities.cpp untouched.

#include "openminerz/modding/gameplay.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::modding::gameplay
{

// How a creature moves.
enum class MoveMode : std::uint8_t
{
    ground, // walks, jumps `jump` blocks, drops `fall` blocks (climbs walls with `climb`)
    fly,    // 3D steering through the air; gravity off
    swim,   // 3D in water only; stranded out of it
    amphibious // walks on land and swims in water
};

enum class AttackKind : std::uint8_t
{
    melee,
    ranged
};

// An attack from a creature definition's `attacks` table. The damage moment
// is `windup` seconds after the attack starts (when its animation begins on
// every client), so the clip's strike frame lines up with the hit.
struct AttackDef final
{
    std::string name;
    AttackKind kind{AttackKind::melee};
    float reach{1.8F};      // melee: hit distance; ranged: the farthest it fires
    float min_range{};      // ranged: no shot closer than this
    float arc{100.0F};      // melee: degrees of the cone in front the target must be in
    float damage{10.0F};    // to players, in player health (100 = full)
    float creature_damage{-1.0F}; // to creatures (< 0: damage / 10)
    float cooldown{1.0F};
    float windup{0.35F};
    float recovery{0.25F};  // after the damage moment, before it moves again
    std::string animation{"attack"};
    std::string projectile; // ranged: a projectile key
    float spread{};         // ranged: degrees of random aim error
    int count{1};           // ranged: projectiles per shot
    bool lead{true};        // ranged: aims where a moving target will be
    float weight{1.0F};     // how often the built-in brain picks it among ready ones
};

// A scripted ability: `run(entity, target)` on the server when it is used.
struct AbilityDef final
{
    std::string name;
    float cooldown{5.0F};
    float range{16.0F};
    float windup{};
    std::string animation;
    Callback run;
};

struct ProjectileDef final
{
    std::string key;
    std::string mod;
    float speed{20.0F};
    float gravity{};         // blocks/s^2 downwards
    float lifetime{6.0F};
    float radius{0.15F};     // hit radius
    float damage{10.0F};     // player health
    float creature_damage{-1.0F};
    float homing{};          // degrees per second it turns toward its target
    std::string model;       // glTF ref (or a retail: ref) drawn along its flight
    std::string texture;
    float scale{1.0F};
    std::string fx;          // a trail effect (Enhanced presentation)
    std::string impact_fx;
    float explosion_radius{};
    float explosion_damage{};
    // Blocks inside the explosion are dug out when the session's rules let
    // monsters break blocks (retail's dragon-fireball rule: Endurance, Dragon
    // Endurance, Hard and Hardcore), never in spawn protection, never bedrock.
    bool block_damage{};
    // (fix) The mod opts in to breaking blocks whatever the difficulty: the
    // explosion digs, or without one the block struck breaks. Never inside
    // spawn protection; the host's server half alone decides it.
    bool breaks_blocks{};
    bool pierce{};           // flies on through creatures and players
    Callback on_hit;         // (projectile, hit) on the server
};

struct BossPhase final
{
    float below{0.5F};       // health fraction that starts it
    std::string name;
    std::string state;       // brain state to switch to
    std::string music;       // overrides the boss music while it lasts
    float speed{1.0F};       // movement multiplier from now on
    float damage{1.0F};      // damage multiplier from now on
};

struct BossDef final
{
    bool enabled{};
    std::string title;       // the bar's label (default: the creature's name)
    std::string color{"#c83c32"};
    std::string music;       // a music/ asset clients play while the bar shows
    float bar_range{64.0F};  // clients within this see the bar
    float arena{};           // > 0: leashed to its home within this radius
    std::vector<BossPhase> phases; // sorted, highest threshold first
};

struct BrainState final
{
    std::string name;
    Callback enter;   // (entity, info)
    Callback tick;    // (entity, info) -> next state name or nil
    Callback exit;    // (entity, info)
    Callback damaged; // (entity, info) with info.attacker, info.amount
    Callback heard;   // (entity, info) with info.noise
    Callback noticed; // (entity, info) with info.noticed
};

struct BrainDef final
{
    std::string key;
    std::string mod;
    std::string initial;
    std::vector<BrainState> states;
    [[nodiscard]] const BrainState *state(std::string_view name) const noexcept;
};

// The AI part of a creature definition. Present when the definition has any
// of: brain, team, hostile_to, senses, attacks, abilities, movement, boss.
struct CreatureAi final
{
    std::string team;                    // default from `behaviour`
    std::vector<std::string> hostile_to; // teams it attacks on sight ("players", "*")
    std::vector<std::string> friends;    // teams it never attacks
    float sight{16.0F};
    float fov{140.0F};                   // degrees
    float hearing{16.0F};
    float memory{10.0F};                 // seconds a lost target is remembered
    bool xray{};                         // sees through blocks
    MoveMode movement{MoveMode::ground};
    int jump{1};
    int fall{3};
    bool climb{};
    float swim_speed{-1.0F};             // < 0: run_speed
    float fly_height{5.0F};              // cruise height above the ground
    float think_rate{5.0F};              // thoughts per second near players
    float leash{};                       // > 0: returns home beyond it
    float flee_health{};                 // built-in brain: flees below this fraction
    float pack_flee{};                   // built-in brain: flees when the pack lost this much
    bool surround{true};                 // built-in brain: packs spread around a target
    std::vector<AttackDef> attacks;
    std::vector<AbilityDef> abilities;
    std::string brain;                   // brain key; "" = the built-in hunter
    BossDef boss;
    [[nodiscard]] const AttackDef *attack(std::string_view name) const noexcept;
    [[nodiscard]] const AbilityDef *ability(std::string_view name) const noexcept;
};

// What a creature knows about one possible target.
struct MemoryEntry final
{
    std::uint64_t player{}; // player serial
    std::uint64_t entity{};
    Vec3 last_position;
    double seen_at{-1.0e9};
    double heard_at{-1.0e9};
    double noticed_at{-1.0e9};
    float threat{};
    bool visible{};
    bool hostile{};
};

enum class GoalKind : std::uint8_t
{
    none,
    point,     // walk/fly/swim to a point
    chase,     // close in on the target
    keep,      // stay between `distance` and `distance2` of the target (archers)
    flee,      // away from the target or point
    wander,
    surround,  // the pack's slot around the target
    home
};

struct MoveGoal final
{
    GoalKind kind{GoalKind::none};
    std::uint64_t player{};
    std::uint64_t entity{};
    Vec3 point;
    float distance{1.5F};
    float distance2{};
    bool run{true};
    double until{};
};

// A creature's mind (Entity::mind).
struct EntityMind final
{
    bool active{};
    std::string state;
    std::string pending_state; // asked for while a callback ran
    double state_since{};
    bool state_entered{};
    std::uint64_t target_player{};
    std::uint64_t target_entity{};
    std::vector<MemoryEntry> memory;
    double think_at{};
    double last_think{};
    MoveGoal goal;
    // The attack or ability under way.
    int attack{-1};
    bool attack_is_ability{};
    float attack_left{};    // until the damage moment
    float recovery_left{};
    std::uint64_t attack_player{};
    std::uint64_t attack_entity{};
    Vec3 attack_point;
    std::map<std::string, double, std::less<>> ready_at; // attack/ability -> server_time
    std::uint64_t pack{};
    Vec3 home;
    int phase{-1};
    float speed_scale{1.0F};
    float damage_scale{1.0F};
    std::string team; // "" = the creature's
    // Navigation.
    std::vector<BlockPos> route;
    std::size_t route_index{};
    BlockPos route_goal{};
    double route_at{-1.0e9};
    float stuck{};
    float hover_phase{};
    bool leashed{};
    double heard_until{};
    Vec3 heard_position;
    bool urgent{}; // replicate this entity this tick (an attack started)
    std::uint32_t lod{1U};
    float move_accumulator{}; // far creatures move in fewer, longer steps
    std::size_t scripted_path{}; // entities.move_to on an AI creature: waypoints walked
};

// A cell as one integer key (x and z within +-2M, y within +-512).
[[nodiscard]] inline std::int64_t cell_key(int x, int y, int z) noexcept
{
    return (static_cast<std::int64_t>(x & 0x3FFFFF) << 32) | (static_cast<std::int64_t>(z & 0x3FFFFF) << 10) |
           static_cast<std::int64_t>((y + 512) & 0x3FF);
}

// A sound creatures may hear (gunshots, digging, explosions, scripts).
struct Noise final
{
    Vec3 position;
    float loudness{16.0F}; // blocks it carries at full hearing
    std::string kind;
    std::uint64_t player{}; // who made it (player serial), if a player
    std::uint64_t entity{};
    double at{};
};

struct Projectile final
{
    std::uint64_t id{};
    const ProjectileDef *def{}; // into the server registry (stable: std::map)
    std::string key;
    Vec3 position;
    Vec3 velocity;
    double born{};
    std::uint64_t owner_entity{};
    std::uint64_t owner_player{};
    std::string team;
    std::uint64_t target_player{};
    std::uint64_t target_entity{};
    float damage_scale{1.0F};
    bool removed{};
    float sent_timer{};
    std::vector<std::uint64_t> pierced;
};

struct Pack final
{
    std::uint64_t id{};
    std::vector<std::uint64_t> members;
    std::uint64_t leader{};
    float health_at_start{};
};

enum class Relation : std::uint8_t
{
    neutral,
    hostile,
    friendly
};

// A cached route: the same start and goal cells for the same body.
struct RouteCacheEntry final
{
    std::vector<BlockPos> route;
    double made_at{};
    bool reached{};
};

// A breadth-first distance field toward one target, shared by every chaser
// of that target with the same body (a pack hunting a player plans once).
struct FlowField final
{
    BlockPos origin;
    double made_at{};
    std::unordered_map<std::int64_t, std::uint16_t> cost; // packed cell -> steps
};

// Numbers for ai.stats() and the performance test.
struct AiStats final
{
    std::uint64_t thinks{};
    std::uint64_t deferred_thinks{};
    std::uint64_t path_searches{};
    std::uint64_t path_nodes{};
    std::uint64_t field_builds{};
    std::uint64_t cache_hits{};
    std::uint64_t line_checks{};
    double think_ms{};
    double step_ms{};
    std::uint64_t steps{};
    std::uint64_t refused_claims{}; // clients' hits on creatures the host did not believe
};

struct AiLimits final
{
    std::size_t thinks_per_step{96U};       // brain thoughts per 20 Hz step
    std::size_t path_nodes_per_step{12'000U}; // A* + field expansions per step
    std::size_t path_node_limit{2'048U};      // one A* search
    std::size_t field_node_limit{3'000U};     // one flow field
    std::size_t max_projectiles{512U};
    std::size_t memory_entries{12U};
    float interest_radius{112.0F};            // entities replicated to a player within it
};

// The server half's AI world (Impl::ai).
struct AiWorld final
{
    AiLimits limits;
    std::deque<Noise> noises;
    std::vector<Projectile> projectiles;
    std::uint64_t next_projectile_id{1U};
    std::map<std::uint64_t, Pack> packs;
    std::uint64_t next_pack_id{1U};
    // Relations between teams (both orders stored), on top of the defaults.
    std::map<std::pair<std::string, std::string>, Relation, std::less<>> relations;
    std::map<std::uint64_t, std::string> player_teams; // player serial -> team
    std::unordered_map<std::string, RouteCacheEntry> routes;
    std::unordered_map<std::string, FlowField> fields;
    double cache_swept_at{};
    AiStats stats;
    std::size_t thinks_left{};
    std::size_t nodes_left{};
    std::uint32_t pass{};
    std::uint32_t replicate_pass{};
    // Entities by 16-block column cell, rebuilt every step (indices into
    // Impl::entities, valid for that step).
    std::unordered_map<std::int64_t, std::vector<std::size_t>> grid;
    // Cells this step's planning asked about (cleared every step).
    std::unordered_map<std::int64_t, bool> solid_cache;
    float boss_timer{};
    // Bosses: what each client was last told.
    std::map<std::uint64_t, std::string> boss_sent;
    // Retail enemy rules (enemies.rules), replicated as "retail".
    RetailEnemyRules retail;
    bool retail_dirty{};
    // Water for swimmers: open cells at or below this height in these biomes.
    float sea_level{1.5F};
    std::vector<std::string> water_biomes{"lagoon"};
    std::unordered_map<std::int64_t, bool> water_columns; // packed x,z -> wet biome
};

// The client half's view of projectiles and boss bars.
struct AiClientState final
{
    std::vector<ClientProjectile> projectiles;
    std::vector<ClientBoss> bosses;
    std::vector<ClientImpact> impacts;
    RetailEnemyRules retail;
    bool retail_known{};
    bool retail_clear{}; // rules with clear_existing arrived (take_retail_clear)
};

} // namespace openminerz::modding::gameplay
