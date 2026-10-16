// The modding API's interface service (openminerz/modding/ui.hpp): colours and
// asset paths, theme layering, rich text, localization, widget layout and
// drawing, event routing and focus navigation, screens, the HUD, story
// messages between server and client, menu rows, key bindings, hostile input,
// and the same features driven from Luau through the runtime's harness.

#include "openminerz/modding/ui.hpp"
#include "modding_test_harness.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;
using namespace openminerz::modding;
using namespace openminerz::modding::ui;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(float left, float right, float tolerance = 0.01F)
{
    return std::fabs(left - right) <= tolerance;
}

template <typename Function> void expect_error(Function &&function, std::string_view what)
{
    try
    {
        function();
    }
    catch (const ScriptError &)
    {
        return;
    }
    throw std::runtime_error(std::format("expected a ScriptError: {}", what));
}

Value table(std::initializer_list<std::pair<std::string, Value>> fields)
{
    return Value{Map(fields)};
}

// Every glyph is half its capital height wide.
class FixedMeasure final : public TextMeasure
{
  public:
    [[nodiscard]] float width(Face, float cap, std::string_view text) const override
    {
        std::size_t points = 0U;
        for (const char c : text)
            points += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1U : 0U;
        return cap * 0.5F * static_cast<float>(points);
    }
};

class FakeBackend final : public UiBackend
{
  public:
    Style current{Style::remastered};
    std::map<std::string, std::uint32_t> orders;
    std::map<std::string, std::string> files; // "mod/path" -> text
    std::map<std::string, std::string> local;
    bool single_player{true};
    std::vector<SoundKey> played;

    [[nodiscard]] Style style() const override { return current; }
    [[nodiscard]] std::uint32_t load_order(std::string_view mod) const override
    {
        const auto found = orders.find(std::string{mod});
        return found == orders.end() ? 0U : found->second;
    }
    [[nodiscard]] bool asset_exists(const AssetRef &asset) const override
    {
        return files.contains(asset.mod + "/" + asset.path);
    }
    [[nodiscard]] std::optional<std::string> read_mod_text(std::string_view mod, std::string_view path,
                                                           std::size_t limit) const override
    {
        const auto found = files.find(std::string{mod} + "/" + std::string{path});
        if (found == files.end() || found->second.size() > limit)
            return std::nullopt;
        return found->second;
    }
    [[nodiscard]] std::optional<std::string> read_local(std::string_view name) const override
    {
        const auto found = local.find(std::string{name});
        return found == local.end() ? std::nullopt : std::optional<std::string>{found->second};
    }
    void write_local(std::string_view name, std::string_view text) override
    {
        local[std::string{name}] = std::string{text};
    }
    [[nodiscard]] bool item_exists(int item) const override { return item >= 0 && item < 300; }
    [[nodiscard]] std::string item_name(int item) const override { return std::format("Item {}", item); }
    [[nodiscard]] std::string canonical_key(std::string_view key) const override
    {
        std::string result{key};
        if (result.size() == 1U)
            result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
        return result.size() <= 24U ? result : std::string{};
    }
    [[nodiscard]] bool offline() const override { return single_player; }
    [[nodiscard]] Handle local_player() const override { return {HandleKind::player, 7U}; }
    void play(SoundKey sound) override { played.push_back(sound); }
};

// Records what the service asks of the scripts.
class RecordingHost final : public ScriptHost
{
  public:
    struct Call final
    {
        Callback callback;
        std::vector<Value> args;
        Handle acting;
    };
    std::vector<Call> calls;
    std::vector<std::pair<std::string, Value>> events;
    std::vector<Callback> released;
    std::vector<std::function<void(Side)>> teardown;
    std::function<Value(const Callback &)> answer;

    bool emit(Side, std::string_view event, const Value &payload) override
    {
        events.emplace_back(std::string{event}, payload);
        return true;
    }
    Value invoke(const Callback &callback, std::span<const Value> args) override
    {
        calls.push_back({callback, {args.begin(), args.end()}, {}});
        return answer ? answer(callback) : Value{};
    }
    Value invoke_as(Handle acting_player, const Callback &callback, std::span<const Value> args) override
    {
        calls.push_back({callback, {args.begin(), args.end()}, acting_player});
        return answer ? answer(callback) : Value{};
    }
    void release(const Callback &callback) noexcept override { released.push_back(callback); }
    [[nodiscard]] std::string_view owner_of(const Callback &) const noexcept override { return "test"; }
    void log(std::string_view, LogLevel, std::string_view) override {}
    void add_teardown_listener(std::function<void(Side)> listener) override { teardown.push_back(std::move(listener)); }

    [[nodiscard]] std::size_t calls_of(std::uint32_t ref) const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(calls, [ref](const Call &call) { return call.callback.ref == ref; }));
    }
};

Callback cb(std::uint32_t ref)
{
    return {1U, ref};
}

FrameContext frame_of(int width, int height, float ui_scale = 1.0F)
{
    FrameContext frame;
    frame.width = width;
    frame.height = height;
    frame.ui_scale = ui_scale;
    frame.safe_area = 1.0F;
    for (std::size_t index = 0U; index < color_key_count; ++index)
        frame.palette[index] = Color{static_cast<std::uint8_t>(index), 10U, 20U, 255U};
    return frame;
}

// Calls a registered function as a mod would.
Value call(ApiRegistry &registry, std::string_view name, std::vector<Value> args, Side side = Side::client,
           std::string_view mod = "test",
           CapabilityMask capabilities = capability::presentation | capability::local_storage | capability::query |
                                         capability::network | capability::gameplay)
{
    const ApiRegistry::Function *function = registry.find_function(name);
    check(function != nullptr, std::format("{} is registered", name));
    check(side_in(side, function->spec.sides), std::format("{} exists on that side", name));
    check((capabilities & function->spec.requires_caps) == function->spec.requires_caps,
          std::format("{} needs a capability the caller lacks", name));
    CallContext context{mod, side, capabilities, {}};
    return function->call(context, args);
}

struct Rig final
{
    FakeBackend backend;
    RecordingHost host;
    ApiRegistry registry;
    UiService service{backend};
    FixedMeasure measure;
    Rig() { service.register_api(registry, host); }
    void draw(DrawList &out, int width = 1280, int height = 720)
    {
        const FrameContext frame = frame_of(width, height);
        service.draw_hud(frame, measure, out);
        service.draw_notices(frame, measure, out);
        service.draw_screens(frame, measure, out);
    }
};

// ---- Basics ---------------------------------------------------------------------

void colours_parse_every_form_and_refuse_garbage()
{
    check(parse_color(Value{"#F00"}) == Color{255U, 0U, 0U, 255U}, "#RGB");
    check(parse_color(Value{"#102030"}) == Color{16U, 32U, 48U, 255U}, "#RRGGBB");
    check(parse_color(Value{"#10203040"}) == Color{16U, 32U, 48U, 64U}, "#RRGGBBAA");
    check(parse_color(Value{Array{Value{1}, Value{2}, Value{3}}}) == Color{1U, 2U, 3U, 255U}, "array");
    check(parse_color(table({{"r", Value{9}}, {"g", Value{8}}, {"b", Value{7}}, {"a", Value{6}}})) ==
              Color{9U, 8U, 7U, 6U},
          "table");
    for (const Value &bad : {Value{"red"}, Value{"#12"}, Value{"#GG0000"}, Value{Array{Value{1}, Value{2}}},
                             Value{Array{Value{1}, Value{2}, Value{300}}}, Value{12}, Value{},
                             table({{"r", Value{1}}, {"g", Value{2}}})})
        check(!parse_color(bad).has_value(), "garbage is not a colour");
    check(scale_alpha(Color{1U, 2U, 3U, 200U}, 0.5F).a == 100U, "opacity scales alpha");
}

void asset_paths_never_leave_the_mod()
{
    check(normalize_asset_path("assets/ui/button.png") == "assets/ui/button.png", "plain path");
    check(normalize_asset_path("assets\\ui\\b.png") == "assets/ui/b.png", "backslashes become slashes");
    for (const std::string_view bad : {"", "/etc/passwd", "../other/x.png", "a/../../x.png", "C:/x.png", "a//b.png",
                                       "a/./b.png", "con.", "a/b ", "x?.png"})
        check(!normalize_asset_path(bad).has_value(), std::format("'{}' is refused", bad));
    check(!normalize_asset_path(std::string(300U, 'a')).has_value(), "long paths are refused");
}

// ---- Themes ----------------------------------------------------------------------

