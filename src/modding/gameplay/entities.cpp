// Mod creatures and NPCs: server-simulated, replicated over the mod channel.
//
// Retail enemies are untouched: they stay on the stock EnemyHorde path and its
// messages. A mod entity exists only on the server and, as presentation, in
// each client's GameplayClient (ent+ / ent~ / ent-), which Graphics draws from
// the model ref and animation name. Nothing about them enters a stock message
// or the retail save.
//
// The simulation is deliberately small: an axis-aligned box with gravity and
// voxel collision, steering behaviours (idle, wander, flee, follow, attack,
// path, look) and a bounded A* for path-to-point, stepped at 20 Hz.

#include "internal.hpp"

#include "openminerz/world/block.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <queue>
#include <unordered_map>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr float step_seconds = 0.05F;
constexpr double gravity = -20.0;
constexpr double jump_speed = 7.5;
constexpr double simulate_distance = 128.0;
constexpr double despawn_distance = 96.0;
constexpr std::size_t path_node_limit = 2048U;

[[nodiscard]] double horizontal_distance_squared(Vec3 a, Vec3 b) noexcept
{
    const double dx = a.x - b.x;
    const double dz = a.z - b.z;
    return dx * dx + dz * dz;
}

[[nodiscard]] const char *behaviour_name(BehaviourKind kind) noexcept
{
    switch (kind)
    {
    case BehaviourKind::idle:
        return "idle";
    case BehaviourKind::wander:
        return "wander";
    case BehaviourKind::flee:
        return "flee";
    case BehaviourKind::follow:
        return "follow";
    case BehaviourKind::attack:
        return "attack";
    case BehaviourKind::path:
        return "path";
    case BehaviourKind::look:
        return "look";
    }
    return "idle";
}

struct CellHash final
{
    [[nodiscard]] std::size_t operator()(const BlockPos &cell) const noexcept
    {
        return static_cast<std::size_t>(static_cast<std::uint32_t>(cell.x) * 73856093U ^
                                        static_cast<std::uint32_t>(cell.y) * 19349663U ^
                                        static_cast<std::uint32_t>(cell.z) * 83492791U);
    }
};

} // namespace

bool GameplayService::Impl::solid(int x, int y, int z) const
{
    const auto block = block_at({x, y, z});
    // Outside the world counts as solid below it and open above it.
    if (!block.has_value())
        return y < -64;
    // (mod-liquid) A liquid stops nothing.
    return world::blocks_player(static_cast<world::BlockType>(*block)) &&
           (!liquids_defined || !liquid_cell({x, y, z}));
}

// ------------------------------------------------------------- lifecycle

