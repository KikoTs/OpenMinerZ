#include "openminerz/net/bootstrap_codec.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::string_view inventory_identifier = "PINV";
constexpr std::int32_t classic_inventory_version = 1;
constexpr std::int32_t modern_inventory_version = 4;
constexpr std::int64_t maximum_datetime_ticks = 3'155'378'975'999'999'999LL;

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

bool registered_item_id(std::int16_t item_id, ProtocolVersion protocol) noexcept
{
    switch (protocol)
    {
    case ProtocolVersion::classic_v3:
        return item_id >= 0 && item_id <= 122;
    case ProtocolVersion::modern_v4:
        return (item_id >= 0 && item_id <= 123) || (item_id >= 126 && item_id <= 133) ||
               (item_id >= 137 && item_id <= 141) || item_id == 143 ||
               (item_id >= 147 && item_id <= 149) || item_id == 151 ||
               (item_id >= 153 && item_id <= 154);
    }
    return false;
}

bool gun_item_id(std::int16_t item_id, ProtocolVersion protocol) noexcept
{
    const bool common = (item_id >= 50 && item_id <= 54) || (item_id >= 56 && item_id <= 60) ||
                        (item_id >= 62 && item_id <= 66) || (item_id >= 68 && item_id <= 72) ||
                        (item_id >= 82 && item_id <= 101) || item_id == 103 || item_id == 104 ||
                        item_id == 112 || item_id == 113 || (item_id >= 119 && item_id <= 122);
    return common || (protocol == ProtocolVersion::modern_v4 &&
                      (item_id == 141 || item_id == 143 || item_id == 149));
}

bool valid_item(const InventoryItemPayload &item, ProtocolVersion protocol,
                PayloadError &error) noexcept
{
    std::int32_t stack_limit{};
    if (!inventory_item_stack_limit(item.item_id, protocol, stack_limit) || item.stack_count <= 0 ||
        item.stack_count > stack_limit)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!std::isfinite(item.health))
    {
        error = PayloadError::non_finite;
        return false;
    }
    if (item.health < 0.0F || item.health > 1.0F)
    {
        error = PayloadError::out_of_range;
        return false;
    }

    const InventoryItemSerializationKind kind =
        inventory_item_serialization_kind(item.item_id, protocol);
    switch (kind)
    {
    case InventoryItemSerializationKind::base:
        if (item.rounds_in_clip || item.point_to_location || item.custom_name)
        {
            error = PayloadError::out_of_range;
            return false;
        }
        break;
    case InventoryItemSerializationKind::gun: {
        std::int32_t clip_capacity{};
        if (item.stack_count != 1 || !item.rounds_in_clip ||
            !inventory_gun_clip_capacity(item.item_id, protocol, clip_capacity) ||
            *item.rounds_in_clip < 0 || *item.rounds_in_clip > clip_capacity ||
            item.point_to_location || item.custom_name)
        {
            error = PayloadError::out_of_range;
            return false;
        }
        break;
    }
    case InventoryItemSerializationKind::locator:
    case InventoryItemSerializationKind::teleport_block:
        if (item.rounds_in_clip || !item.point_to_location || !finite(*item.point_to_location) ||
            !item.custom_name)
        {
            error = item.point_to_location && !finite(*item.point_to_location)
                        ? PayloadError::non_finite
                        : PayloadError::out_of_range;
            return false;
        }
        break;
    case InventoryItemSerializationKind::spawn_block:
        if (item.rounds_in_clip || !item.point_to_location || !finite(*item.point_to_location) ||
            item.custom_name)
        {
            error = item.point_to_location && !finite(*item.point_to_location)
                        ? PayloadError::non_finite
                        : PayloadError::out_of_range;
            return false;
        }
        break;
    case InventoryItemSerializationKind::invalid:
        error = PayloadError::out_of_range;
        return false;
    }
    error = PayloadError::none;
    return true;
}

