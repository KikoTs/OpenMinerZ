// (mod-ai) Creature AI: brains, perception, teams and threat, packs,
// attacks and abilities, bosses, the think budget, and the client half of
// projectiles and boss bars.
//
// A creature whose definition has any AI field (brain, team, hostile_to,
// senses, attacks, abilities, movement, boss) is simulated here instead of by
// the built-in behaviours of entities.cpp, on the server half only; clients
// see it through the same ent+/ent~/ent- messages as every mod entity.
//
//   think    at `think_rate` per second near a player, slower with distance
//            (x2 past 32 blocks, x4 past 64, x8 past 96), at most
//            AiLimits::thinks_per_step thoughts per 20 Hz step (the rest wait
//            for the next step): perception, target choice, then the brain.
//   brain    a state machine. A brain (brains.define) names its states; each
//            may have enter/tick/exit/damaged/heard/noticed callbacks, called
//            with (entity, info); tick returns the next state's name or nil.
//            Without one the built-in hunter runs (and ai.hunt(entity) runs
//            one step of it from a script).
//   senses   sight (range, a cone, line of sight through the terrain),
//            hearing (noises: gunshots, explosions, digging, scripts), memory
//            (a lost target is remembered `memory` seconds), threat (damage
//            done to it, nearness) and pack alerts.
//   teams    every creature has a team; hostile_to / friends and
//            teams.relation decide whom it fights, players (team "players"
//            unless teams.set) included.
//   attacks  melee (reach, arc) and ranged (a projectile) with a windup: the
//            attack's animation starts on every client when it begins and the
//            damage lands `windup` later, so the strike frame and the hit
//            agree. Damage to a player goes through the session's authority.
//   bosses   a bar on every client (in range), phases at health thresholds,
//            an arena leash and music.
//
// Server -> client messages added here (see service.cpp for the others):
//   prj+ {id, kind, model, texture, fx, scale, r, p, v, g}   a projectile launched
//   prj~ {list = [{id, p, v}]}                              homing ones, 10 Hz
//   prj- {id, p, hit}                                       it hit or expired
//   boss {id, mod, title, color, hp, phase, music, p, range} | {id, gone = true}
//   retail                                                  enemies.rules (ai_retail.cpp)

#include "internal.hpp"

#include "openminerz/inventory.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr double memory_threat_decay = 0.15; // per second
constexpr float grid_cell = 16.0F;

[[nodiscard]] double horizontal_squared(Vec3 a, Vec3 b) noexcept
{
    const double dx = a.x - b.x;
    const double dz = a.z - b.z;
    return dx * dx + dz * dz;
}

[[nodiscard]] float yaw_to(Vec3 from, Vec3 to) noexcept
{
    return static_cast<float>(std::atan2(to.x - from.x, -(to.z - from.z)));
}

[[nodiscard]] std::int64_t grid_key(double x, double z) noexcept
{
    const auto cx = static_cast<std::int64_t>(std::floor(x / grid_cell));
    const auto cz = static_cast<std::int64_t>(std::floor(z / grid_cell));
    return (cx << 32) ^ (cz & 0xFFFFFFFFLL);
}

[[nodiscard]] Handle player_handle_for(std::uint64_t serial) noexcept
{
    return Handle{HandleKind::player, serial};
}

[[nodiscard]] Value target_value(std::uint64_t player, std::uint64_t entity) noexcept
{
    if (player != 0U)
        return Value{player_handle_for(player)};
    if (entity != 0U)
        return Value{Handle{HandleKind::entity, entity}};
    return Value{};
}

[[nodiscard]] MoveMode movement_from(std::string_view name)
{
    if (name == "ground" || name == "walk")
        return MoveMode::ground;
    if (name == "fly" || name == "flying")
        return MoveMode::fly;
    if (name == "swim" || name == "swimming")
        return MoveMode::swim;
    if (name == "amphibious")
        return MoveMode::amphibious;
    throw ScriptError{std::format("unknown movement '{}' (ground, fly, swim, amphibious)", name)};
}

[[nodiscard]] std::vector<std::string> string_list(const Value &table, std::string_view key)
{
    std::vector<std::string> out;
    const Value *value = table.field(key);
    if (value == nullptr || value->is_nil())
        return out;
    if (value->type() == ValueType::string)
    {
        out.push_back(value->as_string());
        return out;
    }
    if (value->type() == ValueType::map && value->as_map().empty())
        return out;
    if (value->type() != ValueType::array)
        throw ScriptError{std::format("'{}' must be a list of names", key)};
    for (const Value &entry : value->as_array())
        out.push_back(entry.as_string());
    return out;
}

[[nodiscard]] float clamped(const Value &table, std::string_view key, double fallback, double low,
                            double high)
{
    const double value = number_or(table, key, fallback);
    return static_cast<float>(std::isfinite(value) ? std::clamp(value, low, high) : fallback);
}

} // namespace

const BrainState *BrainDef::state(std::string_view name) const noexcept
{
    const auto found = std::ranges::find(states, name, &BrainState::name);
    return found == states.end() ? nullptr : &*found;
}

const AttackDef *CreatureAi::attack(std::string_view name) const noexcept
{
    const auto found = std::ranges::find(attacks, name, &AttackDef::name);
    return found == attacks.end() ? nullptr : &*found;
}

const AbilityDef *CreatureAi::ability(std::string_view name) const noexcept
{
    const auto found = std::ranges::find(abilities, name, &AbilityDef::name);
    return found == abilities.end() ? nullptr : &*found;
}

// ------------------------------------------------------------ definitions

