// (mod-liquid) The world renderer's side of liquid mod blocks
// (openminerz/world/liquid.hpp): the table the terrain is meshed with, and
// liquids beyond the terrain window for the host's checks.

#include "world_preview_renderer.hpp"

#include "openminerz/world/liquid.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <utility>

namespace openminerz
{
namespace
{

[[nodiscard]] int floor_divide(int value, int divisor) noexcept
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

} // namespace

void WorldPreviewRenderer::set_liquids(std::shared_ptr<const world::LiquidTable> table)
{
    if (table != nullptr && table->empty())
        table.reset();
    if (table == liquids_ || (table != nullptr && liquids_ != nullptr && table->same_as(*liquids_)))
    {
        liquids_ = std::move(table);
        return;
    }
    const bool generated = world::LiquidTable::generated_differ(liquids_.get(), table.get());
    const std::vector<std::array<int, 3>> placed =
        world::LiquidTable::changed_placed(liquids_.get(), table.get());
    liquids_ = std::move(table);
    // The sections whose faces may change: every one of a column that holds
    // generated mod blocks when the liquids themselves changed, and around
    // each placed cell that changed its own and its neighbours' (a face
    // toward a liquid is drawn where one toward its fallback would not be).
    std::set<std::tuple<int, int, int>> sections;
    if (generated)
        for (const world::Chunk &chunk : region_.chunks())
            if (chunk.has_mod_cells() && render_window_.contains(chunk.chunk_x(), chunk.chunk_z()))
                for (int section = 0; section < world::chunk_section_count; ++section)
                    sections.emplace(chunk.chunk_x(), chunk.chunk_z(), section);
    for (const auto &[x, y, z] : placed)
        for (const auto &[dx, dy, dz] : std::array<std::array<int, 3>, 7>{
                 {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}})
        {
            const int section_y = (y + dy - world::world_min_y) / world::chunk_section_height;
            if (section_y < 0 || section_y >= world::chunk_section_count)
                continue;
            sections.emplace(floor_divide(x + dx, world::chunk_width),
                             floor_divide(z + dz, world::chunk_depth), section_y);
        }
    std::vector<world::ChunkCoordinate> changed;
    changed.reserve(sections.size());
    for (const auto &[x, z, y] : sections)
        changed.push_back({x, z, y});
    if (!changed.empty())
        queue_chunk_rebuilds(std::move(changed), !generated);
}

std::uint8_t WorldPreviewRenderer::baseline_mod_cell(int x, int y, int z) const
{
    if (y < world::world_min_y || y >= world::world_min_y + world::chunk_height ||
        generator_.custom_terrain() == nullptr || remembered_block(x, y, z).has_value())
        return 0U;
    // resolved_block caches the chunk this reads.
    if (!resolved_block(x, y, z).has_value())
        return 0U;
    const int chunk_x = floor_divide(x, world::chunk_width);
    const int chunk_z = floor_divide(z, world::chunk_depth);
    const auto found = authority_baselines_.find({chunk_x, chunk_z, 0});
    if (found == authority_baselines_.end())
        return 0U;
    return found->second.mod_cell(x - chunk_x * world::chunk_width, y - world::world_min_y,
                                  z - chunk_z * world::chunk_depth);
}

const world::LiquidTable::Kind *WorldPreviewRenderer::eye_liquid(const world::PlayerState &player) const
{
    if (liquids_ == nullptr)
        return nullptr;
    const float eye = player.position.y + world::PlayerSimulation::eye_height;
    const std::optional<float> surface =
        liquids_->liquid(region_, static_cast<int>(std::floor(player.position.x)),
                         static_cast<int>(std::floor(eye)), static_cast<int>(std::floor(player.position.z)))
            ? world::liquid_surface(region_, player.position.x, eye, player.position.z)
            : std::nullopt;
    if (!surface.has_value() || eye >= *surface)
        return nullptr;
    return liquids_->kind(liquids_->kind_at(region_, static_cast<int>(std::floor(player.position.x)),
                                            static_cast<int>(std::floor(eye)),
                                            static_cast<int>(std::floor(player.position.z))));
}

bool WorldPreviewRenderer::resolved_liquid(int x, int y, int z) const
{
    if (liquids_ == nullptr)
        return false;
    if (region_.contains(x, y, z))
        return liquids_->liquid(region_, x, y, z);
    const std::optional<world::BlockType> block = resolved_block(x, y, z);
    return block.has_value() &&
           liquids_->kind_of(x, y, z, *block, generated_mod_cell(x, y, z, false)) != 0U;
}

} // namespace openminerz
