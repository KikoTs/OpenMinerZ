#include "openminerz/net/legacy_steam_codec.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::size_t maximum_player_id_bytes = 1024U;
constexpr std::size_t maximum_gamer_tag_bytes = 128U;
constexpr std::size_t maximum_password_bytes = 256U;
constexpr std::size_t maximum_game_name_bytes = 128U;
constexpr std::size_t maximum_legacy_peers = 16U;

bool valid_message_type(SteamIncomingMessageType value) noexcept
{
    switch (value)
    {
    case SteamIncomingMessageType::error:
    case SteamIncomingMessageType::status_changed:
    case SteamIncomingMessageType::unconnected_data:
    case SteamIncomingMessageType::connection_approval:
    case SteamIncomingMessageType::data:
    case SteamIncomingMessageType::receipt:
    case SteamIncomingMessageType::discovery_request:
    case SteamIncomingMessageType::discovery_response:
    case SteamIncomingMessageType::verbose_debug_message:
    case SteamIncomingMessageType::debug_message:
    case SteamIncomingMessageType::warning_message:
    case SteamIncomingMessageType::error_message:
    case SteamIncomingMessageType::nat_introduction_success:
    case SteamIncomingMessageType::connection_latency_updated:
        return true;
    }
    return false;
}

bool valid_delivery(SteamDeliveryMethod value) noexcept
{
    switch (value)
    {
    case SteamDeliveryMethod::unknown:
    case SteamDeliveryMethod::unreliable:
    case SteamDeliveryMethod::unreliable_sequenced:
    case SteamDeliveryMethod::reliable_unordered:
    case SteamDeliveryMethod::reliable_sequenced:
    case SteamDeliveryMethod::reliable_ordered:
        return true;
    }
    return false;
}

class LidgrenBitWriter final
{
  public:
    explicit LidgrenBitWriter(std::size_t maximum_bytes = legacy_steam_maximum_wire_size)
        : maximum_bytes_{maximum_bytes}
    {
    }

    bool write_bool(bool value)
    {
        return write_bits(value ? 1U : 0U, 1U);
    }
    bool write_u8(std::uint8_t value)
    {
        return write_bits(value, 8U);
    }
    bool write_i32(std::int32_t value)
    {
        return write_bits(std::bit_cast<std::uint32_t>(value), 32U);
    }
    bool write_bytes(std::span<const std::uint8_t> bytes)
    {
        for (const std::uint8_t value : bytes)
        {
            if (!write_u8(value))
                return false;
        }
        return true;
    }
    bool write_byte_array(std::span<const std::uint8_t> bytes)
    {
        return bytes.size() <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) &&
               write_i32(static_cast<std::int32_t>(bytes.size())) && write_bytes(bytes);
    }
    // Keep the conversion explicit so the wire stays independent of char
    // signedness.
    bool write_utf8(std::string_view value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max() ||
            !write_variable_u32(static_cast<std::uint32_t>(value.size())))
            return false;
        for (const unsigned char character : value)
        {
            if (!write_u8(character))
                return false;
        }
        return true;
    }
    bool write_variable_u32(std::uint32_t value)
    {
        while (value >= 0x80U)
        {
            if (!write_u8(static_cast<std::uint8_t>(value | 0x80U)))
                return false;
            value >>= 7U;
        }
        return write_u8(static_cast<std::uint8_t>(value));
    }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept
    {
        return std::move(bytes_);
    }

  private:
    bool write_bits(std::uint32_t value, std::size_t bit_count)
    {
        if (bit_count > 32U || bit_length_ > maximum_bytes_ * 8U ||
            bit_count > maximum_bytes_ * 8U - bit_length_)
            return false;
        const std::size_t required_bytes = (bit_length_ + bit_count + 7U) / 8U;
        if (required_bytes > bytes_.size())
            bytes_.resize(required_bytes, 0U);
        for (std::size_t bit{}; bit < bit_count; ++bit)
        {
            const std::size_t destination = bit_length_ + bit;
            const std::uint8_t mask = static_cast<std::uint8_t>(1U << (destination & 7U));
            if (((value >> bit) & 1U) != 0U)
                bytes_[destination >> 3U] |= mask;
            else
                bytes_[destination >> 3U] &= static_cast<std::uint8_t>(~mask);
        }
        bit_length_ += bit_count;
        return true;
    }

    std::size_t maximum_bytes_{};
    std::size_t bit_length_{};
    std::vector<std::uint8_t> bytes_;
};

