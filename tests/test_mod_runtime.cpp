// The modding runtime: Value <-> Luau marshalling, capability and side
// filtering, the sandbox, budgets, events, require, lifecycle, storage and the
// core libraries. Everything runs in memory or in temporary folders.

#include "modding_test_harness.hpp"

#include "openminerz/modding/package.hpp"
#include "openminerz/net/mod_download.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace openminerz;
using namespace openminerz::modding;
using modding::testing::Harness;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check_ok(const RunResult &result, const std::string &what)
{
    if (!result.ok)
        throw std::runtime_error(what + ": " + result.error);
}

void check_fails(const RunResult &result, std::string_view needle, const std::string &what)
{
    if (result.ok)
        throw std::runtime_error(what + ": expected an error");
    if (result.error.find(needle) == std::string::npos)
        throw std::runtime_error(what + ": error was \"" + result.error + "\", expected \"" +
                                 std::string{needle} + "\"");
}

// A folder private to one test, removed again when the test ends.
class TemporaryFolder final
{
  public:
    explicit TemporaryFolder(std::string_view name)
        : path_(std::filesystem::temp_directory_path() /
                ("openminerz-mod-runtime-" + std::string{name}))
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryFolder(const TemporaryFolder &) = delete;
    TemporaryFolder &operator=(const TemporaryFolder &) = delete;
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

// probe.echo / probe.keep / probe.handle: a tiny service for marshalling tests.
struct Probe final
{
    std::vector<Value> received;
    std::vector<Callback> kept;
    int calls{};

