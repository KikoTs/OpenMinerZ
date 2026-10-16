#pragma once

// The OpenMinerZ modding contract.
//
// Every scriptable feature (UI, gameplay, networking, graphics, ...) is a C++
// service that registers named functions and events here. The script runtime
// (Luau, src/modding/runtime) marshals between scripts and these registrations,
// so no service ever touches a Luau type, every service can be unit-tested from
// C++ alone, network RPC arguments are plain Values, and the SDK reference and
// editor type definitions are generated from the same registry.
//
// Read docs/modding/architecture.md before extending this file.

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace openminerz::modding
{

// Bumped only on breaking changes to the script-visible API. A mod declares the
// version it was written against; the runtime refuses newer ones.
inline constexpr std::uint32_t api_version = 1U;

// ---------------------------------------------------------------------------
// Where code runs
// ---------------------------------------------------------------------------

// A script VM runs on one side. In single-player and on a listen host BOTH
// VMs run in the same process, isolated from each other, and talk only through
// the mod network channel (loopback), so a mod behaves the same offline and
// online.
enum class Side : std::uint8_t
{
    client = 1U,
    server = 2U
};

using SideMask = std::uint8_t;
inline constexpr SideMask side_client = 1U;
inline constexpr SideMask side_server = 2U;
inline constexpr SideMask side_both = side_client | side_server;

[[nodiscard]] constexpr bool side_in(Side side, SideMask mask) noexcept
{
    return (static_cast<SideMask>(side) & mask) != 0U;
}

// What a function is allowed to touch. The runtime grants capabilities per VM
// from the mod's scope (net::ModScope) and side; a call without the required
// capability raises a script error and never reaches the service.
//
//  client-scope mod  (texture packs, UI themes, HUD tweaks; usable on ANY
//                     server, including vanilla CastleMiner Z hosts):
//                     client VM only; presentation | local_storage.
//  server/core mod   (gameplay; every player in the session must have it):
//                     server VM: everything except presentation.
//                     client VM: presentation | local_storage | network.
//                     Not loaded at all when joining a session whose
//                     negotiated manifest does not contain it (e.g. vanilla).
using CapabilityMask = std::uint32_t;
namespace capability
{
inline constexpr CapabilityMask none = 0U;
// Local visuals and audio: UI, HUD, themes, VFX, lights, sounds, camera
// effects. Never changes simulation state or anything another peer sees.
inline constexpr CapabilityMask presentation = 1U << 0U;
// Authoritative game state: world edits, entities, inventories, stats,
// damage, registries. Server VM only.
inline constexpr CapabilityMask gameplay = 1U << 1U;
// Mod network channel: RPC, replicated state.
inline constexpr CapabilityMask network = 1U << 2U;
// Persistent per-world / per-player mod data (server) .
inline constexpr CapabilityMask world_storage = 1U << 3U;
// Per-machine mod settings and caches (client).
inline constexpr CapabilityMask local_storage = 1U << 4U;
// Read-only queries of the shared simulation (block at, time of day, player
// positions). Safe on both sides.
inline constexpr CapabilityMask query = 1U << 5U;
} // namespace capability

// ---------------------------------------------------------------------------
// Values crossing the script boundary
// ---------------------------------------------------------------------------

struct Vec3 final
{
    double x{};
    double y{};
    double z{};
    auto operator<=>(const Vec3 &) const = default;
};

// Opaque reference to an engine object (entity, widget, light, timer, ...).
// `kind` is a HandleKind; `id` is never reused within a session. Handles that
// name replicated objects are identical on host and clients.
enum class HandleKind : std::uint32_t
{
    none = 0U,
    player = 1U,
    entity = 2U,
    widget = 3U,
    light = 4U,
    effect = 5U,
    sound = 6U,
    timer = 7U,
    quest = 8U,
    // Services may add kinds at 100+ without editing this list.
    user_base = 100U
};

struct Handle final
{
    HandleKind kind{HandleKind::none};
    std::uint64_t id{};
    auto operator<=>(const Handle &) const = default;
    [[nodiscard]] bool valid() const noexcept
    {
        return kind != HandleKind::none;
    }
};

// A script function held by C++ (event handler, button click, timer). Only
// meaningful inside the VM that created it: never serialized, never sent over
// the network. Release it through ScriptHost::release when done.
struct Callback final
{
    std::uint32_t vm{};
    std::uint32_t ref{};
    auto operator<=>(const Callback &) const = default;
    [[nodiscard]] bool valid() const noexcept
    {
        return ref != 0U;
    }
};

enum class ValueType : std::uint8_t
{
    nil,
    boolean,
    integer,
    number,
    string,
    vec3,
    handle,
    callback,
    array,
    map,
    any // only in specs: accepts every type
};

class Value;
using Array = std::vector<Value>;
// Ordered by insertion: serialization and iteration stay deterministic.
using Map = std::vector<std::pair<std::string, Value>>;

class Value final
{
  public:
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string, Vec3,
                                 Handle, Callback, std::shared_ptr<Array>, std::shared_ptr<Map>>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool value) : storage_{value} {}
    Value(int value) : storage_{static_cast<std::int64_t>(value)} {}
    Value(std::int64_t value) : storage_{value} {}
    Value(std::uint32_t value) : storage_{static_cast<std::int64_t>(value)} {}
    Value(double value) : storage_{value} {}
    Value(float value) : storage_{static_cast<double>(value)} {}
    Value(std::string value) : storage_{std::move(value)} {}
    Value(std::string_view value) : storage_{std::string{value}} {}
    Value(const char *value) : storage_{std::string{value}} {}
    Value(Vec3 value) : storage_{value} {}
    Value(Handle value) : storage_{value} {}
    Value(Callback value) : storage_{value} {}
    Value(Array value) : storage_{std::make_shared<Array>(std::move(value))} {}
    Value(Map value) : storage_{std::make_shared<Map>(std::move(value))} {}

    [[nodiscard]] ValueType type() const noexcept
    {
        return static_cast<ValueType>(storage_.index());
    }
    [[nodiscard]] bool is_nil() const noexcept
    {
        return storage_.index() == 0U;
    }
    [[nodiscard]] bool is_number() const noexcept
    {
        return type() == ValueType::integer || type() == ValueType::number;
    }

    // Typed reads. A mismatch throws ScriptError with a readable message, so a
    // service can read arguments without checking each type first.
    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] std::int64_t as_integer() const; // numbers must be whole
    [[nodiscard]] double as_number() const;        // integers widen
    [[nodiscard]] const std::string &as_string() const;
    [[nodiscard]] Vec3 as_vec3() const;
    [[nodiscard]] Handle as_handle() const;
    [[nodiscard]] Callback as_callback() const;
    [[nodiscard]] const Array &as_array() const;
    [[nodiscard]] const Map &as_map() const;

    // Map field lookup; nullptr when this is not a map or the key is absent.
    [[nodiscard]] const Value *field(std::string_view key) const noexcept;
    // Field with a fallback when absent or nil.
    [[nodiscard]] Value field_or(std::string_view key, Value fallback) const;

    [[nodiscard]] const Storage &storage() const noexcept
    {
        return storage_;
    }

    friend bool operator==(const Value &left, const Value &right);

  private:
    Storage storage_;
};

