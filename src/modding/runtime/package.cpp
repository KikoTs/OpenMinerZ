// Mod packages on disk (openminerz/modding/package.hpp): descriptor metadata,
// the file allowlist, mod roots and the dependency-aware load order.

#include "openminerz/modding/package.hpp"

#include "runtime_impl.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace openminerz::modding
{
namespace
{

constexpr std::size_t maximum_metadata_bytes = 512U;
constexpr std::size_t maximum_description_bytes = 2048U;
constexpr std::size_t maximum_dependencies = 32U;
constexpr std::size_t maximum_script_bytes = 4U * 1024U * 1024U;

std::string trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1U);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1U);
    return std::string{text};
}

std::filesystem::path local_app_data()
{
#ifdef _WIN32
    char *value{};
    std::size_t length{};
    if (_dupenv_s(&value, &length, "LOCALAPPDATA") == 0 && value != nullptr)
    {
        std::filesystem::path path{std::u8string{reinterpret_cast<const char8_t *>(value)}};
        std::free(value);
        return path;
    }
    return {};
#else
    const char *value = std::getenv("XDG_DATA_HOME");
    if (value != nullptr && *value != '\0')
        return value;
    const char *home = std::getenv("HOME");
    return home != nullptr ? std::filesystem::path{home} / ".local" / "share"
                           : std::filesystem::path{};
#endif
}

} // namespace

bool is_allowed_package_file(std::string_view path) noexcept
{
    if (path == net::mod_descriptor_filename)
        return true;
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) ||
        dot + 1U >= path.size())
        return false;
    const std::string_view extension = path.substr(dot + 1U);
    return std::any_of(std::begin(package_file_extensions), std::end(package_file_extensions),
                       [&](std::string_view allowed) { return extension == allowed; });
}

bool version_at_least(std::string_view version, std::string_view minimum)
{
    if (minimum.empty())
        return true;
    const auto split = [](std::string_view text) {
        std::vector<std::string_view> parts;
        std::size_t start{};
        while (true)
        {
            const std::size_t dot = text.find('.', start);
            parts.push_back(text.substr(start, dot == std::string_view::npos ? dot : dot - start));
            if (dot == std::string_view::npos)
                break;
            start = dot + 1U;
        }
        return parts;
    };
    const std::vector<std::string_view> have = split(version);
    const std::vector<std::string_view> need = split(minimum);
    for (std::size_t index{}; index < std::max(have.size(), need.size()); ++index)
    {
        const std::string_view left = index < have.size() ? have[index] : std::string_view{"0"};
        const std::string_view right = index < need.size() ? need[index] : std::string_view{"0"};
        std::uint64_t left_number{};
        std::uint64_t right_number{};
        const auto left_parsed = std::from_chars(left.data(), left.data() + left.size(), left_number);
        const auto right_parsed =
            std::from_chars(right.data(), right.data() + right.size(), right_number);
        const bool numeric = left_parsed.ec == std::errc{} &&
                             left_parsed.ptr == left.data() + left.size() &&
                             right_parsed.ec == std::errc{} &&
                             right_parsed.ptr == right.data() + right.size();
        if (numeric ? left_number != right_number : left != right)
            return numeric ? left_number > right_number : left > right;
    }
    return true;
}

bool parse_package_info(const net::LoadedMod &mod, PackageInfo &info, std::string &error)
{
    PackageInfo parsed;
    parsed.name = mod.descriptor.id;
    for (const auto &[key, value] : mod.metadata)
    {
        const std::size_t limit =
            key == "description" ? maximum_description_bytes : maximum_metadata_bytes;
        if (value.size() > limit)
        {
            error = std::format("descriptor field '{}' is longer than {} bytes", key, limit);
            return false;
        }
        if (key == "name")
            parsed.name = value;
        else if (key == "author")
            parsed.author = value;
        else if (key == "description")
            parsed.description = value;
        else if (key == "website")
            parsed.website = value;
        else if (key == "api")
        {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed.api);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                parsed.api == 0U)
            {
                error = "api must be a positive whole number";
                return false;
            }
        }
        else if (key == "depends")
        {
            std::size_t start{};
            while (start <= value.size())
            {
                const std::size_t comma = value.find(',', start);
                const std::string entry = trim(std::string_view{value}.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!entry.empty())
                {
                    const std::size_t at = entry.find('@');
                    PackageDependency dependency{trim(entry.substr(0U, at)),
                                                 at == std::string::npos ? std::string{}
                                                                         : trim(entry.substr(at + 1U))};
                    if (dependency.id.empty() || dependency.id.size() > net::maximum_mod_id_bytes ||
                        (at != std::string::npos && dependency.minimum_version.empty()))
                    {
                        error = std::format("malformed dependency '{}' (use id or id@version)", entry);
                        return false;
                    }
                    if (lowercase_ascii(dependency.id) == lowercase_ascii(mod.descriptor.id))
                    {
                        error = "a mod cannot depend on itself";
                        return false;
                    }
                    parsed.depends.push_back(std::move(dependency));
                }
                if (comma == std::string::npos)
                    break;
                start = comma + 1U;
            }
            if (parsed.depends.size() > maximum_dependencies)
            {
                error = "a mod may name at most 32 dependencies";
                return false;
            }
        }
    }
    info = std::move(parsed);
    error.clear();
    return true;
}

