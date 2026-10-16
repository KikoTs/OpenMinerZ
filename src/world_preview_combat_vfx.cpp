// WorldPreviewRenderer's weapon, dragon, mining and creature effects in the
// Enhanced presentation: when each recipe of openminerz/vfx_combat.hpp is
// spawned, where, and with what.
//
// PRESENTATION ONLY. Every function here returns at once (the `bool` ones
// false) unless the Enhanced effects layer is drawing, so the Original
// ("Classic") presentation runs the older recipes in world_preview_renderer.cpp
// exactly as before. Nothing here is read by the simulation or the network: an
// effect is placed where the event already happened and spawned when it
// already happened, and its size is only ever a drawn size.

#include "world_preview_renderer.hpp"

#include "avatar_viewmodel.hpp"
#include "openminerz/vfx_combat.hpp"
#include "openminerz/view_projection.hpp"
#include "openminerz/world/block.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

[[nodiscard]] world::FaceDirection face_of(vfx::Vec3 normal) noexcept
{
    const float ax = std::abs(normal.x);
    const float ay = std::abs(normal.y);
    const float az = std::abs(normal.z);
    if (ay >= ax && ay >= az)
        return normal.y >= 0.0F ? world::FaceDirection::positive_y
                                : world::FaceDirection::negative_y;
    if (ax >= az)
        return normal.x >= 0.0F ? world::FaceDirection::positive_x
                                : world::FaceDirection::negative_x;
    return normal.z >= 0.0F ? world::FaceDirection::positive_z : world::FaceDirection::negative_z;
}

void add_tile(vfx::SpawnParams &params, std::uint8_t tile) noexcept
{
    for (std::size_t index = 0U; index < params.debris_tile_count; ++index)
        if (params.debris_tiles[index] == tile)
            return;
    if (params.debris_tile_count < params.debris_tiles.size())
        params.debris_tiles[params.debris_tile_count++] = tile;
}

// A bolt's own colour, whitened: its flash is hotter than its body.
[[nodiscard]] vfx::Color bolt_tint(const std::array<float, 3> &color) noexcept
{
    return {0.28F + 0.72F * color[0], 0.28F + 0.72F * color[1], 0.28F + 0.72F * color[2]};
}

} // namespace

struct WorldPreviewRenderer::CombatVfxState final
{
    struct Tracked final
    {
        vfx::EffectHandle handle{};
        vfx::Vec3 position{};
        bool seen{};
    };
    std::unordered_map<int, Tracked> fireballs;
    // Rockets already seen, so each gets one backblast as it leaves.
    std::unordered_map<int, bool> rockets;
    // Thrown grenades carry no id: matched frame to frame by where they are.
    std::vector<Tracked> grenades;
    // What each creature was doing when last drawn, so it throws up dirt once
    // as it claws out of the ground and raises dust once as it falls.
    struct Creature final
    {
        world::EnemyState state{world::EnemyState::chase};
        bool seen{};
    };
    std::unordered_map<std::uint32_t, Creature> creatures;
    vfx::EffectHandle breath{};
    bool breath_ice{};
    vfx::Vec3 mouth{};
    bool mouth_known{};
    bool dragon_was_dying{};
    bool dragon_seen{};
    vfx::Vec3 dragon_position{};
    float sparkle_timer{};
    std::uint32_t random{0x51ED270BU};
    // Diagnostics (OPENMINERZ_DIAGNOSTIC_VFX=combat:<scene>,...): a scripted
    // scene, either played to an age and held for a still or repeated at a
    // rate for a benchmark. `shots` are the scene's own stand-in fireballs:
    // drawn like real ones, they hit nothing and are known to nothing else.
    struct Shot final
    {
        FireballVisual visual{};
        float ground{};
        float age{};
    };
    std::vector<Shot> shots;
    std::vector<FireballVisual> scene_visuals;
    std::string scene;
    bool frozen{};
    float scene_rate{};
    float scene_timer{};
    int scene_count{};
    // A held still of the local muzzle flash waits these draws for the
    // viewmodel pass to have drawn the gun it sits on, then plays to this age.
    int scene_defer{};
    float scene_age{};
    int next_shot_id{1 << 20};

    [[nodiscard]] float random01() noexcept
    {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        return static_cast<float>(random & 0x00FFFFFFU) / 16777216.0F;
    }
};

void WorldPreviewRenderer::CombatVfxStateDeleter::operator()(CombatVfxState *state) const noexcept
{
    delete state;
}

WorldPreviewRenderer::CombatVfxState &WorldPreviewRenderer::combat_vfx_state()
{
    if (combat_vfx_state_ == nullptr)
        combat_vfx_state_.reset(new CombatVfxState{});
    return *combat_vfx_state_;
}

// One strike on a block face: the chips, sparks and flash of that surface, and
// its dust in the block's own colour.
void WorldPreviewRenderer::combat_vfx_strike(int strike_value, world::BlockType block,
                                             world::Vec3 point, world::Vec3 normal)
{
    const auto strike = static_cast<vfx::Strike>(strike_value);
    const vfx::Surface surface = vfx::surface_of(block);
    vfx::SpawnParams params;
    params.position = to_vfx(point);
    params.direction = vfx::normalized(to_vfx(normal));
    // Chips wear the block's own faces: the one struck and the block's side.
    // (Polish) A mod block's are its own tiles (mod_debris_tile).
    {
        const world::BlockType chipped =
            block == world::BlockType::empty || block == world::BlockType::uninitialized
                ? world::BlockType::rock
                : block;
        const int x = cell(point.x - normal.x * 0.01F);
        const int y = cell(point.y - normal.y * 0.01F);
        const int z = cell(point.z - normal.z * 0.01F);
        add_tile(params, mod_debris_tile(x, y, z, chipped, face_of(params.direction)));
        add_tile(params, mod_debris_tile(x, y, z, chipped, world::FaceDirection::positive_x));
    }
    (void)spawn_effect(vfx::effects::surface_strike(strike, surface), params);
    const vfx::DustAmount dust = vfx::strike_dust_amount(strike, surface);
    if (dust.intensity <= 0.0F)
        return;
    vfx::SpawnParams cloud;
    cloud.position = params.position;
    cloud.direction = params.direction;
    cloud.intensity = dust.intensity;
    cloud.scale = dust.scale;
    cloud.tint = vfx::dust_color(block);
    // (Visuals) a mod block raises dust of its own texture's colour.
    if (mod_block_overlay_ != nullptr)
        if (const auto mod = mod_block_color(static_cast<int>(std::floor(point.x - normal.x * 0.01F)),
                                             static_cast<int>(std::floor(point.y - normal.y * 0.01F)),
                                             static_cast<int>(std::floor(point.z - normal.z * 0.01F)),
                                             block))
            cloud.tint = {(*mod)[0], (*mod)[1], (*mod)[2]};
    (void)spawn_effect(vfx::effects::strike_dust(strike), cloud);
}

