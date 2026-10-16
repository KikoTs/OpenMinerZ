// GameplayService: lifecycle, players, the engine hooks, the server tick and
// the replication protocol between the server and client halves.
//
// Replication messages are Value maps; "t" names them. Server -> client:
//   registry    the session registry (registry_snapshot())
//   stats       {values={name=n}, full}            owner only
//   effects     {list=[{kind, remaining, strength, icon}], speed}
//   difficulty  DifficultyScaling                    everyone
//   do          a directive for the owner's own player: op = health | damage |
//               heal | teleport | give | take | clear (+ fields)
//   ent+ ent~ ent-   entity spawned / moved (10 Hz, unreliable) / removed
//   overlay     {cells=[{x,y,z,b}]}: mod block identity of cells (b = key, "" clears)
//   dialogue    the open dialogue node, or {session, close=true}
//   quests      the quest log
//   tags        {slots=[{slot, item, key}]}: mod item identities to restore
//   chat        {text}: a system line for this player
//   crate pickup  mod items in crates and pickups (containers.cpp)
// Client -> server (validated and rate limited before anything runs):
//   ready  report{health, dead}  hurt{amount, cause}  died{cause, p}
//   respawned{p}  crafted{item, count, key}  inventory{counts, tags}
//   use{item, key}  attack{entity, damage, weapon}  interact{entity}
//   choose{session, choice}  craft{recipe}  command{text}
//   crate{x, y, z, s, m}  pickup{p, m}

#include "internal.hpp"

