#include "openminerz/world/world.hpp"

#include "openminerz/world/custom_terrain.hpp" // mod-world
#include "openminerz/world/legacy_noise.hpp"
#include "openminerz/world/liquid.hpp" // mod-liquid

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace openminerz::world
{
namespace
{

// ---------------------------------------------------------------------------
// Retail terrain is generated locally by every peer from the WorldInfoMessage
// seed; only edits travel as deltas. This generator therefore has to place the
// same block in the same cell as CastleMinerZBuilder does, and that is decided
// by float arithmetic the shipped 32-bit game runs through the legacy JIT on
// the x87 FPU: intermediates are 53-bit, and a value is narrowed to float only
// where the JIT stores it to a 4-byte slot. Which values those are differs
// from biome to biome (the JIT keeps some accumulators on the FPU stack and
// spills others), so each pass below follows the disassembled retail method,
// not the C# source. `double` is the x87 register; every static_cast<float> is
// one of the JIT's stores; every static_cast<int>(double) is its cvttsd2si.
// TerrainParityProbe diffs the result against the original builder block for
// block. Do not "simplify" a cast here without re-running it.
#if defined(_MSC_VER)
#pragma fp_contract(off)
#else
#pragma STDC FP_CONTRACT OFF
#endif

// XNA MathHelper.Lerp: value1 + ((value2 - value1) * amount), returned on the
// FPU stack. Retail truncates this unrounded value.
double lerp_x87(float value1, float value2, float amount) noexcept
{
    return static_cast<double>(value1) +
           ((static_cast<double>(value2) - static_cast<double>(value1)) *
            static_cast<double>(amount));
}

// BuildWorldChunk's distance from the origin: a double square root narrowed to
// float, then folded every 4400 blocks and mirrored in odd rings. The fold is
// exact (the operand is a float, the steps are multiples of 4400), so the
// result is always representable as a float; it is returned as double because
// that is how the blends below consume it. The original subtracts 4400 in a
// loop; the closed form is the same value without the half million iterations
// a far-away chunk would need.
double folded_radius(int x, int z) noexcept
{
    const double exact = static_cast<double>(static_cast<std::int64_t>(x) * x +
                                             static_cast<std::int64_t>(z) * z);
    double radius = static_cast<double>(static_cast<float>(std::sqrt(exact)));
    if (!(radius > 4400.0))
    {
        return radius;
    }
    // `while (r > 4400) { r -= 4400; ++rings; }` leaves r in (0, 4400].
    double remainder = std::fmod(radius, 4400.0);
    std::int64_t rings = static_cast<std::int64_t>((radius - remainder) / 4400.0);
    if (remainder == 0.0)
    {
        remainder = 4400.0;
        --rings;
    }
    return (rings & 1) != 0 ? 4400.0 - remainder : remainder;
}

// `scale * (float)coordinate * (float)frequency [+ offset]`, evaluated on the
// FPU stack and narrowed once when it is pushed as a float argument.
float octave_argument(float scale, int coordinate, int frequency, float offset = 0.0F) noexcept
{
    const double scaled =
        (static_cast<double>(scale) * static_cast<double>(static_cast<float>(coordinate))) *
        static_cast<double>(static_cast<float>(frequency));
    return static_cast<float>(scaled + static_cast<double>(offset));
}

// `(float)coordinate * scale`, narrowed once as a float argument.
float scaled_argument(int coordinate, float scale) noexcept
{
    return static_cast<float>(static_cast<double>(static_cast<float>(coordinate)) *
                              static_cast<double>(scale));
}

// The `num4 += noise(...) / num3` octave loop where the JIT keeps the
// accumulator in a float stack slot: each unrounded noise term is added at 53
// bits and the SUM is narrowed. Classic, Lagoon, Desert, Arctic, Descent and
// both hell passes compile this way.
float octaves_stored(const PerlinNoise &noise, int x, int z, float scale, int octaves,
                     float offset = 0.0F) noexcept
{
    int frequency = 1;
    float value{};
    for (int octave = 0; octave < octaves; ++octave)
    {
        const double term = noise.noise_x87(octave_argument(scale, x, frequency, offset),
                                            octave_argument(scale, z, frequency, offset)) /
                            static_cast<double>(static_cast<float>(frequency));
        value = static_cast<float>(term + static_cast<double>(value));
        frequency *= 2;
    }
    return value;
}

// The column passes below write through `Target`, which is either a `Chunk`
// (the simulated world) or a `ColumnBuffer` (one column on the stack, for the
// far-terrain summaries of WorldGenerator::surface_column). Both run the very
// same arithmetic, so a distant hill is the hill the player will walk on.
struct ColumnBuffer final
{
    std::array<BlockType, chunk_height> cells{};

    [[nodiscard]] BlockType block(int, int local_y, int) const noexcept
    {
        return local_y >= 0 && local_y < chunk_height
                   ? cells[static_cast<std::size_t>(local_y)]
                   : BlockType::empty;
    }
    void set_block(int, int local_y, int, BlockType type) noexcept
    {
        if (local_y >= 0 && local_y < chunk_height)
            cells[static_cast<std::size_t>(local_y)] = type;
    }
    void record_cave_site(int, int, int, bool) noexcept {}
    void record_hell_site(int, int, int) noexcept {}
};

template <typename Target>
void classic_column(Target &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                    int world_z, float blender)
{
    // ClassicBiome.BuildColumn. Both lerps are truncated unrounded, and the
    // octave sum (a float local) times the amplitude is truncated as a 53-bit
    // product: narrowing either to float first rounds up onto an integer where
    // retail stays one block lower.
    const int amplitude = static_cast<int>(lerp_x87(0.0F, 32.0F, blender));
    const int base = static_cast<int>(lerp_x87(32.0F, 86.0F, blender));
    const float octaves = octaves_stored(noise, world_x, world_z, 0.009375F, 4);
    int height = base + static_cast<int>(static_cast<double>(octaves) *
                                         static_cast<double>(static_cast<float>(amplitude)));
    bool sandy{};
    if (height <= 66)
    {
        height = 66;
        sandy = true;
    }
    if (height >= 128)
    {
        height = 127;
    }
    // Here the noise IS stored to the float local before it is scaled.
    const float jitter_noise =
        noise.noise(scaled_argument(world_x, 0.5F), scaled_argument(world_z, 0.5F));
    const int material_jitter = static_cast<int>(static_cast<double>(jitter_noise) * 4.0);
    for (int y = 0; y <= height; ++y)
    {
        BlockType type = BlockType::rock;
        if (y >= height - 3)
        {
            if (height + material_jitter > 95)
            {
                type = height + material_jitter > 108
                           ? BlockType::snow
                           : (height + material_jitter > 98 ? BlockType::rock : BlockType::dirt);
            }
            else if (sandy)
            {
                type = BlockType::sand;
            }
            else
            {
                type = y == height ? BlockType::grass : BlockType::dirt;
            }
        }
        chunk.set_block(local_x, y, local_z, type);
    }
}

template <typename Target>
void desert_column(Target &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                   int world_z)
{
    // DesertBiome.BuildColumn ignores its blender entirely.
    int height = 66 + static_cast<int>(static_cast<double>(octaves_stored(
                                           noise, world_x, world_z, 0.009375F, 4)) *
                                       16.0);
    height = std::max(height, 66);
    for (int y = 0; y <= height && y < chunk_height; ++y)
    {
        chunk.set_block(local_x, y, local_z, y <= height - 3 ? BlockType::rock : BlockType::sand);
    }
}

template <typename Target>
void arctic_column(Target &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                   int world_z)
{
    // ArcticBiome.BuildColumn. Its trailing `else if (j <= num5) snow` branch is
    // unreachable in the original and is deliberately not reproduced.
    int height = 64 + static_cast<int>(static_cast<double>(octaves_stored(
                                           noise, world_x, world_z, 0.009375F, 4)) *
                                       16.0);
    const bool low = height <= 54;
    height = std::max(height, 54);
    for (int y = 0; y < chunk_height; ++y)
    {
        if (y == height)
        {
            chunk.set_block(local_x, y, local_z, low ? BlockType::ice : BlockType::snow);
        }
        else if (y == height - 1)
        {
            chunk.set_block(local_x, y, local_z, BlockType::ice);
        }
        else if (y <= height)
        {
            chunk.set_block(local_x, y, local_z, BlockType::rock);
        }
    }
}

template <typename Target>
void mountain_column(Target &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                     int world_z, float blender)
{
    // MountainBiome.BuildColumn. Unlike every other biome the JIT keeps both
    // octave accumulators ON the FPU stack and only spills them to a float slot
    // around each ComputeNoise call. So the running sum is narrowed before each
    // term is added, but the sum that LEAVES the loop is the unrounded 53-bit
    // value, and that is what gets scaled and truncated.
    const int amplitude = static_cast<int>(lerp_x87(0.0F, 32.0F, blender));
    const int base = static_cast<int>(lerp_x87(32.0F, 86.0F, blender));
    double octaves{};
    int frequency = 1;
    for (int octave = 0; octave < 4; ++octave)
    {
        const float spilled = static_cast<float>(octaves);
        octaves = static_cast<double>(spilled) +
                  (noise.noise_x87(octave_argument(0.021875F, world_x, frequency),
                                   octave_argument(0.021875F, world_z, frequency)) /
                   static_cast<double>(static_cast<float>(frequency)));
        frequency *= 2;
    }
    int height =
        base + static_cast<int>(octaves * static_cast<double>(static_cast<float>(amplitude)));

    // The ridge field: `new Vector2(x / 160, z / 160)` is two float stores, and
    // `vector * num3` two more (exact, a power of two).
    const float ridge_x = scaled_argument(world_x, 0.00625F);
    const float ridge_z = scaled_argument(world_z, 0.00625F);
    double ridge_octaves{};
    frequency = 1;
    for (int octave = 0; octave < 6; ++octave)
    {
        const float spilled = static_cast<float>(ridge_octaves);
        const float scale = static_cast<float>(frequency);
        ridge_octaves = static_cast<double>(spilled) +
                        (noise.noise_x87(ridge_x * scale, ridge_z * scale) /
                         static_cast<double>(scale));
        frequency *= 2;
    }
    // `num4 = (num4 + 1) / 2; num7 = num4; num7 -= (float)Math.Floor(num7);`
    // Math.Floor receives the UNROUNDED value while num7 itself is spilled to a
    // float across the call. The two only disagree within an ulp of an integer,
    // where the result flips between +0.5 and -0.5 and the square is the same.
    const double ridge_unit = (ridge_octaves + 1.0) / 2.0;
    const float ridge_stored = static_cast<float>(ridge_unit);
    const double ridge_floor = std::floor(ridge_unit);
    double ridge = static_cast<double>(ridge_stored) -
                   static_cast<double>(static_cast<float>(ridge_floor));
    ridge = ridge - 0.5;
    const double ridge_exponent = ((-ridge) * ridge) * 1000.0;
    const float ridge_depth = static_cast<float>(std::pow(2.0, ridge_exponent));
    height -= static_cast<int>(static_cast<double>(ridge_depth) *
                               static_cast<double>(static_cast<float>(amplitude)));
    const bool low = height <= 65;
    height = std::max(height, 65);
    // `(int)(noise * blender * 10f)`: nothing is narrowed before the cast.
    height += static_cast<int>(
        (noise.noise_x87(ridge_x + 1000.0F, ridge_z + 1000.0F) * static_cast<double>(blender)) *
        10.0);
    const int jitter = static_cast<int>(
        noise.noise_x87(scaled_argument(world_x, 0.5F), scaled_argument(world_z, 0.5F)) * 4.0);
    const int snow_line = std::max(height - 4, 98) + jitter;
    if (height >= 128)
    {
        height = 127;
    }
    for (int y = 0; y <= height; ++y)
    {
        chunk.set_block(local_x, y, local_z,
                        low || y >= snow_line ? BlockType::snow : BlockType::rock);
    }
}

template <typename Target>
void lagoon_column(Target &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                   int world_z, float blender)
{
    // LagoonBiome.BuildColumn. This is retail terrain like any other band: the
    // original generates the overhangs and the sand floor at local y 66 here
    // whether or not anything draws water over them.
    int height = static_cast<int>(lerp_x87(44.0F, 76.0F, blender)) +
                 static_cast<int>(static_cast<double>(octaves_stored(noise, world_x, world_z,
                                                                     0.009375F, 4)) *
                                  32.0);
    bool sandy{};
    if (height <= 66)
    {
        sandy = true;
        height = 66;
    }
    // Scaled straight off the FPU stack, unlike Classic's stored copy.
    const int jitter = static_cast<int>(
        noise.noise_x87(scaled_argument(world_x, 0.5F), scaled_argument(world_z, 0.5F)) * 4.0);
    // `-1f + 0.65f * blender` never leaves the FPU stack; the float density is
    // compared against this 53-bit threshold.
    const double overhang_threshold =
        (static_cast<double>(blender) * static_cast<double>(0.65F)) + static_cast<double>(-1.0F);
    // `intVector * 0.0625f / 2f * (1, 1.5, 1) + (1000, 2000, 1500)`: four
    // Vector3 operators, each storing float components.
    const float vx = ((scaled_argument(world_x, 0.0625F) * 0.5F) * 1.0F) + 1000.0F;
    const float vz = ((scaled_argument(world_z, 0.0625F) * 0.5F) * 1.0F) + 1500.0F;
    for (int y = 0; y < 100; ++y)
    {
        const float vy = ((scaled_argument(y + world_min_y, 0.0625F) * 0.5F) * 1.5F) + 2000.0F;
        // The first octave is stored to the float local; the second is added
        // unrounded and the sum stored again.
        float value = noise.noise(vx, vy, vz);
        value = static_cast<float>((noise.noise_x87(vx * 2.0F, vy * 2.0F, vz * 2.0F) / 2.0) +
                                   static_cast<double>(value));
        if (static_cast<double>(value) < overhang_threshold)
        {
            chunk.set_block(local_x, y, local_z, BlockType::rock);
        }
    }
    for (int y = 0; y < 127; ++y)
    {
        if (chunk.block(local_x, y, local_z) == BlockType::rock &&
            chunk.block(local_x, y + 1, local_z) != BlockType::rock)
        {
            chunk.set_block(local_x, y, local_z, BlockType::grass);
        }
    }
    height = std::min(height, 127);
    for (int y = 0; y <= height; ++y)
    {
        BlockType type = BlockType::rock;
        if (y >= height - 3)
        {
            if (height + jitter > 85)
            {
                type = height + jitter > 88 ? BlockType::rock : BlockType::dirt;
            }
            else if (sandy)
            {
                type = BlockType::sand;
            }
            else
            {
                type = y == height ? BlockType::grass : BlockType::dirt;
            }
        }
        chunk.set_block(local_x, y, local_z, type);
    }
}

template <typename Target>
void descent_column(Target &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                    int world_z, float blender)
{
    // DecentBiome.BuildColumn. The lerp toward -16 is truncated unrounded; a
    // float lerp lands exactly on an integer on every column where
    // 80 * (radius - 3000) / 600 is whole and leaves one extra rock on top.
    const int height = static_cast<int>(lerp_x87(64.0F, -16.0F, blender)) +
                       static_cast<int>(static_cast<double>(octaves_stored(
                                            noise, world_x, world_z, 0.009375F, 4)) *
                                        16.0);
    // `blender * 2f - 1f` stays on the FPU stack for the comparison.
    const double carve_threshold = (static_cast<double>(blender) * 2.0) - 1.0;
    const float vx = scaled_argument(world_x, 0.0625F) * 1.0F;
    const float vz = scaled_argument(world_z, 0.0625F) * 1.0F;
    for (int y = 0; y < chunk_height; ++y)
    {
        if (y <= height)
        {
            chunk.set_block(local_x, y, local_z, BlockType::rock);
        }
        const float vy = scaled_argument(y + world_min_y, 0.0625F) * 1.0F;
        float cave = noise.noise(vx, vy, vz);
        cave = static_cast<float>((noise.noise_x87(vx * 2.0F, vy * 2.0F, vz * 2.0F) / 2.0) +
                                  static_cast<double>(cave));
        if (static_cast<double>(cave) < carve_threshold)
        {
            chunk.set_block(local_x, y, local_z, BlockType::empty);
        }
    }
}

template <typename Target>
void generate_modern_loot_block(Target &chunk, const IntegerNoise &integer_noise,
                                GameMode game_mode, int local_x, int local_y, int local_z,
                                int world_x, int world_y, int world_z)
{
    if (game_mode == GameMode::endurance || game_mode == GameMode::dragon_endurance)
        return;

    const BlockType existing = chunk.block(local_x, local_y, local_z);
    const bool surface_block = existing == BlockType::sand || existing == BlockType::snow;
    const int x = world_x + 333;
    const int y = world_y + 333;
    const int z = world_z + 333;
    const int noise = integer_noise.noise(x / 5, y / 5, z / 5) +
                      (integer_noise.noise(x, y, z) - 128) / 8;
    const int detail = integer_noise.noise(x / 2, y / 2, z / 2);

    if (game_mode == GameMode::survival || game_mode == GameMode::exploration)
    {
        if (surface_block && local_y > 60)
            return;
        if (noise > 268)
        {
            if (detail > 249 && (local_y < 55 || local_y >= 100))
                chunk.set_block(local_x, local_y, local_z, BlockType::lucky_loot_block);
            else if (detail > 145)
                chunk.set_block(local_x, local_y, local_z, BlockType::loot_block);
        }
        return;
    }

    // Scavenger is the remaining Modern mode that deposits these blocks.
    // Creative takes the same deployed branch, although its thresholds make
    // the result intentionally sparse at the surface.
    const int surface_modifier = surface_block && local_y > 60 ? 1 : 0;
    if (noise > 267 + surface_modifier)
    {
        if (detail > 250 + surface_modifier * 3)
            chunk.set_block(local_x, local_y, local_z, BlockType::lucky_loot_block);
        else if (detail > 165)
            chunk.set_block(local_x, local_y, local_z, BlockType::loot_block);
    }
}

// `surface_only` and `origin_tower` exist for the far-terrain summaries and are
// false/true for every simulated chunk, which leaves this exactly the retail
// pass order. With `surface_only` the two per-cell passes that cost real time
// (caves, ores) are resolved only for the cell that ends up on top: each of
// them decides a cell from that cell alone, so scanning down from the sky and
// stopping at the first cell that survives yields the same top block as
// carving the whole column. Everything under that cell is then left as the
// surface pass wrote it and must not be read -- except under an overhang:
// where the surface pass left open air beneath the top span, the scan goes on
// down to the ground under that air, and `settled_floor` receives the lowest
// cell that is then exactly generate_chunk's (the top cell's own height when
// there is no such air).
template <typename Target>
void deposits(Target &chunk, const PerlinNoise &noise, const IntegerNoise &integer_noise,
              const IntegerNoise &slime_noise, TerrainProfile profile, GameMode game_mode,
              int local_x, int local_z, int world_x, int world_z, float distance_blend,
              float hell_blend, bool deposit_crash_site, bool surface_only = false,
              bool origin_tower = true, int *settled_floor = nullptr)
{
    // HellCeilingBiome.BuildColumn: `num3++` is a float store of the sum, then
    // `(int)(num3 * 4f)` is exact.
    const float ceiling_noise = static_cast<float>(
        1.0 + static_cast<double>(octaves_stored(noise, world_x, world_z, 0.03125F, 4, 1000.0F)));
    const int ceiling = static_cast<int>(lerp_x87(0.0F, 32.0F, hell_blend)) -
                        static_cast<int>(static_cast<double>(ceiling_noise) * 4.0);
    for (int y = 0; y <= ceiling && y < chunk_height; ++y)
    {
        if (y < ceiling)
        {
            chunk.set_block(local_x, y, local_z, BlockType::empty);
        }
        else if (chunk.block(local_x, y, local_z) == BlockType::rock)
        {
            chunk.set_block(local_x, y, local_z, BlockType::blood_stone);
        }
    }

    // (surface_only) Cells under this one are the hollow a crash site leaves
    // beneath its crater: sealed under ten cells of ground, never an overhang.
    int crash_hollow_top = 0;
    if (deposit_crash_site)
    {
        // CrashSiteDepositer.BuildColumn. The noise is spilled to a float
        // (around Math.Max), but `(num2 - 0.5f) * 7f * 20f` and
        // `(num2 - 0.55f) * 10f * 20f` are two multiplies each on the FPU stack
        // and truncated unrounded -- not one float multiply by 140 or 200.
        const float crash_noise = noise.noise(scaled_argument(world_x, 0.0046875F),
                                              scaled_argument(world_z, 0.0046875F));
        if (crash_noise > 0.5F)
        {
            const int depth = static_cast<int>(
                ((static_cast<double>(crash_noise) - static_cast<double>(0.5F)) * 7.0) * 20.0);
            int rim{};
            if (crash_noise > 0.55F)
            {
                rim = std::min(depth, static_cast<int>(((static_cast<double>(crash_noise) -
                                                          static_cast<double>(0.55F)) *
                                                         10.0) *
                                                        20.0));
            }
            const int slime_blend = static_cast<int>(static_cast<double>(distance_blend) * 10.0);
            crash_hollow_top = 66 - depth - rim - 10;
            for (int y = 20; y < 126; ++y)
            {
                if (y < 66 - depth - rim - 10)
                {
                    if (chunk.block(local_x, y, local_z) != BlockType::blood_stone &&
                        chunk.block(local_x, y - 1, local_z) != BlockType::blood_stone)
                    {
                        chunk.set_block(local_x, y, local_z, BlockType::empty);
                    }
                }
                else if (rim > 0 && y < 66 - depth + rim)
                {
                    chunk.set_block(local_x, y, local_z, BlockType::space_rock);
                }
                else if (y >= 66 - depth + rim)
                {
                    chunk.set_block(local_x, y, local_z, BlockType::empty);
                }
                if (chunk.block(local_x, y, local_z) == BlockType::space_rock &&
                    y < 66 - depth + rim - 3)
                {
                    const int world_y = y + world_min_y;
                    int value =
                        slime_noise.noise((world_x + 777) / 2, (world_y + 777) / 2,
                                          (world_z + 777) / 2) +
                        (slime_noise.noise(world_x + 777, world_y + 777, world_z + 777) - 128) / 8;
                    if (value > 265 - slime_blend)
                    {
                        chunk.set_block(local_x, y, local_z, BlockType::slime);
                    }
                }
            }
        }
    }

    // (surface_only) the cell the downward scan stopped at; chunk_height when
    // the whole column was carved away.
    int resolved_top = chunk_height;
    bool first_cavity_cell = true;
    for (int step = 0; step < chunk_height; ++step)
    {
        const int y = surface_only ? chunk_height - 1 - step : step;
        const BlockType current = chunk.block(local_x, y, local_z);
        if (current == BlockType::empty || current == BlockType::uninitialized ||
            current == BlockType::sand)
        {
            first_cavity_cell = true;
            if (surface_only && current == BlockType::sand)
            {
                resolved_top = y;
                break;
            }
            continue;
        }
        // CaveBiome.BuildColumn: `intVector * 0.0625f * (1, 1.5, 1)`. The first
        // octave is spilled to a float around the second call, but the sum is
        // compared straight off the FPU stack -- it is never a float.
        const float vx = scaled_argument(world_x, 0.0625F) * 1.0F;
        const float vy = scaled_argument(y + world_min_y, 0.0625F) * 1.5F;
        const float vz = scaled_argument(world_z, 0.0625F) * 1.0F;
        const double cave = static_cast<double>(noise.noise(vx, vy, vz)) +
                            (noise.noise_x87(vx * 2.0F, vy * 2.0F, vz * 2.0F) / 2.0);
        if (cave < static_cast<double>(-0.35F))
        {
            chunk.set_block(local_x, y, local_z, BlockType::empty);
            if (profile == TerrainProfile::modern)
                chunk.record_cave_site(world_x, y + world_min_y, world_z, first_cavity_cell);
            first_cavity_cell = false;
        }
        else if (surface_only)
        {
            resolved_top = y;
            break;
        }
    }

    // (surface_only) Open air that the SHAPE of the land left under the top
    // span -- a Lagoon overhang, a floating island -- is settled as well, down
    // to the ground beneath it, so a far column can say where its top span
    // ends instead of standing on a pillar. The Hell cavern under the whole
    // world and the hollow under a crash site are not looked for: both are
    // sealed under ordinary ground, and finding them would mean carving every
    // column to the bottom.
    int lower_cell = -1;
    if (surface_only && settled_floor != nullptr)
    {
        *settled_floor = resolved_top;
        const int sealed = std::max({ceiling, crash_hollow_top - 1, 0});
        int open = resolved_top < chunk_height ? resolved_top - 1 : -1;
        while (open > sealed && chunk.block(local_x, open, local_z) != BlockType::empty &&
               chunk.block(local_x, open, local_z) != BlockType::uninitialized)
            --open;
        if (open > sealed)
        {
            const auto vacant = [&](int y) {
                const BlockType current = chunk.block(local_x, y, local_z);
                return current == BlockType::empty || current == BlockType::uninitialized;
            };
            // Whether a cell is still there once the cave pass has had it: the
            // very test of the loop above, and like it a carved cell is emptied.
            const auto survives = [&](int y) {
                if (vacant(y))
                    return false;
                if (chunk.block(local_x, y, local_z) == BlockType::sand)
                    return true;
                const float vx = scaled_argument(world_x, 0.0625F) * 1.0F;
                const float vy = scaled_argument(y + world_min_y, 0.0625F) * 1.5F;
                const float vz = scaled_argument(world_z, 0.0625F) * 1.0F;
                const double cave = static_cast<double>(noise.noise(vx, vy, vz)) +
                                    (noise.noise_x87(vx * 2.0F, vy * 2.0F, vz * 2.0F) / 2.0);
                if (cave < static_cast<double>(-0.35F))
                {
                    chunk.set_block(local_x, y, local_z, BlockType::empty);
                    return false;
                }
                return true;
            };
            // The top span, cell by cell, to where it ends...
            int y = resolved_top - 1;
            while (y >= 0 && survives(y))
                --y;
            // ...then down through the air to the ground. A piece that itself
            // hangs in that air (the shape left more air under it) is settled
            // and passed: a far column has one gap, and what it ends on must
            // be ground, or the piece would be drawn standing on a pillar.
            for (;;)
            {
                while (y >= 0 && !survives(y))
                    --y;
                if (y <= 0)
                    break;
                int next = y - 1;
                while (next > sealed && !vacant(next))
                    --next;
                if (next <= sealed)
                    break;
                for (--y; y > next; --y)
                    (void)survives(y);
            }
            lower_cell = std::max(y, 0);
            *settled_floor = lower_cell;
        }
    }

    // OreDepositer: `(int)(blender * 10f)` truncates the exact product. As a
    // float multiply 0.7f * 10 rounds up to 7.0 and shifts every ore threshold
    // on the ring at radius 2520.
    const int ore_blend = static_cast<int>(static_cast<double>(distance_blend) * 10.0);
    // (surface_only: the top cell and, under an overhang, the ground cell.)
    const int ore_first = surface_only ? (lower_cell >= 0 ? lower_cell : resolved_top) : 0;
    const int ore_last = surface_only ? std::min(resolved_top + 1, chunk_height) : chunk_height;
    for (int y = ore_first; y < ore_last; y = y == lower_cell ? resolved_top : y + 1)
    {
        const BlockType initial_block = chunk.block(local_x, y, local_z);
        if (initial_block != BlockType::rock)
        {
            if (profile == TerrainProfile::modern &&
                (initial_block == BlockType::sand || initial_block == BlockType::snow ||
                 initial_block == BlockType::blood_stone))
            {
                generate_modern_loot_block(chunk, integer_noise, game_mode, local_x, y, local_z,
                                           world_x, y + world_min_y, world_z);
            }
            continue;
        }
        const int world_y = y + world_min_y;
        int value = integer_noise.noise(world_x / 4, world_y / 4, world_z / 4) +
                    (integer_noise.noise(world_x, world_y, world_z) - 128) / 8;
        if (value > 255 - ore_blend)
        {
            chunk.set_block(local_x, y, local_z, BlockType::coal_ore);
        }
        else if (value < ore_blend - 5)
        {
            chunk.set_block(local_x, y, local_z, BlockType::copper_ore);
        }
        if (profile == TerrainProfile::modern)
        {
            generate_modern_loot_block(chunk, integer_noise, game_mode, local_x, y, local_z,
                                       world_x, world_y, world_z);
        }
        value =
            integer_noise.noise((world_x + 1000) / 3, (world_y + 1000) / 3, (world_z + 1000) / 3) +
            (integer_noise.noise(world_x + 1000, world_y + 1000, world_z + 1000) - 128) / 8;
        if (value > 264 - ore_blend)
        {
            chunk.set_block(local_x, y, local_z, BlockType::iron_ore);
        }
        else if (value < -9 + ore_blend && y < 50)
        {
            chunk.set_block(local_x, y, local_z, BlockType::gold_ore);
        }
        if (y < 50)
        {
            value =
                integer_noise.noise((world_x + 777) / 2, (world_y + 777) / 2, (world_z + 777) / 2) +
                (integer_noise.noise(world_x + 777, world_y + 777, world_z + 777) - 128) / 8;
            const int lava_threshold = profile == TerrainProfile::modern ? 266 : 265;
            if (value > lava_threshold - ore_blend)
            {
                chunk.set_block(local_x, y, local_z, BlockType::surface_lava);
            }
            else if (value < -11 + ore_blend && y < 40)
            {
                chunk.set_block(local_x, y, local_z, BlockType::diamond_ore);
            }
        }
    }

    // HellFloorBiome.BuildColumn: `4 + (int)(num2 * 10f) + 3`, the product
    // truncated unrounded (10 is not a power of two, so a float multiply can
    // round up onto the next integer).
    const float floor_noise = octaves_stored(noise, world_x, world_z, 0.03125F, 4, 200.0F);
    const int hell_height = 7 + static_cast<int>(static_cast<double>(floor_noise) * 10.0);
    if (profile == TerrainProfile::modern && hell_height > 4 && hell_height < 32)
        chunk.record_hell_site(world_x, hell_height + world_min_y, world_z);
    for (int y = 0; y < 32; ++y)
    {
        if (y < hell_height)
        {
            chunk.set_block(local_x, y, local_z, BlockType::blood_stone);
        }
        else if (y <= 4)
        {
            chunk.set_block(local_x, y, local_z, BlockType::deep_lava);
        }
    }

    const int bedrock_height = 1 + integer_noise.noise(world_x, world_z) * 3 / 256;
    for (int y = 0; y < bedrock_height; ++y)
    {
        chunk.set_block(local_x, y, local_z, BlockType::bedrock);
    }

    // OriginBiome.BuildColumn squares in 32 bits: `worldX * worldX + worldZ *
    // worldZ` wraps. Wherever the wrapped sum is negative (first beyond 46340
    // blocks from the origin) Math.Sqrt returns NaN, `(int)Math.Round(NaN)` is
    // int.MinValue, `num2 * 8` wraps to 0 and the column becomes a full
    // 125-high bedrock pillar with a lantern on top; where it wraps into
    // [0, 15625) an ordinary tower piece appears. Retail really does generate
    // these far-off slabs, so they are reproduced rather than "fixed".
    const std::int32_t radius_squared = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(world_x) * static_cast<std::uint32_t>(world_x) +
        static_cast<std::uint32_t>(world_z) * static_cast<std::uint32_t>(world_z));
    if (origin_tower && radius_squared < 15625)
    {
        const int radius =
            radius_squared < 0
                ? std::numeric_limits<int>::min()
                : static_cast<int>(std::nearbyint(std::sqrt(static_cast<double>(radius_squared))));
        if (radius <= 4)
        {
            const int height = static_cast<int>(
                125U - static_cast<std::uint32_t>(radius) * 8U);
            for (int y = 0; y < height; ++y)
            {
                chunk.set_block(local_x, y, local_z,
                                y == height - 1 && radius < 2 ? BlockType::fixed_lantern
                                                              : BlockType::bedrock);
            }
        }
    }
}

// `allowed` (mod-world) limits trunks to the columns a mod terrain reuses
// retail in; null, as for every retail chunk, plants them all.
void add_trees(Chunk &chunk, const PerlinNoise &noise,
               bool (*allowed)(const void *, int, int) = nullptr, const void *context = nullptr)
{
    const int base_x = chunk.chunk_x() * chunk_width;
    const int base_z = chunk.chunk_z() * chunk_depth;
    for (int local_z = 3; local_z < 13; ++local_z)
    {
        for (int local_x = 3; local_x < 13; ++local_x)
        {
            if (allowed != nullptr && !allowed(context, local_x, local_z))
                continue;
            const int world_x = base_x + local_x;
            const int world_z = base_z + local_z;
            // TreeDepositer.BuildColumn. The placement noise is stored to a
            // float; `!(num > 0.6f)` also rejects NaN.
            const float tree_noise =
                noise.noise(scaled_argument(world_x, 0.4375F), scaled_argument(world_z, 0.4375F));
            if (!(tree_noise > 0.6F))
            {
                continue;
            }
            int ground = 124;
            for (; ground > 0; --ground)
            {
                if (chunk.block(local_x, ground, local_z) == BlockType::grass)
                {
                    break;
                }
            }
            if (ground <= 1)
            {
                continue;
            }
            const int trunk_base = ground + 1;
            // `(int)(9f * (num - 0.6f))` truncates the exact product.
            int height = 5 + static_cast<int>((static_cast<double>(tree_noise) -
                                               static_cast<double>(0.6F)) *
                                              9.0);
            for (int offset = 0; offset < height; ++offset)
            {
                const BlockType current = chunk.block(local_x, trunk_base + offset, local_z);
                if (current != BlockType::empty && current != BlockType::uninitialized)
                {
                    height = offset;
                    break;
                }
                chunk.set_block(local_x, trunk_base + offset, local_z, BlockType::log);
            }
            const int crown_y = trunk_base + height;
            for (int dx = -3; dx <= 3; ++dx)
            {
                for (int dz = -3; dz <= 3; ++dz)
                {
                    for (int dy = -3; dy <= 3; ++dy)
                    {
                        if (chunk.block(local_x + dx, crown_y + dy, local_z + dz) !=
                            BlockType::empty)
                        {
                            continue;
                        }
                        // The leaf noise, the falloff and their sum all stay on
                        // the FPU stack; only `(float)Math.Sqrt(...)` is
                        // narrowed.
                        const double value = noise.noise_x87(
                            scaled_argument(world_x + dx, 0.5F),
                            scaled_argument(crown_y + dy + world_min_y, 0.5F),
                            scaled_argument(world_z + dz, 0.5F));
                        const float distance = static_cast<float>(
                            std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)));
                        const double falloff = 1.0 - (static_cast<double>(distance) / 3.0);
                        if (value + falloff > static_cast<double>(0.25F))
                        {
                            chunk.set_block(local_x + dx, crown_y + dy, local_z + dz,
                                            BlockType::leaves);
                        }
                    }
                }
            }
        }
    }
}

