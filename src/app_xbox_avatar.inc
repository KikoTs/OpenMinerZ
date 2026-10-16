// Member functions of Application::Impl: the "Xbox Avatars" add-on.
// Included inside the class body by app.cpp.
//
// Everything the application does for the add-on is here, and all of it is
// presentation: which skin each player wears, the two switches on the Add-Ons
// screen, and carrying the add-on's own network exchange (XboxAvatarAddon,
// net::AvatarSyncSession) over whichever session role this client is in.
//
// Two rules decide what may go on the wire, and neither lives in this file:
//   * the capability marker rides a duplicate stock PlayerExistsMessage that
//     every client can read (LegacySteamRuntime*::send_avatar_capability);
//   * the add-on's own message goes only to a gamer who announced the add-on -
//     LegacySteamRuntime*::send_avatar refuses anyone else, whatever is asked
//     of it here.

void configure_xbox_avatars()
{
    XboxAvatarAddon::Options options;
    options.show_mine = settings_.xbox_avatar_show_mine;
    options.show_others = settings_.xbox_avatar_show_others;
    if (!settings_.xbox_avatar_file.empty())
        options.file = std::filesystem::path{
            std::u8string{settings_.xbox_avatar_file.begin(), settings_.xbox_avatar_file.end()}};
    xbox_avatars_.configure(options);
}

// Capture aid: OPENMINERZ_DIAGNOSTIC_REMOTE_AVATAR=<file|local> dresses the
// --diagnostic-multiplayer players, through the same verification and decoding
// a received avatar goes through. OPENMINERZ_DIAGNOSTIC_REMOTE_AVATAR_GIDS=0,3
// limits it to some of them (the rest stay stock, for comparison).
void start_xbox_avatar_diagnostics()
{
    // A capture is taken on a known frame: decode before it, not during it.
    xbox_avatar_deterministic_ = options_.screenshot_path.has_value() || options_.bench_frames > 0;
    const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_REMOTE_AVATAR");
    if (value == nullptr || !options_.diagnostic_multiplayer)
        return;
    xbox_avatars_.wait_idle();
    std::filesystem::path file = std::string_view{value} == "local"
                                     ? xbox_avatars_.status().file
                                     : std::filesystem::path{std::u8string{
                                           value, value + std::char_traits<char>::length(value)}};
    std::array<bool, 256> chosen{};
    chosen.fill(true);
    if (const char *gids = SDL_getenv("OPENMINERZ_DIAGNOSTIC_REMOTE_AVATAR_GIDS"))
    {
        chosen.fill(false);
        for (const char *cursor = gids; *cursor != '\0';)
        {
            char *end{};
            const long gid = std::strtol(cursor, &end, 10);
            if (end == cursor)
                break;
            if (gid >= 0 && gid < 256)
                chosen[static_cast<std::size_t>(gid)] = true;
            cursor = *end == ',' ? end + 1 : end;
        }
    }
    for (const auto &[gid, remote] : online_remote_players_)
        if (chosen[gid])
            xbox_avatars_.diagnostic_remote(gid, file);
}

// Once per frame, after the session has been pumped.
void update_xbox_avatars()
{
    const bool hosting = online_host_ != nullptr && online_host_->running();
    const bool joined = online_client_ != nullptr && online_client_joined_ &&
                        online_client_->connected() && online_bootstrap_client_ == nullptr;
    const bool session = hosting || joined;
    if (session != xbox_avatars_.in_session())
    {
        if (session)
        {
            // This client's own presence has been announced by now (the host's
            // is part of every joiner's bootstrap; a client's bootstrap ends
            // with the echo of its own), so the marker can only ever follow it.
            xbox_avatars_.session_started(hosting ? host_settings_.protocol : online_client_protocol_);
            xbox_avatar_greeted_.fill(false);
        }
        else
            xbox_avatars_.session_ended();
    }
    if (session)
    {
        // A player who has just announced themselves has not seen the marker.
        // It is sent after this client's own answer to that announcement, so a
        // stock client always reads it as the duplicate presence it ignores.
        for (const auto &[gid, remote] : online_remote_players_)
            if (remote.presence_announced && !xbox_avatar_greeted_[gid])
            {
                xbox_avatar_greeted_[gid] = true;
                xbox_avatars_.request_advertisement();
            }
        if (xbox_avatars_.advertisement_due())
        {
            std::string error;
            if (hosting ? online_host_->send_avatar_capability(error)
                        : online_client_->send_avatar_capability(error))
                xbox_avatars_.advertisement_sent();
            else
                std::cerr << "xbox-avatar.capability.failure=" << error << '\n';
        }
    }
    xbox_avatars_.update();
    for (const net::AvatarSyncSend &send : xbox_avatars_.take_sends())
    {
        if (!session)
            continue;
        std::string error;
        if (!(hosting ? online_host_->send_avatar(send.recipient_gid, send.packet, error)
                      : online_client_->send_avatar(send.recipient_gid, send.packet, error)))
            // Refused by the session's own gate (the peer left, or never
            // announced the add-on there): nothing was sent.
            std::cerr << "xbox-avatar.send.refused gid=" << static_cast<int>(send.recipient_gid)
                      << " reason=" << error << '\n';
    }
}

