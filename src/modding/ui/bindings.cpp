// Mod key bindings and the names of the built-in HUD's elements
// (openminerz/modding/ui.hpp).

#include "openminerz/modding/ui.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace openminerz::modding::ui
{
namespace
{

constexpr std::array<std::string_view, hud_element_count> hud_names{
    "crosshair", "vitals", "hotbar",   "item_name",   "block_name", "compass",
    "clock",     "distance", "oxygen", "chat",        "player_list", "notifications",
    "damage",    "scope",  "death",    "server",      "coordinates"};

// Every key process_world_key and the held-movement poll read in the world
// (src/app.cpp), as SDL_GetKeyName spells them.
constexpr std::array<std::string_view, 34> reserved_keys{
    "W", "A",  "S",  "D",  "Space",  "Left Shift", "Left Ctrl", "E",     "Q",    "G",   "Return", "Keypad Enter",
    "Left", "Right", "Up", "Down", "T", "Y",       "Tab",       "R",     "F",    "F1",  "F2",     "F3",
    "Escape", "V", "1",  "2",  "3",      "4",          "5",         "6",     "7",    "8"};

[[nodiscard]] bool same_key(std::string_view left, std::string_view right) noexcept
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

[[nodiscard]] bool valid_action_name(std::string_view name) noexcept
{
    return !name.empty() && name.size() <= 48U && std::ranges::all_of(name, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
    });
}

} // namespace

bool KeyBindings::game_key(std::string_view key) noexcept
{
    return std::ranges::any_of(reserved_keys, [&](std::string_view reserved) { return same_key(reserved, key); });
}

std::string KeyBindings::canonical(std::string_view key) const
{
    if (canonical_)
        return canonical_(key);
    std::string result{key};
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    while (!result.empty() && result.front() == ' ')
        result.erase(result.begin());
    if (result.size() == 1U)
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result.size() <= 32U ? result : std::string{};
}

Callback KeyBindings::bind(std::string_view mod, std::uint32_t order, std::string_view name,
                           std::string_view default_key, std::string_view label, Callback callback)
{
    if (!valid_action_name(name))
        throw ScriptError{"input.bind: an action name is 1..48 letters, digits, '_', '-' or '.'"};
    const std::string action = std::format("{}.{}", mod, name);
    std::string key;
    if (!default_key.empty())
    {
        key = canonical(default_key);
        if (key.empty())
            throw ScriptError{std::format("input.bind: '{}' is not a key", default_key)};
    }
    if (label.size() > 96U)
        throw ScriptError{"input.bind: the label is longer than 96 bytes"};
    Callback replaced;
    auto found = std::ranges::find(bindings_, action, &KeyBinding::action);
    if (found == bindings_.end())
    {
        if (bindings_.size() >= max_bindings)
            throw ScriptError{std::format("input.bind: at most {} bindings", max_bindings)};
        KeyBinding binding;
        binding.action = action;
        binding.mod = std::string{mod};
        binding.order = order;
        bindings_.push_back(std::move(binding));
        found = bindings_.end() - 1;
    }
    else
        replaced = found->callback;
    found->label = label.empty() ? std::string{name} : std::string{label};
    found->default_key = std::move(key);
    found->callback = callback;
    found->order = order;
    std::ranges::stable_sort(bindings_, {}, &KeyBinding::order);
    resolve();
    return replaced == callback ? Callback{} : replaced;
}

Callback KeyBindings::unbind(std::string_view action)
{
    const auto found = std::ranges::find(bindings_, action, &KeyBinding::action);
    if (found == bindings_.end())
        return {};
    const Callback callback = found->callback;
    bindings_.erase(found);
    resolve();
    return callback;
}

void KeyBindings::forget_mod(std::string_view mod, std::vector<Callback> &released)
{
    for (const KeyBinding &binding : bindings_)
        if (binding.mod == mod && binding.callback.valid())
            released.push_back(binding.callback);
    std::erase_if(bindings_, [&](const KeyBinding &binding) { return binding.mod == mod; });
    resolve();
}

void KeyBindings::clear(std::vector<Callback> &released)
{
    for (const KeyBinding &binding : bindings_)
        if (binding.callback.valid())
            released.push_back(binding.callback);
    bindings_.clear();
}

