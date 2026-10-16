// Far terrain (the Enhanced presentation's distant landscape) and the surface
// atmosphere that goes with it. Everything here is CPU-side and needs neither a
// graphics device nor the game's content: the single-column generator path
// against the full chunk generator, tile summaries, level selection and
// stitching, known edits, the disk cache, and the biome mist function.
//
// The disk cache is pointed at a temporary directory; nothing is written to
// %LOCALAPPDATA% or the repository.

#include "openminerz/world/far_terrain.hpp"
#include "openminerz/world/surface_atmosphere.hpp"
#include "openminerz/world/world.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace openminerz;
using namespace openminerz::world;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void set_environment(const char *name, const std::string &value)
{
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

class TemporaryDirectory final
{
  public:
    explicit TemporaryDirectory(const std::string &name)
        : path_(std::filesystem::temp_directory_path() / ("openminerz-far-terrain-" + name))
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

bool is_tree(BlockType type)
{
    return type == BlockType::log || type == BlockType::leaves;
}

// Chunks that between them stand in every band, every transition, the open
// pit past the descent, the mirrored outer rings, and negative coordinates.
std::vector<std::array<int, 2>> sample_chunks()
{
    std::vector<std::array<int, 2>> chunks;
    for (const int radius : {0,    100,  210,  250,  290,  600,  880,  950,  990,  1300, 1620,
                             1680, 2000, 2310, 2390, 2700, 2990, 3100, 3400, 3590, 3700, 4390,
                             4500, 5300, 5900, 6600, 7400, 8100, 8700, 9000})
    {
        chunks.push_back({radius / 16, 0});
        chunks.push_back({0, -radius / 16 - 1});
        const int diagonal = static_cast<int>(static_cast<double>(radius) * 0.70710678);
        chunks.push_back({-diagonal / 16 - 1, diagonal / 16});
    }
    // The tower's own chunks.
    chunks.push_back({-1, -1});
    chunks.push_back({-1, 0});
    return chunks;
}

void column_fast_path_matches_the_full_generator()
{
    struct Case final
    {
        std::int32_t seed;
        TerrainProfile profile;
        GameMode mode;
    };
    const std::array cases{
        Case{1337, TerrainProfile::classic, GameMode::endurance},
        Case{-90210, TerrainProfile::classic, GameMode::endurance},
        Case{1337, TerrainProfile::modern, GameMode::survival},
        Case{424242, TerrainProfile::modern, GameMode::scavenger},
        Case{7, TerrainProfile::modern, GameMode::endurance},
    };
    std::size_t columns{};
    std::size_t overhangs{};
    std::size_t lagoon_overhangs{};
    std::size_t tall_gaps{};
    std::size_t plain_gaps{};
    for (const Case &test : cases)
    {
        const WorldGenerator generator{test.seed, test.profile, test.mode};
        for (const auto &coordinate : sample_chunks())
        {
            const Chunk chunk = generator.generate_chunk(coordinate[0], coordinate[1]);
            for (int local_z = 0; local_z < chunk_depth; ++local_z)
                for (int local_x = 0; local_x < chunk_width; ++local_x)
                {
                    const int world_x = coordinate[0] * chunk_width + local_x;
                    const int world_z = coordinate[1] * chunk_depth + local_z;
                    // The real column, with the tree blocks taken back out: a
                    // tree only ever fills cells the terrain left empty.
                    std::array<BlockType, chunk_height> expected{};
                    SurfaceColumn expected_top{};
                    for (int y = 0; y < chunk_height; ++y)
                    {
                        const BlockType type = chunk.block(local_x, y, local_z);
                        expected[static_cast<std::size_t>(y)] =
                            is_tree(type) ? BlockType::empty : type;
                        if (expected[static_cast<std::size_t>(y)] != BlockType::empty &&
                            expected[static_cast<std::size_t>(y)] != BlockType::uninitialized)
                            expected_top = {static_cast<std::int16_t>(y),
                                            expected[static_cast<std::size_t>(y)]};
                    }
                    std::array<BlockType, chunk_height> column{};
                    generator.generate_column(world_x, world_z, column);
                    check(column == expected,
                          "generate_column differs from generate_chunk at " +
                              std::to_string(world_x) + "," + std::to_string(world_z));
                    const SurfaceColumn surface = generator.surface_column(world_x, world_z);
                    check(surface.top_local_y == expected_top.top_local_y &&
                              surface.top == expected_top.top,
                          "surface_column differs from the real top at " + std::to_string(world_x) +
                              "," + std::to_string(world_z) + ": got y " +
                              std::to_string(surface.top_local_y) + " block " +
                              std::to_string(static_cast<int>(surface.top)) + ", expected y " +
                              std::to_string(expected_top.top_local_y) + " block " +
                              std::to_string(static_cast<int>(expected_top.top)));
                    ++columns;
                    // An overhang column: the end of its top span, the air
                    // under it and the ground under that are the real ones.
                    const auto solid = [&](int y) {
                        return expected[static_cast<std::size_t>(y)] != BlockType::empty &&
                               expected[static_cast<std::size_t>(y)] != BlockType::uninitialized;
                    };
                    if (!surface.overhangs())
                    {
                        check(surface.under_local_y == -1 && surface.lower_local_y == -1 &&
                                  surface.lower == BlockType::empty,
                              "a column without an overhang reports none");
                        continue;
                    }
                    const std::string where =
                        " at " + std::to_string(world_x) + "," + std::to_string(world_z);
                    int y = surface.top_local_y;
                    while (y >= 0 && solid(y))
                        --y;
                    check(y > 0 && surface.under_local_y == y + 1,
                          "the underside of the top span differs from the real column" + where +
                              ": got " + std::to_string(surface.under_local_y) + ", expected " +
                              std::to_string(y + 1));
                    // The ground: a real top face with air over it, under that
                    // air. (A smaller piece hanging in between is passed over;
                    // most columns have none, and then the ground is simply
                    // the first cell under the air.)
                    const int air_top = y;
                    const int ground = surface.lower_local_y;
                    check(ground >= 0 && ground < air_top && solid(ground) && !solid(ground + 1) &&
                              surface.lower == expected[static_cast<std::size_t>(std::max(ground, 0))],
                          "the ground under an overhang differs from the real column" + where +
                              ": got y " + std::to_string(ground) + " block " +
                              std::to_string(static_cast<int>(surface.lower)));
                    while (y >= 0 && !solid(y))
                        --y;
                    check(ground <= y, "the ground is at or under the first cell beneath the air");
                    ++overhangs;
                    if (ground == y)
                        ++plain_gaps;
                    if (generator.biome_at(world_x, world_z) == BiomeKind::lagoon)
                    {
                        ++lagoon_overhangs;
                        if (air_top - ground >= 4)
                            ++tall_gaps;
                    }
                }
        }
        // Without the tower the origin is ordinary ground.
        const SurfaceColumn with_tower = generator.surface_column(0, 0, true);
        const SurfaceColumn without = generator.surface_column(0, 0, false);
        check(with_tower.top == BlockType::fixed_lantern && with_tower.top_local_y == 124,
              "the origin tower's lantern is the top of the origin column");
        check(without.top != BlockType::fixed_lantern && without.top != BlockType::bedrock &&
                  without.top_local_y < 124,
              "leaving the tower out reveals the ground under it");
        check(generator.surface_column(40, 40, false) == generator.surface_column(40, 40, true),
              "the tower switch changes nothing away from the origin");
    }
    check(columns > 100000U, "the column comparison covered enough columns");
    // The Lagoon's overhangs and islands are among them, gaps tall enough to
    // see included; a floating island never stands on a pillar.
    check(overhangs > 400U && lagoon_overhangs > 200U && tall_gaps > 100U &&
              plain_gaps * 2U > overhangs,
          "the column comparison covered overhang columns: " + std::to_string(overhangs) + ", " +
              std::to_string(lagoon_overhangs) + " in the Lagoon, " + std::to_string(tall_gaps) +
              " with a tall gap, " + std::to_string(plain_gaps) + " with nothing in the gap");
}

void tree_sites_are_the_generated_trees()
{
    const WorldGenerator generator{1337, TerrainProfile::classic, GameMode::endurance};
    std::size_t trees{};
    std::size_t quick{};
    for (int chunk_z = -6; chunk_z <= 6; ++chunk_z)
        for (int chunk_x = -6; chunk_x <= 6; ++chunk_x)
        {
            const Chunk chunk = generator.generate_chunk(chunk_x, chunk_z);
            std::vector<TreeSite> sites;
            generator.tree_sites(chunk_x, chunk_z, sites);
            for (const TreeSite &site : sites)
            {
                const int local_x = site.x - chunk_x * chunk_width;
                const int local_z = site.z - chunk_z * chunk_depth;
                check(local_x >= 3 && local_x < 13 && local_z >= 3 && local_z < 13,
                      "a tree stands in the middle of its chunk");
                check(chunk.block(local_x, site.ground_local_y, local_z) == BlockType::grass,
                      "a tree stands on grass");
                // (A trunk can be shorter in the chunk when an earlier tree's
                // leaves were in its way; it is never taller.)
                for (int offset = 0; offset < site.trunk_height; ++offset)
                {
                    const BlockType type =
                        chunk.block(local_x, site.ground_local_y + 1 + offset, local_z);
                    check(is_tree(type), "the reported trunk is tree in the chunk");
                }
            }
            // Every column of logs standing on grass is a reported tree.
            for (int local_z = 3; local_z < 13; ++local_z)
                for (int local_x = 3; local_x < 13; ++local_x)
                    for (int y = 1; y < chunk_height - 1; ++y)
                        if (chunk.block(local_x, y, local_z) == BlockType::grass &&
                            chunk.block(local_x, y + 1, local_z) == BlockType::log)
                        {
                            const bool reported =
                                std::any_of(sites.begin(), sites.end(), [&](const TreeSite &site) {
                                    return site.x == chunk_x * chunk_width + local_x &&
                                           site.z == chunk_z * chunk_depth + local_z;
                                });
                            check(reported, "a generated tree is missing from tree_sites");
                        }
            std::vector<TreeSite> fast;
            generator.tree_sites(chunk_x, chunk_z, fast, true);
            for (const TreeSite &site : fast)
            {
                const bool known = std::any_of(sites.begin(), sites.end(), [&](const TreeSite &full) {
                    return full.x == site.x && full.z == site.z &&
                           full.ground_local_y == site.ground_local_y;
                });
                check(known, "the quick tree list invents no tree");
            }
            trees += sites.size();
            quick += fast.size();
        }
    check(trees > 50U, "the plains have trees");
    check(quick * 10U >= trees * 9U, "the quick tree list finds nearly every tree");
}

void tiles_sample_the_generator()
{
    const WorldGenerator generator{1337, TerrainProfile::classic, GameMode::endurance};
    const FarTerrainSettings settings{4000, 1, 256};
    // A tile by the origin and one in the mountains, at three levels.
    for (const FarTileKey key : {FarTileKey{0, 0, -1}, FarTileKey{0, 30, 2}, FarTileKey{2, 7, -1},
                                 FarTileKey{4, 1, 0}, FarTileKey{4, -1, -1}})
    {
        const FarTileSummary summary = summarize_far_tile(generator, settings, key);
        for (int cell_z = -far_tile_low_apron; cell_z <= far_tile_cells; cell_z += 3)
            for (int cell_x = -far_tile_low_apron; cell_x <= far_tile_cells; cell_x += 5)
            {
                const auto column = far_cell_column(settings, key, cell_x, cell_z);
                const SurfaceColumn surface = generator.surface_column(column[0], column[1], false);
                const std::size_t index = far_grid_index(cell_x, cell_z);
                check(summary.height[index] == surface.top_local_y + 1 &&
                          summary.block[index] == static_cast<std::uint8_t>(surface.top),
                      "a tile cell is the generator's column at the cell's corner");
            }
        // A coarser tile holds every other column of the finer ones.
        if (key.level > 0)
            for (int quadrant = 0; quadrant < 4; ++quadrant)
            {
                const FarTileKey child = far_child(key, quadrant);
                check(far_parent(child) == key, "a child's parent is the tile it came from");
                const FarTileSummary fine = summarize_far_tile(generator, settings, child);
                for (int cell_z = 0; cell_z < far_tile_cells; cell_z += 2)
                    for (int cell_x = 0; cell_x < far_tile_cells; cell_x += 2)
                    {
                        const int parent_x = cell_x / 2 + (quadrant & 1) * far_quadrant_cells;
                        const int parent_z = cell_z / 2 + (quadrant >> 1) * far_quadrant_cells;
                        check(fine.height[far_grid_index(cell_x, cell_z)] ==
                                      summary.height[far_grid_index(parent_x, parent_z)] &&
                                  fine.block[far_grid_index(cell_x, cell_z)] ==
                                      summary.block[far_grid_index(parent_x, parent_z)],
                              "a parent cell is its child's even cell");
                    }
            }
        const auto tile = build_far_tile(generator, settings, key, summary, nullptr);
        check(tile->cells.size() == static_cast<std::size_t>(far_tile_grid * far_tile_grid) * 4U,
              "a tile's cell map is the whole stored grid");
        for (int quadrant = 0; quadrant < 4; ++quadrant)
            for (int cell_z = 0; cell_z < far_quadrant_cells; ++cell_z)
                for (int cell_x = 0; cell_x < far_quadrant_cells; ++cell_x)
                {
                    const std::uint8_t height =
                        tile->cells[far_grid_index(cell_x + (quadrant & 1) * far_quadrant_cells,
                                                   cell_z + (quadrant >> 1) * far_quadrant_cells) *
                                    4U];
                    check(height >= tile->minimum_height[static_cast<std::size_t>(quadrant)] &&
                              height <= tile->maximum_height[static_cast<std::size_t>(quadrant)],
                          "a quadrant's bounds contain its cells");
                }
        for (std::size_t index = 0U; index < tile->cells.size(); index += 4U)
        {
            const auto normal = far_cell_normal(tile->cells[index + 2U], tile->cells[index + 3U]);
            check(normal[1] >= 0.0F && std::abs(normal[0]) <= 1.0F && std::abs(normal[2]) <= 1.0F,
                  "a cell's slope is a normal that never points down");
            check((tile->cells[index + 2U] & 15U) <= 15U, "sky occlusion is four bits");
        }
        const bool has_trees = std::any_of(tile->trees.begin(), tile->trees.end(),
                                           [](const auto &list) { return !list.empty(); });
        check(far_cell_size(settings, key.level) <= far_tree_cell_limit || !has_trees,
              "coarse tiles carry no trees");
    }
    // The plains tile by the origin has trees, each inside its quadrant.
    const FarTileKey plains{0, 0, -1};
    const auto tile = build_far_tile(generator, settings, plains,
                                     summarize_far_tile(generator, settings, plains), nullptr);
    std::size_t trees{};
    for (int quadrant = 0; quadrant < 4; ++quadrant)
        for (const TreeSite &site : tile->trees[static_cast<std::size_t>(quadrant)])
        {
            const int local_x = site.x - plains.x * far_tile_cells;
            const int local_z = site.z - plains.z * far_tile_cells;
            check(local_x / far_quadrant_cells == (quadrant & 1) &&
                      local_z / far_quadrant_cells == (quadrant >> 1),
                  "a tree is listed in the quadrant it stands in");
            ++trees;
        }
    check(trees > 0U, "a plains tile lists its trees");

    // A slope is stored as its normal: a staircase rising toward +x leans -x.
    FarTileSummary ramp;
    for (int cell_z = -far_tile_low_apron; cell_z <= far_tile_cells; ++cell_z)
        for (int cell_x = -far_tile_low_apron; cell_x <= far_tile_cells; ++cell_x)
        {
            ramp.height[far_grid_index(cell_x, cell_z)] = static_cast<std::uint8_t>(20 + cell_x);
            ramp.block[far_grid_index(cell_x, cell_z)] = static_cast<std::uint8_t>(BlockType::rock);
        }
    const auto sloped = build_far_tile(generator, settings, FarTileKey{0, 500, 500}, ramp, nullptr);
    const std::size_t middle = far_grid_index(30, 30) * 4U;
    const auto normal = far_cell_normal(sloped->cells[middle + 2U], sloped->cells[middle + 3U]);
    check(std::abs(normal[0] + 0.7071F) < 0.04F && std::abs(normal[2]) < 0.04F &&
              std::abs(normal[1] - 0.7071F) < 0.04F,
          "a one-in-one slope is stored as a 45 degree normal");
    check((sloped->cells[middle + 2U] & 15U) < 15U && (sloped->cells[middle + 2U] & 15U) > 8U,
          "a hillside sees most, but not all, of the sky");
}

// A lookup in which every tile is ready, each with its own slice.
struct EverythingLoaded final
{
    mutable std::unordered_map<FarTileKey, FarLoadedTile, FarTileKeyHash> tiles;
    const std::set<std::array<int, 3>> *missing{};

    const FarLoadedTile *operator()(const FarTileKey &key) const
    {
        if (missing != nullptr && missing->contains({key.level, key.x, key.z}))
            return nullptr;
        auto found = tiles.find(key);
        if (found == tiles.end())
        {
            FarLoadedTile tile;
            tile.slice = static_cast<std::uint32_t>(tiles.size());
            tile.minimum_height = {60U, 60U, 60U, 60U};
            tile.maximum_height = {70U, 70U, 70U, 70U};
            found = tiles.emplace(key, tile).first;
        }
        return &found->second;
    }
};

struct DrawnQuadrant final
{
    double minimum_x{};
    double minimum_z{};
    double size{};
    int cell{};
    float morph_start{};
    float morph_scale{};
};

std::vector<DrawnQuadrant> drawn(const FarSelection &selection)
{
    std::vector<DrawnQuadrant> quadrants;
    for (const FarQuadrantInstance &instance : selection.quadrants)
    {
        const int cell = static_cast<int>(instance.packed & 0xFFFU);
        quadrants.push_back({static_cast<double>(instance.origin_x),
                             static_cast<double>(instance.origin_z),
                             static_cast<double>(cell * far_quadrant_cells), cell,
                             instance.morph_start, instance.morph_scale});
    }
    return quadrants;
}

float morph_at(const DrawnQuadrant &quadrant, double distance)
{
    return std::clamp((static_cast<float>(distance) - quadrant.morph_start) * quadrant.morph_scale,
                      0.0F, 1.0F);
}

void levels_cover_the_ground_once_and_meet_exactly()
{
    for (const FarTerrainSettings settings :
         {FarTerrainSettings{4000, 1, 256}, FarTerrainSettings{2000, 2, 192},
          FarTerrainSettings{8000, 1, 384}, FarTerrainSettings{1000, 1, 192}})
    {
        check(settings.ring_cells >= 192, "the morph band needs at least 192 cells of range");
        for (const std::array<double, 2> eye :
             {std::array{0.5, 0.5}, std::array{1888.3, -17.2}, std::array{-2600.9, 2599.1},
              std::array{63.999, 64.001}, std::array{100000.25, -70000.75}})
        {
            FarView view;
            view.eye_x = eye[0];
            view.eye_y = 30.0;
            view.eye_z = eye[1];
            view.clip_radius = 80.0F;
            const EverythingLoaded lookup;
            const FarSelection selection = select_far_tiles(settings, view, lookup, false);
            check(selection.wanted.empty() && selection.roots_ready,
                  "nothing is wanted when everything is loaded");
            const auto quadrants = drawn(selection);
            check(!quadrants.empty(), "something is drawn");

            // Every point of the ring between the hand-over and the far
            // distance is covered by exactly one quadrant.
            const double step = static_cast<double>(settings.distance) / 97.0;
            for (double z = eye[1] - settings.distance; z <= eye[1] + settings.distance; z += step)
                for (double x = eye[0] - settings.distance; x <= eye[0] + settings.distance;
                     x += step)
                {
                    const double distance = std::hypot(x - eye[0], z - eye[1]);
                    int covering{};
                    for (const DrawnQuadrant &quadrant : quadrants)
                        if (x >= quadrant.minimum_x && x < quadrant.minimum_x + quadrant.size &&
                            z >= quadrant.minimum_z && z < quadrant.minimum_z + quadrant.size)
                            ++covering;
                    check(covering <= 1, "two quadrants overlap");
                    // (A quadrant wholly inside the clip radius or beyond the
                    // far distance is left out; anything else must be there.)
                    if (distance > view.clip_radius + quadrants.front().size * 1.5 &&
                        distance < settings.distance * 0.98)
                        check(covering == 1, "a hole in the far terrain at distance " +
                                                 std::to_string(distance));
                }

            // Each level is only used inside its own range (plus the tile it
            // is part of), and its cells are never larger on screen than the
            // setting allows.
            for (const DrawnQuadrant &quadrant : quadrants)
            {
                const float nearest = far_detail::rectangle_distance(
                    eye[0], eye[1], quadrant.minimum_x, quadrant.minimum_z, quadrant.size);
                if (quadrant.cell > settings.base_cell)
                    check(nearest + 1.0F >=
                              static_cast<float>(settings.ring_cells) *
                                      static_cast<float>(quadrant.cell / 2) -
                                  static_cast<float>(quadrant.size * 2.0 * 1.4143),
                          "a coarse quadrant is drawn where a finer level belongs");
            }

            // Where two levels meet, the finer one has finished sliding onto
            // the coarser grid and the coarser one has not started: the
            // shared edge is the same geometry on both sides.
            std::size_t seams{};
            for (const DrawnQuadrant &fine : quadrants)
                for (const DrawnQuadrant &coarse : quadrants)
                {
                    if (coarse.cell <= fine.cell)
                        continue;
                    check(coarse.cell == fine.cell * 2 ||
                              !(fine.minimum_x <= coarse.minimum_x + coarse.size &&
                                coarse.minimum_x <= fine.minimum_x + fine.size &&
                                fine.minimum_z <= coarse.minimum_z + coarse.size &&
                                coarse.minimum_z <= fine.minimum_z + fine.size),
                          "neighbouring quadrants differ by more than one level");
                    // Shared edge: the overlap of the two closed rectangles
                    // when it is a segment.
                    const double x0 = std::max(fine.minimum_x, coarse.minimum_x);
                    const double x1 =
                        std::min(fine.minimum_x + fine.size, coarse.minimum_x + coarse.size);
                    const double z0 = std::max(fine.minimum_z, coarse.minimum_z);
                    const double z1 =
                        std::min(fine.minimum_z + fine.size, coarse.minimum_z + coarse.size);
                    const bool edge = (x0 == x1 && z1 > z0) || (z0 == z1 && x1 > x0);
                    if (!edge)
                        continue;
                    ++seams;
                    for (int sample = 0; sample <= 8; ++sample)
                    {
                        const double x = x0 + (x1 - x0) * sample / 8.0;
                        const double z = z0 + (z1 - z0) * sample / 8.0;
                        const double distance = std::hypot(x - eye[0], z - eye[1]);
                        check(morph_at(fine, distance) == 1.0F,
                              "the finer side of a seam has not reached the coarser grid");
                        check(morph_at(coarse, distance) == 0.0F,
                              "the coarser side of a seam has already started to move");
                    }
                }
            check(seams > 0U || far_top_level(settings) == 0, "levels meet somewhere");
        }
    }
}

void missing_tiles_fall_back_to_their_parents()
{
    const FarTerrainSettings settings{4000, 1, 256};
    FarView view;
    view.eye_x = 10.0;
    view.eye_z = 10.0;
    view.clip_radius = 80.0F;
    const int top = far_top_level(settings);
    // Nothing below the coarsest level is ready yet.
    std::set<std::array<int, 3>> missing;
    for (int level = 0; level < top; ++level)
        for (int z = -80; z < 80; ++z)
            for (int x = -80; x < 80; ++x)
                missing.insert({level, x, z});
    EverythingLoaded lookup;
    lookup.missing = &missing;
    const FarSelection coarse = select_far_tiles(settings, view, lookup, false);
    check(!coarse.quadrants.empty(), "the coarsest tiles are drawn while finer ones load");
    for (const DrawnQuadrant &quadrant : drawn(coarse))
        check(quadrant.cell == far_cell_size(settings, top), "only the coarsest level is drawn");
    check(!coarse.wanted.empty(), "the next level is asked for");
    for (std::size_t index = 0U; index < coarse.wanted.size(); ++index)
    {
        check(coarse.wanted[index].level == top - 1, "only direct children are asked for");
        if (index > 0U)
        {
            const auto distance = [&](const FarTileKey &key) {
                const double size = far_tile_size(settings, key.level);
                return far_detail::rectangle_distance(view.eye_x, view.eye_z, key.x * size,
                                                      key.z * size, size);
            };
            check(distance(coarse.wanted[index - 1U]) <= distance(coarse.wanted[index]),
                  "wanted tiles are ordered nearest first");
        }
    }
    // Coverage is still complete.
    const auto quadrants = drawn(coarse);
    for (double angle = 0.0; angle < 6.28; angle += 0.37)
        for (const double distance : {700.0, 1500.0, 3200.0})
        {
            const double x = view.eye_x + std::cos(angle) * distance;
            const double z = view.eye_z + std::sin(angle) * distance;
            const auto covering = std::count_if(quadrants.begin(), quadrants.end(),
                                                [&](const DrawnQuadrant &quadrant) {
                                                    return x >= quadrant.minimum_x &&
                                                           x < quadrant.minimum_x + quadrant.size &&
                                                           z >= quadrant.minimum_z &&
                                                           z < quadrant.minimum_z + quadrant.size;
                                                });
            check(covering == 1, "the fallback leaves a hole or an overlap");
        }
    // A tile that only just became ready starts on its parent's grid.
    EverythingLoaded fresh;
    FarLoadedTile young;
    young.ready_seconds = view.seconds;
    young.minimum_height = {60U, 60U, 60U, 60U};
    young.maximum_height = {70U, 70U, 70U, 70U};
    fresh.tiles.emplace(FarTileKey{0, 2, 2}, young);
    const FarSelection settled = select_far_tiles(settings, view, fresh, false);
    bool found{};
    for (std::size_t index = 0U; index < settled.quadrants.size(); ++index)
        if (settled.quadrant_tiles[index] == FarTileKey{0, 2, 2})
        {
            found = true;
            check(settled.quadrants[index].morph_floor == 1.0F,
                  "a new tile starts fully morphed onto its parent");
        }
        else if (settled.quadrant_tiles[index].level == 0)
            check(settled.quadrants[index].morph_floor == 0.0F, "a settled tile is itself");
    check(found, "the fresh tile is drawn");
}

void frustum_culling_only_removes_what_is_out_of_view()
{
    const FarTerrainSettings settings{4000, 1, 256};
    FarView view;
    view.eye_x = 0.0;
    view.eye_y = 40.0;
    view.eye_z = 0.0;
    view.forward = {0.0F, 0.0F, -1.0F};
    view.right = {1.0F, 0.0F, 0.0F};
    view.up = {0.0F, 1.0F, 0.0F};
    view.tangent_x = 1.2F;
    view.tangent_y = 0.75F;
    view.clip_radius = 80.0F;
    const EverythingLoaded lookup;
    const FarSelection all = select_far_tiles(settings, view, lookup, false);
    const FarSelection visible = select_far_tiles(settings, view, lookup, true);
    check(visible.quadrants.size() < all.quadrants.size() * 6U / 10U,
          "looking one way draws well under the whole ring");
    check(visible.quadrants.size() * 8U > all.quadrants.size(), "but not nothing");
    for (const DrawnQuadrant &quadrant : drawn(visible))
        check(quadrant.minimum_z < 0.0, "nothing behind the camera is drawn");
    // Everything whose centre is well inside the view survives.
    const auto kept = drawn(visible);
    for (const DrawnQuadrant &quadrant : drawn(all))
    {
        const double x = quadrant.minimum_x + quadrant.size * 0.5;
        const double z = quadrant.minimum_z + quadrant.size * 0.5;
        if (-z < 1.0 || std::abs(x) > -z * 1.0)
            continue;
        const bool present = std::any_of(kept.begin(), kept.end(), [&](const DrawnQuadrant &other) {
            return other.minimum_x == quadrant.minimum_x && other.minimum_z == quadrant.minimum_z &&
                   other.cell == quadrant.cell;
        });
        check(present, "a quadrant in view was culled");
    }
}

void settings_scale_the_work()
{
    check(!FarTerrainSettings{}.enabled(), "far terrain is off until a distance is set");
    check(far_tile_capacity(FarTerrainSettings{}) == 0, "and then holds no tiles");
    int previous{};
    for (const int distance : {1000, 2000, 4000, 8000, 12000})
    {
        const FarTerrainSettings settings{distance, 1, 256};
        const int capacity = far_tile_capacity(settings);
        check(capacity >= previous && capacity <= 2048, "a longer distance holds more tiles");
        check(far_level_range(settings, far_top_level(settings)) >= static_cast<float>(distance),
              "the coarsest level reaches the far distance");
        previous = capacity;
    }
    check(far_tile_capacity({4000, 2, 192}) < far_tile_capacity({4000, 1, 256}) &&
              far_tile_capacity({4000, 1, 256}) < far_tile_capacity({4000, 1, 384}),
          "a higher detail level holds more tiles");
    // The hand-over sits inside the detailed window for every view distance.
    for (int view_distance = 4; view_distance <= 32; ++view_distance)
    {
        const float visibility = static_cast<float>(std::clamp(view_distance * 4, 32, 128));
        const FarHandover handover = far_handover(visibility);
        check(handover.clip < handover.start && handover.start < handover.end,
              "the hand-over is an interval");
        check(handover.end <= visibility - 10.0F + 14.0F && handover.end <= 118.0F,
              "the hand-over ends inside what is meshed");
        check(handover.clip >= 6.0F, "far terrain never reaches the camera");
    }
}

// A column whose top span floats is not drawn standing on a pillar: its tile
// carries the span's underside and the ground beneath it, and its quadrant is
// drawn with the two further layers that show them.
void floating_columns_get_a_second_layer()
{
    const WorldGenerator generator{1337, TerrainProfile::classic, GameMode::endurance};
    const FarTerrainSettings settings{4000, 1, 256};
    // A tile of single-block cells in the Lagoon with an overhang in it.
    FarTileKey key{};
    FarTileSummary summary;
    bool found{};
    for (int tile_x = 6; tile_x <= 12 && !found; ++tile_x)
        for (int tile_z = -2; tile_z <= 2 && !found; ++tile_z)
        {
            key = {0, tile_x, tile_z};
            summary = summarize_far_tile(generator, settings, key);
            found = std::any_of(summary.under.begin(), summary.under.end(),
                                [](std::uint8_t under) { return under > 0U; });
        }
    check(found, "the Lagoon has overhangs");
    const auto tile = build_far_tile(generator, settings, key, summary, nullptr);
    check(tile->lower.size() == tile->cells.size() && tile->lower_quadrants != 0U,
          "a tile with an overhang carries a second layer");
    for (int cell_z = -far_tile_low_apron; cell_z <= far_tile_cells; ++cell_z)
        for (int cell_x = -far_tile_low_apron; cell_x <= far_tile_cells; ++cell_x)
        {
            const auto column = far_cell_column(settings, key, cell_x, cell_z);
            const SurfaceColumn surface = generator.surface_column(column[0], column[1], false);
            const std::size_t index = far_grid_index(cell_x, cell_z);
            const std::uint8_t *texel = tile->lower.data() + index * 4U;
            if (!surface.overhangs())
            {
                check(summary.under[index] == 0U && summary.lower_height[index] == 0U &&
                          summary.lower_block[index] == 0U && texel[0] == 0U && texel[1] == 0U,
                      "a column that is solid to the floor has no second layer");
                continue;
            }
            check(summary.under[index] == surface.under_local_y &&
                      summary.lower_height[index] == surface.lower_local_y + 1 &&
                      summary.lower_block[index] == static_cast<std::uint8_t>(surface.lower),
                  "a tile cell carries the generator's underside and lower ground");
            check(summary.lower_height[index] < summary.under[index] &&
                      summary.under[index] < summary.height[index],
                  "ground, air and the floating span stand in that order");
            // (Single-block cells: every gap is at least a cell tall.)
            check(texel[0] == summary.under[index] && texel[1] == summary.lower_height[index] &&
                      texel[2] == summary.lower_block[index],
                  "the second layer is the summary's");
            // (The outermost apron cells are only ever a parent to morph onto.)
            if (cell_x < -1 || cell_z < -1)
                continue;
            const int quadrant = (std::clamp(cell_x, 0, far_tile_cells - 1) / far_quadrant_cells) +
                                 (std::clamp(cell_z, 0, far_tile_cells - 1) / far_quadrant_cells) * 2;
            check((tile->lower_quadrants & (1U << quadrant)) != 0U &&
                      tile->minimum_height[static_cast<std::size_t>(quadrant)] <= texel[1],
                  "the quadrant of a floating column is marked and bounded by its ground");
        }

    // A coarser level keeps a gap only while it is a cell of its own tall.
    FarTileSummary made;
    made.height.fill(80U);
    made.block.fill(static_cast<std::uint8_t>(BlockType::rock));
    const std::size_t shallow = far_grid_index(10, 10);
    const std::size_t deep = far_grid_index(40, 40);
    made.under[shallow] = 70U;
    made.lower_height[shallow] = 67U;
    made.lower_block[shallow] = static_cast<std::uint8_t>(BlockType::sand);
    made.under[deep] = 70U;
    made.lower_height[deep] = 66U;
    made.lower_block[deep] = static_cast<std::uint8_t>(BlockType::sand);
    const FarTileKey coarse{2, 40, 40};
    check(far_cell_size(settings, coarse.level) == 4, "the made tile has four-block cells");
    const auto layered = build_far_tile(generator, settings, coarse, made, nullptr);
    check(layered->lower.size() == layered->cells.size(), "the made tile has a second layer");
    check(layered->lower[shallow * 4U] == 0U && layered->lower[shallow * 4U + 1U] == 0U,
          "a gap shorter than a cell is drawn as a solid column");
    check(layered->lower[deep * 4U] == 70U && layered->lower[deep * 4U + 1U] == 66U &&
              layered->lower[deep * 4U + 2U] == static_cast<std::uint8_t>(BlockType::sand),
          "a gap a cell tall is kept");
    check(layered->lower_quadrants == 8U && layered->minimum_height[3] == 66U &&
              layered->minimum_height[0] == 80U && layered->cells[deep * 4U] == 80U,
          "only the quadrant with the gap is layered; the top layer is unchanged");
    // (Cell 40, 40 is cell 8, 8 of the last quadrant: its first block.)
    check(layered->lower_blocks == (1U << 12U), "and of that quadrant only the block with the gap");
    made.under[deep] = 0U;
    made.lower_height[deep] = 0U;
    const auto solid = build_far_tile(generator, settings, coarse, made, nullptr);
    check(solid->lower.empty() && solid->lower_quadrants == 0U,
          "a tile without a floating column has no second layer");

    // Drawn: every quadrant once as before, and the block with the gap twice
    // more.
    FarLoadedTile info;
    info.slice = 5U;
    info.minimum_height = layered->minimum_height;
    info.maximum_height = layered->maximum_height;
    info.lower_quadrants = layered->lower_quadrants;
    info.lower_blocks = layered->lower_blocks;
    FarPlan plan;
    plan.tiles.push_back({coarse, &info, 15U});
    FarView view;
    view.eye_x = 40.0 * 256.0 + 128.0;
    view.eye_y = 100.0;
    view.eye_z = 40.0 * 256.0 + 128.0;
    FarSelection selection;
    emit_far_quadrants(settings, view, plan, false, selection);
    check(selection.quadrants.size() == 4U && selection.quadrant_tiles.size() == 4U &&
              selection.quadrant_plan.size() == 4U && selection.blocks.size() == 2U,
          "four quadrants, and two more layers for the block with the gap");
    for (const FarQuadrantInstance &instance : selection.quadrants)
    {
        const unsigned quadrant = (instance.packed >> 24U) & 3U;
        check(((instance.packed >> 28U) & 3U) == 0U && ((instance.packed >> 26U) & 1U) == 1U &&
                  ((instance.packed >> 11U) & 1U) == 0U && (instance.packed >> 31U) == 0U,
              "a quadrant is the columns' own layer, with its trees");
        check(((instance.packed >> 30U) & 1U) == (quadrant == 3U ? 1U : 0U),
              "only the quadrant with the gap reads the second layer");
    }
    unsigned layers{};
    for (const FarQuadrantInstance &instance : selection.blocks)
    {
        check(((instance.packed >> 24U) & 3U) == 3U && ((instance.packed >> 12U) & 0xFFFU) == 5U &&
                  (instance.packed & 0x7FFU) == 4U && ((instance.packed >> 30U) & 1U) == 1U,
              "a block is a piece of the quadrant with the gap, in the tile's slice");
        check(((instance.packed >> 11U) & 1U) == 0U && (instance.packed >> 31U) == 0U &&
                  ((instance.packed >> 26U) & 1U) == 0U,
              "it is the block the gap is in, and draws no trees");
        layers |= 1U << ((instance.packed >> 28U) & 3U);
    }
    check(layers == 6U, "the ground under the floating columns, and their undersides");

    // Something built in the air above the land floats as well.
    const FarTileKey plain_key{0, 1, 1};
    const FarTileSummary plain = summarize_far_tile(generator, settings, plain_key);
    const std::size_t spot = far_grid_index(20, 21);
    check(plain.under[spot] == 0U, "the plains by the origin do not overhang");
    const int ground = plain.height[spot];
    FarEditOverlay edits;
    edits.set(far_tile_cells + 20, ground + 10 + world_min_y, far_tile_cells + 21,
              BlockType::wood);
    const auto platform = build_far_tile(generator, settings, plain_key, plain, &edits);
    check(platform->cells[spot * 4U] == ground + 11 &&
              platform->lower.size() == platform->cells.size() &&
              platform->lower[spot * 4U] == ground + 10 &&
              platform->lower[spot * 4U + 1U] == ground &&
              platform->lower[spot * 4U + 2U] == plain.block[spot],
          "a block placed in the air floats over the ground it was placed above");
}

void known_edits_show_in_far_tiles()
{
    const WorldGenerator generator{1337, TerrainProfile::classic, GameMode::endurance};
    const FarTerrainSettings settings{4000, 1, 256};
    const FarTileKey key{0, 3, 3};
    const FarTileSummary summary = summarize_far_tile(generator, settings, key);
    const auto plain = build_far_tile(generator, settings, key, summary, nullptr);

    FarEditOverlay edits;
    check(!edits.touches(0, 0, 100000, 100000), "no edits, nothing touched");
    const int column_x = 3 * far_tile_cells + 20;
    const int column_z = 3 * far_tile_cells + 21;
    const std::size_t index = far_grid_index(20, 21) * 4U;
    const int ground = plain->cells[index]; // top face, local
    // A tower built on the column.
    for (int local_y = ground; local_y < ground + 30; ++local_y)
        edits.set(column_x, local_y + world_min_y, column_z, BlockType::wood);
    check(edits.column_count() == 1U, "the edits are one column");
    check(edits.touches(column_x - 5, column_z - 5, column_x + 5, column_z + 5) &&
              !edits.touches(column_x + 200, column_z, column_x + 300, column_z + 50),
          "the overlay knows where its edits are");
    const auto built = build_far_tile(generator, settings, key, summary, &edits);
    check(built->cells[index] == ground + 30 &&
              built->cells[index + 1U] == static_cast<std::uint8_t>(BlockType::wood),
          "a built tower raises its column");
    check(built->cells[far_grid_index(40, 40) * 4U] == plain->cells[far_grid_index(40, 40) * 4U],
          "and leaves the others alone");
    check(built->maximum_height[0] >= ground + 30, "the quadrant's bounds follow the edit");

    // A shaft dug straight down from the top lowers it to the real block below.
    FarEditOverlay dug;
    for (int local_y = ground - 6; local_y < ground; ++local_y)
        dug.set(column_x, local_y + world_min_y, column_z, BlockType::empty);
    std::array<BlockType, chunk_height> column{};
    generator.generate_column(column_x, column_z, column);
    int expected = ground - 6;
    while (expected > 0 && column[static_cast<std::size_t>(expected - 1)] == BlockType::empty)
        --expected;
    const auto lowered = build_far_tile(generator, settings, key, summary, &dug);
    check(lowered->cells[index] == expected &&
              lowered->cells[index + 1U] ==
                  static_cast<std::uint8_t>(column[static_cast<std::size_t>(expected - 1)]),
          "a dug shaft shows the generated block at its bottom");

    // A torch is not a column.
    FarEditOverlay torch;
    torch.set(column_x, ground + world_min_y, column_z, BlockType::torch);
    const auto lit = build_far_tile(generator, settings, key, summary, &torch);
    check(lit->cells[index] == ground, "a torch does not raise the far terrain");

    // A coarse tile only shows an edit on a column it samples.
    const FarTileKey coarse{2, 0, 0};
    const FarTileSummary coarse_summary = summarize_far_tile(generator, settings, coarse);
    FarEditOverlay off_grid;
    off_grid.set(212 + 1, 60, 216, BlockType::wood);
    const auto untouched = build_far_tile(generator, settings, coarse, coarse_summary, &off_grid);
    const auto reference = build_far_tile(generator, settings, coarse, coarse_summary, nullptr);
    check(untouched->cells == reference->cells, "an edit between sample columns changes nothing");
    FarEditOverlay on_grid;
    on_grid.set(212, 60, 216, BlockType::wood);
    const auto touched = build_far_tile(generator, settings, coarse, coarse_summary, &on_grid);
    check(touched->cells[far_grid_index(53, 54) * 4U] == 60 - world_min_y + 1,
          "an edit on a sampled column shows at that level");
}

FarView overhead_view(double x, double z)
{
    FarView view;
    view.eye_x = x;
    view.eye_y = 60.0;
    view.eye_z = z;
    return view;
}

void streaming_is_progressive_bounded_and_cached()
{
    const TemporaryDirectory directory{"cache"};
    set_environment("OPENMINERZ_FAR_TERRAIN_CACHE", directory.path().string());
    const FarTerrainSettings settings{2000, 1, 192};
    const std::array<float, 3> light{0.5F, 0.4F, 0.3F};
    std::map<std::array<int, 3>, std::vector<std::uint8_t>> first_tiles;
    FarTerrainStats first_stats;
    {
        FarTerrain terrain{1337, TerrainProfile::classic, GameMode::endurance};
        terrain.set_settings(settings);
        check(terrain.settings() == settings, "the settings are taken as given");
        // The first frame has nothing to draw and asks for the coarsest tiles.
        FarTerrainFrame frame = terrain.update(overhead_view(0.0, 0.0), 128.0F, light, 0.0, 4);
        check(frame.active && frame.quadrants.empty() && frame.opacity == 0.0F,
              "nothing is drawn before the first tile arrives");
        check(frame.slice_capacity == static_cast<std::uint32_t>(far_tile_capacity(settings)),
              "the frame states how many slices the renderer needs");
        // Then tiles arrive a bounded number per frame, coarsest first.
        int top_level_seen = -1;
        double seconds{};
        std::size_t frames{};
        std::set<std::uint32_t> slices;
        while (frames < 200000U)
        {
            seconds += 1.0 / 60.0;
            frame = terrain.update(overhead_view(0.0, 0.0), 128.0F, light, seconds, 3);
            ++frames;
            check(frame.uploads.size() <= 3U, "no more tiles per frame than the budget");
            for (const auto &upload : frame.uploads)
            {
                check(upload.slice < frame.slice_capacity, "a slice inside the array");
                check(slices.insert(upload.slice).second, "a slice is handed out once");
                if (top_level_seen < 0)
                    top_level_seen = upload.tile->key.level;
                first_tiles[{upload.tile->key.level, upload.tile->key.x, upload.tile->key.z}] =
                    upload.tile->cells;
            }
            const FarTerrainStats stats = terrain.stats();
            if (stats.tiles_pending == 0U && frame.uploads.empty() && frames > 30U &&
                stats.tiles_loaded > 0U)
                break;
            if (frame.uploads.empty())
                std::this_thread::sleep_for(std::chrono::microseconds{200});
        }
        check(top_level_seen == far_top_level(settings), "the coarsest level arrives first");
        check(frame.opacity == 1.0F, "the landscape has faded in");
        check(!frame.quadrants.empty(), "and is drawn");
        check(!frame.boxes.empty(), "with its trees and the origin tower");
        std::size_t anchored_crowns{};
        for (const auto &box : frame.boxes)
            if (box.fade_end > 0)
            {
                const int anchor = box.flags >> 1;
                check(anchor > 0 && anchor <= box.minimum_y,
                      "distant tree has no shared ground anchor");
                if (box.side_tile == 15)
                    check(anchor == box.minimum_y, "trunk does not shrink into its root");
                if (box.side_tile == 17 && box.minimum_y > anchor)
                    ++anchored_crowns;
            }
        check(anchored_crowns > 0, "no crown/trunk transition was exercised");
        first_stats = terrain.stats();
        check(first_stats.tiles_generated == first_stats.tiles_built &&
                  first_stats.tiles_from_disk == 0U,
              "a new world is generated, not read");
        check(first_stats.tiles_loaded <= first_stats.tile_capacity, "within the tile budget");
        // (A tile with floating columns carries a second map of the same size.)
        const std::size_t map_bytes = static_cast<std::size_t>(far_tile_grid * far_tile_grid) * 8U;
        check(first_stats.memory_bytes >= first_stats.tiles_loaded * map_bytes &&
                  first_stats.memory_bytes < first_stats.tiles_loaded * map_bytes * 2U &&
                  first_stats.memory_bytes % map_bytes == 0U,
              "memory is the cell maps, once here and once on the GPU");
        terrain.wait_until_idle();

        // A long flight never holds more than the budget, and what is drawn
        // always follows the camera.
        for (int step = 1; step <= 60; ++step)
        {
            seconds += 0.25;
            const double x = step * 160.0;
            frame = terrain.update(overhead_view(x, x * 0.3), 128.0F, light, seconds, 6);
            const FarTerrainStats stats = terrain.stats();
            check(stats.tiles_loaded <= stats.tile_capacity, "the flight stayed within budget");
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        terrain.wait_until_idle();
        frame = terrain.update(overhead_view(60 * 160.0, 60 * 48.0), 128.0F, light, seconds + 5.0, 6);
        check(frame.opacity > 0.0F && !frame.quadrants.empty(), "terrain is drawn at the far end");
        check(terrain.stats().tiles_evicted > 0U, "tiles left behind were given up");

        // The shadow field arrives, and its far-caster channel never claims
        // more shadow than the one that counts every caster.
        for (int attempt = 0; attempt < 4000 && frame.shadows[0] == nullptr; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            seconds += 0.5;
            frame = terrain.update(overhead_view(60 * 160.0, 60 * 48.0), 128.0F, light, seconds, 6);
        }
        check(frame.shadows[0] != nullptr && frame.shadows[1] != nullptr,
              "the shadow fields are built");
        for (const auto &shadow : frame.shadows)
        {
            check(shadow->heights.size() ==
                      static_cast<std::size_t>(shadow->size * shadow->size) * 2U,
                  "a shadow field has two channels");
            // (Each line of the sweep blends its neighbours, so a caster seen
            // through many lines is a little lower than one seen through few;
            // the comparison allows for that softening.)
            double all{};
            double distant{};
            std::size_t higher{};
            for (std::size_t texel = 0U; texel < shadow->heights.size(); texel += 2U)
            {
                all += shadow->heights[texel];
                distant += shadow->heights[texel + 1U];
                if (shadow->heights[texel + 1U] > shadow->heights[texel] + 6U * 64U)
                    ++higher;
            }
            check(all > 0.0, "the low sun casts shadows somewhere");
            check(distant <= all, "distant casters alone shade less than all casters");
            check(higher * 200U < shadow->heights.size() / 2U,
                  "distant casters alone are never much higher than all casters");
        }
    }
    // The same world again: every tile comes from disk and is identical.
    {
        FarTerrain terrain{1337, TerrainProfile::classic, GameMode::endurance};
        terrain.set_settings(settings);
        double seconds{};
        std::size_t compared{};
        for (int frame_index = 0; frame_index < 200000; ++frame_index)
        {
            seconds += 1.0 / 60.0;
            const FarTerrainFrame frame =
                terrain.update(overhead_view(0.0, 0.0), 128.0F, light, seconds, 8);
            for (const auto &upload : frame.uploads)
            {
                const auto found =
                    first_tiles.find({upload.tile->key.level, upload.tile->key.x, upload.tile->key.z});
                check(found != first_tiles.end() && found->second == upload.tile->cells,
                      "a tile read back from disk differs from the generated one");
                ++compared;
            }
            const FarTerrainStats stats = terrain.stats();
            if (stats.tiles_pending == 0U && frame.uploads.empty() && frame_index > 30 &&
                stats.tiles_loaded > 0U)
                break;
            if (frame.uploads.empty())
                std::this_thread::sleep_for(std::chrono::microseconds{200});
        }
        const FarTerrainStats stats = terrain.stats();
        check(compared > 20U, "the second visit loaded the same tiles");
        check(stats.tiles_generated == 0U && stats.tiles_from_disk == stats.tiles_built,
              "a place visited before is read from disk, not generated");
        // An edit rebuilds the tiles that sample its column, in place.
        const std::size_t uploaded = stats.tiles_uploaded;
        terrain.set_edit(128, 90, 128, BlockType::wood);
        terrain.wait_until_idle();
        check(terrain.stats().tiles_uploaded > uploaded, "an edit re-uploads the tiles it changes");
        check(terrain.stats().tiles_loaded == stats.tiles_loaded,
              "and neither adds nor drops a tile");
    }
    // Another seed is another directory.
    check(far_terrain_cache_directory(1337, TerrainProfile::classic, GameMode::endurance) !=
              far_terrain_cache_directory(1338, TerrainProfile::classic, GameMode::endurance) &&
              far_terrain_cache_directory(1337, TerrainProfile::classic, GameMode::endurance) !=
                  far_terrain_cache_directory(1337, TerrainProfile::modern, GameMode::endurance),
          "every world has its own cache");
    check(far_terrain_cache_directory(1337, TerrainProfile::classic, GameMode::endurance)
                  .parent_path() == directory.path(),
          "the cache lives where it was told to");
    set_environment("OPENMINERZ_FAR_TERRAIN_CACHE", "off");
    check(far_terrain_cache_directory(1337, TerrainProfile::classic, GameMode::endurance).empty(),
          "the cache can be turned off");
    set_environment("OPENMINERZ_FAR_TERRAIN_CACHE", directory.path().string());
}

// The size limit. It used to be enforced only by deleting OTHER worlds, so one
// world travelled far enough grew without bound; and the walk used throwing
// iterators on a worker thread, where a folder vanishing mid-walk (a second
// instance pruning) ended the process.
void the_cache_limit_holds_inside_one_world_too()
{
    const TemporaryDirectory directory{"prune"};
    const auto write = [](const std::filesystem::path &file, std::size_t bytes, int hours_old) {
        std::filesystem::create_directories(file.parent_path());
        {
            std::ofstream out{file, std::ios::binary | std::ios::trunc};
            const std::string block(bytes, 'x');
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() -
                                                   std::chrono::hours{hours_old});
    };
    const auto exists = [](const std::filesystem::path &file) {
        std::error_code ignored;
        return std::filesystem::exists(file, ignored);
    };
    const std::filesystem::path old_world = directory.path() / "classic-0-1";
    const std::filesystem::path older_world = directory.path() / "classic-0-9";
    const std::filesystem::path current = directory.path() / "classic-0-2";
    const std::filesystem::path stranger = directory.path() / "not-a-cache";
    write(older_world / "c1" / "0_0.rgn", 4000U, 0);
    write(older_world / "last-used", 8U, 96);
    write(old_world / "c1" / "0_0.rgn", 4000U, 0);
    write(old_world / "last-used", 8U, 48);
    write(current / "c1" / "0_0.rgn", 3000U, 30);
    write(current / "c1" / "1_0.rgn", 3000U, 20);
    write(current / "c2" / "0_0.rgn", 3000U, 10);
    write(current / "last-used", 8U, 0);
    write(stranger / "keep.bin", 50000U, 500);

    // 17 024 bytes of cache. One world too many: the one visited longest ago.
    std::uintmax_t left = prune_far_terrain_cache(current, 14000U);
    check(!exists(older_world) && exists(old_world / "c1" / "0_0.rgn") && left == 13016U,
          "the world visited longest ago goes first, and only it");
    // Still over with every other world gone: nothing of the current one yet.
    left = prune_far_terrain_cache(current, 9008U);
    check(!exists(old_world) && left == 9008U && exists(current / "c1" / "0_0.rgn") &&
              exists(current / "c1" / "1_0.rgn") && exists(current / "c2" / "0_0.rgn"),
          "the other worlds go before anything of the current one");
    // The current world alone over the limit: its least recently written files.
    left = prune_far_terrain_cache(current, 6500U);
    check(!exists(current / "c1" / "0_0.rgn") && exists(current / "c1" / "1_0.rgn") &&
              exists(current / "c2" / "0_0.rgn") && left == 6008U,
          "a world over the limit on its own gives up its oldest region file");
    left = prune_far_terrain_cache(current, 100U);
    check(!exists(current / "c1" / "1_0.rgn") && !exists(current / "c2" / "0_0.rgn") &&
              exists(current / "last-used") && left == 8U,
          "and as many as it takes, but never the marker");
    check(exists(stranger / "keep.bin"), "a folder the cache did not make is left alone");
    // Nothing to walk is not an error, and nothing is thrown.
    check(prune_far_terrain_cache(directory.path() / "missing" / "classic-0-3", 1U) == 0U,
          "a cache that does not exist is empty");
}

// --- Atmosphere ---------------------------------------------------------------

bool close(float left, float right, float tolerance)
{
    return std::abs(left - right) <= tolerance * std::max({1.0F, std::abs(left), std::abs(right)});
}

// The atmosphere round an eye, with far terrain drawn to 4 km.
SurfaceAtmosphere air_at(float eye_x, float eye_z)
{
    return surface_atmosphere(std::hypot(eye_x, eye_z), 128.0F, 4000.0F, 1.0F);
}

// The same integral by brute force.
AirPath marched(const SurfaceAtmosphere &air, const std::array<float, 3> &eye,
                const std::array<float, 3> &direction, float length)
{
    AirPath path;
    constexpr int steps = 40000;
    const double step = static_cast<double>(length) / steps;
    double clear{};
    double mist{};
    for (int index = 0; index < steps; ++index)
    {
        // The field itself, point by point: what the light-shaft pass marches.
        const double s = (index + 0.5) * step;
        const std::array<float, 2> density =
            atmosphere_density(air, static_cast<float>(eye[0] + direction[0] * s),
                               static_cast<float>(eye[1] + direction[1] * s),
                               static_cast<float>(eye[2] + direction[2] * s));
        clear += static_cast<double>(density[0]) * step;
        mist += static_cast<double>(density[1]) * step;
    }
    path.air = static_cast<float>(clear);
    path.mist = static_cast<float>(mist);
    return path;
}

std::array<float, 3> toward(const std::array<float, 3> &from, const std::array<float, 3> &to,
                            float &length)
{
    const std::array<float, 3> delta{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    length = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    return {delta[0] / length, delta[1] / length, delta[2] / length};
}

void mist_belongs_to_the_first_biome()
{
    // The radial profile: all of it on the plains, none in any other band.
    check(biome_mist_weight(0.0F) == 1.0F && biome_mist_weight(150.0F) == 1.0F,
          "the plains are in the mist");
    check(biome_mist_weight(245.0F) > 0.4F && biome_mist_weight(245.0F) < 0.6F,
          "the mist thins across the band's edge");
    for (const float radius : {330.0F, 600.0F, 1300.0F, 2000.0F, 2700.0F, 3300.0F, 4400.0F, 6000.0F})
        check(biome_mist_weight(radius) == 0.0F, "no mist in the lagoon, desert, mountains or arctic");
    check(biome_mist_weight(8800.0F) == 1.0F && biome_mist_weight(8700.0F) == 1.0F &&
              biome_mist_weight(9200.0F) == 0.0F && biome_mist_weight(8400.0F) == 0.0F,
          "the plains and their mist come round again where the world repeats");
    for (float radius = 0.0F; radius < 9500.0F; radius += 1.0F)
        check(std::abs(biome_mist_weight(radius + 1.0F) - biome_mist_weight(radius)) < 0.011F,
              "the mist has no edge");
    // The discs add up to that profile.
    for (const float eye_radius : {0.0F, 2000.0F, 8600.0F})
    {
        const auto discs = mist_discs(eye_radius);
        const float centre = std::round(eye_radius / 8800.0F) * 8800.0F;
        for (float offset = -400.0F; offset <= 400.0F; offset += 2.5F)
        {
            const float radius = centre + offset;
            if (radius < 0.0F)
                continue;
            float weight{};
            for (const MistDisc &disc : discs)
                if (radius < disc.radius)
                    weight += disc.weight;
            check(std::abs(weight - biome_mist_weight(radius)) < 0.26F,
                  "the discs follow the band's profile at radius " + std::to_string(radius));
        }
    }
}

void air_paths_are_exact_and_behave()
{
    // Closed form against a brute-force march, from inside and outside the
    // plains, up, down and level, short and kilometres long.
    struct Ray final
    {
        std::array<float, 3> eye;
        std::array<float, 3> target;
    };
    const std::array rays{
        Ray{{10.0F, 8.0F, -20.0F}, {150.0F, 9.0F, 60.0F}},      // through a valley
        Ray{{10.0F, 40.0F, -20.0F}, {150.0F, 4.0F, 60.0F}},     // down into it
        Ray{{60.0F, 36.0F, 2.0F}, {-1900.0F, 70.0F, 100.0F}},   // plains to a mountain peak
        Ray{{1888.0F, 75.0F, 0.0F}, {0.0F, 20.0F, 0.0F}},       // mountain to the plains
        Ray{{1888.0F, 75.0F, 0.0F}, {3000.0F, 10.0F, 800.0F}},  // mountain to the arctic
        Ray{{2600.0F, 20.0F, 0.0F}, {2600.0F, 30.0F, 2500.0F}}, // across the arctic
        Ray{{-500.0F, 20.0F, 500.0F}, {600.0F, 25.0F, -400.0F}},// lagoon, across the plains
        Ray{{50.0F, 5.0F, 50.0F}, {50.0F, 90.0F, 50.01F}},      // straight up
        Ray{{8650.0F, 12.0F, 0.0F}, {9300.0F, 14.0F, 300.0F}},  // the repeated plains
        Ray{{7000.0F, 60.0F, 0.0F}, {10500.0F, 30.0F, 100.0F}}, // through the repeated ring
    };
    for (const Ray &ray : rays)
    {
        const SurfaceAtmosphere air = air_at(ray.eye[0], ray.eye[2]);
        float length{};
        const auto direction = toward(ray.eye, ray.target, length);
        const AirPath exact = air_path(air, ray.eye, direction, length);
        const AirPath reference = marched(air, ray.eye, direction, length);
        check(close(exact.air, reference.air, 0.002F), "clear-air path matches the march");
        check(close(exact.mist, reference.mist, 0.004F) ||
                  std::abs(exact.mist - reference.mist) < 0.004F,
              "mist path matches the march: " + std::to_string(exact.mist) + " vs " +
                  std::to_string(reference.mist));
        // More path, never less air.
        const AirPath half = air_path(air, ray.eye, direction, length * 0.5F);
        check(half.air <= exact.air + 1.0e-6F && half.mist <= exact.mist + 1.0e-5F,
              "air only accumulates along a path");
        check(air_path(air, ray.eye, direction, 0.0F).air == 0.0F &&
                  air_path(air, ray.eye, direction, 0.0F).mist == 0.0F,
              "no path, no air");
    }

    const SurfaceAtmosphere plains = air_at(0.0F, 0.0F);
    // Standing in a valley of the plains the far side of the valley is in
    // mist; from a hilltop above the layer the same distance is nearly clear.
    const AirPath valley = air_path(plains, {0.0F, 6.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 150.0F);
    const AirPath hilltop = air_path(plains, {0.0F, 48.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 150.0F);
    check(std::exp(-valley.mist) < 0.60F && std::exp(-valley.mist) > 0.25F,
          "a valley of the plains is misty but not a wall");
    check(std::exp(-hilltop.mist) > 0.80F && hilltop.mist < valley.mist * 0.25F,
          "a hilltop stands above the valley mist, in the lighter haze");
    // Mist stops at the band: looking outward from its edge there is none.
    const AirPath outward = air_path(plains, {330.0F, 6.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 3000.0F);
    check(outward.mist == 0.0F, "no mist beyond the first biome");
    // In the mountains and the arctic the air is clear for kilometres: only
    // the thin aerial term, which leaves a peak two kilometres off distinct.
    for (const std::array<float, 3> eye :
         {std::array{1900.0F, 70.0F, 0.0F}, std::array{0.0F, 30.0F, 2650.0F}})
    {
        const SurfaceAtmosphere air = air_at(eye[0], eye[2]);
        // Tangent to the origin circle, so the ray never crosses the plains.
        const float radius = std::hypot(eye[0], eye[2]);
        const std::array<float, 3> tangent{-eye[2] / radius, 0.02F, eye[0] / radius};
        const float norm = std::sqrt(1.0F + 0.0004F);
        const std::array<float, 3> direction{tangent[0] / norm, tangent[1] / norm,
                                             tangent[2] / norm};
        const AirPath path = air_path(air, eye, direction, 2000.0F);
        check(path.mist == 0.0F, "clear air in the mountains and the arctic");
        check(std::exp(-path.air) > 0.30F && std::exp(-path.air) < 0.75F,
              "two kilometres of clear air leave a peak pale but distinct");
        check(std::exp(-air_path(air, eye, direction, 100.0F).air) > 0.93F,
              "and what is close by nearly untouched");
    }
    // From the mountains the plains are a bank of mist: a ray down into them
    // meets it, one over them does not.
    const SurfaceAtmosphere mountain = air_at(1900.0F, 0.0F);
    float length{};
    const std::array<float, 3> summit{1900.0F, 80.0F, 0.0F};
    const AirPath into = air_path(mountain, summit, toward(summit, {0.0F, 4.0F, 0.0F}, length), 1901.0F);
    const AirPath over = air_path(mountain, summit, toward(summit, {-1900.0F, 80.0F, 0.0F}, length), 3800.0F);
    check(into.mist > 0.15F, "the plains show as mist from the mountains");
    check(over.mist < into.mist * 0.25F, "a ray above the valley mist passes over most of it");
    // Higher is clearer for both media.
    for (float height = 0.0F; height < 100.0F; height += 5.0F)
    {
        const AirPath low = air_path(plains, {0.0F, height, 0.0F}, {0.0F, 0.0F, 1.0F}, 200.0F);
        const AirPath high = air_path(plains, {0.0F, height + 5.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, 200.0F);
        check(high.mist <= low.mist && high.air <= low.air, "air thins with height");
    }
    // Crossing the band's edge changes the mist smoothly for the eye and for
    // what is looked at.
    float previous = air_path(air_at(0.0F, 0.0F), {0.0F, 8.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 0.5F).mist;
    for (float distance = 1.0F; distance < 500.0F; distance += 0.5F)
    {
        const float mist =
            air_path(air_at(0.0F, 0.0F), {0.0F, 8.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, distance).mist;
        check(mist >= previous && mist - previous < 0.0042F, "mist builds up without a step");
        previous = mist;
    }
    previous = air_path(air_at(100.0F, 0.0F), {100.0F, 8.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, 80.0F).mist;
    for (float x = 101.0F; x < 500.0F; x += 1.0F)
    {
        const float mist = air_path(air_at(x, 0.0F), {x, 8.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, 80.0F).mist;
        check(std::abs(mist - previous) < 0.08F, "walking out of the plains has no fog wall");
        previous = mist;
    }
    check(previous == 0.0F, "and ends in clear air");
}

// More distance, never more light: nothing may look clearer than what stands
// in front of it on the same line of sight, in any biome or blend of two,
// whatever the view's reach.
void transmittance_only_falls_along_a_ray()
{
    std::uint32_t state = 0x1234ABCDU;
    const auto random = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8U) / 16777216.0F;
    };
    constexpr std::array reaches{0.0F, 2000.0F, 4000.0F, 8000.0F, 12000.0F};
    int rays{};
    for (int sample = 0; sample < 4000; ++sample)
    {
        // Eyes everywhere, and most of them where the first biome ends.
        const float radius = sample % 3 == 0   ? random() * 9600.0F
                             : sample % 3 == 1 ? 120.0F + random() * 260.0F
                                               : 8400.0F + random() * 800.0F;
        const float bearing = random() * 6.2831853F;
        const std::array<float, 3> eye{radius * std::cos(bearing), -8.0F + random() * 130.0F,
                                       radius * std::sin(bearing)};
        const float heading = random() * 6.2831853F;
        const float pitch = (random() - 0.5F) * 1.2F;
        const std::array<float, 3> direction{std::cos(pitch) * std::cos(heading), std::sin(pitch),
                                             std::cos(pitch) * std::sin(heading)};
        const float far_reach = reaches[static_cast<std::size_t>(sample) % reaches.size()];
        const SurfaceAtmosphere air = surface_atmosphere(radius, 64.0F + random() * 128.0F,
                                                         far_reach, random());
        float previous = 1.0F;
        float previous_depth = 0.0F;
        for (float length = 0.25F; length < 60000.0F; length *= 1.31F)
        {
            const AirPath path = air_path(air, eye, direction, length);
            const float depth = path.air + path.mist;
            check(path.air >= 0.0F && path.mist >= 0.0F, "optical depth is never negative");
            check(depth >= previous_depth - 2.0e-4F * std::max(1.0F, depth),
                  "optical depth only grows along a ray: " + std::to_string(previous_depth) +
                      " then " + std::to_string(depth) + " at " + std::to_string(length));
            check(path.transmittance() <= previous + 1.0e-4F,
                  "nothing is clearer than what stands in front of it");
            previous = path.transmittance();
            previous_depth = depth;
        }
        ++rays;
    }
    check(rays == 4000, "every ray was walked");
    // The field itself is never negative, which is why.
    for (int sample = 0; sample < 20000; ++sample)
    {
        const float eye_radius = random() * 9600.0F;
        const SurfaceAtmosphere air = surface_atmosphere(eye_radius, 128.0F, 4000.0F, 1.0F);
        const std::array<float, 2> density =
            atmosphere_density(air, (random() - 0.5F) * 20000.0F, -64.0F + random() * 300.0F,
                               (random() - 0.5F) * 20000.0F);
        check(density[0] >= 0.0F && density[1] >= 0.0F, "the air is never less than nothing");
    }
}

// A point is fogged the same whichever renderer path draws it: the detailed
// pass and the far pass store its distance differently and must find the
// same air in front of it.
void a_point_has_one_fog_on_every_path()
{
    constexpr float detailed_near = 0.1F;
    constexpr float detailed_far = 1000.0F;
    constexpr float landscape_near = 1.0F;
    const std::array<float, 3> eye{60.0F, 36.0F, 2.0F};
    const SurfaceAtmosphere air = air_at(eye[0], eye[2]);
    // A camera looking west and slightly down.
    const std::array<float, 3> forward{-0.9962F, -0.0872F, 0.0F};
    float worst{};
    for (const std::array<float, 3> point :
         {std::array{20.0F, 30.0F, 10.0F}, std::array{-26.0F, 21.0F, -40.0F},
          std::array{-50.0F, 8.0F, 30.0F}, std::array{-180.0F, 40.0F, 90.0F},
          std::array{-300.0F, 12.0F, -200.0F}, std::array{-420.0F, 60.0F, 15.0F},
          std::array{-55.0F, 100.0F, 0.0F}, std::array{-610.0F, 3.0F, 300.0F}})
    {
        float distance{};
        const std::array<float, 3> direction = toward(eye, point, distance);
        const float camera_depth = distance * (direction[0] * forward[0] +
                                               direction[1] * forward[1] +
                                               direction[2] * forward[2]);
        check(camera_depth > 1.0F, "the point is in front of the camera");
        // Per unit of camera depth the ray is this long.
        const float ray_length = distance / camera_depth;
        const float through_detailed =
            near_view_depth(near_depth_value(camera_depth, detailed_near, detailed_far),
                            detailed_near, detailed_far) *
            ray_length;
        const float through_landscape =
            far_view_depth(far_depth_value(camera_depth, landscape_near), landscape_near) *
            ray_length;
        const float exact = air_path(air, eye, direction, distance).transmittance();
        const float detailed = air_path(air, eye, direction, through_detailed).transmittance();
        const float landscape = air_path(air, eye, direction, through_landscape).transmittance();
        worst = std::max({worst, std::abs(detailed - landscape), std::abs(detailed - exact)});
        check(std::abs(detailed - landscape) < 0.004F,
              "the detailed pass and the far pass fog one point alike: " +
                  std::to_string(detailed) + " vs " + std::to_string(landscape));
        check(std::abs(landscape - exact) < 0.002F, "and as its true distance says");
    }
    check(worst < 0.004F, "no path disagrees");
}

// The clear air is as thick as the view is short, and that one rule both
// lets the far landscape recede into the horizon and buries the edge of the
// detailed window when there is no landscape beyond it.
void the_air_ends_the_view()
{
    const auto level = [](const SurfaceAtmosphere &air, float eye_y, float length) {
        return air_path(air, {2000.0F, eye_y, 0.0F}, {0.0F, 0.0F, 1.0F}, length).transmittance();
    };
    const auto overhead = [](const SurfaceAtmosphere &air, float eye_y) {
        return air_path(air, {2000.0F, eye_y, 0.0F}, {0.0F, 1.0F, 0.0F}, 60000.0F).transmittance();
    };
    for (const float reach : {2000.0F, 4000.0F, 8000.0F, 12000.0F})
    {
        // (In the mountains: clear air only.)
        const SurfaceAtmosphere air = surface_atmosphere(2000.0F, 128.0F, reach, 1.0F);
        check(close(level(air, 20.0F, reach), air_left_at_far_edge, 0.01F),
              "a tenth of the light is left where the far landscape ends");
        check(level(air, 20.0F, 300.0F) > level(air, 20.0F, 1000.0F) + 0.04F &&
                  level(air, 20.0F, 1000.0F) > level(air, 20.0F, reach) + 0.04F,
              "a ridge a kilometre off is paler than one at 300 blocks, the last of it paler still");
        check(level(air, 20.0F, 30.0F) > 0.96F, "what is close by is clear");
        check(overhead(air, 20.0F) > 0.55F, "the sky overhead stays the sky");
    }
    // Snow peaks 2 km off still read at the default distance.
    check(level(surface_atmosphere(2000.0F, 128.0F, 4000.0F, 1.0F), 20.0F, 2000.0F) > 0.25F,
          "a peak two kilometres away keeps a quarter of its contrast");
    // Without far terrain the view ends at the window: the air closes in over
    // the last third of it and leaves what is nearer as it is with far
    // terrain on.
    const SurfaceAtmosphere with_far = surface_atmosphere(2000.0F, 128.0F, 4000.0F, 1.0F);
    for (const float window : {64.0F, 128.0F, 180.0F})
    {
        SurfaceAtmosphere air = surface_atmosphere(2000.0F, window, 0.0F, 0.0F);
        for (const float eye_y : {4.0F, 20.0F, 36.0F, 90.0F})
        {
            air.edge_ceiling = closing_air_ceiling(eye_y);
            check(level(air, eye_y, window) < air_left_at_window_edge + 0.002F,
                  "the edge of the detailed window is buried");
            for (float share = 0.05F; share <= 0.5F; share += 0.05F)
                check(std::abs(level(air, eye_y, window * share) -
                               level(with_far, eye_y, window * share)) < 0.09F,
                      "the near half of the view looks as it does with far terrain on");
            check(level(air, eye_y, window * 0.66F) > 0.60F,
                  "two thirds of the way out the land is still plainly seen");
            float previous = 1.0F;
            for (float length = 1.0F; length < window * 1.5F; length += 1.0F)
            {
                const float left = level(air, eye_y, length);
                check(left <= previous && previous - left < 0.045F,
                      "and from there it closes in without a step");
                previous = left;
            }
        }
        air.edge_ceiling = closing_air_ceiling(36.0F);
        check(overhead(air, 36.0F) > 0.90F, "the sky straight up stays the sky");
        // Half way up it is too, and with the longest view twenty degrees up.
        check(air_path(air, {2000.0F, 36.0F, 0.0F}, {0.0F, 0.7071F, 0.7071F}, 60000.0F)
                      .transmittance() > 0.85F,
              "and so is the sky half way up");
        if (window >= 128.0F)
            check(air_path(air, {2000.0F, 36.0F, 0.0F}, {0.0F, 0.342F, 0.940F}, 60000.0F)
                          .transmittance() > 0.60F,
                  "and the sky a hand above the horizon");
        // A hilltop at the window's edge, seen from a valley floor, is inside
        // the closing air with the rest.
        float length{};
        const std::array<float, 3> eye{2000.0F, 6.0F, 0.0F};
        air.edge_ceiling = closing_air_ceiling(eye[1]);
        const auto direction = toward(eye, {2000.0F, 58.0F, window}, length);
        check(air_path(air, eye, direction, length).transmittance() < 0.08F,
              "high ground at the edge is closed in too");
    }
    // Ground, leaves and a creature under the sky all take the whole of it.
    for (const float skylight : {0.30F, 0.55F, 0.80F, 1.0F})
        check(atmosphere_exposure(1.0F, skylight) == 1.0F && atmosphere_exposure(0.0F, skylight) == 1.0F,
              "every surface under the sky is fogged alike");
    check(atmosphere_exposure(0.0F, 0.0F) == 0.0F, "a cave seen from a cave has no sky's air");
    // While the far landscape fades in, the air clears with it and never jumps.
    float previous = surface_atmosphere(0.0F, 128.0F, 4000.0F, 0.0F).air.density;
    for (float presence = 0.0F; presence <= 1.0F; presence += 0.05F)
    {
        const float density = surface_atmosphere(0.0F, 128.0F, 4000.0F, presence).air.density;
        check(density <= previous + 1.0e-7F && previous - density < 0.0015F,
              "the air clears evenly as far terrain arrives");
        previous = density;
    }
}

} // namespace

int main(int argc, char **argv)
{
    // `--surface <seed> <x> <z> [half-width] [step]`: prints the generated
    // top height (world y) around a point, for choosing diagnostic cameras.
    if (argc >= 5 && std::string{argv[1]} == "--surface")
    {
        const WorldGenerator generator{std::atoi(argv[2])};
        const int centre_x = std::atoi(argv[3]);
        const int centre_z = std::atoi(argv[4]);
        const int half = argc > 5 ? std::atoi(argv[5]) : 32;
        const int step = argc > 6 ? std::max(std::atoi(argv[6]), 1) : 8;
        for (int z = centre_z - half; z <= centre_z + half; z += step)
        {
            for (int x = centre_x - half; x <= centre_x + half; x += step)
            {
                // An overhang column as top/underside/ground.
                const SurfaceColumn column = generator.surface_column(x, z);
                std::cout << (column.top_local_y + world_min_y + 1);
                if (column.overhangs())
                    std::cout << '/' << (column.under_local_y + world_min_y) << '/'
                              << (column.lower_local_y + world_min_y + 1);
                std::cout << '\t';
            }
            std::cout << '\n';
        }
        return 0;
    }
    struct Test final
    {
        const char *name;
        void (*run)();
    };
    constexpr std::array tests{
        Test{"column fast path matches the full generator",
             column_fast_path_matches_the_full_generator},
        Test{"tree sites are the generated trees", tree_sites_are_the_generated_trees},
        Test{"tiles sample the generator", tiles_sample_the_generator},
        Test{"levels cover the ground once and meet exactly",
             levels_cover_the_ground_once_and_meet_exactly},
        Test{"missing tiles fall back to their parents", missing_tiles_fall_back_to_their_parents},
        Test{"frustum culling only removes what is out of view",
             frustum_culling_only_removes_what_is_out_of_view},
        Test{"settings scale the work", settings_scale_the_work},
        Test{"floating columns get a second layer", floating_columns_get_a_second_layer},
        Test{"known edits show in far tiles", known_edits_show_in_far_tiles},
        Test{"streaming is progressive, bounded and cached",
             streaming_is_progressive_bounded_and_cached},
        Test{"the cache limit holds inside one world too",
             the_cache_limit_holds_inside_one_world_too},
        Test{"mist belongs to the first biome", mist_belongs_to_the_first_biome},
        Test{"air paths are exact and behave", air_paths_are_exact_and_behave},
        Test{"transmittance only falls along a ray", transmittance_only_falls_along_a_ray},
        Test{"a point has one fog on every path", a_point_has_one_fog_on_every_path},
        Test{"the air ends the view", the_air_ends_the_view},
    };
    int failures{};
    for (const Test &test : tests)
    {
        try
        {
            test.run();
            std::cout << "ok   " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0)
        std::cout << failures << " far terrain test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
