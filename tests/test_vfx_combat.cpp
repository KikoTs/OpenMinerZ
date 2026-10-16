// The weapon, dragon, mining and creature effects of the Enhanced presentation
// (openminerz/vfx_combat.hpp): the recipes are well formed, every surface and
// strike has one, the worst cases stay inside their budgets, and nothing drawn
// is able to change what the simulation does. No graphics device is needed.

#include "openminerz/vfx.hpp"
#include "openminerz/vfx_combat.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/enemy.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
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

// Flat ground at y = 0 under open sky.
class FlatWorld final : public vfx::WorldQuery
{
  public:
    [[nodiscard]] bool solid(int, int y, int) const override
    {
        return y < 0;
    }
    [[nodiscard]] vfx::LightSample light(vfx::Vec3) const override
    {
        return {1.0F, 0.0F};
    }
};

[[nodiscard]] int burst_total(const vfx::EffectDesc &effect)
{
    int total{};
    for (const vfx::EmitterDesc &emitter : effect.emitters)
        if (!emitter.sub_only)
            total += emitter.burst;
    return total;
}

[[nodiscard]] const vfx::EmitterDesc *first_of(const vfx::EffectDesc &effect, vfx::Material material)
{
    for (const vfx::EmitterDesc &emitter : effect.emitters)
        if (emitter.material == material)
            return &emitter;
    return nullptr;
}

[[nodiscard]] int burst_of(const vfx::EffectDesc &effect, vfx::Material material)
{
    int total{};
    for (const vfx::EmitterDesc &emitter : effect.emitters)
        if (emitter.material == material)
            total += emitter.burst;
    return total;
}

void every_recipe_is_well_formed()
{
    const auto effects = vfx::combat_effects();
    check(effects.size() >= 90U, "the combat library lists every recipe");
    std::set<std::string_view> names;
    for (const vfx::EffectDesc *effect : effects)
    {
        check(effect != nullptr && !effect->name.empty(), "every recipe has a name");
        const std::string name{effect->name};
        check(names.insert(effect->name).second, name + " is listed once");
        check(!effect->emitters.empty(), name + " emits something");
        check(effect->emitters.size() <= vfx::System::max_emitters,
              name + " fits the emitter limit");
        check(effect->max_particles >= 1, name + " has a particle budget");
        check(effect->cull_distance > 0.0F && effect->lod_end >= effect->lod_start,
              name + " has a level of detail");
        check(burst_total(*effect) <= effect->max_particles,
              name + " can hold its own opening burst");
        std::vector<bool> referenced(effect->emitters.size(), false);
        for (const vfx::EmitterDesc &emitter : effect->emitters)
        {
            check(emitter.life.min > 0.0F && emitter.life.max >= emitter.life.min,
                  name + " particles live a positive time");
            check(emitter.size.min > 0.0F && emitter.size.max >= emitter.size.min,
                  name + " particles have a size");
            check(emitter.speed.max >= emitter.speed.min, name + " speed range is ordered");
            check(emitter.sub_only || emitter.burst > 0 || emitter.rate > 0.0F,
                  name + " has no emitter that never fires");
            if (emitter.sub_emitter >= 0)
            {
                const auto target = static_cast<std::size_t>(emitter.sub_emitter);
                check(target < effect->emitters.size(), name + " sub-emitter exists");
                check(effect->emitters[target].sub_only, name + " sub-emitter is sub-only");
                check(emitter.sub_rate > 0.0F || emitter.sub_on_death > 0,
                      name + " sub-emitter is driven");
                referenced[target] = true;
            }
        }
        for (std::size_t index = 0U; index < effect->emitters.size(); ++index)
            check(!effect->emitters[index].sub_only || referenced[index],
                  name + " has no orphan sub-emitter");
        for (const vfx::LightDesc &light : effect->lights)
            check(light.radius > 0.0F && light.radius <= 40.0F, name + " light has a sane reach");
    }
}

