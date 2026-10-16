#pragma once

// The modding API's interface service: themes, widgets, screens, the HUD,
// story screens, menu entries, key bindings and localization.
//
// Everything here is PRESENTATION. A client-scope UI mod gets the client VM's
// `presentation` and `local_storage` capabilities only, so it works on any
// server, vanilla CastleMiner Z hosts included: nothing in this file touches
// the simulation, the wire or a save. The one exception is the small
// server-side surface (ui.show_dialogue(player, ...), ui.toast(player, ...),
// ...), which only SENDS a request to a player's client over the mod channel
// (UiChannel) and runs only where a server mod runs.
//
// The service is C++ and never sees Luau (docs/modding/architecture.md). It
// owns its state, lays widgets out and turns them into a DrawList of plain
// rectangles, text runs, images and item icons in window pixels; the
// application (src/app_mod_ui.inc) draws that list with the game's own
// renderer, fonts and icons, and feeds pointer, key and text input back in.
// So all of it - layout, theme layering, event routing, focus navigation,
// localization, hostile input - is tested without a window
// (tests/test_mod_ui.cpp).
//
// With no mod UI loaded nothing changes: every hook the game calls returns its
// input untouched, and the game's interface is pixel-identical to a build
// without this file.

#include "openminerz/modding/api.hpp"
#include "openminerz/modding/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::modding
{
class ModNetServer;
class ModNetClient;
} // namespace openminerz::modding

namespace openminerz::modding::ui
{

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

// The interface style in effect (Settings::ui_style, resolved): a theme can
// say something different for each.
enum class Style : std::uint8_t
{
    classic,
    modern,
    remastered
};
inline constexpr std::size_t style_count = 3U;
[[nodiscard]] std::string_view style_name(Style style) noexcept;
[[nodiscard]] std::optional<Style> parse_style(std::string_view name) noexcept;

// Straight (not premultiplied) RGBA, as SDL takes it.
struct Color final
{
    std::uint8_t r{255U};
    std::uint8_t g{255U};
    std::uint8_t b{255U};
    std::uint8_t a{255U};
    [[nodiscard]] constexpr bool operator==(const Color &) const noexcept = default;
};
// "#RGB", "#RRGGBB", "#RRGGBBAA", {r, g, b[, a]} or a table with r/g/b/a,
// each 0..255. Nothing for anything else.
[[nodiscard]] std::optional<Color> parse_color(const Value &value) noexcept;
[[nodiscard]] Color scale_alpha(Color color, float opacity) noexcept;

struct Rect final
{
    float x{};
    float y{};
    float width{};
    float height{};
    [[nodiscard]] constexpr float right() const noexcept { return x + width; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0.0F || height <= 0.0F; }
    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
    [[nodiscard]] Rect intersect(const Rect &other) const noexcept;
    [[nodiscard]] constexpr bool operator==(const Rect &) const noexcept = default;
};

struct Insets final
{
    float left{};
    float top{};
    float right{};
    float bottom{};
    [[nodiscard]] constexpr bool operator==(const Insets &) const noexcept = default;
};

// The game's two lettering families: the display face of menu rows and
// headings, and the text face of descriptions (app_menus.inc, MenuFace).
enum class Face : std::uint8_t
{
    display,
    text
};
enum class Align : std::uint8_t
{
    left,
    center,
    right
};

// A file inside a mod ("assets/ui/button.png"), always relative to the mod's
// root. normalize_asset_path() refuses absolute paths, "..", drive letters,
// empty segments and anything longer than 240 bytes, so an AssetRef can never
// name a file outside its mod.
struct AssetRef final
{
    std::string mod;
    std::string path;
    [[nodiscard]] bool empty() const noexcept { return path.empty(); }
    auto operator<=>(const AssetRef &) const = default;
};
[[nodiscard]] std::optional<std::string> normalize_asset_path(std::string_view path);

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------

// The game's own inks (MenuInk in app_menus.inc, RmTheme in
// app_remastered.inc), by the name a theme uses for each.
enum class ColorKey : std::uint8_t
{
    menu_row,      // "menu.row"      a menu row at rest
    menu_focus,    // "menu.focus"    what the focused row runs to
    menu_heading,  // "menu.heading"
    menu_text,     // "menu.text"     descriptions, values, most lettering
    menu_quiet,    // "menu.quiet"    secondary lines, hints
    menu_disabled, // "menu.disabled"
    menu_accent,   // "menu.accent"   bars, marks
    menu_warning,  // "menu.warning"
    hud_plate,     // "hud.plate"     the wash behind HUD elements
    hud_panel,     // "hud.panel"     a screen's panel
    hud_well,      // "hud.well"      a recessed area in a panel
    hud_slot,      // "hud.slot"      an empty slot
    hud_slot_full, // "hud.slot_full"
    hud_band,      // "hud.band"      the focus band
    hud_line,      // "hud.line"      thin rules
    hud_health,    // "hud.health"
    hud_stamina,   // "hud.stamina"
    hud_oxygen,    // "hud.oxygen"
    hud_good,      // "hud.good"
    hud_caution,   // "hud.caution"
    count
};
inline constexpr std::size_t color_key_count = static_cast<std::size_t>(ColorKey::count);
[[nodiscard]] std::string_view color_key_name(ColorKey key) noexcept;
[[nodiscard]] std::optional<ColorKey> parse_color_key(std::string_view name) noexcept;
using Palette = std::array<Color, color_key_count>;

// Nine-slice images a theme may put in place of the game's flat plates.
enum class ImageKey : std::uint8_t
{
    panel,        // "panel"         screens, dialogs and mod panels
    button,       // "button"        a button / plate button at rest
    button_focus, // "button_focus"  focused or pointed at
    dialog,       // "dialog"        the game's dialog panel
    slot,         // "slot"          an item slot
    count
};
inline constexpr std::size_t image_key_count = static_cast<std::size_t>(ImageKey::count);
[[nodiscard]] std::string_view image_key_name(ImageKey key) noexcept;

// The menus' cues (service_menu_sounds) and the screens' error cue.
enum class SoundKey : std::uint8_t
{
    click, // "Click": focus moves and choices
    popup, // "Popup": a dialog opens
    error, // "Error"
    count
};
inline constexpr std::size_t sound_key_count = static_cast<std::size_t>(SoundKey::count);
[[nodiscard]] std::string_view sound_key_name(SoundKey key) noexcept;

struct NineSlice final
{
    AssetRef image;
    // Edge widths in image pixels; the corners keep their size (times the
    // interface scale), the edges and the middle stretch.
    Insets slice{};
    bool operator==(const NineSlice &) const = default;
};
struct CursorSpec final
{
    AssetRef image;
    int hot_x{};
    int hot_y{};
    bool operator==(const CursorSpec &) const = default;
};
// A sound is a WAV inside the mod or another of the game's own cues.
struct SoundSpec final
{
    AssetRef file;
    std::string cue;
    bool operator==(const SoundSpec &) const = default;
};

// What one mod says, for every style ("any") or for one.
struct ThemeEntries final
{
    std::array<std::optional<Color>, color_key_count> colors{};
    std::array<std::optional<AssetRef>, 2U> fonts{}; // by Face
    std::array<std::optional<NineSlice>, image_key_count> images{};
    std::optional<CursorSpec> cursor;
    std::array<std::optional<SoundSpec>, sound_key_count> sounds{};
    // The game's own words, by the exact English text it shows ("Play
    // Offline"), and what to show instead.
    std::map<std::string, std::string, std::less<>> strings;
    [[nodiscard]] bool empty() const noexcept;
};

// Every mod's theme, layered by load order. A lookup asks the highest layer
// first and, within a layer, the current style's entries before the entries
// for every style; the first answer wins, and with none the caller keeps the
// game's own value. So a mod later in the load order restyles over an earlier
// one, and a style-specific line beats a general one of the same mod.
class ThemeStack final
{
  public:
    static constexpr std::size_t max_strings = 4096U;
    static constexpr std::size_t max_string_bytes = 1024U;

