#pragma once

#include "openminerz/settings.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace openminerz::ui
{

enum class SettingsTab
{
    display,
    graphics,
    gameplay,
    audio,
    controls,
    accessibility
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
    subtitle_size
};

struct SettingRow final
{
    SettingId id{};
    std::string_view label;
    std::string_view description;
    SettingKind kind{};
};

[[nodiscard]] const std::vector<SettingRow> &rows_for_tab(SettingsTab tab);
[[nodiscard]] std::string_view tab_label(SettingsTab tab) noexcept;
[[nodiscard]] SettingsTab next_tab(SettingsTab tab, int direction) noexcept;
[[nodiscard]] std::string setting_value(const Settings &settings, SettingId id);
[[nodiscard]] float setting_normalized_value(const Settings &settings, SettingId id) noexcept;
void adjust_setting(Settings &settings, SettingId id, int direction);

} // namespace openminerz::ui
