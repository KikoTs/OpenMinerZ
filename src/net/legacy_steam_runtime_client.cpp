#include "openminerz/net/legacy_steam_runtime_client.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

namespace openminerz::net
{
namespace
{

LegacyGamerIdentity local_identity(PeerId peer, std::uint64_t lobby, std::string persona)
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

LegacySteamRuntimeClientEventKind runtime_kind(LegacySteamClientInboundKind kind) noexcept
{
    switch (kind)
    {
    case LegacySteamClientInboundKind::connected:
        return LegacySteamRuntimeClientEventKind::connected;
    case LegacySteamClientInboundKind::peer_joined:
        return LegacySteamRuntimeClientEventKind::peer_joined;
    case LegacySteamClientInboundKind::peer_left:
        return LegacySteamRuntimeClientEventKind::peer_left;
    case LegacySteamClientInboundKind::session_properties_changed:
        return LegacySteamRuntimeClientEventKind::session_properties_changed;
    case LegacySteamClientInboundKind::gameplay:
        return LegacySteamRuntimeClientEventKind::gameplay;
    }
    return LegacySteamRuntimeClientEventKind::rejected;
}

} // namespace

std::vector<DiscoveredSession> make_discovered_sessions(const SteamBridgeDiscoveryResults &results)
{
    std::vector<DiscoveredSession> converted;
    converted.reserve(results.lobbies.size());
    for (const SteamBridgeDiscoveredLobby &lobby : results.lobbies)
    {
        DiscoveredSession session;
        session.session_id = lobby.lobby_id;
        session.server_name = lobby.name;
        session.host_name = lobby.host_name;
        session.server_message = lobby.created_at;
        session.source = lobby.friend_count > 0U ? DiscoverySource::friend_session
                                                 : DiscoverySource::public_lobby;
        session.properties = lobby.session_properties;
        session.current_players = lobby.current_players;
        session.maximum_players = lobby.maximum_players;
        session.password_required = lobby.password_protected;
        if (!lobby.extension_metadata.empty())
        {
            session.advertises_extension = true;
            ExtensionMessage advertisement;
            ExtensionDecodeError extension_error{};
            if (decode_extension_message(lobby.extension_metadata, advertisement,
                                         extension_error) &&
                advertisement.kind == ExtensionMessageKind::server_accept &&
                advertisement.nonce == 0U && advertisement.detail.empty())
                session.required_manifest = std::move(advertisement.manifest);
            else
                session.extension_metadata_valid = false;
        }
        session.platform_authorized = true;
        converted.push_back(std::move(session));
    }
    return converted;
}

LegacySteamRuntimeClient::~LegacySteamRuntimeClient()
{
    std::string ignored;
    static_cast<void>(stop(ignored));
}

LegacySteamRuntimeClient::LegacySteamRuntimeClient(LegacySteamRuntimeClient &&) noexcept = default;
LegacySteamRuntimeClient &LegacySteamRuntimeClient::operator=(
    LegacySteamRuntimeClient &&) noexcept = default;

bool LegacySteamRuntimeClient::start_browser(const LegacySteamRuntimeClientOptions &options,
                                             SteamBridgeDiscoveryResults &results,
                                             std::string &error)
{
    if (options_.has_value() || bridge_.running())
    {
        error = "The legacy Steam client is already running";
        return false;
    }
    if (options.trace_path.has_value())
    {
        trace_.emplace();
        if (!trace_->open(*options.trace_path, ProtocolTraceRole::client, options.protocol, error))
        {
            trace_.reset();
            return false;
        }
    }
    SteamBridgeClientOptions bridge_options;
    bridge_options.helper_path = options.helper_path;
    bridge_options.steam_api_path = options.steam_api_path;
    const SteamBridgeDiscoveryRequest request{options.discovery_protocol, std::uint8_t{50}};
    if (!bridge_.start_discovery(bridge_options, request, results, error))
    {
        if (trace_.has_value())
            static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                             ProtocolTraceResult::failure,
                                             "DiscoveryStartFailed", 0U, std::nullopt,
                                             MessageDeliveryPolicy::reliable_ordered, 0U, 0U,
                                             error));
        trace_.reset();
        return false;
    }
    options_ = options;
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "DiscoveryStarted"));
    error.clear();
    return true;
}

bool LegacySteamRuntimeClient::refresh(SteamBridgeDiscoveryResults &results, std::string &error)
{
    if (!options_.has_value() || session_.has_value() || !bridge_.running())
    {
        error = "Steam discovery is not in browser state";
        return false;
    }
    return bridge_.refresh_discovery({options_->discovery_protocol, std::uint8_t{50}}, results,
                                     error);
}