void theme_layers_by_load_order_then_style()
{
    ThemeStack theme;
    check(theme.empty() && !theme.color(ColorKey::menu_focus, Style::classic), "an empty stack overrides nothing");
    Palette base{};
    base.fill(Color{1U, 2U, 3U, 4U});
    check(theme.resolve(base, Style::modern) == base, "unmodded: the game's inks exactly");

    theme.apply("early", 1U, table({{"colors", table({{"menu.focus", Value{"#FF0000"}}, {"hud.health", Value{"#00FF00"}}})}}));
    theme.apply("late", 2U,
                table({{"classic", table({{"colors", table({{"menu.focus", Value{"#0000FF"}}})}})}}));
    check(theme.color(ColorKey::menu_focus, Style::classic) == Color{0U, 0U, 255U, 255U},
          "the later mod wins in the style it names");
    check(theme.color(ColorKey::menu_focus, Style::modern) == Color{255U, 0U, 0U, 255U},
          "elsewhere the earlier mod's general entry shows through");
    check(theme.color(ColorKey::hud_health, Style::classic) == Color{0U, 255U, 0U, 255U}, "untouched keys stay");

    // Within one mod, a style's entry beats the general one.
    theme.apply("early", 1U, table({{"modern", table({{"colors", table({{"menu.focus", Value{"#123456"}}})}})}}));
    check(theme.color(ColorKey::menu_focus, Style::modern) == Color{0x12U, 0x34U, 0x56U, 255U}, "style beats general");
    check(theme.color(ColorKey::menu_focus, Style::remastered) == Color{255U, 0U, 0U, 255U}, "general elsewhere");

    // Load order, not call order, decides.
    ThemeStack reordered;
    reordered.apply("late", 2U, table({{"colors", table({{"menu.row", Value{"#000002"}}})}}));
    reordered.apply("early", 1U, table({{"colors", table({{"menu.row", Value{"#000001"}}})}}));
    check(reordered.color(ColorKey::menu_row, Style::remastered) == Color{0U, 0U, 2U, 255U}, "load order decides");

    const Palette resolved = theme.resolve(base, Style::classic);
    check(resolved[static_cast<std::size_t>(ColorKey::menu_focus)] == Color{0U, 0U, 255U, 255U}, "resolve applies");
    check(resolved[static_cast<std::size_t>(ColorKey::menu_text)] == base[0], "resolve keeps the rest");

    const std::uint64_t revision = theme.revision();
    theme.clear("late");
    check(theme.revision() != revision, "clearing bumps the revision");
    check(theme.color(ColorKey::menu_focus, Style::classic) == Color{255U, 0U, 0U, 255U}, "the layer is gone");

    // Strings by the game's English text.
    theme.apply("early", 1U, table({{"strings", table({{"Play Offline", Value{"Begin"}}})}}));
    check(theme.has_strings(), "has strings");
    check(theme.text("Play Offline", Style::classic) != nullptr && *theme.text("Play Offline", Style::classic) == "Begin",
          "a replaced word");
    check(theme.text("Options", Style::classic) == nullptr, "others untouched");
}

void theme_refuses_bad_keys_without_changing_anything()
{
    ThemeStack theme;
    theme.apply("mod", 1U, table({{"colors", table({{"menu.row", Value{"#010203"}}})}}));
    const std::uint64_t revision = theme.revision();
    expect_error([&] { theme.apply("mod", 1U, table({{"colours", table({{"menu.bogus", Value{"#FFF"}}})}})); },
                 "unknown colour");
    expect_error([&] { theme.apply("mod", 1U, table({{"colors", table({{"menu.row", Value{"blue"}}})}, {"x", Value{1}}})); },
                 "unknown key");
    expect_error([&] { theme.apply("mod", 1U, table({{"fonts", table({{"display", Value{"../evil.ttf"}}})}})); },
                 "a font outside the mod");
    expect_error([&] { theme.apply("mod", 1U, table({{"fonts", table({{"display", Value{"a.exe"}}})}})); },
                 "not a font");
    expect_error([&] { theme.apply("mod", 1U, table({{"sounds", table({{"click", Value{"cue:no spaces"}}})}})); },
                 "bad cue");
    const auto exists = [](const AssetRef &asset) { return asset.path == "ui/b.png"; };
    expect_error([&] { theme.apply("mod", 1U, table({{"images", table({{"button", Value{"ui/missing.png"}}})}}), exists); },
                 "a missing file");
    check(theme.revision() == revision, "a refused theme changes nothing");
    check(theme.color(ColorKey::menu_row, Style::classic) == Color{1U, 2U, 3U, 255U}, "the old layer stands");

    theme.apply("mod", 1U,
                table({{"images", table({{"button", table({{"path", Value{"ui/b.png"}}, {"slice", Value{6}}})}})},
                       {"cursor", table({{"path", Value{"ui/b.png"}}, {"hotspot", Value{Array{Value{3}, Value{4}}}}})},
                       {"sounds", table({{"click", Value{"cue:Craft"}}})}}),
                exists);
    const NineSlice *button = theme.image(ImageKey::button, Style::modern);
    check(button != nullptr && button->slice == Insets{6.0F, 6.0F, 6.0F, 6.0F}, "a nine-slice image");
    check(theme.cursor(Style::classic) != nullptr && theme.cursor(Style::classic)->hot_x == 3, "a cursor");
    check(theme.sound(SoundKey::click, Style::classic)->cue == "Craft", "a cue");
}

// ---- Text ---------------------------------------------------------------------------

void markup_parses_runs_and_never_fails()
{
    const auto runs = parse_markup("plain [c=#FF0000]red [s=2]big[/s][/c] [f=display]face[/f]");
    check(runs.size() == 5U, std::format("five runs, got {}", runs.size()));
    check(runs[0].text == "plain " && !runs[0].color, "plain first");
    check(runs[1].text == "red " && runs[1].color == Color{255U, 0U, 0U, 255U}, "coloured");
    check(runs[2].text == "big" && runs[2].size == 2.0F && runs[2].color.has_value(), "nested size keeps colour");
    check(runs[4].face == Face::display, "face");
    check(plain_text(parse_markup("[[not a tag]")) == "[not a tag]", "[[ is a literal");
    check(plain_text(parse_markup("[c=nope]x[/c]")) == "[c=nope]x[/c]", "bad tags stay text");
    check(plain_text(parse_markup("[/c]stray[s=9]")) == "[/c]stray[s=9]", "unbalanced tags stay text");
    std::string deep;
    for (int index = 0; index < 100; ++index)
        deep += "[s=1.1]";
    check(!parse_markup(deep + "x").empty(), "deep nesting is bounded, not an error");
    const auto list = runs_from_value(Value{Array{Value{"a"}, table({{"text", Value{"b"}}, {"color", Value{"#00F"}}})}});
    check(list.size() == 2U && list[1].color == Color{0U, 0U, 255U, 255U}, "an array of runs");
    expect_error([] { (void)runs_from_value(table({{"x", Value{1}}})); }, "a table is not text");
}

void text_layout_wraps_truncates_and_aligns()
{
    const FixedMeasure measure;
    const std::vector<TextRun> runs{TextRun{"one two three four"}};
    // cap 10: each glyph 5 wide. "one two" = 35; wrap at 40.
    const TextBlock block = layout_text(runs, Face::text, 10.0F, Color{}, 40.0F, 0U, Align::left, measure);
    check(block.lines == 3U, std::format("three lines, got {}", block.lines));
    check(block.pieces[0].text == "one two" && block.pieces[1].text == "three", "greedy breaking");
    check(near(block.pieces[1].top, line_pitch(10.0F)), "line pitch");
    const TextBlock cut = layout_text(runs, Face::text, 10.0F, Color{}, 40.0F, 1U, Align::left, measure);
    check(cut.truncated && cut.lines == 1U && cut.pieces[0].text.ends_with("..."), "max_lines adds an ellipsis");
    check(cut.width <= 40.0F, "the shortened line fits");
    const TextBlock long_word = layout_text(std::vector<TextRun>{TextRun{std::string(30U, 'x')}}, Face::text, 10.0F, Color{}, 50.0F, 0U,
                                            Align::left, measure);
    check(long_word.lines == 3U && long_word.width <= 50.0F, "a word wider than a line is cut");
    const TextBlock centred = layout_text(std::vector<TextRun>{TextRun{"ab"}}, Face::text, 10.0F, Color{}, 100.0F, 0U, Align::center, measure);
    check(near(centred.pieces[0].x, 45.0F), "centred within the width");
    const TextBlock mixed = layout_text(parse_markup("a [s=2]B[/s]"), Face::text, 10.0F, Color{}, 0.0F, 0U,
                                        Align::left, measure);
    check(mixed.pieces.size() == 2U && near(mixed.pieces[0].top, 10.0F) && near(mixed.pieces[1].top, 0.0F),
          "runs share the foot of the line");
    const TextBlock utf8 = layout_text(std::vector<TextRun>{TextRun{"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"}}, Face::text, 10.0F, Color{}, 12.0F,
                                       0U, Align::left, measure);
    for (const PlacedText &piece : utf8.pieces)
        check(piece.text.size() % 2U == 0U, "a cut never splits a UTF-8 sequence");
}

