#include "openminerz/net/legacy_steam_host_session.hpp"

#include "openminerz/net/payload_codec.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::uint8_t connected_status = 5U;

bool constant_time_equal(std::string_view expected, std::string_view actual) noexcept
{
    const std::size_t count = std::max(expected.size(), actual.size());
    std::size_t difference = expected.size() ^ actual.size();
    for (std::size_t index{}; index < count; ++index)
    {
        const auto left = index < expected.size() ? static_cast<unsigned char>(expected[index])
                                                  : static_cast<unsigned char>(0U);
        const auto right = index < actual.size() ? static_cast<unsigned char>(actual[index])
                                                 : static_cast<unsigned char>(0U);
        difference |= static_cast<std::size_t>(left ^ right);
    }
    return difference == 0U;
}

bool has_non_whitespace(std::string_view value) noexcept
{
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char character) { return std::isspace(character) == 0; });
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

} // namespace

std::optional<SteamDeliveryMethod> to_steam_delivery(MessageDeliveryPolicy delivery) noexcept
{
    switch (delivery)
    {
    case MessageDeliveryPolicy::unreliable:
        return SteamDeliveryMethod::unreliable;
    case MessageDeliveryPolicy::unreliable_sequenced:
        return SteamDeliveryMethod::unreliable_sequenced;
    case MessageDeliveryPolicy::reliable_unordered:
        return SteamDeliveryMethod::reliable_unordered;
    case MessageDeliveryPolicy::reliable_sequenced:
        return SteamDeliveryMethod::reliable_sequenced;
    case MessageDeliveryPolicy::reliable_ordered:
        return SteamDeliveryMethod::reliable_ordered;
    case MessageDeliveryPolicy::chat:
        return SteamDeliveryMethod::unknown;
    case MessageDeliveryPolicy::player_update_state:
    case MessageDeliveryPolicy::dragon_update_warmup:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<MessageDeliveryPolicy> from_steam_delivery(SteamDeliveryMethod delivery) noexcept
{
    switch (delivery)
    {
    case SteamDeliveryMethod::unknown:
        return MessageDeliveryPolicy::chat;
    case SteamDeliveryMethod::unreliable:
        return MessageDeliveryPolicy::unreliable;
    case SteamDeliveryMethod::unreliable_sequenced:
        return MessageDeliveryPolicy::unreliable_sequenced;
    case SteamDeliveryMethod::reliable_unordered:
        return MessageDeliveryPolicy::reliable_unordered;
    case SteamDeliveryMethod::reliable_sequenced:
        return MessageDeliveryPolicy::reliable_sequenced;
    case SteamDeliveryMethod::reliable_ordered:
        return MessageDeliveryPolicy::reliable_ordered;
    }
    return std::nullopt;
}

LegacySteamHostSession::LegacySteamHostSession(LegacySteamHostSessionOptions options)
    : options_{std::move(options)}
{
    std::string settings_error;
    if (!options_.settings.validate(settings_error))
        throw std::invalid_argument("invalid legacy Steam host settings: " + settings_error);
    if (options_.host_gamer.player_id.size() > 1024U || options_.host_gamer.gamer_tag.empty() ||
        options_.host_gamer.gamer_tag.size() > 128U)
        throw std::invalid_argument("invalid legacy Steam host gamer identity");
    properties_ =
        make_session_properties(options_.settings, options_.join_closed, options_.game_mode,
                                options_.difficulty, options_.infinite_resources);
}

LegacySteamHostSessionResult LegacySteamHostSession::process_wire_packet(
    PeerId sender, std::span<const std::uint8_t> wire)
{
    if (sender == 0U)
        return audit_reject(sender, LegacySteamRejectionStage::session, "InvalidSender",
                            wire.size());
    LegacySteamWirePacket packet;
    LegacySteamCodecError codec_error{};
    if (!decode_legacy_steam_wire(wire, packet, codec_error))
        return audit_reject(sender, LegacySteamRejectionStage::wire_envelope,
                            "MalformedWireEnvelope", wire.size());
    if (packet.message_type == SteamIncomingMessageType::connection_approval &&
        packet.channel == 0U)
        return process_approval(sender, packet.payload, wire.size());
    if (packet.message_type == SteamIncomingMessageType::data)
        return process_data(sender, packet, wire.size());
    return audit_reject(sender, LegacySteamRejectionStage::routing, "UnexpectedWireMessage",
                        wire.size());
}

LegacySteamHostSessionResult LegacySteamHostSession::process_approval(
    PeerId sender, std::span<const std::uint8_t> payload, std::size_t input_length)
{
    LegacyConnectionRequest request;
    LegacySteamCodecError codec_error{};
    if (!decode_legacy_connection_request(payload, request, codec_error))
        return reject(sender, "VersionIsInvalid", input_length);
    if (const auto rejection = validate_connection(sender, request); rejection.has_value())
        return reject(sender, *rejection, input_length, request.gamer.gamer_tag);
    const auto gid = allocate_gid();
    if (!gid.has_value())
        return reject(sender, "GameIsFull", input_length, request.gamer.gamer_tag);

    // The roster entry carries a session-unique display tag. The tag the client
    // reported for itself is kept separately: its own name-addressed packets and
    // chat transcripts keep using it, because the stock response-to-connection
    // never tells a joiner how the host listed it.
    RemotePeer joining{sender, *gid, request.gamer, request.gamer.gamer_tag};
    joining.gamer.gamer_tag = unique_display_tag(request.gamer.gamer_tag);
    // Gamer ids are reused: a newcomer must not inherit the previous occupant's
    // add-on capability.
    avatar_capabilities_.forget(*gid);
    LegacyConnectedResponse response;
    response.player_gid = *gid;
    response.peers.reserve(peers_.size() + 1U);
    response.peer_gids.reserve(peers_.size() + 1U);
    response.peers.push_back(advertised_host());
    response.peer_gids.push_back(0U);
    for (const RemotePeer &peer : peers_)
    {
        response.peers.push_back(peer.gamer);
        response.peer_gids.push_back(peer.gid);
    }

    LegacySteamCodecError error{};
    const std::array<std::uint8_t, 4> status{connected_status, 0U, 0U, 0U};
    const auto status_wire =
        encode_legacy_steam_wire(SteamIncomingMessageType::status_changed, 0U, status, error);
    const auto response_payload = encode_legacy_connected_response(response, error);
    const auto response_wire =
        response_payload.has_value()
            ? encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, *response_payload, error)
            : std::nullopt;
    const auto new_peer_payload = encode_legacy_new_peer(*gid, joining.gamer, error);
    const auto new_peer_wire =
        new_peer_payload.has_value()
            ? encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, *new_peer_payload, error)
            : std::nullopt;
    if (!status_wire.has_value() || !response_wire.has_value() || !new_peer_wire.has_value())
        return audit_reject(sender, LegacySteamRejectionStage::session, "HostSessionEncodingFailed",
                            input_length);

    LegacySteamHostSessionResult result;
    result.sends.push_back({sender, *status_wire, SteamDeliveryMethod::reliable_ordered, 0U});
    result.sends.push_back({sender, *response_wire, SteamDeliveryMethod::reliable_ordered, 0U});
    for (const RemotePeer &peer : peers_)
        result.sends.push_back(
            {peer.peer, *new_peer_wire, SteamDeliveryMethod::reliable_ordered, 0U});
    peers_.push_back(std::move(joining));
    result.inbound.push_back({LegacySteamInboundKind::peer_connected,
                              sender,
                              {},
                              MessageDeliveryPolicy::reliable_ordered,
                              0U,
                              *gid,
                              0U});
    return result;
}

