#pragma once

// `openminerz_mod_sdk validate`: everything the game would refuse or trip
// over in a mod folder, found before the game runs it.
//
//   - the descriptor, its optional keys and dependencies (the catalog's and
//     the runtime's own parsers, so the verdict is the game's);
//   - files the package may not contain, links, size budgets and the join
//     download cap for server mods;
//   - every script parsed and compiled with the game's Luau, errors with
//     file:line;
//   - API use checked against the registry: unknown functions and events,
//     functions or events of the other side, capabilities the mod's scope
//     does not get, writes to library tables, sandboxed-away globals;
//   - require() resolved with the runtime's rules (missing modules,
//     undeclared dependencies, server code required on a client, cycles);
//   - lang files, asset names, and asset paths named in scripts that do not
//     exist.

#include "openminerz/modding/api.hpp"
#include "openminerz/net/mod_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace openminerz::mod_sdk
{

struct Diagnostic final
{
    enum class Level : std::uint8_t
    {
        error,
        warning,
        info // (Polish) worth knowing, never a problem
    };
    Level level{Level::error};
    std::string file; // mod-relative, "" for the mod as a whole
    int line{};       // 1-based, 0 when not about a line
    int column{};
    std::string message;
};

struct ValidationResult final
{
    bool loaded{}; // the descriptor and catalog accepted the folder
    net::LoadedMod mod;
    std::vector<Diagnostic> diagnostics;
    std::size_t files{};
    std::size_t scripts{};
    std::uint64_t bytes{};
    std::uint64_t client_bytes{}; // what a joining player downloads (server mods)
    std::string digest;           // content digest (hex), what admission compares
    std::string client_digest;

    [[nodiscard]] std::size_t count(Diagnostic::Level level) const noexcept;
    [[nodiscard]] bool ok() const noexcept
    {
        return loaded && count(Diagnostic::Level::error) == 0U;
    }
};

[[nodiscard]] ValidationResult validate_mod(const std::filesystem::path &folder,
                                            const modding::ApiRegistry &registry);

// "file:line:column: error: message" lines, then a summary.
[[nodiscard]] std::string format_diagnostics(const ValidationResult &result);

} // namespace openminerz::mod_sdk
