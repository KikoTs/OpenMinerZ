// The effect recipes for smoke, fire and explosions (openminerz/vfx.hpp).
//
// Each recipe is built once and returned by reference. Weapon, dragon and
// mining recipes belong in a file of their own next to this one; list them in
// all_effects() at the bottom so diagnostics and tests can find them by name.
//
// Reading a recipe: sizes are billboard half-extents in blocks, times in
// seconds, colours linear. A gas particle's `temperature` is what makes it
// fire (above ~0.6) or smoke (0); most fire here is smoke that starts hot.

#include "openminerz/vfx.hpp"

#include <algorithm>
#include <array>

namespace openminerz::vfx
{
namespace
{

constexpr Color soot{0.085F, 0.080F, 0.075F};
constexpr Color dark_smoke{0.17F, 0.165F, 0.16F};
constexpr Color grey_smoke{0.36F, 0.355F, 0.35F};
constexpr Color pale_smoke{0.58F, 0.575F, 0.57F};
constexpr Color dust{0.47F, 0.43F, 0.37F};
constexpr Color spark_hot{1.0F, 0.80F, 0.46F};
constexpr Color spark_cool{1.0F, 0.26F, 0.035F};
constexpr Color ember_dying{0.75F, 0.07F, 0.01F};

// A puff of lit smoke: the parts every smoke emitter shares.
[[nodiscard]] EmitterDesc smoke_base(Color young, Color old, float peak_alpha)
{
    EmitterDesc emitter;
    emitter.material = Material::smoke;
    emitter.color = ColorCurve::keys({{0.0F, young}, {1.0F, old}});
    emitter.alpha =
        Curve::keys({{0.0F, 0.0F}, {0.09F, peak_alpha}, {0.55F, peak_alpha * 0.78F}, {1.0F, 0.0F}});
    // A puff holds together for most of its life and tears apart at the end.
    emitter.erosion = Curve::keys({{0.0F, 0.0F}, {0.70F, 0.12F}, {1.0F, 0.78F}});
    emitter.spin = {-0.5F, 0.5F};
    emitter.frame_rate = 10.0F;
    return emitter;
}

[[nodiscard]] EmitterDesc spark_base()
{
    EmitterDesc emitter;
    emitter.material = Material::spark;
    emitter.gravity = 10.0F;
    emitter.drag = 0.5F;
    emitter.collide = true;
    emitter.bounce = 0.35F;
    emitter.friction = 0.35F;
    emitter.size = {0.012F, 0.022F};
    emitter.stretch = 0.016F;
    emitter.emissive = Curve::keys({{0.0F, 7.0F}, {0.6F, 3.6F}, {1.0F, 1.0F}});
    emitter.color = ColorCurve::keys({{0.0F, spark_hot}, {1.0F, spark_cool}});
    emitter.alpha = Curve::keys({{0.0F, 1.0F}, {0.75F, 1.0F}, {1.0F, 0.0F}});
    return emitter;
}

[[nodiscard]] EffectDesc make_impact_puff()
{
    EffectDesc effect;
    effect.name = "impact_puff";
    effect.cull_distance = 90.0F;
    effect.lod_start = 20.0F;
    effect.lod_end = 60.0F;
    effect.max_particles = 16;
    EmitterDesc puff = smoke_base(pale_smoke, grey_smoke, 0.50F);
    puff.burst = 5;
    puff.life = {0.7F, 1.25F};
    puff.speed = {0.5F, 1.7F};
    puff.spread = 0.42F;
    puff.drag = 3.4F;
    puff.buoyancy = 0.55F;
    puff.turbulence = 1.3F;
    puff.turbulence_scale = 1.1F;
    puff.size = {0.07F, 0.12F};
    puff.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.25F, 2.9F}, {1.0F, 4.8F}});
    effect.emitters.push_back(puff);
    return effect;
}

[[nodiscard]] EffectDesc make_dust_puff()
{
    EffectDesc effect;
    effect.name = "dust_puff";
    effect.cull_distance = 90.0F;
    effect.lod_start = 20.0F;
    effect.lod_end = 60.0F;
    effect.max_particles = 16;
    EmitterDesc puff = smoke_base({0.80F, 0.78F, 0.74F}, {0.70F, 0.68F, 0.64F}, 0.40F);
    puff.burst = 6;
    puff.life = {0.8F, 1.4F};
    puff.speed = {0.3F, 1.1F};
    puff.spread = 0.72F;
    puff.drag = 2.6F;
    puff.gravity = 0.25F;
    puff.turbulence = 0.9F;
    puff.turbulence_scale = 1.4F;
    puff.size = {0.10F, 0.16F};
    puff.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.3F, 2.6F}, {1.0F, 4.0F}});
    effect.emitters.push_back(puff);
    return effect;
}

