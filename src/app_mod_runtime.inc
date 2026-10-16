// Member functions and state of Application::Impl: the mod script runtime
// (openminerz/modding/runtime.hpp, openminerz/modding/package.hpp). Included
// inside the class body by app.cpp.
//
// Scripts run only when there are some: with no script mod installed no
// ModRuntime and no VM is ever created and every hook below returns at once,
// so packets, saves and gameplay are exactly as before
// (docs/retail-parity.md, rule 1).
//
// Which VMs run (docs/development.md):
//   menus (no world)           a client VM of the client-scope mods, from
//                              boot: UI themes, menu rows and settings pages
//                              apply on the main menu
//   offline world or hosting   a server VM (server and core mods) and a client
//                              VM (every mod), both in this process
//   joined a remote host       a client VM only: client mods, plus the host's
//                              synchronized mods this client has
// A session that adds no mod to the menus' client VM keeps it running (its
// mods see world.loaded / world.unloading); one that does (any server or core
// mod) gives the client VM a clean restart with the session's mods, and the
// menus' VM comes back after it.
//
// Every area registers its script library in register_mod_apis. The hooks
// app.cpp calls: start_mod_session_local (a world was started or loaded),
// start_mod_session_joined (a host's world was installed), stop_mod_session
// (the world is left), shutdown_mod_runtime (~Impl), update_mod_runtime (every
// frame), tick_mods (every simulation step), persist_mods (with every world
// save), run_mod_command (chat lines) and check_mod_packages (the Add-Ons
// screen's scan). A session opens the mod channel (start_mod_net_session)
// before its VMs and closes it after them, and starts Gameplay's halves once
// their VM's scripts ran.

enum class ModSessionKind : std::uint8_t
{
    none,
    local, // offline or hosting: this process is authoritative
    joined // a remote host is authoritative
};

std::unique_ptr<modding::ModRuntime> mod_runtime_;
ModSessionKind mod_session_{ModSessionKind::none};
bool mod_world_announced_{};
// (Network) The mods a join negotiated, e.g. LegacySteamRuntimeClient's
// downloads->selection. Unset, the lobby's required manifest and the local
// catalog decide (net::select_session_mods without a download cache).
std::optional<std::vector<net::LoadedMod>> mod_join_session_mods_;
// (Integration) The client VM of the menus: what it was built from (catalog
// and protocol; "" = build it again) and whether the running client VM is it.
std::string mod_menu_key_;
net::ProtocolVersion mod_menu_protocol_{net::ProtocolVersion::classic_v3};
bool mod_menu_vm_{};
// The ids the client VM loaded, to tell whether a session adds a mod to it.
std::vector<std::string> mod_client_vm_ids_;
// The session kept the menus' client VM running (it added no mod).
bool mod_session_kept_client_{};
// Script errors reported while the game runs, by mod id (the latest one,
// shown on the mod's Add-Ons row until it runs again, is turned off or goes),
// and chat lines paced per mod.
std::map<std::string, std::string, std::less<>> mod_script_errors_;
std::map<std::string, float, std::less<>> mod_error_chat_at_;
// (Add-Ons) Mods that are on but cannot run (a dependency missing, a cycle),
// by id: recomputed with every scan.
std::map<std::string, std::string, std::less<>> mod_refusals_;

// One call per area module (src/app_mod_<area>.inc), each registering its
// library and backend.
void register_mod_apis(modding::ApiRegistry &registry, modding::ScriptHost &host)
{
    register_mod_gameplay_api(registry, host); // app_mod_gameplay.inc
    register_mod_graphics_api(registry, host); // app_mod_graphics.inc
    register_mod_ui_api(registry, host);       // app_mod_ui.inc
    register_mod_net_api(registry, host);      // app_mod_net.inc
    register_mod_world_api(registry, host);    // app_mod_world.inc (mod-world)
    // storage.player keys players the way gameplay identifies them.
    mod_runtime_->set_player_key([this](modding::Handle player) -> std::optional<std::string> {
        if (mod_gameplay_ == nullptr)
            return std::nullopt;
        return mod_gameplay_->player_key(player);
    });
}

