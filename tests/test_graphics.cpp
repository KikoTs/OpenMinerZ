// The Enhanced presentation's graphics options and the arithmetic of the
// post-processing chain. Everything here runs against temporary files; it never
// touches the real settings in %APPDATA% and needs no graphics device.

#include "openminerz/graphics_options.hpp"
#include "openminerz/graphics_settings.hpp"
#include "openminerz/post_math.hpp"
#include "openminerz/settings.hpp"
#include "openminerz/settings_menu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

class TemporaryFile final
{
  public:
    explicit TemporaryFile(std::string_view name)
        : path_(std::filesystem::temp_directory_path() /
                ("openminerz-graphics-" + std::string{name} + ".cfg"))
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
        std::ostringstream text;
        text << input.rdbuf();
        return text.str();
    }
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

// Every field of GraphicsSettings, compared exactly.
std::string first_difference(const GraphicsSettings &a, const GraphicsSettings &b)
{
    if (a.ambient_shading != b.ambient_shading)
        return "ambient_shading";
    if (a.contact_shadows != b.contact_shadows)
        return "contact_shadows";
    if (a.volumetric_quality != b.volumetric_quality)
        return "volumetric_quality";
    if (a.cloud_quality != b.cloud_quality)
        return "cloud_quality";
    if (a.depth_of_field != b.depth_of_field)
        return "depth_of_field";
    if (a.lens_flare != b.lens_flare)
        return "lens_flare";
    if (a.shadow_distance != b.shadow_distance)
        return "shadow_distance";
    if (!near(a.depth_of_field_strength, b.depth_of_field_strength))
        return "depth_of_field_strength";
    if (!near(a.bloom_strength, b.bloom_strength))
        return "bloom_strength";
    if (!near(a.motion_blur, b.motion_blur))
        return "motion_blur";
    if (!near(a.sharpening, b.sharpening))
        return "sharpening";
    if (!near(a.contrast, b.contrast))
        return "contrast";
    if (!near(a.saturation, b.saturation))
        return "saturation";
    if (!near(a.temperature, b.temperature))
        return "temperature";
    if (!near(a.vignette, b.vignette))
        return "vignette";
    if (!near(a.film_grain, b.film_grain))
        return "film_grain";
    if (!near(a.chromatic_aberration, b.chromatic_aberration))
        return "chromatic_aberration";
    if (!near(a.shadow_softness, b.shadow_softness))
        return "shadow_softness";
    if (!near(a.texture_detail, b.texture_detail))
        return "texture_detail";
    if (a.heat_haze != b.heat_haze)
        return "heat_haze";
    if (a.embers != b.embers)
        return "embers";
    if (!near(a.deep_light, b.deep_light))
        return "deep_light";
    if (a.effect_lighting != b.effect_lighting)
        return "effect_lighting";
    if (a.distortion_effects != b.distortion_effects)
        return "distortion_effects";
    if (!near(a.explosion_shake, b.explosion_shake))
        return "explosion_shake";
    if (a.explosion_flash != b.explosion_flash)
        return "explosion_flash";
    return {};
}

// A value for every option that differs from its default and is in range.
GraphicsSettings everything_changed()
{
    GraphicsSettings g;
    g.ambient_shading = EffectQuality::low;
    g.contact_shadows = false;
    g.volumetric_quality = EffectQuality::ultra;
    g.cloud_quality = EffectQuality::off;
    g.depth_of_field = DepthOfFieldMode::always;
    g.lens_flare = false;
    g.shadow_distance = 96;
    g.depth_of_field_strength = 0.85F;
    g.bloom_strength = 1.75F;
    g.motion_blur = 0.6F;
    g.sharpening = 0.75F;
    g.contrast = 1.12F;
    g.saturation = 0.65F;
    g.temperature = -0.4F;
    g.vignette = 0.55F;
    g.film_grain = 0.35F;
    g.chromatic_aberration = 0.25F;
    g.shadow_softness = 0.9F;
    g.texture_detail = -0.5F;
    g.heat_haze = false;
    g.embers = EffectQuality::low;
    g.effect_lighting = false;
    g.distortion_effects = false;
    g.explosion_shake = 0.30F;
    g.explosion_flash = false;
    g.deep_light = 1.4F;
    return g;
}

void defaults_are_in_range_and_are_the_high_preset()
{
    const GraphicsSettings defaults;
    GraphicsSettings normalized = defaults;
    normalized.normalize();
    check(first_difference(defaults, normalized).empty(),
          "factory defaults must already be in range: " + first_difference(defaults, normalized));
    check(graphics_matches_preset(defaults, 2, true, true),
          "the defaults must be the HIGH bundle so a fresh install reads HIGH");
    const Settings settings;
    check(ui::detect_quality_preset(settings) == ui::QualityPreset::high,
          "a fresh install must read HIGH, not CUSTOM");
    // Effects that can make play harder to read, or that players commonly
    // dislike, are never on unasked.
    check(near(defaults.motion_blur, 0.0F) && near(defaults.film_grain, 0.0F) &&
              near(defaults.chromatic_aberration, 0.0F),
          "motion blur, film grain and colour fringing must default to off");
    check(defaults.depth_of_field != DepthOfFieldMode::always,
          "depth of field must not blur ordinary play by default");
    check(near(defaults.contrast, 1.0F) && near(defaults.saturation, 1.0F) &&
              near(defaults.temperature, 0.0F),
          "the colour controls must default to the intended look");
}

void every_option_has_one_row_key_and_plain_description()
{
    std::set<ui::SettingId> ids;
    std::set<std::string_view> keys;
    std::set<std::string_view> labels;
    for (const ui::GraphicsOption &option : ui::graphics_options())
    {
        check(ids.insert(option.id).second, "an option id is listed twice");
        check(keys.insert(option.key).second, "a settings key is listed twice");
        check(labels.insert(option.label).second, "two rows share a name");
        check(ui::graphics_option(option.id) == &option, "lookup by id must find the option");
        check(!option.label.empty() && option.description.size() >= 40U,
              std::string{option.label} + ": needs a real description");
        check(option.description.size() <= 170U,
              std::string{option.label} + ": description must fit two lines");
        check(option.description.back() == '.',
              std::string{option.label} + ": description must be a sentence");
        // Each says what it costs.
        check(option.description.find("cost") != std::string_view::npos,
              std::string{option.label} + ": description must say what it costs");
        // Exactly one row, on the page the table names, after its anchor.
        std::size_t found{};
        for (const ui::SettingsTab tab : ui::all_tabs())
        {
            const auto &rows = ui::rows_for_tab(tab);
            for (std::size_t index = 0U; index < rows.size(); ++index)
            {
                if (rows[index].id != option.id)
                    continue;
                ++found;
                check(tab == option.tab, std::string{option.label} + ": wrong page");
                check(rows[index].label == option.label &&
                          rows[index].description == option.description &&
                          rows[index].kind == option.kind,
                      std::string{option.label} + ": row differs from the table");
                check(index > 0U, std::string{option.label} + ": row needs its anchor");
            }
        }
        check(found == 1U, std::string{option.label} + ": must have exactly one row");
    }
    check(ids.size() >= 19U, "every field of GraphicsSettings needs an option");
}

