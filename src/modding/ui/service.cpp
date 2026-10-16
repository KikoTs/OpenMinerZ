// The interface service: screens and dialogs, the mod HUD, notices, story
// screens, menu rows, settings and bindings pages, and the "omz.ui" channel
// (openminerz/modding/ui.hpp).

#include "openminerz/modding/ui.hpp"
#include "service_types.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::ui
{

namespace
{

[[nodiscard]] Value field(const Value &value, std::string_view name)
{
    const Value *found = value.field(name);
    return found != nullptr ? *found : Value{};
}

[[nodiscard]] Value make(std::initializer_list<std::pair<std::string, Value>> fields)
{
    return Value{Map(fields)};
}

[[nodiscard]] const Array &array_of(const Value &value, std::string_view what)
{
    static const Array empty;
    if (value.is_nil() || (value.type() == ValueType::map && value.as_map().empty()))
        return empty;
    if (value.type() != ValueType::array)
        throw ScriptError{std::format("{} must be an array", what)};
    return value.as_array();
}

[[nodiscard]] std::string text_field(const Value &value, std::string_view name, std::size_t limit, bool required)
{
    const Value *found = value.field(name);
    if (found == nullptr || found->is_nil())
    {
        if (required)
            throw ScriptError{std::format("'{}' is required", name)};
        return {};
    }
    if (found->type() != ValueType::string)
        throw ScriptError{std::format("'{}' must be a string", name)};
    if (found->as_string().size() > limit)
        throw ScriptError{std::format("'{}' is longer than {} bytes", name, limit)};
    return found->as_string();
}

[[nodiscard]] std::optional<double> number_field(const Value &value, std::string_view name, double low, double high)
{
    const Value *found = value.field(name);
    if (found == nullptr || found->is_nil())
        return std::nullopt;
    if (!found->is_number())
        throw ScriptError{std::format("'{}' must be a number", name)};
    const double number = found->as_number();
    if (!std::isfinite(number) || number < low || number > high)
        throw ScriptError{std::format("'{}' must be {}..{}", name, low, high)};
    return number;
}

[[nodiscard]] bool bool_field(const Value &value, std::string_view name, bool fallback)
{
    const Value *found = value.field(name);
    if (found == nullptr || found->is_nil())
        return fallback;
    if (found->type() != ValueType::boolean)
        throw ScriptError{std::format("'{}' must be true or false", name)};
    return found->as_bool();
}

// Every callback anywhere inside `value` (a spec a script handed over).
void collect_callbacks(const Value &value, std::vector<Callback> &out, std::size_t depth = 0U)
{
    if (depth > 64U)
        return;
    switch (value.type())
    {
    case ValueType::callback:
        out.push_back(value.as_callback());
        break;
    case ValueType::array:
        for (const Value &item : value.as_array())
            collect_callbacks(item, out, depth + 1U);
        break;
    case ValueType::map:
        for (const auto &[key, item] : value.as_map())
            collect_callbacks(item, out, depth + 1U);
        break;
    default:
        break;
    }
}

// The plain words of a server message's text: markup is allowed, sizes are
// bounded.
[[nodiscard]] std::string bounded(const Value &message, std::string_view name, std::size_t limit)
{
    const Value *found = message.field(name);
    if (found == nullptr || found->type() != ValueType::string)
        return {};
    std::string text = found->as_string();
    if (text.size() > limit)
    {
        text.resize(limit);
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U)
            text.pop_back();
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xC0U)
            text.pop_back();
    }
    return text;
}

// Where the objective tracker starts, as a share of the safe height: under the
// Remastered clock, notice and the toasts above it.
constexpr float tracker_top = 0.44F;

[[nodiscard]] bool valid_id(std::string_view id) noexcept
{
    return !id.empty() && id.size() <= max_id_bytes;
}

} // namespace

UiService::UiService(UiBackend &backend) : backend_(backend)
{
    localization_.set_reader([this](std::string_view mod, std::string_view path) {
        return backend_.read_mod_text(mod, path, max_lang_bytes);
    });
    widgets_.set_checks({[this](const AssetRef &asset) { return backend_.asset_exists(asset); },
                         [this](int item) { return backend_.item_exists(item); }});
    bindings_.set_canonical([this](std::string_view key) { return backend_.canonical_key(key); });
    if (const std::optional<std::string> saved = backend_.read_local("mod-bindings.cfg"))
        bindings_.load(*saved);
}

UiService::~UiService() = default;

bool UiService::active() const noexcept
{
    return !screens_.empty() || !hud_.empty() || !toasts_.empty() || title_.has_value() || !objectives_.empty() ||
           !hud_stats_.empty() || !menu_items_.empty() || !bindings_.empty() || any_hud_hidden() || any_hud_moved() ||
           !boss_bars_.empty(); // (mod-ai)
}

bool UiService::pauses_world() const noexcept
{
    return std::ranges::any_of(screens_, [](const auto &screen) { return screen->pause; });
}

bool UiService::any_hud_hidden() const noexcept
{
    return std::ranges::any_of(hud_hidden_, [](bool hidden) { return hidden; });
}

bool UiService::any_hud_moved() const noexcept
{
    return std::ranges::any_of(hud_offsets_, [](const auto &offset) { return offset.first != 0.0F || offset.second != 0.0F; });
}

bool UiService::editing_text() const noexcept
{
    const Surface *surface = top_surface();
    return surface != nullptr && surface->editing;
}

const Surface *UiService::top_surface() const noexcept
{
    return screens_.empty() ? nullptr : &screens_.back()->surface;
}

UiService::Screen *UiService::top_screen() noexcept
{
    return screens_.empty() ? nullptr : screens_.back().get();
}

std::vector<std::uint64_t> UiService::screen_roots() const
{
    std::vector<std::uint64_t> roots;
    for (const auto &screen : screens_)
        roots.push_back(screen->content);
    return roots;
}

std::vector<std::uint64_t> UiService::hud_roots() const
{
    std::vector<std::uint64_t> roots;
    for (const auto &item : hud_)
        roots.push_back(item->root);
    return roots;
}

void UiService::release(std::vector<Callback> &callbacks) noexcept
{
    if (host_ != nullptr)
        for (const Callback &callback : callbacks)
            if (callback.valid())
                host_->release(callback);
    callbacks.clear();
}

void UiService::release(Callback callback) noexcept
{
    if (host_ != nullptr && callback.valid())
        host_->release(callback);
}

void UiService::destroy_tree(std::uint64_t root)
{
    std::vector<Callback> released;
    widgets_.destroy(root, released);
    release(released);
}

// ---- Screens -------------------------------------------------------------------

std::uint64_t UiService::open_screen(const Value &spec, const Owner &owner, bool dialog)
{
    if (spec.type() != ValueType::map)
        throw ScriptError{"ui.open_screen takes a table"};
    if (screens_.size() >= 16U)
        throw ScriptError{"at most 16 screens can be open"};
    std::vector<Callback> handed;
    collect_callbacks(spec, handed);
    std::vector<Callback> adopted;
    auto screen = std::make_unique<Screen>();
    screen->mod = owner.mod;
    screen->side = owner.side;
    screen->dialog = dialog;
    try
    {
        // The content: `root` (one widget), or the spec's own children.
        Value content = field(spec, "root");
        if (content.is_nil())
            content = make({{"kind", Value{"stack"}}, {"gap", Value{10}}, {"stretch", Value{true}},
                            {"children", field(spec, "children")}});
        const std::string title = text_field(spec, "title", 512U, false);
        screen->pause = bool_field(spec, "pause", !dialog);
        screen->dim = bool_field(spec, "dim", true);
        screen->closable = bool_field(spec, "closable", true);
        if (const std::optional<double> width = number_field(spec, "max_width", 100.0, 20000.0))
            screen->max_width = static_cast<float>(*width);
        Array frame_children;
        if (!title.empty())
            frame_children.push_back(make({{"kind", Value{"label"}}, {"text", Value{title}}, {"face", Value{"display"}},
                                           {"size", Value{18}}, {"color", Value{"menu.heading"}},
                                           {"wrap", Value{false}}}));
        // A full screen fills the margins; a dialog fits its content. Either
        // takes the spec's own width and height when it gives them.
        const Value *width = spec.field("width");
        const Value *height = spec.field("height");
        Map fields{{"kind", Value{"stack"}},
                   {"background", Value{"panel"}},
                   {"padding", Value{dialog ? 20 : 24}},
                   {"gap", Value{12}},
                   {"stretch", Value{true}},
                   {"anchor", Value{"center"}},
                   {"children", Value{std::move(frame_children)}}};
        if (width != nullptr && !width->is_nil())
            fields.emplace_back("width", *width);
        else if (!dialog)
            fields.emplace_back("width", Value{"fill"});
        if (height != nullptr && !height->is_nil())
            fields.emplace_back("height", *height);
        else if (!dialog)
            fields.emplace_back("height", Value{"fill"});
        screen->root = widgets_.build(Value{std::move(fields)}, owner, adopted);
        try
        {
            screen->content = widgets_.add_child(screen->root, content, adopted);
        }
        catch (...)
        {
            std::vector<Callback> ignored;
            widgets_.destroy(screen->root, ignored);
            throw;
        }
        if (const Value *close = spec.field("on_close"); close != nullptr && close->type() == ValueType::callback)
        {
            screen->on_close = close->as_callback();
            adopted.push_back(screen->on_close);
        }
        if (const Value *back = spec.field("on_back"); back != nullptr && back->type() == ValueType::callback)
        {
            screen->on_back = back->as_callback();
            adopted.push_back(screen->on_back);
        }
    }
    catch (...)
    {
        release(handed);
        throw;
    }
    // Hand back what the screen did not keep.
    for (const Callback &callback : handed)
        if (std::ranges::find(adopted, callback) == adopted.end())
            release(callback);
    screen->surface.root = screen->root;
    const std::uint64_t content = screen->content;
    screens_.push_back(std::move(screen));
    // The first control starts focused, as a row of the game's menus always
    // is, so the keys and the pad work at once.
    std::vector<Dispatch> ignored;
    move_focus(widgets_, screens_.back()->surface, NavKey::down, ignored);
    backend_.play(SoundKey::popup);
    return content;
}

