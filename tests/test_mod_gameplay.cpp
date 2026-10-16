// The gameplay modding service (openminerz/modding/gameplay.hpp).
//
// Every service from C++ against a fake game (FakeBackend) and a fake script
// host (FakeHost, whose "handlers" are C++ lambdas); the replication protocol
// between a host and a joined client, directly and over the real mod channel;
// the world sidecar; hostile client input; script-level mods through the
// runtime harness (a small survival mod); and the parity guarantee: the
// production simulation stepped with the hooks in place but no VM produces
// exactly the baseline state, saves and stock packets.

#include "modding_test_harness.hpp"
#include "state_digest.hpp"

#include "openminerz/inventory.hpp"
#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/gameplay_net.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/world.hpp"
#include "openminerz/world_save.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;
using namespace openminerz::modding;
using namespace openminerz::modding::gameplay;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check_ok(const RunResult &result, const std::string &what)
{
    if (!result.ok)
        throw std::runtime_error(what + ": " + result.error);
}

// ------------------------------------------------------------------ fakes

struct Emitted final
{
    Side side{};
    std::string name;
    Value payload;
};

// A script host whose mods are C++ lambdas: a handler per event name, and
// callbacks made with make().
class FakeHost final : public ScriptHost
{
  public:
    std::vector<Emitted> events;
    std::map<std::string, std::function<bool(const Value &)>, std::less<>> handlers;
    std::map<std::uint32_t, std::function<Value(std::span<const Value>)>> callbacks;
    std::vector<std::uint32_t> released;
    std::vector<std::function<void(Side)>> teardown;
    std::vector<std::string> logs;
    std::vector<Handle> acting;
    std::uint32_t next{1U};

    Callback make(std::function<Value(std::span<const Value>)> function)
    {
        const std::uint32_t ref = next++;
        callbacks[ref] = std::move(function);
        return Callback{7U, ref};
    }

    bool emit(Side side, std::string_view event, const Value &payload) override
    {
        events.push_back({side, std::string{event}, payload});
        const auto handler = handlers.find(event);
        return handler == handlers.end() || handler->second(payload);
    }
    Value invoke(const Callback &callback, std::span<const Value> args) override
    {
        const auto found = callbacks.find(callback.ref);
        return found == callbacks.end() ? Value{} : found->second(args);
    }
    Value invoke_as(Handle player, const Callback &callback, std::span<const Value> args) override
    {
        acting.push_back(player);
        return invoke(callback, args);
    }
    void release(const Callback &callback) noexcept override
    {
        released.push_back(callback.ref);
    }
    [[nodiscard]] std::string_view owner_of(const Callback &) const noexcept override
    {
        return "test";
    }
    void log(std::string_view, LogLevel, std::string_view message) override
    {
        logs.emplace_back(message);
    }
    void add_teardown_listener(std::function<void(Side)> listener) override
    {
        teardown.push_back(std::move(listener));
    }

    [[nodiscard]] std::size_t count(std::string_view name, std::optional<Side> side = {}) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(events, [&](const Emitted &event) {
            return event.name == name && (!side.has_value() || event.side == *side);
        }));
    }
    [[nodiscard]] const Emitted *last(std::string_view name) const
    {
        for (auto event = events.rbegin(); event != events.rend(); ++event)
            if (event->name == name)
                return &*event;
        return nullptr;
    }
};

// A small world (rock below y 0, grass at 0, air above), one local player with
// a real PlayerInventory, and whatever remote players a test adds.
class FakeBackend final : public GameplayBackend
{
  public:
    std::int32_t seed{12345};
    bool modern{true};
    std::optional<std::uint8_t> local{std::uint8_t{0U}};
    std::string local_name{"Tester"};
    std::vector<PlayerSnapshot> remotes;
    std::map<BlockPos, std::uint8_t> edits;
    std::size_t set_block_calls{};
    std::vector<std::pair<Vec3, StackView>> pickups;
    std::map<std::pair<BlockPos, std::size_t>, std::optional<StackView>> crates;
    PlayerInventory inventory;
    float speed{1.0F};
    std::vector<std::string> chat_lines;
    double clock{0.5};
    std::filesystem::path sidecar;
    float health{100.0F};
    bool alive{true};
    Vec3 position{8.5, 1.0, -7.5};
    // (fix) The grass layer's height (a world generator may raise it), and
    // whether the player stands in the world yet (local_player_placed).
    int ground{};
    bool placed{true};
    std::vector<std::pair<float, DamageCause>> damage_taken;
    std::vector<int> spawned_enemies;
    // The game's own damage path calls the service like apply_player_damage.
    GameplayService *service{};

    FakeBackend()
    {
        // An empty inventory instead of the starter loadout, so a given item
        // lands in the hand.
        inventory.set_profile(InventoryProfile::modern);
        visit([](PlayerInventory::Slot &slot, std::uint16_t) { slot.reset(); });
    }

    [[nodiscard]] std::int32_t world_seed() const override
    {
        return seed;
    }
    [[nodiscard]] bool modern_rules() const override
    {
        return modern;
    }
    [[nodiscard]] std::optional<std::uint8_t> local_player() const override
    {
        return local;
    }
    [[nodiscard]] bool local_player_placed() const override
    {
        return placed;
    }
    void players(std::vector<PlayerSnapshot> &out) const override
    {
        out.clear();
        if (local.has_value())
            out.push_back({*local, local_name, true, alive, position, 0.0F, health});
        for (const PlayerSnapshot &remote : remotes)
            out.push_back(remote);
    }
    void local_set_health(float value) override
    {
        health = value;
        if (health <= 0.0F)
            alive = false;
    }
    void local_damage(float amount, Vec3 source, DamageCause cause) override
    {
        if (!alive)
            return;
        float applied = amount;
        if (service != nullptr && service->running())
            applied = service->local_player_damaging(amount, cause, source);
        if (applied <= 0.0F)
            return;
        damage_taken.emplace_back(applied, cause);
        health -= applied;
        if (health <= 0.0F)
        {
            health = 0.0F;
            alive = false;
            if (service != nullptr)
                service->local_player_died(position, cause);
        }
    }
    void local_teleport(Vec3 feet) override
    {
        position = feet;
    }
    std::uint16_t local_give(StackView stack, bool drop_rest) override
    {
        InventoryStack given{static_cast<InventoryItemId>(stack.item), stack.count};
        given.mod_item = stack.mod_item;
        const std::uint16_t left = inventory.add_stack(given);
        if (left != 0U && drop_rest)
            pickups.emplace_back(position, StackView{stack.item, left, stack.mod_item});
        return drop_rest ? std::uint16_t{} : left;
    }
    std::uint32_t local_take(std::uint16_t item, std::uint32_t mod_item, std::uint32_t count) override
    {
        std::uint32_t taken{};
        visit([&](PlayerInventory::Slot &slot, std::uint16_t) {
            if (taken >= count || !slot.has_value() || static_cast<std::uint16_t>(slot->item) != item ||
                slot->mod_item != mod_item)
                return;
            const std::uint32_t here = std::min<std::uint32_t>(slot->count, count - taken);
            slot->count = static_cast<std::uint16_t>(slot->count - here);
            taken += here;
            if (slot->count == 0U)
                slot.reset();
        });
        return taken;
    }
    [[nodiscard]] std::uint32_t local_count(std::uint16_t item, std::uint32_t mod_item) const override
    {
        std::uint32_t total{};
        const_cast<FakeBackend *>(this)->visit([&](PlayerInventory::Slot &slot, std::uint16_t) {
            if (slot.has_value() && static_cast<std::uint16_t>(slot->item) == item && slot->mod_item == mod_item)
                total += slot->count;
        });
        return total;
    }
    void local_clear_inventory() override
    {
        visit([](PlayerInventory::Slot &slot, std::uint16_t) { slot.reset(); });
    }
    [[nodiscard]] std::optional<StackView> local_held() const override
    {
        const PlayerInventory::Slot &held = inventory.selected();
        if (!held.has_value())
            return std::nullopt;
        return StackView{static_cast<std::uint16_t>(held->item), held->count, held->mod_item};
    }
    void local_slots(std::vector<std::pair<std::uint16_t, StackView>> &out) const override
    {
        out.clear();
        const_cast<FakeBackend *>(this)->visit([&](PlayerInventory::Slot &slot, std::uint16_t index) {
            if (slot.has_value())
                out.emplace_back(index, StackView{static_cast<std::uint16_t>(slot->item), slot->count,
                                                  slot->mod_item});
        });
    }
    bool local_tag_slot(std::uint16_t index, std::uint16_t item, std::uint32_t mod_item) override
    {
        bool tagged{};
        visit([&](PlayerInventory::Slot &slot, std::uint16_t at) {
            if (at == index && slot.has_value() && static_cast<std::uint16_t>(slot->item) == item)
            {
                slot->mod_item = mod_item;
                tagged = true;
            }
        });
        return tagged;
    }
    void local_set_speed(float multiplier) override
    {
        speed = multiplier;
    }
    [[nodiscard]] std::uint8_t terrain(BlockPos cell) const noexcept
    {
        return cell.y < ground ? 6U : cell.y == ground ? 2U : 0U;
    }
    [[nodiscard]] std::optional<std::uint8_t> block(BlockPos cell) const override
    {
        if (cell.y < -64 || cell.y > 63)
            return std::nullopt;
        const auto found = edits.find(cell);
        return found != edits.end() ? found->second : terrain(cell);
    }
    bool set_block(BlockPos cell, std::uint8_t block) override
    {
        ++set_block_calls;
        edits[cell] = block;
        return true;
    }
    [[nodiscard]] int biome(int, int) const override
    {
        return 0;
    }
    [[nodiscard]] std::optional<std::array<float, 2>> light(BlockPos) const override
    {
        return std::array<float, 2>{1.0F, 0.0F};
    }
    bool set_crate_slot(BlockPos crate, std::size_t index, std::optional<StackView> stack) override
    {
        crates[{crate, index}] = stack;
        return true;
    }
    bool spawn_pickup(Vec3 at, StackView stack) override
    {
        pickups.emplace_back(at, stack);
        return true;
    }
    [[nodiscard]] double day() const override
    {
        return clock;
    }
    void set_time_of_day(float time_of_day) override
    {
        clock = std::floor(clock) + time_of_day;
    }
    bool spawn_enemy(int type, Vec3) override
    {
        spawned_enemies.push_back(type);
        return true;
    }
    void chat(std::optional<std::uint8_t>, std::string_view text) override
    {
        chat_lines.emplace_back(text);
    }
    [[nodiscard]] std::filesystem::path sidecar_directory() const override
    {
        return sidecar;
    }

