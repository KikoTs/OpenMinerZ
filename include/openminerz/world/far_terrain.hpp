#pragma once

// Far terrain: the landscape beyond the detailed window, drawn cheaply out to
// kilometres and never simulated.
//
// PRESENTATION ONLY, and only for the Enhanced ("Rebirth") presentation.
// Nothing here loads a chunk into the gameplay window, creates an entity,
// feeds light or collision, changes `region_loaded()`, or asks a multiplayer
// host for anything: every column comes from the local, deterministic
// generator (WorldGenerator::surface_column) plus the block edits this client
// already knows.
//
// HOW IT IS LAID OUT
//   * The world is cut into square TILES of 64 x 64 cells. A tile of level L
//     has cells `base_cell << L` blocks wide (1, 2, 4 ... blocks), so a tile
//     of the next level covers exactly four tiles of this one: a quadtree.
//   * A cell is one column of the real world, sampled at the cell's lowest
//     corner: its top block and how high that is. Level L therefore samples a
//     subset of the columns level L - 1 samples, which is what lets a fine
//     tile morph onto its parent without storing the parent's data twice.
//   * A level is used out to `ring_cells` of its own cells from the camera.
//     Inside that distance its tiles are replaced by their four children.
//     Over the last quarter of the range every vertex slides onto the parent
//     grid (a geomorph), so a tile swap never changes the picture.
//   * Tiles are built on worker threads, nearest first and coarse before fine,
//     kept in a bounded cache, and their column summaries are stored on disk
//     per world so a place visited before appears at once.
//
// The GPU side (far_terrain_renderer.hpp) draws each quadrant of a selected
// tile as a fixed grid of flat-topped columns that reads this module's cell
// map; it owns no terrain knowledge of its own.

