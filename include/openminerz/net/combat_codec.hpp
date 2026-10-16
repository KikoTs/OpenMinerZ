#pragma once

#include "openminerz/net/payload_codec.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openminerz::net
{

struct AlterBlockPayload final
{
    IntVector3 block_location;
    std::int32_t block_type{};
    auto operator<=>(const AlterBlockPayload &) const = default;
};

struct GunshotPayload final
{
    Vector3 direction;
    std::int16_t item_id{};
    auto operator<=>(const GunshotPayload &) const = default;
};

struct ShotgunShotPayload final
{
    static constexpr std::size_t direction_count = 5U;
    std::array<Vector3, direction_count> directions;
    std::int16_t item_id{};
    auto operator<=>(const ShotgunShotPayload &) const = default;
};

struct MeleePlayerPayload final
{
    std::int16_t item_id{};
    Vector3 damage_source;
    auto operator<=>(const MeleePlayerPayload &) const = default;
};

struct GrenadePayload final
{
    Vector3 direction;
    Vector3 position;
    std::uint8_t grenade_type{};
    float seconds_left{};
    auto operator<=>(const GrenadePayload &) const = default;
};

struct FireRocketPayload final
{
    Vector3 direction;
    Vector3 position;
    bool guided{};
    std::int16_t weapon_type{};
    auto operator<=>(const FireRocketPayload &) const = default;
};

struct DetonateGrenadePayload final
{
    Vector3 location;
    std::uint8_t grenade_type{};
    bool on_ground{};
    auto operator<=>(const DetonateGrenadePayload &) const = default;
};

struct DetonateRocketPayload final
{
    Vector3 location;
    bool hit_dragon{};
    std::uint8_t explosive_type{};
    std::uint8_t item_type{};
    auto operator<=>(const DetonateRocketPayload &) const = default;
};

struct DetonateExplosivePayload final
{
    IntVector3 location;
    bool original_explosion{};
    std::uint8_t explosive_type{};
    auto operator<=>(const DetonateExplosivePayload &) const = default;
};

struct DigPayload final
{
    bool placing{};
    Vector3 location;
    Vector3 direction;
    std::int32_t block_type{};
    auto operator<=>(const DigPayload &) const = default;
};

struct RemoveBlocksPayload final
{
    std::vector<IntVector3> blocks;
    bool do_dig_effects{};
    auto operator<=>(const RemoveBlocksPayload &) const = default;
};

struct CrateFocusPayload final
{
    IntVector3 location;
    std::int32_t item_x{};
    std::int32_t item_y{};
    auto operator<=>(const CrateFocusPayload &) const = default;
};

struct DoorOpenClosePayload final
{
    IntVector3 location;
    bool opened{};
    auto operator<=>(const DoorOpenClosePayload &) const = default;
};

struct KickPayload final
{
    bool banned{};
    std::uint8_t player_id{};
    auto operator<=>(const KickPayload &) const = default;
};

// Below the wire ceiling on purpose: one syntactically valid packet must not
// force more than a bounded amount of world/snapshot work at every recipient.
inline constexpr std::size_t maximum_remove_blocks = 4096U;
// What a RECEIVED RemoveBlocksMessage may hold. Explosive.FindBlocksToRemove
// puts a whole chain reaction into ONE message with no limit of its own (the
// original's message buffer simply grows), so a stock player's large TNT or C4
// field legitimately exceeds the 4096 cells this client chooses to send per
// message. Refusing it left thousands of blocks removed for the sender and
// intact for everybody else. The bound is what fits the native packet budget.
inline constexpr std::size_t maximum_received_remove_blocks = 21'000U;

[[nodiscard]] bool stock_block_type(std::int32_t block_type, ProtocolVersion protocol) noexcept;
[[nodiscard]] bool stock_item_id(std::int16_t item_id, ProtocolVersion protocol) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_alter_block_payload(
    const AlterBlockPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_alter_block_payload(std::span<const std::uint8_t> payload,
                                              ProtocolVersion protocol, AlterBlockPayload &message,
                                              PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_gunshot_payload(
    const GunshotPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_gunshot_payload(std::span<const std::uint8_t> payload,
                                          ProtocolVersion protocol, GunshotPayload &message,
                                          PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_shotgun_shot_payload(
    const ShotgunShotPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_shotgun_shot_payload(std::span<const std::uint8_t> payload,
                                               ProtocolVersion protocol,
                                               ShotgunShotPayload &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_melee_player_payload(
    const MeleePlayerPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_melee_player_payload(std::span<const std::uint8_t> payload,
                                               ProtocolVersion protocol,
                                               MeleePlayerPayload &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_grenade_payload(
    const GrenadePayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_grenade_payload(std::span<const std::uint8_t> payload,
                                          ProtocolVersion protocol, GrenadePayload &message,
                                          PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_fire_rocket_payload(
    const FireRocketPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_fire_rocket_payload(std::span<const std::uint8_t> payload,
                                              ProtocolVersion protocol, FireRocketPayload &message,
                                              PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_detonate_grenade_payload(
    const DetonateGrenadePayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_detonate_grenade_payload(std::span<const std::uint8_t> payload,
                                                   ProtocolVersion protocol,
                                                   DetonateGrenadePayload &message,
                                                   PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_detonate_rocket_payload(
    const DetonateRocketPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_detonate_rocket_payload(std::span<const std::uint8_t> payload,
                                                  ProtocolVersion protocol,
                                                  DetonateRocketPayload &message,
                                                  PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_detonate_explosive_payload(
    const DetonateExplosivePayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_detonate_explosive_payload(std::span<const std::uint8_t> payload,
                                                     ProtocolVersion protocol,
                                                     DetonateExplosivePayload &message,
                                                     PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_int_vector_payload(
    IntVector3 message, PayloadError &error);
[[nodiscard]] bool decode_int_vector_payload(std::span<const std::uint8_t> payload,
                                             IntVector3 &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_dig_payload(const DigPayload &message,
                                                                          ProtocolVersion protocol,
                                                                          PayloadError &error);
[[nodiscard]] bool decode_dig_payload(std::span<const std::uint8_t> payload,
                                      ProtocolVersion protocol, DigPayload &message,
                                      PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_remove_blocks_payload(
    const RemoveBlocksPayload &message, PayloadError &error);
[[nodiscard]] bool decode_remove_blocks_payload(std::span<const std::uint8_t> payload,
                                                RemoveBlocksPayload &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_crate_focus_payload(
    const CrateFocusPayload &message, PayloadError &error);
[[nodiscard]] bool decode_crate_focus_payload(std::span<const std::uint8_t> payload,
                                              CrateFocusPayload &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_door_open_close_payload(
    const DoorOpenClosePayload &message, PayloadError &error);
[[nodiscard]] bool decode_door_open_close_payload(std::span<const std::uint8_t> payload,
                                                  DoorOpenClosePayload &message,
                                                  PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_kick_payload(
    const KickPayload &message, PayloadError &error);
[[nodiscard]] bool decode_kick_payload(std::span<const std::uint8_t> payload, KickPayload &message,
                                       PayloadError &error);

} // namespace openminerz::net
