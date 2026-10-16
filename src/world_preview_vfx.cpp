// WorldPreviewRenderer's side of the Enhanced effects layer (openminerz/vfx.hpp):
// which effects exist, where, and what the next draw is told about them.
//
// PRESENTATION ONLY. Nothing here is read by the simulation or the network.
// Everything below is inert in the Original ("Classic") presentation, which
// keeps the older particle path in world_preview_renderer.cpp exactly.

#include "world_preview_renderer.hpp"

#include "depth_voxel_renderer.hpp"
#include "openminerz/world/block.hpp"
#include "vfx/vfx_renderer.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>
#include <unordered_map>

namespace openminerz
{
namespace
{

[[nodiscard]] vfx::Vec3 to_vfx(world::Vec3 value) noexcept
{
    return {value.x, value.y, value.z};
}

[[nodiscard]] int cell(float value) noexcept
{
    return static_cast<int>(std::floor(value));
}

// Terrain and light for the effects, read from the resident region.
class RegionQuery final : public vfx::WorldQuery
{
  public:
    const world::WorldRegion *region{};

    [[nodiscard]] bool solid(int x, int y, int z) const override
    {
        return region != nullptr && region->contains(x, y, z) &&
               world::blocks_player(region->block(x, y, z));
    }

    [[nodiscard]] vfx::LightSample light(vfx::Vec3 position) const override
    {
        vfx::LightSample sample;
        if (region == nullptr)
            return sample;
        const int center_x = cell(position.x);
        const int center_y = cell(position.y);
        const int center_z = cell(position.z);
        // Skylight: how many of five columns round the point are open above
        // it, the point's own counted double.
        constexpr std::array<std::array<int, 3>, 5> columns{
            {{0, 0, 2}, {1, 0, 1}, {-1, 0, 1}, {0, 1, 1}, {0, -1, 1}}};
        const int top = world::world_min_y + world::chunk_height;
        int open = 0;
        for (const auto &column : columns)
        {
            const int x = center_x + column[0];
            const int z = center_z + column[1];
            bool clear = true;
            for (int y = center_y + 1; y < top; ++y)
                if (region->contains(x, y, z) && world::occludes(region->block(x, y, z)))
                {
                    clear = false;
                    break;
                }
            if (clear)
                open += column[2];
        }
        sample.sky = static_cast<float>(open) / 6.0F;
        // Block light: the brightest torch or lava within reach.
        constexpr int reach = 6;
        constexpr float radius = 8.0F;
        float block{};
        for (int y = center_y - reach; y <= center_y + 4; ++y)
            for (int z = center_z - reach; z <= center_z + reach; ++z)
                for (int x = center_x - reach; x <= center_x + reach; ++x)
                {
                    if (!region->contains(x, y, z))
                        continue;
                    const std::uint8_t emission = world::self_illumination(region->block(x, y, z));
                    if (emission == 0U)
                        continue;
                    const float dx = static_cast<float>(x) + 0.5F - position.x;
                    const float dy = static_cast<float>(y) + 0.5F - position.y;
                    const float dz = static_cast<float>(z) + 0.5F - position.z;
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    block = std::max(block, static_cast<float>(emission) / 15.0F *
                                                (1.0F - std::min(distance / radius, 1.0F)));
                }
        sample.block = std::clamp(block, 0.0F, 1.0F);
        return sample;
    }
};

[[nodiscard]] std::uint64_t cell_key(int x, int y, int z) noexcept
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0x1FFFFFU) << 42U) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0x1FFFFFU) << 21U) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x1FFFFFU));
}

// "name,dist=8,age=0.4": the value after `key=`, or `fallback`.
[[nodiscard]] float spec_number(std::string_view spec, std::string_view key, float fallback)
{
    std::size_t at = 0U;
    while (at < spec.size())
    {
        const std::size_t end = std::min(spec.find(',', at), spec.size());
        const std::string_view field = spec.substr(at, end - at);
        if (field.size() > key.size() + 1U && field.starts_with(key) && field[key.size()] == '=')
        {
            const std::string_view text = field.substr(key.size() + 1U);
            float value{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec == std::errc{})
                return value;
        }
        at = end + 1U;
    }
    return fallback;
}

} // namespace

