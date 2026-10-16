#include "openminerz/net/legacy_steam_client_session.hpp"

#include "openminerz/net/payload_codec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::uint8_t connected_status = 5U;

bool printable(std::string_view value, bool allow_empty = false) noexcept
{
    return (allow_empty || !value.empty()) &&
           std::none_of(value.begin(), value.end(), [](unsigned char character) {
               return character < 0x20U || character == 0x7FU;
           });
}

bool valid_gamer(const LegacyGamerIdentity &gamer, bool require_complete) noexcept
{
    return (!require_complete || !gamer.player_id.empty()) &&
           gamer.player_id.size() <= 1024U && gamer.gamer_tag.size() <= 128U &&
           printable(gamer.gamer_tag, !require_complete);
}

bool valid_properties(const SessionProperties &properties, ProtocolVersion protocol) noexcept
{
    if (!properties[0].has_value() || *properties[0] != static_cast<std::int32_t>(protocol) ||
        !properties[1].has_value() || (*properties[1] != 0 && *properties[1] != 1) ||
        !properties[2].has_value() || !properties[3].has_value() || !properties[4].has_value() ||
        (*properties[4] != 0 && *properties[4] != 1) || !properties[5].has_value() ||
        *properties[5] < 0 || *properties[5] > 2 || properties[6].has_value() ||
        properties[7].has_value())
        return false;
    const std::int32_t maximum_mode = protocol == ProtocolVersion::modern_v4 ? 5 : 3;
    return *properties[2] >= 0 && *properties[2] <= maximum_mode && *properties[3] >= 0 &&
           *properties[3] <= 3;
}

bool delivery_matches(const MessageDescriptor &message, std::span<const std::uint8_t> payload,
                      MessageDeliveryPolicy actual, ProtocolVersion protocol) noexcept
{
    if (message.transport.delivery == MessageDeliveryPolicy::player_update_state)
    {
        PlayerUpdatePayload update;
        PayloadError error{};
        if (!decode_player_update_payload(payload, protocol, update, error))
            return false;
        const bool ordered = (update.state_flags & (PlayerUpdatePayload::using_flag |
                                                    PlayerUpdatePayload::reloading_flag)) != 0U;
        return actual == (ordered ? MessageDeliveryPolicy::reliable_ordered
                                  : MessageDeliveryPolicy::unreliable_sequenced);
    }
    if (message.transport.delivery == MessageDeliveryPolicy::dragon_update_warmup)
        return actual == MessageDeliveryPolicy::reliable_ordered ||
               actual == MessageDeliveryPolicy::unreliable;
    return actual == message.transport.delivery;
}

bool validate_application(ProtocolVersion protocol, std::span<const std::uint8_t> packet,
                          MessageDeliveryPolicy delivery, bool validate_delivery,
                          DecodedPacket &decoded) noexcept
{
    PacketError packet_error{};
    if (!decode_packet(protocol, packet, decoded, packet_error))
        return false;
    PayloadError payload_error{};
    return validate_known_payload(protocol, *decoded.message, decoded.payload, payload_error) &&
           (!validate_delivery ||
            delivery_matches(*decoded.message, decoded.payload, delivery, protocol));
}

bool transient_before_admission(ProtocolVersion protocol,
                                const LegacySteamWirePacket &packet) noexcept
{
    if (packet.message_type != SteamIncomingMessageType::data || packet.channel != 0U)
        return false;
    LegacyDirectDataFrame frame;
    LegacySteamCodecError codec_error{};
    if (!decode_legacy_direct_frame(packet.payload, frame, codec_error))
        return false;
    DecodedPacket decoded;
    PacketError packet_error{};
    if (!decode_packet(protocol, frame.application_packet, decoded, packet_error))
        return false;
    switch (decoded.message->transport.delivery)
    {
    case MessageDeliveryPolicy::unreliable:
    case MessageDeliveryPolicy::unreliable_sequenced:
    case MessageDeliveryPolicy::reliable_sequenced:
    case MessageDeliveryPolicy::player_update_state:
    case MessageDeliveryPolicy::dragon_update_warmup:
        return true;
    case MessageDeliveryPolicy::reliable_unordered:
    case MessageDeliveryPolicy::reliable_ordered:
    case MessageDeliveryPolicy::chat:
        return false;
    }
    return false;
}

