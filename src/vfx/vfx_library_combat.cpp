// Weapon, dragon, mining and creature recipes (openminerz/vfx_combat.hpp), on
// the effects layer of openminerz/vfx.hpp. Sizes are billboard half-extents in
// blocks, times in seconds, colours linear; an effect's +Y is the direction it
// is spawned with (a surface normal, a barrel, a flight path).
//
// Smoke, fire and explosions themselves live in vfx_library.cpp; a dragon's
// detonation is that file's explosion, scaled, plus the extras here.

#include "openminerz/vfx_combat.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace openminerz::vfx
{
namespace
{

constexpr Color soot{0.085F, 0.080F, 0.075F};
constexpr Color dark_smoke{0.17F, 0.165F, 0.16F};
constexpr Color grey_smoke{0.36F, 0.355F, 0.35F};
constexpr Color pale_smoke{0.58F, 0.575F, 0.57F};
constexpr Color frost_mist{0.80F, 0.88F, 0.96F};
constexpr Color frost_thin{0.66F, 0.76F, 0.90F};
// The body of burning-cold gas: a deep blue, so that in daylight an iceball is
// a blue-hearted comet and not a white cloud.
constexpr Color frost_deep{0.16F, 0.30F, 0.58F};
constexpr Color spark_hot{1.0F, 0.80F, 0.46F};
constexpr Color spark_cool{1.0F, 0.26F, 0.035F};
constexpr Color ember_dying{0.75F, 0.07F, 0.01F};
constexpr Color ice_hot{0.86F, 0.95F, 1.0F};
constexpr Color ice_cool{0.30F, 0.62F, 1.0F};
constexpr Color ice_dying{0.08F, 0.22F, 0.60F};

[[nodiscard]] EmitterDesc smoke_base(Color young, Color old, float peak_alpha)
{
    EmitterDesc emitter;
    emitter.material = Material::smoke;
    emitter.color = ColorCurve::keys({{0.0F, young}, {1.0F, old}});
    emitter.alpha =
        Curve::keys({{0.0F, 0.0F}, {0.09F, peak_alpha}, {0.5F, peak_alpha * 0.62F}, {1.0F, 0.0F}});
    emitter.erosion = Curve::keys({{0.0F, 0.04F}, {0.45F, 0.30F}, {1.0F, 0.86F}});
    emitter.spin = {-0.5F, 0.5F};
    emitter.frame_rate = 10.0F;
    return emitter;
}

// A hot streak that falls, skips off the ground and cools. `cold` makes it a
// splinter of ice instead.
[[nodiscard]] EmitterDesc spark_base(bool cold = false)
{
    EmitterDesc emitter;
    emitter.material = Material::spark;
    emitter.gravity = 10.0F;
    emitter.drag = 0.5F;
    emitter.collide = true;
    emitter.bounce = cold ? 0.45F : 0.35F;
    emitter.friction = 0.35F;
    emitter.size = {0.012F, 0.022F};
    emitter.stretch = 0.03F;
    emitter.emissive = Curve::keys({{0.0F, 11.0F}, {0.6F, 5.0F}, {1.0F, 1.2F}});
    // (A cold emitter's colours are authored warm: the layer swaps red and
    // blue for it, which turns this same ramp into white-blue to deep blue.)
    emitter.color = ColorCurve::keys({{0.0F, spark_hot}, {1.0F, spark_cool}});
    emitter.alpha = Curve::keys({{0.0F, 1.0F}, {0.75F, 1.0F}, {1.0F, 0.0F}});
    emitter.cold = cold;
    return emitter;
}

// A fragment of the block itself: arcs, bounces, comes to rest and is gone.
[[nodiscard]] EmitterDesc chip_base()
{
    EmitterDesc emitter;
    emitter.material = Material::debris;
    emitter.gravity = 17.0F;
    emitter.drag = 0.2F;
    emitter.collide = true;
    emitter.bounce = 0.34F;
    emitter.friction = 0.5F;
    emitter.spin = {-14.0F, 14.0F};
    emitter.alpha = Curve::keys({{0.0F, 1.0F}, {0.84F, 1.0F}, {1.0F, 0.0F}});
    return emitter;
}

[[nodiscard]] EmitterDesc glow_base(Color color, float emissive)
{
    EmitterDesc emitter;
    emitter.material = Material::glow;
    emitter.speed = {0.0F, 0.0F};
    emitter.color = ColorCurve::constant(color);
    emitter.emissive = Curve::constant(emissive);
    emitter.alpha = Curve::keys({{0.0F, 1.0F}, {0.4F, 0.6F}, {1.0F, 0.0F}});
    return emitter;
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

// What a surface gives off under one ordinary tool hit.
struct SurfaceLook final
{
    int chips{};
    Range chip_size{0.018F, 0.042F};
    Range chip_speed{1.2F, 4.0F};
    // Leaves and paper-light flakes flutter instead of dropping.
    float chip_gravity{17.0F};
    float chip_drag{0.2F};
    float chip_bounce{0.34F};
    int sparks{};
    Range spark_speed{3.0F, 7.0F};
    bool cold_sparks{};
    Color spark_from{spark_hot};
    Color spark_to{spark_cool};
    // Peak gain of the contact flash (0 = none).
    float flash{};
    // Dust: emission and size against the plain recipe.
    float dust{1.0F};
    float dust_scale{1.0F};
    // Hard enough that a bullet strikes sparks off it.
    bool hard{};
};

[[nodiscard]] SurfaceLook surface_look(Surface surface) noexcept
{
    SurfaceLook look;
    switch (surface)
    {
    case Surface::stone:
        look.chips = 7;
        look.sparks = 3;
        look.hard = true;
        break;
    case Surface::dirt:
        look.chips = 6;
        look.chip_size = {0.020F, 0.045F};
        look.chip_speed = {1.0F, 3.2F};
        look.chip_bounce = 0.18F;
        look.dust = 1.4F;
        look.dust_scale = 1.1F;
        break;
    case Surface::sand:
        look.chips = 4;
        look.chip_size = {0.012F, 0.026F};
        look.chip_speed = {1.0F, 2.6F};
        look.chip_bounce = 0.10F;
        look.dust = 2.0F;
        look.dust_scale = 1.25F;
        break;
    case Surface::snow:
        look.chips = 5;
        look.chip_size = {0.014F, 0.030F};
        look.chip_speed = {1.0F, 2.8F};
        look.chip_bounce = 0.08F;
        look.dust = 2.0F;
        look.dust_scale = 1.15F;
        break;
    case Surface::ice:
        look.chips = 9;
        look.chip_size = {0.012F, 0.034F};
        look.chip_speed = {1.5F, 4.6F};
        look.chip_bounce = 0.46F;
        look.sparks = 4;
        look.cold_sparks = true;
        look.dust = 0.6F;
        look.dust_scale = 0.9F;
        break;
    case Surface::wood:
        look.chips = 8;
        look.chip_size = {0.014F, 0.040F};
        look.chip_speed = {1.2F, 3.8F};
        look.chip_bounce = 0.28F;
        look.dust = 0.7F;
        look.dust_scale = 0.9F;
        break;
    case Surface::leaves:
        look.chips = 7;
        look.chip_size = {0.016F, 0.036F};
        look.chip_speed = {0.8F, 2.2F};
        look.chip_gravity = 5.0F;
        look.chip_drag = 2.6F;
        look.chip_bounce = 0.05F;
        look.dust = 0.4F;
        look.dust_scale = 0.8F;
        break;
    case Surface::metal:
        look.chips = 2;
        look.chip_size = {0.010F, 0.020F};
        look.chip_speed = {1.5F, 4.0F};
        look.sparks = 14;
        look.spark_speed = {3.0F, 8.5F};
        look.flash = 1.0F;
        look.dust = 0.3F;
        look.dust_scale = 0.7F;
        look.hard = true;
        break;
    case Surface::ore:
        look.chips = 7;
        look.sparks = 9;
        look.spark_speed = {3.0F, 7.5F};
        look.flash = 0.6F;
        look.hard = true;
        break;
    case Surface::crystal:
        look.chips = 8;
        look.chip_size = {0.014F, 0.036F};
        look.chip_speed = {1.5F, 4.4F};
        look.chip_bounce = 0.42F;
        look.sparks = 7;
        look.spark_from = {1.0F, 0.42F, 0.30F};
        look.spark_to = {0.9F, 0.06F, 0.04F};
        look.flash = 0.5F;
        look.dust = 0.5F;
        look.dust_scale = 0.9F;
        look.hard = true;
        break;
    case Surface::lava:
        look.sparks = 12;
        look.spark_speed = {1.5F, 4.5F};
        look.flash = 0.7F;
        look.dust = 0.0F;
        break;
    case Surface::slime:
        look.chips = 7;
        look.chip_size = {0.020F, 0.040F};
        look.chip_speed = {1.0F, 2.8F};
        look.chip_bounce = 0.05F;
        look.dust = 0.4F;
        look.dust_scale = 0.8F;
        break;
    case Surface::glass:
        look.chips = 10;
        look.chip_size = {0.008F, 0.026F};
        look.chip_speed = {1.5F, 4.8F};
        look.chip_bounce = 0.44F;
        look.sparks = 3;
        look.cold_sparks = true;
        look.spark_from = {1.0F, 0.95F, 0.86F};
        look.spark_to = {1.0F, 0.8F, 0.6F};
        look.dust = 0.3F;
        look.dust_scale = 0.7F;
        break;
    case Surface::count:
        break;
    }
    return look;
}

constexpr std::array<std::string_view, static_cast<std::size_t>(Surface::count)> surface_names{
    "stone", "dirt", "sand", "snow", "ice", "wood", "leaves", "metal",
    "ore", "crystal", "lava", "slime", "glass"};
constexpr std::array<std::string_view, static_cast<std::size_t>(Strike::count)> strike_names{
    "tool", "shatter", "bullet", "bolt", "place"};

[[nodiscard]] EffectDesc make_surface_strike(std::string_view name, Strike strike, Surface surface)
{
    const SurfaceLook look = surface_look(surface);
    EffectDesc effect;
    effect.name = name;
    effect.cull_distance = strike == Strike::shatter ? 110.0F : 80.0F;
    effect.lod_start = 18.0F;
    effect.lod_end = 60.0F;
    effect.lod_floor = 0.35F;
    effect.max_particles = strike == Strike::shatter ? 96 : 48;
    effect.priority = strike == Strike::shatter ? 1 : 0;

    int chips = look.chips;
    Range size = look.chip_size;
    Range speed = look.chip_speed;
    int sparks = look.sparks;
    float flash = look.flash;
    switch (strike)
    {
    case Strike::tool:
    case Strike::count:
        break;
    case Strike::shatter:
        // The whole block comes apart: several times the fragments, larger
        // ones among them, out of the cell in every direction.
        chips = chips * 3 + 6;
        size = {size.min * 1.3F, size.max * 1.9F};
        speed = {0.8F, std::max(speed.max, 3.4F) * 1.05F};
        sparks += sparks / 2;
        break;
    case Strike::bullet:
        // A round is small and fast: fewer, finer chips thrown harder, and a
        // flash of sparks off anything hard.
        chips = chips > 0 ? std::max(2, chips * 2 / 3) : 0;
        size = {size.min * 0.8F, size.max * 0.8F};
        speed = {2.0F, speed.max * 1.7F};
        if (look.hard)
        {
            sparks += 6;
            flash = std::max(flash, 0.8F);
        }
        break;
    case Strike::bolt:
        // The bolt's own flash and molten sparks are effects::bolt_impact(),
        // in the bolt's colour; this is only what the surface gives up.
        chips = chips > 0 ? std::max(2, chips / 2) : 0;
        speed = {1.5F, speed.max * 1.3F};
        sparks = 0;
        flash = 0.0F;
        break;
    case Strike::place:
        // A block set down: a couple of crumbs, nothing that reads as damage.
        chips = std::min(chips, 2);
        size = {size.min, std::min(size.max, 0.026F)};
        speed = {0.4F, 1.4F};
        sparks = 0;
        flash = 0.0F;
        break;
    }

    if (chips > 0)
    {
        EmitterDesc chip = chip_base();
        chip.burst = chips;
        chip.size = size;
        chip.speed = speed;
        chip.gravity = look.chip_gravity;
        chip.drag = look.chip_drag;
        chip.bounce = look.chip_bounce;
        if (strike == Strike::shatter)
        {
            chip.shape = Shape::sphere;
            chip.shape_radius = 0.36F;
            chip.spread = 0.82F;
            chip.velocity_bias = {0.0F, 1.7F, 0.0F};
            chip.life = {1.9F, 3.0F};
        }
        else
        {
            chip.offset = {0.0F, 0.05F, 0.0F};
            chip.spread = strike == Strike::place ? 0.5F : 0.36F;
            chip.velocity_bias = {0.0F, strike == Strike::place ? 0.4F : 0.9F, 0.0F};
            chip.life = {1.1F, 1.8F};
        }
        effect.emitters.push_back(chip);
    }
    if (strike == Strike::shatter && look.chips > 0)
    {
        // A few real lumps of it that tumble away and lie there a moment.
        EmitterDesc lump = chip_base();
        lump.burst = 4;
        lump.shape = Shape::sphere;
        lump.shape_radius = 0.26F;
        lump.size = {std::max(0.05F, look.chip_size.max * 1.5F),
                     std::max(0.085F, look.chip_size.max * 2.6F)};
        lump.speed = {0.9F, 3.0F};
        lump.spread = 0.7F;
        lump.velocity_bias = {0.0F, 1.9F, 0.0F};
        lump.gravity = look.chip_gravity;
        lump.drag = look.chip_drag;
        lump.bounce = look.chip_bounce;
        lump.spin = {-9.0F, 9.0F};
        lump.life = {2.3F, 3.4F};
        effect.emitters.push_back(lump);
    }
    if (sparks > 0)
    {
        EmitterDesc spark = spark_base(look.cold_sparks);
        spark.burst = sparks;
        spark.offset = {0.0F, 0.03F, 0.0F};
        spark.life = {0.14F, 0.46F};
        spark.speed = {look.spark_speed.min, look.spark_speed.max};
        spark.spread = strike == Strike::shatter ? 0.8F : 0.42F;
        spark.size = {0.006F, 0.012F};
        spark.color = ColorCurve::keys({{0.0F, look.spark_from}, {1.0F, look.spark_to}});
        if (surface == Surface::lava)
        {
            // Molten droplets: heavier, slower, and they do not bounce.
            spark.life = {0.4F, 0.9F};
            spark.size = {0.012F, 0.024F};
            spark.bounce = 0.0F;
            spark.stretch = 0.02F;
        }
        effect.emitters.push_back(spark);
    }
    if (surface == Surface::lava)
    {
        // Whatever touches lava, it hisses: a spit of pale vapour.
        EmitterDesc hiss = smoke_base(pale_smoke, grey_smoke, 0.34F);
        hiss.burst = 2;
        hiss.offset = {0.0F, 0.06F, 0.0F};
        hiss.life = {0.5F, 0.9F};
        hiss.speed = {0.5F, 1.4F};
        hiss.spread = 0.3F;
        hiss.drag = 2.6F;
        hiss.buoyancy = 1.1F;
        hiss.size = {0.06F, 0.10F};
        hiss.size_over_life = Curve::line(1.0F, 3.6F);
        effect.emitters.push_back(hiss);
    }
    if (flash > 0.0F)
    {
        const Color color = look.cold_sparks ? ice_hot : look.spark_from;
        EmitterDesc pop = glow_base(color, 7.0F * flash);
        pop.burst = 1;
        pop.offset = {0.0F, 0.05F, 0.0F};
        pop.life = {0.06F, 0.08F};
        pop.size = {0.07F, 0.10F};
        pop.size_over_life = Curve::line(0.7F, 1.9F);
        effect.emitters.push_back(pop);
        LightDesc light;
        light.color = color;
        light.radius = 3.6F;
        light.duration = 0.09F;
        light.intensity = Curve::line(1.1F * flash, 0.0F);
        light.offset = {0.0F, 0.12F, 0.0F};
        effect.lights.push_back(light);
    }
    return effect;
}

[[nodiscard]] EffectDesc make_strike_dust(std::string_view name, Strike strike)
{
    EffectDesc effect;
    effect.name = name;
    effect.cull_distance = 90.0F;
    effect.lod_start = 20.0F;
    effect.lod_end = 60.0F;
    effect.max_particles = strike == Strike::shatter ? 40 : 20;
    // Authored near white: the block's own dust colour is the tint.
    EmitterDesc puff = smoke_base({0.96F, 0.96F, 0.96F}, {0.84F, 0.84F, 0.84F}, 0.42F);
    puff.turbulence = 0.9F;
    puff.turbulence_scale = 1.3F;
    switch (strike)
    {
    case Strike::tool:
    case Strike::count:
        puff.burst = 3;
        puff.offset = {0.0F, 0.07F, 0.0F};
        puff.life = {0.6F, 1.0F};
        puff.speed = {0.3F, 1.0F};
        puff.spread = 0.55F;
        puff.drag = 3.0F;
        puff.gravity = 0.2F;
        puff.size = {0.06F, 0.10F};
        puff.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.3F, 2.6F}, {1.0F, 3.8F}});
        break;
    case Strike::shatter:
        // The dust of the whole block, born all through its cell, that hangs
        // where the block was for a second and a half.
        puff = smoke_base({0.96F, 0.96F, 0.96F}, {0.84F, 0.84F, 0.84F}, 0.50F);
        puff.burst = 9;
        puff.shape = Shape::sphere;
        puff.shape_radius = 0.34F;
        puff.life = {1.1F, 1.8F};
        puff.speed = {0.2F, 0.9F};
        puff.spread = 1.0F;
        puff.drag = 2.4F;
        puff.gravity = 0.12F;
        puff.turbulence = 0.7F;
        puff.turbulence_scale = 1.6F;
        puff.size = {0.16F, 0.26F};
        puff.size_over_life = Curve::keys({{0.0F, 0.8F}, {0.25F, 1.9F}, {1.0F, 2.9F}});
        break;
    case Strike::bullet:
        // A sharp jet straight off the face that mushrooms.
        puff.burst = 4;
        puff.offset = {0.0F, 0.05F, 0.0F};
        puff.life = {0.55F, 0.95F};
        puff.speed = {0.8F, 2.6F};
        puff.spread = 0.30F;
        puff.drag = 4.2F;
        puff.size = {0.04F, 0.07F};
        puff.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.25F, 3.2F}, {1.0F, 5.0F}});
        break;
    case Strike::bolt:
        // Vapour off a burnt face: thin, quick, rising.
        puff = smoke_base({0.96F, 0.96F, 0.96F}, {0.84F, 0.84F, 0.84F}, 0.30F);
        puff.burst = 3;
        puff.offset = {0.0F, 0.05F, 0.0F};
        puff.life = {0.5F, 0.85F};
        puff.speed = {0.4F, 1.3F};
        puff.spread = 0.35F;
        puff.drag = 3.0F;
        puff.buoyancy = 0.9F;
        puff.size = {0.04F, 0.07F};
        puff.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.3F, 2.8F}, {1.0F, 4.2F}});
        break;
    case Strike::place:
        // A low ring squeezed out from under the block as it seats.
        puff = smoke_base({0.96F, 0.96F, 0.96F}, {0.84F, 0.84F, 0.84F}, 0.28F);
        puff.burst = 7;
        puff.shape = Shape::ring;
        puff.shape_radius = 0.42F;
        puff.life = {0.45F, 0.75F};
        puff.speed = {0.5F, 1.2F};
        puff.spread = 0.12F;
        puff.drag = 4.0F;
        puff.size = {0.07F, 0.11F};
        puff.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.4F, 2.2F}, {1.0F, 2.8F}});
        break;
    }
    effect.emitters.push_back(puff);
    return effect;
}