#include "openminerz/inventory.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/enemy.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace openminerz::modding::gameplay
{

// ------------------------------------------------------------ value helpers

Value make_map(std::initializer_list<std::pair<std::string, Value>> fields)
{
    Map map;
    map.reserve(fields.size());
    for (const auto &field : fields)
        map.emplace_back(field.first, field.second);
    return Value{std::move(map)};
}

Value vec3_value(Vec3 value)
{
    return Value{value};
}

Value cell_value(BlockPos cell)
{
    return Value{Vec3{static_cast<double>(cell.x), static_cast<double>(cell.y),
                      static_cast<double>(cell.z)}};
}

std::optional<Vec3> try_vec3(const Value &value) noexcept
{
    if (value.type() == ValueType::vec3)
        return std::get<Vec3>(value.storage());
    const auto number = [](const Value *field) -> std::optional<double> {
        if (field == nullptr || !field->is_number())
            return std::nullopt;
        return field->type() == ValueType::integer
                   ? static_cast<double>(std::get<std::int64_t>(field->storage()))
                   : std::get<double>(field->storage());
    };
    if (value.type() == ValueType::map)
    {
        const auto x = number(value.field("x"));
        const auto y = number(value.field("y"));
        const auto z = number(value.field("z"));
        if (x && y && z)
            return Vec3{*x, *y, *z};
        return std::nullopt;
    }
    if (value.type() == ValueType::array)
    {
        const Array &array = *std::get<std::shared_ptr<Array>>(value.storage());
        if (array.size() != 3U)
            return std::nullopt;
        const auto x = number(&array[0]);
        const auto y = number(&array[1]);
        const auto z = number(&array[2]);
        if (x && y && z)
            return Vec3{*x, *y, *z};
    }
    return std::nullopt;
}

bool finite(Vec3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Vec3 to_vec3(const Value &value, std::string_view what)
{
    const auto vector = try_vec3(value);
    if (!vector.has_value() || !finite(*vector) || std::abs(vector->x) > 1.0e7 ||
        std::abs(vector->y) > 1.0e7 || std::abs(vector->z) > 1.0e7)
        throw ScriptError{std::format("{}: expected a position (vector or {{x, y, z}})", what)};
    return *vector;
}

BlockPos floor_cell(Vec3 position) noexcept
{
    return {static_cast<int>(std::floor(position.x)), static_cast<int>(std::floor(position.y)),
            static_cast<int>(std::floor(position.z))};
}

BlockPos to_cell(const Value &value, std::string_view what)
{
    return floor_cell(to_vec3(value, what));
}

double number_or(const Value &table, std::string_view key, double fallback)
{
    const Value *field = table.field(key);
    if (field == nullptr || field->is_nil())
        return fallback;
    const double number = field->as_number();
    if (!std::isfinite(number))
        throw ScriptError{std::format("'{}' must be a finite number", key)};
    return number;
}

std::string string_or(const Value &table, std::string_view key, std::string_view fallback)
{
    const Value *field = table.field(key);
    if (field == nullptr || field->is_nil())
        return std::string{fallback};
    return field->as_string();
}

bool bool_or(const Value &table, std::string_view key, bool fallback)
{
    const Value *field = table.field(key);
    return field == nullptr || field->is_nil() ? fallback : field->as_bool();
}

Callback callback_or_none(const Value &table, std::string_view key)
{
    const Value *field = table.field(key);
    return field == nullptr || field->is_nil() ? Callback{} : field->as_callback();
}

double distance_squared(Vec3 a, Vec3 b) noexcept
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

const char *damage_cause_name(DamageCause cause) noexcept
{
    switch (cause)
    {
    case DamageCause::unknown:
        return "unknown";
    case DamageCause::enemy:
        return "enemy";
    case DamageCause::fall:
        return "fall";
    case DamageCause::lava:
        return "lava";
    case DamageCause::drowning:
        return "drowning";
    case DamageCause::player:
        return "player";
    case DamageCause::explosion:
        return "explosion";
    case DamageCause::creature:
        return "creature";
    case DamageCause::effect:
        return "effect";
    case DamageCause::script:
        return "script";
    }
    return "unknown";
}

namespace
{

[[nodiscard]] DamageCause damage_cause_from(std::string_view name) noexcept
{
    for (int index = 0; index <= static_cast<int>(DamageCause::script); ++index)
        if (name == damage_cause_name(static_cast<DamageCause>(index)))
            return static_cast<DamageCause>(index);
    return DamageCause::unknown;
}

[[nodiscard]] std::string_view message_type(const Value &message)
{
    const Value *type = message.field("t");
    if (type == nullptr || type->type() != ValueType::string)
        return {};
    return type->as_string();
}

[[nodiscard]] std::string enemy_type_name(int type)
{
    switch (world::enemy_stats(type).enemy_class)
    {
    case world::EnemyClass::zombie:
        return "zombie";
    case world::EnemyClass::archer:
        return "archer";
    case world::EnemyClass::skeleton:
    case world::EnemyClass::skeleton_sword:
    case world::EnemyClass::skeleton_axes:
        return "skeleton";
    case world::EnemyClass::felguard:
        return "felguard";
    case world::EnemyClass::alien:
        return "alien";
    }
    return "enemy";
}

// Day phases by the classic sky's own hours: night is 21:00 to 06:00.
[[nodiscard]] int day_phase(double day) noexcept
{
    const double time_of_day = day - std::floor(day);
    return time_of_day < 0.25 || time_of_day >= 0.875 ? 1 : 0;
}

} // namespace

float DifficultyScaling::health_for(int type) const noexcept
{
    const auto found = per_type.find(type);
    return found != per_type.end() ? found->second[0] : enemy_health;
}

float DifficultyScaling::damage_for(int type) const noexcept
{
    const auto found = per_type.find(type);
    return found != per_type.end() ? found->second[1] : enemy_damage;
}

// ---------------------------------------------------------------- loopback

void LoopbackGameplaySync::to_player(std::uint8_t gid, const Value &message, bool reliable)
{
    const auto local = service_.impl_->backend.local_player();
    if (local.has_value() && *local == gid)
    {
        if (service_.client_running())
            service_.receive_from_server(message);
        return;
    }
    if (remote_ != nullptr)
        remote_->to_player(gid, message, reliable);
}

void LoopbackGameplaySync::to_all(const Value &message, bool reliable)
{
    if (service_.client_running())
        service_.receive_from_server(message);
    if (remote_ != nullptr)
        remote_->to_all(message, reliable);
}

void LoopbackGameplaySync::to_server(const Value &message)
{
    if (service_.server_running())
    {
        if (const auto local = service_.impl_->backend.local_player())
            service_.receive_from_client(*local, message);
        return;
    }
    if (remote_ != nullptr)
        remote_->to_server(message);
}

// ------------------------------------------------------------------- Impl

GameplayService::Impl::Impl(GameplayService &owner_service, ScriptHost &script_host,
                            GameplayBackend &game, GameplayLimits service_limits)
    : owner{owner_service}, host{script_host}, backend{game}, limits{service_limits},
      loopback{owner_service}
{
}

void GameplayService::Impl::hold(Side side, const Callback &callback)
{
    if (callback.valid())
        held_callbacks[side == Side::server ? 1 : 0].push_back(callback);
}

void GameplayService::Impl::release_side(Side side)
{
    auto &held = held_callbacks[side == Side::server ? 1 : 0];
    for (const Callback &callback : held)
        host.release(callback);
    held.clear();
}

bool GameplayService::Impl::emit(Side side, std::string_view event, Value payload)
{
    return host.emit(side, event, payload);
}

void GameplayService::Impl::log(std::string_view mod, LogLevel level, std::string_view message)
{
    host.log(mod, level, message);
}

float GameplayService::Impl::random01() noexcept
{
    // xorshift32: the service's own stream, so nothing here draws from the
    // game's deterministic generators.
    random_state ^= random_state << 13U;
    random_state ^= random_state >> 17U;
    random_state ^= random_state << 5U;
    return static_cast<float>(random_state >> 8U) / 16777216.0F;
}

int GameplayService::Impl::random_int(int minimum, int maximum_inclusive) noexcept
{
    if (maximum_inclusive <= minimum)
        return minimum;
    const auto span = static_cast<std::int64_t>(maximum_inclusive) - minimum + 1;
    return minimum + static_cast<int>(std::min<std::int64_t>(
                         span - 1, static_cast<std::int64_t>(random01() * static_cast<float>(span))));
}

// ----------------------------------------------------------------- players

PlayerRecord *GameplayService::Impl::player_by_gid(std::uint8_t gid) noexcept
{
    for (PlayerRecord &player : players)
        if (player.present && player.gid == gid)
            return &player;
    return nullptr;
}

PlayerRecord *GameplayService::Impl::player_by_serial(std::uint64_t serial) noexcept
{
    for (PlayerRecord &player : players)
        if (player.serial == serial)
            return &player;
    return nullptr;
}

PlayerRecord *GameplayService::Impl::player_by_handle(Handle handle) noexcept
{
    if (handle.kind != HandleKind::player)
        return nullptr;
    return player_by_serial(handle.id);
}

PlayerRecord &GameplayService::Impl::player_arg(const Value &value)
{
    if (value.type() != ValueType::handle)
        throw ScriptError{"expected a player handle"};
    ensure_players();
    PlayerRecord *player = player_by_handle(value.as_handle());
    if (player == nullptr || !player->present)
        throw ScriptError{"that player is not in the session"};
    return *player;
}

PlayerRecord *GameplayService::Impl::local_player() noexcept
{
    const auto gid = backend.local_player();
    return gid.has_value() ? player_by_gid(*gid) : nullptr;
}

Handle GameplayService::Impl::handle_of(const PlayerRecord &player) const noexcept
{
    return Handle{HandleKind::player, player.serial};
}

PlayerRecord &GameplayService::Impl::join(std::uint8_t gid, std::string_view name, bool local,
                                          bool announce_now)
{
    if (PlayerRecord *existing = player_by_gid(gid))
        return *existing;
    prune_departed();
    PlayerRecord &player = players.emplace_back();
    player.serial = next_player_serial++;
    player.gid = gid;
    player.name = std::string{name};
    player.local = local;
    player.tokens = limits.client_messages_per_second * 2.0F;
    player.attack_tokens = 20.0F;
    if (activated)
        restore(player);
    // Stats the mods define start at their initial value.
    for (const auto &[stat_name, definition] : server_registry.stats)
        if (!player.stats.contains(stat_name))
            player.stats[stat_name] = definition.initial;
    const std::uint64_t serial = player.serial;
    // A handle may be handed out before any server VM runs (the mod channel
    // admits peers first); scripts hear of the player once they can.
    if (announce_now)
        announce(player);
    // A handler may have joined someone else: the vector may have moved.
    return *player_by_serial(serial);
}

void GameplayService::Impl::announce(PlayerRecord &player)
{
    if (player.announced || !player.present || !owner.server_running())
        return;
    player.announced = true;
    emit(Side::server, "player.joined", make_map({{"player", handle_of(player)}, {"name", player.name}}));
}

void GameplayService::Impl::prune_departed()
{
    constexpr std::size_t kept_departed = 64U;
    std::size_t departed = static_cast<std::size_t>(
        std::ranges::count_if(players, [](const PlayerRecord &player) { return !player.present; }));
    // Oldest first: the vector is in join order.
    std::erase_if(players, [&](const PlayerRecord &player) {
        if (player.present || departed <= kept_departed)
            return false;
        --departed;
        return true;
    });
}

void GameplayService::Impl::restore(PlayerRecord &player)
{
    player.restored = true;
    const auto stored = persisted_players.find(player.name);
    if (stored == persisted_players.end())
        return;
    for (const auto &[stat_name, value] : stored->second.stats)
        player.stats[stat_name] = value;
    for (const auto &[quest, progress] : stored->second.quests)
        player.quests.insert_or_assign(quest, progress);
    player.completed_quests.insert(stored->second.completed_quests.begin(),
                                   stored->second.completed_quests.end());
    player.inventory_tags = stored->second.inventory_tags;
    player.stats_full_resend = true;
    player.quests_dirty = true;
}

void GameplayService::Impl::ensure_players()
{
    // Before activation (a deferred start, while scripts still load) the
    // players are known at once; their saved data follows at activation.
    if (!activated && owner.server_running())
        refresh_players();
}

void GameplayService::Impl::store_persisted(const PlayerRecord &player)
{
    if (player.name.empty())
        return;
    PersistedPlayer &stored = persisted_players[player.name];
    stored.stats.clear();
    for (const auto &[stat_name, value] : player.stats)
    {
        const auto definition = server_registry.stats.find(stat_name);
        if (definition == server_registry.stats.end() || definition->second.persist)
            stored.stats[stat_name] = value;
    }
    stored.quests = player.quests;
    stored.completed_quests = player.completed_quests;
    stored.inventory_tags = player.inventory_tags;
}

void GameplayService::Impl::leave(PlayerRecord &player)
{
    if (!player.present)
        return;
    if (player.dialogue.has_value())
        close_dialogue(player, false);
    // Scripts that never heard of the player do not hear it leave.
    if (player.announced)
        emit(Side::server, "player.left",
             make_map({{"player", handle_of(player)}, {"name", player.name}}));
    store_persisted(player);
    forget_held_item(player); // (Items)
    player.held_item = 0U;
    player.present = false;
    player.admitted = false;
    // Entities that were following or hunting this player forget them.
    for (Entity &entity : entities)
        if (entity.behaviour.target.player == player.serial)
        {
            entity.behaviour = {};
            entity.path.clear();
        }
}

void GameplayService::Impl::refresh_players()
{
    // A handler of the events below may ask for the players again (e.g.
    // players.list before activation): the outer pass already does it.
    if (refreshing_players)
        return;
    refreshing_players = true;
    struct Done final
    {
        bool &flag;
        ~Done()
        {
            flag = false;
        }
    } done{refreshing_players};
    std::vector<PlayerSnapshot> snapshot;
    backend.players(snapshot);
    for (const PlayerSnapshot &seen : snapshot)
    {
        PlayerRecord *player = player_by_gid(seen.gid);
        const bool fresh = player == nullptr;
        if (fresh)
            player = &join(seen.gid, seen.name, seen.local, false);
        player->local = seen.local;
        if (finite(seen.position))
            player->position = seen.position;
        player->yaw = seen.yaw;
        if (seen.local)
        {
            player->health = seen.health;
            player->alive = seen.alive;
        }
        if (fresh)
        {
            // (Integration) player.joined handlers see where the player is.
            player->alive = seen.alive;
            announce(*player);
        }
        else if (!seen.local && seen.alive != player->alive && owner.server_running())
        {
            // Another peer's player, by its PlayerUpdate death flag (its own
            // OpenMinerZ client may have reported it first).
            player->alive = seen.alive;
            emit(Side::server, seen.alive ? "player.respawned" : "player.died",
                 make_map({{"player", handle_of(*player)}, {"position", vec3_value(player->position)}}));
        }
    }
    // An admitted player leaves only through remote_player_left: the game
    // lists a joiner only once its presence is announced, which is after its
    // mod channel (and so its handle) exists. Gone players are collected
    // first: player.left handlers may join others and move the vector.
    std::vector<std::uint64_t> gone;
    for (const PlayerRecord &player : players)
        if (player.present && !player.admitted &&
            std::ranges::none_of(snapshot,
                                 [&](const PlayerSnapshot &seen) { return seen.gid == player.gid; }))
            gone.push_back(player.serial);
    for (const std::uint64_t serial : gone)
        if (PlayerRecord *player = player_by_serial(serial))
            leave(*player);
}

void GameplayService::Impl::directive(PlayerRecord &player, Value message)
{
    out().to_player(player.gid, message, true);
}

void GameplayService::Impl::set_health(PlayerRecord &player, float health)
{
    health = std::clamp(health, 0.0F, 100.0F);
    if (player.local)
    {
        backend.local_set_health(health);
        player.health = health;
        return;
    }
    // (Authority) The host's authority owns the player's health instead.
    if (backend.authority_set_health(player.gid, health))
        return;
    directive(player, make_map({{"t", "do"}, {"op", "health"}, {"value", health}}));
    player.health = health;
}

void GameplayService::Impl::damage(PlayerRecord &player, float amount, DamageCause cause,
                                   Vec3 source)
{
    if (!(amount > 0.0F) || !std::isfinite(amount))
        return;
    if (player.local)
    {
        // The game's own path, which runs player.damaged through the hook.
        backend.local_damage(amount, source, cause);
        return;
    }
    // (Authority) Applied by the host, through player.damaged like any damage.
    if (backend.authority_damage(player.gid, amount, cause, source))
        return;
    directive(player, make_map({{"t", "do"},
                                {"op", "damage"},
                                {"value", amount},
                                {"cause", damage_cause_name(cause)},
                                {"source", vec3_value(source)}}));
}

void GameplayService::Impl::heal(PlayerRecord &player, float amount)
{
    if (!(amount > 0.0F) || !std::isfinite(amount) || !player.alive)
        return;
    if (player.local)
    {
        const float health = std::min(100.0F, player.health + amount);
        backend.local_set_health(health);
        player.health = health;
        return;
    }
    if (backend.authority_heal(player.gid, amount)) // (Authority)
        return;
    directive(player, make_map({{"t", "do"}, {"op", "heal"}, {"value", amount}}));
}

void GameplayService::Impl::teleport(PlayerRecord &player, Vec3 position)
{
    if (player.local)
    {
        backend.local_teleport(position);
        player.position = position;
        return;
    }
    backend.authority_teleport(player.gid, position); // (Authority) the jump is the host's
    directive(player, make_map({{"t", "do"}, {"op", "teleport"}, {"p", vec3_value(position)}}));
    player.position = position;
}

std::uint32_t GameplayService::Impl::give(PlayerRecord &player, ItemRef item, std::uint32_t count)
{
    if (!item.valid() || count == 0U)
        return 0U;
    count = std::min<std::uint32_t>(count, 4096U);
    if (player.local)
    {
        std::uint32_t given{};
        while (given < count)
        {
            const auto chunk = static_cast<std::uint16_t>(std::min<std::uint32_t>(count - given, 999U));
            const std::uint16_t left =
                backend.local_give({item.item, chunk, item.mod_item}, true);
            given += chunk - left;
            if (left != 0U)
                break;
        }
        return given;
    }
    backend.authority_give(player.gid, item.item, count); // (Authority) the host gave them
    directive(player, make_map({{"t", "do"},
                                {"op", "give"},
                                {"item", static_cast<std::int64_t>(item.item)},
                                {"key", item_key(item)},
                                {"count", static_cast<std::int64_t>(count)}}));
    // The owner adds what fits and drops the rest at its feet.
    player.inventory_counts[item_key(item)] += count;
    return count;
}

std::uint32_t GameplayService::Impl::take(PlayerRecord &player, ItemRef item, std::uint32_t count)
{
    if (!item.valid() || count == 0U)
        return 0U;
    if (player.local)
        return backend.local_take(item.item, item.mod_item, count);
    auto &known = player.inventory_counts[item_key(item)];
    const std::uint32_t taken = std::min(known, count);
    if (taken == 0U)
        return 0U;
    known -= taken;
    directive(player, make_map({{"t", "do"},
                                {"op", "take"},
                                {"item", static_cast<std::int64_t>(item.item)},
                                {"key", item_key(item)},
                                {"count", static_cast<std::int64_t>(taken)}}));
    return taken;
}

std::uint32_t GameplayService::Impl::count(PlayerRecord &player, ItemRef item)
{
    if (!item.valid())
        return 0U;
    if (player.local)
        return backend.local_count(item.item, item.mod_item);
    const auto found = player.inventory_counts.find(item_key(item));
    return found == player.inventory_counts.end() ? 0U : found->second;
}

void GameplayService::Impl::clear_inventory(PlayerRecord &player)
{
    if (player.local)
    {
        backend.local_clear_inventory();
        return;
    }
    player.inventory_counts.clear();
    directive(player, make_map({{"t", "do"}, {"op", "clear"}}));
}

void GameplayService::Impl::message(std::optional<std::uint8_t> gid, std::string_view text)
{
    std::string line{text.substr(0, 160U)};
    for (char &character : line)
        if (static_cast<unsigned char>(character) < 0x20U)
            character = ' ';
    if (!gid.has_value())
    {
        backend.chat(std::nullopt, line);
        return;
    }
    PlayerRecord *player = player_by_gid(*gid);
    if (player == nullptr)
        return;
    if (player->local)
        backend.chat(gid, line);
    else
        out().to_player(*gid, make_map({{"t", "chat"}, {"text", line}}), true);
}

float GameplayService::Impl::filter_local_damage(PlayerRecord &player, float amount,
                                                 DamageCause cause, Vec3 source)
{
    float applied = amount;
    damage_overrides.push_back(&applied);
    const bool allowed = emit(Side::server, "player.damaged",
                              make_map({{"player", handle_of(player)},
                                        {"amount", amount},
                                        {"cause", damage_cause_name(cause)},
                                        {"source", vec3_value(source)},
                                        {"health", player.health},
                                        // (Authority) another player whose damage
                                        // the host decides is as changeable
                                        {"remote", !player.local}}));
    damage_overrides.pop_back();
    if (!allowed || !std::isfinite(applied))
        return 0.0F;
    return std::clamp(applied, 0.0F, 10'000.0F);
}

ChatVerdict GameplayService::Impl::chat(PlayerRecord &player, std::string_view text)
{
    if (!text.empty() && text.front() == '/')
        return run_command(player, text) ? ChatVerdict::command : ChatVerdict::pass;
    const bool allowed = emit(Side::server, "chat.message",
                              make_map({{"player", handle_of(player)}, {"text", text}}));
    return allowed ? ChatVerdict::pass : ChatVerdict::cancel;
}

bool GameplayService::Impl::run_command(PlayerRecord &player, std::string_view text)
{
    std::vector<std::string> words;
    std::string current;
    for (const char character : text.substr(1U, 240U))
    {
        if (character == ' ')
        {
            if (!current.empty())
                words.push_back(std::move(current));
            current.clear();
        }
        else
            current.push_back(character);
    }
    if (!current.empty())
        words.push_back(std::move(current));
    if (words.empty())
        return false;
    const std::string name = words.front();
    if (name == "help" && !server_registry.commands.empty())
    {
        message(player.gid, "Mod commands:");
        for (const auto &[command, definition] : server_registry.commands)
            message(player.gid, std::format("/{} - {}", command, definition.help));
        return true;
    }
    const auto found = server_registry.commands.find(name);
    if (found == server_registry.commands.end())
        return false;
    Array arguments;
    for (std::size_t index = 1U; index < words.size() && index <= 32U; ++index)
        arguments.emplace_back(words[index]);
    const std::array<Value, 3> call{Value{handle_of(player)}, Value{std::move(arguments)},
                                    Value{std::string{text}}};
    const Value reply = host.invoke_as(handle_of(player), found->second.function, call);
    if (reply.type() == ValueType::string && !reply.as_string().empty())
        message(player.gid, reply.as_string());
    return true;
}

// ------------------------------------------------- client -> server messages

void GameplayService::Impl::handle_client_message(PlayerRecord &player, const Value &incoming)
{
    const std::string_view type = message_type(incoming);
    const auto number = [&](std::string_view key, double fallback) {
        const Value *field = incoming.field(key);
        return field != nullptr && field->is_number() ? field->as_number() : fallback;
    };
    const auto text = [&](std::string_view key) -> std::string {
        const Value *field = incoming.field(key);
        if (field == nullptr || field->type() != ValueType::string)
            return {};
        return field->as_string().substr(0, 256U);
    };
    if (type == "ready")
    {
        // The full state is large: a client asking again and again gets it
        // at most every few seconds (a joining client asks once or twice).
        if (server_time - player.full_state_at < 5.0)
            return;
        player.full_state_at = server_time;
        owner.send_full_state(player.gid);
        return;
    }
    // (Authority) The host's authority decided this player's health and life;
    // what its own client says about them is not believed.
    if (owner.authority() && !player.local &&
        (type == "report" || type == "hurt" || type == "died" || type == "respawned"))
        return;
    // (Integration) Mod item identities beside stock crate and pickup messages.
    if (type == "crate")
    {
        server_crate_message(player, incoming);
        return;
    }
    if (type == "pickup")
    {
        server_pickup_message(player, incoming);
        return;
    }
    if (type == "held") // (Items) the mod item in the sender's hand
    {
        server_held_message(player, incoming);
        return;
    }
    if (type == "report")
    {
        const double health = number("health", player.health);
        if (std::isfinite(health))
            player.health = static_cast<float>(std::clamp(health, 0.0, 100.0));
        return;
    }
    if (type == "hurt")
    {
        const double amount = number("amount", 0.0);
        if (!(amount > 0.0) || !std::isfinite(amount))
            return;
        // After the fact: the owner already applied it.
        static_cast<void>(emit(Side::server, "player.damaged",
                               make_map({{"player", handle_of(player)},
                                         {"amount", std::min(amount, 10'000.0)},
                                         {"cause", text("cause")},
                                         {"source", vec3_value(player.position)},
                                         {"health", player.health},
                                         {"remote", true}})));
        return;
    }
    if (type == "died")
    {
        if (!player.alive)
            return;
        player.alive = false;
        player.health = 0.0F;
        const Value *at = incoming.field("p");
        const auto position = at != nullptr ? try_vec3(*at) : std::nullopt;
        if (position.has_value() && finite(*position))
            player.position = *position;
        emit(Side::server, "player.died",
             make_map({{"player", handle_of(player)},
                       {"position", vec3_value(player.position)},
                       {"cause", text("cause")}}));
        return;
    }
    if (type == "respawned")
    {
        player.alive = true;
        player.health = 100.0F;
        emit(Side::server, "player.respawned",
             make_map({{"player", handle_of(player)}, {"position", vec3_value(player.position)}}));
        return;
    }
    if (type == "crafted")
    {
        const double item = number("item", 0.0);
        const double amount = number("count", 0.0);
        if (item < 1.0 || item > 65535.0 || amount < 1.0 || amount > 4096.0)
            return;
        const std::string key = text("key");
        emit(Side::server, "item.crafted",
             make_map({{"player", handle_of(player)},
                       {"item", key.empty() ? std::string{retail_item_name(
                                                  static_cast<std::uint16_t>(item))}
                                            : key},
                       {"count", static_cast<std::int64_t>(amount)}}));
        return;
    }
    if (type == "inventory")
    {
        const Value *counts = incoming.field("counts");
        if (counts != nullptr && counts->type() == ValueType::map)
        {
            player.inventory_counts.clear();
            for (const auto &[key, value] : counts->as_map())
            {
                if (player.inventory_counts.size() >= 256U || key.size() > 128U ||
                    !value.is_number())
                    break;
                const double amount = value.as_number();
                if (amount >= 0.0 && amount <= 1'000'000.0)
                    player.inventory_counts[key] = static_cast<std::uint32_t>(amount);
            }
        }
        const Value *tags = incoming.field("tags");
        if (tags != nullptr && tags->type() == ValueType::array)
        {
            player.inventory_tags.clear();
            for (const Value &entry : tags->as_array())
            {
                if (player.inventory_tags.size() >= 64U || entry.type() != ValueType::map)
                    break;
                const Value *slot = entry.field("slot");
                const Value *key = entry.field("key");
                if (slot == nullptr || !slot->is_number() || key == nullptr ||
                    key->type() != ValueType::string || server_registry.item(key->as_string()) == nullptr)
                    continue;
                const double index = slot->as_number();
                if (index >= 0.0 && index < 128.0)
                    player.inventory_tags.emplace_back(static_cast<std::uint16_t>(index),
                                                       key->as_string());
            }
        }
        quest_event(player, ObjectiveKind::collect, {}, 0U);
        return;
    }
    if (type == "use")
    {
        const std::string key = text("key");
        const ItemDef *item = server_registry.item(key);
        if (item == nullptr || player.item_cooldown > 0.0F || !player.alive)
            return;
        player.item_cooldown = std::max(0.05F, item->cooldown);
        // (Integration) The aimed block, if the sender names one in its reach;
        // what is there comes from this world, never from the message.
        std::optional<BlockPos> aimed;
        std::optional<BlockPos> face;
        std::optional<Vec3> look;
        if (const Value *at = incoming.field("aim"))
            if (const auto cell = try_vec3(*at); cell.has_value() && finite(*cell) &&
                                                 distance_squared(*cell, player.position) <= 64.0)
                aimed = floor_cell(*cell);
        if (const Value *at = incoming.field("face"); at != nullptr && aimed.has_value())
            if (const auto cell = try_vec3(*at); cell.has_value() && finite(*cell) &&
                                                 distance_squared(*cell, player.position) <= 81.0)
                face = floor_cell(*cell);
        if (const Value *direction = incoming.field("look"))
            if (const auto vector = try_vec3(*direction);
                vector.has_value() && finite(*vector) && distance_squared(*vector, {}) > 0.01)
            {
                const double length = std::sqrt(distance_squared(*vector, {}));
                look = Vec3{vector->x / length, vector->y / length, vector->z / length};
            }
        // (mod-liquid) The liquid the sender looks into: its own word when the
        // cell is near it (aim_value checks that one holds a liquid), else
        // this world's along its look.
        std::optional<BlockPos> liquid;
        if (const Value *at = incoming.field("liquid"))
            if (const auto cell = try_vec3(*at); cell.has_value() && finite(*cell) &&
                                                 distance_squared(*cell, player.position) <= 64.0)
                liquid = floor_cell(*cell);
        if (!liquid.has_value() && look.has_value())
            liquid = liquid_on_ray({player.position.x, player.position.y + 1.4, player.position.z},
                                   *look, 5.0F);
        use_item(player, {item->fallback, item->id}, aim_value(aimed, face, look, liquid));
        return;
    }
    if (type == "attack")
    {
        const double id = number("entity", 0.0);
        const double amount = number("damage", 0.0);
        const double weapon = number("weapon", 0.0);
        if (player.attack_tokens < 1.0F || !player.alive || !(amount > 0.0) || weapon < 0.0 ||
            weapon > 65535.0)
            return;
        player.attack_tokens -= 1.0F;
        Entity *target = entity(static_cast<std::uint64_t>(std::max(0.0, id)));
        if (target == nullptr || target->removed)
            return;
        // A hit is believable within gun range of the attacker's last known
        // position, and never stronger than the held retail weapon.
        const InventoryItemId held = static_cast<InventoryItemId>(static_cast<std::uint16_t>(weapon));
        float limit =
            std::max(0.1F, item_enemy_damage(held, backend.modern_rules()
                                                       ? InventoryProfile::modern
                                                       : InventoryProfile::classic)) *
            1.01F;
        float reach = 5.0F;
        if (distance_squared(target->position, player.position) > 160.0 * 160.0)
            return;
        // (Items) A swing with a mod weapon is judged by that weapon: the
        // player must hold it (items.cpp), within its rate, its damage and
        // its reach.
        if (const std::string key = text("key"); !key.empty())
        {
            const ItemDef *mod_weapon = held_weapon_def(player, static_cast<std::uint16_t>(held));
            if (mod_weapon == nullptr || mod_weapon->key != key || !take_weapon_swing(player, *mod_weapon->weapon))
            {
                ++ai.stats.refused_claims;
                return;
            }
            limit = std::max(0.01F, mod_weapon->weapon->damage) * 1.01F;
            reach = mod_weapon->weapon->reach;
        }
        // mod-ai: checked like the authority checks a melee or a shot - a
        // living player, a melee weapon within its reach, a clear line from
        // the eye, and a look direction that passes the creature.
        if (!ai_attack_believable(player, *target, static_cast<std::uint16_t>(held), incoming, reach))
        {
            ++ai.stats.refused_claims;
            return;
        }
        damage_entity(*target, static_cast<float>(std::min(amount, static_cast<double>(limit))),
                      player.serial, 0U);
        return;
    }
    if (type == "interact")
    {
        Entity *target = entity(static_cast<std::uint64_t>(std::max(0.0, number("entity", 0.0))));
        if (target != nullptr && !target->removed &&
            distance_squared(target->position, player.position) <= 8.0 * 8.0)
            static_cast<void>(interact(player, *target));
        return;
    }
    if (type == "choose")
    {
        choose(player, static_cast<std::uint64_t>(std::max(0.0, number("session", 0.0))),
               static_cast<std::size_t>(std::clamp(number("choice", -1.0), -1.0, 64.0)));
        return;
    }
    if (type == "craft")
    {
        craft(player, static_cast<std::uint32_t>(std::clamp(number("recipe", 0.0), 0.0, 1.0e6)));
        return;
    }
    if (type == "placed")
    {
        // A mod block a remote player built: believed only where the world
        // already holds its fallback, within reach of that player.
        const BlockDef *placed = server_registry.block(text("key"));
        const BlockPos cell{static_cast<int>(std::clamp(number("x", 0.0), -1.0e6, 1.0e6)),
                            static_cast<int>(std::clamp(number("y", 0.0), -64.0, 63.0)),
                            static_cast<int>(std::clamp(number("z", 0.0), -1.0e6, 1.0e6))};
        const Vec3 centre{cell.x + 0.5, cell.y + 0.5, cell.z + 0.5};
        if (placed == nullptr || block_at(cell) != placed->fallback ||
            distance_squared(centre, player.position) > 12.0 * 12.0 ||
            overlay.size() >= limits.max_overlay_cells)
            return;
        overlay[cell] = placed->id;
        overlay_outbox.emplace_back(cell, placed->id);
        return;
    }
    if (type == "command")
    {
        const std::string line = text("text");
        if (line.size() > 1U && line.front() == '/' && !run_command(player, line))
            message(player.gid, "Unknown command. Type /help for the mod commands.");
        return;
    }
}

void GameplayService::Impl::use_item(PlayerRecord &player, ItemRef item, const Value &aim)
{
    const ItemDef *definition = server_registry.item(item.mod_item);
    const bool allowed =
        emit(Side::server, "item.used",
             make_map({{"player", handle_of(player)},
                       {"item", item_label(item)},
                       {"mod_item", definition != nullptr ? Value{definition->key} : Value{}},
                       {"target", aim}}));
    if (!allowed || definition == nullptr || !definition->on_use.valid())
        return;
    const std::array<Value, 3> call{Value{handle_of(player)}, Value{definition->key}, aim};
    static_cast<void>(host.invoke_as(handle_of(player), definition->on_use, call));
}

Value GameplayService::Impl::aim_value(std::optional<BlockPos> cell, std::optional<BlockPos> face,
                                       std::optional<Vec3> look, std::optional<BlockPos> liquid)
{
    // (mod-liquid) The liquid the look passes through first (the selection
    // goes through it): {position, block}.
    std::optional<Value> wet;
    if (liquid.has_value() && liquid_cell(*liquid))
    {
        const std::uint8_t block = block_at(*liquid).value_or(0U);
        wet = make_map({{"position", cell_value(*liquid)},
                        {"block", Value{block_label(block, mod_block_at(*liquid, block))}}});
    }
    if (!cell.has_value())
    {
        if (!wet.has_value())
            return look.has_value() ? make_map({{"look", vec3_value(*look)}}) : Value{};
        Map fields{{"liquid", std::move(*wet)}};
        if (look.has_value())
            fields.emplace_back("look", vec3_value(*look));
        return Value{std::move(fields)};
    }
    const std::uint8_t block = block_at(*cell).value_or(0U);
    Map fields{{"position", cell_value(*cell)},
               {"block", Value{block_label(block, mod_block_at(*cell, block))}}};
    if (face.has_value())
        fields.emplace_back("face", cell_value(*face));
    if (look.has_value())
        fields.emplace_back("look", vec3_value(*look));
    if (wet.has_value())
        fields.emplace_back("liquid", std::move(*wet));
    return Value{std::move(fields)};
}

void GameplayService::Impl::craft(PlayerRecord &player, std::uint32_t recipe_id)
{
    const auto recipe = std::ranges::find(server_registry.recipes, recipe_id, &RecipeDef::id);
    if (recipe == server_registry.recipes.end() || !player.alive)
        return;
    for (const auto &[ingredient, amount] : recipe->ingredients)
        if (count(player, ingredient) < amount)
        {
            message(player.gid, "Missing materials.");
            return;
        }
    for (const auto &[ingredient, amount] : recipe->ingredients)
        static_cast<void>(take(player, ingredient, amount));
    static_cast<void>(give(player, recipe->result, recipe->count));
    emit(Side::server, "item.crafted",
         make_map({{"player", handle_of(player)},
                   {"item", item_label(recipe->result)},
                   {"count", static_cast<std::int64_t>(recipe->count)},
                   {"recipe", static_cast<std::int64_t>(recipe->id)}}));
    quest_event(player, ObjectiveKind::collect, {}, 0U);
}

// ----------------------------------------------------------- client half

void GameplayService::Impl::notify(ClientChange change)
{
    ++client_revisions[static_cast<std::size_t>(change) % client_revisions.size()]; // (Visuals)
    if (client_listener)
        client_listener(change);
}

namespace
{

[[nodiscard]] float field_float(const Value &message, std::string_view key, float fallback)
{
    const Value *field = message.field(key);
    if (field == nullptr || !field->is_number())
        return fallback;
    const double value = field->as_number();
    return std::isfinite(value) ? static_cast<float>(value) : fallback;
}

[[nodiscard]] std::string field_string(const Value &message, std::string_view key)
{
    const Value *field = message.field(key);
    return field != nullptr && field->type() == ValueType::string ? field->as_string()
                                                                  : std::string{};
}

} // namespace

void GameplayService::Impl::client_receive(const Value &message)
{
    const std::string_view type = message_type(message);
    if (ai_client_receive(type, message)) // mod-ai: projectiles, bosses, retail rules
        return;
    if (type == "registry")
    {
        apply_registry(message);
        notify(ClientChange::registry);
        return;
    }
    if (type == "crate")
    {
        client_crate_message(message);
        return;
    }
    if (type == "pickup")
    {
        client_pickup_message(message);
        return;
    }
    if (type == "held") // (Items) what another player holds
    {
        client_held_message(message);
        return;
    }
    if (type == "stats")
    {
        const Value *values = message.field("values");
        if (values == nullptr || values->type() != ValueType::map)
            return;
        if (bool_or(message, "full", false))
            for (ClientStat &stat : client_stats)
                stat.value = stat.minimum;
        for (const auto &[name, value] : values->as_map())
        {
            if (!value.is_number())
                continue;
            auto stat = std::ranges::find(client_stats, name, &ClientStat::name);
            if (stat == client_stats.end())
            {
                if (client_stats.size() >= 64U)
                    continue;
                client_stats.push_back({name});
                stat = client_stats.end() - 1;
            }
            const double previous = stat->value;
            stat->value = value.as_number();
            if (previous != stat->value)
                emit(Side::client, "stat.changed",
                     make_map({{"stat", name}, {"value", stat->value}, {"previous", previous}}));
        }
        notify(ClientChange::stats);
        return;
    }
    if (type == "effects")
    {
        client_effects.clear();
        if (const Value *list = message.field("list"); list != nullptr && list->type() == ValueType::array)
            for (const Value &entry : list->as_array())
                if (client_effects.size() < 64U)
                    client_effects.push_back({field_string(entry, "kind"),
                                              field_float(entry, "remaining", -1.0F),
                                              field_float(entry, "strength", 1.0F),
                                              field_string(entry, "icon")});
        client_speed = std::clamp(field_float(message, "speed", 1.0F), 0.0F, 4.0F);
        backend.local_set_speed(client_speed);
        notify(ClientChange::effects);
        return;
    }
    if (type == "difficulty")
    {
        DifficultyScaling scaling;
        scaling.active = bool_or(message, "active", false);
        scaling.enemy_health = std::clamp(field_float(message, "enemy_health", 1.0F), 0.05F, 100.0F);
        scaling.enemy_damage = std::clamp(field_float(message, "enemy_damage", 1.0F), 0.0F, 100.0F);
        scaling.spawn_rate = std::clamp(field_float(message, "spawn_rate", 1.0F), 0.0F, 20.0F);
        if (const Value *types = message.field("per_type"); types != nullptr && types->type() == ValueType::array)
            for (const Value &entry : types->as_array())
            {
                const int type_id = static_cast<int>(field_float(entry, "type", -1.0F));
                if (type_id >= 0 && type_id < world::enemy_type_count)
                    scaling.per_type[type_id] = {
                        std::clamp(field_float(entry, "health", 1.0F), 0.05F, 100.0F),
                        std::clamp(field_float(entry, "damage", 1.0F), 0.0F, 100.0F)};
            }
        client_difficulty = std::move(scaling);
        notify(ClientChange::difficulty);
        return;
    }
    if (type == "do")
    {
        const std::string op = field_string(message, "op");
        if (op == "health")
            backend.local_set_health(std::clamp(field_float(message, "value", 100.0F), 0.0F, 100.0F));
        else if (op == "damage")
        {
            const Value *source = message.field("source");
            const auto position = source != nullptr ? try_vec3(*source) : std::nullopt;
            backend.local_damage(std::clamp(field_float(message, "value", 0.0F), 0.0F, 10'000.0F),
                                 position.value_or(Vec3{}),
                                 damage_cause_from(field_string(message, "cause")));
        }
        else if (op == "heal")
        {
            std::vector<PlayerSnapshot> snapshot;
            backend.players(snapshot);
            for (const PlayerSnapshot &seen : snapshot)
                if (seen.local && seen.alive)
                    backend.local_set_health(std::min(
                        100.0F, seen.health + std::clamp(field_float(message, "value", 0.0F), 0.0F, 100.0F)));
        }
        else if (op == "teleport")
        {
            const Value *target = message.field("p");
            const auto position = target != nullptr ? try_vec3(*target) : std::nullopt;
            if (position.has_value() && finite(*position))
                backend.local_teleport(*position);
        }
        else if (op == "give" || op == "take")
        {
            const auto item = static_cast<std::uint16_t>(
                std::clamp(field_float(message, "item", 0.0F), 0.0F, 65535.0F));
            const auto amount = static_cast<std::uint32_t>(
                std::clamp(field_float(message, "count", 0.0F), 0.0F, 4096.0F));
            const std::string key = field_string(message, "key");
            std::uint32_t mod_item{};
            if (key.find(':') != std::string::npos)
            {
                const auto found = client_item_ids.find(key);
                if (found == client_item_ids.end())
                    return;
                mod_item = found->second;
            }
            if (item == 0U || amount == 0U)
                return;
            if (op == "give")
            {
                std::uint32_t left = amount;
                while (left > 0U)
                {
                    const auto chunk = static_cast<std::uint16_t>(std::min<std::uint32_t>(left, 999U));
                    static_cast<void>(backend.local_give({item, chunk, mod_item}, true));
                    left -= chunk;
                }
            }
            else
                static_cast<void>(backend.local_take(item, mod_item, amount));
            client_report_timer = 0.0F;
        }
        else if (op == "clear")
            backend.local_clear_inventory();
        return;
    }
    if (type == "ent+")
    {
        const auto id = static_cast<std::uint64_t>(std::max(0.0F, field_float(message, "id", 0.0F)));
        if (id == 0U)
            return;
        auto existing = std::ranges::find(client_entities, id, &ClientEntity::id);
        if (existing == client_entities.end())
        {
            if (client_entities.size() >= limits.max_entities)
                return;
            client_entities.push_back({});
            existing = client_entities.end() - 1;
        }
        ClientEntity &entity = *existing;
        entity.id = id;
        entity.kind = field_string(message, "kind");
        entity.model = field_string(message, "model");
        entity.texture = field_string(message, "texture");
        entity.animation = field_string(message, "a");
        entity.name = field_string(message, "name");
        entity.width = std::clamp(field_float(message, "w", 0.6F), 0.1F, 8.0F);
        entity.height = std::clamp(field_float(message, "h", 1.8F), 0.1F, 8.0F);
        entity.interactable = bool_or(message, "interact", false);
        entity.health_fraction = std::clamp(field_float(message, "hp", 1.0F), 0.0F, 1.0F);
        const Value *position = message.field("p");
        const auto at = position != nullptr ? try_vec3(*position) : std::nullopt;
        entity.position = entity.from = entity.to = at.value_or(Vec3{});
        entity.yaw = entity.from_yaw = entity.to_yaw = field_float(message, "y", 0.0F);
        entity.blend = 1.0F;
        notify(ClientChange::entities);
        return;
    }
    if (type == "ent~")
    {
        const Value *list = message.field("list");
        if (list == nullptr || list->type() != ValueType::array)
            return;
        for (const Value &entry : list->as_array())
        {
            const auto id = static_cast<std::uint64_t>(std::max(0.0F, field_float(entry, "id", 0.0F)));
            const auto found = std::ranges::find(client_entities, id, &ClientEntity::id);
            if (found == client_entities.end())
                continue;
            const Value *position = entry.field("p");
            const auto at = position != nullptr ? try_vec3(*position) : std::nullopt;
            if (at.has_value() && finite(*at))
            {
                found->from = found->position;
                found->to = *at;
                found->from_yaw = found->yaw;
                found->to_yaw = field_float(entry, "y", found->to_yaw);
                found->blend = 0.0F;
            }
            if (const Value *animation = entry.field("a"); animation != nullptr && animation->type() == ValueType::string)
                found->animation = animation->as_string();
            if (const Value *health = entry.field("hp"); health != nullptr && health->is_number())
                found->health_fraction = std::clamp(static_cast<float>(health->as_number()), 0.0F, 1.0F);
            if (const Value *name = entry.field("name"); name != nullptr && name->type() == ValueType::string)
                found->name = name->as_string();
        }
        notify(ClientChange::entities);
        return;
    }
    if (type == "ent-")
    {
        const auto id = static_cast<std::uint64_t>(std::max(0.0F, field_float(message, "id", 0.0F)));
        std::erase_if(client_entities, [&](const ClientEntity &entity) { return entity.id == id; });
        // (Visuals) a death, not a despawn: the renderer still plays it.
        if (bool_or(message, "died", false))
        {
            client_deaths.push_back(id);
            if (client_deaths.size() > 64U)
                client_deaths.pop_front();
        }
        notify(ClientChange::entities);
        return;
    }
    if (type == "overlay")
    {
        const Value *cells = message.field("cells");
        if (cells == nullptr || cells->type() != ValueType::array)
            return;
        for (const Value &entry : cells->as_array())
        {
            const BlockPos cell{static_cast<int>(field_float(entry, "x", 0.0F)),
                                static_cast<int>(field_float(entry, "y", 0.0F)),
                                static_cast<int>(field_float(entry, "z", 0.0F))};
            const std::string key = field_string(entry, "b");
            const auto found = client_block_ids.find(key);
            if (key.empty() || found == client_block_ids.end())
                client_overlay.erase(cell);
            else if (client_overlay.size() < limits.max_overlay_cells)
                client_overlay[cell] = found->second;
        }
        notify(ClientChange::overlay);
        return;
    }
    if (type == "dialogue")
    {
        if (bool_or(message, "close", false))
        {
            if (client_dialogue.has_value())
            {
                client_dialogue.reset();
                emit(Side::client, "dialogue.closed", make_map({}));
            }
        }
        else
        {
            ClientDialogue dialogue;
            dialogue.session = static_cast<std::uint64_t>(std::max(0.0F, field_float(message, "session", 0.0F)));
            dialogue.title = field_string(message, "title");
            dialogue.speaker = field_string(message, "speaker");
            dialogue.text = field_string(message, "text");
            dialogue.portrait = field_string(message, "portrait");
            dialogue.entity = static_cast<std::uint64_t>(std::max(0.0F, field_float(message, "entity", 0.0F)));
            if (const Value *choices = message.field("choices"); choices != nullptr && choices->type() == ValueType::array)
                for (const Value &choice : choices->as_array())
                    if (choice.type() == ValueType::string && dialogue.choices.size() < 16U)
                        dialogue.choices.push_back(choice.as_string());
            client_dialogue = std::move(dialogue);
            emit(Side::client, "dialogue.opened",
                 make_map({{"session", static_cast<std::int64_t>(client_dialogue->session)},
                           {"speaker", client_dialogue->speaker},
                           {"text", client_dialogue->text}}));
        }
        notify(ClientChange::dialogue);
        return;
    }
    if (type == "quests")
    {
        client_quests.clear();
        if (const Value *list = message.field("list"); list != nullptr && list->type() == ValueType::array)
            for (const Value &entry : list->as_array())
            {
                if (client_quests.size() >= 64U)
                    break;
                ClientQuest quest;
                quest.id = field_string(entry, "id");
                quest.title = field_string(entry, "title");
                quest.description = field_string(entry, "description");
                quest.stage = field_string(entry, "stage");
                quest.completed = bool_or(entry, "completed", false);
                if (const Value *objectives = entry.field("objectives"); objectives != nullptr && objectives->type() == ValueType::array)
                    for (const Value &objective : objectives->as_array())
                        if (quest.objectives.size() < 16U)
                            quest.objectives.push_back(
                                {field_string(objective, "text"),
                                 static_cast<std::uint32_t>(std::max(0.0F, field_float(objective, "progress", 0.0F))),
                                 static_cast<std::uint32_t>(std::max(1.0F, field_float(objective, "required", 1.0F))),
                                 bool_or(objective, "done", false)});
                client_quests.push_back(std::move(quest));
            }
        emit(Side::client, "quest.updated", make_map({}));
        notify(ClientChange::quests);
        return;
    }
    if (type == "tags")
    {
        client_pending_tags.clear();
        // The inventory may still be arriving: keep trying for a while.
        client_tags_seconds = 10.0F;
        if (const Value *slots = message.field("slots"); slots != nullptr && slots->type() == ValueType::array)
            for (const Value &entry : slots->as_array())
                if (client_pending_tags.size() < 128U)
                    client_pending_tags.emplace_back(
                        static_cast<std::uint16_t>(std::clamp(field_float(entry, "slot", 0.0F), 0.0F, 127.0F)),
                        field_string(entry, "key"));
        return;
    }
    if (type == "chat")
    {
        backend.chat(backend.local_player(), field_string(message, "text").substr(0, 160U));
        return;
    }
}

void GameplayService::Impl::client_tick(float seconds)
{
    ai_client_tick(seconds); // mod-ai
    // Entity presentation: blend from the previous sample to the newest over
    // one replication period.
    for (ClientEntity &entity : client_entities)
    {
        if (entity.blend >= 1.0F)
            continue;
        entity.blend = std::min(1.0F, entity.blend + seconds * 10.0F);
        const double t = entity.blend;
        entity.position = {entity.from.x + (entity.to.x - entity.from.x) * t,
                           entity.from.y + (entity.to.y - entity.from.y) * t,
                           entity.from.z + (entity.to.z - entity.from.z) * t};
        float turn = entity.to_yaw - entity.from_yaw;
        while (turn > 3.14159265F)
            turn -= 6.2831853F;
        while (turn < -3.14159265F)
            turn += 6.2831853F;
        entity.yaw = entity.from_yaw + turn * static_cast<float>(t);
    }
    // Mod item identities the server restored for this player: applied once
    // the matching stacks are there.
    if (!client_pending_tags.empty())
    {
        std::erase_if(client_pending_tags, [&](const auto &tag) {
            const auto found = client_item_ids.find(tag.second);
            const ClientItemType *type =
                found != client_item_ids.end() ? owner.client_item(found->second) : nullptr;
            return type == nullptr ||
                   backend.local_tag_slot(tag.first, type->fallback, found->second);
        });
        client_tags_seconds -= seconds;
        if (client_tags_seconds <= 0.0F)
            client_pending_tags.clear();
    }
    // (Items) The mod item in this player's hand, to everyone, at once (also
    // from the host's own player: the others draw it).
    client_report_held();
    // Reports only travel when this process is not itself the server: the
    // server reads its own player live.
    if (owner.server_running())
        return;
    client_report_timer -= seconds;
    if (client_report_timer > 0.0F)
        return;
    client_report_timer = 2.0F;
    std::vector<PlayerSnapshot> snapshot;
    backend.players(snapshot);
    for (const PlayerSnapshot &seen : snapshot)
        if (seen.local && std::abs(seen.health - client_reported_health) >= 1.0F)
        {
            client_reported_health = seen.health;
            out().to_server(make_map({{"t", "report"}, {"health", seen.health}, {"dead", !seen.alive}}));
        }
    std::vector<std::pair<std::uint16_t, StackView>> slots;
    backend.local_slots(slots);
    std::map<std::string, std::uint32_t, std::less<>> counts;
    std::vector<std::pair<std::uint16_t, std::string>> tags;
    for (const auto &[slot, stack] : slots)
    {
        const ClientItemType *mod_item = owner.client_item(stack.mod_item);
        const std::string key =
            mod_item != nullptr ? mod_item->key : std::string{retail_item_name(stack.item)};
        if (key.empty())
            continue;
        counts[key] += stack.count;
        if (mod_item != nullptr)
            tags.emplace_back(slot, mod_item->key);
    }
    if (counts == client_reported_counts && tags == client_reported_tags)
        return;
    Map count_map;
    for (const auto &[key, amount] : counts)
        if (count_map.size() < 256U)
            count_map.emplace_back(key, static_cast<std::int64_t>(amount));
    Array tag_list;
    for (const auto &[slot, key] : tags)
        if (tag_list.size() < 64U)
            tag_list.push_back(make_map({{"slot", static_cast<std::int64_t>(slot)}, {"key", key}}));
    out().to_server(make_map({{"t", "inventory"}, {"counts", Value{std::move(count_map)}},
                              {"tags", Value{std::move(tag_list)}}}));
    client_reported_counts = std::move(counts);
    client_reported_tags = std::move(tags);
}

// --------------------------------------------------------------- service

GameplayService::GameplayService(ScriptHost &host, GameplayBackend &backend, GameplayLimits limits)
    : impl_{std::make_unique<Impl>(*this, host, backend, limits)},
      alive_{std::make_shared<bool>(true)}
{
    // When a side's VM goes away its callbacks go with it. The runtime may
    // outlive this service; the listener then finds it gone and does nothing.
    host.add_teardown_listener([this, alive = std::weak_ptr<bool>{alive_}](Side side) {
        if (alive.expired())
            return;
        if (side == Side::server)
            stop_server();
        else
            stop_client();
    });
}

GameplayService::~GameplayService()
{
    alive_.reset();
}

void GameplayService::set_sync(GameplaySync *sync) noexcept
{
    impl_->sync = sync;
}

LoopbackGameplaySync &GameplayService::loopback() noexcept
{
    return impl_->loopback;
}

void GameplayService::start_server_deferred()
{
    if (server_running_)
        return;
    server_running_ = true;
    Impl &impl = *impl_;
    impl.random_state = static_cast<std::uint32_t>(impl.backend.world_seed()) * 2654435761U + 1U;
    if (impl.random_state == 0U)
        impl.random_state = 0x9E3779B9U;
    impl.activated = false;
}

void GameplayService::start_server()
{
    start_server_deferred();
    if (!impl_->activated)
        impl_->activate();
}

void GameplayService::Impl::activate()
{
    activated = true;
    load_sidecar();
    apply_crate_tags(); // (Integration) the saved crates' mod items
    last_day = static_cast<int>(std::floor(backend.day()));
    last_phase = day_phase(backend.day());
    // Players a script reached before activation get their saved data now.
    for (PlayerRecord &player : players)
        if (player.present && !player.restored)
            restore(player);
    refresh_players();
    for (std::size_t index{}; index < players.size(); ++index)
        if (players[index].present && !players[index].announced)
        {
            players[index].announced = true;
            emit(Side::server, "player.joined",
                 make_map({{"player", handle_of(players[index])}, {"name", players[index].name}}));
        }
    // The saved world's inventory is in place by now: its mod items get
    // their identities back.
    if (PlayerRecord *local = local_player(); local != nullptr)
        for (const auto &[slot, key] : local->inventory_tags)
            if (const ItemDef *item = server_registry.item(key))
                static_cast<void>(backend.local_tag_slot(slot, item->fallback, item->id));
    // A world already entered: its player is in it. (fix) One still loading
    // announces its player from the spawn (local_player_spawned).
    if (PlayerRecord *local = local_player();
        local != nullptr && local->alive && !local->spawned && backend.local_player_placed())
    {
        local->spawned = true;
        emit(Side::server, "player.spawned",
             make_map({{"player", handle_of(*local)}, {"position", vec3_value(local->position)}}));
    }
    out().to_all(registry_snapshot(), true);
    // This process's own client half (offline, or the host's player) gets
    // its full state straight away.
    if (owner.client_running())
        if (const auto local = backend.local_player())
            owner.send_full_state(*local);
}

void GameplayService::start_client()
{
    if (client_running_)
        return;
    client_running_ = true;
    if (server_running_ && impl_->activated)
    {
        if (const auto local = impl_->backend.local_player())
            send_full_state(*local);
        return;
    }
    // A joined client asks for its state (again once its channel is ready).
    impl_->out().to_server(make_map({{"t", "ready"}}));
}

void GameplayService::client_channel_ready()
{
    if (client_running_ && !server_running_)
        impl_->out().to_server(make_map({{"t", "ready"}}));
}

void GameplayService::stop_server()
{
    if (!server_running_)
    {
        // (Integration) Players admitted for a session whose server half
        // never ran (its VM did not start): forgotten with the session.
        if (!impl_->refreshing_players)
            impl_->players.clear();
        return;
    }
    Impl &impl = *impl_;
    // Never before activation: the sidecar was not read yet, and writing
    // would replace it with nothing.
    if (impl.activated)
        save();
    for (PlayerRecord &player : impl.players)
        player.present = false;
    impl.players.clear();
    impl.entities.clear();
    impl.edit_jobs.clear();
    impl.overlay.clear();
    impl.overlay_outbox.clear();
    impl.decorated_chunks.clear();
    impl.crate_tags.clear();
    impl.pickup_tags.clear();
    impl.pickup_order.clear();
    impl.worldgen_queue.clear();
    impl.worldgen_queued.clear();
    impl.persisted_players.clear();
    impl.difficulty = {};
    impl.ai_stop_server(); // mod-ai
    impl.release_side(Side::server);
    impl.server_registry = {};
    impl.refresh_liquids(); // (mod-liquid)
    impl.loaded_sidecar = false;
    impl.activated = false;
    server_running_ = false;
}

void GameplayService::stop_client()
{
    if (!client_running_)
        return;
    Impl &impl = *impl_;
    impl.release_side(Side::client);
    impl.client_registry = {};
    impl.client_stats.clear();
    impl.client_effects.clear();
    impl.client_entities.clear();
    impl.client_dialogue.reset();
    impl.client_quests.clear();
    impl.client_difficulty = {};
    impl.ai_stop_client(); // mod-ai
    if (impl.client_speed != 1.0F)
        impl.backend.local_set_speed(1.0F);
    impl.client_speed = 1.0F;
    impl.client_blocks.clear();
    impl.client_items.clear();
    impl.client_recipes.clear();
    impl.client_block_ids.clear();
    impl.client_item_ids.clear();
    impl.client_overlay.clear();
    // (Visuals) every view changed: pollers start over.
    impl.client_deaths.clear();
    for (std::uint64_t &revision : impl.client_revisions)
        ++revision;
    impl.client_disabled_recipes.clear();
    impl.client_crate_tags.clear();
    impl.client_pickup_tags.clear();
    impl.client_pickup_order.clear();
    impl.client_pending_tags.clear();
    impl.client_reported_counts.clear();
    impl.client_reported_tags.clear();
    impl.client_reported_health = -1.0F;
    impl.client_held.clear(); // (Items)
    impl.client_held_known = false;
    client_running_ = false;
}

void GameplayService::save()
{
    if (!server_running_ || !impl_->activated)
        return;
    for (PlayerRecord &player : impl_->players)
        if (player.present)
        {
            if (player.local)
            {
                // The local player's mod item identities, read live.
                std::vector<std::pair<std::uint16_t, StackView>> slots;
                impl_->backend.local_slots(slots);
                player.inventory_tags.clear();
                for (const auto &[slot, stack] : slots)
                    if (const ItemDef *item = impl_->server_registry.item(stack.mod_item))
                        player.inventory_tags.emplace_back(slot, item->key);
            }
            impl_->store_persisted(player);
        }
    impl_->save_sidecar();
}

void GameplayService::restore_local_inventory_tags()
{
    if (!server_running_)
        return;
    Impl &impl = *impl_;
    impl.refresh_players();
    PlayerRecord *player = impl.local_player();
    if (player == nullptr)
        return;
    for (const auto &[slot, key] : player->inventory_tags)
        if (const ItemDef *item = impl.server_registry.item(key))
            static_cast<void>(impl.backend.local_tag_slot(slot, item->fallback, item->id));
}

void GameplayService::tick(float seconds)
{
    if (!(seconds > 0.0F) || !std::isfinite(seconds))
        return;
    Impl &impl = *impl_;
    if (server_running_ && !impl.activated)
        impl.activate();
    if (server_running_)
    {
        impl.server_time += seconds;
        ++impl.tick_count;
        impl.refresh_players();
        // Time of day: new day, dawn and dusk.
        const double day = impl.backend.day();
        const int day_number = static_cast<int>(std::floor(day));
        if (day_number != impl.last_day)
        {
            if (impl.last_day >= 0 && day_number > impl.last_day)
                impl.emit(Side::server, "time.new_day", make_map({{"day", day_number + 1}}));
            impl.last_day = day_number;
        }
        const int phase = day_phase(day);
        if (phase != impl.last_phase)
        {
            impl.last_phase = phase;
            impl.emit(Side::server, phase == 0 ? "time.day_started" : "time.night_started",
                      make_map({{"day", day_number + 1}}));
        }
        for (PlayerRecord &player : impl.players)
            if (player.present)
            {
                player.tokens = std::min(impl.limits.client_messages_per_second * 2.0F,
                                         player.tokens +
                                             impl.limits.client_messages_per_second * seconds);
                player.attack_tokens = std::min(20.0F, player.attack_tokens + 10.0F * seconds);
                player.item_cooldown = std::max(0.0F, player.item_cooldown - seconds);
                impl.tick_stats_and_effects(player, seconds);
            }
        impl.run_jobs();
        impl.tick_entities(seconds);
        impl.tick_ai_world(seconds); // mod-ai: projectiles, bosses, noise, retail rules
        impl.spawn_timer -= seconds;
        if (impl.spawn_timer <= 0.0F)
        {
            impl.spawn_timer = 1.0F;
            impl.spawn_rules();
            impl.queue_worldgen();
        }
        impl.run_worldgen();
        impl.quest_timer -= seconds;
        if (impl.quest_timer <= 0.0F)
        {
            impl.quest_timer = 0.5F;
            impl.tick_quests();
        }
        impl.sweep_timer -= seconds;
        if (impl.sweep_timer <= 0.0F)
        {
            impl.sweep_timer = 2.0F;
            impl.sweep_overlay();
        }
        impl.flush_overlay();
        if (impl.difficulty_dirty)
        {
            impl.difficulty_dirty = false;
            Array per_type;
            for (const auto &[type, scales] : impl.difficulty.per_type)
                per_type.push_back(make_map({{"type", type}, {"health", scales[0]}, {"damage", scales[1]}}));
            impl.out().to_all(make_map({{"t", "difficulty"},
                                        {"active", impl.difficulty.active},
                                        {"enemy_health", impl.difficulty.enemy_health},
                                        {"enemy_damage", impl.difficulty.enemy_damage},
                                        {"spawn_rate", impl.difficulty.spawn_rate},
                                        {"per_type", Value{std::move(per_type)}}}),
                              true);
        }
        // Positions and stats go out ten times a second; anything that must
        // not wait (spawns, removals, effects, quests) goes at once.
        impl.replicate_timer -= seconds;
        impl.replicate_due = impl.replicate_timer <= 0.0F;
        if (impl.replicate_due)
            impl.replicate_timer = 0.1F;
        impl.replicate_entities(false);
        for (PlayerRecord &player : impl.players)
            if (player.present)
            {
                impl.replicate_player(player, false);
                if (player.quests_dirty)
                    impl.replicate_quests(player);
            }
    }
    if (client_running_)
        impl.client_tick(seconds);
}

// ------------------------------------------------------------ local hooks

float GameplayService::local_player_damaging(float amount, DamageCause cause, Vec3 source)
{
    Impl &impl = *impl_;
    if (server_running_)
    {
        PlayerRecord *player = impl.local_player();
        if (player == nullptr)
            return amount;
        return impl.filter_local_damage(*player, amount, cause, source);
    }
    if (client_running_ && amount > 0.0F)
        impl.out().to_server(make_map({{"t", "hurt"}, {"amount", amount}, {"cause", damage_cause_name(cause)}}));
    return amount;
}

void GameplayService::local_player_died(Vec3 position, DamageCause cause)
{
    Impl &impl = *impl_;
    if (server_running_)
    {
        PlayerRecord *player = impl.local_player();
        if (player == nullptr || !player->alive)
            return;
        player->alive = false;
        player->health = 0.0F;
        player->position = position;
        if (player->dialogue.has_value())
            impl.close_dialogue(*player, true);
        impl.emit(Side::server, "player.died",
                  make_map({{"player", impl.handle_of(*player)},
                            {"position", vec3_value(position)},
                            {"cause", damage_cause_name(cause)}}));
        return;
    }
    if (client_running_)
        impl.out().to_server(make_map({{"t", "died"}, {"cause", damage_cause_name(cause)}, {"p", vec3_value(position)}}));
}

void GameplayService::local_player_spawned(Vec3 position)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return;
    impl.refresh_players();
    PlayerRecord *player = impl.local_player();
    if (player == nullptr)
        return;
    player->position = position;
    player->alive = true;
    if (player->spawned)
        return;
    player->spawned = true;
    impl.emit(Side::server, "player.spawned",
              make_map({{"player", impl.handle_of(*player)}, {"position", vec3_value(position)}}));
}

void GameplayService::local_player_respawned(Vec3 position)
{
    Impl &impl = *impl_;
    if (server_running_)
    {
        PlayerRecord *player = impl.local_player();
        if (player == nullptr)
            return;
        player->alive = true;
        player->health = 100.0F;
        player->position = position;
        impl.emit(Side::server, "player.respawned",
                  make_map({{"player", impl.handle_of(*player)}, {"position", vec3_value(position)}}));
        return;
    }
    if (client_running_)
        impl.out().to_server(make_map({{"t", "respawned"}, {"p", vec3_value(position)}}));
}

bool GameplayService::local_block_breaking(BlockPos cell, std::uint8_t block)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return true;
    PlayerRecord *player = impl.local_player();
    // (The dig already emptied the cell: a generated identity is the one it
    // had before - mod-world.)
    std::uint16_t mod_block = impl.mod_block_at(cell, block);
    if (mod_block == 0U)
        mod_block = impl.generated_block_at(cell, block, true);
    return impl.emit(Side::server, "block.breaking",
                     make_map({{"player", player != nullptr ? Value{impl.handle_of(*player)} : Value{}},
                               {"position", cell_value(cell)},
                               {"block", impl.block_label(block, mod_block)}}));
}

void GameplayService::local_block_broken(BlockPos cell, std::uint8_t block)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return;
    PlayerRecord *player = impl.local_player();
    const auto mod = impl.overlay.find(cell);
    const std::uint16_t mod_block = mod != impl.overlay.end()
                                        ? mod->second
                                        : impl.generated_block_at(cell, block, true); // mod-world
    impl.forget_overlay(cell);
    impl.ai_world_edit(cell, player != nullptr ? player->serial : 0U, 8.0F); // mod-ai: creatures hear digging
    impl.emit(Side::server, "block.broken",
              make_map({{"player", player != nullptr ? Value{impl.handle_of(*player)} : Value{}},
                        {"position", cell_value(cell)},
                        {"block", impl.block_label(block, mod_block)}}));
}

bool GameplayService::local_block_drops(BlockPos cell, std::uint8_t block)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return true;
    const auto mod = impl.overlay.find(cell);
    // mod-world: or the mod block a world generator put there.
    const std::uint16_t mod_block =
        mod != impl.overlay.end() ? mod->second : impl.generated_block_at(cell, block, true);
    if (mod_block == 0U)
        return true;
    const BlockDef *definition = impl.server_registry.block(mod_block);
    if (definition == nullptr || block != definition->fallback)
        return true;
    const Vec3 at{cell.x + 0.5, cell.y + 0.5, cell.z + 0.5};
    std::vector<std::pair<ItemRef, std::uint16_t>> drops;
    const std::string own_table = std::format("{}:{}", definition->mod, definition->drops);
    if (impl.server_registry.loot.contains(definition->drops))
        drops = impl.roll_loot(definition->drops);
    else if (impl.server_registry.loot.contains(own_table))
        drops = impl.roll_loot(own_table);
    else if (!definition->drops.empty() && definition->drops != "none")
    {
        try
        {
            const ItemRef item = impl.resolve_item(Value{definition->drops}, definition->mod, "drops");
            drops.emplace_back(item, definition->drop_count);
        }
        catch (const ScriptError &error)
        {
            impl.log(definition->mod, LogLevel::warning,
                     std::format("block '{}': {}", definition->key, error.what()));
        }
    }
    else if (const ItemDef *same = impl.server_registry.item(definition->key))
        drops.emplace_back(ItemRef{same->fallback, same->id}, std::uint16_t{1U});
    if (drops.empty() && definition->drops == "none")
        return false;
    if (drops.empty())
        return true;
    for (const auto &[item, amount] : drops)
        static_cast<void>(impl.backend.spawn_pickup(at, {item.item, amount, item.mod_item}));
    return false;
}

void GameplayService::local_blocks_blasted(std::span<const BlockPos> cells,
                                           std::span<const std::uint8_t> previous)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return;
    PlayerRecord *player = impl.local_player();
    const Value who = player != nullptr ? Value{impl.handle_of(*player)} : Value{};
    for (std::size_t index{}; index < cells.size() && index < 4096U; ++index)
    {
        const auto mod = impl.overlay.find(cells[index]);
        const std::uint16_t mod_block =
            mod != impl.overlay.end() ? mod->second
            : index < previous.size()  ? impl.generated_block_at(cells[index], previous[index], true)
                                       : std::uint16_t{0U}; // mod-world
        impl.forget_overlay(cells[index]);
        impl.ai_world_edit(cells[index], player != nullptr ? player->serial : 0U, index == 0U ? 48.0F : 0.0F); // mod-ai
        impl.emit(Side::server, "block.broken",
                  make_map({{"player", who},
                            {"position", cell_value(cells[index])},
                            {"block", impl.block_label(index < previous.size() ? previous[index] : 0U, mod_block)},
                            {"explosion", true}}));
    }
}

bool GameplayService::local_block_placing(BlockPos cell, std::uint8_t block, StackView held)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return true;
    PlayerRecord *player = impl.local_player();
    const ItemDef *item = impl.server_registry.item(held.mod_item);
    const std::uint16_t mod_block = item != nullptr ? item->places_block : 0U;
    return impl.emit(Side::server, "block.placing",
                     make_map({{"player", player != nullptr ? Value{impl.handle_of(*player)} : Value{}},
                               {"position", cell_value(cell)},
                               {"block", impl.block_label(block, mod_block)},
                               {"item", impl.item_label({held.item, held.mod_item})}}));
}

