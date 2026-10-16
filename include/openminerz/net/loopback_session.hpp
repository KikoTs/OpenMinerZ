#pragma once

#include "openminerz/net/admission.hpp"
#include "openminerz/net/payload_codec.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace openminerz::net
{

enum class SessionPacketError : std::uint8_t
{
    none,
    wrong_channel,
    peer_not_admitted,
    control_rejected,
    frame_rejected,
    payload_rejected,
    delivery_mismatch,
    queue_rejected
};

struct SessionPacketResult final
{
    SessionPacketError error{SessionPacketError::none};
    PacketError frame_error{PacketError::none};
    PayloadError payload_error{PayloadError::none};
    QueueError queue_error{QueueError::none};
    std::string detail;

    [[nodiscard]] bool accepted() const noexcept
    {
        return error == SessionPacketError::none;
    }
};

// A paired deterministic host/peer transport used to validate the complete
// admission -> channel -> framing -> typed payload -> delivery pipeline before
// any Steam packet is published.
class LoopbackSession final
{
  public:
    LoopbackSession(HostSessionSettings settings, bool join_closed, std::int32_t game_mode,
                    std::int32_t difficulty, bool infinite_resources,
                    bool host_occupies_player_slot = true);

    [[nodiscard]] AdmissionDecision connect(const PeerAdmissionRequest &request);
    [[nodiscard]] SessionPacketResult submit_from_peer(PeerId peer,
                                                       std::span<const std::uint8_t> packet,
                                                       MessageDeliveryPolicy delivery,
                                                       std::uint8_t channel);
    [[nodiscard]] SessionPacketResult submit_from_host(
        PeerId peer, std::span<const std::uint8_t> packet, MessageDeliveryPolicy delivery,
        std::uint8_t channel = stock_gameplay_channel);
    [[nodiscard]] SessionPacketResult relay_from_host(PeerId original_sender, PeerId recipient,
                                                      std::span<const std::uint8_t> packet,
                                                      MessageDeliveryPolicy delivery);

    [[nodiscard]] std::optional<Datagram> receive_for_host() noexcept;
    [[nodiscard]] std::optional<Datagram> receive_for_peer(PeerId peer) noexcept;
    void disconnect(PeerId peer) noexcept;

    [[nodiscard]] const HostAdmissionGate &admission() const noexcept
    {
        return admission_;
    }

  private:
    // (Network) A mod channel or mod download frame on the control channel.
    [[nodiscard]] SessionPacketResult submit_mod_frame(PeerId peer, bool from_peer,
                                                       std::span<const std::uint8_t> packet);
    [[nodiscard]] SessionPacketResult submit_gameplay(PeerId sender, PeerId recipient,
                                                      std::span<const std::uint8_t> packet,
                                                      MessageDeliveryPolicy delivery,
                                                      BoundedDatagramQueue &destination);

    ProtocolVersion protocol_;
    HostAdmissionGate admission_;
    BoundedDatagramQueue host_inbox_;
    BoundedDatagramQueue peer_inbox_;
};

} // namespace openminerz::net
