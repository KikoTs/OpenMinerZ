#include "openminerz/graphics_settings.hpp"

#include "openminerz/graphics_options.hpp"
#include "openminerz/settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ostream>

namespace openminerz
{
namespace
{

float snapped(float value, float minimum, float maximum, float step) noexcept
{
    if (!std::isfinite(value))
        return minimum;
    const float steps = std::round((value - minimum) / step);
    return std::clamp(minimum + steps * step, minimum, maximum);
}

EffectQuality valid_quality(EffectQuality value, EffectQuality fallback, bool allow_off) noexcept
{
    if (static_cast<unsigned>(value) > static_cast<unsigned>(EffectQuality::ultra))
        return fallback;
    if (!allow_off && value == EffectQuality::off)
        return EffectQuality::low;
    return value;
}

// The preset-controlled half of GraphicsSettings.
struct GraphicsPreset final
{
    EffectQuality ambient_shading;
    bool contact_shadows;
    EffectQuality volumetric_quality;
    EffectQuality cloud_quality;
    DepthOfFieldMode depth_of_field;
    bool lens_flare;
    int shadow_distance;
    // The deep underground (src/hell_atmosphere.hpp).
    bool heat_haze;
    EffectQuality embers;
    // Far terrain (openminerz/world/far_terrain.hpp): distance, then detail.
    FarTerrainDistance far_terrain;
    EffectQuality far_terrain_detail;
    // The effects layer (openminerz/vfx.hpp).
    bool effect_lighting;
    bool distortion_effects;
};

// HIGH must equal the defaults in GraphicsSettings (a unit test checks).
// Far terrain: off on Low, then 2, 4 and 8 km; 500 m, 1 km and 12 km are
// manual choices.
constexpr std::array<GraphicsPreset, 4> graphics_presets{{
    {EffectQuality::off, false, EffectQuality::low, EffectQuality::low, DepthOfFieldMode::off,
     false, 56, false, EffectQuality::off, FarTerrainDistance::off, EffectQuality::low, false,
     false},
    {EffectQuality::medium, true, EffectQuality::medium, EffectQuality::medium,
     DepthOfFieldMode::aiming, true, 88, true, EffectQuality::medium, FarTerrainDistance::km2,
     EffectQuality::medium, true, true},
    {EffectQuality::high, true, EffectQuality::high, EffectQuality::high,
     DepthOfFieldMode::aiming, true, 150, true, EffectQuality::high, FarTerrainDistance::km4,
     EffectQuality::high, true, true},
    {EffectQuality::ultra, true, EffectQuality::ultra, EffectQuality::ultra,
     DepthOfFieldMode::aiming, true, 150, true, EffectQuality::ultra, FarTerrainDistance::km8,
     EffectQuality::ultra, true, true},
}};

} // namespace

void GraphicsSettings::normalize() noexcept
{
    const GraphicsSettings defaults{};
    ambient_shading = valid_quality(ambient_shading, defaults.ambient_shading, true);
    volumetric_quality = valid_quality(volumetric_quality, defaults.volumetric_quality, false);
    cloud_quality = valid_quality(cloud_quality, defaults.cloud_quality, true);
    embers = valid_quality(embers, defaults.embers, true);
    if (static_cast<unsigned>(far_terrain) > static_cast<unsigned>(FarTerrainDistance::km12))
        far_terrain = defaults.far_terrain;
    far_terrain_detail = valid_quality(far_terrain_detail, defaults.far_terrain_detail, false);
    if (static_cast<unsigned>(depth_of_field) > static_cast<unsigned>(DepthOfFieldMode::always))
        depth_of_field = defaults.depth_of_field;
    // 48, 56 .. 144 in steps of eight blocks, then the renderer's limit of 150.
    shadow_distance = shadow_distance >= 148
                          ? 150
                          : std::clamp(48 + ((shadow_distance - 48 + 4) / 8) * 8, 48, 144);
    depth_of_field_strength = snapped(depth_of_field_strength, 0.10F, 1.00F, 0.05F);
    bloom_strength = snapped(bloom_strength, 0.25F, 2.00F, 0.25F);
    motion_blur = snapped(motion_blur, 0.0F, 1.0F, 0.10F);
    explosion_shake = snapped(explosion_shake, 0.0F, 1.0F, 0.10F);
    sharpening = snapped(sharpening, 0.0F, 1.0F, 0.05F);
    contrast = snapped(contrast, 0.80F, 1.20F, 0.02F);
    saturation = snapped(saturation, 0.0F, 1.50F, 0.05F);
    temperature = snapped(temperature, -1.0F, 1.0F, 0.10F);
    vignette = snapped(vignette, 0.0F, 1.0F, 0.05F);
    film_grain = snapped(film_grain, 0.0F, 1.0F, 0.05F);
    chromatic_aberration = snapped(chromatic_aberration, 0.0F, 1.0F, 0.05F);
    shadow_softness = snapped(shadow_softness, 0.0F, 1.0F, 0.05F);
    texture_detail = snapped(texture_detail, -1.0F, 1.0F, 0.25F);
    deep_light = snapped(deep_light, 0.50F, 2.00F, 0.10F);
}

void apply_graphics_preset(GraphicsSettings &graphics, int level) noexcept
{
    if (level < 0 || level >= static_cast<int>(graphics_presets.size()))
        return;
    const GraphicsPreset &preset = graphics_presets[static_cast<std::size_t>(level)];
    graphics.ambient_shading = preset.ambient_shading;
    graphics.contact_shadows = preset.contact_shadows;
    graphics.volumetric_quality = preset.volumetric_quality;
    graphics.cloud_quality = preset.cloud_quality;
    graphics.depth_of_field = preset.depth_of_field;
    graphics.lens_flare = preset.lens_flare;
    graphics.shadow_distance = preset.shadow_distance;
    graphics.heat_haze = preset.heat_haze;
    graphics.embers = preset.embers;
    graphics.far_terrain = preset.far_terrain;
    graphics.far_terrain_detail = preset.far_terrain_detail;
    graphics.effect_lighting = preset.effect_lighting;
    graphics.distortion_effects = preset.distortion_effects;
}

bool graphics_matches_preset(const GraphicsSettings &graphics, int level, bool shadows_enabled,
                             bool light_shafts) noexcept
{
    if (level < 0 || level >= static_cast<int>(graphics_presets.size()))
        return false;
    const GraphicsPreset &preset = graphics_presets[static_cast<std::size_t>(level)];
    const bool shadow_rows_match = !shadows_enabled ||
                                   (graphics.contact_shadows == preset.contact_shadows &&
                                    graphics.shadow_distance == preset.shadow_distance);
    const bool shaft_rows_match =
        !light_shafts || graphics.volumetric_quality == preset.volumetric_quality;
    return graphics.ambient_shading == preset.ambient_shading && shadow_rows_match &&
           shaft_rows_match && graphics.cloud_quality == preset.cloud_quality &&
           graphics.depth_of_field == preset.depth_of_field &&
           graphics.lens_flare == preset.lens_flare && graphics.heat_haze == preset.heat_haze &&
           graphics.embers == preset.embers && graphics.far_terrain == preset.far_terrain &&
           graphics.effect_lighting == preset.effect_lighting &&
           graphics.distortion_effects == preset.distortion_effects &&
           // (The detail row draws nothing while far terrain is off.)
           (graphics.far_terrain == FarTerrainDistance::off ||
            graphics.far_terrain_detail == preset.far_terrain_detail);
}

int far_terrain_blocks(FarTerrainDistance distance) noexcept
{
    constexpr std::array<int, 7> blocks{0, 500, 1000, 2000, 4000, 8000, 12000};
    return blocks[static_cast<std::size_t>(std::clamp(static_cast<int>(distance), 0, 6))];
}

FarTerrainChoice resolve_far_terrain(const Settings &settings) noexcept
{
    FarTerrainChoice choice;
    // The Original presentation keeps the 2011 game's draw distance and fog.
    if (settings.presentation != PresentationProfile::rebirth)
        return choice;
    GraphicsSettings graphics = settings.graphics;
    graphics.normalize();
    choice.distance = far_terrain_blocks(graphics.far_terrain);
    // Low starts at two-block cells; the others at single blocks, each level
    // reaching further before it hands over to the next.
    switch (graphics.far_terrain_detail)
    {
    case EffectQuality::off:
    case EffectQuality::low:
        choice.base_cell = 2;
        choice.ring_cells = 192;
        break;
    case EffectQuality::medium:
        choice.base_cell = 1;
        choice.ring_cells = 192;
        break;
    case EffectQuality::high:
        choice.base_cell = 1;
        choice.ring_cells = 256;
        break;
    case EffectQuality::ultra:
        choice.base_cell = 1;
        choice.ring_cells = 384;
        break;
    }
    return choice;
}

PostEffects resolve_post_effects(const Settings &settings) noexcept
{
    PostEffects effects;
    // The Original presentation is the 2011 game's picture and never changes.
    if (settings.presentation != PresentationProfile::rebirth)
        return effects;
    GraphicsSettings graphics = settings.graphics;
    graphics.normalize();
    effects.enabled = true;
    effects.reduced_motion = settings.reduced_motion;

    effects.bloom = settings.bloom;
    effects.bloom_strength = graphics.bloom_strength;

    switch (graphics.ambient_shading)
    {
    case EffectQuality::off:
        break;
    // Horizon slices per pixel and steps along each side of a slice. Every
    // pixel of a 4x4 block uses a different set of directions and the blur
    // that follows averages the block, so two slices already see 32 directions.
    case EffectQuality::low:
        effects.ao_slices = 1;
        effects.ao_steps = 3;
        break;
    case EffectQuality::medium:
        effects.ao_slices = 2;
        effects.ao_steps = 3;
        break;
    case EffectQuality::high:
        effects.ao_slices = 2;
        effects.ao_steps = 5;
        break;
    case EffectQuality::ultra:
        effects.ao_slices = 4;
        effects.ao_steps = 6;
        break;
    }
    // Both shadow terms follow the Shadows switch: sun shadows that stop at a
    // creature's feet without the terrain's own would look wrong.
    effects.contact_shadows = graphics.contact_shadows && settings.shadows_enabled;
    effects.contact_steps = graphics.ambient_shading == EffectQuality::ultra ? 14 : 8;

    if (settings.sun_shafts)
    {
        switch (graphics.volumetric_quality)
        {
        // Steps of the march at half resolution. Each pixel starts its march at
        // a different offset and a 3x3 blur follows, so the beams resolve as
        // if every pixel had taken nine times as many.
        case EffectQuality::off:
        case EffectQuality::low:
            effects.volumetric_steps = 6;
            break;
        case EffectQuality::medium:
            effects.volumetric_steps = 10;
            break;
        case EffectQuality::high:
            effects.volumetric_steps = 16;
            break;
        case EffectQuality::ultra:
            effects.volumetric_steps = 32;
            break;
        }
    }

    effects.cloud_quality = static_cast<int>(graphics.cloud_quality);

    effects.depth_of_field = graphics.depth_of_field;
    effects.depth_of_field_strength = graphics.depth_of_field_strength;
    effects.depth_of_field_taps = 44;

    // A streak across the picture is exactly the motion Reduced motion removes.
    effects.motion_blur = settings.reduced_motion ? 0.0F : graphics.motion_blur;
    effects.motion_blur_taps = 12;

    effects.fxaa = settings.anti_aliasing != AntiAliasing::off;
    effects.sharpening = graphics.sharpening;
    effects.lens_flare = graphics.lens_flare;
    effects.contrast = graphics.contrast;
    effects.saturation = graphics.saturation;
    effects.temperature = graphics.temperature;
    effects.vignette = graphics.vignette;
    effects.film_grain = graphics.film_grain;
    effects.chromatic_aberration = graphics.chromatic_aberration;
    effects.shadow_softness = graphics.shadow_softness;
    effects.texture_detail = graphics.texture_detail;

    // The deep underground. Its light is always on: it is what makes Hell
    // visible at all. How finely the lava's glow is traced through the air
    // follows the same level as the light in the air above ground, whether or
    // not light shafts are on.
    effects.deep_light = graphics.deep_light;
    switch (graphics.volumetric_quality)
    {
    case EffectQuality::off:
    case EffectQuality::low:
        effects.deep_air_steps = 5;
        break;
    case EffectQuality::medium:
        effects.deep_air_steps = 6;
        break;
    case EffectQuality::high:
        effects.deep_air_steps = 8;
        break;
    case EffectQuality::ultra:
        effects.deep_air_steps = 12;
        break;
    }
    // Shimmer and drifting sparks are exactly the motion Reduced motion removes.
    effects.heat_haze = graphics.heat_haze && !settings.reduced_motion;
    constexpr std::array<int, 5> ember_counts{0, 700, 1500, 3000, 6000};
    effects.ember_count =
        settings.reduced_motion ? 0 : ember_counts[static_cast<std::size_t>(graphics.embers)];

    // The effects layer (openminerz/vfx.hpp). How many particles it may keep
    // alive follows the Particles level; these are what the picture does with
    // them. A bent picture, a shaking camera and a flash are exactly the
    // motion Reduced motion removes.
    effects.effect_lights = graphics.effect_lighting ? 16 : 0;
    effects.effect_distortion = graphics.distortion_effects && !settings.reduced_motion;
    effects.effect_shake = settings.reduced_motion ? 0.0F : graphics.explosion_shake;
    effects.effect_flash = graphics.explosion_flash && !settings.reduced_motion ? 1.0F : 0.0F;
    // Gas is drawn at half resolution and upsampled against scene depth; the
    // Ultra particle level draws it at full resolution.
    effects.effect_resolution = settings.particle_quality == QualityLevel::ultra ? 1 : 2;
    return effects;
}

namespace ui
{
namespace
{

std::string_view shadows_off(const Settings &settings) noexcept
{
    return settings.shadows_enabled ? std::string_view{} : "Shadows are turned off.";
}

std::string_view bloom_off(const Settings &settings) noexcept
{
    return settings.bloom ? std::string_view{} : "Bloom is turned off.";
}

std::string_view shafts_off(const Settings &settings) noexcept
{
    return settings.sun_shafts ? std::string_view{} : "Light shafts are turned off.";
}

std::string_view focus_off(const Settings &settings) noexcept
{
    return settings.graphics.depth_of_field != DepthOfFieldMode::off
               ? std::string_view{}
               : "Depth of field is turned off.";
}

std::string_view smoothing_reduced(const Settings &settings) noexcept
{
    return settings.reduced_motion ? "Reduced motion is on." : std::string_view{};
}

// Only the Enhanced presentation runs these passes; say so rather than let a
// row change nothing on screen.
std::string_view classic_reason(const Settings &settings) noexcept
{
    return settings.presentation == PresentationProfile::rebirth
               ? std::string_view{}
               : "Only with the Enhanced lighting style.";
}

GraphicsOption toggle_option(SettingId id, SettingsTab tab, SettingId after, std::string_view key,
                             std::string_view label, std::string_view description,
                             bool GraphicsSettings::*field, bool preset_controlled,
                             std::string_view (*unavailable)(const Settings &) noexcept = nullptr)
{
    GraphicsOption option;
    option.id = id;
    option.tab = tab;
    option.after = after;
    option.key = key;
    option.label = label;
    option.description = description;
    option.kind = SettingKind::toggle;
    option.preset_controlled = preset_controlled;
    option.storage = GraphicsOptionStorage::flag;
    option.flag = field;
    option.unavailable = unavailable;
    return option;
}

GraphicsOption quality_option(SettingId id, SettingsTab tab, SettingId after, std::string_view key,
                              std::string_view label, std::string_view description,
                              EffectQuality GraphicsSettings::*field, bool allows_off,
                              std::string_view (*unavailable)(const Settings &) noexcept = nullptr)
{
    GraphicsOption option;
    option.id = id;
    option.tab = tab;
    option.after = after;
    option.key = key;
    option.label = label;
    option.description = description;
    option.kind = SettingKind::choice;
    option.preset_controlled = true;
    option.storage = GraphicsOptionStorage::quality;
    option.quality = field;
    option.quality_allows_off = allows_off;
    option.unavailable = unavailable;
    return option;
}

GraphicsOption scalar_option(SettingId id, SettingsTab tab, SettingId after, std::string_view key,
                             std::string_view label, std::string_view description,
                             float GraphicsSettings::*field, float minimum, float maximum,
                             float step, GraphicsValueFormat format,
                             std::string_view (*unavailable)(const Settings &) noexcept = nullptr)
{
    GraphicsOption option;
    option.id = id;
    option.tab = tab;
    option.after = after;
    option.key = key;
    option.label = label;
    option.description = description;
    option.kind = SettingKind::slider;
    option.storage = GraphicsOptionStorage::scalar;
    option.scalar = field;
    option.minimum = minimum;
    option.maximum = maximum;
    option.step = step;
    option.format = format;
    option.unavailable = unavailable;
    return option;
}

// Every description says what the option changes on screen and what it costs,
// in words for someone who has never opened a graphics menu, within the two
// lines the settings screen shows (about 170 characters; a unit test checks).
std::vector<GraphicsOption> build_options()
{
    std::vector<GraphicsOption> options;

    // --- Graphics page ---
    {
        GraphicsOption option;
        option.id = SettingId::shadow_distance;
        option.tab = SettingsTab::graphics;
        option.after = SettingId::shadow_darkness;
        option.key = "shadow_distance";
        option.label = "Shadow distance";
        option.description =
            "How far from you sun shadows are still drawn. Farther means more terrain is drawn "
            "into the shadows every frame. Medium cost.";
        option.kind = SettingKind::slider;
        option.preset_controlled = true;
        option.storage = GraphicsOptionStorage::integer;
        option.integer = &GraphicsSettings::shadow_distance;
        option.minimum = 48.0F;
        option.maximum = 150.0F;
        option.step = 8.0F;
        option.format = GraphicsValueFormat::blocks;
        option.unavailable = shadows_off;
        options.push_back(option);
    }
    options.push_back(scalar_option(
        SettingId::shadow_softness, SettingsTab::graphics, SettingId::shadow_distance,
        "shadow_softness", "Shadow softness",
        "How much a sun shadow's edge blurs the farther it falls from what casts it. 0% keeps "
        "every edge crisp. A matter of taste. No cost.",
        &GraphicsSettings::shadow_softness, 0.0F, 1.0F, 0.05F, GraphicsValueFormat::percent,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? shadows_off(settings) : presentation;
        }));
    options.push_back(quality_option(
        SettingId::ambient_shading, SettingsTab::graphics, SettingId::shadow_softness,
        "ambient_shading", "Ambient shading",
        "Soft shade in corners, under ledges and where things touch, worked out from what is on "
        "screen (ambient occlusion). Higher is smoother. Low to medium cost.",
        &GraphicsSettings::ambient_shading, true, classic_reason));
    options.push_back(toggle_option(
        SettingId::contact_shadows, SettingsTab::graphics, SettingId::ambient_shading,
        "contact_shadows", "Contact shadows",
        "Short, sharp sun shadows right where creatures, dropped items and block edges meet the "
        "ground, so nothing looks like it floats. Low cost.",
        &GraphicsSettings::contact_shadows, true,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? shadows_off(settings) : presentation;
        }));
    options.push_back(quality_option(
        SettingId::cloud_quality, SettingsTab::graphics, SettingId::contact_shadows,
        "cloud_quality", "Clouds",
        "Drifting clouds lit by the sun and the moon. Off keeps only the painted sky. Higher "
        "adds finer detail and softer light. Low cost.",
        &GraphicsSettings::cloud_quality, true, classic_reason));
    options.push_back(scalar_option(
        SettingId::texture_detail, SettingsTab::graphics, SettingId::texture_filtering,
        "texture_detail", "Texture detail",
        "Picks a sharper or softer copy of each block texture in the distance. Higher is "
        "crisper and can shimmer; lower is calmer. No cost.",
        &GraphicsSettings::texture_detail, -1.0F, 1.0F, 0.25F,
        GraphicsValueFormat::signed_steps, classic_reason));
    // Far terrain (openminerz/world/far_terrain.hpp), right under the view
    // distance it continues.
    // Seven distances (far_distance_labels), so a storage of its own rather
    // than a five-level quality.
    {
        GraphicsOption option;
        option.id = SettingId::far_terrain;
        option.tab = SettingsTab::graphics;
        option.after = SettingId::view_distance;
        option.key = "far_terrain";
        option.label = "Far terrain";
        option.description =
            "Draws the land far beyond the view distance as simple low-detail terrain, so hills "
            "and mountains stay on the horizon. Nothing out there is simulated. Medium cost.";
        option.kind = SettingKind::choice;
        option.preset_controlled = true;
        option.storage = GraphicsOptionStorage::far_distance;
        option.far_distance = &GraphicsSettings::far_terrain;
        option.unavailable = classic_reason;
        options.push_back(option);
    }
    options.push_back(quality_option(
        SettingId::far_terrain_detail, SettingsTab::graphics, SettingId::far_terrain,
        "far_terrain_detail", "Far terrain detail",
        "How finely that far land is drawn. Higher keeps single blocks visible further away "
        "and uses more memory. Low to medium cost.",
        &GraphicsSettings::far_terrain_detail, false,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            if (!presentation.empty())
                return presentation;
            return settings.graphics.far_terrain != FarTerrainDistance::off
                       ? std::string_view{}
                       : "Far terrain is turned off.";
        }));