// ---------------------------------------------------------------------------
// The dragon
// ---------------------------------------------------------------------------

// The fireball in flight. Everything rides with the head (inherit_velocity
// near one) except what is meant to fall behind it: the roiling heart is
// reborn at the centre several dozen times a second, white-hot, and is pushed
// outward as it cools, so the middle is always the hottest thing in the frame
// and the rim is the oldest, darkest, most torn gas. Flame streams off the
// back and shrinks away (FireTrail), each tongue leaving a puff of smoke that
// swells (SmokeTrail); embers are shed all along the path.
[[nodiscard]] EffectDesc make_fireball_flight(bool ice)
{
    EffectDesc effect;
    effect.name = ice ? "dragon_iceball" : "dragon_fireball";
    effect.priority = 4;
    effect.max_particles = 560;
    effect.lod_start = 70.0F;
    effect.lod_end = 260.0F;
    effect.lod_floor = 0.5F;
    effect.cull_distance = 420.0F;
    const float radius = fireball_drawn_radius;

    // 0: the heart.
    EmitterDesc heart;
    heart.material = Material::smoke;
    heart.cold = ice;
    heart.rate = 40.0F;
    heart.loop = true;
    heart.shape = Shape::sphere;
    heart.shape_radius = radius * 0.20F;
    heart.inherit_velocity = 0.97F;
    heart.speed = {0.5F, 1.9F};
    heart.spread = 1.0F;
    heart.life = {0.20F, 0.34F};
    heart.drag = 2.0F;
    heart.turbulence = 2.6F;
    heart.turbulence_scale = 0.9F;
    heart.size = {radius * 0.34F, radius * 0.48F};
    heart.size_over_life = Curve::line(0.8F, 1.5F);
    heart.spin = {-2.2F, 2.2F};
    heart.alpha = Curve::keys({{0.0F, 0.0F}, {0.12F, 0.90F}, {0.6F, 0.75F}, {1.0F, 0.0F}});
    heart.temperature = Curve::keys({{0.0F, 2.6F}, {0.5F, 1.7F}, {1.0F, 1.0F}});
    heart.emissive = Curve::constant(1.15F);
    heart.erosion = Curve::line(0.0F, 0.5F);
    heart.frame_rate = 18.0F;
    heart.color = ColorCurve::constant(ice ? frost_deep : soot);
    effect.emitters.push_back(heart);

    // 1: the mantle around it, cooling to a dark, torn rim.
    EmitterDesc mantle;
    mantle.material = Material::smoke;
    mantle.cold = ice;
    mantle.rate = 34.0F;
    mantle.loop = true;
    mantle.shape = Shape::sphere;
    mantle.shape_radius = radius * 0.50F;
    mantle.inherit_velocity = 0.84F;
    mantle.speed = {0.6F, 2.2F};
    mantle.spread = 1.0F;
    mantle.life = {0.45F, 0.80F};
    mantle.drag = 1.2F;
    mantle.turbulence = 3.0F;
    mantle.turbulence_scale = 1.2F;
    mantle.size = {radius * 0.48F, radius * 0.68F};
    mantle.size_over_life = Curve::line(0.9F, 1.7F);
    mantle.spin = {-1.4F, 1.4F};
    mantle.alpha = Curve::keys({{0.0F, 0.0F}, {0.10F, 0.90F}, {0.6F, 0.7F}, {1.0F, 0.0F}});
    mantle.temperature =
        Curve::keys({{0.0F, 1.25F}, {0.30F, 0.70F}, {0.65F, 0.22F}, {1.0F, 0.0F}});
    mantle.emissive = Curve::constant(0.95F);
    mantle.erosion = Curve::keys({{0.0F, 0.05F}, {0.5F, 0.40F}, {1.0F, 0.92F}});
    mantle.frame_rate = 14.0F;
    mantle.color = ice ? ColorCurve::keys({{0.0F, frost_deep}, {1.0F, frost_thin}})
                       : ColorCurve::keys({{0.0F, soot}, {1.0F, dark_smoke}});
    effect.emitters.push_back(mantle);

    // 2: flame streaming off the back, which shrinks to nothing (billowing
    // gas, not upright tongues: it lies along the flight, whichever way that is)...
    EmitterDesc tail;
    tail.material = Material::smoke;
    tail.cold = ice;
    // Dense enough that neighbours overlap at flight speed (about 30 blocks a
    // second leaves one every quarter block): at 40 a second the tail was a
    // string of separate beads.
    tail.rate = 120.0F;
    tail.loop = true;
    tail.shape = Shape::sphere;
    tail.shape_radius = radius * 0.42F;
    tail.offset = {0.0F, -radius * 0.7F, 0.0F};
    tail.inherit_velocity = 0.36F;
    tail.speed = {0.5F, 2.0F};
    tail.spread = 1.0F;
    tail.life = {0.50F, 0.95F};
    tail.drag = 1.0F;
    tail.buoyancy = 1.0F;
    tail.turbulence = 2.4F;
    tail.turbulence_scale = 1.4F;
    tail.size = {radius * 0.62F, radius * 0.88F};
    tail.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.5F, 0.9F}, {1.0F, 0.45F}});
    tail.spin = {-1.6F, 1.6F};
    tail.alpha = Curve::keys({{0.0F, 0.0F}, {0.10F, 0.85F}, {0.7F, 0.55F}, {1.0F, 0.0F}});
    tail.temperature = Curve::keys({{0.0F, 1.6F}, {0.4F, 0.9F}, {1.0F, 0.2F}});
    tail.emissive = Curve::constant(0.95F);
    tail.erosion = Curve::keys({{0.0F, 0.0F}, {0.55F, 0.30F}, {1.0F, 0.80F}});
    tail.frame_rate = 16.0F;
    tail.color = ColorCurve::constant(ice ? frost_deep : soot);
    effect.emitters.push_back(tail);

    // 3: ...and breaks into smoke that swells, appearing as the flame goes.
    EmitterDesc smoke = ice ? smoke_base(frost_mist, frost_thin, 0.42F)
                            : smoke_base(soot, grey_smoke, 0.60F);
    smoke.rate = 60.0F;
    smoke.loop = true;
    smoke.shape = Shape::sphere;
    smoke.shape_radius = radius * 0.4F;
    smoke.offset = {0.0F, -radius * 1.2F, 0.0F};
    smoke.inherit_velocity = 0.10F;
    smoke.life = {2.2F, 3.6F};
    smoke.speed = {0.2F, 0.8F};
    smoke.spread = 1.0F;
    smoke.buoyancy = ice ? 0.15F : 0.5F;
    smoke.drag = 0.9F;
    smoke.turbulence = 1.2F;
    smoke.turbulence_scale = 2.5F;
    smoke.size = {radius * 0.52F, radius * 0.72F};
    smoke.size_over_life = Curve::keys({{0.0F, 0.8F}, {0.3F, 1.9F}, {1.0F, 3.3F}});
    smoke.alpha = ice ? Curve::keys({{0.0F, 0.0F}, {0.22F, 0.42F}, {0.5F, 0.28F}, {1.0F, 0.0F}})
                      : Curve::keys({{0.0F, 0.0F}, {0.22F, 0.60F}, {0.5F, 0.40F}, {1.0F, 0.0F}});
    effect.emitters.push_back(smoke);

    // 4: embers (splinters of ice) shed along the way.
    EmitterDesc embers = spark_base(ice);
    embers.rate = 54.0F;
    embers.loop = true;
    embers.shape = Shape::sphere;
    embers.shape_radius = radius * 0.6F;
    embers.inherit_velocity = 0.26F;
    embers.speed = {1.0F, 4.5F};
    embers.spread = 1.0F;
    embers.life = {0.7F, 1.9F};
    embers.gravity = ice ? 2.2F : 4.0F;
    embers.drag = 1.3F;
    embers.turbulence = 3.0F;
    embers.turbulence_scale = 1.0F;
    embers.bounce = 0.25F;
    embers.size = {0.012F, 0.026F};
    embers.stretch = 0.018F;
    embers.emissive = Curve::keys({{0.0F, 9.0F}, {0.6F, 4.0F}, {1.0F, 0.0F}});
    embers.color =
        ColorCurve::keys({{0.0F, spark_hot}, {0.4F, spark_cool}, {1.0F, ember_dying}});
    effect.emitters.push_back(embers);

    // 5 + 6: the halo that makes the head a light source: a hot inner one and
    // a wide faint one, both far softer than the head so neither shows an edge.
    EmitterDesc inner = glow_base({1.0F, 0.62F, 0.26F}, 2.2F);
    inner.cold = ice;
    inner.rate = 12.0F;
    inner.loop = true;
    inner.inherit_velocity = 1.0F;
    inner.life = {0.16F, 0.24F};
    inner.size = {radius * 1.25F, radius * 1.5F};
    inner.alpha = Curve::keys({{0.0F, 0.0F}, {0.5F, 0.22F}, {1.0F, 0.0F}});
    effect.emitters.push_back(inner);
    EmitterDesc outer = glow_base({1.0F, 0.42F, 0.12F}, 1.0F);
    outer.cold = ice;
    outer.rate = 9.0F;
    outer.loop = true;
    outer.inherit_velocity = 1.0F;
    outer.life = {0.22F, 0.30F};
    outer.size = {fireball_halo_radius * 0.92F, fireball_halo_radius * 1.08F};
    outer.alpha = Curve::keys({{0.0F, 0.0F}, {0.5F, 0.07F}, {1.0F, 0.0F}});
    effect.emitters.push_back(outer);

    // 7: the air around a fireball shimmers; around an iceball it does not.
    if (!ice)
    {
        EmitterDesc shimmer;
        shimmer.material = Material::heat;
        shimmer.rate = 7.0F;
        shimmer.loop = true;
        shimmer.shape = Shape::sphere;
        shimmer.shape_radius = radius * 0.4F;
        shimmer.inherit_velocity = 0.7F;
        shimmer.life = {0.5F, 0.8F};
        shimmer.speed = {0.0F, 0.6F};
        shimmer.size = {radius * 1.0F, radius * 1.3F};
        shimmer.size_over_life = Curve::line(1.0F, 1.7F);
        shimmer.alpha = Curve::keys({{0.0F, 0.0F}, {0.3F, 1.0F}, {1.0F, 0.0F}});
        shimmer.distortion = 0.007F;
        effect.emitters.push_back(shimmer);
    }

    LightDesc light;
    light.color = ice ? Color{0.30F, 0.68F, 1.0F} : Color{1.0F, 0.52F, 0.18F};
    light.radius = 17.0F;
    light.intensity = Curve::constant(ice ? 2.6F : 3.0F);
    light.flicker = 0.22F;
    effect.lights.push_back(light);
    return effect;
}

