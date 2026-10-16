#pragma once

#include "openminerz/world/player.hpp"
#include "openminerz/world/world.hpp"

#include <cstdint>

namespace openminerz::world
{

// The original's swept box: AABBTraceProbe driven by BlockTerrain's
// BruteForceTrace. Players and every ordinary enemy move with it, so a peer
// that resolves terrain any other way drifts from what a stock client
// simulates for the same enemy (enemies are never position-replicated) and
// reports a different position for its own player.
//
// BlockFace order, as the original numbers it.
enum class ProbeFace : std::uint8_t
{
    positive_x,
    negative_z,
    negative_x,
    positive_z,
    positive_y,
    negative_y,
};

struct MovementTrace final
{
    // TraceProbe._collides / _inT / _inNormal / _inFace: the first blocking
    // voxel the box enters, as a fraction of the start-to-end segment.
    bool collides{};
    float in_t{1.0F};
    Vec3 in_normal{};
    ProbeFace in_face{};
    // SimulateSlopedSides: a block the box may ramp up instead of stopping at
    // (Player's AutoClimb).
    bool found_sloped_block{};
    float sloped_block_t{};
    int sloped_block_y{};
};

// AABBTraceProbe.Init(start, end, box) + BlockTerrain.Trace, always with
// SkipEmbedded (both movers set it). `start` and `end` are feet positions and
// the box is given relative to them. Cells outside `world` are open, as the
// original clamps the scan to its loaded window.
[[nodiscard]] MovementTrace trace_movement_box(const WorldRegion &world, Vec3 start, Vec3 end,
                                               Vec3 box_minimum, Vec3 box_maximum,
                                               bool simulate_sloped_sides) noexcept;

struct BoxSlide final
{
    Vec3 position{};
    Vec3 velocity{};
    // A +Y face stopped the box this frame (OnGround); any other face did
    // (TouchingWall).
    bool on_ground{};
    bool touching_wall{};
};

// The ResolveCollsion loop BaseZombie and PickupEntity share: up to four slide
// iterations of the swept box, stopping 0.001 of the step short of each
// contact and dropping the velocity into it; a fourth contact, or a slide that
// would turn back on the frame's own velocity, stops the box dead.
[[nodiscard]] BoxSlide slide_movement_box(const WorldRegion &world, Vec3 position, Vec3 velocity,
                                          Vec3 box_minimum, Vec3 box_maximum,
                                          float delta_seconds) noexcept;

// One frame of PickupEntity, which every peer runs for itself from the
// position and vector in CreatePickupMessage: a quarter of gravity
// (BasicPhysics.Accelerate), the swept slide of its 0.6 box
// (PickupEntity.ResolveCollsion), then OnUpdate's damping of the horizontal
// velocity: x0.9 on the ground, x0.99 in the air, and zero below 0.1. The
// damping is per FRAME in the original, so stock peers at different refresh
// rates rest a thrown stack up to a block apart; this is the 60 Hz answer.
[[nodiscard]] BoxSlide advance_pickup(const WorldRegion &world, Vec3 position, Vec3 velocity,
                                      float delta_seconds) noexcept;

// BlockType.AllowSlopes: every block that stops a player except the four
// closed door halves.
[[nodiscard]] bool allows_slopes(BlockType type) noexcept;

} // namespace openminerz::world