    // --- Effects page ---
    options.push_back(scalar_option(
        SettingId::bloom_strength, SettingsTab::effects, SettingId::bloom, "bloom_strength",
        "Bloom strength",
        "How far the glow spreads around the sun, lava, torches and gunfire. 100% is the "
        "intended look. No extra cost.",
        &GraphicsSettings::bloom_strength, 0.25F, 2.0F, 0.25F, GraphicsValueFormat::percent,
        bloom_off));
    options.push_back(quality_option(
        SettingId::volumetric_quality, SettingsTab::effects, SettingId::sun_shafts,
        "volumetric_quality", "Light shaft quality",
        "How finely the light in the air is traced. Higher gives smoother beams with cleaner "
        "edges. Low to medium cost.",
        &GraphicsSettings::volumetric_quality, false,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? shafts_off(settings) : presentation;
        }));
    {
        GraphicsOption option;
        option.id = SettingId::depth_of_field;
        option.tab = SettingsTab::effects;
        option.after = SettingId::volumetric_quality;
        option.key = "depth_of_field";
        option.label = "Depth of field";
        option.description =
            "Blurs what is nearer or farther than the thing you are looking at, like a camera "
            "lens. When aiming only does it while a weapon is raised. Low cost.";
        option.kind = SettingKind::choice;
        option.preset_controlled = true;
        option.storage = GraphicsOptionStorage::focus_mode;
        option.focus_mode = &GraphicsSettings::depth_of_field;
        option.unavailable = classic_reason;
        options.push_back(option);
    }
    options.push_back(scalar_option(
        SettingId::depth_of_field_strength, SettingsTab::effects, SettingId::depth_of_field,
        "depth_of_field_strength", "Depth of field strength",
        "How strong that blur is, like opening a lens wider. Low keeps almost everything sharp. "
        "No extra cost.",
        &GraphicsSettings::depth_of_field_strength, 0.10F, 1.0F, 0.05F,
        GraphicsValueFormat::percent, focus_off));
    options.push_back(toggle_option(
        SettingId::lens_flare, SettingsTab::effects, SettingId::depth_of_field_strength,
        "lens_flare", "Sun glare",
        "A soft glare and faint lens reflections when you look toward the sun. Very low cost.",
        &GraphicsSettings::lens_flare, true, classic_reason));
    // The deep underground (src/hell_atmosphere.hpp).
    options.push_back(toggle_option(
        SettingId::heat_haze, SettingsTab::effects, SettingId::lens_flare, "heat_haze",
        "Heat haze",
        "The air shimmers above lava and hot ground deep underground. It never moves the "
        "crosshair, your weapon or the interface. Low cost.",
        &GraphicsSettings::heat_haze, true,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? smoothing_reduced(settings) : presentation;
        }));
    options.push_back(quality_option(
        SettingId::embers, SettingsTab::effects, SettingId::heat_haze, "embers",
        "Embers and ash",
        "Glowing embers rise from lava and ash drifts through the deep underground. Higher "
        "shows more of them. Very low cost.",
        &GraphicsSettings::embers, true,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? smoothing_reduced(settings) : presentation;
        }));
    options.push_back(scalar_option(
        SettingId::deep_light, SettingsTab::effects, SettingId::embers, "deep_light",
        "Deep underground light",
        "How brightly the deep underground is lit by its own glow and its lava. Raise it if "
        "Hell is hard to see on your screen. No cost.",
        &GraphicsSettings::deep_light, 0.50F, 2.0F, 0.10F, GraphicsValueFormat::percent,
        classic_reason));
    // The effects layer (openminerz/vfx.hpp).
    options.push_back(toggle_option(
        SettingId::effect_lighting, SettingsTab::effects, SettingId::deep_light,
        "effect_lighting", "Effect lighting",
        "Explosions, flames and sparks light the ground, creatures, smoke and your weapon "
        "around them. Low cost, and only while something burns.",
        &GraphicsSettings::effect_lighting, true, classic_reason));
    options.push_back(toggle_option(
        SettingId::distortion_effects, SettingsTab::effects, SettingId::effect_lighting,
        "distortion_effects", "Distortion effects",
        "A blast's shockwave and the hot air above flames bend the picture behind them. It "
        "never moves the crosshair, your weapon or the interface. Very low cost.",
        &GraphicsSettings::distortion_effects, true,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? smoothing_reduced(settings) : presentation;
        }));
    options.push_back(scalar_option(
        SettingId::explosion_shake, SettingsTab::effects, SettingId::distortion_effects,
        "explosion_shake", "Explosion camera shake",
        "How hard a nearby explosion shakes the view. Your aim is not moved. No cost.",
        &GraphicsSettings::explosion_shake, 0.0F, 1.0F, 0.10F,
        GraphicsValueFormat::percent_or_off,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? smoothing_reduced(settings) : presentation;
        }));
    options.push_back(toggle_option(
        SettingId::explosion_flash, SettingsTab::effects, SettingId::explosion_shake,
        "explosion_flash", "Explosion flash",
        "A nearby explosion you can see briefly brightens the whole picture. No cost.",
        &GraphicsSettings::explosion_flash, false,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? smoothing_reduced(settings) : presentation;
        }));
    options.push_back(scalar_option(
        SettingId::motion_blur, SettingsTab::effects, SettingId::lens_flare, "motion_blur",
        "Motion blur",
        "Streaks fast movement of the view and of creatures, like a film camera. Held items "
        "stay sharp. Off by default. Low cost.",
        &GraphicsSettings::motion_blur, 0.0F, 1.0F, 0.10F, GraphicsValueFormat::percent_or_off,
        [](const Settings &settings) noexcept -> std::string_view {
            const std::string_view presentation = classic_reason(settings);
            return presentation.empty() ? smoothing_reduced(settings) : presentation;
        }));
    options.push_back(scalar_option(
        SettingId::sharpening, SettingsTab::effects, SettingId::motion_blur, "sharpening",
        "Sharpening",
        "Brings back fine texture detail that edge smoothing softens, without bright outlines "
        "(contrast-adaptive sharpening). Very low cost.",
        &GraphicsSettings::sharpening, 0.0F, 1.0F, 0.05F, GraphicsValueFormat::percent_or_off,
        classic_reason));
    options.push_back(scalar_option(
        SettingId::contrast, SettingsTab::effects, SettingId::color_grading, "contrast",
        "Contrast",
        "The difference between the light and dark parts of the picture. 100% is the intended "
        "look. No cost.",
        &GraphicsSettings::contrast, 0.80F, 1.20F, 0.02F, GraphicsValueFormat::percent,
        classic_reason));
    options.push_back(scalar_option(
        SettingId::saturation, SettingsTab::effects, SettingId::contrast, "saturation",
        "Colour strength",
        "How vivid colours are. 0% is black and white, 100% is the intended look. No cost.",
        &GraphicsSettings::saturation, 0.0F, 1.50F, 0.05F, GraphicsValueFormat::percent,
        classic_reason));
    options.push_back(scalar_option(
        SettingId::temperature, SettingsTab::effects, SettingId::saturation, "temperature",
        "Colour temperature",
        "Tints the whole picture cooler (bluer) or warmer (more orange). Neutral is the "
        "intended look. No cost.",
        &GraphicsSettings::temperature, -1.0F, 1.0F, 0.10F, GraphicsValueFormat::temperature,
        classic_reason));
    options.push_back(scalar_option(
        SettingId::vignette, SettingsTab::effects, SettingId::temperature, "vignette", "Vignette",
        "Gently darkens the corners of the picture to draw the eye to the middle. The centre "
        "is never touched. 0% turns it off. No cost.",
        &GraphicsSettings::vignette, 0.0F, 1.0F, 0.05F, GraphicsValueFormat::percent_or_off,
        classic_reason));
    options.push_back(scalar_option(
        SettingId::film_grain, SettingsTab::effects, SettingId::vignette, "film_grain",
        "Film grain",
        "A fine moving grain like camera film. Off by default. Very low cost.",
        &GraphicsSettings::film_grain, 0.0F, 1.0F, 0.05F, GraphicsValueFormat::percent_or_off,
        classic_reason));
    options.push_back(scalar_option(
        SettingId::chromatic_aberration, SettingsTab::effects, SettingId::film_grain,
        "chromatic_aberration", "Colour fringing",
        "Splits colours slightly toward the edges of the picture, like an old lens (chromatic "
        "aberration). Off by default. Very low cost.",
        &GraphicsSettings::chromatic_aberration, 0.0F, 1.0F, 0.05F,
        GraphicsValueFormat::percent_or_off, classic_reason));
    return options;
}

