#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/peer_inventory_store.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr std::int32_t chunk_width = 16;
constexpr std::int32_t chunk_height = 128;
constexpr std::int32_t chunk_minimum_y = -64;
constexpr std::int32_t maximum_world_coordinate = 1'000'000;

std::int32_t floor_to_chunk(std::int32_t coordinate) noexcept
{
    std::int32_t quotient = coordinate / chunk_width;
    if (coordinate % chunk_width < 0)
        --quotient;
    return quotient * chunk_width;
}

bool bounded(IntVector3 value) noexcept
{
    const auto within = [](std::int32_t coordinate) {
        return coordinate >= -maximum_world_coordinate && coordinate <= maximum_world_coordinate;
    };
    return within(value.x) && within(value.y) && within(value.z);
}

bool valid_block_type(std::uint8_t block_type, ProtocolVersion protocol) noexcept
{
    return block_type <= (protocol == ProtocolVersion::classic_v3 ? static_cast<std::uint8_t>(46U)
                                                                  : static_cast<std::uint8_t>(93U));
}

bool inventory_item_from_stack(const openminerz::InventoryStack &stack, ProtocolVersion protocol,
                               InventoryItemPayload &item)
{
    if (!stack.valid() ||
        stack.count > static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max()) ||
        !std::isfinite(stack.durability))
        return false;

    InventoryItemPayload converted;
    if (!inventory_item_to_wire(stack.item, protocol, converted.item_id))
        return false;
    converted.stack_count = static_cast<std::int16_t>(stack.count);
    converted.health = stack.durability;
    const InventoryItemSerializationKind kind =
        inventory_item_serialization_kind(converted.item_id, protocol);
    if (kind == InventoryItemSerializationKind::gun)
        converted.rounds_in_clip = static_cast<std::int32_t>(stack.rounds_in_clip);
    else if (kind == InventoryItemSerializationKind::locator ||
             kind == InventoryItemSerializationKind::teleport_block)
    {
        converted.point_to_location =
            Vector3{stack.waypoint[0], stack.waypoint[1], stack.waypoint[2]};
        converted.custom_name = stack.waypoint_name;
    }
    else if (kind == InventoryItemSerializationKind::spawn_block)
        converted.point_to_location =
            Vector3{stack.waypoint[0], stack.waypoint[1], stack.waypoint[2]};
    else if (kind != InventoryItemSerializationKind::base)
        return false;

    PayloadError payload_error{};
    if (!encode_inventory_item_payload(converted, protocol, payload_error))
        return false;
    item = std::move(converted);
    return true;
}

bool inventory_slot_from_stack(const openminerz::PlayerInventory::Slot &slot,
                               ProtocolVersion protocol, InventorySlotPayload &result)
{
    if (!slot)
    {
        result.reset();
        return true;
    }
    InventoryItemPayload item;
    if (!inventory_item_from_stack(*slot, protocol, item))
        return false;
    result = std::move(item);
    return true;
}

bool player_inventory_from_save(const openminerz::PlayerInventory &source, ProtocolVersion protocol,
                                PlayerInventoryPayload &result)
{
    if (!source.valid_travel_destinations()) return false;
    PlayerInventoryPayload converted = make_empty_player_inventory(protocol);
    for (std::size_t index{}; index < source.storage().size(); ++index)
        if (!inventory_slot_from_stack(source.storage()[index], protocol, converted.storage[index]))
            return false;
    for (std::size_t tray_index{}; tray_index < converted.trays.size(); ++tray_index)
        for (std::size_t index{}; index < source.tray(tray_index).size(); ++index)
            if (!inventory_slot_from_stack(source.tray(tray_index)[index], protocol,
                                           converted.trays[tray_index][index]))
                return false;
    if (protocol == ProtocolVersion::modern_v4)
    {
        if (source.teleport_stations().size() > maximum_teleport_stations ||
            !inventory_slot_from_stack(source.spawn_point(), protocol, converted.spawn_point))
            return false;
        for (const auto &station : source.teleport_stations())
        {
            InventoryItemPayload item;
            if (!inventory_item_from_stack(station, protocol, item)) return false;
            converted.teleport_stations.push_back(std::move(item));
        }
    }
    result = std::move(converted);
    return true;
}

bool inventory_stack_from_item(const InventoryItemPayload &item, ProtocolVersion protocol,
                               openminerz::InventoryStack &result)
{
    PayloadError payload_error{};
    if (!encode_inventory_item_payload(item, protocol, payload_error) || item.item_id <= 0 ||
        item.stack_count <= 0 ||
        item.stack_count > static_cast<std::int32_t>(std::numeric_limits<std::uint16_t>::max()))
        return false;

    openminerz::InventoryItemId internal_item{};
    if (!inventory_item_from_wire(item.item_id, protocol, internal_item))
        return false;
    openminerz::InventoryStack converted{internal_item,
                                         static_cast<std::uint16_t>(item.stack_count)};
    converted.durability = item.health;
    if (item.rounds_in_clip.has_value())
    {
        if (*item.rounds_in_clip < 0 ||
            *item.rounds_in_clip >
                static_cast<std::int32_t>(std::numeric_limits<std::uint16_t>::max()))
            return false;
        converted.rounds_in_clip = static_cast<std::uint16_t>(*item.rounds_in_clip);
    }
    if (item.point_to_location.has_value())
    {
        converted.waypoint = {item.point_to_location->x, item.point_to_location->y,
                              item.point_to_location->z};
        converted.waypoint_valid = true;
    }
    if (item.custom_name.has_value())
        converted.waypoint_name = *item.custom_name;
    if (!converted.valid())
        return false;
    result = std::move(converted);
    return true;
}

bool save_slot_from_payload(const InventorySlotPayload &source, ProtocolVersion protocol,
                            openminerz::PlayerInventory::Slot &result)
{
    if (!source.has_value())
    {
        result.reset();
        return true;
    }
    openminerz::InventoryStack converted;
    if (!inventory_stack_from_item(*source, protocol, converted))
        return false;
    result = std::move(converted);
    return true;
}

bool client_block_type(std::uint8_t block_type, ProtocolVersion protocol) noexcept
{
    return block_type <=
           (protocol == ProtocolVersion::classic_v3 ? std::uint8_t{46U} : std::uint8_t{93U});
}

IntVector3 corner_from_chunk_id(std::int32_t value) noexcept
{
    const auto bits = static_cast<std::uint32_t>(value);
    const auto chunk_x = static_cast<std::int16_t>(bits & 0xFFFFU);
    const auto chunk_z = static_cast<std::int16_t>(bits >> 16U);
    return {static_cast<std::int32_t>(chunk_x) * chunk_width, chunk_minimum_y,
            static_cast<std::int32_t>(chunk_z) * chunk_width};
}

using LocationKey = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
using CrateSnapshotMap = std::map<IntVector3, InventoryStoragePayload>;

bool collect_snapshot_crates(const AuthoritativeBootstrapSnapshot &snapshot,
                             CrateSnapshotMap &crates)
{
    for (const WorldCratePayload &crate : snapshot.world.crates)
        if (!crates.emplace(crate.location, crate.inventory).second)
            return false;
    for (const ItemCratePayload &item : snapshot.crate_replay)
    {
        if (item.index < 0 ||
            item.index >= static_cast<std::int32_t>(openminerz::world_save_crate_slot_count))
            return false;
        crates[item.location][static_cast<std::size_t>(item.index)] = item.item;
    }
    return crates.size() <= maximum_world_crates;
}

