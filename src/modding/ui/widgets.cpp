// Retained widgets built from Luau tables: building, layout, drawing and input
// routing (openminerz/modding/ui.hpp).
//
// Layout is in canvas units (the Remastered HUD's 1280x720 reference), drawing
// in window pixels. The look follows the Remastered screens (app_remastered.inc:
// rm_draw_button, rm_draw_slot): a black plate for a button, the white focus
// band, a thin frame, the menus' inks. Every colour comes from the frame's
// palette, which is the game's own inks with the theme applied.

#include "openminerz/modding/ui.hpp"
#include "openminerz/remastered_layout.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace openminerz::modding::ui
{
namespace
{

constexpr std::array<std::string_view, static_cast<std::size_t>(WidgetKind::count)> kind_names{
    "panel", "label", "image", "button", "toggle", "slider", "text_input",
    "list",  "progress", "item_slot", "stack", "grid", "spacer"};

constexpr std::array<std::string_view, 9> anchor_names{"top_left",    "top",    "top_right",
                                                       "left",        "center", "right",
                                                       "bottom_left", "bottom", "bottom_right"};

// Default capital heights, in units (the Remastered HUD's: 11 for labels,
// 13 for names, 18 for titles).
constexpr float label_size = 11.0F;
constexpr float button_size = 11.0F;
constexpr float slot_units = 56.0F;

[[nodiscard]] bool container(WidgetKind kind) noexcept
{
    return kind == WidgetKind::panel || kind == WidgetKind::stack || kind == WidgetKind::grid ||
           kind == WidgetKind::list;
}

[[nodiscard]] float finite(const Value &value, std::string_view what, float low, float high)
{
    if (!value.is_number())
        throw ScriptError{std::format("{} must be a number", what)};
    const double number = value.as_number();
    if (!std::isfinite(number) || number < low || number > high)
        throw ScriptError{std::format("{} must be {}..{}", what, low, high)};
    return static_cast<float>(number);
}

[[nodiscard]] Length length(const Value &value, std::string_view what)
{
    if (value.is_nil())
        return {};
    if (value.is_number())
        return {Length::Mode::units, finite(value, what, 0.0F, 20000.0F)};
    if (value.type() != ValueType::string)
        throw ScriptError{std::format("{} is a number, \"50%\", \"fill\" or \"auto\"", what)};
    const std::string &text = value.as_string();
    if (text == "fill")
        return {Length::Mode::fill, 1.0F};
    if (text == "auto")
        return {};
    if (text.size() >= 2U && text.size() <= 6U && text.back() == '%')
    {
        char *end{};
        const std::string number = text.substr(0U, text.size() - 1U);
        const float share = std::strtof(number.c_str(), &end);
        if (end != nullptr && *end == '\0' && std::isfinite(share) && share >= 0.0F && share <= 100.0F)
            return {Length::Mode::fraction, share / 100.0F};
    }
    throw ScriptError{std::format("{}: '{}' is not a length", what, text)};
}

[[nodiscard]] Insets insets(const Value &value, std::string_view what)
{
    if (value.is_number())
    {
        const float edge = finite(value, what, 0.0F, 2000.0F);
        return {edge, edge, edge, edge};
    }
    if (value.type() == ValueType::array && value.as_array().size() == 2U)
    {
        const float horizontal = finite(value.as_array()[0], what, 0.0F, 2000.0F);
        const float vertical = finite(value.as_array()[1], what, 0.0F, 2000.0F);
        return {horizontal, vertical, horizontal, vertical};
    }
    if (value.type() == ValueType::array && value.as_array().size() == 4U)
    {
        const Array &edges = value.as_array();
        return {finite(edges[0], what, 0.0F, 2000.0F), finite(edges[1], what, 0.0F, 2000.0F),
                finite(edges[2], what, 0.0F, 2000.0F), finite(edges[3], what, 0.0F, 2000.0F)};
    }
    throw ScriptError{std::format("{} is a number, {{x, y}} or {{left, top, right, bottom}}", what)};
}

[[nodiscard]] Color color_of(const Value &value, std::string_view what)
{
    const std::optional<Color> color = parse_color(value);
    if (!color)
        throw ScriptError{std::format("{} is not a colour", what)};
    return *color;
}

// A colour, or the name of one of the theme's inks ("menu.accent").
void read_ink(const Value &value, std::string_view what, std::optional<Color> &color, std::optional<ColorKey> &key)
{
    color.reset();
    key.reset();
    if (value.is_nil())
        return;
    if (value.type() == ValueType::string)
        if (const std::optional<ColorKey> named = parse_color_key(value.as_string()))
        {
            key = named;
            return;
        }
    color = color_of(value, what);
}

[[nodiscard]] bool boolean(const Value &value, std::string_view what)
{
    if (value.type() != ValueType::boolean)
        throw ScriptError{std::format("{} must be true or false", what)};
    return value.as_bool();
}

[[nodiscard]] std::string bounded_string(const Value &value, std::string_view what, std::size_t limit)
{
    if (value.type() != ValueType::string)
        throw ScriptError{std::format("{} must be a string", what)};
    if (value.as_string().size() > limit)
        throw ScriptError{std::format("{} is longer than {} bytes", what, limit)};
    return value.as_string();
}

[[nodiscard]] Callback callback(const Value &value, std::string_view what)
{
    if (value.type() != ValueType::callback)
        throw ScriptError{std::format("{} must be a function", what)};
    return value.as_callback();
}

// Code points in a UTF-8 string.
[[nodiscard]] std::size_t code_points(std::string_view text) noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(
        text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

void erase_last_code_point(std::string &text) noexcept
{
    if (text.empty())
        return;
    std::size_t at = text.size() - 1U;
    while (at > 0U && (static_cast<unsigned char>(text[at]) & 0xC0U) == 0x80U)
        --at;
    text.resize(at);
}

[[nodiscard]] Rect to_window(const Rect &units, float scale) noexcept
{
    return {units.x * scale, units.y * scale, units.width * scale, units.height * scale};
}

[[nodiscard]] Color ink(const FrameContext &frame, ColorKey key) noexcept
{
    return frame.palette[static_cast<std::size_t>(key)];
}

void fill(DrawList &out, const Rect &rect, Color color)
{
    if (rect.empty() || color.a == 0U)
        return;
    DrawCmd draw;
    draw.kind = DrawCmd::Kind::fill;
    draw.rect = rect;
    draw.color = color;
    out.push_back(std::move(draw));
}

void frame(DrawList &out, const Rect &rect, Color color, float thickness)
{
    if (rect.empty() || color.a == 0U || thickness <= 0.0F)
        return;
    DrawCmd draw;
    draw.kind = DrawCmd::Kind::frame;
    draw.rect = rect;
    draw.color = color;
    draw.thickness = std::max(1.0F, std::round(thickness));
    out.push_back(std::move(draw));
}

void nine_slice(DrawList &out, const Rect &rect, const NineSlice &image, float scale, float opacity)
{
    DrawCmd draw;
    draw.kind = DrawCmd::Kind::image;
    draw.rect = rect;
    draw.image = image.image;
    draw.nine = true;
    draw.slice = image.slice;
    draw.slice_scale = scale;
    draw.color = scale_alpha(Color{}, opacity);
    out.push_back(std::move(draw));
}

void text_block(DrawList &out, const TextBlock &block, float left, float top, bool outline, float opacity)
{
    for (const PlacedText &piece : block.pieces)
    {
        if (piece.text.empty())
            continue;
        DrawCmd draw;
        draw.kind = DrawCmd::Kind::text;
        draw.rect = {left + piece.x, top + piece.top, 0.0F, piece.cap};
        draw.text = piece.text;
        draw.face = piece.face;
        draw.cap = piece.cap;
        draw.color = scale_alpha(piece.color, opacity);
        draw.outline = outline;
        out.push_back(std::move(draw));
    }
}

[[nodiscard]] float text_cap(const Widget &widget, const WidgetStore::LayoutContext &context) noexcept
{
    const float size = widget.text_size > 0.0F ? widget.text_size
                       : widget.kind == WidgetKind::button ? button_size
                                                           : label_size;
    return size * context.canvas->scale * context.frame->text_factor;
}

[[nodiscard]] bool interactive(const Widget &widget) noexcept
{
    switch (widget.kind)
    {
    case WidgetKind::button:
    case WidgetKind::toggle:
    case WidgetKind::slider:
    case WidgetKind::text_input:
        return true;
    default:
        return widget.on_click.valid() || widget.on_hover.valid() ||
               widget.action.kind != InternalAction::Kind::none;
    }
}

[[nodiscard]] bool takes_focus(const Widget &widget) noexcept
{
    return widget.focusable && widget.visible && widget.enabled;
}

[[nodiscard]] double snap(const Widget &widget, double value) noexcept
{
    double result = std::clamp(value, std::min(widget.minimum, widget.maximum),
                               std::max(widget.minimum, widget.maximum));
    if (widget.step > 0.0)
        result = widget.minimum + std::round((result - widget.minimum) / widget.step) * widget.step;
    return std::clamp(result, std::min(widget.minimum, widget.maximum),
                      std::max(widget.minimum, widget.maximum));
}

} // namespace

std::string_view widget_kind_name(WidgetKind kind) noexcept
{
    return kind < WidgetKind::count ? kind_names[static_cast<std::size_t>(kind)] : std::string_view{};
}

std::optional<WidgetKind> parse_widget_kind(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < kind_names.size(); ++index)
        if (kind_names[index] == name)
            return static_cast<WidgetKind>(index);
    if (name == "input")
        return WidgetKind::text_input;
    if (name == "item" || name == "slot")
        return WidgetKind::item_slot;
    if (name == "scroll")
        return WidgetKind::list;
    return std::nullopt;
}

std::optional<Anchor> parse_anchor(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < anchor_names.size(); ++index)
        if (anchor_names[index] == name)
            return static_cast<Anchor>(index);
    return std::nullopt;
}