    // Merges a ui.theme{} table into the mod's layer:
    //   { colors = { ["menu.focus"] = "#FFAA00", ... },
    //     fonts = { display = "assets/fonts/Title.ttf", text = "..." },
    //     images = { button = { path = "...", slice = 8 }, panel = "..." },
    //     cursor = { path = "...", hotspot = {2, 2} } or "path",
    //     sounds = { click = "assets/ui/click.wav" or "cue:Craft" },
    //     strings = { ["Play Offline"] = "Begin" },
    //     classic = { ...the same keys... }, modern = {...}, remastered = {...} }
    // Unknown keys and bad values throw ScriptError and change nothing; so does
    // a file `exists` says is not in the mod (when given).
    using Exists = std::function<bool(const AssetRef &)>;
    void apply(std::string_view mod, std::uint32_t order, const Value &spec,
               const Exists &exists = {});
    void clear(std::string_view mod);
    void clear_all() noexcept;
    [[nodiscard]] bool empty() const noexcept { return layers_.empty(); }
    [[nodiscard]] bool has_strings() const noexcept { return string_count_ != 0U; }
    // Changes whenever anything does; caches key off it.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    [[nodiscard]] std::optional<Color> color(ColorKey key, Style style) const noexcept;
    [[nodiscard]] const AssetRef *font(Face face, Style style) const noexcept;
    [[nodiscard]] const NineSlice *image(ImageKey key, Style style) const noexcept;
    [[nodiscard]] const CursorSpec *cursor(Style style) const noexcept;
    [[nodiscard]] const SoundSpec *sound(SoundKey key, Style style) const noexcept;
    [[nodiscard]] const std::string *text(std::string_view english, Style style) const noexcept;
    // `base` with every colour the stack overrides replaced.
    [[nodiscard]] Palette resolve(Palette base, Style style) const noexcept;

  private:
    struct Layer final
    {
        std::string mod;
        std::uint32_t order{};
        ThemeEntries any;
        std::array<ThemeEntries, style_count> styles;
    };
    template <typename Pick>
    [[nodiscard]] auto find(Style style, Pick pick) const noexcept -> decltype(pick(ThemeEntries{}));
    void recount() noexcept;

