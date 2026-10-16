#include "openminerz/net/mod_channel.hpp"

#include <algorithm>
#include <array>

namespace openminerz::net
{
namespace
{

constexpr std::array<std::uint8_t, 4U> mod_frame_magic{'O', 'M', 'Z', 'E'};

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1U] << 8U));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_u16(std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void write_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}

} // namespace

const char *mod_frame_error_name(ModFrameError error) noexcept
{
    switch (error)
    {
    case ModFrameError::none:
        return "none";
    case ModFrameError::too_small:
        return "too_small";
    case ModFrameError::too_large:
        return "too_large";
    case ModFrameError::wrong_magic:
        return "wrong_magic";
    case ModFrameError::wrong_kind:
        return "wrong_kind";
    case ModFrameError::checksum_mismatch:
        return "checksum_mismatch";
    case ModFrameError::invalid_fragment:
        return "invalid_fragment";
    case ModFrameError::trailing_data:
        return "trailing_data";
    case ModFrameError::out_of_order:
        return "out_of_order";
    case ModFrameError::message_too_large:
        return "message_too_large";
    case ModFrameError::rate_limited:
        return "rate_limited";
    }
    return "unknown";
}

bool decode_mod_frame(std::span<const std::uint8_t> packet, ModFrame &frame,
                      ModFrameError &error) noexcept
{
    frame = {};
    if (packet.size() < mod_frame_overhead + 1U)
    {
        error = ModFrameError::too_small;
        return false;
    }
    if (packet.size() > maximum_extension_packet_size)
    {
        error = ModFrameError::too_large;
        return false;
    }
    if (!std::equal(mod_frame_magic.begin(), mod_frame_magic.end(), packet.begin()) ||
        packet[4] != extension_protocol_version)
    {
        error = ModFrameError::wrong_magic;
        return false;
    }
    if (packet[5] != static_cast<std::uint8_t>(ExtensionMessageKind::mod_data))
    {
        error = ModFrameError::wrong_kind;
        return false;
    }
    if (extension_crc32(packet.first(packet.size() - 4U)) != read_u32(packet, packet.size() - 4U))
    {
        error = ModFrameError::checksum_mismatch;
        return false;
    }
    frame.sequence = read_u32(packet, 6U);
    frame.index = read_u16(packet, 10U);
    frame.count = read_u16(packet, 12U);
    frame.total = read_u32(packet, 14U);
    const std::uint16_t length = read_u16(packet, 18U);
    if (frame.total == 0U || frame.total > maximum_mod_message_bytes || frame.count == 0U ||
        frame.count != mod_fragment_count(frame.total) || frame.index >= frame.count)
    {
        error = ModFrameError::invalid_fragment;
        return false;
    }
    const std::size_t expected =
        frame.index + 1U < frame.count
            ? mod_frame_max_chunk
            : frame.total - static_cast<std::size_t>(frame.count - 1U) * mod_frame_max_chunk;
    if (length != expected)
    {
        error = ModFrameError::invalid_fragment;
        return false;
    }
    if (packet.size() != mod_frame_overhead + length)
    {
        error = packet.size() > mod_frame_overhead + length ? ModFrameError::trailing_data
                                                            : ModFrameError::too_small;
        return false;
    }
    frame.chunk = packet.subspan(mod_frame_header_bytes, length);
    error = ModFrameError::none;
    return true;
}

