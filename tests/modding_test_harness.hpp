#pragma once

// Script-level tests for any modding area, without the game.
//
//   modding::testing::Harness harness;
//   register_ui_api(harness.registry(), harness.host(), fake_backend);
//   auto result = harness.run(R"(
//       ui.toast("hello")
//       return 42
//   )", Side::client);
//   check(result.ok, result.error);
//   check(result.value == modding::Value{42}, "returned 42");
//   check(harness.logged("hello"), "...");          // log/print output
//   harness.emit(Side::server, "tick", modding::Map{{"dt", 0.05}});
//
// run() puts the snippet into a test mod ("test" unless you name another)
// on the chosen side, creating that side's VM on first use. Capabilities
// default to everything a server/core mod gets on that side; pass
// client_mod_capabilities() (or any mask) to test refusals. Several mods can
// share a VM: run(..., mod_id = "other") adds another one.
//
// Snippets are plain Luau with the mod's full environment (libraries,
// require of files added with add_file()). An error is returned in
// result.error ("test/chunk:3: message") and also logged.

#include "openminerz/modding/runtime.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openminerz::modding::testing
{

[[nodiscard]] inline CapabilityMask client_mod_capabilities() noexcept
{
    return capabilities_for(net::ModScope::client, Side::client);
}
[[nodiscard]] inline CapabilityMask server_mod_capabilities(Side side) noexcept
{
    return capabilities_for(net::ModScope::server, side);
}

// Small limits so a runaway test fails fast.
[[nodiscard]] inline RuntimeLimits test_limits()
{
    RuntimeLimits limits;
    limits.memory_bytes = 32U * 1024U * 1024U;
    limits.load_budget = std::chrono::milliseconds{1000};
    limits.call_budget = std::chrono::milliseconds{250};
    return limits;
}

class Harness final
{
  public:
    explicit Harness(RuntimeLimits limits = test_limits()) : runtime_{limits} {}
    ~Harness()
    {
        runtime_.stop_all();
    }
    Harness(const Harness &) = delete;
    Harness &operator=(const Harness &) = delete;

    [[nodiscard]] ApiRegistry &registry() noexcept
    {
        return runtime_.registry();
    }
    [[nodiscard]] ScriptHost &host() noexcept
    {
        return runtime_;
    }
    [[nodiscard]] ModRuntime &runtime() noexcept
    {
        return runtime_;
    }
    // Storage folders and seed for the VMs this harness starts.
    void set_session(SessionInfo info)
    {
        session_ = std::move(info);
    }

    // Adds a file to a mod before its first run() (for require tests or a
    // whole mod layout: "shared/init.luau", "server/main.luau", ...). Files in
    // shared/ and the side's folder run when the mod loads.
    void add_file(std::string path, std::string source, std::string_view mod_id = "test")
    {
        files_[std::string{mod_id}][std::move(path)] = std::move(source);
    }

    RunResult run(std::string_view source, Side side = Side::server,
                  std::optional<CapabilityMask> capabilities = std::nullopt,
                  std::string_view mod_id = "test")
    {
        if (!ensure_mod(side, capabilities, mod_id))
        {
            RunResult failed;
            failed.error = "the mod could not load: " + last_error();
            return failed;
        }
        return runtime_.run(side, mod_id, "chunk", source);
    }

    bool emit(Side side, std::string_view event, const Value &payload = {})
    {
        return runtime_.emit(side, event, payload);
    }

    // Every line logged so far (log.*, print, errors), oldest first.
    [[nodiscard]] std::vector<LogEntry> logs() const
    {
        return {runtime_.recent_log().begin(), runtime_.recent_log().end()};
    }
    [[nodiscard]] bool logged(std::string_view text) const
    {
        return std::any_of(runtime_.recent_log().begin(), runtime_.recent_log().end(),
                           [&](const LogEntry &entry) {
                               return entry.message.find(text) != std::string::npos;
                           });
    }
    [[nodiscard]] std::string errors() const
    {
        std::string text;
        for (const LogEntry &entry : runtime_.recent_log())
            if (entry.level == LogLevel::error)
                text += entry.message + '\n';
        return text;
    }
    [[nodiscard]] std::string last_error() const
    {
        for (auto entry = runtime_.recent_log().rbegin(); entry != runtime_.recent_log().rend();
             ++entry)
            if (entry->level == LogLevel::error)
                return entry->message;
        return {};
    }
    void clear_logs() noexcept
    {
        runtime_.clear_log();
    }

  private:
    bool ensure_mod(Side side, std::optional<CapabilityMask> capabilities, std::string_view mod_id)
    {
        const std::string key{mod_id};
        if (runtime_.running(side))
        {
            const auto status = runtime_.status();
            const bool present = std::any_of(status.begin(), status.end(), [&](const ModStatus &mod) {
                return mod.side == side && mod.id == key;
            });
            if (present)
                return true;
            return runtime_.attach(side, make_source(side, capabilities, key));
        }
        std::vector<ModSource> mods;
        mods.push_back(make_source(side, capabilities, key));
        if (!runtime_.start(side, std::move(mods), session_))
            return false;
        const auto status = runtime_.status();
        return std::any_of(status.begin(), status.end(), [&](const ModStatus &mod) {
            return mod.side == side && mod.id == key && mod.state == ModState::running;
        });
    }

    ModSource make_source(Side side, std::optional<CapabilityMask> capabilities,
                          const std::string &id)
    {
        ModSource source;
        source.id = id;
        source.scope = net::ModScope::server;
        source.capabilities = capabilities.value_or(server_mod_capabilities(side));
        if (const auto found = files_.find(id); found != files_.end())
            for (const auto &[path, text] : found->second)
                source.files.insert_or_assign(path, text);
        return source;
    }

    ModRuntime runtime_;
    SessionInfo session_;
    std::map<std::string, std::map<std::string, std::string>> files_;
};

} // namespace openminerz::modding::testing