void every_option_changes_its_value_and_resets()
{
    for (const ui::GraphicsOption &option : ui::graphics_options())
    {
        Settings settings;
        check(ui::setting_unavailable_reason(settings, option.id).empty(),
              std::string{option.label} + ": must be usable on a default install");
        const std::string original = ui::setting_value(settings, option.id);
        check(!original.empty(), std::string{option.label} + ": must show a value");
        // A press in one direction or the other must change what is shown.
        Settings up = settings;
        ui::adjust_setting(up, option.id, 1);
        Settings down = settings;
        ui::adjust_setting(down, option.id, -1);
        check(ui::setting_value(up, option.id) != original ||
                  ui::setting_value(down, option.id) != original,
              std::string{option.label} + ": a press must change the value");
        // ... and must reach the renderer, not only the row.
        const PostEffects before = resolve_post_effects(settings);
        const Settings &moved = ui::setting_value(up, option.id) != original ? up : down;
        const PostEffects after = resolve_post_effects(moved);
        const bool reaches_renderer =
            before.ao_slices != after.ao_slices || before.ao_steps != after.ao_steps ||
            before.contact_shadows != after.contact_shadows ||
            before.volumetric_steps != after.volumetric_steps ||
            before.cloud_quality != after.cloud_quality ||
            before.depth_of_field != after.depth_of_field ||
            !near(before.depth_of_field_strength, after.depth_of_field_strength) ||
            !near(before.bloom_strength, after.bloom_strength) ||
            before.lens_flare != after.lens_flare ||
            !near(before.motion_blur, after.motion_blur) ||
            !near(before.sharpening, after.sharpening) || !near(before.contrast, after.contrast) ||
            !near(before.saturation, after.saturation) ||
            !near(before.temperature, after.temperature) ||
            !near(before.vignette, after.vignette) || !near(before.film_grain, after.film_grain) ||
            !near(before.chromatic_aberration, after.chromatic_aberration) ||
            !near(before.shadow_softness, after.shadow_softness) ||
            !near(before.texture_detail, after.texture_detail) ||
            before.heat_haze != after.heat_haze || before.ember_count != after.ember_count ||
            !near(before.deep_light, after.deep_light) ||
            // (The effects layer, openminerz/vfx.hpp.)
            before.effect_lights != after.effect_lights ||
            before.effect_distortion != after.effect_distortion ||
            !near(before.effect_shake, after.effect_shake) ||
            !near(before.effect_flash, after.effect_flash) ||
            // (Far terrain is resolved on its own, for its own system.)
            !(resolve_far_terrain(settings) == resolve_far_terrain(moved)) ||
            moved.graphics.shadow_distance != settings.graphics.shadow_distance;
        check(reaches_renderer, std::string{option.label} + ": the option must do something");

        // A slider reports its position and stops at both ends.
        if (option.kind == ui::SettingKind::slider)
        {
            Settings walked = settings;
            for (int press = 0; press < 200; ++press)
                ui::adjust_setting(walked, option.id, 1);
            check(near(ui::setting_normalized_value(walked, option.id), 1.0F, 0.002F),
                  std::string{option.label} + ": slider must reach its maximum");
            for (int press = 0; press < 200; ++press)
                ui::adjust_setting(walked, option.id, -1);
            check(near(ui::setting_normalized_value(walked, option.id), 0.0F, 0.002F),
                  std::string{option.label} + ": slider must reach its minimum");
        }
        else
        {
            // A choice is a ring: enough presses come back to the start.
            Settings walked = settings;
            bool returned{};
            for (int press = 0; press < 8 && !returned; ++press)
            {
                ui::adjust_setting(walked, option.id, 1);
                returned = ui::setting_value(walked, option.id) == original;
            }
            check(returned, std::string{option.label} + ": choice must cycle");
        }

        Settings changed;
        changed.graphics = everything_changed();
        ui::reset_setting(changed, option.id);
        check(ui::setting_value(changed, option.id) == original,
              std::string{option.label} + ": reset must restore the default");
    }
    // Resetting both pages restores every graphics option.
    Settings changed;
    changed.graphics = everything_changed();
    ui::reset_tab(changed, ui::SettingsTab::graphics);
    ui::reset_tab(changed, ui::SettingsTab::effects);
    check(first_difference(changed.graphics, GraphicsSettings{}).empty(),
          "page reset must restore: " + first_difference(changed.graphics, GraphicsSettings{}));
}

void options_survive_a_round_trip_exactly()
{
    const TemporaryFile file{"round-trip"};
    const SettingsStore store{file.path()};
    Settings written;
    written.graphics = everything_changed();
    written.graphics.normalize();
    store.save(written);
    const Settings loaded = store.load();
    check(first_difference(written.graphics, loaded.graphics).empty(),
          "round trip changed: " + first_difference(written.graphics, loaded.graphics));
    // Saving what was loaded writes the same bytes: nothing drifts.
    const std::string first = file.read();
    store.save(loaded);
    check(first == file.read(), "a second save must be byte-identical");
    for (const ui::GraphicsOption &option : ui::graphics_options())
        check(first.find(std::string{option.key} + "=") != std::string::npos,
              std::string{option.key} + ": every option must be written");
}

