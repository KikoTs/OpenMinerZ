// The Luau side of ModRuntime: VMs, sandboxed mod environments, marshalling,
// require, budgets, and the ScriptHost entry points services call.
//
// Error handling: Luau is built with C++ exceptions (LUA_USE_LONGJMP 0), so a
// script error unwinds C++ frames normally. Every way into a VM from C++ goes
// through Impl::protect (a lua_pcall), and every native function a script
// calls catches service exceptions and re-raises them as script errors, so no
// C++ exception ever crosses a Luau frame and no Luau error escapes to the game.

#include "runtime_impl.hpp"

#include "luacode.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace openminerz::modding
{
namespace
{

std::atomic<std::uint32_t> next_vm_id{1U};

constexpr std::array<std::string_view, 6U> capability_names{
    "presentation", "gameplay", "network", "world_storage", "local_storage", "query"};

// Globals mods never see: no files, processes, clocks of the host, debug
// introspection, or environment swapping.
constexpr std::array<const char *, 9U> stripped_globals{
    "os", "debug", "getfenv", "setfenv", "loadstring", "dofile", "loadfile", "require", "print"};

void *allocate(void *user, void *block, std::size_t old_size, std::size_t new_size)
{
    auto &allocator = *static_cast<VmAllocator *>(user);
    if (new_size == 0U)
    {
        std::free(block);
        allocator.used -= old_size;
        return nullptr;
    }
    if (allocator.enforcing && new_size > old_size &&
        allocator.used - old_size + new_size > allocator.limit)
        return nullptr;
    void *resized = std::realloc(block, new_size);
    if (resized == nullptr)
        return nullptr;
    allocator.used = allocator.used - old_size + new_size;
    allocator.peak = std::max(allocator.peak, allocator.used);
    return resized;
}

std::string error_text(lua_State *L, int index)
{
    if (lua_type(L, index) == LUA_TSTRING)
    {
        std::size_t length{};
        const char *text = lua_tolstring(L, index, &length);
        return std::string{text, length};
    }
    return std::string{"(error object is a "} + luaL_typename(L, index) + " value)";
}

// Raises a script error at the running Luau line. Called outside any catch
// block so nothing of the C++ exception being handled is left alive.
[[noreturn]] void raise(lua_State *L, const std::string &message)
{
    lua_Debug where{};
    if (lua_getinfo(L, 1, "sl", &where) != 0 && where.currentline > 0)
        lua_pushfstring(L, "%s:%d: %s", where.short_src, where.currentline, message.c_str());
    else
        lua_pushlstring(L, message.data(), message.size());
    lua_error(L);
}

// (Polish) How long this thread has run on a CPU. Read only when a deadline
// has passed, to tell a script that used its budget from a thread that was
// not running: stopped at a breakpoint, the process suspended, or another
// program holding the CPU. Windows counts it in scheduler ticks (about
// 15.6 ms), close enough against a 100 ms budget. Zero when unknown; then the
// wall clock alone decides.
std::chrono::nanoseconds thread_cpu_time() noexcept
{
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) == 0)
        return {};
    const auto ticks = [](const FILETIME &time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    return std::chrono::nanoseconds{static_cast<std::int64_t>((ticks(kernel) + ticks(user)) * 100U)};
#else
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
        return {};
    return std::chrono::seconds{now.tv_sec} + std::chrono::nanoseconds{now.tv_nsec};
#endif
}

// (Polish) The deadline has passed. Time the thread did not run since the
// call began, beyond what game functions already moved the deadline by, is a
// stall, not script time: the deadline moves on by it. True when the script
// really used its budget.
bool budget_spent(Vm &vm, std::chrono::steady_clock::time_point now)
{
    const std::chrono::nanoseconds cpu = thread_cpu_time();
    if (cpu == std::chrono::nanoseconds{} || vm.cpu_start == std::chrono::nanoseconds{})
        return true;
    // Game functions may have run off the CPU too; counting all their time as
    // such keeps the estimate on the strict side.
    const std::chrono::nanoseconds off_cpu =
        (now - vm.call_start) - (cpu - vm.cpu_start) - vm.native_time;
    // Less than a millisecond is clock granularity, not a stall.
    if (off_cpu - vm.stalled_time < std::chrono::milliseconds{1})
        return true;
    vm.deadline += off_cpu - vm.stalled_time;
    vm.stalled_time = off_cpu;
    return now >= vm.deadline;
}

void interrupt(lua_State *L, int gc)
{
    if (gc >= 0)
        return;
    Vm *vm = static_cast<Vm *>(lua_callbacks(L)->userdata);
    if (vm == nullptr || vm->depth == 0)
        return;
    lua_Debug where{};
    if (!vm->timed_out)
    {
        // The clock is read every 256 safepoints; a tight loop costs nothing.
        if ((++vm->interrupts & 0xFFU) != 0U)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now < vm->deadline || !budget_spent(*vm, now))
            return;
        vm->timed_out = true;
        vm->timeout_mod = vm->active_mod;
        vm->timeout_where.clear();
        if (lua_getinfo(L, 0, "sl", &where) != 0 && where.currentline > 0)
            vm->timeout_where = std::format("{}:{}", where.short_src, where.currentline);
    }
    // (Polish) The call is aborted: raised at every safepoint until the
    // outermost call unwinds, so a script cannot pcall its way past the
    // budget. Whether the mod keeps running is decided after (Impl::overran).
    const std::string message = std::format("call aborted: it ran longer than its {} ms budget",
                                            vm->budget.count());
    if (lua_getinfo(L, 0, "sl", &where) != 0 && where.currentline > 0)
        lua_pushfstring(L, "%s:%d: %s", where.short_src, where.currentline, message.c_str());
    else
        lua_pushlstring(L, message.data(), message.size());
    lua_error(L);
}

// The error handler of every call into a mod: appends where the error came
// from, at most eight script frames deep.
int traceback_handler(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TSTRING)
        return 1;
    std::string text = lua_tostring(L, 1);
    lua_Debug frame{};
    int shown{};
    for (int level = 1; shown < 8 && lua_getinfo(L, level, "sln", &frame) != 0; ++level)
    {
        if (frame.what != nullptr && std::strcmp(frame.what, "C") == 0)
            continue;
        text += std::format("\n  at {}:{}", frame.short_src, frame.currentline);
        if (frame.name != nullptr)
            text += std::format(" ({})", frame.name);
        ++shown;
    }
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// The C function every Impl::protect runs in: argument 1 is the work.
int protected_entry(lua_State *L)
{
    const auto *work =
        static_cast<const std::function<void(lua_State *)> *>(lua_tolightuserdata(L, 1));
    lua_settop(L, 0);
    std::string error;
    try
    {
        (*work)(L);
        return 0;
    }
    catch (const std::runtime_error &exception)
    {
        error = exception.what();
    }
    catch (const std::logic_error &exception)
    {
        error = exception.what();
    }
    catch (const std::bad_alloc &)
    {
        error = "out of memory";
    }
    lua_pushlstring(L, error.data(), error.size());
    lua_error(L);
}

std::string capability_list(CapabilityMask mask)
{
    std::string list;
    for (std::size_t bit{}; bit < capability_names.size(); ++bit)
        if ((mask & (1U << bit)) != 0U)
        {
            if (!list.empty())
                list += ", ";
            list += capability_names[bit];
        }
    return list;
}

bool is_identifier(std::string_view name)
{
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) != 0)
        return false;
    return std::all_of(name.begin(), name.end(), [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    });
}

std::vector<std::string> split_name(std::string_view name)
{
    std::vector<std::string> segments;
    std::size_t start{};
    while (true)
    {
        const std::size_t dot = name.find('.', start);
        segments.emplace_back(name.substr(start, dot == std::string_view::npos ? dot : dot - start));
        if (dot == std::string_view::npos)
            break;
        start = dot + 1U;
    }
    return segments;
}

const char *handle_kind_name(HandleKind kind)
{
    switch (kind)
    {
    case HandleKind::none:
        return "none";
    case HandleKind::player:
        return "player";
    case HandleKind::entity:
        return "entity";
    case HandleKind::widget:
        return "widget";
    case HandleKind::light:
        return "light";
    case HandleKind::effect:
        return "effect";
    case HandleKind::sound:
        return "sound";
    case HandleKind::timer:
        return "timer";
    case HandleKind::quest:
        return "quest";
    case HandleKind::user_base:
        break;
    }
    return nullptr;
}