    std::vector<Layer> layers_; // ascending load order
    std::uint64_t revision_{};
    std::size_t string_count_{};
};

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

// One run of rich text. `size` multiplies the widget's text size.
struct TextRun final
{
    std::string text;
    std::optional<Color> color;
    float size{1.0F};
    std::optional<Face> face;
    bool operator==(const TextRun &) const = default;
};
// Inline markup: "[c=#FF4040]red[/c] and [s=1.5]large[/s] and [f=display]a
// heading face[/f]"; "[[" is a literal "[". Unknown or unbalanced tags are
// kept as text, so markup never fails.
[[nodiscard]] std::vector<TextRun> parse_markup(std::string_view text);
// A string (with markup), or an array of strings and {text=, color=, size=,
// face=} runs. Throws ScriptError for anything else.
[[nodiscard]] std::vector<TextRun> runs_from_value(const Value &value);
[[nodiscard]] std::string plain_text(std::span<const TextRun> runs);

// SpriteFont / FreeType measurement, answered by the application with the
// lettering it will draw (app_menus.inc: menu_width).
class TextMeasure
{
  public:
    virtual ~TextMeasure() = default;
    // Width in window pixels of one line whose capitals are `cap` pixels tall.
    [[nodiscard]] virtual float width(Face face, float cap, std::string_view text) const = 0;
};
// Distance from one line's capitals to the next line's (menu_line()).
[[nodiscard]] constexpr float line_pitch(float cap) noexcept
{
    return cap * 1.66F;
}

struct PlacedText final
{
    std::string text;
    float x{};   // left edge, window pixels
    float top{}; // top of the capitals
    float cap{};
    Face face{Face::text};
    Color color{};
};
struct TextBlock final
{
    std::vector<PlacedText> pieces;
    float width{};
    float height{};
    std::size_t lines{};
    bool truncated{};
};
// Lays runs out from (0, 0): broken at spaces to `max_width` pixels (none
// when <= 0), at most `max_lines` lines (0: no limit; the last one shortened
// with "..."), aligned within `max_width` when one is given. Lines align on the
// foot of their capitals.
[[nodiscard]] TextBlock layout_text(std::span<const TextRun> runs, Face face, float cap,
                                    Color color, float max_width, std::size_t max_lines,
                                    Align align, const TextMeasure &measure);

// ---------------------------------------------------------------------------
// Localization: lang/<code>.lang inside a mod
// ---------------------------------------------------------------------------
//
//   # comment
//   quest.start.title = The Old Mine
//   quest.start.body  = Find {name} before nightfall.\nTake a torch.
//
// Keys are [A-Za-z0-9_.-]+. Values run to the end of the line; \n, \t and \\
// are escapes. Placeholders are {name} (from a table of arguments) or {1}..{9}
// (from an array); one with no argument is left as it is.

using StringTable = std::map<std::string, std::string, std::less<>>;
inline constexpr std::size_t max_lang_entries = 16384U;
inline constexpr std::size_t max_lang_bytes = 1024U * 1024U;
[[nodiscard]] bool parse_lang_file(std::string_view text, StringTable &table, std::string &error);
[[nodiscard]] std::string format_text(std::string_view pattern, const Value &args);
// "pt-BR" -> {"pt-br", "pt", "en"}; "en" -> {"en"}.
[[nodiscard]] std::vector<std::string> language_fallbacks(std::string_view language);

class Localization final
{
  public:
    void set_language(std::string_view code);
    [[nodiscard]] const std::string &language() const noexcept { return language_; }
    // Reads a mod's tables on first use through `read` (mod, "lang/xx.lang").
    using Reader = std::function<std::optional<std::string>(std::string_view, std::string_view)>;
    void set_reader(Reader reader) { reader_ = std::move(reader); }
    void add_table(std::string_view mod, std::string_view language, StringTable table);
    void forget(std::string_view mod);
    void clear() noexcept;
    // The calling mod's string for `key` in the first language of the
    // fallback chain that has it, formatted with `args`; the key itself when
    // no table has it.
    [[nodiscard]] std::string text(std::string_view mod, std::string_view key, const Value &args);
    [[nodiscard]] std::vector<std::string> errors() const { return errors_; }

  private:
    const StringTable *table(std::string_view mod, std::string_view language);

    std::string language_{"en"};
    Reader reader_;
    // mod -> language -> table; a language read and found absent is kept as
    // an empty table so the file is read once.
    std::map<std::string, std::map<std::string, StringTable, std::less<>>, std::less<>> tables_;
    std::vector<std::string> errors_;
};

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// One thing to draw, in window pixels. The application renders the list in
// order with its own primitives (fill, stroke, menu_label, the item atlas).
struct DrawCmd final
{
    enum class Kind : std::uint8_t
    {
        fill,
        frame,  // a border `thickness` pixels wide inside rect
        image,  // a mod image, or a game sprite ("hud/HudGrid") when `sprite`
        text,   // one piece of one line, capitals' top at rect.y
        item,   // the game's icon for `item`, with its count and wear bar
        clip_push,
        clip_pop
    };
    Kind kind{Kind::fill};
    Rect rect{};
    Color color{};
    float thickness{1.0F};
    AssetRef image;
    std::string sprite;
    bool nine{};
    Insets slice{};
    float slice_scale{1.0F}; // window pixels per image pixel at the corners
    std::string text;
    Face face{Face::text};
    float cap{};
    bool outline{};
    int item{-1};
    int count{};
    float durability{1.0F};
};
using DrawList = std::vector<DrawCmd>;

// What one frame is drawn for.
struct FrameContext final
{
    int width{1280};
    int height{720};
    float ui_scale{1.0F};
    float safe_area{1.0F};
    Style style{Style::remastered};
    // Extra factor on every capital height: Modern's taller condensed face
    // (14/11) and the Larger Text setting, as rm_cap() applies them.
    float text_factor{1.0F};
    double time{};
    bool reduced_motion{};
    bool gamepad{};
    // The game's inks with the theme applied (ThemeStack::resolve).
    Palette palette{};
    // The theme's nine-slice images for this style (null: flat plates).
    std::array<const NineSlice *, image_key_count> images{};
};

// The canvas mod UI is laid out on: the Remastered HUD's (remastered_layout.hpp)
// - 1280x720 units on a 16:9 window at 100% interface scale, the window in
// those units, anchored to the screen margins - so a mod panel is the same
// share of the picture at 720p, 1440p and 5120x1440 and follows Interface
// scale and Screen margins like the game's own.
struct Canvas final
{
    float scale{1.0F};   // window pixels per unit
    float width{1280.0F};
    float height{720.0F};
    Rect safe{0.0F, 0.0F, 1280.0F, 720.0F};
    // The middle 16:9 part of `safe` (all of it on 16:9 and narrower): where
    // HUD elements anchored to frame = "focus" go on a very wide screen.
    Rect focus{0.0F, 0.0F, 1280.0F, 720.0F};
};
[[nodiscard]] Canvas make_canvas(const FrameContext &frame) noexcept;

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

enum class WidgetKind : std::uint8_t
{
    panel,      // a box; children placed by their anchors
    label,      // rich text
    image,      // a mod image or a game sprite, optionally nine-sliced
    button,
    toggle,
    slider,
    text_input,
    list,       // a vertical scroll view
    progress,   // a bar; `stat` binds it to a replicated stat
    item_slot,  // the game's icon for an item, with count
    stack,      // children one after another (direction, gap, align)
    grid,       // children in `columns` columns
    spacer,
    count
};
[[nodiscard]] std::string_view widget_kind_name(WidgetKind kind) noexcept;
[[nodiscard]] std::optional<WidgetKind> parse_widget_kind(std::string_view name) noexcept;

// Where a widget sits in its parent: the point of the parent's content box
// the widget's same point is put on, then moved by (x, y).
enum class Anchor : std::uint8_t
{
    top_left,
    top,
    top_right,
    left,
    center,
    right,
    bottom_left,
    bottom,
    bottom_right
};
[[nodiscard]] std::optional<Anchor> parse_anchor(std::string_view name) noexcept;
[[nodiscard]] float anchor_x(Anchor anchor) noexcept; // 0, 0.5 or 1
[[nodiscard]] float anchor_y(Anchor anchor) noexcept;

// A width or height: a number of units, a share of the parent ("50%"),
// "fill" (all of what is left), or nothing (fit the content).
struct Length final
{
    enum class Mode : std::uint8_t
    {
        automatic,
        units,
        fraction,
        fill
    };
    Mode mode{Mode::automatic};
    float value{};
    bool operator==(const Length &) const = default;
};

// Something the service itself does when a widget is used (the built-in
// dialogue, journal, settings and bindings screens), instead of a script
// callback.
struct InternalAction final
{
    enum class Kind : std::uint8_t
    {
        none,
        close_screen,
        dialogue_choice, // index: the choice
        journal_select,  // index: the quest
        config_set,      // data: "mod\nkey"
        binding_capture, // data: the action
        binding_reset    // data: the action
    };
    Kind kind{Kind::none};
    std::int64_t index{};
    std::string data;
    bool operator==(const InternalAction &) const = default;
};

struct Widget final
{
    std::uint64_t id{};
    std::uint64_t parent{};
    std::vector<std::uint64_t> children;
    WidgetKind kind{WidgetKind::panel};
    std::string name;      // `id` in the spec, for ui.find
    std::string mod;       // owner
    Side side{Side::client};

