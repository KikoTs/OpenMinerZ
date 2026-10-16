// Themes: colours, fonts, nine-slice images, cursor, sounds and the game's own
// words, per interface style, layered by load order (openminerz/modding/ui.hpp).

#include "openminerz/modding/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace openminerz::modding::ui
{
namespace
{

constexpr std::array<std::string_view, color_key_count> color_names{
    "menu.row",   "menu.focus",  "menu.heading",  "menu.text",     "menu.quiet",
    "menu.disabled", "menu.accent", "menu.warning", "hud.plate",    "hud.panel",
    "hud.well",   "hud.slot",    "hud.slot_full", "hud.band",      "hud.line",
    "hud.health", "hud.stamina", "hud.oxygen",    "hud.good",      "hud.caution"};
constexpr std::array<std::string_view, image_key_count> image_names{"panel", "button", "button_focus",
                                                                     "dialog", "slot"};
constexpr std::array<std::string_view, sound_key_count> sound_names{"click", "popup", "error"};
constexpr std::array<std::string_view, style_count> style_names{"classic", "modern", "remastered"};

[[nodiscard]] int hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

[[nodiscard]] std::optional<std::uint8_t> channel(const Value &value) noexcept
{
    if (!value.is_number())
        return std::nullopt;
    const double number = value.type() == ValueType::integer
                              ? static_cast<double>(std::get<std::int64_t>(value.storage()))
                              : std::get<double>(value.storage());
    if (!std::isfinite(number) || number < 0.0 || number > 255.0)
        return std::nullopt;
    return static_cast<std::uint8_t>(std::lround(number));
}

[[nodiscard]] std::string lower(std::string_view text)
{
    std::string result{text};
    for (char &c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

[[nodiscard]] const Map &table(const Value &value, std::string_view what)
{
    if (value.type() != ValueType::map)
        throw ScriptError{std::format("{} must be a table", what)};
    return value.as_map();
}

[[nodiscard]] std::string string_field(const Value &value, std::string_view what)
{
    if (value.type() != ValueType::string)
        throw ScriptError{std::format("{} must be a string", what)};
    return value.as_string();
}

[[nodiscard]] AssetRef asset(std::string_view mod, const Value &value, std::string_view what,
                             std::span<const std::string_view> extensions,
                             const ThemeStack::Exists &exists)
{
    const std::string raw = string_field(value, what);
    const std::optional<std::string> path = normalize_asset_path(raw);
    if (!path)
        throw ScriptError{std::format("{}: '{}' is not a path inside the mod", what, raw)};
    const std::string lowered = lower(*path);
    if (!std::ranges::any_of(extensions, [&](std::string_view extension) {
            return lowered.ends_with(extension);
        }))
        throw ScriptError{std::format("{}: '{}' is not a supported file type", what, raw)};
    AssetRef result{std::string{mod}, *path};
    if (exists && !exists(result))
        throw ScriptError{std::format("{}: '{}' was not found in the mod", what, raw)};
    return result;
}

constexpr std::array<std::string_view, 2> font_types{".ttf", ".otf"};
constexpr std::array<std::string_view, 2> image_types{".png", ".bmp"};
constexpr std::array<std::string_view, 1> sound_types{".wav"};

[[nodiscard]] float slice_length(const Value &value)
{
    if (!value.is_number())
        throw ScriptError{"theme image slice must be numbers"};
    const double number = value.as_number();
    if (!std::isfinite(number) || number < 0.0 || number > 512.0)
        throw ScriptError{"theme image slice must be 0..512 pixels"};
    return static_cast<float>(number);
}

void read_entries(std::string_view mod, const Map &spec, ThemeEntries &entries, bool top,
                  const ThemeStack::Exists &exists)
{
    for (const auto &[key, value] : spec)
    {
        if (key == "colors" || key == "colours")
        {
            for (const auto &[name, color] : table(value, "theme colors"))
            {
                const std::optional<ColorKey> slot = parse_color_key(name);
                if (!slot)
                    throw ScriptError{std::format("theme colors: unknown colour '{}'", name)};
                const std::optional<Color> parsed = parse_color(color);
                if (!parsed)
                    throw ScriptError{std::format("theme colors: '{}' is not a colour", name)};
                entries.colors[static_cast<std::size_t>(*slot)] = parsed;
            }
        }
        else if (key == "fonts")
        {
            for (const auto &[name, path] : table(value, "theme fonts"))
            {
                const std::size_t face = name == "display" ? 0U : name == "text" ? 1U : 2U;
                if (face > 1U)
                    throw ScriptError{std::format("theme fonts: unknown face '{}' (display, text)", name)};
                entries.fonts[face] = asset(mod, path, "theme font", font_types, exists);
            }
        }
        else if (key == "images")
        {
            for (const auto &[name, image] : table(value, "theme images"))
            {
                const auto found = std::ranges::find(image_names, name);
                if (found == image_names.end())
                    throw ScriptError{std::format("theme images: unknown image '{}'", name)};
                NineSlice nine;
                if (image.type() == ValueType::string)
                    nine.image = asset(mod, image, "theme image", image_types, exists);
                else
                {
                    const Value *path = image.field("path");
                    if (path == nullptr)
                        throw ScriptError{"theme image needs a path"};
                    nine.image = asset(mod, *path, "theme image", image_types, exists);
                    if (const Value *slice = image.field("slice"); slice != nullptr && !slice->is_nil())
                    {
                        if (slice->is_number())
                        {
                            const float edge = slice_length(*slice);
                            nine.slice = {edge, edge, edge, edge};
                        }
                        else if (slice->type() == ValueType::array && slice->as_array().size() == 4U)
                        {
                            const Array &edges = slice->as_array();
                            nine.slice = {slice_length(edges[0]), slice_length(edges[1]),
                                          slice_length(edges[2]), slice_length(edges[3])};
                        }
                        else
                            throw ScriptError{"theme image slice is a number or {left, top, right, bottom}"};
                    }
                }
                entries.images[static_cast<std::size_t>(found - image_names.begin())] = std::move(nine);
            }
        }
        else if (key == "cursor")
        {
            CursorSpec cursor;
            if (value.type() == ValueType::string)
                cursor.image = asset(mod, value, "theme cursor", image_types, exists);
            else
            {
                const Value *path = value.field("path");
                if (path == nullptr)
                    throw ScriptError{"theme cursor needs a path"};
                cursor.image = asset(mod, *path, "theme cursor", image_types, exists);
                if (const Value *hotspot = value.field("hotspot"); hotspot != nullptr && !hotspot->is_nil())
                {
                    if (hotspot->type() != ValueType::array || hotspot->as_array().size() != 2U)
                        throw ScriptError{"theme cursor hotspot is {x, y}"};
                    cursor.hot_x = static_cast<int>(std::clamp<std::int64_t>(hotspot->as_array()[0].as_integer(), 0, 255));
                    cursor.hot_y = static_cast<int>(std::clamp<std::int64_t>(hotspot->as_array()[1].as_integer(), 0, 255));
                }
            }
            entries.cursor = std::move(cursor);
        }
        else if (key == "sounds")
        {
            for (const auto &[name, sound] : table(value, "theme sounds"))
            {
                const auto found = std::ranges::find(sound_names, name);
                if (found == sound_names.end())
                    throw ScriptError{std::format("theme sounds: unknown sound '{}' (click, popup, error)", name)};
                SoundSpec chosen;
                const std::string text = string_field(sound, "theme sound");
                if (text.starts_with("cue:"))
                {
                    chosen.cue = text.substr(4U);
                    if (chosen.cue.empty() || chosen.cue.size() > 64U ||
                        !std::ranges::all_of(chosen.cue, [](char c) {
                            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
                        }))
                        throw ScriptError{std::format("theme sounds: '{}' is not a cue name", text)};
                }
                else
                    chosen.file = asset(mod, sound, "theme sound", sound_types, exists);
                entries.sounds[static_cast<std::size_t>(found - sound_names.begin())] = std::move(chosen);
            }
        }
        else if (key == "strings")
        {
            for (const auto &[english, replacement] : table(value, "theme strings"))
            {
                std::string text = string_field(replacement, "theme string");
                if (english.empty() || english.size() > ThemeStack::max_string_bytes ||
                    text.size() > ThemeStack::max_string_bytes)
                    throw ScriptError{"theme strings are 1..1024 bytes"};
                entries.strings.insert_or_assign(english, std::move(text));
            }
        }
        else if (top && parse_style(key).has_value())
        {
            // Handled by the caller.
        }
        else
            throw ScriptError{std::format("ui.theme: unknown key '{}'", key)};
    }
}

} // namespace

std::string_view style_name(Style style) noexcept
{
    return style_names[static_cast<std::size_t>(style)];
}

std::optional<Style> parse_style(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < style_names.size(); ++index)
        if (style_names[index] == name)
            return static_cast<Style>(index);
    return std::nullopt;
}

std::optional<Color> parse_color(const Value &value) noexcept
{
    if (value.type() == ValueType::string)
    {
        const std::string &text = value.as_string();
        if (text.empty() || text[0] != '#')
            return std::nullopt;
        std::array<int, 8> digits{};
        const std::size_t count = text.size() - 1U;
        if (count != 3U && count != 6U && count != 8U)
            return std::nullopt;
        for (std::size_t index = 0U; index < count; ++index)
        {
            digits[index] = hex_digit(text[index + 1U]);
            if (digits[index] < 0)
                return std::nullopt;
        }
        if (count == 3U)
            return Color{static_cast<std::uint8_t>(digits[0] * 17), static_cast<std::uint8_t>(digits[1] * 17),
                         static_cast<std::uint8_t>(digits[2] * 17), 255U};
        const auto pair = [&](std::size_t at) { return static_cast<std::uint8_t>(digits[at] * 16 + digits[at + 1U]); };
        return Color{pair(0U), pair(2U), pair(4U), count == 8U ? pair(6U) : std::uint8_t{255U}};
    }
    if (value.type() == ValueType::array)
    {
        const Array &parts = value.as_array();
        if (parts.size() != 3U && parts.size() != 4U)
            return std::nullopt;
        std::array<std::uint8_t, 4> channels{0U, 0U, 0U, 255U};
        for (std::size_t index = 0U; index < parts.size(); ++index)
        {
            const std::optional<std::uint8_t> part = channel(parts[index]);
            if (!part)
                return std::nullopt;
            channels[index] = *part;
        }
        return Color{channels[0], channels[1], channels[2], channels[3]};
    }
    if (value.type() == ValueType::map)
    {
        std::array<std::uint8_t, 4> channels{0U, 0U, 0U, 255U};
        std::size_t seen = 0U;
        for (const auto &[key, part] : value.as_map())
        {
            const std::size_t index = key == "r" ? 0U : key == "g" ? 1U : key == "b" ? 2U : key == "a" ? 3U : 4U;
            const std::optional<std::uint8_t> parsed = channel(part);
            if (index > 3U || !parsed)
                return std::nullopt;
            channels[index] = *parsed;
            seen += index < 3U ? 1U : 0U;
        }
        if (seen != 3U)
            return std::nullopt;
        return Color{channels[0], channels[1], channels[2], channels[3]};
    }
    return std::nullopt;
}

Color scale_alpha(Color color, float opacity) noexcept
{
    const float clamped = std::isfinite(opacity) ? std::clamp(opacity, 0.0F, 1.0F) : 0.0F;
    color.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(color.a) * clamped));
    return color;
}

