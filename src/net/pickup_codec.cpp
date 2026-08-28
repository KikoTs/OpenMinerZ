#include "openminerz/net/pickup_codec.hpp"

#include <cmath>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::size_t create_prefix_size = 28U;
constexpr std::size_t consume_prefix_size = 21U;
constexpr std::size_t minimum_item_size = 8U;

bool finite(Vector3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool write_vector(BoundedWriter &writer, Vector3 value)
{
    return writer.write_f32(value.x) && writer.write_f32(value.y) && writer.write_f32(value.z);
}

bool read_vector(BoundedReader &reader, Vector3 &value) noexcept
{
    return reader.read_f32(value.x) && reader.read_f32(value.y) && reader.read_f32(value.z);
}

} // namespace

std::optional<std::vector<std::uint8_t>> encode_request_pickup_payload(
    const RequestPickupPayload &message, PayloadError &error)
{
    if (message.pickup_id < 0 || message.spawner_id < 0 || message.spawner_id > 255)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    BoundedWriter writer{8U};
    if (!writer.write_i32(message.pickup_id) || !writer.write_i32(message.spawner_id))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_request_pickup_payload(std::span<const std::uint8_t> payload,
                                   RequestPickupPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    RequestPickupPayload decoded;
    if (!reader.read_i32(decoded.pickup_id) || !reader.read_i32(decoded.spawner_id) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (decoded.pickup_id < 0 || decoded.spawner_id < 0 || decoded.spawner_id > 255)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_create_pickup_payload(
    const CreatePickupPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (message.pickup_id < 0)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    if (!finite(message.spawn_position) || !finite(message.spawn_vector))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    const auto item = encode_inventory_item_payload(message.item, protocol, error);
    const std::size_t suffix_size = protocol == ProtocolVersion::modern_v4 ? 2U : 1U;
    if (!item || create_prefix_size + item->size() + suffix_size > maximum_payload_size)
    {
        if (item)
            error = PayloadError::too_large;
        return std::nullopt;
    }
    BoundedWriter writer{maximum_payload_size};
    if (!write_vector(writer, message.spawn_position) ||
        !write_vector(writer, message.spawn_vector) || !writer.write_i32(message.pickup_id) ||
        !writer.write_bytes(*item) || !writer.write_bool(message.dropped) ||
        (protocol == ProtocolVersion::modern_v4 && !writer.write_bool(message.display_on_pickup)))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_create_pickup_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                  CreatePickupPayload &message, PayloadError &error)
{
    const std::size_t suffix_size = protocol == ProtocolVersion::modern_v4 ? 2U : 1U;
    if (payload.size() < create_prefix_size + minimum_item_size + suffix_size ||
        payload[payload.size() - suffix_size] > 1U ||
        (protocol == ProtocolVersion::modern_v4 && payload.back() > 1U))
    {
        error = PayloadError::malformed;
        return false;
    }
    BoundedReader reader{payload};
    CreatePickupPayload decoded;
    if (!read_vector(reader, decoded.spawn_position) ||
        !read_vector(reader, decoded.spawn_vector) || !reader.read_i32(decoded.pickup_id))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.spawn_position) || !finite(decoded.spawn_vector))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (decoded.pickup_id < 0)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    const auto item =
        payload.subspan(create_prefix_size, payload.size() - create_prefix_size - suffix_size);
    if (!decode_inventory_item_payload(item, protocol, decoded.item, error))
        return false;
    decoded.dropped = payload[payload.size() - suffix_size] != 0U;
    decoded.display_on_pickup = protocol == ProtocolVersion::modern_v4 && payload.back() != 0U;
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_consume_pickup_payload(
    const ConsumePickupPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (message.pickup_id < 0 || message.spawner_id < 0 || message.spawner_id > 255)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    if (!finite(message.pickup_position))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    const auto item = encode_inventory_item_payload(message.item, protocol, error);
    const std::size_t prefix_size =
        consume_prefix_size + (protocol == ProtocolVersion::modern_v4 ? 1U : 0U);
    if (!item || prefix_size + item->size() > maximum_payload_size)
    {
        if (item)
            error = PayloadError::too_large;
        return std::nullopt;
    }
    BoundedWriter writer{maximum_payload_size};
    if (!write_vector(writer, message.pickup_position) || !writer.write_i32(message.pickup_id) ||
        !writer.write_i32(message.spawner_id) || !writer.write_u8(message.picker_upper) ||
        (protocol == ProtocolVersion::modern_v4 && !writer.write_bool(message.display_on_pickup)) ||
        !writer.write_bytes(*item))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_consume_pickup_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                   ConsumePickupPayload &message, PayloadError &error)
{
    const std::size_t item_offset =
        consume_prefix_size + (protocol == ProtocolVersion::modern_v4 ? 1U : 0U);
    if (payload.size() < item_offset + minimum_item_size)
    {
        error = PayloadError::malformed;
        return false;
    }
    BoundedReader reader{payload};
    ConsumePickupPayload decoded;
    if (!read_vector(reader, decoded.pickup_position) || !reader.read_i32(decoded.pickup_id) ||
        !reader.read_i32(decoded.spawner_id) || !reader.read_u8(decoded.picker_upper))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (protocol == ProtocolVersion::modern_v4 && !reader.read_bool(decoded.display_on_pickup))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.pickup_position))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (decoded.pickup_id < 0 || decoded.spawner_id < 0 || decoded.spawner_id > 255)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!decode_inventory_item_payload(payload.subspan(item_offset), protocol, decoded.item, error))
        return false;
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

} // namespace openminerz::net
