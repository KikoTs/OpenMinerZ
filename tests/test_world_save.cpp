#include "openminerz/net/steam_bridge_protocol.hpp"
#include "openminerz/world_save.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace
{
using namespace openminerz;
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error{message};
}

WorldSaveData fixture()
{
    WorldSaveData save;
    save.seed = 1337;
    save.owner = "Checkpoint test";
    save.history.begin_session(1788580000, "Original creator", true);
    save.history.add_play_time(3661.25);
    save.history.deaths = 3;
    save.inventory.set_profile(InventoryProfile::modern);
    save.inventory.switch_current_tray();
    save.inventory.select(5);
    save.inventory.tray(1)[5] = InventoryStack{InventoryItemId::pistol, 1};
    save.inventory.tray(1)[5]->durability = 0.375F;
    save.inventory.tray(1)[5]->rounds_in_clip = 3;
    save.crates[{1, 2, 3}][7] = InventoryStack{InventoryItemId::bullets, 127};
    save.held_stack = InventoryStack{InventoryItemId::pistol, 1};
    save.held_stack->rounds_in_clip = 2;
    save.held_stack->durability = 0.25F;
    save.edits.push_back({1, 2, 3, static_cast<int>(world::BlockType::rock)});
    return save;
}

void save_integrity()
{
    const auto original = fixture();
    std::ostringstream stream;
    check(write_world_save(stream, original), "checkpoint serialization failed");
    const auto bytes = stream.str();
    WorldSaveData target;
    std::istringstream input{bytes};
    check(read_world_save(input, target) && target.version == 9 &&
              target.history.creator == "Original creator" &&
              target.history.play_seconds == 3661.25 && target.history.sessions == 1 &&
              target.history.deaths == 3 && target.inventory.current_tray_index() == 1 &&
              target.inventory.selected_index() == 5 &&
              target.inventory.tray(1)[5]->rounds_in_clip == 3 && target.held_stack &&
              target.held_stack->rounds_in_clip == 2 && target.held_stack->durability == 0.25F &&
              target.crates.at({1, 2, 3})[7]->count == 127,
          "checkpoint lost tray selection, weapon or crate state");
    // A cut after any byte, including a complete-looking record boundary, cannot
    // be mistaken for a committed save. Failure must preserve the caller's world.
    for (std::size_t end = 0; end < bytes.size(); ++end)
    {
        std::istringstream truncated{bytes.substr(0, end)};
        target.seed = -19;
        check(!read_world_save(truncated, target) && target.seed == -19,
              "truncated checkpoint was accepted or mutated existing state");
    }
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        auto corrupted = bytes;
        corrupted[index] ^= 0x20;
        std::istringstream damaged{corrupted};
        check(!read_world_save(damaged, target), "single-byte corruption was accepted");
    }
    for (const auto &invalid :
         {std::string{"omw 9\nseed 2\n"}, std::string{"omw 0\n"}, std::string{"omw 6\nplayer 1 2"},
          std::string{"omw 6\ninv 0 2 5\n"}, std::string{"omw 6\nedit 1 2 3\n"},
          std::string{"omw 6\nseed broken\n"}, bytes + "seed 2\n"})
    {
        std::istringstream damaged{invalid};
        check(!read_world_save(damaged, target), "malformed/unsupported checkpoint was accepted");
    }
    for (int version = 1; version <= 6; ++version)
    {
        std::istringstream legacy{"omw " + std::to_string(version) + "\nseed 27\ninv 0 2 7 0\n"};
        check(read_world_save(legacy, target) && target.seed == 27 &&
                  target.inventory.storage()[0]->count == 7,
              "valid legacy checkpoint no longer loads");
    }
    auto invalid = original;
    invalid.player_position.x = std::numeric_limits<float>::infinity();
    invalid.has_player = true;
    std::ostringstream rejected;
    check(!write_world_save(rejected, invalid), "non-finite player position was persisted");
    invalid = original;
    invalid.held_stack->waypoint_name = "invalid\nrecord";
    check(!write_world_save(rejected, invalid), "cursor injected an additional save record");
}

void cursor_checkpoint()
{
    for (const auto profile : {InventoryProfile::classic, InventoryProfile::modern})
    {
        PlayerInventory inventory;
        inventory.set_profile(profile);
        const auto limit = item_stack_limit(block_item(world::BlockType::rock), profile);
        for (auto &slot : inventory.storage())
            slot = InventoryStack{world::BlockType::rock, limit};
        for (auto &tray : inventory.trays())
            for (auto &slot : tray)
                slot = InventoryStack{world::BlockType::rock, limit};
        PlayerInventory::Slot held{InventoryStack{InventoryItemId::pistol, 1}};
        held->rounds_in_clip = 2;
        held->durability = 0.375F;
        check(!inventory_checkpoint(inventory, held), "full inventory discarded its cursor");
        inventory.storage()[4].reset();
        const auto snapshot = inventory_checkpoint(inventory, held);
        check(snapshot && snapshot->storage()[4] && snapshot->storage()[4]->rounds_in_clip == 2 &&
                  snapshot->storage()[4]->durability == 0.375F && !inventory.storage()[4] &&
                  held->count == 1,
              "cursor checkpoint mutated live state or refilled a held weapon");
        inventory.storage()[4] =
            InventoryStack{world::BlockType::rock, static_cast<std::uint16_t>(limit - 1U)};
        held = InventoryStack{world::BlockType::rock, 2};
        check(!inventory_checkpoint(inventory, held) &&
                  inventory.storage()[4]->count == limit - 1U && held->count == 2,
              "partial cursor merge changed live state on failure");
        held->count = 1;
        check(inventory_checkpoint(inventory, held)->storage()[4]->count == limit,
              "cursor checkpoint failed to use remaining stack capacity");
        held.reset();
        check(inventory_checkpoint(inventory, held).has_value(), "empty cursor rejected inventory");
    }
}

