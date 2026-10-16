#pragma once

// Internals of ModRuntime shared by the runtime's source files. Nothing outside
// src/modding/runtime/ includes this.

#include "openminerz/modding/runtime.hpp"

#include "lua.h"
#include "lualib.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::modding
{

struct Vm;

// The heap of one VM. The cap is enforced while scripts or the runtime's
// protected work run; the few pushes made outside protection may overshoot it
// by a handful of bytes rather than raise an error nothing could catch.
struct VmAllocator final
{
    std::size_t used{};
    std::size_t limit{};
    std::size_t peak{};
    bool enforcing{};
};

// One registry function bound into one mod's environment: the upvalue of the
// C closure a script calls.
struct NativeBinding final
{
    Vm *vm{};
    std::uint32_t mod{};
    const ApiRegistry::Function *function{};
    std::string name;
    // Non-empty when the mod may not call it (wrong side or capability): the
    // error a call raises instead of reaching the service.
    std::string denied;
};

// A script function held for C++ (Callback::ref is the key).
struct Hold final
{
    int ref{};
    std::uint32_t mod{};
};

struct Subscription final
{
    std::uint32_t mod{};
    Callback callback;
    bool once{};
    bool active{true};
};

struct Timer final
{
    std::uint64_t id{};
    std::uint32_t mod{};
    Callback callback;
    double due{};
    double interval{}; // 0 for timer.after
};

// One mod inside one VM.
struct ModSlot final
{
    ModSource source;
    std::string key; // lowercase id
    // Script sources by normalized lowercase path.
    std::map<std::string, std::string, std::less<>> files;
    CapabilityMask capabilities{};
    std::uint32_t index{};
    int memory_category{};
    int env_ref{LUA_NOREF};       // the readonly environment with its libraries
    int file_meta_ref{LUA_NOREF}; // {__index = env}, shared by its file envs
    int modules_ref{LUA_NOREF};   // require cache: path -> module value
    int console_ref{LUA_NOREF};   // globals shared by run() chunks (console, tests)
    ModState state{ModState::running};
    std::string error;
    std::uint32_t errors{};
    std::uint64_t random_state{};
    // (Polish) When its recent calls overran their budget, oldest first.
    std::deque<std::chrono::steady_clock::time_point> overruns;
};

struct Vm final
{
    std::uint32_t id{};
    Side side{Side::client};
    ModRuntime::Impl *runtime{};
    lua_State *L{};
    VmAllocator allocator;
    SessionInfo session;
    std::vector<std::unique_ptr<ModSlot>> mods;
    std::unordered_map<std::string, std::uint32_t> mod_lookup; // lowercase id -> index
    std::deque<NativeBinding> bindings;
    std::unordered_map<std::uint32_t, Hold> holds;
    std::uint32_t next_hold{1U};
    std::map<std::string, std::vector<std::shared_ptr<Subscription>>, std::less<>> subscriptions;
    std::vector<Timer> timers;
    double clock{};
    std::uint64_t ticks{};
    int entry_ref{LUA_NOREF};        // the C function protected work runs in
    int traceback_ref{LUA_NOREF};    // the error handler of calls into mods
    int handle_cache_ref{LUA_NOREF}; // weak table: handle key -> userdata

    // The call in progress. depth counts script entries from C++; the budget
    // is armed by the outermost one and shared by nested ones.
    int depth{};
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::milliseconds budget{};
    bool timed_out{};
    std::uint32_t interrupts{};
    std::optional<std::uint32_t> timeout_mod;
    // (Polish) What moved the deadline on: time inside game functions, and
    // time the thread did not run (call_start/cpu_start measure that).
    std::chrono::steady_clock::time_point call_start{};
    std::chrono::nanoseconds cpu_start{};
    std::chrono::nanoseconds native_time{};
    std::chrono::nanoseconds stalled_time{};
    std::string timeout_where; // "mod/file.luau:12" where the overrun was caught
    std::optional<std::uint32_t> active_mod;
    lua_State *active_thread{}; // the thread running a native call, if any
    Handle acting_player;
    std::string last_error; // of the latest failed call or protected work
};

inline constexpr int handle_userdata_tag = 1;
inline constexpr int maximum_value_depth = 64;

// A storage scope of one mod: world, one player, or this machine.
struct StorageScope final
{
    struct Entry final
    {
        Value value;
        std::size_t bytes{};
    };
    std::map<std::string, Entry, std::less<>> entries;
    std::size_t bytes{};
    bool loaded{};
    bool dirty{};
    std::filesystem::path file; // "" keeps it in memory
};

struct ConfigEntry final
{
    ConfigSetting setting;
    bool user_set{};
};

class ModRuntime::Impl final
{
  public:
    Impl(ModRuntime &owner, RuntimeLimits limits);
    ~Impl();

    ModRuntime &owner;
    RuntimeLimits limits;
    ApiRegistry registry;
    std::function<void(const LogEntry &)> sink;
    std::deque<LogEntry> history;
    std::function<std::optional<std::string>(Handle)> player_key;
    std::vector<std::function<void(Side)>> teardown_listeners;
    std::unique_ptr<Vm> vms[2];
    std::optional<Value> world_payload[2];
    SessionInfo session;
    std::uint64_t next_timer{1U};

    // Storage and settings outlive VMs, so a developer reload keeps them.
    std::map<std::string, StorageScope, std::less<>> storage; // "<mod>|<scope>"
    std::map<std::string, std::vector<ConfigEntry>, std::less<>> config; // mod key
    std::map<std::string, bool, std::less<>> config_loaded;

    [[nodiscard]] Vm *vm_for(Side side) const noexcept
    {
        return vms[side == Side::server ? 1 : 0].get();
    }
    [[nodiscard]] Vm *vm_by_id(std::uint32_t id) const noexcept;

    // --- runtime.cpp ---
    bool start(Side side, std::vector<ModSource> mods, const SessionInfo &info);
    void stop(Side side);
    bool attach(Vm &vm, ModSource mod, bool run_scripts);
    void log(std::string_view mod_id, Side side, LogLevel level, std::string_view message);
    void mod_error(Vm &vm, std::uint32_t mod, std::string_view message);
    void stop_mod(Vm &vm, std::uint32_t mod, std::string_view reason);
    // Runs `work` inside lua_pcall on the VM's current thread; false with the
    // error logged against `mod` (or the runtime) when it raised.
    bool protect(Vm &vm, std::optional<std::uint32_t> mod,
                 const std::function<void(lua_State *)> &work);
    // Calls the function under nargs arguments on the stack as a script entry
    // of `mod`. On failure logs it and leaves `results` nils. `loading` runs
    // a file's top level under the load budget.
    bool call(Vm &vm, lua_State *L, std::uint32_t mod, int nargs, int results,
              bool loading = false);
    // (Polish) The outermost call that overran has unwound: warn, or stop
    // the mod (see RuntimeLimits::overrun_strikes).
    void overran(Vm &vm, std::uint32_t mod, bool loading);
    bool emit(Side side, std::string_view event, const Value &payload);
    Value invoke(const Callback &callback, std::span<const Value> args, Handle acting_player);
    void release(const Callback &callback) noexcept;
    RunResult run(Side side, std::string_view mod_id, std::string_view chunk_name,
                  std::string_view source);
    void advance_timers(Vm &vm, double seconds);

    // --- marshal ---
    static void push_value(Vm &vm, lua_State *L, const Value &value, int depth = 0);
    static Value read_value(Vm &vm, lua_State *L, int index, std::uint32_t owner, int depth = 0);
    static void push_handle(Vm &vm, lua_State *L, Handle handle);
    static Callback hold_function(Vm &vm, lua_State *L, int index, std::uint32_t owner);
    [[nodiscard]] static bool push_callback(Vm &vm, lua_State *L, const Callback &callback);

    // --- core_api.cpp ---
    void register_core_api();
    [[nodiscard]] ModSlot *calling_mod(const CallContext &context, Vm **vm = nullptr) const;
    void remove_mod_handlers(Vm &vm, std::uint32_t mod);
    StorageScope &storage_scope(std::string_view mod_key, std::string_view scope,
                                const std::filesystem::path &file);
    void persist_storage();
    void load_config(std::string_view mod_key);
    void save_config(std::string_view mod_key);
};

// Shared helpers.
[[nodiscard]] std::string lowercase_ascii(std::string_view text);
// Normalizes a mod-relative path: lowercase, '/' separated, no "." or "..",
// no empty segments. False when it would leave the mod.
[[nodiscard]] bool normalize_mod_path(std::string_view path, std::string &normalized);

} // namespace openminerz::modding
