// The modding SDK: the example mods in examples/mods/, the templates of
// `openminerz_mod_sdk new` and the validator.
//
// Every example is validated exactly as `openminerz_mod_sdk validate --strict`
// does, then loaded into the SDK's headless session (tools/ModSdk/
// sdk_session.hpp: every area's real service on stand-in backends, a server
// VM and a client VM joined by the real mod channel) and played for a while:
// no script may log an error, and what each example promises is checked
// (hunger drains and food fills it, the crypt is built and its quest can be
// finished, animals spawn, the theme and HUD widget exist, torches get lights).
// All of them run together too, under both rule sets. (mod-world) Frontier's
// world generator is built from its scripts, places its biomes and blocks,
// and its landmarks rise where worldgen.chunk offers its biomes.

#include "sdk_session.hpp"
#include "templates.hpp"
#include "validate.hpp"

#include "openminerz/modding/animation_authoring.hpp" // (GUNPACK)
#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/graphics.hpp" // (GUNPACK)
#include "openminerz/modding/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace openminerz;
using namespace openminerz::mod_sdk;
using modding::Side;

namespace
{

namespace fs = std::filesystem;

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

fs::path examples_root()
{
    return fs::path{OPENMINERZ_SOURCE_DIR} / "examples" / "mods";
}

// A temporary folder removed when the test ends.
class Scratch final
{
  public:
    explicit Scratch(std::string_view name)
        : path_{fs::temp_directory_path() / std::format("openminerz-mod-examples-{}", name)}
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
        fs::create_directories(path_);
    }
    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;
    [[nodiscard]] const fs::path &path() const noexcept
    {
        return path_;
    }

  private:
    fs::path path_;
};

void write(const fs::path &file, std::string_view text)
{
    fs::create_directories(file.parent_path());
    std::ofstream{file, std::ios::binary} << text;
}

net::LoadedMod load(const fs::path &folder)
{
    net::LoadedMod mod;
    std::string error;
    check(load_mod_folder(folder, mod, error), folder.generic_string() + ": " + error);
    return mod;
}

std::vector<net::LoadedMod> load_examples(std::initializer_list<std::string_view> names)
{
    std::vector<net::LoadedMod> mods;
    for (const std::string_view name : names)
        mods.push_back(load(examples_root() / name));
    std::stable_sort(mods.begin(), mods.end(),
                     [](const net::LoadedMod &a, const net::LoadedMod &b) { return a.load_order < b.load_order; });
    return mods;
}

std::string errors_of(const Sandbox &sandbox)
{
    std::string text;
    for (const modding::LogEntry &entry : sandbox.log())
        if (entry.level >= modding::LogLevel::warning)
            text += std::format("  [{}] {}: {}\n", modding::side_name(entry.side), entry.mod_id, entry.message);
    return text;
}

// Every mod still running, and nothing logged at warning level or above.
void check_clean(Sandbox &sandbox, const std::string &what)
{
    const std::string problems = errors_of(sandbox);
    check(problems.empty(), what + " logged problems:\n" + problems);
    for (const modding::ModStatus &status : sandbox.runtime().status())
        check(status.state == modding::ModState::running,
              std::format("{}: {} on the {} is not running: {}", what, status.id, modding::side_name(status.side),
                          status.error));
}

void start(Sandbox &sandbox, const std::vector<net::LoadedMod> &mods)
{
    std::string error;
    check(sandbox.start(mods, error), "the session starts: " + error);
    check(sandbox.refused().empty(), "no mod is refused");
}

modding::RunResult server(Sandbox &sandbox, std::string_view mod, std::string_view code)
{
    modding::RunResult result = sandbox.runtime().run(Side::server, mod, "test", code);
    check(result.ok, std::format("server chunk failed: {}", result.error));
    return result;
}

// The slot of the player's inventory that holds a mod item, selected.
bool hold(Sandbox &sandbox, std::string_view key)
{
    std::uint32_t id{};
    for (const modding::gameplay::ClientItemType &type : sandbox.gameplay().client_item_types())
        if (type.key == key)
            id = type.id;
    PlayerInventory &inventory = sandbox.world().inventory;
    for (std::size_t index = 0U; index < PlayerInventory::tray_size; ++index)
        if (inventory.tray()[index].has_value() && inventory.tray()[index]->mod_item == id)
        {
            inventory.select(index);
            return true;
        }
    return false;
}

std::uint32_t count_of(Sandbox &sandbox, std::string_view key)
{
    for (const modding::gameplay::ClientItemType &type : sandbox.gameplay().client_item_types())
        if (type.key == key)
            return sandbox.world().local_count(type.fallback, type.id);
    return 0U;
}

double stat(Sandbox &sandbox, std::string_view name)
{
    for (const modding::gameplay::ClientStat &value : sandbox.gameplay().client_stats())
        if (value.name == name)
            return value.value;
    throw std::runtime_error(std::format("no replicated stat '{}'", name));
}

// ------------------------------------------------------------------- tests

void every_example_validates_strictly()
{
    Sandbox sandbox;
    std::size_t examples{};
    for (const fs::directory_entry &entry : fs::directory_iterator{examples_root()})
    {
        if (!entry.is_directory())
            continue;
        ++examples;
        const ValidationResult result = validate_mod(entry.path(), sandbox.registry());
        check(result.ok() && result.diagnostics.empty(),
              entry.path().filename().string() + " does not validate cleanly:\n" + format_diagnostics(result));
    }
    // (Animation packs) six with examples/mods/animation_pack; (mod-world)
    // seven with examples/mods/frontier; (mod-ai) eight with
    // examples/mods/bestiary; (wave 5) nine with examples/mods/wilderness;
    // (GUNPACK) ten with examples/mods/tactical_animations.
    check(examples == 10U, std::format("ten examples ({})", examples));
}