class LidgrenBitReader final
{
  public:
    explicit LidgrenBitReader(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes}
    {
    }

    [[nodiscard]] std::size_t remaining_bits() const noexcept
    {
        return bytes_.size() * 8U - bit_offset_;
    }
    [[nodiscard]] bool exhausted() const noexcept
    {
        return remaining_bits() == 0U;
    }
    bool read_bool(bool &value) noexcept
    {
        std::uint32_t encoded{};
        if (!read_bits(1U, encoded))
            return false;
        value = encoded != 0U;
        return true;
    }
    bool read_u8(std::uint8_t &value) noexcept
    {
        std::uint32_t encoded{};
        if (!read_bits(8U, encoded))
            return false;
        value = static_cast<std::uint8_t>(encoded);
        return true;
    }
    bool read_i32(std::int32_t &value) noexcept
    {
        std::uint32_t encoded{};
        if (!read_bits(32U, encoded))
            return false;
        value = std::bit_cast<std::int32_t>(encoded);
        return true;
    }
    bool read_bytes(std::size_t count, std::vector<std::uint8_t> &value)
    {
        if (count > remaining_bits() / 8U)
            return false;
        std::vector<std::uint8_t> decoded(count);
        for (std::uint8_t &item : decoded)
        {
            if (!read_u8(item))
                return false;
        }
        value = std::move(decoded);
        return true;
    }
    bool read_byte_array(std::size_t maximum_length, std::vector<std::uint8_t> &value)
    {
        std::int32_t count{};
        if (!read_i32(count) || count < 0 || static_cast<std::size_t>(count) > maximum_length)
            return false;
        return read_bytes(static_cast<std::size_t>(count), value);
    }
    bool read_string(std::size_t maximum_bytes, std::string &value)
    {
        std::uint32_t count{};
        std::vector<std::uint8_t> decoded;
        if (!read_variable_u32(count) || count > maximum_bytes ||
            !read_bytes(static_cast<std::size_t>(count), decoded))
            return false;
        value.assign(decoded.begin(), decoded.end());
        return true;
    }
    bool read_variable_u32(std::uint32_t &value) noexcept
    {
        std::uint32_t result{};
        for (std::size_t index{}; index < 5U; ++index)
        {
            std::uint8_t current{};
            if (!read_u8(current) || (index == 4U && (current & 0xF0U) != 0U))
                return false;
            result |= static_cast<std::uint32_t>(current & 0x7FU) << (index * 7U);
            if ((current & 0x80U) == 0U)
            {
                value = result;
                return true;
            }
        }
        return false;
    }

