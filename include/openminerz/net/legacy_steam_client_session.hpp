#pragma once

#include "openminerz/net/legacy_steam_host_session.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
    gameplay,
    // `gid` announced the Xbox Avatar add-on (openminerz/net/avatar_sync.hpp);
    // `payload` is the one byte of the avatar message id to use with it. Always
    // precedes the PlayerExists it was read from.
    avatar_capability,
    // An add-on avatar message from `gid`. Never a stock message.
    avatar_sync
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

inline constexpr std::string_view legacy_duplicate_gamer_denial = "GamerAlreadyConnected";
inline constexpr std::uint8_t legacy_maximum_admission_attempts = 4U;

// The display tag used for admission attempt `attempt` (1-based). Attempt one is
// the unmodified tag; later attempts append " (2)", " (3)"... within the stock
// 128-byte bound without splitting a UTF-8 sequence.
[[nodiscard]] std::string distinct_gamer_tag(std::string_view base_tag, std::uint8_t attempt);

// Transport-independent deployed Steam client session. Native P2P channel one
// is reserved for a raw denial; channel zero carries the four-byte stock wire
// envelope. No application packet is emitted before both status 5 and the
// response-to-connection peer/GID table have been validated.
class LegacySteamClientSession final
{
  public:
    explicit LegacySteamClientSession(LegacySteamClientSessionOptions options);

    [[nodiscard]] std::optional<LegacySteamNativeSend> begin(std::string &error);
    // The stock host refuses a join when any gamer in the session has the same
    // display tag, even for a different Steam account. After exactly that denial,
    // this re-requests admission under the next distinct display tag; the player id
    // and the Steam peer, which carry identity, are unchanged. Returns no packet
    // once the attempts are spent or when the last denial had another cause.
    [[nodiscard]] std::optional<LegacySteamNativeSend> retry_with_distinct_gamer_tag(
        std::string &error);
    [[nodiscard]] std::uint8_t admission_attempts() const noexcept;
    [[nodiscard]] LegacySteamClientSessionResult process_native_packet(
        PeerId sender, std::uint8_t p2p_channel, std::span<const std::uint8_t> wire);
    [[nodiscard]] std::optional<LegacySteamNativeSend> create_outbound(
        std::optional<std::uint8_t> recipient_gid, std::span<const std::uint8_t> application_packet,
        MessageDeliveryPolicy delivery, std::string &error) const;
    // The Xbox Avatar add-on's extra message, to one gamer (the host directly,
    // anyone else through the host's relay). THE GATE: a stock host forwards a
    // proxied packet without reading it, so this is the last place a packet
    // that a stock client cannot read is stopped. It refuses any recipient that
    // has not announced the add-on in this session (for the current occupant of
    // that gamer id), any packet that is not a well-formed avatar message, and
    // any id but the one that recipient reported. create_outbound rejects every
    // id outside the stock table, so nothing else here can emit one.
    [[nodiscard]] std::optional<LegacySteamNativeSend> create_avatar_outbound(
        std::uint8_t recipient_gid, std::span<const std::uint8_t> avatar_packet,
        std::string &error) const;
    [[nodiscard]] bool avatar_capable(std::uint8_t gid) const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> avatar_message_id(std::uint8_t gid) const noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] ProtocolVersion protocol() const noexcept { return options_.protocol; }
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
    std::string requested_gamer_tag_;
    std::string last_denial_;
    std::uint8_t admission_attempts_{};
    SessionProperties properties_{};
    std::vector<RemoteGamer> remote_gamers_;
    std::optional<std::uint8_t> local_gid_;
    std::optional<PendingWire> pending_connected_response_;
    std::vector<PendingWire> pending_application_;
    std::size_t pending_application_bytes_{};
    AvatarCapabilityRegistry avatar_capabilities_;
    State state_{State::idle};
    std::uint64_t next_audit_sequence_{};
};

} // namespace openminerz::net