    // Layout, in canvas units.
    Anchor anchor{Anchor::top_left};
    float x{};
    float y{};
    Length width;
    Length height;
    Insets padding{};
    float gap{};
    int columns{1};
    bool horizontal{};       // stack direction
    Align align{Align::left}; // stack cross-axis / text alignment
    bool stretch{};          // stack children take the cross-axis size
    bool visible{true};
    bool enabled{true};
    float opacity{1.0F};

    // Look. Unset colours come from the theme; a colour may also name one of
    // the theme's inks ("menu.heading"), resolved every frame.
    std::optional<ColorKey> color_key;
    std::optional<ColorKey> background_key;
    std::optional<ColorKey> border_key;
    enum class Fill : std::uint8_t
    {
        none,
        color, // `background`
        panel, // the theme's panel (its nine-slice image when it has one)
        plate, // the HUD's black wash
        well,
        slot
    };
    Fill fill{Fill::none};
    std::optional<Color> color;      // text / fill / tint
    std::optional<Color> background;
    std::optional<Color> border;
    float border_width{};
    AssetRef image;
    std::string sprite; // a game sprite
    bool nine{};
    Insets slice{};
    std::vector<TextRun> text;
    float text_size{};  // capital height in units; 0 picks the kind's default
    Face face{Face::text};
    bool outline{};
    bool wrap{true};
    std::size_t max_lines{};

    // Values.
    bool checked{};
    double value{};
    double minimum{};
    double maximum{1.0};
    double step{};
    std::string input;
    std::string placeholder;
    std::size_t max_length{256U};
    int item{-1};
    int count{};
    std::string stat;
    bool show_value{};
    float scroll{}; // list: units scrolled

    Callback on_click;
    Callback on_change;
    Callback on_hover;
    Callback on_focus;
    Callback on_submit;
    InternalAction action;
    bool focusable{};

    // Results of the last layout, in window pixels.
    Rect frame{};
    Rect clip{};    // what is visible of it (inside every scrolling parent)
    float content_height{}; // list: everything inside, in units
};

// A Handle naming a widget (HandleKind::widget).
[[nodiscard]] constexpr Handle widget_handle(std::uint64_t id) noexcept
{
    return {HandleKind::widget, id};
}

struct Owner final
{
    std::string mod;
    Side side{Side::client};
};

// Every widget of every screen and HUD element. Trees are built from Value
// specs (Luau tables) and capped: a hostile mod cannot exhaust memory or stack
// with a huge or deep tree, a huge string or thousands of widgets.
class WidgetStore final
{
  public:
    static constexpr std::size_t max_widgets = 8192U;
    static constexpr std::size_t max_depth = 24U;
    static constexpr std::size_t max_children = 1024U;
    static constexpr std::size_t max_text_bytes = 8192U;
    static constexpr std::size_t max_runs = 128U;

    // What a spec may name: files inside the owning mod, the game's items.
    struct Checks final
    {
        std::function<bool(const AssetRef &)> asset;
        std::function<bool(int)> item;
    };
    void set_checks(Checks checks) { checks_ = std::move(checks); }

    // Builds a tree from `spec` ({kind = "panel", children = {...}, ...}).
    // Callbacks the tree keeps are appended to `adopted`. Throws ScriptError
    // and creates nothing when the spec is bad or a cap would be exceeded.
    std::uint64_t build(const Value &spec, const Owner &owner, std::vector<Callback> &adopted);
    std::uint64_t add_child(std::uint64_t parent, const Value &spec, std::vector<Callback> &adopted);
    // Sets the properties named in `props`; `replaced` receives the callbacks
    // the change let go of (the caller releases them).
    void update(std::uint64_t id, const Value &props, std::vector<Callback> &adopted,
                std::vector<Callback> &replaced);
    [[nodiscard]] Value get(std::uint64_t id, std::string_view property) const;
    // Removes the widget and everything under it; their callbacks go to
    // `released`.
    void destroy(std::uint64_t id, std::vector<Callback> &released);
    void clear_children(std::uint64_t id, std::vector<Callback> &released);
    void clear(std::vector<Callback> &released);

    [[nodiscard]] Widget *find(std::uint64_t id) noexcept;
    [[nodiscard]] const Widget *find(std::uint64_t id) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> find_named(std::uint64_t root,
                                                          std::string_view name) const;
    [[nodiscard]] std::uint64_t root_of(std::uint64_t id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return widgets_.size(); }
    [[nodiscard]] std::size_t depth_below(std::uint64_t id) const noexcept;

