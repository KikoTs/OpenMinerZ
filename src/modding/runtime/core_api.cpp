// The runtime's own script libraries: events, log, timer, storage, mod (with
// mod.config) and util. They register through the same ApiRegistry as every
// service, so the SDK documents them like everything else.

#include "runtime_impl.hpp"

#include "openminerz/atomic_file.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace openminerz::modding
{
namespace
{

constexpr std::size_t maximum_storage_key_bytes = 128U;
constexpr std::size_t maximum_timers_per_mod = 4096U;
constexpr std::size_t maximum_config_settings = 256U;

// A folder name for an id or a player key: safe characters kept, anything
// else replaced, and a hash appended when that changed it so two keys can
// never share a file.
std::string safe_file_name(std::string_view text)
{
    std::string name;
    bool changed = text.empty() || text.size() > 64U;
    for (const char character : text.substr(0U, 64U))
    {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '-' || character == '_' || character == '.')
            name += character;
        else
        {
            name += '_';
            changed = true;
        }
    }
    if (name.starts_with('.'))
    {
        name.front() = '_';
        changed = true;
    }
    if (changed)
    {
        std::uint64_t hash = 0xCBF29CE484222325ULL;
        for (const char character : text)
            hash = (hash ^ static_cast<unsigned char>(character)) * 0x100000001B3ULL;
        name += std::format("-{:016x}", hash);
    }
    return name;
}

std::string describe(const Value &value, int depth = 0)
{
    switch (value.type())
    {
    case ValueType::nil:
    case ValueType::any:
        return "nil";
    case ValueType::boolean:
        return value.as_bool() ? "true" : "false";
    case ValueType::integer:
        return std::to_string(value.as_integer());
    case ValueType::number:
        return std::format("{}", value.as_number());
    case ValueType::string:
        return value.as_string();
    case ValueType::vec3: {
        const Vec3 vector = value.as_vec3();
        return std::format("vector({}, {}, {})", vector.x, vector.y, vector.z);
    }
    case ValueType::handle:
        return std::format("Handle({} {})", static_cast<std::uint32_t>(value.as_handle().kind),
                           value.as_handle().id);
    case ValueType::callback:
        return "function";
    case ValueType::array: {
        if (depth > 8)
            return "{...}";
        std::string text = "{";
        for (const Value &element : value.as_array())
            text += (text.size() > 1U ? ", " : "") + describe(element, depth + 1);
        return text + "}";
    }
    case ValueType::map: {
        if (depth > 8)
            return "{...}";
        std::string text = "{";
        for (const auto &[key, element] : value.as_map())
            text += (text.size() > 1U ? ", " : "") + key + " = " + describe(element, depth + 1);
        return text + "}";
    }
    }
    return "?";
}

// Refuses what cannot outlive the session: functions and handles.
void check_storable(const Value &value, int depth = 0)
{
    if (depth > maximum_value_depth)
        throw ScriptError{"stored values may nest at most 64 levels"};
    switch (value.type())
    {
    case ValueType::callback:
        throw ScriptError{"functions cannot be stored"};
    case ValueType::handle:
        throw ScriptError{"handles are only valid for one session and cannot be stored"};
    case ValueType::array:
        for (const Value &element : value.as_array())
            check_storable(element, depth + 1);
        return;
    case ValueType::map:
        for (const auto &[key, element] : value.as_map())
            check_storable(element, depth + 1);
        return;
    default:
        return;
    }
}

std::uint64_t next_random(std::uint64_t &state) noexcept
{
    // splitmix64: small, fast and the same on every machine.
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

void check_key(std::string_view key)
{
    if (key.empty() || key.size() > maximum_storage_key_bytes)
        throw ScriptError{"storage keys are 1-128 bytes"};
}

std::optional<std::string> read_text(const std::filesystem::path &file)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error))
        return std::nullopt;
    std::ifstream input{file, std::ios::binary};
    if (!input)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool write_text(const std::filesystem::path &file, std::string_view text, std::string &error)
{
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    if (!write_file_atomically(file, {reinterpret_cast<const std::uint8_t *>(text.data()),
                                      text.size()}))
    {
        error = "could not write " + file.generic_string();
        return false;
    }
    return true;
}