[[nodiscard]] EffectDesc make_lingering_smoke()
{
    EffectDesc effect;
    effect.name = "lingering_smoke";
    effect.max_particles = 64;
    EmitterDesc column = smoke_base(dark_smoke, grey_smoke, 0.40F);
    column.rate = 5.0F;
    column.duration = 4.0F;
    column.shape = Shape::sphere;
    column.shape_radius = 0.45F;
    column.life = {4.0F, 6.5F};
    column.speed = {0.3F, 0.8F};
    column.spread = 0.3F;
    column.drag = 0.8F;
    column.buoyancy = 0.34F;
    column.turbulence = 0.6F;
    column.turbulence_scale = 3.0F;
    column.collide = true;
    column.size = {0.5F, 0.8F};
    column.size_over_life = Curve::line(1.0F, 3.2F);
    effect.emitters.push_back(column);
    return effect;
}

[[nodiscard]] EffectDesc make_fuse_smoke()
{
    EffectDesc effect;
    effect.name = "fuse_smoke";
    effect.cull_distance = 80.0F;
    effect.lod_start = 16.0F;
    effect.lod_end = 50.0F;
    effect.max_particles = 48;
    EmitterDesc thread = smoke_base(pale_smoke, grey_smoke, 0.42F);
    thread.rate = 9.0F;
    thread.loop = true;
    thread.life = {1.2F, 1.9F};
    thread.speed = {0.5F, 1.0F};
    thread.spread = 0.08F;
    thread.buoyancy = 0.35F;
    thread.drag = 0.6F;
    thread.turbulence = 1.0F;
    thread.turbulence_scale = 0.8F;
    thread.size = {0.04F, 0.07F};
    thread.size_over_life = Curve::line(1.0F, 5.0F);
    effect.emitters.push_back(thread);
    EmitterDesc fizz = spark_base();
    fizz.rate = 16.0F;
    fizz.loop = true;
    fizz.life = {0.15F, 0.38F};
    fizz.speed = {1.0F, 3.2F};
    fizz.spread = 0.55F;
    fizz.gravity = 6.0F;
    fizz.size = {0.007F, 0.012F};
    fizz.emissive = Curve::line(8.0F, 2.0F);
    effect.emitters.push_back(fizz);
    LightDesc glow;
    glow.color = {1.0F, 0.62F, 0.26F};
    glow.radius = 3.2F;
    glow.intensity = Curve::constant(0.55F);
    glow.flicker = 0.6F;
    effect.lights.push_back(glow);
    return effect;
}