void lang_files_parse_and_fall_back()
{
    StringTable table_en;
    std::string error;
    check(parse_lang_file("\xEF\xBB\xBF# comment\n greet = Hello, {name}!\nmulti=a\\nb\r\n\n", table_en, error), error);
    check(table_en.at("greet") == "Hello, {name}!" && table_en.at("multi") == "a\nb", "values and escapes");
    StringTable bad;
    check(!parse_lang_file("no equals here\n", bad, error) && error.find("line 1") != std::string::npos, "a bad line");
    check(!parse_lang_file("bad key! = x\n", bad, error), "a bad key");
    check(format_text("Hi {name}, {missing} {{x}", table({{"name", Value{"Ana"}}})) == "Hi Ana, {missing} {x}",
          "named placeholders");
    check(format_text("{2}-{1}", Value{Array{Value{"a"}, Value{3}}}) == "3-a", "numbered placeholders");
    check(language_fallbacks("pt_BR") == std::vector<std::string>{"pt-br", "pt", "en"}, "fallback chain");

    FakeBackend backend;
    backend.files["quest/lang/en.lang"] = "title = The Old Mine\nonly_en = English only\n";
    backend.files["quest/lang/pt.lang"] = "title = A Mina Velha\n";
    backend.files["quest/lang/de.lang"] = "this line is broken\n";
    Localization localization;
    localization.set_reader([&](std::string_view mod, std::string_view path) {
        return backend.read_mod_text(mod, path, max_lang_bytes);
    });
    localization.set_language("pt-BR");
    check(localization.text("quest", "title", {}) == "A Mina Velha", "the base language");
    check(localization.text("quest", "only_en", {}) == "English only", "falls back to English");
    check(localization.text("quest", "nope", {}) == "nope", "the key itself last");
    check(localization.text("other", "title", {}) == "title", "another mod's strings are its own");
    localization.set_language("de");
    check(localization.text("quest", "title", {}) == "The Old Mine", "a broken file falls back");
    check(!localization.errors().empty(), "and is reported");
}

// ---- Widgets: layout and drawing ------------------------------------------------------

void canvas_follows_scale_margins_and_ultrawide()
{
    const Canvas hd = make_canvas(frame_of(1920, 1080));
    check(near(hd.scale, 1.5F) && near(hd.width, 1280.0F) && near(hd.height, 720.0F), "1080p is the reference x1.5");
    const Canvas big = make_canvas(frame_of(1920, 1080, 1.25F));
    check(near(big.scale, 1.875F), "interface scale multiplies");
    const Canvas wide = make_canvas(frame_of(5120, 1440));
    check(near(wide.scale, 2.0F) && near(wide.width, 2560.0F), "5120x1440: height decides, the canvas is wide");
    check(near(wide.focus.width, wide.safe.height * 16.0F / 9.0F) &&
              near(wide.focus.x + wide.focus.width * 0.5F, wide.safe.x + wide.safe.width * 0.5F),
          "the focus frame is the middle 16:9");
    check(near(hd.focus.width, hd.safe.width), "on 16:9 the focus frame is everything");
}

void layout_anchors_stacks_grids_and_lengths()
{
    WidgetStore store;
    std::vector<Callback> adopted;
    const std::uint64_t root = store.build(
        table({{"kind", Value{"panel"}},
               {"width", Value{400}},
               {"height", Value{200}},
               {"anchor", Value{"center"}},
               {"padding", Value{10}},
               {"children", Value{Array{
                                table({{"kind", Value{"spacer"}}, {"id", Value{"corner"}}, {"width", Value{20}},
                                       {"height", Value{10}}, {"anchor", Value{"bottom_right"}}, {"x", Value{-5}}}),
                                table({{"kind", Value{"stack"}},
                                       {"id", Value{"column"}},
                                       {"width", Value{"50%"}},
                                       {"height", Value{"fill"}},
                                       {"gap", Value{10}},
                                       {"children", Value{Array{
                                                        table({{"kind", Value{"spacer"}}, {"height", Value{30}}}),
                                                        table({{"kind", Value{"spacer"}}, {"id", Value{"filler"}},
                                                               {"height", Value{"fill"}}}),
                                                        table({{"kind", Value{"spacer"}}, {"height", Value{40}}})}}}}),
                                table({{"kind", Value{"grid"}},
                                       {"id", Value{"grid"}},
                                       {"columns", Value{3}},
                                       {"gap", Value{4}},
                                       {"anchor", Value{"top_right"}},
                                       {"width", Value{100}},
                                       {"children", Value{Array{table({{"kind", Value{"item_slot"}}}),
                                                                table({{"kind", Value{"item_slot"}}}),
                                                                table({{"kind", Value{"item_slot"}}}),
                                                                table({{"kind", Value{"item_slot"}}})}}}})}}}}),
        Owner{"test", Side::client}, adopted);
    const FrameContext frame = frame_of(1920, 1080);
    const Canvas canvas = make_canvas(frame);
    const FixedMeasure measure;
    WidgetStore::LayoutContext context{&canvas, &measure, &frame, {}};
    store.layout(root, canvas.safe, context);
    const Widget *panel = store.find(root);
    // 400x200 units, centred on 1280x720 units, at 1.5 px per unit.
    check(near(panel->frame.x, (1280.0F - 400.0F) * 0.5F * 1.5F) && near(panel->frame.width, 600.0F),
          std::format("centred panel at {}", panel->frame.x));
    const Widget *corner = store.find(*store.find_named(root, "corner"));
    check(near(corner->frame.right(), panel->frame.right() - (10.0F + 5.0F) * 1.5F) &&
              near(corner->frame.bottom(), panel->frame.bottom() - 10.0F * 1.5F),
          "bottom_right anchor inside the padding, moved by x");
    const Widget *column = store.find(*store.find_named(root, "column"));
    check(near(column->frame.width, 190.0F * 1.5F) && near(column->frame.height, 180.0F * 1.5F),
          "50% of the content width and all of its height");
    const Widget *filler = store.find(*store.find_named(root, "filler"));
    check(near(filler->frame.height, (180.0F - 30.0F - 40.0F - 20.0F) * 1.5F), "fill takes what is left after gaps");
    const Widget *grid = store.find(*store.find_named(root, "grid"));
    const Widget *fourth = store.find(grid->children[3]);
    const Widget *first = store.find(grid->children[0]);
    check(near(fourth->frame.x, first->frame.x) && fourth->frame.y > first->frame.bottom(), "a grid wraps after 3");
    check(near(grid->frame.right(), panel->frame.right() - 15.0F), "top_right anchor");
}

void drawing_uses_the_palette_and_theme_images()
{
    WidgetStore store;
    std::vector<Callback> adopted;
    const std::uint64_t root = store.build(
        table({{"kind", Value{"stack"}},
               {"background", Value{"panel"}},
               {"children", Value{Array{table({{"kind", Value{"label"}}, {"text", Value{"Hi"}}, {"color", Value{"menu.accent"}}}),
                                        table({{"kind", Value{"button"}}, {"text", Value{"Go"}}})}}}}),
        Owner{"test", Side::client}, adopted);
    FrameContext frame = frame_of(1280, 720);
    const Canvas canvas = make_canvas(frame);
    const FixedMeasure measure;
    WidgetStore::LayoutContext context{&canvas, &measure, &frame, {}};
    store.layout(root, canvas.safe, context);
    DrawList flat;
    store.draw(root, context, 0U, 0U, false, flat);
    check(!flat.empty() && flat.front().kind == DrawCmd::Kind::fill &&
              flat.front().color == frame.palette[static_cast<std::size_t>(ColorKey::hud_panel)],
          "a panel is the theme's panel ink");
    const auto label = std::ranges::find_if(flat, [](const DrawCmd &draw) { return draw.text == "Hi"; });
    check(label != flat.end() && label->color == frame.palette[static_cast<std::size_t>(ColorKey::menu_accent)],
          "an ink name follows the palette");
    const NineSlice panel{{"test", "ui/panel.png"}, {8.0F, 8.0F, 8.0F, 8.0F}};
    frame.images[static_cast<std::size_t>(ImageKey::panel)] = &panel;
    DrawList themed;
    store.draw(root, context, 0U, 0U, false, themed);
    check(themed.front().kind == DrawCmd::Kind::image && themed.front().nine && themed.front().image.path == "ui/panel.png",
          "a theme's nine-slice replaces the flat panel");
}