ValueType config_type(std::string_view name)
{
    if (name == "boolean")
        return ValueType::boolean;
    if (name == "integer")
        return ValueType::integer;
    if (name == "number")
        return ValueType::number;
    if (name == "string")
        return ValueType::string;
    throw ScriptError{"a setting's type is boolean, integer, number or string"};
}

// Whether `value` fits `setting`; integers are accepted for number settings.
bool config_accepts(const ConfigSetting &setting, const Value &value, std::string &problem)
{
    switch (setting.type)
    {
    case ValueType::boolean:
        if (value.type() == ValueType::boolean)
            return true;
        problem = "expects true or false";
        return false;
    case ValueType::integer:
    case ValueType::number: {
        if (setting.type == ValueType::integer ? value.type() != ValueType::integer
                                               : !value.is_number())
        {
            problem = setting.type == ValueType::integer ? "expects a whole number"
                                                         : "expects a number";
            return false;
        }
        const double number = value.as_number();
        if ((setting.minimum && number < *setting.minimum) ||
            (setting.maximum && number > *setting.maximum))
        {
            problem = std::format("expects a value from {} to {}", setting.minimum.value_or(-1e300),
                                  setting.maximum.value_or(1e300));
            return false;
        }
        return true;
    }
    case ValueType::string:
        if (value.type() != ValueType::string)
        {
            problem = "expects text";
            return false;
        }
        if (!setting.choices.empty() &&
            std::find(setting.choices.begin(), setting.choices.end(), value.as_string()) ==
                setting.choices.end())
        {
            problem = "expects one of its choices";
            return false;
        }
        if (value.as_string().size() > 1024U)
        {
            problem = "text settings hold at most 1024 bytes";
            return false;
        }
        return true;
    default:
        problem = "has no editable type";
        return false;
    }
}

} // namespace

ModSlot *ModRuntime::Impl::calling_mod(const CallContext &context, Vm **vm_out) const
{
    Vm *vm = vm_for(context.side);
    if (vm == nullptr)
        throw ScriptError{"no script VM runs on this side"};
    const auto found = vm->mod_lookup.find(lowercase_ascii(context.mod_id));
    if (found == vm->mod_lookup.end())
        throw ScriptError{"the calling mod is not loaded"};
    if (vm_out != nullptr)
        *vm_out = vm;
    return vm->mods[found->second].get();
}

void ModRuntime::Impl::remove_mod_handlers(Vm &vm, std::uint32_t mod)
{
    for (auto &[name, handlers] : vm.subscriptions)
        std::erase_if(handlers, [&](const std::shared_ptr<Subscription> &handler) {
            if (handler->mod != mod)
                return false;
            handler->active = false;
            release(handler->callback);
            return true;
        });
    std::erase_if(vm.timers, [&](const Timer &timer) {
        if (timer.mod != mod)
            return false;
        release(timer.callback);
        return true;
    });
}

StorageScope &ModRuntime::Impl::storage_scope(std::string_view mod_key, std::string_view scope,
                                              const std::filesystem::path &file)
{
    const std::string key = std::string{mod_key} + "|" + std::string{scope};
    auto found = storage.find(key);
    if (found == storage.end())
        found = storage.emplace(key, StorageScope{}).first;
    StorageScope &data = found->second;
    if (data.loaded)
        return data;
    data.loaded = true;
    data.file = file;
    if (file.empty())
        return data;
    const std::optional<std::string> text = read_text(file);
    if (!text)
        return data;
    Value value;
    std::string error;
    if (!deserialize_value(*text, value, error) || value.type() != ValueType::map)
    {
        // Keep the damaged file for the author; the scope starts empty.
        std::error_code ignored;
        std::filesystem::copy_file(file, file.string() + ".damaged",
                                   std::filesystem::copy_options::overwrite_existing, ignored);
        log(mod_key, Side::server, LogLevel::error,
            std::format("{} is damaged ({}); starting empty, a copy was kept as .damaged",
                        file.generic_string(), error.empty() ? "not a table" : error));
        return data;
    }
    for (const auto &[name, element] : value.as_map())
    {
        std::string text_value;
        std::string ignored;
        if (!serialize_value(element, text_value, ignored))
            continue;
        const std::size_t bytes = name.size() + text_value.size();
        data.entries.insert_or_assign(name, StorageScope::Entry{element, bytes});
        data.bytes += bytes;
    }
    return data;
}