void world_history()
{
    WorldHistory history;
    history.begin_session(1000, "Creator", true);
    history.add_play_time(3600.5);
    history.begin_session(2000, "Another player", false);
    history.add_play_time(-10.0);
    history.add_play_time(std::numeric_limits<double>::infinity());
    check(history.creator == "Creator" && history.created_unix_seconds == 1000 &&
              history.tracked_since_unix_seconds == 1000 && history.sessions == 2 &&
              history.play_seconds == 3600.5,
          "a resumed world replaced its creator, date or accumulated playtime");
    WorldHistory legacy;
    legacy.begin_session(2000, "Current player", false);
    check(legacy.creator.empty() && legacy.created_unix_seconds == 0 &&
              legacy.tracked_since_unix_seconds == 2000,
          "legacy history was fabricated");
    const std::string body{"omw 7\nseed 21\nprofile 1\nselection 1 2\ninv 40 51 1 2\n"};
    const auto crc =
        net::steam_bridge_crc32({reinterpret_cast<const std::uint8_t *>(body.data()), body.size()});
    std::istringstream input{body + "complete " + std::to_string(body.size()) + " " +
                             std::to_string(crc) + "\n"};
    WorldSaveData decoded;
    check(read_world_save(input, decoded) && decoded.version == 7 &&
              decoded.inventory.current_tray_index() == 1 &&
              decoded.inventory.tray(1)[0]->rounds_in_clip == 2 &&
              decoded.history.tracked_since_unix_seconds == 0,
          "version 7 inventory or unknown history was not preserved");
}

void write_library_fixtures(const std::filesystem::path &directory)
{
    const std::array names{
        "Cedarwatch refuge",
        "Nightfall expedition",
        "The workshop",
        "Highland trail",
        "A very long world title for checking narrow windows and safe truncation",
        "Southern outpost",
        "Crystal valley",
        "Old frontier"};
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        auto save = fixture();
        save.owner = "Explorer";
        save.history.creator = index % 2U ? "Mira" : "Explorer";
        save.history.created_unix_seconds = 1787000000;
        save.history.tracked_since_unix_seconds = 1787000000;
        save.history.play_seconds = 45632 - static_cast<double>(index) * 5300;
        save.history.sessions = 18U - static_cast<std::uint32_t>(index);
        save.saved_unix_seconds = 1788580000 - static_cast<std::int64_t>(index) * 86400;
        save.day = 12.4F - static_cast<float>(index);
        save.max_distance = 2180 - static_cast<int>(index) * 200;
        save.inventory.set_profile(index % 2U ? InventoryProfile::classic
                                              : InventoryProfile::modern);
        save.held_stack.reset();
        save.seed = 1337;
        save.edits.clear();
        if (index == 7U)
        {
            std::filesystem::create_directories(directory);
            std::ofstream output{directory / (std::string{names[index]} + ".omw")};
            output << "omw 6\nseed 31\nday 2.4\n";
        }
        else
            check(write_world_save(directory / (std::string{names[index]} + ".omw"), save),
                  "library fixture failed");
    }
    std::ofstream broken{directory / "Damaged expedition.omw"};
    broken << "omw 8\nseed truncated";
}

void recovery()
{
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("openminerz-checkpoint-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "world.omw";
    auto backup = path;
    backup += ".bak";
    const auto cleanup = [&] {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(backup, error);
        std::filesystem::remove(directory, error);
    };
    try
    {
        auto first = fixture();
        check(write_world_save(path, first), "first disk checkpoint failed");
        auto second = first;
        second.seed = 222;
        check(write_world_save(path, second), "second disk checkpoint failed");
        check(read_world_save(path)->seed == 222 && read_world_save(backup)->seed == 1337,
              "primary or previous checkpoint differs");
        {
            std::ofstream corrupt{path, std::ios::binary | std::ios::trunc};
            corrupt << "omw 7\nseed 222\n";
        }
        const auto restored = read_world_save(path);
        check(restored && restored->recovered_from_backup && restored->seed == 1337 &&
                  restored->inventory.current_tray_index() == 1,
              "corrupt primary did not recover complete prior state");
        second.seed = 333;
        check(write_world_save(path, second) && read_world_save(backup)->seed == 1337,
              "recovery overwrite destroyed the valid backup");
        std::filesystem::remove(path);
        check(read_world_save(path)->seed == 1337, "missing primary did not recover backup");
        {
            std::ofstream corrupt{backup, std::ios::binary | std::ios::trunc};
            corrupt << "omw 7\n";
        }
        check(!read_world_save(path), "two invalid checkpoints were accepted");
        cleanup();
    }
    catch (...)
    {
        cleanup();
        throw;
    }
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 3 && std::string_view{argv[1]} == "--write-library-fixtures")
        {
            write_library_fixtures(argv[2]);
            return 0;
        }
        save_integrity();
        cursor_checkpoint();
        world_history();
        recovery();
        std::cout << "PASS: selection, every-byte truncation/corruption, legacy migration, atomic "
                     "checkpoint recovery\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