SteamDeliveryMethod routed_native_delivery(SteamDeliveryMethod delivery) noexcept
{
    return delivery == SteamDeliveryMethod::reliable_unordered
               ? SteamDeliveryMethod::reliable_ordered
               : delivery;
}

} // namespace

std::string distinct_gamer_tag(std::string_view base_tag, std::uint8_t attempt)
{
    constexpr std::size_t maximum_tag_bytes = 128U;
    if (attempt <= 1U)
        return std::string{base_tag.substr(0U, maximum_tag_bytes)};
    const std::string suffix = std::format(" ({})", static_cast<unsigned>(attempt));
    std::size_t keep = std::min(base_tag.size(), maximum_tag_bytes - suffix.size());
    // Never cut a UTF-8 sequence: back up over continuation bytes.
    while (keep > 0U && keep < base_tag.size() &&
           (static_cast<unsigned char>(base_tag[keep]) & 0xC0U) == 0x80U)
        --keep;
    return std::string{base_tag.substr(0U, keep)} + suffix;
}

LegacySteamClientSession::LegacySteamClientSession(LegacySteamClientSessionOptions options)
    : options_{std::move(options)}, requested_gamer_tag_{options_.local_gamer.gamer_tag},
      properties_{options_.session_properties}
{
    if (options_.host_peer == 0U || !printable(options_.expected_host_name) ||
        !valid_properties(properties_, options_.protocol) || options_.password.size() > 256U ||
        !printable(options_.password, true) || !valid_gamer(options_.local_gamer, true))
        throw std::invalid_argument{"invalid legacy Steam client session options"};
}

std::optional<LegacySteamNativeSend> LegacySteamClientSession::retry_with_distinct_gamer_tag(
    std::string &error)
{
    if (state_ != State::rejected || last_denial_ != legacy_duplicate_gamer_denial)
    {
        error = "The last admission result does not allow a display-tag retry.";
        return std::nullopt;
    }
    if (admission_attempts_ >= legacy_maximum_admission_attempts)
    {
        error = "Every distinct display tag was refused by the host.";
        return std::nullopt;
    }
    const std::string previous_tag = options_.local_gamer.gamer_tag;
    options_.local_gamer.gamer_tag = distinct_gamer_tag(
        requested_gamer_tag_, static_cast<std::uint8_t>(admission_attempts_ + 1U));
    state_ = State::idle;
    last_denial_.clear();
    auto request = begin(error);
    if (!request.has_value())
    {
        options_.local_gamer.gamer_tag = previous_tag;
        state_ = State::rejected;
        last_denial_ = legacy_duplicate_gamer_denial;
    }
    return request;
}

std::uint8_t LegacySteamClientSession::admission_attempts() const noexcept
{
    return admission_attempts_;
}

std::optional<LegacySteamNativeSend> LegacySteamClientSession::begin(std::string &error)
{
    if (state_ != State::idle)
    {
        error = "The legacy Steam client admission request was already started.";
        return std::nullopt;
    }
    LegacyConnectionRequest request;
    request.network_version = static_cast<std::int32_t>(options_.protocol);
    request.session_id = 0;
    if (!options_.password.empty())
        request.password = options_.password;
    request.gamer = options_.local_gamer;
    request.session_properties = properties_;
    LegacySteamCodecError codec_error{};
    const auto payload = encode_legacy_connection_request(request, codec_error);
    const auto wire = payload.has_value()
                          ? encode_legacy_steam_wire(SteamIncomingMessageType::connection_approval,
                                                     0U, *payload, codec_error)
                          : std::nullopt;
    if (!wire.has_value())
    {
        error = "The stock Steam admission request could not be encoded.";
        return std::nullopt;
    }
    state_ = State::awaiting_status;
    ++admission_attempts_;
    error.clear();
    return LegacySteamNativeSend{options_.host_peer, *wire, SteamDeliveryMethod::reliable_ordered,
                                 0U};
}

LegacySteamClientSessionResult LegacySteamClientSession::process_native_packet(
    PeerId sender, std::uint8_t p2p_channel, std::span<const std::uint8_t> wire)
{
    if (sender != options_.host_peer)
        return reject(LegacySteamRejectionStage::session, "UnexpectedNativeSender", wire.size());
    if (p2p_channel == 1U)
        return process_denial(wire);
    if (p2p_channel != 0U)
        return reject(LegacySteamRejectionStage::routing, "UnexpectedNativeChannel", wire.size());
    LegacySteamWirePacket packet;
    LegacySteamCodecError codec_error{};
    if (!decode_legacy_steam_wire(wire, packet, codec_error))
        return reject(LegacySteamRejectionStage::wire_envelope, "MalformedWireEnvelope",
                      wire.size());
    return process_wire(packet, wire.size());
}