void UiService::close_screen(std::uint64_t root, bool notify)
{
    const auto found = std::ranges::find_if(screens_, [root](const auto &screen) {
        return screen->content == root || screen->root == root;
    });
    if (found == screens_.end())
        return;
    std::unique_ptr<Screen> screen = std::move(*found);
    screens_.erase(found);
    if (screen->builtin == Screen::Builtin::dialogue)
    {
        if (native_session_ != 0U)
        {
            const std::function<void()> closed = std::move(native_closed_);
            native_session_ = 0U;
            native_choose_ = {};
            native_closed_ = {};
            if (notify && closed)
                closed();
        }
        if (notify && dialogue_from_server_ && !dialogue_id_.empty())
            to_server(make({{"op", Value{"dialogue_closed"}}, {"id", Value{dialogue_id_}}}));
        dialogue_id_.clear();
        dialogue_root_ = 0U;
    }
    if (screen->builtin == Screen::Builtin::journal)
        journal_root_ = 0U;
    if (screen->builtin == Screen::Builtin::bindings)
    {
        bindings_root_ = 0U;
        capture_action_.clear();
    }
    if (screen->builtin == Screen::Builtin::config)
    {
        config_root_ = 0U;
        config_mod_.clear();
    }
    if (notify && screen->on_close.valid() && host_ != nullptr)
    {
        const std::array<Value, 1> args{Value{widget_handle(screen->content)}};
        (void)host_->invoke(screen->on_close, args);
    }
    if (notify && host_ != nullptr && screen->builtin == Screen::Builtin::none)
        (void)host_->emit(screen->side, "ui.screen_closed", make({{"screen", Value{widget_handle(screen->content)}}}));
    destroy_tree(screen->root);
    release(screen->on_close);
    release(screen->on_back);
    release(screen->choice_callbacks);
    release(screen->on_choice);
}

void UiService::layout_screen(Screen &screen, const Canvas &canvas, const WidgetStore::LayoutContext &context)
{
    Rect area = canvas.safe;
    // A full screen keeps reading distances on an ultrawide window, as the
    // menus do: it is at most `max_width` units wide, centred.
    const float margin = screen.dialog ? 24.0F : 16.0F;
    area = {area.x + margin, area.y + margin, std::max(0.0F, area.width - margin * 2.0F),
            std::max(0.0F, area.height - margin * 2.0F)};
    if (area.width > screen.max_width)
    {
        area.x += (area.width - screen.max_width) * 0.5F;
        area.width = screen.max_width;
    }
    if (screen.builtin == Screen::Builtin::dialogue)
    {
        // A story dialogue stands at the foot of the picture, over the HUD.
        const float width = std::min(area.width, 900.0F);
        area = {area.x + (area.width - width) * 0.5F, area.y, width, area.height};
    }
    widgets_.layout(screen.root, area, context);
}

void UiService::draw_screens(const FrameContext &frame, const TextMeasure &measure, DrawList &out)
{
    if (screens_.empty())
        return;
    const Canvas canvas = make_canvas(frame);
    WidgetStore::LayoutContext context;
    context.canvas = &canvas;
    context.measure = &measure;
    context.frame = &frame;
    context.stat = [this](std::string_view name) { return stat_value(name); };
    for (std::size_t index = 0U; index < screens_.size(); ++index)
    {
        Screen &screen = *screens_[index];
        layout_screen(screen, canvas, context);
        const bool top = index + 1U == screens_.size();
        if (screen.dim && screen.builtin != Screen::Builtin::dialogue)
        {
            DrawCmd wash;
            wash.kind = DrawCmd::Kind::fill;
            wash.rect = {0.0F, 0.0F, static_cast<float>(frame.width), static_cast<float>(frame.height)};
            wash.color = {0U, 0U, 0U, 150U};
            out.push_back(std::move(wash));
        }
        widgets_.draw(screen.root, context, top ? screen.surface.focused : 0U, top ? screen.surface.hovered : 0U,
                      top && screen.surface.editing, out);
    }
}

// ---- HUD -------------------------------------------------------------------------

std::uint64_t UiService::add_hud(const Value &spec, const Owner &owner)
{
    if (spec.type() != ValueType::map)
        throw ScriptError{"ui.add_hud takes a widget table"};
    if (hud_.size() >= 64U)
        throw ScriptError{"at most 64 HUD elements"};
    std::vector<Callback> handed;
    collect_callbacks(spec, handed);
    std::vector<Callback> adopted;
    // HUD elements never take the pointer: they are drawn over the world while
    // the mouse turns the camera. `frame` says what they anchor to.
    Map fields = spec.as_map();
    bool focus_frame = false;
    bool game_frame = false; // (wave 5, survival)
    if (const auto found = std::ranges::find_if(fields, [](const auto &entry) { return entry.first == "frame"; });
        found != fields.end())
    {
        if (found->second.type() != ValueType::string ||
            (found->second.as_string() != "screen" && found->second.as_string() != "focus" &&
             found->second.as_string() != "game"))
        {
            release(handed);
            throw ScriptError{"a HUD element's frame is \"screen\", \"focus\" or \"game\""};
        }
        focus_frame = found->second.as_string() == "focus";
        game_frame = found->second.as_string() == "game";
        fields.erase(found);
    }
    std::uint64_t root{};
    try
    {
        root = widgets_.build(Value{std::move(fields)}, owner, adopted);
    }
    catch (...)
    {
        release(handed);
        throw;
    }
    for (const Callback &callback : handed)
        if (std::ranges::find(adopted, callback) == adopted.end())
            release(callback);
    auto item = std::make_unique<HudItem>();
    item->root = root;
    item->mod = owner.mod;
    item->side = owner.side;
    item->focus_frame = focus_frame;
    item->game_frame = game_frame;
    hud_.push_back(std::move(item));
    return root;
}

std::optional<std::pair<double, double>> UiService::stat_value(std::string_view name) const
{
    if (stats_ != nullptr)
        if (const std::optional<StatValue> value = stats_->stat(name))
            return std::pair{value->value, value->maximum};
    if (const auto found = local_stats_.find(name); found != local_stats_.end())
        return std::pair{found->second.value, found->second.maximum};
    return std::nullopt;
}