void older_and_damaged_files_load_safely()
{
    // A file from before these options existed: every one takes its default,
    // so the player gets the intended look rather than a stripped one.
    {
        const TemporaryFile file{"older"};
        file.write("# OpenMinerZ settings v1\npresentation=rebirth\nshadow_quality=high\n"
                   "bloom=1\nsun_shafts=1\nview_distance=16\n");
        const Settings loaded = SettingsStore{file.path()}.load();
        check(first_difference(loaded.graphics, GraphicsSettings{}).empty(),
              "an older file must load the defaults: " +
                  first_difference(loaded.graphics, GraphicsSettings{}));
        check(ui::detect_quality_preset(loaded) == ui::QualityPreset::high,
              "an older HIGH profile must still read HIGH");
    }
    // A player who had chosen a lower level keeps it: the new passes start at
    // the bundle of their old Shadows level instead of the High defaults, and
    // the shadow distance stays what that level always drew.
    {
        const TemporaryFile file{"older-low"};
        file.write("presentation=rebirth\nview_distance=8\nshadows_enabled=0\nshadow_quality=low\n"
                   "anti_aliasing=off\ntexture_filtering=1\nparticle_quality=low\nbloom=0\n"
                   "sun_shafts=0\n");
        const Settings loaded = SettingsStore{file.path()}.load();
        check(ui::detect_quality_preset(loaded) == ui::QualityPreset::low,
              "an older LOW profile must still read LOW");
        const PostEffects effects = resolve_post_effects(loaded);
        check(effects.ao_slices == 0 && effects.volumetric_steps == 0 && !effects.contact_shadows &&
                  effects.depth_of_field == DepthOfFieldMode::off && !effects.lens_flare,
              "an older LOW profile must not be handed the High passes");
    }
    {
        const TemporaryFile file{"older-medium"};
        file.write("presentation=rebirth\nshadows_enabled=1\nshadow_quality=medium\n");
        const Settings loaded = SettingsStore{file.path()}.load();
        check(loaded.graphics.shadow_distance == 88 &&
                  loaded.graphics.ambient_shading == EffectQuality::medium,
              "an older MEDIUM shadow level must keep its reach and take the medium bundle");
        // Once the file carries the new keys, they are the player's choice.
        const TemporaryFile saved{"older-medium-saved"};
        Settings changed = loaded;
        changed.graphics.ambient_shading = EffectQuality::ultra;
        changed.graphics.shadow_distance = 120;
        SettingsStore{saved.path()}.save(changed);
        const Settings again = SettingsStore{saved.path()}.load();
        check(again.graphics.ambient_shading == EffectQuality::ultra &&
                  again.graphics.shadow_distance == 120,
              "a saved choice must not be migrated again");
    }
    // A file from after the first graphics rows but before the deep-underground
    // ones: a profile that added up to LOW must still read LOW (and not be
    // handed the High embers), and one that read HIGH still reads HIGH.
    for (const ui::QualityPreset preset : {ui::QualityPreset::low, ui::QualityPreset::medium,
                                           ui::QualityPreset::high, ui::QualityPreset::ultra})
    {
        const TemporaryFile file{"before-deep"};
        Settings written;
        ui::apply_quality_preset(written, preset);
        SettingsStore{file.path()}.save(written);
        std::istringstream lines{file.read()};
        std::string kept;
        for (std::string line; std::getline(lines, line);)
            if (!line.starts_with("heat_haze=") && !line.starts_with("embers=") &&
                !line.starts_with("deep_light="))
                kept += line + "\n";
        file.write(kept);
        const Settings loaded = SettingsStore{file.path()}.load();
        check(ui::detect_quality_preset(loaded) == preset,
              "a profile saved before the deep-underground rows must keep its level");
        check(near(loaded.graphics.deep_light, GraphicsSettings{}.deep_light),
              "a missing look option takes its default");
    }
    // Out-of-range, non-numeric and unknown values never leave the range.
    {
        const TemporaryFile file{"damaged"};
        file.write("ambient_shading=extreme\ncontact_shadows=maybe\nvolumetric_quality=off\n"
                   "cloud_quality=9\ndepth_of_field=sometimes\nshadow_distance=100000\n"
                   "depth_of_field_strength=nan\nbloom_strength=99\nmotion_blur=-4\n"
                   "sharpening=inf\ncontrast=0\nsaturation=7\ntemperature=-9\nvignette=2\n"
                   "film_grain=abc\nchromatic_aberration=1e30\nshadow_softness=-1\n"
                   "texture_detail=5\n");
        const Settings loaded = SettingsStore{file.path()}.load();
        const GraphicsSettings &g = loaded.graphics;
        const GraphicsSettings defaults;
        check(g.ambient_shading == defaults.ambient_shading &&
                  g.contact_shadows == defaults.contact_shadows &&
                  g.cloud_quality == defaults.cloud_quality &&
                  g.depth_of_field == defaults.depth_of_field,
              "an unknown name must keep the default");
        check(g.volumetric_quality == EffectQuality::low,
              "the shaft quality has its own switch and can never be off");
        check(g.shadow_distance == 150, "shadow distance must clamp to the renderer's limit");
        check(near(g.depth_of_field_strength, defaults.depth_of_field_strength) &&
                  near(g.sharpening, defaults.sharpening) &&
                  near(g.film_grain, defaults.film_grain),
              "a non-number must keep the default");
        check(near(g.bloom_strength, 2.0F) && near(g.motion_blur, 0.0F) &&
                  near(g.contrast, 0.80F) && near(g.saturation, 1.5F) &&
                  near(g.temperature, -1.0F) && near(g.vignette, 1.0F) &&
                  near(g.chromatic_aberration, 1.0F) && near(g.shadow_softness, 0.0F) &&
                  near(g.texture_detail, 1.0F),
              "out-of-range numbers must clamp");
        GraphicsSettings again = g;
        again.normalize();
        check(first_difference(g, again).empty(), "normalize must be idempotent");
    }
}

