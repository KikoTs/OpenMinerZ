#pragma once

// The OpenMinerZ script runtime: Luau VMs, mod environments and lifecycle.
//
// One ModRuntime runs at most one VM per Side. Every mod loaded on a side gets
// its own environment inside that side's VM; its library tables (`ui`,
// `world`, `events`, ...) are built from the ApiRegistry, filtered by side and
// by the mod's capabilities, so services never see Luau and never check
// capabilities themselves.
//
// Value <-> Luau, as a mod author sees it:
//   nil, boolean, string     the same Luau types
//   integer, number          Luau numbers. A whole number in +-2^53 arrives in
//                            C++ as an integer, anything else as a number.
//   vec3                     a native Luau `vector` (vector.create(x, y, z),
//                            fields .x .y .z, + - * / work). Single precision,
//                            like the game's own XNA Vector3. A parameter
//                            declared vec3 also accepts a {x=, y=, z=} table.
//   handle                   an opaque, immutable userdata (typeof "Handle",
//                            fields .kind .id). The same handle is the same
//                            Luau value, so it works as a table key and ==.
//   callback                 a Luau function, held for C++ by reference
//   array                    a table with keys 1..n and nothing else
//   map                      a table with string keys (iterated sorted, so
//                            the C++ order is deterministic). An empty table
//                            arrives as an empty map; services may read it as
//                            an empty array too.
// Mixed or non-string-keyed tables, coroutines, buffers and nesting deeper than
// 64 levels are refused with a script error.
//
// Callbacks a native function receives as arguments are held for the service:
// keep them and release() them later, or release() them before returning when
// the function only calls them during the call. Everything still held is
// dropped when the VM is torn down; invoking or releasing a stale callback is
// harmless.
//
// Read docs/modding/architecture.md for sides, scopes and the lifecycle.

#include "openminerz/modding/api.hpp"
#include "openminerz/net/mod_handshake.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
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

// What a mod of `scope` may touch on `side`; capability::none when the mod does
// not run there at all (a client-scope mod on the server).
[[nodiscard]] CapabilityMask capabilities_for(net::ModScope scope, Side side) noexcept;
[[nodiscard]] constexpr bool runs_on(net::ModScope scope, Side side) noexcept
{
    return side == Side::client || scope != net::ModScope::client;
}
[[nodiscard]] const char *side_name(Side side) noexcept;
[[nodiscard]] const char *log_level_name(LogLevel level) noexcept;

struct RuntimeLimits final
{
    // Heap of one VM (all mods on that side together). Allocations past it
    // fail with a script "not enough memory" error; the game keeps running.
    std::size_t memory_bytes{64U * 1024U * 1024U};
    // Budget of one script file's top level while loading. A file that
    // overruns it did not load, so its mod is stopped at once.
    std::chrono::milliseconds load_budget{2000};
    // Budget of one call into a mod: an event handler, a timer, a callback
    // invoked by a service. Budgets count script time: time inside game
    // functions, and time the game's thread was not running at all (a
    // breakpoint, the process suspended, the OS busy elsewhere), does not
    // count. A call that overruns is aborted (pcall cannot catch that) and a
    // warning names the mod, file and line; the mod keeps running.
    std::chrono::milliseconds call_budget{100};
    // (Polish) A mod is stopped (handlers and timers dropped, error logged)
    // on its overrun_strikes-th overrun within overrun_window, or at once
    // when one call kept the game waiting hard_overrun or longer: a budget
    // that large, or a built-in function (a huge sort, a runaway string
    // pattern) the abort could not interrupt in time.
    std::uint32_t overrun_strikes{3U};
    std::chrono::milliseconds overrun_window{60'000};
    std::chrono::milliseconds hard_overrun{1000};
    // Serialized size of one mod's data in one storage scope (world, one
    // player, local machine).
    std::size_t storage_bytes{1024U * 1024U};
    // Entries kept in recent_log().
    std::size_t log_history{512U};
};

// A mod ready to run: what the runtime needs from a package (package.hpp
// loads one from disk; tests build it in memory).
struct ModSource final
{
    std::string id;
    std::string version{"1"};
    net::ModScope scope{net::ModScope::client};
    // Mods this one may require("@id/...") and that load before it.
    std::vector<std::string> dependencies;
    // Script files by mod-relative path ("server/main.luau"); only .luau and
    // .lua files matter. Keys are compared case-insensitively.
    std::map<std::string, std::string> files;
    // On-disk root, when the mod came from a package ("" in memory).
    std::filesystem::path root;
    // Tests and tools only: replaces the capabilities derived from scope.
    std::optional<CapabilityMask> capabilities;
};

// Where a session keeps mod data. Empty paths keep it in memory only.
struct SessionInfo final
{
    // The world's mod folder (the app uses "<world save>.mods", beside its
    // ".players"); server storage lives in <world_dir>/<mod_id>/.
    std::filesystem::path world_dir;
    // Per-machine data: client storage and mod settings, <local_dir>/<mod_id>/.
    std::filesystem::path local_dir;
    // Seeds each mod's util.random, so a world replays the same way.
    std::uint64_t world_seed{};
};

struct LogEntry final
{
    std::string mod_id; // "" for the runtime itself
    Side side{Side::client};
    LogLevel level{LogLevel::info};
    std::string message;
};

struct RunResult final
{
    bool ok{};
    Value value;       // the chunk's first return value
    std::string error; // "mod/file.luau:3: message" when !ok
};

enum class ModState : std::uint8_t
{
    running,
    failed, // a script failed to load; its handlers were dropped
    stopped // overran its budgets at runtime; its handlers were dropped
};

struct ModStatus final
{
    std::string id;
    Side side{Side::client};
    ModState state{ModState::running};
    std::string error;
    std::size_t memory_bytes{};
    std::uint32_t errors{};
    std::uint32_t handlers{};
    std::uint32_t timers{};
};

// One user-editable setting a mod declared with mod.config.define. The UI
// draws a settings page from these; set_config() changes a value.
struct ConfigSetting final
{
    std::string mod_id;
    std::string key;
    std::string label;
    std::string doc;
    ValueType type{ValueType::boolean}; // boolean, integer, number or string
    Value default_value;
    Value value;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> step;
    std::vector<std::string> choices; // a string setting picks one of these
};

class ModRuntime final : public ScriptHost
{
  public:
    explicit ModRuntime(RuntimeLimits limits = {});
    ~ModRuntime() override;
    ModRuntime(const ModRuntime &) = delete;
    ModRuntime &operator=(const ModRuntime &) = delete;

