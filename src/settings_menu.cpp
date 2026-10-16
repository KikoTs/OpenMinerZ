#include "openminerz/settings_menu.hpp"

#include "openminerz/graphics_options.hpp"
#include "openminerz/view_projection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace openminerz::ui
{
namespace
{

// Every description is written for someone who has never opened a graphics
// menu: what the option changes on screen, then what it costs. Technical names
// appear only in brackets after the plain one. The settings screen shows two
// wrapped lines, so keep each under about 170 characters (a unit test checks).
const std::vector<SettingRow> display_rows{
    {SettingId::window_mode, "Display mode",
     "Windowed is a movable window. Borderless fills the screen and switches to other apps "
     "instantly. Fullscreen takes over the display at the resolution below.",
     SettingKind::choice},
    {SettingId::resolution, "Resolution",
     "Size of the game window, or the screen resolution in Fullscreen. Higher is sharper and "
     "lowers the frame rate.",
     SettingKind::choice},
    {SettingId::exposure_compensation, "Brightness",
     "Makes the whole picture brighter or darker. Raise it if caves and nights are too dark on "
     "your screen.",
     SettingKind::slider},
    // The ONE field of view control: the world camera's vertical angle, Hor+
    // (openminerz/view_projection.hpp). The first-person body and held item
    // are drawn with retail's own fixed projection and never follow it, so
    // there is deliberately no second slider for them; Settings::viewmodel_fov
    // remains an advanced value in the settings file only.
    {SettingId::vertical_fov, "Field of view",
     "Angle of your view of the world from top to bottom; a wider screen adds more at the "
     "sides. Higher sees more and looks smaller. Your hands and held item never change.",
     SettingKind::slider},
};

// The rows of Settings::graphics (shadow distance and softness, ambient
// shading, contact shadows, clouds, texture detail) come from the table in
// graphics_options.hpp and are placed after the row each one names.
const std::vector<SettingRow> graphics_rows = with_graphics_options(SettingsTab::graphics, {
    {SettingId::quality_preset, "Overall quality",
     "Sets view distance, shadows, shading, clouds, light shafts, bloom, particles and texture "
     "sharpness together. Changing one of them yourself shows Custom.",
     SettingKind::choice},
    {SettingId::presentation, "Lighting style",
     "Original is the lighting and colour of the 2011 game. Enhanced adds natural sunlight, "
     "softer shadows, haze and richer materials. Switches at once.",
     SettingKind::choice},
    {SettingId::texture_style, "Texture pack",
     "The artwork for blocks, items, creatures and sky: Classic (2011), Classic HD (sharpened "
     "Classic) or Modern (current release). Not the same as Lighting style.",
     SettingKind::choice, true},
    {SettingId::view_distance, "View distance",
     "How far away terrain is drawn before it fades into the haze. Farther shows more of the "
     "world. Medium to high cost.",
     SettingKind::slider},
    {SettingId::shadow_quality, "Shadows",
     "Sun shadows from terrain, trees and creatures. Higher is sharper and reaches farther. "
     "Off is the biggest single speed-up.",
     SettingKind::choice},
    {SettingId::shadow_darkness, "Shadow darkness",
     "How dark sun shadows are. A matter of taste. No cost.", SettingKind::slider},
    {SettingId::anti_aliasing, "Edge smoothing",
     "Softens the jagged staircase look on block edges (FXAA anti-aliasing). Very low cost. For "
     "cleaner edges still, raise Resolution scale on the Performance page.",
     SettingKind::toggle},
    {SettingId::texture_filtering, "Texture sharpness",
     "Keeps the ground and walls crisp when you look along them instead of smearing into the "
     "distance (anisotropic filtering). Low cost.",
     SettingKind::choice},
    {SettingId::particle_quality, "Particles",
     "How many sparks, smoke puffs and rock chips effects throw out. Lower helps most in big "
     "fights and explosions.",
     SettingKind::choice},
});

// Likewise for the lens and colour rows of Settings::graphics.
const std::vector<SettingRow> effects_rows = with_graphics_options(SettingsTab::effects, {
    {SettingId::bloom, "Bloom",
     "A soft glow that spreads from very bright things: the sun, lava, torches, gunfire and "
     "laser bolts. It moves light around without adding any. Low cost.",
     SettingKind::toggle},
    {SettingId::sun_shafts, "Light shafts",
     "Light and shadow in the air itself: beams past hills and trees and through cave "
     "openings, and haze that deepens with distance. Medium cost.",
     SettingKind::toggle},
    {SettingId::ambient_occlusion, "Corner shading",
     "Darkens the creases where blocks meet so shapes stand out. It is stored with the terrain, "
     "so it works at every quality level. Ambient shading adds to it. No cost.",
     SettingKind::toggle},
    {SettingId::ore_glow, "Ore glow",
     "Gives exposed ore a faint shimmer so veins are easier to spot. Looks only: it does not "
     "light the cave. No cost.",
     SettingKind::toggle},
    {SettingId::auto_exposure, "Eye adaptation",
     "Tones down the glare in very bright places such as snow and desert, the way eyes adjust. "
     "Turn off for a fixed brightness.",
     SettingKind::toggle},
    {SettingId::tone_mapping, "Highlight style",
     "How very bright areas are fitted onto the screen (tone mapping). Cinematic keeps detail in "
     "sunlit sand and snow, Punchy adds contrast, Basic can wash out.",
     SettingKind::choice},
    {SettingId::color_grading, "Colour finish",
     "Strength of the final colour and contrast polish. 0% shows the raw picture, 100% is the "
     "intended look. No cost.",
     SettingKind::slider},
});

const std::vector<SettingRow> performance_rows{
    {SettingId::render_scale, "Resolution scale",
     "Draws the 3D world at a fraction or multiple of the screen resolution; menus stay sharp. "
     "Below 100% is faster and blurrier. Above 100% is sharper and costs a lot.",
     SettingKind::slider},
    {SettingId::vsync, "VSync",
     "Shows only whole frames, in step with the display, so the picture never tears. Holds the "
     "frame rate at the display's refresh rate.",
     SettingKind::toggle},
    {SettingId::frame_limit, "Frame rate limit",
     "The most frames per second the game will draw. A limit keeps the PC cooler and quieter. "
     "Match display follows your monitor. Unlimited draws as fast as it can.",
     SettingKind::choice},
    {SettingId::low_latency, "Reduced input delay",
     "Shortens the wait between moving the mouse and seeing the view turn, by not letting "
     "finished frames queue up. Leave on unless the picture stutters.",
     SettingKind::toggle},
    {SettingId::motion_smoothing, "Smooth movement",
     "Blends movement between the game's 60 updates per second so the camera and creatures "
     "glide on fast displays. Off shows the raw steps, which can judder.",
     SettingKind::toggle},
    {SettingId::show_fps, "FPS counter",
     "Shows frames per second and how long each frame takes in the top-right corner.",
     SettingKind::toggle},
};

const std::vector<SettingRow> gameplay_rows{
    {SettingId::auto_climb, "Auto climb", "Step up onto one-block ledges without jumping.",
     SettingKind::toggle},
    {SettingId::creative_flight_speed, "Creative flight speed",
     "How fast the Free flight extra flies. The original game's own flight has one fixed speed.",
     SettingKind::slider},
    {SettingId::camera_bob, "View bobbing",
     "How much the camera sways as you walk. 0% keeps it perfectly steady.", SettingKind::slider},
    {SettingId::screen_shake, "Camera shake",
     "How much the view kicks when you swing a tool or fire a weapon. 0% turns it off.",
     SettingKind::slider},
};

const std::vector<SettingRow> control_rows{
    {SettingId::mouse_sensitivity, "Mouse sensitivity",
     "How far the view turns when you move the mouse.", SettingKind::slider},
    {SettingId::raw_mouse_input, "Raw mouse input",
     "Reads the mouse directly and ignores the Windows pointer speed and acceleration settings, "
     "so the same hand movement always turns you the same amount.",
     SettingKind::toggle},
    {SettingId::invert_y, "Invert vertical look",
     "Pushing the mouse or stick forward looks down instead of up.", SettingKind::toggle},
    {SettingId::controller_sensitivity, "Controller sensitivity",
     "How fast the view turns with the right stick.", SettingKind::slider},
    {SettingId::controller_vibration, "Controller vibration", "Rumble on supported gamepads.",
     SettingKind::toggle},
};

const std::vector<SettingRow> interface_rows{
    // The style row leads the page. One choice sets the HUD and the inventory,
    // crate and crafting screens together; there is no separate crafting row
    // and no way to mix two looks. Looks only: the rules come from the world.
    {SettingId::ui_style, "Interface style",
     "The HUD, inventory, crates and crafting together. Remastered is OpenMinerZ's own; Classic "
     "and Modern copy the original games exactly; Match world picks one of those.",
     SettingKind::choice},
    {SettingId::hud_visible, "Show HUD", "F2 toggles all overlays. Menus still open normally.",
     SettingKind::toggle},
    {SettingId::vitals_style, "Health and energy",
     "Remastered: bars over the item tray, a row of hearts, bars at the top, or hidden. Classic "
     "and Modern draw the original meters, so there it is only shown or hidden.",
     SettingKind::choice},
    {SettingId::ui_scale, "Interface scale",
     "Size of menus and of the Remastered HUD, inventory and crafting. The Classic and Modern "
     "styles keep the original game's size.",
     SettingKind::slider},
    {SettingId::safe_area, "Screen margins",
     "Keeps menus and the Remastered HUD clear of the screen edges. The Classic and Modern HUD "
     "are placed exactly as the original game places them.",
     SettingKind::slider},
    {SettingId::hud_server, "Server information", "Server name and players at the top left.",
     SettingKind::toggle},
    {SettingId::hud_coordinates, "World coordinates", "Your position at the top left.",
     SettingKind::toggle},
    {SettingId::hud_debug, "Debug information", "F3 toggles statistics at the bottom right.",
     SettingKind::toggle},
    {SettingId::hud_compass, "Compass",
     "Heading strip at the top of the screen. Part of the Remastered style.", SettingKind::toggle},
    {SettingId::hud_clock, "Clock", "Time of day and day count at the top right. Part of the "
                                    "Remastered style.",
     SettingKind::toggle},
    {SettingId::hud_distance, "Distance", "Distance from spawn and your record.",
     SettingKind::toggle},
    {SettingId::hud_oxygen, "Oxygen",
     "Air remaining while underwater. Only the Lagoon water extra has water to swim in.",
     SettingKind::toggle},
    {SettingId::hud_crosshair, "Crosshair", "The aiming mark in the centre of the screen.",
     SettingKind::toggle},
    {SettingId::hud_target, "Block highlight",
     "Outline and name of the block you are aiming at.", SettingKind::toggle},
    {SettingId::hud_roster, "Player list", "Show players when holding Tab.", SettingKind::toggle},
    {SettingId::hud_scope, "Scope overlay", "Scope frame and missile lock indicator.",
     SettingKind::toggle},
    {SettingId::hud_hotbar, "Hotbar", "Quick access inventory slots.", SettingKind::toggle},
    {SettingId::hud_item_name, "Selected item", "Item name, ammunition and waypoint distance.",
     SettingKind::toggle},
    {SettingId::hud_hints, "Control hints",
     "Slot numbers and key or button prompts on the item tray. Part of the Remastered style.",
     SettingKind::toggle},
    {SettingId::hud_chat, "Chat feed", "Recent multiplayer messages.", SettingKind::toggle},
    {SettingId::hud_notifications, "Notifications",
     "Notices such as items picked up or crafted, new areas and the time of day. Part of the "
     "Remastered style; the original games show none.",
     SettingKind::toggle},
    {SettingId::hud_damage, "Damage effects", "Red damage tint and attack direction.",
     SettingKind::toggle},
    {SettingId::hud_death, "Death screen", "Respawn prompt and death summary.",
     SettingKind::toggle},
    {SettingId::fade_inactive_tray, "Fade inactive item tray",
     "The Modern game's own option, for the Modern style: the tray you are not using is drawn "
     "at 40% (Faded) instead of solid (Regular). Off by default.",
     SettingKind::toggle},
    {SettingId::subtitles, "Subtitles", "Captions for game sounds.", SettingKind::toggle},
    {SettingId::subtitle_size, "Subtitle size", "Size of sound captions.", SettingKind::slider},
};

const std::vector<SettingRow> audio_rows{
    {SettingId::audio_output, "Output device",
     "Which speakers or headset the game plays through. System default follows Windows.",
     SettingKind::choice, true},
    {SettingId::master_volume, "Master volume", "Overall output level.", SettingKind::slider},
    {SettingId::music_volume, "Music", "Menu and gameplay music level.", SettingKind::slider},
    {SettingId::effects_volume, "Effects", "Weapons, blocks, creatures, and interaction level.",
     SettingKind::slider},
    {SettingId::ambience_volume, "Ambience", "Weather, cave, wind, and environment level.",
     SettingKind::slider},
    {SettingId::voice_volume, "Voice chat", "How loud other players' voices are.",
     SettingKind::slider},
    {SettingId::mute_unfocused, "Mute in background",
     "Silence the game while another application has focus.", SettingKind::toggle},
};

const std::vector<SettingRow> accessibility_rows{
    {SettingId::reduced_motion, "Reduced motion",
     "Turns off view bobbing, camera shake, drifting embers and menu animation, and softens "
     "lightning flashes.",
     SettingKind::toggle},
    {SettingId::high_contrast, "High contrast interface",
     "Make text and panels easier to distinguish.", SettingKind::toggle},
    {SettingId::larger_text, "Larger text", "Increase menu text size.", SettingKind::toggle},
};

constexpr int first_enhancement_setting = static_cast<int>(SettingId::enhancement_lagoon_water);
static_assert(static_cast<int>(SettingId::enhancement_free_flight) - first_enhancement_setting +
                      1 ==
                  static_cast<int>(enhancement_count),
              "every Enhancement needs exactly one SettingId, in the same order");

// Built from retail_parity.hpp, so this page cannot offer a feature the gate
// does not know about, or describe one differently from the register.
const std::vector<SettingRow> enhancement_rows = [] {
    std::vector<SettingRow> rows;
    for (const EnhancementInfo &info : all_enhancements())
        rows.push_back({static_cast<SettingId>(first_enhancement_setting +
                                               static_cast<int>(info.id)),
                        info.label, info.description, SettingKind::toggle});
    return rows;
}();

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

std::string on_off(bool value)
{
    return value ? "ON" : "OFF";
}

// Values are shown in capitals; the persistence names are lower case.
std::string shown(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'a' && character <= 'z' ? character - 32 : character);
    });
    return value;
}