bool commit_snapshot_crates(AuthoritativeBootstrapSnapshot &snapshot,
                            const CrateSnapshotMap &crates, ProtocolVersion protocol)
{
    if (crates.size() > maximum_world_crates)
        return false;
    WorldInfoPayload world = snapshot.world;
    world.crates.clear();
    world.crates.reserve(crates.size());
    for (const auto &[location, inventory] : crates)
        world.crates.push_back({location, inventory});

    PayloadError payload_error{};
    if (encode_world_info_payload(world, protocol, payload_error).has_value())
    {
        snapshot.world = std::move(world);
        snapshot.crate_replay.clear();
        return true;
    }
    if (payload_error != PayloadError::too_large)
        return false;

    world.crates.clear();
    if (!encode_world_info_payload(world, protocol, payload_error).has_value())
        return false;
    std::vector<ItemCratePayload> replay;
    for (const auto &[location, inventory] : crates)
    {
        bool has_item{};
        for (std::size_t index{}; index < inventory.size(); ++index)
        {
            if (!inventory[index].has_value())
                continue;
            ItemCratePayload item{location, static_cast<std::int32_t>(index), inventory[index]};
            if (!encode_item_crate_payload(item, protocol, payload_error).has_value())
                return false;
            replay.push_back(std::move(item));
            has_item = true;
        }
        if (!has_item)
            replay.push_back({location, 0, std::nullopt});
    }
    snapshot.world = std::move(world);
    snapshot.crate_replay = std::move(replay);
    return true;
}

} // namespace

IntVector3 chunk_corner(IntVector3 world_position) noexcept
{
    return {floor_to_chunk(world_position.x), chunk_minimum_y, floor_to_chunk(world_position.z)};
}

std::int32_t chunk_id(IntVector3 corner) noexcept
{
    const std::uint32_t z = static_cast<std::uint32_t>((corner.z / chunk_width) & 0xFFFF);
    const std::uint32_t x = static_cast<std::uint32_t>((corner.x / chunk_width) & 0xFFFF);
    return static_cast<std::int32_t>((z << 16U) | x);
}

bool encode_delta_entry(IntVector3 world_position, std::uint8_t block_type,
                        ProtocolVersion protocol, std::int32_t &entry) noexcept
{
    if (!bounded(world_position) || world_position.y < chunk_minimum_y ||
        world_position.y >= chunk_minimum_y + chunk_height ||
        !valid_block_type(block_type, protocol))
        return false;
    const IntVector3 corner = chunk_corner(world_position);
    const std::int32_t local_x = world_position.x - corner.x;
    const std::int32_t local_y = world_position.y - corner.y;
    const std::int32_t local_z = world_position.z - corner.z;
    const std::uint32_t encoded = (static_cast<std::uint32_t>(block_type) << 24U) |
                                  (static_cast<std::uint32_t>(local_x) << 16U) |
                                  (static_cast<std::uint32_t>(local_y) << 8U) |
                                  static_cast<std::uint32_t>(local_z);
    entry = static_cast<std::int32_t>(encoded);
    return true;
}

bool inventory_item_to_wire(openminerz::InventoryItemId item, ProtocolVersion protocol,
                            std::int16_t &wire_item) noexcept
{
    const auto value = static_cast<std::uint16_t>(item);
    // InventoryItem.Create reads the id and indexes AllItems with it. An id
    // that generation's Initalize never registered (SpaceKnife in both, a
    // dozen Modern enum members) makes a stock peer throw while it is still
    // reading the message, so such an item must never be written.
    if (!openminerz::item_registered(item, protocol == ProtocolVersion::modern_v4
                                               ? openminerz::InventoryProfile::modern
                                               : openminerz::InventoryProfile::classic))
    {
        wire_item = 0;
        return false;
    }
    if (value <= 122U)
    {
        wire_item = static_cast<std::int16_t>(value);
        return true;
    }
    if (protocol == ProtocolVersion::modern_v4 &&
        value >= static_cast<std::uint16_t>(openminerz::InventoryItemId::modern_chainsaw_1) &&
        value <= static_cast<std::uint16_t>(openminerz::InventoryItemId::modern_tech_door))
    {
        wire_item = static_cast<std::int16_t>(
            123U + value -
            static_cast<std::uint16_t>(openminerz::InventoryItemId::modern_chainsaw_1));
        return true;
    }
    wire_item = 0;
    return false;
}

bool inventory_item_from_wire(std::int16_t wire_item, ProtocolVersion protocol,
                              openminerz::InventoryItemId &item) noexcept
{
    if (wire_item < 0)
        return false;
    const auto value = static_cast<std::uint16_t>(wire_item);
    openminerz::InventoryItemId candidate{};
    if (value <= 122U)
        candidate = static_cast<openminerz::InventoryItemId>(value);
    else if (protocol == ProtocolVersion::modern_v4 && value <= 154U)
        candidate = static_cast<openminerz::InventoryItemId>(
            static_cast<std::uint16_t>(openminerz::InventoryItemId::modern_chainsaw_1) + value -
            123U);
    else
        return false;
    // Only ids the matching generation registers are items at all; a stock
    // peer cannot produce the others (it would throw creating them).
    if (!openminerz::item_registered(candidate, protocol == ProtocolVersion::modern_v4
                                                    ? openminerz::InventoryProfile::modern
                                                    : openminerz::InventoryProfile::classic))
        return false;
    item = candidate;
    return true;
}

bool inventory_slot_to_stack(const InventorySlotPayload &source, ProtocolVersion protocol,
                             openminerz::PlayerInventory::Slot &result)
{
    return save_slot_from_payload(source, protocol, result);
}

bool inventory_slot_to_payload(const openminerz::PlayerInventory::Slot &source,
                               ProtocolVersion protocol, InventorySlotPayload &result)
{
    return inventory_slot_from_stack(source, protocol, result);
}

bool player_inventory_to_payload(const openminerz::PlayerInventory &source,
                                 ProtocolVersion protocol, PlayerInventoryPayload &result)
{
    return player_inventory_from_save(source, protocol, result);
}