    // Lays the tree under `root` out inside `area` (canvas units) and records
    // every widget's window frame.
    struct LayoutContext final
    {
        const Canvas *canvas{};
        const TextMeasure *measure{};
        const FrameContext *frame{};
        // Replicated or local stat values for `stat`-bound bars.
        std::function<std::optional<std::pair<double, double>>(std::string_view)> stat;
    };
    void layout(std::uint64_t root, const Rect &area, const LayoutContext &context);
    // Draws the tree; `focused` and `hovered` get the focus band.
    void draw(std::uint64_t root, const LayoutContext &context, std::uint64_t focused,
              std::uint64_t hovered, bool editing, DrawList &out) const;
    // The deepest visible, enabled widget under the point that takes the
    // pointer (a button, a slot with on_click, a list for the wheel...).
    [[nodiscard]] std::optional<std::uint64_t> hit(std::uint64_t root, float x, float y) const;
    // Focusable widgets in reading order.
    [[nodiscard]] std::vector<std::uint64_t> focusables(std::uint64_t root) const;
    // Scrolls every list that holds `id` until it is in view.
    void reveal(std::uint64_t id);

  private:
    struct Measured final
    {
        float width{};
        float height{};
    };
    std::uint64_t create(const Value &spec, const Owner &owner, std::uint64_t parent,
                         std::size_t depth, std::vector<Callback> &adopted,
                         std::vector<std::uint64_t> &created);
    void apply(Widget &widget, const Value &props, bool creating, std::vector<Callback> &adopted,
               std::vector<Callback> &replaced);
    [[nodiscard]] Measured measure(const Widget &widget, float available_width,
                                   float available_height, const LayoutContext &context) const;
    void arrange(Widget &widget, const Rect &units, const Rect &clip, const LayoutContext &context);
    void draw_one(const Widget &widget, const LayoutContext &context, std::uint64_t focused,
                  std::uint64_t hovered, bool editing, DrawList &out) const;
    void count_subtree(std::uint64_t id, std::size_t &count) const;

    std::unordered_map<std::uint64_t, Widget> widgets_;
    std::uint64_t next_id_{1U};
    Checks checks_;
    float last_scale_{1.0F};
};

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// What the game's screens read, mapped by the application from keys (arrows
// and W/A/S/D, Enter/Space, Escape, Tab) and the gamepad (D-pad, A, B, LB/RB).
enum class NavKey : std::uint8_t
{
    up,
    down,
    left,
    right,
    activate,
    back,
    next,     // Tab
    previous, // Shift+Tab
    backspace,
    delete_key,
    home,
    end
};

// A script call the routing produced, run after the input step (never while
// a tree is being walked, since a callback may change or destroy it).
struct Dispatch final
{
    Callback callback;
    Handle widget;
    std::string event; // "click", "change", "hover", "focus", "submit"
    Value value;
    InternalAction action;
};

// The interaction state of one interactive surface (a screen or dialog).
struct Surface final
{
    std::uint64_t root{};
    std::uint64_t focused{};
    std::uint64_t hovered{};
    std::uint64_t pressed{};
    bool editing{};   // a text input has the keyboard
    bool dragging{};  // a slider follows the pointer
    bool pointer_moved{}; // the pointer, not keys, chose the focus last
};

// Routing, shared by every surface. Each returns true when the input was used.
bool route_pointer_move(WidgetStore &store, Surface &surface, float x, float y,
                        std::vector<Dispatch> &out);
bool route_pointer_button(WidgetStore &store, Surface &surface, float x, float y, bool down,
                          std::vector<Dispatch> &out);
bool route_wheel(WidgetStore &store, Surface &surface, float x, float y, float steps);
bool route_key(WidgetStore &store, Surface &surface, NavKey key, std::vector<Dispatch> &out);
bool route_text(WidgetStore &store, Surface &surface, std::string_view text,
                std::vector<Dispatch> &out);
// Moves the focus to the nearest focusable widget in that direction (the
// widget whose centre is closest along it, preferring ones in line).
bool move_focus(WidgetStore &store, Surface &surface, NavKey direction, std::vector<Dispatch> &out);
void set_focus(WidgetStore &store, Surface &surface, std::uint64_t id, std::vector<Dispatch> &out);

// ---------------------------------------------------------------------------
// Key bindings
// ---------------------------------------------------------------------------
//
// input.bind("journal", "J", fn) gives the mod action "<mod>.journal" the key J.
// A key the game itself reads in the world is never taken: the binding is
// kept, shown as in conflict and left without a key until the player gives it
// one. Two mods asking for the same key: the earlier in load order keeps it.
// The player's own choices are saved per machine and win over defaults.

struct KeyBinding final
{
    std::string action;      // "<mod>.<name>"
    std::string mod;
    std::string label;
    std::string default_key; // canonical key name, "" for none
    std::string key;         // in effect, "" when unbound
    bool user_set{};         // the player chose it
    std::string conflict;    // why the default could not be used
    Callback callback;
    std::uint32_t order{};
};

class KeyBindings final
{
  public:
    static constexpr std::size_t max_bindings = 512U;

    // The keys the game reads in the world (process_world_key and the held
    // movement keys); mods may not take them.
    [[nodiscard]] static bool game_key(std::string_view key) noexcept;
    // "j" -> "J", "f6" -> "F6", "numpad 1" -> "Keypad 1": the case-insensitive
    // name SDL_GetKeyName gives, as the app canonicalises it.
    using Canonical = std::function<std::string(std::string_view)>;
    void set_canonical(Canonical canonical) { canonical_ = std::move(canonical); }

    // Returns the callback the binding replaced (the caller releases it).
    Callback bind(std::string_view mod, std::uint32_t order, std::string_view name,
                  std::string_view default_key, std::string_view label, Callback callback);
    Callback unbind(std::string_view action);
    void forget_mod(std::string_view mod, std::vector<Callback> &released);
    void clear(std::vector<Callback> &released);
    // The player's choice; "" clears the key. False when the key is the
    // game's or another action's.
    bool assign(std::string_view action, std::string_view key, std::string &error);
    void reset(std::string_view action);