LegacySteamHostSessionResult LegacySteamHostSession::process_data(
    PeerId sender, const LegacySteamWirePacket &packet, std::size_t input_length)
{
    const RemotePeer *peer = find_peer(sender);
    if (peer == nullptr)
        return audit_reject(sender, LegacySteamRejectionStage::session, "UnknownPeer",
                            input_length);

    DecodedPacket decoded;
    if (packet.channel == 0U)
    {
        LegacyDirectDataFrame frame;
        LegacySteamCodecError error{};
        if (!decode_legacy_direct_frame(packet.payload, frame, error) ||
            frame.recipient_gid != 0U || frame.sender_gid != peer->gid)
            return audit_reject(sender, LegacySteamRejectionStage::routing, "InvalidDirectRoute",
                                input_length);
        if (!frame.application_packet.empty() &&
            message_by_id(options_.settings.protocol, frame.application_packet.front()) == nullptr)
            return process_avatar_packet(*peer, std::nullopt, std::move(frame.application_packet),
                                         true, input_length);
        if (!validate_application(frame.application_packet,
                                  MessageDeliveryPolicy::reliable_unordered, false, decoded))
            return audit_reject(sender, LegacySteamRejectionStage::application,
                                "InvalidApplicationPacket", input_length);
        LegacySteamHostSessionResult result;
        observe_avatar_capability(*peer, frame.application_packet, result);
        result.inbound.push_back({LegacySteamInboundKind::gameplay, sender,
                                  std::move(frame.application_packet),
                                  MessageDeliveryPolicy::reliable_unordered, 0U, peer->gid, 0U});
        return result;
    }

    LegacyClientRouteFrame route;
    LegacySteamCodecError error{};
    if (packet.channel != 1U || !decode_legacy_client_route(packet.payload, route, error) ||
        route.sender_gid != peer->gid)
        return audit_reject(sender, LegacySteamRejectionStage::routing, "InvalidClientRoute",
                            input_length);
    const auto delivery = from_steam_delivery(route.delivery);
    if (!delivery.has_value())
        return audit_reject(sender, LegacySteamRejectionStage::delivery, "InvalidDelivery",
                            input_length);
    if (!route.application_packet.empty() &&
        message_by_id(options_.settings.protocol, route.application_packet.front()) == nullptr)
    {
        // An id outside the stock table is never broadcast: the add-on sends
        // its message to one gamer at a time, and a broadcast would reach
        // clients that cannot read it.
        if (route.kind != LegacyClientRouteKind::proxy_direct || !route.recipient_gid.has_value())
            return audit_reject(sender, LegacySteamRejectionStage::routing,
                                "AvatarPacketNotDirect", input_length);
        return process_avatar_packet(*peer, route.recipient_gid,
                                     std::move(route.application_packet),
                                     *delivery == MessageDeliveryPolicy::reliable_ordered,
                                     input_length);
    }
    if (!validate_application(route.application_packet, *delivery, true, decoded))
        return audit_reject(sender, LegacySteamRejectionStage::application,
                            "InvalidApplicationPacketOrDelivery", input_length);
    std::uint8_t recipient_gid{};
    if (route.kind == LegacyClientRouteKind::proxy_direct)
    {
        if (!route.recipient_gid.has_value())
            return audit_reject(sender, LegacySteamRejectionStage::routing, "InvalidProxyTarget",
                                input_length);
        const RemotePeer *target = find_gid(*route.recipient_gid);
        if (target == nullptr || target->peer == sender)
            return audit_reject(sender, LegacySteamRejectionStage::routing, "InvalidProxyTarget",
                                input_length);
        recipient_gid = *route.recipient_gid;
    }
    LegacySteamHostSessionResult result;
    observe_avatar_capability(*peer, route.application_packet, result);
    result.inbound.push_back({LegacySteamInboundKind::gameplay, sender,
                              std::move(route.application_packet), *delivery, 1U, peer->gid,
                              recipient_gid});
    return result;
}