bool make_authoritative_bootstrap_snapshot(const openminerz::WorldSaveData &save,
                                           const HostSessionSettings &host,
                                           const BootstrapWorldIdentity &identity,
                                           bool inventory_is_default,
                                           AuthoritativeBootstrapSnapshot &snapshot,
                                           BootstrapSnapshotError &error)
{
    std::string settings_error;
    if (!host.validate(settings_error) || identity.name.empty() || !std::isfinite(save.day) ||
        save.day < 0.0F || save.day > 10'000'000.0F)
    {
        error = BootstrapSnapshotError::invalid_metadata;
        return false;
    }

    AuthoritativeBootstrapSnapshot converted;
    converted.world.format_version = host.protocol == ProtocolVersion::classic_v3
                                         ? WorldInfoPayload::classic_format_version
                                         : WorldInfoPayload::modern_format_version;
    converted.world.terrain_version = 1;
    converted.world.name = identity.name;
    converted.world.owner_gamer_tag = identity.owner_gamer_tag;
    converted.world.creator_gamer_tag = identity.creator_gamer_tag;
    converted.world.created_date_ticks = identity.created_date_ticks;
    converted.world.last_played_date_ticks = identity.last_played_date_ticks;
    converted.world.seed = save.seed;
    converted.day = save.day;
    converted.world.world_id = identity.world_id;
    if (save.has_player)
        converted.world.last_position = {save.player_position.x, save.player_position.y,
                                         save.player_position.z};
    converted.world.infinite_resource_mode = save.infinite_resources;
    converted.world.server_message = host.server_message;
    converted.world.server_password = host.password;
    if (host.protocol == ProtocolVersion::modern_v4)
    {
        converted.world.hell_bosses_spawned = identity.hell_bosses_spawned;
        converted.world.max_hell_boss_spawns = identity.max_hell_boss_spawns;
        converted.world.doors.reserve(save.doors.size());
        for (const auto &[location, model_name] : save.doors)
            converted.world.doors.push_back({{location[0], location[1], location[2]}, model_name});
        converted.world.spawners.reserve(save.spawners.size());
        for (const auto &[location, spawn_id] : save.spawners)
            converted.world.spawners.push_back({{location[0], location[1], location[2]}, spawn_id});
    }

    if (!player_inventory_from_save(save.inventory, host.protocol, converted.inventory))
    {
        error = BootstrapSnapshotError::incompatible_inventory;
        return false;
    }
    converted.inventory_is_default = inventory_is_default;

    converted.world.crates.reserve(save.crates.size());
    for (const auto &[location, slots] : save.crates)
    {
        WorldCratePayload crate;
        crate.location = {location[0], location[1], location[2]};
        bool has_item{};
        for (std::size_t index{}; index < slots.size(); ++index)
        {
            if (!inventory_slot_from_stack(slots[index], host.protocol, crate.inventory[index]))
            {
                error = BootstrapSnapshotError::incompatible_crate;
                return false;
            }
            if (crate.inventory[index])
            {
                converted.crate_replay.push_back(
                    {crate.location, static_cast<std::int32_t>(index), crate.inventory[index]});
                has_item = true;
            }
        }
        if (!has_item)
            converted.crate_replay.push_back({crate.location, 0, std::nullopt});
        converted.world.crates.push_back(std::move(crate));
    }

    std::map<IntVector3, std::map<LocationKey, std::int32_t>> latest_edits;
    for (const std::array<int, 4> &edit : save.edits)
    {
        const IntVector3 position{edit[0], edit[1], edit[2]};
        if (edit[3] < 0 || edit[3] > std::numeric_limits<std::uint8_t>::max())
        {
            error = BootstrapSnapshotError::invalid_world_edit;
            return false;
        }
        std::int32_t encoded{};
        if (!encode_delta_entry(position, static_cast<std::uint8_t>(edit[3]), host.protocol,
                                encoded))
        {
            error = BootstrapSnapshotError::invalid_world_edit;
            return false;
        }
        latest_edits[chunk_corner(position)][{position.x, position.y, position.z}] = encoded;
    }
    for (auto &[corner, edits] : latest_edits)
    {
        std::vector<std::int32_t> &delta = converted.chunk_deltas[corner];
        delta.reserve(edits.size());
        for (const auto &[location, value] : edits)
        {
            static_cast<void>(location);
            delta.push_back(value);
        }
        PayloadError payload_error{};
        ProvideChunkPayload check{corner, 0U, delta};
        if (!encode_provide_chunk_payload(check, payload_error))
        {
            error = BootstrapSnapshotError::packet_budget_exceeded;
            return false;
        }
    }

    PayloadError payload_error{};
    auto world_payload = encode_world_info_payload(converted.world, host.protocol, payload_error);
    if (!world_payload && payload_error == PayloadError::too_large &&
        !converted.world.crates.empty())
    {
        WorldInfoPayload without_crates = converted.world;
        without_crates.crates.clear();
        world_payload = encode_world_info_payload(without_crates, host.protocol, payload_error);
        if (world_payload)
            converted.world = std::move(without_crates);
    }
    else if (world_payload)
        converted.crate_replay.clear();
    const auto inventory_payload = encode_inventory_retrieve_payload(
        {converted.inventory, 0U, converted.inventory_is_default}, host.protocol, payload_error);
    std::vector<std::int32_t> chunk_ids;
    chunk_ids.reserve(converted.chunk_deltas.size());
    for (const auto &[corner, delta] : converted.chunk_deltas)
    {
        static_cast<void>(delta);
        chunk_ids.push_back(chunk_id(corner));
    }
    const auto delta_payload = encode_delta_list_payload(chunk_ids, payload_error);
    if (!world_payload || !inventory_payload || !delta_payload)
    {
        error = BootstrapSnapshotError::packet_budget_exceeded;
        return false;
    }
    for (const ItemCratePayload &crate_item : converted.crate_replay)
        if (!encode_item_crate_payload(crate_item, host.protocol, payload_error))
        {
            error = BootstrapSnapshotError::packet_budget_exceeded;
            return false;
        }

    snapshot = std::move(converted);
    error = BootstrapSnapshotError::none;
    return true;
}

AuthoritativeBootstrapHost::AuthoritativeBootstrapHost(ProtocolVersion protocol,
                                                       AuthoritativeBootstrapSnapshot snapshot,
                                                       std::filesystem::path inventory_directory)
    : protocol_{protocol}, inventory_directory_{std::move(inventory_directory)},
      snapshot_{std::move(snapshot)}
{
}

bool AuthoritativeBootstrapHost::register_peer(PeerId peer, std::uint8_t player_gid)
{
    if (peer == 0U || peers_.contains(peer))
        return false;
    if (!inventory_directory_.empty() && !stored_inventories_.contains(peer))
    {
        std::optional<PlayerInventoryPayload> stored;
        if (!read_peer_inventory(inventory_directory_, protocol_, peer, stored))
            return false;
        if (stored.has_value())
        {
            if (stored_inventories_.size() >= 4096U)
                return false;
            stored_inventories_.emplace(peer, std::move(*stored));
        }
    }
    const auto stored = stored_inventories_.find(peer);
    const bool has_stored_inventory = stored != stored_inventories_.end();
    PlayerInventoryPayload inventory = has_stored_inventory ? stored->second : snapshot_.inventory;
    peers_.emplace(peer, PeerState{player_gid,
                                   std::move(inventory),
                                   !has_stored_inventory && snapshot_.inventory_is_default,
                                   {}});
    return true;
}

void AuthoritativeBootstrapHost::disconnect(PeerId peer) noexcept
{
    peers_.erase(peer);
}

bool AuthoritativeBootstrapHost::store_peer_inventory(PeerId peer,
                                                      const PlayerInventoryPayload &inventory)
{
    const auto found = peers_.find(peer);
    if (found == peers_.end())
        return false;
    PayloadError payload_error{};
    if (!encode_player_inventory_payload(inventory, protocol_, payload_error).has_value())
        return false;
    constexpr std::size_t maximum_stored_peer_inventories = 4096U;
    if (!stored_inventories_.contains(peer) &&
        stored_inventories_.size() >= maximum_stored_peer_inventories)
        return false;
    // Do not acknowledge an inventory that exists only in host memory.
    if (!inventory_directory_.empty() &&
        !write_peer_inventory(inventory_directory_, protocol_, peer, inventory))
        return false;
    found->second.inventory = inventory;
    found->second.inventory_is_default = false;
    stored_inventories_.insert_or_assign(peer, inventory);
    return true;
}

