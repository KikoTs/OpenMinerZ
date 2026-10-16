// The one part of openminerz/vfx_combat.hpp that reads the block table, kept
// out of vfx_library_combat.cpp so the recipes link without the world library.

#include "openminerz/vfx_combat.hpp"

#include <algorithm>
#include <array>

namespace openminerz::vfx
{

Color dust_color(world::BlockType block) noexcept
{
    const world::BlockColor color = world::fallback_color(block);
    const std::array<float, 3> base{static_cast<float>(color.red) / 255.0F,
                                    static_cast<float>(color.green) / 255.0F,
                                    static_cast<float>(color.blue) / 255.0F};
    // Dust is paler and greyer than the surface it came off.
    const float grey = (base[0] + base[1] + base[2]) / 3.0F;
    const auto channel = [grey](float value) {
        const float display = std::clamp(0.30F + 0.62F * (value * 0.70F + grey * 0.30F), 0.0F, 1.0F);
        // The block colour is a display value; effects are lit in linear light.
        return display * display;
    };
    return {channel(base[0]), channel(base[1]), channel(base[2])};
}

} // namespace openminerz::vfx