bool write_item(BoundedWriter &writer, const InventoryItemPayload &item, ProtocolVersion protocol,
                PayloadError &error)
{
    if (!valid_item(item, protocol, error))
        return false;
    if (!writer.write_i16(item.item_id) || !writer.write_i16(item.stack_count) ||
        !writer.write_f32(item.health))
    {
        error = PayloadError::too_large;
        return false;
    }

    const InventoryItemSerializationKind kind =
        inventory_item_serialization_kind(item.item_id, protocol);
    if (kind == InventoryItemSerializationKind::gun)
    {
        if (!writer.write_i32(*item.rounds_in_clip))
        {
            error = PayloadError::too_large;
            return false;
        }
    }
    else if (kind == InventoryItemSerializationKind::locator ||
             kind == InventoryItemSerializationKind::spawn_block ||
             kind == InventoryItemSerializationKind::teleport_block)
    {
        if (!write_vector(writer, *item.point_to_location) ||
            ((kind == InventoryItemSerializationKind::locator ||
              kind == InventoryItemSerializationKind::teleport_block) &&
             !writer.write_dotnet_string(*item.custom_name, maximum_payload_size)))
        {
            error = PayloadError::too_large;
            return false;
        }
    }
    error = PayloadError::none;
    return true;
}

bool read_item(BoundedReader &reader, ProtocolVersion protocol, InventoryItemPayload &item,
               PayloadError &error)
{
    InventoryItemPayload decoded;
    if (!reader.read_i16(decoded.item_id) || !reader.read_i16(decoded.stack_count) ||
        !reader.read_f32(decoded.health))
    {
        error = PayloadError::malformed;
        return false;
    }

    const InventoryItemSerializationKind kind =
        inventory_item_serialization_kind(decoded.item_id, protocol);
    if (kind == InventoryItemSerializationKind::gun)
    {
        std::int32_t rounds{};
        if (!reader.read_i32(rounds))
        {
            error = PayloadError::malformed;
            return false;
        }
        decoded.rounds_in_clip = rounds;
    }
    else if (kind == InventoryItemSerializationKind::locator ||
             kind == InventoryItemSerializationKind::spawn_block ||
             kind == InventoryItemSerializationKind::teleport_block)
    {
        Vector3 point;
        if (!read_vector(reader, point))
        {
            error = PayloadError::malformed;
            return false;
        }
        decoded.point_to_location = point;
        if (kind == InventoryItemSerializationKind::locator ||
            kind == InventoryItemSerializationKind::teleport_block)
        {
            std::string name;
            if (!reader.read_dotnet_string(name, maximum_payload_size))
            {
                error = PayloadError::malformed;
                return false;
            }
            decoded.custom_name = std::move(name);
        }
    }
    if (!valid_item(decoded, protocol, error))
        return false;
    item = std::move(decoded);
    return true;
}

bool write_optional_item(BoundedWriter &writer, const InventorySlotPayload &slot,
                         ProtocolVersion protocol, PayloadError &error)
{
    if (!writer.write_bool(slot.has_value()))
    {
        error = PayloadError::too_large;
        return false;
    }
    return !slot || write_item(writer, *slot, protocol, error);
}

bool read_optional_item(BoundedReader &reader, ProtocolVersion protocol, InventorySlotPayload &slot,
                        PayloadError &error)
{
    bool present{};
    if (!reader.read_bool(present))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!present)
    {
        slot.reset();
        error = PayloadError::none;
        return true;
    }
    InventoryItemPayload item;
    if (!read_item(reader, protocol, item, error))
        return false;
    slot = std::move(item);
    return true;
}

template <std::size_t Size>
bool write_slots(BoundedWriter &writer, const std::array<InventorySlotPayload, Size> &slots,
                 ProtocolVersion protocol, PayloadError &error)
{
    for (const InventorySlotPayload &slot : slots)
        if (!write_optional_item(writer, slot, protocol, error))
            return false;
    return true;
}

template <std::size_t Size>
bool read_slots(BoundedReader &reader, std::array<InventorySlotPayload, Size> &slots,
                ProtocolVersion protocol, PayloadError &error)
{
    for (InventorySlotPayload &slot : slots)
        if (!read_optional_item(reader, protocol, slot, error))
            return false;
    return true;
}

InventoryItemPayload base_item(std::int16_t item_id, std::int16_t stack_count)
{
    return {item_id, stack_count, 1.0F, std::nullopt, std::nullopt, std::nullopt};
}

InventoryItemPayload gun_item(std::int16_t item_id, std::int32_t rounds)
{
    return {item_id, 1, 1.0F, rounds, std::nullopt, std::nullopt};
}

bool supported_inventory_version(std::int32_t version, ProtocolVersion protocol) noexcept
{
    switch (protocol)
    {
    case ProtocolVersion::classic_v3:
        return version == classic_inventory_version;
    case ProtocolVersion::modern_v4:
        return version >= 0 && version <= modern_inventory_version;
    }
    return false;
}

} // namespace