std::filesystem::path user_mod_root()
{
    const std::filesystem::path base = local_app_data();
    return base.empty() ? base : base / "OpenMinerZ" / "Mods";
}

std::filesystem::path mod_data_root()
{
    const std::filesystem::path base = local_app_data();
    return base.empty() ? base : base / "OpenMinerZ" / "ModData";
}

std::vector<std::filesystem::path> default_mod_roots()
{
    std::vector<std::filesystem::path> roots{std::filesystem::current_path() / "addons"};
    if (std::filesystem::path user = user_mod_root(); !user.empty())
        roots.push_back(std::move(user));
    return roots;
}

bool load_mod_catalogs(std::span<const std::filesystem::path> roots, net::ModCatalog &catalog,
                       std::string &error)
{
    net::ModCatalog merged;
    std::set<std::string> ids;
    for (const std::filesystem::path &root : roots)
    {
        net::ModCatalog loaded;
        if (!net::load_mod_catalog(root, loaded, error))
            return false;
        for (net::LoadedMod &mod : loaded.mods)
        {
            if (!ids.insert(lowercase_ascii(mod.descriptor.id)).second)
            {
                error = std::format("mod '{}' is installed twice ({})", mod.descriptor.id,
                                    mod.root.generic_string());
                return false;
            }
            merged.mods.push_back(std::move(mod));
        }
    }
    if (merged.mods.size() > net::maximum_catalog_mods)
    {
        error = "The add-on catalog exceeds 256 manifest-backed mods";
        return false;
    }
    std::stable_sort(merged.mods.begin(), merged.mods.end(),
                     [](const net::LoadedMod &left, const net::LoadedMod &right) {
                         if (left.load_order != right.load_order)
                             return left.load_order < right.load_order;
                         return lowercase_ascii(left.descriptor.id) <
                                lowercase_ascii(right.descriptor.id);
                     });
    catalog = std::move(merged);
    error.clear();
    return true;
}

bool read_package(const net::LoadedMod &mod, ModSource &source, std::string &error)
{
    PackageInfo info;
    if (!parse_package_info(mod, info, error))
        return false;
    if (info.api > api_version)
    {
        error = std::format("written for modding API {}, this game has API {}", info.api,
                            api_version);
        return false;
    }
    ModSource loaded;
    loaded.id = mod.descriptor.id;
    loaded.version = mod.descriptor.version;
    loaded.scope = mod.descriptor.scope;
    loaded.root = mod.root;
    for (const PackageDependency &dependency : info.depends)
        loaded.dependencies.push_back(dependency.id);

    std::error_code scan_error;
    std::size_t files{};
    for (std::filesystem::recursive_directory_iterator iterator{mod.root, scan_error}, end;
         iterator != end; iterator.increment(scan_error))
    {
        if (scan_error)
        {
            error = "cannot read the mod folder: " + scan_error.message();
            return false;
        }
        // (Polish) .git, .vscode, .luaurc, Thumbs.db ...: not part of the mod.
        if (net::is_ignored_mod_entry(iterator->path().filename()))
        {
            iterator.disable_recursion_pending();
            continue;
        }
        const std::filesystem::file_status status = iterator->symlink_status(scan_error);
        if (scan_error || std::filesystem::is_symlink(status))
        {
            error = "mods may not contain links";
            return false;
        }
        if (!std::filesystem::is_regular_file(status))
            continue;
        if (++files > net::maximum_mod_files)
        {
            error = "a mod may contain at most 4096 files";
            return false;
        }
        std::string normalized;
        const std::filesystem::path relative =
            std::filesystem::relative(iterator->path(), mod.root, scan_error);
        if (scan_error || !normalize_mod_path(relative.generic_string(), normalized))
        {
            error = "a file path in the mod cannot be read";
            return false;
        }
        if (!is_allowed_package_file(normalized))
        {
            error = std::format("{} is not a file type mods may contain", normalized);
            return false;
        }
        const bool script = normalized.ends_with(".luau") || normalized.ends_with(".lua");
        const bool code_folder = normalized.starts_with("shared/") ||
                                 normalized.starts_with("client/") ||
                                 normalized.starts_with("server/");
        if (!script || !code_folder)
            continue;
        const std::uint64_t size = iterator->file_size(scan_error);
        if (scan_error || size > maximum_script_bytes)
        {
            error = std::format("{} is larger than 4 MiB", normalized);
            return false;
        }
        std::ifstream input{iterator->path(), std::ios::binary};
        std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if (!input.good() && !input.eof())
        {
            error = std::format("{} cannot be read", normalized);
            return false;
        }
        loaded.files.insert_or_assign(std::move(normalized), std::move(text));
    }
    source = std::move(loaded);
    error.clear();
    return true;
}

