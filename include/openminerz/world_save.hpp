#pragma once

#include "openminerz/game_mode.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/world/player.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openminerz
{

inline constexpr std::size_t world_save_crate_slot_count = 32U;
using WorldSaveCrateKey = std::array<int, 3>;
using WorldSaveCrateSlots = std::array<PlayerInventory::Slot, world_save_crate_slot_count>;
using WorldSaveCrateMap = std::map<WorldSaveCrateKey, WorldSaveCrateSlots>;
using WorldSaveDoorMap = std::map<WorldSaveCrateKey, std::uint8_t>;
using WorldSaveSpawnerMap = std::map<WorldSaveCrateKey, std::string>;

// Project-owned offline save data. Networking will use the original binary
// contracts later; this small text codec keeps local worlds deterministic and
// independently testable without constructing the SDL application.
struct WorldSaveData final
{
    int version{6};
    std::string owner;
    std::int64_t saved_unix_seconds{};
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
    WorldSaveCrateMap crates;
    WorldSaveDoorMap doors;
    WorldSaveSpawnerMap spawners;
    std::vector<std::array<int, 4>> edits;
};

[[nodiscard]] bool write_world_save(std::ostream &output, const WorldSaveData &save);
[[nodiscard]] bool read_world_save(std::istream &input, WorldSaveData &save);
[[nodiscard]] bool write_world_save(const std::filesystem::path &path, const WorldSaveData &save);
[[nodiscard]] std::optional<WorldSaveData> read_world_save(const std::filesystem::path &path);

} // namespace openminerz
