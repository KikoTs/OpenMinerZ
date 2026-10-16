// The Enhanced presentation's effects layer (openminerz/vfx.hpp): the parts
// that are arithmetic. Curves, emitters, budgets and eviction, sort keys, light
// selection, the near fade that keeps smoke off the lens, the smoke the eye
// stands in, and every shipped recipe. No graphics device.

#include "openminerz/graphics_settings.hpp"
#include "openminerz/settings.hpp"
#include "openminerz/vfx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;
using namespace openminerz::vfx;

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

// A flat floor at y < 0 under open sky.
class FloorWorld final : public WorldQuery
{
  public:
    [[nodiscard]] bool solid(int, int y, int) const override
    {
        return y < 0;
    }
    [[nodiscard]] LightSample light(Vec3) const override
    {
        return {1.0F, 0.0F};
    }
};

void curves_interpolate_and_hold_their_ends()
{
    const Curve line = Curve::line(2.0F, 6.0F);
    check(near(line.at(0.0F), 2.0F) && near(line.at(0.5F), 4.0F) && near(line.at(1.0F), 6.0F),
          "a line runs straight between its ends");
    check(near(line.at(-1.0F), 2.0F) && near(line.at(3.0F), 6.0F),
          "a curve holds its first and last values outside 0..1");
    const Curve keys = Curve::keys({{0.0F, 0.0F}, {0.1F, 1.0F}, {0.5F, 0.5F}, {1.0F, 0.0F}});
    check(near(keys.at(0.05F), 0.5F) && near(keys.at(0.1F), 1.0F) && near(keys.at(0.3F), 0.75F) &&
              near(keys.at(0.75F), 0.25F),
          "keys are joined by straight segments");
    check(near(Curve::constant(3.0F).at(0.7F), 3.0F), "a constant is constant");
    check(near(Curve{}.at(0.4F), 1.0F), "the default curve is one, so an unset gain is neutral");
    const ColorCurve colour =
        ColorCurve::keys({{0.0F, {1.0F, 0.0F, 0.0F}}, {1.0F, {0.0F, 0.0F, 1.0F}}});
    const Color middle = colour.at(0.5F);
    check(near(middle.r, 0.5F) && near(middle.g, 0.0F) && near(middle.b, 0.5F),
          "a colour curve blends each channel");
    check(near(Range{2.0F, 4.0F}.pick(0.25F), 2.5F) && near(Range{3.0F}.pick(0.9F), 3.0F),
          "a range picks between its bounds; a single value is fixed");
}

void curl_noise_is_smooth_periodic_and_divergence_free()
{
    const Vec3 here = curl_noise({3.3F, 7.1F, 12.6F});
    const Vec3 wrapped = curl_noise({3.3F + 32.0F, 7.1F - 64.0F, 12.6F + 32.0F});
    check(near(here.x, wrapped.x, 0.001F) && near(here.y, wrapped.y, 0.001F) &&
              near(here.z, wrapped.z, 0.001F),
          "the swirl repeats every 32 units, so it can be read anywhere");
    // Flux through a small box round a few points is next to nothing compared
    // with the field itself: particles swirl, they do not bunch up or thin out.
    float worst{};
    float magnitude{};
    constexpr float step = 0.5F;
    for (int sample = 0; sample < 40; ++sample)
    {
        const Vec3 p{static_cast<float>(sample) * 1.37F, static_cast<float>(sample) * 0.91F + 2.0F,
                     static_cast<float>(sample) * 2.13F + 5.0F};
        const float divergence =
            (curl_noise({p.x + step, p.y, p.z}).x - curl_noise({p.x - step, p.y, p.z}).x +
             curl_noise({p.x, p.y + step, p.z}).y - curl_noise({p.x, p.y - step, p.z}).y +
             curl_noise({p.x, p.y, p.z + step}).z - curl_noise({p.x, p.y, p.z - step}).z) /
            (2.0F * step);
        worst = std::max(worst, std::abs(divergence));
        magnitude += length(curl_noise(p));
    }
    magnitude /= 40.0F;
    check(magnitude > 0.4F && magnitude < 2.0F, "the swirl has about unit strength");
    check(worst < 0.12F * magnitude, "the swirl must not compress or thin the particles");
    check(length(curl_noise({1.0F, 1.0F, 1.0F}) - curl_noise({1.02F, 1.0F, 1.0F})) < 0.05F,
          "neighbouring points swirl alike");
}