std::string handle_kind_text(HandleKind kind)
{
    const char *name = handle_kind_name(kind);
    return name != nullptr ? std::string{name} : std::to_string(static_cast<std::uint32_t>(kind));
}

int handle_index(lua_State *L)
{
    const auto *handle = static_cast<const Handle *>(lua_touserdatatagged(L, 1, handle_userdata_tag));
    const char *key = lua_tostring(L, 2);
    if (handle == nullptr || key == nullptr)
        return 0;
    if (std::strcmp(key, "kind") == 0)
    {
        const std::string kind = handle_kind_text(handle->kind);
        lua_pushlstring(L, kind.data(), kind.size());
        return 1;
    }
    if (std::strcmp(key, "id") == 0)
    {
        lua_pushnumber(L, static_cast<double>(handle->id));
        return 1;
    }
    if (std::strcmp(key, "valid") == 0)
    {
        lua_pushboolean(L, handle->valid() ? 1 : 0);
        return 1;
    }
    luaL_error(L, "Handle has no field '%s'", key);
}

int handle_tostring(lua_State *L)
{
    const auto *handle = static_cast<const Handle *>(lua_touserdatatagged(L, 1, handle_userdata_tag));
    if (handle == nullptr)
        return 0;
    const std::string text =
        std::format("Handle({} {})", handle_kind_text(handle->kind), handle->id);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int handle_equal(lua_State *L)
{
    const auto *left = static_cast<const Handle *>(lua_touserdatatagged(L, 1, handle_userdata_tag));
    const auto *right = static_cast<const Handle *>(lua_touserdatatagged(L, 2, handle_userdata_tag));
    lua_pushboolean(L, left != nullptr && right != nullptr && *left == *right ? 1 : 0);
    return 1;
}

// Checks one native-call argument against its declared type. A vec3
// parameter also takes a {x=, y=, z=} table.
bool coerce_argument(Value &value, ValueType type, std::string &problem)
{
    const ValueType actual = value.type();
    switch (type)
    {
    case ValueType::any:
    case ValueType::nil:
        return true;
    case ValueType::integer:
        if (actual == ValueType::integer)
            return true;
        problem = actual == ValueType::number ? "a whole number" : "an integer";
        return false;
    case ValueType::number:
        if (actual == ValueType::integer || actual == ValueType::number)
            return true;
        problem = "a number";
        return false;
    case ValueType::vec3:
        if (actual == ValueType::vec3)
            return true;
        if (actual == ValueType::map)
        {
            const Value *x = value.field("x");
            const Value *y = value.field("y");
            const Value *z = value.field("z");
            if (x != nullptr && y != nullptr && z != nullptr && x->is_number() && y->is_number() &&
                z->is_number() && value.as_map().size() == 3U)
            {
                value = Vec3{x->as_number(), y->as_number(), z->as_number()};
                return true;
            }
        }
        problem = "a vector";
        return false;
    case ValueType::array:
        if (actual == ValueType::array || (actual == ValueType::map && value.as_map().empty()))
            return true;
        problem = "an array (a table with keys 1..n)";
        return false;
    case ValueType::map:
        if (actual == ValueType::map || (actual == ValueType::array && value.as_array().empty()))
            return true;
        problem = "a table";
        return false;
    case ValueType::boolean:
    case ValueType::string:
    case ValueType::handle:
    case ValueType::callback:
        if (actual == type)
            return true;
        problem = std::string{"a "} + value_type_name(type);
        return false;
    }
    return true;
}

// Every registry function a script calls lands here. Upvalue 1 is the binding.
int native_call(lua_State *L)
{
    auto &binding = *static_cast<NativeBinding *>(lua_tolightuserdata(L, lua_upvalueindex(1)));
    Vm &vm = *binding.vm;
    if (!binding.denied.empty())
        raise(L, binding.denied);
    const FunctionSpec &spec = binding.function->spec;
    ModSlot &mod = *vm.mods[binding.mod];
    const int count = lua_gettop(L);

    std::string error;
    Value result;
    {
        // (Integration) Restored however the call ends: a Luau error (out of
        // memory while marshalling) unwinds through here as an exception of
        // its own, and a stale thread would be used by the next protect().
        struct ActiveThread final
        {
            Vm &vm;
            lua_State *previous;
            ~ActiveThread()
            {
                vm.active_thread = previous;
            }
        } restore{vm, std::exchange(vm.active_thread, L)};
        try
        {
            if (!spec.variadic && static_cast<std::size_t>(count) > spec.params.size())
                throw ScriptError{std::format("takes at most {} argument{}, got {}",
                                              spec.params.size(),
                                              spec.params.size() == 1U ? "" : "s", count)};
            std::vector<Value> args;
            args.reserve(static_cast<std::size_t>(count));
            for (int index = 1; index <= count; ++index)
                args.push_back(ModRuntime::Impl::read_value(vm, L, index, binding.mod));
            for (std::size_t index{}; index < spec.params.size(); ++index)
            {
                const ParamSpec &param = spec.params[index];
                if (index >= args.size() || args[index].is_nil())
                {
                    if (!param.optional)
                        throw ScriptError{std::format("argument #{} ({}) is required", index + 1U,
                                                      param.name)};
                    continue;
                }
                std::string problem;
                if (!coerce_argument(args[index], param.type, problem))
                    throw ScriptError{std::format("argument #{} ({}) must be {}, got {}",
                                                  index + 1U, param.name, problem,
                                                  value_type_name(args[index].type()))};
            }
            CallContext context{mod.source.id, vm.side, mod.capabilities, vm.acting_player};
            // (Integration) The budget is for script time: what the game
            // spends inside this call (a large edit, a file read, a slow
            // frame of its own) moves the deadline on. Scripts it runs
            // meanwhile still answer to the deadline as it stood.
            const auto native_start = std::chrono::steady_clock::now();
            result = binding.function->call(context, args);
            if (vm.depth > 0 && !vm.timed_out)
            {
                const auto spent = std::chrono::steady_clock::now() - native_start;
                vm.deadline += spent;
                vm.native_time += spent;
            }
        }
        catch (const std::runtime_error &exception)
        {
            error = exception.what();
        }
        catch (const std::logic_error &exception)
        {
            error = exception.what();
        }
        catch (const std::bad_alloc &)
        {
            error = "out of memory";
        }
    }
    if (!error.empty())
        raise(L, binding.name + ": " + error);
    if (spec.returns == ValueType::nil && result.is_nil())
        return 0;
    ModRuntime::Impl::push_value(vm, L, result);
    return 1;
}

// print(...) logs one line at info level against the mod.
int print_function(lua_State *L)
{
    auto &binding = *static_cast<NativeBinding *>(lua_tolightuserdata(L, lua_upvalueindex(1)));
    const int count = lua_gettop(L);
    std::string line;
    for (int index = 1; index <= count; ++index)
    {
        std::size_t length{};
        const char *text = luaL_tolstring(L, index, &length);
        if (index > 1)
            line += '\t';
        line.append(text, length);
        lua_pop(L, 1);
    }
    Vm &vm = *binding.vm;
    vm.runtime->log(vm.mods[binding.mod]->source.id, vm.side, LogLevel::info, line);
    return 0;
}

bool compile(std::string_view source, std::string &bytecode, std::string &error)
{
    // (Integration) Windows editors (and PowerShell's Set-Content) often save
    // UTF-8 with a byte order mark, which Luau does not expect.
    if (source.starts_with("\xEF\xBB\xBF"))
        source.remove_prefix(3U);
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    std::size_t size{};
    char *data = luau_compile(source.data(), source.size(), &options, &size);
    if (data == nullptr)
    {
        error = ": out of memory while compiling";
        return false;
    }
    bytecode.assign(data, size);
    std::free(data);
    if (bytecode.empty() || bytecode.front() == '\0')
    {
        error = bytecode.empty() ? std::string{": empty bytecode"} : bytecode.substr(1U);
        return false;
    }
    return true;
}

std::string directory_of(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0U, slash)};
}

bool is_script_path(std::string_view path)
{
    return path.ends_with(".luau") || path.ends_with(".lua");
}