LegacySteamClientSessionResult LegacySteamClientSession::process_denial(
    std::span<const std::uint8_t> wire)
{
    if ((state_ != State::awaiting_status && state_ != State::awaiting_response) ||
        wire.size() < 3U || wire.size() > 514U || wire.front() != 0U || wire.back() != 0U ||
        std::any_of(wire.begin() + 1, wire.end() - 1,
                    [](std::uint8_t character) { return character < 0x20U || character == 0x7FU; }))
        return reject(LegacySteamRejectionStage::approval, "MalformedConnectionDenial",
                      wire.size());
    std::string reason{wire.begin() + 1, wire.end() - 1};
    state_ = State::rejected;
    last_denial_ = reason;
    // Anything queued for the refused request belongs to no admitted identity.
    pending_connected_response_.reset();
    pending_application_.clear();
    pending_application_bytes_ = 0U;
    LegacySteamClientSessionResult result;
    result.rejection_reason = reason;
    result.audit = LegacySteamAuditEvent{++next_audit_sequence_, options_.host_peer,
                                         LegacySteamRejectionStage::approval, reason, wire.size()};
    return result;
}

LegacySteamClientSessionResult LegacySteamClientSession::process_wire(
    const LegacySteamWirePacket &packet, std::size_t input_length)
{
    if (packet.message_type == SteamIncomingMessageType::status_changed && packet.channel == 0U)
    {
        // SteamNetworkSessionProvider calls SteamNetBuffer.Write(5) with an int
        // literal. NetBuffer therefore places the connected status on the wire as
        // one little-endian Int32, not one byte.
        if (state_ != State::awaiting_status || packet.payload.size() != 4U ||
            packet.payload[0] != connected_status || packet.payload[1] != 0U ||
            packet.payload[2] != 0U || packet.payload[3] != 0U)
        {
            const std::string reason =
                std::format("InvalidConnectedStatus state={} size={} code={}",
                            static_cast<int>(state_), packet.payload.size(),
                            packet.payload.empty() ? -1 : static_cast<int>(packet.payload.front()));
            return state_ == State::awaiting_status || state_ == State::awaiting_response
                       ? reject_admission(LegacySteamRejectionStage::session, reason, input_length)
                       : reject(LegacySteamRejectionStage::session, reason, input_length);
        }
        state_ = State::awaiting_response;
        if (pending_connected_response_.has_value())
        {
            PendingWire response = std::move(*pending_connected_response_);
            pending_connected_response_.reset();
            return process_connected_response(response.packet.payload, response.input_length);
        }
        return {};
    }
    if (packet.message_type != SteamIncomingMessageType::data)
        return reject(LegacySteamRejectionStage::routing, "UnexpectedWireMessage", input_length);
    const bool connected_response =
        packet.channel == 1U && !packet.payload.empty() &&
        packet.payload.front() ==
            static_cast<std::uint8_t>(InternalSessionMessage::response_to_connection);
    if (state_ == State::awaiting_status)
    {
        if (connected_response)
        {
            if (pending_connected_response_.has_value())
                return reject_admission(LegacySteamRejectionStage::session,
                                        "DuplicateConnectedResponseBeforeStatus", input_length);
            pending_connected_response_ = PendingWire{packet, input_length};
            return {};
        }
        return queue_before_admission(packet, input_length);
    }
    if (state_ == State::awaiting_response)
    {
        if (connected_response)
            return process_connected_response(packet.payload, input_length);
        return queue_before_admission(packet, input_length);
    }
    if (state_ != State::connected)
        return reject(
            LegacySteamRejectionStage::session,
            std::format("ApplicationBeforeAdmission state={} channel={} size={} marker={}",
                        static_cast<int>(state_), packet.channel, packet.payload.size(),
                        packet.payload.empty() ? -1 : static_cast<int>(packet.payload.front())),
            input_length);
    if (packet.channel == 1U)
        return process_internal(packet.payload, input_length);
    if (packet.channel == 0U)
        return process_direct(packet.payload, input_length);
    return reject(LegacySteamRejectionStage::routing, "InvalidEnvelopeChannel", input_length);
}