InventoryItemSerializationKind inventory_item_serialization_kind(std::int16_t item_id,
                                                                 ProtocolVersion protocol) noexcept
{
    if (!registered_item_id(item_id, protocol))
        return InventoryItemSerializationKind::invalid;
    if (item_id == 77 || item_id == 78)
        return InventoryItemSerializationKind::locator;
    if (protocol == ProtocolVersion::modern_v4)
    {
        if (item_id == 133)
            return InventoryItemSerializationKind::spawn_block;
        if (item_id == 151)
            return InventoryItemSerializationKind::teleport_block;
    }
    return gun_item_id(item_id, protocol) ? InventoryItemSerializationKind::gun
                                          : InventoryItemSerializationKind::base;
}

bool inventory_gun_clip_capacity(std::int16_t item_id, ProtocolVersion protocol,
                                 std::int32_t &capacity) noexcept
{
    capacity = 0;
    switch (item_id)
    {
    case 50:
    case 56:
    case 62:
    case 68:
        capacity = 30;
        break;
    case 51:
    case 57:
    case 63:
    case 69:
        capacity = 8;
        break;
    case 52:
    case 58:
    case 64:
    case 70:
        capacity = 6;
        break;
    case 53:
    case 59:
    case 65:
    case 71:
        capacity = 8;
        break;
    case 54:
    case 60:
    case 66:
    case 72:
        capacity = 20;
        break;
    case 103:
    case 104:
    case 112:
    case 113:
        capacity = 1;
        break;
    case 119:
    case 120:
    case 121:
    case 122:
        capacity = 100;
        break;
    case 141:
        if (protocol == ProtocolVersion::modern_v4)
            capacity = 1;
        break;
    case 143:
        if (protocol == ProtocolVersion::modern_v4)
            capacity = 200;
        break;
    case 149:
        if (protocol == ProtocolVersion::modern_v4)
            capacity = 10;
        break;
    default:
        if (item_id >= 82 && item_id <= 101)
        {
            constexpr std::int32_t capacities[]{30, 7, 8, 10, 20};
            capacity = capacities[(item_id - 82) % 5];
        }
        break;
    }
    return capacity != 0 && gun_item_id(item_id, protocol);
}

bool inventory_item_stack_limit(std::int16_t item_id, ProtocolVersion protocol,
                                std::int32_t &limit) noexcept
{
    limit = 0;
    if (protocol == ProtocolVersion::classic_v3)
    {
        if (item_id == 0)
            limit = 1;
        else if (item_id >= 1 && item_id <= 22)
            limit = 64;
        else if (item_id >= 23 && item_id <= 40)
            limit = 1;
        else if (item_id >= 41 && item_id <= 47)
            limit = 5'000;
        else if (item_id >= 49 && item_id <= 72)
            limit = 1;
        else if (item_id >= 73 && item_id <= 76)
            limit = 64;
        else if (item_id == 77 || item_id == 78)
            limit = 1;
        else if (item_id == 79 || item_id == 80)
            limit = 64;
        else if (item_id >= 81 && item_id <= 101)
            limit = 1;
        else if (item_id == 102)
            limit = 64;
        else if (item_id == 103 || item_id == 104)
            limit = 1;
        else if (item_id == 105)
            limit = 5'000;
        else if (item_id >= 106 && item_id <= 108)
            limit = 64;
        else if (item_id == 109)
            limit = 5'000;
        else if (item_id == 110)
            limit = 10;
        else if (item_id == 111)
            limit = 5'000;
        else if (item_id >= 112 && item_id <= 122)
            limit = 1;
        return limit != 0;
    }
    if (protocol != ProtocolVersion::modern_v4)
        return false;

    if (item_id == 0)
        limit = 1;
    else if (item_id >= 1 && item_id <= 12)
        limit = 999;
    else if (item_id == 13)
        limit = 64;
    else if (item_id == 14)
        limit = 999;
    else if (item_id >= 15 && item_id <= 22)
        limit = 255;
    else if (item_id >= 23 && item_id <= 40)
        limit = 1;
    else if (item_id >= 41 && item_id <= 47)
        limit = 5'000;
    else if (item_id >= 49 && item_id <= 72)
        limit = 1;
    else if (item_id >= 73 && item_id <= 76)
        limit = 999;
    else if (item_id == 77 || item_id == 78)
        limit = 1;
    else if (item_id == 79 || item_id == 80)
        limit = 999;
    else if (item_id >= 81 && item_id <= 101)
        limit = 1;
    else if (item_id == 102)
        limit = 999;
    else if (item_id == 103 || item_id == 104)
        limit = 1;
    else if (item_id == 105)
        limit = 5'000;
    else if (item_id == 106)
        limit = 64;
    else if (item_id == 107)
        limit = 999;
    else if (item_id == 108)
        limit = 64;
    else if (item_id == 109)
        limit = 5'000;
    else if (item_id == 110)
        limit = 10;
    else if (item_id == 111)
        limit = 5'000;
    else if (item_id >= 112 && item_id <= 123)
        limit = 1;
    else if (item_id == 126)
        limit = 10;
    else if ((item_id >= 127 && item_id <= 133) || (item_id >= 137 && item_id <= 140))
        limit = 999;
    else if (item_id == 141 || item_id == 143)
        limit = 1;
    else if (item_id == 147 || item_id == 148)
        limit = 999;
    else if (item_id == 149 || item_id == 151)
        limit = 1;
    else if (item_id == 153 || item_id == 154)
        limit = 999;
    return limit != 0;
}