// The shot leaving the mouth: +Y is the way it is going.
[[nodiscard]] EffectDesc make_fireball_launch(bool ice)
{
    EffectDesc effect;
    effect.name = ice ? "dragon_iceball_launch" : "dragon_fireball_launch";
    effect.priority = 3;
    effect.max_particles = 110;
    effect.lod_start = 60.0F;
    effect.lod_end = 220.0F;
    effect.lod_floor = 0.45F;
    effect.cull_distance = 380.0F;

    EmitterDesc flash = glow_base({1.0F, 0.80F, 0.50F}, 9.0F);
    flash.cold = ice;
    flash.burst = 2;
    flash.life = {0.16F, 0.22F};
    flash.size = {1.5F, 1.9F};
    flash.size_over_life = Curve::line(0.7F, 2.4F);
    effect.emitters.push_back(flash);

    // A gout of flame thrown ahead of the shot.
    EmitterDesc gout;
    gout.material = Material::smoke;
    gout.cold = ice;
    gout.burst = 9;
    gout.shape = Shape::sphere;
    gout.shape_radius = 0.5F;
    gout.life = {0.35F, 0.65F};
    gout.speed = {4.0F, 11.0F};
    gout.spread = 0.26F;
    gout.drag = 3.4F;
    gout.turbulence = 2.4F;
    gout.turbulence_scale = 1.2F;
    gout.size = {0.55F, 0.85F};
    gout.size_over_life = Curve::line(0.7F, 1.9F);
    gout.alpha = Curve::keys({{0.0F, 0.9F}, {0.5F, 0.7F}, {1.0F, 0.0F}});
    gout.temperature = Curve::keys({{0.0F, 2.6F}, {0.4F, 1.3F}, {1.0F, 0.3F}});
    gout.emissive = Curve::constant(1.3F);
    gout.erosion = Curve::line(0.0F, 0.75F);
    gout.frame_rate = 16.0F;
    gout.color = ColorCurve::constant(ice ? frost_deep : soot);
    effect.emitters.push_back(gout);

    EmitterDesc sparks = spark_base(ice);
    sparks.burst = 40;
    sparks.life = {0.4F, 1.3F};
    sparks.speed = {7.0F, 22.0F};
    sparks.spread = 0.34F;
    sparks.gravity = ice ? 5.0F : 8.0F;
    sparks.drag = 1.0F;
    sparks.size = {0.014F, 0.028F};
    effect.emitters.push_back(sparks);

    // What the mouth is left breathing for a second.
    EmitterDesc breath = ice ? smoke_base(frost_mist, frost_thin, 0.36F)
                             : smoke_base(dark_smoke, grey_smoke, 0.46F);
    breath.burst = 6;
    breath.shape = Shape::sphere;
    breath.shape_radius = 0.6F;
    breath.life = {1.2F, 2.0F};
    breath.speed = {0.6F, 2.4F};
    breath.spread = 0.6F;
    breath.drag = 1.8F;
    breath.buoyancy = ice ? 0.0F : 0.6F;
    breath.turbulence = 1.4F;
    breath.turbulence_scale = 1.8F;
    breath.size = {0.5F, 0.8F};
    breath.size_over_life = Curve::line(1.0F, 2.6F);
    effect.emitters.push_back(breath);

    LightDesc light;
    light.color = ice ? Color{0.36F, 0.72F, 1.0F} : Color{1.0F, 0.58F, 0.22F};
    light.radius = 18.0F;
    light.duration = 0.30F;
    light.intensity = Curve::keys({{0.0F, 5.5F}, {0.3F, 2.6F}, {1.0F, 0.0F}});
    effect.lights.push_back(light);
    return effect;
}

// The glow gathering in the mouth through the wind-up. Its intensity is set
// every frame from vfx::dragon_charge().
[[nodiscard]] EffectDesc make_dragon_breath(bool ice)
{
    EffectDesc effect;
    effect.name = ice ? "dragon_frost_breath" : "dragon_fire_breath";
    effect.priority = 3;
    effect.max_particles = 80;
    effect.lod_start = 60.0F;
    effect.lod_end = 220.0F;
    effect.lod_floor = 0.5F;
    effect.cull_distance = 380.0F;

    EmitterDesc core = glow_base({1.0F, 0.70F, 0.34F}, 3.2F);
    core.cold = ice;
    core.rate = 16.0F;
    core.loop = true;
    core.inherit_velocity = 1.0F;
    core.life = {0.14F, 0.20F};
    core.size = {0.8F, 1.1F};
    core.alpha = Curve::keys({{0.0F, 0.0F}, {0.5F, 0.55F}, {1.0F, 0.0F}});
    effect.emitters.push_back(core);
    EmitterDesc halo = glow_base({1.0F, 0.42F, 0.12F}, 1.0F);
    halo.cold = ice;
    halo.rate = 10.0F;
    halo.loop = true;
    halo.inherit_velocity = 1.0F;
    halo.life = {0.20F, 0.28F};
    halo.size = {2.6F, 3.2F};
    halo.alpha = Curve::keys({{0.0F, 0.0F}, {0.5F, 0.14F}, {1.0F, 0.0F}});
    effect.emitters.push_back(halo);
    // Wisps of flame licking out between the teeth.
    EmitterDesc wisp;
    wisp.material = Material::flame;
    wisp.cold = ice;
    wisp.rate = 14.0F;
    wisp.loop = true;
    wisp.shape = Shape::sphere;
    wisp.shape_radius = 0.35F;
    wisp.inherit_velocity = 0.85F;
    wisp.life = {0.22F, 0.40F};
    wisp.speed = {0.6F, 2.2F};
    wisp.spread = 0.5F;
    wisp.buoyancy = 1.4F;
    wisp.turbulence = 2.0F;
    wisp.turbulence_scale = 0.8F;
    wisp.size = {0.30F, 0.46F};
    wisp.size_over_life = Curve::line(1.0F, 0.4F);
    wisp.alpha = Curve::keys({{0.0F, 0.0F}, {0.2F, 0.8F}, {1.0F, 0.0F}});
    wisp.temperature = Curve::line(2.2F, 0.9F);
    wisp.emissive = Curve::constant(1.6F);
    wisp.erosion = Curve::line(0.0F, 0.6F);
    wisp.frame_rate = 20.0F;
    wisp.color = ColorCurve::constant(ice ? frost_deep : soot);
    effect.emitters.push_back(wisp);
    EmitterDesc motes = spark_base(ice);
    motes.rate = 22.0F;
    motes.loop = true;
    motes.shape = Shape::sphere;
    motes.shape_radius = 0.5F;
    motes.inherit_velocity = 0.7F;
    motes.life = {0.3F, 0.7F};
    motes.speed = {1.0F, 4.0F};
    motes.spread = 0.8F;
    motes.gravity = 3.0F;
    motes.collide = false;
    motes.size = {0.010F, 0.020F};
    effect.emitters.push_back(motes);

    LightDesc light;
    light.color = ice ? Color{0.36F, 0.72F, 1.0F} : Color{1.0F, 0.58F, 0.22F};
    light.radius = 12.0F;
    light.intensity = Curve::constant(2.0F);
    light.flicker = 0.35F;
    effect.lights.push_back(light);
    return effect;
}