void presets_are_coherent_and_fall_back_to_custom()
{
    constexpr std::array presets{ui::QualityPreset::low, ui::QualityPreset::medium,
                                 ui::QualityPreset::high, ui::QualityPreset::ultra};
    std::vector<PostEffects> resolved;
    for (const ui::QualityPreset preset : presets)
    {
        Settings settings;
        settings.graphics = everything_changed();
        const GraphicsSettings before = settings.graphics;
        ui::apply_quality_preset(settings, preset);
        check(ui::detect_quality_preset(settings) == preset,
              "applying a preset must be detected as that preset");
        // Taste is not the preset's business.
        const GraphicsSettings &g = settings.graphics;
        check(near(g.depth_of_field_strength, before.depth_of_field_strength) &&
                  near(g.bloom_strength, before.bloom_strength) &&
                  near(g.motion_blur, before.motion_blur) &&
                  near(g.sharpening, before.sharpening) && near(g.contrast, before.contrast) &&
                  near(g.saturation, before.saturation) &&
                  near(g.temperature, before.temperature) && near(g.vignette, before.vignette) &&
                  near(g.film_grain, before.film_grain) &&
                  near(g.chromatic_aberration, before.chromatic_aberration) &&
                  near(g.shadow_softness, before.shadow_softness) &&
                  near(g.texture_detail, before.texture_detail),
              "a preset must leave the look options alone");
        // Changing any preset-controlled option that can act is Custom, and
        // putting it back restores the preset.
        for (const ui::GraphicsOption &option : ui::graphics_options())
        {
            if (!option.preset_controlled ||
                !ui::setting_unavailable_reason(settings, option.id).empty())
                continue;
            Settings changed = settings;
            ui::adjust_setting(changed, option.id, 1);
            if (ui::setting_value(changed, option.id) == ui::setting_value(settings, option.id))
            {
                ui::adjust_setting(changed, option.id, -1);
                check(ui::setting_value(changed, option.id) !=
                          ui::setting_value(settings, option.id),
                      std::string{option.label} + ": must move in one direction");
                check(ui::detect_quality_preset(changed) == ui::QualityPreset::custom,
                      std::string{option.label} + ": a change must read CUSTOM");
                ui::adjust_setting(changed, option.id, 1);
            }
            else
            {
                check(ui::detect_quality_preset(changed) == ui::QualityPreset::custom,
                      std::string{option.label} + ": a change must read CUSTOM");
                ui::adjust_setting(changed, option.id, -1);
            }
            check(ui::detect_quality_preset(changed) == preset,
                  std::string{option.label} + ": undoing the change must restore the preset");
        }
        resolved.push_back(resolve_post_effects(settings));
    }
    // Each step up asks the renderer for at least as much work as the last.
    for (std::size_t index = 1U; index < resolved.size(); ++index)
    {
        const PostEffects &lower = resolved[index - 1U];
        const PostEffects &higher = resolved[index];
        check(higher.ao_slices * higher.ao_steps >= lower.ao_slices * lower.ao_steps &&
                  higher.volumetric_steps >= lower.volumetric_steps &&
                  higher.cloud_quality >= lower.cloud_quality &&
                  static_cast<int>(higher.contact_shadows) >=
                      static_cast<int>(lower.contact_shadows) &&
                  higher.ember_count >= lower.ember_count &&
                  higher.deep_air_steps >= lower.deep_air_steps &&
                  static_cast<int>(higher.heat_haze) >= static_cast<int>(lower.heat_haze),
              "a higher preset must not ask for less");
    }
    // LOW runs none of the optional screen-space passes.
    const PostEffects &low = resolved.front();
    check(low.enabled && low.ao_slices == 0 && !low.contact_shadows &&
              low.volumetric_steps == 0 && low.depth_of_field == DepthOfFieldMode::off &&
              !low.bloom && !low.fxaa && !low.lens_flare && !low.heat_haze &&
              low.ember_count == 0,
          "LOW must skip every optional pass");
    // ... but Hell is lit at every level: its light is what makes it visible
    // at all, never an extra. Only how finely the glow is traced changes.
    for (const PostEffects &effects : resolved)
        check(effects.deep_light > 0.0F && effects.deep_air_steps >= 4,
              "every preset must light the deep underground");
    check(resolved[2].heat_haze && resolved[2].ember_count > 0 &&
              resolved[3].ember_count > resolved[2].ember_count,
          "HIGH shimmers and sheds embers; ULTRA sheds more");
    // With shadows off the unused shadow rows must not turn LOW into CUSTOM.
    Settings low_settings;
    ui::apply_quality_preset(low_settings, ui::QualityPreset::low);
    low_settings.graphics.shadow_distance = 150;
    low_settings.graphics.contact_shadows = true;
    low_settings.graphics.volumetric_quality = EffectQuality::ultra;
    check(ui::detect_quality_preset(low_settings) == ui::QualityPreset::low,
          "options that cannot act must not turn LOW into CUSTOM");
}

void the_original_presentation_runs_none_of_it()
{
    Settings settings;
    settings.presentation = PresentationProfile::classic;
    settings.graphics = everything_changed();
    const PostEffects effects = resolve_post_effects(settings);
    check(!effects.enabled && !effects.bloom && effects.ao_slices == 0 &&
              !effects.contact_shadows && effects.volumetric_steps == 0 &&
              effects.cloud_quality == 0 && effects.depth_of_field == DepthOfFieldMode::off &&
              near(effects.motion_blur, 0.0F) && !effects.fxaa &&
              near(effects.sharpening, 0.0F) && !effects.lens_flare &&
              near(effects.vignette, 0.0F) && near(effects.film_grain, 0.0F) &&
              near(effects.chromatic_aberration, 0.0F),
          "the Original presentation must keep the 2011 picture: no new pass may run");
    // Nor any of the deep underground: the Original keeps its fade to black.
    check(near(effects.deep_light, 0.0F) && effects.deep_air_steps == 0 && !effects.heat_haze &&
              effects.ember_count == 0,
          "the Original presentation must keep Hell exactly as the 2011 game drew it");
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::heat_haze).empty() &&
              !ui::setting_unavailable_reason(settings, ui::SettingId::embers).empty() &&
              !ui::setting_unavailable_reason(settings, ui::SettingId::deep_light).empty(),
          "the deep-underground rows must explain themselves in the Original presentation");
    // Rows that only the Enhanced presentation draws say so instead of
    // changing nothing on screen.
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::ambient_shading).empty() &&
              !ui::setting_unavailable_reason(settings, ui::SettingId::depth_of_field).empty(),
          "Enhanced-only rows must explain themselves in the Original presentation");
    const std::string stored = ui::setting_value(settings, ui::SettingId::depth_of_field);
    ui::adjust_setting(settings, ui::SettingId::depth_of_field, 1);
    check(ui::setting_value(settings, ui::SettingId::depth_of_field) == stored,
          "an unavailable row keeps its stored value");
}