[[nodiscard]] const char *value_type_name(ValueType type) noexcept;

// Thrown by services for bad arguments or refused actions; the runtime turns
// it into a script error carrying the message and the calling script's line.
class ScriptError final : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

struct ParamSpec final
{
    std::string name;
    ValueType type{ValueType::any};
    bool optional{};
    std::string doc;
};

// Who is calling, handed to every native function.
struct CallContext final
{
    std::string_view mod_id;
    Side side{Side::client};
    CapabilityMask capabilities{};
    // The player a server-side call acts for when it runs inside a client RPC
    // handler; invalid otherwise.
    Handle acting_player;
};

using NativeFunction = std::function<Value(CallContext &, std::span<const Value>)>;

struct FunctionSpec final
{
    // Dotted script name: "ui.toast", "world.set_block", "net.send".
    // The first segment is the library table the runtime creates.
    std::string name;
    std::string doc;
    std::vector<ParamSpec> params;
    ValueType returns{ValueType::nil};
    std::string returns_doc;
    SideMask sides{side_both};
    CapabilityMask requires_caps{capability::none};
    // Variadic functions accept extra trailing arguments of any type.
    bool variadic{};
};

struct EventSpec final
{
    // Dotted name: "player.joined", "block.breaking".
    std::string name;
    std::string doc;
    SideMask sides{side_both};
    // A cancellable event stops when any handler returns false, and emit()
    // reports the cancellation back to the engine (e.g. refuse a block break).
    bool cancellable{};
    // Fields of the payload map, for documentation and type definitions.
    std::vector<ParamSpec> payload;
};