Rect Rect::intersect(const Rect &other) const noexcept
{
    const float left = std::max(x, other.x);
    const float top = std::max(y, other.y);
    const float right_edge = std::min(right(), other.right());
    const float bottom_edge = std::min(bottom(), other.bottom());
    return {left, top, std::max(0.0F, right_edge - left), std::max(0.0F, bottom_edge - top)};
}

std::optional<std::string> normalize_asset_path(std::string_view path)
{
    if (path.empty() || path.size() > 240U)
        return std::nullopt;
    std::string result;
    result.reserve(path.size());
    for (const char c : path)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20U || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return std::nullopt;
        result.push_back(c == '\\' ? '/' : c);
    }
    if (result.front() == '/')
        return std::nullopt;
    std::size_t start = 0U;
    while (start <= result.size())
    {
        const std::size_t end = std::min(result.find('/', start), result.size());
        const std::string_view segment{result.data() + start, end - start};
        if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' ||
            segment.back() == ' ')
            return std::nullopt;
        start = end + 1U;
    }
    return result;
}

std::string_view color_key_name(ColorKey key) noexcept
{
    return key < ColorKey::count ? color_names[static_cast<std::size_t>(key)] : std::string_view{};
}

std::optional<ColorKey> parse_color_key(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < color_names.size(); ++index)
        if (color_names[index] == name)
            return static_cast<ColorKey>(index);
    return std::nullopt;
}