void sort_keys_put_the_farthest_first()
{
    check(depth_sort_key(1.0F) > depth_sort_key(2.0F) &&
              depth_sort_key(2.0F) > depth_sort_key(50.0F) &&
              depth_sort_key(50.0F) > depth_sort_key(399.0F),
          "a larger depth must give a smaller key");
    check(depth_sort_key(400.0F) == 0U && depth_sort_key(4000.0F) == 0U,
          "everything past the far depth shares the first place");
    check(depth_sort_key(0.0F) == 65535U && depth_sort_key(-3.0F) == 65535U,
          "what is at or behind the eye is drawn last");
    check(depth_sort_key(2.00F) != depth_sort_key(2.05F),
          "two puffs five centimetres apart near the eye must still be ordered");

    std::vector<Instance> instances;
    std::vector<std::uint16_t> keys;
    std::uint32_t state = 12345U;
    for (int index = 0; index < 700; ++index)
    {
        state = state * 1664525U + 1013904223U;
        const float depth = static_cast<float>((state >> 8U) % 30000U) / 100.0F;
        Instance instance;
        instance.position[0] = depth;
        // Same depth, different arrival order: the sort must keep it.
        instance.position[1] = static_cast<float>(index);
        instances.push_back(instance);
        keys.push_back(depth_sort_key(depth));
    }
    std::vector<Instance> scratch;
    std::vector<std::uint16_t> key_scratch;
    sort_by_key(instances, keys, scratch, key_scratch);
    bool ordered = true;
    for (std::size_t index = 1U; index < instances.size(); ++index)
    {
        ordered = ordered && keys[index - 1U] <= keys[index];
        if (keys[index - 1U] == keys[index])
            ordered = ordered && instances[index - 1U].position[1] < instances[index].position[1];
    }
    check(ordered, "the radix sort is ascending by key and stable");
    check(instances.front().position[0] >= instances.back().position[0],
          "the first instance drawn is the farthest");
}

void the_near_fade_keeps_particles_off_the_lens()
{
    check(near(near_fade(0.0F, 1.0F), 0.0F) && near(near_fade(0.5F, 1.0F), 0.0F),
          "a puff the eye is inside draws nothing as a card");
    check(near(near_fade(6.0F, 1.0F), 1.0F), "a puff a few of its sizes away is whole");
    float previous = 0.0F;
    for (int step = 0; step <= 60; ++step)
    {
        const float fade = near_fade(static_cast<float>(step) * 0.1F, 0.8F);
        check(fade >= previous - 1.0e-6F, "the fade only grows with distance");
        previous = fade;
    }
    // The fade is done before the card can be much wider than the view: at the
    // distance it reaches full strength a card spans under about 70 degrees.
    for (const float size : {0.1F, 0.5F, 1.0F, 3.0F})
    {
        float full = 0.0F;
        while (near_fade(full, size) < 0.999F)
            full += 0.01F;
        check(size / full < 0.72F, "a full-strength card is never a wall across the screen");
    }
    check(near(light_falloff(0.0F, 8.0F), 1.0F) && near(light_falloff(8.0F, 8.0F), 0.0F) &&
              near(light_falloff(4.0F, 8.0F), 0.25F) && near(light_falloff(3.0F, 0.0F), 0.0F),
          "a light fades to nothing at its radius");
}

void lights_are_chosen_by_what_the_eye_sees_of_them()
{
    std::vector<PointLight> lights;
    for (int index = 0; index < 40; ++index)
    {
        PointLight light;
        light.position = {static_cast<float>(index) * 4.0F, 0.0F, 0.0F};
        light.radius = 8.0F;
        light.color = {1.0F, 0.6F, 0.3F};
        lights.push_back(light);
    }
    PointLight bright;
    bright.position = {150.0F, 0.0F, 0.0F};
    bright.radius = 30.0F;
    bright.color = {40.0F, 30.0F, 20.0F};
    lights.push_back(bright);
    select_lights(lights, {0.0F, 0.0F, 0.0F}, 8U);
    check(lights.size() == 8U, "the cap holds");
    bool nearest = false;
    bool far_bright = false;
    for (const PointLight &light : lights)
    {
        nearest = nearest || near(light.position.x, 0.0F);
        far_bright = far_bright || near(light.position.x, 150.0F);
        check(light.position.x <= 28.0F + 0.01F || near(light.position.x, 150.0F),
              "a dim distant light must not displace a near one");
    }
    check(nearest && far_bright, "the nearest light and a far, very bright one both make the cut");
    std::vector<PointLight> few(3U);
    select_lights(few, {}, 8U);
    check(few.size() == 3U, "fewer lights than the cap are all kept");
}