    // Services register here before a session starts. The runtime's own
    // libraries (events, log, timer, storage, mod, util) are already in it.
    [[nodiscard]] ApiRegistry &registry() noexcept;
    [[nodiscard]] const ApiRegistry &registry() const noexcept;

    // Every log line also goes here (the app prints it; tests may collect).
    void set_log_sink(std::function<void(const LogEntry &)> sink);
    [[nodiscard]] const std::deque<LogEntry> &recent_log() const noexcept;
    void clear_log() noexcept;

    // Persistent ids for storage.player: maps a player handle to a stable key
    // (gamertag/SteamID). Without it player storage is refused.
    void set_player_key(std::function<std::optional<std::string>(Handle)> resolver);

    // --- Session lifecycle (main thread) ---------------------------------

    // Loads `mods` (already in load order; dependencies are checked, not
    // reordered) into a new VM for `side`: shared/ then the side's folder,
    // then mod.loaded. Mods that do not run on the side are skipped; with none
    // left no VM is created at all. A mod whose scripts fail is marked failed
    // and the others keep running. Returns false only when no VM was started.
    bool start(Side side, std::vector<ModSource> mods, const SessionInfo &info);
    // world.loaded with `payload`; the app calls it once the world is ready.
    void world_loaded(Side side, const Value &payload);
    // world.unloading (when world.loaded ran), storage flush, teardown
    // listeners, VM closed. World and player storage stay cached until
    // stop_all or forget_session_storage.
    void stop(Side side);
    // (Integration) The world closed but this side's VM keeps running (the
    // client VM of client-scope mods lives from boot to exit when a session
    // adds no mod to it): world.unloading, then the storage flush. Nothing
    // for a side that did not see world.loaded.
    void world_unloaded(Side side);
    // (Integration) Saves, then forgets world and player storage, as stop_all
    // does, for a session that ends while a VM keeps running.
    void forget_session_storage();
    // The session is over: both sides stopped, then world and player storage
    // (already saved) forgotten, so the next world starts from its own files.
    void stop_all();
    // Developer hot reload: tears the side down and starts it again with
    // `mods`, keeping storage and settings.
    bool reload(Side side, std::vector<ModSource> mods);
    // Adds one more mod to a running VM (tests and tools; it loads last).
    bool attach(Side side, ModSource mod);
    [[nodiscard]] bool running(Side side) const noexcept;
    [[nodiscard]] bool any_running() const noexcept;

    // Server simulation step: due server timers, then `tick` {dt, tick}.
    void tick(double seconds);
    // Client frame: due client timers, then `frame` {dt}.
    void frame(double seconds);
    // Writes every dirty storage scope (the app calls it when the world saves).
    void persist();

    // Runs a chunk inside a mod's environment (tests, developer console).
    // Successive chunks of one mod share their globals, like a console; they
    // never see the globals of the mod's own files (use require for that).
    RunResult run(Side side, std::string_view mod_id, std::string_view chunk_name,
                  std::string_view source);

    [[nodiscard]] std::vector<ModStatus> status() const;
    [[nodiscard]] std::vector<ConfigSetting> config_settings() const;
    // Validates, stores and persists one setting, then emits config.changed on
    // both sides. Returns false (with `error`) for an unknown key or bad value.
    bool set_config(std::string_view mod_id, std::string_view key, const Value &value,
                    std::string &error);

    // --- ScriptHost -------------------------------------------------------

    bool emit(Side side, std::string_view event, const Value &payload) override;
    Value invoke(const Callback &callback, std::span<const Value> args) override;
    Value invoke_as(Handle acting_player, const Callback &callback,
                    std::span<const Value> args) override;
    void release(const Callback &callback) noexcept override;
    [[nodiscard]] std::string_view owner_of(const Callback &callback) const noexcept override;
    void log(std::string_view mod_id, LogLevel level, std::string_view message) override;
    void add_teardown_listener(std::function<void(Side)> listener) override;

    class Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

// util.serialize / util.deserialize, also the storage file format: compact
// JSON. Vectors are {"$vector":[x,y,z]}, handles {"$handle":[kind,id]};
// callbacks and non-finite numbers cannot be serialized.
[[nodiscard]] bool serialize_value(const Value &value, std::string &text, std::string &error);
[[nodiscard]] bool deserialize_value(std::string_view text, Value &value, std::string &error);

} // namespace openminerz::modding