struct WorldPreviewRenderer::VfxState final
{
    vfx::System system{16384U};
    vfx::Frame frame;
    RegionQuery query;
    struct Tracked final
    {
        vfx::EffectHandle handle{};
        vfx::Vec3 position{};
        bool seen{};
    };
    std::unordered_map<std::uint64_t, Tracked> torches;
    std::unordered_map<int, Tracked> rockets;
    std::unordered_map<int, Tracked> fuses;
    std::vector<vfx::PointLight> lights;
    std::vector<vfx::DebrisCube> debris;
    std::vector<std::uint16_t> keys;
    std::vector<std::uint16_t> key_scratch;
    std::vector<vfx::Instance> instance_scratch;
    vfx::LightSample eye_light{};
    float eye_light_timer{};
    float lava_timer{};
    float shake{};
    std::uint32_t random{0x6C8E9CF5U};
    // Diagnostics (OPENMINERZ_DIAGNOSTIC_VFX): one deterministic effect, either
    // frozen at an age for a still or respawned on a period for a benchmark.
    bool diagnostic_started{};
    bool frozen{};
    float diagnostic_repeat{};
    float diagnostic_timer{};
    std::size_t stat_frames{};
    double stat_build_milliseconds{};
    std::size_t stat_peak_particles{};
    // Simulation ticks since the last report: how many, their total and the
    // slowest (OPENMINERZ_VFX_STATS).
    std::size_t stat_ticks{};
    double stat_tick_milliseconds{};
    double stat_tick_peak_milliseconds{};

    [[nodiscard]] float random01() noexcept
    {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        return static_cast<float>(random & 0x00FFFFFFU) / 16777216.0F;
    }
};

void WorldPreviewRenderer::VfxStateDeleter::operator()(VfxState *state) const noexcept
{
    delete state;
}

WorldPreviewRenderer::VfxState &WorldPreviewRenderer::vfx_state()
{
    if (vfx_state_ == nullptr)
    {
        vfx_state_.reset(new VfxState{});
        vfx_state_->frame.soft.reserve(16384U);
        vfx_state_->frame.sharp.reserve(8192U);
        vfx_state_->frame.debris.reserve(1024U);
        vfx_state_->frame.lights.reserve(64U);
        vfx_state_->lights.reserve(64U);
        vfx_state_->debris.reserve(1024U);
    }
    vfx_state_->query.region = &region_;
    vfx_state_->system.set_world(&vfx_state_->query);
    return *vfx_state_;
}

bool WorldPreviewRenderer::enhanced_effects() const noexcept
{
    return enhanced_effects_;
}

vfx::System *WorldPreviewRenderer::effects() noexcept
{
    return enhanced_effects_ ? &vfx_state().system : nullptr;
}

vfx::EffectHandle WorldPreviewRenderer::spawn_effect(const vfx::EffectDesc &desc,
                                                     const vfx::SpawnParams &params)
{
    if (!enhanced_effects_)
        return {};
    return vfx_state().system.spawn(desc, params);
}

void WorldPreviewRenderer::vfx_set_enhanced(bool enhanced)
{
    if (enhanced == enhanced_effects_)
        return;
    enhanced_effects_ = enhanced;
    if (vfx_state_ != nullptr)
    {
        // Changing presentation: nothing of one path may linger in the other.
        vfx_state_->system.clear();
        vfx_state_->torches.clear();
        vfx_state_->rockets.clear();
        vfx_state_->fuses.clear();
        vfx_state_->debris.clear();
        vfx_state_->lights.clear();
        vfx_state_->shake = 0.0F;
    }
    if (enhanced)
    {
        // The older path's leftovers of the kinds this layer now draws.
        effect_emitters_.clear();
        std::erase_if(mining_particles_, [](const MiningParticle &particle) {
            return particle.kind == ParticleKind::torch_fire ||
                   particle.kind == ParticleKind::torch_smoke;
        });
    }
}

bool WorldPreviewRenderer::vfx_spawn_impact_smoke(world::Vec3 position,
                                                  const std::array<float, 3> &tint)
{
    if (!enhanced_effects_)
        return false;
    vfx::SpawnParams params;
    params.position = to_vfx(position);
    params.tint = {tint[0], tint[1], tint[2]};
    (void)vfx_state().system.spawn(vfx::effects::impact_puff(), params);
    return true;
}

bool WorldPreviewRenderer::vfx_spawn_explosion(world::Vec3 position, ExplosionStyle style,
                                               bool rock_chunks)
{
    if (!enhanced_effects_ || style == ExplosionStyle::fireball)
        return false;
    VfxState &state = vfx_state();
    const vfx::EffectDesc *desc = &vfx::effects::explosion_rocket();
    vfx::SpawnParams params;
    params.position = to_vfx(position);
    switch (style)
    {
    case ExplosionStyle::tnt:
        desc = &vfx::effects::explosion_tnt();
        break;
    case ExplosionStyle::c4:
        desc = &vfx::effects::explosion_c4();
        break;
    case ExplosionStyle::grenade:
        desc = &vfx::effects::explosion_grenade();
        break;
    default:
        break;
    }
    // A placed charge is reported by its block; the blast belongs in the
    // middle of that block. (Where it LOOKS centred; nothing else moves.)
    if (style == ExplosionStyle::tnt || style == ExplosionStyle::c4)
        params.position = {std::floor(params.position.x) + 0.5F,
                           std::floor(params.position.y) + 0.5F,
                           std::floor(params.position.z) + 0.5F};
    if (rock_chunks)
    {
        // Debris wears the faces of the blocks standing nearest the blast.
        const int center_x = cell(params.position.x);
        const int center_y = cell(params.position.y);
        const int center_z = cell(params.position.z);
        for (int ring = 0; ring <= 3 && params.debris_tile_count < params.debris_tiles.size(); ++ring)
            for (int y = center_y - ring; y <= center_y + ring; ++y)
                for (int z = center_z - ring; z <= center_z + ring; ++z)
                    for (int x = center_x - ring; x <= center_x + ring; ++x)
                    {
                        if (std::max({std::abs(x - center_x), std::abs(y - center_y),
                                      std::abs(z - center_z)}) != ring ||
                            params.debris_tile_count >= params.debris_tiles.size() ||
                            !region_.contains(x, y, z))
                            continue;
                        const world::BlockType block = region_.block(x, y, z);
                        if (!world::renders_as_voxel(block) || !world::blocks_player(block))
                            continue;
                        // (Polish) a mod block's debris wears its own tile.
                        const std::uint8_t tile =
                            mod_debris_tile(x, y, z, block, world::FaceDirection::positive_x);
                        bool known = false;
                        for (std::size_t index = 0U; index < params.debris_tile_count; ++index)
                            known = known || params.debris_tiles[index] == tile;
                        if (!known)
                            params.debris_tiles[params.debris_tile_count++] = tile;
                    }
    }
    (void)state.system.spawn(*desc, params);
    return true;
}

