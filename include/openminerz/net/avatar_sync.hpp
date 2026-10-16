#pragma once

// The Xbox Avatar add-on's multiplayer protocol, spoken natively.
//
// The add-on for the original game (KikoTs/castleforge-xbox-avatar; the same
// runtime ships in KikoTs/openclassic-xbox-avatar and
// KikoTs/castleminerz-xbox-avatar) synchronises each player's `.ocavatar` with
// three things, all reproduced here byte for byte and none of them changed:
//
// 1. A CAPABILITY MARKER appended to the avatar description of a duplicate,
//    stock PlayerExistsMessage (RequestResponse = false). A stock client ignores
//    a second PlayerExists from a gamer it already has a Player for
//    (CastleMinerZGame._processPlayerExistsMessage returns at
//    `Sender.Tag != null`), so the marker rides a message every client can read.
//      form 1   "OCXACAP" 0x01                      8 bytes
//      form 2   "OCXACAP" 0x02 <avatar message id>  9 bytes
//    Form 2 exists because the add-on's packet id is POSITIONAL: the engine
//    numbers every Message subclass in the process sorted by full name, so the
//    id is "one past the stock table" on a bare client (53 Classic, 55 Modern)
//    and something else as soon as another mod registers message types.
//
// 2. ONE EXTRA MESSAGE, XboxAvatar.ZZAvatarSyncMessage, sent reliable-in-order
//    directly to one gamer. Its payload is
//      u8 protocol(1)  u8 kind  u32 transfer  i32 total  u16 index  u16 count
//      32-byte SHA-256  u16 length  payload(<= 3000)
//    framed like every stock message: id, payload, XOR-8 checksum.
//
// 3. A FOUR-STEP EXCHANGE over that message:
//      hello     "send me your avatar"
//      manifest  "it is this long, in this many chunks, with this hash"
//      request   "go ahead" (echoes the manifest)
//      chunk     one slice; the receiver verifies the hash when all have come
//
// THE SAFETY PROPERTY. A client that does not know the extra id throws on it
// (Message.GetMessage -> _receiveInstance[id] -> InvalidMessageException), which
// ends that player's session. So the extra message may only ever be addressed
// to a gamer whose marker has been seen in this session, for the current
// occupant of that gamer id. That gate is enforced twice: AvatarSyncSession
// never produces a packet for a peer it has not been told is capable, and the
// production session classes (LegacySteamHostSession, LegacySteamClientSession)
// independently refuse to emit or relay one, whoever asks.
//
// OpenMinerZ does not register message types, so it has no id of its own. It
// speaks to each peer with the id THAT peer reported (the stock-table size for
// a form-1 peer), and announces itself with form 1, which every build of the
// add-on accepts without comparing ids.

#include "openminerz/net/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::net
{

inline constexpr std::array<std::uint8_t, 7> avatar_marker_prefix{0x4FU, 0x43U, 0x58U, 0x41U,
                                                                  0x43U, 0x41U, 0x50U};
inline constexpr std::uint8_t avatar_protocol_version = 1U;
inline constexpr std::uint8_t avatar_marker_with_message_id = 2U;
inline constexpr std::size_t avatar_chunk_payload = 3000U;
inline constexpr std::size_t avatar_maximum_asset_bytes = 4U * 1024U * 1024U;
inline constexpr std::size_t avatar_maximum_incoming_transfers = 8U;
inline constexpr std::size_t avatar_chunks_per_update = 4U;
// The add-on's four-per-update ceiling at a thirty-frame update rate.
inline constexpr double avatar_chunks_per_second = 120.0;
// protocol, kind, transfer, total, index, count, hash, payload length.
inline constexpr std::size_t avatar_sync_header_bytes = 1U + 1U + 4U + 4U + 2U + 2U + 32U + 2U;
inline constexpr double avatar_first_chunk_timeout_seconds = 180.0;
inline constexpr double avatar_transfer_timeout_seconds = 45.0;
inline constexpr double avatar_manifest_interval_seconds = 5.0;
inline constexpr double avatar_cleanup_interval_seconds = 5.0;

using AvatarHash = std::array<std::uint8_t, 32>;

enum class AvatarPacketKind : std::uint8_t
{
    hello = 1,
    manifest = 2,
    request = 3,
    chunk = 4
};

struct AvatarSyncMessage final
{
    std::uint8_t protocol{avatar_protocol_version};
    std::uint8_t kind{static_cast<std::uint8_t>(AvatarPacketKind::hello)};
    std::uint32_t transfer_id{};
    std::int32_t total_length{};
    std::uint16_t chunk_index{};
    std::uint16_t chunk_count{};
    AvatarHash hash{};
    std::vector<std::uint8_t> payload;

    bool operator==(const AvatarSyncMessage &) const = default;
};

// ZZAvatarSyncMessage.SendData / RecieveData.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_avatar_sync_payload(
    const AvatarSyncMessage &message);