int require_function(lua_State *L);

// Pushes a fresh environment for one file of `mod`: its globals are private,
// everything else comes from the mod's readonly environment.
void push_file_env(Vm &vm, lua_State *L, std::uint32_t mod, std::string_view path)
{
    ModSlot &slot = *vm.mods[mod];
    lua_rawcheckstack(L, 4);
    lua_newtable(L);
    lua_getref(L, slot.file_meta_ref);
    lua_setmetatable(L, -2);
    // require resolves relative to this file's folder.
    NativeBinding &binding = vm.bindings.emplace_back();
    binding.vm = &vm;
    binding.mod = mod;
    binding.name = "require";
    lua_pushlightuserdata(L, &binding);
    const std::string directory = directory_of(path);
    lua_pushlstring(L, directory.data(), directory.size());
    lua_pushcclosurek(L, require_function, "require", 2, nullptr);
    lua_rawsetfield(L, -2, "require");
    lua_setsafeenv(L, -1, 1);
}

// Loads (once) and leaves on the stack the module at resolved `path` of
// `mod`. A file's chunk runs in the owning mod's environment.
void load_module(Vm &vm, lua_State *L, std::uint32_t mod, const std::string &path)
{
    ModSlot &slot = *vm.mods[mod];
    lua_rawcheckstack(L, 6);
    lua_getref(L, slot.modules_ref);
    const int modules = lua_gettop(L);
    lua_rawgetfield(L, modules, path.c_str());
    if (!lua_isnil(L, -1))
    {
        if (lua_islightuserdata(L, -1))
            raise(L, std::format("require cycle: {}/{} is still loading", slot.source.id, path));
        lua_remove(L, modules);
        return;
    }
    lua_pop(L, 1);
    const auto file = slot.files.find(path);
    if (file == slot.files.end())
        raise(L, std::format("{}/{} does not exist", slot.source.id, path));

    const std::string chunk = slot.source.id + "/" + path;
    std::string bytecode;
    std::string error;
    if (!compile(file->second, bytecode, error))
    {
        lua_settop(L, modules - 1);
        raise(L, chunk + error);
    }
    // Mark it loading, so a require cycle is an error instead of a hang.
    lua_pushlightuserdata(L, &vm);
    lua_rawsetfield(L, modules, path.c_str());

    push_file_env(vm, L, mod, path);
    const std::string chunk_name = "@" + chunk;
    const int status =
        luau_load(L, chunk_name.c_str(), bytecode.data(), bytecode.size(), lua_gettop(L));
    lua_remove(L, -2); // the environment
    if (status == LUA_OK)
    {
        if (lua_pcall(L, 0, 1, 0) == LUA_OK)
        {
            if (lua_isnil(L, -1))
            {
                lua_pop(L, 1);
                lua_pushboolean(L, 1);
            }
            lua_pushvalue(L, -1);
            lua_rawsetfield(L, modules, path.c_str());
            lua_remove(L, modules);
            return;
        }
    }
    // Forget the loading mark and pass the error on.
    lua_pushnil(L);
    lua_rawsetfield(L, modules, path.c_str());
    lua_remove(L, modules);
    lua_error(L);
}

bool resolve_module(Vm &vm, std::uint32_t from, std::string_view directory,
                    std::string_view request, std::uint32_t &target, std::string &path,
                    std::string &error)
{
    ModSlot &slot = *vm.mods[from];
    target = from;
    std::string relative;
    if (request.starts_with('@'))
    {
        const std::size_t slash = request.find('/');
        const std::string name = lowercase_ascii(
            request.substr(1U, slash == std::string_view::npos ? slash : slash - 1U));
        relative = slash == std::string_view::npos ? std::string{"init"}
                                                   : std::string{request.substr(slash + 1U)};
        if (name != "self" && name != slot.key)
        {
            const bool declared =
                std::any_of(slot.source.dependencies.begin(), slot.source.dependencies.end(),
                            [&](const std::string &dependency) {
                                return lowercase_ascii(dependency) == name;
                            });
            const auto found = vm.mod_lookup.find(name);
            if (!declared)
            {
                error = std::format("'{}' is not a declared dependency of {} (add it to depends= "
                                    "in openminerz.mod)",
                                    name, slot.source.id);
                return false;
            }
            if (found == vm.mod_lookup.end() || vm.mods[found->second]->state != ModState::running)
            {
                error = std::format("mod '{}' is not loaded on the {}", name, side_name(vm.side));
                return false;
            }
            target = found->second;
        }
    }
    else if (request.starts_with("./") || request.starts_with("../"))
        relative = directory.empty() ? std::string{request}
                                     : std::string{directory} + "/" + std::string{request};
    else
        relative = std::string{request};

    std::string normalized;
    if (relative.empty() || !normalize_mod_path(relative, normalized))
    {
        error = std::format("'{}' is not a path inside the mod", request);
        return false;
    }
    const std::string_view forbidden = vm.side == Side::client ? "server/" : "client/";
    if (normalized.starts_with(forbidden))
    {
        error = std::format("'{}' is {} code and cannot be required on the {}", request,
                            vm.side == Side::client ? "server" : "client", side_name(vm.side));
        return false;
    }
    const ModSlot &owner = *vm.mods[target];
    const std::array<std::string, 4U> candidates{
        normalized + ".luau", normalized + ".lua", normalized + "/init.luau",
        normalized + "/init.lua"};
    if (is_script_path(normalized) && owner.files.contains(normalized))
    {
        path = normalized;
        return true;
    }
    for (const std::string &candidate : candidates)
        if (owner.files.contains(candidate))
        {
            path = candidate;
            return true;
        }
    error = std::format("no module '{}' in {}", request, owner.source.id);
    return false;
}

int require_function(lua_State *L)
{
    auto &binding = *static_cast<NativeBinding *>(lua_tolightuserdata(L, lua_upvalueindex(1)));
    std::size_t length{};
    const char *directory = lua_tolstring(L, lua_upvalueindex(2), &length);
    if (lua_type(L, 1) != LUA_TSTRING)
        raise(L, "require expects a path string");
    std::size_t request_length{};
    const char *request = lua_tolstring(L, 1, &request_length);
    std::uint32_t target{};
    std::string path;
    std::string error;
    if (!resolve_module(*binding.vm, binding.mod, std::string_view{directory, length},
                        std::string_view{request, request_length}, target, path, error))
        raise(L, "require: " + error);
    load_module(*binding.vm, L, target, path);
    return 1;
}

// Runs one entry file: argument 1 is the mod index, 2 the path.
int run_entry_file(lua_State *L)
{
    Vm *vm = static_cast<Vm *>(lua_callbacks(L)->userdata);
    const auto mod = static_cast<std::uint32_t>(lua_tointeger(L, 1));
    const std::string path = lua_tostring(L, 2);
    lua_settop(L, 0);
    load_module(*vm, L, mod, path);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

std::string lowercase_ascii(std::string_view text)
{
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool normalize_mod_path(std::string_view path, std::string &normalized)
{
    std::vector<std::string> segments;
    std::size_t start{};
    while (start <= path.size())
    {
        std::size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos)
            end = path.size();
        const std::string_view segment = path.substr(start, end - start);
        if (segment == "..")
        {
            if (segments.empty())
                return false;
            segments.pop_back();
        }
        else if (!segment.empty() && segment != ".")
        {
            if (segment.find(':') != std::string_view::npos)
                return false;
            segments.push_back(lowercase_ascii(segment));
        }
        start = end + 1U;
    }
    if (segments.empty())
        return false;
    normalized.clear();
    for (const std::string &segment : segments)
    {
        if (!normalized.empty())
            normalized += '/';
        normalized += segment;
    }
    return true;
}

CapabilityMask capabilities_for(net::ModScope scope, Side side) noexcept
{
    if (scope == net::ModScope::client)
        return side == Side::client
                   ? capability::presentation | capability::local_storage | capability::query
                   : capability::none;
    return side == Side::server
               ? capability::gameplay | capability::network | capability::world_storage |
                     capability::query
               : capability::presentation | capability::local_storage | capability::network |
                     capability::query;
}

const char *side_name(Side side) noexcept
{
    return side == Side::server ? "server" : "client";
}

const char *log_level_name(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warning:
        return "warn";
    case LogLevel::error:
        return "error";
    }
    return "info";
}

