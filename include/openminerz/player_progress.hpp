#pragma once

#include "openminerz/game_mode.hpp"
#include "openminerz/inventory.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz
{
enum class ProgressMetric : std::size_t
{
    play_seconds, distance, depth, days, crafted, kills, undead_dragon, alien,
    laser_kills, tnt_crafted, guided_dragon, tnt_kills, grenade_kills,
    games, deaths, all_kills, all_crafted, count
};

enum class AchievementGroup { exploration, survival, crafting, combat };
struct AchievementDefinition
{
    std::string_view id;
    std::string_view name;
    std::string_view description;
    ProgressMetric metric;
    double target;
    AchievementGroup group;
};
inline constexpr std::size_t achievement_count = 31;
[[nodiscard]] std::span<const AchievementDefinition, achievement_count> achievement_definitions();

struct ItemProgress
{
    double held_seconds{};
    double crafted{};
    double used{};
    double hits{};
    double kills{};
    bool operator==(const ItemProgress &) const noexcept = default;
};

struct PlayerProgress
{
    std::array<double, static_cast<std::size_t>(ProgressMetric::count)> counters{};
    std::array<ItemProgress, static_cast<std::size_t>(InventoryItemId::item_count)> items{};
    std::array<double, 52> enemy_kills{};
    std::array<double, 5> dragon_kills{};
    std::array<std::int64_t, achievement_count> unlocked_at{};

    [[nodiscard]] double value(ProgressMetric metric) const noexcept;
    void add(ProgressMetric metric, double amount = 1.0) noexcept;
    void observe(GameMode mode, double seconds, float x, float y, float z,
                 float previous_day, float day, InventoryItemId held) noexcept;
    void record_craft(GameMode mode, InventoryItemId item, std::uint32_t batches) noexcept;
    void record_enemy_kill(GameMode mode, int enemy_type, InventoryItemId weapon) noexcept;
    // Original dragon milestones are shared with the party; weapon/total kill credit is local.
    void record_dragon_kill(GameMode mode, std::size_t kind, bool local_killer,
                            InventoryItemId weapon) noexcept;
    [[nodiscard]] std::vector<std::size_t> unlock_earned(std::int64_t now);
    [[nodiscard]] std::size_t unlocked_count() const noexcept;
};

// How many crafts one crafting action adds to ItemStats.Crafted (and, in
// Endurance, to TotalItemsCrafted) when it ran PlayerInventory.Craft `crafts`
// times. 1.9.9.8's Tier2Item.CheckInput counts inside its Shift loop; 1.6.3.8's
// CraftingUIScreen has the two increments after the loop, so a Shift-click
// that crafted forty times is one craft to its statistics and achievements.
[[nodiscard]] constexpr std::uint32_t stock_crafted_count(bool modern_rules, std::uint32_t crafts) noexcept
{
    return modern_rules || crafts == 0U ? crafts : 1U;
}

[[nodiscard]] std::string serialize_player_progress(const PlayerProgress &progress);
[[nodiscard]] std::optional<PlayerProgress> parse_player_progress(std::string_view text);
[[nodiscard]] PlayerProgress load_player_progress(const std::filesystem::path &path);
[[nodiscard]] bool save_player_progress(const std::filesystem::path &path,
                                       const PlayerProgress &progress);
} // namespace openminerz