void WorldPreviewRenderer::vfx_torch_begin()
{
    if (!enhanced_effects_ || vfx_state_ == nullptr)
        return;
    for (auto &[key, torch] : vfx_state_->torches)
        torch.seen = false;
}

bool WorldPreviewRenderer::vfx_torch(int x, int y, int z, world::Vec3 flame)
{
    if (!enhanced_effects_)
        return false;
    VfxState &state = vfx_state();
    VfxState::Tracked &torch = state.torches[cell_key(x, y, z)];
    torch.seen = true;
    if (!state.system.alive(torch.handle))
    {
        vfx::SpawnParams params;
        params.position = to_vfx(flame);
        torch.handle = state.system.spawn(vfx::effects::torch_flame(), params);
        torch.position = params.position;
    }
    return true;
}

void WorldPreviewRenderer::vfx_torch_end()
{
    if (!enhanced_effects_ || vfx_state_ == nullptr)
        return;
    VfxState &state = *vfx_state_;
    std::erase_if(state.torches, [&](auto &entry) {
        if (entry.second.seen)
            return false;
        state.system.kill(entry.second.handle);
        return true;
    });
}

void WorldPreviewRenderer::vfx_update(float delta, world::Vec3 listener)
{
    if (!enhanced_effects_)
        return;
    VfxState &state = vfx_state();
    vfx::Budget budget = vfx::budget_for_density(particle_density_);
    budget.max_lights = std::min<std::size_t>(
        budget.max_lights, static_cast<std::size_t>(std::max(post_effects_.effect_lights, 0)));
    state.system.set_budget(budget);
    if (state.frozen || !(delta > 0.0F))
        return;
    const vfx::Vec3 eye{listener.x, listener.y + world::PlayerSimulation::eye_height, listener.z};

    // Rockets in flight trail exhaust; a lit charge smokes.
    for (auto &[id, rocket] : state.rockets)
        rocket.seen = false;
    for (const RocketVisual &rocket : rocket_visuals_)
    {
        VfxState::Tracked &tracked = state.rockets[rocket.id];
        const vfx::Vec3 nozzle{rocket.position.x - rocket.direction.x * 0.25F,
                               rocket.position.y - rocket.direction.y * 0.25F,
                               rocket.position.z - rocket.direction.z * 0.25F};
        const vfx::Vec3 backward{-rocket.direction.x, -rocket.direction.y, -rocket.direction.z};
        if (!state.system.alive(tracked.handle))
        {
            vfx::SpawnParams params;
            params.position = nozzle;
            params.direction = backward;
            tracked.handle = state.system.spawn(vfx::effects::rocket_trail(), params);
        }
        else
        {
            state.system.move(tracked.handle, nozzle, (nozzle - tracked.position) * (1.0F / delta));
            state.system.set_direction(tracked.handle, backward);
        }
        tracked.position = nozzle;
        tracked.seen = true;
    }
    std::erase_if(state.rockets, [&](auto &entry) {
        if (entry.second.seen)
            return false;
        state.system.kill(entry.second.handle);
        return true;
    });
    for (auto &[id, fuse] : state.fuses)
        fuse.seen = false;
    for (const ExplosiveFuseVisual &fuse : explosive_fuse_visuals_)
    {
        VfxState::Tracked &tracked = state.fuses[fuse.id];
        tracked.seen = true;
        if (state.system.alive(tracked.handle))
            continue;
        vfx::SpawnParams params;
        params.position = {static_cast<float>(fuse.x) + 0.5F, static_cast<float>(fuse.y) + 1.0F,
                           static_cast<float>(fuse.z) + 0.5F};
        tracked.handle = state.system.spawn(vfx::effects::fuse_smoke(), params);
    }
    std::erase_if(state.fuses, [&](auto &entry) {
        if (entry.second.seen)
            return false;
        state.system.kill(entry.second.handle);
        return true;
    });

    // Lava is never still: a few random cells near the eye are tried each
    // tick, and one that is lava under open air bubbles or spits.
    state.lava_timer += delta * 26.0F * particle_density_;
    while (state.lava_timer >= 1.0F)
    {
        state.lava_timer -= 1.0F;
        const int x = cell(eye.x + (state.random01() - 0.5F) * 44.0F);
        const int z = cell(eye.z + (state.random01() - 0.5F) * 44.0F);
        const int from_y = cell(eye.y + (state.random01() - 0.6F) * 22.0F);
        for (int y = from_y; y > from_y - 8; --y)
        {
            if (!region_.contains(x, y, z) || !region_.contains(x, y + 1, z))
                break;
            const world::BlockType block = region_.block(x, y, z);
            if (block != world::BlockType::surface_lava && block != world::BlockType::deep_lava)
                continue;
            if (region_.block(x, y + 1, z) != world::BlockType::empty)
                break;
            vfx::SpawnParams params;
            params.position = {static_cast<float>(x) + state.random01(),
                               static_cast<float>(y) + 1.02F,
                               static_cast<float>(z) + state.random01()};
            (void)state.system.spawn(state.random01() < 0.22F ? vfx::effects::lava_spurt()
                                                              : vfx::effects::lava_bubble(),
                                     params);
            break;
        }
    }

    if (state.diagnostic_repeat > 0.0F)
    {
        state.diagnostic_timer += delta;
        if (state.diagnostic_timer >= state.diagnostic_repeat)
        {
            state.diagnostic_timer = 0.0F;
            state.diagnostic_started = false;
        }
    }
    const auto tick_begin = std::chrono::steady_clock::now();
    state.system.update(delta, eye);
    const double tick_milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick_begin)
            .count();
    ++state.stat_ticks;
    state.stat_tick_milliseconds += tick_milliseconds;
    state.stat_tick_peak_milliseconds =
        std::max(state.stat_tick_peak_milliseconds, tick_milliseconds);
}