void survival_needs_food_and_water()
{
    Sandbox sandbox;
    start(sandbox, load_examples({"survival"}));
    sandbox.run(1.0);
    check(count_of(sandbox, "survival:canteen") == 1U, "a new player gets a canteen");
    check(count_of(sandbox, "survival:berries") == 4U, "and four berries");
    const auto bars = sandbox.gameplay().client_stats();
    check(std::count_if(bars.begin(), bars.end(), [](const auto &bar) { return bar.hud; }) == 2,
          "hunger and thirst reach the client as HUD bars");
    check(sandbox.report().hud_commands > 0U, "the UI draws them");

    sandbox.run(60.0);
    const double hunger = stat(sandbox, "hunger");
    const double thirst = stat(sandbox, "thirst");
    check(hunger < 93.0 && hunger > 89.0, std::format("hunger drains about 1/7.2 a second ({})", hunger));
    check(thirst < 89.0, std::format("thirst drains faster ({})", thirst));

    // The spawn pond is next to the player: the canteen fills there.
    server(sandbox, "survival", "stats.set(players.list()[1], 'thirst', 40)");
    sandbox.run(0.5);
    const double dry = stat(sandbox, "thirst");
    check(hold(sandbox, "survival:canteen"), "the canteen is in the tray");
    check(!sandbox.gameplay().local_item_using(*sandbox.world().local_held()), "using it runs the mod");
    sandbox.run(0.5);
    check(count_of(sandbox, "survival:canteen_full") == 1U && count_of(sandbox, "survival:canteen") == 0U,
          "next to fresh water the canteen fills");
    check(stat(sandbox, "thirst") > dry + 20.0,
          std::format("and the player drinks ({} -> {})", dry, stat(sandbox, "thirst")));

    server(sandbox, "survival", "stats.set(players.list()[1], 'hunger', 40)");
    sandbox.run(0.5);
    check(hold(sandbox, "survival:berries"), "berries in the tray");
    const double before = stat(sandbox, "hunger");
    static_cast<void>(sandbox.gameplay().local_item_using(*sandbox.world().local_held()));
    sandbox.run(0.5);
    check(stat(sandbox, "hunger") > before + 10.0, "berries fill hunger");
    check(count_of(sandbox, "survival:berries") == 3U, "and are eaten");

    check(sandbox.chat("/drink") == modding::gameplay::ChatVerdict::command, "/drink is a command");

    // Far from water the canteen stays empty, unless the crosshair is on water.
    check(hold(sandbox, "survival:canteen_full"), "the full canteen is in the tray");
    static_cast<void>(sandbox.gameplay().local_item_using(*sandbox.world().local_held()));
    sandbox.run(0.5);
    check(count_of(sandbox, "survival:canteen") == 1U, "drinking it gives the empty canteen back");
    const modding::Vec3 pond = server(sandbox, "survival", "return storage.world.get('spawn_pond')").value.as_vec3();
    sandbox.world().position = {pond.x + 30.0, 1.0, pond.z};
    sandbox.run(0.5);
    check(hold(sandbox, "survival:canteen"), "the empty canteen is in the tray");
    static_cast<void>(sandbox.gameplay().local_item_using(*sandbox.world().local_held()));
    sandbox.run(0.5);
    check(count_of(sandbox, "survival:canteen") == 1U, "far from water nothing fills");
    sandbox.world().aimed = modding::gameplay::BlockPos{static_cast<int>(pond.x), static_cast<int>(pond.y),
                                                        static_cast<int>(pond.z)};
    static_cast<void>(sandbox.gameplay().local_item_using(*sandbox.world().local_held()));
    sandbox.run(0.5);
    sandbox.world().aimed.reset();
    check(count_of(sandbox, "survival:canteen_full") == 1U, "aimed at the pond, the canteen fills");

    // Empty stomach: the player loses health once a second.
    server(sandbox, "survival", "stats.set(players.list()[1], 'hunger', 0)");
    const float health = sandbox.world().health;
    sandbox.run(3.0);
    check(sandbox.world().health < health - 5.0F, std::format("starving hurts ({} -> {})", health,
                                                             sandbox.world().health));
    check_clean(sandbox, "survival");
}

void the_crypt_quest_can_be_finished()
{
    Sandbox sandbox;
    start(sandbox, load_examples({"story_dungeon"}));
    sandbox.run(8.0);
    // Crypt bricks fall back to rock, so only the cells whose retail block
    // changed (carved air, dirt turned to brick) are world edits.
    check(sandbox.world().edits.size() > 1000U &&
              std::any_of(sandbox.log().begin(), sandbox.log().end(),
                          [](const modding::LogEntry &entry) {
                              return entry.message.find("the crypt is built") != std::string::npos;
                          }),
          std::format("the crypt is built as world edits ({})", sandbox.world().edits.size()));
    const SandboxReport report = sandbox.report();
    check(report.atmosphere_layers == 1U, "the client pushes the crypt's atmosphere layer");
    check(report.lights == 9U, std::format("and its nine torch lights ({})", report.lights));
    const auto &quests = sandbox.gameplay().client_quests();
    check(quests.size() == 1U && quests.front().id == "crypt" && !quests.front().completed,
          "the quest is in the client's journal");

    // Talk to the hermit: look at him and press interact.
    const auto find = [&](std::string_view kind) -> std::optional<modding::gameplay::ClientEntity> {
        for (const modding::gameplay::ClientEntity &entity : sandbox.gameplay().client_entities())
            if (entity.kind == kind)
                return entity;
        return std::nullopt;
    };
    const auto talk = [&] {
        const std::optional<modding::gameplay::ClientEntity> hermit = find("story_dungeon:hermit");
        check(hermit.has_value(), "the hermit is there");
        const modding::Vec3 eye{sandbox.world().position.x, sandbox.world().position.y + 1.6,
                                sandbox.world().position.z};
        const modding::Vec3 target{hermit->position.x, hermit->position.y + 1.0, hermit->position.z};
        check(sandbox.gameplay().local_interact(eye, {target.x - eye.x, target.y - eye.y, target.z - eye.z}, 12.0F),
              "the hermit takes the interaction");
        sandbox.run(0.5);
    };
    talk();
    check(sandbox.gameplay().client_dialogue().has_value(), "the hermit speaks");
    check(sandbox.gameplay().client_quests().front().stage.find("ghoul") != std::string::npos,
          "the quest moves on to the ghouls: " + sandbox.gameplay().client_quests().front().stage);
    sandbox.run(1.0);
    std::size_t ghouls{};
    for (const auto &entity : sandbox.gameplay().client_entities())
        ghouls += entity.kind == "story_dungeon:crypt_ghoul" ? 1U : 0U;
    check(ghouls == 4U, std::format("four ghouls rise in the crypt ({})", ghouls));

    // Lay them to rest (the player strikes the killing blows).
    server(sandbox, "story_dungeon", R"(
        local me = players.list()[1]
        for _, ghoul in entities.list({ kind = "story_dungeon:crypt_ghoul" }) do
            entities.damage(ghoul, 100, me)
        end
    )");
    sandbox.run(1.0);
    check(sandbox.gameplay().client_quests().front().stage.find("Return") != std::string::npos,
          "then back to the hermit: " + sandbox.gameplay().client_quests().front().stage);
    talk();
    sandbox.run(0.5);
    check(sandbox.gameplay().client_quests().front().completed, "the quest is complete");
    check(count_of(sandbox, "story_dungeon:hermit_charm") == 1U, "the charm is the reward");
    check_clean(sandbox, "story_dungeon");
}