void every_surface_and_strike_has_a_recipe()
{
    std::set<const vfx::EffectDesc *> seen;
    for (int strike = 0; strike < static_cast<int>(vfx::Strike::count); ++strike)
        for (int surface = 0; surface < static_cast<int>(vfx::Surface::count); ++surface)
        {
            const vfx::EffectDesc &effect = vfx::effects::surface_strike(
                static_cast<vfx::Strike>(strike), static_cast<vfx::Surface>(surface));
            check(seen.insert(&effect).second, "each strike on each surface is its own recipe");
            check(effect.name.starts_with("strike_"), "strike recipes are named for what they are");
        }
    // Out-of-range values (a future enum value) fall back instead of reading
    // past the table.
    check(&vfx::effects::surface_strike(vfx::Strike::count, vfx::Surface::count) != nullptr,
          "an unknown strike still resolves");
}

void blocks_map_to_the_material_they_are_made_of()
{
    using world::BlockType;
    check(vfx::surface_of(BlockType::rock) == vfx::Surface::stone, "rock is stone");
    check(vfx::surface_of(BlockType::bedrock) == vfx::Surface::stone, "bedrock is stone");
    check(vfx::surface_of(BlockType::grass) == vfx::Surface::dirt, "grass digs like dirt");
    check(vfx::surface_of(BlockType::sand) == vfx::Surface::sand, "sand");
    check(vfx::surface_of(BlockType::snow) == vfx::Surface::snow, "snow");
    check(vfx::surface_of(BlockType::ice) == vfx::Surface::ice, "ice");
    check(vfx::surface_of(BlockType::log) == vfx::Surface::wood, "a log is wood");
    check(vfx::surface_of(BlockType::crate) == vfx::Surface::wood, "a crate is wood");
    check(vfx::surface_of(BlockType::lower_door) == vfx::Surface::wood, "a door is wood");
    check(vfx::surface_of(BlockType::strong_lower_door) == vfx::Surface::metal,
          "a strong door is metal");
    check(vfx::surface_of(BlockType::leaves) == vfx::Surface::leaves, "leaves");
    check(vfx::surface_of(BlockType::iron_wall) == vfx::Surface::metal, "an iron wall is metal");
    check(vfx::surface_of(BlockType::gold_ore) == vfx::Surface::ore, "gold ore is ore");
    check(vfx::surface_of(BlockType::diamond_ore) == vfx::Surface::ore, "diamond ore is ore");
    check(vfx::surface_of(BlockType::blood_stone) == vfx::Surface::crystal, "bloodstone");
    check(vfx::surface_of(BlockType::deep_lava) == vfx::Surface::lava, "lava");
    check(vfx::surface_of(BlockType::glass_basic) == vfx::Surface::glass, "glass");
    check(vfx::surface_of(BlockType::slime) == vfx::Surface::slime, "slime");
}

void dust_takes_the_colour_of_its_block()
{
    using world::BlockType;
    for (int block = 0; block <= static_cast<int>(BlockType::uninitialized); ++block)
    {
        const vfx::Color color = vfx::dust_color(static_cast<BlockType>(block));
        check(color.r >= 0.0F && color.r <= 1.0F && color.g >= 0.0F && color.g <= 1.0F &&
                  color.b >= 0.0F && color.b <= 1.0F,
              "dust colour is a colour");
        check(color.r + color.g + color.b > 0.2F, "dust is never black");
    }
    const vfx::Color sand = vfx::dust_color(BlockType::sand);
    const vfx::Color snow = vfx::dust_color(BlockType::snow);
    const vfx::Color dirt = vfx::dust_color(BlockType::dirt);
    check(sand.r > sand.b, "sand dust is warm");
    check(snow.r + snow.g + snow.b > dirt.r + dirt.g + dirt.b, "snow is paler than dirt");
    check(std::abs(snow.r - dirt.r) + std::abs(snow.b - dirt.b) > 0.05F,
          "different blocks give different dust");
}

