// Member functions and state of Application::Impl: the mod network layer
// (openminerz/modding/net.hpp, openminerz/net/mod_download.hpp). Included
// inside the class body by app.cpp, next to app_mod_runtime.inc.
//
// Nothing here puts a byte on the wire unless the session's manifest carries a
// server/core mod: no channel object exists otherwise, every hook returns at
// once, and the live session classes refuse mod frames on their own as well,
// so packets, saves and admission are exactly as before (docs/retail-parity.md,
// rule 1).
//
// Who talks to whom:
//   offline / hosting   ModNetServer (authority) + ModNetClient (this player's
//                       client VM) joined in-process by pump_mod_net_loopback;
//                       remote players' frames go through online_host_.
//                       While hosting, ModDownloadServer serves the client
//                       part of the session's server mods to joiners.
//   joined              ModNetClient only, talking to the host through
//                       online_client_; created when the join completes so the
//                       host's welcome is never missed.
//
// Hooks app.cpp calls (each marked "(Network)" there): mod_net_host_frame,
// mod_download_host_frame, mod_net_peer_connected, mod_net_peer_disconnected
// (pump_online_host); mod_net_client_frame (pump_online_client); pump_mod_net
// (every frame) and flush_mod_net (after each tick_mods); mod_join_download_context
// and mod_net_join_finished (the join); mod_download_input (join panel input);
// mod_net_assessment_manifest (browser preflight).
// The Runtime's session hooks call start_mod_net_session(authoritative) before
// its VMs start and stop_mod_net_session() after they stop, and
// register_mod_apis calls register_mod_net_api.

struct ModNetBackend final : modding::NetBackend
{
    modding::ModNetServer *server_ptr{};
    modding::ModNetClient *client_ptr{};
    [[nodiscard]] modding::ModNetServer *server() noexcept override
    {
        return server_ptr;
    }
    [[nodiscard]] modding::ModNetClient *client() noexcept override
    {
        return client_ptr;
    }
};

std::unique_ptr<modding::ModNetServer> mod_net_server_;
std::unique_ptr<modding::ModNetClient> mod_net_client_;
std::unique_ptr<net::ModDownloadServer> mod_download_server_;
ModNetBackend mod_net_backend_;
// The join in flight (or just finished): the UI's download view and the mods
// the join selected. Null when no join offered downloads.
std::shared_ptr<net::ModJoinDownloadContext> mod_join_download_;
// Gameplay's replication rides the channel (modding/gameplay_net.hpp).
std::unique_ptr<modding::gameplay::ModNetGameplaySync> mod_net_gameplay_;
std::chrono::steady_clock::time_point mod_net_epoch_{std::chrono::steady_clock::now()};
// Bumped each time the channel objects are made, so a service that puts its
// handlers on them (the UI's "omz.ui") knows when to do it again.
std::uint64_t mod_net_generation_{};
// (Authority) Player handles of a session without the gameplay service.
std::uint64_t mod_net_plain_serial_{};
std::map<std::uint64_t, std::uint8_t> mod_net_plain_players_;

void register_mod_net_api(modding::ApiRegistry &registry, modding::ScriptHost &host)
{
    modding::register_net_api(registry, host, mod_net_backend_);
}

[[nodiscard]] double mod_net_now() const
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - mod_net_epoch_).count();
}

// %LOCALAPPDATA%\OpenMinerZ\ModCache: downloaded client parts, by content
// digest. Never part of the installed catalog.
[[nodiscard]] static std::filesystem::path mod_download_cache_root()
{
    const std::filesystem::path data = modding::mod_data_root();
    return data.empty() ? data : data.parent_path() / "ModCache";
}

// (Integration) The one player identity. Gameplay's admitted record names a
// player for net.*, replicated.*, ui.*(player) and players.* alike: a
// Handle{player, serial} never reused in the session, the same on the host
// and (through the welcome and the player directory below) on every client.
[[nodiscard]] modding::Handle mod_net_admit(std::uint8_t gid, std::string_view name, bool local)
{
    if (mod_gameplay_ != nullptr)
        return mod_gameplay_->admit_player(gid, name, local);
    // (Authority) An "OpenMinerZ players only" session with no server mod has
    // no gameplay service, but its channel still names each player: by a
    // serial of its own, never reused in the session.
    const modding::Handle handle{modding::HandleKind::player, ++mod_net_plain_serial_};
    mod_net_plain_players_[handle.id] = gid;
    return handle;
}