void wildlife_spawns_and_is_wary()
{
    Sandbox sandbox;
    start(sandbox, load_examples({"wildlife"}));
    sandbox.run(90.0);
    std::size_t animals{};
    for (const auto &entity : sandbox.gameplay().client_entities())
        animals += entity.kind.starts_with("wildlife:") ? 1U : 0U;
    check(animals > 0U, "animals spawn around the player by their rules");
    check(sandbox.chat("/wildlife") == modding::gameplay::ChatVerdict::command, "/wildlife counts them");
    // Hunting: a dead deer drops venison as pickups.
    const std::size_t before = sandbox.world().pickups.size();
    server(sandbox, "wildlife", R"(
        local deer = entities.spawn("wildlife:deer", players.position(players.list()[1]) + vector.create(3, 0, 0))
        entities.damage(deer, 100, players.list()[1])
    )");
    sandbox.run(0.5);
    check(sandbox.world().pickups.size() > before, "a deer drops meat");
    check_clean(sandbox, "wildlife");
}

void the_theme_restyles_and_adds_a_clock()
{
    Sandbox sandbox;
    start(sandbox, load_examples({"ui_theme"}));
    sandbox.run(1.0);
    const modding::ui::ThemeStack &theme = sandbox.ui().theme();
    check(!theme.empty(), "ui.theme took");
    check(theme.color(modding::ui::ColorKey::menu_focus, modding::ui::Style::remastered) ==
              modding::ui::Color{255U, 176U, 64U, 255U},
          "the focus ink is amber");
    check(theme.color(modding::ui::ColorKey::menu_focus, modding::ui::Style::classic) ==
              modding::ui::Color{255U, 200U, 96U, 255U},
          "and the Classic line wins in Classic");
    check(theme.font(modding::ui::Face::display, modding::ui::Style::remastered) != nullptr, "a display font");
    check(theme.image(modding::ui::ImageKey::button, modding::ui::Style::remastered) != nullptr, "a button image");
    check(sandbox.ui().hud_roots().size() == 1U && sandbox.report().hud_commands > 0U, "the day clock is on the HUD");
    const auto rows = sandbox.ui().menu_items(modding::ui::MenuSlot::pause);
    check(rows.size() == 1U && rows.front().label == "About Ember", "a pause-menu row");
    sandbox.ui().activate_menu_item(modding::ui::MenuSlot::pause, 0U);
    sandbox.run(0.2);
    check(sandbox.ui().screen_open() && sandbox.report().screen_commands > 0U, "which opens a dialog");
    check_clean(sandbox, "ui_theme");
}

void the_lighting_pack_lights_torches()
{
    Sandbox sandbox;
    // Two torches and a lantern near the world's start (8, -8).
    sandbox.world().edits[{8, 1, -8}] = 26U;  // torch
    sandbox.world().edits[{12, 1, -4}] = 31U; // torch_pos_y
    sandbox.world().edits[{4, 1, -12}] = 4U;  // lantern
    start(sandbox, load_examples({"lighting_pack"}));
    sandbox.run(1.0);
    check(sandbox.report().atmosphere_layers == 2U, "golden-hour and night layers");
    check(sandbox.report().lights == 3U, std::format("a light per torch and lantern ({})", sandbox.report().lights));
    sandbox.world().edits.erase({12, 1, -4});
    sandbox.run(3.5);
    check(sandbox.report().lights == 2U, "a removed torch loses its light");
    check(!sandbox.graphics().assets().empty() &&
              sandbox.graphics().assets().resolve("textures/blocks/2.png").has_value(),
          "the grass tile override is in the asset stack");
    check_clean(sandbox, "lighting_pack");
}

