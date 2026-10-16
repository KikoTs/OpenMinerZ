#include "openminerz/world/far_terrain.hpp"

#include "openminerz/world/custom_terrain.hpp" // mod-world

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace openminerz::world
{
namespace
{

using Clock = std::chrono::steady_clock;

double milliseconds_since(Clock::time_point start) noexcept
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string environment_value(const char *name)
{
#if defined(_WIN32)
    char *value{};
    std::size_t size{};
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        return {};
    std::string result{value};
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::string{};
#endif
}

constexpr std::uint64_t column_key(int world_x, int world_z) noexcept
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(world_x)) << 32U) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(world_z));
}

// Edits are indexed by 64-block squares so a tile can tell at once whether
// any of them concerns it.
constexpr int edit_region_shift = 6;
static_assert(far_tile_cells == 64, "tile indices are computed with a shift of six");

constexpr int floor_shift(int value, int shift) noexcept
{
    return value >> shift; // arithmetic on every supported compiler
}

} // namespace

int far_tile_capacity(const FarTerrainSettings &settings) noexcept
{
    if (!settings.enabled())
        return 0;
    const int top_level = far_top_level(settings);
    const int reach = (settings.ring_cells + far_tile_cells - 1) / far_tile_cells;
    const int per_level = (2 * reach + 2) * (2 * reach + 2);
    const int top_size = far_tile_size(settings, top_level);
    const int top_reach = (settings.distance + top_size - 1) / top_size;
    const int top_tiles = (2 * top_reach + 2) * (2 * top_reach + 2);
    const int needed = per_level * top_level + top_tiles;
    // A quarter more for tiles arriving while the ones they replace are still
    // drawn. A D3D11 texture array holds at most 2048 slices.
    return std::clamp(needed + needed / 4 + 16, 64, 2048);
}

// --- Edits -------------------------------------------------------------------

struct FarEditOverlay::Impl final
{
    mutable std::shared_mutex mutex;
    std::unordered_map<std::uint64_t, std::vector<std::array<std::uint8_t, 2>>> columns;
    std::unordered_map<std::uint64_t, std::uint32_t> regions;
};

FarEditOverlay::FarEditOverlay() : impl_(std::make_unique<Impl>())
{
}

FarEditOverlay::~FarEditOverlay() = default;

void FarEditOverlay::set(int world_x, int world_y, int world_z, BlockType block)
{
    const int local_y = world_y - world_min_y;
    if (local_y < 0 || local_y >= chunk_height)
        return;
    const std::unique_lock lock{impl_->mutex};
    auto &column = impl_->columns[column_key(world_x, world_z)];
    if (column.empty())
        ++impl_->regions[column_key(floor_shift(world_x, edit_region_shift),
                                    floor_shift(world_z, edit_region_shift))];
    for (auto &entry : column)
        if (entry[0] == static_cast<std::uint8_t>(local_y))
        {
            entry[1] = static_cast<std::uint8_t>(block);
            return;
        }
    column.push_back({static_cast<std::uint8_t>(local_y), static_cast<std::uint8_t>(block)});
}

std::vector<std::array<int, 2>> FarEditOverlay::erase(int minimum_x, int minimum_z, int maximum_x,
                                                      int maximum_z)
{
    std::vector<std::array<int, 2>> erased;
    const std::unique_lock lock{impl_->mutex};
    for (int z = minimum_z; z <= maximum_z; ++z)
        for (int x = minimum_x; x <= maximum_x; ++x)
        {
            if (impl_->columns.erase(column_key(x, z)) == 0U)
                continue;
            erased.push_back({x, z});
            const auto region = impl_->regions.find(
                column_key(floor_shift(x, edit_region_shift), floor_shift(z, edit_region_shift)));
            if (region != impl_->regions.end() && --region->second == 0U)
                impl_->regions.erase(region);
        }
    return erased;
}

void FarEditOverlay::clear()
{
    const std::unique_lock lock{impl_->mutex};
    impl_->columns.clear();
    impl_->regions.clear();
}

std::size_t FarEditOverlay::column_count() const
{
    const std::shared_lock lock{impl_->mutex};
    return impl_->columns.size();
}

bool FarEditOverlay::column(int world_x, int world_z,
                            std::vector<std::array<std::uint8_t, 2>> &edits) const
{
    const std::shared_lock lock{impl_->mutex};
    const auto found = impl_->columns.find(column_key(world_x, world_z));
    if (found == impl_->columns.end())
        return false;
    edits = found->second;
    return true;
}

bool FarEditOverlay::touches(int minimum_x, int minimum_z, int maximum_x, int maximum_z) const
{
    const std::shared_lock lock{impl_->mutex};
    if (impl_->regions.empty())
        return false;
    const int first_x = floor_shift(minimum_x, edit_region_shift);
    const int first_z = floor_shift(minimum_z, edit_region_shift);
    const int last_x = floor_shift(maximum_x, edit_region_shift);
    const int last_z = floor_shift(maximum_z, edit_region_shift);
    const std::uint64_t area = static_cast<std::uint64_t>(last_x - first_x + 1) *
                               static_cast<std::uint64_t>(last_z - first_z + 1);
    if (area <= impl_->regions.size())
    {
        for (int z = first_z; z <= last_z; ++z)
            for (int x = first_x; x <= last_x; ++x)
                if (impl_->regions.contains(column_key(x, z)))
                    return true;
        return false;
    }
    for (const auto &[key, count] : impl_->regions)
    {
        const int x = static_cast<int>(static_cast<std::uint32_t>(key >> 32U));
        const int z = static_cast<int>(static_cast<std::uint32_t>(key));
        if (count > 0U && x >= first_x && x <= last_x && z >= first_z && z <= last_z)
            return true;
    }
    return false;
}

// --- Building a tile ---------------------------------------------------------

FarTileSummary summarize_far_tile(const WorldGenerator &generator,
                                  const FarTerrainSettings &settings, const FarTileKey &key)
{
    FarTileSummary summary;
    for (int cell_z = -far_tile_low_apron; cell_z < far_tile_cells + far_tile_high_apron; ++cell_z)
        for (int cell_x = -far_tile_low_apron; cell_x < far_tile_cells + far_tile_high_apron;
             ++cell_x)
        {
            const auto column = far_cell_column(settings, key, cell_x, cell_z);
            const SurfaceColumn surface = generator.surface_column(column[0], column[1], false);
            const std::size_t index = far_grid_index(cell_x, cell_z);
            summary.height[index] = static_cast<std::uint8_t>(surface.top_local_y + 1);
            // mod-world: a mod block, or (fix) a mod biome's own layering.
            summary.block[index] = surface.top_profile != 0U
                                       ? far_profile_byte(surface.top_profile)
                                       : far_block_byte(surface.top, surface.top_mod);
            if (surface.overhangs())
            {
                // (The underside of cell y is y above the floor; the top of
                // cell y is y + 1.)
                summary.under[index] = static_cast<std::uint8_t>(surface.under_local_y);
                summary.lower_height[index] = static_cast<std::uint8_t>(surface.lower_local_y + 1);
                summary.lower_block[index] = surface.lower_profile != 0U
                                                 ? far_profile_byte(surface.lower_profile)
                                                 : far_block_byte(surface.lower, surface.lower_mod);
            }
        }
    return summary;
}

std::array<float, 3> far_cell_normal(std::uint8_t byte2, std::uint8_t byte3) noexcept
{
    const int packed_x = ((byte2 >> 4) << 2) | (byte3 & 3);
    const int packed_z = byte3 >> 2;
    const float x = static_cast<float>(packed_x - 32) / 31.0F;
    const float z = static_cast<float>(packed_z - 32) / 31.0F;
    const float y = std::sqrt(std::max(0.0F, 1.0F - x * x - z * z));
    return {x, y, z};
}

