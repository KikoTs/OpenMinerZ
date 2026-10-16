// mod-ai: creature AI, projectiles, bosses and the session's retail enemy
// rules (src/modding/gameplay/ai*.cpp).
//
// Offline (a server VM and a client VM in one process, as single player and a
// listen host run) against a flat fake world: brains as state machines, sight
// with line of sight, hearing, memory, teams fighting each other, packs that
// surround and flee, melee with a windup a target can dodge, ranged and
// homing projectiles, explosions whose block damage the session gates,
// walking around walls, flying over them, swimming in water only, climbing,
// bosses with phases and a leash, the think budget, interest management.
// Then a host and a joined client over the production LoopbackSession: the
// client sees creatures, projectiles and the boss bar, follows the host's
// retail rules, and a creature's hit on the joined player goes to the host's
// authority; hostile clients' claims to have struck creatures are checked.
// And the guarantee for everything else: without a server VM every AI entry
// point is inert, the retail rules are the identity, nothing is sent.
// Finally the load: 50, 200 and 500 creatures, host ms per tick and bytes per
// second to a client.

#include "modding_test_harness.hpp"

#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/gameplay_net.hpp"
#include "openminerz/modding/net.hpp"
#include "openminerz/modding/ui.hpp"
#include "openminerz/net/loopback_session.hpp"
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/mod_channel.hpp"
#include "openminerz/net/mod_handshake.hpp"
#include "openminerz/net/protocol.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/spawner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
using namespace openminerz::net;
using modding::Handle;
using modding::HandleKind;
using modding::Side;
using modding::Value;
namespace gameplay = openminerz::modding::gameplay;
namespace ui = openminerz::modding::ui;

void expect(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error{std::string{message}};
}

// ------------------------------------------------------------------- the world

// A flat world (rock below y 1) with edits, this process's player, remote
// players, and records of every damage the service asked for.
class FakeGame final : public gameplay::GameplayBackend
{
  public:
    std::uint8_t local_gid{};
    std::string local_name{"Host"};
    modding::Vec3 position{0.5, 1.0, 0.5};
    float health{100.0F};
    std::vector<gameplay::PlayerSnapshot> remotes;
    std::map<gameplay::BlockPos, std::uint8_t> edits;
    struct Hurt final
    {
        std::uint8_t gid{};
        float amount{};
        gameplay::DamageCause cause{};
        double at{};
    };
    std::vector<Hurt> hurts;           // this process's player (local_damage)
    std::vector<Hurt> authority_hurts; // remote players, through the authority
    bool authority{};                  // answer authority_damage (an OpenMinerZ-only host)
    bool block_damage{};
    std::map<std::uint8_t, bool> may_act;
    bool spawn_protected{}; // every cell inside the host's spawn protection
    std::function<std::string(int, int)> biome_of;
    double clock{};
    gameplay::GameplayService *service{};

    [[nodiscard]] std::int32_t world_seed() const override
    {
        return 4242;
    }
    [[nodiscard]] bool modern_rules() const override
    {
        return false;
    }
    [[nodiscard]] std::optional<std::uint8_t> local_player() const override
    {
        return local_gid;
    }
    void players(std::vector<gameplay::PlayerSnapshot> &out) const override
    {
        out.clear();
        out.push_back({local_gid, local_name, true, health > 0.0F, position, 0.0F, health});
        out.insert(out.end(), remotes.begin(), remotes.end());
    }
    void local_set_health(float value) override
    {
        health = value;
    }
    void local_damage(float amount, modding::Vec3 source, gameplay::DamageCause cause) override
    {
        if (service != nullptr && service->running())
            amount = service->local_player_damaging(amount, cause, source);
        hurts.push_back({local_gid, amount, cause, clock});
    }
    void local_teleport(modding::Vec3 feet) override
    {
        position = feet;
    }
    std::uint16_t local_give(gameplay::StackView, bool) override
    {
        return 0U;
    }
    std::uint32_t local_take(std::uint16_t, std::uint32_t, std::uint32_t) override
    {
        return 0U;
    }
    [[nodiscard]] std::uint32_t local_count(std::uint16_t, std::uint32_t) const override
    {
        return 0U;
    }
    void local_clear_inventory() override
    {
    }
    [[nodiscard]] std::optional<gameplay::StackView> local_held() const override
    {
        return std::nullopt;
    }
    void local_slots(std::vector<std::pair<std::uint16_t, gameplay::StackView>> &out) const override
    {
        out.clear();
    }
    bool local_tag_slot(std::uint16_t, std::uint16_t, std::uint32_t) override
    {
        return false;
    }
    void local_set_speed(float) override
    {
    }
    [[nodiscard]] std::optional<std::uint8_t> block(gameplay::BlockPos cell) const override
    {
        if (cell.y < -64 || cell.y > 63)
            return std::nullopt;
        const auto found = edits.find(cell);
        if (found != edits.end())
            return found->second;
        return std::uint8_t{cell.y < -8 ? 14U : cell.y < 1 ? 6U : 0U}; // bedrock deep down
    }
    bool set_block(gameplay::BlockPos cell, std::uint8_t value) override
    {
        edits[cell] = value;
        return true;
    }
    [[nodiscard]] int biome(int, int) const override
    {
        return 0;
    }
    [[nodiscard]] std::string biome_key(int x, int z) const override
    {
        return biome_of ? biome_of(x, z) : std::string{};
    }
    [[nodiscard]] std::optional<std::array<float, 2>> light(gameplay::BlockPos) const override
    {
        return std::array<float, 2>{1.0F, 0.0F};
    }
    bool set_crate_slot(gameplay::BlockPos, std::size_t, std::optional<gameplay::StackView>) override
    {
        return false;
    }
    bool spawn_pickup(modding::Vec3, gameplay::StackView) override
    {
        return true;
    }
    [[nodiscard]] double day() const override
    {
        return 0.5;
    }
    void set_time_of_day(float) override
    {
    }
    bool spawn_enemy(int, modding::Vec3) override
    {
        return false;
    }
    void chat(std::optional<std::uint8_t>, std::string_view) override
    {
    }
    [[nodiscard]] std::filesystem::path sidecar_directory() const override
    {
        return {};
    }
    bool authority_damage(std::uint8_t gid, float amount, gameplay::DamageCause cause, modding::Vec3) override
    {
        if (!authority)
            return false;
        authority_hurts.push_back({gid, amount, cause, clock});
        return true;
    }
    [[nodiscard]] bool creature_block_damage_allowed(gameplay::BlockPos) const override
    {
        return block_damage && !spawn_protected;
    }
    [[nodiscard]] bool projectile_block_breaking_allowed(gameplay::BlockPos) const override
    {
        return !spawn_protected;
    }
    [[nodiscard]] bool player_may_act(std::uint8_t gid) const override
    {
        const auto found = may_act.find(gid);
        return found == may_act.end() || found->second;
    }

    void wall(int x0, int x1, int y0, int y1, int z0, int z1, std::uint8_t block = 6U)
    {
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    edits[{x, y, z}] = block;
    }
};

// ------------------------------------------------------------------ the mods

constexpr std::string_view state_module = "return {}";

