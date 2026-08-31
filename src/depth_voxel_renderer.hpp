#pragma once

#include "enemy_renderer.hpp"
#include "openminerz/design_canvas.hpp"
#include "openminerz/world/dynamic_radiance.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/sky.hpp"
#include "openminerz/world/world.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <string_view>
#include <vector>

namespace openminerz
{

// What the tone-mapped world pass used this frame. The sky is drawn outside
// that pass, so it applies the same numbers to stay consistent.
struct DisplayGrading final
{
    float exposure{1.0F};
    int tone_curve{3};
    float color_grading{1.0F};
};

class DepthVoxelRenderer final
{
  public:
    DepthVoxelRenderer(SDL_Renderer *renderer, SDL_Texture *atlas, SDL_Texture *normal_spec_atlas,
                       SDL_Texture *metal_light_atlas);
    ~DepthVoxelRenderer();

    DepthVoxelRenderer(const DepthVoxelRenderer &) = delete;
    DepthVoxelRenderer &operator=(const DepthVoxelRenderer &) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::string_view failure_reason() const noexcept;
    void rebuild(const std::vector<world::WorldFace> &faces, int minimum_x, int minimum_z,
                 int maximum_x, int maximum_z);
    void rebuild_chunks(const std::vector<world::ChunkFaceBatch> &batches, int minimum_x,
                        int minimum_z, int maximum_x, int maximum_z);
    void replace_chunk(const world::ChunkFaceBatch &batch);
    void retain_chunks(int minimum_chunk_x, int minimum_chunk_z, int maximum_chunk_x,
                       int maximum_chunk_z);
    void set_world_bounds(int minimum_x, int minimum_z, int maximum_x, int maximum_z);
    // World-space entity triangles (skinned enemies, projectiles) drawn depth-tested
    // between the terrain and the water pass. The batch is consumed by the next draw.
    void submit_entities(EntityBatch batch);
    // Small world-anchored moving-light volume consumed by the next and later draws
    // until replaced. The payload is CPU-built from authoritative voxel occupancy.
    void submit_dynamic_radiance(world::DynamicRadianceVolume volume);
    // Display pipeline controls: tone curve index (0 legacy, 1 Reinhard,
    // 2 filmic, 3 ACES), auto exposure, manual EV trim and grade strength.
    void set_grading(int tone_curve, bool auto_exposure, float exposure_compensation,
                     float color_grading) noexcept;
    [[nodiscard]] DisplayGrading display_grading() const noexcept;
    [[nodiscard]] bool draw(const ui::UiMetrics &metrics, const world::SkyState &sky,
                            const world::PlayerState &player, float vertical_fov_degrees,
                            float render_scale, float elapsed_seconds, float visibility_distance,
                            world::BiomeKind biome, bool water_enabled, bool enhanced_foliage,
                            bool ambient_occlusion, bool ore_glow, float shadow_strength);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openminerz