modding::ModRuntime &mod_runtime()
{
    if (!mod_runtime_)
    {
        // (Integration) OPENMINERZ_MOD_BUDGET_MS widens the per-call budget
        // (and the load budget with it) for a developer who profiles a slow
        // machine; the default stays 100 ms. (Polish) A breakpoint no longer
        // needs it: time the thread is stopped does not count.
        modding::RuntimeLimits limits;
        if (const char *budget = SDL_getenv("OPENMINERZ_MOD_BUDGET_MS"); budget != nullptr)
            if (const int milliseconds = std::atoi(budget); milliseconds > 0)
            {
                limits.call_budget = std::chrono::milliseconds{std::min(milliseconds, 3'600'000)};
                limits.load_budget = std::max(limits.load_budget, limits.call_budget);
            }
        mod_runtime_ = std::make_unique<modding::ModRuntime>(limits);
        mod_runtime_->set_log_sink([this](const modding::LogEntry &entry) {
            std::ostream &out = entry.level >= modding::LogLevel::warning ? std::cerr : std::cout;
            out << "mod." << modding::log_level_name(entry.level)
                << " side=" << modding::side_name(entry.side)
                << " mod=" << (entry.mod_id.empty() ? std::string{"runtime"} : entry.mod_id) << ' '
                << entry.message << '\n';
            if (entry.level == modding::LogLevel::error && !entry.mod_id.empty())
                show_mod_error(entry);
        });
        register_mod_apis(mod_runtime_->registry(), *mod_runtime_);
    }
    return *mod_runtime_;
}

// A script error while the game runs, where the developer sees it: the mod's
// row on the Add-Ons screen (the latest error) and, in a world, a chat line
// (at most one per mod every few seconds; the log has them all).
void show_mod_error(const modding::LogEntry &entry)
{
    std::string message = entry.message.substr(0U, entry.message.find('\n'));
    // (Add-Ons) The mod's row says "Script error" and shows this line.
    std::string row = std::format("{} ({} side)", message, modding::side_name(entry.side));
    if (row.size() > 240U)
        row.resize(240U);
    mod_script_errors_.insert_or_assign(entry.mod_id, std::move(row));
    if (mod_session_ == ModSessionKind::none)
        return;
    const auto last = mod_error_chat_at_.find(entry.mod_id);
    if (last != mod_error_chat_at_.end() && elapsed_ - last->second < 5.0F)
        return;
    mod_error_chat_at_.insert_or_assign(entry.mod_id, elapsed_);
    // "mod/file.luau:3: ..." already names the mod.
    const std::string line = message.starts_with(entry.mod_id + "/")
                                 ? message
                                 : std::format("{}: {}", entry.mod_id, message);
    // Online the chat console shows it; offline (no console) a notice does.
    push_online_chat("[MOD ERROR] " + line);
    if (!online_gameplay_active())
        toast("MOD SCRIPT ERROR", line.substr(0U, 120U));
}

// A mod that is on but cannot run: its Add-Ons row says "Not loaded" and why
// (the first reason found since the last scan).
void report_mod_problem(std::string_view id, std::string_view reason)
{
    std::string text{reason};
    if (text.size() > 240U)
        text.resize(240U);
    mod_refusals_.emplace(std::string{id}, std::move(text));
}

// The Add-Ons scan: every mod that is on is checked once (descriptor
// metadata, file types, dependencies) so problems show before any session.
void check_mod_packages()
{
    mod_refusals_.clear(); // (Add-Ons) apply_addons changed which mods are on
    if (mod_catalog_valid_ && !mod_catalog_.mods.empty())
    {
        const modding::PackagePlan plan = modding::plan_packages(mod_catalog_.mods);
        for (const auto &[id, reason] : plan.refused)
            report_mod_problem(id, reason);
    }
    // (Integration) The menus' client VM comes up with the catalog, at boot.
    // (Add-Ons) After that only when its mods or their files changed: the
    // Add-Ons screen scans whenever it opens, which must not restart a theme.
    if (mod_session_ == ModSessionKind::none)
        update_mod_menu_runtime(true);
}