constexpr std::string_view definitions = R"(
local state = require("shared/state")
projectiles.define("arrow", { speed = 26, gravity = 9.8, damage = 12, radius = 0.12, model = "models/arrow" })
projectiles.define("bomb", { speed = 12, gravity = 9.8, damage = 5, explosion = { radius = 3, damage = 30, blocks = true } })
projectiles.define("seeker", { speed = 9, homing = 240, damage = 8, lifetime = 10 })
brains.define("hunter", {
    initial = "roam",
    states = {
        roam = {
            enter = function(e, info) ai.wander(e, { radius = 4 }) end,
            tick = function(e, info) if info.target then return "hunt" end end,
        },
        hunt = {
            tick = function(e, info)
                if not info.target then return "roam" end
                state.last_info = info
                ai.hunt(e)
            end,
            damaged = function(e, info) state.damaged = (state.damaged or 0) + info.amount end,
        },
    },
})
creatures.define("ghoul", {
    model = "retail:zombie", health = 20, speed = 2, run_speed = 4.5, size = { 0.6, 1.8 },
    team = "undead", hostile_to = { "players", "beasts" }, brain = "hunter",
    senses = { sight = 20, fov = 120, hearing = 24, memory = 6 },
    attacks = { bite = { reach = 1.6, damage = 15, cooldown = 1, windup = 0.4 } },
})
creatures.define("runner", {
    health = 20, speed = 2, run_speed = 4.5, team = "undead", hostile_to = { "players" },
    senses = { sight = 40, fov = 360 }, attacks = { bite = { reach = 1.6, damage = 15, cooldown = 1, windup = 0.4 } },
})
creatures.define("coward", {
    health = 10, run_speed = 5, team = "undead", hostile_to = { "players" }, pack_flee = 0.3,
    senses = { sight = 30, fov = 360 }, attacks = { bite = { reach = 1.5, damage = 5 } },
})
creatures.define("archer", {
    health = 8, team = "undead", hostile_to = { "players" }, senses = { sight = 32, fov = 360 },
    attacks = { shoot = { projectile = "arrow", range = 26, min_range = 3, cooldown = 1.5, windup = 0.5 } },
})
creatures.define("wolf", {
    health = 10, speed = 3, run_speed = 6, team = "beasts", hostile_to = { "undead" }, senses = { sight = 24, fov = 360 },
    attacks = { bite = { reach = 1.5, damage = 8, creature_damage = 3, cooldown = 0.8, windup = 0.2 } },
})
creatures.define("bat", {
    health = 4, speed = 3, run_speed = 6, size = { 0.5, 0.5 }, movement = { mode = "fly", height = 6 },
    hostile_to = { "players" }, senses = { sight = 40, fov = 360, xray = true },
    attacks = { bite = { reach = 1.5, damage = 4 } },
})
creatures.define("eel", {
    health = 6, speed = 2, run_speed = 3, size = { 0.6, 0.4 }, movement = "swim",
    hostile_to = { "players" }, senses = { sight = 16, fov = 360 }, attacks = { bite = { reach = 1.5, damage = 6 } },
})
creatures.define("spider", {
    health = 6, speed = 3, run_speed = 4, movement = { mode = "ground", climb = true, jump = 0 },
    hostile_to = { "players" }, senses = { sight = 40, fov = 360, xray = true }, attacks = { bite = { reach = 1.5, damage = 2 } },
})
creatures.define("lich", {
    health = 100, speed = 2, run_speed = 3, hostile_to = { "players" }, senses = { sight = 32, fov = 360 },
    boss = { title = "The Lich", color = "#8040c0", music = "music/boss", arena = 30,
             phases = { { below = 0.5, name = "Enraged", speed = 1.5, damage = 2 } } },
    attacks = { bolt = { projectile = "seeker", range = 30, min_range = 2, cooldown = 2, windup = 0.5 } },
})
)";

constexpr std::string_view server_events = R"(
local state = require("shared/state")
state.states = {}
state.targets = {}
state.phases = {}
state.hits = {}
state.died = {}
events.on("entity.state_changed", function(e) table.insert(state.states, e.state) end)
events.on("entity.target_changed", function(e) table.insert(state.targets, e.reason) end)
events.on("entity.phase_changed", function(e) table.insert(state.phases, e.name) end)
events.on("projectile.hit", function(e) table.insert(state.hits, e.target ~= nil and "target" or (e.block or "air")) end)
events.on("entity.died", function(e) table.insert(state.died, e.kind) end)
)";

modding::ModSource ai_mod(std::string_view extra_server = {})
{
    modding::ModSource source;
    source.id = "ai";
    source.scope = ModScope::server;
    source.files["shared/state.luau"] = std::string{state_module};
    source.files["shared/defs.luau"] = std::string{definitions};
    source.files["server/events.luau"] = std::string{server_events};
    if (!extra_server.empty())
        source.files["server/extra.luau"] = std::string{extra_server};
    return source;
}

// --------------------------------------------------- one process, offline

struct Offline final
{
    modding::ModRuntime runtime{modding::testing::test_limits()};
    FakeGame game;
    std::unique_ptr<gameplay::GameplayService> gameplay;
    double now{};

    explicit Offline(std::string_view extra_server = {})
    {
        gameplay = modding::register_gameplay_api(runtime.registry(), runtime, game);
        game.service = gameplay.get();
        runtime.set_player_key([this](Handle player) { return gameplay->player_key(player); });
        for (const Side side : {Side::server, Side::client})
        {
            std::vector<modding::ModSource> mods{ai_mod(extra_server)};
            expect(runtime.start(side, std::move(mods), {}), "the AI mod's VM starts");
        }
        gameplay->start_server();
        gameplay->start_client();
        for (const modding::ModStatus &status : runtime.status())
            expect(status.state == modding::ModState::running,
                   std::format("{} runs: {}", status.id, status.error));
    }
    ~Offline()
    {
        runtime.stop_all();
    }
    Offline(const Offline &) = delete;
    Offline &operator=(const Offline &) = delete;

    void step(double seconds = 1.0 / 60.0)
    {
        now += seconds;
        game.clock = now;
        runtime.tick(seconds);
        gameplay->tick(static_cast<float>(seconds));
        runtime.frame(seconds);
    }
    void run_for(double seconds, const std::function<bool()> &until = {})
    {
        const int steps = static_cast<int>(std::ceil(seconds * 60.0));
        for (int index = 0; index < steps; ++index)
        {
            step();
            if (until && until())
                return;
        }
    }
    Value probe(std::string_view expression)
    {
        const modding::RunResult result =
            runtime.run(Side::server, "ai", "probe", std::format("local state = require('shared/state') return {}", expression));
        expect(result.ok, std::format("probe '{}': {}", expression, result.error));
        return result.value;
    }
    void run(std::string_view source)
    {
        const modding::RunResult result = runtime.run(Side::server, "ai", "chunk", std::format("local state = require('shared/state') {}", source));
        expect(result.ok, std::format("chunk failed: {}", result.error));
    }
    Handle spawn(std::string_view kind, modding::Vec3 at, std::string_view options = "{}")
    {
        const Value value = probe(std::format("entities.spawn('{}', vector.create({}, {}, {}), {})", kind, at.x, at.y,
                                              at.z, options));
        expect(value.type() == modding::ValueType::handle, std::format("{} spawned", kind));
        return value.as_handle();
    }
    [[nodiscard]] Value info(Handle entity)
    {
        return probe(std::format("(function() for _, e in entities.list() do if e.id == {} then return entities.get(e) end "
                                 "end return nil end)()",
                                 entity.id));
    }
    [[nodiscard]] std::optional<modding::Vec3> where(Handle entity)
    {
        const Value value = info(entity);
        if (value.type() != modding::ValueType::map)
            return std::nullopt;
        const Value *position = value.field("position");
        return position != nullptr ? std::optional{position->as_vec3()} : std::nullopt;
    }
    [[nodiscard]] std::string field_text(Handle entity, std::string_view key)
    {
        const Value value = info(entity);
        const Value *field = value.type() == modding::ValueType::map ? value.field(key) : nullptr;
        return field != nullptr && field->type() == modding::ValueType::string ? field->as_string() : std::string{};
    }
    [[nodiscard]] std::size_t count(std::string_view list)
    {
        const Value value = probe(std::format("#{}", list));
        return static_cast<std::size_t>(value.as_integer());
    }
    [[nodiscard]] bool contains(std::string_view list, std::string_view text)
    {
        return probe(std::format("table.find({}, '{}') ~= nil", list, text)) == Value{true};
    }
};

