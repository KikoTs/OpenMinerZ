#pragma once

#include "openminerz/settings.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The in-world HUD of the two retail builds, as a draw list.
//
// CastleMiner Z's InGameHUD.OnDraw is a fixed sequence of SpriteBatch calls
// whose rectangles are plain arithmetic on the canvas size, the sprite sizes
// and a few SpriteFont measurements. layout() is that arithmetic for 1.6.3.8
// ("Classic") and 1.9.9.8 ("Modern"), in the original's order and with its
// integer truncations, so the list it returns is what the original would have
// submitted for the same state. It draws nothing itself: the application
// renders the list, and tests compare it record for record with lists captured
// from the shipped assemblies by tools/OriginalClientInterop/HudParityProbe.
//
// Which build's HUD is laid out is PRESENTATION (Settings::ui_style: the
// Classic and Modern interface styles). Nothing here reads or changes a rule: the health,
// the tray contents, whether the world has stamina or a second tray, all
// arrive in State from the world being played.
namespace openminerz::hud
{

enum class Sprite : std::uint8_t
{
    solid,     // the 1x1 white DummyTexture: every filled rectangle
    item_icon, // InventoryItem._2DImages: the caller draws the item's icon
    hud_grid,
    selector,
    crosshair,
    crosshair_tick, // 1.9.9.8 only
    health_empty,
    health_full,
    stamina_empty, // 1.9.9.8 only
    bubble_bar,
    damage_arrow,
    sniper_scope,
    missile_lock,
    missile_locking,
    count
};

// The sprite fonts InGameHUD uses (sprite_font.hpp's RetailFont has all ten).
enum class Font : std::uint8_t
{
    small,        // SmallFont: item counts
    medium,       // MedFont: every HUD label
    medium_large, // MedLargeFont: the death prompt
    large,        // LargeFont: "Day N"
    console,      // ConsoleFont: chat and broadcast lines
    count
};

struct Size final
{
    int width{};
    int height{};
};
struct Extent final
{
    float width{};
    float height{};
};
struct IntRect final
{
    int x{};
    int y{};
    int width{};
    int height{};
};
struct Rect final
{
    float x{};
    float y{};
    float width{};
    float height{};
};
// Exactly the four bytes the original hands to SpriteBatch. XNA blends
// premultiplied (result = colour + destination * (1 - alpha)), and several HUD
// colours are deliberately not premultiplied values, so the renderer must use
// the same blend to reproduce them.
struct Color final
{
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{};
    [[nodiscard]] constexpr bool operator==(const Color &) const noexcept = default;
};

// The sprite's name in the build's SpriteSheet ("HudGrid"); the importer
// writes it to the content pack as hud_<name>.bmp. Empty for solid/item_icon.
[[nodiscard]] std::string_view sprite_sheet_name(Sprite sprite) noexcept;
// Its authored size in that build's sheet ({0,0} when the build lacks it).
[[nodiscard]] Size sprite_size(UiStyle style, Sprite sprite) noexcept;
// "MedFont": the SpriteFont asset name, the same in both builds.
[[nodiscard]] std::string_view font_asset_name(Font font) noexcept;

// SpriteFont.MeasureString and SpriteFont.LineSpacing of the build being laid
// out. The application answers from the imported fonts (sprite_font.hpp).
class TextMetrics
{
  public:
    virtual ~TextMetrics() = default;
    [[nodiscard]] virtual Extent measure(Font font, std::string_view text) const = 0;
    [[nodiscard]] virtual int line_spacing(Font font) const = 0;
};

struct TraySlot final
{
    bool occupied{};
    // InventoryItemIDs value in the build being laid out: selects the cell of
    // the original's icon atlas. Only the record uses it; the application
    // draws its own icon for the slot.
    int icon{};
    int count{1};
    // ItemClass.ItemSelfDamagePerUse > 0: the item shows a condition bar.
    bool wears{};
    float condition{1.0F}; // ItemHealthLevel
};

struct DamageArrow final
{
    // Angle.ATan2(local.X, -local.Z) of the damage source in player space.
    float rotation{};
    // DamageIndicator.fadeAmount: 1 for two seconds, then down to 0 over one.
    float fade{1.0F};
};

struct ConsoleLine final
{
    std::string text;
    // ConsoleElement.Message.Visibility: 1 for ten seconds, then 1 -> 0 over three.
    float visibility{1.0F};
};

struct RosterLine final
{
    std::string gamertag;
    bool local{};
};

// Everything InGameHUD.OnDraw reads, already resolved by the caller.
struct State final
{
    // PlayerHealth / PlayerStamina / PlayerOxygen, each 0..1.
    float health{1.0F};
    float stamina{1.0F};
    float oxygen{1.0F};
    // The world has stamina (1.9.9.8 rules outside Endurance). Without it the
    // Modern HUD shows the health meter alone, centred, as it does in Endurance.
    bool has_stamina{};
    // StaminaBlockTimer is running: the Modern stamina meter turns OrangeRed.
    bool stamina_blocked{};
    bool underwater{};
    // Player.PercentSubmergedLava, 0..1.
    float lava{};
    bool dead{};
    bool shouldering{};

