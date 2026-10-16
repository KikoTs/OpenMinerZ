#pragma once

#include "openminerz/inventory.hpp"
#include "openminerz/world_save.hpp"

#include <cstddef>
#include <cstdint>

// Items the shipped game does not have, in saves written by earlier builds.
//
// Retail registers 122 item classes in 1.6.3.8 and 143 in 1.9.9.8
// (item_registered). Anything else is not an item to a stock peer:
// InventoryItem.CreateItem indexes AllItems with the id it read, so a stock
// client handed one throws while it is still reading the message. Such an id
// can therefore never be written to the wire, and the encoders refuse to.
//
// A refusal alone made a session impossible to start for a world whose
// inventory or crates still HELD one. The ids in question are
//   * 123..131, which this project invented (see docs/retail-parity.md): block
//     items for grass, the five ore blocks, surface lava, leaves and forced
//     air. Nothing produces them any more;
//   * ids a generation declares but never registers (SpaceKnife, a dozen
//     Modern enum members);
//   * another generation's items in this generation's world.
// The rule: a world plays with the items its own generation has. Stacks of
// anything else are removed when the world is loaded, the player is told how
// many, and the save as it was is kept beside it. After that what the player
// holds, what a crate holds and what is sent are the same thing.
namespace openminerz
{

struct NonRetailItemCleanup final
{
    // Slots emptied, and the item count they held between them.
    std::size_t stacks{};
    std::uint64_t items{};

    [[nodiscard]] constexpr bool anything() const noexcept
    {
        return stacks != 0U;
    }
    constexpr NonRetailItemCleanup &operator+=(const NonRetailItemCleanup &other) noexcept
    {
        stacks += other.stacks;
        items += other.items;
        return *this;
    }
};

inline void remove_non_retail_item(PlayerInventory::Slot &slot, InventoryProfile profile,
                                   NonRetailItemCleanup &cleanup) noexcept
{
    if (!slot.has_value() || item_registered(slot->item, profile))
        return;
    ++cleanup.stacks;
    cleanup.items += slot->count;
    slot.reset();
}

// Everything a player carries: the backpack, both trays and the stack on the
// cursor. `profile` is the world's generation, which is the inventory's own.
[[nodiscard]] inline NonRetailItemCleanup remove_non_retail_items(
    PlayerInventory &inventory, PlayerInventory::Slot &held_stack) noexcept
{
    NonRetailItemCleanup cleanup;
    const InventoryProfile profile = inventory.profile();
    for (PlayerInventory::Slot &slot : inventory.storage())
        remove_non_retail_item(slot, profile, cleanup);
    for (PlayerInventory::Tray &tray : inventory.trays())
        for (PlayerInventory::Slot &slot : tray)
            remove_non_retail_item(slot, profile, cleanup);
    remove_non_retail_item(held_stack, profile, cleanup);
    return cleanup;
}

[[nodiscard]] inline NonRetailItemCleanup remove_non_retail_items(
    WorldSaveCrateMap &crates, InventoryProfile profile) noexcept
{
    NonRetailItemCleanup cleanup;
    for (auto &[location, slots] : crates)
        for (PlayerInventory::Slot &slot : slots)
            remove_non_retail_item(slot, profile, cleanup);
    return cleanup;
}

// A whole checkpoint, as load_world applies it before anything reads the save.
[[nodiscard]] inline NonRetailItemCleanup remove_non_retail_items(WorldSaveData &save) noexcept
{
    NonRetailItemCleanup cleanup = remove_non_retail_items(save.inventory, save.held_stack);
    cleanup += remove_non_retail_items(save.crates, save.inventory.profile());
    return cleanup;
}

} // namespace openminerz
