#pragma once

// Draws a vfx::Frame (openminerz/vfx.hpp) for the Enhanced presentation.
//
// PRESENTATION ONLY, Enhanced only. The Original ("Classic") presentation never
// constructs a frame for this class, binds none of its buffers and keeps its
// own particle path exactly.
//
// WHERE IT RUNS (post_processing.hpp has the whole pass order):
//
//   before pass 2   begin_frame(): the frame's effect lights go into a constant
//                   buffer (register b9) that the terrain and creature pixel
//                   shaders read, so an explosion lights what is around it
//   pass 6c         draw(): after the resolve (so ambient occlusion and contact
//                   shadows are never laid over smoke) and before motion blur,
//                   depth of field and bloom (so all three see the effects):
//                     1. scene depth to a half-resolution linear depth
//                     2. gas and glows, sorted, premultiplied, into a
//                        half-resolution HDR buffer: lit by sun, sky, shadow
//                        cascades and effect lights, normal-mapped from a baked
//                        flipbook, soft against the scene, under the atmosphere
//                     3. shockwaves and heat into a refraction buffer
//                     4. one composite: the scene bent by 3, the smoke the eye
//                        stands in as a fog, 2 upsampled depth-aware, the flash
//                     5. sparks and embers, additive, at full resolution
//
// Every shader is compiled and every texture baked in the constructor, at
// load. The flipbooks are generated on the GPU from noise; no art is loaded.

#include "openminerz/vfx.hpp"

#include <memory>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

namespace openminerz
{

// The constant buffer slot of the effect lights, and how many fit.
inline constexpr unsigned vfx_light_slot = 9U;
inline constexpr unsigned vfx_max_lights = 16U;

// What the player's settings ask of the effects for one frame.
struct VfxOptions final
{
    // Lights from effects on terrain, creatures and smoke (0 = none).
    unsigned max_lights{12U};
    bool distortion{true};
    // 0..1 strength of the brief brightening of the picture.
    float flash{1.0F};
    // 2 draws gas at half resolution, 1 at full.
    int resolution_divisor{2};
};

#if defined(_WIN32)

struct VfxPass final
{
    int width{};
    int height{};
    ID3D11ShaderResourceView *scene{};
    ID3D11RenderTargetView *target{};
    ID3D11ShaderResourceView *depth{};
    ID3D11Buffer *scene_constants{};
    ID3D11SamplerState *shadow_sampler{};
    // Sun cascades, or null when none was rendered.
    ID3D11ShaderResourceView *shadow{};
    float elapsed_seconds{};
    // The one atmosphere (atmosphere_shader.hpp) is bound at register b8.
    bool atmosphere{};
    float eye_sky_light{1.0F};
};

class VfxRenderer final
{
  public:
    VfxRenderer(ID3D11Device *device, ID3D11DeviceContext *context);
    ~VfxRenderer();

    VfxRenderer(const VfxRenderer &) = delete;
    VfxRenderer &operator=(const VfxRenderer &) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const char *failure() const noexcept;

    // The effects of the next draw (moved in; the caller's vectors are reused).
    void submit(vfx::Frame &frame, const VfxOptions &options);
    // Uploads the lights and binds them for the lit pass. With `enabled` false
    // (any frame the Enhanced chain does not draw) the lit shaders see none.
    void begin_frame(bool enabled);
    // Whether draw() has anything to put on the screen this frame.
    [[nodiscard]] bool wants_draw() const noexcept;
    // Pass 6c. Sets its own pipeline state and leaves no target bound.
    void draw(const VfxPass &pass);
    void unbind();

    // The lights of the last submitted frame, for the first-person body.
    [[nodiscard]] const vfx::Frame &frame() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

} // namespace openminerz
