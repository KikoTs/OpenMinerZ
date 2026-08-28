#include "openminerz/net/payload_codec.hpp"

#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/dragon_codec.hpp"
#include "openminerz/net/enemy_codec.hpp"
#include "openminerz/net/misc_codec.hpp"
#include "openminerz/net/pickup_codec.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::size_t player_update_payload_size = 23U;

bool finite(Vector3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite_half(std::uint16_t value) noexcept
{
    return (value & 0x7C00U) != 0x7C00U;
}

bool stock_player_mode(std::uint8_t value, ProtocolVersion protocol) noexcept
{
    switch (protocol)
    {
    case ProtocolVersion::classic_v3:
        return value <= 16U;
    case ProtocolVersion::modern_v4:
        return value <= 18U;
    }
    return false;
}

bool valid_player_update(const PlayerUpdatePayload &message, ProtocolVersion protocol,
                         PayloadError &error) noexcept
{
    if (!finite(message.local_position) || !finite_half(message.velocity_x) ||
        !finite_half(message.velocity_y) || !finite_half(message.velocity_z))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if ((message.state_flags & ~PlayerUpdatePayload::known_state_flags) != 0U ||
        !stock_player_mode(message.player_mode, protocol) ||
        (message.packed_movement & 0x0FU) > 14U || (message.packed_movement >> 4U) > 14U)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    error = PayloadError::none;
    return true;
}

bool write_int_vector(BoundedWriter &writer, IntVector3 value)
{
    return writer.write_i32(value.x) && writer.write_i32(value.y) && writer.write_i32(value.z);
}

bool read_int_vector(BoundedReader &reader, IntVector3 &value) noexcept
{
    return reader.read_i32(value.x) && reader.read_i32(value.y) && reader.read_i32(value.z);
}

bool write_delta(BoundedWriter &writer, std::span<const std::int32_t> delta)
{
    if (delta.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        delta.size() > (maximum_payload_size - sizeof(std::int32_t)) / sizeof(std::int32_t))
        return false;
    if (!writer.write_i32(static_cast<std::int32_t>(delta.size())))
        return false;
    for (const std::int32_t value : delta)
    {
        if (!writer.write_i32(value))
            return false;
    }
    return true;
}

bool read_delta(BoundedReader &reader, std::vector<std::int32_t> &delta, PayloadError &error)
{
    error = PayloadError::none;
    std::int32_t count{};
    if (!reader.read_i32(count) || count < 0 ||
        static_cast<std::size_t>(count) > reader.remaining() / sizeof(std::int32_t))
    {
        error = count < 0 ? PayloadError::out_of_range : PayloadError::malformed;
        return false;
    }
    std::vector<std::int32_t> decoded(static_cast<std::size_t>(count));
    for (std::int32_t &value : decoded)
    {
        if (!reader.read_i32(value))
        {
            error = PayloadError::malformed;
            return false;
        }
    }
    delta = std::move(decoded);
    return true;
}

bool validate_empty(std::span<const std::uint8_t> payload, PayloadError &error) noexcept
{
    error = payload.empty() ? PayloadError::none : PayloadError::malformed;
    return payload.empty();
}

} // namespace