    std::array<TraySlot, 8> tray{};
    // 1.9.9.8's second tray, drawn dimmed behind the first by the Modern HUD.
    bool has_next_tray{};
    std::array<TraySlot, 8> next_tray{};
    // PlayerStats.SecondTrayFaded ? 0.4 : 1.
    float next_tray_alpha{0.4F};
    int selected{};

    // ActiveInventoryItem.GetDisplayText ("Pistol 5/0") and .Name ("Pistol").
    // The original measures the NAME to place the line but draws the TEXT.
    std::string item_text;
    std::string item_name;
    // BlockType.Name of the aimed block while ConstructionProbe.AbleToBuild.
    bool aiming_at_block{};
    std::string block_name;
    // Strings.Distance and Strings.Max in the build's wording.
    std::string distance_word{"Distance"};
    std::string max_word{"Max"};
    int distance{};
    int max_distance{};

    std::vector<DamageArrow> damage;
    // fadeInGameStart.ElaspedTime in seconds; >= 1 means finished.
    float fade_in_seconds{1.0F};
    // drawDayTimer.ElaspedTime in seconds; >= 9 means no banner.
    float day_banner_seconds{9.0F};
    std::string day_text; // Strings.Day + " " + currentDay

    // Scoped weapon fully shouldered (ShoulderedAnimState && GunClass.Scoped).
    bool scope{};
    // RocketLauncherGuidedInventoryItemClass.LockedOnSpriteLocation, when the
    // guided launcher has a dragon in view; `locked` picks the red sprite.
    bool lock_box{};
    bool locked{};
    IntRect lock_rect{};
    // 1.9.9.8 accuracy reticle: (spread angle / FPS camera field of view), and
    // InnaccuracyMultiplier (0 steady .. 1 fully bloomed).
    float reticle_fraction{};
    float inaccuracy{};

    // ConsoleElement lines, oldest first.
    std::vector<ConsoleLine> console;
    // The "show players" key is held: the list replaces the HUD body.
    bool show_players{};
    std::string players_word{"Players"};
    std::string player_word{"Player"};
    int max_players{};
    std::vector<RosterLine> roster;

    // Hide switches of the settings page. They only remove elements; the ones
    // that remain keep the original's rectangles.
    bool show_vitals{true};
    bool show_oxygen{true};
    bool show_tray{true};
    bool show_item_text{true};
    bool show_block_name{true};
    bool show_distance{true};
    bool show_crosshair{true};
    bool show_damage{true};
    bool show_scope{true};
    bool show_console{true};
};

struct Draw final
{
    enum class Kind : std::uint8_t
    {
        sprite,
        text
    };
    Kind kind{Kind::sprite};

    // Sprite: source rectangle relative to the sprite, destination on the canvas.
    Sprite sprite{Sprite::solid};
    IntRect source{};
    Rect destination{};
    // Rotation in radians about `origin` (in source pixels). When non-zero the
    // destination's x/y is where the origin lands, as in SpriteBatch.Draw.
    float rotation{};
    float origin_x{};
    float origin_y{};
    // item_icon only: which tray (0 current, 1 next) and slot it belongs to.
    int tray{};
    int slot{};

    // Text: DrawString(font, text, {x, y}, color, 0, Zero, scale, None, _).
    Font font{Font::medium};
    float x{};
    float y{};
    float scale{1.0F};
    std::string text;

    Color color{255U, 255U, 255U, 255U};
    // True for the screen-filling washes (hurt, lava, fade-in, scope bars).
    // 1.6.3.8 draws them over its 16:9 picture and leaves the rest of a wider
    // or taller window black; a renderer whose world fills the window should
    // extend them to the window instead.
    bool fills_canvas{};
};

struct Frame final
{
    // The space every rectangle is in. 1.6.3.8 always lays out for 1280x720
    // (CastleMinerZGame draws into an offscreen buffer of that size); 1.9.9.8
    // lays out for the real back buffer.
    int canvas_width{};
    int canvas_height{};
    // Where DNAGame.Draw puts that canvas in the window: the whole window for
    // 1.9.9.8, the largest centred 16:9 rectangle for 1.6.3.8.
    IntRect present{};
    std::vector<Draw> draws;
};

// `style` must be classic or modern: the two retail HUDs. (The Remastered HUD
// is not a retail one and is not laid out here; see remastered_layout.hpp.
// Anything else passed in is laid out as Classic.) `window_*` is the
// back-buffer size in pixels.
[[nodiscard]] Frame layout(UiStyle style, const State &state, int window_width, int window_height,
                           const TextMetrics &metrics);

// A canvas rectangle in window pixels (Frame::present applied).
[[nodiscard]] Rect to_window(const Frame &frame, Rect canvas) noexcept;
[[nodiscard]] float window_scale(const Frame &frame) noexcept;

// The frame in HudParityProbe's record format, one line per draw, for
// comparison with a capture of the original. (A Modern capture's header line
// carries one extra field, the reticle quotient the probe observed. That is an
// input here, State::reticle_fraction, and is not echoed.)
[[nodiscard]] std::string record(const Frame &frame);

// XNA colour arithmetic used by the HUD, exposed for tests.
[[nodiscard]] Color color_from_floats(float r, float g, float b, float a) noexcept; // new Color(float...)
[[nodiscard]] Color color_scale(Color color, float scale) noexcept;                // Color * float
[[nodiscard]] Color color_lerp(Color from, Color to, float amount) noexcept;       // Color.Lerp

} // namespace openminerz::hud