// The player directory every client holds: engine keys of each player's
// replicated store (public, so every peer has every player's), naming the
// game's gid and gamer tag behind a handle. Scripts cannot read the engine
// namespace; the app uses it to find a player a handle names (lights and
// models attached to players, the UI's local player). Dropped with the
// player's store when it leaves.
void mod_net_publish_player(modding::Handle player, std::uint8_t gid, std::string_view name)
{
    if (mod_net_server_ == nullptr || !mod_net_server_->enabled() || !player.valid())
        return;
    try
    {
        const modding::StoreRef store = modding::StoreRef::player(player);
        const modding::ReplicatedKeyOptions options{true, modding::ReplicationPriority::critical};
        mod_net_server_->set(store, "", "gid", modding::Value{static_cast<std::int64_t>(gid)}, options);
        mod_net_server_->set(store, "", "name", modding::Value{std::string{name.substr(0U, 64U)}}, options);
    }
    catch (const modding::ScriptError &error)
    {
        std::cerr << "mod.net.directory.failure " << error.what() << '\n';
    }
}

// The gid a player handle names in this session, on the host or a client.
[[nodiscard]] std::optional<std::uint8_t> mod_net_player_gid(modding::Handle player) const
{
    if (player.kind != modding::HandleKind::player)
        return std::nullopt;
    if (mod_net_server_ == nullptr && mod_net_client_ != nullptr)
    {
        const modding::Value *gid = mod_net_client_->get(modding::StoreRef::player(player), "", "gid");
        if (gid == nullptr || gid->type() != modding::ValueType::integer || gid->as_integer() < 0 ||
            gid->as_integer() > 255)
            return std::nullopt;
        return static_cast<std::uint8_t>(gid->as_integer());
    }
    if (mod_gameplay_ != nullptr)
        return mod_gameplay_->player_gid(player);
    const auto plain = mod_net_plain_players_.find(player.id); // (Authority)
    return plain != mod_net_plain_players_.end() ? std::optional<std::uint8_t>{plain->second} : std::nullopt;
}

// This process's own player as server scripts address it.
[[nodiscard]] modding::Handle mod_net_local_player() const
{
    if (mod_net_client_ != nullptr && mod_net_client_->welcomed())
        return mod_net_client_->local_player();
    return mod_gameplay_ != nullptr ? mod_gameplay_->local_player_handle() : modding::Handle{};
}

void attach_mod_net_objects()
{
    ++mod_net_generation_;
    mod_net_backend_.server_ptr = mod_net_server_.get();
    mod_net_backend_.client_ptr = mod_net_client_.get();
    modding::connect_net_events(mod_runtime(), mod_net_server_.get(), mod_net_client_.get());
    if (mod_gameplay_ != nullptr)
    {
        // The listen host's own client half is reached by Gameplay's loopback.
        mod_net_gameplay_ = std::make_unique<modding::gameplay::ModNetGameplaySync>(*mod_gameplay_);
        mod_net_gameplay_->attach(mod_net_server_.get(),
                                  mod_net_server_ != nullptr ? nullptr : mod_net_client_.get());
        mod_gameplay_->loopback().set_remote(mod_net_gameplay_.get());
    }
    // A joined client's gameplay half asks the host for its state once the
    // host's welcome is in (when it was already running; otherwise
    // start_client asks, and the channel holds the request until then).
    if (mod_net_server_ == nullptr && mod_net_client_ != nullptr)
        mod_net_client_->add_event_listener([this](const modding::ModNetEvent &event) {
            if (event.kind == modding::ModNetEventKind::welcome && mod_gameplay_ != nullptr &&
                mod_gameplay_->client_running())
                mod_gameplay_->client_channel_ready();
        });
}