float anchor_x(Anchor anchor) noexcept
{
    return static_cast<float>(static_cast<int>(anchor) % 3) * 0.5F;
}

float anchor_y(Anchor anchor) noexcept
{
    return static_cast<float>(static_cast<int>(anchor) / 3) * 0.5F;
}

Canvas make_canvas(const FrameContext &frame) noexcept
{
    const remastered::Canvas base = remastered::canvas(frame.width, frame.height, frame.ui_scale,
                                                       frame.safe_area);
    Canvas canvas;
    canvas.scale = base.scale;
    canvas.width = base.width;
    canvas.height = base.height;
    canvas.safe = {base.safe.x, base.safe.y, base.safe.width, base.safe.height};
    canvas.focus = canvas.safe;
    // On a window wider than 16:9 (21:9, 32:9) the middle 16:9 of it.
    const float widest = canvas.safe.height * 16.0F / 9.0F;
    if (canvas.safe.width > widest)
    {
        canvas.focus.x = canvas.safe.x + (canvas.safe.width - widest) * 0.5F;
        canvas.focus.width = widest;
    }
    return canvas;
}

// ---- Building ----------------------------------------------------------------

std::uint64_t WidgetStore::build(const Value &spec, const Owner &owner, std::vector<Callback> &adopted)
{
    std::vector<std::uint64_t> created;
    const std::size_t adopted_before = adopted.size();
    try
    {
        return create(spec, owner, 0U, 1U, adopted, created);
    }
    catch (...)
    {
        for (const std::uint64_t id : created)
            widgets_.erase(id);
        adopted.resize(adopted_before);
        throw;
    }
}

std::uint64_t WidgetStore::add_child(std::uint64_t parent, const Value &spec, std::vector<Callback> &adopted)
{
    Widget *holder = find(parent);
    if (holder == nullptr)
        throw ScriptError{"ui.add: the parent widget no longer exists"};
    if (!container(holder->kind))
        throw ScriptError{std::format("ui.add: a {} cannot hold widgets", widget_kind_name(holder->kind))};
    if (holder->children.size() >= max_children)
        throw ScriptError{std::format("ui.add: a widget holds at most {} children", max_children)};
    std::size_t depth = 1U;
    for (const Widget *up = holder; up != nullptr && up->parent != 0U; up = find(up->parent))
        ++depth;
    std::vector<std::uint64_t> created;
    const std::size_t adopted_before = adopted.size();
    try
    {
        const std::uint64_t id = create(spec, Owner{holder->mod, holder->side}, parent, depth + 1U, adopted, created);
        find(parent)->children.push_back(id);
        return id;
    }
    catch (...)
    {
        for (const std::uint64_t id : created)
            widgets_.erase(id);
        adopted.resize(adopted_before);
        throw;
    }
}

std::uint64_t WidgetStore::create(const Value &spec, const Owner &owner, std::uint64_t parent, std::size_t depth,
                                  std::vector<Callback> &adopted, std::vector<std::uint64_t> &created)
{
    if (spec.type() != ValueType::map)
        throw ScriptError{"a widget is a table: {kind = \"label\", text = \"...\"}"};
    if (depth > max_depth)
        throw ScriptError{std::format("widgets nest at most {} deep", max_depth)};
    if (widgets_.size() >= max_widgets)
        throw ScriptError{std::format("too many widgets (at most {})", max_widgets)};
    Widget widget;
    widget.id = next_id_++;
    widget.parent = parent;
    widget.mod = owner.mod;
    widget.side = owner.side;
    const Value *kind = spec.field("kind");
    if (kind == nullptr || kind->is_nil())
        kind = spec.field("type");
    if (kind == nullptr || kind->type() != ValueType::string)
        throw ScriptError{"a widget needs a kind (\"panel\", \"label\", \"button\", ...)"};
    const std::optional<WidgetKind> parsed = parse_widget_kind(kind->as_string());
    if (!parsed)
        throw ScriptError{std::format("unknown widget kind '{}'", kind->as_string())};
    widget.kind = *parsed;
    // Defaults of the kind.
    switch (widget.kind)
    {
    case WidgetKind::button:
        widget.focusable = true;
        widget.padding = {12.0F, 6.0F, 12.0F, 6.0F};
        widget.face = Face::display;
        widget.align = Align::center;
        widget.wrap = false;
        break;
    case WidgetKind::toggle:
    case WidgetKind::slider:
    case WidgetKind::text_input:
        widget.focusable = true;
        break;
    case WidgetKind::stack:
        widget.gap = 8.0F;
        break;
    case WidgetKind::grid:
    case WidgetKind::list:
        widget.gap = 4.0F;
        break;
    default:
        break;
    }
    if (widget.kind == WidgetKind::slider)
        widget.maximum = 1.0;
    std::vector<Callback> replaced;
    apply(widget, spec, true, adopted, replaced);
    const std::uint64_t id = widget.id;
    widgets_.emplace(id, std::move(widget));
    created.push_back(id);
    if (const Value *children = spec.field("children"); children != nullptr && !children->is_nil())
    {
        const bool empty_map = children->type() == ValueType::map && children->as_map().empty();
        if (children->type() != ValueType::array && !empty_map)
            throw ScriptError{"children must be an array of widgets"};
        if (!empty_map)
        {
            if (!container(find(id)->kind))
                throw ScriptError{std::format("a {} cannot hold widgets", widget_kind_name(find(id)->kind))};
            const Array &list = children->as_array();
            if (list.size() > max_children)
                throw ScriptError{std::format("a widget holds at most {} children", max_children)};
            for (const Value &child : list)
            {
                const std::uint64_t child_id = create(child, owner, id, depth + 1U, adopted, created);
                find(id)->children.push_back(child_id);
            }
        }
    }
    return id;
}

