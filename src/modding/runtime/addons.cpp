// The Add-Ons screen's model (openminerz/modding/addons.hpp): installed mods
// in every root, the player's on/off choices, conflicts, dependencies and the
// catalog of the mods that are on.

#include "openminerz/modding/addons.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

namespace openminerz::modding
{
namespace
{

constexpr std::uint64_t maximum_choices_bytes = 64U * 1024U;
constexpr std::size_t maximum_choices = 1024U;

std::string lowercase(std::string_view text)
{
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

std::string trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1U);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1U);
    return std::string{text};
}

bool identifier_character(char character) noexcept
{
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

// At `position`, an opening long bracket ("[[", "[==[") of which `level` is
// the number of '='. Comments and strings use them alike.
bool long_bracket(std::string_view text, std::size_t position, std::size_t &level) noexcept
{
    if (position >= text.size() || text[position] != '[')
        return false;
    std::size_t index = position + 1U;
    while (index < text.size() && text[index] == '=')
        ++index;
    if (index >= text.size() || text[index] != '[')
        return false;
    level = index - position - 1U;
    return true;
}

// Just past the closing bracket of the level `level` that starts at or after
// `position` (the end of the text when there is none).
std::size_t past_long_bracket(std::string_view text, std::size_t position, std::size_t level) noexcept
{
    const std::string closing = "]" + std::string(level, '=') + "]";
    const std::size_t found = text.find(closing, position);
    return found == std::string_view::npos ? text.size() : found + closing.size();
}

// The script with its comments removed and every string reduced to a space,
// so that only code is searched.
std::string code_only(std::string_view text)
{
    std::string code;
    code.reserve(text.size());
    std::size_t index{};
    while (index < text.size())
    {
        const char character = text[index];
        std::size_t level{};
        if (character == '-' && index + 1U < text.size() && text[index + 1U] == '-')
        {
            if (long_bracket(text, index + 2U, level))
                index = past_long_bracket(text, index + 4U + level, level);
            else
                while (index < text.size() && text[index] != '\n')
                    ++index;
            code.push_back(' ');
            continue;
        }
        if (character == '"' || character == '\'' || character == '`')
        {
            ++index;
            while (index < text.size() && text[index] != character && text[index] != '\n')
                index += text[index] == '\\' ? 2U : 1U;
            ++index;
            code.push_back(' ');
            continue;
        }
        if (long_bracket(text, index, level))
        {
            index = past_long_bracket(text, index + 2U + level, level);
            code.push_back(' ');
            continue;
        }
        code.push_back(character);
        ++index;
    }
    return code;
}

bool calls_terrain_define(std::string_view code)
{
    constexpr std::string_view library{"terrain"};
    for (std::size_t at = code.find(library); at != std::string_view::npos;
         at = code.find(library, at + 1U))
    {
        if (at > 0U && (identifier_character(code[at - 1U]) || code[at - 1U] == '.' || code[at - 1U] == ':'))
            continue;
        std::size_t index = at + library.size();
        const auto skip_space = [&] {
            while (index < code.size() && std::isspace(static_cast<unsigned char>(code[index])) != 0)
                ++index;
        };
        skip_space();
        if (index >= code.size() || code[index] != '.')
            continue;
        ++index;
        skip_space();
        constexpr std::string_view function{"define"};
        if (code.substr(index, function.size()) != function)
            continue;
        index += function.size();
        if (index < code.size() && identifier_character(code[index]))
            continue; // terrain.define_something
        skip_space();
        if (index < code.size() && code[index] == '=' && (index + 1U >= code.size() || code[index + 1U] != '='))
            continue; // an assignment, not a call
        return true;
    }
    return false;
}

std::vector<std::string> split_ids(std::string_view value, std::string_view own_id, std::string &error)
{
    std::vector<std::string> ids;
    std::size_t start{};
    while (start <= value.size())
    {
        const std::size_t comma = value.find(',', start);
        const std::string id = lowercase(
            trim(value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start)));
        if (id.size() > net::maximum_mod_id_bytes)
        {
            error = std::format("conflicts names a mod id longer than {} bytes", net::maximum_mod_id_bytes);
            return {};
        }
        if (!id.empty() && id != lowercase(own_id) && std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1U;
    }
    return ids;
}

// Fills in what a readable mod says about itself beyond its descriptor.
void inspect_package(InstalledAddon &addon)
{
    std::string error;
    if (!parse_package_info(addon.mod, addon.info, error))
    {
        addon.problem = error;
        return;
    }
    for (const auto &[key, value] : addon.mod.metadata)
        if (key == "conflicts")
        {
            addon.conflicts = split_ids(value, addon.id(), error);
            if (!error.empty())
            {
                addon.problem = error;
                return;
            }
        }
    ModSource source;
    if (!read_package(addon.mod, source, error))
    {
        addon.problem = error;
        return;
    }
    addon.defines_world = addon.mod.descriptor.scope != net::ModScope::client && scripts_define_world(source);
}

bool load_order_before(const InstalledAddon &left, const InstalledAddon &right)
{
    if (left.mod.load_order != right.mod.load_order)
        return left.mod.load_order < right.mod.load_order;
    return lowercase(left.id()) < lowercase(right.id());
}

// Indices of the scan's mods in load order.
std::vector<std::size_t> load_order(const AddonScan &scan)
{
    std::vector<std::size_t> order(scan.addons.size());
    for (std::size_t index{}; index < order.size(); ++index)
        order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return load_order_before(scan.addons[left], scan.addons[right]);
    });
    return order;
}

