// Settings persistence and the settings-menu model: defaults, presets, page
// reset, availability rules, migration of older files and exact round trips.
// Everything here runs against temporary files; it never touches the real
// settings in %APPDATA%.

#include "openminerz/menu_model.hpp"
#include "openminerz/settings.hpp"
#include "openminerz/settings_menu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(float left, float right, float tolerance = 0.0005F)
{
    return std::abs(left - right) <= tolerance;
}

// A settings file private to one test, removed again when the test ends.
class TemporaryFile final
{
  public:
    explicit TemporaryFile(std::string_view name)
        : path_(std::filesystem::temp_directory_path() /
                ("openminerz-settings-" + std::string{name} + ".cfg"))
    {
        std::filesystem::remove(path_);
    }
    ~TemporaryFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;

    void write(std::string_view text) const
    {
        std::ofstream output{path_, std::ios::binary | std::ios::trunc};
        output << text;
    }
    [[nodiscard]] std::string read() const
    {
        std::ifstream input{path_, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }
    [[nodiscard]] Settings load() const
    {
        return SettingsStore{path_}.load();
    }
    void save(const Settings &settings) const
    {
        SettingsStore{path_}.save(settings);
    }
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<ui::SettingRow> every_row()
{
    std::vector<ui::SettingRow> rows;
    for (const ui::SettingsTab tab : ui::all_tabs())
        for (const ui::SettingRow &row : ui::rows_for_tab(tab))
            rows.push_back(row);
    return rows;
}

[[nodiscard]] std::string value(const Settings &settings, ui::SettingId id)
{
    return ui::setting_value(settings, id);
}

// Defaults with the two context switches set so every row is active:
// VSync off (otherwise the frame-rate limit stands down) and a real window
// (Borderless ignores the resolution).
[[nodiscard]] Settings all_rows_active()
{
    Settings settings;
    settings.vsync = false;
    settings.window_mode = WindowMode::windowed;
    settings.shadows_enabled = true;
    return settings;
}

// Field-by-field comparison, so a setting that is saved but not loaded (or the
// reverse) cannot hide behind identical display strings.
[[nodiscard]] std::string first_difference(const Settings &a, const Settings &b)
{
#define OPENMINERZ_SAME(field)                                                                    \
    if (!(a.field == b.field))                                                                     \
        return #field;
#define OPENMINERZ_NEAR(field)                                                                    \
    if (!near(a.field, b.field))                                                                   \
        return #field;
    OPENMINERZ_SAME(window_mode)
    OPENMINERZ_SAME(window_width)
    OPENMINERZ_SAME(window_height)
    OPENMINERZ_SAME(vsync)
    OPENMINERZ_SAME(frame_limit)
    OPENMINERZ_SAME(frame_limit_mode)
    OPENMINERZ_NEAR(ui_scale)
    OPENMINERZ_NEAR(safe_area)
    OPENMINERZ_SAME(presentation)
    OPENMINERZ_SAME(texture_style)
    OPENMINERZ_NEAR(render_scale)
    OPENMINERZ_SAME(texture_quality)
    OPENMINERZ_SAME(shadow_quality)
    OPENMINERZ_SAME(shadows_enabled)
    OPENMINERZ_NEAR(shadow_darkness)
    OPENMINERZ_SAME(anti_aliasing)
    OPENMINERZ_SAME(texture_filtering)
    OPENMINERZ_SAME(ambient_occlusion)
    OPENMINERZ_SAME(ore_glow)
    OPENMINERZ_SAME(bloom)
    OPENMINERZ_SAME(sun_shafts)
    OPENMINERZ_SAME(particle_quality)
    OPENMINERZ_SAME(tone_mapping)
    OPENMINERZ_NEAR(exposure_compensation)
    OPENMINERZ_SAME(auto_exposure)
    OPENMINERZ_NEAR(color_grading)
    OPENMINERZ_SAME(view_distance)
    OPENMINERZ_NEAR(vertical_fov)
    OPENMINERZ_NEAR(viewmodel_fov)
    OPENMINERZ_NEAR(mouse_sensitivity)
    OPENMINERZ_NEAR(controller_sensitivity)
    OPENMINERZ_SAME(invert_y)
    OPENMINERZ_SAME(auto_climb)
    OPENMINERZ_NEAR(creative_flight_speed)
    OPENMINERZ_NEAR(camera_bob)
    OPENMINERZ_NEAR(screen_shake)
    OPENMINERZ_SAME(hud_visible)
    OPENMINERZ_SAME(ui_style)
    OPENMINERZ_SAME(vitals_style)
    OPENMINERZ_SAME(hud_server)
    OPENMINERZ_SAME(hud_coordinates)
    OPENMINERZ_SAME(hud_debug)
    OPENMINERZ_SAME(hud_compass)
    OPENMINERZ_SAME(hud_clock)
    OPENMINERZ_SAME(hud_distance)
    OPENMINERZ_SAME(hud_oxygen)
    OPENMINERZ_SAME(hud_crosshair)
    OPENMINERZ_SAME(hud_target)
    OPENMINERZ_SAME(hud_roster)
    OPENMINERZ_SAME(hud_scope)
    OPENMINERZ_SAME(hud_hotbar)
    OPENMINERZ_SAME(hud_item_name)
    OPENMINERZ_SAME(hud_hints)
    OPENMINERZ_SAME(hud_chat)
    OPENMINERZ_SAME(hud_notifications)
    OPENMINERZ_SAME(hud_damage)
    OPENMINERZ_SAME(hud_death)
    OPENMINERZ_SAME(fade_inactive_tray)
    OPENMINERZ_SAME(gameplay_profile)
    OPENMINERZ_SAME(game_mode)
    OPENMINERZ_SAME(game_difficulty)
    OPENMINERZ_NEAR(master_volume)
    OPENMINERZ_NEAR(music_volume)
    OPENMINERZ_NEAR(effects_volume)
    OPENMINERZ_NEAR(ambience_volume)
    OPENMINERZ_NEAR(voice_volume)
    OPENMINERZ_SAME(mute_unfocused)
    OPENMINERZ_SAME(audio_output_id)
    OPENMINERZ_SAME(audio_output_name)
    OPENMINERZ_SAME(raw_mouse_input)
    OPENMINERZ_SAME(controller_vibration)
    OPENMINERZ_SAME(controller_aim_assist)
    OPENMINERZ_SAME(hold_to_sprint)
    OPENMINERZ_SAME(reduced_motion)
    OPENMINERZ_SAME(high_contrast)
    OPENMINERZ_SAME(larger_text)
    OPENMINERZ_SAME(color_filter)
    OPENMINERZ_SAME(subtitles)
    OPENMINERZ_NEAR(subtitle_size)
    OPENMINERZ_SAME(show_fps)
    OPENMINERZ_SAME(motion_smoothing)
    OPENMINERZ_SAME(xbox_avatar_show_mine)
    OPENMINERZ_SAME(xbox_avatar_show_others)
    OPENMINERZ_SAME(xbox_avatar_file)
    OPENMINERZ_SAME(low_latency)
    OPENMINERZ_SAME(enhancement_lagoon_water)
    OPENMINERZ_SAME(enhancement_sinking_lava)
#undef OPENMINERZ_SAME
#undef OPENMINERZ_NEAR
    return {};
}

// Every field moved off its default to a value that is valid as it stands.
[[nodiscard]] Settings everything_changed()
{
    Settings s;
    s.window_mode = WindowMode::fullscreen;
    s.window_width = 2560;
    s.window_height = 1440;
    s.vsync = false;
    s.frame_limit = 90;
    s.frame_limit_mode = FrameLimitMode::display;
    s.ui_scale = 1.25F;
    s.safe_area = 0.9F;
    s.presentation = PresentationProfile::classic;
    s.texture_style = AssetStyle::classic_hd;
    s.render_scale = 0.75F;
    s.texture_quality = QualityLevel::low;
    s.shadow_quality = QualityLevel::medium;
    s.shadows_enabled = false;
    s.shadow_darkness = 0.4F;
    s.anti_aliasing = AntiAliasing::off;
    s.texture_filtering = 2;
    s.ambient_occlusion = false;
    s.ore_glow = false;
    s.bloom = false;
    s.sun_shafts = false;
    s.particle_quality = QualityLevel::ultra;
    s.tone_mapping = ToneMapping::filmic;
    s.exposure_compensation = -0.7F;
    s.auto_exposure = false;
    s.color_grading = 0.35F;
    s.view_distance = 23;
    s.vertical_fov = 91.0F;
    s.viewmodel_fov = 70.0F;
    s.mouse_sensitivity = 2.35F;
    s.controller_sensitivity = 0.6F;
    s.invert_y = true;
    s.auto_climb = false;
    s.creative_flight_speed = 21.0F;
    s.camera_bob = 0.2F;
    s.screen_shake = 0.1F;
    s.hud_visible = false;
    s.ui_style = UiStyle::classic;
    s.vitals_style = VitalsStyle::classic;
    s.hud_server = true;
    s.hud_coordinates = true;
    s.hud_debug = true;
    s.hud_compass = false;
    s.hud_clock = true;
    s.hud_distance = false;
    s.hud_oxygen = false;
    s.hud_crosshair = false;
    s.hud_target = false;
    s.hud_roster = false;
    s.hud_scope = false;
    s.hud_hotbar = false;
    s.hud_item_name = false;
    s.hud_hints = true;
    s.hud_chat = false;
    s.hud_notifications = false;
    s.hud_damage = false;
    s.hud_death = false;
    s.fade_inactive_tray = true;
    s.gameplay_profile = 1;
    s.game_mode = 3;
    s.game_difficulty = 2;
    s.master_volume = 0.35F;
    s.music_volume = 0.15F;
    s.effects_volume = 0.45F;
    s.ambience_volume = 0.25F;
    s.voice_volume = 0.55F;
    s.mute_unfocused = true;
    s.audio_output_id = "{0.0.0.00000000}.{test-endpoint}";
    s.audio_output_name = "Speakers (Test Device = 2)";
    s.raw_mouse_input = false;
    s.controller_vibration = false;
    s.controller_aim_assist = false;
    s.hold_to_sprint = true;
    s.reduced_motion = true;
    s.high_contrast = true;
    s.larger_text = true;
    s.color_filter = ColorFilter::tritanopia;
    s.subtitles = false;
    s.subtitle_size = 1.3F;
    s.show_fps = true;
    s.motion_smoothing = false;
    s.xbox_avatar_show_mine = false;
    s.xbox_avatar_show_others = false;
    s.xbox_avatar_file = "D:\\Avatars\\mine.ocavatar";
    s.low_latency = LowLatencyMode::off;
    s.enhancement_lagoon_water = true;
    s.enhancement_sinking_lava = true;
    return s;
}

// The settings file exactly as the game wrote it before this redesign (taken
// from a real profile): no shadow_darkness, no frame_limit_mode, "taa".
constexpr std::string_view legacy_profile =
    "# OpenMinerZ settings v1\n"
    "window_mode=borderless\nwindow_width=1600\nwindow_height=900\nvsync=1\nframe_limit=240\n"
    "ui_scale=1\nsafe_area=0.96\npresentation=rebirth\ntexture_style=classic_hd\n"
    "render_scale=1\ntexture_quality=high\nshadow_quality=ultra\nanti_aliasing=taa\n"
    "ambient_occlusion=1\nore_glow=1\nparticle_quality=ultra\ntone_mapping=aces\n"
    "exposure_compensation=0\nauto_exposure=1\ncolor_grading=1\nview_distance=32\n"
    "vertical_fov=96\nviewmodel_fov=78\nmouse_sensitivity=1\ncontroller_sensitivity=1\n"
    "invert_y=0\nauto_climb=1\ncreative_flight_speed=32\ncamera_bob=0.65\nscreen_shake=0.75\n"
    "hud_visible=1\nvitals_style=remastered\nhud_server=0\nhud_coordinates=0\nhud_debug=0\n"
    "hud_compass=0\nhud_clock=0\nhud_distance=1\nhud_oxygen=1\nhud_crosshair=1\nhud_target=1\n"
    "hud_roster=1\nhud_scope=1\nhud_hotbar=1\nhud_item_name=1\nhud_hints=0\nhud_chat=1\n"
    "hud_notifications=1\nhud_damage=1\nhud_death=1\ncrafting_menu_modern=1\n"
    "gameplay_profile=1\ngame_mode=1\ngame_difficulty=1\nmaster_volume=0.9\nmusic_volume=0.65\n"
    "effects_volume=0.9\nambience_volume=0.8\nvoice_volume=0.85\nmute_unfocused=0\n"
    "audio_output_id=\naudio_output_name=System default (Speakers (Echo Show 8-5WJ))\n"
    "raw_mouse_input=1\ncontroller_vibration=1\ncontroller_aim_assist=1\nhold_to_sprint=0\n"
    "reduced_motion=0\nhigh_contrast=0\nlarger_text=0\ncolor_filter=none\nsubtitles=0\n"
    "subtitle_size=1\n";

void defaults_are_the_high_preset_and_survive_normalize()
{
    const Settings defaults;
    Settings normalized = defaults;
    normalized.normalize();
    check(first_difference(defaults, normalized).empty(),
          "factory defaults must already be in range: " + first_difference(defaults, normalized));
    check(ui::detect_quality_preset(defaults) == ui::QualityPreset::high,
          "a fresh install must read HIGH, not CUSTOM");
    check(value(defaults, ui::SettingId::quality_preset) == "HIGH",
          "the preset row must name the detected preset");
    check(defaults.shadows_enabled && near(defaults.shadow_darkness, 0.68F) &&
              defaults.anti_aliasing == AntiAliasing::fxaa && defaults.texture_filtering == 8 &&
              defaults.bloom && defaults.sun_shafts && !defaults.show_fps,
          "new options must default to what the renderer did before they existed");
    check(near(legacy_shadow_darkness(defaults.shadow_quality), defaults.shadow_darkness),
          "the default darkness must equal what the default quality used to produce");
}

void presets_set_detect_and_fall_back_to_custom()
{
    constexpr std::array presets{ui::QualityPreset::low, ui::QualityPreset::medium,
                                 ui::QualityPreset::high, ui::QualityPreset::ultra};
    constexpr std::array controlled{
        ui::SettingId::view_distance,     ui::SettingId::shadow_quality,
        ui::SettingId::anti_aliasing,     ui::SettingId::texture_filtering,
        ui::SettingId::particle_quality,  ui::SettingId::bloom,
        ui::SettingId::sun_shafts};
    std::set<std::string> fingerprints;
    for (const ui::QualityPreset preset : presets)
    {
        Settings settings = everything_changed();
        const Settings before = settings;
        ui::apply_quality_preset(settings, preset);
        check(ui::detect_quality_preset(settings) == preset,
              "applying a preset must be detected as that preset");
        check(value(settings, ui::SettingId::quality_preset) == ui::quality_preset_label(preset),
              "the preset row must show the applied preset");
        // Look and comfort choices are not the preset's business.
        check(settings.presentation == before.presentation &&
                  settings.texture_style == before.texture_style &&
                  near(settings.render_scale, before.render_scale) &&
                  near(settings.shadow_darkness, before.shadow_darkness) &&
                  near(settings.vertical_fov, before.vertical_fov) &&
                  settings.vsync == before.vsync && settings.frame_limit == before.frame_limit &&
                  settings.tone_mapping == before.tone_mapping &&
                  settings.ambient_occlusion == before.ambient_occlusion &&
                  settings.ore_glow == before.ore_glow,
              "a preset must only change the quality options it lists");
        std::string fingerprint;
        for (const ui::SettingId id : controlled)
            fingerprint += value(settings, id) + '|';
        check(fingerprints.insert(fingerprint).second, "every preset must be a distinct bundle");

        // Changing any one controlled option, in either direction, is Custom.
        for (const ui::SettingId id : controlled)
            for (const int direction : {-1, 1})
            {
                Settings changed = settings;
                ui::adjust_setting(changed, id, direction);
                // LOW and ULTRA sit at the ends of the view-distance slider,
                // where a press outward has nothing left to change.
                if (value(changed, id) == value(settings, id))
                {
                    check(id == ui::SettingId::view_distance &&
                              ui::detect_quality_preset(changed) == preset,
                          "only a slider at its end may ignore a press");
                    continue;
                }
                check(ui::detect_quality_preset(changed) == ui::QualityPreset::custom &&
                          value(changed, ui::SettingId::quality_preset) == "CUSTOM",
                      "changing one option must switch the preset to CUSTOM");
                // Putting it back restores the preset.
                ui::adjust_setting(changed, id, -direction);
                check(ui::detect_quality_preset(changed) == preset,
                      "undoing the change must restore the preset");
            }
    }

    // The preset row is a ring of the four real presets.
    Settings settings;
    ui::apply_quality_preset(settings, ui::QualityPreset::low);
    for (const ui::QualityPreset expected : {ui::QualityPreset::medium, ui::QualityPreset::high,
                                             ui::QualityPreset::ultra, ui::QualityPreset::low})
    {
        ui::adjust_setting(settings, ui::SettingId::quality_preset, 1);
        check(ui::detect_quality_preset(settings) == expected, "preset row must step upward");
    }
    ui::adjust_setting(settings, ui::SettingId::quality_preset, -1);
    check(ui::detect_quality_preset(settings) == ui::QualityPreset::ultra,
          "preset row must wrap downward");

    // CUSTOM is a description, never a destination.
    ui::adjust_setting(settings, ui::SettingId::view_distance, -1);
    check(ui::detect_quality_preset(settings) == ui::QualityPreset::custom, "setup: custom");
    Settings up = settings;
    ui::adjust_setting(up, ui::SettingId::quality_preset, 1);
    Settings down = settings;
    ui::adjust_setting(down, ui::SettingId::quality_preset, -1);
    check(ui::detect_quality_preset(up) == ui::QualityPreset::low &&
              ui::detect_quality_preset(down) == ui::QualityPreset::ultra,
          "stepping off CUSTOM must land on a real preset");
    const Settings untouched = settings;
    ui::apply_quality_preset(settings, ui::QualityPreset::custom);
    check(first_difference(settings, untouched).empty(), "applying CUSTOM must change nothing");

    // With shadows off the stored quality draws nothing and must not matter.
    Settings low;
    ui::apply_quality_preset(low, ui::QualityPreset::low);
    check(!low.shadows_enabled, "LOW must turn shadows off");
    low.shadow_quality = QualityLevel::ultra;
    check(ui::detect_quality_preset(low) == ui::QualityPreset::low,
          "an unused shadow quality must not turn LOW into CUSTOM");
}

void every_row_is_described_in_plain_language()
{
    // Internal or textbook names a player should not have to know. They may
    // appear inside a description as a bracketed aside, never as the name.
    constexpr std::array<std::string_view, 14> jargon{
        "viewmodel", "rebirth",  "presentation", "aces",     "reinhard",
        "filmic",    "taa",      "msaa",         "exposure", "occlusion",
        "anisotrop", "tone map", "render scale", "legacy"};
    const auto lower = [](std::string_view text) {
        std::string result{text};
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        });
        return result;
    };
    std::set<ui::SettingId> seen;
    for (const ui::SettingsTab tab : ui::all_tabs())
    {
        check(!ui::rows_for_tab(tab).empty(), "every page must have options");
        check(!ui::tab_label(tab).empty() && !ui::tab_title(tab).empty() &&
                  !ui::tab_summary(tab).empty() && ui::tab_summary(tab).back() == '.',
              "every page needs a name, a heading and a one-sentence summary");
        check(ui::tab_label(tab).size() <= 13U, "sidebar names must fit the sidebar");
        for (const ui::SettingRow &row : ui::rows_for_tab(tab))
        {
            const std::string name{row.label};
            check(seen.insert(row.id).second, "an option must appear on one page only: " + name);
            check(!row.label.empty() && row.label.size() <= 26U,
                  "option names must be short: " + name);
            check(row.description.size() >= 20U && row.description.size() <= 170U,
                  "descriptions must explain the option within two lines: " + name);
            check(row.description.back() == '.', "descriptions are full sentences: " + name);
            for (const std::string_view word : jargon)
                check(lower(row.label).find(word) == std::string::npos,
                      "option names must be plain language: " + name);
        }
    }

