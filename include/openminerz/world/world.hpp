#pragma once

#include "openminerz/game_mode.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/legacy_noise.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace openminerz::world
{

constexpr int chunk_width = 16;
constexpr int chunk_depth = 16;
constexpr int chunk_height = 128;
constexpr int chunk_section_height = 16;
constexpr int chunk_section_count = chunk_height / chunk_section_height;
constexpr int world_min_y = -64;
constexpr float water_level = 1.5F;

enum class BiomeKind
{
    classic,
    lagoon,
    desert,
    mountain,
    arctic,
    descent,
};

enum class TerrainProfile
{
    classic,
    modern,
};

class Chunk final
{
  public:
    Chunk(int chunk_x, int chunk_z);

    [[nodiscard]] int chunk_x() const noexcept;
    [[nodiscard]] int chunk_z() const noexcept;
    [[nodiscard]] BlockType block(int x, int local_y, int z) const noexcept;
    void set_block(int x, int local_y, int z, BlockType type);
    [[nodiscard]] int surface_local_y(int x, int z) const noexcept;
    [[nodiscard]] std::size_t count(BlockType type) const noexcept;
    [[nodiscard]] std::uint64_t hash() const noexcept;

  private:
    using BlockStorage = std::array<BlockType, chunk_width * chunk_depth * chunk_height>;
    [[nodiscard]] static std::size_t index(int x, int local_y, int z) noexcept;

    int chunk_x_{};
    int chunk_z_{};
    // Regions are snapshotted for asynchronous streaming. Copy-on-write chunk storage keeps
    // that snapshot to a few shared pointers instead of copying the complete voxel window on
    // the game thread, while edits remain isolated from the worker's immutable snapshot.
    std::shared_ptr<BlockStorage> blocks_;
};

struct WorldFace final
{
    int x{};
    int y{};
    int z{};
    FaceDirection direction{};
    BlockType block{};
    std::uint8_t tile{};
    // Recovered vanilla terrain vertices carried smooth sun/torch values and an
    // eight-neighbour occupancy mask. Keep the equivalent lighting data on the
    // clean-room face so both the D3D and SDL paths can shade each corner.
    std::array<std::uint8_t, 4> ambient_occlusion{255U, 255U, 255U, 255U};
    std::array<std::uint8_t, 4> sky_light{255U, 255U, 255U, 255U};
    // Torch-channel light (torches/lanterns/lava/space goo), flood-filled like the
    // original four-bit block lighting and baked per corner.
    std::array<std::uint8_t, 4> block_light{};
    // Rebirth's local directional multi-bounce probe. RGB stores gamma-two
    // companded sky radiance reflected by surfaces in this face's outward
    // hemisphere; alpha stores equivalent torch/emissive bounce. Companding
    // preserves low-light detail in one R8G8B8A8_UNORM vertex channel.
    std::array<std::uint8_t, 4> indirect_light{};
    // Direction toward increasing flood-filled block light, biased into RGB
    // bytes from [-1, 1]; alpha is gradient confidence. This lets Rebirth apply
    // a normal-aware point-light lobe while the scalar per-corner field remains
    // the authoritative occlusion/falloff contract.
    std::array<std::uint8_t, 4> block_light_lobe{128U, 128U, 128U, 0U};
    std::uint8_t emission{};
};

struct ChunkCoordinate final
{
    int x{};
    int z{};
    // Vertical mesh section. Voxel storage stays a copy-on-write column so terrain generation
    // can preserve the original column algorithms, while rendering/rebuild work is 16 high.
    int y{};

    bool operator==(const ChunkCoordinate &) const noexcept = default;
};

struct ChunkFaceBatch final
{
    ChunkCoordinate coordinate;
    std::vector<WorldFace> faces;
};

struct BlockRemovalEffect final
{
    int x{};
    int y{};
    int z{};
    BlockType block{};
    // The original game drops fixtures detached because their supporting
    // block disappeared. Door partner halves are removed without a second drop.
    bool drops_as_dependent{};
};

class WorldRegion final
{
  public:
    WorldRegion(int origin_chunk_x, int origin_chunk_z, int chunks_x, int chunks_z);

    [[nodiscard]] int origin_chunk_x() const noexcept;
    [[nodiscard]] int origin_chunk_z() const noexcept;
    [[nodiscard]] int chunks_x() const noexcept;
    [[nodiscard]] int chunks_z() const noexcept;
    [[nodiscard]] int minimum_x() const noexcept;
    [[nodiscard]] int minimum_z() const noexcept;
    [[nodiscard]] int maximum_x() const noexcept;
    [[nodiscard]] int maximum_z() const noexcept;
    [[nodiscard]] bool contains(int world_x, int world_y, int world_z) const noexcept;
    [[nodiscard]] const std::vector<Chunk> &chunks() const noexcept;
    [[nodiscard]] std::vector<Chunk> &chunks() noexcept;
    [[nodiscard]] BlockType block(int world_x, int world_y, int world_z) const noexcept;
    void set_block(int world_x, int world_y, int world_z, BlockType type);
    [[nodiscard]] std::vector<BlockRemovalEffect> removal_effects(int world_x, int world_y,
                                                                  int world_z) const;
    [[nodiscard]] std::vector<WorldFace> visible_faces() const;
    [[nodiscard]] std::vector<ChunkFaceBatch> visible_faces_by_chunk() const;
    [[nodiscard]] std::vector<ChunkFaceBatch> visible_faces_by_chunk(
        const std::vector<ChunkCoordinate> &selection) const;
    [[nodiscard]] std::uint64_t hash() const noexcept;

  private:
    int origin_chunk_x_{};
    int origin_chunk_z_{};
    int chunks_x_{};
    int chunks_z_{};
    std::vector<Chunk> chunks_;
};

class WorldGenerator final
{
  public:
    explicit WorldGenerator(std::int32_t seed,
                            TerrainProfile profile = TerrainProfile::classic,
                            GameMode game_mode = GameMode::endurance);

    [[nodiscard]] std::int32_t seed() const noexcept;
    [[nodiscard]] TerrainProfile profile() const noexcept;
    [[nodiscard]] GameMode game_mode() const noexcept;
    [[nodiscard]] BiomeKind biome_at(int world_x, int world_z) const noexcept;
    // Diagnostic/navigation helper derived from CrashSiteDepositer's exact
    // low-frequency field. Finds a column whose positive rim produces space
    // rock, without generating a huge region around the origin first.
    [[nodiscard]] std::optional<std::array<int, 2>>
    find_crash_site_column(int center_x, int center_z, int max_distance) const noexcept;
    [[nodiscard]] Chunk generate_chunk(int chunk_x, int chunk_z) const;
    [[nodiscard]] WorldRegion generate_region(int origin_chunk_x, int origin_chunk_z, int chunks_x,
                                              int chunks_z) const;
    [[nodiscard]] WorldRegion stream_region(const WorldRegion &current, int origin_chunk_x,
                                            int origin_chunk_z, int chunks_x = -1,
                                            int chunks_z = -1) const;

  private:
    std::int32_t seed_{};
    TerrainProfile profile_{TerrainProfile::classic};
    GameMode game_mode_{GameMode::endurance};
    PerlinNoise noise_;
    IntegerNoise integer_noise_;
    // Vanilla's CrashSiteDepositer leaves its slime field initializer at seed 1 even when the
    // terrain/ore generators are rebuilt from the world seed.
    IntegerNoise slime_noise_;
};

} // namespace openminerz::world
