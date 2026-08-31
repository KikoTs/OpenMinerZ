#include "openminerz/settings_menu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace openminerz::ui
{
namespace
{

const std::vector<SettingRow> display_rows{
    {SettingId::window_mode, "Window mode", "Choose windowed, borderless, or exclusive fullscreen.",
     SettingKind::choice},
    {SettingId::resolution, "Resolution",
     "Output resolution used by windowed and exclusive fullscreen modes.", SettingKind::choice},
    {SettingId::vsync, "Vertical sync", "Synchronize frame presentation to the display refresh.",
     SettingKind::toggle},
    {SettingId::frame_limit, "Frame limit",
     "Caps render cadence when vertical sync is not the limiting factor.", SettingKind::choice},
    {SettingId::ui_scale, "Interface scale",
     "Scales layout and re-rasterizes text at the matching output density.", SettingKind::slider},
    {SettingId::safe_area, "Safe area", "Moves critical UI away from display and capture edges.",
     SettingKind::slider},
};

const std::vector<SettingRow> graphics_rows{
    {SettingId::presentation, "Presentation",
     "Classic preserves the original mood; Rebirth enables modern rendering.", SettingKind::choice},
    {SettingId::texture_style, "Texture style",
     "Switch Classic, AI-restored Classic HD, or Modern. Takes effect after restart.",
     SettingKind::choice},
    {SettingId::render_scale, "Render scale",
     "Internal 3D resolution. The interface always stays native-resolution.", SettingKind::slider},
    {SettingId::shadow_quality, "Shadow quality",
     "Controls moving sunlight contrast across voxel faces.", SettingKind::choice},
    {SettingId::ambient_occlusion, "Ambient occlusion",
     "Uses the original-style neighbour mask for per-corner contact shading.", SettingKind::toggle},
    {SettingId::ore_glow, "Ore glow",
     "Adds a subtle emissive shimmer to exposed ore veins.", SettingKind::toggle},
    {SettingId::particle_quality, "Particle quality",
     "Controls effect density while preserving event timing.", SettingKind::choice},
    {SettingId::tone_mapping, "Tone mapping",
     "How bright light is compressed for the display. ACES holds detail in sand and snow; "
     "Legacy is the old clipping curve.",
     SettingKind::choice},
    {SettingId::auto_exposure, "Auto exposure",
     "Measures scene brightness each frame and adapts like an eye leaving a cave.",
     SettingKind::toggle},
    {SettingId::exposure_compensation, "Exposure trim",
     "Manual brightness bias in stops on top of the measured exposure.", SettingKind::slider},
    {SettingId::color_grading, "Colour grading",
     "Strength of the final contrast and vibrance pass.", SettingKind::slider},
    {SettingId::view_distance, "Terrain visibility",
     "Fog range in metres; the streamed voxel window expands ahead of it.", SettingKind::slider},
    {SettingId::vertical_fov, "Vertical field of view",
     "Aspect-ratio independent gameplay camera field of view.", SettingKind::slider},
    {SettingId::viewmodel_fov, "Viewmodel field of view",
     "Independent first-person arms and weapon field of view.", SettingKind::slider},
};

const std::vector<SettingRow> gameplay_rows{
    {SettingId::mouse_sensitivity, "Mouse sensitivity", "Look sensitivity for mouse input.",
     SettingKind::slider},
    {SettingId::controller_sensitivity, "Controller sensitivity",
     "Look sensitivity for gamepad input.", SettingKind::slider},
    {SettingId::invert_y, "Invert vertical look", "Reverses vertical camera input.",
     SettingKind::toggle},
    {SettingId::auto_climb, "Auto climb", "Step onto valid voxel ledges while moving forward.",
     SettingKind::toggle},
    {SettingId::creative_flight_speed, "Creative flight speed",
     "Horizontal cruise speed in Creative; climb and descend scale with it.", SettingKind::slider},
    {SettingId::camera_bob, "Camera bob", "Adjust first-person movement motion.",
     SettingKind::slider},
    {SettingId::screen_shake, "Screen shake", "Adjust camera response to impacts and explosions.",
     SettingKind::slider},
    {SettingId::hud_visible, "Heads-up display",
     "Show gameplay meters, inventory, compass, and prompts.", SettingKind::toggle},
};

const std::vector<SettingRow> audio_rows{
    {SettingId::audio_output, "Output device",
     "Choose a Windows playback device. Takes effect after restart.", SettingKind::choice},
    {SettingId::master_volume, "Master volume", "Overall output level.", SettingKind::slider},
    {SettingId::music_volume, "Music", "Menu and gameplay music level.", SettingKind::slider},
    {SettingId::effects_volume, "Effects", "Weapons, blocks, creatures, and interaction level.",
     SettingKind::slider},
    {SettingId::ambience_volume, "Ambience", "Weather, cave, wind, and environment level.",
     SettingKind::slider},
    {SettingId::mute_unfocused, "Mute in background",
     "Silence the game while another application has focus.", SettingKind::toggle},
};

const std::vector<SettingRow> control_rows{
    {SettingId::raw_mouse_input, "Raw mouse input",
     "Read mouse motion without operating-system acceleration.", SettingKind::toggle},
    {SettingId::controller_vibration, "Controller vibration", "Enable compatible gamepad haptics.",
     SettingKind::toggle},
};

const std::vector<SettingRow> accessibility_rows{
    {SettingId::reduced_motion, "Reduced motion",
     "Shorten transitions and suppress nonessential parallax.", SettingKind::toggle},
    {SettingId::high_contrast, "High contrast interface",
     "Strengthen panels, outlines, focus states, and text contrast.", SettingKind::toggle},
    {SettingId::larger_text, "Larger text",
     "Increase type size while keeping the same responsive layout rules.", SettingKind::toggle},
    {SettingId::subtitles, "Subtitles", "Show captions for dialogue and important audio cues.",
     SettingKind::toggle},
    {SettingId::subtitle_size, "Subtitle size",
     "Scale subtitle text independently of the interface.", SettingKind::slider},
};

std::string percent(float value)
{
    return std::to_string(static_cast<int>(std::lround(value * 100.0F))) + "%";
}

std::string decimal(float value, int precision = 2)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

template <typename Enum, std::size_t Size>
Enum cycle(Enum value, int direction, const std::array<Enum, Size> &values)
{
    const auto found = std::find(values.begin(), values.end(), value);
    const int index = found == values.end() ? 0 : static_cast<int>(found - values.begin());
    const int size = static_cast<int>(Size);
    return values[static_cast<std::size_t>((index + (direction < 0 ? -1 : 1) + size) % size)];
}

void toggle(bool &value) noexcept
{
    value = !value;
}

} // namespace

const std::vector<SettingRow> &rows_for_tab(SettingsTab tab)
{
    switch (tab)
    {
    case SettingsTab::display:
        return display_rows;
    case SettingsTab::graphics:
        return graphics_rows;
    case SettingsTab::gameplay:
        return gameplay_rows;
    case SettingsTab::audio:
        return audio_rows;
    case SettingsTab::controls:
        return control_rows;
    case SettingsTab::accessibility:
        return accessibility_rows;
    }
    return display_rows;
}

std::string_view tab_label(SettingsTab tab) noexcept
{
    switch (tab)
    {
    case SettingsTab::display:
        return "DISPLAY";
    case SettingsTab::graphics:
        return "GRAPHICS";
    case SettingsTab::gameplay:
        return "GAMEPLAY";
    case SettingsTab::audio:
        return "AUDIO";
    case SettingsTab::controls:
        return "CONTROLS";
    case SettingsTab::accessibility:
        return "ACCESSIBILITY";
    }
    return "DISPLAY";
}

SettingsTab next_tab(SettingsTab tab, int direction) noexcept
{
    constexpr int count = 6;
    const int current = static_cast<int>(tab);
    return static_cast<SettingsTab>((current + (direction < 0 ? -1 : 1) + count) % count);
}

std::string setting_value(const Settings &settings, SettingId id)
{
    switch (id)
    {
    case SettingId::window_mode:
        switch (settings.window_mode)
        {
        case WindowMode::windowed:
            return "WINDOWED";
        case WindowMode::borderless:
            return "BORDERLESS";
        case WindowMode::fullscreen:
            return "FULLSCREEN";
        }
        break;
    case SettingId::resolution:
        return std::to_string(settings.window_width) + " x " +
               std::to_string(settings.window_height);
    case SettingId::vsync:
        return settings.vsync ? "ON" : "OFF";
    case SettingId::frame_limit:
        return std::to_string(settings.frame_limit) + " FPS";
    case SettingId::ui_scale:
        return percent(settings.ui_scale);
    case SettingId::safe_area:
        return percent(settings.safe_area);
    case SettingId::presentation:
        return settings.presentation == PresentationProfile::classic ? "CLASSIC" : "REBIRTH";
    case SettingId::texture_style:
        switch (settings.texture_style)
        {
        case AssetStyle::classic:
            return "CLASSIC";
        case AssetStyle::classic_hd:
            return "CLASSIC HD";
        case AssetStyle::modern:
            return "MODERN";
        }
        break;
    case SettingId::render_scale:
        return percent(settings.render_scale);
    case SettingId::texture_quality:
        return to_string(settings.texture_quality);
    case SettingId::shadow_quality:
        return to_string(settings.shadow_quality);
    case SettingId::anti_aliasing:
        switch (settings.anti_aliasing)
        {
        case AntiAliasing::off:
            return "OFF";
        case AntiAliasing::fxaa:
            return "FXAA";
        case AntiAliasing::taa:
            return "TAA";
        case AntiAliasing::msaa4x:
            return "MSAA 4X";
        }
        break;
    case SettingId::ambient_occlusion:
        return settings.ambient_occlusion ? "ON" : "OFF";
    case SettingId::ore_glow:
        return settings.ore_glow ? "ON" : "OFF";
    case SettingId::particle_quality:
        return to_string(settings.particle_quality);
    case SettingId::tone_mapping:
        switch (settings.tone_mapping)
        {
        case ToneMapping::legacy:
            return "LEGACY";
        case ToneMapping::reinhard:
            return "REINHARD";
        case ToneMapping::filmic:
            return "FILMIC";
        case ToneMapping::aces:
            return "ACES";
        }
        break;
    case SettingId::auto_exposure:
        return settings.auto_exposure ? "ON" : "OFF";
    case SettingId::exposure_compensation: {
        const int tenths = static_cast<int>(std::lround(settings.exposure_compensation * 10.0F));
        return (tenths > 0 ? "+" : "") + std::to_string(tenths / 10) + "." +
               std::to_string(std::abs(tenths) % 10) + " EV";
    }
    case SettingId::color_grading:
        return percent(settings.color_grading);
    case SettingId::view_distance:
        return std::to_string(settings.view_distance * 4) + " M";
    case SettingId::vertical_fov:
        return std::to_string(static_cast<int>(settings.vertical_fov)) + " DEG";
    case SettingId::viewmodel_fov:
        return std::to_string(static_cast<int>(settings.viewmodel_fov)) + " DEG";
    case SettingId::mouse_sensitivity:
        return decimal(settings.mouse_sensitivity);
    case SettingId::controller_sensitivity:
        return decimal(settings.controller_sensitivity);
    case SettingId::invert_y:
        return settings.invert_y ? "INVERTED" : "REGULAR";
    case SettingId::auto_climb:
        return settings.auto_climb ? "ON" : "OFF";
    case SettingId::creative_flight_speed:
        return std::to_string(static_cast<int>(std::lround(settings.creative_flight_speed))) +
               " M/S";
    case SettingId::camera_bob:
        return percent(settings.camera_bob);
    case SettingId::screen_shake:
        return percent(settings.screen_shake);
    case SettingId::hud_visible:
        return settings.hud_visible ? "VISIBLE" : "HIDDEN";
    case SettingId::audio_output:
        return settings.audio_output_name.empty() ? "SYSTEM DEFAULT" : settings.audio_output_name;
    case SettingId::master_volume:
        return percent(settings.master_volume);
    case SettingId::music_volume:
        return percent(settings.music_volume);
    case SettingId::effects_volume:
        return percent(settings.effects_volume);
    case SettingId::ambience_volume:
        return percent(settings.ambience_volume);
    case SettingId::voice_volume:
        return percent(settings.voice_volume);
    case SettingId::mute_unfocused:
        return settings.mute_unfocused ? "ON" : "OFF";
    case SettingId::raw_mouse_input:
        return settings.raw_mouse_input ? "ON" : "OFF";
    case SettingId::controller_vibration:
        return settings.controller_vibration ? "ON" : "OFF";
    case SettingId::controller_aim_assist:
        return settings.controller_aim_assist ? "ON" : "OFF";
    case SettingId::hold_to_sprint:
        return settings.hold_to_sprint ? "HOLD" : "TOGGLE";
    case SettingId::reduced_motion:
        return settings.reduced_motion ? "ON" : "OFF";
    case SettingId::high_contrast:
        return settings.high_contrast ? "ON" : "OFF";
    case SettingId::larger_text:
        return settings.larger_text ? "ON" : "OFF";
    case SettingId::color_filter:
        switch (settings.color_filter)
        {
        case ColorFilter::none:
            return "NONE";
        case ColorFilter::deuteranopia:
            return "DEUTERANOPIA";
        case ColorFilter::protanopia:
            return "PROTANOPIA";
        case ColorFilter::tritanopia:
            return "TRITANOPIA";
        }
        break;
    case SettingId::subtitles:
        return settings.subtitles ? "ON" : "OFF";
    case SettingId::subtitle_size:
        return percent(settings.subtitle_size);
    }
    return {};
}

float setting_normalized_value(const Settings &settings, SettingId id) noexcept
{
    switch (id)
    {
    case SettingId::ui_scale:
        return (settings.ui_scale - 0.75F) / 0.75F;
    case SettingId::safe_area:
        return (settings.safe_area - 0.80F) / 0.20F;
    case SettingId::render_scale:
        return (settings.render_scale - 0.50F) / 1.50F;
    case SettingId::view_distance:
        return static_cast<float>(settings.view_distance - 4) / 28.0F;
    case SettingId::vertical_fov:
        return (settings.vertical_fov - 50.0F) / 60.0F;
    case SettingId::viewmodel_fov:
        return (settings.viewmodel_fov - 55.0F) / 55.0F;
    case SettingId::mouse_sensitivity:
        return (settings.mouse_sensitivity - 0.10F) / 3.90F;
    case SettingId::controller_sensitivity:
        return (settings.controller_sensitivity - 0.10F) / 3.90F;
    case SettingId::creative_flight_speed:
        return (settings.creative_flight_speed - 4.0F) / 28.0F;
    case SettingId::camera_bob:
        return settings.camera_bob;
    case SettingId::screen_shake:
        return settings.screen_shake;
    case SettingId::master_volume:
        return settings.master_volume;
    case SettingId::music_volume:
        return settings.music_volume;
    case SettingId::effects_volume:
        return settings.effects_volume;
    case SettingId::ambience_volume:
        return settings.ambience_volume;
    case SettingId::voice_volume:
        return settings.voice_volume;
    case SettingId::subtitle_size:
        return (settings.subtitle_size - 0.75F) / 0.75F;
    default:
        return 0.0F;
    }
}

void adjust_setting(Settings &settings, SettingId id, int direction)
{
    const int sign = direction < 0 ? -1 : 1;
    switch (id)
    {
    case SettingId::window_mode:
        settings.window_mode =
            cycle(settings.window_mode, sign,
                  std::array{WindowMode::windowed, WindowMode::borderless, WindowMode::fullscreen});
        break;
    case SettingId::resolution: {
        constexpr std::array resolutions{std::pair{1280, 720},  std::pair{1600, 900},
                                         std::pair{1920, 1080}, std::pair{2560, 1440},
                                         std::pair{3440, 1440}, std::pair{3840, 2160}};
        auto found = std::find(resolutions.begin(), resolutions.end(),
                               std::pair{settings.window_width, settings.window_height});
        int index = found == resolutions.end() ? 2 : static_cast<int>(found - resolutions.begin());
        index = (index + sign + static_cast<int>(resolutions.size())) %
                static_cast<int>(resolutions.size());
        settings.window_width = resolutions[static_cast<std::size_t>(index)].first;
        settings.window_height = resolutions[static_cast<std::size_t>(index)].second;
        break;
    }
    case SettingId::vsync:
        toggle(settings.vsync);
        break;
    case SettingId::frame_limit: {
        constexpr std::array limits{30, 60, 90, 120, 144, 165, 240, 360};
        auto found = std::find(limits.begin(), limits.end(), settings.frame_limit);
        int index = found == limits.end() ? 4 : static_cast<int>(found - limits.begin());
        index = (index + sign + static_cast<int>(limits.size())) % static_cast<int>(limits.size());
        settings.frame_limit = limits[static_cast<std::size_t>(index)];
        break;
    }
    case SettingId::ui_scale:
        settings.ui_scale += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::safe_area:
        settings.safe_area += 0.01F * static_cast<float>(sign);
        break;
    case SettingId::presentation:
        settings.presentation = settings.presentation == PresentationProfile::classic
                                    ? PresentationProfile::rebirth
                                    : PresentationProfile::classic;
        break;
    case SettingId::texture_style:
        settings.texture_style =
            cycle(settings.texture_style, sign,
                  std::array{AssetStyle::classic, AssetStyle::classic_hd, AssetStyle::modern});
        break;
    case SettingId::render_scale:
        settings.render_scale += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::texture_quality:
        settings.texture_quality = cycle(settings.texture_quality, sign,
                                         std::array{QualityLevel::low, QualityLevel::medium,
                                                    QualityLevel::high, QualityLevel::ultra});
        break;
    case SettingId::shadow_quality:
        settings.shadow_quality = cycle(settings.shadow_quality, sign,
                                        std::array{QualityLevel::low, QualityLevel::medium,
                                                   QualityLevel::high, QualityLevel::ultra});
        break;
    case SettingId::anti_aliasing:
        settings.anti_aliasing = cycle(settings.anti_aliasing, sign,
                                       std::array{AntiAliasing::off, AntiAliasing::fxaa,
                                                  AntiAliasing::taa, AntiAliasing::msaa4x});
        break;
    case SettingId::ambient_occlusion:
        toggle(settings.ambient_occlusion);
        break;
    case SettingId::ore_glow:
        toggle(settings.ore_glow);
        break;
    case SettingId::particle_quality:
        settings.particle_quality = cycle(settings.particle_quality, sign,
                                          std::array{QualityLevel::low, QualityLevel::medium,
                                                     QualityLevel::high, QualityLevel::ultra});
        break;
    case SettingId::tone_mapping:
        settings.tone_mapping = cycle(settings.tone_mapping, sign,
                                      std::array{ToneMapping::legacy, ToneMapping::reinhard,
                                                 ToneMapping::filmic, ToneMapping::aces});
        break;
    case SettingId::auto_exposure:
        toggle(settings.auto_exposure);
        break;
    case SettingId::exposure_compensation:
        settings.exposure_compensation += 0.1F * static_cast<float>(sign);
        break;
    case SettingId::color_grading:
        settings.color_grading += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::view_distance:
        settings.view_distance += sign;
        break;
    case SettingId::vertical_fov:
        settings.vertical_fov += static_cast<float>(sign);
        break;
    case SettingId::viewmodel_fov:
        settings.viewmodel_fov += static_cast<float>(sign);
        break;
    case SettingId::mouse_sensitivity:
        settings.mouse_sensitivity += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::controller_sensitivity:
        settings.controller_sensitivity += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::invert_y:
        toggle(settings.invert_y);
        break;
    case SettingId::auto_climb:
        toggle(settings.auto_climb);
        break;
    case SettingId::creative_flight_speed:
        settings.creative_flight_speed += static_cast<float>(sign);
        break;
    case SettingId::camera_bob:
        settings.camera_bob += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::screen_shake:
        settings.screen_shake += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::hud_visible:
        toggle(settings.hud_visible);
        break;
    case SettingId::audio_output:
        // Runtime device discovery and cycling live in Application; this
        // platform-neutral settings module has no Core Audio dependency.
        break;
    case SettingId::master_volume:
        settings.master_volume += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::music_volume:
        settings.music_volume += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::effects_volume:
        settings.effects_volume += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::ambience_volume:
        settings.ambience_volume += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::voice_volume:
        settings.voice_volume += 0.05F * static_cast<float>(sign);
        break;
    case SettingId::mute_unfocused:
        toggle(settings.mute_unfocused);
        break;
    case SettingId::raw_mouse_input:
        toggle(settings.raw_mouse_input);
        break;
    case SettingId::controller_vibration:
        toggle(settings.controller_vibration);
        break;
    case SettingId::controller_aim_assist:
        toggle(settings.controller_aim_assist);
        break;
    case SettingId::hold_to_sprint:
        toggle(settings.hold_to_sprint);
        break;
    case SettingId::reduced_motion:
        toggle(settings.reduced_motion);
        break;
    case SettingId::high_contrast:
        toggle(settings.high_contrast);
        break;
    case SettingId::larger_text:
        toggle(settings.larger_text);
        break;
    case SettingId::color_filter:
        settings.color_filter = cycle(settings.color_filter, sign,
                                      std::array{ColorFilter::none, ColorFilter::deuteranopia,
                                                 ColorFilter::protanopia, ColorFilter::tritanopia});
        break;
    case SettingId::subtitles:
        toggle(settings.subtitles);
        break;
    case SettingId::subtitle_size:
        settings.subtitle_size += 0.05F * static_cast<float>(sign);
        break;
    }
    settings.normalize();
}

} // namespace openminerz::ui