    // Stored names such as "rebirth" or "aces" must never reach the screen.
    Settings settings = all_rows_active();
    for (const ui::SettingRow &row : every_row())
    {
        if (row.kind == ui::SettingKind::slider || row.id == ui::SettingId::audio_output)
            continue;
        for (int step = 0; step < 12; ++step)
        {
            const std::string shown = value(settings, row.id);
            check(!shown.empty(), "every option must show a value");
            check(shown == [&] {
                std::string upper = shown;
                std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
                    return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
                });
                return upper;
            }(), "values are shown in capitals: " + shown);
            for (const std::string_view word : jargon)
                check(lower(shown).find(word) == std::string::npos,
                      "values must be plain language: " + shown);
            ui::adjust_setting(settings, row.id, 1);
        }
    }

    // The two "which look" options must not share a word, or they get confused.
    std::set<std::string> lighting;
    std::set<std::string> packs;
    Settings look;
    for (int step = 0; step < 4; ++step)
    {
        lighting.insert(value(look, ui::SettingId::presentation));
        packs.insert(value(look, ui::SettingId::texture_style));
        ui::adjust_setting(look, ui::SettingId::presentation, 1);
        ui::adjust_setting(look, ui::SettingId::texture_style, 1);
    }
    check(lighting.size() == 2U && packs.size() == 3U, "both look options must cycle fully");
    check(lighting.contains("ORIGINAL") && lighting.contains("ENHANCED"),
          "the lighting style is named for what it does, not after a code name");
    for (const std::string &style : lighting)
        for (const std::string &pack : packs)
            check(pack.find(style) == std::string::npos && style.find(pack) == std::string::npos,
                  "Lighting style and Texture pack must use different words");
    const auto &graphics = ui::rows_for_tab(ui::SettingsTab::graphics);
    const auto row_named = [&](ui::SettingId id) {
        return *std::find_if(graphics.begin(), graphics.end(),
                             [&](const ui::SettingRow &row) { return row.id == id; });
    };
    check(row_named(ui::SettingId::presentation).label != row_named(ui::SettingId::texture_style).label &&
              row_named(ui::SettingId::texture_style).requires_restart &&
              !row_named(ui::SettingId::presentation).requires_restart,
          "only the texture pack needs a restart, and the menu must say so");
}

