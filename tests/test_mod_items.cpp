// (Items) Held and dropped mod items, hold styles and mod weapons
// (src/modding/gameplay/items.cpp, src/modding/graphics/held_item.cpp and the
// melee hook of net/session_authority.cpp).
//
// The definitions: `model`, `hold`, `scale` and `weapon` on items.define,
// their checks, items.get, and the registry message that hands them to every
// client. The hands: the "held" messages that tell every client what each
// player holds (the local player's hand read live), and nothing of it without
// a mod. The weapons: the server judges a claimed hit on a mod creature by the
// weapon's own damage, reach and rate, only for a player that has it in hand;
// the host's authority hurts a player by the weapon's player damage and
// judges the swing by its reach and rate. The looks: hold styles to retail
// arm clips, the grip, the item's own clips (equip, idle, use) and the eat
// and drink use, the posed mesh of the Wilderness broadsword, a dropped
// item's size.

#include "modding_test_harness.hpp"

#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/held_item.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/player_replication.hpp"
#include "openminerz/net/session_authority.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
using modding::Handle;
using modding::Side;
using modding::Value;
namespace gameplay = openminerz::modding::gameplay;
namespace graphics = openminerz::modding::graphics;

void expect(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error{std::string{message}};
}

[[nodiscard]] bool near(double value, double target, double tolerance = 1.0e-4)
{
    return std::abs(value - target) <= tolerance;
}

// ------------------------------------------------------------------- the game

// A flat world (rock below y 1), this process's player with a hand and a
// pack, remote players, and the damage the service asked for.
class FakeGame final : public gameplay::GameplayBackend
{
  public:
    std::uint8_t local_gid{};
    modding::Vec3 position{0.5, 1.0, 0.5};
    std::vector<gameplay::PlayerSnapshot> remotes;
    std::optional<gameplay::StackView> held;
    std::vector<std::pair<std::uint16_t, gameplay::StackView>> slots;
    std::map<gameplay::BlockPos, std::uint8_t> edits;
    gameplay::GameplayService *service{};

    [[nodiscard]] std::int32_t world_seed() const override
    {
        return 77;
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
        out.push_back({local_gid, "Host", true, true, position, 0.0F, 100.0F});
        out.insert(out.end(), remotes.begin(), remotes.end());
    }
    void local_set_health(float) override
    {
    }
    void local_damage(float, modding::Vec3, gameplay::DamageCause) override
    {
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
        return held;
    }
    void local_slots(std::vector<std::pair<std::uint16_t, gameplay::StackView>> &out) const override
    {
        out = slots;
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
        return std::uint8_t{cell.y < 1 ? 6U : 0U};
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
};

// ------------------------------------------------------------------- the mod

constexpr std::string_view definitions = R"(
items.define("sword", {
    name = "Test Sword", fallback = "knife", icon = "textures/sword", model = "models/sword",
    weapon = { damage = 1.5, player_damage = 26, reach = 4, cooldown = 0.5 }, stack = 1,
})
items.define("apple", { name = "Apple", fallback = "stick", icon = "textures/apple", hold = "eat", cooldown = 1.2 })
items.define("club", { name = "Club", fallback = "stick", hold = "tool", scale = 0.8, weapon = {} })
items.define("pebble", { name = "Pebble", fallback = "stick" })
creatures.define("dummy", { health = 1000, size = { 0.8, 1.8 } })
)";

modding::ModSource items_mod()
{
    modding::ModSource source;
    source.id = "items";
    source.scope = net::ModScope::server;
    source.files["shared/defs.luau"] = std::string{definitions};
    return source;
}

// One process, offline: a server VM and a client VM, joined by the
// service's own loopback, as single player and a listen host run.
struct Offline final
{
    modding::ModRuntime runtime{modding::testing::test_limits()};
    FakeGame game;
    std::unique_ptr<gameplay::GameplayService> gameplay;

    Offline()
    {
        gameplay = modding::register_gameplay_api(runtime.registry(), runtime, game);
        game.service = gameplay.get();
        runtime.set_player_key([this](Handle player) { return gameplay->player_key(player); });
        for (const Side side : {Side::server, Side::client})
        {
            std::vector<modding::ModSource> mods{items_mod()};
            expect(runtime.start(side, std::move(mods), {}), "the items mod's VM starts");
        }
        gameplay->start_server();
        gameplay->start_client();
        for (const modding::ModStatus &status : runtime.status())
            expect(status.state == modding::ModState::running, std::format("{} runs: {}", status.id, status.error));
    }
    ~Offline()
    {
        runtime.stop_all();
    }
    Offline(const Offline &) = delete;
    Offline &operator=(const Offline &) = delete;