[[nodiscard]] double flat_distance(modding::Vec3 a, modding::Vec3 b)
{
    return std::hypot(a.x - b.x, a.z - b.z);
}

// Facing -X (yaw = -pi/2): toward a player at the origin from +X.
constexpr std::string_view face_west = "{ yaw = -1.5708 }";

// -------------------------------------------------------------------- tests

void retail_rules_apply_to_families_regions_biomes_and_tiers()
{
    Offline world;
    world.game.biome_of = [](int x, int) { return x > 1000 ? std::string{"test:ashlands"} : std::string{}; };
    world.run(R"(
        enemies.rules({
            zombie = false, dragon = false, hell = false,
            distance = { scale = 0.12, max = 3400 },
            regions = {
                { biome = "test:ashlands", enabled = false },
                { min = vector.create(-50, 0, -50), max = vector.create(50, 0, 50), zombie = true, tier = 100 },
            },
        })
    )");
    gameplay::GameplayService &service = *world.gameplay;
    using gameplay::RetailEnemyClass;
    expect(service.retail_rules_active(), "the rules are in effect");
    expect(service.retail_enemy_allowed(RetailEnemyClass::zombie, {0.0, 1.0, 0.0}),
           "a region switches zombies back on inside its box");
    expect(!service.retail_enemy_allowed(RetailEnemyClass::zombie, {200.0, 1.0, 0.0}), "zombies are off elsewhere");
    expect(service.retail_enemy_allowed(RetailEnemyClass::skeleton, {200.0, 1.0, 0.0}), "skeletons stay");
    expect(!service.retail_enemy_allowed(RetailEnemyClass::skeleton, {1200.0, 1.0, 0.0}),
           "a mod biome's region switches everything off");
    expect(!service.retail_enemy_allowed(RetailEnemyClass::dragon, {0.0, 30.0, 0.0}), "dragons are off");
    expect(std::abs(service.retail_tier_distance(25000.0F, {5000.0, 1.0, 0.0}) - 3000.0F) < 0.01F,
           "a 25,000-block distance maps to retail's 3,000 tier");
    expect(std::abs(service.retail_tier_distance(25000.0F, {10.0, 1.0, 0.0}) - 100.0F) < 0.01F,
           "a region's own tier distance wins");
    const Value got = world.probe("enemies.get_rules()");
    expect(got.field("families") != nullptr && got.field("families")->field("zombie") != nullptr &&
               *got.field("families")->field("zombie") == Value{false},
           "enemies.get_rules reports the families");
    expect(world.probe("enemies.allowed('skeleton', vector.create(1200, 1, 0))") == Value{false},
           "enemies.allowed answers scripts");
    world.step();
    expect(!service.take_retail_clear(), "rules without clear_existing leave the enemies about alone");
    world.run("enemies.reset()");
    expect(!service.retail_rules_active() &&
               service.retail_enemy_allowed(RetailEnemyClass::zombie, {200.0, 1.0, 0.0}) &&
               service.retail_tier_distance(25000.0F, {5000.0, 1.0, 0.0}) == 25000.0F,
           "enemies.reset restores retail exactly");
    // clear_existing: the game is told once (it sends those enemies away).
    world.run("enemies.rules({ zombie = false, clear_existing = true })");
    expect(world.probe("enemies.get_rules().clear_existing") == Value{true}, "enemies.get_rules reports clear_existing");
    world.step();
    expect(service.take_retail_clear() && !service.take_retail_clear(),
           "rules with clear_existing reach this peer's game once");
    expect(service.retail_spawn_allowed(RetailEnemyClass::skeleton, {0.0, 1.0, 0.0}) &&
               !service.retail_spawn_allowed(RetailEnemyClass::zombie, {0.0, 1.0, 0.0}),
           "offline, this peer's own spawns follow the rules at once");
    // Spawner blocks: one whose waves could only bring switched-off families
    // stays silent; one with any family left runs (its other members are
    // counted as defeated by the game).
    for (const world::BlockType block : {world::BlockType::enemy_spawn_off, world::BlockType::alien_spawn_off})
    {
        const world::SpawnerDefinition *definition = world::find_spawner_definition(block);
        expect(definition != nullptr, "a retail spawner definition");
        const std::vector<std::uint8_t> types = world::spawner_wave_enemy_types(*definition, 10);
        std::vector<std::string> families;
        for (const std::uint8_t wire : types)
        {
            const std::string name = gameplay::retail_enemy_class_name(
                gameplay::retail_enemy_class_of(world::enemy_type_from_wire(wire, true).value_or(0)));
            if (std::ranges::find(families, name) == families.end())
                families.push_back(name);
        }
        expect(!types.empty() && !families.empty(), "a spawner's waves bring some families");
        std::string all_off;
        for (const std::string &family : families)
            all_off += family + " = false, ";
        world.run(std::format("enemies.rules({{ {} }})", all_off));
        expect(service.retail_spawner_silent(types, {0.5, 11.0, 0.5}),
               "every family its waves could bring is off: the spawner stays silent");
        world.run(std::format("enemies.rules({{ {} = false }})", families.front() == "felguard" ? "dragon" : "felguard"));
        expect(!service.retail_spawner_silent(types, {0.5, 11.0, 0.5}), "with its families on it runs");
        if (families.size() > 1U)
        {
            world.run(std::format("enemies.rules({{ {} = false }})", families.front()));
            expect(!service.retail_spawner_silent(types, {0.5, 11.0, 0.5}),
                   "a family left: it runs (the switched-off members are counted as defeated)");
        }
    }
    world.run("enemies.reset()");
    expect(!service.retail_spawner_silent(world::spawner_wave_enemy_types(
                                              *world::find_spawner_definition(world::BlockType::enemy_spawn_off), 10),
                                          {0.5, 11.0, 0.5}),
           "without rules no spawner is silent");
    for (const std::string_view bad : {"enemies.rules({ zombie = 'no' })", "enemies.rules({ clear_existing = 1 })"})
    {
        bool refused{};
        try
        {
            world.run(bad);
        }
        catch (const std::exception &)
        {
            refused = true;
        }
        expect(refused, "family switches and clear_existing must be booleans");
    }
}