bool WorldPreviewRenderer::combat_vfx_mining_impact(const world::BlockBreakUpdate &impact)
{
    if (!enhanced_effects_)
        return false;
    world::Vec3 normal{static_cast<float>(impact.normal_x), static_cast<float>(impact.normal_y),
                       static_cast<float>(impact.normal_z)};
    if (impact.normal_x == 0 && impact.normal_y == 0 && impact.normal_z == 0)
        normal = {0.0F, 1.0F, 0.0F};
    // The swing lands where it always did (ProcessDigMessage's hit point).
    combat_vfx_strike(static_cast<int>(vfx::Strike::tool), impact.block, impact.intersection,
                      normal);
    if (!impact.completed)
        return true;
    // The swing that removed the block: it, and whatever hung on it, come
    // apart where they stood, thrown toward the side that was open.
    const auto shatter = [&](int x, int y, int z, world::BlockType block) {
        combat_vfx_strike(static_cast<int>(vfx::Strike::shatter), block,
                          {static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                           static_cast<float>(z) + 0.5F},
                          normal);
    };
    if (impact.removed_blocks.empty())
    {
        shatter(impact.x, impact.y, impact.z, impact.block);
        return true;
    }
    std::size_t bursts{};
    for (const world::BlockRemovalEffect &removed : impact.removed_blocks)
    {
        // An upper door half is the same door; one burst for it.
        if (world::is_upper_door(removed.block))
            continue;
        shatter(removed.x, removed.y, removed.z, removed.block);
        if (++bursts >= 6U)
            break;
    }
    return true;
}

bool WorldPreviewRenderer::combat_vfx_tracer_impact(const world::BlockBreakUpdate &impact)
{
    if (!enhanced_effects_)
        return false;
    world::Vec3 normal{static_cast<float>(impact.normal_x), static_cast<float>(impact.normal_y),
                       static_cast<float>(impact.normal_z)};
    if (impact.normal_x == 0 && impact.normal_y == 0 && impact.normal_z == 0)
        normal = {0.0F, 1.0F, 0.0F};
    combat_vfx_strike(static_cast<int>(vfx::Strike::bullet), impact.block, impact.intersection,
                      normal);
    return true;
}

bool WorldPreviewRenderer::combat_vfx_blaster_impact(const world::BlockBreakUpdate &impact,
                                                     const std::array<float, 3> &tint)
{
    if (!enhanced_effects_)
        return false;
    world::Vec3 normal{static_cast<float>(impact.normal_x), static_cast<float>(impact.normal_y),
                       static_cast<float>(impact.normal_z)};
    if (impact.normal_x == 0 && impact.normal_y == 0 && impact.normal_z == 0)
        normal = {0.0F, 1.0F, 0.0F};
    // The bolt's part: its flash, molten sparks and light, in its own colour.
    vfx::SpawnParams params;
    params.position = to_vfx(impact.intersection);
    params.direction = vfx::normalized(to_vfx(normal));
    params.tint = bolt_tint(tint);
    (void)spawn_effect(vfx::effects::bolt_impact(), params);
    // The surface's part. A bolt on a creature or the dragon names no block.
    if (impact.block != world::BlockType::empty && impact.block != world::BlockType::uninitialized)
        combat_vfx_strike(static_cast<int>(vfx::Strike::bolt), impact.block, impact.intersection,
                          normal);
    return true;
}

bool WorldPreviewRenderer::combat_vfx_dragon_hit(world::Vec3 position)
{
    if (!enhanced_effects_)
        return false;
    vfx::SpawnParams params;
    params.position = to_vfx(position);
    // Off the hide, toward whoever is looking.
    if (overlay_camera_.valid)
        params.direction = vfx::normalized(to_vfx(overlay_camera_.eye) - params.position);
    (void)spawn_effect(vfx::effects::dragon_hit(), params);
    return true;
}