constexpr std::array<std::string_view, 5> quality_names{"off", "low", "medium", "high", "ultra"};
constexpr std::array<std::string_view, 3> focus_names{"off", "aiming", "always"};
constexpr std::array<std::string_view, 3> focus_labels{"OFF", "WHEN AIMING", "ALWAYS"};
// The far-terrain distances (FarTerrainDistance), as settings.cfg spells them
// and as the row shows them.
constexpr std::array<std::string_view, 7> far_distance_names{"off", "500m", "1km", "2km",
                                                             "4km", "8km",  "12km"};
constexpr std::array<std::string_view, 7> far_distance_labels{"OFF",  "500 M", "1 KM", "2 KM",
                                                              "4 KM", "8 KM",  "12 KM"};
// A file written while the row had five levels spelled them as quality names:
// off, then 2, 4, 8 and 12 km. They keep that meaning.
constexpr std::array<std::string_view, 5> far_distance_legacy_names{"off", "low", "medium", "high",
                                                                    "ultra"};
constexpr std::array<FarTerrainDistance, 5> far_distance_legacy{
    FarTerrainDistance::off, FarTerrainDistance::km2, FarTerrainDistance::km4,
    FarTerrainDistance::km8, FarTerrainDistance::km12};

std::string upper(std::string_view text)
{
    std::string result{text};
    for (char &character : result)
        if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 32);
    return result;
}