// What each quality preset sets. HIGH must stay equal to the factory defaults
// in Settings so a fresh install reads "HIGH" rather than "CUSTOM" (tested).
struct PresetValues final
{
    int view_distance;
    bool shadows_enabled;
    QualityLevel shadow_quality;
    AntiAliasing anti_aliasing;
    int texture_filtering;
    QualityLevel particle_quality;
    bool bloom;
    bool sun_shafts;
};

constexpr std::array<PresetValues, 4> preset_values{{
    {8, false, QualityLevel::low, AntiAliasing::off, 1, QualityLevel::low, false, false},
    {12, true, QualityLevel::medium, AntiAliasing::fxaa, 4, QualityLevel::medium, true, true},
    {16, true, QualityLevel::high, AntiAliasing::fxaa, 8, QualityLevel::high, true, true},
    {32, true, QualityLevel::ultra, AntiAliasing::fxaa, 16, QualityLevel::ultra, true, true},
}};

bool matches_preset(const Settings &settings, const PresetValues &preset) noexcept
{
    // With shadows off the stored shadow quality draws nothing, so it must not
    // be able to turn a preset into "custom".
    const bool shadows_match =
        settings.shadows_enabled == preset.shadows_enabled &&
        (!settings.shadows_enabled || settings.shadow_quality == preset.shadow_quality);
    return settings.view_distance == preset.view_distance && shadows_match &&
           (settings.anti_aliasing == AntiAliasing::off) ==
               (preset.anti_aliasing == AntiAliasing::off) &&
           settings.texture_filtering == preset.texture_filtering &&
           settings.particle_quality == preset.particle_quality &&
           settings.bloom == preset.bloom && settings.sun_shafts == preset.sun_shafts &&
           // The preset-controlled half of Settings::graphics, same level.
           graphics_matches_preset(settings.graphics,
                                   static_cast<int>(&preset - preset_values.data()),
                                   settings.shadows_enabled, settings.sun_shafts);
}