int floor_divide(int value, int divisor) noexcept
{
    return value >= 0 ? value / divisor : (value - divisor + 1) / divisor;
}

} // namespace

Chunk::Chunk(int chunk_x, int chunk_z)
    : chunk_x_(chunk_x), chunk_z_(chunk_z), blocks_(std::make_shared<BlockStorage>())
{
    blocks_->fill(BlockType::empty);
}

// mod-world: the palette index of every cell, and what the last few changed
// cells held right before they changed.
struct Chunk::ModCells final
{
    std::array<std::uint8_t, chunk_width * chunk_depth * chunk_height> cells{};
    std::vector<std::pair<std::uint16_t, std::uint8_t>> previous;
};

std::uint8_t Chunk::mod_cell(int x, int local_y, int z) const noexcept
{
    if (mods_ == nullptr || x < 0 || x >= chunk_width || z < 0 || z >= chunk_depth ||
        local_y < 0 || local_y >= chunk_height)
        return 0U;
    return mods_->cells[index(x, local_y, z)];
}

void Chunk::set_mod_cell(int x, int local_y, int z, std::uint8_t palette)
{
    if (x < 0 || x >= chunk_width || z < 0 || z >= chunk_depth || local_y < 0 ||
        local_y >= chunk_height)
        return;
    if (mods_ == nullptr)
    {
        if (palette == 0U)
            return;
        mods_ = std::make_shared<ModCells>();
    }
    else if (mods_.use_count() != 1L)
        mods_ = std::make_shared<ModCells>(*mods_);
    mods_->cells[index(x, local_y, z)] = palette;
}