std::size_t index_of(const AddonScan &scan, std::string_view id) noexcept
{
    const std::string key = lowercase(id);
    for (std::size_t index{}; index < scan.addons.size(); ++index)
        if (lowercase(scan.addons[index].id()) == key)
            return index;
    return scan.addons.size();
}

bool wanted_by(const InstalledAddon &addon, const AddonChoices &choices)
{
    const auto choice = choices.find(lowercase(addon.id()));
    return choice != choices.end() ? choice->second : addon_on_by_default(addon.origin);
}

bool depends_on(const InstalledAddon &addon, std::string_view id)
{
    const std::string key = lowercase(id);
    return std::any_of(addon.info.depends.begin(), addon.info.depends.end(),
                       [&](const PackageDependency &dependency) { return lowercase(dependency.id) == key; });
}

net::ModCatalog catalog_of(const AddonScan &scan, std::span<const AddonStatus> status, bool off_synchronized)
{
    net::ModCatalog catalog;
    for (const std::size_t index : load_order(scan))
    {
        const InstalledAddon &addon = scan.addons[index];
        if (!addon.usable() || index >= status.size())
            continue;
        if (status[index].on || (off_synchronized && addon.mod.descriptor.scope != net::ModScope::client))
            catalog.mods.push_back(addon.mod);
    }
    return catalog;
}

} // namespace

std::string_view addon_origin_label(AddonOrigin origin) noexcept
{
    switch (origin)
    {
    case AddonOrigin::game:
        return "Game folder";
    case AddonOrigin::user:
        return "Mods folder";
    case AddonOrigin::bundled:
        return "Bundled with the game";
    }
    return {};
}

std::filesystem::path bundled_mod_root(const std::filesystem::path &application_directory)
{
    return application_directory.empty() ? std::filesystem::path{}
                                         : application_directory / "examples" / "mods";
}

std::vector<AddonRoot> addon_roots(const std::filesystem::path &bundled)
{
    std::vector<AddonRoot> roots;
    const std::vector<std::filesystem::path> installed = default_mod_roots();
    for (std::size_t index{}; index < installed.size(); ++index)
        roots.push_back({installed[index], index == 0U ? AddonOrigin::game : AddonOrigin::user});
    if (!bundled.empty())
        roots.push_back({bundled, AddonOrigin::bundled});
    return roots;
}

bool scripts_define_world(const ModSource &source)
{
    for (const auto &[path, text] : source.files)
    {
        const std::string normalized = lowercase(path);
        if (!normalized.starts_with("shared/") && !normalized.starts_with("server/"))
            continue;
        if (!normalized.ends_with(".luau") && !normalized.ends_with(".lua"))
            continue;
        if (text.find("terrain") != std::string::npos && calls_terrain_define(code_only(text)))
            return true;
    }
    return false;
}