void WidgetStore::apply(Widget &widget, const Value &props, bool creating, std::vector<Callback> &adopted,
                        std::vector<Callback> &replaced)
{
    if (props.type() != ValueType::map)
        throw ScriptError{"widget properties are a table"};
    const auto set_callback = [&](Callback &slot, const Value &value, std::string_view what) {
        const Callback next = value.is_nil() ? Callback{} : callback(value, what);
        if (slot.valid() && slot != next)
            replaced.push_back(slot);
        slot = next;
        if (next.valid())
            adopted.push_back(next);
    };
    for (const auto &[key, value] : props.as_map())
    {
        if (key == "kind" || key == "type" || key == "children")
        {
            if (!creating)
                throw ScriptError{std::format("ui.set cannot change '{}' (use ui.add / ui.clear)", key)};
            continue;
        }
        if (key == "id")
            widget.name = bounded_string(value, "id", max_id_bytes);
        else if (key == "anchor")
        {
            const std::optional<Anchor> anchor = parse_anchor(bounded_string(value, "anchor", 32U));
            if (!anchor)
                throw ScriptError{std::format("unknown anchor '{}'", value.as_string())};
            widget.anchor = *anchor;
        }
        else if (key == "x")
            widget.x = finite(value, "x", -20000.0F, 20000.0F);
        else if (key == "y")
            widget.y = finite(value, "y", -20000.0F, 20000.0F);
        else if (key == "width")
            widget.width = length(value, "width");
        else if (key == "height")
            widget.height = length(value, "height");
        else if (key == "size" && value.type() == ValueType::array)
        {
            if (value.as_array().size() != 2U)
                throw ScriptError{"size is {width, height}"};
            widget.width = length(value.as_array()[0], "width");
            widget.height = length(value.as_array()[1], "height");
        }
        else if (key == "padding")
            widget.padding = insets(value, "padding");
        else if (key == "gap")
            widget.gap = finite(value, "gap", 0.0F, 2000.0F);
        else if (key == "columns")
            widget.columns = static_cast<int>(finite(value, "columns", 1.0F, 64.0F));
        else if (key == "direction")
        {
            const std::string direction = bounded_string(value, "direction", 16U);
            if (direction != "vertical" && direction != "horizontal")
                throw ScriptError{"direction is \"vertical\" or \"horizontal\""};
            widget.horizontal = direction == "horizontal";
        }
        else if (key == "align")
        {
            const std::string align = bounded_string(value, "align", 16U);
            if (align == "left" || align == "start" || align == "top")
                widget.align = Align::left;
            else if (align == "center" || align == "middle")
                widget.align = Align::center;
            else if (align == "right" || align == "end" || align == "bottom")
                widget.align = Align::right;
            else if (align == "stretch")
                widget.stretch = true;
            else
                throw ScriptError{"align is \"left\", \"center\", \"right\" or \"stretch\""};
        }
        else if (key == "stretch")
            widget.stretch = boolean(value, "stretch");
        else if (key == "visible")
            widget.visible = boolean(value, "visible");
        else if (key == "enabled")
            widget.enabled = boolean(value, "enabled");
        else if (key == "opacity")
            widget.opacity = finite(value, "opacity", 0.0F, 1.0F);
        else if (key == "color")
            read_ink(value, "color", widget.color, widget.color_key);
        else if (key == "background")
        {
            widget.background.reset();
            widget.background_key.reset();
            if (value.is_nil() || (value.type() == ValueType::string && value.as_string() == "none"))
                widget.fill = Widget::Fill::none;
            else if (value.type() == ValueType::string && !value.as_string().starts_with('#') &&
                     !parse_color_key(value.as_string()).has_value())
            {
                const std::string &look = value.as_string();
                widget.fill = look == "panel" ? Widget::Fill::panel
                              : look == "plate" ? Widget::Fill::plate
                              : look == "well"  ? Widget::Fill::well
                              : look == "slot"  ? Widget::Fill::slot
                                                : Widget::Fill::none;
                if (widget.fill == Widget::Fill::none)
                    throw ScriptError{"background is a colour, a theme ink, \"panel\", \"plate\", \"well\", "
                                      "\"slot\" or \"none\""};
            }
            else
            {
                read_ink(value, "background", widget.background, widget.background_key);
                widget.fill = Widget::Fill::color;
            }
        }
        else if (key == "border")
            read_ink(value, "border", widget.border, widget.border_key);
        else if (key == "border_width")
            widget.border_width = finite(value, "border_width", 0.0F, 64.0F);
        else if (key == "image")
        {
            widget.image = {};
            widget.sprite.clear();
            if (!value.is_nil())
            {
                const std::string path = bounded_string(value, "image", 256U);
                if (path.starts_with("game:"))
                {
                    const std::string sprite = path.substr(5U);
                    if (sprite.empty() || sprite.size() > 64U ||
                        !std::ranges::all_of(sprite, [](char c) {
                            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '/';
                        }))
                        throw ScriptError{std::format("'{}' is not a game sprite", path)};
                    widget.sprite = sprite;
                }
                else
                {
                    const std::optional<std::string> normalized = normalize_asset_path(path);
                    std::string lowered = normalized.value_or("");
                    for (char &c : lowered)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (!normalized || (!lowered.ends_with(".png") && !lowered.ends_with(".bmp")))
                        throw ScriptError{std::format("image: '{}' is not a .png or .bmp inside the mod", path)};
                    AssetRef asset{widget.mod, *normalized};
                    if (checks_.asset && !checks_.asset(asset))
                        throw ScriptError{std::format("image: '{}' was not found in the mod", path)};
                    widget.image = std::move(asset);
                }
            }
        }
        else if (key == "slice")
        {
            if (value.is_nil())
                widget.nine = false;
            else
            {
                widget.slice = insets(value, "slice");
                widget.nine = true;
            }
        }
        else if (key == "text" || key == "label")
        {
            std::vector<TextRun> runs = runs_from_value(value);
            std::size_t bytes = 0U;
            for (const TextRun &run : runs)
                bytes += run.text.size();
            if (bytes > max_text_bytes || runs.size() > max_runs)
                throw ScriptError{std::format("text is longer than {} bytes or {} runs", max_text_bytes, max_runs)};
            widget.text = std::move(runs);
        }
        else if (key == "text_size" || (key == "size" && value.is_number()))
            widget.text_size = finite(value, "text size", 4.0F, 200.0F);
        else if (key == "face")
        {
            const std::string face = bounded_string(value, "face", 16U);
            if (face != "display" && face != "text")
                throw ScriptError{"face is \"display\" or \"text\""};
            widget.face = face == "display" ? Face::display : Face::text;
        }
        else if (key == "outline")
            widget.outline = boolean(value, "outline");
        else if (key == "wrap")
            widget.wrap = boolean(value, "wrap");
        else if (key == "max_lines")
            widget.max_lines = static_cast<std::size_t>(finite(value, "max_lines", 0.0F, 1000.0F));
        else if (key == "checked")
            widget.checked = boolean(value, "checked");
        else if (key == "value")
        {
            if (widget.kind == WidgetKind::toggle)
                widget.checked = boolean(value, "value");
            else if (widget.kind == WidgetKind::text_input)
            {
                widget.input = bounded_string(value, "value", max_text_bytes);
                if (code_points(widget.input) > widget.max_length)
                    throw ScriptError{"value is longer than max_length"};
            }
            else
                widget.value = finite(value, "value", -1e12F, 1e12F);
        }
        else if (key == "min")
            widget.minimum = finite(value, "min", -1e12F, 1e12F);
        else if (key == "max")
            widget.maximum = finite(value, "max", -1e12F, 1e12F);
        else if (key == "step")
            widget.step = finite(value, "step", 0.0F, 1e12F);
        else if (key == "placeholder")
            widget.placeholder = bounded_string(value, "placeholder", 512U);
        else if (key == "max_length")
            widget.max_length = static_cast<std::size_t>(finite(value, "max_length", 1.0F, 4096.0F));
        else if (key == "item")
        {
            widget.item = value.is_nil() ? -1 : static_cast<int>(finite(value, "item", 0.0F, 65535.0F));
            if (widget.item >= 0 && checks_.item && !checks_.item(widget.item))
                throw ScriptError{std::format("item {} does not exist", widget.item)};
        }
        else if (key == "count")
            widget.count = static_cast<int>(finite(value, "count", 0.0F, 1e6F));
        else if (key == "stat")
            widget.stat = value.is_nil() ? std::string{} : bounded_string(value, "stat", max_id_bytes);
        else if (key == "show_value")
            widget.show_value = boolean(value, "show_value");
        else if (key == "scroll")
            widget.scroll = finite(value, "scroll", 0.0F, 1e7F);
        else if (key == "focusable")
            widget.focusable = boolean(value, "focusable");
        else if (key == "on_click")
            set_callback(widget.on_click, value, "on_click");
        else if (key == "on_change")
            set_callback(widget.on_change, value, "on_change");
        else if (key == "on_hover")
            set_callback(widget.on_hover, value, "on_hover");
        else if (key == "on_focus")
            set_callback(widget.on_focus, value, "on_focus");
        else if (key == "on_submit")
            set_callback(widget.on_submit, value, "on_submit");
        else
            throw ScriptError{std::format("a {} has no property '{}'", widget_kind_name(widget.kind), key)};
    }
    if (widget.kind == WidgetKind::slider || widget.kind == WidgetKind::progress)
        widget.value = std::clamp(widget.value, std::min(widget.minimum, widget.maximum),
                                  std::max(widget.minimum, widget.maximum));
    // A slot or any widget with a click handler can be reached by keys too.
    if (creating && widget.kind == WidgetKind::item_slot && widget.on_click.valid())
        widget.focusable = true;
}

void WidgetStore::update(std::uint64_t id, const Value &props, std::vector<Callback> &adopted,
                         std::vector<Callback> &replaced)
{
    Widget *widget = find(id);
    if (widget == nullptr)
        throw ScriptError{"ui.set: the widget no longer exists"};
    Widget copy = *widget;
    std::vector<Callback> taken;
    std::vector<Callback> dropped;
    apply(copy, props, false, taken, dropped);
    if (copy.kind == WidgetKind::item_slot && copy.on_click.valid())
        copy.focusable = true;
    *widget = std::move(copy);
    adopted.insert(adopted.end(), taken.begin(), taken.end());
    replaced.insert(replaced.end(), dropped.begin(), dropped.end());
}

Value WidgetStore::get(std::uint64_t id, std::string_view property) const
{
    const Widget *widget = find(id);
    if (widget == nullptr)
        return {};
    if (property == "value")
    {
        if (widget->kind == WidgetKind::toggle)
            return Value{widget->checked};
        if (widget->kind == WidgetKind::text_input)
            return Value{widget->input};
        return Value{widget->value};
    }
    if (property == "checked")
        return Value{widget->checked};
    if (property == "text")
        return Value{plain_text(widget->text)};
    if (property == "visible")
        return Value{widget->visible};
    if (property == "enabled")
        return Value{widget->enabled};
    if (property == "id")
        return Value{widget->name};
    if (property == "kind")
        return Value{std::string{widget_kind_name(widget->kind)}};
    if (property == "item")
        return widget->item >= 0 ? Value{widget->item} : Value{};
    if (property == "count")
        return Value{widget->count};
    if (property == "scroll")
        return Value{widget->scroll};
    if (property == "bounds")
        return Value{Map{{"x", Value{widget->frame.x}},
                         {"y", Value{widget->frame.y}},
                         {"width", Value{widget->frame.width}},
                         {"height", Value{widget->frame.height}}}};
    throw ScriptError{std::format("ui.get: no property '{}'", property)};
}