void LegacySteamHostSession::observe_avatar_capability(
    const RemotePeer &sender, std::span<const std::uint8_t> application_packet,
    LegacySteamHostSessionResult &result)
{
    const AvatarCapabilityAnnouncement announcement =
        read_avatar_capability_announcement(options_.settings.protocol, application_packet);
    if (!announcement.present)
        return;
    // A peer that completed the OpenMinerZ extension hello reads the avatar
    // message under whatever id its sender uses; an add-on client reads it only
    // under its own.
    avatar_capabilities_.announce(sender.gid, announcement.message_id,
                                  extension_authorized_peers_.contains(sender.peer));
    result.inbound.push_back({LegacySteamInboundKind::avatar_capability, sender.peer,
                              {announcement.message_id},
                              MessageDeliveryPolicy::reliable_ordered, 0U, sender.gid, 0U});
}

LegacySteamHostSessionResult LegacySteamHostSession::process_avatar_packet(
    const RemotePeer &sender, std::optional<std::uint8_t> recipient_gid,
    std::vector<std::uint8_t> packet, bool reliable_ordered, std::size_t input_length)
{
    const std::uint8_t id = packet.front();
    const std::optional<std::uint8_t> sender_id = avatar_capabilities_.message_id(sender.gid);
    if (!sender_id.has_value())
        return audit_reject(sender.peer, LegacySteamRejectionStage::application,
                            "AvatarPacketFromUnannouncedPeer", input_length);
    if (!reliable_ordered)
        return audit_reject(sender.peer, LegacySteamRejectionStage::delivery,
                            "AvatarPacketDelivery", input_length);
    if (!valid_avatar_message_id(options_.settings.protocol, id) || !is_avatar_sync_packet(packet, id))
        return audit_reject(sender.peer, LegacySteamRejectionStage::application,
                            "MalformedAvatarPacket", input_length);
    LegacySteamHostSessionResult result;
    if (!recipient_gid.has_value() || *recipient_gid == 0U)
    {
        // To the host itself, which reads the message under its sender's id.
        if (id != *sender_id)
            return audit_reject(sender.peer, LegacySteamRejectionStage::application,
                                "AvatarPacketIdMismatch", input_length);
        result.inbound.push_back({LegacySteamInboundKind::avatar_sync, sender.peer,
                                  std::move(packet), MessageDeliveryPolicy::reliable_ordered, 0U,
                                  sender.gid, 0U});
        return result;
    }
    const RemotePeer *target = find_gid(*recipient_gid);
    if (target == nullptr || target->peer == sender.peer)
        return audit_reject(sender.peer, LegacySteamRejectionStage::routing, "InvalidProxyTarget",
                            input_length);
    // THE GATE, for relayed traffic: a stock host forwards this blindly and
    // trusts the sender to have checked. This one checks for itself.
    const std::optional<std::uint8_t> target_id = avatar_capabilities_.message_id(target->gid);
    if (!target_id.has_value())
        return audit_reject(sender.peer, LegacySteamRejectionStage::routing,
                            "AvatarRecipientNotCapable", input_length);
    const bool readable = id == *target_id ||
                          (avatar_capabilities_.agnostic(target->gid) && id == *sender_id);
    if (!readable)
        return audit_reject(sender.peer, LegacySteamRejectionStage::application,
                            "AvatarPacketIdMismatch", input_length);
    const auto send =
        make_direct_send(*target, sender.gid, packet, SteamDeliveryMethod::reliable_ordered);
    if (!send.has_value())
        return audit_reject(sender.peer, LegacySteamRejectionStage::session,
                            "HostSessionEncodingFailed", input_length);
    result.sends.push_back(*send);
    return result;
}