bool LegacySteamRuntimeClient::join(std::uint64_t lobby_id, ProtocolVersion protocol,
                                    const ModManifest &client_manifest, std::string password,
                                    SteamBridgeJoinReady &ready, std::string &error)
{
    if (!options_.has_value() || session_.has_value() || !bridge_.running())
    {
        error = "Steam lobby join is not in browser state";
        return false;
    }
    if (trace_.has_value())
    {
        trace_->set_protocol(protocol);
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "LobbyJoinStarted"));
    }
    if (!bridge_.join({lobby_id}, ready, error))
        return false;
    if (!ready.lobby.session_properties[0].has_value() ||
        *ready.lobby.session_properties[0] != static_cast<std::int32_t>(protocol))
    {
        error = "The joined lobby changed protocol after discovery";
        std::string ignored;
        static_cast<void>(bridge_.stop(ignored));
        return false;
    }
    if (client_manifest.protocol != protocol || !validate_mod_manifest(client_manifest, error))
    {
        error = "The selected client mod profile is invalid: " + error;
        std::string ignored;
        static_cast<void>(bridge_.stop(ignored));
        return false;
    }
    if (!negotiate_extension(ready, client_manifest, error))
    {
        std::string ignored;
        static_cast<void>(bridge_.stop(ignored));
        return false;
    }
    try
    {
        LegacySteamClientSessionOptions session_options;
        session_options.protocol = protocol;
        session_options.host_peer = ready.lobby.host_peer;
        session_options.expected_host_name = ready.lobby.host_name;
        session_options.session_properties = ready.lobby.session_properties;
        session_options.password = std::move(password);
        session_options.local_gamer =
            local_identity(ready.local_peer, ready.lobby.lobby_id, ready.persona_name);
        session_.emplace(std::move(session_options));
        joined_host_peer_ = ready.lobby.host_peer;
        options_->protocol = protocol;
    }
    catch (const std::exception &exception)
    {
        error = exception.what();
        std::string ignored;
        static_cast<void>(bridge_.stop(ignored));
        return false;
    }
    const auto approval = session_->begin(error);
    if (!approval.has_value() || !send_native(*approval, error))
    {
        session_.reset();
        std::string ignored;
        static_cast<void>(bridge_.stop(ignored));
        return false;
    }
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::outbound,
                                         ProtocolTraceResult::accepted,
                                         "AdmissionRequestSent", 0U, std::uint8_t{0},
                                         MessageDeliveryPolicy::reliable_ordered,
                                         approval->p2p_channel, approval->payload.size()));
    error.clear();
    return true;
}