void WidgetStore::destroy(std::uint64_t id, std::vector<Callback> &released)
{
    Widget *widget = find(id);
    if (widget == nullptr)
        return;
    if (Widget *parent = find(widget->parent))
        std::erase(parent->children, id);
    // Iteratively, so a deep tree never recurses deeply here.
    std::vector<std::uint64_t> pending{id};
    while (!pending.empty())
    {
        const std::uint64_t next = pending.back();
        pending.pop_back();
        const auto found = widgets_.find(next);
        if (found == widgets_.end())
            continue;
        for (const Callback *slot : {&found->second.on_click, &found->second.on_change, &found->second.on_hover,
                                     &found->second.on_focus, &found->second.on_submit})
            if (slot->valid())
                released.push_back(*slot);
        pending.insert(pending.end(), found->second.children.begin(), found->second.children.end());
        widgets_.erase(found);
    }
}

void WidgetStore::clear_children(std::uint64_t id, std::vector<Callback> &released)
{
    Widget *widget = find(id);
    if (widget == nullptr)
        return;
    const std::vector<std::uint64_t> children = widget->children;
    for (const std::uint64_t child : children)
        destroy(child, released);
}

void WidgetStore::clear(std::vector<Callback> &released)
{
    for (const auto &[id, widget] : widgets_)
        for (const Callback *slot :
             {&widget.on_click, &widget.on_change, &widget.on_hover, &widget.on_focus, &widget.on_submit})
            if (slot->valid())
                released.push_back(*slot);
    widgets_.clear();
}

Widget *WidgetStore::find(std::uint64_t id) noexcept
{
    const auto found = widgets_.find(id);
    return found == widgets_.end() ? nullptr : &found->second;
}

const Widget *WidgetStore::find(std::uint64_t id) const noexcept
{
    const auto found = widgets_.find(id);
    return found == widgets_.end() ? nullptr : &found->second;
}

std::optional<std::uint64_t> WidgetStore::find_named(std::uint64_t root, std::string_view name) const
{
    std::vector<std::uint64_t> pending{root};
    while (!pending.empty())
    {
        const std::uint64_t next = pending.back();
        pending.pop_back();
        const Widget *widget = find(next);
        if (widget == nullptr)
            continue;
        if (widget->name == name)
            return next;
        pending.insert(pending.end(), widget->children.rbegin(), widget->children.rend());
    }
    return std::nullopt;
}

std::uint64_t WidgetStore::root_of(std::uint64_t id) const noexcept
{
    const Widget *widget = find(id);
    std::size_t guard = 0U;
    while (widget != nullptr && widget->parent != 0U && guard++ < max_depth + 2U)
    {
        id = widget->parent;
        widget = find(id);
    }
    return widget != nullptr ? id : 0U;
}

std::size_t WidgetStore::depth_below(std::uint64_t id) const noexcept
{
    const Widget *widget = find(id);
    if (widget == nullptr)
        return 0U;
    std::size_t deepest = 0U;
    for (const std::uint64_t child : widget->children)
        deepest = std::max(deepest, depth_below(child));
    return deepest + 1U;
}

void WidgetStore::count_subtree(std::uint64_t id, std::size_t &count) const
{
    const Widget *widget = find(id);
    if (widget == nullptr)
        return;
    ++count;
    for (const std::uint64_t child : widget->children)
        count_subtree(child, count);
}

// ---- Layout ------------------------------------------------------------------

namespace
{

[[nodiscard]] float resolve(const Length &length, float available, float content) noexcept
{
    switch (length.mode)
    {
    case Length::Mode::units:
        return length.value;
    case Length::Mode::fraction:
        return std::max(0.0F, available) * length.value;
    case Length::Mode::fill:
        return std::max(0.0F, available);
    case Length::Mode::automatic:
        return content;
    }
    return content;
}

} // namespace

WidgetStore::Measured WidgetStore::measure(const Widget &widget, float available_width, float available_height,
                                           const LayoutContext &context) const
{
    const float scale = context.canvas->scale;
    const float pad_x = widget.padding.left + widget.padding.right;
    const float pad_y = widget.padding.top + widget.padding.bottom;
    float content_width = 0.0F;
    float content_height = 0.0F;
    const float inner_width = resolve(widget.width, available_width, available_width) - pad_x;
    const auto text_size = [&](float max_width, bool wrap) {
        const float cap = text_cap(widget, context);
        const TextBlock block = layout_text(widget.text, widget.face, cap, {}, wrap ? max_width * scale : 0.0F,
                                            widget.max_lines, widget.align, *context.measure);
        // Room under the last line's capitals for its descenders.
        const float descent = block.lines > 0U ? cap * 0.3F : 0.0F;
        return std::pair{block.width / scale, (block.height + descent) / scale};
    };
    switch (widget.kind)
    {
    case WidgetKind::label: {
        const auto [width, height] = text_size(std::max(1.0F, inner_width), widget.wrap);
        content_width = width;
        content_height = height;
        break;
    }
    case WidgetKind::button: {
        const auto [width, height] = text_size(0.0F, false);
        content_width = std::max(width, 60.0F - pad_x);
        content_height = std::max(height, 30.0F - pad_y);
        break;
    }
    case WidgetKind::toggle: {
        const auto [width, height] = text_size(0.0F, false);
        content_width = 18.0F + (width > 0.0F ? 8.0F + width : 0.0F);
        content_height = std::max(18.0F, height);
        break;
    }
    case WidgetKind::slider:
        content_width = 200.0F;
        content_height = 22.0F;
        break;
    case WidgetKind::text_input:
        content_width = 220.0F;
        content_height = 30.0F;
        break;
    case WidgetKind::progress: {
        const auto [width, height] = text_size(0.0F, false);
        content_width = std::max(200.0F, width + 12.0F);
        content_height = std::max(12.0F, height > 0.0F ? height + 6.0F : 0.0F);
        break;
    }
    case WidgetKind::item_slot:
        content_width = slot_units;
        content_height = slot_units;
        break;
    case WidgetKind::image:
        content_width = 64.0F;
        content_height = 64.0F;
        break;
    case WidgetKind::spacer:
        break;
    case WidgetKind::panel:
        for (const std::uint64_t id : widget.children)
            if (const Widget *child = find(id); child != nullptr && child->visible)
            {
                const Measured size = measure(*child, available_width - pad_x, available_height - pad_y, context);
                content_width = std::max(content_width, size.width + std::fabs(child->x));
                content_height = std::max(content_height, size.height + std::fabs(child->y));
            }
        break;
    case WidgetKind::stack:
    case WidgetKind::list: {
        const bool horizontal = widget.kind == WidgetKind::stack && widget.horizontal;
        std::size_t shown = 0U;
        for (const std::uint64_t id : widget.children)
            if (const Widget *child = find(id); child != nullptr && child->visible)
            {
                const Measured size = measure(*child, available_width - pad_x, available_height - pad_y, context);
                if (horizontal)
                {
                    content_width += size.width;
                    content_height = std::max(content_height, size.height);
                }
                else
                {
                    content_height += size.height;
                    content_width = std::max(content_width, size.width);
                }
                ++shown;
            }
        const float gaps = widget.gap * static_cast<float>(shown > 0U ? shown - 1U : 0U);
        (horizontal ? content_width : content_height) += gaps;
        break;
    }
    case WidgetKind::grid: {
        const auto columns = static_cast<std::size_t>(std::max(1, widget.columns));
        float cell = 0.0F;
        float row_height = 0.0F;
        float total = 0.0F;
        std::size_t index = 0U;
        for (const std::uint64_t id : widget.children)
            if (const Widget *child = find(id); child != nullptr && child->visible)
            {
                const Measured size = measure(*child, available_width - pad_x, available_height - pad_y, context);
                cell = std::max(cell, size.width);
                row_height = std::max(row_height, size.height);
                if (++index % columns == 0U)
                {
                    total += row_height + widget.gap;
                    row_height = 0.0F;
                }
            }
        if (index % columns != 0U)
            total += row_height + widget.gap;
        content_width = cell * static_cast<float>(std::min(columns, std::max<std::size_t>(index, 1U))) +
                        widget.gap * static_cast<float>(std::min(columns, std::max<std::size_t>(index, 1U)) - 1U);
        content_height = std::max(0.0F, total - widget.gap);
        break;
    }
    case WidgetKind::count:
        break;
    }
    Measured result;
    result.width = std::max(0.0F, resolve(widget.width, available_width, content_width + pad_x));
    result.height = std::max(0.0F, resolve(widget.height, available_height, content_height + pad_y));
    // A list never asks for more height than it is offered.
    if (widget.kind == WidgetKind::list && widget.height.mode == Length::Mode::automatic)
        result.height = std::min(result.height, std::max(0.0F, available_height));
    return result;
}