void what_a_strike_throws_depends_on_surface_and_strike()
{
    using vfx::Material;
    using vfx::Strike;
    using vfx::Surface;
    for (int value = 0; value < static_cast<int>(Surface::count); ++value)
    {
        const auto surface = static_cast<Surface>(value);
        const vfx::EffectDesc &tool = vfx::effects::surface_strike(Strike::tool, surface);
        const vfx::EffectDesc &shatter = vfx::effects::surface_strike(Strike::shatter, surface);
        const vfx::EffectDesc &place = vfx::effects::surface_strike(Strike::place, surface);
        const vfx::EffectDesc &bolt = vfx::effects::surface_strike(Strike::bolt, surface);
        if (burst_of(tool, Material::debris) > 0)
        {
            check(burst_of(shatter, Material::debris) >= burst_of(tool, Material::debris) * 3,
                  "a block breaking throws several times the chips of one hit");
            check(first_of(shatter, Material::debris)->size.max >
                      first_of(tool, Material::debris)->size.max,
                  "and larger ones");
            check(first_of(shatter, Material::debris)->shape == vfx::Shape::sphere,
                  "from all through its cell");
        }
        for (const vfx::EmitterDesc &emitter : tool.emitters)
            if (emitter.material == Material::debris)
                check(emitter.collide && emitter.gravity > 0.0F,
                      "chips fall and collide with the terrain");
        check(burst_of(place, Material::spark) == 0 && place.lights.empty(),
              "setting a block down strikes no sparks and throws no light");
        check(burst_of(place, Material::debris) <= 2, "and at most a couple of crumbs");
        check(burst_of(bolt, Material::spark) == 0 && bolt.lights.empty(),
              "a bolt's sparks and flash come from bolt_impact, in its own colour");
    }
    const auto sparks = [](Strike strike, Surface surface) {
        return burst_of(vfx::effects::surface_strike(strike, surface), Material::spark);
    };
    check(sparks(Strike::tool, Surface::metal) > sparks(Strike::tool, Surface::stone),
          "metal sparks more than stone");
    check(sparks(Strike::tool, Surface::ore) > sparks(Strike::tool, Surface::stone),
          "ore sparks more than stone");
    check(sparks(Strike::tool, Surface::dirt) == 0 && sparks(Strike::tool, Surface::wood) == 0 &&
              sparks(Strike::tool, Surface::sand) == 0 && sparks(Strike::tool, Surface::snow) == 0,
          "soft surfaces strike no sparks");
    check(sparks(Strike::bullet, Surface::stone) > sparks(Strike::tool, Surface::stone),
          "a bullet strikes more sparks off stone than a pick does");
    check(sparks(Strike::bullet, Surface::dirt) == 0, "and none off dirt");
    check(first_of(vfx::effects::surface_strike(Strike::tool, Surface::ice), Material::spark)->cold,
          "ice splinters are cold");
    check(burst_of(vfx::effects::surface_strike(Strike::tool, Surface::lava), Material::debris) ==
              0,
          "lava has no chips");

    const auto dust = [](Strike strike, Surface surface) {
        return vfx::strike_dust_amount(strike, surface).intensity;
    };
    check(dust(Strike::tool, Surface::sand) > dust(Strike::tool, Surface::stone) &&
              dust(Strike::tool, Surface::snow) > dust(Strike::tool, Surface::stone),
          "sand and snow are mostly powder");
    check(dust(Strike::tool, Surface::metal) < dust(Strike::tool, Surface::stone),
          "metal gives off little dust");
    check(dust(Strike::tool, Surface::lava) == 0.0F && dust(Strike::bullet, Surface::lava) == 0.0F,
          "lava gives off none");
    check(dust(Strike::shatter, Surface::metal) >= 0.5F, "a broken block always leaves some");
    for (int strike = 0; strike < static_cast<int>(Strike::count); ++strike)
    {
        const vfx::EffectDesc &effect = vfx::effects::strike_dust(static_cast<Strike>(strike));
        check(effect.emitters.size() == 1U && effect.emitters[0].material == Material::smoke,
              "dust is one lit puff recipe per strike");
        // Authored near white so the tint IS the block's dust colour.
        const vfx::Color young = effect.emitters[0].color.at(0.0F);
        check(young.r > 0.9F && young.g > 0.9F && young.b > 0.9F, "dust is authored untinted");
    }
    check(vfx::effects::strike_dust(Strike::place).emitters[0].shape == vfx::Shape::ring,
          "a placed block squeezes a ring of dust out from under it");
    check(vfx::effects::strike_dust(Strike::shatter).emitters[0].life.max >
              vfx::effects::strike_dust(Strike::tool).emitters[0].life.max,
          "the dust of a broken block hangs longer");
}

