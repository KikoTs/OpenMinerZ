#include "openminerz/settings.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace openminerz
{
namespace
{

template <typename T> T clamp(T value, T minimum, T maximum) noexcept
{
    return std::clamp(value, minimum, maximum);
}

bool parse_bool(std::string_view value, bool fallback)
{
    if (value == "true" || value == "1" || value == "on")
    {
        return true;
    }
    if (value == "false" || value == "0" || value == "off")
    {
        return false;
    }
    return fallback;
}

int parse_int(std::string_view value, int fallback)
{
    int parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} ? parsed : fallback;
}

float parse_float(std::string_view value, float fallback)
{
    try
    {
        return std::stof(std::string{value});
    }
    catch (...)
    {
        return fallback;
    }
}

template <typename Enum>
Enum parse_enum(std::string_view value, Enum fallback,
                std::initializer_list<std::pair<std::string_view, Enum>> values)
{
    for (const auto &[name, candidate] : values)
    {
        if (value == name)
        {
            return candidate;
        }
    }
    return fallback;
}

using Values = std::unordered_map<std::string, std::string>;

std::string value_or(const Values &values, std::string_view key, std::string fallback = {})
{
    const auto found = values.find(std::string{key});
    return found == values.end() ? std::move(fallback) : found->second;
}

} // namespace

void Settings::normalize() noexcept
{
    schema_version = 1;
    window_width = clamp(window_width, 960, 7680);
    window_height = clamp(window_height, 540, 4320);
    frame_limit = clamp(frame_limit, 30, 360);
    ui_scale = clamp(ui_scale, 0.75F, 1.50F);
    safe_area = clamp(safe_area, 0.80F, 1.00F);
    render_scale = clamp(render_scale, 0.50F, 2.00F);
    exposure_compensation = clamp(exposure_compensation, -2.0F, 2.0F);
    color_grading = clamp(color_grading, 0.0F, 1.5F);
    view_distance = clamp(view_distance, 4, 32);
    game_mode = clamp(game_mode, 0, 5);
    game_difficulty = clamp(game_difficulty, 0, 3);
    vertical_fov = clamp(vertical_fov, 50.0F, 110.0F);
    viewmodel_fov = clamp(viewmodel_fov, 55.0F, 110.0F);
    mouse_sensitivity = clamp(mouse_sensitivity, 0.10F, 4.00F);
    controller_sensitivity = clamp(controller_sensitivity, 0.10F, 4.00F);
    creative_flight_speed = clamp(creative_flight_speed, 4.0F, 32.0F);
    camera_bob = clamp(camera_bob, 0.0F, 1.0F);
    screen_shake = clamp(screen_shake, 0.0F, 1.0F);
    master_volume = clamp(master_volume, 0.0F, 1.0F);
    music_volume = clamp(music_volume, 0.0F, 1.0F);
    effects_volume = clamp(effects_volume, 0.0F, 1.0F);
    ambience_volume = clamp(ambience_volume, 0.0F, 1.0F);
    voice_volume = clamp(voice_volume, 0.0F, 1.0F);
    subtitle_size = clamp(subtitle_size, 0.75F, 1.50F);
}

SettingsStore::SettingsStore(std::filesystem::path path) : path_(std::move(path))
{
}

