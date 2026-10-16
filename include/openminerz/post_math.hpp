#pragma once

// The arithmetic behind the post-processing chain, kept free of any graphics
// API so it can be unit-tested. The shaders in post_processing.cpp evaluate the
// same expressions; the constants they need are computed here on the CPU.

#include <algorithm>
#include <array>
#include <cmath>

namespace openminerz::post
{

inline constexpr float pi = 3.14159265358979323846F;

// --- Depth of field -----------------------------------------------------------
// A thin lens in front of a 200 mm tall film: an 8x10 large-format camera.
// The size is deliberate. With a 35 mm frame a wide-angle lens holds everything
// past a few metres in focus, which is correct and, on a monitor, invisible;
// large format is what gives the shallow focus that reads as a camera. One
// block is one metre, so every distance below is in metres.
inline constexpr float sensor_height = 0.200F;
// "Always" mode away from the sights: the lens stopped down to this share of
// its aperture, so ordinary play keeps a deep focus.
inline constexpr float resting_aperture = 0.45F;

// The lens that frames `vertical_fov_degrees` on that sensor. Zooming in
// (shouldering a weapon) lengthens it, which is what deepens the blur.
[[nodiscard]] inline float focal_length(float vertical_fov_degrees) noexcept
{
    const float fov = std::clamp(vertical_fov_degrees, 1.0F, 170.0F) * pi / 180.0F;
    return sensor_height * 0.5F / std::tan(fov * 0.5F);
}

// Strength 0..1 opens the aperture from f/11 to f/1.0 on a logarithmic scale,
// the way stops work: every 0.143 of the slider is one stop.
[[nodiscard]] inline float f_number(float strength) noexcept
{
    return 11.0F * std::pow(1.0F / 11.0F, std::clamp(strength, 0.0F, 1.0F));
}

// The lens constant K of coc = K * |1/focus - 1/depth|, where coc is the blur
// circle's DIAMETER as a fraction of the picture height. From the thin-lens
// equation: coc = A * f * |d - s| / (d * (s - f)) / sensor, with aperture
// A = f / N; s - f is taken as s because the focus is always far beyond f.
[[nodiscard]] inline float lens_constant(float vertical_fov_degrees, float strength) noexcept
{
    const float f = focal_length(vertical_fov_degrees);
    return f * f / (f_number(strength) * sensor_height);
}

// Signed blur diameter as a fraction of the picture height: negative in front
// of the focus plane, positive behind it, zero on it. Never exceeds `limit`.
[[nodiscard]] inline float circle_of_confusion(float depth, float focus, float constant,
                                               float limit) noexcept
{
    const float d = std::max(depth, 0.01F);
    const float s = std::max(focus, 0.01F);
    return std::clamp(constant * (1.0F / s - 1.0F / d), -limit, limit);
}

// Autofocus works in dioptres (1 / distance) so a pull from 2 m to 4 m takes as
// long as one from 20 m to 40 m, and eases exponentially. Pulling toward the
// viewer is quicker than letting go, like an eye.
[[nodiscard]] inline float autofocus_step(float current_dioptres, float target_dioptres,
                                          float delta_seconds) noexcept
{
    const float rate = target_dioptres > current_dioptres ? 9.0F : 5.0F;
    const float blend = 1.0F - std::exp(-std::max(delta_seconds, 0.0F) * rate);
    return current_dioptres + (target_dioptres - current_dioptres) * std::clamp(blend, 0.0F, 1.0F);
}

// --- Bloom --------------------------------------------------------------------
inline constexpr int maximum_bloom_levels = 7;

// Halvings of the frame until the shorter side would drop under 8 pixels.
[[nodiscard]] inline int bloom_level_count(int width, int height) noexcept
{
    int levels = 0;
    int side = std::min(width, height) / 2;
    while (side >= 8 && levels < maximum_bloom_levels)
    {
        ++levels;
        side /= 2;
    }
    return std::max(levels, 1);
}

// The up-sample pass blends level i with the already combined coarser levels:
// combined_i = lerp(level_i, combined_(i+1), scatter). Expanded, level i ends
// up with this share of the result. The shares always sum to exactly one,
// which is what makes the bloom energy conserving: it moves light, it never
// adds any.
[[nodiscard]] inline float bloom_level_weight(int level, int count, float scatter) noexcept
{
    const float s = std::clamp(scatter, 0.0F, 1.0F);
    if (level < 0 || level >= count)
        return 0.0F;
    const float spread = std::pow(s, static_cast<float>(level));
    return level == count - 1 ? spread : spread * (1.0F - s);
}

// Final mix: color = lerp(scene, bloom, mix). The setting scales a restrained
// base so 100% reads as a lens, not a filter.
[[nodiscard]] inline float bloom_mix(float strength) noexcept
{
    return std::clamp(0.045F * std::clamp(strength, 0.0F, 2.0F), 0.0F, 0.2F);
}

// Karis average: weighting a tap by 1 / (1 + luma) stops one very bright texel
// (the sun, a spark) from flickering through the whole chain.
[[nodiscard]] inline float karis_weight(float luminance) noexcept
{
    return 1.0F / (1.0F + std::max(luminance, 0.0F));
}

// --- Exposure -----------------------------------------------------------------
// Middle-grey key over the measured log-average luminance. The Enhanced
// presentation may close down in bright snow and desert but never opens up a
// dark scene, so night and caves keep their mood.
[[nodiscard]] inline float auto_exposure(float average_luminance) noexcept
{
    return std::clamp(0.20F / std::max(average_luminance, 0.0008F), 0.354F, 1.0F);
}

[[nodiscard]] inline float exposure_from_stops(float stops) noexcept
{
    return std::pow(2.0F, std::clamp(stops, -2.0F, 2.0F));
}

// --- Lens ---------------------------------------------------------------------
// Brightness kept at `radius` (0 centre .. 1 corner) by a vignette of
// `strength`. Flat through the middle of the picture, where the crosshair is.
[[nodiscard]] inline float vignette(float radius, float strength) noexcept
{
    const float r = std::clamp(radius, 0.0F, 1.0F);
    const float edge = std::clamp((r - 0.35F) / 0.65F, 0.0F, 1.0F);
    const float falloff = edge * edge * (3.0F - 2.0F * edge);
    return 1.0F - std::clamp(strength, 0.0F, 1.0F) * 0.55F * falloff;
}

// --- Volumetric light -----------------------------------------------------------
// Henyey-Greenstein phase function, normalised over the sphere.
[[nodiscard]] inline float henyey_greenstein(float cosine, float g) noexcept
{
    const float denominator = 1.0F + g * g - 2.0F * g * std::clamp(cosine, -1.0F, 1.0F);
    return (1.0F - g * g) / (4.0F * pi * std::pow(std::max(denominator, 1.0e-4F), 1.5F));
}

// Beer-Lambert transmittance through `distance` of fog of the given extinction.
[[nodiscard]] inline float transmittance(float extinction, float distance) noexcept
{
    return std::exp(-std::max(extinction, 0.0F) * std::max(distance, 0.0F));
}

// --- Motion blur ----------------------------------------------------------------
// How much of one frame's motion the blur covers. A film camera exposes for a
// fixed time, so at a high frame rate each frame's own motion is stretched to
// that exposure and at a low one it is shortened; the streak is the same length
// on screen at any frame rate.
[[nodiscard]] inline float motion_blur_scale(float strength, float frame_seconds) noexcept
{
    constexpr float full_shutter_seconds = 1.0F / 60.0F;
    const float frame = std::clamp(frame_seconds, 1.0F / 1000.0F, 1.0F / 15.0F);
    return std::clamp(strength, 0.0F, 1.0F) * full_shutter_seconds / frame;
}

// --- Moon -----------------------------------------------------------------------
// The Enhanced sky's moon stands opposite the sun, tipped a little so it is
// already up when the sun goes down.
[[nodiscard]] inline std::array<float, 3> moon_direction(float sun_x, float sun_y,
                                                         float sun_z) noexcept
{
    std::array<float, 3> moon{-sun_x * 0.92F + 0.18F, -sun_y + 0.10F, -sun_z * 0.92F - 0.22F};
    const float length = std::sqrt(moon[0] * moon[0] + moon[1] * moon[1] + moon[2] * moon[2]);
    if (length > 1.0e-5F)
        for (float &component : moon)
            component /= length;
    else
        moon = {0.0F, 1.0F, 0.0F};
    return moon;
}

// How much of its light the moon gives, 0..1: none until the sun is properly
// down, none while the moon itself is at the horizon. The light's direction
// changes from the sun's to the moon's only while this is zero, so nothing
// ever pops.
[[nodiscard]] inline float moonlight(float sun_y, float moon_y) noexcept
{
    const auto ease = [](float value) {
        const float t = std::clamp(value, 0.0F, 1.0F);
        return t * t * (3.0F - 2.0F * t);
    };
    return ease((-sun_y - 0.02F) / 0.16F) * ease((moon_y - 0.03F) / 0.15F);
}

// --- Ambient occlusion ------------------------------------------------------------
// The baked corner shading and the screen-space term describe the same
// creases. Multiplying them darkens every crease twice; the combined
// visibility is the darker of the two. `baked` has already been applied to the
// lit pixel, so this is the extra factor the post pass may still apply.
[[nodiscard]] inline float residual_occlusion(float baked, float screen_space) noexcept
{
    const float b = std::clamp(baked, 0.05F, 1.0F);
    return std::clamp(std::min(b, screen_space) / b, 0.0F, 1.0F);
}

} // namespace openminerz::post