// A flame as a stream of hot tongues, with the halo, embers, wisp of smoke,
// shimmer and flickering light that go with it. `size` 1 is a torch.
[[nodiscard]] EffectDesc make_flame(std::string_view name, float size, float light_radius,
                                    float light_strength)
{
    EffectDesc effect;
    effect.name = name;
    effect.cull_distance = 72.0F * std::max(size, 1.0F);
    effect.lod_start = 14.0F;
    effect.lod_end = 44.0F;
    effect.lod_floor = 0.4F;
    effect.max_particles = static_cast<int>(72.0F * std::max(size, 1.0F));

    EmitterDesc tongue;
    tongue.material = Material::flame;
    // Many short-lived tongues that overlap: one flame body, not a string of
    // separate little flames.
    tongue.rate = 26.0F * std::max(size, 1.0F);
    tongue.loop = true;
    tongue.shape = size > 1.5F ? Shape::sphere : Shape::point;
    tongue.shape_radius = 0.10F * size;
    tongue.life = {0.30F, 0.50F};
    tongue.speed = {0.12F * size, 0.34F * size};
    tongue.spread = 0.07F;
    tongue.buoyancy = 0.9F * size;
    tongue.turbulence = 0.7F;
    tongue.turbulence_scale = 0.5F * size;
    tongue.size = {0.105F * size, 0.145F * size};
    tongue.size_over_life = Curve::keys({{0.0F, 0.75F}, {0.2F, 1.0F}, {1.0F, 0.5F}});
    tongue.rotation = {-0.22F, 0.22F};
    tongue.alpha = Curve::keys({{0.0F, 0.0F}, {0.14F, 0.95F}, {0.7F, 0.7F}, {1.0F, 0.0F}});
    tongue.temperature = Curve::keys({{0.0F, 1.7F}, {0.5F, 1.2F}, {1.0F, 0.7F}});
    tongue.emissive = Curve::constant(2.2F);
    tongue.erosion = Curve::line(0.0F, 0.45F);
    tongue.frame_rate = 22.0F;
    tongue.color = ColorCurve::constant(soot);
    effect.emitters.push_back(tongue);

    EmitterDesc halo;
    halo.material = Material::glow;
    halo.rate = 5.0F;
    halo.loop = true;
    halo.offset = {0.0F, 0.08F * size, 0.0F};
    halo.life = {0.3F, 0.42F};
    halo.speed = {0.0F, 0.1F};
    halo.size = {0.26F * size, 0.32F * size};
    halo.alpha = Curve::keys({{0.0F, 0.0F}, {0.4F, 0.40F}, {1.0F, 0.0F}});
    halo.emissive = Curve::constant(1.5F);
    halo.color = ColorCurve::constant({1.0F, 0.50F, 0.17F});
    effect.emitters.push_back(halo);

    EmitterDesc ember = spark_base();
    ember.rate = 1.7F * std::max(size, 1.0F);
    ember.loop = true;
    ember.offset = {0.0F, 0.12F * size, 0.0F};
    ember.life = {0.9F, 1.9F};
    ember.speed = {0.5F * size, 1.3F * size};
    ember.spread = 0.25F;
    ember.gravity = 0.0F;
    ember.buoyancy = 0.9F;
    ember.drag = 0.9F;
    ember.turbulence = 2.2F;
    ember.turbulence_scale = 0.6F;
    ember.collide = false;
    ember.size = {0.006F, 0.011F};
    ember.stretch = 0.012F;
    ember.emissive = Curve::keys({{0.0F, 7.0F}, {0.7F, 3.0F}, {1.0F, 0.0F}});
    ember.color = ColorCurve::keys({{0.0F, spark_hot}, {0.5F, spark_cool}, {1.0F, ember_dying}});
    effect.emitters.push_back(ember);

    EmitterDesc wisp = smoke_base(dark_smoke, grey_smoke, 0.15F * std::min(size, 2.0F));
    wisp.rate = 3.0F * std::max(size, 1.0F);
    wisp.loop = true;
    wisp.offset = {0.0F, 0.20F * size, 0.0F};
    wisp.life = {1.4F, 2.3F};
    wisp.speed = {0.5F, 0.9F};
    wisp.spread = 0.12F;
    wisp.buoyancy = 0.4F;
    wisp.drag = 0.5F;
    wisp.turbulence = 0.9F;
    wisp.turbulence_scale = 1.0F;
    wisp.size = {0.05F * size, 0.08F * size};
    wisp.size_over_life = Curve::line(1.0F, 4.6F);
    effect.emitters.push_back(wisp);

    EmitterDesc shimmer;
    shimmer.material = Material::heat;
    shimmer.rate = 4.0F;
    shimmer.loop = true;
    shimmer.offset = {0.0F, 0.22F * size, 0.0F};
    shimmer.life = {0.6F, 0.85F};
    shimmer.speed = {0.6F, 0.9F};
    shimmer.spread = 0.1F;
    shimmer.size = {0.13F * size, 0.17F * size};
    shimmer.size_over_life = Curve::line(1.0F, 1.9F);
    shimmer.alpha = Curve::keys({{0.0F, 0.0F}, {0.3F, 1.0F}, {1.0F, 0.0F}});
    shimmer.distortion = 0.0035F;
    effect.emitters.push_back(shimmer);

    LightDesc light;
    light.color = {1.0F, 0.66F, 0.36F};
    light.radius = light_radius;
    light.intensity = Curve::constant(light_strength);
    light.flicker = 0.42F;
    light.offset = {0.0F, 0.1F * size, 0.0F};
    effect.lights.push_back(light);
    return effect;
}

