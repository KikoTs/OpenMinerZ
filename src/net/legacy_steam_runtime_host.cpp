#include "openminerz/net/legacy_steam_runtime_host.hpp"

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
            make_session_properties(options.settings, true, options.game_mode, options.difficulty,
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
    if (session_.has_value() || bridge_.running())
    {
        error = "The legacy Steam host is already running";
        return false;
    }
    pre_presence_packets_.clear();
    pre_presence_bytes_ = 0U;
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

    LegacySteamHostSessionOptions session_options;
    session_options.settings = options.settings;
    session_options.game_begun = true;
    session_options.game_mode = options.game_mode;
    session_options.difficulty = options.difficulty;
    session_options.infinite_resources = options.infinite_resources;
    const LegacyGamerIdentity local_host =
        host_identity(ready.local_peer, ready.lobby_id, ready.persona_name);
    session_options.host_gamer = local_host;
    // SteamBridge32 accepts native sessions and forwards traffic only for IDs in
    // the current lobby. Membership in a friends/invite lobby is the platform
    // authorization consumed by the source-shaped session gate.
    session_options.platform_authorized = [](PeerId) { return true; };
    session_.emplace(std::move(session_options));
    mod_manifest_ = options.settings.mod_manifest;
    if (options.bootstrap_snapshot.has_value())
    {
        AuthoritativeBootstrapSnapshot snapshot = *options.bootstrap_snapshot;
        if (snapshot.world.owner_gamer_tag.empty())
            snapshot.world.owner_gamer_tag = ready.persona_name;
        if (snapshot.world.creator_gamer_tag.empty())
            snapshot.world.creator_gamer_tag = ready.persona_name;
        PlayerExistsPayload host_presence;
        host_presence.gamer_tag = local_host.gamer_tag;
        host_presence.player_id = local_host.player_id;
        snapshot.host_presence = std::move(host_presence);
        bootstrap_.emplace(options.settings.protocol, std::move(snapshot));
    }
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "HostStarted"));
    error.clear();
    return true;
}

bool LegacySteamRuntimeHost::pump(std::vector<LegacySteamRuntimeEvent> &events, std::string &error)
{
    events.clear();
    if (!session_.has_value() || !bridge_.running())
    {
        error = "The legacy Steam host is not running";
        return false;
    }
    for (std::size_t count{}; count < 256U; ++count)
    {
        std::optional<SteamBridgeClientEvent> bridge_event;
        if (!bridge_.poll(bridge_event, error))
            return false;
        if (!bridge_event.has_value())
            break;
        switch (bridge_event->kind)
        {
        case SteamBridgeClientEventKind::inbound_wire: {
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
                if (!bridge_.send({bridge_event->inbound.peer,
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
                    events.push_back({LegacySteamRuntimeEventKind::rejected,
                                      {},
                                      {},
                                      bridge_event->inbound.peer,
                                      std::move(rejection)});
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
            }
            for (LegacySteamHostInbound &inbound : result.inbound)
            {
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
                        error = "The bootstrap authority rejected a newly admitted peer";
                        return false;
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
                        events.push_back({LegacySteamRuntimeEventKind::rejected,
                                          {},
                                          {},
                                          inbound.peer,
                                          "InvalidOrOutOfOrderBootstrapMessage"});
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
            LegacySteamHostInbound departing;
            departing.peer = bridge_event->peer;
            departing.sender_gid = departing_gid.value_or(0U);
            events.push_back({LegacySteamRuntimeEventKind::peer_disconnected,
                              std::move(departing),
                              {},
                              bridge_event->peer,
                              {}});
            if (trace_.has_value())
                static_cast<void>(trace_->record(
                    ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                    "PeerDisconnected", departing_gid.value_or(0U), std::uint8_t{0}));
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
    }
    error.clear();
    return true;
}

bool LegacySteamRuntimeHost::send_authoritative(const LegacySteamOutbound &outbound,
                                                std::string &error)
{
    if (!session_.has_value() || !bridge_.running())
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
    mod_manifest_ = {};
    pre_presence_packets_.clear();
    pre_presence_bytes_ = 0U;
    const bool stopped = bridge_.stop(error);
    trace_.reset();
    return stopped;
}

bool LegacySteamRuntimeHost::running() const noexcept
{
    return session_.has_value() && bridge_.running();
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

bool LegacySteamRuntimeHost::gameplay_ready(PeerId peer, std::uint8_t player_gid) const noexcept
{
    return session_.has_value() && bootstrap_.has_value() &&
           session_->gid_for(peer) == std::optional<std::uint8_t>{player_gid} &&
           bootstrap_->gameplay_ready(peer, player_gid);
}

bool LegacySteamRuntimeHost::send_all(std::span<const LegacySteamNativeSend> sends,
                                      std::string &error)
{
    for (const LegacySteamNativeSend &send : sends)
    {
        if (!bridge_.send(SteamBridgeOutboundWire{send.remote_peer, send.delivery, send.p2p_channel,
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