LegacySteamClientSessionResult LegacySteamClientSession::process_connected_response(
    std::span<const std::uint8_t> payload, std::size_t input_length)
{
    LegacyConnectedResponse response;
    LegacySteamCodecError codec_error{};
    if (!decode_legacy_connected_response(payload, response, codec_error) ||
        response.player_gid == 0U || response.peers.empty() || response.peer_gids.empty())
        return reject_admission(LegacySteamRejectionStage::session,
                                "InvalidConnectedResponse", input_length);
    std::vector<RemoteGamer> peers;
    peers.reserve(response.peers.size());
    bool has_host{};
    for (std::size_t index{}; index < response.peers.size(); ++index)
    {
        const std::uint8_t gid = response.peer_gids[index];
        const LegacyGamerIdentity &gamer = response.peers[index];
        if (!valid_gamer(gamer, false))
            return reject_admission(
                LegacySteamRejectionStage::session,
                std::format("InvalidConnectedPeerIdentity index={} gid={} player_id_size={} "
                            "gamer_tag_size={}",
                            index, gid, gamer.player_id.size(), gamer.gamer_tag.size()),
                input_length);
        if (gid == response.player_gid)
            return reject_admission(
                LegacySteamRejectionStage::session,
                std::format("ConnectedPeerAliasesLocalGid index={} gid={}", index, gid),
                input_length);
        const auto duplicate =
            std::find_if(peers.begin(), peers.end(), [&](const RemoteGamer &existing) {
                return existing.gid == gid;
            });
        if (duplicate != peers.end())
            return reject_admission(
                LegacySteamRejectionStage::session,
                std::format("DuplicateConnectedPeerGid index={} gid={}", index, gid),
                input_length);
        has_host = has_host || gid == 0U;
        peers.push_back({gid, gamer});
    }
    if (!has_host)
        return reject_admission(LegacySteamRejectionStage::session,
                                "ConnectedPeerTableMissingHost", input_length);
    local_gid_ = response.player_gid;
    remote_gamers_ = std::move(peers);
    // A new session: nobody in it has announced the avatar add-on yet.
    avatar_capabilities_.clear();
    state_ = State::connected;
    LegacySteamClientSessionResult result;
    result.inbound.push_back({LegacySteamClientInboundKind::connected,
                              *local_gid_,
                              {},
                              {},
                              MessageDeliveryPolicy::reliable_ordered});
    for (const RemoteGamer &peer : remote_gamers_)
        result.inbound.push_back({LegacySteamClientInboundKind::peer_joined,
                                  peer.gid,
                                  peer.gamer,
                                  {},
                                  MessageDeliveryPolicy::reliable_ordered});
    drain_pending(result);
    return result;
}

LegacySteamClientSessionResult LegacySteamClientSession::queue_before_admission(
    const LegacySteamWirePacket &packet, std::size_t input_length)
{
    // The stock provider can receive cross-delivery traffic before its reliable
    // roster. Lossy state is replaceable by the next post-admission update and
    // must not consume the bounded reliable quarantine.
    if (transient_before_admission(options_.protocol, packet))
        return {};
    constexpr std::size_t maximum_pending_packets = 64U;
    constexpr std::size_t maximum_pending_bytes = 256U * 1024U;
    if (pending_application_.size() >= maximum_pending_packets ||
        packet.payload.size() > maximum_pending_bytes - pending_application_bytes_)
        // A busy stock lobby can legitimately relay reliable gameplay before its
        // status/roster pair reaches a joining peer. Keep the quarantine bounded,
        // but do not let unrelated pre-admission traffic deny the admission itself.
        // The authoritative bootstrap reconciles any dropped world state afterward.
        return reject(LegacySteamRejectionStage::session,
                      "PreAdmissionQuarantineOverflowDropped", input_length);
    pending_application_bytes_ += packet.payload.size();
    pending_application_.push_back({packet, input_length});
    return {};
}

void LegacySteamClientSession::drain_pending(LegacySteamClientSessionResult &result)
{
    std::vector<PendingWire> pending = std::move(pending_application_);
    pending_application_.clear();
    pending_application_bytes_ = 0U;
    for (const PendingWire &wire : pending)
    {
        LegacySteamClientSessionResult replay = process_wire(wire.packet, wire.input_length);
        result.inbound.insert(result.inbound.end(),
                              std::make_move_iterator(replay.inbound.begin()),
                              std::make_move_iterator(replay.inbound.end()));
        if (!result.audit.has_value() && replay.audit.has_value())
            result.audit = std::move(replay.audit);
    }
}