void without_a_server_vm_everything_is_inert()
{
    // A gameplay service no server mod ever started: what a session without
    // server mods has (the app does not even create one).
    modding::testing::Harness harness;
    FakeGame game;
    std::unique_ptr<gameplay::GameplayService> service =
        modding::register_gameplay_api(harness.registry(), harness.host(), game);
    struct Capture final : gameplay::GameplaySync
    {
        std::size_t sent{};
        void to_player(std::uint8_t, const Value &, bool) override
        {
            ++sent;
        }
        void to_all(const Value &, bool) override
        {
            ++sent;
        }
        void to_server(const Value &) override
        {
            ++sent;
        }
    } capture;
    service->set_sync(&capture);
    using gameplay::RetailEnemyClass;
    for (int family = 0; family < static_cast<int>(RetailEnemyClass::count); ++family)
        expect(service->retail_enemy_allowed(static_cast<RetailEnemyClass>(family), {12.0, -50.0, 99.0}),
               "every retail family is allowed");
    for (const float distance : {0.0F, 0.3F, 299.99F, 1234.5678F, 3000.0F, 25'000.0F, 1.0e6F})
    {
        const float mapped = service->retail_tier_distance(distance, {distance, 1.0, 0.0});
        expect(std::memcmp(&mapped, &distance, sizeof(float)) == 0, "the tier distance is bit for bit the input");
    }
    expect(!service->retail_rules_active(), "no rules");
    service->noise({0.0, 1.0, 0.0}, 32.0F, "gunshot", std::uint8_t{0U});
    for (int step = 0; step < 600; ++step)
        service->tick(1.0F / 60.0F);
    expect(service->local_enemy_spawning(0, {1.0, 1.0, 1.0}), "enemy.spawning lets everything through");
    expect(!service->local_attack({0.0, 1.6, 0.0}, {1.0, 0.0, 0.0}, 5.0F, 1.0F, 0U).has_value(),
           "no creature to strike");
    expect(service->client_projectiles().empty() && service->client_bosses().empty() &&
               service->take_client_impacts().empty(),
           "nothing to draw");
    expect(capture.sent == 0U, "not one message: a session without server mods sends nothing new");
    // The inactive rules themselves are the identity.
    const gameplay::RetailEnemyRules rules;
    expect(rules.allows(RetailEnemyClass::zombie, {}, "anything") && rules.tier_distance(777.0F, {}, "") == 777.0F,
           "inactive rules change nothing");
    // A client-scope mod cannot reach the rules or the AI at all.
    const modding::RunResult attempt = harness.run("enemies.rules({ zombie = false })", Side::client,
                                                   modding::testing::client_mod_capabilities(), "visual");
    expect(!attempt.ok && !service->retail_rules_active(), "a client mod cannot set retail rules");
}

void a_brain_runs_its_states_and_sees_and_bites()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    const Handle ghoul = world.spawn("ghoul", {10.5, 1.0, 0.5}, face_west);
    world.run_for(2.0, [&] { return world.contains("state.states", "hunt"); });
    expect(world.contains("state.states", "hunt"), "the brain went from roam to hunt");
    expect(world.contains("state.targets", "noticed"), "entity.target_changed: it noticed the player");
    world.run_for(0.5);
    const Value info = world.probe("state.last_info");
    expect(info.field("target") != nullptr && info.field("target")->type() == modding::ValueType::handle &&
               info.field("sees_target") != nullptr && *info.field("sees_target") == Value{true},
           "the hunt state's info names a target it sees");
    world.run_for(8.0, [&] { return !world.game.hurts.empty(); });
    expect(!world.game.hurts.empty(), "the ghoul reached the player and bit");
    expect(world.game.hurts.front().cause == gameplay::DamageCause::creature &&
               std::abs(world.game.hurts.front().amount - 15.0F) < 0.01F,
           "a creature's bite, of its attack's damage, through the player's own damage path");
    const auto at = world.where(ghoul);
    expect(at.has_value() && flat_distance(*at, world.game.position) < 2.2, "it stands at biting range");
}

void walls_block_sight_and_noise_gives_the_player_away()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    world.game.wall(4, 4, 1, 4, -6, 6);
    static_cast<void>(world.spawn("ghoul", {10.5, 1.0, 0.5}, face_west));
    world.run_for(3.0);
    expect(!world.contains("state.states", "hunt"), "behind a wall it does not see the player");
    world.gameplay->noise({0.5, 1.0, 0.5}, 32.0F, "gunshot", std::uint8_t{0U});
    world.run_for(2.0, [&] { return world.contains("state.states", "hunt"); });
    expect(world.contains("state.states", "hunt"), "a gunshot gave the player away");
}

void teams_fight_each_other()
{
    Offline world;
    world.game.position = {0.5, 1.0, 60.5}; // near enough to keep them simulated, out of their sight
    static_cast<void>(world.spawn("wolf", {0.5, 1.0, 0.5}));
    static_cast<void>(world.spawn("runner", {6.5, 1.0, 0.5}, "{ team = 'undead' }"));
    world.run(R"(state.wolf_hits = 0
        events.on("entity.damaged", function(e) if e.attacker ~= nil then state.wolf_hits += 1 end end))");
    world.run_for(40.0, [&] { return world.count("state.died") > 0U; });
    expect(world.probe("state.wolf_hits").as_integer() > 0, "the creatures struck each other");
    expect(world.count("state.died") > 0U, "one team's creature killed the other's");
    expect(world.game.hurts.empty(), "the player, out of sight, was left alone");
}

void a_pack_surrounds_its_prey_and_flees_when_hurt()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    const Handle leader = world.spawn("runner", {12.5, 1.0, 0.5});
    world.run(std::format("for _, e in entities.list() do if e.id == {} then state.leader = e end end", leader.id));
    std::vector<Handle> pack{leader};
    for (const double z : {-3.0, 3.0})
        pack.push_back(world.spawn("runner", {12.5, 1.0, 0.5 + z}, "{ pack = state.leader }"));
    // ai.join_pack does the same afterwards.
    pack.push_back(world.spawn("runner", {12.5, 1.0, 6.5}));
    world.run(std::format("for _, e in entities.list() do if e.id == {} then ai.join_pack(e, state.leader) end end",
                          pack.back().id));
    expect(world.probe("#ai.pack(state.leader)") == Value{4}, "four runners form one pack");
    // Stop them biting so they settle into their ring.
    world.run("events.on('entity.attacking', function() return false end)");
    world.run_for(12.0);
    std::vector<double> angles;
    for (const Handle &member : pack)
        if (const auto at = world.where(member))
        {
            expect(flat_distance(*at, world.game.position) < 3.5, "each member closed in");
            angles.push_back(std::atan2(at->z - world.game.position.z, at->x - world.game.position.x));
        }
    std::ranges::sort(angles);
    double widest{};
    for (std::size_t index = 0U; index < angles.size(); ++index)
    {
        const double next = index + 1U < angles.size() ? angles[index + 1U] : angles.front() + 2.0 * std::numbers::pi;
        widest = std::max(widest, next - angles[index]);
    }
    std::string seen;
    for (const double angle : angles)
        seen += std::format(" {:.0f}", angle * 180.0 / std::numbers::pi);
    expect(angles.size() == 4U && widest < 2.6,
           std::format("the pack spread around the player instead of queueing on one side (angles{}, widest gap {:.0f} deg)",
                       seen, widest * 180.0 / std::numbers::pi));

    // The cowards: a pack that has lost a third of its health runs.
    Offline cowards;
    cowards.game.position = {0.5, 1.0, 0.5};
    const Handle first = cowards.spawn("coward", {8.5, 1.0, 0.5});
    cowards.run(std::format("for _, e in entities.list() do if e.id == {} then state.first = e end end", first.id));
    const Handle second = cowards.spawn("coward", {8.5, 1.0, 2.5}, "{ pack = state.first }");
    cowards.spawn("coward", {8.5, 1.0, -1.5}, "{ pack = state.first }");
    cowards.run_for(1.0);
    cowards.run(std::format("for _, e in entities.list() do if e.id == {} then entities.damage(e, 9) end end", second.id));
    cowards.run(std::format("for _, e in entities.list() do if e.id ~= {} then entities.damage(e, 3) end end", second.id));
    cowards.run_for(1.5, [&] { return cowards.contains("state.states", "flee"); });
    expect(cowards.contains("state.states", "flee"), "the hurt pack fled");
}

void a_melee_strike_lands_after_its_windup_and_can_be_dodged()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    const Handle runner = world.spawn("runner", {2.5, 1.0, 0.5});
    // Wait for the windup to begin.
    double started{-1.0};
    world.run_for(4.0, [&] {
        if (world.field_text(runner, "animation") == "attack")
        {
            started = world.now;
            return true;
        }
        return false;
    });
    expect(started >= 0.0, "the attack's animation started");
    expect(world.game.hurts.empty(), "no damage at the start of the windup");
    world.run_for(1.0, [&] { return !world.game.hurts.empty(); });
    expect(!world.game.hurts.empty() && world.game.hurts.front().at - started >= 0.38,
           "the bite landed a windup (0.4 s) after its animation began");

    // Dodge: step out of reach as the next windup starts.
    Offline dodge;
    dodge.game.position = {0.5, 1.0, 0.5};
    const Handle other = dodge.spawn("runner", {2.5, 1.0, 0.5});
    dodge.run_for(4.0, [&] { return dodge.field_text(other, "animation") == "attack"; });
    dodge.game.position = {-8.5, 1.0, 0.5};
    dodge.run_for(0.45);
    expect(dodge.game.hurts.empty(), "a player who stepped out of reach during the windup was not hit");
}