// Over the shared explosion: embers thrown high that rain back down, skip off
// the ground and go out one by one.
[[nodiscard]] EffectDesc make_fireball_embers()
{
    EffectDesc effect;
    effect.name = "dragon_fireball_embers";
    effect.priority = 2;
    effect.max_particles = 260;
    effect.lod_start = 44.0F;
    effect.lod_end = 150.0F;
    effect.cull_distance = 300.0F;

    EmitterDesc rain = spark_base();
    rain.burst = 80;
    rain.life = {1.2F, 2.9F};
    rain.speed = {5.0F, 18.0F};
    rain.spread = 0.52F;
    rain.gravity = 11.0F;
    rain.drag = 0.45F;
    rain.size = {0.012F, 0.026F};
    rain.sub_emitter = 1;
    // (80 embers x 9 a second x 0.2 s of trail: about 150 more, inside the
    // budget above with room for the level of detail to matter.)
    rain.sub_rate = 9.0F;
    effect.emitters.push_back(rain);
    EmitterDesc trail;
    trail.material = Material::spark;
    trail.sub_only = true;
    trail.life = {0.14F, 0.28F};
    trail.speed = {0.0F, 0.2F};
    trail.size = {0.008F, 0.013F};
    trail.emissive = Curve::line(4.0F, 0.0F);
    trail.color = ColorCurve::keys({{0.0F, {1.0F, 0.55F, 0.16F}}, {1.0F, ember_dying}});
    effect.emitters.push_back(trail);
    // A white-hot instant at the centre, over the explosion's own flash.
    EmitterDesc core = glow_base({1.0F, 0.90F, 0.72F}, 15.0F);
    core.burst = 1;
    core.life = {0.12F, 0.16F};
    core.size = {1.2F, 1.5F};
    core.size_over_life = Curve::line(0.8F, 2.6F);
    effect.emitters.push_back(core);
    return effect;
}

// One patch of ground left burning: tall flames that die back to a few licks
// over four seconds, the smoke and embers they give off, and their light.
[[nodiscard]] EffectDesc make_ground_fire()
{
    EffectDesc effect;
    effect.name = "dragon_ground_fire";
    effect.priority = 1;
    effect.max_particles = 90;
    effect.lod_start = 30.0F;
    effect.lod_end = 110.0F;
    effect.lod_floor = 0.3F;
    effect.cull_distance = 220.0F;

    const auto tongue = [](float duration, float rate, float size) {
        EmitterDesc flame;
        flame.material = Material::flame;
        flame.rate = rate;
        flame.duration = duration;
        flame.shape = Shape::sphere;
        flame.shape_radius = 0.45F;
        flame.life = {0.40F, 0.72F};
        flame.speed = {0.5F, 1.2F};
        flame.spread = 0.10F;
        flame.buoyancy = 1.6F;
        flame.turbulence = 0.9F;
        flame.turbulence_scale = 0.6F;
        flame.size = {0.30F * size, 0.46F * size};
        flame.size_over_life = Curve::keys({{0.0F, 0.75F}, {0.2F, 1.0F}, {1.0F, 0.5F}});
        flame.rotation = {-0.22F, 0.22F};
        flame.alpha = Curve::keys({{0.0F, 0.0F}, {0.14F, 0.95F}, {0.7F, 0.7F}, {1.0F, 0.0F}});
        flame.temperature = Curve::keys({{0.0F, 1.7F}, {0.5F, 1.2F}, {1.0F, 0.7F}});
        flame.emissive = Curve::constant(2.0F);
        flame.erosion = Curve::line(0.0F, 0.45F);
        flame.frame_rate = 22.0F;
        flame.color = ColorCurve::constant(soot);
        return flame;
    };
    effect.emitters.push_back(tongue(1.6F, 22.0F, 1.5F));
    effect.emitters.push_back(tongue(3.2F, 13.0F, 1.0F));
    effect.emitters.push_back(tongue(4.6F, 8.0F, 0.6F));
    // The bed of light the flames stand in.
    EmitterDesc bed = glow_base({1.0F, 0.46F, 0.12F}, 1.5F);
    bed.rate = 6.0F;
    bed.duration = 4.2F;
    bed.offset = {0.0F, 0.2F, 0.0F};
    bed.life = {0.30F, 0.42F};
    bed.size = {0.75F, 0.95F};
    bed.alpha = Curve::keys({{0.0F, 0.0F}, {0.5F, 0.20F}, {1.0F, 0.0F}});
    effect.emitters.push_back(bed);

    EmitterDesc smoke = smoke_base(soot, grey_smoke, 0.42F);
    smoke.rate = 3.5F;
    smoke.duration = 4.6F;
    smoke.offset = {0.0F, 0.5F, 0.0F};
    smoke.life = {1.8F, 3.0F};
    smoke.speed = {0.5F, 1.1F};
    smoke.spread = 0.15F;
    smoke.buoyancy = 0.7F;
    smoke.drag = 0.7F;
    smoke.turbulence = 1.0F;
    smoke.turbulence_scale = 1.6F;
    smoke.size = {0.22F, 0.34F};
    smoke.size_over_life = Curve::line(1.0F, 3.4F);
    effect.emitters.push_back(smoke);

    EmitterDesc embers = spark_base();
    embers.rate = 5.0F;
    embers.duration = 4.4F;
    embers.shape = Shape::sphere;
    embers.shape_radius = 0.25F;
    embers.life = {0.8F, 1.7F};
    embers.speed = {0.6F, 1.8F};
    embers.spread = 0.25F;
    embers.gravity = 0.0F;
    embers.buoyancy = 0.9F;
    embers.drag = 0.9F;
    embers.turbulence = 2.2F;
    embers.turbulence_scale = 0.7F;
    embers.collide = false;
    embers.size = {0.007F, 0.013F};
    embers.stretch = 0.01F;
    embers.emissive = Curve::keys({{0.0F, 7.0F}, {0.7F, 3.0F}, {1.0F, 0.0F}});
    effect.emitters.push_back(embers);

    EmitterDesc shimmer;
    shimmer.material = Material::heat;
    shimmer.rate = 3.0F;
    shimmer.duration = 3.6F;
    shimmer.offset = {0.0F, 0.4F, 0.0F};
    shimmer.life = {0.6F, 0.9F};
    shimmer.speed = {0.6F, 1.0F};
    shimmer.spread = 0.1F;
    shimmer.size = {0.35F, 0.5F};
    shimmer.size_over_life = Curve::line(1.0F, 1.9F);
    shimmer.alpha = Curve::keys({{0.0F, 0.0F}, {0.3F, 1.0F}, {1.0F, 0.0F}});
    shimmer.distortion = 0.004F;
    effect.emitters.push_back(shimmer);

    LightDesc light;
    light.color = {1.0F, 0.50F, 0.16F};
    light.radius = 6.5F;
    light.duration = 4.6F;
    light.intensity = Curve::keys({{0.0F, 0.0F}, {0.06F, 1.3F}, {0.5F, 0.7F}, {1.0F, 0.0F}});
    light.flicker = 0.45F;
    light.offset = {0.0F, 0.4F, 0.0F};
    effect.lights.push_back(light);
    return effect;
}