// Session events, from the two pumps.
void xbox_avatar_capability(std::uint8_t gid, std::span<const std::uint8_t> payload)
{
    if (payload.size() == 1U)
        xbox_avatars_.peer_capable(gid, payload.front());
}

void xbox_avatar_packet(std::uint8_t gid, std::span<const std::uint8_t> packet)
{
    xbox_avatars_.packet_received(gid, packet);
}

void xbox_avatar_player_left(std::uint8_t gid) noexcept
{
    xbox_avatar_greeted_[gid] = false;
    xbox_avatars_.peer_left(gid);
    if (world_preview_ != nullptr)
        world_preview_->set_player_avatar(gid, nullptr);
}

void xbox_avatar_players_cleared() noexcept
{
    for (const auto &[gid, remote] : online_remote_players_)
        xbox_avatar_player_left(gid);
}

// Before the world is drawn: who wears what, restated every frame. The renderer
// keeps a player's current character until a new skin is completely uploaded,
// so a change here never shows half an avatar.
void present_xbox_avatars()
{
    if (world_preview_ == nullptr)
        return;
    if (xbox_avatar_deterministic_)
        xbox_avatars_.wait_idle();
    world_preview_->set_player_avatar(256U, xbox_avatars_.local_skin());
    for (const auto &[gid, remote] : online_remote_players_)
        world_preview_->set_player_avatar(gid, xbox_avatars_.remote_skin(gid));
    if (xbox_avatar_deterministic_)
        world_preview_->finish_player_avatars();
}

// The Add-Ons screen's rows.
[[nodiscard]] std::string xbox_avatar_setting_value(ui::MenuCommand command) const
{
    switch (command)
    {
    case ui::MenuCommand::toggle_avatar_mine:
        return settings_.xbox_avatar_show_mine ? "ON" : "OFF";
    case ui::MenuCommand::toggle_avatar_others:
        return settings_.xbox_avatar_show_others ? "ON" : "OFF";
    default:
        return {};
    }
}

void handle_xbox_avatar_command(ui::MenuCommand command)
{
    switch (command)
    {
    case ui::MenuCommand::toggle_avatar_mine:
        settings_.xbox_avatar_show_mine = !settings_.xbox_avatar_show_mine;
        break;
    case ui::MenuCommand::toggle_avatar_others:
        settings_.xbox_avatar_show_others = !settings_.xbox_avatar_show_others;
        break;
    case ui::MenuCommand::reload_avatar:
        xbox_avatars_.reload();
        return;
    default:
        return;
    }
    // Applied at once: the next frame draws (and the session offers) the result.
    configure_xbox_avatars();
    dirty_ = true;
}

// What the screen says about the local avatar, in one or two plain sentences.
[[nodiscard]] std::string xbox_avatar_status_text() const
{
    const XboxAvatarAddon::Status status = xbox_avatars_.status();
    std::string text;
    if (!status.file_found)
        text = "No avatar file found. Export one with the Xbox Avatar add-on's importer, or put "
               "avatar.ocavatar in the OpenMinerZ folder under your local app data.";
    else
    {
        const std::u8string file = status.file.u8string();
        text = "Avatar file: " + std::string{file.begin(), file.end()} + ". ";
        if (status.loaded && status.error.empty())
            text += std::format("Loaded ({} triangles).", status.triangles);
        else if (status.loaded)
            text += "The newest file could not be used (" + status.error +
                    "); still wearing the last good one.";
        else if (status.loading)
            text += "Loading.";
        else
            text += "It could not be used: " + status.error + ".";
    }
    if (status.in_session)
        text += std::format(" In this session: {} player{} can exchange avatars, {} shown.",
                            status.capable_peers, status.capable_peers == 1U ? "" : "s",
                            status.remote_avatars);
    return text;
}

XboxAvatarAddon xbox_avatars_;
// Remote players who have been sent the capability marker since they announced.
std::array<bool, 256> xbox_avatar_greeted_{};
bool xbox_avatar_deterministic_{};