void GameplayService::local_block_placed(BlockPos cell, std::uint8_t block, StackView held)
{
    Impl &impl = *impl_;
    if (!server_running_)
    {
        // A joined client's mod block: the stock AlterBlockMessage carried
        // the fallback; the identity goes to the server separately.
        const ClientItemType *item = client_running_ ? client_item(held.mod_item) : nullptr;
        if (item != nullptr && !item->places.empty())
            impl.out().to_server(make_map({{"t", "placed"}, {"x", cell.x}, {"y", cell.y}, {"z", cell.z},
                                           {"key", item->places}}));
        return;
    }
    PlayerRecord *player = impl.local_player();
    const ItemDef *item = impl.server_registry.item(held.mod_item);
    std::uint16_t mod_block{};
    if (item != nullptr && item->places_block != 0U)
    {
        const BlockDef *placed = impl.server_registry.block(item->places_block);
        if (placed != nullptr && placed->fallback == block)
        {
            mod_block = placed->id;
            impl.overlay[cell] = mod_block;
            impl.overlay_outbox.emplace_back(cell, mod_block);
        }
    }
    impl.emit(Side::server, "block.placed",
              make_map({{"player", player != nullptr ? Value{impl.handle_of(*player)} : Value{}},
                        {"position", cell_value(cell)},
                        {"block", impl.block_label(block, mod_block)}}));
}