[[nodiscard]] EffectDesc make_lava_bubble()
{
    EffectDesc effect;
    effect.name = "lava_bubble";
    effect.cull_distance = 56.0F;
    effect.lod_start = 14.0F;
    effect.lod_end = 40.0F;
    effect.max_particles = 16;
    EmitterDesc pop;
    pop.material = Material::glow;
    pop.burst = 1;
    pop.life = {0.30F, 0.42F};
    pop.speed = {0.0F, 0.0F};
    pop.size = {0.16F, 0.24F};
    pop.size_over_life = Curve::line(0.6F, 2.0F);
    pop.alpha = Curve::keys({{0.0F, 0.0F}, {0.2F, 0.8F}, {1.0F, 0.0F}});
    pop.emissive = Curve::constant(2.6F);
    pop.color = ColorCurve::constant({1.0F, 0.42F, 0.08F});
    effect.emitters.push_back(pop);
    EmitterDesc droplets = spark_base();
    droplets.burst = 5;
    droplets.life = {0.35F, 0.8F};
    droplets.speed = {1.4F, 3.6F};
    droplets.spread = 0.3F;
    droplets.bounce = 0.0F;
    droplets.size = {0.016F, 0.03F};
    droplets.stretch = 0.02F;
    droplets.emissive = Curve::line(7.0F, 2.5F);
    droplets.color = ColorCurve::keys({{0.0F, {1.0F, 0.62F, 0.20F}}, {1.0F, spark_cool}});
    effect.emitters.push_back(droplets);
    LightDesc light;
    light.color = {1.0F, 0.42F, 0.10F};
    light.radius = 4.5F;
    light.duration = 0.4F;
    light.intensity = Curve::keys({{0.0F, 0.0F}, {0.25F, 0.9F}, {1.0F, 0.0F}});
    effect.lights.push_back(light);
    return effect;
}

[[nodiscard]] EffectDesc make_lava_spurt()
{
    EffectDesc effect;
    effect.name = "lava_spurt";
    effect.cull_distance = 64.0F;
    effect.lod_start = 16.0F;
    effect.lod_end = 44.0F;
    effect.max_particles = 40;
    EmitterDesc jet = spark_base();
    jet.burst = 14;
    jet.life = {0.6F, 1.35F};
    jet.speed = {3.0F, 7.2F};
    jet.spread = 0.2F;
    jet.bounce = 0.15F;
    jet.size = {0.018F, 0.034F};
    jet.emissive = Curve::keys({{0.0F, 9.0F}, {0.7F, 4.0F}, {1.0F, 1.0F}});
    jet.color = ColorCurve::keys({{0.0F, {1.0F, 0.66F, 0.24F}}, {1.0F, spark_cool}});
    effect.emitters.push_back(jet);
    EmitterDesc lick;
    lick.material = Material::flame;
    lick.burst = 5;
    lick.shape = Shape::sphere;
    lick.shape_radius = 0.18F;
    lick.life = {0.4F, 0.75F};
    lick.speed = {0.8F, 2.0F};
    lick.spread = 0.12F;
    lick.buoyancy = 1.6F;
    lick.turbulence = 0.8F;
    lick.turbulence_scale = 0.6F;
    lick.size = {0.16F, 0.26F};
    lick.size_over_life = Curve::keys({{0.0F, 0.6F}, {0.3F, 1.0F}, {1.0F, 0.5F}});
    lick.rotation = {-0.25F, 0.25F};
    lick.alpha = Curve::keys({{0.0F, 0.0F}, {0.15F, 0.9F}, {0.7F, 0.6F}, {1.0F, 0.0F}});
    lick.temperature = Curve::line(1.5F, 0.6F);
    lick.emissive = Curve::constant(1.8F);
    lick.erosion = Curve::line(0.0F, 0.5F);
    lick.frame_rate = 20.0F;
    lick.color = ColorCurve::constant(soot);
    effect.emitters.push_back(lick);
    EmitterDesc shimmer;
    shimmer.material = Material::heat;
    shimmer.burst = 2;
    shimmer.life = {0.8F, 1.1F};
    shimmer.speed = {0.8F, 1.4F};
    shimmer.spread = 0.1F;
    shimmer.size = {0.3F, 0.4F};
    shimmer.size_over_life = Curve::line(1.0F, 2.0F);
    shimmer.alpha = Curve::keys({{0.0F, 0.0F}, {0.3F, 1.0F}, {1.0F, 0.0F}});
    shimmer.distortion = 0.005F;
    effect.emitters.push_back(shimmer);
    LightDesc light;
    light.color = {1.0F, 0.45F, 0.11F};
    light.radius = 6.5F;
    light.duration = 0.6F;
    light.intensity = Curve::keys({{0.0F, 0.0F}, {0.15F, 1.4F}, {1.0F, 0.0F}});
    effect.lights.push_back(light);
    return effect;
}