void UiService::draw_hud(const FrameContext &frame, const TextMeasure &measure, DrawList &out)
{
    if (hud_.empty() && objectives_.empty() && hud_stats_.empty() && boss_bars_.empty()) // (mod-ai) bosses
        return;
    const Canvas canvas = make_canvas(frame);
    WidgetStore::LayoutContext context;
    context.canvas = &canvas;
    context.measure = &measure;
    context.frame = &frame;
    context.stat = [this](std::string_view name) { return stat_value(name); };
    // The breathing space the Remastered HUD keeps from the margins.
    constexpr float edge = 10.0F;
    for (const auto &item : hud_)
    {
        // (Wave 5, survival) frame = "game": the whole window without the
        // screen margins, where the Classic and Modern HUDs are drawn, so an
        // element can line up with the game's own hearts, tray or gauges.
        if (item->game_frame)
        {
            widgets_.layout(item->root, {0.0F, 0.0F, canvas.width, canvas.height}, context);
            widgets_.draw(item->root, context, 0U, 0U, false, out);
            continue;
        }
        const Rect base = item->focus_frame ? canvas.focus : canvas.safe;
        const Rect area{base.x + edge, base.y + edge, std::max(0.0F, base.width - edge * 2.0F),
                        std::max(0.0F, base.height - edge * 2.0F)};
        widgets_.layout(item->root, area, context);
        widgets_.draw(item->root, context, 0U, 0U, false, out);
    }
    // The Gameplay service's `hud` stats: bars stacked up from the bottom-left
    // corner, in the band beside the tray (the chat feed starts above it).
    if (!hud_stats_.empty())
    {
        const float scale = canvas.scale;
        const float cap = 9.0F * scale * frame.text_factor;
        const float bar_width = 150.0F * scale;
        const float row = 20.0F * scale;
        float bottom = (canvas.safe.y + canvas.safe.height - edge) * scale;
        const float left = (canvas.safe.x + edge) * scale;
        for (std::size_t index = 0U; index < hud_stats_.size() && index < 8U; ++index)
        {
            const HudStat &stat = hud_stats_[index];
            const float top = bottom - row;
            const float label_width = 70.0F * scale;
            DrawCmd plate;
            plate.kind = DrawCmd::Kind::fill;
            plate.rect = {left, top, label_width + bar_width + 12.0F * scale, row - 3.0F * scale};
            plate.color = frame.palette[static_cast<std::size_t>(ColorKey::hud_plate)];
            out.push_back(plate);
            const std::vector<TextRun> label{TextRun{stat.label.empty() ? stat.name : stat.label}};
            const TextBlock block = layout_text(label, Face::text, cap,
                                                frame.palette[static_cast<std::size_t>(ColorKey::menu_text)],
                                                label_width - 8.0F * scale, 1U, Align::left, measure);
            for (const PlacedText &piece : block.pieces)
            {
                DrawCmd text;
                text.kind = DrawCmd::Kind::text;
                text.rect = {std::round(left + 6.0F * scale + piece.x),
                             std::round(top + (plate.rect.height - block.height) * 0.5F + piece.top), 0.0F, piece.cap};
                text.text = piece.text;
                text.face = piece.face;
                text.cap = piece.cap;
                text.color = piece.color;
                out.push_back(std::move(text));
            }
            const double range = stat.maximum - stat.minimum;
            const float share = range > 0.0 ? static_cast<float>(std::clamp((stat.value - stat.minimum) / range, 0.0, 1.0))
                                            : 0.0F;
            DrawCmd track;
            track.kind = DrawCmd::Kind::fill;
            track.rect = {left + label_width, top + (plate.rect.height - 8.0F * scale) * 0.5F, bar_width, 8.0F * scale};
            track.color = {46U, 48U, 50U, 235U};
            out.push_back(track);
            track.rect.width = bar_width * share;
            track.color = stat.color.value_or(frame.palette[static_cast<std::size_t>(ColorKey::menu_accent)]);
            out.push_back(track);
            bottom = top;
        }
    }
    // (mod-ai) Boss bars: top centre, under the compass and clock row, the
    // name (and phase) above a wide bar.
    if (!boss_bars_.empty())
    {
        const float scale = canvas.scale;
        const float cap = 11.0F * scale * frame.text_factor;
        const float width = 440.0F * scale;
        const float bar = 12.0F * scale;
        const float row = 38.0F * scale;
        const float left = (canvas.safe.x + canvas.safe.width * 0.5F) * scale - width * 0.5F;
        float top = (canvas.safe.y + 64.0F) * scale;
        for (std::size_t index = 0U; index < boss_bars_.size() && index < 3U; ++index)
        {
            const BossBar &boss = boss_bars_[index];
            DrawCmd plate;
            plate.kind = DrawCmd::Kind::fill;
            plate.rect = {left - 8.0F * scale, top - 4.0F * scale, width + 16.0F * scale, row};
            plate.color = frame.palette[static_cast<std::size_t>(ColorKey::hud_plate)];
            out.push_back(plate);
            const std::string caption = boss.phase.empty() ? boss.label : boss.label + "  -  " + boss.phase;
            const std::vector<TextRun> label{TextRun{caption}};
            const TextBlock block = layout_text(label, Face::text, cap,
                                                frame.palette[static_cast<std::size_t>(ColorKey::menu_text)], width, 1U,
                                                Align::center, measure);
            for (const PlacedText &piece : block.pieces)
            {
                DrawCmd text;
                text.kind = DrawCmd::Kind::text;
                text.rect = {std::round(left + piece.x), std::round(top + piece.top), 0.0F, piece.cap};
                text.text = piece.text;
                text.face = piece.face;
                text.cap = piece.cap;
                text.color = piece.color;
                out.push_back(std::move(text));
            }
            DrawCmd track;
            track.kind = DrawCmd::Kind::fill;
            track.rect = {left, top + block.height + 4.0F * scale, width, bar};
            track.color = {30U, 24U, 24U, 235U};
            out.push_back(track);
            track.rect.width = width * static_cast<float>(std::clamp(boss.fraction, 0.0, 1.0));
            track.color = boss.color.value_or(Color{200U, 60U, 50U, 255U});
            out.push_back(track);
            top += row + 6.0F * scale;
        }
    }
    if (objectives_.empty())
        return;
    // The objective tracker: right edge, under the clock and the notices.
    const float scale = canvas.scale;
    const float cap = 11.0F * scale * frame.text_factor;
    const float width = 260.0F * scale;
    const float pad = 10.0F * scale;
    const float x = (canvas.safe.x + canvas.safe.width - edge) * scale - width;
    float y = (canvas.safe.y + canvas.safe.height * tracker_top) * scale;
    std::vector<TextBlock> blocks;
    float height = pad * 2.0F + cap * 1.66F;
    for (const auto &objective : objectives_)
    {
        const Color color = objective->done ? frame.palette[static_cast<std::size_t>(ColorKey::menu_quiet)]
                                            : frame.palette[static_cast<std::size_t>(ColorKey::menu_text)];
        blocks.push_back(layout_text(objective->text, Face::text, cap, color, width - pad * 2.0F - 16.0F * scale, 3U,
                                     Align::left, measure));
        height += blocks.back().height + 8.0F * scale + (objective->progress ? 8.0F * scale : 0.0F);
    }
    DrawCmd plate;
    plate.kind = DrawCmd::Kind::fill;
    plate.rect = {x, y, width, height};
    plate.color = frame.palette[static_cast<std::size_t>(ColorKey::hud_plate)];
    out.push_back(plate);
    DrawCmd bar = plate;
    bar.rect = {x, y, std::max(1.0F, std::round(3.0F * scale)), height};
    bar.color = frame.palette[static_cast<std::size_t>(ColorKey::menu_accent)];
    out.push_back(bar);
    const std::vector<TextRun> heading{TextRun{"Objectives"}};
    const TextBlock title = layout_text(heading, Face::display, cap, frame.palette[static_cast<std::size_t>(ColorKey::menu_accent)],
                                        0.0F, 1U, Align::left, measure);
    for (const PlacedText &piece : title.pieces)
    {
        DrawCmd text;
        text.kind = DrawCmd::Kind::text;
        text.rect = {x + pad + piece.x, y + pad + piece.top, 0.0F, piece.cap};
        text.text = piece.text;
        text.face = piece.face;
        text.cap = piece.cap;
        text.color = piece.color;
        out.push_back(std::move(text));
    }
    y += pad + cap * 1.66F + 4.0F * scale;
    for (std::size_t index = 0U; index < objectives_.size(); ++index)
    {
        const Objective &objective = *objectives_[index];
        const float mark = std::round(8.0F * scale);
        DrawCmd box;
        box.kind = objective.done ? DrawCmd::Kind::fill : DrawCmd::Kind::frame;
        box.rect = {x + pad, y + (cap - mark) * 0.5F, mark, mark};
        box.color = objective.done ? frame.palette[static_cast<std::size_t>(ColorKey::hud_good)]
                                   : frame.palette[static_cast<std::size_t>(ColorKey::menu_text)];
        box.thickness = std::max(1.0F, std::round(scale));
        out.push_back(box);
        for (const PlacedText &piece : blocks[index].pieces)
        {
            DrawCmd text;
            text.kind = DrawCmd::Kind::text;
            text.rect = {x + pad + 16.0F * scale + piece.x, y + piece.top, 0.0F, piece.cap};
            text.text = piece.text;
            text.face = piece.face;
            text.cap = piece.cap;
            text.color = piece.color;
            out.push_back(std::move(text));
        }
        y += blocks[index].height + 4.0F * scale;
        if (objective.progress)
        {
            const float track_width = width - pad * 2.0F - 16.0F * scale;
            DrawCmd track;
            track.kind = DrawCmd::Kind::fill;
            track.rect = {x + pad + 16.0F * scale, y, track_width, std::max(2.0F, std::round(4.0F * scale))};
            track.color = {255U, 255U, 255U, 40U};
            out.push_back(track);
            track.rect.width = track_width * static_cast<float>(std::clamp(*objective.progress, 0.0, 1.0));
            track.color = frame.palette[static_cast<std::size_t>(ColorKey::menu_accent)];
            out.push_back(track);
            y += 8.0F * scale;
        }
        y += 4.0F * scale;
    }
}

// ---- Notices: toasts and titles ----------------------------------------------

void UiService::show_toast(const Value &spec, const std::string &mod)
{
    (void)mod;
    auto toast = std::make_unique<Toast>();
    toast->title = runs_from_value(field(spec, "title"));
    toast->text = runs_from_value(field(spec, "text"));
    if (plain_text(toast->title).size() > max_message_text || plain_text(toast->text).size() > max_message_text)
        throw ScriptError{std::format("a toast's text is at most {} bytes", max_message_text)};
    if (toast->title.empty() && toast->text.empty())
        throw ScriptError{"a toast needs a title or text"};
    if (const std::optional<double> item = number_field(spec, "item", 0.0, 65535.0))
        toast->item = backend_.item_exists(static_cast<int>(*item)) ? static_cast<int>(*item) : -1;
    if (const std::optional<double> seconds = number_field(spec, "seconds", 0.5, 30.0))
        toast->seconds = *seconds;
    if (const Value *color = spec.field("color"); color != nullptr && !color->is_nil())
        toast->color = parse_color(*color);
    toasts_.push_back(std::move(toast));
    while (toasts_.size() > max_toasts)
        toasts_.pop_front();
}

void UiService::show_title(const Value &spec)
{
    TitleState title;
    title.title = runs_from_value(field(spec, "title"));
    title.subtitle = runs_from_value(field(spec, "subtitle"));
    if (plain_text(title.title).size() > 512U || plain_text(title.subtitle).size() > 1024U)
        throw ScriptError{"a title is at most 512 bytes, its subtitle 1024"};
    title.fade_in = number_field(spec, "fade_in", 0.0, 10.0).value_or(0.4);
    title.stay = number_field(spec, "stay", 0.0, 60.0).value_or(2.5);
    title.fade_out = number_field(spec, "fade_out", 0.0, 10.0).value_or(0.6);
    if (title.title.empty() && title.subtitle.empty())
        title_.reset();
    else
        title_ = std::move(title);
}