constexpr std::array built_in_resolutions{std::pair{1280, 720},  std::pair{1600, 900},
                                          std::pair{1920, 1080}, std::pair{2560, 1440},
                                          std::pair{3440, 1440}, std::pair{3840, 2160}};

constexpr std::array frame_limit_steps{30, 60, 90, 120, 144, 165, 240, 360};
constexpr std::array texture_filtering_steps{1, 2, 4, 8, 16};

} // namespace

std::optional<Enhancement> setting_enhancement(SettingId id) noexcept
{
    const int index = static_cast<int>(id) - first_enhancement_setting;
    if (index < 0 || index >= static_cast<int>(enhancement_count))
        return std::nullopt;
    return static_cast<Enhancement>(index);
}

std::string_view quality_preset_label(QualityPreset preset) noexcept
{
    switch (preset)
    {
    case QualityPreset::low:
        return "LOW";
    case QualityPreset::medium:
        return "MEDIUM";
    case QualityPreset::high:
        return "HIGH";
    case QualityPreset::ultra:
        return "ULTRA";
    case QualityPreset::custom:
        return "CUSTOM";
    }
    return "CUSTOM";
}

QualityPreset detect_quality_preset(const Settings &settings) noexcept
{
    for (std::size_t index = 0U; index < preset_values.size(); ++index)
    {
        if (matches_preset(settings, preset_values[index]))
        {
            return static_cast<QualityPreset>(index);
        }
    }
    return QualityPreset::custom;
}

void apply_quality_preset(Settings &settings, QualityPreset preset) noexcept
{
    const auto index = static_cast<std::size_t>(preset);
    if (index >= preset_values.size())
    {
        return;
    }
    const PresetValues &values = preset_values[index];
    settings.view_distance = values.view_distance;
    settings.shadows_enabled = values.shadows_enabled;
    settings.shadow_quality = values.shadow_quality;
    settings.anti_aliasing = values.anti_aliasing;
    settings.texture_filtering = values.texture_filtering;
    settings.particle_quality = values.particle_quality;
    settings.bloom = values.bloom;
    settings.sun_shafts = values.sun_shafts;
    apply_graphics_preset(settings.graphics, static_cast<int>(index));
}