void archers_shoot_arcing_arrows_that_the_host_resolves()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    static_cast<void>(world.spawn("archer", {16.5, 1.0, 0.5}));
    bool flying{};
    double peak{-100.0};
    world.run_for(8.0, [&] {
        const auto &shots = world.gameplay->client_projectiles();
        if (!shots.empty())
        {
            flying = true;
            peak = std::max(peak, shots.front().position.y);
        }
        return !world.game.hurts.empty();
    });
    expect(flying, "the client half saw an arrow in flight (prj+)");
    expect(peak > 2.6, "the arrow arced above the straight line for gravity");
    expect(!world.game.hurts.empty() && std::abs(world.game.hurts.front().amount - 12.0F) < 0.01F,
           "the host decided the arrow struck the player, for the projectile's damage");
    expect(world.contains("state.hits", "target"), "projectile.hit named the player");
    world.run_for(0.2);
    bool impact{};
    for (const gameplay::ClientImpact &hit : world.gameplay->take_client_impacts())
        impact = impact || hit.hit == "player";
    expect(impact, "the client was told where it landed (prj-)");

    // projectile.hit returning false: it flies on, harmless.
    Offline spared;
    spared.game.position = {0.5, 1.0, 0.5};
    spared.run("events.on('projectile.hit', function(e) if e.target ~= nil then return false end end)");
    static_cast<void>(spared.spawn("archer", {16.5, 1.0, 0.5}));
    spared.run_for(6.0);
    expect(spared.game.hurts.empty(), "a cancelled hit does no harm");
}

void homing_bolts_follow_their_target()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    world.run(R"(state.bolt = projectiles.launch("seeker", vector.create(10, 4, 10), nil, { velocity = vector.create(0, 0, 9) }))");
    // Launched away from the player: a target makes it turn.
    world.run(R"(for _, p in players.list() do state.me = p end
        state.bolt = projectiles.launch("seeker", vector.create(10, 4, 10), state.me))");
    world.run_for(8.0, [&] { return !world.game.hurts.empty(); });
    expect(!world.game.hurts.empty() && std::abs(world.game.hurts.front().amount - 8.0F) < 0.01F,
           "the homing bolt found the player");
}

void explosions_dig_only_where_the_rules_allow()
{
    for (const bool allowed : {false, true})
    {
        Offline world;
        world.game.position = {40.5, 1.0, 0.5};
        world.game.block_damage = allowed;
        world.run(R"(projectiles.launch("bomb", vector.create(0.5, 3, 0.5), nil, { velocity = vector.create(0, -12, 0) }))");
        world.run_for(2.0);
        const auto block = world.game.block({0, 0, 0});
        expect(block.has_value() && (*block == 0U) == allowed,
               allowed ? "where monsters may break blocks, the blast dug the ground"
                       : "without that rule (Normal/Easy, spawn protection) not one block changed");
        expect(world.game.block({0, -9, 0}) == std::optional<std::uint8_t>{std::uint8_t{14U}}, "bedrock never breaks");
    }
}

void projectiles_that_break_blocks_do_so_outside_spawn_protection()
{
    // breaks_blocks: the mod's choice, whatever the difficulty (block_damage
    // stays false: Normal), but never inside spawn protection.
    for (const bool protected_cells : {false, true})
    {
        Offline world(R"(
            projectiles.define("pick", { speed = 20, damage = 1, breaks_blocks = true })
            projectiles.define("charge", { speed = 12, gravity = 9.8, damage = 5,
                explosion = { radius = 3, damage = 10, breaks_blocks = true } })
        )");
        world.game.position = {40.5, 1.0, 0.5};
        world.game.spawn_protected = protected_cells;
        world.run(R"(projectiles.launch("pick", vector.create(0.5, 3, 0.5), nil, { velocity = vector.create(0, -20, 0) })
            projectiles.launch("charge", vector.create(10.5, 3, 0.5), nil, { velocity = vector.create(0, -12, 0) }))");
        world.run_for(2.0);
        const auto struck = world.game.block({0, 0, 0});
        const auto below = world.game.block({0, -1, 0});
        const auto blasted = world.game.block({10, 0, 0});
        if (!protected_cells)
            expect(struck == std::optional<std::uint8_t>{std::uint8_t{0U}} &&
                       below == std::optional<std::uint8_t>{std::uint8_t{6U}} &&
                       blasted == std::optional<std::uint8_t>{std::uint8_t{0U}},
                   "a projectile that breaks blocks broke the one it struck (only that one), and its "
                   "explosion dug, on Normal");
        else
            expect(struck == std::optional<std::uint8_t>{std::uint8_t{6U}} &&
                       blasted == std::optional<std::uint8_t>{std::uint8_t{6U}},
                   "inside spawn protection not one block changed");
    }
}

void walkers_path_around_walls_and_flyers_and_climbers_go_over()
{
    {
        Offline world;
        world.game.position = {0.5, 1.0, 0.5};
        world.game.wall(5, 5, 1, 3, -5, 5); // too high to jump, open at the ends
        const Handle runner = world.spawn("runner", {12.5, 1.0, 0.5});
        world.run("events.on('entity.attacking', function() return false end)");
        world.run_for(25.0, [&] {
            const auto at = world.where(runner);
            return at.has_value() && flat_distance(*at, world.game.position) < 2.5;
        });
        const auto at = world.where(runner);
        expect(at.has_value() && flat_distance(*at, world.game.position) < 2.5, "the walker found its way around the wall");
    }
    {
        Offline world;
        world.game.position = {0.5, 1.0, 0.5};
        world.game.wall(5, 5, 1, 6, -30, 30); // a long, tall wall
        const Handle bat = world.spawn("bat", {12.5, 2.0, 0.5});
        double highest{};
        world.run("events.on('entity.attacking', function() return false end)");
        world.run_for(20.0, [&] {
            const auto at = world.where(bat);
            if (at.has_value())
                highest = std::max(highest, at->y);
            return at.has_value() && at->x < 4.0;
        });
        const auto at = world.where(bat);
        expect(at.has_value() && at->x < 4.0 && highest > 7.0, "the flyer rose over the wall");
    }
    {
        Offline world;
        world.game.position = {0.5, 1.0, 0.5};
        world.game.wall(5, 5, 1, 3, -30, 30);
        const Handle spider = world.spawn("spider", {9.5, 1.0, 0.5});
        world.run("events.on('entity.attacking', function() return false end)");
        world.run_for(20.0, [&] {
            const auto at = world.where(spider);
            return at.has_value() && at->x < 4.0;
        });
        const auto at = world.where(spider);
        expect(at.has_value() && at->x < 4.0, "the climber went up and over the wall it cannot jump");
    }
}

void swimmers_stay_in_the_water()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    world.run(R"(ai.set_water({ level = 0.9, biomes = { "*" } }))");
    // A pool 12 x 12, four deep, next to the player.
    for (int y = -3; y <= 0; ++y)
        for (int z = -6; z <= 6; ++z)
            for (int x = 2; x <= 13; ++x)
                world.game.edits[{x, y, z}] = 0U;
    const Handle eel = world.spawn("eel", {10.5, -2.0, 0.5});
    double highest{-10.0};
    world.run_for(15.0, [&] {
        if (const auto at = world.where(eel))
            highest = std::max(highest, at->y);
        return false;
    });
    const auto at = world.where(eel);
    expect(at.has_value() && at->x < 6.0, "the swimmer came to the pool's edge nearest the player");
    expect(highest < 1.0, "it never left the water");
}