AddonScan scan_addons(std::span<const AddonRoot> roots)
{
    AddonScan scan;
    struct Found final
    {
        InstalledAddon addon;
        bool read{}; // its descriptor was read: its id is the mod's own
    };
    std::vector<Found> found;
    for (const AddonRoot &root : roots)
    {
        std::error_code error;
        if (root.path.empty() || !std::filesystem::exists(root.path, error))
        {
            if (error)
                scan.errors.push_back(std::format("{}: {}", root.path.string(), error.message()));
            continue;
        }
        if (!std::filesystem::is_directory(root.path, error))
        {
            scan.errors.push_back(std::format("{}: not a folder", root.path.string()));
            continue;
        }
        std::vector<std::filesystem::path> folders;
        for (std::filesystem::directory_iterator iterator{root.path, error}, end; !error && iterator != end;
             iterator.increment(error))
            if (!net::is_ignored_mod_entry(iterator->path().filename()))
                folders.push_back(iterator->path());
        if (error)
            scan.errors.push_back(std::format("{}: {}", root.path.string(), error.message()));
        std::sort(folders.begin(), folders.end());
        for (const std::filesystem::path &folder : folders)
        {
            Found entry;
            InstalledAddon &addon = entry.addon;
            addon.origin = root.origin;
            addon.mod.root = folder;
            addon.mod.descriptor.id = folder.filename().string();
            addon.mod.load_order = std::numeric_limits<std::uint32_t>::max();
            std::error_code status_error;
            const std::filesystem::file_status status = std::filesystem::symlink_status(folder, status_error);
            if (status_error)
                addon.problem = "the folder cannot be read: " + status_error.message();
            else if (std::filesystem::is_symlink(status))
                addon.problem = "mod folders may not be links";
            else if (!std::filesystem::is_directory(status) ||
                     !std::filesystem::is_regular_file(folder / net::mod_descriptor_filename, status_error))
                continue; // not a mod (a junction, a loose file, a folder without a descriptor)
            else
            {
                std::string load_error;
                net::LoadedMod loaded;
                if (!net::read_mod_folder(folder, loaded, load_error))
                    addon.problem = load_error;
                else
                {
                    addon.mod = std::move(loaded);
                    entry.read = true;
                    inspect_package(addon);
                }
            }
            if (addon.info.name.empty())
                addon.info.name = addon.id();
            found.push_back(std::move(entry));
        }
    }
    // An id the game or user root has: a bundled copy is replaced by it; a
    // second installed copy is a problem of its own.
    std::map<std::string, std::string> owners;
    for (Found &entry : found)
    {
        if (!entry.read)
        {
            scan.addons.push_back(std::move(entry.addon));
            continue;
        }
        const std::string key = lowercase(entry.addon.id());
        const auto owner = owners.find(key);
        if (owner == owners.end())
        {
            owners.emplace(key, entry.addon.mod.root.parent_path().string());
            scan.addons.push_back(std::move(entry.addon));
            continue;
        }
        if (entry.addon.origin == AddonOrigin::bundled)
        {
            scan.hidden.push_back(entry.addon.id());
            continue;
        }
        entry.addon.problem = std::format("installed twice: also in {}", owner->second);
        scan.addons.push_back(std::move(entry.addon));
    }
    std::stable_sort(scan.addons.begin(), scan.addons.end(), [](const InstalledAddon &left, const InstalledAddon &right) {
        const std::string left_name = lowercase(left.info.name);
        const std::string right_name = lowercase(right.info.name);
        return left_name != right_name ? left_name < right_name : lowercase(left.id()) < lowercase(right.id());
    });
    return scan;
}