std::optional<std::vector<std::uint8_t>> encode_inventory_item_payload(
    const InventoryItemPayload &item, ProtocolVersion protocol, PayloadError &error)
{
    BoundedWriter writer{maximum_payload_size};
    if (!write_item(writer, item, protocol, error))
        return std::nullopt;
    error = PayloadError::none;
    return writer.take();
}

bool decode_inventory_item_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                   InventoryItemPayload &item, PayloadError &error)
{
    BoundedReader reader{payload};
    InventoryItemPayload decoded;
    if (!read_item(reader, protocol, decoded, error) || !reader.exhausted())
    {
        if (error == PayloadError::none)
            error = PayloadError::malformed;
        return false;
    }
    item = std::move(decoded);
    error = PayloadError::none;
    return true;
}

PlayerInventoryPayload make_empty_player_inventory(ProtocolVersion protocol)
{
    PlayerInventoryPayload inventory;
    inventory.format_version = protocol == ProtocolVersion::classic_v3 ? classic_inventory_version
                                                                       : modern_inventory_version;
    inventory.trays.resize(protocol == ProtocolVersion::classic_v3 ? 1U : 2U);
    return inventory;
}

PlayerInventoryPayload make_default_player_inventory(ProtocolVersion protocol, bool hardcore)
{
    PlayerInventoryPayload inventory = make_empty_player_inventory(protocol);
    if (hardcore)
        return inventory;
    InventoryTrayPayload &tray = inventory.trays.front();
    tray[0] = base_item(23, 1);
    tray[1] = base_item(39, 1);
    tray[2] = gun_item(51, 8);
    tray[3] = base_item(49, 1);
    tray[4] = base_item(44, 200);
    tray[5] = base_item(14, 16);
    return inventory;
}