void the_fireball_is_drawn_bigger_and_only_drawn()
{
    // BIGGER: more than twice the radius of the older volume and of the box
    // it collides with. Nothing below reads the drawn size back.
    check(vfx::fireball_drawn_radius >= vfx::classic_fireball_drawn_radius * 2.0F,
          "the head is at least twice as wide as before");
    check(vfx::fireball_halo_radius > vfx::fireball_drawn_radius * 2.0F, "the halo is wider still");
    check(vfx::fireball_drawn_radius > 0.57F * 2.0F,
          "it is drawn larger than the retail +/-0.57 box it hits with");

    for (const bool ice : {false, true})
    {
        const vfx::EffectDesc &flight = vfx::effects::fireball_flight(ice);
        const std::string name{flight.name};
        check(flight.emitters.size() >= 7U, name + " has head, mantle, tail, smoke, embers, halo");
        check(!flight.lights.empty() && flight.lights[0].radius >= 12.0F &&
                  flight.lights[0].duration == 0.0F,
              name + " carries a light for as long as it flies");
        check(flight.cull_distance >= 300.0F, name + " is seen as far as the dragon is");
        check(flight.shake == 0.0F && flight.flash == 0.0F, name + " never shakes the camera");
        bool loops{};
        for (const vfx::EmitterDesc &emitter : flight.emitters)
        {
            loops = loops || emitter.loop;
            if (ice && (emitter.material == vfx::Material::glow ||
                        emitter.material == vfx::Material::spark ||
                        emitter.temperature.at(0.0F) > 0.0F))
                check(emitter.cold, name + " glows cold everywhere");
            if (!ice)
                check(!emitter.cold, name + " is never cold");
        }
        check(loops, name + " is a looping effect");
        // The heart rides with the head; the tail is left behind.
        check(flight.emitters[0].inherit_velocity > 0.9F, name + " heart stays on the head");
        check(flight.emitters[2].inherit_velocity < 0.5F, name + " tail falls behind");
        check(flight.emitters[0].temperature.at(0.0F) > flight.emitters[1].temperature.at(1.0F) + 2.0F,
              name + " has a hot centre and a cool rim");
        check(flight.emitters[2].size_over_life.at(1.0F) < flight.emitters[2].size_over_life.at(0.0F),
              name + " flame shrinks away (FireTrail)");
        check(flight.emitters[3].size_over_life.at(1.0F) > flight.emitters[3].size_over_life.at(0.0F),
              name + " smoke swells (SmokeTrail)");
        check((first_of(flight, vfx::Material::heat) != nullptr) == !ice,
              "only fire makes the air shimmer");
        // The head is about fireball_drawn_radius: where the heart's gas ends
        // up plus its own half-extent.
        const vfx::EmitterDesc &mantle = flight.emitters[1];
        const float reach = mantle.shape_radius + mantle.size.max * mantle.size_over_life.at(0.5F);
        check(reach > vfx::fireball_drawn_radius * 0.9F && reach < vfx::fireball_drawn_radius * 1.6F,
              name + " head is the size it says");
    }
    check(vfx::effects::fireball_flight(false).lights[0].color.r >
              vfx::effects::fireball_flight(false).lights[0].color.b,
          "fire lights warm");
    check(vfx::effects::fireball_flight(true).lights[0].color.b >
              vfx::effects::fireball_flight(true).lights[0].color.r,
          "ice lights cold");
}

