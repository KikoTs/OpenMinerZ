#include "openminerz/net/protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <limits>
#include <utility>

namespace openminerz::net
{
namespace
{

using namespace std::literals;

constexpr std::array classic_message_names{
    "DNA.CastleMinerZ.Net.AddExplosionEffectsMessage"sv,
    "DNA.CastleMinerZ.Net.AddExplosiveFlashMessage"sv,
    "DNA.CastleMinerZ.Net.AlterBlockMessage"sv,
    "DNA.CastleMinerZ.Net.AppointServerMessage"sv,
    "DNA.CastleMinerZ.Net.BroadcastTextMessage"sv,
    "DNA.CastleMinerZ.Net.ChangeCarriedItemMessage"sv,
    "DNA.CastleMinerZ.Net.ClientReadyForChunksMessage"sv,
    "DNA.CastleMinerZ.Net.ConsumePickupMessage"sv,
    "DNA.CastleMinerZ.Net.CrateFocusMessage"sv,
    "DNA.CastleMinerZ.Net.CreatePickupMessage"sv,
    "DNA.CastleMinerZ.Net.DestroyCrateMessage"sv,
    "DNA.CastleMinerZ.Net.DetonateExplosiveMessage"sv,
    "DNA.CastleMinerZ.Net.DetonateFireballMessage"sv,
    "DNA.CastleMinerZ.Net.DetonateGrenadeMessage"sv,
    "DNA.CastleMinerZ.Net.DetonateRocketMessage"sv,
    "DNA.CastleMinerZ.Net.DigMessage"sv,
    "DNA.CastleMinerZ.Net.DoorOpenCloseMessage"sv,
    "DNA.CastleMinerZ.Net.DragonAttackMessage"sv,
    "DNA.CastleMinerZ.Net.EnemyGiveUpMessage"sv,
    "DNA.CastleMinerZ.Net.ExistingDragonMessage"sv,
    "DNA.CastleMinerZ.Net.FireRocketMessage"sv,
    "DNA.CastleMinerZ.Net.GrenadeMessage"sv,
    "DNA.CastleMinerZ.Net.GunshotMessage"sv,
    "DNA.CastleMinerZ.Net.InventoryRetrieveFromServerMessage"sv,
    "DNA.CastleMinerZ.Net.InventoryStoreOnServerMessage"sv,
    "DNA.CastleMinerZ.Net.ItemCrateMessage"sv,
    "DNA.CastleMinerZ.Net.KickMessage"sv,
    "DNA.CastleMinerZ.Net.KillDragonMessage"sv,
    "DNA.CastleMinerZ.Net.KillEnemyMessage"sv,
    "DNA.CastleMinerZ.Net.MeleePlayerMessage"sv,
    "DNA.CastleMinerZ.Net.MigrateDragonMessage"sv,
    "DNA.CastleMinerZ.Net.PlayerExistsMessage"sv,
    "DNA.CastleMinerZ.Net.PlayerUpdateMessage"sv,
    "DNA.CastleMinerZ.Net.ProvideChunkMessage"sv,
    "DNA.CastleMinerZ.Net.ProvideDeltaListMessage"sv,
    "DNA.CastleMinerZ.Net.RemoveBlocksMessage"sv,
    "DNA.CastleMinerZ.Net.RemoveDragonMessage"sv,
    "DNA.CastleMinerZ.Net.RequestChunkMessage"sv,
    "DNA.CastleMinerZ.Net.RequestDragonMessage"sv,
    "DNA.CastleMinerZ.Net.RequestInventoryMessage"sv,
    "DNA.CastleMinerZ.Net.RequestPickupMessage"sv,
    "DNA.CastleMinerZ.Net.RequestWorldInfoMessage"sv,
    "DNA.CastleMinerZ.Net.RestartLevelMessage"sv,
    "DNA.CastleMinerZ.Net.ShotgunShotMessage"sv,
    "DNA.CastleMinerZ.Net.SpawnDragonMessage"sv,
    "DNA.CastleMinerZ.Net.SpawnEnemyMessage"sv,
    "DNA.CastleMinerZ.Net.SpeedUpEnemyMessage"sv,
    "DNA.CastleMinerZ.Net.TimeConnectedMessage"sv,
    "DNA.CastleMinerZ.Net.TimeOfDayMessage"sv,
    "DNA.CastleMinerZ.Net.UpdateDragonMessage"sv,
    "DNA.CastleMinerZ.Net.WorldInfoMessage"sv,
    "DNA.Net.ChatMessage"sv,
    "DNA.Net.VoiceChatMessage"sv,
};

constexpr std::array modern_message_names{
    "DNA.CastleMinerZ.Net.AddExplosionEffectsMessage"sv,
    "DNA.CastleMinerZ.Net.AddExplosiveFlashMessage"sv,
    "DNA.CastleMinerZ.Net.AlterBlockMessage"sv,
    "DNA.CastleMinerZ.Net.AppointServerMessage"sv,
    "DNA.CastleMinerZ.Net.BroadcastTextMessage"sv,
    "DNA.CastleMinerZ.Net.ChangeCarriedItemMessage"sv,
    "DNA.CastleMinerZ.Net.ClientReadyForChunksMessage"sv,
    "DNA.CastleMinerZ.Net.ConsumePickupMessage"sv,
    "DNA.CastleMinerZ.Net.CrateFocusMessage"sv,
    "DNA.CastleMinerZ.Net.CreatePickupMessage"sv,
    "DNA.CastleMinerZ.Net.DestroyCrateMessage"sv,
    "DNA.CastleMinerZ.Net.DestroyCustomBlockMessage"sv,
    "DNA.CastleMinerZ.Net.DetonateExplosiveMessage"sv,
    "DNA.CastleMinerZ.Net.DetonateFireballMessage"sv,
    "DNA.CastleMinerZ.Net.DetonateGrenadeMessage"sv,
    "DNA.CastleMinerZ.Net.DetonateRocketMessage"sv,
    "DNA.CastleMinerZ.Net.DigMessage"sv,
    "DNA.CastleMinerZ.Net.DoorOpenCloseMessage"sv,
    "DNA.CastleMinerZ.Net.DragonAttackMessage"sv,
    "DNA.CastleMinerZ.Net.EnemyGiveUpMessage"sv,
    "DNA.CastleMinerZ.Net.ExistingDragonMessage"sv,
    "DNA.CastleMinerZ.Net.FireRocketMessage"sv,
    "DNA.CastleMinerZ.Net.GrenadeMessage"sv,
    "DNA.CastleMinerZ.Net.GunshotMessage"sv,
    "DNA.CastleMinerZ.Net.InventoryRetrieveFromServerMessage"sv,
    "DNA.CastleMinerZ.Net.InventoryStoreOnServerMessage"sv,
    "DNA.CastleMinerZ.Net.ItemCrateMessage"sv,
    "DNA.CastleMinerZ.Net.KickMessage"sv,
    "DNA.CastleMinerZ.Net.KillDragonMessage"sv,
    "DNA.CastleMinerZ.Net.KillEnemyMessage"sv,
    "DNA.CastleMinerZ.Net.MeleePlayerMessage"sv,
    "DNA.CastleMinerZ.Net.MigrateDragonMessage"sv,
    "DNA.CastleMinerZ.Net.PlayerExistsMessage"sv,
    "DNA.CastleMinerZ.Net.PlayerUpdateMessage"sv,
    "DNA.CastleMinerZ.Net.ProvideChunkMessage"sv,
    "DNA.CastleMinerZ.Net.ProvideDeltaListMessage"sv,
    "DNA.CastleMinerZ.Net.RemoveBlocksMessage"sv,
    "DNA.CastleMinerZ.Net.RemoveDragonMessage"sv,
    "DNA.CastleMinerZ.Net.RequestChunkMessage"sv,
    "DNA.CastleMinerZ.Net.RequestDragonMessage"sv,
    "DNA.CastleMinerZ.Net.RequestInventoryMessage"sv,
    "DNA.CastleMinerZ.Net.RequestPickupMessage"sv,
    "DNA.CastleMinerZ.Net.RequestWorldInfoMessage"sv,
    "DNA.CastleMinerZ.Net.RestartLevelMessage"sv,
    "DNA.CastleMinerZ.Net.ShotgunShotMessage"sv,
    "DNA.CastleMinerZ.Net.SpawnDragonMessage"sv,
    "DNA.CastleMinerZ.Net.SpawnEnemyMessage"sv,
    "DNA.CastleMinerZ.Net.SpeedUpEnemyMessage"sv,
    "DNA.CastleMinerZ.Net.TimeConnectedMessage"sv,
    "DNA.CastleMinerZ.Net.TimeOfDayMessage"sv,
    "DNA.CastleMinerZ.Net.UpdateDragonMessage"sv,
    "DNA.CastleMinerZ.Net.UpdateSpawnerMessage"sv,
    "DNA.CastleMinerZ.Net.WorldInfoMessage"sv,
    "DNA.Net.ChatMessage"sv,
    "DNA.Net.VoiceChatMessage"sv,
};

constexpr bool one_of(std::string_view value,
                      std::initializer_list<std::string_view> choices) noexcept
{
    for (const std::string_view choice : choices)
    {
        if (value == choice)
            return true;
    }
    return false;
}

constexpr MessageTransportContract transport_for(std::string_view name) noexcept
{
    const bool reliable_unordered = one_of(name, {"AddExplosionEffectsMessage",
                                                  "AddExplosiveFlashMessage",
                                                  "ChangeCarriedItemMessage",
                                                  "ConsumePickupMessage",
                                                  "CreatePickupMessage",
                                                  "DetonateGrenadeMessage",
                                                  "DetonateRocketMessage",
                                                  "DigMessage",
                                                  "EnemyGiveUpMessage",
                                                  "FireRocketMessage",
                                                  "GrenadeMessage",
                                                  "GunshotMessage",
                                                  "KillEnemyMessage",
                                                  "ProvideChunkMessage",
                                                  "ProvideDeltaListMessage",
                                                  "RemoveDragonMessage",
                                                  "RequestChunkMessage",
                                                  "RequestPickupMessage",
                                                  "ShotgunShotMessage",
                                                  "SpawnEnemyMessage",
                                                  "SpeedUpEnemyMessage",
                                                  "UpdateSpawnerMessage"});
    const bool reliable_ordered = one_of(name, {"AlterBlockMessage",
                                                "AppointServerMessage",
                                                "BroadcastTextMessage",
                                                "ClientReadyForChunksMessage",
                                                "CrateFocusMessage",
                                                "DestroyCrateMessage",
                                                "DestroyCustomBlockMessage",
                                                "DetonateExplosiveMessage",
                                                "DetonateFireballMessage",
                                                "DoorOpenCloseMessage",
                                                "DragonAttackMessage",
                                                "ExistingDragonMessage",
                                                "InventoryRetrieveFromServerMessage",
                                                "InventoryStoreOnServerMessage",
                                                "ItemCrateMessage",
                                                "KickMessage",
                                                "KillDragonMessage",
                                                "MeleePlayerMessage",
                                                "MigrateDragonMessage",
                                                "PlayerExistsMessage",
                                                "RemoveBlocksMessage",
                                                "RequestDragonMessage",
                                                "RequestInventoryMessage",
                                                "RequestWorldInfoMessage",
                                                "RestartLevelMessage",
                                                "SpawnDragonMessage",
                                                "TimeConnectedMessage",
                                                "TimeOfDayMessage",
                                                "WorldInfoMessage"});

    MessageDeliveryPolicy delivery = MessageDeliveryPolicy::unreliable;
    if (reliable_unordered)
        delivery = MessageDeliveryPolicy::reliable_unordered;
    else if (reliable_ordered)
        delivery = MessageDeliveryPolicy::reliable_ordered;
    else if (name == "ChatMessage" || name == "VoiceChatMessage")
        delivery = MessageDeliveryPolicy::chat;
    else if (name == "PlayerUpdateMessage")
        delivery = MessageDeliveryPolicy::player_update_state;
    else if (name == "UpdateDragonMessage")
        delivery = MessageDeliveryPolicy::dragon_update_warmup;

    const bool echo =
        !one_of(name, {"ClientReadyForChunksMessage", "ItemCrateMessage", "MeleePlayerMessage",
                       "PlayerUpdateMessage", "TimeConnectedMessage", "TimeOfDayMessage",
                       "WorldInfoMessage"});
    return {delivery, echo};
}

template <std::size_t Size>
constexpr std::array<MessageDescriptor, Size> make_manifest(
    const std::array<std::string_view, Size> &names) noexcept
{
    std::array<MessageDescriptor, Size> result{};
    for (std::size_t index = 0U; index < Size; ++index)
    {
        const std::string_view full_name = names[index];
        const std::size_t separator = full_name.find_last_of('.');
        const std::string_view name = full_name.substr(separator + 1U);
        result[index] = {static_cast<std::uint8_t>(index), full_name, name, transport_for(name)};
    }
    return result;
}

constexpr auto classic_manifest = make_manifest(classic_message_names);
constexpr auto modern_manifest = make_manifest(modern_message_names);

bool valid_utf8(std::string_view value) noexcept
{
    std::size_t index{};
    while (index < value.size())
    {
        const auto lead = static_cast<std::uint8_t>(value[index]);
        std::size_t continuation_count{};
        std::uint32_t codepoint{};
        if (lead <= 0x7FU)
        {
            ++index;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U)
        {
            continuation_count = 1U;
            codepoint = lead & 0x1FU;
        }
        else if ((lead & 0xF0U) == 0xE0U)
        {
            continuation_count = 2U;
            codepoint = lead & 0x0FU;
        }
        else if ((lead & 0xF8U) == 0xF0U)
        {
            continuation_count = 3U;
            codepoint = lead & 0x07U;
        }
        else
            return false;

        if (index + continuation_count >= value.size())
            return false;
        for (std::size_t part = 1U; part <= continuation_count; ++part)
        {
            const auto next = static_cast<std::uint8_t>(value[index + part]);
            if ((next & 0xC0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if ((continuation_count == 1U && codepoint < 0x80U) ||
            (continuation_count == 2U && codepoint < 0x800U) ||
            (continuation_count == 3U && codepoint < 0x10000U) || codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU))
            return false;
        index += continuation_count + 1U;
    }
    return true;
}

} // namespace

bool player_routed_message(std::string_view name) noexcept
{
    return one_of(name, {"ChangeCarriedItemMessage",
                         "CrateFocusMessage",
                         "DigMessage",
                         "FireRocketMessage",
                         "GrenadeMessage",
                         "GunshotMessage",
                         "PlayerUpdateMessage",
                         "ShotgunShotMessage",
                         "TimeConnectedMessage"});
}

std::span<const MessageDescriptor> message_manifest(ProtocolVersion version) noexcept
{
    switch (version)
    {
    case ProtocolVersion::classic_v3:
        return classic_manifest;
    case ProtocolVersion::modern_v4:
        return modern_manifest;
    }
    return {};
}

const MessageDescriptor *message_by_id(ProtocolVersion version, std::uint8_t id) noexcept
{
    const auto manifest = message_manifest(version);
    return static_cast<std::size_t>(id) < manifest.size() ? &manifest[id] : nullptr;
}

const MessageDescriptor *message_by_name(ProtocolVersion version, std::string_view name) noexcept
{
    const auto manifest = message_manifest(version);
    const auto found = std::find_if(manifest.begin(), manifest.end(), [name](const auto &entry) {
        return entry.name == name || entry.full_name == name;
    });
    return found == manifest.end() ? nullptr : &*found;
}

std::uint8_t xor8(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint8_t checksum{};
    for (const std::uint8_t value : bytes)
        checksum ^= value;
    return checksum;
}

std::optional<std::vector<std::uint8_t>> encode_packet(ProtocolVersion version,
                                                       std::uint8_t message_id,
                                                       std::span<const std::uint8_t> payload,
                                                       PacketError &error)
{
    if (message_by_id(version, message_id) == nullptr)
    {
        error = PacketError::unknown_message_id;
        return std::nullopt;
    }
    if (payload.size() > maximum_payload_size)
    {
        error = PacketError::payload_too_large;
        return std::nullopt;
    }
    std::vector<std::uint8_t> packet(payload.size() + minimum_packet_size);
    packet[0] = message_id;
    std::copy(payload.begin(), payload.end(), packet.begin() + 1);
    packet.back() = xor8(std::span<const std::uint8_t>{packet}.first(packet.size() - 1U));
    error = PacketError::none;
    return packet;
}

bool decode_packet(ProtocolVersion version, std::span<const std::uint8_t> packet,
                   DecodedPacket &decoded, PacketError &error) noexcept
{
    decoded = {};
    if (packet.size() < minimum_packet_size)
    {
        error = PacketError::too_small;
        return false;
    }
    if (packet.size() > maximum_packet_size)
    {
        error = PacketError::too_large;
        return false;
    }
    const MessageDescriptor *message = message_by_id(version, packet.front());
    if (message == nullptr)
    {
        error = PacketError::unknown_message_id;
        return false;
    }
    if (xor8(packet.first(packet.size() - 1U)) != packet.back())
    {
        error = PacketError::checksum_mismatch;
        return false;
    }
    decoded = {message, packet.subspan(1U, packet.size() - minimum_packet_size)};
    error = PacketError::none;
    return true;
}

std::size_t BoundedReader::remaining() const noexcept
{
    return bytes_.size() - offset_;
}

bool BoundedReader::read_u8(std::uint8_t &value) noexcept
{
    if (remaining() < 1U)
        return false;
    value = bytes_[offset_++];
    return true;
}

bool BoundedReader::read_bool(bool &value) noexcept
{
    std::uint8_t encoded{};
    if (!read_u8(encoded) || encoded > 1U)
        return false;
    value = encoded != 0U;
    return true;
}

bool BoundedReader::read_i16(std::int16_t &value) noexcept
{
    std::uint16_t encoded{};
    if (!read_u16(encoded))
        return false;
    value = std::bit_cast<std::int16_t>(encoded);
    return true;
}

bool BoundedReader::read_u16(std::uint16_t &value) noexcept
{
    if (remaining() < 2U)
        return false;
    value = static_cast<std::uint16_t>(bytes_[offset_]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes_[offset_ + 1U]) << 8U);
    offset_ += 2U;
    return true;
}

bool BoundedReader::read_u32(std::uint32_t &value) noexcept
{
    if (remaining() < 4U)
        return false;
    value = static_cast<std::uint32_t>(bytes_[offset_]) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 1U]) << 8U) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 2U]) << 16U) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 3U]) << 24U);
    offset_ += 4U;
    return true;
}

