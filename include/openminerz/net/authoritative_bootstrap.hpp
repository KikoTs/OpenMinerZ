#pragma once

#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/host_session.hpp"
#include "openminerz/net/loopback_session.hpp"
#include "openminerz/world_save.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openminerz::net
{

struct BootstrapWorldIdentity final
{
    std::string name{"World"};
    std::string owner_gamer_tag;
    std::string creator_gamer_tag;
    std::int64_t created_date_ticks{};
    std::int64_t last_played_date_ticks{};
    std::array<std::uint8_t, 16U> world_id{};
    std::int32_t hell_bosses_spawned{};
    std::int32_t max_hell_boss_spawns{};

    auto operator<=>(const BootstrapWorldIdentity &) const = default;
};

struct AuthoritativeBootstrapSnapshot final
{
    WorldInfoPayload world;
    float day{0.41F};
    std::optional<PlayerExistsPayload> host_presence;
    PlayerInventoryPayload inventory;
    bool inventory_is_default{};
    std::vector<ItemCratePayload> crate_replay;
    std::map<IntVector3, std::vector<std::int32_t>> chunk_deltas;
};

enum class BootstrapSnapshotError : std::uint8_t
{
    none,
    invalid_metadata,
    incompatible_inventory,
    incompatible_crate,
    invalid_world_edit,
    packet_budget_exceeded
};

[[nodiscard]] bool make_authoritative_bootstrap_snapshot(const openminerz::WorldSaveData &save,
                                                         const HostSessionSettings &host,
                                                         const BootstrapWorldIdentity &identity,
                                                         bool inventory_is_default,
                                                         AuthoritativeBootstrapSnapshot &snapshot,
                                                         BootstrapSnapshotError &error);

[[nodiscard]] IntVector3 chunk_corner(IntVector3 world_position) noexcept;
[[nodiscard]] std::int32_t chunk_id(IntVector3 corner) noexcept;
[[nodiscard]] bool encode_delta_entry(IntVector3 world_position, std::uint8_t block_type,
                                      ProtocolVersion protocol, std::int32_t &entry) noexcept;
[[nodiscard]] bool inventory_item_to_wire(openminerz::InventoryItemId item,
                                          ProtocolVersion protocol,
                                          std::int16_t &wire_item) noexcept;
[[nodiscard]] bool inventory_item_from_wire(std::int16_t wire_item, ProtocolVersion protocol,
                                            openminerz::InventoryItemId &item) noexcept;
[[nodiscard]] bool inventory_slot_to_stack(const InventorySlotPayload &source,
                                           ProtocolVersion protocol,
                                           openminerz::PlayerInventory::Slot &result);
[[nodiscard]] bool inventory_slot_to_payload(const openminerz::PlayerInventory::Slot &source,
                                             ProtocolVersion protocol,
                                             InventorySlotPayload &result);
[[nodiscard]] bool player_inventory_to_payload(const openminerz::PlayerInventory &source,
                                               ProtocolVersion protocol,
                                               PlayerInventoryPayload &result);

enum class BootstrapPumpError : std::uint8_t
{
    none,
    no_datagram,
    peer_not_registered,
    malformed_frame,
    unexpected_message,
    out_of_order,
    invalid_snapshot,
    transport_rejected
};

struct BootstrapPumpResult final
{
    BootstrapPumpError error{BootstrapPumpError::none};
    std::string_view input_message;
    std::string_view output_message;
    SessionPacketResult transport;

    [[nodiscard]] bool handled() const noexcept
    {
        return error == BootstrapPumpError::none;
    }
};

struct AuthoritativeBootstrapReply final
{
    PeerId peer{};
    PeerId original_sender{};
    std::vector<std::uint8_t> application_packet;
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::reliable_ordered};
};

struct BootstrapHandleResult final
{
    BootstrapPumpError error{BootstrapPumpError::none};
    std::string_view input_message;
    std::string_view output_message;
    std::vector<AuthoritativeBootstrapReply> replies;

    [[nodiscard]] bool handled() const noexcept
    {
        return error == BootstrapPumpError::none;
    }
};

struct PeerBootstrapStatus final
{
    bool world_info_sent{};
    bool player_announced{};
    bool inventory_sent{};
    bool delta_list_sent{};
    std::size_t chunks_sent{};

    auto operator<=>(const PeerBootstrapStatus &) const = default;
};

// Deterministic authoritative handler for the stock late-join transcript. It
// consumes already-admitted loopback datagrams and emits responses through the
// same framing, typed-validation, delivery, and bounded-queue gates used by a
// future native transport.
class AuthoritativeBootstrapHost final
{
  public:
    AuthoritativeBootstrapHost(ProtocolVersion protocol, AuthoritativeBootstrapSnapshot snapshot,
                               std::filesystem::path inventory_directory = {});