void ModRuntime::Impl::persist_storage()
{
    for (auto &[key, scope] : storage)
    {
        if (!scope.dirty || scope.file.empty())
            continue;
        Map map;
        map.reserve(scope.entries.size());
        for (const auto &[name, entry] : scope.entries)
            map.emplace_back(name, entry.value);
        std::string text;
        std::string error;
        if (serialize_value(Value{std::move(map)}, text, error) &&
            write_text(scope.file, text, error))
            scope.dirty = false;
        else
            log({}, Side::server, LogLevel::error,
                std::format("mod storage {} was not saved: {}", key, error));
    }
}

void ModRuntime::Impl::load_config(std::string_view mod_key)
{
    if (config_loaded[std::string{mod_key}])
        return;
    config_loaded[std::string{mod_key}] = true;
    if (session.local_dir.empty())
        return;
    const std::optional<std::string> text =
        read_text(session.local_dir / safe_file_name(mod_key) / "config.json");
    Value value;
    std::string error;
    if (!text || !deserialize_value(*text, value, error) || value.type() != ValueType::map)
        return;
    // Stored values wait as "user_set" placeholders until their definition.
    std::vector<ConfigEntry> &entries = config[std::string{mod_key}];
    for (const auto &[key, stored] : value.as_map())
    {
        ConfigEntry entry;
        entry.setting.key = key;
        entry.setting.value = stored;
        entry.setting.type = ValueType::nil;
        entry.user_set = true;
        entries.push_back(std::move(entry));
    }
}

void ModRuntime::Impl::save_config(std::string_view mod_key)
{
    if (session.local_dir.empty())
        return;
    Map map;
    for (const ConfigEntry &entry : config[std::string{mod_key}])
        if (entry.user_set)
            map.emplace_back(entry.setting.key, entry.setting.value);
    std::string text;
    std::string error;
    if (!serialize_value(Value{std::move(map)}, text, error) ||
        !write_text(session.local_dir / safe_file_name(mod_key) / "config.json", text, error))
        log(mod_key, Side::client, LogLevel::error, "mod settings were not saved: " + error);
}