void every_row_changes_its_value_in_both_directions()
{
    for (const ui::SettingRow &row : every_row())
    {
        if (row.id == ui::SettingId::audio_output)
            continue; // Device enumeration lives in the application.
        const std::string name{row.label};
        Settings settings = all_rows_active();
        check(ui::setting_unavailable_reason(settings, row.id).empty(),
              "every row must be active in the reference configuration: " + name);
        if (row.kind == ui::SettingKind::slider)
        {
            for (int press = 0; press < 400; ++press)
                ui::adjust_setting(settings, row.id, 1);
            const std::string top = value(settings, row.id);
            check(near(ui::setting_normalized_value(settings, row.id), 1.0F, 0.002F),
                  "a slider held right must reach the end of its bar: " + name);
            ui::adjust_setting(settings, row.id, 1);
            check(value(settings, row.id) == top, "a slider must stop at its maximum: " + name);
            ui::adjust_setting(settings, row.id, -1);
            check(value(settings, row.id) != top, "one step back must show: " + name);
            for (int press = 0; press < 400; ++press)
                ui::adjust_setting(settings, row.id, -1);
            check(near(ui::setting_normalized_value(settings, row.id), 0.0F, 0.002F),
                  "a slider held left must reach the start of its bar: " + name);
            const std::string bottom = value(settings, row.id);
            ui::adjust_setting(settings, row.id, -1);
            check(value(settings, row.id) == bottom, "a slider must stop at its minimum: " + name);
            check(bottom != top, "a slider must have a range: " + name);
            continue;
        }
        // Toggles and choices are rings: one step changes the value, the
        // opposite step returns to it, and enough steps come back round.
        const std::string start = value(settings, row.id);
        ui::adjust_setting(settings, row.id, 1);
        check(value(settings, row.id) != start, "right must change the value: " + name);
        ui::adjust_setting(settings, row.id, -1);
        check(value(settings, row.id) == start, "left must undo right: " + name);
        ui::adjust_setting(settings, row.id, -1);
        check(value(settings, row.id) != start, "left must change the value: " + name);
        ui::adjust_setting(settings, row.id, 1);
        check(value(settings, row.id) == start, "right must undo left: " + name);
        std::set<std::string> ring;
        for (int press = 0; press < 40; ++press)
        {
            ring.insert(value(settings, row.id));
            ui::adjust_setting(settings, row.id, 1);
        }
        check(ring.size() >= 2U && ring.contains(start), "a choice must cycle: " + name);
        if (row.kind == ui::SettingKind::toggle)
            check(ring.size() == 2U, "a toggle has exactly two states: " + name);
    }
}

void unavailable_options_say_why_and_keep_their_value()
{
    Settings settings = all_rows_active();
    for (const ui::SettingRow &row : every_row())
        check(ui::setting_unavailable_reason(settings, row.id).empty(),
              "nothing is greyed out in the reference configuration");

    settings.window_mode = WindowMode::borderless;
    const std::string_view resolution_reason =
        ui::setting_unavailable_reason(settings, ui::SettingId::resolution);
    check(!resolution_reason.empty() && resolution_reason.back() == '.',
          "Borderless must explain why Resolution is inactive");
    const int width = settings.window_width;
    ui::adjust_setting(settings, ui::SettingId::resolution, 1);
    const std::array modes{std::pair{1920, 1080}, std::pair{2560, 1440}};
    ui::adjust_resolution(settings, 1, modes);
    check(settings.window_width == width, "an inactive Resolution must not change");
    settings.window_mode = WindowMode::fullscreen;
    check(ui::setting_unavailable_reason(settings, ui::SettingId::resolution).empty(),
          "Fullscreen uses the resolution");

    settings.shadows_enabled = false;
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::shadow_darkness).empty(),
          "Shadow darkness is inactive while shadows are off");
    const float darkness = settings.shadow_darkness;
    ui::adjust_setting(settings, ui::SettingId::shadow_darkness, 1);
    check(near(settings.shadow_darkness, darkness), "an inactive slider must not move");
    check(ui::setting_unavailable_reason(settings, ui::SettingId::shadow_quality).empty(),
          "the Shadows row itself is how they are switched back on");

    settings.vsync = true;
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::frame_limit).empty(),
          "the frame-rate limit stands down while VSync paces the game");
    const int limit = settings.frame_limit;
    const FrameLimitMode mode = settings.frame_limit_mode;
    ui::adjust_setting(settings, ui::SettingId::frame_limit, 1);
    check(settings.frame_limit == limit && settings.frame_limit_mode == mode,
          "an inactive frame-rate limit must not change");
    check(ui::setting_unavailable_reason(settings, ui::SettingId::vsync).empty(),
          "VSync itself is always available");
}

void frame_rate_limit_covers_fixed_display_and_unlimited()
{
    Settings settings = all_rows_active();
    check(settings.frame_limit == 144 && settings.frame_limit_mode == FrameLimitMode::fixed &&
              value(settings, ui::SettingId::frame_limit) == "144 FPS",
          "default cap");
    const std::array<std::string_view, 10> ring{"165 FPS",       "240 FPS", "360 FPS",
                                                "MATCH DISPLAY", "UNLIMITED", "30 FPS",
                                                "60 FPS",        "90 FPS",  "120 FPS",
                                                "144 FPS"};
    for (const std::string_view expected : ring)
    {
        ui::adjust_setting(settings, ui::SettingId::frame_limit, 1);
        check(value(settings, ui::SettingId::frame_limit) == expected,
              "frame-rate limit ring is out of order at " + std::string{expected});
        if (expected == "UNLIMITED")
            check(settings.frame_limit == 0 &&
                      settings.frame_limit_mode == FrameLimitMode::unlimited,
                  "UNLIMITED must store the zero the frame limiter treats as no cap");
        else
            check(settings.frame_limit >= 30 && settings.frame_limit <= 360,
                  "a capped mode must always hold a usable number for the limiter");
        if (expected == "MATCH DISPLAY")
            check(settings.frame_limit_mode == FrameLimitMode::display, "display mode stored");
    }
    settings.frame_limit = 30;
    settings.frame_limit_mode = FrameLimitMode::fixed;
    ui::adjust_setting(settings, ui::SettingId::frame_limit, -1);
    check(value(settings, ui::SettingId::frame_limit) == "UNLIMITED", "ring wraps downward");
    ui::adjust_setting(settings, ui::SettingId::frame_limit, -1);
    check(value(settings, ui::SettingId::frame_limit) == "MATCH DISPLAY", "ring steps downward");

    // A hand-typed value steps to its neighbour in the direction pressed.
    Settings typed = all_rows_active();
    typed.frame_limit = 75;
    ui::adjust_setting(typed, ui::SettingId::frame_limit, 1);
    check(typed.frame_limit == 90, "75 steps up to 90");
    typed.frame_limit = 75;
    ui::adjust_setting(typed, ui::SettingId::frame_limit, -1);
    check(typed.frame_limit == 60, "75 steps down to 60");

    Settings clamp;
    clamp.frame_limit = 5000;
    clamp.normalize();
    check(clamp.frame_limit == 360, "caps above the supported range clamp");
    clamp.frame_limit = 7;
    clamp.normalize();
    check(clamp.frame_limit == 30, "caps below the supported range clamp");
    clamp.frame_limit = 0;
    clamp.normalize();
    check(clamp.frame_limit == 0 && clamp.frame_limit_mode == FrameLimitMode::unlimited,
          "a literal zero means unlimited");
    clamp.frame_limit = 200;
    clamp.frame_limit_mode = FrameLimitMode::unlimited;
    clamp.normalize();
    check(clamp.frame_limit == 0, "unlimited always stores zero");
    clamp.frame_limit_mode = static_cast<FrameLimitMode>(77);
    clamp.frame_limit = 60;
    clamp.normalize();
    check(clamp.frame_limit_mode == FrameLimitMode::fixed && clamp.frame_limit == 60,
          "an invalid mode falls back to a fixed cap");
}

void shadows_row_is_one_ring_over_two_fields()
{
    Settings settings;
    const std::array<std::string_view, 6> ring{"ULTRA", "OFF", "LOW", "MEDIUM", "HIGH", "ULTRA"};
    check(value(settings, ui::SettingId::shadow_quality) == "HIGH", "default shadows");
    for (const std::string_view expected : ring)
    {
        ui::adjust_setting(settings, ui::SettingId::shadow_quality, 1);
        check(value(settings, ui::SettingId::shadow_quality) == expected,
              "shadow ring out of order at " + std::string{expected});
        check(settings.shadows_enabled == (expected != "OFF"), "OFF must disable the passes");
    }
    ui::adjust_setting(settings, ui::SettingId::shadow_quality, 1);
    ui::adjust_setting(settings, ui::SettingId::shadow_quality, -1);
    check(value(settings, ui::SettingId::shadow_quality) == "ULTRA", "ring steps downward");

    // Quality no longer decides darkness: every level keeps the chosen look.
    const float darkness = settings.shadow_darkness;
    for (int press = 0; press < 5; ++press)
    {
        ui::adjust_setting(settings, ui::SettingId::shadow_quality, 1);
        check(near(settings.shadow_darkness, darkness),
              "changing shadow quality must not change how dark shadows are");
    }
    check(near(legacy_shadow_darkness(QualityLevel::low), 0.25F) &&
              near(legacy_shadow_darkness(QualityLevel::medium), 0.45F) &&
              near(legacy_shadow_darkness(QualityLevel::high), 0.68F) &&
              near(legacy_shadow_darkness(QualityLevel::ultra), 0.86F),
          "the migration table must reproduce the old strengths exactly");
}