// One-shot capture switch: OPENMINERZ_DIAGNOSTIC_VFX="name,dist=10,age=0.35".
//   name    a recipe of vfx::all_effects(), or "inside" (stand in thick smoke)
//   dist    blocks in front of the camera, on the ground there (default 8)
//   up      blocks above that ground (default 0.5; lava and torches want 0)
//   age     simulate this long, then hold still: every frame shows the same
//           instant, so a still is reproducible. Without it the effect plays
//   count   that many, side by side `gap` blocks apart (default 1, 4)
//   repeat  play, and start it again every this many seconds (benchmarks)
//   scale   SpawnParams::scale
void WorldPreviewRenderer::vfx_diagnostic(const world::PlayerState &camera)
{
    VfxState &state = vfx_state();
    if (state.diagnostic_started)
        return;
    const char *request = SDL_getenv("OPENMINERZ_DIAGNOSTIC_VFX");
    const char *legacy = SDL_getenv("OPENMINERZ_DIAGNOSTIC_VOLUMES");
    std::string spec = request != nullptr ? request : "";
    if (spec.empty() && legacy != nullptr && std::string_view{legacy} == "inside")
        spec = "inside";
    if (spec.empty())
        return;
    state.diagnostic_started = true;
    const std::string_view name = std::string_view{spec}.substr(0U, spec.find(','));
    // "combat:<scene>": a scripted weapon, dragon or mining scene, run through
    // the game's own spawn functions (world_preview_combat_vfx.cpp). It plays
    // itself forward to its `age`; a repeating one is left to run.
    if (name.starts_with("combat:"))
    {
        state.frozen = combat_vfx_diagnostic(spec, camera);
        state.diagnostic_repeat = 0.0F;
        return;
    }
    const vfx::Vec3 eye{camera.position.x,
                        camera.position.y + world::PlayerSimulation::eye_height,
                        camera.position.z};
    const vfx::Vec3 ahead{std::sin(camera.yaw), 0.0F, -std::cos(camera.yaw)};
    const vfx::Vec3 right{std::cos(camera.yaw), 0.0F, std::sin(camera.yaw)};
    const bool inside = name == "inside";
    const vfx::EffectDesc *desc =
        inside ? &vfx::effects::lingering_smoke() : vfx::find_effect(name);
    if (desc == nullptr)
    {
        SDL_Log("OPENMINERZ_DIAGNOSTIC_VFX: no effect named '%.*s'", static_cast<int>(name.size()),
                name.data());
        return;
    }
    const float distance = spec_number(spec, "dist", inside ? 0.0F : 8.0F);
    const float lift = spec_number(spec, "up", inside ? -0.6F : 0.5F);
    const int count = std::clamp(static_cast<int>(spec_number(spec, "count", 1.0F)), 1, 64);
    const float gap = spec_number(spec, "gap", 4.0F);
    const float age = spec_number(spec, "age", inside ? 3.5F : -1.0F);
    state.diagnostic_repeat = spec_number(spec, "repeat", 0.0F);
    for (int index = 0; index < count; ++index)
    {
        const float side = spec_number(spec, "side", 0.0F) +
                           (static_cast<float>(index) - static_cast<float>(count - 1) * 0.5F) * gap;
        vfx::Vec3 point = eye + ahead * distance + right * side;
        if (!inside)
        {
            // Stand it on the ground at that point. Under a roof (the eye has
            // rock overhead) walk toward it and stop at the first wall, so a
            // cave's effect stays in the cave; in the open, climb a hillside
            // the point lies inside.
            bool roofed = false;
            for (int rise = 1; rise <= 24 && !roofed; ++rise)
                roofed = state.query.solid(cell(eye.x), cell(eye.y) + rise, cell(eye.z));
            if (roofed)
            {
                const vfx::Vec3 travel = point - eye;
                const int steps = std::max(1, static_cast<int>(vfx::length(travel) * 2.0F));
                vfx::Vec3 reached = eye;
                for (int step = 1; step <= steps; ++step)
                {
                    const vfx::Vec3 probe =
                        eye + travel * (static_cast<float>(step) / static_cast<float>(steps));
                    if (state.query.solid(cell(probe.x), cell(probe.y), cell(probe.z)))
                        break;
                    reached = probe;
                }
                point = reached;
            }
            int y = cell(eye.y);
            const int x = cell(point.x);
            const int z = cell(point.z);
            if (state.query.solid(x, y, z))
            {
                for (int step = 0; step < 64 && state.query.solid(x, y + 1, z); ++step)
                    ++y;
            }
            else
            {
                for (int step = 0; step < 64 && !state.query.solid(x, y, z); ++step)
                    --y;
            }
            point.y = static_cast<float>(y) + 1.0F;
        }
        point.y += lift;
        vfx::SpawnParams params;
        params.position = point;
        params.scale = spec_number(spec, "scale", inside ? 1.5F : 1.0F);
        params.intensity = spec_number(spec, "intensity", inside ? 1.6F : 1.0F);
        if (desc->name.starts_with("explosion"))
        {
            const world::Vec3 at{point.x, point.y, point.z};
            const ExplosionStyle style = desc == &vfx::effects::explosion_c4()
                                             ? ExplosionStyle::c4
                                             : desc == &vfx::effects::explosion_tnt()
                                                   ? ExplosionStyle::tnt
                                                   : desc == &vfx::effects::explosion_grenade()
                                                         ? ExplosionStyle::grenade
                                                         : ExplosionStyle::rocket;
            // (Placed charges are centred in their block by vfx_spawn_explosion.)
            (void)vfx_spawn_explosion(at, style, true);
        }
        else
        {
            (void)state.system.spawn(*desc, params);
        }
    }
    if (age >= 0.0F && state.diagnostic_repeat <= 0.0F)
    {
        constexpr float step = 1.0F / 60.0F;
        for (float simulated = 0.0F; simulated < age; simulated += step)
            state.system.update(step, eye);
        state.frozen = true;
    }
}