std::uint8_t Chunk::previous_mod_cell(int x, int local_y, int z) const noexcept
{
    if (mods_ == nullptr || x < 0 || x >= chunk_width || z < 0 || z >= chunk_depth ||
        local_y < 0 || local_y >= chunk_height)
        return 0U;
    const auto cell = static_cast<std::uint16_t>(index(x, local_y, z));
    for (const auto &[changed, palette] : mods_->previous)
        if (changed == cell)
            return palette;
    return 0U;
}

std::uint64_t Chunk::mod_hash() const noexcept
{
    if (mods_ == nullptr)
        return 0U;
    std::uint64_t result = 1469598103934665603ULL;
    for (const std::uint8_t palette : mods_->cells)
    {
        result ^= palette;
        result *= 1099511628211ULL;
    }
    return result;
}

void Chunk::record_cave_site(int x, int y, int z, bool first)
{
    if (!sites_) sites_ = std::make_shared<ChunkGenerationSites>();
    else if (sites_.use_count() != 1) sites_ = std::make_shared<ChunkGenerationSites>(*sites_);
    ++sites_->carved_count;
    if (first) sites_->caves.push_back({{x, y, z}, sites_->carved_count});
}

void Chunk::record_hell_site(int x, int y, int z)
{
    if (!sites_) sites_ = std::make_shared<ChunkGenerationSites>();
    else if (sites_.use_count() != 1) sites_ = std::make_shared<ChunkGenerationSites>(*sites_);
    sites_->hell_floor.push_back({x, y, z});
}

int Chunk::chunk_x() const noexcept
{
    return chunk_x_;
}

int Chunk::chunk_z() const noexcept
{
    return chunk_z_;
}

std::size_t Chunk::index(int x, int local_y, int z) noexcept
{
    return static_cast<std::size_t>(x + chunk_width * (z + chunk_depth * local_y));
}

BlockType Chunk::block(int x, int local_y, int z) const noexcept
{
    if (x < 0 || x >= chunk_width || z < 0 || z >= chunk_depth || local_y < 0 ||
        local_y >= chunk_height)
    {
        return BlockType::empty;
    }
    return (*blocks_)[index(x, local_y, z)];
}

void Chunk::set_block(int x, int local_y, int z, BlockType type)
{
    if (x >= 0 && x < chunk_width && z >= 0 && z < chunk_depth && local_y >= 0 &&
        local_y < chunk_height)
    {
        // mod-world: a changed cell loses its generated mod identity, and
        // remembers it for a hook that hears of the change afterwards.
        if (mods_ != nullptr && (*blocks_)[index(x, local_y, z)] != type)
        {
            const std::size_t cell = index(x, local_y, z);
            const std::uint8_t was = mods_->cells[cell];
            const auto known = std::find_if(
                mods_->previous.begin(), mods_->previous.end(),
                [cell](const auto &entry) { return static_cast<std::size_t>(entry.first) == cell; });
            if (was != 0U || known != mods_->previous.end())
            {
                const std::ptrdiff_t known_index = known - mods_->previous.begin();
                if (mods_.use_count() != 1L)
                    mods_ = std::make_shared<ModCells>(*mods_);
                mods_->cells[cell] = 0U;
                auto &previous = mods_->previous;
                if (known_index < static_cast<std::ptrdiff_t>(previous.size()))
                    previous[static_cast<std::size_t>(known_index)].second = was;
                else
                {
                    if (previous.size() >= 64U)
                        previous.erase(previous.begin());
                    previous.emplace_back(static_cast<std::uint16_t>(cell), was);
                }
            }
        }
        if (blocks_.use_count() != 1L)
        {
            blocks_ = std::make_shared<BlockStorage>(*blocks_);
        }
        (*blocks_)[index(x, local_y, z)] = type;
    }
}

int Chunk::surface_local_y(int x, int z) const noexcept
{
    for (int y = chunk_height - 1; y >= 0; --y)
    {
        if (is_solid(block(x, y, z)))
        {
            return y;
        }
    }
    return -1;
}

std::size_t Chunk::count(BlockType type) const noexcept
{
    return static_cast<std::size_t>(std::count(blocks_->begin(), blocks_->end(), type));
}

std::uint64_t Chunk::hash() const noexcept
{
    std::uint64_t result = 1469598103934665603ULL;
    for (const BlockType block_type : *blocks_)
    {
        result ^= static_cast<std::uint8_t>(block_type);
        result *= 1099511628211ULL;
    }
    return result;
}

WorldRegion::WorldRegion(int origin_chunk_x, int origin_chunk_z, int chunks_x, int chunks_z)
    : origin_chunk_x_(origin_chunk_x), origin_chunk_z_(origin_chunk_z), chunks_x_(chunks_x),
      chunks_z_(chunks_z)
{
    if (chunks_x <= 0 || chunks_z <= 0)
    {
        throw std::invalid_argument{"world region dimensions must be positive"};
    }
    chunks_.reserve(static_cast<std::size_t>(chunks_x * chunks_z));
    for (int z = 0; z < chunks_z; ++z)
    {
        for (int x = 0; x < chunks_x; ++x)
        {
            chunks_.emplace_back(origin_chunk_x + x, origin_chunk_z + z);
        }
    }
}

WorldRegion::WorldRegion(int origin_chunk_x, int origin_chunk_z, int chunks_x, int chunks_z,
                         std::vector<Chunk> chunks)
    : origin_chunk_x_(origin_chunk_x), origin_chunk_z_(origin_chunk_z), chunks_x_(chunks_x),
      chunks_z_(chunks_z), chunks_(std::move(chunks))
{
    if (chunks_x <= 0 || chunks_z <= 0 ||
        chunks_.size() != static_cast<std::size_t>(chunks_x) * static_cast<std::size_t>(chunks_z))
    {
        throw std::invalid_argument{"world region columns do not fill its rectangle"};
    }
    for (int z = 0; z < chunks_z; ++z)
    {
        for (int x = 0; x < chunks_x; ++x)
        {
            const Chunk &chunk = chunks_[static_cast<std::size_t>(x + z * chunks_x)];
            if (chunk.chunk_x() != origin_chunk_x + x || chunk.chunk_z() != origin_chunk_z + z)
                throw std::invalid_argument{"world region columns are out of order"};
        }
    }
}

int WorldRegion::origin_chunk_x() const noexcept
{
    return origin_chunk_x_;
}
int WorldRegion::origin_chunk_z() const noexcept
{
    return origin_chunk_z_;
}
int WorldRegion::chunks_x() const noexcept
{
    return chunks_x_;
}
int WorldRegion::chunks_z() const noexcept
{
    return chunks_z_;
}
int WorldRegion::minimum_x() const noexcept
{
    return origin_chunk_x_ * chunk_width;
}
int WorldRegion::minimum_z() const noexcept
{
    return origin_chunk_z_ * chunk_depth;
}
int WorldRegion::maximum_x() const noexcept
{
    return minimum_x() + chunks_x_ * chunk_width - 1;
}
int WorldRegion::maximum_z() const noexcept
{
    return minimum_z() + chunks_z_ * chunk_depth - 1;
}
bool WorldRegion::contains(int world_x, int world_y, int world_z) const noexcept
{
    return world_x >= minimum_x() && world_x <= maximum_x() && world_z >= minimum_z() &&
           world_z <= maximum_z() && world_y >= world_min_y && world_y < world_min_y + chunk_height;
}
const std::vector<Chunk> &WorldRegion::chunks() const noexcept
{
    return chunks_;
}
std::vector<Chunk> &WorldRegion::chunks() noexcept
{
    return chunks_;
}

BlockType WorldRegion::block(int world_x, int world_y, int world_z) const noexcept
{
    const int chunk_x = floor_divide(world_x, chunk_width);
    const int chunk_z = floor_divide(world_z, chunk_depth);
    const int region_x = chunk_x - origin_chunk_x_;
    const int region_z = chunk_z - origin_chunk_z_;
    const int local_y = world_y - world_min_y;
    if (region_x < 0 || region_x >= chunks_x_ || region_z < 0 || region_z >= chunks_z_ ||
        local_y < 0 || local_y >= chunk_height)
    {
        return BlockType::empty;
    }
    const int local_x = world_x - chunk_x * chunk_width;
    const int local_z = world_z - chunk_z * chunk_depth;
    const std::size_t chunk_index = static_cast<std::size_t>(region_x + chunks_x_ * region_z);
    return chunks_[chunk_index].block(local_x, local_y, local_z);
}

std::uint8_t WorldRegion::mod_cell(int world_x, int world_y, int world_z) const noexcept
{
    const int chunk_x = floor_divide(world_x, chunk_width);
    const int chunk_z = floor_divide(world_z, chunk_depth);
    const int region_x = chunk_x - origin_chunk_x_;
    const int region_z = chunk_z - origin_chunk_z_;
    if (region_x < 0 || region_x >= chunks_x_ || region_z < 0 || region_z >= chunks_z_)
        return 0U;
    return chunks_[static_cast<std::size_t>(region_x + chunks_x_ * region_z)].mod_cell(
        world_x - chunk_x * chunk_width, world_y - world_min_y, world_z - chunk_z * chunk_depth);
}