[[nodiscard]] bool mod_console_available() const noexcept
{
    return mod_session_ == ModSessionKind::local;
}

// The mods an offline or hosted world of this profile runs: exactly the mods
// the host manifest advertises.
[[nodiscard]] std::vector<net::LoadedMod> mod_profile_mods(bool client_scope_only) const
{
    std::vector<net::LoadedMod> mods;
    if (!mod_catalog_valid_)
        return mods;
    const bool classic = host_settings_.protocol == net::ProtocolVersion::classic_v3;
    for (const net::LoadedMod &mod : mod_catalog_.mods)
        if ((classic ? mod.classic : mod.modern) &&
            (!client_scope_only || mod.descriptor.scope == net::ModScope::client))
            mods.push_back(mod);
    return mods;
}

[[nodiscard]] static bool mod_plan_has_scripts(const modding::PackagePlan &plan)
{
    return std::any_of(plan.mods.begin(), plan.mods.end(),
                       [](const modding::ModSource &mod) { return !mod.files.empty(); });
}

[[nodiscard]] static std::vector<std::string> mod_client_ids(std::span<const modding::ModSource> mods)
{
    std::vector<std::string> ids;
    for (const modding::ModSource &mod : mods)
        if (modding::runs_on(mod.scope, modding::Side::client))
            ids.push_back(mod.id);
    return ids;
}

// (Integration) Outside a session: the client VM of the client-scope mods,
// built when the catalog or the profile changes (and after a session that
// restarted the client VM). With none that has scripts, no VM - and no
// runtime, when none was ever needed.
void update_mod_menu_runtime(bool catalog_changed = false)
{
    // Every frame in the menus: two comparisons unless something changed
    // (the catalog changes only through scan_addons and the Add-Ons screen,
    // which pass catalog_changed: the key below then decides).
    if (!catalog_changed && !mod_menu_key_.empty() && mod_menu_protocol_ == host_settings_.protocol)
        return;
    mod_menu_protocol_ = host_settings_.protocol;
    std::string key = mod_catalog_valid_ ? "v" : "x";
    key += std::to_string(static_cast<int>(host_settings_.protocol)) + '\n';
    // (Add-Ons) Only the mods this VM runs, with their digests: turning a
    // server mod on or off leaves it be, and a mod edited on disk restarts it.
    const std::vector<net::LoadedMod> mods = mod_profile_mods(true);
    for (const net::LoadedMod &mod : mods)
        key += mod.descriptor.id + '\n' + mod.root.string() + '\n' +
               net::digest_hex(mod.descriptor.content_digest) + '\n';
    if (key == mod_menu_key_)
        return;
    mod_menu_key_ = std::move(key);
    if (mod_runtime_ && mod_runtime_->running(modding::Side::client))
        mod_runtime_->stop(modding::Side::client);
    mod_menu_vm_ = false;
    mod_client_vm_ids_.clear();
    // (Add-Ons) A fresh run: what its mods reported last time is past.
    for (const net::LoadedMod &mod : mods)
        mod_script_errors_.erase(mod.descriptor.id);
    if (mods.empty())
        return;
    modding::PackagePlan plan = modding::plan_packages(mods);
    for (const auto &[id, reason] : plan.refused)
        report_mod_problem(id, reason);
    if (!mod_plan_has_scripts(plan))
        return;
    modding::SessionInfo info;
    info.local_dir = modding::mod_data_root();
    std::vector<std::string> ids = mod_client_ids(plan.mods);
    if (!mod_runtime().start(modding::Side::client, std::move(plan.mods), info))
        return;
    mod_menu_vm_ = true;
    mod_client_vm_ids_ = std::move(ids);
    std::cout << "mod.menu client_vm=1 mods=" << mod_client_vm_ids_.size() << '\n';
}