void WorldPreviewRenderer::vfx_adopt_particle(const MiningParticle &particle)
{
    // An effect the older path still simulates (dragon fire, mining dust),
    // drawn by this layer: lit, soft, sorted with everything else, and never
    // as a full-screen volume.
    VfxState &state = vfx_state();
    const float progress = std::clamp(particle.age / std::max(particle.lifetime, 1.0e-3F), 0.0F, 1.0F);
    vfx::Instance instance;
    instance.position[0] = particle.position.x;
    instance.position[1] = particle.position.y;
    instance.position[2] = particle.position.z;
    instance.size = std::max(0.02F, std::lerp(particle.size, particle.end_size, progress) * 0.62F);
    instance.rotation = particle.rotation * (std::numbers::pi_v<float> / 180.0F);
    instance.frame = std::fmod(std::abs(particle.rotation) * 0.37F, 64.0F) + particle.age * 11.0F;
    instance.sky_light = state.eye_light.sky;
    instance.block_light = state.eye_light.block;
    instance.axis[1] = 1.0F;
    instance.emissive = 1.0F;
    const bool ice = particle.kind == ParticleKind::ice_soft || particle.kind == ParticleKind::ice_flash;
    switch (particle.kind)
    {
    case ParticleKind::flash:
    case ParticleKind::ice_flash:
        instance.material = static_cast<float>(vfx::Material::glow) + (ice ? 16.0F : 0.0F);
        instance.color[0] = 1.0F;
        instance.color[1] = 0.84F;
        instance.color[2] = 0.60F;
        instance.alpha = 1.0F - progress;
        instance.emissive = 7.0F;
        instance.size *= 1.3F;
        break;
    case ParticleKind::fire_soft:
    case ParticleKind::ice_soft:
    case ParticleKind::torch_fire:
        instance.material = static_cast<float>(vfx::Material::smoke) + (ice ? 16.0F : 0.0F);
        instance.color[0] = 0.09F;
        instance.color[1] = 0.085F;
        instance.color[2] = 0.08F;
        instance.alpha = std::min(1.0F, progress * 12.0F) * (1.0F - progress * progress);
        instance.temperature = 0.4F + 2.0F * (1.0F - progress);
        instance.erosion = progress * 0.55F;
        instance.emissive = 1.4F;
        break;
    default:
        instance.material = static_cast<float>(vfx::Material::smoke);
        instance.color[0] = std::min(particle.tint[0] * 0.8F, 0.8F);
        instance.color[1] = std::min(particle.tint[1] * 0.8F, 0.8F);
        instance.color[2] = std::min(particle.tint[2] * 0.8F, 0.8F);
        instance.alpha = std::min(1.0F, progress * 10.0F) * (1.0F - progress) * 0.62F *
                         std::clamp(particle.opacity, 0.0F, 1.0F);
        instance.erosion = 0.05F + 0.75F * progress;
        break;
    }
    if (instance.alpha > 0.003F)
        state.frame.soft.push_back(instance);
}