void WorldRegion::set_block(int world_x, int world_y, int world_z, BlockType type)
{
    const int chunk_x = floor_divide(world_x, chunk_width);
    const int chunk_z = floor_divide(world_z, chunk_depth);
    const int region_x = chunk_x - origin_chunk_x_;
    const int region_z = chunk_z - origin_chunk_z_;
    const int local_y = world_y - world_min_y;
    if (region_x < 0 || region_x >= chunks_x_ || region_z < 0 || region_z >= chunks_z_ ||
        local_y < 0 || local_y >= chunk_height)
    {
        return;
    }
    const int local_x = world_x - chunk_x * chunk_width;
    const int local_z = world_z - chunk_z * chunk_depth;
    chunks_[static_cast<std::size_t>(region_x + chunks_x_ * region_z)].set_block(local_x, local_y,
                                                                                 local_z, type);
}

std::vector<BlockRemovalEffect> WorldRegion::removal_effects(int world_x, int world_y,
                                                             int world_z) const
{
    constexpr std::array<std::array<int, 3>, 6> offsets{{
        {{1, 0, 0}},
        {{0, 0, -1}},
        {{-1, 0, 0}},
        {{0, 0, 1}},
        {{0, 1, 0}},
        {{0, -1, 0}},
    }};
    std::vector<BlockRemovalEffect> result;
    const auto append = [&](int x, int y, int z, bool dependent) {
        if (!contains(x, y, z))
            return;
        const BlockType type = block(x, y, z);
        if (type == BlockType::empty || type == BlockType::uninitialized)
            return;
        const auto duplicate = std::find_if(result.begin(), result.end(), [&](const auto &entry) {
            return entry.x == x && entry.y == y && entry.z == z;
        });
        if (duplicate == result.end())
            result.push_back({x, y, z, type, dependent});
    };
    const auto append_door_partner = [&](int x, int y, int z, BlockType type) {
        if (is_lower_door(type))
            append(x, y + 1, z, false);
        else if (is_upper_door(type))
            append(x, y - 1, z, false);
    };

    const BlockType primary = block(world_x, world_y, world_z);
    append(world_x, world_y, world_z, false);
    append_door_partner(world_x, world_y, world_z, primary);

    // InGameHUD.Dig / Explosive.RememberDependentObjects compare each
    // neighbour's BlockType.Facing with the face from the removed support to
    // that neighbour.
    for (std::size_t face = 0; face < offsets.size(); ++face)
    {
        const int x = world_x + offsets[face][0];
        const int y = world_y + offsets[face][1];
        const int z = world_z + offsets[face][2];
        const BlockType dependent = block(x, y, z);
        if (attached_face(dependent) != static_cast<FaceDirection>(face))
            continue;
        append(x, y, z, true);
        append_door_partner(x, y, z, dependent);
    }
    return result;
}

namespace
{

constexpr std::array<std::array<int, 3>, 6> face_offsets{{
    {{1, 0, 0}},
    {{0, 0, -1}},
    {{-1, 0, 0}},
    {{0, 0, 1}},
    {{0, 1, 0}},
    {{0, -1, 0}},
}};

constexpr std::array<std::array<std::array<int, 3>, 4>, 6> face_corners{{
    {{{{1, 0, 0}}, {{1, 1, 0}}, {{1, 1, 1}}, {{1, 0, 1}}}},
    {{{{1, 0, 0}}, {{0, 0, 0}}, {{0, 1, 0}}, {{1, 1, 0}}}},
    {{{{0, 0, 1}}, {{0, 1, 1}}, {{0, 1, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 1}}, {{1, 0, 1}}, {{1, 1, 1}}, {{0, 1, 1}}}},
    {{{{0, 1, 0}}, {{1, 1, 0}}, {{1, 1, 1}}, {{0, 1, 1}}}},
    {{{{0, 0, 1}}, {{1, 0, 1}}, {{1, 0, 0}}, {{0, 0, 0}}}},
}};

// A level-15 source reaches at most fifteen transparent cells. Rebirth's
// The primary probe looks six cells into the outward face hemisphere. Its local
// second bounce looks three cells around the surface it hits, then direct-light
// lookup samples one more neighbour. Compact section rebuilds therefore need a
// 25-cell apron to remain identical to a full-region bake.
constexpr int direct_light_reach = 15;
constexpr int indirect_probe_radius = 6;
constexpr int secondary_probe_radius = 3;
constexpr int indirect_surface_apron = indirect_probe_radius + secondary_probe_radius + 1;
constexpr int light_bake_margin = direct_light_reach + indirect_surface_apron;

bool lighting_occluder(const WorldRegion &region, int x, int y, int z) noexcept
{
    return !region.contains(x, y, z) || occludes(region.block(x, y, z));
}

// Full voxel light volume with the original four-bit contract: sunlight seeds at 15
// down every sky column and light channels flood-fill through transparent cells with
// TransmitLight(in) = max(0, (in - 1) * LightTransmission >> 4). The previous
// heightmap heuristic leaked daylight through solid walls into caves because its
// horizontal search never tested occlusion along the path.
struct LightMap final
{
    int minimum_x{};
    int minimum_z{};
    int width{};
    int depth{};
    std::vector<std::uint8_t> sun;
    std::vector<std::uint8_t> torch;
    std::vector<std::uint8_t> lava;

    [[nodiscard]] bool covers(int x, int y, int z) const noexcept
    {
        return x >= minimum_x && x < minimum_x + width && z >= minimum_z && z < minimum_z + depth &&
               y >= world_min_y && y < world_min_y + chunk_height;
    }

    [[nodiscard]] std::size_t cell(int x, int y, int z) const noexcept
    {
        return (static_cast<std::size_t>(z - minimum_z) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x - minimum_x)) *
                   static_cast<std::size_t>(chunk_height) +
               static_cast<std::size_t>(y - world_min_y);
    }

    [[nodiscard]] std::uint8_t sun_at(int x, int y, int z) const noexcept
    {
        return covers(x, y, z) ? sun[cell(x, y, z)] : 0U;
    }

    [[nodiscard]] std::uint8_t torch_at(int x, int y, int z) const noexcept
    {
        return covers(x, y, z) ? torch[cell(x, y, z)] : 0U;
    }
    [[nodiscard]] std::uint8_t lava_at(int x, int y, int z) const noexcept
    {
        return covers(x, y, z) ? lava[cell(x, y, z)] : 0U;
    }
};

void flood_fill_light(const WorldRegion &region, LightMap &map, std::vector<std::uint8_t> &channel,
                      std::vector<std::uint32_t> &queue)
{
    std::size_t head = 0U;
    while (head < queue.size())
    {
        const std::uint32_t packed = queue[head++];
        const int local_y = static_cast<int>(packed % static_cast<std::uint32_t>(chunk_height));
        const auto column = packed / static_cast<std::uint32_t>(chunk_height);
        const int local_x = static_cast<int>(column % static_cast<std::uint32_t>(map.width));
        const int local_z = static_cast<int>(column / static_cast<std::uint32_t>(map.width));
        const int x = map.minimum_x + local_x;
        const int y = world_min_y + local_y;
        const int z = map.minimum_z + local_z;
        const std::uint8_t level = channel[packed];
        if (level <= 1U)
            continue;
        constexpr std::array<std::array<int, 3>, 6> neighbors{
            {{{1, 0, 0}}, {{-1, 0, 0}}, {{0, 1, 0}}, {{0, -1, 0}}, {{0, 0, 1}}, {{0, 0, -1}}}};
        for (const auto &offset : neighbors)
        {
            const int nx = x + offset[0];
            const int ny = y + offset[1];
            const int nz = z + offset[2];
            if (!map.covers(nx, ny, nz) || !region.contains(nx, ny, nz))
                continue;
            const std::uint8_t transmission = light_transmission(region.block(nx, ny, nz));
            if (transmission == 0U)
                continue;
            const auto next = static_cast<std::uint8_t>(
                (static_cast<int>(level) - 1) * static_cast<int>(transmission) >> 4);
            const std::size_t neighbor_cell = map.cell(nx, ny, nz);
            if (next > channel[neighbor_cell])
            {
                channel[neighbor_cell] = next;
                queue.push_back(static_cast<std::uint32_t>(neighbor_cell));
            }
        }
    }
}

// `margin` is how far beyond the selection the flood is computed; the default
// is the full bake reach. `bright_seeds` are extra cells seeded at maximum
// torch and lava level, which the light-change probe uses to bound what an
// edited cell could ever have emitted.
LightMap build_light_map(const WorldRegion &region, int selection_minimum_x,
                         int selection_minimum_z, int selection_maximum_x, int selection_maximum_z,
                         int margin = light_bake_margin,
                         const std::vector<std::array<int, 3>> *bright_seeds = nullptr)
{
    // Include the original flood reach plus both directional bounce probes. This
    // makes a selected section's packed radiance exactly match a full mesh bake.
    LightMap map;
    map.minimum_x = std::max(region.minimum_x(), selection_minimum_x - margin);
    map.minimum_z = std::max(region.minimum_z(), selection_minimum_z - margin);
    const int maximum_x = std::min(region.maximum_x(), selection_maximum_x + margin);
    const int maximum_z = std::min(region.maximum_z(), selection_maximum_z + margin);
    map.width = maximum_x - map.minimum_x + 1;
    map.depth = maximum_z - map.minimum_z + 1;
    if (map.width <= 0 || map.depth <= 0)
    {
        map.width = 0;
        map.depth = 0;
        return map;
    }
    const std::size_t cells = static_cast<std::size_t>(map.width) *
                              static_cast<std::size_t>(map.depth) *
                              static_cast<std::size_t>(chunk_height);
    map.sun.assign(cells, 0U);
    map.torch.assign(cells, 0U);
    map.lava.assign(cells, 0U);

    // Sunlight columns: full brightness through every fully-transmissive cell from the
    // sky down, exactly like the original IsSky scan.
    std::vector<int> sky_floor(static_cast<std::size_t>(map.width * map.depth),
                               world_min_y + chunk_height);
    // Canopy cells lit directly by the sky; they seed the flood fill so light
    // spreads sideways through the leaves.
    std::vector<std::uint32_t> sunlit_translucent;
    for (int z = map.minimum_z; z <= maximum_z; ++z)
    {
        for (int x = map.minimum_x; x <= maximum_x; ++x)
        {
            int y = world_min_y + chunk_height - 1;
            for (; y >= world_min_y; --y)
            {
                if (light_transmission(region.block(x, y, z)) != 16U)
                    break;
                map.sun[map.cell(x, y, z)] = 15U;
            }
            // The original's sky scan does not stop dead at a partly
            // transmissive block: the first leaf (or ice) under open sky is
            // itself marked lit at `LightTransmission - 1`. Without this the
            // top of a flat canopy is never reached by the flood fill at all
            // and renders pitch black.
            if (y >= world_min_y)
            {
                const std::uint8_t transmission = light_transmission(region.block(x, y, z));
                if (transmission > 0U && transmission < 16U)
                {
                    const std::size_t cell = map.cell(x, y, z);
                    const auto seeded = static_cast<std::uint8_t>(transmission - 1U);
                    if (seeded > map.sun[cell])
                    {
                        map.sun[cell] = seeded;
                        sunlit_translucent.push_back(static_cast<std::uint32_t>(cell));
                    }
                }
            }
            sky_floor[static_cast<std::size_t>(x - map.minimum_x +
                                               (z - map.minimum_z) * map.width)] = y + 1;
        }
    }
    // Seed the sun flood fill only where a sky column borders terrain; interior sky
    // cells surrounded by more sky can never push a brighter value anywhere.
    std::vector<std::uint32_t> queue;
    for (int z = map.minimum_z; z <= maximum_z; ++z)
    {
        for (int x = map.minimum_x; x <= maximum_x; ++x)
        {
            const int own_floor = sky_floor[static_cast<std::size_t>(
                x - map.minimum_x + (z - map.minimum_z) * map.width)];
            int deepest_neighbor = own_floor;
            for (const auto &offset :
                 std::array<std::array<int, 2>, 4>{{{{1, 0}}, {{-1, 0}}, {{0, 1}}, {{0, -1}}}})
            {
                const int nx = x + offset[0];
                const int nz = z + offset[1];
                if (nx < map.minimum_x || nx > maximum_x || nz < map.minimum_z || nz > maximum_z)
                    continue;
                deepest_neighbor = std::max(
                    deepest_neighbor, sky_floor[static_cast<std::size_t>(
                                          nx - map.minimum_x + (nz - map.minimum_z) * map.width)]);
            }
            for (int y = own_floor; y < deepest_neighbor; ++y)
                queue.push_back(static_cast<std::uint32_t>(map.cell(x, y, z)));
        }
    }
    queue.insert(queue.end(), sunlit_translucent.begin(), sunlit_translucent.end());
    flood_fill_light(region, map, map.sun, queue);

    // Torch channel: seed every self-illuminating block in the apron volume.
    queue.clear();
    std::vector<std::uint32_t> lava_queue;
    for (int z = map.minimum_z; z <= maximum_z; ++z)
    {
        for (int x = map.minimum_x; x <= maximum_x; ++x)
        {
            for (int y = world_min_y; y < world_min_y + chunk_height; ++y)
            {
                const std::uint8_t illumination = self_illumination(region.block(x, y, z));
                if (illumination == 0U)
                    continue;
                const std::size_t cell = map.cell(x, y, z);
                map.torch[cell] = illumination;
                queue.push_back(static_cast<std::uint32_t>(cell));
                const auto block = region.block(x, y, z);
                if (block == BlockType::surface_lava || block == BlockType::deep_lava)
                {
                    map.lava[cell] = illumination;
                    lava_queue.push_back(static_cast<std::uint32_t>(cell));
                }
            }
        }
    }
    if (bright_seeds != nullptr)
    {
        for (const std::array<int, 3> &seed : *bright_seeds)
        {
            if (!map.covers(seed[0], seed[1], seed[2]))
                continue;
            const std::size_t cell = map.cell(seed[0], seed[1], seed[2]);
            map.torch[cell] = 15U;
            map.lava[cell] = 15U;
            queue.push_back(static_cast<std::uint32_t>(cell));
            lava_queue.push_back(static_cast<std::uint32_t>(cell));
        }
    }
    flood_fill_light(region, map, map.torch, queue);
    flood_fill_light(region, map, map.lava, lava_queue);
    return map;
}

struct SurfaceLight final
{
    float sky{};
    float torch{};
};

[[nodiscard]] SurfaceLight brightest_surface_light(const WorldRegion &region, const LightMap &light,
                                                   int x, int y, int z) noexcept
{
    SurfaceLight result{};
    const BlockType surface = region.block(x, y, z);
    result.torch = static_cast<float>(self_illumination(surface)) / 15.0F;
    // The adjacent transparent cell is the incident field on this surface. A
    // maximum is intentional: it preserves a narrow sun shaft or torch-facing
    // side instead of averaging it away with the solid cells behind the block.
    for (const auto &offset : face_offsets)
    {
        const int nx = x + offset[0];
        const int ny = y + offset[1];
        const int nz = z + offset[2];
        if (!region.contains(nx, ny, nz) || light_transmission(region.block(nx, ny, nz)) == 0U)
            continue;
        result.sky = std::max(result.sky, static_cast<float>(light.sun_at(nx, ny, nz)) / 15.0F);
        result.torch =
            std::max(result.torch, static_cast<float>(light.torch_at(nx, ny, nz)) / 15.0F);
    }
    return result;
}

[[nodiscard]] std::array<float, 3> linear_reflectance(BlockType type) noexcept
{
    // The fallback palette is display-authored like the diffuse atlas. Gamma-two
    // decoding turns it into a compact material-average reflectance for the CPU
    // bake without tying world generation to the selected texture pack.
    const BlockColor color = fallback_color(type);
    const auto channel = [](std::uint8_t value) {
        const float display = static_cast<float>(value) / 255.0F;
        return display * display;
    };
    return {channel(color.red), channel(color.green), channel(color.blue)};
}