bool WorldPreviewRenderer::combat_vfx_fireball_detonation(world::Vec3 position, bool ice,
                                                          bool rock_chunks, ExplosionStyle style)
{
    if (!enhanced_effects_ || style != ExplosionStyle::fireball)
        return false;
    CombatVfxState &state = combat_vfx_state();
    vfx::SpawnParams params;
    params.position = to_vfx(position);
    const int center_x = cell(position.x);
    const int center_y = cell(position.y);
    const int center_z = cell(position.z);
    if (ice)
    {
        // IceFlash + IcePuff + BigPuff + BigSnowBlast, as one recipe. Its
        // shards are ice: what the blast turns the ground into.
        add_tile(params, world::atlas_tile(world::BlockType::ice, world::FaceDirection::positive_y));
        add_tile(params, world::atlas_tile(world::BlockType::snow, world::FaceDirection::positive_y));
        (void)spawn_effect(vfx::effects::iceball_burst(), params);
    }
    else
    {
        // FlashEffect + FirePuff + BigSmokePuff + BigRockBlast: the shared
        // explosion sequence at a dragon's size, its debris wearing the faces
        // of the blocks standing nearest, and a rain of embers over it.
        if (rock_chunks)
            for (int ring = 0; ring <= 3 && params.debris_tile_count < params.debris_tiles.size();
                 ++ring)
                for (int y = center_y - ring; y <= center_y + ring; ++y)
                    for (int z = center_z - ring; z <= center_z + ring; ++z)
                        for (int x = center_x - ring; x <= center_x + ring; ++x)
                        {
                            if (std::max({std::abs(x - center_x), std::abs(y - center_y),
                                          std::abs(z - center_z)}) != ring ||
                                !region_.contains(x, y, z))
                                continue;
                            const world::BlockType block = region_.block(x, y, z);
                            if (world::renders_as_voxel(block) && world::blocks_player(block))
                                add_tile(params, mod_debris_tile(x, y, z, block,
                                                                 world::FaceDirection::positive_x));
                        }
        vfx::SpawnParams blast = params;
        blast.scale = 1.25F;
        (void)spawn_effect(vfx::effects::explosion_tnt(), blast);
        (void)spawn_effect(vfx::effects::fireball_embers(), params);
    }
    // What stays: ground left burning, or frosted, round where it struck.
    const vfx::EffectDesc &patch = ice ? vfx::effects::ground_frost() : vfx::effects::ground_fire();
    int placed{};
    for (int attempt = 0; attempt < 28 && placed < 8; ++attempt)
    {
        const float angle = state.random01() * 2.0F * std::numbers::pi_v<float>;
        const float reach = 0.5F + state.random01() * 3.2F;
        const int x = cell(position.x + std::cos(angle) * reach);
        const int z = cell(position.z + std::sin(angle) * reach);
        for (int y = center_y + 3; y >= center_y - 6; --y)
        {
            if (!region_.contains(x, y, z) || !world::blocks_player(region_.block(x, y, z)) ||
                world::blocks_player(region_.block(x, y + 1, z)))
                continue;
            vfx::SpawnParams ground;
            ground.position = {static_cast<float>(x) + 0.25F + state.random01() * 0.5F,
                               static_cast<float>(y) + 1.02F,
                               static_cast<float>(z) + 0.25F + state.random01() * 0.5F};
            ground.scale = 0.7F + state.random01() * 0.55F;
            (void)spawn_effect(patch, ground);
            ++placed;
            break;
        }
    }
    return true;
}