// ---------------------------------------------------------------------------
// Marshalling
// ---------------------------------------------------------------------------

void ModRuntime::Impl::push_value(Vm &vm, lua_State *L, const Value &value, int depth)
{
    if (depth > maximum_value_depth)
        luaL_error(L, "value nesting is deeper than %d levels", maximum_value_depth);
    lua_rawcheckstack(L, 3);
    switch (value.type())
    {
    case ValueType::nil:
    case ValueType::any:
        lua_pushnil(L);
        return;
    case ValueType::boolean:
        lua_pushboolean(L, value.as_bool() ? 1 : 0);
        return;
    case ValueType::integer:
        lua_pushnumber(L, static_cast<double>(std::get<std::int64_t>(value.storage())));
        return;
    case ValueType::number:
        lua_pushnumber(L, std::get<double>(value.storage()));
        return;
    case ValueType::string: {
        const std::string &text = value.as_string();
        lua_pushlstring(L, text.data(), text.size());
        return;
    }
    case ValueType::vec3: {
        const Vec3 vector = value.as_vec3();
        lua_pushvector(L, static_cast<float>(vector.x), static_cast<float>(vector.y),
                       static_cast<float>(vector.z));
        return;
    }
    case ValueType::handle:
        push_handle(vm, L, value.as_handle());
        return;
    case ValueType::callback:
        if (!push_callback(vm, L, value.as_callback()))
            lua_pushnil(L);
        return;
    case ValueType::array: {
        const Array &array = value.as_array();
        lua_createtable(L, static_cast<int>(array.size()), 0);
        int index{};
        for (const Value &element : array)
        {
            push_value(vm, L, element, depth + 1);
            lua_rawseti(L, -2, ++index);
        }
        return;
    }
    case ValueType::map: {
        const Map &map = value.as_map();
        lua_createtable(L, 0, static_cast<int>(map.size()));
        for (const auto &[key, element] : map)
        {
            lua_pushlstring(L, key.data(), key.size());
            push_value(vm, L, element, depth + 1);
            lua_rawset(L, -3);
        }
        return;
    }
    }
    lua_pushnil(L);
}

Value ModRuntime::Impl::read_value(Vm &vm, lua_State *L, int index, std::uint32_t owner,
                                   int depth)
{
    switch (lua_type(L, index))
    {
    case LUA_TNONE:
    case LUA_TNIL:
        return {};
    case LUA_TBOOLEAN:
        return Value{lua_toboolean(L, index) != 0};
    case LUA_TNUMBER: {
        const double number = lua_tonumber(L, index);
        constexpr double exact = 9007199254740992.0; // 2^53
        if (std::trunc(number) == number && std::fabs(number) <= exact)
            return Value{static_cast<std::int64_t>(number)};
        return Value{number};
    }
    case LUA_TVECTOR: {
        const float *vector = lua_tovector(L, index);
        return Value{Vec3{vector[0], vector[1], vector[2]}};
    }
    case LUA_TSTRING: {
        std::size_t length{};
        const char *text = lua_tolstring(L, index, &length);
        return Value{std::string{text, length}};
    }
    case LUA_TUSERDATA:
        if (const auto *handle =
                static_cast<const Handle *>(lua_touserdatatagged(L, index, handle_userdata_tag)))
            return Value{*handle};
        break;
    case LUA_TFUNCTION:
        return Value{hold_function(vm, L, index, owner)};
    case LUA_TTABLE: {
        if (depth >= maximum_value_depth)
            throw ScriptError{"table nesting is deeper than 64 levels (a table inside itself?)"};
        index = lua_absindex(L, index);
        lua_rawcheckstack(L, 4);
        const int length = lua_objlen(L, index);
        std::size_t strings{};
        std::size_t sequence{};
        std::size_t others{};
        lua_pushnil(L);
        while (lua_next(L, index) != 0)
        {
            const int key_type = lua_type(L, -2);
            if (key_type == LUA_TSTRING)
                ++strings;
            else if (key_type == LUA_TNUMBER)
            {
                const double key = lua_tonumber(L, -2);
                if (key >= 1.0 && key <= static_cast<double>(length) && std::trunc(key) == key)
                    ++sequence;
                else
                    ++others;
            }
            else
                ++others;
            lua_pop(L, 1);
        }
        if (strings == 0U && sequence == 0U && others == 0U)
            return Value{Map{}};
        if (strings == 0U && others == 0U && sequence == static_cast<std::size_t>(length))
        {
            Array array;
            array.reserve(sequence);
            for (int element = 1; element <= length; ++element)
            {
                lua_rawgeti(L, index, element);
                array.push_back(read_value(vm, L, -1, owner, depth + 1));
                lua_pop(L, 1);
            }
            return Value{std::move(array)};
        }
        if (sequence != 0U || others != 0U)
            throw ScriptError{"a table crossing into the game must be a list (keys 1..n) or a "
                              "record (string keys), not a mix"};
        std::vector<std::string> keys;
        keys.reserve(strings);
        lua_pushnil(L);
        while (lua_next(L, index) != 0)
        {
            std::size_t key_length{};
            const char *key = lua_tolstring(L, -2, &key_length);
            keys.emplace_back(key, key_length);
            lua_pop(L, 1);
        }
        std::sort(keys.begin(), keys.end());
        Map map;
        map.reserve(keys.size());
        for (std::string &key : keys)
        {
            lua_pushlstring(L, key.data(), key.size());
            lua_rawget(L, index);
            Value element = read_value(vm, L, -1, owner, depth + 1);
            lua_pop(L, 1);
            map.emplace_back(std::move(key), std::move(element));
        }
        return Value{std::move(map)};
    }
    default:
        break;
    }
    throw ScriptError{std::string{"a "} + luaL_typename(L, index) + " cannot cross into the game"};
}

void ModRuntime::Impl::push_handle(Vm &vm, lua_State *L, Handle handle)
{
    lua_rawcheckstack(L, 4);
    lua_getref(L, vm.handle_cache_ref);
    const int cache = lua_gettop(L);
    const auto kind = static_cast<std::uint32_t>(handle.kind);
    // Small handles key the cache by number, the rest by text.
    if (kind < 4096U && handle.id < (std::uint64_t{1} << 40U))
        lua_pushnumber(L, static_cast<double>((std::uint64_t{kind} << 40U) | handle.id));
    else
    {
        const std::string key = std::format("{}:{}", kind, handle.id);
        lua_pushlstring(L, key.data(), key.size());
    }
    lua_pushvalue(L, -1);
    lua_rawget(L, cache);
    if (lua_type(L, -1) == LUA_TUSERDATA)
    {
        lua_replace(L, cache);
        lua_settop(L, cache);
        return;
    }
    lua_pop(L, 1);
    auto *storage = static_cast<Handle *>(
        lua_newuserdatataggedwithmetatable(L, sizeof(Handle), handle_userdata_tag));
    *storage = handle;
    lua_pushvalue(L, -1);
    lua_insert(L, cache + 1); // cache, ud, key, ud
    lua_rawset(L, cache);     // cache, ud
    lua_remove(L, cache);
}

Callback ModRuntime::Impl::hold_function(Vm &vm, lua_State *L, int index, std::uint32_t owner)
{
    lua_pushvalue(L, index);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    std::uint32_t id = vm.next_hold++;
    if (id == 0U)
        id = vm.next_hold++;
    vm.holds.insert_or_assign(id, Hold{ref, owner});
    return Callback{vm.id, id};
}

bool ModRuntime::Impl::push_callback(Vm &vm, lua_State *L, const Callback &callback)
{
    if (callback.vm != vm.id)
        return false;
    const auto found = vm.holds.find(callback.ref);
    if (found == vm.holds.end())
        return false;
    lua_rawcheckstack(L, 1);
    lua_getref(L, found->second.ref);
    return true;
}

// ---------------------------------------------------------------------------
// VMs and mod environments
// ---------------------------------------------------------------------------

ModRuntime::Impl::Impl(ModRuntime &runtime, RuntimeLimits runtime_limits)
    : owner{runtime}, limits{runtime_limits}
{
    register_core_api();
}