    void install(ApiRegistry &registry)
    {
        registry.add_function({"probe.echo", "returns its argument", {{"value", ValueType::any, true, ""}},
                               ValueType::any},
                              [this](CallContext &, std::span<const Value> args) {
                                  ++calls;
                                  received.push_back(args.empty() ? Value{} : args[0]);
                                  return args.empty() ? Value{} : args[0];
                              });
        registry.add_function({"probe.keep", "keeps a callback", {{"fn", ValueType::callback, false, ""}}},
                              [this](CallContext &, std::span<const Value> args) {
                                  kept.push_back(args[0].as_callback());
                                  return Value{};
                              });
        registry.add_function({"probe.handle", "makes a handle", {}, ValueType::handle},
                              [](CallContext &, std::span<const Value>) {
                                  return Value{Handle{HandleKind::entity, 77U}};
                              });
        registry.add_function({"probe.where", "takes a vector", {{"at", ValueType::vec3, false, ""}},
                               ValueType::vec3},
                              [](CallContext &, std::span<const Value> args) { return args[0]; });
        registry.add_function({"probe.count", "takes an integer", {{"n", ValueType::integer, false, ""}},
                               ValueType::integer},
                              [](CallContext &, std::span<const Value> args) {
                                  return Value{args[0].as_integer() + 1};
                              });
        registry.add_function({"probe.fail", "always refuses", {}},
                              [](CallContext &, std::span<const Value>) -> Value {
                                  throw ScriptError{"refused on purpose"};
                              });
    }
};

void every_value_type_round_trips()
{
    Harness harness;
    Probe probe;
    probe.install(harness.registry());

    const RunResult result = harness.run(R"(
        local v = vector.create(1.5, -2, 3)
        local h = probe.handle()
        assert(h == probe.handle(), "the same handle is the same value")
        assert(rawequal(h, probe.handle()), "handles are interned")
        assert(typeof(h) == "Handle" and h.kind == "entity" and h.id == 77)
        local back = probe.echo({
            n = 1.25, i = 3, big = 2^60, s = "text\0with nul", b = false, v = v,
            list = {1, "two", {3}}, h = h, empty = {}, nested = {deeper = {x = 1}},
        })
        assert(back.n == 1.25 and back.i == 3 and back.big == 2^60)
        assert(back.s == "text\0with nul" and back.b == false)
        assert(back.v == v and back.v.x == 1.5 and back.v.y == -2)
        assert(back.list[2] == "two" and back.list[3][1] == 3)
        assert(back.h == h and next(back.empty) == nil and back.nested.deeper.x == 1)
        assert(probe.echo(nil) == nil and probe.echo(true) == true)
        return back
    )");
    check_ok(result, "round trip");
    const Value &sent = probe.received.at(0);
    check(sent.type() == ValueType::map, "a record arrives as a map");
    const Map &map = sent.as_map();
    check(map.front().first == "b" && map.back().first == "v", "map keys arrive sorted");
    check(sent.field("n")->type() == ValueType::number, "1.25 is a number");
    check(sent.field("i")->type() == ValueType::integer && sent.field("i")->as_integer() == 3,
          "3 is an integer");
    check(sent.field("big")->type() == ValueType::number, "2^60 stays a number (not exact)");
    check(sent.field("s")->as_string() == std::string("text\0with nul", 13U), "binary strings survive");
    check(sent.field("v")->as_vec3() == Vec3{1.5, -2.0, 3.0}, "vector -> vec3");
    check(sent.field("h")->as_handle() == Handle{HandleKind::entity, 77U}, "handle -> handle");
    check(sent.field("list")->type() == ValueType::array && sent.field("list")->as_array().size() == 3U,
          "a list arrives as an array");
    check(sent.field("empty")->type() == ValueType::map && sent.field("empty")->as_array().empty(),
          "an empty table is an empty map and an empty array");
    check(result.value == sent, "the returned value equals what C++ saw");

    // C++ -> Luau for every type, read back by the script.
    harness.registry().add_function(
        {"probe.all", "every type", {}, ValueType::map},
        [](CallContext &, std::span<const Value>) {
            return Value{Map{{"nil", Value{}},
                             {"yes", true},
                             {"int", std::int64_t{-5}},
                             {"num", 0.5},
                             {"str", "s"},
                             {"vec", Vec3{1, 2, 3}},
                             {"handle", Handle{HandleKind::player, 3U}},
                             {"array", Array{1, 2}},
                             {"map", Map{{"k", "v"}}}}};
        });
    harness.clear_logs();
    // A second mod sees the function registered after the first VM was built.
    const RunResult typed = harness.run(R"(
        local all = probe.all()
        assert(all["nil"] == nil and all.yes == true and all.int == -5 and all.num == 0.5)
        assert(all.str == "s" and typeof(all.vec) == "vector" and all.vec.z == 3)
        assert(all.handle.kind == "player" and all.handle.id == 3)
        assert(#all.array == 2 and all.map.k == "v")
        return true
    )",
                                        Side::server, std::nullopt, "second");
    check_ok(typed, "C++ to Luau");

    // Mixed tables and coroutines cannot cross; {x,y,z} coerces to a vector.
    check_fails(harness.run("return probe.echo({1, 2, x = 3})"), "not a mix", "mixed table");
    check_fails(harness.run("return probe.echo(coroutine.create(print))"), "cannot cross",
                "coroutine");
    const RunResult coerced = harness.run("return probe.where({x = 1, y = 2, z = 3})");
    check_ok(coerced, "vector coercion");
    check(coerced.value.as_vec3() == Vec3{1, 2, 3}, "a {x,y,z} table became a vector");
    check_fails(harness.run("return probe.where(5)"), "must be a vector", "wrong type");
    check_fails(harness.run("return probe.count(1.5)"), "whole number", "integer parameter");
    check_fails(harness.run("return probe.count()"), "is required", "missing argument");
    check_fails(harness.run("return probe.count(1, 2)"), "at most 1", "extra argument");
    check(harness.run("return probe.count(41)").value == Value{42}, "integer argument");
    check_fails(harness.run("probe.fail()"), "test/chunk:1: probe.fail: refused on purpose",
                "service errors carry the mod, file and line");
}

void callbacks_are_held_invoked_and_released()
{
    Harness harness;
    Probe probe;
    probe.install(harness.registry());
    check_ok(harness.run(R"(
        local seen = 0
        probe.keep(function(n) seen += n; return n * 2 end)
        probe.keep(function() error("boom") end)
    )"),
             "keep");
    check(probe.kept.size() == 2U, "two callbacks kept");
    const Value doubled = harness.host().invoke(probe.kept[0], std::vector<Value>{Value{21}});
    check(doubled == Value{42}, "invoke returns the result");
    check(harness.host().owner_of(probe.kept[0]) == "test", "owner_of names the mod");
    harness.clear_logs();
    check(harness.host().invoke(probe.kept[1], {}).is_nil(), "an erroring callback yields nil");
    check(harness.logged("boom"), "and its error is logged");
    harness.host().release(probe.kept[0]);
    harness.host().release(probe.kept[0]);
    check(harness.host().invoke(probe.kept[0], {}).is_nil(), "a released callback is inert");

    bool torn_down{};
    harness.host().add_teardown_listener([&](Side side) { torn_down = side == Side::server; });
    harness.runtime().stop(Side::server);
    check(torn_down, "teardown listeners run when a VM stops");
    check(harness.host().invoke(probe.kept[1], {}).is_nil(), "callbacks die with their VM");
    check(harness.host().owner_of(probe.kept[1]).empty(), "and have no owner any more");
    harness.host().release(probe.kept[1]);
}

void capabilities_and_sides_are_enforced()
{
    Harness harness;
    int gameplay_calls{};
    harness.registry().add_function({"world.break_block", "server gameplay", {}, ValueType::nil, {},
                                     side_server, capability::gameplay},
                                    [&](CallContext &, std::span<const Value>) {
                                        ++gameplay_calls;
                                        return Value{};
                                    });
    harness.registry().add_function({"ui.toast", "client presentation", {}, ValueType::nil, {},
                                     side_client, capability::presentation},
                                    [](CallContext &, std::span<const Value>) { return Value{}; });
    harness.registry().add_function(
        {"net.send", "network", {}, ValueType::string, {}, side_both, capability::network},
        [](CallContext &context, std::span<const Value>) {
            return Value{std::string{context.mod_id}};
        });

    const auto client_only = modding::testing::client_mod_capabilities();
    check_fails(harness.run("world.break_block()", Side::client, client_only, "pack"),
                "only available on the server", "client mod calling server gameplay");
    check_fails(harness.run("net.send()", Side::client, client_only, "pack"),
                "needs the network capability", "client mod calling network");
    check_ok(harness.run("ui.toast()", Side::client, client_only, "pack"), "client mod UI");
    check_ok(harness.run("assert(not mod.has('net.send') and mod.has('ui.toast'))", Side::client,
                         client_only, "pack"),
             "mod.has reflects capabilities");
    check(harness.run("return net.send()", Side::client, std::nullopt, "gameplay").value ==
              Value{"gameplay"},
          "a server mod's client VM has network, and the context names the caller");

    check_fails(harness.run("world.break_block()", Side::server, capability::query, "reader"),
                "needs the gameplay capability", "missing capability on the server");
    check_ok(harness.run("world.break_block()", Side::server), "a server mod may");
    check(gameplay_calls == 1, "refused calls never reach the service");
    check_fails(harness.run("ui.toast()", Side::server), "only available on the client",
                "UI is client-only");
}

void the_sandbox_refuses_escapes()
{
    Harness harness;
    check_ok(harness.run(R"(
        assert(io == nil and os == nil and debug == nil)
        assert(getfenv == nil and setfenv == nil and loadstring == nil and dofile == nil)
        assert(type(require) == "function" and type(print) == "function")
        assert(not pcall(function() string.upper = nil end), "builtins are readonly")
        assert(not pcall(function() math.pi = 3 end))
        assert(not pcall(function() events.on = nil end), "libraries are readonly")
        assert(not pcall(function() _G.leak = 1 end), "the shared globals are readonly")
        assert(not pcall(function() getmetatable("").__index = {} end))
        assert(not pcall(setmetatable, events, {}))
        secret = "mine"  -- a file global, private to this file
    )"),
             "sandbox");
    check_ok(harness.run("assert(secret == nil, 'another chunk does not see it')", Side::server,
                         std::nullopt, "other"),
             "globals are per file");

    // Bombs are refused by the memory cap and the game keeps going.
    harness.clear_logs();
    check_fails(harness.run("local s = string.rep('x', 2^30)"), "memory", "string.rep bomb");
    check_fails(harness.run("local s = string.rep('abcdefgh', 2^20) .. string.rep('x', 2^25)"),
                "memory", "concatenation bomb");
    check(harness.logged("memory"), "the bomb is reported as a memory error");
    check_fails(harness.run("local t = {} for i = 1, 1e9 do t[i] = {i} end"), "memory",
                "table growth bomb");
    check_ok(harness.run("local t = {} for i = 1, 1000 do t[i] = i end return #t"),
             "the VM still works after running out of memory");
    check_fails(harness.run("local f; f = function() return f() + 1 end; f()"), "stack",
                "unbounded recursion");
}

ModStatus status_of(Harness &harness, std::string_view id)
{
    for (const ModStatus &status : harness.runtime().status())
        if (status.id == id)
            return status;
    throw std::runtime_error(std::string{id} + " is not loaded");
}

std::size_t warnings_about(const Harness &harness, std::string_view text)
{
    std::size_t count{};
    for (const LogEntry &entry : harness.logs())
        if (entry.level == LogLevel::warning && entry.message.find(text) != std::string::npos)
            ++count;
    return count;
}

void runaway_scripts_are_stopped()
{
    RuntimeLimits limits = modding::testing::test_limits();
    limits.call_budget = std::chrono::milliseconds{50};
    Harness harness{limits};
    const auto started = std::chrono::steady_clock::now();
    check_fails(harness.run("while true do end"), "longer than its 50 ms budget", "tight loop");
    check_fails(harness.run(R"(
        while true do pcall(function() while true do end end) end
    )",
                            Side::server, std::nullopt, "sneaky"),
                "budget", "pcall cannot swallow the budget");
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds{5},
          "both were aborted promptly");
    // (Polish) One overrun aborts that call only, with a warning naming the
    // mod, file and line; it is not a script error.
    check(status_of(harness, "test").state == ModState::running &&
              status_of(harness, "sneaky").state == ModState::running,
          "an overrun aborts the call, not the mod");
    check(warnings_about(harness, "test/chunk:1: call aborted") == 1U,
          "the overrun is a warning with the mod, file and line");
    check(harness.errors().empty(), "and no error: " + harness.errors());
    check_ok(harness.run("return 1"), "the mod still runs after an overrun");

    // A handler that overruns twice keeps its mod; the other mod keeps ticking.
    harness.add_file("server/main.luau", R"(calls = 0
events.on('tick', function()
    calls += 1
    if calls <= 2 then while true do end end
    print("calls=" .. calls)
end)
)",
                     "twice");
    check_ok(harness.run("return 0", Side::server, std::nullopt, "twice"), "twice setup");
    check_ok(harness.run("ticks = 0 events.on('tick', function() ticks += 1 end)", Side::server,
                         std::nullopt, "steady"),
             "steady setup");
    for (int tick{}; tick < 4; ++tick)
        harness.runtime().tick(0.05);
    check(status_of(harness, "twice").state == ModState::running, "two overruns keep the mod");
    check(harness.logged("calls=4"), "its handler still runs after them");
    check(warnings_about(harness, "twice/server/main.luau:4: call aborted") == 2U,
          "each overrun names the file and line of the loop");

    // A handler that overruns on every call is stopped by its third overrun.
    check_ok(harness.run("events.on('tick', function() while true do end end)", Side::server,
                         std::nullopt, "hang"),
             "hang setup");
    harness.runtime().tick(0.05);
    harness.runtime().tick(0.05);
    check(status_of(harness, "hang").state == ModState::running, "two overruns are forgiven");
    harness.runtime().tick(0.05);
    const ModStatus hang = status_of(harness, "hang");
    check(hang.state == ModState::stopped && hang.handlers == 0U,
          "the third overrun within the window stops the mod and drops its handlers");
    check(harness.logged("hang stopped on the server") && harness.logged("3 times within 60 s"),
          "and that is logged");
    check(harness.run("return ticks", Side::server, std::nullopt, "steady").value == Value{7},
          "the other mod is still running");
}