AddonClash addon_clash(const InstalledAddon &first, const InstalledAddon &second) noexcept
{
    if (&first == &second || !first.usable() || !second.usable())
        return AddonClash::none;
    // A Classic-only and a Modern-only mod never run in the same world.
    if (!((first.mod.classic && second.mod.classic) || (first.mod.modern && second.mod.modern)))
        return AddonClash::none;
    if (first.defines_world && second.defines_world)
        return AddonClash::world;
    const std::string first_id = lowercase(first.id());
    const std::string second_id = lowercase(second.id());
    if (std::find(first.conflicts.begin(), first.conflicts.end(), second_id) != first.conflicts.end() ||
        std::find(second.conflicts.begin(), second.conflicts.end(), first_id) != second.conflicts.end())
        return AddonClash::declared;
    return AddonClash::none;
}

AddonChoices read_addon_choices(const std::filesystem::path &file)
{
    AddonChoices choices;
    std::error_code error;
    const std::uint64_t size = std::filesystem::file_size(file, error);
    if (error || size > maximum_choices_bytes)
        return choices;
    std::ifstream input{file, std::ios::binary};
    std::string line;
    while (std::getline(input, line) && choices.size() < maximum_choices)
    {
        const std::string text = trim(line);
        if (text.empty() || text.front() == '#')
            continue;
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = lowercase(trim(std::string_view{text}.substr(0U, equals)));
        const std::string id = lowercase(trim(std::string_view{text}.substr(equals + 1U)));
        if (id.empty() || id.size() > net::maximum_mod_id_bytes || (key != "on" && key != "off"))
            continue;
        choices.insert_or_assign(id, key == "on");
    }
    return choices;
}

bool write_addon_choices(const std::filesystem::path &file, const AddonChoices &choices, std::string &error)
{
    std::error_code file_error;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), file_error);
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        output << "# OpenMinerZ add-ons: which installed add-ons are on, written by the Add-Ons\n"
                  "# screen. A mod with no line here is on, unless it is a bundled example.\n";
        for (const auto &[id, on] : choices)
            output << (on ? "on=" : "off=") << id << '\n';
        output.flush();
        if (!output)
        {
            error = "the add-on choices could not be written";
            return false;
        }
    }
    std::filesystem::rename(temporary, file, file_error);
    if (file_error)
    {
        error = "the add-on choices could not be saved: " + file_error.message();
        std::filesystem::remove(temporary, file_error);
        return false;
    }
    error.clear();
    return true;
}

std::vector<AddonStatus> resolve_addons(const AddonScan &scan, const AddonChoices &choices)
{
    std::vector<AddonStatus> status(scan.addons.size());
    std::vector<std::size_t> chosen;
    std::vector<std::size_t> defaulted;
    for (const std::size_t index : load_order(scan))
    {
        const InstalledAddon &addon = scan.addons[index];
        status[index].wanted = wanted_by(addon, choices);
        if (!addon.usable())
        {
            status[index].reason = addon.problem;
            continue;
        }
        if (status[index].wanted)
            (choices.contains(lowercase(addon.id())) ? chosen : defaulted).push_back(index);
    }
    // The player's own choices first, then the defaults: a newly installed
    // mod never pushes out one the player turned on.
    std::vector<std::size_t> on;
    for (const std::vector<std::size_t> *group : {&chosen, &defaulted})
        for (const std::size_t index : *group)
        {
            const auto blocker = std::find_if(on.begin(), on.end(), [&](std::size_t other) {
                return addon_clash(scan.addons[index], scan.addons[other]) != AddonClash::none;
            });
            if (blocker != on.end())
            {
                const InstalledAddon &other = scan.addons[*blocker];
                status[index].reason =
                    addon_clash(scan.addons[index], other) == AddonClash::world
                        ? std::format("{} is on, and only one add-on can replace the world generator.",
                                      other.info.name)
                        : std::format("{} is on, and the two cannot run together.", other.info.name);
                continue;
            }
            on.push_back(index);
            status[index].on = true;
        }
    // A mod whose dependency is installed but off (or cannot load) stays off.
    // A dependency that is not installed at all is the runtime's to report.
    for (bool changed = true; changed;)
    {
        changed = false;
        for (const std::size_t index : on)
        {
            if (!status[index].on)
                continue;
            for (const PackageDependency &dependency : scan.addons[index].info.depends)
            {
                const std::size_t needed = index_of(scan, dependency.id);
                if (needed >= scan.addons.size() || status[needed].on)
                    continue;
                status[index].on = false;
                status[index].reason =
                    std::format("It needs {}, which {}.", scan.addons[needed].info.name,
                                scan.addons[needed].usable() ? "is off" : "cannot load");
                changed = true;
                break;
            }
        }
    }
    return status;
}