void WidgetStore::arrange(Widget &widget, const Rect &units, const Rect &clip, const LayoutContext &context)
{
    const float scale = context.canvas->scale;
    widget.frame = to_window(units, scale);
    widget.clip = widget.frame.intersect(clip);
    const Rect content{units.x + widget.padding.left, units.y + widget.padding.top,
                       std::max(0.0F, units.width - widget.padding.left - widget.padding.right),
                       std::max(0.0F, units.height - widget.padding.top - widget.padding.bottom)};
    if (!container(widget.kind) || widget.children.empty())
        return;
    // Children are arranged against copies of their ids: a child is a
    // different element of the same map, never this one.
    const std::vector<std::uint64_t> children = widget.children;
    if (widget.kind == WidgetKind::panel)
    {
        for (const std::uint64_t id : children)
        {
            Widget *child = find(id);
            if (child == nullptr || !child->visible)
                continue;
            const Measured size = measure(*child, content.width, content.height, context);
            const float x = content.x + anchor_x(child->anchor) * (content.width - size.width) + child->x;
            const float y = content.y + anchor_y(child->anchor) * (content.height - size.height) + child->y;
            arrange(*child, {x, y, size.width, size.height}, widget.clip, context);
        }
        return;
    }
    if (widget.kind == WidgetKind::grid)
    {
        const auto columns = static_cast<std::size_t>(std::max(1, widget.columns));
        const float share = (content.width - widget.gap * static_cast<float>(columns - 1U)) / static_cast<float>(columns);
        // Cells are as wide as the widest child, at most an equal share; with
        // `stretch` (or a child that fills) they take the whole share.
        float widest = 0.0F;
        bool fills = widget.stretch;
        for (const std::uint64_t id : children)
            if (const Widget *child = find(id); child != nullptr && child->visible)
            {
                widest = std::max(widest, measure(*child, share, content.height, context).width);
                fills = fills || child->width.mode == Length::Mode::fill;
            }
        const float cell = fills ? share : std::min(share, widest);
        float row_top = content.y;
        float row_height = 0.0F;
        std::size_t index = 0U;
        for (const std::uint64_t id : children)
        {
            Widget *child = find(id);
            if (child == nullptr || !child->visible)
                continue;
            const Measured size = measure(*child, cell, content.height, context);
            const std::size_t column = index % columns;
            const float width = child->width.mode == Length::Mode::automatic || widget.stretch
                                    ? std::min(std::max(size.width, widget.stretch ? cell : 0.0F), cell)
                                    : std::min(size.width, cell);
            arrange(*child,
                    {content.x + static_cast<float>(column) * (cell + widget.gap), row_top, width, size.height},
                    widget.clip, context);
            row_height = std::max(row_height, size.height);
            if (++index % columns == 0U)
            {
                row_top += row_height + widget.gap;
                row_height = 0.0F;
            }
        }
        return;
    }
    // Stacks and lists: fixed sizes first, then what is left shared by "fill".
    const bool horizontal = widget.kind == WidgetKind::stack && widget.horizontal;
    const bool list = widget.kind == WidgetKind::list;
    const float main = horizontal ? content.width : content.height;
    const float cross = horizontal ? content.height : content.width;
    std::vector<std::pair<Widget *, Measured>> placed;
    float fixed = 0.0F;
    std::size_t fills = 0U;
    for (const std::uint64_t id : children)
    {
        Widget *child = find(id);
        if (child == nullptr || !child->visible)
            continue;
        Measured size = measure(*child, content.width, list ? std::numeric_limits<float>::max() / 4.0F : content.height,
                                context);
        const Length &along = horizontal ? child->width : child->height;
        if (along.mode == Length::Mode::fill && !list)
            ++fills;
        else
            fixed += horizontal ? size.width : size.height;
        placed.emplace_back(child, size);
    }
    const float gaps = widget.gap * static_cast<float>(placed.empty() ? 0U : placed.size() - 1U);
    const float share = fills > 0U ? std::max(0.0F, main - fixed - gaps) / static_cast<float>(fills) : 0.0F;
    if (list)
    {
        widget.content_height = fixed + gaps;
        widget.scroll = std::clamp(widget.scroll, 0.0F, std::max(0.0F, widget.content_height - content.height));
    }
    float along = (horizontal ? content.x : content.y) - (list ? widget.scroll : 0.0F);
    const Rect inner_clip = list ? to_window(content, scale).intersect(widget.clip) : widget.clip;
    for (auto &[child, size] : placed)
    {
        const Length &main_length = horizontal ? child->width : child->height;
        const float extent = main_length.mode == Length::Mode::fill && !list ? share
                                                                             : (horizontal ? size.width : size.height);
        const Length &cross_length = horizontal ? child->height : child->width;
        float breadth = horizontal ? size.height : size.width;
        // `stretch` widens the children that did not ask for a size.
        if ((widget.stretch && cross_length.mode == Length::Mode::automatic) || cross_length.mode == Length::Mode::fill)
            breadth = cross;
        breadth = std::min(breadth, cross);
        const float offset = widget.stretch ? 0.0F
                             : widget.align == Align::center ? (cross - breadth) * 0.5F
                             : widget.align == Align::right  ? cross - breadth
                                                             : 0.0F;
        const Rect box = horizontal ? Rect{along, content.y + offset, extent, breadth}
                                    : Rect{content.x + offset, along, breadth, extent};
        arrange(*child, box, inner_clip, context);
        along += extent + widget.gap;
    }
}

void WidgetStore::layout(std::uint64_t root, const Rect &area, const LayoutContext &context)
{
    Widget *widget = find(root);
    if (widget == nullptr || context.canvas == nullptr || context.measure == nullptr || context.frame == nullptr)
        return;
    const Measured size = measure(*widget, area.width, area.height, context);
    const float x = area.x + anchor_x(widget->anchor) * (area.width - size.width) + widget->x;
    const float y = area.y + anchor_y(widget->anchor) * (area.height - size.height) + widget->y;
    const Rect window{0.0F, 0.0F, context.canvas->width * context.canvas->scale,
                      context.canvas->height * context.canvas->scale};
    last_scale_ = context.canvas->scale;
    arrange(*widget, {x, y, size.width, size.height}, window, context);
}

// ---- Drawing -----------------------------------------------------------------

void WidgetStore::draw(std::uint64_t root, const LayoutContext &context, std::uint64_t focused,
                       std::uint64_t hovered, bool editing, DrawList &out) const
{
    const Widget *widget = find(root);
    if (widget == nullptr || !widget->visible || context.canvas == nullptr || context.frame == nullptr ||
        context.measure == nullptr)
        return;
    draw_one(*widget, context, focused, hovered, editing, out);
}