void UiService::draw_notices(const FrameContext &frame, const TextMeasure &measure, DrawList &out)
{
    if (toasts_.empty() && !title_)
        return;
    const Canvas canvas = make_canvas(frame);
    const float scale = canvas.scale;
    const auto ink = [&](ColorKey key) { return frame.palette[static_cast<std::size_t>(key)]; };
    const auto emit_block = [&](const TextBlock &block, float left, float top, float opacity, bool outline) {
        for (const PlacedText &piece : block.pieces)
        {
            DrawCmd text;
            text.kind = DrawCmd::Kind::text;
            text.rect = {std::round(left + piece.x), std::round(top + piece.top), 0.0F, piece.cap};
            text.text = piece.text;
            text.face = piece.face;
            text.cap = piece.cap;
            text.color = scale_alpha(piece.color, opacity);
            text.outline = outline;
            out.push_back(std::move(text));
        }
    };
    // Toasts: the Remastered notice's look, stacked down the right edge under
    // the game's own notice, newest first, as many as fit above the
    // objective tracker.
    float y = (canvas.safe.y + 150.0F) * scale;
    const float limit =
        (canvas.safe.y + canvas.safe.height * (objectives_.empty() ? 0.80F : tracker_top) - 6.0F) * scale;
    const float width = 300.0F * scale;
    const float right = (canvas.safe.x + canvas.safe.width - 10.0F) * scale;
    const float pad = 10.0F * scale;
    for (auto entry = toasts_.rbegin(); entry != toasts_.rend(); ++entry)
    {
        const auto &toast = *entry;
        const double remaining = toast->seconds - toast->age;
        float opacity = static_cast<float>(std::clamp(std::min(toast->age * 5.0, remaining * 4.0), 0.0, 1.0));
        opacity = opacity * opacity * (3.0F - 2.0F * opacity);
        const float slide = frame.reduced_motion ? 0.0F : (1.0F - opacity) * 14.0F * scale;
        const float icon = toast->item >= 0 ? 32.0F * scale : 0.0F;
        const float inner = width - pad * 2.0F - 3.0F * scale - (icon > 0.0F ? icon + pad : 0.0F);
        const float title_cap = 11.0F * scale * frame.text_factor;
        const TextBlock title = layout_text(toast->title, Face::display, title_cap, ink(ColorKey::menu_text), inner, 1U,
                                            Align::left, measure);
        const TextBlock body = layout_text(toast->text, Face::text, title_cap, ink(ColorKey::menu_quiet), inner, 3U,
                                           Align::left, measure);
        const float content = title.height + (body.lines > 0U ? 8.0F * scale + body.height : 0.0F);
        const float height = std::max(content, icon) + pad * 2.0F;
        if (y + height > limit)
            break;
        const Rect box{right - width + slide, y, width, height};
        DrawCmd plate;
        plate.kind = DrawCmd::Kind::fill;
        plate.rect = box;
        plate.color = scale_alpha(ink(ColorKey::hud_plate), opacity * 0.85F);
        out.push_back(plate);
        plate.rect = {box.x, box.y, std::max(1.0F, std::round(3.0F * scale)), box.height};
        plate.color = scale_alpha(toast->color.value_or(ink(ColorKey::menu_accent)), opacity);
        out.push_back(plate);
        float left = box.x + pad + 3.0F * scale;
        if (icon > 0.0F)
        {
            DrawCmd item;
            item.kind = DrawCmd::Kind::item;
            item.rect = {left, box.y + (box.height - icon) * 0.5F, icon, icon};
            item.item = toast->item;
            item.color = scale_alpha(Color{}, opacity);
            out.push_back(item);
            left += icon + pad;
        }
        emit_block(title, left, box.y + pad, opacity, false);
        emit_block(body, left, box.y + pad + title.height + 8.0F * scale, opacity, false);
        y += height + 6.0F * scale;
    }
    if (title_)
    {
        const TitleState &state = *title_;
        const double age = state.age;
        float opacity = 1.0F;
        if (age < state.fade_in)
            opacity = static_cast<float>(age / std::max(state.fade_in, 1e-3));
        else if (age > state.fade_in + state.stay)
            opacity = static_cast<float>(1.0 - (age - state.fade_in - state.stay) / std::max(state.fade_out, 1e-3));
        opacity = std::clamp(opacity, 0.0F, 1.0F);
        const float room = canvas.safe.width * scale * 0.8F;
        const float center = (canvas.safe.x + canvas.safe.width * 0.5F) * scale;
        float top = (canvas.safe.y + canvas.safe.height * 0.34F) * scale;
        const TextBlock title = layout_text(state.title, Face::display, 36.0F * scale * frame.text_factor,
                                            ink(ColorKey::menu_heading), room, 2U, Align::center, measure);
        emit_block(title, center - room * 0.5F, top, opacity, true);
        top += title.height + (title.lines > 0U ? 14.0F * scale : 0.0F);
        const TextBlock subtitle = layout_text(state.subtitle, Face::text, 18.0F * scale * frame.text_factor,
                                               ink(ColorKey::menu_text), room, 3U, Align::center, measure);
        emit_block(subtitle, center - room * 0.5F, top, opacity, true);
    }
}

void UiService::set_objective(const Value &spec, const std::string &mod, Side side)
{
    const std::string id = text_field(spec, "id", max_id_bytes, true);
    std::vector<TextRun> text = runs_from_value(field(spec, "text"));
    if (plain_text(text).size() > 1024U)
        throw ScriptError{"an objective's text is at most 1024 bytes"};
    const std::optional<double> progress = number_field(spec, "progress", 0.0, 1.0);
    const bool done = bool_field(spec, "done", false);
    const bool server = side == Side::server;
    auto found = std::ranges::find_if(objectives_, [&](const auto &objective) {
        return objective->id == id && objective->mod == mod && objective->from_server == server;
    });
    if (found == objectives_.end())
    {
        if (objectives_.size() >= max_objectives)
            throw ScriptError{std::format("at most {} objectives", max_objectives)};
        objectives_.push_back(std::make_unique<Objective>());
        found = objectives_.end() - 1;
    }
    (*found)->id = id;
    (*found)->mod = mod;
    (*found)->text = std::move(text);
    (*found)->progress = progress;
    (*found)->done = done;
    (*found)->from_server = server;
}

// ---- Story: dialogue and journal ----------------------------------------------

void UiService::open_dialogue(const Value &spec, const Owner &owner, bool from_server)
{
    const std::string id = text_field(spec, "id", max_id_bytes, false);
    const std::string speaker = text_field(spec, "speaker", 256U, false);
    const Value text = field(spec, "text");
    if (text.type() == ValueType::string && text.as_string().size() > max_message_text)
        throw ScriptError{std::format("dialogue text is at most {} bytes", max_message_text)};
    const Array &choices = array_of(field(spec, "choices"), "choices");
    if (choices.size() > max_dialogue_choices)
        throw ScriptError{std::format("a dialogue has at most {} choices", max_dialogue_choices)};
    std::vector<std::string> choice_ids;
    std::vector<Callback> choice_callbacks;
    Array buttons;
    for (std::size_t index = 0U; index < choices.size(); ++index)
    {
        const Value &choice = choices[index];
        std::string choice_id;
        std::string label;
        if (choice.type() == ValueType::string)
        {
            label = choice.as_string();
            choice_id = std::to_string(index + 1U);
        }
        else
        {
            label = text_field(choice, "text", 512U, true);
            choice_id = text_field(choice, "id", max_id_bytes, false);
            if (choice_id.empty())
                choice_id = std::to_string(index + 1U);
            if (!from_server)
                if (const Value *select = choice.field("on_select"); select != nullptr && select->type() == ValueType::callback)
                    choice_callbacks.push_back(select->as_callback());
        }
        if (label.size() > 512U)
            throw ScriptError{"a choice's text is at most 512 bytes"};
        choice_ids.push_back(choice_id);
        buttons.push_back(make({{"kind", Value{"button"}},
                                {"text", Value{std::format("{}.  {}", index + 1U, label)}},
                                {"align", Value{"left"}},
                                {"width", Value{"fill"}}}));
    }
    if (buttons.empty())
        buttons.push_back(make({{"kind", Value{"button"}}, {"text", Value{"Continue"}}, {"width", Value{"fill"}}}));
    // Close any dialogue already up: one conversation at a time.
    if (dialogue_root_ != 0U)
        close_screen(dialogue_root_, true);
    Array header;
    if (const std::optional<double> portrait = number_field(spec, "portrait_item", 0.0, 65535.0);
        portrait && backend_.item_exists(static_cast<int>(*portrait)))
        header.push_back(make({{"kind", Value{"item_slot"}}, {"item", Value{static_cast<int>(*portrait)}},
                               {"width", Value{48}}, {"height", Value{48}}}));
    if (!speaker.empty())
        header.push_back(make({{"kind", Value{"label"}}, {"text", Value{speaker}}, {"face", Value{"display"}},
                               {"size", Value{18}}, {"color", Value{"menu.heading"}}, {"wrap", Value{false}}}));
    Array body;
    if (!header.empty())
        body.push_back(make({{"kind", Value{"stack"}}, {"direction", Value{"horizontal"}}, {"gap", Value{12}},
                             {"align", Value{"center"}}, {"children", Value{std::move(header)}}}));
    if (!text.is_nil())
        body.push_back(make({{"kind", Value{"label"}}, {"text", text}, {"size", Value{13}}, {"width", Value{"fill"}}}));
    body.push_back(make({{"kind", Value{"stack"}}, {"gap", Value{6}}, {"stretch", Value{true}},
                         {"children", Value{std::move(buttons)}}}));
    // (The caller lets go of the spec's functions if this throws.)
    const std::uint64_t content =
        open_screen(make({{"root", make({{"kind", Value{"stack"}}, {"gap", Value{12}}, {"stretch", Value{true}},
                                         {"children", Value{std::move(body)}}})},
                          {"pause", Value{bool_field(spec, "pause", true)}},
                          {"width", Value{"fill"}},
                          {"height", Value{"auto"}},
                          {"max_width", Value{900}}}),
                    owner, true);
    Screen *screen = top_screen();
    screen->builtin = Screen::Builtin::dialogue;
    screen->choice_ids = std::move(choice_ids);
    screen->choice_callbacks = std::move(choice_callbacks);
    if (!from_server)
        if (const Value *on_choice = spec.field("on_choice"); on_choice != nullptr && on_choice->type() == ValueType::callback)
            screen->on_choice = on_choice->as_callback();
    // The frame sits at the foot of the screen.
    if (Widget *frame = widgets_.find(screen->root))
        frame->anchor = Anchor::bottom;
    // Each choice button runs the service's own action.
    if (const Widget *root = widgets_.find(content))
    {
        const std::uint64_t list = root->children.back();
        if (const Widget *stack = widgets_.find(list))
            for (std::size_t index = 0U; index < stack->children.size(); ++index)
                if (Widget *button = widgets_.find(stack->children[index]))
                    button->action = screen->choice_ids.empty()
                                         ? InternalAction{InternalAction::Kind::close_screen, 0, {}}
                                         : InternalAction{InternalAction::Kind::dialogue_choice,
                                                          static_cast<std::int64_t>(index), {}};
    }
    screen->closable = bool_field(spec, "closable", true);
    dialogue_id_ = id.empty() ? std::string{"dialogue"} : id;
    dialogue_from_server_ = from_server;
    dialogue_root_ = content;
    // (open_screen focused the first choice, so Enter / A answers at once.)
}

