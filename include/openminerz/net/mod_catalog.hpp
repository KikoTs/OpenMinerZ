#pragma once

#include "openminerz/net/mod_handshake.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace openminerz::net
{

struct LoadedMod final
{
    ModDescriptor descriptor;
    std::filesystem::path root;
    std::uint32_t load_order{};
    bool classic{};
    bool modern{};
};

struct ModCatalog final
{
    std::vector<LoadedMod> mods;
};

inline constexpr std::string_view mod_descriptor_filename = "openminerz.mod";
inline constexpr std::size_t maximum_catalog_mods = 256U;
inline constexpr std::size_t maximum_mod_files = 4096U;
inline constexpr std::uint64_t maximum_mod_file_bytes = 64U * 1024U * 1024U;
inline constexpr std::uint64_t maximum_mod_content_bytes = 256U * 1024U * 1024U;

// Loads strict key=value descriptors from immediate child directories and
// hashes each complete directory tree in normalized path order. Missing roots
// are a valid empty catalog; malformed descriptors, links, duplicate IDs, and
// exceeded file/byte budgets fail the entire catalog before any mod is active.
[[nodiscard]] bool load_mod_catalog(const std::filesystem::path &root, ModCatalog &catalog,
                                    std::string &error);

// Selects the requested deployed protocol and preserves catalog load order.
// Client-only entries remain visible in the local manifest but are ignored by
// synchronized admission comparison exactly as specified by ModScope.
[[nodiscard]] bool make_mod_manifest(const ModCatalog &catalog, ProtocolVersion protocol,
                                     ModManifest &manifest, std::string &error);

[[nodiscard]] const char *mod_scope_label(ModScope scope) noexcept;

} // namespace openminerz::net