void a_fireball_in_flight_stays_inside_its_budget()
{
    const FlatWorld world;
    vfx::System system;
    system.set_world(&world);
    system.set_budget(vfx::budget_for_density(1.0F));
    const vfx::EffectDesc &flight = vfx::effects::fireball_flight(false);
    // Eight in the air at once, as fast as the fastest dragon throws them.
    std::vector<vfx::EffectHandle> handles;
    const vfx::Vec3 velocity{60.0F, -6.0F, 0.0F};
    for (int index = 0; index < 8; ++index)
    {
        vfx::SpawnParams params;
        params.position = {0.0F, 40.0F, static_cast<float>(index) * 6.0F};
        params.direction = vfx::normalized(velocity);
        params.velocity = velocity;
        handles.push_back(system.spawn(flight, params));
        check(handles.back().valid(), "a fireball effect starts");
    }
    const vfx::Vec3 listener{30.0F, 2.0F, 20.0F};
    std::size_t peak{};
    for (int tick = 0; tick < 90; ++tick)
    {
        const float seconds = static_cast<float>(tick + 1) / 60.0F;
        for (std::size_t index = 0U; index < handles.size(); ++index)
            system.move(handles[index],
                        {velocity.x * seconds, 40.0F + velocity.y * seconds,
                         static_cast<float>(index) * 6.0F},
                        velocity);
        system.update(1.0F / 60.0F, listener);
        peak = std::max(peak, system.particle_count());
    }
    check(system.particle_count(handles[0]) > 60U, "a fireball is made of many particles");
    for (const vfx::EffectHandle handle : handles)
        check(system.particle_count(handle) <= static_cast<std::size_t>(flight.max_particles),
              "and never more than its recipe allows");
    check(peak <= 8U * static_cast<std::size_t>(flight.max_particles) &&
              peak <= system.budget().max_particles,
          "eight fireballs fit the global budget");
    // Detonation: the flight effect is stopped and what it left dies out.
    for (const vfx::EffectHandle handle : handles)
        system.kill(handle);
    for (int tick = 0; tick < 60 * 6; ++tick)
        system.update(1.0F / 60.0F, listener);
    check(system.particle_count() == 0U, "nothing of a fireball outlives its smoke");
    for (const vfx::EffectHandle handle : handles)
        check(!system.alive(handle), "and its effect is gone");
}

void sustained_fire_and_rapid_mining_reach_a_steady_state()
{
    const FlatWorld world;
    vfx::System system;
    system.set_world(&world);
    system.set_budget(vfx::budget_for_density(1.0F));
    const vfx::Vec3 listener{0.0F, 1.6F, 0.0F};
    const auto spawn = [&](const vfx::EffectDesc &effect, float x, float intensity = 1.0F) {
        vfx::SpawnParams params;
        params.position = {x, 0.02F, -6.0F};
        params.intensity = intensity;
        params.debris_tiles[0] = 5U;
        params.debris_tile_count = 1U;
        (void)system.spawn(effect, params);
    };
    // Automatic laser fire into rock at 15 shots a second, mining beside it,
    // and muzzle flashes for every shot.
    std::size_t at_four_seconds{};
    std::size_t effects_at_four{};
    std::size_t peak{};
    std::size_t peak_effects{};
    for (int tick = 0; tick < 60 * 8; ++tick)
    {
        if (tick % 4 == 0)
        {
            const float x = static_cast<float>(tick % 12) * 0.2F;
            spawn(vfx::effects::bolt_impact(), x);
            spawn(vfx::effects::surface_strike(vfx::Strike::bolt, vfx::Surface::stone), x);
            spawn(vfx::effects::strike_dust(vfx::Strike::bolt), x,
                  vfx::strike_dust_amount(vfx::Strike::bolt, vfx::Surface::stone).intensity);
            spawn(vfx::effects::muzzle_flash(vfx::Muzzle::laser), 0.3F);
        }
        if (tick % 15 == 0)
        {
            spawn(vfx::effects::surface_strike(vfx::Strike::tool, vfx::Surface::ore), 3.0F);
            spawn(vfx::effects::strike_dust(vfx::Strike::tool), 3.0F);
        }
        if (tick % 60 == 0)
        {
            spawn(vfx::effects::surface_strike(vfx::Strike::shatter, vfx::Surface::ore), 3.0F);
            spawn(vfx::effects::strike_dust(vfx::Strike::shatter), 3.0F);
        }
        system.update(1.0F / 60.0F, listener);
        peak = std::max(peak, system.particle_count());
        peak_effects = std::max(peak_effects, system.effect_count());
        if (tick == 60 * 4)
        {
            at_four_seconds = system.particle_count();
            effects_at_four = system.effect_count();
        }
    }
    check(at_four_seconds > 100U, "the worst case really is busy");
    check(system.particle_count() <= at_four_seconds + at_four_seconds / 3U + 64U,
          "particles stop growing once the first ones die");
    check(system.effect_count() <= effects_at_four + effects_at_four / 3U + 8U,
          "and so do live effects");
    check(peak_effects < vfx::System::max_effects * 3U / 4U,
          "sustained fire leaves effect slots for everything else");
    check(peak < system.budget().max_particles / 2U,
          "sustained fire uses under half the particle budget");
    check(system.dropped_particles() == 0U, "and nothing had to be dropped");
}

