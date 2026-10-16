#pragma once

// The Enhanced ("Rebirth") presentation's sky and post-processing chain.
//
// PRESENTATION ONLY, and only for the Enhanced presentation: the Original
// ("Classic") one never constructs a frame for this class, so its picture is
// the 2011 game's, pass for pass.
//
// PASS ORDER of an Enhanced frame (DepthVoxelRenderer::draw drives 1-3 and
// hands the rest to PostProcessor):
//
//    1  sun cascades, held-torch point shadow            (depth only)
//    2  terrain + opaque entities  -> HDR scene, surface data, depth
//       soft particles, decals, water                    (HDR scene only)
//    2b far terrain and THE ATMOSPHERE (far_terrain_renderer.hpp,
//       atmosphere_shader.hpp): the far landscape into a colour and depth
//       target of its own while that setting is on, then the one atmosphere
//       over every pixel of the scene from its own depth, with the far
//       landscape and the open sky's air under what holds no depth
//    3  sky                        -> under everything already in the scene,
//       and so behind the atmosphere's share of each pixel
//    --- PostProcessor::run -------------------------------------------------
//    4  ambient occlusion (half res) + depth-aware blur
//    5  light shafts (half res) + depth-aware blur: a march through the same
//       atmosphere for the sunlight that shadows keep out of it
//    6  resolve: scene x occlusion, contact shadows, the shafts' shadowed
//       light taken out of the air
//    6b the deep underground's air (hell_atmosphere.hpp): lava's glow in the
//       haze, heat shimmer, embers and ash. Only deep underground or beside
//       lava; above ground it is not reached and costs nothing
//    6c the effects layer (vfx/vfx_renderer.hpp): smoke, fire, sparks,
//       shockwaves. After the resolve, so occlusion and contact shadows are
//       never laid over smoke; before motion blur, depth of field and bloom,
//       so all three see it. Skipped outright while no effect is alive
//    7  motion blur                                      (optional)
//    8  depth of field: autofocus, CoC, bokeh gather, composite (optional)
//    9  bloom: down-sample chain, up-sample chain
//   10  composite: exposure, bloom, sun glare, white balance, tone curve,
//       display encode, grade, colour controls, colour fringing, dither
//   11  FXAA                                             (optional)
//   12  contrast-adaptive sharpening                     (optional)
//    --- back in SDL ----------------------------------------------------------
//   13  the first-person body and held item (AvatarViewmodel), drawn with
//       their own projection into their own target and composited on top
//   14  PostProcessor::lens: vignette, film grain and the player's contrast,
//       colour strength and colour temperature, over world + viewmodel, so a
//       held weapon is always graded exactly like the world behind it
//   15  HUD, nametags, menus
//
// THE VIEWMODEL HOOK. The first-person body and held item are a SEPARATE
// LAYER: they never write the scene colour, the scene depth or the surface
// targets, and they are composited after pass 12. That is the whole contract,
// and it is why depth of field cannot blur a rifle as if it were centimetres
// from the lens, ambient occlusion and contact shadows cannot be cast by it
// onto the world, and motion blur leaves it sharp. `viewmodel_is_separate_layer`
// states it for code that wants to assert it. If the viewmodel is ever drawn
// into the scene target instead, it must write stencil bit
// `viewmodel_stencil_bit` and passes 4-8 must reject it.
//
// The 2D interface (pass 15) is drawn by SDL after everything here and is
// never an input to any pass, so no effect can blur or tint it.

#include "openminerz/graphics_settings.hpp"
#include "openminerz/world/sky.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <memory>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

namespace openminerz
{

inline constexpr bool viewmodel_is_separate_layer = true;
inline constexpr unsigned viewmodel_stencil_bit = 0x80U;

// The deep underground's lighting, haze and embers (hell_atmosphere.hpp).
class HellAtmosphere;
// Smoke, fire, sparks and shockwaves (vfx/vfx_renderer.hpp).
class VfxRenderer;

// Diagnostic A/B switches, applied on top of the player's settings and never
// written back: OPENMINERZ_POSTFX="bloom=0,ao=0,vol=0,dof=2,mb=0.5,...".
// See apply_post_effect_overrides in post_processing.cpp for every name.
void apply_post_effect_overrides(PostEffects &effects);

#if defined(_WIN32)

struct PostFrame final
{
    int width{};
    int height{};
    ID3D11Texture2D *scene_resource{};
    ID3D11RenderTargetView *scene_target{};
    ID3D11ShaderResourceView *scene_view{};
    ID3D11DepthStencilView *depth_readonly{};
    ID3D11ShaderResourceView *depth_view{};
    // The renderer's scene constants (register b0) and samplers s0 (linear
    // clamp) and s1 (shadow comparison).
    ID3D11Buffer *scene_constants{};
    ID3D11SamplerState *linear_sampler{};
    ID3D11SamplerState *shadow_sampler{};
    // Sun cascades, or null when no sun shadow was rendered this frame.
    ID3D11ShaderResourceView *shadow_view{};
    ID3D11ShaderResourceView *shadow_transmission_view{};
    // 1x1 adapted log luminance, or null for manual exposure.
    ID3D11ShaderResourceView *adapted_view{};
    // Presentation-format target the finished frame is written to.
    ID3D11RenderTargetView *output_target{};