// Per tick, before the effects layer advances: everything that follows
// something (fireballs in flight, the dragon's mouth, grenades) is moved to
// where that thing now is.
void WorldPreviewRenderer::combat_vfx_update(float delta, world::Vec3 player_position)
{
    if (!enhanced_effects_ || !(delta > 0.0F))
        return;
    vfx::System *system = effects();
    if (system == nullptr)
        return;
    CombatVfxState &state = combat_vfx_state();
    if (state.frozen)
        return;
    combat_vfx_play_scene(delta);

    // --- Fireballs in flight. ---
    for (auto &[id, fireball] : state.fireballs)
        fireball.seen = false;
    // (A diagnostic scene's stand-in shots are drawn exactly like real ones.)
    const std::vector<FireballVisual> *in_flight = &fireball_visuals_;
    if (!state.shots.empty())
    {
        state.scene_visuals = fireball_visuals_;
        for (const CombatVfxState::Shot &shot : state.shots)
            state.scene_visuals.push_back(shot.visual);
        in_flight = &state.scene_visuals;
    }
    for (const FireballVisual &fireball : *in_flight)
    {
        const vfx::Vec3 position = to_vfx(fireball.position);
        const vfx::Vec3 flight = to_vfx(fireball.velocity);
        const float speed = vfx::length(flight);
        const vfx::Vec3 axis = vfx::normalized(flight, {0.0F, 0.0F, -1.0F});
        CombatVfxState::Tracked &tracked = state.fireballs[fireball.id];
        if (!system->alive(tracked.handle))
        {
            vfx::SpawnParams params;
            params.position = position;
            params.direction = axis;
            params.velocity = flight;
            tracked.handle = system->spawn(vfx::effects::fireball_flight(fireball.ice), params);
            // THE LAUNCH, where and when FireballEntity appears: out of the
            // dragon's mouth, along the shot. (A parked diagnostic fireball
            // was not launched.)
            if (!tracked.seen && speed > 1.0F)
                (void)system->spawn(vfx::effects::fireball_launch(fireball.ice), params);
        }
        else
        {
            // Ride with the head. A fireball parked for its detonation message
            // (or seen by a slow frame) has not moved: nothing streams off it.
            const vfx::Vec3 moved = (position - tracked.position) * (1.0F / delta);
            const bool flying = vfx::length(moved) > speed * 0.25F;
            system->move(tracked.handle, position, flying ? flight : moved);
            system->set_direction(tracked.handle, axis);
        }
        tracked.position = position;
        tracked.seen = true;
    }
    std::erase_if(state.fireballs, [&](auto &entry) {
        if (entry.second.seen)
            return false;
        // Detonated or gone: stop emitting; what it shed lives out its life.
        system->kill(entry.second.handle);
        return true;
    });

    // --- The dragon: breath gathering before a shot, and its death. ---
    const world::Dragon *dragon = visible_dragon_;
    const bool dying = dragon != nullptr && (dragon->state == world::DragonState::dying ||
                                             dragon->state == world::DragonState::despawn);
    float charge{};
    if (dragon != nullptr && !dying)
        charge = vfx::dragon_charge(dragon->clip == std::string_view{"attack"}, dragon->clip_time);
    if (dragon != nullptr)
    {
        const bool ice = dragon->kind == world::DragonKind::ice;
        const vfx::Vec3 body = to_vfx(dragon->position);
        const vfx::Vec3 body_velocity =
            state.dragon_seen ? (body - state.dragon_position) * (1.0F / delta) : vfx::Vec3{};
        if (dying && !state.dragon_was_dying && state.dragon_seen)
        {
            // DragonClientEntity.Kill: Flash + FirePuff + BigSmokePuff on it.
            vfx::SpawnParams params;
            params.position = body;
            params.velocity = body_velocity;
            (void)system->spawn(vfx::effects::dragon_death(ice), params);
        }
        state.dragon_was_dying = dying;
        state.dragon_position = body;
        state.dragon_seen = true;
        if (charge > 0.01F)
        {
            // Where ShootFireball reads the muzzle: the ponytail bone.
            const world::DragonBasis basis =
                world::dragon_attitude_basis(dragon->yaw, dragon->pitch, dragon->roll);
            const world::Vec3 offset =
                world::dragon_muzzle_offset(world::dragon_animation_index(dragon->clip));
            const vfx::Vec3 mouth = body + to_vfx(basis.right) * offset.x +
                                    to_vfx(basis.up) * offset.y - to_vfx(basis.forward) * offset.z;
            const vfx::Vec3 mouth_velocity =
                state.mouth_known ? (mouth - state.mouth) * (1.0F / delta) : body_velocity;
            if (!system->alive(state.breath) || state.breath_ice != ice)
            {
                system->kill(state.breath);
                vfx::SpawnParams params;
                params.position = mouth;
                params.direction = to_vfx(basis.forward);
                params.velocity = mouth_velocity;
                params.intensity = charge;
                state.breath = system->spawn(vfx::effects::dragon_breath(ice), params);
                state.breath_ice = ice;
            }
            else
            {
                system->move(state.breath, mouth, mouth_velocity);
                system->set_direction(state.breath, to_vfx(basis.forward));
                system->set_intensity(state.breath, charge);
            }
            state.mouth = mouth;
            state.mouth_known = true;
        }
    }
    else
    {
        state.dragon_seen = false;
        state.dragon_was_dying = false;
    }
    if (charge <= 0.01F)
    {
        if (system->alive(state.breath))
            system->kill(state.breath);
        state.breath = {};
        state.mouth_known = false;
    }

    // --- Rockets: one backblast as each leaves its launcher. ---
    for (auto &[id, seen] : state.rockets)
        seen = false;
    for (const RocketVisual &rocket : rocket_visuals_)
    {
        const auto [entry, fresh] = state.rockets.try_emplace(rocket.id, true);
        entry->second = true;
        if (!fresh)
            continue;
        const vfx::Vec3 forward = vfx::normalized(to_vfx(rocket.direction), {0.0F, 0.0F, -1.0F});
        vfx::SpawnParams params;
        params.position = to_vfx(rocket.position) - forward * 0.7F;
        params.direction = forward * -1.0F;
        (void)system->spawn(vfx::effects::rocket_backblast(), params);
    }
    std::erase_if(state.rockets, [](const auto &entry) { return !entry.second; });

    // --- Thrown grenades: a spitting fuse. They are the dark, thick visuals. ---
    for (CombatVfxState::Tracked &grenade : state.grenades)
        grenade.seen = false;
    for (const ProjectileVisual &projectile : projectile_visuals_)
    {
        if (std::max({projectile.color[0], projectile.color[1], projectile.color[2]}) >= 0.5F ||
            projectile.width < 0.09F)
            continue;
        const vfx::Vec3 position = to_vfx(projectile.head);
        CombatVfxState::Tracked *nearest{};
        float nearest_distance = 3.0F;
        for (CombatVfxState::Tracked &grenade : state.grenades)
        {
            const float distance = vfx::length(grenade.position - position);
            if (!grenade.seen && distance < nearest_distance)
            {
                nearest = &grenade;
                nearest_distance = distance;
            }
        }
        if (nearest == nullptr)
        {
            if (state.grenades.size() >= 16U)
                continue;
            vfx::SpawnParams params;
            params.position = position;
            state.grenades.push_back({system->spawn(vfx::effects::grenade_fuse(), params), position,
                                      true});
            continue;
        }
        system->move(nearest->handle, position, (position - nearest->position) * (1.0F / delta));
        nearest->position = position;
        nearest->seen = true;
    }
    std::erase_if(state.grenades, [&](const CombatVfxState::Tracked &grenade) {
        if (grenade.seen)
            return false;
        system->kill(grenade.handle);
        return true;
    });

    // --- Things lying on the ground catch the light now and then. ---
    state.sparkle_timer += delta * 5.0F * particle_density_;
    while (state.sparkle_timer >= 1.0F)
    {
        state.sparkle_timer -= 1.0F;
        if (block_pickups_.empty())
            break;
        const BlockPickup &pickup = block_pickups_[static_cast<std::size_t>(
            state.random01() * static_cast<float>(block_pickups_.size())) %
                                                    block_pickups_.size()];
        const float dx = pickup.position.x - player_position.x;
        const float dy = pickup.position.y - player_position.y;
        const float dz = pickup.position.z - player_position.z;
        if (pickup.flying_to_player || dx * dx + dy * dy + dz * dz > 24.0F * 24.0F)
            continue;
        vfx::SpawnParams params;
        params.position = {pickup.position.x,
                           pickup.position.y + std::sin(pickup.bounce_phase) * 0.1F + 0.12F,
                           pickup.position.z};
        (void)system->spawn(vfx::effects::pickup_sparkle(), params);
    }
}

// --- Diagnostics -------------------------------------------------------------
//
// OPENMINERZ_DIAGNOSTIC_VFX="combat:<scene>,key=value,..." runs one of the
// events of this file through the SAME function the game calls for it, in
// front of the camera, so a still or a benchmark shows the real path:
//   fireball    a dragon's shot crossing the view (speed=30, toward=1 flies at
//               the camera, ice=1)
//   fight       shots arriving from the sky and detonating round the camera
//   detonation  a fireball's impact (ice=1)
//   mine        a tool hit on the ground (block=<BlockType>, done=N: every Nth
//               hit removes the block)          bullet / bolt / place likewise
//   muzzle      a local shot (gun=0 pistol, 1 rifle, 2 shotgun, 3 laser)
//   hit, death, emerge, arrival, dragonhit, dragondeath, launch, breath
// Shared keys: dist (blocks ahead, default 8), side, up, age (play this long,
// then hold still for a reproducible still) or rate (repeat this many times a
// second, for a benchmark), r/g/b (a bolt's colour).
// Nothing a scene spawns is known to the simulation: its fireballs are drawn
// stand-ins that hit nothing, its hits break no block.
namespace
{
[[nodiscard]] float scene_number(std::string_view spec, std::string_view key, float fallback)
{
    std::size_t at = 0U;
    while (at < spec.size())
    {
        const std::size_t end = std::min(spec.find(',', at), spec.size());
        const std::string_view field = spec.substr(at, end - at);
        if (field.size() > key.size() + 1U && field.starts_with(key) && field[key.size()] == '=')
            return static_cast<float>(std::atof(std::string{field.substr(key.size() + 1U)}.c_str()));
        at = end + 1U;
    }
    return fallback;
}
} // namespace

