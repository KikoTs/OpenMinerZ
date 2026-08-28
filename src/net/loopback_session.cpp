#include "openminerz/net/loopback_session.hpp"

#include <utility>

namespace openminerz::net
{
namespace
{

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
        return false;
    return actual == message.transport.delivery;
}

} // namespace

LoopbackSession::LoopbackSession(HostSessionSettings settings, bool game_begun,
                                 std::int32_t game_mode, std::int32_t difficulty,
                                 bool infinite_resources, bool host_occupies_player_slot)
    : protocol_{settings.protocol},
      admission_{std::move(settings), game_begun,         game_mode,
                 difficulty,          infinite_resources, host_occupies_player_slot}
{
}

AdmissionDecision LoopbackSession::connect(const PeerAdmissionRequest &request)
{
    return admission_.begin(request);
}

SessionPacketResult LoopbackSession::submit_from_peer(PeerId peer,
                                                      std::span<const std::uint8_t> packet,
                                                      MessageDeliveryPolicy delivery,
                                                      std::uint8_t channel)
{
    if (channel == openminerz_control_channel)
    {
        if (delivery != MessageDeliveryPolicy::reliable_ordered)
            return {SessionPacketError::delivery_mismatch, PacketError::none, PayloadError::none,
                    QueueError::none,
                    "Extension control frames require reliable ordered delivery."};
        ExtensionAdmissionResult admission = admission_.receive_extension(peer, packet);
        if (admission.reply.empty())
            return {SessionPacketError::control_rejected, PacketError::none, PayloadError::none,
                    QueueError::none, std::move(admission.decision.detail)};
        QueueError queue_error{};
        if (!peer_inbox_.push(0U, peer, admission.reply, MessageDeliveryPolicy::reliable_ordered,
                              openminerz_control_channel, queue_error))
        {
            if (admission.decision.status == AdmissionStatus::admitted)
                admission_.disconnect(peer);
            return {SessionPacketError::queue_rejected, PacketError::none, PayloadError::none,
                    queue_error, "Extension reply queue rejected the frame."};
        }
        return {admission.decision.status == AdmissionStatus::admitted
                    ? SessionPacketError::none
                    : SessionPacketError::control_rejected,
                PacketError::none, PayloadError::none, QueueError::none,
                std::move(admission.decision.detail)};
    }
    if (channel != stock_gameplay_channel)
        return {SessionPacketError::wrong_channel, PacketError::none, PayloadError::none,
                QueueError::none, "Unknown transport channel."};
    if (!admission_.may_forward_gameplay(peer))
        return {SessionPacketError::peer_not_admitted, PacketError::none, PayloadError::none,
                QueueError::none, "Gameplay is closed until peer admission completes."};
    return submit_gameplay(peer, 0U, packet, delivery, host_inbox_);
}

SessionPacketResult LoopbackSession::submit_from_host(PeerId peer,
                                                      std::span<const std::uint8_t> packet,
                                                      MessageDeliveryPolicy delivery,
                                                      std::uint8_t channel)
{
    if (channel != stock_gameplay_channel)
        return {SessionPacketError::wrong_channel, PacketError::none, PayloadError::none,
                QueueError::none, "Host control frames are emitted only by admission."};
    if (!admission_.may_forward_gameplay(peer))
        return {SessionPacketError::peer_not_admitted, PacketError::none, PayloadError::none,
                QueueError::none, "Host cannot send gameplay to a non-admitted peer."};
    return submit_gameplay(0U, peer, packet, delivery, peer_inbox_);
}

SessionPacketResult LoopbackSession::relay_from_host(PeerId original_sender, PeerId recipient,
                                                     std::span<const std::uint8_t> packet,
                                                     MessageDeliveryPolicy delivery)
{
    if (!admission_.may_forward_gameplay(original_sender) ||
        !admission_.may_forward_gameplay(recipient))
        return {SessionPacketError::peer_not_admitted, PacketError::none, PayloadError::none,
                QueueError::none, "Relay endpoints must both be admitted session members."};
    return submit_gameplay(original_sender, recipient, packet, delivery, peer_inbox_);
}

SessionPacketResult LoopbackSession::submit_gameplay(PeerId sender, PeerId recipient,
                                                     std::span<const std::uint8_t> packet,
                                                     MessageDeliveryPolicy delivery,
                                                     BoundedDatagramQueue &destination)
{
    DecodedPacket decoded;
    PacketError frame_error{};
    if (!decode_packet(protocol_, packet, decoded, frame_error) || decoded.message == nullptr)
        return {SessionPacketError::frame_rejected, frame_error, PayloadError::none,
                QueueError::none, "Stock packet framing was rejected."};

    PayloadError payload_error{};
    if (!validate_known_payload(protocol_, *decoded.message, decoded.payload, payload_error))
        return {SessionPacketError::payload_rejected, PacketError::none, payload_error,
                QueueError::none,
                payload_error == PayloadError::unsupported_message
                    ? "Message has no typed C++ codec yet."
                    : "Typed payload validation failed."};
    if (!delivery_matches(*decoded.message, decoded.payload, delivery, protocol_))
        return {SessionPacketError::delivery_mismatch, PacketError::none, PayloadError::none,
                QueueError::none, "Delivery class does not match the stock message contract."};

    QueueError queue_error{};
    if (!destination.push(sender, recipient, packet, delivery, stock_gameplay_channel, queue_error))
        return {SessionPacketError::queue_rejected, PacketError::none, PayloadError::none,
                queue_error, "Bounded gameplay queue rejected the frame."};
    return {};
}

std::optional<Datagram> LoopbackSession::receive_for_host() noexcept
{
    return host_inbox_.pop_for_recipient(0U);
}

std::optional<Datagram> LoopbackSession::receive_for_peer(PeerId peer) noexcept
{
    return peer_inbox_.pop_for_recipient(peer);
}

void LoopbackSession::disconnect(PeerId peer) noexcept
{
    admission_.disconnect(peer);
    host_inbox_.remove_peer(peer);
    peer_inbox_.remove_peer(peer);
}

} // namespace openminerz::net