void resolution_steps_through_the_monitors_own_modes()
{
    // Unsorted, with a duplicate and a mode too small for the interface.
    const std::array modes{std::pair{2560, 1440}, std::pair{800, 600},  std::pair{1920, 1080},
                           std::pair{1280, 720},  std::pair{1920, 1080}, std::pair{1920, 1200}};
    Settings settings;
    settings.window_mode = WindowMode::fullscreen;
    settings.window_width = 1280;
    settings.window_height = 720;
    const std::array expected{std::pair{1920, 1080}, std::pair{1920, 1200},
                              std::pair{2560, 1440}, std::pair{1280, 720}};
    for (const auto &[width, height] : expected)
    {
        ui::adjust_resolution(settings, 1, modes);
        check(settings.window_width == width && settings.window_height == height,
              "resolutions must step in size order, skip duplicates and unusable modes, wrap");
    }
    ui::adjust_resolution(settings, -1, modes);
    check(settings.window_width == 2560 && settings.window_height == 1440, "wraps downward");

    // A dragged window size the monitor does not list.
    settings.window_mode = WindowMode::windowed;
    settings.window_width = 1723;
    settings.window_height = 954;
    Settings up = settings;
    ui::adjust_resolution(up, 1, modes);
    check(up.window_width == 1920 && up.window_height == 1080, "next larger listed size");
    Settings down = settings;
    ui::adjust_resolution(down, -1, modes);
    check(down.window_width == 1280 && down.window_height == 720, "next smaller listed size");

    // Without platform information the built-in list still works.
    Settings fallback;
    check(fallback.window_width == 1600 && fallback.window_height == 900, "default window");
    ui::adjust_setting(fallback, ui::SettingId::resolution, 1);
    check(fallback.window_width == 1920 && fallback.window_height == 1080, "built-in list up");
    ui::adjust_resolution(fallback, -1, {});
    ui::adjust_resolution(fallback, -1, {});
    check(fallback.window_width == 1280 && fallback.window_height == 720, "built-in list down");
    check(value(fallback, ui::SettingId::resolution) == "1280 X 720", "resolution is readable");

    const std::array unusable{std::pair{640, 480}};
    Settings stuck = fallback;
    ui::adjust_resolution(stuck, 1, unusable);
    check(stuck.window_width == 1280, "a list with no usable mode changes nothing");
}

void reset_restores_one_page_and_only_that_page()
{
    const Settings defaults;
    const auto changed_everywhere = [] {
        Settings settings = all_rows_active();
        for (const ui::SettingRow &row : every_row())
            for (int press = 0; press < 3; ++press)
                ui::adjust_setting(settings, row.id, -1);
        settings.audio_output_id = "{device}";
        settings.audio_output_name = "Headset";
        return settings;
    };
    for (const ui::SettingsTab page : ui::all_tabs())
    {
        Settings settings = changed_everywhere();
        const Settings before = settings;
        ui::reset_tab(settings, page);
        for (const ui::SettingsTab tab : ui::all_tabs())
            for (const ui::SettingRow &row : ui::rows_for_tab(tab))
            {
                const std::string name{row.label};
                if (tab == page)
                    check(value(settings, row.id) == value(defaults, row.id),
                          "reset must restore every option on the page: " + name);
                // The preset row summarises other rows, some on the Effects
                // page; it is allowed to change when that page is reset.
                // (Bloom, Light shafts and the preset-controlled rows of
                // Settings::graphics that live on the Effects page.)
                else if (row.id != ui::SettingId::quality_preset &&
                         !(page == ui::SettingsTab::graphics &&
                           (row.id == ui::SettingId::bloom || row.id == ui::SettingId::sun_shafts ||
                            row.id == ui::SettingId::volumetric_quality ||
                            row.id == ui::SettingId::depth_of_field ||
                            row.id == ui::SettingId::lens_flare ||
                            row.id == ui::SettingId::heat_haze ||
                            row.id == ui::SettingId::embers)))
                    check(value(settings, row.id) == value(before, row.id),
                          "reset must leave other pages alone: " + name);
            }
        // Choices no page shows (last game mode, generation) are not a page's
        // to reset.
        check(settings.game_mode == before.game_mode &&
                  settings.gameplay_profile == before.gameplay_profile,
              "reset must not touch state that has no row");
    }

    Settings settings = changed_everywhere();
    ui::reset_tab(settings, ui::SettingsTab::graphics);
    check(ui::detect_quality_preset(settings) == ui::QualityPreset::high,
          "resetting Graphics must return to the default preset, bloom and shafts included");
    ui::reset_tab(settings, ui::SettingsTab::audio);
    check(settings.audio_output_id.empty() && settings.audio_output_name.empty(),
          "resetting Audio returns to the system default device");
    for (const ui::SettingsTab tab : ui::all_tabs())
        ui::reset_tab(settings, tab);
    for (const ui::SettingRow &row : every_row())
        check(value(settings, row.id) == value(defaults, row.id),
              "resetting every page must equal a fresh install");

    Settings single = changed_everywhere();
    // The hands' projection has no row any more; it is the file-only neighbour.
    single.viewmodel_fov = 70.0F;
    ui::reset_setting(single, ui::SettingId::vertical_fov);
    check(near(single.vertical_fov, defaults.vertical_fov) &&
              !near(single.viewmodel_fov, defaults.viewmodel_fov) &&
              !near(single.exposure_compensation, defaults.exposure_compensation),
          "resetting one option must not touch its neighbour");
}

void legacy_file_loads_and_keeps_the_players_picture()
{
    const TemporaryFile file{"legacy"};
    file.write(legacy_profile);
    const Settings loaded = file.load();
    // Everything the old file said is still what the player gets.
    check(loaded.window_mode == WindowMode::borderless && loaded.vsync &&
              loaded.frame_limit == 240 && loaded.frame_limit_mode == FrameLimitMode::fixed &&
              loaded.texture_style == AssetStyle::classic_hd &&
              loaded.presentation == PresentationProfile::rebirth &&
              loaded.shadow_quality == QualityLevel::ultra &&
              loaded.particle_quality == QualityLevel::ultra && loaded.view_distance == 32 &&
              near(loaded.vertical_fov, 96.0F) && near(loaded.viewmodel_fov, 78.0F) &&
              near(loaded.creative_flight_speed, 32.0F) && !loaded.subtitles &&
              !loaded.hud_compass && loaded.gameplay_profile == 1 &&
              loaded.audio_output_name == "System default (Speakers (Echo Show 8-5WJ))",
          "an existing settings file must load unchanged");
    // Options the file has never heard of take their defaults...
    check(loaded.shadows_enabled && loaded.texture_filtering == 8 && loaded.bloom &&
              loaded.sun_shafts && !loaded.show_fps && loaded.motion_smoothing &&
              loaded.low_latency == LowLatencyMode::on,
          "missing keys must take defaults");
    // ...except where a default would change what the player sees.
    check(near(loaded.shadow_darkness, 0.86F),
          "ULTRA shadows used to be the darkest; that darkness must carry over");
    check(loaded.anti_aliasing == AntiAliasing::fxaa,
          "the never-implemented TAA name must become the smoothing that really ran");
    // Texture sharpness stays at the 8x the old renderer always used rather than
    // jumping to ULTRA's 16x, so the menu honestly reports a custom mix.
    check(value(loaded, ui::SettingId::quality_preset) == "CUSTOM",
          "migration must not silently raise options to match a preset");
    Settings as_ultra = loaded;
    ui::adjust_setting(as_ultra, ui::SettingId::texture_filtering, 1);
    check(value(as_ultra, ui::SettingId::quality_preset) == "ULTRA",
          "one step of texture sharpness completes the ULTRA preset for this profile");

    file.write("shadow_quality=low\nanti_aliasing=msaa_4x\nview_distance=5\nvertical_fov=110\n"
               "color_grading=1.5\n");
    const Settings older = file.load();
    check(near(older.shadow_darkness, 0.25F) && older.shadow_quality == QualityLevel::low,
          "LOW shadows used to be the faintest");
    check(older.anti_aliasing == AntiAliasing::fxaa, "MSAA was never implemented either");
    check(older.view_distance == 8,
          "view distances below 32 blocks were never drawn differently and clamp to it");
    check(near(older.vertical_fov, 105.0F), "the renderer never drew more than 105 degrees");
    check(near(older.color_grading, 1.0F), "colour finish above 100% never changed the picture");

    file.write("shadow_quality=ultra\nshadow_darkness=0.3\nanti_aliasing=off\n");
    const Settings current = file.load();
    check(near(current.shadow_darkness, 0.3F),
          "once the file has its own darkness, quality must not override it");
    check(current.anti_aliasing == AntiAliasing::off, "an explicit Off stays off");

    file.write("ui_scale=1.2\n");
    check(near(file.load().shadow_darkness, Settings{}.shadow_darkness),
          "a file without either shadow key gets the default darkness");
}