std::string_view setting_unavailable_reason(const Settings &settings, SettingId id) noexcept
{
    if (const GraphicsOption *option = graphics_option(id))
        return option->unavailable != nullptr ? option->unavailable(settings) : std::string_view{};
    switch (id)
    {
    case SettingId::resolution:
        return settings.window_mode == WindowMode::borderless
                   ? "Borderless always uses your desktop resolution."
                   : std::string_view{};
    case SettingId::shadow_darkness:
        return settings.shadows_enabled ? std::string_view{} : "Shadows are turned off.";
    case SettingId::frame_limit:
        // With VSync on the display paces the game and the limiter stands down.
        return settings.vsync ? "VSync is on, so the display sets the frame rate."
                              : std::string_view{};
    default:
        break;
    }
    // OpenMinerZ's own HUD pieces exist in the Remastered style only. The
    // stored choice is kept for when the player comes back to it.
    if (setting_is_remastered_only(id) && settings.ui_style != UiStyle::remastered)
        return "Only in the Remastered style. Classic and Modern copy the original HUD exactly.";
    return {};
}

bool setting_is_remastered_only(SettingId id) noexcept
{
    return id == SettingId::hud_compass || id == SettingId::hud_clock ||
           id == SettingId::hud_hints || id == SettingId::hud_notifications;
}

void adjust_resolution(Settings &settings, int direction,
                       std::span<const std::pair<int, int>> modes)
{
    if (!setting_unavailable_reason(settings, SettingId::resolution).empty())
    {
        return;
    }
    std::vector<std::pair<int, int>> sorted{modes.begin(), modes.end()};
    if (sorted.empty())
    {
        sorted.assign(built_in_resolutions.begin(), built_in_resolutions.end());
    }
    std::erase_if(sorted, [](const auto &mode) { return mode.first < 960 || mode.second < 540; });
    const auto by_size = [](const auto &left, const auto &right) {
        const long long left_area = static_cast<long long>(left.first) * left.second;
        const long long right_area = static_cast<long long>(right.first) * right.second;
        return left_area != right_area ? left_area < right_area : left.first < right.first;
    };
    std::sort(sorted.begin(), sorted.end(), by_size);
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.empty())
    {
        return;
    }
    const std::pair current{settings.window_width, settings.window_height};
    const int count = static_cast<int>(sorted.size());
    const auto found = std::find(sorted.begin(), sorted.end(), current);
    int index{};
    if (found != sorted.end())
    {
        index = (static_cast<int>(found - sorted.begin()) + (direction < 0 ? -1 : 1) + count) %
                count;
    }
    else
    {
        // A size the monitor does not list (a dragged window, another PC's
        // file): step to the nearest listed size in the requested direction.
        const auto upper = std::upper_bound(sorted.begin(), sorted.end(), current, by_size);
        const int above = static_cast<int>(upper - sorted.begin());
        index = direction < 0 ? std::max(0, above - 1) : std::min(count - 1, above);
    }
    settings.window_width = sorted[static_cast<std::size_t>(index)].first;
    settings.window_height = sorted[static_cast<std::size_t>(index)].second;
    settings.normalize();
}