void WorldPreviewRenderer::combat_vfx_scene_event()
{
    CombatVfxState &state = combat_vfx_state();
    const std::string_view spec{state.scene};
    constexpr std::string_view prefix{"combat:"};
    if (!spec.starts_with(prefix) || !overlay_camera_.valid)
        return;
    const std::string_view name =
        spec.substr(prefix.size(), std::min(spec.find(','), spec.size()) - prefix.size());
    const bool repeating = state.scene_rate > 0.0F;
    const world::Vec3 eye = overlay_camera_.eye;
    const world::Vec3 ahead{std::sin(overlay_camera_.yaw), 0.0F, -std::cos(overlay_camera_.yaw)};
    const world::Vec3 right{std::cos(overlay_camera_.yaw), 0.0F, std::sin(overlay_camera_.yaw)};
    const float distance = scene_number(spec, "dist", 8.0F);
    // A repeated scene wanders a little so its hits do not stack on a point.
    const float side = scene_number(spec, "side", 0.0F) +
                       (repeating ? (state.random01() - 0.5F) * 1.6F : 0.0F);
    const bool ice = scene_number(spec, "ice", 0.0F) > 0.5F;
    const auto ground_at = [&](float forward, float sideways) {
        world::Vec3 point{};
        // Underground the requested spot may be inside a wall: come back
        // toward the eye until there is air at eye level, then find the floor.
        for (int attempt = 0; attempt < 12; ++attempt, forward *= 0.8F, sideways *= 0.8F)
        {
            point = {eye.x + ahead.x * forward + right.x * sideways, eye.y,
                     eye.z + ahead.z * forward + right.z * sideways};
            if (!world::blocks_player(region_.block(cell(point.x), cell(eye.y), cell(point.z))))
                break;
        }
        int y = cell(eye.y);
        for (int step = 0; step < 48; ++step, --y)
            if (region_.contains(cell(point.x), y, cell(point.z)) &&
                world::blocks_player(region_.block(cell(point.x), y, cell(point.z))))
                break;
        point.y = static_cast<float>(y) + 1.0F;
        return point;
    };
    const world::Vec3 ground = ground_at(distance, side);
    const float lift = scene_number(spec, "up", 0.0F);
    const world::Vec3 raised{ground.x, ground.y + lift, ground.z};
    const std::array<float, 3> color{scene_number(spec, "r", 0.10F), scene_number(spec, "g", 0.45F),
                                     scene_number(spec, "b", 1.0F)};
    world::BlockBreakUpdate impact;
    impact.x = cell(ground.x);
    impact.y = cell(ground.y) - 1;
    impact.z = cell(ground.z);
    impact.block = static_cast<world::BlockType>(
        static_cast<std::uint8_t>(scene_number(spec, "block", 0.0F)));
    if (impact.block == world::BlockType::empty)
        impact.block = region_.block(impact.x, impact.y, impact.z);
    impact.intersection = ground;
    impact.normal_y = 1;
    impact.impact = true;
    const int every = static_cast<int>(scene_number(spec, "done", 0.0F));
    impact.completed = every > 0 && state.scene_count % every == every - 1;
    ++state.scene_count;

    if (name == "fireball")
    {
        const float speed = scene_number(spec, "speed", 30.0F);
        const float age = std::max(scene_number(spec, "age", 0.6F), 0.0F);
        const bool toward = scene_number(spec, "toward", 0.0F) > 0.5F;
        const world::Vec3 at{ground.x, ground.y + (lift > 0.0F ? lift : 4.0F), ground.z};
        // Where it must start to be at that point when the still is taken.
        const world::Vec3 heading =
            toward ? world::Vec3{-ahead.x, -0.08F, -ahead.z} : world::Vec3{right.x, -0.06F, right.z};
        CombatVfxState::Shot shot;
        shot.visual.velocity = {heading.x * speed, heading.y * speed, heading.z * speed};
        shot.visual.position = {at.x - shot.visual.velocity.x * age,
                                at.y - shot.visual.velocity.y * age,
                                at.z - shot.visual.velocity.z * age};
        shot.visual.id = state.next_shot_id++;
        shot.visual.ice = ice;
        shot.ground = -1.0e9F;
        state.shots.push_back(shot);
    }
    else if (name == "fight")
    {
        const float speed = scene_number(spec, "speed", 40.0F);
        const world::Vec3 target =
            ground_at(8.0F + state.random01() * 16.0F, (state.random01() - 0.5F) * 18.0F);
        const float far = 60.0F + state.random01() * 25.0F;
        const float across = (state.random01() - 0.5F) * 70.0F;
        const world::Vec3 from{eye.x + ahead.x * far + right.x * across, eye.y + 26.0F,
                               eye.z + ahead.z * far + right.z * across};
        const vfx::Vec3 heading = vfx::normalized(to_vfx(target) - to_vfx(from));
        CombatVfxState::Shot shot;
        shot.visual.position = from;
        shot.visual.velocity = {heading.x * speed, heading.y * speed, heading.z * speed};
        shot.visual.id = state.next_shot_id++;
        shot.visual.ice = ice || (scene_number(spec, "mixed", 0.0F) > 0.5F &&
                                  state.scene_count % 3 == 0);
        shot.ground = target.y;
        state.shots.push_back(shot);
    }
    else if (name == "detonation")
        spawn_fireball_explosion({ground.x, ground.y + 0.3F + lift, ground.z}, ice, true,
                                 ExplosionStyle::fireball);
    else if (name == "mine")
    {
        if (impact.completed)
        {
            // In play the block is gone by the time its burst is drawn. This is
            // the one place a scene edits its (diagnostic) world to match.
            region_.set_block(impact.x, impact.y, impact.z, world::BlockType::empty);
            rebuild_block_edit(impact.x, impact.y, impact.z);
        }
        spawn_mining_impact(impact);
    }
    else if (name == "bullet")
        spawn_tracer_impact(impact);
    else if (name == "bolt")
    {
        spawn_blaster_impact(impact, color);
        if (repeating)
            spawn_gun_flash(InventoryItemId::iron_space_assault_rifle, true, false, color,
                            std::nullopt, ahead, {});
    }
    else if (name == "place")
        spawn_block_placed(impact);
    else if (name == "muzzle")
    {
        const int gun = static_cast<int>(scene_number(spec, "gun", 0.0F));
        const float sine_pitch = std::sin(overlay_camera_.pitch);
        const float cosine_pitch = std::cos(overlay_camera_.pitch);
        spawn_gun_flash(gun == 0 ? InventoryItemId::pistol : InventoryItemId::assault_rifle,
                        gun == 3, gun == 2, color, std::nullopt,
                        {ahead.x * cosine_pitch, sine_pitch, ahead.z * cosine_pitch}, {});
    }
    else if (name == "hit")
        spawn_creature_hit({ground.x, ground.y + 1.2F + lift, ground.z});
    else if (name == "death")
        spawn_creature_death(raised);
    else if (name == "emerge")
        spawn_ground_burst(raised);
    else if (name == "arrival")
        spawn_arrival(raised);
    else if (name == "dragonhit")
        spawn_flash_effect({ground.x, ground.y + 3.0F + lift, ground.z});
    else
    {
        vfx::SpawnParams params;
        params.position = {ground.x, ground.y + 5.0F + lift, ground.z};
        params.direction = {right.x, 0.0F, right.z};
        if (name == "dragondeath")
            (void)spawn_effect(vfx::effects::dragon_death(ice), params);
        else if (name == "launch")
            (void)spawn_effect(vfx::effects::fireball_launch(ice), params);
        else if (name == "breath")
            (void)spawn_effect(vfx::effects::dragon_breath(ice), params);
        else
            SDL_Log("OPENMINERZ_DIAGNOSTIC_VFX: no combat scene named '%.*s'",
                    static_cast<int>(name.size()), name.data());
    }
}