// ---- Widgets: events and navigation ----------------------------------------------------

struct Built final
{
    WidgetStore store;
    std::uint64_t root{};
    FrameContext frame = frame_of(1280, 720);
    Canvas canvas = make_canvas(frame_of(1280, 720));
    FixedMeasure measure;
    void layout()
    {
        WidgetStore::LayoutContext context{&canvas, &measure, &frame, {}};
        store.layout(root, canvas.safe, context);
    }
    [[nodiscard]] const Widget &named(std::string_view name) const { return *store.find(*store.find_named(root, name)); }
    [[nodiscard]] std::pair<float, float> centre(std::string_view name) const
    {
        const Rect &rect = named(name).frame;
        return {rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F};
    }
};

void events_route_clicks_toggles_sliders_and_text()
{
    Built ui;
    std::vector<Callback> adopted;
    ui.root = ui.store.build(
        table({{"kind", Value{"stack"}},
               {"gap", Value{8}},
               {"children",
                Value{Array{table({{"kind", Value{"button"}}, {"id", Value{"ok"}}, {"text", Value{"OK"}}, {"on_click", Value{cb(1U)}}}),
                            table({{"kind", Value{"toggle"}}, {"id", Value{"flag"}}, {"text", Value{"Flag"}}, {"on_change", Value{cb(2U)}}}),
                            table({{"kind", Value{"slider"}}, {"id", Value{"level"}}, {"min", Value{0}}, {"max", Value{10}},
                                   {"step", Value{1}}, {"on_change", Value{cb(3U)}}}),
                            table({{"kind", Value{"text_input"}}, {"id", Value{"name"}}, {"max_length", Value{4}},
                                   {"on_change", Value{cb(4U)}}, {"on_submit", Value{cb(5U)}}})}}}}),
        Owner{"test", Side::client}, adopted);
    check(adopted.size() == 5U, "every handler adopted");
    ui.layout();
    Surface surface{ui.root};
    std::vector<Dispatch> out;
    auto [x, y] = ui.centre("ok");
    route_pointer_move(ui.store, surface, x, y, out);
    check(surface.focused == ui.named("ok").id, "resting on a button focuses it");
    route_pointer_button(ui.store, surface, x, y, true, out);
    route_pointer_button(ui.store, surface, x, y, false, out);
    check(std::ranges::count_if(out, [](const Dispatch &d) { return d.callback == cb(1U) && d.event == "click"; }) == 1,
          "press and release on the button is one click");
    out.clear();
    route_pointer_button(ui.store, surface, x, y, true, out);
    auto [tx, ty] = ui.centre("flag");
    route_pointer_button(ui.store, surface, tx, ty, false, out);
    check(std::ranges::none_of(out, [](const Dispatch &d) { return d.event == "click"; }), "a press dragged off is no click");

    out.clear();
    route_pointer_button(ui.store, surface, tx, ty, true, out);
    route_pointer_button(ui.store, surface, tx, ty, false, out);
    check(ui.named("flag").checked && out.back().callback == cb(2U) && out.back().value == Value{true}, "a toggle flips");

    out.clear();
    const Rect &slider = ui.named("level").frame;
    route_pointer_button(ui.store, surface, slider.x + slider.width * 0.72F, slider.y + 2.0F, true, out);
    route_pointer_move(ui.store, surface, slider.x + slider.width * 0.31F, slider.y + 2.0F, out);
    route_pointer_button(ui.store, surface, slider.x + slider.width * 0.31F, slider.y + 2.0F, false, out);
    check(ui.named("level").value == 3.0, std::format("the slider follows a drag and snaps, got {}", ui.named("level").value));
    check(route_key(ui.store, surface, NavKey::right, out) && ui.named("level").value == 4.0, "Right steps a slider");

    out.clear();
    auto [ix, iy] = ui.centre("name");
    route_pointer_button(ui.store, surface, ix, iy, true, out);
    route_pointer_button(ui.store, surface, ix, iy, false, out);
    check(surface.editing, "clicking a field edits it");
    route_text(ui.store, surface, "ab\x01" "c\xC3\xA9xyz", out);
    check(ui.named("name").input == "abc\xC3\xA9", std::format("control bytes dropped, max_length in characters: '{}'",
                                                               ui.named("name").input));
    route_key(ui.store, surface, NavKey::backspace, out);
    check(ui.named("name").input == "abc", "backspace removes one whole character");
    route_key(ui.store, surface, NavKey::activate, out);
    check(!surface.editing && out.back().callback == cb(5U) && out.back().value == Value{"abc"}, "Enter submits");
}

void navigation_is_spatial_wraps_and_tabs()
{
    Built ui;
    std::vector<Callback> adopted;
    ui.root = ui.store.build(
        table({{"kind", Value{"stack"}},
               {"gap", Value{6}},
               {"children", Value{Array{
                                table({{"kind", Value{"stack"}},
                                       {"direction", Value{"horizontal"}},
                                       {"gap", Value{6}},
                                       {"children", Value{Array{table({{"kind", Value{"button"}}, {"id", Value{"a"}}, {"text", Value{"A"}}}),
                                                                table({{"kind", Value{"button"}}, {"id", Value{"b"}}, {"text", Value{"B"}}})}}}}),
                                table({{"kind", Value{"button"}}, {"id", Value{"c"}}, {"text", Value{"C"}}}),
                                table({{"kind", Value{"button"}}, {"id", Value{"d"}}, {"text", Value{"D"}}, {"enabled", Value{false}}}),
                                table({{"kind", Value{"button"}}, {"id", Value{"e"}}, {"text", Value{"E"}}})}}}}),
        Owner{"test", Side::client}, adopted);
    ui.layout();
    Surface surface{ui.root};
    std::vector<Dispatch> out;
    check(move_focus(ui.store, surface, NavKey::down, out) && surface.focused == ui.named("a").id, "first focus");
    move_focus(ui.store, surface, NavKey::right, out);
    check(surface.focused == ui.named("b").id, "right goes across");
    move_focus(ui.store, surface, NavKey::down, out);
    check(surface.focused == ui.named("c").id, "down goes to the row below");
    move_focus(ui.store, surface, NavKey::down, out);
    check(surface.focused == ui.named("e").id, "a disabled button is skipped");
    move_focus(ui.store, surface, NavKey::down, out);
    check(surface.focused == ui.named("a").id || surface.focused == ui.named("b").id, "down from the last wraps to the top");
    move_focus(ui.store, surface, NavKey::previous, out);
    const auto order = ui.store.focusables(ui.root);
    check(order.size() == 4U, "four focusable controls");
    check(!move_focus(ui.store, surface, NavKey::left, out) || surface.focused != 0U, "left at the edge does not wrap");
    // Activate on the focused button clicks it.
    Surface second{ui.root};
    set_focus(ui.store, second, ui.named("c").id, out);
    out.clear();
    ui.store.find(ui.named("c").id)->on_click = cb(9U);
    check(route_key(ui.store, second, NavKey::activate, out) && out.back().callback == cb(9U), "Enter clicks");
}

void lists_scroll_and_reveal_the_focus()
{
    Built ui;
    Array rows;
    for (int index = 0; index < 30; ++index)
        rows.push_back(table({{"kind", Value{"button"}}, {"id", Value{std::format("row{}", index)}},
                              {"text", Value{std::format("Row {}", index)}}, {"height", Value{30}}}));
    std::vector<Callback> adopted;
    ui.root = ui.store.build(table({{"kind", Value{"list"}}, {"height", Value{200}}, {"width", Value{300}},
                                    {"gap", Value{4}}, {"children", Value{std::move(rows)}}}),
                             Owner{"test", Side::client}, adopted);
    ui.layout();
    const Widget &list = *ui.store.find(ui.root);
    check(near(list.content_height, 30.0F * 30.0F + 29.0F * 4.0F), "the list knows its content height");
    check(ui.named("row20").clip.empty(), "a row below the view is clipped away");
    Surface surface{ui.root};
    std::vector<Dispatch> out;
    set_focus(ui.store, surface, ui.named("row20").id, out);
    ui.layout();
    check(!ui.named("row20").clip.empty(), "focusing a row scrolls it into view");
    const float scrolled = ui.store.find(ui.root)->scroll;
    route_wheel(ui.store, surface, list.frame.x + 5.0F, list.frame.y + 5.0F, 1.0F);
    check(ui.store.find(ui.root)->scroll < scrolled, "the wheel scrolls back");
    DrawList draws;
    WidgetStore::LayoutContext context{&ui.canvas, &ui.measure, &ui.frame, {}};
    ui.layout();
    ui.store.draw(ui.root, context, 0U, 0U, false, draws);
    check(std::ranges::count_if(draws, [](const DrawCmd &d) { return d.kind == DrawCmd::Kind::clip_push; }) == 1 &&
              std::ranges::count_if(draws, [](const DrawCmd &d) { return d.kind == DrawCmd::Kind::clip_pop; }) == 1,
          "a list clips its rows");
}

