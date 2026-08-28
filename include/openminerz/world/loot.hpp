#pragma once

#include "openminerz/inventory.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/legacy_noise.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openminerz::world
{

// A direct representation of Modern 1.9.9.8 PossibleLootType. The quantity
// maximum is exclusive because DNA.MathTools.RandomInt(min, max) delegates to
// System.Random.Next(min, max).
struct ModernLootDefinition final
{
    std::int16_t wire_item{};
    InventoryItemId item{InventoryItemId::bare_hands};
    int weight{};
    int tier{};
    int minimum_world_level{};
    int maximum_world_level{};
    int minimum_quantity{};
    int maximum_quantity_exclusive{};
    int worth{};
    std::uint32_t package_flags{};
};

struct ModernLootDrop final
{
    InventoryStack stack;
    std::array<float, 3> spawn_velocity{};
    int value{};
    bool rare_roll{};
};

struct ModernLootBlockOutput final
{
    int pre_roll{};
    int statistic_id{};
    std::vector<ModernLootDrop> drops;
};

[[nodiscard]] std::span<const ModernLootDefinition> modern_loot_catalog() noexcept;

// Mirrors PossibleLootType.GetRandomLoot, including the original <= 0 weighted
// subtraction edge and Snow fallback when no candidate is eligible.
[[nodiscard]] ModernLootDrop select_modern_loot(LegacyRandom &random, int world_level,
                                                int minimum_tier, int maximum_tier,
                                                bool rare_roll) noexcept;

// Checks whether an already decoded pickup could have been produced by one
// eligible catalog entry. This is used by the host before relaying client-owned
// loot creation; quantity maxima retain their exclusive Random.Next semantics.
[[nodiscard]] bool modern_loot_stack_matches(const InventoryStack &stack, int world_level,
                                             int minimum_tier, int maximum_tier,
                                             bool rare_only) noexcept;

// Mirrors ProcessLootBlockOutput. Only LootBlock (69) and LuckyLootBlock (70)
// are accepted; BombBlock (71) deliberately does not enter this path.
[[nodiscard]] std::optional<ModernLootBlockOutput> process_modern_loot_block(LegacyRandom &random,
                                                                             BlockType block,
                                                                             int world_level);

} // namespace openminerz::world
