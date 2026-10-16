#pragma once

// mod-world: custom world generation.
//
// A server or core mod declares a world in Luau tables (the `terrain` and
// `noise` script libraries, openminerz/modding/worldgen.hpp). That becomes a
// TerrainDefinition, compiled once into a CustomTerrain, which a
// WorldGenerator then consults instead of the retail passes
// (WorldGenerator::set_custom_terrain). The definition is data, never script:
// evaluation is native, every CustomTerrain function is const, keeps no cache
// and may run on any number of threads at once, and what a cell holds depends
// only on the world seed, the definition and the cell's coordinates. So host,
// joined clients and offline worlds generate the same terrain on their own,
// the way retail peers all run the same generator, and only edits travel.
//
// A layout places biomes by distance from the origin (bands with blends, then
// a mirror or repeat rule past the last band, like the retail fold at 4,400
// blocks) or by noise (climate regions), nested freely. A biome is a surface
// height expression plus optional 3D density (overhangs, arches, floating
// islands), surface and filler layers, a fluid below the sea level, caves,
// ore blobs and trees. A biome may also reuse retail terrain: one retail
// biome, or the whole retail generator with its own rings.
//
// Mod blocks: a definition names mod blocks by key; each becomes a palette
// entry with its retail fallback. A generated chunk holds the fallback (what
// retail, saves and stock messages see) and the palette index in its mod
// layer (Chunk::mod_cell), so every peer derives the identity of generated
// mod blocks itself and nothing about them is ever sent.
//
// Nothing here runs while no mod generator is installed: the WorldGenerator
// then runs exactly the retail passes it always did (tests/test_mod_world.cpp
// pins them with golden hashes).