void a_boss_has_phases_a_bar_music_and_a_leash()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    const Handle lich = world.spawn("lich", {12.5, 1.0, 0.5});
    world.run_for(0.5);
    const auto &bosses = world.gameplay->client_bosses();
    expect(bosses.size() == 1U && bosses.front().title == "The Lich" && bosses.front().music == "music/boss" &&
               bosses.front().mod == "ai",
           "every client is told of the boss: title, music");
    world.run(std::format("for _, e in entities.list() do if e.id == {} then entities.damage(e, 60) end end", lich.id));
    world.run_for(0.5);
    expect(world.contains("state.phases", "Enraged"), "entity.phase_changed at half health");
    expect(world.gameplay->client_bosses().front().phase == "Enraged" &&
               std::abs(world.gameplay->client_bosses().front().fraction - 0.4F) < 0.02F,
           "the bar shows the phase and the health left");
    // Drag it far past its arena: it goes home and heals.
    world.run(std::format("for _, e in entities.list() do if e.id == {} then entities.teleport(e, vector.create(60, 1, 0.5)) end end",
                          lich.id));
    world.game.position = {200.5, 1.0, 0.5};
    world.game.remotes.push_back({9U, "Watcher", false, true, {60.5, 1.0, 20.5}, 0.0F, 100.0F});
    world.run_for(30.0, [&] { return world.contains("state.states", "idle") && world.contains("state.states", "return"); });
    const Value status = world.info(lich);
    expect(world.contains("state.states", "return"), "past its leash the boss turned home");
    expect(status.type() == modding::ValueType::map && status.field("health") != nullptr &&
               status.field("health")->as_number() > 99.0,
           "back home a boss that lost its fight heals");
    world.run(std::format("for _, e in entities.list() do if e.id == {} then entities.set_health(e, 0) end end", lich.id));
    world.run_for(0.5);
    expect(world.gameplay->client_bosses().empty(), "a dead boss's bar goes away");
}

void the_think_budget_and_distance_slow_thinking()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    world.run(R"(for i = 1, 300 do
        local angle = i * 0.618 * 6.283
        local r = 6 + (i % 100)
        entities.spawn("runner", vector.create(math.cos(angle) * r, 1, math.sin(angle) * r))
    end)");
    world.run_for(2.0);
    const Value stats = world.probe("ai.stats()");
    const auto thinks = stats.field("thinks")->as_integer();
    const auto deferred = stats.field("deferred")->as_integer();
    // 40 steps of 96 at most.
    expect(thinks <= 40 * 96 + 96, "never more thoughts per step than the budget");
    expect(thinks > 0 && deferred >= 0, "and they think");
    std::cout << "  think budget: " << world.gameplay->ai_report() << '\n';
}

// ----------------------------------------------------- host and joined client

constexpr std::string_view session_server = R"(
local state = require("shared/state")
enemies.rules({ zombie = false, skeleton = false, archer = false, dragon = false, clear_existing = true })
events.on("player.joined", function(e)
    if e.name == "Joiner" then state.joiner = e.player end
end)
)";

modding::ModSource session_mod()
{
    modding::ModSource source = ai_mod(session_server);
    source.id = "ai";
    return source;
}

ModDigest digest_of(std::uint8_t seed)
{
    ModDigest digest{};
    for (std::size_t index = 0U; index < digest.size(); ++index)
        digest[index] = static_cast<std::uint8_t>(seed + index * 7U + 1U);
    return digest;
}

ModManifest session_manifest()
{
    ModManifest manifest;
    manifest.protocol = ProtocolVersion::classic_v3;
    manifest.mods.push_back({"ai", "1", digest_of(3U), ModScope::server});
    return manifest;
}

HostSessionSettings settings_for(const ModManifest &manifest)
{
    HostSessionSettings settings;
    settings.server_name = "Bestiary";
    settings.protocol = ProtocolVersion::classic_v3;
    settings.maximum_players = 8;
    settings.visibility = HostVisibility::public_lobby;
    settings.mod_manifest = manifest;
    return settings;
}

AdmissionStatus admit(LoopbackSession &session, const HostSessionSettings &settings, PeerId peer,
                      const ModManifest &client_manifest)
{
    PeerAdmissionRequest request;
    request.peer = peer;
    request.properties = make_session_properties(settings, false, 1, 1, false);
    request.advertised_extension = true;
    const AdmissionDecision decision = session.connect(request);
    if (!decision.accepted())
        return decision.status;
    ExtensionMessage hello;
    hello.kind = ExtensionMessageKind::client_hello;
    hello.nonce = 0x7150U + peer;
    hello.manifest = client_manifest;
    static_cast<void>(session.submit_from_peer(peer, encode_extension_message(hello),
                                               MessageDeliveryPolicy::reliable_ordered, openminerz_control_channel));
    static_cast<void>(session.receive_for_peer(peer));
    return session.admission().status(peer);
}

struct NetObjects final : modding::NetBackend
{
    modding::ModNetServer *server_ptr{};
    modding::ModNetClient *client_ptr{};
    [[nodiscard]] modding::ModNetServer *server() noexcept override
    {
        return server_ptr;
    }
    [[nodiscard]] modding::ModNetClient *client() noexcept override
    {
        return client_ptr;
    }
};

struct Process final
{
    modding::ModRuntime runtime{modding::testing::test_limits()};
    FakeGame game;
    std::unique_ptr<gameplay::GameplayService> gameplay;
    NetObjects net;
    std::unique_ptr<modding::ModNetServer> server;
    std::unique_ptr<modding::ModNetClient> client;
    std::unique_ptr<gameplay::ModNetGameplaySync> link;
    double now{};

    Process()
    {
        gameplay = modding::register_gameplay_api(runtime.registry(), runtime, game);
        game.service = gameplay.get();
        modding::register_net_api(runtime.registry(), runtime, net);
        runtime.set_player_key([this](Handle player) { return gameplay->player_key(player); });
    }
    ~Process()
    {
        runtime.stop_all();
        if (link != nullptr)
            link->attach(nullptr, nullptr);
        gameplay->loopback().set_remote(nullptr);
    }
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    void attach(const ModManifest &manifest, bool authority)
    {
        if (authority)
            server = std::make_unique<modding::ModNetServer>(manifest);
        client = std::make_unique<modding::ModNetClient>(manifest);
        net.server_ptr = server.get();
        net.client_ptr = client.get();
        modding::connect_net_events(runtime, server.get(), client.get());
        link = std::make_unique<gameplay::ModNetGameplaySync>(*gameplay);
        link->attach(server.get(), server != nullptr ? nullptr : client.get());
        gameplay->loopback().set_remote(link.get());
        if (!authority)
            client->add_event_listener([this](const modding::ModNetEvent &event) {
                if (event.kind == modding::ModNetEventKind::welcome && gameplay->client_running())
                    gameplay->client_channel_ready();
            });
    }
    Handle host(const ModManifest &manifest)
    {
        attach(manifest, true);
        const Handle local = gameplay->admit_player(game.local_gid, game.local_name, true);
        expect(server->add_peer(modding::local_mod_peer, local, now), "the host's own player gets a channel");
        start({Side::server, Side::client});
        return local;
    }
    void join(const ModManifest &manifest)
    {
        attach(manifest, false);
        start({Side::client});
    }
    void start(std::initializer_list<Side> sides)
    {
        for (const Side side : sides)
        {
            std::vector<modding::ModSource> mods{session_mod()};
            expect(runtime.start(side, std::move(mods), {}), "the session mod's VM starts");
        }
        if (runtime.running(Side::server))
            gameplay->start_server();
        gameplay->start_client();
    }
    Handle admit_remote(PeerId peer, std::uint8_t gid, std::string_view name)
    {
        const Handle player = gameplay->admit_player(gid, name, false);
        expect(server->add_peer(peer, player, now), "the joiner gets a channel");
        gameplay->remote_player_joined(gid, name);
        return player;
    }
    void step(double seconds)
    {
        now += seconds;
        game.clock = now;
        if (runtime.running(Side::server))
            runtime.tick(seconds);
        gameplay->tick(static_cast<float>(seconds));
        runtime.frame(seconds);
        if (client != nullptr && runtime.running(Side::client))
            client->set_ready(now);
        if (server != nullptr)
            server->update(now);
    }
    Value probe(std::string_view expression)
    {
        const modding::RunResult result = runtime.run(
            Side::server, "ai", "probe", std::format("local state = require('shared/state') return {}", expression));
        expect(result.ok, std::format("probe '{}': {}", expression, result.error));
        return result.value;
    }
};

