#pragma once

#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/protocol_trace.hpp"
#include "openminerz/net/steam_bridge_client.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::net
{

struct LegacySteamRuntimeHostOptions final
{
    HostSessionSettings settings;
    std::int32_t game_mode{1};
    std::int32_t difficulty{};
    bool infinite_resources{};
    std::optional<AuthoritativeBootstrapSnapshot> bootstrap_snapshot;
    std::filesystem::path helper_path;
    std::filesystem::path steam_api_path;
    std::filesystem::path inventory_directory;
    std::optional<std::filesystem::path> trace_path;
};

enum class LegacySteamRuntimeEventKind : std::uint8_t
{
    peer_connected,
    // Only ever emitted for a peer that had been admitted and holds a GID.
    peer_disconnected,
    gameplay,
    rejected,
    transport_failure,
    // Progress of a join attempt, for the host's screen and log. Carries no
    // gameplay and never changes session state.
    join_notice,
    // The Xbox Avatar add-on (openminerz/net/avatar_sync.hpp): the peer with
    // `inbound.sender_gid` announced it; `inbound.payload` holds the one byte of
    // its avatar message id.
    avatar_capability,
    // An add-on avatar message addressed to the host. Never a stock message.
    avatar_sync,
    // (Network) One mod channel frame (openminerz/modding/net.hpp) from
    // `peer`, an admitted peer that completed the extension hello of a
    // session with server/core mods. `inbound.payload` holds the frame.
    mod_channel,
    // (Network) One mod download frame (net/mod_download.hpp) from lobby
    // member `peer`, admitted or not, of a session with server/core mods.
    mod_download
};

enum class LegacySteamJoinNoticeKind : std::uint8_t
{
    // A Steam user entered the lobby. No join request has arrived yet.
    entered_lobby,
    // The join request or mod handshake was refused; `reason` is what the
    // joining client was told.
    refused,
    // In the lobby for a while with no join request: Steam P2P never connected.
    request_overdue,
    // Admitted, but has not announced itself (still loading, or stuck).
    loading_overdue,
    // Left the lobby without ever being admitted.
    left_before_admission
};

struct LegacySteamJoinNotice final
{
    LegacySteamJoinNoticeKind kind{LegacySteamJoinNoticeKind::entered_lobby};
    PeerId peer{};
    // Best name known for the peer: the tag from its join request, otherwise
    // its Steam persona, otherwise empty.
    std::string name;
    std::string reason;
    std::uint32_t waited_seconds{};
};

struct LegacySteamRuntimeEvent final
{
    LegacySteamRuntimeEventKind kind{LegacySteamRuntimeEventKind::gameplay};
    LegacySteamHostInbound inbound;
    LegacySteamAuditEvent audit;
    PeerId peer{};
    std::string detail;
    LegacySteamJoinNotice notice;
};

// How long a lobby member may sit without a join request, and an admitted peer
// without announcing itself, before the host is told once.
inline constexpr std::chrono::seconds legacy_steam_join_request_overdue{10};
inline constexpr std::chrono::seconds legacy_steam_join_loading_overdue{90};

// Replaces the x86 helper for tests and tooling: outbound wire datagrams go to
// `send`, and `now` supplies the clock used for join-progress notices.
struct LegacySteamDetachedTransport final
{
    std::function<bool(const SteamBridgeOutboundWire &, std::string &)> send;
    std::function<std::chrono::steady_clock::time_point()> now;
};

[[nodiscard]] bool make_steam_bridge_host_config(const LegacySteamRuntimeHostOptions &options,
                                                 SteamBridgeHostConfig &config,
                                                 std::string &error) noexcept;

// What the host is shown and what is logged for one join notice. `log` is a
// single `network.host.join ...` line naming the Steam ID, the name and the
// raw reason; `title`/`body` are the on-screen wording of the same facts.
struct LegacySteamJoinNoticeText final
{
    std::string title;
    std::string body;
    std::string log;
};

[[nodiscard]] LegacySteamJoinNoticeText describe_join_notice(const LegacySteamJoinNotice &notice);

// The app-facing authority boundary. The x86 helper supplies only lobby-member
// wire datagrams; this class runs every stock envelope, approval, routing,
// application, and delivery check before emitting gameplay events.
class LegacySteamRuntimeHost final
{
  public:
    LegacySteamRuntimeHost() = default;
    ~LegacySteamRuntimeHost();

    LegacySteamRuntimeHost(const LegacySteamRuntimeHost &) = delete;
    LegacySteamRuntimeHost &operator=(const LegacySteamRuntimeHost &) = delete;
    LegacySteamRuntimeHost(LegacySteamRuntimeHost &&) noexcept;
    LegacySteamRuntimeHost &operator=(LegacySteamRuntimeHost &&) noexcept;

    [[nodiscard]] bool start(const LegacySteamRuntimeHostOptions &options,
                             SteamBridgeHostReady &ready, std::string &error);
    // Runs the identical admission, bootstrap, relay and diagnostic authority
    // with no helper process: `identity` stands in for the helper's host_ready,
    // inbound bridge events are queued with inject(), and pump() consumes them
    // exactly as it consumes helper frames.
    [[nodiscard]] bool start_detached(const LegacySteamRuntimeHostOptions &options,
                                      const SteamBridgeHostReady &identity,
                                      LegacySteamDetachedTransport transport, std::string &error);
    void inject(SteamBridgeClientEvent event);
    [[nodiscard]] bool pump(std::vector<LegacySteamRuntimeEvent> &events, std::string &error);
    [[nodiscard]] bool send_authoritative(const LegacySteamOutbound &outbound, std::string &error);
    // The Xbox Avatar add-on's extra message from the host to one peer. Fails,
    // sending nothing, unless that peer announced the add-on in this session
    // (LegacySteamHostSession::create_avatar_outbound).
    [[nodiscard]] bool send_avatar(std::uint8_t recipient_gid,
                                   std::span<const std::uint8_t> avatar_packet, std::string &error);
    [[nodiscard]] bool avatar_capable(std::uint8_t gid) const noexcept;
    // Announces the add-on to everyone in the session: a duplicate, stock
    // PlayerExistsMessage from the host gamer whose avatar description ends in
    // the capability marker (net::make_avatar_capability_presence).
    [[nodiscard]] bool send_avatar_capability(std::string &error);
    // (Network) One mod channel or mod download frame to `peer`, on the
    // reliable-ordered path beside the extension hello. Fails, sending
    // nothing, unless this session carries a server/core mod and, for a mod
    // channel frame, the peer is admitted through the extension hello. A
    // stock peer can therefore never be sent one.
    [[nodiscard]] bool send_mod_frame(PeerId peer, std::span<const std::uint8_t> frame,
                                      std::string &error);
    [[nodiscard]] bool mod_channel_peer(PeerId peer) const noexcept;
    [[nodiscard]] bool store_authoritative_inventory(PeerId peer,
                                                     const PlayerInventoryPayload &inventory);
    [[nodiscard]] bool apply_authoritative_crate_item(const ItemCratePayload &crate);
    [[nodiscard]] bool destroy_authoritative_crate(IntVector3 location);
    [[nodiscard]] bool set_authoritative_custom_door(IntVector3 location, std::uint8_t model_name);
    [[nodiscard]] bool destroy_authoritative_custom_block(IntVector3 location);
    [[nodiscard]] bool update_authoritative_spawner(IntVector3 location, bool started);
    [[nodiscard]] bool update_hell_boss_progress(std::uint32_t spawned, std::uint32_t maximum);
    // A session property changed while hosting (HostOptions' PVP row is slot
    // 5). SteamNetworkSessionProvider.UpdateHostSession stores it and rewrites
    // the Steam lobby data, and that is all: the next joiner is told the new
    // value, and peers already connected are sent nothing.
    [[nodiscard]] bool update_session_property(std::size_t index, std::int32_t value,
                                               std::string &error);
    [[nodiscard]] SessionProperties session_properties() const noexcept;
    // HostOptions' other three lobby rows, as the shipped host applies them
    // (GameScreen._hostOptionsMenu_MenuItemSelected ->
    // SteamNetworkSessionProvider.UpdateHostSession / UpdateHostSessionJoinPolicy
    // -> SteamWorks.UpdateHostLobbyData). Each rewrites the Steam lobby's
    // `Name`, `Protected` and `JoinPolicy` keys and its lobby type, and sends
    // the peers already connected nothing at all:
    //   Server Message  the lobby `Name` (what the server list shows). The
    //                   caller has already trimmed it and dropped a blank one.
    //   Set Password    `Protected` True/False, and the password every LATER
    //                   join request must carry; blank or all-whitespace clears.
    //   Who can join    `JoinPolicy` 0/1/2 (Anyone / FriendsOnly / InviteOnly)
    //                   and the lobby type public / friends-only / private.
    [[nodiscard]] bool update_server_message(std::string_view message, std::string &error);
    [[nodiscard]] bool update_password(std::string password, std::string &error);
    [[nodiscard]] bool update_join_policy(SteamBridgeJoinPolicy policy, std::string &error);
    // What the lobby is advertised with right now (empty when not hosting).
    [[nodiscard]] std::optional<SteamBridgeHostConfig> advertised_lobby() const;
    [[nodiscard]] bool apply_authoritative_block_edit(IntVector3 position, std::uint8_t block_type);
    [[nodiscard]] bool apply_authoritative_block_replacements(std::span<const IntVector3> positions,
                                                              std::uint8_t block_type);
    [[nodiscard]] bool apply_authoritative_block_removals(std::span<const IntVector3> positions);
    [[nodiscard]] bool stop(std::string &error) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> gid_for(PeerId peer) const noexcept;
    [[nodiscard]] std::optional<PeerId> peer_for_gid(std::uint8_t gid) const noexcept;
    [[nodiscard]] const LegacyGamerIdentity *gamer_for_gid(std::uint8_t gid) const noexcept;
    // The tag the peer's own client uses in chat and name-addressed packets; it
    // differs from the roster tag only after a display-name collision.
    [[nodiscard]] const std::string *self_reported_tag_for_gid(std::uint8_t gid) const noexcept;
    [[nodiscard]] bool gameplay_ready(PeerId peer, std::uint8_t player_gid) const noexcept;
    // See LegacySteamHostSession::all_peers_extension_capable. Without a live
    // session this answers false: unconfirmed counts as stock.
    [[nodiscard]] bool all_peers_extension_capable() const noexcept;

  private:
    using Clock = std::chrono::steady_clock;

    struct PendingPrePresenceInbound final
    {
        LegacySteamHostInbound inbound;
        std::string message_name;
    };

    // What the host has seen of one lobby member's join attempt.
    struct JoinProgress final
    {
        std::string name;
        Clock::time_point entered{};
        Clock::time_point admitted{};
        std::string refusal;
        bool request_seen{};
        bool is_admitted{};
        bool request_overdue_reported{};
        bool loading_overdue_reported{};
    };

    void open_session(const LegacySteamRuntimeHostOptions &options,
                      const SteamBridgeHostReady &ready);
    [[nodiscard]] bool transport_running() const noexcept;
    [[nodiscard]] bool next_bridge_event(std::optional<SteamBridgeClientEvent> &event,
                                         std::string &error);
    [[nodiscard]] bool send_wire(const SteamBridgeOutboundWire &datagram, std::string &error);
    [[nodiscard]] Clock::time_point now() const;
    [[nodiscard]] JoinProgress &track_member(PeerId peer);
    void note_refusal(PeerId peer, std::string name, std::string reason,
                      std::vector<LegacySteamRuntimeEvent> &events);
    void report_overdue_joins(std::vector<LegacySteamRuntimeEvent> &events);
    [[nodiscard]] bool send_all(std::span<const LegacySteamNativeSend> sends, std::string &error);
    [[nodiscard]] bool queue_pre_presence(LegacySteamHostInbound inbound,
                                          std::string_view message_name);
    void replay_pre_presence(PeerId peer, std::vector<LegacySteamRuntimeEvent> &events);
    void forget_pre_presence(PeerId peer) noexcept;

    SteamBridgeClient bridge_;
    ProtocolVersion protocol_{ProtocolVersion::classic_v3};
    std::optional<LegacySteamHostSession> session_;
    // What the Steam lobby was created with, kept so a later property change
    // can rewrite the lobby data without disturbing anything else in it.
    std::optional<SteamBridgeHostConfig> lobby_config_;
    std::optional<AuthoritativeBootstrapHost> bootstrap_;
    std::optional<ProtocolTraceWriter> trace_;
    ModManifest mod_manifest_;
    // The host's own gamer, as its presence announces it.
    LegacyGamerIdentity host_gamer_;
    std::unordered_map<PeerId, std::vector<PendingPrePresenceInbound>> pre_presence_packets_;
    std::size_t pre_presence_bytes_{};
    std::unordered_map<PeerId, JoinProgress> join_progress_;
    Clock::time_point next_overdue_check_{};
    std::optional<LegacySteamDetachedTransport> detached_;
    std::deque<SteamBridgeClientEvent> detached_inbound_;
};

} // namespace openminerz::net
