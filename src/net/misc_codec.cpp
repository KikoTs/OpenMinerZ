#include "openminerz/net/misc_codec.hpp"

#include <cmath>
#include <limits>
#include <utility>

namespace openminerz::net
{
namespace
{

bool finite(Vector3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool write_int_vector(BoundedWriter &writer, IntVector3 value)
{
    return writer.write_i32(value.x) && writer.write_i32(value.y) && writer.write_i32(value.z);
}

bool read_int_vector(BoundedReader &reader, IntVector3 &value) noexcept
{
    return reader.read_i32(value.x) && reader.read_i32(value.y) && reader.read_i32(value.z);
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

std::optional<std::vector<std::uint8_t>> encode_destroy_custom_block_payload(
    const DestroyCustomBlockPayload &message, PayloadError &error)
{
    if (message.block_type > 93U)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    BoundedWriter writer{13U};
    if (!write_int_vector(writer, message.location) || !writer.write_u8(message.block_type))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_destroy_custom_block_payload(std::span<const std::uint8_t> payload,
                                         DestroyCustomBlockPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    DestroyCustomBlockPayload decoded;
    if (!read_int_vector(reader, decoded.location) || !reader.read_u8(decoded.block_type) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (decoded.block_type > 93U)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_update_spawner_payload(
    const UpdateSpawnerPayload &message, PayloadError &error)
{
    if (!finite(message.spawner_position))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    BoundedWriter writer{13U};
    if (!write_vector(writer, message.spawner_position) || !writer.write_bool(message.is_started))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_update_spawner_payload(std::span<const std::uint8_t> payload,
                                   UpdateSpawnerPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    UpdateSpawnerPayload decoded;
    if (!read_vector(reader, decoded.spawner_position) || !reader.read_bool(decoded.is_started) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.spawner_position))
    {
        error = PayloadError::non_finite;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_voice_chat_payload(
    std::span<const std::uint8_t> audio, PayloadError &error)
{
    if (audio.size() > maximum_payload_size - sizeof(std::int32_t) ||
        audio.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    BoundedWriter writer{maximum_payload_size};
    if (!writer.write_i32(static_cast<std::int32_t>(audio.size())) || !writer.write_bytes(audio))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_voice_chat_payload(std::span<const std::uint8_t> payload,
                               std::vector<std::uint8_t> &audio, PayloadError &error)
{
    BoundedReader reader{payload};
    std::int32_t count{};
    if (!reader.read_i32(count))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (count < 0 || static_cast<std::size_t>(count) > reader.remaining())
    {
        error = count < 0 ? PayloadError::out_of_range : PayloadError::malformed;
        return false;
    }
    std::vector<std::uint8_t> decoded(static_cast<std::size_t>(count));
    if (!reader.read_bytes(decoded) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    audio = std::move(decoded);
    error = PayloadError::none;
    return true;
}

} // namespace openminerz::net