LegacySteamClientSessionResult LegacySteamClientSession::process_internal(
    std::span<const std::uint8_t> payload, std::size_t input_length)
{
    if (payload.empty())
        return reject(LegacySteamRejectionStage::routing, "EmptyInternalMessage", input_length);
    LegacySteamCodecError codec_error{};
    const auto message = static_cast<InternalSessionMessage>(payload.front());
    if (message == InternalSessionMessage::new_peer)
    {
        std::uint8_t gid{};
        LegacyGamerIdentity gamer;
        if (!decode_legacy_new_peer(payload, gid, gamer, codec_error) || gid == 0U ||
            gid == local_gid_ || !valid_gamer(gamer, false) || find_gid(gid) != nullptr)
            return reject(LegacySteamRejectionStage::session, "InvalidNewPeer", input_length);
        remote_gamers_.push_back({gid, gamer});
        // Gamer ids are reused: a newcomer must not inherit the previous
        // occupant's add-on capability.
        avatar_capabilities_.forget(gid);
        LegacySteamClientSessionResult result;
        result.inbound.push_back({LegacySteamClientInboundKind::peer_joined,
                                  gid,
                                  std::move(gamer),
                                  {},
                                  MessageDeliveryPolicy::reliable_ordered});
        return result;
    }
    if (message == InternalSessionMessage::drop_peer)
    {
        std::uint8_t gid{};
        if (!decode_legacy_drop_peer(payload, gid, codec_error))
            return reject(LegacySteamRejectionStage::session, "InvalidDropPeer", input_length);
        const auto found = std::find_if(remote_gamers_.begin(), remote_gamers_.end(),
                                        [gid](const RemoteGamer &peer) { return peer.gid == gid; });
        // SteamNetworkSessionProvider.TryGetValue ignores stale or duplicate
        // departures. They are common around reconnects and are not a session fault.
        avatar_capabilities_.forget(gid);
        if (found == remote_gamers_.end())
            return {};
        remote_gamers_.erase(found);
        LegacySteamClientSessionResult result;
        result.inbound.push_back({LegacySteamClientInboundKind::peer_left,
                                  gid,
                                  {},
                                  {},
                                  MessageDeliveryPolicy::reliable_ordered});
        return result;
    }
    if (message == InternalSessionMessage::session_properties_changed)
    {
        SessionProperties properties;
        if (!decode_legacy_session_properties_changed(payload, properties, codec_error))
            return reject(LegacySteamRejectionStage::session, "InvalidSessionProperties",
                          input_length);
        // The stock Steam client updates only slots that are present in both
        // the received table and its current table. Nulls are wildcards, not a
        // request to erase or replace the complete eight-slot contract.
        for (std::size_t index{}; index < properties_.size(); ++index)
            if (properties[index].has_value() && properties_[index].has_value())
                properties_[index] = properties[index];
        LegacySteamClientSessionResult result;
        result.inbound.push_back({LegacySteamClientInboundKind::session_properties_changed,
                                  0U,
                                  {},
                                  {},
                                  MessageDeliveryPolicy::reliable_ordered});
        return result;
    }
    return reject(LegacySteamRejectionStage::routing, "HostSentClientRouteCommand", input_length);
}