struct DiffuseRadiance final
{
    std::array<float, 3> sky{};
    float torch{};
};

[[nodiscard]] bool indirect_probe_surface(BlockType candidate) noexcept
{
    if (!is_solid(candidate))
        return false;
    // Thin fixtures do not subtend a complete voxel direction. Their emission
    // is already represented by the occlusion-safe torch flood.
    if (!occludes(candidate) && !lights_as_translucent(candidate) && !emits_light(candidate))
    {
        return false;
    }
    return candidate < BlockType::torch || candidate > BlockType::torch_neg_y;
}

[[nodiscard]] DiffuseRadiance direct_surface_reflection(const WorldRegion &region,
                                                        const LightMap &light, int x, int y,
                                                        int z) noexcept
{
    const BlockType surface = region.block(x, y, z);
    const SurfaceLight incident = brightest_surface_light(region, light, x, y, z);
    const auto reflectance = linear_reflectance(surface);
    DiffuseRadiance result;
    for (std::size_t channel = 0; channel < result.sky.size(); ++channel)
        result.sky[channel] = reflectance[channel] * incident.sky;
    const float reflectance_luma =
        reflectance[0] * 0.2126F + reflectance[1] * 0.7152F + reflectance[2] * 0.0722F;
    result.torch = reflectance_luma * incident.torch;
    return result;
}

[[nodiscard]] bool second_bounce_enabled() noexcept
{
    // Capture-only diagnostic switch. Cache once because this path runs for
    // every exposed face and the setting is startup-scoped by design.
    static const bool enabled = [] {
#if defined(_WIN32)
        char *value{};
        std::size_t value_size{};
        const bool missing =
            _dupenv_s(&value, &value_size, "OPENMINERZ_DISABLE_SECOND_BOUNCE") != 0 ||
            value == nullptr;
        std::free(value);
        return missing;
#else
        return std::getenv("OPENMINERZ_DISABLE_SECOND_BOUNCE") == nullptr;
#endif
    }();
    return enabled;
}

[[nodiscard]] DiffuseRadiance secondary_surface_irradiance(
    const WorldRegion &region, const LightMap &light, int x, int y, int z,
    const std::array<int, 3> &normal, const std::array<int, 2> &tangent_axes) noexcept
{
    DiffuseRadiance result;
    float hemisphere_weight{};
    // A five-ray cross is enough for low-frequency mutual bounce while keeping
    // chunk-mesh compilation bounded. Unlike screen-space GI it remains stable
    // off-camera and across streamed chunk rebuilds.
    constexpr std::array<std::array<int, 2>, 5> cross{{
        {{0, 0}},
        {{1, 0}},
        {{-1, 0}},
        {{0, 1}},
        {{0, -1}},
    }};
    for (const auto &offset : cross)
    {
        std::array<int, 3> ray = normal;
        ray[static_cast<std::size_t>(tangent_axes[0])] += offset[0];
        ray[static_cast<std::size_t>(tangent_axes[1])] += offset[1];
        const float ray_length =
            std::sqrt(static_cast<float>(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]));
        const float cosine_weight = 1.0F / ray_length;
        hemisphere_weight += cosine_weight;
        for (int step = 1; step <= secondary_probe_radius; ++step)
        {
            const int sample_x = x + ray[0] * step;
            const int sample_y = y + ray[1] * step;
            const int sample_z = z + ray[2] * step;
            if (!region.contains(sample_x, sample_y, sample_z))
                break;
            if (!indirect_probe_surface(region.block(sample_x, sample_y, sample_z)))
                continue;
            const DiffuseRadiance reflected =
                direct_surface_reflection(region, light, sample_x, sample_y, sample_z);
            const float distance = static_cast<float>(step - 1);
            const float weight = cosine_weight / (1.0F + distance * distance * 0.20F);
            for (std::size_t channel = 0; channel < result.sky.size(); ++channel)
                result.sky[channel] += reflected.sky[channel] * weight;
            result.torch += reflected.torch * weight;
            break;
        }
    }
    if (hemisphere_weight > 0.0F)
    {
        for (float &channel : result.sky)
            channel /= hemisphere_weight;
        result.torch /= hemisphere_weight;
    }
    return result;
}

void fill_face_indirect_light(WorldFace &face, const WorldRegion &region, const LightMap &light,
                              const std::array<int, 3> &normal,
                              const std::array<int, 2> &tangent_axes) noexcept
{
    std::array<float, 3> sky_radiance{};
    float torch_radiance{};
    float hemisphere_weight{};

    // Nine fixed rays form a low-frequency cosine-weighted probe over the
    // exposed face hemisphere. First-hit surfaces provide one diffuse bounce;
    // open rays contribute no reflected energy. The field is directional because
    // every face orientation bakes its own outward hemisphere.
    for (int first = -1; first <= 1; ++first)
    {
        for (int second = -1; second <= 1; ++second)
        {
            std::array<int, 3> ray = normal;
            ray[static_cast<std::size_t>(tangent_axes[0])] += first;
            ray[static_cast<std::size_t>(tangent_axes[1])] += second;
            const float ray_length =
                std::sqrt(static_cast<float>(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]));
            const float cosine_weight = 1.0F / ray_length;
            hemisphere_weight += cosine_weight;

            for (int step = 1; step <= indirect_probe_radius; ++step)
            {
                const int x = face.x + ray[0] * step;
                const int y = face.y + ray[1] * step;
                const int z = face.z + ray[2] * step;
                if (!region.contains(x, y, z))
                    break;
                const BlockType candidate = region.block(x, y, z);
                if (!indirect_probe_surface(candidate))
                    continue;

                const SurfaceLight incident = brightest_surface_light(region, light, x, y, z);
                const auto reflectance = linear_reflectance(candidate);
                DiffuseRadiance secondary;
                if (second_bounce_enabled())
                {
                    const std::array<int, 3> candidate_normal{-normal[0], -normal[1], -normal[2]};
                    secondary = secondary_surface_irradiance(region, light, x, y, z,
                                                             candidate_normal, tangent_axes);
                }
                // A fixed ray represents solid angle, so distance should not use
                // point-light inverse-square falloff. This gentle term only keeps
                // isolated far voxels from occupying a complete probe direction.
                const float distance = static_cast<float>(step - 1);
                const float weight = cosine_weight / (1.0F + distance * distance * 0.12F);
                for (std::size_t channel = 0; channel < sky_radiance.size(); ++channel)
                {
                    // The second reflected field already includes the remote
                    // material albedo. Apply this hit surface's albedo here for
                    // a real two-surface energy path. A restrained transport
                    // coefficient prevents bright snow enclosures from clipping
                    // the compact R8 radiance channel.
                    constexpr float secondary_transport = 0.72F;
                    sky_radiance[channel] +=
                        reflectance[channel] *
                        (incident.sky + secondary.sky[channel] * secondary_transport) * weight;
                }
                const float reflectance_luma =
                    reflectance[0] * 0.2126F + reflectance[1] * 0.7152F + reflectance[2] * 0.0722F;
                constexpr float secondary_torch_transport = 0.62F;
                torch_radiance += reflectance_luma *
                                  (incident.torch + secondary.torch * secondary_torch_transport) *
                                  weight;
                break;
            }
        }
    }

    const auto encode = [hemisphere_weight](float value) {
        const float normalized = hemisphere_weight > 0.0F ? value / hemisphere_weight : 0.0F;
        return static_cast<std::uint8_t>(
            std::lround(std::sqrt(std::clamp(normalized, 0.0F, 1.0F)) * 255.0F));
    };
    face.indirect_light = {encode(sky_radiance[0]), encode(sky_radiance[1]),
                           encode(sky_radiance[2]), encode(torch_radiance)};
}

void fill_face_block_light_lobe(WorldFace &face, const WorldRegion &region, const LightMap &light,
                                const std::array<int, 3> &normal) noexcept
{
    const std::array<int, 3> outside{face.x + normal[0], face.y + normal[1], face.z + normal[2]};
    const float center = static_cast<float>(light.torch_at(outside[0], outside[1], outside[2]));
    const auto sample = [&](int x, int y, int z) {
        // A solid neighbor is not a zero-light sample: treating the receiving
        // block behind the face as darkness creates a false outward gradient.
        // Hold the exposed-cell value across an occluder instead.
        if (!region.contains(x, y, z) || lighting_occluder(region, x, y, z))
            return center;
        return static_cast<float>(light.torch_at(x, y, z));
    };

    std::array<float, 3> gradient{};
    for (std::size_t axis = 0U; axis < gradient.size(); ++axis)
    {
        auto positive = outside;
        auto negative = outside;
        ++positive[axis];
        --negative[axis];
        gradient[axis] = sample(positive[0], positive[1], positive[2]) -
                         sample(negative[0], negative[1], negative[2]);
    }
    float magnitude = std::sqrt(gradient[0] * gradient[0] + gradient[1] * gradient[1] +
                                gradient[2] * gradient[2]);
    float confidence = std::clamp(magnitude / 3.0F, 0.0F, 1.0F);
    if (magnitude > 0.001F)
    {
        for (float &component : gradient)
            component /= magnitude;
        const float inward = gradient[0] * static_cast<float>(normal[0]) +
                             gradient[1] * static_cast<float>(normal[1]) +
                             gradient[2] * static_cast<float>(normal[2]);
        if (inward < 0.0F)
        {
            // A face only receives light from its exposed hemisphere. Preserve
            // a valid tangential gradient but discard any component that would
            // have crossed the solid voxel behind it.
            for (std::size_t axis = 0U; axis < gradient.size(); ++axis)
                gradient[axis] -= static_cast<float>(normal[axis]) * inward;
            magnitude = std::sqrt(gradient[0] * gradient[0] + gradient[1] * gradient[1] +
                                  gradient[2] * gradient[2]);
            if (magnitude > 0.001F)
                for (float &component : gradient)
                    component /= magnitude;
            else
                confidence = 0.0F;
        }
    }
    else
    {
        gradient = {};
        confidence = 0.0F;
    }

    const auto encode_direction = [](float value) {
        return static_cast<std::uint8_t>(
            std::lround(std::clamp(value * 0.5F + 0.5F, 0.0F, 1.0F) * 255.0F));
    };
    face.block_light_lobe = {encode_direction(gradient[0]), encode_direction(gradient[1]),
                             encode_direction(gradient[2]),
                             static_cast<std::uint8_t>(std::lround(confidence * 255.0F))};
}

void fill_face_lighting(WorldFace &face, const WorldRegion &region, const LightMap &light) noexcept
{
    const auto direction_index = static_cast<std::size_t>(face.direction);
    const auto &normal = face_offsets[direction_index];
    std::array<int, 2> tangent_axes{};
    int tangent_count{};
    for (int axis = 0; axis < 3; ++axis)
    {
        if (normal[static_cast<std::size_t>(axis)] == 0)
        {
            tangent_axes[static_cast<std::size_t>(tangent_count++)] = axis;
        }
    }

    const std::array<int, 3> outside{face.x + normal[0], face.y + normal[1], face.z + normal[2]};
    constexpr std::array<std::uint8_t, 4> ao_levels{145U, 181U, 219U, 255U};
    for (std::size_t corner_index = 0; corner_index < 4U; ++corner_index)
    {
        const auto &corner = face_corners[direction_index][corner_index];
        const int first_axis = tangent_axes[0];
        const int second_axis = tangent_axes[1];
        const int first_sign = corner[static_cast<std::size_t>(first_axis)] == 0 ? -1 : 1;
        const int second_sign = corner[static_cast<std::size_t>(second_axis)] == 0 ? -1 : 1;

        auto first = outside;
        auto second = outside;
        auto diagonal = outside;
        first[static_cast<std::size_t>(first_axis)] += first_sign;
        second[static_cast<std::size_t>(second_axis)] += second_sign;
        diagonal[static_cast<std::size_t>(first_axis)] += first_sign;
        diagonal[static_cast<std::size_t>(second_axis)] += second_sign;

        const bool first_blocked = lighting_occluder(region, first[0], first[1], first[2]);
        const bool second_blocked = lighting_occluder(region, second[0], second[1], second[2]);
        const bool diagonal_blocked =
            lighting_occluder(region, diagonal[0], diagonal[1], diagonal[2]);
        const int ao = first_blocked && second_blocked ? 0
                                                       : 3 - static_cast<int>(first_blocked) -
                                                             static_cast<int>(second_blocked) -
                                                             static_cast<int>(diagonal_blocked);
        face.ambient_occlusion[corner_index] = ao_levels[static_cast<std::size_t>(ao)];

        const std::array<std::array<int, 3>, 4> samples{outside, first, second, diagonal};
        int accumulated_sun{};
        int accumulated_torch{};
        int accumulated_lava{};
        int count{};
        for (const auto &sample : samples)
        {
            if (!lighting_occluder(region, sample[0], sample[1], sample[2]))
            {
                accumulated_sun += light.sun_at(sample[0], sample[1], sample[2]);
                accumulated_torch += light.torch_at(sample[0], sample[1], sample[2]);
                accumulated_lava += light.lava_at(sample[0], sample[1], sample[2]);
                ++count;
            }
        }
        // Four-bit levels scale by 17 so a full 15 maps exactly onto byte 255.
        auto sun_level = count == 0 ? 0U : static_cast<unsigned>(accumulated_sun * 17 / count);
        auto torch_level = count == 0 ? 0U : static_cast<unsigned>(accumulated_torch * 17 / count);
        auto lava_level = count == 0 ? 0U : static_cast<unsigned>(accumulated_lava * 17 / count);
        // RenderChunk's LightAsTranslucent rule: a leaf or ice face is never
        // lit darker than the block's own stored level. Neighbouring leaves
        // are dim, so without this every leaf face that happens to abut
        // another leaf reads far darker than one facing air, and a canopy
        // breaks into hard self-shadowed blotches instead of glowing evenly.
        if (lights_as_translucent(face.block))
        {
            sun_level = std::max(sun_level,
                                 static_cast<unsigned>(light.sun_at(face.x, face.y, face.z)) * 17U);
            torch_level = std::max(
                torch_level, static_cast<unsigned>(light.torch_at(face.x, face.y, face.z)) * 17U);
            lava_level = std::max(
                lava_level, static_cast<unsigned>(light.lava_at(face.x, face.y, face.z)) * 17U);
        }
        face.sky_light[corner_index] = static_cast<std::uint8_t>(std::min(sun_level, 255U));
        face.block_light[corner_index] = static_cast<std::uint8_t>(std::min(torch_level, 255U));
        face.lava_light[corner_index] = static_cast<std::uint8_t>(std::min(lava_level, 255U));
    }
    fill_face_indirect_light(face, region, light, normal, tangent_axes);
    fill_face_block_light_lobe(face, region, light, normal);
    face.emission = emits_light(face.block) ? 255U : 0U;
}

