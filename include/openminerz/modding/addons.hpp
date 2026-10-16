#pragma once

// The Add-Ons screen's model: every mod installed in any mod root, whether
// the player has it on, and the catalog the game uses - only the mods that
// are on. Nothing here touches the network or a VM. The app turns
// active_catalog() into VMs, asset stacks and manifests exactly as it turned
// the whole installed catalog into them before, so a mod that is off runs
// nowhere, adds no assets and is in neither the host's manifest nor the join
// client's: with every server mod off, hosting is what it is with no mods
// installed (tests/test_mod_addons.cpp holds the two equal).
//
// Roots, earliest first; an id installed in an earlier root wins:
//   game     addons\ in the working directory (the game's folder)  on by default
//   user     %LOCALAPPDATA%\OpenMinerZ\Mods                         on by default
//   bundled  examples\mods\ beside openminerz.exe, the example
//            mods the build ships                                  off by default
// A bundled mod whose id is also installed in the game or user root is
// hidden: the player's copy replaces it. The same id in the game and the
// user root is a problem the later copy shows ("installed twice").
//
// Joining an OpenMinerZ host, the host's server mods decide: a server mod the
// host runs is used even if it is off here (join_catalog), exactly as a
// download of the same files would be. Client mods that are off stay off.
//
// Conflicts: two server mods whose scripts define the world generator
// (terrain.define) cannot both be on, and neither can two mods when one names
// the other in its descriptor's conflicts= key (local only, like name and
// author: never part of admission or of a message). Turning one on turns the
// others off. Dependencies follow too: turning a mod on turns on the mods it
// depends on, turning one off turns off the mods that need it.

#include "openminerz/modding/package.hpp"
#include "openminerz/net/mod_catalog.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::modding
{

enum class AddonOrigin : std::uint8_t
{
    game,   // addons\ in the game's folder
    user,   // the per-user Mods folder
    bundled // the examples shipped beside the executable
};

// "Game folder", "Mods folder", "Bundled with the game".
[[nodiscard]] std::string_view addon_origin_label(AddonOrigin origin) noexcept;

// A mod the player never chose for or against: bundled examples start off,
// so the stock game is unchanged until one is turned on; anything the player
// installed starts on.
[[nodiscard]] constexpr bool addon_on_by_default(AddonOrigin origin) noexcept
{
    return origin != AddonOrigin::bundled;
}

struct AddonRoot final
{
    std::filesystem::path path;
    AddonOrigin origin{AddonOrigin::game};
};

// examples\mods beside the executable that lives in `application_directory`.
[[nodiscard]] std::filesystem::path bundled_mod_root(const std::filesystem::path &application_directory);
// default_mod_roots() (the game's addons\, then the user's Mods folder), then
// `bundled` unless it is empty.
[[nodiscard]] std::vector<AddonRoot> addon_roots(const std::filesystem::path &bundled);

struct InstalledAddon final
{
    // Descriptor, root and tree digest. A mod whose descriptor cannot be read
    // has only its folder name, as its id.
    net::LoadedMod mod;
    AddonOrigin origin{AddonOrigin::game};
    PackageInfo info;                   // name (the id when it has none), author, ...
    std::vector<std::string> conflicts; // its conflicts= ids, lowercase
    bool defines_world{};               // a server/core mod whose scripts call terrain.define
    std::string problem;                // why it cannot be used at all ("" = usable)

    [[nodiscard]] bool usable() const noexcept
    {
        return problem.empty();
    }
    [[nodiscard]] const std::string &id() const noexcept
    {
        return mod.descriptor.id;
    }
};

struct AddonScan final
{
    std::vector<InstalledAddon> addons; // by name, as the Add-Ons screen lists them
    std::vector<std::string> hidden;    // bundled ids an installed copy replaces
    std::vector<std::string> errors;    // roots that could not be listed at all
};

// Reads every root mod by mod: descriptor, digest, package files, optional
// keys and whether the scripts define the world. Never fails as a whole.
[[nodiscard]] AddonScan scan_addons(std::span<const AddonRoot> roots);

// Whether a mod's shared/ or server/ scripts call terrain.define (comments
// and strings aside): it replaces the world generator.
[[nodiscard]] bool scripts_define_world(const ModSource &source);

enum class AddonClash : std::uint8_t
{
    none,
    world,   // both replace the world generator
    declared // one names the other in conflicts=
};

// Whether two usable mods cannot both be on. Mods that never run in the same
// game version (classic only and modern only) never clash.
[[nodiscard]] AddonClash addon_clash(const InstalledAddon &first, const InstalledAddon &second) noexcept;

// --- The player's choices ------------------------------------------------------

// Lowercase mod id -> on. A mod with no entry takes its origin's default.
using AddonChoices = std::map<std::string, bool, std::less<>>;

// %APPDATA%\OpenMinerZ\addons.cfg beside settings.cfg: "on=<id>" and
// "off=<id>" lines, '#' comments. A missing or unreadable file is no choices.
[[nodiscard]] AddonChoices read_addon_choices(const std::filesystem::path &file);
[[nodiscard]] bool write_addon_choices(const std::filesystem::path &file, const AddonChoices &choices,
                                       std::string &error);

struct AddonStatus final
{
    bool on{};          // in the active catalog
    bool wanted{};      // the player's choice (or the default) is on
    std::string reason; // why it is off although wanted, or its problem
};

// One status per scan.addons entry. Wanted mods are taken the player's
// explicit choices first, then defaults, each in load order; a mod that
// conflicts with one already on, or needs one that is off, stays off with a
// reason.
[[nodiscard]] std::vector<AddonStatus> resolve_addons(const AddonScan &scan, const AddonChoices &choices);

// The mods that are on, in load order: the catalog everything else uses.
[[nodiscard]] net::ModCatalog active_catalog(const AddonScan &scan, std::span<const AddonStatus> status);
// What a join may use: the mods that are on plus the usable server and core
// mods that are off (net::select_session_mods takes only the ones the host
// requires, so an off server mod never runs anywhere else).
[[nodiscard]] net::ModCatalog join_catalog(const AddonScan &scan, std::span<const AddonStatus> status);

struct AddonChange final
{
    AddonChoices choices;             // what to store
    std::vector<std::string> notices; // what else changed, one sentence each
};

// The player turned the mod `id` on or off: the new choices, with the mods
// that conflict with it turned off and its dependencies (on) or dependents
// (off) following.
[[nodiscard]] AddonChange set_addon(const AddonScan &scan, const AddonChoices &choices, std::string_view id,
                                    bool on);

[[nodiscard]] const InstalledAddon *find_addon(const AddonScan &scan, std::string_view id) noexcept;

} // namespace openminerz::modding
