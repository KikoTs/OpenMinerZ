#include "openminerz/net/dragon_codec.hpp"

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

bool write_int_vector(BoundedWriter &writer, IntVector3 value)
{
    return writer.write_i32(value.x) && writer.write_i32(value.y) && writer.write_i32(value.z);
}

bool read_int_vector(BoundedReader &reader, IntVector3 &value) noexcept
{
    return reader.read_i32(value.x) && reader.read_i32(value.y) && reader.read_i32(value.z);
}

bool dragon_type(std::uint8_t value) noexcept
{
    return value < 5U;
}

bool health(float value) noexcept
{
    return std::isfinite(value) && (value == -1.0F || value >= 0.0F);
}

bool waypoint(const DragonWaypointPayload &value, PayloadError &error) noexcept
{
    if (!finite(value.position) || !finite(value.velocity) || !std::isfinite(value.host_time) ||
        !std::isfinite(value.target_roll))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (value.animation >= 4U || value.sound >= 4U)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    error = PayloadError::none;
    return true;
}

bool write_waypoint(BoundedWriter &writer, const DragonWaypointPayload &value)
{
    return write_vector(writer, value.position) && write_vector(writer, value.velocity) &&
           writer.write_f32(value.host_time) && writer.write_f32(value.target_roll) &&
           writer.write_u8(value.animation) && writer.write_u8(value.sound);
}

bool read_waypoint(BoundedReader &reader, DragonWaypointPayload &value) noexcept
{
    return read_vector(reader, value.position) && read_vector(reader, value.velocity) &&
           reader.read_f32(value.host_time) && reader.read_f32(value.target_roll) &&
           reader.read_u8(value.animation) && reader.read_u8(value.sound);
}

bool migration(const DragonMigrationPayload &value, PayloadError &error) noexcept
{
    const bool all_finite =
        std::isfinite(value.next_dragon_time) && std::isfinite(value.roll) &&
        std::isfinite(value.target_roll) && std::isfinite(value.pitch) &&
        std::isfinite(value.target_pitch) && std::isfinite(value.yaw) &&
        std::isfinite(value.target_yaw) && std::isfinite(value.velocity) &&
        std::isfinite(value.target_velocity) && std::isfinite(value.default_heading) &&
        std::isfinite(value.flap_debt) && std::isfinite(value.next_update_time) &&
        finite(value.position) && finite(value.target);
    if (!all_finite)
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (value.next_fireball_index < 0 || value.animation >= 4U || !dragon_type(value.dragon_type))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    error = PayloadError::none;
    return true;
}

bool write_migration(BoundedWriter &writer, const DragonMigrationPayload &value)
{
    return writer.write_f32(value.next_dragon_time) && writer.write_f32(value.roll) &&
           writer.write_f32(value.target_roll) && writer.write_f32(value.pitch) &&
           writer.write_f32(value.target_pitch) && writer.write_f32(value.yaw) &&
           writer.write_f32(value.target_yaw) && writer.write_f32(value.velocity) &&
           writer.write_f32(value.target_velocity) && writer.write_f32(value.default_heading) &&
           writer.write_f32(value.flap_debt) && writer.write_i32(value.next_fireball_index) &&
           writer.write_bool(value.for_biome) && writer.write_f32(value.next_update_time) &&
           writer.write_u8(value.animation) && writer.write_u8(value.dragon_type) &&
           write_vector(writer, value.position) && write_vector(writer, value.target);
}

bool read_migration(BoundedReader &reader, DragonMigrationPayload &value) noexcept
{
    return reader.read_f32(value.next_dragon_time) && reader.read_f32(value.roll) &&
           reader.read_f32(value.target_roll) && reader.read_f32(value.pitch) &&
           reader.read_f32(value.target_pitch) && reader.read_f32(value.yaw) &&
           reader.read_f32(value.target_yaw) && reader.read_f32(value.velocity) &&
           reader.read_f32(value.target_velocity) && reader.read_f32(value.default_heading) &&
           reader.read_f32(value.flap_debt) && reader.read_i32(value.next_fireball_index) &&
           reader.read_bool(value.for_biome) && reader.read_f32(value.next_update_time) &&
           reader.read_u8(value.animation) && reader.read_u8(value.dragon_type) &&
           read_vector(reader, value.position) && read_vector(reader, value.target);
}

template <typename Write>
std::optional<std::vector<std::uint8_t>> encode_fixed(std::size_t size, Write &&write,
                                                      PayloadError &error)
{
    BoundedWriter writer{size};
    if (!write(writer))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

} // namespace

std::optional<std::vector<std::uint8_t>> encode_spawn_dragon_payload(
    const SpawnDragonPayload &message, PayloadError &error)
{
    if (!std::isfinite(message.health))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!dragon_type(message.dragon_type) || !health(message.health))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encode_fixed(
        7U,
        [&](BoundedWriter &writer) {
            return writer.write_u8(message.dragon_type) && writer.write_u8(message.spawner_gid) &&
                   writer.write_bool(message.for_biome) && writer.write_f32(message.health);
        },
        error);
}

bool decode_spawn_dragon_payload(std::span<const std::uint8_t> payload, SpawnDragonPayload &message,
                                 PayloadError &error)
{
    BoundedReader reader{payload};
    SpawnDragonPayload decoded;
    if (!reader.read_u8(decoded.dragon_type) || !reader.read_u8(decoded.spawner_gid) ||
        !reader.read_bool(decoded.for_biome) || !reader.read_f32(decoded.health) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!std::isfinite(decoded.health))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!dragon_type(decoded.dragon_type) || !health(decoded.health))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_request_dragon_payload(
    const RequestDragonPayload &message, PayloadError &error)
{
    if (!dragon_type(message.dragon_type))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encode_fixed(
        2U,
        [&](BoundedWriter &writer) {
            return writer.write_u8(message.dragon_type) && writer.write_bool(message.for_biome);
        },
        error);
}