void the_dragon_winds_up_before_it_fires()
{
    check(vfx::dragon_charge(false, 0.5F) == 0.0F, "no glow outside the attack clip");
    check(vfx::dragon_charge(true, 0.0F) == 0.0F, "none as the clip starts");
    float previous{};
    for (int step = 1; step <= 20; ++step)
    {
        const float seconds = vfx::dragon_shot_clip_seconds * static_cast<float>(step) / 20.0F;
        const float glow = vfx::dragon_charge(true, seconds);
        check(glow >= previous && glow <= 1.0F, "the glow only builds through the wind-up");
        previous = glow;
    }
    check(std::abs(vfx::dragon_charge(true, vfx::dragon_shot_clip_seconds) - 1.0F) < 0.001F,
          "it peaks exactly as the shot leaves");
    check(vfx::dragon_charge(true, vfx::dragon_shot_clip_seconds + 0.25F) == 0.0F,
          "and is gone a moment later");
    // The retail shot time, which the simulation owns; this is only where the
    // presentation aims its peak.
    check(std::abs(vfx::dragon_shot_clip_seconds - 1.1333333F) < 0.0001F,
          "the glow peaks at retail's shot time");
    for (const bool ice : {false, true})
    {
        check(burst_total(vfx::effects::fireball_launch(ice)) >= 40,
              "a launch is a real burst out of the mouth");
        check(!vfx::effects::fireball_launch(ice).lights.empty() &&
                  !vfx::effects::dragon_breath(ice).lights.empty() &&
                  !vfx::effects::dragon_death(ice).lights.empty(),
              "launch, breath and death all throw light");
    }
}

void a_detonation_leaves_fire_or_frost_behind()
{
    const vfx::EffectDesc &fire = vfx::effects::ground_fire();
    float longest{};
    for (const vfx::EmitterDesc &emitter : fire.emitters)
    {
        check(!emitter.loop, "burning ground goes out by itself");
        longest = std::max(longest, emitter.delay + emitter.duration);
    }
    check(longest >= 3.0F && longest <= 8.0F, "it burns for a few seconds");
    check(first_of(fire, vfx::Material::flame) != nullptr &&
              first_of(fire, vfx::Material::heat) != nullptr && !fire.lights.empty(),
          "with flame, heat and light");
    const vfx::EffectDesc &frost = vfx::effects::ground_frost();
    for (const vfx::EmitterDesc &emitter : frost.emitters)
        check(!emitter.loop, "frost fades by itself");
    check(first_of(frost, vfx::Material::flame) == nullptr, "frost does not burn");

    const vfx::EffectDesc &ice = vfx::effects::iceball_burst();
    check(first_of(ice, vfx::Material::shockwave) != nullptr &&
              first_of(ice, vfx::Material::debris) != nullptr &&
              first_of(ice, vfx::Material::flame) == nullptr,
          "an iceball bursts into shards and vapour, never flame");
    check(ice.flash_color.b > ice.flash_color.r, "its flash is cold");
    check(burst_of(vfx::effects::fireball_embers(), vfx::Material::spark) >= 60,
          "a fireball rains embers");

    // Both die out completely.
    const FlatWorld world;
    vfx::System system;
    system.set_world(&world);
    system.set_budget(vfx::budget_for_density(1.0F));
    vfx::SpawnParams params;
    params.position = {0.0F, 0.1F, 0.0F};
    params.debris_tiles[0] = 16U;
    params.debris_tile_count = 1U;
    (void)system.spawn(ice, params);
    (void)system.spawn(vfx::effects::fireball_embers(), params);
    for (int patch = 0; patch < 6; ++patch)
    {
        params.position = {static_cast<float>(patch) - 2.5F, 0.0F, 1.0F};
        (void)system.spawn(patch % 2 == 0 ? fire : frost, params);
    }
    for (int tick = 0; tick < 60 * 16; ++tick)
        system.update(1.0F / 60.0F, {4.0F, 1.6F, 4.0F});
    check(system.particle_count() == 0U && system.effect_count() == 0U,
          "sixteen seconds after a detonation nothing of it is left");
}