void levels_of_detail_thin_and_then_cull()
{
    EffectDesc desc;
    desc.lod_start = 10.0F;
    desc.lod_end = 30.0F;
    desc.lod_floor = 0.2F;
    desc.cull_distance = 50.0F;
    check(near(lod_factor(desc, 0.0F), 1.0F) && near(lod_factor(desc, 10.0F), 1.0F),
          "full detail up close");
    check(near(lod_factor(desc, 20.0F), 0.6F) && near(lod_factor(desc, 30.0F), 0.2F) &&
              near(lod_factor(desc, 45.0F), 0.2F),
          "emission thins to the floor and stays there");
    check(near(lod_factor(desc, 50.0F), 0.0F) && near(lod_factor(desc, 500.0F), 0.0F),
          "nothing is spawned beyond the cull distance");
    const Budget low = budget_for_density(0.2F);
    const Budget high = budget_for_density(1.0F);
    const Budget ultra = budget_for_density(1.5F);
    check(low.max_particles < high.max_particles && high.max_particles <= ultra.max_particles &&
              low.max_lights < high.max_lights && high.max_lights <= ultra.max_lights &&
              ultra.max_lights <= 16U,
          "budgets grow with the particle level and the lights fit the shader's table");
}

void emitters_burst_stream_and_expire()
{
    EffectDesc desc;
    desc.name = "test";
    EmitterDesc burst;
    burst.burst = 10;
    burst.life = {1.0F, 1.0F};
    burst.speed = {2.0F, 2.0F};
    burst.spread = 0.0F;
    desc.emitters.push_back(burst);
    EmitterDesc stream;
    stream.rate = 20.0F;
    stream.delay = 0.5F;
    stream.duration = 1.0F;
    stream.life = {0.25F, 0.25F};
    desc.emitters.push_back(stream);

    System system{4096U};
    SpawnParams params;
    params.position = {0.0F, 10.0F, 0.0F};
    const EffectHandle handle = system.spawn(desc, params);
    check(handle.valid() && system.alive(handle) && system.effect_count() == 1U,
          "an effect starts with a valid handle");
    system.update(0.1F, {});
    check(system.particle_count() == 10U, "the burst is emitted at once, the stream not before its delay");
    for (int step = 0; step < 4; ++step)
        system.update(0.1F, {});
    // Half a second in: the burst is still alive, the stream is about to start.
    check(system.particle_count() == 10U, "nothing streams before the delay");
    for (int step = 0; step < 5; ++step)
        system.update(0.1F, {});
    // One second in: the burst has just expired; the stream has run 0.5 s at
    // 20 per second with a quarter-second life, so about five are alive.
    const std::size_t streaming = system.particle_count();
    check(streaming >= 3U && streaming <= 8U, "a stream keeps rate x life particles alive");
    for (int step = 0; step < 12; ++step)
        system.update(0.1F, {});
    check(system.particle_count() == 0U && !system.alive(handle) && system.effect_count() == 0U,
          "a finished effect frees its slot");
    check(!system.alive(EffectHandle{}), "the default handle is never alive");

    // The same recipe at twice the scale throws its particles twice as far.
    System scaled{4096U};
    SpawnParams big = params;
    big.scale = 2.0F;
    (void)scaled.spawn(desc, big);
    scaled.update(0.1F, {});
    Frame frame;
    View view;
    view.eye = {0.0F, 10.0F, 30.0F};
    scaled.build_frame(view, frame);
    check(!frame.soft.empty(), "the burst is drawn");
    // Speed 2 x scale 2 along +Y for 0.1 s.
    check(near(frame.soft.front().position[1], 10.4F, 0.02F),
          "scale multiplies speed as well as size");
}

