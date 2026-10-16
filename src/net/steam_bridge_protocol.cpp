#include "openminerz/net/steam_bridge_protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::array<std::uint8_t, 4> bridge_magic{'O', 'M', 'S', 'B'};
constexpr std::size_t maximum_bridge_name_bytes = 128U;
constexpr std::size_t maximum_bridge_persona_bytes = 128U;
constexpr std::size_t maximum_bridge_failure_bytes = 512U;
constexpr std::size_t maximum_bridge_date_bytes = 64U;

bool valid_message_type(SteamBridgeMessageType value) noexcept
{
    switch (value)
    {
    case SteamBridgeMessageType::hello:
    case SteamBridgeMessageType::start_host:
    case SteamBridgeMessageType::host_ready:
    case SteamBridgeMessageType::inbound_wire:
    case SteamBridgeMessageType::peer_left:
    case SteamBridgeMessageType::outbound_wire:
    case SteamBridgeMessageType::update_host:
    case SteamBridgeMessageType::stop:
    case SteamBridgeMessageType::stopped:
    case SteamBridgeMessageType::failure:
    case SteamBridgeMessageType::heartbeat:
    case SteamBridgeMessageType::peer_entered:
    case SteamBridgeMessageType::discover_request:
    case SteamBridgeMessageType::discovery_results:
    case SteamBridgeMessageType::join_request:
    case SteamBridgeMessageType::join_ready:
        return true;
    }
    return false;
}

bool valid_join_policy(SteamBridgeJoinPolicy value) noexcept
{
    return value == SteamBridgeJoinPolicy::anyone || value == SteamBridgeJoinPolicy::friends_only ||
           value == SteamBridgeJoinPolicy::invite_only;
}

bool valid_failure_code(SteamBridgeFailureCode value) noexcept
{
    return value >= SteamBridgeFailureCode::invalid_config &&
           value <= SteamBridgeFailureCode::internal_error;
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

bool printable(std::string_view value) noexcept
{
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
    });
}

class Writer final
{
  public:
    explicit Writer(std::size_t maximum = steam_bridge_maximum_payload_size) : maximum_{maximum}
    {
    }