bool GameplayService::local_item_using(StackView held)
{
    Impl &impl = *impl_;
    // (Integration) What the player aims at, for item.used and use callbacks.
    const std::optional<LocalView> view = impl.backend.local_view();
    if (server_running_)
    {
        PlayerRecord *player = impl.local_player();
        if (player == nullptr)
            return true;
        const Value aim = view.has_value() ? impl.aim_value(view->target, view->target_adjacent, view->look,
                                                            view->liquid) // (mod-liquid)
                                           : Value{};
        const ItemDef *item = impl.server_registry.item(held.mod_item);
        if (item != nullptr && item->on_use.valid())
        {
            if (player->item_cooldown <= 0.0F)
            {
                player->item_cooldown = std::max(0.05F, item->cooldown);
                impl.use_item(*player, {held.item, held.mod_item}, aim);
            }
            return false;
        }
        return impl.emit(Side::server, "item.used",
                         make_map({{"player", impl.handle_of(*player)},
                                   {"item", impl.item_label({held.item, held.mod_item})},
                                   {"mod_item", item != nullptr ? Value{item->key} : Value{}},
                                   {"target", aim}}));
    }
    if (client_running_)
    {
        const ClientItemType *item = client_item(held.mod_item);
        if (item != nullptr && item->usable)
        {
            Map request{{"t", Value{"use"}}, {"key", Value{item->key}}};
            if (view.has_value())
            {
                request.emplace_back("look", vec3_value(view->look));
                if (view->target.has_value())
                    request.emplace_back("aim", cell_value(*view->target));
                if (view->target_adjacent.has_value())
                    request.emplace_back("face", cell_value(*view->target_adjacent));
                if (view->liquid.has_value()) // (mod-liquid)
                    request.emplace_back("liquid", cell_value(*view->liquid));
            }
            impl.out().to_server(Value{std::move(request)});
            return false;
        }
    }
    return true;
}