// (Polish) Overruns are counted in a sliding window; one call that keeps the
// game waiting the hard limit stops its mod at once.
void overruns_are_forgiven_over_time()
{
    RuntimeLimits limits = modding::testing::test_limits();
    limits.call_budget = std::chrono::milliseconds{30};
    limits.overrun_window = std::chrono::milliseconds{400};
    {
        Harness harness{limits};
        check_ok(harness.run("events.on('tick', function() while true do end end)", Side::server,
                             std::nullopt, "slow"),
                 "slow setup");
        harness.runtime().tick(0.05);
        harness.runtime().tick(0.05);
        std::this_thread::sleep_for(std::chrono::milliseconds{450});
        harness.runtime().tick(0.05);
        harness.runtime().tick(0.05);
        check(status_of(harness, "slow").state == ModState::running,
              "overruns older than the window are forgotten");
        harness.runtime().tick(0.05);
        check(status_of(harness, "slow").state == ModState::stopped,
              "three inside the window still stop it");
    }

    // A loop that runs past the hard limit is stopped on its first call. With
    // the default limits that takes a budget of at least the hard limit (a
    // file loading), or a built-in the abort cannot interrupt in time.
    limits.call_budget = std::chrono::milliseconds{250};
    limits.hard_overrun = std::chrono::milliseconds{200};
    Harness harness{limits};
    check_ok(harness.run("events.on('tick', function() while true do end end)", Side::server,
                         std::nullopt, "forever"),
             "forever setup");
    harness.runtime().tick(0.05);
    const ModStatus forever = status_of(harness, "forever");
    check(forever.state == ModState::stopped && forever.handlers == 0U,
          "a hard overrun stops the mod at once");
    check(forever.error.find("kept the game waiting") != std::string::npos,
          "and says why: " + forever.error);

    // With the shipped limits a loop on every tick is gone by the third tick.
    Harness shipped{RuntimeLimits{}};
    check_ok(shipped.run("events.on('tick', function() while true do end end)", Side::server,
                         std::nullopt, "forever"),
             "shipped setup");
    const auto started = std::chrono::steady_clock::now();
    for (int tick{}; tick < 3; ++tick)
        shipped.runtime().tick(0.05);
    check(status_of(shipped, "forever").state == ModState::stopped,
          "an endless loop is stopped within three calls");
    check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{1000},
          "having held the game for about three budgets");
}

