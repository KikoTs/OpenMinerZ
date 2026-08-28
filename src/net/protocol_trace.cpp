#include "openminerz/net/protocol_trace.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <system_error>

namespace openminerz::net
{
namespace
{

std::string_view role_label(ProtocolTraceRole role) noexcept
{
    return role == ProtocolTraceRole::host ? "host" : "client";
}

std::string_view direction_label(ProtocolTraceDirection direction) noexcept
{
    switch (direction)
    {
    case ProtocolTraceDirection::inbound:
        return "inbound";
    case ProtocolTraceDirection::outbound:
        return "outbound";
    case ProtocolTraceDirection::internal:
        return "internal";
    }
    return "internal";
}

std::string_view result_label(ProtocolTraceResult result) noexcept
{
    switch (result)
    {
    case ProtocolTraceResult::accepted:
        return "accepted";
    case ProtocolTraceResult::rejected:
        return "rejected";
    case ProtocolTraceResult::failure:
        return "failure";
    }
    return "failure";
}

std::string_view delivery_label(MessageDeliveryPolicy delivery) noexcept
{
    switch (delivery)
    {
    case MessageDeliveryPolicy::unreliable:
        return "unreliable";
    case MessageDeliveryPolicy::reliable_unordered:
        return "reliable_unordered";
    case MessageDeliveryPolicy::unreliable_sequenced:
        return "unreliable_sequenced";
    case MessageDeliveryPolicy::reliable_sequenced:
        return "reliable_sequenced";
    case MessageDeliveryPolicy::reliable_ordered:
        return "reliable_ordered";
    case MessageDeliveryPolicy::chat:
        return "chat";
    case MessageDeliveryPolicy::player_update_state:
        return "player_update_state";
    case MessageDeliveryPolicy::dragon_update_warmup:
        return "dragon_update_warmup";
    }
    return "unknown";
}

bool valid_event_label(std::string_view event) noexcept
{
    return !event.empty() && event.size() <= 128U &&
           std::ranges::all_of(event, [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '_' || character == '.' ||
                      character == '-';
           });
}

std::optional<std::uint64_t> reason_fingerprint(std::string_view reason) noexcept
{
    if (reason.empty())
        return std::nullopt;
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : reason)
        hash = (hash ^ character) * 1099511628211ULL;
    return hash;
}

} // namespace

std::string encode_protocol_trace_record_json(std::uint64_t sequence,
                                              std::int64_t unix_milliseconds,
                                              std::uint64_t elapsed_microseconds,
                                              const ProtocolTraceRecord &record)
{
    const std::string_view event = valid_event_label(record.event) ? std::string_view{record.event}
                                                                   : "InvalidEventLabel";
    const auto reason_hash = reason_fingerprint(record.reason);
    const std::string recipient = record.recipient_gid.has_value()
                                      ? std::to_string(*record.recipient_gid)
                                      : std::string{"null"};
    const std::string fingerprint =
        reason_hash.has_value() ? std::format("\"{:016x}\"", *reason_hash) : "null";
    return std::format(
        "{{\"schema\":\"openminerz.protocol-trace.v1\",\"sequence\":{},"
        "\"unix_ms\":{},\"elapsed_us\":{},\"role\":\"{}\",\"direction\":\"{}\","
        "\"result\":\"{}\",\"protocol\":{},\"event\":\"{}\",\"sender_gid\":{},"
        "\"recipient_gid\":{},\"delivery\":\"{}\",\"channel\":{},"
        "\"packet_bytes\":{},\"reason_hash\":{}}}",
        sequence, unix_milliseconds, elapsed_microseconds, role_label(record.role),
        direction_label(record.direction), result_label(record.result),
        static_cast<unsigned int>(record.protocol), event,
        static_cast<unsigned int>(record.sender_gid), recipient, delivery_label(record.delivery),
        static_cast<unsigned int>(record.channel), record.packet_bytes, fingerprint);
}

bool ProtocolTraceWriter::open(const std::filesystem::path &path, ProtocolTraceRole role,
                               ProtocolVersion protocol, std::string &error)
{
    if (path.empty() || output_.is_open())
    {
        error = "The protocol trace path is empty or already active";
        return false;
    }
    std::error_code filesystem_error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error)
    {
        error = "The protocol trace directory could not be created";
        return false;
    }
    output_.open(path, std::ios::binary | std::ios::out | std::ios::app);
    if (!output_)
    {
        error = "The protocol trace file could not be opened";
        return false;
    }
    role_ = role;
    protocol_ = protocol;
    started_ = std::chrono::steady_clock::now();
    sequence_ = 0U;
    if (!record(ProtocolTraceDirection::internal, ProtocolTraceResult::accepted, "TraceStarted"))
    {
        output_.close();
        error = "The protocol trace header could not be written";
        return false;
    }
    error.clear();
    return true;
}

void ProtocolTraceWriter::set_protocol(ProtocolVersion protocol) noexcept
{
    protocol_ = protocol;
}

bool ProtocolTraceWriter::record(ProtocolTraceDirection direction, ProtocolTraceResult result,
                                 std::string_view event, std::uint8_t sender_gid,
                                 std::optional<std::uint8_t> recipient_gid,
                                 MessageDeliveryPolicy delivery, std::uint8_t channel,
                                 std::size_t packet_bytes, std::string_view reason) noexcept
{
    if (!output_.is_open() || sequence_ >= maximum_records)
        return false;
    try
    {
        const auto now = std::chrono::steady_clock::now();
        const auto wall = std::chrono::system_clock::now();
        ProtocolTraceRecord record;
        record.role = role_;
        record.direction = direction;
        record.result = result;
        record.protocol = protocol_;
        record.event = std::string{event};
        record.sender_gid = sender_gid;
        record.recipient_gid = recipient_gid;
        record.delivery = delivery;
        record.channel = channel;
        record.packet_bytes = packet_bytes;
        record.reason = std::string{reason};
        const auto unix_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count();
        const auto elapsed_microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(now - started_).count();
        output_ << encode_protocol_trace_record_json(
                       ++sequence_, unix_milliseconds,
                       static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed_microseconds)),
                       record)
                << '\n'
                << std::flush;
        return output_.good();
    }
    catch (...)
    {
        return false;
    }
}

bool ProtocolTraceWriter::record_packet(
    ProtocolTraceDirection direction, ProtocolTraceResult result,
    std::span<const std::uint8_t> application_packet, std::uint8_t sender_gid,
    std::optional<std::uint8_t> recipient_gid, MessageDeliveryPolicy delivery,
    std::uint8_t channel, std::string_view reason) noexcept
{
    DecodedPacket decoded;
    PacketError packet_error{};
    const std::string_view event =
        decode_packet(protocol_, application_packet, decoded, packet_error) &&
                decoded.message != nullptr
            ? decoded.message->name
            : std::string_view{"InvalidApplicationPacket"};
    return record(direction, result, event, sender_gid, recipient_gid, delivery, channel,
                  application_packet.size(), reason);
}

bool ProtocolTraceWriter::active() const noexcept
{
    return output_.is_open() && output_.good();
}

} // namespace openminerz::net