Settings SettingsStore::load() const
{
    Settings result;
    std::ifstream input{path_};
    if (!input)
    {
        return result;
    }

    Values values;
    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0U)
        {
            continue;
        }
        values.insert_or_assign(line.substr(0U, separator), line.substr(separator + 1U));
    }

    result.window_mode = parse_enum<WindowMode>(value_or(values, "window_mode"), result.window_mode,
                                                {{"windowed", WindowMode::windowed},
                                                 {"borderless", WindowMode::borderless},
                                                 {"fullscreen", WindowMode::fullscreen}});
    result.window_width = parse_int(value_or(values, "window_width"), result.window_width);
    result.window_height = parse_int(value_or(values, "window_height"), result.window_height);
    result.vsync = parse_bool(value_or(values, "vsync"), result.vsync);
    result.frame_limit = parse_int(value_or(values, "frame_limit"), result.frame_limit);
    result.ui_scale = parse_float(value_or(values, "ui_scale"), result.ui_scale);
    result.safe_area = parse_float(value_or(values, "safe_area"), result.safe_area);
    result.presentation = parse_enum<PresentationProfile>(
        value_or(values, "presentation"), result.presentation,
        {{"classic", PresentationProfile::classic}, {"rebirth", PresentationProfile::rebirth}});
    result.texture_style =
        parse_enum<AssetStyle>(value_or(values, "texture_style"), result.texture_style,
                               {{"classic", AssetStyle::classic}, {"modern", AssetStyle::modern}});
    result.render_scale = parse_float(value_or(values, "render_scale"), result.render_scale);
    result.texture_quality =
        parse_enum<QualityLevel>(value_or(values, "texture_quality"), result.texture_quality,
                                 {{"low", QualityLevel::low},
                                  {"medium", QualityLevel::medium},
                                  {"high", QualityLevel::high},
                                  {"ultra", QualityLevel::ultra}});
    result.shadow_quality =
        parse_enum<QualityLevel>(value_or(values, "shadow_quality"), result.shadow_quality,
                                 {{"low", QualityLevel::low},
                                  {"medium", QualityLevel::medium},
                                  {"high", QualityLevel::high},
                                  {"ultra", QualityLevel::ultra}});
    result.anti_aliasing =
        parse_enum<AntiAliasing>(value_or(values, "anti_aliasing"), result.anti_aliasing,
                                 {{"off", AntiAliasing::off},
                                  {"fxaa", AntiAliasing::fxaa},
                                  {"taa", AntiAliasing::taa},
                                  {"msaa_4x", AntiAliasing::msaa4x}});
    result.ambient_occlusion =
        parse_bool(value_or(values, "ambient_occlusion"), result.ambient_occlusion);
    result.particle_quality =
        parse_enum<QualityLevel>(value_or(values, "particle_quality"), result.particle_quality,
                                 {{"low", QualityLevel::low},
                                  {"medium", QualityLevel::medium},
                                  {"high", QualityLevel::high},
                                  {"ultra", QualityLevel::ultra}});
    result.tone_mapping =
        parse_enum<ToneMapping>(value_or(values, "tone_mapping"), result.tone_mapping,
                                {{"legacy", ToneMapping::legacy},
                                 {"reinhard", ToneMapping::reinhard},
                                 {"filmic", ToneMapping::filmic},
                                 {"aces", ToneMapping::aces}});
    result.exposure_compensation =
        parse_float(value_or(values, "exposure_compensation"), result.exposure_compensation);
    result.auto_exposure = parse_bool(value_or(values, "auto_exposure"), result.auto_exposure);
    result.color_grading = parse_float(value_or(values, "color_grading"), result.color_grading);
    result.view_distance = parse_int(value_or(values, "view_distance"), result.view_distance);
    result.vertical_fov = parse_float(value_or(values, "vertical_fov"), result.vertical_fov);
    result.viewmodel_fov = parse_float(value_or(values, "viewmodel_fov"), result.viewmodel_fov);
    result.mouse_sensitivity =
        parse_float(value_or(values, "mouse_sensitivity"), result.mouse_sensitivity);
    result.controller_sensitivity =
        parse_float(value_or(values, "controller_sensitivity"), result.controller_sensitivity);
    result.invert_y = parse_bool(value_or(values, "invert_y"), result.invert_y);
    result.auto_climb = parse_bool(value_or(values, "auto_climb"), result.auto_climb);
    result.creative_flight_speed =
        parse_float(value_or(values, "creative_flight_speed"), result.creative_flight_speed);
    result.camera_bob = parse_float(value_or(values, "camera_bob"), result.camera_bob);
    result.screen_shake = parse_float(value_or(values, "screen_shake"), result.screen_shake);
    result.hud_visible = parse_bool(value_or(values, "hud_visible"), result.hud_visible);
    result.crafting_menu_modern =
        parse_bool(value_or(values, "crafting_menu_modern"), result.crafting_menu_modern);
    result.game_mode = parse_int(value_or(values, "game_mode"), result.game_mode);
    result.game_difficulty = parse_int(value_or(values, "game_difficulty"), result.game_difficulty);
    result.master_volume = parse_float(value_or(values, "master_volume"), result.master_volume);
    result.music_volume = parse_float(value_or(values, "music_volume"), result.music_volume);
    result.effects_volume = parse_float(value_or(values, "effects_volume"), result.effects_volume);
    result.ambience_volume =
        parse_float(value_or(values, "ambience_volume"), result.ambience_volume);
    result.voice_volume = parse_float(value_or(values, "voice_volume"), result.voice_volume);
    result.mute_unfocused = parse_bool(value_or(values, "mute_unfocused"), result.mute_unfocused);
    result.audio_output_id = value_or(values, "audio_output_id", result.audio_output_id);
    result.audio_output_name = value_or(values, "audio_output_name", result.audio_output_name);
    result.raw_mouse_input =
        parse_bool(value_or(values, "raw_mouse_input"), result.raw_mouse_input);
    result.controller_vibration =
        parse_bool(value_or(values, "controller_vibration"), result.controller_vibration);
    result.controller_aim_assist =
        parse_bool(value_or(values, "controller_aim_assist"), result.controller_aim_assist);
    result.hold_to_sprint = parse_bool(value_or(values, "hold_to_sprint"), result.hold_to_sprint);
    result.reduced_motion = parse_bool(value_or(values, "reduced_motion"), result.reduced_motion);
    result.high_contrast = parse_bool(value_or(values, "high_contrast"), result.high_contrast);
    result.larger_text = parse_bool(value_or(values, "larger_text"), result.larger_text);
    result.color_filter =
        parse_enum<ColorFilter>(value_or(values, "color_filter"), result.color_filter,
                                {{"none", ColorFilter::none},
                                 {"deuteranopia", ColorFilter::deuteranopia},
                                 {"protanopia", ColorFilter::protanopia},
                                 {"tritanopia", ColorFilter::tritanopia}});
    result.subtitles = parse_bool(value_or(values, "subtitles"), result.subtitles);
    result.subtitle_size = parse_float(value_or(values, "subtitle_size"), result.subtitle_size);
    result.normalize();
    return result;
}