// mod-world: Frontier replaces the world generator.
void the_frontier_shapes_a_new_world()
{
    Sandbox sandbox;
    // Stand in the redwoods, so worldgen.chunk offers redwood chunks.
    sandbox.world().position = {3200.5, 1.0, 0.5};
    start(sandbox, load_examples({"frontier"}));
    const auto &terrain = sandbox.terrain();
    check(terrain != nullptr, "the world generator is built: " + errors_of(sandbox));
    const world::TerrainDefinition &definition = terrain->definition();
    check(definition.biomes.size() == 4U && definition.palette.size() >= 6U,
          std::format("the retail core, three biomes ({}) and their blocks ({})", definition.biomes.size(),
                      definition.palette.size()));
    const world::WorldGenerator &generator = sandbox.terrain_generator();
    const auto biome_key = [&](int x, int z) {
        return definition.biomes[terrain->biome_at(generator, x, z)].key;
    };
    check(biome_key(0, 0) == "retail:world" && biome_key(3200, 0) == "frontier:redwood" &&
              biome_key(10000, 0) == "frontier:salt_flats" && biome_key(-16000, 0) == "frontier:ashlands",
          "the rings: retail, redwood, salt flats, ashlands");
    check(biome_key(25000 + 6800, 0) == "frontier:ashlands" && biome_key(50000 + 3200, 0) == "frontier:redwood",
          "past 25,000 the rings run backwards, and start over at 50,000");
    for (const int x : {3200, 10000, 16000})
    {
        const world::Chunk chunk = generator.generate_chunk(x / 16, 0);
        check(chunk.has_mod_cells(), std::format("Frontier's blocks are in the terrain at {}", x));
    }
    const modding::RunResult info = server(sandbox, "frontier", "return world.biome(10000, 0)");
    check(info.value == modding::Value{"frontier:salt_flats"}, "world.biome names the mod biome");
    sandbox.run(8.0);
    std::size_t bark{};
    const modding::RunResult placed = server(
        sandbox, "frontier",
        "return #world.find_blocks(vector.create(3040, 2, -160), vector.create(3375, 2, 175), "
        "'frontier:redwood_bark')");
    if (placed.value.is_number())
        bark = static_cast<std::size_t>(placed.value.as_number());
    check(!sandbox.world().edits.empty() && bark > 0U,
          std::format("ranger huts rise in the redwoods ({} edits, {} bark cells)", sandbox.world().edits.size(),
                      bark));
    check_clean(sandbox, "frontier");
}

// mod-ai: the Bestiary switches retail enemies off and fights with its own.
void the_bestiary_replaces_the_retail_enemies()
{
    Sandbox sandbox;
    start(sandbox, load_examples({"bestiary"}));
    sandbox.run(1.0);
    using modding::gameplay::RetailEnemyClass;
    const modding::Vec3 here = sandbox.world().position;
    check(sandbox.gameplay().retail_rules_active() &&
              !sandbox.gameplay().retail_enemy_allowed(RetailEnemyClass::zombie, here) &&
              !sandbox.gameplay().retail_enemy_allowed(RetailEnemyClass::dragon, here),
          "every retail enemy is off for the session");
    server(sandbox, "bestiary", R"(
        entities.spawn("hollow_king", vector.create(8.5, 1, 26))
        local first = entities.spawn("ghoul", vector.create(30, 1, 8))
        entities.spawn("ghoul", vector.create(31, 1, 9), { pack = first })
        entities.spawn("ghoul", vector.create(30, 1, 10), { pack = first })
        entities.spawn("grey_wolf", vector.create(26, 1, 8))
        entities.spawn("bone_archer", vector.create(-12, 1, 8))
        entities.spawn("gloom_bat", vector.create(8, 4, -14))
    )");
    bool arrow{};
    for (int second = 0; second < 20; ++second)
    {
        sandbox.run(1.0);
        arrow = arrow || !sandbox.gameplay().client_projectiles().empty();
    }
    const auto &bosses = sandbox.gameplay().client_bosses();
    check(bosses.size() == 1U && bosses.front().title == "The Hollow King" && bosses.front().music == "music/hollow_king",
          "the Hollow King's bar and music reach the client");
    check(arrow, "an archer's arrows were seen in flight");
    check(!sandbox.world().damage_taken.empty(), "the bestiary's creatures attacked the player");
    const modding::RunResult hurt = server(sandbox, "bestiary", R"(
        local wounded = 0
        for _, e in entities.list() do
            local info = entities.get(e)
            if info.health < info.max_health then wounded += 1 end
        end
        return wounded)");
    check(hurt.value.is_number() && hurt.value.as_number() >= 1.0, "the teams fought each other");
    const modding::RunResult summoned = server(sandbox, "bestiary", "return #entities.find({ kind = 'grave_leaper' })");
    check(summoned.value == modding::Value{0}, "entities.find answers by kind");
    check_clean(sandbox, "bestiary");
}