    void run_for(double seconds)
    {
        const int steps = static_cast<int>(std::ceil(seconds * 60.0));
        for (int index = 0; index < steps; ++index)
        {
            runtime.tick(1.0 / 60.0);
            gameplay->tick(1.0F / 60.0F);
            runtime.frame(1.0 / 60.0);
        }
    }
    Value probe(std::string_view expression, Side side = Side::server)
    {
        const modding::RunResult result = runtime.run(side, "items", "probe", std::format("return {}", expression));
        expect(result.ok, std::format("probe '{}': {}", expression, result.error));
        return result.value;
    }
    [[nodiscard]] const gameplay::ClientItemType &client_type(std::string_view key) const
    {
        for (const gameplay::ClientItemType &type : gameplay->client_item_types())
            if (type.key == key)
                return type;
        throw std::runtime_error{std::format("the client knows {}", key)};
    }
};

// ------------------------------------------------------------- definitions

void definitions_carry_model_hold_scale_and_weapon()
{
    Offline world;
    const Value sword = world.probe("items.get('sword')");
    expect(sword.field("model") != nullptr && sword.field("model")->as_string() == "models/sword",
           "items.get gives the model");
    expect(sword.field("hold")->as_string() == "swing", "a weapon is held for a swing unless told otherwise");
    const Value *weapon = sword.field("weapon");
    expect(weapon != nullptr && weapon->type() == modding::ValueType::map, "items.get gives the weapon");
    expect(near(weapon->field("damage")->as_number(), 1.5) && near(weapon->field("player_damage")->as_number(), 26.0) &&
               near(weapon->field("reach")->as_number(), 4.0) && near(weapon->field("cooldown")->as_number(), 0.5),
           "the weapon's stats are what the mod said");
    expect(world.probe("items.get('pebble')").field("hold")->as_string() == "fallback",
           "anything else is held as its fallback is");
    expect(world.probe("items.get('pebble')").field("weapon") == nullptr ||
               world.probe("items.get('pebble')").field("weapon")->is_nil(),
           "and is no weapon");

    // The client half has them from the registry message.
    const gameplay::ClientItemType &client_sword = world.client_type("items:sword");
    expect(client_sword.model == "models/sword" && client_sword.hold == gameplay::HoldStyle::swing &&
               client_sword.weapon.has_value() && near(client_sword.weapon->damage, 1.5) &&
               near(client_sword.weapon->player_damage, 26.0) && near(client_sword.weapon->reach, 4.0) &&
               near(client_sword.weapon->cooldown, 0.5),
           "a client draws and swings the sword as the server defined it");
    const gameplay::ClientItemType &apple = world.client_type("items:apple");
    expect(apple.hold == gameplay::HoldStyle::eat && near(apple.cooldown, 1.2) && !apple.weapon.has_value(),
           "the apple is eaten, once in 1.2 s");
    const gameplay::ClientItemType &club = world.client_type("items:club");
    expect(club.hold == gameplay::HoldStyle::tool && near(club.scale, 0.8), "the club keeps its own hold and size");
    // weapon = {} takes the fallback's numbers: a stick strikes for 0.05,
    // 21 health points, every 0.3 s.
    expect(club.weapon.has_value() && near(club.weapon->damage, 0.05) && near(club.weapon->player_damage, 21.0) &&
               near(club.weapon->reach, 5.0) && near(club.weapon->cooldown, 0.3),
           "an empty weapon table is the fallback's own stats");
    expect(world.client_type("items:pebble").hold == gameplay::HoldStyle::fallback, "the pebble is held as a stick");
}

void definitions_refuse_bad_holds_and_weapons()
{
    Offline world;
    const auto refused = [&](std::string_view source, std::string_view says) {
        const modding::RunResult result = world.runtime.run(Side::server, "items", "bad", source);
        expect(!result.ok, std::format("refused: {}", source));
        expect(result.error.find(says) != std::string::npos,
               std::format("the error says why ({}): {}", says, result.error));
    };
    refused("items.define('a', { fallback = 'stick', hold = 'juggle' })", "hold must be");
    refused("items.define('b', { fallback = 'pistol', weapon = { damage = 3 } })", "fallback must be an item that strikes");
    refused("items.define('c', { fallback = 'dirt_block', weapon = { damage = 3 } })", "fallback must be an item that strikes");
    refused("items.define('d', { fallback = 'knife', weapon = 5 })", "weapon must be a table");
    // Out-of-range stats are held to their bounds.
    const modding::RunResult wild = world.runtime.run(
        Side::server, "items", "wild",
        "items.define('e', { fallback = 'knife', weapon = { damage = 1e9, reach = 100, cooldown = 0 }, scale = 99 })");
    expect(wild.ok, wild.error);
    const Value e = world.probe("items.get('e')");
    expect(near(e.field("weapon")->field("damage")->as_number(), 1000.0) &&
               near(e.field("weapon")->field("reach")->as_number(), 8.0) &&
               near(e.field("weapon")->field("cooldown")->as_number(), 0.1),
           "damage, reach and rate are bounded");
}

