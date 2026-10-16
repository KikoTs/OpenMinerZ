#pragma once

// The Enhanced ("Rebirth") presentation's deep underground: Hell as a place you
// can see, that glows, and that feels hot.
//
// PRESENTATION ONLY, Enhanced only, and only below ground. Nothing here reads
// or changes the voxel light field, lava damage, enemy behaviour or anything
// the simulation or the network sees. The Original ("Classic") presentation
// never binds any of it: its constants read as zero and every branch that uses
// them is skipped, so its fade to black below Y=-32 is pixel for pixel what it
// was.
//
// WHAT IT ADDS, and where each part lives:
//
//   lit pass (terrain and creatures, depth_voxel_renderer.cpp; the shader text
//   is shader_library() below)
//     - the cavern's own glow: a dim, warm fill that is stronger from below, so
//       unlit rock is dark red-brown instead of black and still has shape
//     - lava as an AREA light: the lava map (openminerz/world/lava_field.hpp)
//       read at a footprint as wide as the surface is high above the lava. The
//       voxel light field keeps the first blocks; this carries the lake's light
//       on up the cavern walls to the roof
//     - molten lava: drifting crust, cracks that glow, a slow breath
//
//   air pass (PostProcessor::run, after the resolve and before motion blur,
//   depth of field and bloom, so all three see it)
//     - glow in the air above lava and a thin warm haze through the deep
//       layer, marched to each pixel: depth reads as layers of light instead
//       of a fade to one flat colour
//     - heat haze: the picture shimmers where the sight line crosses hot air.
//       It runs before the first-person body and the interface are drawn, so
//       it can never move either
//     - embers rising from lava and ash drifting through the deep layer
//
//   and, through the values begin_frame() returns: how far auto exposure may
//   open in Hell, and the lens's warm vignette there.
//
// SMOOTH BY CONSTRUCTION. Every term is a continuous function of where a
// surface or a point of air is (how deep, how high above which lava), never of
// a switch: walking down from a cave, the light arrives with the depth; looking
// down a shaft from above, the bottom is lit and the cave around the eye is
// not; lava met in daylight shimmers and sheds embers but does not tint the
// surface.

#include "openminerz/graphics_settings.hpp"
#include "openminerz/world/lava_field.hpp"
#include "openminerz/world/world.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

namespace openminerz
{

// Every column of a mesh section that shows lava, highest surface first where
// a column shows two. Pure CPU work on the faces the mesher already emitted,
// so the mesh worker does it.
[[nodiscard]] std::vector<world::LavaColumn>
scan_lava_columns(const std::vector<world::WorldFace> &faces);

// What one frame tells the deep-underground model.
struct HellFrameInput final
{
    // The Enhanced presentation with its post-processing chain. False draws
    // nothing and zeroes every constant.
    bool enhanced{};
    std::array<float, 3> eye{};
    // SkyState::hell: how deep the eye is, 0 above Hell .. 1 in it.
    float eye_presence{};
    // Baked skylight at the eye and the sky's daylight: lava met under an open
    // noon sky glows less into the air.
    float eye_sky_light{};
    float daylight{};
    float elapsed_seconds{};
    int width{1};
    int height{1};
    PostEffects effects{};
};

// What the rest of the frame needs back.
struct HellFrameState final
{
    // The lit pass uses the deep-underground model (and not the older flat red
    // fill, which remains the fallback without the post-processing chain).
    bool model{};
    // The air pass will run: glow, haze and embers are worth drawing here.
    bool air{};
    bool embers{};
    // How far auto exposure may open this frame (1 everywhere but Hell).
    float exposure_limit{1.0F};
    // 0..1: how much of the deep layer's warm vignette the lens applies.
    float deep_grade{};
};

#if defined(_WIN32)

class HellAtmosphere final
{
  public:
    // Compiles every shader and creates every fixed resource, here, at load.
    HellAtmosphere(ID3D11Device *device, ID3D11DeviceContext *context);
    ~HellAtmosphere();

    HellAtmosphere(const HellAtmosphere &) = delete;
    HellAtmosphere &operator=(const HellAtmosphere &) = delete;

    [[nodiscard]] bool ready() const noexcept;

    // HLSL that the terrain and creature pixel shaders are compiled with: the
    // constants (register b3), the lava map (t20), a noise texture (t21), their
    // samplers (s5, s6) and the functions that light a surface. Self-contained;
    // it declares nothing the shaders it joins already declare.
    [[nodiscard]] static const std::string &shader_library();

    // Where the lava is. The renderer feeds it as chunk meshes come and go.
    [[nodiscard]] world::LavaField &lava() noexcept;

    // Once per frame, before anything is drawn: follows the eye with the lava
    // map, uploads what changed, and fixes the frame's constants.
    HellFrameState begin_frame(const HellFrameInput &input);
    [[nodiscard]] const HellFrameState &state() const noexcept;

    // Constants, map and noise on their own registers for the vertex and pixel
    // stages. They collide with nothing, so they stay bound for the frame.
    void bind();
    void unbind();

    // The air pass: `scene` with glow and haze, written to `target`.
    void draw_air(ID3D11ShaderResourceView *scene, ID3D11RenderTargetView *target,
                  ID3D11ShaderResourceView *depth, ID3D11Buffer *scene_constants, int width,
                  int height);
    // Embers and ash over `target`, depth-tested against the scene.
    void draw_embers(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth_readonly,
                     ID3D11Buffer *scene_constants, int width, int height);

    // Light from lava at a point, as a share of the lava's own (0..1): the area
    // light a surface facing `normal_y` receives there. For the first-person
    // body, which is drawn outside the lit pass.
    [[nodiscard]] float lava_light_at(float x, float y, float z, float normal_y) const noexcept;
    // The lava light's slow pulse this frame (about 1).
    [[nodiscard]] float pulse() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

} // namespace openminerz
