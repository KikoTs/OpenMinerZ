#include "openminerz/net/transport.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace openminerz::net
{

BoundedDatagramQueue::BoundedDatagramQueue(std::size_t maximum_packets,
                                           std::size_t maximum_buffered_bytes,
                                           std::size_t maximum_datagram_bytes)
    : maximum_packets_{maximum_packets}, maximum_buffered_bytes_{maximum_buffered_bytes},
      maximum_datagram_bytes_{maximum_datagram_bytes}
{
    if (maximum_packets_ == 0U || maximum_buffered_bytes_ == 0U || maximum_datagram_bytes_ == 0U ||
        maximum_datagram_bytes_ > maximum_buffered_bytes_)
        throw std::invalid_argument("invalid bounded datagram queue limits");
}

bool BoundedDatagramQueue::push(PeerId sender, PeerId recipient,
                                std::span<const std::uint8_t> payload,
                                MessageDeliveryPolicy delivery, std::uint8_t channel,
                                QueueError &error)
{
    if (payload.empty())
    {
        error = QueueError::empty_payload;
        return false;
    }
    if (payload.size() > maximum_datagram_bytes_)
    {
        error = QueueError::packet_too_large;
        return false;
    }
    if (queue_.size() >= maximum_packets_)
    {
        error = QueueError::queue_full;
        return false;
    }
    if (payload.size() >
        maximum_buffered_bytes_ - std::min(maximum_buffered_bytes_, buffered_bytes_))
    {
        error = QueueError::byte_budget_exhausted;
        return false;
    }

    Datagram datagram;
    datagram.sender = sender;
    datagram.recipient = recipient;
    datagram.delivery = delivery;
    datagram.channel = channel;
    datagram.payload.assign(payload.begin(), payload.end());
    buffered_bytes_ += datagram.payload.size();
    queue_.push_back(std::move(datagram));
    error = QueueError::none;
    return true;
}

std::optional<Datagram> BoundedDatagramQueue::pop() noexcept
{
    if (queue_.empty())
        return std::nullopt;
    Datagram datagram = std::move(queue_.front());
    queue_.pop_front();
    buffered_bytes_ -= datagram.payload.size();
    return datagram;
}

std::optional<Datagram> BoundedDatagramQueue::pop_for_recipient(PeerId recipient) noexcept
{
    const auto found =
        std::find_if(queue_.begin(), queue_.end(),
                     [recipient](const auto &datagram) { return datagram.recipient == recipient; });
    if (found == queue_.end())
        return std::nullopt;
    Datagram datagram = std::move(*found);
    buffered_bytes_ -= datagram.payload.size();
    queue_.erase(found);
    return datagram;
}

void BoundedDatagramQueue::clear() noexcept
{
    queue_.clear();
    buffered_bytes_ = 0U;
}

void BoundedDatagramQueue::remove_peer(PeerId peer) noexcept
{
    for (auto iterator = queue_.begin(); iterator != queue_.end();)
    {
        if (iterator->sender == peer || iterator->recipient == peer)
        {
            buffered_bytes_ -= iterator->payload.size();
            iterator = queue_.erase(iterator);
        }
        else
            ++iterator;
    }
}

NativeEnvelopeError validate_advertised_length(std::uint64_t advertised_length,
                                               std::size_t maximum_wire_size) noexcept
{
    if (maximum_wire_size == 0U || advertised_length == 0U || advertised_length > maximum_wire_size)
        return NativeEnvelopeError::invalid_advertised_length;
    return NativeEnvelopeError::none;
}

NativeEnvelopeError validate_native_read(std::uint64_t advertised_length, bool read_succeeded,
                                         std::uint64_t received_length,
                                         bool sender_is_session_member) noexcept
{
    if (!read_succeeded)
        return NativeEnvelopeError::read_failed;
    if (received_length != advertised_length)
        return NativeEnvelopeError::length_mismatch;
    if (!sender_is_session_member)
        return NativeEnvelopeError::sender_not_session_member;
    return NativeEnvelopeError::none;
}

} // namespace openminerz::net