net::ModCatalog active_catalog(const AddonScan &scan, std::span<const AddonStatus> status)
{
    return catalog_of(scan, status, false);
}

net::ModCatalog join_catalog(const AddonScan &scan, std::span<const AddonStatus> status)
{
    return catalog_of(scan, status, true);
}

const InstalledAddon *find_addon(const AddonScan &scan, std::string_view id) noexcept
{
    const std::size_t index = index_of(scan, id);
    return index < scan.addons.size() ? &scan.addons[index] : nullptr;
}

AddonChange set_addon(const AddonScan &scan, const AddonChoices &choices, std::string_view id, bool on)
{
    AddonChange change{choices, {}};
    const std::size_t target = index_of(scan, id);
    if (target >= scan.addons.size())
        return change;
    const std::vector<AddonStatus> before = resolve_addons(scan, choices);
    const auto choose = [&](std::size_t index, bool value) {
        change.choices.insert_or_assign(lowercase(scan.addons[index].id()), value);
    };
    // Turning a mod off takes the mods that need it with it, transitively.
    const auto turn_off_dependents = [&](std::size_t start) {
        std::vector<std::size_t> pending{start};
        std::set<std::size_t> done{start};
        while (!pending.empty())
        {
            const std::size_t needed = pending.back();
            pending.pop_back();
            for (std::size_t index{}; index < scan.addons.size(); ++index)
            {
                if (done.contains(index) || !wanted_by(scan.addons[index], change.choices) ||
                    !depends_on(scan.addons[index], scan.addons[needed].id()))
                    continue;
                done.insert(index);
                choose(index, false);
                if (before[index].on)
                    change.notices.push_back(std::format("{} turned off: it needs {}.",
                                                         scan.addons[index].info.name,
                                                         scan.addons[needed].info.name));
                pending.push_back(index);
            }
        }
    };
    choose(target, on);
    if (!on)
    {
        turn_off_dependents(target);
        return change;
    }
    // On: what it depends on comes on with it, transitively...
    std::vector<std::size_t> coming{target};
    for (std::size_t next{}; next < coming.size(); ++next)
        for (const PackageDependency &dependency : scan.addons[coming[next]].info.depends)
        {
            const std::size_t needed = index_of(scan, dependency.id);
            if (needed >= scan.addons.size() || !scan.addons[needed].usable() ||
                std::find(coming.begin(), coming.end(), needed) != coming.end())
                continue;
            coming.push_back(needed);
            if (!before[needed].on)
                change.notices.push_back(std::format("{} turned on: {} needs it.", scan.addons[needed].info.name,
                                                     scan.addons[coming[next]].info.name));
            choose(needed, true);
        }
    // ...and whatever cannot run beside them goes off, with what needs it.
    for (std::size_t index{}; index < scan.addons.size(); ++index)
    {
        if (std::find(coming.begin(), coming.end(), index) != coming.end() ||
            !wanted_by(scan.addons[index], change.choices))
            continue;
        const auto clash = std::find_if(coming.begin(), coming.end(), [&](std::size_t other) {
            return addon_clash(scan.addons[index], scan.addons[other]) != AddonClash::none;
        });
        if (clash == coming.end())
            continue;
        choose(index, false);
        if (before[index].on)
            change.notices.push_back(
                addon_clash(scan.addons[index], scan.addons[*clash]) == AddonClash::world
                    ? std::format("{} turned off: only one add-on can replace the world generator.",
                                  scan.addons[index].info.name)
                    : std::format("{} turned off: it cannot run with {}.", scan.addons[index].info.name,
                                  scan.addons[*clash].info.name));
        turn_off_dependents(index);
    }
    return change;
}

} // namespace openminerz::modding