#ifdef _WIN32
// (Polish) Time the game's thread does not run - a breakpoint, the OS
// suspending the process - is not the script's: a call that stalls through
// it is not an overrun.
void stalls_do_not_count_as_overruns()
{
    Harness harness; // 250 ms budget
    check_ok(harness.run(R"(
        function spin(n) local x = 0 for i = 1, n do x += i % 7 end return x end
    )"),
             "spin setup");
    // About 80 ms of script time on this machine.
    long long iterations = 1'000'000;
    for (;;)
    {
        const auto begun = std::chrono::steady_clock::now();
        check_ok(harness.run(std::format("return spin({})", iterations)), "calibration");
        const auto took = std::chrono::steady_clock::now() - begun;
        if (took >= std::chrono::milliseconds{20} || iterations >= 4'000'000'000LL)
        {
            iterations = std::max(1LL, static_cast<long long>(static_cast<double>(iterations) *
                                                              80.0 / std::chrono::duration<double, std::milli>(took).count()));
            break;
        }
        iterations *= 4;
    }

    HANDLE self = OpenThread(THREAD_SUSPEND_RESUME, FALSE, GetCurrentThreadId());
    check(self != nullptr, "the test can suspend its own thread");
    // Suspends this thread 20 ms into the call, for 600 ms, as a debugger
    // breakpoint or a suspended laptop would.
    std::thread stall{[self] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        SuspendThread(self);
        std::this_thread::sleep_for(std::chrono::milliseconds{600});
        ResumeThread(self);
    }};
    const auto begun = std::chrono::steady_clock::now();
    const RunResult result = harness.run(std::format("return spin({})", iterations));
    const auto took = std::chrono::steady_clock::now() - begun;
    stall.join();
    CloseHandle(self);
    check_ok(result, "a call stalled from outside finishes");
    check(took >= std::chrono::milliseconds{550},
          std::format("the stall happened during the call ({} ms)",
                      std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
    check(warnings_about(harness, "call aborted") == 0U && harness.errors().empty(),
          "and it was not an overrun");
}
#endif

void events_follow_their_specs()
{
    Harness harness;
    harness.registry().add_event({"block.breaking", "", side_server, true, {}});
    check_ok(harness.run(R"(
        seen = {}
        events.on("block.breaking", function(e) table.insert(seen, "first:" .. e.x) end)
        events.once("block.breaking", function(e) table.insert(seen, "once") end)
    )",
                         Side::server, std::nullopt, "a"),
             "first mod");
    check_ok(harness.run(R"(
        events.on("block.breaking", function(e) return e.x ~= 13 end)
    )",
                         Side::server, std::nullopt, "b"),
             "second mod");
    check(harness.emit(Side::server, "block.breaking", Map{{"x", 1}}), "not cancelled");
    check(!harness.emit(Side::server, "block.breaking", Map{{"x", 13}}), "cancelled by b");
    const RunResult seen = harness.run("return table.concat(seen, ',')", Side::server,
                                       std::nullopt, "a");
    check(seen.value == Value{"first:1,once,first:13"},
          "handlers run in load order and once fires once: " + seen.value.as_string());

    check_fails(harness.run("events.on('no.such.event', print)"), "unknown event", "unknown event");
    check_fails(harness.run("events.on('frame', print)"), "only fires on the client",
                "client event on the server");
    check_ok(harness.run(R"(
        local function h() count = (count or 0) + 1 end
        events.on("a:ping", h)
        assert(events.off("a:ping", h) and not events.off("a:ping", h))
    )",
                         Side::server, std::nullopt, "a"),
             "off removes a handler");
    check_fails(harness.run("events.emit('a:ping')", Side::server, std::nullopt, "b"),
                "only its own events", "mods emit only their own events");
    check_ok(harness.run(R"(
        local got
        events.on("b:hello", function(p) got = p.text end)
        assert(events.emit("b:hello", {text = "hi"}) and got == "hi")
    )",
                         Side::server, std::nullopt, "b"),
             "custom events");
    check(harness.emit(Side::client, "block.breaking", {}), "no VM on a side means emit is true");
}

void require_stays_inside_the_mod()
{
    Harness harness;
    harness.add_file("shared/util.luau", "return { answer = 42 }", "lib");
    harness.add_file("shared/init.luau", "local u = require('./util') loaded_lib = u.answer", "lib");
    harness.add_file("server/secret.luau", "return 'server only'", "lib");
    harness.add_file("shared/cycle/a.luau", "return require('./b')", "lib");
    harness.add_file("shared/cycle/b.luau", "return require('./a')", "lib");
    check_ok(harness.run("assert(loaded_lib == nil) return require('shared/util').answer",
                         Side::server, std::nullopt, "lib"),
             "the library mod loads (file globals stay in their file)");
    check_fails(harness.run("return require('shared/cycle/a')", Side::server, std::nullopt, "lib"),
                "require cycle", "a require cycle is an error, not a hang");
    check_fails(harness.run("return require('shared/cycle/a')", Side::server, std::nullopt, "lib"),
                "require cycle", "and stays one");
    harness.clear_logs();
    harness.add_file("shared/main.luau", "return true", "user");
    check_ok(harness.run("return true", Side::server, std::nullopt, "user"), "user loads");
    check_fails(harness.run("return require('@lib/shared/util')", Side::server, std::nullopt, "user"),
                "not a declared dependency", "undeclared cross-mod require");
    check_fails(harness.run("return require('../../etc/passwd')", Side::server, std::nullopt, "user"),
                "not a path inside the mod", "escaping the mod");
    check_fails(harness.run("return require('C:/Windows/win.ini')", Side::server, std::nullopt, "user"),
                "not a path inside the mod", "absolute path");
    check_fails(harness.run("return require('nothing')", Side::server, std::nullopt, "user"),
                "no module", "missing module");

    ModSource dependent;
    dependent.id = "dependent";
    dependent.scope = net::ModScope::server;
    dependent.dependencies = {"lib"};
    dependent.files["shared/init.luau"] = "value = require('@lib/shared/util').answer";
    check(harness.runtime().attach(Side::server, dependent), "a dependent mod loads");
    check(harness.run("return value", Side::server, std::nullopt, "dependent").ok,
          "and it ran");
    check(harness.run("return require('@lib/shared/util').answer", Side::server, std::nullopt,
                      "dependent")
                  .value == Value{42},
          "declared dependencies can be required");

    // Server code cannot be required on a client.
    harness.add_file("server/hidden.luau", "return 1", "split");
    harness.add_file("client/view.luau", "return 2", "split");
    check_fails(harness.run("return require('server/hidden')", Side::client, std::nullopt, "split"),
                "cannot be required on the client", "server code on the client");
    check(harness.run("return require('client/view')", Side::client, std::nullopt, "split").value ==
              Value{2},
          "client code on the client");
}

void dependencies_load_first()
{
    Harness harness;
    ModSource base;
    base.id = "base";
    base.scope = net::ModScope::server;
    base.files["shared/init.luau"] = "return true";
    ModSource addon;
    addon.id = "addon";
    addon.scope = net::ModScope::server;
    addon.dependencies = {"base"};
    addon.files["shared/init.luau"] = "return true";

    check(harness.runtime().start(Side::server, {addon, base}, {}), "the VM starts");
    for (const ModStatus &status : harness.runtime().status())
        if (status.id == "addon")
            check(status.state == ModState::failed && status.error.find("base") != std::string::npos,
                  "a mod listed before its dependency fails: " + status.error);
    harness.runtime().stop(Side::server);

    check(harness.runtime().start(Side::server, {base, addon}, {}), "the VM starts again");
    for (const ModStatus &status : harness.runtime().status())
        check(status.state == ModState::running, status.id + " runs in dependency order");
}