std::optional<std::vector<std::uint8_t>> encode_chat_payload(std::string_view message,
                                                             PayloadError &error)
{
    BoundedWriter writer{maximum_payload_size};
    if (!writer.write_dotnet_string(message, maximum_payload_size))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_chat_payload(std::span<const std::uint8_t> payload, std::string &message,
                         PayloadError &error)
{
    BoundedReader reader{payload};
    std::string decoded;
    if (!reader.read_dotnet_string(decoded, maximum_payload_size) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_time_of_day_payload(float day, PayloadError &error)
{
    if (!std::isfinite(day) || day < 0.0F || day > 10'000'000.0F)
    {
        error = !std::isfinite(day) ? PayloadError::non_finite : PayloadError::out_of_range;
        return std::nullopt;
    }
    BoundedWriter writer{sizeof(float)};
    if (!writer.write_f32(day))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_time_of_day_payload(std::span<const std::uint8_t> payload, float &day,
                                PayloadError &error)
{
    BoundedReader reader{payload};
    float decoded{};
    if (!reader.read_f32(decoded) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!std::isfinite(decoded) || decoded < 0.0F || decoded > 10'000'000.0F)
    {
        error = !std::isfinite(decoded) ? PayloadError::non_finite : PayloadError::out_of_range;
        return false;
    }
    day = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_time_connected_payload(std::int64_t ticks,
                                                                       PayloadError &error)
{
    if (ticks < 0)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    BoundedWriter writer{sizeof(ticks)};
    if (!writer.write_i64(ticks))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_time_connected_payload(std::span<const std::uint8_t> payload, std::int64_t &ticks,
                                   PayloadError &error)
{
    BoundedReader reader{payload};
    std::int64_t decoded{};
    if (!reader.read_i64(decoded) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (decoded < 0)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    ticks = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_player_exists_payload(
    const PlayerExistsPayload &message, PayloadError &error)
{
    if (message.avatar_description.size() > maximum_payload_size ||
        message.player_id.size() > maximum_payload_size ||
        message.avatar_description.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        message.player_id.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    BoundedWriter writer{maximum_payload_size};
    if (!writer.write_bool(message.request_response) ||
        !writer.write_i32(static_cast<std::int32_t>(message.avatar_description.size())) ||
        !writer.write_bytes(message.avatar_description) ||
        !writer.write_dotnet_string(message.gamer_tag, maximum_payload_size) ||
        !writer.write_i32(static_cast<std::int32_t>(message.player_id.size())) ||
        !writer.write_bytes(message.player_id))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_player_exists_payload(std::span<const std::uint8_t> payload,
                                  PlayerExistsPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    PlayerExistsPayload decoded;
    std::int32_t avatar_length{};
    std::int32_t player_id_length{};
    if (!reader.read_bool(decoded.request_response) || !reader.read_i32(avatar_length) ||
        avatar_length < 0 || static_cast<std::size_t>(avatar_length) > reader.remaining())
    {
        error = avatar_length < 0 ? PayloadError::out_of_range : PayloadError::malformed;
        return false;
    }
    decoded.avatar_description.resize(static_cast<std::size_t>(avatar_length));
    if (!reader.read_bytes(decoded.avatar_description) ||
        !reader.read_dotnet_string(decoded.gamer_tag, maximum_payload_size) ||
        !reader.read_i32(player_id_length) || player_id_length < 0 ||
        static_cast<std::size_t>(player_id_length) > reader.remaining())
    {
        error = player_id_length < 0 ? PayloadError::out_of_range : PayloadError::malformed;
        return false;
    }
    decoded.player_id.resize(static_cast<std::size_t>(player_id_length));
    if (!reader.read_bytes(decoded.player_id) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_player_update_payload(
    const PlayerUpdatePayload &message, ProtocolVersion protocol, PayloadError &error)
{
    if (!valid_player_update(message, protocol, error))
        return std::nullopt;
    BoundedWriter writer{player_update_payload_size};
    if (!writer.write_f32(message.local_position.x) ||
        !writer.write_f32(message.local_position.y) ||
        !writer.write_f32(message.local_position.z) || !writer.write_u8(message.torso_pitch) ||
        !writer.write_u8(message.yaw) || !writer.write_u8(message.state_flags) ||
        !writer.write_u8(message.player_mode) || !writer.write_u8(message.packed_movement) ||
        !writer.write_u16(message.velocity_x) || !writer.write_u16(message.velocity_y) ||
        !writer.write_u16(message.velocity_z))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_player_update_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                  PlayerUpdatePayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    PlayerUpdatePayload decoded;
    if (payload.size() != player_update_payload_size ||
        !reader.read_f32(decoded.local_position.x) || !reader.read_f32(decoded.local_position.y) ||
        !reader.read_f32(decoded.local_position.z) || !reader.read_u8(decoded.torso_pitch) ||
        !reader.read_u8(decoded.yaw) || !reader.read_u8(decoded.state_flags) ||
        !reader.read_u8(decoded.player_mode) || !reader.read_u8(decoded.packed_movement) ||
        !reader.read_u16(decoded.velocity_x) || !reader.read_u16(decoded.velocity_y) ||
        !reader.read_u16(decoded.velocity_z) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!valid_player_update(decoded, protocol, error))
        return false;
    message = decoded;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_request_chunk_payload(
    const RequestChunkPayload &message, PayloadError &error)
{
    BoundedWriter writer{13U};
    if (!write_int_vector(writer, message.block_location) || !writer.write_u8(message.priority))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_request_chunk_payload(std::span<const std::uint8_t> payload,
                                  RequestChunkPayload &message, PayloadError &error)
{
    BoundedReader reader{payload};
    RequestChunkPayload decoded;
    if (payload.size() != 13U || !read_int_vector(reader, decoded.block_location) ||
        !reader.read_u8(decoded.priority) || !reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    message = decoded;
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_delta_list_payload(
    std::span<const std::int32_t> delta, PayloadError &error)
{
    BoundedWriter writer{maximum_payload_size};
    if (!write_delta(writer, delta))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_delta_list_payload(std::span<const std::uint8_t> payload,
                               std::vector<std::int32_t> &delta, PayloadError &error)
{
    error = PayloadError::none;
    BoundedReader reader{payload};
    std::vector<std::int32_t> decoded;
    if (!read_delta(reader, decoded, error) || !reader.exhausted())
    {
        if (error == PayloadError::none)
            error = PayloadError::malformed;
        return false;
    }
    delta = std::move(decoded);
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_provide_chunk_payload(
    const ProvideChunkPayload &message, PayloadError &error)
{
    BoundedWriter writer{maximum_payload_size};
    if (!write_int_vector(writer, message.block_location) || !writer.write_u8(message.priority) ||
        !write_delta(writer, message.delta))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_provide_chunk_payload(std::span<const std::uint8_t> payload,
                                  ProvideChunkPayload &message, PayloadError &error)
{
    error = PayloadError::none;
    BoundedReader reader{payload};
    ProvideChunkPayload decoded;
    if (!read_int_vector(reader, decoded.block_location) || !reader.read_u8(decoded.priority) ||
        !read_delta(reader, decoded.delta, error) || !reader.exhausted())
    {
        if (error == PayloadError::none)
            error = PayloadError::malformed;
        return false;
    }
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

bool validate_known_payload(ProtocolVersion protocol, const MessageDescriptor &message,
                            std::span<const std::uint8_t> payload, PayloadError &error)
{
    if (message.name == "ClientReadyForChunksMessage" ||
        message.name == "RequestInventoryMessage" || message.name == "RequestWorldInfoMessage" ||
        message.name == "RemoveDragonMessage" || message.name == "RestartLevelMessage")
        return validate_empty(payload, error);
    if (message.name == "AppointServerMessage")
    {
        error = payload.size() == 1U ? PayloadError::none : PayloadError::malformed;
        return payload.size() == 1U;
    }
    if (message.name == "AddExplosionEffectsMessage" ||
        message.name == "AddExplosiveFlashMessage" || message.name == "DestroyCrateMessage")
    {
        IntVector3 ignored;
        return decode_int_vector_payload(payload, ignored, error);
    }
    if (message.name == "AlterBlockMessage")
    {
        AlterBlockPayload ignored;
        return decode_alter_block_payload(payload, protocol, ignored, error);
    }
    if (message.name == "ChatMessage" || message.name == "BroadcastTextMessage")
    {
        std::string ignored;
        return decode_chat_payload(payload, ignored, error);
    }
    if (message.name == "PlayerExistsMessage")
    {
        PlayerExistsPayload ignored;
        return decode_player_exists_payload(payload, ignored, error);
    }
    if (message.name == "PlayerUpdateMessage")
    {
        PlayerUpdatePayload ignored;
        return decode_player_update_payload(payload, protocol, ignored, error);
    }
    if (message.name == "ChangeCarriedItemMessage")
    {
        BoundedReader reader{payload};
        std::int16_t item_id{};
        if (!reader.read_i16(item_id) || !reader.exhausted())
        {
            error = PayloadError::malformed;
            return false;
        }
        const std::int16_t maximum = protocol == ProtocolVersion::classic_v3 ? 122 : 154;
        error =
            item_id >= 0 && item_id <= maximum ? PayloadError::none : PayloadError::out_of_range;
        return error == PayloadError::none;
    }
    if (message.name == "ConsumePickupMessage")
    {
        ConsumePickupPayload ignored;
        return decode_consume_pickup_payload(payload, protocol, ignored, error);
    }
    if (message.name == "CrateFocusMessage")
    {
        CrateFocusPayload ignored;
        return decode_crate_focus_payload(payload, ignored, error);
    }
    if (message.name == "CreatePickupMessage")
    {
        CreatePickupPayload ignored;
        return decode_create_pickup_payload(payload, protocol, ignored, error);
    }
    if (message.name == "DetonateExplosiveMessage")
    {
        DetonateExplosivePayload ignored;
        return decode_detonate_explosive_payload(payload, protocol, ignored, error);
    }
    if (message.name == "DetonateFireballMessage")
    {
        DetonateFireballPayload ignored;
        return decode_detonate_fireball_payload(payload, ignored, error);
    }
    if (message.name == "DetonateGrenadeMessage")
    {
        DetonateGrenadePayload ignored;
        return decode_detonate_grenade_payload(payload, protocol, ignored, error);
    }
    if (message.name == "DetonateRocketMessage")
    {
        DetonateRocketPayload ignored;
        return decode_detonate_rocket_payload(payload, protocol, ignored, error);
    }
    if (message.name == "DigMessage")
    {
        DigPayload ignored;
        return decode_dig_payload(payload, protocol, ignored, error);
    }
    if (message.name == "DoorOpenCloseMessage")
    {
        DoorOpenClosePayload ignored;
        return decode_door_open_close_payload(payload, ignored, error);
    }
    if (message.name == "DragonAttackMessage")
    {
        DragonAttackPayload ignored;
        return decode_dragon_attack_payload(payload, ignored, error);
    }
    if (message.name == "DestroyCustomBlockMessage")
    {
        if (protocol != ProtocolVersion::modern_v4)
        {
            error = PayloadError::out_of_range;
            return false;
        }
        DestroyCustomBlockPayload ignored;
        return decode_destroy_custom_block_payload(payload, ignored, error);
    }
    if (message.name == "FireRocketMessage")
    {
        FireRocketPayload ignored;
        return decode_fire_rocket_payload(payload, protocol, ignored, error);
    }
    if (message.name == "ExistingDragonMessage")
    {
        ExistingDragonPayload ignored;
        return decode_existing_dragon_payload(payload, ignored, error);
    }
    if (message.name == "EnemyGiveUpMessage" || message.name == "SpeedUpEnemyMessage")
    {
        EnemyTargetPayload ignored;
        return decode_enemy_target_payload(payload, ignored, error);
    }
    if (message.name == "GrenadeMessage")
    {
        GrenadePayload ignored;
        return decode_grenade_payload(payload, protocol, ignored, error);
    }
    if (message.name == "GunshotMessage")
    {
        GunshotPayload ignored;
        return decode_gunshot_payload(payload, protocol, ignored, error);
    }
    if (message.name == "InventoryStoreOnServerMessage")
    {
        InventoryStorePayload ignored;
        return decode_inventory_store_payload(payload, protocol, ignored, error);
    }
    if (message.name == "InventoryRetrieveFromServerMessage")
    {
        InventoryRetrievePayload ignored;
        return decode_inventory_retrieve_payload(payload, protocol, ignored, error);
    }
    if (message.name == "ItemCrateMessage")
    {
        ItemCratePayload ignored;
        return decode_item_crate_payload(payload, protocol, ignored, error);
    }
    if (message.name == "KickMessage")
    {
        KickPayload ignored;
        return decode_kick_payload(payload, ignored, error);
    }
    if (message.name == "KillDragonMessage")
    {
        KillDragonPayload ignored;
        return decode_kill_dragon_payload(payload, protocol, ignored, error);
    }
    if (message.name == "KillEnemyMessage")
    {
        KillEnemyPayload ignored;
        return decode_kill_enemy_payload(payload, protocol, ignored, error);
    }
    if (message.name == "MeleePlayerMessage")
    {
        MeleePlayerPayload ignored;
        return decode_melee_player_payload(payload, protocol, ignored, error);
    }
    if (message.name == "MigrateDragonMessage")
    {
        MigrateDragonPayload ignored;
        return decode_migrate_dragon_payload(payload, ignored, error);
    }
    if (message.name == "RemoveBlocksMessage")
    {
        RemoveBlocksPayload ignored;
        return decode_remove_blocks_payload(payload, ignored, error);
    }
    if (message.name == "RequestPickupMessage")
    {
        RequestPickupPayload ignored;
        return decode_request_pickup_payload(payload, ignored, error);
    }
    if (message.name == "RequestDragonMessage")
    {
        RequestDragonPayload ignored;
        return decode_request_dragon_payload(payload, ignored, error);
    }
    if (message.name == "RequestChunkMessage")
    {
        RequestChunkPayload ignored;
        return decode_request_chunk_payload(payload, ignored, error);
    }
    if (message.name == "ProvideDeltaListMessage")
    {
        std::vector<std::int32_t> ignored;
        return decode_delta_list_payload(payload, ignored, error);
    }
    if (message.name == "ProvideChunkMessage")
    {
        ProvideChunkPayload ignored;
        return decode_provide_chunk_payload(payload, ignored, error);
    }
    if (message.name == "ShotgunShotMessage")
    {
        ShotgunShotPayload ignored;
        return decode_shotgun_shot_payload(payload, protocol, ignored, error);
    }
    if (message.name == "SpawnEnemyMessage")
    {
        SpawnEnemyPayload ignored;
        return decode_spawn_enemy_payload(payload, protocol, ignored, error);
    }
    if (message.name == "SpawnDragonMessage")
    {
        SpawnDragonPayload ignored;
        return decode_spawn_dragon_payload(payload, ignored, error);
    }
    if (message.name == "WorldInfoMessage")
    {
        WorldInfoPayload ignored;
        return decode_world_info_payload(payload, protocol, ignored, error);
    }
    if (message.name == "TimeOfDayMessage")
    {
        float ignored{};
        return decode_time_of_day_payload(payload, ignored, error);
    }
    if (message.name == "TimeConnectedMessage")
    {
        std::int64_t ignored{};
        return decode_time_connected_payload(payload, ignored, error);
    }
    if (message.name == "UpdateSpawnerMessage")
    {
        if (protocol != ProtocolVersion::modern_v4)
        {
            error = PayloadError::out_of_range;
            return false;
        }
        UpdateSpawnerPayload ignored;
        return decode_update_spawner_payload(payload, ignored, error);
    }
    if (message.name == "UpdateDragonMessage")
    {
        DragonWaypointPayload ignored;
        return decode_update_dragon_payload(payload, ignored, error);
    }
    if (message.name == "VoiceChatMessage")
    {
        std::vector<std::uint8_t> ignored;
        return decode_voice_chat_payload(payload, ignored, error);
    }
    error = PayloadError::unsupported_message;
    return false;
}

} // namespace openminerz::net