// Far terrain (openminerz/world/far_terrain.hpp): the two rows, what they ask
// the system for, and that the Original presentation never draws any of it.
void far_terrain_rows_resolve_and_stay_out_of_the_original()
{
    // A fresh install draws four kilometres at High detail.
    Settings settings;
    check(settings.presentation == PresentationProfile::rebirth ||
              resolve_far_terrain(settings).distance == 0,
          "the default presentation decides whether far terrain is drawn at all");
    settings.presentation = PresentationProfile::rebirth;
    FarTerrainChoice choice = resolve_far_terrain(settings);
    check(choice.distance == 4000 && choice.base_cell == 1 && choice.ring_cells == 256,
          "the default is four kilometres at High detail");
    check(ui::setting_value(settings, ui::SettingId::far_terrain) == "4 KM" &&
              ui::setting_value(settings, ui::SettingId::far_terrain_detail) == "HIGH",
          "the rows show a distance and a detail level");

    // Every distance the row offers, in order, and Off.
    const std::array<std::pair<FarTerrainDistance, int>, 7> distances{
        {{FarTerrainDistance::off, 0},
         {FarTerrainDistance::m500, 500},
         {FarTerrainDistance::km1, 1000},
         {FarTerrainDistance::km2, 2000},
         {FarTerrainDistance::km4, 4000},
         {FarTerrainDistance::km8, 8000},
         {FarTerrainDistance::km12, 12000}}};
    for (const auto &[level, blocks] : distances)
    {
        settings.graphics.far_terrain = level;
        check(resolve_far_terrain(settings).distance == blocks &&
                  far_terrain_blocks(level) == blocks,
              "each far-terrain choice is its distance");
    }
    settings.graphics.far_terrain = FarTerrainDistance::off;
    check(ui::setting_value(settings, ui::SettingId::far_terrain) == "OFF",
          "Off is spelled out on the row");
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::far_terrain_detail).empty(),
          "the detail row says why it does nothing while far terrain is off");
    check(ui::setting_unavailable_reason(settings, ui::SettingId::far_terrain).empty(),
          "the distance row itself always works in the Enhanced presentation");
    // The row steps through all seven and wraps, in both directions.
    std::vector<std::string> seen;
    for (int press = 0; press < 7; ++press)
    {
        seen.push_back(ui::setting_value(settings, ui::SettingId::far_terrain));
        ui::adjust_setting(settings, ui::SettingId::far_terrain, 1);
    }
    check(seen == std::vector<std::string>{"OFF", "500 M", "1 KM", "2 KM", "4 KM", "8 KM",
                                           "12 KM"} &&
              ui::setting_value(settings, ui::SettingId::far_terrain) == "OFF",
          "the distance row offers Off, 500 m, 1, 2, 4, 8 and 12 km");
    ui::adjust_setting(settings, ui::SettingId::far_terrain, -1);
    check(settings.graphics.far_terrain == FarTerrainDistance::km12,
          "stepping back from Off wraps to the furthest distance");
    ui::reset_setting(settings, ui::SettingId::far_terrain);
    check(settings.graphics.far_terrain == FarTerrainDistance::km4,
          "resetting the row returns to four kilometres");

    // Every choice survives the settings file, and a file from when the row
    // had five levels (spelled as quality names) keeps what it meant.
    for (const auto &[level, blocks] : distances)
    {
        Settings stored;
        stored.graphics.far_terrain = level;
        std::ostringstream text;
        ui::save_graphics_options(stored, text);
        std::unordered_map<std::string, std::string> values;
        std::istringstream lines{text.str()};
        std::string line;
        while (std::getline(lines, line))
            if (const std::size_t equals = line.find('='); equals != std::string::npos)
                values[line.substr(0U, equals)] = line.substr(equals + 1U);
        Settings loaded;
        loaded.graphics.far_terrain =
            level == FarTerrainDistance::off ? FarTerrainDistance::km4 : FarTerrainDistance::off;
        ui::load_graphics_options(loaded, values);
        check(loaded.graphics.far_terrain == level && far_terrain_blocks(level) == blocks,
              "a far-terrain distance is saved and loaded as itself");
    }
    const std::array<std::pair<std::string_view, FarTerrainDistance>, 5> legacy{
        {{"off", FarTerrainDistance::off},
         {"low", FarTerrainDistance::km2},
         {"medium", FarTerrainDistance::km4},
         {"high", FarTerrainDistance::km8},
         {"ultra", FarTerrainDistance::km12}}};
    for (const auto &[name, distance] : legacy)
    {
        Settings stored;
        std::ostringstream text;
        ui::save_graphics_options(stored, text);
        std::unordered_map<std::string, std::string> values;
        std::istringstream lines{text.str()};
        std::string line;
        while (std::getline(lines, line))
            if (const std::size_t equals = line.find('='); equals != std::string::npos)
                values[line.substr(0U, equals)] = line.substr(equals + 1U);
        values["far_terrain"] = std::string{name};
        Settings loaded;
        loaded.graphics.far_terrain = FarTerrainDistance::m500;
        ui::load_graphics_options(loaded, values);
        check(loaded.graphics.far_terrain == distance,
              "a five-level settings file keeps the distance it chose");
    }
    {
        Settings damaged;
        damaged.graphics.far_terrain = static_cast<FarTerrainDistance>(99);
        damaged.graphics.normalize();
        check(damaged.graphics.far_terrain == FarTerrainDistance::km4,
              "a distance outside the seven falls back to the default");
    }

    // Detail: coarser cells or a shorter reach per level, never Off.
    settings.graphics.far_terrain = FarTerrainDistance::km4;
    int previous_work{};
    for (const EffectQuality detail : {EffectQuality::low, EffectQuality::medium,
                                       EffectQuality::high, EffectQuality::ultra})
    {
        settings.graphics.far_terrain_detail = detail;
        choice = resolve_far_terrain(settings);
        check(choice.ring_cells >= 192 && (choice.base_cell == 1 || choice.base_cell == 2),
              "every detail level keeps the reach its geomorph needs");
        // Cells drawn per level, relative: reach squared over cell squared.
        const int work = choice.ring_cells * choice.ring_cells / (choice.base_cell * choice.base_cell);
        check(work > previous_work, "a higher detail level draws more");
        previous_work = work;
    }
    settings.graphics.far_terrain_detail = EffectQuality::off;
    settings.graphics.normalize();
    check(settings.graphics.far_terrain_detail != EffectQuality::off,
          "the detail level is never Off: the distance row is the switch");

    // The presets: off on Low, then further and finer.
    int previous_distance = -1;
    for (int level = 0; level < 4; ++level)
    {
        Settings preset;
        preset.presentation = PresentationProfile::rebirth;
        apply_graphics_preset(preset.graphics, level);
        const FarTerrainChoice resolved = resolve_far_terrain(preset);
        check(resolved.distance > previous_distance, "a higher preset draws further");
        check(level != 0 || resolved.distance == 0, "LOW draws no far terrain");
        check(level != 2 || (resolved == resolve_far_terrain(Settings{}) ||
                             Settings{}.presentation != PresentationProfile::rebirth),
              "HIGH is the default");
        previous_distance = resolved.distance;
    }

    // The Original presentation keeps the 2011 draw distance and fog exactly:
    // whatever the rows say, nothing is asked for.
    Settings original;
    original.presentation = PresentationProfile::classic;
    original.graphics.far_terrain = FarTerrainDistance::km12;
    original.graphics.far_terrain_detail = EffectQuality::ultra;
    check(resolve_far_terrain(original).distance == 0,
          "the Original presentation never draws far terrain");
    check(!ui::setting_unavailable_reason(original, ui::SettingId::far_terrain).empty() &&
              !ui::setting_unavailable_reason(original, ui::SettingId::far_terrain_detail).empty(),
          "the far-terrain rows explain themselves in the Original presentation");

    // A settings file from before the rows existed keeps its quality level:
    // a LOW profile does not gain far terrain, a HIGH one gets the default.
    for (int level = 0; level < 4; ++level)
    {
        Settings stored;
        apply_graphics_preset(stored.graphics, level);
        std::ostringstream text;
        ui::save_graphics_options(stored, text);
        std::unordered_map<std::string, std::string> values;
        std::istringstream lines{text.str()};
        std::string line;
        while (std::getline(lines, line))
        {
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = line.substr(0U, equals);
            if (key != "far_terrain" && key != "far_terrain_detail")
                values[key] = line.substr(equals + 1U);
        }
        Settings loaded;
        ui::load_graphics_options(loaded, values);
        check(loaded.graphics.far_terrain == stored.graphics.far_terrain &&
                  (loaded.graphics.far_terrain == FarTerrainDistance::off ||
                   loaded.graphics.far_terrain_detail == stored.graphics.far_terrain_detail),
              "an older settings file keeps the far terrain of its quality level");
    }
}

