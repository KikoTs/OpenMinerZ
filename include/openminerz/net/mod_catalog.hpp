#pragma once

#include "openminerz/net/mod_handshake.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    // (Runtime) The optional descriptor keys (name, author, description,
    // website, api, depends; mod_descriptor_optional_keys), lowercase key ->
    // value as written. Never part of admission; modding/package.hpp reads them.
    std::vector<std::pair<std::string, std::string>> metadata;
};

// (Runtime) Descriptor keys allowed besides the five required ones.
// (Add-Ons) "conflicts" names mods that cannot be on together with this one
// (modding/addons.hpp); like the others it stays on this machine and is never
// part of admission or of any message.
inline constexpr std::string_view mod_descriptor_optional_keys[]{
    "name", "author", "description", "website", "api", "depends", "conflicts"};

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
// (Add-Ons) One child of a root, read exactly as load_mod_catalog reads each:
// the descriptor and the digest of the whole tree. The Add-Ons screen reads
// its roots mod by mod, so one broken mod is listed instead of hiding the rest.
[[nodiscard]] bool read_mod_folder(const std::filesystem::path &folder, LoadedMod &mod,
                                   std::string &error);

// Selects the requested deployed protocol and preserves catalog load order.
// Client-only entries remain visible in the local manifest but are ignored by
// synchronized admission comparison exactly as specified by ModScope.
[[nodiscard]] bool make_mod_manifest(const ModCatalog &catalog, ProtocolVersion protocol,
                                     ModManifest &manifest, std::string &error);

[[nodiscard]] const char *mod_scope_label(ModScope scope) noexcept;

// --- Content listing and digests (shared with net/mod_download.hpp) -------

using ModDigest = std::array<std::uint8_t, 32U>;

struct ModContentFile final
{
    std::string path; // normalized: lowercase, '/' separators, relative
    std::filesystem::path source;
    std::uint64_t size{};
};

// (Polish) Entries a mod folder may hold that are not part of the mod: any
// file or folder whose name starts with '.' (.git, .vscode, .luaurc, ...)
// and the shell's Thumbs.db and desktop.ini, at any depth. Every mod path
// skips them: loading, the file-type check, content and client digests,
// downloads, the download cache and the SDK's validate and pack.
[[nodiscard]] bool is_ignored_mod_entry(const std::filesystem::path &name) noexcept;
// True when any segment of a '/'-separated relative path is such a name.
[[nodiscard]] bool is_ignored_mod_path(std::string_view relative_path) noexcept;

// Enumerates a mod tree exactly as load_mod_catalog hashes it: ignored
// entries skipped, links refused, file/byte budgets applied, sorted by
// normalized path.
[[nodiscard]] bool list_mod_content(const std::filesystem::path &root,
                                    std::vector<ModContentFile> &files, std::string &error);
// SHA-256 of the catalog digest stream over `files`: for each, the normalized
// path, a NUL, its u64 little-endian size and its bytes. Over a whole tree
// this is ModDescriptor::content_digest.
[[nodiscard]] bool digest_mod_content(std::span<const ModContentFile> files, ModDigest &digest,
                                      std::string &error);
[[nodiscard]] ModDigest sha256_digest(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::string digest_hex(const ModDigest &digest);
[[nodiscard]] bool parse_digest_hex(std::string_view text, ModDigest &digest) noexcept;

// The client-visible part of a mod: the descriptor plus the files under
// shared/, client/, assets/ and lang/ whose extension is a script or data
// type below (see net/mod_download.hpp). Never server/, never anything else.
inline constexpr std::string_view client_visible_folders[]{"shared/", "client/", "assets/",
                                                           "lang/"};
inline constexpr std::string_view client_visible_extensions[]{
    "luau", "lua",                                                  // scripts
    "json", "txt", "md", "csv", "toml", "yaml", "yml", "ini", "cfg", // data
    "png", "jpg", "jpeg", "bmp", "tga", "gif", "hdr", "dds", "ktx", "ktx2", // images
    "gltf", "glb", "bin", "obj", "mtl",                             // models
    "ogg", "wav", "mp3", "flac",                                    // audio
    "ttf", "otf", "fnt"};                                           // fonts
[[nodiscard]] bool is_client_visible_mod_path(std::string_view normalized_path) noexcept;
// Its digest: digest_mod_content over the client-visible files only. Equal
// to the content digest for a mod that has no other files.
[[nodiscard]] bool client_visible_digest(const std::filesystem::path &root, ModDigest &digest,
                                         std::uint64_t &bytes, std::string &error);

// --- Downloaded mods ---------------------------------------------------------

inline constexpr std::string_view mod_download_record_filename = ".omz-download";

// The record's text: id, version, scope, content and client digests.
[[nodiscard]] std::string format_mod_download_record(const ModDescriptor &descriptor,
                                                     const ModDigest &client_digest);

// Loads the per-user download cache: <root>/<content digest hex>/ holding a
// mod's client-visible files and a .omz-download record (id, version, scope,
// content and client digests). Each entry is re-verified (the folder name,
// the record, the descriptor and the client digest of the files present);
// an entry that fails is skipped and named in `skipped`, never fatal. The
// loaded descriptors carry the CONTENT digest the host advertised, which is
// what admission compares.
// One cache folder (any name) holding the mod whose content digest is
// `content`: the same checks load_mod_cache applies to each entry.
[[nodiscard]] bool load_cached_mod(const std::filesystem::path &folder, const ModDigest &content,
                                   LoadedMod &mod, std::string &error);
struct ModCacheLoad final
{
    ModCatalog catalog;
    std::vector<std::string> skipped;
};
[[nodiscard]] ModCacheLoad load_mod_cache(const std::filesystem::path &root);

// What this client presents for one session. Client-scope mods come from the
// local catalog; the host's synchronized mods (`required`, null for a host
// that advertises none) come from the local catalog or the download cache
// when id, version, scope and content digest all match, in the host's order.
// Synchronized mods the host does not require are left out, so installing a
// server mod never blocks joining a vanilla server.
struct SessionModSelection final
{
    ModManifest manifest;
    std::vector<LoadedMod> mods;        // backing `manifest`, in its order
    std::vector<ModDescriptor> missing; // required but unavailable here
};
[[nodiscard]] SessionModSelection select_session_mods(const ModCatalog &local,
                                                      const ModCatalog &cache,
                                                      const ModManifest *required,
                                                      ProtocolVersion protocol);

} // namespace openminerz::net
