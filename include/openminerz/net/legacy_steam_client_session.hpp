#pragma once

#include "openminerz/net/legacy_steam_host_session.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openminerz::net
{

struct LegacySteamClientSessionOptions final
{
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    PeerId host_peer{};
    std::string expected_host_name;
    SessionProperties session_properties{};
    std::string password;
    LegacyGamerIdentity local_gamer;
};

enum class LegacySteamClientInboundKind : std::uint8_t
{
    connected,
    peer_joined,
    peer_left,
    session_properties_changed,
    gameplay
};

struct LegacySteamClientInbound final
{
    LegacySteamClientInboundKind kind{LegacySteamClientInboundKind::gameplay};
    std::uint8_t gid{};
    LegacyGamerIdentity gamer;
    std::vector<std::uint8_t> payload;
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::reliable_ordered};
};

struct LegacySteamClientSessionResult final
{
    std::vector<LegacySteamClientInbound> inbound;
    std::optional<std::string> rejection_reason;
    std::optional<LegacySteamAuditEvent> audit;
};

// Transport-independent deployed Steam client session. Native P2P channel one
// is reserved for a raw denial; channel zero carries the four-byte stock wire
// envelope. No application packet is emitted before both status 5 and the
// response-to-connection peer/GID table have been validated.
class LegacySteamClientSession final
{
  public:
    explicit LegacySteamClientSession(LegacySteamClientSessionOptions options);

    [[nodiscard]] std::optional<LegacySteamNativeSend> begin(std::string &error);
    [[nodiscard]] LegacySteamClientSessionResult process_native_packet(
        PeerId sender, std::uint8_t p2p_channel, std::span<const std::uint8_t> wire);
    [[nodiscard]] std::optional<LegacySteamNativeSend> create_outbound(
        std::optional<std::uint8_t> recipient_gid, std::span<const std::uint8_t> application_packet,
        MessageDeliveryPolicy delivery, std::string &error) const;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> local_gid() const noexcept;
    [[nodiscard]] const LegacyGamerIdentity &local_gamer() const noexcept;
    [[nodiscard]] const LegacyGamerIdentity *gamer_for(std::uint8_t gid) const noexcept;
    [[nodiscard]] SessionProperties session_properties() const noexcept;

  private:
    enum class State : std::uint8_t
    {
        idle,
        awaiting_status,
        awaiting_response,
        connected,
        rejected
    };

    struct RemoteGamer final
    {
        std::uint8_t gid{};
        LegacyGamerIdentity gamer;
    };

    struct PendingWire final
    {
        LegacySteamWirePacket packet;
        std::size_t input_length{};
    };

    [[nodiscard]] LegacySteamClientSessionResult process_denial(std::span<const std::uint8_t> wire);
    [[nodiscard]] LegacySteamClientSessionResult process_wire(const LegacySteamWirePacket &packet,
                                                              std::size_t input_length);
    [[nodiscard]] LegacySteamClientSessionResult process_connected_response(
        std::span<const std::uint8_t> payload, std::size_t input_length);
    [[nodiscard]] LegacySteamClientSessionResult process_internal(
        std::span<const std::uint8_t> payload, std::size_t input_length);
    [[nodiscard]] LegacySteamClientSessionResult process_direct(
        std::span<const std::uint8_t> payload, std::size_t input_length);
    [[nodiscard]] LegacySteamClientSessionResult queue_before_admission(
        const LegacySteamWirePacket &packet, std::size_t input_length);
    void drain_pending(LegacySteamClientSessionResult &result);
    [[nodiscard]] LegacySteamClientSessionResult reject(LegacySteamRejectionStage stage,
                                                        std::string reason,
                                                        std::size_t input_length);
    [[nodiscard]] LegacySteamClientSessionResult reject_admission(
        LegacySteamRejectionStage stage, std::string reason, std::size_t input_length);
    [[nodiscard]] const RemoteGamer *find_gid(std::uint8_t gid) const noexcept;

    LegacySteamClientSessionOptions options_;
    SessionProperties properties_{};
    std::vector<RemoteGamer> remote_gamers_;
    std::optional<std::uint8_t> local_gid_;
    std::optional<PendingWire> pending_connected_response_;
    std::vector<PendingWire> pending_application_;
    std::size_t pending_application_bytes_{};
    State state_{State::idle};
    std::uint64_t next_audit_sequence_{};
};

} // namespace openminerz::net