bool AuthoritativeBootstrapHost::apply_crate_item(const ItemCratePayload &crate)
{
    if (!bounded(crate.location) || crate.location.y < chunk_minimum_y ||
        crate.location.y >= chunk_minimum_y + chunk_height || crate.index < 0 ||
        crate.index >= static_cast<std::int32_t>(openminerz::world_save_crate_slot_count))
        return false;
    PayloadError payload_error{};
    if (!encode_item_crate_payload(crate, protocol_, payload_error).has_value())
        return false;
    CrateSnapshotMap crates;
    if (!collect_snapshot_crates(snapshot_, crates))
        return false;
    if (!crates.contains(crate.location) && crates.size() >= maximum_world_crates)
        return false;
    crates[crate.location][static_cast<std::size_t>(crate.index)] = crate.item;
    return commit_snapshot_crates(snapshot_, crates, protocol_);
}

bool AuthoritativeBootstrapHost::destroy_crate(IntVector3 location)
{
    if (!bounded(location) || location.y < chunk_minimum_y ||
        location.y >= chunk_minimum_y + chunk_height)
        return false;
    CrateSnapshotMap crates;
    if (!collect_snapshot_crates(snapshot_, crates))
        return false;
    crates.erase(location);
    return commit_snapshot_crates(snapshot_, crates, protocol_);
}

bool AuthoritativeBootstrapHost::set_custom_door(IntVector3 location, std::uint8_t model_name)
{
    if (protocol_ != ProtocolVersion::modern_v4 || !bounded(location) ||
        location.y < chunk_minimum_y || location.y >= chunk_minimum_y + chunk_height ||
        model_name < 1U || model_name > 4U)
        return false;
    WorldInfoPayload updated = snapshot_.world;
    const auto found = std::ranges::find(updated.doors, location, &WorldDoorPayload::location);
    if (found != updated.doors.end())
        return found->model_name == model_name;
    if (updated.doors.size() >= maximum_world_doors)
        return false;
    updated.doors.push_back({location, model_name});
    PayloadError payload_error{};
    if (!encode_world_info_payload(updated, protocol_, payload_error).has_value())
        return false;
    snapshot_.world = std::move(updated);
    return true;
}

bool AuthoritativeBootstrapHost::destroy_custom_block(IntVector3 location)
{
    if (protocol_ != ProtocolVersion::modern_v4 || !bounded(location) ||
        location.y < chunk_minimum_y || location.y >= chunk_minimum_y + chunk_height)
        return false;
    WorldInfoPayload updated = snapshot_.world;
    const auto found = std::ranges::find(updated.doors, location, &WorldDoorPayload::location);
    if (found == updated.doors.end())
        return false;
    updated.doors.erase(found);
    PayloadError payload_error{};
    if (!encode_world_info_payload(updated, protocol_, payload_error).has_value())
        return false;
    snapshot_.world = std::move(updated);
    return true;
}

bool AuthoritativeBootstrapHost::update_spawner(IntVector3 location, bool started)
{
    if (protocol_ != ProtocolVersion::modern_v4 || !bounded(location) ||
        location.y < chunk_minimum_y || location.y >= chunk_minimum_y + chunk_height)
        return false;
    WorldInfoPayload updated = snapshot_.world;
    const auto found =
        std::ranges::find(updated.spawners, location, &WorldSpawnerPayload::location);
    if (!started && found == updated.spawners.end())
        return false;
    if (started && found == updated.spawners.end())
    {
        if (updated.spawners.size() >= maximum_world_spawners)
            return false;
        std::size_t ordinal{};
        std::string spawn_id;
        do
        {
            spawn_id = "spawner" + std::to_string(ordinal++);
        } while (std::ranges::any_of(updated.spawners, [&](const WorldSpawnerPayload &spawner) {
            return spawner.spawn_id == spawn_id;
        }));
        updated.spawners.push_back({location, std::move(spawn_id)});
    }
    PayloadError payload_error{};
    if (!encode_world_info_payload(updated, protocol_, payload_error).has_value())
        return false;
    snapshot_.world = std::move(updated);
    return true;
}

bool AuthoritativeBootstrapHost::update_hell_boss_progress(std::uint32_t spawned, std::uint32_t maximum)
{
    if (protocol_ != ProtocolVersion::modern_v4 || spawned > maximum || maximum > 1000) return false;
    snapshot_.world.hell_bosses_spawned = static_cast<std::int32_t>(spawned);
    snapshot_.world.max_hell_boss_spawns = static_cast<std::int32_t>(maximum);
    return true;
}

bool AuthoritativeBootstrapHost::apply_block_edit(IntVector3 position, std::uint8_t block_type)
{
    const std::array positions{position};
    return apply_block_replacements(positions, block_type);
}

bool AuthoritativeBootstrapHost::apply_block_replacements(std::span<const IntVector3> positions,
                                                          std::uint8_t block_type)
{
    if (positions.empty())
        return false;
    auto updated_chunks = snapshot_.chunk_deltas;
    std::set<IntVector3> touched;
    for (const IntVector3 position : positions)
    {
        std::int32_t encoded{};
        if (!encode_delta_entry(position, block_type, protocol_, encoded))
            return false;
        const IntVector3 corner = chunk_corner(position);
        std::vector<std::int32_t> &updated = updated_chunks[corner];
        const std::uint32_t position_bits = static_cast<std::uint32_t>(encoded) & 0x00FFFFFFU;
        const auto existing = std::ranges::find_if(updated, [position_bits](std::int32_t entry) {
            return (static_cast<std::uint32_t>(entry) & 0x00FFFFFFU) == position_bits;
        });
        if (existing == updated.end())
            updated.push_back(encoded);
        else
            *existing = encoded;
        touched.insert(corner);
    }
    PayloadError payload_error{};
    for (const IntVector3 corner : touched)
    {
        std::vector<std::int32_t> &updated = updated_chunks[corner];
        std::ranges::sort(updated, {}, [](std::int32_t entry) {
            return static_cast<std::uint32_t>(entry) & 0x00FFFFFFU;
        });
        if (!encode_provide_chunk_payload({corner, 0U, updated}, payload_error).has_value())
            return false;
    }
    snapshot_.chunk_deltas = std::move(updated_chunks);
    return true;
}

bool AuthoritativeBootstrapHost::apply_block_removals(std::span<const IntVector3> positions)
{
    return !positions.empty() && positions.size() <= maximum_received_remove_blocks &&
           apply_block_replacements(positions, 0U);
}

const AuthoritativeBootstrapSnapshot &AuthoritativeBootstrapHost::snapshot() const noexcept
{
    return snapshot_;
}

std::optional<PeerBootstrapStatus> AuthoritativeBootstrapHost::status(PeerId peer) const noexcept
{
    const auto found = peers_.find(peer);
    return found == peers_.end() ? std::nullopt
                                 : std::optional<PeerBootstrapStatus>{found->second.status};
}

bool AuthoritativeBootstrapHost::gameplay_ready(PeerId peer,
                                                std::uint8_t player_gid) const noexcept
{
    const auto found = peers_.find(peer);
    return found != peers_.end() && found->second.player_gid == player_gid &&
           found->second.status.player_announced;
}

std::optional<AuthoritativeBootstrapReply> AuthoritativeBootstrapHost::make_reply(
    PeerId peer, std::string_view output_message, std::span<const std::uint8_t> payload,
    MessageDeliveryPolicy delivery, PeerId original_sender) const
{
    const MessageDescriptor *descriptor = message_by_name(protocol_, output_message);
    if (descriptor == nullptr)
        return std::nullopt;
    PacketError packet_error{};
    const auto packet = encode_packet(protocol_, descriptor->id, payload, packet_error);
    if (!packet)
        return std::nullopt;
    return AuthoritativeBootstrapReply{peer, original_sender, *packet, delivery};
}