void streams_follow_a_moving_effect_and_inherit_its_velocity()
{
    EffectDesc desc;
    EmitterDesc trail;
    trail.rate = 100.0F;
    trail.loop = true;
    trail.life = {5.0F, 5.0F};
    trail.speed = {0.0F, 0.0F};
    trail.inherit_velocity = 0.5F;
    desc.emitters.push_back(trail);
    System system{4096U};
    SpawnParams params;
    const EffectHandle handle = system.spawn(desc, params);
    system.update(0.1F, {});
    system.move(handle, {10.0F, 0.0F, 0.0F}, {100.0F, 0.0F, 0.0F});
    system.update(0.1F, {});
    Frame frame;
    View view;
    view.eye = {5.0F, 0.0F, 40.0F};
    system.build_frame(view, frame);
    float lowest = 1.0e9F;
    float highest = -1.0e9F;
    for (const Instance &instance : frame.soft)
    {
        lowest = std::min(lowest, instance.position[0]);
        highest = std::max(highest, instance.position[0]);
    }
    // Laid along the ten blocks it travelled (and carried on by the half of
    // its velocity they kept), not dropped in one clump at either end.
    check(highest - lowest > 6.0F, "a stream is laid along the path of the thing it follows");
    system.kill(handle);
    const std::size_t before = system.particle_count();
    system.update(0.1F, {});
    check(system.particle_count() <= before, "a killed effect emits nothing more");
    check(system.particle_count() > 0U, "but what it emitted lives on");
    system.kill(system.spawn(desc, params), true);
    check(system.effect_count() <= 1U, "an immediate kill frees the slot at once");
}

void budgets_hold_and_the_oldest_give_way()
{
    EffectDesc filler;
    EmitterDesc many;
    many.burst = 400;
    many.life = {10.0F, 10.0F};
    filler.emitters.push_back(many);
    filler.max_particles = 150;

    System system{4096U};
    Budget budget;
    budget.max_particles = 256U;
    system.set_budget(budget);
    const EffectHandle first = system.spawn(filler, {});
    system.update(0.01F, {});
    check(system.particle_count(first) == 150U, "one effect never exceeds its own cap");
    (void)system.spawn(filler, {});
    (void)system.spawn(filler, {});
    system.update(0.01F, {});
    check(system.particle_count() <= 256U, "the global budget holds");
    check(system.dropped_particles() > 0U, "and the particles that did not fit were counted");

    // A more important effect arriving into a full pool takes the place of
    // particles that are nearly spent.
    for (int step = 0; step < 80; ++step)
        system.update(0.1F, {});
    EffectDesc important = filler;
    important.priority = 5;
    important.max_particles = 64;
    const EffectHandle urgent = system.spawn(important, {});
    system.update(0.01F, {});
    check(system.particle_count(urgent) > 0U,
          "a high-priority burst finds room among old particles");
    check(system.particle_count() <= 256U, "without going over the budget");

    // Slots are bounded too, and a full table refuses politely.
    System crowded{512U};
    EffectDesc forever;
    EmitterDesc loop;
    loop.rate = 1.0F;
    loop.loop = true;
    forever.emitters.push_back(loop);
    std::size_t started = 0U;
    for (std::size_t index = 0U; index < System::max_effects + 20U; ++index)
        started += crowded.spawn(forever, {}).valid() ? 1U : 0U;
    check(started == System::max_effects, "the effect table is bounded");
}

void sparks_and_debris_stop_at_terrain()
{
    const FloorWorld world;
    EffectDesc desc;
    EmitterDesc chunks;
    chunks.material = Material::debris;
    chunks.burst = 24;
    chunks.life = {6.0F, 6.0F};
    chunks.speed = {2.0F, 5.0F};
    chunks.spread = 0.4F;
    chunks.gravity = 17.0F;
    chunks.collide = true;
    chunks.size = {0.05F, 0.1F};
    desc.emitters.push_back(chunks);
    System system{1024U};
    system.set_world(&world);
    SpawnParams params;
    params.position = {0.0F, 1.5F, 0.0F};
    (void)system.spawn(desc, params);
    system.update(1.0F / 60.0F, {});
    check(system.particle_count() == 0U, "debris with no block named is not spawned");
    params.debris_tiles = {7U, 9U, 0U, 0U};
    params.debris_tile_count = 2U;
    (void)system.spawn(desc, params);
    for (int step = 0; step < 300; ++step)
        system.update(1.0F / 60.0F, {});
    Frame frame;
    system.build_frame(View{}, frame);
    check(frame.debris.size() == 24U, "every chunk is drawn as a cube");
    for (const DebrisCube &cube : frame.debris)
    {
        check(cube.position.y >= -0.001F, "no chunk falls through the floor");
        check(cube.position.y < 0.5F, "after five seconds every chunk has come to rest on it");
        check(cube.tile == 7U || cube.tile == 9U, "chunks wear the faces they were given");
    }
}