// The ice dragon's detonation (IceFlash + IcePuff + BigPuff + BigSnowBlast): a
// cold white flash, a cloud of freezing vapour with a blue heart, a shockwave,
// shards and splinters of ice, snow driven out along the ground and thrown up
// in clods, and the mist that hangs where it struck.
[[nodiscard]] EffectDesc make_iceball_burst()
{
    EffectDesc effect;
    effect.name = "dragon_iceball_burst";
    effect.priority = 2;
    effect.max_particles = 520;
    effect.lod_start = 44.0F;
    effect.lod_end = 150.0F;
    effect.lod_floor = 0.35F;
    effect.cull_distance = 340.0F;
    effect.shake = 0.60F;
    effect.shake_duration = 0.6F;
    effect.shake_radius = 36.0F;
    effect.flash = 0.42F;
    effect.flash_duration = 0.14F;
    effect.flash_radius = 30.0F;
    effect.flash_color = {0.70F, 0.86F, 1.0F};

    EmitterDesc flash = glow_base({1.0F, 0.92F, 0.78F}, 13.0F);
    flash.cold = true;
    flash.burst = 2;
    flash.life = {0.10F, 0.16F};
    flash.size = {2.0F, 2.6F};
    flash.size_over_life = Curve::line(0.8F, 2.3F);
    effect.emitters.push_back(flash);

    EmitterDesc vapour;
    vapour.material = Material::smoke;
    vapour.cold = true;
    vapour.burst = 18;
    vapour.shape = Shape::sphere;
    vapour.shape_radius = 0.45F;
    vapour.life = {1.1F, 1.9F};
    vapour.speed = {3.0F, 8.5F};
    vapour.spread = 1.0F;
    vapour.velocity_bias = {0.0F, 0.9F, 0.0F};
    vapour.drag = 3.3F;
    vapour.buoyancy = 0.5F;
    vapour.turbulence = 2.6F;
    vapour.turbulence_scale = 2.0F;
    vapour.collide = true;
    vapour.size = {0.70F, 1.10F};
    vapour.size_over_life = Curve::keys({{0.0F, 0.45F}, {0.14F, 1.25F}, {1.0F, 2.4F}});
    vapour.spin = {-0.9F, 0.9F};
    vapour.color = ColorCurve::keys({{0.0F, frost_mist}, {1.0F, frost_thin}});
    vapour.alpha = Curve::keys({{0.0F, 0.9F}, {0.55F, 0.7F}, {1.0F, 0.0F}});
    vapour.temperature = Curve::keys({{0.0F, 2.2F}, {0.16F, 1.2F}, {0.4F, 0.4F}, {0.6F, 0.0F}});
    vapour.emissive = Curve::constant(1.0F);
    vapour.erosion = Curve::keys({{0.0F, 0.0F}, {0.5F, 0.22F}, {1.0F, 0.86F}});
    vapour.frame_rate = 12.0F;
    effect.emitters.push_back(vapour);

    EmitterDesc shock;
    shock.material = Material::shockwave;
    shock.burst = 1;
    shock.life = {0.36F, 0.36F};
    shock.speed = {0.0F, 0.0F};
    shock.size = {0.9F, 0.9F};
    shock.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.5F, 7.0F}, {1.0F, 10.0F}});
    shock.alpha = Curve::keys({{0.0F, 1.0F}, {0.5F, 0.7F}, {1.0F, 0.0F}});
    shock.distortion = 0.03F;
    effect.emitters.push_back(shock);

    // Shards of ice (the caller passes the ice tile) that skitter and settle.
    EmitterDesc shards = chip_base();
    shards.burst = 22;
    shards.shape = Shape::sphere;
    shards.shape_radius = 0.6F;
    shards.life = {2.2F, 3.4F};
    shards.speed = {5.0F, 15.0F};
    shards.spread = 0.52F;
    shards.bounce = 0.46F;
    shards.friction = 0.3F;
    shards.size = {0.035F, 0.10F};
    effect.emitters.push_back(shards);

    EmitterDesc splinters = spark_base(true);
    splinters.burst = 64;
    splinters.life = {0.8F, 2.3F};
    splinters.speed = {5.0F, 18.0F};
    splinters.spread = 0.6F;
    splinters.gravity = 10.0F;
    splinters.size = {0.010F, 0.022F};
    splinters.color =
        ColorCurve::keys({{0.0F, spark_hot}, {0.5F, spark_cool}, {1.0F, ember_dying}});
    effect.emitters.push_back(splinters);

    // Snow driven out along the ground...
    EmitterDesc drift = smoke_base(frost_mist, frost_thin, 0.50F);
    drift.burst = 20;
    drift.shape = Shape::ring;
    drift.shape_radius = 0.9F;
    drift.offset = {0.0F, 0.25F, 0.0F};
    drift.life = {1.3F, 2.4F};
    drift.speed = {4.0F, 8.0F};
    drift.spread = 0.18F;
    drift.drag = 3.0F;
    drift.turbulence = 0.8F;
    drift.turbulence_scale = 2.0F;
    drift.collide = true;
    drift.size = {0.40F, 0.62F};
    drift.size_over_life = Curve::keys({{0.0F, 0.7F}, {0.3F, 2.0F}, {1.0F, 3.1F}});
    effect.emitters.push_back(drift);
    // ...and thrown up in clods that fall back.
    EmitterDesc clods = smoke_base({0.94F, 0.96F, 1.0F}, frost_mist, 0.85F);
    clods.burst = 16;
    clods.shape = Shape::sphere;
    clods.shape_radius = 0.5F;
    clods.life = {0.8F, 1.4F};
    clods.speed = {4.0F, 12.0F};
    clods.spread = 0.45F;
    clods.gravity = 11.0F;
    clods.drag = 0.6F;
    clods.collide = true;
    clods.size = {0.10F, 0.18F};
    clods.size_over_life = Curve::line(1.0F, 2.2F);
    effect.emitters.push_back(clods);

    EmitterDesc linger = smoke_base(frost_mist, frost_thin, 0.26F);
    linger.rate = 6.0F;
    linger.delay = 0.5F;
    linger.duration = 3.6F;
    linger.shape = Shape::sphere;
    linger.shape_radius = 1.4F;
    linger.life = {3.0F, 5.0F};
    linger.speed = {0.1F, 0.5F};
    linger.spread = 1.0F;
    linger.gravity = 0.25F;
    linger.drag = 0.9F;
    linger.turbulence = 0.6F;
    linger.turbulence_scale = 3.0F;
    linger.collide = true;
    linger.size = {0.8F, 1.2F};
    linger.size_over_life = Curve::line(1.0F, 2.2F);
    effect.emitters.push_back(linger);

    LightDesc blast;
    blast.color = {0.45F, 0.76F, 1.0F};
    blast.radius = 19.0F;
    blast.duration = 0.5F;
    blast.intensity = Curve::keys({{0.0F, 6.0F}, {0.12F, 3.2F}, {0.5F, 1.2F}, {1.0F, 0.0F}});
    blast.offset = {0.0F, 0.6F, 0.0F};
    effect.lights.push_back(blast);
    LightDesc afterglow;
    afterglow.color = {0.30F, 0.60F, 1.0F};
    afterglow.radius = 10.0F;
    afterglow.duration = 1.4F;
    afterglow.intensity = Curve::keys({{0.0F, 0.0F}, {0.3F, 0.9F}, {1.0F, 0.0F}});
    afterglow.offset = {0.0F, 1.0F, 0.0F};
    effect.lights.push_back(afterglow);
    return effect;
}

// One patch of frosted ground: cold mist creeping off it and ice that glints.
[[nodiscard]] EffectDesc make_ground_frost()
{
    EffectDesc effect;
    effect.name = "dragon_ground_frost";
    effect.max_particles = 60;
    effect.lod_start = 30.0F;
    effect.lod_end = 110.0F;
    effect.lod_floor = 0.3F;
    effect.cull_distance = 200.0F;

    EmitterDesc mist = smoke_base(frost_mist, frost_thin, 0.26F);
    mist.rate = 4.0F;
    mist.duration = 5.0F;
    mist.shape = Shape::ring;
    mist.shape_radius = 0.2F;
    mist.offset = {0.0F, 0.12F, 0.0F};
    mist.life = {1.6F, 2.6F};
    mist.speed = {0.3F, 0.8F};
    mist.spread = 0.05F;
    mist.gravity = 0.2F;
    mist.drag = 1.4F;
    mist.turbulence = 0.5F;
    mist.turbulence_scale = 1.6F;
    mist.collide = true;
    mist.size = {0.24F, 0.36F};
    mist.size_over_life = Curve::line(1.0F, 2.8F);
    effect.emitters.push_back(mist);

    EmitterDesc glint = glow_base({1.0F, 0.92F, 0.80F}, 5.0F);
    glint.cold = true;
    glint.rate = 5.0F;
    glint.duration = 5.0F;
    glint.shape = Shape::sphere;
    glint.shape_radius = 0.55F;
    glint.life = {0.25F, 0.5F};
    glint.size = {0.02F, 0.04F};
    glint.alpha = Curve::keys({{0.0F, 0.0F}, {0.3F, 1.0F}, {1.0F, 0.0F}});
    effect.emitters.push_back(glint);

    LightDesc light;
    light.color = {0.30F, 0.60F, 1.0F};
    light.radius = 5.0F;
    light.duration = 5.0F;
    light.intensity = Curve::keys({{0.0F, 0.0F}, {0.08F, 0.6F}, {1.0F, 0.0F}});
    effect.lights.push_back(light);
    return effect;
}

// A shot landing on the dragon (the original's FlashEffect): a hot flash off
// its hide, sparks and a knock of dark dust.
[[nodiscard]] EffectDesc make_dragon_hit()
{
    EffectDesc effect;
    effect.name = "dragon_hit";
    effect.priority = 1;
    effect.max_particles = 40;
    effect.lod_start = 50.0F;
    effect.lod_end = 200.0F;
    effect.lod_floor = 0.5F;
    effect.cull_distance = 340.0F;

    EmitterDesc flash = glow_base({1.0F, 0.84F, 0.60F}, 8.0F);
    flash.burst = 1;
    flash.life = {0.10F, 0.14F};
    flash.size = {0.40F, 0.55F};
    flash.size_over_life = Curve::line(0.7F, 2.2F);
    effect.emitters.push_back(flash);
    EmitterDesc sparks = spark_base();
    sparks.burst = 16;
    sparks.life = {0.25F, 0.7F};
    sparks.speed = {3.0F, 10.0F};
    sparks.spread = 0.7F;
    sparks.size = {0.010F, 0.020F};
    effect.emitters.push_back(sparks);
    EmitterDesc puff = smoke_base(dark_smoke, grey_smoke, 0.5F);
    puff.burst = 4;
    puff.life = {0.6F, 1.0F};
    puff.speed = {0.6F, 2.0F};
    puff.spread = 0.6F;
    puff.drag = 3.0F;
    puff.size = {0.20F, 0.32F};
    puff.size_over_life = Curve::line(1.0F, 3.0F);
    effect.emitters.push_back(puff);
    LightDesc light;
    light.color = {1.0F, 0.70F, 0.40F};
    light.radius = 9.0F;
    light.duration = 0.14F;
    light.intensity = Curve::line(2.4F, 0.0F);
    effect.lights.push_back(light);
    return effect;
}

// The dragon killed (the original's Flash + FirePuff + BigSmokePuff on the
// body): it bursts into flame, or into freezing vapour, where it was hit.
[[nodiscard]] EffectDesc make_dragon_death(bool ice)
{
    EffectDesc effect;
    effect.name = ice ? "dragon_death_ice" : "dragon_death";
    effect.priority = 3;
    effect.max_particles = 320;
    effect.lod_start = 70.0F;
    effect.lod_end = 260.0F;
    effect.lod_floor = 0.5F;
    effect.cull_distance = 420.0F;
    effect.flash = 0.3F;
    effect.flash_radius = 60.0F;
    effect.flash_color = ice ? Color{0.70F, 0.86F, 1.0F} : Color{1.0F, 0.86F, 0.66F};

    EmitterDesc flash = glow_base({1.0F, 0.86F, 0.62F}, 13.0F);
    flash.cold = ice;
    flash.burst = 2;
    flash.life = {0.14F, 0.20F};
    flash.size = {3.0F, 3.8F};
    flash.size_over_life = Curve::line(0.8F, 2.2F);
    effect.emitters.push_back(flash);

    EmitterDesc fire;
    fire.material = Material::smoke;
    fire.cold = ice;
    fire.burst = 22;
    fire.rate = 26.0F;
    fire.duration = 1.2F;
    fire.shape = Shape::sphere;
    fire.shape_radius = 1.6F;
    fire.life = {1.0F, 1.8F};
    fire.speed = {1.5F, 6.0F};
    fire.spread = 1.0F;
    fire.velocity_bias = {0.0F, 1.2F, 0.0F};
    fire.inherit_velocity = 0.5F;
    fire.drag = 2.6F;
    fire.buoyancy = 2.0F;
    fire.turbulence = 3.0F;
    fire.turbulence_scale = 2.2F;
    fire.size = {1.0F, 1.6F};
    fire.size_over_life = Curve::keys({{0.0F, 0.5F}, {0.16F, 1.2F}, {1.0F, 2.2F}});
    fire.spin = {-0.9F, 0.9F};
    fire.color = ice ? ColorCurve::keys({{0.0F, frost_mist}, {1.0F, frost_thin}})
                     : ColorCurve::keys({{0.0F, soot}, {1.0F, dark_smoke}});
    fire.alpha = Curve::keys({{0.0F, 0.95F}, {0.55F, 0.8F}, {1.0F, 0.0F}});
    fire.temperature = Curve::keys({{0.0F, 2.7F}, {0.16F, 1.7F}, {0.42F, 0.75F}, {0.68F, 0.0F}});
    fire.emissive = Curve::constant(1.0F);
    fire.erosion = Curve::keys({{0.0F, 0.0F}, {0.5F, 0.22F}, {1.0F, 0.86F}});
    fire.frame_rate = 12.0F;
    effect.emitters.push_back(fire);

    EmitterDesc smoke = ice ? smoke_base(frost_mist, frost_thin, 0.46F)
                            : smoke_base(soot, grey_smoke, 0.66F);
    smoke.rate = 14.0F;
    smoke.delay = 0.2F;
    smoke.duration = 3.0F;
    smoke.shape = Shape::sphere;
    smoke.shape_radius = 1.8F;
    smoke.inherit_velocity = 0.3F;
    smoke.life = {3.0F, 5.0F};
    smoke.speed = {0.6F, 2.2F};
    smoke.spread = 0.6F;
    smoke.buoyancy = ice ? 0.1F : 0.9F;
    smoke.drag = 1.0F;
    smoke.turbulence = 1.6F;
    smoke.turbulence_scale = 3.0F;
    smoke.size = {1.4F, 2.2F};
    smoke.size_over_life = Curve::keys({{0.0F, 0.8F}, {0.3F, 1.6F}, {1.0F, 2.6F}});
    effect.emitters.push_back(smoke);

    EmitterDesc sparks = spark_base(ice);
    sparks.burst = 70;
    sparks.shape = Shape::sphere;
    sparks.shape_radius = 1.2F;
    sparks.life = {0.9F, 2.6F};
    sparks.speed = {5.0F, 20.0F};
    sparks.spread = 1.0F;
    sparks.gravity = 9.0F;
    sparks.drag = 0.6F;
    sparks.size = {0.016F, 0.032F};
    effect.emitters.push_back(sparks);

    LightDesc blast;
    blast.color = ice ? Color{0.45F, 0.76F, 1.0F} : Color{1.0F, 0.62F, 0.30F};
    blast.radius = 26.0F;
    blast.duration = 0.9F;
    blast.intensity = Curve::keys({{0.0F, 6.0F}, {0.15F, 3.0F}, {1.0F, 0.0F}});
    effect.lights.push_back(blast);
    return effect;
}

