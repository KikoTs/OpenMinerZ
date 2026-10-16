#pragma once

#include "openminerz/graphics_settings.hpp"

#include <filesystem>
#include <string>

namespace openminerz
{

enum class WindowMode
{
    windowed,
    borderless,
    fullscreen
};
enum class PresentationProfile
{
    classic,
    rebirth
};
enum class AssetStyle
{
    classic,
    classic_hd,
    modern
};
enum class QualityLevel
{
    low,
    medium,
    high,
    ultra
};
enum class AntiAliasing
{
    off,
    fxaa,
    taa,
    msaa4x
};
// How the HDR scene is compressed to the display. "legacy" is the old
// soft-knee clamp that blew out bright terrain; the rest are real tone curves.
enum class ToneMapping
{
    legacy,
    reinhard,
    filmic,
    aces
};
enum class ColorFilter
{
    none,
    deuteranopia,
    protanopia,
    tritanopia
};

// How the Remastered HUD shows health (and stamina, where the world has it):
//   remastered  labelled bars over the item tray (the default)
//   classic     a row of ten hearts over the tray, stamina as a slim bar
//   modern      the same bars at the top centre of the screen
//   none        hidden
// The Classic and Modern interface styles are exact copies of the retail HUD
// and draw that release's own meters; for them only `none` matters (hidden)
// and every other value means shown.
enum class VitalsStyle
{
    classic,
    modern,
    remastered,
    none
};

// The look of the whole in-game interface: the HUD and the inventory, crate
// and crafting screens together, as one unit. There is no way to mix them.
// PRESENTATION ONLY. It never changes a rule or a byte on the network: what
// exists, what can be crafted, stack sizes, health and stamina always come
// from the generation of the world being played, and every style performs the
// same inventory operations (screens::Session). A Classic world shown with the
// Modern style is still a Classic game.
//
// (The enumerators keep their numeric values; `remastered` was added last.)
enum class UiStyle
{
    automatic, // the retail look matching the world being played: Classic or Modern
    classic,   // retail 1.6.3.8, exact
    modern,    // retail 1.9.9.8, exact
    remastered // OpenMinerZ's own interface (the default)
};

// What the frame-rate cap follows. `fixed` uses Settings::frame_limit as
// chosen; `display` keeps frame_limit equal to the monitor's refresh rate;
// `unlimited` stores frame_limit 0, which the frame limiter treats as no cap.
enum class FrameLimitMode
{
    fixed,
    display,
    unlimited
};

// Input-latency policy for the frame loop (see Settings::low_latency).
enum class LowLatencyMode
{
    off,
    on
};

struct Settings final
{
    int schema_version{1};

    WindowMode window_mode{WindowMode::windowed};
    int window_width{1600};
    int window_height{900};
    bool vsync{true};
    int frame_limit{144};
    float ui_scale{1.0F};
    float safe_area{0.96F};

    PresentationProfile presentation{PresentationProfile::rebirth};
    // Selects a complete original-content generation at the next process start.
    AssetStyle texture_style{AssetStyle::modern};
    float render_scale{1.0F};
    QualityLevel texture_quality{QualityLevel::high};
    QualityLevel shadow_quality{QualityLevel::high};
    // Off or FXAA. The taa/msaa4x names predate any implementation and are
    // normalized to fxaa, the only smoothing the renderer performs.
    AntiAliasing anti_aliasing{AntiAliasing::fxaa};
    bool ambient_occlusion{true};
    // Adds a restrained emissive shimmer to exposed ore veins. This is a
    // presentation-only option and never changes block lighting or gameplay.
    bool ore_glow{true};
    QualityLevel particle_quality{QualityLevel::high};
    // Tone mapping and its manual exposure trim (stops, -2..+2). Auto exposure
    // measures the scene each frame; this biases the result.
    ToneMapping tone_mapping{ToneMapping::aces};
    float exposure_compensation{0.0F};
    bool auto_exposure{true};
    // Post-grade strength: 0 disables the saturation/vibrance/lift pass.
    float color_grading{1.0F};
    int view_distance{16};
    // "Field of view": the WORLD camera's vertical angle in degrees, 36..105
    // (60..133 across a 16:9 display; wider displays see more to the sides).
    // Presentation only - no rule and no network message reads it - and it
    // never reaches the first-person hands. All of the arithmetic, including
    // what shouldering does to it, is openminerz/view_projection.hpp. Retail's
    // camera is 73; 74 is the view this game has always opened with.
    float vertical_fov{74.0F};
    // The first-person body's OWN projection: CastleMiner Z's
    // Avatar.EyePointCamera, 90 degrees (45 shouldered). Retail's value is the
    // default baseline, scaled with the world FOV for both hands and weapon.
    // The menu has one FOV slider; this baseline is kept in the settings
    // file as an advanced option for a player who tuned it (55..110).
    float viewmodel_fov{90.0F};