void WorldPreviewRenderer::combat_vfx_play_scene(float delta)
{
    CombatVfxState &state = combat_vfx_state();
    if (state.scene.empty())
        return;
    for (CombatVfxState::Shot &shot : state.shots)
    {
        shot.age += delta;
        shot.visual.position.x += shot.visual.velocity.x * delta;
        shot.visual.position.y += shot.visual.velocity.y * delta;
        shot.visual.position.z += shot.visual.velocity.z * delta;
        shot.visual.spin += delta * 3.0F;
    }
    std::erase_if(state.shots, [&](const CombatVfxState::Shot &shot) {
        if (shot.visual.position.y > shot.ground + 0.6F && shot.age < 8.0F)
            return false;
        if (shot.age < 8.0F)
            spawn_fireball_explosion(shot.visual.position, shot.visual.ice, true,
                                     ExplosionStyle::fireball);
        return true;
    });
    if (state.scene_rate <= 0.0F)
        return;
    state.scene_timer += delta;
    const float period = 1.0F / state.scene_rate;
    while (state.scene_timer >= period)
    {
        state.scene_timer -= period;
        combat_vfx_scene_event();
    }
}

bool WorldPreviewRenderer::combat_vfx_diagnostic(std::string_view spec,
                                                 const world::PlayerState &camera)
{
    if (!enhanced_effects_ || effects() == nullptr)
        return false;
    CombatVfxState &state = combat_vfx_state();
    state.scene = std::string{spec};
    state.scene_rate = scene_number(spec, "rate", 0.0F);
    state.scene_timer = 0.0F;
    state.scene_count = 0;
    state.shots.clear();
    const float age = scene_number(spec, "age", -1.0F);
    if (spec.starts_with("combat:muzzle") && age >= 0.0F && state.scene_rate <= 0.0F)
    {
        // Held until the gun has been drawn (combat_vfx_frame fires it).
        state.scene_defer = 3;
        state.scene_age = age;
        state.frozen = true;
        return true;
    }
    combat_vfx_scene_event();
    if (age < 0.0F || state.scene_rate > 0.0F)
        return false;
    // Play it forward exactly as the game would, then hold that instant.
    constexpr float step = 1.0F / 60.0F;
    const vfx::Vec3 eye{camera.position.x,
                        camera.position.y + world::PlayerSimulation::eye_height,
                        camera.position.z};
    for (float simulated = 0.0F; simulated < age; simulated += step)
    {
        combat_vfx_update(step, camera.position);
        effects()->update(step, eye);
    }
    state.frozen = true;
    return true;
}

void WorldPreviewRenderer::combat_vfx_pickup_collected(world::Vec3 position)
{
    if (!enhanced_effects_)
        return;
    vfx::SpawnParams params;
    params.position = to_vfx(position);
    (void)spawn_effect(vfx::effects::pickup_collect(), params);
}