void damaged_files_are_clamped_defaulted_or_ignored()
{
    const Settings defaults;
    const TemporaryFile file{"damaged"};
    check(first_difference(file.load(), defaults).empty(), "a missing file is a fresh install");
    file.write("");
    check(first_difference(file.load(), defaults).empty(), "an empty file is a fresh install");
    file.write("\x01\x02garbage\n=novalue\nnoequals\n# vsync=0\n\n====\n");
    check(first_difference(file.load(), defaults).empty(), "junk lines are skipped");

    file.write("some_future_option=42\nanother.unknown=key\nvsync=0\nfuture_enum=sparkly\n");
    Settings loaded = file.load();
    check(!loaded.vsync, "known keys still load beside unknown ones");
    loaded.vsync = defaults.vsync;
    check(first_difference(loaded, defaults).empty(), "unknown keys change nothing");

    // Every numeric option, far outside its range in both directions.
    file.write("window_width=99999\nwindow_height=1\nframe_limit=100000\nui_scale=50\n"
               "safe_area=0\nrender_scale=77\nexposure_compensation=-40\ncolor_grading=9\n"
               "view_distance=1000\nvertical_fov=5\nviewmodel_fov=999\nmouse_sensitivity=-3\n"
               "controller_sensitivity=1e9\ncreative_flight_speed=0\ncamera_bob=4\n"
               "screen_shake=-4\nmaster_volume=11\nmusic_volume=-1\neffects_volume=3\n"
               "ambience_volume=-0.5\nvoice_volume=2\nsubtitle_size=40\nshadow_darkness=5\n"
               "texture_filtering=1000\ngameplay_profile=9\ngame_mode=-4\ngame_difficulty=99\n");
    loaded = file.load();
    check(loaded.window_width == 7680 && loaded.window_height == 540 && loaded.frame_limit == 360 &&
              near(loaded.ui_scale, 1.5F) && near(loaded.safe_area, 0.8F) &&
              near(loaded.render_scale, 2.0F) && near(loaded.exposure_compensation, -2.0F) &&
              near(loaded.color_grading, 1.0F) && loaded.view_distance == 32 &&
              near(loaded.vertical_fov, 36.0F) && near(loaded.viewmodel_fov, 110.0F) &&
              near(loaded.mouse_sensitivity, 0.1F) && near(loaded.controller_sensitivity, 4.0F) &&
              near(loaded.creative_flight_speed, 4.0F) && near(loaded.camera_bob, 1.0F) &&
              near(loaded.screen_shake, 0.0F) && near(loaded.master_volume, 1.0F) &&
              near(loaded.music_volume, 0.0F) && near(loaded.effects_volume, 1.0F) &&
              near(loaded.ambience_volume, 0.0F) && near(loaded.voice_volume, 1.0F) &&
              near(loaded.subtitle_size, 1.5F) && near(loaded.shadow_darkness, 0.9F) &&
              loaded.texture_filtering == 16 && loaded.gameplay_profile == 1 &&
              loaded.game_mode == 0 && loaded.game_difficulty == 3,
          "out-of-range numbers must clamp to the nearest valid value");
    file.write("shadow_darkness=-9\ntexture_filtering=-5\nview_distance=-1\nrender_scale=0\n");
    loaded = file.load();
    check(near(loaded.shadow_darkness, 0.25F) && loaded.texture_filtering == 1 &&
              loaded.view_distance == 8 && near(loaded.render_scale, 0.5F),
          "negative numbers clamp to the minimum");
    for (const auto &[written, expected] :
         {std::pair{0, 1}, std::pair{1, 1}, std::pair{2, 2}, std::pair{3, 4}, std::pair{5, 4},
          std::pair{6, 8}, std::pair{7, 8}, std::pair{11, 8}, std::pair{12, 16},
          std::pair{16, 16}})
    {
        file.write("texture_filtering=" + std::to_string(written) + "\n");
        check(file.load().texture_filtering == expected,
              "texture filtering snaps to a level the GPU accepts");
    }

    // Values that are not numbers, or not finite, fall back to the default
    // instead of poisoning the setting.
    file.write("render_scale=nan\nui_scale=inf\nvertical_fov=-inf\nmaster_volume=loud\n"
               "frame_limit=fast\nview_distance=\nshadow_darkness=1e999\nvsync=maybe\n"
               "window_mode=hologram\nframe_limit_mode=warp\nlow_latency=perhaps\n"
               "tone_mapping=technicolor\nshadow_quality=extreme\npresentation=vr\n"
               "texture_style=8k\ncolor_filter=sepia\nbloom=yes\n");
    loaded = file.load();
    check(first_difference(loaded, defaults).empty(),
          "unreadable values must leave the default: " + first_difference(loaded, defaults));

    // Hand edits: spaces around the sign, Windows line endings, repeated keys.
    file.write("vsync = 0\r\n  view_distance=20  \r\nshadow_darkness =\t0.5\r\n"
               "frame_limit_mode = unlimited\r\nbloom=0\r\nbloom=1\r\n"
               "audio_output_name= Speakers (USB = 2) \r\n");
    loaded = file.load();
    check(!loaded.vsync && loaded.view_distance == 20 && near(loaded.shadow_darkness, 0.5F) &&
              loaded.frame_limit_mode == FrameLimitMode::unlimited && loaded.frame_limit == 0 &&
              loaded.bloom && loaded.audio_output_name == "Speakers (USB = 2)",
          "spacing, CRLF and repeated keys must be tolerated; the last value wins");
}

void settings_file_round_trips_every_field()
{
    const TemporaryFile file{"round-trip"};
    const Settings original = everything_changed();
    Settings in_range = original;
    in_range.normalize();
    // (Only the cap is rewritten: display mode keeps a usable number for it.)
    check(first_difference(original, in_range).empty(),
          "the fixture must be valid as written: " + first_difference(original, in_range));
    check(first_difference(original, Settings{}).size() > 0U, "fixture differs from defaults");

    file.save(original);
    const std::string first_text = file.read();
    const Settings loaded = file.load();
    check(first_difference(original, loaded).empty(),
          "field lost between save and load: " + first_difference(original, loaded));
    for (const ui::SettingRow &row : every_row())
        check(value(original, row.id) == value(loaded, row.id),
              "menu value changed across a restart: " + std::string{row.label});
    file.save(loaded);
    check(file.read() == first_text, "saving what was loaded must reproduce the file exactly");

    // The same holds for a fresh install and for an unlimited frame rate.
    Settings fresh;
    file.save(fresh);
    check(first_difference(file.load(), fresh).empty(), "defaults round trip");
    fresh.frame_limit_mode = FrameLimitMode::unlimited;
    fresh.normalize();
    file.save(fresh);
    const Settings unlimited = file.load();
    check(unlimited.frame_limit == 0 && unlimited.frame_limit_mode == FrameLimitMode::unlimited,
          "unlimited survives a restart");

    // Every option moved off its default has its own line in the file, so the
    // comparison above cannot pass by accident.
    for (const std::string_view key :
         {"frame_limit_mode=", "shadows_enabled=", "shadow_darkness=", "texture_filtering=",
          "bloom=", "sun_shafts=", "show_fps=", "motion_smoothing=", "low_latency=",
          "anti_aliasing=", "view_distance=", "window_mode=", "interface_style="})
        check(first_text.find(std::string{"\n"} + std::string{key}) != std::string::npos,
              "missing line in the settings file: " + std::string{key});

    // A file from this build still loads in the old format's terms: one
    // "key=value" per line, no sections, comments start with '#'.
    std::istringstream lines{first_text};
    std::string line;
    std::set<std::string> keys;
    while (std::getline(lines, line))
    {
        if (line.empty() || line.front() == '#')
            continue;
        const auto separator = line.find('=');
        check(separator != std::string::npos && separator > 0U, "malformed line: " + line);
        check(keys.insert(line.substr(0U, separator)).second, "duplicate key: " + line);
    }
}

void pages_are_ordered_and_navigable()
{
    const auto tabs = ui::all_tabs();
    const std::array expected{ui::SettingsTab::display,     ui::SettingsTab::graphics,
                              ui::SettingsTab::effects,     ui::SettingsTab::performance,
                              ui::SettingsTab::gameplay,    ui::SettingsTab::controls,
                              ui::SettingsTab::interface,   ui::SettingsTab::audio,
                              ui::SettingsTab::accessibility, ui::SettingsTab::enhancements};
    check(tabs.size() == expected.size() && std::equal(tabs.begin(), tabs.end(), expected.begin()),
          "pages must appear in the documented order, the not-in-the-original page last");
    for (std::size_t index = 0U; index < tabs.size(); ++index)
    {
        // --settings-tab and the sidebar both rely on index == enum value.
        check(static_cast<std::size_t>(tabs[index]) == index, "page order must match the enum");
        check(ui::next_tab(tabs[index], 1) == tabs[(index + 1U) % tabs.size()] &&
                  ui::next_tab(tabs[index], -1) == tabs[(index + tabs.size() - 1U) % tabs.size()],
              "page navigation must wrap in both directions");
    }
    check(ui::tab_title(ui::SettingsTab::performance) == "PERFORMANCE & LATENCY",
          "the performance page is headed Performance & latency");

    const auto has = [](ui::SettingsTab tab, ui::SettingId id) {
        const auto &rows = ui::rows_for_tab(tab);
        return std::any_of(rows.begin(), rows.end(),
                           [&](const ui::SettingRow &row) { return row.id == id; });
    };
    check(ui::rows_for_tab(ui::SettingsTab::graphics).front().id == ui::SettingId::quality_preset,
          "the preset leads the Graphics page");
    for (const ui::SettingId id :
         {ui::SettingId::window_mode, ui::SettingId::resolution,
          ui::SettingId::exposure_compensation, ui::SettingId::vertical_fov})
        check(has(ui::SettingsTab::display, id), "Display page is missing an option");
    // One field of view slider. The hands keep retail's own projection, so the
    // old "Held item field of view" row is gone (tests/test_view_projection.cpp).
    for (const ui::SettingsTab tab : ui::all_tabs())
        check(!has(tab, ui::SettingId::viewmodel_fov),
              "the held-item projection must not be a menu row");
    for (const ui::SettingId id :
         {ui::SettingId::presentation, ui::SettingId::texture_style, ui::SettingId::view_distance,
          ui::SettingId::shadow_quality, ui::SettingId::shadow_darkness,
          ui::SettingId::anti_aliasing, ui::SettingId::texture_filtering,
          ui::SettingId::particle_quality})
        check(has(ui::SettingsTab::graphics, id), "Graphics page is missing an option");
    for (const ui::SettingId id :
         {ui::SettingId::bloom, ui::SettingId::sun_shafts, ui::SettingId::ambient_occlusion,
          ui::SettingId::ore_glow, ui::SettingId::auto_exposure, ui::SettingId::tone_mapping,
          ui::SettingId::color_grading})
        check(has(ui::SettingsTab::effects, id), "Effects page is missing an option");
    // Everything about pacing and input delay, including the frame loop's own
    // switches, lives on one page.
    for (const ui::SettingId id :
         {ui::SettingId::render_scale, ui::SettingId::vsync, ui::SettingId::frame_limit,
          ui::SettingId::low_latency, ui::SettingId::motion_smoothing, ui::SettingId::show_fps})
        check(has(ui::SettingsTab::performance, id), "Performance page is missing an option");
    for (const ui::SettingId id :
         {ui::SettingId::mouse_sensitivity, ui::SettingId::raw_mouse_input,
          ui::SettingId::invert_y, ui::SettingId::controller_sensitivity,
          ui::SettingId::controller_vibration})
        check(has(ui::SettingsTab::controls, id), "Controls page is missing an option");
    check(has(ui::SettingsTab::gameplay, ui::SettingId::camera_bob) &&
              has(ui::SettingsTab::gameplay, ui::SettingId::screen_shake) &&
              has(ui::SettingsTab::audio, ui::SettingId::voice_volume) &&
              has(ui::SettingsTab::accessibility, ui::SettingId::reduced_motion),
          "comfort and voice options must be reachable");
    // Options with no implementation behind them must not be offered.
    for (const ui::SettingRow &row : every_row())
        check(row.id != ui::SettingId::texture_quality &&
                  row.id != ui::SettingId::controller_aim_assist &&
                  row.id != ui::SettingId::hold_to_sprint && row.id != ui::SettingId::color_filter,
              "an option nothing reads must not be in the menu");
}