void GameplayService::local_item_crafted(std::uint16_t item, std::uint32_t count,
                                         std::uint32_t mod_item)
{
    Impl &impl = *impl_;
    if (server_running_)
    {
        PlayerRecord *player = impl.local_player();
        if (player == nullptr)
            return;
        impl.emit(Side::server, "item.crafted",
                  make_map({{"player", impl.handle_of(*player)},
                            {"item", impl.item_label({item, mod_item})},
                            {"count", static_cast<std::int64_t>(count)}}));
        impl.quest_event(*player, ObjectiveKind::collect, {}, 0U);
        return;
    }
    if (client_running_)
    {
        const ClientItemType *type = client_item(mod_item);
        impl.out().to_server(make_map({{"t", "crafted"},
                                       {"item", static_cast<std::int64_t>(item)},
                                       {"count", static_cast<std::int64_t>(count)},
                                       {"key", type != nullptr ? type->key : std::string{}}}));
    }
}

ChatVerdict GameplayService::local_chat(std::string_view text)
{
    Impl &impl = *impl_;
    if (server_running_)
    {
        PlayerRecord *player = impl.local_player();
        return player != nullptr ? impl.chat(*player, text) : ChatVerdict::pass;
    }
    if (client_running_ && !text.empty() && text.front() == '/')
    {
        // Commands go to the server over the mod channel, never into chat.
        impl.out().to_server(make_map({{"t", "command"}, {"text", text.substr(0, 240U)}}));
        return ChatVerdict::command;
    }
    return ChatVerdict::pass;
}

