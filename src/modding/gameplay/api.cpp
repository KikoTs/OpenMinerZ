// Every script-visible gameplay function and event. The runtime builds the
// `world`, `blocks`, `items`, `recipes`, `loot`, `creatures`, `entities`,
// `players`, `stats`, `effects`, `structures`, `worldgen`, `commands`,
// `quests`, `dialogue`, `time` and `difficulty` libraries from these
// registrations, filtered by side and capability; the SDK reference and the
// Luau type definitions are generated from the same text.
//
// Conventions a mod author sees:
//   * positions are vectors (or {x=, y=, z=} tables); block cells are floored
//   * blocks and items are retail names ("rock", "iron_pickaxe"), mod keys
//     ("mymod:lamp") or a mod's own short names ("lamp")
//   * players and entities are handles; a stale one raises an error
//   * names a mod defines (blocks, items, creatures, loot tables) live in its
//     own namespace; stats, effects, quests, dialogues and commands are
//     session-wide names owned by the first mod that defines them

#include "internal.hpp"

#include "openminerz/world/block.hpp"
#include "openminerz/world/custom_terrain.hpp" // mod-world
#include "openminerz/world/enemy.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::gameplay
{
namespace
{

[[nodiscard]] ParamSpec param(std::string name, ValueType type, std::string doc,
                              bool optional = false)
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
constexpr ValueType integer = ValueType::integer;
constexpr ValueType vector = ValueType::vec3;
constexpr ValueType handle = ValueType::handle;
constexpr ValueType table = ValueType::map;
constexpr ValueType list = ValueType::array;
constexpr ValueType function = ValueType::callback;
constexpr ValueType boolean = ValueType::boolean;
constexpr ValueType nothing = ValueType::nil;

const ParamSpec player_param = param("player", handle, "A player handle.");
const ParamSpec entity_param = param("entity", handle, "An entity handle.");

class Builder final
{
  public:
    Builder(ApiRegistry &registry, GameplayService &service) : registry_{registry}, service_{service}
    {
    }

    // A server-side function that changes the game.
    template <typename Function>
    void act(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
             std::string returns_doc, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc),
            side_server, capability::gameplay, true, std::forward<Function>(function));
    }
    // A read-only query of server state.
    template <typename Function>
    void ask(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
             std::string returns_doc, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc),
            side_server, capability::query, true, std::forward<Function>(function));
    }
    // A read-only query either side may make.
    template <typename Function>
    void query(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
               std::string returns_doc, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc),
            side_both, capability::query, false, std::forward<Function>(function));
    }
    // A definition: runs on both sides (shared/ scripts), changes nothing yet.
    template <typename Function>
    void define(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
                std::string returns_doc, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc),
            side_both, capability::none, false, std::forward<Function>(function));
    }
    // A client-side view of replicated state, or a request from the client.
    template <typename Function>
    void client(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
                std::string returns_doc, CapabilityMask caps, Function &&function)
    {
        add(std::move(name), std::move(doc), std::move(params), returns, std::move(returns_doc),
            side_client, caps, false, std::forward<Function>(function));
    }

    void event(std::string name, std::string doc, SideMask sides, bool cancellable,
               std::vector<ParamSpec> payload)
    {
        registry_.add_event(
            EventSpec{std::move(name), std::move(doc), sides, cancellable, std::move(payload)});
    }

  private:
    template <typename Function>
    void add(std::string name, std::string doc, std::vector<ParamSpec> params, ValueType returns,
             std::string returns_doc, SideMask sides, CapabilityMask caps, bool needs_server,
             Function &&function)
    {
        GameplayService *service = &service_;
        registry_.add_function(
            FunctionSpec{name, std::move(doc), std::move(params), returns, std::move(returns_doc),
                         sides, caps, false},
            [service, needs_server, call = std::forward<Function>(function)](
                CallContext &context, std::span<const Value> args) -> Value {
                if (needs_server && !service->server_running())
                {
                    // A server VM is evidently running: the service starts
                    // with it and brings in its data at the first tick.
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

[[nodiscard]] std::string string_arg(std::span<const Value> args, std::size_t index,
                                     std::string_view what)
{
    const Value &value = arg(args, index);
    if (value.type() != ValueType::string)
        throw ScriptError{std::format("{}: expected a string", what)};
    return value.as_string();
}

[[nodiscard]] double number_arg(std::span<const Value> args, std::size_t index,
                                std::string_view what)
{
    const Value &value = arg(args, index);
    if (!value.is_number())
        throw ScriptError{std::format("{}: expected a number", what)};
    const double result = value.as_number();
    if (!std::isfinite(result))
        throw ScriptError{std::format("{}: expected a finite number", what)};
    return result;
}

[[nodiscard]] double number_arg_or(std::span<const Value> args, std::size_t index,
                                   double fallback, std::string_view what)
{
    return arg(args, index).is_nil() ? fallback : number_arg(args, index, what);
}

[[nodiscard]] const Value &table_arg(std::span<const Value> args, std::size_t index)
{
    static const Value empty{Map{}};
    const Value &value = arg(args, index);
    if (value.is_nil())
        return empty;
    if (value.type() != ValueType::map && value.type() != ValueType::array)
        throw ScriptError{"expected a table"};
    return value;
}

[[nodiscard]] Callback callback_arg(std::span<const Value> args, std::size_t index)
{
    const Value &value = arg(args, index);
    return value.is_nil() ? Callback{} : value.as_callback();
}

// The box between two corners, as inclusive integer bounds.
struct Box final
{
    BlockPos low;
    BlockPos high;
    [[nodiscard]] std::size_t cells() const noexcept
    {
        return static_cast<std::size_t>(high.x - low.x + 1) * static_cast<std::size_t>(high.y - low.y + 1) *
               static_cast<std::size_t>(high.z - low.z + 1);
    }
};

[[nodiscard]] Box box_args(std::span<const Value> args, std::size_t first)
{
    const BlockPos a = to_cell(arg(args, first), "from");
    const BlockPos b = to_cell(arg(args, first + 1U), "to");
    Box box{{std::min(a.x, b.x), std::clamp(std::min(a.y, b.y), -64, 63), std::min(a.z, b.z)},
            {std::max(a.x, b.x), std::clamp(std::max(a.y, b.y), -64, 63), std::max(a.z, b.z)}};
    if (box.high.x - box.low.x > 4096 || box.high.z - box.low.z > 4096)
        throw ScriptError{"a region is at most 4096 cells across"};
    return box;
}

[[nodiscard]] float yaw_toward(Vec3 from, Vec3 to) noexcept
{
    return static_cast<float>(std::atan2(to.x - from.x, -(to.z - from.z)));
}

[[nodiscard]] std::string enemy_class_name(int type)
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

} // namespace

void GameplayService::register_api(ApiRegistry &registry)
{
    Impl &impl = *impl_;
    Builder api{registry, *this};
    GameplayService &self = *this;

    // ================================================================ world
    api.query("world.get_block",
              "The block in a cell: a retail name (\"rock\", \"air\") or a mod block key. On a "
              "client this is the client's own view of the world.",
              {param("position", vector, "Any point inside the cell.")}, text, "Block name.",
              [&impl, &self](CallContext &context, std::span<const Value> args) -> Value {
                  const BlockPos cell = to_cell(arg(args, 0), "position");
                  const auto block = impl.block_at(cell);
                  if (!block.has_value())
                      return Value{};
                  if (context.side == Side::server)
                      return impl.block_label(*block, impl.mod_block_at(cell, *block));
                  if (const ClientBlockType *mod = self.client_block_at(cell, *block))
                      return mod->key;
                  const std::string_view name = retail_block_name(*block);
                  return name == "empty" ? std::string{"air"} : std::string{name};
              });
    api.act("world.set_block",
            "Sets one cell, exactly as a player's edit: every peer sees it, the world save keeps "
            "it. A mod block is placed as its retail fallback plus its identity.",
            {param("position", vector, "The cell."),
             param("block", any, "Block name, mod key, retail id, or \"air\".")},
            boolean, "True when the cell changed.",
            [&impl](CallContext &context, std::span<const Value> args) -> Value {
                const BlockPos cell = to_cell(arg(args, 0), "position");
                const auto spec = impl.resolve_block(arg(args, 1), context.mod_id, "block");
                return impl.set_cell(cell, spec.block, spec.mod_block);
            });
    api.act("world.fill",
            "Fills the box between two corners with one block. Large fills are spread over "
            "several ticks (a budget per tick); `on_done(job, changed)` runs when it finishes.",
            {param("from", vector, "One corner."), param("to", vector, "The opposite corner."),
             param("block", any, "Block to fill with."),
             optional("on_done", function, "Called with (job, cells changed).")},
            integer, "The job id.",
            [&impl](CallContext &context, std::span<const Value> args) -> Value {
                const Box box = box_args(args, 0);
                if (box.cells() > impl.limits.edit_cells_per_call)
                    throw ScriptError{std::format("a fill may touch at most {} cells", impl.limits.edit_cells_per_call)};
                const auto spec = impl.resolve_block(arg(args, 2), context.mod_id, "block");
                std::vector<EditCell> cells;
                cells.reserve(box.cells());
                for (int y = box.low.y; y <= box.high.y; ++y)
                    for (int z = box.low.z; z <= box.high.z; ++z)
                        for (int x = box.low.x; x <= box.high.x; ++x)
                            cells.push_back({{x, y, z}, spec.block, spec.mod_block});
                return static_cast<std::int64_t>(impl.queue_job(context.mod_id, std::move(cells), callback_arg(args, 3)));
            });
    api.act("world.replace",
            "Replaces every `find` block in the box with `replacement` (budgeted like fill).",
            {param("from", vector, "One corner."), param("to", vector, "The opposite corner."),
             param("find", any, "The block to look for."),
             param("replacement", any, "What it becomes."),
             optional("on_done", function, "Called with (job, cells changed).")},
            integer, "The job id.",
            [&impl](CallContext &context, std::span<const Value> args) -> Value {
                const Box box = box_args(args, 0);
                if (box.cells() > impl.limits.edit_cells_per_call)
                    throw ScriptError{std::format("a replace may examine at most {} cells", impl.limits.edit_cells_per_call)};
                const auto find = impl.resolve_block(arg(args, 2), context.mod_id, "find");
                const auto into = impl.resolve_block(arg(args, 3), context.mod_id, "replacement");
                std::vector<EditCell> cells;
                cells.reserve(box.cells());
                for (int y = box.low.y; y <= box.high.y; ++y)
                    for (int z = box.low.z; z <= box.high.z; ++z)
                        for (int x = box.low.x; x <= box.high.x; ++x)
                            cells.push_back({{x, y, z}, into.block, into.mod_block, true, find.block, find.mod_block});
                return static_cast<std::int64_t>(impl.queue_job(context.mod_id, std::move(cells), callback_arg(args, 4)));
            });
    api.ask("world.pending_edits", "Cells still queued by fill/replace/structures.", {}, integer,
            "Queued cells.", [&impl](CallContext &, std::span<const Value>) -> Value {
                std::size_t pending{};
                for (const EditJob &job : impl.edit_jobs)
                    pending += job.cells.size() - job.next;
                return static_cast<std::int64_t>(pending);
            });
    api.query("world.raycast",
              "The first non-air cell along a ray.",
              {param("origin", vector, "Start point."), param("direction", vector, "Direction."),
               optional("reach", number, "Maximum distance (default 64, at most 256).")},
              table, "{position, place (the cell before it), block, distance} or nil.",
              [&impl](CallContext &context, std::span<const Value> args) -> Value {
                  const Vec3 origin = to_vec3(arg(args, 0), "origin");
                  const Vec3 direction = to_vec3(arg(args, 1), "direction");
                  const float reach = static_cast<float>(std::clamp(number_arg_or(args, 2, 64.0, "reach"), 0.0, 256.0));
                  const auto hit = impl.raycast(origin, direction, reach);
                  if (!hit.has_value())
                      return Value{};
                  const std::uint8_t block = impl.block_at(hit->first).value_or(0U);
                  const Vec3 centre{hit->first.x + 0.5, hit->first.y + 0.5, hit->first.z + 0.5};
                  return make_map({{"position", cell_value(hit->first)},
                                   {"place", cell_value(hit->second)},
                                   {"block", context.side == Side::server
                                                 ? impl.block_label(block, impl.mod_block_at(hit->first, block))
                                                 : std::string{retail_block_name(block)}},
                                   {"distance", std::sqrt(distance_squared(centre, origin))}});
              });
    api.query("world.surface_y", "The Y of the highest block in a column that stops a player.",
              {param("x", number, "World X."), param("z", number, "World Z.")}, integer,
              "Y, or nil for an empty column.",
              [&impl](CallContext &, std::span<const Value> args) -> Value {
                  const auto y = impl.surface_y(static_cast<int>(std::floor(number_arg(args, 0, "x"))),
                                                static_cast<int>(std::floor(number_arg(args, 1, "z"))));
                  return y.has_value() ? Value{*y} : Value{};
              });
    // (Wave 5, survival) A client's find_blocks and count_blocks read the
    // client's own view, as its world.get_block does: a mod key matches the
    // cells of the client's mod-block layer that hold it (a joined client has
    // no host registry to resolve it in, and the fallback alone matched every
    // rock for a rock-backed block), a retail name only cells that are not a
    // mod block.
    struct ClientBlockQuery final
    {
        std::uint8_t block{};
        std::string key;  // the mod block's key; empty for a retail block
        bool known{true}; // false: a mod block this client has not been told of
    };
    const auto client_block_query = [&impl, &self](const Value &value, std::string_view mod) {
        if (value.type() == ValueType::string)
        {
            const std::string &name = value.as_string();
            const bool qualified = name.find(':') != std::string::npos;
            const std::string key = qualified ? name : std::format("{}:{}", mod, name);
            for (const ClientBlockType &type : self.client_block_types())
                if (type.key == key)
                    return ClientBlockQuery{type.fallback, type.key, true};
            if (qualified)
                return ClientBlockQuery{0U, name, false};
        }
        return ClientBlockQuery{impl.resolve_block(value, {}, "block").block, {}, true};
    };
    const auto client_cell_holds = [&self](const ClientBlockQuery &query, BlockPos cell, std::uint8_t block) {
        if (!query.known || block != query.block)
            return false;
        const ClientBlockType *mod = self.client_block_at(cell, block);
        return mod != nullptr ? mod->key == query.key : query.key.empty();
    };
    api.query("world.find_blocks", "Cells in a box holding a block (bounded scan).",
              {param("from", vector, "One corner."), param("to", vector, "The opposite corner."),
               param("block", any, "The block to look for."),
               optional("limit", integer, "Most cells to return (default 64, at most 4096).")},
              list, "Cell positions.",
              [&impl, client_block_query, client_cell_holds](CallContext &context, std::span<const Value> args) -> Value {
                  const Box box = box_args(args, 0);
                  if (box.cells() > impl.limits.query_cells)
                      throw ScriptError{std::format("a query may examine at most {} cells", impl.limits.query_cells)};
                  const bool server = context.side == Side::server;
                  Impl::BlockSpec spec{};
                  ClientBlockQuery wanted;
                  if (server)
                      spec = impl.resolve_block(arg(args, 2), context.mod_id, "block");
                  else
                      wanted = client_block_query(arg(args, 2), context.mod_id);
                  const auto limit = static_cast<std::size_t>(std::clamp(number_arg_or(args, 3, 64.0, "limit"), 1.0, 4096.0));
                  Array found;
                  for (int y = box.low.y; y <= box.high.y && found.size() < limit; ++y)
                      for (int z = box.low.z; z <= box.high.z && found.size() < limit; ++z)
                          for (int x = box.low.x; x <= box.high.x && found.size() < limit; ++x)
                          {
                              const auto block = impl.block_at({x, y, z});
                              if (!block.has_value())
                                  continue;
                              if (server ? *block != spec.block || impl.mod_block_at({x, y, z}, *block) != spec.mod_block
                                         : !client_cell_holds(wanted, {x, y, z}, *block))
                                  continue;
                              found.push_back(cell_value({x, y, z}));
                          }
                  return Value{std::move(found)};
              });
    api.query("world.count_blocks", "How many cells in a box hold a block (bounded scan).",
              {param("from", vector, "One corner."), param("to", vector, "The opposite corner."),
               param("block", any, "The block to count.")},
              integer, "The count.",
              [&impl, client_block_query, client_cell_holds](CallContext &context, std::span<const Value> args) -> Value {
                  const Box box = box_args(args, 0);
                  if (box.cells() > impl.limits.query_cells)
                      throw ScriptError{std::format("a query may examine at most {} cells", impl.limits.query_cells)};
                  const bool server = context.side == Side::server;
                  Impl::BlockSpec spec{};
                  ClientBlockQuery wanted;
                  if (server)
                      spec = impl.resolve_block(arg(args, 2), context.mod_id, "block");
                  else
                      wanted = client_block_query(arg(args, 2), context.mod_id);
                  std::int64_t total{};
                  for (int y = box.low.y; y <= box.high.y; ++y)
                      for (int z = box.low.z; z <= box.high.z; ++z)
                          for (int x = box.low.x; x <= box.high.x; ++x)
                          {
                              const auto block = impl.block_at({x, y, z});
                              if (block.has_value() &&
                                  (server ? *block == spec.block && impl.mod_block_at({x, y, z}, *block) == spec.mod_block
                                          : client_cell_holds(wanted, {x, y, z}, *block)))
                                  ++total;
                          }
                  return total;
              });
    api.query("world.biome", "The biome of a column.",
              {param("x", number, "World X."), param("z", number, "World Z.")}, text,
              "classic, lagoon, desert, mountain, arctic, descent, or a mod biome key "
              "(\"mymod:redwood\") where a mod world generator runs.",
              [&impl](CallContext &, std::span<const Value> args) -> Value {
                  const int biome = impl.backend.biome(static_cast<int>(std::floor(number_arg(args, 0, "x"))),
                                                       static_cast<int>(std::floor(number_arg(args, 1, "z"))));
                  return world::biome_name(biome); // mod-world
              });
    api.query("world.light", "Sun and torch light of a loaded cell, each 0..1.",
              {param("position", vector, "The cell.")}, table, "{sun, torch} or nil when not loaded.",
              [&impl](CallContext &, std::span<const Value> args) -> Value {
                  const auto light = impl.backend.light(to_cell(arg(args, 0), "position"));
                  if (!light.has_value())
                      return Value{};
                  return make_map({{"sun", (*light)[0]}, {"torch", (*light)[1]}});
              });
    api.query("world.seed", "The world seed.", {}, integer, "Seed.",
              [&impl](CallContext &, std::span<const Value>) -> Value {
                  return static_cast<std::int64_t>(impl.backend.world_seed());
              });

    // ================================================================= time
    api.query("time.of_day", "Time of day, 0..1 from midnight (0.5 is noon).", {}, number,
              "Time of day.", [&impl](CallContext &, std::span<const Value>) -> Value {
                  const double day = impl.backend.day();
                  return day - std::floor(day);
              });
    api.query("time.day", "The day number, 1 on the first day.", {}, integer, "Day.",
              [&impl](CallContext &, std::span<const Value>) -> Value {
                  return static_cast<std::int64_t>(std::floor(impl.backend.day())) + 1;
              });
    api.query("time.is_night", "Whether it is night (21:00 to 06:00).", {}, boolean, "Night.",
              [&impl](CallContext &, std::span<const Value>) -> Value {
                  const double day = impl.backend.day();
                  const double time_of_day = day - std::floor(day);
                  return time_of_day < 0.25 || time_of_day >= 0.875;
              });
    api.act("time.set", "Sets the time of day (every peer follows the host's clock).",
            {param("time_of_day", number, "0..1 from midnight.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const double value = number_arg(args, 0, "time_of_day");
                impl.backend.set_time_of_day(static_cast<float>(value - std::floor(value)));
                return Value{};
            });

    // =========================================================== difficulty
    api.act("difficulty.set",
            "Scales retail enemies for everyone in the session: health, the damage they deal and "
            "how fast they spawn. Applied by every peer to the enemies it simulates. `types` "
            "overrides the health scale per enemy class (zombie, archer, skeleton, felguard, "
            "alien); damage is scaled for all classes alike.",
            {param("scaling", table, "{enemy_health, enemy_damage, spawn_rate, types = {zombie = {health = 2}}}")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &options = table_arg(args, 0);
                DifficultyScaling scaling;
                scaling.active = true;
                scaling.enemy_health = static_cast<float>(std::clamp(number_or(options, "enemy_health", 1.0), 0.05, 100.0));
                scaling.enemy_damage = static_cast<float>(std::clamp(number_or(options, "enemy_damage", 1.0), 0.0, 100.0));
                scaling.spawn_rate = static_cast<float>(std::clamp(number_or(options, "spawn_rate", 1.0), 0.0, 20.0));
                if (const Value *types = options.field("types"); types != nullptr && types->type() == ValueType::map)
                    for (const auto &[name, scales] : types->as_map())
                    {
                        bool known{};
                        for (int type = 0; type < world::enemy_type_count; ++type)
                            if (enemy_class_name(type) == name)
                            {
                                known = true;
                                scaling.per_type[type] = {
                                    static_cast<float>(std::clamp(number_or(scales, "health", scaling.enemy_health), 0.05, 100.0)),
                                    static_cast<float>(std::clamp(number_or(scales, "damage", scaling.enemy_damage), 0.0, 100.0))};
                            }
                        if (!known)
                            throw ScriptError{std::format("unknown enemy '{}'", name)};
                    }
                impl.difficulty = std::move(scaling);
                impl.difficulty_dirty = true;
                return Value{};
            });
    api.act("difficulty.reset", "Back to the game's own difficulty.", {}, nothing, "",
            [&impl](CallContext &, std::span<const Value>) -> Value {
                impl.difficulty = {};
                impl.difficulty_dirty = true;
                return Value{};
            });
    api.query("difficulty.get", "The scaling in effect.", {}, table,
              "{active, enemy_health, enemy_damage, spawn_rate}.",
              [&impl](CallContext &context, std::span<const Value>) -> Value {
                  const DifficultyScaling &scaling =
                      context.side == Side::server ? impl.difficulty : impl.client_difficulty;
                  return make_map({{"active", scaling.active},
                                   {"enemy_health", scaling.enemy_health},
                                   {"enemy_damage", scaling.enemy_damage},
                                   {"spawn_rate", scaling.spawn_rate}});
              });

    // ======================================================= blocks / items
    api.define("blocks.define",
               "Defines a mod block. `fallback` (a retail block) is what the world, the save and "
               "every stock message hold; the identity and presentation live in the mod layer. "
               "Fields: name, fallback, texture, faces = {top, side, bottom}, model, light (0-15), "
               "drops (loot table, item, or \"none\"), drop_count, liquid (true: water-like - no "
               "collision, swum in, passed by the crosshair, drawn translucent; its fallback must "
               "let light through, e.g. ice).",
               {param("name", text, "Short name or \"mod:name\"."), param("definition", table, "The block.")},
               text, "The full key.",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.define_block(context.side, context.mod_id, string_arg(args, 0, "name"), table_arg(args, 1));
                   return impl.qualify(context.mod_id, string_arg(args, 0, "name"));
               });
    api.query("blocks.get", "A mod block's definition.", {param("key", text, "Block key.")}, table,
              "{key, name, fallback, texture, model, light, liquid} or nil.",
              [&impl](CallContext &context, std::span<const Value> args) -> Value {
                  const std::string key = string_arg(args, 0, "key");
                  const BlockDef *block = impl.registry(context.side).block(
                      key.find(':') != std::string::npos ? key : std::format("{}:{}", context.mod_id, key));
                  if (block == nullptr)
                      return Value{};
                  return make_map({{"key", block->key}, {"name", block->name},
                                   {"fallback", std::string{retail_block_name(block->fallback)}},
                                   {"texture", block->texture}, {"model", block->model},
                                   {"light", static_cast<std::int64_t>(block->light)},
                                   {"liquid", block->liquid}}); // (mod-liquid)
              });
    api.query("blocks.list", "Every mod block key.", {}, list, "Keys.",
              [&impl](CallContext &context, std::span<const Value>) -> Value {
                  Array keys;
                  for (const BlockDef &block : impl.registry(context.side).blocks)
                      keys.emplace_back(block.key);
                  return Value{std::move(keys)};
              });
    api.define("items.define",
               "Defines a mod item. `fallback` (a retail item this game has) is what inventories, "
               "saves and stock messages hold; its stack limit caps `stack`. Fields: name, "
               "description, fallback, icon, model (a glTF drawn in the hand and on the ground; "
               "without one the icon is, as a thin solid sprite), scale (its size there, 1), "
               "hold (fallback, item, swing, tool, block, fist, eat or drink: the arm clips and the "
               "grip), stack, places (a mod block of the same fallback), cooldown, weapon = {damage, "
               "player_damage, reach, cooldown} (melee stats the host checks every swing against; "
               "the fallback must be an item that strikes), use = function(player, key, target) "
               "(server; runs instead of the fallback's own action; target is item.used's aimed block "
               "or nil).",
               {param("name", text, "Short name or \"mod:name\"."), param("definition", table, "The item.")},
               text, "The full key.",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.define_item(context.side, context.mod_id, string_arg(args, 0, "name"), table_arg(args, 1));
                   return impl.qualify(context.mod_id, string_arg(args, 0, "name"));
               });
    api.query("items.get", "A mod item's definition.", {param("key", text, "Item key.")}, table,
              "{key, name, description, fallback, icon, model, hold, stack, weapon} or nil "
              "(weapon: {damage, player_damage, reach, cooldown} or nil).",
              [&impl](CallContext &context, std::span<const Value> args) -> Value {
                  const std::string key = string_arg(args, 0, "key");
                  const ItemDef *item = impl.registry(context.side).item(
                      key.find(':') != std::string::npos ? key : std::format("{}:{}", context.mod_id, key));
                  if (item == nullptr)
                      return Value{};
                  // (Items) model, hold and weapon.
                  Value weapon;
                  if (item->weapon.has_value())
                      weapon = make_map({{"damage", static_cast<double>(item->weapon->damage)},
                                         {"player_damage", static_cast<double>(item->weapon->player_damage)},
                                         {"reach", static_cast<double>(item->weapon->reach)},
                                         {"cooldown", static_cast<double>(item->weapon->cooldown)}});
                  return make_map({{"key", item->key}, {"name", item->name}, {"description", item->description},
                                   {"fallback", std::string{retail_item_name(item->fallback)}},
                                   {"icon", item->icon}, {"model", item->model},
                                   {"hold", std::string{hold_style_name(item->hold)}},
                                   {"stack", static_cast<std::int64_t>(item->stack_limit)},
                                   {"weapon", std::move(weapon)}});
              });
    api.query("items.list", "Every mod item key.", {}, list, "Keys.",
              [&impl](CallContext &context, std::span<const Value>) -> Value {
                  Array keys;
                  for (const ItemDef &item : impl.registry(context.side).items)
                      keys.emplace_back(item.key);
                  return Value{std::move(keys)};
              });

    // ============================================================= recipes
    api.define("recipes.add",
               "Adds a crafting recipe: {result, count, ingredients = {{\"iron\", 2}, ...} or "
               "{iron = 2}, category}. Crafted through the server (recipes.craft, recipes.request, "
               "or the crafting screen, which lists it under its result's fallback; at most four "
               "ingredients there).",
               {param("recipe", table, "The recipe.")}, integer, "Recipe id.",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   return static_cast<std::int64_t>(impl.add_recipe(context.side, context.mod_id, table_arg(args, 0)));
               });
    api.define("recipes.remove",
               "Removes a mod recipe (by id) or switches off the retail recipes that make an item "
               "(by name) for this session.",
               {param("what", any, "Recipe id or result item.")}, boolean, "True when something was removed.",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   return impl.remove_recipe(context.side, context.mod_id, arg(args, 0));
               });
    api.query("recipes.list", "The mod recipes of this session.", {}, list,
              "{id, category, result, count, ingredients} entries.",
              [&impl](CallContext &context, std::span<const Value>) -> Value {
                  Array recipes;
                  if (context.side == Side::server)
                  {
                      for (const RecipeDef &recipe : impl.server_registry.recipes)
                      {
                          Array ingredients;
                          for (const auto &[item, amount] : recipe.ingredients)
                              ingredients.push_back(make_map({{"item", impl.item_label(item)}, {"count", static_cast<std::int64_t>(amount)}}));
                          recipes.push_back(make_map({{"id", static_cast<std::int64_t>(recipe.id)}, {"category", recipe.category},
                                                      {"result", impl.item_label(recipe.result)},
                                                      {"count", static_cast<std::int64_t>(recipe.count)},
                                                      {"ingredients", Value{std::move(ingredients)}}}));
                      }
                      return Value{std::move(recipes)};
                  }
                  for (const ClientRecipe &recipe : impl.client_recipes)
                  {
                      Array ingredients;
                      for (const auto &[item, amount] : recipe.ingredients)
                          ingredients.push_back(make_map({{"item", item}, {"count", static_cast<std::int64_t>(amount)}}));
                      recipes.push_back(make_map({{"id", static_cast<std::int64_t>(recipe.id)}, {"category", recipe.category},
                                                  {"result", recipe.result}, {"count", static_cast<std::int64_t>(recipe.count)},
                                                  {"ingredients", Value{std::move(ingredients)}}}));
                  }
                  return Value{std::move(recipes)};
              });
    api.act("recipes.craft", "Crafts a mod recipe for a player from what it carries.",
            {player_param, param("recipe", integer, "Recipe id.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.craft(impl.player_arg(arg(args, 0)),
                           static_cast<std::uint32_t>(std::clamp(number_arg(args, 1, "recipe"), 0.0, 4.0e9)));
                return Value{};
            });
    api.client("recipes.request",
               "Asks the server to craft a mod recipe for the local player.",
               {param("recipe", integer, "Recipe id.")}, nothing, "", capability::network,
               [&self](CallContext &, std::span<const Value> args) -> Value {
                   self.client_craft(static_cast<std::uint32_t>(std::clamp(number_arg(args, 0, "recipe"), 0.0, 4.0e9)));
                   return Value{};
               });

    // ================================================================= loot
    api.define("loot.define",
               "Defines a loot table: {rolls = n or {min, max}, entries = {{item, weight, min, max}}}.",
               {param("name", text, "Short name or \"mod:name\"."), param("table", table, "The table.")},
               text, "The full key.",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.define_loot(context.side, context.mod_id, string_arg(args, 0, "name"), table_arg(args, 1));
                   return impl.qualify(context.mod_id, string_arg(args, 0, "name"));
               });
    api.define("loot.enemy",
               "Adds a loot table to what a retail enemy drops when the player this process "
               "simulates kills it (retail drops are unchanged).",
               {param("enemy", any, "zombie, archer, skeleton, felguard, alien, any, or a type id."),
                param("table", text, "Loot table.")},
               nothing, "", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.add_enemy_loot(context.side, context.mod_id, arg(args, 0), arg(args, 1));
                   return Value{};
               });
    api.act("loot.roll", "Rolls a loot table.", {param("table", text, "Loot table.")}, list,
            "{item, count} entries.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                std::string key = string_arg(args, 0, "table");
                if (key.find(':') == std::string::npos)
                    key = std::format("{}:{}", context.mod_id, key);
                if (!impl.server_registry.loot.contains(key))
                    throw ScriptError{std::format("no loot table '{}'", key)};
                return impl.stacks_value(impl.roll_loot(key));
            });
    api.act("loot.drop", "Rolls a loot table and drops the result as pickups.",
            {param("table", text, "Loot table."), param("position", vector, "Where.")}, integer,
            "Stacks dropped.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                std::string key = string_arg(args, 0, "table");
                if (key.find(':') == std::string::npos)
                    key = std::format("{}:{}", context.mod_id, key);
                if (!impl.server_registry.loot.contains(key))
                    throw ScriptError{std::format("no loot table '{}'", key)};
                const Vec3 at = to_vec3(arg(args, 1), "position");
                std::int64_t dropped{};
                for (const auto &[item, amount] : impl.roll_loot(key))
                    dropped += impl.backend.spawn_pickup(at, {item.item, amount, item.mod_item}) ? 1 : 0;
                return dropped;
            });
    api.act("loot.fill_crate", "Rolls a loot table into a crate's empty slots.",
            {param("table", text, "Loot table."), param("position", vector, "The crate.")}, integer,
            "Stacks placed.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                std::string key = string_arg(args, 0, "table");
                if (key.find(':') == std::string::npos)
                    key = std::format("{}:{}", context.mod_id, key);
                if (!impl.server_registry.loot.contains(key))
                    throw ScriptError{std::format("no loot table '{}'", key)};
                const BlockPos crate = to_cell(arg(args, 1), "position");
                const auto block = impl.block_at(crate);
                if (!block.has_value() || !world::is_container(static_cast<world::BlockType>(*block)))
                    throw ScriptError{"there is no crate there"};
                std::int64_t placed{};
                std::size_t slot{};
                for (const auto &[item, amount] : impl.roll_loot(key))
                {
                    if (slot >= 32U)
                        break;
                    if (impl.backend.set_crate_slot(crate, slot, StackView{item.item, amount, item.mod_item}))
                    {
                        ++placed;
                        // (Integration) every peer's copy learns the mod item.
                        static_cast<void>(impl.set_crate_tag(crate, static_cast<std::uint8_t>(slot), item.mod_item));
                    }
                    ++slot;
                }
                return placed;
            });

    // ============================================================ creatures
    api.define("creatures.define",
               "Defines a creature or NPC. Fields: name, model, texture, health (retail enemy "
               "units: a pistol round is about 1), speed, run_speed, damage (player health, 100 = "
               "full), attack_range, attack_cooldown, view_range, size = {width, height}, "
               "behaviour (passive, neutral, hostile, npc), drops, interact, dialogue, show_name, "
               "spawn = {biomes, light_min, light_max, time (day/night/any), surface, chance, max, "
               "group_min, group_max, y_min, y_max}. AI (any of these makes it an AI creature, "
               "docs/modding/authoring.md): brain (a brains.define key; none = the built-in "
               "hunter), team, hostile_to, friends, senses = {sight, fov, hearing, memory, xray}, "
               "movement (ground, fly, swim, amphibious, or {mode, jump, fall, climb, height, "
               "swim_speed}), attacks = {name = {type = melee|ranged, reach | range, min_range, arc, "
               "damage, creature_damage, cooldown, windup, recovery, animation, projectile, spread, "
               "count, lead, weight}}, abilities = {name = {cooldown, range, windup, animation, run}}, "
               "boss = {title, color, music, range, arena, phases = {{below, name, state, music, "
               "speed, damage}}}, think_rate, leash, flee_health, pack_flee, surround. A `model` of "
               "\"retail:zombie\" (skeleton, archer, skeleton_axes, skeleton_sword, felguard, alien) "
               "wears that retail enemy from the player's own game, where it is installed.",
               {param("name", text, "Short name or \"mod:name\"."), param("definition", table, "The creature.")},
               text, "The full key.",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.define_creature(context.side, context.mod_id, string_arg(args, 0, "name"), table_arg(args, 1));
                   return impl.qualify(context.mod_id, string_arg(args, 0, "name"));
               });
    api.query("creatures.list", "Every creature key.", {}, list, "Keys.",
              [&impl](CallContext &context, std::span<const Value>) -> Value {
                  Array keys;
                  for (const CreatureDef &creature : impl.registry(context.side).creatures)
                      keys.emplace_back(creature.key);
                  return Value{std::move(keys)};
              });

    // ============================================================= entities
    api.act("entities.spawn",
            "Spawns a creature or NPC. Options: name, health, yaw, persistent (kept in the "
            "world's mod data), behaviour, target, data; for AI creatures team and pack (an "
            "entity whose pack it joins).",
            {param("kind", text, "Creature key."), param("position", vector, "Feet position."),
             optional("options", table, "Spawn options.")},
            handle, "The entity, or nil when the entity limit is reached.",
            [&impl](CallContext &context, std::span<const Value> args) -> Value {
                std::string key = string_arg(args, 0, "kind");
                if (key.find(':') == std::string::npos)
                    key = std::format("{}:{}", context.mod_id, key);
                const CreatureDef *creature = impl.server_registry.creature(key);
                if (creature == nullptr)
                    throw ScriptError{std::format("no creature '{}'", key)};
                Entity *spawned = impl.spawn_entity(*creature, to_vec3(arg(args, 1), "position"), table_arg(args, 2));
                return spawned != nullptr ? Value{impl.entity_handle(*spawned)} : Value{};
            });
    api.act("entities.remove", "Removes an entity (no drops, no death).", {entity_param}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.remove_entity(impl.entity_arg(arg(args, 0)), false);
                return Value{};
            });
    api.ask("entities.exists", "Whether the entity is still there.", {entity_param}, boolean, "Exists.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &value = arg(args, 0);
                return value.type() == ValueType::handle && value.as_handle().kind == HandleKind::entity &&
                       impl.entity(value.as_handle().id) != nullptr;
            });
    api.ask("entities.get", "An entity's state.", {entity_param}, table,
            "{id, kind, position, yaw, health, max_health, name, behaviour, animation, data}.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                return impl.entity_info(impl.entity_arg(arg(args, 0)));
            });
    api.ask("entities.list", "Entities, optionally of one kind and/or near a point.",
            {optional("options", table, "{kind, near = position, radius}")}, list, "Entity handles.",
            [&impl](CallContext &context, std::span<const Value> args) -> Value {
                const Value &options = table_arg(args, 0);
                std::string kind = string_or(options, "kind", "");
                if (!kind.empty() && kind.find(':') == std::string::npos)
                    kind = std::format("{}:{}", context.mod_id, kind);
                const CreatureDef *creature = kind.empty() ? nullptr : impl.server_registry.creature(kind);
                if (!kind.empty() && creature == nullptr)
                    throw ScriptError{std::format("no creature '{}'", kind)};
                const Value *near = options.field("near");
                const std::optional<Vec3> centre = near != nullptr && !near->is_nil() ? std::optional{to_vec3(*near, "near")}
                                                                                       : std::nullopt;
                const double radius = number_or(options, "radius", 32.0);
                Array found;
                for (const Entity &entity : impl.entities)
                {
                    if (entity.removed || (creature != nullptr && entity.creature != creature->id))
                        continue;
                    if (centre.has_value() && distance_squared(entity.position, *centre) > radius * radius)
                        continue;
                    found.emplace_back(impl.entity_handle(entity));
                }
                return Value{std::move(found)};
            });
    api.act("entities.set_behaviour",
            "Steers an entity: idle, wander, flee, follow, attack, path (to a position), look; "
            "nil hands it back to its creature's own behaviour. Options: target (player, entity "
            "or position), distance, duration (seconds; 0 = until changed).",
            {entity_param, param("behaviour", any, "Behaviour name or nil."),
             optional("options", table, "{target, distance, duration}")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                Entity &entity = impl.entity_arg(arg(args, 0));
                if (arg(args, 1).is_nil())
                {
                    entity.behaviour = {};
                    entity.path.clear();
                    return Value{};
                }
                impl.set_behaviour(entity, string_arg(args, 1, "behaviour"), table_arg(args, 2));
                return Value{};
            });
    api.act("entities.move_to", "Walks an entity to a point along a found path; entity.arrived fires.",
            {entity_param, param("position", vector, "Destination.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.set_behaviour(impl.entity_arg(arg(args, 0)), "path",
                                   make_map({{"target", Value{to_vec3(arg(args, 1), "position")}}}));
                return Value{};
            });
    api.act("entities.look_at", "Turns an entity to face a point, player or entity.",
            {entity_param, param("target", any, "Position, player or entity.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                Entity &entity = impl.entity_arg(arg(args, 0));
                const Value &target = arg(args, 1);
                Vec3 point{};
                if (target.type() == ValueType::handle)
                {
                    const Handle other = target.as_handle();
                    if (other.kind == HandleKind::player)
                        point = impl.player_arg(target).position;
                    else
                        point = impl.entity_arg(target).position;
                }
                else
                    point = to_vec3(target, "target");
                entity.yaw = yaw_toward(entity.position, point);
                return Value{};
            });
    api.act("entities.damage", "Damages an entity (entity.damaged may cancel it).",
            {entity_param, param("amount", number, "Damage in health units."),
             optional("attacker", handle, "The player or entity responsible.")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                Entity &entity = impl.entity_arg(arg(args, 0));
                std::uint64_t player{};
                std::uint64_t other{};
                if (arg(args, 2).type() == ValueType::handle)
                {
                    const Handle attacker = arg(args, 2).as_handle();
                    if (attacker.kind == HandleKind::player)
                        player = attacker.id;
                    else if (attacker.kind == HandleKind::entity)
                        other = attacker.id;
                }
                impl.damage_entity(entity, static_cast<float>(std::clamp(number_arg(args, 1, "amount"), 0.0, 1.0e6)),
                                   player, other);
                return Value{};
            });
    api.act("entities.heal", "Heals an entity up to its maximum.",
            {entity_param, param("amount", number, "Health units.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                Entity &entity = impl.entity_arg(arg(args, 0));
                entity.health = std::min(entity.max_health,
                                         entity.health + static_cast<float>(std::max(0.0, number_arg(args, 1, "amount"))));
                return Value{};
            });
    api.act("entities.set_health", "Sets an entity's health (0 kills it).",
            {entity_param, param("health", number, "Health units.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                Entity &entity = impl.entity_arg(arg(args, 0));
                entity.health = static_cast<float>(std::clamp(number_arg(args, 1, "health"), 0.0,
                                                              static_cast<double>(entity.max_health)));
                if (entity.health <= 0.0F)
                    impl.remove_entity(entity, true);
                return Value{};
            });
    api.act("entities.teleport", "Moves an entity at once.",
            {entity_param, param("position", vector, "Feet position.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                Entity &entity = impl.entity_arg(arg(args, 0));
                entity.position = to_vec3(arg(args, 1), "position");
                entity.velocity = {};
                entity.path.clear();
                return Value{};
            });
    api.act("entities.set_name", "Sets the nametag shown over an entity (\"\" hides it).",
            {entity_param, param("name", text, "Name.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.entity_arg(arg(args, 0)).name = string_arg(args, 1, "name").substr(0, 64U);
                return Value{};
            });
    api.act("entities.set_animation",
            "Plays a named animation instead of the automatic one (nil returns to automatic).",
            {entity_param, param("animation", any, "Animation name or nil.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                Entity &entity = impl.entity_arg(arg(args, 0));
                entity.animation_override = arg(args, 1).is_nil() ? std::string{} : string_arg(args, 1, "animation").substr(0, 64U);
                return Value{};
            });
    api.act("entities.set_data", "Stores script data on an entity (persisted with persistent ones).",
            {entity_param, param("data", any, "Any serializable value.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.entity_arg(arg(args, 0)).data = arg(args, 1);
                return Value{};
            });
    api.client("entities.visible", "The mod entities this client draws.", {}, list,
               "{id, entity (the handle lights, models and effects attach to), kind, position, yaw, "
               "animation, name, health} entries.",
               capability::query,
               [&impl](CallContext &, std::span<const Value>) -> Value {
                   Array list_value;
                   for (const ClientEntity &entity : impl.client_entities)
                       list_value.push_back(make_map({{"id", static_cast<std::int64_t>(entity.id)},
                                                      // (Visuals) attachable handle
                                                      {"entity", Handle{HandleKind::entity, entity.id}},
                                                      {"kind", entity.kind},
                                                      {"position", vec3_value(entity.position)},
                                                      {"yaw", entity.yaw},
                                                      {"animation", entity.animation},
                                                      {"name", entity.name},
                                                      {"health", entity.health_fraction}}));
                   return Value{std::move(list_value)};
               });

    // ============================================================== players
    // (Integration) Local data only, so a client mod may use it on any server.
    api.query("players.me",
              "The player this machine plays: {position (feet), eye, look (view direction), yaw, pitch, "
              "health, alive, target = {position, face} of the block the game's selection aims at, "
              "liquid (the first liquid cell the look passes within reach), in_liquid, underwater}. nil "
              "outside a world. Works for client mods on any server.",
              {}, table, "The local player's view, or nil.",
              [&impl](CallContext &, std::span<const Value>) -> Value {
                  const std::optional<LocalView> view = impl.backend.local_view();
                  const auto local = impl.backend.local_player();
                  if (!view.has_value() || !local.has_value())
                      return Value{};
                  std::vector<PlayerSnapshot> players;
                  impl.backend.players(players);
                  const auto self = std::ranges::find(players, *local, &PlayerSnapshot::gid);
                  Map fields{{"eye", vec3_value(view->eye)},
                             {"look", vec3_value(view->look)},
                             {"yaw", view->yaw},
                             {"pitch", view->pitch}};
                  if (self != players.end())
                  {
                      fields.emplace_back("position", vec3_value(self->position));
                      fields.emplace_back("health", self->health);
                      fields.emplace_back("alive", self->alive);
                  }
                  if (view->target.has_value())
                  {
                      Map target{{"position", cell_value(*view->target)}};
                      if (view->target_adjacent.has_value())
                          target.emplace_back("face", cell_value(*view->target_adjacent));
                      fields.emplace_back("target", Value{std::move(target)});
                  }
                  // (mod-liquid)
                  if (view->liquid.has_value())
                      fields.emplace_back("liquid", cell_value(*view->liquid));
                  fields.emplace_back("in_liquid", view->in_liquid);
                  fields.emplace_back("underwater", view->underwater);
                  return Value{std::move(fields)};
              });
    api.ask("players.list", "Every player in the session.", {}, list, "Player handles.",
            [&impl](CallContext &, std::span<const Value>) -> Value {
                impl.ensure_players();
                Array handles;
                for (const PlayerRecord &player : impl.players)
                    if (player.present)
                        handles.emplace_back(impl.handle_of(player));
                return Value{std::move(handles)};
            });
    api.ask("players.get", "A player's state.", {player_param}, table,
            "{name, position, yaw, health, alive, local, in_liquid (the feet are in a liquid block), "
            "underwater (the eyes are)}.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const PlayerRecord &player = impl.player_arg(arg(args, 0));
                // (mod-liquid) From this world's liquids at the player's feet
                // and eyes (PlayerSimulation::eye_height above them).
                const Vec3 eye{player.position.x, player.position.y + 1.4, player.position.z};
                return make_map({{"name", player.name},
                                 {"position", vec3_value(player.position)},
                                 {"yaw", player.yaw},
                                 {"health", player.health},
                                 {"alive", player.alive},
                                 {"local", player.local},
                                 {"in_liquid", impl.liquid_depth_at(player.position) > 0.0},
                                 {"underwater", impl.liquid_depth_at(eye) > 0.0}});
            });
    api.ask("players.by_name", "The player with a gamer tag.", {param("name", text, "Gamer tag.")},
            handle, "Player handle or nil.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                const std::string name = string_arg(args, 0, "name");
                impl.ensure_players();
                for (const PlayerRecord &player : impl.players)
                    if (player.present && player.name == name)
                        return impl.handle_of(player);
                return Value{};
            });
    api.ask("players.position", "A player's feet position.", {player_param}, vector, "Position.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                return vec3_value(impl.player_arg(arg(args, 0)).position);
            });
    api.act("players.teleport", "Moves a player.", {player_param, param("position", vector, "Feet position.")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.teleport(impl.player_arg(arg(args, 0)), to_vec3(arg(args, 1), "position"));
                return Value{};
            });
    api.ask("players.health", "A player's health (0-100; a remote player's last report).",
            {player_param}, number, "Health.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                return impl.player_arg(arg(args, 0)).health;
            });
    api.act("players.set_health", "Sets a player's health (0-100).",
            {player_param, param("health", number, "0-100.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.set_health(impl.player_arg(arg(args, 0)), static_cast<float>(number_arg(args, 1, "health")));
                return Value{};
            });
    api.act("players.damage", "Damages a player through the game's own damage path.",
            {player_param, param("amount", number, "Health points (100 = all)."),
             optional("source", vector, "Where it came from (for the indicator).")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                PlayerRecord &player = impl.player_arg(arg(args, 0));
                const Vec3 source = arg(args, 2).is_nil() ? player.position : to_vec3(arg(args, 2), "source");
                impl.damage(player, static_cast<float>(std::clamp(number_arg(args, 1, "amount"), 0.0, 10'000.0)),
                            DamageCause::script, source);
                return Value{};
            });
    api.act("players.heal", "Heals a player (up to 100).", {player_param, param("amount", number, "Health points.")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.heal(impl.player_arg(arg(args, 0)), static_cast<float>(std::clamp(number_arg(args, 1, "amount"), 0.0, 100.0)));
                return Value{};
            });
    api.act("players.set_damage",
            "Inside a player.damaged handler: changes how much damage is applied (0 = none). "
            "Works for every player when the host decides health (a session with server mods, or "
            "OpenMinerZ players only); otherwise only for the players this server simulates "
            "itself, and remote players' damage is reported after the fact.",
            {param("amount", number, "The damage to apply.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                if (impl.damage_overrides.empty())
                    throw ScriptError{"players.set_damage only works inside a player.damaged handler"};
                *impl.damage_overrides.back() = static_cast<float>(std::clamp(number_arg(args, 0, "amount"), 0.0, 10'000.0));
                return Value{};
            });
    api.act("players.give", "Gives a player items (what does not fit is dropped at its feet).",
            {player_param, param("item", any, "Item name, mod key or retail id."),
             optional("count", integer, "How many (default 1).")},
            integer, "How many were given.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                return static_cast<std::int64_t>(impl.give(
                    impl.player_arg(arg(args, 0)), impl.resolve_item(arg(args, 1), context.mod_id, "item"),
                    static_cast<std::uint32_t>(std::clamp(number_arg_or(args, 2, 1.0, "count"), 1.0, 4096.0))));
            });
    api.act("players.take", "Takes items from a player.",
            {player_param, param("item", any, "Item name, mod key or retail id."),
             optional("count", integer, "How many (default 1).")},
            integer, "How many were taken.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                return static_cast<std::int64_t>(impl.take(
                    impl.player_arg(arg(args, 0)), impl.resolve_item(arg(args, 1), context.mod_id, "item"),
                    static_cast<std::uint32_t>(std::clamp(number_arg_or(args, 2, 1.0, "count"), 1.0, 1.0e6))));
            });
    api.ask("players.count", "How many of an item a player carries (a remote player's last report).",
            {player_param, param("item", any, "Item name, mod key or retail id.")}, integer, "Count.",
            [&impl](CallContext &context, std::span<const Value> args) -> Value {
                return static_cast<std::int64_t>(
                    impl.count(impl.player_arg(arg(args, 0)), impl.resolve_item(arg(args, 1), context.mod_id, "item")));
            });
    api.act("players.clear_inventory", "Empties a player's inventory.", {player_param}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.clear_inventory(impl.player_arg(arg(args, 0)));
                return Value{};
            });
    api.ask("players.held", "The stack in a player's hand (the local player only).", {player_param}, table,
            "{item, count} or nil.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                const PlayerRecord &player = impl.player_arg(arg(args, 0));
                if (!player.local)
                    return Value{};
                const auto held = impl.backend.local_held();
                if (!held.has_value())
                    return Value{};
                return make_map({{"item", impl.item_label({held->item, held->mod_item})},
                                 {"count", static_cast<std::int64_t>(held->count)}});
            });
    api.act("players.message", "A system chat line for one player.",
            {player_param, param("text", text, "The line (160 characters at most).")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.message(impl.player_arg(arg(args, 0)).gid, string_arg(args, 1, "text"));
                return Value{};
            });
    api.act("players.broadcast", "A system chat line for everyone.", {param("text", text, "The line.")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.message(std::nullopt, string_arg(args, 0, "text"));
                return Value{};
            });

    // ================================================================ stats
    api.define("stats.define",
               "Defines a per-player stat (hunger, thirst, ...): {min, max, initial, decay (per "
               "second toward min; negative refills), persist, public, hud = true or {label, "
               "color, icon}}. Stored by the server, replicated to its player.",
               {param("name", text, "Stat name."), param("definition", table, "The stat.")}, nothing, "",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.define_stat(context.side, context.mod_id, string_arg(args, 0, "name"), table_arg(args, 1));
                   return Value{};
               });
    api.ask("stats.get", "A player's stat.", {player_param, param("stat", text, "Stat name.")}, number,
            "Value.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                return impl.stat(impl.player_arg(arg(args, 0)), string_arg(args, 1, "stat"));
            });
    api.act("stats.set", "Sets a player's stat (clamped to its range).",
            {player_param, param("stat", text, "Stat name."), param("value", number, "New value.")}, number,
            "The value now.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                return impl.set_stat(impl.player_arg(arg(args, 0)), string_arg(args, 1, "stat"),
                                     number_arg(args, 2, "value"), true);
            });
    api.act("stats.add", "Adds to a player's stat (negative subtracts).",
            {player_param, param("stat", text, "Stat name."), param("amount", number, "Change.")}, number,
            "The value now.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                PlayerRecord &player = impl.player_arg(arg(args, 0));
                const std::string name = string_arg(args, 1, "stat");
                return impl.set_stat(player, name, impl.stat(player, name) + number_arg(args, 2, "amount"), true);
            });
    api.query("stats.list", "The defined stat names.", {}, list, "Names.",
              [&impl](CallContext &context, std::span<const Value>) -> Value {
                  Array names;
                  for (const auto &[name, stat] : impl.registry(context.side).stats)
                      names.emplace_back(name);
                  return Value{std::move(names)};
              });
    api.client("stats.mine", "The local player's stat, as replicated.", {param("stat", text, "Stat name.")},
               number, "Value, or nil before it arrives.", capability::query,
               [&impl](CallContext &, std::span<const Value> args) -> Value {
                   const std::string name = string_arg(args, 0, "stat");
                   const auto found = std::ranges::find(impl.client_stats, name, &ClientStat::name);
                   return found != impl.client_stats.end() ? Value{found->value} : Value{};
               });

    // ============================================================== effects
    api.define("effects.define",
               "Defines a status effect: {interval, icon, start = fn(player, strength), tick = "
               "fn(player, strength), stop = fn(player, strength)}. Built in: speed (strength = "
               "movement multiplier), regen and damage (health per second), stat (moves `stat` "
               "by strength per second).",
               {param("name", text, "Effect name."), param("definition", table, "The effect.")}, nothing, "",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.define_effect(context.side, context.mod_id, string_arg(args, 0, "name"), table_arg(args, 1));
                   return Value{};
               });
    api.act("effects.apply",
            "Applies an effect: {duration (seconds; omit for until removed), strength, stat, "
            "stack}. Re-applying refreshes it unless stack is true.",
            {player_param, param("effect", text, "Effect name."), optional("options", table, "Options.")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.apply_effect(impl.player_arg(arg(args, 0)), string_arg(args, 1, "effect"), table_arg(args, 2));
                return Value{};
            });
    api.act("effects.remove", "Removes an effect.", {player_param, param("effect", text, "Effect name.")},
            boolean, "True when it was active.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                return impl.remove_effect(impl.player_arg(arg(args, 0)), string_arg(args, 1, "effect"));
            });
    api.ask("effects.has", "Whether an effect is active.", {player_param, param("effect", text, "Effect name.")},
            boolean, "Active.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                const PlayerRecord &player = impl.player_arg(arg(args, 0));
                const std::string kind = string_arg(args, 1, "effect");
                return std::ranges::any_of(player.effects, [&](const ActiveEffect &effect) { return effect.kind == kind; });
            });
    api.ask("effects.list", "A player's active effects.", {player_param}, list,
            "{effect, remaining, strength} entries.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                Array effects;
                for (const ActiveEffect &effect : impl.player_arg(arg(args, 0)).effects)
                    effects.push_back(make_map({{"effect", effect.kind}, {"remaining", effect.remaining},
                                                {"strength", effect.strength}}));
                return Value{std::move(effects)};
            });
    api.client("effects.mine", "The local player's effects, as replicated.", {}, list,
               "{effect, remaining, strength, icon} entries.", capability::query,
               [&impl](CallContext &, std::span<const Value>) -> Value {
                   Array effects;
                   for (const ClientEffect &effect : impl.client_effects)
                       effects.push_back(make_map({{"effect", effect.kind}, {"remaining", effect.remaining},
                                                   {"strength", effect.strength}, {"icon", effect.icon}}));
                   return Value{std::move(effects)};
               });

    // =========================================================== structures
    api.define("structures.define",
               "Defines a structure from a table: {palette = {[\"#\"] = \"rock\", [\".\"] = "
               "\"air\"}, layers = {{\"###\", \"#.#\", \"###\"}, ...} (bottom first; rows run "
               "along +Z, characters along +X; space = leave as is), origin = {x, y, z}}.",
               {param("name", text, "Short name or \"mod:name\"."), param("definition", table, "The structure.")},
               vector, "Its size.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   const std::string key = impl.qualify(context.mod_id, string_arg(args, 0, "name"));
                   Schematic schematic = impl.schematic_from_table(table_arg(args, 1));
                   const Vec3 size{static_cast<double>(schematic.size_x), static_cast<double>(schematic.size_y),
                                   static_cast<double>(schematic.size_z)};
                   impl.registry(context.side).structures.insert_or_assign(key, std::move(schematic));
                   return size;
               });
    api.define("structures.parse",
               "Defines a structure from the text schematic format (a mod file's contents): "
               "'origin x y z', 'palette <char> <block>', 'layer' then rows.",
               {param("name", text, "Short name or \"mod:name\"."), param("text", text, "Schematic text.")},
               vector, "Its size.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   const std::string key = impl.qualify(context.mod_id, string_arg(args, 0, "name"));
                   Schematic schematic;
                   std::string error;
                   if (!parse_schematic(string_arg(args, 1, "text"), schematic, error))
                       throw ScriptError{std::format("structure '{}': {}", key, error)};
                   const Vec3 size{static_cast<double>(schematic.size_x), static_cast<double>(schematic.size_y),
                                   static_cast<double>(schematic.size_z)};
                   impl.registry(context.side).structures.insert_or_assign(key, std::move(schematic));
                   return size;
               });
    api.act("structures.place",
            "Builds a structure with its origin at a cell: {rotation = 0/90/180/270 (about Y, +X "
            "toward +Z), air = true (whether \"air\" cells carve), on_done = fn(job, changed)}. "
            "The cells are ordinary world edits, spread over ticks.",
            {param("name", text, "Structure."), param("position", vector, "Where its origin goes."),
             optional("options", table, "Options.")},
            integer, "The job id.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                std::string key = string_arg(args, 0, "name");
                if (key.find(':') == std::string::npos)
                    key = std::format("{}:{}", context.mod_id, key);
                const auto found = impl.server_registry.structures.find(key);
                if (found == impl.server_registry.structures.end())
                    throw ScriptError{std::format("no structure '{}'", key)};
                const Value &options = table_arg(args, 2);
                const double rotation = number_or(options, "rotation", 0.0);
                if (std::fmod(rotation, 90.0) != 0.0)
                    throw ScriptError{"rotation is 0, 90, 180 or 270"};
                return static_cast<std::int64_t>(impl.place_structure(
                    context.mod_id, found->second, to_cell(arg(args, 1), "position"),
                    static_cast<int>(rotation / 90.0), bool_or(options, "air", true), callback_or_none(options, "on_done")));
            });
    api.query("structures.size", "A structure's size before rotation.", {param("name", text, "Structure.")},
              vector, "{x, y, z} size.", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                  std::string key = string_arg(args, 0, "name");
                  if (key.find(':') == std::string::npos)
                      key = std::format("{}:{}", context.mod_id, key);
                  const auto &structures = impl.registry(context.side).structures;
                  const auto found = structures.find(key);
                  if (found == structures.end())
                      throw ScriptError{std::format("no structure '{}'", key)};
                  return Vec3{static_cast<double>(found->second.size_x), static_cast<double>(found->second.size_y),
                              static_cast<double>(found->second.size_z)};
              });

    // ============================================================= worldgen
    api.act("worldgen.enable",
            "Starts offering chunks to worldgen.chunk handlers: each chunk once, the first time "
            "a player comes within `radius` chunks (default 14; retail peers see 12). What "
            "handlers build is ordinary world edits, so every peer receives it as chunk data.",
            {optional("options", table, "{radius}")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &options = table_arg(args, 0);
                impl.worldgen_enabled = true;
                impl.limits.worldgen_radius_chunks =
                    static_cast<int>(std::clamp(number_or(options, "radius", impl.limits.worldgen_radius_chunks), 1.0, 32.0));
                return Value{};
            });
    api.query("worldgen.random",
              "A repeatable random number in [0, 1) for a chunk (and a salt): the same world "
              "gives the same answer every time.",
              {param("cx", integer, "Chunk X."), param("cz", integer, "Chunk Z."),
               optional("salt", integer, "Distinguishes draws.")},
              number, "0..1.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                  auto value = static_cast<std::uint64_t>(static_cast<std::uint32_t>(impl.backend.world_seed()));
                  value = value * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(number_arg(args, 0, "cx")));
                  value = value * 0xBF58476D1CE4E5B9ULL ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(number_arg(args, 1, "cz")));
                  value = value * 0x94D049BB133111EBULL ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(number_arg_or(args, 2, 0.0, "salt")));
                  value ^= value >> 31U;
                  value *= 0xD6E8FEB86659FD93ULL;
                  value ^= value >> 32U;
                  return static_cast<double>(value >> 11U) / 9007199254740992.0;
              });

    // ============================================================= commands
    api.act("commands.register",
            "Registers a chat command. `/name args` calls fn(player, args, line); a string it "
            "returns is shown to that player. /help lists every mod command.",
            {param("name", text, "Command name without the slash."), param("help", text, "One line of help."),
             param("handler", function, "fn(player, args, line).")},
            nothing, "", [&impl](CallContext &context, std::span<const Value> args) -> Value {
                const std::string name = string_arg(args, 0, "name");
                const Callback handler = callback_arg(args, 2);
                // /help lists mod commands; /mods is the runtime's console.
                if (name.empty() || name.size() > 32U || name == "help" || name == "mods" ||
                    !std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
                {
                    impl.host.release(handler);
                    throw ScriptError{"a command name is 1-32 of a-z, 0-9 and _ (not 'help' or 'mods')"};
                }
                const auto existing = impl.server_registry.commands.find(name);
                if (existing != impl.server_registry.commands.end() && existing->second.mod != context.mod_id)
                {
                    impl.host.release(handler);
                    throw ScriptError{std::format("/{} belongs to '{}'", name, existing->second.mod)};
                }
                impl.hold(Side::server, handler);
                impl.server_registry.commands.insert_or_assign(
                    name, CommandDef{name, string_arg(args, 1, "help").substr(0, 120U), std::string{context.mod_id}, handler});
                return Value{};
            });

    // =============================================================== quests
    api.define("quests.define",
               "Defines a quest: {title, description, stages = {{id, text, objectives = {{type = "
               "kill|collect|reach|talk|custom, target, count, position, radius, id, text}}}}, "
               "rewards = {items = {{\"iron\", 5}}}, on_stage = fn(player, quest, stage), "
               "on_complete = fn(player, quest), repeatable}.",
               {param("id", text, "Quest id."), param("definition", table, "The quest.")}, nothing, "",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.define_quest(context.side, context.mod_id, string_arg(args, 0, "id"), table_arg(args, 1));
                   return Value{};
               });
    api.act("quests.start", "Gives a player a quest.", {player_param, param("quest", text, "Quest id.")},
            boolean, "False when it is already active or done (and not repeatable).",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                return impl.start_quest(impl.player_arg(arg(args, 0)), string_arg(args, 1, "quest"));
            });
    api.act("quests.advance", "Advances a custom (or any) objective of the current stage.",
            {player_param, param("quest", text, "Quest id."),
             optional("objective", text, "Objective id (nil: every objective of the stage)."),
             optional("amount", integer, "How much (default 1).")},
            nothing, "", [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.advance_objective(impl.player_arg(arg(args, 0)), string_arg(args, 1, "quest"),
                                       arg(args, 2).is_nil() ? std::string{} : string_arg(args, 2, "objective"),
                                       static_cast<std::uint32_t>(std::clamp(number_arg_or(args, 3, 1.0, "amount"), 0.0, 1.0e6)));
                return Value{};
            });
    api.act("quests.complete", "Completes a quest now (rewards included).",
            {player_param, param("quest", text, "Quest id.")}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.complete_quest(impl.player_arg(arg(args, 0)), string_arg(args, 1, "quest"));
                return Value{};
            });
    api.act("quests.abandon", "Drops an active quest.", {player_param, param("quest", text, "Quest id.")},
            boolean, "True when it was active.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                return impl.abandon_quest(impl.player_arg(arg(args, 0)), string_arg(args, 1, "quest"));
            });
    api.ask("quests.status", "A player's progress on a quest.", {player_param, param("quest", text, "Quest id.")},
            table, "{id, active, completed, stage, stage_index, objectives = {{id, progress, required, done}}}.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                return impl.quest_status(impl.player_arg(arg(args, 0)), string_arg(args, 1, "quest"));
            });
    api.ask("quests.active", "A player's active quests.", {player_param}, list, "Quest ids.",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                Array ids;
                for (const auto &[id, progress] : impl.player_arg(arg(args, 0)).quests)
                    if (!progress.completed)
                        ids.emplace_back(id);
                return Value{std::move(ids)};
            });
    api.client("quests.log", "The local player's quest log, as replicated.", {}, list,
               "{id, title, description, stage, completed, objectives = {{text, progress, required, done}}}.",
               capability::query, [&impl](CallContext &, std::span<const Value>) -> Value {
                   Array quests;
                   for (const ClientQuest &quest : impl.client_quests)
                   {
                       Array objectives;
                       for (const ClientQuestObjective &objective : quest.objectives)
                           objectives.push_back(make_map({{"text", objective.text},
                                                          {"progress", static_cast<std::int64_t>(objective.progress)},
                                                          {"required", static_cast<std::int64_t>(objective.required)},
                                                          {"done", objective.done}}));
                       quests.push_back(make_map({{"id", quest.id}, {"title", quest.title},
                                                  {"description", quest.description}, {"stage", quest.stage},
                                                  {"completed", quest.completed},
                                                  {"objectives", Value{std::move(objectives)}}}));
                   }
                   return Value{std::move(quests)};
               });

    // ============================================================= dialogue
    api.define("dialogue.define",
               "Defines a dialogue tree: {title, start, nodes = {name = {speaker, text, portrait, "
               "next, on_enter = fn(player, node), choices = {{text, next, condition = "
               "fn(player) -> bool, action = fn(player), start_quest}}}}}. A creature with "
               "`dialogue` opens it when talked to.",
               {param("id", text, "Dialogue id."), param("definition", table, "The tree.")}, nothing, "",
               [&impl](CallContext &context, std::span<const Value> args) -> Value {
                   impl.define_dialogue(context.side, context.mod_id, string_arg(args, 0, "id"), table_arg(args, 1));
                   return Value{};
               });
    api.act("dialogue.open", "Opens a dialogue for a player: {node, entity}.",
            {player_param, param("dialogue", text, "Dialogue id."), optional("options", table, "Options.")},
            boolean, "True when opened.", [&impl](CallContext &, std::span<const Value> args) -> Value {
                const Value &options = table_arg(args, 2);
                std::uint64_t entity_id{};
                if (const Value *npc = options.field("entity"); npc != nullptr && npc->type() == ValueType::handle)
                    entity_id = npc->as_handle().id;
                return impl.open_dialogue(impl.player_arg(arg(args, 0)), string_arg(args, 1, "dialogue"),
                                          string_or(options, "node", ""), entity_id);
            });
    api.act("dialogue.close", "Closes a player's dialogue.", {player_param}, nothing, "",
            [&impl](CallContext &, std::span<const Value> args) -> Value {
                impl.close_dialogue(impl.player_arg(arg(args, 0)), true);
                return Value{};
            });
    api.client("dialogue.current", "The dialogue node shown to the local player.", {}, table,
               "{session, title, speaker, text, portrait, choices} or nil.", capability::query,
               [&impl](CallContext &, std::span<const Value>) -> Value {
                   if (!impl.client_dialogue.has_value())
                       return Value{};
                   Array choices;
                   for (const std::string &choice : impl.client_dialogue->choices)
                       choices.emplace_back(choice);
                   return make_map({{"session", static_cast<std::int64_t>(impl.client_dialogue->session)},
                                    {"title", impl.client_dialogue->title},
                                    {"speaker", impl.client_dialogue->speaker},
                                    {"text", impl.client_dialogue->text},
                                    {"portrait", impl.client_dialogue->portrait},
                                    {"choices", Value{std::move(choices)}}});
               });
    api.client("dialogue.choose", "Answers the dialogue shown to the local player (1-based).",
               {param("choice", integer, "Choice number.")}, nothing, "", capability::network,
               [&impl, &self](CallContext &, std::span<const Value> args) -> Value {
                   if (!impl.client_dialogue.has_value())
                       return Value{};
                   const double choice = number_arg(args, 0, "choice");
                   if (choice < 1.0 || choice > 64.0)
                       throw ScriptError{"choice is 1-based"};
                   self.client_choose(impl.client_dialogue->session, static_cast<std::size_t>(choice) - 1U);
                   return Value{};
               });

    // =============================================================== events
    const ParamSpec who = param("player", handle, "The player.");
    const ParamSpec where = param("position", vector, "Where.");
    const ParamSpec block_name = param("block", text, "Block name or mod key.");
    api.event("player.joined", "A player joined the session (or the world opened).", side_server, false,
              {who, param("name", text, "Gamer tag.")});
    api.event("player.left", "A player left.", side_server, false, {who, param("name", text, "Gamer tag.")});
    api.event("player.spawned", "A player was placed into the world for the first time this session.",
              side_server, false, {who, where});
    api.event("player.died", "A player died.", side_server, false,
              {who, where, param("cause", text, "What killed them.")});
    api.event("player.respawned", "A player respawned.", side_server, false, {who, where});
    api.event("player.damaged",
              "A player is about to take damage. Return false to cancel; call players.set_damage "
              "to change it. `remote` true: another peer's player. Where the host decides health "
              "(server mods, or OpenMinerZ players only) that is asked first too and both work; "
              "in a session that may hold a player of the original game it is reported after the "
              "fact and neither works.",
              side_server, true,
              {who, param("amount", number, "Health points."), param("cause", text, "enemy, fall, lava, ..."),
               param("source", vector, "Where it came from."), param("health", number, "Health before."),
               param("remote", boolean, "Reported by another peer.")});
    api.event("block.breaking", "A block is about to be dug out. Return false to keep it.", side_server, true,
              {who, where, block_name});
    api.event("block.broken", "A block was dug out or blasted away.", side_server, false,
              {who, where, block_name, param("explosion", boolean, "Blasted by the server's own player.")});
    api.event("block.placing", "A block is about to be placed. Return false to refuse it.", side_server, true,
              {who, where, block_name, param("item", text, "The item placing it.")});
    api.event("block.placed", "A block was placed.", side_server, false, {who, where, block_name});
    api.event("item.used",
              "A player pressed use with an item. Return false to stop the item's own action.",
              side_server, true,
              {who, param("item", text, "Item name or mod key."), param("mod_item", text, "Mod key or nil."),
               param("target", table,
                     "The block the player aims at: {position, block, face (the empty cell before it), look "
                     "(view direction), liquid = {position, block} (the first liquid cell the look passes "
                     "within reach; the aim passes through liquids)}; {look} (and liquid) alone when no "
                     "block is targeted; nil when unknown.",
                     true)});
    api.event("item.crafted", "A player crafted something.", side_server, false,
              {who, param("item", text, "Result."), param("count", integer, "How many."),
               param("recipe", integer, "Mod recipe id, if one.")});
    api.event("enemy.spawning",
              "The server's own player is about to get a retail enemy. Return false to stop it.",
              side_server, true,
              {param("type", text, "zombie, archer, skeleton, felguard, alien."),
               param("type_id", integer, "EnemyTypeEnum."), where, who});
    api.event("enemy.spawned", "A retail enemy appeared (any player's).", side_server, false,
              {param("type", text, "Class."), param("type_id", integer, "EnemyTypeEnum."),
               param("id", integer, "Enemy id."), where, who});
    api.event("enemy.killed", "A retail enemy was killed.", side_server, false,
              {param("type", text, "Class."), param("type_id", integer, "EnemyTypeEnum."),
               param("id", integer, "Enemy id."), where, who, param("killer", handle, "Who killed it.")});
    api.event("chat.message", "A chat line. Return false to drop it.", side_server, true,
              {who, param("text", text, "The line, as sent.")});
    api.event("time.new_day", "A new day began (midnight).", side_server, false, {param("day", integer, "Day number.")});
    api.event("time.day_started", "Dawn (06:00).", side_server, false, {param("day", integer, "Day number.")});
    api.event("time.night_started", "Nightfall (21:00).", side_server, false, {param("day", integer, "Day number.")});
    api.event("worldgen.chunk",
              "A chunk is offered for decoration (after worldgen.enable). Build with "
              "structures.place / world.*; use `seed` or worldgen.random for repeatable choices.",
              side_server, false,
              {param("cx", integer, "Chunk X."), param("cz", integer, "Chunk Z."),
               param("x", integer, "World X of its corner."), param("z", integer, "World Z of its corner."),
               param("seed", integer, "This chunk's seed."), param("biome", text, "Biome at its centre.")});
    api.event("world.edit_done", "A fill/replace/structure job finished.", side_server, false,
              {param("job", integer, "Job id."), param("changed", integer, "Cells changed."),
               param("mod", text, "The mod that queued it.")});
    api.event("entity.spawning", "A spawn rule is about to spawn creatures. Return false to stop it.",
              side_server, true, {param("kind", text, "Creature key."), where, who});
    api.event("entity.spawned", "An entity appeared.", side_server, false,
              {param("entity", handle, "The entity."), param("kind", text, "Creature key."), where});
    api.event("entity.damaged", "An entity is about to be damaged. Return false to cancel.", side_server, true,
              {param("entity", handle, "The entity."), param("amount", number, "Damage."),
               param("attacker", handle, "Player or entity, or nil.")});
    api.event("entity.died", "An entity was killed (drops are rolled).", side_server, false,
              {param("entity", handle, "The entity."), param("kind", text, "Creature key."), where,
               param("killer", handle, "The player, or nil.")});
    api.event("entity.interacted",
              "A player talked to / used an entity. Return false to stop its dialogue opening.",
              side_server, true,
              {param("entity", handle, "The entity."), param("kind", text, "Creature key."), who});
    api.event("entity.arrived", "An entity on a path reached its end.", side_server, false,
              {param("entity", handle, "The entity."), param("reached", boolean, "Whether it got there.")});
    api.event("stat.changed",
              "A stat was set (server: by a script, with the player) or the local player's "
              "replicated stat changed (client).",
              side_both, false,
              {param("player", handle, "Server only."), param("stat", text, "Stat."),
               param("value", number, "New value."), param("previous", number, "Old value.")});
    api.event("stat.depleted", "A stat reached its minimum.", side_server, false, {who, param("stat", text, "Stat.")});
    api.event("stat.filled", "A stat reached its maximum.", side_server, false, {who, param("stat", text, "Stat.")});
    api.event("effect.started", "An effect began.", side_server, false,
              {who, param("effect", text, "Effect."), param("strength", number, "Strength."),
               param("duration", number, "Seconds, or -1.")});
    api.event("effect.ended", "An effect ended.", side_server, false, {who, param("effect", text, "Effect.")});
    api.event("quest.started", "A quest was given.", side_server, false, {who, param("quest", text, "Quest id.")});
    api.event("quest.progress", "An objective advanced.", side_server, false,
              {who, param("quest", text, "Quest id."), param("objective", text, "Objective id."),
               param("progress", integer, "Now."), param("required", integer, "Needed.")});
    api.event("quest.stage", "A quest moved to its next stage.", side_server, false,
              {who, param("quest", text, "Quest id."), param("stage", text, "Stage id.")});
    api.event("quest.completed", "A quest was completed.", side_server, false, {who, param("quest", text, "Quest id.")});
    api.event("quest.abandoned", "A quest was dropped.", side_server, false, {who, param("quest", text, "Quest id.")});
    api.event("quest.updated", "The local player's quest log changed.", side_client, false, {});
    api.event("dialogue.opened", "A dialogue opened (server: for a player; client: for this one).", side_both,
              false,
              {param("player", handle, "Server only."), param("dialogue", text, "Server only."),
               param("speaker", text, "Client only."), param("text", text, "Client only.")});
    api.event("dialogue.choice", "A player picked a dialogue choice.", side_server, false,
              {who, param("dialogue", text, "Dialogue id."), param("node", text, "Node."),
               param("choice", integer, "1-based."), param("text", text, "Choice text.")});
    api.event("dialogue.closed", "A dialogue closed.", side_both, false,
              {param("player", handle, "Server only."), param("dialogue", text, "Server only.")});

    // mod-ai: brains, ai, projectiles, teams, enemies, entities.find (ai_api.cpp).
    register_ai_api(registry, self, impl);
}

} // namespace openminerz::modding::gameplay