void the_smoke_round_the_eye_becomes_fog_not_cards()
{
    EffectDesc desc;
    EmitterDesc cloud;
    cloud.burst = 60;
    cloud.shape = Shape::sphere;
    cloud.shape_radius = 1.5F;
    cloud.life = {20.0F, 20.0F};
    cloud.speed = {0.0F, 0.0F};
    cloud.size = {1.2F, 1.2F};
    cloud.alpha = Curve::constant(0.6F);
    cloud.color = ColorCurve::constant({0.3F, 0.3F, 0.3F});
    desc.emitters.push_back(cloud);
    desc.max_particles = 100;
    System system{1024U};
    (void)system.spawn(desc, {});
    system.update(0.01F, {});

    Frame inside;
    View view;
    system.build_frame(view, inside);
    check(inside.immersion > 0.5F, "an eye in the middle of thick smoke stands in fog");
    check(near(inside.immersion_color.r, 0.3F, 0.01F) && inside.immersion_reach > 1.0F,
          "the fog has the smoke's own colour and reach");
    // What a card may cover is bounded: no puff near the lens is drawn at
    // anything like full strength.
    float covered{};
    for (const Instance &instance : inside.soft)
    {
        const float distance = std::sqrt(instance.position[0] * instance.position[0] +
                                         instance.position[1] * instance.position[1] +
                                         instance.position[2] * instance.position[2]);
        if (distance < 0.7F)
            check(instance.alpha < 0.01F, "a puff on the lens is not drawn as a card");
        covered += instance.alpha;
    }
    check(covered < 60.0F * 0.6F * 0.6F, "most of the cloud round the eye is fog, not cards");

    Frame outside;
    view.eye = {0.0F, 0.0F, 30.0F};
    system.build_frame(view, outside);
    check(near(outside.immersion, 0.0F), "seen from outside there is no fog over the picture");
    check(outside.soft.size() == 60U, "and every puff is drawn");
    for (std::size_t index = 1U; index < outside.soft.size(); ++index)
        check(outside.soft[index - 1U].position[2] <= outside.soft[index].position[2] + 0.02F,
              "puffs are drawn farthest first");
}

void lights_shake_and_flash_follow_the_effect()
{
    EffectDesc desc;
    EmitterDesc puff;
    puff.burst = 1;
    puff.life = {3.0F, 3.0F};
    desc.emitters.push_back(puff);
    LightDesc light;
    light.color = {1.0F, 0.5F, 0.25F};
    light.radius = 10.0F;
    light.duration = 1.0F;
    light.intensity = Curve::line(4.0F, 0.0F);
    desc.lights.push_back(light);
    desc.shake = 0.8F;
    desc.shake_duration = 0.5F;
    desc.shake_radius = 20.0F;
    desc.flash = 0.6F;
    desc.flash_duration = 0.2F;
    desc.flash_radius = 20.0F;

    System system{256U};
    SpawnParams params;
    params.position = {0.0F, 0.0F, -5.0F};
    params.scale = 2.0F;
    (void)system.spawn(desc, params);
    system.update(0.05F, {});
    Frame frame;
    View view;
    system.build_frame(view, frame);
    check(frame.lights.size() == 1U && near(frame.lights[0].radius, 20.0F),
          "the light's radius follows the effect's scale");
    check(frame.lights[0].color.r > 3.5F && frame.lights[0].color.r < 4.0F,
          "its strength follows its curve");
    check(frame.shake > 0.2F && frame.shake <= 1.0F && frame.flash > 0.1F && frame.flash <= 1.0F,
          "a blast five blocks away shakes and brightens the picture");
    view.eye = {0.0F, 0.0F, 500.0F};
    system.build_frame(view, frame);
    check(near(frame.shake, 0.0F) && near(frame.flash, 0.0F),
          "a blast half a kilometre away does neither");
    for (int step = 0; step < 30; ++step)
        system.update(0.05F, {});
    system.build_frame(View{}, frame);
    check(frame.lights.empty() && near(frame.shake, 0.0F) && near(frame.flash, 0.0F),
          "light, shake and flash end when their time is up");
}