void GameplayService::Impl::define_creature_ai(Side side, std::string_view mod, CreatureDef &creature,
                                               const Value &def, std::vector<Callback> &kept)
{
    static constexpr std::array<std::string_view, 9> ai_fields{
        "brain", "team", "hostile_to", "friends", "senses", "attacks", "abilities", "movement", "boss"};
    if (std::ranges::none_of(ai_fields, [&](std::string_view key) {
            const Value *field = def.field(key);
            return field != nullptr && !field->is_nil();
        }))
        return;
    auto made = std::make_shared<CreatureAi>();
    const bool hostile = creature.brain == Brain::hostile;
    made->team = string_or(def, "team", hostile                        ? "monsters"
                                      : creature.brain == Brain::npc ? "villagers"
                                                                     : "animals");
    made->hostile_to = def.field("hostile_to") != nullptr ? string_list(def, "hostile_to")
                     : hostile                          ? std::vector<std::string>{"players"}
                                                        : std::vector<std::string>{};
    made->friends = string_list(def, "friends");
    if (const Value *senses = def.field("senses"); senses != nullptr && senses->type() == ValueType::map)
    {
        made->sight = clamped(*senses, "sight", creature.view_range, 0.0, 128.0);
        made->fov = clamped(*senses, "fov", 140.0, 1.0, 360.0);
        made->hearing = clamped(*senses, "hearing", made->sight, 0.0, 128.0);
        made->memory = clamped(*senses, "memory", 10.0, 0.0, 600.0);
        made->xray = bool_or(*senses, "xray", false);
    }
    else
    {
        made->sight = creature.view_range;
        made->hearing = creature.view_range;
    }
    if (const Value *movement = def.field("movement"); movement != nullptr && !movement->is_nil())
    {
        if (movement->type() == ValueType::string)
            made->movement = movement_from(movement->as_string());
        else
        {
            made->movement = movement_from(string_or(*movement, "mode", "ground"));
            made->jump = static_cast<int>(clamped(*movement, "jump", 1.0, 0.0, 4.0));
            made->fall = static_cast<int>(clamped(*movement, "fall", 3.0, 0.0, 16.0));
            made->climb = bool_or(*movement, "climb", false);
            made->swim_speed = clamped(*movement, "swim_speed", -1.0, -1.0, 30.0);
            made->fly_height = clamped(*movement, "height", 5.0, 0.0, 48.0);
        }
    }
    made->think_rate = clamped(def, "think_rate", 5.0, 0.2, 20.0);
    made->leash = clamped(def, "leash", 0.0, 0.0, 512.0);
    made->flee_health = clamped(def, "flee_health", 0.0, 0.0, 1.0);
    made->pack_flee = clamped(def, "pack_flee", 0.0, 0.0, 1.0);
    made->surround = bool_or(def, "surround", true);
    made->brain = string_or(def, "brain", "");
    if (!made->brain.empty() && made->brain.find(':') == std::string::npos)
        made->brain = qualify(mod, made->brain);
    if (const Value *attacks = def.field("attacks"); attacks != nullptr && attacks->type() == ValueType::map)
        for (const auto &[name, entry] : attacks->as_map())
        {
            if (entry.type() != ValueType::map)
                throw ScriptError{std::format("attack '{}' must be a table", name)};
            AttackDef attack;
            attack.name = name;
            const std::string kind = string_or(entry, "type", entry.field("projectile") != nullptr ? "ranged" : "melee");
            if (kind != "melee" && kind != "ranged")
                throw ScriptError{std::format("attack '{}': type must be melee or ranged", name)};
            attack.kind = kind == "ranged" ? AttackKind::ranged : AttackKind::melee;
            attack.reach = clamped(entry, attack.kind == AttackKind::ranged ? "range" : "reach",
                                   attack.kind == AttackKind::ranged ? 20.0 : creature.attack_range, 0.5, 128.0);
            attack.min_range = clamped(entry, "min_range", 0.0, 0.0, 64.0);
            attack.arc = clamped(entry, "arc", 100.0, 1.0, 360.0);
            attack.damage = clamped(entry, "damage", creature.damage, 0.0, 1000.0);
            attack.creature_damage = clamped(entry, "creature_damage", -1.0, -1.0, 100'000.0);
            attack.cooldown = clamped(entry, "cooldown", creature.attack_cooldown, 0.05, 600.0);
            attack.windup = clamped(entry, "windup", 0.35, 0.0, 10.0);
            attack.recovery = clamped(entry, "recovery", 0.25, 0.0, 10.0);
            attack.animation = string_or(entry, "animation", "attack");
            attack.projectile = string_or(entry, "projectile", "");
            if (!attack.projectile.empty() && attack.projectile.find(':') == std::string::npos)
                attack.projectile = qualify(mod, attack.projectile);
            if (attack.kind == AttackKind::ranged && attack.projectile.empty())
                throw ScriptError{std::format("ranged attack '{}' needs a projectile", name)};
            attack.spread = clamped(entry, "spread", 0.0, 0.0, 45.0);
            attack.count = static_cast<int>(clamped(entry, "count", 1.0, 1.0, 16.0));
            attack.lead = bool_or(entry, "lead", true);
            attack.weight = clamped(entry, "weight", 1.0, 0.0, 1000.0);
            made->attacks.push_back(std::move(attack));
        }
    if (const Value *abilities = def.field("abilities"); abilities != nullptr && abilities->type() == ValueType::map)
        for (const auto &[name, entry] : abilities->as_map())
        {
            if (entry.type() != ValueType::map)
                throw ScriptError{std::format("ability '{}' must be a table", name)};
            AbilityDef ability;
            ability.name = name;
            ability.cooldown = clamped(entry, "cooldown", 5.0, 0.0, 3600.0);
            ability.range = clamped(entry, "range", 16.0, 0.0, 256.0);
            ability.windup = clamped(entry, "windup", 0.0, 0.0, 10.0);
            ability.animation = string_or(entry, "animation", "");
            ability.run = callback_or_none(entry, "run");
            if (ability.run.valid())
            {
                kept.push_back(ability.run);
                hold(side, ability.run);
            }
            made->abilities.push_back(std::move(ability));
        }
    if (const Value *boss = def.field("boss"); boss != nullptr && !boss->is_nil())
    {
        made->boss.enabled = true;
        if (boss->type() == ValueType::map)
        {
            made->boss.title = string_or(*boss, "title", creature.name);
            made->boss.color = string_or(*boss, "color", "#c83c32");
            made->boss.music = string_or(*boss, "music", "");
            made->boss.bar_range = clamped(*boss, "range", 64.0, 4.0, 512.0);
            made->boss.arena = clamped(*boss, "arena", 0.0, 0.0, 512.0);
            if (const Value *phases = boss->field("phases"); phases != nullptr && phases->type() == ValueType::array)
                for (const Value &entry : phases->as_array())
                {
                    BossPhase phase;
                    phase.below = clamped(entry, "below", 0.5, 0.0, 1.0);
                    phase.name = string_or(entry, "name", "");
                    phase.state = string_or(entry, "state", "");
                    phase.music = string_or(entry, "music", "");
                    phase.speed = clamped(entry, "speed", 1.0, 0.1, 10.0);
                    phase.damage = clamped(entry, "damage", 1.0, 0.0, 100.0);
                    made->boss.phases.push_back(std::move(phase));
                }
            std::ranges::sort(made->boss.phases, std::greater{}, &BossPhase::below);
        }
        else
            made->boss.title = creature.name;
        if (made->boss.arena > 0.0F && made->leash <= 0.0F)
            made->leash = made->boss.arena;
    }
    creature.ai = std::move(made);
}

void GameplayService::Impl::define_brain(Side side, std::string_view mod, std::string_view name,
                                         const Value &def)
{
    Registry &definitions = registry(side);
    BrainDef brain;
    brain.key = qualify(mod, name);
    brain.mod = std::string{mod};
    const Value *states = def.field("states");
    if (states == nullptr || states->type() != ValueType::map || states->as_map().empty())
        throw ScriptError{std::format("brain '{}' needs states = {{name = {{enter, tick, exit}}}}", name)};
    std::vector<Callback> kept;
    const auto take = [&](const Value &table, std::string_view key) {
        Callback callback = callback_or_none(table, key);
        if (callback.valid())
        {
            kept.push_back(callback);
            hold(side, callback);
        }
        return callback;
    };
    for (const auto &[state_name, entry] : states->as_map())
    {
        if (entry.type() != ValueType::map && entry.type() != ValueType::callback)
            throw ScriptError{std::format("brain '{}': state '{}' must be a table", name, state_name)};
        BrainState state;
        state.name = state_name;
        if (entry.type() == ValueType::callback)
        {
            // `state = function(entity, info) ... end` is its tick.
            state.tick = entry.as_callback();
            kept.push_back(state.tick);
            hold(side, state.tick);
        }
        else
        {
            state.enter = take(entry, "enter");
            state.tick = take(entry, "tick");
            state.exit = take(entry, "exit");
            state.damaged = take(entry, "damaged");
            state.heard = take(entry, "heard");
            state.noticed = take(entry, "noticed");
        }
        brain.states.push_back(std::move(state));
    }
    brain.initial = string_or(def, "initial", brain.states.front().name);
    if (brain.state(brain.initial) == nullptr)
        throw ScriptError{std::format("brain '{}': initial state '{}' is not one of its states", name, brain.initial)};
    release_unused(def, kept);
    definitions.brains.insert_or_assign(brain.key, std::move(brain));
}

const BrainDef *GameplayService::Impl::brain(std::string_view key, std::string_view mod) const noexcept
{
    if (key.empty())
        return nullptr;
    const auto found = server_registry.brains.find(key);
    if (found != server_registry.brains.end())
        return &found->second;
    const auto local = server_registry.brains.find(std::format("{}:{}", mod, key));
    return local == server_registry.brains.end() ? nullptr : &local->second;
}

// ------------------------------------------------------------------ minds

void GameplayService::Impl::ai_entity_spawned(Entity &body, const CreatureDef &creature, const Value &options)
{
    if (creature.ai == nullptr)
        return;
    EntityMind &mind = body.mind;
    mind.active = true;
    mind.home = body.position;
    mind.think_at = server_time + random01() / std::max(0.2F, creature.ai->think_rate);
    mind.last_think = server_time;
    mind.hover_phase = random01() * 6.2831853F;
    const BrainDef *definition = brain(creature.ai->brain, creature.mod);
    mind.state = definition != nullptr ? definition->initial : std::string{"idle"};
    mind.state_since = server_time;
    mind.state_entered = definition == nullptr;
    if (const Value *team = options.field("team"); team != nullptr && team->type() == ValueType::string)
        mind.team = team->as_string().substr(0, 64U);
    // A pack: options.pack names a member (or leader) to join.
    if (const Value *pack = options.field("pack"); pack != nullptr && pack->type() == ValueType::handle &&
                                                   pack->as_handle().kind == HandleKind::entity)
        if (Entity *other = entity(pack->as_handle().id); other != nullptr && other->id != body.id && other->mind.active)
            join_pack(body, *other);
    if (creature.ai->boss.enabled)
        mind.phase = -1;
}

void GameplayService::Impl::join_pack(Entity &body, Entity &other)
{
    if (other.mind.pack == 0U || !ai.packs.contains(other.mind.pack))
    {
        Pack created;
        created.id = ai.next_pack_id++;
        created.leader = other.id;
        created.members.push_back(other.id);
        created.health_at_start = other.max_health;
        other.mind.pack = created.id;
        ai.packs.emplace(created.id, std::move(created));
    }
    Pack &joined = ai.packs[other.mind.pack];
    if (std::ranges::find(joined.members, body.id) == joined.members.end())
    {
        joined.members.push_back(body.id);
        joined.health_at_start += body.max_health;
    }
    body.mind.pack = joined.id;
}

void GameplayService::Impl::ai_entity_removed(Entity &body, bool died)
{
    static_cast<void>(died);
    if (!body.mind.active)
        return;
    if (body.mind.pack != 0U)
        if (const auto pack = ai.packs.find(body.mind.pack); pack != ai.packs.end())
        {
            std::erase(pack->second.members, body.id);
            if (pack->second.members.empty())
                ai.packs.erase(pack);
            else if (pack->second.leader == body.id)
                pack->second.leader = pack->second.members.front();
        }
    // Whoever was after it forgets it.
    for (Entity &other : entities)
        if (other.mind.active)
        {
            std::erase_if(other.mind.memory, [&](const MemoryEntry &entry) { return entry.entity == body.id; });
            if (other.mind.target_entity == body.id)
                other.mind.target_entity = 0U;
            if (other.mind.attack_entity == body.id)
                other.mind.attack_entity = 0U;
        }
}

MemoryEntry &GameplayService::Impl::remember(EntityMind &mind, std::uint64_t player, std::uint64_t other)
{
    for (MemoryEntry &entry : mind.memory)
        if (entry.player == player && entry.entity == other)
            return entry;
    if (mind.memory.size() >= ai.limits.memory_entries)
    {
        // Forget the least threatening.
        const auto weakest = std::ranges::min_element(mind.memory, {}, &MemoryEntry::threat);
        mind.memory.erase(weakest);
    }
    MemoryEntry &entry = mind.memory.emplace_back();
    entry.player = player;
    entry.entity = other;
    return entry;
}

void GameplayService::Impl::ai_entity_damaged(std::uint64_t id, float amount, std::uint64_t attacker_player,
                                              std::uint64_t attacker_entity)
{
    Entity *body = entity(id);
    if (body == nullptr || !body->mind.active)
        return;
    const CreatureDef *creature = server_registry.creature(body->creature);
    if (creature == nullptr || creature->ai == nullptr)
        return;
    if (attacker_player != 0U || attacker_entity != 0U)
    {
        MemoryEntry &entry = remember(body->mind, attacker_player, attacker_entity);
        entry.threat += 2.0F + amount * 0.5F;
        entry.hostile = true; // anyone who hurts it is fair game
        entry.heard_at = server_time;
        if (const auto at = target_point(attacker_player, attacker_entity))
            entry.last_position = *at;
        if (body->mind.target_player == 0U && body->mind.target_entity == 0U && !body->mind.leashed)
            set_target(*body, attacker_player, attacker_entity, "damage");
    }
    // Boss phases at health thresholds.
    if (creature->ai->boss.enabled && body->max_health > 0.0F)
    {
        const float fraction = body->health / body->max_health;
        int phase = -1;
        for (std::size_t index{}; index < creature->ai->boss.phases.size(); ++index)
            if (fraction <= creature->ai->boss.phases[index].below)
                phase = static_cast<int>(index);
        while (body != nullptr && phase > body->mind.phase)
        {
            const int previous = body->mind.phase;
            const BossPhase &next = creature->ai->boss.phases[static_cast<std::size_t>(previous + 1)];
            body->mind.phase = previous + 1;
            body->mind.speed_scale = next.speed;
            body->mind.damage_scale = next.damage;
            emit(Side::server, "entity.phase_changed",
                 make_map({{"entity", Handle{HandleKind::entity, id}},
                           {"phase", body->mind.phase + 1},
                           {"name", next.name},
                           {"previous", previous + 1},
                           {"health", fraction}}));
            body = entity(id);
            if (body != nullptr && !next.state.empty())
            {
                body->mind.pending_state = next.state;
                if (const auto found = std::ranges::find(entities, id, &Entity::id); found != entities.end())
                    run_state_change(static_cast<std::size_t>(found - entities.begin()));
                body = entity(id);
            }
        }
    }
    // The brain's own say.
    if (body == nullptr)
        return;
    if (const BrainDef *definition = brain(creature->ai->brain, creature->mod))
        if (const BrainState *state = definition->state(body->mind.state); state != nullptr && state->damaged.valid())
        {
            Value info = mind_info(*body, *creature, 0.0F);
            if (info.type() == ValueType::map)
            {
                Map fields = info.as_map();
                fields.emplace_back("amount", amount);
                fields.emplace_back("attacker", target_value(attacker_player, attacker_entity));
                info = Value{std::move(fields)};
            }
            if (const auto found = std::ranges::find(entities, id, &Entity::id); found != entities.end())
                static_cast<void>(invoke_brain(static_cast<std::size_t>(found - entities.begin()), state->damaged,
                                               std::move(info), true));
        }
}

// ------------------------------------------------------------------ teams

std::string GameplayService::Impl::team_of_entity(const Entity &body) const
{
    if (!body.mind.team.empty())
        return body.mind.team;
    const CreatureDef *creature = server_registry.creature(body.creature);
    if (creature == nullptr)
        return "monsters";
    if (creature->ai != nullptr)
        return creature->ai->team;
    return creature->brain == Brain::hostile ? "monsters" : creature->brain == Brain::npc ? "villagers" : "animals";
}

std::string GameplayService::Impl::team_of_player(std::uint64_t serial) const
{
    const auto found = ai.player_teams.find(serial);
    return found != ai.player_teams.end() ? found->second : std::string{"players"};
}

Relation GameplayService::Impl::relation(std::string_view from_team, std::string_view to_team,
                                         const CreatureAi *from) const
{
    if (from_team == to_team)
        return Relation::friendly;
    if (const auto found = ai.relations.find(std::pair{std::string{from_team}, std::string{to_team}});
        found != ai.relations.end())
        return found->second;
    if (from != nullptr)
    {
        if (std::ranges::find(from->friends, to_team) != from->friends.end())
            return Relation::friendly;
        if (std::ranges::find(from->hostile_to, to_team) != from->hostile_to.end() ||
            std::ranges::find(from->hostile_to, "*") != from->hostile_to.end())
            return Relation::hostile;
    }
    return Relation::neutral;
}

bool GameplayService::Impl::hostile_to_player(const Entity &body, const CreatureDef &creature,
                                              std::uint64_t serial) const
{
    return relation(team_of_entity(body), team_of_player(serial), creature.ai.get()) == Relation::hostile;
}

bool GameplayService::Impl::hostile_to_entity(const Entity &body, const CreatureDef &creature,
                                              const Entity &other) const
{
    return relation(team_of_entity(body), team_of_entity(other), creature.ai.get()) == Relation::hostile;
}

std::optional<Vec3> GameplayService::Impl::target_point(std::uint64_t player, std::uint64_t other) const
{
    if (player != 0U)
    {
        for (const PlayerRecord &record : players)
            if (record.serial == player)
                return record.present && record.alive ? std::optional{record.position} : std::nullopt;
        return std::nullopt;
    }
    if (other != 0U)
        for (const Entity &body : entities)
            if (body.id == other)
                return body.removed ? std::nullopt : std::optional{body.position};
    return std::nullopt;
}

Vec3 GameplayService::Impl::eye_of(const Entity &body) const
{
    const CreatureDef *creature = server_registry.creature(body.creature);
    const double height = creature != nullptr ? creature->height : 1.8;
    return {body.position.x, body.position.y + height * 0.85, body.position.z};
}

// Voxel walk (Amanatides-Woo) from eye to eye: blocked by anything that
// blocks a player.
bool GameplayService::Impl::line_of_sight(Vec3 from, Vec3 to)
{
    ++ai.stats.line_checks;
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double dz = to.z - from.z;
    const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 1e-6)
        return true;
    int x = static_cast<int>(std::floor(from.x));
    int y = static_cast<int>(std::floor(from.y));
    int z = static_cast<int>(std::floor(from.z));
    const int end_x = static_cast<int>(std::floor(to.x));
    const int end_y = static_cast<int>(std::floor(to.y));
    const int end_z = static_cast<int>(std::floor(to.z));
    const int step_x = dx > 0 ? 1 : -1;
    const int step_y = dy > 0 ? 1 : -1;
    const int step_z = dz > 0 ? 1 : -1;
    const auto first = [](double origin, double delta, int cell) {
        if (std::abs(delta) < 1e-12)
            return std::numeric_limits<double>::infinity();
        const double boundary = delta > 0 ? cell + 1.0 : static_cast<double>(cell);
        return (boundary - origin) / delta;
    };
    double t_x = first(from.x, dx, x);
    double t_y = first(from.y, dy, y);
    double t_z = first(from.z, dz, z);
    const double d_x = std::abs(dx) < 1e-12 ? std::numeric_limits<double>::infinity() : 1.0 / std::abs(dx);
    const double d_y = std::abs(dy) < 1e-12 ? std::numeric_limits<double>::infinity() : 1.0 / std::abs(dy);
    const double d_z = std::abs(dz) < 1e-12 ? std::numeric_limits<double>::infinity() : 1.0 / std::abs(dz);
    for (int guard = 0; guard < 512; ++guard)
    {
        if (x == end_x && y == end_y && z == end_z)
            return true;
        if (t_x < t_y && t_x < t_z)
        {
            x += step_x;
            t_x += d_x;
        }
        else if (t_y < t_z)
        {
            y += step_y;
            t_y += d_y;
        }
        else
        {
            z += step_z;
            t_z += d_z;
        }
        if (std::min({t_x, t_y, t_z}) > 1.0 + 1e-9 && !(x == end_x && y == end_y && z == end_z))
            return true;
        if (!(x == end_x && y == end_y && z == end_z) && ai_solid(x, y, z))
            return false;
    }
    return true;
}

