// (mod-ai) How AI creatures move: walking with jump and fall limits (and
// climbing), swimming, flying, and the planning behind them.
//
// Planning is shared and budgeted. Chasing a player or creature reads a
// breadth-first distance field grown from the target's cell, one per target
// and body shape, so a pack of ten hunting one player plans once; walking to
// a point is a bounded A* whose routes are cached for a few seconds by start
// and goal. Both stop at AiLimits::path_nodes_per_step expansions per 20 Hz
// step across every creature; a creature whose plan has to wait steers
// straight at its goal meanwhile. Water costs walkers extra; swimmers plan
// through it. Block edits drop the plans that ran through the changed cell.
// Flyers steer in 3D (cruising `height` above the ground, climbing over what
// is in front of them) and swimmers move in 3D inside water.

#include "internal.hpp"

#include "openminerz/world/block.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <numbers>
#include <queue>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr double gravity = -20.0;

[[nodiscard]] double flat_squared(Vec3 a, Vec3 b) noexcept
{
    const double dx = a.x - b.x;
    const double dz = a.z - b.z;
    return dx * dx + dz * dz;
}

[[nodiscard]] int body_cells(const CreatureDef &creature) noexcept
{
    return std::max(1, static_cast<int>(std::ceil(creature.height - 0.05F)));
}

[[nodiscard]] bool swims(const CreatureAi &ai) noexcept
{
    return ai.movement == MoveMode::swim || ai.movement == MoveMode::amphibious;
}

[[nodiscard]] std::string profile_of(const CreatureDef &creature)
{
    const CreatureAi &ai = *creature.ai;
    return std::format("{}:{}:{}:{}:{}", ai.jump, ai.fall, body_cells(creature), static_cast<int>(ai.movement),
                       ai.climb ? 1 : 0);
}

constexpr std::array<std::array<int, 2>, 4> directions{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};

} // namespace

