#pragma once

#include "openminerz/game_mode.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/legacy_noise.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace openminerz::world
{

class CustomTerrain; // mod-world: openminerz/world/custom_terrain.hpp

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

struct CaveGenerationSite
{
    std::array<int, 3> cell;
    std::uint32_t carved_index{};
};

struct ChunkGenerationSites
{
    std::uint32_t carved_count{};
    std::vector<CaveGenerationSite> caves;
    std::vector<std::array<int, 3>> hell_floor;
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
    [[nodiscard]] const ChunkGenerationSites *generation_sites() const noexcept { return sites_.get(); }
    void record_cave_site(int x, int y, int z, bool first);
    void record_hell_site(int x, int y, int z);

    // mod-world: the mod layer of a chunk a mod generator built
    // (openminerz/world/custom_terrain.hpp). A cell's palette index (1-based
    // into the generator's mod blocks; 0 = a retail block) holds while the
    // cell keeps the block it was generated with: set_block with a different
    // block clears it, and remembers what it was (previous_mod_cell) for the
    // hooks that learn about a dig after the fact. Retail chunks have no layer
    // and pay one null test in set_block.
    [[nodiscard]] std::uint8_t mod_cell(int x, int local_y, int z) const noexcept;
    void set_mod_cell(int x, int local_y, int z, std::uint8_t palette);
    [[nodiscard]] std::uint8_t previous_mod_cell(int x, int local_y, int z) const noexcept;
    [[nodiscard]] bool has_mod_cells() const noexcept
    {
        return mods_ != nullptr;
    }
    // Hash of the mod layer (0 without one); hash() covers the blocks only.
    [[nodiscard]] std::uint64_t mod_hash() const noexcept;

  private:
    using BlockStorage = std::array<BlockType, chunk_width * chunk_depth * chunk_height>;
    struct ModCells;
    [[nodiscard]] static std::size_t index(int x, int local_y, int z) noexcept;

    int chunk_x_{};
    int chunk_z_{};
    // Regions are snapshotted for asynchronous streaming. Copy-on-write chunk storage keeps
    // that snapshot to a few shared pointers instead of copying the complete voxel window on
    // the game thread, while edits remain isolated from the worker's immutable snapshot.
    std::shared_ptr<BlockStorage> blocks_;
    std::shared_ptr<ChunkGenerationSites> sites_;
    std::shared_ptr<ModCells> mods_; // mod-world
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
    // Separate lava irradiance for the enhanced palette; block_light retains
    // the original combined emitter field used by Classic and simulation.
    std::array<std::uint8_t, 4> lava_light{};
    // (mod-liquid) A face of a liquid cell (liquid.hpp), drawn translucent:
    // 0 none, 1 a liquid face, 2 one of a cell with no liquid above it (the
    // top face, and the sides' upper edge, sit at the lowered surface).
    std::uint8_t liquid{};
};

class LiquidTable; // (mod-liquid) openminerz/world/liquid.hpp

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

// How far a baked section reads around itself: the original fifteen-cell light
// flood plus Rebirth's six-cell primary and three-cell secondary bounce probes
// and their one-cell surface sample. A cell change can therefore only alter the
// bake of sections within this many cells of it -- with one exception, sunlight
// running straight down an open sky column, which `edited_sections` handles.
inline constexpr int light_influence_radius = 25;

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
    // Adopts already built columns, row by row (x within z), instead of
    // allocating an empty one for every cell first. Streaming a window reuses
    // almost all of its columns, so this is what keeps a large window cheap.
    // Throws if the columns are not exactly this rectangle in that order.
    WorldRegion(int origin_chunk_x, int origin_chunk_z, int chunks_x, int chunks_z,
                std::vector<Chunk> chunks);

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
    // (mod-liquid) Chunk::mod_cell of a world cell (0 outside the region).
    [[nodiscard]] std::uint8_t mod_cell(int world_x, int world_y, int world_z) const noexcept;
    [[nodiscard]] std::vector<BlockRemovalEffect> removal_effects(int world_x, int world_y,
                                                                  int world_z) const;
    [[nodiscard]] std::vector<WorldFace> visible_faces() const;
    [[nodiscard]] std::vector<ChunkFaceBatch> visible_faces_by_chunk() const;
    // (mod-liquid) With `liquids`, a liquid cell shows faces only toward cells
    // that are neither liquid nor opaque (marked WorldFace::liquid), and a
    // block beside a liquid keeps the face toward it even where both hold the
    // same retail block. Null: exactly the retail mesher.
    [[nodiscard]] std::vector<ChunkFaceBatch> visible_faces_by_chunk(
        const std::vector<ChunkCoordinate> &selection, const LiquidTable *liquids = nullptr) const;
    // The sections whose baked faces can differ after the cells in `edits`
    // changed. See `edited_sections` below; this is the member form that also
    // reports the subset holding the edits themselves.
    [[nodiscard]] std::vector<ChunkCoordinate> edited_sections(
        const std::vector<std::array<int, 3>> &edits) const;
    // The same answer, but measured instead of assumed: a bounded local light
    // flood run over two states of the window, so only sections that read a
    // cell whose light value actually moved (or that read an edited cell
    // itself) are returned. With `previous_blocks` -- one per edit, what the
    // cell held before -- the two states are exactly before and after. Without
    // it the two states bracket them: every edited cell fully open and emitting
    // against every one sealed and dark, which is still sound but has to assume
    // an emitter may have been removed, so it reports a wider set. The result
    // is always a subset of `edited_sections`, and usually a small one, because
    // most of the geometric apron around a change is already at its final
    // brightness. Scattered edits fall back to `edited_sections`.
    [[nodiscard]] std::vector<ChunkCoordinate> light_changed_sections(
        const std::vector<std::array<int, 3>> &edits,
        const std::vector<BlockType> *previous_blocks = nullptr) const;
    [[nodiscard]] static std::vector<ChunkCoordinate> edited_geometry_sections(
        const std::vector<std::array<int, 3>> &edits);
    [[nodiscard]] std::uint64_t hash() const noexcept;

  private:
    int origin_chunk_x_{};
    int origin_chunk_z_{};
    int chunks_x_{};
    int chunks_z_{};
    std::vector<Chunk> chunks_;
};