std::optional<std::vector<LegacySteamNativeSend>> LegacySteamHostSession::create_avatar_outbound(
    std::uint8_t recipient_gid, std::span<const std::uint8_t> avatar_packet,
    std::string &error) const
{
    const RemotePeer *target = find_gid(recipient_gid);
    const std::optional<std::uint8_t> target_id =
        target == nullptr ? std::nullopt : avatar_capabilities_.message_id(recipient_gid);
    if (!target_id.has_value())
    {
        error = "The recipient has not announced the Xbox Avatar add-on.";
        return std::nullopt;
    }
    if (avatar_packet.empty() || avatar_packet.front() != *target_id ||
        !valid_avatar_message_id(options_.settings.protocol, *target_id) ||
        !is_avatar_sync_packet(avatar_packet, *target_id))
    {
        error = "Not an avatar message under the id the recipient reported.";
        return std::nullopt;
    }
    const auto send =
        make_direct_send(*target, 0U, avatar_packet, SteamDeliveryMethod::reliable_ordered);
    if (!send.has_value())
    {
        error = "Failed to encode the avatar message.";
        return std::nullopt;
    }
    error.clear();
    return std::vector<LegacySteamNativeSend>{*send};
}

bool LegacySteamHostSession::avatar_capable(std::uint8_t gid) const noexcept
{
    return find_gid(gid) != nullptr && avatar_capabilities_.capable(gid);
}