void start_mod_session(std::span<const net::LoadedMod> mods, bool authoritative)
{
    if (mods.empty())
        return;
    modding::PackagePlan plan = modding::plan_packages(mods);
    for (const auto &[id, reason] : plan.refused)
        report_mod_problem(id, reason);
    // (Add-Ons) A fresh run of the session's mods: their old errors are past.
    for (const modding::ModSource &mod : plan.mods)
        mod_script_errors_.erase(mod.id);
    // Packs without scripts (textures, sounds, UI themes) need no VM. (fix)
    // A joined session with server mods does, scripts or not: its gameplay
    // half is what receives the host's creatures, blocks and enemy rules
    // (a server mod's download holds no server/ scripts).
    const bool joined_server_mods = !authoritative && mod_net_client_ != nullptr && mod_net_client_->enabled();
    if (!mod_plan_has_scripts(plan) && !joined_server_mods)
        return;
    modding::SessionInfo info;
    if (authoritative && !active_world_path_.empty())
    {
        // Beside the world save, like its ".players" inventory folder.
        info.world_dir = active_world_path_;
        info.world_dir += ".mods";
    }
    info.local_dir = modding::mod_data_root();
    info.world_seed = static_cast<std::uint32_t>(world_seed_);
    modding::ModRuntime &runtime = mod_runtime();
    // The mod channel exists before the VMs (app_mod_net.inc; a joined
    // session's channel opened with the join).
    start_mod_net_session(authoritative);
    bool started{};
    if (authoritative)
        started = runtime.start(modding::Side::server, plan.mods, info);
    // The menus' client VM stays when the session adds nothing to it;
    // otherwise the client VM restarts with every mod of the session.
    std::vector<std::string> client_ids = mod_client_ids(plan.mods);
    mod_session_kept_client_ =
        mod_menu_vm_ && runtime.running(modding::Side::client) && client_ids == mod_client_vm_ids_;
    if (mod_session_kept_client_)
        started = true;
    else
    {
        mod_menu_key_.clear(); // built again once the session ends
        mod_menu_vm_ = false;
        mod_client_vm_ids_.clear();
        if (runtime.start(modding::Side::client, std::move(plan.mods), info))
        {
            mod_client_vm_ids_ = std::move(client_ids);
            started = true;
        }
    }
    if (!started)
    {
        if (authoritative)
            stop_mod_net_session();
        return;
    }
    mod_session_ = authoritative ? ModSessionKind::local : ModSessionKind::joined;
    mod_world_announced_ = false;
    // mod-world (fix): the session's world generator goes in first, so the
    // gameplay halves, every script and the player's spawn see its terrain;
    // the loading screen then waits for it (app_mod_world.inc).
    mod_world_session_started();
    if (mod_session_ == ModSessionKind::none)
        return; // the world was refused
    // Gameplay's halves start once their VM's definitions are in, before
    // world.loaded (app_mod_gameplay.inc); only a session with server mods
    // has them.
    if (mod_gameplay_ != nullptr && runtime.running(modding::Side::server))
        mod_gameplay_->start_server();
    if (mod_gameplay_ != nullptr && mod_gameplay_client_wanted())
        mod_gameplay_->start_client();
    std::cout << "mod.session kind=" << (authoritative ? "local" : "joined")
              << " server_vm=" << runtime.running(modding::Side::server)
              << " client_vm=" << runtime.running(modding::Side::client)
              << " kept_client_vm=" << mod_session_kept_client_ << '\n';
}

// An offline world or a hosted one: this profile's mods for the world's
// protocol, exactly the mods the host manifest advertises.
void start_mod_session_local()
{
    stop_mod_session();
    start_mod_session(mod_profile_mods(false), true);
}