// ---------------------------------------------------------------------------
// Weapons
// ---------------------------------------------------------------------------

// A muzzle flash, +Y down the barrel: a white-hot core, tongues of flame thrown
// forward, a wide soft bloom seed, the light on everything near, a few stray
// sparks and the smoke left hanging at the muzzle. It lasts as long as the
// original's one-frame flash model does at 60 Hz, but fades instead of
// vanishing; everything travels with the shooter while it lives.
[[nodiscard]] EffectDesc make_muzzle_flash(std::string_view name, Muzzle muzzle)
{
    const bool laser = muzzle == Muzzle::laser;
    const float size = muzzle == Muzzle::shotgun ? 1.7F : muzzle == Muzzle::rifle ? 1.3F : 1.0F;
    EffectDesc effect;
    effect.name = name;
    effect.priority = 1;
    effect.max_particles = 40;
    effect.lod_start = 30.0F;
    effect.lod_end = 90.0F;
    effect.lod_floor = 0.4F;
    effect.cull_distance = 160.0F;
    // An energy weapon is authored near white and takes the bolt's colour.
    const Color core_color = laser ? Color{1.0F, 1.0F, 1.0F} : Color{1.0F, 0.80F, 0.46F};
    const Color body_color = laser ? Color{1.0F, 1.0F, 1.0F} : Color{1.0F, 0.56F, 0.20F};

    EmitterDesc core = glow_base(core_color, laser ? 12.0F : 10.0F);
    core.burst = 1;
    core.inherit_velocity = 1.0F;
    core.offset = {0.0F, 0.02F, 0.0F};
    core.life = {0.045F, 0.060F};
    // Real sizes: the flash is seen from arm's length, where a hand's width
    // already fills a tenth of the screen. The core stays small so that the
    // flash is its tongues and its light, never a white disc.
    core.size = {0.022F * size, 0.030F * size};
    core.size_over_life = Curve::line(0.8F, 1.5F);
    core.alpha = Curve::keys({{0.0F, 1.0F}, {0.5F, 0.8F}, {1.0F, 0.0F}});
    effect.emitters.push_back(core);

    EmitterDesc bloom = glow_base(body_color, 3.0F);
    bloom.burst = 1;
    bloom.inherit_velocity = 1.0F;
    bloom.life = {0.055F, 0.070F};
    bloom.size = {0.10F * size, 0.13F * size};
    bloom.alpha = Curve::line(0.20F, 0.0F);
    effect.emitters.push_back(bloom);

    // Tongues: thick, very short streaks down the barrel's line.
    EmitterDesc tongue;
    tongue.material = Material::spark;
    tongue.burst = muzzle == Muzzle::shotgun ? 6 : muzzle == Muzzle::rifle ? 4 : 3;
    tongue.inherit_velocity = 1.0F;
    tongue.life = {0.025F, 0.045F};
    tongue.speed = {2.0F * size, 5.0F * size};
    tongue.spread = muzzle == Muzzle::shotgun ? 0.26F : 0.16F;
    tongue.drag = 6.0F;
    tongue.size = {0.010F * size, 0.017F * size};
    tongue.stretch = 0.012F;
    tongue.emissive = Curve::line(14.0F, 3.0F);
    tongue.color = ColorCurve::keys({{0.0F, core_color}, {1.0F, body_color}});
    tongue.alpha = Curve::line(1.0F, 0.0F);
    effect.emitters.push_back(tongue);

    if (!laser)
    {
        // A wisp, not a cloud: it must never hide what was just shot at.
        EmitterDesc smoke = smoke_base(pale_smoke, grey_smoke, 0.16F);
        smoke.burst = muzzle == Muzzle::shotgun ? 4 : 2;
        smoke.inherit_velocity = 0.6F;
        smoke.offset = {0.0F, 0.06F, 0.0F};
        smoke.life = {0.5F, 0.9F};
        smoke.speed = {0.4F, 1.2F * size};
        smoke.spread = 0.30F;
        smoke.drag = 3.0F;
        smoke.buoyancy = 0.5F;
        smoke.turbulence = 0.8F;
        smoke.turbulence_scale = 0.8F;
        smoke.size = {0.018F * size, 0.030F * size};
        smoke.size_over_life = Curve::line(1.0F, 4.0F);
        effect.emitters.push_back(smoke);
    }

    EmitterDesc stray = spark_base();
    stray.burst = muzzle == Muzzle::shotgun ? 4 : 2;
    stray.inherit_velocity = 0.8F;
    stray.life = {0.10F, 0.22F};
    stray.speed = {2.0F, 5.0F};
    stray.spread = 0.30F;
    stray.size = {0.003F, 0.006F};
    stray.stretch = 0.012F;
    if (laser)
        stray.color = ColorCurve::constant({1.0F, 1.0F, 1.0F});
    effect.emitters.push_back(stray);

    LightDesc light;
    light.color = laser ? Color{1.0F, 1.0F, 1.0F} : Color{1.0F, 0.70F, 0.36F};
    light.radius = 6.0F + 1.2F * (size - 1.0F) * 3.0F;
    light.duration = laser ? 0.075F : 0.060F;
    light.intensity = Curve::line(2.4F * size, 0.0F);
    light.offset = {0.0F, 0.1F, 0.0F};
    effect.lights.push_back(light);
    return effect;
}

[[nodiscard]] EffectDesc make_shell_casing()
{
    EffectDesc effect;
    effect.name = "shell_casing";
    effect.max_particles = 2;
    effect.lod_start = 10.0F;
    effect.lod_end = 28.0F;
    effect.lod_floor = 0.0F;
    effect.cull_distance = 30.0F;
    EmitterDesc casing = chip_base();
    casing.burst = 1;
    casing.inherit_velocity = 1.0F;
    casing.life = {1.6F, 2.2F};
    casing.speed = {1.6F, 2.6F};
    casing.spread = 0.14F;
    casing.gravity = 14.0F;
    casing.bounce = 0.45F;
    // A fleck of brass: at arm's length anything larger reads as a block.
    casing.size = {0.0035F, 0.0045F};
    casing.spin = {-26.0F, 26.0F};
    effect.emitters.push_back(casing);
    return effect;
}

// A bolt burning into whatever it struck, authored white: the bolt's colour is
// the tint. A flash, molten sparks that skip away, a moment of heat, light.
[[nodiscard]] EffectDesc make_bolt_impact()
{
    EffectDesc effect;
    effect.name = "bolt_impact";
    effect.priority = 1;
    effect.max_particles = 40;
    effect.lod_start = 24.0F;
    effect.lod_end = 80.0F;
    effect.lod_floor = 0.4F;
    effect.cull_distance = 160.0F;

    EmitterDesc flash = glow_base({1.0F, 1.0F, 1.0F}, 13.0F);
    flash.burst = 1;
    flash.offset = {0.0F, 0.05F, 0.0F};
    flash.life = {0.09F, 0.12F};
    flash.size = {0.11F, 0.15F};
    flash.size_over_life = Curve::line(0.7F, 2.3F);
    effect.emitters.push_back(flash);
    EmitterDesc bloom = glow_base({1.0F, 1.0F, 1.0F}, 2.4F);
    bloom.burst = 1;
    bloom.offset = {0.0F, 0.08F, 0.0F};
    bloom.life = {0.14F, 0.18F};
    bloom.size = {0.40F, 0.50F};
    bloom.alpha = Curve::line(0.30F, 0.0F);
    effect.emitters.push_back(bloom);
    EmitterDesc molten = spark_base();
    molten.burst = 16;
    molten.offset = {0.0F, 0.03F, 0.0F};
    molten.life = {0.22F, 0.60F};
    molten.speed = {3.0F, 9.0F};
    molten.spread = 0.50F;
    molten.size = {0.007F, 0.014F};
    molten.color = ColorCurve::keys({{0.0F, {1.0F, 1.0F, 1.0F}}, {1.0F, {0.75F, 0.75F, 0.75F}}});
    effect.emitters.push_back(molten);
    EmitterDesc shimmer;
    shimmer.material = Material::heat;
    shimmer.burst = 1;
    shimmer.offset = {0.0F, 0.1F, 0.0F};
    shimmer.life = {0.35F, 0.5F};
    shimmer.speed = {0.3F, 0.6F};
    shimmer.spread = 0.1F;
    shimmer.size = {0.16F, 0.22F};
    shimmer.size_over_life = Curve::line(1.0F, 1.9F);
    shimmer.alpha = Curve::keys({{0.0F, 0.0F}, {0.3F, 1.0F}, {1.0F, 0.0F}});
    shimmer.distortion = 0.004F;
    effect.emitters.push_back(shimmer);
    LightDesc light;
    light.color = {1.0F, 1.0F, 1.0F};
    light.radius = 5.5F;
    light.duration = 0.13F;
    light.intensity = Curve::line(2.2F, 0.0F);
    light.offset = {0.0F, 0.15F, 0.0F};
    effect.lights.push_back(light);
    return effect;
}

// Out of the back of a launcher: +Y is the way the blast goes (opposite the
// rocket).
[[nodiscard]] EffectDesc make_rocket_backblast()
{
    EffectDesc effect;
    effect.name = "rocket_backblast";
    effect.priority = 1;
    effect.max_particles = 60;
    effect.cull_distance = 200.0F;
    EmitterDesc flash = glow_base({1.0F, 0.78F, 0.46F}, 10.0F);
    flash.burst = 1;
    flash.inherit_velocity = 1.0F;
    flash.life = {0.08F, 0.11F};
    flash.size = {0.30F, 0.40F};
    flash.size_over_life = Curve::line(0.7F, 2.2F);
    effect.emitters.push_back(flash);
    EmitterDesc jet;
    jet.material = Material::flame;
    jet.burst = 6;
    jet.life = {0.10F, 0.20F};
    jet.speed = {6.0F, 14.0F};
    jet.spread = 0.10F;
    jet.drag = 5.0F;
    jet.size = {0.16F, 0.24F};
    jet.size_over_life = Curve::line(1.0F, 0.4F);
    jet.alpha = Curve::keys({{0.0F, 0.9F}, {0.6F, 0.7F}, {1.0F, 0.0F}});
    jet.temperature = Curve::line(2.4F, 1.0F);
    jet.emissive = Curve::constant(2.0F);
    jet.erosion = Curve::line(0.0F, 0.5F);
    jet.color = ColorCurve::constant(soot);
    effect.emitters.push_back(jet);
    EmitterDesc smoke = smoke_base(grey_smoke, pale_smoke, 0.5F);
    smoke.burst = 9;
    smoke.life = {0.9F, 1.6F};
    smoke.speed = {2.5F, 8.0F};
    smoke.spread = 0.20F;
    smoke.drag = 3.4F;
    smoke.buoyancy = 0.4F;
    smoke.turbulence = 1.6F;
    smoke.turbulence_scale = 1.2F;
    smoke.collide = true;
    smoke.size = {0.14F, 0.22F};
    smoke.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.25F, 3.0F}, {1.0F, 5.0F}});
    effect.emitters.push_back(smoke);
    EmitterDesc sparks = spark_base();
    sparks.burst = 18;
    sparks.life = {0.2F, 0.5F};
    sparks.speed = {5.0F, 13.0F};
    sparks.spread = 0.28F;
    sparks.size = {0.007F, 0.013F};
    effect.emitters.push_back(sparks);
    LightDesc light;
    light.color = {1.0F, 0.62F, 0.28F};
    light.radius = 8.0F;
    light.duration = 0.14F;
    light.intensity = Curve::line(3.0F, 0.0F);
    effect.lights.push_back(light);
    return effect;
}

