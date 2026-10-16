#include "openminerz/net/mod_catalog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::size_t maximum_descriptor_bytes = 16U * 1024U;

std::string trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
        value.remove_prefix(1U);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
        value.remove_suffix(1U);
    return std::string{value};
}

std::string lowercase(std::string_view value)
{
    std::string result{value};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

class Sha256 final
{
  public:
    void update(std::span<const std::uint8_t> bytes) noexcept
    {
        total_bytes_ += bytes.size();
        while (!bytes.empty())
        {
            const std::size_t copy = std::min(bytes.size(), block_.size() - buffered_);
            std::copy_n(bytes.begin(), copy,
                        block_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            buffered_ += copy;
            bytes = bytes.subspan(copy);
            if (buffered_ == block_.size())
            {
                transform(block_);
                buffered_ = 0U;
            }
        }
    }

    std::array<std::uint8_t, 32U> finish() noexcept
    {
        const std::uint64_t bit_length = total_bytes_ * 8U;
        block_[buffered_++] = 0x80U;
        if (buffered_ > 56U)
        {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(buffered_), block_.end(),
                      std::uint8_t{});
            transform(block_);
            buffered_ = 0U;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(buffered_), block_.begin() + 56,
                  std::uint8_t{});
        for (std::size_t index{}; index < 8U; ++index)
            block_[63U - index] = static_cast<std::uint8_t>(bit_length >> (index * 8U));
        transform(block_);

        std::array<std::uint8_t, 32U> digest{};
        for (std::size_t word{}; word < state_.size(); ++word)
            for (std::size_t byte{}; byte < 4U; ++byte)
                digest[word * 4U + byte] =
                    static_cast<std::uint8_t>(state_[word] >> ((3U - byte) * 8U));
        return digest;
    }

  private:
    static constexpr std::array<std::uint32_t, 64U> constants{
        0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
        0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
        0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
        0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
        0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
        0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
        0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
        0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
        0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
        0xC67178F2U};

    static std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
    {
        return (x & y) ^ (~x & z);
    }
    static std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
    {
        return (x & y) ^ (x & z) ^ (y & z);
    }
    static std::uint32_t big0(std::uint32_t x) noexcept
    {
        return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
    }
    static std::uint32_t big1(std::uint32_t x) noexcept
    {
        return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
    }
    static std::uint32_t small0(std::uint32_t x) noexcept
    {
        return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3U);
    }
    static std::uint32_t small1(std::uint32_t x) noexcept
    {
        return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10U);
    }

    void transform(std::span<const std::uint8_t, 64U> block) noexcept
    {
        std::array<std::uint32_t, 64U> words{};
        for (std::size_t index{}; index < 16U; ++index)
            words[index] = (static_cast<std::uint32_t>(block[index * 4U]) << 24U) |
                           (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U) |
                           static_cast<std::uint32_t>(block[index * 4U + 3U]);
        for (std::size_t index = 16U; index < words.size(); ++index)
            words[index] = small1(words[index - 2U]) + words[index - 7U] +
                           small0(words[index - 15U]) + words[index - 16U];

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index{}; index < words.size(); ++index)
        {
            const std::uint32_t first =
                h + big1(e) + choose(e, f, g) + constants[index] + words[index];
            const std::uint32_t second = big0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8U> state_{0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
                                         0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
    std::array<std::uint8_t, 64U> block_{};
    std::size_t buffered_{};
    std::uint64_t total_bytes_{};
};

void hash_u64(Sha256 &hash, std::uint64_t value) noexcept
{
    std::array<std::uint8_t, 8U> bytes{};
    for (std::size_t index{}; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    hash.update(bytes);
}

bool hash_tree(const std::filesystem::path &root, std::array<std::uint8_t, 32U> &digest,
               std::string &error)
{
    std::vector<ModContentFile> files;
    return list_mod_content(root, files, error) && digest_mod_content(files, digest, error);
}

bool parse_descriptor(const std::filesystem::path &path, LoadedMod &mod, std::string &error);

// (Polish) One file or folder name, narrow or wide, compared without
// converting it (it need not be ASCII).
template <typename Text> bool ignored_name(const Text &name) noexcept
{
    if (name.empty())
        return false;
    if (name.front() == '.')
        return true;
    const auto named = [&](std::string_view wanted) {
        return name.size() == wanted.size() &&
               std::equal(name.begin(), name.end(), wanted.begin(), [](auto have, char want) {
                   const auto code = static_cast<std::uint32_t>(have);
                   return code < 128U && std::tolower(static_cast<int>(code)) == want;
               });
    };
    return named("thumbs.db") || named("desktop.ini");
}

} // namespace

bool is_ignored_mod_entry(const std::filesystem::path &name) noexcept
{
    return ignored_name(name.native());
}

bool is_ignored_mod_path(std::string_view path) noexcept
{
    std::size_t start{};
    while (start < path.size())
    {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if (ignored_name(path.substr(start, end - start)))
            return true;
        start = end + 1U;
    }
    return false;
}

bool list_mod_content(const std::filesystem::path &root, std::vector<ModContentFile> &result,
                      std::string &error)
{
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    std::error_code scan_error;
    for (std::filesystem::recursive_directory_iterator iterator{root, scan_error}, end;
         iterator != end; iterator.increment(scan_error))
    {
        if (scan_error)
        {
            error = "Cannot enumerate mod content: " + scan_error.message();
            return false;
        }
        // (Polish) Editor and repository folders are not part of the mod,
        // and neither is anything inside them.
        if (is_ignored_mod_entry(iterator->path().filename()))
        {
            iterator.disable_recursion_pending();
            continue;
        }
        const std::filesystem::file_status status = iterator->symlink_status(scan_error);
        if (scan_error || std::filesystem::is_symlink(status))
        {
            error = "Mod content may not contain symbolic links";
            return false;
        }
        if (!std::filesystem::is_regular_file(status))
            continue;
        std::filesystem::path relative =
            std::filesystem::relative(iterator->path(), root, scan_error);
        if (scan_error || relative.empty() || relative.is_absolute())
        {
            error = "A mod content path cannot be normalized";
            return false;
        }
        std::string normalized = relative.generic_string();
        if (normalized.size() > 1024U)
        {
            error = "A mod content path exceeds 1024 bytes";
            return false;
        }
        files.emplace_back(lowercase(normalized), iterator->path());
        if (files.size() > maximum_mod_files)
        {
            error = "A mod exceeds the 4096-file content budget";
            return false;
        }
    }
    std::sort(files.begin(), files.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    for (std::size_t index = 1U; index < files.size(); ++index)
        if (files[index - 1U].first == files[index].first)
        {
            error = "A mod contains case-colliding content paths";
            return false;
        }

    std::vector<ModContentFile> listed;
    listed.reserve(files.size());
    std::uint64_t total_bytes{};
    for (auto &[relative, path] : files)
    {
        const std::uint64_t file_size = std::filesystem::file_size(path, scan_error);
        if (scan_error || file_size > maximum_mod_file_bytes ||
            total_bytes > maximum_mod_content_bytes - file_size)
        {
            error = "A mod exceeds its bounded file or aggregate content budget";
            return false;
        }
        total_bytes += file_size;
        listed.push_back({std::move(relative), std::move(path), file_size});
    }
    result = std::move(listed);
    error.clear();
    return true;
}

bool digest_mod_content(std::span<const ModContentFile> files, ModDigest &digest,
                        std::string &error)
{
    Sha256 hash;
    std::vector<std::uint8_t> buffer(64U * 1024U);
    for (const ModContentFile &file : files)
    {
        hash.update(
            {reinterpret_cast<const std::uint8_t *>(file.path.data()), file.path.size()});
        const std::array<std::uint8_t, 1U> separator{};
        hash.update(separator);
        hash_u64(hash, file.size);
        std::ifstream input{file.source, std::ios::binary};
        if (!input)
        {
            error = "A mod content file cannot be opened";
            return false;
        }
        std::uint64_t bytes_read{};
        while (input)
        {
            input.read(reinterpret_cast<char *>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0)
            {
                hash.update(
                    std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(count)});
                bytes_read += static_cast<std::uint64_t>(count);
            }
        }
        if (!input.eof() || bytes_read != file.size)
        {
            error = "A mod content file changed or could not be read completely";
            return false;
        }
    }
    digest = hash.finish();
    error.clear();
    return true;
}

ModDigest sha256_digest(std::span<const std::uint8_t> bytes) noexcept
{
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

std::string digest_hex(const ModDigest &digest)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2U);
    for (const std::uint8_t value : digest)
    {
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 0x0FU]);
    }
    return text;
}