void reset_setting(Settings &settings, SettingId id)
{
    const Settings defaults{};
    if (const GraphicsOption *option = graphics_option(id))
    {
        reset_graphics_option(*option, settings);
        settings.normalize();
        return;
    }
    switch (id)
    {
    case SettingId::window_mode:
        settings.window_mode = defaults.window_mode;
        break;
    case SettingId::resolution:
        settings.window_width = defaults.window_width;
        settings.window_height = defaults.window_height;
        break;
    case SettingId::vsync:
        settings.vsync = defaults.vsync;
        break;
    case SettingId::frame_limit:
        settings.frame_limit = defaults.frame_limit;
        settings.frame_limit_mode = defaults.frame_limit_mode;
        break;
    case SettingId::ui_scale:
        settings.ui_scale = defaults.ui_scale;
        break;
    case SettingId::safe_area:
        settings.safe_area = defaults.safe_area;
        break;
    case SettingId::presentation:
        settings.presentation = defaults.presentation;
        break;
    case SettingId::texture_style:
        settings.texture_style = defaults.texture_style;
        break;
    case SettingId::render_scale:
        settings.render_scale = defaults.render_scale;
        break;
    case SettingId::texture_quality:
        settings.texture_quality = defaults.texture_quality;
        break;
    case SettingId::shadow_quality:
        settings.shadows_enabled = defaults.shadows_enabled;
        settings.shadow_quality = defaults.shadow_quality;
        break;
    case SettingId::anti_aliasing:
        settings.anti_aliasing = defaults.anti_aliasing;
        break;
    case SettingId::ambient_occlusion:
        settings.ambient_occlusion = defaults.ambient_occlusion;
        break;
    case SettingId::ore_glow:
        settings.ore_glow = defaults.ore_glow;
        break;
    case SettingId::particle_quality:
        settings.particle_quality = defaults.particle_quality;
        break;
    case SettingId::tone_mapping:
        settings.tone_mapping = defaults.tone_mapping;
        break;
    case SettingId::auto_exposure:
        settings.auto_exposure = defaults.auto_exposure;
        break;
    case SettingId::exposure_compensation:
        settings.exposure_compensation = defaults.exposure_compensation;
        break;
    case SettingId::color_grading:
        settings.color_grading = defaults.color_grading;
        break;
    case SettingId::view_distance:
        settings.view_distance = defaults.view_distance;
        break;
    case SettingId::vertical_fov:
        settings.vertical_fov = defaults.vertical_fov;
        break;
    case SettingId::viewmodel_fov:
        settings.viewmodel_fov = defaults.viewmodel_fov;
        break;
    case SettingId::mouse_sensitivity:
        settings.mouse_sensitivity = defaults.mouse_sensitivity;
        break;
    case SettingId::controller_sensitivity:
        settings.controller_sensitivity = defaults.controller_sensitivity;
        break;
    case SettingId::invert_y:
        settings.invert_y = defaults.invert_y;
        break;
    case SettingId::auto_climb:
        settings.auto_climb = defaults.auto_climb;
        break;
    case SettingId::creative_flight_speed:
        settings.creative_flight_speed = defaults.creative_flight_speed;
        break;
    case SettingId::camera_bob:
        settings.camera_bob = defaults.camera_bob;
        break;
    case SettingId::screen_shake:
        settings.screen_shake = defaults.screen_shake;
        break;
    case SettingId::hud_visible:
        settings.hud_visible = defaults.hud_visible;
        break;
    case SettingId::vitals_style:
        settings.vitals_style = defaults.vitals_style;
        break;
    case SettingId::hud_server:
        settings.hud_server = defaults.hud_server;
        break;
    case SettingId::hud_coordinates:
        settings.hud_coordinates = defaults.hud_coordinates;
        break;
    case SettingId::hud_debug:
        settings.hud_debug = defaults.hud_debug;
        break;
    case SettingId::hud_compass:
        settings.hud_compass = defaults.hud_compass;
        break;
    case SettingId::hud_clock:
        settings.hud_clock = defaults.hud_clock;
        break;
    case SettingId::hud_distance:
        settings.hud_distance = defaults.hud_distance;
        break;
    case SettingId::hud_oxygen:
        settings.hud_oxygen = defaults.hud_oxygen;
        break;
    case SettingId::hud_crosshair:
        settings.hud_crosshair = defaults.hud_crosshair;
        break;
    case SettingId::hud_target:
        settings.hud_target = defaults.hud_target;
        break;
    case SettingId::hud_roster:
        settings.hud_roster = defaults.hud_roster;
        break;
    case SettingId::hud_scope:
        settings.hud_scope = defaults.hud_scope;
        break;
    case SettingId::hud_hotbar:
        settings.hud_hotbar = defaults.hud_hotbar;
        break;
    case SettingId::hud_item_name:
        settings.hud_item_name = defaults.hud_item_name;
        break;
    case SettingId::hud_hints:
        settings.hud_hints = defaults.hud_hints;
        break;
    case SettingId::hud_chat:
        settings.hud_chat = defaults.hud_chat;
        break;
    case SettingId::hud_notifications:
        settings.hud_notifications = defaults.hud_notifications;
        break;
    case SettingId::hud_damage:
        settings.hud_damage = defaults.hud_damage;
        break;
    case SettingId::hud_death:
        settings.hud_death = defaults.hud_death;
        break;
    case SettingId::fade_inactive_tray:
        settings.fade_inactive_tray = defaults.fade_inactive_tray;
        break;
    case SettingId::audio_output:
        // An empty id follows the Windows default device; the application
        // fills in the resolved name the next time it starts.
        settings.audio_output_id.clear();
        settings.audio_output_name.clear();
        break;
    case SettingId::master_volume:
        settings.master_volume = defaults.master_volume;
        break;
    case SettingId::music_volume:
        settings.music_volume = defaults.music_volume;
        break;
    case SettingId::effects_volume:
        settings.effects_volume = defaults.effects_volume;
        break;
    case SettingId::ambience_volume:
        settings.ambience_volume = defaults.ambience_volume;
        break;
    case SettingId::voice_volume:
        settings.voice_volume = defaults.voice_volume;
        break;
    case SettingId::mute_unfocused:
        settings.mute_unfocused = defaults.mute_unfocused;
        break;
    case SettingId::raw_mouse_input:
        settings.raw_mouse_input = defaults.raw_mouse_input;
        break;
    case SettingId::controller_vibration:
        settings.controller_vibration = defaults.controller_vibration;
        break;
    case SettingId::controller_aim_assist:
        settings.controller_aim_assist = defaults.controller_aim_assist;
        break;
    case SettingId::hold_to_sprint:
        settings.hold_to_sprint = defaults.hold_to_sprint;
        break;
    case SettingId::reduced_motion:
        settings.reduced_motion = defaults.reduced_motion;
        break;
    case SettingId::high_contrast:
        settings.high_contrast = defaults.high_contrast;
        break;
    case SettingId::larger_text:
        settings.larger_text = defaults.larger_text;
        break;
    case SettingId::color_filter:
        settings.color_filter = defaults.color_filter;
        break;
    case SettingId::subtitles:
        settings.subtitles = defaults.subtitles;
        break;
    case SettingId::subtitle_size:
        settings.subtitle_size = defaults.subtitle_size;
        break;
    case SettingId::quality_preset:
        apply_quality_preset(settings, detect_quality_preset(defaults));
        break;
    case SettingId::shadow_darkness:
        settings.shadow_darkness = defaults.shadow_darkness;
        break;
    case SettingId::texture_filtering:
        settings.texture_filtering = defaults.texture_filtering;
        break;
    case SettingId::bloom:
        settings.bloom = defaults.bloom;
        break;
    case SettingId::sun_shafts:
        settings.sun_shafts = defaults.sun_shafts;
        break;
    case SettingId::show_fps:
        settings.show_fps = defaults.show_fps;
        break;
    case SettingId::low_latency:
        settings.low_latency = defaults.low_latency;
        break;
    case SettingId::motion_smoothing:
        settings.motion_smoothing = defaults.motion_smoothing;
        break;
    case SettingId::ui_style:
        settings.ui_style = defaults.ui_style;
        break;
    case SettingId::enhancement_lagoon_water:
    case SettingId::enhancement_sinking_lava:
    case SettingId::enhancement_free_flight:
        // The factory default of every non-retail feature is OFF.
        set_enhancement_enabled(settings, *setting_enhancement(id),
                                enhancement_enabled(defaults, *setting_enhancement(id)));
        break;
    }
    settings.normalize();
}

void reset_tab(Settings &settings, SettingsTab tab)
{
    for (const SettingRow &row : rows_for_tab(tab))
    {
        reset_setting(settings, row.id);
    }
    // The hands' projection has no row of its own (see display_rows); resetting
    // the page that holds "Field of view" returns it to retail's value too, so
    // a file that carries a tuned value can be undone from the menu.
    if (tab == SettingsTab::display)
        reset_setting(settings, SettingId::viewmodel_fov);
}

const std::vector<SettingRow> &rows_for_tab(SettingsTab tab)
{
    switch (tab)
    {
    case SettingsTab::display:
        return display_rows;
    case SettingsTab::graphics:
        return graphics_rows;
    case SettingsTab::effects:
        return effects_rows;
    case SettingsTab::performance:
        return performance_rows;
    case SettingsTab::gameplay:
        return gameplay_rows;
    case SettingsTab::controls:
        return control_rows;
    case SettingsTab::interface:
        return interface_rows;
    case SettingsTab::audio:
        return audio_rows;
    case SettingsTab::accessibility:
        return accessibility_rows;
    case SettingsTab::enhancements:
        return enhancement_rows;
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
    case SettingsTab::effects:
        return "EFFECTS";
    case SettingsTab::performance:
        return "PERFORMANCE";
    case SettingsTab::gameplay:
        return "GAMEPLAY";
    case SettingsTab::controls:
        return "CONTROLS";
    case SettingsTab::interface:
        return "INTERFACE";
    case SettingsTab::audio:
        return "AUDIO";
    case SettingsTab::accessibility:
        return "ACCESSIBILITY";
    case SettingsTab::enhancements:
        return "EXTRAS";
    }
    return "DISPLAY";
}