// A remote host's world was installed: client mods plus the synchronized mods
// the host requires (never a server mod the host did not ask for).
void start_mod_session_joined()
{
    // Taken before stop_mod_session, which forgets it.
    std::optional<std::vector<net::LoadedMod>> negotiated =
        std::exchange(mod_join_session_mods_, std::nullopt);
    stop_mod_session();
    if (!mod_catalog_valid_)
        return;
    std::vector<net::LoadedMod> mods;
    if (negotiated)
        mods = std::move(*negotiated);
    else
    {
        std::optional<net::ModManifest> required;
        if (const std::optional<std::size_t> index = join_browser_.index_of(online_join_lobby_id_))
            required = join_browser_.sessions()[*index].required_manifest;
        // (Add-Ons) A server mod that is off here is used if the host runs it.
        mods = net::select_session_mods(addon_join_catalog(), net::ModCatalog{},
                                        required ? &*required : nullptr, online_client_protocol_)
                   .mods;
    }
    start_mod_session(mods, false);
}

void stop_mod_session()
{
    if (mod_session_ == ModSessionKind::none || !mod_runtime_)
    {
        mod_session_ = ModSessionKind::none;
        return;
    }
    mod_world_session_stopped(); // app_mod_world.inc (mod-world)
    // (Add-Ons) A mod that failed or was stopped keeps saying why on its row.
    for (const modding::ModStatus &status : mod_runtime_->status())
        if (status.state != modding::ModState::running && !status.error.empty())
            mod_script_errors_.insert_or_assign(status.id, status.error.substr(0U, status.error.find('\n')));
    if (mod_session_kept_client_)
    {
        // The menus' client VM goes on: only the world ends for it.
        mod_runtime_->stop(modding::Side::server);
        mod_runtime_->world_unloaded(modding::Side::client);
        mod_runtime_->forget_session_storage();
    }
    else
        mod_runtime_->stop_all();
    stop_mod_net_session(); // app_mod_net.inc: the channel after the VMs
    mod_session_ = ModSessionKind::none;
    mod_session_kept_client_ = false;
    mod_join_session_mods_.reset();
    mod_error_chat_at_.clear();
}

// ~Impl: every VM stops while every service is still alive (their teardown
// listeners run), the menus' too.
void shutdown_mod_runtime()
{
    stop_mod_session();
    if (mod_runtime_)
        mod_runtime_->stop_all();
    mod_menu_vm_ = false;
    mod_menu_key_ = "shutdown";
}

// Gameplay's client half runs beside a client VM that has the mod channel: a
// session with server mods. Client-scope mods alone never start it, so their
// sessions (and the menus) keep every gameplay hook inert.
[[nodiscard]] bool mod_gameplay_client_wanted() const
{
    return mod_runtime_ && mod_runtime_->running(modding::Side::client) && mod_net_client_ != nullptr &&
           mod_net_client_->enabled();
}

// Every frame, near the top of update(): the menus' VM outside a session;
// in one, ends a joined session whose host is gone and announces the world
// once it is ready. Then the client VM's frame.
void update_mod_runtime(float delta)
{
    if (mod_session_ == ModSessionKind::none)
    {
        update_mod_menu_runtime();
        if (mod_runtime_ && mod_menu_vm_)
            mod_runtime_->frame(static_cast<double>(delta));
        return;
    }
    if (!mod_runtime_)
        return;
    if (mod_session_ == ModSessionKind::joined && !online_client_joined_)
    {
        stop_mod_session();
        return;
    }
    if (!mod_world_announced_ && world_ready())
    {
        mod_world_announced_ = true;
        const std::string mode = mod_session_ == ModSessionKind::joined ? "client"
                                 : online_host_ != nullptr             ? "host"
                                                                        : "offline";
        const modding::Value payload{modding::Map{
            {"name", active_world_path_.empty() ? std::string{}
                                                : active_world_path_.stem().string()},
            {"seed", static_cast<std::int64_t>(world_seed_)},
            {"mode", mode}}};
        mod_runtime_->world_loaded(modding::Side::server, payload);
        mod_runtime_->world_loaded(modding::Side::client, payload);
    }
    mod_runtime_->frame(static_cast<double>(delta));
}