// Per draw, into the frame the effects layer is about to hand over: the things
// that are drawn exactly where something is THIS frame rather than simulated.
void WorldPreviewRenderer::combat_vfx_frame(const vfx::View &view, vfx::Frame &frame,
                                            float elapsed_seconds)
{
    if (!enhanced_effects_)
        return;
    if (combat_vfx_state_ != nullptr && combat_vfx_state_->scene_defer > 0 &&
        --combat_vfx_state_->scene_defer == 0 && effects() != nullptr)
    {
        // The deferred diagnostic muzzle flash: fired now, played to its age.
        combat_vfx_scene_event();
        constexpr float step = 1.0F / 240.0F;
        for (float simulated = 0.0F; simulated < combat_vfx_state_->scene_age; simulated += step)
            effects()->update(step, view.eye);
    }
    // Creatures, read where the draw's own list of them is valid: dirt thrown
    // up as one emerges, dust as one goes down. (Read-only: what a creature
    // is doing is decided by the simulation and only looked at here.)
    if (visible_enemies_ != nullptr)
    {
        CombatVfxState &state = combat_vfx_state();
        for (auto &[id, creature] : state.creatures)
            creature.seen = false;
        for (const world::Enemy &enemy : *visible_enemies_)
        {
            const auto [entry, fresh] = state.creatures.try_emplace(enemy.id);
            if (fresh && enemy.state == world::EnemyState::emerge)
                spawn_ground_burst(enemy.position);
            else if (!fresh && enemy.state == world::EnemyState::die &&
                     entry->second.state != world::EnemyState::die)
                spawn_creature_death(enemy.position);
            entry->second.state = enemy.state;
            entry->second.seen = true;
        }
        std::erase_if(state.creatures, [](const auto &entry) { return !entry.second.seen; });
    }

    // Tracers and bolts: a glowing sheath round the original's spear, and a
    // whiter point at a bolt's head. The spear itself (shape, length, colour)
    // is still drawn by the entity pass.
    for (const ProjectileVisual &projectile : projectile_visuals_)
    {
        const float brightness =
            std::max({projectile.color[0], projectile.color[1], projectile.color[2]});
        if (brightness < 0.5F)
            continue;
        const vfx::Vec3 head = to_vfx(projectile.head);
        const vfx::Vec3 tail = to_vfx(projectile.tail);
        const float reach = vfx::length(head - tail);
        if (reach < 0.02F)
            continue;
        const bool bolt = projectile.width > 0.045F;
        const vfx::Vec3 axis = (head - tail) * (1.0F / reach);
        const vfx::Vec3 middle = (head + tail) * 0.5F;
        vfx::Instance sheath;
        sheath.position[0] = middle.x;
        sheath.position[1] = middle.y;
        sheath.position[2] = middle.z;
        sheath.size = projectile.width * (bolt ? 1.3F : 1.2F);
        sheath.stretch = reach * 0.5F;
        sheath.axis[0] = axis.x;
        sheath.axis[1] = axis.y;
        sheath.axis[2] = axis.z;
        sheath.color[0] = projectile.color[0];
        sheath.color[1] = projectile.color[1];
        sheath.color[2] = projectile.color[2];
        sheath.alpha = bolt ? 0.85F : 0.45F;
        sheath.emissive = bolt ? 7.0F : 3.0F;
        sheath.material = static_cast<float>(vfx::Material::spark);
        frame.sharp.push_back(sheath);
        if (!bolt)
            continue;
        vfx::Instance point = sheath;
        point.position[0] = head.x;
        point.position[1] = head.y;
        point.position[2] = head.z;
        point.size = projectile.width * 2.2F;
        point.stretch = projectile.width * 1.2F;
        point.color[0] = 0.6F + 0.4F * projectile.color[0];
        point.color[1] = 0.6F + 0.4F * projectile.color[1];
        point.color[2] = 0.6F + 0.4F * projectile.color[2];
        point.alpha = 0.9F;
        point.emissive = 12.0F * (0.92F + 0.08F * std::sin(elapsed_seconds * 90.0F));
        frame.sharp.push_back(point);
    }
}

// --- Events the application reports. ---------------------------------------

void WorldPreviewRenderer::spawn_gun_flash(InventoryItemId item, bool laser, bool shotgun,
                                           const std::array<float, 3> &bolt_color,
                                           std::optional<world::Vec3> world_muzzle,
                                           world::Vec3 direction, world::Vec3 shooter_velocity)
{
    if (!enhanced_effects_)
        return;
    const vfx::Vec3 aim = vfx::normalized(to_vfx(direction), {0.0F, 0.0F, -1.0F});
    vfx::Vec3 muzzle{};
    vfx::Vec3 right{1.0F, 0.0F, 0.0F};
    vfx::Vec3 up{0.0F, 1.0F, 0.0F};
    const bool local = !world_muzzle.has_value();
    if (local)
    {
        // The local player's: the muzzle the viewmodel pass last drew, as the
        // point of the WORLD camera that covers the same pixel. The flash is
        // drawn in the scene (so it blooms, lights the smoke and is fogged
        // like everything else) and the weapon is composited over it.
        if (!overlay_camera_.valid)
            return;
        const float sine_yaw = std::sin(overlay_camera_.yaw);
        const float cosine_yaw = std::cos(overlay_camera_.yaw);
        const float sine_pitch = std::sin(overlay_camera_.pitch);
        const float cosine_pitch = std::cos(overlay_camera_.pitch);
        right = {cosine_yaw, 0.0F, sine_yaw};
        up = {-sine_yaw * sine_pitch, cosine_pitch, cosine_yaw * sine_pitch};
        const vfx::Vec3 forward{sine_yaw * cosine_pitch, sine_pitch, -cosine_yaw * cosine_pitch};
        // Where retail's Player.GetGunTipPosition puts it when no gun was
        // drawn last frame (scoped, hidden): low and right of the eye.
        view::ViewPoint point{0.28F, -0.22F, 0.55F};
        // (Between draws the renderer holds last frame's muzzle; during one it
        // has been cleared and the viewmodel still holds it.)
        std::optional<view::ViewmodelPoint> drawn = first_person_muzzle_;
        if (!drawn.has_value() && avatar_viewmodel_ != nullptr)
            drawn = avatar_viewmodel_->muzzle();
        if (drawn.has_value())
            point = view::viewmodel_to_world_view(drawn->point, drawn->fov_degrees,
                                                  overlay_camera_.vertical_fov_degrees);
        muzzle = to_vfx(overlay_camera_.eye) + right * point.x + up * point.y +
                 forward * std::max(point.z, 0.2F);
    }
    else
    {
        muzzle = to_vfx(*world_muzzle);
        right = vfx::normalized(vfx::cross(aim, {0.0F, 1.0F, 0.0F}), {1.0F, 0.0F, 0.0F});
        up = vfx::cross(right, aim);
    }
    const bool pistol = item == InventoryItemId::pistol || item == InventoryItemId::gold_pistol ||
                        item == InventoryItemId::diamond_pistol ||
                        item == InventoryItemId::blood_stone_pistol;
    const vfx::Muzzle kind = laser     ? vfx::Muzzle::laser
                             : shotgun ? vfx::Muzzle::shotgun
                             : pistol  ? vfx::Muzzle::pistol
                                       : vfx::Muzzle::rifle;
    vfx::SpawnParams params;
    params.position = muzzle;
    params.direction = aim;
    params.velocity = to_vfx(shooter_velocity);
    if (laser)
    {
        params.tint = bolt_tint(bolt_color);
        // A scattergun's five bolts leave as one wider burst.
        if (shotgun)
            params.scale = 1.35F;
    }
    (void)spawn_effect(vfx::effects::muzzle_flash(kind), params);
    if (laser || !local)
        return;
    // The spent case, thrown up and to the right of the breech.
    vfx::SpawnParams casing;
    casing.position = muzzle - aim * 0.30F + right * 0.03F;
    casing.direction = vfx::normalized(right * 0.85F + up * 0.50F - aim * 0.15F);
    casing.velocity = params.velocity;
    add_tile(casing,
             world::atlas_tile(world::BlockType::golden_wall, world::FaceDirection::positive_x));
    (void)spawn_effect(vfx::effects::shell_casing(), casing);
}