    template <typename Visit>
    void visit(Visit &&function)
    {
        std::uint16_t index{};
        for (PlayerInventory::Slot &slot : inventory.storage())
            function(slot, index++);
        for (PlayerInventory::Tray &tray : inventory.trays())
            for (PlayerInventory::Slot &slot : tray)
                function(slot, index++);
    }
};

// Records what the service hands to "the network".
class RecordingSync final : public GameplaySync
{
  public:
    std::vector<std::pair<std::uint8_t, Value>> to_players;
    std::vector<Value> broadcasts;
    std::vector<Value> to_host;
    std::function<void(std::uint8_t, const Value &)> deliver;
    std::function<void(const Value &)> deliver_all;
    std::function<void(const Value &)> deliver_host;

    void to_player(std::uint8_t gid, const Value &message, bool) override
    {
        to_players.emplace_back(gid, message);
        if (deliver)
            deliver(gid, message);
    }
    void to_all(const Value &message, bool) override
    {
        broadcasts.push_back(message);
        if (deliver_all)
            deliver_all(message);
    }
    void to_server(const Value &message) override
    {
        to_host.push_back(message);
        if (deliver_host)
            deliver_host(message);
    }
    [[nodiscard]] std::size_t count(std::string_view type, std::optional<std::uint8_t> gid = {}) const
    {
        std::size_t total{};
        for (const auto &[to, message] : to_players)
            if ((!gid || *gid == to) && message.field("t") != nullptr && message.field("t")->as_string() == type)
                ++total;
        for (const Value &message : broadcasts)
            if (!gid && message.field("t") != nullptr && message.field("t")->as_string() == type)
                ++total;
        return total;
    }
};

// One service with its fakes and registry.
struct Rig final
{
    FakeHost host;
    FakeBackend backend;
    ApiRegistry registry;
    std::unique_ptr<GameplayService> service;

    explicit Rig(GameplayLimits limits = {})
    {
        service = std::make_unique<GameplayService>(host, backend, limits);
        service->register_api(registry);
        backend.service = service.get();
    }

    Value call(std::string_view name, std::vector<Value> args, Side side = Side::server,
               std::string_view mod = "test")
    {
        const ApiRegistry::Function *function = registry.find_function(name);
        if (function == nullptr)
            throw std::runtime_error(std::format("no function {}", name));
        CallContext context{mod, side, ~CapabilityMask{}, {}};
        return function->call(context, args);
    }
    bool fails(std::string_view name, std::vector<Value> args, std::string_view needle = {},
               Side side = Side::server)
    {
        try
        {
            static_cast<void>(call(name, std::move(args), side));
        }
        catch (const ScriptError &error)
        {
            return needle.empty() || std::string_view{error.what()}.find(needle) != std::string_view::npos;
        }
        return false;
    }
    void tick(float seconds, int steps = 1)
    {
        for (int step = 0; step < steps; ++step)
            service->tick(seconds);
    }
    [[nodiscard]] Handle me() const
    {
        return service->player_handle(0U);
    }
};

[[nodiscard]] Value table(std::initializer_list<std::pair<std::string, Value>> fields)
{
    Map map;
    for (const auto &field : fields)
        map.emplace_back(field.first, field.second);
    return Value{std::move(map)};
}

[[nodiscard]] Value list(std::initializer_list<Value> values)
{
    return Value{Array{values}};
}

[[nodiscard]] Value at(double x, double y, double z)
{
    return Value{Vec3{x, y, z}};
}

[[nodiscard]] std::uint16_t id_of(std::string_view item)
{
    return *retail_item_from_name(item);
}