    std::array<float, 3> eye{};
    std::array<float, 3> right{};
    std::array<float, 3> up{};
    std::array<float, 3> forward{};
    // x scale, y scale, z scale, z offset (the renderer's Projection).
    std::array<float, 4> projection{};
    float vertical_fov_degrees{74.0F};
    float elapsed_seconds{};
    float delta_seconds{1.0F / 60.0F};
    world::SkyState sky{};
    PostView view{};
    // Farthest camera depth the sun cascades cover.
    float shadow_far{150.0F};
    float fog_end{118.0F};
    // The deep underground (hell_atmosphere.hpp), or null. When its frame state
    // asks for the air pass, run() draws it between the resolve and motion
    // blur. `deep_grade` (0..1) is how much of its warm vignette the lens adds.
    HellAtmosphere *hell{};
    float deep_grade{};
    // The effects layer, or null. run() draws it (pass 6c) between the resolve
    // and motion blur whenever it has something to show.
    VfxRenderer *vfx{};
    // Pass 2b laid the atmosphere (atmosphere_shader.hpp) over this frame and
    // left its constants bound: the light-shaft pass marches it, and nothing
    // here adds fog of its own.
    bool atmosphere{};
};

class PostProcessor final
{
  public:
    PostProcessor(ID3D11Device *device, ID3D11DeviceContext *context);
    ~PostProcessor();

    PostProcessor(const PostProcessor &) = delete;
    PostProcessor &operator=(const PostProcessor &) = delete;

    // Every shader compiled and every fixed resource created. Compilation
    // happens once, here, at load: no pass compiles anything during play.
    [[nodiscard]] bool ready() const noexcept;
    void set_effects(const PostEffects &effects) noexcept;
    [[nodiscard]] const PostEffects &effects() const noexcept;

    // Size-dependent targets. Returns false (and releases them) when the
    // driver refuses one, in which case the renderer falls back to its
    // original chain for the frame.
    [[nodiscard]] bool resize(int width, int height, DXGI_FORMAT output_format);
    // Surface data written beside the lit colour: 0 normal and baked
    // occlusion, 1 light shares and flags, 2 per-object screen motion (only
    // while motion blur is on; otherwise null). Null until resize() succeeded.
    [[nodiscard]] ID3D11RenderTargetView *surface_target(std::size_t index) const noexcept;
    // The same data to read (index 0 or 1), for the far-terrain composite.
    [[nodiscard]] ID3D11ShaderResourceView *surface_view(std::size_t index) const noexcept;
    void clear_surface_targets();
    // Call once per frame before anything is drawn into the scene: fixes the
    // frame's constants, among them last frame's camera, which the entity
    // vertex shader reads (camera_constants(), register b2) to work out how far
    // each vertex moved on screen.
    void begin_frame(const PostFrame &frame);
    [[nodiscard]] ID3D11Buffer *camera_constants() const noexcept;
    // The installed pack's painted sky for this frame: six faces of one cube,
    // six of the cube it is blending toward, and that second cube's weight.
    // `bound` false draws the procedural sky alone.
    void set_sky_plates(const std::array<ID3D11ShaderResourceView *, 12> &plates,
                        float second_weight, bool bound) noexcept;

    // Pass 3: the sky, blended UNDER whatever the scene already holds.
    void draw_sky(const PostFrame &frame);
    // Passes 4-12. Leaves the finished frame in frame.output_target.
    void run(const PostFrame &frame);
    // Pass 14: vignette, film grain and the player's contrast, colour strength
    // and colour temperature, applied in place to `target` (the output the
    // world AND the first-person body have been composited on). A no-op, and
    // lens_needed() false, while every one of them is neutral.
    [[nodiscard]] bool lens_needed() const noexcept;
    void lens(ID3D11RenderTargetView *target, float elapsed_seconds);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

} // namespace openminerz
