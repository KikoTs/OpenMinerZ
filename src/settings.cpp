#include "openminerz/settings.hpp"

#include "openminerz/graphics_options.hpp"
#include "openminerz/retail_parity.hpp"
#include "openminerz/view_projection.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
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
        // "nan" and "inf" parse successfully but cannot be clamped into range,
        // so a damaged file would otherwise poison the value permanently.
        const float parsed = std::stof(std::string{value});
        return std::isfinite(parsed) ? parsed : fallback;
    }
    catch (...)
    {
        return fallback;
    }
}

std::string_view trimmed(std::string_view value) noexcept
{
    constexpr std::string_view blank{" \t\r\n"};
    const auto first = value.find_first_not_of(blank);
    if (first == std::string_view::npos)
    {
        return {};
    }
    return value.substr(first, value.find_last_not_of(blank) - first + 1U);
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
    if (static_cast<unsigned>(vitals_style) > static_cast<unsigned>(VitalsStyle::none))
        vitals_style = VitalsStyle::remastered;
    // Anything unreadable is OpenMinerZ's own look, never a retail one by accident.
    if (static_cast<unsigned>(ui_style) > static_cast<unsigned>(UiStyle::remastered))
        ui_style = UiStyle::remastered;
    window_width = clamp(window_width, 960, 7680);
    window_height = clamp(window_height, 540, 4320);
    if (static_cast<unsigned>(frame_limit_mode) > static_cast<unsigned>(FrameLimitMode::unlimited))
        frame_limit_mode = FrameLimitMode::fixed;
    if (static_cast<unsigned>(low_latency) > static_cast<unsigned>(LowLatencyMode::on))
        low_latency = LowLatencyMode::on;
    // A hand-written "frame_limit=0" is the natural way to ask for no cap.
    if (frame_limit_mode == FrameLimitMode::fixed && frame_limit <= 0)
        frame_limit_mode = FrameLimitMode::unlimited;
    // The limiter reads frame_limit alone: zero means "no cap", so the mode is
    // folded into the number here instead of being a second thing to consult.
    frame_limit =
        frame_limit_mode == FrameLimitMode::unlimited ? 0 : clamp(frame_limit, 30, 360);
    ui_scale = clamp(ui_scale, 0.75F, 1.50F);
    safe_area = clamp(safe_area, 0.80F, 1.00F);
    render_scale = clamp(render_scale, 0.50F, 2.00F);
    exposure_compensation = clamp(exposure_compensation, -2.0F, 2.0F);
    // The grade shader saturates its strength, so anything above 100% was a
    // slider range that changed nothing on screen.
    color_grading = clamp(color_grading, 0.0F, 1.0F);
    // Terrain visibility is view_distance * 4 blocks and the renderer never
    // draws less than 32, so the old 4..7 steps were four identical settings.
    view_distance = clamp(view_distance, 8, 32);
    gameplay_profile = clamp(gameplay_profile, 0, 1);
    game_mode = clamp(game_mode, 0, 5);
    game_difficulty = clamp(game_difficulty, 0, 3);
    // The "Field of view" slider's range, in vertical degrees (60..133 across a
    // 16:9 display). 105 was always the maximum, so no saved value changes.
    vertical_fov = clamp(vertical_fov, view::minimum_vertical_fov_degrees,
                         view::maximum_vertical_fov_degrees);
    // Advanced, file-only: the hands' own projection. Retail's 90 by default.
    viewmodel_fov = clamp(viewmodel_fov, view::minimum_viewmodel_fov_degrees,
                          view::maximum_viewmodel_fov_degrees);
    if (static_cast<unsigned>(shadow_quality) > static_cast<unsigned>(QualityLevel::ultra))
        shadow_quality = QualityLevel::high;
    if (static_cast<unsigned>(particle_quality) > static_cast<unsigned>(QualityLevel::ultra))
        particle_quality = QualityLevel::high;
    shadow_darkness = clamp(shadow_darkness, 0.25F, 0.90F);
    // Only FXAA exists in the renderer. "taa" and "msaa_4x" were selectable
    // names with no implementation; they meant "smoothing on" to whoever
    // chose them, which is what FXAA provides.
    if (anti_aliasing != AntiAliasing::off)
        anti_aliasing = AntiAliasing::fxaa;
    // Snap to the nearest supported sample count (1 = filtering off).
    texture_filtering = texture_filtering >= 12  ? 16
                        : texture_filtering >= 6 ? 8
                        : texture_filtering >= 3 ? 4
                        : texture_filtering >= 2 ? 2
                                                 : 1;
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
    graphics.normalize();
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
        // Tolerate hand edits ("vsync = 1") and stray carriage returns. Device
        // names keep their inner spaces; only the ends are trimmed.
        const std::string_view key = trimmed(std::string_view{line}.substr(0U, separator));
        if (key.empty() || key.front() == '#')
        {
            continue;
        }
        values.insert_or_assign(std::string{key},
                                std::string{trimmed(std::string_view{line}.substr(separator + 1U))});
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
                               {{"classic", AssetStyle::classic},
                                {"classic_hd", AssetStyle::classic_hd},
                                {"modern", AssetStyle::modern}});
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
    result.ore_glow = parse_bool(value_or(values, "ore_glow"), result.ore_glow);
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
    // Interface style. Three generations of settings file exist:
    //
    //   `interface_style`    this build's key; what it says goes.
    //   `ui_style` only      the build that introduced the two retail styles
    //                        and made `automatic` its default. `classic` or
    //                        `modern` there was the player's own choice and is
    //                        kept. `automatic` was only that build's default,
    //                        so it is NOT a choice of a retail look: the file
    //                        becomes Remastered. `crafting_style` is dropped;
    //                        crafting follows the interface style now.
    //   neither              before any style existed, when Remastered was the
    //                        only interface. It is the default again.
    //
    // A missing, damaged or unknown value is Remastered in every case.
    if (values.contains("interface_style"))
        result.ui_style =
            parse_enum<UiStyle>(value_or(values, "interface_style"), UiStyle::remastered,
                                {{"remastered", UiStyle::remastered},
                                 {"classic", UiStyle::classic},
                                 {"modern", UiStyle::modern},
                                 {"automatic", UiStyle::automatic}});
    else
        result.ui_style = parse_enum<UiStyle>(value_or(values, "ui_style"), UiStyle::remastered,
                                              {{"classic", UiStyle::classic},
                                               {"modern", UiStyle::modern}});
    result.vitals_style =
        parse_enum<VitalsStyle>(value_or(values, "vitals_style"), result.vitals_style,
                                {{"classic", VitalsStyle::classic},
                                 {"modern", VitalsStyle::modern},
                                 {"remastered", VitalsStyle::remastered},
                                 {"none", VitalsStyle::none}});
    result.hud_server = parse_bool(value_or(values, "hud_server"), result.hud_server);
    result.hud_coordinates =
        parse_bool(value_or(values, "hud_coordinates"), result.hud_coordinates);
    result.hud_debug = parse_bool(value_or(values, "hud_debug"), result.hud_debug);
    // The compass is Remastered's (no retail HUD has one) and is believed as
    // the file states it, whichever build wrote the file. The build that
    // introduced the retail styles wrote "hud_compass=0" into every file it
    // loaded; a player who had the compass before that switches it back on
    // once, under Settings > Interface. Guessing here instead would switch it
    // on for the players who had deliberately turned it off.
    result.hud_compass = parse_bool(value_or(values, "hud_compass"), result.hud_compass);
    result.hud_clock = parse_bool(value_or(values, "hud_clock"), result.hud_clock);
    result.hud_distance = parse_bool(value_or(values, "hud_distance"), result.hud_distance);
    result.hud_oxygen = parse_bool(value_or(values, "hud_oxygen"), result.hud_oxygen);
    result.hud_crosshair = parse_bool(value_or(values, "hud_crosshair"), result.hud_crosshair);
    result.hud_target = parse_bool(value_or(values, "hud_target"), result.hud_target);
    result.hud_roster = parse_bool(value_or(values, "hud_roster"), result.hud_roster);
    result.hud_scope = parse_bool(value_or(values, "hud_scope"), result.hud_scope);
    result.hud_hotbar = parse_bool(value_or(values, "hud_hotbar"), result.hud_hotbar);
    result.hud_item_name = parse_bool(value_or(values, "hud_item_name"), result.hud_item_name);
    result.hud_hints = parse_bool(value_or(values, "hud_hints"), result.hud_hints);
    result.hud_chat = parse_bool(value_or(values, "hud_chat"), result.hud_chat);
    result.hud_notifications =
        parse_bool(value_or(values, "hud_notifications"), result.hud_notifications);
    result.hud_damage = parse_bool(value_or(values, "hud_damage"), result.hud_damage);
    result.hud_death = parse_bool(value_or(values, "hud_death"), result.hud_death);
    result.fade_inactive_tray =
        parse_bool(value_or(values, "fade_inactive_tray"), result.fade_inactive_tray);
    result.gameplay_profile = parse_int(value_or(values, "gameplay_profile"), result.gameplay_profile);
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
    result.frame_limit_mode =
        parse_enum<FrameLimitMode>(value_or(values, "frame_limit_mode"), result.frame_limit_mode,
                                   {{"fixed", FrameLimitMode::fixed},
                                    {"display", FrameLimitMode::display},
                                    {"unlimited", FrameLimitMode::unlimited}});
    result.shadows_enabled = parse_bool(value_or(values, "shadows_enabled"), result.shadows_enabled);
    // Before shadow_darkness existed the quality level decided how dark shadows
    // were. Carry that look forward for a file that predates the split, so
    // "ultra" keeps its deep shadows now that quality means resolution.
    result.shadow_darkness =
        values.contains("shadow_darkness")
            ? parse_float(value_or(values, "shadow_darkness"), result.shadow_darkness)
            : (values.contains("shadow_quality") ? legacy_shadow_darkness(result.shadow_quality)
                                                 : result.shadow_darkness);
    result.texture_filtering =
        parse_int(value_or(values, "texture_filtering"), result.texture_filtering);
    result.bloom = parse_bool(value_or(values, "bloom"), result.bloom);
    result.sun_shafts = parse_bool(value_or(values, "sun_shafts"), result.sun_shafts);
    result.show_fps = parse_bool(value_or(values, "show_fps"), result.show_fps);
    result.motion_smoothing =
        parse_bool(value_or(values, "motion_smoothing"), result.motion_smoothing);
    result.xbox_avatar_show_mine =
        parse_bool(value_or(values, "xbox_avatar_show_mine"), result.xbox_avatar_show_mine);
    result.xbox_avatar_show_others =
        parse_bool(value_or(values, "xbox_avatar_show_others"), result.xbox_avatar_show_others);
    result.xbox_avatar_file = value_or(values, "xbox_avatar_file", result.xbox_avatar_file);
    result.low_latency =
        parse_enum<LowLatencyMode>(value_or(values, "low_latency"), result.low_latency,
                                   {{"off", LowLatencyMode::off}, {"on", LowLatencyMode::on}});
    // Non-retail gameplay: absent from the file means OFF, never "default on".
    for (const EnhancementInfo &enhancement : all_enhancements())
        result.*(enhancement.enabled) = parse_bool(value_or(values, enhancement.key), false);
    // Settings::graphics, one key per row of its table (graphics_options.hpp).
    ui::load_graphics_options(result, values);
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
    output << "ore_glow=" << settings.ore_glow << '\n';
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
    // The one style key. `ui_style` / `crafting_style` are never written: a
    // file that has `interface_style` is this build's and is not migrated.
    output << "interface_style=" << to_string(settings.ui_style) << '\n';
    output << "vitals_style=" << to_string(settings.vitals_style) << '\n';
    output << "hud_server=" << settings.hud_server << '\n';
    output << "hud_coordinates=" << settings.hud_coordinates << '\n';
    output << "hud_debug=" << settings.hud_debug << '\n';
    output << "hud_compass=" << settings.hud_compass << '\n';
    output << "hud_clock=" << settings.hud_clock << '\n';
    output << "hud_distance=" << settings.hud_distance << '\n';
    output << "hud_oxygen=" << settings.hud_oxygen << '\n';
    output << "hud_crosshair=" << settings.hud_crosshair << '\n';
    output << "hud_target=" << settings.hud_target << '\n';
    output << "hud_roster=" << settings.hud_roster << '\n';
    output << "hud_scope=" << settings.hud_scope << '\n';
    output << "hud_hotbar=" << settings.hud_hotbar << '\n';
    output << "hud_item_name=" << settings.hud_item_name << '\n';
    output << "hud_hints=" << settings.hud_hints << '\n';
    output << "hud_chat=" << settings.hud_chat << '\n';
    output << "hud_notifications=" << settings.hud_notifications << '\n';
    output << "hud_damage=" << settings.hud_damage << '\n';
    output << "hud_death=" << settings.hud_death << '\n';
    output << "fade_inactive_tray=" << settings.fade_inactive_tray << '\n';
    output << "gameplay_profile=" << settings.gameplay_profile << '\n';
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
    output << "frame_limit_mode=" << to_string(settings.frame_limit_mode) << '\n';
    output << "shadows_enabled=" << settings.shadows_enabled << '\n';
    output << "shadow_darkness=" << settings.shadow_darkness << '\n';
    output << "texture_filtering=" << settings.texture_filtering << '\n';
    output << "bloom=" << settings.bloom << '\n';
    output << "sun_shafts=" << settings.sun_shafts << '\n';
    output << "show_fps=" << settings.show_fps << '\n';
    output << "motion_smoothing=" << settings.motion_smoothing << '\n';
    output << "xbox_avatar_show_mine=" << settings.xbox_avatar_show_mine << '\n';
    output << "xbox_avatar_show_others=" << settings.xbox_avatar_show_others << '\n';
    output << "xbox_avatar_file=" << settings.xbox_avatar_file << '\n';
    output << "low_latency=" << to_string(settings.low_latency) << '\n';
    for (const EnhancementInfo &enhancement : all_enhancements())
        output << enhancement.key << '=' << settings.*(enhancement.enabled) << '\n';
    ui::save_graphics_options(settings, output);
    if (!output)
    {
        throw std::runtime_error{"failed while writing settings file"};
    }
}