bool GameplayService::local_enemy_spawning(int type, Vec3 position)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return true;
    PlayerRecord *player = impl.local_player();
    return impl.emit(Side::server, "enemy.spawning",
                     make_map({{"type", enemy_type_name(type)},
                               {"type_id", type},
                               {"position", vec3_value(position)},
                               {"player", player != nullptr ? Value{impl.handle_of(*player)} : Value{}}}));
}

void GameplayService::local_enemy_spawned(int type, std::uint32_t enemy_id, std::uint8_t target_gid,
                                          Vec3 position)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return;
    PlayerRecord *target = impl.player_by_gid(target_gid);
    impl.emit(Side::server, "enemy.spawned",
              make_map({{"type", enemy_type_name(type)},
                        {"type_id", type},
                        {"id", static_cast<std::int64_t>(enemy_id)},
                        {"position", vec3_value(position)},
                        {"player", target != nullptr ? Value{impl.handle_of(*target)} : Value{}}}));
}

void GameplayService::local_enemy_killed(int type, std::uint32_t enemy_id, std::uint8_t target_gid,
                                         Vec3 position)
{
    Impl &impl = *impl_;
    if (!server_running_)
        return;
    PlayerRecord *killer = impl.local_player();
    PlayerRecord *target = impl.player_by_gid(target_gid);
    impl.emit(Side::server, "enemy.killed",
              make_map({{"type", enemy_type_name(type)},
                        {"type_id", type},
                        {"id", static_cast<std::int64_t>(enemy_id)},
                        {"position", vec3_value(position)},
                        {"player", target != nullptr ? Value{impl.handle_of(*target)} : Value{}},
                        {"killer", killer != nullptr ? Value{impl.handle_of(*killer)} : Value{}}}));
    if (killer != nullptr)
        impl.quest_event(*killer, ObjectiveKind::kill, enemy_type_name(type), 1U);
    // Extra drops a mod attached to this enemy type.
    const auto [first, last] = impl.server_registry.enemy_loot.equal_range(type);
    for (auto table = first; table != last; ++table)
        for (const auto &[item, amount] : impl.roll_loot(table->second))
            static_cast<void>(impl.backend.spawn_pickup(position, {item.item, amount, item.mod_item}));
}