bool LegacySteamRuntimeClient::pump(std::vector<LegacySteamRuntimeClientEvent> &events,
                                    std::string &error)
{
    events.clear();
    if (!session_.has_value() || !bridge_.running())
    {
        error = "The legacy Steam client has not joined a lobby";
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
            LegacySteamClientSessionResult result = session_->process_native_packet(
                bridge_event->inbound.peer, bridge_event->inbound.p2p_channel,
                bridge_event->inbound.wire);
            if (result.rejection_reason.has_value())
            {
                if (trace_.has_value())
                    static_cast<void>(trace_->record(
                        ProtocolTraceDirection::inbound, ProtocolTraceResult::rejected,
                        "AdmissionRejected", 0U, std::nullopt,
                        MessageDeliveryPolicy::reliable_ordered,
                        bridge_event->inbound.p2p_channel, bridge_event->inbound.wire.size(),
                        *result.rejection_reason));
                events.push_back({LegacySteamRuntimeClientEventKind::rejected,
                                  0U,
                                  {},
                                  {},
                                  MessageDeliveryPolicy::reliable_ordered,
                                  *result.rejection_reason});
                error = *result.rejection_reason;
                return false;
            }
            if (result.audit.has_value())
            {
                if (trace_.has_value())
                    static_cast<void>(trace_->record(
                        ProtocolTraceDirection::inbound, ProtocolTraceResult::rejected,
                        "PacketRejected", 0U, std::nullopt,
                        MessageDeliveryPolicy::reliable_ordered,
                        bridge_event->inbound.p2p_channel, bridge_event->inbound.wire.size(),
                        result.audit->reason));
                events.push_back({LegacySteamRuntimeClientEventKind::rejected,
                                  0U,
                                  {},
                                  {},
                                  MessageDeliveryPolicy::reliable_ordered,
                                  result.audit->reason});
            }
            for (LegacySteamClientInbound &inbound : result.inbound)
            {
                if (trace_.has_value())
                {
                    if (inbound.kind == LegacySteamClientInboundKind::gameplay)
                        static_cast<void>(trace_->record_packet(
                            ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                            inbound.payload, inbound.gid, session_->local_gid(), inbound.delivery,
                            bridge_event->inbound.p2p_channel));
                    else
                    {
                        const std::string_view event =
                            inbound.kind == LegacySteamClientInboundKind::connected
                                ? "Connected"
                            : inbound.kind == LegacySteamClientInboundKind::peer_joined
                                ? "PeerJoined"
                            : inbound.kind == LegacySteamClientInboundKind::peer_left
                                ? "PeerLeft"
                                : "SessionPropertiesChanged";
                        static_cast<void>(trace_->record(
                            ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted, event,
                            inbound.gid, session_->local_gid(), inbound.delivery,
                            bridge_event->inbound.p2p_channel, bridge_event->inbound.wire.size()));
                    }
                }
                events.push_back({runtime_kind(inbound.kind),
                                  inbound.gid,
                                  std::move(inbound.gamer),
                                  std::move(inbound.payload),
                                  inbound.delivery,
                                  {}});
            }
            break;
        }
        case SteamBridgeClientEventKind::peer_left:
            if (bridge_event->peer != joined_host_peer_)
                break;
            events.push_back({LegacySteamRuntimeClientEventKind::transport_failure,
                              0U,
                              {},
                              {},
                              MessageDeliveryPolicy::reliable_ordered,
                              "A Steam lobby peer disconnected"});
            error = "A Steam lobby peer disconnected";
            return false;
        case SteamBridgeClientEventKind::failure:
            if (trace_.has_value())
                static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                                 ProtocolTraceResult::failure,
                                                 "TransportFailure", 0U, std::nullopt,
                                                 MessageDeliveryPolicy::reliable_ordered, 0U, 0U,
                                                 bridge_event->failure.detail));
            events.push_back({LegacySteamRuntimeClientEventKind::transport_failure,
                              0U,
                              {},
                              {},
                              MessageDeliveryPolicy::reliable_ordered,
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

bool LegacySteamRuntimeClient::send(std::optional<std::uint8_t> recipient_gid,
                                    std::span<const std::uint8_t> application_packet,
                                    MessageDeliveryPolicy delivery, std::string &error)
{
    if (!session_.has_value() || !bridge_.running())
    {
        error = "The legacy Steam client has not joined a lobby";
        return false;
    }
    const auto outbound =
        session_->create_outbound(recipient_gid, application_packet, delivery, error);
    if (!outbound.has_value() || !send_native(*outbound, error))
        return false;
    if (trace_.has_value())
        static_cast<void>(trace_->record_packet(
            ProtocolTraceDirection::outbound, ProtocolTraceResult::accepted, application_packet,
            session_->local_gid().value_or(0U), recipient_gid, delivery, outbound->p2p_channel));
    return true;
}

bool LegacySteamRuntimeClient::stop(std::string &error) noexcept
{
    if (trace_.has_value())
        static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                         ProtocolTraceResult::accepted, "RuntimeStopped"));
    session_.reset();
    options_.reset();
    joined_host_peer_ = 0U;
    const bool stopped = bridge_.stop(error);
    trace_.reset();
    return stopped;
}

bool LegacySteamRuntimeClient::running() const noexcept
{
    return options_.has_value() && bridge_.running();
}

bool LegacySteamRuntimeClient::connected() const noexcept
{
    return session_.has_value() && session_->connected() && bridge_.running();
}

std::optional<std::uint8_t> LegacySteamRuntimeClient::local_gid() const noexcept
{
    return session_.has_value() ? session_->local_gid() : std::nullopt;
}

const LegacyGamerIdentity *LegacySteamRuntimeClient::local_gamer() const noexcept
{
    return session_.has_value() ? &session_->local_gamer() : nullptr;
}