// (Wave 5, survival) The Wilderness pack's survival half: the starter kit,
// hunger and thirst on its own HUD, food, filling and drinking vessels at
// water, milking, cooking on a stone oven, drinking with bare hands, the
// animals and their drops, and damage while starving.
void wilderness_lives_off_the_land()
{
    Sandbox sandbox;
    start(sandbox, load_examples({"wilderness"}));
    sandbox.run(1.0);
    for (const auto &[key, count] : std::vector<std::pair<std::string, std::uint32_t>>{
             {"wilderness:bucket", 1U}, {"wilderness:bottle", 2U}, {"wilderness:apple", 4U},
             {"wilderness:raw_beef", 2U}, {"wilderness:stone_oven", 1U}})
        check(count_of(sandbox, key) == count, std::format("the starter kit holds {} x {} ({})", count, key,
                                                           count_of(sandbox, key)));
    check(sandbox.ui().hud_roots().size() == 1U && sandbox.report().hud_commands > 0U,
          "the hunger and thirst bars are on the HUD");
    const auto bars = sandbox.gameplay().client_stats();
    check(std::none_of(bars.begin(), bars.end(), [](const auto &bar) { return bar.hud; }),
          "the pack draws its own bars, not the plain built-in ones");

    sandbox.run(60.0);
    check(std::abs(stat(sandbox, "hunger") - 95.0) < 1.0,
          std::format("hunger lasts 20 minutes ({})", stat(sandbox, "hunger")));
    check(std::abs(stat(sandbox, "thirst") - (100.0 - 6000.0 / 840.0)) < 1.0,
          std::format("thirst lasts 14 minutes ({})", stat(sandbox, "thirst")));

    const auto use_held = [&] {
        static_cast<void>(sandbox.gameplay().local_item_using(*sandbox.world().local_held()));
        sandbox.run(1.3); // past the cooldown (a meal's is 1.2 s, and it holds every item)
    };
    // An apple fills hunger.
    server(sandbox, "wilderness", "stats.set(players.list()[1], 'hunger', 40)");
    sandbox.run(0.5);
    check(hold(sandbox, "wilderness:apple"), "an apple in the tray");
    use_held();
    check(std::abs(stat(sandbox, "hunger") - 55.0) < 0.5 && count_of(sandbox, "wilderness:apple") == 3U,
          std::format("an apple is eaten for 15 hunger ({})", stat(sandbox, "hunger")));

    // A pool of water just in front of the player: bottles and buckets fill
    // at it, and drinking them gives the empty vessel back.
    server(sandbox, "wilderness", R"(
        for x = 7, 9 do
            for z = 5, 6 do
                world.set_block(vector.create(x, 0, z), "wilderness:water")
            end
        end)");
    sandbox.run(0.5);
    sandbox.world().aimed = modding::gameplay::BlockPos{8, 0, 5};
    check(hold(sandbox, "wilderness:bottle"), "a bottle in the tray");
    use_held();
    check(count_of(sandbox, "wilderness:water_bottle") == 1U && count_of(sandbox, "wilderness:bottle") == 1U,
          "used on water, a bottle fills");
    check(hold(sandbox, "wilderness:bucket"), "the bucket in the tray");
    use_held();
    check(count_of(sandbox, "wilderness:water_bucket") == 1U, "and so does the bucket");
    sandbox.world().aimed.reset();
    server(sandbox, "wilderness", "stats.set(players.list()[1], 'thirst', 30)");
    sandbox.run(0.5);
    check(hold(sandbox, "wilderness:water_bottle"), "the full bottle in the tray");
    use_held();
    check(std::abs(stat(sandbox, "thirst") - 55.0) < 0.5 && count_of(sandbox, "wilderness:bottle") == 2U,
          std::format("drinking it gives 25 thirst and the bottle back ({})", stat(sandbox, "thirst")));
    check(hold(sandbox, "wilderness:water_bucket"), "the full bucket in the tray");
    use_held();
    check(stat(sandbox, "thirst") > 99.0 && count_of(sandbox, "wilderness:bucket") == 1U,
          "a bucket of water fills thirst up and the bucket comes back");

    // Bare hands: the drink key (the client asks the server) and /drink, with
    // the water at the player's feet; nothing far from water.
    server(sandbox, "wilderness", "stats.set(players.list()[1], 'thirst', 50)");
    sandbox.world().position = {8.5, 1.0, 7.5};
    sandbox.run(0.5);
    check(sandbox.runtime()
              .run(Side::client, "wilderness", "test", "net.send('drink', { look = vector.create(0, -1, -1) })")
              .ok,
          "the client sends a drink");
    sandbox.run(0.5);
    check(std::abs(stat(sandbox, "thirst") - 58.0) < 0.5, std::format("a mouthful of water ({})", stat(sandbox, "thirst")));
    sandbox.run(0.5); // a sip at most twice a second
    check(sandbox.chat("/drink") == modding::gameplay::ChatVerdict::command, "/drink is a command");
    sandbox.run(0.6);
    check(stat(sandbox, "thirst") > 65.0, std::format("and another ({})", stat(sandbox, "thirst")));
    sandbox.world().position = {40.5, 1.0, 40.5};
    sandbox.run(0.6);
    static_cast<void>(sandbox.chat("/drink"));
    sandbox.run(0.6);
    check(stat(sandbox, "thirst") < 66.5, "far from water nothing is drunk");

    // Milking: a cow right in front of the player (looking along -Z).
    sandbox.world().position = {20.5, 1.0, 20.5};
    server(sandbox, "wilderness", "entities.spawn('wilderness:cow', vector.create(20.5, 1, 17.5))");
    sandbox.run(0.2);
    check(hold(sandbox, "wilderness:bucket"), "the empty bucket in the tray");
    use_held();
    check(count_of(sandbox, "wilderness:milk_bucket") == 1U, "used on a cow, the bucket fills with milk");
    check(hold(sandbox, "wilderness:milk_bucket"), "the milk in the tray");
    use_held();
    check(count_of(sandbox, "wilderness:bucket") == 1U, "drinking the milk gives the bucket back");
    check(hold(sandbox, "wilderness:bucket"), "the bucket again");
    use_held();
    check(count_of(sandbox, "wilderness:milk_bucket") == 0U, "the same cow has no milk again at once");

    // Cooking: raw beef used on a stone oven.
    server(sandbox, "wilderness", "world.set_block(vector.create(30, 1, 30), 'wilderness:stone_oven')");
    sandbox.run(0.2);
    sandbox.world().aimed = modding::gameplay::BlockPos{30, 1, 30};
    check(hold(sandbox, "wilderness:raw_beef"), "raw beef in the tray");
    use_held();
    sandbox.world().aimed.reset();
    check(count_of(sandbox, "wilderness:steak") == 1U && count_of(sandbox, "wilderness:raw_beef") == 1U,
          "on the oven it cooks into a steak");
    // The client finds the oven by its mod key in its own copy of the world
    // (not every rock, its fallback) and lights its four fire mouths.
    sandbox.run(2.1);
    check(sandbox.report().lights == 4U, std::format("the oven glows ({} lights)", sandbox.report().lights));
    check(sandbox.runtime()
              .run(Side::client, "wilderness", "test",
                   "ui.close(ui.add_hud({ kind = 'label', text = 'x', frame = 'game' }))")
              .ok,
          "a HUD element may use the game's own frame");

    // The host brings a herd over; a dead cow drops beef.
    check(sandbox.chat("/wilderness herd cow 4") == modding::gameplay::ChatVerdict::command, "/wilderness herd");
    sandbox.run(0.5);
    const modding::RunResult cows = server(sandbox, "wilderness", "return #entities.list({ kind = 'wilderness:cow' })");
    check(cows.value.is_number() && cows.value.as_number() >= 5.0, "four more cows came");
    const std::size_t pickups = sandbox.world().pickups.size();
    server(sandbox, "wilderness", R"(
        local cow = entities.list({ kind = "wilderness:cow" })[1]
        entities.damage(cow, 100, players.list()[1]))");
    sandbox.run(0.5);
    check(sandbox.world().pickups.size() > pickups, "a cow drops its meat");

    // Grassy land fills with animals by their spawn rules.
    sandbox.world().position = {8.5, 1.0, 8.5};
    sandbox.run(60.0);
    std::size_t animals{};
    for (const auto &entity : sandbox.gameplay().client_entities())
        animals += entity.kind == "wilderness:pig" || entity.kind == "wilderness:sheep" ||
                           entity.kind == "wilderness:chicken"
                       ? 1U
                       : 0U;
    check(animals > 0U, "pigs, sheep and chickens appear by their spawn rules");

    // Starving hurts.
    server(sandbox, "wilderness", "stats.set(players.list()[1], 'hunger', 0)");
    const float health = sandbox.world().health;
    sandbox.run(3.0);
    check(sandbox.world().health < health - 3.0F,
          std::format("starving hurts ({} -> {})", health, sandbox.world().health));
    check_clean(sandbox, "wilderness");

    // The pack with the client-side examples, under both rule sets.
    for (const bool modern : {true, false})
    {
        Sandbox together;
        together.world().modern = modern;
        start(together, load_examples({"wilderness", "ui_theme", "lighting_pack"}));
        together.run(30.0);
        check_clean(together, modern ? "wilderness with the client mods (Modern rules)"
                                     : "wilderness with the client mods (Classic rules)");
    }
}