    float mouse_sensitivity{1.0F};
    float controller_sensitivity{1.0F};
    bool invert_y{false};
    bool auto_climb{true};
    // Creative-mode horizontal cruise speed in metres per second. Vertical
    // flight keeps the original 3:4 ratio so one control tunes both axes.
    float creative_flight_speed{8.0F};
    float camera_bob{0.65F};
    float screen_shake{0.75F};
    bool hud_visible{true};
    // "Interface style" (file key `interface_style`): the one choice that sets
    // the HUD and the inventory, crate and crafting screens together. Read it
    // through resolved_ui_style(), never directly: `automatic` follows the
    // world. It does not affect gameplay (see UiStyle). The older keys
    // `ui_style` and `crafting_style` are read once, to migrate a file, and
    // never written (see SettingsStore::load).
    UiStyle ui_style{UiStyle::remastered};
    VitalsStyle vitals_style{VitalsStyle::remastered};
    bool hud_server{false};
    bool hud_coordinates{false};
    bool hud_debug{false};
    // The Remastered HUD's heading strip, on as it always was. Neither retail
    // HUD has one, so the Classic and Modern styles never draw it (nor the
    // clock or the control hints): they are exact copies.
    bool hud_compass{true};
    bool hud_clock{false};
    bool hud_distance{true};
    bool hud_oxygen{true};
    bool hud_crosshair{true};
    bool hud_target{true};
    bool hud_roster{true};
    bool hud_scope{true};
    bool hud_hotbar{true};
    bool hud_item_name{true};
    bool hud_hints{false};
    bool hud_chat{true};
    bool hud_notifications{true};
    bool hud_damage{true};
    bool hud_death{true};
    // 1.9.9.8's "Fade Inactive Item Tray" (PlayerStats.SecondTrayFaded, off by
    // default): the second tray at 40% instead of solid. Presentation only.
    bool fade_inactive_tray{false};
    // (The crafting screen follows `ui_style` above like everything else. The
    // older `crafting_style` and `crafting_menu_modern` lines are not read:
    // the first allowed mixed pairings that are gone, the second also switched
    // the recipe rules, which no setting may do. Both are dropped at the next
    // save.)
    // Last chosen game mode / difficulty (original GameModeTypes /
    // GameDifficultyTypes enum indices; not persisted per world, same as the
    // original menu flow which re-asks on every launch but remembers cursor).
    int gameplay_profile{0}; // Classic/Modern rules, independent of the content pack.
    int game_mode{1};
    int game_difficulty{1};

    float master_volume{0.90F};
    float music_volume{0.65F};
    float effects_volume{0.90F};
    float ambience_volume{0.80F};
    float voice_volume{0.85F};
    bool mute_unfocused{false};
    // Empty id follows the Windows default render endpoint. A named endpoint
    // is selected at the next process start so XACT can preload one renderer.
    std::string audio_output_id;
    std::string audio_output_name;

    bool raw_mouse_input{true};
    bool controller_vibration{true};
    bool controller_aim_assist{true};
    bool hold_to_sprint{false};

    bool reduced_motion{false};
    bool high_contrast{false};
    bool larger_text{false};
    ColorFilter color_filter{ColorFilter::none};
    bool subtitles{true};
    float subtitle_size{1.0F};

    // --- Graphics options added with the settings-menu redesign. ---
    // The frame limiter only reads frame_limit; this records what the player
    // asked for so "match display" survives a monitor change (see FrameLimitMode).
    FrameLimitMode frame_limit_mode{FrameLimitMode::fixed};
    // "Shadows: Off" skips every sun-shadow pass. shadow_quality then selects
    // the shadow-map resolution and how far from the camera shadows reach.
    bool shadows_enabled{true};
    // How much direct sunlight a shadow removes (0.25 faint .. 0.90 deep).
    // Purely a look preference; it has no performance cost.
    float shadow_darkness{0.68F};
    // Anisotropic texture filtering samples: 1 (off), 2, 4, 8 or 16.
    int texture_filtering{8};
    // Soft glow around bright things (sun, lava, torches).
    bool bloom{true};
    // Light shafts drawn toward the sun in the post-process pass.
    bool sun_shafts{true};
    // Small frames-per-second / frame-time read-out in the screen corner.
    bool show_fps{false};
    // Screen-space shading, volumetric light, depth of field, colour controls
    // and lens effects of the Enhanced presentation (graphics_settings.hpp).
    GraphicsSettings graphics{};