void weapons_flash_briefly_and_by_calibre()
{
    using vfx::Material;
    using vfx::Muzzle;
    const vfx::EffectDesc &pistol = vfx::effects::muzzle_flash(Muzzle::pistol);
    const vfx::EffectDesc &rifle = vfx::effects::muzzle_flash(Muzzle::rifle);
    const vfx::EffectDesc &shotgun = vfx::effects::muzzle_flash(Muzzle::shotgun);
    const vfx::EffectDesc &laser = vfx::effects::muzzle_flash(Muzzle::laser);
    for (const vfx::EffectDesc *flash : {&pistol, &rifle, &shotgun, &laser})
    {
        const std::string name{flash->name};
        // The original shows its flash model for one 60 Hz frame; the remaster
        // may fade, it may not linger.
        for (const vfx::EmitterDesc &emitter : flash->emitters)
            if (emitter.material == Material::glow)
            {
                check(emitter.life.max <= 0.08F, name + " flash is over within five frames");
                check(emitter.inherit_velocity >= 1.0F, name + " flash stays on the muzzle");
            }
        check(flash->lights.size() == 1U && flash->lights[0].duration <= 0.08F &&
                  flash->lights[0].intensity.at(1.0F) == 0.0F,
              name + " lights its surroundings for the same instant");
        check(flash->shake == 0.0F && flash->flash == 0.0F,
              name + " leaves the camera and the picture alone");
        check(first_of(*flash, Material::glow)->emissive.at(0.0F) >= 10.0F,
              name + " is bright enough to bloom");
    }
    check(shotgun.lights[0].radius > rifle.lights[0].radius &&
              rifle.lights[0].radius > pistol.lights[0].radius,
          "a bigger gun throws more light");
    check(burst_of(shotgun, Material::spark) > burst_of(pistol, Material::spark),
          "and more fire");
    check(first_of(pistol, Material::smoke) != nullptr &&
              first_of(shotgun, Material::smoke) != nullptr,
          "powder leaves smoke at the muzzle");
    check(first_of(laser, Material::smoke) == nullptr, "an energy weapon leaves none");
    // An energy flash and a bolt's impact are authored white: the tint is the
    // bolt's own colour.
    for (const vfx::EffectDesc *tinted : {&laser, &vfx::effects::bolt_impact()})
        for (const vfx::EmitterDesc &emitter : tinted->emitters)
            if (emitter.material == Material::glow)
            {
                const vfx::Color color = emitter.color.at(0.0F);
                check(color.r == 1.0F && color.g == 1.0F && color.b == 1.0F,
                      "energy effects take the bolt's colour whole");
            }
    check(vfx::effects::shell_casing().emitters.size() == 1U &&
              vfx::effects::shell_casing().emitters[0].collide &&
              vfx::effects::shell_casing().cull_distance <= 40.0F,
          "a spent case bounces and is only drawn close by");
    check(first_of(vfx::effects::creature_hit(), Material::glow) == nullptr &&
              first_of(vfx::effects::creature_hit(), Material::spark) == nullptr &&
              vfx::effects::creature_hit().lights.empty(),
          "a hit on flesh does not spark or glow");
    check(!vfx::effects::dragon_hit().lights.empty() &&
              burst_of(vfx::effects::dragon_hit(), Material::spark) > 0,
          "a hit on the dragon flashes off its hide");
}