ModRuntime::Impl::~Impl()
{
    for (Side side : {Side::server, Side::client})
        stop(side);
}

Vm *ModRuntime::Impl::vm_by_id(std::uint32_t id) const noexcept
{
    for (const auto &vm : vms)
        if (vm && vm->id == id)
            return vm.get();
    return nullptr;
}

void ModRuntime::Impl::log(std::string_view mod_id, Side side, LogLevel level,
                           std::string_view message)
{
    LogEntry entry{std::string{mod_id}, side, level, std::string{message}};
    if (sink)
        sink(entry);
    if (limits.log_history == 0U)
        return;
    if (history.size() >= limits.log_history)
        history.pop_front();
    history.push_back(std::move(entry));
}

void ModRuntime::Impl::mod_error(Vm &vm, std::uint32_t mod, std::string_view message)
{
    ModSlot &slot = *vm.mods[mod];
    ++slot.errors;
    log(slot.source.id, vm.side, LogLevel::error, message);
}

void ModRuntime::Impl::stop_mod(Vm &vm, std::uint32_t mod, std::string_view reason)
{
    ModSlot &slot = *vm.mods[mod];
    if (slot.state == ModState::running)
        slot.state = ModState::stopped;
    if (slot.error.empty())
        slot.error = std::string{reason};
    log(slot.source.id, vm.side, LogLevel::error,
        std::format("{} stopped on the {}: {}", slot.source.id, side_name(vm.side), reason));
    remove_mod_handlers(vm, mod);
}

bool ModRuntime::Impl::protect(Vm &vm, std::optional<std::uint32_t> mod,
                               const std::function<void(lua_State *)> &work)
{
    lua_State *L = vm.active_thread != nullptr ? vm.active_thread : vm.L;
    const bool enforcing = std::exchange(vm.allocator.enforcing, false);
    lua_rawcheckstack(L, 2);
    lua_getref(L, vm.entry_ref);
    lua_pushlightuserdata(L, const_cast<std::function<void(lua_State *)> *>(&work));
    vm.allocator.enforcing = true;
    const int status = lua_pcall(L, 1, 0, 0);
    vm.allocator.enforcing = enforcing;
    if (status == LUA_OK)
        return true;
    std::string message = status == LUA_ERRMEM
                              ? std::format("not enough memory (the {} VM is capped at {} MiB)",
                                            side_name(vm.side), limits.memory_bytes >> 20U)
                              : error_text(L, -1);
    lua_pop(L, 1);
    if (mod)
        mod_error(vm, *mod, message);
    else
        log({}, vm.side, LogLevel::error, message);
    vm.last_error = std::move(message);
    if (status == LUA_ERRMEM)
        lua_gc(L, LUA_GCCOLLECT, 0);
    return false;
}

bool ModRuntime::Impl::call(Vm &vm, lua_State *L, std::uint32_t mod, int nargs, int results,
                            bool loading)
{
    ModSlot &slot = *vm.mods[mod];
    // (Integration) A handler that fires its own event through the game
    // (player.damaged -> players.damage -> player.damaged ...) nests C++ and
    // script frames; it is refused here, as a script error, long before the
    // native stack could run out.
    constexpr int maximum_nested_calls = 48;
    if (vm.depth >= maximum_nested_calls)
    {
        lua_pop(L, nargs + 1);
        for (int index{}; index < results; ++index)
            lua_pushnil(L);
        vm.last_error = std::format("calls into scripts nested more than {} deep (an event whose "
                                    "handler fires it again?)",
                                    maximum_nested_calls);
        mod_error(vm, mod, vm.last_error);
        return false;
    }
    const bool outermost = vm.depth == 0;
    if (outermost)
    {
        vm.budget = loading ? limits.load_budget : limits.call_budget;
        vm.call_start = std::chrono::steady_clock::now();
        vm.deadline = vm.call_start + vm.budget;
        vm.cpu_start = thread_cpu_time();
        vm.native_time = {};
        vm.stalled_time = {};
        vm.timed_out = false;
        vm.interrupts = 0U;
        vm.timeout_mod.reset();
    }
    const std::optional<std::uint32_t> previous = std::exchange(vm.active_mod, mod);
    lua_setmemcat(L, slot.memory_category);
    // The traceback handler goes under the function and its arguments.
    lua_rawcheckstack(L, 1);
    lua_getref(L, vm.traceback_ref);
    lua_insert(L, -(nargs + 2));
    const int handler = lua_gettop(L) - nargs - 1;
    ++vm.depth;
    const int status = lua_pcall(L, nargs, results, handler);
    --vm.depth;
    vm.active_mod = previous;
    lua_setmemcat(L, previous ? vm.mods[*previous]->memory_category : 0);
    if (status == LUA_OK)
    {
        lua_remove(L, handler);
        return true;
    }

    std::string message = status == LUA_ERRMEM
                              ? std::format("not enough memory (the {} VM is capped at {} MiB)",
                                            side_name(vm.side), limits.memory_bytes >> 20U)
                              : error_text(L, -1);
    lua_settop(L, handler - 1);
    for (int index{}; index < results; ++index)
        lua_pushnil(L);
    vm.last_error = message;
    // (Polish) An overrun is reported once, by the outermost call, as a
    // warning or a stop; the frames it unwound through are not errors.
    if (!vm.timed_out)
        mod_error(vm, mod, message);
    else if (outermost)
    {
        overran(vm, vm.timeout_mod.value_or(mod), loading);
        vm.timed_out = false;
    }
    if (status == LUA_ERRMEM)
        lua_gc(L, LUA_GCCOLLECT, 0);
    return false;
}

void ModRuntime::Impl::overran(Vm &vm, std::uint32_t mod, bool loading)
{
    ModSlot &slot = *vm.mods[mod];
    const auto now = std::chrono::steady_clock::now();
    // Script time: what was left after game functions and stalls.
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - vm.call_start - vm.native_time - vm.stalled_time);
    const std::string where = vm.timeout_where.empty() ? slot.source.id : vm.timeout_where;
    if (loading)
    {
        // A file that did not finish loading leaves its mod half set up.
        stop_mod(vm, mod,
                 std::format("{}: its top level ran longer than the {} ms load budget", where,
                             vm.budget.count()));
        return;
    }
    if (waited >= limits.hard_overrun)
    {
        stop_mod(vm, mod,
                 std::format("{}: one call kept the game waiting {} ms (the limit is {} ms)", where,
                             waited.count(), limits.hard_overrun.count()));
        return;
    }
    while (!slot.overruns.empty() && now - slot.overruns.front() >= limits.overrun_window)
        slot.overruns.pop_front();
    slot.overruns.push_back(now);
    const auto strikes = static_cast<std::uint32_t>(slot.overruns.size());
    const auto window = std::chrono::duration_cast<std::chrono::seconds>(limits.overrun_window);
    if (strikes >= limits.overrun_strikes)
    {
        stop_mod(vm, mod,
                 std::format("{}: calls overran the {} ms budget {} times within {} s", where,
                             vm.budget.count(), strikes, window.count()));
        return;
    }
    log(slot.source.id, vm.side, LogLevel::warning,
        std::format("{}: call aborted: it ran longer than its {} ms budget (overrun {} within "
                    "{} s; the mod stops at {})",
                    where, vm.budget.count(), strikes, window.count(), limits.overrun_strikes));
}