LegacySteamClientSessionResult LegacySteamClientSession::process_direct(
    std::span<const std::uint8_t> payload, std::size_t input_length)
{
    LegacyDirectDataFrame frame;
    LegacySteamCodecError codec_error{};
    DecodedPacket decoded;
    if (!local_gid_.has_value() || !decode_legacy_direct_frame(payload, frame, codec_error) ||
        frame.recipient_gid != *local_gid_ ||
        (frame.sender_gid != *local_gid_ && find_gid(frame.sender_gid) == nullptr))
        return reject(LegacySteamRejectionStage::application, "InvalidDirectApplicationPacket",
                      input_length);
    if (!frame.application_packet.empty() &&
        message_by_id(options_.protocol, frame.application_packet.front()) == nullptr)
    {
        // An id outside the stock table is the avatar add-on's message, and
        // only from a gamer who announced the add-on under that very id.
        const std::uint8_t id = frame.application_packet.front();
        const std::optional<std::uint8_t> sender_id =
            frame.sender_gid == *local_gid_ ? std::nullopt
                                            : avatar_capabilities_.message_id(frame.sender_gid);
        if (!sender_id.has_value() || id != *sender_id ||
            !is_avatar_sync_packet(frame.application_packet, id))
            return reject(LegacySteamRejectionStage::application, "UnannouncedAvatarPacket",
                          input_length);
        LegacySteamClientSessionResult avatar;
        avatar.inbound.push_back({LegacySteamClientInboundKind::avatar_sync,
                                  frame.sender_gid,
                                  {},
                                  std::move(frame.application_packet),
                                  MessageDeliveryPolicy::reliable_ordered});
        return avatar;
    }
    if (!validate_application(options_.protocol, frame.application_packet,
                              MessageDeliveryPolicy::reliable_ordered, false, decoded) ||
        (frame.sender_gid == *local_gid_ && !decoded.message->transport.echo))
        return reject(LegacySteamRejectionStage::application, "InvalidDirectApplicationPacket",
                      input_length);
    LegacySteamClientSessionResult result;
    if (frame.sender_gid != *local_gid_ && decoded.message->name == "PlayerExistsMessage")
    {
        const AvatarCapabilityAnnouncement announcement =
            read_avatar_capability_announcement(options_.protocol, frame.application_packet);
        if (announcement.present)
        {
            avatar_capabilities_.announce(frame.sender_gid, announcement.message_id);
            result.inbound.push_back({LegacySteamClientInboundKind::avatar_capability,
                                      frame.sender_gid,
                                      {},
                                      {announcement.message_id},
                                      MessageDeliveryPolicy::reliable_ordered});
        }
    }
    result.inbound.push_back({LegacySteamClientInboundKind::gameplay,
                              frame.sender_gid,
                              {},
                              std::move(frame.application_packet),
                              decoded.message->transport.delivery});
    return result;
}

std::optional<LegacySteamNativeSend> LegacySteamClientSession::create_outbound(
    std::optional<std::uint8_t> recipient_gid, std::span<const std::uint8_t> application_packet,
    MessageDeliveryPolicy delivery, std::string &error) const
{
    if (state_ != State::connected || !local_gid_.has_value())
    {
        error = "The legacy Steam client is not admitted.";
        return std::nullopt;
    }
    DecodedPacket decoded;
    if (!validate_application(options_.protocol, application_packet, delivery, true, decoded))
    {
        error = "Outbound application packet or delivery violates the stock contract.";
        return std::nullopt;
    }
    const auto steam_delivery = to_steam_delivery(delivery);
    if (!steam_delivery.has_value())
    {
        error = "Outbound delivery is semantic rather than a concrete Steam delivery.";
        return std::nullopt;
    }
    LegacySteamCodecError codec_error{};
    std::optional<std::vector<std::uint8_t>> route;
    std::uint8_t envelope_channel{};
    SteamDeliveryMethod native_delivery = *steam_delivery;
    if (recipient_gid.has_value() && *recipient_gid == 0U)
    {
        route = encode_legacy_direct_frame(
            LegacyDirectDataFrame{
                0U, *local_gid_, {application_packet.begin(), application_packet.end()}},
            codec_error);
    }
    else
    {
        if (recipient_gid.has_value() &&
            (*recipient_gid == *local_gid_ || find_gid(*recipient_gid) == nullptr))
        {
            error = "The outbound proxy target is not a remote session gamer.";
            return std::nullopt;
        }
        envelope_channel = 1U;
        route =
            recipient_gid.has_value()
                ? encode_legacy_client_proxy_direct(*recipient_gid, *local_gid_, *steam_delivery,
                                                    application_packet, codec_error)
                : encode_legacy_client_broadcast(*local_gid_, *steam_delivery, application_packet,
                                                 codec_error);
        native_delivery = routed_native_delivery(native_delivery);
    }
    const auto wire = route.has_value()
                          ? encode_legacy_steam_wire(SteamIncomingMessageType::data,
                                                     envelope_channel, *route, codec_error)
                          : std::nullopt;
    if (!wire.has_value())
    {
        error = "The outbound stock client route could not be encoded.";
        return std::nullopt;
    }
    error.clear();
    return LegacySteamNativeSend{options_.host_peer, *wire, native_delivery, 0U};
}