[[nodiscard]] EffectDesc make_rocket_trail()
{
    EffectDesc effect;
    effect.name = "rocket_trail";
    effect.max_particles = 220;
    effect.lod_start = 40.0F;
    effect.lod_end = 120.0F;
    EmitterDesc exhaust;
    exhaust.material = Material::flame;
    exhaust.rate = 80.0F;
    exhaust.loop = true;
    exhaust.life = {0.07F, 0.15F};
    exhaust.speed = {2.0F, 5.0F};
    exhaust.spread = 0.06F;
    exhaust.size = {0.10F, 0.15F};
    exhaust.size_over_life = Curve::line(1.0F, 0.35F);
    exhaust.alpha = Curve::keys({{0.0F, 0.9F}, {0.6F, 0.7F}, {1.0F, 0.0F}});
    exhaust.temperature = Curve::line(2.4F, 1.0F);
    exhaust.emissive = Curve::constant(3.0F);
    exhaust.frame_rate = 30.0F;
    exhaust.color = ColorCurve::constant(soot);
    effect.emitters.push_back(exhaust);
    EmitterDesc smoke = smoke_base(pale_smoke, grey_smoke, 0.48F);
    smoke.rate = 44.0F;
    smoke.loop = true;
    smoke.life = {1.3F, 2.5F};
    smoke.speed = {0.3F, 1.3F};
    smoke.spread = 0.22F;
    smoke.drag = 1.6F;
    smoke.buoyancy = 0.22F;
    smoke.turbulence = 1.4F;
    smoke.turbulence_scale = 1.2F;
    smoke.size = {0.10F, 0.16F};
    smoke.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.3F, 3.0F}, {1.0F, 5.2F}});
    smoke.temperature = Curve::keys({{0.0F, 0.9F}, {0.06F, 0.0F}});
    effect.emitters.push_back(smoke);
    EmitterDesc sparks = spark_base();
    sparks.rate = 26.0F;
    sparks.loop = true;
    sparks.life = {0.2F, 0.5F};
    sparks.speed = {2.0F, 6.0F};
    sparks.spread = 0.18F;
    sparks.gravity = 5.0F;
    sparks.size = {0.008F, 0.014F};
    effect.emitters.push_back(sparks);
    LightDesc light;
    light.color = {1.0F, 0.58F, 0.26F};
    light.radius = 7.5F;
    light.intensity = Curve::constant(1.3F);
    light.flicker = 0.3F;
    effect.lights.push_back(light);
    return effect;
}