void ModRuntime::Impl::register_core_api()
{
    using enum ValueType;
    const auto add = [this](FunctionSpec spec, NativeFunction function) {
        registry.add_function(std::move(spec), std::move(function));
    };

    // --- Events the runtime fires ------------------------------------------
    registry.add_event({"mod.loaded",
                        "Every mod on this side has run its scripts.",
                        side_both,
                        false,
                        {{"side", string, false, "\"server\" or \"client\""},
                         {"mods", array, false, "ids of the mods running here, in load order"}}});
    registry.add_event({"world.loaded",
                        "The world is ready: blocks, players and storage can be used.",
                        side_both,
                        false,
                        {{"name", string, true, "the world's name"},
                         {"seed", integer, true, "the world's generation seed"},
                         {"mode", string, true, "\"offline\", \"host\" or \"client\""}}});
    registry.add_event({"world.unloading",
                        "The session is ending (or a developer reload). Storage is saved "
                        "right after the handlers run.",
                        side_both,
                        false,
                        {{"side", string, false, "\"server\" or \"client\""}}});
    registry.add_event({"tick",
                        "One fixed simulation step on the server.",
                        side_server,
                        false,
                        {{"dt", number, false, "seconds simulated by this step"},
                         {"tick", integer, false, "steps since the session started"}}});
    registry.add_event({"frame",
                        "One rendered frame on the client.",
                        side_client,
                        false,
                        {{"dt", number, false, "seconds since the previous frame"}}});
    registry.add_event({"config.changed",
                        "The player changed one of a mod's settings.",
                        side_both,
                        false,
                        {{"mod", string, false, "the mod that owns the setting"},
                         {"key", string, false, "the setting"},
                         {"value", any, false, "its new value"}}});

    // --- events -------------------------------------------------------------
    const auto check_event = [this](const Vm &vm, std::string_view name) {
        if (const std::size_t colon = name.find(':');
            colon != std::string_view::npos && colon > 0U && colon + 1U < name.size())
            return;
        const EventSpec *spec = registry.find_event(name);
        if (spec == nullptr)
            throw ScriptError{std::format("unknown event '{}'", name)};
        if (!side_in(vm.side, spec->sides))
            throw ScriptError{std::format("event '{}' only fires on the {}", name,
                                          spec->sides == side_server ? "server" : "client")};
    };
    const auto subscribe = [this, check_event](CallContext &context, std::span<const Value> args,
                                               bool once) -> Value {
        Vm *vm{};
        ModSlot *slot = calling_mod(context, &vm);
        const Callback handler = args[1].as_callback();
        try
        {
            check_event(*vm, args[0].as_string());
        }
        catch (...)
        {
            release(handler);
            throw;
        }
        auto &handlers = vm->subscriptions[args[0].as_string()];
        const auto position =
            std::upper_bound(handlers.begin(), handlers.end(), slot->index,
                             [](std::uint32_t mod, const auto &handler) { return mod < handler->mod; });
        handlers.insert(position, std::make_shared<Subscription>(
                                      Subscription{slot->index, handler, once, true}));
        return {};
    };
    const std::vector<ParamSpec> handler_params{
        {"name", string, false, "an event name, or \"<mod id>:<name>\" for a mod's own event"},
        {"handler", callback, false, "called with the event's payload table"}};
    add({"events.on",
         "Calls `handler(payload)` every time the event fires on this side. A handler of a "
         "cancellable event returns false to cancel it.",
         handler_params},
        [subscribe](CallContext &context, std::span<const Value> args) {
            return subscribe(context, args, false);
        });
    add({"events.once", "Like events.on, for the next time only.", handler_params},
        [subscribe](CallContext &context, std::span<const Value> args) {
            return subscribe(context, args, true);
        });
    add({"events.off",
         "Removes a handler this mod added for the event.",
         handler_params,
         boolean,
         "true when a handler was removed"},
        [this](CallContext &context, std::span<const Value> args) -> Value {
            Vm *vm{};
            ModSlot *slot = calling_mod(context, &vm);
            const Callback handler = args[1].as_callback();
            bool removed{};
            const auto found = vm->subscriptions.find(args[0].as_string());
            lua_State *L = vm->active_thread;
            if (found != vm->subscriptions.end() && L != nullptr)
            {
                auto &handlers = found->second;
                for (auto entry = handlers.begin(); entry != handlers.end(); ++entry)
                {
                    if ((*entry)->mod != slot->index || !push_callback(*vm, L, (*entry)->callback))
                        continue;
                    if (!push_callback(*vm, L, handler))
                        lua_pushnil(L);
                    const bool same = lua_rawequal(L, -1, -2) != 0;
                    lua_pop(L, 2);
                    if (!same)
                        continue;
                    (*entry)->active = false;
                    release((*entry)->callback);
                    handlers.erase(entry);
                    removed = true;
                    break;
                }
            }
            release(handler);
            return removed;
        });
    add({"events.emit",
         "Fires one of this mod's own events (\"<mod id>:<name>\") to every mod on this side. "
         "Any handler returning false cancels it.",
         {{"name", string, false, "\"<this mod's id>:<name>\""},
          {"payload", any, true, "passed to every handler"}},
         boolean,
         "false when a handler cancelled the event"},
        [this](CallContext &context, std::span<const Value> args) -> Value {
            Vm *vm{};
            ModSlot *slot = calling_mod(context, &vm);
            const std::string &name = args[0].as_string();
            const std::size_t colon = name.find(':');
            if (colon == std::string::npos || lowercase_ascii(name.substr(0U, colon)) != slot->key ||
                colon + 1U >= name.size())
                throw ScriptError{std::format("a mod emits only its own events, named \"{}:<name>\"",
                                              slot->source.id)};
            const Value payload = args.size() > 1U ? args[1] : Value{};
            return emit(vm->side, name, payload);
        });

    // --- log ----------------------------------------------------------------
    for (const auto &[suffix, level] :
         {std::pair{"debug", LogLevel::debug}, std::pair{"info", LogLevel::info},
          std::pair{"warn", LogLevel::warning}, std::pair{"error", LogLevel::error}})
    {
        FunctionSpec spec{std::string{"log."} + suffix,
                          std::format("Writes its arguments, separated by spaces, to the game log "
                                      "at {} level, tagged with the mod's id.",
                                      suffix)};
        spec.variadic = true;
        add(std::move(spec), [this, level](CallContext &context, std::span<const Value> args) {
            std::string line;
            for (const Value &argument : args)
            {
                if (!line.empty())
                    line += ' ';
                line += describe(argument);
                if (argument.type() == ValueType::callback)
                    release(argument.as_callback());
            }
            log(context.mod_id, context.side, level, line);
            return Value{};
        });
    }

    // --- timer --------------------------------------------------------------
    const auto schedule = [this](CallContext &context, std::span<const Value> args,
                                 bool repeating) -> Value {
        Vm *vm{};
        ModSlot *slot = calling_mod(context, &vm);
        const Callback function = args[1].as_callback();
        const double seconds = args[0].as_number();
        const std::size_t owned = static_cast<std::size_t>(std::count_if(
            vm->timers.begin(), vm->timers.end(),
            [&](const Timer &timer) { return timer.mod == slot->index; }));
        std::string problem;
        if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 1e7)
            problem = "seconds must be from 0 to 10000000";
        else if (repeating && seconds < 0.001)
            problem = "a repeating timer needs at least 0.001 seconds";
        else if (owned >= maximum_timers_per_mod)
            problem = "a mod may have at most 4096 timers";
        if (!problem.empty())
        {
            release(function);
            throw ScriptError{problem};
        }
        const std::uint64_t id = next_timer++;
        vm->timers.push_back(
            Timer{id, slot->index, function, vm->clock + seconds, repeating ? seconds : 0.0});
        return Handle{HandleKind::timer, id};
    };
    const std::vector<ParamSpec> timer_params{
        {"seconds", number, false, "delay; server timers count simulation ticks, client timers frames"},
        {"callback", callback, false, "called with no arguments"}};
    add({"timer.after", "Calls `callback` once, `seconds` from now.", timer_params, handle,
         "the timer, for timer.cancel"},
        [schedule](CallContext &context, std::span<const Value> args) {
            return schedule(context, args, false);
        });
    add({"timer.every", "Calls `callback` every `seconds` until cancelled.", timer_params, handle,
         "the timer, for timer.cancel"},
        [schedule](CallContext &context, std::span<const Value> args) {
            return schedule(context, args, true);
        });
    add({"timer.cancel",
         "Stops one of this mod's timers.",
         {{"timer", handle, false, "from timer.after or timer.every"}},
         boolean,
         "true when it was still pending"},
        [this](CallContext &context, std::span<const Value> args) -> Value {
            Vm *vm{};
            ModSlot *slot = calling_mod(context, &vm);
            const Handle timer = args[0].as_handle();
            const auto found = std::find_if(vm->timers.begin(), vm->timers.end(), [&](const Timer &entry) {
                return timer.kind == HandleKind::timer && entry.id == timer.id &&
                       entry.mod == slot->index;
            });
            if (found == vm->timers.end())
                return false;
            const Callback callback = found->callback;
            vm->timers.erase(found);
            release(callback);
            return true;
        });

    // --- storage ------------------------------------------------------------
    // world/player live in the world's sidecar folder, machine in the local
    // mod data folder. Each scope of each mod is capped by limits.storage_bytes.
    const auto scope_for = [this](CallContext &context, std::string_view kind,
                                  const Value *player) -> StorageScope & {
        Vm *vm{};
        ModSlot *slot = calling_mod(context, &vm);
        const std::filesystem::path base =
            kind == "machine" ? vm->session.local_dir : vm->session.world_dir;
        const std::string folder = safe_file_name(slot->source.id);
        if (kind == "player")
        {
            if (!player_key)
                throw ScriptError{"player storage is not available in this session"};
            const std::optional<std::string> key = player_key(player->as_handle());
            if (!key || key->empty())
                throw ScriptError{"that handle is not a known player"};
            return storage_scope(slot->key, "player|" + *key,
                                 base.empty() ? std::filesystem::path{}
                                              : base / folder / "players" /
                                                    (safe_file_name(*key) + ".json"));
        }
        if (kind == "world")
            return storage_scope(slot->key, "world",
                                 base.empty() ? std::filesystem::path{}
                                              : base / folder / "world.json");
        return storage_scope(slot->key, "machine",
                             base.empty() ? std::filesystem::path{} : base / folder / "machine.json");
    };
    const auto store = [this](StorageScope &scope, const std::string &key, const Value &value) {
        check_key(key);
        const auto existing = scope.entries.find(key);
        const std::size_t old_bytes = existing == scope.entries.end() ? 0U : existing->second.bytes;
        if (value.is_nil())
        {
            if (existing != scope.entries.end())
            {
                scope.bytes -= old_bytes;
                scope.entries.erase(existing);
                scope.dirty = true;
            }
            return;
        }
        check_storable(value);
        std::string text;
        std::string error;
        if (!serialize_value(value, text, error))
            throw ScriptError{error};
        const std::size_t bytes = key.size() + text.size();
        if (scope.bytes - old_bytes + bytes > limits.storage_bytes)
            throw ScriptError{std::format("this would exceed the mod's {} KiB storage limit",
                                          limits.storage_bytes >> 10U)};
        scope.bytes = scope.bytes - old_bytes + bytes;
        scope.entries.insert_or_assign(key, StorageScope::Entry{value, bytes});
        scope.dirty = true;
    };
    const auto keys_of = [](const StorageScope &scope) {
        Array keys;
        keys.reserve(scope.entries.size());
        for (const auto &[key, entry] : scope.entries)
            keys.emplace_back(key);
        return Value{std::move(keys)};
    };
    struct StorageKind final
    {
        const char *name;
        const char *doc;
        SideMask sides;
        CapabilityMask caps;
        bool player;
    };
    for (const StorageKind &kind :
         {StorageKind{"world", "this world (saved in the world's mods/ folder)", side_server,
                      capability::world_storage, false},
          StorageKind{"player", "one player in this world", side_server,
                      capability::world_storage, true},
          StorageKind{"machine", "this computer (client settings and caches)", side_client,
                      capability::local_storage, false}})
    {
        const std::string library = std::string{"storage."} + kind.name;
        std::vector<ParamSpec> prefix;
        if (kind.player)
            prefix.push_back({"player", handle, false, "the player"});
        const auto params = [&](std::initializer_list<ParamSpec> extra) {
            std::vector<ParamSpec> result = prefix;
            result.insert(result.end(), extra);
            return result;
        };
        const std::size_t first = kind.player ? 1U : 0U;
        const std::string scope_name = kind.name;
        add({library + ".get", std::string{"Reads a value this mod stored for "} + kind.doc + ".",
             params({{"key", string, false, "1-128 bytes"}}), any, "the value, or nil",
             kind.sides, kind.caps},
            [scope_for, scope_name, first](CallContext &context, std::span<const Value> args) {
                StorageScope &scope =
                    scope_for(context, scope_name, first != 0U ? &args[0] : nullptr);
                const auto found = scope.entries.find(args[first].as_string());
                return found == scope.entries.end() ? Value{} : found->second.value;
            });
        add({library + ".set",
             std::string{"Stores a value (tables, numbers, text, booleans, vectors) for "} +
                 kind.doc + "; nil deletes it. Saved with the world.",
             params({{"key", string, false, "1-128 bytes"},
                     {"value", any, true, "anything but functions and handles"}}),
             nil, {}, kind.sides, kind.caps},
            [scope_for, store, scope_name, first](CallContext &context,
                                                  std::span<const Value> args) {
                StorageScope &scope =
                    scope_for(context, scope_name, first != 0U ? &args[0] : nullptr);
                store(scope, args[first].as_string(),
                      args.size() > first + 1U ? args[first + 1U] : Value{});
                return Value{};
            });
        add({library + ".keys", std::string{"Every key this mod stored for "} + kind.doc + ".",
             params({}), array, "sorted keys", kind.sides, kind.caps},
            [scope_for, keys_of, scope_name, first](CallContext &context,
                                                    std::span<const Value> args) {
                return keys_of(scope_for(context, scope_name, first != 0U ? &args[0] : nullptr));
            });
    }

    // --- mod ----------------------------------------------------------------
    add({"mod.id", "This mod's id.", {}, string, "the id from openminerz.mod"},
        [this](CallContext &context, std::span<const Value>) -> Value {
            return calling_mod(context)->source.id;
        });
    add({"mod.version", "This mod's version.", {}, string, "the version from openminerz.mod"},
        [this](CallContext &context, std::span<const Value>) -> Value {
            return calling_mod(context)->source.version;
        });
    add({"mod.side", "Where this script runs.", {}, string, "\"server\" or \"client\""},
        [](CallContext &context, std::span<const Value>) -> Value {
            return side_name(context.side);
        });
    add({"mod.has",
         "Whether this mod may call an API function here (it exists, runs on this side, and "
         "the mod has its capability).",
         {{"name", string, false, "a dotted name such as \"world.set_block\""}},
         boolean,
         "true when calling it would not be refused"},
        [this](CallContext &context, std::span<const Value> args) -> Value {
            const ApiRegistry::Function *function = registry.find_function(args[0].as_string());
            return function != nullptr && side_in(context.side, function->spec.sides) &&
                   (function->spec.requires_caps & ~context.capabilities) == 0U;
        });
    add({"mod.loaded",
         "Whether another mod is loaded and running on this side.",
         {{"id", string, false, "a mod id"}},
         boolean,
         "true when it runs here"},
        [this](CallContext &context, std::span<const Value> args) -> Value {
            const Vm *vm = vm_for(context.side);
            if (vm == nullptr)
                return false;
            const auto found = vm->mod_lookup.find(lowercase_ascii(args[0].as_string()));
            return found != vm->mod_lookup.end() &&
                   vm->mods[found->second]->state == ModState::running;
        });

    // --- mod.config ---------------------------------------------------------
    add({"mod.config.define",
         "Declares a setting the player can change on the mod's settings page. Call it from "
         "shared/ so both sides know it.",
         {{"key", string, false, "1-64 characters: letters, digits, _ . -"},
          {"spec", map, false,
           "{type = \"boolean\"|\"integer\"|\"number\"|\"string\", default = value, label, "
           "description, min, max, step, choices = {\"a\", \"b\"}}"}},
         any,
         "the setting's current value"},
        [this](CallContext &context, std::span<const Value> args) -> Value {
            ModSlot *slot = calling_mod(context);
            const std::string &key = args[0].as_string();
            if (key.empty() || key.size() > 64U ||
                !std::all_of(key.begin(), key.end(), [](char character) {
                    return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
                           character == '_' || character == '.' || character == '-';
                }))
                throw ScriptError{"setting keys are 1-64 letters, digits, _ . or -"};
            const Value &spec = args[1];
            ConfigSetting setting;
            setting.mod_id = slot->source.id;
            setting.key = key;
            setting.type = config_type(spec.field_or("type", "boolean").as_string());
            setting.label = spec.field_or("label", key).as_string();
            setting.doc = spec.field_or("description", "").as_string();
            if (const Value *minimum = spec.field("min"); minimum && !minimum->is_nil())
                setting.minimum = minimum->as_number();
            if (const Value *maximum = spec.field("max"); maximum && !maximum->is_nil())
                setting.maximum = maximum->as_number();
            if (const Value *step = spec.field("step"); step && !step->is_nil())
                setting.step = step->as_number();
            if (const Value *choices = spec.field("choices"); choices && !choices->is_nil())
                for (const Value &choice : choices->as_array())
                    setting.choices.push_back(choice.as_string());
            const Value *fallback = spec.field("default");
            if (fallback == nullptr || fallback->is_nil())
                throw ScriptError{"a setting needs a default"};
            std::string problem;
            if (!config_accepts(setting, *fallback, problem))
                throw ScriptError{"the default " + problem};
            setting.default_value = *fallback;

            load_config(slot->key);
            std::vector<ConfigEntry> &entries = config[slot->key];
            auto existing = std::find_if(entries.begin(), entries.end(),
                                         [&](const ConfigEntry &entry) { return entry.setting.key == key; });
            if (existing == entries.end())
            {
                if (entries.size() >= maximum_config_settings)
                    throw ScriptError{"a mod may define at most 256 settings"};
                entries.push_back({});
                existing = std::prev(entries.end());
            }
            const bool keep = existing->user_set && config_accepts(setting, existing->setting.value, problem);
            setting.value = keep ? existing->setting.value : setting.default_value;
            existing->setting = std::move(setting);
            existing->user_set = keep;
            return existing->setting.value;
        });
    add({"mod.config.get",
         "Reads one of this mod's settings.",
         {{"key", string, false, "a key passed to mod.config.define"}},
         any,
         "its current value"},
        [this](CallContext &context, std::span<const Value> args) -> Value {
            ModSlot *slot = calling_mod(context);
            for (const ConfigEntry &entry : config[slot->key])
                if (entry.setting.key == args[0].as_string() && entry.setting.type != ValueType::nil)
                    return entry.setting.value;
            throw ScriptError{std::format("setting '{}' is not defined (mod.config.define first)",
                                          args[0].as_string())};
        });

    // --- util ---------------------------------------------------------------
    add({"util.random",
         "Deterministic random numbers, seeded from the world seed and the mod id: the same "
         "world replays the same sequence. random() is in [0, 1), random(n) in 1..n, "
         "random(m, n) in m..n.",
         {{"m", integer, true, "upper bound, or lower bound with n"},
          {"n", integer, true, "upper bound"}},
         number,
         "the number"},
        [this](CallContext &context, std::span<const Value> args) -> Value {
            ModSlot *slot = calling_mod(context);
            const std::uint64_t bits = next_random(slot->random_state);
            const double unit = static_cast<double>(bits >> 11U) * 0x1.0p-53;
            const bool has_m = !args.empty() && !args[0].is_nil();
            const bool has_n = args.size() > 1U && !args[1].is_nil();
            if (!has_m)
                return unit;
            const std::int64_t low = has_n ? args[0].as_integer() : 1;
            const std::int64_t high = has_n ? args[1].as_integer() : args[0].as_integer();
            if (high < low || static_cast<double>(high) - static_cast<double>(low) > 9.0e15)
                throw ScriptError{"util.random: empty or too large interval"};
            const double span = static_cast<double>(high - low) + 1.0;
            return low + static_cast<std::int64_t>(std::floor(unit * span));
        });
    add({"util.serialize",
         "Turns a value into compact JSON text (vectors and nested tables included; functions "
         "and handles cannot be serialized).",
         {{"value", any, true, "the value"}},
         string,
         "JSON text"},
        [this](CallContext &, std::span<const Value> args) -> Value {
            const Value value = args.empty() ? Value{} : args[0];
            if (value.type() == ValueType::callback)
                release(value.as_callback());
            check_storable(value);
            std::string text;
            std::string error;
            if (!serialize_value(value, text, error))
                throw ScriptError{error};
            return text;
        });
    add({"util.deserialize",
         "Reads text written by util.serialize (or plain JSON) back into a value.",
         {{"text", string, false, "JSON text"}},
         any,
         "the value"},
        [](CallContext &, std::span<const Value> args) -> Value {
            Value value;
            std::string error;
            if (!deserialize_value(args[0].as_string(), value, error))
                throw ScriptError{error};
            return value;
        });
}