    [[nodiscard]] bool register_peer(PeerId peer, std::uint8_t player_gid);
    void disconnect(PeerId peer) noexcept;
    [[nodiscard]] bool store_peer_inventory(PeerId peer, const PlayerInventoryPayload &inventory);
    [[nodiscard]] bool apply_crate_item(const ItemCratePayload &crate);
    [[nodiscard]] bool destroy_crate(IntVector3 location);
    [[nodiscard]] bool set_custom_door(IntVector3 location, std::uint8_t model_name);
    [[nodiscard]] bool destroy_custom_block(IntVector3 location);
    [[nodiscard]] bool update_spawner(IntVector3 location, bool started);
    [[nodiscard]] bool update_hell_boss_progress(std::uint32_t spawned, std::uint32_t maximum);
    [[nodiscard]] bool apply_block_edit(IntVector3 position, std::uint8_t block_type);
    [[nodiscard]] bool apply_block_replacements(std::span<const IntVector3> positions,
                                                std::uint8_t block_type);
    [[nodiscard]] bool apply_block_removals(std::span<const IntVector3> positions);
    [[nodiscard]] std::optional<PeerBootstrapStatus> status(PeerId peer) const noexcept;
    // Exactly the world state a late joiner would now be told about. Exposed so
    // a desync comparison can digest the authoritative world without replaying
    // the whole bootstrap transcript.
    [[nodiscard]] const AuthoritativeBootstrapSnapshot &snapshot() const noexcept;
    // Ordinary gameplay is valid only for the currently admitted peer/GID
    // generation after its source-ordered PlayerExists request was accepted.
    [[nodiscard]] bool gameplay_ready(PeerId peer, std::uint8_t player_gid) const noexcept;
    [[nodiscard]] BootstrapHandleResult handle_packet(
        PeerId peer, std::span<const std::uint8_t> application_packet, std::uint8_t channel,
        bool addressed_to_host = true);
    [[nodiscard]] BootstrapPumpResult pump_one(LoopbackSession &session);

  private:
    struct PeerState final
    {
        std::uint8_t player_gid{};
        PlayerInventoryPayload inventory;
        bool inventory_is_default{};
        PeerBootstrapStatus status;
    };

    [[nodiscard]] std::optional<AuthoritativeBootstrapReply> make_reply(
        PeerId peer, std::string_view output_message, std::span<const std::uint8_t> payload,
        MessageDeliveryPolicy delivery, PeerId original_sender = 0U) const;

    ProtocolVersion protocol_;
    std::filesystem::path inventory_directory_;
    AuthoritativeBootstrapSnapshot snapshot_;
    std::unordered_map<PeerId, PeerState> peers_;
    std::unordered_map<PeerId, PlayerInventoryPayload> stored_inventories_;
};

struct AuthoritativeBootstrapClientOptions final
{
    ProtocolVersion protocol{ProtocolVersion::classic_v3};
    std::uint8_t local_gid{};
    std::vector<std::uint8_t> avatar_description;
    std::string gamer_tag;
    std::vector<std::uint8_t> player_id;
};

struct AuthoritativeBootstrapClientSend final
{
    // nullopt is the original session broadcast route; GID zero is the
    // original direct-to-host/terrain-server route.
    std::optional<std::uint8_t> recipient_gid{std::uint8_t{0}};
    std::vector<std::uint8_t> application_packet;
    MessageDeliveryPolicy delivery{MessageDeliveryPolicy::reliable_ordered};
};

enum class BootstrapClientError : std::uint8_t
{
    none,
    not_started,
    already_started,
    malformed_frame,
    unexpected_sender,
    unexpected_message,
    out_of_order,
    invalid_world,
    invalid_presence_echo,
    invalid_inventory,
    invalid_delta_list,
    invalid_chunk,
    packet_encode_failed,
    already_failed
};

struct BootstrapClientHandleResult final
{
    BootstrapClientError error{BootstrapClientError::none};
    std::string_view input_message;
    std::vector<AuthoritativeBootstrapClientSend> outbound;
    bool completed{};

    [[nodiscard]] bool handled() const noexcept
    {
        return error == BootstrapClientError::none;
    }
};

// Strict client half of the original late-join transcript. The caller feeds
// only already-admitted, typed application packets and sends every returned
// packet directly to host GID zero using the declared delivery policy. A
// WorldSaveData projection becomes visible atomically after every advertised
// delta chunk has arrived and passed its canonical corner/entry checks.
class AuthoritativeBootstrapClient final
{
  public:
    explicit AuthoritativeBootstrapClient(AuthoritativeBootstrapClientOptions options);

    [[nodiscard]] std::optional<AuthoritativeBootstrapClientSend> begin(
        BootstrapClientError &error);
    [[nodiscard]] BootstrapClientHandleResult handle_packet(
        std::uint8_t sender_gid, std::span<const std::uint8_t> application_packet);

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] const std::optional<openminerz::WorldSaveData> &world_save() const noexcept;
    [[nodiscard]] const std::optional<WorldInfoPayload> &world_info() const noexcept;
    [[nodiscard]] const std::optional<PlayerInventoryPayload> &native_inventory() const noexcept;

  private:
    enum class State : std::uint8_t
    {
        idle,
        awaiting_world,
        awaiting_presence,
        awaiting_inventory,
        awaiting_delta_list,
        awaiting_chunks,
        complete,
        failed
    };

    [[nodiscard]] std::optional<AuthoritativeBootstrapClientSend> make_send(
        std::string_view message, std::span<const std::uint8_t> payload,
        MessageDeliveryPolicy delivery,
        std::optional<std::uint8_t> recipient_gid = std::uint8_t{0}) const;
    [[nodiscard]] BootstrapClientHandleResult fail(BootstrapClientError error,
                                                   std::string_view input_message);
    [[nodiscard]] bool install_world(const WorldInfoPayload &world);
    [[nodiscard]] bool apply_crate_item(const ItemCratePayload &crate);
    [[nodiscard]] bool install_inventory(const PlayerInventoryPayload &inventory);
    [[nodiscard]] bool install_chunk(const ProvideChunkPayload &chunk);

    AuthoritativeBootstrapClientOptions options_;
    PlayerExistsPayload presence_;
    State state_{State::idle};
    std::optional<WorldInfoPayload> world_info_;
    std::optional<PlayerInventoryPayload> native_inventory_;
    openminerz::WorldSaveData staged_save_;
    std::optional<openminerz::WorldSaveData> completed_save_;
    std::set<IntVector3> expected_chunks_;
    std::set<IntVector3> received_chunks_;
};

} // namespace openminerz::net