// The explosion sequence, sized by one number (a grenade is about 0.8, C4
// 1.75): flash and light, a fireball that rolls and darkens into smoke, a
// shockwave, sparks with trails, embers, debris, a ground dust ring, heat and
// the smoke that hangs afterwards. Sizes only: what it destroys and whom it
// hurts is decided elsewhere and is not read here.
[[nodiscard]] EffectDesc make_explosion(std::string_view name, float size)
{
    EffectDesc effect;
    effect.name = name;
    effect.priority = 2;
    effect.max_particles = static_cast<int>(430.0F * size);
    effect.lod_start = 44.0F;
    effect.lod_end = 150.0F;
    effect.lod_floor = 0.35F;
    effect.cull_distance = 340.0F;
    effect.shake = std::min(0.50F * size, 1.0F);
    effect.shake_duration = 0.45F + 0.12F * size;
    effect.shake_radius = 30.0F * size;
    effect.flash = std::min(0.42F * size, 0.8F);
    effect.flash_duration = 0.14F;
    effect.flash_radius = 24.0F * size;
    const auto count = [size](float amount) { return static_cast<int>(amount * size + 0.5F); };

    // 0: the flash itself.
    EmitterDesc flash;
    flash.material = Material::glow;
    flash.burst = 2;
    flash.life = {0.10F, 0.16F};
    flash.speed = {0.0F, 0.0F};
    flash.size = {1.7F * size, 2.2F * size};
    flash.size_over_life = Curve::line(0.8F, 2.3F);
    flash.alpha = Curve::keys({{0.0F, 1.0F}, {0.4F, 0.6F}, {1.0F, 0.0F}});
    flash.emissive = Curve::constant(13.0F);
    flash.color = ColorCurve::constant({1.0F, 0.86F, 0.62F});
    effect.emitters.push_back(flash);

    // 1: the fireball. Hot gas thrown outward that billows, rises and cools to
    // soot within a second.
    EmitterDesc fireball;
    fireball.material = Material::smoke;
    fireball.burst = count(20.0F);
    fireball.shape = Shape::sphere;
    fireball.shape_radius = 0.45F * size;
    fireball.life = {1.1F, 1.9F};
    fireball.speed = {3.0F * size, 9.0F * size};
    fireball.spread = 1.0F;
    fireball.velocity_bias = {0.0F, 1.4F, 0.0F};
    fireball.drag = 3.3F;
    fireball.buoyancy = 2.1F;
    fireball.turbulence = 3.0F;
    fireball.turbulence_scale = 1.6F * size;
    fireball.collide = true;
    fireball.size = {0.75F * size, 1.20F * size};
    fireball.size_over_life = Curve::keys({{0.0F, 0.45F}, {0.14F, 1.25F}, {1.0F, 2.4F}});
    fireball.spin = {-0.9F, 0.9F};
    fireball.color = ColorCurve::keys({{0.0F, soot}, {1.0F, dark_smoke}});
    fireball.alpha = Curve::keys({{0.0F, 0.95F}, {0.55F, 0.80F}, {1.0F, 0.0F}});
    fireball.temperature =
        Curve::keys({{0.0F, 2.6F}, {0.14F, 1.9F}, {0.40F, 1.0F}, {0.66F, 0.0F}});
    fireball.emissive = Curve::constant(1.0F);
    fireball.erosion = Curve::keys({{0.0F, 0.0F}, {0.5F, 0.22F}, {1.0F, 0.86F}});
    fireball.frame_rate = 12.0F;
    effect.emitters.push_back(fireball);

    // 2: the column that boils up behind it.
    EmitterDesc column = smoke_base(soot, grey_smoke, 0.92F);
    column.rate = 26.0F * size;
    column.delay = 0.14F;
    column.duration = 0.8F;
    column.shape = Shape::sphere;
    column.shape_radius = 0.6F * size;
    column.life = {2.6F, 4.6F};
    column.speed = {0.8F, 2.6F};
    column.spread = 0.55F;
    column.buoyancy = 1.6F;
    column.drag = 0.9F;
    column.turbulence = 1.6F;
    column.turbulence_scale = 2.4F;
    column.collide = true;
    column.size = {0.95F * size, 1.45F * size};
    column.size_over_life = Curve::keys({{0.0F, 0.8F}, {0.3F, 1.7F}, {1.0F, 2.7F}});
    column.temperature = Curve::keys({{0.0F, 0.75F}, {0.16F, 0.0F}});
    effect.emitters.push_back(column);

    // 3: the shockwave, as a bend in the picture.
    EmitterDesc shock;
    shock.material = Material::shockwave;
    shock.burst = 1;
    shock.life = {0.36F, 0.36F};
    shock.speed = {0.0F, 0.0F};
    shock.size = {0.7F * size, 0.7F * size};
    shock.size_over_life = Curve::keys({{0.0F, 1.0F}, {0.5F, 7.0F}, {1.0F, 10.0F}});
    shock.alpha = Curve::keys({{0.0F, 1.0F}, {0.5F, 0.7F}, {1.0F, 0.0F}});
    shock.distortion = 0.055F;
    effect.emitters.push_back(shock);

    // 4 + 5: sparks that bounce, each leaving a short glowing trail.
    EmitterDesc sparks = spark_base();
    sparks.burst = count(44.0F);
    sparks.life = {0.5F, 1.45F};
    sparks.speed = {5.0F * size, 15.0F * size};
    sparks.drag = 0.9F;
    sparks.spread = 0.6F;
    sparks.gravity = 11.0F;
    sparks.sub_emitter = 5;
    sparks.sub_rate = 26.0F;
    effect.emitters.push_back(sparks);
    EmitterDesc trail;
    trail.material = Material::spark;
    trail.sub_only = true;
    trail.life = {0.14F, 0.30F};
    trail.speed = {0.0F, 0.2F};
    trail.size = {0.008F, 0.013F};
    trail.emissive = Curve::line(4.0F, 0.0F);
    trail.color = ColorCurve::keys({{0.0F, {1.0F, 0.55F, 0.16F}}, {1.0F, ember_dying}});
    effect.emitters.push_back(trail);

    // 6: embers that drift down through the smoke.
    EmitterDesc embers = spark_base();
    embers.burst = count(26.0F);
    embers.life = {1.6F, 3.3F};
    embers.speed = {2.0F * size, 8.0F * size};
    embers.spread = 0.8F;
    embers.gravity = 1.7F;
    embers.drag = 1.4F;
    embers.turbulence = 2.6F;
    embers.turbulence_scale = 1.2F;
    embers.bounce = 0.2F;
    embers.size = {0.008F, 0.016F};
    embers.stretch = 0.01F;
    embers.emissive = Curve::keys({{0.0F, 8.0F}, {0.7F, 3.0F}, {1.0F, 0.0F}});
    embers.color = ColorCurve::keys({{0.0F, spark_hot}, {0.4F, spark_cool}, {1.0F, ember_dying}});
    effect.emitters.push_back(embers);

    // 7: chunks of what was broken. They arc, bounce and settle.
    EmitterDesc debris;
    debris.material = Material::debris;
    debris.burst = count(15.0F);
    debris.shape = Shape::sphere;
    debris.shape_radius = 0.5F * size;
    debris.life = {2.3F, 3.5F};
    debris.speed = {5.0F * size, 13.0F * size};
    debris.spread = 0.52F;
    debris.gravity = 17.0F;
    debris.drag = 0.15F;
    debris.collide = true;
    debris.bounce = 0.38F;
    debris.friction = 0.45F;
    debris.size = {0.05F, 0.115F};
    debris.spin = {-9.0F, 9.0F};
    debris.alpha = Curve::keys({{0.0F, 1.0F}, {0.86F, 1.0F}, {1.0F, 0.0F}});
    effect.emitters.push_back(debris);

    // 8: dust driven out along the ground.
    EmitterDesc ring = smoke_base(dust, {0.56F, 0.53F, 0.48F}, 0.40F);
    ring.burst = count(18.0F);
    ring.shape = Shape::ring;
    ring.shape_radius = 0.8F * size;
    ring.offset = {0.0F, 0.25F, 0.0F};
    ring.life = {1.2F, 2.3F};
    ring.speed = {3.6F * size, 7.2F * size};
    ring.spread = 0.22F;
    ring.drag = 3.0F;
    ring.buoyancy = 0.15F;
    ring.turbulence = 0.8F;
    ring.turbulence_scale = 2.0F;
    ring.collide = true;
    ring.size = {0.35F * size, 0.55F * size};
    ring.size_over_life = Curve::keys({{0.0F, 0.7F}, {0.3F, 2.0F}, {1.0F, 3.1F}});
    effect.emitters.push_back(ring);

    // 9: what hangs in the air afterwards.
    EmitterDesc linger = smoke_base(dark_smoke, grey_smoke, 0.50F);
    linger.rate = 4.0F * size;
    linger.delay = 0.8F;
    linger.duration = 3.4F;
    linger.shape = Shape::sphere;
    linger.shape_radius = 0.9F * size;
    linger.life = {4.0F, 7.0F};
    linger.speed = {0.2F, 0.7F};
    linger.spread = 0.4F;
    linger.buoyancy = 0.3F;
    linger.drag = 0.7F;
    linger.turbulence = 0.7F;
    linger.turbulence_scale = 3.0F;
    linger.collide = true;
    linger.size = {0.8F * size, 1.2F * size};
    linger.size_over_life = Curve::line(1.0F, 2.4F);
    effect.emitters.push_back(linger);

    // 10: tongues of flame licking up through the first half second.
    EmitterDesc licks;
    licks.material = Material::flame;
    licks.burst = count(8.0F);
    licks.shape = Shape::sphere;
    licks.shape_radius = 0.6F * size;
    licks.life = {0.35F, 0.72F};
    licks.speed = {1.0F * size, 3.6F * size};
    licks.spread = 0.5F;
    licks.buoyancy = 2.6F;
    licks.drag = 1.5F;
    licks.turbulence = 1.5F;
    licks.turbulence_scale = 1.0F;
    licks.size = {0.38F * size, 0.62F * size};
    licks.size_over_life = Curve::keys({{0.0F, 0.6F}, {0.3F, 1.2F}, {1.0F, 1.5F}});
    licks.rotation = {-0.4F, 0.4F};
    licks.alpha = Curve::keys({{0.0F, 0.9F}, {0.6F, 0.7F}, {1.0F, 0.0F}});
    licks.temperature = Curve::line(2.1F, 0.75F);
    licks.emissive = Curve::constant(1.5F);
    licks.erosion = Curve::line(0.0F, 0.6F);
    licks.frame_rate = 20.0F;
    licks.color = ColorCurve::constant(soot);
    effect.emitters.push_back(licks);

    // 11: the hot air above it.
    EmitterDesc shimmer;
    shimmer.material = Material::heat;
    shimmer.burst = 3;
    shimmer.shape = Shape::sphere;
    shimmer.shape_radius = 0.6F * size;
    shimmer.life = {1.0F, 1.5F};
    shimmer.speed = {0.6F, 1.4F};
    shimmer.spread = 0.2F;
    shimmer.size = {1.1F * size, 1.5F * size};
    shimmer.size_over_life = Curve::line(1.0F, 2.0F);
    shimmer.alpha = Curve::keys({{0.0F, 0.0F}, {0.2F, 1.0F}, {1.0F, 0.0F}});
    shimmer.distortion = 0.007F;
    effect.emitters.push_back(shimmer);

    LightDesc blast;
    blast.color = {1.0F, 0.62F, 0.30F};
    blast.radius = 15.0F * size;
    blast.duration = 0.5F;
    blast.intensity = Curve::keys({{0.0F, 6.0F}, {0.12F, 3.2F}, {0.5F, 1.2F}, {1.0F, 0.0F}});
    blast.offset = {0.0F, 0.6F, 0.0F};
    effect.lights.push_back(blast);
    LightDesc afterglow;
    afterglow.color = {1.0F, 0.42F, 0.14F};
    afterglow.radius = 9.0F * size;
    afterglow.duration = 1.3F;
    afterglow.intensity = Curve::keys({{0.0F, 0.0F}, {0.3F, 1.1F}, {1.0F, 0.0F}});
    afterglow.flicker = 0.3F;
    afterglow.offset = {0.0F, 1.2F, 0.0F};
    effect.lights.push_back(afterglow);
    return effect;
}

} // namespace