// A temporary folder removed when the test ends.
class TemporaryFolder final
{
  public:
    explicit TemporaryFolder(std::string_view name)
        : path_{std::filesystem::temp_directory_path() / ("openminerz-mod-gameplay-" + std::string{name})}
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

// ------------------------------------------------------------------ tests

// With no VM the service is not running: every hook hands the game its own
// value back, nothing is emitted, sent, edited or dropped.
void hooks_are_inert_without_a_vm()
{
    Rig rig;
    RecordingSync wire;
    rig.service->loopback().set_remote(&wire);
    GameplayService &service = *rig.service;
    check(!service.running(), "a service nobody started is not running");
    check(service.local_player_damaging(25.0F, DamageCause::enemy, {}) == 25.0F, "damage untouched");
    check(service.local_block_breaking({1, 0, 1}, 2U), "digs allowed");
    service.local_block_broken({1, 0, 1}, 2U);
    check(service.local_block_drops({1, 0, 1}, 2U), "retail drops kept");
    check(service.local_block_placing({1, 1, 1}, 6U, {id_of("rock_block"), 1U, 0U}), "builds allowed");
    service.local_block_placed({1, 1, 1}, 6U, {id_of("rock_block"), 1U, 0U});
    check(service.local_item_using({id_of("pistol"), 1U, 0U}), "item actions allowed");
    service.local_item_crafted(id_of("torch"), 4U, 0U);
    check(service.local_chat("/home") == ChatVerdict::pass, "chat untouched");
    check(service.local_enemy_spawning(0, {}), "spawns allowed");
    service.local_enemy_spawned(0, 1U, 0U, {});
    service.local_enemy_killed(0, 1U, 0U, {});
    service.local_player_died({}, DamageCause::fall);
    service.local_player_respawned({});
    service.local_player_spawned({});
    check(!service.local_attack({0, 2, 0}, {1, 0, 0}, 5.0F, 1.0F, 0U).has_value(), "no entity hit");
    check(!service.local_interact({0, 2, 0}, {1, 0, 0}, 5.0F), "nothing to talk to");
    const std::array<BlockPos, 1> cells{BlockPos{1, 0, 1}};
    check(service.remote_block_edit(3U, {1, 0, 1}, 2U, 0U), "remote digs allowed");
    check(service.remote_blocks_removed(3U, cells), "remote blasts allowed");
    check(service.remote_chat(3U, "hi") == ChatVerdict::pass, "remote chat untouched");
    service.remote_player_joined(3U, "Someone");
    service.remote_player_dead(3U, true, {});
    service.remote_enemy_spawned(3U, 0, 1U, {});
    service.remote_enemy_killed(3U, 0, 1U, 3U, {});
    service.receive_from_client(0U, table({{"t", "ready"}}));
    service.receive_from_server(table({{"t", "registry"}}));
    for (int step = 0; step < 120; ++step)
        service.tick(1.0F / 60.0F);
    check(rig.host.events.empty(), "no event reached a script");
    check(wire.to_players.empty() && wire.broadcasts.empty() && wire.to_host.empty(), "nothing was sent");
    check(rig.backend.set_block_calls == 0U && rig.backend.pickups.empty() &&
              rig.backend.chat_lines.empty() && rig.backend.damage_taken.empty(),
          "the game was not touched");
    check(rig.backend.speed == 1.0F && service.client_speed_multiplier() == 1.0F, "speed unchanged");
    check(!service.client_difficulty().active, "difficulty inactive");
    // A client-side call of a server function is refused outright.
    check(rig.fails("players.list", {}, "not running", Side::client), "client calls refused");
}

void definitions_and_registry_snapshot()
{
    Rig rig;
    check(rig.call("blocks.define", {"copper_lamp", table({{"fallback", "lantern"}, {"name", "Copper Lamp"},
                                                           {"texture", "test/lamp.png"}, {"light", 12}})}) ==
              Value{"test:copper_lamp"},
          "a block key is namespaced by its mod");
    check(rig.fails("blocks.define", {"broken", table({})}, "fallback"), "a fallback is required");
    check(rig.fails("blocks.define", {"other:thing", table({{"fallback", "rock"}})}, "own namespace"),
          "a mod cannot define into another's namespace");
    check(rig.fails("blocks.define", {"ghost", table({{"fallback", "uninitialized"}})}), "only real blocks");
    rig.call("items.define", {"water", table({{"fallback", "stick"}, {"stack", 5000}, {"name", "Water"},
                                              {"use", Value{rig.host.make([](auto) { return Value{}; })}}})});
    rig.call("items.define", {"lamp", table({{"fallback", "lantern_block"}, {"places", "copper_lamp"}})});
    check(rig.fails("items.define", {"bad_lamp", table({{"fallback", "stick"}, {"places", "copper_lamp"}})},
                    "block item"),
          "an item can only place a mod block of its own fallback");
    check(rig.fails("items.define", {"nope", table({{"fallback", "forced_air_block"}})}), "never a project-only id");
    const Value recipe = rig.call("recipes.add", {table({{"result", "water"}, {"count", 2},
                                                         {"ingredients", list({list({"sand_block", 3})})}})});
    check(recipe.as_integer() == 1, "the first mod recipe is id 1");
    check(rig.call("recipes.remove", {"torch"}).as_bool(), "a retail recipe can be switched off");
    rig.call("loot.define", {"chest", table({{"rolls", list({1, 3})},
                                             {"entries", list({table({{"item", "iron"}, {"weight", 3}, {"max", 4}}),
                                                               table({{"item", "water"}})})}})});
    rig.call("creatures.define", {"deer", table({{"model", "test/deer.gltf"}, {"behaviour", "passive"}})});
    rig.call("stats.define", {"thirst", table({{"max", 100}, {"decay", 0.5},
                                              {"hud", table({{"label", "Thirst"}, {"color", "#3080ff"}})}})});
    bool foreign_refused{};
    try
    {
        static_cast<void>(rig.call("stats.define", {"thirst", table({})}, Side::server, "other"));
    }
    catch (const ScriptError &error)
    {
        foreign_refused = std::string_view{error.what()}.find("already defined") != std::string_view::npos;
    }
    check(foreign_refused, "another mod cannot take over a stat");

    // The snapshot a joining client receives, through the channel's codec.
    const Value snapshot = rig.service->registry_snapshot();
    std::vector<std::uint8_t> bytes;
    std::string error;
    check(modding::encode_value(snapshot, ModNetLimits{}.server_values, bytes, error),
          "the registry fits the mod channel: " + error);
    Rig client;
    client.service->start_client();
    client.service->receive_from_server(snapshot);
    const auto &blocks = client.service->client_block_types();
    check(blocks.size() == 1U && blocks[0].key == "test:copper_lamp" && blocks[0].fallback == 4U &&
              blocks[0].light == 12U && blocks[0].texture == "test/lamp.png",
          "the client knows the mod block");
    const auto &items = client.service->client_item_types();
    check(items.size() == 2U && items[0].key == "test:water" && items[0].usable && !items[1].usable,
          "the client knows the mod items");
    check(items[0].stack_limit == item_stack_limit(InventoryItemId::stick, InventoryProfile::modern),
          "a mod item never stacks past its fallback");
    check(client.service->client_recipes().size() == 1U &&
              client.service->client_recipes()[0].ingredients.size() == 1U,
          "the client knows the mod recipe");
    const auto &book = crafting_recipes(InventoryProfile::modern);
    std::size_t torch_index{};
    while (torch_index < book.size() && book[torch_index].result != InventoryItemId::torch)
        ++torch_index;
    check(torch_index < book.size() && client.service->client_recipe_disabled(torch_index),
          "the switched-off retail recipe reaches the client");
    check(client.service->client_stats().size() == 1U && client.service->client_stats()[0].hud &&
              client.service->client_stats()[0].label == "Thirst",
          "hud stats are described to the client");
}

void world_edits_are_budgeted()
{
    GameplayLimits limits;
    limits.edits_per_tick = 100U;
    Rig rig{limits};
    rig.service->start_server();
    check(rig.call("world.set_block", {at(2, 1, 2), "rock"}).as_bool(), "a cell is set");
    check(rig.backend.edits[{2, 1, 2}] == 6U, "through the backend's authoritative path");
    check(rig.call("world.get_block", {at(2.7, 1.2, 2.9)}) == Value{"rock"}, "and reads back");
    check(rig.call("world.get_block", {at(2, 5, 2)}) == Value{"air"}, "air is air");
    std::int64_t done_job{};
    std::int64_t done_changed{};
    const Callback done = rig.host.make([&](std::span<const Value> args) {
        done_job = args[0].as_integer();
        done_changed = args[1].as_integer();
        return Value{};
    });
    const std::int64_t job = rig.call("world.fill", {at(0, 1, 0), at(9, 2, 9), "wood", Value{done}}).as_integer();
    check(rig.backend.set_block_calls == 1U, "a large fill waits for the tick");
    rig.tick(1.0F / 60.0F);
    check(rig.backend.set_block_calls == 101U, "one tick's budget at a time");
    rig.tick(1.0F / 60.0F);
    check(done_job == job && done_changed == 200, "the job reports when it is done");
    check(std::ranges::find(rig.host.released, done.ref) != rig.host.released.end(), "and lets go of it");
    rig.call("world.replace", {at(0, 1, 0), at(9, 3, 9), "wood", "glass_basic"});
    rig.tick(1.0F / 60.0F, 4);
    check(rig.call("world.count_blocks", {at(0, 0, 0), at(9, 3, 9), "glass_basic"}).as_integer() == 200,
          "replace changes exactly the matching cells");
    check(rig.call("world.count_blocks", {at(0, 0, 0), at(9, 0, 9), "grass"}).as_integer() == 100,
          "and nothing else");
    check(rig.host.count("world.edit_done") == 2U, "each job announces itself");
    check(rig.call("world.find_blocks", {at(0, 0, 0), at(9, 3, 9), "glass_basic", 5}).as_array().size() == 5U,
          "region queries honour their limit");
    const Value hit = rig.call("world.raycast", {at(5.5, 10, 5.5), at(0, -1, 0)});
    check(hit.field("block")->as_string() == "glass_basic" && hit.field("position")->as_vec3().y == 2.0 &&
              hit.field("place")->as_vec3().y == 3.0,
          "a raycast finds the first block and the cell before it");
    check(rig.call("world.surface_y", {5, 5}).as_integer() == 2, "the surface is the top solid block");
    check(rig.call("world.surface_y", {50, 50}).as_integer() == 0, "plain ground is at 0");
    check(rig.fails("world.fill", {at(0, -64, 0), at(1000, 63, 1000), "air"}, "at most"),
          "an unbounded fill is refused");
}

void mod_blocks_keep_their_identity()
{
    Rig rig;
    rig.call("blocks.define", {"marble", table({{"fallback", "rock"}, {"drops", "marble_chunk"}, {"drop_count", 2}})});
    rig.call("items.define", {"marble_chunk", table({{"fallback", "rock_block"}})});
    rig.service->start_server();
    rig.service->start_client();
    rig.call("world.set_block", {at(1, 1, 1), "marble"});
    check(rig.backend.edits[{1, 1, 1}] == 6U, "the world holds the retail fallback");
    check(rig.call("world.get_block", {at(1, 1, 1)}) == Value{"test:marble"}, "the server knows the mod block");
    rig.tick(1.0F / 60.0F);
    const ClientBlockType *shown = rig.service->client_block_at({1, 1, 1}, 6U);
    check(shown != nullptr && shown->key == "test:marble", "the client draws the mod block");
    check(rig.service->client_block_at({1, 1, 1}, 0U) == nullptr, "only while the fallback is there");
    check(!rig.service->local_block_drops({1, 1, 1}, 6U), "its own drops replace the retail pickup");
    check(rig.backend.pickups.size() == 1U && rig.backend.pickups[0].second.count == 2U &&
              rig.backend.pickups[0].second.mod_item != 0U,
          "the drop is the mod item");
    rig.service->local_block_broken({1, 1, 1}, 6U);
    check(rig.host.last("block.broken")->payload.field("block")->as_string() == "test:marble",
          "block.broken names the mod block");
    rig.tick(1.0F / 60.0F);
    check(rig.service->client_block_at({1, 1, 1}, 6U) == nullptr, "and its identity is gone everywhere");
    // An edit the service never heard about ends the identity too.
    rig.call("world.set_block", {at(2, 1, 2), "marble"});
    rig.tick(1.0F / 60.0F);
    rig.backend.edits[{2, 1, 2}] = 0U;
    rig.tick(2.5F);
    check(rig.call("world.get_block", {at(2, 1, 2)}) == Value{"air"}, "a dug fallback is plain air");
    check(rig.service->client_block_at({2, 1, 2}, 6U) == nullptr, "and the client forgets it");
}

void structures_parse_rotate_and_place()
{
    Schematic schematic;
    std::string error;
    const std::string_view text = "# a hut\norigin 1 0 0\npalette # rock\npalette . air\nlayer\n###\n#.#\nlayer\n # \n";
    check(parse_schematic(text, schematic, error), "the text format parses: " + error);
    check(schematic.size_x == 3 && schematic.size_y == 2 && schematic.size_z == 2, "its size");
    check(schematic_cell(schematic, {10, 5, 10}, 2, 0, 0, 0) == BlockPos{11, 5, 10}, "no rotation");
    check(schematic_cell(schematic, {10, 5, 10}, 2, 0, 0, 1) == BlockPos{10, 5, 11}, "+X turns to +Z");
    check(schematic_cell(schematic, {10, 5, 10}, 2, 0, 0, 2) == BlockPos{9, 5, 10}, "then -X");
    check(schematic_cell(schematic, {10, 5, 10}, 2, 0, 0, 3) == BlockPos{10, 5, 9}, "then -Z");
    check(!parse_schematic("palette # rock\nlayer\n#x#\n", schematic, error) && error.find("palette") != std::string::npos,
          "an unmapped character is an error");
    check(!parse_schematic("palette # rock\n", schematic, error), "a schematic needs a layer");

    Rig rig;
    const Value size = rig.call("structures.parse", {"hut", std::string{text}});
    check(size.as_vec3() == Vec3{3, 2, 2}, "structures.parse reports the size");
    rig.service->start_server();
    rig.call("world.set_block", {at(11, 1, 11), "wood"});
    rig.call("structures.place", {"hut", at(11, 1, 11), table({{"rotation", 90}})});
    rig.tick(1.0F / 60.0F);
    // Rotated a quarter turn: the row along X now runs along Z through the anchor.
    check(rig.backend.edits[{11, 1, 10}] == 6U && rig.backend.edits[{11, 1, 12}] == 6U &&
              rig.backend.edits[{11, 1, 11}] == 6U,
          "the first row stands along Z");
    check(rig.backend.edits[{10, 1, 11}] == 0U, "the door gap is carved");
    rig.call("structures.define", {"post", table({{"palette", table({{"#", "rock"}, {".", "air"}})},
                                                   {"layers", list({list({".#."})})}})});
    rig.call("world.set_block", {at(30, 1, 30), "wood"});
    rig.call("structures.place", {"post", at(30, 1, 30), table({{"air", false}})});
    rig.tick(1.0F / 60.0F);
    check(rig.backend.edits[{30, 1, 30}] == 19U && rig.backend.edits[{31, 1, 30}] == 6U,
          "air = false leaves the world where the plan has air");
    check(rig.fails("structures.place", {"hut", at(0, 1, 0), table({{"rotation", 45}})}, "rotation"),
          "only quarter turns");
}

void world_generation_offers_each_chunk_once()
{
    GameplayLimits limits;
    limits.worldgen_chunks_per_tick = 1000U;
    Rig rig{limits};
    std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> offered;
    rig.host.handlers["worldgen.chunk"] = [&](const Value &chunk) {
        const auto key = std::pair{chunk.field("cx")->as_integer(), chunk.field("cz")->as_integer()};
        check(!offered.contains(key), "a chunk is offered once");
        offered[key] = chunk.field("seed")->as_integer();
        rig.call("world.set_block", {at(chunk.field("x")->as_number() + 8, 20, chunk.field("z")->as_number() + 8), "rock"});
        return true;
    };
    rig.service->start_server();
    rig.tick(1.0F);
    check(offered.empty(), "nothing is offered before a mod asks");
    rig.call("worldgen.enable", {table({{"radius", 2}})});
    rig.tick(1.0F);
    rig.tick(1.0F);
    check(offered.size() == 25U, "every chunk within the radius, once");
    check(rig.backend.edits.size() >= 25U, "what handlers build are ordinary world edits");
    rig.backend.position = {200.0, 1.0, 200.0};
    rig.tick(1.0F);
    rig.tick(1.0F);
    check(offered.size() == 50U, "new ground is offered as the player reaches it");
    Rig again{limits};
    std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> repeated;
    again.host.handlers["worldgen.chunk"] = [&](const Value &chunk) {
        repeated[{chunk.field("cx")->as_integer(), chunk.field("cz")->as_integer()}] = chunk.field("seed")->as_integer();
        return true;
    };
    again.service->start_server();
    again.call("worldgen.enable", {table({{"radius", 2}})});
    again.tick(1.0F);
    again.tick(1.0F);
    for (const auto &[chunk, seed] : repeated)
        check(offered.contains(chunk) && offered[chunk] == seed, "a chunk's seed is repeatable");
    const double first = rig.call("worldgen.random", {3, -4, 7}).as_number();
    check(first == again.call("worldgen.random", {3, -4, 7}).as_number() && first >= 0.0 && first < 1.0,
          "worldgen.random is repeatable");
}

// (fix) A world still loading - a mod generator's terrain going in - does not
// announce its player at activation: player.spawned comes from the spawn that
// ends the loading, and what scripts read then is the world being played.
void player_spawned_waits_for_the_loaded_world()
{
    Rig rig;
    std::optional<double> ground_seen;
    rig.host.handlers["player.spawned"] = [&](const Value &) {
        ground_seen = rig.call("world.surface_y", {8, -8}).as_number();
        return true;
    };
    rig.backend.placed = false; // the loading screen is up
    rig.service->start_server();
    const auto count = [&](std::string_view name) {
        return std::ranges::count_if(rig.host.events, [&](const Emitted &event) { return event.name == name; });
    };
    check(count("player.joined") == 1 && count("player.spawned") == 0,
          "activation during loading announces the player but not a spawn");
    // The generator's terrain is in and the spawn stands on it.
    rig.backend.ground = 20;
    rig.backend.position = {8.5, 21.0, -7.5};
    rig.backend.placed = true;
    rig.service->local_player_spawned(rig.backend.position);
    rig.service->local_player_spawned(rig.backend.position);
    check(count("player.spawned") == 1, "player.spawned fires once, from the spawn");
    const Emitted &spawned = *std::ranges::find(rig.host.events, "player.spawned", &Emitted::name);
    check(spawned.payload.field("position") != nullptr &&
              spawned.payload.field("position")->as_vec3().y == 21.0,
          "it carries the spawn on the loaded terrain");
    check(ground_seen.has_value() && *ground_seen == 20.0, "and world.surface_y inside it sees that terrain");
    const auto joined = std::ranges::find(rig.host.events, "player.joined", &Emitted::name);
    check(joined < std::ranges::find(rig.host.events, "player.spawned", &Emitted::name),
          "player.joined comes first");

    // A session started in a world already entered (a mid-game reload):
    // activation announces the player where it stands.
    Rig again;
    again.service->start_server();
    check(std::ranges::count_if(again.host.events, [](const Emitted &event) { return event.name == "player.spawned"; }) == 1,
          "a world already entered announces its player at once");
}

void stats_and_effects_replicate()
{
    Rig rig;
    rig.call("stats.define", {"thirst", table({{"max", 100}, {"decay", 10}, {"hud", true}})});
    rig.service->start_server();
    rig.service->start_client();
    rig.tick(0.1F, 10);
    const double thirst = rig.call("stats.get", {Value{rig.me()}, "thirst"}).as_number();
    check(std::abs(thirst - 90.0) < 0.01, std::format("thirst decays ({})", thirst));
    const auto &stats = rig.service->client_stats();
    // Replicated ten times a second: at most a tenth of a second behind.
    check(stats.size() == 1U && std::abs(stats[0].value - thirst) <= 1.0 + 1e-6,
          std::format("and reaches its player ({} vs {})", stats.empty() ? -1.0 : stats[0].value, thirst));
    check(rig.call("stats.set", {Value{rig.me()}, "thirst", 0.5}).as_number() == 0.5, "set returns the value");
    rig.tick(0.1F, 2);
    check(rig.host.count("stat.depleted") == 1U, "stat.depleted fires once at the minimum");
    check(rig.host.count("stat.changed", Side::server) == 1U, "stat.changed fires for script changes");
    check(rig.host.count("stat.changed", Side::client) > 0U, "and the client hears its stat move");
    rig.call("effects.apply", {Value{rig.me()}, "speed", table({{"duration", 1}, {"strength", 1.5}})});
    rig.tick(0.1F);
    check(rig.backend.speed == 1.5F && rig.service->client_effects().size() == 1U, "speed reaches the player");
    rig.tick(0.1F, 11);
    check(rig.backend.speed == 1.0F && rig.service->client_effects().empty(), "and ends with its time");
    rig.call("effects.apply", {Value{rig.me()}, "damage", table({{"duration", 2.05}, {"strength", 5}})});
    rig.tick(0.1F, 22);
    check(rig.backend.damage_taken.size() == 2U && rig.backend.damage_taken[0].second == DamageCause::effect,
          "damage over time, once a second");
    const float before = rig.backend.health;
    rig.call("effects.apply", {Value{rig.me()}, "regen", table({{"duration", 1.05}, {"strength", 5}})});
    rig.tick(0.1F, 12);
    check(rig.backend.health > before, "regeneration heals");
    int poisoned{};
    rig.call("effects.define", {"poison", table({{"interval", 0.5},
                                                 {"tick", Value{rig.host.make([&](auto) {
                                                      ++poisoned;
                                                      return Value{};
                                                  })}}})});
    rig.call("effects.apply", {Value{rig.me()}, "poison", table({{"duration", 2.0}})});
    rig.tick(0.1F, 25);
    check(poisoned == 4, std::format("a custom effect ticks on its interval ({})", poisoned));
    check(rig.fails("effects.apply", {Value{rig.me()}, "nonsense"}, "no effect"), "unknown effects are refused");
}

void damage_can_be_cancelled_and_changed()
{
    Rig rig;
    rig.host.handlers["player.damaged"] = [&](const Value &event) {
        if (event.field("cause")->as_string() == "fall")
            return false;
        rig.call("players.set_damage", {event.field("amount")->as_number() / 2.0});
        return true;
    };
    rig.service->start_server();
    check(rig.service->local_player_damaging(40.0F, DamageCause::fall, {}) == 0.0F, "a handler cancels");
    check(rig.service->local_player_damaging(40.0F, DamageCause::enemy, {}) == 20.0F, "a handler halves");
    check(rig.fails("players.set_damage", {5}, "only works inside"), "set_damage outside a handler");
    rig.host.handlers.erase("player.damaged");
    rig.backend.local_damage(500.0F, {}, DamageCause::enemy);
    const Emitted *died = rig.host.last("player.died");
    check(died != nullptr && died->payload.field("cause")->as_string() == "enemy", "death names its cause");
    rig.service->local_player_respawned({1, 2, 3});
    check(rig.host.count("player.respawned") == 1U, "and the respawn follows");
}

void commands_and_chat()
{
    Rig rig;
    rig.backend.remotes.push_back({3U, "Joiner", false, true, {0, 1, 0}, 0.0F, 100.0F});
    std::vector<std::string> arguments;
    rig.call("commands.register",
             {"home", "Teleport home", Value{rig.host.make([&](std::span<const Value> args) {
                  for (const Value &word : args[1].as_array())
                      arguments.push_back(word.as_string());
                  rig.call("players.teleport", {args[0], at(50, 1, 50)});
                  return Value{"Welcome home"};
              })}});
    rig.service->start_server();
    check(rig.service->local_chat("/home now please") == ChatVerdict::command, "a mod command is consumed");
    check(arguments == std::vector<std::string>{"now", "please"}, "with its words");
    check(rig.backend.position == Vec3{50, 1, 50}, "it acts on the caller");
    check(!rig.host.acting.empty() && rig.host.acting.back() == rig.me(), "as the calling player");
    check(std::ranges::find(rig.backend.chat_lines, "Welcome home") != rig.backend.chat_lines.end(),
          "and its reply is shown");
    check(rig.service->local_chat("/mods") == ChatVerdict::pass, "the runtime's console is left alone");
    check(rig.service->local_chat("/help") == ChatVerdict::command, "/help lists mod commands");
    rig.host.handlers["chat.message"] = [](const Value &event) {
        return event.field("text")->as_string().find("spam") == std::string::npos;
    };
    check(rig.service->local_chat("hello") == ChatVerdict::pass, "chat passes");
    check(rig.service->local_chat("buy spam") == ChatVerdict::cancel, "a handler drops a line");
    check(rig.fails("commands.register", {"help", "x", Value{rig.host.make([](auto) { return Value{}; })}}),
          "'help' is reserved");
    check(rig.fails("commands.register", {"mods", "x", Value{rig.host.make([](auto) { return Value{}; })}}),
          "'mods' is the runtime's");
    std::size_t refused{};
    for (int line = 0; line < 200; ++line)
        refused += rig.service->remote_chat(3U, "hi") == ChatVerdict::cancel ? 1U : 0U;
    check(refused > 100U, "a remote player's chat is rate limited");
}

// A host and a joined client, joined by a direct link: what the Network area's
// channel carries, without the channel.
struct HostAndClient final
{
    Rig host;
    Rig client;
    RecordingSync host_link;
    RecordingSync client_link;