void dependent_rows_follow_their_switch()
{
    Settings settings;
    settings.shadows_enabled = false;
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::shadow_distance).empty() &&
              !ui::setting_unavailable_reason(settings, ui::SettingId::shadow_softness).empty() &&
              !ui::setting_unavailable_reason(settings, ui::SettingId::contact_shadows).empty(),
          "the shadow rows need Shadows");
    check(!resolve_post_effects(settings).contact_shadows,
          "contact shadows must not be drawn with Shadows off");
    settings = Settings{};
    settings.sun_shafts = false;
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::volumetric_quality).empty(),
          "the shaft quality needs Light shafts");
    check(resolve_post_effects(settings).volumetric_steps == 0,
          "Light shafts off must skip the march");
    settings = Settings{};
    settings.bloom = false;
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::bloom_strength).empty(),
          "bloom strength needs Bloom");
    settings = Settings{};
    settings.graphics.depth_of_field = DepthOfFieldMode::off;
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::depth_of_field_strength)
               .empty(),
          "the strength needs Depth of field");
    // Reduced motion removes the streak, whatever the slider says.
    settings = Settings{};
    settings.graphics.motion_blur = 0.8F;
    check(near(resolve_post_effects(settings).motion_blur, 0.8F), "setup: motion blur on");
    settings.reduced_motion = true;
    check(near(resolve_post_effects(settings).motion_blur, 0.0F),
          "Reduced motion must switch motion blur off");
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::motion_blur).empty(),
          "the motion blur row must say why it is inactive");
    // ... and the shimmer and the drifting sparks of the deep underground,
    // but never its light.
    settings = Settings{};
    const PostEffects moving = resolve_post_effects(settings);
    check(moving.heat_haze && moving.ember_count > 0, "setup: haze and embers on");
    settings.reduced_motion = true;
    const PostEffects still = resolve_post_effects(settings);
    check(!still.heat_haze && still.ember_count == 0,
          "Reduced motion must switch heat haze and embers off");
    check(near(still.deep_light, moving.deep_light) &&
              still.deep_air_steps == moving.deep_air_steps,
          "Reduced motion must leave Hell as visible as before");
    check(!ui::setting_unavailable_reason(settings, ui::SettingId::heat_haze).empty() &&
              !ui::setting_unavailable_reason(settings, ui::SettingId::embers).empty() &&
              ui::setting_unavailable_reason(settings, ui::SettingId::deep_light).empty(),
          "the haze and ember rows must say why they are inactive");
    // The light strength is a matter of the player's screen: it scales, within
    // a range that can neither black Hell out nor wash it out.
    settings = Settings{};
    settings.graphics.deep_light = 0.0F;
    check(near(resolve_post_effects(settings).deep_light, 0.5F),
          "the deep light can be lowered, never switched off");
    settings.graphics.deep_light = 9.0F;
    check(near(resolve_post_effects(settings).deep_light, 2.0F), "the deep light has a ceiling");
}

void circle_of_confusion_is_a_thin_lens()
{
    using namespace post;
    // A longer lens (a narrower view) and a wider aperture both blur more.
    check(focal_length(37.0F) > focal_length(74.0F), "zooming in must lengthen the lens");
    check(near(focal_length(90.0F), 0.100F, 0.0001F),
          "a 90 degree view on 200 mm film is a 100 mm lens");
    check(near(f_number(0.0F), 11.0F, 0.001F) && near(f_number(1.0F), 1.0F, 0.001F),
          "strength must run from f/11 to f/1");
    check(lens_constant(74.0F, 0.8F) > lens_constant(74.0F, 0.2F) &&
              lens_constant(37.0F, 0.5F) > lens_constant(74.0F, 0.5F),
          "aperture and zoom must both deepen the blur");

    const float k = lens_constant(40.0F, 0.5F);
    constexpr float limit = 0.02F;
    check(near(circle_of_confusion(8.0F, 8.0F, k, limit), 0.0F, 1.0e-7F),
          "the focus plane must be perfectly sharp");
    check(circle_of_confusion(2.0F, 8.0F, k, limit) < 0.0F &&
              circle_of_confusion(60.0F, 8.0F, k, limit) > 0.0F,
          "near is negative and far is positive");
    // Smooth and monotonic through the focus plane: no step, no pop.
    float previous = circle_of_confusion(0.5F, 8.0F, k, limit);
    for (float depth = 0.6F; depth < 400.0F; depth *= 1.07F)
    {
        const float coc = circle_of_confusion(depth, 8.0F, k, limit);
        check(coc >= previous - 1.0e-7F, "blur must grow steadily with distance from focus");
        check(std::abs(coc - previous) < 0.012F, "blur must not jump between nearby depths");
        check(std::abs(coc) <= limit + 1.0e-7F, "blur must respect its limit");
        previous = coc;
    }
    // The background blur converges (the lens is focused short of infinity),
    // and focusing far away leaves the whole distance sharp.
    const float horizon = circle_of_confusion(1000.0F, 8.0F, k, 1.0F);
    check(near(horizon, k * (1.0F / 8.0F - 1.0F / 1000.0F), 1.0e-6F),
          "far blur must follow K (1/focus - 1/depth)");
    check(circle_of_confusion(1000.0F, 80.0F, k, 1.0F) < horizon * 0.15F,
          "a distant focus must keep the distance sharp");
    // "Always" mode in ordinary play, default view and strength, looking at
    // something 10 m away: the horizon behind it blurs by well under a quarter
    // of a percent of the picture height (about two pixels at 1080p), and
    // things between 5 m and the horizon stay within that. Subtle by
    // construction; raising a weapon opens the lens and lengthens it.
    const float resting = lens_constant(74.0F, 0.5F) * resting_aperture;
    const float play = circle_of_confusion(1000.0F, 10.0F, resting, 1.0F);
    check(play > 0.0005F && play < 0.0025F, "normal-view blur must be present but subtle");
    check(std::abs(circle_of_confusion(5.0F, 10.0F, resting, 1.0F)) < 0.0025F,
          "the middle distance must stay readable");
    const float aiming = circle_of_confusion(1000.0F, 10.0F, lens_constant(45.0F, 0.5F), 1.0F);
    check(aiming > play * 4.0F, "aiming must deepen the blur clearly");
}