    bool u8(std::uint8_t value)
    {
        if (!reserve(1U))
            return false;
        bytes_.push_back(value);
        return true;
    }
    bool boolean(bool value)
    {
        return u8(value ? 1U : 0U);
    }
    bool u16(std::uint16_t value)
    {
        return u8(static_cast<std::uint8_t>(value)) && u8(static_cast<std::uint8_t>(value >> 8U));
    }
    bool u32(std::uint32_t value)
    {
        return u16(static_cast<std::uint16_t>(value)) &&
               u16(static_cast<std::uint16_t>(value >> 16U));
    }
    bool i32(std::int32_t value)
    {
        return u32(std::bit_cast<std::uint32_t>(value));
    }
    bool u64(std::uint64_t value)
    {
        return u32(static_cast<std::uint32_t>(value)) &&
               u32(static_cast<std::uint32_t>(value >> 32U));
    }
    bool bytes(std::span<const std::uint8_t> value)
    {
        if (!reserve(value.size()))
            return false;
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return true;
    }
    bool sized_bytes(std::span<const std::uint8_t> value)
    {
        return value.size() <= std::numeric_limits<std::uint32_t>::max() &&
               u32(static_cast<std::uint32_t>(value.size())) && bytes(value);
    }
    bool string(std::string_view value, std::size_t maximum_bytes)
    {
        return value.size() <= maximum_bytes &&
               value.size() <= std::numeric_limits<std::uint16_t>::max() &&
               u16(static_cast<std::uint16_t>(value.size())) &&
               bytes({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
    }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept
    {
        return std::move(bytes_);
    }

  private:
    bool reserve(std::size_t count) const noexcept
    {
        return count <= maximum_ - std::min(maximum_, bytes_.size());
    }

    std::size_t maximum_{};
    std::vector<std::uint8_t> bytes_;
};

class Reader final
{
  public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes}
    {
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return bytes_.size() - offset_;
    }
    [[nodiscard]] bool exhausted() const noexcept
    {
        return offset_ == bytes_.size();
    }
    bool u8(std::uint8_t &value) noexcept
    {
        if (remaining() < 1U)
            return false;
        value = bytes_[offset_++];
        return true;
    }
    bool boolean(bool &value) noexcept
    {
        std::uint8_t encoded{};
        if (!u8(encoded) || encoded > 1U)
            return false;
        value = encoded != 0U;
        return true;
    }
    bool u16(std::uint16_t &value) noexcept
    {
        std::uint8_t low{};
        std::uint8_t high{};
        if (!u8(low) || !u8(high))
            return false;
        value = static_cast<std::uint16_t>(low) | (static_cast<std::uint16_t>(high) << 8U);
        return true;
    }
    bool u32(std::uint32_t &value) noexcept
    {
        std::uint16_t low{};
        std::uint16_t high{};
        if (!u16(low) || !u16(high))
            return false;
        value = static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 16U);
        return true;
    }
    bool i32(std::int32_t &value) noexcept
    {
        std::uint32_t encoded{};
        if (!u32(encoded))
            return false;
        value = std::bit_cast<std::int32_t>(encoded);
        return true;
    }
    bool u64(std::uint64_t &value) noexcept
    {
        std::uint32_t low{};
        std::uint32_t high{};
        if (!u32(low) || !u32(high))
            return false;
        value = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32U);
        return true;
    }
    bool bytes(std::size_t count, std::vector<std::uint8_t> &value)
    {
        if (count > remaining())
            return false;
        value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                     bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += count;
        return true;
    }
    bool sized_bytes(std::size_t maximum, std::vector<std::uint8_t> &value)
    {
        std::uint32_t count{};
        return u32(count) && count <= maximum && bytes(count, value);
    }
    bool string(std::size_t maximum, std::string &value)
    {
        std::uint16_t count{};
        std::vector<std::uint8_t> decoded;
        if (!u16(count) || count > maximum || !bytes(count, decoded))
            return false;
        value.assign(decoded.begin(), decoded.end());
        return true;
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

void write_u16_at(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32_at(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value)
{
    write_u16_at(bytes, offset, static_cast<std::uint16_t>(value));
    write_u16_at(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

bool read_u16_at(std::span<const std::uint8_t> bytes, std::size_t offset,
                 std::uint16_t &value) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < 2U)
        return false;
    value = static_cast<std::uint16_t>(bytes[offset]) |
            (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
    return true;
}

bool read_u32_at(std::span<const std::uint8_t> bytes, std::size_t offset,
                 std::uint32_t &value) noexcept
{
    std::uint16_t low{};
    std::uint16_t high{};
    if (!read_u16_at(bytes, offset, low) || !read_u16_at(bytes, offset + 2U, high))
        return false;
    value = static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 16U);
    return true;
}

bool valid_session_properties(const SessionProperties &properties) noexcept
{
    if (!properties[0].has_value() ||
        (*properties[0] != static_cast<std::int32_t>(ProtocolVersion::classic_v3) &&
         *properties[0] != static_cast<std::int32_t>(ProtocolVersion::modern_v4)) ||
        !properties[1].has_value() || (*properties[1] != 0 && *properties[1] != 1) ||
        !properties[2].has_value() || !properties[3].has_value() || !properties[4].has_value() ||
        (*properties[4] != 0 && *properties[4] != 1) || !properties[5].has_value() ||
        *properties[5] < 0 || *properties[5] > 2 || properties[6].has_value() ||
        properties[7].has_value())
        return false;
    const std::int32_t maximum_mode =
        *properties[0] == static_cast<std::int32_t>(ProtocolVersion::modern_v4) ? 5 : 3;
    return *properties[2] >= 0 && *properties[2] <= maximum_mode && *properties[3] >= 0 &&
           *properties[3] <= 3;
}

bool write_properties(Writer &writer, const SessionProperties &properties)
{
    for (const std::optional<std::int32_t> property : properties)
    {
        if (!writer.boolean(property.has_value()) ||
            (property.has_value() && !writer.i32(*property)))
            return false;
    }
    return true;
}

bool read_properties(Reader &reader, SessionProperties &properties)
{
    SessionProperties decoded{};
    for (std::optional<std::int32_t> &property : decoded)
    {
        bool present{};
        if (!reader.boolean(present))
            return false;
        if (present)
        {
            std::int32_t value{};
            if (!reader.i32(value))
                return false;
            property = value;
        }
    }
    if (!valid_session_properties(decoded))
        return false;
    properties = decoded;
    return true;
}

bool read_unvalidated_properties(Reader &reader, SessionProperties &properties)
{
    SessionProperties decoded{};
    for (std::optional<std::int32_t> &property : decoded)
    {
        bool present{};
        if (!reader.boolean(present))
            return false;
        if (present)
        {
            std::int32_t value{};
            if (!reader.i32(value))
                return false;
            property = value;
        }
    }
    properties = decoded;
    return true;
}

bool valid_discovered_lobby(const SteamBridgeDiscoveredLobby &lobby,
                            bool require_valid_properties) noexcept
{
    return lobby.lobby_id != 0U && (!require_valid_properties || lobby.host_peer != 0U) &&
           lobby.current_players <= 16U && lobby.maximum_players >= 1U &&
           lobby.maximum_players <= 16U && lobby.current_players <= lobby.maximum_players &&
           valid_join_policy(lobby.join_policy) && lobby.friend_count <= 16U &&
           !lobby.name.empty() && lobby.name.size() <= maximum_bridge_name_bytes &&
           printable(lobby.name) && !lobby.host_name.empty() &&
           lobby.host_name.size() <= maximum_bridge_persona_bytes && printable(lobby.host_name) &&
           !lobby.created_at.empty() && lobby.created_at.size() <= maximum_bridge_date_bytes &&
           printable(lobby.created_at) &&
           lobby.extension_metadata.size() <= steam_bridge_maximum_extension_metadata_size &&
           (!require_valid_properties || valid_session_properties(lobby.session_properties));
}

bool write_discovered_lobby(Writer &writer, const SteamBridgeDiscoveredLobby &lobby)
{
    return writer.u64(lobby.lobby_id) && writer.u64(lobby.host_peer) &&
           writer.u8(lobby.current_players) && writer.u8(lobby.maximum_players) &&
           writer.u8(static_cast<std::uint8_t>(lobby.join_policy)) &&
           writer.boolean(lobby.password_protected) && writer.u8(lobby.friend_count) &&
           writer.boolean(lobby.host_is_friend) && writer.u16(0U) &&
           writer.string(lobby.name, maximum_bridge_name_bytes) &&
           writer.string(lobby.host_name, maximum_bridge_persona_bytes) &&
           writer.string(lobby.created_at, maximum_bridge_date_bytes) &&
           write_properties(writer, lobby.session_properties) &&
           writer.sized_bytes(lobby.extension_metadata);
}

bool read_discovered_lobby(Reader &reader, SteamBridgeDiscoveredLobby &lobby)
{
    SteamBridgeDiscoveredLobby decoded;
    std::uint8_t raw_policy{};
    std::uint16_t reserved16{};
    if (!reader.u64(decoded.lobby_id) || !reader.u64(decoded.host_peer) ||
        !reader.u8(decoded.current_players) || !reader.u8(decoded.maximum_players) ||
        !reader.u8(raw_policy) || !reader.boolean(decoded.password_protected) ||
        !reader.u8(decoded.friend_count) || !reader.boolean(decoded.host_is_friend) ||
        !reader.u16(reserved16) || reserved16 != 0U ||
        !reader.string(maximum_bridge_name_bytes, decoded.name) ||
        !reader.string(maximum_bridge_persona_bytes, decoded.host_name) ||
        !reader.string(maximum_bridge_date_bytes, decoded.created_at) ||
        !read_unvalidated_properties(reader, decoded.session_properties) ||
        !reader.sized_bytes(steam_bridge_maximum_extension_metadata_size,
                            decoded.extension_metadata))
        return false;
    decoded.join_policy = static_cast<SteamBridgeJoinPolicy>(raw_policy);
    if (!valid_discovered_lobby(decoded, false))
        return false;
    lobby = std::move(decoded);
    return true;
}

} // namespace

std::uint32_t steam_bridge_crc32(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t value : bytes)
    {
        crc ^= value;
        for (std::size_t bit{}; bit < 8U; ++bit)
            crc = (crc >> 1U) ^
                  (0xEDB88320U & static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U)));
    }
    return ~crc;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_frame(
    SteamBridgeMessageType type, std::uint32_t sequence, std::span<const std::uint8_t> payload,
    SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (!valid_message_type(type))
    {
        error = SteamBridgeProtocolError::unknown_message_type;
        return std::nullopt;
    }
    if (payload.size() > steam_bridge_maximum_payload_size)
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    std::vector<std::uint8_t> encoded(steam_bridge_header_size + payload.size(), 0U);
    std::copy(bridge_magic.begin(), bridge_magic.end(), encoded.begin());
    write_u16_at(encoded, 4U, steam_bridge_protocol_version);
    write_u16_at(encoded, 6U, static_cast<std::uint16_t>(type));
    write_u32_at(encoded, 8U, static_cast<std::uint32_t>(payload.size()));
    write_u32_at(encoded, 12U, sequence);
    std::copy(payload.begin(), payload.end(), encoded.begin() + steam_bridge_header_size);
    std::vector<std::uint8_t> checksum_input;
    checksum_input.reserve(16U + payload.size());
    checksum_input.insert(checksum_input.end(), encoded.begin(), encoded.begin() + 16U);
    checksum_input.insert(checksum_input.end(), payload.begin(), payload.end());
    write_u32_at(encoded, 16U, steam_bridge_crc32(checksum_input));
    return encoded;
}

