#pragma once

#include "openminerz/net/host_session.hpp"
#include "openminerz/net/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::net
{

// Values emitted by the deployed Lidgren Steam network provider. They are not
// SteamNetworking message types; the four-byte envelope is owned by the game.
enum class SteamIncomingMessageType : std::int16_t
{
    error = 0,
    status_changed = 1,
    unconnected_data = 2,
    connection_approval = 4,
    data = 8,
    receipt = 16,
    discovery_request = 32,
    discovery_response = 64,
    verbose_debug_message = 128,
    debug_message = 256,
    warning_message = 512,
    error_message = 1024,
    nat_introduction_success = 2048,
    connection_latency_updated = 4096
};

enum class SteamDeliveryMethod : std::uint8_t
{
    unknown = 0,
    unreliable = 1,
    unreliable_sequenced = 2,
    reliable_unordered = 34,
    reliable_sequenced = 35,
    reliable_ordered = 67
};

enum class LegacySteamCodecError : std::uint8_t
{
    none,
    too_small,
    too_large,
    invalid_message_type,
    invalid_delivery,
    malformed,
    out_of_range,
    length_mismatch,
    trailing_data
};

inline constexpr std::size_t legacy_steam_wire_header_size = 4U;
inline constexpr std::size_t legacy_steam_maximum_wire_size = maximum_packet_size + 64U;
inline constexpr std::string_view legacy_steam_game_name = "CastleMinerZSteam";
inline constexpr std::size_t legacy_session_property_count = 8U;

struct LegacySteamWirePacket final
{
    SteamIncomingMessageType message_type{SteamIncomingMessageType::error};
    std::uint8_t channel{};
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] bool uses_unreliable_native_send(SteamDeliveryMethod delivery) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_legacy_steam_wire(
    SteamIncomingMessageType message_type, std::uint8_t channel,
    std::span<const std::uint8_t> payload, LegacySteamCodecError &error);
[[nodiscard]] bool decode_legacy_steam_wire(std::span<const std::uint8_t> wire,
                                            LegacySteamWirePacket &packet,
                                            LegacySteamCodecError &error);

struct LegacyGamerIdentity final
{
    std::vector<std::uint8_t> player_id;
    std::string gamer_tag;

    bool operator==(const LegacyGamerIdentity &) const = default;
};

struct LegacyConnectionRequest final
{
    std::string game_name{legacy_steam_game_name};
    std::int32_t network_version{};
    std::int32_t session_id{};
    std::optional<std::string> password;
    LegacyGamerIdentity gamer;
    SessionProperties session_properties{};
};

struct LegacyConnectedResponse final
{
    std::uint8_t player_gid{};
    std::vector<LegacyGamerIdentity> peers;
    std::vector<std::uint8_t> peer_gids;
};

enum class InternalSessionMessage : std::uint8_t
{
    new_peer = 0,
    response_to_connection = 1,
    drop_peer = 2,
    forward_message = 3,
    broadcast_message = 4,
    session_properties_changed = 5
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_legacy_connection_request(
    const LegacyConnectionRequest &request, LegacySteamCodecError &error);
[[nodiscard]] bool decode_legacy_connection_request(std::span<const std::uint8_t> payload,
                                                    LegacyConnectionRequest &request,
                                                    LegacySteamCodecError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_legacy_connected_response(
    const LegacyConnectedResponse &response, LegacySteamCodecError &error);
[[nodiscard]] bool decode_legacy_connected_response(std::span<const std::uint8_t> payload,
                                                    LegacyConnectedResponse &response,
                                                    LegacySteamCodecError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_legacy_new_peer(
    std::uint8_t gid, const LegacyGamerIdentity &gamer, LegacySteamCodecError &error);
[[nodiscard]] bool decode_legacy_new_peer(std::span<const std::uint8_t> payload, std::uint8_t &gid,
                                          LegacyGamerIdentity &gamer, LegacySteamCodecError &error);
[[nodiscard]] std::vector<std::uint8_t> encode_legacy_drop_peer(std::uint8_t gid);
[[nodiscard]] bool decode_legacy_drop_peer(std::span<const std::uint8_t> payload, std::uint8_t &gid,
                                           LegacySteamCodecError &error) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_legacy_session_properties_changed(
    const SessionProperties &properties, LegacySteamCodecError &error);
[[nodiscard]] bool decode_legacy_session_properties_changed(std::span<const std::uint8_t> payload,
                                                            SessionProperties &properties,
                                                            LegacySteamCodecError &error);

enum class LegacyClientRouteKind : std::uint8_t
{
    proxy_direct = 3,
    broadcast = 4
};

struct LegacyDirectDataFrame final
{
    std::uint8_t recipient_gid{};
    std::uint8_t sender_gid{};
    std::vector<std::uint8_t> application_packet;
};

struct LegacyClientRouteFrame final
{
    LegacyClientRouteKind kind{LegacyClientRouteKind::broadcast};
    std::optional<std::uint8_t> recipient_gid;
    std::uint8_t sender_gid{};
    SteamDeliveryMethod delivery{SteamDeliveryMethod::unknown};
    std::vector<std::uint8_t> application_packet;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_legacy_direct_frame(
    const LegacyDirectDataFrame &frame, LegacySteamCodecError &error);
[[nodiscard]] bool decode_legacy_direct_frame(std::span<const std::uint8_t> encoded,
                                              LegacyDirectDataFrame &frame,
                                              LegacySteamCodecError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_legacy_client_broadcast(
    std::uint8_t sender_gid, SteamDeliveryMethod delivery,
    std::span<const std::uint8_t> application_packet, LegacySteamCodecError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_legacy_client_proxy_direct(
    std::uint8_t recipient_gid, std::uint8_t sender_gid, SteamDeliveryMethod delivery,
    std::span<const std::uint8_t> application_packet, LegacySteamCodecError &error);
[[nodiscard]] bool decode_legacy_client_route(std::span<const std::uint8_t> encoded,
                                              LegacyClientRouteFrame &frame,
                                              LegacySteamCodecError &error);

} // namespace openminerz::net
