// (mod-ai) The script API of creature AI: `brains`, `ai`, `projectiles`,
// `teams`, `enemies` (retail enemy rules), `entities.find`, and the events
// entity.target_changed / state_changed / noticed / attacking /
// phase_changed and projectile.launched / hit. Registered into the same
// ApiRegistry as the rest of the gameplay service (GameplayService::
// register_api calls register_ai_api), so the SDK reference and the Luau
// types are generated from these strings.

#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::gameplay
{
namespace
{

[[nodiscard]] ParamSpec param(std::string name, ValueType type, std::string doc, bool optional = false)
{
    return ParamSpec{std::move(name), type, optional, std::move(doc)};
}

[[nodiscard]] ParamSpec optional(std::string name, ValueType type, std::string doc)
{
    return param(std::move(name), type, std::move(doc), true);
}

constexpr ValueType any = ValueType::any;
constexpr ValueType text = ValueType::string;
constexpr ValueType number = ValueType::number;
constexpr ValueType vector = ValueType::vec3;
constexpr ValueType handle = ValueType::handle;
constexpr ValueType table = ValueType::map;
constexpr ValueType list = ValueType::array;
constexpr ValueType boolean = ValueType::boolean;
constexpr ValueType nothing = ValueType::nil;

const ParamSpec entity_param = param("entity", handle, "An entity whose creature has AI.");

// What a function may touch, as api.cpp's Builder sorts it.
class Registrar final
{
  public:
    Registrar(ApiRegistry &registry, GameplayService &service) : registry_{registry}, service_{service}
    {
    }
    template <typename Function>
    void act(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
             std::string returns_doc, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc), side_server,
            capability::gameplay, true, std::forward<Function>(function));
    }
    template <typename Function>
    void ask(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
             std::string returns_doc, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc), side_server,
            capability::query, true, std::forward<Function>(function));
    }
    template <typename Function>
    void query(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
               std::string returns_doc, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc), side_both,
            capability::query, false, std::forward<Function>(function));
    }
    template <typename Function>
    void define(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
                std::string returns_doc, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc), side_both,
            capability::none, false, std::forward<Function>(function));
    }
    void event(std::string name, std::string doc, bool cancellable, std::vector<ParamSpec> payload)
    {
        registry_.add_event(EventSpec{std::move(name), std::move(doc), side_server, cancellable, std::move(payload)});
    }

  private:
    template <typename Function>
    void add(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
             std::string returns_doc, SideMask sides, CapabilityMask caps, bool needs_server, Function &&function)
    {
        GameplayService *service = &service_;
        registry_.add_function(
            FunctionSpec{name, std::move(doc), std::move(params), returns, std::move(returns_doc), sides, caps, false},
            [service, needs_server, call = std::forward<Function>(function)](CallContext &context,
                                                                              std::span<const Value> args) -> Value {
                if (needs_server && !service->server_running())
                {
                    if (context.side != Side::server)
                        throw ScriptError{"the gameplay server is not running in this session"};
                    service->start_server_deferred();
                }
                return call(context, args);
            });
    }

    ApiRegistry &registry_;
    GameplayService &service_;
};

[[nodiscard]] std::string text_arg(std::span<const Value> args, std::size_t index, std::string_view what)
{
    const Value &value = arg(args, index);
    if (value.type() != ValueType::string)
        throw ScriptError{std::format("{}: expected a string", what)};
    return value.as_string();
}

[[nodiscard]] double number_arg(std::span<const Value> args, std::size_t index, std::string_view what)
{
    const Value &value = arg(args, index);
    if (!value.is_number() || !std::isfinite(value.as_number()))
        throw ScriptError{std::format("{}: expected a number", what)};
    return value.as_number();
}

[[nodiscard]] const Value &options_arg(std::span<const Value> args, std::size_t index)
{
    static const Value empty{Map{}};
    const Value &value = arg(args, index);
    if (value.is_nil())
        return empty;
    if (value.type() != ValueType::map && !(value.type() == ValueType::array && value.as_array().empty()))
        throw ScriptError{"expected a table"};
    return value;
}

struct TargetArg final
{
    std::uint64_t player{};
    std::uint64_t entity{};
    std::optional<Vec3> position;
};

} // namespace