BootstrapHandleResult AuthoritativeBootstrapHost::handle_packet(
    PeerId sender, std::span<const std::uint8_t> application_packet, std::uint8_t channel,
    bool addressed_to_host)
{
    const auto peer = peers_.find(sender);
    if (peer == peers_.end())
        return {BootstrapPumpError::peer_not_registered, {}, {}, {}};

    DecodedPacket decoded;
    PacketError packet_error{};
    if (!addressed_to_host ||
        !decode_packet(protocol_, application_packet, decoded, packet_error) ||
        decoded.message == nullptr)
        return {BootstrapPumpError::malformed_frame, {}, {}, {}};

    PeerState &state = peer->second;
    const std::string_view input = decoded.message->name;
    const bool direct_to_host = channel == 0U;
    const bool session_broadcast = channel == 1U;
    PayloadError payload_error{};
    if (input == "RequestWorldInfoMessage")
    {
        if (!direct_to_host)
            return {BootstrapPumpError::malformed_frame, input, {}, {}};
        if (state.status.world_info_sent)
            return {BootstrapPumpError::out_of_order, input, {}, {}};
        const auto payload = encode_world_info_payload(snapshot_.world, protocol_, payload_error);
        if (!payload)
            return {BootstrapPumpError::invalid_snapshot, input, "WorldInfoMessage", {}};
        BootstrapHandleResult result{BootstrapPumpError::none, input, "WorldInfoMessage", {}};
        auto world_reply = make_reply(sender, "WorldInfoMessage", *payload,
                                      MessageDeliveryPolicy::reliable_ordered);
        if (!world_reply.has_value())
            return {BootstrapPumpError::invalid_snapshot, input, "WorldInfoMessage", {}};
        result.replies.push_back(std::move(*world_reply));
        for (const ItemCratePayload &crate_item : snapshot_.crate_replay)
        {
            const auto crate_payload =
                encode_item_crate_payload(crate_item, protocol_, payload_error);
            if (!crate_payload)
                return {BootstrapPumpError::invalid_snapshot, input, "ItemCrateMessage", {}};
            auto crate_reply = make_reply(sender, "ItemCrateMessage", *crate_payload,
                                          MessageDeliveryPolicy::reliable_ordered);
            if (!crate_reply.has_value())
                return {BootstrapPumpError::invalid_snapshot, input, "ItemCrateMessage", {}};
            result.replies.push_back(std::move(*crate_reply));
        }
        state.status.world_info_sent = true;
        return result;
    }
    if (input == "PlayerExistsMessage")
    {
        if (!session_broadcast)
            return {BootstrapPumpError::malformed_frame, input, {}, {}};
        PlayerExistsPayload presence;
        if (!decode_player_exists_payload(decoded.payload, presence, payload_error))
            return {BootstrapPumpError::malformed_frame, input, {}, {}};
        // Once this peer has announced itself, false means it is answering a
        // later newcomer's broadcast. That is ordinary gameplay relay, not a
        // duplicate bootstrap attempt.
        if (state.status.player_announced && !presence.request_response)
            return {BootstrapPumpError::unexpected_message, input, {}, {}};
        if (!state.status.world_info_sent || state.status.player_announced)
            return {BootstrapPumpError::out_of_order, input, {}, {}};
        if (!presence.request_response)
            return {BootstrapPumpError::malformed_frame, input, {}, {}};
        BootstrapHandleResult result{BootstrapPumpError::none, input, "PlayerExistsMessage", {}};
        auto self_echo = make_reply(sender, "PlayerExistsMessage", decoded.payload,
                                    MessageDeliveryPolicy::reliable_ordered, sender);
        if (!self_echo.has_value())
            return {BootstrapPumpError::invalid_snapshot, input, "PlayerExistsMessage", {}};
        result.replies.push_back(std::move(*self_echo));
        std::vector<PeerId> existing_peers;
        existing_peers.reserve(peers_.size());
        for (const auto &[peer_id, ignored] : peers_)
        {
            static_cast<void>(ignored);
            if (peer_id != sender)
                existing_peers.push_back(peer_id);
        }
        std::ranges::sort(existing_peers);
        for (const PeerId target : existing_peers)
        {
            auto announcement = make_reply(target, "PlayerExistsMessage", decoded.payload,
                                           MessageDeliveryPolicy::reliable_ordered, sender);
            if (!announcement.has_value())
                return {BootstrapPumpError::invalid_snapshot, input, "PlayerExistsMessage", {}};
            result.replies.push_back(std::move(*announcement));
        }
        if (snapshot_.host_presence.has_value())
        {
            const auto host_presence_payload =
                encode_player_exists_payload(*snapshot_.host_presence, payload_error);
            if (!host_presence_payload.has_value())
                return {BootstrapPumpError::invalid_snapshot, input, "PlayerExistsMessage", {}};
            std::vector<PeerId> presence_recipients;
            presence_recipients.reserve(existing_peers.size() + 1U);
            presence_recipients.push_back(sender);
            presence_recipients.insert(presence_recipients.end(), existing_peers.begin(),
                                       existing_peers.end());
            for (const PeerId target : presence_recipients)
            {
                auto host_presence_reply =
                    make_reply(target, "PlayerExistsMessage", *host_presence_payload,
                               MessageDeliveryPolicy::reliable_ordered);
                if (!host_presence_reply.has_value())
                    return {BootstrapPumpError::invalid_snapshot, input, "PlayerExistsMessage", {}};
                result.replies.push_back(std::move(*host_presence_reply));
            }
        }
        const auto time_payload = encode_time_of_day_payload(snapshot_.day, payload_error);
        if (!time_payload.has_value())
            return {BootstrapPumpError::invalid_snapshot, input, "TimeOfDayMessage", {}};
        std::vector<PeerId> time_recipients;
        time_recipients.reserve(existing_peers.size() + 1U);
        time_recipients.push_back(sender);
        time_recipients.insert(time_recipients.end(), existing_peers.begin(), existing_peers.end());
        for (const PeerId target : time_recipients)
        {
            auto time_reply = make_reply(target, "TimeOfDayMessage", *time_payload,
                                         MessageDeliveryPolicy::reliable_ordered);
            if (!time_reply.has_value())
                return {BootstrapPumpError::invalid_snapshot, input, "TimeOfDayMessage", {}};
            result.replies.push_back(std::move(*time_reply));
        }
        state.status.player_announced = true;
        return result;
    }
    if (input == "RequestInventoryMessage")
    {
        if (!session_broadcast)
            return {BootstrapPumpError::malformed_frame, input, {}, {}};
        // The original accepts later inventory requests too. They provide a
        // reliable read-after-store barrier during a graceful disconnect.
        if (!state.status.player_announced)
            return {BootstrapPumpError::out_of_order, input, {}, {}};
        const auto payload = encode_inventory_retrieve_payload(
            {state.inventory, state.player_gid, state.inventory_is_default}, protocol_,
            payload_error);
        if (!payload)
            return {BootstrapPumpError::invalid_snapshot,
                    input,
                    "InventoryRetrieveFromServerMessage",
                    {}};
        BootstrapHandleResult result{
            BootstrapPumpError::none, input, "InventoryRetrieveFromServerMessage", {}};
        // The recovered server broadcasts this response. Direct frames to
        // every current peer are wire-equivalent at each recipient and avoid
        // exposing an unvalidated client relay path for host-origin data.
        std::vector<PeerId> recipients;
        recipients.reserve(peers_.size());
        recipients.push_back(sender);
        for (const auto &[peer_id, ignored] : peers_)
        {
            static_cast<void>(ignored);
            if (peer_id != sender)
                recipients.push_back(peer_id);
        }
        std::ranges::sort(recipients.begin() + 1, recipients.end());
        for (const PeerId target : recipients)
        {
            auto reply = make_reply(target, "InventoryRetrieveFromServerMessage", *payload,
                                    MessageDeliveryPolicy::reliable_ordered);
            if (!reply.has_value())
                return {BootstrapPumpError::invalid_snapshot,
                        input,
                        "InventoryRetrieveFromServerMessage",
                        {}};
            result.replies.push_back(std::move(*reply));
        }
        state.status.inventory_sent = true;
        return result;
    }
    if (input == "ClientReadyForChunksMessage")
    {
        if (!direct_to_host)
            return {BootstrapPumpError::malformed_frame, input, {}, {}};
        // The stock client announces chunk readiness from BlockTerrain.Init, as
        // soon as WorldInfo arrives: ChunkCache.BECOMECLIENT calls
        // BroadcastThatWereReady() while the terrain is still loading, and
        // StartGame() sends PlayerExists only after that load has finished.
        // Requiring presence first therefore deadlocks every stock joiner on its
        // loading screen. The original host answers this request whenever it
        // arrives, including again after a client-side terrain reset.
        if (!state.status.world_info_sent)
            return {BootstrapPumpError::out_of_order, input, {}, {}};
        std::vector<std::int32_t> chunk_ids;
        chunk_ids.reserve(snapshot_.chunk_deltas.size());
        for (const auto &[corner, delta] : snapshot_.chunk_deltas)
        {
            static_cast<void>(delta);
            chunk_ids.push_back(chunk_id(corner));
        }
        const auto payload = encode_delta_list_payload(chunk_ids, payload_error);
        if (!payload)
            return {BootstrapPumpError::invalid_snapshot, input, "ProvideDeltaListMessage", {}};
        auto reply = make_reply(sender, "ProvideDeltaListMessage", *payload,
                                MessageDeliveryPolicy::reliable_unordered);
        if (!reply.has_value())
            return {BootstrapPumpError::invalid_snapshot, input, "ProvideDeltaListMessage", {}};
        state.status.delta_list_sent = true;
        return {BootstrapPumpError::none, input, "ProvideDeltaListMessage", {std::move(*reply)}};
    }
    if (input == "RequestChunkMessage")
    {
        if (!direct_to_host)
            return {BootstrapPumpError::malformed_frame, input, {}, {}};
        // Until its delta list arrives (_remoteChunks == null) the stock
        // ChunkCache asks the terrain server for every chunk it loads, so chunk
        // requests legitimately precede both the delta list and PlayerExists.
        // They are read-only and already limited to an admitted peer that has
        // been sent WorldInfo.
        if (!state.status.world_info_sent)
            return {BootstrapPumpError::out_of_order, input, {}, {}};
        RequestChunkPayload request;
        if (!decode_request_chunk_payload(decoded.payload, request, payload_error) ||
            !bounded(request.block_location))
            return {BootstrapPumpError::malformed_frame, input, {}, {}};
        const IntVector3 corner = chunk_corner(request.block_location);
        const auto found = snapshot_.chunk_deltas.find(corner);
        const std::span<const std::int32_t> delta =
            found == snapshot_.chunk_deltas.end() ? std::span<const std::int32_t>{}
                                                  : std::span<const std::int32_t>{found->second};
        const auto payload = encode_provide_chunk_payload(
            {corner, request.priority, std::vector<std::int32_t>{delta.begin(), delta.end()}},
            payload_error);
        if (!payload)
            return {BootstrapPumpError::invalid_snapshot, input, "ProvideChunkMessage", {}};
        auto reply = make_reply(sender, "ProvideChunkMessage", *payload,
                                MessageDeliveryPolicy::reliable_unordered);
        if (!reply.has_value())
            return {BootstrapPumpError::invalid_snapshot, input, "ProvideChunkMessage", {}};
        ++state.status.chunks_sent;
        return {BootstrapPumpError::none, input, "ProvideChunkMessage", {std::move(*reply)}};
    }
    return {BootstrapPumpError::unexpected_message, input, {}, {}};
}

