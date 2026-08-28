#pragma once

#include "openminerz/net/legacy_steam_codec.hpp"
#include "openminerz/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace openminerz::net
{

enum class LegacySteamRejectionStage : std::uint8_t
{
    wire_envelope,
    approval,
    session,
    routing,
    application,
    delivery
};

struct LegacySteamAuditEvent final
{
    std::uint64_t sequence{};
    PeerId peer{};
    LegacySteamRejectionStage stage{LegacySteamRejectionStage::wire_envelope};
    std::string reason;
    std::size_t input_length{};
};

struct LegacySteamNativeSend final
{
    PeerId remote_peer{};
    std::vector<std::uint8_t> payload;
    SteamDeliveryMethod delivery{SteamDeliveryMethod::reliable_ordered};
    std::uint8_t p2p_channel{};
};

enum class LegacySteamInboundKind : std::uint8_t
{
    peer_connected,
    gameplay
};

struct LegacySteamHostInbound final
{
    LegacySteamInboundKind kind{LegacySteamInboundKind::gameplay};
    PeerId peer{};
    std::vector<std::uint8_t> payload;
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::reliable_ordered};
    std::uint8_t channel{};
    std::uint8_t sender_gid{};
    std::uint8_t recipient_gid{};
};

struct LegacySteamHostSessionResult final
{
    std::vector<LegacySteamNativeSend> sends;
    std::vector<LegacySteamHostInbound> inbound;
    std::optional<std::string> rejection_reason;
    std::optional<LegacySteamAuditEvent> audit;
};

struct LegacySteamHostSessionOptions final
{
    HostSessionSettings settings;
    bool game_begun{};
    std::int32_t game_mode{1};
    std::int32_t difficulty{};
    bool infinite_resources{};
    LegacyGamerIdentity host_gamer{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
                                   "OpenMinerZ Host"};
    std::function<bool(PeerId)> platform_authorized;
};

struct LegacySteamOutbound final
{
    std::optional<PeerId> recipient;
    std::vector<std::uint8_t> application_packet;
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::reliable_ordered};
    std::uint8_t channel{};
    std::uint8_t sender_gid{};
    std::uint8_t recipient_gid{};
    bool client_relay{};
};

// Source-shaped, transport-independent stock host state machine. A native
// Steam adapter may only pass packets from current lobby members into this
// class; all route, GID, checksum, payload, and delivery validation happens
// here before gameplay reaches the simulation.
class LegacySteamHostSession final
{
  public:
    explicit LegacySteamHostSession(LegacySteamHostSessionOptions options);

    [[nodiscard]] LegacySteamHostSessionResult process_wire_packet(
        PeerId sender, std::span<const std::uint8_t> wire);
    [[nodiscard]] std::optional<std::vector<LegacySteamNativeSend>> create_outbound(
        const LegacySteamOutbound &outbound, std::string &error) const;
    [[nodiscard]] std::vector<LegacySteamNativeSend> remove_peer(PeerId peer);
    [[nodiscard]] bool authorize_extension(PeerId peer);

    [[nodiscard]] bool update_password(std::string password, std::string &error);
    [[nodiscard]] bool update_session_property(std::size_t index, std::int32_t value,
                                               std::string &error);
    [[nodiscard]] bool connected(PeerId peer) const noexcept;
    [[nodiscard]] std::size_t connected_peer_count() const noexcept;
    [[nodiscard]] SessionProperties session_properties() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> gid_for(PeerId peer) const noexcept;
    [[nodiscard]] std::optional<PeerId> peer_for_gid(std::uint8_t gid) const noexcept;
    [[nodiscard]] const LegacyGamerIdentity *gamer_for_gid(std::uint8_t gid) const noexcept;

  private:
    struct RemotePeer final
    {
        PeerId peer{};
        std::uint8_t gid{};
        LegacyGamerIdentity gamer;
    };

    [[nodiscard]] LegacySteamHostSessionResult process_approval(
        PeerId sender, std::span<const std::uint8_t> payload, std::size_t input_length);
    [[nodiscard]] LegacySteamHostSessionResult process_data(PeerId sender,
                                                            const LegacySteamWirePacket &packet,
                                                            std::size_t input_length);
    [[nodiscard]] LegacySteamHostSessionResult reject(PeerId peer, std::string reason,
                                                      std::size_t input_length);
    [[nodiscard]] LegacySteamHostSessionResult audit_reject(PeerId peer,
                                                            LegacySteamRejectionStage stage,
                                                            std::string reason,
                                                            std::size_t input_length);
    [[nodiscard]] std::optional<std::string> validate_connection(
        PeerId sender, const LegacyConnectionRequest &request) const;
    [[nodiscard]] std::optional<std::uint8_t> allocate_gid() noexcept;
    [[nodiscard]] const RemotePeer *find_peer(PeerId peer) const noexcept;
    [[nodiscard]] RemotePeer *find_peer(PeerId peer) noexcept;
    [[nodiscard]] const RemotePeer *find_gid(std::uint8_t gid) const noexcept;
    [[nodiscard]] bool validate_application(std::span<const std::uint8_t> packet,
                                            MessageDeliveryPolicy delivery, bool validate_delivery,
                                            DecodedPacket &decoded) const noexcept;
    [[nodiscard]] std::optional<LegacySteamNativeSend> make_direct_send(
        const RemotePeer &recipient, std::uint8_t sender_gid,
        std::span<const std::uint8_t> application_packet, SteamDeliveryMethod delivery) const;
    [[nodiscard]] LegacyGamerIdentity advertised_host() const;

    LegacySteamHostSessionOptions options_;
    SessionProperties properties_{};
    std::vector<RemotePeer> peers_;
    std::unordered_set<PeerId> extension_authorized_peers_;
    std::uint8_t next_gid_{1};
    std::uint64_t next_audit_sequence_{};
};

[[nodiscard]] std::optional<SteamDeliveryMethod> to_steam_delivery(
    MessageDeliveryPolicy delivery) noexcept;
[[nodiscard]] std::optional<MessageDeliveryPolicy> from_steam_delivery(
    SteamDeliveryMethod delivery) noexcept;

} // namespace openminerz::net