std::string_view image_key_name(ImageKey key) noexcept
{
    return key < ImageKey::count ? image_names[static_cast<std::size_t>(key)] : std::string_view{};
}

std::string_view sound_key_name(SoundKey key) noexcept
{
    return key < SoundKey::count ? sound_names[static_cast<std::size_t>(key)] : std::string_view{};
}

bool ThemeEntries::empty() const noexcept
{
    return std::ranges::none_of(colors, [](const auto &color) { return color.has_value(); }) &&
           std::ranges::none_of(fonts, [](const auto &font) { return font.has_value(); }) &&
           std::ranges::none_of(images, [](const auto &image) { return image.has_value(); }) &&
           !cursor.has_value() &&
           std::ranges::none_of(sounds, [](const auto &sound) { return sound.has_value(); }) &&
           strings.empty();
}

void ThemeStack::apply(std::string_view mod, std::uint32_t order, const Value &spec, const Exists &exists)
{
    const Map &top = table(spec, "ui.theme's argument");
    auto found = std::ranges::find(layers_, mod, &Layer::mod);
    // Work on a copy: a bad key anywhere leaves the theme as it was.
    Layer layer = found != layers_.end() ? *found : Layer{std::string{mod}, order, {}, {}};
    layer.order = order;
    read_entries(mod, top, layer.any, true, exists);
    for (const auto &[key, value] : top)
        if (const std::optional<Style> style = parse_style(key))
            read_entries(mod, table(value, std::format("theme {}", key)),
                         layer.styles[static_cast<std::size_t>(*style)], false, exists);
    std::size_t strings = layer.any.strings.size();
    for (const ThemeEntries &entries : layer.styles)
        strings += entries.strings.size();
    std::size_t others = 0U;
    for (const Layer &other : layers_)
        if (other.mod != mod)
        {
            others += other.any.strings.size();
            for (const ThemeEntries &entries : other.styles)
                others += entries.strings.size();
        }
    if (strings + others > max_strings)
        throw ScriptError{std::format("ui.theme: more than {} replaced strings", max_strings)};
    if (found != layers_.end())
        *found = std::move(layer);
    else
        layers_.push_back(std::move(layer));
    std::ranges::stable_sort(layers_, [](const Layer &left, const Layer &right) {
        return left.order != right.order ? left.order < right.order : left.mod < right.mod;
    });
    ++revision_;
    recount();
}