bool BoundedReader::read_i32(std::int32_t &value) noexcept
{
    std::uint32_t encoded{};
    if (!read_u32(encoded))
        return false;
    value = std::bit_cast<std::int32_t>(encoded);
    return true;
}

bool BoundedReader::read_u64(std::uint64_t &value) noexcept
{
    if (remaining() < 8U)
        return false;
    value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index)
        value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8U);
    offset_ += 8U;
    return true;
}

bool BoundedReader::read_i64(std::int64_t &value) noexcept
{
    std::uint64_t encoded{};
    if (!read_u64(encoded))
        return false;
    value = std::bit_cast<std::int64_t>(encoded);
    return true;
}

bool BoundedReader::read_f32(float &value) noexcept
{
    std::uint32_t encoded{};
    if (!read_u32(encoded))
        return false;
    value = std::bit_cast<float>(encoded);
    return true;
}

bool BoundedReader::read_bytes(std::span<std::uint8_t> destination) noexcept
{
    if (destination.size() > remaining())
        return false;
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), destination.size(),
                destination.begin());
    offset_ += destination.size();
    return true;
}

bool BoundedReader::read_dotnet_string(std::string &value, std::size_t maximum_bytes)
{
    const std::size_t original_offset = offset_;
    std::uint32_t length{};
    std::uint32_t shift{};
    bool complete{};
    for (std::size_t index = 0U; index < 5U; ++index)
    {
        std::uint8_t next{};
        if (!read_u8(next))
        {
            offset_ = original_offset;
            return false;
        }
        if (index == 4U && (next & 0xF0U) != 0U)
        {
            offset_ = original_offset;
            return false;
        }
        length |= static_cast<std::uint32_t>(next & 0x7FU) << shift;
        if ((next & 0x80U) == 0U)
        {
            complete = true;
            break;
        }
        shift += 7U;
    }
    if (!complete || length > maximum_bytes || length > remaining())
    {
        offset_ = original_offset;
        return false;
    }
    const char *begin = reinterpret_cast<const char *>(bytes_.data() + offset_);
    std::string candidate(begin, static_cast<std::size_t>(length));
    if (!valid_utf8(candidate))
    {
        offset_ = original_offset;
        return false;
    }
    offset_ += length;
    value = std::move(candidate);
    return true;
}

