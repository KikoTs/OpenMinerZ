#pragma once

#include "openminerz/world/block.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/world.hpp"

#include <algorithm>
#include <cmath>

// Where the shipped game puts a player after a relocation.
//
// GameScreen.TeleportToLocation sets LocalPosition, centres the terrain there
// and, once it is loaded, calls CastleMinerZGame.MakeAboveGround(spawnOnTop):
//
//     LocalPosition = spawnOnTop ? terrain.FindTopmostGroundLocation(LocalPosition)
//                                : terrain.FindSafeStartLocation(LocalPosition);
//
// Both searches are written as an expanding ring of columns that only accepts
// a height at or above the water line. Retail never has water
// (BlockTerrain.IsWaterWorld is never set), the water line is then -1, and
// every height satisfies it: the FIRST column examined, the target's own, is
// always the answer. Neither function looks at what the blocks are beyond
// BlockPlayer, so neither avoids lava, and nothing in the path touches the
// player's velocity. Identical in 1.6.3.8 and 1.9.9.8.
namespace openminerz::world
{

namespace stock_landing_detail
{
// BlockTerrain block indices: 0..127 from the bottom of the world.
[[nodiscard]] inline bool blocks(const WorldRegion &region, int x, int index, int z) noexcept
{
    const int y = world_min_y + index;
    return region.contains(x, y, z) && blocks_player(region.block(x, y, z));
}

[[nodiscard]] inline Vec3 standing_in(int x, int index, int z) noexcept
{
    // MakePositionFromIndexVector(...) + (0.5, 0.1, 0.5).
    return {static_cast<float>(x) + 0.5F, static_cast<float>(world_min_y + index) + 0.1F,
            static_cast<float>(z) + 0.5F};
}
} // namespace stock_landing_detail

// BlockTerrain.FindTopmostGroundLocation: the cell above the highest
// BlockPlayer block of the guess's own column. Used with spawnOnTop: true,
// which is every return to the start (menu, respawn, level restart) and the
// menu's "Surface".
[[nodiscard]] inline Vec3 stock_topmost_ground_location(const WorldRegion &region,
                                                        Vec3 guess) noexcept
{
    using namespace stock_landing_detail;
    const int x = static_cast<int>(std::floor(guess.x));
    const int z = static_cast<int>(std::floor(guess.z));
    int index = chunk_height - 1;
    while (index > 0 && !blocks(region, x, index, z))
        --index;
    return standing_in(x, index + 1, z);
}

// BlockTerrain.FindSafeStartLocation: the two-cell-high gap nearest the guess
// in the guess's own column. Used with spawnOnTop: false, which is teleporting
// to a player and the Endurance respawn beside a living one. "Safe" means only
// that the body fits: the gap may be in mid-air, in a cave below the target,
// or over lava.
[[nodiscard]] inline Vec3 stock_safe_start_location(const WorldRegion &region, Vec3 guess) noexcept
{
    using namespace stock_landing_detail;
    const int top = chunk_height - 1; // 127
    const int x = static_cast<int>(std::floor(guess.x));
    const int z = static_cast<int>(std::floor(guess.z));
    int start =
        std::clamp(static_cast<int>(std::floor(guess.y)) - world_min_y, 0, top);
    if (blocks(region, x, start, z))
    {
        // Inside something: climb out of it.
        while (start < top && blocks(region, x, start, z))
            ++start;
    }
    else
    {
        // In the open: drop to the cell above the first block below.
        while (start > 1)
        {
            if (blocks(region, x, start, z))
            {
                ++start;
                break;
            }
            --start;
        }
    }
    // The original leaves this at -1 when the column offers no gap at all and
    // returns it unchanged, one cell below the bottom of the world.
    int chosen = -1;
    for (int index = start; index <= top; ++index)
        if (!blocks(region, x, index, z) && (index == top || !blocks(region, x, index + 1, z)))
        {
            chosen = index;
            break;
        }
    for (int index = start - 1; index > 0; --index)
        if (!blocks(region, x, index, z) && !blocks(region, x, index + 1, z))
        {
            // The gap below wins only when it is strictly nearer.
            if (start - index < chosen - start)
                chosen = index;
            break;
        }
    return standing_in(x, chosen, z);
}

} // namespace openminerz::world