// ------------------------------------------------------------------- hands

void nothing_runs_without_a_mod()
{
    modding::ModRuntime runtime{modding::testing::test_limits()};
    FakeGame game;
    auto service = modding::register_gameplay_api(runtime.registry(), runtime, game);
    expect(!service->running(), "no mod: the service is not running");
    expect(service->client_held_item(0U) == 0U && service->client_held_item(5U) == 0U,
           "no mod: nobody holds a mod item");
    expect(!service->held_weapon(0U, static_cast<std::uint16_t>(InventoryItemId::knife)).has_value(),
           "no mod: every swing is the retail item's");
    service->tick(1.0F / 60.0F); // no messages, no reports
    expect(!service->running(), "and ticking changes nothing");
}

void every_client_learns_what_each_player_holds()
{
    Offline world;
    world.game.remotes.push_back({5U, "Joiner", false, true, {0.5, 1.0, 4.5}, 0.0F, 100.0F});
    world.run_for(0.1);
    const std::uint32_t sword = world.client_type("items:sword").id;
    expect(world.gameplay->client_held_item(5U) == 0U, "the joiner holds nothing of a mod yet");

    // The joiner's client tells the server; the server tells everyone.
    world.gameplay->receive_from_client(5U, modding::Map{{"t", Value{"held"}}, {"key", Value{"items:sword"}}});
    world.run_for(0.05);
    expect(world.gameplay->client_held_item(5U) == sword, "every client knows the joiner holds the sword");
    world.gameplay->receive_from_client(5U, modding::Map{{"t", Value{"held"}}, {"key", Value{"items:no_such"}}});
    world.run_for(0.05);
    expect(world.gameplay->client_held_item(5U) == sword, "an item the session does not define changes nothing");
    world.gameplay->receive_from_client(5U, modding::Map{{"t", Value{"held"}}, {"key", Value{""}}});
    world.run_for(0.05);
    expect(world.gameplay->client_held_item(5U) == 0U, "an empty hand clears it");

    // This process's own player: its client reports the hand as it changes.
    world.game.held = gameplay::StackView{static_cast<std::uint16_t>(InventoryItemId::knife), 1U, sword};
    world.run_for(0.05);
    expect(world.gameplay->client_held_item(world.game.local_gid) == sword, "the host's own hand reaches every client");
    world.game.held = gameplay::StackView{static_cast<std::uint16_t>(InventoryItemId::knife), 1U, 0U};
    world.run_for(0.05);
    expect(world.gameplay->client_held_item(world.game.local_gid) == 0U, "and so does a retail knife in it");

    // A player who leaves takes the hand with them (its gid may come back).
    world.gameplay->receive_from_client(5U, modding::Map{{"t", Value{"held"}}, {"key", Value{"items:sword"}}});
    world.run_for(0.05);
    expect(world.gameplay->client_held_item(5U) == sword, "the joiner holds the sword again");
    world.gameplay->remote_player_left(5U);
    world.game.remotes.clear();
    world.run_for(0.05);
    expect(world.gameplay->client_held_item(5U) == 0U, "a player who left holds nothing");
}

