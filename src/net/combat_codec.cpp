#include "openminerz/net/combat_codec.hpp"

#include "openminerz/net/stock_parity.hpp"

#include "openminerz/net/bootstrap_codec.hpp"

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

bool grenade_type(std::uint8_t value, ProtocolVersion protocol) noexcept
{
    return value <= (protocol == ProtocolVersion::classic_v3 ? 2U : 3U);
}

bool explosive_type(std::uint8_t value, ProtocolVersion protocol) noexcept
{
    return value <= (protocol == ProtocolVersion::classic_v3 ? 4U : 5U);
}

bool gun_weapon(std::int16_t item_id, ProtocolVersion protocol) noexcept
{
    std::int32_t capacity{};
    return inventory_gun_clip_capacity(item_id, protocol, capacity);
}

bool shotgun_weapon(std::int16_t item_id, ProtocolVersion protocol) noexcept
{
    return gun_weapon(item_id, protocol) &&
           (item_id == 52 || item_id == 58 || item_id == 64 || item_id == 70 || item_id == 84 ||
            item_id == 89 || item_id == 94 || item_id == 99);
}

bool rocket_weapon(std::int16_t item_id, ProtocolVersion protocol) noexcept
{
    return gun_weapon(item_id, protocol) &&
           (item_id == 103 || item_id == 104 || item_id == 112 || item_id == 113);
}

bool gunshot_weapon(std::int16_t item_id, ProtocolVersion protocol) noexcept
{
    return gun_weapon(item_id, protocol) && !shotgun_weapon(item_id, protocol) &&
           !rocket_weapon(item_id, protocol);
}

template <typename Write>
std::optional<std::vector<std::uint8_t>> encoded(Write &&write, PayloadError &error,
                                                 std::size_t maximum = maximum_payload_size)
{
    BoundedWriter writer{maximum};
    if (!write(writer))
    {
        if (error == PayloadError::none)
            error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool fixed_end(BoundedReader &reader, PayloadError &error) noexcept
{
    if (!reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    error = PayloadError::none;
    return true;
}

} // namespace

bool stock_block_type(std::int32_t block_type, ProtocolVersion protocol) noexcept
{
    const std::int32_t maximum = protocol == ProtocolVersion::classic_v3 ? 46 : 93;
    return block_type >= 0 && block_type <= maximum;
}

bool stock_item_id(std::int16_t item_id, ProtocolVersion protocol) noexcept
{
    const std::int16_t maximum = protocol == ProtocolVersion::classic_v3 ? 122 : 154;
    return item_id >= 0 && item_id <= maximum;
}

std::optional<std::vector<std::uint8_t>> encode_alter_block_payload(
    const AlterBlockPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!stock_block_type(message.block_type, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return write_int_vector(writer, message.block_location) &&
                   writer.write_i32(message.block_type);
        },
        error, 16U);
}

bool decode_alter_block_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                AlterBlockPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    AlterBlockPayload decoded;
    if (!read_int_vector(reader, decoded.block_location) || !reader.read_i32(decoded.block_type))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!stock_block_type(decoded.block_type, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_gunshot_payload(const GunshotPayload &message,
                                                                ProtocolVersion protocol,
                                                                PayloadError &error)
{
    if (!finite(message.direction))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!gunshot_weapon(message.item_id, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return write_vector(writer, message.direction) && writer.write_i16(message.item_id);
        },
        error, 14U);
}

bool decode_gunshot_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                            GunshotPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    GunshotPayload decoded;
    if (!read_vector(reader, decoded.direction) || !reader.read_i16(decoded.item_id))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.direction))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!gunshot_weapon(decoded.item_id, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_shotgun_shot_payload(
    const ShotgunShotPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    for (const Vector3 direction : message.directions)
        if (!finite(direction))
        {
            error = PayloadError::non_finite;
            return std::nullopt;
        }
    if (!shotgun_weapon(message.item_id, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            for (const Vector3 direction : message.directions)
                if (!write_vector(writer, direction))
                    return false;
            return writer.write_i16(message.item_id);
        },
        error, 62U);
}

bool decode_shotgun_shot_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                 ShotgunShotPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    ShotgunShotPayload decoded;
    for (Vector3 &direction : decoded.directions)
        if (!read_vector(reader, direction))
        {
            error = PayloadError::malformed;
            return false;
        }
        else if (!finite(direction))
        {
            error = PayloadError::non_finite;
            return false;
        }
    if (!reader.read_i16(decoded.item_id))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!shotgun_weapon(decoded.item_id, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_melee_player_payload(
    const MeleePlayerPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!finite(message.damage_source))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!stock_item_id(message.item_id, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return writer.write_i16(message.item_id) && write_vector(writer, message.damage_source);
        },
        error, 14U);
}