void menu_model_drives_rows_presets_and_inactive_rows()
{
    Settings settings = all_rows_active();
    ui::MenuModel menu;
    menu.open(ui::MenuScreen::settings);
    check(menu.settings_tab() == ui::SettingsTab::display && menu.selection() == 0U,
          "settings open on the first page");
    const auto go_to = [&](ui::SettingsTab tab, ui::SettingId id) {
        for (std::size_t step = 0U; step < ui::all_tabs().size() && menu.settings_tab() != tab;
             ++step)
            menu.change_tab(1, settings);
        check(menu.settings_tab() == tab, "page must be reachable with next-page");
        const auto &rows = ui::rows_for_tab(tab);
        for (std::size_t index = 0U; index < rows.size(); ++index)
            if (rows[index].id == id)
            {
                menu.set_selection(index, settings);
                return;
            }
        check(false, "row not found on its page");
    };

    go_to(ui::SettingsTab::graphics, ui::SettingId::quality_preset);
    (void)menu.adjust_selected(settings, 1);
    check(ui::detect_quality_preset(settings) == ui::QualityPreset::ultra &&
              settings.view_distance == 32 && settings.texture_filtering == 16 &&
              settings.shadow_quality == QualityLevel::ultra,
          "choosing a preset must set the individual options");
    go_to(ui::SettingsTab::graphics, ui::SettingId::view_distance);
    (void)menu.adjust_selected(settings, -1);
    check(value(settings, ui::SettingId::quality_preset) == "CUSTOM",
          "changing an option through the menu must show CUSTOM");
    go_to(ui::SettingsTab::graphics, ui::SettingId::quality_preset);
    (void)menu.activate(settings);
    check(value(settings, ui::SettingId::quality_preset) == "LOW" && !settings.shadows_enabled &&
              !settings.bloom && settings.anti_aliasing == AntiAliasing::off,
          "activating the preset row from CUSTOM must pick a real preset");

    settings.vsync = true;
    go_to(ui::SettingsTab::performance, ui::SettingId::frame_limit);
    const int limit = settings.frame_limit;
    (void)menu.adjust_selected(settings, 1);
    (void)menu.activate(settings);
    check(settings.frame_limit == limit, "an inactive row must ignore the menu");

    menu.select_next(-1, settings);
    menu.select_next(-1, settings);
    menu.select_next(-1, settings);
    check(menu.selection() < ui::rows_for_tab(menu.settings_tab()).size(),
          "row selection stays inside the page");
    menu.set_selection(999U, settings);
    check(menu.selection() == ui::rows_for_tab(menu.settings_tab()).size() - 1U,
          "an out-of-range selection clamps to the last row");
    menu.change_tab(1, settings);
    check(menu.selection() == 0U, "a new page starts at its first row");
}

// ---------------------------------------------------------------------------
// Interface style (Settings::ui_style, file key `interface_style`): ONE choice
// for the HUD and the inventory, crate and crafting screens together.
// Remastered (OpenMinerZ's own) is the default; Classic and Modern are exact
// copies of the retail releases; Automatic is "the retail look of the world I
// am in". Presentation only.

void interface_style_defaults_to_remastered_and_resolves_to_three_values()
{
    const Settings defaults;
    check(defaults.ui_style == UiStyle::remastered,
          "a fresh install must get OpenMinerZ's own interface, Remastered");
    for (const bool world_is_modern : {false, true})
    {
        const UiStyle world = world_is_modern ? UiStyle::modern : UiStyle::classic;
        check(resolved_ui_style(defaults, world_is_modern) == UiStyle::remastered,
              "Remastered is Remastered in either world");
        check(retail_build_of(defaults.ui_style, world_is_modern) == world,
              "Remastered draws the world's own item names, fonts and icons");
        Settings follow;
        follow.ui_style = UiStyle::automatic;
        check(resolved_ui_style(follow, world_is_modern) == world &&
                  is_retail_style(resolved_ui_style(follow, world_is_modern)),
              "automatic must mean the retail look of the world being played, never Remastered");
        for (const UiStyle forced : {UiStyle::classic, UiStyle::modern})
        {
            Settings settings;
            settings.ui_style = forced;
            check(resolved_ui_style(settings, world_is_modern) == forced &&
                      retail_build_of(forced, world_is_modern) == forced,
                  "an explicit retail style wins over the world and uses its own release's art");
        }
        for (const UiStyle any :
             {UiStyle::automatic, UiStyle::classic, UiStyle::modern, UiStyle::remastered})
        {
            check(resolve_ui_style(any, world_is_modern) != UiStyle::automatic,
                  "the resolved style is never automatic");
            check(is_retail_style(retail_build_of(any, world_is_modern)),
                  "the art in use is always exactly one retail release's");
        }
    }
    check(is_retail_style(UiStyle::classic) && is_retail_style(UiStyle::modern) &&
              !is_retail_style(UiStyle::remastered),
          "exactly Classic and Modern are the retail copies");
    // The enumerators keep their numbers: diagnostics and --ui-style rely on them.
    static_assert(static_cast<int>(UiStyle::automatic) == 0 && static_cast<int>(UiStyle::classic) == 1 &&
                  static_cast<int>(UiStyle::modern) == 2 && static_cast<int>(UiStyle::remastered) == 3);
    // Usable in a constant expression, so a layout table can be selected by one.
    static_assert(resolve_ui_style(UiStyle::automatic, true) == UiStyle::modern);
    static_assert(resolve_ui_style(UiStyle::automatic, false) == UiStyle::classic);
    static_assert(resolve_ui_style(UiStyle::classic, true) == UiStyle::classic);
    static_assert(resolve_ui_style(UiStyle::remastered, true) == UiStyle::remastered);
    static_assert(retail_build_of(UiStyle::remastered, false) == UiStyle::classic);
}

void interface_style_persists_migrates_and_survives_damage()
{
    const TemporaryFile file{"interface-style"};
    // Every choice comes back exactly, under the one key, and the two retired
    // keys are never written again.
    for (const UiStyle style :
         {UiStyle::remastered, UiStyle::classic, UiStyle::modern, UiStyle::automatic})
    {
        Settings chosen;
        chosen.ui_style = style;
        file.save(chosen);
        // (Read back in binary: on Windows the line ends "\r\n".)
        std::string text = file.read();
        std::erase(text, '\r');
        check(text.find("\ninterface_style=" + to_string(style) + "\n") != std::string::npos,
              "the style must be written under its documented key");
        check(text.find("\nui_style=") == std::string::npos &&
                  text.find("crafting_style=") == std::string::npos &&
                  text.find("crafting_menu_modern=") == std::string::npos,
              "the retired style keys must not be written");
        check(file.load().ui_style == style, "the style must survive a restart");
    }
    check(to_string(UiStyle::automatic) == "automatic" && to_string(UiStyle::classic) == "classic" &&
              to_string(UiStyle::modern) == "modern" && to_string(UiStyle::remastered) == "remastered",
          "the file values are remastered / classic / modern / automatic");

    // 1. A settings file from BEFORE the retail styles existed (it has
    //    `vitals_style` and no style key): Remastered was the only interface
    //    then and is what the player gets back, with everything else intact.
    file.write(legacy_profile);
    const Settings old_profile = file.load();
    check(old_profile.ui_style == UiStyle::remastered, "a file from before the styles is Remastered");
    check(old_profile.vitals_style == VitalsStyle::remastered,
          "and keeps the health look it had chosen");
    check(std::string{legacy_profile}.find("crafting_menu_modern=1") != std::string::npos,
          "(the fixture carries the retired crafting switch, which is ignored)");
    check(old_profile.gameplay_profile == 1 && old_profile.texture_style == AssetStyle::classic_hd,
          "the rest of an old file must be untouched by the new key");
    for (const std::string_view vitals : {"classic", "modern", "remastered", "none"})
    {
        file.write("hud_compass=1\nvitals_style=" + std::string{vitals} + "\n");
        const Settings loaded = file.load();
        check(loaded.ui_style == UiStyle::remastered && to_string(loaded.vitals_style) == vitals &&
                  loaded.hud_compass,
              "a pre-style file keeps its health look and its compass: " + std::string{vitals});
    }

    // 2. A file from the build that introduced the retail styles (`ui_style`,
    //    `crafting_style`, no `interface_style`). `automatic` was only that
    //    build's default: a player who never chose a retail look lands on
    //    Remastered. `classic` / `modern` was a choice and is kept. The
    //    crafting style is gone: crafting follows the interface.
    struct Interim final
    {
        std::string_view ui;
        std::string_view crafting;
        UiStyle expected;
    };
    for (const Interim &entry :
         {Interim{"automatic", "automatic", UiStyle::remastered},
          Interim{"automatic", "classic", UiStyle::remastered},
          Interim{"automatic", "modern", UiStyle::remastered},
          Interim{"classic", "automatic", UiStyle::classic},
          Interim{"classic", "modern", UiStyle::classic},
          Interim{"modern", "automatic", UiStyle::modern},
          Interim{"modern", "classic", UiStyle::modern}})
    {
        file.write("ui_style=" + std::string{entry.ui} + "\ncrafting_style=" +
                   std::string{entry.crafting} + "\nvitals_style=remastered\nhud_compass=0\n");
        Settings loaded = file.load();
        check(loaded.ui_style == entry.expected,
              "interim file ui_style=" + std::string{entry.ui} + " crafting_style=" +
                  std::string{entry.crafting} + " loaded as " + to_string(loaded.ui_style));
        check(!loaded.hud_compass, "a file is believed about the compass it states");
        // Saved by this build, it is this build's file from then on.
        file.save(loaded);
        std::string text = file.read();
        std::erase(text, '\r');
        check(text.find("\ninterface_style=" + to_string(entry.expected) + "\n") != std::string::npos &&
                  text.find("\nui_style=") == std::string::npos &&
                  text.find("crafting_style=") == std::string::npos,
              "a migrated file is rewritten with the one key");
        check(file.load().ui_style == entry.expected, "and loads the same again");
    }
    // `ui_style=automatic` alone (no crafting line) is the same interim file.
    file.write("ui_style=automatic\n");
    check(file.load().ui_style == UiStyle::remastered, "the interim default is not a retail choice");

    // 3. This build's key wins over a stale interim line left beside it, and
    //    here `automatic` IS a choice: the retail look of the world in play.
    file.write("ui_style=classic\ncrafting_style=modern\ninterface_style=automatic\n");
    check(file.load().ui_style == UiStyle::automatic, "interface_style is the key that counts");
    file.write("ui_style=modern\ninterface_style=remastered\n");
    check(file.load().ui_style == UiStyle::remastered, "interface_style wins over ui_style");

    // Damaged or unknown values are Remastered, never a retail look by accident.
    for (const std::string_view damaged :
         {"interface_style=retro\n", "interface_style=1\n", "interface_style=\n",
          "interface_style=CLASSIC\n", "ui_style=retro\ncrafting_style=\n", "ui_style=2\n",
          "ui_style=\ncrafting_style=remastered\n"})
    {
        file.write(damaged);
        check(file.load().ui_style == UiStyle::remastered,
              "an unreadable style must fall back to Remastered: " + std::string{damaged});
    }
    file.write("interface_style = modern \r\nvitals_style = none\r\n");
    const Settings hand_edited = file.load();
    check(hand_edited.ui_style == UiStyle::modern && hand_edited.vitals_style == VitalsStyle::none,
          "hand edits with spaces and CRLF must load");
    Settings out_of_range;
    out_of_range.ui_style = static_cast<UiStyle>(9);
    out_of_range.vitals_style = static_cast<VitalsStyle>(200);
    out_of_range.normalize();
    check(out_of_range.ui_style == UiStyle::remastered &&
              out_of_range.vitals_style == VitalsStyle::remastered,
          "an invalid in-memory style normalizes to Remastered");
}

