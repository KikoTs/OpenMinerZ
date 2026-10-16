#include "openminerz/net/legacy_steam_runtime_host.hpp"
#include "openminerz/net/combat_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace openminerz::net
{
namespace
{

SteamBridgeJoinPolicy bridge_join_policy(const HostSessionSettings &settings) noexcept
{
    if (settings.visibility == HostVisibility::invite_only ||
        settings.join_policy == HostJoinPolicy::invite_only)
        return SteamBridgeJoinPolicy::invite_only;
    if (settings.visibility == HostVisibility::friends_only ||
        settings.join_policy == HostJoinPolicy::allow_list)
        return SteamBridgeJoinPolicy::friends_only;
    return SteamBridgeJoinPolicy::anyone;
}

LegacyGamerIdentity host_identity(PeerId peer, std::uint64_t lobby, std::string persona)
{
    LegacyGamerIdentity identity;
    identity.player_id.resize(16U);
    for (std::size_t index{}; index < 8U; ++index)
    {
        identity.player_id[index] = static_cast<std::uint8_t>(peer >> (index * 8U));
        identity.player_id[index + 8U] = static_cast<std::uint8_t>(lobby >> (index * 8U));
    }
    identity.gamer_tag = std::move(persona);
    return identity;
}

} // namespace

bool make_steam_bridge_host_config(const LegacySteamRuntimeHostOptions &options,
                                   SteamBridgeHostConfig &config, std::string &error) noexcept
{
    try
    {
        if (!options.settings.validate(error))
            return false;
        if (options.settings.visibility == HostVisibility::lan)
        {
            error = "LAN visibility requires the LAN transport, not the Steam bridge";
            return false;
        }
        SteamBridgeHostConfig converted;
        converted.maximum_players = static_cast<std::uint8_t>(options.settings.maximum_players);
        converted.join_policy = bridge_join_policy(options.settings);
        converted.password_protected = !options.settings.password.empty();
        converted.server_name = options.settings.server_name;
        converted.session_properties =
            make_session_properties(options.settings, false, options.game_mode, options.difficulty,
                                    options.infinite_resources);
        ExtensionMessage advertisement;
        advertisement.kind = ExtensionMessageKind::server_accept;
        advertisement.manifest = options.settings.mod_manifest;
        converted.extension_metadata = encode_extension_message(advertisement);
        SteamBridgeProtocolError protocol_error{};
        if (!encode_steam_bridge_host_config(converted, protocol_error).has_value())
        {
            error = "The host settings cannot be represented by the stock Steam lobby contract";
            return false;
        }
        config = std::move(converted);
        error.clear();
        return true;
    }
    catch (...)
    {
        error = "The Steam lobby configuration could not be constructed";
        return false;
    }
}

LegacySteamJoinNoticeText describe_join_notice(const LegacySteamJoinNotice &notice)
{
    const std::string who =
        notice.name.empty() ? "Steam user " + std::to_string(notice.peer) : notice.name;
    // Plain wording for the stock denial codes the joining client is sent.
    const auto explain = [](std::string_view reason) -> std::string {
        if (reason == "IncorrectPassword")
            return "wrong or missing password";
        if (reason == "ServerHasNewerVersion")
            return "their game is an older version than this server (Classic joining Modern)";
        if (reason == "ServerHasOlderVersion")
            return "their game is a newer version than this server (Modern joining Classic)";
        if (reason == "GameNamesDontMatch" || reason == "VersionIsInvalid")
            return "the join request was not a valid CastleMiner Z request";
        if (reason == "SessionPropertiesDontMatch")
            return "their server list is out of date; they should refresh and retry";
        if (reason == "GamerAlreadyConnected")
            return "that Steam account is already connected to this game";
        if (reason == "GameIsFull")
            return "the server is full";
        if (reason == "ConnectionDenied")
            return "this server's join policy or required add-ons do not allow them";
        if (reason == legacy_openminerz_required_reason)
            return "they are not running OpenMinerZ, which this server requires (server mods or "
                   "OpenMinerZ players only)";
        if (reason == "PeerInventoryUnavailable")
            return "their saved inventory for this world could not be read";
        if (reason == "NoJoinRequestReceived")
            return "Steam never delivered a join request (peer-to-peer or firewall problem)";
        if (reason == "JoinNotCompleted")
            return "they left before the join finished";
        if (reason == "NoWorldInfoRequest")
            return "it has not asked for the world yet";
        if (reason == "NoTerrainRequest")
            return "it received the world but has not asked for terrain";
        if (reason == "StillLoadingTerrain")
            return "it is still loading terrain";
        if (reason.starts_with("ModHandshakeRejected: "))
            return "add-ons do not match (" +
                   std::string{reason.substr(std::string_view{"ModHandshakeRejected: "}.size())} +
                   ")";
        return std::string{reason};
    };
    const char *stage = "entered";
    LegacySteamJoinNoticeText text;
    switch (notice.kind)
    {
    case LegacySteamJoinNoticeKind::entered_lobby:
        text.title = "PLAYER CONNECTING";
        text.body = who + " entered the lobby. Waiting for the join request.";
        break;
    case LegacySteamJoinNoticeKind::refused:
        stage = "refused";
        text.title = "JOIN REFUSED";
        text.body = who + " was refused: " + explain(notice.reason) + ".";
        break;
    case LegacySteamJoinNoticeKind::request_overdue:
        stage = "stalled";
        text.title = "JOIN IS NOT ARRIVING";
        text.body = who + " has been in the lobby for " + std::to_string(notice.waited_seconds) +
                    "s: " + explain(notice.reason) + ".";
        break;
    case LegacySteamJoinNoticeKind::loading_overdue:
        stage = "stalled";
        text.title = "PLAYER STILL LOADING";
        text.body = who + " connected " + std::to_string(notice.waited_seconds) +
                    "s ago but is not in the world: " + explain(notice.reason) + ".";
        break;
    case LegacySteamJoinNoticeKind::left_before_admission:
        stage = "abandoned";
        text.title = "JOIN DID NOT COMPLETE";
        text.body = who + " left without joining: " + explain(notice.reason) + ".";
        break;
    }
    text.log = std::string{"network.host.join "} + stage + " peer=" + std::to_string(notice.peer) +
               " name=\"" + notice.name + "\" waited_s=" + std::to_string(notice.waited_seconds) +
               " reason=" + (notice.reason.empty() ? std::string{"-"} : notice.reason);
    return text;
}

LegacySteamRuntimeHost::~LegacySteamRuntimeHost()
{
    std::string ignored;
    static_cast<void>(stop(ignored));
}

LegacySteamRuntimeHost::LegacySteamRuntimeHost(LegacySteamRuntimeHost &&) noexcept = default;
LegacySteamRuntimeHost &LegacySteamRuntimeHost::operator=(LegacySteamRuntimeHost &&) noexcept =
    default;

bool LegacySteamRuntimeHost::start(const LegacySteamRuntimeHostOptions &options,
                                   SteamBridgeHostReady &ready, std::string &error)
{
    if (session_.has_value() || transport_running())
    {
        error = "The legacy Steam host is already running";
        return false;
    }
    SteamBridgeHostConfig config;
    if (!make_steam_bridge_host_config(options, config, error))
        return false;
    if (options.trace_path.has_value())
    {
        trace_.emplace();
        if (!trace_->open(*options.trace_path, ProtocolTraceRole::host,
                          options.settings.protocol, error))
        {
            trace_.reset();
            return false;
        }
    }
    SteamBridgeClientOptions bridge_options;
    bridge_options.helper_path = options.helper_path;
    bridge_options.steam_api_path = options.steam_api_path;
    if (!bridge_.start_host(bridge_options, config, ready, error))
    {
        if (trace_.has_value())
            static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                             ProtocolTraceResult::failure, "HostStartFailed", 0U,
                                             std::nullopt,
                                             MessageDeliveryPolicy::reliable_ordered, 0U, 0U,
                                             error));
        trace_.reset();
        return false;
    }
    lobby_config_ = std::move(config);
    open_session(options, ready);
    error.clear();
    return true;
}

