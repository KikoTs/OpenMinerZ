// Add-Ons: installed mods turned on and off (openminerz/modding/addons.hpp).
// The roots read mod by mod (a broken mod is listed, never fatal), bundled
// examples off by default and replaced by an installed copy, the player's
// choices on disk, world-generator and declared conflicts, dependencies, the
// catalog of the mods that are on - and, with every mod off, a host lobby and
// a join profile byte for byte those of a game with no mods installed.

#include "openminerz/menu_model.hpp"
#include "openminerz/modding/addons.hpp"
#include "openminerz/net/legacy_steam_runtime_host.hpp"
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/steam_bridge_protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;
namespace fs = std::filesystem;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// A fresh folder under the temp directory, removed when the test ends.
class Scratch final
{
  public:
    Scratch()
    {
        std::random_device device;
        path_ = fs::temp_directory_path() / ("omz-addons-" + std::to_string(device()));
        fs::create_directories(path_);
    }
    ~Scratch()
    {
        std::error_code error;
        fs::remove_all(path_, error);
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
    std::ofstream output{file, std::ios::binary};
    output << text;
}

// A mod folder: its descriptor (with `extra` keys) and, optionally, files.
void make_mod(const fs::path &root, std::string_view id, std::string_view scope, std::string_view extra = {},
              const std::map<std::string, std::string> &files = {}, std::string_view order = "100")
{
    const fs::path folder = root / std::string{id};
    write(folder / "openminerz.mod", std::format("id={}\nversion=1.0.0\nscope={}\nprotocol=both\norder={}\nname={}\n{}",
                                                 id, scope, order, id, extra));
    for (const auto &[path, text] : files)
        write(folder / path, text);
}

const std::map<std::string, std::string> world_mod{
    {"shared/world.luau", "terrain.define({ biomes = {} })\n"}};

std::vector<std::string> ids(const net::ModCatalog &catalog)
{
    std::vector<std::string> result;
    for (const net::LoadedMod &mod : catalog.mods)
        result.push_back(mod.descriptor.id);
    return result;
}

std::size_t index_of(const modding::AddonScan &scan, std::string_view id)
{
    for (std::size_t index{}; index < scan.addons.size(); ++index)
        if (scan.addons[index].id() == id)
            return index;
    throw std::runtime_error("no mod " + std::string{id} + " in the scan");
}

std::vector<modding::AddonRoot> three_roots(const Scratch &scratch)
{
    return {{scratch.path() / "game", modding::AddonOrigin::game},
            {scratch.path() / "user", modding::AddonOrigin::user},
            {scratch.path() / "bundled", modding::AddonOrigin::bundled}};
}

// Installed mods start on, bundled examples off; the catalog everything else
// uses holds exactly the mods that are on, in load order.
void installed_on_bundled_off()
{
    Scratch scratch;
    const auto roots = three_roots(scratch);
    make_mod(roots[0].path, "game_pack", "client", {}, {}, "300");
    make_mod(roots[1].path, "user_rules", "server", {}, {}, "50");
    make_mod(roots[2].path, "example_world", "server", {}, world_mod);
    make_mod(roots[2].path, "example_theme", "client");
    const modding::AddonScan scan = modding::scan_addons(roots);
    check(scan.addons.size() == 4U && scan.errors.empty(), "every mod of the three roots is listed");
    const auto status = modding::resolve_addons(scan, {});
    check(status[index_of(scan, "game_pack")].on && status[index_of(scan, "user_rules")].on,
          "mods the player installed start on");
    check(!status[index_of(scan, "example_world")].on && !status[index_of(scan, "example_theme")].on,
          "bundled examples start off");
    check(ids(modding::active_catalog(scan, status)) == std::vector<std::string>{"user_rules", "game_pack"},
          "the active catalog is the mods that are on, by order");
    check(scan.addons[index_of(scan, "example_world")].defines_world &&
              !scan.addons[index_of(scan, "user_rules")].defines_world,
          "a mod calling terrain.define replaces the world generator");

    // The same set, read the old way, comes out in the same order.
    Scratch only_on;
    make_mod(only_on.path(), "game_pack", "client", {}, {}, "300");
    make_mod(only_on.path(), "user_rules", "server", {}, {}, "50");
    net::ModCatalog loaded;
    std::string error;
    check(modding::load_mod_catalogs(std::vector<fs::path>{only_on.path()}, loaded, error), error);
    check(ids(loaded) == ids(modding::active_catalog(scan, status)), "load order unchanged");

    const auto bundled_roots = modding::addon_roots(scratch.path() / "bundled");
    check(bundled_roots.size() == 3U && bundled_roots.back().origin == modding::AddonOrigin::bundled &&
              bundled_roots.front().origin == modding::AddonOrigin::game,
          "roots: the game's addons, the user's Mods folder, then the bundled examples");
    check(modding::bundled_mod_root("C:/Game") == fs::path{"C:/Game"} / "examples" / "mods",
          "the bundled examples live in examples/mods beside the game");
}

// One broken mod is a row of its own: the others still load.
void a_broken_mod_is_listed_not_fatal()
{
    Scratch scratch;
    const auto roots = three_roots(scratch);
    make_mod(roots[1].path, "good", "client");
    write(roots[1].path / "bad_descriptor" / "openminerz.mod", "id=bad\nthis is not a key\n");
    make_mod(roots[1].path, "binary", "server", {}, {{"server/tool.exe", "MZ"}});
    make_mod(roots[1].path, "too_new", "client", "api=999\n");
    write(roots[1].path / "notes" / "readme.txt", "not a mod: no descriptor");
    const modding::AddonScan scan = modding::scan_addons(roots);
    check(scan.addons.size() == 4U, "three broken mods and a good one are listed; a folder without a descriptor is not");
    check(scan.addons[index_of(scan, "good")].usable(), "the good mod is usable");
    check(!scan.addons[index_of(scan, "bad_descriptor")].usable(), "an unreadable descriptor is a problem, named by folder");
    check(scan.addons[index_of(scan, "binary")].problem.find("not a file type") != std::string::npos,
          "a file mods may not contain is the mod's problem");
    check(scan.addons[index_of(scan, "too_new")].problem.find("API") != std::string::npos,
          "a mod for a newer API is the mod's problem");
    const auto status = modding::resolve_addons(scan, {});
    check(ids(modding::active_catalog(scan, status)) == std::vector<std::string>{"good"},
          "only the usable mod reaches the catalog");
    check(!status[index_of(scan, "binary")].reason.empty(), "the problem is the row's reason");
}

// A mod the player installed replaces the bundled example with its id; the
// same id installed twice by the player is a problem of the later copy.
void installed_copy_replaces_the_bundled_one()
{
    Scratch scratch;
    const auto roots = three_roots(scratch);
    make_mod(roots[1].path, "wilderness", "server", {}, world_mod);
    make_mod(roots[2].path, "wilderness", "server", {}, world_mod);
    make_mod(roots[2].path, "Survival", "server");
    make_mod(roots[0].path, "survival", "server");
    make_mod(roots[0].path, "twice", "client");
    make_mod(roots[1].path, "TWICE", "client");
    const modding::AddonScan scan = modding::scan_addons(roots);
    check(std::count(scan.hidden.begin(), scan.hidden.end(), "wilderness") == 1 &&
              std::count(scan.hidden.begin(), scan.hidden.end(), "Survival") == 1,
          "the bundled copies are hidden, never 'installed twice'");
    const auto &wilderness = scan.addons[index_of(scan, "wilderness")];
    check(wilderness.origin == modding::AddonOrigin::user && wilderness.usable(), "the user's copy wins");
    check(scan.addons[index_of(scan, "twice")].usable() &&
              scan.addons[index_of(scan, "TWICE")].problem.find("installed twice") != std::string::npos,
          "the game folder's copy is used, the Mods folder's is named as installed twice");
    const auto status = modding::resolve_addons(scan, {});
    check(status[index_of(scan, "wilderness")].on, "and, installed by the player, it starts on");
    net::ModManifest manifest;
    std::string error;
    check(net::make_mod_manifest(modding::active_catalog(scan, status), net::ProtocolVersion::classic_v3, manifest,
                                 error),
          "no duplicate reaches the manifest: " + error);
}

// The choices survive a restart; a choice overrides the default either way.
void choices_persist()
{
    Scratch scratch;
    const auto roots = three_roots(scratch);
    make_mod(roots[1].path, "mine", "server");
    make_mod(roots[2].path, "example", "client");
    const modding::AddonScan scan = modding::scan_addons(roots);
    const fs::path file = scratch.path() / "appdata" / "addons.cfg";
    check(modding::read_addon_choices(file).empty(), "no file, no choices");
    modding::AddonChoices choices = modding::set_addon(scan, {}, "mine", false).choices;
    choices = modding::set_addon(scan, choices, "Example", true).choices;
    std::string error;
    check(modding::write_addon_choices(file, choices, error), error);
    const modding::AddonChoices read = modding::read_addon_choices(file);
    check(read == choices && read.at("mine") == false && read.at("example") == true, "written and read back by id");
    const auto status = modding::resolve_addons(scan, read);
    check(!status[index_of(scan, "mine")].on && status[index_of(scan, "example")].on,
          "the player's choice beats the default in both directions");
    write(file, "# comment\non=example\nbogus line\nmaybe=mine\noff = MINE \n");
    const modding::AddonChoices sloppy = modding::read_addon_choices(file);
    check(sloppy.size() == 2U && sloppy.at("mine") == false, "keys are trimmed and lowercased, junk ignored");
}

// Off is everywhere: a vanilla host's join takes only the client mods that
// are on; a host that runs a server mod which is off here gets it, from the
// local copy, exactly as if it were on.
void off_mods_and_joining()
{
    Scratch scratch;
    const auto roots = three_roots(scratch);
    make_mod(roots[1].path, "theme_on", "client");
    make_mod(roots[1].path, "theme_off", "client");
    make_mod(roots[1].path, "rules_on", "server");
    make_mod(roots[1].path, "rules_off", "server");
    const modding::AddonScan scan = modding::scan_addons(roots);
    modding::AddonChoices choices{{"theme_off", false}, {"rules_off", false}};
    const auto status = modding::resolve_addons(scan, choices);
    check(ids(modding::active_catalog(scan, status)) == std::vector<std::string>{"rules_on", "theme_on"},
          "the mods that are off are in no catalog the game runs");
    const net::ModCatalog join = modding::join_catalog(scan, status);
    check(ids(join) == std::vector<std::string>{"rules_off", "rules_on", "theme_on"},
          "a join may use a server mod that is off, never a client mod that is off");

    const net::SessionModSelection vanilla =
        net::select_session_mods(join, {}, nullptr, net::ProtocolVersion::classic_v3);
    check(vanilla.manifest.mods.size() == 1U && vanilla.manifest.mods.front().id == "theme_on",
          "a vanilla host: only the client mods that are on");

    net::ModManifest required;
    required.protocol = net::ProtocolVersion::classic_v3;
    required.mods.push_back(scan.addons[index_of(scan, "rules_off")].mod.descriptor);
    const net::SessionModSelection modded =
        net::select_session_mods(join, {}, &required, net::ProtocolVersion::classic_v3);
    check(modded.missing.empty() && modded.mods.size() == 2U &&
              std::any_of(modded.mods.begin(), modded.mods.end(),
                          [](const net::LoadedMod &mod) { return mod.descriptor.id == "rules_off"; }),
          "a host running the server mod that is off here: the local copy is used, nothing to download");
    check(std::none_of(modded.mods.begin(), modded.mods.end(),
                       [](const net::LoadedMod &mod) { return mod.descriptor.id == "rules_on"; }),
          "and a server mod the host does not run is left out, on or not");
}

// With every mod off, the host's lobby, its manifest and the join profile
// are byte for byte those of a game with no mods installed.
void every_mod_off_is_retail_on_the_wire()
{
    Scratch scratch;
    const auto roots = three_roots(scratch);
    make_mod(roots[1].path, "rules", "server");
    make_mod(roots[1].path, "core_rules", "core");
    make_mod(roots[1].path, "theme", "client");
    make_mod(roots[2].path, "example", "server", {}, world_mod);
    const modding::AddonScan scan = modding::scan_addons(roots);
    modding::AddonChoices all_off;
    for (const modding::InstalledAddon &addon : scan.addons)
        all_off = modding::set_addon(scan, all_off, addon.id(), false).choices;
    const auto status = modding::resolve_addons(scan, all_off);
    const net::ModCatalog off = modding::active_catalog(scan, status);
    check(off.mods.empty(), "every mod off: an empty catalog");

    Scratch nothing; // no mods installed at all
    const modding::AddonScan empty_scan = modding::scan_addons(three_roots(nothing));
    const net::ModCatalog none = modding::active_catalog(empty_scan, modding::resolve_addons(empty_scan, {}));
    check(empty_scan.addons.empty() && none.mods.empty(), "no mods installed: an empty catalog");

    for (const net::ProtocolVersion protocol : {net::ProtocolVersion::classic_v3, net::ProtocolVersion::modern_v4})
    {
        net::ModManifest with_off;
        net::ModManifest without;
        std::string error;
        check(net::make_mod_manifest(off, protocol, with_off, error) &&
                  net::make_mod_manifest(none, protocol, without, error),
              error);
        check(with_off.protocol == without.protocol && with_off.mods == without.mods &&
                  with_off.openminerz_only == without.openminerz_only,
              "the same manifest");
        check(!net::requires_extension(with_off), "stock players may join: no extension is required");

        net::LegacySteamRuntimeHostOptions host_off;
        host_off.settings.protocol = protocol;
        host_off.settings.mod_manifest = with_off;
        net::LegacySteamRuntimeHostOptions host_none = host_off;
        host_none.settings.mod_manifest = without;
        net::SteamBridgeHostConfig config_off;
        net::SteamBridgeHostConfig config_none;
        check(net::make_steam_bridge_host_config(host_off, config_off, error) &&
                  net::make_steam_bridge_host_config(host_none, config_none, error),
              error);
        net::SteamBridgeProtocolError encode_error{};
        const auto bytes_off = net::encode_steam_bridge_host_config(config_off, encode_error);
        const auto bytes_none = net::encode_steam_bridge_host_config(config_none, encode_error);
        check(bytes_off.has_value() && bytes_none.has_value() && *bytes_off == *bytes_none,
              "the lobby the host opens is byte for byte the same");

        // What a joining client presents to a vanilla host.
        const net::SessionModSelection join_off =
            net::select_session_mods(modding::join_catalog(scan, status), {}, nullptr, protocol);
        const net::SessionModSelection join_none = net::select_session_mods(none, {}, nullptr, protocol);
        check(join_off.manifest.mods == join_none.manifest.mods && join_off.manifest.mods.empty(),
              "and a join presents the same nothing");
    }
}

// Two world generators cannot both be on; neither can mods that say they
// conflict. Turning one on turns the other off and says so.
void conflicts()
{
    Scratch scratch;
    const auto roots = three_roots(scratch);
    make_mod(roots[2].path, "frontier", "server", {}, world_mod, "160");
    make_mod(roots[2].path, "wilderness", "server", "conflicts= Survival ,wildlife,wilderness\n", world_mod, "200");
    make_mod(roots[2].path, "survival", "server", {}, {}, "100");
    make_mod(roots[2].path, "wildlife", "server", {}, {}, "150");
    make_mod(roots[2].path, "story", "server");
    // Not generators: the call only in a comment, a string, or another function.
    make_mod(roots[2].path, "talker", "server", {},
             {{"server/main.luau", "-- terrain.define(x)\n--[[ terrain.define{} ]]\nlocal s = \"terrain.define(\"\n"
                                   "terrain.define_later = 1\nmyterrain.define(2)\n"},
              {"client/world.luau", "terrain.define({})\n"}});
    make_mod(roots[2].path, "spaced", "server", {}, {{"shared/a.luau", "local t = terrain . define {\n}\n"}});
    const modding::AddonScan scan = modding::scan_addons(roots);
    check(!scan.addons[index_of(scan, "talker")].defines_world, "comments, strings and other names are not a call");
    check(scan.addons[index_of(scan, "spaced")].defines_world, "spacing and table-call syntax are");
    const auto &wilderness = scan.addons[index_of(scan, "wilderness")];
    check(wilderness.conflicts == std::vector<std::string>{"survival", "wildlife"},
          "conflicts= is read lowercase and trimmed, without the mod itself");
    check(modding::addon_clash(wilderness, scan.addons[index_of(scan, "frontier")]) == modding::AddonClash::world &&
              modding::addon_clash(scan.addons[index_of(scan, "survival")], wilderness) == modding::AddonClash::declared &&
              modding::addon_clash(wilderness, scan.addons[index_of(scan, "story")]) == modding::AddonClash::none,
          "generator and declared clashes, either way round");

    // Frontier, Survival and Wildlife on; Wilderness turned on.
    modding::AddonChoices choices;
    for (const char *id : {"frontier", "survival", "wildlife"})
        choices = modding::set_addon(scan, choices, id, true).choices;
    check(modding::resolve_addons(scan, choices)[index_of(scan, "frontier")].on, "frontier is on");
    const modding::AddonChange change = modding::set_addon(scan, choices, "wilderness", true);
    const auto status = modding::resolve_addons(scan, change.choices);
    check(status[index_of(scan, "wilderness")].on && !status[index_of(scan, "frontier")].on &&
              !status[index_of(scan, "survival")].on && !status[index_of(scan, "wildlife")].on,
          "turning Wilderness on turns the other generator and the declared conflicts off");
    check(change.notices.size() == 3U &&
              std::any_of(change.notices.begin(), change.notices.end(),
                          [](const std::string &notice) {
                              return notice.starts_with("frontier turned off") &&
                                     notice.find("world generator") != std::string::npos;
                          }),
          "each with a short notice");
    check(ids(modding::active_catalog(scan, status)) == std::vector<std::string>{"wilderness"}, "one generator runs");

    // Both stored on (an edited file, a newer install): the player's own
    // choice wins over a default, and the loser says why.
    Scratch installed;
    const auto user_roots = three_roots(installed);
    make_mod(user_roots[1].path, "frontier", "server", {}, world_mod, "160");
    make_mod(user_roots[2].path, "wilderness", "server", {}, world_mod, "200");
    const modding::AddonScan both = modding::scan_addons(user_roots);
    const auto chosen = modding::resolve_addons(both, {{"wilderness", true}});
    check(chosen[index_of(both, "wilderness")].on && !chosen[index_of(both, "frontier")].on,
          "the explicit choice wins over the installed default");
    check(chosen[index_of(both, "frontier")].wanted &&
              chosen[index_of(both, "frontier")].reason.find("wilderness is on") != std::string::npos,
          "and the other row says why it is off");
    const auto both_chosen = modding::resolve_addons(both, {{"wilderness", true}, {"frontier", true}});
    check(both_chosen[index_of(both, "frontier")].on != both_chosen[index_of(both, "wilderness")].on &&
              both_chosen[index_of(both, "frontier")].on,
          "two explicit choices: the first in load order");
}

// Turning a mod on turns on what it needs; turning one off turns off what
// needs it.
void dependencies_follow()
{
    Scratch scratch;
    const auto roots = three_roots(scratch);
    make_mod(roots[2].path, "library", "server");
    make_mod(roots[2].path, "quests", "server", "depends=library\n");
    make_mod(roots[2].path, "epilogue", "server", "depends=quests@1.0\n");
    const modding::AddonScan scan = modding::scan_addons(roots);
    const modding::AddonChange on = modding::set_addon(scan, {}, "epilogue", true);
    const auto status = modding::resolve_addons(scan, on.choices);
    check(status[index_of(scan, "epilogue")].on && status[index_of(scan, "quests")].on &&
              status[index_of(scan, "library")].on && on.notices.size() == 2U,
          "on: its dependencies, transitively, with notices");
    const modding::AddonChange off = modding::set_addon(scan, on.choices, "library", false);
    const auto after = modding::resolve_addons(scan, off.choices);
    check(!after[index_of(scan, "library")].on && !after[index_of(scan, "quests")].on &&
              !after[index_of(scan, "epilogue")].on && off.notices.size() == 2U,
          "off: what needs it, transitively");
    // A dependency turned off by hand in the file: the dependent stays off and says why.
    const auto stale = modding::resolve_addons(scan, {{"quests", true}, {"library", false}});
    check(!stale[index_of(scan, "quests")].on && stale[index_of(scan, "quests")].reason.find("library") != std::string::npos,
          "a mod whose dependency is off stays off, with the reason");
}

// The shipped examples, as the build copies them beside the game.
void the_examples()
{
    const std::vector<modding::AddonRoot> roots{
        {fs::path{OPENMINERZ_SOURCE_DIR} / "examples" / "mods", modding::AddonOrigin::bundled}};
    const modding::AddonScan scan = modding::scan_addons(roots);
    check(scan.addons.size() >= 9U, "every example is listed");
    for (const modding::InstalledAddon &addon : scan.addons)
        check(addon.usable(), addon.id() + " loads: " + addon.problem);
    const auto status = modding::resolve_addons(scan, {});
    check(modding::active_catalog(scan, status).mods.empty(), "bundled examples all start off: the stock game");
    for (const modding::InstalledAddon &addon : scan.addons)
        check(addon.defines_world == (addon.id() == "frontier" || addon.id() == "wilderness"),
              addon.id() + ": only Frontier and Wilderness replace the world generator");
    const auto &wilderness = scan.addons[index_of(scan, "wilderness")];
    check(wilderness.conflicts == std::vector<std::string>{"survival", "wildlife"},
          "Wilderness declares Survival and Wildlife as conflicts");
    modding::AddonChoices choices;
    for (const char *id : {"frontier", "survival", "wildlife", "ui_theme"})
        choices = modding::set_addon(scan, choices, id, true).choices;
    const modding::AddonChange change = modding::set_addon(scan, choices, "wilderness", true);
    const auto after = modding::resolve_addons(scan, change.choices);
    check(ids(modding::active_catalog(scan, after)) == std::vector<std::string>{"wilderness", "ui_theme"},
          "Wilderness on: Frontier, Survival and Wildlife off, the theme untouched");
    check(change.notices.size() == 3U, "three notices");
}

// The screen's rows: Xbox Avatars, the installed mods, then mod rows.
void menu_rows()
{
    Settings settings;
    ui::MenuModel menu;
    menu.open(ui::MenuScreen::addons);
    const std::size_t avatars = menu.entries().size();
    check(avatars == 3U && !menu.selected_addon_entry(), "the Xbox Avatars rows alone");
    std::vector<ui::MenuEntry> rows{{"Wilderness", "", ui::MenuCommand::toggle_addon, true, {}},
                                    {"Frontier", "", ui::MenuCommand::toggle_addon, true, {}},
                                    {"Open Mods Folder", "", ui::MenuCommand::open_mods_folder, true, {}}};
    menu.set_addon_entries(rows);
    menu.set_mod_entries(ui::MenuScreen::addons, {{"Ember Theme Settings", "", ui::MenuCommand::none, true, {}}});
    const auto &entries = menu.entries();
    check(entries.size() == 7U && entries[3].label == "Wilderness" && entries[5].command == ui::MenuCommand::open_mods_folder &&
              entries[6].command == ui::MenuCommand::mod_entry,
          "avatars, installed mods, Open Mods Folder, then the rows mods add");
    menu.set_selection(4U, settings);
    check(menu.selected_addon_entry() == 1U && !menu.selected_mod_entry(), "the second installed mod is selected");
    check(menu.activate(settings).command == ui::MenuCommand::toggle_addon && menu.screen() == ui::MenuScreen::addons,
          "choosing it toggles, on the same screen");
    menu.set_selection(6U, settings);
    check(menu.selected_mod_entry() == 0U && !menu.selected_addon_entry(), "the mod row is still the mod row");
    menu.set_addon_entries({});
    check(menu.entries().size() == 4U && menu.selection() == 3U, "fewer rows: the selection stays in range");
    menu.open(ui::MenuScreen::main);
    check(!menu.selected_addon_entry(), "no add-on row off the Add-Ons screen");
}

struct TestCase final
{
    const char *name;
    void (*body)();
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"installed_on_bundled_off", installed_on_bundled_off},
        {"a_broken_mod_is_listed_not_fatal", a_broken_mod_is_listed_not_fatal},
        {"installed_copy_replaces_the_bundled_one", installed_copy_replaces_the_bundled_one},
        {"choices_persist", choices_persist},
        {"off_mods_and_joining", off_mods_and_joining},
        {"every_mod_off_is_retail_on_the_wire", every_mod_off_is_retail_on_the_wire},
        {"conflicts", conflicts},
        {"dependencies_follow", dependencies_follow},
        {"the_examples", the_examples},
        {"menu_rows", menu_rows},
    };
    std::size_t failures{};
    for (const auto &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
