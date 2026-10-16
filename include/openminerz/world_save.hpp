#pragma once

#include "openminerz/game_mode.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/encounters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz
{

inline constexpr std::size_t world_save_crate_slot_count = 32U;
using WorldSaveCrateKey = std::array<int, 3>;
using WorldSaveCrateSlots = std::array<PlayerInventory::Slot, world_save_crate_slot_count>;
using WorldSaveCrateMap = std::map<WorldSaveCrateKey, WorldSaveCrateSlots>;
using WorldSaveDoorMap = std::map<WorldSaveCrateKey, std::uint8_t>;
using WorldSaveSpawnerMap = std::map<WorldSaveCrateKey, std::string>;

struct WorldHistory final
{
    std::string creator;
    std::int64_t created_unix_seconds{};
    std::int64_t tracked_since_unix_seconds{};
    double play_seconds{};
    std::uint32_t sessions{};
    std::uint32_t deaths{};

    // Legacy worlds begin tracking now; their earlier history stays unknown.
    void begin_session(std::int64_t now, std::string_view player, bool new_world);
    void add_play_time(double seconds) noexcept;
};

// Project-owned checkpoint. Version 9 adds travel destinations and encounter
// progression to version 8's world history. Versions 1–8 remain readable. Network peers
// use the separate original binary contracts. Files retain one valid .bak checkpoint.
struct WorldSaveData final
{
    int version{9};
    bool recovered_from_backup{}; // Load result only; not serialized.
    std::string owner;
    std::int64_t saved_unix_seconds{};
    WorldHistory history;
    std::int32_t seed{};
    GameMode mode{GameMode::survival};
    GameDifficulty difficulty{GameDifficulty::hard};
    bool infinite_resources{};
    float day{0.41F};

    world::Vec3 player_position{};
    float player_yaw{2.3561945F};
    float player_pitch{-0.3F};
    float player_health{100.0F};
    bool has_player{};

    float distance_origin_x{};
    float distance_origin_z{};
    bool has_distance_origin{};
    int dragon_distance_index{};
    int max_distance{};
    int next_song_index{};

    PlayerInventory inventory;
    PlayerInventory::Slot held_stack;
    WorldSaveCrateMap crates;
    WorldSaveDoorMap doors;
    WorldSaveSpawnerMap spawners;
    world::WorldEncounterState encounters;
    std::vector<std::array<int, 4>> edits;
};

[[nodiscard]] bool write_world_save(std::ostream &output, const WorldSaveData &save);
[[nodiscard]] bool read_world_save(std::istream &input, WorldSaveData &save);
[[nodiscard]] bool write_world_save(const std::filesystem::path &path, const WorldSaveData &save);
[[nodiscard]] std::optional<WorldSaveData> read_world_save(const std::filesystem::path &path);

} // namespace openminerz