    // --- Xbox avatars (the Add-Ons screen; openminerz/xbox_avatar.hpp). ---
    // Presentation only: no rule, hit box or stock network message reads them,
    // and the stock character is the fallback wherever an avatar is missing.
    // Wear the local player's Xbox avatar when a file is found, and offer it to
    // players who run the Xbox Avatar add-on or OpenMinerZ.
    bool xbox_avatar_show_mine{true};
    // Ask those players for their avatars and draw them.
    bool xbox_avatar_show_others{true};
    // A specific `.ocavatar` to wear. Empty looks where the add-on's importer
    // puts it (and in %LOCALAPPDATA%\OpenMinerZ), taking the newest.
    std::string xbox_avatar_file;

    // --- Frame pacing (presentation only; the 60 Hz simulation is untouched). ---
    // Blend the camera and moving creatures between simulation ticks so motion
    // is even at any refresh rate. Off draws raw tick positions, which step
    // visibly on displays faster than 60 Hz.
    bool motion_smoothing{true};
    // On: at most one frame queued for the display, the mouse is sampled again
    // immediately before the view is built, and a vsynced frame starts late
    // instead of finishing early and waiting in the queue. Off restores the
    // driver's default three-frame queue and frame timing.
    LowLatencyMode low_latency{LowLatencyMode::on};

    // --- Gameplay the original game does not have. Default OFF, every one. ---
    // These are only the player's stored choices. Whether one is in effect is
    // decided by enhancement_active() in retail_parity.hpp, which also holds
    // the name and description of each and refuses them in an online session.
    bool enhancement_lagoon_water{false};
    bool enhancement_sinking_lava{false};
    bool enhancement_free_flight{false};

    void normalize() noexcept;
};

class SettingsStore final
{
  public:
    explicit SettingsStore(std::filesystem::path path);

    [[nodiscard]] Settings load() const;
    void save(const Settings &settings) const;
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string to_string(VitalsStyle value);
[[nodiscard]] std::string to_string(UiStyle value);
[[nodiscard]] std::string to_string(WindowMode value);
[[nodiscard]] std::string to_string(PresentationProfile value);
[[nodiscard]] std::string to_string(AssetStyle value);
[[nodiscard]] std::string to_string(QualityLevel value);
[[nodiscard]] std::string to_string(AntiAliasing value);
[[nodiscard]] std::string to_string(ToneMapping value);
[[nodiscard]] std::string to_string(ColorFilter value);
[[nodiscard]] std::string to_string(FrameLimitMode value);
[[nodiscard]] std::string to_string(LowLatencyMode value);

// The shadow darkness the old "shadow quality" levels produced. Settings files
// written before shadow_darkness existed are migrated through this so the
// picture a player had chosen does not change underneath them.
[[nodiscard]] float legacy_shadow_darkness(QualityLevel quality) noexcept;

// The style to draw with: the player's choice, or - when the choice is
// `automatic` - the retail look of the world's own generation. Never returns
// `automatic`, so callers branch on exactly three values: remastered, classic,
// modern. `world_is_modern` is the generation of the world in play
// (InventoryProfile::modern); pass the last-chosen generation in a menu.
[[nodiscard]] constexpr UiStyle resolve_ui_style(UiStyle choice, bool world_is_modern) noexcept
{
    if (choice == UiStyle::classic || choice == UiStyle::modern || choice == UiStyle::remastered)
        return choice;
    return world_is_modern ? UiStyle::modern : UiStyle::classic;
}
// HUD, inventory, crate and crafting screens: all of them, always together.
[[nodiscard]] constexpr UiStyle resolved_ui_style(const Settings &settings,
                                                  bool world_is_modern) noexcept
{
    return resolve_ui_style(settings.ui_style, world_is_modern);
}
// True for the two exact copies of a retail release (given a resolved style).
[[nodiscard]] constexpr bool is_retail_style(UiStyle resolved) noexcept
{
    return resolved == UiStyle::classic || resolved == UiStyle::modern;
}
// The retail release whose words, sprite fonts and item icons a style draws
// with: its own for Classic and Modern, the world's for Remastered (which has
// no release of its own). Always exactly `classic` or `modern`.
[[nodiscard]] constexpr UiStyle retail_build_of(UiStyle choice, bool world_is_modern) noexcept
{
    const UiStyle resolved = resolve_ui_style(choice, world_is_modern);
    if (is_retail_style(resolved))
        return resolved;
    return world_is_modern ? UiStyle::modern : UiStyle::classic;
}

} // namespace openminerz