void WidgetStore::draw_one(const Widget &widget, const LayoutContext &context, std::uint64_t focused,
                           std::uint64_t hovered, bool editing, DrawList &out) const
{
    if (!widget.visible || widget.clip.empty() || widget.opacity <= 0.0F)
        return;
    const FrameContext &frame_context = *context.frame;
    const float scale = context.canvas->scale;
    // Opacity carries down the tree.
    float opacity = widget.opacity;
    for (const Widget *up = find(widget.parent); up != nullptr; up = find(up->parent))
        opacity *= up->opacity;
    const auto tint = [&](Color color) { return scale_alpha(color, opacity); };
    const bool hot = widget.enabled && (widget.id == focused || widget.id == hovered) && interactive(widget);
    const Rect box = widget.frame;
    const Rect content{box.x + widget.padding.left * scale, box.y + widget.padding.top * scale,
                       std::max(0.0F, box.width - (widget.padding.left + widget.padding.right) * scale),
                       std::max(0.0F, box.height - (widget.padding.top + widget.padding.bottom) * scale)};
    const NineSlice *panel_image = frame_context.images[static_cast<std::size_t>(ImageKey::panel)];
    const auto line = std::max(1.0F, std::round(scale));
    // A colour named by its ink follows the theme.
    const auto own = [&](const std::optional<Color> &color, const std::optional<ColorKey> &key) {
        return key ? std::optional<Color>{ink(frame_context, *key)} : color;
    };
    const std::optional<Color> own_color = own(widget.color, widget.color_key);
    const std::optional<Color> own_background = own(widget.background, widget.background_key);
    const std::optional<Color> own_border = own(widget.border, widget.border_key);
    const Color text_ink = own_color.value_or(ink(frame_context, ColorKey::menu_text));
    const Color disabled = ink(frame_context, ColorKey::menu_disabled);

    // Background.
    switch (widget.fill)
    {
    case Widget::Fill::color:
        if (own_background)
            fill(out, box, tint(*own_background));
        break;
    case Widget::Fill::panel:
        if (panel_image != nullptr)
            nine_slice(out, box, *panel_image, scale, opacity);
        else
            fill(out, box, tint(ink(frame_context, ColorKey::hud_panel)));
        break;
    case Widget::Fill::plate:
        fill(out, box, tint(ink(frame_context, ColorKey::hud_plate)));
        break;
    case Widget::Fill::well:
        fill(out, box, tint(ink(frame_context, ColorKey::hud_well)));
        break;
    case Widget::Fill::slot:
        fill(out, box, tint(ink(frame_context, ColorKey::hud_slot)));
        break;
    case Widget::Fill::none:
        break;
    }
    if (own_border && widget.border_width > 0.0F)
        frame(out, box, tint(*own_border), widget.border_width * scale);
    else if (own_border)
        frame(out, box, tint(*own_border), line);

    const auto draw_text = [&](const Rect &area, Color color, Align align, bool wrap, bool center_vertically) {
        if (widget.text.empty() || area.width <= 0.0F)
            return;
        const float cap = text_cap(widget, context);
        TextBlock block = layout_text(widget.text, widget.face, cap, color, wrap ? area.width : 0.0F,
                                      widget.max_lines, align, *context.measure);
        if (!wrap && block.width > area.width + 0.5F)
            block = layout_text(widget.text, widget.face, cap, color, area.width, 1U, align, *context.measure);
        float left = area.x;
        if (!wrap && block.width <= area.width)
            left += (area.width - block.width) * (align == Align::center ? 0.5F : align == Align::right ? 1.0F : 0.0F);
        const float top = center_vertically ? area.y + (area.height - block.height) * 0.5F : area.y;
        text_block(out, block, std::round(left), std::round(top), widget.outline, opacity);
    };

    switch (widget.kind)
    {
    case WidgetKind::label:
        draw_text(content, widget.enabled ? text_ink : disabled, widget.align, widget.wrap, false);
        break;
    case WidgetKind::button: {
        const NineSlice *image = frame_context.images[static_cast<std::size_t>(hot ? ImageKey::button_focus
                                                                                       : ImageKey::button)];
        if (image == nullptr && hot)
            image = frame_context.images[static_cast<std::size_t>(ImageKey::button)];
        if (widget.fill == Widget::Fill::none)
        {
            if (image != nullptr)
                nine_slice(out, box, *image, scale, opacity * (widget.enabled ? 1.0F : 0.5F));
            else
            {
                fill(out, box, tint({0U, 0U, 0U, static_cast<std::uint8_t>(hot ? 215U : 165U)}));
                if (hot)
                    fill(out, box, tint(ink(frame_context, ColorKey::hud_band)));
            }
        }
        if (image == nullptr || widget.fill != Widget::Fill::none)
            frame(out, box,
                  tint(!widget.enabled ? ink(frame_context, ColorKey::hud_line)
                       : hot          ? ink(frame_context, ColorKey::menu_text)
                                      : Color{255U, 255U, 255U, 100U}),
                  line);
        const Color label = !widget.enabled ? disabled
                            : own_color     ? *own_color
                            : hot           ? ink(frame_context, ColorKey::menu_focus)
                                            : ink(frame_context, ColorKey::menu_row);
        draw_text(content, label, widget.align, false, true);
        break;
    }
    case WidgetKind::toggle: {
        const float side = 18.0F * scale;
        const Rect mark{content.x, content.y + (content.height - side) * 0.5F, side, side};
        if (hot)
            fill(out, box, tint(ink(frame_context, ColorKey::hud_band)));
        fill(out, mark, tint({0U, 0U, 0U, 200U}));
        frame(out, mark, tint(widget.enabled ? ink(frame_context, ColorKey::menu_text) : disabled), line);
        if (widget.checked)
        {
            const float inset = std::round(4.0F * scale);
            fill(out, {mark.x + inset, mark.y + inset, mark.width - inset * 2.0F, mark.height - inset * 2.0F},
                 tint(widget.enabled ? ink(frame_context, ColorKey::menu_accent) : disabled));
        }
        draw_text({mark.right() + 8.0F * scale, content.y, std::max(0.0F, content.right() - mark.right() - 8.0F * scale),
                   content.height},
                  widget.enabled ? (hot ? ink(frame_context, ColorKey::menu_focus) : text_ink) : disabled, Align::left,
                  false, true);
        break;
    }
    case WidgetKind::slider: {
        if (hot)
            fill(out, box, tint(ink(frame_context, ColorKey::hud_band)));
        const float range = static_cast<float>(widget.maximum - widget.minimum);
        const float share = range != 0.0F ? std::clamp(static_cast<float>(widget.value - widget.minimum) / range, 0.0F, 1.0F)
                                          : 0.0F;
        float value_room = 0.0F;
        if (widget.show_value)
            value_room = std::min(content.width * 0.3F, 48.0F * scale);
        const float bar_height = std::max(4.0F, std::round(8.0F * scale));
        const Rect track{content.x, content.y + (content.height - bar_height) * 0.5F,
                         std::max(0.0F, content.width - value_room), bar_height};
        fill(out, track, tint({0U, 0U, 0U, 220U}));
        fill(out, {track.x + line, track.y + line, std::max(0.0F, (track.width - line * 2.0F) * share),
                   track.height - line * 2.0F},
             tint(widget.enabled ? own_color.value_or(ink(frame_context, ColorKey::menu_accent)) : disabled));
        const float knob = std::round(6.0F * scale);
        fill(out, {track.x + track.width * share - knob * 0.5F, content.y + content.height * 0.15F, knob,
                   content.height * 0.7F},
             tint(widget.enabled ? ink(frame_context, hot ? ColorKey::menu_focus : ColorKey::menu_text) : disabled));
        if (widget.show_value)
        {
            const double shown = widget.step >= 1.0 || widget.step == 0.0 ? std::round(widget.value) : widget.value;
            const std::string number = widget.step >= 1.0 || (widget.step == 0.0 && range > 10.0F)
                                           ? std::format("{}", static_cast<std::int64_t>(shown))
                                           : std::format("{:.2f}", widget.value);
            const std::vector<TextRun> runs{TextRun{number}};
            const float cap = label_size * scale * frame_context.text_factor;
            const TextBlock block = layout_text(runs, Face::text, cap, text_ink, 0.0F, 1U, Align::right, *context.measure);
            text_block(out, block, std::round(content.right() - block.width), std::round(content.y + (content.height - block.height) * 0.5F),
                       false, opacity);
        }
        break;
    }
    case WidgetKind::text_input: {
        fill(out, box, tint({0U, 0U, 0U, 230U}));
        frame(out, box,
              tint(widget.id == focused && editing ? ink(frame_context, ColorKey::menu_accent)
                   : hot                           ? ink(frame_context, ColorKey::menu_text)
                                                   : Color{255U, 255U, 255U, 120U}),
              line);
        const float pad = 8.0F * scale;
        const Rect field{box.x + pad, box.y, std::max(0.0F, box.width - pad * 2.0F), box.height};
        const float cap = text_cap(widget, context);
        const bool empty = widget.input.empty();
        const bool caret = widget.id == focused && editing &&
                           (frame_context.reduced_motion || std::fmod(frame_context.time, 1.0) < 0.5);
        std::string shown = empty ? widget.placeholder : widget.input;
        // Keep the end of a long entry in view.
        while (!empty && !shown.empty() &&
               context.measure->width(widget.face, cap, shown + "|") > field.width)
            shown.erase(0U, 1U + (shown.size() > 1U && (static_cast<unsigned char>(shown[1]) & 0xC0U) == 0x80U ? 1U : 0U));
        const std::vector<TextRun> runs{TextRun{empty ? shown : shown + (caret ? "|" : "")}};
        const Color color = empty ? ink(frame_context, ColorKey::menu_quiet) : text_ink;
        const TextBlock block = layout_text(runs, widget.face, cap, color, field.width, 1U, Align::left, *context.measure);
        text_block(out, block, std::round(field.x), std::round(field.y + (field.height - block.height) * 0.5F), false,
                   opacity);
        if (empty && caret)
        {
            const std::vector<TextRun> bar{TextRun{"|"}};
            const TextBlock mark = layout_text(bar, widget.face, cap, text_ink, 0.0F, 1U, Align::left, *context.measure);
            text_block(out, mark, std::round(field.x), std::round(field.y + (field.height - mark.height) * 0.5F), false,
                       opacity);
        }
        break;
    }
    case WidgetKind::progress: {
        double value = widget.value;
        double maximum = widget.maximum;
        double minimum = widget.minimum;
        if (!widget.stat.empty() && context.stat)
            if (const auto stat = context.stat(widget.stat))
            {
                value = stat->first;
                maximum = stat->second;
                minimum = 0.0;
            }
        const float range = static_cast<float>(maximum - minimum);
        const float share = range > 0.0F ? std::clamp(static_cast<float>(value - minimum) / range, 0.0F, 1.0F) : 0.0F;
        fill(out, box, tint({0U, 0U, 0U, 200U}));
        const Rect inner{box.x + line, box.y + line, std::max(0.0F, box.width - line * 2.0F),
                         std::max(0.0F, box.height - line * 2.0F)};
        fill(out, inner, tint({46U, 48U, 50U, 235U}));
        if (share > 0.0F)
        {
            const Rect level{inner.x, inner.y, std::max(1.0F, inner.width * share), inner.height};
            fill(out, level, tint(own_color.value_or(ink(frame_context, ColorKey::menu_accent))));
            fill(out, {level.x, level.y, level.width, std::max(1.0F, std::round(2.0F * scale))},
                 tint({255U, 255U, 255U, 56U}));
        }
        draw_text({inner.x + 4.0F * scale, inner.y, std::max(0.0F, inner.width - 8.0F * scale), inner.height},
                  ink(frame_context, ColorKey::menu_text), widget.align, false, true);
        break;
    }
    case WidgetKind::item_slot: {
        const NineSlice *image = frame_context.images[static_cast<std::size_t>(ImageKey::slot)];
        if (widget.fill == Widget::Fill::none)
        {
            if (image != nullptr)
                nine_slice(out, box, *image, scale, opacity);
            else
                fill(out, box, tint(hot ? ink(frame_context, ColorKey::hud_band)
                                        : ink(frame_context, widget.item >= 0 ? ColorKey::hud_slot_full
                                                                              : ColorKey::hud_slot)));
        }
        if (widget.item >= 0)
        {
            DrawCmd item;
            item.kind = DrawCmd::Kind::item;
            const float inset = 5.0F * scale;
            item.rect = {box.x + inset, box.y + inset, std::max(0.0F, box.width - inset * 2.0F),
                         std::max(0.0F, box.height - inset * 2.0F)};
            item.item = widget.item;
            item.count = widget.count;
            item.color = tint({255U, 255U, 255U, 255U});
            item.cap = label_size * scale * frame_context.text_factor;
            out.push_back(std::move(item));
        }
        frame(out, box, tint(hot ? ink(frame_context, ColorKey::menu_text) : ink(frame_context, ColorKey::hud_line)),
              hot ? std::round(2.0F * scale) : line);
        break;
    }
    case WidgetKind::image: {
        if (widget.image.empty() && widget.sprite.empty())
            break;
        DrawCmd image;
        image.kind = DrawCmd::Kind::image;
        image.rect = content;
        image.image = widget.image;
        image.sprite = widget.sprite;
        image.nine = widget.nine;
        image.slice = widget.slice;
        image.slice_scale = scale;
        image.color = tint(own_color.value_or(Color{}));
        out.push_back(std::move(image));
        break;
    }
    case WidgetKind::panel:
    case WidgetKind::stack:
    case WidgetKind::grid:
    case WidgetKind::spacer:
    case WidgetKind::count:
        break;
    case WidgetKind::list:
        break;
    }
    if (hot && widget.kind != WidgetKind::button && widget.kind != WidgetKind::toggle &&
        widget.kind != WidgetKind::slider && widget.kind != WidgetKind::item_slot && widget.kind != WidgetKind::text_input)
        fill(out, box, tint(ink(frame_context, ColorKey::hud_band)));

    if (widget.children.empty())
        return;
    const bool list = widget.kind == WidgetKind::list;
    if (list)
    {
        DrawCmd clip;
        clip.kind = DrawCmd::Kind::clip_push;
        clip.rect = content.intersect(widget.clip);
        out.push_back(std::move(clip));
    }
    for (const std::uint64_t id : widget.children)
        if (const Widget *child = find(id))
            draw_one(*child, context, focused, hovered, editing, out);
    if (list)
    {
        DrawCmd clip;
        clip.kind = DrawCmd::Kind::clip_pop;
        out.push_back(std::move(clip));
        // Where the visible part is in the whole list.
        const float visible = content.height / scale;
        if (widget.content_height > visible + 0.5F)
        {
            const float track_x = box.right() - std::round(3.0F * scale);
            const Rect track{track_x, content.y, std::max(2.0F, std::round(3.0F * scale)), content.height};
            fill(out, track, tint({255U, 255U, 255U, 48U}));
            const float thumb = std::max(track.height * visible / widget.content_height, 6.0F * scale);
            const float position = widget.scroll / std::max(1.0F, widget.content_height - visible);
            fill(out, {track.x, track.y + (track.height - thumb) * std::clamp(position, 0.0F, 1.0F), track.width, thumb},
                 tint(ink(frame_context, ColorKey::menu_accent)));
        }
    }
}