std::optional<float> GameplayService::local_attack(Vec3 origin, Vec3 direction, float reach,
                                                   float damage, std::uint16_t weapon,
                                                   std::uint32_t mod_item)
{
    Impl &impl = *impl_;
    if (server_running_)
    {
        const auto hit = impl.trace_entities(origin, direction, reach, false);
        if (!hit.has_value())
            return std::nullopt;
        PlayerRecord *player = impl.local_player();
        impl.damage_entity(*hit->first, damage, player != nullptr ? player->serial : 0U, 0U);
        return hit->second;
    }
    if (!client_running_)
        return std::nullopt;
    // A joined client traces what it draws and asks the server.
    const double length = std::sqrt(distance_squared(direction, {}));
    if (!(length > 0.0))
        return std::nullopt;
    const Vec3 unit{direction.x / length, direction.y / length, direction.z / length};
    std::optional<std::pair<std::uint64_t, float>> best;
    for (const ClientEntity &entity : impl.client_entities)
    {
        const double half = entity.width * 0.5;
        const std::array<double, 3> lo{entity.position.x - half, entity.position.y, entity.position.z - half};
        const std::array<double, 3> hi{entity.position.x + half, entity.position.y + entity.height,
                                       entity.position.z + half};
        const std::array<double, 3> o{origin.x, origin.y, origin.z};
        const std::array<double, 3> d{unit.x, unit.y, unit.z};
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
            best = std::pair{entity.id, static_cast<float>(near)};
    }
    if (!best.has_value())
        return std::nullopt;
    Map attack{{"t", Value{"attack"}},
               {"entity", Value{static_cast<std::int64_t>(best->first)}},
               {"damage", Value{damage}},
               {"weapon", Value{static_cast<std::int64_t>(weapon)}},
               {"dir", vec3_value(unit)}}; // mod-ai: the host checks the aim
    // (Items) The mod weapon it was struck with: the host judges it by that.
    if (const ClientItemType *type = client_item(mod_item); type != nullptr && type->weapon.has_value())
        attack.emplace_back("key", Value{type->key});
    impl.out().to_server(Value{std::move(attack)});
    return best->second;
}

