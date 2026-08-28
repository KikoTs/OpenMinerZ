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

    Sha256 hash;
    std::uint64_t total_bytes{};
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    for (const auto &[relative, path] : files)
    {
        const std::uint64_t file_size = std::filesystem::file_size(path, scan_error);
        if (scan_error || file_size > maximum_mod_file_bytes ||
            total_bytes > maximum_mod_content_bytes - file_size)
        {
            error = "A mod exceeds its bounded file or aggregate content budget";
            return false;
        }
        total_bytes += file_size;
        hash.update({reinterpret_cast<const std::uint8_t *>(relative.data()), relative.size()});
        const std::array<std::uint8_t, 1U> separator{};
        hash.update(separator);
        hash_u64(hash, file_size);
        std::ifstream input{path, std::ios::binary};
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
        if (!input.eof() || bytes_read != file_size)
        {
            error = "A mod content file changed or could not be read completely";
            return false;
        }
    }
    digest = hash.finish();
    return true;
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
    std::size_t offset{};
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
    if (values.size() != expected.size() ||
        std::any_of(values.begin(), values.end(),
                    [&](const auto &entry) { return !expected.contains(entry.first); }))
    {
        error = "Mod descriptors require exactly id, version, scope, protocol, and order";
        return false;
    }

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
        mod.root = entry.path();
        if (!parse_descriptor(descriptor, mod, error) ||
            !hash_tree(entry.path(), mod.descriptor.content_digest, error))
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

bool make_mod_manifest(const ModCatalog &catalog, ProtocolVersion protocol, ModManifest &manifest,
                       std::string &error)
{
    ModManifest selected;
    selected.protocol = protocol;
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

} // namespace openminerz::net