namespace
{

void lock_tables(lua_State *L, int table, int depth)
{
    table = lua_absindex(L, table);
    lua_rawcheckstack(L, 3);
    lua_pushnil(L);
    while (lua_next(L, table) != 0)
    {
        if (depth < maximum_value_depth && lua_istable(L, -1) && lua_getreadonly(L, -1) == 0)
            lock_tables(L, -1, depth + 1);
        lua_pop(L, 1);
    }
    lua_setreadonly(L, table, 1);
}

// Builds `slot`'s readonly environment: every registry library (live or a
// stub that explains the refusal), constants, print, then {__index=globals}.
void build_environment(ModRuntime::Impl &runtime, Vm &vm, lua_State *L, std::uint32_t mod)
{
    ModSlot &slot = *vm.mods[mod];
    lua_rawcheckstack(L, 8);
    lua_newtable(L);
    const int env = lua_gettop(L);

    // Walks/creates the tables for name's leading segments; leaves the parent
    // table on the stack and returns the leaf segment, or "" on a conflict.
    auto parent_of = [&](std::string_view name) -> std::string {
        const std::vector<std::string> segments = split_name(name);
        if (!std::all_of(segments.begin(), segments.end(), is_identifier))
            return {};
        lua_rawgetfield(L, LUA_GLOBALSINDEX, segments.front().c_str());
        const bool reserved = !lua_isnil(L, -1) || segments.front() == "require" ||
                              segments.front() == "print" || segments.size() < 2U;
        lua_pop(L, 1);
        if (reserved)
            return {};
        lua_pushvalue(L, env);
        for (std::size_t index{}; index + 1U < segments.size(); ++index)
        {
            lua_rawgetfield(L, -1, segments[index].c_str());
            if (lua_isnil(L, -1))
            {
                lua_pop(L, 1);
                lua_newtable(L);
                lua_pushvalue(L, -1);
                lua_rawsetfield(L, -3, segments[index].c_str());
            }
            else if (!lua_istable(L, -1))
            {
                lua_pop(L, 2);
                return {};
            }
            lua_remove(L, -2);
        }
        lua_rawgetfield(L, -1, segments.back().c_str());
        const bool taken = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (taken)
        {
            lua_pop(L, 1);
            return {};
        }
        return segments.back();
    };

    for (const auto &[name, function] : runtime.registry.functions())
    {
        const std::string leaf = parent_of(name);
        if (leaf.empty())
        {
            if (mod == 0U)
                runtime.log({}, vm.side, LogLevel::warning,
                            std::format("API function '{}' is not a usable library name; skipped",
                                        name));
            continue;
        }
        NativeBinding &binding = vm.bindings.emplace_back();
        binding.vm = &vm;
        binding.mod = mod;
        binding.function = &function;
        binding.name = name;
        const FunctionSpec &spec = function.spec;
        const CapabilityMask missing = spec.requires_caps & ~slot.capabilities;
        if (!side_in(vm.side, spec.sides))
            binding.denied = std::format("{} is only available on the {}", name,
                                         spec.sides == side_server ? "server" : "client");
        else if (missing != 0U)
            binding.denied =
                std::format("{} needs the {} capability, which {} does not have on the {}", name,
                            capability_list(missing), slot.source.id, side_name(vm.side));
        lua_pushlightuserdata(L, &binding);
        lua_pushcclosurek(L, native_call, binding.name.c_str(), 1, nullptr);
        lua_rawsetfield(L, -2, leaf.c_str());
        lua_pop(L, 1);
    }
    for (const auto &[name, constant] : runtime.registry.constants())
    {
        if (!side_in(vm.side, constant.sides))
            continue;
        const std::string leaf = parent_of(name);
        if (leaf.empty())
            continue;
        ModRuntime::Impl::push_value(vm, L, constant.value);
        lua_rawsetfield(L, -2, leaf.c_str());
        lua_pop(L, 1);
    }

    NativeBinding &printer = vm.bindings.emplace_back();
    printer.vm = &vm;
    printer.mod = mod;
    printer.name = "print";
    lua_pushlightuserdata(L, &printer);
    lua_pushcclosurek(L, print_function, "print", 1, nullptr);
    lua_rawsetfield(L, env, "print");
    lua_settop(L, env);

    lua_newtable(L);
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_rawsetfield(L, -2, "__index");
    lua_pushliteral(L, "locked");
    lua_rawsetfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, 1);
    lua_setmetatable(L, env);
    // Every library table (constants included) and the environment itself
    // are readonly, which also keeps Luau's import caching valid.
    lock_tables(L, env, 0);
    lua_setsafeenv(L, env, 1);
    slot.env_ref = lua_ref(L, env);

    lua_newtable(L);
    lua_pushvalue(L, env);
    lua_rawsetfield(L, -2, "__index");
    lua_pushliteral(L, "locked");
    lua_rawsetfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, 1);
    slot.file_meta_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    lua_newtable(L);
    slot.modules_ref = lua_ref(L, -1);
    lua_pop(L, 2);
}

void prepare_state(Vm &vm, lua_State *L)
{
    luaL_openlibs(L);
    for (const char *name : stripped_globals)
    {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }

    lua_newtable(L);
    lua_pushcfunction(L, handle_index, "__index");
    lua_rawsetfield(L, -2, "__index");
    lua_pushcfunction(L, handle_tostring, "__tostring");
    lua_rawsetfield(L, -2, "__tostring");
    lua_pushcfunction(L, handle_equal, "__eq");
    lua_rawsetfield(L, -2, "__eq");
    lua_pushliteral(L, "Handle");
    lua_rawsetfield(L, -2, "__type");
    lua_pushliteral(L, "locked");
    lua_rawsetfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, 1);
    lua_pushvalue(L, -1);
    lua_ref(L, -1); // kept alive with the state
    lua_pop(L, 1);
    lua_setuserdatametatable(L, handle_userdata_tag);

    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_rawsetfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    vm.handle_cache_ref = lua_ref(L, -1);
    lua_pop(L, 1);

    luaL_sandbox(L);
}

} // namespace

bool ModRuntime::Impl::attach(Vm &vm, ModSource source, bool run_scripts)
{
    const std::string key = lowercase_ascii(source.id);
    if (key.empty() || vm.mod_lookup.contains(key))
    {
        log(source.id, vm.side, LogLevel::error,
            key.empty() ? "a mod without an id cannot load"
                        : std::format("{} is already loaded on the {}", source.id,
                                      side_name(vm.side)));
        return false;
    }
    auto slot = std::make_unique<ModSlot>();
    slot->key = key;
    slot->index = static_cast<std::uint32_t>(vm.mods.size());
    slot->memory_category = static_cast<int>(std::min<std::size_t>(slot->index + 1U, 255U));
    slot->capabilities = source.capabilities.value_or(capabilities_for(source.scope, vm.side));
    for (const auto &[path, text] : source.files)
    {
        std::string normalized;
        if (normalize_mod_path(path, normalized) && is_script_path(normalized))
            slot->files.insert_or_assign(std::move(normalized), text);
    }
    std::uint64_t seed = vm.session.world_seed ^ 0x9E3779B97F4A7C15ULL;
    for (const char character : key)
        seed = (seed ^ static_cast<unsigned char>(character)) * 0x100000001B3ULL;
    slot->random_state = seed;
    slot->source = std::move(source);
    slot->source.files.clear(); // the normalized copy above is the one used
    const std::uint32_t mod = slot->index;
    vm.mod_lookup.emplace(key, mod);
    vm.mods.push_back(std::move(slot));
    ModSlot &added = *vm.mods[mod];

    for (const std::string &dependency : added.source.dependencies)
    {
        const auto found = vm.mod_lookup.find(lowercase_ascii(dependency));
        if (found == vm.mod_lookup.end() || found->second >= mod ||
            vm.mods[found->second]->state != ModState::running)
        {
            added.state = ModState::failed;
            added.error = std::format("needs mod '{}', which is not loaded before it", dependency);
            log(added.source.id, vm.side, LogLevel::error, added.error);
            return false;
        }
    }

    if (!protect(vm, mod, [&](lua_State *L) { build_environment(*this, vm, L, mod); }))
    {
        added.state = ModState::failed;
        added.error = "its environment could not be built";
        return false;
    }
    if (!run_scripts)
        return true;

    // shared/ first, then this side's folder: every file directly inside runs
    // once, in name order; deeper files are modules for require.
    std::vector<std::string> entries;
    for (const std::string_view folder : {std::string_view{"shared/"},
                                          vm.side == Side::server ? std::string_view{"server/"}
                                                                  : std::string_view{"client/"}})
        for (const auto &[path, text] : added.files)
            if (path.starts_with(folder) && path.find('/', folder.size()) == std::string::npos)
                entries.push_back(path);
    for (const std::string &path : entries)
    {
        bool loaded{};
        protect(vm, mod, [&](lua_State *L) {
            lua_pushcfunction(L, run_entry_file, "load");
            lua_pushinteger(L, static_cast<int>(mod));
            lua_pushlstring(L, path.data(), path.size());
            loaded = call(vm, L, mod, 2, 0, true);
        });
        if (!loaded)
        {
            if (added.state == ModState::running)
                added.state = ModState::failed;
            if (added.error.empty())
                added.error = vm.last_error.substr(0U, vm.last_error.find('\n'));
            remove_mod_handlers(vm, mod);
            return false;
        }
        if (added.state != ModState::running)
            return false;
    }
    return true;
}