void SettingsStore::save(const Settings &source) const
{
    Settings settings = source;
    settings.normalize();
    if (!path_.parent_path().empty())
    {
        std::filesystem::create_directories(path_.parent_path());
    }
    std::ofstream output{path_, std::ios::trunc};
    if (!output)
    {
        throw std::runtime_error{"could not open settings file for writing"};
    }
    output << "# OpenMinerZ settings v1\n";
    output << "window_mode=" << to_string(settings.window_mode) << '\n';
    output << "window_width=" << settings.window_width << '\n';
    output << "window_height=" << settings.window_height << '\n';
    output << "vsync=" << settings.vsync << '\n';
    output << "frame_limit=" << settings.frame_limit << '\n';
    output << "ui_scale=" << settings.ui_scale << '\n';
    output << "safe_area=" << settings.safe_area << '\n';
    output << "presentation=" << to_string(settings.presentation) << '\n';
    output << "texture_style=" << to_string(settings.texture_style) << '\n';
    output << "render_scale=" << settings.render_scale << '\n';
    output << "texture_quality=" << to_string(settings.texture_quality) << '\n';
    output << "shadow_quality=" << to_string(settings.shadow_quality) << '\n';
    output << "anti_aliasing=" << to_string(settings.anti_aliasing) << '\n';
    output << "ambient_occlusion=" << settings.ambient_occlusion << '\n';
    output << "particle_quality=" << to_string(settings.particle_quality) << '\n';
    output << "tone_mapping=" << to_string(settings.tone_mapping) << '\n';
    output << "exposure_compensation=" << settings.exposure_compensation << '\n';
    output << "auto_exposure=" << settings.auto_exposure << '\n';
    output << "color_grading=" << settings.color_grading << '\n';
    output << "view_distance=" << settings.view_distance << '\n';
    output << "vertical_fov=" << settings.vertical_fov << '\n';
    output << "viewmodel_fov=" << settings.viewmodel_fov << '\n';
    output << "mouse_sensitivity=" << settings.mouse_sensitivity << '\n';
    output << "controller_sensitivity=" << settings.controller_sensitivity << '\n';
    output << "invert_y=" << settings.invert_y << '\n';
    output << "auto_climb=" << settings.auto_climb << '\n';
    output << "creative_flight_speed=" << settings.creative_flight_speed << '\n';
    output << "camera_bob=" << settings.camera_bob << '\n';
    output << "screen_shake=" << settings.screen_shake << '\n';
    output << "hud_visible=" << settings.hud_visible << '\n';
    output << "crafting_menu_modern=" << settings.crafting_menu_modern << '\n';
    output << "game_mode=" << settings.game_mode << '\n';
    output << "game_difficulty=" << settings.game_difficulty << '\n';
    output << "master_volume=" << settings.master_volume << '\n';
    output << "music_volume=" << settings.music_volume << '\n';
    output << "effects_volume=" << settings.effects_volume << '\n';
    output << "ambience_volume=" << settings.ambience_volume << '\n';
    output << "voice_volume=" << settings.voice_volume << '\n';
    output << "mute_unfocused=" << settings.mute_unfocused << '\n';
    output << "audio_output_id=" << settings.audio_output_id << '\n';
    output << "audio_output_name=" << settings.audio_output_name << '\n';
    output << "raw_mouse_input=" << settings.raw_mouse_input << '\n';
    output << "controller_vibration=" << settings.controller_vibration << '\n';
    output << "controller_aim_assist=" << settings.controller_aim_assist << '\n';
    output << "hold_to_sprint=" << settings.hold_to_sprint << '\n';
    output << "reduced_motion=" << settings.reduced_motion << '\n';
    output << "high_contrast=" << settings.high_contrast << '\n';
    output << "larger_text=" << settings.larger_text << '\n';
    output << "color_filter=" << to_string(settings.color_filter) << '\n';
    output << "subtitles=" << settings.subtitles << '\n';
    output << "subtitle_size=" << settings.subtitle_size << '\n';
    if (!output)
    {
        throw std::runtime_error{"failed while writing settings file"};
    }
}