BoundedWriter::BoundedWriter(std::size_t maximum_size) : maximum_size_{maximum_size}
{
    bytes_.reserve(std::min(maximum_size, maximum_packet_size));
}

bool BoundedWriter::can_append(std::size_t count) const noexcept
{
    return count <= maximum_size_ - std::min(maximum_size_, bytes_.size());
}

bool BoundedWriter::write_u8(std::uint8_t value)
{
    if (!can_append(1U))
        return false;
    bytes_.push_back(value);
    return true;
}

bool BoundedWriter::write_bool(bool value)
{
    return write_u8(value ? 1U : 0U);
}

bool BoundedWriter::write_i16(std::int16_t value)
{
    return write_u16(std::bit_cast<std::uint16_t>(value));
}

bool BoundedWriter::write_u16(std::uint16_t value)
{
    if (!can_append(2U))
        return false;
    bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    return true;
}

bool BoundedWriter::write_u32(std::uint32_t value)
{
    if (!can_append(4U))
        return false;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    return true;
}

bool BoundedWriter::write_i32(std::int32_t value)
{
    return write_u32(std::bit_cast<std::uint32_t>(value));
}

bool BoundedWriter::write_u64(std::uint64_t value)
{
    if (!can_append(8U))
        return false;
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    return true;
}