void small_effects_cost_nothing_at_a_distance()
{
    // Pickup sparkles, footsteps and spent cases are decoration: none of them
    // may be simulated for something across the valley.
    for (const vfx::EffectDesc *effect :
         {&vfx::effects::pickup_sparkle(), &vfx::effects::footstep_dust(),
          &vfx::effects::shell_casing(), &vfx::effects::pickup_collect()})
    {
        check(effect->cull_distance <= 40.0F, std::string{effect->name} + " is culled early");
        check(effect->max_particles <= 16, std::string{effect->name} + " is tiny");
    }
    vfx::System system;
    system.set_budget(vfx::budget_for_density(1.0F));
    vfx::SpawnParams params;
    params.position = {0.0F, 0.0F, -200.0F};
    for (int index = 0; index < 50; ++index)
        (void)system.spawn(vfx::effects::pickup_sparkle(), params);
    for (int tick = 0; tick < 10; ++tick)
        system.update(1.0F / 60.0F, {0.0F, 1.6F, 0.0F});
    check(system.particle_count() == 0U, "far pickups emit nothing");
}

void the_drawn_fireball_cannot_reach_the_simulation()
{
    // world::fireball_sweep is the only thing that decides a hit, and it takes
    // no drawn size: a fireball passing 1.0 from a wall (inside the drawn
    // head's 1.6 radius, outside the retail 0.57 box) does not detonate.
    world::WorldRegion region{0, 0, 2, 2};
    for (int y = 0; y < 12; ++y)
        for (int z = 0; z < 16; ++z)
            region.set_block(10, y, z, world::BlockType::rock);
    world::PlayerState player;
    player.position = {2.0F, 40.0F, 2.0F};
    const world::FireballSweepHit grazing =
        world::fireball_sweep(region, player, {9.0F, 4.5F, 2.0F}, {9.0F, 4.5F, 10.0F});
    check(!static_cast<bool>(grazing),
          "a fireball one block from a wall flies on, whatever size it is drawn");
    const world::FireballSweepHit striking =
        world::fireball_sweep(region, player, {9.0F, 4.5F, 2.0F}, {9.6F, 4.5F, 6.0F});
    check(static_cast<bool>(striking), "and still detonates on its retail box");
}

} // namespace

int main()
{
    struct Test final
    {
        const char *name;
        void (*run)();
    };
    const Test tests[]{
        Test{"every recipe is well formed", every_recipe_is_well_formed},
        Test{"every surface and strike has a recipe", every_surface_and_strike_has_a_recipe},
        Test{"blocks map to the material they are made of",
             blocks_map_to_the_material_they_are_made_of},
        Test{"dust takes the colour of its block", dust_takes_the_colour_of_its_block},
        Test{"what a strike throws depends on surface and strike",
             what_a_strike_throws_depends_on_surface_and_strike},
        Test{"the fireball is drawn bigger and only drawn",
             the_fireball_is_drawn_bigger_and_only_drawn},
        Test{"a fireball in flight stays inside its budget",
             a_fireball_in_flight_stays_inside_its_budget},
        Test{"sustained fire and rapid mining reach a steady state",
             sustained_fire_and_rapid_mining_reach_a_steady_state},
        Test{"the dragon winds up before it fires", the_dragon_winds_up_before_it_fires},
        Test{"a detonation leaves fire or frost behind", a_detonation_leaves_fire_or_frost_behind},
        Test{"weapons flash briefly and by calibre", weapons_flash_briefly_and_by_calibre},
        Test{"small effects cost nothing at a distance",
             small_effects_cost_nothing_at_a_distance},
        Test{"the drawn fireball cannot reach the simulation",
             the_drawn_fireball_cannot_reach_the_simulation},
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
    const std::size_t total = sizeof(tests) / sizeof(tests[0]);
    std::cout << (total - static_cast<std::size_t>(failures)) << '/' << total
              << " combat effect checks passed\n";
    return failures == 0 ? 0 : 1;
}