bool decode_melee_player_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                 MeleePlayerPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    MeleePlayerPayload decoded;
    if (!reader.read_i16(decoded.item_id) || !read_vector(reader, decoded.damage_source))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.damage_source))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!stock_item_id(decoded.item_id, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_grenade_payload(const GrenadePayload &message,
                                                                ProtocolVersion protocol,
                                                                PayloadError &error)
{
    if (!finite(message.direction) || !finite(message.position) ||
        !std::isfinite(message.seconds_left))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!grenade_type(message.grenade_type, protocol) ||
        !stock_grenade_fuse_valid(message.seconds_left))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return write_vector(writer, message.direction) &&
                   write_vector(writer, message.position) &&
                   writer.write_u8(message.grenade_type) && writer.write_f32(message.seconds_left);
        },
        error, 29U);
}

bool decode_grenade_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                            GrenadePayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    GrenadePayload decoded;
    if (!read_vector(reader, decoded.direction) || !read_vector(reader, decoded.position) ||
        !reader.read_u8(decoded.grenade_type) || !reader.read_f32(decoded.seconds_left))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.direction) || !finite(decoded.position) ||
        !std::isfinite(decoded.seconds_left))
    {
        error = PayloadError::non_finite;
        return false;
    }
    // Not "at least one second": Player.OnUpdate lets the cook time overshoot
    // four seconds by a frame, so a held stock grenade arrives with 0.98 s or
    // less. This reader gates the packet at the transport on BOTH roles, so
    // the old bound dropped the throw before the host could even relay it.
    if (!grenade_type(decoded.grenade_type, protocol) ||
        !stock_grenade_fuse_valid(decoded.seconds_left))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_fire_rocket_payload(
    const FireRocketPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!finite(message.direction) || !finite(message.position))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!rocket_weapon(message.weapon_type, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return write_vector(writer, message.direction) &&
                   write_vector(writer, message.position) && writer.write_bool(message.guided) &&
                   writer.write_i16(message.weapon_type);
        },
        error, 27U);
}

bool decode_fire_rocket_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                FireRocketPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    FireRocketPayload decoded;
    if (!read_vector(reader, decoded.direction) || !read_vector(reader, decoded.position) ||
        !reader.read_bool(decoded.guided) || !reader.read_i16(decoded.weapon_type))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.direction) || !finite(decoded.position))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!rocket_weapon(decoded.weapon_type, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_detonate_grenade_payload(
    const DetonateGrenadePayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!finite(message.location))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!grenade_type(message.grenade_type, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return write_vector(writer, message.location) &&
                   writer.write_u8(message.grenade_type) && writer.write_bool(message.on_ground);
        },
        error, 14U);
}

bool decode_detonate_grenade_payload(std::span<const std::uint8_t> payload,
                                     ProtocolVersion protocol, DetonateGrenadePayload &message,
                                     PayloadError &error)
{
    BoundedReader reader{payload};
    DetonateGrenadePayload decoded;
    if (!read_vector(reader, decoded.location) || !reader.read_u8(decoded.grenade_type) ||
        !reader.read_bool(decoded.on_ground))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.location))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!grenade_type(decoded.grenade_type, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_detonate_rocket_payload(
    const DetonateRocketPayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!finite(message.location))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!explosive_type(message.explosive_type, protocol) ||
        !rocket_weapon(static_cast<std::int16_t>(message.item_type), protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return write_vector(writer, message.location) &&
                   writer.write_bool(message.hit_dragon) &&
                   writer.write_u8(message.explosive_type) && writer.write_u8(message.item_type);
        },
        error, 15U);
}

bool decode_detonate_rocket_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                    DetonateRocketPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    DetonateRocketPayload decoded;
    if (!read_vector(reader, decoded.location) || !reader.read_bool(decoded.hit_dragon) ||
        !reader.read_u8(decoded.explosive_type) || !reader.read_u8(decoded.item_type))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.location))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!explosive_type(decoded.explosive_type, protocol) ||
        !rocket_weapon(static_cast<std::int16_t>(decoded.item_type), protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_detonate_explosive_payload(
    const DetonateExplosivePayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!explosive_type(message.explosive_type, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return write_int_vector(writer, message.location) &&
                   writer.write_bool(message.original_explosion) &&
                   writer.write_u8(message.explosive_type);
        },
        error, 14U);
}

