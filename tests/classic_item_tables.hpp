#pragma once

// Test-only shorthands. The production item-table API takes the game generation
// explicitly, because a caller that forgets it silently applies Classic rules to
// a Modern world (that is how the inventory screen came to merge Modern stacks
// with Classic limits). The older assertions in this suite all describe the
// Classic 1.6.3.8 tables, so they keep their two-argument spelling through
// these overloads instead of every call site growing a profile argument.
//
// Never include this from src/.

#include "openminerz/inventory.hpp"

namespace openminerz
{

[[nodiscard]] inline float dig_time_seconds(InventoryItemId tool, world::BlockType block) noexcept
{
    return dig_time_seconds(tool, block, InventoryProfile::classic);
}

[[nodiscard]] inline float item_enemy_damage(InventoryItemId item) noexcept
{
    return item_enemy_damage(item, InventoryProfile::classic);
}

[[nodiscard]] inline float item_self_damage_per_use(InventoryItemId item) noexcept
{
    return item_self_damage_per_use(item, InventoryProfile::classic);
}

[[nodiscard]] inline bool inflict_item_damage(InventoryStack &stack,
                                              bool infinite_resource_mode) noexcept
{
    return inflict_item_damage(stack, infinite_resource_mode, InventoryProfile::classic);
}

[[nodiscard]] inline std::uint16_t item_stack_limit(InventoryItemId item) noexcept
{
    return item_stack_limit(item, InventoryProfile::classic);
}

} // namespace openminerz
