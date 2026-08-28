#include "openminerz/net/enemy_codec.hpp"

#include "openminerz/net/combat_codec.hpp"

#include <cmath>
#include <utility>

namespace openminerz::net
{
namespace
{

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

bool enemy_type(std::uint8_t value, ProtocolVersion protocol) noexcept
{
    return value < (protocol == ProtocolVersion::classic_v3 ? 52U : 53U);
}

bool target_id(std::int32_t value) noexcept
{
    return value >= 0 && value <= 255;
}

bool initialization(const EnemyInitPayload &value) noexcept
{
    return value.slow_speed_milliseconds >= 0 && value.fast_speed_milliseconds >= 0 &&
           value.emerge_speed_milliseconds >= 0 && value.run_activation_milliseconds >= 0 &&
           value.normal_activation_milliseconds >= 0;
}

bool write_initialization(BoundedWriter &writer, const EnemyInitPayload &value)
{
    return writer.write_i32(value.slow_speed_milliseconds) &&
           writer.write_i32(value.fast_speed_milliseconds) &&
           writer.write_i32(value.emerge_speed_milliseconds) &&
           writer.write_i32(value.run_activation_milliseconds) &&
           writer.write_i32(value.normal_activation_milliseconds);
}

bool read_initialization(BoundedReader &reader, EnemyInitPayload &value) noexcept
{
    return reader.read_i32(value.slow_speed_milliseconds) &&
           reader.read_i32(value.fast_speed_milliseconds) &&
           reader.read_i32(value.emerge_speed_milliseconds) &&
           reader.read_i32(value.run_activation_milliseconds) &&
           reader.read_i32(value.normal_activation_milliseconds);
}

bool valid_spawn(const SpawnEnemyPayload &message, ProtocolVersion protocol,
                 PayloadError &error) noexcept
{
    if (!finite(message.spawn_position) ||
        (protocol == ProtocolVersion::modern_v4 && !finite(message.spawner_position)))
    {
        error = PayloadError::non_finite;
        return false;
    }
    const bool classic_extension = protocol == ProtocolVersion::classic_v3 &&
                                   message.spawner_position == Vector3{} &&
                                   message.spawn_value == 0 && message.player_name.empty();
    const bool modern_extension =
        protocol == ProtocolVersion::modern_v4 && message.spawn_value >= 0;
    if (!enemy_type(message.enemy_type, protocol) || message.enemy_id < 0 ||
        message.random_seed < 0 || !initialization(message.initialization) ||
        (!classic_extension && !modern_extension))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    error = PayloadError::none;
    return true;
}

} // namespace

std::optional<std::vector<std::uint8_t>> encode_spawn_enemy_payload(
    const SpawnEnemyPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!valid_spawn(message, protocol, error))
        return std::nullopt;

    BoundedWriter writer{maximum_payload_size};
    if (!write_vector(writer, message.spawn_position) ||
        (protocol == ProtocolVersion::modern_v4 &&
         !write_vector(writer, message.spawner_position)) ||
        !writer.write_u8(message.enemy_type) || !writer.write_i32(message.enemy_id) ||
        !writer.write_i32(message.random_seed) ||
        (protocol == ProtocolVersion::modern_v4 &&
         (!writer.write_i32(message.spawn_value) ||
          !writer.write_dotnet_string(message.player_name, maximum_payload_size))) ||
        !write_initialization(writer, message.initialization))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_spawn_enemy_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                SpawnEnemyPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    SpawnEnemyPayload decoded;
    if (!read_vector(reader, decoded.spawn_position) ||
        (protocol == ProtocolVersion::modern_v4 &&
         !read_vector(reader, decoded.spawner_position)) ||
        !reader.read_u8(decoded.enemy_type) || !reader.read_i32(decoded.enemy_id) ||
        !reader.read_i32(decoded.random_seed) ||
        (protocol == ProtocolVersion::modern_v4 &&
         (!reader.read_i32(decoded.spawn_value) ||
          !reader.read_dotnet_string(decoded.player_name, maximum_payload_size))) ||
        !read_initialization(reader, decoded.initialization) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!valid_spawn(decoded, protocol, error))
        return false;
    message = std::move(decoded);
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_kill_enemy_payload(const KillEnemyPayload &message,
                                                                   ProtocolVersion protocol,
                                                                   PayloadError &error)
{
    if (message.enemy_id < 0 || !target_id(message.target_id) ||
        !stock_item_id(message.weapon_id, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    BoundedWriter writer{11U};
    if (!writer.write_i32(message.enemy_id) || !writer.write_i32(message.target_id) ||
        !writer.write_i16(message.weapon_id) || !writer.write_u8(message.killer_id))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_kill_enemy_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                               KillEnemyPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    KillEnemyPayload decoded;
    if (!reader.read_i32(decoded.enemy_id) || !reader.read_i32(decoded.target_id) ||
        !reader.read_i16(decoded.weapon_id) || !reader.read_u8(decoded.killer_id) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (decoded.enemy_id < 0 || !target_id(decoded.target_id) ||
        !stock_item_id(decoded.weapon_id, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_enemy_target_payload(
    const EnemyTargetPayload &message, PayloadError &error)
{
    if (message.enemy_id < 0 || !target_id(message.target_id))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    BoundedWriter writer{8U};
    if (!writer.write_i32(message.enemy_id) || !writer.write_i32(message.target_id))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_enemy_target_payload(std::span<const std::uint8_t> payload, EnemyTargetPayload &message,
                                 PayloadError &error)
{
    BoundedReader reader{payload};
    EnemyTargetPayload decoded;
    if (!reader.read_i32(decoded.enemy_id) || !reader.read_i32(decoded.target_id) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (decoded.enemy_id < 0 || !target_id(decoded.target_id))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

} // namespace openminerz::net