bool parse_digest_hex(std::string_view text, ModDigest &digest) noexcept
{
    if (text.size() != digest.size() * 2U)
        return false;
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0U; index < digest.size(); ++index)
    {
        const int high = nibble(text[index * 2U]);
        const int low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0)
            return false;
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool is_client_visible_mod_path(std::string_view path) noexcept
{
    if (path == mod_descriptor_filename)
        return true;
    // (Polish) Never sent, so never accepted from a host either.
    if (is_ignored_mod_path(path))
        return false;
    const bool in_folder =
        std::any_of(std::begin(client_visible_folders), std::end(client_visible_folders),
                    [&](std::string_view folder) {
                        return path.starts_with(folder) && path.size() > folder.size();
                    });
    const std::size_t dot = path.rfind('.');
    if (!in_folder || dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos)
        return false;
    // Scripts and data only: nothing a machine could execute natively.
    const std::string_view extension = path.substr(dot + 1U);
    return std::any_of(std::begin(client_visible_extensions), std::end(client_visible_extensions),
                       [&](std::string_view allowed) { return extension == allowed; });
}

namespace
{

std::vector<ModContentFile> client_visible_files(std::vector<ModContentFile> files)
{
    std::erase_if(files,
                  [](const ModContentFile &file) { return !is_client_visible_mod_path(file.path); });
    return files;
}

bool parse_descriptor(const std::filesystem::path &path, LoadedMod &mod, std::string &error)
{
    std::error_code file_error;
    const std::uint64_t size = std::filesystem::file_size(path, file_error);
    if (file_error || size == 0U || size > maximum_descriptor_bytes)
    {
        error = "openminerz.mod must contain 1-16384 bytes";
        return false;
    }
    std::ifstream input{path, std::ios::binary};
    std::string content(static_cast<std::size_t>(size), '\0');
    if (!input.read(content.data(), static_cast<std::streamsize>(content.size())))
    {
        error = "openminerz.mod could not be read completely";
        return false;
    }
    if (content.find('\0') != std::string::npos)
    {
        error = "openminerz.mod must be plain UTF-8 text without NUL bytes";
        return false;
    }

    std::map<std::string, std::string> values;
    // (Integration) A UTF-8 byte order mark (Notepad, PowerShell) is not part
    // of the first key. The digest still covers the file's bytes as they are.
    std::size_t offset = content.starts_with("\xEF\xBB\xBF") ? 3U : 0U;
    while (offset <= content.size())
    {
        const std::size_t end = content.find('\n', offset);
        std::string line = trim(std::string_view{content}.substr(
            offset, end == std::string::npos ? content.size() - offset : end - offset));
        if (!line.empty() && line.front() != '#')
        {
            const std::size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                error = "Every mod descriptor field must use key=value syntax";
                return false;
            }
            std::string key = lowercase(trim(std::string_view{line}.substr(0U, separator)));
            std::string value = trim(std::string_view{line}.substr(separator + 1U));
            if (key.empty() || value.empty() ||
                !values.emplace(std::move(key), std::move(value)).second)
            {
                error = "Mod descriptor fields must be nonempty and unique";
                return false;
            }
        }
        if (end == std::string::npos)
            break;
        offset = end + 1U;
    }
    const std::set<std::string> expected{"id", "version", "scope", "protocol", "order"};
    // (Runtime) The five keys stay required with their meaning; the optional
    // keys are kept for modding/package.hpp. Anything else is still an error.
    const auto optional = [](std::string_view key) {
        return std::find(std::begin(mod_descriptor_optional_keys),
                         std::end(mod_descriptor_optional_keys),
                         key) != std::end(mod_descriptor_optional_keys);
    };
    if (std::any_of(expected.begin(), expected.end(),
                    [&](const std::string &key) { return !values.contains(key); }) ||
        std::any_of(values.begin(), values.end(), [&](const auto &entry) {
            return !expected.contains(entry.first) && !optional(entry.first);
        }))
    {
        error = "Mod descriptors require exactly id, version, scope, protocol, and order "
                "(optional: name, author, description, website, api, depends, conflicts)";
        return false;
    }
    mod.metadata.clear();
    for (const auto &[key, value] : values)
        if (optional(key))
            mod.metadata.emplace_back(key, value);

