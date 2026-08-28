#pragma once

#include "openminerz/net/protocol.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openminerz::net
{

struct IntVector3 final
{
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    auto operator<=>(const IntVector3 &) const = default;
};

struct Vector3 final
{
    float x{};
    float y{};
    float z{};

    auto operator<=>(const Vector3 &) const = default;
};

struct PlayerExistsPayload final
{
    bool request_response{};
    std::vector<std::uint8_t> avatar_description;
    std::string gamer_tag;
    std::vector<std::uint8_t> player_id;

    auto operator<=>(const PlayerExistsPayload &) const = default;
};

struct PlayerUpdatePayload final
{
    static constexpr std::uint8_t using_flag = 1U << 0U;
    static constexpr std::uint8_t dead_flag = 1U << 1U;
    static constexpr std::uint8_t shouldering_flag = 1U << 2U;
    static constexpr std::uint8_t reloading_flag = 1U << 3U;
    static constexpr std::uint8_t throwing_grenade_flag = 1U << 4U;
    static constexpr std::uint8_t ready_to_throw_grenade_flag = 1U << 5U;
    static constexpr std::uint8_t known_state_flags = using_flag | dead_flag | shouldering_flag |
                                                      reloading_flag | throwing_grenade_flag |
                                                      ready_to_throw_grenade_flag;

    Vector3 local_position;
    std::uint8_t torso_pitch{};
    std::uint8_t yaw{};
    std::uint8_t state_flags{};
    std::uint8_t player_mode{};
    std::uint8_t packed_movement{};
    std::uint16_t velocity_x{};
    std::uint16_t velocity_y{};
    std::uint16_t velocity_z{};

    auto operator<=>(const PlayerUpdatePayload &) const = default;
};

struct RequestChunkPayload final
{
    IntVector3 block_location;
    std::uint8_t priority{};

    auto operator<=>(const RequestChunkPayload &) const = default;
};

struct ProvideChunkPayload final
{
    IntVector3 block_location;
    std::uint8_t priority{};
    std::vector<std::int32_t> delta;

    auto operator<=>(const ProvideChunkPayload &) const = default;
};

enum class PayloadError : std::uint8_t
{
    none,
    unsupported_message,
    malformed,
    non_finite,
    out_of_range,
    too_large
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_chat_payload(std::string_view message,
                                                                           PayloadError &error);
[[nodiscard]] bool decode_chat_payload(std::span<const std::uint8_t> payload, std::string &message,
                                       PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_time_of_day_payload(
    float day, PayloadError &error);
[[nodiscard]] bool decode_time_of_day_payload(std::span<const std::uint8_t> payload, float &day,
                                              PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_time_connected_payload(
    std::int64_t ticks, PayloadError &error);
[[nodiscard]] bool decode_time_connected_payload(std::span<const std::uint8_t> payload,
                                                 std::int64_t &ticks, PayloadError &error);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_player_exists_payload(
    const PlayerExistsPayload &message, PayloadError &error);
[[nodiscard]] bool decode_player_exists_payload(std::span<const std::uint8_t> payload,
                                                PlayerExistsPayload &message, PayloadError &error);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_player_update_payload(
    const PlayerUpdatePayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_player_update_payload(std::span<const std::uint8_t> payload,
                                                ProtocolVersion protocol,
                                                PlayerUpdatePayload &message, PayloadError &error);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_request_chunk_payload(
    const RequestChunkPayload &message, PayloadError &error);
[[nodiscard]] bool decode_request_chunk_payload(std::span<const std::uint8_t> payload,
                                                RequestChunkPayload &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_delta_list_payload(
    std::span<const std::int32_t> delta, PayloadError &error);
[[nodiscard]] bool decode_delta_list_payload(std::span<const std::uint8_t> payload,
                                             std::vector<std::int32_t> &delta, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_provide_chunk_payload(
    const ProvideChunkPayload &message, PayloadError &error);
[[nodiscard]] bool decode_provide_chunk_payload(std::span<const std::uint8_t> payload,
                                                ProvideChunkPayload &message, PayloadError &error);

[[nodiscard]] bool validate_known_payload(ProtocolVersion protocol,
                                          const MessageDescriptor &message,
                                          std::span<const std::uint8_t> payload,
                                          PayloadError &error);

} // namespace openminerz::net