namespace effects
{

const EffectDesc &impact_puff()
{
    static const EffectDesc desc = make_impact_puff();
    return desc;
}
const EffectDesc &dust_puff()
{
    static const EffectDesc desc = make_dust_puff();
    return desc;
}
const EffectDesc &lingering_smoke()
{
    static const EffectDesc desc = make_lingering_smoke();
    return desc;
}
const EffectDesc &fuse_smoke()
{
    static const EffectDesc desc = make_fuse_smoke();
    return desc;
}
const EffectDesc &torch_flame()
{
    static const EffectDesc desc = make_flame("torch_flame", 1.0F, 5.5F, 0.30F);
    return desc;
}
const EffectDesc &fire()
{
    static const EffectDesc desc = make_flame("fire", 3.0F, 10.0F, 1.2F);
    return desc;
}
const EffectDesc &lava_bubble()
{
    static const EffectDesc desc = make_lava_bubble();
    return desc;
}
const EffectDesc &lava_spurt()
{
    static const EffectDesc desc = make_lava_spurt();
    return desc;
}
const EffectDesc &rocket_trail()
{
    static const EffectDesc desc = make_rocket_trail();
    return desc;
}
const EffectDesc &explosion_grenade()
{
    static const EffectDesc desc = make_explosion("explosion_grenade", 0.8F);
    return desc;
}
const EffectDesc &explosion_rocket()
{
    static const EffectDesc desc = make_explosion("explosion_rocket", 1.0F);
    return desc;
}
const EffectDesc &explosion_tnt()
{
    static const EffectDesc desc = make_explosion("explosion_tnt", 1.3F);
    return desc;
}
const EffectDesc &explosion_c4()
{
    static const EffectDesc desc = make_explosion("explosion_c4", 1.75F);
    return desc;
}

} // namespace effects