PackagePlan plan_packages(std::span<const net::LoadedMod> mods)
{
    PackagePlan plan;
    struct Candidate final
    {
        const net::LoadedMod *mod{};
        PackageInfo info;
        ModSource source;
        bool ready{};
        bool placed{};
        std::string refusal;
    };
    std::vector<Candidate> candidates(mods.size());
    std::map<std::string, std::size_t> by_id;
    for (std::size_t index{}; index < mods.size(); ++index)
    {
        Candidate &candidate = candidates[index];
        candidate.mod = &mods[index];
        by_id.emplace(lowercase_ascii(mods[index].descriptor.id), index);
        std::string error;
        if (!parse_package_info(mods[index], candidate.info, error) ||
            !read_package(mods[index], candidate.source, error))
            candidate.refusal = error;
        else
            candidate.ready = true;
    }
    // Dependencies must exist, be new enough and not be client mods a
    // synchronized mod would rely on (a client mod may be absent on the host).
    for (Candidate &candidate : candidates)
        for (const PackageDependency &dependency : candidate.info.depends)
        {
            if (!candidate.ready)
                break;
            const auto found = by_id.find(lowercase_ascii(dependency.id));
            if (found == by_id.end())
                candidate.refusal = std::format("needs mod '{}', which is not installed", dependency.id);
            else if (!version_at_least(mods[found->second].descriptor.version,
                                       dependency.minimum_version))
                candidate.refusal = std::format("needs {} {} or newer (installed: {})",
                                                dependency.id, dependency.minimum_version,
                                                mods[found->second].descriptor.version);
            else if (candidate.mod->descriptor.scope != net::ModScope::client &&
                     mods[found->second].descriptor.scope == net::ModScope::client)
                candidate.refusal = std::format("a {} mod cannot depend on the client mod '{}'",
                                                net::mod_scope_label(candidate.mod->descriptor.scope),
                                                dependency.id);
            if (!candidate.refusal.empty())
                candidate.ready = false;
        }
    // Kahn's algorithm, always taking the earliest ready mod in catalog order:
    // with no dependencies the order is exactly the catalog's.
    bool progress = true;
    while (progress)
    {
        progress = false;
        for (Candidate &candidate : candidates)
        {
            if (candidate.placed || !candidate.ready)
                continue;
            bool waiting{};
            for (const PackageDependency &dependency : candidate.info.depends)
            {
                const Candidate &needed = candidates[by_id.at(lowercase_ascii(dependency.id))];
                if (!needed.ready)
                {
                    candidate.ready = false;
                    candidate.refusal = std::format("needs mod '{}', which cannot load: {}",
                                                    dependency.id, needed.refusal);
                    progress = true;
                }
                if (!needed.placed)
                    waiting = true;
            }
            if (!candidate.ready || waiting)
                continue;
            candidate.placed = true;
            plan.mods.push_back(std::move(candidate.source));
            progress = true;
            break;
        }
    }
    for (Candidate &candidate : candidates)
    {
        if (candidate.placed)
            continue;
        if (candidate.refusal.empty())
            candidate.refusal = "its dependencies form a cycle";
        plan.refused.emplace_back(candidate.mod->descriptor.id, candidate.refusal);
    }
    return plan;
}

} // namespace openminerz::modding