std::optional<std::uint8_t> LegacySteamHostSession::avatar_message_id(
    std::uint8_t gid) const noexcept
{
    return find_gid(gid) == nullptr ? std::nullopt : avatar_capabilities_.message_id(gid);
}

std::optional<std::vector<LegacySteamNativeSend>> LegacySteamHostSession::create_outbound(
    const LegacySteamOutbound &outbound, std::string &error) const
{
    DecodedPacket decoded;
    if (!validate_application(outbound.application_packet, outbound.delivery, true, decoded))
    {
        error = "Outbound application packet or delivery violates the stock contract.";
        return std::nullopt;
    }
    const auto steam_delivery = to_steam_delivery(outbound.delivery);
    if (!steam_delivery.has_value())
    {
        error = "Outbound delivery is semantic rather than a concrete Steam delivery.";
        return std::nullopt;
    }
    std::vector<LegacySteamNativeSend> sends;
    if (outbound.client_relay)
    {
        const RemotePeer *sender = find_gid(outbound.sender_gid);
        if (outbound.channel != 1U || sender == nullptr)
        {
            error = "Client relay has an unknown sender or wrong route channel.";
            return std::nullopt;
        }
        if (outbound.recipient_gid != 0U)
        {
            const RemotePeer *target = find_gid(outbound.recipient_gid);
            if (target == nullptr ||
                (target->peer == sender->peer && !decoded.message->transport.echo))
            {
                error.clear();
                return sends;
            }
            const auto send = make_direct_send(*target, sender->gid, outbound.application_packet,
                                               *steam_delivery);
            if (!send.has_value())
            {
                error = "Failed to encode accepted direct relay.";
                return std::nullopt;
            }
            sends.push_back(*send);
            error.clear();
            return sends;
        }
        for (const RemotePeer &target : peers_)
        {
            if (target.peer == sender->peer)
                continue;
            const auto send =
                make_direct_send(target, sender->gid, outbound.application_packet, *steam_delivery);
            if (!send.has_value())
            {
                error = "Failed to encode accepted broadcast relay.";
                return std::nullopt;
            }
            sends.push_back(*send);
        }
        error.clear();
        return sends;
    }

    if (outbound.recipient_gid != 0U)
    {
        error = "Server-originated packets cannot name only a recipient GID.";
        return std::nullopt;
    }
    if (outbound.recipient.has_value())
    {
        const RemotePeer *target = find_peer(*outbound.recipient);
        if (target == nullptr)
        {
            error.clear();
            return sends;
        }
        const auto send =
            make_direct_send(*target, 0U, outbound.application_packet, *steam_delivery);
        if (!send.has_value())
        {
            error = "Failed to encode server direct packet.";
            return std::nullopt;
        }
        sends.push_back(*send);
        error.clear();
        return sends;
    }
    for (const RemotePeer &target : peers_)
    {
        const auto send =
            make_direct_send(target, 0U, outbound.application_packet, *steam_delivery);
        if (!send.has_value())
        {
            error = "Failed to encode server broadcast packet.";
            return std::nullopt;
        }
        sends.push_back(*send);
    }
    error.clear();
    return sends;
}