[[nodiscard]] bool decode_avatar_sync_payload(std::span<const std::uint8_t> payload,
                                              AvatarSyncMessage &message);
// The complete application packet: message id, payload, XOR-8 checksum.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_avatar_sync_packet(
    std::uint8_t message_id, const AvatarSyncMessage &message);
[[nodiscard]] bool decode_avatar_sync_packet(std::span<const std::uint8_t> packet,
                                             std::uint8_t &message_id, AvatarSyncMessage &message);

// Where the add-on's message lands on a client that registers nothing else:
// one past the stock table.
[[nodiscard]] std::uint8_t natural_avatar_message_id(ProtocolVersion protocol) noexcept;
// An id is usable only when it cannot be mistaken for a stock message.
[[nodiscard]] bool valid_avatar_message_id(ProtocolVersion protocol, std::uint8_t id) noexcept;

struct AvatarCapabilityMarker final
{
    bool present{};
    // The id the sender registered the avatar message under (form 2 only).
    std::optional<std::uint8_t> message_id;
    // The description without the marker.
    std::size_t description_length{};
};

// AvatarNetworkBridge.TryReadCapabilityMarker.
[[nodiscard]] AvatarCapabilityMarker read_avatar_capability_marker(
    std::span<const std::uint8_t> description) noexcept;
// AvatarNetworkBridge.AppendCapabilityMarker: form 2 when an id is given.
[[nodiscard]] std::vector<std::uint8_t> append_avatar_capability_marker(
    std::span<const std::uint8_t> description, std::optional<std::uint8_t> message_id);

// What one PlayerExistsMessage application packet says about its sender.
struct AvatarCapabilityAnnouncement final
{
    bool present{};
    // The id to use with this peer: the reported one, else the natural one.
    std::uint8_t message_id{};
    bool reported_id{};
};
[[nodiscard]] AvatarCapabilityAnnouncement read_avatar_capability_announcement(
    ProtocolVersion protocol, std::span<const std::uint8_t> application_packet);

// This client's own announcement: the duplicate PlayerExistsMessage
// (RequestResponse = false) the add-on broadcasts, as a complete application
// packet. The description is the ten zero bytes every PC client announces,
// followed by the form-1 marker; the gamer is the one already announced.
// It is a stock message: a client without the add-on reads a second presence
// from a gamer it already knows, and ignores it.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> make_avatar_capability_presence(
    ProtocolVersion protocol, std::string_view gamer_tag, std::span<const std::uint8_t> player_id);

// Who in the session has announced the add-on, by gamer id. Owned by each
// production session class; the only thing that can open the gate.
class AvatarCapabilityRegistry final
{
  public:
    // Returns true when the peer is (still) capable after this announcement.
    bool announce(std::uint8_t gid, std::uint8_t message_id, bool agnostic = false) noexcept;
    void forget(std::uint8_t gid) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool capable(std::uint8_t gid) const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> message_id(std::uint8_t gid) const noexcept;
    // A peer that reads the message under whatever id it arrives with (another
    // OpenMinerZ). Only ever set from the private extension handshake.
    void set_agnostic(std::uint8_t gid, bool agnostic) noexcept;
    [[nodiscard]] bool agnostic(std::uint8_t gid) const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    struct Entry final
    {
        bool capable{};
        bool agnostic{};
        std::uint8_t message_id{};
    };
    std::array<Entry, 256> entries_{};
};

// True when `packet` is a well-formed avatar message under `message_id`.
[[nodiscard]] bool is_avatar_sync_packet(std::span<const std::uint8_t> packet,
                                         std::uint8_t message_id) noexcept;

struct AvatarSyncSend final
{
    std::uint8_t recipient_gid{};
    std::vector<std::uint8_t> packet;
};