BootstrapPumpResult AuthoritativeBootstrapHost::pump_one(LoopbackSession &session)
{
    std::optional<Datagram> datagram = session.receive_for_host();
    if (!datagram)
        return {BootstrapPumpError::no_datagram, {}, {}, {}};
    const auto status_before = status(datagram->sender);
    // LoopbackSession has one logical stock-gameplay channel and therefore
    // does not expose the legacy Steam direct/broadcast envelope. Recover the
    // source-defined route solely for this transport-neutral test adapter.
    DecodedPacket route_packet;
    PacketError route_error{};
    if (!decode_packet(protocol_, datagram->payload, route_packet, route_error) ||
        route_packet.message == nullptr)
        return {BootstrapPumpError::malformed_frame, {}, {}, {}};
    const std::uint8_t legacy_route_channel =
        route_packet.message->name == "PlayerExistsMessage" ||
                route_packet.message->name == "RequestInventoryMessage"
            ? 1U
            : 0U;
    BootstrapHandleResult handled = handle_packet(datagram->sender, datagram->payload,
                                                  legacy_route_channel, datagram->recipient == 0U);
    if (!handled.handled())
        return {handled.error, handled.input_message, handled.output_message, {}};
    for (const AuthoritativeBootstrapReply &reply : handled.replies)
    {
        SessionPacketResult transport =
            reply.original_sender == 0U
                ? session.submit_from_host(reply.peer, reply.application_packet, reply.delivery)
                : session.relay_from_host(reply.original_sender, reply.peer,
                                          reply.application_packet, reply.delivery);
        if (!transport.accepted())
        {
            if (status_before.has_value())
                peers_[datagram->sender].status = *status_before;
            return {BootstrapPumpError::transport_rejected, handled.input_message,
                    handled.output_message, std::move(transport)};
        }
    }
    return {BootstrapPumpError::none, handled.input_message, handled.output_message, {}};
}

AuthoritativeBootstrapClient::AuthoritativeBootstrapClient(
    AuthoritativeBootstrapClientOptions options)
    : options_{std::move(options)}
{
    presence_.request_response = true;
    presence_.avatar_description = options_.avatar_description;
    presence_.gamer_tag = options_.gamer_tag;
    presence_.player_id = options_.player_id;
    PayloadError error{};
    if (options_.local_gid == 0U || !encode_player_exists_payload(presence_, error).has_value())
        throw std::invalid_argument{"invalid authoritative bootstrap client options"};
}

std::optional<AuthoritativeBootstrapClientSend> AuthoritativeBootstrapClient::make_send(
    std::string_view message, std::span<const std::uint8_t> payload, MessageDeliveryPolicy delivery,
    std::optional<std::uint8_t> recipient_gid) const
{
    const MessageDescriptor *descriptor = message_by_name(options_.protocol, message);
    if (descriptor == nullptr || descriptor->transport.delivery != delivery)
        return std::nullopt;
    PacketError packet_error{};
    const auto packet = encode_packet(options_.protocol, descriptor->id, payload, packet_error);
    if (!packet.has_value())
        return std::nullopt;
    return AuthoritativeBootstrapClientSend{recipient_gid, *packet, delivery};
}

