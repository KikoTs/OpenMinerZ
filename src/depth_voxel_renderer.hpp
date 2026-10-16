#pragma once

#include "enemy_renderer.hpp"
#include "openminerz/design_canvas.hpp"
#include "openminerz/graphics_settings.hpp"
#include "openminerz/modding/graphics.hpp"
#include "openminerz/vfx.hpp"
#include "openminerz/world/dynamic_radiance.hpp"
#include "openminerz/world/custom_terrain.hpp" // mod-world
#include "openminerz/world/far_terrain.hpp"
#include "openminerz/world/lava_field.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/sky.hpp"
#include "openminerz/world/world.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace openminerz
{

// What the settings ask of the effects layer (vfx/vfx_renderer.hpp).
struct VfxOptions;

// One section's GPU payload, already interleaved in the renderer's own vertex
// layout. Packing a face batch is pure CPU work on immutable data, so the mesh
// worker does it and the game thread is left with the buffer upload alone.
struct PackedChunkMesh final
{
    world::ChunkCoordinate coordinate{};
    std::vector<std::byte> vertices;
    std::uint32_t vertex_count{};
    std::uint32_t vertex_stride{};
    // (mod-liquid) Translucent liquid faces, after the vertex_count opaque
    // ones in the same buffer (openminerz/world/liquid.hpp).
    std::uint32_t liquid_vertex_count{};
    // Exposed space-rock columns, which drive the crash-crater masks.
    std::vector<std::array<int, 2>> crash_columns;
    // Columns that show lava, for the Enhanced presentation's lava map
    // (openminerz/world/lava_field.hpp). Presentation only.
    std::vector<world::LavaColumn> lava_columns;
};

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
    // Off-thread half of replace_chunk: no device is touched, so a mesh worker
    // may call it. install_packed_chunk then only creates the vertex buffer.
    [[nodiscard]] static PackedChunkMesh pack_chunk(const world::ChunkFaceBatch &batch);
    void install_packed_chunk(PackedChunkMesh packed);
    void retain_chunks(int minimum_chunk_x, int minimum_chunk_z, int maximum_chunk_x,
                       int maximum_chunk_z);
    void set_world_bounds(int minimum_x, int minimum_z, int maximum_x, int maximum_z);
    // World-space entity triangles (skinned enemies, projectiles) drawn depth-tested
    // between the terrain and the water pass. The batch is consumed by the next draw.
    void submit_entities(EntityBatch batch);
    // A texture that entity batches have named is about to be destroyed (an
    // Xbox-avatar skin being replaced or released): forget the shader view
    // cached for it, since the next texture created may reuse its address.
    void forget_entity_texture(SDL_Texture *texture) noexcept;
    // Small world-anchored moving-light volume consumed by the next and later draws
    // until replaced. The payload is CPU-built from authoritative voxel occupancy.
    void submit_dynamic_radiance(world::DynamicRadianceVolume volume);
    // The far landscape for the draws that follow (openminerz/world/
    // far_terrain.hpp): the tiles to upload and what to draw. Enhanced
    // presentation only; an inactive frame, which is also what the Original
    // presentation always has, draws exactly as before far terrain existed.
    void submit_far_terrain(world::FarTerrainFrame frame);
    // mod-world: how the far landscape draws generated mod blocks.
    void set_far_mod_palette(const world::ModPaletteLooks &looks,
                             std::span<const world::BlockType> fallbacks,
                             std::span<const world::FarSurfaceProfile> profiles);
    [[nodiscard]] world::FarTerrainGpuStats far_terrain_stats() const noexcept;
    // The Enhanced presentation's effects layer (openminerz/vfx.hpp,
    // vfx/vfx_renderer.hpp). `vfx_available` is whether this device can draw
    // it at all; `submit_vfx` hands over what the next draw shows (the frame's
    // vectors are swapped, so the caller reuses them). Nothing is drawn, and no
    // light is added, in a frame of the Original presentation.
    [[nodiscard]] bool vfx_available() const noexcept;
    void submit_vfx(vfx::Frame &frame, const VfxOptions &options);
    // --- Mods (graphics area; openminerz/modding/graphics.hpp). Presentation
    // only, both presentations. Every unmodded frame submits nothing here and
    // draws with exactly the pipeline above.
    // Lighting props: this frame's lights (copied), which the next draw lists
    // per view-space cluster on the GPU. An empty frame binds nothing.
    void submit_mod_lights(const modding::graphics::LightFrame &frame);
    // What the last draw's cluster lists held, read back from the GPU (a
    // stall: diagnostics only).
    [[nodiscard]] modding::graphics::LightClusterStats mod_light_statistics();
    // Terrain slices for mod blocks after the retail 64 (BlockTextureTable),
    // each resampled to the pack's tile size and mipmapped with the rest.
    void install_mod_tiles(const std::vector<modding::graphics::Image> &tiles);
    // The three terrain atlases again, after a texture pack changed under a
    // live world (the textures as the constructor takes them).
    void replace_atlases(SDL_Texture *atlas, SDL_Texture *normal_spec_atlas,
                         SDL_Texture *metal_light_atlas);
    // Atmosphere layers whose fog band blends over this renderer's own; the
    // stack must outlive the draws (nullptr: none).
    void set_mod_atmosphere(const modding::graphics::AtmosphereStack *stack) noexcept;
    // A mod's colour-grading LUT over the finished world image of the next
    // draws (nullptr or zero strength: none). The image must outlive them.
    void set_mod_grade(const modding::graphics::Image *lut, std::string_view name,
                       float strength);
    // (Visuals) Grades the current target in place with that LUT: called
    // after the first-person body is composited, so it is graded too.
    void draw_mod_grade();
    // (mod-liquid) The camera is inside a mod liquid of this colour (display
    // 0..1) for the draws that follow: the distance fogs into it within a few
    // blocks, and the liquid's surface is seen from below. Both presentations;
    // without a liquid (the default) nothing changes.
    void set_liquid_view(bool inside, std::array<float, 3> color) noexcept;
    // Display pipeline controls: tone curve index (0 legacy, 1 Reinhard,
    // 2 filmic, 3 ACES), auto exposure, manual EV trim and grade strength.
    void set_grading(int tone_curve, bool auto_exposure, float exposure_compensation,
                     float color_grading) noexcept;
    [[nodiscard]] DisplayGrading display_grading() const noexcept;
    // Player-facing quality controls; the defaults are the values this
    // renderer used before they became settings. shadow_resolution is the side
    // of each sun cascade in texels (512..4096) and shadow_distance how far
    // from the camera, in blocks, sun shadows are still drawn. anisotropy is
    // the terrain sampler's sample count (1 = plain trilinear). Turning bloom
    // off skips its two blur passes; fxaa and sun_shafts select a composite
    // shader compiled without that work rather than multiplying it by zero.
    void set_quality(int shadow_resolution, float shadow_distance, int anisotropy, bool fxaa,
                     bool bloom, bool sun_shafts) noexcept;
    // --- The Enhanced presentation's sky and post-processing chain. ---
    // (post_processing.hpp has the pass order and the viewmodel contract.)
    // None of these does anything for a frame drawn with the Original
    // presentation, whose pipeline is exactly the one above.
    void set_post_effects(const PostEffects &effects) noexcept;
    // Per-frame values the camera does not carry; consumed by the next draw.
    void set_post_view(const PostView &view) noexcept;
    // The installed pack's painted sky cubes for the next draw (face order of
    // WorldPreviewRenderer::SkyCube) and the weight of the second one. Null
    // faces draw the procedural sky alone.
    void set_sky_plates(const std::array<SDL_Texture *, 6> &first,
                        const std::array<SDL_Texture *, 6> &second, float second_weight) noexcept;
    // True when the last draw composed its own sky into the frame, so nothing
    // drawn behind the world can show through it.
    [[nodiscard]] bool draws_sky() const noexcept;
    // The lens pass: vignette, film grain and the player's colour controls,
    // applied in place to whatever SDL is rendering to. Call it after the
    // first-person body has been composited and BEFORE any HUD is drawn, so
    // world and hands are graded alike and the interface is never touched.
    // Does nothing unless the last draw took the Enhanced path.
    void draw_lens(float elapsed_seconds);
    // The Enhanced presentation's deep underground (hell_atmosphere.hpp), for
    // the first-person body, which is drawn outside the lit pass: whether the
    // last draw lit Hell with that model, and the light from lava a surface
    // facing `normal_y` receives at a point, as a share of the lava's own
    // (0..1, its slow pulse included). Presentation only.
    [[nodiscard]] bool deep_light_model() const noexcept;
    [[nodiscard]] float lava_light_at(float x, float y, float z, float normal_y) const noexcept;
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