// Before the session's VMs start. A joined session's client already exists
// (mod_net_join_finished); an authoritative one builds its server and its
// in-process client here, from the manifest this session advertises.
void start_mod_net_session(bool authoritative)
{
    if (!authoritative)
        return;
    stop_mod_net_session();
    net::ModManifest manifest;
    std::string error;
    if (online_host_ != nullptr || online_host_start_.valid() || hosting_flow_)
        manifest = host_settings_.mod_manifest;
    else if (!mod_catalog_valid_ ||
             !net::make_mod_manifest(mod_catalog_, host_settings_.protocol, manifest, error))
        return;
    if (!net::requires_extension(manifest))
        return;
    mod_net_server_ = std::make_unique<modding::ModNetServer>(manifest);
    mod_net_client_ = std::make_unique<modding::ModNetClient>(manifest);
    attach_mod_net_objects();
    // This process's own player is a peer like any other, over the loopback.
    const std::uint8_t local_gid =
        mod_gameplay_backend_ != nullptr ? mod_gameplay_backend_->local_player().value_or(0U) : 0U;
    const std::string local_name = mod_gameplay_local_name();
    const modding::Handle local = mod_net_admit(local_gid, local_name, true);
    if (mod_net_server_->add_peer(modding::local_mod_peer, local, mod_net_now()))
        mod_net_publish_player(local, local_gid, local_name);
    else
        std::cerr << "mod.net.peer.refused local\n";
    std::cout << "mod.net.session authority=1 mods=" << manifest.mods.size() << '\n';
}

void stop_mod_net_session()
{
    stop_authority_link(); // (Authority) its handlers live on these objects
    mod_net_plain_players_.clear();
    if (mod_net_gameplay_ != nullptr)
    {
        mod_net_gameplay_->attach(nullptr, nullptr);
        if (mod_gameplay_ != nullptr)
            mod_gameplay_->loopback().set_remote(nullptr);
        mod_net_gameplay_.reset();
    }
    // The handles this channel was given end with it (the server half, when
    // it ran, forgot them as its VM stopped).
    if (mod_gameplay_ != nullptr && !mod_gameplay_->server_running())
        mod_gameplay_->stop_server();
    mod_net_backend_.server_ptr = nullptr;
    mod_net_backend_.client_ptr = nullptr;
    mod_net_client_.reset();
    mod_net_server_.reset();
    mod_download_server_.reset();
}

// Every simulation step, right after tick_mods: replication flushes once per
// tick, within each peer's budget.
void flush_mod_net()
{
    if (mod_net_server_ == nullptr)
        return;
    mod_net_server_->update(mod_net_now());
    route_mod_net_frames();
}

void route_mod_net_frames()
{
    const double now = mod_net_now();
    if (mod_net_server_ != nullptr && mod_net_client_ != nullptr)
    {
        modding::pump_mod_net_loopback(
            *mod_net_server_, *mod_net_client_, modding::local_mod_peer, now,
            [this](modding::ModNetSend &&send) {
                std::string error;
                if (online_host_ != nullptr &&
                    !online_host_->send_mod_frame(send.peer, send.frame, error))
                    std::cerr << "mod.net.send.failure peer=" << send.peer << " reason=" << error
                              << '\n';
            });
        return;
    }
    if (mod_net_client_ != nullptr && online_client_ != nullptr && online_client_joined_)
    {
        mod_net_client_->update(now);
        for (const std::vector<std::uint8_t> &frame : mod_net_client_->take_sends())
        {
            std::string error;
            if (!online_client_->send_mod_frame(frame, error))
                std::cerr << "mod.net.send.failure host reason=" << error << '\n';
        }
    }
}