bool LegacySteamRuntimeHost::start_detached(const LegacySteamRuntimeHostOptions &options,
                                            const SteamBridgeHostReady &identity,
                                            LegacySteamDetachedTransport transport,
                                            std::string &error)
{
    if (session_.has_value() || transport_running())
    {
        error = "The legacy Steam host is already running";
        return false;
    }
    SteamBridgeHostConfig config;
    if (!make_steam_bridge_host_config(options, config, error))
        return false;
    if (!transport.send || identity.local_peer == 0U || identity.lobby_id == 0U ||
        identity.persona_name.empty())
    {
        error = "A detached legacy Steam host needs a send sink and a host identity";
        return false;
    }
    detached_ = std::move(transport);
    detached_inbound_.clear();
    lobby_config_ = std::move(config);
    open_session(options, identity);
    error.clear();
    return true;
}

void LegacySteamRuntimeHost::inject(SteamBridgeClientEvent event)
{
    if (detached_.has_value())
        detached_inbound_.push_back(std::move(event));
}

void LegacySteamRuntimeHost::open_session(const LegacySteamRuntimeHostOptions &options,
                                          const SteamBridgeHostReady &ready)
{
    pre_presence_packets_.clear();
    pre_presence_bytes_ = 0U;
    join_progress_.clear();
    next_overdue_check_ = {};
    LegacySteamHostSessionOptions session_options;
    session_options.settings = options.settings;
    session_options.join_closed = false;
    session_options.game_mode = options.game_mode;
    session_options.difficulty = options.difficulty;
    session_options.infinite_resources = options.infinite_resources;
    const LegacyGamerIdentity local_host =
        host_identity(ready.local_peer, ready.lobby_id, ready.persona_name);
    session_options.host_gamer = local_host;
    host_gamer_ = local_host;
    // SteamBridge32 accepts native sessions and forwards traffic only for IDs in
    // the current lobby. Membership in a friends/invite lobby is the platform
    // authorization consumed by the source-shaped session gate.
    session_options.platform_authorized = [](PeerId) { return true; };
    session_.emplace(std::move(session_options));
    protocol_ = options.settings.protocol;
    mod_manifest_ = options.settings.mod_manifest;
    if (options.bootstrap_snapshot.has_value())
    {
        AuthoritativeBootstrapSnapshot snapshot = *options.bootstrap_snapshot;
        if (snapshot.world.owner_gamer_tag.empty())
            snapshot.world.owner_gamer_tag = ready.persona_name;
        if (snapshot.world.creator_gamer_tag.empty())
            snapshot.world.creator_gamer_tag = ready.persona_name;
        PlayerExistsPayload host_presence;
        // The stock client announces `new AvatarDescription(new byte[10])`.
        host_presence.avatar_description.assign(10U, 0U);
        host_presence.gamer_tag = local_host.gamer_tag;
        host_presence.player_id = local_host.player_id;
        snapshot.host_presence = std::move(host_presence);
        bootstrap_.emplace(options.settings.protocol, std::move(snapshot),
                           options.inventory_directory);
    }
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "HostStarted"));
}

bool LegacySteamRuntimeHost::transport_running() const noexcept
{
    return detached_.has_value() || bridge_.running();
}

bool LegacySteamRuntimeHost::next_bridge_event(std::optional<SteamBridgeClientEvent> &event,
                                               std::string &error)
{
    if (!detached_.has_value())
        return bridge_.poll(event, error);
    event.reset();
    if (!detached_inbound_.empty())
    {
        event = std::move(detached_inbound_.front());
        detached_inbound_.pop_front();
    }
    error.clear();
    return true;
}

bool LegacySteamRuntimeHost::send_wire(const SteamBridgeOutboundWire &datagram,
                                       std::string &error)
{
    return detached_.has_value() ? detached_->send(datagram, error)
                                 : bridge_.send(datagram, error);
}

LegacySteamRuntimeHost::Clock::time_point LegacySteamRuntimeHost::now() const
{
    return detached_.has_value() && detached_->now ? detached_->now() : Clock::now();
}

LegacySteamRuntimeHost::JoinProgress &LegacySteamRuntimeHost::track_member(PeerId peer)
{
    const auto [found, inserted] = join_progress_.try_emplace(peer);
    if (inserted)
        found->second.entered = now();
    return found->second;
}