std::string percent_text(float value)
{
    return std::to_string(static_cast<int>(std::lround(value * 100.0F))) + "%";
}

std::string scalar_text(const GraphicsOption &option, float value)
{
    switch (option.format)
    {
    case GraphicsValueFormat::percent:
        return percent_text(value);
    case GraphicsValueFormat::percent_or_off:
        return value <= option.step * 0.5F ? "OFF" : percent_text(value);
    case GraphicsValueFormat::temperature:
        if (std::abs(value) < option.step * 0.5F)
            return "NEUTRAL";
        return (value < 0.0F ? "COOL " : "WARM ") + percent_text(std::abs(value));
    case GraphicsValueFormat::signed_steps: {
        if (std::abs(value) < option.step * 0.5F)
            return "NORMAL";
        const int hundredths = static_cast<int>(std::lround(std::abs(value) * 100.0F));
        std::string text = value < 0.0F ? "SOFTER " : "CRISPER ";
        text += std::to_string(hundredths) + "%";
        return text;
    }
    case GraphicsValueFormat::blocks:
        return std::to_string(static_cast<int>(std::lround(value))) + " BLOCKS";
    }
    return {};
}

} // namespace

std::span<const GraphicsOption> graphics_options()
{
    // Built on first use: the menu's row tables are namespace-scope objects in
    // another translation unit and read this while they are constructed.
    static const std::vector<GraphicsOption> options = build_options();
    return options;
}