// Every frame: opens the local client's channel once its VM runs, routes
// frames, serves downloads, removes peers that flood, and keeps the join
// panel's download line current.
void pump_mod_net()
{
    update_mod_download_prompt();
    if (mod_net_client_ != nullptr && online_client_ == nullptr && mod_net_server_ == nullptr)
    {
        stop_mod_net_session(); // the joined session is gone
        return;
    }
    const double now = mod_net_now();
    if (mod_net_client_ != nullptr && mod_runtime_ && mod_runtime_->running(modding::Side::client))
        mod_net_client_->set_ready(now);
    if (mod_net_server_ != nullptr)
    {
        if (mod_download_server_ == nullptr && online_host_ != nullptr && online_host_->running())
        {
            net::ModDownloadServerOptions options;
            options.password = host_settings_.password;
            mod_download_server_ = std::make_unique<net::ModDownloadServer>(
                host_settings_.mod_manifest, mod_catalog_.mods, std::move(options));
            std::string error;
            if (!mod_download_server_->prepare(error))
                std::cerr << "mod.download.prepare.failure " << error << '\n';
        }
        if (mod_download_server_ != nullptr && online_host_ != nullptr)
        {
            mod_download_server_->update(now);
            for (const net::ModDownloadSend &send : mod_download_server_->take_sends())
            {
                std::string error;
                if (!online_host_->send_mod_frame(send.peer, send.frame, error))
                    std::cerr << "mod.download.send.failure peer=" << send.peer
                              << " reason=" << error << '\n';
            }
        }
        for (const modding::ModNetEvent &event : mod_net_server_->take_events())
        {
            if (event.kind != modding::ModNetEventKind::peer_misbehaving || online_host_ == nullptr)
                continue;
            std::cerr << "mod.net.kick peer=" << event.peer << " reason=" << event.detail << '\n';
            if (const std::optional<std::uint8_t> gid = online_host_->gid_for(event.peer))
                send_host_kick(*gid, false);
        }
    }
    if (mod_net_client_ != nullptr)
        static_cast<void>(mod_net_client_->take_events());
    route_mod_net_frames();
}

// --- Live session events ------------------------------------------------------

void mod_net_host_frame(net::PeerId peer, std::span<const std::uint8_t> frame)
{
    if (mod_net_server_ != nullptr)
        mod_net_server_->receive(peer, frame, mod_net_now());
}

void mod_download_host_frame(net::PeerId peer, std::span<const std::uint8_t> frame)
{
    if (mod_download_server_ != nullptr)
        mod_download_server_->receive(peer, frame, mod_net_now());
}

void mod_net_peer_connected(net::PeerId peer, std::uint8_t gid, std::string_view name)
{
    if (mod_download_server_ != nullptr)
        mod_download_server_->forget(peer);
    if (mod_net_server_ == nullptr || online_host_ == nullptr ||
        !online_host_->mod_channel_peer(peer))
        return;
    // (Integration) The one path a player enters by: Gameplay admits it (its
    // handle), the channel opens with that handle, then scripts hear
    // player.joined - so a handler can already reach it with net.send_to,
    // ui.toast or replicated.set (queued until its client is ready).
    const modding::Handle player = mod_net_admit(gid, name, false);
    if (!player.valid() || !mod_net_server_->add_peer(peer, player, mod_net_now()))
    {
        std::cerr << "mod.net.peer.refused peer=" << peer << '\n';
        return;
    }
    mod_net_publish_player(player, gid, name);
    if (mod_gameplay_ != nullptr)
        mod_gameplay_->remote_player_joined(gid, name);
    authority_peer_joined(gid); // (Authority)
}

void mod_net_peer_disconnected(net::PeerId peer, std::uint8_t gid)
{
    authority_peer_left(gid); // (Authority)
    if (mod_download_server_ != nullptr)
        mod_download_server_->forget(peer);
    if (mod_net_server_ == nullptr || !mod_net_server_->has_peer(peer))
        return;
    // Nothing more is sent to it (net.player_left); player.left handlers can
    // still read its replicated values; then its store goes, so a long
    // session does not keep every departed player's state.
    const std::optional<modding::Handle> player = mod_net_server_->player_of(peer);
    mod_net_server_->remove_peer(peer);
    if (mod_gameplay_ != nullptr)
        mod_gameplay_->remote_player_left(gid);
    if (player.has_value() && mod_net_server_ != nullptr)
        mod_net_server_->drop_store(modding::StoreRef::player(*player));
    // Its open server dialogues (and their on_choice callbacks) go too.
    if (player.has_value() && mod_ui_ != nullptr)
        mod_ui_->forget_player(*player);
}