void LegacySteamRuntimeHost::note_refusal(PeerId peer, std::string name, std::string reason,
                                          std::vector<LegacySteamRuntimeEvent> &events)
{
    JoinProgress &progress = track_member(peer);
    progress.request_seen = true;
    std::string shown = name.empty() ? progress.name : std::move(name);
    // A request refused because the account is already playing says nothing
    // about that live player; keep its record as it is.
    if (!progress.is_admitted)
    {
        progress.name = shown;
        progress.refusal = reason;
    }
    LegacySteamRuntimeEvent event;
    event.kind = LegacySteamRuntimeEventKind::join_notice;
    event.peer = peer;
    event.detail = reason;
    event.notice = {LegacySteamJoinNoticeKind::refused, peer, std::move(shown), std::move(reason),
                    0U};
    events.push_back(std::move(event));
}

void LegacySteamRuntimeHost::report_overdue_joins(std::vector<LegacySteamRuntimeEvent> &events)
{
    const Clock::time_point current = now();
    if (current < next_overdue_check_)
        return;
    next_overdue_check_ = current + std::chrono::seconds{1};
    const auto waited = [&](Clock::time_point since) {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(current - since).count());
    };
    for (auto &[peer, progress] : join_progress_)
    {
        std::optional<LegacySteamJoinNotice> notice;
        if (!progress.is_admitted)
        {
            // A refused player has already been reported with its reason.
            if (!progress.request_seen && !progress.request_overdue_reported &&
                current - progress.entered >= legacy_steam_join_request_overdue)
            {
                progress.request_overdue_reported = true;
                notice = LegacySteamJoinNotice{LegacySteamJoinNoticeKind::request_overdue, peer,
                                               progress.name, "NoJoinRequestReceived",
                                               waited(progress.entered)};
            }
        }
        else if (!progress.loading_overdue_reported &&
                 current - progress.admitted >= legacy_steam_join_loading_overdue)
        {
            const std::optional<std::uint8_t> gid = session_->gid_for(peer);
            if (bootstrap_.has_value() && gid.has_value() &&
                !bootstrap_->gameplay_ready(peer, *gid))
            {
                progress.loading_overdue_reported = true;
                const std::optional<PeerBootstrapStatus> status = bootstrap_->status(peer);
                const char *phase = !status.has_value() || !status->world_info_sent
                                        ? "NoWorldInfoRequest"
                                    : status->chunks_sent == 0U && !status->delta_list_sent
                                        ? "NoTerrainRequest"
                                        : "StillLoadingTerrain";
                notice = LegacySteamJoinNotice{LegacySteamJoinNoticeKind::loading_overdue, peer,
                                               progress.name, phase, waited(progress.admitted)};
            }
        }
        if (!notice.has_value())
            continue;
        if (trace_.has_value())
            static_cast<void>(trace_->record(
                ProtocolTraceDirection::internal, ProtocolTraceResult::failure,
                notice->kind == LegacySteamJoinNoticeKind::request_overdue ? "JoinRequestOverdue"
                                                                           : "JoinLoadingOverdue",
                session_->gid_for(peer).value_or(0U), std::nullopt,
                MessageDeliveryPolicy::reliable_ordered, 0U, 0U, notice->reason));
        LegacySteamRuntimeEvent event;
        event.kind = LegacySteamRuntimeEventKind::join_notice;
        event.peer = peer;
        event.detail = notice->reason;
        event.notice = std::move(*notice);
        events.push_back(std::move(event));
    }
}