void UiService::show_native_dialogue(NativeDialogue dialogue)
{
    Array choices;
    for (const std::string &choice : dialogue.choices)
        choices.push_back(Value{choice});
    Map spec{{"id", Value{std::format("native:{}", dialogue.session)}},
             {"speaker", Value{dialogue.speaker}},
             {"text", Value{dialogue.text}},
             {"choices", Value{std::move(choices)}}};
    if (dialogue.portrait_item >= 0)
        spec.emplace_back("portrait_item", Value{dialogue.portrait_item});
    // Bounded like a server request: a malformed one is dropped whole.
    try
    {
        open_dialogue(Value{std::move(spec)}, Owner{"", Side::client}, false);
    }
    catch (const ScriptError &)
    {
        return;
    }
    native_session_ = dialogue.session;
    native_choose_ = std::move(dialogue.choose);
    native_closed_ = std::move(dialogue.closed);
}

void UiService::close_native_dialogue(std::uint64_t session)
{
    if (native_session_ != 0U && native_session_ == session && dialogue_root_ != 0U)
        close_screen(dialogue_root_, false);
}

std::uint64_t UiService::native_dialogue_session() const noexcept
{
    return native_session_;
}

void UiService::close_dialogue(std::string_view id)
{
    if (dialogue_root_ != 0U && (id.empty() || id == dialogue_id_))
        close_screen(dialogue_root_, false);
}

bool UiService::choice_key(int number)
{
    Screen *screen = top_screen();
    if (screen == nullptr || screen->builtin != Screen::Builtin::dialogue || number < 1 ||
        static_cast<std::size_t>(number) > screen->choice_ids.size())
        return false;
    Dispatch dispatch;
    dispatch.action = {InternalAction::Kind::dialogue_choice, number - 1, {}};
    std::vector<Dispatch> dispatches{std::move(dispatch)};
    this->dispatch(dispatches);
    return true;
}

void UiService::set_journal(const Value &quests)
{
    const Array &list = array_of(quests, "quests");
    if (list.size() > max_quests)
        throw ScriptError{std::format("at most {} quests", max_quests)};
    std::vector<std::unique_ptr<Quest>> next;
    for (const Value &entry : list)
    {
        auto quest = std::make_unique<Quest>();
        quest->id = text_field(entry, "id", max_id_bytes, true);
        quest->title = text_field(entry, "title", 256U, true);
        quest->text = text_field(entry, "text", max_message_text, false);
        const std::string state = text_field(entry, "state", 16U, false);
        if (!state.empty())
        {
            if (state != "active" && state != "done" && state != "failed")
                throw ScriptError{"a quest's state is \"active\", \"done\" or \"failed\""};
            quest->state = state;
        }
        const Array &steps = array_of(field(entry, "objectives"), "objectives");
        if (steps.size() > max_objectives)
            throw ScriptError{std::format("a quest has at most {} objectives", max_objectives)};
        for (const Value &step : steps)
        {
            Quest::Step item;
            item.text = step.type() == ValueType::string ? step.as_string() : text_field(step, "text", 1024U, true);
            if (item.text.size() > 1024U)
                throw ScriptError{"an objective's text is at most 1024 bytes"};
            if (step.type() == ValueType::map)
            {
                item.done = bool_field(step, "done", false);
                item.progress = number_field(step, "progress", 0.0, 1.0);
            }
            quest->steps.push_back(std::move(item));
        }
        next.push_back(std::move(quest));
    }
    quests_ = std::move(next);
    journal_selected_ = std::min(journal_selected_, quests_.empty() ? 0U : quests_.size() - 1U);
    if (journal_root_ != 0U)
        open_journal();
}

void UiService::open_journal()
{
    const bool was_open = journal_root_ != 0U;
    if (was_open)
        close_screen(journal_root_, false);
    Array rows;
    for (std::size_t index = 0U; index < quests_.size(); ++index)
    {
        const Quest &quest = *quests_[index];
        const std::string mark = quest.state == "done" ? "[c=#6EDC7C]Done[/c]  " : quest.state == "failed" ? "[c=#E8343A]Failed[/c]  " : "";
        rows.push_back(make({{"kind", Value{"button"}},
                             {"text", Value{mark + quest.title}},
                             {"align", Value{"left"}},
                             {"width", Value{"fill"}}}));
    }
    if (rows.empty())
        rows.push_back(make({{"kind", Value{"label"}}, {"text", Value{"No quests yet."}}, {"color", Value{"menu.quiet"}}}));
    Array detail;
    if (!quests_.empty())
    {
        const Quest &quest = *quests_[std::min(journal_selected_, quests_.size() - 1U)];
        detail.push_back(make({{"kind", Value{"label"}}, {"text", Value{quest.title}}, {"face", Value{"display"}},
                               {"size", Value{18}}, {"color", Value{"menu.heading"}}, {"width", Value{"fill"}}}));
        if (!quest.text.empty())
            detail.push_back(make({{"kind", Value{"label"}}, {"text", Value{quest.text}}, {"size", Value{12}},
                                   {"width", Value{"fill"}}}));
        for (const Quest::Step &step : quest.steps)
        {
            Array line;
            line.push_back(make({{"kind", Value{"label"}},
                                 {"text", Value{step.done ? std::string{"[c=#6EDC7C]+[/c]  "} + step.text : "-  " + step.text}},
                                 {"color", Value{step.done ? "menu.quiet" : "menu.text"}},
                                 {"width", Value{"fill"}}}));
            if (step.progress)
                line.push_back(make({{"kind", Value{"progress"}}, {"value", Value{*step.progress}},
                                     {"width", Value{"fill"}}, {"height", Value{6}}}));
            detail.push_back(make({{"kind", Value{"stack"}}, {"gap", Value{4}}, {"stretch", Value{true}},
                                   {"children", Value{std::move(line)}}}));
        }
    }
    Value root = make({{"kind", Value{"stack"}},
                       {"direction", Value{"horizontal"}},
                       {"gap", Value{20}},
                       {"height", Value{"fill"}},
                       {"children", Value{Array{make({{"kind", Value{"list"}},
                                                      {"width", Value{320}},
                                                      {"height", Value{"fill"}},
                                                      {"background", Value{"well"}},
                                                      {"padding", Value{6}},
                                                      {"stretch", Value{true}},
                                                      {"children", Value{std::move(rows)}}}),
                                                make({{"kind", Value{"list"}},
                                                      {"width", Value{"fill"}},
                                                      {"height", Value{"fill"}},
                                                      {"gap", Value{10}},
                                                      {"stretch", Value{true}},
                                                      {"children", Value{std::move(detail)}}})}}}});
    const std::uint64_t content = open_screen(make({{"title", Value{"Journal"}}, {"root", root}, {"max_width", Value{1100}}}),
                                              Owner{"", Side::client}, false);
    Screen *screen = top_screen();
    screen->builtin = Screen::Builtin::journal;
    journal_root_ = content;
    if (const Widget *panes = widgets_.find(content); panes != nullptr && !panes->children.empty())
        if (const Widget *list = widgets_.find(panes->children.front()))
            for (std::size_t index = 0U; index < list->children.size() && index < quests_.size(); ++index)
                if (Widget *row = widgets_.find(list->children[index]))
                    row->action = {InternalAction::Kind::journal_select, static_cast<std::int64_t>(index), {}};
    std::vector<Dispatch> ignored;
    const std::vector<std::uint64_t> focusable = widgets_.focusables(screen->root);
    if (journal_selected_ < focusable.size())
        set_focus(widgets_, screen->surface, focusable[journal_selected_], ignored);
}