void selection_bounds(const WorldRegion &region, const std::vector<ChunkCoordinate> &selection,
                      int &minimum_x, int &minimum_z, int &maximum_x, int &maximum_z, bool &found)
{
    minimum_x = region.maximum_x();
    minimum_z = region.maximum_z();
    maximum_x = region.minimum_x();
    maximum_z = region.minimum_z();
    found = false;
    for (const ChunkCoordinate coordinate : selection)
    {
        const int chunk_minimum_x = coordinate.x * chunk_width;
        const int chunk_minimum_z = coordinate.z * chunk_depth;
        if (chunk_minimum_x > region.maximum_x() ||
            chunk_minimum_x + chunk_width - 1 < region.minimum_x() ||
            chunk_minimum_z > region.maximum_z() ||
            chunk_minimum_z + chunk_depth - 1 < region.minimum_z())
        {
            continue;
        }
        minimum_x = std::min(minimum_x, chunk_minimum_x);
        minimum_z = std::min(minimum_z, chunk_minimum_z);
        maximum_x = std::max(maximum_x, chunk_minimum_x + chunk_width - 1);
        maximum_z = std::max(maximum_z, chunk_minimum_z + chunk_depth - 1);
        found = true;
    }
}

std::vector<WorldFace> build_chunk_faces(const WorldRegion &region, const Chunk &chunk,
                                         const LightMap &light, int section_y,
                                         const LiquidTable *liquids)
{
    std::vector<WorldFace> result;
    const int base_x = chunk.chunk_x() * chunk_width;
    const int base_z = chunk.chunk_z() * chunk_depth;
    const int minimum_y = section_y * chunk_section_height;
    const int maximum_y = std::min(minimum_y + chunk_section_height, chunk_height);
    for (int y = minimum_y; y < maximum_y; ++y)
    {
        const int world_y = y + world_min_y;
        for (int z = 0; z < chunk_depth; ++z)
        {
            for (int x = 0; x < chunk_width; ++x)
            {
                const BlockType type = chunk.block(x, y, z);
                if (!is_solid(type) || !renders_as_voxel(type))
                {
                    continue;
                }
                const int world_x = base_x + x;
                const int world_z = base_z + z;
                // (mod-liquid) A liquid shows its faces only toward what is
                // neither liquid nor opaque; the top cell of its column sits
                // at the lowered surface.
                const bool liquid =
                    liquids != nullptr && liquids->liquid(region, world_x, world_y, world_z);
                if (liquid)
                {
                    const std::uint8_t mark =
                        liquids->liquid(region, world_x, world_y + 1, world_z) ? 1U : 2U;
                    for (std::size_t direction_index = 0; direction_index < face_offsets.size();
                         ++direction_index)
                    {
                        const auto &offset = face_offsets[direction_index];
                        const int nx = world_x + offset[0];
                        const int ny = world_y + offset[1];
                        const int nz = world_z + offset[2];
                        if (!region.contains(nx, ny, nz) || occludes(region.block(nx, ny, nz)) ||
                            liquids->liquid(region, nx, ny, nz))
                            continue;
                        const auto direction = static_cast<FaceDirection>(direction_index);
                        WorldFace face{world_x,   world_y, world_z,
                                       direction, type,    atlas_tile(type, direction)};
                        fill_face_lighting(face, region, light);
                        face.liquid = mark;
                        result.push_back(face);
                    }
                    continue;
                }
                for (std::size_t direction_index = 0; direction_index < face_offsets.size();
                     ++direction_index)
                {
                    const auto &offset = face_offsets[direction_index];
                    if (!region.contains(world_x + offset[0], world_y + offset[1],
                                         world_z + offset[2]))
                    {
                        // Unloaded neighbours are sealed until their chunk batch arrives.
                        continue;
                    }
                    const BlockType neighbor =
                        region.block(world_x + offset[0], world_y + offset[1], world_z + offset[2]);
                    // RenderChunk compares the neighboring block type before
                    // emitting translucent geometry. Leaves must pass light,
                    // but two adjacent leaf cells never draw their shared
                    // interior faces; stacking those hidden sheets makes a
                    // crown read as an opaque black-green voxel mass.
                    // (mod-liquid) Not toward a liquid holding the same block:
                    // that face shows through it.
                    if (neighbor == type &&
                        (liquids == nullptr || !liquids->liquid(region, world_x + offset[0],
                                                                world_y + offset[1],
                                                                world_z + offset[2])))
                        continue;
                    if (!occludes(neighbor) || neighbor != type && !occludes(type) ||
                        neighbor == type)
                    {
                        const auto direction = static_cast<FaceDirection>(direction_index);
                        WorldFace face{world_x,   world_y, world_z,
                                       direction, type,    atlas_tile(type, direction)};
                        fill_face_lighting(face, region, light);
                        result.push_back(face);
                    }
                }
            }
        }
    }
    return result;
}

} // namespace

// BlockTerrain.GetSimpleSunlightAtPoint: 0 outside the loaded volume, 1 in an
// uninitialised cell, -1 inside an opaque block, otherwise the cell's four-bit
// sunlight level / 15 (the original's stored light, flooded 15 cells from open
// sky). DragonBaseState.FindBestTarget scores players by it.
float simple_sunlight_at(const WorldRegion &region, int x, int y, int z)
{
    if (!region.contains(x, y, z))
        return 0.0F;
    const BlockType block = region.block(x, y, z);
    if (block == BlockType::uninitialized)
        return 1.0F;
    if (occludes(block))
        return -1.0F;
    const LightMap map = build_light_map(region, x, z, x, z, direct_light_reach + 1);
    return static_cast<float>(map.sun_at(x, y, z)) / 15.0F;
}

float local_sunlight_at(const WorldRegion &region, int x, int y, int z)
{
    if (!region.contains(x, y, z))
        return 0.0F;
    const BlockType block = region.block(x, y, z);
    if (block == BlockType::uninitialized)
        return 1.0F;
    if (occludes(block))
        return -1.0F;
    constexpr int top = world_min_y + chunk_height - 1;
    // Fourteen steps is as far as any sunlight carries.
    constexpr int reach = 14;
    constexpr int side = 2 * reach + 1;
    // The lowest cell of a column that the sky scan lights at 15: everything
    // above it lets all the light through. Columns are scanned once, on need.
    constexpr int unscanned = std::numeric_limits<int>::min();
    std::array<int, static_cast<std::size_t>(side) * side> floors;
    floors.fill(unscanned);
    const auto sky_floor = [&](int cx, int cz) {
        int &floor = floors[static_cast<std::size_t>(cz - z + reach) * side +
                            static_cast<std::size_t>(cx - x + reach)];
        if (floor == unscanned)
        {
            int cy = top;
            while (cy >= world_min_y && light_transmission(region.block(cx, cy, cz)) == 16U)
                --cy;
            floor = cy + 1;
        }
        return floor;
    };
    if (y >= sky_floor(x, z))
        return 1.0F;
    // Breadth-first outwards through clear cells: the first one under open
    // sky, `steps` away, delivers 15 - steps.
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(side) * side * side, 0U);
    const auto mark = [&](int cx, int cy, int cz) {
        std::uint8_t &cell = seen[(static_cast<std::size_t>(cz - z + reach) * side +
                                   static_cast<std::size_t>(cx - x + reach)) *
                                      side +
                                  static_cast<std::size_t>(cy - y + reach)];
        return std::exchange(cell, std::uint8_t{1U}) == 0U;
    };
    std::vector<std::array<int, 3>> frontier{{x, y, z}};
    std::vector<std::array<int, 3>> next;
    static_cast<void>(mark(x, y, z));
    for (int steps = 1; steps <= reach && !frontier.empty(); ++steps)
    {
        next.clear();
        for (const std::array<int, 3> &cell : frontier)
            for (const auto &offset : std::array<std::array<int, 3>, 6>{
                     {{{1, 0, 0}}, {{-1, 0, 0}}, {{0, 1, 0}}, {{0, -1, 0}}, {{0, 0, 1}}, {{0, 0, -1}}}})
            {
                const int nx = cell[0] + offset[0];
                const int ny = cell[1] + offset[1];
                const int nz = cell[2] + offset[2];
                if (ny < world_min_y || ny > top || !region.contains(nx, ny, nz) || !mark(nx, ny, nz) ||
                    light_transmission(region.block(nx, ny, nz)) != 16U)
                    continue;
                if (ny >= sky_floor(nx, nz))
                    return static_cast<float>(15 - steps) / 15.0F;
                next.push_back({nx, ny, nz});
            }
        frontier.swap(next);
    }
    return 0.0F;
}

SimpleLight simple_light_at(const WorldRegion &region, int x, int y, int z)
{
    if (!region.contains(x, y, z))
        return {};
    const BlockType block = region.block(x, y, z);
    if (block == BlockType::uninitialized)
        return {1.0F, 0.0F};
    if (occludes(block))
        return {-1.0F, -1.0F};
    const LightMap map = build_light_map(region, x, z, x, z, direct_light_reach + 1);
    // The original has one "torch" channel for every emitter, lava included;
    // the bake here keeps lava's apart only to colour it.
    return {static_cast<float>(map.sun_at(x, y, z)) / 15.0F,
            static_cast<float>(std::max(map.torch_at(x, y, z), map.lava_at(x, y, z))) / 15.0F};
}

std::vector<WorldFace> WorldRegion::visible_faces() const
{
    auto batches = visible_faces_by_chunk();
    std::size_t face_count{};
    for (const auto &batch : batches)
    {
        face_count += batch.faces.size();
    }
    std::vector<WorldFace> result;
    result.reserve(face_count);
    for (auto &batch : batches)
    {
        result.insert(result.end(), std::make_move_iterator(batch.faces.begin()),
                      std::make_move_iterator(batch.faces.end()));
    }
    return result;
}

std::vector<ChunkFaceBatch> WorldRegion::visible_faces_by_chunk() const
{
    std::vector<ChunkCoordinate> selection;
    selection.reserve(chunks_.size() * static_cast<std::size_t>(chunk_section_count));
    for (const Chunk &chunk : chunks_)
    {
        for (int section_y = 0; section_y < chunk_section_count; ++section_y)
            selection.push_back({chunk.chunk_x(), chunk.chunk_z(), section_y});
    }
    return visible_faces_by_chunk(selection);
}

std::vector<ChunkFaceBatch> WorldRegion::visible_faces_by_chunk(
    const std::vector<ChunkCoordinate> &selection, const LiquidTable *liquids) const
{
    if (liquids != nullptr && liquids->empty())
        liquids = nullptr;
    int minimum_x{};
    int minimum_z{};
    int maximum_x{};
    int maximum_z{};
    bool found{};
    selection_bounds(*this, selection, minimum_x, minimum_z, maximum_x, maximum_z, found);
    if (!found)
    {
        return {};
    }
    const LightMap light = build_light_map(*this, minimum_x, minimum_z, maximum_x, maximum_z);
    struct MeshTask final
    {
        ChunkCoordinate coordinate;
        const Chunk *chunk{};
    };
    std::vector<MeshTask> tasks;
    tasks.reserve(selection.size());
    for (const ChunkCoordinate coordinate : selection)
    {
        const int region_x = coordinate.x - origin_chunk_x_;
        const int region_z = coordinate.z - origin_chunk_z_;
        if (region_x < 0 || region_x >= chunks_x_ || region_z < 0 || region_z >= chunks_z_ ||
            coordinate.y < 0 || coordinate.y >= chunk_section_count)
        {
            continue;
        }
        const Chunk &chunk = chunks_[static_cast<std::size_t>(region_x + region_z * chunks_x_)];
        tasks.push_back({coordinate, &chunk});
    }
    std::vector<ChunkFaceBatch> result(tasks.size());
    const auto build_range = [&](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index)
        {
            const MeshTask &task = tasks[index];
            result[index] = {task.coordinate,
                             build_chunk_faces(*this, *task.chunk, light, task.coordinate.y,
                                               liquids)};
        }
    };

    // A complete view-distance window contains thousands of independent
    // 16-high sections. Their immutable region snapshot and shared LightMap are
    // safe to read concurrently, and parallelizing them hides the extra probe
    // work during restart/preload. Small edit selections stay serial so a single
    // block change never pays thread-launch overhead.
    // Preload admits sixteen sections per batch, so that exact batch size must
    // enter the parallel path. Cap fan-out to avoid launching one thread per
    // section during the hundreds of streamed batches in a far view window.
    constexpr std::size_t parallel_threshold = 16U;
    constexpr std::size_t maximum_workers = 8U;
    const unsigned int hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t worker_count =
        tasks.size() < parallel_threshold
            ? 1U
            : std::min({static_cast<std::size_t>(hardware_threads), maximum_workers, tasks.size()});
    if (worker_count == 1U)
    {
        build_range(0U, tasks.size());
    }
    else
    {
        const std::size_t batch_size = (tasks.size() + worker_count - 1U) / worker_count;
        std::vector<std::future<void>> workers;
        workers.reserve(worker_count - 1U);
        for (std::size_t worker = 1U; worker < worker_count; ++worker)
        {
            const std::size_t begin = worker * batch_size;
            const std::size_t end = std::min(begin + batch_size, tasks.size());
            if (begin >= end)
                break;
            workers.push_back(
                std::async(std::launch::async, [&, begin, end] { build_range(begin, end); }));
        }
        build_range(0U, std::min(batch_size, tasks.size()));
        for (auto &worker : workers)
            worker.get();
    }
    return result;
}

namespace
{

void sort_unique_sections(std::vector<ChunkCoordinate> &sections)
{
    const auto order = [](const ChunkCoordinate &left, const ChunkCoordinate &right) {
        if (left.z != right.z)
            return left.z < right.z;
        if (left.x != right.x)
            return left.x < right.x;
        return left.y < right.y;
    };
    std::sort(sections.begin(), sections.end(), order);
    sections.erase(std::unique(sections.begin(), sections.end()), sections.end());
}

// Sunlight is seeded by a straight downward column scan, so an edit in a column
// that is open to the sky changes the sun value of every cell beneath it, however
// far down. Anything else is bounded by light_influence_radius.
[[nodiscard]] bool column_open_to_sky(const WorldRegion &region, int x, int y, int z) noexcept
{
    for (int above = y + 1; above < world_min_y + chunk_height; ++above)
    {
        if (!region.contains(x, above, z))
            return true;
        if (light_transmission(region.block(x, above, z)) != 16U)
            return false;
    }
    return true;
}

// The lowest cell whose direct sunlight the column scan can report differently
// after an edit at (x, y, z) in a column that is open to the sky. Removing the
// cell drops the sky floor to the first block below that is not fully
// transmissive; placing one lifts it back from the same place. Cells that this
// batch also edits count as open, so the answer does not depend on whether the
// caller has applied the edits yet.
[[nodiscard]] int sky_exposed_bottom(const WorldRegion &region, int x, int y, int z,
                                     const std::vector<std::array<int, 3>> &edits) noexcept
{
    const auto edited = [&](int cell_y) {
        for (const std::array<int, 3> &edit : edits)
            if (edit[0] == x && edit[2] == z && edit[1] == cell_y)
                return true;
        return false;
    };
    int lowest = y;
    for (int below = y - 1; below >= world_min_y; --below)
    {
        if (!region.contains(x, below, z))
            break;
        lowest = below;
        // The sky scan seeds the first partly transmissive block it meets, so
        // that cell's own value can change; nothing under it can.
        if (!edited(below) && light_transmission(region.block(x, below, z)) != 16U)
            break;
    }
    return lowest;
}

} // namespace