#include "openminerz/world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::world
{

class WorldGenerator;

// ---------------------------------------------------------------------------
// Noise expressions
// ---------------------------------------------------------------------------

// An index into TerrainDefinition::nodes; `no_noise` for "none".
using NoiseRef = std::uint32_t;
inline constexpr NoiseRef no_noise = 0xFFFFFFFFU;

enum class NoiseOp : std::uint8_t
{
    constant,
    coord_x,  // world X
    coord_y,  // world Y (0 for a 2D expression)
    coord_z,  // world Z
    distance, // sqrt(x^2 + z^2) from the origin
    // Gradient noise summed over octaves: fbm in [-1, 1], ridged in [0, 1]
    // (sharp crests), billow in [-1, 1] (rounded hills, sharp valleys).
    fbm,
    ridged,
    billow,
    // Worley noise: distance to the nearest feature point (about 0..1), the
    // gap to the second nearest (cell edges, 0 on an edge), or a random
    // value in [0, 1) per cell.
    cellular,
    add,
    multiply,
    subtract, // inputs[0] - inputs[1]
    minimum,
    maximum,
    absolute,
    clamp,  // inputs[0] into [low, high]
    lerp,   // inputs[0] + (inputs[1] - inputs[0]) * inputs[2]
    curve,  // inputs[0] through `points` (piecewise linear, or smooth)
    // inputs[1] where inputs[0] < low, inputs[2] where inputs[0] > high,
    // blended between.
    select,
};

enum class CellularMode : std::uint8_t
{
    distance,
    edge,
    cell
};

struct NoiseNode final
{
    NoiseOp op{NoiseOp::constant};
    double value{};
    // fbm / ridged / billow / cellular: blocks per feature, and along Y for a
    // 3D expression (0 = the same as horizontally).
    double scale{64.0};
    double vertical_scale{};
    int octaves{4};
    double gain{0.5};
    double lacunarity{2.0};
    std::uint32_t seed{};
    bool three_d{};
    CellularMode cellular{CellularMode::distance};
    double jitter{1.0};
    std::vector<NoiseRef> inputs;
    double low{};
    double high{1.0};
    std::vector<std::array<double, 2>> points;
    bool smooth{};
};

// ---------------------------------------------------------------------------
// Biomes and the layout
// ---------------------------------------------------------------------------

// A block as a definition names it: a retail block, or a mod block (its
// fallback plus its 1-based palette index).
struct TerrainBlock final
{
    BlockType block{BlockType::empty};
    std::uint8_t mod{};
    bool operator==(const TerrainBlock &) const noexcept = default;
};

struct TerrainLayer final
{
    TerrainBlock block;
    int depth{1};
};

enum class RetailReuse : std::uint8_t
{
    none,
    // The whole retail generator (its rings, blends and fold) at this column.
    world,
    // One retail biome at full strength, with retail caves, ores and trees.
    classic,
    lagoon,
    desert,
    mountain,
    arctic,
    descent
};

struct TerrainCaves final
{
    bool enabled{true};
    // "cheese" caverns: carved where 3D noise falls under `threshold`.
    // Tunnels: carved where two noise fields both stay within `width` of 0.
    bool tunnels{true};
    double scale{48.0};
    double threshold{-0.55};
    double width{0.06};
    int y_min{-58};
    int y_max{40};
    // Whether caves may open through the top layer of the surface.
    bool breach_surface{true};
    std::uint32_t seed{};
};

// Blobs of `block` in `in` (the biome's filler when empty): each cube of
// `spacing` blocks holds one blob of radius `size` with probability `chance`.
struct TerrainOre final
{
    TerrainBlock block;
    std::vector<TerrainBlock> in;
    double chance{0.1};
    int size{2};
    int spacing{12};
    int y_min{-62};
    int y_max{40};
    std::uint32_t seed{};
};

enum class TreeShape : std::uint8_t
{
    // Retail's oak: a trunk and a noisy ball of leaves about three across.
    retail,
    round,   // a trunk and a sphere of leaves
    cone,    // a trunk inside stacked rings of leaves (spruce, redwood)
    column,  // one block stacked (cactus, crystal, pillar)
    dead,    // a bare trunk with a few stubs
    boulder, // a lump of `trunk` blocks resting on the ground
};

struct TerrainTree final
{
    TreeShape shape{TreeShape::retail};
    // Per surface column.
    double chance{0.01};
    int height_min{5};
    int height_max{8};
    int radius{3};
    // Two blocks wide (2 x 2) trunks for big trees.
    bool thick{};
    TerrainBlock trunk{BlockType::log};
    TerrainBlock leaves{BlockType::leaves};
    // Ground it may stand on; empty: the biome's top surface layer.
    std::vector<TerrainBlock> on;
    std::uint32_t seed{};
};

struct TerrainBiome final
{
    std::string key;  // "mod:name"
    std::string name; // display name
    // The retail biome the engine treats this one as where it needs one
    // (the default sky, the Lagoon water option, retail-only code).
    BiomeKind like{BiomeKind::classic};
    RetailReuse retail{RetailReuse::none};
    // World Y of the surface.
    NoiseRef height{no_noise};
    // Optional 3D density: a cell is solid where
    // y < height + strength * density(x, y, z).
    NoiseRef density{no_noise};
    double density_strength{};
    std::vector<TerrainLayer> surface;
    // Layers where the surface lies below the sea level.
    std::vector<TerrainLayer> underwater;
    TerrainBlock filler{BlockType::rock};
    // Fills the open cells under the sea level above the ground (frozen
    // lakes, lava pools); empty leaves them open.
    TerrainBlock fluid{BlockType::empty};
    TerrainCaves caves;
    std::vector<TerrainOre> ores;
    std::vector<TerrainTree> trees;
};

enum class LayoutInput : std::uint8_t
{
    distance, // from the origin, plus `warp`
    noise     // the `select` expression
};

enum class LayoutBeyond : std::uint8_t
{
    mirror, // past `period`, odd rings run backwards (retail's fold)
    repeat, // past `period`, start again from 0
    extend  // the last band goes on forever
};

struct LayoutBand final
{
    // The band ends at this input value (the last band's `to` is ignored).
    double to{};
    // Width of the blend from the previous band into this one, centred on
    // the boundary.
    double blend{};
    // A biome (index into TerrainDefinition::biomes) or a nested layout.
    std::int32_t biome{-1};
    std::int32_t layout{-1};
};

struct TerrainLayout final
{
    LayoutInput input{LayoutInput::distance};
    NoiseRef select{no_noise};
    NoiseRef warp{no_noise};
    LayoutBeyond beyond{LayoutBeyond::mirror};
    double period{}; // 0: the last band's start, rounded up to its `to`
    std::vector<LayoutBand> bands;
};

struct TerrainDefinition final
{
    std::string mod_id;
    std::string mod_version;
    std::vector<NoiseNode> nodes;
    std::vector<TerrainBiome> biomes;
    // layouts[0] is the root.
    std::vector<TerrainLayout> layouts;
    // Mod block keys used anywhere; palette index i + 1.
    std::vector<std::string> palette;
    std::vector<BlockType> palette_fallback;
    // World Y: columns whose surface is below it use their underwater
    // layers, and their fluid fills up to it.
    int sea_level{1};
    // Where a new world's player starts (x, z; on the ground).
    int spawn_x{8};
    int spawn_z{-8};
    // Retail's tower of bedrock with a lantern on top at the origin.
    bool origin_tower{true};
    // Bedrock at the bottom of the world, 1 to `bedrock_depth` cells.
    int bedrock_depth{3};
};

// What a column is made of at (x, z): up to this many biomes with weights
// that add up to 1, heaviest first.
inline constexpr std::size_t maximum_biome_weights = 8U;
struct BiomeWeight final
{
    std::uint16_t biome{};
    float weight{};
};

// (fix, wave 5) How the far landscape textures the side of a column whose top
// a biome's own surface layers made: the top block, the block under it, and
// how many cells down the rock starts (0: never). Retail's look of a block
// assumes retail's layering (four cells of snow on a retail peak); a mod's one
// cell of snow over stone has to read as a stone cliff from afar too.
// SurfaceColumn::top_profile / lower_profile name one (1-based); a far tile
// stores far_profile_byte() for it (far_terrain.hpp).
struct FarSurfaceProfile final
{
    TerrainBlock top;
    TerrainBlock under;
    std::uint8_t soil{};
    bool operator==(const FarSurfaceProfile &) const noexcept = default;
};

class CustomTerrain final
{
  public:
    // Checks and compiles the definition; throws std::invalid_argument with a
    // readable reason when it cannot be evaluated (a dangling or cyclic
    // reference, an empty layout, a palette over 255 blocks, ...).
    explicit CustomTerrain(TerrainDefinition definition);
    ~CustomTerrain();
    CustomTerrain(const CustomTerrain &) = delete;
    CustomTerrain &operator=(const CustomTerrain &) = delete;

    [[nodiscard]] const TerrainDefinition &definition() const noexcept;
    // SHA-256 (hex) of the canonical definition: what a save records and
    // peers compare. Presentation (names, atmosphere, music) is not in it.
    [[nodiscard]] const std::string &digest() const noexcept;

    // --- Evaluation. `retail` is the generator this terrain is installed in
    // (its seed, profile and mode; the retail passes a layout reuses).
    [[nodiscard]] Chunk generate_chunk(const WorldGenerator &retail, int chunk_x,
                                       int chunk_z) const;
    // The column without trees; `mods` (optional) receives the palette index
    // of each cell (0 for a retail block).
    void generate_column(const WorldGenerator &retail, int world_x, int world_z,
                         std::span<BlockType, chunk_height> cells,
                         std::span<std::uint8_t, chunk_height> *mods = nullptr) const noexcept;
    [[nodiscard]] SurfaceColumn surface_column(const WorldGenerator &retail, int world_x,
                                               int world_z, bool origin_tower) const noexcept;
    void tree_sites(const WorldGenerator &retail, int chunk_x, int chunk_z,
                    std::vector<TreeSite> &sites, bool top_grass_only) const;
    // (fix, wave 5) The side profiles surface_column() names (index + 1);
    // never more than far_mod_block_count minus the palette.
    [[nodiscard]] const std::vector<FarSurfaceProfile> &far_profiles() const noexcept;

    // The biomes at a column, heaviest first; returns how many.
    std::size_t biome_weights(const WorldGenerator &retail, int world_x, int world_z,
                              std::span<BiomeWeight, maximum_biome_weights> weights) const noexcept;
    // The heaviest biome (an index into definition().biomes).
    [[nodiscard]] std::uint16_t biome_at(const WorldGenerator &retail, int world_x,
                                         int world_z) const noexcept;
    // The retail biome the engine should assume at a column.
    [[nodiscard]] BiomeKind retail_kind_at(const WorldGenerator &retail, int world_x,
                                           int world_z) const noexcept;
    // World Y of the surface (the top of the shape before caves).
    [[nodiscard]] int surface_height(const WorldGenerator &retail, int world_x,
                                     int world_z) const noexcept;
    // One expression at a point (scripts' noise.sample, the SDK preview).
    [[nodiscard]] double sample(NoiseRef expression, std::int32_t seed, double x, double y,
                                double z) const noexcept;

    class Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

// The canonical bytes CustomTerrain::digest() hashes (exposed for tests).
[[nodiscard]] std::string canonical_terrain_bytes(const TerrainDefinition &definition);

// ---------------------------------------------------------------------------
// Drawing generated mod blocks
// ---------------------------------------------------------------------------

// How a client draws one palette entry: the atlas tiles of its top, side and
// bottom faces. `valid` false (its textures are not loaded) keeps the
// fallback's look.
struct ModPaletteLook final
{
    std::array<std::uint8_t, 3> tiles{};
    bool valid{};
    bool liquid{}; // (mod-liquid) meshed as a liquid (openminerz/world/liquid.hpp)
    bool operator==(const ModPaletteLook &) const noexcept = default;
};
using ModPaletteLooks = std::vector<ModPaletteLook>; // palette index - 1

// Gives the faces of generated mod blocks in a mesh section their tiles, after
// the retail mesher built it (every other face is left exactly as it was).
// Returns how many faces changed.
std::size_t apply_mod_palette(const WorldRegion &region, ChunkFaceBatch &batch,
                              const ModPaletteLooks &looks);

// ---------------------------------------------------------------------------
// Biome ids
// ---------------------------------------------------------------------------

// Biomes as scripts and the gameplay service compare them: 0..5 are the
// retail BiomeKind values ("classic" ... "descent"); any other name (a mod
// biome key, "mymod:redwood") gets a process-wide id from 64 up the first time
// it is named. Ids are never saved or sent.
inline constexpr int first_custom_biome_id = 64;
[[nodiscard]] int biome_id(std::string_view name);
// The name of an id ("classic" for an unknown one).
[[nodiscard]] std::string biome_name(int id);
[[nodiscard]] std::string_view retail_biome_name(BiomeKind kind) noexcept;
[[nodiscard]] std::optional<BiomeKind> retail_biome_from_name(std::string_view name) noexcept;

} // namespace openminerz::world