// ---- Hostile input -------------------------------------------------------------------

void hostile_specs_are_refused_whole()
{
    WidgetStore store;
    std::vector<Callback> adopted;
    Value deep = table({{"kind", Value{"label"}}, {"text", Value{"leaf"}}});
    for (int level = 0; level < 40; ++level)
        deep = table({{"kind", Value{"panel"}}, {"on_click", Value{cb(static_cast<std::uint32_t>(level + 1))}},
                      {"children", Value{Array{deep}}}});
    expect_error([&] { (void)store.build(deep, Owner{"m", Side::client}, adopted); }, "a deep tree");
    check(store.size() == 0U && adopted.empty(), "nothing of a refused tree remains");

    Array many;
    for (int index = 0; index < 2000; ++index)
        many.push_back(table({{"kind", Value{"spacer"}}}));
    expect_error([&] { (void)store.build(table({{"kind", Value{"stack"}}, {"children", Value{many}}}), Owner{"m", Side::client}, adopted); },
                 "too many children");
    check(store.size() == 0U, "nothing remains");

    expect_error([&] {
        (void)store.build(table({{"kind", Value{"label"}}, {"text", Value{std::string(WidgetStore::max_text_bytes + 1U, 'x')}}}),
                          Owner{"m", Side::client}, adopted);
    }, "a huge string");
    expect_error([&] { (void)store.build(table({{"kind", Value{"label"}}, {"bogus", Value{1}}}), Owner{"m", Side::client}, adopted); },
                 "an unknown property");
    expect_error([&] { (void)store.build(table({{"kind", Value{"label"}}, {"width", Value{"1e99%"}}}), Owner{"m", Side::client}, adopted); },
                 "a bad length");
    expect_error([&] { (void)store.build(table({{"kind", Value{"label"}}, {"x", Value{std::nan("")}}}), Owner{"m", Side::client}, adopted); },
                 "NaN");
    expect_error([&] { (void)store.build(table({{"kind", Value{"image"}}, {"image", Value{"../../secret.png"}}}), Owner{"m", Side::client}, adopted); },
                 "an image outside the mod");
    expect_error([&] { (void)store.build(table({{"kind", Value{"label"}}, {"children", Value{Array{table({{"kind", Value{"label"}}})}}}}), Owner{"m", Side::client}, adopted); },
                 "a label holding widgets");

    // Thousands of widgets: capped across all trees.
    std::size_t built = 0U;
    try
    {
        for (int tree = 0; tree < 20; ++tree)
        {
            Array children;
            for (int index = 0; index < 1000; ++index)
                children.push_back(table({{"kind", Value{"spacer"}}}));
            (void)store.build(table({{"kind", Value{"stack"}}, {"children", Value{Array{table({{"kind", Value{"stack"}}, {"children", Value{children}}})}}}}),
                              Owner{"m", Side::client}, adopted);
            built = store.size();
        }
        check(false, "the widget cap was never reached");
    }
    catch (const ScriptError &)
    {
    }
    check(store.size() == built && store.size() <= WidgetStore::max_widgets, "the cap holds and the failed tree is gone");
}

// ---- The service ------------------------------------------------------------------------

void unmodded_service_draws_nothing_and_routes_nothing()
{
    Rig rig;
    check(!rig.service.active() && rig.service.theme().empty() && !rig.service.screen_open(), "nothing loaded");
    DrawList draws;
    rig.draw(draws);
    check(draws.empty(), "no mod UI: an empty draw list");
    check(!rig.service.key(NavKey::back) && !rig.service.pointer_button(10.0F, 10.0F, true), "input falls through");
    check(!rig.service.world_key("J"), "no binding");
    check(!rig.service.any_hud_hidden() && !rig.service.any_hud_moved(), "the HUD is the game's");
    check(rig.service.menu_items(MenuSlot::main).empty() && rig.service.menu_items(MenuSlot::addons).empty(), "no rows");
}

void screens_stack_pause_and_go_back()
{
    Rig rig;
    const Value screen = call(rig.registry, "ui.open_screen",
                              {table({{"title", Value{"Shop"}},
                                      {"on_close", Value{cb(11U)}},
                                      {"children", Value{Array{table({{"kind", Value{"button"}}, {"text", Value{"Buy"}}, {"on_click", Value{cb(12U)}}})}}}})});
    check(screen.type() == ValueType::handle && rig.service.screen_open() && rig.service.pauses_world(),
          "a screen opens and asks for a pause");
    const Value dialog = call(rig.registry, "ui.open_dialog",
                              {table({{"title", Value{"Sure?"}}, {"text", Value{"Really buy?"}},
                                      {"buttons", Value{Array{table({{"text", Value{"Yes"}}, {"on_click", Value{cb(13U)}}}),
                                                              table({{"text", Value{"No"}}})}}}})});
    check(rig.service.screen_roots().size() == 2U, "dialogs stack on screens");
    DrawList draws;
    rig.draw(draws);
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.text == "Really buy?"; }), "the dialog is drawn");
    // Enter on the focused first button runs its handler, then closes the dialog.
    check(rig.service.key(NavKey::activate), "Enter on the dialog");
    check(rig.host.calls_of(13U) == 1U && rig.service.screen_roots().size() == 1U, "Yes ran and the dialog closed");
    rig.service.key(NavKey::back);
    check(!rig.service.screen_open() && rig.host.calls_of(11U) == 1U, "Escape closes the screen and on_close runs");
    check(std::ranges::find(rig.host.released, cb(12U)) != rig.host.released.end() &&
              std::ranges::find(rig.host.released, cb(11U)) != rig.host.released.end(),
          "the screen's handlers are let go");
    check(std::ranges::any_of(rig.host.events, [](const auto &e) { return e.first == "ui.screen_closed"; }), "the event");
    (void)dialog;
}

void on_back_false_keeps_the_screen()
{
    Rig rig;
    call(rig.registry, "ui.open_screen", {table({{"on_back", Value{cb(21U)}}, {"children", Value{Array{}}}})});
    rig.host.answer = [](const Callback &) { return Value{false}; };
    rig.service.key(NavKey::back);
    check(rig.service.screen_open() && rig.host.calls_of(21U) == 1U, "on_back returned false: still open");
    rig.host.answer = {};
    rig.service.key(NavKey::back);
    check(!rig.service.screen_open(), "then it closes");
}

void widgets_belong_to_their_mod()
{
    Rig rig;
    const Value mine = call(rig.registry, "ui.open_screen", {table({{"children", Value{Array{table({{"kind", Value{"label"}}, {"id", Value{"t"}}})}}}})});
    expect_error([&] { call(rig.registry, "ui.set", {mine, table({{"visible", Value{false}}})}, Side::client, "intruder"); },
                 "another mod's widget");
    const Value label = call(rig.registry, "ui.find", {mine, Value{"t"}});
    check(label.type() == ValueType::handle, "ui.find");
    call(rig.registry, "ui.set", {label, table({{"text", Value{"[c=#00FF00]ok[/c]"}}, {"on_click", Value{cb(31U)}}})});
    call(rig.registry, "ui.set", {label, table({{"on_click", Value{cb(32U)}}})});
    check(std::ranges::find(rig.host.released, cb(31U)) != rig.host.released.end(), "a replaced handler is let go");
    check(call(rig.registry, "ui.get", {label, Value{"text"}}) == Value{"ok"}, "ui.get text is the plain words");
    expect_error([&] { call(rig.registry, "ui.set", {label, table({{"kind", Value{"button"}}})}); }, "kind cannot change");
    expect_error([&] { call(rig.registry, "ui.set", {label, table({{"nope", Value{cb(33U)}}})}); }, "bad property");
    check(std::ranges::find(rig.host.released, cb(33U)) != rig.host.released.end(), "a refused set lets its handlers go");
}