std::vector<ConfigSetting> ModRuntime::config_settings() const
{
    std::vector<ConfigSetting> settings;
    for (const auto &[mod, entries] : impl_->config)
        for (const ConfigEntry &entry : entries)
            if (entry.setting.type != ValueType::nil)
                settings.push_back(entry.setting);
    return settings;
}

bool ModRuntime::set_config(std::string_view mod_id, std::string_view key, const Value &value,
                            std::string &error)
{
    const std::string mod_key = lowercase_ascii(mod_id);
    const auto found = impl_->config.find(mod_key);
    if (found == impl_->config.end())
    {
        error = "that mod has no settings";
        return false;
    }
    for (ConfigEntry &entry : found->second)
    {
        if (entry.setting.key != key || entry.setting.type == ValueType::nil)
            continue;
        std::string problem;
        if (!config_accepts(entry.setting, value, problem))
        {
            error = entry.setting.label + " " + problem;
            return false;
        }
        entry.setting.value = value;
        entry.user_set = true;
        impl_->save_config(mod_key);
        const Map payload{{"mod", entry.setting.mod_id}, {"key", std::string{key}}, {"value", value}};
        impl_->emit(Side::server, "config.changed", payload);
        impl_->emit(Side::client, "config.changed", payload);
        error.clear();
        return true;
    }
    error = "unknown setting";
    return false;
}

} // namespace openminerz::modding