void lifecycle_events_and_scripts_run_in_order()
{
    Harness harness;
    ModSource mod;
    mod.id = "life";
    mod.scope = net::ModScope::server;
    mod.files["shared/a.luau"] = "table.insert(require('shared/lib/trace'), 'shared')";
    mod.files["shared/lib/trace.luau"] = "return {}";
    mod.files["server/b.luau"] = R"(
        local trace = require("shared/lib/trace")
        table.insert(trace, "server")
        events.on("mod.loaded", function(e) table.insert(trace, "loaded:" .. e.side) end)
        events.on("world.loaded", function(e) table.insert(trace, "world:" .. e.name) end)
        events.on("tick", function(e) table.insert(trace, "tick:" .. e.tick) end)
        events.on("world.unloading", function() storage.world.set("bye", true) end)
    )";
    mod.files["client/c.luau"] = "error('client code must not run on the server')";
    check(harness.runtime().start(Side::server, {mod}, {}), "starts");
    harness.runtime().world_loaded(Side::server, Map{{"name", "Test World"}});
    harness.runtime().tick(0.05);
    const RunResult trace = harness.run("return table.concat(require('shared/lib/trace'), ',')",
                                        Side::server, std::nullopt, "life");
    check(trace.value == Value{"shared,server,loaded:server,world:Test World,tick:1"},
          "lifecycle order: " + (trace.ok ? trace.value.as_string() : trace.error));
    check(harness.errors().empty(), "no errors: " + harness.errors());
}

void timers_follow_the_simulation()
{
    Harness harness;
    check_ok(harness.run(R"(
        fired = {}
        timer.after(0.1, function() table.insert(fired, "after") end)
        repeating = timer.every(0.05, function() table.insert(fired, "every") end)
        local cancelled = timer.after(0.05, function() table.insert(fired, "never") end)
        assert(timer.cancel(cancelled) and not timer.cancel(cancelled))
    )"),
             "schedule");
    for (int step{}; step < 3; ++step)
        harness.runtime().tick(0.05);
    check_ok(harness.run("timer.cancel(repeating)"), "cancel");
    harness.runtime().tick(0.05);
    const RunResult fired = harness.run("return table.concat(fired, ',')");
    check(fired.value == Value{"every,after,every,every"},
          "timers fire on ticks: " + fired.value.as_string());
    check_fails(harness.run("timer.every(0, print)"), "at least 0.001", "zero interval");
}

void storage_persists_across_reloads_and_sessions()
{
    TemporaryFolder folder{"storage"};
    SessionInfo info;
    info.world_dir = folder.path() / "world";
    info.local_dir = folder.path() / "local";
    {
        Harness harness;
        harness.set_session(info);
        check_ok(harness.run(R"(
            storage.world.set("quests", {done = {"intro"}, gold = 12})
            storage.world.set("spot", vector.create(1, 2, 3))
            storage.world.set("gone", 1)
            storage.world.set("gone", nil)
        )"),
                 "set");
        check_fails(harness.run("storage.world.set('f', print)"), "cannot be stored", "functions");
        check_fails(harness.run("storage.world.set('x', string.rep('a', 2 * 1024 * 1024))"),
                    "storage limit", "the size cap");
        check_fails(harness.run("storage.machine.get('x')"), "only available on the client",
                    "machine storage is client-side");

        // Developer reload: VM rebuilt, storage kept.
        ModSource mod;
        mod.id = "test";
        mod.scope = net::ModScope::server;
        check(harness.runtime().reload(Side::server, {mod}), "reload");
        const RunResult kept = harness.run("return storage.world.get('quests').gold");
        check(kept.value == Value{12}, "storage survives a reload");
        check_ok(harness.run("assert(#storage.world.keys() == 2)"), "keys");
    }
    const auto file = info.world_dir / "test" / "world.json";
    check(std::filesystem::exists(file), "storage was written to the world sidecar");
    {
        Harness harness;
        harness.set_session(info);
        const RunResult loaded = harness.run(R"(
            local q = storage.world.get("quests")
            local spot = storage.world.get("spot")
            return q.done[1] .. ":" .. q.gold .. ":" .. spot.z
        )");
        check(loaded.value == Value{"intro:12:3"}, "a new session reads it back: " +
                                                       (loaded.ok ? loaded.value.as_string() : loaded.error));

        // The same runtime entering another world sees that world's data only.
        harness.runtime().stop_all();
        SessionInfo other = info;
        other.world_dir = folder.path() / "other-world";
        ModSource mod;
        mod.id = "test";
        mod.scope = net::ModScope::server;
        check(harness.runtime().start(Side::server, {mod}, other), "another world");
        check(harness.run("return storage.world.get('quests')").value.is_nil(),
              "world storage does not leak between worlds");
    }
}

void settings_are_declared_and_changed()
{
    TemporaryFolder folder{"config"};
    SessionInfo info;
    info.local_dir = folder.path();
    Harness harness;
    harness.set_session(info);
    check_ok(harness.run(R"(
        local hunger = mod.config.define("hunger_rate", {
            type = "number", default = 1, min = 0, max = 5, label = "Hunger rate",
        })
        assert(hunger == 1)
        mod.config.define("difficulty", {type = "string", default = "normal",
                                         choices = {"easy", "normal", "hard"}})
        changes = {}
        events.on("config.changed", function(e) table.insert(changes, e.key .. "=" .. tostring(e.value)) end)
    )"),
             "define");
    const std::vector<ConfigSetting> settings = harness.runtime().config_settings();
    check(settings.size() == 2U && settings[0].label == "Hunger rate" && settings[0].maximum == 5.0,
          "definitions are exposed for a settings page");
    std::string error;
    check(!harness.runtime().set_config("test", "hunger_rate", Value{9}, error), "out of range refused");
    check(!harness.runtime().set_config("test", "difficulty", Value{"insane"}, error),
          "not a choice refused");
    check(harness.runtime().set_config("test", "hunger_rate", Value{2.5}, error), error);
    check(harness.run("return mod.config.get('hunger_rate')").value == Value{2.5}, "the new value");
    check(harness.run("return table.concat(changes, ',')").value == Value{"hunger_rate=2.5"},
          "config.changed fired");
    check(std::filesystem::exists(folder.path() / "test" / "config.json"), "settings are saved");
}