// ------------------------------------------------------------- perception

void GameplayService::Impl::perceive(Entity &body, const CreatureDef &creature)
{
    const CreatureAi &brain_def = *creature.ai;
    EntityMind &mind = body.mind;
    const Vec3 eye = eye_of(body);
    const double now = server_time;
    const double half_fov = std::cos(static_cast<double>(brain_def.fov) * std::numbers::pi / 360.0);
    const double forward_x = std::sin(static_cast<double>(body.yaw));
    const double forward_z = -std::cos(static_cast<double>(body.yaw));
    const double sight_squared = static_cast<double>(brain_def.sight) * brain_def.sight;
    for (MemoryEntry &entry : mind.memory)
        entry.visible = false;
    const auto sense = [&](std::uint64_t player, std::uint64_t other, Vec3 feet, double height, bool hostile) {
        const Vec3 target{feet.x, feet.y + height * 0.8, feet.z};
        const double dx = target.x - eye.x;
        const double dz = target.z - eye.z;
        const double distance_squared_value = dx * dx + (target.y - eye.y) * (target.y - eye.y) + dz * dz;
        if (distance_squared_value > sight_squared)
            return;
        const double flat = std::sqrt(dx * dx + dz * dz);
        const bool close = distance_squared_value < 6.25;
        if (!close && brain_def.fov < 359.0F && flat > 1e-6 &&
            (dx * forward_x + dz * forward_z) / flat < half_fov)
            return;
        if (!brain_def.xray && !line_of_sight(eye, target))
            return;
        MemoryEntry &entry = remember(mind, player, other);
        const bool fresh = now - std::max(entry.seen_at, entry.heard_at) > brain_def.memory || entry.noticed_at < 0.0;
        entry.visible = true;
        entry.seen_at = now;
        entry.last_position = feet;
        entry.hostile = entry.hostile || hostile;
        const double distance = std::sqrt(distance_squared_value);
        entry.threat = std::max(entry.threat, static_cast<float>(1.0 + (brain_def.sight - distance) / std::max(1.0F, brain_def.sight)));
        if (fresh)
            entry.noticed_at = -now - 1.0; // marked: announced after the scan
    };
    for (const PlayerRecord &player : players)
    {
        if (!player.present || !player.alive)
            continue;
        const bool hostile = hostile_to_player(body, creature, player.serial);
        if (!hostile && std::ranges::none_of(mind.memory, [&](const MemoryEntry &entry) {
                return entry.player == player.serial;
            }))
            continue;
        sense(player.serial, 0U, player.position, 1.8, hostile);
    }
    // Creatures nearby, from this step's grid.
    const int cells = static_cast<int>(std::ceil(brain_def.sight / grid_cell));
    for (int gz = -cells; gz <= cells; ++gz)
        for (int gx = -cells; gx <= cells; ++gx)
        {
            const auto bucket = ai.grid.find(grid_key(body.position.x + gx * grid_cell, body.position.z + gz * grid_cell));
            if (bucket == ai.grid.end())
                continue;
            for (const std::size_t index : bucket->second)
            {
                if (index >= entities.size())
                    continue;
                const Entity &other = entities[index];
                if (other.removed || other.id == body.id)
                    continue;
                const CreatureDef *other_creature = server_registry.creature(other.creature);
                if (other_creature == nullptr)
                    continue;
                const bool hostile = hostile_to_entity(body, creature, other);
                if (!hostile && std::ranges::none_of(mind.memory, [&](const MemoryEntry &entry) {
                        return entry.entity == other.id;
                    }))
                    continue;
                sense(0U, other.id, other.position, other_creature->height, hostile);
            }
        }
    // Hearing: noises since the last thought.
    for (const Noise &noise : ai.noises)
    {
        if (noise.at <= mind.last_think || brain_def.hearing <= 0.0F)
            continue;
        const double reach = static_cast<double>(brain_def.hearing) * noise.loudness / 16.0;
        if (distance_squared(noise.position, body.position) > reach * reach)
            continue;
        mind.heard_position = noise.position;
        mind.heard_until = now + std::max(2.0F, brain_def.memory);
        if (noise.player != 0U || noise.entity != 0U)
        {
            MemoryEntry &entry = remember(mind, noise.player, noise.entity);
            entry.heard_at = now;
            entry.last_position = noise.position;
            if (noise.player != 0U)
                entry.hostile = entry.hostile || hostile_to_player(body, creature, noise.player);
            entry.threat = std::max(entry.threat, 0.75F);
        }
    }
    // Forget, and let threat fade.
    const float dt = static_cast<float>(std::max(0.0, now - mind.last_think));
    for (MemoryEntry &entry : mind.memory)
        entry.threat *= static_cast<float>(std::exp(-memory_threat_decay * dt));
    std::erase_if(mind.memory, [&](const MemoryEntry &entry) {
        if (now - std::max(entry.seen_at, entry.heard_at) > brain_def.memory)
            return true;
        return !target_point(entry.player, entry.entity).has_value();
    });
}