    mod.descriptor.id = values["id"];
    mod.descriptor.version = values["version"];
    const std::string scope = lowercase(values["scope"]);
    if (scope == "client")
        mod.descriptor.scope = ModScope::client;
    else if (scope == "server")
        mod.descriptor.scope = ModScope::server;
    else if (scope == "core")
        mod.descriptor.scope = ModScope::core;
    else
    {
        error = "Mod scope must be client, server, or core";
        return false;
    }
    const std::string protocol = lowercase(values["protocol"]);
    mod.classic = protocol == "classic" || protocol == "both";
    mod.modern = protocol == "modern" || protocol == "both";
    if (!mod.classic && !mod.modern)
    {
        error = "Mod protocol must be classic, modern, or both";
        return false;
    }
    const std::string &order = values["order"];
    const auto parsed = std::from_chars(order.data(), order.data() + order.size(), mod.load_order);
    if (parsed.ec != std::errc{} || parsed.ptr != order.data() + order.size() ||
        mod.load_order > 1'000'000U)
    {
        error = "Mod order must be an integer from 0 through 1000000";
        return false;
    }
    return true;
}

} // namespace

bool load_mod_catalog(const std::filesystem::path &root, ModCatalog &catalog, std::string &error)
{
    ModCatalog loaded;
    std::error_code scan_error;
    if (!std::filesystem::exists(root, scan_error))
    {
        if (scan_error)
        {
            error = "Cannot inspect the add-on directory: " + scan_error.message();
            return false;
        }
        catalog = {};
        error.clear();
        return true;
    }
    if (!std::filesystem::is_directory(root, scan_error) || scan_error)
    {
        error = "The add-on root is not a readable directory";
        return false;
    }
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator{root, scan_error})
    {
        if (scan_error)
        {
            error = "Cannot enumerate the add-on root: " + scan_error.message();
            return false;
        }
        // (Polish) A .git of a mods repository, desktop.ini, ...
        if (is_ignored_mod_entry(entry.path().filename()))
            continue;
        const std::filesystem::file_status status = entry.symlink_status(scan_error);
        if (scan_error || std::filesystem::is_symlink(status))
        {
            error = "The add-on root may not contain symbolic links";
            return false;
        }
        if (!std::filesystem::is_directory(status))
            continue;
        const std::filesystem::path descriptor = entry.path() / mod_descriptor_filename;
        if (!std::filesystem::is_regular_file(descriptor, scan_error))
        {
            if (scan_error)
            {
                error = "Cannot inspect an add-on descriptor: " + scan_error.message();
                return false;
            }
            continue;
        }
        LoadedMod mod;
        if (!read_mod_folder(entry.path(), mod, error))
        {
            error = entry.path().filename().string() + ": " + error;
            return false;
        }
        loaded.mods.push_back(std::move(mod));
        if (loaded.mods.size() > maximum_catalog_mods)
        {
            error = "The add-on catalog exceeds 256 manifest-backed mods";
            return false;
        }
    }
    std::sort(loaded.mods.begin(), loaded.mods.end(),
              [](const LoadedMod &left, const LoadedMod &right) {
                  if (left.load_order != right.load_order)
                      return left.load_order < right.load_order;
                  return lowercase(left.descriptor.id) < lowercase(right.descriptor.id);
              });
    std::set<std::string> ids;
    for (const LoadedMod &mod : loaded.mods)
        if (!ids.insert(lowercase(mod.descriptor.id)).second)
        {
            error = "The add-on catalog contains duplicate case-insensitive mod IDs";
            return false;
        }
    catalog = std::move(loaded);
    error.clear();
    return true;
}