// ---- Menus, settings pages, bindings --------------------------------------------

std::vector<MenuItem> UiService::menu_items(MenuSlot slot) const
{
    std::vector<MenuItem> items;
    for (const auto &item : menu_items_)
        if (item->slot == slot)
            items.push_back(*item);
    if (slot != MenuSlot::addons)
        return items;
    if (config_ != nullptr)
    {
        std::vector<std::string> mods;
        for (const ConfigSetting &setting : config_->settings())
            if (std::ranges::find(mods, setting.mod_id) == mods.end())
                mods.push_back(setting.mod_id);
        for (const std::string &mod : mods)
        {
            MenuItem item;
            item.slot = MenuSlot::addons;
            const std::string title = backend_.mod_title(mod);
            item.label = std::format("{} Settings", title.empty() ? mod : title);
            item.description = "Change this add-on's own settings.";
            item.mod = mod;
            item.config_mod = mod;
            items.push_back(std::move(item));
        }
    }
    if (!bindings_.empty())
    {
        MenuItem item;
        item.slot = MenuSlot::addons;
        item.label = "Add-On Key Bindings";
        item.description = "See and change the keys add-ons use. The game's own keys are never taken.";
        item.bindings = true;
        items.push_back(std::move(item));
    }
    return items;
}

void UiService::activate_menu_item(MenuSlot slot, std::size_t index)
{
    const std::vector<MenuItem> items = menu_items(slot);
    if (index >= items.size())
        return;
    const MenuItem &item = items[index];
    if (item.bindings)
        open_bindings_screen();
    else if (!item.config_mod.empty())
        open_config_screen(item.config_mod);
    else if (item.callback.valid())
    {
        Dispatch dispatch;
        dispatch.callback = item.callback;
        dispatch.event = "select";
        std::vector<Dispatch> dispatches{std::move(dispatch)};
        this->dispatch(dispatches);
    }
}

void UiService::open_bindings_screen()
{
    // Rebuilding the page keeps a capture that is waiting for its key.
    const std::string capture = capture_action_;
    if (bindings_root_ != 0U)
        close_screen(bindings_root_, false);
    capture_action_ = capture;
    Array rows;
    for (const KeyBinding &binding : bindings_.all())
    {
        const std::string title = backend_.mod_title(binding.mod);
        const std::string key = capture_action_ == binding.action ? std::string{"Press a key..."}
                                : binding.key.empty()               ? std::string{"None"}
                                                                    : binding.key;
        Array line{make({{"kind", Value{"stack"}},
                         {"gap", Value{2}},
                         {"width", Value{"fill"}},
                         {"children", Value{Array{make({{"kind", Value{"label"}}, {"text", Value{binding.label}},
                                                        {"wrap", Value{false}}}),
                                                  make({{"kind", Value{"label"}},
                                                        {"text", Value{binding.conflict.empty()
                                                                           ? (title.empty() ? binding.mod : title)
                                                                           : std::format("[c=#FFBD00]{}[/c]", binding.conflict)}},
                                                        {"color", Value{"menu.quiet"}},
                                                        {"size", Value{9}},
                                                        {"wrap", Value{false}}})}}}}),
                   make({{"kind", Value{"button"}}, {"text", Value{key}}, {"width", Value{160}}}),
                   make({{"kind", Value{"button"}}, {"text", Value{"Default"}}, {"width", Value{100}},
                         {"enabled", Value{binding.user_set}}})};
        rows.push_back(make({{"kind", Value{"stack"}},
                             {"direction", Value{"horizontal"}},
                             {"gap", Value{10}},
                             {"align", Value{"center"}},
                             {"width", Value{"fill"}},
                             {"children", Value{std::move(line)}}}));
    }
    if (rows.empty())
        rows.push_back(make({{"kind", Value{"label"}}, {"text", Value{"No add-on uses a key."}}, {"color", Value{"menu.quiet"}}}));
    rows.push_back(make({{"kind", Value{"label"}},
                         {"text", Value{"Choose a key, then press the new one. Esc cancels, Backspace clears it. "
                                        "Keys the game uses cannot be given to an add-on."}},
                         {"color", Value{"menu.quiet"}},
                         {"size", Value{10}},
                         {"width", Value{"fill"}}}));
    const std::uint64_t content = open_screen(
        make({{"title", Value{"Add-On Key Bindings"}},
              {"root", make({{"kind", Value{"list"}}, {"height", Value{"fill"}}, {"gap", Value{8}}, {"stretch", Value{true}},
                             {"children", Value{std::move(rows)}}})},
              {"max_width", Value{900}},
              {"pause", Value{true}}}),
        Owner{"", Side::client}, false);
    Screen *screen = top_screen();
    screen->builtin = Screen::Builtin::bindings;
    bindings_root_ = content;
    if (const Widget *list = widgets_.find(content))
        for (std::size_t index = 0U; index < list->children.size() && index < bindings_.all().size(); ++index)
            if (const Widget *row = widgets_.find(list->children[index]); row != nullptr && row->children.size() == 3U)
            {
                const std::string &action = bindings_.all()[index].action;
                if (Widget *assign = widgets_.find(row->children[1]))
                    assign->action = {InternalAction::Kind::binding_capture, 0, action};
                if (Widget *reset = widgets_.find(row->children[2]))
                    reset->action = {InternalAction::Kind::binding_reset, 0, action};
            }
}

void UiService::rebuild_bindings_screen()
{
    if (bindings_root_ == 0U)
        return;
    const std::uint64_t focused = top_screen() != nullptr ? top_screen()->surface.focused : 0U;
    std::size_t focus_index = 0U;
    if (Screen *screen = top_screen(); screen != nullptr && screen->builtin == Screen::Builtin::bindings)
    {
        const std::vector<std::uint64_t> order = widgets_.focusables(screen->root);
        focus_index = static_cast<std::size_t>(std::ranges::find(order, focused) - order.begin());
    }
    const std::string capture = capture_action_;
    open_bindings_screen();
    capture_action_ = capture;
    if (Screen *screen = top_screen())
    {
        const std::vector<std::uint64_t> order = widgets_.focusables(screen->root);
        std::vector<Dispatch> ignored;
        if (focus_index < order.size())
            set_focus(widgets_, screen->surface, order[focus_index], ignored);
    }
}

void UiService::capture_key(std::string_view canonical_key)
{
    if (capture_action_.empty())
        return;
    const std::string action = std::exchange(capture_action_, {});
    if (canonical_key == "Escape")
    {
        rebuild_bindings_screen();
        return;
    }
    std::string error;
    const bool clear = canonical_key == "Backspace" || canonical_key == "Delete";
    if (!bindings_.assign(action, clear ? std::string_view{} : canonical_key, error))
    {
        auto toast = std::make_unique<Toast>();
        toast->title = {TextRun{"Key Not Changed"}};
        toast->text = {TextRun{error}};
        toasts_.push_back(std::move(toast));
        while (toasts_.size() > max_toasts)
            toasts_.pop_front();
        backend_.play(SoundKey::error);
    }
    else
        save_bindings();
    rebuild_bindings_screen();
}

void UiService::save_bindings()
{
    backend_.write_local("mod-bindings.cfg", bindings_.save());
}

