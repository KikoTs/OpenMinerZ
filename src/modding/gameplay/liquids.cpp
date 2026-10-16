// (mod-liquid) Liquid mod blocks as the server sees them: which cells hold
// one (for creature movement and spawning, item.used's target and
// players.get), resolved from the server's own registry, overlay and the
// world generator's identities - the same knowledge every client builds its
// world::LiquidTable from (openminerz/world/liquid.hpp).

#include "internal.hpp"

#include "openminerz/world/liquid.hpp"

#include <algorithm>
#include <cmath>

namespace openminerz::modding::gameplay
{
namespace
{

// A wading body is lifted at this rate up to this speed, so it bobs with its
// middle at the surface.
constexpr double liquid_lift = 30.0;
constexpr double liquid_rise_speed = 1.5;
constexpr double gravity = -20.0;

} // namespace

double wading_vertical_speed(double vertical, double dt) noexcept
{
    if (vertical > liquid_rise_speed)
        return vertical + gravity * dt;
    return std::min(vertical + liquid_lift * dt, liquid_rise_speed);
}

bool GameplayService::Impl::wading_in_liquid(Vec3 feet, double height) const
{
    return liquids_defined && liquid_cell(floor_cell({feet.x, feet.y + height * 0.5, feet.z}));
}

bool GameplayService::Impl::steps_into_liquid(Vec3 feet) const
{
    return liquids_defined &&
           (liquid_cell(floor_cell({feet.x, feet.y + 0.05, feet.z})) ||
            liquid_cell(floor_cell({feet.x, feet.y - 0.5, feet.z})));
}

bool GameplayService::Impl::liquid_cell(BlockPos cell) const
{
    if (!liquids_defined)
        return false;
    const std::optional<std::uint8_t> block = block_at(cell);
    if (!block.has_value() || !liquid_fallbacks[*block])
        return false;
    const auto marked = overlay.find(cell);
    const std::uint16_t id =
        marked != overlay.end() ? marked->second : generated_block_at(cell, *block, false);
    const BlockDef *definition = server_registry.block(id);
    return definition != nullptr && definition->liquid && definition->fallback == *block;
}

std::optional<BlockPos> GameplayService::Impl::liquid_on_ray(Vec3 origin, Vec3 direction,
                                                             float reach) const
{
    if (!liquids_defined)
        return std::nullopt;
    const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                     direction.z * direction.z);
    if (!(length > 1e-9) || !(reach > 0.0F) || !finite(origin))
        return std::nullopt;
    const std::array<double, 3> d{direction.x / length, direction.y / length, direction.z / length};
    const std::array<double, 3> o{origin.x, origin.y, origin.z};
    std::array<int, 3> cell{static_cast<int>(std::floor(o[0])), static_cast<int>(std::floor(o[1])),
                            static_cast<int>(std::floor(o[2]))};
    std::array<int, 3> step{};
    std::array<double, 3> next{};
    std::array<double, 3> delta{};
    for (std::size_t axis = 0U; axis < 3U; ++axis)
    {
        step[axis] = d[axis] > 0.0 ? 1 : d[axis] < 0.0 ? -1 : 0;
        delta[axis] = step[axis] != 0 ? std::abs(1.0 / d[axis]) : 1e30;
        const double boundary = step[axis] > 0 ? std::floor(o[axis]) + 1.0 : std::floor(o[axis]);
        next[axis] = step[axis] != 0 ? (boundary - o[axis]) / d[axis] : 1e30;
    }
    double travelled{};
    for (int steps = 0; steps < 64 && travelled <= reach; ++steps)
    {
        const BlockPos here{cell[0], cell[1], cell[2]};
        if (liquid_cell(here))
            return here;
        // Anything else that is there stops the look (the cell the eye is
        // in does not: the selection skips it too).
        if (const auto block = block_at(here); block.has_value() && *block != 0U && steps > 0)
            return std::nullopt;
        const std::size_t axis = next[0] < next[1] ? (next[0] < next[2] ? 0U : 2U)
                                                   : (next[1] < next[2] ? 1U : 2U);
        travelled = next[axis];
        next[axis] += delta[axis];
        cell[axis] += step[axis];
    }
    return std::nullopt;
}

double GameplayService::Impl::liquid_depth_at(Vec3 point) const
{
    if (!liquids_defined || !finite(point))
        return 0.0;
    BlockPos top = floor_cell(point);
    if (!liquid_cell(top))
        return 0.0;
    for (int above = 0; above < 128 && liquid_cell({top.x, top.y + 1, top.z}); ++above)
        ++top.y;
    const double surface = static_cast<double>(top.y + 1) - world::liquid_surface_drop;
    return std::max(surface - point.y, 0.0);
}

} // namespace openminerz::modding::gameplay
