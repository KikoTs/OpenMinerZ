#pragma once

#include "openminerz/net/payload_codec.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openminerz::net
{

struct EnemyInitPayload final
{
    std::int32_t slow_speed_milliseconds{};
    std::int32_t fast_speed_milliseconds{};
    std::int32_t emerge_speed_milliseconds{};
    std::int32_t run_activation_milliseconds{};
    std::int32_t normal_activation_milliseconds{};

    auto operator<=>(const EnemyInitPayload &) const = default;
};

struct SpawnEnemyPayload final
{
    Vector3 spawn_position;
    std::uint8_t enemy_type{};
    std::int32_t enemy_id{};
    std::int32_t random_seed{};
    EnemyInitPayload initialization;
    Vector3 spawner_position;
    std::int32_t spawn_value{};
    std::string player_name;

    auto operator<=>(const SpawnEnemyPayload &) const = default;
};

struct KillEnemyPayload final
{
    std::int32_t enemy_id{};
    std::int32_t target_id{};
    std::int16_t weapon_id{};
    std::uint8_t killer_id{};

    auto operator<=>(const KillEnemyPayload &) const = default;
};

struct EnemyTargetPayload final
{
    std::int32_t enemy_id{};
    std::int32_t target_id{};

    auto operator<=>(const EnemyTargetPayload &) const = default;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_spawn_enemy_payload(
    const SpawnEnemyPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_spawn_enemy_payload(std::span<const std::uint8_t> payload,
                                              ProtocolVersion protocol, SpawnEnemyPayload &message,
                                              PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_kill_enemy_payload(
    const KillEnemyPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_kill_enemy_payload(std::span<const std::uint8_t> payload,
                                             ProtocolVersion protocol, KillEnemyPayload &message,
                                             PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_enemy_target_payload(
    const EnemyTargetPayload &message, PayloadError &error);
[[nodiscard]] bool decode_enemy_target_payload(std::span<const std::uint8_t> payload,
                                               EnemyTargetPayload &message, PayloadError &error);

} // namespace openminerz::net