struct ConstantSpec final
{
    std::string name; // "blocks.STONE"
    Value value;
    std::string doc;
    SideMask sides{side_both};
};

// The catalogue every service registers into before scripts load. The runtime
// builds each VM's library tables from it, filtering by side; the SDK
// generator writes the API reference and Luau type definitions from it.
class ApiRegistry final
{
  public:
    void add_function(FunctionSpec spec, NativeFunction function)
    {
        const std::string key = spec.name;
        functions_.insert_or_assign(key, Function{std::move(spec), std::move(function)});
    }
    void add_event(EventSpec spec)
    {
        const std::string key = spec.name;
        events_.insert_or_assign(key, std::move(spec));
    }
    void add_constant(ConstantSpec spec)
    {
        const std::string key = spec.name;
        constants_.insert_or_assign(key, std::move(spec));
    }

    struct Function final
    {
        FunctionSpec spec;
        NativeFunction call;
    };

    [[nodiscard]] const std::map<std::string, Function, std::less<>> &functions() const noexcept
    {
        return functions_;
    }
    [[nodiscard]] const std::map<std::string, EventSpec, std::less<>> &events() const noexcept
    {
        return events_;
    }
    [[nodiscard]] const std::map<std::string, ConstantSpec, std::less<>> &constants() const noexcept
    {
        return constants_;
    }
    [[nodiscard]] const Function *find_function(std::string_view name) const
    {
        const auto found = functions_.find(name);
        return found == functions_.end() ? nullptr : &found->second;
    }
    [[nodiscard]] const EventSpec *find_event(std::string_view name) const
    {
        const auto found = events_.find(name);
        return found == events_.end() ? nullptr : &found->second;
    }

  private:
    std::map<std::string, Function, std::less<>> functions_;
    std::map<std::string, EventSpec, std::less<>> events_;
    std::map<std::string, ConstantSpec, std::less<>> constants_;
};

// ---------------------------------------------------------------------------
// Calling back into scripts
// ---------------------------------------------------------------------------

enum class LogLevel : std::uint8_t
{
    debug,
    info,
    warning,
    error
};

// Implemented by the runtime; services keep a reference to call scripts.
// Every entry point is main-thread only and safe to call when no VM for the
// side is running (it then does nothing and emit() returns true).
class ScriptHost
{
  public:
    virtual ~ScriptHost() = default;

    // Delivers a registered event to every handler on that side, in mod load
    // order. Returns false when a cancellable event was cancelled. A handler
    // that errors is logged and skipped; it never cancels.
    virtual bool emit(Side side, std::string_view event, const Value &payload) = 0;

    // Calls one script function. Errors are logged against the owning mod and
    // yield nil.
    virtual Value invoke(const Callback &callback, std::span<const Value> args) = 0;

    // Drops C++'s hold on a callback. Releasing twice or after the VM has been
    // torn down is harmless.
    virtual void release(const Callback &callback) noexcept = 0;

    // The mod that owns a callback ("" when the VM is gone).
    [[nodiscard]] virtual std::string_view owner_of(const Callback &callback) const noexcept = 0;

    virtual void log(std::string_view mod_id, LogLevel level, std::string_view message) = 0;

    // (Runtime, additive) invoke() on behalf of a player: native functions the
    // script calls meanwhile see `acting_player` in their CallContext. The mod
    // channel uses it to run a client RPC's server handler.
    virtual Value invoke_as(Handle acting_player, const Callback &callback,
                            std::span<const Value> args)
    {
        (void)acting_player;
        return invoke(callback, args);
    }

    // (Runtime, additive) Called just before a side's VM is torn down (session
    // end or developer reload), after world.unloading ran: a service drops the
    // widgets, lights, effects and callbacks that side's scripts created.
    virtual void add_teardown_listener(std::function<void(Side)> listener)
    {
        (void)listener;
    }
};

// Every service module exposes one entry point of this shape, declared in its
// own header (e.g. register_ui_api in modding/ui.hpp):
//
//   void register_<module>_api(ApiRegistry &registry, ScriptHost &host,
//                              <Module>Backend &backend);
//
// `Backend` is the module's narrow interface onto the game, implemented by the
// app (src/app_mod_<module>.inc). Services never include app internals.

} // namespace openminerz::modding