// A complete transfer. `bytes` is NOT yet verified against `hash`: hashing four
// megabytes is worker-thread work, and the consumer does it there.
struct AvatarReceived final
{
    std::uint8_t gid{};
    AvatarHash hash{};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

struct AvatarSyncCounters final
{
    std::uint64_t hellos_sent{};
    std::uint64_t manifests_sent{};
    std::uint64_t requests_sent{};
    std::uint64_t chunks_sent{};
    std::uint64_t transfers_completed{};
    std::uint64_t packets_ignored{};
};

// AvatarNetworkBridge, transport-independent. One instance per session; time is
// a monotonic clock in seconds supplied by the caller.
class AvatarSyncSession final
{
  public:
    // A new session: every peer, offer and transfer of the previous one is gone.
    void reset(ProtocolVersion protocol) noexcept;
    // Whether this client offers its avatar to peers, and asks for theirs.
    void set_policy(bool offer_local, bool request_remote, double now);
    // The local avatar and its SHA-256, or nullptr for none. A change is
    // offered again to every peer that can take it.
    void set_local_avatar(std::shared_ptr<const std::vector<std::uint8_t>> bytes,
                          const AvatarHash &hash, double now);
    // Nothing is sent until the local capability marker has gone out: a peer
    // cannot recognise this client before that and would refuse to serve it.
    void set_advertised(bool advertised) noexcept;
    // `gid` announced the add-on (again) under `message_id`.
    void peer_capable(std::uint8_t gid, std::uint8_t message_id, double now);
    void peer_left(std::uint8_t gid) noexcept;
    // An avatar message from `sender_gid`, already recognised by the session.
    void receive(std::uint8_t sender_gid, std::span<const std::uint8_t> packet, double now);
    // Once per frame: serve chunks, expire offers and transfers.
    void update(double now);

    [[nodiscard]] std::vector<AvatarSyncSend> take_sends() noexcept;
    [[nodiscard]] std::vector<AvatarReceived> take_received() noexcept;

    // The consumer verified (or refused) a completed transfer.
    void transfer_verified(std::uint8_t gid, const AvatarHash &hash, bool valid) noexcept;

    [[nodiscard]] bool peer_ready(std::uint8_t gid) const noexcept;
    [[nodiscard]] std::size_t incoming_transfers() const noexcept;
    [[nodiscard]] std::size_t outgoing_transfers() const noexcept;
    [[nodiscard]] std::size_t pending_offers() const noexcept;
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    [[nodiscard]] const AvatarSyncCounters &counters() const noexcept { return counters_; }

  private:
    struct Peer final
    {
        std::uint8_t message_id{};
        bool hello_sent{};
        bool hello_pending{};
        // The avatar this peer most recently showed us, by hash.
        std::optional<AvatarHash> applied;
        double last_manifest_received{-1.0e9};
        double last_manifest_sent{-1.0e9};
        // The local avatar changed while the peer could not yet be re-offered.
        bool offer_due{};
        // The peer said hello while an offer it may have missed is pending.
        bool repeat_due{};
        // The peer's latest manifest, when it came inside the manifest
        // interval: answered as soon as the interval has passed.
        std::optional<AvatarSyncMessage> deferred_manifest;
    };
    struct Offer final
    {
        std::uint8_t gid{};
        std::uint32_t transfer_id{};
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        AvatarHash hash{};
        double expires{};
    };
    struct Outgoing final
    {
        std::uint8_t gid{};
        std::uint32_t transfer_id{};
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        AvatarHash hash{};
        std::uint32_t chunk_count{};
        std::uint32_t next_chunk{};
    };
    struct Incoming final
    {
        std::uint8_t gid{};
        std::uint32_t transfer_id{};
        std::int32_t total_length{};
        std::uint16_t chunk_count{};
        AvatarHash hash{};
        std::vector<std::uint8_t> bytes;
        std::vector<bool> received;
        std::uint32_t received_count{};
        double expires{};
    };

    void queue(std::uint8_t gid, const AvatarSyncMessage &message);
    void offer(std::uint8_t gid, double now);
    void repeat_offer(std::uint8_t gid, double now);
    void handle_hello(std::uint8_t gid, double now);
    void handle_manifest(std::uint8_t gid, const AvatarSyncMessage &message, double now);
    void handle_request(std::uint8_t gid, const AvatarSyncMessage &message, double now);
    void handle_chunk(std::uint8_t gid, const AvatarSyncMessage &message, double now);
    void cleanup(double now);
    void drop_incoming_for(std::uint8_t gid) noexcept;
    [[nodiscard]] std::uint32_t next_transfer_id() noexcept;

    ProtocolVersion protocol_{ProtocolVersion::classic_v3};
    bool offer_local_{true};
    bool request_remote_{true};
    bool advertised_{};
    std::shared_ptr<const std::vector<std::uint8_t>> local_bytes_;
    AvatarHash local_hash_{};
    std::unordered_map<std::uint8_t, Peer> peers_;
    std::vector<Offer> offers_;
    std::deque<Outgoing> outgoing_;
    std::vector<Incoming> incoming_;
    std::vector<AvatarSyncSend> sends_;
    std::vector<AvatarReceived> received_;
    std::uint32_t next_transfer_id_{1U};
    double next_cleanup_{-1.0e9};
    double last_update_{-1.0e9};
    double chunk_credit_{};
    AvatarSyncCounters counters_{};
};

[[nodiscard]] constexpr std::uint32_t avatar_chunk_count(std::size_t length) noexcept
{
    return static_cast<std::uint32_t>((length + avatar_chunk_payload - 1U) / avatar_chunk_payload);
}

} // namespace openminerz::net