bool ModRuntime::Impl::start(Side side, std::vector<ModSource> mods, const SessionInfo &info)
{
    stop(side);
    session = info;
    std::erase_if(mods, [&](const ModSource &mod) { return !runs_on(mod.scope, side); });
    if (mods.empty())
        return false;

    auto vm = std::make_unique<Vm>();
    vm->id = next_vm_id.fetch_add(1U);
    vm->side = side;
    vm->runtime = this;
    vm->session = info;
    vm->allocator.limit = limits.memory_bytes;
    vm->L = lua_newstate(allocate, &vm->allocator);
    if (vm->L == nullptr)
    {
        log({}, side, LogLevel::error, "the script VM could not be created");
        return false;
    }
    lua_callbacks(vm->L)->userdata = vm.get();
    lua_callbacks(vm->L)->interrupt = interrupt;
    lua_pushcfunction(vm->L, protected_entry, "runtime");
    vm->entry_ref = lua_ref(vm->L, -1);
    lua_pop(vm->L, 1);
    lua_pushcfunction(vm->L, traceback_handler, "traceback");
    vm->traceback_ref = lua_ref(vm->L, -1);
    lua_pop(vm->L, 1);
    Vm &created = *vm;
    vms[side == Side::server ? 1 : 0] = std::move(vm);
    if (!protect(created, std::nullopt, [&](lua_State *L) { prepare_state(created, L); }))
    {
        stop(side);
        return false;
    }

    for (ModSource &mod : mods)
        attach(created, std::move(mod), true);

    Array loaded;
    for (const auto &slot : created.mods)
        if (slot->state == ModState::running)
            loaded.emplace_back(slot->source.id);
    emit(side, "mod.loaded", Map{{"side", side_name(side)}, {"mods", std::move(loaded)}});
    return true;
}

void ModRuntime::Impl::stop(Side side)
{
    std::unique_ptr<Vm> &slot = vms[side == Side::server ? 1 : 0];
    if (!slot)
        return;
    if (slot->depth > 0 || slot->active_thread != nullptr)
    {
        // A script is on the stack (a native call asked for this): closing
        // the VM under it would crash. The caller retries from the game loop.
        log({}, side, LogLevel::error, "the mod VM cannot stop while a script is running");
        return;
    }
    // (Integration) Only a VM that saw world.loaded hears world.unloading: the
    // client VM of client-scope mods also runs in the menus, with no world.
    if (slot->entry_ref != LUA_NOREF && world_payload[side == Side::server ? 1 : 0].has_value())
        emit(side, "world.unloading", Map{{"side", side_name(side)}});
    persist_storage();
    for (const auto &listener : teardown_listeners)
        listener(side);
    std::unique_ptr<Vm> vm = std::move(slot);
    vm->subscriptions.clear();
    vm->timers.clear();
    vm->holds.clear();
    if (vm->L != nullptr)
    {
        vm->allocator.enforcing = false;
        lua_close(vm->L);
    }
    world_payload[side == Side::server ? 1 : 0].reset();
}

bool ModRuntime::Impl::emit(Side side, std::string_view event, const Value &payload)
{
    Vm *vm = vm_for(side);
    if (vm == nullptr)
        return true;
    const auto found = vm->subscriptions.find(event);
    if (found == vm->subscriptions.end() || found->second.empty())
        return true;
    const EventSpec *spec = registry.find_event(event);
    const bool cancellable = spec == nullptr || spec->cancellable;
    const std::vector<std::shared_ptr<Subscription>> handlers = found->second;
    bool proceed = true;
    protect(*vm, std::nullopt, [&](lua_State *L) {
        push_value(*vm, L, payload);
        const int argument = lua_gettop(L);
        for (const auto &handler : handlers)
        {
            if (!handler->active || vm->mods[handler->mod]->state != ModState::running)
                continue;
            if (handler->once)
            {
                handler->active = false;
                std::erase(vm->subscriptions[std::string{event}], handler);
            }
            if (push_callback(*vm, L, handler->callback))
            {
                lua_pushvalue(L, argument);
                const bool ok = call(*vm, L, handler->mod, 1, 1);
                const bool cancelled =
                    ok && cancellable && lua_isboolean(L, -1) && lua_toboolean(L, -1) == 0;
                lua_pop(L, 1);
                if (cancelled)
                    proceed = false;
            }
            if (handler->once)
                release(handler->callback);
            if (!proceed)
                break;
        }
        lua_settop(L, argument - 1);
    });
    return proceed;
}

Value ModRuntime::Impl::invoke(const Callback &callback, std::span<const Value> args,
                               Handle acting_player)
{
    Vm *vm = vm_by_id(callback.vm);
    if (vm == nullptr)
        return {};
    const auto hold = vm->holds.find(callback.ref);
    if (hold == vm->holds.end())
        return {};
    const std::uint32_t mod = hold->second.mod;
    if (vm->mods[mod]->state != ModState::running)
        return {};
    Value result;
    const Handle previous = std::exchange(vm->acting_player, acting_player);
    protect(*vm, mod, [&](lua_State *L) {
        lua_rawcheckstack(L, static_cast<int>(args.size()) + 2);
        if (!push_callback(*vm, L, callback))
            return;
        for (const Value &argument : args)
            push_value(*vm, L, argument);
        if (call(*vm, L, mod, static_cast<int>(args.size()), 1))
            result = read_value(*vm, L, -1, mod);
        lua_pop(L, 1);
    });
    vm->acting_player = previous;
    return result;
}

void ModRuntime::Impl::release(const Callback &callback) noexcept
{
    Vm *vm = vm_by_id(callback.vm);
    if (vm == nullptr)
        return;
    const auto hold = vm->holds.find(callback.ref);
    if (hold == vm->holds.end())
        return;
    lua_unref(vm->L, hold->second.ref);
    vm->holds.erase(hold);
}

RunResult ModRuntime::Impl::run(Side side, std::string_view mod_id, std::string_view chunk_name,
                                std::string_view source)
{
    RunResult result;
    Vm *vm = vm_for(side);
    const auto found = vm == nullptr ? decltype(vm->mod_lookup)::const_iterator{}
                                     : vm->mod_lookup.find(lowercase_ascii(mod_id));
    if (vm == nullptr || found == vm->mod_lookup.end())
    {
        result.error = std::format("{} is not loaded on the {}", mod_id, side_name(side));
        return result;
    }
    const std::uint32_t mod = found->second;
    ModSlot &slot = *vm->mods[mod];
    std::string path;
    if (!normalize_mod_path(chunk_name, path))
        path = "chunk";
    const std::string chunk = slot.source.id + "/" + path;
    bool logged{};
    const bool protected_ok = protect(*vm, mod, [&](lua_State *L) {
        std::string bytecode;
        std::string error;
        if (!compile(source, bytecode, error))
        {
            result.error = chunk + error;
            return;
        }
        // Chunks run one after another share their globals, like a console.
        if (slot.console_ref == LUA_NOREF)
        {
            push_file_env(*vm, L, mod, "");
            slot.console_ref = lua_ref(L, -1);
        }
        else
            lua_getref(L, slot.console_ref);
        const std::string name = "@" + chunk;
        const int status =
            luau_load(L, name.c_str(), bytecode.data(), bytecode.size(), lua_gettop(L));
        lua_remove(L, -2);
        if (status != LUA_OK)
        {
            result.error = error_text(L, -1);
            lua_pop(L, 1);
            return;
        }
        if (call(*vm, L, mod, 0, 1))
        {
            result.value = read_value(*vm, L, -1, mod);
            result.ok = true;
        }
        else
        {
            result.error = vm->last_error;
            logged = true;
        }
        lua_pop(L, 1);
    });
    if (!protected_ok)
    {
        result.ok = false;
        result.error = vm->last_error;
        logged = true;
    }
    if (!result.ok && !logged)
        mod_error(*vm, mod, result.error);
    return result;
}

