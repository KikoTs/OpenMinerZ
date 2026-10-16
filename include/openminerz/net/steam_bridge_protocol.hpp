#pragma once

#include "openminerz/net/host_session.hpp"
#include "openminerz/net/legacy_steam_codec.hpp"
#include "openminerz/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openminerz::net
{

inline constexpr std::uint32_t castle_miner_z_steam_app_id = 253'430U;
inline constexpr std::uint16_t steam_bridge_protocol_version = 3U;
inline constexpr std::size_t steam_bridge_header_size = 20U;
inline constexpr std::size_t steam_bridge_maximum_payload_size = legacy_steam_maximum_wire_size + 1024U;
inline constexpr std::size_t steam_bridge_maximum_frame_size =
    steam_bridge_header_size + steam_bridge_maximum_payload_size;
inline constexpr std::size_t steam_bridge_maximum_discovery_results = 50U;
inline constexpr std::size_t steam_bridge_maximum_extension_metadata_size =
    maximum_extension_packet_size;

enum class SteamBridgeMessageType : std::uint16_t
{
    hello = 1,
    start_host = 2,
    host_ready = 3,
    inbound_wire = 4,
    peer_left = 5,
    outbound_wire = 6,
    update_host = 7,
    stop = 8,
    stopped = 9,
    failure = 10,
    heartbeat = 11,
    // Host mode only: a Steam user entered the hosted lobby. It carries no
    // authority; it lets the host report a join attempt that never progresses.
    peer_entered = 12,
    discover_request = 20,
    discovery_results = 21,
    join_request = 22,
    join_ready = 23
};

enum class SteamBridgeProtocolError : std::uint8_t
{
    none,
    too_small,
    too_large,
    wrong_magic,
    unsupported_version,
    unknown_message_type,
    length_mismatch,
    checksum_mismatch,
    malformed_payload,
    out_of_range,
    trailing_data
};

struct SteamBridgeFrame final
{
    SteamBridgeMessageType type{SteamBridgeMessageType::hello};
    std::uint32_t sequence{};
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::uint32_t steam_bridge_crc32(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_frame(
    SteamBridgeMessageType type, std::uint32_t sequence, std::span<const std::uint8_t> payload,
    SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_frame(std::span<const std::uint8_t> encoded,
                                             SteamBridgeFrame &frame,
                                             SteamBridgeProtocolError &error);

// The stock metadata JoinPolicy values used by the deployed lobby browser.
enum class SteamBridgeJoinPolicy : std::uint8_t
{
    anyone = 0,
    friends_only = 1,
    invite_only = 2
};

struct SteamBridgeHostConfig final
{
    std::uint32_t app_id{castle_miner_z_steam_app_id};
    std::uint8_t maximum_players{8};
    SteamBridgeJoinPolicy join_policy{SteamBridgeJoinPolicy::anyone};
    bool password_protected{};
    std::string server_name{"OPENMINERZ HOST"};
    SessionProperties session_properties{};
    std::vector<std::uint8_t> extension_metadata;
};

struct SteamBridgeHostReady final
{
    PeerId local_peer{};
    std::uint64_t lobby_id{};
    std::string persona_name;
};

struct SteamBridgeInboundWire final
{
    PeerId peer{};
    std::uint8_t p2p_channel{};
    std::vector<std::uint8_t> wire;
};

// A lobby member as Steam reports it. The persona name is display-only and may
// be empty while Steam is still resolving it.
struct SteamBridgeMember final
{
    PeerId peer{};
    std::string persona_name;
};

struct SteamBridgeOutboundWire final
{
    PeerId peer{};
    SteamDeliveryMethod delivery{SteamDeliveryMethod::reliable_ordered};
    std::uint8_t p2p_channel{};
    std::vector<std::uint8_t> wire;
};

enum class SteamBridgeFailureCode : std::uint16_t
{
    invalid_config = 1,
    steam_init_failed = 2,
    wrong_app_id = 3,
    not_logged_on = 4,
    lobby_create_failed = 5,
    lobby_metadata_failed = 6,
    native_send_failed = 7,
    protocol_error = 8,
    internal_error = 9
};

struct SteamBridgeFailure final
{
    SteamBridgeFailureCode code{SteamBridgeFailureCode::internal_error};
    std::string detail;
};

struct SteamBridgeDiscoveryRequest final
{
    std::optional<ProtocolVersion> protocol;
    std::uint8_t maximum_results{50U};
};

struct SteamBridgeDiscoveredLobby final
{
    std::uint64_t lobby_id{};
    PeerId host_peer{};
    std::uint8_t current_players{};
    std::uint8_t maximum_players{};
    SteamBridgeJoinPolicy join_policy{SteamBridgeJoinPolicy::anyone};
    bool password_protected{};
    std::uint8_t friend_count{};
    bool host_is_friend{};
    std::string name;
    std::string host_name;
    std::string created_at;
    SessionProperties session_properties{};
    std::vector<std::uint8_t> extension_metadata;
};

struct SteamBridgeDiscoveryResults final
{
    PeerId local_peer{};
    std::string persona_name;
    std::vector<SteamBridgeDiscoveredLobby> lobbies;
    std::uint16_t reported_lobby_count{};
    std::uint16_t rejected_lobby_count{};
    std::uint16_t rejected_native_count{};
    std::uint16_t rejected_metadata_count{};
    std::uint16_t rejected_contract_count{};
    std::string diagnostic;
};

struct SteamBridgeJoinRequest final
{
    std::uint64_t lobby_id{};
};

struct SteamBridgeJoinReady final
{
    PeerId local_peer{};
    std::string persona_name;
    SteamBridgeDiscoveredLobby lobby;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_host_config(
    const SteamBridgeHostConfig &config, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_host_config(std::span<const std::uint8_t> payload,
                                                   SteamBridgeHostConfig &config,
                                                   SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_host_ready(
    const SteamBridgeHostReady &ready, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_host_ready(std::span<const std::uint8_t> payload,
                                                  SteamBridgeHostReady &ready,
                                                  SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_inbound_wire(
    const SteamBridgeInboundWire &datagram, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_inbound_wire(std::span<const std::uint8_t> payload,
                                                    SteamBridgeInboundWire &datagram,
                                                    SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_outbound_wire(
    const SteamBridgeOutboundWire &datagram, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_outbound_wire(std::span<const std::uint8_t> payload,
                                                     SteamBridgeOutboundWire &datagram,
                                                     SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_peer(
    PeerId peer, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_peer(std::span<const std::uint8_t> payload, PeerId &peer,
                                            SteamBridgeProtocolError &error) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_member(
    const SteamBridgeMember &member, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_member(std::span<const std::uint8_t> payload,
                                              SteamBridgeMember &member,
                                              SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_failure(
    const SteamBridgeFailure &failure, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_failure(std::span<const std::uint8_t> payload,
                                               SteamBridgeFailure &failure,
                                               SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_discovery_request(
    const SteamBridgeDiscoveryRequest &request, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_discovery_request(std::span<const std::uint8_t> payload,
                                                         SteamBridgeDiscoveryRequest &request,
                                                         SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_discovery_results(
    const SteamBridgeDiscoveryResults &results, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_discovery_results(std::span<const std::uint8_t> payload,
                                                         SteamBridgeDiscoveryResults &results,
                                                         SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_join_request(
    const SteamBridgeJoinRequest &request, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_join_request(std::span<const std::uint8_t> payload,
                                                    SteamBridgeJoinRequest &request,
                                                    SteamBridgeProtocolError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_steam_bridge_join_ready(
    const SteamBridgeJoinReady &ready, SteamBridgeProtocolError &error);
[[nodiscard]] bool decode_steam_bridge_join_ready(std::span<const std::uint8_t> payload,
                                                  SteamBridgeJoinReady &ready,
                                                  SteamBridgeProtocolError &error);

} // namespace openminerz::net