std::vector<std::vector<std::uint8_t>> encode_mod_message(std::uint32_t sequence,
                                                          std::span<const std::uint8_t> message)
{
    std::vector<std::vector<std::uint8_t>> packets;
    if (message.empty() || message.size() > maximum_mod_message_bytes)
        return packets;
    const std::size_t count = mod_fragment_count(message.size());
    packets.reserve(count);
    for (std::size_t index = 0U; index < count; ++index)
    {
        const std::span<const std::uint8_t> chunk = message.subspan(
            index * mod_frame_max_chunk,
            std::min(mod_frame_max_chunk, message.size() - index * mod_frame_max_chunk));
        std::vector<std::uint8_t> packet;
        packet.reserve(mod_frame_overhead + chunk.size());
        packet.insert(packet.end(), mod_frame_magic.begin(), mod_frame_magic.end());
        packet.push_back(extension_protocol_version);
        packet.push_back(static_cast<std::uint8_t>(ExtensionMessageKind::mod_data));
        write_u32(packet, sequence);
        write_u16(packet, static_cast<std::uint16_t>(index));
        write_u16(packet, static_cast<std::uint16_t>(count));
        write_u32(packet, static_cast<std::uint32_t>(message.size()));
        write_u16(packet, static_cast<std::uint16_t>(chunk.size()));
        packet.insert(packet.end(), chunk.begin(), chunk.end());
        write_u32(packet, extension_crc32(packet));
        packets.push_back(std::move(packet));
    }
    return packets;
}

ModMessageAssembler::ModMessageAssembler(std::size_t maximum_message_bytes,
                                         double timeout_seconds) noexcept
    : maximum_message_bytes_{std::min(maximum_message_bytes, maximum_mod_message_bytes)},
      timeout_seconds_{timeout_seconds}
{
}

ModMessageAssembler::Result ModMessageAssembler::accept(const ModFrame &frame, double now,
                                                        std::vector<std::uint8_t> &message,
                                                        ModFrameError &error)
{
    const auto reject = [&](ModFrameError reason) {
        reset();
        error = reason;
        return Result::rejected;
    };
    if (!assembling_)
    {
        // A message starts with its first fragment and a fresh number. A
        // repeated or older number is a replay, never a retransmission: the
        // transport is reliable.
        if (frame.index != 0U || frame.sequence <= last_sequence_)
            return reject(ModFrameError::out_of_order);
        if (frame.total > maximum_message_bytes_)
            return reject(ModFrameError::message_too_large);
        assembling_ = true;
        sequence_ = frame.sequence;
        next_index_ = 0U;
        count_ = frame.count;
        total_ = frame.total;
        started_ = now;
        partial_.clear();
    }
    else if (frame.sequence != sequence_ || frame.index != next_index_ || frame.count != count_ ||
             frame.total != total_)
    {
        return reject(ModFrameError::out_of_order);
    }
    // Grows by what actually arrived, never by the declared total.
    partial_.insert(partial_.end(), frame.chunk.begin(), frame.chunk.end());
    ++next_index_;
    if (next_index_ < count_)
    {
        error = ModFrameError::none;
        return Result::pending;
    }
    last_sequence_ = sequence_;
    message = std::move(partial_);
    partial_ = {};
    assembling_ = false;
    error = ModFrameError::none;
    return Result::complete;
}

bool ModMessageAssembler::expire(double now) noexcept
{
    if (!assembling_ || now - started_ < timeout_seconds_)
        return false;
    reset();
    return true;
}

void ModMessageAssembler::reset() noexcept
{
    partial_.clear();
    partial_.shrink_to_fit();
    assembling_ = false;
    next_index_ = 0U;
    count_ = 0U;
    total_ = 0U;
}

TokenBucket::TokenBucket(double rate, double burst) noexcept
    : rate_{std::max(rate, 0.0)}, burst_{std::max(burst, 0.0)}, tokens_{burst_}
{
}

void TokenBucket::refill(double now) noexcept
{
    if (!started_)
    {
        started_ = true;
        last_ = now;
        return;
    }
    if (now > last_)
    {
        tokens_ = std::min(burst_, tokens_ + (now - last_) * rate_);
        last_ = now;
    }
}

bool TokenBucket::take(double amount, double now) noexcept
{
    refill(now);
    if (tokens_ < amount)
        return false;
    tokens_ -= amount;
    return true;
}

bool TokenBucket::take_allowing_debt(double amount, double now) noexcept
{
    refill(now);
    if (tokens_ <= 0.0)
        return false;
    tokens_ -= amount;
    return true;
}

void TokenBucket::consume(double amount, double now) noexcept
{
    refill(now);
    tokens_ -= amount;
}

double TokenBucket::available(double now) noexcept
{
    refill(now);
    return tokens_;
}

} // namespace openminerz::net