struct Session final
{
    LoopbackSession &session;
    Process &host;
    std::map<PeerId, Process *> clients;

    void pump()
    {
        for (int round = 0; round < 64; ++round)
        {
            bool moved = false;
            host.client->update(host.now);
            for (const auto &frame : host.client->take_sends())
            {
                host.server->receive(modding::local_mod_peer, frame, host.now);
                moved = true;
            }
            for (modding::ModNetSend &send : host.server->take_sends())
            {
                if (send.peer == modding::local_mod_peer)
                {
                    host.client->receive(send.frame, host.now);
                    moved = true;
                    continue;
                }
                static_cast<void>(session.submit_from_host(send.peer, send.frame, MessageDeliveryPolicy::reliable_ordered,
                                                           openminerz_control_channel));
            }
            for (auto &[peer, process] : clients)
            {
                process->client->update(process->now);
                for (const auto &frame : process->client->take_sends())
                    static_cast<void>(session.submit_from_peer(peer, frame, MessageDeliveryPolicy::reliable_ordered,
                                                               openminerz_control_channel));
            }
            while (auto datagram = session.receive_for_host())
            {
                host.server->receive(datagram->sender, datagram->payload, host.now);
                moved = true;
            }
            for (auto &[peer, process] : clients)
                while (auto datagram = session.receive_for_peer(peer))
                {
                    process->client->receive(datagram->payload, process->now);
                    moved = true;
                }
            if (!moved)
                return;
        }
    }
    void steps(int count = 1)
    {
        for (int step = 0; step < count; ++step)
        {
            host.step(1.0 / 20.0);
            for (auto &[peer, process] : clients)
                process->step(1.0 / 20.0);
            pump();
        }
    }
};

void host_and_joined_client_share_creatures_projectiles_bosses_and_rules()
{
    const ModManifest manifest = session_manifest();
    const HostSessionSettings settings = settings_for(manifest);
    LoopbackSession wire{settings, false, 1, 1, false};
    expect(requires_extension(manifest), "a session with a server mod admits only OpenMinerZ players");

    Process host;
    host.game.local_gid = 0U;
    host.game.position = {0.5, 1.0, -200.5}; // far off: the creatures are the joiner's
    host.game.authority = true;              // an OpenMinerZ-only host decides health
    static_cast<void>(host.host(manifest));
    Session session{wire, host};
    session.steps(2);

    // A stock peer (no extension) is refused: the rules never meet one.
    PeerAdmissionRequest stock;
    stock.peer = 300U;
    stock.properties = make_session_properties(settings, false, 1, 1, false);
    stock.advertised_extension = false;
    expect(!wire.connect(stock).accepted(), "a stock peer cannot join a session with server mods");

    expect(admit(wire, settings, 101U, manifest) == AdmissionStatus::admitted, "the OpenMinerZ peer is admitted");
    static_cast<void>(host.admit_remote(101U, 5U, "Joiner"));
    Process client;
    client.game.local_gid = 5U;
    client.game.local_name = "Joiner";
    client.game.position = {0.5, 1.0, 0.5};
    client.join(manifest);
    session.clients[101U] = &client;
    host.game.remotes.push_back({5U, "Joiner", false, true, {0.5, 1.0, 0.5}, 0.0F, 100.0F});

    // Until the host's rules arrive (with its full state) the joined client
    // spawns no retail enemy of its own, however long that takes - here 30 s
    // of nothing from the host - while what another peer spawned still shows.
    using gameplay::RetailEnemyClass;
    for (int step = 0; step < 30 * 20; ++step)
        client.step(1.0 / 20.0);
    expect(!client.gameplay->retail_rules_known() &&
               !client.gameplay->retail_spawn_allowed(RetailEnemyClass::felguard, {0.5, 1.0, 0.5}) &&
               !client.gameplay->retail_spawn_allowed(RetailEnemyClass::zombie, {0.5, 1.0, 0.5}),
           "before the host's rules arrive the joined client holds every spawn of its own");
    expect(client.gameplay->retail_enemy_allowed(RetailEnemyClass::zombie, {0.5, 1.0, 0.5}),
           "an enemy another peer announced meanwhile still appears (its sender spawned it by its rules)");
    session.steps(8);

    expect(client.gameplay->retail_rules_known(), "the rules came with the join's full state");
    expect(!client.gameplay->retail_spawn_allowed(RetailEnemyClass::zombie, {0.5, 1.0, 0.5}) &&
               !client.gameplay->retail_enemy_allowed(RetailEnemyClass::zombie, {0.5, 1.0, 0.5}) &&
               !client.gameplay->retail_enemy_allowed(RetailEnemyClass::dragon, {0.5, 30.0, 0.5}) &&
               client.gameplay->retail_spawn_allowed(RetailEnemyClass::felguard, {0.5, 1.0, 0.5}),
           "the joined client follows the host's retail rules for its own spawns");
    expect(client.gameplay->retail_rules_active(), "the client knows the rules are on");
    expect(client.gameplay->take_retail_clear() && !client.gameplay->take_retail_clear(),
           "clear_existing reached the joined client: it sends the switched-off enemies about away");

    // An archer and the boss near the joiner; a far ghoul it should not hear of.
    host.probe(R"((function()
        entities.spawn("archer", vector.create(14.5, 1, 0.5))
        entities.spawn("lich", vector.create(0.5, 1, 20.5))
        entities.spawn("ghoul", vector.create(400.5, 1, 400.5))
        return true end)())");
    bool saw_arrow{};
    for (int step = 0; step < 200 && host.game.authority_hurts.empty(); ++step)
    {
        session.steps(1);
        saw_arrow = saw_arrow || !client.gameplay->client_projectiles().empty();
    }
    const auto &entities = client.gameplay->client_entities();
    expect(std::ranges::any_of(entities, [](const gameplay::ClientEntity &entity) { return entity.kind == "ai:archer"; }) &&
               std::ranges::any_of(entities, [](const gameplay::ClientEntity &entity) { return entity.kind == "ai:lich"; }),
           "the joined client sees the creatures near it");
    expect(std::ranges::none_of(entities, [](const gameplay::ClientEntity &entity) { return entity.kind == "ai:ghoul"; }),
           "and not one 560 blocks away (interest management)");
    expect(saw_arrow, "the joined client saw the projectiles in flight");
    expect(client.gameplay->client_bosses().size() == 1U && client.gameplay->client_bosses().front().title == "The Lich",
           "the joined client has the boss bar");
    expect(!host.game.authority_hurts.empty() && host.game.authority_hurts.front().gid == 5U &&
               host.game.authority_hurts.front().cause == gameplay::DamageCause::creature,
           "a creature's hit on the joined player went to the host's authority, as a creature's");
    expect(client.game.hurts.empty(), "and the joined client did not lower its own health");
}