bool LegacySteamRuntimeHost::pump(std::vector<LegacySteamRuntimeEvent> &events, std::string &error)
{
    events.clear();
    if (!session_.has_value() || !transport_running())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    report_overdue_joins(events);
    for (std::size_t count{}; count < 256U; ++count)
    {
        std::optional<SteamBridgeClientEvent> bridge_event;
        if (!next_bridge_event(bridge_event, error))
            return false;
        if (!bridge_event.has_value())
            break;
        switch (bridge_event->kind)
        {
        case SteamBridgeClientEventKind::inbound_wire: {
            // Any datagram proves Steam P2P reached us, so this member is no
            // longer a candidate for the "no join request arrived" notice.
            track_member(bridge_event->inbound.peer).request_seen = true;
            // (Network) In a session with server mods, mod channel and mod
            // download frames go to the app's mod layer, never to the hello
            // path or the stock session. Without, nothing changes: they take
            // the hello path exactly as before.
            if (bridge_event->inbound.p2p_channel == 0U && requires_extension(mod_manifest_) &&
                (is_mod_channel_packet(bridge_event->inbound.wire) ||
                 is_mod_download_packet(bridge_event->inbound.wire)))
            {
                const PeerId sender = bridge_event->inbound.peer;
                const bool download = is_mod_download_packet(bridge_event->inbound.wire);
                const bool allowed = download || mod_channel_peer(sender);
                if (trace_.has_value())
                    static_cast<void>(trace_->record(
                        ProtocolTraceDirection::inbound,
                        allowed ? ProtocolTraceResult::accepted : ProtocolTraceResult::rejected,
                        download ? "ModDownloadFrame" : "ModChannelFrame",
                        session_->gid_for(sender).value_or(0U), std::uint8_t{0},
                        MessageDeliveryPolicy::reliable_ordered, 0U,
                        bridge_event->inbound.wire.size()));
                if (!allowed)
                {
                    events.push_back({LegacySteamRuntimeEventKind::rejected,
                                      {},
                                      {0U, sender, LegacySteamRejectionStage::routing,
                                       "UnexpectedModFrame", bridge_event->inbound.wire.size()},
                                      sender,
                                      "UnexpectedModFrame"});
                    continue;
                }
                LegacySteamHostInbound inbound;
                inbound.peer = sender;
                inbound.payload = std::move(bridge_event->inbound.wire);
                inbound.sender_gid = session_->gid_for(sender).value_or(0U);
                events.push_back({download ? LegacySteamRuntimeEventKind::mod_download
                                           : LegacySteamRuntimeEventKind::mod_channel,
                                  std::move(inbound),
                                  {},
                                  sender,
                                  {}});
                continue;
            }
            if (bridge_event->inbound.p2p_channel == 0U &&
                is_extension_packet(bridge_event->inbound.wire))
            {
                ExtensionMessage hello;
                ExtensionDecodeError decode_error{};
                ExtensionMessage response;
                response.kind = ExtensionMessageKind::server_reject;
                response.manifest = mod_manifest_;
                std::string rejection;
                if (!decode_extension_message(bridge_event->inbound.wire, hello, decode_error) ||
                    hello.kind != ExtensionMessageKind::client_hello)
                    rejection = "Malformed OpenMinerZ extension hello.";
                else
                {
                    response.nonce = hello.nonce;
                    const ModCompatibilityResult compatibility =
                        compare_mod_manifests(mod_manifest_, &hello.manifest, true);
                    if (!compatibility.compatible())
                        rejection = compatibility.detail;
                    else if (!session_->authorize_extension(bridge_event->inbound.peer))
                        rejection = "Duplicate OpenMinerZ extension hello.";
                }
                if (rejection.empty())
                    response.kind = ExtensionMessageKind::server_accept;
                else
                    response.detail = rejection.substr(0U, maximum_extension_detail_bytes);
                const std::vector<std::uint8_t> response_packet =
                    encode_extension_message(response);
                if (!send_wire({bridge_event->inbound.peer,
                                SteamDeliveryMethod::reliable_ordered, 0U, response_packet},
                               error))
                    return false;
                if (trace_.has_value())
                    static_cast<void>(trace_->record(
                        ProtocolTraceDirection::inbound,
                        rejection.empty() ? ProtocolTraceResult::accepted
                                          : ProtocolTraceResult::rejected,
                        rejection.empty() ? "ModHandshakeAccepted" : "ModHandshakeRejected", 0U,
                        std::uint8_t{0}, MessageDeliveryPolicy::reliable_ordered,
                        bridge_event->inbound.p2p_channel, bridge_event->inbound.wire.size(),
                        rejection));
                if (!rejection.empty())
                {
                    events.push_back({LegacySteamRuntimeEventKind::rejected,
                                      {},
                                      {},
                                      bridge_event->inbound.peer,
                                      rejection});
                    note_refusal(bridge_event->inbound.peer, {},
                                 "ModHandshakeRejected: " + rejection, events);
                }
                continue;
            }
            if (bridge_event->inbound.p2p_channel != 0U)
            {
                events.push_back(
                    {LegacySteamRuntimeEventKind::rejected,
                     {},
                     {0U, bridge_event->inbound.peer, LegacySteamRejectionStage::routing,
                      "UnexpectedNativeChannel", bridge_event->inbound.wire.size()},
                     bridge_event->inbound.peer,
                     "UnexpectedNativeChannel"});
                break;
            }
            LegacySteamHostSessionResult result = session_->process_wire_packet(
                bridge_event->inbound.peer, bridge_event->inbound.wire);
            if (!send_all(result.sends, error))
                return false;
            if (result.audit.has_value())
            {
                if (trace_.has_value())
                    static_cast<void>(trace_->record(
                        ProtocolTraceDirection::inbound, ProtocolTraceResult::rejected,
                        "PacketRejected", 0U, std::nullopt,
                        MessageDeliveryPolicy::reliable_ordered,
                        bridge_event->inbound.p2p_channel, bridge_event->inbound.wire.size(),
                        result.audit->reason));
                events.push_back({LegacySteamRuntimeEventKind::rejected,
                                  {},
                                  *result.audit,
                                  result.audit->peer,
                                  result.audit->reason});
                // An admission refusal has already been sent to the joiner on
                // native channel one; surface the same who/why to the host.
                if (result.rejection_reason.has_value())
                    note_refusal(result.audit->peer, result.audit->gamer_tag,
                                 *result.rejection_reason, events);
            }
            for (LegacySteamHostInbound &inbound : result.inbound)
            {
                if (inbound.kind == LegacySteamInboundKind::avatar_capability ||
                    inbound.kind == LegacySteamInboundKind::avatar_sync)
                {
                    // The Xbox Avatar add-on: not gameplay, so it passes neither
                    // the bootstrap nor gameplay authority. The session has
                    // already checked who sent it and what it is.
                    const bool capability =
                        inbound.kind == LegacySteamInboundKind::avatar_capability;
                    if (trace_.has_value())
                        static_cast<void>(trace_->record(
                            ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                            capability ? "AvatarCapabilityAnnounced" : "AvatarSyncMessage",
                            inbound.sender_gid, std::uint8_t{0},
                            MessageDeliveryPolicy::reliable_ordered, inbound.channel,
                            inbound.payload.size()));
                    events.push_back({capability ? LegacySteamRuntimeEventKind::avatar_capability
                                                 : LegacySteamRuntimeEventKind::avatar_sync,
                                      std::move(inbound),
                                      {},
                                      bridge_event->inbound.peer,
                                      {}});
                    continue;
                }
                if (inbound.kind == LegacySteamInboundKind::peer_connected)
                {
                    JoinProgress &progress = track_member(inbound.peer);
                    progress.is_admitted = true;
                    progress.admitted = now();
                    progress.refusal.clear();
                    if (const std::string *tag =
                            session_->self_reported_tag_for_gid(inbound.sender_gid);
                        tag != nullptr && !tag->empty())
                        progress.name = *tag;
                }
                const LegacySteamRuntimeEventKind kind =
                    inbound.kind == LegacySteamInboundKind::peer_connected
                         ? LegacySteamRuntimeEventKind::peer_connected
                         : LegacySteamRuntimeEventKind::gameplay;
                if (trace_.has_value())
                {
                    if (inbound.kind == LegacySteamInboundKind::peer_connected)
                        static_cast<void>(trace_->record(
                            ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                            "PeerConnected", inbound.sender_gid, std::uint8_t{0},
                            MessageDeliveryPolicy::reliable_ordered, inbound.channel,
                            bridge_event->inbound.wire.size()));
                }
                if (inbound.kind == LegacySteamInboundKind::peer_connected &&
                    bootstrap_.has_value())
                {
                    if (!bootstrap_->register_peer(inbound.peer, inbound.sender_gid))
                    {
                        // A damaged guest save must neither reset their progress nor
                        // shut down the host and every other player's session.
                        PayloadError payload_error{};
                        PacketError packet_error{};
                        const auto payload =
                            encode_kick_payload({false, inbound.sender_gid}, payload_error);
                        const auto *descriptor = message_by_name(protocol_, "KickMessage");
                        const auto packet =
                            payload && descriptor != nullptr
                                ? encode_packet(protocol_, descriptor->id, *payload, packet_error)
                                : std::nullopt;
                        if (!packet)
                        {
                            error = "Could not encode an unavailable-inventory disconnect";
                            return false;
                        }
                        LegacySteamOutbound kick;
                        kick.recipient = inbound.peer;
                        kick.application_packet = *packet;
                        if (!send_authoritative(kick, error))
                            return false;
                        const auto drops = session_->remove_peer(inbound.peer);
                        forget_pre_presence(inbound.peer);
                        if (!send_all(drops, error))
                            return false;
                        events.push_back({LegacySteamRuntimeEventKind::rejected,
                                          {},
                                          {},
                                          inbound.peer,
                                          "PeerInventoryUnavailable"});
                        track_member(inbound.peer).is_admitted = false;
                        note_refusal(inbound.peer, {}, "PeerInventoryUnavailable", events);
                        continue;
                    }
                }
                if (inbound.kind == LegacySteamInboundKind::gameplay && bootstrap_.has_value())
                {
                    BootstrapHandleResult bootstrap_result =
                        bootstrap_->handle_packet(inbound.peer, inbound.payload, inbound.channel,
                                                  inbound.recipient_gid == 0U);
                    if (bootstrap_result.handled())
                    {
                        if (trace_.has_value())
                            static_cast<void>(trace_->record_packet(
                                ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                                inbound.payload, inbound.sender_gid,
                                std::optional<std::uint8_t>{inbound.recipient_gid},
                                inbound.delivery, inbound.channel));
                        for (const AuthoritativeBootstrapReply &reply : bootstrap_result.replies)
                        {
                            LegacySteamOutbound outbound;
                            outbound.recipient = reply.peer;
                            outbound.application_packet = reply.application_packet;
                            outbound.delivery = reply.delivery;
                            if (reply.original_sender != 0U)
                            {
                                const auto sender_gid = session_->gid_for(reply.original_sender);
                                const auto recipient_gid = session_->gid_for(reply.peer);
                                if (!sender_gid.has_value() || !recipient_gid.has_value())
                                {
                                    error = "A bootstrap echo lost its admitted GID";
                                    return false;
                                }
                                outbound.channel = 1U;
                                outbound.sender_gid = *sender_gid;
                                outbound.recipient_gid = *recipient_gid;
                                outbound.client_relay = true;
                            }
                            if (!send_authoritative(outbound, error))
                                return false;
                        }
                        if (bootstrap_result.input_message == "PlayerExistsMessage" &&
                            bootstrap_->gameplay_ready(inbound.peer, inbound.sender_gid))
                            replay_pre_presence(inbound.peer, events);
                        continue;
                    }
                    if (bootstrap_result.error != BootstrapPumpError::unexpected_message)
                    {
                        if (trace_.has_value())
                            static_cast<void>(trace_->record_packet(
                                ProtocolTraceDirection::inbound, ProtocolTraceResult::rejected,
                                inbound.payload, inbound.sender_gid, std::uint8_t{0},
                                inbound.delivery, inbound.channel,
                                "InvalidOrOutOfOrderBootstrapMessage"));
                        // Name the message: a stalled join is diagnosed from which
                        // bootstrap request the host turned away.
                        events.push_back(
                            {LegacySteamRuntimeEventKind::rejected,
                             {},
                             {},
                             inbound.peer,
                             "InvalidOrOutOfOrderBootstrapMessage:" +
                                 std::string{bootstrap_result.input_message.empty()
                                                 ? std::string_view{"Undecodable"}
                                                 : bootstrap_result.input_message}});
                        continue;
                    }
                    if (!bootstrap_->gameplay_ready(inbound.peer, inbound.sender_gid))
                    {
                        if (queue_pre_presence(std::move(inbound),
                                               bootstrap_result.input_message))
                            continue;
                        if (trace_.has_value())
                            static_cast<void>(trace_->record_packet(
                                ProtocolTraceDirection::inbound, ProtocolTraceResult::rejected,
                                inbound.payload, inbound.sender_gid, std::uint8_t{0},
                                inbound.delivery, inbound.channel,
                                "PrePresenceQueueCapacityExceeded"));
                        events.push_back({LegacySteamRuntimeEventKind::rejected,
                                          {},
                                          {0U, inbound.peer,
                                           LegacySteamRejectionStage::application,
                                           "PrePresenceQueueCapacityExceeded",
                                           inbound.payload.size()},
                                          inbound.peer,
                                          "PrePresenceQueueCapacityExceeded"});
                        continue;
                    }
                }
                if (inbound.kind == LegacySteamInboundKind::gameplay && trace_.has_value())
                    static_cast<void>(trace_->record_packet(
                        ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                        inbound.payload, inbound.sender_gid,
                        std::optional<std::uint8_t>{inbound.recipient_gid}, inbound.delivery,
                        inbound.channel));
                events.push_back({kind, std::move(inbound), {}, bridge_event->inbound.peer, {}});
            }
            break;
        }
        case SteamBridgeClientEventKind::peer_left: {
            const std::optional<std::uint8_t> departing_gid = session_->gid_for(bridge_event->peer);
            if (bootstrap_.has_value())
                bootstrap_->disconnect(bridge_event->peer);
            const std::vector<LegacySteamNativeSend> drops =
                session_->remove_peer(bridge_event->peer);
            forget_pre_presence(bridge_event->peer);
            if (!send_all(drops, error))
                return false;
            JoinProgress progress;
            if (const auto found = join_progress_.find(bridge_event->peer);
                found != join_progress_.end())
            {
                progress = std::move(found->second);
                join_progress_.erase(found);
            }
            if (!departing_gid.has_value())
            {
                // This lobby member never held a GID. Reporting it as a player
                // disconnect would make the application tear down GID zero, which
                // is the host itself (its enemies, dragon and remote-player state).
                LegacySteamRuntimeEvent event;
                event.kind = LegacySteamRuntimeEventKind::join_notice;
                event.peer = bridge_event->peer;
                event.notice.kind = LegacySteamJoinNoticeKind::left_before_admission;
                event.notice.peer = bridge_event->peer;
                event.notice.name = std::move(progress.name);
                event.notice.reason = !progress.refusal.empty()  ? progress.refusal
                                      : progress.request_seen    ? "JoinNotCompleted"
                                                                 : "NoJoinRequestReceived";
                event.notice.waited_seconds = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(now() - progress.entered)
                        .count());
                event.detail = event.notice.reason;
                if (trace_.has_value())
                    static_cast<void>(trace_->record(
                        ProtocolTraceDirection::inbound, ProtocolTraceResult::rejected,
                        "LeftBeforeAdmission", 0U, std::nullopt,
                        MessageDeliveryPolicy::reliable_ordered, 0U, 0U, event.notice.reason));
                events.push_back(std::move(event));
                break;
            }
            LegacySteamHostInbound departing;
            departing.peer = bridge_event->peer;
            departing.sender_gid = *departing_gid;
            events.push_back({LegacySteamRuntimeEventKind::peer_disconnected,
                              std::move(departing),
                              {},
                              bridge_event->peer,
                              {}});
            if (trace_.has_value())
                static_cast<void>(trace_->record(
                    ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                    "PeerDisconnected", *departing_gid, std::uint8_t{0}));
            break;
        }
        case SteamBridgeClientEventKind::peer_entered: {
            JoinProgress &progress = track_member(bridge_event->peer);
            if (!bridge_event->peer_name.empty() && progress.name.empty())
                progress.name = bridge_event->peer_name;
            // A member that is already admitted, or whose request is already in
            // hand, needs no "is trying to join" notice.
            if (progress.is_admitted || progress.request_seen)
                break;
            if (trace_.has_value())
                static_cast<void>(trace_->record(ProtocolTraceDirection::inbound,
                                                 ProtocolTraceResult::accepted,
                                                 "PeerEnteredLobby"));
            LegacySteamRuntimeEvent event;
            event.kind = LegacySteamRuntimeEventKind::join_notice;
            event.peer = bridge_event->peer;
            event.notice = {LegacySteamJoinNoticeKind::entered_lobby, bridge_event->peer,
                            progress.name, {}, 0U};
            events.push_back(std::move(event));
            break;
        }
        case SteamBridgeClientEventKind::failure:
            if (trace_.has_value())
                static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                                 ProtocolTraceResult::failure,
                                                 "TransportFailure", 0U, std::nullopt,
                                                 MessageDeliveryPolicy::reliable_ordered, 0U, 0U,
                                                 bridge_event->failure.detail));
            events.push_back({LegacySteamRuntimeEventKind::transport_failure,
                              {},
                              {},
                              0U,
                              bridge_event->failure.detail});
            error = bridge_event->failure.detail;
            return false;
        case SteamBridgeClientEventKind::stopped:
            error = "The x86 Steam helper stopped unexpectedly";
            return false;
        case SteamBridgeClientEventKind::heartbeat:
            break;
        }
        // Deliver application mutations before consuming a later bootstrap
        // request. Otherwise a queued inventory store/block edit can be
        // overtaken by an inventory/chunk response built from stale state.
        if (!events.empty())
            break;
    }
    error.clear();
    return true;
}