bool read_mod_folder(const std::filesystem::path &folder, LoadedMod &mod, std::string &error)
{
    LoadedMod loaded;
    loaded.root = folder;
    if (!parse_descriptor(folder / mod_descriptor_filename, loaded, error) ||
        !hash_tree(folder, loaded.descriptor.content_digest, error))
        return false;
    mod = std::move(loaded);
    error.clear();
    return true;
}

bool make_mod_manifest(const ModCatalog &catalog, ProtocolVersion protocol, ModManifest &manifest,
                       std::string &error)
{
    ModManifest selected;
    selected.protocol = protocol;
    // (Authority) The host's "OpenMinerZ players only" is not the catalog's to
    // change: it survives a rescan and a protocol switch.
    selected.openminerz_only = manifest.openminerz_only;
    for (const LoadedMod &mod : catalog.mods)
    {
        const bool compatible = protocol == ProtocolVersion::classic_v3 ? mod.classic : mod.modern;
        if (compatible)
            selected.mods.push_back(mod.descriptor);
    }
    if (!validate_mod_manifest(selected, error))
        return false;
    manifest = std::move(selected);
    error.clear();
    return true;
}

const char *mod_scope_label(ModScope scope) noexcept
{
    switch (scope)
    {
    case ModScope::client:
        return "CLIENT";
    case ModScope::server:
        return "SERVER";
    case ModScope::core:
        return "CORE";
    }
    return "INVALID";
}