// A blade on stone or metal, authored white-hot orange; tint it for a laser
// sword.
[[nodiscard]] EffectDesc make_melee_sparks()
{
    EffectDesc effect;
    effect.name = "melee_sparks";
    effect.max_particles = 24;
    effect.lod_start = 16.0F;
    effect.lod_end = 50.0F;
    effect.cull_distance = 70.0F;
    EmitterDesc sparks = spark_base();
    sparks.burst = 12;
    sparks.offset = {0.0F, 0.03F, 0.0F};
    sparks.life = {0.12F, 0.38F};
    sparks.speed = {2.5F, 7.0F};
    sparks.spread = 0.62F;
    sparks.size = {0.005F, 0.010F};
    effect.emitters.push_back(sparks);
    EmitterDesc pop = glow_base(spark_hot, 6.0F);
    pop.burst = 1;
    pop.offset = {0.0F, 0.04F, 0.0F};
    pop.life = {0.05F, 0.07F};
    pop.size = {0.05F, 0.07F};
    pop.size_over_life = Curve::line(0.7F, 1.9F);
    effect.emitters.push_back(pop);
    LightDesc light;
    light.color = spark_hot;
    light.radius = 3.0F;
    light.duration = 0.08F;
    light.intensity = Curve::line(0.8F, 0.0F);
    effect.lights.push_back(light);
    return effect;
}

// The fuse of a grenade in the air: it spits and trails a thread of smoke.
[[nodiscard]] EffectDesc make_grenade_fuse()
{
    EffectDesc effect;
    effect.name = "grenade_fuse";
    effect.max_particles = 60;
    effect.lod_start = 20.0F;
    effect.lod_end = 70.0F;
    effect.cull_distance = 110.0F;
    EmitterDesc glow = glow_base({1.0F, 0.56F, 0.18F}, 4.0F);
    glow.rate = 20.0F;
    glow.loop = true;
    glow.inherit_velocity = 1.0F;
    glow.life = {0.06F, 0.10F};
    glow.size = {0.035F, 0.055F};
    glow.alpha = Curve::keys({{0.0F, 0.0F}, {0.4F, 0.9F}, {1.0F, 0.0F}});
    effect.emitters.push_back(glow);
    EmitterDesc fizz = spark_base();
    fizz.rate = 22.0F;
    fizz.loop = true;
    fizz.inherit_velocity = 0.5F;
    fizz.life = {0.12F, 0.30F};
    fizz.speed = {0.8F, 2.6F};
    fizz.spread = 1.0F;
    fizz.gravity = 6.0F;
    fizz.collide = false;
    fizz.size = {0.005F, 0.009F};
    fizz.emissive = Curve::line(8.0F, 2.0F);
    effect.emitters.push_back(fizz);
    EmitterDesc thread = smoke_base(pale_smoke, grey_smoke, 0.34F);
    thread.rate = 26.0F;
    thread.loop = true;
    thread.inherit_velocity = 0.05F;
    thread.life = {0.6F, 1.0F};
    thread.speed = {0.0F, 0.3F};
    thread.spread = 1.0F;
    thread.buoyancy = 0.3F;
    thread.drag = 1.0F;
    thread.turbulence = 0.8F;
    thread.turbulence_scale = 0.7F;
    thread.size = {0.03F, 0.05F};
    thread.size_over_life = Curve::line(1.0F, 3.6F);
    effect.emitters.push_back(thread);
    return effect;
}

// ---------------------------------------------------------------------------
// Creatures and the rest
// ---------------------------------------------------------------------------

// A shot landing on a creature (the original's SmokeEffect at the hit point):
// a knock of dark matter off the body. Nothing in it glows.
[[nodiscard]] EffectDesc make_creature_hit()
{
    EffectDesc effect;
    effect.name = "creature_hit";
    effect.max_particles = 20;
    effect.lod_start = 20.0F;
    effect.lod_end = 60.0F;
    effect.cull_distance = 90.0F;
    EmitterDesc knock = smoke_base({0.20F, 0.07F, 0.06F}, {0.16F, 0.10F, 0.09F}, 0.66F);
    knock.burst = 4;
    knock.life = {0.30F, 0.55F};
    knock.speed = {0.8F, 2.6F};
    knock.spread = 0.42F;
    knock.drag = 4.5F;
    knock.gravity = 0.8F;
    knock.size = {0.045F, 0.075F};
    knock.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.3F, 2.8F}, {1.0F, 3.6F}});
    effect.emitters.push_back(knock);
    EmitterDesc haze = smoke_base(grey_smoke, pale_smoke, 0.30F);
    haze.burst = 2;
    haze.life = {0.6F, 1.0F};
    haze.speed = {0.3F, 1.0F};
    haze.spread = 0.6F;
    haze.drag = 3.0F;
    haze.buoyancy = 0.4F;
    haze.size = {0.07F, 0.11F};
    haze.size_over_life = Curve::line(1.0F, 3.8F);
    effect.emitters.push_back(haze);
    return effect;
}

// A creature going down: the dust its fall raises, up the height of the body
// and out along the ground.
[[nodiscard]] EffectDesc make_creature_death()
{
    EffectDesc effect;
    effect.name = "creature_death";
    effect.max_particles = 32;
    effect.lod_start = 24.0F;
    effect.lod_end = 70.0F;
    effect.cull_distance = 110.0F;
    EmitterDesc column = smoke_base({0.22F, 0.19F, 0.17F}, {0.34F, 0.31F, 0.28F}, 0.46F);
    column.burst = 8;
    column.shape = Shape::sphere;
    column.shape_radius = 0.55F;
    column.offset = {0.0F, 0.8F, 0.0F};
    column.life = {0.8F, 1.4F};
    column.speed = {0.3F, 1.2F};
    column.spread = 1.0F;
    column.drag = 2.4F;
    column.buoyancy = 0.3F;
    column.turbulence = 0.9F;
    column.turbulence_scale = 1.4F;
    column.size = {0.20F, 0.32F};
    column.size_over_life = Curve::line(1.0F, 2.8F);
    effect.emitters.push_back(column);
    EmitterDesc ring = smoke_base({0.40F, 0.36F, 0.31F}, {0.50F, 0.47F, 0.42F}, 0.36F);
    ring.burst = 7;
    ring.delay = 0.25F;
    ring.shape = Shape::ring;
    ring.shape_radius = 0.35F;
    ring.offset = {0.0F, 0.1F, 0.0F};
    ring.life = {0.7F, 1.2F};
    ring.speed = {0.8F, 1.8F};
    ring.spread = 0.12F;
    ring.drag = 3.4F;
    ring.size = {0.14F, 0.22F};
    ring.size_over_life = Curve::line(1.0F, 2.6F);
    effect.emitters.push_back(ring);
    return effect;
}

// Something appearing out of thin air (a teleport, a respawn): the air bends
// around the spot, a cold light, and motes that spiral up and go out.
[[nodiscard]] EffectDesc make_arrival()
{
    EffectDesc effect;
    effect.name = "arrival";
    effect.priority = 1;
    effect.max_particles = 60;
    effect.cull_distance = 160.0F;
    EmitterDesc bend;
    bend.material = Material::shockwave;
    bend.burst = 1;
    bend.offset = {0.0F, 0.9F, 0.0F};
    bend.life = {0.34F, 0.34F};
    bend.speed = {0.0F, 0.0F};
    bend.size = {0.30F, 0.30F};
    bend.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.5F, 4.0F}, {1.0F, 5.5F}});
    bend.alpha = Curve::keys({{0.0F, 1.0F}, {0.5F, 0.6F}, {1.0F, 0.0F}});
    bend.distortion = 0.018F;
    effect.emitters.push_back(bend);
    EmitterDesc pop = glow_base({1.0F, 0.84F, 0.62F}, 4.0F);
    pop.cold = true;
    pop.burst = 1;
    pop.offset = {0.0F, 0.9F, 0.0F};
    pop.life = {0.26F, 0.32F};
    pop.size = {0.6F, 0.75F};
    pop.size_over_life = Curve::line(0.6F, 2.0F);
    pop.alpha = Curve::keys({{0.0F, 0.0F}, {0.2F, 0.7F}, {1.0F, 0.0F}});
    effect.emitters.push_back(pop);
    EmitterDesc motes = spark_base(true);
    motes.burst = 26;
    motes.shape = Shape::ring;
    motes.shape_radius = 0.42F;
    motes.life = {0.45F, 1.0F};
    motes.speed = {0.2F, 0.9F};
    motes.spread = 0.3F;
    motes.velocity_bias = {0.0F, 1.5F, 0.0F};
    motes.gravity = 0.0F;
    motes.buoyancy = 1.2F;
    motes.drag = 1.4F;
    motes.turbulence = 3.0F;
    motes.turbulence_scale = 0.6F;
    motes.collide = false;
    motes.size = {0.008F, 0.015F};
    motes.stretch = 0.012F;
    motes.emissive = Curve::keys({{0.0F, 7.0F}, {0.7F, 3.0F}, {1.0F, 0.0F}});
    effect.emitters.push_back(motes);
    LightDesc light;
    light.color = {0.36F, 0.66F, 1.0F};
    light.radius = 7.0F;
    light.duration = 0.34F;
    light.intensity = Curve::keys({{0.0F, 0.0F}, {0.2F, 1.8F}, {1.0F, 0.0F}});
    light.offset = {0.0F, 0.9F, 0.0F};
    effect.lights.push_back(light);
    return effect;
}

// An item lying on the ground catching the light now and then.
[[nodiscard]] EffectDesc make_pickup_sparkle()
{
    EffectDesc effect;
    effect.name = "pickup_sparkle";
    effect.max_particles = 4;
    effect.lod_start = 10.0F;
    effect.lod_end = 26.0F;
    effect.lod_floor = 0.0F;
    effect.cull_distance = 28.0F;
    EmitterDesc glint = glow_base({1.0F, 0.94F, 0.78F}, 4.5F);
    glint.burst = 2;
    glint.shape = Shape::sphere;
    glint.shape_radius = 0.14F;
    glint.life = {0.35F, 0.6F};
    glint.speed = {0.15F, 0.4F};
    glint.spread = 0.2F;
    glint.size = {0.014F, 0.026F};
    glint.alpha = Curve::keys({{0.0F, 0.0F}, {0.3F, 1.0F}, {1.0F, 0.0F}});
    effect.emitters.push_back(glint);
    EmitterDesc soft = glow_base({1.0F, 0.90F, 0.70F}, 1.0F);
    soft.burst = 1;
    soft.life = {0.5F, 0.7F};
    soft.size = {0.14F, 0.18F};
    soft.alpha = Curve::keys({{0.0F, 0.0F}, {0.4F, 0.10F}, {1.0F, 0.0F}});
    effect.emitters.push_back(soft);
    return effect;
}

// The moment an item is taken: it winks out in a small burst of light.
[[nodiscard]] EffectDesc make_pickup_collect()
{
    EffectDesc effect;
    effect.name = "pickup_collect";
    effect.max_particles = 16;
    effect.lod_start = 12.0F;
    effect.lod_end = 30.0F;
    effect.cull_distance = 40.0F;
    EmitterDesc pop = glow_base({1.0F, 0.94F, 0.78F}, 4.0F);
    pop.burst = 1;
    pop.life = {0.14F, 0.18F};
    pop.size = {0.06F, 0.08F};
    pop.size_over_life = Curve::line(0.8F, 3.0F);
    pop.alpha = Curve::keys({{0.0F, 0.8F}, {1.0F, 0.0F}});
    effect.emitters.push_back(pop);
    EmitterDesc motes = spark_base();
    motes.burst = 7;
    motes.life = {0.22F, 0.42F};
    motes.speed = {0.6F, 1.8F};
    motes.spread = 0.6F;
    motes.gravity = 0.0F;
    motes.buoyancy = 1.0F;
    motes.drag = 3.0F;
    motes.collide = false;
    motes.size = {0.006F, 0.011F};
    motes.stretch = 0.01F;
    motes.emissive = Curve::line(6.0F, 0.0F);
    motes.color = ColorCurve::keys({{0.0F, {1.0F, 0.96F, 0.80F}}, {1.0F, {1.0F, 0.74F, 0.30F}}});
    effect.emitters.push_back(motes);
    return effect;
}