void hud_elements_stat_bars_and_built_in_switches()
{
    struct Stats final : StatSource
    {
        [[nodiscard]] std::optional<StatValue> stat(std::string_view name) const override
        {
            if (name == "survival.hunger")
                return StatValue{3.0, 4.0};
            return std::nullopt;
        }
    } stats;
    Rig rig;
    rig.service.set_stats(&stats);
    call(rig.registry, "ui.stat_bar", {table({{"stat", Value{"survival.hunger"}}, {"label", Value{"Food"}},
                                              {"anchor", Value{"bottom_left"}}, {"color", Value{"#C08040"}}})});
    call(rig.registry, "ui.stat_bar", {table({{"stat", Value{"local.thirst"}}, {"anchor", Value{"bottom_right"}}})});
    call(rig.registry, "ui.set_stat", {Value{"local.thirst"}, Value{1}, Value{2}});
    DrawList draws;
    rig.draw(draws);
    const auto fills = [&](Color color) {
        return std::ranges::find_if(draws, [&](const DrawCmd &d) { return d.kind == DrawCmd::Kind::fill && d.color == color; });
    };
    const auto hunger = fills(Color{0xC0U, 0x80U, 0x40U, 255U});
    check(hunger != draws.end(), "the hunger bar's level is drawn in its colour");
    const auto track = std::ranges::find_if(draws, [&](const DrawCmd &d) {
        return d.kind == DrawCmd::Kind::fill && d.color == Color{46U, 48U, 50U, 235U} && d.rect.y <= hunger->rect.y &&
               d.rect.bottom() >= hunger->rect.bottom() && d.rect.x <= hunger->rect.x;
    });
    check(track != draws.end() && near(hunger->rect.width / track->rect.width, 0.75F, 0.02F), "3 of 4 fills three quarters");
    check(hunger->rect.x < 640.0F && hunger->rect.y > 600.0F, "anchored bottom left");

    call(rig.registry, "ui.hide_hud", {Value{"crosshair"}});
    check(rig.service.hud_hidden(HudElement::crosshair) && rig.service.any_hud_hidden(), "a built-in element hidden");
    check(call(rig.registry, "ui.move_hud", {Value{"compass"}, Value{0}, Value{40}}) == Value{true}, "moves in Remastered");
    check(rig.service.hud_offset(HudElement::compass).second == 40.0F, "the offset is kept");
    expect_error([&] { call(rig.registry, "ui.move_hud", {Value{"crosshair"}, Value{1}, Value{1}}); }, "the crosshair stays");
    expect_error([&] { call(rig.registry, "ui.hide_hud", {Value{"everything"}}); }, "an unknown element");
    rig.backend.current = Style::classic;
    check(call(rig.registry, "ui.move_hud", {Value{"compass"}, Value{0}, Value{40}}) == Value{false},
          "the retail HUDs do not move");
    const Value names = call(rig.registry, "ui.hud_elements", {});
    check(names.as_array().size() == hud_element_count, "every element listed");
}

void toasts_titles_and_objectives()
{
    Rig rig;
    for (int index = 0; index < 10; ++index)
        call(rig.registry, "ui.toast", {Value{std::format("T{}", index)}, Value{"body"}, table({{"item", Value{5}}})});
    check(rig.service.toast_count() == max_toasts, "toasts are capped");
    call(rig.registry, "ui.title", {Value{"Day 1"}, Value{"Survive"}, table({{"fade_in", Value{0}}, {"stay", Value{1}}, {"fade_out", Value{0}}})});
    call(rig.registry, "ui.objective", {Value{"wood"}, Value{"Collect wood"}, table({{"progress", Value{0.5}}})});
    DrawList draws;
    rig.draw(draws);
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.text == "Day 1"; }), "the title is drawn");
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.text == "Collect wood"; }), "the objective");
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.kind == DrawCmd::Kind::item && d.item == 5; }),
          "a toast's item icon");
    rig.service.update(5.0);
    check(rig.service.toast_count() == 0U, "toasts expire");
    DrawList later;
    rig.draw(later);
    check(std::ranges::none_of(later, [](const DrawCmd &d) { return d.text == "Day 1"; }), "the title faded");
    call(rig.registry, "ui.clear_objective", {Value{"wood"}});
    DrawList cleared;
    rig.draw(cleared);
    check(cleared.empty(), "everything gone");
    expect_error([&] { call(rig.registry, "ui.toast", {Value{std::string(max_message_text + 1U, 'x')}}); }, "a huge toast");
}

void server_dialogue_round_trip_is_validated()
{
    Rig rig;
    const Value player{Handle{HandleKind::player, 7U}};
    // The server asks; with no network channel the local player's client gets
    // it in process (single player).
    call(rig.registry, "ui.show_dialogue",
         {player,
          table({{"id", Value{"miner"}}, {"speaker", Value{"Old Miner"}}, {"text", Value{"Help me?"}},
                 {"choices", Value{Array{table({{"id", Value{"yes"}}, {"text", Value{"Yes"}}}),
                                         table({{"id", Value{"no"}}, {"text", Value{"No"}}})}}}}),
          Value{cb(41U)}},
         Side::server, "quest");
    check(rig.service.screen_open() && rig.service.pauses_world(), "the client shows the dialogue");
    DrawList draws;
    rig.draw(draws);
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.text == "Old Miner"; }), "the speaker");
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.text.find("No") != std::string::npos; }), "choices");

    // A forged answer: a choice that was not offered, a dialogue never shown.
    rig.service.receive_from_client({HandleKind::player, 7U},
                                    table({{"op", Value{"choice"}}, {"id", Value{"miner"}}, {"choice", Value{"gold"}}}), 0.0);
    rig.service.receive_from_client({HandleKind::player, 7U},
                                    table({{"op", Value{"choice"}}, {"id", Value{"other"}}, {"choice", Value{"yes"}}}), 0.0);
    rig.service.receive_from_client({HandleKind::player, 9U},
                                    table({{"op", Value{"choice"}}, {"id", Value{"miner"}}, {"choice", Value{"yes"}}}), 0.0);
    check(rig.host.calls.empty() && rig.host.events.empty(), "forged answers reach no script");

    check(rig.service.choice_key(2), "the player presses 2");
    check(!rig.service.screen_open(), "the dialogue closes");
    check(rig.host.calls_of(41U) == 1U && rig.host.calls.back().acting == Handle{HandleKind::player, 7U} &&
              rig.host.calls.back().args[1] == Value{"no"},
          "on_choice ran for that player with their choice");
    const auto event = std::ranges::find_if(rig.host.events, [](const auto &e) { return e.first == "ui.dialogue_choice"; });
    check(event != rig.host.events.end() && *event->second.field("choice") == Value{"no"}, "and the event");
    // Answering twice does nothing more.
    rig.service.receive_from_client({HandleKind::player, 7U},
                                    table({{"op", Value{"choice"}}, {"id", Value{"miner"}}, {"choice", Value{"yes"}}}), 0.0);
    check(rig.host.calls_of(41U) == 1U, "one answer per dialogue");
    expect_error([&] {
        call(rig.registry, "ui.show_dialogue", {Value{Array{player, player}}, table({{"id", Value{"x"}}}), Value{cb(42U)}},
             Side::server, "quest");
    }, "on_choice for several players");
    check(std::ranges::find(rig.host.released, cb(42U)) != rig.host.released.end(), "the refused handler is let go");
}

void client_messages_are_rate_limited_and_bounded()
{
    Rig rig;
    const Handle player{HandleKind::player, 7U};
    call(rig.registry, "ui.show_dialogue",
         {Value{player}, table({{"id", Value{"d"}}, {"choices", Value{Array{Value{"A"}}}}})}, Side::server, "quest");
    for (int index = 0; index < max_client_messages_per_second + 5; ++index)
        rig.service.receive_from_client(player, table({{"op", Value{"bogus"}}, {"id", Value{"d"}}}), 1.0);
    rig.service.receive_from_client(player, table({{"op", Value{"choice"}}, {"id", Value{"d"}}, {"choice", Value{"1"}}}), 1.5);
    check(rig.host.events.empty(), "over the limit in that second: dropped before any script");
    rig.service.receive_from_client(player, table({{"op", Value{"choice"}}, {"id", Value{"d"}}, {"choice", Value{"1"}}}), 2.6);
    check(rig.host.events.size() == 1U, "the next second it is heard");
    // Hostile server messages are dropped whole on the client.
    rig.service.key(NavKey::back); // the dialogue shown above
    check(!rig.service.screen_open(), "the dialogue closed");
    rig.service.receive_from_server(table({{"op", Value{"dialogue"}},
                                           {"choices", Value{Array(max_dialogue_choices + 1U, Value{"x"})}}}));
    rig.service.receive_from_server(table({{"op", Value{"journal"}}, {"quests", Value{"not a list"}}}));
    rig.service.receive_from_server(Value{"garbage"});
    check(!rig.service.screen_open(), "nothing opened from bad requests");
    rig.service.receive_from_server(table({{"op", Value{"toast"}}, {"title", Value{std::string(100000U, 'x')}}}));
    check(rig.service.toast_count() == 0U, "an oversized toast is dropped");
}