bool LegacySteamRuntimeHost::send_authoritative(const LegacySteamOutbound &outbound,
                                                std::string &error)
{
    if (!session_.has_value() || !transport_running())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    const auto sends = session_->create_outbound(outbound, error);
    if (!sends.has_value() || !send_all(*sends, error))
        return false;
    if (trace_.has_value())
    {
        const std::uint8_t sender_gid = outbound.client_relay ? outbound.sender_gid : 0U;
        const std::optional<std::uint8_t> recipient_gid =
            outbound.client_relay
                ? std::optional<std::uint8_t>{outbound.recipient_gid}
            : outbound.recipient.has_value() ? session_->gid_for(*outbound.recipient)
                                             : std::nullopt;
        static_cast<void>(trace_->record_packet(
            ProtocolTraceDirection::outbound, ProtocolTraceResult::accepted,
            outbound.application_packet, sender_gid, recipient_gid, outbound.delivery,
            outbound.channel));
    }
    return true;
}

bool LegacySteamRuntimeHost::send_avatar(std::uint8_t recipient_gid,
                                         std::span<const std::uint8_t> avatar_packet,
                                         std::string &error)
{
    if (!session_.has_value() || !transport_running())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    // The session refuses any recipient that has not announced the add-on.
    const auto sends = session_->create_avatar_outbound(recipient_gid, avatar_packet, error);
    if (!sends.has_value() || !send_all(*sends, error))
        return false;
    if (trace_.has_value())
        static_cast<void>(trace_->record(
            ProtocolTraceDirection::outbound, ProtocolTraceResult::accepted, "AvatarSyncMessage",
            0U, std::optional<std::uint8_t>{recipient_gid},
            MessageDeliveryPolicy::reliable_ordered, 0U, avatar_packet.size()));
    return true;
}