// The weapon, dragon, mining and creature recipes (vfx_library_combat.cpp,
// declared in openminerz/vfx_combat.hpp).
[[nodiscard]] std::span<const EffectDesc *const> combat_effects();

std::span<const EffectDesc *const> all_effects()
{
    // Add a recipe here to make it reachable by name.
    static const std::array<const EffectDesc *, 13> own{
        &effects::impact_puff(),       &effects::dust_puff(),        &effects::lingering_smoke(),
        &effects::fuse_smoke(),        &effects::torch_flame(),      &effects::fire(),
        &effects::lava_bubble(),       &effects::lava_spurt(),       &effects::rocket_trail(),
        &effects::explosion_grenade(), &effects::explosion_rocket(), &effects::explosion_tnt(),
        &effects::explosion_c4(),
    };
    // ...followed by the combat library's, which lists its own.
    static const std::vector<const EffectDesc *> list = [] {
        std::vector<const EffectDesc *> all{own.begin(), own.end()};
        const std::span<const EffectDesc *const> combat = combat_effects();
        all.insert(all.end(), combat.begin(), combat.end());
        return all;
    }();
    return list;
}

const EffectDesc *find_effect(std::string_view name) noexcept
{
    for (const EffectDesc *effect : all_effects())
        if (effect->name == name)
            return effect;
    return nullptr;
}

} // namespace openminerz::vfx