bool GameplayService::local_interact(Vec3 origin, Vec3 direction, float reach)
{
    Impl &impl = *impl_;
    if (server_running_)
    {
        const auto hit = impl.trace_entities(origin, direction, reach, true);
        PlayerRecord *player = impl.local_player();
        return hit.has_value() && player != nullptr && impl.interact(*player, *hit->first);
    }
    if (!client_running_)
        return false;
    for (const ClientEntity &entity : impl.client_entities)
    {
        if (!entity.interactable)
            continue;
        const Vec3 centre{entity.position.x, entity.position.y + entity.height * 0.5, entity.position.z};
        const Vec3 to{centre.x - origin.x, centre.y - origin.y, centre.z - origin.z};
        const double length = std::sqrt(distance_squared(direction, {}));
        if (!(length > 0.0))
            return false;
        const double along = (to.x * direction.x + to.y * direction.y + to.z * direction.z) / length;
        if (along < 0.0 || along > reach)
            continue;
        const double off = distance_squared(to, {}) - along * along;
        if (off <= std::pow(std::max(entity.width, entity.height * 0.5F), 2.0F))
        {
            impl.out().to_server(make_map({{"t", "interact"}, {"entity", static_cast<std::int64_t>(entity.id)}}));
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------- host hooks

Handle GameplayService::admit_player(std::uint8_t gid, std::string_view name, bool local)
{
    Impl &impl = *impl_;
    PlayerRecord &player = impl.join(gid, name, local, false);
    player.admitted = true;
    return impl.handle_of(player);
}

void GameplayService::remote_player_joined(std::uint8_t gid, std::string_view name)
{
    Impl &impl = *impl_;
    PlayerRecord &player = impl.join(gid, name, false, false);
    player.admitted = true;
    // Where the game already knows it to be (a joiner usually announces its
    // position just after its channel opens; the record follows each tick).
    std::vector<PlayerSnapshot> snapshot;
    impl.backend.players(snapshot);
    for (const PlayerSnapshot &seen : snapshot)
        if (seen.gid == gid && finite(seen.position))
        {
            player.position = seen.position;
            player.yaw = seen.yaw;
        }
    impl.announce(player);
}

void GameplayService::remote_player_left(std::uint8_t gid)
{
    PlayerRecord *player = impl_->player_by_gid(gid);
    if (player == nullptr)
        return;
    if (server_running_)
        impl_->leave(*player);
    else
    {
        // Admitted before any server VM ran: nobody heard of it.
        player->present = false;
        player->admitted = false;
    }
}

void GameplayService::remote_player_dead(std::uint8_t gid, bool dead, Vec3 position)
{
    if (!server_running_)
        return;
    PlayerRecord *player = impl_->player_by_gid(gid);
    if (player == nullptr || player->local || player->alive == !dead)
        return;
    player->alive = !dead;
    if (finite(position))
        player->position = position;
    impl_->emit(Side::server, dead ? "player.died" : "player.respawned",
                make_map({{"player", impl_->handle_of(*player)}, {"position", vec3_value(player->position)}}));
}

// --- (Authority) ---------------------------------------------------------------

void GameplayService::set_authority(bool on) noexcept
{
    authority_ = on;
}

bool GameplayService::authority() const noexcept
{
    return authority_;
}

float GameplayService::authority_player_damaging(std::uint8_t gid, float amount, DamageCause cause,
                                                 Vec3 source)
{
    if (!server_running_)
        return amount;
    PlayerRecord *player = impl_->player_by_gid(gid);
    if (player == nullptr)
        return amount;
    return impl_->filter_local_damage(*player, amount, cause, source);
}

float GameplayService::player_speed_multiplier(std::uint8_t gid) const
{
    if (!server_running_)
        return 1.0F;
    const PlayerRecord *player = impl_->player_by_gid(gid);
    return player != nullptr ? impl_->speed_multiplier(*player) : 1.0F;
}

void GameplayService::authority_player_health(std::uint8_t gid, float health, bool alive,
                                              Vec3 position, DamageCause cause)
{
    if (!server_running_)
        return;
    PlayerRecord *player = impl_->player_by_gid(gid);
    if (player == nullptr || player->local)
        return;
    if (std::isfinite(health))
        player->health = std::clamp(health, 0.0F, 100.0F);
    if (player->alive == alive)
        return;
    player->alive = alive;
    if (finite(position))
        player->position = position;
    if (alive)
        impl_->emit(Side::server, "player.respawned",
                    make_map({{"player", impl_->handle_of(*player)},
                              {"position", vec3_value(player->position)}}));
    else
        impl_->emit(Side::server, "player.died",
                    make_map({{"player", impl_->handle_of(*player)},
                              {"position", vec3_value(player->position)},
                              {"cause", damage_cause_name(cause)}}));
}

bool GameplayService::remote_block_edit(std::uint8_t gid, BlockPos cell, std::uint8_t previous,
                                        std::uint8_t block)
{
    if (!server_running_)
        return true;
    Impl &impl = *impl_;
    PlayerRecord *player = impl.player_by_gid(gid);
    const Value who = player != nullptr ? Value{impl.handle_of(*player)} : Value{};
    if (block == 0U && previous != 0U)
    {
        const std::uint16_t mod_block = impl.mod_block_at(cell, previous);
        if (!impl.emit(Side::server, "block.breaking",
                       make_map({{"player", who}, {"position", cell_value(cell)},
                                 {"block", impl.block_label(previous, mod_block)}})))
            return false;
        impl.forget_overlay(cell);
        impl.ai_world_edit(cell, player != nullptr ? player->serial : 0U, 8.0F); // mod-ai
        impl.emit(Side::server, "block.broken",
                  make_map({{"player", who}, {"position", cell_value(cell)},
                            {"block", impl.block_label(previous, mod_block)}}));
        return true;
    }
    if (previous == 0U && block != 0U)
    {
        if (!impl.emit(Side::server, "block.placing",
                       make_map({{"player", who}, {"position", cell_value(cell)},
                                 {"block", impl.block_label(block, 0U)}})))
            return false;
        impl.ai_world_edit(cell, player != nullptr ? player->serial : 0U, 6.0F); // mod-ai
        impl.emit(Side::server, "block.placed",
                  make_map({{"player", who}, {"position", cell_value(cell)},
                            {"block", impl.block_label(block, 0U)}}));
        return true;
    }
    // A door toggling or a spawner light: not a build or a dig.
    if (impl.overlay.contains(cell))
        impl.forget_overlay(cell);
    return true;
}

bool GameplayService::remote_blocks_removed(std::uint8_t gid, std::span<const BlockPos> cells)
{
    if (!server_running_)
        return true;
    Impl &impl = *impl_;
    PlayerRecord *player = impl.player_by_gid(gid);
    const Value who = player != nullptr ? Value{impl.handle_of(*player)} : Value{};
    for (const BlockPos &cell : cells)
    {
        const std::uint8_t previous = impl.block_at(cell).value_or(0U);
        if (previous == 0U)
            continue;
        const std::uint16_t mod_block = impl.mod_block_at(cell, previous);
        if (!impl.emit(Side::server, "block.breaking",
                       make_map({{"player", who}, {"position", cell_value(cell)},
                                 {"block", impl.block_label(previous, mod_block)}})))
            return false;
    }
    for (const BlockPos &cell : cells)
    {
        const auto mod = impl.overlay.find(cell);
        const std::uint16_t mod_block = mod != impl.overlay.end() ? mod->second : 0U;
        impl.forget_overlay(cell);
        impl.ai_world_edit(cell, player != nullptr ? player->serial : 0U, &cell == cells.data() ? 48.0F : 0.0F); // mod-ai
        impl.emit(Side::server, "block.broken",
                  make_map({{"player", who}, {"position", cell_value(cell)},
                            {"block", impl.block_label(impl.block_at(cell).value_or(0U), mod_block)}}));
    }
    return true;
}

ChatVerdict GameplayService::remote_chat(std::uint8_t gid, std::string_view text)
{
    if (!server_running_)
        return ChatVerdict::pass;
    PlayerRecord *player = impl_->player_by_gid(gid);
    if (player == nullptr)
        return ChatVerdict::pass;
    if (player->tokens < 1.0F)
        return ChatVerdict::cancel;
    player->tokens -= 1.0F;
    return impl_->chat(*player, text);
}

void GameplayService::remote_enemy_spawned(std::uint8_t gid, int type, std::uint32_t enemy_id,
                                           Vec3 position)
{
    if (!server_running_)
        return;
    PlayerRecord *player = impl_->player_by_gid(gid);
    impl_->emit(Side::server, "enemy.spawned",
                make_map({{"type", enemy_type_name(type)},
                          {"type_id", type},
                          {"id", static_cast<std::int64_t>(enemy_id)},
                          {"position", vec3_value(position)},
                          {"player", player != nullptr ? Value{impl_->handle_of(*player)} : Value{}}}));
}

void GameplayService::remote_enemy_killed(std::uint8_t killer_gid, int type, std::uint32_t enemy_id,
                                          std::uint8_t target_gid, Vec3 position)
{
    if (!server_running_)
        return;
    Impl &impl = *impl_;
    PlayerRecord *killer = impl.player_by_gid(killer_gid);
    PlayerRecord *target = impl.player_by_gid(target_gid);
    impl.emit(Side::server, "enemy.killed",
              make_map({{"type", enemy_type_name(type)},
                        {"type_id", type},
                        {"id", static_cast<std::int64_t>(enemy_id)},
                        {"position", vec3_value(position)},
                        {"player", target != nullptr ? Value{impl.handle_of(*target)} : Value{}},
                        {"killer", killer != nullptr ? Value{impl.handle_of(*killer)} : Value{}}}));
    if (killer != nullptr)
        impl.quest_event(*killer, ObjectiveKind::kill, enemy_type_name(type), 1U);
}

// ------------------------------------------------------------ replication

void GameplayService::receive_from_client(std::uint8_t gid, const Value &message)
{
    if (!server_running_ || message.type() != ValueType::map)
        return;
    Impl &impl = *impl_;
    PlayerRecord *player = impl.player_by_gid(gid);
    if (player == nullptr)
        return;
    // Before any script runs: the per-player message budget.
    if (!player->local)
    {
        if (player->tokens < 1.0F)
            return;
        player->tokens -= 1.0F;
    }
    try
    {
        impl.handle_client_message(*player, message);
    }
    catch (const ScriptError &error)
    {
        impl.log({}, LogLevel::warning,
                 std::format("gameplay: refused a message from player {}: {}", gid, error.what()));
    }
}

void GameplayService::receive_from_server(const Value &message)
{
    if (!client_running_ || message.type() != ValueType::map)
        return;
    try
    {
        impl_->client_receive(message);
    }
    catch (const ScriptError &error)
    {
        impl_->log({}, LogLevel::warning,
                   std::format("gameplay: ignored a malformed server message: {}", error.what()));
    }
}

Value GameplayService::registry_snapshot() const
{
    return impl_->registry_snapshot();
}

void GameplayService::send_full_state(std::uint8_t gid)
{
    if (!server_running_)
        return;
    Impl &impl = *impl_;
    PlayerRecord *player = impl.player_by_gid(gid);
    if (player == nullptr)
        return;
    GameplaySync &out = impl.out();
    out.to_player(gid, impl.registry_snapshot(), true);
    std::vector<std::pair<BlockPos, std::uint16_t>> cells{impl.overlay.begin(), impl.overlay.end()};
    for (std::size_t offset{}; offset < cells.size(); offset += 256U)
    {
        const std::vector<std::pair<BlockPos, std::uint16_t>> part{
            cells.begin() + static_cast<std::ptrdiff_t>(offset),
            cells.begin() + static_cast<std::ptrdiff_t>(std::min(cells.size(), offset + 256U))};
        out.to_player(gid, impl.overlay_value(part), true);
    }
    // mod-ai: entities reach it by interest (replicate_entities, within
    // AiLimits::interest_radius), starting with the next pass; and the
    // retail rules, projectiles in flight and boss bars.
    impl.ai_send_full_state(*player);
    Array per_type;
    for (const auto &[type, scales] : impl.difficulty.per_type)
        per_type.push_back(make_map({{"type", type}, {"health", scales[0]}, {"damage", scales[1]}}));
    out.to_player(gid,
                  make_map({{"t", "difficulty"},
                            {"active", impl.difficulty.active},
                            {"enemy_health", impl.difficulty.enemy_health},
                            {"enemy_damage", impl.difficulty.enemy_damage},
                            {"spawn_rate", impl.difficulty.spawn_rate},
                            {"per_type", Value{std::move(per_type)}}}),
                  true);
    impl.send_crate_tags(gid); // (Integration) mod items in crates
    impl.send_held_items(gid); // (Items) mod items in hands
    player->stats_full_resend = true;
    player->effects_dirty = true;
    impl.replicate_player(*player, true);
    impl.replicate_quests(*player);
    if (!player->inventory_tags.empty())
    {
        Array slots;
        for (const auto &[slot, key] : player->inventory_tags)
            slots.push_back(make_map({{"slot", static_cast<std::int64_t>(slot)}, {"key", key}}));
        out.to_player(gid, make_map({{"t", "tags"}, {"slots", Value{std::move(slots)}}}), true);
    }
    if (player->dialogue.has_value())
        impl.show_dialogue_node(*player);
}

const std::vector<ClientStat> &GameplayService::client_stats() const noexcept
{
    return impl_->client_stats;
}
const std::vector<ClientEffect> &GameplayService::client_effects() const noexcept
{
    return impl_->client_effects;
}
const std::vector<ClientEntity> &GameplayService::client_entities() const noexcept
{
    return impl_->client_entities;
}
const std::optional<ClientDialogue> &GameplayService::client_dialogue() const noexcept
{
    return impl_->client_dialogue;
}
const std::vector<ClientQuest> &GameplayService::client_quests() const noexcept
{
    return impl_->client_quests;
}
const DifficultyScaling &GameplayService::client_difficulty() const noexcept
{
    return impl_->client_difficulty;
}
float GameplayService::client_speed_multiplier() const noexcept
{
    return impl_->client_speed;
}
const std::vector<ClientBlockType> &GameplayService::client_block_types() const noexcept
{
    return impl_->client_blocks;
}
const std::vector<ClientItemType> &GameplayService::client_item_types() const noexcept
{
    return impl_->client_items;
}
const std::vector<ClientRecipe> &GameplayService::client_recipes() const noexcept
{
    return impl_->client_recipes;
}

const ClientBlockType *GameplayService::client_block_at(BlockPos cell,
                                                        std::uint8_t current) const noexcept
{
    const auto found = impl_->client_overlay.find(cell);
    if (found == impl_->client_overlay.end() || found->second == 0U ||
        found->second > impl_->client_blocks.size())
        return nullptr;
    const ClientBlockType &type = impl_->client_blocks[found->second - 1U];
    return type.fallback == current ? &type : nullptr;
}

const ClientItemType *GameplayService::client_item(std::uint32_t mod_item) const noexcept
{
    if (mod_item == 0U || mod_item > impl_->client_items.size())
        return nullptr;
    return &impl_->client_items[mod_item - 1U];
}

bool GameplayService::client_recipe_disabled(std::size_t retail_index) const noexcept
{
    return impl_->client_disabled_recipes.contains(retail_index);
}

void GameplayService::set_client_listener(std::function<void(ClientChange)> listener)
{
    impl_->client_listener = std::move(listener);
}

// (Visuals) what the renderer polls.
std::uint64_t GameplayService::client_revision(ClientChange change) const noexcept
{
    return impl_->client_revisions[static_cast<std::size_t>(change) %
                                   impl_->client_revisions.size()];
}

const std::map<BlockPos, std::uint16_t> &GameplayService::client_overlay() const noexcept
{
    return impl_->client_overlay;
}

bool GameplayService::client_entity_died(std::uint64_t id) const noexcept
{
    return std::ranges::find(impl_->client_deaths, id) != impl_->client_deaths.end();
}

void GameplayService::client_choose(std::uint64_t dialogue_session, std::size_t choice)
{
    if (!client_running_)
        return;
    // Closed first: the answer (the next node) may arrive inside to_server
    // when the server runs in this process.
    impl_->client_dialogue.reset();
    impl_->notify(ClientChange::dialogue);
    impl_->out().to_server(make_map({{"t", "choose"},
                                     {"session", static_cast<std::int64_t>(dialogue_session)},
                                     {"choice", static_cast<std::int64_t>(std::min<std::size_t>(choice, 64U))}}));
}

void GameplayService::client_craft(std::uint32_t recipe_id)
{
    if (client_running_)
        impl_->out().to_server(make_map({{"t", "craft"}, {"recipe", static_cast<std::int64_t>(recipe_id)}}));
}

std::optional<std::string> GameplayService::player_key(Handle player) const
{
    const PlayerRecord *record = impl_->player_by_handle(player);
    if (record == nullptr || record->name.empty())
        return std::nullopt;
    return record->name;
}

Handle GameplayService::player_handle(std::uint8_t gid) const
{
    const PlayerRecord *record = impl_->player_by_gid(gid);
    if (record == nullptr)
    {
        // The game knows the player before the service does (the mod channel
        // admits a peer before the first tick): take it in now.
        impl_->refresh_players();
        record = impl_->player_by_gid(gid);
    }
    return record != nullptr ? impl_->handle_of(*record) : Handle{};
}

Handle GameplayService::local_player_handle() const
{
    const auto gid = impl_->backend.local_player();
    return gid.has_value() ? player_handle(*gid) : Handle{};
}

std::optional<std::uint8_t> GameplayService::player_gid(Handle player) const
{
    const PlayerRecord *record = impl_->player_by_handle(player);
    if (record == nullptr || !record->present)
        return std::nullopt;
    return record->gid;
}

} // namespace openminerz::modding::gameplay

namespace openminerz::modding
{

std::unique_ptr<gameplay::GameplayService> register_gameplay_api(ApiRegistry &registry,
                                                                 ScriptHost &host,
                                                                 gameplay::GameplayBackend &backend)
{
    auto service = std::make_unique<gameplay::GameplayService>(host, backend);
    service->register_api(registry);
    return service;
}

} // namespace openminerz::modding
