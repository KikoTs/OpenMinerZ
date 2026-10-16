#include "openminerz/world/movement_probe.hpp"

#include "openminerz/world/liquid.hpp" // mod-liquid

#include <algorithm>
#include <array>
#include <cmath>

namespace openminerz::world
{
namespace
{

struct Probe final
{
    Vec3 start{};
    Vec3 end{};
    Vec3 half{};
    Vec3 direction{};
    bool has_direction{};
    bool simulate_sloped_sides{};
    bool shape_has_sloped_sides{};
    MovementTrace result{};
};

[[nodiscard]] int floor_to_int(float value) noexcept
{
    return static_cast<int>(std::floor(value));
}

[[nodiscard]] bool blocks_at(const WorldRegion &world, int x, int y, int z) noexcept
{
    // (mod-liquid) A liquid stops nothing.
    return world.contains(x, y, z) && cell_blocks_player(world, x, y, z);
}

// TraceProbe.SetIntersection with SkipEmbedded set and TraceCompletePath clear.
void set_intersection(Probe &probe, float in_t, Vec3 in_normal, bool starts_in, int in_face,
                      int block_y) noexcept
{
    MovementTrace &result = probe.result;
    if (starts_in)
    {
        if (probe.simulate_sloped_sides && probe.shape_has_sloped_sides)
        {
            result.found_sloped_block = true;
            result.sloped_block_y = block_y;
            result.sloped_block_t = 0.0F;
        }
        return;
    }
    if (result.collides && !(in_t < result.in_t))
        return;
    if (probe.simulate_sloped_sides && probe.shape_has_sloped_sides &&
        in_face < static_cast<int>(ProbeFace::positive_y))
    {
        if (in_t <= result.sloped_block_t)
        {
            result.found_sloped_block = true;
            result.sloped_block_y = block_y;
            result.sloped_block_t = in_t;
        }
        return;
    }
    result.collides = true;
    result.in_t = in_t;
    result.in_normal = in_normal;
    result.in_face = static_cast<ProbeFace>(in_face);
}

// AABBTraceProbe.TestShape against one voxel's six face planes.
void test_block(Probe &probe, int x, int y, int z) noexcept
{
    struct Plane final
    {
        Vec3 normal;
        float distance;
    };
    const std::array<Plane, 6> planes{{
        {{1.0F, 0.0F, 0.0F}, static_cast<float>(-x - 1)},
        {{0.0F, 0.0F, -1.0F}, static_cast<float>(z)},
        {{-1.0F, 0.0F, 0.0F}, static_cast<float>(x)},
        {{0.0F, 0.0F, 1.0F}, static_cast<float>(-z - 1)},
        {{0.0F, 1.0F, 0.0F}, static_cast<float>(-y - 1)},
        {{0.0F, -1.0F, 0.0F}, static_cast<float>(y)},
    }};
    float in_t{};
    float out_t = 1.0F;
    bool starts_in = true;
    int in_face{};
    for (int index = 0; index < 6; ++index)
    {
        const Plane &plane = planes[static_cast<std::size_t>(index)];
        const float reach = -(std::abs(probe.half.x * plane.normal.x) +
                              std::abs(probe.half.y * plane.normal.y) +
                              std::abs(probe.half.z * plane.normal.z));
        float from = plane.normal.x * probe.start.x + plane.normal.y * probe.start.y +
                     plane.normal.z * probe.start.z + plane.distance + reach;
        float to = plane.normal.x * probe.end.x + plane.normal.y * probe.end.y +
                   plane.normal.z * probe.end.z + plane.distance + reach;
        const bool from_outside = from > -0.0001F;
        if (from_outside)
            from = std::max(from, 0.0F);
        const bool to_outside = to > -0.0001F;
        if (to_outside)
            to = std::max(to, 0.0F);
        if (from_outside && to_outside)
            return;
        if (from_outside == to_outside)
            continue;
        if (from > to)
        {
            const float fraction = from / (from - to);
            if (fraction >= in_t)
            {
                starts_in = false;
                in_face = index;
                in_t = fraction;
            }
        }
        else
        {
            const float fraction = (0.0F - from) / (to - from);
            if (fraction <= out_t)
                out_t = fraction;
        }
    }
    if (!(in_t <= out_t))
        return;
    const Vec3 normal = planes[static_cast<std::size_t>(in_face)].normal;
    const float along = probe.direction.x * normal.x + probe.direction.y * normal.y +
                        probe.direction.z * normal.z;
    // A box sliding exactly along a face it already touches is not stopped by
    // it. SkipEmbedded movers only keep an embedded start for slope climbing.
    if (!probe.has_direction || std::abs(along) > 1e-05F ||
        (starts_in && probe.simulate_sloped_sides))
        set_intersection(probe, in_t, normal, starts_in, in_face, y);
}

} // namespace

bool allows_slopes(BlockType type) noexcept
{
    switch (type)
    {
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
        return false;
    default:
        return blocks_player(type);
    }
}

BoxSlide slide_movement_box(const WorldRegion &world, Vec3 position, Vec3 velocity,
                            Vec3 box_minimum, Vec3 box_maximum, float delta_seconds) noexcept
{
    BoxSlide result;
    const Vec3 original_velocity = velocity;
    float remaining = delta_seconds;
    int iterations{};
    bool collides{};
    do
    {
        const Vec3 start = position;
        const Vec3 step{velocity.x * remaining, velocity.y * remaining, velocity.z * remaining};
        position = {start.x + step.x, start.y + step.y, start.z + step.z};
        const MovementTrace trace =
            trace_movement_box(world, start, position, box_minimum, box_maximum, false);
        collides = trace.collides;
        if (collides)
        {
            if (trace.in_face == ProbeFace::positive_y)
                result.on_ground = true;
            else
                result.touching_wall = true;
            const float travelled = std::max(trace.in_t - 0.001F, 0.0F);
            position = {start.x + step.x * travelled, start.y + step.y * travelled,
                        start.z + step.z * travelled};
            const float into = trace.in_normal.x * velocity.x + trace.in_normal.y * velocity.y +
                               trace.in_normal.z * velocity.z;
            velocity = {velocity.x - trace.in_normal.x * into, velocity.y - trace.in_normal.y * into,
                        velocity.z - trace.in_normal.z * into};
            remaining *= 1.0F - travelled;
            if (remaining <= 1e-07F)
                break;
            if (velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z <=
                    1e-06F ||
                original_velocity.x * velocity.x + original_velocity.y * velocity.y +
                        original_velocity.z * velocity.z <=
                    1e-06F)
            {
                velocity = {};
                break;
            }
        }
        ++iterations;
    } while (collides && iterations < 4);
    if (iterations == 4)
        velocity = {};
    result.position = position;
    result.velocity = velocity;
    return result;
}

BoxSlide advance_pickup(const WorldRegion &world, Vec3 position, Vec3 velocity,
                        float delta_seconds) noexcept
{
    // PickupEntity: WorldAcceleration = BasicPhysics.Gravity * 0.25f.
    velocity.y += -20.0F * 0.25F * delta_seconds;
    BoxSlide moved = slide_movement_box(world, position, velocity, {-0.3F, -0.3F, -0.3F},
                                        {0.3F, 0.3F, 0.3F}, delta_seconds);
    const float damping = moved.on_ground ? 0.9F : 0.99F;
    moved.velocity.x *= damping;
    moved.velocity.z *= damping;
    if (std::abs(moved.velocity.x) < 0.1F)
        moved.velocity.x = 0.0F;
    if (std::abs(moved.velocity.z) < 0.1F)
        moved.velocity.z = 0.0F;
    return moved;
}

MovementTrace trace_movement_box(const WorldRegion &world, Vec3 start, Vec3 end, Vec3 box_minimum,
                                 Vec3 box_maximum, bool simulate_sloped_sides) noexcept
{
    Probe probe;
    const Vec3 centre{(box_minimum.x + box_maximum.x) * 0.5F, (box_minimum.y + box_maximum.y) * 0.5F,
                      (box_minimum.z + box_maximum.z) * 0.5F};
    probe.start = {start.x + centre.x, start.y + centre.y, start.z + centre.z};
    probe.end = {end.x + centre.x, end.y + centre.y, end.z + centre.z};
    probe.half = {std::abs(box_maximum.x - centre.x), std::abs(box_maximum.y - centre.y),
                  std::abs(box_maximum.z - centre.z)};
    probe.direction = {probe.start.x - probe.end.x, probe.start.y - probe.end.y,
                       probe.start.z - probe.end.z};
    const float length_squared = probe.direction.x * probe.direction.x +
                                 probe.direction.y * probe.direction.y +
                                 probe.direction.z * probe.direction.z;
    probe.has_direction = length_squared > 0.0F;
    if (probe.has_direction)
    {
        const float inverse = 1.0F / std::sqrt(length_squared);
        probe.direction = {probe.direction.x * inverse, probe.direction.y * inverse,
                           probe.direction.z * inverse};
    }
    probe.simulate_sloped_sides = simulate_sloped_sides;

    int minimum_x = floor_to_int(std::min(probe.start.x, probe.end.x) - probe.half.x);
    int minimum_y = floor_to_int(std::min(probe.start.y, probe.end.y) - probe.half.y);
    int minimum_z = floor_to_int(std::min(probe.start.z, probe.end.z) - probe.half.z);
    int maximum_x = floor_to_int(std::max(probe.start.x, probe.end.x) + probe.half.x);
    int maximum_y = floor_to_int(std::max(probe.start.y, probe.end.y) + probe.half.y);
    int maximum_z = floor_to_int(std::max(probe.start.z, probe.end.z) + probe.half.z);

    // BruteForceTrace: slopes are only considered while the cell above the
    // mover's feet is open, and only for blocks in the feet band.
    bool slopes_possible{};
    int band_bottom{};
    int band_top{};
    if (simulate_sloped_sides)
    {
        const float feet = probe.start.y - probe.half.y;
        const int above_feet = floor_to_int(feet) + 1;
        const int feet_x = floor_to_int(probe.start.x);
        const int feet_z = floor_to_int(probe.start.z);
        if (world.contains(feet_x, above_feet, feet_z) &&
            !cell_blocks_player(world, feet_x, above_feet, feet_z))
        {
            band_bottom = floor_to_int(feet + 0.01F);
            band_top = floor_to_int(feet + 0.2F);
            slopes_possible = true;
        }
    }

    minimum_x = std::max(minimum_x, world.minimum_x());
    minimum_z = std::max(minimum_z, world.minimum_z());
    minimum_y = std::max(minimum_y, world_min_y);
    maximum_x = std::min(maximum_x, world.maximum_x());
    maximum_z = std::min(maximum_z, world.maximum_z());
    maximum_y = std::min(maximum_y, world_min_y + chunk_height - 1);

    // SlopingIsBlocked: any column under the swept box whose cell one above
    // the step is open but capped by a blocking cell is too low to climb into.
    const auto sloping_is_blocked = [&](int block_y) noexcept {
        if (block_y - world_min_y >= chunk_height - 2)
            return false;
        for (int z = minimum_z; z <= maximum_z; ++z)
            for (int x = minimum_x; x <= maximum_x; ++x)
                if (!blocks_at(world, x, block_y + 1, z) && blocks_at(world, x, block_y + 2, z))
                    return true;
        return false;
    };

    for (int z = minimum_z; z <= maximum_z; ++z)
        for (int x = minimum_x; x <= maximum_x; ++x)
            for (int y = minimum_y; y <= maximum_y; ++y)
            {
                const BlockType type = world.block(x, y, z);
                if (!blocks_player(type) || liquid_at(world, x, y, z))
                    continue;
                // The original leaves the flag from the previous voxel in
                // place for a block that does not allow slopes.
                if (slopes_possible && allows_slopes(type))
                    probe.shape_has_sloped_sides =
                        y <= band_top && y >= band_bottom && !sloping_is_blocked(y);
                test_block(probe, x, y, z);
            }
    return probe.result;
}

} // namespace openminerz::world