void UiService::open_config_screen(std::string_view mod)
{
    if (config_ == nullptr)
        return;
    const std::string wanted{mod};
    if (config_root_ != 0U)
        close_screen(config_root_, false);
    config_mod_ = wanted;
    Array rows;
    std::vector<std::string> keys;
    for (const ConfigSetting &setting : config_->settings())
    {
        if (setting.mod_id != wanted || rows.size() >= 200U)
            continue;
        Value control;
        const std::string label = setting.label.empty() ? setting.key : setting.label;
        if (setting.type == ValueType::boolean)
            control = make({{"kind", Value{"toggle"}}, {"text", Value{label}},
                            {"value", Value{setting.value.type() == ValueType::boolean && setting.value.as_bool()}},
                            {"width", Value{"fill"}}});
        else if ((setting.type == ValueType::number || setting.type == ValueType::integer) && setting.minimum &&
                 setting.maximum)
        {
            Map fields{{"kind", Value{"slider"}},
                       {"min", Value{*setting.minimum}},
                       {"max", Value{*setting.maximum}},
                       {"value", Value{setting.value.is_number() ? setting.value.as_number() : *setting.minimum}},
                       {"show_value", Value{true}},
                       {"width", Value{"fill"}}};
            if (setting.step)
                fields.emplace_back("step", Value{*setting.step});
            else if (setting.type == ValueType::integer)
                fields.emplace_back("step", Value{1});
            control = make({{"kind", Value{"stack"}}, {"gap", Value{4}}, {"stretch", Value{true}}, {"width", Value{"fill"}},
                            {"children", Value{Array{make({{"kind", Value{"label"}}, {"text", Value{label}}}), Value{std::move(fields)}}}}});
        }
        else if (setting.type == ValueType::string && !setting.choices.empty())
        {
            const std::string current = setting.value.type() == ValueType::string ? setting.value.as_string() : std::string{};
            control = make({{"kind", Value{"button"}},
                            {"text", Value{std::format("{}:  {}", label, current)}},
                            {"align", Value{"left"}},
                            {"width", Value{"fill"}}});
        }
        else
        {
            const std::string current = setting.value.type() == ValueType::string ? setting.value.as_string()
                                        : setting.value.is_number()           ? std::format("{}", setting.value.as_number())
                                                                              : std::string{};
            control = make({{"kind", Value{"stack"}}, {"gap", Value{4}}, {"stretch", Value{true}}, {"width", Value{"fill"}},
                            {"children", Value{Array{make({{"kind", Value{"label"}}, {"text", Value{label}}}),
                                                     make({{"kind", Value{"text_input"}}, {"value", Value{current}},
                                                           {"width", Value{"fill"}}, {"max_length", Value{256}}})}}}});
        }
        Array cell{control};
        if (!setting.doc.empty())
            cell.push_back(make({{"kind", Value{"label"}}, {"text", Value{setting.doc}}, {"size", Value{9}},
                                 {"color", Value{"menu.quiet"}}, {"width", Value{"fill"}}}));
        rows.push_back(make({{"kind", Value{"stack"}}, {"gap", Value{4}}, {"stretch", Value{true}}, {"width", Value{"fill"}},
                             {"children", Value{std::move(cell)}}}));
        keys.push_back(setting.key);
    }
    if (rows.empty())
        rows.push_back(make({{"kind", Value{"label"}}, {"text", Value{"This add-on has no settings."}}, {"color", Value{"menu.quiet"}}}));
    const std::string title = backend_.mod_title(wanted);
    const std::uint64_t content = open_screen(
        make({{"title", Value{std::format("{} Settings", title.empty() ? wanted : title)}},
              {"root", make({{"kind", Value{"list"}}, {"height", Value{"fill"}}, {"gap", Value{12}}, {"stretch", Value{true}},
                             {"children", Value{std::move(rows)}}})},
              {"max_width", Value{800}},
              {"pause", Value{true}}}),
        Owner{"", Side::client}, false);
    Screen *screen = top_screen();
    screen->builtin = Screen::Builtin::config;
    config_root_ = content;
    // Every control in a row sets that row's key.
    if (const Widget *list = widgets_.find(content))
        for (std::size_t index = 0U; index < list->children.size() && index < keys.size(); ++index)
        {
            std::vector<std::uint64_t> pending{list->children[index]};
            while (!pending.empty())
            {
                const std::uint64_t next = pending.back();
                pending.pop_back();
                Widget *widget = widgets_.find(next);
                if (widget == nullptr)
                    continue;
                if (widget->kind == WidgetKind::toggle || widget->kind == WidgetKind::slider ||
                    widget->kind == WidgetKind::button || widget->kind == WidgetKind::text_input)
                    widget->action = {InternalAction::Kind::config_set, 0, keys[index]};
                pending.insert(pending.end(), widget->children.begin(), widget->children.end());
            }
        }
}

void UiService::rebuild_config_screen()
{
    if (config_root_ == 0U || config_mod_.empty())
        return;
    std::size_t focus_index = 0U;
    if (Screen *screen = top_screen(); screen != nullptr && screen->builtin == Screen::Builtin::config)
    {
        const std::vector<std::uint64_t> order = widgets_.focusables(screen->root);
        focus_index = static_cast<std::size_t>(std::ranges::find(order, screen->surface.focused) - order.begin());
    }
    open_config_screen(config_mod_);
    if (Screen *screen = top_screen())
    {
        const std::vector<std::uint64_t> order = widgets_.focusables(screen->root);
        std::vector<Dispatch> ignored;
        if (focus_index < order.size())
            set_focus(widgets_, screen->surface, order[focus_index], ignored);
    }
}

// ---- Dispatching --------------------------------------------------------------

void UiService::dispatch(std::vector<Dispatch> &dispatches)
{
    for (Dispatch &dispatch : dispatches)
        pending_.push_back(std::move(dispatch));
    dispatches.clear();
    flush();
}

void UiService::flush()
{
    // A callback may queue more (a click that opens a dialog that focuses a
    // button). Those wait for the pass in progress, never recurse into it, and
    // a bounded number of passes keeps a loop from hanging a frame.
    if (flushing_)
        return;
    flushing_ = true;
    struct Done final
    {
        bool &flag;
        ~Done() { flag = false; }
    } done{flushing_};
    for (int pass = 0; pass < 16 && !pending_.empty(); ++pass)
    {
        std::vector<Dispatch> batch;
        batch.swap(pending_);
        for (const Dispatch &dispatch : batch)
        {
            // The script's handler first, then what the service itself does
            // (a dialog button runs its on_click and then closes the dialog).
            if (dispatch.callback.valid() && host_ != nullptr)
            {
                if (dispatch.widget.valid())
                {
                    const std::array<Value, 2> args{Value{dispatch.widget}, dispatch.value};
                    (void)host_->invoke(dispatch.callback, args);
                }
                else
                {
                    const std::array<Value, 1> args{dispatch.value};
                    (void)host_->invoke(dispatch.callback, args);
                }
            }
            if (dispatch.action.kind != InternalAction::Kind::none)
                run_internal(dispatch);
        }
    }
    pending_.clear();
}

void UiService::run_internal(const Dispatch &dispatch)
{
    const InternalAction &action = dispatch.action;
    switch (action.kind)
    {
    case InternalAction::Kind::none:
        break;
    case InternalAction::Kind::close_screen: {
        const std::uint64_t root = widgets_.root_of(dispatch.widget.id);
        close_screen(root != 0U ? root : (screens_.empty() ? 0U : screens_.back()->root), true);
        break;
    }
    case InternalAction::Kind::dialogue_choice: {
        Screen *screen = nullptr;
        for (auto &candidate : screens_)
            if (candidate->builtin == Screen::Builtin::dialogue)
                screen = candidate.get();
        if (screen == nullptr || action.index < 0 || static_cast<std::size_t>(action.index) >= screen->choice_ids.size())
            break;
        const auto index = static_cast<std::size_t>(action.index);
        const std::string choice = screen->choice_ids[index];
        const std::string id = dialogue_id_;
        backend_.play(SoundKey::click);
        if (native_session_ != 0U)
        {
            // The Gameplay service's dialogue: it answers its server itself.
            const std::function<void(std::size_t)> choose = native_choose_;
            close_screen(dialogue_root_, false);
            if (choose)
                choose(index);
            break;
        }
        if (dialogue_from_server_)
        {
            // Close first: the server's answer may open the next dialogue.
            close_screen(dialogue_root_, false);
            to_server(make({{"op", Value{"choice"}}, {"id", Value{id}}, {"choice", Value{choice}}}));
            break;
        }
        if (host_ != nullptr)
        {
            if (index < screen->choice_callbacks.size() && screen->choice_callbacks[index].valid())
            {
                const std::array<Value, 1> args{Value{choice}};
                (void)host_->invoke(screen->choice_callbacks[index], args);
            }
            else if (screen->on_choice.valid())
            {
                const std::array<Value, 2> args{Value{choice}, Value{id}};
                (void)host_->invoke(screen->on_choice, args);
            }
        }
        // The answer is given; the dialogue closes unless a handler already
        // opened the next one (which replaced it).
        if (dialogue_id_ == id && dialogue_root_ != 0U)
            close_screen(dialogue_root_, false);
        break;
    }
    case InternalAction::Kind::journal_select:
        if (action.index >= 0 && static_cast<std::size_t>(action.index) < quests_.size())
        {
            journal_selected_ = static_cast<std::size_t>(action.index);
            open_journal();
        }
        break;
    case InternalAction::Kind::config_set: {
        if (config_ == nullptr || config_mod_.empty())
            break;
        const Widget *widget = widgets_.find(dispatch.widget.id);
        std::optional<ConfigSetting> setting;
        for (ConfigSetting &candidate : config_->settings())
            if (candidate.mod_id == config_mod_ && candidate.key == action.data)
                setting = std::move(candidate);
        if (widget == nullptr || !setting)
            break;
        Value next;
        if (widget->kind == WidgetKind::toggle)
            next = Value{widget->checked};
        else if (widget->kind == WidgetKind::slider)
            next = setting->type == ValueType::integer ? Value{static_cast<std::int64_t>(std::llround(widget->value))}
                                                       : Value{widget->value};
        else if (widget->kind == WidgetKind::button && !setting->choices.empty())
        {
            const std::string current = setting->value.type() == ValueType::string ? setting->value.as_string() : std::string{};
            auto found = std::ranges::find(setting->choices, current);
            const std::size_t at = found == setting->choices.end()
                                       ? 0U
                                       : (static_cast<std::size_t>(found - setting->choices.begin()) + 1U) %
                                             setting->choices.size();
            next = Value{setting->choices[at]};
        }
        else if (widget->kind == WidgetKind::text_input)
        {
            if (dispatch.event != "submit")
                break;
            if (setting->type == ValueType::string)
                next = Value{widget->input};
            else
            {
                char *end{};
                const double number = std::strtod(widget->input.c_str(), &end);
                if (end == nullptr || *end != '\0' || widget->input.empty() || !std::isfinite(number))
                    break;
                next = setting->type == ValueType::integer ? Value{static_cast<std::int64_t>(std::llround(number))}
                                                           : Value{number};
            }
        }
        else
            break;
        std::string error;
        if (!config_->set(config_mod_, action.data, next, error))
        {
            auto toast = std::make_unique<Toast>();
            toast->title = {TextRun{"Setting Not Changed"}};
            toast->text = {TextRun{error}};
            toasts_.push_back(std::move(toast));
            while (toasts_.size() > max_toasts)
                toasts_.pop_front();
        }
        // A typed value is kept as typed until the field is left.
        if (widget->kind != WidgetKind::text_input)
            rebuild_config_screen();
        break;
    }
    case InternalAction::Kind::binding_capture:
        capture_action_ = action.data;
        rebuild_bindings_screen();
        break;
    case InternalAction::Kind::binding_reset:
        bindings_.reset(action.data);
        save_bindings();
        rebuild_bindings_screen();
        break;
    }
}