// ------------------------------------------------------------ the brain

void GameplayService::Impl::set_target(Entity &body, std::uint64_t player, std::uint64_t other,
                                       std::string_view why)
{
    EntityMind &mind = body.mind;
    if (mind.target_player == player && mind.target_entity == other)
        return;
    const Value previous = target_value(mind.target_player, mind.target_entity);
    mind.target_player = player;
    mind.target_entity = other;
    mind.route.clear();
    const std::uint64_t id = body.id;
    const std::uint64_t pack = mind.pack;
    emit(Side::server, "entity.target_changed",
         make_map({{"entity", Handle{HandleKind::entity, id}},
                   {"target", target_value(player, other)},
                   {"previous", previous},
                   {"reason", std::string{why}}}));
    // The pack hears of it.
    if (pack == 0U || (player == 0U && other == 0U))
        return;
    const auto found = ai.packs.find(pack);
    if (found == ai.packs.end())
        return;
    const std::optional<Vec3> where = target_point(player, other);
    for (const std::uint64_t member : found->second.members)
        if (Entity *mate = entity(member); mate != nullptr && member != id && mate->mind.active)
        {
            MemoryEntry &entry = remember(mate->mind, player, other);
            entry.hostile = true;
            entry.heard_at = server_time;
            entry.threat = std::max(entry.threat, 0.9F);
            if (where.has_value())
                entry.last_position = *where;
        }
}

Value GameplayService::Impl::mind_info(const Entity &body, const CreatureDef &creature, float dt)
{
    const EntityMind &mind = body.mind;
    const std::optional<Vec3> target = target_point(mind.target_player, mind.target_entity);
    bool sees{};
    for (const MemoryEntry &entry : mind.memory)
        if (entry.player == mind.target_player && entry.entity == mind.target_entity && entry.visible)
            sees = true;
    std::size_t pack_size = 1U;
    float pack_hurt{};
    bool leader = true;
    if (mind.pack != 0U)
        if (const auto found = ai.packs.find(mind.pack); found != ai.packs.end())
        {
            pack_size = found->second.members.size();
            leader = found->second.leader == body.id;
            float health{};
            for (const std::uint64_t member : found->second.members)
                for (const Entity &mate : entities)
                    if (mate.id == member && !mate.removed)
                        health += mate.health;
            if (found->second.health_at_start > 0.0F)
                pack_hurt = std::clamp(1.0F - health / found->second.health_at_start, 0.0F, 1.0F);
        }
    Map info{{"dt", Value{dt}},
             {"state", Value{mind.state}},
             {"time", Value{server_time - mind.state_since}},
             {"target", target_value(mind.target_player, mind.target_entity)},
             {"sees_target", Value{sees}},
             {"health", Value{body.health}},
             {"max_health", Value{body.max_health}},
             {"health_fraction", Value{body.max_health > 0.0F ? body.health / body.max_health : 1.0F}},
             {"position", vec3_value(body.position)},
             {"home", vec3_value(mind.home)},
             {"home_distance", Value{std::sqrt(distance_squared(body.position, mind.home))}},
             {"pack_size", Value{static_cast<std::int64_t>(pack_size)}},
             {"pack_hurt", Value{pack_hurt}},
             {"leader", Value{leader}},
             {"phase", Value{mind.phase + 1}},
             {"attacking", mind.attack >= 0 ? Value{true} : Value{false}},
             {"known", Value{static_cast<std::int64_t>(mind.memory.size())}},
             {"data", body.data}};
    if (target.has_value())
    {
        info.emplace_back("target_position", vec3_value(*target));
        info.emplace_back("target_distance", std::sqrt(distance_squared(*target, body.position)));
    }
    if (mind.heard_until > server_time)
        info.emplace_back("heard", vec3_value(mind.heard_position));
    static_cast<void>(creature);
    return Value{std::move(info)};
}

bool GameplayService::Impl::invoke_brain(std::size_t index, const Callback &callback, Value info,
                                         bool takes_state)
{
    const std::uint64_t id = entities[index].id;
    const std::array<Value, 2> args{Value{Handle{HandleKind::entity, id}}, std::move(info)};
    const Value result = host.invoke(callback, args);
    Entity *body = entity(id);
    if (body == nullptr)
        return false;
    if (takes_state && result.type() == ValueType::string && result.as_string() != body->mind.state)
        body->mind.pending_state = result.as_string();
    return true;
}

void GameplayService::Impl::set_state(Entity &body, std::string_view state)
{
    if (state != body.mind.state)
        body.mind.pending_state = std::string{state};
}

void GameplayService::Impl::run_state_change(std::size_t index)
{
    for (int guard = 0; guard < 4; ++guard)
    {
        if (index >= entities.size() || entities[index].removed)
            return;
        Entity &body = entities[index];
        EntityMind &mind = body.mind;
        const bool enter_initial = !mind.state_entered;
        if (mind.pending_state.empty() && !enter_initial)
            return;
        const CreatureDef *creature = server_registry.creature(body.creature);
        if (creature == nullptr || creature->ai == nullptr)
            return;
        const BrainDef *definition = brain(creature->ai->brain, creature->mod);
        const std::uint64_t id = body.id;
        std::string next = enter_initial && mind.pending_state.empty() ? mind.state : mind.pending_state;
        mind.pending_state.clear();
        if (definition != nullptr && definition->state(next) == nullptr)
        {
            log(creature->mod, LogLevel::warning,
                std::format("brain '{}' has no state '{}'", definition->key, next));
            return;
        }
        const std::string previous = mind.state;
        if (!enter_initial && definition != nullptr)
            if (const BrainState *old = definition->state(previous); old != nullptr && old->exit.valid())
                if (!invoke_brain(index, old->exit, mind_info(body, *creature, 0.0F), false))
                    return;
        if (index >= entities.size() || entities[index].id != id)
            return;
        Entity &moved = entities[index];
        moved.mind.state = next;
        moved.mind.state_since = server_time;
        moved.mind.state_entered = true;
        if (!enter_initial || previous != next)
            emit(Side::server, "entity.state_changed",
                 make_map({{"entity", Handle{HandleKind::entity, id}}, {"state", next}, {"previous", previous}}));
        if (index >= entities.size() || entities[index].id != id || entities[index].removed)
            return;
        if (definition != nullptr)
            if (const BrainState *entered = definition->state(next); entered != nullptr && entered->enter.valid())
                if (!invoke_brain(index, entered->enter, mind_info(entities[index], *creature, 0.0F), true))
                    return;
    }
}

void GameplayService::Impl::think(std::size_t index)
{
    Entity &body = entities[index];
    const CreatureDef *creature = server_registry.creature(body.creature);
    if (creature == nullptr || creature->ai == nullptr)
        return;
    const std::shared_ptr<const CreatureAi> held = creature->ai; // alive through any redefinition
    const CreatureAi &brain_def = *held;
    const std::uint64_t id = body.id;
    const float dt = static_cast<float>(std::max(0.0, server_time - body.mind.last_think));
    perceive(body, *creature);
    body.mind.last_think = server_time;
    ++ai.stats.thinks;

    // The leash: too far from home, it goes back and forgets its target.
    if (brain_def.leash > 0.0F && !body.mind.leashed &&
        horizontal_squared(body.position, body.mind.home) > static_cast<double>(brain_def.leash) * brain_def.leash)
    {
        body.mind.leashed = true;
        set_target(body, 0U, 0U, "leash");
        if (index >= entities.size() || entities[index].id != id)
            return;
    }
    Entity &self = entities[index];
    EntityMind &mind = self.mind;
    // The target: the most threatening hostile it knows of.
    if (!mind.leashed)
    {
        const MemoryEntry *best{};
        const MemoryEntry *current{};
        for (const MemoryEntry &entry : mind.memory)
        {
            if (!entry.hostile)
                continue;
            if (entry.player == mind.target_player && entry.entity == mind.target_entity)
                current = &entry;
            if (best == nullptr || entry.threat > best->threat)
                best = &entry;
        }
        const bool target_known = mind.target_player != 0U || mind.target_entity != 0U;
        if (target_known && current == nullptr)
            set_target(self, best != nullptr ? best->player : 0U, best != nullptr ? best->entity : 0U,
                       best != nullptr ? "threat" : "lost");
        else if (best != nullptr && (current == nullptr || best->threat > current->threat * 1.35F + 0.25F))
            set_target(self, best->player, best->entity, current == nullptr ? "noticed" : "threat");
    }
    if (index >= entities.size() || entities[index].id != id)
        return;
    // Announce what it newly noticed (marked by perceive).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> noticed;
    for (MemoryEntry &entry : entities[index].mind.memory)
        if (entry.noticed_at < 0.0 && entry.noticed_at > -1.0e8)
        {
            entry.noticed_at = server_time;
            noticed.emplace_back(entry.player, entry.entity);
        }
    for (const auto &[player, other] : noticed)
    {
        emit(Side::server, "entity.noticed",
             make_map({{"entity", Handle{HandleKind::entity, id}}, {"target", target_value(player, other)}}));
        if (index >= entities.size() || entities[index].id != id)
            return;
    }

    const BrainDef *definition = brain(brain_def.brain, creature->mod);
    if (definition == nullptr)
    {
        hunt(entities[index], *creature);
        run_state_change(index); // the hunter's states, with entity.state_changed
        return;
    }
    run_state_change(index); // the initial state's enter, or a pending switch
    if (index >= entities.size() || entities[index].id != id || entities[index].removed)
        return;
    const BrainState *state = definition->state(entities[index].mind.state);
    if (state == nullptr)
        return;
    for (const auto &[player, other] : noticed)
        if (state->noticed.valid())
        {
            Value info = mind_info(entities[index], *creature, dt);
            Map fields = info.as_map();
            fields.emplace_back("noticed", target_value(player, other));
            if (!invoke_brain(index, state->noticed, Value{std::move(fields)}, true))
                return;
        }
    if (state->heard.valid() && entities[index].mind.heard_until > server_time &&
        entities[index].mind.heard_until - std::max(2.0F, brain_def.memory) >= server_time - 1e-9)
    {
        Value info = mind_info(entities[index], *creature, dt);
        Map fields = info.as_map();
        fields.emplace_back("noise", vec3_value(entities[index].mind.heard_position));
        if (!invoke_brain(index, state->heard, Value{std::move(fields)}, true))
            return;
    }
    if (state->tick.valid() && !invoke_brain(index, state->tick, mind_info(entities[index], *creature, dt), true))
        return;
    run_state_change(index);
}