const GraphicsOption *graphics_option(SettingId id)
{
    for (const GraphicsOption &option : graphics_options())
        if (option.id == id)
            return &option;
    return nullptr;
}

std::vector<SettingRow> with_graphics_options(SettingsTab tab, std::vector<SettingRow> rows)
{
    for (const GraphicsOption &option : graphics_options())
    {
        if (option.tab != tab)
            continue;
        const auto anchor = std::find_if(rows.begin(), rows.end(), [&](const SettingRow &row) {
            return row.id == option.after;
        });
        const SettingRow row{option.id, option.label, option.description, option.kind, false};
        // An anchor another engineer has renamed or removed must not drop the
        // option: it then goes to the end of its page.
        rows.insert(anchor == rows.end() ? rows.end() : anchor + 1, row);
    }
    return rows;
}

std::string graphics_option_value(const GraphicsOption &option, const Settings &settings)
{
    const GraphicsSettings &graphics = settings.graphics;
    switch (option.storage)
    {
    case GraphicsOptionStorage::flag:
        return graphics.*(option.flag) ? "ON" : "OFF";
    case GraphicsOptionStorage::quality: {
        const auto level = static_cast<std::size_t>(
            std::clamp(static_cast<int>(graphics.*(option.quality)), 0, 4));
        if (!option.quality_labels[level].empty())
            return std::string{option.quality_labels[level]};
        return upper(quality_names[level]);
    }
    case GraphicsOptionStorage::focus_mode:
        return std::string{focus_labels[static_cast<std::size_t>(
            std::clamp(static_cast<int>(graphics.*(option.focus_mode)), 0, 2))]};
    case GraphicsOptionStorage::scalar:
        return scalar_text(option, graphics.*(option.scalar));
    case GraphicsOptionStorage::integer:
        return scalar_text(option, static_cast<float>(graphics.*(option.integer)));
    case GraphicsOptionStorage::far_distance:
        return std::string{far_distance_labels[static_cast<std::size_t>(
            std::clamp(static_cast<int>(graphics.*(option.far_distance)), 0, 6))]};
    }
    return {};
}