void every_recipe_is_sound()
{
    const auto recipes = all_effects();
    check(recipes.size() >= 13U, "the smoke, fire and explosion recipes are registered");
    for (const EffectDesc *recipe : recipes)
    {
        const std::string name{recipe->name};
        check(!name.empty() && find_effect(recipe->name) == recipe, name + ": is found by its name");
        // (A recipe may be a light alone, or nothing at all where a
        // combination has nothing to show; the system runs both.)
        check(recipe->emitters.size() <= System::max_emitters,
              name + ": has no more emitters than the system runs");
        check(recipe->max_particles > 0 && recipe->cull_distance > recipe->lod_start,
              name + ": has a budget and a cull distance");
        bool loops = false;
        for (const EmitterDesc &emitter : recipe->emitters)
        {
            loops = loops || emitter.loop;
            check(emitter.life.min > 0.0F && emitter.life.max >= emitter.life.min,
                  name + ": every particle has a life");
            check(emitter.size.min > 0.0F, name + ": every particle has a size");
            check(emitter.sub_emitter < static_cast<int>(recipe->emitters.size()),
                  name + ": a sub-emitter names an emitter of the same effect");
            if (emitter.sub_emitter >= 0)
                check(recipe->emitters[static_cast<std::size_t>(emitter.sub_emitter)].sub_only,
                      name + ": a sub-emitter's target emits only as one");
            if (emitter.sub_only)
            {
                // Something must drive it, or it is dead weight in the recipe.
                bool driven = false;
                for (const EmitterDesc &parent : recipe->emitters)
                    driven = driven ||
                             (parent.sub_emitter >= 0 &&
                              &recipe->emitters[static_cast<std::size_t>(parent.sub_emitter)] ==
                                  &emitter &&
                              (parent.sub_rate > 0.0F || parent.sub_on_death > 0));
                check(driven, name + ": a sub-only emitter is driven by a parent");
            }
        }
        for (const LightDesc &light : recipe->lights)
            check(light.radius > 0.0F && (light.duration > 0.0F || loops),
                  name + ": a light either ends or belongs to a looping effect");

        // Run it: it must stay within its budget, and a one-shot must end.
        System system{16384U};
        const FloorWorld world;
        system.set_world(&world);
        SpawnParams params;
        params.position = {0.0F, 1.0F, 0.0F};
        params.debris_tiles = {1U, 2U, 3U, 4U};
        params.debris_tile_count = 4U;
        const EffectHandle handle = system.spawn(*recipe, params);
        std::size_t peak = 0U;
        for (int step = 0; step < 60 * 14; ++step)
        {
            system.update(1.0F / 60.0F, {0.0F, 1.0F, 6.0F});
            peak = std::max(peak, system.particle_count());
        }
        constexpr std::array<std::string_view, 13> shipped{
            "impact_puff",       "dust_puff",        "lingering_smoke", "fuse_smoke",
            "torch_flame",       "fire",             "lava_bubble",     "lava_spurt",
            "rocket_trail",      "explosion_grenade", "explosion_rocket", "explosion_tnt",
            "explosion_c4"};
        if (std::find(shipped.begin(), shipped.end(), recipe->name) != shipped.end())
            check(peak > 0U, name + ": shows something");
        check(peak <= static_cast<std::size_t>(recipe->max_particles),
              name + ": stays within its own particle budget");
        if (loops)
        {
            check(system.alive(handle), name + ": a looping effect runs until it is stopped");
            system.kill(handle);
            for (int step = 0; step < 60 * 12; ++step)
                system.update(1.0F / 60.0F, {0.0F, 1.0F, 6.0F});
        }
        check(!system.alive(handle) && system.particle_count() == 0U,
              name + ": ends, and leaves nothing behind");
    }
    check(find_effect("no_such_effect") == nullptr, "an unknown name finds nothing");
    // Explosions are sized in the order of the charges that cause them.
    check(effects::explosion_grenade().max_particles < effects::explosion_rocket().max_particles &&
              effects::explosion_rocket().max_particles < effects::explosion_tnt().max_particles &&
              effects::explosion_tnt().max_particles < effects::explosion_c4().max_particles,
          "grenade < rocket < TNT < C4");
}

