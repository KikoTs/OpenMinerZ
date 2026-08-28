#pragma once

#include "openminerz/net/payload_codec.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openminerz::net
{

struct DestroyCustomBlockPayload final
{
    IntVector3 location;
    std::uint8_t block_type{};
    auto operator<=>(const DestroyCustomBlockPayload &) const = default;
};

struct UpdateSpawnerPayload final
{
    Vector3 spawner_position;
    bool is_started{};
    auto operator<=>(const UpdateSpawnerPayload &) const = default;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_destroy_custom_block_payload(
    const DestroyCustomBlockPayload &message, PayloadError &error);
[[nodiscard]] bool decode_destroy_custom_block_payload(std::span<const std::uint8_t> payload,
                                                       DestroyCustomBlockPayload &message,
                                                       PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_update_spawner_payload(
    const UpdateSpawnerPayload &message, PayloadError &error);
[[nodiscard]] bool decode_update_spawner_payload(std::span<const std::uint8_t> payload,
                                                 UpdateSpawnerPayload &message,
                                                 PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_voice_chat_payload(
    std::span<const std::uint8_t> audio, PayloadError &error);
[[nodiscard]] bool decode_voice_chat_payload(std::span<const std::uint8_t> payload,
                                             std::vector<std::uint8_t> &audio, PayloadError &error);

} // namespace openminerz::net