// The built-in hunter: chases the target and uses whichever attack fits;
// returns home past its leash, investigates what it heard, wanders, and flees
// when hurt (flee_health) or when its pack has lost enough (pack_flee).
void GameplayService::Impl::hunt(Entity &body, const CreatureDef &creature)
{
    const CreatureAi &brain_def = *creature.ai;
    EntityMind &mind = body.mind;
    const double now = server_time;
    const auto change = [&](std::string_view state) {
        if (mind.state != state)
            mind.pending_state = std::string{state};
    };
    if (mind.leashed)
    {
        mind.goal = {GoalKind::home};
        mind.goal.point = mind.home;
        mind.goal.run = true;
        change("return");
        if (horizontal_squared(body.position, mind.home) < 4.0)
        {
            mind.leashed = false;
            if (brain_def.boss.enabled)
            {
                // A boss that lost its fight resets.
                body.health = body.max_health;
                mind.phase = -1;
                mind.speed_scale = 1.0F;
                mind.damage_scale = 1.0F;
            }
            mind.goal = {};
            change("idle");
        }
    }
    else if (const std::optional<Vec3> target = target_point(mind.target_player, mind.target_entity))
    {
        const double distance = std::sqrt(distance_squared(*target, body.position));
        float pack_hurt{};
        std::size_t pack_size = 1U;
        if (mind.pack != 0U)
            if (const auto found = ai.packs.find(mind.pack); found != ai.packs.end())
            {
                pack_size = found->second.members.size();
                float health{};
                for (const std::uint64_t member : found->second.members)
                    if (const Entity *mate = entity(member))
                        health += mate->health;
                if (found->second.health_at_start > 0.0F)
                    pack_hurt = 1.0F - health / found->second.health_at_start;
            }
        const float fraction = body.max_health > 0.0F ? body.health / body.max_health : 1.0F;
        if ((brain_def.flee_health > 0.0F && fraction < brain_def.flee_health) ||
            (brain_def.pack_flee > 0.0F && pack_hurt >= brain_def.pack_flee) || mind.state == "flee")
        {
            if (mind.state != "flee")
            {
                change("flee");
                mind.goal = {GoalKind::flee, mind.target_player, mind.target_entity, {}, 24.0F};
                mind.goal.until = now + 5.0;
            }
            else if (now > mind.goal.until)
            {
                mind.goal = {};
                change("idle");
                set_target(body, 0U, 0U, "fled"); // last: its handlers may change the entity list
            }
            return;
        }
        change("hunt");
        // A pack spreads around its prey first: a member still on its way to
        // its place in the ring does not stop to strike.
        const bool surrounding = brain_def.surround && pack_size >= 3U;
        bool in_place = true;
        if (surrounding)
        {
            float reach{};
            for (const AttackDef &attack : brain_def.attacks)
                if (attack.kind == AttackKind::melee)
                    reach = std::max(reach, attack.reach);
            if (const auto slot = surround_slot(body, *target, std::max(1.2F, reach * 0.8F)))
                in_place = horizontal_squared(*slot, body.position) < 1.0;
        }
        // An attack that fits now: ranged ones need a clear shot.
        std::vector<std::pair<int, float>> ready;
        bool has_ranged{};
        float melee_reach{};
        for (std::size_t index{}; index < brain_def.attacks.size(); ++index)
        {
            const AttackDef &attack = brain_def.attacks[index];
            if (attack.kind == AttackKind::ranged)
                has_ranged = true;
            else
                melee_reach = std::max(melee_reach, attack.reach);
            const auto ready_at = mind.ready_at.find(attack.name);
            if (ready_at != mind.ready_at.end() && ready_at->second > now)
                continue;
            if (attack.kind == AttackKind::melee && (distance > attack.reach + 0.3 || !in_place))
                continue;
            if (attack.kind == AttackKind::ranged && (distance > attack.reach || distance < attack.min_range))
                continue;
            ready.emplace_back(static_cast<int>(index), attack.weight);
        }
        if (mind.attack < 0 && !ready.empty())
        {
            float total{};
            for (const auto &[index, weight] : ready)
                total += weight;
            float roll = random01() * total;
            int chosen = ready.front().first;
            for (const auto &[index, weight] : ready)
            {
                if (roll <= weight)
                {
                    chosen = index;
                    break;
                }
                roll -= weight;
            }
            const AttackDef &attack = brain_def.attacks[static_cast<std::size_t>(chosen)];
            const bool clear = attack.kind == AttackKind::melee ||
                               line_of_sight(eye_of(body), {target->x, target->y + 1.2, target->z});
            if (clear)
            {
                const auto found = std::ranges::find(entities, body.id, &Entity::id);
                if (found != entities.end())
                    static_cast<void>(start_attack(static_cast<std::size_t>(found - entities.begin()), attack.name,
                                                   mind.target_player, mind.target_entity, false));
            }
        }
        // Where to stand.
        if (has_ranged && (melee_reach <= 0.0F || distance > melee_reach * 2.5F))
        {
            float keep_near{};
            float keep_far{};
            for (const AttackDef &attack : brain_def.attacks)
                if (attack.kind == AttackKind::ranged)
                {
                    keep_near = std::max(keep_near, attack.min_range + 1.5F);
                    keep_far = std::max(keep_far, attack.reach * 0.75F);
                }
            mind.goal = {GoalKind::keep, mind.target_player, mind.target_entity, {}, keep_near, std::max(keep_near + 1.0F, keep_far)};
        }
        else if (surrounding)
            mind.goal = {GoalKind::surround, mind.target_player, mind.target_entity, {},
                         std::max(1.2F, melee_reach * 0.8F)};
        else
            mind.goal = {GoalKind::chase, mind.target_player, mind.target_entity, {},
                         std::max(0.8F, melee_reach * 0.7F)};
    }
    else if (mind.heard_until > now)
    {
        change("investigate");
        mind.goal = {GoalKind::point};
        mind.goal.point = mind.heard_position;
        mind.goal.run = false;
        if (horizontal_squared(body.position, mind.heard_position) < 4.0)
            mind.heard_until = 0.0;
    }
    else
    {
        change("idle");
        const bool far_from_home = horizontal_squared(body.position, mind.home) > 24.0 * 24.0;
        if (mind.goal.kind != GoalKind::wander || horizontal_squared(body.position, mind.goal.point) < 1.0 ||
            now > mind.goal.until)
        {
            if (random01() < 0.3F || far_from_home)
            {
                const Vec3 around = far_from_home ? mind.home : body.position;
                const float angle = random01() * 6.2831853F;
                const float reach = 3.0F + random01() * 6.0F;
                mind.goal = {GoalKind::wander};
                mind.goal.point = {around.x + std::cos(angle) * reach, body.position.y, around.z + std::sin(angle) * reach};
                if (brain_def.movement == MoveMode::fly)
                    mind.goal.point.y = mind.home.y + random01() * 4.0;
                if (brain_def.movement == MoveMode::swim)
                    mind.goal.point.y = body.position.y + (random01() - 0.5) * 2.0;
                mind.goal.run = false;
                mind.goal.until = now + 10.0;
            }
            else
                mind.goal = {};
        }
    }
}

// ---------------------------------------------------------------- attacks