void the_settings_reach_the_renderer_and_classic_gets_none()
{
    Settings settings;
    settings.presentation = PresentationProfile::rebirth;
    const PostEffects defaults = resolve_post_effects(settings);
    check(defaults.effect_lights > 0 && defaults.effect_distortion && defaults.effect_shake > 0.0F &&
              defaults.effect_flash > 0.0F && defaults.effect_resolution == 2,
          "by default effects light the world, bend the picture, shake and flash, at half resolution");
    settings.graphics.effect_lighting = false;
    settings.graphics.distortion_effects = false;
    settings.graphics.explosion_shake = 0.0F;
    settings.graphics.explosion_flash = false;
    const PostEffects off = resolve_post_effects(settings);
    check(off.effect_lights == 0 && !off.effect_distortion && near(off.effect_shake, 0.0F) &&
              near(off.effect_flash, 0.0F),
          "each option switches its part off");
    settings.graphics = GraphicsSettings{};
    settings.reduced_motion = true;
    const PostEffects still = resolve_post_effects(settings);
    check(!still.effect_distortion && near(still.effect_shake, 0.0F) &&
              near(still.effect_flash, 0.0F) && still.effect_lights > 0,
          "Reduced motion removes the bend, the shake and the flash, and keeps the light");
    settings.reduced_motion = false;
    settings.particle_quality = QualityLevel::ultra;
    check(resolve_post_effects(settings).effect_resolution == 1,
          "the Ultra particle level draws gas at full resolution");
    settings.presentation = PresentationProfile::classic;
    const PostEffects classic = resolve_post_effects(settings);
    check(!classic.enabled && classic.effect_lights == 0 && !classic.effect_distortion &&
              near(classic.effect_shake, 0.0F) && near(classic.effect_flash, 0.0F),
          "the Original presentation is handed nothing of the effects layer");

    GraphicsSettings low;
    apply_graphics_preset(low, 0);
    GraphicsSettings high;
    apply_graphics_preset(high, 2);
    check(!low.effect_lighting && !low.distortion_effects && high.effect_lighting &&
              high.distortion_effects,
          "LOW turns effect lighting and distortion off; HIGH has both");
    check(graphics_matches_preset(GraphicsSettings{}, 2, true, true),
          "a fresh install still reads HIGH");
    GraphicsSettings custom;
    custom.effect_lighting = false;
    check(!graphics_matches_preset(custom, 2, true, true),
          "changing a preset-controlled effect row makes the preset Custom");
    GraphicsSettings taste;
    taste.explosion_shake = 0.2F;
    taste.explosion_flash = false;
    check(graphics_matches_preset(taste, 2, true, true),
          "shake and flash are taste and never change the preset");
    GraphicsSettings wild;
    wild.explosion_shake = 7.0F;
    wild.normalize();
    check(near(wild.explosion_shake, 1.0F), "the shake is clamped to its range");
}

} // namespace

int main()
{
    struct Test final
    {
        const char *name;
        void (*run)();
    };
    const std::array tests{
        Test{"curves interpolate and hold their ends", curves_interpolate_and_hold_their_ends},
        Test{"curl noise is smooth, periodic and divergence-free",
             curl_noise_is_smooth_periodic_and_divergence_free},
        Test{"sort keys put the farthest first", sort_keys_put_the_farthest_first},
        Test{"the near fade keeps particles off the lens",
             the_near_fade_keeps_particles_off_the_lens},
        Test{"lights are chosen by what the eye sees of them",
             lights_are_chosen_by_what_the_eye_sees_of_them},
        Test{"levels of detail thin and then cull", levels_of_detail_thin_and_then_cull},
        Test{"emitters burst, stream and expire", emitters_burst_stream_and_expire},
        Test{"streams follow a moving effect and inherit its velocity",
             streams_follow_a_moving_effect_and_inherit_its_velocity},
        Test{"budgets hold and the oldest give way", budgets_hold_and_the_oldest_give_way},
        Test{"sparks and debris stop at terrain", sparks_and_debris_stop_at_terrain},
        Test{"the smoke round the eye becomes fog, not cards",
             the_smoke_round_the_eye_becomes_fog_not_cards},
        Test{"lights, shake and flash follow the effect",
             lights_shake_and_flash_follow_the_effect},
        Test{"every recipe is sound", every_recipe_is_sound},
        Test{"the settings reach the renderer and Classic gets none",
             the_settings_reach_the_renderer_and_classic_gets_none},
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
              << " effects-layer checks passed\n";
    return failures == 0 ? 0 : 1;
}
