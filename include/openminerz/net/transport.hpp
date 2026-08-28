#pragma once

#include "openminerz/net/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace openminerz::net
{

using PeerId = std::uint64_t;

inline constexpr std::uint8_t stock_gameplay_channel = 0U;
inline constexpr std::uint8_t openminerz_control_channel = 1U;

struct Datagram final
{
    PeerId sender{};
    PeerId recipient{};
    std::vector<std::uint8_t> payload;
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::reliable_ordered};
    std::uint8_t channel{stock_gameplay_channel};
};

enum class QueueError : std::uint8_t
{
    none,
    empty_payload,
    packet_too_large,
    queue_full,
    byte_budget_exhausted
};

// Deterministic in-memory transport boundary used before Steam. Capacity and
// byte limits are checked before copying attacker-controlled payloads.
class BoundedDatagramQueue final
{
  public:
    explicit BoundedDatagramQueue(std::size_t maximum_packets = 1024U,
                                  std::size_t maximum_buffered_bytes = 4U * 1024U * 1024U,
                                  std::size_t maximum_datagram_bytes = maximum_packet_size);

    [[nodiscard]] bool push(PeerId sender, PeerId recipient, std::span<const std::uint8_t> payload,
                            MessageDeliveryPolicy delivery, std::uint8_t channel,
                            QueueError &error);
    [[nodiscard]] std::optional<Datagram> pop() noexcept;
    [[nodiscard]] std::optional<Datagram> pop_for_recipient(PeerId recipient) noexcept;
    [[nodiscard]] std::size_t size() const noexcept
    {
        return queue_.size();
    }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept
    {
        return buffered_bytes_;
    }
    void clear() noexcept;
    void remove_peer(PeerId peer) noexcept;

  private:
    std::size_t maximum_packets_{};
    std::size_t maximum_buffered_bytes_{};
    std::size_t maximum_datagram_bytes_{};
    std::size_t buffered_bytes_{};
    std::deque<Datagram> queue_;
};

enum class NativeEnvelopeError : std::uint8_t
{
    none,
    invalid_advertised_length,
    read_failed,
    length_mismatch,
    sender_not_session_member
};

[[nodiscard]] NativeEnvelopeError validate_advertised_length(
    std::uint64_t advertised_length, std::size_t maximum_wire_size = maximum_packet_size) noexcept;
[[nodiscard]] NativeEnvelopeError validate_native_read(std::uint64_t advertised_length,
                                                       bool read_succeeded,
                                                       std::uint64_t received_length,
                                                       bool sender_is_session_member) noexcept;

} // namespace openminerz::net