void WorldPreviewRenderer::vfx_submit(const world::PlayerState &camera, float elapsed_seconds)
{
    (void)elapsed_seconds;
    if (!enhanced_effects_ || depth_renderer_ == nullptr)
        return;
    VfxState &state = vfx_state();
    vfx_diagnostic(camera);
    const auto build_begin = std::chrono::steady_clock::now();
    const float sine_yaw = std::sin(camera.yaw);
    const float cosine_yaw = std::cos(camera.yaw);
    const float sine_pitch = std::sin(camera.pitch);
    const float cosine_pitch = std::cos(camera.pitch);
    vfx::View view;
    view.eye = {camera.position.x, camera.position.y + world::PlayerSimulation::eye_height,
                camera.position.z};
    view.right = {cosine_yaw, 0.0F, sine_yaw};
    view.up = {-sine_yaw * sine_pitch, cosine_pitch, cosine_yaw * sine_pitch};
    view.forward = {sine_yaw * cosine_pitch, sine_pitch, -cosine_yaw * cosine_pitch};
    view.lag_seconds = state.frozen ? 0.0F : presentation_lag_seconds_;
    // The light at the eye, for effects the older path still simulates.
    state.eye_light_timer -= 1.0F;
    if (state.eye_light_timer <= 0.0F)
    {
        state.eye_light_timer = 20.0F;
        state.eye_light = state.query.light(view.eye);
    }
    state.system.build_frame(view, state.frame);

    // What the older path still simulates joins the same sorted list.
    const std::size_t own = state.frame.soft.size();
    for (const MiningParticle &particle : mining_particles_)
    {
        if (particle.kind == ParticleKind::rock || particle.kind == ParticleKind::blast_rock ||
            particle.kind == ParticleKind::spark)
            continue;
        vfx_adopt_particle(particle);
    }
    // A dragon's fireball in flight is an effect of its own that follows it
    // (vfx::effects::fireball_flight, moved in combat_vfx_update), so it is
    // already in the frame built above. What the weapon effects draw exactly
    // where something is THIS frame (tracer and bolt glow) is added here.
    combat_vfx_frame(view, state.frame, elapsed_seconds);
    if (state.frame.soft.size() > own)
    {
        // Adopted particles get the same near fade and count toward the smoke
        // the eye stands in, then everything is sorted together.
        float extra{};
        float extra_weight{};
        for (std::size_t index = own; index < state.frame.soft.size(); ++index)
        {
            vfx::Instance &instance = state.frame.soft[index];
            const vfx::Vec3 offset{instance.position[0] - view.eye.x,
                                   instance.position[1] - view.eye.y,
                                   instance.position[2] - view.eye.z};
            const float distance = vfx::length(offset);
            if (instance.material < 0.5F && instance.temperature < 0.6F)
            {
                const float inside = std::clamp(1.0F - distance / (instance.size * 1.6F), 0.0F, 1.0F);
                const float weight = instance.alpha * inside * inside;
                extra += weight / (2.0F * instance.size);
                extra_weight += weight;
                if (weight > 0.0F && state.frame.immersion <= 0.0F)
                {
                    state.frame.immersion_color = {instance.color[0], instance.color[1],
                                                   instance.color[2]};
                    state.frame.immersion_light = state.eye_light;
                    state.frame.immersion_reach = instance.size * 1.6F;
                }
            }
            instance.alpha *= vfx::near_fade(distance, instance.size);
        }
        state.frame.immersion += extra;
        (void)extra_weight;
        state.keys.clear();
        for (const vfx::Instance &instance : state.frame.soft)
            state.keys.push_back(vfx::depth_sort_key(
                (instance.position[0] - view.eye.x) * view.forward.x +
                (instance.position[1] - view.eye.y) * view.forward.y +
                (instance.position[2] - view.eye.z) * view.forward.z));
        vfx::sort_by_key(state.frame.soft, state.keys, state.instance_scratch, state.key_scratch);
    }

    state.lights.assign(state.frame.lights.begin(), state.frame.lights.end());
    state.debris.assign(state.frame.debris.begin(), state.frame.debris.end());
    // Shake and flash are exactly the motion Reduced motion removes.
    state.shake =
        post_effects_.reduced_motion ? 0.0F : state.frame.shake * post_effects_.effect_shake;

    VfxOptions options;
    options.max_lights =
        static_cast<unsigned>(std::clamp(post_effects_.effect_lights, 0, static_cast<int>(vfx_max_lights)));
    options.distortion = post_effects_.effect_distortion;
    options.flash = post_effects_.reduced_motion ? 0.0F : post_effects_.effect_flash;
    options.resolution_divisor = post_effects_.effect_resolution;

    if (SDL_getenv("OPENMINERZ_VFX_STATS") != nullptr)
    {
        state.stat_build_milliseconds +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_begin)
                .count();
        state.stat_peak_particles = std::max(state.stat_peak_particles, state.system.particle_count());
        if (++state.stat_frames >= 240U)
        {
            SDL_Log("vfx: effects=%zu particles=%zu (peak %zu, dropped %zu) soft=%zu sharp=%zu "
                    "distort=%zu debris=%zu lights=%zu immersion=%.2f build=%.3f ms/frame "
                    "tick=%.3f ms avg %.3f ms peak",
                    state.system.effect_count(), state.system.particle_count(),
                    state.stat_peak_particles, state.system.dropped_particles(),
                    state.frame.soft.size(), state.frame.sharp.size(), state.frame.distortion.size(),
                    state.debris.size(), state.lights.size(), static_cast<double>(state.frame.immersion),
                    state.stat_build_milliseconds / static_cast<double>(state.stat_frames),
                    state.stat_tick_milliseconds /
                        static_cast<double>(std::max<std::size_t>(state.stat_ticks, 1U)),
                    state.stat_tick_peak_milliseconds);
            state.stat_frames = 0U;
            state.stat_build_milliseconds = 0.0;
            state.stat_peak_particles = 0U;
            state.stat_ticks = 0U;
            state.stat_tick_milliseconds = 0.0;
            state.stat_tick_peak_milliseconds = 0.0;
        }
    }
    depth_renderer_->submit_vfx(state.frame, options);
}