std::vector<ChunkCoordinate> WorldRegion::edited_geometry_sections(
    const std::vector<std::array<int, 3>> &edits)
{
    // The cell's own section, plus the neighbour across each face: a face is
    // culled against the block on the other side, so a border edit changes the
    // adjacent section's geometry too.
    std::vector<ChunkCoordinate> sections;
    sections.reserve(edits.size() * 7U);
    for (const auto &edit : edits)
    {
        for (const std::array<int, 3> &offset :
             {std::array<int, 3>{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1},
              {0, 0, -1}})
        {
            const int local_y = edit[1] + offset[1] - world_min_y;
            if (local_y < 0 || local_y >= chunk_height)
                continue;
            sections.push_back({floor_divide(edit[0] + offset[0], chunk_width),
                                floor_divide(edit[2] + offset[2], chunk_depth),
                                local_y / chunk_section_height});
        }
    }
    sort_unique_sections(sections);
    return sections;
}

std::vector<ChunkCoordinate> WorldRegion::edited_sections(
    const std::vector<std::array<int, 3>> &edits) const
{
    std::vector<ChunkCoordinate> sections;
    sections.reserve(edits.size() * 64U);
    for (const auto &edit : edits)
    {
        const int minimum_chunk_x = floor_divide(edit[0] - light_influence_radius, chunk_width);
        const int maximum_chunk_x = floor_divide(edit[0] + light_influence_radius, chunk_width);
        const int minimum_chunk_z = floor_divide(edit[2] - light_influence_radius, chunk_depth);
        const int maximum_chunk_z = floor_divide(edit[2] + light_influence_radius, chunk_depth);
        const int local_y = edit[1] - world_min_y;
        // The vertical band is the same radius, not "everything underneath":
        // only an open sky column reaches further down, and even there only as
        // far as the newly exposed (or newly shadowed) part of the column, plus
        // the usual influence radius under its lowest cell.
        const int lowest_cell =
            column_open_to_sky(*this, edit[0], edit[1], edit[2])
                ? std::clamp(sky_exposed_bottom(*this, edit[0], edit[1], edit[2], edits) -
                                 world_min_y - light_influence_radius,
                             0, chunk_height - 1)
                : std::clamp(local_y - light_influence_radius, 0, chunk_height - 1);
        const int highest_cell =
            std::clamp(local_y + light_influence_radius, 0, chunk_height - 1);
        const int lowest_section = lowest_cell / chunk_section_height;
        const int highest_section = highest_cell / chunk_section_height;
        for (int chunk_z = minimum_chunk_z; chunk_z <= maximum_chunk_z; ++chunk_z)
            for (int chunk_x = minimum_chunk_x; chunk_x <= maximum_chunk_x; ++chunk_x)
                for (int section_y = lowest_section; section_y <= highest_section; ++section_y)
                    sections.push_back({chunk_x, chunk_z, section_y});
    }
    sort_unique_sections(sections);
    return sections;
}

std::vector<ChunkCoordinate> WorldRegion::light_changed_sections(
    const std::vector<std::array<int, 3>> &edits,
    const std::vector<BlockType> *previous_blocks) const
{
    if (edits.empty())
        return {};
    const bool exact = previous_blocks != nullptr && previous_blocks->size() == edits.size();
    int minimum_x = edits.front()[0];
    int maximum_x = edits.front()[0];
    int minimum_z = edits.front()[2];
    int maximum_z = edits.front()[2];
    for (const std::array<int, 3> &edit : edits)
    {
        minimum_x = std::min(minimum_x, edit[0]);
        maximum_x = std::max(maximum_x, edit[0]);
        minimum_z = std::min(minimum_z, edit[2]);
        maximum_z = std::max(maximum_z, edit[2]);
    }
    // Scattered edits would need a probe window wider than the apron it is
    // meant to shrink, so two floods over it would cost more than the bakes
    // they save. Those keep the geometric rule.
    constexpr int maximum_probe_span = 32;
    if (maximum_x - minimum_x > maximum_probe_span || maximum_z - minimum_z > maximum_probe_span)
        return edited_sections(edits);

    // With the previous block known, flooding the window once per state gives
    // the exact set of cells whose light value moved. Without it, flood light is
    // monotone in both transmission and self-illumination, so a window computed
    // with every edited cell fully open and emitting, against one with every
    // cell sealed and dark, brackets both states: wherever those two agree
    // neither state can differ. The bracket needs no record of the past but has
    // to assume an emitter may have been removed, so it reports a wider set.
    WorldRegion high = *this;
    WorldRegion low = *this;
    for (std::size_t index = 0U; index < edits.size(); ++index)
    {
        const std::array<int, 3> &edit = edits[index];
        if (!contains(edit[0], edit[1], edit[2]))
            continue;
        if (exact)
            low.set_block(edit[0], edit[1], edit[2], (*previous_blocks)[index]);
        else
        {
            high.set_block(edit[0], edit[1], edit[2], BlockType::empty);
            low.set_block(edit[0], edit[1], edit[2], BlockType::bedrock);
        }
    }
    // One cell past the flood reach is enough: a changed value is at most
    // direct_light_reach steps from an edit, and cutting paths at the window
    // edge affects both floods identically, so it can only over-report.
    constexpr int probe_margin = direct_light_reach + 1;
    const LightMap bright = build_light_map(high, minimum_x, minimum_z, maximum_x, maximum_z,
                                            probe_margin, exact ? nullptr : &edits);
    const LightMap dark = build_light_map(low, minimum_x, minimum_z, maximum_x, maximum_z,
                                          probe_margin, nullptr);
    if (bright.width <= 0 || bright.width != dark.width || bright.depth != dark.depth ||
        bright.minimum_x != dark.minimum_x || bright.minimum_z != dark.minimum_z)
        return edited_sections(edits);

    // A changed cell can only alter the bake of a section within the probe
    // reach of it, so mark sections on a small flat grid around the window.
    constexpr int grid_reach = probe_margin + indirect_surface_apron;
    const int first_section_x = floor_divide(minimum_x - grid_reach, chunk_width);
    const int last_section_x = floor_divide(maximum_x + grid_reach, chunk_width);
    const int first_section_z = floor_divide(minimum_z - grid_reach, chunk_depth);
    const int last_section_z = floor_divide(maximum_z + grid_reach, chunk_depth);
    const int grid_width = last_section_x - first_section_x + 1;
    const int grid_depth = last_section_z - first_section_z + 1;
    std::vector<char> dirty(static_cast<std::size_t>(grid_width) *
                                static_cast<std::size_t>(grid_depth) *
                                static_cast<std::size_t>(chunk_section_count),
                            0);
    const auto mark = [&](int x, int y, int z) {
        const int local_y = y - world_min_y;
        const int lowest_section =
            std::clamp(local_y - indirect_surface_apron, 0, chunk_height - 1) /
            chunk_section_height;
        const int highest_section =
            std::clamp(local_y + indirect_surface_apron, 0, chunk_height - 1) /
            chunk_section_height;
        const int lowest_x =
            std::clamp(floor_divide(x - indirect_surface_apron, chunk_width), first_section_x,
                       last_section_x);
        const int highest_x =
            std::clamp(floor_divide(x + indirect_surface_apron, chunk_width), first_section_x,
                       last_section_x);
        const int lowest_z =
            std::clamp(floor_divide(z - indirect_surface_apron, chunk_depth), first_section_z,
                       last_section_z);
        const int highest_z =
            std::clamp(floor_divide(z + indirect_surface_apron, chunk_depth), first_section_z,
                       last_section_z);
        for (int section_z = lowest_z; section_z <= highest_z; ++section_z)
            for (int section_x = lowest_x; section_x <= highest_x; ++section_x)
                for (int section_y = lowest_section; section_y <= highest_section; ++section_y)
                    dirty[((static_cast<std::size_t>(section_z - first_section_z) *
                                static_cast<std::size_t>(grid_width) +
                            static_cast<std::size_t>(section_x - first_section_x)) *
                           static_cast<std::size_t>(chunk_section_count)) +
                          static_cast<std::size_t>(section_y)] = 1;
    };
    // The edited cells' own block content changed, which moves face culling,
    // ambient occlusion and every probe ray that passes through them.
    for (const std::array<int, 3> &edit : edits)
        mark(edit[0], edit[1], edit[2]);
    for (int z = bright.minimum_z; z < bright.minimum_z + bright.depth; ++z)
    {
        for (int x = bright.minimum_x; x < bright.minimum_x + bright.width; ++x)
        {
            const std::size_t column = bright.cell(x, world_min_y, z);
            for (int local_y = 0; local_y < chunk_height; ++local_y)
            {
                const std::size_t cell = column + static_cast<std::size_t>(local_y);
                if (bright.sun[cell] == dark.sun[cell] &&
                    bright.torch[cell] == dark.torch[cell] &&
                    bright.lava[cell] == dark.lava[cell])
                    continue;
                mark(x, world_min_y + local_y, z);
            }
        }
    }

    std::vector<ChunkCoordinate> sections;
    for (int section_z = first_section_z; section_z <= last_section_z; ++section_z)
    {
        if (section_z < origin_chunk_z_ || section_z >= origin_chunk_z_ + chunks_z_)
            continue;
        for (int section_x = first_section_x; section_x <= last_section_x; ++section_x)
        {
            if (section_x < origin_chunk_x_ || section_x >= origin_chunk_x_ + chunks_x_)
                continue;
            for (int section_y = 0; section_y < chunk_section_count; ++section_y)
            {
                if (dirty[((static_cast<std::size_t>(section_z - first_section_z) *
                                static_cast<std::size_t>(grid_width) +
                            static_cast<std::size_t>(section_x - first_section_x)) *
                           static_cast<std::size_t>(chunk_section_count)) +
                          static_cast<std::size_t>(section_y)] != 0)
                    sections.push_back({section_x, section_z, section_y});
            }
        }
    }
    sort_unique_sections(sections);
    return sections;
}

std::uint64_t WorldRegion::hash() const noexcept
{
    std::uint64_t result = 1469598103934665603ULL;
    for (const Chunk &chunk : chunks_)
    {
        result ^= chunk.hash();
        result *= 1099511628211ULL;
    }
    return result;
}

WorldGenerator::WorldGenerator(std::int32_t seed, TerrainProfile profile, GameMode game_mode)
    : seed_(seed), profile_(profile), game_mode_(game_mode), noise_(seed), integer_noise_(1),
      slime_noise_(1)
{
}

std::int32_t WorldGenerator::seed() const noexcept
{
    return seed_;
}

TerrainProfile WorldGenerator::profile() const noexcept
{
    return profile_;
}

GameMode WorldGenerator::game_mode() const noexcept
{
    return game_mode_;
}

void WorldGenerator::set_custom_terrain(std::shared_ptr<const CustomTerrain> terrain) noexcept
{
    custom_ = std::move(terrain);
}

BiomeKind WorldGenerator::biome_at(int world_x, int world_z) const noexcept
{
    if (custom_ != nullptr) // mod-world
        return custom_->retail_kind_at(*this, world_x, world_z);
    return retail_biome_at(world_x, world_z);
}

BiomeKind WorldGenerator::retail_biome_at(int world_x, int world_z) const noexcept
{
    const double radius = folded_radius(world_x, world_z);
    if (radius < 300.0)
        return BiomeKind::classic;
    if (radius < 1000.0)
        return BiomeKind::lagoon;
    if (radius < 1700.0)
        return BiomeKind::desert;
    if (radius < 2400.0)
        return BiomeKind::mountain;
    if (radius < 3000.0)
        return BiomeKind::arctic;
    return BiomeKind::descent;
}

std::optional<std::array<int, 2>> WorldGenerator::find_crash_site_column(
    int center_x, int center_z, int max_distance) const noexcept
{
    // The crash field has a ~213-block base period, so a four-block lattice is
    // substantially finer than a possible positive-rim island. Square rings
    // make the result stable and near the requested centre.
    constexpr int step = 4;
    const auto candidate = [&](int world_x, int world_z) {
        const float dx = static_cast<float>(world_x - center_x);
        const float dz = static_cast<float>(world_z - center_z);
        if (dx * dx + dz * dz > static_cast<float>(max_distance * max_distance))
            return false;
        const double radius = folded_radius(world_x, world_z);
        if (radius <= 300.0 || radius >= 3600.0)
            return false;
        // The same arithmetic the crash-site pass in deposits() uses.
        const float crash_noise = noise_.noise(scaled_argument(world_x, 0.0046875F),
                                               scaled_argument(world_z, 0.0046875F));
        if (!(crash_noise > 0.55F))
            return false;
        const int depth = static_cast<int>(
            ((static_cast<double>(crash_noise) - static_cast<double>(0.5F)) * 7.0) * 20.0);
        const int rim = std::min(
            depth, static_cast<int>(((static_cast<double>(crash_noise) -
                                      static_cast<double>(0.55F)) *
                                     10.0) *
                                    20.0));
        return rim > 0;
    };
    for (int ring = 0; ring <= max_distance; ring += step)
    {
        for (int offset = -ring; offset <= ring; offset += step)
        {
            const int top_x = center_x + offset;
            const int top_z = center_z - ring;
            if (candidate(top_x, top_z))
                return std::array{top_x, top_z};
            if (ring != 0)
            {
                const int bottom_z = center_z + ring;
                if (candidate(top_x, bottom_z))
                    return std::array{top_x, bottom_z};
            }
        }
        for (int offset = -ring + step; offset <= ring - step; offset += step)
        {
            const int side_z = center_z + offset;
            const int left_x = center_x - ring;
            if (candidate(left_x, side_z))
                return std::array{left_x, side_z};
            if (ring != 0)
            {
                const int right_x = center_x + ring;
                if (candidate(right_x, side_z))
                    return std::array{right_x, side_z};
            }
        }
    }
    return std::nullopt;
}