void KeyBindings::resolve()
{
    const auto used_by = [&](std::string_view key) -> const KeyBinding * {
        for (const KeyBinding &binding : bindings_)
            if (!binding.key.empty() && same_key(binding.key, key))
                return &binding;
        return nullptr;
    };
    for (KeyBinding &binding : bindings_)
    {
        binding.key.clear();
        binding.conflict.clear();
        binding.user_set = false;
    }
    // The player's own choices first: they were checked when they were made,
    // but a game key never becomes a mod's even from an old settings file.
    for (KeyBinding &binding : bindings_)
        if (const auto found = user_.find(binding.action); found != user_.end())
        {
            binding.user_set = true;
            if (found->second.empty() || game_key(found->second) || used_by(found->second) != nullptr)
                continue;
            binding.key = found->second;
        }
    for (KeyBinding &binding : bindings_)
    {
        if (binding.user_set || binding.default_key.empty())
            continue;
        if (game_key(binding.default_key))
        {
            binding.conflict = std::format("The game uses {}", binding.default_key);
            continue;
        }
        if (const KeyBinding *other = used_by(binding.default_key))
        {
            binding.conflict = std::format("{} is used by {}", binding.default_key, other->label);
            continue;
        }
        binding.key = binding.default_key;
    }
}

bool KeyBindings::assign(std::string_view action, std::string_view key, std::string &error)
{
    const auto found = std::ranges::find(bindings_, action, &KeyBinding::action);
    if (found == bindings_.end())
    {
        error = "no such action";
        return false;
    }
    const std::string wanted = key.empty() ? std::string{} : canonical(key);
    if (!key.empty() && wanted.empty())
    {
        error = std::format("'{}' is not a key", key);
        return false;
    }
    if (!wanted.empty())
    {
        if (game_key(wanted))
        {
            error = std::format("The game uses {}", wanted);
            return false;
        }
        for (const KeyBinding &other : bindings_)
            if (other.action != action && !other.key.empty() && same_key(other.key, wanted))
            {
                error = std::format("{} is used by {}", wanted, other.label);
                return false;
            }
    }
    user_.insert_or_assign(std::string{action}, wanted);
    resolve();
    return true;
}

void KeyBindings::reset(std::string_view action)
{
    if (const auto found = user_.find(action); found != user_.end())
    {
        user_.erase(found);
        resolve();
    }
}

const KeyBinding *KeyBindings::find(std::string_view action) const noexcept
{
    const auto found = std::ranges::find(bindings_, action, &KeyBinding::action);
    return found == bindings_.end() ? nullptr : &*found;
}

const KeyBinding *KeyBindings::for_key(std::string_view key) const noexcept
{
    if (key.empty())
        return nullptr;
    for (const KeyBinding &binding : bindings_)
        if (!binding.key.empty() && same_key(binding.key, key))
            return &binding;
    return nullptr;
}

std::string KeyBindings::save() const
{
    std::string text = "# OpenMinerZ mod key bindings: action=key (empty: no key)\n";
    for (const auto &[action, key] : user_)
        text += std::format("{}={}\n", action, key);
    return text;
}

void KeyBindings::load(std::string_view text)
{
    user_.clear();
    std::size_t at = 0U;
    while (at < text.size() && user_.size() < 4096U)
    {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        std::string_view line = text.substr(at, end - at);
        at = end + 1U;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1U);
        if (line.empty() || line.front() == '#')
            continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view action = line.substr(0U, equals);
        const std::string_view key = line.substr(equals + 1U);
        if (action.empty() || action.size() > 160U || key.size() > 32U ||
            !std::ranges::all_of(action, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
            }))
            continue;
        user_.insert_or_assign(std::string{action}, key.empty() ? std::string{} : canonical(key));
    }
    resolve();
}

std::string_view hud_element_name(HudElement element) noexcept
{
    return element < HudElement::count ? hud_names[static_cast<std::size_t>(element)] : std::string_view{};
}

std::optional<HudElement> parse_hud_element(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < hud_names.size(); ++index)
        if (hud_names[index] == name)
            return static_cast<HudElement>(index);
    return std::nullopt;
}

bool hud_element_movable(HudElement element) noexcept
{
    switch (element)
    {
    case HudElement::crosshair:
    case HudElement::damage:
    case HudElement::scope:
    case HudElement::death:
    case HudElement::count:
        return false;
    default:
        return true;
    }
}

std::optional<MenuSlot> parse_menu_slot(std::string_view name) noexcept
{
    if (name == "main")
        return MenuSlot::main;
    if (name == "pause")
        return MenuSlot::pause;
    if (name == "addons")
        return MenuSlot::addons;
    return std::nullopt;
}

} // namespace openminerz::modding::ui