bool WorldPreviewRenderer::vfx_debris_pending() const noexcept
{
    return enhanced_effects_ && vfx_state_ != nullptr && !vfx_state_->debris.empty();
}

void WorldPreviewRenderer::vfx_append_debris(EntityBatch &batch)
{
    if (!vfx_debris_pending() || atlas_ == nullptr)
        return;
    // Small cubes of the broken blocks' own faces, in the opaque pass: the lit
    // shader shades and shadows them like any block.
    EntitySpan span;
    span.texture = atlas_;
    span.first = static_cast<std::uint32_t>(batch.vertices.size());
    static constexpr std::array<std::array<float, 3>, 6> normals{
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    static constexpr std::array<std::array<std::array<float, 3>, 4>, 6> corners{{
        {{{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}}},
        {{{-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}}},
        {{{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}}},
        {{{-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}}},
        {{{1, -1, 1}, {1, 1, 1}, {-1, 1, 1}, {-1, -1, 1}}},
        {{{-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {1, -1, -1}}},
    }};
    static constexpr std::array<std::array<float, 2>, 4> face_uv{
        {{0.0F, 1.0F}, {0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}}};
    // Wound so the lit shader sees each face from its front: the other way
    // round it flips the normal inward, the cube takes no sun and ambient
    // occlusion blacks it out.
    static constexpr std::array<int, 6> order{0, 2, 1, 0, 3, 2};
    // A fragment shows a quarter of the face it broke from: `origin` is that
    // face's corner in the texture, `step` a quarter's offset, `margin` keeps
    // the neighbours' texels out and `size` is what is left of the quarter.
    const auto append_cube = [&](const vfx::DebrisCube &cube, std::uint32_t which,
                                 std::array<float, 2> origin, std::array<float, 2> step,
                                 std::array<float, 2> margin, std::array<float, 2> size) {
        // Rodrigues' rotation about the cube's own axis.
        const float cosine = std::cos(cube.angle);
        const float sine = std::sin(cube.angle);
        const vfx::Vec3 axis = cube.axis;
        const auto turn = [&](vfx::Vec3 v) {
            return v * cosine + vfx::cross(axis, v) * sine +
                   axis * (vfx::dot(axis, v) * (1.0F - cosine));
        };
        const float quarter_u = static_cast<float>(which & 1U) * step[0];
        const float quarter_v = static_cast<float>((which >> 1U) & 1U) * step[1];
        const float half = cube.half_size * std::clamp(cube.alpha, 0.0F, 1.0F);
        for (std::size_t face = 0U; face < 6U; ++face)
        {
            const vfx::Vec3 normal =
                turn({normals[face][0], normals[face][1], normals[face][2]});
            for (const int index : order)
            {
                const auto &corner = corners[face][static_cast<std::size_t>(index)];
                const vfx::Vec3 point =
                    cube.position + turn({corner[0] * half, corner[1] * half, corner[2] * half});
                EntityVertex vertex;
                vertex.x = point.x;
                vertex.y = point.y;
                vertex.z = point.z;
                vertex.u = origin[0] + quarter_u + margin[0] +
                           face_uv[static_cast<std::size_t>(index)][0] * size[0];
                vertex.v = origin[1] + quarter_v + margin[1] +
                           face_uv[static_cast<std::size_t>(index)][1] * size[1];
                vertex.normal_x = normal.x;
                vertex.normal_y = normal.y;
                vertex.normal_z = normal.z;
                vertex.sky_light = cube.sky_light;
                vertex.torch_light = cube.block_light;
                // (Opaque spans ignore vertex alpha as coverage; zero marks a
                // prop that screen-space occlusion and contact shadows must
                // leave alone. At a few pixels across they painted every chunk
                // solid black.)
                vertex.alpha = 0.0F;
                batch.vertices.push_back(vertex);
            }
        }
    };
    // (Polish) Chips of mod blocks (tiles 64 and up) come second, in a span
    // of their own over the mod debris atlas.
    bool any_mod_tiles = false;
    std::uint32_t ordinal = 0U;
    for (const vfx::DebrisCube &cube : vfx_state_->debris)
    {
        const std::uint32_t own = ordinal++;
        if (cube.tile >= 64U)
        {
            any_mod_tiles = true;
            continue;
        }
        append_cube(cube, own,
                    {static_cast<float>(cube.tile & 7U) / 8.0F,
                     static_cast<float>(cube.tile >> 3U) / 8.0F},
                    {0.0625F, 0.0625F}, {0.0015F, 0.0015F},
                    {0.0625F - 0.003F, 0.0625F - 0.003F});
    }
    span.count = static_cast<std::uint32_t>(batch.vertices.size()) - span.first;
    if (span.count > 0U)
        batch.spans.push_back(span);
    if (!any_mod_tiles)
        return;
    EntitySpan mod_span;
    mod_span.first = static_cast<std::uint32_t>(batch.vertices.size());
    ordinal = 0U;
    for (const vfx::DebrisCube &cube : vfx_state_->debris)
    {
        const std::uint32_t own = ordinal++;
        std::array<float, 4> tile{};
        SDL_Texture *texture = cube.tile >= 64U ? mod_debris_atlas(cube.tile, tile) : nullptr;
        if (texture == nullptr)
            continue;
        mod_span.texture = texture;
        // The retail atlas's proportions: a quarter of the tile, 1.2 % kept
        // clear at each edge.
        append_cube(cube, own, {tile[0], tile[1]}, {tile[2] * 0.5F, tile[3] * 0.5F},
                    {tile[2] * 0.012F, tile[3] * 0.012F},
                    {tile[2] * (0.5F - 0.024F), tile[3] * (0.5F - 0.024F)});
    }
    mod_span.count = static_cast<std::uint32_t>(batch.vertices.size()) - mod_span.first;
    if (mod_span.count > 0U)
        batch.spans.push_back(mod_span);
}

void WorldPreviewRenderer::vfx_apply_camera_shake(world::PlayerState &camera,
                                                  float elapsed_seconds) const
{
    if (!enhanced_effects_ || vfx_state_ == nullptr || vfx_state_->shake <= 0.001F)
        return;
    // A few incommensurate sines: restless, never a jump from frame to frame.
    const float amount = vfx_state_->shake;
    const float time = elapsed_seconds;
    camera.yaw += amount * 0.020F *
                  (std::sin(time * 37.0F) * 0.6F + std::sin(time * 61.3F + 1.7F) * 0.4F);
    camera.pitch += amount * 0.017F *
                    (std::sin(time * 43.1F + 0.6F) * 0.6F + std::sin(time * 71.9F + 2.9F) * 0.4F);
    camera.position.y += amount * 0.035F * std::sin(time * 52.7F + 4.2F);
}

std::array<float, 3> WorldPreviewRenderer::vfx_light_at(world::Vec3 position) const
{
    std::array<float, 3> light{};
    if (!enhanced_effects_ || vfx_state_ == nullptr || post_effects_.effect_lights <= 0)
        return light;
    for (const vfx::PointLight &source : vfx_state_->lights)
    {
        const float share = vfx::light_falloff(
            vfx::length(source.position - vfx::Vec3{position.x, position.y, position.z}),
            source.radius);
        light[0] += source.color.r * share;
        light[1] += source.color.g * share;
        light[2] += source.color.b * share;
    }
    return light;
}

} // namespace openminerz