// ---- Input -----------------------------------------------------------------------

bool UiService::pointer_move(float x, float y)
{
    Screen *screen = top_screen();
    if (screen == nullptr)
        return false;
    std::vector<Dispatch> dispatches;
    route_pointer_move(widgets_, screen->surface, x, y, dispatches);
    dispatch(dispatches);
    return true;
}

bool UiService::pointer_button(float x, float y, bool down)
{
    Screen *screen = top_screen();
    if (screen == nullptr)
        return false;
    std::vector<Dispatch> dispatches;
    const bool used = route_pointer_button(widgets_, screen->surface, x, y, down, dispatches);
    if (used && down)
        backend_.play(SoundKey::click);
    dispatch(dispatches);
    // A modal screen swallows clicks outside its controls too.
    return true;
}

bool UiService::wheel(float x, float y, float steps)
{
    Screen *screen = top_screen();
    if (screen == nullptr)
        return false;
    route_wheel(widgets_, screen->surface, x, y, steps);
    return true;
}

bool UiService::key(NavKey key)
{
    Screen *screen = top_screen();
    if (screen == nullptr)
        return false;
    std::vector<Dispatch> dispatches;
    const std::uint64_t before = screen->surface.focused;
    const bool used = route_key(widgets_, screen->surface, key, dispatches);
    if (!used && key == NavKey::back)
    {
        // Escape / B: the screen's on_back may keep it; otherwise it closes.
        if (!screen->closable)
            return true;
        if (screen->on_back.valid() && host_ != nullptr)
        {
            const std::array<Value, 1> args{Value{widget_handle(screen->content)}};
            const Value keep = host_->invoke(screen->on_back, args);
            if (keep.type() == ValueType::boolean && !keep.as_bool())
                return true;
        }
        backend_.play(SoundKey::click);
        close_screen(screen->content, true);
        return true;
    }
    if (used && screen->surface.focused != before)
        backend_.play(SoundKey::click);
    dispatch(dispatches);
    return true;
}

bool UiService::text(std::string_view text)
{
    Screen *screen = top_screen();
    if (screen == nullptr)
        return false;
    std::vector<Dispatch> dispatches;
    const bool used = route_text(widgets_, screen->surface, text, dispatches);
    dispatch(dispatches);
    return used;
}

bool UiService::world_key(std::string_view canonical_key)
{
    const KeyBinding *binding = bindings_.for_key(canonical_key);
    if (binding == nullptr || !binding->callback.valid())
        return false;
    Dispatch dispatch;
    dispatch.callback = binding->callback;
    dispatch.value = Value{binding->action};
    std::vector<Dispatch> dispatches{std::move(dispatch)};
    this->dispatch(dispatches);
    return true;
}

void UiService::update(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        return;
    time_ += seconds;
    for (auto &toast : toasts_)
        toast->age += seconds;
    std::erase_if(toasts_, [](const auto &toast) { return toast->age >= toast->seconds; });
    if (title_)
    {
        title_->age += seconds;
        if (title_->age >= title_->fade_in + title_->stay + title_->fade_out)
            title_.reset();
    }
    flush();
}

// ---- The channel -------------------------------------------------------------------

void UiService::to_server(const Value &message)
{
    // Without the Network service's channel only a world this process hosts
    // can answer: the local player's reply goes straight to it.
    if (channel_ != nullptr)
        channel_->send_to_server(message);
    else
        receive_from_client(backend_.local_player(), message, time_);
}

void UiService::to_players(const std::vector<Handle> &players, const Value &message)
{
    for (const Handle &player : players)
    {
        if (channel_ != nullptr)
            channel_->send_to_player(player, message);
        else if (player == backend_.local_player())
            receive_from_server(message);
    }
}

std::vector<Handle> UiService::session_players() const
{
    if (channel_ != nullptr)
        return channel_->players();
    const Handle local = backend_.local_player();
    return local.valid() ? std::vector<Handle>{local} : std::vector<Handle>{};
}

void UiService::receive_from_server(const Value &message)
{
    // Everything a server sends is checked before anything is shown; a bad
    // message is dropped whole, never half-applied.
    if (message.type() != ValueType::map)
        return;
    const std::string op = bounded(message, "op", 32U);
    const Owner server_owner{"", Side::client};
    try
    {
        if (op == "dialogue")
        {
            Value spec = message;
            open_dialogue(spec, server_owner, true);
        }
        else if (op == "close_dialogue")
            close_dialogue(bounded(message, "id", max_id_bytes));
        else if (op == "toast")
            show_toast(message, {});
        else if (op == "title")
            show_title(message);
        else if (op == "objective")
            set_objective(message, {}, Side::server);
        else if (op == "clear_objective")
        {
            const std::string id = bounded(message, "id", max_id_bytes);
            std::erase_if(objectives_, [&](const auto &objective) {
                return objective->from_server && (id.empty() || objective->id == id);
            });
        }
        else if (op == "journal")
            set_journal(field(message, "quests"));
        else if (op == "open_journal")
            open_journal();
    }
    catch (const ScriptError &)
    {
        // A malformed request from the host: ignored.
    }
}

void UiService::receive_from_client(Handle player, const Value &message, double now)
{
    if (!player.valid() || message.type() != ValueType::map)
        return;
    // Rate limit before anything else runs.
    auto &[window, count] = client_rate_[player.id];
    if (now - window >= 1.0 || now < window)
    {
        window = now;
        count = 0;
    }
    if (++count > max_client_messages_per_second)
        return;
    const std::string op = bounded(message, "op", 32U);
    const std::string id = bounded(message, "id", max_id_bytes + 1U);
    if (!valid_id(id))
        return;
    const auto open = server_dialogues_.find(player.id);
    if (open == server_dialogues_.end())
        return;
    auto &dialogues = open->second;
    const auto found = std::ranges::find_if(dialogues, [&](const auto &dialogue) { return dialogue->id == id; });
    if (found == dialogues.end())
        return;
    if (op == "choice")
    {
        const std::string choice = bounded(message, "choice", max_id_bytes + 1U);
        if (std::ranges::find((*found)->choices, choice) == (*found)->choices.end())
            return;
        std::unique_ptr<ServerDialogue> dialogue = std::move(*found);
        dialogues.erase(found);
        if (host_ != nullptr)
        {
            (void)host_->emit(Side::server, "ui.dialogue_choice",
                              make({{"player", Value{player}}, {"dialogue", Value{dialogue->id}}, {"choice", Value{choice}}}));
            if (dialogue->callback.valid())
            {
                const std::array<Value, 3> args{Value{player}, Value{choice}, Value{dialogue->id}};
                (void)host_->invoke_as(player, dialogue->callback, args);
            }
        }
        release(dialogue->callback);
    }
    else if (op == "dialogue_closed")
    {
        std::unique_ptr<ServerDialogue> dialogue = std::move(*found);
        dialogues.erase(found);
        if (host_ != nullptr)
            (void)host_->emit(Side::server, "ui.dialogue_closed",
                              make({{"player", Value{player}}, {"dialogue", Value{dialogue->id}}}));
        release(dialogue->callback);
    }
}

void UiService::forget_player(Handle player)
{
    if (const auto found = server_dialogues_.find(player.id); found != server_dialogues_.end())
    {
        for (const auto &dialogue : found->second)
            release(dialogue->callback);
        server_dialogues_.erase(found);
    }
    client_rate_.erase(player.id);
}

// ---- Lifecycle -------------------------------------------------------------------

void UiService::clear_side(Side side)
{
    if (side == Side::server)
    {
        for (auto &[player, dialogues] : server_dialogues_)
            for (const auto &dialogue : dialogues)
                release(dialogue->callback);
        server_dialogues_.clear();
        client_rate_.clear();
        return;
    }
    // The client VM goes: everything on screen was its own or the session's.
    while (!screens_.empty())
        close_screen(screens_.back()->content, false);
    for (const auto &item : hud_)
        destroy_tree(item->root);
    hud_.clear();
    toasts_.clear();
    title_.reset();
    objectives_.clear();
    quests_.clear();
    std::vector<Callback> released;
    for (const auto &item : menu_items_)
        if (item->callback.valid())
            released.push_back(item->callback);
    menu_items_.clear();
    ++menu_revision_;
    bindings_.clear(released);
    release(released);
    hud_hidden_.fill(false);
    hud_offsets_.fill({0.0F, 0.0F});
    local_stats_.clear();
    hud_stats_.clear();
    boss_bars_.clear(); // (mod-ai)
    theme_.clear_all();
    localization_.clear();
    pending_.clear();
    capture_action_.clear();
}

void UiService::clear_all()
{
    clear_side(Side::server);
    clear_side(Side::client);
    // Anything left (a tree nobody holds any more).
    std::vector<Callback> released;
    widgets_.clear(released);
    release(released);
}

} // namespace openminerz::modding::ui