bool LegacySteamRuntimeHost::avatar_capable(std::uint8_t gid) const noexcept
{
    return session_.has_value() && session_->avatar_capable(gid);
}

bool LegacySteamRuntimeHost::send_avatar_capability(std::string &error)
{
    if (!session_.has_value() || !transport_running())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    // A stock PlayerExistsMessage from the host gamer, broadcast: every client
    // can read it, and one without the add-on ignores a second presence.
    const auto packet = make_avatar_capability_presence(protocol_, host_gamer_.gamer_tag,
                                                        host_gamer_.player_id);
    if (!packet.has_value())
    {
        error = "Could not encode the avatar capability presence";
        return false;
    }
    LegacySteamOutbound outbound;
    outbound.application_packet = *packet;
    outbound.delivery = MessageDeliveryPolicy::reliable_ordered;
    return send_authoritative(outbound, error);
}

bool LegacySteamRuntimeHost::mod_channel_peer(PeerId peer) const noexcept
{
    return session_.has_value() && requires_extension(mod_manifest_) &&
           session_->extension_capable(peer) && session_->connected(peer);
}

bool LegacySteamRuntimeHost::send_mod_frame(PeerId peer, std::span<const std::uint8_t> frame,
                                            std::string &error)
{
    if (!session_.has_value() || !transport_running())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    const bool download = is_mod_download_packet(frame);
    if (!requires_extension(mod_manifest_) || (!download && !is_mod_channel_packet(frame)) ||
        frame.size() > maximum_extension_packet_size)
    {
        error = "Only a session with server mods sends mod frames";
        return false;
    }
    if (!download && !mod_channel_peer(peer))
    {
        error = "The peer has no mod channel";
        return false;
    }
    if (!send_wire({peer, SteamDeliveryMethod::reliable_ordered, 0U,
                    std::vector<std::uint8_t>{frame.begin(), frame.end()}},
                   error))
        return false;
    if (trace_.has_value())
        static_cast<void>(trace_->record(
            ProtocolTraceDirection::outbound, ProtocolTraceResult::accepted,
            download ? "ModDownloadFrame" : "ModChannelFrame", 0U,
            session_->gid_for(peer), MessageDeliveryPolicy::reliable_ordered, 0U, frame.size()));
    return true;
}