std::string_view tab_title(SettingsTab tab) noexcept
{
    switch (tab)
    {
    case SettingsTab::enhancements:
        return "NOT IN THE ORIGINAL GAME";
    case SettingsTab::graphics:
        return "GRAPHICS QUALITY";
    case SettingsTab::effects:
        return "EFFECTS & COLOUR";
    case SettingsTab::performance:
        return "PERFORMANCE & LATENCY";
    case SettingsTab::interface:
        return "INTERFACE & HUD";
    default:
        return tab_label(tab);
    }
}

std::string_view tab_summary(SettingsTab tab) noexcept
{
    switch (tab)
    {
    case SettingsTab::display:
        return "Window, resolution, brightness and field of view.";
    case SettingsTab::graphics:
        return "How detailed the world looks. Higher settings look better and lower the frame rate.";
    case SettingsTab::effects:
        return "Optional lighting effects and the colour treatment. Most cost little or nothing.";
    case SettingsTab::performance:
        return "Frame rate, smoothness and how quickly the game reacts to your hand.";
    case SettingsTab::gameplay:
        return "How the game plays and how the camera moves.";
    case SettingsTab::controls:
        return "Mouse and controller.";
    case SettingsTab::interface:
        return "What appears on screen while you play.";
    case SettingsTab::audio:
        return "Volume and playback device.";
    case SettingsTab::accessibility:
        return "Comfort and readability.";
    case SettingsTab::enhancements:
        return "Off by default. These change how the game plays, so they work in single-player "
               "only: online, your game always matches the original.";
    }
    return {};
}

std::span<const SettingsTab> all_tabs() noexcept
{
    static constexpr std::array tabs{SettingsTab::display,     SettingsTab::graphics,
                                     SettingsTab::effects,     SettingsTab::performance,
                                     SettingsTab::gameplay,    SettingsTab::controls,
                                     SettingsTab::interface,   SettingsTab::audio,
                                     SettingsTab::accessibility, SettingsTab::enhancements};
    return tabs;
}

SettingsTab next_tab(SettingsTab tab, int direction) noexcept
{
    const auto tabs = all_tabs();
    const auto found = std::find(tabs.begin(), tabs.end(), tab);
    const int count = static_cast<int>(tabs.size());
    const int current = found == tabs.end() ? 0 : static_cast<int>(found - tabs.begin());
    return tabs[static_cast<std::size_t>((current + (direction < 0 ? -1 : 1) + count) % count)];
}

std::string setting_value(const Settings &settings, SettingId id)
{
    if (const GraphicsOption *option = graphics_option(id))
        return graphics_option_value(*option, settings);
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
        return std::to_string(settings.window_width) + " X " +
               std::to_string(settings.window_height);
    case SettingId::vsync:
        return settings.vsync ? "ON" : "OFF";
    case SettingId::frame_limit:
        switch (settings.frame_limit_mode)
        {
        case FrameLimitMode::unlimited:
            return "UNLIMITED";
        case FrameLimitMode::display:
            return "MATCH DISPLAY";
        case FrameLimitMode::fixed:
            break;
        }
        return std::to_string(settings.frame_limit) + " FPS";
    case SettingId::ui_scale:
        return percent(settings.ui_scale);
    case SettingId::safe_area:
        return percent(settings.safe_area);
    case SettingId::presentation:
        // Deliberately not "Classic": that word belongs to the texture pack.
        return settings.presentation == PresentationProfile::classic ? "ORIGINAL" : "ENHANCED";
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
        return settings.shadows_enabled ? shown(to_string(settings.shadow_quality)) : "OFF";
    case SettingId::anti_aliasing:
        return on_off(settings.anti_aliasing != AntiAliasing::off);
    case SettingId::ambient_occlusion:
        return settings.ambient_occlusion ? "ON" : "OFF";
    case SettingId::ore_glow:
        return settings.ore_glow ? "ON" : "OFF";
    case SettingId::particle_quality:
        return shown(to_string(settings.particle_quality));
    case SettingId::tone_mapping:
        switch (settings.tone_mapping)
        {
        // Named for what the player sees, not after the curve's author.
        case ToneMapping::legacy:
            return "BASIC";
        case ToneMapping::reinhard:
            return "SOFT";
        case ToneMapping::filmic:
            return "PUNCHY";
        case ToneMapping::aces:
            return "CINEMATIC";
        }
        break;
    case SettingId::auto_exposure:
        return settings.auto_exposure ? "ON" : "OFF";
    case SettingId::exposure_compensation: {
        // One step per tenth of a stop; "EV" means nothing to most players.
        const int steps = static_cast<int>(std::lround(settings.exposure_compensation * 10.0F));
        return steps == 0 ? "NORMAL" : (steps > 0 ? "+" : "") + std::to_string(steps);
    }
    case SettingId::color_grading:
        return percent(settings.color_grading);
    case SettingId::view_distance:
        return std::to_string(settings.view_distance * 4) + " BLOCKS";
    case SettingId::vertical_fov:
        return std::to_string(static_cast<int>(std::lround(settings.vertical_fov))) + " DEGREES";
    case SettingId::viewmodel_fov:
        return std::to_string(static_cast<int>(std::lround(settings.viewmodel_fov))) + " DEGREES";
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
    case SettingId::vitals_style:
        // The retail styles draw their release's own meters: shown or hidden.
        if (settings.ui_style != UiStyle::remastered)
            return on_off(settings.vitals_style != VitalsStyle::none);
        switch (settings.vitals_style)
        {
        case VitalsStyle::remastered:
            return "BARS";
        case VitalsStyle::classic:
            return "HEARTS";
        case VitalsStyle::modern:
            return "TOP BARS";
        case VitalsStyle::none:
            break;
        }
        return "HIDDEN";
    case SettingId::hud_server:
        return settings.hud_server ? "ON" : "OFF";
    case SettingId::hud_coordinates:
        return settings.hud_coordinates ? "ON" : "OFF";
    case SettingId::hud_debug:
        return settings.hud_debug ? "ON" : "OFF";
    case SettingId::hud_compass:
        return settings.hud_compass ? "ON" : "OFF";
    case SettingId::hud_clock:
        return settings.hud_clock ? "ON" : "OFF";
    case SettingId::hud_distance:
        return settings.hud_distance ? "ON" : "OFF";
    case SettingId::hud_oxygen:
        return settings.hud_oxygen ? "ON" : "OFF";
    case SettingId::hud_crosshair:
        return settings.hud_crosshair ? "ON" : "OFF";
    case SettingId::hud_target:
        return settings.hud_target ? "ON" : "OFF";
    case SettingId::hud_roster:
        return settings.hud_roster ? "ON" : "OFF";
    case SettingId::hud_scope:
        return settings.hud_scope ? "ON" : "OFF";
    case SettingId::hud_hotbar:
        return settings.hud_hotbar ? "ON" : "OFF";
    case SettingId::hud_item_name:
        return settings.hud_item_name ? "ON" : "OFF";
    case SettingId::hud_hints:
        return settings.hud_hints ? "ON" : "OFF";
    case SettingId::hud_chat:
        return settings.hud_chat ? "ON" : "OFF";
    case SettingId::hud_notifications:
        return settings.hud_notifications ? "ON" : "OFF";
    case SettingId::hud_damage:
        return settings.hud_damage ? "ON" : "OFF";
    case SettingId::hud_death:
        return settings.hud_death ? "ON" : "OFF";
    case SettingId::fade_inactive_tray:
        // BoolSettingItem(Strings.Fade_Inactive_Tray, ..., Strings.Faded, Strings.Regular)
        return settings.fade_inactive_tray ? "FADED" : "REGULAR";
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
    case SettingId::quality_preset:
        return std::string{quality_preset_label(detect_quality_preset(settings))};
    case SettingId::shadow_darkness:
        return percent(settings.shadow_darkness);
    case SettingId::texture_filtering:
        return settings.texture_filtering <= 1 ? "OFF"
                                               : std::to_string(settings.texture_filtering) + "X";
    case SettingId::bloom:
        return on_off(settings.bloom);
    case SettingId::sun_shafts:
        return on_off(settings.sun_shafts);
    case SettingId::show_fps:
        return on_off(settings.show_fps);
    case SettingId::low_latency:
        return on_off(settings.low_latency != LowLatencyMode::off);
    case SettingId::motion_smoothing:
        return on_off(settings.motion_smoothing);
    case SettingId::ui_style:
        // `automatic` is "the retail look of the world I am in", never ours.
        return settings.ui_style == UiStyle::automatic ? "MATCH WORLD"
                                                       : shown(to_string(settings.ui_style));
    case SettingId::enhancement_lagoon_water:
    case SettingId::enhancement_sinking_lava:
    case SettingId::enhancement_free_flight:
        return on_off(enhancement_enabled(settings, *setting_enhancement(id)));
    }
    return {};
}

