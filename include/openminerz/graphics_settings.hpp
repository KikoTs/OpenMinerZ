#pragma once

// Graphics options of the Enhanced ("Rebirth") presentation that arrived with
// the post-processing chain: screen-space shading, volumetric light, depth of
// field, colour controls and lens effects.
//
// PRESENTATION ONLY. Nothing here may reach the simulation or the network, and
// none of it applies to the Original ("Classic") presentation, which keeps the
// look of the 2011 game exactly: resolve_post_effects() returns a disabled
// chain for it.

namespace openminerz
{

struct Settings;

// One row = on/off plus a quality level. `off` skips every pass of the effect.
enum class EffectQuality
{
    off,
    low,
    medium,
    high,
    ultra
};

// How far the far terrain is drawn (openminerz/world/far_terrain.hpp). Seven
// choices, so a type of its own and not an EffectQuality.
enum class FarTerrainDistance
{
    off,
    m500,
    km1,
    km2,
    km4,
    km8,
    km12
};

enum class DepthOfFieldMode
{
    off,
    // Only while a weapon is brought up to the eye. Ordinary play is never
    // blurred, so nothing is harder to read.
    aiming,
    // A subtle focus on whatever the crosshair is on, all the time.
    always
};

// The player's stored choices (Settings::graphics). Every field has a row in
// the settings menu; see graphics_options() in graphics_options.hpp.
struct GraphicsSettings final
{
    // --- Quality: set together by the Low/Medium/High/Ultra preset. ---
    // Screen-space ambient occlusion (GTAO) layered over the baked corner
    // shading. The two are combined with min(), never multiplied.
    EffectQuality ambient_shading{EffectQuality::high};
    // Short screen-space sun shadows that ground creatures, props and edges.
    bool contact_shadows{true};
    // Steps of the shadowed fog march behind Settings::sun_shafts (never off:
    // that switch is the on/off).
    EffectQuality volumetric_quality{EffectQuality::high};
    // Procedural cloud layer of the sky. Off keeps the painted sky only.
    EffectQuality cloud_quality{EffectQuality::high};
    DepthOfFieldMode depth_of_field{DepthOfFieldMode::aiming};
    // Sun glare and ghosts when the sun is in view.
    bool lens_flare{true};
    // How far from the camera sun shadows are drawn, in blocks.
    int shadow_distance{150};
    // The deep underground (src/hell_atmosphere.hpp). The air shimmers above
    // lava and hot ground.
    bool heat_haze{true};
    // Embers rising from lava and ash drifting through the deep layer.
    EffectQuality embers{EffectQuality::high};
    // Far terrain (openminerz/world/far_terrain.hpp): how far the landscape
    // beyond the detailed window is drawn. Off, 500 m, then 1, 2, 4, 8 and
    // 12 km.
    FarTerrainDistance far_terrain{FarTerrainDistance::km4};
    // How finely it is drawn (never off: the row above is the on/off).
    EffectQuality far_terrain_detail{EffectQuality::high};
    // The effects layer (openminerz/vfx.hpp). Explosions, flames and sparks
    // light the terrain, creatures and smoke around them.
    bool effect_lighting{true};
    // Shockwaves and hot air bend the picture behind them.
    bool distortion_effects{true};

    // --- Look: a matter of taste, never changed by a preset. ---
    // How hard a nearby explosion shakes the camera. 0 is off.
    float explosion_shake{0.60F};
    // A nearby explosion briefly brightens the whole picture.
    bool explosion_flash{true};
    float depth_of_field_strength{0.50F};
    float bloom_strength{1.00F};
    // 0 is off. Camera and per-object blur along the motion of each pixel.
    float motion_blur{0.0F};
    // Contrast-adaptive sharpening after edge smoothing. 0 is off.
    float sharpening{0.30F};
    float contrast{1.00F};
    float saturation{1.00F};
    // -1 cool .. +1 warm white balance.
    float temperature{0.0F};
    float vignette{0.20F};
    float film_grain{0.0F};
    float chromatic_aberration{0.0F};
    // 0 hard-edged .. 1 wide penumbra. No cost.
    float shadow_softness{0.50F};
    // -1 softer .. +1 crisper: shifts which mip level the terrain samples.
    float texture_detail{0.0F};
    // How strongly the deep underground is lit by its own glow and its lava.
    float deep_light{1.00F};