void all_examples_together_under_both_rule_sets()
{
    for (const bool modern : {true, false})
    {
        Sandbox sandbox;
        sandbox.world().modern = modern;
        start(sandbox, load_examples({"survival", "story_dungeon", "wildlife", "ui_theme", "lighting_pack", "frontier",
                                      "bestiary"})); // mod-ai: the bestiary too
        sandbox.run(45.0);
        check(sandbox.runtime().status().size() == 12U, "five server mods on both sides and two client mods");
        check_clean(sandbox, modern ? "all examples (Modern rules)" : "all examples (Classic rules)");
    }
}

// (GUNPACK) Tactical Animations: every gun family's clips are there (with
// the from-empty and per-model variants), they move only the arms, the head
// and gun parts - never the hips or legs, which belong to the walk - every cue
// names a sound or model the pack ships, and its client script runs clean.
void the_tactical_animations_cover_every_gun()
{
    const fs::path pack = examples_root() / "tactical_animations";
    static const std::set<std::string> lower_body{"Root",      "BackLower", "4",         "HipLeft",   "HipRight",
                                                  "7",         "9",         "10",        "KneeLeft",  "KneeRight",
                                                  "13",        "17",        "AnkleLeft", "AnkleRight", "ToeLeft",
                                                  "ToeRight"};
    std::set<std::string> clips;
    for (const fs::directory_entry &entry : fs::directory_iterator{pack / "assets" / "animations"})
    {
        std::string error;
        const auto model =
            modding::graphics::load_model(entry.path(), &error, modding::graphics::animation_file_limits());
        check(model != nullptr, entry.path().filename().string() + ": " + error);
        for (const modding::graphics::ModelAnimation &animation : model->animations)
        {
            check(clips.insert(animation.name).second, animation.name + " is in the pack twice");
            check(animation.duration > 0.0F && animation.duration <= 10.0F, animation.name + " has a sane length");
            for (const modding::graphics::AnimationChannel &channel : animation.channels)
            {
                const std::string &node = model->nodes[static_cast<std::size_t>(channel.node)].name;
                check(!lower_body.contains(node), std::format("{} moves {} (the lower body)", animation.name, node));
                const bool part = !node.empty() && node[0] >= 'a' && node[0] <= 'z';
                check(part || channel.path == modding::graphics::AnimationChannel::Path::rotation,
                      std::format("{} moves the joint {} (only rotations are read)", animation.name, node));
            }
        }
    }
    for (const std::string_view family : {"Gun", "Pistol", "SMG", "Rifle", "LMG", "LaserGun", "LaserRifle",
                                           "LaserPistol", "LaserSMG"})
    {
        for (const std::string_view clip : {"Idle", "Shoot", "Reload", "Shoulder", "ShoulderIdle", "ShoulderShoot",
                                            "ShoulderWalk"})
            check(clips.contains(std::string{family} + std::string{clip}), std::string{family} + std::string{clip});
        check(clips.contains(std::string{family} + "Walk") || clips.contains(std::string{family} + "Run"),
              std::string{family} + " walks");
    }
    for (const std::string_view name :
         {"GunReloadEmpty", "PistolReloadEmpty", "SMGReloadEmpty", "LaserGunReloadEmpty", "LaserRifleReloadEmpty",
          "LaserPistolReloadEmpty", "LaserSMGReloadEmpty", "PumpShotgunIdle", "PumpShotgunShoot",
          "PumpShotgunShoulderShoot", "PumpShotgunReload", "PumpShotgunReload@rpg", "PumpShotgunShoulderShoot@rpg",
          "GunShoulderIdle@rpg", "RPGIdle", "RPGWalk", "RPGShoot", "LaserShotgunShoot", "LaserShotgunReload",
          "LaserGunIdle@space_shotgun", "PickIdle@knife", "PickWalk@knife", "PickUse@knife",
          "GrenadeIdle", "GrenadeWalk", "Grenade_Cook", "Grenade_Throw", "Grenade_Reset"})
        check(clips.contains(std::string{name}), std::string{name} + " is in the pack");

    std::ifstream file{pack / "client" / "tactical.luau", std::ios::binary};
    std::stringstream text;
    text << file.rdbuf();
    const std::string script = text.str();
    std::size_t sounds{};
    const std::regex sound{R"re(sound = "(sounds/[a-z0-9_]+)")re"};
    for (auto match = std::sregex_iterator{script.begin(), script.end(), sound}; match != std::sregex_iterator{};
         ++match, ++sounds)
        check(fs::exists(pack / "assets" / ((*match)[1].str() + ".wav")), (*match)[1].str() + ".wav is shipped");
    const std::regex prop{R"re(model = "(models/[a-z0-9_]+)")re"};
    for (auto match = std::sregex_iterator{script.begin(), script.end(), prop}; match != std::sregex_iterator{};
         ++match)
        check(fs::exists(pack / "assets" / ((*match)[1].str() + ".gltf")), (*match)[1].str() + ".gltf is shipped");
    check(sounds >= 100U, std::format("the clips cue their sounds ({})", sounds));

    Sandbox sandbox;
    start(sandbox, load_examples({"tactical_animations"}));
    sandbox.run(1.0);
    check_clean(sandbox, "tactical_animations");
}

