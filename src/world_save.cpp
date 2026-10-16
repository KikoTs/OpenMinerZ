#include "openminerz/world_save.hpp"
#include "openminerz/atomic_file.hpp"
#include "openminerz/net/steam_bridge_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace openminerz
{
namespace
{
constexpr std::size_t maximum_save_bytes = 128U * 1024U * 1024U;

std::span<const std::uint8_t> bytes_of(std::string_view value) noexcept
{
    return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

bool read_bounded(std::istream &input, std::string &bytes)
{
    std::array<char, 8192> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (bytes.size() > maximum_save_bytes - count)
            return false;
        bytes.append(buffer.data(), count);
    }
    return input.eof() && !input.bad();
}

std::filesystem::path backup_path(std::filesystem::path path)
{
    path += ".bak";
    return path;
}

void clear_inventory(PlayerInventory &inventory) noexcept
{
    for (auto &slot : inventory.storage())
        slot.reset();
    for (auto &tray : inventory.trays())
        for (auto &slot : tray)
            slot.reset();
}

[[nodiscard]] bool valid_item(int item) noexcept
{
    return item > static_cast<int>(InventoryItemId::bare_hands) && item <= 65535 &&
           valid_inventory_item_id(static_cast<InventoryItemId>(item));
}

[[nodiscard]] bool valid_owner(std::string_view owner) noexcept
{
    return owner.size() <= 64U && std::ranges::none_of(owner, [](unsigned char character) {
               return character < 0x20U || character == 0x7FU;
           });
}

[[nodiscard]] PlayerInventory::Slot *linear_inventory_slot(PlayerInventory &inventory,
                                                           std::size_t index,
                                                           bool second_tray_supported) noexcept
{
    if (index < PlayerInventory::storage_size)
        return &inventory.storage()[index];
    const std::size_t tray_index = index - PlayerInventory::storage_size;
    const std::size_t tray_slots =
        PlayerInventory::tray_size * (second_tray_supported ? PlayerInventory::tray_count : 1U);
    if (tray_index >= tray_slots)
        return nullptr;
    return &inventory.tray(tray_index /
                           PlayerInventory::tray_size)[tray_index % PlayerInventory::tray_size];
}

void apply_navigation(PlayerInventory::Slot &slot, std::array<float, 3> waypoint, std::string name)
{
    if (!slot.has_value())
        return;
    slot->waypoint = waypoint;
    slot->waypoint_name = std::move(name);
    slot->waypoint_valid = true;
}

} // namespace

void WorldHistory::begin_session(std::int64_t now, std::string_view player, bool new_world)
{
    if (new_world)
    {
        *this = {};
        creator = player;
        created_unix_seconds = now;
    }
    if (tracked_since_unix_seconds == 0)
        tracked_since_unix_seconds = now;
    if (sessions < 1'000'000'000U)
        ++sessions;
}

void WorldHistory::add_play_time(double seconds) noexcept
{
    if (std::isfinite(seconds) && seconds > 0.0)
        play_seconds = std::min(1.0e12, play_seconds + seconds);
}

static bool write_world_save_body(std::ostream &output, const WorldSaveData &save)
{
    if (!output || !valid_owner(save.owner) || save.saved_unix_seconds < 0 ||
        !save.inventory.valid_travel_destinations() ||
        save.encounters.carved_cells > 100'000'000'000ULL ||
        save.encounters.max_hell_boss_spawns > 1000 ||
        save.encounters.hell_bosses_spawned > save.encounters.max_hell_boss_spawns ||
        save.encounters.hell_countdown > world::hell_boss_interval(save.encounters.hell_bosses_spawned) ||
        save.encounters.populated_chunks.size() > world::maximum_encounter_chunks ||
        std::ranges::any_of(save.encounters.populated_chunks, [](const auto &cell) {
            return cell[0] < -62500 || cell[0] > 62500 || cell[1] < -62500 || cell[1] > 62500;
        }) ||
        save.doors.size() > 4096U || save.spawners.size() > 4096U ||
        std::ranges::any_of(save.doors, [](const auto &entry) { return entry.second > 4U; }) ||
        std::ranges::any_of(save.spawners, [](const auto &entry) {
            return entry.second.empty() || entry.second.size() > 1024U;
        }))
        return false;
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "omw 9\n";
    if (!save.owner.empty())
        output << "owner " << std::quoted(save.owner) << '\n';
    if (save.saved_unix_seconds != 0)
        output << "saved " << save.saved_unix_seconds << '\n';
    if (!valid_owner(save.history.creator))
        return false;
    output << "history " << std::quoted(save.history.creator) << ' '
           << save.history.created_unix_seconds << ' ' << save.history.tracked_since_unix_seconds
           << ' ' << std::setprecision(std::numeric_limits<double>::max_digits10)
           << save.history.play_seconds << ' ' << save.history.sessions << ' '
           << save.history.deaths << '\n'
           << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "seed " << save.seed << '\n';
    output << "mode " << static_cast<int>(save.mode) << '\n';
    output << "difficulty " << static_cast<int>(save.difficulty) << '\n';
    output << "infinite " << (save.infinite_resources ? 1 : 0) << '\n';
    output << "day " << save.day << '\n';
    if (save.has_player)
        output << "player " << save.player_position.x << ' ' << save.player_position.y << ' '
               << save.player_position.z << ' ' << save.player_yaw << ' ' << save.player_pitch
               << ' ' << save.player_health << '\n';
    if (save.has_distance_origin)
        output << "origin " << save.distance_origin_x << ' ' << save.distance_origin_z << '\n';
    output << "progress " << save.dragon_distance_index << ' ' << save.max_distance << ' '
           << save.next_song_index << '\n';
    output << "profile " << static_cast<int>(save.inventory.profile()) << '\n';
    output << "selection " << save.inventory.current_tray_index() << ' '
           << save.inventory.selected_index() << '\n';
    const auto write_destination = [&](std::string_view kind, const InventoryStack &stack) {
        output << kind << ' ' << stack.waypoint[0] << ' ' << stack.waypoint[1] << ' '
               << stack.waypoint[2] << ' ' << stack.durability << ' ' << std::quoted(stack.waypoint_name) << '\n';
    };
    if (save.inventory.spawn_point()) write_destination("spawn", *save.inventory.spawn_point());
    for (const auto &station : save.inventory.teleport_stations()) write_destination("station", station);
    if (save.held_stack)
    {
        const auto &stack = *save.held_stack;
        output << "cursor " << static_cast<int>(stack.item) << ' ' << stack.count << ' '
               << stack.rounds_in_clip << ' ' << stack.durability << ' '
               << (stack.waypoint_valid ? 1 : 0) << ' ' << stack.waypoint[0] << ' '
               << stack.waypoint[1] << ' ' << stack.waypoint[2] << ' '
               << std::quoted(stack.waypoint_name) << '\n';
    }

    const auto write_slot = [&](std::size_t index, const PlayerInventory::Slot &slot) {
        if (!slot.has_value())
            return;
        output << "inv " << index << ' ' << static_cast<int>(slot->item) << ' ' << slot->count
               << ' ' << slot->rounds_in_clip << '\n';
        if (slot->durability != 1.0F)
            output << "dur " << index << ' ' << slot->durability << '\n';
        if (slot->waypoint_valid)
            output << "nav " << index << ' ' << slot->waypoint[0] << ' ' << slot->waypoint[1] << ' '
                   << slot->waypoint[2] << ' ' << std::quoted(slot->waypoint_name) << '\n';
    };
    for (std::size_t index = 0; index < PlayerInventory::storage_size; ++index)
        write_slot(index, save.inventory.storage()[index]);
    for (std::size_t tray_index = 0; tray_index < PlayerInventory::tray_count; ++tray_index)
        for (std::size_t index = 0; index < PlayerInventory::tray_size; ++index)
            write_slot(PlayerInventory::storage_size + tray_index * PlayerInventory::tray_size +
                           index,
                       save.inventory.tray(tray_index)[index]);

    for (const auto &[location, slots] : save.crates)
        for (std::size_t index = 0; index < slots.size(); ++index)
            if (slots[index].has_value())
            {
                const InventoryStack &stack = *slots[index];
                output << "crate " << location[0] << ' ' << location[1] << ' ' << location[2] << ' '
                       << index << ' ' << static_cast<int>(stack.item) << ' ' << stack.count << ' '
                       << stack.rounds_in_clip << '\n';
                if (stack.durability != 1.0F)
                    output << "crate_dur " << location[0] << ' ' << location[1] << ' '
                           << location[2] << ' ' << index << ' ' << stack.durability << '\n';
                if (stack.waypoint_valid)
                    output << "crate_nav " << location[0] << ' ' << location[1] << ' '
                           << location[2] << ' ' << index << ' ' << stack.waypoint[0] << ' '
                           << stack.waypoint[1] << ' ' << stack.waypoint[2] << ' '
                           << std::quoted(stack.waypoint_name) << '\n';
            }
    for (const auto &[location, model] : save.doors)
        output << "door " << location[0] << ' ' << location[1] << ' ' << location[2] << ' '
               << static_cast<unsigned int>(model) << '\n';
    for (const auto &[location, spawn_id] : save.spawners)
        output << "spawner " << location[0] << ' ' << location[1] << ' ' << location[2] << ' '
               << std::quoted(spawn_id) << '\n';
    const auto &encounters = save.encounters;
    output << "encounters " << encounters.carved_cells << ' ' << encounters.hell_bosses_spawned << ' '
           << encounters.max_hell_boss_spawns << ' ' << encounters.hell_countdown << '\n';
    for (const auto &chunk : encounters.populated_chunks)
        output << "encounter_chunk " << chunk[0] << ' ' << chunk[1] << '\n';
    for (const std::array<int, 4> &edit : save.edits)
        output << "edit " << edit[0] << ' ' << edit[1] << ' ' << edit[2] << ' ' << edit[3] << '\n';
    return static_cast<bool>(output);
}

static bool read_world_save_body(std::istream &source, WorldSaveData &save)
{
    if (!source)
        return false;
    WorldSaveData decoded;
    clear_inventory(decoded.inventory);
    bool recognized{};
    std::string keyword;
    std::string line;
    while (std::getline(source, line))
    {
        std::istringstream input{line};
        if (!(input >> keyword))
            continue;
        if (keyword == "omw")
        {
            if (recognized)
                return false;
            input >> decoded.version;
            if (decoded.version < 1 || decoded.version > 9)
                return false;
            recognized = true;
        }
        else if (!recognized)
            return false;
        else if (keyword == "owner")
        {
            input >> std::quoted(decoded.owner);
            if (!valid_owner(decoded.owner))
                return false;
        }
        else if (keyword == "saved")
        {
            input >> decoded.saved_unix_seconds;
            if (decoded.saved_unix_seconds < 0)
                return false;
        }
        else if (keyword == "history")
        {
            auto &history = decoded.history;
            input >> std::quoted(history.creator) >> history.created_unix_seconds >>
                history.tracked_since_unix_seconds >> history.play_seconds >> history.sessions >>
                history.deaths;
            if (decoded.version < 8 || !valid_owner(history.creator) ||
                history.created_unix_seconds < 0 || history.tracked_since_unix_seconds < 0 ||
                !std::isfinite(history.play_seconds) || history.play_seconds < 0.0 ||
                history.play_seconds > 1.0e12 || history.sessions > 1'000'000'000U ||
                history.deaths > 1'000'000'000U)
                return false;
        }
        else if (keyword == "seed")
            input >> decoded.seed;
        else if (keyword == "mode")
        {
            int mode{};
            input >> mode;
            if (mode < 0 || mode > 5)
                return false;
            decoded.mode = static_cast<GameMode>(std::clamp(mode, 0, 5));
        }
        else if (keyword == "difficulty")
        {
            int difficulty{};
            input >> difficulty;
            if (difficulty < 0 || difficulty > 3)
                return false;
            decoded.difficulty = static_cast<GameDifficulty>(std::clamp(difficulty, 0, 3));
        }
        else if (keyword == "infinite")
        {
            int infinite{};
            input >> infinite;
            if (infinite != 0 && infinite != 1)
                return false;
            decoded.infinite_resources = infinite != 0;
        }
        else if (keyword == "day")
            input >> decoded.day;
        else if (keyword == "player")
        {
            input >> decoded.player_position.x >> decoded.player_position.y >>
                decoded.player_position.z >> decoded.player_yaw >> decoded.player_pitch >>
                decoded.player_health;
            decoded.has_player = true;
        }
        else if (keyword == "origin")
        {
            input >> decoded.distance_origin_x >> decoded.distance_origin_z;
            decoded.has_distance_origin = true;
        }
        else if (keyword == "progress")
            input >> decoded.dragon_distance_index >> decoded.max_distance >>
                decoded.next_song_index;
        else if (keyword == "profile")
        {
            int profile{};
            input >> profile;
            if (profile < 0 || profile > 1)
                return false;
            if (decoded.version >= 5)
                decoded.inventory.set_profile(profile == static_cast<int>(InventoryProfile::modern)
                                                  ? InventoryProfile::modern
                                                  : InventoryProfile::classic);
        }
        else if (keyword == "selection")
        {
            std::size_t tray{}, slot{};
            input >> tray >> slot;
            if (decoded.version < 7 || tray >= PlayerInventory::tray_count ||
                slot >= PlayerInventory::tray_size ||
                (tray != 0 && decoded.inventory.profile() == InventoryProfile::classic))
                return false;
            if (tray != decoded.inventory.current_tray_index())
                decoded.inventory.switch_current_tray();
            decoded.inventory.select(slot);
        }
        else if (keyword == "spawn" || keyword == "station")
        {
            std::array<float, 3> point{};
            float health{};
            std::string name;
            if (decoded.version < 9 || !(input >> point[0] >> point[1] >> point[2] >> health >> std::quoted(name)) ||
                !std::isfinite(health) || health < 0 || health > 1 || name.size() > 1024 ||
                std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; }) ||
                decoded.inventory.profile() != InventoryProfile::modern)
                return false;
            if (keyword == "spawn")
            {
                if (decoded.inventory.spawn_point() || !decoded.inventory.register_spawn_point(point)) return false;
                decoded.inventory.spawn_point()->durability = health;
                decoded.inventory.spawn_point()->waypoint_name = std::move(name);
            }
            else
            {
                const auto count = decoded.inventory.teleport_stations().size();
                if (!decoded.inventory.register_teleport_station(point, std::move(name)) ||
                    decoded.inventory.teleport_stations().size() != count + 1) return false;
                decoded.inventory.teleport_stations().back().durability = health;
            }
        }
        else if (keyword == "cursor")
        {
            int item{}, count{}, rounds{}, navigation{};
            InventoryStack stack;
            input >> item >> count >> rounds >> stack.durability >> navigation >>
                stack.waypoint[0] >> stack.waypoint[1] >> stack.waypoint[2] >>
                std::quoted(stack.waypoint_name);
            if (decoded.version < 7 || decoded.held_stack || !valid_item(item) || count <= 0 ||
                count > 65535 || rounds < 0 || rounds > 65535 || !std::isfinite(stack.durability) ||
                stack.durability < 0.0F || stack.durability > 1.0F ||
                (navigation != 0 && navigation != 1) ||
                std::ranges::any_of(stack.waypoint,
                                    [](float value) { return !std::isfinite(value); }) ||
                stack.waypoint_name.size() > 1024U)
                return false;
            stack.item = static_cast<InventoryItemId>(item);
            stack.block = item_block(stack.item);
            stack.count = static_cast<std::uint16_t>(count);
            stack.rounds_in_clip = static_cast<std::uint16_t>(rounds);
            stack.waypoint_valid = navigation != 0;
            decoded.held_stack = std::move(stack);
        }
        else if (keyword == "inv")
        {
            std::size_t index{};
            int item{};
            int count{};
            int rounds{};
            input >> index >> item >> count >> rounds;
            if (valid_item(item) && count > 0 && count <= 65535 && rounds >= 0 && rounds <= 65535)
            {
                InventoryStack stack{static_cast<InventoryItemId>(item),
                                     static_cast<std::uint16_t>(count)};
                stack.rounds_in_clip = static_cast<std::uint16_t>(rounds);
                if (PlayerInventory::Slot *slot =
                        linear_inventory_slot(decoded.inventory, index, decoded.version >= 5))
                    *slot = stack;
                else if (decoded.version >= 7)
                    return false;
            }
            else
                return false;
        }
        else if (keyword == "nav")
        {
            std::size_t index{};
            std::array<float, 3> waypoint{};
            std::string name;
            input >> index >> waypoint[0] >> waypoint[1] >> waypoint[2] >> std::quoted(name);
            if (PlayerInventory::Slot *slot =
                    linear_inventory_slot(decoded.inventory, index, decoded.version >= 5))
                apply_navigation(*slot, waypoint, std::move(name));
            else
                return false;
        }
        else if (keyword == "dur")
        {
            std::size_t index{};
            float durability{1.0F};
            input >> index >> durability;
            PlayerInventory::Slot *slot =
                linear_inventory_slot(decoded.inventory, index, decoded.version >= 5);
            if (slot != nullptr && slot->has_value())
                (*slot)->durability = std::clamp(durability, 0.0F, 1.0F);
            else
                return false;
        }
        else if (keyword == "crate")
        {
            WorldSaveCrateKey location{};
            std::size_t index{};
            int item{};
            int count{};
            int rounds{};
            input >> location[0] >> location[1] >> location[2] >> index >> item >> count >> rounds;
            if (index < world_save_crate_slot_count && valid_item(item) && count > 0 &&
                count <= 65535 && rounds >= 0 && rounds <= 65535)
            {
                InventoryStack stack{static_cast<InventoryItemId>(item),
                                     static_cast<std::uint16_t>(count)};
                stack.rounds_in_clip = static_cast<std::uint16_t>(rounds);
                decoded.crates[location][index] = stack;
            }
            else
                return false;
        }
        else if (keyword == "crate_nav")
        {
            WorldSaveCrateKey location{};
            std::size_t index{};
            std::array<float, 3> waypoint{};
            std::string name;
            input >> location[0] >> location[1] >> location[2] >> index >> waypoint[0] >>
                waypoint[1] >> waypoint[2] >> std::quoted(name);
            if (index < world_save_crate_slot_count)
                apply_navigation(decoded.crates[location][index], waypoint, std::move(name));
            else
                return false;
        }
        else if (keyword == "crate_dur")
        {
            WorldSaveCrateKey location{};
            std::size_t index{};
            float durability{1.0F};
            input >> location[0] >> location[1] >> location[2] >> index >> durability;
            if (index < world_save_crate_slot_count && decoded.crates[location][index].has_value())
                decoded.crates[location][index]->durability = std::clamp(durability, 0.0F, 1.0F);
            else
                return false;
        }
        else if (keyword == "door")
        {
            WorldSaveCrateKey location{};
            unsigned int model{};
            input >> location[0] >> location[1] >> location[2] >> model;
            if (model <= 4U)
                decoded.doors.insert_or_assign(location, static_cast<std::uint8_t>(model));
            else
                return false;
        }
        else if (keyword == "spawner")
        {
            WorldSaveCrateKey location{};
            std::string spawn_id;
            input >> location[0] >> location[1] >> location[2] >> std::quoted(spawn_id);
            if (!spawn_id.empty() && spawn_id.size() <= 1024U)
                decoded.spawners.insert_or_assign(location, std::move(spawn_id));
            else
                return false;
        }
        else if (keyword == "encounters")
        {
            auto &state = decoded.encounters;
            if (decoded.version < 9 || !(input >> state.carved_cells >> state.hell_bosses_spawned >>
                state.max_hell_boss_spawns >> state.hell_countdown) || state.carved_cells > 100'000'000'000ULL ||
                state.max_hell_boss_spawns > 1000 || state.hell_bosses_spawned > state.max_hell_boss_spawns ||
                state.hell_countdown > world::hell_boss_interval(state.hell_bosses_spawned))
                return false;
        }
        else if (keyword == "encounter_chunk")
        {
            std::array<int, 2> cell{};
            if (decoded.version < 9 || !(input >> cell[0] >> cell[1]) ||
                cell[0] < -62500 || cell[0] > 62500 || cell[1] < -62500 || cell[1] > 62500 ||
                decoded.encounters.populated_chunks.size() >= world::maximum_encounter_chunks ||
                !decoded.encounters.populated_chunks.insert(cell).second)
                return false;
        }
        else if (keyword == "edit")
        {
            std::array<int, 4> edit{};
            input >> edit[0] >> edit[1] >> edit[2] >> edit[3];
            if (edit[3] >= static_cast<int>(world::BlockType::empty) &&
                edit[3] <= static_cast<int>(world::BlockType::uninitialized))
                decoded.edits.push_back(edit);
            else
                return false;
        }
        else
        {
            if (decoded.version >= 7)
                return false;
            std::string ignored;
            std::getline(input, ignored);
        }
        if (input.fail() || input.bad())
            return false;
        input >> std::ws;
        if (!input.eof())
            return false;
    }
    if (!recognized || source.bad() || !std::isfinite(decoded.day) ||
        !std::isfinite(decoded.player_health) || decoded.doors.size() > 4096U ||
        decoded.spawners.size() > 4096U)
        return false;
    save = std::move(decoded);
    return true;
}

bool write_world_save(std::ostream &output, const WorldSaveData &save)
{
    std::ostringstream body;
    if (!write_world_save_body(body, save))
        return false;
    const auto bytes = body.str();
    if (bytes.size() > maximum_save_bytes - 64U)
        return false;
    // Validate before replacing an existing checkpoint, including non-finite values
    // and invalid item state that would otherwise make the new file unreadable.
    std::istringstream validation{bytes};
    WorldSaveData checked;
    if (!read_world_save_body(validation, checked))
        return false;
    output << bytes << "complete " << bytes.size() << ' '
           << net::steam_bridge_crc32(bytes_of(bytes)) << '\n';
    return static_cast<bool>(output);
}

bool read_world_save(std::istream &input, WorldSaveData &save)
{
    std::string bytes;
    if (!read_bounded(input, bytes))
        return false;
    std::istringstream header{bytes};
    std::string magic;
    int version{};
    if (!(header >> magic >> version) || magic != "omw" || version < 1 || version > 9)
        return false;
    if (version >= 7)
    {
        const auto end = bytes.rfind("\ncomplete ");
        if (end == std::string::npos || bytes.back() != '\n')
            return false;
        std::istringstream footer{bytes.substr(end + 1U)};
        std::size_t length{};
        std::uint32_t checksum{};
        if (!(footer >> magic >> length >> checksum) || length != end + 1U ||
            checksum != net::steam_bridge_crc32(bytes_of(bytes).first(length)))
            return false;
        footer >> std::ws;
        if (!footer.eof())
            return false;
        bytes.resize(length);
    }
    std::istringstream body{bytes};
    return read_world_save_body(body, save);
}

bool write_world_save(const std::filesystem::path &path, const WorldSaveData &save)
{
    std::ostringstream output;
    if (!write_world_save(output, save))
        return false;
    const std::string bytes = output.str();
    // Retain the previous valid checkpoint. A corrupt primary must never replace
    // the recovery copy, including after an automatic backup load.
    std::ifstream previous{path, std::ios::binary};
    if (previous)
    {
        std::string prior_bytes;
        WorldSaveData validated;
        if (read_bounded(previous, prior_bytes))
        {
            std::istringstream prior{prior_bytes};
            if (read_world_save(prior, validated) &&
                !write_file_atomically(backup_path(path), bytes_of(prior_bytes)))
                return false;
        }
    }
    previous.close();
    return write_file_atomically(
        path, {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}

std::optional<WorldSaveData> read_world_save(const std::filesystem::path &path)
{
    std::ifstream input{path, std::ios::binary};
    WorldSaveData result;
    if (!read_world_save(input, result))
    {
        std::ifstream backup{backup_path(path), std::ios::binary};
        if (!read_world_save(backup, result))
            return std::nullopt;
        result.recovered_from_backup = true;
    }
    return result;
}

} // namespace openminerz