bool decode_detonate_explosive_payload(std::span<const std::uint8_t> payload,
                                       ProtocolVersion protocol, DetonateExplosivePayload &message,
                                       PayloadError &error)
{
    BoundedReader reader{payload};
    DetonateExplosivePayload decoded;
    if (!read_int_vector(reader, decoded.location) ||
        !reader.read_bool(decoded.original_explosion) || !reader.read_u8(decoded.explosive_type))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!explosive_type(decoded.explosive_type, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_int_vector_payload(IntVector3 message,
                                                                   PayloadError &error)
{
    return encoded([&](BoundedWriter &writer) { return write_int_vector(writer, message); }, error,
                   12U);
}

bool decode_int_vector_payload(std::span<const std::uint8_t> payload, IntVector3 &message,
                               PayloadError &error)
{
    BoundedReader reader{payload};
    IntVector3 decoded;
    if (!read_int_vector(reader, decoded))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_dig_payload(const DigPayload &message,
                                                            ProtocolVersion protocol,
                                                            PayloadError &error)
{
    if (!finite(message.location) || !finite(message.direction))
    {
        error = PayloadError::non_finite;
        return std::nullopt;
    }
    if (!stock_block_type(message.block_type, protocol))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return writer.write_bool(message.placing) && write_vector(writer, message.location) &&
                   write_vector(writer, message.direction) && writer.write_i32(message.block_type);
        },
        error, 29U);
}

bool decode_dig_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                        DigPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    DigPayload decoded;
    if (!reader.read_bool(decoded.placing) || !read_vector(reader, decoded.location) ||
        !read_vector(reader, decoded.direction) || !reader.read_i32(decoded.block_type))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!finite(decoded.location) || !finite(decoded.direction))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (!stock_block_type(decoded.block_type, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_remove_blocks_payload(
    const RemoveBlocksPayload &message, PayloadError &error)
{
    if (message.blocks.size() > maximum_remove_blocks)
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            if (!writer.write_i32(static_cast<std::int32_t>(message.blocks.size())))
                return false;
            for (const IntVector3 block : message.blocks)
                if (!write_int_vector(writer, block))
                    return false;
            return writer.write_bool(message.do_dig_effects);
        },
        error);
}

bool decode_remove_blocks_payload(std::span<const std::uint8_t> payload,
                                  RemoveBlocksPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    std::int32_t count{};
    if (!reader.read_i32(count))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (count < 0 || static_cast<std::size_t>(count) > maximum_received_remove_blocks ||
        static_cast<std::size_t>(count) > reader.remaining() / 12U)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    RemoveBlocksPayload decoded;
    decoded.blocks.resize(static_cast<std::size_t>(count));
    for (IntVector3 &block : decoded.blocks)
        if (!read_int_vector(reader, block))
        {
            error = PayloadError::malformed;
            return false;
        }
    if (!reader.read_bool(decoded.do_dig_effects) || !fixed_end(reader, error))
    {
        if (error == PayloadError::none)
            error = PayloadError::malformed;
        return false;
    }
    message = std::move(decoded);
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_crate_focus_payload(
    const CrateFocusPayload &message, PayloadError &error)
{
    if (message.item_x < 0 || message.item_x >= 8 || message.item_y < 0 || message.item_y >= 5)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    return encoded(
        [&](BoundedWriter &writer) {
            return write_int_vector(writer, message.location) && writer.write_i32(message.item_x) &&
                   writer.write_i32(message.item_y);
        },
        error, 20U);
}

bool decode_crate_focus_payload(std::span<const std::uint8_t> payload, CrateFocusPayload &message,
                                PayloadError &error)
{
    BoundedReader reader{payload};
    CrateFocusPayload decoded;
    if (!read_int_vector(reader, decoded.location) || !reader.read_i32(decoded.item_x) ||
        !reader.read_i32(decoded.item_y))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (decoded.item_x < 0 || decoded.item_x >= 8 || decoded.item_y < 0 || decoded.item_y >= 5)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_door_open_close_payload(
    const DoorOpenClosePayload &message, PayloadError &error)
{
    return encoded(
        [&](BoundedWriter &writer) {
            return write_int_vector(writer, message.location) && writer.write_bool(message.opened);
        },
        error, 13U);
}

bool decode_door_open_close_payload(std::span<const std::uint8_t> payload,
                                    DoorOpenClosePayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    DoorOpenClosePayload decoded;
    if (!read_int_vector(reader, decoded.location) || !reader.read_bool(decoded.opened))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_kick_payload(const KickPayload &message,
                                                             PayloadError &error)
{
    return encoded(
        [&](BoundedWriter &writer) {
            return writer.write_bool(message.banned) && writer.write_u8(message.player_id);
        },
        error, 2U);
}

bool decode_kick_payload(std::span<const std::uint8_t> payload, KickPayload &message,
                         PayloadError &error)
{
    BoundedReader reader{payload};
    KickPayload decoded;
    if (!reader.read_bool(decoded.banned) || !reader.read_u8(decoded.player_id))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!fixed_end(reader, error))
        return false;
    message = decoded;
    return true;
}

} // namespace openminerz::net
