#include "openminerz/world/encounters.hpp"

#include <algorithm>
#include <cmath>

namespace openminerz::world
{
CaveEncounterRates cave_encounter_rates(GameMode mode) noexcept
{
    switch (mode)
    {
    case GameMode::scavenger: return {150, 1000, 200};
    case GameMode::creative: return {1'000'000, 1'000'000, 2000};
    case GameMode::exploration: return {2100, 7000, 1000};
    case GameMode::survival: return {20000, 35000, 2000};
    default: return {1'000'000, 1'000'000, 2100};
    }
}

std::uint32_t hell_boss_interval(std::uint32_t spawned) noexcept
{
    // HellFloorBiome.GetNextBossBlockCountdown: `float num2 = 1.1f;
    // 4000 * (int)Math.Pow(spawnCount + 1, num2)` -- the exponent is the float
    // 1.1f widened to double, not the double 1.1.
    return 4000U * static_cast<std::uint32_t>(std::pow(double(std::min(spawned, 1000U) + 1U),
                                                       static_cast<double>(1.1F)));
}

std::vector<std::array<int, 4>> populate_chunk_encounters(
    const Chunk &chunk, const WorldRegion &region, std::int32_t seed, GameMode mode,
    WorldEncounterState &state)
{
    std::vector<std::array<int, 4>> edits;
    if (state.populated_chunks.size() >= maximum_encounter_chunks ||
        !state.populated_chunks.insert({chunk.chunk_x(), chunk.chunk_z()}).second)
        return edits;
    const auto *sites = chunk.generation_sites();
    if (!sites) return edits;
    const auto rates = cave_encounter_rates(mode);
    const IntegerNoise choices(seed);
    const auto place = [&](const std::array<int, 3> &cell, BlockType block) {
        if (block != BlockType::empty && region.contains(cell[0], cell[1], cell[2]) &&
            region.block(cell[0], cell[1], cell[2]) == BlockType::empty)
            edits.push_back({cell[0], cell[1], cell[2], static_cast<int>(block)});
    };
    for (const auto &site : sites->caves)
    {
        const std::uint64_t ordinal = state.carved_cells + site.carved_index;
        BlockType block = BlockType::empty;
        if (ordinal % rates.loot == 0) block = BlockType::loot_block;
        if (ordinal % rates.enemy == 0)
        {
            constexpr std::array kinds{BlockType::enemy_spawn_rare_off,
                                       BlockType::enemy_spawn_off, BlockType::alien_spawn_off};
            // Original uses global randomness; a coordinate choice survives save/reload
            // without depending on other unrelated gameplay random draws.
            const auto roll = static_cast<unsigned>(choices.noise(site.cell[0], site.cell[1], site.cell[2]));
            block = kinds[roll % kinds.size()];
        }
        if (ordinal % rates.lucky_loot == 0) block = BlockType::lucky_loot_block;
        place(site.cell, block);
    }
    state.carved_cells += sites->carved_count;
    const bool bosses = mode == GameMode::survival || mode == GameMode::creative ||
                        mode == GameMode::exploration || mode == GameMode::scavenger;
    if (bosses)
        for (const auto &cell : sites->hell_floor)
        {
            if (state.hell_bosses_spawned >= state.max_hell_boss_spawns) break;
            if (state.hell_countdown > 0) --state.hell_countdown;
            if (state.hell_countdown == 0)
            {
                // Do not overwrite a player's building or an earlier cave encounter.
                if (region.block(cell[0], cell[1], cell[2]) != BlockType::empty ||
                    std::any_of(edits.begin(), edits.end(), [&](const auto &e) {
                        return e[0] == cell[0] && e[1] == cell[1] && e[2] == cell[2];
                    }))
                    continue;
                place(cell, BlockType::boss_spawn_off);
                ++state.hell_bosses_spawned;
                state.hell_countdown = hell_boss_interval(state.hell_bosses_spawned);
            }
        }
    return edits;
}
} // namespace openminerz::world