    HostAndClient()
    {
        host.backend.remotes.push_back({3U, "Joiner", false, true, {20.5, 1, -7.5}, 0.0F, 100.0F});
        client.backend.local = std::uint8_t{3U};
        client.backend.local_name = "Joiner";
        client.backend.position = {20.5, 1, -7.5};
        host_link.deliver = [this](std::uint8_t gid, const Value &message) {
            if (gid == 3U)
                client.service->receive_from_server(message);
        };
        host_link.deliver_all = [this](const Value &message) { client.service->receive_from_server(message); };
        client_link.deliver_host = [this](const Value &message) { host.service->receive_from_client(3U, message); };
        host.service->loopback().set_remote(&host_link);
        client.service->loopback().set_remote(&client_link);
    }
};

void a_joined_client_plays_through_the_host()
{
    HostAndClient session;
    Rig &host = session.host;
    Rig &client = session.client;
    host.call("stats.define", {"thirst", table({{"max", 100}, {"hud", true}})});
    host.call("items.define", {"water", table({{"fallback", "stick"},
                                               {"use", Value{host.host.make([&](std::span<const Value> args) {
                                                    if (host.call("players.take", {args[0], "water", 1}).as_integer() == 1)
                                                        host.call("stats.add", {args[0], "thirst", 25});
                                                    return Value{};
                                                })}}})});
    host.service->start_server();
    client.service->start_client();
    host.tick(0.05F);
    check(client.service->client_item_types().size() == 1U, "the joined client got the registry");
    check(client.service->client_stats().size() == 1U, "and its stats");
    const Handle joiner = host.service->player_handle(3U);
    check(joiner.valid() && *host.service->player_key(joiner) == "Joiner", "the joiner has a handle and key");
    host.call("stats.set", {Value{joiner}, "thirst", 50});
    host.call("players.give", {Value{joiner}, "water", 2});
    host.tick(0.05F);
    check(client.backend.local_count(id_of("stick"), 1U) == 2U, "the owner's inventory holds the tagged item");
    check(client.backend.local_count(id_of("stick"), 0U) == 0U, "not a plain stick");
    const auto held = client.backend.local_held();
    check(held.has_value() && held->mod_item == 1U, "it is in hand");
    check(!client.service->local_item_using(*held), "the client asks the server instead of acting");
    host.tick(0.05F);
    check(client.backend.local_count(id_of("stick"), 1U) == 1U, "the server's use took one");
    check(host.call("stats.get", {Value{joiner}, "thirst"}).as_number() == 75.0, "and refilled the stat");
    host.tick(0.1F, 3);
    check(std::abs(client.service->client_stats()[0].value - 75.0) < 0.01, "which the owner sees");
    client.tick(2.1F);
    check(host.call("players.count", {Value{joiner}, "water"}).as_integer() == 1, "the owner reports its inventory");
    check(client.service->local_player_damaging(30.0F, DamageCause::enemy, {}) == 30.0F,
          "a remote player's own damage is not the server's to cancel");
    const Emitted *hurt = host.host.last("player.damaged");
    check(hurt != nullptr && hurt->payload.field("remote")->as_bool(), "but the server hears of it");
    // Dialogue round trip.
    int chosen{};
    host.call("dialogue.define",
              {"smith", table({{"title", "Smith"},
                               {"nodes", table({{"hello", table({{"text", "Need work?"},
                                                                 {"choices", list({table({{"text", "Yes"}, {"next", "deal"},
                                                                                          {"action", Value{host.host.make([&](auto) {
                                                                                               ++chosen;
                                                                                               return Value{};
                                                                                           })}}}),
                                                                                   table({{"text", "No"}})})}})},
                                                {"deal", table({{"text", "Bring iron."}})}})},
                               {"start", "hello"}})});
    host.call("dialogue.open", {Value{joiner}, "smith"});
    const auto &shown = client.service->client_dialogue();
    check(shown.has_value() && shown->text == "Need work?" && shown->choices.size() == 2U, "the client sees the node");
    client.service->client_choose(shown->session, 0U);
    check(chosen == 1, "the server ran the choice");
    check(client.service->client_dialogue().has_value() && client.service->client_dialogue()->text == "Bring iron.",
          "and showed the next node");
    client.service->client_choose(client.service->client_dialogue()->session, 0U);
    check(!client.service->client_dialogue().has_value(), "Goodbye closes it");
    // Difficulty and entities reach every peer.
    host.call("difficulty.set", {table({{"enemy_health", 2.0}, {"types", table({{"zombie", table({{"damage", 3.0}})}})}})});
    host.call("creatures.define", {"deer", table({{"model", "test/deer.gltf"}, {"health", 4}})});
    const Value deer = host.call("entities.spawn", {"deer", at(22.5, 1, -7.5)});
    host.tick(0.05F);
    check(client.service->client_difficulty().active && client.service->client_difficulty().enemy_health == 2.0F &&
              client.service->client_difficulty().damage_for(0) == 3.0F,
          "difficulty is replicated");
    check(client.service->client_entities().size() == 1U && client.service->client_entities()[0].model == "test/deer.gltf",
          "the entity is drawn on the client");
    const auto struck = client.service->local_attack({20.5, 2.0, -7.5}, {1, 0, 0}, 10.0F, 1.0F,
                                                     id_of("pistol"));
    check(struck.has_value(), "the client hits what it draws");
    check(host.call("entities.get", {deer}).field("health")->as_number() < 4.0, "and the server applies it");
    host.call("entities.remove", {deer});
    host.tick(0.05F);
    check(client.service->client_entities().empty(), "removal is replicated");
    // A mod block the joiner builds: the stock edit carries the fallback, the
    // identity follows over the mod channel and is checked against the world.
    host.call("blocks.define", {"lamp", table({{"fallback", "lantern"}})});
    host.call("items.define", {"lamp_item", table({{"fallback", "lantern_block"}, {"places", "lamp"}})});
    host.service->send_full_state(3U);
    client.backend.local_clear_inventory();
    host.call("players.give", {Value{joiner}, "lamp_item", 1});
    const auto lamp = client.backend.local_held();
    check(lamp.has_value() && lamp->mod_item != 0U, "the joiner holds the lamp item");
    host.backend.edits[{21, 1, -7}] = 4U; // the AlterBlockMessage the host applied
    client.service->local_block_placed({21, 1, -7}, 4U, *lamp);
    check(host.call("world.get_block", {at(21, 1, -7)}) == Value{"test:lamp"}, "the host knows the mod block");
    client.service->local_block_placed({21, 30, -7}, 4U, *lamp);
    check(host.call("world.get_block", {at(21, 30, -7)}) == Value{"air"}, "but not where the world disagrees");
}

void the_mod_channel_carries_it()
{
    net::ModManifest manifest;
    manifest.protocol = net::ProtocolVersion::modern_v4;
    std::array<std::uint8_t, 32U> digest{};
    digest.fill(3U);
    manifest.mods.push_back({"test", "1.0", digest, net::ModScope::server});
    ModNetServer server{manifest};
    ModNetClient channel{manifest};
    Rig host;
    Rig client;
    host.backend.remotes.push_back({3U, "Joiner", false, true, {0, 1, 0}, 0.0F, 100.0F});
    client.backend.local = std::uint8_t{3U};
    host.call("items.define", {"water", table({{"fallback", "stick"}})});
    host.service->start_server();
    ModNetGameplaySync host_link{*host.service};
    host_link.attach(&server, nullptr);
    host.service->loopback().set_remote(&host_link);
    ModNetGameplaySync client_link{*client.service};
    client_link.attach(nullptr, &channel);
    client.service->loopback().set_remote(&client_link);
    double now{};
    const auto pump = [&] {
        for (int round = 0; round < 32; ++round)
        {
            bool moved{};
            channel.update(now);
            for (const auto &frame : channel.take_sends())
            {
                server.receive(42U, frame, now);
                moved = true;
            }
            server.update(now);
            for (ModNetSend &send : server.take_sends())
                if (send.peer == 42U)
                {
                    channel.receive(send.frame, now);
                    moved = true;
                }
            if (!moved)
                return;
        }
    };
    check(server.add_peer(42U, host.service->player_handle(3U), now), "the joiner gets a channel");
    pump();
    channel.set_ready(now);
    pump();
    client.service->start_client();
    client.service->client_channel_ready();
    for (int step = 0; step < 4; ++step)
    {
        now += 0.05;
        pump();
    }
    check(client.service->client_item_types().size() == 1U, "the registry crossed the channel");
    host.call("players.give", {Value{host.service->player_handle(3U)}, "water", 3});
    now += 0.05;
    pump();
    check(client.backend.local_count(id_of("stick"), 1U) == 3U, "a directive crossed the channel");
    check(host_link.dropped() == 0U && client_link.dropped() == 0U, "nothing was dropped");
    // (Integration) One mechanism: the definitions travel as the gameplay
    // "registry" message above; the channel's generic content registry is
    // not used for them.
    check(!channel.content_registry().has_value(), "no second registry crossed the channel");
}

void creatures_behave()
{
    GameplayLimits limits;
    limits.max_entities = 8U;
    Rig rig{limits};
    rig.call("items.define", {"hide", table({{"fallback", "stick"}})});
    rig.call("creatures.define", {"deer", table({{"health", 4}, {"speed", 3}, {"behaviour", "passive"}, {"drops", "hide"}})});
    rig.call("creatures.define", {"wolf", table({{"behaviour", "hostile"}, {"damage", 10}, {"speed", 4},
                                                 {"attack_cooldown", 1}, {"view_range", 16}})});
    rig.call("creatures.define", {"smith", table({{"behaviour", "npc"}, {"dialogue", "smith_talk"}})});
    rig.call("dialogue.define", {"smith_talk", table({{"nodes", table({{"hello", table({{"text", "Hi"}})}})}})});
    rig.service->start_server();
    rig.service->start_client();
    const Value deer = rig.call("entities.spawn", {"deer", at(14.5, 1, -7.5)});
    check(rig.call("entities.get", {deer}).field("health")->as_number() == 4.0, "a creature spawns whole");
    rig.call("entities.set_behaviour", {deer, "follow", table({{"target", Value{rig.me()}}, {"distance", 2}})});
    rig.tick(0.05F, 60);
    const Vec3 near = rig.call("entities.get", {deer}).field("position")->as_vec3();
    check(std::hypot(near.x - 8.5, near.z + 7.5) < 3.0, "follow closes in");
    rig.call("entities.move_to", {deer, at(20.5, 1, 2.5)});
    for (int step = 0; step < 400 && rig.host.count("entity.arrived") == 0U; ++step)
        rig.tick(0.05F);
    const Emitted *arrived = rig.host.last("entity.arrived");
    check(arrived != nullptr && arrived->payload.field("reached")->as_bool(), "a path is walked to its end");
    rig.call("world.fill", {at(24, 1, 0), at(24, 2, 6), "rock"});
    rig.call("entities.move_to", {deer, at(27.5, 1, 2.5)});
    for (int step = 0; step < 600 && rig.host.count("entity.arrived") < 2U; ++step)
        rig.tick(0.05F);
    check(rig.host.count("entity.arrived") == 2U && rig.host.last("entity.arrived")->payload.field("reached")->as_bool(),
          "and around a wall");
    static_cast<void>(rig.call("entities.spawn", {"wolf", at(11.5, 1, -7.5)}));
    rig.tick(0.05F, 80);
    check(std::ranges::any_of(rig.backend.damage_taken,
                              [](const auto &hit) { return hit.second == DamageCause::creature; }),
          "a hostile creature attacks the player");
    rig.call("entities.damage", {deer, 10, Value{rig.me()}});
    check(rig.host.count("entity.died") == 1U && !rig.call("entities.exists", {deer}).as_bool(), "it dies");
    check(std::ranges::any_of(rig.backend.pickups, [](const auto &drop) { return drop.second.mod_item == 1U; }),
          "and drops its mod item");
    static_cast<void>(rig.call("entities.spawn", {"smith", at(10.5, 1, -7.5), table({{"name", "Bob"}})}));
    rig.tick(0.05F);
    check(rig.service->local_interact({8.5, 2.4, -7.5}, {1, 0, 0}, 5.0F), "the player talks to the NPC");
    check(rig.host.count("entity.interacted") == 1U && rig.service->client_dialogue().has_value() &&
              rig.service->client_dialogue()->text == "Hi",
          "which opens its dialogue");
    std::size_t spawned = rig.call("entities.list", {}).as_array().size();
    while (spawned < 20U && rig.call("entities.spawn", {"deer", at(0.5, 1, 0.5)}).type() == ValueType::handle)
        ++spawned;
    check(spawned == limits.max_entities, "the entity limit holds");
}

void creatures_spawn_by_rule()
{
    Rig rig;
    rig.call("creatures.define", {"rabbit", table({{"spawn", table({{"chance", 1.0}, {"max", 3}, {"time", "day"}})}})});
    rig.call("creatures.define", {"bat", table({{"spawn", table({{"chance", 1.0}, {"max", 3}, {"time", "night"}})}})});
    rig.backend.clock = 0.5; // noon
    rig.service->start_server();
    rig.tick(1.0F, 6);
    const auto count = [&](std::string_view kind) {
        return rig.call("entities.list", {table({{"kind", std::string{kind}}})}).as_array().size();
    };
    check(count("rabbit") == 3U, std::format("a day creature spawns up to its cap ({})", count("rabbit")));
    check(count("bat") == 0U, "a night creature waits for the night");
    rig.host.handlers["entity.spawning"] = [](const Value &) { return false; };
    rig.backend.clock = 0.95;
    rig.tick(1.0F, 4);
    check(count("bat") == 0U, "entity.spawning can refuse");
}

void quests_follow_the_player()
{
    Rig rig;
    rig.call("items.define", {"gem", table({{"fallback", "diamond"}})});
    rig.call("creatures.define", {"smith", table({{"behaviour", "npc"}})});
    rig.call("quests.define",
             {"q1", table({{"title", "Hunter"},
                           {"stages", list({table({{"text", "Kill"}, {"objectives", list({table({{"type", "kill"}, {"target", "zombie"}, {"count", 2}})})}}),
                                            table({{"text", "Gems"}, {"objectives", list({table({{"type", "collect"}, {"target", "gem"}, {"count", 2}})})}}),
                                            table({{"text", "Go"}, {"objectives", list({table({{"type", "reach"}, {"position", at(100, 1, 100)}, {"radius", 3}})})}}),
                                            table({{"text", "Talk"}, {"objectives", list({table({{"type", "talk"}, {"target", "test:smith"}})})}}),
                                            table({{"text", "Ritual"}, {"objectives", list({table({{"type", "custom"}, {"id", "ritual"}, {"count", 3}})})}})})},
                           {"rewards", table({{"items", list({list({"iron", 5})})}})}})});
    rig.service->start_server();
    rig.service->start_client();
    check(rig.call("quests.start", {Value{rig.me()}, "q1"}).as_bool(), "a quest starts");
    check(!rig.call("quests.start", {Value{rig.me()}, "q1"}).as_bool(), "only once while active");
    rig.service->local_enemy_killed(0, 1U, 0U, {});
    rig.service->local_enemy_killed(0, 2U, 0U, {});
    check(rig.call("quests.status", {Value{rig.me()}, "q1"}).field("stage_index")->as_integer() == 2, "kills count");
    rig.call("players.give", {Value{rig.me()}, "gem", 2});
    rig.tick(0.5F);
    check(rig.call("quests.status", {Value{rig.me()}, "q1"}).field("stage_index")->as_integer() == 3, "items count");
    rig.backend.position = {100.5, 1, 100.5};
    rig.tick(0.5F);
    check(rig.call("quests.status", {Value{rig.me()}, "q1"}).field("stage_index")->as_integer() == 4, "places count");
    static_cast<void>(rig.call("entities.spawn", {"smith", at(102.5, 1, 100.5)}));
    check(rig.service->local_interact({100.5, 2.4, 100.5}, {1, 0, 0}, 5.0F), "the NPC is in reach");
    check(rig.call("quests.status", {Value{rig.me()}, "q1"}).field("stage_index")->as_integer() == 5, "talking counts");
    rig.call("quests.advance", {Value{rig.me()}, "q1", "ritual", 3});
    const Value status = rig.call("quests.status", {Value{rig.me()}, "q1"});
    check(status.field("completed")->as_bool() && !status.field("active")->as_bool(), "the quest completes");
    check(rig.backend.local_count(id_of("iron"), 0U) == 5U, "and pays its reward");
    check(rig.host.count("quest.completed") == 1U && rig.host.count("quest.stage") == 4U, "with its events");
    rig.tick(0.1F);
    check(rig.service->client_quests().size() == 1U && rig.service->client_quests()[0].completed,
          "the quest log shows it");
    check(!rig.call("quests.start", {Value{rig.me()}, "q1"}).as_bool(), "a finished quest does not repeat");
}

void the_sidecar_round_trips()
{
    TemporaryFolder folder{"sidecar"};
    const auto define = [&folder](Rig &rig, bool with_marble) {
        if (with_marble)
            rig.call("blocks.define", {"marble", table({{"fallback", "rock"}})});
        rig.call("stats.define", {"thirst", table({{"max", 100}})});
        rig.call("creatures.define", {"smith", table({{"behaviour", "npc"}})});
        rig.call("quests.define", {"q1", table({{"stages", list({table({{"objectives", list({table({{"type", "custom"}, {"count", 5}})})}})})}})});
        rig.backend.sidecar = folder.path();
    };
    {
        Rig rig;
        define(rig, true);
        rig.service->start_server();
        rig.call("world.set_block", {at(4, 1, 4), "marble"});
        rig.call("stats.set", {Value{rig.me()}, "thirst", 42});
        rig.call("quests.start", {Value{rig.me()}, "q1"});
        rig.call("quests.advance", {Value{rig.me()}, "q1", Value{}, 2});
        rig.call("entities.spawn", {"smith", at(6.5, 1, 6.5), table({{"name", "Bob"}, {"data", table({{"mood", "grumpy"}})}})});
        rig.call("worldgen.enable", {table({{"radius", 1}})});
        rig.call("difficulty.set", {table({{"enemy_damage", 1.5}})});
        rig.tick(1.0F, 2);
        rig.service->save();
        check(std::filesystem::exists(folder.path() / sidecar_file_name), "the sidecar is written");
    }
    {
        Rig rig;
        define(rig, true);
        rig.backend.edits[{4, 1, 4}] = 6U; // the world save kept the fallback
        rig.service->start_server();
        rig.service->start_client();
        check(rig.call("world.get_block", {at(4, 1, 4)}) == Value{"test:marble"}, "mod blocks come back");
        check(rig.call("stats.get", {Value{rig.me()}, "thirst"}).as_number() == 42.0, "stats come back");
        const Value quest = rig.call("quests.status", {Value{rig.me()}, "q1"});
        check(quest.field("active")->as_bool() &&
                  quest.field("objectives")->as_array()[0].field("progress")->as_integer() == 2,
              "quest progress comes back");
        const Array npcs = rig.call("entities.list", {}).as_array();
        check(npcs.size() == 1U && rig.call("entities.get", {npcs[0]}).field("name")->as_string() == "Bob" &&
                  rig.call("entities.get", {npcs[0]}).field("data")->field("mood")->as_string() == "grumpy",
              "persistent NPCs come back with their data");
        rig.tick(0.05F);
        check(rig.service->client_difficulty().active && rig.service->client_difficulty().enemy_damage == 1.5F,
              "difficulty comes back");
    }
    {
        Rig rig;
        define(rig, false);
        rig.backend.edits[{4, 1, 4}] = 6U;
        rig.service->start_server();
        check(rig.call("world.get_block", {at(4, 1, 4)}) == Value{"rock"}, "without the mod it is its fallback");
        check(std::ranges::any_of(rig.host.logs, [](const std::string &line) { return line.find("not loaded") != std::string::npos; }),
              "and the player is told why");
    }
    {
        // A server torn down before its first tick never read the sidecar,
        // so it must not write one either.
        const auto before = std::filesystem::file_size(folder.path() / sidecar_file_name);
        Rig rig;
        define(rig, true);
        rig.service->start_server_deferred();
        rig.service->stop_server();
        check(std::filesystem::file_size(folder.path() / sidecar_file_name) == before,
              "an unread sidecar is left alone");
    }
    {
        std::ofstream{folder.path() / sidecar_file_name, std::ios::binary} << "OMZG\x01\0\0\0\x09\xff\xff";
        Rig rig;
        define(rig, true);
        rig.service->start_server();
        check(!rig.host.logs.empty(), "a damaged sidecar is reported, not trusted");
    }
}

void hostile_client_messages_are_contained()
{
    Rig rig;
    rig.backend.remotes.push_back({3U, "Mallory", false, true, {0.5, 1, 0.5}, 0.0F, 100.0F});
    rig.call("creatures.define", {"deer", table({{"health", 50}})});
    rig.service->start_server();
    rig.service->tick(0.05F);
    const Value deer = rig.call("entities.spawn", {"deer", at(2.5, 1, 0.5)});
    const Value far_deer = rig.call("entities.spawn", {"deer", at(400.5, 1, 0.5)});
    const std::vector<Value> garbage{
        Value{42},
        table({{"t", 123}}),
        table({{"t", "use"}, {"key", 999}}),
        table({{"t", "attack"}, {"entity", "x"}}),
        table({{"t", "choose"}, {"session", -5}, {"choice", 1e9}}),
        table({{"t", "report"}, {"health", "lots"}}),
        table({{"t", "crafted"}, {"item", 70000}, {"count", -3}}),
        table({{"t", "command"}, {"text", std::string(5000, '/')}}),
        table({{"t", "interact"}, {"entity", static_cast<std::int64_t>(far_deer.as_handle().id)}}),
        table({{"t", "nonsense"}}),
    };
    for (const Value &message : garbage)
        rig.service->receive_from_client(3U, message);
    rig.service->receive_from_client(99U, table({{"t", "ready"}}));
    check(rig.host.count("entity.interacted") == 0U, "a far interaction is refused");
    // A forged one-hit kill is clamped to the weapon's own damage.
    rig.service->receive_from_client(3U, table({{"t", "attack"}, {"entity", static_cast<std::int64_t>(deer.as_handle().id)},
                                                {"damage", 1e9}, {"weapon", static_cast<std::int64_t>(id_of("pistol"))}}));
    const double health = rig.call("entities.get", {deer}).field("health")->as_number();
    check(health > 40.0, std::format("a forged hit is clamped ({})", health));
    Map counts;
    for (int index = 0; index < 5000; ++index)
        counts.emplace_back(std::format("item{}", index), index);
    rig.service->receive_from_client(3U, table({{"t", "inventory"}, {"counts", Value{std::move(counts)}}}));
    std::size_t hurt_before = rig.host.count("player.damaged");
    for (int flood = 0; flood < 1000; ++flood)
        rig.service->receive_from_client(3U, table({{"t", "hurt"}, {"amount", 1}}));
    check(rig.host.count("player.damaged") - hurt_before <= 60U, "a flood is cut off by the budget");
    // The client half shrugs off a malformed server too.
    Rig client;
    client.service->start_client();
    for (const Value &message : {table({{"t", "ent+"}, {"id", -1}}), table({{"t", "overlay"}, {"cells", "x"}}),
                                 table({{"t", "do"}, {"op", "give"}, {"item", 1e9}, {"count", 1e9}}),
                                 table({{"t", "registry"}, {"blocks", list({table({{"id", 7}})})}}),
                                 table({{"t", "dialogue"}, {"choices", 5}})})
        client.service->receive_from_server(message);
    check(client.service->client_entities().empty() && client.service->client_block_types().empty(),
          "nothing malformed is kept");
}

void mod_items_never_mix_with_retail_stacks()
{
    PlayerInventory inventory;
    inventory.set_profile(InventoryProfile::modern);
    for (auto &tray : inventory.trays())
        for (auto &slot : tray)
            slot.reset();
    check(inventory.add_item(InventoryItemId::stick, 10U) == 0U, "sticks go in");
    InventoryStack water{InventoryItemId::stick, 5U};
    water.mod_item = 7U;
    check(inventory.add_stack(water) == 0U, "the mod item goes in");
    std::size_t stacks{};
    for (const auto &slot : inventory.tray())
        stacks += slot.has_value() ? 1U : 0U;
    check(stacks == 2U, "a mod item does not merge into its fallback's stack");
    check(inventory.count(InventoryItemId::stick) == 10U, "retail counts ignore mod items");
    check(inventory.consume_item(InventoryItemId::stick, 10U), "retail consumption takes retail sticks");
    check(inventory.count(InventoryItemId::stick) == 0U && inventory.tray()[1].has_value() &&
              inventory.tray()[1]->mod_item == 7U,
          "and leaves the mod item alone");
    InventoryStack plain{InventoryItemId::stick, 5U};
    check(!plain.same_kind(water) && water.same_kind(water.with_count(1U)), "same_kind compares identities");
    // The identity is never written to the retail save or a stock message.
    WorldSaveData tagged;
    tagged.inventory = inventory;
    WorldSaveData untagged = tagged;
    untagged.inventory.tray()[1]->mod_item = 0U;
    std::ostringstream tagged_bytes;
    std::ostringstream untagged_bytes;
    check(write_world_save(tagged_bytes, tagged) && write_world_save(untagged_bytes, untagged), "saves write");
    check(tagged_bytes.str() == untagged_bytes.str(), "the retail save holds the fallback only");
    net::InventorySlotPayload tagged_payload;
    net::InventorySlotPayload untagged_payload;
    check(net::inventory_slot_to_payload(tagged.inventory.tray()[1], net::ProtocolVersion::modern_v4, tagged_payload) &&
              net::inventory_slot_to_payload(untagged.inventory.tray()[1], net::ProtocolVersion::modern_v4,
                                             untagged_payload),
          "both encode");
    check(test::describe_slot(tagged_payload) == test::describe_slot(untagged_payload),
          "a stock message carries the fallback only");
}

// ----------------------------------------------------------------- parity

// The production simulation (terrain, player physics, the enemy horde, the
// inventory, block edits, the save and stock packets) stepped for ten seconds
// three ways: as the game runs without the gameplay hooks, with the hooks in
// place and the service idle (no VM), and with the service running but no mod
// content. The hook calls mirror app.cpp's call sites and use their results
// the same way. All three must produce identical state, saves and packets.
enum class HookMode
{
    none,
    idle,
    running
};

struct ParityResult final
{
    std::string facts;
    std::uint64_t hash{};
    std::string save;
    std::string packets;
};

[[nodiscard]] std::string bits(float value)
{
    return std::format("{:08x}", std::bit_cast<std::uint32_t>(value));
}

ParityResult run_parity(HookMode mode)
{
    const world::WorldGenerator generator{424242, world::TerrainProfile::modern, GameMode::survival};
    world::WorldRegion region = generator.generate_region(-2, -2, 4, 4);
    world::PlayerSimulation player;
    player.spawn(region, 8, 8);
    world::EnemyHorde horde;
    horde.set_modern_rules(true);
    horde.seed_dig_random(7);
    PlayerInventory inventory;
    inventory.set_profile(InventoryProfile::modern);
    std::vector<std::array<int, 4>> edits;
    float health = 100.0F;

    FakeHost host;
    FakeBackend backend;
    ApiRegistry registry;
    std::unique_ptr<GameplayService> service;
    if (mode != HookMode::none)
    {
        service = std::make_unique<GameplayService>(host, backend);
        service->register_api(registry);
        if (mode == HookMode::running)
        {
            service->start_server();
            service->start_client();
        }
    }
    const auto live = [&] { return service != nullptr && service->running(); };
    float speed = 1.0F;
    player.set_velocity_attenuation([&](world::Vec3 forward, world::Vec3 position) {
        const float attenuation = horde.attenuate_velocity(0U, forward, position);
        return speed == 1.0F ? attenuation : attenuation * speed;
    });

    std::uint32_t next_enemy{1U};
    constexpr float step = 1.0F / 60.0F;
    for (int tick = 0; tick < 600; ++tick)
    {
        world::PlayerInput input;
        input.forward = (tick / 120) % 2 == 0 ? 1.0F : -0.5F;
        input.strafe = (tick / 75) % 3 == 1 ? 1.0F : 0.0F;
        input.jump = tick % 47 == 0;
        player.update(region, input, step, true, false, world::PlayerSimulation::fly_speed, true);
        if (tick % 90 == 30)
        {
            const world::Vec3 spot{player.state().position.x + 6.0F, player.state().position.y,
                                   player.state().position.z + 3.0F};
            const int type = (tick / 90) % 3;
            const bool allowed = !live() || service->local_enemy_spawning(type, {spot.x, spot.y, spot.z});
            if (allowed && horde.spawn(type, spot, 0.6F, next_enemy++, 0U))
            {
                world::Enemy &enemy = horde.enemies().back();
                if (live())
                {
                    if (service->client_difficulty().active)
                        enemy.health *= service->client_difficulty().health_for(enemy.type);
                    service->local_enemy_spawned(enemy.type, enemy.id, 0U, {enemy.position.x, enemy.position.y, enemy.position.z});
                }
            }
        }
        const world::EnemyEvents events = horde.update(region, player.state(), step);
        if (events.player_damage > 0.0F)
        {
            float amount = events.player_damage * 100.0F;
            if (live() && service->client_difficulty().active)
                amount *= service->client_difficulty().enemy_damage;
            if (live())
                amount = service->local_player_damaging(amount, DamageCause::enemy, {});
            health = std::max(0.0F, health - amount);
        }
        if (tick % 100 == 50)
        {
            // A dig: the cell under the player's feet ahead.
            const int x = static_cast<int>(std::floor(player.state().position.x)) + 1;
            const int y = static_cast<int>(std::floor(player.state().position.y)) - 1;
            const int z = static_cast<int>(std::floor(player.state().position.z));
            const world::BlockType block = region.block(x, y, z);
            if (block != world::BlockType::empty &&
                (!live() || service->local_block_breaking({x, y, z}, static_cast<std::uint8_t>(block))))
            {
                region.set_block(x, y, z, world::BlockType::empty);
                edits.push_back({x, y, z, 0});
                if (!live() || service->local_block_drops({x, y, z}, static_cast<std::uint8_t>(block)))
                    static_cast<void>(inventory.add(block, 1U));
                if (live())
                    service->local_block_broken({x, y, z}, static_cast<std::uint8_t>(block));
            }
        }
        if (tick % 100 == 99)
        {
            // A build, from the held stack.
            const int x = static_cast<int>(std::floor(player.state().position.x)) - 2;
            const int y = static_cast<int>(std::floor(player.state().position.y)) + 2;
            const int z = static_cast<int>(std::floor(player.state().position.z));
            const StackView held{static_cast<std::uint16_t>(inventory.selected_item()), 1U, 0U};
            if (inventory.selected().has_value() && region.block(x, y, z) == world::BlockType::empty &&
                (!live() || service->local_block_placing({x, y, z}, 6U, held)))
            {
                region.set_block(x, y, z, world::BlockType::rock);
                edits.push_back({x, y, z, 6});
                static_cast<void>(inventory.consume_selected());
                if (live())
                    service->local_block_placed({x, y, z}, 6U, held);
            }
        }
        if (tick == 300)
        {
            static_cast<void>(inventory.add_item(InventoryItemId::stick, 6U));
            static_cast<void>(inventory.add_item(InventoryItemId::coal, 3U));
            const auto &book = crafting_recipes(InventoryProfile::modern);
            for (const CraftingRecipe &recipe : book)
                if (recipe.result == InventoryItemId::torch && inventory.can_craft(recipe))
                {
                    static_cast<void>(inventory.craft(recipe));
                    if (live())
                        service->local_item_crafted(static_cast<std::uint16_t>(recipe.result), recipe.result_count, 0U);
                    break;
                }
        }
        if (tick % 60 == 0 && live())
            check(service->local_item_using({static_cast<std::uint16_t>(inventory.selected_item()), 1U, 0U}) ||
                      inventory.selected_item() == InventoryItemId::bare_hands,
                  "no mod content: item use is never refused");
        if (live())
        {
            service->tick(step);
            speed = backend.speed;
        }
    }

    ParityResult result;
    test::StateDigest digest{"parity"};
    WorldSaveData save;
    save.seed = 424242;
    save.inventory = inventory;
    save.edits = edits;
    save.player_position = player.state().position;
    save.player_yaw = player.state().yaw;
    save.player_health = health;
    save.has_player = true;
    test::record_world_state(digest, save, net::ProtocolVersion::modern_v4, test::DigestRegion{});
    test::record_enemies(digest, horde);
    test::record_player(digest, 0U, {"tester", static_cast<std::uint16_t>(inventory.selected_item()), health <= 0.0F, true});
    const world::Vec3 at_end = player.state().position;
    digest.record(test::StateDomain::player, "position", bits(at_end.x) + bits(at_end.y) + bits(at_end.z));
    digest.record(test::StateDomain::player, "health", bits(health));
    std::size_t slot_index{};
    const auto record_slot = [&](const PlayerInventory::Slot &slot) {
        net::InventorySlotPayload payload;
        static_cast<void>(net::inventory_slot_to_payload(slot, net::ProtocolVersion::modern_v4, payload));
        digest.record(test::StateDomain::inventory, std::to_string(slot_index++), test::describe_slot(payload));
        if (slot.has_value())
            result.packets += test::describe_slot(payload);
    };
    for (const auto &slot : inventory.storage())
        record_slot(slot);
    for (const auto &tray : inventory.trays())
        for (const auto &slot : tray)
            record_slot(slot);
    for (const world::Enemy &enemy : horde.enemies())
        digest.record(test::StateDomain::enemies, std::format("pose{}", enemy.id),
                      bits(enemy.position.x) + bits(enemy.position.y) + bits(enemy.position.z) + bits(enemy.health));
    std::ostringstream out;
    check(write_world_save(out, save), "the parity save writes");
    result.save = out.str();
    for (const auto &edit : edits)
    {
        net::PayloadError error{};
        const auto payload = net::encode_alter_block_payload(
            {{edit[0], edit[1], edit[2]}, edit[3]}, net::ProtocolVersion::modern_v4, error);
        if (payload.has_value())
            result.packets.append(payload->begin(), payload->end());
    }
    result.hash = digest.hash();
    for (std::size_t domain{}; domain < test::state_domain_count; ++domain)
        for (const auto &[key, value] : digest.facts(static_cast<test::StateDomain>(domain)))
            result.facts += std::format("{}|{}={}\n", domain, key, value);
    if (mode == HookMode::running)
        check(host.count("enemy.spawned") > 0U && host.count("block.broken") > 0U,
              "the running service did see the simulation's events");
    return result;
}

void the_hooked_simulation_matches_the_baseline()
{
    const ParityResult baseline = run_parity(HookMode::none);
    check(baseline.facts.find("enemies") != std::string::npos || !baseline.facts.empty(), "the scenario did something");
    for (const HookMode mode : {HookMode::idle, HookMode::running})
    {
        const ParityResult hooked = run_parity(mode);
        const std::string label = mode == HookMode::idle ? "idle service" : "running service, no mods";
        if (hooked.facts != baseline.facts)
        {
            std::istringstream left{baseline.facts};
            std::istringstream right{hooked.facts};
            std::string a;
            std::string b;
            while (std::getline(left, a) && std::getline(right, b))
                if (a != b)
                    throw std::runtime_error(label + ": state diverged: " + a + " vs " + b);
            throw std::runtime_error(label + ": state diverged (fact count)");
        }
        check(hooked.hash == baseline.hash, label + ": digest differs");
        check(hooked.save == baseline.save, label + ": the save bytes differ");
        check(hooked.packets == baseline.packets, label + ": the stock packet bytes differ");
    }
}

// ---------------------------------------------------------- script level

void a_survival_mod_in_luau()
{
    testing::Harness harness;
    FakeBackend backend;
    auto service = register_gameplay_api(harness.registry(), harness.host(), backend);
    backend.service = service.get();
    // The whole mod: thirst drains, an empty stat hurts, water refills it.
    const RunResult loaded = harness.run(R"(
        stats.define("thirst", { max = 100, decay = 2, hud = { label = "Thirst", color = "#3080ff" } })
        items.define("water", { name = "Water Bottle", fallback = "stick",
            use = function(player)
                if players.take(player, "water", 1) == 1 then
                    stats.add(player, "thirst", 50)
                end
            end })
        local parched = 0
        events.on("tick", function(e)
            parched += e.dt
            if parched < 1 then return end
            parched -= 1
            for _, player in players.list() do
                if stats.get(player, "thirst") <= 0 then
                    players.damage(player, 5)
                end
            end
        end)
        commands.register("drink", "A bottle of water", function(player)
            players.give(player, "water", 1)
            return "Stay hydrated."
        end)
        return true
    )", Side::server);
    check_ok(loaded, "the mod loads");
    service->start_server();
    for (int step = 0; step < 600; ++step)
    {
        harness.runtime().tick(0.1);
        service->tick(0.1F);
    }
    check(backend.health < 100.0F && backend.health > 0.0F,
          std::format("an empty stat hurts once a second ({})", backend.health));
    const Handle me = service->player_handle(0U);
    check(service->local_chat("/drink") == ChatVerdict::command, "the mod's command runs");
    const auto held = backend.local_held();
    check(held.has_value() && held->mod_item == 1U, "the bottle is in hand");
    check(!service->local_item_using(*held), "using it runs the mod instead of the stick");
    const RunResult thirst = harness.run("return stats.get(players.list()[1], 'thirst')", Side::server);
    check_ok(thirst, "the stat reads back");
    check(thirst.value.as_number() == 50.0, std::format("drinking refills ({})", thirst.value.as_number()));
    check(backend.local_count(id_of("stick"), 1U) == 0U, "and empties the bottle");
    // The client VM sees it through replication.
    check_ok(harness.run("return true", Side::client), "a client VM starts");
    service->start_client();
    service->tick(0.1F);
    const RunResult mine = harness.run("return stats.mine('thirst')", Side::client);
    check_ok(mine, "the client reads its stat");
    check(mine.value.is_number() && std::abs(mine.value.as_number() - 49.8) < 0.5,
          std::format("the replicated thirst ({})", mine.value.is_number() ? mine.value.as_number() : -1.0));
    check(!harness.run("players.damage(nil, 1)", Side::client).ok, "the client VM cannot change the game");
    static_cast<void>(me);
}

void the_api_from_luau()
{
    testing::Harness harness;
    FakeBackend backend;
    auto service = register_gameplay_api(harness.registry(), harness.host(), backend);
    backend.service = service.get();
    const RunResult result = harness.run(R"(
        blocks.define("mossy_brick", { fallback = "rock", texture = "dungeon/mossy.png" })
        structures.define("cell", {
            palette = { ["#"] = "mossy_brick", ["."] = "air" },
            layers = { { "###", "#.#", "###" }, { "###", "#.#", "###" } },
            origin = { x = 1, y = 0, z = 1 },
        })
        creatures.define("guard", { behaviour = "npc", dialogue = "guard_talk" })
        dialogue.define("guard_talk", { nodes = { hello = { text = "Halt!" } } })
        quests.define("escape", { title = "Escape", stages = { { objectives = { { type = "custom", id = "door" } } } } })
        local job = structures.place("cell", vector.create(5, 1, 5))
        local guard = entities.spawn("guard", vector.create(5.5, 1, 5.5), { name = "Guard" })
        local me = players.list()[1]
        quests.start(me, "escape")
        assert(world.get_block(vector.create(4, 1, 4)) == "test:mossy_brick")
        assert(world.get_block(vector.create(5, 1, 5)) == "air")
        assert(entities.get(guard).name == "Guard")
        assert(quests.status(me, "escape").active)
        assert(time.day() >= 1)
        return job
    )", Side::server);
    check_ok(result, "dungeon building from Luau");
    check(result.value.is_number(), "structures.place returns its job");
    check(backend.edits[{4, 1, 4}] == 6U, "the dungeon is retail rock in the world");
}

struct TestCase final
{
    const char *name;
    void (*run)();
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"hooks_are_inert_without_a_vm", hooks_are_inert_without_a_vm},
        {"definitions_and_registry_snapshot", definitions_and_registry_snapshot},
        {"world_edits_are_budgeted", world_edits_are_budgeted},
        {"mod_blocks_keep_their_identity", mod_blocks_keep_their_identity},
        {"structures_parse_rotate_and_place", structures_parse_rotate_and_place},
        {"world_generation_offers_each_chunk_once", world_generation_offers_each_chunk_once},
        {"player_spawned_waits_for_the_loaded_world", player_spawned_waits_for_the_loaded_world},
        {"stats_and_effects_replicate", stats_and_effects_replicate},
        {"damage_can_be_cancelled_and_changed", damage_can_be_cancelled_and_changed},
        {"commands_and_chat", commands_and_chat},
        {"a_joined_client_plays_through_the_host", a_joined_client_plays_through_the_host},
        {"the_mod_channel_carries_it", the_mod_channel_carries_it},
        {"creatures_behave", creatures_behave},
        {"creatures_spawn_by_rule", creatures_spawn_by_rule},
        {"quests_follow_the_player", quests_follow_the_player},
        {"the_sidecar_round_trips", the_sidecar_round_trips},
        {"hostile_client_messages_are_contained", hostile_client_messages_are_contained},
        {"mod_items_never_mix_with_retail_stacks", mod_items_never_mix_with_retail_stacks},
        {"the_hooked_simulation_matches_the_baseline", the_hooked_simulation_matches_the_baseline},
        {"a_survival_mod_in_luau", a_survival_mod_in_luau},
        {"the_api_from_luau", the_api_from_luau},
    };
    std::size_t failures{};
    for (const TestCase &test : tests)
    {
        try
        {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << " of " << tests.size() << " gameplay modding checks passed\n";
    return failures == 0U ? 0 : 1;
}