    [[nodiscard]] const KeyBinding *find(std::string_view action) const noexcept;
    [[nodiscard]] const KeyBinding *for_key(std::string_view key) const noexcept;
    [[nodiscard]] const std::vector<KeyBinding> &all() const noexcept { return bindings_; }
    [[nodiscard]] bool empty() const noexcept { return bindings_.empty(); }

    // The player's choices: "action=key" lines.
    [[nodiscard]] std::string save() const;
    void load(std::string_view text);

  private:
    void resolve();
    [[nodiscard]] std::string canonical(std::string_view key) const;

    std::vector<KeyBinding> bindings_;
    std::map<std::string, std::string, std::less<>> user_; // action -> key ("" unbound)
    Canonical canonical_;
};

// ---------------------------------------------------------------------------
// The built-in HUD's elements a mod may hide or move
// ---------------------------------------------------------------------------
//
// Hiding works in every style (it is the same switch the HUD settings page
// flips, for this frame only, never saved). Moving works where the HUD is laid
// out by rectangles - the Remastered style; the Classic and Modern HUDs are
// exact copies of the retail draw lists and stay where the originals put them.
enum class HudElement : std::uint8_t
{
    crosshair,
    vitals,        // health and stamina
    hotbar,        // the item tray(s)
    item_name,     // the held item's line
    block_name,    // the aimed block's name
    compass,       // Remastered
    clock,         // Remastered
    distance,
    oxygen,
    chat,
    player_list,
    notifications, // Remastered notices
    damage,        // hurt wash and damage arrows
    scope,
    death,         // the death screen
    server,        // server card
    coordinates,
    count
};
inline constexpr std::size_t hud_element_count = static_cast<std::size_t>(HudElement::count);
[[nodiscard]] std::string_view hud_element_name(HudElement element) noexcept;
[[nodiscard]] std::optional<HudElement> parse_hud_element(std::string_view name) noexcept;
[[nodiscard]] bool hud_element_movable(HudElement element) noexcept;

// ---------------------------------------------------------------------------
// Cross-area interfaces
// ---------------------------------------------------------------------------

// Mod stats the local player has (hunger, thirst, ...), replicated by the
// Gameplay/Network services. The UI reads them every frame it draws a bar.
struct StatValue final
{
    double value{};
    double maximum{1.0};
};
class StatSource
{
  public:
    virtual ~StatSource() = default;
    [[nodiscard]] virtual std::optional<StatValue> stat(std::string_view name) const = 0;
};

// The mod channel, as the UI uses it. Messages are Values on the reserved
// channel name "omz.ui" (the Network service frames, limits and routes them;
// offline and on a listen host the local player's are delivered in process).
class UiChannel
{
  public:
    virtual ~UiChannel() = default;
    // Client: to the host's UI service.
    virtual void send_to_server(const Value &message) = 0;
    // Server: to one player's client.
    virtual void send_to_player(Handle player, const Value &message) = 0;
    // Server: everyone in the session (ui.toast(nil, ...)).
    [[nodiscard]] virtual std::vector<Handle> players() const = 0;
};
inline constexpr std::string_view ui_channel_name = "omz.ui";

class UiService;

// UiChannel over the Network service's mod channel (modding/net.hpp): the
// engine's own slot (mod id "") and the RPC name "omz.ui", so it exists only
// in a session whose manifest negotiated server mods, and the channel's
// per-peer limits and bounded decoding apply before the service sees a byte.
// Either end may be null (no authority here / no client VM). Registers its
// handlers on construction and removes them on destruction.
class ModNetUiChannel final : public UiChannel
{
  public:
    ModNetUiChannel(UiService &service, ModNetServer *server, ModNetClient *client,
                    std::function<double()> clock);
    ~ModNetUiChannel() override;
    ModNetUiChannel(const ModNetUiChannel &) = delete;
    ModNetUiChannel &operator=(const ModNetUiChannel &) = delete;

    void send_to_server(const Value &message) override;
    void send_to_player(Handle player, const Value &message) override;
    [[nodiscard]] std::vector<Handle> players() const override;