    void normalize() noexcept;
};

// What the renderer consumes for one frame. Derived from Settings by
// resolve_post_effects(); never stored. A value of zero or false means the
// pass is skipped entirely, not multiplied by zero.
struct PostEffects final
{
    // False for the Original presentation: the renderer then runs exactly the
    // pipeline it always had.
    bool enabled{};

    bool bloom{};
    float bloom_strength{1.0F};

    // Ambient occlusion: horizon slices and steps per side (0 slices = off).
    int ao_slices{};
    int ao_steps{};
    bool contact_shadows{};
    int contact_steps{};

    // Volumetric light: march steps at half resolution (0 = off).
    int volumetric_steps{};

    // 0 none, 1..4 cloud detail.
    int cloud_quality{};

    DepthOfFieldMode depth_of_field{DepthOfFieldMode::off};
    float depth_of_field_strength{};
    // Taps of the bokeh gather (derived from the quality preset's level).
    int depth_of_field_taps{};

    float motion_blur{};
    int motion_blur_taps{};

    bool fxaa{};
    float sharpening{};
    bool lens_flare{};

    float contrast{1.0F};
    float saturation{1.0F};
    float temperature{};
    float vignette{};
    float film_grain{};
    float chromatic_aberration{};

    float shadow_softness{0.5F};
    float texture_detail{};
    bool reduced_motion{};

    // The deep underground (src/hell_atmosphere.hpp). Zero deep_light keeps the
    // older flat fill; the steps are those of the march that puts the lava's
    // glow in the air, zero embers draws none.
    float deep_light{};
    int deep_air_steps{};
    bool heat_haze{};
    int ember_count{};

    // The effects layer (openminerz/vfx.hpp, src/vfx/vfx_renderer.hpp): the
    // most effect lights on the lit pass (0 = none), whether shockwaves and
    // heat bend the picture, how hard explosions shake the camera and brighten
    // the picture (0 = not at all), and the gas buffer's resolution divisor.
    int effect_lights{};
    bool effect_distortion{};
    float effect_shake{};
    float effect_flash{};
    int effect_resolution{2};
};

[[nodiscard]] PostEffects resolve_post_effects(const Settings &settings) noexcept;

// What the far-terrain system is asked to draw (the fields of
// world::FarTerrainSettings, in the same order). Presentation only, and only
// the Enhanced presentation: the Original one resolves to a distance of zero,
// which is off, and keeps its short draw distance and fog exactly.
struct FarTerrainChoice final
{
    // Blocks; 0 is off.
    int distance{};
    // Width of the finest cell in blocks.
    int base_cell{1};
    // How many of its own cells each level of detail reaches.
    int ring_cells{256};

    bool operator==(const FarTerrainChoice &) const noexcept = default;
};
[[nodiscard]] FarTerrainChoice resolve_far_terrain(const Settings &settings) noexcept;
// The distance a far-terrain choice stands for, in blocks (0 for off).
[[nodiscard]] int far_terrain_blocks(FarTerrainDistance distance) noexcept;

// Values of one frame that only the application knows and the Enhanced
// presentation's sky, fog and lens react to. Presentation only.
struct PostView final
{
    // 0 at the hip .. 1 with the weapon fully at the eye.
    float aim_progress{};
    // Baked skylight at the eye, 0 sealed cave .. 1 open sky.
    float eye_sky_light{1.0F};
    // BiomeAtmosphere::storminess at the player.
    float storminess{};
    // 0..1 while the frame's "sun" direction and light are really the moon's
    // (post::moonlight): the sky then draws no sun disc there.
    float moonlight{};
};

// The preset-controlled half of GraphicsSettings for level 0 (Low) .. 3
// (Ultra). Level 2 (High) equals the defaults, so a fresh install reads HIGH.
void apply_graphics_preset(GraphicsSettings &graphics, int level) noexcept;
// An option that cannot act (the shadow rows with Shadows off, the shaft
// quality with Light shafts off) draws nothing, so its stored value must not be
// able to turn a preset into Custom.
[[nodiscard]] bool graphics_matches_preset(const GraphicsSettings &graphics, int level,
                                           bool shadows_enabled, bool light_shafts) noexcept;

} // namespace openminerz