bool GameplayService::Impl::start_attack(std::size_t index, std::string_view name, std::uint64_t player,
                                         std::uint64_t other, bool ability)
{
    Entity &body = entities[index];
    const CreatureDef *creature = server_registry.creature(body.creature);
    if (creature == nullptr || creature->ai == nullptr || body.removed)
        return false;
    EntityMind &mind = body.mind;
    if (mind.attack >= 0)
        return false;
    const std::shared_ptr<const CreatureAi> held = creature->ai; // alive through any redefinition
    const CreatureAi &brain_def = *held;
    const auto ready = mind.ready_at.find(name);
    if (ready != mind.ready_at.end() && ready->second > server_time)
        return false;
    const std::optional<Vec3> target = target_point(player, other);
    int position{-1};
    float windup{};
    float cooldown{};
    float recovery{};
    if (ability)
    {
        const AbilityDef *definition = brain_def.ability(name);
        if (definition == nullptr)
            return false;
        if (target.has_value() && distance_squared(*target, body.position) >
                                      static_cast<double>(definition->range) * definition->range)
            return false;
        position = static_cast<int>(definition - brain_def.abilities.data());
        windup = definition->windup;
        cooldown = definition->cooldown;
        recovery = 0.1F;
    }
    else
    {
        const AttackDef *definition = brain_def.attack(name);
        if (definition == nullptr || !target.has_value())
            return false;
        const double distance = std::sqrt(distance_squared(*target, body.position));
        if (definition->kind == AttackKind::melee ? distance > definition->reach + 0.6
                                                  : distance > definition->reach || distance < definition->min_range)
            return false;
        position = static_cast<int>(definition - brain_def.attacks.data());
        windup = definition->windup;
        cooldown = definition->cooldown;
        recovery = definition->recovery;
    }
    mind.attack = position;
    mind.attack_is_ability = ability;
    mind.attack_left = windup;
    mind.recovery_left = recovery;
    mind.attack_player = player;
    mind.attack_entity = other;
    mind.attack_point = target.value_or(body.position);
    mind.ready_at[std::string{name}] = server_time + cooldown;
    mind.urgent = true;
    if (target.has_value())
        body.yaw = yaw_to(body.position, *target);
    // Even without a windup the strike comes at the next step: nothing a
    // handler does to the entity list can pull the creature from under the
    // caller.
    return true;
}

void GameplayService::Impl::advance_attack(std::size_t index, float dt)
{
    Entity &body = entities[index];
    EntityMind &mind = body.mind;
    if (mind.attack < 0)
        return;
    const CreatureDef *creature = server_registry.creature(body.creature);
    if (creature == nullptr || creature->ai == nullptr)
    {
        mind.attack = -1;
        return;
    }
    const std::shared_ptr<const CreatureAi> held = creature->ai; // alive through any redefinition
    const CreatureAi &brain_def = *held;
    const std::optional<Vec3> target = target_point(mind.attack_player, mind.attack_entity);
    if (mind.attack_left > 0.0F)
    {
        // Melee tracks its target through the windup; the strike decides.
        if (target.has_value() && !mind.attack_is_ability)
            body.yaw = yaw_to(body.position, *target);
        mind.attack_left -= dt;
        if (mind.attack_left > 0.0F)
            return;
        mind.attack_left = 0.0F;
        // The damage moment.
        const std::uint64_t id = body.id;
        const std::string name = mind.attack_is_ability
                                     ? brain_def.abilities[static_cast<std::size_t>(mind.attack)].name
                                     : brain_def.attacks[static_cast<std::size_t>(mind.attack)].name;
        const bool allowed = emit(Side::server, "entity.attacking",
                                  make_map({{"entity", Handle{HandleKind::entity, id}},
                                            {"attack", name},
                                            {"target", target_value(mind.attack_player, mind.attack_entity)}}));
        Entity *striker = entity(id);
        if (striker == nullptr)
            return;
        EntityMind &live = striker->mind;
        if (!allowed || live.attack < 0)
            return;
        if (live.attack_is_ability)
        {
            const AbilityDef &ability = brain_def.abilities[static_cast<std::size_t>(live.attack)];
            if (ability.run.valid())
            {
                const std::array<Value, 2> args{Value{Handle{HandleKind::entity, id}},
                                                target_value(live.attack_player, live.attack_entity)};
                static_cast<void>(host.invoke(ability.run, args));
            }
            return;
        }
        const AttackDef &attack = brain_def.attacks[static_cast<std::size_t>(live.attack)];
        const std::optional<Vec3> now_at = target_point(live.attack_player, live.attack_entity);
        if (attack.kind == AttackKind::melee)
        {
            if (!now_at.has_value())
                return;
            const double dx = now_at->x - striker->position.x;
            const double dz = now_at->z - striker->position.z;
            const double flat = std::sqrt(dx * dx + dz * dz);
            const double dy = now_at->y - striker->position.y;
            if (flat > attack.reach + 0.4 || std::abs(dy) > 2.5)
                return; // it dodged
            if (flat > 0.3 && attack.arc < 359.0F)
            {
                const double forward_x = std::sin(static_cast<double>(striker->yaw));
                const double forward_z = -std::cos(static_cast<double>(striker->yaw));
                if ((dx * forward_x + dz * forward_z) / flat < std::cos(attack.arc * std::numbers::pi / 360.0))
                    return;
            }
            const Vec3 from = striker->position;
            const float scale = live.damage_scale;
            if (live.attack_player != 0U)
            {
                if (PlayerRecord *victim = player_by_serial(live.attack_player))
                    damage(*victim, attack.damage * scale, DamageCause::creature, from);
            }
            else if (Entity *victim = entity(live.attack_entity))
                damage_entity(*victim,
                              (attack.creature_damage >= 0.0F ? attack.creature_damage : attack.damage / 10.0F) * scale,
                              0U, id);
            return;
        }
        // Ranged: one or more projectiles at where the target will be.
        const ProjectileDef *projectile = projectile_def(attack.projectile, creature->mod);
        if (projectile == nullptr)
        {
            log(creature->mod, LogLevel::warning, std::format("attack '{}': no projectile '{}'", attack.name, attack.projectile));
            return;
        }
        const Vec3 aim = now_at.value_or(live.attack_point);
        Vec3 target_velocity{};
        if (live.attack_player != 0U)
        {
            // Players' velocity is not tracked here; lead by the last motion.
            static_cast<void>(target_velocity);
        }
        else if (const Entity *victim = entity(live.attack_entity))
            target_velocity = victim->velocity;
        const Vec3 muzzle = eye_of(*striker);
        const Vec3 aim_point{aim.x, aim.y + (live.attack_player != 0U ? 1.3 : 0.8), aim.z};
        for (int shot = 0; shot < attack.count; ++shot)
        {
            std::optional<Vec3> velocity = aim_velocity(muzzle, aim_point, target_velocity, *projectile, attack.lead);
            if (!velocity.has_value())
                break;
            if (attack.spread > 0.0F)
            {
                const double spread = attack.spread * std::numbers::pi / 180.0;
                const double yaw = (random01() - 0.5) * 2.0 * spread;
                const double pitch = (random01() - 0.5) * 2.0 * spread;
                const double cos_yaw = std::cos(yaw);
                const double sin_yaw = std::sin(yaw);
                Vec3 turned{velocity->x * cos_yaw - velocity->z * sin_yaw, velocity->y,
                            velocity->x * sin_yaw + velocity->z * cos_yaw};
                const double speed = std::sqrt(distance_squared(turned, {}));
                turned.y += speed * std::sin(pitch);
                velocity = turned;
            }
            Entity *shooter = entity(id);
            if (shooter == nullptr)
                return;
            static_cast<void>(launch_projectile(*projectile, muzzle, *velocity, id, 0U, shooter->mind.attack_player,
                                                shooter->mind.attack_entity, shooter->mind.damage_scale));
        }
        return;
    }
    mind.recovery_left -= dt;
    if (mind.recovery_left <= 0.0F)
        mind.attack = -1;
}

// ----------------------------------------------------------------- bosses

void GameplayService::Impl::tick_bosses()
{
    std::map<std::uint64_t, bool> alive;
    for (const Entity &body : entities)
    {
        if (body.removed || !body.mind.active)
            continue;
        const CreatureDef *creature = server_registry.creature(body.creature);
        if (creature == nullptr || creature->ai == nullptr || !creature->ai->boss.enabled)
            continue;
        const BossDef &boss = creature->ai->boss;
        alive[body.id] = true;
        const float fraction = body.max_health > 0.0F ? std::clamp(body.health / body.max_health, 0.0F, 1.0F) : 1.0F;
        std::string phase_name;
        std::string music = boss.music;
        if (body.mind.phase >= 0 && static_cast<std::size_t>(body.mind.phase) < boss.phases.size())
        {
            const BossPhase &phase = boss.phases[static_cast<std::size_t>(body.mind.phase)];
            phase_name = phase.name;
            if (!phase.music.empty())
                music = phase.music;
        }
        const std::string title = body.name.empty() ? boss.title : body.name;
        // Position to a block: the bar shows by distance, not to the inch.
        const std::string signature =
            std::format("{}|{:.3f}|{}|{}|{:.0f}|{:.0f}|{:.0f}", title, fraction, phase_name, music,
                        body.position.x, body.position.y, body.position.z);
        std::string &sent = ai.boss_sent[body.id];
        if (sent == signature)
            continue;
        const bool reliable = sent.empty() || sent.substr(0, sent.find('|')) != title ||
                              sent.find(std::format("|{}|{}|", phase_name, music)) == std::string::npos;
        sent = signature;
        out().to_all(make_map({{"t", "boss"},
                               {"id", static_cast<std::int64_t>(body.id)},
                               {"mod", creature->mod},
                               {"title", title},
                               {"color", boss.color},
                               {"hp", fraction},
                               {"phase", phase_name},
                               {"music", music},
                               {"p", vec3_value({static_cast<float>(body.position.x), static_cast<float>(body.position.y),
                                                 static_cast<float>(body.position.z)})},
                               {"range", boss.bar_range}}),
                     reliable);
    }
    for (auto it = ai.boss_sent.begin(); it != ai.boss_sent.end();)
    {
        if (alive.contains(it->first))
        {
            ++it;
            continue;
        }
        out().to_all(make_map({{"t", "boss"}, {"id", static_cast<std::int64_t>(it->first)}, {"gone", true}}), true);
        it = ai.boss_sent.erase(it);
    }
}

// -------------------------------------------------------------- the step

void GameplayService::Impl::ai_begin_step()
{
    ai.thinks_left = ai.limits.thinks_per_step;
    ai.nodes_left = ai.limits.path_nodes_per_step;
    ++ai.pass;
    ai.solid_cache.clear();
    ai.grid.clear();
    for (std::size_t index{}; index < entities.size(); ++index)
        if (!entities[index].removed)
            ai.grid[grid_key(entities[index].position.x, entities[index].position.z)].push_back(index);
}

