// (mod-ai) Mod projectiles: arrows, spit, fireballs. Simulated on the host
// only, which alone decides what they hit; clients are told the launch
// (prj+, with velocity and gravity, so they fly it themselves), homing ones'
// corrections ten times a second (prj~), and the end (prj-).
//
// A hit on a player is tested against where the session's authority says the
// player was over its lag (GameplayBackend::projectile_hits_player) and goes
// through the same damage path as every creature hit: player.damaged, then
// the authority in an OpenMinerZ-only session, or the local player's own
// damage offline. Projectiles do not hurt their owner or its friends.
// Explosions hurt everything in range (behind blocks a quarter as much) and
// dig blocks out only where GameplayBackend::creature_block_damage_allowed
// says the session's rules let monsters break blocks.

#include "internal.hpp"

#include "openminerz/world/block.hpp"
#include "openminerz/world/explosive.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::gameplay
{
namespace
{

// Where a segment first enters a box, as a fraction of the segment.
[[nodiscard]] std::optional<double> segment_box(Vec3 from, Vec3 to, Vec3 low, Vec3 high) noexcept
{
    const std::array<double, 3> o{from.x, from.y, from.z};
    const std::array<double, 3> d{to.x - from.x, to.y - from.y, to.z - from.z};
    const std::array<double, 3> lo{low.x, low.y, low.z};
    const std::array<double, 3> hi{high.x, high.y, high.z};
    double near = 0.0;
    double far = 1.0;
    for (std::size_t axis{}; axis < 3U; ++axis)
    {
        if (std::abs(d[axis]) < 1e-12)
        {
            if (o[axis] < lo[axis] || o[axis] > hi[axis])
                return std::nullopt;
            continue;
        }
        double t0 = (lo[axis] - o[axis]) / d[axis];
        double t1 = (hi[axis] - o[axis]) / d[axis];
        if (t0 > t1)
            std::swap(t0, t1);
        near = std::max(near, t0);
        far = std::min(far, t1);
        if (near > far)
            return std::nullopt;
    }
    return near;
}

[[nodiscard]] double length_of(Vec3 v) noexcept
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

[[nodiscard]] Vec3 narrow(Vec3 v) noexcept
{
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

} // namespace

void GameplayService::Impl::define_projectile(Side side, std::string_view mod, std::string_view name,
                                              const Value &def)
{
    Registry &definitions = registry(side);
    ProjectileDef projectile;
    projectile.key = qualify(mod, name);
    projectile.mod = std::string{mod};
    const auto number = [&](std::string_view key, double fallback, double low, double high) {
        const double value = number_or(def, key, fallback);
        return static_cast<float>(std::isfinite(value) ? std::clamp(value, low, high) : fallback);
    };
    projectile.speed = number("speed", 20.0, 0.1, 200.0);
    projectile.gravity = number("gravity", 0.0, -100.0, 100.0);
    projectile.lifetime = number("lifetime", 6.0, 0.05, 60.0);
    projectile.radius = number("radius", 0.15, 0.01, 4.0);
    projectile.damage = number("damage", 10.0, 0.0, 1000.0);
    projectile.creature_damage = number("creature_damage", -1.0, -1.0, 100'000.0);
    projectile.homing = number("homing", 0.0, 0.0, 1440.0);
    projectile.model = string_or(def, "model", "");
    projectile.texture = string_or(def, "texture", "");
    projectile.scale = number("scale", 1.0, 0.05, 16.0);
    projectile.fx = string_or(def, "fx", "");
    projectile.impact_fx = string_or(def, "impact_fx", "");
    projectile.pierce = bool_or(def, "pierce", false);
    projectile.breaks_blocks = bool_or(def, "breaks_blocks", false);
    if (const Value *explosion = def.field("explosion"); explosion != nullptr && explosion->type() == ValueType::map)
    {
        const auto blast = [&](std::string_view key, double fallback, double low, double high) {
            const double value = number_or(*explosion, key, fallback);
            return static_cast<float>(std::isfinite(value) ? std::clamp(value, low, high) : fallback);
        };
        projectile.explosion_radius = blast("radius", 3.0, 0.0, 8.0);
        projectile.explosion_damage = blast("damage", projectile.damage, 0.0, 1000.0);
        projectile.block_damage = bool_or(*explosion, "blocks", false);
        projectile.breaks_blocks = projectile.breaks_blocks || bool_or(*explosion, "breaks_blocks", false);
    }
    std::vector<Callback> kept;
    projectile.on_hit = callback_or_none(def, "on_hit");
    if (projectile.on_hit.valid())
    {
        kept.push_back(projectile.on_hit);
        hold(side, projectile.on_hit);
    }
    release_unused(def, kept);
    definitions.projectiles.insert_or_assign(projectile.key, std::move(projectile));
}

const ProjectileDef *GameplayService::Impl::projectile_def(std::string_view key, std::string_view mod) const noexcept
{
    if (const auto found = server_registry.projectiles.find(key); found != server_registry.projectiles.end())
        return &found->second;
    const auto local = server_registry.projectiles.find(std::format("{}:{}", mod, key));
    return local == server_registry.projectiles.end() ? nullptr : &local->second;
}

std::optional<Vec3> GameplayService::Impl::aim_velocity(Vec3 from, Vec3 target, Vec3 target_velocity,
                                                        const ProjectileDef &def, bool lead)
{
    const double speed = def.speed;
    Vec3 aim = target;
    if (lead)
    {
        // Where it will be when the shot arrives (two refinements).
        for (int pass = 0; pass < 2; ++pass)
        {
            const double time = length_of({aim.x - from.x, aim.y - from.y, aim.z - from.z}) / speed;
            aim = {target.x + target_velocity.x * time, target.y + target_velocity.y * time,
                   target.z + target_velocity.z * time};
        }
    }
    const double dx = aim.x - from.x;
    const double dy = aim.y - from.y;
    const double dz = aim.z - from.z;
    const double flat = std::sqrt(dx * dx + dz * dz);
    if (flat < 1e-6 && std::abs(dy) < 1e-6)
        return std::nullopt;
    const double g = def.gravity;
    if (std::abs(g) < 1e-6 || flat < 1e-6)
    {
        const double length = std::sqrt(flat * flat + dy * dy);
        return Vec3{dx / length * speed, dy / length * speed, dz / length * speed};
    }
    // The low arc that lands on it; out of range, the farthest shot (45 deg).
    const double v2 = speed * speed;
    const double root = v2 * v2 - g * (g * flat * flat + 2.0 * dy * v2);
    const double angle = root >= 0.0 ? std::atan((v2 - std::sqrt(root)) / (g * flat)) : 0.7853981633974483;
    return Vec3{dx / flat * std::cos(angle) * speed, std::sin(angle) * speed, dz / flat * std::cos(angle) * speed};
}

Value GameplayService::Impl::projectile_value(const Projectile &projectile) const
{
    const ProjectileDef &def = *projectile.def;
    return make_map({{"t", "prj+"},
                     {"id", static_cast<std::int64_t>(projectile.id)},
                     {"kind", projectile.key},
                     {"model", def.model},
                     {"texture", def.texture},
                     {"fx", def.fx},
                     {"ifx", def.impact_fx},
                     {"boom", def.explosion_radius},
                     {"scale", def.scale},
                     {"r", def.radius},
                     {"p", vec3_value(narrow(projectile.position))},
                     {"v", vec3_value(narrow(projectile.velocity))},
                     {"g", projectile.def->homing > 0.0F ? 0.0F : def.gravity}});
}

Projectile *GameplayService::Impl::launch_projectile(const ProjectileDef &def, Vec3 from, Vec3 velocity,
                                                     std::uint64_t owner_entity, std::uint64_t owner_player,
                                                     std::uint64_t target_player, std::uint64_t target_entity,
                                                     float damage_scale)
{
    if (!finite(from) || !finite(velocity) || length_of(velocity) < 1e-6)
        return nullptr;
    const auto alive = std::ranges::count_if(ai.projectiles, [](const Projectile &p) { return !p.removed; });
    if (static_cast<std::size_t>(alive) >= ai.limits.max_projectiles)
        return nullptr;
    Projectile projectile;
    projectile.id = ai.next_projectile_id++;
    projectile.def = &def;
    projectile.key = def.key;
    projectile.position = from;
    projectile.velocity = velocity;
    projectile.born = server_time;
    projectile.owner_entity = owner_entity;
    projectile.owner_player = owner_player;
    if (const Entity *shooter = entity(owner_entity))
        projectile.team = team_of_entity(*shooter);
    else if (owner_player != 0U)
        projectile.team = team_of_player(owner_player);
    projectile.target_player = target_player;
    projectile.target_entity = target_entity;
    projectile.damage_scale = damage_scale;
    ai.projectiles.push_back(projectile);
    out().to_all(projectile_value(projectile), true);
    const std::uint64_t id = projectile.id;
    emit(Side::server, "projectile.launched",
         make_map({{"projectile", Handle{projectile_handle_kind, id}},
                   {"kind", def.key},
                   {"owner", owner_entity != 0U   ? Value{Handle{HandleKind::entity, owner_entity}}
                             : owner_player != 0U ? Value{Handle{HandleKind::player, owner_player}}
                                                  : Value{}},
                   {"position", vec3_value(from)}}));
    const auto found = std::ranges::find(ai.projectiles, id, &Projectile::id);
    return found == ai.projectiles.end() || found->removed ? nullptr : &*found;
}

void GameplayService::Impl::explode(const ProjectileDef &def, Vec3 at, std::uint64_t owner_entity,
                                    std::uint64_t owner_player, float damage_scale)
{
    const float radius = def.explosion_radius;
    if (radius <= 0.0F)
        return;
    GameplayService &service = owner;
    service.noise(at, 48.0F, "explosion", std::nullopt);
    const auto falloff = [&](Vec3 point) -> float {
        const double distance = std::sqrt(distance_squared(point, at));
        if (distance > radius)
            return 0.0F;
        const float open = line_of_sight(at, point) ? 1.0F : 0.25F;
        return static_cast<float>(1.0 - distance / radius) * open;
    };
    std::vector<std::pair<std::uint64_t, float>> hurt_players;
    for (const PlayerRecord &player : players)
        if (player.present && player.alive)
        {
            const float share = falloff({player.position.x, player.position.y + 1.0, player.position.z});
            if (share > 0.0F)
                hurt_players.emplace_back(player.serial, share);
        }
    std::vector<std::pair<std::uint64_t, float>> hurt_entities;
    for (const Entity &body : entities)
        if (!body.removed && body.id != owner_entity)
        {
            const float share = falloff({body.position.x, body.position.y + 0.8, body.position.z});
            if (share > 0.0F)
                hurt_entities.emplace_back(body.id, share);
        }
    for (const auto &[serial, share] : hurt_players)
        if (PlayerRecord *victim = player_by_serial(serial))
            damage(*victim, def.explosion_damage * share * damage_scale,
                   owner_player != 0U ? DamageCause::player : DamageCause::creature, at);
    for (const auto &[id, share] : hurt_entities)
        if (Entity *victim = entity(id))
            damage_entity(*victim,
                          (def.creature_damage >= 0.0F ? def.creature_damage : def.explosion_damage / 10.0F) * share *
                              damage_scale,
                          owner_player, owner_entity);
    if (!def.block_damage && !def.breaks_blocks)
        return;
    std::vector<EditCell> cells;
    const int reach = static_cast<int>(std::ceil(std::min(radius, 4.0F)));
    const BlockPos centre = floor_cell(at);
    for (int y = -reach; y <= reach; ++y)
        for (int z = -reach; z <= reach; ++z)
            for (int x = -reach; x <= reach; ++x)
            {
                if (x * x + y * y + z * z > static_cast<int>(radius * radius * 0.6F))
                    continue;
                const BlockPos cell{centre.x + x, centre.y + y, centre.z + z};
                if (projectile_may_break(def, cell))
                    cells.push_back({cell, 0U, 0U});
            }
    if (!cells.empty())
        static_cast<void>(queue_job(def.mod, std::move(cells), {}));
}

bool GameplayService::Impl::projectile_may_break(const ProjectileDef &def, BlockPos cell) const
{
    const auto block = block_at(cell);
    if (!block.has_value() || *block == 0U || !world::rocket_breaks_block(static_cast<world::BlockType>(*block)))
        return false;
    // (fix) breaks_blocks: the mod's own choice, still never inside spawn
    // protection; `blocks` alone: where retail lets a dragon's fireball dig.
    return def.breaks_blocks ? backend.projectile_block_breaking_allowed(cell)
                             : def.block_damage && backend.creature_block_damage_allowed(cell);
}

void GameplayService::Impl::tick_projectiles(float dt)
{
    if (ai.projectiles.empty())
        return;
    const bool due = (ai.pass % 2U) == 0U;
    Array corrections;
    const std::size_t count = ai.projectiles.size();
    for (std::size_t index{}; index < count && index < ai.projectiles.size(); ++index)
    {
        if (ai.projectiles[index].removed)
            continue;
        Projectile &shot = ai.projectiles[index];
        const ProjectileDef &def = *shot.def;
        const std::uint64_t id = shot.id;
        const auto finish = [&](Vec3 where, std::string_view hit) {
            const auto found = std::ranges::find(ai.projectiles, id, &Projectile::id);
            if (found == ai.projectiles.end() || found->removed)
                return;
            found->removed = true;
            out().to_all(make_map({{"t", "prj-"},
                                   {"id", static_cast<std::int64_t>(id)},
                                   {"p", vec3_value(narrow(where))},
                                   {"hit", std::string{hit}}}),
                         true);
        };
        if (server_time - shot.born > def.lifetime)
        {
            finish(shot.position, "expired");
            continue;
        }
        // Homing: turn toward the target's chest, at most `homing` deg/s.
        if (def.homing > 0.0F)
            if (const std::optional<Vec3> target = target_point(shot.target_player, shot.target_entity))
            {
                const Vec3 chest{target->x, target->y + 1.0, target->z};
                const double speed = length_of(shot.velocity);
                Vec3 want{chest.x - shot.position.x, chest.y - shot.position.y, chest.z - shot.position.z};
                const double want_length = length_of(want);
                if (speed > 1e-6 && want_length > 1e-6)
                {
                    const Vec3 have{shot.velocity.x / speed, shot.velocity.y / speed, shot.velocity.z / speed};
                    want = {want.x / want_length, want.y / want_length, want.z / want_length};
                    const double cosine = std::clamp(have.x * want.x + have.y * want.y + have.z * want.z, -1.0, 1.0);
                    const double angle = std::acos(cosine);
                    const double turn = std::min(1.0, def.homing * 0.017453292519943295 * dt / std::max(angle, 1e-6));
                    Vec3 next{have.x + (want.x - have.x) * turn, have.y + (want.y - have.y) * turn,
                              have.z + (want.z - have.z) * turn};
                    const double next_length = length_of(next);
                    if (next_length > 1e-6)
                        shot.velocity = {next.x / next_length * speed, next.y / next_length * speed,
                                         next.z / next_length * speed};
                }
            }
        if (def.homing <= 0.0F)
            shot.velocity.y -= def.gravity * dt;
        const Vec3 from = shot.position;
        const Vec3 to{from.x + shot.velocity.x * dt, from.y + shot.velocity.y * dt, from.z + shot.velocity.z * dt};
        // The first thing on the segment: terrain, a player, a creature.
        double best = 2.0;
        std::string hit_kind;
        std::uint64_t hit_player{};
        std::uint64_t hit_entity{};
        {
            const double length = length_of({to.x - from.x, to.y - from.y, to.z - from.z});
            const int samples = std::max(1, static_cast<int>(std::ceil(length / 0.25)));
            for (int sample = 1; sample <= samples; ++sample)
            {
                const double t = static_cast<double>(sample) / samples;
                const Vec3 point{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                                 from.z + (to.z - from.z) * t};
                const BlockPos cell = floor_cell(point);
                if (ai_solid(cell.x, cell.y, cell.z))
                {
                    best = std::max(0.0, t - 0.5 / samples);
                    hit_kind = "block";
                    break;
                }
            }
        }
        const CreatureAi *owner_ai{};
        const Entity *owner_body = entity(shot.owner_entity);
        if (owner_body != nullptr)
            if (const CreatureDef *creature = server_registry.creature(owner_body->creature))
                owner_ai = creature->ai.get();
        const auto spares = [&](std::string_view team) {
            return !shot.team.empty() && relation(shot.team, team, owner_ai) == Relation::friendly;
        };
        for (const PlayerRecord &player : players)
        {
            if (!player.present || !player.alive || player.serial == shot.owner_player ||
                std::ranges::find(shot.pierced, player.serial | (1ULL << 63)) != shot.pierced.end() ||
                spares(team_of_player(player.serial)))
                continue;
            if (distance_squared(player.position, from) > 64.0 * 64.0)
                continue;
            std::optional<double> t;
            if (const auto answered = backend.projectile_hits_player(player.gid, from, to); answered.has_value())
            {
                if (answered->has_value())
                {
                    const double length = std::max(1e-6, length_of({to.x - from.x, to.y - from.y, to.z - from.z}));
                    t = std::clamp(static_cast<double>(**answered) / length, 0.0, 1.0);
                }
            }
            else
            {
                const double grow = def.radius;
                t = segment_box(from, to, {player.position.x - 0.3 - grow, player.position.y - grow, player.position.z - 0.3 - grow},
                                {player.position.x + 0.3 + grow, player.position.y + 1.8 + grow,
                                 player.position.z + 0.3 + grow});
            }
            if (t.has_value() && *t < best)
            {
                best = *t;
                hit_kind = "player";
                hit_player = player.serial;
                hit_entity = 0U;
            }
        }
        for (const Entity &body : entities)
        {
            if (body.removed || body.id == shot.owner_entity ||
                std::ranges::find(shot.pierced, body.id) != shot.pierced.end() ||
                distance_squared(body.position, from) > 64.0 * 64.0 || spares(team_of_entity(body)))
                continue;
            const CreatureDef *creature = server_registry.creature(body.creature);
            if (creature == nullptr)
                continue;
            const double half = creature->width * 0.5 + def.radius;
            const auto t = segment_box(from, to, {body.position.x - half, body.position.y - def.radius, body.position.z - half},
                                       {body.position.x + half, body.position.y + creature->height + def.radius,
                                        body.position.z + half});
            if (t.has_value() && *t < best)
            {
                best = *t;
                hit_kind = "entity";
                hit_entity = body.id;
                hit_player = 0U;
            }
        }
        if (hit_kind.empty())
        {
            shot.position = to;
            if (def.homing > 0.0F && due && shot.sent_timer <= 0.0F)
                corrections.push_back(make_map({{"id", static_cast<std::int64_t>(id)},
                                                {"p", vec3_value(narrow(to))},
                                                {"v", vec3_value(narrow(shot.velocity))}}));
            continue;
        }
        const Vec3 at{from.x + (to.x - from.x) * best, from.y + (to.y - from.y) * best, from.z + (to.z - from.z) * best};
        // projectile.hit: return false and it flies on (through a player or
        // a creature) or simply ends (terrain), without damage.
        const std::uint64_t owner_entity = shot.owner_entity;
        const std::uint64_t owner_player = shot.owner_player;
        const float scale = shot.damage_scale;
        const Value target = hit_player != 0U   ? Value{Handle{HandleKind::player, hit_player}}
                             : hit_entity != 0U ? Value{Handle{HandleKind::entity, hit_entity}}
                                                : Value{};
        std::string block_name;
        const BlockPos struck =
            floor_cell({at.x + shot.velocity.x * 0.02, at.y + shot.velocity.y * 0.02, at.z + shot.velocity.z * 0.02});
        if (hit_kind == "block")
            if (const auto block = block_at(struck))
                block_name = std::string{retail_block_name(*block)};
        const Value hit_info = make_map({{"projectile", Handle{projectile_handle_kind, id}},
                                         {"kind", def.key},
                                         {"owner", owner_entity != 0U   ? Value{Handle{HandleKind::entity, owner_entity}}
                                                   : owner_player != 0U ? Value{Handle{HandleKind::player, owner_player}}
                                                                        : Value{}},
                                         {"position", vec3_value(at)},
                                         {"target", target},
                                         {"block", block_name.empty() ? Value{} : Value{block_name}},
                                         {"damage", def.damage * scale}});
        const bool allowed = emit(Side::server, "projectile.hit", hit_info);
        if (!allowed)
        {
            const auto found = std::ranges::find(ai.projectiles, id, &Projectile::id);
            if (found == ai.projectiles.end() || found->removed)
                continue;
            if (hit_kind == "block")
                finish(at, "block");
            else
            {
                found->pierced.push_back(hit_player != 0U ? (hit_player | (1ULL << 63)) : hit_entity);
                found->position = to;
            }
            continue;
        }
        if (hit_player != 0U)
        {
            if (PlayerRecord *victim = player_by_serial(hit_player))
                damage(*victim, def.damage * scale, owner_player != 0U ? DamageCause::player : DamageCause::creature,
                       at);
        }
        else if (hit_entity != 0U)
        {
            if (Entity *victim = entity(hit_entity))
                damage_entity(*victim, (def.creature_damage >= 0.0F ? def.creature_damage : def.damage / 10.0F) * scale,
                              owner_player, owner_entity);
        }
        explode(def, at, owner_entity, owner_player, scale);
        // (fix) Without an explosion, a projectile that breaks blocks breaks
        // the one it struck.
        if (hit_kind == "block" && def.breaks_blocks && def.explosion_radius <= 0.0F &&
            projectile_may_break(def, struck))
            static_cast<void>(queue_job(def.mod, std::vector<EditCell>{{struck, 0U, 0U}}, {}));
        if (def.on_hit.valid())
        {
            const std::array<Value, 2> args{Value{Handle{projectile_handle_kind, id}}, hit_info};
            static_cast<void>(host.invoke(def.on_hit, args));
        }
        const auto found = std::ranges::find(ai.projectiles, id, &Projectile::id);
        if (found == ai.projectiles.end() || found->removed)
            continue;
        if (def.pierce && hit_kind != "block")
        {
            found->pierced.push_back(hit_player != 0U ? (hit_player | (1ULL << 63)) : hit_entity);
            found->position = to;
            continue;
        }
        finish(at, hit_kind);
    }
    if (!corrections.empty())
        out().to_all(make_map({{"t", "prj~"}, {"list", Value{std::move(corrections)}}}), false);
    std::erase_if(ai.projectiles, [](const Projectile &projectile) { return projectile.removed; });
}

} // namespace openminerz::modding::gameplay
