#pragma once

#include "openminerz/world/world.hpp"
#include <set>

namespace openminerz::world
{
struct CaveEncounterRates
{
    std::uint32_t loot;
    std::uint32_t lucky_loot;
    std::uint32_t enemy;
};
[[nodiscard]] CaveEncounterRates cave_encounter_rates(GameMode mode) noexcept;
[[nodiscard]] std::uint32_t hell_boss_interval(std::uint32_t already_spawned) noexcept;

// Encounters are authority-owned edits. Keeping them out of the immutable base
// terrain prevents a guest's chunk loading order from creating different spawners.
struct WorldEncounterState
{
    std::uint64_t carved_cells{};
    std::uint32_t hell_bosses_spawned{};
    std::uint32_t max_hell_boss_spawns{50};
    std::uint32_t hell_countdown{4000};
    std::set<std::array<int, 2>> populated_chunks;
};
inline constexpr std::size_t maximum_encounter_chunks = 1'000'000;

[[nodiscard]] std::vector<std::array<int, 4>> populate_chunk_encounters(
    const Chunk &chunk, const WorldRegion &region, std::int32_t seed, GameMode mode,
    WorldEncounterState &state);
} // namespace openminerz::world
