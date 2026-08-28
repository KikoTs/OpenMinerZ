#pragma once

#include "openminerz/net/payload_codec.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openminerz::net
{

struct DragonWaypointPayload final
{
    Vector3 position;
    Vector3 velocity;
    float host_time{};
    float target_roll{};
    std::uint8_t animation{};
    std::uint8_t sound{};

    auto operator<=>(const DragonWaypointPayload &) const = default;
};

struct SpawnDragonPayload final
{
    std::uint8_t dragon_type{};
    std::uint8_t spawner_gid{};
    bool for_biome{};
    float health{};

    auto operator<=>(const SpawnDragonPayload &) const = default;
};

struct RequestDragonPayload final
{
    std::uint8_t dragon_type{};
    bool for_biome{};

    auto operator<=>(const RequestDragonPayload &) const = default;
};

struct ExistingDragonPayload final
{
    std::uint8_t dragon_type{};
    std::uint8_t new_client_gid{};
    bool for_biome{};
    float current_health{};

    auto operator<=>(const ExistingDragonPayload &) const = default;
};

struct DragonAttackPayload final
{
    DragonWaypointPayload waypoint;
    Vector3 target;
    bool animated_attack{};
    std::int32_t fireball_index{};

    auto operator<=>(const DragonAttackPayload &) const = default;
};

struct DragonMigrationPayload final
{
    float next_dragon_time{};
    float roll{};
    float target_roll{};
    float pitch{};
    float target_pitch{};
    float yaw{};
    float target_yaw{};
    float velocity{};
    float target_velocity{};
    float default_heading{};
    float flap_debt{};
    std::int32_t next_fireball_index{};
    bool for_biome{};
    float next_update_time{};
    std::uint8_t animation{};
    std::uint8_t dragon_type{};
    Vector3 position;
    Vector3 target;

    auto operator<=>(const DragonMigrationPayload &) const = default;
};

struct MigrateDragonPayload final
{
    DragonMigrationPayload migration;
    std::uint8_t target_gid{};

    auto operator<=>(const MigrateDragonPayload &) const = default;
};

struct KillDragonPayload final
{
    Vector3 location;
    std::int16_t weapon_id{};
    std::uint8_t killer_gid{};

    auto operator<=>(const KillDragonPayload &) const = default;
};

struct DetonateFireballPayload final
{
    Vector3 location;
    std::int32_t fireball_index{};
    std::uint8_t dragon_type{};
    std::vector<IntVector3> blocks_to_remove;

    auto operator<=>(const DetonateFireballPayload &) const = default;
};

inline constexpr std::size_t maximum_fireball_blocks = 255U;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_spawn_dragon_payload(
    const SpawnDragonPayload &message, PayloadError &error);
[[nodiscard]] bool decode_spawn_dragon_payload(std::span<const std::uint8_t> payload,
                                               SpawnDragonPayload &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_request_dragon_payload(
    const RequestDragonPayload &message, PayloadError &error);
[[nodiscard]] bool decode_request_dragon_payload(std::span<const std::uint8_t> payload,
                                                 RequestDragonPayload &message,
                                                 PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_existing_dragon_payload(
    const ExistingDragonPayload &message, PayloadError &error);
[[nodiscard]] bool decode_existing_dragon_payload(std::span<const std::uint8_t> payload,
                                                  ExistingDragonPayload &message,
                                                  PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_update_dragon_payload(
    const DragonWaypointPayload &message, PayloadError &error);
[[nodiscard]] bool decode_update_dragon_payload(std::span<const std::uint8_t> payload,
                                                DragonWaypointPayload &message,
                                                PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_dragon_attack_payload(
    const DragonAttackPayload &message, PayloadError &error);
[[nodiscard]] bool decode_dragon_attack_payload(std::span<const std::uint8_t> payload,
                                                DragonAttackPayload &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_migrate_dragon_payload(
    const MigrateDragonPayload &message, PayloadError &error);
[[nodiscard]] bool decode_migrate_dragon_payload(std::span<const std::uint8_t> payload,
                                                 MigrateDragonPayload &message,
                                                 PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_kill_dragon_payload(
    const KillDragonPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_kill_dragon_payload(std::span<const std::uint8_t> payload,
                                              ProtocolVersion protocol, KillDragonPayload &message,
                                              PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_detonate_fireball_payload(
    const DetonateFireballPayload &message, PayloadError &error);
[[nodiscard]] bool decode_detonate_fireball_payload(std::span<const std::uint8_t> payload,
                                                    DetonateFireballPayload &message,
                                                    PayloadError &error);

} // namespace openminerz::net
