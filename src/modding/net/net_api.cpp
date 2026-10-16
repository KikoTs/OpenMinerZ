// The `net` and `replicated` script libraries (openminerz/modding/net.hpp).

#include "openminerz/modding/net.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <tuple>

namespace openminerz::modding
{
namespace
{

// Script callbacks this module holds, so a replaced handler, an off() or the
// VM's teardown releases exactly what was taken.
struct Bindings final
{
    Bindings(ScriptHost &script_host, NetBackend &net_backend)
        : host{script_host}, backend{net_backend}
    {
    }

    ScriptHost &host;
    NetBackend &backend;
    std::map<std::tuple<Side, std::string, std::string>, Callback> handlers;
    struct Held final
    {
        Callback callback;
        std::string mod_id;
    };
    std::map<std::pair<Side, SubscriptionId>, Held> subscriptions;

    void release_handler(Side side, std::string_view mod_id, std::string_view name)
    {
        const auto found = handlers.find({side, std::string{mod_id}, std::string{name}});
        if (found == handlers.end())
            return;
        host.release(found->second);
        handlers.erase(found);
    }

    void teardown(Side side)
    {
        for (auto handler = handlers.begin(); handler != handlers.end();)
        {
            const auto &[handler_side, mod_id, name] = handler->first;
            if (handler_side != side)
            {
                ++handler;
                continue;
            }
            if (side == Side::server && backend.server() != nullptr)
                backend.server()->off(mod_id, name);
            if (side == Side::client && backend.client() != nullptr)
                backend.client()->off(mod_id, name);
            host.release(handler->second);
            handler = handlers.erase(handler);
        }
        for (auto subscription = subscriptions.begin(); subscription != subscriptions.end();)
        {
            if (subscription->first.first != side)
            {
                ++subscription;
                continue;
            }
            if (side == Side::server && backend.server() != nullptr)
                backend.server()->unsubscribe(subscription->first.second);
            if (side == Side::client && backend.client() != nullptr)
                backend.client()->unsubscribe(subscription->first.second);
            host.release(subscription->second.callback);
            subscription = subscriptions.erase(subscription);
        }
    }
};

ModNetServer &server_for(Bindings &bindings, std::string_view function)
{
    ModNetServer *server = bindings.backend.server();
    if (server == nullptr || !server->enabled())
        throw ScriptError{std::string{function} + ": this process is not the session's host"};
    return *server;
}

ModNetClient &client_for(Bindings &bindings, std::string_view function)
{
    ModNetClient *client = bindings.backend.client();
    if (client == nullptr || !client->enabled())
        throw ScriptError{std::string{function} + ": not connected to a host running this mod"};
    return *client;
}

const Value &argument(std::span<const Value> args, std::size_t index)
{
    static const Value nil;
    return index < args.size() ? args[index] : nil;
}

StoreRef parse_store(const Value &scope, std::string_view function)
{
    if (scope.type() == ValueType::string && scope.as_string() == "world")
        return StoreRef::world();
    if (scope.type() == ValueType::handle)
    {
        const Handle handle = scope.as_handle();
        const StoreRef store =
            handle.kind == HandleKind::player ? StoreRef::player(handle) : StoreRef::entity(handle);
        if (store.valid())
            return store;
    }
    throw ScriptError{std::string{function} +
                      ": scope must be \"world\", a player handle or an entity handle"};
}

StateFilter parse_filter(const Value &scope, const Value &key, std::string_view mod_id)
{
    StateFilter filter;
    filter.mod_id = std::string{mod_id};
    if (!key.is_nil())
        filter.key = key.as_string();
    if (scope.type() == ValueType::string)
    {
        const std::string &name = scope.as_string();
        if (name == "world")
            filter.scope = StoreScope::world;
        else if (name == "player")
            filter.scope = StoreScope::player;
        else if (name == "entity")
            filter.scope = StoreScope::entity;
        else if (name != "any")
            throw ScriptError{
                "replicated.on_change: scope must be \"world\", \"player\", \"entity\", \"any\" "
                "or a handle"};
        return filter;
    }
    const StoreRef store = parse_store(scope, "replicated.on_change");
    filter.scope = store.scope;
    filter.owner = store.owner;
    return filter;
}

ReplicatedKeyOptions parse_options(const Value &options)
{
    ReplicatedKeyOptions parsed;
    if (options.is_nil())
        return parsed;
    if (options.type() != ValueType::map)
        throw ScriptError{"replicated.set: options must be a table"};
    for (const auto &[name, value] : options.as_map())
    {
        if (name == "public")
            parsed.is_public = value.as_bool();
        else if (name == "priority")
        {
            if (value.type() == ValueType::string)
            {
                const std::string &level = value.as_string();
                if (level == "low")
                    parsed.priority = ReplicationPriority::low;
                else if (level == "normal")
                    parsed.priority = ReplicationPriority::normal;
                else if (level == "high")
                    parsed.priority = ReplicationPriority::high;
                else if (level == "critical")
                    parsed.priority = ReplicationPriority::critical;
                else
                    throw ScriptError{"replicated.set: priority is low, normal, high or critical"};
            }
            else
            {
                const std::int64_t level = value.as_integer();
                if (level < 0 || level > 3)
                    throw ScriptError{"replicated.set: priority is 0-3"};
                parsed.priority = static_cast<ReplicationPriority>(level);
            }
        }
        else
            throw ScriptError{"replicated.set: unknown option '" + name + "'"};
    }
    return parsed;
}

Value owner_value(const StoreRef &store)
{
    return store.scope == StoreScope::world ? Value{} : Value{store.owner};
}

ParamSpec param(std::string name, ValueType type, std::string doc, bool optional = false)
{
    return ParamSpec{std::move(name), type, optional, std::move(doc)};
}

} // namespace

void connect_net_events(ScriptHost &host, ModNetServer *server, ModNetClient *client)
{
    if (server != nullptr)
    {
        server->set_log_sink(
            [&host](std::string_view mod_id, LogLevel level, std::string_view message) {
                host.log(mod_id, level, message);
            });
        server->add_event_listener([&host](const ModNetEvent &event) {
            if (event.kind == ModNetEventKind::player_ready)
                host.emit(Side::server, "net.player_ready", Value{Map{{"player", event.player}}});
            else if (event.kind == ModNetEventKind::player_left)
                host.emit(Side::server, "net.player_left", Value{Map{{"player", event.player}}});
        });
    }
    if (client != nullptr)
    {
        client->set_log_sink(
            [&host](std::string_view mod_id, LogLevel level, std::string_view message) {
                host.log(mod_id, level, message);
            });
        client->add_event_listener([&host](const ModNetEvent &event) {
            if (event.kind == ModNetEventKind::ready)
                host.emit(Side::client, "net.ready", Value{Map{{"player", event.player}}});
        });
    }
}

void register_net_api(ApiRegistry &registry, ScriptHost &host, NetBackend &backend)
{
    auto bindings = std::make_shared<Bindings>(host, backend);
    host.add_teardown_listener([bindings](Side side) { bindings->teardown(side); });

    const ParamSpec name_param =
        param("name", ValueType::string,
              "Message name (1-64 letters, digits, _ . : - /), private to this mod.");
    const ParamSpec payload_param = param(
        "payload", ValueType::any,
        "Any value without functions; tables nest up to 8 levels (client) or 16 (host).", true);

    registry.add_function(
        {"net.send",
         "Sends a message to the host's `net.on(name, ...)` handler of this mod. Queued and paced "
         "under the host's rate limits; returns false when the outgoing queue is full.",
         {name_param, payload_param},
         ValueType::boolean,
         "true when queued",
         side_client,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            return Value{client_for(*bindings, "net.send")
                             .send(context.mod_id, args[0].as_string(), argument(args, 1U))};
        });

    registry.add_function(
        {"net.send_to",
         "Sends a message to one player's `net.on(name, ...)` handler of this mod.",
         {param("player", ValueType::handle, "The receiving player."), name_param, payload_param},
         ValueType::boolean,
         "false when the player has no mod channel",
         side_server,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            return Value{server_for(*bindings, "net.send_to")
                             .send_to(args[0].as_handle(), context.mod_id, args[1].as_string(),
                                      argument(args, 2U))};
        });

    registry.add_function(
        {"net.broadcast",
         "Sends a message to every ready player's `net.on(name, ...)` handler of this mod.",
         {name_param, payload_param},
         ValueType::integer,
         "how many players were addressed",
         side_server,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            return Value{static_cast<std::int64_t>(
                server_for(*bindings, "net.broadcast")
                    .broadcast(context.mod_id, args[0].as_string(), argument(args, 1U)))};
        });

    registry.add_function(
        {"net.on",
         "Handles messages `name` of this mod. On the host the handler is called as "
         "fn(player, payload) with the sending player (taken from the connection, never from the "
         "payload); on a client as fn(payload). With a schema (field -> \"number\", "
         "\"string?\", {type=\"integer\", min=0, max=9}, ...), payloads that do not match are "
         "dropped and logged before the handler runs. Replaces an earlier handler.",
         {name_param, param("handler", ValueType::callback, "The handler."),
          param("schema", ValueType::map, "Expected payload fields.", true)},
         ValueType::nil,
         {},
         side_both,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            const std::string &name = args[0].as_string();
            const Callback callback = args[1].as_callback();
            std::optional<PayloadSchema> schema;
            if (!argument(args, 2U).is_nil())
            {
                PayloadSchema parsed;
                std::string error;
                if (!parse_payload_schema(args[2], parsed, error))
                {
                    bindings->host.release(callback);
                    throw ScriptError{"net.on schema: " + error};
                }
                schema = std::move(parsed);
            }
            ScriptHost &script_host = bindings->host;
            try
            {
                if (context.side == Side::server)
                    server_for(*bindings, "net.on")
                        .on(
                            context.mod_id, name,
                            [&script_host, callback](Handle player, const Value &payload) {
                                const Value call_args[]{Value{player}, payload};
                                script_host.invoke_as(player, callback, call_args);
                            },
                            std::move(schema));
                else
                    client_for(*bindings, "net.on")
                        .on(
                            context.mod_id, name,
                            [&script_host, callback](const Value &payload) {
                                const Value call_args[]{payload};
                                script_host.invoke(callback, call_args);
                            },
                            std::move(schema));
            }
            catch (...)
            {
                bindings->host.release(callback);
                throw;
            }
            bindings->release_handler(context.side, context.mod_id, name);
            bindings->handlers.emplace(
                std::tuple<Side, std::string, std::string>{context.side,
                                                           std::string{context.mod_id}, name},
                callback);
            return {};
        });

    registry.add_function(
        {"net.off",
         "Removes this mod's handler for `name`.",
         {name_param},
         ValueType::nil,
         {},
         side_both,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            const std::string &name = args[0].as_string();
            if (context.side == Side::server && bindings->backend.server() != nullptr)
                bindings->backend.server()->off(context.mod_id, name);
            if (context.side == Side::client && bindings->backend.client() != nullptr)
                bindings->backend.client()->off(context.mod_id, name);
            bindings->release_handler(context.side, context.mod_id, name);
            return {};
        });

    registry.add_function({"net.players",
                           "Players whose mod channel is ready, in join order.",
                           {},
                           ValueType::array,
                           "player handles",
                           side_server,
                           capability::network},
                          [bindings](CallContext &, std::span<const Value>) -> Value {
                              Array players;
                              for (const Handle player :
                                   server_for(*bindings, "net.players").ready_players())
                                  players.emplace_back(player);
                              return Value{std::move(players)};
                          });

    registry.add_function(
        {"net.local_player",
         "This client's player handle, as the host knows it (nil before the host's welcome).",
         {},
         ValueType::handle,
         "the local player, or nil",
         side_client,
         capability::network},
        [bindings](CallContext &, std::span<const Value>) -> Value {
            const ModNetClient &client = client_for(*bindings, "net.local_player");
            return client.welcomed() ? Value{client.local_player()} : Value{};
        });

    registry.add_function(
        {"net.is_ready",
         "Whether the host's initial replicated snapshot has arrived (see the net.ready event).",
         {},
         ValueType::boolean,
         {},
         side_client,
         capability::network},
        [bindings](CallContext &, std::span<const Value>) -> Value {
            return Value{client_for(*bindings, "net.is_ready").ready()};
        });

    const ParamSpec scope_param =
        param("scope", ValueType::any, "\"world\", a player handle, or an entity handle.");
    const ParamSpec key_param = param(
        "key", ValueType::string, "Key (1-64 letters, digits, _ . : - /), private to this mod.");

    registry.add_function(
        {"replicated.set",
         "Sets (nil removes) a host-owned replicated value. World values reach every player, "
         "player values only that player unless {public = true}, entity values every interested "
         "player. Changes are sent once per tick, highest {priority = \"low\"|\"normal\"|"
         "\"high\"|\"critical\"} first within each player's bandwidth budget. Options stick to "
         "the key.",
         {scope_param, key_param,
          param("value", ValueType::any, "Any value without functions (<= 16 KiB encoded)."),
          param("options", ValueType::map, "{public = boolean, priority = ...}", true)},
         ValueType::nil,
         {},
         side_server,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            const StoreRef store = parse_store(args[0], "replicated.set");
            std::optional<ReplicatedKeyOptions> options;
            if (!argument(args, 3U).is_nil())
                options = parse_options(args[3]);
            server_for(*bindings, "replicated.set")
                .set(store, context.mod_id, args[1].as_string(), argument(args, 2U), options);
            return {};
        });

    registry.add_function({"replicated.get",
                           "Reads a replicated value (on a client: the last value the host sent).",
                           {scope_param, key_param},
                           ValueType::any,
                           "the value, or nil",
                           side_both,
                           capability::network},
                          [bindings](CallContext &context, std::span<const Value> args) -> Value {
                              const StoreRef store = parse_store(args[0], "replicated.get");
                              const Value *value =
                                  context.side == Side::server
                                      ? server_for(*bindings, "replicated.get")
                                            .get(store, context.mod_id, args[1].as_string())
                                      : client_for(*bindings, "replicated.get")
                                            .get(store, context.mod_id, args[1].as_string());
                              return value == nullptr ? Value{} : *value;
                          });

    registry.add_function(
        {"replicated.all",
         "Every key of this mod in one store, as a table.",
         {scope_param},
         ValueType::map,
         "key -> value",
         side_both,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            const StoreRef store = parse_store(args[0], "replicated.all");
            return Value{context.side == Side::server
                             ? server_for(*bindings, "replicated.all").all(store, context.mod_id)
                             : client_for(*bindings, "replicated.all").all(store, context.mod_id)};
        });

    registry.add_function({"replicated.clear",
                           "Removes every key of this mod from one store.",
                           {scope_param},
                           ValueType::nil,
                           {},
                           side_server,
                           capability::network},
                          [bindings](CallContext &context, std::span<const Value> args) -> Value {
                              server_for(*bindings, "replicated.clear")
                                  .clear(parse_store(args[0], "replicated.clear"), context.mod_id);
                              return {};
                          });

    registry.add_function(
        {"replicated.on_change",
         "Calls fn(value, old, key, owner) when one of this mod's replicated values changes "
         "(value nil when removed; owner nil for world values). scope is \"world\", \"player\", "
         "\"entity\", \"any\" or one handle; key nil watches every key. On a client it fires "
         "for the initial snapshot too.",
         {param("scope", ValueType::any, "Which stores to watch."),
          param("key", ValueType::any, "A key, or nil for every key."),
          param("handler", ValueType::callback, "fn(value, old, key, owner)")},
         ValueType::integer,
         "a subscription id for replicated.off",
         side_both,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            const Callback callback = args[2].as_callback();
            StateFilter filter;
            try
            {
                filter = parse_filter(args[0], args[1], context.mod_id);
            }
            catch (...)
            {
                bindings->host.release(callback);
                throw;
            }
            ScriptHost &script_host = bindings->host;
            StateListener listener = [&script_host, callback](const StateChange &change) {
                const Value call_args[]{change.value, change.old_value,
                                        Value{std::string{change.key}}, owner_value(change.store)};
                script_host.invoke(callback, call_args);
            };
            SubscriptionId id{};
            if (context.side == Side::server)
            {
                ModNetServer *server = bindings->backend.server();
                if (server == nullptr)
                {
                    bindings->host.release(callback);
                    throw ScriptError{"replicated.on_change: this process is not the host"};
                }
                id = server->subscribe(std::move(filter), std::move(listener));
            }
            else
            {
                ModNetClient *client = bindings->backend.client();
                if (client == nullptr)
                {
                    bindings->host.release(callback);
                    throw ScriptError{"replicated.on_change: not connected to a host"};
                }
                id = client->subscribe(std::move(filter), std::move(listener));
            }
            bindings->subscriptions.emplace(std::pair{context.side, id},
                                            Bindings::Held{callback, std::string{context.mod_id}});
            return Value{static_cast<std::int64_t>(id)};
        });

    registry.add_function(
        {"replicated.off",
         "Stops a replicated.on_change subscription of this mod.",
         {param("id", ValueType::integer, "The id replicated.on_change returned.")},
         ValueType::nil,
         {},
         side_both,
         capability::network},
        [bindings](CallContext &context, std::span<const Value> args) -> Value {
            const auto id = static_cast<SubscriptionId>(args[0].as_integer());
            const auto found = bindings->subscriptions.find({context.side, id});
            if (found == bindings->subscriptions.end() || found->second.mod_id != context.mod_id)
                return {};
            if (context.side == Side::server && bindings->backend.server() != nullptr)
                bindings->backend.server()->unsubscribe(id);
            if (context.side == Side::client && bindings->backend.client() != nullptr)
                bindings->backend.client()->unsubscribe(id);
            bindings->host.release(found->second.callback);
            bindings->subscriptions.erase(found);
            return {};
        });

    const std::vector<ParamSpec> player_payload{param("player", ValueType::handle, "The player.")};
    registry.add_event({"net.player_ready",
                        "A player's mod channel opened: its replicated snapshot is on its way "
                        "and net.send_to reaches it. Initialise per-player state here.",
                        side_server, false, player_payload});
    registry.add_event(
        {"net.player_left", "A player's mod channel closed.", side_server, false, player_payload});
    registry.add_event({"net.ready",
                        "The host's initial replicated snapshot has arrived on this client.",
                        side_client,
                        false,
                        {param("player", ValueType::handle, "This client's player.")}});
}

} // namespace openminerz::modding