void autofocus_eases_without_overshoot()
{
    using namespace post;
    // Pull focus from 40 m to 2 m at several frame rates: always between the
    // two, always approaching, and frame-rate independent.
    const float start = 1.0F / 40.0F;
    const float target = 1.0F / 2.0F;
    std::array<float, 3> after_half_second{};
    std::size_t slot{};
    for (const float frames_per_second : {30.0F, 144.0F, 600.0F})
    {
        float focus = start;
        const int frames = static_cast<int>(std::lround(frames_per_second * 0.5F));
        for (int frame = 0; frame < frames; ++frame)
        {
            const float next = autofocus_step(focus, target, 1.0F / frames_per_second);
            check(next >= focus - 1.0e-7F && next <= target + 1.0e-7F,
                  "focus must approach its target and never overshoot");
            focus = next;
        }
        after_half_second[slot++] = focus;
    }
    check(near(after_half_second[0], after_half_second[1], 0.004F) &&
              near(after_half_second[1], after_half_second[2], 0.004F),
          "the focus pull must take the same time at any frame rate");
    check(after_half_second[1] > start + (target - start) * 0.9F,
          "half a second must nearly complete a pull toward the viewer");
    // Letting go is slower than pulling in, and a still target stays put.
    const float pull_in = autofocus_step(0.1F, 0.5F, 0.05F) - 0.1F;
    const float let_go = 0.5F - autofocus_step(0.5F, 0.1F, 0.05F);
    check(pull_in > let_go, "focusing nearer must be quicker than focusing away");
    check(near(autofocus_step(0.25F, 0.25F, 0.016F), 0.25F, 1.0e-7F),
          "a focused lens must not drift");
    check(near(autofocus_step(0.25F, 0.5F, 0.0F), 0.25F, 1.0e-7F),
          "a paused frame must not move the focus");
}

void bloom_conserves_energy()
{
    using namespace post;
    check(bloom_level_count(1920, 1080) == 7 && bloom_level_count(1280, 720) == 6 &&
              bloom_level_count(640, 360) == 5 && bloom_level_count(320, 180) == 4,
          "bloom levels must follow the resolution");
    check(bloom_level_count(1, 1) == 1 && bloom_level_count(100000, 100000) == 7,
          "bloom levels must stay within one and the maximum");
    for (const float scatter : {0.0F, 0.35F, 0.65F, 0.9F, 1.0F})
        for (int count = 1; count <= maximum_bloom_levels; ++count)
        {
            float total{};
            for (int level = 0; level < count; ++level)
            {
                const float weight = bloom_level_weight(level, count, scatter);
                check(weight >= 0.0F && weight <= 1.0F, "a bloom weight must be a share");
                total += weight;
            }
            check(near(total, 1.0F, 1.0e-5F),
                  "the bloom levels must sum to one: light is moved, never added");
        }
    check(near(bloom_level_weight(-1, 4, 0.5F), 0.0F) && near(bloom_level_weight(4, 4, 0.5F), 0.0F),
          "levels outside the chain carry nothing");
    // More scatter moves light to the wide levels.
    check(bloom_level_weight(4, 6, 0.8F) > bloom_level_weight(4, 6, 0.4F) &&
              bloom_level_weight(0, 6, 0.8F) < bloom_level_weight(0, 6, 0.4F),
          "scatter must widen the glow");
    // The final mix is a lerp, so it cannot brighten a flat field.
    for (const float strength : {0.0F, 0.25F, 1.0F, 2.0F, 50.0F})
    {
        const float mix = bloom_mix(strength);
        check(mix >= 0.0F && mix <= 0.2F, "the bloom mix must stay a small share");
        constexpr float flat = 0.6F;
        check(near(flat * (1.0F - mix) + flat * mix, flat, 1.0e-6F),
              "a uniform picture must come out of bloom unchanged");
    }
    check(near(bloom_mix(0.0F), 0.0F) && bloom_mix(2.0F) > bloom_mix(1.0F),
          "bloom strength must scale the mix");
    check(near(karis_weight(0.0F), 1.0F) && karis_weight(50.0F) < 0.02F,
          "one very bright texel must not dominate a downsample");
}

void exposure_closes_but_never_opens()
{
    using namespace post;
    check(near(auto_exposure(0.20F), 1.0F), "middle grey needs no correction");
    check(near(auto_exposure(0.0001F), 1.0F) && near(auto_exposure(0.02F), 1.0F),
          "night and caves must never be brightened into day");
    check(near(auto_exposure(0.40F), 0.5F) && near(auto_exposure(50.0F), 0.354F),
          "bright snow closes the exposure, down to 1.5 stops");
    float previous = auto_exposure(0.01F);
    for (float luminance = 0.012F; luminance < 20.0F; luminance *= 1.1F)
    {
        const float exposure = auto_exposure(luminance);
        check(exposure <= previous + 1.0e-6F, "a brighter scene never raises the exposure");
        previous = exposure;
    }
    check(near(exposure_from_stops(0.0F), 1.0F) && near(exposure_from_stops(1.0F), 2.0F) &&
              near(exposure_from_stops(-1.0F), 0.5F) && near(exposure_from_stops(9.0F), 4.0F),
          "the brightness slider is in stops, within two either way");
}

