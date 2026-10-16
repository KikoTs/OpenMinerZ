#pragma once

// Mod packages on disk: where they live, what a descriptor may say beyond the
// five keys admission compares, which files a package may contain, and the
// load order the runtime uses.
//
// Layout of a package folder (an immediate child of a mod root):
//
//   openminerz.mod   key=value descriptor (net/mod_catalog.hpp)
//   shared/          Luau that runs on both sides (definitions, helpers)
//   client/          Luau for the client VM only
//   server/          Luau for the server VM only (never sent to clients)
//   assets/          textures, models, sounds, fonts, UI data
//   lang/            strings
//
// Every .luau/.lua file directly inside shared/ and then the side's folder
// runs once at load, in name order; files in subfolders are modules that
// scripts require(). Zipped packages are not supported yet: install folders.
//
// Descriptor keys. Required and compared by multiplayer admission (unchanged):
//   id, version, scope (client|server|core), protocol (classic|modern|both),
//   order (0-1000000)
// Optional, for people and the runtime (never part of admission):
//   name, author, description, website   shown on the Add-Ons screen
//   api      the modding API version the mod was written for (default 1);
//            a mod for a newer API than this game's is refused
//   depends  comma-separated "id" or "id@minimum-version" entries; those
//            mods load first and may be required as "@id/..."

#include "openminerz/modding/runtime.hpp"
#include "openminerz/net/mod_catalog.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openminerz::modding
{

// Lowercase extensions (no dot) a package may contain. Scripts, text and data,
// images, models, audio and fonts: nothing a machine could execute. The same
// list as net::client_visible_extensions plus "lang".
inline constexpr std::string_view package_file_extensions[]{
    "luau", "lua",                                                          // scripts
    "json", "txt", "md", "csv", "toml", "yaml", "yml", "ini", "cfg", "lang", // text and data
    "png", "jpg", "jpeg", "bmp", "tga", "gif", "hdr", "dds", "ktx", "ktx2", // images
    "gltf", "glb", "bin", "obj", "mtl",                                     // models
    "ogg", "wav", "mp3", "flac",                                            // audio
    "ttf", "otf", "fnt"};                                                   // fonts

// Whether a normalized (lowercase, '/'-separated) package path may ship:
// the descriptor, or a file whose extension is in package_file_extensions.
[[nodiscard]] bool is_allowed_package_file(std::string_view normalized_path) noexcept;

struct PackageDependency final
{
    std::string id;
    std::string minimum_version; // "" for any
};

struct PackageInfo final
{
    std::string name;
    std::string author;
    std::string description;
    std::string website;
    std::uint32_t api{api_version};
    std::vector<PackageDependency> depends;
};

// Reads the optional descriptor keys the catalog kept for `mod`.
[[nodiscard]] bool parse_package_info(const net::LoadedMod &mod, PackageInfo &info,
                                      std::string &error);

// "1.10" >= "1.9"; dotted numeric parts compare as numbers, anything else as
// text.
[[nodiscard]] bool version_at_least(std::string_view version, std::string_view minimum);

// --- Where mods live ---------------------------------------------------------

// %LOCALAPPDATA%\OpenMinerZ\Mods: per-user mods, so installing one needs no
// write access to the game folder ("" when the folder is unknown).
[[nodiscard]] std::filesystem::path user_mod_root();
// %LOCALAPPDATA%\OpenMinerZ\ModData: client storage and mod settings.
[[nodiscard]] std::filesystem::path mod_data_root();
// addons/ in the working directory (the game's folder when launched
// normally, as before), then the per-user root.
[[nodiscard]] std::vector<std::filesystem::path> default_mod_roots();

// net::load_mod_catalog over several roots, merged in the catalog's own order
// (order, then id). An id present in two roots is an error, as within one.
[[nodiscard]] bool load_mod_catalogs(std::span<const std::filesystem::path> roots,
                                     net::ModCatalog &catalog, std::string &error);

// --- Loading -----------------------------------------------------------------

// Reads a package for the runtime: refuses files outside the allowlist and
// mods for a newer API, then collects its scripts.
[[nodiscard]] bool read_package(const net::LoadedMod &mod, ModSource &source, std::string &error);

struct PackagePlan final
{
    std::vector<ModSource> mods;                              // load order
    std::vector<std::pair<std::string, std::string>> refused; // id, reason
};

// The runtime's load order for one session's mods (given in catalog/manifest
// order): every mod after its dependencies, otherwise unchanged. A mod whose
// dependency is missing, too old, refused or cyclic is refused with a reason,
// and so are the mods that depend on it.
[[nodiscard]] PackagePlan plan_packages(std::span<const net::LoadedMod> mods);

} // namespace openminerz::modding
