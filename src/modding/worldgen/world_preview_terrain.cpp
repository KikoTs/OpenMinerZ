// mod-world: the world renderer's side of a mod world generator
// (openminerz/world/custom_terrain.hpp): installing one, the looks of the mod
// blocks it generates, and their identity for the gameplay service.

#include "world_preview_renderer.hpp"

#include "depth_voxel_renderer.hpp"

#include "openminerz/world/custom_terrain.hpp"

#include <algorithm>
#include <utility>

namespace openminerz
{

void WorldPreviewRenderer::set_custom_terrain(std::shared_ptr<const world::CustomTerrain> terrain)
{
    const auto &installed = generator_.custom_terrain();
    if (installed == terrain ||
        (installed != nullptr && terrain != nullptr && installed->digest() == terrain->digest()))
        return;
    // Nothing generated or meshed from the old terrain may land after this.
    cancel_stream_job();
    discard_pending_mesh();
    generator_.set_custom_terrain(std::move(terrain));
    authority_baselines_.clear();
    // (fix, wave 5) Its side profiles are drawable before any mod look is.
    push_far_mod_table();
    // The resident window again, every column at once (spread over the
    // machine's cores by stream_region), with the edits this client knows.
    const ChunkRect rect = region_rect();
    const world::WorldRegion stand_in{rect.maximum_x + 1'000, rect.minimum_z, 1, 1};
    region_ = generator_.stream_region(stand_in, rect.minimum_x, rect.minimum_z,
                                       rect.maximum_x - rect.minimum_x + 1,
                                       rect.maximum_z - rect.minimum_z + 1);
    apply_saved_edits();
    stats_.chunk_count = region_.chunks().size();
    biome_ = generator_.biome_at(focus_chunk_x_ * world::chunk_width + 8,
                                 focus_chunk_z_ * world::chunk_depth + 8);
    pending_biome_ = biome_;
    // The far landscape follows the same generator, with the same edits.
    if (far_terrain_ != nullptr)
    {
        const world::FarTerrainSettings settings = far_terrain_->settings();
        far_terrain_ = std::make_unique<world::FarTerrain>(generator_);
        for (const auto &[coordinate, block] : saved_edits_)
            far_terrain_->set_edit(coordinate.x, coordinate.y, coordinate.z, block);
        far_terrain_->set_settings(settings);
        far_terrain_settled_ = false;
        far_terrain_quiet_since_ = -1.0F;
    }
    remesh_everything();
    // (fix) The stream cancelled above may have been the one growing a new
    // world's first few chunks to the resident window: nothing else starts
    // one before the world has loaded, so the loading screen never ended.
    begin_stream_to_simulation_window(biome_);
}

void WorldPreviewRenderer::remesh_everything()
{
    discard_pending_mesh();
    mesh_batches_.clear();
    reindex_mesh_batches();
    faces_dirty_ = true;
    pending_uploads_.clear();
    pending_mesh_rebuilds_.clear();
    pending_lookup_.clear();
    pending_priority_ = 0U;
    // An empty window makes the next sync queue every section in view; until
    // their meshes arrive the old ones stay on the GPU.
    render_window_ = {};
    sync_render_window();
    stats_.stream_pending = true;
}

void WorldPreviewRenderer::set_mod_palette(std::shared_ptr<const world::ModPaletteLooks> looks)
{
    if (looks != nullptr && looks->empty())
        looks.reset();
    if (looks == mod_palette_ ||
        (looks != nullptr && mod_palette_ != nullptr && *looks == *mod_palette_))
        return;
    mod_palette_ = std::move(looks);
    // The far landscape draws them with the same tiles.
    push_far_mod_table();
    // Only sections of columns that hold generated mod blocks change.
    std::vector<world::ChunkCoordinate> changed;
    for (const world::Chunk &chunk : region_.chunks())
        if (chunk.has_mod_cells() && render_window_.contains(chunk.chunk_x(), chunk.chunk_z()))
            for (int section = 0; section < world::chunk_section_count; ++section)
                changed.push_back({chunk.chunk_x(), chunk.chunk_z(), section});
    if (!changed.empty())
        queue_chunk_rebuilds(std::move(changed));
}

void WorldPreviewRenderer::push_far_mod_table()
{
    const auto &terrain = generator_.custom_terrain();
    if (depth_renderer_ == nullptr || terrain == nullptr)
        return;
    depth_renderer_->set_far_mod_palette(mod_palette_ != nullptr ? *mod_palette_ : world::ModPaletteLooks{},
                                         terrain->definition().palette_fallback, terrain->far_profiles());
}

std::uint8_t WorldPreviewRenderer::generated_mod_cell(int x, int y, int z, bool before_change) const
{
    if (generator_.custom_terrain() == nullptr)
        return 0U;
    // (mod-liquid) Beyond the terrain window, the generated baseline the
    // authority resolves blocks from (resolved_block), unless an edit
    // replaced the cell: a guest's lake far from the host is a lake too.
    if (!region_.contains(x, y, z))
        return before_change ? 0U : baseline_mod_cell(x, y, z);
    const int chunk_x = x >= 0 ? x / world::chunk_width : (x - world::chunk_width + 1) / world::chunk_width;
    const int chunk_z = z >= 0 ? z / world::chunk_depth : (z - world::chunk_depth + 1) / world::chunk_depth;
    const world::Chunk &chunk =
        region_.chunks()[static_cast<std::size_t>((chunk_x - region_.origin_chunk_x()) +
                                                  (chunk_z - region_.origin_chunk_z()) *
                                                      region_.chunks_x())];
    const int local_x = x - chunk_x * world::chunk_width;
    const int local_y = y - world::world_min_y;
    const int local_z = z - chunk_z * world::chunk_depth;
    return before_change ? chunk.previous_mod_cell(local_x, local_y, local_z)
                         : chunk.mod_cell(local_x, local_y, local_z);
}

std::optional<std::uint8_t> WorldPreviewRenderer::generated_mod_tile(int x, int y, int z,
                                                                     world::BlockType block,
                                                                     world::FaceDirection face) const
{
    if (mod_palette_ == nullptr || generator_.custom_terrain() == nullptr || !region_.contains(x, y, z))
        return std::nullopt;
    // A chip flies as its block is dug out: by then the cell may already hold
    // what replaced it, and the generated block is what it held before.
    const std::uint8_t palette = generated_mod_cell(x, y, z, region_.block(x, y, z) != block);
    const auto &fallbacks = generator_.custom_terrain()->definition().palette_fallback;
    if (palette == 0U || palette > mod_palette_->size() || palette > fallbacks.size() ||
        fallbacks[palette - 1U] != block || !(*mod_palette_)[palette - 1U].valid)
        return std::nullopt;
    const std::size_t slot = face == world::FaceDirection::positive_y   ? 0U
                             : face == world::FaceDirection::negative_y ? 2U
                                                                        : 1U;
    return (*mod_palette_)[palette - 1U].tiles[slot];
}

} // namespace openminerz