void the_templates_validate_and_run()
{
    Sandbox validator;
    for (const std::string_view kind : {"client", "server"})
    {
        Scratch scratch{std::string{"template-"} + std::string{kind}};
        const fs::path root = scratch.path() / "my_mod";
        for (const TemplateFile &file : *template_files(kind))
        {
            std::string text{file.text};
            for (std::size_t at = text.find("{{id}}"); at != std::string::npos; at = text.find("{{id}}", at))
                text.replace(at, 6U, "my_mod");
            write(root / file.path, text);
        }
        const ValidationResult result = validate_mod(root, validator.registry());
        check(result.ok() && result.diagnostics.empty(),
              std::format("the {} template validates:\n{}", kind, format_diagnostics(result)));
        Sandbox sandbox;
        start(sandbox, {load(root)});
        sandbox.run(3.0);
        if (kind == "server")
            check(sandbox.chat("/my_mod") == modding::gameplay::ChatVerdict::command, "its command works");
        check_clean(sandbox, std::format("the {} template", kind));
    }
}

void the_validator_finds_what_the_game_would_refuse()
{
    Scratch scratch{"validator"};
    const fs::path root = scratch.path() / "broken";
    write(root / "openminerz.mod", "id=broken\nversion=1\nscope=client\nprotocol=both\norder=1\n");
    write(root / "shared" / "a.luau", R"(world.set_block(vector.create(0, 0, 0), "rock")
events.on("tick", function() end)
local x = world.get_blok(vector.create(1, 2, 3))
local m = require("./missing")
os.exit()
world.foo = 1
net.send("x", {})
events.on("no.such.event", function() end)
events.emit("other:thing")
)");
    write(root / "client" / "b.luau", "local c = require('./c')\nreturn {}\n");
    write(root / "client" / "c.luau", "local b = require('./b')\nreturn {}\n");
    write(root / "client" / "d.luau", "local broken = (\n");
    write(root / "notes.exe", "MZ");
    write(root / "lang" / "en.lang", "good = yes\n= no key\n");
    Sandbox sandbox;
    const ValidationResult result = validate_mod(root, sandbox.registry());
    const std::string report = format_diagnostics(result);
    for (const std::string_view expected : {
             "shared/a.luau:1:1: error: world.set_block is only available on the server",
             "shared/a.luau:2:11: error: event 'tick' only fires on the server",
             "shared/a.luau:3:11: error: unknown API 'world.get_blok' (did you mean world.get_block?)",
             "shared/a.luau:4:19: error: no module './missing'",
             "shared/a.luau:5:1: error: 'os' is not available to mods",
             "shared/a.luau:6:1: error: library tables are read-only",
             "shared/a.luau:7:1: error: net.send needs the network capability, which client mods do not have",
             "shared/a.luau:8:11: error: unknown event 'no.such.event'",
             "shared/a.luau:9:13: error: a mod emits only its own events",
             "error: require cycle: client/",
             "client/d.luau:2:1: error:",
             "notes.exe: error: not a file type mods may contain",
             "lang/en.lang: error: line 2",
         })
        check(report.find(expected) != std::string::npos,
              std::format("the validator reports \"{}\":\n{}", expected, report));
    check(!result.ok(), "and fails the mod");

    // A server mod's shared file: one-sided calls are fine inside functions
    // and ifs, fatal at the top.
    const fs::path shared = scratch.path() / "sided";
    write(shared / "openminerz.mod", "id=sided\nversion=1\nscope=server\nprotocol=both\norder=1\n");
    write(shared / "shared" / "a.luau", R"(events.on("frame", function() end)
if mod.side() == "server" then
    world.set_block(vector.create(0, 0, 0), "rock")
end
local function later() players.list() end
ui.toast("a", "b")
)");
    const std::string sided = format_diagnostics(validate_mod(shared, sandbox.registry()));
    check(sided.find("shared/a.luau:1:11: error: event 'frame' only fires on the client: subscribing at the top") !=
              std::string::npos,
          "a top-level one-sided event in shared/ is an error:\n" + sided);
    check(sided.find(":3:") == std::string::npos && sided.find(":5:") == std::string::npos,
          "guarded and deferred calls are not:\n" + sided);
}