// ---- Hit testing and focus -----------------------------------------------------

std::optional<std::uint64_t> WidgetStore::hit(std::uint64_t root, float x, float y) const
{
    const Widget *widget = find(root);
    if (widget == nullptr || !widget->visible || !widget->clip.contains(x, y))
        return std::nullopt;
    for (auto child = widget->children.rbegin(); child != widget->children.rend(); ++child)
        if (const std::optional<std::uint64_t> found = hit(*child, x, y))
            return found;
    if (widget->enabled && interactive(*widget))
        return root;
    return std::nullopt;
}

std::vector<std::uint64_t> WidgetStore::focusables(std::uint64_t root) const
{
    std::vector<std::uint64_t> result;
    std::vector<std::uint64_t> pending{root};
    while (!pending.empty())
    {
        const std::uint64_t next = pending.back();
        pending.pop_back();
        const Widget *widget = find(next);
        if (widget == nullptr || !widget->visible)
            continue;
        if (takes_focus(*widget))
            result.push_back(next);
        pending.insert(pending.end(), widget->children.rbegin(), widget->children.rend());
    }
    return result;
}

void WidgetStore::reveal(std::uint64_t id)
{
    const Widget *target = find(id);
    if (target == nullptr)
        return;
    // Frames are window pixels from the last layout; scroll is in units.
    const float scale = std::max(1e-3F, last_scale_);
    for (Widget *up = find(target->parent); up != nullptr; up = find(up->parent))
    {
        if (up->kind != WidgetKind::list || up->frame.height <= 0.0F)
            continue;
        const float top = up->frame.y + up->padding.top * scale;
        const float bottom = up->frame.bottom() - up->padding.bottom * scale;
        if (target->frame.y < top)
            up->scroll = std::max(0.0F, up->scroll - (top - target->frame.y) / scale);
        else if (target->frame.bottom() > bottom)
            up->scroll += (target->frame.bottom() - bottom) / scale;
    }
}

// ---- Routing -------------------------------------------------------------------

namespace
{

void emit(std::vector<Dispatch> &out, const Widget &widget, const Callback &callback, std::string_view event,
          Value value)
{
    if (!callback.valid() && widget.action.kind == InternalAction::Kind::none)
        return;
    Dispatch dispatch;
    dispatch.callback = callback;
    dispatch.widget = widget_handle(widget.id);
    dispatch.event = std::string{event};
    dispatch.value = std::move(value);
    if (event == "click" || event == "change" || event == "submit")
        dispatch.action = widget.action;
    out.push_back(std::move(dispatch));
}

void slider_to(Widget &widget, float x, std::vector<Dispatch> &out)
{
    const Rect track{widget.frame.x + widget.padding.left, widget.frame.y,
                     std::max(1.0F, widget.frame.width - widget.padding.left - widget.padding.right), 1.0F};
    const double share = std::clamp(static_cast<double>((x - track.x) / track.width), 0.0, 1.0);
    const double next = snap(widget, widget.minimum + share * (widget.maximum - widget.minimum));
    if (next != widget.value)
    {
        widget.value = next;
        emit(out, widget, widget.on_change, "change", Value{next});
    }
}

void activate(Widget &widget, Surface &surface, std::vector<Dispatch> &out)
{
    if (!widget.enabled)
        return;
    switch (widget.kind)
    {
    case WidgetKind::toggle:
        widget.checked = !widget.checked;
        emit(out, widget, widget.on_change, "change", Value{widget.checked});
        break;
    case WidgetKind::text_input:
        surface.editing = true;
        break;
    case WidgetKind::slider:
        break;
    default:
        emit(out, widget, widget.on_click, "click", Value{});
        break;
    }
}

[[nodiscard]] float center_x(const Rect &rect) noexcept
{
    return rect.x + rect.width * 0.5F;
}
[[nodiscard]] float center_y(const Rect &rect) noexcept
{
    return rect.y + rect.height * 0.5F;
}

} // namespace

void set_focus(WidgetStore &store, Surface &surface, std::uint64_t id, std::vector<Dispatch> &out)
{
    if (surface.focused == id)
        return;
    if (Widget *old = store.find(surface.focused))
        emit(out, *old, old->on_focus, "focus", Value{false});
    surface.focused = id;
    surface.editing = false;
    if (Widget *next = store.find(id))
    {
        emit(out, *next, next->on_focus, "focus", Value{true});
        store.reveal(id);
    }
}