void register_ai_api(ApiRegistry &registry, GameplayService &service, GameplayService::Impl &impl)
{
    Registrar api{registry, service};

    const auto mind_of = [&impl](const Value &value) -> Entity & {
        Entity &body = impl.entity_arg(value);
        if (!body.mind.active)
            throw ScriptError{"that entity's creature has no AI (give it a brain, team, senses, attacks or movement)"};
        return body;
    };
    const auto index_of = [&impl](const Entity &body) {
        return static_cast<std::size_t>(&body - impl.entities.data());
    };
    const auto target_of = [&impl](const Value &value, std::string_view what, bool positions) {
        TargetArg target;
        if (value.is_nil())
            return target;
        if (value.type() == ValueType::handle)
        {
            const Handle other = value.as_handle();
            if (other.kind == HandleKind::player)
            {
                if (impl.player_by_handle(other) == nullptr)
                    throw ScriptError{std::format("{}: no such player", what)};
                target.player = other.id;
                return target;
            }
            if (other.kind == HandleKind::entity)
            {
                if (impl.entity(other.id) == nullptr)
                    throw ScriptError{std::format("{}: that entity no longer exists", what)};
                target.entity = other.id;
                return target;
            }
        }
        if (!positions)
            throw ScriptError{std::format("{}: expected a player or an entity", what)};
        target.position = to_vec3(value, what);
        return target;
    };

    // ============================================================== brains
    api.define("brains.define",
               "Defines a brain: a state machine creatures run on the server (creatures.define{brain = name}). "
               "`states` maps each state's name to {enter, tick, exit, damaged, heard, noticed} callbacks, each "
               "called as (entity, info); `tick` (and the others) may return the name of the state to switch to. "
               "A state may also be just a function (its tick). `initial` is the first state (default: the "
               "first one). info = {dt, state, time (seconds in this state), target, target_position, "
               "target_distance, sees_target, health, max_health, health_fraction, position, home, home_distance, "
               "pack_size, pack_hurt (0..1 of the pack's health lost), leader, phase, attacking, known, heard "
               "(where it last heard something), data}; damaged adds amount and attacker, heard adds noise, "
               "noticed adds noticed.",
               {param("name", text, "Short name or \"mod:name\"."), param("definition", table, "{initial, states}")},
               text, "The full key.",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   const std::string name = text_arg(args, 0, "name");
                   impl.define_brain(context.side, context.mod_id, name, options_arg(args, 1));
                   return impl.qualify(context.mod_id, name);
               });

    // ========================================================== projectiles
    api.define("projectiles.define",
               "Defines a projectile creatures (attacks = {x = {projectile = name}}) and scripts launch. "
               "Fields: speed (blocks/s), gravity (blocks/s^2 down), lifetime, radius, damage (player health, "
               "100 = full), creature_damage (default damage / 10), homing (degrees/s it turns toward its "
               "target), model / texture / scale (drawn along its flight; a glTF, or retail:arrow-style refs "
               "where the content exists), fx (a trail effect that follows it), impact_fx (an effect where it "
               "lands; with an explosion it stands for the game's own blast), pierce, explosion = {radius, damage, "
               "blocks (dig blocks out where the session's rules let monsters break them)}, breaks_blocks (the "
               "mod's own choice, whatever the difficulty: the explosion digs, or without one the block struck "
               "breaks; never inside spawn protection, decided by the host), on_hit = function(projectile, hit).",
               {param("name", text, "Short name or \"mod:name\"."), param("definition", table, "The projectile.")},
               text, "The full key.",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   const std::string name = text_arg(args, 0, "name");
                   impl.define_projectile(context.side, context.mod_id, name, options_arg(args, 1));
                   return impl.qualify(context.mod_id, name);
               });
    api.act("projectiles.launch",
            "Launches a projectile from a point. `toward` is a position, player or entity it is aimed at "
            "(leading a moving creature and arcing for gravity), or use options.velocity. Options: owner (an "
            "entity or player: its team's friends are spared and kills are credited), velocity, damage_scale.",
            {param("kind", text, "Projectile key."), param("from", vector, "Start."),
             optional("toward", any, "Position, player or entity."), optional("options", table, "{owner, velocity, damage_scale}")},
            handle, "The projectile, or nil (at the limit, or nowhere to aim).",
            [&impl, target_of](CallContext &context, std::span<const Value> args) -> Value {
                const ProjectileDef *def = impl.projectile_def(text_arg(args, 0, "kind"), context.mod_id);
                if (def == nullptr)
                    throw ScriptError{std::format("no projectile '{}'", text_arg(args, 0, "kind"))};
                const Vec3 from = to_vec3(arg(args, 1), "from");
                const Value &options = options_arg(args, 3);
                std::uint64_t owner_entity{};
                std::uint64_t owner_player{};
                if (const Value *owner = options.field("owner"); owner != nullptr && !owner->is_nil())
                {
                    const TargetArg who = target_of(*owner, "owner", false);
                    owner_entity = who.entity;
                    owner_player = who.player;
                }
                const TargetArg target = target_of(arg(args, 2), "toward", true);
                std::optional<Vec3> velocity;
                if (const Value *given = options.field("velocity"); given != nullptr && !given->is_nil())
                    velocity = to_vec3(*given, "velocity");
                else
                {
                    std::optional<Vec3> point = target.position;
                    Vec3 moving{};
                    if (target.player != 0U || target.entity != 0U)
                    {
                        point = impl.target_point(target.player, target.entity);
                        if (point.has_value())
                            point->y += target.player != 0U ? 1.3 : 0.8;
                        if (const Entity *body = impl.entity(target.entity))
                            moving = body->velocity;
                    }
                    if (point.has_value())
                        velocity = impl.aim_velocity(from, *point, moving, *def, true);
                }
                if (!velocity.has_value())
                    return Value{};
                const double scale = number_or(options, "damage_scale", 1.0);
                Projectile *launched = impl.launch_projectile(
                    *def, from, *velocity, owner_entity, owner_player, target.player, target.entity,
                    static_cast<float>(std::isfinite(scale) ? std::clamp(scale, 0.0, 100.0) : 1.0));
                return launched != nullptr ? Value{Handle{projectile_handle_kind, launched->id}} : Value{};
            });
    api.act("projectiles.remove", "Removes a projectile in flight (no hit).",
            {param("projectile", handle, "The projectile.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &value = arg(args, 0);
                if (value.type() != ValueType::handle || value.as_handle().kind != projectile_handle_kind)
                    throw ScriptError{"expected a projectile handle"};
                for (Projectile &projectile : impl.ai.projectiles)
                    if (projectile.id == value.as_handle().id && !projectile.removed)
                    {
                        projectile.removed = true;
                        impl.out().to_all(make_map({{"t", "prj-"},
                                                    {"id", static_cast<std::int64_t>(projectile.id)},
                                                    {"p", vec3_value(projectile.position)},
                                                    {"hit", "removed"}}),
                                          true);
                    }
                return Value{};
            });
    api.ask("projectiles.list", "Projectiles in flight, optionally within a radius of a point.",
            {optional("options", table, "{near, radius, kind}")}, list, "{projectile, kind, position, velocity} entries.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &options = options_arg(args, 0);
                const Value *near = options.field("near");
                const std::optional<Vec3> centre =
                    near != nullptr && !near->is_nil() ? std::optional{to_vec3(*near, "near")} : std::nullopt;
                const double radius = number_or(options, "radius", 32.0);
                const std::string kind = string_or(options, "kind", "");
                Array found;
                for (const Projectile &projectile : impl.ai.projectiles)
                {
                    if (projectile.removed || (!kind.empty() && projectile.key != kind &&
                                               projectile.key.substr(projectile.key.find(':') + 1U) != kind))
                        continue;
                    if (centre.has_value() && distance_squared(projectile.position, *centre) > radius * radius)
                        continue;
                    found.push_back(make_map({{"projectile", Handle{projectile_handle_kind, projectile.id}},
                                              {"kind", projectile.key},
                                              {"position", vec3_value(projectile.position)},
                                              {"velocity", vec3_value(projectile.velocity)}}));
                }
                return Value{std::move(found)};
            });

    // ==================================================================== ai
    api.act("ai.set_state", "Switches a creature's brain to a state (its exit and the new state's enter run).",
            {entity_param, param("state", text, "State name.")}, nothing, "",
            [&impl, mind_of, index_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                impl.set_state(body, text_arg(args, 1, "state"));
                impl.run_state_change(index_of(body));
                return Value{};
            });
    api.ask("ai.state", "A creature's current brain state.", {entity_param}, text, "State name.",
            [mind_of](CallContext &, std::span<const Value> args) -> Value { return mind_of(arg(args, 0)).mind.state; });
    api.ask("ai.target", "Whom a creature is after.", {entity_param}, handle, "A player or entity, or nil.",
            [mind_of](CallContext &, std::span<const Value> args) -> Value {
                const EntityMind &mind = mind_of(arg(args, 0)).mind;
                if (mind.target_player != 0U)
                    return Handle{HandleKind::player, mind.target_player};
                if (mind.target_entity != 0U)
                    return Handle{HandleKind::entity, mind.target_entity};
                return Value{};
            });
    api.act("ai.set_target",
            "Sets whom a creature is after (nil: nobody); entity.target_changed fires. It keeps the target until "
            "it loses it (memory) or something far more threatening appears.",
            {entity_param, param("target", any, "A player, an entity, or nil.")}, nothing, "",
            [&impl, mind_of, target_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const TargetArg target = target_of(arg(args, 1), "target", false);
                if (target.player != 0U || target.entity != 0U)
                {
                    MemoryEntry &entry = impl.remember(body.mind, target.player, target.entity);
                    entry.hostile = true;
                    entry.heard_at = impl.server_time;
                    entry.threat = std::max(entry.threat, 5.0F);
                    if (const auto at = impl.target_point(target.player, target.entity))
                        entry.last_position = *at;
                }
                impl.set_target(body, target.player, target.entity, "script");
                return Value{};
            });
    api.ask("ai.known", "What a creature knows of: every target it saw or heard and still remembers.",
            {entity_param}, list, "{target, position, visible, hostile, threat, seen (seconds ago)} entries.",
            [&impl, mind_of](CallContext &, std::span<const Value> args) -> Value {
                Array known;
                for (const MemoryEntry &entry : mind_of(arg(args, 0)).mind.memory)
                    known.push_back(make_map(
                        {{"target", entry.player != 0U ? Value{Handle{HandleKind::player, entry.player}}
                                                       : Value{Handle{HandleKind::entity, entry.entity}}},
                         {"position", vec3_value(entry.last_position)},
                         {"visible", entry.visible},
                         {"hostile", entry.hostile},
                         {"threat", entry.threat},
                         {"seen", impl.server_time - std::max(entry.seen_at, entry.heard_at)}}));
                return Value{std::move(known)};
            });
    api.act("ai.hunt",
            "Runs one step of the built-in hunter for this creature: chase or keep its distance, surround with its "
            "pack, use whichever attack fits, flee when hurt, return home past its leash, investigate what it "
            "heard, wander. Call it from a state's tick to borrow the default behaviour.",
            {entity_param}, nothing, "",
            [&impl, mind_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                if (const CreatureDef *creature = impl.server_registry.creature(body.creature))
                {
                    const std::string state = body.mind.state;
                    impl.hunt(body, *creature);
                    // A scripted brain owns its states: the hunter only steers.
                    if (impl.brain(creature->ai->brain, creature->mod) != nullptr)
                        body.mind.pending_state.clear();
                    static_cast<void>(state);
                }
                return Value{};
            });
    const auto goal_setter = [&](std::string name, std::string doc, std::vector<ParamSpec> params, GoalKind kind) {
        api.act(std::move(name), std::move(doc), std::move(params), nothing, "",
                [&impl, mind_of, target_of, kind](CallContext &, std::span<const Value> args) -> Value {
                    Entity &body = mind_of(arg(args, 0));
                    MoveGoal goal;
                    goal.kind = kind;
                    const Value &options = options_arg(args, 2);
                    if (kind == GoalKind::wander)
                    {
                        const double radius = std::clamp(number_or(arg(args, 1), "radius", 8.0), 1.0, 64.0);
                        const double angle = impl.random01() * 6.283185307179586;
                        const double reach = radius * (0.4 + 0.6 * impl.random01());
                        goal.point = {body.mind.home.x + std::cos(angle) * reach, body.position.y,
                                      body.mind.home.z + std::sin(angle) * reach};
                        goal.run = false;
                        body.mind.goal = goal;
                        return Value{};
                    }
                    if (kind == GoalKind::home)
                    {
                        goal.point = body.mind.home;
                        body.mind.goal = goal;
                        return Value{};
                    }
                    const TargetArg target = target_of(arg(args, 1), "target", true);
                    goal.player = target.player;
                    goal.entity = target.entity;
                    goal.point = target.position.value_or(body.position);
                    goal.run = bool_or(options, "run", kind != GoalKind::point);
                    goal.distance = static_cast<float>(std::clamp(number_or(options, "distance", kind == GoalKind::keep ? 6.0 : 1.5), 0.3, 128.0));
                    goal.distance2 = static_cast<float>(std::clamp(number_or(options, "far", goal.distance + 6.0), static_cast<double>(goal.distance), 256.0));
                    if (kind == GoalKind::surround)
                        goal.distance = static_cast<float>(std::clamp(number_or(options, "radius", 2.0), 0.5, 32.0));
                    if (kind == GoalKind::flee)
                        goal.until = impl.server_time + std::clamp(number_or(options, "seconds", 4.0), 0.1, 600.0);
                    if (kind == GoalKind::point && !target.position.has_value())
                    {
                        // A moving point: walk to where it is now.
                        if (const auto at = impl.target_point(target.player, target.entity))
                            goal.point = *at;
                        goal.player = 0U;
                        goal.entity = 0U;
                    }
                    body.mind.goal = goal;
                    return Value{};
                });
    };
    goal_setter("ai.chase", "Closes in on a player, an entity or a point (planning around terrain). Options: distance "
                            "(stop this close, default 1.5), run (default true).",
                {entity_param, param("target", any, "Player, entity or position."), optional("options", table, "{distance, run}")},
                GoalKind::chase);
    goal_setter("ai.keep_distance",
                "Stays between `distance` and `far` blocks of a target (what archers do), facing it.",
                {entity_param, param("target", any, "Player, entity or position."), optional("options", table, "{distance, far, run}")},
                GoalKind::keep);
    goal_setter("ai.move_to", "Walks (flies, swims) to a point. Options: run (default false).",
                {entity_param, param("position", any, "Position (or a player/entity's current position)."),
                 optional("options", table, "{run}")},
                GoalKind::point);
    goal_setter("ai.flee", "Runs away from a player, entity or point for `seconds` (default 4).",
                {entity_param, param("from", any, "Player, entity or position."), optional("options", table, "{seconds}")},
                GoalKind::flee);
    goal_setter("ai.surround",
                "Takes this creature's slot in a ring around the target (its pack spreads out evenly).",
                {entity_param, param("target", any, "Player or entity."), optional("options", table, "{radius}")},
                GoalKind::surround);
    goal_setter("ai.wander", "Strolls to a random point within `radius` (default 8) of its home.",
                {entity_param, optional("options", table, "{radius}")}, GoalKind::wander);
    goal_setter("ai.go_home", "Walks back to its home (where it spawned, or ai.set_home).", {entity_param},
                GoalKind::home);
    api.act("ai.stop", "Stops moving (the brain keeps thinking).", {entity_param}, nothing, "",
            [mind_of](CallContext &, std::span<const Value> args) -> Value {
                mind_of(arg(args, 0)).mind.goal = {};
                return Value{};
            });
    api.act("ai.set_home", "Sets the point a creature returns to (and its leash's centre).",
            {entity_param, param("position", vector, "Home.")}, nothing, "",
            [mind_of](CallContext &, std::span<const Value> args) -> Value {
                mind_of(arg(args, 0)).mind.home = to_vec3(arg(args, 1), "position");
                return Value{};
            });
    api.act("ai.attack",
            "Starts one of the creature's attacks (creatures.define{attacks = {...}}) at a target (default: its "
            "own). The attack's animation starts now on every client and the hit is decided `windup` seconds "
            "later: melee strikes what is still within reach and arc, ranged fires its projectile. False when "
            "the attack is cooling down, out of range, or another one is under way.",
            {entity_param, param("attack", text, "Attack name."), optional("target", handle, "Player or entity.")},
            boolean, "Whether it started.",
            [&impl, mind_of, index_of, target_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const std::string name = text_arg(args, 1, "attack");
                const CreatureDef *creature = impl.server_registry.creature(body.creature);
                if (creature == nullptr || creature->ai->attack(name) == nullptr)
                    throw ScriptError{std::format("the creature has no attack '{}'", name)};
                TargetArg target = target_of(arg(args, 2), "target", false);
                if (target.player == 0U && target.entity == 0U)
                {
                    target.player = body.mind.target_player;
                    target.entity = body.mind.target_entity;
                }
                return impl.start_attack(index_of(body), name, target.player, target.entity, false);
            });
    api.act("ai.use",
            "Uses one of the creature's abilities (creatures.define{abilities = {x = {cooldown, range, windup, "
            "animation, run = function(entity, target)}}}): after its windup, `run` is called. False while it "
            "cools down or another attack is under way.",
            {entity_param, param("ability", text, "Ability name."), optional("target", handle, "Player or entity.")},
            boolean, "Whether it started.",
            [&impl, mind_of, index_of, target_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const std::string name = text_arg(args, 1, "ability");
                const CreatureDef *creature = impl.server_registry.creature(body.creature);
                if (creature == nullptr || creature->ai->ability(name) == nullptr)
                    throw ScriptError{std::format("the creature has no ability '{}'", name)};
                TargetArg target = target_of(arg(args, 2), "target", false);
                if (target.player == 0U && target.entity == 0U)
                {
                    target.player = body.mind.target_player;
                    target.entity = body.mind.target_entity;
                }
                return impl.start_attack(index_of(body), name, target.player, target.entity, true);
            });
    api.ask("ai.ready", "Whether an attack or ability has cooled down.",
            {entity_param, param("name", text, "Attack or ability.")}, boolean, "Ready.",
            [&impl, mind_of](CallContext &, std::span<const Value> args) -> Value {
                const EntityMind &mind = mind_of(arg(args, 0)).mind;
                const auto found = mind.ready_at.find(text_arg(args, 1, "name"));
                return found == mind.ready_at.end() || found->second <= impl.server_time;
            });
    api.ask("ai.cooldown", "Seconds until an attack or ability is ready (0 when it is).",
            {entity_param, param("name", text, "Attack or ability.")}, number, "Seconds.",
            [&impl, mind_of](CallContext &, std::span<const Value> args) -> Value {
                const EntityMind &mind = mind_of(arg(args, 0)).mind;
                const auto found = mind.ready_at.find(text_arg(args, 1, "name"));
                return found == mind.ready_at.end() ? 0.0 : std::max(0.0, found->second - impl.server_time);
            });
    api.act("ai.fire",
            "Fires a projectile from the creature's eyes at a target, outside its attacks (no windup, no "
            "cooldown). Options: damage_scale, lead (default true).",
            {entity_param, param("projectile", text, "Projectile key."), param("target", any, "Player, entity or position."),
             optional("options", table, "{damage_scale, lead}")},
            handle, "The projectile, or nil.",
            [&impl, mind_of, target_of](CallContext &context, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const ProjectileDef *def = impl.projectile_def(text_arg(args, 1, "projectile"), context.mod_id);
                if (def == nullptr)
                    throw ScriptError{std::format("no projectile '{}'", text_arg(args, 1, "projectile"))};
                const TargetArg target = target_of(arg(args, 2), "target", true);
                std::optional<Vec3> point = target.position;
                Vec3 moving{};
                if (target.player != 0U || target.entity != 0U)
                {
                    point = impl.target_point(target.player, target.entity);
                    if (point.has_value())
                        point->y += target.player != 0U ? 1.3 : 0.8;
                    if (const Entity *other = impl.entity(target.entity))
                        moving = other->velocity;
                }
                if (!point.has_value())
                    return Value{};
                const Value &options = options_arg(args, 3);
                const Vec3 from = impl.eye_of(body);
                const auto velocity = impl.aim_velocity(from, *point, moving, *def, bool_or(options, "lead", true));
                if (!velocity.has_value())
                    return Value{};
                const double scale = number_or(options, "damage_scale", 1.0);
                Projectile *launched = impl.launch_projectile(
                    *def, from, *velocity, body.id, 0U, target.player, target.entity,
                    static_cast<float>(std::isfinite(scale) ? std::clamp(scale, 0.0, 100.0) : 1.0) * body.mind.damage_scale);
                return launched != nullptr ? Value{Handle{projectile_handle_kind, launched->id}} : Value{};
            });
    api.ask("ai.can_see", "Whether the creature sees a player or entity now (range, field of view, line of sight).",
            {entity_param, param("target", handle, "Player or entity.")}, boolean, "Visible.",
            [&impl, mind_of, target_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const TargetArg target = target_of(arg(args, 1), "target", false);
                const CreatureDef *creature = impl.server_registry.creature(body.creature);
                const auto point = impl.target_point(target.player, target.entity);
                if (creature == nullptr || !point.has_value())
                    return false;
                const Vec3 eye = impl.eye_of(body);
                const Vec3 chest{point->x, point->y + 1.2, point->z};
                const double distance = std::sqrt(distance_squared(eye, chest));
                if (distance > creature->ai->sight)
                    return false;
                const double dx = chest.x - eye.x;
                const double dz = chest.z - eye.z;
                const double flat = std::sqrt(dx * dx + dz * dz);
                if (distance > 2.5 && flat > 1e-6 && creature->ai->fov < 359.0F &&
                    (dx * std::sin(body.yaw) - dz * std::cos(body.yaw)) / flat <
                        std::cos(creature->ai->fov * 3.141592653589793 / 360.0))
                    return false;
                return creature->ai->xray || impl.line_of_sight(eye, chest);
            });
    api.act("ai.remember",
            "Makes a creature aware of a player or entity (as if it had heard it), hostile unless "
            "options.hostile is false. Options: threat (default 1).",
            {entity_param, param("target", handle, "Player or entity."), optional("options", table, "{threat, hostile}")},
            nothing, "",
            [&impl, mind_of, target_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const TargetArg target = target_of(arg(args, 1), "target", false);
                const Value &options = options_arg(args, 2);
                MemoryEntry &entry = impl.remember(body.mind, target.player, target.entity);
                entry.heard_at = impl.server_time;
                entry.hostile = bool_or(options, "hostile", true);
                entry.threat = std::max(entry.threat, static_cast<float>(std::clamp(number_or(options, "threat", 1.0), 0.0, 1000.0)));
                if (const auto at = impl.target_point(target.player, target.entity))
                    entry.last_position = *at;
                return Value{};
            });
    api.act("ai.forget", "Makes a creature forget a player or entity (and stop chasing it).",
            {entity_param, param("target", handle, "Player or entity.")}, nothing, "",
            [&impl, mind_of, target_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const TargetArg target = target_of(arg(args, 1), "target", false);
                std::erase_if(body.mind.memory, [&](const MemoryEntry &entry) {
                    return entry.player == target.player && entry.entity == target.entity;
                });
                if (body.mind.target_player == target.player && body.mind.target_entity == target.entity)
                    impl.set_target(body, 0U, 0U, "forgot");
                return Value{};
            });
    api.act("ai.threat", "Adds to (or, negative, takes from) how threatening a creature finds a target.",
            {entity_param, param("target", handle, "Player or entity."), param("amount", number, "Threat.")}, nothing,
            "", [&impl, mind_of, target_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const TargetArg target = target_of(arg(args, 1), "target", false);
                MemoryEntry &entry = impl.remember(body.mind, target.player, target.entity);
                entry.heard_at = impl.server_time;
                entry.threat = std::max(0.0F, entry.threat + static_cast<float>(std::clamp(number_arg(args, 2, "amount"), -1.0e4, 1.0e4)));
                return Value{};
            });
    api.ask("ai.pack", "The members of a creature's pack (itself included), leader first.", {entity_param}, list,
            "Entity handles.", [&impl, mind_of](CallContext &, std::span<const Value> args) -> Value {
                const Entity &body = mind_of(arg(args, 0));
                Array members;
                const auto found = impl.ai.packs.find(body.mind.pack);
                if (found == impl.ai.packs.end())
                {
                    members.emplace_back(Handle{HandleKind::entity, body.id});
                    return Value{std::move(members)};
                }
                members.emplace_back(Handle{HandleKind::entity, found->second.leader});
                for (const std::uint64_t member : found->second.members)
                    if (member != found->second.leader)
                        members.emplace_back(Handle{HandleKind::entity, member});
                return Value{std::move(members)};
            });
    api.act("ai.join_pack",
            "Puts a creature in another's pack (creatures spawned together by a spawn rule, or with "
            "entities.spawn{pack = other}, already are): packs share what they notice, surround a target "
            "together, and info.pack_hurt tells a brain how badly the pack is doing.",
            {entity_param, param("other", handle, "A member of the pack to join.")}, nothing, "",
            [&impl, mind_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                Entity &other = mind_of(arg(args, 1));
                if (other.id == body.id || (body.mind.pack != 0U && body.mind.pack == other.mind.pack))
                    return Value{};
                if (body.mind.pack != 0U)
                    if (const auto old = impl.ai.packs.find(body.mind.pack); old != impl.ai.packs.end())
                    {
                        std::erase(old->second.members, body.id);
                        if (old->second.members.empty())
                            impl.ai.packs.erase(old);
                        else if (old->second.leader == body.id)
                            old->second.leader = old->second.members.front();
                    }
                body.mind.pack = 0U;
                impl.join_pack(body, other);
                return Value{};
            });
    api.ask("ai.phase", "A boss's phase: 0 before the first threshold, then 1, 2, ...", {entity_param}, number,
            "Phase.", [mind_of](CallContext &, std::span<const Value> args) -> Value {
                return static_cast<std::int64_t>(mind_of(arg(args, 0)).mind.phase + 1);
            });
    api.act("ai.set_scale", "Scales a creature's speed and damage from now on (boss phases do this too).",
            {entity_param, param("scale", table, "{speed, damage}")}, nothing, "",
            [mind_of](CallContext &, std::span<const Value> args) -> Value {
                Entity &body = mind_of(arg(args, 0));
                const Value &scale = arg(args, 1);
                body.mind.speed_scale = static_cast<float>(std::clamp(number_or(scale, "speed", body.mind.speed_scale), 0.0, 10.0));
                body.mind.damage_scale = static_cast<float>(std::clamp(number_or(scale, "damage", body.mind.damage_scale), 0.0, 100.0));
                return Value{};
            });
    api.act("ai.noise",
            "Makes a noise creatures may hear: those within hearing * loudness / 16 blocks remember it (and who "
            "made it, given a player or entity). The game makes noises itself: gunshots (32), explosions (48), "
            "digging and building (8).",
            {param("position", vector, "Where."), optional("loudness", number, "Blocks it carries at hearing 16 (default 16)."),
             optional("source", handle, "The player or entity that made it.")},
            nothing, "", [&impl, &service, target_of](CallContext &, std::span<const Value> args) -> Value {
                const Vec3 at = to_vec3(arg(args, 0), "position");
                const double loudness = arg(args, 1).is_nil() ? 16.0 : number_arg(args, 1, "loudness");
                const TargetArg source = target_of(arg(args, 2), "source", false);
                std::optional<std::uint8_t> gid;
                if (source.player != 0U)
                    if (const PlayerRecord *player = impl.player_by_serial(source.player))
                        gid = player->gid;
                service.noise(at, static_cast<float>(std::clamp(loudness, 0.1, 256.0)), "script", gid);
                if (source.entity != 0U && !impl.ai.noises.empty())
                    impl.ai.noises.back().entity = source.entity;
                return Value{};
            });
    api.act("ai.set_water",
            "Where swimmers find water: open cells whose centre is below `level` (default 1.5, the Lagoon's sea) "
            "in the listed biomes (default {\"lagoon\"}; \"*\" for every biome).",
            {param("water", table, "{level, biomes}")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &water = options_arg(args, 0);
                impl.ai.sea_level = static_cast<float>(std::clamp(number_or(water, "level", 1.5), -64.0, 64.0));
                if (const Value *biomes = water.field("biomes"); biomes != nullptr && biomes->type() == ValueType::array)
                {
                    impl.ai.water_biomes.clear();
                    for (const Value &biome : biomes->as_array())
                        impl.ai.water_biomes.push_back(biome.as_string());
                }
                impl.ai.water_columns.clear();
                return Value{};
            });
    api.ask("ai.stats", "AI load since the session started: thoughts, deferred thoughts, planning, time spent.", {},
            table, "{minds, thinks, deferred, think_ms, step_ms, paths, nodes, fields, cache_hits, sight_checks, projectiles}.",
            [&impl](CallContext &, std::span<const Value>) -> Value {
                const AiStats &stats = impl.ai.stats;
                std::int64_t minds{};
                for (const Entity &body : impl.entities)
                    if (!body.removed && body.mind.active)
                        ++minds;
                return make_map({{"minds", minds},
                                 {"thinks", static_cast<std::int64_t>(stats.thinks)},
                                 {"deferred", static_cast<std::int64_t>(stats.deferred_thinks)},
                                 {"think_ms", stats.think_ms},
                                 {"step_ms", stats.step_ms},
                                 {"paths", static_cast<std::int64_t>(stats.path_searches)},
                                 {"nodes", static_cast<std::int64_t>(stats.path_nodes)},
                                 {"fields", static_cast<std::int64_t>(stats.field_builds)},
                                 {"cache_hits", static_cast<std::int64_t>(stats.cache_hits)},
                                 {"sight_checks", static_cast<std::int64_t>(stats.line_checks)},
                                 {"projectiles", static_cast<std::int64_t>(impl.ai.projectiles.size())}});
            });

    // ================================================================= teams
    api.act("teams.set_relation",
            "How one team treats another: \"hostile\" (attacks on sight), \"neutral\" (only fights back) or "
            "\"friendly\" (never attacks; its projectiles spare them). Both ways unless options.one_way. Players "
            "are team \"players\" unless teams.set gives them another.",
            {param("team", text, "Team."), param("other", text, "Other team."), param("relation", text, "hostile, neutral or friendly."),
             optional("options", table, "{one_way}")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                const std::string kind = text_arg(args, 2, "relation");
                const Relation relation = kind == "hostile"   ? Relation::hostile
                                          : kind == "friendly" ? Relation::friendly
                                          : kind == "neutral"  ? Relation::neutral
                                                               : throw ScriptError{"relation: hostile, neutral or friendly"};
                const std::string a = text_arg(args, 0, "team");
                const std::string b = text_arg(args, 1, "other");
                impl.ai.relations[std::pair{a, b}] = relation;
                if (!bool_or(options_arg(args, 3), "one_way", false))
                    impl.ai.relations[std::pair{b, a}] = relation;
                return Value{};
            });
    api.ask("teams.relation", "How a team treats another (as a creature of `team` with no own lists would).",
            {param("team", text, "Team."), param("other", text, "Other team.")}, text, "hostile, neutral or friendly.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Relation relation = impl.relation(text_arg(args, 0, "team"), text_arg(args, 1, "other"), nullptr);
                return relation == Relation::hostile ? "hostile" : relation == Relation::friendly ? "friendly" : "neutral";
            });
    api.ask("teams.of", "A player's or entity's team.", {param("who", handle, "Player or entity.")}, text, "Team.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &who = arg(args, 0);
                if (who.type() == ValueType::handle && who.as_handle().kind == HandleKind::player)
                    return impl.team_of_player(impl.player_arg(who).serial);
                return impl.team_of_entity(impl.entity_arg(who));
            });
    api.act("teams.set", "Puts a player or an entity in a team (\"\" restores the default).",
            {param("who", handle, "Player or entity."), param("team", text, "Team.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &who = arg(args, 0);
                const std::string team = text_arg(args, 1, "team").substr(0, 64U);
                if (who.type() == ValueType::handle && who.as_handle().kind == HandleKind::player)
                {
                    const std::uint64_t serial = impl.player_arg(who).serial;
                    if (team.empty())
                        impl.ai.player_teams.erase(serial);
                    else
                        impl.ai.player_teams[serial] = team;
                    return Value{};
                }
                impl.entity_arg(who).mind.team = team;
                return Value{};
            });

    // ======================================================= retail enemies
    api.act("enemies.rules",
            "Rules for the RETAIL enemies of the whole session (every peer spawns its own player's enemies; every "
            "peer applies these). Fields: enabled (false: no retail enemies at all), family switches zombie, "
            "skeleton, archer, felguard, alien, dragon, spawner (1.9.9.8 spawner blocks), hell (anything below "
            "y -40, hell spawners), distance = {scale, offset, max} (the distance retail picks zombie tiers, "
            "skeleton kinds, dragons and spawn pace by: distance * scale + offset, at most max - for a world wider "
            "than retail's), regions = {{min = vector, max = vector (x/z box), y_min, y_max, biome (a biome key, "
            "mod biomes too), enabled, zombie = ..., tier (a fixed tier distance)}, ...} (later regions win), "
            "clear_existing (true: the retail enemies already about that the rules switch off leave, as retail's "
            "own give-up sends them, without drops, on every peer; default false: only spawns from now on follow "
            "the rules). A spawner block whose waves could only bring switched-off families stays silent (no "
            "start, no reward); in one that runs, members of a switched-off family never appear and count as "
            "defeated. A joined player spawns no retail enemy until these rules reach it. Never reaches a stock "
            "player: server mods only run where none can join.",
            {param("rules", table, "The rules.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.ai.retail = impl.parse_retail_rules(options_arg(args, 0));
                impl.ai.retail_dirty = true;
                return Value{};
            });
    api.act("enemies.reset", "Back to the retail enemies, exactly as the game spawns them.", {}, nothing, "",
            [&impl](CallContext &, std::span<const Value>) -> Value {
                impl.ai.retail = {};
                impl.ai.retail_dirty = true;
                return Value{};
            });
    api.query("enemies.get_rules", "The retail enemy rules in effect (the host's, on a client).", {}, table,
              "{active, enabled, families = {zombie = true, ...}, distance = {scale, offset, max}, regions, "
              "clear_existing}.",
              [&impl](CallContext &, std::span<const Value>) -> Value {
                  const RetailEnemyRules &rules = impl.effective_retail_rules();
                  Map families;
                  for (std::size_t index{}; index < static_cast<std::size_t>(RetailEnemyClass::count); ++index)
                      families.emplace_back(retail_enemy_class_name(static_cast<RetailEnemyClass>(index)),
                                            Value{(rules.classes & (1U << index)) != 0U});
                  Value raw = impl.retail_rules_value(rules);
                  const Value *regions = raw.field("regions");
                  return make_map({{"active", rules.active},
                                   {"enabled", rules.enabled},
                                   {"families", Value{std::move(families)}},
                                   {"distance", make_map({{"scale", rules.distance_scale},
                                                          {"offset", rules.distance_offset},
                                                          {"max", rules.distance_maximum}})},
                                   {"regions", regions != nullptr ? *regions : Value{Array{}}},
                                   {"clear_existing", rules.clear_existing}});
              });
    api.query("enemies.allowed", "Whether the rules let a retail enemy family appear at a position.",
              {param("family", text, "zombie, skeleton, archer, felguard, alien, dragon, spawner or hell."),
               param("position", vector, "Where.")},
              boolean, "Allowed.",
              [&service](CallContext &, std::span<const Value> args) -> Value {
                  const auto family = retail_enemy_class_from_name(text_arg(args, 0, "family"));
                  if (!family.has_value())
                      throw ScriptError{"family: zombie, skeleton, archer, felguard, alien, dragon, spawner or hell"};
                  return service.retail_enemy_allowed(*family, to_vec3(arg(args, 1), "position"));
              });

    // ===================================================== entities.find
    api.ask("entities.find",
            "Finds entities: near = position, radius (default 32), kind (creature key), team, hostile_to (a "
            "player or entity: those whose team attacks it), targeting (a player or entity: those after it), "
            "state (brain state), alive_only (default true), limit, sort = \"distance\" (from near).",
            {optional("options", table, "{near, radius, kind, team, hostile_to, targeting, state, limit, sort}")}, list,
            "Entity handles.",
            [&impl, target_of](CallContext &context, std::span<const Value> args) -> Value {
                const Value &options = options_arg(args, 0);
                std::string kind = string_or(options, "kind", "");
                if (!kind.empty() && kind.find(':') == std::string::npos)
                    kind = std::format("{}:{}", context.mod_id, kind);
                const CreatureDef *creature = kind.empty() ? nullptr : impl.server_registry.creature(kind);
                if (!kind.empty() && creature == nullptr)
                    throw ScriptError{std::format("no creature '{}'", kind)};
                const Value *near = options.field("near");
                const std::optional<Vec3> centre =
                    near != nullptr && !near->is_nil() ? std::optional{to_vec3(*near, "near")} : std::nullopt;
                const double radius = std::clamp(number_or(options, "radius", 32.0), 0.0, 4096.0);
                const std::string team = string_or(options, "team", "");
                const std::string state = string_or(options, "state", "");
                const TargetArg hostile_to =
                    options.field("hostile_to") != nullptr ? target_of(*options.field("hostile_to"), "hostile_to", false) : TargetArg{};
                const TargetArg targeting =
                    options.field("targeting") != nullptr ? target_of(*options.field("targeting"), "targeting", false) : TargetArg{};
                const auto limit = static_cast<std::size_t>(std::clamp(number_or(options, "limit", 256.0), 0.0, 4096.0));
                std::vector<std::pair<double, std::uint64_t>> found;
                for (const Entity &body : impl.entities)
                {
                    if (body.removed || (creature != nullptr && body.creature != creature->id))
                        continue;
                    const double distance = centre.has_value() ? distance_squared(body.position, *centre) : 0.0;
                    if (centre.has_value() && distance > radius * radius)
                        continue;
                    if (!team.empty() && impl.team_of_entity(body) != team)
                        continue;
                    if (!state.empty() && body.mind.state != state)
                        continue;
                    if (targeting.player != 0U || targeting.entity != 0U)
                        if (body.mind.target_player != targeting.player || body.mind.target_entity != targeting.entity)
                            continue;
                    if (hostile_to.player != 0U || hostile_to.entity != 0U)
                    {
                        const CreatureDef *own = impl.server_registry.creature(body.creature);
                        if (own == nullptr)
                            continue;
                        bool hostile{};
                        if (hostile_to.player != 0U)
                            hostile = impl.hostile_to_player(body, *own, hostile_to.player);
                        else if (const Entity *other = impl.entity(hostile_to.entity))
                            hostile = impl.hostile_to_entity(body, *own, *other);
                        if (!hostile)
                            continue;
                    }
                    found.emplace_back(distance, body.id);
                }
                if (string_or(options, "sort", "") == "distance")
                    std::ranges::sort(found);
                Array handles;
                for (const auto &[distance, id] : found)
                {
                    if (handles.size() >= limit)
                        break;
                    handles.emplace_back(Handle{HandleKind::entity, id});
                }
                return Value{std::move(handles)};
            });

    // ================================================================ events
    const ParamSpec who = param("entity", handle, "The creature.");
    api.event("entity.target_changed", "A creature chose a new target (or lost its old one).", false,
              {who, param("target", handle, "Player or entity, or nil."), param("previous", handle, "The old target, or nil."),
               param("reason", text, "noticed, threat, damage, lost, fled, leash, script.")});
    api.event("entity.state_changed", "A creature's brain switched state.", false,
              {who, param("state", text, "New state."), param("previous", text, "Old state.")});
    api.event("entity.noticed", "A creature noticed a player or entity it did not know of (sight).", false,
              {who, param("target", handle, "Player or entity.")});
    api.event("entity.attacking",
              "A creature's attack or ability reaches its damage moment. Return false to stop the hit (or the "
              "shot, or the ability's run).",
              true, {who, param("attack", text, "Attack or ability name."), param("target", handle, "Player or entity, or nil.")});
    api.event("entity.phase_changed", "A boss passed a health threshold (creatures.define{boss = {phases}}).", false,
              {who, param("phase", number, "New phase (1-based)."), param("name", text, "Its name."),
               param("previous", number, "Old phase (0 = none)."), param("health", number, "Health fraction.")});
    api.event("projectile.launched", "A projectile was launched.", false,
              {param("projectile", handle, "The projectile."), param("kind", text, "Projectile key."),
               param("owner", handle, "Entity or player, or nil."), param("position", vector, "Start.")});
    api.event("projectile.hit",
              "A projectile struck terrain, a player or a creature (decided on the host). Return false to let it "
              "fly on through a player or creature without harm (against terrain it just ends).",
              true,
              {param("projectile", handle, "The projectile."), param("kind", text, "Projectile key."),
               param("owner", handle, "Entity or player, or nil."), param("position", vector, "Where."),
               param("target", handle, "Player or entity struck, or nil."), param("block", text, "Block struck, or nil."),
               param("damage", number, "Damage it will deal.")});
}

} // namespace openminerz::modding::gameplay