void remastered_is_the_default_and_the_retail_styles_stay_exact()
{
    // A fresh install is Remastered with its own compass, as it always was;
    // the diagnostics OpenMinerZ adds are opt-in in every style.
    const Settings defaults;
    check(defaults.ui_style == UiStyle::remastered && defaults.hud_compass &&
              defaults.vitals_style == VitalsStyle::remastered,
          "the default interface is Remastered: its bars and its compass");
    check(!defaults.hud_clock && !defaults.hud_server && !defaults.hud_coordinates &&
              !defaults.hud_debug && !defaults.hud_hints,
          "the clock, session cards, debug panel and control hints are opt-in");
    check(defaults.hud_visible && defaults.hud_hotbar && defaults.hud_crosshair &&
              defaults.hud_distance && defaults.hud_item_name && defaults.hud_target &&
              defaults.hud_damage && defaults.hud_chat && defaults.hud_scope &&
              defaults.hud_roster && defaults.hud_notifications,
          "every element of the HUD is on by default");

    // The pieces that are OpenMinerZ's own belong to Remastered. In a retail
    // style (Classic, Modern, or Automatic, which is always one of the two)
    // the row says why it does nothing and keeps the stored choice.
    constexpr std::array remastered_only{ui::SettingId::hud_compass, ui::SettingId::hud_clock,
                                         ui::SettingId::hud_hints, ui::SettingId::hud_notifications};
    const auto &rows = ui::rows_for_tab(ui::SettingsTab::interface);
    for (const ui::SettingRow &row : rows)
        check(ui::setting_is_remastered_only(row.id) ==
                  (std::find(remastered_only.begin(), remastered_only.end(), row.id) !=
                   remastered_only.end()),
              "unexpected Remastered-only row: " + std::string{row.label});
    for (const ui::SettingId id : remastered_only)
    {
        Settings ours;
        check(ui::setting_unavailable_reason(ours, id).empty(),
              "every Remastered row works in the Remastered style");
        const std::string before = value(ours, id);
        ui::adjust_setting(ours, id, 1);
        check(value(ours, id) != before, "and its switch switches");
        for (const UiStyle retail : {UiStyle::classic, UiStyle::modern, UiStyle::automatic})
        {
            Settings copy = ours;
            copy.ui_style = retail;
            const std::string kept = value(copy, id);
            check(!ui::setting_unavailable_reason(copy, id).empty(),
                  "a Remastered-only row must say so in a retail style");
            ui::adjust_setting(copy, id, 1);
            check(value(copy, id) == kept, "and must keep the stored choice for later");
        }
    }
    // Rows the retail HUD also has stay available in every style.
    for (const ui::SettingId id :
         {ui::SettingId::ui_style, ui::SettingId::vitals_style, ui::SettingId::hud_hotbar,
          ui::SettingId::hud_crosshair, ui::SettingId::hud_distance, ui::SettingId::hud_chat,
          ui::SettingId::hud_damage, ui::SettingId::hud_item_name, ui::SettingId::hud_roster,
          ui::SettingId::ui_scale, ui::SettingId::safe_area, ui::SettingId::hud_debug})
        for (const UiStyle style :
             {UiStyle::remastered, UiStyle::classic, UiStyle::modern, UiStyle::automatic})
        {
            Settings settings;
            settings.ui_style = style;
            check(ui::setting_unavailable_reason(settings, id).empty(),
                  "a row every style has must never be unavailable");
        }

    // Health and energy. Remastered has three looks of its own and hidden, the
    // choices the row always had; the retail styles draw their release's own
    // meters, so there it only shows or hides them.
    const auto vitals = std::find_if(rows.begin(), rows.end(), [](const ui::SettingRow &row) {
        return row.id == ui::SettingId::vitals_style;
    });
    check(vitals != rows.end() && vitals->kind == ui::SettingKind::choice,
          "Health and energy is a choice row");
    Settings ours;
    const std::array<std::string_view, 4> looks{"BARS", "HEARTS", "TOP BARS", "HIDDEN"};
    check(value(ours, ui::SettingId::vitals_style) == looks[0], "Remastered starts with its bars");
    for (int press = 1; press <= 8; ++press)
    {
        Settings before = ours;
        ui::adjust_setting(ours, ui::SettingId::vitals_style, 1);
        check(value(ours, ui::SettingId::vitals_style) == looks[static_cast<std::size_t>(press % 4)],
              "the Remastered ring is BARS, HEARTS, TOP BARS, HIDDEN");
        before.vitals_style = ours.vitals_style;
        check(first_difference(before, ours).empty(), "the health row changed another setting");
    }
    ui::adjust_setting(ours, ui::SettingId::vitals_style, -1);
    check(value(ours, ui::SettingId::vitals_style) == "HIDDEN", "the ring steps downward and wraps");
    const TemporaryFile file{"vitals"};
    for (const std::string_view old : {"classic", "modern", "remastered", "none"})
        for (const UiStyle retail : {UiStyle::classic, UiStyle::modern, UiStyle::automatic})
        {
            file.write("interface_style=" + to_string(retail) + "\nvitals_style=" + std::string{old} +
                       "\n");
            Settings loaded = file.load();
            const bool shown = old != "none";
            check(value(loaded, ui::SettingId::vitals_style) == (shown ? "ON" : "OFF"),
                  "a retail style shows or hides its own meters: " + std::string{old});
            ui::adjust_setting(loaded, ui::SettingId::vitals_style, 1);
            check(value(loaded, ui::SettingId::vitals_style) == (shown ? "OFF" : "ON") &&
                      loaded.ui_style == retail,
                  "and the row is a plain switch there, which never changes the style");
        }
    // Changing the look never hides or shows the meters.
    Settings look;
    look.vitals_style = VitalsStyle::none;
    for (int press = 0; press < 4; ++press)
    {
        ui::adjust_setting(look, ui::SettingId::ui_style, 1);
        check(look.vitals_style == VitalsStyle::none, "the style row must not unhide the meters");
    }
    // The compass is believed as the file states it (see SettingsStore::load).
    file.write(legacy_profile);
    check(!file.load().hud_compass, "an old file that had the compass off keeps it off");
    file.write("hud_compass=1\nhud_clock=1\nvitals_style=remastered\n");
    check(file.load().hud_compass && file.load().hud_clock, "and one that had it on keeps it on");
}

void one_interface_style_row_leads_the_page_and_changes_nothing_else()
{
    const auto &rows = ui::rows_for_tab(ui::SettingsTab::interface);
    check(!rows.empty() && rows[0].id == ui::SettingId::ui_style,
          "the style row must lead the Interface page");
    check(rows[0].label == "Interface style" && rows[0].kind == ui::SettingKind::choice &&
              !rows[0].requires_restart,
          "it is a plainly named choice that applies at once");
    const std::string text{rows[0].description};
    check(text.find("Remastered") != std::string::npos && text.find("Classic") != std::string::npos &&
              text.find("Modern") != std::string::npos && text.find("Match world") != std::string::npos &&
              text.find("crafting") != std::string::npos,
          "its description must name the four choices and say crafting comes with it");
    // One row: no separate crafting style anywhere, so no pairing can be mixed.
    for (const ui::SettingsTab tab : ui::all_tabs())
        for (const ui::SettingRow &row : ui::rows_for_tab(tab))
            check(row.label.find("Crafting style") == std::string_view::npos &&
                      (row.id == ui::SettingId::ui_style || row.label != "Interface style"),
                  "there must be exactly one style row and no crafting style row");
    // Looks only: stepping the row may change that one field and no other, in
    // particular not the generation, the mode or any gameplay extra.
    Settings settings = everything_changed();
    const std::array<std::string_view, 4> ring{"REMASTERED", "CLASSIC", "MODERN", "MATCH WORLD"};
    ui::reset_setting(settings, ui::SettingId::ui_style);
    check(value(settings, ui::SettingId::ui_style) == "REMASTERED" &&
              settings.ui_style == UiStyle::remastered,
          "reset returns the style to Remastered, the default");
    for (int press = 1; press <= 8; ++press)
    {
        Settings before = settings;
        ui::adjust_setting(settings, ui::SettingId::ui_style, 1);
        check(value(settings, ui::SettingId::ui_style) == ring[static_cast<std::size_t>(press % 4)],
              "the style ring is REMASTERED, CLASSIC, MODERN, MATCH WORLD");
        before.ui_style = settings.ui_style;
        check(first_difference(before, settings).empty(),
              "the style row changed another setting: " + first_difference(before, settings));
    }
    ui::adjust_setting(settings, ui::SettingId::ui_style, -1);
    check(value(settings, ui::SettingId::ui_style) == "MATCH WORLD" &&
              settings.ui_style == UiStyle::automatic,
          "the ring steps downward and wraps; Match world is listed last");
}

struct TestCase final
{
    const char *name;
    void (*body)();
};

} // namespace

// ---------------------------------------------------------------------------
// Retail parity gate (retail_parity.hpp): retail is the default, everything
// the original does not have is opt-in and never in effect in a session.