void mod_net_client_frame(std::span<const std::uint8_t> frame)
{
    if (mod_net_client_ != nullptr)
        mod_net_client_->receive(frame, mod_net_now());
}

// --- Joining --------------------------------------------------------------------

// The download hook for one join (LegacySteamRuntimeClient::join). The worker
// thread fills it; the UI reads mod_join_download_->shared.
[[nodiscard]] std::shared_ptr<net::ModJoinDownloadContext> mod_join_download_context()
{
    auto context = std::make_shared<net::ModJoinDownloadContext>();
    context->shared = std::make_shared<net::ModJoinDownloads>();
    // (Add-Ons) The mods that are on, and the server mods that are off: one
    // the host requires is used as it is, rather than downloaded again.
    if (mod_catalog_valid_)
        context->local = addon_join_catalog();
    context->options.cache_root = mod_download_cache_root();
    mod_join_download_ = context;
    return context;
}

// The join ended (admitted, failed or cancelled). On success the session runs
// the mods it selected, and the client half of the mod channel opens now so
// the host's welcome cannot arrive before it.
void mod_net_join_finished(bool admitted)
{
    std::shared_ptr<net::ModJoinDownloadContext> context = std::move(mod_join_download_);
    if (!admitted || context == nullptr)
        return;
    mod_join_session_mods_ = context->selection.mods;
    if (online_client_ == nullptr || !online_client_->mod_channel_open())
        return;
    stop_mod_net_session();
    mod_net_client_ = std::make_unique<modding::ModNetClient>(context->selection.manifest);
    attach_mod_net_objects();
}

// The text fallback of the download prompt on the join panel. The UI area
// draws the real dialog from mod_join_download_->shared->status().
void update_mod_download_prompt()
{
    if (mod_join_download_ == nullptr || mod_join_download_->shared == nullptr)
        return;
    const net::ModDownloadStatus status = mod_join_download_->shared->status();
    if (status.phase == net::ModDownloadPhase::idle || status.finished())
        return;
    const net::ModDownloadPromptText text = net::describe_mod_download(status);
    std::string line = text.title + "  /  " + text.body;
    std::replace(line.begin(), line.end(), '\n', ' ');
    if (status.phase == net::ModDownloadPhase::awaiting_consent)
        line += "  /  ENTER: DOWNLOAD AND JOIN   ESC: CANCEL";
    online_join_detail_ = uppercase(std::move(line));
}

// Join-panel input while a download waits for an answer: Enter (or A)
// accepts. Escape still cancels the join through the panel's own handling,
// and the waiting download is told to stop.
[[nodiscard]] bool mod_download_input(const SDL_Event &event)
{
    if (mod_join_download_ == nullptr || mod_join_download_->shared == nullptr)
        return false;
    net::ModJoinDownloads &downloads = *mod_join_download_->shared;
    const bool escape = (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) ||
                        (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                         event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST);
    if (escape)
    {
        downloads.decline();
        downloads.cancel();
        return false;
    }
    if (!downloads.awaiting_consent())
        return false;
    const bool accept = (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                         (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) ||
                        (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                         event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH);
    if (!accept)
        return false;
    downloads.accept();
    return true;
}

// The profile a browser row is judged against, matching what the join will
// present (net::select_session_mods): this client's own client mods plus
// exactly the server/core mods the lobby requires, since a missing one is
// downloaded on join and one the host does not use is left out.
[[nodiscard]] net::ModManifest mod_net_assessment_manifest(const net::DiscoveredSession &session,
                                                           net::ProtocolVersion protocol) const
{
    net::ModManifest manifest = join_manifest_for(protocol);
    std::erase_if(manifest.mods,
                  [](const net::ModDescriptor &mod) { return mod.scope != net::ModScope::client; });
    if (session.required_manifest.has_value())
        for (const net::ModDescriptor &mod : session.required_manifest->mods)
            if (mod.scope != net::ModScope::client)
                manifest.mods.push_back(mod);
    std::string error;
    return net::validate_mod_manifest(manifest, error) ? manifest : join_manifest_for(protocol);
}