#include "openminerz/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace openminerz::world
{

struct ModPaletteLook; // custom_terrain.hpp (mod-world)

inline constexpr int far_tile_cells = 64;
inline constexpr int far_quadrant_cells = far_tile_cells / 2;
// Cells stored below the tile's own (so a border cell's parent, two cells
// back, is in the map) and above it (the neighbour a side face runs down to).
inline constexpr int far_tile_low_apron = 2;
inline constexpr int far_tile_high_apron = 1;
inline constexpr int far_tile_grid = far_tile_cells + far_tile_low_apron + far_tile_high_apron;
inline constexpr int far_maximum_level = 9;
// mod-world: a far cell's block byte above the retail ids names a generated
// mod block (palette index = byte - far_mod_block_base + 1), which the far
// renderer draws with that block's own tiles.
inline constexpr int far_mod_block_base = 96;
inline constexpr int far_mod_block_count = 256 - far_mod_block_base;
[[nodiscard]] constexpr std::uint8_t far_block_byte(BlockType block, std::uint8_t mod) noexcept
{
    return mod != 0U && mod <= far_mod_block_count
               ? static_cast<std::uint8_t>(far_mod_block_base + mod - 1)
               : static_cast<std::uint8_t>(block);
}
// (fix, wave 5) A generator's side profile (CustomTerrain::far_profiles,
// 1-based) takes the bytes from the top of the mod range down; the generator
// keeps palette + profiles within far_mod_block_count.
[[nodiscard]] constexpr std::uint8_t far_profile_byte(std::uint8_t profile) noexcept
{
    return static_cast<std::uint8_t>(256 - static_cast<int>(profile));
}
// Trees are drawn as their own boxes while a cell is at most this wide.
inline constexpr int far_tree_cell_limit = 4;

struct FarTerrainSettings final
{
    // How far the landscape is drawn, in blocks. 0 turns the system off.
    int distance{};
    // Width of the finest cell, in blocks (1 or 2).
    int base_cell{1};
    // How many of its own cells a level reaches from the camera. Larger means
    // each level hands over to the coarser one further away.
    int ring_cells{256};

    [[nodiscard]] constexpr bool enabled() const noexcept
    {
        return distance > 0;
    }
    bool operator==(const FarTerrainSettings &) const noexcept = default;
};

// Where the detailed window hands over to far terrain, as horizontal distances
// from the eye. The detailed terrain is fully itself up to `start`, has faded
// completely into the far terrain by `end`, and the far terrain is not drawn
// nearer than `clip`.
struct FarHandover final
{
    float start{};
    float end{};
    float clip{};
};
// `visibility_distance` is the detailed window's draw distance in blocks (the
// View distance setting times four).
[[nodiscard]] constexpr FarHandover far_handover(float visibility_distance) noexcept
{
    const float limited = visibility_distance - 10.0F;
    const float end = limited < 24.0F ? 24.0F : limited > 118.0F ? 118.0F : limited;
    const float band = end * 0.30F < 10.0F ? 10.0F : end * 0.30F > 32.0F ? 32.0F : end * 0.30F;
    const float start = end - band;
    return {start, end, start - 2.0F < 6.0F ? 6.0F : start - 2.0F};
}

[[nodiscard]] constexpr int far_cell_size(const FarTerrainSettings &settings, int level) noexcept
{
    return settings.base_cell << level;
}
[[nodiscard]] constexpr int far_tile_size(const FarTerrainSettings &settings, int level) noexcept
{
    return far_cell_size(settings, level) * far_tile_cells;
}
// The distance out to which a level is the one drawn.
[[nodiscard]] constexpr float far_level_range(const FarTerrainSettings &settings,
                                              int level) noexcept
{
    return static_cast<float>(settings.ring_cells) *
           static_cast<float>(far_cell_size(settings, level));
}
// Where that level's vertices start sliding onto the parent grid. The slide
// has to begin beyond anything the finer level can still be drawing, which a
// quarter of the range guarantees for ring_cells >= 192.
[[nodiscard]] constexpr float far_level_morph_start(const FarTerrainSettings &settings,
                                                    int level) noexcept
{
    return far_level_range(settings, level) * 0.75F;
}
// The coarsest level in use: the first whose range reaches the far distance.
[[nodiscard]] constexpr int far_top_level(const FarTerrainSettings &settings) noexcept
{
    int level = 0;
    while (level < far_maximum_level &&
           far_level_range(settings, level) < static_cast<float>(settings.distance))
        ++level;
    return level;
}
// An upper bound on the tiles that can be in use at once, plus slack for the
// ones on their way in or out. The cell-map array on the GPU and the tile
// cache in memory are both exactly this big: 18 KB of each per tile. (The GPU
// holds a second array of the same shape for the tiles with floating columns;
// in memory only those tiles carry a second map.)
[[nodiscard]] int far_tile_capacity(const FarTerrainSettings &settings) noexcept;

struct FarTileKey final
{
    std::int32_t level{};
    std::int32_t x{};
    std::int32_t z{};

    bool operator==(const FarTileKey &) const noexcept = default;
};
struct FarTileKeyHash final
{
    [[nodiscard]] std::size_t operator()(const FarTileKey &key) const noexcept
    {
        std::uint64_t value = static_cast<std::uint32_t>(key.x);
        value = value * 0x9E3779B97F4A7C15ULL + static_cast<std::uint32_t>(key.z);
        value = value * 0x9E3779B97F4A7C15ULL + static_cast<std::uint32_t>(key.level);
        return static_cast<std::size_t>(value ^ (value >> 29U));
    }
};
[[nodiscard]] constexpr FarTileKey far_parent(const FarTileKey &key) noexcept
{
    return {key.level + 1, key.x >> 1, key.z >> 1};
}
[[nodiscard]] constexpr FarTileKey far_child(const FarTileKey &key, int quadrant) noexcept
{
    return {key.level - 1, key.x * 2 + (quadrant & 1), key.z * 2 + (quadrant >> 1)};
}

// What the generator says about a tile: for every cell of the stored grid the
// height of its top face above the bottom of the world (local y + 1; 0 where
// the column is empty) and the block on top. This is the part that is a pure
// function of the world's seed, and so the part kept on disk.
//
// A column whose top span has open air under it (SurfaceColumn::overhangs: a
// Lagoon overhang, a floating island) also says where: `under` is the height
// of the span's underside, `lower_height` the height of the ground beneath
// the air and `lower_block` the block on that ground. All three are 0 for a
// column that is solid from its top down.
struct FarTileSummary final
{
    std::array<std::uint8_t, far_tile_grid * far_tile_grid> height{};
    std::array<std::uint8_t, far_tile_grid * far_tile_grid> block{};
    std::array<std::uint8_t, far_tile_grid * far_tile_grid> under{};
    std::array<std::uint8_t, far_tile_grid * far_tile_grid> lower_height{};
    std::array<std::uint8_t, far_tile_grid * far_tile_grid> lower_block{};
};
[[nodiscard]] constexpr std::size_t far_grid_index(int cell_x, int cell_z) noexcept
{
    return static_cast<std::size_t>((cell_z + far_tile_low_apron) * far_tile_grid +
                                    (cell_x + far_tile_low_apron));
}
// The world column a cell of a tile samples.
[[nodiscard]] constexpr std::array<int, 2> far_cell_column(const FarTerrainSettings &settings,
                                                           const FarTileKey &key, int cell_x,
                                                           int cell_z) noexcept
{
    const int cell = far_cell_size(settings, key.level);
    return {(key.x * far_tile_cells + cell_x) * cell, (key.z * far_tile_cells + cell_z) * cell};
}

// Block edits this client knows about (its own save's, or a delta list a host
// already sent), keyed by column. Shared with the tile workers.
class FarEditOverlay final
{
  public:
    FarEditOverlay();
    ~FarEditOverlay();
    FarEditOverlay(const FarEditOverlay &) = delete;
    FarEditOverlay &operator=(const FarEditOverlay &) = delete;

    void set(int world_x, int world_y, int world_z, BlockType block);
    // Forgets every edit of the columns in the rectangle (inclusive) and
    // returns the columns that had any.
    std::vector<std::array<int, 2>> erase(int minimum_x, int minimum_z, int maximum_x,
                                          int maximum_z);
    void clear();
    [[nodiscard]] std::size_t column_count() const;
    // The edits of one column as {local y, block}, or nothing.
    [[nodiscard]] bool column(int world_x, int world_z,
                              std::vector<std::array<std::uint8_t, 2>> &edits) const;
    // True when some edited column lies inside the rectangle.
    [[nodiscard]] bool touches(int minimum_x, int minimum_z, int maximum_x, int maximum_z) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The generator's summary of one tile. The origin tower is left out: it is far
// narrower than a coarse cell and is drawn as its own object instead.
[[nodiscard]] FarTileSummary summarize_far_tile(const WorldGenerator &generator,
                                                const FarTerrainSettings &settings,
                                                const FarTileKey &key);

// A tile ready to draw.
struct FarTile final
{
    FarTileKey key{};
    // far_tile_grid x far_tile_grid texels of four bytes, row by row from the
    // low apron:
    //   [0] height of the top face above the world's floor (0 = no column)
    //   [1] the block on top (BlockType)
    //   [2] bits 0-3 sky occlusion (15 = open), bits 4-7 slope x, high part
    //   [3] bits 0-1 slope x, low part; bits 2-7 slope z
    // The slope is the surface normal's x and z, six signed bits each.
    std::vector<std::uint8_t> cells;
    // The second layer: what lies under the columns whose top span floats.
    // Empty unless the tile has such a column; otherwise the same grid of
    // four-byte texels:
    //   [0] height of the underside of the top span (0 = solid to the floor)
    //   [1] height of the ground under the air beneath it
    //   [2] the block on that ground
    //   [3] unused
    // A gap is kept only while it is at least one cell of the tile's level
    // tall; a smaller one is drawn as the solid column it nearly is.
    std::vector<std::uint8_t> lower;
    // Bit q: quadrant q (or the cell just beyond its edge) has such a column.
    std::uint8_t lower_quadrants{};
    // The same, finer: bit q * 4 + b for block b (x + 2 * z) of the four
    // blocks of 16 x 16 cells that make up quadrant q. Only these blocks are
    // drawn a second and a third time.
    std::uint16_t lower_blocks{};
    // Per quadrant (x + 2 * z), in the same units as the height byte. The
    // minimum counts the ground under an overhang.
    std::array<std::uint8_t, 4> minimum_height{};
    std::array<std::uint8_t, 4> maximum_height{};
    // Trees standing in each quadrant; empty above far_tree_cell_limit.
    std::array<std::vector<TreeSite>, 4> trees;
};
[[nodiscard]] std::shared_ptr<const FarTile> build_far_tile(const WorldGenerator &generator,
                                                            const FarTerrainSettings &settings,
                                                            const FarTileKey &key,
                                                            FarTileSummary summary,
                                                            const FarEditOverlay *edits);
// The six-bit slope fields of a cell-map texel, back as a unit normal.
[[nodiscard]] std::array<float, 3> far_cell_normal(std::uint8_t byte2,
                                                   std::uint8_t byte3) noexcept;

// --- Selection ---------------------------------------------------------------

// The camera a frame is drawn from.
struct FarView final
{
    double eye_x{};
    double eye_y{};
    double eye_z{};
    std::array<float, 3> right{1.0F, 0.0F, 0.0F};
    std::array<float, 3> up{0.0F, 1.0F, 0.0F};
    std::array<float, 3> forward{0.0F, 0.0F, -1.0F};
    // Tangents of half the horizontal and vertical field of view.
    float tangent_x{1.0F};
    float tangent_y{1.0F};
    // Far terrain is not drawn nearer than this (FarHandover::clip).
    float clip_radius{};
    // The clock tiles' arrival times are measured on.
    double seconds{};
};

// One quadrant of one tile, as the vertex shader reads it. 32 bytes.
struct FarQuadrantInstance final
{
    // World block coordinates of the quadrant's first cell.
    std::int32_t origin_x{};
    std::int32_t origin_z{};
    // bits 0-10 cell size in blocks, 12-23 slice of the cell-map array,
    // 24 quadrant x, 25 quadrant z, 26 draws trees, 27 no vertex morphs,
    // 28-29 what this instance draws (0 the quadrant's columns; 1 the ground
    // under the overhangs of one 16 x 16 block of it, 2 the undersides of
    // those overhangs), 11 and 31 that block's x and z, 30 the tile has a
    // second layer (FarTile::lower) in the same slice of the lower array.
    std::uint32_t packed{};
    // The geomorph: 0 below `morph_start`, 1 at `morph_start + 1 / morph_scale`.
    float morph_start{};
    float morph_scale{};
    // A tile that arrived late starts fully morphed and relaxes over a moment.
    float morph_floor{};
    // Nearest horizontal distance to the eye (the draw order).
    float reserved0{};
    float reserved1{};
};
static_assert(sizeof(FarQuadrantInstance) == 32U);

// A box drawn on top of the columns: a tree trunk or crown, a piece of the
// origin tower. Positions are relative to FarTerrainFrame::box_origin. 16 bytes.
struct FarBoxInstance final
{
    // x and z relative to the frame's box origin; y above the world's floor.
    std::int16_t minimum_x{};
    std::int16_t minimum_y{};
    std::int16_t minimum_z{};
    // Horizontal distance over which the box shrinks away (`fade_end` 0 for
    // never).
    std::int16_t fade_start{};
    std::uint8_t size_x{};
    std::uint8_t size_y{};
    std::uint8_t size_z{};
    // Atlas tiles of the side and the top face.
    std::uint8_t side_tile{};
    std::uint8_t top_tile{};
    // bit 0 glows (the tower's lantern); bits 1..7 hold the shared tree
    // ground height. All crown/trunk boxes must shrink around that same foot.
    std::uint8_t flags{};
    std::uint16_t fade_end{};
};
static_assert(sizeof(FarBoxInstance) == 16U);

// What the selection needs to know about a tile that is ready to draw.
struct FarLoadedTile final
{
    std::uint32_t slice{};
    std::array<std::uint8_t, 4> minimum_height{};
    std::array<std::uint8_t, 4> maximum_height{};
    // FarTile::lower_quadrants and lower_blocks: what is drawn with its second
    // layer.
    std::uint8_t lower_quadrants{};
    std::uint16_t lower_blocks{};
    // FarView::seconds at which the tile became drawable.
    double ready_seconds{-1000.0};
};

// Which tiles are in use for an eye position, and which quadrants each one
// draws itself (the others are drawn by its children). This is the part of
// the selection that walks the quadtree; it only changes when the eye has
// moved or a tile has arrived, so it is kept from frame to frame.
struct FarPlan final
{
    struct Tile final
    {
        FarTileKey key{};
        // Stays valid for as long as the caller keeps the record it returned.
        const FarLoadedTile *tile{};
        // Bit q set: this tile draws its quadrant q.
        std::uint8_t quadrants{};
    };
    std::vector<Tile> tiles;
    // Tiles that would be drawn if they were loaded, nearest first within a
    // level, coarser levels first.
    std::vector<FarTileKey> wanted;
    // False while a tile of the coarsest level inside the distance is missing.
    bool roots_ready{true};
};
// `loaded(key)` returns the tile's record, or nullptr while it is not ready.
template <typename Lookup>
[[nodiscard]] FarPlan plan_far_tiles(const FarTerrainSettings &settings, const FarView &view,
                                     Lookup &&loaded);

struct FarSelection final
{
    std::vector<FarQuadrantInstance> quadrants;
    // The key of each entry of `quadrants`, and the index of its tile in the
    // plan it was drawn from, for callers that need the tile.
    std::vector<FarTileKey> quadrant_tiles;
    std::vector<std::uint32_t> quadrant_plan;
    // The further layers of the drawn quadrants that have floating columns:
    // two instances (ground, undersides) per 16 x 16 block that has one.
    std::vector<FarQuadrantInstance> blocks;
    std::vector<FarTileKey> wanted;
    // Every loaded tile the walk passed through; these must stay loaded.
    std::vector<FarTileKey> used;
    bool roots_ready{true};
};
// What of a plan this view draws. `frustum_cull` false keeps everything in
// range (tests).
void emit_far_quadrants(const FarTerrainSettings &settings, const FarView &view,
                        const FarPlan &plan, bool frustum_cull, FarSelection &selection);
// Both steps at once.
template <typename Lookup>
[[nodiscard]] FarSelection select_far_tiles(const FarTerrainSettings &settings, const FarView &view,
                                            Lookup &&loaded, bool frustum_cull = true);

// --- The streaming cache -----------------------------------------------------

struct FarTerrainStats final
{
    std::size_t tiles_loaded{};
    std::size_t tile_capacity{};
    std::size_t tiles_pending{};
    // Cumulative since the world was entered.
    std::size_t tiles_built{};
    std::size_t tiles_from_disk{};
    std::size_t tiles_generated{};
    std::size_t tiles_uploaded{};
    std::size_t tiles_evicted{};
    double build_milliseconds_total{};
    double build_milliseconds_peak{};
    double generate_milliseconds_total{};
    // Columns evaluated by the generator (not served from disk).
    std::size_t columns_generated{};
    // Game-thread cost of update(), in milliseconds.
    double update_milliseconds_total{};
    double update_milliseconds_peak{};
    std::size_t updates{};
    std::size_t quadrants_drawn{};
    std::size_t boxes_drawn{};
    std::size_t memory_bytes{};
    std::size_t shadow_builds{};
    double shadow_milliseconds_total{};
};

// What drawing it costs, as the renderer measured it.
struct FarTerrainGpuStats final
{
    // Averages over the frames measured (OPENMINERZ_FAR_TRACE or
    // OPENMINERZ_POSTFX_TRACE), in milliseconds of GPU time.
    double geometry_milliseconds{};
    double composite_milliseconds{};
    double geometry_peak_milliseconds{};
    double composite_peak_milliseconds{};
    std::size_t frames{};
    // Game-thread time spent uploading cell maps and shadow fields.
    double upload_milliseconds_total{};
    double upload_milliseconds_peak{};
    std::size_t uploads{};
    std::size_t triangles_submitted{};
    // The far colour and depth targets, and the cell-map array.
    std::size_t target_bytes{};
    std::size_t array_bytes{};
};

// A sun (or moon) shadow field over the far landscape: for every texel the
// height below which the ground is in the shadow of terrain further toward the
// light. Two channels: [0] counts every caster at least a few cells away (what
// far terrain uses), [1] only casters beyond the reach of the detailed
// window's own shadow maps (what is laid over the detailed terrain so a
// mountain's evening shadow does not stop at the hand-over).
struct FarShadowMap final
{
    int size{};
    // World block coordinate of texel (0, 0)'s corner, and blocks per texel.
    int origin_x{};
    int origin_z{};
    int texel{};
    // size * size * 2 heights above the world's floor, in 1/64 block.
    std::vector<std::uint16_t> heights;
    std::array<float, 3> light{};
    std::uint32_t serial{};
};

struct FarTerrainFrame final
{
    bool active{};
    FarTerrainSettings settings{};
    // The cell-map array: how many slices, and a number that changes whenever
    // every slice has to be thrown away (a settings change, a new world).
    std::uint32_t slice_capacity{};
    std::uint32_t generation{};
    struct Upload final
    {
        std::uint32_t slice{};
        std::shared_ptr<const FarTile> tile;
    };
    std::vector<Upload> uploads;
    std::vector<FarQuadrantInstance> quadrants;
    // FarSelection::blocks: drawn after the quadrants, 16 x 16 cells each.
    std::vector<FarQuadrantInstance> blocks;
    std::vector<FarBoxInstance> boxes;
    std::array<std::int32_t, 3> box_origin{};
    // 0 while the coarsest tiles are still on their way, easing to 1.
    float opacity{};
    FarHandover handover{};
    // The newest shadow fields (fine, coarse); null until the first is built.
    std::array<std::shared_ptr<const FarShadowMap>, 2> shadows;
};

class FarTerrain final
{
  public:
    FarTerrain(std::int32_t seed, TerrainProfile profile, GameMode game_mode);
    // mod-world: from a generator as it is, a mod's terrain included (its
    // tiles are cached apart from retail's and other generators').
    explicit FarTerrain(const WorldGenerator &generator);
    ~FarTerrain();
    FarTerrain(const FarTerrain &) = delete;
    FarTerrain &operator=(const FarTerrain &) = delete;

    // Changing the settings drops every tile; they come back from the disk
    // cache at once.
    void set_settings(const FarTerrainSettings &settings);
    [[nodiscard]] const FarTerrainSettings &settings() const noexcept;

    // A block edit this client knows. Far tiles that sample the column are
    // rebuilt in the background.
    void set_edit(int world_x, int world_y, int world_z, BlockType block);
    // Forgets the edits of every column in the rectangle (inclusive): a chunk
    // whose edits are about to be replaced by a terrain authority's list.
    void erase_edits(int minimum_x, int minimum_z, int maximum_x, int maximum_z);
    void clear_edits();
    // mod-world (wave 5): the tiles of the generator's mod blocks (the client's
    // palette looks, ModPaletteLooks), so a mod's trees wear their own trunk
    // and leaves. Game thread; null until the looks are known.
    void set_mod_palette(std::shared_ptr<const std::vector<ModPaletteLook>> looks);

    // Once per drawn frame, on the game thread: picks what to draw from
    // `view`, hands out work, and admits at most `maximum_uploads` finished
    // tiles. `light` is the direction toward the sun or moon.
    [[nodiscard]] FarTerrainFrame update(const FarView &view, float visibility_distance,
                                         const std::array<float, 3> &light, double seconds,
                                         int maximum_uploads);
    // Blocks until nothing is pending for the last view (diagnostics, tests).
    void wait_until_idle();
    [[nodiscard]] FarTerrainStats stats() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// The directory the column summaries of a world are cached in:
// %LOCALAPPDATA%\OpenMinerZ\FarTerrain\<world>, or OPENMINERZ_FAR_TERRAIN_CACHE
// when that is set (tests and diagnostics). Empty when neither is available,
// which only turns the cache off.
[[nodiscard]] std::filesystem::path far_terrain_cache_directory(std::int32_t seed,
                                                                TerrainProfile profile,
                                                                GameMode game_mode);
// Whole-cache size limit, enforced when a world is entered by removing the
// worlds that were visited longest ago.
inline constexpr std::uintmax_t far_terrain_cache_limit_bytes = 768ULL * 1024ULL * 1024ULL;
// Enforces a limit on the cache that `current_world` (a directory returned by
// far_terrain_cache_directory) belongs to. The other worlds go first, whole,
// longest unvisited first. A world travelled far enough is over the limit on
// its own (about 66 MB at the spawn alone, and more with every kilometre), so
// what is then still too much comes out of the current world: its region
// files, least recently written first. Returns the bytes left.
// Never throws: an entry that vanishes while the cache is walked, as when a
// second instance prunes at the same moment, is skipped.
std::uintmax_t prune_far_terrain_cache(const std::filesystem::path &current_world,
                                       std::uintmax_t limit_bytes) noexcept;

// --- Selection (template body) ------------------------------------------------

namespace far_detail
{
// Nearest horizontal distance from a point to a tile's rectangle.
[[nodiscard]] float rectangle_distance(double x, double z, double minimum_x, double minimum_z,
                                       double size) noexcept;
void sort_wanted(std::vector<FarTileKey> &wanted, const FarTerrainSettings &settings,
                 const FarView &view);

// The view's four side planes, for testing a bounding sphere. Conservative:
// a sphere that touches the view is always kept.
struct Frustum final
{
    explicit Frustum(const FarView &view) noexcept
        : right(view.right), up(view.up), forward(view.forward),
          // A little wider than the picture, so a quadrant whose skirt or
          // morphing edge just reaches into view is there.
          tangent_x(view.tangent_x * 1.04F + 0.02F), tangent_y(view.tangent_y * 1.04F + 0.02F),
          side_x(std::sqrt(1.0F + tangent_x * tangent_x)),
          side_y(std::sqrt(1.0F + tangent_y * tangent_y))
    {
    }

    // (x, y, z) is the sphere's centre relative to the eye.
    [[nodiscard]] bool sphere(float x, float y, float z, float radius) const noexcept
    {
        const float depth = x * forward[0] + y * forward[1] + z * forward[2];
        if (depth < -radius)
            return false;
        const float across = x * right[0] + y * right[1] + z * right[2];
        if (std::abs(across) > depth * tangent_x + radius * side_x)
            return false;
        const float above = x * up[0] + y * up[1] + z * up[2];
        return std::abs(above) <= depth * tangent_y + radius * side_y;
    }

    std::array<float, 3> right;
    std::array<float, 3> up;
    std::array<float, 3> forward;
    float tangent_x;
    float tangent_y;
    float side_x;
    float side_y;
};
} // namespace far_detail

template <typename Lookup>
FarPlan plan_far_tiles(const FarTerrainSettings &settings, const FarView &view, Lookup &&loaded)
{
    FarPlan plan;
    if (!settings.enabled())
        return plan;
    const int top_level = far_top_level(settings);
    const double top_size = static_cast<double>(far_tile_size(settings, top_level));
    const float distance = static_cast<float>(settings.distance);
    const auto tile_distance = [&](const FarTileKey &key) {
        const double size = static_cast<double>(far_tile_size(settings, key.level));
        return far_detail::rectangle_distance(view.eye_x, view.eye_z,
                                              static_cast<double>(key.x) * size,
                                              static_cast<double>(key.z) * size, size);
    };
    const auto walk = [&](auto &&self, const FarTileKey &key, const FarLoadedTile &tile) -> void {
        // A level is drawn out to its range; nearer than the range of the
        // level below, a tile hands each quadrant to the child that covers
        // it, as soon as that child is ready.
        const bool refine =
            key.level > 0 && tile_distance(key) < far_level_range(settings, key.level - 1);
        const std::size_t slot = plan.tiles.size();
        plan.tiles.push_back({key, &tile, 0U});
        std::uint8_t own{};
        for (int quadrant = 0; quadrant < 4; ++quadrant)
        {
            if (refine)
            {
                const FarTileKey child = far_child(key, quadrant);
                const float child_distance = tile_distance(child);
                if (child_distance < far_level_range(settings, key.level - 1) &&
                    child_distance <= distance)
                {
                    if (const FarLoadedTile *ready = loaded(child))
                    {
                        self(self, child, *ready);
                        continue;
                    }
                    plan.wanted.push_back(child);
                }
            }
            own = static_cast<std::uint8_t>(own | (1U << quadrant));
        }
        plan.tiles[slot].quadrants = own;
    };
    const auto first = [&](double eye) {
        return static_cast<std::int32_t>(
            std::floor((eye - static_cast<double>(distance)) / top_size));
    };
    const auto last = [&](double eye) {
        return static_cast<std::int32_t>(
            std::floor((eye + static_cast<double>(distance)) / top_size));
    };
    for (std::int32_t z = first(view.eye_z); z <= last(view.eye_z); ++z)
        for (std::int32_t x = first(view.eye_x); x <= last(view.eye_x); ++x)
        {
            const FarTileKey key{top_level, x, z};
            if (tile_distance(key) > distance)
                continue;
            if (const FarLoadedTile *ready = loaded(key))
                walk(walk, key, *ready);
            else
            {
                plan.wanted.push_back(key);
                plan.roots_ready = false;
            }
        }
    far_detail::sort_wanted(plan.wanted, settings, view);
    return plan;
}

template <typename Lookup>
FarSelection select_far_tiles(const FarTerrainSettings &settings, const FarView &view,
                              Lookup &&loaded, bool frustum_cull)
{
    FarSelection selection;
    FarPlan plan = plan_far_tiles(settings, view, loaded);
    emit_far_quadrants(settings, view, plan, frustum_cull, selection);
    selection.used.reserve(plan.tiles.size());
    for (const FarPlan::Tile &tile : plan.tiles)
        selection.used.push_back(tile.key);
    selection.wanted = std::move(plan.wanted);
    selection.roots_ready = plan.roots_ready;
    return selection;
}

} // namespace openminerz::world