// The top of one generated column, for far-terrain summaries: the highest cell
// that is not empty, trees excluded (they are reported by tree_sites).
// `top_local_y` is -1 for a column with nothing in it.
//
// Where the shape of the land leaves open air under that top span (a Lagoon
// overhang or floating island, the broken ground of the descent) the column
// also says where the span ends and what lies beneath: `under_local_y` is the
// lowest cell of the top solid span, `lower_local_y` / `lower` the top cell of
// the ground under the air below it. A smaller piece hanging in that air,
// between the span and the ground, is passed over. All of these are cells
// exactly as generate_chunk builds them. They stay -1 for a column without
// such air; air that only a cave or the hollow under a crash site cuts beneath
// ordinary ground is not reported.
struct SurfaceColumn final
{
    std::int16_t top_local_y{-1};
    BlockType top{BlockType::empty};
    std::int16_t under_local_y{-1};
    std::int16_t lower_local_y{-1};
    BlockType lower{BlockType::empty};
    // mod-world: the palette index of a generated mod block on top / on the
    // ground under the air (0: the retail block itself; always 0 for retail).
    std::uint8_t top_mod{};
    std::uint8_t lower_mod{};
    // (fix, wave 5) mod-world: the side profile (CustomTerrain::far_profiles,
    // 1-based) of the run under the top / the ground, when the far landscape
    // must not take the block's retail layering (0; always 0 for retail).
    std::uint8_t top_profile{};
    std::uint8_t lower_profile{};

    [[nodiscard]] constexpr bool overhangs() const noexcept
    {
        return under_local_y > 0;
    }
    bool operator==(const SurfaceColumn &) const noexcept = default;
};

// mod-world (wave 5): the crown a far landscape draws for a tree site.
enum class TreeCrown : std::uint8_t
{
    retail, // retail's oak: a blob of leaves about three across above the trunk
    round,  // a ball of `crown_radius` around the top of the trunk
    cone,   // stacked rings narrowing upwards from a third of the way up (spruce)
};

// A generated tree: the grass block it stands on and how many logs its trunk
// has. The crown is centred one block above the trunk and about three across.
struct TreeSite final
{
    int x{};
    int z{};
    std::int16_t ground_local_y{};
    std::int16_t trunk_height{};
    // mod-world (wave 5): a mod generator's tree as the far landscape draws
    // it. Retail's own trees keep these defaults.
    TreeCrown crown{TreeCrown::retail};
    std::uint8_t crown_radius{};
    bool thick{};
    BlockType trunk{BlockType::log};
    BlockType leaves{BlockType::leaves};
    // Palette indices of generated mod blocks (0: the retail block itself).
    std::uint8_t trunk_mod{};
    std::uint8_t leaves_mod{};
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
    // --- Far-terrain summaries. Presentation only: nothing simulated, saved or
    // sent reads these. They run the same column passes as generate_chunk, on
    // one column instead of a 16 x 128 x 16 chunk, and are safe to call from
    // any number of threads at once.
    //
    // The whole generated column, without trees (a tree only ever fills cells
    // that are empty here).
    void generate_column(int world_x, int world_z,
                         std::span<BlockType, chunk_height> cells) const noexcept;
    // The same column's top cell, resolved without carving the caves and ores
    // underneath it. `origin_tower` false leaves the tower at the world origin
    // out, for callers that draw it as its own object.
    [[nodiscard]] SurfaceColumn surface_column(int world_x, int world_z,
                                               bool origin_tower = true) const noexcept;
    // Appends the trees generate_chunk plants in one chunk. `top_grass_only`
    // reports only those standing on the top block of their column (all but
    // the rare tree under an overhang), which costs a fraction of the time.
    void tree_sites(int chunk_x, int chunk_z, std::vector<TreeSite> &sites,
                    bool top_grass_only = false) const;
    [[nodiscard]] WorldRegion generate_region(int origin_chunk_x, int origin_chunk_z, int chunks_x,
                                              int chunks_z) const;
    [[nodiscard]] WorldRegion stream_region(const WorldRegion &current, int origin_chunk_x,
                                            int origin_chunk_z, int chunks_x = -1,
                                            int chunks_z = -1) const;

