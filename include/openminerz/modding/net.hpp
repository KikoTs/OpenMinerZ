#pragma once

// The mod network layer: RPC, replicated state and the session content
// registry for server/core mods, carried over the OpenMinerZ extension channel
// (openminerz/net/mod_channel.hpp) on the existing reliable-ordered path.
//
// Who talks to whom
//   ModNetServer   one per authoritative session (offline world, listen host).
//                  Every connected player, including the host's own client VM,
//                  is a peer identified by its transport PeerId.
//   ModNetClient   one per client VM session (offline, listen host, or joined
//                  to an OpenMinerZ host whose manifest carries server mods).
// Offline and on a listen host both run in one process and exchange the same
// frames through pump_mod_net_loopback(), so single-player takes the exact
// code path of online play.
//
// Wire safety
//   - Nothing exists on the wire unless the session manifest carries a
//     server/core mod: a ModNetServer/ModNetClient built for a manifest
//     without one is disabled and never produces a byte.
//   - The server only ever hands frames for peers the app added with
//     add_peer(), i.e. peers that completed the extension hello. Stock peers
//     are never added (and cannot be admitted to a modded session at all).
//   - Every inbound frame passes per-peer token buckets (frames, bytes,
//     messages), CRC, bounded reassembly and a bounded Value decode before any
//     script runs. The sender is the transport PeerId given to receive(),
//     never anything in the payload. Clients cannot write replicated state.
//
// Namespacing
//   RPC names and replicated keys belong to the mod that uses them: the wire
//   carries the mod's slot in the session manifest, so mod A's "buy" and mod
//   B's "buy" never meet. Engine-level state that belongs to no mod (Gameplay
//   C++) uses the empty mod id "".