bool LegacySteamRuntimeClient::negotiate_extension(const SteamBridgeJoinReady &ready,
                                                   const ModManifest &client_manifest,
                                                   std::string &error)
{
    if (ready.lobby.extension_metadata.empty())
    {
        if (requires_extension(client_manifest))
        {
            error = "The selected server does not advertise OpenMinerZ mod negotiation";
            if (trace_.has_value())
                static_cast<void>(trace_->record(
                    ProtocolTraceDirection::internal, ProtocolTraceResult::rejected,
                    "ModHandshakeUnavailable", 0U, std::uint8_t{0},
                    MessageDeliveryPolicy::reliable_ordered, 0U, 0U, error));
            return false;
        }
        if (trace_.has_value())
            static_cast<void>(trace_->record(ProtocolTraceDirection::internal,
                                             ProtocolTraceResult::accepted,
                                             "StockCompatibilitySelected"));
        error.clear();
        return true;
    }

    ExtensionMessage advertisement;
    ExtensionDecodeError decode_error{};
    if (!decode_extension_message(ready.lobby.extension_metadata, advertisement, decode_error) ||
        advertisement.kind != ExtensionMessageKind::server_accept || advertisement.nonce != 0U ||
        !advertisement.detail.empty())
    {
        error = "The lobby's OpenMinerZ mod advertisement is malformed";
        return false;
    }
    const ModCompatibilityResult advertised =
        compare_mod_manifests(advertisement.manifest, &client_manifest, true);
    if (!advertised.compatible())
    {
        error = advertised.detail;
        return false;
    }

    std::random_device random;
    std::uint64_t nonce = (static_cast<std::uint64_t>(random()) << 32U) | random();
    if (nonce == 0U)
        nonce = 1U;
    ExtensionMessage hello;
    hello.kind = ExtensionMessageKind::client_hello;
    hello.nonce = nonce;
    hello.manifest = client_manifest;
    const std::vector<std::uint8_t> hello_packet = encode_extension_message(hello);
    if (!bridge_.send(
            {ready.lobby.host_peer, SteamDeliveryMethod::reliable_ordered, 0U, hello_packet},
            error))
        return false;
    if (trace_.has_value())
        static_cast<void>(trace_->record(
            ProtocolTraceDirection::outbound, ProtocolTraceResult::accepted,
            "ModHandshakeHello", 0U, std::uint8_t{0},
            MessageDeliveryPolicy::reliable_ordered, 0U, hello_packet.size()));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::optional<SteamBridgeClientEvent> event;
        if (!bridge_.poll(event, error))
            return false;
        if (!event.has_value())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        if (event->kind == SteamBridgeClientEventKind::heartbeat)
            continue;
        if (event->kind == SteamBridgeClientEventKind::inbound_wire)
        {
            ExtensionMessage response;
            if (event->inbound.peer != ready.lobby.host_peer || event->inbound.p2p_channel != 0U ||
                !decode_extension_message(event->inbound.wire, response, decode_error) ||
                response.nonce != nonce ||
                (response.kind != ExtensionMessageKind::server_accept &&
                 response.kind != ExtensionMessageKind::server_reject))
            {
                error = "The server returned an invalid mod-negotiation response";
                return false;
            }
            if (response.kind == ExtensionMessageKind::server_reject)
            {
                error = response.detail.empty() ? "The server rejected the client mod profile"
                                                : response.detail;
                if (trace_.has_value())
                    static_cast<void>(trace_->record(
                        ProtocolTraceDirection::inbound, ProtocolTraceResult::rejected,
                        "ModHandshakeRejected", 0U, std::uint8_t{0},
                        MessageDeliveryPolicy::reliable_ordered, event->inbound.p2p_channel,
                        event->inbound.wire.size(), error));
                return false;
            }
            const ModCompatibilityResult accepted =
                compare_mod_manifests(advertisement.manifest, &response.manifest, true);
            if (!accepted.compatible())
            {
                error = "The server changed its mod profile during negotiation";
                return false;
            }
            if (trace_.has_value())
                static_cast<void>(trace_->record(
                    ProtocolTraceDirection::inbound, ProtocolTraceResult::accepted,
                    "ModHandshakeAccepted", 0U, std::uint8_t{0},
                    MessageDeliveryPolicy::reliable_ordered, event->inbound.p2p_channel,
                    event->inbound.wire.size()));
            error.clear();
            return true;
        }
        if (event->kind == SteamBridgeClientEventKind::failure)
            error = event->failure.detail;
        else if (event->kind == SteamBridgeClientEventKind::peer_left)
            error = "The server left during mod negotiation";
        else
            error = "The Steam helper stopped during mod negotiation";
        return false;
    }
    error = "OpenMinerZ mod negotiation timed out";
    return false;
}

bool LegacySteamRuntimeClient::send_native(const LegacySteamNativeSend &send, std::string &error)
{
    return bridge_.send(
        SteamBridgeOutboundWire{send.remote_peer, send.delivery, send.p2p_channel, send.payload},
        error);
}

} // namespace openminerz::net