void lens_and_light_helpers_behave()
{
    using namespace post;
    // The vignette never touches the middle of the picture, where the
    // crosshair and whatever it is on are.
    for (const float strength : {0.0F, 0.2F, 1.0F})
    {
        check(near(vignette(0.0F, strength), 1.0F) && near(vignette(0.35F, strength), 1.0F),
              "the centre of the picture must stay at full brightness");
        float previous = 1.0F;
        for (float radius = 0.0F; radius <= 1.0F; radius += 0.02F)
        {
            const float kept = vignette(radius, strength);
            check(kept <= previous + 1.0e-6F && kept >= 0.44F, "vignette must fall off smoothly");
            previous = kept;
        }
    }
    check(near(vignette(1.0F, 0.0F), 1.0F), "0% must be off");
    check(near(vignette(1.0F, 0.2F), 0.89F, 0.001F), "the default darkens corners by 11%");

    // The phase function integrates to one over the sphere, so the fog never
    // creates light, and it favours looking toward the sun.
    for (const float g : {0.0F, 0.4F, 0.7F})
    {
        double total{};
        constexpr int samples = 20000;
        for (int index = 0; index < samples; ++index)
        {
            const float cosine = -1.0F + 2.0F * (static_cast<float>(index) + 0.5F) / samples;
            total += henyey_greenstein(cosine, g) * 2.0 * pi * (2.0 / samples);
        }
        check(std::abs(total - 1.0) < 0.002, "the phase function must be normalised");
    }
    check(henyey_greenstein(1.0F, 0.6F) > henyey_greenstein(0.0F, 0.6F) &&
              henyey_greenstein(0.0F, 0.6F) > henyey_greenstein(-1.0F, 0.6F),
          "fog must glow toward the light");
    check(near(transmittance(0.0F, 100.0F), 1.0F) && near(transmittance(0.01F, 0.0F), 1.0F) &&
              near(transmittance(0.01F, 100.0F), std::exp(-1.0F)),
          "fog must follow Beer-Lambert");
    check(near(transmittance(0.004F, 50.0F) * transmittance(0.004F, 50.0F),
               transmittance(0.004F, 100.0F), 1.0e-6F),
          "fog must compose over distance, whatever the step count");

    // The same streak on screen at any frame rate.
    check(near(motion_blur_scale(1.0F, 1.0F / 60.0F), 1.0F) &&
              near(motion_blur_scale(1.0F, 1.0F / 240.0F), 4.0F) &&
              near(motion_blur_scale(0.5F, 1.0F / 120.0F), 1.0F),
          "motion blur must cover a fixed exposure time");
    check(near(motion_blur_scale(0.0F, 1.0F / 60.0F), 0.0F), "0% must be off");
    check(motion_blur_scale(1.0F, 0.0F) <= 1000.0F / 60.0F + 0.001F &&
              motion_blur_scale(1.0F, 10.0F) >= 0.24F,
          "a stalled or instant frame must not explode the streak");

    // Screen-space and baked occlusion combine as the darker of the two.
    check(near(residual_occlusion(1.0F, 0.6F), 0.6F) && near(residual_occlusion(0.6F, 0.6F), 1.0F) &&
              near(residual_occlusion(0.5F, 0.9F), 1.0F),
          "a crease the bake already darkened must not be darkened again");
    for (float baked = 0.1F; baked <= 1.0F; baked += 0.1F)
        for (float screen = 0.0F; screen <= 1.0F; screen += 0.1F)
            check(near(baked * residual_occlusion(baked, screen), std::min(baked, screen), 1.0e-5F),
                  "combined occlusion must equal min(baked, screen-space)");
}

void moonlight_takes_over_without_a_pop()
{
    using namespace post;
    // Walk the sun round a full day. The moon is a unit vector, it is up
    // whenever it gives light, and it gives none while the sun is up.
    float previous{};
    bool lit_at_night{};
    for (int step = 0; step <= 720; ++step)
    {
        const float angle = 2.0F * pi * static_cast<float>(step) / 720.0F;
        const float sun_x = std::cos(angle) * 0.75F;
        const float sun_y = std::sin(angle) * 0.93F;
        const float sun_z = 0.37F;
        const std::array<float, 3> moon = moon_direction(sun_x, sun_y, sun_z);
        check(near(moon[0] * moon[0] + moon[1] * moon[1] + moon[2] * moon[2], 1.0F, 1.0e-4F),
              "the moon direction must be a unit vector");
        const float light = moonlight(sun_y, moon[1]);
        check(light >= 0.0F && light <= 1.0F, "moonlight is a share");
        if (sun_y >= -0.02F)
            check(near(light, 0.0F, 1.0e-6F), "no moonlight while the sun is up or setting");
        if (light > 0.0F)
            check(moon[1] > 0.03F, "the moon must be risen while it lights the world");
        // The hand-over is smooth: the light never jumps between two moments.
        if (step > 0)
            check(std::abs(light - previous) < 0.16F, "moonlight must fade, not switch");
        previous = light;
        lit_at_night = lit_at_night || light > 0.95F;
    }
    check(lit_at_night, "the middle of the night must be fully moonlit");
    check(near(moonlight(-1.0F, 0.0F), 0.0F), "a moon on the horizon gives no light");
    const std::array<float, 3> overhead = moon_direction(0.0F, -1.0F, 0.0F);
    check(overhead[1] > 0.9F, "at midnight the moon is high");
}

} // namespace

int main()
{
    struct Test final
    {
        const char *name;
        void (*run)();
    };
    constexpr std::array tests{
        Test{"defaults are in range and are the HIGH preset",
             defaults_are_in_range_and_are_the_high_preset},
        Test{"every option has one row, key and plain description",
             every_option_has_one_row_key_and_plain_description},
        Test{"every option changes its value and resets",
             every_option_changes_its_value_and_resets},
        Test{"options survive a round trip exactly", options_survive_a_round_trip_exactly},
        Test{"older and damaged files load safely", older_and_damaged_files_load_safely},
        Test{"presets are coherent and fall back to custom",
             presets_are_coherent_and_fall_back_to_custom},
        Test{"the Original presentation runs none of it", the_original_presentation_runs_none_of_it},
        Test{"far terrain rows resolve and stay out of the Original",
             far_terrain_rows_resolve_and_stay_out_of_the_original},
        Test{"dependent rows follow their switch", dependent_rows_follow_their_switch},
        Test{"circle of confusion is a thin lens", circle_of_confusion_is_a_thin_lens},
        Test{"autofocus eases without overshoot", autofocus_eases_without_overshoot},
        Test{"bloom conserves energy", bloom_conserves_energy},
        Test{"exposure closes but never opens", exposure_closes_but_never_opens},
        Test{"lens and light helpers behave", lens_and_light_helpers_behave},
        Test{"moonlight takes over without a pop", moonlight_takes_over_without_a_pop},
    };
    int failures{};
    for (const Test &test : tests)
    {
        try
        {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - static_cast<std::size_t>(failures)) << '/' << tests.size()
              << " graphics checks passed\n";
    return failures == 0 ? 0 : 1;
}