void util_is_deterministic()
{
    SessionInfo info;
    info.world_seed = 1234U;
    std::vector<Value> runs;
    for (int attempt{}; attempt < 2; ++attempt)
    {
        Harness harness;
        harness.set_session(info);
        runs.push_back(harness.run(R"(
            local out = {}
            for i = 1, 5 do table.insert(out, util.random(1, 100)) end
            local f = util.random()
            assert(f >= 0 and f < 1)
            return out
        )")
                           .value);
    }
    check(runs[0] == runs[1] && runs[0].as_array().size() == 5U,
          "the same world seed gives the same numbers");

    Harness harness;
    check_ok(harness.run(R"(
        local value = {name = "Bob", hp = 7.5, n = 3, tags = {"a", "b"}, at = vector.create(1, 2, 3),
                       ["$weird"] = true, text = "line\n\"quoted\""}
        local text = util.serialize(value)
        local back = util.deserialize(text)
        assert(back.name == "Bob" and back.hp == 7.5 and back.n == 3 and back.tags[2] == "b")
        assert(back.at == value.at and back["$weird"] == true and back.text == value.text)
        assert(util.deserialize('{"a": [1, 2.5e3, -4], "b": null}').a[2] == 2500)
        assert(not pcall(util.deserialize, '{"a": }'))
        assert(not pcall(util.serialize, {f = print}))
    )"),
             "serialize");
    std::string text;
    std::string error;
    check(serialize_value(Value{2.0}, text, error) && text == "2.0", "whole doubles stay doubles");
    Value back;
    check(deserialize_value(text, back, error) && back.type() == ValueType::number, "and read back");
}

void print_and_log_are_tagged()
{
    Harness harness;
    std::vector<LogEntry> sunk;
    harness.runtime().set_log_sink([&](const LogEntry &entry) { sunk.push_back(entry); });
    check_ok(harness.run("print('hello', 1, true) log.warn('careful', {a = 1}) log.debug(nil)"),
             "log");
    check(sunk.size() == 3U && sunk[0].message == "hello\t1\ttrue" && sunk[0].mod_id == "test",
          "print logs at info level with the mod id");
    check(sunk[1].level == LogLevel::warning && sunk[1].message == "careful {a = 1}",
          "log.warn formats values: " + sunk[1].message);
    check(sunk[2].message == "nil", "nil prints as nil");
    check_fails(harness.run("local x = nil + 1"), "test/chunk:1:", "errors carry file:line");
    check_fails(harness.run("local x = "), "test/chunk:1:", "syntax errors carry file:line");
    harness.add_file("shared/deep.luau", "return {fail = function() error('deep failure') end}",
                     "traced");
    check_fails(harness.run(R"(
        local deep = require("shared/deep")
        local function outer() deep.fail() end
        outer()
    )",
                            Side::server, std::nullopt, "traced"),
                "traced/shared/deep.luau:1: deep failure\n  at traced/shared/deep.luau:1",
                "errors carry a traceback");
}

void no_mods_means_no_vm()
{
    ModRuntime runtime;
    check(!runtime.start(Side::server, {}, {}), "no mods, no VM");
    ModSource pack;
    pack.id = "pack";
    pack.scope = net::ModScope::client;
    check(!runtime.start(Side::server, {pack}, {}), "a client mod never starts a server VM");
    check(!runtime.any_running(), "nothing runs");
    check(runtime.emit(Side::server, "tick", {}), "emit is a no-op");
    runtime.tick(0.05);
    runtime.frame(0.016);
    check(runtime.invoke(Callback{1U, 1U}, {}).is_nil(), "invoke is a no-op");
}

void performance_is_reported()
{
    Harness harness;
    harness.registry().add_function({"perf.noop", "", {{"n", ValueType::number, false, ""}},
                                     ValueType::number},
                                    [](CallContext &, std::span<const Value> args) { return args[0]; });
    check_ok(harness.run("events.on('tick', function(e) end)"), "handler");
    constexpr int ticks = 20000;
    const auto tick_start = std::chrono::steady_clock::now();
    for (int index{}; index < ticks; ++index)
        harness.runtime().tick(1.0 / 60.0);
    const double tick_us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tick_start)
            .count() /
        ticks;

    const RunResult loop = harness.run(R"(
        local t0 = os == nil and 0
        for round = 1, 20 do
            for i = 1, 1000 do perf.noop(i) end
        end
        return true
    )");
    check_ok(loop, "loop");
    const auto loop_start = std::chrono::steady_clock::now();
    check_ok(harness.run("for i = 1, 1000 do perf.noop(i) end"), "1000 calls");
    const double loop_us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - loop_start)
            .count();
    std::cout << std::format("[perf] empty tick handler: {:.2f} us per tick; 1000 native calls: "
                             "{:.1f} us ({:.0f} ns per call)\n",
                             tick_us, loop_us, loop_us);
    check(tick_us < 200.0, "an empty tick handler stays cheap");
}

void handles_work_as_keys_and_acting_player_is_passed()
{
    Harness harness;
    Probe probe;
    probe.install(harness.registry());
    Handle seen_player;
    harness.registry().add_function({"probe.actor", "who acts", {}, ValueType::handle},
                                    [&](CallContext &context, std::span<const Value>) {
                                        seen_player = context.acting_player;
                                        return Value{context.acting_player};
                                    });
    check_ok(harness.run(R"lua(
        local scores = {}
        scores[probe.handle()] = 5
        scores[probe.handle()] += 1
        assert(scores[probe.handle()] == 6, "a handle is a stable table key")
        assert(tostring(probe.handle()) == "Handle(entity 77)")
        assert(not pcall(function() return probe.handle().nope end))
        probe.keep(function() return probe.actor() end)
    )lua"),
             "handles");
    const Handle player{HandleKind::player, 4U};
    const Value acted = harness.host().invoke_as(player, probe.kept.at(0), {});
    check(acted == Value{player} && seen_player == player, "invoke_as reaches CallContext");
    check(harness.host().invoke(probe.kept.at(0), {}) == Value{Handle{}},
          "a plain invoke has no acting player");
}