bool move_focus(WidgetStore &store, Surface &surface, NavKey direction, std::vector<Dispatch> &out)
{
    const std::vector<std::uint64_t> candidates = store.focusables(surface.root);
    if (candidates.empty())
        return false;
    const Widget *current = store.find(surface.focused);
    if (current == nullptr || std::ranges::find(candidates, surface.focused) == candidates.end())
    {
        set_focus(store, surface, direction == NavKey::up || direction == NavKey::previous ? candidates.back()
                                                                                         : candidates.front(),
                  out);
        return true;
    }
    if (direction == NavKey::next || direction == NavKey::previous)
    {
        const auto at = std::ranges::find(candidates, surface.focused) - candidates.begin();
        const auto count = static_cast<std::ptrdiff_t>(candidates.size());
        const std::ptrdiff_t next = (at + (direction == NavKey::next ? 1 : count - 1)) % count;
        set_focus(store, surface, candidates[static_cast<std::size_t>(next)], out);
        return true;
    }
    const float x = center_x(current->frame);
    const float y = center_y(current->frame);
    const bool vertical = direction == NavKey::up || direction == NavKey::down;
    const float sign = direction == NavKey::down || direction == NavKey::right ? 1.0F : -1.0F;
    std::uint64_t best = 0U;
    float best_score = std::numeric_limits<float>::max();
    for (const std::uint64_t id : candidates)
    {
        if (id == surface.focused)
            continue;
        const Widget *candidate = store.find(id);
        const float dx = center_x(candidate->frame) - x;
        const float dy = center_y(candidate->frame) - y;
        const float along = (vertical ? dy : dx) * sign;
        const float across = std::fabs(vertical ? dx : dy);
        if (along <= 0.5F)
            continue;
        const float score = along + across * 2.0F;
        if (score < best_score)
        {
            best_score = score;
            best = id;
        }
    }
    if (best == 0U && vertical)
    {
        // Up from the first row goes to the last and down from the last to the
        // first, as the game's menus wrap.
        float extreme = sign > 0.0F ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max();
        for (const std::uint64_t id : candidates)
        {
            if (id == surface.focused)
                continue;
            const Widget *candidate = store.find(id);
            const float cy = center_y(candidate->frame) + std::fabs(center_x(candidate->frame) - x) * 0.01F;
            if (sign > 0.0F ? cy < extreme : cy > extreme)
            {
                extreme = cy;
                best = id;
            }
        }
    }
    if (best == 0U)
        return false;
    surface.pointer_moved = false;
    set_focus(store, surface, best, out);
    return true;
}

bool route_pointer_move(WidgetStore &store, Surface &surface, float x, float y, std::vector<Dispatch> &out)
{
    if (surface.dragging)
        if (Widget *slider = store.find(surface.pressed); slider != nullptr && slider->kind == WidgetKind::slider)
        {
            slider_to(*slider, x, out);
            return true;
        }
    const std::uint64_t over = store.hit(surface.root, x, y).value_or(0U);
    if (over != surface.hovered)
    {
        if (Widget *old = store.find(surface.hovered))
            emit(out, *old, old->on_hover, "hover", Value{false});
        surface.hovered = over;
        if (Widget *next = store.find(over))
            emit(out, *next, next->on_hover, "hover", Value{true});
        // The pointer coming to rest on a control focuses it, as it selects a
        // row of the game's menus - but never away from a field being typed in.
        if (const Widget *next = store.find(over); next != nullptr && takes_focus(*next) && !surface.editing)
        {
            surface.pointer_moved = true;
            if (surface.focused != over)
            {
                if (Widget *old = store.find(surface.focused))
                    emit(out, *old, old->on_focus, "focus", Value{false});
                surface.focused = over;
                emit(out, *store.find(over), store.find(over)->on_focus, "focus", Value{true});
            }
        }
    }
    return over != 0U;
}

bool route_pointer_button(WidgetStore &store, Surface &surface, float x, float y, bool down,
                          std::vector<Dispatch> &out)
{
    const std::uint64_t over = store.hit(surface.root, x, y).value_or(0U);
    Widget *target = store.find(over);
    if (down)
    {
        surface.pressed = over;
        if (target == nullptr)
        {
            surface.editing = false;
            return false;
        }
        if (takes_focus(*target) && surface.focused != over)
        {
            surface.pointer_moved = true;
            set_focus(store, surface, over, out);
        }
        if (target->kind == WidgetKind::slider && target->enabled)
        {
            surface.dragging = true;
            slider_to(*target, x, out);
        }
        else if (target->kind == WidgetKind::text_input && target->enabled)
            surface.editing = true;
        else if (surface.focused != over)
            surface.editing = false;
        return true;
    }
    const bool was_pressed = surface.pressed == over && over != 0U;
    const bool dragging = surface.dragging;
    surface.pressed = 0U;
    surface.dragging = false;
    if (dragging)
        return true;
    if (was_pressed && target != nullptr && target->kind != WidgetKind::slider && target->kind != WidgetKind::text_input)
        activate(*target, surface, out);
    return target != nullptr;
}

bool route_wheel(WidgetStore &store, Surface &surface, float x, float y, float steps)
{
    // The innermost list under the pointer scrolls.
    std::uint64_t best = 0U;
    std::vector<std::uint64_t> pending{surface.root};
    while (!pending.empty())
    {
        const std::uint64_t next = pending.back();
        pending.pop_back();
        const Widget *widget = store.find(next);
        if (widget == nullptr || !widget->visible || !widget->clip.contains(x, y))
            continue;
        if (widget->kind == WidgetKind::list)
            best = next;
        pending.insert(pending.end(), widget->children.begin(), widget->children.end());
    }
    Widget *list = store.find(best);
    if (list == nullptr || !std::isfinite(steps))
        return false;
    list->scroll = std::max(0.0F, list->scroll - std::clamp(steps, -20.0F, 20.0F) * 40.0F);
    return true;
}

bool route_key(WidgetStore &store, Surface &surface, NavKey key, std::vector<Dispatch> &out)
{
    Widget *focused = store.find(surface.focused);
    if (focused != nullptr && surface.editing && focused->kind == WidgetKind::text_input)
    {
        switch (key)
        {
        case NavKey::backspace:
            if (!focused->input.empty())
            {
                erase_last_code_point(focused->input);
                emit(out, *focused, focused->on_change, "change", Value{focused->input});
            }
            return true;
        case NavKey::delete_key:
            if (!focused->input.empty())
            {
                focused->input.clear();
                emit(out, *focused, focused->on_change, "change", Value{focused->input});
            }
            return true;
        case NavKey::activate:
            surface.editing = false;
            emit(out, *focused, focused->on_submit, "submit", Value{focused->input});
            return true;
        case NavKey::back:
            surface.editing = false;
            return true;
        case NavKey::up:
        case NavKey::down:
        case NavKey::next:
        case NavKey::previous:
            surface.editing = false;
            return move_focus(store, surface, key, out);
        default:
            return true;
        }
    }
    switch (key)
    {
    case NavKey::left:
    case NavKey::right:
        if (focused != nullptr && focused->kind == WidgetKind::slider && focused->enabled)
        {
            const double step = focused->step > 0.0 ? focused->step : (focused->maximum - focused->minimum) / 20.0;
            const double next = snap(*focused, focused->value + (key == NavKey::right ? step : -step));
            if (next != focused->value)
            {
                focused->value = next;
                emit(out, *focused, focused->on_change, "change", Value{next});
            }
            return true;
        }
        return move_focus(store, surface, key, out);
    case NavKey::up:
    case NavKey::down:
    case NavKey::next:
    case NavKey::previous:
        return move_focus(store, surface, key, out);
    case NavKey::activate:
        if (focused == nullptr || !takes_focus(*focused))
            return false;
        activate(*focused, surface, out);
        return true;
    default:
        return false;
    }
}

bool route_text(WidgetStore &store, Surface &surface, std::string_view text, std::vector<Dispatch> &out)
{
    Widget *focused = store.find(surface.focused);
    if (focused == nullptr || !surface.editing || focused->kind != WidgetKind::text_input || !focused->enabled)
        return false;
    std::string accepted;
    for (std::size_t at = 0U; at < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[at]);
        std::size_t length = lead < 0x80U ? 1U : (lead & 0xE0U) == 0xC0U ? 2U : (lead & 0xF0U) == 0xE0U ? 3U
                                                : (lead & 0xF8U) == 0xF0U ? 4U : 0U;
        if (length == 0U || at + length > text.size())
            break;
        if (lead >= 0x20U && lead != 0x7FU)
        {
            if (code_points(focused->input) + code_points(accepted) + 1U > focused->max_length ||
                focused->input.size() + accepted.size() + length > WidgetStore::max_text_bytes)
                break;
            accepted.append(text.substr(at, length));
        }
        at += length;
    }
    if (accepted.empty())
        return true;
    focused->input += accepted;
    emit(out, *focused, focused->on_change, "change", Value{focused->input});
    return true;
}

} // namespace openminerz::modding::ui