bool decode_steam_bridge_frame(std::span<const std::uint8_t> encoded, SteamBridgeFrame &frame,
                               SteamBridgeProtocolError &error)
{
    if (encoded.size() < steam_bridge_header_size)
    {
        error = SteamBridgeProtocolError::too_small;
        return false;
    }
    if (encoded.size() > steam_bridge_maximum_frame_size)
    {
        error = SteamBridgeProtocolError::too_large;
        return false;
    }
    if (!std::equal(bridge_magic.begin(), bridge_magic.end(), encoded.begin()))
    {
        error = SteamBridgeProtocolError::wrong_magic;
        return false;
    }
    std::uint16_t version{};
    std::uint16_t raw_type{};
    std::uint32_t payload_length{};
    std::uint32_t sequence{};
    std::uint32_t expected_crc{};
    if (!read_u16_at(encoded, 4U, version) || !read_u16_at(encoded, 6U, raw_type) ||
        !read_u32_at(encoded, 8U, payload_length) || !read_u32_at(encoded, 12U, sequence) ||
        !read_u32_at(encoded, 16U, expected_crc))
    {
        error = SteamBridgeProtocolError::too_small;
        return false;
    }
    if (version != steam_bridge_protocol_version)
    {
        error = SteamBridgeProtocolError::unsupported_version;
        return false;
    }
    const auto type = static_cast<SteamBridgeMessageType>(raw_type);
    if (!valid_message_type(type))
    {
        error = SteamBridgeProtocolError::unknown_message_type;
        return false;
    }
    if (payload_length > steam_bridge_maximum_payload_size ||
        payload_length != encoded.size() - steam_bridge_header_size)
    {
        error = SteamBridgeProtocolError::length_mismatch;
        return false;
    }
    std::vector<std::uint8_t> checksum_input;
    checksum_input.reserve(16U + payload_length);
    checksum_input.insert(checksum_input.end(), encoded.begin(), encoded.begin() + 16U);
    checksum_input.insert(checksum_input.end(), encoded.begin() + steam_bridge_header_size,
                          encoded.end());
    if (steam_bridge_crc32(checksum_input) != expected_crc)
    {
        error = SteamBridgeProtocolError::checksum_mismatch;
        return false;
    }
    SteamBridgeFrame decoded;
    decoded.type = type;
    decoded.sequence = sequence;
    decoded.payload.assign(encoded.begin() + steam_bridge_header_size, encoded.end());
    frame = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_host_config(
    const SteamBridgeHostConfig &config, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (config.app_id != castle_miner_z_steam_app_id || config.maximum_players < 1U ||
        config.maximum_players > 16U || !valid_join_policy(config.join_policy) ||
        config.server_name.empty() || config.server_name.size() > maximum_bridge_name_bytes ||
        !printable(config.server_name) || !valid_session_properties(config.session_properties) ||
        config.extension_metadata.size() > steam_bridge_maximum_extension_metadata_size)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer;
    if (!writer.u32(config.app_id) || !writer.u8(config.maximum_players) ||
        !writer.u8(static_cast<std::uint8_t>(config.join_policy)) ||
        !writer.boolean(config.password_protected) || !writer.u8(0U) ||
        !writer.string(config.server_name, maximum_bridge_name_bytes) ||
        !write_properties(writer, config.session_properties) ||
        !writer.sized_bytes(config.extension_metadata))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_host_config(std::span<const std::uint8_t> payload,
                                     SteamBridgeHostConfig &config, SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeHostConfig decoded;
    std::uint8_t raw_policy{};
    std::uint8_t reserved{};
    if (!reader.u32(decoded.app_id) || !reader.u8(decoded.maximum_players) ||
        !reader.u8(raw_policy) || !reader.boolean(decoded.password_protected) ||
        !reader.u8(reserved) || reserved != 0U ||
        !reader.string(maximum_bridge_name_bytes, decoded.server_name) ||
        !read_properties(reader, decoded.session_properties) ||
        !reader.sized_bytes(steam_bridge_maximum_extension_metadata_size,
                            decoded.extension_metadata))
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    decoded.join_policy = static_cast<SteamBridgeJoinPolicy>(raw_policy);
    if (decoded.app_id != castle_miner_z_steam_app_id || decoded.maximum_players < 1U ||
        decoded.maximum_players > 16U || !valid_join_policy(decoded.join_policy) ||
        decoded.server_name.empty() || !printable(decoded.server_name))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    if (!reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    config = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_host_ready(
    const SteamBridgeHostReady &ready, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (ready.local_peer == 0U || ready.lobby_id == 0U || ready.persona_name.empty() ||
        ready.persona_name.size() > maximum_bridge_persona_bytes || !printable(ready.persona_name))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer;
    if (!writer.u64(ready.local_peer) || !writer.u64(ready.lobby_id) ||
        !writer.string(ready.persona_name, maximum_bridge_persona_bytes))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_host_ready(std::span<const std::uint8_t> payload,
                                    SteamBridgeHostReady &ready, SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeHostReady decoded;
    if (!reader.u64(decoded.local_peer) || !reader.u64(decoded.lobby_id) ||
        !reader.string(maximum_bridge_persona_bytes, decoded.persona_name))
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    if (decoded.local_peer == 0U || decoded.lobby_id == 0U || decoded.persona_name.empty() ||
        !printable(decoded.persona_name))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    if (!reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    ready = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_inbound_wire(
    const SteamBridgeInboundWire &datagram, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (datagram.peer == 0U || datagram.p2p_channel > 1U || datagram.wire.empty() ||
        datagram.wire.size() > legacy_steam_maximum_wire_size)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer;
    if (!writer.u64(datagram.peer) || !writer.u8(datagram.p2p_channel) || !writer.u8(0U) ||
        !writer.u16(0U) || !writer.sized_bytes(datagram.wire))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_inbound_wire(std::span<const std::uint8_t> payload,
                                      SteamBridgeInboundWire &datagram,
                                      SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeInboundWire decoded;
    std::uint8_t reserved8{};
    std::uint16_t reserved16{};
    if (!reader.u64(decoded.peer) || !reader.u8(decoded.p2p_channel) || !reader.u8(reserved8) ||
        reserved8 != 0U || !reader.u16(reserved16) || reserved16 != 0U ||
        !reader.sized_bytes(legacy_steam_maximum_wire_size, decoded.wire))
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    if (decoded.peer == 0U || decoded.p2p_channel > 1U || decoded.wire.empty())
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    if (!reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    datagram = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_outbound_wire(
    const SteamBridgeOutboundWire &datagram, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (datagram.peer == 0U || !valid_delivery(datagram.delivery) || datagram.p2p_channel > 1U ||
        datagram.wire.empty() || datagram.wire.size() > legacy_steam_maximum_wire_size)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer;
    if (!writer.u64(datagram.peer) || !writer.u8(static_cast<std::uint8_t>(datagram.delivery)) ||
        !writer.u8(datagram.p2p_channel) || !writer.u16(0U) || !writer.sized_bytes(datagram.wire))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_outbound_wire(std::span<const std::uint8_t> payload,
                                       SteamBridgeOutboundWire &datagram,
                                       SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeOutboundWire decoded;
    std::uint8_t raw_delivery{};
    std::uint16_t reserved{};
    if (!reader.u64(decoded.peer) || !reader.u8(raw_delivery) || !reader.u8(decoded.p2p_channel) ||
        !reader.u16(reserved) || reserved != 0U ||
        !reader.sized_bytes(legacy_steam_maximum_wire_size, decoded.wire))
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    decoded.delivery = static_cast<SteamDeliveryMethod>(raw_delivery);
    if (decoded.peer == 0U || !valid_delivery(decoded.delivery) || decoded.p2p_channel > 1U ||
        decoded.wire.empty())
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    if (!reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    datagram = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_peer(PeerId peer,
                                                                  SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (peer == 0U)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer{8U};
    static_cast<void>(writer.u64(peer));
    return writer.take();
}

bool decode_steam_bridge_peer(std::span<const std::uint8_t> payload, PeerId &peer,
                              SteamBridgeProtocolError &error) noexcept
{
    Reader reader{payload};
    PeerId decoded{};
    if (!reader.u64(decoded) || !reader.exhausted())
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    if (decoded == 0U)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    peer = decoded;
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_member(
    const SteamBridgeMember &member, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (member.peer == 0U || member.persona_name.size() > maximum_bridge_persona_bytes ||
        !printable(member.persona_name))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer;
    if (!writer.u64(member.peer) ||
        !writer.string(member.persona_name, maximum_bridge_persona_bytes))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_member(std::span<const std::uint8_t> payload, SteamBridgeMember &member,
                                SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeMember decoded;
    if (!reader.u64(decoded.peer) ||
        !reader.string(maximum_bridge_persona_bytes, decoded.persona_name))
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    if (decoded.peer == 0U || !printable(decoded.persona_name))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    if (!reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    member = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_failure(
    const SteamBridgeFailure &failure, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (!valid_failure_code(failure.code) || failure.detail.empty() ||
        failure.detail.size() > maximum_bridge_failure_bytes || !printable(failure.detail))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer;
    if (!writer.u16(static_cast<std::uint16_t>(failure.code)) ||
        !writer.string(failure.detail, maximum_bridge_failure_bytes))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_failure(std::span<const std::uint8_t> payload, SteamBridgeFailure &failure,
                                 SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeFailure decoded;
    std::uint16_t raw_code{};
    if (!reader.u16(raw_code) || !reader.string(maximum_bridge_failure_bytes, decoded.detail))
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    decoded.code = static_cast<SteamBridgeFailureCode>(raw_code);
    if (!valid_failure_code(decoded.code) || decoded.detail.empty() || !printable(decoded.detail))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    if (!reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    failure = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_discovery_request(
    const SteamBridgeDiscoveryRequest &request, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if ((request.protocol.has_value() && *request.protocol != ProtocolVersion::classic_v3 &&
         *request.protocol != ProtocolVersion::modern_v4) ||
        request.maximum_results < 1U ||
        request.maximum_results > steam_bridge_maximum_discovery_results)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer{4U};
    if (!writer.u8(request.protocol.has_value() ? static_cast<std::uint8_t>(*request.protocol)
                                                : 0U) ||
        !writer.u8(request.maximum_results) || !writer.u16(0U))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_discovery_request(std::span<const std::uint8_t> payload,
                                           SteamBridgeDiscoveryRequest &request,
                                           SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    std::uint8_t raw_protocol{};
    std::uint8_t maximum_results{};
    std::uint16_t reserved{};
    if (!reader.u8(raw_protocol) || !reader.u8(maximum_results) || !reader.u16(reserved) ||
        reserved != 0U)
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    SteamBridgeDiscoveryRequest decoded;
    if (raw_protocol != 0U)
        decoded.protocol = static_cast<ProtocolVersion>(raw_protocol);
    decoded.maximum_results = maximum_results;
    if ((decoded.protocol.has_value() && *decoded.protocol != ProtocolVersion::classic_v3 &&
         *decoded.protocol != ProtocolVersion::modern_v4) ||
        decoded.maximum_results < 1U ||
        decoded.maximum_results > steam_bridge_maximum_discovery_results)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    if (!reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    request = decoded;
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_discovery_results(
    const SteamBridgeDiscoveryResults &results, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (results.local_peer == 0U || results.persona_name.empty() ||
        results.persona_name.size() > maximum_bridge_persona_bytes ||
        !printable(results.persona_name) ||
        results.lobbies.size() > steam_bridge_maximum_discovery_results ||
        results.reported_lobby_count > steam_bridge_maximum_discovery_results ||
        results.rejected_lobby_count > steam_bridge_maximum_discovery_results ||
        results.rejected_native_count > steam_bridge_maximum_discovery_results ||
        results.rejected_metadata_count > steam_bridge_maximum_discovery_results ||
        results.rejected_contract_count > steam_bridge_maximum_discovery_results ||
        results.diagnostic.size() > maximum_bridge_failure_bytes || !printable(results.diagnostic))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer;
    if (!writer.u64(results.local_peer) ||
        !writer.string(results.persona_name, maximum_bridge_persona_bytes) ||
        !writer.u16(static_cast<std::uint16_t>(results.lobbies.size())))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(results.lobbies.size());
    for (const SteamBridgeDiscoveredLobby &lobby : results.lobbies)
    {
        if (!valid_discovered_lobby(lobby, false) ||
            std::find(ids.begin(), ids.end(), lobby.lobby_id) != ids.end())
        {
            error = SteamBridgeProtocolError::out_of_range;
            return std::nullopt;
        }
        ids.push_back(lobby.lobby_id);
        if (!write_discovered_lobby(writer, lobby))
        {
            error = SteamBridgeProtocolError::too_large;
            return std::nullopt;
        }
    }
    if (!writer.u16(results.reported_lobby_count) || !writer.u16(results.rejected_lobby_count) ||
        !writer.u16(results.rejected_native_count) ||
        !writer.u16(results.rejected_metadata_count) ||
        !writer.u16(results.rejected_contract_count) ||
        !writer.string(results.diagnostic, maximum_bridge_failure_bytes))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_discovery_results(std::span<const std::uint8_t> payload,
                                           SteamBridgeDiscoveryResults &results,
                                           SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeDiscoveryResults decoded;
    std::uint16_t count{};
    if (!reader.u64(decoded.local_peer) || decoded.local_peer == 0U ||
        !reader.string(maximum_bridge_persona_bytes, decoded.persona_name) ||
        decoded.persona_name.empty() || !printable(decoded.persona_name) || !reader.u16(count) ||
        count > steam_bridge_maximum_discovery_results)
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    decoded.lobbies.reserve(count);
    for (std::uint16_t index{}; index < count; ++index)
    {
        SteamBridgeDiscoveredLobby lobby;
        if (!read_discovered_lobby(reader, lobby) ||
            std::any_of(decoded.lobbies.begin(), decoded.lobbies.end(),
                        [&](const auto &existing) { return existing.lobby_id == lobby.lobby_id; }))
        {
            error = SteamBridgeProtocolError::malformed_payload;
            return false;
        }
        decoded.lobbies.push_back(std::move(lobby));
    }
    if (!reader.u16(decoded.reported_lobby_count) || !reader.u16(decoded.rejected_lobby_count) ||
        !reader.u16(decoded.rejected_native_count) ||
        !reader.u16(decoded.rejected_metadata_count) ||
        !reader.u16(decoded.rejected_contract_count) ||
        !reader.string(maximum_bridge_failure_bytes, decoded.diagnostic) ||
        decoded.reported_lobby_count > steam_bridge_maximum_discovery_results ||
        decoded.rejected_lobby_count > steam_bridge_maximum_discovery_results ||
        decoded.rejected_native_count > steam_bridge_maximum_discovery_results ||
        decoded.rejected_metadata_count > steam_bridge_maximum_discovery_results ||
        decoded.rejected_contract_count > steam_bridge_maximum_discovery_results ||
        !printable(decoded.diagnostic) || !reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    results = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_join_request(
    const SteamBridgeJoinRequest &request, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (request.lobby_id == 0U)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer{8U};
    static_cast<void>(writer.u64(request.lobby_id));
    return writer.take();
}

bool decode_steam_bridge_join_request(std::span<const std::uint8_t> payload,
                                      SteamBridgeJoinRequest &request,
                                      SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeJoinRequest decoded;
    if (!reader.u64(decoded.lobby_id) || !reader.exhausted())
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    if (decoded.lobby_id == 0U)
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    request = decoded;
    error = SteamBridgeProtocolError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_steam_bridge_join_ready(
    const SteamBridgeJoinReady &ready, SteamBridgeProtocolError &error)
{
    error = SteamBridgeProtocolError::none;
    if (ready.local_peer == 0U || ready.persona_name.empty() ||
        ready.persona_name.size() > maximum_bridge_persona_bytes ||
        !printable(ready.persona_name) || !valid_discovered_lobby(ready.lobby, true))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return std::nullopt;
    }
    Writer writer;
    if (!writer.u64(ready.local_peer) ||
        !writer.string(ready.persona_name, maximum_bridge_persona_bytes) ||
        !write_discovered_lobby(writer, ready.lobby))
    {
        error = SteamBridgeProtocolError::too_large;
        return std::nullopt;
    }
    return writer.take();
}

bool decode_steam_bridge_join_ready(std::span<const std::uint8_t> payload,
                                    SteamBridgeJoinReady &ready, SteamBridgeProtocolError &error)
{
    Reader reader{payload};
    SteamBridgeJoinReady decoded;
    if (!reader.u64(decoded.local_peer) ||
        !reader.string(maximum_bridge_persona_bytes, decoded.persona_name) ||
        !read_discovered_lobby(reader, decoded.lobby))
    {
        error = SteamBridgeProtocolError::malformed_payload;
        return false;
    }
    if (decoded.local_peer == 0U || decoded.persona_name.empty() ||
        !printable(decoded.persona_name) ||
        !valid_session_properties(decoded.lobby.session_properties))
    {
        error = SteamBridgeProtocolError::out_of_range;
        return false;
    }
    if (!reader.exhausted())
    {
        error = SteamBridgeProtocolError::trailing_data;
        return false;
    }
    ready = std::move(decoded);
    error = SteamBridgeProtocolError::none;
    return true;
}

} // namespace openminerz::net