#include "openminerz/modding/api.hpp"
#include "openminerz/net/mod_handshake.hpp"
#include "openminerz/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::modding
{

// ---------------------------------------------------------------------------
// Value wire encoding
// ---------------------------------------------------------------------------

// Bounds applied to every Value that crosses the wire, in both directions.
struct ValueLimits final
{
    std::uint32_t max_depth{16U};
    // Total values (every nil, number, array element, map entry) in one payload.
    std::uint32_t max_nodes{4096U};
    std::uint32_t max_string_bytes{16U * 1024U};
    std::uint32_t max_key_bytes{128U};
    std::uint32_t max_encoded_bytes{64U * 1024U};
};

// Compact tagged binary form (little-endian, LEB128 lengths, zigzag integers,
// floats sent as f32 when that is exact). Callbacks and non-finite numbers are
// refused; map keys must be unique. encode appends to `out`.
[[nodiscard]] bool encode_value(const Value &value, const ValueLimits &limits,
                                std::vector<std::uint8_t> &out, std::string &error);
// Decodes exactly one value occupying all of `bytes`.
[[nodiscard]] bool decode_value(std::span<const std::uint8_t> bytes, const ValueLimits &limits,
                                Value &value, std::string &error);
// Decodes one value from the start of `bytes`; `consumed` is its length.
[[nodiscard]] bool decode_value_prefix(std::span<const std::uint8_t> bytes,
                                       const ValueLimits &limits, Value &value,
                                       std::size_t &consumed, std::string &error);

// ---------------------------------------------------------------------------
// Payload schemas (net.on(name, fn, schema))
// ---------------------------------------------------------------------------

struct FieldSchema;

// What a server handler expects. A payload that does not match is dropped and
// logged before the handler runs.
struct PayloadSchema final
{
    std::vector<FieldSchema> fields;
    // Fields not listed are refused unless this is set.
    bool allow_extra{};
};

struct FieldSchema final
{
    std::string name;
    ValueType type{ValueType::any};
    bool optional{};
    std::optional<double> minimum; // numbers: value; strings/arrays: length
    std::optional<double> maximum;
    std::shared_ptr<PayloadSchema> nested; // type map
};

// Script form, a table of field -> spec where spec is
//   "number" | "integer" | "string" | "boolean" | "vector" | "handle" |
//   "array" | "table" | "any", optionally suffixed "?" for optional, or
//   { type = "<name>[?]", min = n, max = n, fields = {...} }, or a nested table
//   of fields (shorthand for a required "table" with those fields).
// The reserved key `_extra = true` allows unlisted fields.
[[nodiscard]] bool parse_payload_schema(const Value &spec, PayloadSchema &schema,
                                        std::string &error);
[[nodiscard]] bool validate_payload(const PayloadSchema &schema, const Value &payload,
                                    std::string &error);

// ---------------------------------------------------------------------------
// Replicated state
// ---------------------------------------------------------------------------

enum class StoreScope : std::uint8_t
{
    world = 0,  // one store per session; every player receives it
    player = 1, // one per player; only that player receives it (unless public)
    entity = 2  // one per entity handle; every interested player receives it
};

struct StoreRef final
{
    StoreScope scope{StoreScope::world};
    Handle owner; // invalid for world; HandleKind::player for player stores

    [[nodiscard]] static StoreRef world() noexcept
    {
        return {};
    }
    [[nodiscard]] static StoreRef player(Handle player) noexcept
    {
        return {StoreScope::player, player};
    }
    [[nodiscard]] static StoreRef entity(Handle entity) noexcept
    {
        return {StoreScope::entity, entity};
    }
    [[nodiscard]] bool valid() const noexcept;
    auto operator<=>(const StoreRef &) const = default;
};

// Higher priorities are sent first when a peer's replication budget is short;
// the rest stay pending (coalesced to their latest value) for the next tick.
enum class ReplicationPriority : std::uint8_t
{
    low = 0,
    normal = 1,
    high = 2,
    critical = 3
};

struct ReplicatedKeyOptions final
{
    // Player-store keys only: also send this key to every other player.
    bool is_public{};
    ReplicationPriority priority{ReplicationPriority::normal};
};

// One change seen by a server or client listener. `old_value` is nil for a
// new key, `value` is nil for a removed one.
struct StateChange final
{
    StoreRef store;
    std::string_view mod_id;
    std::string_view key;
    const Value &value;
    const Value &old_value;
};
using StateListener = std::function<void(const StateChange &)>;
using SubscriptionId = std::uint64_t;

// Which changes a listener sees. Unset members match everything.
struct StateFilter final
{
    std::optional<StoreScope> scope;
    Handle owner;                      // one store of `scope` when valid
    std::optional<std::string> mod_id; // "" is the engine namespace
    std::optional<std::string> key;

    [[nodiscard]] bool matches(const StoreRef &store, std::string_view mod,
                               std::string_view name) const noexcept;
};

// ---------------------------------------------------------------------------
// Session content registry: a generic, generation-numbered table the host
// pushes to every peer. (Integration) Gameplay's blocks, items and creatures
// do NOT travel here: their full definitions are the gameplay "registry"
// message (modding/gameplay_net.hpp). Nothing in the game sets one today.
// ---------------------------------------------------------------------------

struct ContentRegistryEntry final
{
    std::string id;          // "dungeons:mossy_brick"
    std::uint32_t numeric{}; // session-local id
    auto operator<=>(const ContentRegistryEntry &) const = default;
};

struct ContentRegistryTable final
{
    std::string category; // "block", "item", "entity", ...
    std::vector<ContentRegistryEntry> entries;
    auto operator<=>(const ContentRegistryTable &) const = default;
};

// Sent by the host to every peer after its mod channel opens, and again (with
// a higher generation) whenever set_content_registry() changes it.
struct ContentRegistry final
{
    std::uint32_t generation{};
    std::vector<ContentRegistryTable> tables;
    // Anything else Gameplay needs on the client (bounded like an RPC payload).
    Value extra;
    friend bool operator==(const ContentRegistry &, const ContentRegistry &) = default;
};

inline constexpr std::size_t maximum_registry_tables = 64U;
inline constexpr std::size_t maximum_registry_entries = 65536U;
inline constexpr std::size_t maximum_registry_id_bytes = 128U;

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

struct ModNetLimits final
{
    // Client -> server, enforced by the host per peer before decoding.
    double client_frames_per_second{120.0};
    double client_frame_burst{240.0};
    double client_bytes_per_second{64.0 * 1024.0};
    double client_byte_burst{128.0 * 1024.0};
    double client_messages_per_second{60.0};
    double client_message_burst{120.0};
    std::uint32_t client_message_bytes{16U * 1024U};
    ValueLimits client_values{8U, 1024U, 4U * 1024U, 64U, 16U * 1024U};
    // Server -> client (the host is trusted with more: full-state messages
    // such as Gameplay's registry and snapshot are large).
    std::uint32_t server_message_bytes{1024U * 1024U + 1024U};
    ValueLimits server_values{24U, 65536U, 64U * 1024U, 128U, 1024U * 1024U};
    // Replicated-state bandwidth per peer (RPC traffic is not counted here).
    double replication_bytes_per_second{48.0 * 1024.0};
    double replication_burst_bytes{16.0 * 1024.0};
    // Messages queued for a peer whose channel is not ready yet.
    std::size_t pending_messages{256U};
    std::size_t pending_bytes{1024U * 1024U};
    // A partly received fragmented message is dropped after this long.
    double reassembly_timeout_seconds{10.0};
    // Dropped frames/messages from one peer before it is reported for removal.
    std::uint32_t violation_limit{32U};
};

// ---------------------------------------------------------------------------
// Transport-facing types
// ---------------------------------------------------------------------------

// One extension frame for the app to put on the wire (or the loopback).
struct ModNetSend final
{
    net::PeerId peer{};
    std::vector<std::uint8_t> frame;
};

struct ModNetCounters final
{
    std::uint64_t frames_sent{};
    std::uint64_t frames_received{};
    std::uint64_t bytes_sent{};
    std::uint64_t bytes_received{};
    std::uint64_t messages_sent{};
    std::uint64_t messages_received{};
    std::uint64_t rpc_dropped{};       // no handler, schema mismatch
    std::uint64_t frames_rejected{};   // rate, CRC, framing, reassembly
    std::uint64_t messages_rejected{}; // decode, wrong direction, unknown mod
    std::uint64_t replication_bytes{};
};

// Something the app (or the script bridge) must act on.
enum class ModNetEventKind : std::uint8_t
{
    // Server: a peer's client opened its channel; its snapshot is on its way.
    player_ready,
    // Server: a peer left (remove_peer).
    player_left,
    // Server: a peer exceeded its violation limit; the app should kick it.
    // Nothing from it is processed any more.
    peer_misbehaving,
    // Client: the host's welcome arrived (local_player() is valid).
    welcome,
    // Client: the initial replicated snapshot is complete.
    ready,
    // Client: a new content registry arrived (content_registry()).
    registry
};

struct ModNetEvent final
{
    ModNetEventKind kind{ModNetEventKind::player_ready};
    net::PeerId peer{};
    Handle player;
    std::string detail;
};

using RpcServerHandler = std::function<void(Handle player, const Value &payload)>;
using RpcClientHandler = std::function<void(const Value &payload)>;
using ModNetEventListener = std::function<void(const ModNetEvent &)>;
// Dropped payloads, violations and handler failures. `mod_id` is "" for
// channel-level problems.
using ModNetLogSink =
    std::function<void(std::string_view mod_id, LogLevel level, std::string_view message)>;

// Most events the app has not taken yet; older ones are discarded.
inline constexpr std::size_t maximum_queued_mod_net_events = 256U;
// Encoded size of one replicated value.
inline constexpr std::size_t maximum_replicated_value_bytes = 16U * 1024U;
// Replicated entries a client holds, all stores together. A host that tries
// to exceed it is ignored for the excess (and logged).
inline constexpr std::size_t maximum_client_state_entries = 65536U;

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

class ModNetServer final
{
  public:
    // `manifest` is the session's negotiated manifest. Its server/core mods,
    // in order, are the slots messages are namespaced by.
    explicit ModNetServer(const net::ModManifest &manifest, ModNetLimits limits = {});
    ~ModNetServer();
    ModNetServer(const ModNetServer &) = delete;
    ModNetServer &operator=(const ModNetServer &) = delete;

    // False when the manifest has no server/core mod: every call is then a
    // no-op and nothing is ever sent.
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] const ModNetLimits &limits() const noexcept;

    // --- Transport (app) --------------------------------------------------

    // A peer that completed the extension hello and is admitted, with the
    // player handle the game uses for it. Sends the welcome and the content
    // registry; replication and RPC start when its client reports ready.
    bool add_peer(net::PeerId peer, Handle player, double now);
    void remove_peer(net::PeerId peer);
    // A frame from `sender`, exactly as the transport delivered it.
    void receive(net::PeerId sender, std::span<const std::uint8_t> frame, double now);
    // Once per server tick, after scripts ran: flushes replication within each
    // peer's budget and expires stale reassembly.
    void update(double now);
    [[nodiscard]] std::vector<ModNetSend> take_sends();
    // Every event is queued here for the app (kicks) AND handed to each
    // listener as it happens (the script bridge).
    [[nodiscard]] std::vector<ModNetEvent> take_events();
    void add_event_listener(ModNetEventListener listener);
    void set_log_sink(ModNetLogSink sink);

    [[nodiscard]] bool has_peer(net::PeerId peer) const noexcept;
    [[nodiscard]] bool peer_ready(net::PeerId peer) const noexcept;
    [[nodiscard]] std::optional<Handle> player_of(net::PeerId peer) const noexcept;
    [[nodiscard]] std::optional<net::PeerId> peer_of(Handle player) const noexcept;
    // Players whose channel is ready, in join order.
    [[nodiscard]] std::vector<Handle> ready_players() const;
    [[nodiscard]] std::uint32_t violations(net::PeerId peer) const noexcept;
    [[nodiscard]] const ModNetCounters &counters() const noexcept;
    // Replication bytes still queued for a peer (budget-limited backlog).
    [[nodiscard]] std::size_t pending_replication(net::PeerId peer) const noexcept;

    // --- RPC ---------------------------------------------------------------

    // Handler for client messages `name` of mod `mod_id` (replaces any
    // previous one). `schema` drops non-matching payloads before the handler.
    // Throws ScriptError for an unknown mod or a bad name.
    void on(std::string_view mod_id, std::string_view name, RpcServerHandler handler,
            std::optional<PayloadSchema> schema = std::nullopt);
    void off(std::string_view mod_id, std::string_view name);
    // Returns false when the player has no channel. Throws ScriptError for an
    // unknown mod, a bad name or an unencodable payload.
    bool send_to(Handle player, std::string_view mod_id, std::string_view name,
                 const Value &payload);
    // To every ready player. Returns how many were addressed.
    std::size_t broadcast(std::string_view mod_id, std::string_view name, const Value &payload);

    // --- Replicated state --------------------------------------------------

    // Sets (or with a nil value removes) one key; changes reach clients at the
    // next update(). `options` are sticky for the key once given. Throws
    // ScriptError for an invalid store/key/value.
    void set(const StoreRef &store, std::string_view mod_id, std::string_view key, Value value,
             std::optional<ReplicatedKeyOptions> options = std::nullopt);
    [[nodiscard]] const Value *get(const StoreRef &store, std::string_view mod_id,
                                   std::string_view key) const;
    // Every key of `mod_id` in the store.
    [[nodiscard]] Map all(const StoreRef &store, std::string_view mod_id) const;
    // Removes every key of every mod (an entity despawned, a player left for
    // good). Clients drop their copy.
    void drop_store(const StoreRef &store);
    // Removes every key of one mod from the store.
    void clear(const StoreRef &store, std::string_view mod_id);
    // Stores that currently hold at least one key.
    [[nodiscard]] std::vector<StoreRef> stores(StoreScope scope) const;
    // Called for each change made through set()/clear()/drop_store().
    SubscriptionId subscribe(StateFilter filter, StateListener listener);
    void unsubscribe(SubscriptionId id) noexcept;
    // Entity stores go to every ready player unless this says otherwise. It is
    // evaluated each update(); a store that becomes interesting is sent whole,
    // one that stops being interesting is dropped on that client.
    void set_entity_interest(std::function<bool(Handle player, Handle entity)> interest);

    // --- Content registry -------------------------------------------------

    void set_content_registry(ContentRegistry registry);
    [[nodiscard]] const ContentRegistry &content_registry() const noexcept;

    // Mod id <-> slot (for tests and diagnostics).
    [[nodiscard]] std::optional<std::uint8_t> slot_of(std::string_view mod_id) const noexcept;

    class Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

class ModNetClient final
{
  public:
    explicit ModNetClient(const net::ModManifest &manifest, ModNetLimits limits = {});
    ~ModNetClient();
    ModNetClient(const ModNetClient &) = delete;
    ModNetClient &operator=(const ModNetClient &) = delete;

    [[nodiscard]] bool enabled() const noexcept;

    // --- Transport (app) --------------------------------------------------

    // A frame from the host. The app only calls this for frames from the
    // session's host (the transport identifies it).
    void receive(std::span<const std::uint8_t> frame, double now);
    // Once per frame: paces queued client messages to the host's limits.
    void update(double now);
    // Tells the host this client's VM is up; replication and RPC start.
    // Messages that arrived before are delivered by this call, in order.
    void set_ready(double now);
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> take_sends();
    [[nodiscard]] std::vector<ModNetEvent> take_events();
    void add_event_listener(ModNetEventListener listener);
    void set_log_sink(ModNetLogSink sink);

    [[nodiscard]] bool welcomed() const noexcept;
    // The initial snapshot arrived.
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Handle local_player() const noexcept;
    [[nodiscard]] const ModNetCounters &counters() const noexcept;
    // Bytes waiting to be paced out to the host.
    [[nodiscard]] std::size_t queued_bytes() const noexcept;

    // --- RPC ---------------------------------------------------------------

    void on(std::string_view mod_id, std::string_view name, RpcClientHandler handler,
            std::optional<PayloadSchema> schema = std::nullopt);
    void off(std::string_view mod_id, std::string_view name);
    // Queues a message to the host. Returns false when the outgoing queue is
    // full (the script is sending faster than the host accepts). Throws
    // ScriptError for an unknown mod, a bad name or an unencodable payload.
    bool send(std::string_view mod_id, std::string_view name, const Value &payload);

    // --- Replicated state (read-only) --------------------------------------

    [[nodiscard]] const Value *get(const StoreRef &store, std::string_view mod_id,
                                   std::string_view key) const;
    [[nodiscard]] Map all(const StoreRef &store, std::string_view mod_id) const;
    // Fired after a whole state message is applied (and for every key of a
    // dropped store, with a nil value).
    SubscriptionId subscribe(StateFilter filter, StateListener listener);
    void unsubscribe(SubscriptionId id) noexcept;
    // Stores of a scope this client currently holds (e.g. every visible player).
    [[nodiscard]] std::vector<StoreRef> stores(StoreScope scope) const;

    // --- Content registry -------------------------------------------------

    [[nodiscard]] const std::optional<ContentRegistry> &content_registry() const noexcept;

    class Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

// Offline / listen host: moves frames between a server and the in-process
// client (as peer `local_peer`) until neither has anything left to say, or
// `max_rounds` passes. Frames still go through the full encode/decode path.
// Server frames for any other peer go to `remote` (the live transport), or
// are dropped when it is empty. Call it after server.update().
void pump_mod_net_loopback(ModNetServer &server, ModNetClient &client, net::PeerId local_peer,
                           double now, const std::function<void(ModNetSend &&)> &remote = {},
                           std::size_t max_rounds = 8U);

// The PeerId an in-process client uses on a listen host / offline. Never a
// valid Steam ID.
inline constexpr net::PeerId local_mod_peer = 1U;

// ---------------------------------------------------------------------------
// Script bindings
// ---------------------------------------------------------------------------

// The app's view of the session for the net/replicated libraries. Either
// object may be null (no authority here / no client VM). Both must already
// exist for the session when register_net_api is called, and outlive the VMs.
class NetBackend
{
  public:
    virtual ~NetBackend() = default;
    [[nodiscard]] virtual ModNetServer *server() noexcept = 0;
    [[nodiscard]] virtual ModNetClient *client() noexcept = 0;
};

// Registers the `net` and `replicated` libraries and their events:
//   net.send(name, payload?)              client -> host
//   net.send_to(player, name, payload?)   host -> one player
//   net.broadcast(name, payload?)         host -> every ready player
//   net.on(name, fn, schema?)             host: fn(player, payload);
//                                         client: fn(payload)
//   net.off(name)
//   net.players()                         host: ready players
//   net.local_player()                    client: this player's handle
//   net.is_ready()                        client: snapshot complete
//   replicated.set(scope, key, value, opts?)   host only; opts {public, priority}
//   replicated.get(scope, key)
//   replicated.all(scope)
//   replicated.clear(scope)
//   replicated.on_change(scope, key?, fn(value, old, key, owner)) -> id
//   replicated.off(id)
//   scope = "world" | a player handle | an entity handle; on_change also
//   accepts "player" / "entity" for every store of that kind.
//   events: net.player_ready {player}, net.player_left {player} (host),
//           net.ready {player} (client).
// The registry outlives sessions: the functions look the session objects up
// through `backend` on every call.
void register_net_api(ApiRegistry &registry, ScriptHost &host, NetBackend &backend);

// Per session, when the app creates its ModNetServer / ModNetClient: routes
// their events to scripts (net.player_ready, net.player_left, net.ready) and
// their log lines to the runtime's log. Either may be null.
void connect_net_events(ScriptHost &host, ModNetServer *server, ModNetClient *client);

} // namespace openminerz::modding