bool LegacySteamRuntimeHost::store_authoritative_inventory(PeerId peer,
                                                           const PlayerInventoryPayload &inventory)
{
    return bootstrap_.has_value() && bootstrap_->store_peer_inventory(peer, inventory);
}

bool LegacySteamRuntimeHost::apply_authoritative_crate_item(const ItemCratePayload &crate)
{
    return bootstrap_.has_value() && bootstrap_->apply_crate_item(crate);
}

bool LegacySteamRuntimeHost::destroy_authoritative_crate(IntVector3 location)
{
    return bootstrap_.has_value() && bootstrap_->destroy_crate(location);
}

bool LegacySteamRuntimeHost::set_authoritative_custom_door(IntVector3 location,
                                                           std::uint8_t model_name)
{
    return bootstrap_.has_value() && bootstrap_->set_custom_door(location, model_name);
}

bool LegacySteamRuntimeHost::destroy_authoritative_custom_block(IntVector3 location)
{
    return bootstrap_.has_value() && bootstrap_->destroy_custom_block(location);
}

bool LegacySteamRuntimeHost::update_authoritative_spawner(IntVector3 location, bool started)
{
    return bootstrap_.has_value() && bootstrap_->update_spawner(location, started);
}

bool LegacySteamRuntimeHost::update_session_property(std::size_t index, std::int32_t value,
                                                     std::string &error)
{
    if (!session_.has_value() || !transport_running())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    if (!session_->update_session_property(index, value, error))
        return false;
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted,
                                         "SessionPropertyUpdated"));
    if (!lobby_config_.has_value())
        return true;
    lobby_config_->session_properties = session_->session_properties();
    // A detached host has no lobby; the session's own copy is the whole of it.
    return detached_.has_value() || bridge_.update_host(*lobby_config_, error);
}

SessionProperties LegacySteamRuntimeHost::session_properties() const noexcept
{
    return session_.has_value() ? session_->session_properties() : SessionProperties{};
}

namespace
{
// string.IsNullOrWhiteSpace for the bytes a host can type here.
bool blank(std::string_view text) noexcept
{
    return std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return character == ' ' || (character >= '\t' && character <= '\r');
    });
}
} // namespace

bool LegacySteamRuntimeHost::update_server_message(std::string_view message, std::string &error)
{
    if (!session_.has_value() || !transport_running() || !lobby_config_.has_value())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    // UpdateHostSession: `if (!string.IsNullOrWhiteSpace(serverName)) Name = serverName;`
    if (blank(message))
    {
        error.clear();
        return true;
    }
    SteamBridgeHostConfig updated = *lobby_config_;
    updated.server_name = std::string{message};
    SteamBridgeProtocolError protocol_error{};
    if (!encode_steam_bridge_host_config(updated, protocol_error).has_value())
    {
        error = "That server message cannot be published in the Steam lobby";
        return false;
    }
    if (!detached_.has_value() && !bridge_.update_host(updated, error))
        return false;
    lobby_config_ = std::move(updated);
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "ServerMessageUpdated"));
    error.clear();
    return true;
}

bool LegacySteamRuntimeHost::update_password(std::string password, std::string &error)
{
    if (!session_.has_value() || !transport_running() || !lobby_config_.has_value())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    // HostOptionItems.Password: a blank entry is "no password" (the session
    // password becomes null); anything else is stored exactly as typed.
    if (blank(password))
        password.clear();
    SteamBridgeHostConfig updated = *lobby_config_;
    updated.password_protected = !password.empty();
    if (!session_->update_password(std::move(password), error))
        return false;
    if (!detached_.has_value() && !bridge_.update_host(updated, error))
        return false;
    lobby_config_ = std::move(updated);
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "PasswordUpdated"));
    error.clear();
    return true;
}

bool LegacySteamRuntimeHost::update_join_policy(SteamBridgeJoinPolicy policy, std::string &error)
{
    if (!session_.has_value() || !transport_running() || !lobby_config_.has_value())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    if (static_cast<std::uint8_t>(policy) > static_cast<std::uint8_t>(SteamBridgeJoinPolicy::invite_only))
    {
        error = "Unknown join policy";
        return false;
    }
    SteamBridgeHostConfig updated = *lobby_config_;
    updated.join_policy = policy;
    if (!detached_.has_value() && !bridge_.update_host(updated, error))
        return false;
    lobby_config_ = std::move(updated);
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "JoinPolicyUpdated"));
    error.clear();
    return true;
}

std::optional<SteamBridgeHostConfig> LegacySteamRuntimeHost::advertised_lobby() const
{
    return session_.has_value() ? lobby_config_ : std::nullopt;
}

bool LegacySteamRuntimeHost::update_hell_boss_progress(std::uint32_t spawned, std::uint32_t maximum)
{
    return bootstrap_.has_value() && bootstrap_->update_hell_boss_progress(spawned, maximum);
}

bool LegacySteamRuntimeHost::apply_authoritative_block_edit(IntVector3 position,
                                                            std::uint8_t block_type)
{
    return bootstrap_.has_value() && bootstrap_->apply_block_edit(position, block_type);
}