  private:
    ModNetServer *server_{};
    ModNetClient *client_{};
};

// Mod settings declared with mod.config.define (runtime.hpp); the Add-Ons
// screen draws a page for each mod that has any.
class ConfigSource
{
  public:
    virtual ~ConfigSource() = default;
    [[nodiscard]] virtual std::vector<ConfigSetting> settings() const = 0;
    virtual bool set(std::string_view mod, std::string_view key, const Value &value,
                     std::string &error) = 0;
};

// What the service needs from the game. Implemented by src/app_mod_ui.inc.
class UiBackend
{
  public:
    virtual ~UiBackend() = default;
    [[nodiscard]] virtual Style style() const = 0;
    // A mod's place in the load order (later restyles over earlier).
    [[nodiscard]] virtual std::uint32_t load_order(std::string_view mod) const = 0;
    // A display name for the mod ("" uses the id).
    [[nodiscard]] virtual std::string mod_title(std::string_view mod) const
    {
        (void)mod;
        return {};
    }
    // The file exists inside the mod (fonts, images, cursors, sounds).
    [[nodiscard]] virtual bool asset_exists(const AssetRef &asset) const = 0;
    // A text file inside the mod, at most `limit` bytes (lang files).
    [[nodiscard]] virtual std::optional<std::string> read_mod_text(std::string_view mod,
                                                                   std::string_view path,
                                                                   std::size_t limit) const = 0;
    // Per-machine files of the UI service itself (key bindings).
    [[nodiscard]] virtual std::optional<std::string> read_local(std::string_view name) const = 0;
    virtual void write_local(std::string_view name, std::string_view text) = 0;
    // Whether the game has an item with this id (InventoryItemId), and its name.
    [[nodiscard]] virtual bool item_exists(int item) const = 0;
    [[nodiscard]] virtual std::string item_name(int item) const = 0;
    // The canonical name of a key ("" when there is no such key).
    [[nodiscard]] virtual std::string canonical_key(std::string_view key) const = 0;
    // A world is being played and this process simulates it alone (single
    // player): a pausing screen freezes it.
    [[nodiscard]] virtual bool offline() const = 0;
    // The local player, as the server-side services name players.
    [[nodiscard]] virtual Handle local_player() const = 0;
    virtual void play(SoundKey sound) = 0;
};

// ---------------------------------------------------------------------------
// The service
// ---------------------------------------------------------------------------

enum class MenuSlot : std::uint8_t
{
    main,   // the title screen, before Exit
    pause,  // the in-game menu, before Main Menu
    addons  // the Add-Ons screen, after its own rows
};
inline constexpr std::size_t menu_slot_count = 3U;
[[nodiscard]] std::optional<MenuSlot> parse_menu_slot(std::string_view name) noexcept;

struct MenuItem final
{
    std::uint64_t id{};
    MenuSlot slot{MenuSlot::main};
    std::string label;
    std::string description;
    std::string mod;
    Callback callback;
    // Built-in Add-Ons rows: open a mod's settings page / the bindings page.
    std::string config_mod;
    bool bindings{};
};

// What a server sends a client (op = ...) and what a client answers. Shapes:
//   {op="dialogue", id, speaker?, text, portrait_item?, choices={{id, text}...}, pause?}
//   {op="close_dialogue", id}
//   {op="toast", title, text?, item?, seconds?, color?}
//   {op="title", title, subtitle?, fade_in?, stay?, fade_out?}
//   {op="objective", id, text, progress?, done?}   {op="clear_objective", id}
//   {op="journal", quests={{id, title, text?, state?, objectives={{text, done?, progress?}}}...}}
//   {op="open_journal"}
//   client -> server: {op="choice", id, choice}   {op="dialogue_closed", id}
// Every field is bounded (see the limits below) and checked before use.
inline constexpr std::size_t max_dialogue_choices = 9U;
inline constexpr std::size_t max_quests = 64U;
inline constexpr std::size_t max_objectives = 32U;
inline constexpr std::size_t max_toasts = 6U;
inline constexpr std::size_t max_message_text = 4096U;
inline constexpr std::size_t max_id_bytes = 64U;
// Client -> server UI messages one player may send per second.
inline constexpr int max_client_messages_per_second = 20;

class UiService final
{
  public:
    explicit UiService(UiBackend &backend);
    ~UiService();
    UiService(const UiService &) = delete;
    UiService &operator=(const UiService &) = delete;

    // Registers the `ui` and `input` libraries. `host` must outlive the
    // service; its teardown listener drops a side's widgets and callbacks.
    void register_api(ApiRegistry &registry, ScriptHost &host);
    void set_channel(UiChannel *channel) noexcept { channel_ = channel; }
    void set_stats(const StatSource *stats) noexcept { stats_ = stats; }
    void set_config(ConfigSource *config) noexcept { config_ = config; }
    void set_language(std::string_view code) { localization_.set_language(code); }

    // ---- What the game asks every frame (all cheap when nothing is loaded)
    // Anything at all to draw or route besides the theme.
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const ThemeStack &theme() const noexcept { return theme_; }
    // A mod screen or dialog is open: it has the keyboard and the pointer.
    [[nodiscard]] bool screen_open() const noexcept { return !screens_.empty(); }
    // The top screen asks for the world to stop (honoured offline only).
    [[nodiscard]] bool pauses_world() const noexcept;
    [[nodiscard]] bool hud_hidden(HudElement element) const noexcept
    {
        return hud_hidden_[static_cast<std::size_t>(element)];
    }
    [[nodiscard]] bool any_hud_hidden() const noexcept;
    // Canvas units to move a Remastered HUD element by.
    [[nodiscard]] std::pair<float, float> hud_offset(HudElement element) const noexcept
    {
        return hud_offsets_[static_cast<std::size_t>(element)];
    }
    [[nodiscard]] bool any_hud_moved() const noexcept;

    void update(double seconds);
    // The mod HUD: elements, stat bars, the objective tracker. Drawn after the
    // game's HUD, only while it is shown.
    void draw_hud(const FrameContext &frame, const TextMeasure &measure, DrawList &out);
    // Toasts and titles, over the HUD.
    void draw_notices(const FrameContext &frame, const TextMeasure &measure, DrawList &out);
    // Screens and dialogs, over everything.
    void draw_screens(const FrameContext &frame, const TextMeasure &measure, DrawList &out);

    // ---- Input to the top screen. True when used.
    bool pointer_move(float x, float y);
    bool pointer_button(float x, float y, bool down);
    bool wheel(float x, float y, float steps);
    bool key(NavKey key);
    bool text(std::string_view text);
    // A text input has the keyboard (the app starts SDL text input).
    [[nodiscard]] bool editing_text() const noexcept;
    // A key pressed in the world: runs the mod binding on it, if any. Game
    // keys never reach here (the app only offers keys it does not use).
    bool world_key(std::string_view canonical_key);
    // The bindings page waits for a key; the app sends the next one here.
    [[nodiscard]] bool capturing_key() const noexcept { return !capture_action_.empty(); }
    void capture_key(std::string_view canonical_key);

    // ---- Menus
    [[nodiscard]] std::uint64_t menu_revision() const noexcept { return menu_revision_; }
    // The rows to add to a menu: the mods' own, and on the Add-Ons screen one
    // per mod settings page and one for the key bindings page.
    [[nodiscard]] std::vector<MenuItem> menu_items(MenuSlot slot) const;
    void activate_menu_item(MenuSlot slot, std::size_t index);
    // 1..9 on a story dialogue picks that choice.
    bool choice_key(int number);
    void open_bindings_screen();
    void open_config_screen(std::string_view mod);
    [[nodiscard]] const KeyBindings &bindings() const noexcept { return bindings_; }