std::vector<LegacySteamNativeSend> LegacySteamHostSession::remove_peer(PeerId peer)
{
    extension_authorized_peers_.erase(peer);
    const auto found = std::find_if(peers_.begin(), peers_.end(),
                                    [peer](const RemotePeer &item) { return item.peer == peer; });
    if (found == peers_.end())
        return {};
    const std::uint8_t gid = found->gid;
    peers_.erase(found);
    avatar_capabilities_.forget(gid);
    const auto payload = encode_legacy_drop_peer(gid);
    LegacySteamCodecError error{};
    const auto wire = encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, payload, error);
    if (!wire.has_value())
        return {};
    std::vector<LegacySteamNativeSend> sends;
    sends.reserve(peers_.size());
    for (const RemotePeer &remaining : peers_)
        sends.push_back({remaining.peer, *wire, SteamDeliveryMethod::reliable_ordered, 0U});
    return sends;
}

bool LegacySteamHostSession::authorize_extension(PeerId peer)
{
    return peer != 0U && !connected(peer) && extension_authorized_peers_.insert(peer).second;
}

bool LegacySteamHostSession::update_password(std::string password, std::string &error)
{
    if (password.size() > 256U ||
        std::any_of(password.begin(), password.end(), [](unsigned char character) {
            return character < 0x20U || character == 0x7FU;
        }))
    {
        error = "Password must contain at most 256 printable bytes.";
        return false;
    }
    options_.settings.password = std::move(password);
    error.clear();
    return true;
}

bool LegacySteamHostSession::update_session_property(std::size_t index, std::int32_t value,
                                                     std::string &error)
{
    if (index >= properties_.size())
    {
        error = "Session property index is out of range.";
        return false;
    }
    properties_[index] = value;
    error.clear();
    return true;
}

bool LegacySteamHostSession::connected(PeerId peer) const noexcept
{
    return find_peer(peer) != nullptr;
}

bool LegacySteamHostSession::extension_capable(PeerId peer) const noexcept
{
    return extension_authorized_peers_.contains(peer);
}

bool LegacySteamHostSession::all_peers_extension_capable() const noexcept
{
    return std::all_of(peers_.begin(), peers_.end(), [this](const RemotePeer &peer) {
        return extension_authorized_peers_.contains(peer.peer);
    });
}

std::size_t LegacySteamHostSession::connected_peer_count() const noexcept
{
    return peers_.size();
}

SessionProperties LegacySteamHostSession::session_properties() const noexcept
{
    return properties_;
}

std::optional<std::uint8_t> LegacySteamHostSession::gid_for(PeerId peer) const noexcept
{
    const RemotePeer *found = find_peer(peer);
    return found == nullptr ? std::nullopt : std::optional<std::uint8_t>{found->gid};
}

std::optional<PeerId> LegacySteamHostSession::peer_for_gid(std::uint8_t gid) const noexcept
{
    const RemotePeer *found = find_gid(gid);
    return found == nullptr ? std::nullopt : std::optional<PeerId>{found->peer};
}

const LegacyGamerIdentity *LegacySteamHostSession::gamer_for_gid(std::uint8_t gid) const noexcept
{
    const RemotePeer *peer = find_gid(gid);
    return peer == nullptr ? nullptr : &peer->gamer;
}

const std::string *LegacySteamHostSession::self_reported_tag_for_gid(
    std::uint8_t gid) const noexcept
{
    const RemotePeer *peer = find_gid(gid);
    return peer == nullptr ? nullptr : &peer->self_reported_tag;
}