[[nodiscard]] EffectDesc make_footstep_dust()
{
    EffectDesc effect;
    effect.name = "footstep_dust";
    effect.max_particles = 6;
    effect.lod_start = 10.0F;
    effect.lod_end = 30.0F;
    effect.lod_floor = 0.0F;
    effect.cull_distance = 34.0F;
    EmitterDesc puff = smoke_base({0.96F, 0.96F, 0.96F}, {0.84F, 0.84F, 0.84F}, 0.20F);
    puff.burst = 2;
    puff.offset = {0.0F, 0.04F, 0.0F};
    puff.life = {0.45F, 0.8F};
    puff.speed = {0.2F, 0.6F};
    puff.spread = 0.8F;
    puff.inherit_velocity = 0.25F;
    puff.drag = 3.0F;
    puff.size = {0.07F, 0.10F};
    puff.size_over_life = Curve::line(1.0F, 2.8F);
    effect.emitters.push_back(puff);
    return effect;
}

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

constexpr std::size_t surface_count = static_cast<std::size_t>(Surface::count);
constexpr std::size_t strike_count = static_cast<std::size_t>(Strike::count);
constexpr std::size_t muzzle_count = static_cast<std::size_t>(Muzzle::count);

struct Library final
{
    // Names of the generated recipes; EffectDesc::name views into these.
    std::vector<std::string> names;
    std::array<std::array<EffectDesc, surface_count>, strike_count> strikes;
    std::array<EffectDesc, strike_count> dust;
    std::array<EffectDesc, muzzle_count> muzzles;
    std::array<EffectDesc, 2> flight;
    std::array<EffectDesc, 2> launch;
    std::array<EffectDesc, 2> breath;
    std::array<EffectDesc, 2> death;
    EffectDesc embers;
    EffectDesc ground_fire;
    EffectDesc iceball_burst;
    EffectDesc ground_frost;
    EffectDesc dragon_hit;
    EffectDesc shell_casing;
    EffectDesc bolt_impact;
    EffectDesc rocket_backblast;
    EffectDesc melee_sparks;
    EffectDesc grenade_fuse;
    EffectDesc creature_hit;
    EffectDesc creature_death;
    EffectDesc arrival;
    EffectDesc pickup_sparkle;
    EffectDesc pickup_collect;
    EffectDesc footstep_dust;
    std::vector<const EffectDesc *> all;

    Library()
    {
        names.reserve(strike_count * surface_count + strike_count + muzzle_count);
        for (std::size_t strike = 0U; strike < strike_count; ++strike)
        {
            for (std::size_t surface = 0U; surface < surface_count; ++surface)
            {
                names.push_back("strike_" + std::string{strike_names[strike]} + "_" +
                                std::string{surface_names[surface]});
                strikes[strike][surface] = make_surface_strike(
                    names.back(), static_cast<Strike>(strike), static_cast<Surface>(surface));
                all.push_back(&strikes[strike][surface]);
            }
            names.push_back("dust_" + std::string{strike_names[strike]});
            dust[strike] = make_strike_dust(names.back(), static_cast<Strike>(strike));
            all.push_back(&dust[strike]);
        }
        static constexpr std::array<std::string_view, muzzle_count> muzzle_names{
            "muzzle_flash_pistol", "muzzle_flash_rifle", "muzzle_flash_shotgun",
            "muzzle_flash_laser"};
        for (std::size_t muzzle = 0U; muzzle < muzzle_count; ++muzzle)
        {
            muzzles[muzzle] = make_muzzle_flash(muzzle_names[muzzle], static_cast<Muzzle>(muzzle));
            all.push_back(&muzzles[muzzle]);
        }
        for (std::size_t ice = 0U; ice < 2U; ++ice)
        {
            flight[ice] = make_fireball_flight(ice != 0U);
            launch[ice] = make_fireball_launch(ice != 0U);
            breath[ice] = make_dragon_breath(ice != 0U);
            death[ice] = make_dragon_death(ice != 0U);
            all.push_back(&flight[ice]);
            all.push_back(&launch[ice]);
            all.push_back(&breath[ice]);
            all.push_back(&death[ice]);
        }
        const auto add = [this](EffectDesc &slot, EffectDesc made) {
            slot = std::move(made);
            all.push_back(&slot);
        };
        add(embers, make_fireball_embers());
        add(ground_fire, make_ground_fire());
        add(iceball_burst, make_iceball_burst());
        add(ground_frost, make_ground_frost());
        add(dragon_hit, make_dragon_hit());
        add(shell_casing, make_shell_casing());
        add(bolt_impact, make_bolt_impact());
        add(rocket_backblast, make_rocket_backblast());
        add(melee_sparks, make_melee_sparks());
        add(grenade_fuse, make_grenade_fuse());
        add(creature_hit, make_creature_hit());
        add(creature_death, make_creature_death());
        add(arrival, make_arrival());
        add(pickup_sparkle, make_pickup_sparkle());
        add(pickup_collect, make_pickup_collect());
        add(footstep_dust, make_footstep_dust());
    }
};

[[nodiscard]] const Library &library()
{
    static const Library instance;
    return instance;
}

} // namespace

Surface surface_of(world::BlockType block) noexcept
{
    using world::BlockType;
    switch (block)
    {
    case BlockType::dirt:
    case BlockType::grass:
        return Surface::dirt;
    case BlockType::sand:
        return Surface::sand;
    case BlockType::snow:
        return Surface::snow;
    case BlockType::ice:
        return Surface::ice;
    case BlockType::log:
    case BlockType::wood:
    case BlockType::crate:
    case BlockType::torch:
    case BlockType::torch_pos_x:
    case BlockType::torch_neg_z:
    case BlockType::torch_neg_x:
    case BlockType::torch_pos_z:
    case BlockType::torch_pos_y:
    case BlockType::torch_neg_y:
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::upper_door_open:
    case BlockType::crafting_station:
        return Surface::wood;
    case BlockType::leaves:
        return Surface::leaves;
    case BlockType::gold_ore:
    case BlockType::iron_ore:
    case BlockType::copper_ore:
    case BlockType::coal_ore:
    case BlockType::diamond_ore:
        return Surface::ore;
    case BlockType::iron_wall:
    case BlockType::copper_wall:
    case BlockType::golden_wall:
    case BlockType::diamond_wall:
    case BlockType::lantern:
    case BlockType::fixed_lantern:
    case BlockType::lantern_fancy:
    case BlockType::crate_copper:
    case BlockType::crate_iron:
    case BlockType::crate_gold:
    case BlockType::crate_diamond:
    case BlockType::crate_safe:
    case BlockType::strong_lower_door_closed_z:
    case BlockType::strong_lower_door_closed_x:
    case BlockType::strong_lower_door:
    case BlockType::strong_upper_door_closed:
    case BlockType::strong_lower_door_open_z:
    case BlockType::strong_lower_door_open_x:
    case BlockType::strong_upper_door_open:
    case BlockType::turret_block:
    case BlockType::teleport_station:
    case BlockType::hell_forge:
    case BlockType::tnt:
    case BlockType::c4:
        return Surface::metal;
    case BlockType::blood_stone:
    case BlockType::crate_bloodstone:
    case BlockType::space_rock:
    case BlockType::space_rock_inventory:
        return Surface::crystal;
    case BlockType::surface_lava:
    case BlockType::deep_lava:
        return Surface::lava;
    case BlockType::slime:
        return Surface::slime;
    case BlockType::glass_basic:
    case BlockType::glass_iron:
    case BlockType::glass_strong:
    case BlockType::glass_mystery:
        return Surface::glass;
    default:
        return Surface::stone;
    }
}

DustAmount strike_dust_amount(Strike strike, Surface surface) noexcept
{
    const SurfaceLook look = surface_look(surface);
    DustAmount amount{look.dust, look.dust_scale};
    switch (strike)
    {
    case Strike::tool:
    case Strike::count:
        break;
    case Strike::shatter:
        // Even a block that gives off little dust leaves some when it breaks.
        amount.intensity = std::max(amount.intensity, 0.5F);
        break;
    case Strike::bullet:
        amount.intensity = std::max(amount.intensity * 0.9F, look.dust > 0.0F ? 0.6F : 0.0F);
        amount.scale *= 0.9F;
        break;
    case Strike::bolt:
        amount.intensity = look.dust > 0.0F ? 0.8F : 0.0F;
        amount.scale = 0.9F;
        break;
    case Strike::place:
        amount.intensity = std::max(amount.intensity * 0.7F, 0.5F);
        amount.scale = std::min(amount.scale, 1.0F);
        break;
    }
    return amount;
}

float dragon_charge(bool attacking, float clip_seconds) noexcept
{
    if (!attacking || clip_seconds <= 0.0F)
        return 0.0F;
    // Builds through the wind-up to the moment the shot leaves, then lets go
    // over the fifth of a second after it.
    if (clip_seconds <= dragon_shot_clip_seconds)
    {
        const float rise = clip_seconds / dragon_shot_clip_seconds;
        return rise * rise;
    }
    return std::clamp(1.0F - (clip_seconds - dragon_shot_clip_seconds) / 0.2F, 0.0F, 1.0F);
}

namespace effects
{

const EffectDesc &surface_strike(Strike strike, Surface surface)
{
    const std::size_t strike_index =
        std::min(static_cast<std::size_t>(strike), strike_count - 1U);
    const std::size_t surface_index =
        std::min(static_cast<std::size_t>(surface), surface_count - 1U);
    return library().strikes[strike_index][surface_index];
}
const EffectDesc &strike_dust(Strike strike)
{
    return library().dust[std::min(static_cast<std::size_t>(strike), strike_count - 1U)];
}
const EffectDesc &fireball_flight(bool ice)
{
    return library().flight[ice ? 1U : 0U];
}
const EffectDesc &fireball_launch(bool ice)
{
    return library().launch[ice ? 1U : 0U];
}
const EffectDesc &dragon_breath(bool ice)
{
    return library().breath[ice ? 1U : 0U];
}
const EffectDesc &fireball_embers()
{
    return library().embers;
}
const EffectDesc &ground_fire()
{
    return library().ground_fire;
}
const EffectDesc &iceball_burst()
{
    return library().iceball_burst;
}
const EffectDesc &ground_frost()
{
    return library().ground_frost;
}
const EffectDesc &dragon_hit()
{
    return library().dragon_hit;
}
const EffectDesc &dragon_death(bool ice)
{
    return library().death[ice ? 1U : 0U];
}
const EffectDesc &muzzle_flash(Muzzle muzzle)
{
    return library().muzzles[std::min(static_cast<std::size_t>(muzzle), muzzle_count - 1U)];
}
const EffectDesc &shell_casing()
{
    return library().shell_casing;
}
const EffectDesc &bolt_impact()
{
    return library().bolt_impact;
}
const EffectDesc &rocket_backblast()
{
    return library().rocket_backblast;
}
const EffectDesc &melee_sparks()
{
    return library().melee_sparks;
}
const EffectDesc &grenade_fuse()
{
    return library().grenade_fuse;
}
const EffectDesc &creature_hit()
{
    return library().creature_hit;
}
const EffectDesc &creature_death()
{
    return library().creature_death;
}
const EffectDesc &arrival()
{
    return library().arrival;
}
const EffectDesc &pickup_sparkle()
{
    return library().pickup_sparkle;
}
const EffectDesc &pickup_collect()
{
    return library().pickup_collect;
}
const EffectDesc &footstep_dust()
{
    return library().footstep_dust;
}

} // namespace effects

std::span<const EffectDesc *const> combat_effects()
{
    const Library &all = library();
    return {all.all.data(), all.all.size()};
}

} // namespace openminerz::vfx