// A placeholder for an asset a snippet names: a real file of that kind from
// the examples, so the services that load it are satisfied.
void materialize_assets(const fs::path &mod, const std::string &source)
{
    static const std::regex quoted{R"re("((?:assets|textures|models|sounds|music|luts)/[A-Za-z0-9_./-]+)")re"};
    const fs::path examples = examples_root();
    const std::map<std::string, fs::path> by_extension{
        {".png", examples / "survival" / "assets" / "textures" / "fresh_water.png"},
        {".ttf", examples / "ui_theme" / "assets" / "fonts" / "EmberPixel.ttf"},
        {".wav", examples / "ui_theme" / "assets" / "ui" / "click.wav"},
        {".gltf", examples / "wildlife" / "assets" / "models" / "deer.gltf"},
    };
    const std::map<std::string, std::string> by_folder{
        {"textures", ".png"}, {"models", ".gltf"}, {"sounds", ".wav"}, {"music", ".wav"}, {"luts", ".png"}};
    for (std::sregex_iterator match{source.begin(), source.end(), quoted}, end; match != end; ++match)
    {
        std::string name = (*match)[1].str();
        fs::path target;
        if (name.starts_with("assets/"))
            target = mod / name;
        else
        {
            const std::string folder = name.substr(0U, name.find('/'));
            target = mod / "assets" / (name + (fs::path{name}.has_extension() ? "" : by_folder.at(folder)));
        }
        fs::path template_file = by_extension.contains(target.extension().string())
                                     ? by_extension.at(target.extension().string())
                                     : fs::path{};
        if (name.starts_with("luts/"))
            template_file = examples / "lighting_pack" / "assets" / "luts" / "warm_evening.png";
        if (template_file.empty() || fs::exists(target))
            continue;
        fs::create_directories(target.parent_path());
        fs::copy_file(template_file, target);
    }
}

// Every ```luau block of the guides says where it runs - ```luau server,
// client, shared (a server mod "mymod"), client-mod (a client mod), or
// nocheck - and becomes a file of a scratch mod; each guide's mods must
// validate and then run in the SDK session without an error.
void every_doc_snippet_runs()
{
    const fs::path docs = fs::path{OPENMINERZ_SOURCE_DIR} / "docs" / "modding";
    std::size_t snippets{};
    for (const fs::directory_entry &entry : fs::directory_iterator{docs})
    {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".md" || name == "api-reference.md" || name == "architecture.md")
            continue;
        Scratch scratch{"doc-" + entry.path().stem().string()};
        const fs::path server_mod = scratch.path() / "mymod";
        const fs::path client_mod = scratch.path() / "mytheme";
        std::ifstream input{entry.path()};
        std::string line;
        std::size_t number{};
        int line_number{};
        bool any_server{};
        bool any_client{};
        while (std::getline(input, line))
        {
            ++line_number;
            if (!line.starts_with("```luau"))
                continue;
            const std::string tag = line.size() > 8U ? line.substr(8U) : std::string{};
            const int opened = line_number;
            std::string code;
            while (std::getline(input, line) && !line.starts_with("```"))
            {
                ++line_number;
                code += line + '\n';
            }
            ++line_number;
            check(tag == "server" || tag == "client" || tag == "shared" || tag == "client-mod" || tag == "nocheck",
                  std::format("{}:{}: tag the luau block with server, client, shared, client-mod or nocheck", name,
                              opened));
            if (tag == "nocheck")
                continue;
            ++snippets;
            const std::string file = std::format("{:02}_line{}.luau", ++number, opened);
            if (tag == "client-mod")
            {
                write(client_mod / "client" / file, code);
                materialize_assets(client_mod, code);
                any_client = true;
            }
            else
            {
                write(server_mod / tag / file, code);
                materialize_assets(server_mod, code);
                any_server = true;
            }
        }
        std::vector<net::LoadedMod> mods;
        Sandbox validator;
        if (any_server)
        {
            write(server_mod / "openminerz.mod", "id=mymod\nversion=1.0.0\nscope=server\nprotocol=both\norder=1\n");
            const ValidationResult result = validate_mod(server_mod, validator.registry());
            check(result.ok(), name + " (server snippets) does not validate:\n" + format_diagnostics(result));
            mods.push_back(load(server_mod));
        }
        if (any_client)
        {
            write(client_mod / "openminerz.mod", "id=mytheme\nversion=1.0.0\nscope=client\nprotocol=both\norder=2\n");
            const ValidationResult result = validate_mod(client_mod, validator.registry());
            check(result.ok(), name + " (client-mod snippets) does not validate:\n" + format_diagnostics(result));
            mods.push_back(load(client_mod));
        }
        if (mods.empty())
            continue;
        Sandbox sandbox;
        start(sandbox, mods);
        sandbox.run(3.0);
        std::string problems;
        for (const modding::LogEntry &logged : sandbox.log())
            if (logged.level == modding::LogLevel::error)
                problems += std::format("  [{}] {}\n", modding::side_name(logged.side), logged.message);
        check(problems.empty(), name + " snippets log errors:\n" + problems);
        for (const modding::ModStatus &status : sandbox.runtime().status())
            check(status.state == modding::ModState::running,
                  std::format("{}: {} stopped on the {}: {}", name, status.id, modding::side_name(status.side),
                              status.error));
    }
    check(snippets >= 8U, std::format("the guides' snippets were found ({})", snippets));
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
        {"every_example_validates_strictly", every_example_validates_strictly},
        {"survival_needs_food_and_water", survival_needs_food_and_water},
        {"the_crypt_quest_can_be_finished", the_crypt_quest_can_be_finished},
        {"wildlife_spawns_and_is_wary", wildlife_spawns_and_is_wary},
        {"the_theme_restyles_and_adds_a_clock", the_theme_restyles_and_adds_a_clock},
        {"the_lighting_pack_lights_torches", the_lighting_pack_lights_torches},
        {"the_frontier_shapes_a_new_world", the_frontier_shapes_a_new_world},
        {"the_bestiary_replaces_the_retail_enemies", the_bestiary_replaces_the_retail_enemies}, // mod-ai
        {"wilderness_lives_off_the_land", wilderness_lives_off_the_land},                       // wave 5
        {"the_tactical_animations_cover_every_gun", the_tactical_animations_cover_every_gun},   // GUNPACK
        {"all_examples_together_under_both_rule_sets", all_examples_together_under_both_rule_sets},
        {"the_templates_validate_and_run", the_templates_validate_and_run},
        {"the_validator_finds_what_the_game_would_refuse", the_validator_finds_what_the_game_would_refuse},
        {"every_doc_snippet_runs", every_doc_snippet_runs},
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
    std::cout << (tests.size() - failures) << " of " << tests.size() << " example mod checks passed\n";
    return failures == 0U ? 0 : 1;
}