void the_server_believes_a_weapon_only_in_hand()
{
    Offline world;
    world.game.remotes.push_back({5U, "Joiner", false, true, {0.5, 1.0, 4.5}, 0.0F, 100.0F});
    world.run_for(0.1);
    const std::uint32_t sword = world.client_type("items:sword").id;
    const auto knife = static_cast<std::uint16_t>(InventoryItemId::knife);
    const auto stick = static_cast<std::uint16_t>(InventoryItemId::stick);

    // The local player: read live.
    world.game.held = gameplay::StackView{knife, 1U, sword};
    const auto local = world.gameplay->held_weapon(world.game.local_gid, knife);
    expect(local.has_value() && near(local->damage, 1.5) && near(local->player_damage, 26.0),
           "the host's own player swings the sword in its hand");
    expect(!world.gameplay->held_weapon(world.game.local_gid, stick).has_value(),
           "a stock swing of another item is not the sword's");
    world.game.held = gameplay::StackView{knife, 1U, 0U};
    expect(!world.gameplay->held_weapon(world.game.local_gid, knife).has_value(), "a retail knife is a retail knife");

    // A remote player: announced, and in its last inventory report.
    world.gameplay->receive_from_client(5U, modding::Map{{"t", Value{"held"}}, {"key", Value{"items:sword"}}});
    expect(!world.gameplay->held_weapon(5U, knife).has_value(),
           "announced but in no inventory report: not believed");
    world.gameplay->receive_from_client(
        5U, modding::Map{{"t", Value{"inventory"}}, {"counts", Value{modding::Map{{"items:sword", Value{std::int64_t{1}}}}}}});
    expect(world.gameplay->held_weapon(5U, knife).has_value(), "announced and carried: the sword");
    expect(!world.gameplay->held_weapon(5U, stick).has_value(), "but only while the stock hand holds its knife");
}

void mod_weapon_hits_are_judged_by_the_weapon()
{
    Offline world;
    world.game.position = {0.5, 1.0, -40.5};
    world.game.remotes.push_back({5U, "Joiner", false, true, {0.5, 1.0, 4.5}, 0.0F, 100.0F});
    world.run_for(0.1);
    const Value spawned = world.probe("entities.spawn('dummy', vector.create(0.5, 1, 7.5), { behaviour = 'idle' })");
    expect(spawned.type() == modding::ValueType::handle, "the dummy stands three blocks from the joiner");
    const std::int64_t dummy = static_cast<std::int64_t>(spawned.as_handle().id);
    world.run_for(0.2);
    const auto health = [&] {
        return world.probe(std::format(
                               "(function() for _, e in entities.list() do if e.id == {} then return entities.get(e).health end end end)()",
                               dummy))
            .as_number();
    };
    const auto knife = static_cast<std::int64_t>(InventoryItemId::knife);
    const auto claim = [&](std::string_view key, double damage) {
        modding::Map attack{{"t", Value{"attack"}},
                            {"entity", Value{dummy}},
                            {"damage", Value{damage}},
                            {"weapon", Value{knife}},
                            {"dir", Value{modding::Vec3{0.0, -0.15, 1.0}}}};
        if (!key.empty())
            attack.emplace_back("key", Value{std::string{key}});
        world.gameplay->receive_from_client(5U, Value{std::move(attack)});
        world.run_for(0.6); // the sword's rate, and the attack budget
    };

    double before = health();
    claim("", 10.0);
    expect(near(before - health(), 0.5, 0.01), "a retail knife hit is held to the knife's 0.5");

    before = health();
    claim("items:sword", 10.0);
    expect(near(before - health(), 0.0), "a sword hit from a player not holding one is refused");

    world.gameplay->receive_from_client(5U, modding::Map{{"t", Value{"held"}}, {"key", Value{"items:sword"}}});
    world.gameplay->receive_from_client(
        5U, modding::Map{{"t", Value{"inventory"}}, {"counts", Value{modding::Map{{"items:sword", Value{std::int64_t{1}}}}}}});
    before = health();
    claim("items:sword", 10.0);
    expect(near(before - health(), 1.5, 0.02), "the sword's hit is held to the sword's 1.5 - not the knife's");
    before = health();
    claim("items:sword", 1.0);
    expect(near(before - health(), 1.0, 0.02), "a smaller claim is what it says");
    before = health();
    claim("items:club", 10.0);
    expect(near(before - health(), 0.0), "a hit claimed with another weapon than the one in hand is refused");

    // Its reach is 4 blocks: from six away it cannot have struck.
    world.game.remotes.front().position = {0.5, 1.0, -1.5};
    world.run_for(0.2);
    before = health();
    claim("items:sword", 1.0);
    expect(near(before - health(), 0.0), "a sword hit from beyond its reach (and the lag slack) is refused");
    world.game.remotes.front().position = {0.5, 1.0, 4.5};
    world.run_for(0.2);

    // Its rate: one swing in 0.5 s, a couple bunched by lag.
    before = health();
    for (int swing = 0; swing < 12; ++swing)
    {
        modding::Map attack{{"t", Value{"attack"}},         {"entity", Value{dummy}},
                            {"damage", Value{1.5}},         {"weapon", Value{knife}},
                            {"dir", Value{modding::Vec3{0.0, -0.15, 1.0}}}, {"key", Value{"items:sword"}}};
        world.gameplay->receive_from_client(5U, Value{std::move(attack)});
        world.run_for(1.0 / 60.0);
    }
    world.run_for(0.1);
    const double landed = (before - health()) / 1.5;
    expect(landed >= 2.0 && landed <= 4.5, std::format("twelve swings in 0.3 s land a burst's worth, not all ({:.1f})", landed));
}