std::optional<AuthoritativeBootstrapClientSend> AuthoritativeBootstrapClient::begin(
    BootstrapClientError &error)
{
    if (state_ != State::idle)
    {
        error = BootstrapClientError::already_started;
        return std::nullopt;
    }
    auto request =
        make_send("RequestWorldInfoMessage", {}, MessageDeliveryPolicy::reliable_ordered);
    if (!request.has_value())
    {
        state_ = State::failed;
        error = BootstrapClientError::packet_encode_failed;
        return std::nullopt;
    }
    state_ = State::awaiting_world;
    error = BootstrapClientError::none;
    return request;
}

BootstrapClientHandleResult AuthoritativeBootstrapClient::fail(BootstrapClientError error,
                                                               std::string_view input_message)
{
    state_ = State::failed;
    return {error, input_message, {}, false};
}

bool AuthoritativeBootstrapClient::install_world(const WorldInfoPayload &world)
{
    openminerz::WorldSaveData converted;
    converted.seed = world.seed;
    converted.encounters.hell_bosses_spawned = static_cast<std::uint32_t>(world.hell_bosses_spawned);
    converted.encounters.max_hell_boss_spawns = static_cast<std::uint32_t>(world.max_hell_boss_spawns);
    converted.encounters.hell_countdown = world::hell_boss_interval(converted.encounters.hell_bosses_spawned);
    converted.infinite_resources = world.infinite_resource_mode;
    converted.has_player = true;
    converted.player_position = {world.last_position.x, world.last_position.y,
                                 world.last_position.z};
    for (const WorldCratePayload &crate : world.crates)
    {
        const openminerz::WorldSaveCrateKey key{crate.location.x, crate.location.y,
                                                crate.location.z};
        if (converted.crates.contains(key))
            return false;
        openminerz::WorldSaveCrateSlots slots;
        for (std::size_t index{}; index < slots.size(); ++index)
            if (!save_slot_from_payload(crate.inventory[index], options_.protocol, slots[index]))
                return false;
        converted.crates.emplace(key, std::move(slots));
    }
    for (const WorldDoorPayload &door : world.doors)
    {
        const openminerz::WorldSaveCrateKey key{door.location.x, door.location.y, door.location.z};
        if (!converted.doors.emplace(key, door.model_name).second)
            return false;
    }
    for (const WorldSpawnerPayload &spawner : world.spawners)
    {
        const openminerz::WorldSaveCrateKey key{spawner.location.x, spawner.location.y,
                                                spawner.location.z};
        if (spawner.spawn_id.empty() || !converted.spawners.emplace(key, spawner.spawn_id).second)
            return false;
    }
    staged_save_ = std::move(converted);
    return true;
}

bool AuthoritativeBootstrapClient::apply_crate_item(const ItemCratePayload &crate)
{
    if (crate.index < 0 ||
        crate.index >= static_cast<std::int32_t>(openminerz::world_save_crate_slot_count))
        return false;
    const openminerz::WorldSaveCrateKey key{crate.location.x, crate.location.y, crate.location.z};
    openminerz::PlayerInventory::Slot converted;
    if (!save_slot_from_payload(crate.item, options_.protocol, converted))
        return false;
    staged_save_.crates[key][static_cast<std::size_t>(crate.index)] = std::move(converted);
    return true;
}

bool AuthoritativeBootstrapClient::install_inventory(const PlayerInventoryPayload &inventory)
{
    if (inventory.trays.empty())
        return false;
    openminerz::PlayerInventory converted;
    converted.set_profile(options_.protocol == ProtocolVersion::modern_v4
                              ? openminerz::InventoryProfile::modern
                              : openminerz::InventoryProfile::classic);
    for (openminerz::PlayerInventory::Slot &slot : converted.storage())
        slot.reset();
    for (auto &tray : converted.trays())
        for (openminerz::PlayerInventory::Slot &slot : tray)
            slot.reset();
    for (std::size_t index{}; index < converted.storage().size(); ++index)
        if (!save_slot_from_payload(inventory.storage[index], options_.protocol,
                                    converted.storage()[index]))
            return false;
    if (inventory.trays.size() > converted.trays().size())
        return false;
    for (std::size_t tray_index{}; tray_index < inventory.trays.size(); ++tray_index)
        for (std::size_t index{}; index < converted.tray(tray_index).size(); ++index)
            if (!save_slot_from_payload(inventory.trays[tray_index][index], options_.protocol,
                                        converted.tray(tray_index)[index]))
                return false;
    if (!save_slot_from_payload(inventory.spawn_point, options_.protocol, converted.spawn_point()))
        return false;
    for (const auto &station : inventory.teleport_stations)
    {
        openminerz::PlayerInventory::Slot slot;
        if (!save_slot_from_payload(station, options_.protocol, slot)) return false;
        if (slot) converted.teleport_stations().push_back(std::move(*slot));
    }
    if (!converted.valid_travel_destinations()) return false;
    staged_save_.inventory = std::move(converted);
    return true;
}

bool AuthoritativeBootstrapClient::install_chunk(const ProvideChunkPayload &chunk)
{
    if (chunk.priority != 0U || !expected_chunks_.contains(chunk.block_location) ||
        received_chunks_.contains(chunk.block_location) ||
        chunk_corner(chunk.block_location) != chunk.block_location)
        return false;

    std::set<IntVector3> positions;
    std::vector<std::array<int, 4>> edits;
    edits.reserve(chunk.delta.size());
    for (const std::int32_t encoded_entry : chunk.delta)
    {
        const auto encoded = static_cast<std::uint32_t>(encoded_entry);
        const std::uint8_t block_type = static_cast<std::uint8_t>(encoded >> 24U);
        const std::uint8_t local_x = static_cast<std::uint8_t>(encoded >> 16U);
        const std::uint8_t local_y = static_cast<std::uint8_t>(encoded >> 8U);
        const std::uint8_t local_z = static_cast<std::uint8_t>(encoded);
        if (local_x >= chunk_width || local_y >= chunk_height || local_z >= chunk_width ||
            !client_block_type(block_type, options_.protocol))
            return false;
        const IntVector3 position{chunk.block_location.x + local_x,
                                  chunk.block_location.y + local_y,
                                  chunk.block_location.z + local_z};
        if (!bounded(position) || !positions.insert(position).second)
            return false;
        edits.push_back({position.x, position.y, position.z, block_type});
    }
    staged_save_.edits.insert(staged_save_.edits.end(), edits.begin(), edits.end());
    received_chunks_.insert(chunk.block_location);
    return true;
}