bool BoundedWriter::write_i64(std::int64_t value)
{
    return write_u64(std::bit_cast<std::uint64_t>(value));
}

bool BoundedWriter::write_f32(float value)
{
    return write_u32(std::bit_cast<std::uint32_t>(value));
}

bool BoundedWriter::write_bytes(std::span<const std::uint8_t> value)
{
    if (!can_append(value.size()))
        return false;
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
}

bool BoundedWriter::write_dotnet_string(std::string_view value, std::size_t maximum_bytes)
{
    if (value.size() > maximum_bytes || value.size() > std::numeric_limits<std::uint32_t>::max() ||
        !valid_utf8(value))
        return false;

    std::uint32_t length = static_cast<std::uint32_t>(value.size());
    std::array<std::uint8_t, 5U> prefix{};
    std::size_t prefix_size{};
    do
    {
        std::uint8_t next = static_cast<std::uint8_t>(length & 0x7FU);
        length >>= 7U;
        if (length != 0U)
            next |= 0x80U;
        prefix[prefix_size++] = next;
    } while (length != 0U);

    if (!can_append(prefix_size + value.size()))
        return false;
    bytes_.insert(bytes_.end(), prefix.begin(),
                  prefix.begin() + static_cast<std::ptrdiff_t>(prefix_size));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
}

} // namespace openminerz::net