// --------------------------------------------------------------- authority

void the_authority_hurts_by_the_mod_weapon()
{
    using net::AuthorityHost;
    net::AuthorityRules rules;
    rules.protocol = net::ProtocolVersion::modern_v4;
    rules.mode = GameMode::survival;
    rules.pvp = net::PlayerVersusPlayer::everyone;
    rules.strict = false;
    AuthorityHost authority{rules};
    double now = 1.0;
    authority.add_player(0U, now, true);
    authority.add_player(5U, now);
    authority.add_player(6U, now);
    const auto place = [&](std::uint8_t gid, world::Vec3 at) {
        net::LocalPlayerReplicationInput input;
        input.position = at;
        net::PlayerUpdatePayload update;
        net::PayloadError error{};
        expect(net::make_player_update(input, rules.protocol, update, error), "a player update");
        static_cast<void>(authority.observe_player_update(gid, update, now));
    };
    place(5U, {0.5F, 1.0F, 0.5F});
    place(6U, {0.5F, 1.0F, 2.5F});
    authority.observe_carried_item(5U, InventoryItemId::knife, now);
    authority.observe_carried_item(6U, InventoryItemId::knife, now);
    // Player 5 holds a mod weapon: 26 points, reach 4 (players: 2), one swing a second.
    authority.set_melee_weapon([](std::uint8_t gid, InventoryItemId item) -> std::optional<AuthorityHost::MeleeWeapon> {
        if (gid == 5U && item == InventoryItemId::knife)
            return AuthorityHost::MeleeWeapon{26.0F, 4.0F, 1.0F};
        return std::nullopt;
    });
    std::int16_t wire{};
    expect(net::inventory_item_to_wire(InventoryItemId::knife, rules.protocol, wire), "the knife's wire id");
    net::MeleePlayerPayload melee;
    melee.item_id = wire;
    melee.damage_source = {0.5F, 2.0F, 2.5F};

    now += 0.5;
    expect(authority.observe_melee(5U, 6U, melee, now) == net::AuthorityVerdict::accept, "the sword's swing lands");
    expect(near(authority.player(6U)->health, 74.0, 0.01), "and hurts by the sword's 26, not the knife's 21");
    now += 0.5;
    melee.damage_source = {0.5F, 2.0F, 0.5F};
    expect(authority.observe_melee(6U, 5U, melee, now) == net::AuthorityVerdict::accept, "a retail knife's swing lands");
    expect(near(authority.player(5U)->health, 79.0, 0.01), "and hurts by the knife's 21");

    // Reach: the knife reaches players 2.5 (+3 slack), this sword 2 (+3).
    place(6U, {0.5F, 1.0F, 5.9F});
    now += 2.0;
    melee.damage_source = {0.5F, 2.0F, 5.9F};
    expect(authority.observe_melee(5U, 6U, melee, now) == net::AuthorityVerdict::refuse,
           "5.4 blocks is out of the sword's reach");
    expect(authority.observe_melee(6U, 5U, melee, now) == net::AuthorityVerdict::accept,
           "but within the knife's");

    // Rate: one sword swing a second (a burst of four), the knife two a second.
    place(6U, {0.5F, 1.0F, 2.0F});
    now += 5.0;
    int sword_landed{};
    for (int swing = 0; swing < 10; ++swing)
    {
        now += 0.55;
        if (authority.observe_melee(5U, 6U, melee, now) == net::AuthorityVerdict::accept)
            ++sword_landed;
        authority.heal(6U, 100.0F, now);
    }
    expect(sword_landed >= 5 && sword_landed < 10,
           std::format("a swing every 0.55 s with a one-second sword: some refused ({})", sword_landed));
}

// ----------------------------------------------------------------- looks