float setting_normalized_value(const Settings &settings, SettingId id) noexcept
{
    if (const GraphicsOption *option = graphics_option(id))
        return graphics_option_normalized(*option, settings);
    switch (id)
    {
    case SettingId::ui_scale:
        return (settings.ui_scale - 0.75F) / 0.75F;
    case SettingId::safe_area:
        return (settings.safe_area - 0.80F) / 0.20F;
    case SettingId::render_scale:
        return (settings.render_scale - 0.50F) / 1.50F;
    case SettingId::view_distance:
        return static_cast<float>(settings.view_distance - 8) / 24.0F;
    case SettingId::vertical_fov:
        return view::setting_normalized(settings.vertical_fov);
    case SettingId::exposure_compensation:
        return (settings.exposure_compensation + 2.0F) / 4.0F;
    case SettingId::color_grading:
        return settings.color_grading;
    case SettingId::shadow_darkness:
        return (settings.shadow_darkness - 0.25F) / 0.65F;
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
    // An option that cannot act right now keeps its stored value; the menu
    // tells the player why instead of silently changing something unseen.
    if (!setting_unavailable_reason(settings, id).empty())
    {
        return;
    }
    if (const GraphicsOption *option = graphics_option(id))
    {
        adjust_graphics_option(*option, settings, sign);
        settings.normalize();
        return;
    }
    switch (id)
    {
    case SettingId::window_mode:
        settings.window_mode =
            cycle(settings.window_mode, sign,
                  std::array{WindowMode::windowed, WindowMode::borderless, WindowMode::fullscreen});
        break;
    case SettingId::resolution:
        adjust_resolution(settings, sign, {});
        break;
    case SettingId::vsync:
        toggle(settings.vsync);
        break;
    case SettingId::frame_limit: {
        // One ring: every fixed step, then MATCH DISPLAY, then UNLIMITED.
        const int fixed_count = static_cast<int>(frame_limit_steps.size());
        const int count = fixed_count + 2;
        int index = fixed_count + 1;
        if (settings.frame_limit_mode == FrameLimitMode::display)
        {
            index = fixed_count;
        }
        else if (settings.frame_limit_mode == FrameLimitMode::fixed)
        {
            // A hand-typed value such as 75 steps to its nearest neighbour in
            // the direction pressed rather than jumping to an arbitrary slot.
            const auto upper = std::upper_bound(frame_limit_steps.begin(),
                                                frame_limit_steps.end(), settings.frame_limit);
            const auto found = std::find(frame_limit_steps.begin(), frame_limit_steps.end(),
                                         settings.frame_limit);
            const int above = static_cast<int>(upper - frame_limit_steps.begin());
            index = found != frame_limit_steps.end()
                        ? static_cast<int>(found - frame_limit_steps.begin())
                        : (sign > 0 ? above - 1 : above);
        }
        index = (index + sign + count) % count;
        if (index < fixed_count)
        {
            settings.frame_limit_mode = FrameLimitMode::fixed;
            settings.frame_limit = frame_limit_steps[static_cast<std::size_t>(index)];
        }
        else
        {
            // In display mode the application overwrites frame_limit with the
            // monitor's refresh rate; until then keep the last fixed value.
            settings.frame_limit_mode =
                index == fixed_count ? FrameLimitMode::display : FrameLimitMode::unlimited;
            if (settings.frame_limit <= 0)
            {
                settings.frame_limit = Settings{}.frame_limit;
            }
        }
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
        settings.render_scale =
            std::round((settings.render_scale + 0.05F * static_cast<float>(sign)) * 20.0F) / 20.0F;
        break;
    case SettingId::texture_quality:
        settings.texture_quality = cycle(settings.texture_quality, sign,
                                         std::array{QualityLevel::low, QualityLevel::medium,
                                                    QualityLevel::high, QualityLevel::ultra});
        break;
    case SettingId::shadow_quality: {
        // OFF, LOW, MEDIUM, HIGH, ULTRA as one ring over two stored fields.
        constexpr int count = 5;
        const int current =
            settings.shadows_enabled ? static_cast<int>(settings.shadow_quality) + 1 : 0;
        const int next = (current + sign + count) % count;
        settings.shadows_enabled = next != 0;
        if (next != 0)
        {
            settings.shadow_quality = static_cast<QualityLevel>(next - 1);
        }
        break;
    }
    case SettingId::anti_aliasing:
        settings.anti_aliasing =
            settings.anti_aliasing == AntiAliasing::off ? AntiAliasing::fxaa : AntiAliasing::off;
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
    // Snapped to the step grid so repeated presses cannot drift off "NORMAL"
    // or leave values such as 0.6500001 in the settings file.
    case SettingId::exposure_compensation:
        settings.exposure_compensation =
            std::round((settings.exposure_compensation + 0.1F * static_cast<float>(sign)) * 10.0F) /
            10.0F;
        break;
    case SettingId::color_grading:
        settings.color_grading =
            std::round((settings.color_grading + 0.05F * static_cast<float>(sign)) * 20.0F) / 20.0F;
        break;
    case SettingId::view_distance:
        settings.view_distance += sign;
        break;
    case SettingId::vertical_fov:
        // Whole vertical degrees; a hand-edited fraction snaps on the first step.
        settings.vertical_fov = std::round(settings.vertical_fov) + static_cast<float>(sign);
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
    case SettingId::vitals_style:
        // Remastered has three looks of its own and hidden. The retail styles
        // draw their release's own meters, so there the row only shows or
        // hides them (see VitalsStyle).
        if (settings.ui_style == UiStyle::remastered)
            settings.vitals_style =
                cycle(settings.vitals_style, sign,
                      std::array{VitalsStyle::remastered, VitalsStyle::classic,
                                 VitalsStyle::modern, VitalsStyle::none});
        else
            settings.vitals_style = settings.vitals_style == VitalsStyle::none
                                        ? Settings{}.vitals_style
                                        : VitalsStyle::none;
        break;
    case SettingId::hud_server:
        toggle(settings.hud_server);
        break;
    case SettingId::hud_coordinates:
        toggle(settings.hud_coordinates);
        break;
    case SettingId::hud_debug:
        toggle(settings.hud_debug);
        break;
    case SettingId::hud_compass:
        toggle(settings.hud_compass);
        break;
    case SettingId::hud_clock:
        toggle(settings.hud_clock);
        break;
    case SettingId::hud_distance:
        toggle(settings.hud_distance);
        break;
    case SettingId::hud_oxygen:
        toggle(settings.hud_oxygen);
        break;
    case SettingId::hud_crosshair:
        toggle(settings.hud_crosshair);
        break;
    case SettingId::hud_target:
        toggle(settings.hud_target);
        break;
    case SettingId::hud_roster:
        toggle(settings.hud_roster);
        break;
    case SettingId::hud_scope:
        toggle(settings.hud_scope);
        break;
    case SettingId::hud_hotbar:
        toggle(settings.hud_hotbar);
        break;
    case SettingId::hud_item_name:
        toggle(settings.hud_item_name);
        break;
    case SettingId::hud_hints:
        toggle(settings.hud_hints);
        break;
    case SettingId::hud_chat:
        toggle(settings.hud_chat);
        break;
    case SettingId::hud_notifications:
        toggle(settings.hud_notifications);
        break;
    case SettingId::hud_damage:
        toggle(settings.hud_damage);
        break;
    case SettingId::hud_death:
        toggle(settings.hud_death);
        break;
    case SettingId::fade_inactive_tray:
        toggle(settings.fade_inactive_tray);
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
    case SettingId::quality_preset: {
        // CUSTOM is a description of the options, not a choice: stepping off
        // it lands on the first real preset in the direction pressed.
        constexpr int count = 4;
        const QualityPreset current = detect_quality_preset(settings);
        const int next = current == QualityPreset::custom
                             ? (sign > 0 ? 0 : count - 1)
                             : (static_cast<int>(current) + sign + count) % count;
        apply_quality_preset(settings, static_cast<QualityPreset>(next));
        break;
    }
    case SettingId::shadow_darkness:
        settings.shadow_darkness =
            std::round((settings.shadow_darkness + 0.05F * static_cast<float>(sign)) * 20.0F) /
            20.0F;
        break;
    case SettingId::texture_filtering: {
        const auto found = std::find(texture_filtering_steps.begin(),
                                     texture_filtering_steps.end(), settings.texture_filtering);
        const int count = static_cast<int>(texture_filtering_steps.size());
        const int index = found == texture_filtering_steps.end()
                              ? 3
                              : static_cast<int>(found - texture_filtering_steps.begin());
        settings.texture_filtering =
            texture_filtering_steps[static_cast<std::size_t>((index + sign + count) % count)];
        break;
    }
    case SettingId::bloom:
        toggle(settings.bloom);
        break;
    case SettingId::sun_shafts:
        toggle(settings.sun_shafts);
        break;
    case SettingId::show_fps:
        toggle(settings.show_fps);
        break;
    case SettingId::low_latency:
        settings.low_latency =
            settings.low_latency == LowLatencyMode::off ? LowLatencyMode::on : LowLatencyMode::off;
        break;
    case SettingId::motion_smoothing:
        toggle(settings.motion_smoothing);
        break;
    case SettingId::ui_style:
        // Ours first, then the two exact copies, then "whichever of those two
        // the world is" last.
        settings.ui_style = cycle(settings.ui_style, sign,
                                  std::array{UiStyle::remastered, UiStyle::classic,
                                             UiStyle::modern, UiStyle::automatic});
        break;
    case SettingId::enhancement_lagoon_water:
    case SettingId::enhancement_sinking_lava:
    case SettingId::enhancement_free_flight: {
        // Only the stored choice changes here. The session decides whether it
        // takes effect (enhancement_active), so it can be set at any time.
        const Enhancement enhancement = *setting_enhancement(id);
        set_enhancement_enabled(settings, enhancement, !enhancement_enabled(settings, enhancement));
        break;
    }
    }
    settings.normalize();
}

} // namespace openminerz::ui
