#include "openminerz/world_save.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <string_view>

namespace openminerz
{
namespace
{

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

bool write_world_save(std::ostream &output, const WorldSaveData &save)
{
    if (!output || !valid_owner(save.owner) || save.saved_unix_seconds < 0 ||
        save.doors.size() > 4096U || save.spawners.size() > 4096U ||
        std::ranges::any_of(save.doors, [](const auto &entry) { return entry.second > 4U; }) ||
        std::ranges::any_of(save.spawners, [](const auto &entry) {
            return entry.second.empty() || entry.second.size() > 1024U;
        }))
        return false;
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "omw 6\n";
    if (!save.owner.empty())
        output << "owner " << std::quoted(save.owner) << '\n';
    if (save.saved_unix_seconds != 0)
        output << "saved " << save.saved_unix_seconds << '\n';
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
    for (const std::array<int, 4> &edit : save.edits)
        output << "edit " << edit[0] << ' ' << edit[1] << ' ' << edit[2] << ' ' << edit[3] << '\n';
    return static_cast<bool>(output);
}

bool read_world_save(std::istream &input, WorldSaveData &save)
{
    if (!input)
        return false;
    WorldSaveData decoded;
    clear_inventory(decoded.inventory);
    bool recognized{};
    std::string keyword;
    while (input >> keyword)
    {
        if (keyword == "omw")
        {
            input >> decoded.version;
            recognized = true;
        }
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
        else if (keyword == "seed")
            input >> decoded.seed;
        else if (keyword == "mode")
        {
            int mode{};
            input >> mode;
            decoded.mode = static_cast<GameMode>(std::clamp(mode, 0, 5));
        }
        else if (keyword == "difficulty")
        {
            int difficulty{};
            input >> difficulty;
            decoded.difficulty = static_cast<GameDifficulty>(std::clamp(difficulty, 0, 3));
        }
        else if (keyword == "infinite")
        {
            int infinite{};
            input >> infinite;
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
            if (decoded.version >= 5)
                decoded.inventory.set_profile(profile == static_cast<int>(InventoryProfile::modern)
                                                  ? InventoryProfile::modern
                                                  : InventoryProfile::classic);
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
            }
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
        }
        else if (keyword == "crate_dur")
        {
            WorldSaveCrateKey location{};
            std::size_t index{};
            float durability{1.0F};
            input >> location[0] >> location[1] >> location[2] >> index >> durability;
            if (index < world_save_crate_slot_count && decoded.crates[location][index].has_value())
                decoded.crates[location][index]->durability = std::clamp(durability, 0.0F, 1.0F);
        }
        else if (keyword == "door")
        {
            WorldSaveCrateKey location{};
            unsigned int model{};
            input >> location[0] >> location[1] >> location[2] >> model;
            if (model <= 4U)
                decoded.doors.insert_or_assign(location, static_cast<std::uint8_t>(model));
        }
        else if (keyword == "spawner")
        {
            WorldSaveCrateKey location{};
            std::string spawn_id;
            input >> location[0] >> location[1] >> location[2] >> std::quoted(spawn_id);
            if (!spawn_id.empty() && spawn_id.size() <= 1024U)
                decoded.spawners.insert_or_assign(location, std::move(spawn_id));
        }
        else if (keyword == "edit")
        {
            std::array<int, 4> edit{};
            input >> edit[0] >> edit[1] >> edit[2] >> edit[3];
            if (edit[3] >= static_cast<int>(world::BlockType::empty) &&
                edit[3] <= static_cast<int>(world::BlockType::uninitialized))
                decoded.edits.push_back(edit);
        }
        else
        {
            std::string ignored;
            std::getline(input, ignored);
        }
    }
    if (!recognized)
        return false;
    save = std::move(decoded);
    return true;
}

bool write_world_save(const std::filesystem::path &path, const WorldSaveData &save)
{
    std::ofstream output{path, std::ios::trunc};
    return write_world_save(output, save);
}

std::optional<WorldSaveData> read_world_save(const std::filesystem::path &path)
{
    std::ifstream input{path};
    WorldSaveData result;
    if (!read_world_save(input, result))
        return std::nullopt;
    return result;
}

} // namespace openminerz