Entity *GameplayService::Impl::spawn_entity(const CreatureDef &creature, Vec3 position,
                                            const Value &options)
{
    if (!finite(position))
        return nullptr;
    const auto alive = std::ranges::count_if(entities, [](const Entity &entity) { return !entity.removed; });
    if (static_cast<std::size_t>(alive) >= limits.max_entities)
        return nullptr;
    Entity created;
    created.id = next_entity_id++;
    created.creature = creature.id;
    created.position = position;
    created.max_health = static_cast<float>(std::clamp(number_or(options, "health", creature.health), 0.01, 100'000.0));
    created.health = created.max_health;
    created.yaw = static_cast<float>(number_or(options, "yaw", random01() * 6.2831853F));
    created.name = string_or(options, "name", creature.show_name ? creature.name : "");
    created.persistent = bool_or(options, "persistent", creature.brain == Brain::npc);
    created.from_spawn_rule = bool_or(options, "natural", false);
    if (const Value *data = options.field("data"); data != nullptr)
        created.data = *data;
    created.think_timer = random01() * 0.25F;
    entities.push_back(std::move(created));
    Entity &spawned = entities.back();
    if (const Value *behaviour = options.field("behaviour"); behaviour != nullptr && behaviour->type() == ValueType::string)
        set_behaviour(spawned, behaviour->as_string(), options);
    ai_entity_spawned(spawned, creature, options); // mod-ai: its mind, team, pack
    // mod-ai: the players near it learn of it now, the rest as they come
    // within AiLimits::interest_radius (replicate_entities).
    for (PlayerRecord &player : players)
        if (player.present &&
            distance_squared(player.position, spawned.position) <= static_cast<double>(ai.limits.interest_radius) * ai.limits.interest_radius)
        {
            out().to_player(player.gid, entity_spawn_value(spawned), true);
            player.known_entities[spawned.id] = {spawned.position, spawned.yaw, spawned.animation,
                                                 spawned.max_health > 0.0F ? spawned.health / spawned.max_health : 1.0F,
                                                 spawned.name};
        }
    const std::uint64_t id = spawned.id;
    emit(Side::server, "entity.spawned",
         make_map({{"entity", Handle{HandleKind::entity, id}}, {"kind", creature.key},
                   {"position", vec3_value(position)}}));
    // A handler may have spawned more (the vector moved): look it up again.
    return entity(id);
}

Entity *GameplayService::Impl::entity(std::uint64_t id) noexcept
{
    const auto found = std::ranges::find(entities, id, &Entity::id);
    return found == entities.end() || found->removed ? nullptr : &*found;
}

Entity &GameplayService::Impl::entity_arg(const Value &value)
{
    if (value.type() != ValueType::handle || value.as_handle().kind != HandleKind::entity)
        throw ScriptError{"expected an entity handle"};
    Entity *found = entity(value.as_handle().id);
    if (found == nullptr)
        throw ScriptError{"that entity no longer exists"};
    return *found;
}

Handle GameplayService::Impl::entity_handle(const Entity &target) const noexcept
{
    return Handle{HandleKind::entity, target.id};
}

void GameplayService::Impl::remove_entity(Entity &target, bool died)
{
    if (target.removed)
        return;
    target.removed = true;
    // (Visuals) "died" lets clients play the death instead of dropping it.
    // mod-ai: to the clients that knew of it.
    forget_entity_for_all(target.id, died);
    ai_entity_removed(target, died);
    const CreatureDef *creature = server_registry.creature(target.creature);
    if (!died || creature == nullptr)
        return;
    PlayerRecord *killer = target.last_attacker != 0U ? player_by_serial(target.last_attacker) : nullptr;
    const Vec3 at = target.position;
    const std::uint64_t id = target.id;
    emit(Side::server, "entity.died",
         make_map({{"entity", Handle{HandleKind::entity, id}},
                   {"kind", creature->key},
                   {"position", vec3_value(at)},
                   {"killer", killer != nullptr ? Value{handle_of(*killer)} : Value{}}}));
    const std::string table = server_registry.loot.contains(creature->drops)
                                  ? creature->drops
                                  : std::format("{}:{}", creature->mod, creature->drops);
    if (!creature->drops.empty())
    {
        if (server_registry.loot.contains(table))
        {
            for (const auto &[item, amount] : roll_loot(table))
                static_cast<void>(backend.spawn_pickup({at.x, at.y + 0.5, at.z}, {item.item, amount, item.mod_item}));
        }
        else
        {
            try
            {
                const ItemRef item = resolve_item(Value{creature->drops}, creature->mod, "drops");
                static_cast<void>(backend.spawn_pickup({at.x, at.y + 0.5, at.z}, {item.item, 1U, item.mod_item}));
            }
            catch (const ScriptError &error)
            {
                log(creature->mod, LogLevel::warning,
                    std::format("creature '{}': {}", creature->key, error.what()));
            }
        }
    }
    if (killer != nullptr)
        quest_event(*killer, ObjectiveKind::kill, creature->key, 1U);
}

void GameplayService::Impl::damage_entity(Entity &target, float amount, std::uint64_t attacker_player,
                                          std::uint64_t attacker_entity)
{
    if (target.removed || !(amount > 0.0F) || !std::isfinite(amount))
        return;
    const std::uint64_t id = target.id;
    PlayerRecord *attacker = attacker_player != 0U ? player_by_serial(attacker_player) : nullptr;
    const bool allowed = emit(
        Side::server, "entity.damaged",
        make_map({{"entity", Handle{HandleKind::entity, id}},
                  {"amount", amount},
                  {"attacker", attacker != nullptr ? Value{handle_of(*attacker)}
                               : attacker_entity != 0U ? Value{Handle{HandleKind::entity, attacker_entity}}
                                                       : Value{}}}));
    Entity *victim = entity(id);
    if (!allowed || victim == nullptr)
        return;
    victim->health -= amount;
    victim->hurt_timer = 0.4F;
    if (attacker_player != 0U)
        victim->last_attacker = attacker_player;
    if (victim->health <= 0.0F)
    {
        victim->health = 0.0F;
        remove_entity(*victim, true);
        return;
    }
    if (victim->mind.active)
    {
        // mod-ai: an AI creature remembers who hurt it; its brain answers.
        ai_entity_damaged(id, amount, attacker_player, attacker_entity);
        return;
    }
    const CreatureDef *creature = server_registry.creature(victim->creature);
    if (creature == nullptr || victim->behaviour.scripted)
        return;
    EntityTarget source;
    source.player = attacker_player;
    source.entity = attacker_entity;
    if (!source.any())
        return;
    if (creature->brain == Brain::passive || creature->brain == Brain::npc)
        victim->behaviour = {BehaviourKind::flee, source, 16.0F, 5.0F};
    else
        victim->behaviour = {BehaviourKind::attack, source, creature->attack_range, 15.0F};
    victim->path.clear();
}

void GameplayService::Impl::set_behaviour(Entity &target, std::string_view kind, const Value &options)
{
    Behaviour behaviour;
    static constexpr std::array<std::pair<std::string_view, BehaviourKind>, 8> kinds{
        {{"idle", BehaviourKind::idle},
         {"wander", BehaviourKind::wander},
         {"flee", BehaviourKind::flee},
         {"follow", BehaviourKind::follow},
         {"attack", BehaviourKind::attack},
         {"path", BehaviourKind::path},
         {"move_to", BehaviourKind::path},
         {"look", BehaviourKind::look}}};
    const auto found = std::ranges::find(kinds, kind, &std::pair<std::string_view, BehaviourKind>::first);
    if (found == kinds.end())
        throw ScriptError{std::format(
            "unknown behaviour '{}' (idle, wander, flee, follow, attack, path, look, or nil for "
            "the creature's own)",
            kind)};
    behaviour.kind = found->second;
    behaviour.scripted = true;
    behaviour.timer = static_cast<float>(std::clamp(number_or(options, "duration", 0.0), 0.0, 1.0e6));
    behaviour.distance = static_cast<float>(std::clamp(number_or(options, "distance", 2.0), 0.5, 64.0));
    if (const Value *goal = options.field("target"); goal != nullptr && !goal->is_nil())
    {
        if (goal->type() == ValueType::handle)
        {
            const Handle handle = goal->as_handle();
            if (handle.kind == HandleKind::player && player_by_handle(handle) != nullptr)
                behaviour.target.player = handle.id;
            else if (handle.kind == HandleKind::entity && entity(handle.id) != nullptr)
                behaviour.target.entity = handle.id;
            else
                throw ScriptError{"behaviour target: no such player or entity"};
        }
        else
            behaviour.target.position = to_vec3(*goal, "behaviour target");
    }
    const bool needs_target = behaviour.kind == BehaviourKind::flee || behaviour.kind == BehaviourKind::follow ||
                              behaviour.kind == BehaviourKind::attack || behaviour.kind == BehaviourKind::path ||
                              behaviour.kind == BehaviourKind::look;
    if (needs_target && !behaviour.target.any())
        throw ScriptError{std::format("behaviour '{}' needs a target", kind)};
    if (behaviour.kind == BehaviourKind::path && !behaviour.target.position.has_value())
        throw ScriptError{"path needs a position target"};
    target.behaviour = behaviour;
    target.path.clear();
    target.path_index = 0U;
}

// ------------------------------------------------------------- simulation

void GameplayService::Impl::tick_entities(float seconds)
{
    // 20 Hz: the accumulator lives with the entities so a service restart
    // starts it over.
    entity_step_accumulator += seconds;
    if (entity_step_accumulator < step_seconds)
        return;
    const float dt = std::min(entity_step_accumulator, 0.25F);
    entity_step_accumulator = 0.0F;
    ai_begin_step(); // mod-ai: this step's think and planning budgets, the grid

    const auto target_position = [&](const EntityTarget &target) -> std::optional<Vec3> {
        if (target.player != 0U)
        {
            const PlayerRecord *player = player_by_serial(target.player);
            if (player == nullptr || !player->present || !player->alive)
                return std::nullopt;
            return player->position;
        }
        if (target.entity != 0U)
        {
            const Entity *other = entity(target.entity);
            return other != nullptr ? std::optional{other->position} : std::nullopt;
        }
        return target.position;
    };
    const auto collides = [&](Vec3 at, const CreatureDef &creature) {
        const double half = creature.width * 0.5 - 0.01;
        const int x0 = static_cast<int>(std::floor(at.x - half));
        const int x1 = static_cast<int>(std::floor(at.x + half));
        const int y0 = static_cast<int>(std::floor(at.y + 0.01));
        const int y1 = static_cast<int>(std::floor(at.y + creature.height - 0.01));
        const int z0 = static_cast<int>(std::floor(at.z - half));
        const int z1 = static_cast<int>(std::floor(at.z + half));
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    if (solid(x, y, z))
                        return true;
        return false;
    };

    const std::size_t count = entities.size();
    for (std::size_t index{}; index < count; ++index)
    {
        if (index >= entities.size() || entities[index].removed)
            continue;
        const CreatureDef *creature = server_registry.creature(entities[index].creature);
        if (creature == nullptr)
        {
            remove_entity(entities[index], false);
            continue;
        }
        // Nearest present player: entities sleep where nobody is.
        double nearest = std::numeric_limits<double>::max();
        const PlayerRecord *closest{};
        for (const PlayerRecord &player : players)
            if (player.present)
            {
                const double distance = horizontal_distance_squared(player.position, entities[index].position);
                if (distance < nearest)
                {
                    nearest = distance;
                    if (player.alive)
                        closest = &player;
                }
            }
        Entity &body = entities[index];
        if (nearest > despawn_distance * despawn_distance)
        {
            body.far_timer += dt;
            if (body.from_spawn_rule && !body.persistent && body.far_timer > 30.0F)
            {
                remove_entity(body, false);
                continue;
            }
        }
        else
            body.far_timer = 0.0F;
        if (nearest > simulate_distance * simulate_distance)
            continue;
        if (body.mind.active && creature->ai != nullptr)
        {
            tick_ai_entity(index, dt, nearest); // mod-ai: brains, senses, attacks, movement
            continue;
        }

        body.attack_timer = std::max(0.0F, body.attack_timer - dt);
        body.hurt_timer = std::max(0.0F, body.hurt_timer - dt);
        Behaviour &behaviour = body.behaviour;
        if (behaviour.timer > 0.0F)
        {
            behaviour.timer -= dt;
            if (behaviour.timer <= 0.0F)
            {
                behaviour = {};
                body.path.clear();
            }
        }
        // The creature's own brain, when no script is steering it.
        body.think_timer -= dt;
        if (!behaviour.scripted && body.think_timer <= 0.0F)
        {
            body.think_timer = 0.25F;
            switch (creature->brain)
            {
            case Brain::hostile:
                if (behaviour.kind != BehaviourKind::attack && closest != nullptr &&
                    nearest <= static_cast<double>(creature->view_range * creature->view_range))
                {
                    behaviour = {BehaviourKind::attack, {closest->serial, 0U, std::nullopt},
                                 creature->attack_range, 0.0F};
                    break;
                }
                [[fallthrough]];
            case Brain::passive:
            case Brain::neutral:
                if (behaviour.kind == BehaviourKind::idle && random01() < 0.08F)
                {
                    const float angle = random01() * 6.2831853F;
                    const float reach = 3.0F + random01() * 6.0F;
                    behaviour = {BehaviourKind::wander, {}, 1.0F, 12.0F};
                    behaviour.goal = {body.position.x + std::cos(angle) * reach, body.position.y,
                                      body.position.z + std::sin(angle) * reach};
                    behaviour.has_goal = true;
                }
                break;
            case Brain::npc:
                if (closest != nullptr && nearest <= 36.0)
                    behaviour = {BehaviourKind::look, {closest->serial, 0U, std::nullopt}, 2.0F, 0.0F};
                else if (behaviour.kind == BehaviourKind::look)
                    behaviour = {};
                break;
            }
        }

        // Steering: where this behaviour wants to go, and how fast.
        Vec3 desired{};
        bool running{};
        std::optional<Vec3> face;
        const auto steer_toward = [&](Vec3 goal, double speed) {
            const double dx = goal.x - body.position.x;
            const double dz = goal.z - body.position.z;
            const double length = std::sqrt(dx * dx + dz * dz);
            if (length > 1e-6)
                desired = {dx / length * speed, 0.0, dz / length * speed};
            face = goal;
        };
        const std::optional<Vec3> goal = target_position(behaviour.target);
        switch (behaviour.kind)
        {
        case BehaviourKind::idle:
            break;
        case BehaviourKind::wander:
            if (!behaviour.has_goal ||
                horizontal_distance_squared(body.position, behaviour.goal) < 0.5 || body.stuck_timer > 2.0F)
            {
                if (!behaviour.scripted)
                    behaviour = {};
                else
                {
                    const float angle = random01() * 6.2831853F;
                    behaviour.goal = {body.position.x + std::cos(angle) * 6.0, body.position.y,
                                      body.position.z + std::sin(angle) * 6.0};
                    behaviour.has_goal = true;
                }
                body.stuck_timer = 0.0F;
                break;
            }
            steer_toward(behaviour.goal, creature->speed);
            break;
        case BehaviourKind::flee:
            if (!goal.has_value() ||
                horizontal_distance_squared(*goal, body.position) > behaviour.distance * behaviour.distance)
            {
                if (!behaviour.scripted || !goal.has_value())
                    behaviour = {};
                break;
            }
            steer_toward({2.0 * body.position.x - goal->x, body.position.y, 2.0 * body.position.z - goal->z},
                         creature->run_speed);
            running = true;
            break;
        case BehaviourKind::follow:
            if (!goal.has_value())
            {
                behaviour = {};
                break;
            }
            if (horizontal_distance_squared(*goal, body.position) > behaviour.distance * behaviour.distance)
            {
                const bool far = horizontal_distance_squared(*goal, body.position) > 64.0;
                steer_toward(*goal, far ? creature->run_speed : creature->speed);
                running = far;
            }
            else
                face = *goal;
            break;
        case BehaviourKind::attack: {
            if (!goal.has_value() ||
                horizontal_distance_squared(*goal, body.position) >
                    std::pow(creature->view_range * 1.5, 2.0))
            {
                behaviour = {};
                break;
            }
            const double distance = std::sqrt(distance_squared(*goal, body.position));
            if (distance > creature->attack_range * 0.8)
            {
                steer_toward(*goal, creature->run_speed);
                running = true;
            }
            else
                face = *goal;
            if (distance <= creature->attack_range && body.attack_timer <= 0.0F &&
                creature->damage > 0.0F)
            {
                body.attack_timer = creature->attack_cooldown;
                body.animation_override.clear();
                const std::uint64_t attacker = body.id;
                const float damage_amount = creature->damage;
                const Vec3 from = body.position;
                if (behaviour.target.player != 0U)
                {
                    if (PlayerRecord *victim = player_by_serial(behaviour.target.player))
                        damage(*victim, damage_amount, DamageCause::creature, from);
                }
                else if (behaviour.target.entity != 0U)
                {
                    if (Entity *victim = entity(behaviour.target.entity))
                        damage_entity(*victim, damage_amount, 0U, attacker);
                }
                // A handler may have changed the entity list.
                if (index >= entities.size() || entities[index].id != attacker || entities[index].removed)
                    continue;
            }
            break;
        }
        case BehaviourKind::path: {
            Entity &walker = entities[index];
            if (walker.path.empty() && behaviour.target.position.has_value())
            {
                // Bounded A* over cells a body can stand in.
                const BlockPos start = floor_cell(walker.position);
                const BlockPos end = floor_cell(*behaviour.target.position);
                const auto standable = [&](BlockPos cell) {
                    return !solid(cell.x, cell.y, cell.z) && !solid(cell.x, cell.y + 1, cell.z) &&
                           solid(cell.x, cell.y - 1, cell.z);
                };
                struct Node final
                {
                    int cost;
                    int estimate;
                    BlockPos cell;
                    bool operator>(const Node &other) const noexcept
                    {
                        return cost + estimate > other.cost + other.estimate;
                    }
                };
                const auto heuristic = [&](BlockPos cell) {
                    return std::abs(cell.x - end.x) + std::abs(cell.y - end.y) + std::abs(cell.z - end.z);
                };
                std::priority_queue<Node, std::vector<Node>, std::greater<>> open;
                std::unordered_map<BlockPos, BlockPos, CellHash> came_from;
                std::unordered_map<BlockPos, int, CellHash> best;
                open.push({0, heuristic(start), start});
                best[start] = 0;
                std::optional<BlockPos> reached;
                BlockPos closest_cell = start;
                int closest_estimate = heuristic(start);
                while (!open.empty() && best.size() < path_node_limit)
                {
                    const Node node = open.top();
                    open.pop();
                    if (node.cell == end)
                    {
                        reached = end;
                        break;
                    }
                    if (node.estimate < closest_estimate)
                    {
                        closest_estimate = node.estimate;
                        closest_cell = node.cell;
                    }
                    static constexpr std::array<std::array<int, 2>, 4> directions{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
                    for (const auto &direction : directions)
                        for (int dy = 1; dy >= -3; --dy)
                        {
                            const BlockPos next{node.cell.x + direction[0], node.cell.y + dy,
                                                node.cell.z + direction[1]};
                            if (dy == 1 && solid(node.cell.x, node.cell.y + 2, node.cell.z))
                                continue;
                            if (!standable(next))
                                continue;
                            const int cost = node.cost + 1 + (dy != 0 ? 1 : 0);
                            const auto known = best.find(next);
                            if (known != best.end() && known->second <= cost)
                                break;
                            best[next] = cost;
                            came_from[next] = node.cell;
                            open.push({cost, heuristic(next), next});
                            break;
                        }
                }
                BlockPos cursor = reached.value_or(closest_cell);
                std::vector<BlockPos> reversed;
                while (!(cursor == start) && reversed.size() < path_node_limit)
                {
                    reversed.push_back(cursor);
                    const auto parent = came_from.find(cursor);
                    if (parent == came_from.end())
                        break;
                    cursor = parent->second;
                }
                walker.path.assign(reversed.rbegin(), reversed.rend());
                walker.path_index = 0U;
                if (walker.path.empty())
                {
                    const std::uint64_t id = walker.id;
                    behaviour = {};
                    emit(Side::server, "entity.arrived",
                         make_map({{"entity", Handle{HandleKind::entity, id}}, {"reached", reached.has_value()}}));
                    if (index >= entities.size() || entities[index].removed)
                        continue;
                    break;
                }
            }
            Entity &mover = entities[index];
            if (mover.path_index < mover.path.size())
            {
                const BlockPos waypoint = mover.path[mover.path_index];
                const Vec3 centre{waypoint.x + 0.5, static_cast<double>(waypoint.y), waypoint.z + 0.5};
                if (horizontal_distance_squared(mover.position, centre) < 0.09)
                    ++mover.path_index;
                else
                    steer_toward(centre, creature->speed);
            }
            if (mover.path_index >= mover.path.size())
            {
                const std::uint64_t id = mover.id;
                const bool reached =
                    behaviour.target.position.has_value() &&
                    horizontal_distance_squared(mover.position, *behaviour.target.position) < 2.25;
                behaviour = {};
                mover.path.clear();
                emit(Side::server, "entity.arrived",
                     make_map({{"entity", Handle{HandleKind::entity, id}}, {"reached", reached}}));
                if (index >= entities.size() || entities[index].removed)
                    continue;
            }
            break;
        }
        case BehaviourKind::look:
            if (goal.has_value())
                face = *goal;
            else
                behaviour = {};
            break;
        }

        // Physics.
        Entity &mover = entities[index];
        // (mod-liquid) In a liquid a creature floats up and wades out at half
        // speed; on land it never steps into one (the bank stops it like a
        // wall it does not jump).
        const bool wading = wading_in_liquid(mover.position, creature->height);
        mover.velocity.x = desired.x * (wading ? 0.5 : 1.0);
        mover.velocity.z = desired.z * (wading ? 0.5 : 1.0);
        mover.velocity.y = wading ? wading_vertical_speed(mover.velocity.y, dt)
                                  : std::max(mover.velocity.y + gravity * dt, -40.0);
        bool blocked{};
        Vec3 next = mover.position;
        next.x += mover.velocity.x * dt;
        if (collides(next, *creature))
        {
            next.x = mover.position.x;
            blocked = std::abs(mover.velocity.x) > 0.01;
        }
        else if (!wading && steps_into_liquid(next))
            next.x = mover.position.x;
        next.z += mover.velocity.z * dt;
        if (collides(next, *creature))
        {
            next.z = mover.position.z;
            blocked = blocked || std::abs(mover.velocity.z) > 0.01;
        }
        else if (!wading && steps_into_liquid(next))
            next.z = mover.position.z;
        next.y += mover.velocity.y * dt;
        mover.grounded = false;
        if (collides(next, *creature))
        {
            if (mover.velocity.y < 0.0)
            {
                mover.grounded = true;
                next.y = std::floor(next.y) + 1.0;
                if (collides(next, *creature))
                    next.y = mover.position.y;
            }
            else
                next.y = mover.position.y;
            mover.velocity.y = 0.0;
        }
        if (blocked && (mover.grounded || wading))
            mover.velocity.y = jump_speed;
        const double moved = horizontal_distance_squared(next, mover.position);
        mover.stuck_timer = (desired.x != 0.0 || desired.z != 0.0) && moved < 1e-6 ? mover.stuck_timer + dt : 0.0F;
        mover.position = next;
        if (mover.position.y < -80.0)
        {
            remove_entity(mover, false);
            continue;
        }
        if (face.has_value())
        {
            const double dx = face->x - mover.position.x;
            const double dz = face->z - mover.position.z;
            if (dx * dx + dz * dz > 1e-6)
                mover.yaw = static_cast<float>(std::atan2(dx, -dz));
        }
        const bool moving = desired.x != 0.0 || desired.z != 0.0;
        mover.animation = !mover.animation_override.empty() ? mover.animation_override
                          : mover.hurt_timer > 0.0F        ? "hurt"
                          : mover.attack_timer > creature->attack_cooldown - 0.4F && mover.attack_timer > 0.0F
                              ? "attack"
                          : moving ? (running ? "run" : "walk")
                                   : "idle";
    }
    std::erase_if(entities, [](const Entity &entity) { return entity.removed; });
}

// ----------------------------------------------------------- spawn rules

void GameplayService::Impl::spawn_rules()
{
    const double day = backend.day();
    const double time_of_day = day - std::floor(day);
    const bool night = time_of_day < 0.25 || time_of_day >= 0.875;
    for (const CreatureDef &creature : server_registry.creatures)
    {
        const SpawnRule &rule = creature.spawn;
        if (!rule.enabled || rule.chance <= 0.0F || rule.max_near_player <= 0)
            continue;
        if ((rule.time == SpawnTime::day && night) || (rule.time == SpawnTime::night && !night))
            continue;
        for (const PlayerRecord &player : players)
        {
            if (!player.present || !player.alive || random01() >= rule.chance)
                continue;
            const auto nearby = std::ranges::count_if(entities, [&](const Entity &entity) {
                return !entity.removed && entity.creature == creature.id &&
                       horizontal_distance_squared(entity.position, player.position) < 64.0 * 64.0;
            });
            if (nearby >= rule.max_near_player)
                continue;
            const float angle = random01() * 6.2831853F;
            const float distance = 24.0F + random01() * 24.0F;
            const int x = static_cast<int>(std::floor(player.position.x + std::cos(angle) * distance));
            const int z = static_cast<int>(std::floor(player.position.z + std::sin(angle) * distance));
            // Anywhere the authority can resolve, not only where this
            // process holds terrain: a guest far from the host gets its
            // creatures too. (Light is only known where terrain is loaded.)
            if (!rule.biomes.empty() &&
                std::ranges::find(rule.biomes, backend.biome(x, z)) == rule.biomes.end())
                continue;
            std::optional<int> y;
            if (rule.surface)
            {
                const auto top = surface_y(x, z);
                // (mod-liquid) Nothing spawns on the surface of a lake.
                if (top.has_value() && !liquid_cell({x, *top, z}))
                    y = *top + 1;
            }
            else
            {
                // Underground: an open, floored cell between the bounds.
                for (int attempt = 0; attempt < 8 && !y.has_value(); ++attempt)
                {
                    const int candidate = random_int(rule.y_min, rule.y_max);
                    if (!solid(x, candidate, z) && !solid(x, candidate + 1, z) && solid(x, candidate - 1, z))
                        y = candidate;
                }
            }
            if (!y.has_value() || *y < rule.y_min || *y > rule.y_max || solid(x, *y, z) || solid(x, *y + 1, z))
                continue;
            // (mod-liquid) Only swimmers start in a liquid.
            const bool swimmer = creature.ai != nullptr && (creature.ai->movement == MoveMode::swim ||
                                                            creature.ai->movement == MoveMode::amphibious);
            if (!swimmer && liquid_cell({x, *y, z}))
                continue;
            const auto light = backend.light({x, *y, z});
            if (light.has_value())
            {
                const float level = std::max((*light)[0], (*light)[1]);
                if (level < rule.light_min || level > rule.light_max)
                    continue;
            }
            const Vec3 at{x + 0.5, static_cast<double>(*y), z + 0.5};
            if (!emit(Side::server, "entity.spawning",
                      make_map({{"kind", creature.key}, {"position", vec3_value(at)},
                                {"player", handle_of(player)}})))
                continue;
            const int group = random_int(rule.group_min, std::max(rule.group_min, rule.group_max));
            std::uint64_t first{}; // mod-ai: a group of AI creatures is a pack
            for (int member = 0; member < group; ++member)
            {
                const Vec3 spot{at.x + (member == 0 ? 0.0 : random01() * 3.0 - 1.5), at.y,
                                at.z + (member == 0 ? 0.0 : random01() * 3.0 - 1.5)};
                if (rule.surface && member > 0 && steps_into_liquid(spot)) // (mod-liquid)
                    continue;
                const Entity *spawned = spawn_entity(
                    creature, spot,
                    first != 0U ? make_map({{"natural", true}, {"pack", Handle{HandleKind::entity, first}}})
                                : make_map({{"natural", true}}));
                if (spawned != nullptr && first == 0U)
                    first = spawned->id;
            }
        }
    }
}

// ----------------------------------------------------------- replication

Value GameplayService::Impl::entity_spawn_value(const Entity &target) const
{
    const CreatureDef *creature = server_registry.creature(target.creature);
    return make_map({{"t", "ent+"},
                     {"id", static_cast<std::int64_t>(target.id)},
                     {"kind", creature != nullptr ? creature->key : std::string{}},
                     {"model", creature != nullptr ? creature->model : std::string{}},
                     {"texture", creature != nullptr ? creature->texture : std::string{}},
                     {"a", target.animation},
                     {"name", target.name},
                     {"w", creature != nullptr ? creature->width : 0.6F},
                     {"h", creature != nullptr ? creature->height : 1.8F},
                     {"interact", creature != nullptr && creature->interactable},
                     {"hp", target.max_health > 0.0F ? target.health / target.max_health : 1.0F},
                     {"p", vec3_value(target.position)},
                     {"y", target.yaw}});
}

// mod-ai: interest management. Each player's client hears only of the
// entities within AiLimits::interest_radius of its player (ent+ as one comes
// in range, ent- once it is 8 blocks past it), and of their changes at a rate
// that falls with distance: every pass (10 Hz) within 48 blocks, every second
// pass within 80, every fourth beyond. An entity whose attack just started
// (EntityMind::urgent) goes out at once, so its clip starts with the windup.
// Positions travel as 32-bit floats, what the renderer draws anyway.
void GameplayService::Impl::replicate_entities(bool force)
{
    bool urgent{};
    for (const Entity &target : entities)
        urgent = urgent || (!target.removed && target.mind.urgent);
    if (!force && !replicate_due && !urgent)
        return;
    if (replicate_due || force)
        ++ai.replicate_pass;
    const double radius = ai.limits.interest_radius;
    const double leave = radius + 8.0;
    for (PlayerRecord &player : players)
    {
        if (!player.present)
            continue;
        // Packed: per entity its id, what follows, the position in 1/64
        // blocks and the yaw in 1/256 turns (ai.cpp reads it back).
        std::string packed;
        const auto varint = [&packed](std::uint64_t value) {
            while (value >= 0x80U)
            {
                packed.push_back(static_cast<char>((value & 0x7FU) | 0x80U));
                value >>= 7U;
            }
            packed.push_back(static_cast<char>(value));
        };
        const auto signed_varint = [&varint](double value) {
            const auto fixed = static_cast<std::int64_t>(std::llround(std::clamp(value, -4.0e6, 4.0e6) * 64.0));
            varint((static_cast<std::uint64_t>(fixed) << 1U) ^ static_cast<std::uint64_t>(fixed >> 63));
        };
        const auto text = [&](std::string_view value) {
            const std::string_view clipped = value.substr(0U, 255U);
            varint(clipped.size());
            packed.append(clipped);
        };
        const auto flush = [&] {
            if (packed.empty())
                return;
            out().to_player(player.gid, make_map({{"t", "ent~"}, {"pk", Value{std::move(packed)}}}), false);
            packed = {};
        };
        for (const Entity &target : entities)
        {
            if (target.removed)
                continue;
            const double dx = target.position.x - player.position.x;
            const double dz = target.position.z - player.position.z;
            const double reach = dx * dx + dz * dz;
            const float health = target.max_health > 0.0F ? target.health / target.max_health : 1.0F;
            const auto known = player.known_entities.find(target.id);
            if (known == player.known_entities.end())
            {
                if (reach > radius * radius || (!replicate_due && !force))
                    continue;
                out().to_player(player.gid, entity_spawn_value(target), true);
                player.known_entities.emplace(
                    target.id, PlayerRecord::SentEntity{target.position, target.yaw, target.animation, health, target.name});
                continue;
            }
            if (reach > leave * leave)
            {
                out().to_player(player.gid, make_map({{"t", "ent-"}, {"id", static_cast<std::int64_t>(target.id)}}), true);
                player.known_entities.erase(known);
                continue;
            }
            if (!force && !target.mind.urgent)
            {
                if (!replicate_due)
                    continue;
                const std::uint32_t every = reach < 48.0 * 48.0 ? 1U : reach < 80.0 * 80.0 ? 2U : 4U;
                if ((ai.replicate_pass + static_cast<std::uint32_t>(target.id)) % every != 0U)
                    continue;
            }
            PlayerRecord::SentEntity &sent = known->second;
            const bool moved = distance_squared(target.position, sent.position) > 0.0025 ||
                               std::abs(target.yaw - sent.yaw) > 0.03F;
            const bool animated = target.animation != sent.animation;
            const bool hurt = std::abs(health - sent.health) > 0.001F;
            const bool renamed = target.name != sent.name;
            if (!moved && !animated && !hurt && !renamed && !force)
                continue;
            varint(target.id);
            const auto flags = static_cast<std::uint8_t>((animated || force ? 1U : 0U) | (hurt || force ? 2U : 0U) |
                                                         (renamed || force ? 4U : 0U));
            packed.push_back(static_cast<char>(flags));
            signed_varint(target.position.x);
            signed_varint(target.position.y);
            signed_varint(target.position.z);
            const double turn = std::fmod(static_cast<double>(target.yaw) / 6.283185307179586 + 1.0, 1.0);
            packed.push_back(static_cast<char>(static_cast<std::uint8_t>(std::lround(turn * 256.0) & 0xFF)));
            if ((flags & 1U) != 0U)
                text(target.animation);
            if ((flags & 2U) != 0U)
                packed.push_back(static_cast<char>(static_cast<std::uint8_t>(std::lround(std::clamp(health, 0.0F, 1.0F) * 255.0F))));
            if ((flags & 4U) != 0U)
                text(target.name);
            sent = {target.position, target.yaw, target.animation, health, target.name};
            if (packed.size() >= 1800U)
                flush();
        }
        flush();
    }
    for (Entity &target : entities)
        target.mind.urgent = false;
}

void GameplayService::Impl::forget_entity_for_all(std::uint64_t id, bool died)
{
    for (PlayerRecord &player : players)
        if (player.known_entities.erase(id) != 0U && player.present)
            out().to_player(player.gid,
                            make_map({{"t", "ent-"}, {"id", static_cast<std::int64_t>(id)}, {"died", died}}), true);
}

Value GameplayService::Impl::entity_info(const Entity &target) const
{
    const CreatureDef *creature = server_registry.creature(target.creature);
    return make_map({{"id", static_cast<std::int64_t>(target.id)},
                     {"kind", creature != nullptr ? creature->key : std::string{}},
                     {"position", vec3_value(target.position)},
                     {"yaw", target.yaw},
                     {"health", target.health},
                     {"max_health", target.max_health},
                     {"name", target.name},
                     {"behaviour", behaviour_name(target.behaviour.kind)},
                     {"animation", target.animation},
                     {"data", target.data}});
}

bool GameplayService::Impl::interact(PlayerRecord &player, Entity &target)
{
    const CreatureDef *creature = server_registry.creature(target.creature);
    if (creature == nullptr || !creature->interactable)
        return false;
    const std::uint64_t id = target.id;
    const std::string dialogue = creature->dialogue;
    const bool allowed = emit(Side::server, "entity.interacted",
                              make_map({{"entity", Handle{HandleKind::entity, id}},
                                        {"kind", creature->key},
                                        {"player", handle_of(player)}}));
    quest_event(player, ObjectiveKind::talk, creature->key, 1U);
    if (Entity *npc = entity(id))
        if (!npc->name.empty())
            quest_event(player, ObjectiveKind::talk, npc->name, 1U);
    if (allowed && !dialogue.empty() && !player.dialogue.has_value())
        static_cast<void>(open_dialogue(player, dialogue, {}, id));
    return true;
}

std::optional<std::pair<Entity *, float>> GameplayService::Impl::trace_entities(Vec3 origin,
                                                                                Vec3 direction,
                                                                                float reach,
                                                                                bool interactable_only)
{
    const double length = std::sqrt(distance_squared(direction, {}));
    if (!(length > 1e-9))
        return std::nullopt;
    const std::array<double, 3> d{direction.x / length, direction.y / length, direction.z / length};
    const std::array<double, 3> o{origin.x, origin.y, origin.z};
    std::optional<std::pair<Entity *, float>> best;
    for (Entity &target : entities)
    {
        if (target.removed)
            continue;
        const CreatureDef *creature = server_registry.creature(target.creature);
        if (creature == nullptr || (interactable_only && !creature->interactable))
            continue;
        const double half = creature->width * 0.5;
        const std::array<double, 3> lo{target.position.x - half, target.position.y, target.position.z - half};
        const std::array<double, 3> hi{target.position.x + half, target.position.y + creature->height,
                                       target.position.z + half};
        double near = 0.0;
        double far = reach;
        bool missed{};
        for (std::size_t axis = 0U; axis < 3U && !missed; ++axis)
        {
            if (std::abs(d[axis]) < 1e-9)
            {
                missed = o[axis] < lo[axis] || o[axis] > hi[axis];
                continue;
            }
            double t0 = (lo[axis] - o[axis]) / d[axis];
            double t1 = (hi[axis] - o[axis]) / d[axis];
            if (t0 > t1)
                std::swap(t0, t1);
            near = std::max(near, t0);
            far = std::min(far, t1);
            missed = near > far;
        }
        if (!missed && (!best.has_value() || near < best->second))
            best = std::pair{&target, static_cast<float>(near)};
    }
    return best;
}

} // namespace openminerz::modding::gameplay