  private:
    bool read_bits(std::size_t bit_count, std::uint32_t &value) noexcept
    {
        if (bit_count > 32U || remaining_bits() < bit_count)
            return false;
        std::uint32_t result{};
        for (std::size_t bit{}; bit < bit_count; ++bit)
        {
            const std::size_t source = bit_offset_ + bit;
            if (((bytes_[source >> 3U] >> (source & 7U)) & 1U) != 0U)
                result |= 1U << bit;
        }
        bit_offset_ += bit_count;
        value = result;
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t bit_offset_{};
};

bool write_gamer(LidgrenBitWriter &writer, const LegacyGamerIdentity &gamer,
                 LegacySteamCodecError &error)
{
    if (gamer.player_id.size() > maximum_player_id_bytes ||
        gamer.gamer_tag.size() > maximum_gamer_tag_bytes)
    {
        error = LegacySteamCodecError::out_of_range;
        return false;
    }
    if (!writer.write_byte_array(gamer.player_id) || !writer.write_utf8(gamer.gamer_tag))
    {
        error = LegacySteamCodecError::too_large;
        return false;
    }
    return true;
}

bool read_gamer(LidgrenBitReader &reader, LegacyGamerIdentity &gamer)
{
    LegacyGamerIdentity decoded;
    if (!reader.read_byte_array(maximum_player_id_bytes, decoded.player_id) ||
        !reader.read_string(maximum_gamer_tag_bytes, decoded.gamer_tag))
        return false;
    gamer = std::move(decoded);
    return true;
}

bool write_properties(LidgrenBitWriter &writer, const SessionProperties &properties)
{
    if (!writer.write_i32(static_cast<std::int32_t>(properties.size())))
        return false;
    for (const std::optional<std::int32_t> property : properties)
    {
        if (!writer.write_bool(property.has_value()) ||
            (property.has_value() && !writer.write_i32(*property)))
            return false;
    }
    return true;
}

bool read_properties(LidgrenBitReader &reader, SessionProperties &properties)
{
    std::int32_t count{};
    if (!reader.read_i32(count) || count != static_cast<std::int32_t>(properties.size()))
        return false;
    SessionProperties decoded{};
    for (std::optional<std::int32_t> &property : decoded)
    {
        bool present{};
        if (!reader.read_bool(present))
            return false;
        if (present)
        {
            std::int32_t value{};
            if (!reader.read_i32(value))
                return false;
            property = value;
        }
    }
    properties = decoded;
    return true;
}

void append_i32(std::vector<std::uint8_t> &bytes, std::int32_t value)
{
    const std::uint32_t encoded = std::bit_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<std::uint8_t>(encoded));
    bytes.push_back(static_cast<std::uint8_t>(encoded >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(encoded >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(encoded >> 24U));
}

bool read_i32_at(std::span<const std::uint8_t> bytes, std::size_t offset,
                 std::int32_t &value) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::int32_t))
        return false;
    const std::uint32_t encoded = static_cast<std::uint32_t>(bytes[offset]) |
                                  (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
                                  (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
                                  (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    value = std::bit_cast<std::int32_t>(encoded);
    return true;
}

bool validate_application_packet(std::span<const std::uint8_t> packet,
                                 LegacySteamCodecError &error) noexcept
{
    if (packet.size() < minimum_packet_size)
    {
        error = LegacySteamCodecError::too_small;
        return false;
    }
    if (packet.size() > maximum_packet_size)
    {
        error = LegacySteamCodecError::too_large;
        return false;
    }
    return true;
}

bool read_application_packet(std::span<const std::uint8_t> encoded, std::size_t length_offset,
                             std::vector<std::uint8_t> &packet, LegacySteamCodecError &error)
{
    std::int32_t length{};
    if (!read_i32_at(encoded, length_offset, length))
    {
        error = LegacySteamCodecError::too_small;
        return false;
    }
    const std::size_t data_offset = length_offset + sizeof(std::int32_t);
    if (length < static_cast<std::int32_t>(minimum_packet_size) ||
        length > static_cast<std::int32_t>(maximum_packet_size))
    {
        error = LegacySteamCodecError::out_of_range;
        return false;
    }
    if (data_offset > encoded.size() ||
        static_cast<std::size_t>(length) != encoded.size() - data_offset)
    {
        error = LegacySteamCodecError::length_mismatch;
        return false;
    }
    packet.assign(encoded.begin() + static_cast<std::ptrdiff_t>(data_offset), encoded.end());
    return true;
}

} // namespace

bool uses_unreliable_native_send(SteamDeliveryMethod delivery) noexcept
{
    // The original's SteamWorks.SendPacket selects k_EP2PSendUnreliable with an
    // unsigned `deliveryMethod - Unreliable <= 1`, so only Unreliable and
    // UnreliableSequenced qualify. Unknown is zero, underflows that
    // subtraction, and is therefore sent reliably - which matters because
    // SendDataOptions.Chat (chat and voice) maps to Unknown.
    return delivery == SteamDeliveryMethod::unreliable ||
           delivery == SteamDeliveryMethod::unreliable_sequenced;
}

std::optional<std::vector<std::uint8_t>> encode_legacy_steam_wire(
    SteamIncomingMessageType message_type, std::uint8_t channel,
    std::span<const std::uint8_t> payload, LegacySteamCodecError &error)
{
    error = LegacySteamCodecError::none;
    if (!valid_message_type(message_type))
    {
        error = LegacySteamCodecError::invalid_message_type;
        return std::nullopt;
    }
    if (payload.size() > legacy_steam_maximum_wire_size - legacy_steam_wire_header_size)
    {
        error = LegacySteamCodecError::too_large;
        return std::nullopt;
    }
    const std::uint16_t type = static_cast<std::uint16_t>(message_type);
    std::vector<std::uint8_t> wire(legacy_steam_wire_header_size + payload.size());
    wire[0] = static_cast<std::uint8_t>(type);
    wire[1] = static_cast<std::uint8_t>(type >> 8U);
    wire[2] = channel;
    wire[3] = 0U;
    std::copy(payload.begin(), payload.end(), wire.begin() + legacy_steam_wire_header_size);
    error = LegacySteamCodecError::none;
    return wire;
}

bool decode_legacy_steam_wire(std::span<const std::uint8_t> wire, LegacySteamWirePacket &packet,
                              LegacySteamCodecError &error)
{
    if (wire.size() < legacy_steam_wire_header_size)
    {
        error = LegacySteamCodecError::too_small;
        return false;
    }
    if (wire.size() > legacy_steam_maximum_wire_size)
    {
        error = LegacySteamCodecError::too_large;
        return false;
    }
    const auto type = static_cast<SteamIncomingMessageType>(
        static_cast<std::uint16_t>(wire[0]) | (static_cast<std::uint16_t>(wire[1]) << 8U));
    if (!valid_message_type(type))
    {
        error = LegacySteamCodecError::invalid_message_type;
        return false;
    }
    LegacySteamWirePacket decoded;
    decoded.message_type = type;
    decoded.channel = wire[2];
    decoded.payload.assign(wire.begin() + legacy_steam_wire_header_size, wire.end());
    packet = std::move(decoded);
    error = LegacySteamCodecError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_legacy_connection_request(
    const LegacyConnectionRequest &request, LegacySteamCodecError &error)
{
    error = LegacySteamCodecError::none;
    if (request.game_name.size() > maximum_game_name_bytes ||
        (request.password.has_value() && request.password->size() > maximum_password_bytes))
    {
        error = LegacySteamCodecError::out_of_range;
        return std::nullopt;
    }
    LidgrenBitWriter writer;
    const bool has_password = request.password.has_value() && !request.password->empty();
    if (!writer.write_utf8(request.game_name) || !writer.write_i32(request.network_version) ||
        !writer.write_i32(request.session_id) || !writer.write_i32(has_password ? 1 : 0) ||
        (has_password && !writer.write_utf8(*request.password)) ||
        !write_gamer(writer, request.gamer, error) ||
        !write_properties(writer, request.session_properties))
    {
        if (error == LegacySteamCodecError::none)
            error = LegacySteamCodecError::too_large;
        return std::nullopt;
    }
    error = LegacySteamCodecError::none;
    return writer.take();
}

bool decode_legacy_connection_request(std::span<const std::uint8_t> payload,
                                      LegacyConnectionRequest &request,
                                      LegacySteamCodecError &error)
{
    LidgrenBitReader reader{payload};
    LegacyConnectionRequest decoded;
    std::int32_t password_flag{};
    if (!reader.read_string(maximum_game_name_bytes, decoded.game_name) ||
        !reader.read_i32(decoded.network_version) || !reader.read_i32(decoded.session_id) ||
        !reader.read_i32(password_flag) || password_flag < 0 || password_flag > 1)
    {
        error = LegacySteamCodecError::malformed;
        return false;
    }
    if (password_flag == 1)
    {
        std::string password;
        if (!reader.read_string(maximum_password_bytes, password))
        {
            error = LegacySteamCodecError::malformed;
            return false;
        }
        decoded.password = std::move(password);
    }
    if (!read_gamer(reader, decoded.gamer) || !read_properties(reader, decoded.session_properties))
    {
        error = LegacySteamCodecError::malformed;
        return false;
    }
    if (!reader.exhausted())
    {
        error = LegacySteamCodecError::trailing_data;
        return false;
    }
    request = std::move(decoded);
    error = LegacySteamCodecError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_legacy_connected_response(
    const LegacyConnectedResponse &response, LegacySteamCodecError &error)
{
    error = LegacySteamCodecError::none;
    if (response.peers.size() != response.peer_gids.size() ||
        response.peers.size() > maximum_legacy_peers)
    {
        error = LegacySteamCodecError::out_of_range;
        return std::nullopt;
    }
    LidgrenBitWriter writer;
    if (!writer.write_u8(
            static_cast<std::uint8_t>(InternalSessionMessage::response_to_connection)) ||
        !writer.write_u8(response.player_gid) ||
        !writer.write_i32(static_cast<std::int32_t>(response.peers.size())))
    {
        error = LegacySteamCodecError::too_large;
        return std::nullopt;
    }
    for (const LegacyGamerIdentity &gamer : response.peers)
    {
        if (!write_gamer(writer, gamer, error))
            return std::nullopt;
    }
    if (!writer.write_byte_array(response.peer_gids))
    {
        error = LegacySteamCodecError::too_large;
        return std::nullopt;
    }
    error = LegacySteamCodecError::none;
    return writer.take();
}

bool decode_legacy_connected_response(std::span<const std::uint8_t> payload,
                                      LegacyConnectedResponse &response,
                                      LegacySteamCodecError &error)
{
    LidgrenBitReader reader{payload};
    LegacyConnectedResponse decoded;
    std::uint8_t marker{};
    std::int32_t peer_count{};
    if (!reader.read_u8(marker) ||
        marker != static_cast<std::uint8_t>(InternalSessionMessage::response_to_connection) ||
        !reader.read_u8(decoded.player_gid) || !reader.read_i32(peer_count) || peer_count < 0 ||
        peer_count > static_cast<std::int32_t>(maximum_legacy_peers))
    {
        error = LegacySteamCodecError::malformed;
        return false;
    }
    decoded.peers.resize(static_cast<std::size_t>(peer_count));
    for (LegacyGamerIdentity &gamer : decoded.peers)
    {
        if (!read_gamer(reader, gamer))
        {
            error = LegacySteamCodecError::malformed;
            return false;
        }
    }
    if (!reader.read_byte_array(maximum_legacy_peers, decoded.peer_gids) ||
        decoded.peer_gids.size() != decoded.peers.size())
    {
        error = LegacySteamCodecError::length_mismatch;
        return false;
    }
    if (!reader.exhausted())
    {
        error = LegacySteamCodecError::trailing_data;
        return false;
    }
    response = std::move(decoded);
    error = LegacySteamCodecError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_legacy_new_peer(std::uint8_t gid,
                                                                const LegacyGamerIdentity &gamer,
                                                                LegacySteamCodecError &error)
{
    error = LegacySteamCodecError::none;
    LidgrenBitWriter writer;
    if (!writer.write_u8(static_cast<std::uint8_t>(InternalSessionMessage::new_peer)) ||
        !writer.write_u8(gid) || !write_gamer(writer, gamer, error))
    {
        if (error == LegacySteamCodecError::none)
            error = LegacySteamCodecError::too_large;
        return std::nullopt;
    }
    error = LegacySteamCodecError::none;
    return writer.take();
}

bool decode_legacy_new_peer(std::span<const std::uint8_t> payload, std::uint8_t &gid,
                            LegacyGamerIdentity &gamer, LegacySteamCodecError &error)
{
    LidgrenBitReader reader{payload};
    std::uint8_t marker{};
    LegacyGamerIdentity decoded;
    std::uint8_t decoded_gid{};
    if (!reader.read_u8(marker) ||
        marker != static_cast<std::uint8_t>(InternalSessionMessage::new_peer) ||
        !reader.read_u8(decoded_gid) || !read_gamer(reader, decoded))
    {
        error = LegacySteamCodecError::malformed;
        return false;
    }
    if (!reader.exhausted())
    {
        error = LegacySteamCodecError::trailing_data;
        return false;
    }
    gid = decoded_gid;
    gamer = std::move(decoded);
    error = LegacySteamCodecError::none;
    return true;
}

std::vector<std::uint8_t> encode_legacy_drop_peer(std::uint8_t gid)
{
    return {static_cast<std::uint8_t>(InternalSessionMessage::drop_peer), gid};
}

bool decode_legacy_drop_peer(std::span<const std::uint8_t> payload, std::uint8_t &gid,
                             LegacySteamCodecError &error) noexcept
{
    if (payload.size() != 2U ||
        payload[0] != static_cast<std::uint8_t>(InternalSessionMessage::drop_peer))
    {
        error = LegacySteamCodecError::malformed;
        return false;
    }
    gid = payload[1];
    error = LegacySteamCodecError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_legacy_session_properties_changed(
    const SessionProperties &properties, LegacySteamCodecError &error)
{
    error = LegacySteamCodecError::none;
    LidgrenBitWriter writer;
    if (!writer.write_u8(
            static_cast<std::uint8_t>(InternalSessionMessage::session_properties_changed)) ||
        !write_properties(writer, properties))
    {
        error = LegacySteamCodecError::too_large;
        return std::nullopt;
    }
    error = LegacySteamCodecError::none;
    return writer.take();
}

bool decode_legacy_session_properties_changed(std::span<const std::uint8_t> payload,
                                              SessionProperties &properties,
                                              LegacySteamCodecError &error)
{
    LidgrenBitReader reader{payload};
    std::uint8_t marker{};
    SessionProperties decoded{};
    if (!reader.read_u8(marker) ||
        marker != static_cast<std::uint8_t>(InternalSessionMessage::session_properties_changed) ||
        !read_properties(reader, decoded))
    {
        error = LegacySteamCodecError::malformed;
        return false;
    }
    if (!reader.exhausted())
    {
        error = LegacySteamCodecError::trailing_data;
        return false;
    }
    properties = decoded;
    error = LegacySteamCodecError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_legacy_direct_frame(
    const LegacyDirectDataFrame &frame, LegacySteamCodecError &error)
{
    error = LegacySteamCodecError::none;
    if (!validate_application_packet(frame.application_packet, error))
        return std::nullopt;
    std::vector<std::uint8_t> encoded;
    encoded.reserve(2U + sizeof(std::int32_t) + frame.application_packet.size());
    encoded.push_back(frame.recipient_gid);
    encoded.push_back(frame.sender_gid);
    append_i32(encoded, static_cast<std::int32_t>(frame.application_packet.size()));
    encoded.insert(encoded.end(), frame.application_packet.begin(), frame.application_packet.end());
    error = LegacySteamCodecError::none;
    return encoded;
}

bool decode_legacy_direct_frame(std::span<const std::uint8_t> encoded, LegacyDirectDataFrame &frame,
                                LegacySteamCodecError &error)
{
    if (encoded.size() < 2U + sizeof(std::int32_t))
    {
        error = LegacySteamCodecError::too_small;
        return false;
    }
    LegacyDirectDataFrame decoded;
    decoded.recipient_gid = encoded[0];
    decoded.sender_gid = encoded[1];
    if (!read_application_packet(encoded, 2U, decoded.application_packet, error))
        return false;
    frame = std::move(decoded);
    error = LegacySteamCodecError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_legacy_client_broadcast(
    std::uint8_t sender_gid, SteamDeliveryMethod delivery,
    std::span<const std::uint8_t> application_packet, LegacySteamCodecError &error)
{
    error = LegacySteamCodecError::none;
    if (!valid_delivery(delivery))
    {
        error = LegacySteamCodecError::invalid_delivery;
        return std::nullopt;
    }
    if (!validate_application_packet(application_packet, error))
        return std::nullopt;
    std::vector<std::uint8_t> encoded;
    encoded.reserve(3U + sizeof(std::int32_t) + application_packet.size());
    encoded.push_back(static_cast<std::uint8_t>(LegacyClientRouteKind::broadcast));
    encoded.push_back(static_cast<std::uint8_t>(delivery));
    encoded.push_back(sender_gid);
    append_i32(encoded, static_cast<std::int32_t>(application_packet.size()));
    encoded.insert(encoded.end(), application_packet.begin(), application_packet.end());
    error = LegacySteamCodecError::none;
    return encoded;
}

std::optional<std::vector<std::uint8_t>> encode_legacy_client_proxy_direct(
    std::uint8_t recipient_gid, std::uint8_t sender_gid, SteamDeliveryMethod delivery,
    std::span<const std::uint8_t> application_packet, LegacySteamCodecError &error)
{
    error = LegacySteamCodecError::none;
    if (!valid_delivery(delivery))
    {
        error = LegacySteamCodecError::invalid_delivery;
        return std::nullopt;
    }
    if (!validate_application_packet(application_packet, error))
        return std::nullopt;
    std::vector<std::uint8_t> encoded;
    encoded.reserve(4U + sizeof(std::int32_t) + application_packet.size());
    encoded.push_back(static_cast<std::uint8_t>(LegacyClientRouteKind::proxy_direct));
    encoded.push_back(recipient_gid);
    encoded.push_back(static_cast<std::uint8_t>(delivery));
    encoded.push_back(sender_gid);
    append_i32(encoded, static_cast<std::int32_t>(application_packet.size()));
    encoded.insert(encoded.end(), application_packet.begin(), application_packet.end());
    error = LegacySteamCodecError::none;
    return encoded;
}

bool decode_legacy_client_route(std::span<const std::uint8_t> encoded,
                                LegacyClientRouteFrame &frame, LegacySteamCodecError &error)
{
    if (encoded.empty())
    {
        error = LegacySteamCodecError::too_small;
        return false;
    }
    LegacyClientRouteFrame decoded;
    decoded.kind = static_cast<LegacyClientRouteKind>(encoded[0]);
    std::size_t length_offset{};
    if (decoded.kind == LegacyClientRouteKind::broadcast)
    {
        if (encoded.size() < 3U + sizeof(std::int32_t))
        {
            error = LegacySteamCodecError::too_small;
            return false;
        }
        decoded.delivery = static_cast<SteamDeliveryMethod>(encoded[1]);
        decoded.sender_gid = encoded[2];
        length_offset = 3U;
    }
    else if (decoded.kind == LegacyClientRouteKind::proxy_direct)
    {
        if (encoded.size() < 4U + sizeof(std::int32_t))
        {
            error = LegacySteamCodecError::too_small;
            return false;
        }
        decoded.recipient_gid = encoded[1];
        decoded.delivery = static_cast<SteamDeliveryMethod>(encoded[2]);
        decoded.sender_gid = encoded[3];
        length_offset = 4U;
    }
    else
    {
        error = LegacySteamCodecError::malformed;
        return false;
    }
    if (!valid_delivery(decoded.delivery))
    {
        error = LegacySteamCodecError::invalid_delivery;
        return false;
    }
    if (!read_application_packet(encoded, length_offset, decoded.application_packet, error))
        return false;
    frame = std::move(decoded);
    error = LegacySteamCodecError::none;
    return true;
}

} // namespace openminerz::net