bool client_visible_digest(const std::filesystem::path &root, ModDigest &digest,
                           std::uint64_t &bytes, std::string &error)
{
    std::vector<ModContentFile> files;
    if (!list_mod_content(root, files, error))
        return false;
    files = client_visible_files(std::move(files));
    bytes = 0U;
    for (const ModContentFile &file : files)
        bytes += file.path.size() + 9U + file.size;
    return digest_mod_content(files, digest, error);
}

std::string format_mod_download_record(const ModDescriptor &descriptor,
                                       const ModDigest &client_digest)
{
    return "id=" + descriptor.id + "\nversion=" + descriptor.version +
           "\nscope=" + lowercase(mod_scope_label(descriptor.scope)) +
           "\ncontent=" + digest_hex(descriptor.content_digest) +
           "\nclient=" + digest_hex(client_digest) + "\n";
}

bool load_cached_mod(const std::filesystem::path &folder, const ModDigest &content, LoadedMod &mod,
                     std::string &error)
{
    std::error_code file_error;
    const std::filesystem::path record_path = folder / mod_download_record_filename;
    const std::uint64_t record_size = std::filesystem::file_size(record_path, file_error);
    if (file_error || record_size == 0U || record_size > 4096U)
    {
        error = "missing or oversized download record";
        return false;
    }
    std::ifstream input{record_path, std::ios::binary};
    std::string text(static_cast<std::size_t>(record_size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())))
    {
        error = "unreadable download record";
        return false;
    }
    std::map<std::string, std::string> record;
    std::size_t offset{};
    while (offset < text.size())
    {
        const std::size_t end = std::min(text.find('\n', offset), text.size());
        const std::string line = trim(std::string_view{text}.substr(offset, end - offset));
        offset = end + 1U;
        if (line.empty())
            continue;
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos ||
            !record.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second)
        {
            error = "malformed download record";
            return false;
        }
    }
    ModDigest recorded{};
    ModDigest client{};
    if (record.size() != 5U || !parse_digest_hex(record["content"], recorded) ||
        recorded != content || !parse_digest_hex(record["client"], client))
    {
        error = "download record does not match its folder";
        return false;
    }
    if (!parse_descriptor(folder / mod_descriptor_filename, mod, error))
        return false;
    if (mod.descriptor.id != record["id"] || mod.descriptor.version != record["version"] ||
        lowercase(mod_scope_label(mod.descriptor.scope)) != record["scope"] ||
        mod.descriptor.scope == ModScope::client)
    {
        error = "descriptor does not match the download record";
        return false;
    }
    std::vector<ModContentFile> files;
    if (!list_mod_content(folder, files, error))
        return false;
    // Only what a download can contain: the client-visible files and the record.
    for (const ModContentFile &file : files)
        if (!is_client_visible_mod_path(file.path) && file.path != mod_download_record_filename)
        {
            error = "unexpected file " + file.path;
            return false;
        }
    ModDigest actual{};
    if (!digest_mod_content(client_visible_files(std::move(files)), actual, error))
        return false;
    if (actual != client)
    {
        error = "files do not match the downloaded digest";
        return false;
    }
    mod.descriptor.content_digest = content;
    mod.root = folder;
    return true;
}