void journal_shows_quests_and_selects()
{
    Rig rig;
    call(rig.registry, "ui.set_journal",
         {Value{Array{table({{"id", Value{"q1"}}, {"title", Value{"The Old Mine"}}, {"text", Value{"Find the miner."}},
                             {"objectives", Value{Array{table({{"text", Value{"Reach the mine"}}, {"done", Value{true}}}),
                                                        table({{"text", Value{"Talk to him"}}, {"progress", Value{0.25}}})}}}}),
                      table({{"id", Value{"q2"}}, {"title", Value{"Water"}}, {"state", Value{"done"}}})}}});
    call(rig.registry, "ui.open_journal", {});
    DrawList draws;
    rig.draw(draws);
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.text == "Find the miner."; }), "the selected quest");
    rig.service.key(NavKey::down);
    rig.service.key(NavKey::activate);
    DrawList second;
    rig.draw(second);
    check(std::ranges::count_if(second, [](const DrawCmd &d) { return d.text.find("Water") != std::string::npos; }) >= 2,
          "the other quest opened (its row and its title)");
    check(std::ranges::none_of(second, [](const DrawCmd &d) { return d.text == "Find the miner."; }),
          "the first quest's text is gone");
    expect_error([&] { call(rig.registry, "ui.set_journal", {Value{Array{table({{"title", Value{"no id"}}})}}}); }, "a quest needs an id");
}

void menu_rows_settings_and_key_bindings()
{
    Rig rig;
    rig.backend.orders = {{"early", 1U}, {"late", 2U}};
    const Value id = call(rig.registry, "ui.add_menu_entry",
                          {table({{"menu", Value{"pause"}}, {"label", Value{"Quests"}}, {"on_select", Value{cb(51U)}}})});
    const auto pause = rig.service.menu_items(MenuSlot::pause);
    check(pause.size() == 1U && pause[0].label == "Quests", "a pause-menu row");
    rig.service.activate_menu_item(MenuSlot::pause, 0U);
    check(rig.host.calls_of(51U) == 1U, "choosing it runs on_select");
    const std::uint64_t revision = rig.service.menu_revision();
    call(rig.registry, "ui.remove_menu_entry", {id});
    check(rig.service.menu_items(MenuSlot::pause).empty() && rig.service.menu_revision() != revision, "removed");
    expect_error([&] { call(rig.registry, "ui.add_menu_entry", {table({{"menu", Value{"title"}}, {"label", Value{"x"}}})}); },
                 "an unknown menu");

    // Bindings: game keys are never taken; earlier mods keep a contested key.
    call(rig.registry, "input.bind", {Value{"jump2"}, Value{"space"}, Value{cb(61U)}}, Side::client, "early");
    call(rig.registry, "input.bind", {Value{"journal"}, Value{"j"}, Value{cb(62U)}, table({{"label", Value{"Journal"}}})},
         Side::client, "late");
    call(rig.registry, "input.bind", {Value{"map"}, Value{"J"}, Value{cb(63U)}}, Side::client, "early");
    const KeyBinding *jump = rig.service.bindings().find("early.jump2");
    check(jump != nullptr && jump->key.empty() && !jump->conflict.empty(), "a game key stays the game's");
    check(rig.service.bindings().find("early.map")->key == "J", "the earlier mod keeps J");
    check(rig.service.bindings().find("late.journal")->key.empty(), "the later one waits for the player");
    check(rig.service.world_key("J") && rig.host.calls_of(63U) == 1U, "J runs the earlier mod's handler");
    check(!rig.service.world_key("Space"), "Space is never a mod's");
    check(call(rig.registry, "input.key", {Value{"map"}}, Side::client, "early") == Value{"J"}, "input.key");

    // The bindings page: give Journal the key K, persist, reload.
    check(rig.service.menu_items(MenuSlot::addons).back().bindings, "the Add-Ons screen offers the bindings page");
    rig.service.open_bindings_screen();
    check(rig.service.screen_open(), "the page opens");
    std::string error;
    KeyBindings copy;
    copy.load(rig.backend.local["mod-bindings.cfg"]);
    (void)copy;
    rig.service.capture_key("K"); // nothing is being captured: ignored
    check(!rig.service.capturing_key(), "no capture without a click");
    // Find the Journal row's key button by walking the page.
    const auto roots = rig.service.screen_roots();
    std::uint64_t journal_button = 0U;
    for (const std::uint64_t root : roots)
    {
        std::vector<std::uint64_t> pending{root};
        while (!pending.empty())
        {
            const std::uint64_t next = pending.back();
            pending.pop_back();
            const Widget *widget = rig.service.widgets().find(next);
            if (widget == nullptr)
                continue;
            if (widget->action.kind == InternalAction::Kind::binding_capture && widget->action.data == "late.journal")
                journal_button = next;
            pending.insert(pending.end(), widget->children.begin(), widget->children.end());
        }
    }
    check(journal_button != 0U, "the Journal row has a key button");
    DrawList layout;
    rig.draw(layout);
    const Widget *button = rig.service.widgets().find(journal_button);
    rig.service.pointer_button(button->frame.x + 2.0F, button->frame.y + 2.0F, true);
    rig.service.pointer_button(button->frame.x + 2.0F, button->frame.y + 2.0F, false);
    check(rig.service.capturing_key(), "clicking the key button waits for a key");
    rig.service.capture_key("W");
    check(rig.service.bindings().find("late.journal")->key.empty(), "W is the game's: refused");
    check(rig.service.toast_count() == 1U, "and the player is told why");
    rig.service.capture_key("K"); // not capturing any more
    check(rig.service.bindings().find("late.journal")->key.empty(), "a key only counts while capturing");

    KeyBindings bindings;
    bindings.set_canonical([](std::string_view key) { return std::string{key}; });
    (void)bindings.bind("m", 1U, "a", "K", "A", cb(70U));
    check(bindings.assign("m.a", "L", error) && bindings.find("m.a")->key == "L" && bindings.find("m.a")->user_set,
          "the player's choice");
    const std::string saved = bindings.save();
    KeyBindings restored;
    restored.set_canonical([](std::string_view key) { return std::string{key}; });
    restored.load(saved);
    (void)restored.bind("m", 1U, "a", "K", "A", cb(70U));
    check(restored.find("m.a")->key == "L", "saved choices win over defaults");
    restored.load("m.a=Space\n");
    check(restored.find("m.a")->key.empty(), "a game key in an edited file is still refused");
    check(!bindings.assign("m.a", "Escape", error), "assigning a game key fails");
}

void config_pages_come_from_mod_config()
{
    struct Config final : ConfigSource
    {
        std::vector<ConfigSetting> list;
        std::vector<std::pair<std::string, Value>> sets;
        [[nodiscard]] std::vector<ConfigSetting> settings() const override { return list; }
        bool set(std::string_view mod, std::string_view key, const Value &value, std::string &error) override
        {
            for (ConfigSetting &setting : list)
                if (setting.mod_id == mod && setting.key == key)
                {
                    setting.value = value;
                    sets.emplace_back(std::string{key}, value);
                    return true;
                }
            error = "unknown";
            return false;
        }
    } config;
    ConfigSetting hard;
    hard.mod_id = "survival";
    hard.key = "hard";
    hard.label = "Hard Mode";
    hard.type = ValueType::boolean;
    hard.value = Value{false};
    ConfigSetting rate;
    rate.mod_id = "survival";
    rate.key = "rate";
    rate.label = "Hunger Rate";
    rate.type = ValueType::number;
    rate.value = Value{1.0};
    rate.minimum = 0.0;
    rate.maximum = 2.0;
    rate.step = 0.5;
    config.list = {hard, rate};
    Rig rig;
    rig.service.set_config(&config);
    const auto addons = rig.service.menu_items(MenuSlot::addons);
    check(addons.size() == 1U && addons[0].config_mod == "survival", "a settings row per mod with settings");
    rig.service.activate_menu_item(MenuSlot::addons, 0U);
    check(rig.service.screen_open(), "the settings page opens");
    DrawList draws;
    rig.draw(draws);
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.text == "Hard Mode"; }), "a toggle row");
    check(rig.service.top_surface()->focused != 0U, "the first control starts focused");
    rig.service.key(NavKey::activate);
    check(!config.sets.empty() && config.sets.back() == std::pair<std::string, Value>{"hard", Value{true}},
          "toggling sets the setting through mod.config");
    rig.draw(draws);
    rig.service.key(NavKey::down);
    rig.service.key(NavKey::right);
    check(config.sets.back().first == "rate" && config.sets.back().second == Value{1.5}, "a slider steps by its step");
}