LegacySteamHostSessionResult LegacySteamHostSession::reject(PeerId peer, std::string reason,
                                                            std::size_t input_length,
                                                            std::string gamer_tag)
{
    // (Authority) The host's own word for a stock peer refused by an
    // OpenMinerZ-only session stays in its log; the peer reads the retail one.
    const std::string_view wire_reason = reason == legacy_openminerz_required_reason
                                             ? legacy_modded_session_denial
                                             : std::string_view{reason};
    std::vector<std::uint8_t> denial(wire_reason.size() + 2U, 0U);
    std::copy(wire_reason.begin(), wire_reason.end(), denial.begin() + 1U);
    // A refused joiner that is not already playing starts over from its mod
    // hello (for example after correcting the password). Without this the
    // second hello was reported as a duplicate and the retry could never pass.
    if (!connected(peer))
        extension_authorized_peers_.erase(peer);
    LegacySteamHostSessionResult result;
    result.sends.push_back({peer, std::move(denial), SteamDeliveryMethod::reliable_ordered, 1U});
    result.rejection_reason = reason;
    result.audit =
        LegacySteamAuditEvent{++next_audit_sequence_, peer, LegacySteamRejectionStage::approval,
                              std::move(reason), input_length, std::move(gamer_tag)};
    return result;
}

LegacySteamHostSessionResult LegacySteamHostSession::audit_reject(PeerId peer,
                                                                  LegacySteamRejectionStage stage,
                                                                  std::string reason,
                                                                  std::size_t input_length)
{
    LegacySteamHostSessionResult result;
    result.audit =
        LegacySteamAuditEvent{++next_audit_sequence_, peer, stage, std::move(reason), input_length};
    return result;
}

std::optional<std::string> LegacySteamHostSession::validate_connection(
    PeerId sender, const LegacyConnectionRequest &request) const
{
    if (request.game_name != legacy_steam_game_name)
        return "GameNamesDontMatch";
    const auto expected_version = static_cast<std::int32_t>(options_.settings.protocol);
    if (request.network_version < expected_version)
        return "ServerHasNewerVersion";
    if (request.network_version > expected_version)
        return "ServerHasOlderVersion";
    const std::string_view supplied =
        request.password.has_value() ? std::string_view{*request.password} : std::string_view{};
    if (has_non_whitespace(options_.settings.password) &&
        !constant_time_equal(options_.settings.password, supplied))
        return "IncorrectPassword";
    if ((options_.settings.join_policy != HostJoinPolicy::anyone &&
         !options_.platform_authorized) ||
        (options_.platform_authorized && !options_.platform_authorized(sender)))
        return "ConnectionDenied";
    for (std::size_t index{}; index < properties_.size(); ++index)
    {
        if (properties_[index].has_value() &&
            request.session_properties[index] != properties_[index])
            return "SessionPropertiesDontMatch";
    }
    if (requires_extension(options_.settings.mod_manifest) &&
        !extension_authorized_peers_.contains(sender))
        return std::string{legacy_openminerz_required_reason};
    // Identity is the authenticated transport peer (the Steam ID) and nothing
    // else. Gamer.PlayerID is not unique: the stock client derives it from the
    // MD5 of the first connected Ethernet adapter's MAC address, so every
    // Wi-Fi-only stock client presents the same MD5("") blob. The display tag
    // is not identity either; a collision is resolved in process_approval by
    // de-duplicating the roster tag instead of refusing the player.
    if (connected(sender))
        return "GamerAlreadyConnected";
    if (peers_.size() + 1U >= static_cast<std::size_t>(options_.settings.maximum_players))
        return "GameIsFull";
    return std::nullopt;
}