std::string to_string(VitalsStyle value)
{
    switch (value)
    {
    case VitalsStyle::classic:
        return "classic";
    case VitalsStyle::modern:
        return "modern";
    case VitalsStyle::remastered:
        return "remastered";
    case VitalsStyle::none:
        return "none";
    }
    return "remastered";
}

std::string to_string(UiStyle value)
{
    switch (value)
    {
    case UiStyle::automatic:
        return "automatic";
    case UiStyle::classic:
        return "classic";
    case UiStyle::modern:
        return "modern";
    case UiStyle::remastered:
        return "remastered";
    }
    return "remastered";
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
    switch (value)
    {
    case AssetStyle::classic:
        return "classic";
    case AssetStyle::classic_hd:
        return "classic_hd";
    case AssetStyle::modern:
        return "modern";
    }
    return "modern";
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

std::string to_string(FrameLimitMode value)
{
    switch (value)
    {
    case FrameLimitMode::fixed:
        return "fixed";
    case FrameLimitMode::display:
        return "display";
    case FrameLimitMode::unlimited:
        return "unlimited";
    }
    return "fixed";
}

std::string to_string(LowLatencyMode value)
{
    return value == LowLatencyMode::off ? "off" : "on";
}

float legacy_shadow_darkness(QualityLevel quality) noexcept
{
    switch (quality)
    {
    case QualityLevel::low:
        return 0.25F;
    case QualityLevel::medium:
        return 0.45F;
    case QualityLevel::high:
        return 0.68F;
    case QualityLevel::ultra:
        return 0.86F;
    }
    return 0.68F;
}

} // namespace openminerz