void natives_work_from_coroutines_and_nested_calls()
{
    Harness harness;
    Probe probe;
    probe.install(harness.registry());
    // A service that calls back into scripts while a script is calling it.
    harness.registry().add_function(
        {"probe.call_now", "invokes its argument", {{"fn", ValueType::callback, false, ""}},
         ValueType::any},
        [&](CallContext &, std::span<const Value> args) {
            const Callback fn = args[0].as_callback();
            const Value result = harness.host().invoke(fn, std::vector<Value>{Value{10}});
            harness.host().release(fn);
            return result;
        });
    check_ok(harness.run(R"(
        assert(probe.call_now(function(n) return probe.echo(n) * 2 end) == 20)
        local co = coroutine.wrap(function()
            local value = probe.call_now(function(n) return n + 1 end)
            coroutine.yield(value)
            return probe.echo("after yield")
        end)
        assert(co() == 11 and co() == "after yield")
        events.on("test:inner", function(p) inner = p.n end)
        probe.call_now(function() events.emit("test:inner", {n = 3}) end)
        assert(inner == 3)
    )"),
             "nested calls");
    // The callback's error is logged against it; the caller carries on.
    harness.clear_logs();
    check_ok(harness.run("probe.call_now(function() coroutine.yield() end)"), "yield inside");
    check(harness.logged("yield"), "yielding across a service call is refused, not a crash");
}

// --- Packages ---------------------------------------------------------------

void write_file(const std::filesystem::path &path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path, std::ios::binary} << text;
}

std::string hex(const std::array<std::uint8_t, 32U> &digest)
{
    std::string text;
    for (const std::uint8_t byte : digest)
        text += std::format("{:02x}", byte);
    return text;
}

void descriptors_stay_compatible()
{
    TemporaryFolder folder{"descriptors"};
    // The five-key descriptor every existing add-on uses: the content digest is
    // pinned (computed independently of the C++ code), so it cannot drift.
    write_file(folder.path() / "pinned" / "openminerz.mod",
               "id=demo.pinned\nversion=1.0\nscope=server\nprotocol=both\norder=5\n");
    write_file(folder.path() / "pinned" / "shared" / "init.luau", "return true\n");
    net::ModCatalog catalog;
    std::string error;
    check(net::load_mod_catalog(folder.path(), catalog, error), error);
    check(catalog.mods.size() == 1U && catalog.mods[0].metadata.empty(), "five keys, no metadata");
    check(hex(catalog.mods[0].descriptor.content_digest) ==
              "8c560e7b8c93cd933f8997f5ba905479d350d6f90130826e966b57b54c13e0a3",
          "the content digest of an existing descriptor is unchanged: " +
              hex(catalog.mods[0].descriptor.content_digest));

    // Optional keys load; the admission descriptor is the same five fields.
    write_file(folder.path() / "rich" / "openminerz.mod",
               "id=demo.rich\nversion=2.0\nscope=core\nprotocol=both\norder=6\n"
               "name=Rich Demo\nauthor=Someone\ndescription=Shows every key\n"
               "website=https://example.invalid\napi=1\ndepends=demo.pinned@1.0\n");
    check(net::load_mod_catalog(folder.path(), catalog, error), error);
    PackageInfo info;
    check(parse_package_info(catalog.mods[1], info, error), error);
    check(info.name == "Rich Demo" && info.author == "Someone" && info.api == 1U &&
              info.depends.size() == 1U && info.depends[0].id == "demo.pinned" &&
              info.depends[0].minimum_version == "1.0",
          "optional keys are parsed");

    write_file(folder.path() / "typo" / "openminerz.mod",
               "id=demo.typo\nversion=1\nscope=client\nprotocol=both\norder=1\nnmae=Oops\n");
    check(!net::load_mod_catalog(folder.path(), catalog, error), "unknown keys are still refused");
    std::filesystem::remove_all(folder.path() / "typo");
    write_file(folder.path() / "short" / "openminerz.mod",
               "id=demo.short\nversion=1\nscope=client\nprotocol=both\nname=No order\n");
    check(!net::load_mod_catalog(folder.path(), catalog, error),
          "the five keys stay required even with optional ones");
}

// (Polish) .git, .vscode, .luaurc, Thumbs.db and desktop.ini are not part of
// a mod: no refusal, and every digest is what it is without them.
void editor_files_are_not_part_of_a_mod()
{
    TemporaryFolder folder{"editor-files"};
    const std::filesystem::path root = folder.path() / "pinned";
    write_file(root / "openminerz.mod", "id=pinned\nversion=1.0\nscope=server\nprotocol=both\norder=0\n");
    write_file(root / "shared" / "main.luau", "print('pinned')\n");
    write_file(root / "server" / "rules.luau", "events.on('tick', function() end)\n");
    write_file(root / "assets" / "textures" / "blocks" / "3.png", "not really a png\n");
    write_file(root / "lang" / "en.lang", "greeting=Hello\n");
    // Digests of this exact folder from the SDK built before the change: a
    // mod without such entries keeps its digests byte for byte.
    constexpr std::string_view content = "63cf61d70a7a6a289a713b3e03a059995c37620d2ddc4cf553fab5acba86f6c9";
    constexpr std::string_view client = "2b19dc834629ed734572df06271be59cfc9f7ff62e9113b0adf04ae797f441b2";
    const auto digests = [&](const std::string &what) {
        net::ModCatalog catalog;
        std::string error;
        check(net::load_mod_catalog(folder.path(), catalog, error) && catalog.mods.size() == 1U,
              what + ": the catalog loads: " + error);
        check(hex(catalog.mods[0].descriptor.content_digest) == content,
              what + ": content digest " + hex(catalog.mods[0].descriptor.content_digest));
        net::ModDigest visible{};
        std::uint64_t bytes{};
        check(net::client_visible_digest(root, visible, bytes, error) && hex(visible) == client,
              what + ": client digest " + hex(visible));
        net::ModArchive archive;
        check(net::build_client_archive(root, 1U << 20U, archive, error) && hex(archive.client_digest) == client,
              what + ": the download archive matches: " + error);
        return catalog;
    };
    digests("a clean folder");

    write_file(root / ".git" / "config", "[core]\n");
    write_file(root / ".git" / "objects" / "ab", "x");
    write_file(root / ".vscode" / "settings.json", "{}");
    write_file(root / ".luaurc", "{\"languageMode\":\"strict\"}");
    write_file(root / "desktop.ini", "[.ShellClassInfo]\n");
    write_file(root / "assets" / "textures" / "Thumbs.db", "thumb");
    write_file(root / "assets" / ".cache" / "blob.png", "x");
    write_file(root / "shared" / ".notes.luau", "this is not luau");
    write_file(folder.path() / ".git" / "HEAD", "ref: refs/heads/main\n"); // a repository of mods
    const net::ModCatalog catalog = digests("with editor files");

    std::vector<net::ModContentFile> files;
    std::string error;
    check(net::list_mod_content(root, files, error) && files.size() == 5U,
          std::format("only the five mod files are listed ({}): {}", files.size(), error));
    ModSource source;
    check(read_package(catalog.mods[0], source, error), "the mod is not refused: " + error);
    check(source.files.size() == 2U && !source.files.contains("shared/.notes.luau"),
          "hidden scripts are not loaded");
    for (const std::string_view hidden :
         {".git/config", "assets/.cache/blob.png", "shared/.notes.luau", "assets/thumbs.db", "desktop.ini"})
        check(net::is_ignored_mod_path(hidden) && !net::is_client_visible_mod_path(hidden),
              std::string{hidden} + " is never sent or accepted in a download");
    check(!net::is_ignored_mod_path("assets/textures/blocks/3.png") &&
              !net::is_ignored_mod_path("shared/a.b.luau") && !net::is_ignored_mod_entry("thumbs.dbx"),
          "ordinary names are kept");
}

