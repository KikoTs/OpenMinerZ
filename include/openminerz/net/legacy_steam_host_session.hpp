#pragma once

#include "openminerz/net/avatar_sync.hpp"
#include "openminerz/net/legacy_steam_codec.hpp"
#include "openminerz/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace openminerz::net
{

// (Network, Authority) The denial a capability-silent (stock) client gets from
// a session only OpenMinerZ players may join: one with server/core mods, or
// one whose host chose "OpenMinerZ players only". It is the retail word the
// stock client already knows (ResultCode.ConnectionDenied, "Connection failed:
// ConnectionDenied"); a sentence of our own was tried and the owner chose the
// retail text (2026-10-14).
inline constexpr std::string_view legacy_modded_session_denial = "ConnectionDenied";
// What the host's own log and join notice call that refusal. Never sent: the
// wire carries legacy_modded_session_denial.
inline constexpr std::string_view legacy_openminerz_required_reason = "OpenMinerZRequired";

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
    // The tag a refused joiner reported for itself; empty when the request
    // could not be decoded or the rejection is not an admission decision.
    std::string gamer_tag;
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
    gameplay,
    // `sender_gid` announced the Xbox Avatar add-on (openminerz/net/
    // avatar_sync.hpp); `payload` is the one byte of the avatar message id to
    // use with it. Always precedes the PlayerExists it was read from.
    avatar_capability,
    // An add-on avatar message addressed to the host. Never a stock message:
    // it bypasses gameplay authority and is only ever handed to AvatarSyncSession.
    avatar_sync
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
    bool join_closed{};
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

    // The Xbox Avatar add-on's extra message, host to one peer. THE GATE: this
    // refuses any recipient that has not announced the add-on in this session
    // (for the current occupant of that gamer id), any packet that is not a
    // well-formed avatar message, and any id but the one that recipient
    // reported. A client that does not know the id is disconnected by its own
    // message reader, so nothing else in this class can emit one: create_outbound
    // rejects every id outside the stock table.
    [[nodiscard]] std::optional<std::vector<LegacySteamNativeSend>> create_avatar_outbound(
        std::uint8_t recipient_gid, std::span<const std::uint8_t> avatar_packet,
        std::string &error) const;
    [[nodiscard]] bool avatar_capable(std::uint8_t gid) const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> avatar_message_id(std::uint8_t gid) const noexcept;

    [[nodiscard]] bool update_password(std::string password, std::string &error);
    [[nodiscard]] bool update_session_property(std::size_t index, std::int32_t value,
                                               std::string &error);
    [[nodiscard]] bool connected(PeerId peer) const noexcept;
    // Whether this peer completed the OpenMinerZ extension hello before it was
    // admitted. A stock or OpenClassic client never sends one, so this is the
    // session's only positive capability signal; nothing may be inferred from
    // a peer's display tag or player id.
    [[nodiscard]] bool extension_capable(PeerId peer) const noexcept;
    // True only when every peer currently in the session is positively
    // confirmed capable. An empty session is trivially capable. A peer whose
    // capability is unknown counts as stock, because a silent desync with a
    // real stock client is worse than losing an OpenMinerZ-only behaviour.
    [[nodiscard]] bool all_peers_extension_capable() const noexcept;
    [[nodiscard]] std::size_t connected_peer_count() const noexcept;
    [[nodiscard]] SessionProperties session_properties() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> gid_for(PeerId peer) const noexcept;
    [[nodiscard]] std::optional<PeerId> peer_for_gid(std::uint8_t gid) const noexcept;
    // The roster identity: gamer_tag is the session-unique display tag.
    [[nodiscard]] const LegacyGamerIdentity *gamer_for_gid(std::uint8_t gid) const noexcept;
    // The tag the peer's own client uses for itself. It differs from the roster
    // tag only when admission had to de-duplicate a colliding display name.
    [[nodiscard]] const std::string *self_reported_tag_for_gid(std::uint8_t gid) const noexcept;

  private:
    struct RemotePeer final
    {
        PeerId peer{};
        std::uint8_t gid{};
        LegacyGamerIdentity gamer;
        std::string self_reported_tag;
    };

    [[nodiscard]] LegacySteamHostSessionResult process_approval(
        PeerId sender, std::span<const std::uint8_t> payload, std::size_t input_length);
    [[nodiscard]] LegacySteamHostSessionResult process_data(PeerId sender,
                                                            const LegacySteamWirePacket &packet,
                                                            std::size_t input_length);
    [[nodiscard]] LegacySteamHostSessionResult reject(PeerId peer, std::string reason,
                                                      std::size_t input_length,
                                                      std::string gamer_tag = {});
    [[nodiscard]] LegacySteamHostSessionResult audit_reject(PeerId peer,
                                                            LegacySteamRejectionStage stage,
                                                            std::string reason,
                                                            std::size_t input_length);
    [[nodiscard]] std::optional<std::string> validate_connection(
        PeerId sender, const LegacyConnectionRequest &request) const;
    [[nodiscard]] std::optional<std::uint8_t> allocate_gid() noexcept;
    [[nodiscard]] std::string unique_display_tag(const std::string &requested) const;
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
    // An application packet whose id lies outside the stock table: an add-on
    // avatar message, or nothing this host will touch.
    [[nodiscard]] LegacySteamHostSessionResult process_avatar_packet(
        const RemotePeer &sender, std::optional<std::uint8_t> recipient_gid,
        std::vector<std::uint8_t> packet, bool reliable_ordered, std::size_t input_length);
    void observe_avatar_capability(const RemotePeer &sender,
                                   std::span<const std::uint8_t> application_packet,
                                   LegacySteamHostSessionResult &result);

    LegacySteamHostSessionOptions options_;
    SessionProperties properties_{};
    std::vector<RemotePeer> peers_;
    std::unordered_set<PeerId> extension_authorized_peers_;
    AvatarCapabilityRegistry avatar_capabilities_;
    std::uint8_t next_gid_{1};
    std::uint64_t next_audit_sequence_{};
};

[[nodiscard]] std::optional<SteamDeliveryMethod> to_steam_delivery(
    MessageDeliveryPolicy delivery) noexcept;
[[nodiscard]] std::optional<MessageDeliveryPolicy> from_steam_delivery(
    SteamDeliveryMethod delivery) noexcept;

} // namespace openminerz::net