std::string to_string(WindowMode value)
{
    switch (value)
    {
    case WindowMode::windowed:
        return "windowed";
    case WindowMode::borderless:
        return "borderless";
    case WindowMode::fullscreen:
        return "fullscreen";
    }
    return "windowed";
}

std::string to_string(PresentationProfile value)
{
    return value == PresentationProfile::classic ? "classic" : "rebirth";
}

std::string to_string(AssetStyle value)
{
    return value == AssetStyle::classic ? "classic" : "modern";
}

std::string to_string(QualityLevel value)
{
    switch (value)
    {
    case QualityLevel::low:
        return "low";
    case QualityLevel::medium:
        return "medium";
    case QualityLevel::high:
        return "high";
    case QualityLevel::ultra:
        return "ultra";
    }
    return "high";
}

std::string to_string(AntiAliasing value)
{
    switch (value)
    {
    case AntiAliasing::off:
        return "off";
    case AntiAliasing::fxaa:
        return "fxaa";
    case AntiAliasing::taa:
        return "taa";
    case AntiAliasing::msaa4x:
        return "msaa_4x";
    }
    return "taa";
}

std::string to_string(ToneMapping value)
{
    switch (value)
    {
    case ToneMapping::legacy:
        return "legacy";
    case ToneMapping::reinhard:
        return "reinhard";
    case ToneMapping::filmic:
        return "filmic";
    case ToneMapping::aces:
        return "aces";
    }
    return "aces";
}

std::string to_string(ColorFilter value)
{
    switch (value)
    {
    case ColorFilter::none:
        return "none";
    case ColorFilter::deuteranopia:
        return "deuteranopia";
    case ColorFilter::protanopia:
        return "protanopia";
    case ColorFilter::tritanopia:
        return "tritanopia";
    }
    return "none";
}

} // namespace openminerz
