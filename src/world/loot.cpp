#include "openminerz/world/loot.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace openminerz::world
{
namespace
{

constexpr InventoryItemId internal_item(std::int16_t wire_item) noexcept
{
    if (wire_item <= 122)
        return static_cast<InventoryItemId>(wire_item);
    return static_cast<InventoryItemId>(
        static_cast<std::uint16_t>(InventoryItemId::modern_chainsaw_1) +
        static_cast<std::uint16_t>(wire_item - 123));
}

constexpr ModernLootDefinition loot(std::int16_t item, int weight, int tier, int minimum_quantity,
                                    int maximum_quantity_exclusive) noexcept
{
    return {item,
            internal_item(item),
            weight,
            tier,
            -1000,
            1000,
            minimum_quantity,
            maximum_quantity_exclusive,
            100,
            1U};
}

// Private _PossibleLootTypes catalog from Modern CastleMiner Z 1.9.9.8, in
// constructor order. Order is observable because weighted selection walks this
// exact array after one global RandomInt(totalWeight) call.
constexpr std::array modern_catalog{
    loot(44, 100, 1, 50, 400), loot(45, 100, 2, 50, 300), loot(46, 80, 3, 50, 250),
    loot(47, 60, 4, 50, 200),  loot(109, 35, 6, 50, 100), loot(109, 15, 6, 300, 400),
    loot(21, 150, 1, 3, 6),    loot(20, 130, 1, 3, 6),    loot(22, 120, 3, 2, 4),
    loot(19, 50, 6, 1, 1),     loot(19, 10, 8, 2, 3),     loot(7, 75, 5, 1, 1),
    loot(7, 10, 7, 8, 10),     loot(107, 45, 7, 1, 1),    loot(107, 15, 7, 3, 5),
    loot(102, 30, 7, 1, 1),    loot(102, 6, 8, 3, 5),     loot(129, 50, 6, 1, 1),
    loot(14, 75, 2, 2, 8),     loot(5, 70, 1, 2, 4),      loot(108, 50, 6, 1, 1),
    loot(108, 10, 8, 4, 6),    loot(116, 5, 6, 1, 1),     loot(61, 30, 5, 1, 1),
    loot(110, 50, 6, 1, 2),    loot(126, 50, 7, 1, 2),    loot(80, 30, 7, 1, 1),
    loot(79, 30, 7, 1, 1),     loot(103, 20, 5, 1, 1),    loot(83, 6, 8, 1, 1),
    loot(85, 5, 8, 1, 1),      loot(82, 4, 8, 1, 1),      loot(84, 5, 8, 1, 1),
    loot(86, 4, 8, 1, 1),      loot(69, 15, 8, 1, 1),     loot(121, 20, 7, 1, 1),
    loot(56, 25, 6, 1, 1),     loot(59, 35, 5, 1, 1),     loot(120, 25, 6, 1, 1),
    loot(57, 35, 5, 1, 1),     loot(58, 25, 6, 1, 1),     loot(60, 25, 6, 1, 1),
    loot(143, 5, 8, 1, 1),     loot(28, 7, 7, 1, 1),      loot(27, 20, 6, 1, 1),
    loot(26, 30, 3, 1, 1),     loot(25, 50, 2, 1, 1),     loot(33, 25, 5, 1, 1),
    loot(38, 25, 5, 1, 1),
};

constexpr bool eligible(const ModernLootDefinition &candidate, int world_level, int minimum_tier,
                        int maximum_tier, bool rare_roll) noexcept
{
    if (rare_roll && candidate.weight > 20)
        return false;
    if (candidate.package_flags != 1U)
        return false;
    if (candidate.minimum_world_level != -1000 && world_level < candidate.minimum_world_level)
        return false;
    if (candidate.maximum_world_level != 1000 && world_level > candidate.maximum_world_level)
        return false;
    return candidate.tier >= minimum_tier && candidate.tier <= maximum_tier &&
           candidate.worth <= std::numeric_limits<int>::max();
}

void place_drop(LegacyRandom &random, ModernLootBlockOutput &output, ModernLootDrop drop)
{
    // PickupManager.CreateUpwardPickup consumes two RandomFloat(-0.5, 0.501)
    // calls from the same MathTools.Rnd stream between successive item rolls.
    const auto random_component = [&]() {
        const float amount = random.next_float();
        return -0.5F + (0.501F - -0.5F) * amount;
    };
    float x = random_component();
    float y = 0.1F;
    float z = random_component();
    const float inverse_length = 1.0F / std::sqrt(x * x + y * y + z * z);
    x *= inverse_length * 1.5F;
    y *= inverse_length * 1.5F;
    z *= inverse_length * 1.5F;
    drop.spawn_velocity = {x, y, z};
    output.drops.push_back(std::move(drop));
}

} // namespace

std::span<const ModernLootDefinition> modern_loot_catalog() noexcept
{
    return modern_catalog;
}

ModernLootDrop select_modern_loot(LegacyRandom &random, int world_level, int minimum_tier,
                                  int maximum_tier, bool rare_roll) noexcept
{
    int total_weight{};
    for (const ModernLootDefinition &candidate : modern_catalog)
        if (eligible(candidate, world_level, minimum_tier, maximum_tier, rare_roll))
            total_weight += candidate.weight;

    int remaining = random.next(total_weight);
    const ModernLootDefinition *selected{};
    for (const ModernLootDefinition &candidate : modern_catalog)
    {
        if (!eligible(candidate, world_level, minimum_tier, maximum_tier, rare_roll))
            continue;
        remaining -= candidate.weight;
        if (remaining <= 0)
        {
            selected = &candidate;
            break;
        }
    }

    if (selected == nullptr)
        return {{InventoryItemId::snow, 1U}, {}, 500, rare_roll};

    const int quantity =
        random.next(selected->minimum_quantity, selected->maximum_quantity_exclusive);
    return {{selected->item, static_cast<std::uint16_t>(quantity)}, {}, selected->worth, rare_roll};
}

bool modern_loot_stack_matches(const InventoryStack &stack, int world_level, int minimum_tier,
                               int maximum_tier, bool rare_only) noexcept
{
    if (!stack.valid())
        return false;
    for (const ModernLootDefinition &candidate : modern_catalog)
    {
        if (!eligible(candidate, world_level, minimum_tier, maximum_tier, rare_only) ||
            candidate.item != stack.item)
            continue;
        const int count = static_cast<int>(stack.count);
        if (count >= candidate.minimum_quantity &&
            (candidate.minimum_quantity == candidate.maximum_quantity_exclusive
                 ? count == candidate.minimum_quantity
                 : count < candidate.maximum_quantity_exclusive))
            return true;
    }
    return false;
}

std::optional<ModernLootBlockOutput> process_modern_loot_block(LegacyRandom &random,
                                                               BlockType block, int world_level)
{
    if (block != BlockType::loot_block && block != BlockType::lucky_loot_block)
        return std::nullopt;

    ModernLootBlockOutput output;
    output.pre_roll = random.next(100);
    output.statistic_id = 9;
    int minimum_tier = 1;
    int maximum_tier = 5;
    int regular_count = 1;

    // The original checks preRoll < 8 but writes the existing value 1 back to
    // output count. The call and branch remain observable through RNG state.
    if (output.pre_roll < 8)
        regular_count = 1;

    if (block == BlockType::lucky_loot_block)
    {
        minimum_tier = 4;
        maximum_tier = 10;
        ++regular_count;
        place_drop(random, output,
                   select_modern_loot(random, world_level, minimum_tier, maximum_tier, true));
        output.statistic_id = 10;
    }

    for (int index{}; index < regular_count; ++index)
        place_drop(random, output,
                   select_modern_loot(random, world_level, minimum_tier, maximum_tier, false));
    return output;
}

} // namespace openminerz::world