bool decode_request_dragon_payload(std::span<const std::uint8_t> payload,
                                   RequestDragonPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    RequestDragonPayload decoded;
    if (!reader.read_u8(decoded.dragon_type) || !reader.read_bool(decoded.for_biome) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!dragon_type(decoded.dragon_type))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_existing_dragon_payload(
    const ExistingDragonPayload &message, PayloadError &error)
{
    return encode_spawn_dragon_payload(
        {message.dragon_type, message.new_client_gid, message.for_biome, message.current_health},
        error);
}

bool decode_existing_dragon_payload(std::span<const std::uint8_t> payload,
                                    ExistingDragonPayload &message, PayloadError &error)
{
    SpawnDragonPayload decoded;
    if (!decode_spawn_dragon_payload(payload, decoded, error))
        return false;
    message = {decoded.dragon_type, decoded.spawner_gid, decoded.for_biome, decoded.health};
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_update_dragon_payload(
    const DragonWaypointPayload &message, PayloadError &error)
{
    if (!waypoint(message, error))
        return std::nullopt;
    return encode_fixed(
        34U, [&](BoundedWriter &writer) { return write_waypoint(writer, message); }, error);
}

bool decode_update_dragon_payload(std::span<const std::uint8_t> payload,
                                  DragonWaypointPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    DragonWaypointPayload decoded;
    if (!read_waypoint(reader, decoded) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!waypoint(decoded, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_dragon_attack_payload(
    const DragonAttackPayload &message, PayloadError &error)
{
    if (!waypoint(message.waypoint, error))
        return std::nullopt;
    if (!finite(message.target))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    return encode_fixed(
        51U,
        [&](BoundedWriter &writer) {
            return write_waypoint(writer, message.waypoint) &&
                   write_vector(writer, message.target) &&
                   writer.write_bool(message.animated_attack) &&
                   writer.write_i32(message.fireball_index);
        },
        error);
}

bool decode_dragon_attack_payload(std::span<const std::uint8_t> payload,
                                  DragonAttackPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    DragonAttackPayload decoded;
    if (!read_waypoint(reader, decoded.waypoint) || !read_vector(reader, decoded.target) ||
        !reader.read_bool(decoded.animated_attack) || !reader.read_i32(decoded.fireball_index) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!waypoint(decoded.waypoint, error))
        return false;
    if (!finite(decoded.target))
    {
        error = PayloadError::non_finite;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_migrate_dragon_payload(
    const MigrateDragonPayload &message, PayloadError &error)
{
    if (!migration(message.migration, error))
        return std::nullopt;
    return encode_fixed(
        80U,
        [&](BoundedWriter &writer) {
            return write_migration(writer, message.migration) &&
                   writer.write_u8(message.target_gid);
        },
        error);
}

bool decode_migrate_dragon_payload(std::span<const std::uint8_t> payload,
                                   MigrateDragonPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    MigrateDragonPayload decoded;
    if (!read_migration(reader, decoded.migration) || !reader.read_u8(decoded.target_gid) ||
        !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!migration(decoded.migration, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_kill_dragon_payload(
    const KillDragonPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!finite(message.location))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!stock_item_id(message.weapon_id, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encode_fixed(
        15U,
        [&](BoundedWriter &writer) {
            return write_vector(writer, message.location) && writer.write_i16(message.weapon_id) &&
                   writer.write_u8(message.killer_gid);
        },
        error);
}

bool decode_kill_dragon_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                KillDragonPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    KillDragonPayload decoded;
    if (!read_vector(reader, decoded.location) || !reader.read_i16(decoded.weapon_id) ||
        !reader.read_u8(decoded.killer_gid) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.location))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!stock_item_id(decoded.weapon_id, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_detonate_fireball_payload(
    const DetonateFireballPayload &message, PayloadError &error)
{
    if (!finite(message.location))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!dragon_type(message.dragon_type) ||
        message.blocks_to_remove.size() > maximum_fireball_blocks)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    BoundedWriter writer{maximum_payload_size};
    if (!write_vector(writer, message.location) || !writer.write_i32(message.fireball_index) ||
        !writer.write_u8(message.dragon_type) ||
        !writer.write_u8(static_cast<std::uint8_t>(message.blocks_to_remove.size())))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    for (const IntVector3 block : message.blocks_to_remove)
    {
        if (!write_int_vector(writer, block))
        {
            error = PayloadError::too_large;
            return std::nullopt;
        }
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_detonate_fireball_payload(std::span<const std::uint8_t> payload,
                                      DetonateFireballPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    DetonateFireballPayload decoded;
    std::uint8_t count{};
    if (!read_vector(reader, decoded.location) || !reader.read_i32(decoded.fireball_index) ||
        !reader.read_u8(decoded.dragon_type) || !reader.read_u8(count))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.location))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!dragon_type(decoded.dragon_type))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (reader.remaining() != static_cast<std::size_t>(count) * 12U)
    {
        error = PayloadError::malformed;
        return false;
    }
    decoded.blocks_to_remove.resize(count);
    for (IntVector3 &block : decoded.blocks_to_remove)
    {
        if (!read_int_vector(reader, block))
        {
            error = PayloadError::malformed;
            return false;
        }
    }
    if (!reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

} // namespace openminerz::net