void packages_are_checked_and_ordered()
{
    TemporaryFolder folder{"packages"};
    const auto mod = [&](std::string_view id, std::string_view extra, std::string_view scope = "server",
                         std::string_view order = "10") {
        write_file(folder.path() / id / "openminerz.mod",
                   std::format("id={}\nversion=1.2\nscope={}\nprotocol=both\norder={}\n{}", id,
                               scope, order, extra));
        write_file(folder.path() / id / "shared" / "init.luau", "return true\n");
    };
    mod("base", "");
    mod("addon", "depends=base@1.1\n", "server", "5"); // listed before base by order
    mod("future", "api=99\n");
    mod("needs.new", "depends=base@2\n");
    mod("needs.missing", "depends=nowhere\n");
    mod("loop.a", "depends=loop.b\n");
    mod("loop.b", "depends=loop.a\n");
    mod("look", "", "client");
    mod("needs.client", "depends=look\n");
    mod("native", "");
    write_file(folder.path() / "native" / "assets" / "helper.dll", "MZ");
    mod("documented", "");
    write_file(folder.path() / "documented" / "README.md", "# hello");
    write_file(folder.path() / "documented" / "assets" / "icon.png", "png");

    net::ModCatalog catalog;
    std::string error;
    check(net::load_mod_catalog(folder.path(), catalog, error), error);
    const PackagePlan plan = plan_packages(catalog.mods);
    std::string order;
    for (const ModSource &source : plan.mods)
        order += source.id + " ";
    check(order == "base addon documented look ", "dependencies load first: " + order);
    const auto refused = [&](std::string_view id, std::string_view reason) {
        for (const auto &[name, why] : plan.refused)
            if (name == id)
            {
                check(why.find(reason) != std::string::npos,
                      std::string{id} + " refused for: " + why);
                return;
            }
        throw std::runtime_error(std::string{id} + " was not refused");
    };
    refused("future", "modding API 99");
    refused("needs.new", "base 2 or newer");
    refused("needs.missing", "not installed");
    refused("loop.a", "cycle");
    refused("loop.b", "cycle");
    refused("needs.client", "cannot depend on the client mod");
    refused("native", "helper.dll is not a file type");
    check(plan.mods[1].dependencies == std::vector<std::string>{"base"} &&
              plan.mods[0].files.contains("shared/init.luau"),
          "sources carry their scripts and dependencies");
    check(is_allowed_package_file("assets/a.png") && is_allowed_package_file("openminerz.mod") &&
              !is_allowed_package_file("a.exe") && !is_allowed_package_file("x/y.luau.bat") &&
              !is_allowed_package_file("noext"),
          "the allowlist");
    check(version_at_least("1.10", "1.9") && !version_at_least("1.2", "1.10") &&
              version_at_least("2", "2.0") && version_at_least("1.0-beta", ""),
          "version comparison");

    // Two roots merge in catalog order; the same id twice is refused.
    TemporaryFolder second{"packages-user"};
    write_file(second.path() / "extra" / "openminerz.mod",
               "id=extra\nversion=1\nscope=client\nprotocol=both\norder=0\n");
    const std::vector<std::filesystem::path> roots{folder.path(), second.path()};
    check(load_mod_catalogs(roots, catalog, error) && catalog.mods.front().descriptor.id == "extra",
          "two roots merge in order: " + error);
    write_file(second.path() / "dupe" / "openminerz.mod",
               "id=BASE\nversion=1\nscope=client\nprotocol=both\norder=0\n");
    check(!load_mod_catalogs(roots, catalog, error) && error.find("twice") != std::string::npos,
          "an id installed twice is refused");

    // A planned package runs.
    Harness harness;
    check(harness.runtime().start(Side::server, plan.mods, {}), "the plan starts");
    for (const ModStatus &status : harness.runtime().status())
        check(status.state == ModState::running, status.id + " runs: " + status.error);
}

struct TestCase final
{
    const char *name;
    void (*body)();
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"every_value_type_round_trips", every_value_type_round_trips},
        {"callbacks_are_held_invoked_and_released", callbacks_are_held_invoked_and_released},
        {"capabilities_and_sides_are_enforced", capabilities_and_sides_are_enforced},
        {"the_sandbox_refuses_escapes", the_sandbox_refuses_escapes},
        {"runaway_scripts_are_stopped", runaway_scripts_are_stopped},
        {"overruns_are_forgiven_over_time", overruns_are_forgiven_over_time},
#ifdef _WIN32
        {"stalls_do_not_count_as_overruns", stalls_do_not_count_as_overruns},
#endif
        {"events_follow_their_specs", events_follow_their_specs},
        {"require_stays_inside_the_mod", require_stays_inside_the_mod},
        {"dependencies_load_first", dependencies_load_first},
        {"lifecycle_events_and_scripts_run_in_order", lifecycle_events_and_scripts_run_in_order},
        {"timers_follow_the_simulation", timers_follow_the_simulation},
        {"storage_persists_across_reloads_and_sessions", storage_persists_across_reloads_and_sessions},
        {"settings_are_declared_and_changed", settings_are_declared_and_changed},
        {"util_is_deterministic", util_is_deterministic},
        {"print_and_log_are_tagged", print_and_log_are_tagged},
        {"no_mods_means_no_vm", no_mods_means_no_vm},
        {"handles_work_as_keys_and_acting_player_is_passed",
         handles_work_as_keys_and_acting_player_is_passed},
        {"natives_work_from_coroutines_and_nested_calls",
         natives_work_from_coroutines_and_nested_calls},
        {"descriptors_stay_compatible", descriptors_stay_compatible},
        {"editor_files_are_not_part_of_a_mod", editor_files_are_not_part_of_a_mod},
        {"packages_are_checked_and_ordered", packages_are_checked_and_ordered},
        {"performance_is_reported", performance_is_reported},
    };

    std::size_t failures{};
    for (const auto &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