std::optional<LegacySteamNativeSend> LegacySteamClientSession::create_avatar_outbound(
    std::uint8_t recipient_gid, std::span<const std::uint8_t> avatar_packet,
    std::string &error) const
{
    if (state_ != State::connected || !local_gid_.has_value())
    {
        error = "The legacy Steam client is not admitted.";
        return std::nullopt;
    }
    const std::optional<std::uint8_t> recipient_id =
        recipient_gid == *local_gid_ || find_gid(recipient_gid) == nullptr
            ? std::nullopt
            : avatar_capabilities_.message_id(recipient_gid);
    if (!recipient_id.has_value())
    {
        error = "The recipient has not announced the Xbox Avatar add-on.";
        return std::nullopt;
    }
    if (avatar_packet.empty() || avatar_packet.front() != *recipient_id ||
        !valid_avatar_message_id(options_.protocol, *recipient_id) ||
        !is_avatar_sync_packet(avatar_packet, *recipient_id))
    {
        error = "Not an avatar message under the id the recipient reported.";
        return std::nullopt;
    }
    LegacySteamCodecError codec_error{};
    std::optional<std::vector<std::uint8_t>> route;
    std::uint8_t envelope_channel{};
    if (recipient_gid == 0U)
        route = encode_legacy_direct_frame(
            LegacyDirectDataFrame{0U, *local_gid_, {avatar_packet.begin(), avatar_packet.end()}},
            codec_error);
    else
    {
        // ZZAvatarSyncMessage is ReliableInOrder and addressed to one gamer.
        envelope_channel = 1U;
        route = encode_legacy_client_proxy_direct(recipient_gid, *local_gid_,
                                                  SteamDeliveryMethod::reliable_ordered,
                                                  avatar_packet, codec_error);
    }
    const auto wire = route.has_value()
                          ? encode_legacy_steam_wire(SteamIncomingMessageType::data,
                                                     envelope_channel, *route, codec_error)
                          : std::nullopt;
    if (!wire.has_value())
    {
        error = "The avatar message could not be encoded.";
        return std::nullopt;
    }
    error.clear();
    return LegacySteamNativeSend{options_.host_peer, *wire, SteamDeliveryMethod::reliable_ordered,
                                 0U};
}

bool LegacySteamClientSession::avatar_capable(std::uint8_t gid) const noexcept
{
    return find_gid(gid) != nullptr && avatar_capabilities_.capable(gid);
}

std::optional<std::uint8_t> LegacySteamClientSession::avatar_message_id(
    std::uint8_t gid) const noexcept
{
    return find_gid(gid) == nullptr ? std::nullopt : avatar_capabilities_.message_id(gid);
}

bool LegacySteamClientSession::connected() const noexcept
{
    return state_ == State::connected;
}

std::optional<std::uint8_t> LegacySteamClientSession::local_gid() const noexcept
{
    return local_gid_;
}

const LegacyGamerIdentity &LegacySteamClientSession::local_gamer() const noexcept
{
    return options_.local_gamer;
}

const LegacyGamerIdentity *LegacySteamClientSession::gamer_for(std::uint8_t gid) const noexcept
{
    const RemoteGamer *found = find_gid(gid);
    return found == nullptr ? nullptr : &found->gamer;
}

SessionProperties LegacySteamClientSession::session_properties() const noexcept
{
    return properties_;
}

LegacySteamClientSessionResult LegacySteamClientSession::reject(LegacySteamRejectionStage stage,
                                                                std::string reason,
                                                                std::size_t input_length)
{
    LegacySteamClientSessionResult result;
    result.audit = LegacySteamAuditEvent{++next_audit_sequence_, options_.host_peer, stage,
                                         std::move(reason), input_length};
    return result;
}

LegacySteamClientSessionResult LegacySteamClientSession::reject_admission(
    LegacySteamRejectionStage stage, std::string reason, std::size_t input_length)
{
    state_ = State::rejected;
    pending_connected_response_.reset();
    pending_application_.clear();
    pending_application_bytes_ = 0U;
    LegacySteamClientSessionResult result = reject(stage, std::move(reason), input_length);
    result.rejection_reason = result.audit->reason;
    return result;
}

const LegacySteamClientSession::RemoteGamer *LegacySteamClientSession::find_gid(
    std::uint8_t gid) const noexcept
{
    const auto found = std::find_if(remote_gamers_.begin(), remote_gamers_.end(),
                                    [gid](const RemoteGamer &peer) { return peer.gid == gid; });
    return found == remote_gamers_.end() ? nullptr : &*found;
}

} // namespace openminerz::net