std::optional<std::vector<std::uint8_t>> encode_player_inventory_payload(
    const PlayerInventoryPayload &inventory, ProtocolVersion protocol, PayloadError &error)
{
    const std::size_t expected_trays = protocol == ProtocolVersion::classic_v3 ? 1U : 2U;
    if (inventory.trays.size() != expected_trays ||
        inventory.teleport_stations.size() > maximum_teleport_stations ||
        (protocol == ProtocolVersion::classic_v3 &&
         (!inventory.teleport_stations.empty() || inventory.spawn_point)))
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    for (const InventorySlotPayload &station : inventory.teleport_stations)
        if (station && station->item_id != 151)
        {
            error = PayloadError::out_of_range;
            return std::nullopt;
        }
    if (inventory.spawn_point && inventory.spawn_point->item_id != 133)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }

    BoundedWriter writer{maximum_payload_size};
    const std::int32_t write_version = protocol == ProtocolVersion::classic_v3
                                           ? classic_inventory_version
                                           : modern_inventory_version;
    if (!writer.write_dotnet_string(inventory_identifier, maximum_payload_size) ||
        !writer.write_i32(write_version) ||
        !write_slots(writer, inventory.storage, protocol, error))
    {
        if (error == PayloadError::none)
            error = PayloadError::too_large;
        return std::nullopt;
    }
    if (protocol == ProtocolVersion::classic_v3)
    {
        if (!write_slots(writer, inventory.trays.front(), protocol, error))
            return std::nullopt;
    }
    else
    {
        if (!writer.write_i32(2))
        {
            error = PayloadError::too_large;
            return std::nullopt;
        }
        for (const InventoryTrayPayload &tray : inventory.trays)
            if (!writer.write_i32(8) || !write_slots(writer, tray, protocol, error))
            {
                if (error == PayloadError::none)
                    error = PayloadError::too_large;
                return std::nullopt;
            }
        if (!writer.write_i32(static_cast<std::int32_t>(inventory.teleport_stations.size())))
        {
            error = PayloadError::too_large;
            return std::nullopt;
        }
        for (const InventorySlotPayload &station : inventory.teleport_stations)
            if (!write_optional_item(writer, station, protocol, error))
                return std::nullopt;
        if (!write_optional_item(writer, inventory.spawn_point, protocol, error))
            return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_player_inventory_payload(std::span<const std::uint8_t> payload,
                                     ProtocolVersion protocol, PlayerInventoryPayload &inventory,
                                     PayloadError &error)
{
    if (payload.size() > maximum_payload_size)
    {
        error = PayloadError::too_large;
        return false;
    }
    BoundedReader reader{payload};
    PlayerInventoryPayload decoded;
    std::string identifier;
    if (!reader.read_dotnet_string(identifier, maximum_payload_size) ||
        identifier != inventory_identifier || !reader.read_i32(decoded.format_version))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!supported_inventory_version(decoded.format_version, protocol))
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!read_slots(reader, decoded.storage, protocol, error))
        return false;

    if (protocol == ProtocolVersion::classic_v3 || decoded.format_version <= 3)
    {
        decoded.trays.resize(1U);
        if (!read_slots(reader, decoded.trays.front(), protocol, error))
            return false;
    }
    else
    {
        std::int32_t tray_count{};
        if (!reader.read_i32(tray_count) || tray_count != 2)
        {
            error = PayloadError::out_of_range;
            return false;
        }
        decoded.trays.resize(2U);
        for (InventoryTrayPayload &tray : decoded.trays)
        {
            std::int32_t item_count{};
            if (!reader.read_i32(item_count) || item_count != 8)
            {
                error = PayloadError::out_of_range;
                return false;
            }
            if (!read_slots(reader, tray, protocol, error))
                return false;
        }
    }

    if (protocol == ProtocolVersion::modern_v4 && decoded.format_version > 2)
    {
        std::int32_t teleport_count{};
        if (!reader.read_i32(teleport_count))
        {
            error = PayloadError::malformed;
            return false;
        }
        if (teleport_count < 0 ||
            static_cast<std::size_t>(teleport_count) > maximum_teleport_stations ||
            static_cast<std::size_t>(teleport_count) > reader.remaining())
        {
            error = PayloadError::out_of_range;
            return false;
        }
        decoded.teleport_stations.resize(static_cast<std::size_t>(teleport_count));
        for (InventorySlotPayload &station : decoded.teleport_stations)
        {
            if (!read_optional_item(reader, protocol, station, error))
                return false;
            if (station && station->item_id != 151)
            {
                error = PayloadError::out_of_range;
                return false;
            }
        }
    }
    if (protocol == ProtocolVersion::modern_v4 && decoded.format_version > 1)
    {
        if (!read_optional_item(reader, protocol, decoded.spawn_point, error))
            return false;
        if (decoded.spawn_point && decoded.spawn_point->item_id != 133)
        {
            error = PayloadError::out_of_range;
            return false;
        }
    }
    if (!reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    inventory = std::move(decoded);
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_inventory_store_payload(
    const InventoryStorePayload &message, ProtocolVersion protocol, PayloadError &error)
{
    auto inventory = encode_player_inventory_payload(message.inventory, protocol, error);
    if (!inventory || inventory->size() + 1U > maximum_payload_size)
    {
        if (inventory)
            error = PayloadError::too_large;
        return std::nullopt;
    }
    BoundedWriter writer{maximum_payload_size};
    if (!writer.write_bytes(*inventory) || !writer.write_bool(message.final_save))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_inventory_store_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                                    InventoryStorePayload &message, PayloadError &error)
{
    if (payload.empty() || payload.back() > 1U)
    {
        error = PayloadError::malformed;
        return false;
    }
    InventoryStorePayload decoded;
    if (!decode_player_inventory_payload(payload.first(payload.size() - 1U), protocol,
                                         decoded.inventory, error))
        return false;
    decoded.final_save = payload.back() != 0U;
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_inventory_retrieve_payload(
    const InventoryRetrievePayload &message, ProtocolVersion protocol, PayloadError &error)
{
    auto inventory = encode_player_inventory_payload(message.inventory, protocol, error);
    if (!inventory || inventory->size() + 2U > maximum_payload_size)
    {
        if (inventory)
            error = PayloadError::too_large;
        return std::nullopt;
    }
    BoundedWriter writer{maximum_payload_size};
    if (!writer.write_bytes(*inventory) || !writer.write_u8(message.player_id) ||
        !writer.write_bool(message.default_inventory))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_inventory_retrieve_payload(std::span<const std::uint8_t> payload,
                                       ProtocolVersion protocol, InventoryRetrievePayload &message,
                                       PayloadError &error)
{
    if (payload.size() < 2U || payload.back() > 1U)
    {
        error = PayloadError::malformed;
        return false;
    }
    InventoryRetrievePayload decoded;
    if (!decode_player_inventory_payload(payload.first(payload.size() - 2U), protocol,
                                         decoded.inventory, error))
        return false;
    decoded.player_id = payload[payload.size() - 2U];
    decoded.default_inventory = payload.back() != 0U;
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_item_crate_payload(const ItemCratePayload &message,
                                                                   ProtocolVersion protocol,
                                                                   PayloadError &error)
{
    if (message.index < 0 || message.index >= 32)
    {
        error = PayloadError::out_of_range;
        return std::nullopt;
    }
    BoundedWriter writer{maximum_payload_size};
    if (!write_int_vector(writer, message.location) || !writer.write_i32(message.index) ||
        !write_optional_item(writer, message.item, protocol, error))
    {
        if (error == PayloadError::none)
            error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_item_crate_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                               ItemCratePayload &message, PayloadError &error)
{
    if (payload.size() < 17U)
    {
        error = PayloadError::malformed;
        return false;
    }
    BoundedReader reader{payload};
    ItemCratePayload decoded;
    if (!read_int_vector(reader, decoded.location) || !reader.read_i32(decoded.index))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (decoded.index < 0 || decoded.index >= 32)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!read_optional_item(reader, protocol, decoded.item, error) || !reader.exhausted())
    {
        if (error == PayloadError::none)
            error = PayloadError::malformed;
        return false;
    }
    message = std::move(decoded);
    error = PayloadError::none;
    return true;
}

std::optional<std::vector<std::uint8_t>> encode_world_info_payload(const WorldInfoPayload &world,
                                                                   ProtocolVersion protocol,
                                                                   PayloadError &error)
{
    const bool classic = protocol == ProtocolVersion::classic_v3;
    if ((!classic && protocol != ProtocolVersion::modern_v4) || world.terrain_version != 1 ||
        !finite(world.last_position) || world.created_date_ticks < 0 ||
        world.created_date_ticks > maximum_datetime_ticks || world.last_played_date_ticks < 0 ||
        world.last_played_date_ticks > maximum_datetime_ticks ||
        world.crates.size() > maximum_world_crates || world.doors.size() > maximum_world_doors ||
        world.spawners.size() > maximum_world_spawners ||
        (classic && (!world.doors.empty() || !world.spawners.empty() ||
                     world.hell_bosses_spawned != 0 || world.max_hell_boss_spawns != 0)) ||
        (!classic && (world.hell_bosses_spawned < 0 || world.max_hell_boss_spawns < 0 ||
                      world.hell_bosses_spawned > world.max_hell_boss_spawns)))
    {
        error =
            !finite(world.last_position) ? PayloadError::non_finite : PayloadError::out_of_range;
        return std::nullopt;
    }

    std::set<IntVector3> crate_locations;
    for (const WorldCratePayload &crate : world.crates)
        if (!crate_locations.insert(crate.location).second)
        {
            error = PayloadError::out_of_range;
            return std::nullopt;
        }
    std::set<IntVector3> door_locations;
    for (const WorldDoorPayload &door : world.doors)
        if (door.model_name > 4U || !door_locations.insert(door.location).second)
        {
            error = PayloadError::out_of_range;
            return std::nullopt;
        }
    std::set<IntVector3> spawner_locations;
    for (const WorldSpawnerPayload &spawner : world.spawners)
        if (!spawner_locations.insert(spawner.location).second)
        {
            error = PayloadError::out_of_range;
            return std::nullopt;
        }

    BoundedWriter writer{maximum_payload_size};
    const std::int32_t format_version = classic ? WorldInfoPayload::classic_format_version
                                                : WorldInfoPayload::modern_format_version;
    if (!writer.write_i32(format_version) || !writer.write_i32(world.terrain_version) ||
        !writer.write_dotnet_string(world.name, maximum_payload_size) ||
        !writer.write_dotnet_string(world.owner_gamer_tag, maximum_payload_size) ||
        !writer.write_dotnet_string(world.creator_gamer_tag, maximum_payload_size) ||
        !writer.write_i64(world.created_date_ticks) ||
        !writer.write_i64(world.last_played_date_ticks) || !writer.write_i32(world.seed) ||
        !writer.write_bytes(world.world_id) || !write_vector(writer, world.last_position) ||
        !writer.write_i32(static_cast<std::int32_t>(world.crates.size())))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    for (const WorldCratePayload &crate : world.crates)
    {
        if (!write_int_vector(writer, crate.location) ||
            !write_slots(writer, crate.inventory, protocol, error))
        {
            if (error == PayloadError::none)
                error = PayloadError::too_large;
            return std::nullopt;
        }
    }
    if (!classic)
    {
        if (!writer.write_i32(static_cast<std::int32_t>(world.doors.size())))
        {
            error = PayloadError::too_large;
            return std::nullopt;
        }
        for (const WorldDoorPayload &door : world.doors)
            if (!write_int_vector(writer, door.location) || !writer.write_u8(door.model_name))
            {
                error = PayloadError::too_large;
                return std::nullopt;
            }

        if (!writer.write_i32(static_cast<std::int32_t>(world.spawners.size())))
        {
            error = PayloadError::too_large;
            return std::nullopt;
        }
        for (const WorldSpawnerPayload &spawner : world.spawners)
            if (!write_int_vector(writer, spawner.location) ||
                !writer.write_dotnet_string(spawner.spawn_id, maximum_payload_size))
            {
                error = PayloadError::too_large;
                return std::nullopt;
            }
    }
    if (!writer.write_bool(world.infinite_resource_mode) ||
        !writer.write_dotnet_string(world.server_message, maximum_payload_size) ||
        !writer.write_dotnet_string(world.server_password, maximum_payload_size) ||
        (!classic && (!writer.write_i32(world.hell_bosses_spawned) ||
                      !writer.write_i32(world.max_hell_boss_spawns))))
    {
        error = PayloadError::too_large;
        return std::nullopt;
    }
    error = PayloadError::none;
    return writer.take();
}

bool decode_world_info_payload(std::span<const std::uint8_t> payload, ProtocolVersion protocol,
                               WorldInfoPayload &world, PayloadError &error)
{
    if (payload.size() > maximum_payload_size)
    {
        error = PayloadError::too_large;
        return false;
    }
    BoundedReader reader{payload};
    WorldInfoPayload decoded;
    if (!reader.read_i32(decoded.format_version))
    {
        error = PayloadError::malformed;
        return false;
    }
    const bool classic = protocol == ProtocolVersion::classic_v3;
    const bool supported_version =
        classic ? decoded.format_version == WorldInfoPayload::classic_format_version
                : protocol == ProtocolVersion::modern_v4 && decoded.format_version >= 1 &&
                      decoded.format_version <= WorldInfoPayload::modern_format_version;
    if (!supported_version)
    {
        error = PayloadError::out_of_range;
        return false;
    }

    if (!reader.read_i32(decoded.terrain_version) ||
        !reader.read_dotnet_string(decoded.name, maximum_payload_size) ||
        !reader.read_dotnet_string(decoded.owner_gamer_tag, maximum_payload_size) ||
        !reader.read_dotnet_string(decoded.creator_gamer_tag, maximum_payload_size) ||
        !reader.read_i64(decoded.created_date_ticks) ||
        !reader.read_i64(decoded.last_played_date_ticks) || !reader.read_i32(decoded.seed) ||
        !reader.read_bytes(decoded.world_id) || !read_vector(reader, decoded.last_position))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (decoded.terrain_version != 1 || decoded.created_date_ticks < 0 ||
        decoded.created_date_ticks > maximum_datetime_ticks || decoded.last_played_date_ticks < 0 ||
        decoded.last_played_date_ticks > maximum_datetime_ticks)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    if (!finite(decoded.last_position))
    {
        error = PayloadError::non_finite;
        return false;
    }

    std::int32_t crate_count{};
    constexpr std::size_t minimum_crate_size = 12U + 32U;
    if (!reader.read_i32(crate_count))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (crate_count < 0 || static_cast<std::size_t>(crate_count) > maximum_world_crates ||
        static_cast<std::size_t>(crate_count) > reader.remaining() / minimum_crate_size)
    {
        error = PayloadError::out_of_range;
        return false;
    }
    decoded.crates.reserve(static_cast<std::size_t>(crate_count));
    std::set<IntVector3> crate_locations;
    for (std::int32_t index{}; index < crate_count; ++index)
    {
        WorldCratePayload crate;
        if (!read_int_vector(reader, crate.location) ||
            !crate_locations.insert(crate.location).second)
        {
            error = PayloadError::malformed;
            return false;
        }
        if (!read_slots(reader, crate.inventory, protocol, error))
            return false;
        decoded.crates.push_back(std::move(crate));
    }

    if (!classic && decoded.format_version > 2)
    {
        std::int32_t door_count{};
        if (!reader.read_i32(door_count))
        {
            error = PayloadError::malformed;
            return false;
        }
        if (door_count < 0 || static_cast<std::size_t>(door_count) > maximum_world_doors ||
            static_cast<std::size_t>(door_count) > reader.remaining() / 13U)
        {
            error = PayloadError::out_of_range;
            return false;
        }
        decoded.doors.reserve(static_cast<std::size_t>(door_count));
        std::set<IntVector3> door_locations;
        for (std::int32_t index{}; index < door_count; ++index)
        {
            WorldDoorPayload door;
            if (!read_int_vector(reader, door.location) || !reader.read_u8(door.model_name))
            {
                error = PayloadError::malformed;
                return false;
            }
            if (door.model_name > 4U || !door_locations.insert(door.location).second)
            {
                error = PayloadError::out_of_range;
                return false;
            }
            decoded.doors.push_back(door);
        }
    }

    if (!classic && decoded.format_version > 3)
    {
        std::int32_t spawner_count{};
        if (!reader.read_i32(spawner_count))
        {
            error = PayloadError::malformed;
            return false;
        }
        if (spawner_count < 0 || static_cast<std::size_t>(spawner_count) > maximum_world_spawners ||
            static_cast<std::size_t>(spawner_count) > reader.remaining() / 13U)
        {
            error = PayloadError::out_of_range;
            return false;
        }
        decoded.spawners.reserve(static_cast<std::size_t>(spawner_count));
        std::set<IntVector3> spawner_locations;
        for (std::int32_t index{}; index < spawner_count; ++index)
        {
            WorldSpawnerPayload spawner;
            if (!read_int_vector(reader, spawner.location) ||
                !reader.read_dotnet_string(spawner.spawn_id, maximum_payload_size))
            {
                error = PayloadError::malformed;
                return false;
            }
            if (!spawner_locations.insert(spawner.location).second)
            {
                error = PayloadError::out_of_range;
                return false;
            }
            decoded.spawners.push_back(std::move(spawner));
        }
    }

    if (!reader.read_bool(decoded.infinite_resource_mode) ||
        !reader.read_dotnet_string(decoded.server_message, maximum_payload_size) ||
        !reader.read_dotnet_string(decoded.server_password, maximum_payload_size))
    {
        error = PayloadError::malformed;
        return false;
    }
    if (!classic && decoded.format_version > 4)
    {
        if (!reader.read_i32(decoded.hell_bosses_spawned) ||
            !reader.read_i32(decoded.max_hell_boss_spawns))
        {
            error = PayloadError::malformed;
            return false;
        }
        if (decoded.hell_bosses_spawned < 0 || decoded.max_hell_boss_spawns < 0 ||
            decoded.hell_bosses_spawned > decoded.max_hell_boss_spawns)
        {
            error = PayloadError::out_of_range;
            return false;
        }
    }
    if (!reader.exhausted())
    {
        error = PayloadError::malformed;
        return false;
    }
    world = std::move(decoded);
    error = PayloadError::none;
    return true;
}

} // namespace openminerz::net
