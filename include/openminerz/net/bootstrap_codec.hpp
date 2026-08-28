#pragma once

#include "openminerz/net/payload_codec.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openminerz::net
{

enum class InventoryItemSerializationKind : std::uint8_t
{
    invalid,
    base,
    gun,
    locator,
    spawn_block,
    teleport_block
};

struct InventoryItemPayload final
{
    std::int16_t item_id{};
    std::int16_t stack_count{};
    float health{};
    std::optional<std::int32_t> rounds_in_clip;
    std::optional<Vector3> point_to_location;
    std::optional<std::string> custom_name;

    auto operator<=>(const InventoryItemPayload &) const = default;
};

using InventorySlotPayload = std::optional<InventoryItemPayload>;
using InventoryStoragePayload = std::array<InventorySlotPayload, 32U>;
using InventoryTrayPayload = std::array<InventorySlotPayload, 8U>;

struct PlayerInventoryPayload final
{
    std::int32_t format_version{};
    InventoryStoragePayload storage;
    std::vector<InventoryTrayPayload> trays;
    std::vector<InventorySlotPayload> teleport_stations;
    InventorySlotPayload spawn_point;

    auto operator<=>(const PlayerInventoryPayload &) const = default;
};

struct InventoryStorePayload final
{
    PlayerInventoryPayload inventory;
    bool final_save{};

    auto operator<=>(const InventoryStorePayload &) const = default;
};

struct InventoryRetrievePayload final
{
    PlayerInventoryPayload inventory;
    std::uint8_t player_id{};
    bool default_inventory{};

    auto operator<=>(const InventoryRetrievePayload &) const = default;
};

struct ItemCratePayload final
{
    IntVector3 location;
    std::int32_t index{};
    InventorySlotPayload item;

    auto operator<=>(const ItemCratePayload &) const = default;
};

struct WorldCratePayload final
{
    IntVector3 location;
    InventoryStoragePayload inventory;

    auto operator<=>(const WorldCratePayload &) const = default;
};

struct WorldDoorPayload final
{
    IntVector3 location;
    std::uint8_t model_name{};

    auto operator<=>(const WorldDoorPayload &) const = default;
};

struct WorldSpawnerPayload final
{
    IntVector3 location;
    std::string spawn_id;

    auto operator<=>(const WorldSpawnerPayload &) const = default;
};

struct WorldInfoPayload final
{
    static constexpr std::int32_t classic_format_version = 1;
    static constexpr std::int32_t modern_format_version = 5;

    std::int32_t format_version{};
    std::int32_t terrain_version{1};
    std::string name{"World"};
    std::string owner_gamer_tag;
    std::string creator_gamer_tag;
    std::int64_t created_date_ticks{};
    std::int64_t last_played_date_ticks{};
    std::int32_t seed{839'880'689};
    std::array<std::uint8_t, 16U> world_id{};
    Vector3 last_position{8.0F, 128.0F, -8.0F};
    std::vector<WorldCratePayload> crates;
    std::vector<WorldDoorPayload> doors;
    std::vector<WorldSpawnerPayload> spawners;
    bool infinite_resource_mode{};
    std::string server_message{"Server"};
    std::string server_password;
    std::int32_t hell_bosses_spawned{};
    std::int32_t max_hell_boss_spawns{};

    auto operator<=>(const WorldInfoPayload &) const = default;
};

inline constexpr std::size_t maximum_world_crates = 4096U;
inline constexpr std::size_t maximum_world_doors = 4096U;
inline constexpr std::size_t maximum_world_spawners = 4096U;
inline constexpr std::size_t maximum_teleport_stations = 256U;

[[nodiscard]] InventoryItemSerializationKind inventory_item_serialization_kind(
    std::int16_t item_id, ProtocolVersion protocol) noexcept;
[[nodiscard]] bool inventory_item_stack_limit(std::int16_t item_id, ProtocolVersion protocol,
                                              std::int32_t &limit) noexcept;
[[nodiscard]] bool inventory_gun_clip_capacity(std::int16_t item_id, ProtocolVersion protocol,
                                               std::int32_t &capacity) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_inventory_item_payload(
    const InventoryItemPayload &item, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_inventory_item_payload(std::span<const std::uint8_t> payload,
                                                 ProtocolVersion protocol,
                                                 InventoryItemPayload &item, PayloadError &error);

[[nodiscard]] PlayerInventoryPayload make_empty_player_inventory(ProtocolVersion protocol);
[[nodiscard]] PlayerInventoryPayload make_default_player_inventory(ProtocolVersion protocol,
                                                                   bool hardcore);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_player_inventory_payload(
    const PlayerInventoryPayload &inventory, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_player_inventory_payload(std::span<const std::uint8_t> payload,
                                                   ProtocolVersion protocol,
                                                   PlayerInventoryPayload &inventory,
                                                   PayloadError &error);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_inventory_store_payload(
    const InventoryStorePayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_inventory_store_payload(std::span<const std::uint8_t> payload,
                                                  ProtocolVersion protocol,
                                                  InventoryStorePayload &message,
                                                  PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_inventory_retrieve_payload(
    const InventoryRetrievePayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_inventory_retrieve_payload(std::span<const std::uint8_t> payload,
                                                     ProtocolVersion protocol,
                                                     InventoryRetrievePayload &message,
                                                     PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_item_crate_payload(
    const ItemCratePayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_item_crate_payload(std::span<const std::uint8_t> payload,
                                             ProtocolVersion protocol, ItemCratePayload &message,
                                             PayloadError &error);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_world_info_payload(
    const WorldInfoPayload &world, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_world_info_payload(std::span<const std::uint8_t> payload,
                                             ProtocolVersion protocol, WorldInfoPayload &world,
                                             PayloadError &error);

} // namespace openminerz::net
