#include "openminerz/world/world.hpp"

#include "openminerz/world/legacy_noise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <utility>

namespace openminerz::world
{
namespace
{

float lerp(float left, float right, float amount) noexcept
{
    return left + (right - left) * amount;
}

float folded_radius(int x, int z) noexcept
{
    float radius = std::sqrt(
        static_cast<float>(static_cast<std::int64_t>(x) * x + static_cast<std::int64_t>(z) * z));
    int bands{};
    while (radius > 4400.0F)
    {
        radius -= 4400.0F;
        ++bands;
    }
    return (bands & 1) != 0 ? 4400.0F - radius : radius;
}

float octave2(const PerlinNoise &noise, int x, int z, float scale, int octaves) noexcept
{
    int frequency = 1;
    float value{};
    for (int octave = 0; octave < octaves; ++octave)
    {
        value += noise.noise(scale * static_cast<float>(x * frequency),
                             scale * static_cast<float>(z * frequency)) /
                 static_cast<float>(frequency);
        frequency *= 2;
    }
    return value;
}

void classic_column(Chunk &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                    int world_z, float blender)
{
    const int amplitude = static_cast<int>(lerp(0.0F, 32.0F, blender));
    const int base = static_cast<int>(lerp(32.0F, 86.0F, blender));
    int height = base + static_cast<int>(octave2(noise, world_x, world_z, 0.009375F, 4) *
                                         static_cast<float>(amplitude));
    bool sandy{};
    if (height <= 66)
    {
        height = 66;
        sandy = true;
    }
    height = std::min(height, 127);
    const int material_jitter =
        static_cast<int>(noise.noise(world_x * 0.5F, world_z * 0.5F) * 4.0F);
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

void desert_column(Chunk &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                   int world_z)
{
    int height = 66 + static_cast<int>(octave2(noise, world_x, world_z, 0.009375F, 4) * 16.0F);
    height = std::max(height, 66);
    for (int y = 0; y <= height && y < chunk_height; ++y)
    {
        chunk.set_block(local_x, y, local_z, y <= height - 3 ? BlockType::rock : BlockType::sand);
    }
}

void arctic_column(Chunk &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                   int world_z)
{
    int height = 64 + static_cast<int>(octave2(noise, world_x, world_z, 0.009375F, 4) * 16.0F);
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

void mountain_column(Chunk &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                     int world_z, float blender)
{
    const int amplitude = static_cast<int>(lerp(0.0F, 32.0F, blender));
    const int base = static_cast<int>(lerp(32.0F, 86.0F, blender));
    int height = base + static_cast<int>(octave2(noise, world_x, world_z, 0.021875F, 4) *
                                         static_cast<float>(amplitude));
    const float ridge_noise = octave2(noise, world_x, world_z, 0.00625F, 6);
    float ridge = (ridge_noise + 1.0F) * 0.5F;
    ridge -= std::floor(ridge);
    ridge -= 0.5F;
    ridge = std::pow(2.0F, -ridge * ridge * 1000.0F);
    height -= static_cast<int>(ridge * static_cast<float>(amplitude));
    const bool low = height <= 65;
    height = std::max(height, 65);
    height += static_cast<int>(
        noise.noise(world_x * 0.00625F + 1000.0F, world_z * 0.00625F + 1000.0F) * blender * 10.0F);
    const int jitter = static_cast<int>(noise.noise(world_x * 0.5F, world_z * 0.5F) * 4.0F);
    const int snow_line = std::max(height - 4, 98) + jitter;
    height = std::min(height, 127);
    for (int y = 0; y <= height; ++y)
    {
        chunk.set_block(local_x, y, local_z,
                        low || y >= snow_line ? BlockType::snow : BlockType::rock);
    }
}

void lagoon_column(Chunk &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                   int world_z, float blender)
{
    int height = static_cast<int>(lerp(44.0F, 76.0F, blender)) +
                 static_cast<int>(octave2(noise, world_x, world_z, 0.009375F, 4) * 32.0F);
    bool sandy{};
    if (height <= 66)
    {
        sandy = true;
        height = 66;
    }
    const int jitter = static_cast<int>(noise.noise(world_x * 0.5F, world_z * 0.5F) * 4.0F);
    for (int y = 0; y < 100; ++y)
    {
        const float vx = world_x * 0.03125F + 1000.0F;
        const float vy = (y + world_min_y) * 0.046875F + 2000.0F;
        const float vz = world_z * 0.03125F + 1500.0F;
        const float value =
            noise.noise(vx, vy, vz) + noise.noise(vx * 2.0F, vy * 2.0F, vz * 2.0F) * 0.5F;
        if (value < -1.0F + 0.65F * blender)
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

void descent_column(Chunk &chunk, const PerlinNoise &noise, int local_x, int local_z, int world_x,
                    int world_z, float blender)
{
    const int height = static_cast<int>(lerp(64.0F, -16.0F, blender)) +
                       static_cast<int>(octave2(noise, world_x, world_z, 0.009375F, 4) * 16.0F);
    for (int y = 0; y < chunk_height; ++y)
    {
        if (y <= height)
        {
            chunk.set_block(local_x, y, local_z, BlockType::rock);
        }
        const float vx = world_x * 0.0625F;
        const float vy = (y + world_min_y) * 0.0625F;
        const float vz = world_z * 0.0625F;
        const float cave =
            noise.noise(vx, vy, vz) + noise.noise(vx * 2.0F, vy * 2.0F, vz * 2.0F) * 0.5F;
        if (cave < blender * 2.0F - 1.0F)
        {
            chunk.set_block(local_x, y, local_z, BlockType::empty);
        }
    }
}

void generate_modern_loot_block(Chunk &chunk, const IntegerNoise &integer_noise,
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

void deposits(Chunk &chunk, const PerlinNoise &noise, const IntegerNoise &integer_noise,
              const IntegerNoise &slime_noise, TerrainProfile profile, GameMode game_mode,
              int local_x, int local_z, int world_x, int world_z, float distance_blend,
              float hell_blend, bool deposit_crash_site)
{
    int frequency = 1;
    float ceiling_noise{};
    for (int octave = 0; octave < 4; ++octave)
    {
        ceiling_noise += noise.noise(0.03125F * world_x * frequency + 1000.0F,
                                     0.03125F * world_z * frequency + 1000.0F) /
                         static_cast<float>(frequency);
        frequency *= 2;
    }
    const int ceiling =
        static_cast<int>(32.0F * hell_blend) - static_cast<int>((ceiling_noise + 1.0F) * 4.0F);
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

    if (deposit_crash_site)
    {
        const float crash_noise = noise.noise(world_x * 0.0046875F, world_z * 0.0046875F);
        if (crash_noise > 0.5F)
        {
            const int depth = static_cast<int>((crash_noise - 0.5F) * 140.0F);
            int rim{};
            if (crash_noise > 0.55F)
            {
                rim = std::min(depth, static_cast<int>((crash_noise - 0.55F) * 200.0F));
            }
            const int slime_blend = static_cast<int>(distance_blend * 10.0F);
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

    for (int y = 0; y < chunk_height; ++y)
    {
        const BlockType current = chunk.block(local_x, y, local_z);
        if (current == BlockType::empty || current == BlockType::uninitialized ||
            current == BlockType::sand)
        {
            continue;
        }
        const float vx = world_x * 0.0625F;
        const float vy = (y + world_min_y) * 0.09375F;
        const float vz = world_z * 0.0625F;
        const float cave =
            noise.noise(vx, vy, vz) + noise.noise(vx * 2.0F, vy * 2.0F, vz * 2.0F) * 0.5F;
        if (cave < -0.35F)
        {
            chunk.set_block(local_x, y, local_z, BlockType::empty);
        }
    }

    const int ore_blend = static_cast<int>(distance_blend * 10.0F);
    for (int y = 0; y < chunk_height; ++y)
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

    frequency = 1;
    float floor_noise{};
    for (int octave = 0; octave < 4; ++octave)
    {
        floor_noise += noise.noise(0.03125F * world_x * frequency + 200.0F,
                                   0.03125F * world_z * frequency + 200.0F) /
                       static_cast<float>(frequency);
        frequency *= 2;
    }
    const int hell_height = 7 + static_cast<int>(floor_noise * 10.0F);
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

    const std::int64_t radius_squared =
        static_cast<std::int64_t>(world_x) * world_x + static_cast<std::int64_t>(world_z) * world_z;
    if (radius_squared < 15625)
    {
        const int radius =
            static_cast<int>(std::nearbyint(std::sqrt(static_cast<double>(radius_squared))));
        if (radius <= 4)
        {
            const int height = 125 - radius * 8;
            for (int y = 0; y < height; ++y)
            {
                chunk.set_block(local_x, y, local_z,
                                y == height - 1 && radius < 2 ? BlockType::fixed_lantern
                                                              : BlockType::bedrock);
            }
        }
    }
}

void add_trees(Chunk &chunk, const PerlinNoise &noise)
{
    const int base_x = chunk.chunk_x() * chunk_width;
    const int base_z = chunk.chunk_z() * chunk_depth;
    for (int local_z = 3; local_z < 13; ++local_z)
    {
        for (int local_x = 3; local_x < 13; ++local_x)
        {
            const int world_x = base_x + local_x;
            const int world_z = base_z + local_z;
            const float tree_noise = noise.noise(world_x * 0.4375F, world_z * 0.4375F);
            if (tree_noise <= 0.6F)
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
            int height = 5 + static_cast<int>(9.0F * (tree_noise - 0.6F));
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
                        const float value =
                            noise.noise((world_x + dx) * 0.5F, (crown_y + dy + world_min_y) * 0.5F,
                                        (world_z + dz) * 0.5F);
                        const float falloff =
                            1.0F -
                            std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz)) / 3.0F;
                        if (value + falloff > 0.25F)
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
// down every sky column and both channels flood-fill through transparent cells with
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

LightMap build_light_map(const WorldRegion &region, int selection_minimum_x,
                         int selection_minimum_z, int selection_maximum_x, int selection_maximum_z)
{
    // Include the original flood reach plus both directional bounce probes. This
    // makes a selected section's packed radiance exactly match a full mesh bake.
    LightMap map;
    map.minimum_x = std::max(region.minimum_x(), selection_minimum_x - light_bake_margin);
    map.minimum_z = std::max(region.minimum_z(), selection_minimum_z - light_bake_margin);
    const int maximum_x = std::min(region.maximum_x(), selection_maximum_x + light_bake_margin);
    const int maximum_z = std::min(region.maximum_z(), selection_maximum_z + light_bake_margin);
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
            }
        }
    }
    flood_fill_light(region, map, map.torch, queue);
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
        int count{};
        for (const auto &sample : samples)
        {
            if (!lighting_occluder(region, sample[0], sample[1], sample[2]))
            {
                accumulated_sun += light.sun_at(sample[0], sample[1], sample[2]);
                accumulated_torch += light.torch_at(sample[0], sample[1], sample[2]);
                ++count;
            }
        }
        // Four-bit levels scale by 17 so a full 15 maps exactly onto byte 255.
        auto sun_level = count == 0 ? 0U : static_cast<unsigned>(accumulated_sun * 17 / count);
        auto torch_level = count == 0 ? 0U : static_cast<unsigned>(accumulated_torch * 17 / count);
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
        }
        face.sky_light[corner_index] = static_cast<std::uint8_t>(std::min(sun_level, 255U));
        face.block_light[corner_index] = static_cast<std::uint8_t>(std::min(torch_level, 255U));
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
                                         const LightMap &light, int section_y)
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
                    if (neighbor == type)
                        continue;
                    if (!occludes(neighbor) || neighbor != type && !occludes(type))
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
    const std::vector<ChunkCoordinate> &selection) const
{
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
                             build_chunk_faces(*this, *task.chunk, light, task.coordinate.y)};
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

BiomeKind WorldGenerator::biome_at(int world_x, int world_z) const noexcept
{
    const float radius = folded_radius(world_x, world_z);
    if (radius < 300.0F)
        return BiomeKind::classic;
    if (radius < 1000.0F)
        return BiomeKind::lagoon;
    if (radius < 1700.0F)
        return BiomeKind::desert;
    if (radius < 2400.0F)
        return BiomeKind::mountain;
    if (radius < 3000.0F)
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
        const float radius = folded_radius(world_x, world_z);
        if (radius <= 300.0F || radius >= 3600.0F)
            return false;
        const float crash_noise = noise_.noise(world_x * 0.0046875F, world_z * 0.0046875F);
        if (crash_noise <= 0.55F)
            return false;
        const int depth = static_cast<int>((crash_noise - 0.5F) * 140.0F);
        const int rim = std::min(depth, static_cast<int>((crash_noise - 0.55F) * 200.0F));
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

Chunk WorldGenerator::generate_chunk(int chunk_x, int chunk_z) const
{
    Chunk chunk{chunk_x, chunk_z};
    const PerlinNoise &noise = noise_;
    const IntegerNoise &integer_noise = integer_noise_;
    for (int local_z = 0; local_z < chunk_depth; ++local_z)
    {
        const int world_z = chunk_z * chunk_depth + local_z;
        for (int local_x = 0; local_x < chunk_width; ++local_x)
        {
            const int world_x = chunk_x * chunk_width + local_x;
            const float radius = folded_radius(world_x, world_z);
            float hell_blend = 1.0F;
            if (radius < 200.0F)
            {
                classic_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
            }
            else if (radius < 300.0F)
            {
                const float blend = (radius - 200.0F) / 100.0F;
                classic_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F - blend);
                lagoon_column(chunk, noise, local_x, local_z, world_x, world_z, blend);
            }
            else if (radius < 900.0F)
            {
                lagoon_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
            }
            else if (radius < 1000.0F)
            {
                const float blend = (radius - 900.0F) / 100.0F;
                lagoon_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F - blend);
                desert_column(chunk, noise, local_x, local_z, world_x, world_z);
            }
            else if (radius < 1600.0F)
            {
                desert_column(chunk, noise, local_x, local_z, world_x, world_z);
            }
            else if (radius < 1700.0F)
            {
                const float blend = (radius - 1600.0F) / 100.0F;
                desert_column(chunk, noise, local_x, local_z, world_x, world_z);
                mountain_column(chunk, noise, local_x, local_z, world_x, world_z, blend);
            }
            else if (radius < 2300.0F)
            {
                mountain_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F);
            }
            else if (radius < 2400.0F)
            {
                const float blend = (radius - 2300.0F) / 100.0F;
                mountain_column(chunk, noise, local_x, local_z, world_x, world_z, 1.0F - blend);
                arctic_column(chunk, noise, local_x, local_z, world_x, world_z);
            }
            else if (radius < 3000.0F)
            {
                arctic_column(chunk, noise, local_x, local_z, world_x, world_z);
            }
            else if (radius < 3600.0F)
            {
                const float blend = (radius - 3000.0F) / 600.0F;
                hell_blend = 1.0F - blend;
                descent_column(chunk, noise, local_x, local_z, world_x, world_z, blend);
            }
            const float distance_blend = std::clamp(radius / 3600.0F, 0.0F, 1.0F);
            deposits(chunk, noise, integer_noise, slime_noise_, profile_, game_mode_, local_x,
                     local_z, world_x, world_z, distance_blend, hell_blend,
                     radius > 300.0F && radius < 3600.0F);
        }
    }
    add_trees(chunk, noise);
    return chunk;
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
    WorldRegion next{origin_chunk_x, origin_chunk_z, chunks_x, chunks_z};
    for (Chunk &destination : next.chunks())
    {
        const int source_x = destination.chunk_x() - current.origin_chunk_x();
        const int source_z = destination.chunk_z() - current.origin_chunk_z();
        if (source_x >= 0 && source_x < current.chunks_x() && source_z >= 0 &&
            source_z < current.chunks_z())
        {
            destination =
                current
                    .chunks()[static_cast<std::size_t>(source_x + source_z * current.chunks_x())];
        }
        else
        {
            destination = generate_chunk(destination.chunk_x(), destination.chunk_z());
        }
    }
    return next;
}

} // namespace openminerz::world