void enhancements_default_off_and_persist_only_as_a_choice()
{
    const Settings defaults;
    check(all_enhancements().size() == enhancement_count && enhancement_count >= 2U,
          "the enhancement table must list every Enhancement");
    for (const EnhancementInfo &info : all_enhancements())
    {
        const std::string name{info.label};
        check(&enhancement_info(info.id) == &info, "the table must be indexed by id: " + name);
        check(!enhancement_enabled(defaults, info.id),
              "every non-retail feature must be OFF on a fresh install: " + name);
        check(!enhancement_active(defaults, info.id, ParitySession::offline()),
              "a fresh install must play exactly as the original, even offline: " + name);
        check(info.key.starts_with("enhancement_") && !info.label.empty() &&
                  info.description.size() >= 20U && info.description.back() == '.',
              "each needs a settings key, a name and a one-line description: " + name);
    }
    check(!any_enhancement_enabled(defaults), "no enhancement may be on by default");

    // A settings file written before these existed, or one with the keys
    // deleted, must load as OFF rather than inherit any other default.
    const TemporaryFile legacy{"enhancements-legacy"};
    legacy.write(legacy_profile);
    const Settings old_profile = legacy.load();
    check(!any_enhancement_enabled(old_profile),
          "a settings file without enhancement keys must load with all of them off");
    // The same old file carries `crafting_menu_modern`, a switch that no
    // longer does anything (crafting follows the world's generation). It must
    // load without complaint and not be written back.
    check(std::string{legacy_profile}.find("crafting_menu_modern=1") != std::string::npos,
          "fixture: the legacy profile still has the retired crafting switch");
    legacy.save(old_profile);
    check(legacy.read().find("crafting_menu_modern") == std::string::npos,
          "a retired setting must not survive a save");
    legacy.write("enhancement_lagoon_water=maybe\nenhancement_sinking_lava=\n");
    check(!any_enhancement_enabled(legacy.load()),
          "a damaged enhancement value must fall back to off, never on");

    // The choice survives a restart, one enhancement at a time.
    for (const EnhancementInfo &info : all_enhancements())
    {
        const TemporaryFile file{"enhancement-" + std::string{info.key}};
        Settings chosen;
        set_enhancement_enabled(chosen, info.id, true);
        file.save(chosen);
        check(file.read().find(std::string{info.key} + "=1") != std::string::npos,
              "the choice must be written under its own key: " + std::string{info.key});
        const Settings loaded = file.load();
        for (const EnhancementInfo &other : all_enhancements())
            check(enhancement_enabled(loaded, other.id) == (other.id == info.id),
                  "exactly the chosen enhancement must come back on: " + std::string{info.key});
    }
}

void enhancement_gate_truth_table()
{
    Settings on;
    for (const EnhancementInfo &info : all_enhancements())
        set_enhancement_enabled(on, info.id, true);
    const Settings off;

    const ParitySession offline = ParitySession::offline();
    // Hosting with nobody else connected: all_peers_extension_capable() is
    // trivially true for an empty session.
    const ParitySession hosting_alone = ParitySession::hosting(true);
    const ParitySession all_openminerz = ParitySession::hosting(true);
    const ParitySession stock_peer_present = ParitySession::hosting(false);
    const ParitySession guest_of_stock_host = ParitySession::joined(false);
    const ParitySession guest_of_openminerz_host = ParitySession::joined(true);

    check(!offline.online() && hosting_alone.online() && guest_of_stock_host.online(),
          "hosting and joining are online; nothing else is");

    for (const EnhancementInfo &info : all_enhancements())
    {
        const std::string name{info.label};
        // What ships: enhancements are OFFLINE-ONLY, because no private
        // message agrees an enhancement set between peers.
        check(enhancement_active(on, info.id, offline), "offline, an enabled one is on: " + name);
        check(!enhancement_active(on, info.id, hosting_alone),
              "hosting alone is a session a stock player can join: off: " + name);
        check(!enhancement_active(on, info.id, all_openminerz),
              "all-OpenMinerZ is still off until the set is negotiated: " + name);
        check(!enhancement_active(on, info.id, stock_peer_present),
              "a stock peer in the session: off: " + name);
        check(!enhancement_active(on, info.id, guest_of_stock_host),
              "as the guest of a stock host: off: " + name);
        check(!enhancement_active(on, info.id, guest_of_openminerz_host),
              "as a guest, the other guests are unknown: off: " + name);
        // The stored choice never overrides the session, and a switched-off
        // enhancement is off everywhere.
        for (const ParitySession session : {offline, hosting_alone, all_openminerz,
                                            stock_peer_present, guest_of_stock_host,
                                            guest_of_openminerz_host})
            check(!enhancement_active(off, info.id, session),
                  "a feature the player did not enable is never on: " + name);
    }

    // A stock player joining mid-session: the same host flips from capable to
    // not capable. Whatever the gate said before, it says NO afterwards, and
    // it is asked again every tick so nothing has to remember to switch off.
    ParitySession session = ParitySession::hosting(true);
    const bool before = enhancement_active(on, Enhancement::lagoon_water, session);
    session = ParitySession::hosting(false);
    check(!enhancement_active(on, Enhancement::lagoon_water, session) && !before,
          "a stock peer joining mid-session must leave every enhancement off");

    check(!enhancement_sets_are_negotiated,
          "this test documents the offline-only policy; update it with the negotiation");
    check(enhancement_blocked_reason(offline).empty() &&
              !enhancement_blocked_reason(hosting_alone).empty() &&
              !enhancement_blocked_reason(guest_of_stock_host).empty(),
          "the settings page must be able to say why an enabled feature is off");

    // The capability rule that takes over once peers can agree a set. It is
    // pinned now so that flipping enhancement_sets_are_negotiated cannot
    // quietly admit a stock peer.
    const bool negotiated = true;
    check(session_permits_enhancements(offline, negotiated), "negotiated: offline permits");
    check(session_permits_enhancements(hosting_alone, negotiated),
          "negotiated: hosting alone permits (an empty session is all-capable)");
    check(session_permits_enhancements(all_openminerz, negotiated),
          "negotiated: every peer confirmed OpenMinerZ permits");
    check(!session_permits_enhancements(stock_peer_present, negotiated),
          "negotiated: one unconfirmed peer still refuses");
    check(!session_permits_enhancements(guest_of_stock_host, negotiated),
          "negotiated: a stock host still refuses");
    check(session_permits_enhancements(ParitySession::hosting(true), negotiated) &&
              !session_permits_enhancements(ParitySession::hosting(false), negotiated),
          "negotiated: a stock peer joining mid-session switches enhancements off");
    for (const ParitySession any : {hosting_alone, stock_peer_present, guest_of_stock_host,
                                    guest_of_openminerz_host})
        check(!session_permits_enhancements(any, false),
              "without negotiation no online session permits an enhancement");
}

void enhancements_have_one_clearly_labelled_page()
{
    const auto &rows = ui::rows_for_tab(ui::SettingsTab::enhancements);
    check(rows.size() == enhancement_count, "one row per enhancement, no more");
    check(ui::tab_title(ui::SettingsTab::enhancements) == "NOT IN THE ORIGINAL GAME",
          "the page must be headed for what it is");
    const std::string summary{ui::tab_summary(ui::SettingsTab::enhancements)};
    check(summary.find("Off by default") != std::string::npos &&
              summary.find("single-player") != std::string::npos,
          "the page must say these are off by default and offline only");
    Settings settings;
    for (std::size_t index = 0U; index < rows.size(); ++index)
    {
        const EnhancementInfo &info = all_enhancements()[index];
        const ui::SettingRow &row = rows[index];
        check(ui::setting_enhancement(row.id) == info.id && row.label == info.label &&
                  row.description == info.description && row.kind == ui::SettingKind::toggle,
              "each row must be the register's own name and description");
        check(value(settings, row.id) == "OFF", "an enhancement row starts OFF");
        ui::adjust_setting(settings, row.id, 1);
        check(enhancement_enabled(settings, info.id) && value(settings, row.id) == "ON",
              "the row must switch its own enhancement on");
        ui::reset_setting(settings, row.id);
        check(!enhancement_enabled(settings, info.id), "resetting a row must switch it off");
        ui::adjust_setting(settings, row.id, -1);
    }
    check(any_enhancement_enabled(settings), "fixture: every enhancement is on");
    ui::reset_tab(settings, ui::SettingsTab::enhancements);
    check(!any_enhancement_enabled(settings), "resetting the page restores retail");
    // No ordinary row is an enhancement, and no other page lists one.
    for (const ui::SettingsTab tab : ui::all_tabs())
        if (tab != ui::SettingsTab::enhancements)
            for (const ui::SettingRow &row : ui::rows_for_tab(tab))
                check(!ui::setting_enhancement(row.id).has_value(),
                      "non-retail gameplay must live on its own page: " + std::string{row.label});
}

int main()
{
    const std::vector<TestCase> tests{
        {"enhancements_default_off_and_persist_only_as_a_choice",
         enhancements_default_off_and_persist_only_as_a_choice},
        {"enhancement_gate_truth_table", enhancement_gate_truth_table},
        {"enhancements_have_one_clearly_labelled_page",
         enhancements_have_one_clearly_labelled_page},
        {"defaults_are_the_high_preset_and_survive_normalize",
         defaults_are_the_high_preset_and_survive_normalize},
        {"presets_set_detect_and_fall_back_to_custom", presets_set_detect_and_fall_back_to_custom},
        {"every_row_is_described_in_plain_language", every_row_is_described_in_plain_language},
        {"every_row_changes_its_value_in_both_directions",
         every_row_changes_its_value_in_both_directions},
        {"unavailable_options_say_why_and_keep_their_value",
         unavailable_options_say_why_and_keep_their_value},
        {"frame_rate_limit_covers_fixed_display_and_unlimited",
         frame_rate_limit_covers_fixed_display_and_unlimited},
        {"shadows_row_is_one_ring_over_two_fields", shadows_row_is_one_ring_over_two_fields},
        {"resolution_steps_through_the_monitors_own_modes",
         resolution_steps_through_the_monitors_own_modes},
        {"reset_restores_one_page_and_only_that_page", reset_restores_one_page_and_only_that_page},
        {"legacy_file_loads_and_keeps_the_players_picture",
         legacy_file_loads_and_keeps_the_players_picture},
        {"damaged_files_are_clamped_defaulted_or_ignored",
         damaged_files_are_clamped_defaulted_or_ignored},
        {"settings_file_round_trips_every_field", settings_file_round_trips_every_field},
        {"pages_are_ordered_and_navigable", pages_are_ordered_and_navigable},
        {"menu_model_drives_rows_presets_and_inactive_rows",
         menu_model_drives_rows_presets_and_inactive_rows},
        {"interface_style_defaults_to_remastered_and_resolves_to_three_values",
         interface_style_defaults_to_remastered_and_resolves_to_three_values},
        {"interface_style_persists_migrates_and_survives_damage",
         interface_style_persists_migrates_and_survives_damage},
        {"one_interface_style_row_leads_the_page_and_changes_nothing_else",
         one_interface_style_row_leads_the_page_and_changes_nothing_else},
        {"remastered_is_the_default_and_the_retail_styles_stay_exact",
         remastered_is_the_default_and_the_retail_styles_stay_exact},
    };

    std::size_t failures{};
    for (const auto &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
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