    // ---- Story content driven from C++ (the Gameplay service's replicated
    // NPC dialogue, quest log and HUD stats: modding/gameplay.hpp's
    // ClientDialogue, ClientQuest and ClientStat, converted by the app).
    struct NativeDialogue final
    {
        std::uint64_t session{};
        std::string speaker;
        std::string text;
        int portrait_item{-1};
        std::vector<std::string> choices; // empty: one "Continue"
        std::function<void(std::size_t)> choose; // the answer
        std::function<void()> closed;            // closed unanswered
    };
    void show_native_dialogue(NativeDialogue dialogue);
    void close_native_dialogue(std::uint64_t session);
    [[nodiscard]] std::uint64_t native_dialogue_session() const noexcept;
    // The journal from a list of quests ({id, title, text, state, objectives}).
    void set_quests(const Value &quests) { set_journal(quests); }
    struct HudStat final
    {
        std::string name;
        std::string label;
        std::optional<Color> color;
        double value{};
        double minimum{};
        double maximum{100.0};
    };
    // Stats a definition marked `hud`: drawn as bars bottom left, above the
    // tray's height, whatever the interface style.
    void set_hud_stats(std::vector<HudStat> stats) { hud_stats_ = std::move(stats); }
    // (mod-ai) Boss health bars (Gameplay's client_bosses() near the player):
    // drawn top centre, at most three, whatever the interface style.
    struct BossBar final
    {
        std::string label;
        std::string phase;
        double fraction{1.0};
        std::optional<Color> color;
    };
    void set_boss_bars(std::vector<BossBar> bars) { boss_bars_ = std::move(bars); }
    [[nodiscard]] const std::vector<BossBar> &boss_bars() const noexcept { return boss_bars_; }

    // ---- The mod channel ("omz.ui")
    void receive_from_server(const Value &message);
    void receive_from_client(Handle player, const Value &message, double now);
    // The player left: forget their open dialogues.
    void forget_player(Handle player);

    // ---- Lifecycle
    // Drops everything one side's scripts made (VM teardown).
    void clear_side(Side side);
    void clear_all();

    // For tests and the diagnostic driver: run queued script calls now.
    void flush();
    [[nodiscard]] const WidgetStore &widgets() const noexcept { return widgets_; }
    [[nodiscard]] WidgetStore &widgets() noexcept { return widgets_; }
    [[nodiscard]] std::vector<std::uint64_t> screen_roots() const;
    [[nodiscard]] std::vector<std::uint64_t> hud_roots() const;
    [[nodiscard]] const Surface *top_surface() const noexcept;
    [[nodiscard]] std::size_t toast_count() const noexcept { return toasts_.size(); }
    [[nodiscard]] Localization &localization() noexcept { return localization_; }

    struct Screen;
    struct HudItem;
    struct Toast;
    struct Objective;
    struct Quest;
    struct ServerDialogue;

  private:
    friend class Api;

    std::uint64_t open_screen(const Value &spec, const Owner &owner, bool dialog);
    void close_screen(std::uint64_t root, bool notify);
    std::uint64_t add_hud(const Value &spec, const Owner &owner);
    void show_toast(const Value &spec, const std::string &mod);
    void show_title(const Value &spec);
    void set_objective(const Value &spec, const std::string &mod, Side side);
    void open_dialogue(const Value &spec, const Owner &owner, bool from_server);
    void close_dialogue(std::string_view id);
    void set_journal(const Value &quests);
    void open_journal();
    void dispatch(std::vector<Dispatch> &dispatches);
    void run_internal(const Dispatch &dispatch);
    void to_server(const Value &message);
    void to_players(const std::vector<Handle> &players, const Value &message);
    [[nodiscard]] std::vector<Handle> session_players() const;
    void release(std::vector<Callback> &callbacks) noexcept;
    void release(Callback callback) noexcept;
    void destroy_tree(std::uint64_t root);
    void layout_screen(Screen &screen, const Canvas &canvas, const WidgetStore::LayoutContext &context);
    [[nodiscard]] std::optional<std::pair<double, double>> stat_value(std::string_view name) const;
    void save_bindings();
    void rebuild_bindings_screen();
    void rebuild_config_screen();
    [[nodiscard]] Screen *top_screen() noexcept;

    UiBackend &backend_;
    ScriptHost *host_{};
    UiChannel *channel_{};
    const StatSource *stats_{};
    ConfigSource *config_{};

    ThemeStack theme_;
    Localization localization_;
    WidgetStore widgets_;
    KeyBindings bindings_;
    std::vector<std::unique_ptr<Screen>> screens_;
    std::vector<std::unique_ptr<HudItem>> hud_;
    std::deque<std::unique_ptr<Toast>> toasts_;
    struct TitleState final
    {
        std::vector<TextRun> title;
        std::vector<TextRun> subtitle;
        double fade_in{0.4};
        double stay{2.5};
        double fade_out{0.6};
        double age{};
    };
    std::optional<TitleState> title_;
    std::vector<std::unique_ptr<Objective>> objectives_;
    std::vector<std::unique_ptr<Quest>> quests_;
    std::array<bool, hud_element_count> hud_hidden_{};
    std::array<std::pair<float, float>, hud_element_count> hud_offsets_{};
    std::vector<std::unique_ptr<MenuItem>> menu_items_;
    std::uint64_t menu_revision_{1U};
    std::uint64_t next_menu_id_{1U};
    std::map<std::string, StatValue, std::less<>> local_stats_;
    std::vector<HudStat> hud_stats_;
    std::vector<BossBar> boss_bars_; // (mod-ai)
    std::uint64_t native_session_{};
    std::function<void(std::size_t)> native_choose_;
    std::function<void()> native_closed_;
    std::vector<Dispatch> pending_;
    bool flushing_{};
    // Server side: what each player was shown and may answer.
    std::map<std::uint64_t, std::vector<std::unique_ptr<ServerDialogue>>> server_dialogues_;
    std::map<std::uint64_t, std::pair<double, int>> client_rate_;
    std::string capture_action_;
    std::uint64_t bindings_root_{};
    std::uint64_t config_root_{};
    std::string config_mod_;
    std::string dialogue_id_;
    bool dialogue_from_server_{};
    std::uint64_t dialogue_root_{};
    std::uint64_t journal_root_{};
    std::size_t journal_selected_{};
    double time_{};
    // Expires with the service: the runtime's teardown listener checks it.
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
};

// The entry point of architecture.md's shape. The service outlives the
// registry's use of it.
void register_ui_api(ApiRegistry &registry, ScriptHost &host, UiService &service);

} // namespace openminerz::modding::ui