bool GameplayService::Impl::ai_solid(int x, int y, int z)
{
    const std::int64_t key = cell_key(x, y, z);
    const auto found = ai.solid_cache.find(key);
    if (found != ai.solid_cache.end())
        return found->second;
    const bool value = solid(x, y, z);
    if (ai.solid_cache.size() < 262'144U)
        ai.solid_cache.emplace(key, value);
    return value;
}

bool GameplayService::Impl::water(int x, int y, int z)
{
    // (mod-liquid) A liquid mod block is water wherever it lies.
    if (liquids_defined && liquid_cell({x, y, z}))
        return true;
    if (static_cast<double>(y) + 0.5 > ai.sea_level || ai.water_biomes.empty())
        return false;
    const std::int64_t column = cell_key(x, 0, z);
    auto wet = ai.water_columns.find(column);
    if (wet == ai.water_columns.end())
    {
        bool any = std::ranges::find(ai.water_biomes, "*") != ai.water_biomes.end();
        if (!any)
        {
            const std::string biome = biome_name(x, z);
            any = std::ranges::find(ai.water_biomes, biome) != ai.water_biomes.end();
        }
        wet = ai.water_columns.emplace(column, any).first;
    }
    return wet->second && !ai_solid(x, y, z);
}

bool GameplayService::Impl::standable(const CreatureDef &creature, BlockPos cell)
{
    const int height = body_cells(creature);
    for (int k = 0; k < height; ++k)
        if (ai_solid(cell.x, cell.y + k, cell.z))
            return false;
    const bool swimmer = creature.ai != nullptr && swims(*creature.ai);
    // (mod-liquid) Walkers keep out of liquids: a lake bed is no path.
    if (!swimmer && liquids_defined && liquid_cell(cell))
        return false;
    if (ai_solid(cell.x, cell.y - 1, cell.z))
        return true;
    return swimmer && water(cell.x, cell.y, cell.z);
}

// Whether a body standing in `from` can step to the neighbouring `to`.
namespace
{
template <typename Solid>
[[nodiscard]] bool step_ok(BlockPos from, BlockPos to, int height, int jump, int fall, Solid &&is_solid)
{
    const int dy = to.y - from.y;
    if (dy > jump || dy < -fall)
        return false;
    if (dy > 0)
    {
        for (int k = 0; k < dy; ++k)
            if (is_solid(from.x, from.y + height + k, from.z))
                return false;
    }
    else if (dy < 0)
    {
        for (int y = to.y + height; y < from.y + height; ++y)
            if (is_solid(to.x, y, to.z))
                return false;
    }
    return true;
}
} // namespace

std::optional<BlockPos> GameplayService::Impl::next_step_toward(Entity &body, const CreatureDef &creature,
                                                                BlockPos goal, bool follow_field,
                                                                std::uint64_t target_key)
{
    const CreatureAi &brain_def = *creature.ai;
    const int height = body_cells(creature);
    const BlockPos start = floor_cell(body.position);
    const auto solid_at = [this](int x, int y, int z) { return ai_solid(x, y, z); };
    const auto cost_of = [&](BlockPos cell) {
        return !swims(brain_def) && water(cell.x, cell.y, cell.z) ? 4 : 1;
    };
    const std::string profile = profile_of(creature);

    if (follow_field)
    {
        // The target's field: grown again when the target moved two cells or
        // it is over a second and a half old.
        const std::string key = std::format("{}|{}", profile, target_key);
        auto field = ai.fields.find(key);
        const bool stale = field == ai.fields.end() || server_time - field->second.made_at > 1.5 ||
                           std::abs(field->second.origin.x - goal.x) + std::abs(field->second.origin.z - goal.z) >= 2 ||
                           std::abs(field->second.origin.y - goal.y) >= 2;
        if (stale && ai.nodes_left > 0U)
        {
            FlowField built;
            built.origin = goal;
            built.made_at = server_time;
            // Grow from the cell the target stands in (or the nearest one a
            // body could stand in just below it: a jumping player).
            BlockPos origin = goal;
            for (int drop = 0; drop < 4 && !standable(creature, origin); ++drop)
                --origin.y;
            std::deque<BlockPos> open;
            built.cost.emplace(cell_key(origin.x, origin.y, origin.z), 0U);
            open.push_back(origin);
            std::size_t expanded{};
            while (!open.empty() && expanded < ai.limits.field_node_limit && ai.nodes_left > 0U)
            {
                const BlockPos cell = open.front();
                open.pop_front();
                ++expanded;
                --ai.nodes_left;
                const std::uint16_t here = built.cost[cell_key(cell.x, cell.y, cell.z)];
                for (const auto &direction : directions)
                    for (int dy = -brain_def.jump; dy <= brain_def.fall; ++dy)
                    {
                        // A neighbour that can step into `cell`.
                        const BlockPos from{cell.x + direction[0], cell.y + dy, cell.z + direction[1]};
                        const std::int64_t from_key = cell_key(from.x, from.y, from.z);
                        if (built.cost.contains(from_key) || !standable(creature, from) ||
                            !step_ok(from, cell, height, brain_def.jump, brain_def.fall, solid_at))
                            continue;
                        built.cost.emplace(from_key, static_cast<std::uint16_t>(std::min(65000, here + cost_of(from))));
                        open.push_back(from);
                    }
            }
            ai.stats.path_nodes += expanded;
            ++ai.stats.field_builds;
            field = ai.fields.insert_or_assign(key, std::move(built)).first;
        }
        else if (!stale)
            ++ai.stats.cache_hits;
        if (field != ai.fields.end())
        {
            const auto here = field->second.cost.find(cell_key(start.x, start.y, start.z));
            if (here != field->second.cost.end())
            {
                std::optional<BlockPos> best;
                std::uint16_t best_cost = here->second;
                for (const auto &direction : directions)
                    for (int dy = brain_def.jump; dy >= -brain_def.fall; --dy)
                    {
                        const BlockPos next{start.x + direction[0], start.y + dy, start.z + direction[1]};
                        const auto cost = field->second.cost.find(cell_key(next.x, next.y, next.z));
                        if (cost == field->second.cost.end() || cost->second >= best_cost)
                            continue;
                        if (!step_ok(start, next, height, brain_def.jump, brain_def.fall, solid_at))
                            continue;
                        best = next;
                        best_cost = cost->second;
                    }
                if (best.has_value())
                    return best;
                if (here->second == 0U)
                    return std::nullopt; // there
            }
        }
    }

    // A route to a point: the one it has, a cached one, or a new search.
    EntityMind &mind = body.mind;
    const bool route_current = !mind.route.empty() && mind.route_index < mind.route.size() &&
                               std::abs(mind.route_goal.x - goal.x) + std::abs(mind.route_goal.z - goal.z) <= 1 &&
                               server_time - mind.route_at < 4.0;
    if (!route_current)
    {
        mind.route.clear();
        mind.route_index = 0U;
        const std::string key = std::format("{}|{},{},{}|{},{},{}", profile, start.x, start.y, start.z, goal.x, goal.y, goal.z);
        if (const auto cached = ai.routes.find(key); cached != ai.routes.end())
        {
            ++ai.stats.cache_hits;
            mind.route = cached->second.route;
        }
        else if (ai.nodes_left > 0U)
        {
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
                return std::abs(cell.x - goal.x) + std::abs(cell.y - goal.y) + std::abs(cell.z - goal.z);
            };
            std::priority_queue<Node, std::vector<Node>, std::greater<>> open;
            std::unordered_map<std::int64_t, std::pair<BlockPos, int>> seen; // cell -> (parent, cost)
            open.push({0, heuristic(start), start});
            seen.emplace(cell_key(start.x, start.y, start.z), std::pair{start, 0});
            std::optional<BlockPos> reached;
            BlockPos closest = start;
            int closest_estimate = heuristic(start);
            std::size_t expanded{};
            const std::size_t limit = std::min(ai.limits.path_node_limit, ai.nodes_left);
            while (!open.empty() && expanded < limit)
            {
                const Node node = open.top();
                open.pop();
                ++expanded;
                if (node.cell == goal)
                {
                    reached = goal;
                    break;
                }
                if (node.estimate < closest_estimate)
                {
                    closest_estimate = node.estimate;
                    closest = node.cell;
                }
                for (const auto &direction : directions)
                    for (int dy = brain_def.jump; dy >= -brain_def.fall; --dy)
                    {
                        const BlockPos next{node.cell.x + direction[0], node.cell.y + dy, node.cell.z + direction[1]};
                        if (!standable(creature, next) ||
                            !step_ok(node.cell, next, height, brain_def.jump, brain_def.fall, solid_at))
                            continue;
                        const int cost = node.cost + cost_of(next) + (dy != 0 ? 1 : 0);
                        const std::int64_t key_next = cell_key(next.x, next.y, next.z);
                        const auto known = seen.find(key_next);
                        if (known != seen.end() && known->second.second <= cost)
                            break;
                        seen.insert_or_assign(key_next, std::pair{node.cell, cost});
                        open.push({cost, heuristic(next), next});
                        break; // the highest cell it can stand in on this side
                    }
            }
            ai.nodes_left -= std::min(ai.nodes_left, expanded);
            ai.stats.path_nodes += expanded;
            ++ai.stats.path_searches;
            BlockPos cursor = reached.value_or(closest);
            std::vector<BlockPos> reversed;
            while (!(cursor == start) && reversed.size() < ai.limits.path_node_limit)
            {
                reversed.push_back(cursor);
                const auto parent = seen.find(cell_key(cursor.x, cursor.y, cursor.z));
                if (parent == seen.end())
                    break;
                cursor = parent->second.first;
            }
            mind.route.assign(reversed.rbegin(), reversed.rend());
            if (ai.routes.size() < 4096U)
                ai.routes.insert_or_assign(key, RouteCacheEntry{mind.route, server_time, reached.has_value()});
        }
        else
            return std::nullopt; // over budget: straight at it this step
        mind.route_goal = goal;
        mind.route_at = server_time;
    }
    // Waypoints already behind it are skipped.
    while (mind.route_index < mind.route.size())
    {
        const BlockPos &waypoint = mind.route[mind.route_index];
        const Vec3 centre{waypoint.x + 0.5, static_cast<double>(waypoint.y), waypoint.z + 0.5};
        if (flat_squared(body.position, centre) < 0.12 && std::abs(body.position.y - centre.y) < 1.2)
            ++mind.route_index;
        else
            return waypoint;
    }
    return std::nullopt;
}

