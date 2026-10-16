#pragma once

// GPU half of the far terrain (openminerz/world/far_terrain.hpp) and the
// Enhanced presentation's one atmosphere (atmosphere_shader.hpp,
// openminerz/world/surface_atmosphere.hpp).
//
// PRESENTATION ONLY, Enhanced ("Rebirth") presentation only: the Original
// presentation never draws through this class.
//
// WHERE IT SITS IN A FRAME (post_processing.hpp has the full pass order):
//
//    2   terrain + entities + effects -> HDR scene, depth    (unchanged)
//    2b  (i)  FAR GEOMETRY -> its own colour and depth targets, when far
//        terrain is on. Reversed, infinite depth of its own, so kilometres of
//        terrain sort exactly and the scene's depth buffer keeps all of its
//        precision for what is near. Lit like the detailed terrain: the block
//        textures themselves, the same sky and sun terms, and a shadow field
//        swept from the height data.
//        (ii) THE ATMOSPHERE, always. Every pixel takes the air between it
//        and the eye from the one function of atmosphere_shader.hpp, at its
//        own depth:
//          - where the scene holds depth, over what is there; and where the
//            detailed terrain blends out at the edge of its window, the far
//            landscape standing in the same place is blended in (a hand-over
//            of geometry; the air does not know where that edge is);
//          - where it holds none, the far landscape and the open sky's air go
//            UNDER what is there, so effects in front of the sky stay in front.
//    3   sky, under the scene: it fills what the air left of the alpha, which
//        is how the sky itself is fogged down to the horizon.
//    4-  occlusion, light shafts (which march the same atmosphere and take
//        out the sunlight that shadows keep from it), resolve ...
//
// To the passes after it a far pixel has no entry in the scene's depth
// buffer: it takes no screen-space occlusion and is at infinity for the depth
// of field. The light-shaft pass reads the far depth this class leaves bound.
//
// Nothing here is in the way of a near pixel: far geometry is not drawn
// inside the detailed window at all.

#include "openminerz/world/custom_terrain.hpp" // mod-world
#include "openminerz/world/far_terrain.hpp"
#include "openminerz/world/sky.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

namespace openminerz
{

using FarTerrainGpuStats = world::FarTerrainGpuStats;

#if defined(_WIN32)

// What the voxel renderer hands over for one frame.
struct FarTerrainPass final
{
    int width{};
    int height{};
    // The HDR scene (before the sky goes under it) and its depth, read-only.
    ID3D11RenderTargetView *scene_target{};
    ID3D11ShaderResourceView *depth_view{};
    // Surface data of the lit pass: y baked skylight, z entity.
    ID3D11ShaderResourceView *surface_view{};
    // The terrain's diffuse tiles as a mipmapped array.
    ID3D11ShaderResourceView *atlas_view{};
    // The renderer's scene constants (register b0).
    ID3D11Buffer *scene_constants{};
    std::array<double, 3> eye{};
    world::SkyState sky{};
    float storminess{};
    float eye_sky_light{1.0F};
    float elapsed_seconds{};
    // How far the detailed window lets the eye see, in blocks: where the view
    // ends when there is no far landscape.
    float visibility{128.0F};
    // Opacity of sun shadows this frame (0 when they are off or the sun is down).
    float shadow_opacity{};
};

class FarTerrainRenderer final
{
  public:
    FarTerrainRenderer(ID3D11Device *device, ID3D11DeviceContext *context);
    ~FarTerrainRenderer();
    FarTerrainRenderer(const FarTerrainRenderer &) = delete;
    FarTerrainRenderer &operator=(const FarTerrainRenderer &) = delete;

    // Every shader compiled and every fixed resource created, at load.
    [[nodiscard]] bool ready() const noexcept;
    // The landscape for the frames that follow: uploads the cell maps and
    // shadow fields it brings. An inactive frame turns the passes off.
    void submit(world::FarTerrainFrame frame);
    // True when the next draw() will put far terrain into the scene, which is
    // also when the detailed terrain's edge fade becomes the hand-over.
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] world::FarHandover handover() const noexcept;
    // The atmosphere over the scene, with the far landscape under it when
    // there is one. Leaves the atmosphere's constants (register b8) and the
    // far depth (t24) bound for the light-shaft pass.
    void draw(const FarTerrainPass &pass);
    // True once draw() has laid the atmosphere over the current frame.
    [[nodiscard]] bool atmosphere_drawn() const noexcept;

    // The shadow field of the far landscape for the detailed terrain's own
    // lit pass (pixel-shader registers b7, t22-t23 and s8; see
    // far_field_shadow() in depth_voxel_renderer.cpp): where a mountain beyond
    // the reach of the sun cascades still shades the ground near the player,
    // so its evening shadow does not stop at the edge of the detailed window.
    // `opacity` is how much of the sun a shadow takes this frame. With far
    // terrain off this binds zeros, which switches the shader's term off.
    void bind_near_shadow(const std::array<double, 3> &eye, float elapsed_seconds, float opacity);
    void unbind_near_shadow();

    [[nodiscard]] FarTerrainGpuStats stats() const noexcept;

    // mod-world: how generated mod blocks are drawn (palette index - 1: the
    // block's top, side and bottom tiles, and the retail block it stands in
    // for); an empty list (or an entry without tiles) draws the fallback.
    // (fix, wave 5) `profiles`: the generator's side profiles
    // (CustomTerrain::far_profiles), drawn from those same looks.
    void set_mod_palette(const world::ModPaletteLooks &looks,
                         std::span<const world::BlockType> fallbacks,
                         std::span<const world::FarSurfaceProfile> profiles);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

} // namespace openminerz