std::string LegacySteamHostSession::unique_display_tag(const std::string &requested) const
{
    // The stock protocol compares tags byte for byte (Modern SpawnEnemyMessage
    // addresses a player by Gamertag), so only an exact collision needs a new
    // roster name. Folding case here would rename players the protocol can
    // already tell apart.
    const auto taken = [&](const std::string &candidate) {
        return options_.host_gamer.gamer_tag == candidate ||
               std::any_of(peers_.begin(), peers_.end(), [&](const RemotePeer &peer) {
                   return peer.gamer.gamer_tag == candidate;
               });
    };
    if (!taken(requested))
        return requested;
    constexpr std::size_t maximum_tag_bytes = 128U;
    for (unsigned suffix = 2U; suffix < 1000U; ++suffix)
    {
        const std::string tail = " (" + std::to_string(suffix) + ")";
        std::size_t kept = std::min(requested.size(), maximum_tag_bytes - tail.size());
        // Never cut a UTF-8 sequence in half: back up over continuation bytes.
        while (kept > 0U && kept < requested.size() &&
               (static_cast<unsigned char>(requested[kept]) & 0xC0U) == 0x80U)
            --kept;
        std::string candidate = requested.substr(0U, kept) + tail;
        if (!taken(candidate))
            return candidate;
    }
    return requested;
}

std::optional<std::uint8_t> LegacySteamHostSession::allocate_gid() noexcept
{
    for (std::size_t attempt{}; attempt < 255U; ++attempt)
    {
        std::uint8_t candidate = next_gid_++;
        if (candidate == 0U)
            candidate = next_gid_++;
        if (find_gid(candidate) == nullptr)
            return candidate;
    }
    return std::nullopt;
}

const LegacySteamHostSession::RemotePeer *LegacySteamHostSession::find_peer(
    PeerId peer) const noexcept
{
    const auto found = std::find_if(peers_.begin(), peers_.end(),
                                    [peer](const RemotePeer &item) { return item.peer == peer; });
    return found == peers_.end() ? nullptr : &*found;
}

LegacySteamHostSession::RemotePeer *LegacySteamHostSession::find_peer(PeerId peer) noexcept
{
    const auto found = std::find_if(peers_.begin(), peers_.end(),
                                    [peer](const RemotePeer &item) { return item.peer == peer; });
    return found == peers_.end() ? nullptr : &*found;
}

const LegacySteamHostSession::RemotePeer *LegacySteamHostSession::find_gid(
    std::uint8_t gid) const noexcept
{
    const auto found = std::find_if(peers_.begin(), peers_.end(),
                                    [gid](const RemotePeer &item) { return item.gid == gid; });
    return found == peers_.end() ? nullptr : &*found;
}

bool LegacySteamHostSession::validate_application(std::span<const std::uint8_t> packet,
                                                  MessageDeliveryPolicy delivery,
                                                  bool validate_delivery,
                                                  DecodedPacket &decoded) const noexcept
{
    PacketError packet_error{};
    if (!decode_packet(options_.settings.protocol, packet, decoded, packet_error))
        return false;
    PayloadError payload_error{};
    if (!validate_known_payload(options_.settings.protocol, *decoded.message, decoded.payload,
                                payload_error))
        return false;
    return !validate_delivery || delivery_matches(*decoded.message, decoded.payload, delivery,
                                                  options_.settings.protocol);
}

std::optional<LegacySteamNativeSend> LegacySteamHostSession::make_direct_send(
    const RemotePeer &recipient, std::uint8_t sender_gid,
    std::span<const std::uint8_t> application_packet, SteamDeliveryMethod delivery) const
{
    LegacySteamCodecError error{};
    LegacyDirectDataFrame frame{
        recipient.gid, sender_gid, {application_packet.begin(), application_packet.end()}};
    const auto direct = encode_legacy_direct_frame(frame, error);
    if (!direct.has_value())
        return std::nullopt;
    const auto wire = encode_legacy_steam_wire(SteamIncomingMessageType::data, 0U, *direct, error);
    if (!wire.has_value())
        return std::nullopt;
    return LegacySteamNativeSend{recipient.peer, *wire, delivery, 0U};
}

LegacyGamerIdentity LegacySteamHostSession::advertised_host() const
{
    return options_.host_gamer;
}

} // namespace openminerz::net