std::optional<Vec3> GameplayService::Impl::surround_slot(const Entity &body, Vec3 around, float radius)
{
    if (body.mind.pack == 0U)
        return std::nullopt;
    const auto found = ai.packs.find(body.mind.pack);
    if (found == ai.packs.end() || found->second.members.size() < 2U)
        return std::nullopt;
    std::vector<std::uint64_t> members = found->second.members;
    std::ranges::sort(members);
    const auto self = std::ranges::find(members, body.id);
    if (self == members.end())
        return std::nullopt;
    const auto slot = static_cast<double>(self - members.begin());
    const Entity *leader = entity(found->second.leader);
    const Vec3 from = leader != nullptr ? leader->position : body.position;
    const double base = std::atan2(from.z - around.z, from.x - around.x);
    const double angle = base + slot * 2.0 * std::numbers::pi / static_cast<double>(members.size());
    return Vec3{around.x + std::cos(angle) * radius, around.y, around.z + std::sin(angle) * radius};
}

void GameplayService::Impl::move_ai_entity(std::size_t index, float dt)
{
    Entity &body = entities[index];
    const CreatureDef *creature_pointer = server_registry.creature(body.creature);
    if (creature_pointer == nullptr || creature_pointer->ai == nullptr)
        return;
    const CreatureDef &creature = *creature_pointer;
    const CreatureAi &brain_def = *creature.ai;
    EntityMind &mind = body.mind;
    // Far away, fewer and longer steps.
    mind.move_accumulator += dt;
    const std::uint32_t every = std::max(1U, mind.lod / 2U);
    if (every > 1U && (ai.pass + static_cast<std::uint32_t>(body.id)) % every != 0U)
        return;
    const float step = std::min(mind.move_accumulator, 0.25F);
    mind.move_accumulator = 0.0F;

    // A script's entities.set_behaviour steers instead of the brain.
    MoveGoal goal = mind.goal;
    std::optional<Vec3> face;
    Behaviour &behaviour = body.behaviour;
    bool arrived_event{};
    if (behaviour.scripted)
    {
        if (behaviour.timer > 0.0F)
        {
            behaviour.timer -= step;
            if (behaviour.timer <= 0.0F)
                behaviour = {};
        }
        const std::uint64_t player = behaviour.target.player;
        const std::uint64_t other = behaviour.target.entity;
        switch (behaviour.kind)
        {
        case BehaviourKind::idle:
            goal = {};
            break;
        case BehaviourKind::wander:
            if (!behaviour.has_goal || flat_squared(body.position, behaviour.goal) < 0.5)
            {
                const float angle = random01() * 6.2831853F;
                behaviour.goal = {body.position.x + std::cos(angle) * 6.0, body.position.y,
                                  body.position.z + std::sin(angle) * 6.0};
                behaviour.has_goal = true;
            }
            goal = {GoalKind::point};
            goal.point = behaviour.goal;
            goal.run = false;
            break;
        case BehaviourKind::flee:
            goal = {GoalKind::flee, player, other, behaviour.target.position.value_or(Vec3{}), behaviour.distance};
            break;
        case BehaviourKind::follow:
            goal = {GoalKind::chase, player, other, behaviour.target.position.value_or(Vec3{}), behaviour.distance};
            break;
        case BehaviourKind::attack:
            goal = {GoalKind::chase, player, other, {}, 1.0F};
            if (mind.attack < 0)
                for (const AttackDef &attack : brain_def.attacks)
                    if (start_attack(index, attack.name, player, other, false))
                        break;
            break;
        case BehaviourKind::path:
            goal = {GoalKind::point};
            goal.point = behaviour.target.position.value_or(body.position);
            goal.run = false;
            arrived_event = flat_squared(body.position, goal.point) < 1.0 &&
                            std::abs(body.position.y - goal.point.y) < 1.5;
            break;
        case BehaviourKind::look:
            goal = {};
            face = target_point(player, other);
            if (!face.has_value())
                face = behaviour.target.position;
            break;
        }
    }
    Entity &mover = entities[index];
    if (arrived_event)
    {
        const std::uint64_t id = mover.id;
        mover.behaviour = {};
        mover.mind.route.clear();
        emit(Side::server, "entity.arrived", make_map({{"entity", Handle{HandleKind::entity, id}}, {"reached", true}}));
        return;
    }

    const bool rooted = mind.attack >= 0 && !mind.attack_is_ability;
    const float speed_scale = mind.speed_scale;
    const float walk = creature.speed * speed_scale;
    const float run = creature.run_speed * speed_scale;
    const float swim = (brain_def.swim_speed >= 0.0F ? brain_def.swim_speed : creature.run_speed) * speed_scale;
    float speed = goal.run ? run : walk;
    // Where this goal wants to be.
    std::optional<Vec3> destination;
    const std::optional<Vec3> target = target_point(goal.player, goal.entity);
    switch (goal.kind)
    {
    case GoalKind::none:
        break;
    case GoalKind::point:
    case GoalKind::home:
    case GoalKind::wander:
        destination = goal.point;
        break;
    case GoalKind::chase:
        if (target.has_value())
        {
            face = *target;
            if (flat_squared(*target, mover.position) > static_cast<double>(goal.distance) * goal.distance ||
                std::abs(target->y - mover.position.y) > 1.6)
                destination = *target;
        }
        else if (goal.player == 0U && goal.entity == 0U)
            destination = goal.point;
        break;
    case GoalKind::keep:
        if (target.has_value())
        {
            face = *target;
            const double distance = std::sqrt(flat_squared(*target, mover.position));
            if (distance > goal.distance2)
                destination = *target;
            else if (distance < goal.distance && distance > 1e-3)
            {
                const double away = goal.distance + 1.0 - distance;
                destination = Vec3{mover.position.x + (mover.position.x - target->x) / distance * away, mover.position.y,
                                   mover.position.z + (mover.position.z - target->z) / distance * away};
            }
        }
        break;
    case GoalKind::flee: {
        const Vec3 from = target.value_or(goal.point);
        const double distance = std::sqrt(flat_squared(from, mover.position));
        if (distance < 32.0)
        {
            const double dx = distance > 1e-3 ? (mover.position.x - from.x) / distance : 1.0;
            const double dz = distance > 1e-3 ? (mover.position.z - from.z) / distance : 0.0;
            destination = Vec3{mover.position.x + dx * 6.0, mover.position.y, mover.position.z + dz * 6.0};
            speed = run;
        }
        break;
    }
    case GoalKind::surround:
        if (target.has_value())
        {
            face = *target;
            const std::optional<Vec3> slot = surround_slot(mover, *target, goal.distance);
            const Vec3 spot = slot.value_or(*target);
            if (flat_squared(spot, mover.position) > 0.36 || std::abs(target->y - mover.position.y) > 1.6)
                destination = spot;
        }
        break;
    }
    if (rooted)
        destination.reset();

    // The velocity it wants.
    Vec3 desired{};
    const Vec3 centre{mover.position.x, mover.position.y + creature.height * 0.5, mover.position.z};
    const BlockPos centre_cell = floor_cell(centre);
    const bool in_water = swims(brain_def) && water(centre_cell.x, centre_cell.y, centre_cell.z);
    const auto toward = [&](Vec3 point, double pace, bool vertical) {
        const double dx = point.x - mover.position.x;
        const double dy = vertical ? point.y - mover.position.y : 0.0;
        const double dz = point.z - mover.position.z;
        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length > 1e-4)
            desired = {dx / length * pace, dy / length * pace, dz / length * pace};
    };
    if (destination.has_value())
    {
        switch (brain_def.movement)
        {
        case MoveMode::fly: {
            Vec3 aim = *destination;
            // Cruise over the ground, come down to what it hunts.
            if (flat_squared(aim, mover.position) > 36.0)
            {
                int ground = static_cast<int>(std::floor(mover.position.y));
                for (int depth = 0; depth < 32 && !ai_solid(centre_cell.x, ground - 1, centre_cell.z); ++depth)
                    --ground;
                aim.y = std::max(aim.y + 1.0, static_cast<double>(ground) + brain_def.fly_height);
            }
            else
                aim.y += 0.8;
            toward(aim, speed, true);
            // Something ahead: climb over it.
            const double length = std::sqrt(desired.x * desired.x + desired.z * desired.z);
            if (length > 1e-4)
            {
                const int probe_x = static_cast<int>(std::floor(centre.x + desired.x / length * 1.2));
                const int probe_z = static_cast<int>(std::floor(centre.z + desired.z / length * 1.2));
                if (ai_solid(probe_x, centre_cell.y, probe_z) || ai_solid(probe_x, centre_cell.y + 1, probe_z))
                    desired.y = std::max(desired.y, static_cast<double>(speed));
            }
            break;
        }
        case MoveMode::swim:
            if (in_water)
            {
                Vec3 aim = *destination;
                // (mod-liquid) Under a liquid's own surface; under the sea
                // level in the sea.
                const double depth = liquid_depth_at(centre);
                const double surface = depth > 0.0 ? centre.y + depth : ai.sea_level;
                aim.y = std::min(aim.y, surface - creature.height * 0.6);
                toward(aim, swim, true);
            }
            break;
        case MoveMode::amphibious:
            if (in_water)
            {
                toward(*destination, swim, true);
                break;
            }
            [[fallthrough]];
        case MoveMode::ground: {
            Vec3 aim = *destination;
            const BlockPos goal_cell = floor_cell(*destination);
            // Close and nothing in the way at the body's height: straight at it.
            const auto open_line = [&] {
                const double dx = destination->x - mover.position.x;
                const double dz = destination->z - mover.position.z;
                const double length = std::sqrt(dx * dx + dz * dz);
                const int samples = static_cast<int>(std::ceil(length / 0.5));
                const int height = body_cells(creature);
                for (int sample = 1; sample <= samples; ++sample)
                {
                    const double t = static_cast<double>(sample) / samples;
                    const int x = static_cast<int>(std::floor(mover.position.x + dx * t));
                    const int z = static_cast<int>(std::floor(mover.position.z + dz * t));
                    const int y = static_cast<int>(std::floor(mover.position.y + 0.5));
                    for (int k = brain_def.jump > 0 ? 1 : 0; k < height + (brain_def.jump > 0 ? 1 : 0); ++k)
                        if (ai_solid(x, y + k, z))
                            return false;
                    // (mod-liquid) Nor straight into a lake.
                    if (!swims(brain_def) && liquids_defined &&
                        (liquid_cell({x, y, z}) || liquid_cell({x, y - 1, z})))
                        return false;
                }
                return true;
            };
            const bool near = flat_squared(*destination, mover.position) < 25.0 &&
                              std::abs(destination->y - mover.position.y) < 1.5 && open_line();
            if (!near)
            {
                const std::uint64_t target_key = goal.player != 0U ? goal.player : goal.entity != 0U ? (goal.entity | (1ULL << 63)) : 0U;
                const bool chase = target_key != 0U && (goal.kind == GoalKind::chase || goal.kind == GoalKind::keep ||
                                                        goal.kind == GoalKind::surround);
                if (const auto waypoint = next_step_toward(mover, creature, goal_cell, chase, target_key))
                    aim = {waypoint->x + 0.5, static_cast<double>(waypoint->y), waypoint->z + 0.5};
            }
            toward(aim, speed, false);
            break;
        }
        }
    }
    Entity &walker = entities[index];
    EntityMind &live = walker.mind;

    // Physics.
    const auto collides = [&](Vec3 at) {
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
                    if (ai_solid(x, y, z))
                        return true;
        return false;
    };
    const bool flying = brain_def.movement == MoveMode::fly;
    const bool floating = in_water || flying;
    // (mod-liquid) A walker in a liquid floats up and wades out at half
    // speed; on land it never steps into one (the bank stops it like a wall
    // it does not jump), unless it can swim.
    const bool wading = !floating && wading_in_liquid(walker.position, creature.height);
    const bool keeps_ashore = !floating && !wading && !swims(brain_def);
    if (floating)
    {
        // Momentum, so turns are curves.
        const double blend = std::min(1.0, static_cast<double>(step) * 4.0);
        walker.velocity.x += (desired.x - walker.velocity.x) * blend;
        walker.velocity.y += (desired.y - walker.velocity.y) * blend;
        walker.velocity.z += (desired.z - walker.velocity.z) * blend;
        if (in_water && brain_def.movement == MoveMode::swim)
        {
            // Never above the surface.
            const BlockPos above = floor_cell({centre.x, centre.y + creature.height * 0.5 + 0.1, centre.z});
            if (!water(above.x, above.y, above.z))
                walker.velocity.y = std::min(walker.velocity.y, 0.0);
        }
        if (flying && desired.x == 0.0 && desired.y == 0.0 && desired.z == 0.0)
        {
            // Hover with a gentle bob.
            live.hover_phase += step * 2.0F;
            walker.velocity.y = std::sin(live.hover_phase) * 0.4;
        }
    }
    else
    {
        walker.velocity.x = desired.x * (wading ? 0.5 : 1.0);
        walker.velocity.z = desired.z * (wading ? 0.5 : 1.0);
        const bool stranded = brain_def.movement == MoveMode::swim;
        if (stranded)
        {
            walker.velocity.x = 0.0;
            walker.velocity.z = 0.0;
        }
        walker.velocity.y = wading ? wading_vertical_speed(walker.velocity.y, step)
                                   : std::max(walker.velocity.y + gravity * step, -40.0);
    }
    bool blocked{};
    Vec3 next = walker.position;
    next.x += walker.velocity.x * step;
    if (collides(next))
    {
        next.x = walker.position.x;
        blocked = std::abs(walker.velocity.x) > 0.01;
    }
    else if (keeps_ashore && steps_into_liquid(next))
        next.x = walker.position.x;
    next.z += walker.velocity.z * step;
    if (collides(next))
    {
        next.z = walker.position.z;
        blocked = blocked || std::abs(walker.velocity.z) > 0.01;
    }
    else if (keeps_ashore && steps_into_liquid(next))
        next.z = walker.position.z;
    const bool climbing = blocked && brain_def.climb && !floating;
    if (climbing)
        walker.velocity.y = walker.position.y < (destination.has_value() ? destination->y + 1.0 : walker.position.y + 2.0)
                                ? walk
                                : walker.velocity.y;
    next.y += walker.velocity.y * step;
    walker.grounded = false;
    if (collides(next))
    {
        if (walker.velocity.y < 0.0)
        {
            walker.grounded = true;
            next.y = std::floor(next.y) + 1.0;
            if (collides(next))
                next.y = walker.position.y;
        }
        else
            next.y = walker.position.y;
        walker.velocity.y = 0.0;
    }
    if (blocked && floating && flying)
        walker.velocity.y = std::max(walker.velocity.y, static_cast<double>(speed) * 0.8);
    else if (blocked && (walker.grounded || wading) && !climbing && brain_def.jump > 0)
        walker.velocity.y = std::sqrt(2.0 * -gravity * (brain_def.jump + 0.3));
    const double moved = flat_squared(next, walker.position) + (next.y - walker.position.y) * (next.y - walker.position.y);
    const bool wants = desired.x != 0.0 || desired.z != 0.0 || (floating && desired.y != 0.0);
    live.stuck = wants && moved < 1e-6 ? live.stuck + step : 0.0F;
    if (live.stuck > 1.5F)
    {
        // Plan again, from a step aside.
        live.stuck = 0.0F;
        live.route.clear();
        const float angle = random01() * 6.2831853F;
        walker.velocity.x = std::cos(angle) * walk;
        walker.velocity.z = std::sin(angle) * walk;
    }
    walker.position = next;
    if (walker.position.y < -80.0)
    {
        remove_entity(walker, false);
        return;
    }
    // Facing.
    if (face.has_value())
        walker.yaw = static_cast<float>(std::atan2(face->x - walker.position.x, -(face->z - walker.position.z)));
    else if (std::abs(desired.x) + std::abs(desired.z) > 1e-3)
        walker.yaw = static_cast<float>(std::atan2(desired.x, -desired.z));
    // Animation: an attack's own clip from its start, so the strike lands
    // with the damage.
    const bool moving = std::abs(desired.x) + std::abs(desired.z) + (floating ? std::abs(desired.y) : 0.0) > 1e-3;
    std::string animation;
    if (live.attack >= 0)
    {
        animation = live.attack_is_ability ? brain_def.abilities[static_cast<std::size_t>(live.attack)].animation
                                           : brain_def.attacks[static_cast<std::size_t>(live.attack)].animation;
        if (animation.empty())
            animation = moving ? "walk" : "idle";
    }
    else if (!walker.animation_override.empty())
        animation = walker.animation_override;
    else if (walker.hurt_timer > 0.0F)
        animation = "hurt";
    else if (moving)
        animation = speed >= run - 1e-3F && run > walk ? "run" : "walk";
    else
        animation = "idle";
    walker.animation = std::move(animation);
}

} // namespace openminerz::modding::gameplay