BootstrapClientHandleResult AuthoritativeBootstrapClient::handle_packet(
    std::uint8_t sender_gid, std::span<const std::uint8_t> application_packet)
{
    if (state_ == State::idle)
        return {BootstrapClientError::not_started, {}, {}, false};
    if (state_ == State::failed)
        return {BootstrapClientError::already_failed, {}, {}, false};
    if (state_ == State::complete)
        return {BootstrapClientError::out_of_order, {}, {}, true};

    DecodedPacket decoded;
    PacketError packet_error{};
    if (!decode_packet(options_.protocol, application_packet, decoded, packet_error) ||
        decoded.message == nullptr)
        return fail(BootstrapClientError::malformed_frame, {});
    const std::string_view input = decoded.message->name;
    const bool bootstrap_message =
        input == "WorldInfoMessage" || input == "ItemCrateMessage" ||
        input == "PlayerExistsMessage" || input == "InventoryRetrieveFromServerMessage" ||
        input == "ProvideDeltaListMessage" || input == "ProvideChunkMessage";
    if (!bootstrap_message)
        return {BootstrapClientError::unexpected_message, input, {}, false};

    PayloadError payload_error{};
    if (input == "WorldInfoMessage")
    {
        if (state_ != State::awaiting_world)
            return fail(BootstrapClientError::out_of_order, input);
        if (sender_gid != 0U)
            return fail(BootstrapClientError::unexpected_sender, input);
        WorldInfoPayload world;
        if (!decode_world_info_payload(decoded.payload, options_.protocol, world, payload_error) ||
            !install_world(world))
            return fail(BootstrapClientError::invalid_world, input);
        const auto presence_payload = encode_player_exists_payload(presence_, payload_error);
        const auto presence = presence_payload.has_value()
                                  ? make_send("PlayerExistsMessage", *presence_payload,
                                              MessageDeliveryPolicy::reliable_ordered, std::nullopt)
                                  : std::nullopt;
        if (!presence.has_value())
            return fail(BootstrapClientError::packet_encode_failed, input);
        world_info_ = std::move(world);
        state_ = State::awaiting_presence;
        return {BootstrapClientError::none, input, {*presence}, false};
    }
    if (input == "ItemCrateMessage")
    {
        // Only this part of the host transcript is initial crate replay. A
        // player's live crate edit can arrive at any other bootstrap stage;
        // the caller retains it for ordered application after installation.
        if (state_ != State::awaiting_presence || sender_gid != 0U)
            return {BootstrapClientError::unexpected_message, input, {}, false};
        ItemCratePayload crate;
        if (!decode_item_crate_payload(decoded.payload, options_.protocol, crate, payload_error) ||
            !apply_crate_item(crate))
            return fail(BootstrapClientError::invalid_world, input);
        return {BootstrapClientError::none, input, {}, false};
    }
    if (input == "PlayerExistsMessage")
    {
        if (sender_gid != options_.local_gid)
            return {BootstrapClientError::unexpected_message, input, {}, false};
        if (state_ == State::awaiting_world)
            return fail(BootstrapClientError::out_of_order, input);
        PlayerExistsPayload echo;
        if (!decode_player_exists_payload(decoded.payload, echo, payload_error) ||
            echo != presence_)
            return fail(BootstrapClientError::invalid_presence_echo, input);
        // The application performs DNA's synchronous local broadcast echo.
        // Native hosts also return that announcement over the network. Accept
        // only the identical repeat after the local echo advanced bootstrap;
        // requesting inventory again would restart the ordered transcript.
        if (state_ != State::awaiting_presence)
            return {BootstrapClientError::none, input, {}, false};
        auto request = make_send("RequestInventoryMessage", {},
                                 MessageDeliveryPolicy::reliable_ordered, std::nullopt);
        if (!request.has_value())
            return fail(BootstrapClientError::packet_encode_failed, input);
        state_ = State::awaiting_inventory;
        return {BootstrapClientError::none, input, {*request}, false};
    }
    if (input == "InventoryRetrieveFromServerMessage")
    {
        if (sender_gid != 0U)
            return fail(BootstrapClientError::unexpected_sender, input);
        InventoryRetrievePayload inventory;
        if (!decode_inventory_retrieve_payload(decoded.payload, options_.protocol, inventory,
                                               payload_error))
            return fail(BootstrapClientError::invalid_inventory, input);
        if (inventory.player_id != options_.local_gid)
            return {BootstrapClientError::unexpected_message, input, {}, false};
        if (state_ != State::awaiting_inventory)
            return fail(BootstrapClientError::out_of_order, input);
        if (!install_inventory(inventory.inventory))
            return fail(BootstrapClientError::invalid_inventory, input);
        auto request =
            make_send("ClientReadyForChunksMessage", {}, MessageDeliveryPolicy::reliable_ordered);
        if (!request.has_value())
            return fail(BootstrapClientError::packet_encode_failed, input);
        native_inventory_ = std::move(inventory.inventory);
        state_ = State::awaiting_delta_list;
        return {BootstrapClientError::none, input, {*request}, false};
    }
    if (input == "ProvideDeltaListMessage")
    {
        if (state_ != State::awaiting_delta_list)
            return fail(BootstrapClientError::out_of_order, input);
        if (sender_gid != 0U)
            return fail(BootstrapClientError::unexpected_sender, input);
        std::vector<std::int32_t> chunk_ids;
        if (!decode_delta_list_payload(decoded.payload, chunk_ids, payload_error))
            return fail(BootstrapClientError::invalid_delta_list, input);
        std::set<IntVector3> expected;
        std::vector<AuthoritativeBootstrapClientSend> requests;
        requests.reserve(chunk_ids.size());
        for (const std::int32_t id : chunk_ids)
        {
            const IntVector3 corner = corner_from_chunk_id(id);
            if (chunk_id(corner) != id || !bounded(corner) || !expected.insert(corner).second)
                return fail(BootstrapClientError::invalid_delta_list, input);
            const auto payload = encode_request_chunk_payload({corner, 0U}, payload_error);
            const auto request = payload.has_value()
                                     ? make_send("RequestChunkMessage", *payload,
                                                 MessageDeliveryPolicy::reliable_unordered)
                                     : std::nullopt;
            if (!request.has_value())
                return fail(BootstrapClientError::packet_encode_failed, input);
            requests.push_back(*request);
        }
        expected_chunks_ = std::move(expected);
        if (expected_chunks_.empty())
        {
            completed_save_ = staged_save_;
            state_ = State::complete;
            return {BootstrapClientError::none, input, {}, true};
        }
        state_ = State::awaiting_chunks;
        return {BootstrapClientError::none, input, std::move(requests), false};
    }
    if (state_ != State::awaiting_chunks)
        return fail(BootstrapClientError::out_of_order, input);
    if (sender_gid != 0U)
        return fail(BootstrapClientError::unexpected_sender, input);
    ProvideChunkPayload chunk;
    if (!decode_provide_chunk_payload(decoded.payload, chunk, payload_error) ||
        !install_chunk(chunk))
        return fail(BootstrapClientError::invalid_chunk, input);
    if (received_chunks_.size() == expected_chunks_.size())
    {
        completed_save_ = staged_save_;
        state_ = State::complete;
    }
    return {BootstrapClientError::none, input, {}, state_ == State::complete};
}

bool AuthoritativeBootstrapClient::complete() const noexcept
{
    return state_ == State::complete;
}

bool AuthoritativeBootstrapClient::failed() const noexcept
{
    return state_ == State::failed;
}

const std::optional<openminerz::WorldSaveData> &AuthoritativeBootstrapClient::world_save()
    const noexcept
{
    return completed_save_;
}

const std::optional<WorldInfoPayload> &AuthoritativeBootstrapClient::world_info() const noexcept
{
    return world_info_;
}

const std::optional<PlayerInventoryPayload> &AuthoritativeBootstrapClient::native_inventory()
    const noexcept
{
    return native_inventory_;
}

} // namespace openminerz::net