void hostile_clients_cannot_claim_creature_hits()
{
    Offline world;
    world.game.position = {0.5, 1.0, 0.5};
    world.game.remotes.push_back({5U, "Cheater", false, true, {0.5, 1.0, 4.5}, 0.0F, 100.0F});
    world.run("events.on('entity.attacking', function() return false end)");
    const Handle dummy = world.spawn("runner", {0.5, 1.0, 7.5}, "{ health = 1000, behaviour = 'idle' }");
    world.run_for(0.2);
    const auto health = [&] { return world.info(dummy).field("health")->as_number(); };
    const auto claim = [&](modding::Vec3 direction, std::uint16_t weapon) {
        world.gameplay->receive_from_client(
            5U, modding::Map{{"t", Value{"attack"}},
                             {"entity", Value{static_cast<std::int64_t>(dummy.id)}},
                             {"damage", Value{5.0}},
                             {"weapon", Value{static_cast<std::int64_t>(weapon)}},
                             {"dir", Value{direction}}});
        world.run_for(0.15);
    };
    // 0x14 is a pistol in the classic table; 0 bare hands (melee).
    const double before = health();
    claim({0.0, 0.0, 1.0}, 0U);
    expect(health() < before, "an honest swing in reach, aimed at it, lands");

    const double aimed_away = health();
    claim({0.0, 0.0, -1.0}, 0U);
    expect(health() == aimed_away, "a hit claimed while looking the other way is refused");

    world.game.remotes.front().position = {0.5, 1.0, -9.5};
    world.run_for(0.2);
    const double far = health();
    claim({0.0, 0.0, 1.0}, 0U);
    expect(health() == far, "a melee hit from 17 blocks away is refused");

    world.game.remotes.front().position = {0.5, 1.0, 4.5};
    world.game.wall(-2, 3, 1, 4, 6, 6);
    world.run_for(0.2);
    const double walled = health();
    claim({0.0, 0.0, 1.0}, 0U);
    expect(health() == walled, "a hit through a wall is refused");
    world.game.edits.clear();

    world.game.may_act[5U] = false;
    world.run_for(0.2);
    const double dead = health();
    claim({0.0, 0.0, 1.0}, 0U);
    expect(health() == dead, "a player the host holds dead cannot strike");
    world.game.may_act.clear();

    world.gameplay->receive_from_client(5U, modding::Map{{"t", Value{"attack"}},
                                                         {"entity", Value{static_cast<std::int64_t>(dummy.id)}},
                                                         {"damage", Value{5.0}},
                                                         {"weapon", Value{std::int64_t{0}}}});
    world.run_for(0.15);
    expect(health() == dead, "a claim without its aim is refused");
    expect(world.gameplay->ai_report().find("projectiles=") != std::string::npos, "the report is readable");
}

// ------------------------------------------------------------------ the load

struct Meter final : gameplay::GameplaySync
{
    std::uint8_t remote{};
    std::size_t bytes{};
    std::size_t messages{};
    void count(const Value &message)
    {
        std::vector<std::uint8_t> encoded;
        std::string error;
        if (modding::encode_value(message, modding::ValueLimits{}, encoded, error))
            bytes += encoded.size() + 8U; // + the channel's frame header, about
        ++messages;
    }
    void to_player(std::uint8_t gid, const Value &message, bool) override
    {
        if (gid == remote)
            count(message);
    }
    void to_all(const Value &message, bool) override
    {
        count(message);
    }
    void to_server(const Value &) override
    {
    }
};

void load_of_50_200_and_500_creatures()
{
    std::cout << "  creatures | host ms/tick (mean, worst 1%) | AI ms/tick | KiB/s to one client | thinks/s\n";
    for (const int count : {50, 200, 500})
    {
        Offline world;
        world.game.position = {0.5, 1.0, 0.5};
        world.game.remotes.push_back({5U, "Remote", false, true, {12.5, 1.0, 12.5}, 0.0F, 100.0F});
        world.game.authority = true;
        Meter meter;
        meter.remote = 5U;
        world.gameplay->loopback().set_remote(&meter);
        world.run(std::format(R"(
            events.on("entity.attacking", function() return false end)
            for i = 1, {} do
                local angle = i * 2.39996
                local r = 8 + (i * 37 % 90)
                entities.spawn(i % 5 == 0 and "archer" or "runner", vector.create(math.cos(angle) * r, 1, math.sin(angle) * r))
            end)",
                              count));
        world.run_for(1.0); // settle
        meter.bytes = 0U;
        const std::string before = world.gameplay->ai_report();
        std::vector<double> ticks;
        constexpr int measured = 600; // 10 s at 60 Hz
        const Value stats_before = world.probe("ai.stats()");
        for (int index = 0; index < measured; ++index)
        {
            const auto started = std::chrono::steady_clock::now();
            world.step();
            ticks.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
        const Value stats_after = world.probe("ai.stats()");
        std::ranges::sort(ticks);
        double total{};
        for (const double tick : ticks)
            total += tick;
        const double mean = total / measured;
        const double worst = ticks[static_cast<std::size_t>(measured * 0.99)];
        const double ai_ms = (stats_after.field("step_ms")->as_number() - stats_before.field("step_ms")->as_number()) / measured;
        const double thinks =
            static_cast<double>(stats_after.field("thinks")->as_integer() - stats_before.field("thinks")->as_integer()) / 10.0;
        const double kib = static_cast<double>(meter.bytes) / 10.0 / 1024.0;
        std::cout << std::format("  {:9} | {:8.3f} {:8.3f}            | {:10.3f} | {:19.1f} | {:8.0f}\n", count, mean, worst,
                                 ai_ms, kib, thinks);
        expect(mean < 16.0, "the host keeps a 60 Hz tick");
        world.gameplay->loopback().set_remote(nullptr);
    }
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"retail_rules_apply_to_families_regions_biomes_and_tiers", retail_rules_apply_to_families_regions_biomes_and_tiers},
        {"without_a_server_vm_everything_is_inert", without_a_server_vm_everything_is_inert},
        {"a_brain_runs_its_states_and_sees_and_bites", a_brain_runs_its_states_and_sees_and_bites},
        {"walls_block_sight_and_noise_gives_the_player_away", walls_block_sight_and_noise_gives_the_player_away},
        {"teams_fight_each_other", teams_fight_each_other},
        {"a_pack_surrounds_its_prey_and_flees_when_hurt", a_pack_surrounds_its_prey_and_flees_when_hurt},
        {"a_melee_strike_lands_after_its_windup_and_can_be_dodged", a_melee_strike_lands_after_its_windup_and_can_be_dodged},
        {"archers_shoot_arcing_arrows_that_the_host_resolves", archers_shoot_arcing_arrows_that_the_host_resolves},
        {"homing_bolts_follow_their_target", homing_bolts_follow_their_target},
        {"explosions_dig_only_where_the_rules_allow", explosions_dig_only_where_the_rules_allow},
        {"projectiles_that_break_blocks_do_so_outside_spawn_protection",
         projectiles_that_break_blocks_do_so_outside_spawn_protection},
        {"walkers_path_around_walls_and_flyers_and_climbers_go_over", walkers_path_around_walls_and_flyers_and_climbers_go_over},
        {"swimmers_stay_in_the_water", swimmers_stay_in_the_water},
        {"a_boss_has_phases_a_bar_music_and_a_leash", a_boss_has_phases_a_bar_music_and_a_leash},
        {"the_think_budget_and_distance_slow_thinking", the_think_budget_and_distance_slow_thinking},
        {"host_and_joined_client_share_creatures_projectiles_bosses_and_rules",
         host_and_joined_client_share_creatures_projectiles_bosses_and_rules},
        {"hostile_clients_cannot_claim_creature_hits", hostile_clients_cannot_claim_creature_hits},
        {"load_of_50_200_and_500_creatures", load_of_50_200_and_500_creatures},
    };
    int passed{};
    for (const auto &[name, test] : tests)
    {
        try
        {
            test();
            std::cout << "[PASS] " << name << '\n';
            ++passed;
        }
        catch (const std::exception &error)
        {
            std::cout << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << passed << " of " << tests.size() << " creature AI checks passed\n";
    return passed == static_cast<int>(tests.size()) ? 0 : 1;
}
