#pragma once

#include "openminerz/retail_parity.hpp"
#include "openminerz/settings.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openminerz::ui
{

// Pages of the settings screen, in the order they are shown. all_tabs()
// returns the same order; code must not assume a particular count.
enum class SettingsTab
{
    display,
    graphics,
    effects,
    performance,
    gameplay,
    controls,
    interface,
    audio,
    accessibility,
    // Gameplay the original game does not have (retail_parity.hpp). Every
    // option on this page is off by default and works offline only.
    enhancements
};
enum class SettingKind
{
    toggle,
    choice,
    slider
};

enum class SettingId
{
    window_mode,
    resolution,
    vsync,
    frame_limit,
    ui_scale,
    safe_area,
    presentation,
    texture_style,
    render_scale,
    texture_quality,
    shadow_quality,
    anti_aliasing,
    ambient_occlusion,
    ore_glow,
    particle_quality,
    tone_mapping,
    auto_exposure,
    exposure_compensation,
    color_grading,
    view_distance,
    vertical_fov,
    viewmodel_fov,
    mouse_sensitivity,
    controller_sensitivity,
    invert_y,
    auto_climb,
    creative_flight_speed,
    camera_bob,
    screen_shake,
    hud_visible,
    vitals_style,
    hud_server,
    hud_coordinates,
    hud_debug,
    hud_compass,
    hud_clock,
    hud_distance,
    hud_oxygen,
    hud_crosshair,
    hud_target,
    hud_roster,
    hud_scope,
    hud_hotbar,
    hud_item_name,
    hud_hints,
    hud_chat,
    hud_notifications,
    hud_damage,
    hud_death,
    fade_inactive_tray,
    audio_output,
    master_volume,
    music_volume,
    effects_volume,
    ambience_volume,
    voice_volume,
    mute_unfocused,
    raw_mouse_input,
    controller_vibration,
    controller_aim_assist,
    hold_to_sprint,
    reduced_motion,
    high_contrast,
    larger_text,
    color_filter,
    subtitles,
    subtitle_size,
    // Added with the settings redesign; appended so existing values keep theirs.
    quality_preset,
    shadow_darkness,
    texture_filtering,
    bloom,
    sun_shafts,
    show_fps,
    // Frame-pacing switches owned by the frame loop (Settings::low_latency,
    // Settings::motion_smoothing); the menu only exposes them.
    low_latency,
    motion_smoothing,
    // "Interface style": the look of the HUD and of the inventory, crate and
    // crafting screens, as one unit (Settings::ui_style). Presentation only.
    ui_style,
    // One row per Enhancement in retail_parity.hpp, in the same order.
    enhancement_lagoon_water,
    enhancement_sinking_lava,
    enhancement_free_flight,
    // One row per field of Settings::graphics. Their labels, descriptions and
    // value handling live in one table (graphics_options.hpp); appended so
    // every earlier value keeps its number.
    shadow_distance,
    shadow_softness,
    ambient_shading,
    contact_shadows,
    cloud_quality,
    texture_detail,
    bloom_strength,
    volumetric_quality,
    depth_of_field,
    depth_of_field_strength,
    lens_flare,
    motion_blur,
    sharpening,
    contrast,
    saturation,
    temperature,
    vignette,
    film_grain,
    chromatic_aberration,
    // The deep underground (src/hell_atmosphere.hpp); rows of the same table.
    heat_haze,
    embers,
    deep_light,
    // Far terrain (openminerz/world/far_terrain.hpp); rows of the same table.
    far_terrain,
    far_terrain_detail,
    // The effects layer (openminerz/vfx.hpp); rows of the same table.
    effect_lighting,
    distortion_effects,
    explosion_shake,
    explosion_flash
};

struct SettingRow final
{
    SettingId id{};
    // Plain-language name, then one or two sentences saying what the option
    // changes and what it costs. Both are shown to the player verbatim.
    std::string_view label;
    std::string_view description;
    SettingKind kind{};
    // The new value is only picked up the next time the game starts.
    bool requires_restart{};
};

// One-click bundles of the options that trade picture quality for speed.
// `custom` is what the menu shows when the options match no bundle.
enum class QualityPreset
{
    low,
    medium,
    high,
    ultra,
    custom
};

[[nodiscard]] const std::vector<SettingRow> &rows_for_tab(SettingsTab tab);
// Short sidebar name ("PERFORMANCE"), full page heading ("PERFORMANCE &
// LATENCY") and a one-line explanation of what the page is for.
[[nodiscard]] std::string_view tab_label(SettingsTab tab) noexcept;
[[nodiscard]] std::string_view tab_title(SettingsTab tab) noexcept;
[[nodiscard]] std::string_view tab_summary(SettingsTab tab) noexcept;
[[nodiscard]] std::span<const SettingsTab> all_tabs() noexcept;
[[nodiscard]] SettingsTab next_tab(SettingsTab tab, int direction) noexcept;
[[nodiscard]] std::string setting_value(const Settings &settings, SettingId id);
[[nodiscard]] float setting_normalized_value(const Settings &settings, SettingId id) noexcept;
void adjust_setting(Settings &settings, SettingId id, int direction);

// Empty while the option is doing something. Otherwise a short sentence
// explaining why it has no effect right now ("Shadows are off."); the menu
// shows it and adjust_setting leaves the stored value alone.
[[nodiscard]] std::string_view setting_unavailable_reason(const Settings &settings,
                                                          SettingId id) noexcept;

// The Interface rows that belong to the Remastered style alone (compass, clock,
// control hints, notifications): the Classic and Modern styles are exact copies
// of a retail HUD that has none of them, so there the row is unavailable and
// says so. `Settings::ui_style == automatic` always resolves to a retail style.
[[nodiscard]] bool setting_is_remastered_only(SettingId id) noexcept;

// The non-retail gameplay feature a row switches, if it is one of those rows.
// The row stores only the player's choice; whether the feature is in effect
// is enhancement_active(), which the application shows beside the value.
[[nodiscard]] std::optional<Enhancement> setting_enhancement(SettingId id) noexcept;

// The preset the current options add up to, or `custom`.
[[nodiscard]] QualityPreset detect_quality_preset(const Settings &settings) noexcept;
// Sets every preset-controlled option. `custom` changes nothing.
void apply_quality_preset(Settings &settings, QualityPreset preset) noexcept;
[[nodiscard]] std::string_view quality_preset_label(QualityPreset preset) noexcept;

// Restore one option, or every option on a page, to its factory default.
void reset_setting(Settings &settings, SettingId id);
void reset_tab(Settings &settings, SettingsTab tab);

// Step to the neighbouring entry of `modes` (width, height; any order). The
// platform layer passes what the monitor really supports; an empty list falls
// back to a built-in set of common sizes.
void adjust_resolution(Settings &settings, int direction,
                       std::span<const std::pair<int, int>> modes);

} // namespace openminerz::ui