// One fixed simulation step (only an authoritative session has a server VM).
void tick_mods(float seconds)
{
    if (mod_session_ == ModSessionKind::local && mod_runtime_)
        mod_runtime_->tick(static_cast<double>(seconds));
}

void persist_mods()
{
    if (mod_runtime_)
        mod_runtime_->persist();
}

void print_mod_line(std::string line)
{
    std::cout << "mod.console " << line << '\n';
    push_online_chat(std::move(line));
}

// Whether a chat line is for the mod console ("/mods ..."): it stays on this
// machine, before any mod command or the session sees it.
[[nodiscard]] static bool is_mod_console_line(std::string_view line) noexcept
{
    return line == "/mods" || line.starts_with("/mods ");
}

// The developer console in the chat line. "/mods" lists the running mods,
// "/mods reload" rebuilds them from disk (offline only), keeping their
// storage and settings. Returns true when the line was handled here; offline
// every line is (there is nobody to chat with).
bool run_mod_command(std::string_view line, bool online)
{
    if (!is_mod_console_line(line))
        return !online;
    std::string_view argument = line.substr(std::min<std::size_t>(line.size(), 5U));
    while (!argument.empty() && argument.front() == ' ')
        argument.remove_prefix(1U);
    if (argument.empty() || argument == "list")
    {
        if (!mod_runtime_ || mod_session_ == ModSessionKind::none)
        {
            print_mod_line("No mod scripts are running.");
            return true;
        }
        for (const modding::ModStatus &status : mod_runtime_->status())
        {
            const char *state = status.state == modding::ModState::running ? "running"
                                : status.state == modding::ModState::failed ? "FAILED"
                                                                            : "STOPPED";
            print_mod_line(std::format("{} [{}] {}: {} handlers, {} timers, {} KiB, {} errors{}{}",
                                       status.id, modding::side_name(status.side), state,
                                       status.handlers, status.timers, status.memory_bytes / 1024U,
                                       status.errors, status.error.empty() ? "" : " - ",
                                       status.error));
        }
        return true;
    }
    if (argument == "reload")
    {
        if (mod_session_ != ModSessionKind::local || !mod_runtime_)
        {
            print_mod_line("/mods reload works offline, with mods running.");
            return true;
        }
        // (Integration) A hosted session advertised its manifest (ids and
        // content digests) to the lobby and admitted players by it: reloading
        // from disk would change what it runs under them. Refused; leave the
        // world and host again instead.
        if (hosting_flow_ || online_host_ != nullptr || online_host_start_.valid())
        {
            print_mod_line("/mods reload is not available while hosting: players joined by the "
                           "advertised mods. Leave the world and host it again to reload.");
            return true;
        }
        scan_addons();
        if (!mod_catalog_valid_)
        {
            print_mod_line("Reload failed: " + mod_catalog_error_);
            return true;
        }
        // A clean restart of the session: VMs, mod channel and Gameplay's
        // halves, so every handle and handler is made again together. Storage
        // stays cached in the runtime (and settings on disk) across it.
        for (const modding::Side side : {modding::Side::server, modding::Side::client})
            mod_runtime_->stop(side);
        stop_mod_net_session();
        mod_session_ = ModSessionKind::none;
        mod_session_kept_client_ = false;
        mod_menu_vm_ = false;
        mod_menu_key_.clear();
        mod_client_vm_ids_.clear();
        start_mod_session(mod_profile_mods(false), true);
        print_mod_line(std::format(
            "Mods reloaded (server VM {}, client VM {}).",
            mod_runtime_->running(modding::Side::server) ? "on" : "off",
            mod_runtime_->running(modding::Side::client) ? "on" : "off"));
        return true;
    }
    print_mod_line("Usage: /mods [list|reload]");
    return true;
}
