// Camera effects for mods: shake, fade and field-of-view pulses, under the
// player's accessibility settings (openminerz/modding/graphics.hpp).

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openminerz::modding::graphics
{
namespace
{

float finite(float value, float fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

// Smooth, zero-mean noise in -1..1 from a few incommensurate sines.
float wobble(float t, float seed) noexcept
{
    return (std::sin(t * 1.00F + seed) * 0.50F + std::sin(t * 2.31F + seed * 1.7F) * 0.30F +
            std::sin(t * 4.17F + seed * 2.9F) * 0.20F);
}

} // namespace

void CameraEffects::shake(float strength, float seconds, float frequency)
{
    strength = std::clamp(finite(strength, 0.0F), 0.0F, 1.0F);
    seconds = std::clamp(finite(seconds, 0.0F), 0.0F, 10.0F);
    if (strength <= 0.0F || seconds <= 0.0F)
        return;
    if (shakes_.size() >= max_shakes)
        shakes_.erase(shakes_.begin());
    seed_ = seed_ * 1664525U + 1013904223U;
    shakes_.push_back({strength, seconds, std::clamp(finite(frequency, 14.0F), 1.0F, 40.0F), 0.0F,
                       static_cast<float>(seed_ >> 8U) / static_cast<float>(1U << 24U) * 100.0F});
}

void CameraEffects::fade(Color color, float alpha, float seconds)
{
    // Start from wherever the overlay is now, so a fade can be reversed midway.
    const float now = fade_duration_ > 0.0F
                          ? fade_from_ + (fade_to_ - fade_from_) *
                                             std::clamp(fade_age_ / fade_duration_, 0.0F, 1.0F)
                          : fade_to_;
    fade_color_ = {std::clamp(finite(color.r, 0.0F), 0.0F, 1.0F),
                   std::clamp(finite(color.g, 0.0F), 0.0F, 1.0F),
                   std::clamp(finite(color.b, 0.0F), 0.0F, 1.0F)};
    fade_from_ = now;
    fade_to_ = std::clamp(finite(alpha, 0.0F), 0.0F, 1.0F);
    fade_duration_ = std::clamp(finite(seconds, 0.0F), 0.0F, 30.0F);
    fade_age_ = 0.0F;
}

void CameraEffects::fov_pulse(float degrees, float seconds)
{
    degrees = std::clamp(finite(degrees, 0.0F), -30.0F, 30.0F);
    seconds = std::clamp(finite(seconds, 0.0F), 0.05F, 10.0F);
    if (degrees == 0.0F)
        return;
    if (pulses_.size() >= max_shakes)
        pulses_.erase(pulses_.begin());
    pulses_.push_back({degrees, seconds, 0.0F});
}

// (GunFX)
void CameraEffects::kick(float pitch_degrees, float yaw_degrees, float seconds)
{
    pitch_degrees = std::clamp(finite(pitch_degrees, 0.0F), -10.0F, 10.0F);
    yaw_degrees = std::clamp(finite(yaw_degrees, 0.0F), -10.0F, 10.0F);
    seconds = std::clamp(finite(seconds, 0.0F), 0.02F, 2.0F);
    if (pitch_degrees == 0.0F && yaw_degrees == 0.0F)
        return;
    if (kicks_.size() >= max_shakes)
        kicks_.erase(kicks_.begin());
    kicks_.push_back({pitch_degrees, yaw_degrees, seconds, 0.0F});
}

void CameraEffects::clear() noexcept
{
    shakes_.clear();
    pulses_.clear();
    kicks_.clear(); // (GunFX)
    fade_from_ = 0.0F;
    fade_to_ = 0.0F;
    fade_duration_ = 0.0F;
    fade_age_ = 0.0F;
}

void CameraEffects::step(float seconds)
{
    seconds = std::clamp(finite(seconds, 0.0F), 0.0F, 1.0F);
    clock_ += seconds;
    if (clock_ > 10000.0F)
        clock_ = std::fmod(clock_, 10000.0F);
    for (Shake &shake : shakes_)
        shake.age += seconds;
    std::erase_if(shakes_, [](const Shake &shake) { return shake.age >= shake.duration; });
    for (Pulse &pulse : pulses_)
        pulse.age += seconds;
    std::erase_if(pulses_, [](const Pulse &pulse) { return pulse.age >= pulse.duration; });
    for (Kick &kick : kicks_) // (GunFX)
        kick.age += seconds;
    std::erase_if(kicks_, [](const Kick &kick) { return kick.age >= kick.duration; });
    fade_age_ = std::min(fade_age_ + seconds, std::max(fade_duration_, 0.0F));
}

bool CameraEffects::active() const noexcept
{
    return !shakes_.empty() || !pulses_.empty() || !kicks_.empty() || fade_to_ > 0.0F ||
           (fade_duration_ > 0.0F && fade_age_ < fade_duration_ && fade_from_ > 0.0F);
}

CameraOutput CameraEffects::output(const CameraAccessibility &options) const noexcept
{
    CameraOutput result;
    const float shake_scale =
        options.reduced_motion ? 0.0F : std::clamp(finite(options.shake_scale, 1.0F), 0.0F, 1.0F);
    if (shake_scale > 0.0F)
    {
        constexpr float degrees = std::numbers::pi_v<float> / 180.0F;
        for (const Shake &shake : shakes_)
        {
            // Trauma squared, easing out over the shake's life.
            const float life = 1.0F - std::clamp(shake.age / shake.duration, 0.0F, 1.0F);
            const float amount = shake.strength * shake.strength * life * life * shake_scale *
                                 max_shake_degrees * degrees;
            const float t = (clock_ + shake.seed) * shake.frequency;
            result.yaw += amount * wobble(t, shake.seed);
            result.pitch += amount * wobble(t, shake.seed + 31.0F);
            result.roll += amount * 0.5F * wobble(t, shake.seed + 57.0F);
        }
    }
    if (!options.reduced_motion)
        for (const Pulse &pulse : pulses_)
        {
            // Out and back: a half sine over the pulse.
            const float t = std::clamp(pulse.age / pulse.duration, 0.0F, 1.0F);
            result.fov += pulse.degrees * std::sin(t * std::numbers::pi_v<float>);
        }
    if (!options.reduced_motion)
        for (const Kick &kick : kicks_) // (GunFX)
        {
            // Up quickly over the first quarter, eased back over the rest.
            constexpr float half_pi = std::numbers::pi_v<float> * 0.5F;
            constexpr float degrees = std::numbers::pi_v<float> / 180.0F;
            const float t = std::clamp(kick.age / kick.duration, 0.0F, 1.0F);
            const float settle = std::cos((t - 0.25F) / 0.75F * half_pi);
            const float envelope = t < 0.25F ? std::sin(t / 0.25F * half_pi) : settle * settle;
            result.pitch += kick.pitch * degrees * envelope;
            result.yaw -= kick.yaw * degrees * envelope;
        }
    const float t =
        fade_duration_ > 0.0F ? std::clamp(fade_age_ / fade_duration_, 0.0F, 1.0F) : 1.0F;
    result.fade_alpha = std::clamp(fade_from_ + (fade_to_ - fade_from_) * t, 0.0F, 1.0F);
    result.fade_color = fade_color_;
    return result;
}

} // namespace openminerz::modding::graphics