void GameplayService::Impl::tick_ai_entity(std::size_t index, float dt, double nearest_squared)
{
    const auto started = std::chrono::steady_clock::now();
    Entity &body = entities[index];
    const std::uint64_t id = body.id;
    const CreatureDef *creature = server_registry.creature(body.creature);
    if (creature == nullptr || creature->ai == nullptr)
        return;
    body.hurt_timer = std::max(0.0F, body.hurt_timer - dt);
    // Think less often far from every player.
    const double nearest = std::sqrt(nearest_squared);
    body.mind.lod = nearest < 32.0 ? 1U : nearest < 64.0 ? 2U : nearest < 96.0 ? 4U : 8U;
    // An attack under way first: one this step's thought starts has all of
    // its windup still ahead of it (its clip starts on clients now).
    advance_attack(index, dt);
    if (index >= entities.size() || entities[index].id != id || entities[index].removed)
        return;
    if (server_time >= entities[index].mind.think_at)
    {
        if (ai.thinks_left > 0U)
        {
            --ai.thinks_left;
            const double interval =
                static_cast<double>(entities[index].mind.lod) / std::max(0.2F, creature->ai->think_rate);
            entities[index].mind.think_at = server_time + interval * (0.9 + 0.2 * random01());
            const auto thought = std::chrono::steady_clock::now();
            think(index);
            ai.stats.think_ms +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - thought).count();
        }
        else
            ++ai.stats.deferred_thinks;
    }
    if (index >= entities.size() || entities[index].id != id || entities[index].removed)
        return;
    move_ai_entity(index, dt);
    ai.stats.step_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void GameplayService::Impl::tick_ai_world(float seconds)
{
    // Noises are heard by the next thought of everything in range.
    while (!ai.noises.empty() && server_time - ai.noises.front().at > 2.0)
        ai.noises.pop_front();
    tick_projectiles(seconds);
    ai.boss_timer -= seconds;
    if (ai.boss_timer <= 0.0F)
    {
        ai.boss_timer = 0.2F;
        tick_bosses();
    }
    if (server_time - ai.cache_swept_at > 2.0)
    {
        ai.cache_swept_at = server_time;
        std::erase_if(ai.routes, [&](const auto &entry) { return server_time - entry.second.made_at > 3.0; });
        std::erase_if(ai.fields, [&](const auto &entry) { return server_time - entry.second.made_at > 3.0; });
        if (ai.water_columns.size() > 65'536U)
            ai.water_columns.clear();
    }
    if (ai.retail_dirty)
    {
        ai.retail_dirty = false;
        out().to_all(retail_rules_value(ai.retail), true);
    }
}

void GameplayService::Impl::ai_block_changed(BlockPos cell)
{
    // Routes and fields through a changed cell are planned again.
    if (ai.routes.empty() && ai.fields.empty())
        return;
    std::erase_if(ai.fields, [&](const auto &entry) {
        return std::abs(entry.second.origin.x - cell.x) < 48 && std::abs(entry.second.origin.z - cell.z) < 48;
    });
    std::erase_if(ai.routes, [&](const auto &entry) {
        return std::ranges::any_of(entry.second.route, [&](const BlockPos &step) {
            return std::abs(step.x - cell.x) <= 1 && std::abs(step.z - cell.z) <= 1 && std::abs(step.y - cell.y) <= 3;
        });
    });
}

void GameplayService::Impl::ai_world_edit(BlockPos cell, std::uint64_t player, float loudness)
{
    ai_block_changed(cell);
    if (loudness <= 0.0F || std::ranges::none_of(entities, [](const Entity &body) { return body.mind.active; }))
        return;
    Noise noise;
    noise.position = {cell.x + 0.5, cell.y + 0.5, cell.z + 0.5};
    noise.loudness = loudness;
    noise.kind = loudness >= 32.0F ? "explosion" : "digging";
    noise.player = player;
    noise.at = server_time;
    if (ai.noises.size() >= 128U)
        ai.noises.pop_front();
    ai.noises.push_back(std::move(noise));
}

bool GameplayService::Impl::ai_attack_believable(const PlayerRecord &player, const Entity &target,
                                                 std::uint16_t weapon, const Value &message, float reach)
{
    if (!player.alive || !backend.player_may_act(player.gid))
        return false;
    const CreatureDef *creature = server_registry.creature(target.creature);
    const double half = creature != nullptr ? creature->width * 0.5 : 0.3;
    const double height = creature != nullptr ? creature->height : 1.8;
    // The client struck what it drew, up to a few hundred milliseconds old.
    const double slack = (creature != nullptr ? creature->run_speed : 4.0) * 0.4 + 1.0;
    const Vec3 eye{player.position.x, player.position.y + 1.62, player.position.z};
    const Vec3 centre{target.position.x, target.position.y + height * 0.5, target.position.z};
    const Vec3 to{centre.x - eye.x, centre.y - eye.y, centre.z - eye.z};
    const double distance = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
    // A melee weapon reaches 2.5 blocks (and the authority's slack of 3);
    // (Items) a mod weapon its own reach and the same slack.
    if (item_is_melee_weapon(static_cast<InventoryItemId>(weapon)) && distance > reach + 0.5 + half + slack)
        return false;
    if (!line_of_sight(eye, centre) && !line_of_sight(eye, {centre.x, target.position.y + height - 0.1, centre.z}) &&
        !line_of_sight(eye, {centre.x, target.position.y + 0.3, centre.z}))
        return false;
    const Value *look = message.field("dir");
    const std::optional<Vec3> direction = look != nullptr ? try_vec3(*look) : std::nullopt;
    if (!direction.has_value() || !finite(*direction))
        return false;
    const double length = std::sqrt(distance_squared(*direction, {}));
    if (!(length > 1e-6))
        return false;
    const double along = (to.x * direction->x + to.y * direction->y + to.z * direction->z) / length;
    if (along < -1.0)
        return false;
    const double off = std::sqrt(std::max(0.0, distance * distance - along * along));
    return off <= std::max(half, height * 0.5) + slack + 0.5;
}

void GameplayService::Impl::ai_send_full_state(PlayerRecord &player)
{
    // What it was told of entities before is stale: it learns them again.
    player.known_entities.clear();
    out().to_player(player.gid, retail_rules_value(ai.retail), true);
    for (const Projectile &projectile : ai.projectiles)
        if (!projectile.removed)
            out().to_player(player.gid, projectile_value(projectile), true);
    // Boss bars go out with the next boss pass.
    ai.boss_sent.clear();
}

void GameplayService::Impl::ai_stop_server()
{
    ai = AiWorld{};
}

void GameplayService::Impl::ai_stop_client()
{
    ai_client = AiClientState{};
}

// ------------------------------------------------------------ the client

bool GameplayService::Impl::ai_client_receive(std::string_view type, const Value &message)
{
    const auto number = [&](const Value &from, std::string_view key, double fallback) {
        const double value = number_or(from, key, fallback);
        return std::isfinite(value) ? value : fallback;
    };
    const auto read_vec = [](const Value &from, std::string_view key) -> std::optional<Vec3> {
        const Value *field = from.field(key);
        if (field == nullptr)
            return std::nullopt;
        const auto value = try_vec3(*field);
        return value.has_value() && finite(*value) ? value : std::nullopt;
    };
    if (type == "retail")
    {
        ai_client.retail = retail_rules_from_value(message);
        ai_client.retail_known = true;
        // Every receipt clears again: a joiner, or a peer that heard of an
        // enemy before the rules, sends away what the rules switch off.
        ai_client.retail_clear = ai_client.retail_clear || ai_client.retail.clear_existing;
        return true;
    }
    if (type == "ent~" && message.field("pk") != nullptr)
    {
        // Packed entity updates (entities.cpp, replicate_entities).
        const Value &field = *message.field("pk");
        if (field.type() != ValueType::string)
            return true;
        const std::string &bytes = field.as_string();
        std::size_t at{};
        bool broken{};
        const auto byte = [&]() -> std::uint8_t {
            if (at >= bytes.size())
            {
                broken = true;
                return 0U;
            }
            return static_cast<std::uint8_t>(bytes[at++]);
        };
        const auto varint = [&]() -> std::uint64_t {
            std::uint64_t value{};
            for (unsigned shift = 0U; shift < 64U && !broken; shift += 7U)
            {
                const std::uint8_t next = byte();
                value |= static_cast<std::uint64_t>(next & 0x7FU) << shift;
                if ((next & 0x80U) == 0U)
                    return value;
            }
            broken = true;
            return 0U;
        };
        const auto coordinate = [&]() {
            const std::uint64_t raw = varint();
            const auto value = static_cast<std::int64_t>((raw >> 1U) ^ (~(raw & 1U) + 1U));
            return static_cast<double>(value) / 64.0;
        };
        const auto text = [&]() {
            const std::uint64_t length = varint();
            if (broken || length > 255U || at + length > bytes.size())
            {
                broken = true;
                return std::string{};
            }
            std::string value = bytes.substr(at, static_cast<std::size_t>(length));
            at += static_cast<std::size_t>(length);
            return value;
        };
        while (at < bytes.size() && !broken)
        {
            const std::uint64_t id = varint();
            const std::uint8_t flags = byte();
            const Vec3 position{coordinate(), coordinate(), coordinate()};
            const float yaw = static_cast<float>(byte()) / 256.0F * 6.2831853F;
            const std::string animation = (flags & 1U) != 0U ? text() : std::string{};
            const float health = (flags & 2U) != 0U ? static_cast<float>(byte()) / 255.0F : -1.0F;
            const std::string name = (flags & 4U) != 0U ? text() : std::string{};
            if (broken)
                break;
            const auto found = std::ranges::find(client_entities, id, &ClientEntity::id);
            if (found == client_entities.end())
                continue;
            if (finite(position))
            {
                found->from = found->position;
                found->to = position;
                found->from_yaw = found->yaw;
                found->to_yaw = yaw > 3.14159265F ? yaw - 6.2831853F : yaw;
                found->blend = 0.0F;
            }
            if ((flags & 1U) != 0U)
                found->animation = animation;
            if (health >= 0.0F)
                found->health_fraction = health;
            if ((flags & 4U) != 0U)
                found->name = name;
        }
        notify(ClientChange::entities);
        return true;
    }
    if (type == "prj+")
    {
        const auto id = static_cast<std::uint64_t>(std::max(0.0, number(message, "id", 0.0)));
        const auto position = read_vec(message, "p");
        const auto velocity = read_vec(message, "v");
        if (id == 0U || !position.has_value() || !velocity.has_value())
            return true;
        if (ai_client.projectiles.size() >= 1024U)
            ai_client.projectiles.erase(ai_client.projectiles.begin());
        ClientProjectile projectile;
        projectile.id = id;
        projectile.kind = string_or(message, "kind", "").substr(0, 128U);
        projectile.model = string_or(message, "model", "").substr(0, 256U);
        projectile.texture = string_or(message, "texture", "").substr(0, 256U);
        projectile.fx = string_or(message, "fx", "").substr(0, 128U);
        projectile.scale = static_cast<float>(std::clamp(number(message, "scale", 1.0), 0.05, 16.0));
        projectile.radius = static_cast<float>(std::clamp(number(message, "r", 0.15), 0.01, 8.0));
        projectile.gravity = static_cast<float>(std::clamp(number(message, "g", 0.0), -200.0, 200.0));
        projectile.impact_fx = string_or(message, "ifx", "").substr(0, 128U);
        projectile.explosion = static_cast<float>(std::clamp(number(message, "boom", 0.0), 0.0, 64.0));
        projectile.position = *position;
        projectile.velocity = *velocity;
        std::erase_if(ai_client.projectiles, [&](const ClientProjectile &other) { return other.id == id; });
        ai_client.projectiles.push_back(std::move(projectile));
        return true;
    }
    if (type == "prj~")
    {
        const Value *list = message.field("list");
        if (list == nullptr || list->type() != ValueType::array)
            return true;
        for (const Value &entry : list->as_array())
        {
            const auto id = static_cast<std::uint64_t>(std::max(0.0, number(entry, "id", 0.0)));
            const auto found = std::ranges::find(ai_client.projectiles, id, &ClientProjectile::id);
            if (found == ai_client.projectiles.end())
                continue;
            if (const auto position = read_vec(entry, "p"))
                found->position = *position;
            if (const auto velocity = read_vec(entry, "v"))
                found->velocity = *velocity;
        }
        return true;
    }
    if (type == "prj-")
    {
        const auto id = static_cast<std::uint64_t>(std::max(0.0, number(message, "id", 0.0)));
        const auto found = std::ranges::find(ai_client.projectiles, id, &ClientProjectile::id);
        if (found == ai_client.projectiles.end())
            return true;
        if (ai_client.impacts.size() < 64U)
            ai_client.impacts.push_back({read_vec(message, "p").value_or(found->position), found->kind,
                                         string_or(message, "hit", "").substr(0, 16U), found->impact_fx,
                                         found->explosion});
        ai_client.projectiles.erase(found);
        return true;
    }
    if (type == "boss")
    {
        const auto id = static_cast<std::uint64_t>(std::max(0.0, number(message, "id", 0.0)));
        if (id == 0U)
            return true;
        if (bool_or(message, "gone", false))
        {
            std::erase_if(ai_client.bosses, [&](const ClientBoss &boss) { return boss.entity == id; });
            return true;
        }
        auto found = std::ranges::find(ai_client.bosses, id, &ClientBoss::entity);
        if (found == ai_client.bosses.end())
        {
            if (ai_client.bosses.size() >= 16U)
                return true;
            ai_client.bosses.push_back({});
            found = ai_client.bosses.end() - 1;
        }
        found->entity = id;
        found->mod = string_or(message, "mod", "").substr(0, 64U);
        found->title = string_or(message, "title", "").substr(0, 96U);
        found->color = string_or(message, "color", "").substr(0, 16U);
        found->fraction = static_cast<float>(std::clamp(number(message, "hp", 1.0), 0.0, 1.0));
        found->phase = string_or(message, "phase", "").substr(0, 64U);
        found->music = string_or(message, "music", "").substr(0, 256U);
        found->position = read_vec(message, "p").value_or(found->position);
        found->range = static_cast<float>(std::clamp(number(message, "range", 64.0), 1.0, 1024.0));
        return true;
    }
    return false;
}

void GameplayService::Impl::ai_client_tick(float seconds)
{
    // Projectiles fly on between the host's corrections; the host removes
    // them (prj-), so a lost removal only lingers for their lifetime.
    for (ClientProjectile &projectile : ai_client.projectiles)
    {
        projectile.age += seconds;
        projectile.velocity.y -= projectile.gravity * seconds;
        projectile.position.x += projectile.velocity.x * seconds;
        projectile.position.y += projectile.velocity.y * seconds;
        projectile.position.z += projectile.velocity.z * seconds;
        const double flat = std::sqrt(projectile.velocity.x * projectile.velocity.x +
                                      projectile.velocity.z * projectile.velocity.z);
        if (flat > 1e-6 || std::abs(projectile.velocity.y) > 1e-6)
        {
            projectile.yaw = static_cast<float>(std::atan2(projectile.velocity.x, -projectile.velocity.z));
            projectile.pitch = static_cast<float>(std::atan2(projectile.velocity.y, flat));
        }
    }
    std::erase_if(ai_client.projectiles, [](const ClientProjectile &projectile) { return projectile.age > 30.0F; });
}

// ------------------------------------------------------------- the public

void GameplayService::noise(Vec3 position, float loudness, std::string_view kind, std::optional<std::uint8_t> gid)
{
    if (!server_running_ || !finite(position) || !(loudness > 0.0F))
        return;
    Impl &impl = *impl_;
    // Nobody to hear it: no cost.
    if (std::ranges::none_of(impl.entities, [](const Entity &body) { return body.mind.active; }))
        return;
    Noise noise;
    noise.position = position;
    noise.loudness = std::min(loudness, 256.0F);
    noise.kind = std::string{kind.substr(0, 32U)};
    if (gid.has_value())
        if (const PlayerRecord *player = impl.player_by_gid(*gid))
            noise.player = player->serial;
    noise.at = impl.server_time;
    if (impl.ai.noises.size() >= 128U)
        impl.ai.noises.pop_front();
    impl.ai.noises.push_back(std::move(noise));
}

const std::vector<ClientProjectile> &GameplayService::client_projectiles() const noexcept
{
    return impl_->ai_client.projectiles;
}

const std::vector<ClientBoss> &GameplayService::client_bosses() const noexcept
{
    return impl_->ai_client.bosses;
}

std::vector<ClientImpact> GameplayService::take_client_impacts()
{
    return std::exchange(impl_->ai_client.impacts, {});
}

std::vector<std::string> GameplayService::ai_definition_problems() const
{
    const Impl &impl = *impl_;
    std::vector<std::string> problems;
    static constexpr std::array<std::string_view, 7> looks{"zombie", "archer", "skeleton", "skeleton_sword",
                                                           "skeleton_axes", "felguard", "alien"};
    for (const CreatureDef &creature : impl.server_registry.creatures)
    {
        // "retail:zombie|models/ghoul": each alternative in turn.
        std::string_view rest = creature.model;
        while (!rest.empty())
        {
            const std::string_view choice = rest.substr(0U, rest.find('|'));
            rest = rest.find('|') == std::string_view::npos ? std::string_view{} : rest.substr(rest.find('|') + 1U);
            if (!choice.starts_with("retail:"))
                continue;
            std::string_view family = choice.substr(7U);
            family = family.substr(0U, family.find(':'));
            if (std::ranges::find(looks, family) == looks.end())
                problems.push_back(std::format("creature '{}': '{}' is not a retail enemy (retail:zombie, archer, "
                                               "skeleton, skeleton_sword, skeleton_axes, felguard, alien)",
                                               creature.key, choice));
        }
        if (creature.ai == nullptr)
            continue;
        const CreatureAi &ai_def = *creature.ai;
        const BrainDef *definition = impl.brain(ai_def.brain, creature.mod);
        if (!ai_def.brain.empty() && definition == nullptr)
            problems.push_back(std::format("creature '{}': brain '{}' is not defined (brains.define)", creature.key,
                                           ai_def.brain));
        for (const AttackDef &attack : ai_def.attacks)
            if (attack.kind == AttackKind::ranged && impl.projectile_def(attack.projectile, creature.mod) == nullptr)
                problems.push_back(std::format("creature '{}': attack '{}' fires '{}', which is not defined "
                                               "(projectiles.define)",
                                               creature.key, attack.name, attack.projectile));
        for (const AbilityDef &ability : ai_def.abilities)
            if (!ability.run.valid())
                problems.push_back(std::format("creature '{}': ability '{}' has no run function", creature.key,
                                               ability.name));
        for (const BossPhase &phase : ai_def.boss.phases)
            if (!phase.state.empty() && (definition == nullptr || definition->state(phase.state) == nullptr))
                problems.push_back(std::format("creature '{}': boss phase '{}' switches to state '{}', which {} has not",
                                               creature.key, phase.name, phase.state,
                                               definition != nullptr ? std::format("brain '{}'", definition->key)
                                                                     : std::string{"the built-in brain"}));
        if (ai_def.movement == MoveMode::swim && creature.spawn.enabled && creature.spawn.surface)
            problems.push_back(std::format("creature '{}': a swimmer's spawn rule needs surface = false and the "
                                           "water's heights (y_min, y_max)",
                                           creature.key));
    }
    return problems;
}

std::string GameplayService::ai_report() const
{
    const AiStats &stats = impl_->ai.stats;
    std::size_t minds{};
    for (const Entity &body : impl_->entities)
        if (!body.removed && body.mind.active)
            ++minds;
    return std::format("ai minds={} thinks={} deferred={} think_ms={:.2f} step_ms={:.2f} paths={} nodes={} "
                       "fields={} cache_hits={} sight_checks={} projectiles={}",
                       minds, stats.thinks, stats.deferred_thinks, stats.think_ms, stats.step_ms,
                       stats.path_searches, stats.path_nodes, stats.field_builds, stats.cache_hits,
                       stats.line_checks, impl_->ai.projectiles.size());
}

} // namespace openminerz::modding::gameplay