namespace
{

// One column of CastleMinerZBuilder.BuildWorldChunk: the surface pass of the
// band the column stands in, then every deposit pass. `Target` is the chunk
// being generated, or a ColumnBuffer for a far-terrain summary.
template <typename Target>
void build_column(Target &chunk, const PerlinNoise &noise, const IntegerNoise &integer_noise,
                  const IntegerNoise &slime_noise, TerrainProfile profile, GameMode game_mode,
                  int local_x, int local_z, int world_x, int world_z, bool surface_only = false,
                  bool origin_tower = true, int *settled_floor = nullptr)
{
    // Each transition's blend is `(radius - start) / width` held on the FPU
    // stack; the two column builders then receive `1 - blend` and `blend` as
    // SEPARATE float stores of 53-bit values, so the first is not the float
    // complement of the second.
    const double radius = folded_radius(world_x, world_z);
    float hell_blend = 1.0F;
    if (radius < 200.0)
    {
        classic_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
    }
    else if (radius < 300.0)
    {
        const double blend = (radius - 200.0) / 100.0;
        classic_column(chunk, noise, local_x, local_z, world_x, world_z,
                       static_cast<float>(1.0 - blend));
        lagoon_column(chunk, noise, local_x, local_z, world_x, world_z, static_cast<float>(blend));
    }
    else if (radius < 900.0)
    {
        lagoon_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
    }
    else if (radius < 1000.0)
    {
        const double blend = (radius - 900.0) / 100.0;
        lagoon_column(chunk, noise, local_x, local_z, world_x, world_z,
                      static_cast<float>(1.0 - blend));
        desert_column(chunk, noise, local_x, local_z, world_x, world_z);
    }
    else if (radius < 1600.0)
    {
        desert_column(chunk, noise, local_x, local_z, world_x, world_z);
    }
    else if (radius < 1700.0)
    {
        const double blend = (radius - 1600.0) / 100.0;
        desert_column(chunk, noise, local_x, local_z, world_x, world_z);
        mountain_column(chunk, noise, local_x, local_z, world_x, world_z,
                        static_cast<float>(blend));
    }
    else if (radius < 2300.0)
    {
        mountain_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
    }
    else if (radius < 2400.0)
    {
        const double blend = (radius - 2300.0) / 100.0;
        mountain_column(chunk, noise, local_x, local_z, world_x, world_z,
                        static_cast<float>(1.0 - blend));
        arctic_column(chunk, noise, local_x, local_z, world_x, world_z);
    }
    else if (radius < 3000.0)
    {
        arctic_column(chunk, noise, local_x, local_z, world_x, world_z);
    }
    else if (radius < 3600.0)
    {
        const double blend = (radius - 3000.0) / 600.0;
        hell_blend = static_cast<float>(1.0 - blend);
        descent_column(chunk, noise, local_x, local_z, world_x, world_z,
                       static_cast<float>(blend));
    }
    // MathHelper.Clamp(radius / 3600f, 0f, 1f): the quotient is narrowed as the
    // argument, and Clamp returns one of its float inputs.
    const float distance_blend = std::clamp(static_cast<float>(radius / 3600.0), 0.0F, 1.0F);
    deposits(chunk, noise, integer_noise, slime_noise, profile, game_mode, local_x, local_z,
             world_x, world_z, distance_blend, hell_blend, radius > 300.0 && radius < 3600.0,
             surface_only, origin_tower, settled_floor);
}

// mod-world: one retail biome at full strength wherever a mod layout puts it,
// with the deposit passes as they run in the middle of that biome's band.
template <typename Target>
void build_retail_biome_column(Target &chunk, const PerlinNoise &noise,
                               const IntegerNoise &integer_noise, const IntegerNoise &slime_noise,
                               TerrainProfile profile, GameMode game_mode, BiomeKind biome,
                               int local_x, int local_z, int world_x, int world_z)
{
    float hell_blend = 1.0F;
    double band_radius = 150.0;
    switch (biome)
    {
    case BiomeKind::classic:
        classic_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
        break;
    case BiomeKind::lagoon:
        lagoon_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
        band_radius = 600.0;
        break;
    case BiomeKind::desert:
        desert_column(chunk, noise, local_x, local_z, world_x, world_z);
        band_radius = 1300.0;
        break;
    case BiomeKind::mountain:
        mountain_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
        band_radius = 2000.0;
        break;
    case BiomeKind::arctic:
        arctic_column(chunk, noise, local_x, local_z, world_x, world_z);
        band_radius = 2700.0;
        break;
    case BiomeKind::descent:
        descent_column(chunk, noise, local_x, local_z, world_x, world_z, 0.5F);
        hell_blend = 0.5F;
        band_radius = 3300.0;
        break;
    }
    const float distance_blend = std::clamp(static_cast<float>(band_radius / 3600.0), 0.0F, 1.0F);
    deposits(chunk, noise, integer_noise, slime_noise, profile, game_mode, local_x, local_z,
             world_x, world_z, distance_blend, hell_blend, biome != BiomeKind::classic);
}

} // namespace

Chunk WorldGenerator::generate_chunk(int chunk_x, int chunk_z) const
{
    if (custom_ != nullptr) // mod-world
        return custom_->generate_chunk(*this, chunk_x, chunk_z);
    return retail_chunk(chunk_x, chunk_z);
}

Chunk WorldGenerator::retail_chunk(int chunk_x, int chunk_z) const
{
    Chunk chunk{chunk_x, chunk_z};
    const PerlinNoise &noise = noise_;
    const IntegerNoise &integer_noise = integer_noise_;
    for (int local_z = 0; local_z < chunk_depth; ++local_z)
    {
        // `i + minLoc.Z` in 32 bits, as the original computes it.
        const int world_z = static_cast<int>(static_cast<std::uint32_t>(chunk_z) *
                                                 static_cast<std::uint32_t>(chunk_depth) +
                                             static_cast<std::uint32_t>(local_z));
        for (int local_x = 0; local_x < chunk_width; ++local_x)
        {
            const int world_x = static_cast<int>(static_cast<std::uint32_t>(chunk_x) *
                                                     static_cast<std::uint32_t>(chunk_width) +
                                                 static_cast<std::uint32_t>(local_x));
            // CastleMinerZBuilder.BuildWorldChunk.
            build_column(chunk, noise, integer_noise, slime_noise_, profile_, game_mode_, local_x,
                         local_z, world_x, world_z);
        }
    }
    add_trees(chunk, noise);
    return chunk;
}

// --- Far-terrain summaries (presentation only; nothing simulated reads them).

void WorldGenerator::generate_column(int world_x, int world_z,
                                     std::span<BlockType, chunk_height> cells) const noexcept
{
    if (custom_ != nullptr) // mod-world
    {
        custom_->generate_column(*this, world_x, world_z, cells);
        return;
    }
    retail_column(world_x, world_z, cells);
}

void WorldGenerator::retail_column(int world_x, int world_z,
                                   std::span<BlockType, chunk_height> cells) const noexcept
{
    ColumnBuffer column;
    build_column(column, noise_, integer_noise_, slime_noise_, profile_, game_mode_, 0, 0, world_x,
                 world_z);
    std::copy(column.cells.begin(), column.cells.end(), cells.begin());
}

SurfaceColumn WorldGenerator::surface_column(int world_x, int world_z,
                                             bool origin_tower) const noexcept
{
    if (custom_ != nullptr) // mod-world
        return custom_->surface_column(*this, world_x, world_z, origin_tower);
    return retail_surface_column(world_x, world_z, origin_tower);
}

SurfaceColumn WorldGenerator::retail_surface_column(int world_x, int world_z,
                                                    bool origin_tower) const noexcept
{
    ColumnBuffer column;
    int settled_floor = chunk_height;
    build_column(column, noise_, integer_noise_, slime_noise_, profile_, game_mode_, 0, 0, world_x,
                 world_z, true, origin_tower, &settled_floor);
    const auto solid = [&](int y) {
        const BlockType type = column.cells[static_cast<std::size_t>(y)];
        return type != BlockType::empty && type != BlockType::uninitialized;
    };
    SurfaceColumn surface;
    int y = chunk_height - 1;
    while (y >= 0 && !solid(y))
        --y;
    if (y < 0)
        return surface;
    surface.top_local_y = static_cast<std::int16_t>(y);
    surface.top = column.cells[static_cast<std::size_t>(y)];
    // Under an overhang the cells down to `settled_floor` are the real ones:
    // the end of the top span, the air under it and the ground under that.
    // (The passes that run after the caves only ever fill cells.)
    while (y >= settled_floor && solid(y))
        --y;
    if (y < settled_floor)
        return surface;
    // The ground is the cell the scan settled on, or what a later pass (the
    // floor of Hell, bedrock) piled on top of it.
    const int under = y + 1;
    int ground = std::max(settled_floor, 0);
    while (ground + 1 < under && solid(ground + 1))
        ++ground;
    surface.under_local_y = static_cast<std::int16_t>(under);
    surface.lower_local_y = static_cast<std::int16_t>(ground);
    surface.lower = column.cells[static_cast<std::size_t>(ground)];
    return surface;
}

void WorldGenerator::tree_sites(int chunk_x, int chunk_z, std::vector<TreeSite> &sites,
                                bool top_grass_only) const
{
    if (custom_ != nullptr) // mod-world
    {
        custom_->tree_sites(*this, chunk_x, chunk_z, sites, top_grass_only);
        return;
    }
    retail_tree_sites(chunk_x, chunk_z, sites, top_grass_only);
}

void WorldGenerator::retail_tree_sites(int chunk_x, int chunk_z, std::vector<TreeSite> &sites,
                                       bool top_grass_only) const
{
    const int base_x = chunk_x * chunk_width;
    const int base_z = chunk_z * chunk_depth;
    for (int local_z = 3; local_z < 13; ++local_z)
    {
        for (int local_x = 3; local_x < 13; ++local_x)
        {
            const int world_x = base_x + local_x;
            const int world_z = base_z + local_z;
            // TreeDepositer.BuildColumn's placement test (see add_trees).
            const float tree_noise = noise_.noise(scaled_argument(world_x, 0.4375F),
                                                  scaled_argument(world_z, 0.4375F));
            if (!(tree_noise > 0.6F))
                continue;
            if (top_grass_only)
            {
                // Nearly every tree stands on the column's top block, and then
                // nothing can be in the trunk's way. Resolving only that block
                // skips the caves and ores of the whole column.
                const SurfaceColumn surface = retail_surface_column(world_x, world_z);
                if (surface.top != BlockType::grass || surface.top_local_y > 124 ||
                    surface.top_local_y <= 1)
                    continue;
                sites.push_back({world_x, world_z, surface.top_local_y,
                                 static_cast<std::int16_t>(
                                     5 + static_cast<int>((static_cast<double>(tree_noise) -
                                                           static_cast<double>(0.6F)) *
                                                          9.0))});
                continue;
            }
            // The tree stands on the highest grass of the finished column, and
            // its trunk stops under the first block in its way.
            ColumnBuffer column;
            build_column(column, noise_, integer_noise_, slime_noise_, profile_, game_mode_, 0, 0,
                         world_x, world_z);
            int ground = 124;
            for (; ground > 0; --ground)
                if (column.cells[static_cast<std::size_t>(ground)] == BlockType::grass)
                    break;
            if (ground <= 1)
                continue;
            int height = 5 + static_cast<int>((static_cast<double>(tree_noise) -
                                               static_cast<double>(0.6F)) *
                                              9.0);
            for (int offset = 0; offset < height; ++offset)
            {
                const BlockType current = column.block(0, ground + 1 + offset, 0);
                if (current != BlockType::empty && current != BlockType::uninitialized)
                {
                    height = offset;
                    break;
                }
            }
            sites.push_back({world_x, world_z, static_cast<std::int16_t>(ground),
                             static_cast<std::int16_t>(height)});
        }
    }
}

// --- mod-world: the retail passes a mod layout reuses.

void WorldGenerator::retail_build_column(Chunk &chunk, int local_x, int local_z) const
{
    // `i + minLoc.Z` in 32 bits, as generate_chunk computes it.
    const int world_x = static_cast<int>(static_cast<std::uint32_t>(chunk.chunk_x()) *
                                             static_cast<std::uint32_t>(chunk_width) +
                                         static_cast<std::uint32_t>(local_x));
    const int world_z = static_cast<int>(static_cast<std::uint32_t>(chunk.chunk_z()) *
                                             static_cast<std::uint32_t>(chunk_depth) +
                                         static_cast<std::uint32_t>(local_z));
    build_column(chunk, noise_, integer_noise_, slime_noise_, profile_, game_mode_, local_x,
                 local_z, world_x, world_z);
}

void WorldGenerator::retail_build_biome_column(Chunk &chunk, BiomeKind biome, int local_x,
                                               int local_z) const
{
    build_retail_biome_column(chunk, noise_, integer_noise_, slime_noise_, profile_, game_mode_,
                              biome, local_x, local_z, chunk.chunk_x() * chunk_width + local_x,
                              chunk.chunk_z() * chunk_depth + local_z);
}

void WorldGenerator::retail_biome_column(BiomeKind biome, int world_x, int world_z,
                                         std::span<BlockType, chunk_height> cells) const noexcept
{
    ColumnBuffer column;
    build_retail_biome_column(column, noise_, integer_noise_, slime_noise_, profile_, game_mode_,
                              biome, 0, 0, world_x, world_z);
    std::copy(column.cells.begin(), column.cells.end(), cells.begin());
}

void WorldGenerator::retail_trees(Chunk &chunk, bool (*allowed)(const void *, int, int),
                                  const void *context) const
{
    add_trees(chunk, noise_, allowed, context);
}

WorldRegion WorldGenerator::generate_region(int origin_chunk_x, int origin_chunk_z, int chunks_x,
                                            int chunks_z) const
{
    WorldRegion region{origin_chunk_x, origin_chunk_z, chunks_x, chunks_z};
    for (Chunk &chunk : region.chunks())
    {
        chunk = generate_chunk(chunk.chunk_x(), chunk.chunk_z());
    }
    return region;
}

WorldRegion WorldGenerator::stream_region(const WorldRegion &current, int origin_chunk_x,
                                          int origin_chunk_z, int chunks_x, int chunks_z) const
{
    if (chunks_x <= 0)
    {
        chunks_x = current.chunks_x();
    }
    if (chunks_z <= 0)
    {
        chunks_z = current.chunks_z();
    }
    if (chunks_x <= 0 || chunks_z <= 0)
    {
        throw std::invalid_argument{"world region dimensions must be positive"};
    }
    // Which columns the current window cannot supply.
    struct Missing final
    {
        int chunk_x{};
        int chunk_z{};
    };
    const auto source_of = [&](int chunk_x, int chunk_z) -> const Chunk * {
        const int source_x = chunk_x - current.origin_chunk_x();
        const int source_z = chunk_z - current.origin_chunk_z();
        if (source_x < 0 || source_x >= current.chunks_x() || source_z < 0 ||
            source_z >= current.chunks_z())
            return nullptr;
        return &current
                    .chunks()[static_cast<std::size_t>(source_x + source_z * current.chunks_x())];
    };
    std::vector<Missing> missing;
    for (int z = 0; z < chunks_z; ++z)
        for (int x = 0; x < chunks_x; ++x)
            if (source_of(origin_chunk_x + x, origin_chunk_z + z) == nullptr)
                missing.push_back({origin_chunk_x + x, origin_chunk_z + z});

    std::vector<std::optional<Chunk>> generated(missing.size());
    const auto generate_range = [&](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index)
            generated[index].emplace(generate_chunk(missing[index].chunk_x, missing[index].chunk_z));
    };
    // One step of a walking window is a row or two of columns and stays on the
    // calling thread, so streaming never competes with the frame for more than
    // one core. A whole window (a teleport, the first load) is hundreds of
    // independent columns and is spread over the machine instead: the caller of
    // those is waiting for it.
    constexpr std::size_t parallel_threshold = 64U;
    constexpr std::size_t maximum_workers = 8U;
    const std::size_t hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t worker_count =
        missing.size() < parallel_threshold
            ? 1U
            : std::min({hardware_threads, maximum_workers, missing.size()});
    if (worker_count <= 1U)
    {
        generate_range(0U, missing.size());
    }
    else
    {
        const std::size_t batch_size = (missing.size() + worker_count - 1U) / worker_count;
        std::vector<std::future<void>> workers;
        workers.reserve(worker_count - 1U);
        for (std::size_t worker = 1U; worker < worker_count; ++worker)
        {
            const std::size_t begin = worker * batch_size;
            const std::size_t end = std::min(begin + batch_size, missing.size());
            if (begin >= end)
                break;
            workers.push_back(
                std::async(std::launch::async, [&, begin, end] { generate_range(begin, end); }));
        }
        generate_range(0U, std::min(batch_size, missing.size()));
        for (auto &worker : workers)
            worker.get();
    }

    std::vector<Chunk> chunks;
    chunks.reserve(static_cast<std::size_t>(chunks_x) * static_cast<std::size_t>(chunks_z));
    std::size_t next_generated{};
    for (int z = 0; z < chunks_z; ++z)
    {
        for (int x = 0; x < chunks_x; ++x)
        {
            if (const Chunk *source = source_of(origin_chunk_x + x, origin_chunk_z + z))
                chunks.push_back(*source);
            else
                chunks.push_back(std::move(*generated[next_generated++]));
        }
    }
    return WorldRegion{origin_chunk_x, origin_chunk_z, chunks_x, chunks_z, std::move(chunks)};
}

} // namespace openminerz::world