ModCacheLoad load_mod_cache(const std::filesystem::path &root)
{
    ModCacheLoad result;
    std::error_code scan_error;
    if (root.empty() || !std::filesystem::is_directory(root, scan_error))
        return result;
    for (std::filesystem::directory_iterator iterator{root, scan_error}, end;
         !scan_error && iterator != end; iterator.increment(scan_error))
    {
        const std::filesystem::file_status status = iterator->symlink_status(scan_error);
        if (scan_error || !std::filesystem::is_directory(status))
            continue;
        const std::string name = iterator->path().filename().string();
        ModDigest content{};
        // Staging folders and anything else that is not a digest are ignored.
        if (!parse_digest_hex(name, content))
            continue;
        LoadedMod mod;
        std::string error;
        if (!load_cached_mod(iterator->path(), content, mod, error))
        {
            result.skipped.push_back(name + ": " + error);
            continue;
        }
        result.catalog.mods.push_back(std::move(mod));
        if (result.catalog.mods.size() >= maximum_catalog_mods)
            break;
    }
    std::sort(result.catalog.mods.begin(), result.catalog.mods.end(),
              [](const LoadedMod &left, const LoadedMod &right) {
                  return std::tie(left.descriptor.id, left.root) <
                         std::tie(right.descriptor.id, right.root);
              });
    return result;
}

SessionModSelection select_session_mods(const ModCatalog &local, const ModCatalog &cache,
                                        const ModManifest *required, ProtocolVersion protocol)
{
    SessionModSelection selection;
    selection.manifest.protocol = protocol;
    const auto supports = [protocol](const LoadedMod &mod) {
        return protocol == ProtocolVersion::classic_v3 ? mod.classic : mod.modern;
    };
    for (const LoadedMod &mod : local.mods)
        if (mod.descriptor.scope == ModScope::client && supports(mod))
        {
            selection.manifest.mods.push_back(mod.descriptor);
            selection.mods.push_back(mod);
        }
    if (required == nullptr)
        return selection;
    // (Authority) The session's rule, not a mod: the hello and this client's
    // mod channel follow the host's "OpenMinerZ players only".
    selection.manifest.openminerz_only = required->openminerz_only;
    for (const ModDescriptor &wanted : required->mods)
    {
        if (wanted.scope == ModScope::client)
            continue;
        const auto matches = [&](const LoadedMod &mod) {
            return mod.descriptor == wanted && supports(mod);
        };
        const LoadedMod *found = nullptr;
        if (const auto local_mod = std::find_if(local.mods.begin(), local.mods.end(), matches);
            local_mod != local.mods.end())
            found = &*local_mod;
        else if (const auto cached = std::find_if(cache.mods.begin(), cache.mods.end(), matches);
                 cached != cache.mods.end())
            found = &*cached;
        if (found == nullptr)
        {
            selection.missing.push_back(wanted);
            continue;
        }
        selection.manifest.mods.push_back(found->descriptor);
        selection.mods.push_back(*found);
    }
    return selection;
}

} // namespace openminerz::net