void ModRuntime::Impl::advance_timers(Vm &vm, double seconds)
{
    vm.clock += seconds;
    if (vm.timers.empty())
        return;
    std::vector<std::pair<double, std::uint64_t>> due;
    for (const Timer &timer : vm.timers)
        if (timer.due <= vm.clock)
            due.emplace_back(timer.due, timer.id);
    std::sort(due.begin(), due.end());
    for (const auto &[when, id] : due)
    {
        const auto found = std::find_if(vm.timers.begin(), vm.timers.end(),
                                        [&](const Timer &timer) { return timer.id == id; });
        if (found == vm.timers.end())
            continue;
        const Callback callback = found->callback;
        const bool repeating = found->interval > 0.0;
        if (repeating)
        {
            found->due += found->interval;
            if (found->due <= vm.clock)
                found->due = vm.clock + found->interval;
        }
        else
            vm.timers.erase(found);
        invoke(callback, {}, vm.acting_player);
        if (!repeating)
            release(callback);
    }
}

// ---------------------------------------------------------------------------
// ModRuntime
// ---------------------------------------------------------------------------

ModRuntime::ModRuntime(RuntimeLimits limits) : impl_{std::make_unique<Impl>(*this, limits)} {}

ModRuntime::~ModRuntime()
{
    // While impl_ is whole: teardown listeners may still call back in.
    impl_->stop(Side::server);
    impl_->stop(Side::client);
}

ApiRegistry &ModRuntime::registry() noexcept
{
    return impl_->registry;
}

const ApiRegistry &ModRuntime::registry() const noexcept
{
    return impl_->registry;
}

void ModRuntime::set_log_sink(std::function<void(const LogEntry &)> sink)
{
    impl_->sink = std::move(sink);
}

const std::deque<LogEntry> &ModRuntime::recent_log() const noexcept
{
    return impl_->history;
}

void ModRuntime::clear_log() noexcept
{
    impl_->history.clear();
}

void ModRuntime::set_player_key(std::function<std::optional<std::string>(Handle)> resolver)
{
    impl_->player_key = std::move(resolver);
}

bool ModRuntime::start(Side side, std::vector<ModSource> mods, const SessionInfo &info)
{
    return impl_->start(side, std::move(mods), info);
}

void ModRuntime::world_loaded(Side side, const Value &payload)
{
    if (!running(side))
        return;
    impl_->world_payload[side == Side::server ? 1 : 0] = payload;
    impl_->emit(side, "world.loaded", payload);
}

void ModRuntime::stop(Side side)
{
    impl_->stop(side);
}

void ModRuntime::world_unloaded(Side side)
{
    std::optional<Value> &payload = impl_->world_payload[side == Side::server ? 1 : 0];
    if (!running(side) || !payload.has_value())
        return;
    payload.reset();
    impl_->emit(side, "world.unloading", Map{{"side", side_name(side)}});
    impl_->persist_storage();
}

void ModRuntime::forget_session_storage()
{
    impl_->persist_storage();
    // Saved (or logged as unsaved) just above.
    std::erase_if(impl_->storage, [](const auto &entry) {
        return entry.first.ends_with("|world") || entry.first.find("|player|") != std::string::npos;
    });
}

void ModRuntime::stop_all()
{
    impl_->stop(Side::server);
    impl_->stop(Side::client);
    if (any_running())
        return; // a script is on the stack; stop() refused and logged it
    forget_session_storage();
}

bool ModRuntime::reload(Side side, std::vector<ModSource> mods)
{
    const std::optional<Value> payload = impl_->world_payload[side == Side::server ? 1 : 0];
    const SessionInfo info = impl_->vm_for(side) != nullptr ? impl_->vm_for(side)->session
                                                            : impl_->session;
    impl_->stop(side);
    if (!impl_->start(side, std::move(mods), info))
        return false;
    if (payload)
        world_loaded(side, *payload);
    return true;
}

bool ModRuntime::attach(Side side, ModSource mod)
{
    Vm *vm = impl_->vm_for(side);
    if (vm == nullptr || !runs_on(mod.scope, side))
        return false;
    return impl_->attach(*vm, std::move(mod), true);
}

bool ModRuntime::running(Side side) const noexcept
{
    return impl_->vm_for(side) != nullptr;
}

bool ModRuntime::any_running() const noexcept
{
    return running(Side::server) || running(Side::client);
}

void ModRuntime::tick(double seconds)
{
    Vm *vm = impl_->vm_for(Side::server);
    if (vm == nullptr)
        return;
    impl_->advance_timers(*vm, seconds);
    ++vm->ticks;
    if (const auto found = vm->subscriptions.find("tick");
        found != vm->subscriptions.end() && !found->second.empty())
        impl_->emit(Side::server, "tick",
                    Map{{"dt", seconds}, {"tick", static_cast<std::int64_t>(vm->ticks)}});
}

void ModRuntime::frame(double seconds)
{
    Vm *vm = impl_->vm_for(Side::client);
    if (vm == nullptr)
        return;
    impl_->advance_timers(*vm, seconds);
    ++vm->ticks;
    if (const auto found = vm->subscriptions.find("frame");
        found != vm->subscriptions.end() && !found->second.empty())
        impl_->emit(Side::client, "frame", Map{{"dt", seconds}});
}

void ModRuntime::persist()
{
    impl_->persist_storage();
}

RunResult ModRuntime::run(Side side, std::string_view mod_id, std::string_view chunk_name,
                          std::string_view source)
{
    return impl_->run(side, mod_id, chunk_name, source);
}

std::vector<ModStatus> ModRuntime::status() const
{
    std::vector<ModStatus> result;
    for (Side side : {Side::server, Side::client})
    {
        const Vm *vm = impl_->vm_for(side);
        if (vm == nullptr)
            continue;
        for (const auto &slot : vm->mods)
        {
            ModStatus status;
            status.id = slot->source.id;
            status.side = side;
            status.state = slot->state;
            status.error = slot->error;
            status.memory_bytes = lua_totalbytes(vm->L, slot->memory_category);
            status.errors = slot->errors;
            for (const auto &[name, handlers] : vm->subscriptions)
                status.handlers += static_cast<std::uint32_t>(std::count_if(
                    handlers.begin(), handlers.end(),
                    [&](const auto &handler) { return handler->mod == slot->index; }));
            status.timers = static_cast<std::uint32_t>(
                std::count_if(vm->timers.begin(), vm->timers.end(),
                              [&](const Timer &timer) { return timer.mod == slot->index; }));
            result.push_back(std::move(status));
        }
    }
    return result;
}

bool ModRuntime::emit(Side side, std::string_view event, const Value &payload)
{
    return impl_->emit(side, event, payload);
}

Value ModRuntime::invoke(const Callback &callback, std::span<const Value> args)
{
    const Vm *vm = impl_->vm_by_id(callback.vm);
    return impl_->invoke(callback, args, vm != nullptr ? vm->acting_player : Handle{});
}

Value ModRuntime::invoke_as(Handle acting_player, const Callback &callback,
                            std::span<const Value> args)
{
    return impl_->invoke(callback, args, acting_player);
}

void ModRuntime::release(const Callback &callback) noexcept
{
    impl_->release(callback);
}

std::string_view ModRuntime::owner_of(const Callback &callback) const noexcept
{
    const Vm *vm = impl_->vm_by_id(callback.vm);
    if (vm == nullptr)
        return {};
    const auto hold = vm->holds.find(callback.ref);
    return hold == vm->holds.end() ? std::string_view{}
                                   : std::string_view{vm->mods[hold->second.mod]->source.id};
}

void ModRuntime::log(std::string_view mod_id, LogLevel level, std::string_view message)
{
    // A service logging for a mod: attribute it to the side running now, else
    // the side the mod is loaded on.
    Side side = Side::client;
    const std::string key = lowercase_ascii(mod_id);
    for (Side candidate : {Side::server, Side::client})
        if (const Vm *vm = impl_->vm_for(candidate);
            vm != nullptr && (vm->depth > 0 || vm->mod_lookup.contains(key)))
        {
            side = candidate;
            if (vm->depth > 0)
                break;
        }
    impl_->log(mod_id, side, level, message);
}

void ModRuntime::add_teardown_listener(std::function<void(Side)> listener)
{
    impl_->teardown_listeners.push_back(std::move(listener));
}

} // namespace openminerz::modding
