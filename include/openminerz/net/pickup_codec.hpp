#pragma once

#include "openminerz/net/bootstrap_codec.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openminerz::net
{

struct RequestPickupPayload final
{
    std::int32_t pickup_id{};
    std::int32_t spawner_id{};
    auto operator<=>(const RequestPickupPayload &) const = default;
};

struct CreatePickupPayload final
{
    Vector3 spawn_position;
    Vector3 spawn_vector;
    std::int32_t pickup_id{};
    InventoryItemPayload item;
    bool dropped{};
    bool display_on_pickup{};
    auto operator<=>(const CreatePickupPayload &) const = default;
};

struct ConsumePickupPayload final
{
    Vector3 pickup_position;
    std::int32_t pickup_id{};
    std::int32_t spawner_id{};
    std::uint8_t picker_upper{};
    InventoryItemPayload item;
    bool display_on_pickup{};
    auto operator<=>(const ConsumePickupPayload &) const = default;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_request_pickup_payload(
    const RequestPickupPayload &message, PayloadError &error);
[[nodiscard]] bool decode_request_pickup_payload(std::span<const std::uint8_t> payload,
                                                 RequestPickupPayload &message,
                                                 PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_create_pickup_payload(
    const CreatePickupPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_create_pickup_payload(std::span<const std::uint8_t> payload,
                                                ProtocolVersion protocol,
                                                CreatePickupPayload &message, PayloadError &error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_consume_pickup_payload(
    const ConsumePickupPayload &message, ProtocolVersion protocol, PayloadError &error);
[[nodiscard]] bool decode_consume_pickup_payload(std::span<const std::uint8_t> payload,
                                                 ProtocolVersion protocol,
                                                 ConsumePickupPayload &message,
                                                 PayloadError &error);

} // namespace openminerz::net