void teardown_drops_a_sides_work()
{
    Rig rig;
    call(rig.registry, "ui.theme", {table({{"colors", table({{"menu.row", Value{"#FFFFFF"}}})}})});
    call(rig.registry, "ui.add_hud", {table({{"kind", Value{"label"}}, {"text", Value{"x"}}, {"on_hover", Value{cb(81U)}}})});
    call(rig.registry, "ui.open_screen", {table({{"children", Value{Array{}}}, {"on_close", Value{cb(82U)}}})});
    call(rig.registry, "input.bind", {Value{"a"}, Value{"K"}, Value{cb(83U)}});
    check(rig.host.teardown.size() == 1U, "the service listens for teardown");
    rig.host.teardown.front()(Side::client);
    check(!rig.service.active() && rig.service.theme().empty() && rig.service.widgets().size() == 0U,
          "the client side's UI is gone");
    for (const std::uint32_t ref : {81U, 82U, 83U})
        check(std::ranges::find(rig.host.released, cb(ref)) != rig.host.released.end(), "every handler let go");
    check(rig.host.calls_of(82U) == 0U, "teardown is not the player closing a screen");
}

void server_functions_need_network_and_client_ones_presentation()
{
    Rig rig;
    const ApiRegistry::Function *theme = rig.registry.find_function("ui.theme");
    check(theme->spec.sides == side_client && theme->spec.requires_caps == capability::presentation,
          "ui.theme: client, presentation");
    const ApiRegistry::Function *toast = rig.registry.find_function("ui.toast");
    check(toast->spec.sides == side_both, "ui.toast exists on both sides");
    CallContext no_network{"m", Side::server, capability::gameplay, {}};
    const std::array<Value, 2> args{Value{}, Value{"hi"}};
    expect_error([&] { (void)toast->call(no_network, args); }, "server toast without network");
    CallContext client_only{"m", Side::client, capability::local_storage, {}};
    expect_error([&] { (void)toast->call(client_only, args); }, "client toast without presentation");
    check(rig.registry.find_event("ui.dialogue_choice") != nullptr && rig.registry.find_event("ui.screen_closed") != nullptr,
          "the events are registered");
}

// ---- Through Luau ----------------------------------------------------------------------

void scripts_drive_the_service_through_the_runtime()
{
    testing::Harness harness;
    FakeBackend backend;
    backend.files["test/lang/en.lang"] = "hello = Hello, {name}!\n";
    UiService service{backend};
    register_ui_api(harness.registry(), harness.host(), service);
    const CapabilityMask client = testing::client_mod_capabilities();
    RunResult result = harness.run(R"(
        ui.theme({ colors = { ["menu.focus"] = "#FF8800" }, remastered = { colors = { ["hud.health"] = "#00FF00" } } })
        clicked = 0
        local root = ui.open_screen({
            title = ui.text("hello", { name = "Ana" }),
            children = {
                { kind = "button", id = "go", text = "Go", on_click = function(widget) clicked = clicked + 1 end },
                { kind = "slider", id = "level", min = 0, max = 10, step = 1, value = 5 },
            },
        })
        level = ui.find(root, "level")
        return ui.text("hello", { name = "Ana" })
    )",
                                   Side::client, client);
    check(result.ok, result.error);
    check(result.value == Value{"Hello, Ana!"}, "ui.text formats from lang/en.lang");
    check(service.theme().color(ColorKey::menu_focus, Style::classic) == Color{255U, 136U, 0U, 255U}, "ui.theme from Luau");
    check(service.screen_open(), "ui.open_screen from Luau");
    FixedMeasure measure;
    DrawList draws;
    service.draw_screens(frame_of(1280, 720), measure, draws);
    check(std::ranges::any_of(draws, [](const DrawCmd &d) { return d.text == "Hello, Ana!"; }), "the title is the text");
    check(service.key(NavKey::activate), "the first button starts focused: Enter clicks it");
    result = harness.run("return clicked", Side::client, client);
    check(result.ok && result.value == Value{1}, "the Luau on_click ran once");
    check(service.key(NavKey::down) && service.key(NavKey::right), "Right on the slider");
    result = harness.run("return ui.get(level, 'value')", Side::client, client);
    check(result.ok && result.value == Value{6}, std::format("the slider moved: {}", result.error));
    // A client-scope mod has no network: server requests are not even there.
    result = harness.run("ui.toast(nil, 'x')", Side::client, client);
    check(result.ok, "a client toast with a nil title is just an empty toast? " + result.error);
    result = harness.run("return ui.hide_hud('crosshair')", Side::client, client);
    check(result.ok && service.hud_hidden(HudElement::crosshair), "ui.hide_hud from Luau");
    result = harness.run("ui.open_screen({ children = { { kind = 'nope' } } })", Side::client, client);
    check(!result.ok && result.error.find("unknown widget kind") != std::string::npos, "script errors carry the reason");
    harness.runtime().stop(Side::client);
    check(!service.screen_open() && service.theme().empty(), "stopping the VM tears the UI down");
}

void server_scripts_send_story_requests()
{
    testing::Harness harness;
    FakeBackend backend;
    UiService service{backend};
    register_ui_api(harness.registry(), harness.host(), service);
    RunResult result = harness.run(R"(
        answers = {}
        events.on("ui.dialogue_choice", function(e) answers[#answers + 1] = e.choice end)
        return true
    )",
                                   Side::server);
    check(result.ok, result.error);
    // The player handle the runtime marshals: built in C++ and passed in.
    const ApiRegistry::Function *show = harness.registry().find_function("ui.show_dialogue");
    CallContext server{"test", Side::server, testing::server_mod_capabilities(Side::server), {}};
    const std::array<Value, 2> args{
        Value{Handle{HandleKind::player, 7U}},
        table({{"id", Value{"intro"}}, {"text", Value{"Welcome."}}, {"choices", Value{Array{Value{"Thanks"}, Value{"Bye"}}}}})};
    (void)show->call(server, args);
    check(service.screen_open(), "the local client shows the server's dialogue");
    check(service.choice_key(1), "the player answers");
    result = harness.run("return answers[1]", Side::server);
    check(result.ok && result.value == Value{"1"}, std::format("the server script heard the answer: {}", result.error));
}

struct TestCase final
{
    const char *name;
    void (*body)();
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"colours_parse_every_form_and_refuse_garbage", colours_parse_every_form_and_refuse_garbage},
        {"asset_paths_never_leave_the_mod", asset_paths_never_leave_the_mod},
        {"theme_layers_by_load_order_then_style", theme_layers_by_load_order_then_style},
        {"theme_refuses_bad_keys_without_changing_anything", theme_refuses_bad_keys_without_changing_anything},
        {"markup_parses_runs_and_never_fails", markup_parses_runs_and_never_fails},
        {"text_layout_wraps_truncates_and_aligns", text_layout_wraps_truncates_and_aligns},
        {"lang_files_parse_and_fall_back", lang_files_parse_and_fall_back},
        {"canvas_follows_scale_margins_and_ultrawide", canvas_follows_scale_margins_and_ultrawide},
        {"layout_anchors_stacks_grids_and_lengths", layout_anchors_stacks_grids_and_lengths},
        {"drawing_uses_the_palette_and_theme_images", drawing_uses_the_palette_and_theme_images},
        {"events_route_clicks_toggles_sliders_and_text", events_route_clicks_toggles_sliders_and_text},
        {"navigation_is_spatial_wraps_and_tabs", navigation_is_spatial_wraps_and_tabs},
        {"lists_scroll_and_reveal_the_focus", lists_scroll_and_reveal_the_focus},
        {"hostile_specs_are_refused_whole", hostile_specs_are_refused_whole},
        {"unmodded_service_draws_nothing_and_routes_nothing", unmodded_service_draws_nothing_and_routes_nothing},
        {"screens_stack_pause_and_go_back", screens_stack_pause_and_go_back},
        {"on_back_false_keeps_the_screen", on_back_false_keeps_the_screen},
        {"widgets_belong_to_their_mod", widgets_belong_to_their_mod},
        {"hud_elements_stat_bars_and_built_in_switches", hud_elements_stat_bars_and_built_in_switches},
        {"toasts_titles_and_objectives", toasts_titles_and_objectives},
        {"server_dialogue_round_trip_is_validated", server_dialogue_round_trip_is_validated},
        {"client_messages_are_rate_limited_and_bounded", client_messages_are_rate_limited_and_bounded},
        {"journal_shows_quests_and_selects", journal_shows_quests_and_selects},
        {"menu_rows_settings_and_key_bindings", menu_rows_settings_and_key_bindings},
        {"config_pages_come_from_mod_config", config_pages_come_from_mod_config},
        {"teardown_drops_a_sides_work", teardown_drops_a_sides_work},
        {"server_functions_need_network_and_client_ones_presentation",
         server_functions_need_network_and_client_ones_presentation},
        {"scripts_drive_the_service_through_the_runtime", scripts_drive_the_service_through_the_runtime},
        {"server_scripts_send_story_requests", server_scripts_send_story_requests},
    };
    std::size_t failures{};
    for (const auto &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << std::endl;
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