float graphics_option_normalized(const GraphicsOption &option, const Settings &settings) noexcept
{
    const GraphicsSettings &graphics = settings.graphics;
    const float range = std::max(option.maximum - option.minimum, 0.0001F);
    switch (option.storage)
    {
    case GraphicsOptionStorage::scalar:
        return std::clamp((graphics.*(option.scalar) - option.minimum) / range, 0.0F, 1.0F);
    case GraphicsOptionStorage::integer:
        return std::clamp(
            (static_cast<float>(graphics.*(option.integer)) - option.minimum) / range, 0.0F, 1.0F);
    default:
        return 0.0F;
    }
}

void adjust_graphics_option(const GraphicsOption &option, Settings &settings, int direction)
{
    const int sign = direction < 0 ? -1 : 1;
    GraphicsSettings &graphics = settings.graphics;
    switch (option.storage)
    {
    case GraphicsOptionStorage::flag:
        graphics.*(option.flag) = !(graphics.*(option.flag));
        break;
    case GraphicsOptionStorage::quality: {
        const int first = option.quality_allows_off ? 0 : 1;
        const int count = 5 - first;
        const int index = std::clamp(static_cast<int>(graphics.*(option.quality)), first, 4) - first;
        graphics.*(option.quality) =
            static_cast<EffectQuality>(first + (index + sign + count) % count);
        break;
    }
    case GraphicsOptionStorage::focus_mode: {
        const int index = std::clamp(static_cast<int>(graphics.*(option.focus_mode)), 0, 2);
        graphics.*(option.focus_mode) = static_cast<DepthOfFieldMode>((index + sign + 3) % 3);
        break;
    }
    case GraphicsOptionStorage::scalar:
        graphics.*(option.scalar) =
            std::clamp(graphics.*(option.scalar) + option.step * static_cast<float>(sign),
                       option.minimum, option.maximum);
        break;
    case GraphicsOptionStorage::integer:
        graphics.*(option.integer) = std::clamp(
            graphics.*(option.integer) + static_cast<int>(option.step) * sign,
            static_cast<int>(option.minimum), static_cast<int>(option.maximum));
        break;
    case GraphicsOptionStorage::far_distance: {
        // Round and round, like every other choice row.
        constexpr int count = static_cast<int>(far_distance_names.size());
        const int index =
            std::clamp(static_cast<int>(graphics.*(option.far_distance)), 0, count - 1);
        graphics.*(option.far_distance) =
            static_cast<FarTerrainDistance>((index + sign + count) % count);
        break;
    }
    }
    graphics.normalize();
}