    // --- mod-world: a server or core mod's generator
    // (openminerz/world/custom_terrain.hpp). Installed, it answers every
    // function above in place of the retail passes (copies of this generator
    // share it; it is immutable and thread-safe). Null (the default) is
    // retail, untouched.
    void set_custom_terrain(std::shared_ptr<const CustomTerrain> terrain) noexcept;
    [[nodiscard]] const std::shared_ptr<const CustomTerrain> &custom_terrain() const noexcept
    {
        return custom_;
    }
    // The retail passes whatever is installed: what a custom layout reuses
    // (and exactly what the functions above run without one).
    [[nodiscard]] Chunk retail_chunk(int chunk_x, int chunk_z) const;
    void retail_column(int world_x, int world_z,
                       std::span<BlockType, chunk_height> cells) const noexcept;
    [[nodiscard]] SurfaceColumn retail_surface_column(int world_x, int world_z,
                                                      bool origin_tower = true) const noexcept;
    void retail_tree_sites(int chunk_x, int chunk_z, std::vector<TreeSite> &sites,
                           bool top_grass_only = false) const;
    [[nodiscard]] BiomeKind retail_biome_at(int world_x, int world_z) const noexcept;
    // One column of a chunk being built, as generate_chunk builds it (with
    // Modern's encounter sites).
    void retail_build_column(Chunk &chunk, int local_x, int local_z) const;
    // One retail biome at full strength (its surface pass, then retail caves,
    // ores, the hell layers and bedrock as in the middle of its band; no
    // trees), into a chunk or a column.
    void retail_build_biome_column(Chunk &chunk, BiomeKind biome, int local_x, int local_z) const;
    void retail_biome_column(BiomeKind biome, int world_x, int world_z,
                             std::span<BlockType, chunk_height> cells) const noexcept;
    // Retail's tree pass over a chunk, for trunks in columns `allowed` says
    // yes to (local x, z).
    void retail_trees(Chunk &chunk, bool (*allowed)(const void *context, int local_x, int local_z),
                      const void *context) const;

  private:
    std::int32_t seed_{};
    TerrainProfile profile_{TerrainProfile::classic};
    GameMode game_mode_{GameMode::endurance};
    PerlinNoise noise_;
    IntegerNoise integer_noise_;
    // Vanilla's CrashSiteDepositer leaves its slime field initializer at seed 1 even when the
    // terrain/ore generators are rebuilt from the world seed.
    IntegerNoise slime_noise_;
    std::shared_ptr<const CustomTerrain> custom_; // mod-world
};

// BlockTerrain.GetSimpleSunlightAtPoint (see world.cpp): the sunlight term of
// the dragon's target score. 0 outside the region, -1 inside an opaque block.
[[nodiscard]] float simple_sunlight_at(const WorldRegion &region, int x, int y, int z);

// The same question answered for one cell without baking the 33 x 33 column
// box around it (about 5 ms): a search outwards from the cell for the nearest
// one under open sky, which is `15 - steps` over 15. Equal to
// simple_sunlight_at wherever the light reaches the cell through clear cells;
// a leaf or ice block in the way counts as a wall here, so under a canopy it
// can read a level or two low. Meant for a caller that asks every tick, like
// EnemyManager.OnUpdate's sunlight test at the player's chest.
[[nodiscard]] float local_sunlight_at(const WorldRegion &region, int x, int y, int z);

// BlockTerrain.GetSimpleLightAtPoint: the cell's stored sunlight and torchlight
// levels over 15. (1, 0) for a cell that holds no block yet, (-1, -1) inside
// an opaque block, (0, 0) outside the region. It is what EnemyManager asks
// before it lets a cave enemy appear: both must be at most 0.4, so a torch
// keeps skeletons out of the eight cells around it.
struct SimpleLight final
{
    float sun{};
    float torch{};
};
[[nodiscard]] SimpleLight simple_light_at(const WorldRegion &region, int x, int y, int z);

} // namespace openminerz::world