void ThemeStack::clear(std::string_view mod)
{
    const auto removed = std::erase_if(layers_, [&](const Layer &layer) { return layer.mod == mod; });
    if (removed != 0U)
    {
        ++revision_;
        recount();
    }
}

void ThemeStack::clear_all() noexcept
{
    if (layers_.empty())
        return;
    layers_.clear();
    ++revision_;
    string_count_ = 0U;
}

void ThemeStack::recount() noexcept
{
    string_count_ = 0U;
    for (const Layer &layer : layers_)
    {
        string_count_ += layer.any.strings.size();
        for (const ThemeEntries &entries : layer.styles)
            string_count_ += entries.strings.size();
    }
}

template <typename Pick>
auto ThemeStack::find(Style style, Pick pick) const noexcept -> decltype(pick(ThemeEntries{}))
{
    for (auto layer = layers_.rbegin(); layer != layers_.rend(); ++layer)
    {
        if (auto found = pick(layer->styles[static_cast<std::size_t>(style)]))
            return found;
        if (auto found = pick(layer->any))
            return found;
    }
    return {};
}

std::optional<Color> ThemeStack::color(ColorKey key, Style style) const noexcept
{
    if (key >= ColorKey::count)
        return std::nullopt;
    return find(style, [key](const ThemeEntries &entries) { return entries.colors[static_cast<std::size_t>(key)]; });
}

const AssetRef *ThemeStack::font(Face face, Style style) const noexcept
{
    return find(style, [face](const ThemeEntries &entries) -> const AssetRef * {
        const auto &font = entries.fonts[static_cast<std::size_t>(face)];
        return font ? &*font : nullptr;
    });
}

const NineSlice *ThemeStack::image(ImageKey key, Style style) const noexcept
{
    if (key >= ImageKey::count)
        return nullptr;
    return find(style, [key](const ThemeEntries &entries) -> const NineSlice * {
        const auto &image = entries.images[static_cast<std::size_t>(key)];
        return image ? &*image : nullptr;
    });
}

const CursorSpec *ThemeStack::cursor(Style style) const noexcept
{
    return find(style, [](const ThemeEntries &entries) -> const CursorSpec * {
        return entries.cursor ? &*entries.cursor : nullptr;
    });
}

const SoundSpec *ThemeStack::sound(SoundKey key, Style style) const noexcept
{
    if (key >= SoundKey::count)
        return nullptr;
    return find(style, [key](const ThemeEntries &entries) -> const SoundSpec * {
        const auto &sound = entries.sounds[static_cast<std::size_t>(key)];
        return sound ? &*sound : nullptr;
    });
}

const std::string *ThemeStack::text(std::string_view english, Style style) const noexcept
{
    if (string_count_ == 0U)
        return nullptr;
    return find(style, [english](const ThemeEntries &entries) -> const std::string * {
        const auto found = entries.strings.find(english);
        return found != entries.strings.end() ? &found->second : nullptr;
    });
}

Palette ThemeStack::resolve(Palette base, Style style) const noexcept
{
    if (layers_.empty())
        return base;
    for (std::size_t index = 0U; index < color_key_count; ++index)
        if (const std::optional<Color> color = this->color(static_cast<ColorKey>(index), style))
            base[index] = *color;
    return base;
}

} // namespace openminerz::modding::ui