bool LegacySteamRuntimeHost::apply_authoritative_block_replacements(
    std::span<const IntVector3> positions, std::uint8_t block_type)
{
    return bootstrap_.has_value() && bootstrap_->apply_block_replacements(positions, block_type);
}

bool LegacySteamRuntimeHost::apply_authoritative_block_removals(
    std::span<const IntVector3> positions)
{
    return bootstrap_.has_value() && bootstrap_->apply_block_removals(positions);
}

bool LegacySteamRuntimeHost::stop(std::string &error) noexcept
{
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "RuntimeStopped"));
    bootstrap_.reset();
    session_.reset();
    lobby_config_.reset();
    mod_manifest_ = {};
    pre_presence_packets_.clear();
    pre_presence_bytes_ = 0U;
    join_progress_.clear();
    detached_inbound_.clear();
    const bool detached = detached_.has_value();
    detached_.reset();
    const bool stopped = bridge_.stop(error);
    trace_.reset();
    return detached || stopped;
}

bool LegacySteamRuntimeHost::running() const noexcept
{
    return session_.has_value() && transport_running();
}

std::optional<std::uint8_t> LegacySteamRuntimeHost::gid_for(PeerId peer) const noexcept
{
    return session_.has_value() ? session_->gid_for(peer) : std::nullopt;
}

std::optional<PeerId> LegacySteamRuntimeHost::peer_for_gid(std::uint8_t gid) const noexcept
{
    return session_.has_value() ? session_->peer_for_gid(gid) : std::nullopt;
}

const LegacyGamerIdentity *LegacySteamRuntimeHost::gamer_for_gid(std::uint8_t gid) const noexcept
{
    return session_.has_value() ? session_->gamer_for_gid(gid) : nullptr;
}

const std::string *LegacySteamRuntimeHost::self_reported_tag_for_gid(
    std::uint8_t gid) const noexcept
{
    return session_.has_value() ? session_->self_reported_tag_for_gid(gid) : nullptr;
}

bool LegacySteamRuntimeHost::gameplay_ready(PeerId peer, std::uint8_t player_gid) const noexcept
{
    return session_.has_value() && bootstrap_.has_value() &&
           session_->gid_for(peer) == std::optional<std::uint8_t>{player_gid} &&
           bootstrap_->gameplay_ready(peer, player_gid);
}

bool LegacySteamRuntimeHost::all_peers_extension_capable() const noexcept
{
    return session_.has_value() && session_->all_peers_extension_capable();
}

bool LegacySteamRuntimeHost::send_all(std::span<const LegacySteamNativeSend> sends,
                                      std::string &error)
{
    for (const LegacySteamNativeSend &send : sends)
    {
        if (!send_wire(SteamBridgeOutboundWire{send.remote_peer, send.delivery, send.p2p_channel,
                                               send.payload},
                       error))
            return false;
    }
    error.clear();
    return true;
}

bool LegacySteamRuntimeHost::queue_pre_presence(LegacySteamHostInbound inbound,
                                                std::string_view message_name)
{
    constexpr std::size_t max_packets_per_peer = 64U;
    constexpr std::size_t max_total_bytes = 256U * 1024U;
    const auto coalesced = [](std::string_view message) {
        return message == "PlayerUpdateMessage" || message == "ChangeCarriedItemMessage" ||
               message == "TimeConnectedMessage" || message == "CrateFocusMessage";
    };
    auto &pending = pre_presence_packets_[inbound.peer];
    if (coalesced(message_name))
    {
        const auto existing = std::ranges::find_if(
            pending, [&](const PendingPrePresenceInbound &packet) {
                return packet.message_name == message_name;
            });
        if (existing != pending.end())
        {
            const std::size_t replaced_size = existing->inbound.payload.size();
            const std::size_t next_bytes =
                pre_presence_bytes_ - std::min(pre_presence_bytes_, replaced_size) +
                inbound.payload.size();
            if (next_bytes > max_total_bytes)
                return false;
            existing->inbound = std::move(inbound);
            pre_presence_bytes_ = next_bytes;
            return true;
        }
    }
    if (pending.size() >= max_packets_per_peer ||
        inbound.payload.size() >
            max_total_bytes - std::min(max_total_bytes, pre_presence_bytes_))
        return false;
    pre_presence_bytes_ += inbound.payload.size();
    pending.push_back({std::move(inbound), std::string{message_name}});
    return true;
}

void LegacySteamRuntimeHost::replay_pre_presence(
    PeerId peer, std::vector<LegacySteamRuntimeEvent> &events)
{
    const auto found = pre_presence_packets_.find(peer);
    if (found == pre_presence_packets_.end())
        return;
    std::vector<PendingPrePresenceInbound> pending = std::move(found->second);
    pre_presence_packets_.erase(found);
    for (const PendingPrePresenceInbound &packet : pending)
        pre_presence_bytes_ -=
            std::min(pre_presence_bytes_, packet.inbound.payload.size());
    const auto priority = [](std::string_view message) {
        if (message == "PlayerUpdateMessage")
            return 0;
        if (message == "ChangeCarriedItemMessage")
            return 1;
        if (message == "TimeConnectedMessage")
            return 2;
        if (message == "CrateFocusMessage")
            return 3;
        return 4;
    };
    std::ranges::stable_sort(pending, {}, [&](const PendingPrePresenceInbound &packet) {
        return priority(packet.message_name);
    });
    for (PendingPrePresenceInbound &packet : pending)
    {
        if (trace_.has_value())
            static_cast<void>(trace_->record_packet(
                ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                packet.inbound.payload, packet.inbound.sender_gid,
                std::optional<std::uint8_t>{packet.inbound.recipient_gid},
                packet.inbound.delivery, packet.inbound.channel));
        events.push_back({LegacySteamRuntimeEventKind::gameplay,
                          std::move(packet.inbound),
                          {},
                          peer,
                          {}});
    }
}

void LegacySteamRuntimeHost::forget_pre_presence(PeerId peer) noexcept
{
    const auto found = pre_presence_packets_.find(peer);
    if (found == pre_presence_packets_.end())
        return;
    for (const PendingPrePresenceInbound &packet : found->second)
        pre_presence_bytes_ -=
            std::min(pre_presence_bytes_, packet.inbound.payload.size());
    pre_presence_packets_.erase(found);
}

} // namespace openminerz::net