std::shared_ptr<const FarTile> build_far_tile(const WorldGenerator &generator,
                                              const FarTerrainSettings &settings,
                                              const FarTileKey &key, FarTileSummary summary,
                                              const FarEditOverlay *edits)
{
    const int cell = far_cell_size(settings, key.level);
    const auto first = far_cell_column(settings, key, -far_tile_low_apron, -far_tile_low_apron);
    const auto last = far_cell_column(settings, key, far_tile_cells, far_tile_cells);

    // Edits this client knows. A column that has any is generated in full so
    // the block under a dug-out top is the real one.
    if (edits != nullptr && edits->touches(first[0], first[1], last[0], last[1]))
    {
        std::vector<std::array<std::uint8_t, 2>> column_edits;
        std::array<BlockType, chunk_height> column{};
        for (int cell_z = -far_tile_low_apron; cell_z < far_tile_cells + far_tile_high_apron;
             ++cell_z)
            for (int cell_x = -far_tile_low_apron; cell_x < far_tile_cells + far_tile_high_apron;
                 ++cell_x)
            {
                const auto world = far_cell_column(settings, key, cell_x, cell_z);
                // The origin tower is drawn as its own object.
                if (std::abs(world[0]) <= 5 && std::abs(world[1]) <= 5)
                    continue;
                if (!edits->column(world[0], world[1], column_edits))
                    continue;
                generator.generate_column(world[0], world[1], column);
                for (const auto &edit : column_edits)
                    column[edit[0]] = static_cast<BlockType>(edit[1]);
                const std::size_t index = far_grid_index(cell_x, cell_z);
                // Whether the land itself overhangs here, before the edits.
                const bool natural_overhang = summary.under[index] > 0U;
                const int natural_top = summary.height[index];
                const int natural_ground =
                    natural_overhang ? summary.lower_height[index] : natural_top;
                summary.height[index] = 0U;
                summary.block[index] = static_cast<std::uint8_t>(BlockType::empty);
                summary.under[index] = 0U;
                summary.lower_height[index] = 0U;
                summary.lower_block[index] = 0U;
                // Torches and doors are thin models, not columns.
                const auto solid = [&](int y) {
                    const BlockType type = column[static_cast<std::size_t>(y)];
                    return type != BlockType::empty && type != BlockType::uninitialized &&
                           renders_as_voxel(type);
                };
                int y = chunk_height - 1;
                while (y >= 0 && !solid(y))
                    --y;
                if (y < 0)
                    continue;
                summary.height[index] = static_cast<std::uint8_t>(y + 1);
                summary.block[index] = static_cast<std::uint8_t>(column[static_cast<std::size_t>(y)]);
                // The end of the top span and the ground under the air below
                // it: where the land overhangs anyway, or where the span is
                // something built above the land (a bridge, a platform). Air
                // that a cave leaves under ordinary ground stays unreported,
                // as it is for the columns nobody has touched.
                while (y >= 0 && solid(y))
                    --y;
                const int under = y + 1;
                if (y < 0 || !(natural_overhang || under >= natural_top))
                    continue;
                // The ground is where the land's own ground was, or whatever
                // now stands on it; a piece hanging between is passed over,
                // as the generator's summary passes it over.
                int ground = std::min(y, natural_ground - 1);
                while (ground >= 0 && !solid(ground))
                    --ground;
                if (ground < 0)
                    continue;
                while (ground + 1 < y && solid(ground + 1))
                    ++ground;
                summary.under[index] = static_cast<std::uint8_t>(under);
                summary.lower_height[index] = static_cast<std::uint8_t>(ground + 1);
                summary.lower_block[index] =
                    static_cast<std::uint8_t>(column[static_cast<std::size_t>(ground)]);
            }
    }

    auto tile = std::make_shared<FarTile>();
    tile->key = key;
    tile->cells.resize(static_cast<std::size_t>(far_tile_grid * far_tile_grid) * 4U);
    const auto height_at = [&](int grid_x, int grid_z) {
        grid_x = std::clamp(grid_x, 0, far_tile_grid - 1);
        grid_z = std::clamp(grid_z, 0, far_tile_grid - 1);
        return static_cast<float>(
            summary.height[static_cast<std::size_t>(grid_z * far_tile_grid + grid_x)]);
    };
    constexpr std::array<std::array<int, 2>, 8> directions{
        {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};
    const float inverse_cell = 1.0F / static_cast<float>(cell);
    for (int grid_z = 0; grid_z < far_tile_grid; ++grid_z)
        for (int grid_x = 0; grid_x < far_tile_grid; ++grid_x)
        {
            const std::size_t index = static_cast<std::size_t>(grid_z * far_tile_grid + grid_x);
            const float height = height_at(grid_x, grid_z);
            // The surface normal from the heights either side.
            const int low_x = std::max(grid_x - 1, 0);
            const int high_x = std::min(grid_x + 1, far_tile_grid - 1);
            const int low_z = std::max(grid_z - 1, 0);
            const int high_z = std::min(grid_z + 1, far_tile_grid - 1);
            const float slope_x = (height_at(high_x, grid_z) - height_at(low_x, grid_z)) *
                                  inverse_cell / static_cast<float>(high_x - low_x);
            const float slope_z = (height_at(grid_x, high_z) - height_at(grid_x, low_z)) *
                                  inverse_cell / static_cast<float>(high_z - low_z);
            const float length = std::sqrt(slope_x * slope_x + slope_z * slope_z + 1.0F);
            const int packed_x =
                std::clamp(static_cast<int>(std::lround(-slope_x / length * 31.0F)), -31, 31) + 32;
            const int packed_z =
                std::clamp(static_cast<int>(std::lround(-slope_z / length * 31.0F)), -31, 31) + 32;
            // How much of the sky the cell sees: ground that rises around it
            // in every direction (a ravine, the foot of a cliff) shuts it out.
            float blocked{};
            for (const auto &direction : directions)
            {
                const float diagonal = direction[0] != 0 && direction[1] != 0 ? 1.41421356F : 1.0F;
                float steepest{};
                for (const int reach : {1, 2, 4})
                {
                    const float rise = height_at(grid_x + direction[0] * reach,
                                                 grid_z + direction[1] * reach) -
                                       height;
                    steepest = std::max(steepest, rise * inverse_cell /
                                                      (static_cast<float>(reach) * diagonal));
                }
                blocked += steepest / std::sqrt(1.0F + steepest * steepest);
            }
            const float open = std::clamp(1.0F - blocked / 8.0F, 0.0F, 1.0F);
            const int occlusion = static_cast<int>(std::lround(open * 15.0F));
            std::uint8_t *texel = tile->cells.data() + index * 4U;
            texel[0] = summary.height[index];
            texel[1] = summary.block[index];
            texel[2] = static_cast<std::uint8_t>(occlusion | ((packed_x >> 2) << 4));
            texel[3] = static_cast<std::uint8_t>((packed_x & 3) | (packed_z << 2));
        }

    // The second layer, for a tile that has a column whose top span floats.
    // A gap shorter than one of this level's cells is not worth its faces:
    // that column is drawn solid, as every column was before.
    const auto floats = [&](std::size_t index) {
        return summary.under[index] > 0U &&
               static_cast<int>(summary.under[index]) - static_cast<int>(summary.lower_height[index]) >=
                   cell;
    };
    for (std::size_t index = 0U; index < summary.under.size(); ++index)
    {
        if (!floats(index))
            continue;
        if (tile->lower.empty())
            tile->lower.resize(tile->cells.size());
        std::uint8_t *texel = tile->lower.data() + index * 4U;
        texel[0] = summary.under[index];
        texel[1] = summary.lower_height[index];
        texel[2] = summary.lower_block[index];
    }

    for (int quadrant = 0; quadrant < 4; ++quadrant)
    {
        const int origin_x = (quadrant & 1) * far_quadrant_cells;
        const int origin_z = (quadrant >> 1) * far_quadrant_cells;
        std::uint8_t minimum = 255U;
        std::uint8_t maximum = 0U;
        // One cell further on each side: the side faces run down to them.
        for (int cell_z = origin_z - 1; cell_z <= origin_z + far_quadrant_cells; ++cell_z)
            for (int cell_x = origin_x - 1; cell_x <= origin_x + far_quadrant_cells; ++cell_x)
            {
                const std::size_t index = far_grid_index(cell_x, cell_z);
                const std::uint8_t height = summary.height[index];
                minimum = std::min(minimum, height);
                maximum = std::max(maximum, height);
                if (!tile->lower.empty() && floats(index))
                {
                    minimum = std::min(minimum, summary.lower_height[index]);
                    tile->lower_quadrants =
                        static_cast<std::uint8_t>(tile->lower_quadrants | (1U << quadrant));
                }
            }
        tile->minimum_height[static_cast<std::size_t>(quadrant)] = minimum;
        tile->maximum_height[static_cast<std::size_t>(quadrant)] = maximum;
        // Which of the quadrant's four 16 x 16 blocks have such a column in
        // them or just beyond their edge: only those are drawn again.
        if ((tile->lower_quadrants & (1U << quadrant)) == 0U)
            continue;
        constexpr int block_cells = far_quadrant_cells / 2;
        for (int block = 0; block < 4; ++block)
        {
            const int block_x = origin_x + (block & 1) * block_cells;
            const int block_z = origin_z + (block >> 1) * block_cells;
            bool any{};
            for (int cell_z = block_z - 1; cell_z <= block_z + block_cells && !any; ++cell_z)
                for (int cell_x = block_x - 1; cell_x <= block_x + block_cells && !any; ++cell_x)
                    any = floats(far_grid_index(cell_x, cell_z));
            if (any)
                tile->lower_blocks =
                    static_cast<std::uint16_t>(tile->lower_blocks | (1U << (quadrant * 4 + block)));
        }
    }

    if (cell <= far_tree_cell_limit)
    {
        const int minimum_x = key.x * far_tile_cells * cell;
        const int minimum_z = key.z * far_tile_cells * cell;
        const int size = far_tile_cells * cell;
        std::vector<TreeSite> sites;
        for (int chunk_z = floor_shift(minimum_z, 4); chunk_z <= floor_shift(minimum_z + size - 1, 4);
             ++chunk_z)
            for (int chunk_x = floor_shift(minimum_x, 4);
                 chunk_x <= floor_shift(minimum_x + size - 1, 4); ++chunk_x)
                generator.tree_sites(chunk_x, chunk_z, sites, true);
        std::vector<std::array<std::uint8_t, 2>> column_edits;
        for (const TreeSite &site : sites)
        {
            if (site.x < minimum_x || site.x >= minimum_x + size || site.z < minimum_z ||
                site.z >= minimum_z + size)
                continue;
            // A tree whose column the player has dug or built on is theirs now.
            if (edits != nullptr && edits->column(site.x, site.z, column_edits))
                continue;
            const int quadrant = ((site.x - minimum_x) * 2 / size) + ((site.z - minimum_z) * 2 / size) * 2;
            tile->trees[static_cast<std::size_t>(quadrant)].push_back(site);
            auto &maximum = tile->maximum_height[static_cast<std::size_t>(quadrant)];
            maximum = static_cast<std::uint8_t>(std::min(
                255, std::max<int>(maximum, site.ground_local_y + site.trunk_height +
                                                std::max(5, site.crown_radius + 2))));
        }
    }
    return tile;
}

// --- Selection helpers -------------------------------------------------------

namespace far_detail
{

float rectangle_distance(double x, double z, double minimum_x, double minimum_z,
                         double size) noexcept
{
    const double dx = std::max({minimum_x - x, 0.0, x - (minimum_x + size)});
    const double dz = std::max({minimum_z - z, 0.0, z - (minimum_z + size)});
    return static_cast<float>(std::sqrt(dx * dx + dz * dz));
}

} // namespace far_detail

void emit_far_quadrants(const FarTerrainSettings &settings, const FarView &view,
                        const FarPlan &plan, bool frustum_cull, FarSelection &selection)
{
    selection.quadrants.clear();
    selection.quadrant_tiles.clear();
    selection.quadrant_plan.clear();
    selection.blocks.clear();
    if (!settings.enabled())
        return;
    const int top_level = far_top_level(settings);
    const float distance = static_cast<float>(settings.distance);
    const double clip_squared =
        static_cast<double>(view.clip_radius) * static_cast<double>(view.clip_radius);
    const far_detail::Frustum frustum{view};
    // Whether a square of ground between two heights can be in the picture:
    // its bounding sphere against the view. (Heights as the cell map stores
    // them; `skirt` is how far the quadrant's skirt hangs below the lowest.)
    const auto in_view = [&](double minimum_x, double minimum_z, double size, float lowest,
                             float highest, float skirt) {
        const float bottom = lowest + static_cast<float>(world_min_y) - skirt;
        const float top = highest + static_cast<float>(world_min_y) + 1.0F;
        const float half = static_cast<float>(size) * 0.5F;
        const float half_height = (top - bottom) * 0.5F;
        return frustum.sphere(static_cast<float>(minimum_x - view.eye_x) + half,
                              (top + bottom) * 0.5F - static_cast<float>(view.eye_y),
                              static_cast<float>(minimum_z - view.eye_z) + half,
                              std::sqrt(2.0F * half * half + half_height * half_height));
    };
    for (std::size_t index = 0U; index < plan.tiles.size(); ++index)
    {
        const FarPlan::Tile &entry = plan.tiles[index];
        if (entry.quadrants == 0U)
            continue;
        const FarLoadedTile &tile = *entry.tile;
        const FarTileKey &key = entry.key;
        const int cell = far_cell_size(settings, key.level);
        const float skirt = static_cast<float>(cell) * 3.0F;
        const double tile_size = static_cast<double>(cell * far_tile_cells);
        const double quadrant_size = static_cast<double>(cell * far_quadrant_cells);
        // A tile wholly out of view draws nothing (it stays in the plan, and
        // so loaded, for a quick turn of the head).
        if (frustum_cull &&
            !in_view(static_cast<double>(key.x) * tile_size, static_cast<double>(key.z) * tile_size,
                     tile_size,
                     static_cast<float>(*std::min_element(tile.minimum_height.begin(),
                                                          tile.minimum_height.end())),
                     static_cast<float>(*std::max_element(tile.maximum_height.begin(),
                                                          tile.maximum_height.end())),
                     skirt))
            continue;
        float morph_start = 1.0e9F;
        float morph_scale = 0.0F;
        float morph_floor = 0.0F;
        if (key.level < top_level)
        {
            morph_start = far_level_morph_start(settings, key.level);
            morph_scale = 1.0F / (far_level_range(settings, key.level) - morph_start);
            // A tile that became drawable after it should have been in use
            // starts on its parent's grid and settles over a third of a second.
            morph_floor = std::clamp(
                1.0F - static_cast<float>(view.seconds - tile.ready_seconds) / 0.35F, 0.0F, 1.0F);
        }
        for (int quadrant = 0; quadrant < 4; ++quadrant)
        {
            if ((entry.quadrants & (1U << quadrant)) == 0U)
                continue;
            const int quadrant_x = quadrant & 1;
            const int quadrant_z = quadrant >> 1;
            const std::int32_t origin_x =
                (key.x * far_tile_cells + quadrant_x * far_quadrant_cells) * cell;
            const std::int32_t origin_z =
                (key.z * far_tile_cells + quadrant_z * far_quadrant_cells) * cell;
            if (frustum_cull &&
                !in_view(static_cast<double>(origin_x), static_cast<double>(origin_z),
                         quadrant_size,
                         static_cast<float>(tile.minimum_height[static_cast<std::size_t>(quadrant)]),
                         static_cast<float>(tile.maximum_height[static_cast<std::size_t>(quadrant)]),
                         skirt))
                continue;
            const float nearest = far_detail::rectangle_distance(
                view.eye_x, view.eye_z, static_cast<double>(origin_x),
                static_cast<double>(origin_z), quadrant_size);
            if (nearest > distance)
                continue;
            // Entirely inside the detailed window: nothing of it would be drawn.
            const double far_corner_x =
                std::max(std::abs(static_cast<double>(origin_x) - view.eye_x),
                         std::abs(static_cast<double>(origin_x) + quadrant_size - view.eye_x));
            const double far_corner_z =
                std::max(std::abs(static_cast<double>(origin_z) - view.eye_z),
                         std::abs(static_cast<double>(origin_z) + quadrant_size - view.eye_z));
            if (far_corner_x * far_corner_x + far_corner_z * far_corner_z < clip_squared)
                continue;
            FarQuadrantInstance instance;
            instance.origin_x = origin_x;
            instance.origin_z = origin_z;
            instance.packed = static_cast<std::uint32_t>(cell) | (tile.slice << 12U) |
                              (static_cast<std::uint32_t>(quadrant_x) << 24U) |
                              (static_cast<std::uint32_t>(quadrant_z) << 25U) |
                              (cell <= far_tree_cell_limit ? 1U << 26U : 0U);
            instance.morph_start = morph_start;
            instance.morph_scale = morph_scale;
            instance.morph_floor = morph_floor;
            // No vertex of it is anywhere near sliding onto the parent grid:
            // the vertex shader then never reads the parent cells.
            const double farthest =
                std::sqrt(far_corner_x * far_corner_x + far_corner_z * far_corner_z);
            if (morph_floor <= 0.0F && farthest + 1.0 < static_cast<double>(morph_start))
                instance.packed |= 1U << 27U;
            instance.reserved0 = nearest;
            const bool layered = (tile.lower_quadrants & (1U << quadrant)) != 0U;
            if (layered)
                instance.packed |= 1U << 30U;
            selection.quadrants.push_back(instance);
            selection.quadrant_tiles.push_back(key);
            selection.quadrant_plan.push_back(static_cast<std::uint32_t>(index));
            if (!layered)
                continue;
            // The blocks of the quadrant that have floating columns are drawn
            // twice more: the ground under them, then their undersides.
            instance.packed &= ~(1U << 26U);
            for (std::uint32_t block = 0U; block < 4U; ++block)
            {
                if (((tile.lower_blocks >> (static_cast<std::uint32_t>(quadrant) * 4U + block)) &
                     1U) == 0U)
                    continue;
                for (std::uint32_t layer = 1U; layer <= 2U; ++layer)
                {
                    FarQuadrantInstance extra = instance;
                    extra.packed |= (layer << 28U) | ((block & 1U) << 11U) | ((block >> 1U) << 31U);
                    selection.blocks.push_back(extra);
                }
            }
        }
    }
    // Nearest first: what is drawn later is then mostly hidden behind what is
    // already there and never shaded.
    const std::size_t count = selection.quadrants.size();
    std::vector<std::uint32_t> order(count);
    for (std::size_t index = 0U; index < count; ++index)
        order[index] = static_cast<std::uint32_t>(index);
    std::sort(order.begin(), order.end(), [&](std::uint32_t left, std::uint32_t right) {
        return selection.quadrants[left].reserved0 < selection.quadrants[right].reserved0;
    });
    std::vector<FarQuadrantInstance> quadrants(count);
    std::vector<FarTileKey> tiles(count);
    std::vector<std::uint32_t> plans(count);
    for (std::size_t index = 0U; index < count; ++index)
    {
        quadrants[index] = selection.quadrants[order[index]];
        tiles[index] = selection.quadrant_tiles[order[index]];
        plans[index] = selection.quadrant_plan[order[index]];
    }
    selection.quadrants = std::move(quadrants);
    selection.quadrant_tiles = std::move(tiles);
    selection.quadrant_plan = std::move(plans);
}

namespace far_detail
{

void sort_wanted(std::vector<FarTileKey> &wanted, const FarTerrainSettings &settings,
                 const FarView &view)
{
    const auto distance = [&](const FarTileKey &key) {
        const double size = static_cast<double>(far_tile_size(settings, key.level));
        return rectangle_distance(view.eye_x, view.eye_z, static_cast<double>(key.x) * size,
                                  static_cast<double>(key.z) * size, size);
    };
    std::sort(wanted.begin(), wanted.end(), [&](const FarTileKey &left, const FarTileKey &right) {
        if (left.level != right.level)
            return left.level > right.level;
        const float left_distance = distance(left);
        const float right_distance = distance(right);
        if (left_distance != right_distance)
            return left_distance < right_distance;
        return left.x != right.x ? left.x < right.x : left.z < right.z;
    });
}

} // namespace far_detail

// --- The disk cache ----------------------------------------------------------

std::filesystem::path far_terrain_cache_directory(std::int32_t seed, TerrainProfile profile,
                                                  GameMode game_mode)
{
    std::filesystem::path root;
    if (const std::string override_root = environment_value("OPENMINERZ_FAR_TERRAIN_CACHE");
        !override_root.empty())
    {
        if (override_root == "off")
            return {};
        root = override_root;
    }
    else if (const std::string local = environment_value("LOCALAPPDATA"); !local.empty())
        root = std::filesystem::path{local} / "OpenMinerZ" / "FarTerrain";
    else
        return {};
    const std::string world = std::string{profile == TerrainProfile::modern ? "modern" : "classic"} +
                              "-" + std::to_string(static_cast<int>(game_mode)) + "-" +
                              std::to_string(seed);
    return root / world;
}

std::uintmax_t prune_far_terrain_cache(const std::filesystem::path &current_world,
                                       std::uintmax_t limit_bytes) noexcept
{
    try
    {
        struct File final
        {
            std::filesystem::path path;
            std::filesystem::file_time_type written;
            std::uintmax_t bytes{};
        };
        struct World final
        {
            std::filesystem::path path;
            std::filesystem::file_time_type used;
            std::uintmax_t bytes{};
            bool current{};
        };
        std::vector<World> worlds;
        std::vector<File> current_files;
        std::uintmax_t total{};
        // Iterators are advanced with increment(error): operator++ throws when
        // a directory disappears under the walk, which is exactly what a
        // second instance pruning the same cache does to this one.
        std::error_code walk;
        const std::filesystem::directory_iterator no_more_worlds;
        for (std::filesystem::directory_iterator entry{current_world.parent_path(), walk};
             !walk && entry != no_more_worlds; entry.increment(walk))
        {
            std::error_code error;
            if (!entry->is_directory(error) || error)
                continue;
            World world;
            world.path = entry->path();
            world.used = std::filesystem::last_write_time(world.path / "last-used", error);
            if (error)
                continue; // Not a folder this cache made: leave it alone.
            world.current = std::filesystem::equivalent(world.path, current_world, error) && !error;
            std::error_code inner;
            const std::filesystem::recursive_directory_iterator no_more_files;
            for (std::filesystem::recursive_directory_iterator file{
                     world.path, std::filesystem::directory_options::skip_permission_denied, inner};
                 !inner && file != no_more_files; file.increment(inner))
            {
                std::error_code status;
                if (!file->is_regular_file(status) || status)
                    continue;
                // (file_size answers -1 for a file that has just gone; adding
                // that would have made every world look over the limit.)
                const std::uintmax_t bytes = file->file_size(status);
                if (status)
                    continue;
                world.bytes += bytes;
                if (world.current && file->path().extension() == ".rgn")
                {
                    const auto written = file->last_write_time(status);
                    if (!status)
                        current_files.push_back({file->path(), written, bytes});
                }
            }
            total += world.bytes;
            worlds.push_back(std::move(world));
        }
        std::sort(worlds.begin(), worlds.end(),
                  [](const World &left, const World &right) { return left.used < right.used; });
        for (const World &world : worlds)
        {
            if (total <= limit_bytes)
                break;
            if (world.current)
                continue;
            std::error_code error;
            std::filesystem::remove_all(world.path, error);
            total -= std::min(total, world.bytes);
        }
        // Still too much: it is all this world's. Its oldest region files go;
        // a tile asked for again is generated again and written back. (A file
        // a worker has open at this instant is not removed on Windows, and
        // simply stays for the next visit.)
        std::sort(current_files.begin(), current_files.end(),
                  [](const File &left, const File &right) { return left.written < right.written; });
        for (const File &file : current_files)
        {
            if (total <= limit_bytes)
                break;
            std::error_code error;
            if (std::filesystem::remove(file.path, error) && !error)
                total -= std::min(total, file.bytes);
        }
        return total;
    }
    catch (...)
    {
        return 0U;
    }
}

namespace
{

// One file per 16 x 16 tiles of one cell size. A slot is the tile's two byte
// grids and a checksum; the header says which slots hold a tile.
class FarTileCache final
{
  public:
    explicit FarTileCache(std::filesystem::path directory) : directory_(std::move(directory))
    {
    }

    [[nodiscard]] bool enabled() const noexcept
    {
        return !directory_.empty();
    }

    [[nodiscard]] bool read(const FarTerrainSettings &settings, const FarTileKey &key,
                            FarTileSummary &summary)
    {
        if (!enabled())
            return false;
        const std::lock_guard lock{mutex_};
        std::ifstream file{path(settings, key), std::ios::binary};
        if (!file)
            return false;
        Header header{};
        file.read(reinterpret_cast<char *>(&header), sizeof(header));
        const int slot = slot_of(key);
        if (!file || header.magic != magic || header.version != version ||
            header.present[static_cast<std::size_t>(slot)] != 1U)
            return false;
        file.seekg(static_cast<std::streamoff>(sizeof(Header)) +
                   static_cast<std::streamoff>(slot) * static_cast<std::streamoff>(record_size));
        std::uint32_t stored{};
        for (auto *grid : grids(summary))
            file.read(reinterpret_cast<char *>(grid->data()),
                      static_cast<std::streamsize>(grid->size()));
        file.read(reinterpret_cast<char *>(&stored), sizeof(stored));
        return static_cast<bool>(file) && stored == checksum(summary);
    }

    void write(const FarTerrainSettings &settings, const FarTileKey &key,
               const FarTileSummary &summary)
    {
        if (!enabled())
            return;
        const std::lock_guard lock{mutex_};
        const std::filesystem::path target = path(settings, key);
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        Header header{};
        {
            std::ifstream existing{target, std::ios::binary};
            if (existing)
                existing.read(reinterpret_cast<char *>(&header), sizeof(header));
            if (!existing || header.magic != magic || header.version != version)
            {
                // Missing or from another format: start the file again.
                header = {};
                header.magic = magic;
                header.version = version;
                std::ofstream fresh{target, std::ios::binary | std::ios::trunc};
                fresh.write(reinterpret_cast<const char *>(&header), sizeof(header));
                if (!fresh)
                    return;
            }
        }
        std::fstream file{target, std::ios::binary | std::ios::in | std::ios::out};
        if (!file)
            return;
        const int slot = slot_of(key);
        file.seekp(static_cast<std::streamoff>(sizeof(Header)) +
                   static_cast<std::streamoff>(slot) * static_cast<std::streamoff>(record_size));
        const std::uint32_t sum = checksum(summary);
        for (const auto *grid : grids(summary))
            file.write(reinterpret_cast<const char *>(grid->data()),
                       static_cast<std::streamsize>(grid->size()));
        file.write(reinterpret_cast<const char *>(&sum), sizeof(sum));
        // The slot is marked only once its bytes are in the file.
        header.present[static_cast<std::size_t>(slot)] = 1U;
        file.seekp(0);
        file.write(reinterpret_cast<const char *>(&header), sizeof(header));
    }

    // Marks this world as the one visited last and, if the cache as a whole
    // has outgrown its limit, removes the worlds visited longest ago.
    void touch_and_prune() const
    {
        if (!enabled())
            return;
        std::error_code error;
        std::filesystem::create_directories(directory_, error);
        {
            std::ofstream marker{directory_ / "last-used", std::ios::binary | std::ios::trunc};
            marker << "far terrain column summaries; safe to delete\n";
        }
        // (This runs on the housekeeping thread; an exception here would end
        // the process, so the walk below never throws.)
        static_cast<void>(prune_far_terrain_cache(directory_, far_terrain_cache_limit_bytes));
    }

  private:
    static constexpr std::uint32_t magic = 0x465A4D4FU; // "OMZF"
    // Raise whenever what a summary means changes (the generator's column
    // passes, the sampling grid): old files are then simply rebuilt.
    // 4: the underside of the top span and the ground under it.
    static constexpr std::uint32_t version = 4U;
    static constexpr int region_tiles = 16;
    static constexpr std::size_t grid_count = 5U;
    static constexpr std::size_t record_size =
        static_cast<std::size_t>(far_tile_grid * far_tile_grid) * grid_count +
        sizeof(std::uint32_t);

    using Grid = std::array<std::uint8_t, far_tile_grid * far_tile_grid>;
    // The byte grids of a summary, in the order a slot stores them.
    [[nodiscard]] static std::array<Grid *, grid_count> grids(FarTileSummary &summary) noexcept
    {
        return {&summary.height, &summary.block, &summary.under, &summary.lower_height,
                &summary.lower_block};
    }
    [[nodiscard]] static std::array<const Grid *, grid_count>
    grids(const FarTileSummary &summary) noexcept
    {
        return {&summary.height, &summary.block, &summary.under, &summary.lower_height,
                &summary.lower_block};
    }

    struct Header final
    {
        std::uint32_t magic{};
        std::uint32_t version{};
        std::array<std::uint8_t, region_tiles * region_tiles> present{};
    };

    [[nodiscard]] static int slot_of(const FarTileKey &key) noexcept
    {
        return (key.x & (region_tiles - 1)) + (key.z & (region_tiles - 1)) * region_tiles;
    }

    [[nodiscard]] static std::uint32_t checksum(const FarTileSummary &summary) noexcept
    {
        std::uint32_t value = 2166136261U;
        for (const auto *grid : grids(summary))
            for (const std::uint8_t byte : *grid)
                value = (value ^ byte) * 16777619U;
        return value;
    }

    [[nodiscard]] std::filesystem::path path(const FarTerrainSettings &settings,
                                             const FarTileKey &key) const
    {
        // Keyed by the cell's width in blocks, not the level, so every detail
        // setting shares the same files.
        return directory_ / ("c" + std::to_string(far_cell_size(settings, key.level))) /
               (std::to_string(key.x >> 4) + "_" + std::to_string(key.z >> 4) + ".rgn");
    }

    std::filesystem::path directory_;
    std::mutex mutex_;
};

// --- Shadow field ------------------------------------------------------------

constexpr int shadow_size = 512;
constexpr std::array<int, 2> shadow_texels{4, 32};
// Nearest caster each channel counts, in blocks.
constexpr float shadow_near_caster = 14.0F;
constexpr float shadow_far_caster = 104.0F;

struct ShadowRequest final
{
    std::vector<std::shared_ptr<const FarTile>> tiles;
    FarTerrainSettings settings{};
    double eye_x{};
    double eye_z{};
    std::array<float, 3> light{};
    std::uint32_t serial{};
};

std::shared_ptr<const FarShadowMap> build_shadow_map(const ShadowRequest &request, int cascade)
{
    auto map = std::make_shared<FarShadowMap>();
    map->size = shadow_size;
    map->texel = shadow_texels[static_cast<std::size_t>(cascade)];
    map->light = request.light;
    map->serial = request.serial;
    const int snap = map->texel * 16;
    const int half = shadow_size * map->texel / 2;
    map->origin_x = static_cast<int>(std::floor(request.eye_x / snap)) * snap - half;
    map->origin_z = static_cast<int>(std::floor(request.eye_z / snap)) * snap - half;

    // The ground under every texel, from the finest tile that covers it.
    std::vector<float> ground(static_cast<std::size_t>(shadow_size * shadow_size), 0.0F);
    std::vector<const FarTile *> ordered;
    ordered.reserve(request.tiles.size());
    for (const auto &tile : request.tiles)
        ordered.push_back(tile.get());
    std::sort(ordered.begin(), ordered.end(), [](const FarTile *left, const FarTile *right) {
        return left->key.level > right->key.level;
    });
    for (const FarTile *tile : ordered)
    {
        const int cell = far_cell_size(request.settings, tile->key.level);
        const int tile_x = tile->key.x * far_tile_cells * cell;
        const int tile_z = tile->key.z * far_tile_cells * cell;
        const int tile_size = far_tile_cells * cell;
        const auto first_texel = [&](int tile_origin, int map_origin) {
            return std::max(0, static_cast<int>(std::ceil(
                                   (static_cast<double>(tile_origin - map_origin) /
                                    map->texel) -
                                   0.5)));
        };
        const auto last_texel = [&](int tile_origin, int map_origin) {
            return std::min(shadow_size - 1,
                            static_cast<int>(std::ceil((static_cast<double>(tile_origin + tile_size -
                                                                            map_origin) /
                                                        map->texel) -
                                                       0.5)) -
                                1);
        };
        const int z0 = first_texel(tile_z, map->origin_z);
        const int z1 = last_texel(tile_z, map->origin_z);
        const int x0 = first_texel(tile_x, map->origin_x);
        const int x1 = last_texel(tile_x, map->origin_x);
        for (int z = z0; z <= z1; ++z)
        {
            const int world_z = map->origin_z + z * map->texel + map->texel / 2;
            const int cell_z = std::clamp((world_z - tile_z) / cell, 0, far_tile_cells - 1);
            for (int x = x0; x <= x1; ++x)
            {
                const int world_x = map->origin_x + x * map->texel + map->texel / 2;
                const int cell_x = std::clamp((world_x - tile_x) / cell, 0, far_tile_cells - 1);
                ground[static_cast<std::size_t>(z * shadow_size + x)] = static_cast<float>(
                    tile->cells[far_grid_index(cell_x, cell_z) * 4U]);
            }
        }
    }

    // Sweep away from the light along whichever axis it leans on more. Each
    // line takes the shadow of the line before it, shifted sideways by the
    // light's other component and lowered by how far the light drops over one
    // texel; a caster only starts to count `delay` lines after its own.
    const float horizontal =
        std::sqrt(request.light[0] * request.light[0] + request.light[2] * request.light[2]);
    map->heights.assign(static_cast<std::size_t>(shadow_size * shadow_size) * 2U, 0U);
    if (horizontal < 0.02F)
        return map; // Straight overhead: nothing casts on anything.
    const bool along_x = std::abs(request.light[0]) >= std::abs(request.light[2]);
    const float major = along_x ? request.light[0] : request.light[2];
    const float minor = along_x ? request.light[2] : request.light[0];
    const int toward = major > 0.0F ? 1 : -1;
    const float sideways = minor / std::abs(major);
    const float step_length = static_cast<float>(map->texel) * std::sqrt(1.0F + sideways * sideways);
    const float drop = step_length * std::max(request.light[1], 0.02F) / horizontal;
    const auto ground_at = [&](int line, int index) {
        return along_x ? ground[static_cast<std::size_t>(index * shadow_size + line)]
                       : ground[static_cast<std::size_t>(line * shadow_size + index)];
    };
    // Linear read of a line at a fractional position; off the map there is
    // nothing to cast a shadow.
    const auto sample = [](const std::vector<float> &line, float position) {
        const float base = std::floor(position);
        const int index = static_cast<int>(base);
        if (index < 0 || index >= shadow_size - 1)
            return 0.0F;
        const float blend = position - base;
        return line[static_cast<std::size_t>(index)] * (1.0F - blend) +
               line[static_cast<std::size_t>(index + 1)] * blend;
    };
    const std::array<int, 2> delay{
        std::max(1, static_cast<int>(std::lround(shadow_near_caster / step_length))),
        std::max(2, static_cast<int>(std::lround(shadow_far_caster / step_length)))};
    std::array<std::vector<float>, 2> previous{std::vector<float>(shadow_size, 0.0F),
                                               std::vector<float>(shadow_size, 0.0F)};
    std::array<std::vector<float>, 2> current = previous;
    std::vector<float> caster(shadow_size);
    for (int step = 0; step < shadow_size; ++step)
    {
        const int line = toward > 0 ? shadow_size - 1 - step : step;
        for (int channel = 0; channel < 2; ++channel)
        {
            const int source_line = line + toward * delay[static_cast<std::size_t>(channel)];
            const bool has_caster = source_line >= 0 && source_line < shadow_size;
            if (has_caster)
                for (int index = 0; index < shadow_size; ++index)
                    caster[static_cast<std::size_t>(index)] = ground_at(source_line, index);
            const float caster_shift =
                sideways * static_cast<float>(delay[static_cast<std::size_t>(channel)]);
            const float caster_drop =
                drop * static_cast<float>(delay[static_cast<std::size_t>(channel)]);
            auto &out = current[static_cast<std::size_t>(channel)];
            const auto &in = previous[static_cast<std::size_t>(channel)];
            for (int index = 0; index < shadow_size; ++index)
            {
                float height =
                    step == 0 ? 0.0F : sample(in, static_cast<float>(index) + sideways) - drop;
                if (has_caster)
                    height = std::max(height, sample(caster, static_cast<float>(index) + caster_shift) -
                                                  caster_drop);
                height = std::max(height, 0.0F);
                out[static_cast<std::size_t>(index)] = height;
                const std::size_t texel = along_x
                                              ? static_cast<std::size_t>(index * shadow_size + line)
                                              : static_cast<std::size_t>(line * shadow_size + index);
                map->heights[texel * 2U + static_cast<std::size_t>(channel)] =
                    static_cast<std::uint16_t>(std::min(height * 64.0F, 65535.0F));
            }
            std::swap(previous[static_cast<std::size_t>(channel)],
                      current[static_cast<std::size_t>(channel)]);
        }
    }
    return map;
}

// --- The origin tower --------------------------------------------------------

// OriginBiome: a column `round(distance)` blocks from the origin is bedrock up
// to local y 125 - 8 * distance for distances up to four, with a lantern as
// the top block of the two innermost rings. As boxes: one run of columns per
// row of each ring.
struct TowerBox final
{
    int minimum_x{};
    int minimum_y{};
    int minimum_z{};
    int size_x{};
    int size_y{};
    bool lantern{};
};

std::vector<TowerBox> origin_tower_boxes()
{
    std::vector<TowerBox> boxes;
    for (int z = -4; z <= 4; ++z)
    {
        int run_start = 0;
        int run_radius = -1;
        const auto flush = [&](int end_x) {
            if (run_radius < 0)
                return;
            const int top = 125 - run_radius * 8; // one past the highest cell
            const int below = run_radius < 2 ? top - 1 : top;
            boxes.push_back({run_start, world_min_y, z, end_x - run_start, below, false});
            if (run_radius < 2)
                boxes.push_back({run_start, world_min_y + below, z, end_x - run_start, 1, true});
        };
        for (int x = -5; x <= 5; ++x)
        {
            const int radius = static_cast<int>(
                std::nearbyint(std::sqrt(static_cast<double>(x * x + z * z))));
            const int ring = radius <= 4 && std::abs(x) <= 4 ? radius : -1;
            if (ring != run_radius)
            {
                flush(x);
                run_start = x;
                run_radius = ring;
            }
        }
    }
    return boxes;
}

} // namespace

// mod-world: where a generator's column summaries are cached.
static std::filesystem::path custom_cache_directory(const WorldGenerator &generator)
{
    std::filesystem::path directory = far_terrain_cache_directory(
        generator.seed(), generator.profile(), generator.game_mode());
    // (fix, wave 5: "-2", block bytes may name side profiles.)
    if (!directory.empty() && generator.custom_terrain() != nullptr)
        directory += "-mod-" + generator.custom_terrain()->digest().substr(0U, 16U) + "-2";
    return directory;
}

// --- The streaming cache -----------------------------------------------------

class FarTerrain::Impl final
{
  public:
    Impl(std::int32_t seed, TerrainProfile profile, GameMode game_mode)
        : Impl(WorldGenerator{seed, profile, game_mode})
    {
    }

    // mod-world: a mod generator's tiles go to a cache of their own (keyed by
    // its digest), and its world may have no origin tower.
    explicit Impl(const WorldGenerator &generator)
        : generator_(generator),
          cache_(custom_cache_directory(generator)),
          tower_(generator.custom_terrain() != nullptr &&
                         !generator.custom_terrain()->definition().origin_tower
                     ? std::vector<TowerBox>{}
                     : origin_tower_boxes())
    {
        unsigned count = std::clamp(std::thread::hardware_concurrency() / 4U, 1U, 4U);
        if (const std::string text = environment_value("OPENMINERZ_FAR_TERRAIN_WORKERS");
            !text.empty())
            count = static_cast<unsigned>(std::clamp(std::atoi(text.c_str()), 1, 16));
        workers_.reserve(count + 1U);
        for (unsigned index = 0U; index < count; ++index)
            workers_.emplace_back([this] { work(); });
        // Housekeeping of the disk cache and the shadow fields share a thread
        // of their own, so neither ever waits behind a queue of tiles.
        workers_.emplace_back([this] { shadow_work(); });
    }

    ~Impl()
    {
        {
            const std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        work_ready_.notify_all();
        shadow_ready_.notify_all();
        for (std::thread &worker : workers_)
            worker.join();
    }

    void set_settings(const FarTerrainSettings &settings)
    {
        FarTerrainSettings next = settings;
        next.base_cell = std::clamp(next.base_cell, 1, 2);
        next.ring_cells = std::clamp(next.ring_cells, 192, 512);
        next.distance = std::clamp(next.distance, 0, 16384);
        if (next == settings_)
            return;
        settings_ = next;
        reset();
    }

    [[nodiscard]] const FarTerrainSettings &settings() const noexcept
    {
        return settings_;
    }

    void set_edit(int world_x, int world_y, int world_z, BlockType block)
    {
        edits_.set(world_x, world_y, world_z, block);
        mark_column_dirty(world_x, world_z);
    }

    void erase_edits(int minimum_x, int minimum_z, int maximum_x, int maximum_z)
    {
        if (!edits_.touches(minimum_x, minimum_z, maximum_x, maximum_z))
            return;
        for (const auto &column : edits_.erase(minimum_x, minimum_z, maximum_x, maximum_z))
            mark_column_dirty(column[0], column[1]);
    }

    void clear_edits()
    {
        if (edits_.column_count() == 0U)
            return;
        edits_.clear();
        for (auto &[key, entry] : tiles_)
            entry.dirty = true;
        edits_dirty_ = true;
    }

    // mod-world (wave 5)
    void set_mod_palette(std::shared_ptr<const ModPaletteLooks> looks)
    {
        if (looks == mod_palette_)
            return;
        mod_palette_ = std::move(looks);
        // Trees are packed into boxes with their tiles: pack them again.
        for (auto &[key, entry] : tiles_)
            entry.boxes_built = false;
    }

    [[nodiscard]] FarTerrainFrame update(const FarView &view, float visibility_distance,
                                         const std::array<float, 3> &light, double seconds,
                                         int maximum_uploads)
    {
        FarTerrainFrame frame;
        last_view_ = view;
        last_visibility_ = visibility_distance;
        last_light_ = light;
        if (!settings_.enabled())
            return frame;
        const auto started = Clock::now();
        ++frame_index_;
        frame.active = true;
        frame.settings = settings_;
        frame.slice_capacity = static_cast<std::uint32_t>(capacity_);
        frame.generation = generation_;
        frame.handover = far_handover(visibility_distance);

        admit_finished(frame, seconds, maximum_uploads);

        FarView clipped = view;
        clipped.clip_radius = frame.handover.clip;
        clipped.seconds = seconds;
        // Which tiles are in use only changes when the eye has moved or a tile
        // has come or gone, so the walk of the quadtree is kept between such
        // frames; every frame only culls the plan against the view.
        const double moved = std::hypot(view.eye_x - plan_eye_x_, view.eye_z - plan_eye_z_);
        if (!plan_valid_ || plan_version_ != tiles_version_ || moved >= 0.5 ||
            frame_index_ - plan_frame_ >= 24U || plan_clip_ != clipped.clip_radius)
        {
            plan_entries_.clear();
            const auto lookup = [&](const FarTileKey &key) -> const FarLoadedTile * {
                const auto found = tiles_.find(key);
                if (found == tiles_.end())
                    return nullptr;
                // Every tile the walk asks for is one it goes through: in use.
                found->second.last_used = frame_index_;
                return &found->second.info;
            };
            plan_ = plan_far_tiles(settings_, clipped, lookup);
            // (The records live in tiles_, whose nodes stay put until erased,
            // and erasing changes tiles_version_.)
            plan_entries_.reserve(plan_.tiles.size());
            for (const FarPlan::Tile &tile : plan_.tiles)
                plan_entries_.push_back(&tiles_.find(tile.key)->second);
            plan_valid_ = true;
            plan_version_ = tiles_version_;
            plan_frame_ = frame_index_;
            plan_eye_x_ = view.eye_x;
            plan_eye_z_ = view.eye_z;
            plan_clip_ = clipped.clip_radius;
            plan_core_ready_ = core_tiles_ready(view);
            schedule(plan_.wanted);
        }
        emit_far_quadrants(settings_, clipped, plan_, true, selection_);
        FarSelection &selection = selection_;

        // The landscape eases in once the tiles around the camera are there,
        // and out again while a teleport's are still on their way.
        const bool core_ready = plan_core_ready_;
        const float delta = last_seconds_ < 0.0
                                ? 0.0F
                                : static_cast<float>(std::clamp(seconds - last_seconds_, 0.0, 0.25));
        last_seconds_ = seconds;
        opacity_ = std::clamp(opacity_ + (core_ready ? delta : -delta) / 0.45F, 0.0F, 1.0F);
        frame.opacity = opacity_;

        // Boxes are placed relative to a point that only moves every couple of
        // thousand blocks, so a tile's trees are packed once and copied after.
        constexpr double box_snap = 2048.0;
        frame.box_origin = {
            static_cast<std::int32_t>(std::floor(view.eye_x / box_snap) * box_snap), world_min_y,
            static_cast<std::int32_t>(std::floor(view.eye_z / box_snap) * box_snap)};
        append_boxes(frame, selection, view);
        frame.quadrants = selection.quadrants;
        frame.blocks = selection.blocks;

        request_shadows(view, light, seconds);
        {
            const std::lock_guard lock{mutex_};
            frame.shadows = shadows_;
        }

        const double elapsed = milliseconds_since(started);
        const std::lock_guard lock{mutex_};
        stats_.update_milliseconds_total += elapsed;
        stats_.update_milliseconds_peak = std::max(stats_.update_milliseconds_peak, elapsed);
        ++stats_.updates;
        stats_.quadrants_drawn = frame.quadrants.size();
        stats_.boxes_drawn = frame.boxes.size();
        stats_.tiles_loaded = tiles_.size();
        stats_.tile_capacity = static_cast<std::size_t>(capacity_);
        stats_.tiles_pending = in_flight_.size() + jobs_.size();
        // The cell map of every loaded tile here, and the same again on the
        // GPU; a tile with floating columns has a second map of the same size.
        stats_.memory_bytes = (tiles_.size() + lower_tiles_) *
                              static_cast<std::size_t>(far_tile_grid * far_tile_grid) * 4U * 2U;
        return frame;
    }

    void wait_until_idle()
    {
        if (!settings_.enabled())
            return;
        double seconds = last_seconds_ < 0.0 ? 0.0 : last_seconds_;
        for (int attempt = 0; attempt < 20000; ++attempt)
        {
            seconds += 0.05;
            (void)update(last_view_, last_visibility_, last_light_, seconds, 1 << 20);
            bool idle{};
            {
                const std::lock_guard lock{mutex_};
                idle = jobs_.empty() && in_flight_.empty() && finished_.empty();
            }
            if (idle && !rebuild_pending())
            {
                // One more pass: the tiles just admitted may want children.
                (void)update(last_view_, last_visibility_, last_light_, seconds, 1 << 20);
                const std::lock_guard lock{mutex_};
                if (jobs_.empty() && in_flight_.empty() && finished_.empty())
                    return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    [[nodiscard]] FarTerrainStats stats() const
    {
        const std::lock_guard lock{mutex_};
        return stats_;
    }

  private:
    struct Entry final
    {
        std::shared_ptr<const FarTile> tile;
        FarLoadedTile info{};
        // The frame of the last plan that went through this tile.
        std::uint64_t last_used{};
        bool dirty{};
        // The tile's trees as boxes, per quadrant, for one box origin.
        std::array<std::vector<FarBoxInstance>, 4> boxes;
        std::array<std::int32_t, 2> boxes_origin{};
        bool boxes_built{};
    };
    struct Job final
    {
        FarTileKey key{};
        FarTerrainSettings settings{};
        std::uint32_t generation{};
    };
    struct Finished final
    {
        FarTileKey key{};
        std::uint32_t generation{};
        std::shared_ptr<const FarTile> tile;
    };

    // Every loaded tile that samples this column has to be built again.
    void mark_column_dirty(int world_x, int world_z)
    {
        if (!settings_.enabled() || tiles_.empty())
            return;
        // At each level the column is a sample point of at most a handful of
        // tiles: the one it stands in and any neighbour whose apron reaches
        // it. (Importing a large save calls this once per edit.)
        const int top_level = far_top_level(settings_);
        for (int level = 0; level <= top_level; ++level)
        {
            const int cell = far_cell_size(settings_, level);
            if (world_x % cell != 0 || world_z % cell != 0)
                break; // nor of any coarser level
            const int grid_x = world_x / cell;
            const int grid_z = world_z / cell;
            const int tile_x = floor_shift(grid_x, 6);
            const int tile_z = floor_shift(grid_z, 6);
            for (int z = tile_z - 1; z <= tile_z + 1; ++z)
                for (int x = tile_x - 1; x <= tile_x + 1; ++x)
                {
                    const int cell_x = grid_x - x * far_tile_cells;
                    const int cell_z = grid_z - z * far_tile_cells;
                    if (cell_x < -far_tile_low_apron || cell_x > far_tile_cells ||
                        cell_z < -far_tile_low_apron || cell_z > far_tile_cells)
                        continue;
                    const auto found = tiles_.find({level, x, z});
                    if (found == tiles_.end())
                        continue;
                    found->second.dirty = true;
                    edits_dirty_ = true;
                }
        }
    }

    void reset()
    {
        ++generation_;
        tiles_.clear();
        lower_tiles_ = 0U;
        ++tiles_version_;
        plan_valid_ = false;
        plan_ = {};
        plan_entries_.clear();
        {
            const std::lock_guard lock{mutex_};
            jobs_.clear();
            finished_.clear();
            in_flight_.clear();
            shadows_ = {};
            // A shadow field still being built belongs to the tiles just dropped.
            shadow_request_.reset();
            shadow_generation_ = generation_;
        }
        capacity_ = far_tile_capacity(settings_);
        free_slices_.clear();
        for (int slice = capacity_ - 1; slice >= 0; --slice)
            free_slices_.push_back(static_cast<std::uint32_t>(slice));
        opacity_ = 0.0F;
        shadow_valid_ = false;
    }

    [[nodiscard]] bool rebuild_pending() const
    {
        if (!edits_dirty_)
            return false;
        for (const auto &[key, entry] : tiles_)
            if (entry.dirty)
                return true;
        return false;
    }

    // Tiles the workers have finished become drawable here, a bounded number
    // per frame: each is one 18 KB slice for the renderer to upload.
    void admit_finished(FarTerrainFrame &frame, double seconds, int maximum_uploads)
    {
        std::vector<Finished> arrived;
        {
            const std::lock_guard lock{mutex_};
            const std::size_t count =
                std::min(finished_.size(), static_cast<std::size_t>(std::max(maximum_uploads, 0)));
            arrived.assign(std::make_move_iterator(finished_.begin()),
                           std::make_move_iterator(finished_.begin() +
                                                   static_cast<std::ptrdiff_t>(count)));
            finished_.erase(finished_.begin(),
                            finished_.begin() + static_cast<std::ptrdiff_t>(count));
            for (const Finished &result : arrived)
                in_flight_.erase(result.key);
        }
        for (Finished &result : arrived)
        {
            if (result.generation != generation_ || result.tile == nullptr)
                continue;
            const auto existing = tiles_.find(result.key);
            if (existing != tiles_.end())
            {
                // A rebuild after an edit: same slice, no fade.
                if (!existing->second.tile->lower.empty())
                    --lower_tiles_;
                if (!result.tile->lower.empty())
                    ++lower_tiles_;
                existing->second.tile = result.tile;
                existing->second.boxes_built = false;
                existing->second.info.minimum_height = result.tile->minimum_height;
                existing->second.info.maximum_height = result.tile->maximum_height;
                existing->second.info.lower_quadrants = result.tile->lower_quadrants;
                existing->second.info.lower_blocks = result.tile->lower_blocks;
                frame.uploads.push_back({existing->second.info.slice, std::move(result.tile)});
                ++uploaded_;
                continue;
            }
            if (free_slices_.empty() && !evict_one())
                continue; // Everything loaded is in use; the tile is asked for again later.
            Entry entry;
            entry.info.slice = free_slices_.back();
            free_slices_.pop_back();
            entry.info.minimum_height = result.tile->minimum_height;
            entry.info.maximum_height = result.tile->maximum_height;
            entry.info.lower_quadrants = result.tile->lower_quadrants;
            entry.info.lower_blocks = result.tile->lower_blocks;
            entry.info.ready_seconds = seconds;
            if (!result.tile->lower.empty())
                ++lower_tiles_;
            entry.last_used = frame_index_;
            entry.tile = result.tile;
            frame.uploads.push_back({entry.info.slice, std::move(result.tile)});
            tiles_.emplace(result.key, std::move(entry));
            ++tiles_version_;
            ++uploaded_;
        }
        if (!frame.uploads.empty())
        {
            const std::lock_guard lock{mutex_};
            stats_.tiles_uploaded = uploaded_;
        }
    }

    // Frees the slice of the tile that has gone unused longest. Tiles the
    // current plan goes through are never taken.
    [[nodiscard]] bool evict_one()
    {
        auto oldest = tiles_.end();
        for (auto tile = tiles_.begin(); tile != tiles_.end(); ++tile)
            if ((!plan_valid_ || tile->second.last_used < plan_frame_) &&
                (oldest == tiles_.end() || tile->second.last_used < oldest->second.last_used))
                oldest = tile;
        if (oldest == tiles_.end())
            return false;
        free_slices_.push_back(oldest->second.info.slice);
        if (!oldest->second.tile->lower.empty())
            --lower_tiles_;
        tiles_.erase(oldest);
        ++tiles_version_;
        const std::lock_guard lock{mutex_};
        ++stats_.tiles_evicted;
        return true;
    }

    void schedule(const std::vector<FarTileKey> &wanted)
    {
        const std::size_t limit = (workers_.size() - 1U) * 2U;
        bool queued{};
        {
            const std::lock_guard lock{mutex_};
            // What is still waiting was chosen for an earlier camera: the
            // newest list replaces it, so a teleport is never stuck behind
            // the place the player left.
            for (const Job &job : jobs_)
            {
                in_flight_.erase(job.key);
                // (A queued rebuild of a loaded tile is owed again.)
                if (const auto loaded = tiles_.find(job.key); loaded != tiles_.end())
                {
                    loaded->second.dirty = true;
                    edits_dirty_ = true;
                }
            }
            jobs_.clear();
            for (const FarTileKey &key : wanted)
            {
                if (jobs_.size() >= limit)
                    break;
                if (in_flight_.contains(key))
                    continue;
                in_flight_.insert(key);
                jobs_.push_back({key, settings_, generation_});
                queued = true;
            }
            // Edited tiles are redone when nothing new is waiting.
            if (edits_dirty_ && jobs_.size() < limit)
            {
                bool any{};
                for (auto &[key, entry] : tiles_)
                {
                    if (!entry.dirty)
                        continue;
                    any = true;
                    if (jobs_.size() >= limit || in_flight_.contains(key))
                        continue;
                    entry.dirty = false;
                    in_flight_.insert(key);
                    jobs_.push_back({key, settings_, generation_});
                    queued = true;
                }
                edits_dirty_ = any;
            }
        }
        if (queued)
            work_ready_.notify_all();
    }

    [[nodiscard]] bool core_tiles_ready(const FarView &view) const
    {
        const int top_level = far_top_level(settings_);
        const double size = static_cast<double>(far_tile_size(settings_, top_level));
        const double reach = static_cast<double>(settings_.distance) * 0.6;
        for (auto z = static_cast<std::int32_t>(std::floor((view.eye_z - reach) / size));
             z <= static_cast<std::int32_t>(std::floor((view.eye_z + reach) / size)); ++z)
            for (auto x = static_cast<std::int32_t>(std::floor((view.eye_x - reach) / size));
                 x <= static_cast<std::int32_t>(std::floor((view.eye_x + reach) / size)); ++x)
                if (!tiles_.contains({top_level, x, z}))
                    return false;
        return true;
    }

    // mod-world (wave 5): a mod generator's tree in its own blocks and shape,
    // drawn inside the blocks the generator grows (custom_terrain.cpp plant),
    // so where the detailed window and the far landscape overlap the boxes
    // hide in the real crown. `box(x0, y0, z0, sx, sy, sz, side, top)`.
    template <typename Box>
    void append_mod_tree(const TreeSite &tree, const Box &box) const
    {
        const auto tiles = [&](BlockType block, std::uint8_t mod) {
            std::array<std::uint8_t, 2> side_top{atlas_tile(block, FaceDirection::positive_x),
                                                 atlas_tile(block, FaceDirection::positive_y)};
            if (mod != 0U && mod_palette_ != nullptr && mod <= mod_palette_->size() &&
                (*mod_palette_)[mod - 1U].valid)
                side_top = {(*mod_palette_)[mod - 1U].tiles[1], (*mod_palette_)[mod - 1U].tiles[0]};
            return side_top;
        };
        const auto trunk = tiles(tree.trunk, tree.trunk_mod);
        const auto leaves = tiles(tree.leaves, tree.leaves_mod);
        const int thickness = tree.thick ? 2 : 1;
        const int radius = std::max<int>(tree.crown_radius, 1);
        const int base = tree.ground_local_y + 1;
        const int top = base + tree.trunk_height; // first cell above the trunk
        const auto slab = [&](int half, int from, int to) {
            if (to < from)
                return;
            box(-half, from, -half, 2 * half + thickness, to - from + 1, 2 * half + thickness,
                leaves[0], leaves[1]);
        };
        // The trunk up to the crown (the rest is hidden in it, and a trunk as
        // narrow as the tip of a cone would flicker against it).
        const auto stem = [&](int up_to) {
            const int height = std::min(up_to, top) - base;
            if (height > 0)
                box(0, base, 0, thickness, height, thickness, trunk[0], trunk[1]);
        };
        switch (tree.crown)
        {
        case TreeCrown::cone: {
            // Three tiers of the rings, each as wide as its rings are on
            // average (every other ring is tucked in to 0.7).
            const int crown_bottom = base + std::max(2, tree.trunk_height / 3);
            const int crown_top = top + 1;
            stem(crown_bottom);
            const int span = crown_top - crown_bottom + 1;
            for (int tier = 0; tier < 3; ++tier)
            {
                const int from = crown_bottom + span * tier / 3;
                const int to = crown_bottom + span * (tier + 1) / 3 - 1;
                const double middle = (from + to) * 0.5;
                const double along = (crown_top - middle) / std::max(crown_top - crown_bottom, 1);
                const double ring = (0.6 + along * radius) * 0.85;
                slab(static_cast<int>(ring), from, to);
            }
            break;
        }
        case TreeCrown::round: {
            // A ball around the top of the trunk: a wide flat slab through a
            // tall narrow one.
            const int centre = top - 1;
            const int flat = std::max(radius / 2, 0);
            stem(centre - flat);
            slab(radius - 1 > 0 ? radius - 1 : 0, centre - flat, centre + flat);
            if (radius - 1 > flat) // only when it stands out of the flat one
                slab(std::max(radius - 2, 0), centre - radius + 1, centre + radius - 1);
            break;
        }
        case TreeCrown::retail:
            stem(top);
            box(-2, top - 1, -2, 5, 3, 5, leaves[0], leaves[1]);
            box(-1, top - 2, -1, 3, 5, 3, leaves[0], leaves[1]);
            break;
        }
    }

    void append_boxes(FarTerrainFrame &frame, const FarSelection &selection, const FarView &view)
    {
        constexpr std::size_t maximum_boxes = 48'000U;
        const auto relative = [](int value, int origin) {
            return static_cast<std::int16_t>(std::clamp(value - origin, -32768, 32767));
        };
        // Trees shrink away over the last stretch of the level they belong to.
        int tree_level = 0;
        while (far_cell_size(settings_, tree_level + 1) <= far_tree_cell_limit)
            ++tree_level;
        const float tree_range = std::min(far_level_range(settings_, tree_level),
                                          static_cast<float>(settings_.distance));
        const auto fade_start = static_cast<std::int16_t>(std::min(tree_range * 0.72F, 32000.0F));
        const auto fade_end = static_cast<std::uint16_t>(std::min(tree_range * 0.98F, 65000.0F));
        constexpr std::uint8_t log_side = 15U;
        constexpr std::uint8_t log_end = 16U;
        constexpr std::uint8_t leaves = 17U;
        for (std::size_t index = 0U; index < selection.quadrants.size(); ++index)
        {
            const FarQuadrantInstance &quadrant = selection.quadrants[index];
            if ((quadrant.packed & (1U << 26U)) == 0U)
                continue;
            Entry &entry = *plan_entries_[selection.quadrant_plan[index]];
            const std::size_t which = ((quadrant.packed >> 24U) & 1U) | (((quadrant.packed >> 25U) & 1U) << 1U);
            if (!entry.boxes_built || entry.boxes_origin[0] != frame.box_origin[0] ||
                entry.boxes_origin[1] != frame.box_origin[2])
            {
                // Pack this tile's trees for the current origin, once.
                entry.boxes_built = true;
                entry.boxes_origin = {frame.box_origin[0], frame.box_origin[2]};
                for (std::size_t part = 0U; part < 4U; ++part)
                {
                    auto &boxes = entry.boxes[part];
                    boxes.clear();
                    for (const TreeSite &tree : entry.tile->trees[part])
                    {
                        const int base = tree.ground_local_y + 1;
                        const int crown = base + tree.trunk_height;
                        const auto box = [&](int x0, int y0, int z0, int sx, int sy, int sz,
                                             std::uint8_t side, std::uint8_t top) {
                            boxes.push_back({relative(tree.x + x0, frame.box_origin[0]),
                                             static_cast<std::int16_t>(y0),
                                             relative(tree.z + z0, frame.box_origin[2]),
                                             fade_start, static_cast<std::uint8_t>(sx),
                                             static_cast<std::uint8_t>(sy),
                                             static_cast<std::uint8_t>(sz), side, top,
                                             static_cast<std::uint8_t>(std::clamp(base, 0, 127) << 1),
                                             fade_end});
                        };
                        if (tree.crown == TreeCrown::retail && tree.trunk == BlockType::log &&
                            tree.leaves == BlockType::leaves && tree.trunk_mod == 0U &&
                            tree.leaves_mod == 0U)
                        {
                            box(0, base, 0, 1, std::max<int>(tree.trunk_height, 1), 1, log_side,
                                log_end);
                            // The crown: a wide flat slab through a tall narrow
                            // one, which is the rounded blob of leaves the
                            // generator grows.
                            box(-2, crown - 1, -2, 5, 3, 5, leaves, leaves);
                            box(-1, crown - 2, -1, 3, 5, 3, leaves, leaves);
                            continue;
                        }
                        append_mod_tree(tree, box);
                    }
                }
            }
            const auto &boxes = entry.boxes[which];
            if (frame.boxes.size() + boxes.size() > maximum_boxes)
                break;
            frame.boxes.insert(frame.boxes.end(), boxes.begin(), boxes.end());
        }
        // The tower at the world's origin, wherever it is in range.
        const double tower_distance = std::hypot(view.eye_x, view.eye_z);
        if (tower_distance < static_cast<double>(settings_.distance) && tower_distance < 30000.0)
        {
            constexpr std::uint8_t bedrock = 12U;
            constexpr std::uint8_t lantern = 4U;
            for (const TowerBox &box : tower_)
                frame.boxes.push_back(
                    {relative(box.minimum_x, frame.box_origin[0]),
                     static_cast<std::int16_t>(box.minimum_y - world_min_y),
                     relative(box.minimum_z, frame.box_origin[2]), 0,
                     static_cast<std::uint8_t>(box.size_x), static_cast<std::uint8_t>(box.size_y),
                     1U, box.lantern ? lantern : bedrock, box.lantern ? lantern : bedrock,
                     static_cast<std::uint8_t>(box.lantern ? 1U : 0U), 0U});
        }
    }

    // --- Workers -------------------------------------------------------------

    static void lower_thread_priority() noexcept
    {
#if defined(_WIN32)
        // Never compete with the frame, the mesh worker or the audio thread.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    }

    void work()
    {
        lower_thread_priority();
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock{mutex_};
                work_ready_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                if (stopping_)
                    return;
                job = jobs_.front();
                jobs_.pop_front();
            }
            const auto started = Clock::now();
            FarTileSummary summary;
            bool from_disk = cache_.read(job.settings, job.key, summary);
            double generate_milliseconds{};
            if (!from_disk)
            {
                const auto generate_started = Clock::now();
                summary = summarize_far_tile(generator_, job.settings, job.key);
                generate_milliseconds = milliseconds_since(generate_started);
                cache_.write(job.settings, job.key, summary);
            }
            std::shared_ptr<const FarTile> tile =
                build_far_tile(generator_, job.settings, job.key, std::move(summary), &edits_);
            const double elapsed = milliseconds_since(started);
            const std::lock_guard lock{mutex_};
            finished_.push_back({job.key, job.generation, std::move(tile)});
            ++stats_.tiles_built;
            if (from_disk)
                ++stats_.tiles_from_disk;
            else
            {
                ++stats_.tiles_generated;
                stats_.generate_milliseconds_total += generate_milliseconds;
                stats_.columns_generated += static_cast<std::size_t>(far_tile_grid * far_tile_grid);
            }
            stats_.build_milliseconds_total += elapsed;
            stats_.build_milliseconds_peak = std::max(stats_.build_milliseconds_peak, elapsed);
        }
    }

    void request_shadows(const FarView &view, const std::array<float, 3> &light, double seconds)
    {
        const float alignment = light[0] * shadow_light_[0] + light[1] * shadow_light_[1] +
                                light[2] * shadow_light_[2];
        const bool light_moved = !shadow_valid_ || alignment < 0.99998F; // about a third of a degree
        const bool eye_moved = !shadow_valid_ || std::abs(view.eye_x - shadow_eye_x_) > 48.0 ||
                               std::abs(view.eye_z - shadow_eye_z_) > 48.0;
        const bool tiles_changed = uploaded_ != shadow_uploaded_ && seconds - shadow_seconds_ > 0.35;
        if (!(light_moved || eye_moved || tiles_changed) || tiles_.empty())
            return;
        {
            const std::lock_guard lock{mutex_};
            if (shadow_busy_ || shadow_request_ != nullptr)
                return;
        }
        auto request = std::make_unique<ShadowRequest>();
        request->tiles.reserve(tiles_.size());
        for (const auto &[key, entry] : tiles_)
            request->tiles.push_back(entry.tile);
        request->settings = settings_;
        request->eye_x = view.eye_x;
        request->eye_z = view.eye_z;
        request->light = light;
        request->serial = ++shadow_serial_;
        shadow_light_ = light;
        shadow_eye_x_ = view.eye_x;
        shadow_eye_z_ = view.eye_z;
        shadow_uploaded_ = uploaded_;
        shadow_seconds_ = seconds;
        shadow_valid_ = true;
        {
            const std::lock_guard lock{mutex_};
            shadow_request_ = std::move(request);
            shadow_generation_ = generation_;
        }
        shadow_ready_.notify_one();
    }

    void shadow_work()
    {
        lower_thread_priority();
        cache_.touch_and_prune();
        for (;;)
        {
            std::unique_ptr<ShadowRequest> request;
            std::uint32_t generation{};
            {
                std::unique_lock lock{mutex_};
                shadow_ready_.wait(lock, [&] { return stopping_ || shadow_request_ != nullptr; });
                if (stopping_)
                    return;
                request = std::move(shadow_request_);
                generation = shadow_generation_;
                shadow_busy_ = true;
            }
            const auto started = Clock::now();
            std::array<std::shared_ptr<const FarShadowMap>, 2> maps{build_shadow_map(*request, 0),
                                                                   build_shadow_map(*request, 1)};
            const double elapsed = milliseconds_since(started);
            // The tiles are released here, off the game thread.
            request.reset();
            const std::lock_guard lock{mutex_};
            shadow_busy_ = false;
            if (generation == shadow_generation_)
                shadows_ = std::move(maps);
            ++stats_.shadow_builds;
            stats_.shadow_milliseconds_total += elapsed;
        }
    }

    WorldGenerator generator_;
    FarTileCache cache_;
    FarEditOverlay edits_;
    std::vector<TowerBox> tower_;
    FarTerrainSettings settings_{};
    std::uint32_t generation_{};
    int capacity_{};
    std::unordered_map<FarTileKey, Entry, FarTileKeyHash> tiles_;
    std::vector<std::uint32_t> free_slices_;
    std::uint64_t frame_index_{};
    std::size_t uploaded_{};
    // Loaded tiles that carry a second layer (FarTile::lower).
    std::size_t lower_tiles_{};
    bool edits_dirty_{};
    float opacity_{};
    // The kept plan (see update()) and what it was made for.
    FarPlan plan_;
    std::vector<Entry *> plan_entries_;
    std::shared_ptr<const ModPaletteLooks> mod_palette_; // mod-world (wave 5)
    FarSelection selection_;
    bool plan_valid_{};
    bool plan_core_ready_{};
    std::uint64_t tiles_version_{};
    std::uint64_t plan_version_{};
    std::uint64_t plan_frame_{};
    double plan_eye_x_{};
    double plan_eye_z_{};
    float plan_clip_{};
    double last_seconds_{-1.0};
    FarView last_view_{};
    float last_visibility_{128.0F};
    std::array<float, 3> last_light_{0.0F, 1.0F, 0.0F};

    // Shadow field scheduling (game thread).
    bool shadow_valid_{};
    std::array<float, 3> shadow_light_{};
    double shadow_eye_x_{};
    double shadow_eye_z_{};
    std::size_t shadow_uploaded_{};
    double shadow_seconds_{};
    std::uint32_t shadow_serial_{};

    // Shared with the workers.
    mutable std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable shadow_ready_;
    std::deque<Job> jobs_;
    std::vector<Finished> finished_;
    std::unordered_set<FarTileKey, FarTileKeyHash> in_flight_;
    std::unique_ptr<ShadowRequest> shadow_request_;
    std::uint32_t shadow_generation_{};
    bool shadow_busy_{};
    std::array<std::shared_ptr<const FarShadowMap>, 2> shadows_;
    FarTerrainStats stats_;
    bool stopping_{};
    std::vector<std::thread> workers_;
};

FarTerrain::FarTerrain(std::int32_t seed, TerrainProfile profile, GameMode game_mode)
    : impl_(std::make_unique<Impl>(seed, profile, game_mode))
{
}

FarTerrain::FarTerrain(const WorldGenerator &generator) : impl_(std::make_unique<Impl>(generator))
{
}

FarTerrain::~FarTerrain() = default;

void FarTerrain::set_settings(const FarTerrainSettings &settings)
{
    impl_->set_settings(settings);
}

const FarTerrainSettings &FarTerrain::settings() const noexcept
{
    return impl_->settings();
}

void FarTerrain::set_edit(int world_x, int world_y, int world_z, BlockType block)
{
    impl_->set_edit(world_x, world_y, world_z, block);
}

void FarTerrain::erase_edits(int minimum_x, int minimum_z, int maximum_x, int maximum_z)
{
    impl_->erase_edits(minimum_x, minimum_z, maximum_x, maximum_z);
}

void FarTerrain::clear_edits()
{
    impl_->clear_edits();
}

// mod-world (wave 5)
void FarTerrain::set_mod_palette(std::shared_ptr<const std::vector<ModPaletteLook>> looks)
{
    impl_->set_mod_palette(std::move(looks));
}

FarTerrainFrame FarTerrain::update(const FarView &view, float visibility_distance,
                                   const std::array<float, 3> &light, double seconds,
                                   int maximum_uploads)
{
    return impl_->update(view, visibility_distance, light, seconds, maximum_uploads);
}

void FarTerrain::wait_until_idle()
{
    impl_->wait_until_idle();
}

FarTerrainStats FarTerrain::stats() const
{
    return impl_->stats();
}

} // namespace openminerz::world