void WorldPreviewRenderer::spawn_block_placed(const world::BlockBreakUpdate &placement)
{
    if (!enhanced_effects_)
    {
        // The Original presentation: ProcessDigMessage's burst, as retail.
        spawn_mining_impact(placement);
        return;
    }
    world::Vec3 normal{static_cast<float>(placement.normal_x),
                       static_cast<float>(placement.normal_y),
                       static_cast<float>(placement.normal_z)};
    if (placement.normal_x == 0 && placement.normal_y == 0 && placement.normal_z == 0)
        normal = {0.0F, 1.0F, 0.0F};
    combat_vfx_strike(static_cast<int>(vfx::Strike::place), placement.block,
                      placement.intersection, normal);
}

void WorldPreviewRenderer::spawn_creature_hit(world::Vec3 position)
{
    if (!enhanced_effects_)
    {
        // The Original presentation: TracerManager's untinted SmokeEffect.
        spawn_impact_smoke(position, {1.0F, 1.0F, 1.0F});
        return;
    }
    vfx::SpawnParams params;
    params.position = to_vfx(position);
    // Knocked back toward the shooter's side: away from the body.
    if (overlay_camera_.valid)
        params.direction = vfx::normalized(to_vfx(overlay_camera_.eye) - params.position);
    (void)spawn_effect(vfx::effects::creature_hit(), params);
}

void WorldPreviewRenderer::spawn_creature_death(world::Vec3 feet)
{
    if (!enhanced_effects_)
        return;
    vfx::SpawnParams params;
    params.position = to_vfx(feet);
    (void)spawn_effect(vfx::effects::creature_death(), params);
}

void WorldPreviewRenderer::spawn_ground_burst(world::Vec3 feet)
{
    if (!enhanced_effects_)
        return;
    // Whatever the ground is made of there, thrown up from under the feet.
    const int x = cell(feet.x);
    const int z = cell(feet.z);
    int y = cell(feet.y - 0.05F);
    if (!region_.contains(x, y, z))
        return;
    world::BlockType block = region_.block(x, y, z);
    if (!world::blocks_player(block) && region_.contains(x, y - 1, z))
        block = region_.block(x, --y, z);
    if (!world::blocks_player(block))
        return;
    // (A whole wave can emerge at once: a hit's worth of dirt each, not a
    // broken block's.)
    combat_vfx_strike(static_cast<int>(vfx::Strike::tool), block,
                      {feet.x, static_cast<float>(y) + 1.05F, feet.z}, {0.0F, 1.0F, 0.0F});
}

void WorldPreviewRenderer::spawn_arrival(world::Vec3 feet)
{
    if (!enhanced_effects_)
        return;
    vfx::SpawnParams params;
    params.position = to_vfx(feet);
    (void)spawn_effect(vfx::effects::arrival(), params);
}

void WorldPreviewRenderer::spawn_melee_sparks(world::Vec3 point, world::Vec3 normal,
                                              const std::array<float, 3> *blade_color)
{
    if (!enhanced_effects_)
        return;
    vfx::SpawnParams params;
    params.position = to_vfx(point);
    params.direction = vfx::normalized(to_vfx(normal));
    if (blade_color != nullptr)
        params.tint = bolt_tint(*blade_color);
    (void)spawn_effect(vfx::effects::melee_sparks(), params);
}

void WorldPreviewRenderer::spawn_footstep_dust(world::Vec3 feet, world::Vec3 velocity)
{
    if (!enhanced_effects_)
        return;
    const int x = cell(feet.x);
    const int y = cell(feet.y - 0.05F);
    const int z = cell(feet.z);
    if (!region_.contains(x, y, z))
        return;
    const world::BlockType block = region_.block(x, y, z);
    // Only ground that is dry and loose raises dust underfoot.
    const vfx::Surface surface = vfx::surface_of(block);
    if (!world::blocks_player(block) ||
        (surface != vfx::Surface::dirt && surface != vfx::Surface::sand &&
         surface != vfx::Surface::snow) ||
        block == world::BlockType::grass)
        return;
    vfx::SpawnParams params;
    params.position = {feet.x, static_cast<float>(y) + 1.02F, feet.z};
    params.velocity = to_vfx(velocity);
    params.tint = vfx::dust_color(block);
    (void)spawn_effect(vfx::effects::footstep_dust(), params);
}

} // namespace openminerz