void hold_styles_play_the_retail_arm_clips()
{
    using graphics::HoldStyle;
    const auto knife = InventoryItemId::knife;
    expect(graphics::hold_animation_mode(HoldStyle::swing, InventoryItemId::stick) == PlayerAnimationMode::pick &&
               graphics::hold_animation_mode(HoldStyle::tool, InventoryItemId::stick) == PlayerAnimationMode::pick,
           "swing and tool: PickIdle / PickUse");
    expect(graphics::hold_animation_mode(HoldStyle::item, knife) == PlayerAnimationMode::generic,
           "item: GenericIdle / GenericUse");
    expect(graphics::hold_animation_mode(HoldStyle::eat, knife) == PlayerAnimationMode::block &&
               graphics::hold_animation_mode(HoldStyle::drink, knife) == PlayerAnimationMode::block &&
               graphics::hold_animation_mode(HoldStyle::block, knife) == PlayerAnimationMode::block,
           "eat, drink and block: BlockIdle / BlockUse");
    expect(graphics::hold_animation_mode(HoldStyle::fist, knife) == PlayerAnimationMode::fist, "fist: FistIdle");
    expect(graphics::hold_animation_mode(HoldStyle::fallback, InventoryItemId::assault_rifle) ==
               item_player_animation_mode(InventoryItemId::assault_rifle),
           "fallback: the fallback's own clips");
    expect(graphics::resolved_hold(HoldStyle::fallback, knife) == HoldStyle::tool &&
               graphics::resolved_hold(HoldStyle::fallback, InventoryItemId::dirt_block) == HoldStyle::block &&
               graphics::resolved_hold(HoldStyle::fallback, InventoryItemId::bare_hands) == HoldStyle::fist &&
               graphics::resolved_hold(HoldStyle::fallback, InventoryItemId::compass) == HoldStyle::item &&
               graphics::resolved_hold(HoldStyle::eat, knife) == HoldStyle::eat,
           "a fallback hold is drawn like the fallback's clips");
    expect(graphics::grip_reference_item(HoldStyle::swing) == InventoryItemId::knife &&
               graphics::grip_reference_item(HoldStyle::eat) == InventoryItemId::dirt_block,
           "the fist grips like the knife, the front holds like a block");
    for (std::size_t index = 0U; index < gameplay::hold_style_names.size(); ++index)
        expect(gameplay::hold_style_from_name(gameplay::hold_style_names[index]) == static_cast<HoldStyle>(index) &&
                   gameplay::hold_style_name(static_cast<HoldStyle>(index)) == gameplay::hold_style_names[index],
               "hold style names round-trip");
    // The eat/drink use: up to the mouth, bites there, back down.
    expect(near(graphics::mouth_clip_fraction(0.0F), 0.0) && near(graphics::mouth_clip_fraction(1.0F), 1.0),
           "the use clip starts and ends where BlockUse does");
    for (const float progress : {0.25F, 0.4F, 0.55F, 0.7F})
        expect(graphics::mouth_clip_fraction(progress) <= 0.2F && graphics::mouth_clip_fraction(progress) >= 0.13F,
               "the hand stays up at the mouth through the bites");
}

// Frames of 50 ms (the animator takes at most a quarter second a frame).
void run(graphics::HeldItemAnimator &animator, std::uint32_t item, const graphics::ItemLook &look, float seconds,
         float use_seconds = 1.0F)
{
    for (float done = 0.0F; done + 1.0e-4F < seconds; done += 0.05F)
        animator.update(item, look, false, use_seconds, std::min(0.05F, seconds - done));
}

[[nodiscard]] std::shared_ptr<const graphics::Model> broadsword()
{
    std::string error;
    const auto model = graphics::load_model(std::filesystem::path{OPENMINERZ_SOURCE_DIR} / "examples" / "mods" /
                                                "wilderness" / "assets" / "models" / "weapons" / "iron_broadsword.gltf",
                                            &error);
    expect(model != nullptr, std::format("the Wilderness broadsword loads: {}", error));
    return model;
}