void reset_graphics_option(const GraphicsOption &option, Settings &settings)
{
    const GraphicsSettings defaults{};
    GraphicsSettings &graphics = settings.graphics;
    switch (option.storage)
    {
    case GraphicsOptionStorage::flag:
        graphics.*(option.flag) = defaults.*(option.flag);
        break;
    case GraphicsOptionStorage::quality:
        graphics.*(option.quality) = defaults.*(option.quality);
        break;
    case GraphicsOptionStorage::focus_mode:
        graphics.*(option.focus_mode) = defaults.*(option.focus_mode);
        break;
    case GraphicsOptionStorage::scalar:
        graphics.*(option.scalar) = defaults.*(option.scalar);
        break;
    case GraphicsOptionStorage::integer:
        graphics.*(option.integer) = defaults.*(option.integer);
        break;
    case GraphicsOptionStorage::far_distance:
        graphics.*(option.far_distance) = defaults.*(option.far_distance);
        break;
    }
}

void load_graphics_options(Settings &settings,
                           const std::unordered_map<std::string, std::string> &values)
{
    GraphicsSettings &graphics = settings.graphics;
    // A file written before these options existed chose a quality level with
    // the options it had. Carry that level over instead of handing a player
    // who picked Low the High defaults: the new passes start at the bundle of
    // their old Shadows level (off counts as Low), which also keeps the shadow
    // distance that level always drew.
    if (!values.contains("ambient_shading") && values.contains("shadow_quality"))
        apply_graphics_preset(graphics, settings.shadows_enabled
                                            ? static_cast<int>(settings.shadow_quality)
                                            : 0);
    for (const GraphicsOption &option : graphics_options())
    {
        const auto found = values.find(std::string{option.key});
        if (found == values.end())
            continue;
        const std::string &text = found->second;
        switch (option.storage)
        {
        case GraphicsOptionStorage::flag:
            if (text == "1" || text == "true" || text == "on")
                graphics.*(option.flag) = true;
            else if (text == "0" || text == "false" || text == "off")
                graphics.*(option.flag) = false;
            break;
        case GraphicsOptionStorage::quality:
            for (std::size_t index = 0U; index < quality_names.size(); ++index)
                if (text == quality_names[index])
                    graphics.*(option.quality) = static_cast<EffectQuality>(index);
            break;
        case GraphicsOptionStorage::focus_mode:
            for (std::size_t index = 0U; index < focus_names.size(); ++index)
                if (text == focus_names[index])
                    graphics.*(option.focus_mode) = static_cast<DepthOfFieldMode>(index);
            break;
        case GraphicsOptionStorage::scalar:
        case GraphicsOptionStorage::integer:
            try
            {
                // A damaged value ("nan", text) keeps the default.
                const float parsed = std::stof(text);
                if (!std::isfinite(parsed))
                    break;
                if (option.storage == GraphicsOptionStorage::scalar)
                    graphics.*(option.scalar) = parsed;
                else
                    graphics.*(option.integer) =
                        static_cast<int>(std::lround(std::clamp(parsed, -100000.0F, 100000.0F)));
            }
            catch (...)
            {
            }
            break;
        case GraphicsOptionStorage::far_distance:
            // The names of the five-level row first, then today's: "off" is
            // in both and means the same.
            for (std::size_t index = 0U; index < far_distance_legacy_names.size(); ++index)
                if (text == far_distance_legacy_names[index])
                    graphics.*(option.far_distance) = far_distance_legacy[index];
            for (std::size_t index = 0U; index < far_distance_names.size(); ++index)
                if (text == far_distance_names[index])
                    graphics.*(option.far_distance) = static_cast<FarTerrainDistance>(index);
            break;
        }
    }
    // A file written before the deep-underground rows existed: if everything
    // else in it adds up to one quality level, the new rows take that level's
    // values too, so a profile that read LOW still reads LOW (and is not handed
    // the High embers).
    // The far-terrain rows arrived later still and are filled the same way, so
    // a LOW profile keeps far terrain off.
    const bool deep_rows_missing = values.contains("ambient_shading") &&
                                   !values.contains("heat_haze") && !values.contains("embers");
    const bool far_rows_missing =
        values.contains("ambient_shading") && !values.contains("far_terrain");
    // The effects layer's rows arrived after both, and are filled the same way.
    const bool effect_rows_missing = values.contains("ambient_shading") &&
                                     !values.contains("effect_lighting") &&
                                     !values.contains("distortion_effects");
    if (deep_rows_missing || far_rows_missing || effect_rows_missing)
        for (int level = 0; level < static_cast<int>(graphics_presets.size()); ++level)
        {
            const GraphicsPreset &preset = graphics_presets[static_cast<std::size_t>(level)];
            GraphicsSettings candidate = graphics;
            if (deep_rows_missing)
            {
                candidate.heat_haze = preset.heat_haze;
                candidate.embers = preset.embers;
            }
            if (far_rows_missing)
            {
                candidate.far_terrain = preset.far_terrain;
                candidate.far_terrain_detail = preset.far_terrain_detail;
            }
            if (effect_rows_missing)
            {
                candidate.effect_lighting = preset.effect_lighting;
                candidate.distortion_effects = preset.distortion_effects;
            }
            if (!graphics_matches_preset(candidate, level, settings.shadows_enabled,
                                         settings.sun_shafts))
                continue;
            graphics = candidate;
            break;
        }
    graphics.normalize();
}

void save_graphics_options(const Settings &settings, std::ostream &output)
{
    GraphicsSettings graphics = settings.graphics;
    graphics.normalize();
    for (const GraphicsOption &option : graphics_options())
    {
        output << option.key << '=';
        switch (option.storage)
        {
        case GraphicsOptionStorage::flag:
            output << (graphics.*(option.flag) ? 1 : 0);
            break;
        case GraphicsOptionStorage::quality:
            output << quality_names[static_cast<std::size_t>(
                std::clamp(static_cast<int>(graphics.*(option.quality)), 0, 4))];
            break;
        case GraphicsOptionStorage::focus_mode:
            output << focus_names[static_cast<std::size_t>(
                std::clamp(static_cast<int>(graphics.*(option.focus_mode)), 0, 2))];
            break;
        case GraphicsOptionStorage::scalar:
            output << graphics.*(option.scalar);
            break;
        case GraphicsOptionStorage::integer:
            output << graphics.*(option.integer);
            break;
        case GraphicsOptionStorage::far_distance:
            output << far_distance_names[static_cast<std::size_t>(
                std::clamp(static_cast<int>(graphics.*(option.far_distance)), 0, 6))];
            break;
        }
        output << '\n';
    }
}

} // namespace ui
} // namespace openminerz