void the_broadsword_reads_as_a_sword()
{
    const auto model = broadsword();
    const graphics::Vec3 size = model->bounds_max - model->bounds_min;
    // Blade up +Y from the fist; flat towards the holder (thin in Z); guard across X.
    expect(model->bounds_max.y > 0.45F && model->bounds_max.y < 0.6F && model->bounds_min.y > -0.12F &&
               model->bounds_min.y < -0.08F,
           "the blade runs up from the fist, the pommel just below it");
    expect(size.x > 0.15F && size.x < 0.22F && size.z < 0.06F, "the guard spans X, the flat faces Z");
    expect(model->materials.size() == 7U, "steel, edge, fuller, brass, leather, wrap and iron");
    expect(model->triangle_count > 100U && model->triangle_count < 1500U,
           std::format("low poly ({} triangles)", model->triangle_count));
    for (const std::string_view clip : {"idle", "equip", "use"})
        expect(graphics::find_state_clip(*model, clip) >= 0, std::format("it has its {} clip", clip));

    // Posed: wound like the retail models (clockwise from the front), so the
    // normal of each triangle's corners points away from its winding's.
    std::vector<graphics::PosedItemPart> parts;
    graphics::ItemPoseScratch scratch;
    graphics::pose_item_model(*model, {}, parts, scratch);
    std::size_t triangles{};
    std::size_t clockwise{};
    for (const graphics::PosedItemPart &part : parts)
        for (std::size_t corner = 0U; corner + 2U < part.vertices.size(); corner += 3U)
        {
            const graphics::Vec3 a = part.vertices[corner].position;
            const graphics::Vec3 b = part.vertices[corner + 1U].position;
            const graphics::Vec3 c = part.vertices[corner + 2U].position;
            const graphics::Vec3 u = b - a;
            const graphics::Vec3 v = c - a;
            const graphics::Vec3 face{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
            const graphics::Vec3 normal = part.vertices[corner].normal;
            ++triangles;
            if (face.x * normal.x + face.y * normal.y + face.z * normal.z < 0.0F)
                ++clockwise;
        }
    expect(triangles == model->triangle_count && clockwise == triangles,
           std::format("every triangle is turned clockwise ({} of {})", clockwise, triangles));

    // Its clips move it: mid-use the tip is somewhere else.
    graphics::HeldItemAnimator animator;
    graphics::ItemLook look;
    look.model = model;
    look.hold = graphics::HoldStyle::swing;
    animator.update(7U, look, false, 1.0F, 0.0F);
    run(animator, 7U, look, 1.0F); // the equip is over: idle
    const graphics::HeldItemPose idle = animator.pose();
    expect(idle.layer_count >= 1U && idle.layers[0].animation == graphics::find_state_clip(*model, "idle"),
           "after its equip the sword idles");
    animator.update(7U, look, true, 1.0F, 0.0F);
    run(animator, 7U, look, 0.25F);
    const graphics::HeldItemPose swing = animator.pose();
    expect(swing.layers[0].animation == graphics::find_state_clip(*model, "use") && !swing.layers[0].loop,
           "a use plays the use clip once");
    const auto tip = [&](const graphics::HeldItemPose &pose) {
        graphics::pose_item_model(*model, {pose.layers.data(), pose.layer_count}, parts, scratch);
        graphics::Vec3 highest{0.0F, -1.0F, 0.0F};
        for (const graphics::PosedItemPart &part : parts)
            for (const graphics::PosedItemVertex &vertex : part.vertices)
                if (vertex.position.y > highest.y)
                    highest = vertex.position;
        return highest;
    };
    const graphics::Vec3 rest = tip(idle);
    const graphics::Vec3 struck = tip(swing);
    expect(std::abs(rest.x - struck.x) > 0.1F, "mid-use the blade has swept across");
    run(animator, 7U, look, 1.0F);
    expect(animator.pose().layers[0].animation == graphics::find_state_clip(*model, "idle"),
           "after the use it idles again");
    // Another item starts over with its own equip; nothing of a mod in hand
    // clears the clips.
    animator.update(8U, look, false, 1.0F, 0.0F);
    expect(animator.pose().layers[0].animation == graphics::find_state_clip(*model, "equip"),
           "a new item comes in with its equip");
    animator.update(0U, {}, false, 1.0F, 0.1F);
    expect(animator.pose().layer_count == 0U, "an empty hand has no clips");

    // Dropped: no larger than a pickup, centred on it.
    const graphics::Matrix4 dropped = graphics::dropped_item_transform(look, {10.0F, 5.0F, 3.0F}, 0.7F);
    graphics::Vec3 low{1.0e9F, 1.0e9F, 1.0e9F};
    graphics::Vec3 high{-1.0e9F, -1.0e9F, -1.0e9F};
    graphics::pose_item_model(*model, {}, parts, scratch);
    for (const graphics::PosedItemPart &part : parts)
        for (const graphics::PosedItemVertex &vertex : part.vertices)
        {
            const graphics::Vec3 at = graphics::transform_point(dropped, vertex.position);
            low = {std::min(low.x, at.x), std::min(low.y, at.y), std::min(low.z, at.z)};
            high = {std::max(high.x, at.x), std::max(high.y, at.y), std::max(high.z, at.z)};
        }
    expect(high.y - low.y <= 0.61F && high.y - low.y > 0.5F, "a dropped sword stands 0.6 tall");
    expect(near((high.y + low.y) * 0.5F, 5.0, 0.01), "on the pickup's centre");
}

void eat_and_drink_go_to_the_mouth()
{
    graphics::HeldItemAnimator animator;
    graphics::ItemLook apple;
    apple.icon = std::make_shared<const graphics::IconModel>();
    apple.hold = graphics::HoldStyle::eat;
    animator.update(3U, apple, false, 1.2F, 0.0F);
    expect(animator.pose().use_progress == 0.0F, "at rest the apple is held out");
    animator.update(3U, apple, true, 1.2F, 0.0F);
    run(animator, 3U, apple, 0.6F, 1.2F);
    expect(near(animator.pose().use_progress, 0.5, 0.01), "half a bite in, half way through");
    run(animator, 3U, apple, 0.7F, 1.2F);
    expect(animator.pose().use_progress == 0.0F, "after its cooldown the use is over");

    // At the mouth the item turns and bobs; at rest it does not move.
    const graphics::Matrix4 rest = graphics::held_item_transform(apple, 0.0F, 1.0F, true);
    const graphics::Matrix4 bite = graphics::held_item_transform(apple, 0.5F, 1.0F, true);
    expect(rest != bite, "the bites move it");
    graphics::ItemLook bottle = apple;
    bottle.hold = graphics::HoldStyle::drink;
    const graphics::Matrix4 sip = graphics::held_item_transform(bottle, 0.5F, 1.0F, true);
    const graphics::Vec3 up = graphics::transform_direction(sip, {0.0F, 1.0F, 0.0F});
    const graphics::Vec3 upright = graphics::transform_direction(rest, {0.0F, 1.0F, 0.0F});
    const float tilt = (up.x * upright.x + up.y * upright.y + up.z * upright.z) /
                       std::sqrt((up.x * up.x + up.y * up.y + up.z * up.z) *
                                 (upright.x * upright.x + upright.y * upright.y + upright.z * upright.z));
    expect(tilt < 0.8F, "a drink tips the bottle up");

    // The grip undoes the reference's scale and applies the item's own.
    graphics::ItemLook sword;
    sword.hold = graphics::HoldStyle::swing;
    sword.scale = 1.0F;
    const graphics::Matrix4 one = graphics::held_item_transform(sword, 0.0F, 1.0F, false);
    const graphics::Matrix4 two = graphics::held_item_transform(sword, 0.0F, 2.0F, false);
    const graphics::Vec3 a = graphics::transform_direction(one, {0.0F, 1.0F, 0.0F});
    const graphics::Vec3 b = graphics::transform_direction(two, {0.0F, 1.0F, 0.0F});
    expect(near(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z), 1.0, 1.0e-4) &&
               near(std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z), 0.5, 1.0e-4),
           "a reference hand twice the size halves the item to keep its size");
    // A sword sprite is held a fifth of the way up its diagonal.
    const graphics::Matrix4 sprite = graphics::held_item_transform(sword, 0.0F, 1.0F, true);
    const graphics::Vec3 grip = graphics::transform_point(sprite, {-0.3F, -0.3F, 0.0F});
    const graphics::Vec3 origin = graphics::transform_point(one, {0.0F, 0.0F, 0.0F});
    expect(near(grip.x, origin.x, 1.0e-3) && near(grip.y, origin.y, 1.0e-3) && near(grip.z, origin.z, 1.0e-3),
           "the icon's lower-left part sits in the fist");
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"definitions_carry_model_hold_scale_and_weapon", definitions_carry_model_hold_scale_and_weapon},
        {"definitions_refuse_bad_holds_and_weapons", definitions_refuse_bad_holds_and_weapons},
        {"nothing_runs_without_a_mod", nothing_runs_without_a_mod},
        {"every_client_learns_what_each_player_holds", every_client_learns_what_each_player_holds},
        {"the_server_believes_a_weapon_only_in_hand", the_server_believes_a_weapon_only_in_hand},
        {"mod_weapon_hits_are_judged_by_the_weapon", mod_weapon_hits_are_judged_by_the_weapon},
        {"the_authority_hurts_by_the_mod_weapon", the_authority_hurts_by_the_mod_weapon},
        {"hold_styles_play_the_retail_arm_clips", hold_styles_play_the_retail_arm_clips},
        {"the_broadsword_reads_as_a_sword", the_broadsword_reads_as_a_sword},
        {"eat_and_drink_go_to_the_mouth", eat_and_drink_go_to_the_mouth},
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
    std::cout << passed << " of " << tests.size() << " held item checks passed\n";
    return passed == static_cast<int>(tests.size()) ? 0 : 1;
}
