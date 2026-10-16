#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Field of view: the one place the two first-person projections are defined.
//
// A frame is drawn with TWO cameras, as the original draws it (GameScreen adds
// a CameraView on Player.FPSCamera for the world and a second CameraView on
// Player.GunEyePointCamera for the first-person body):
//
//   WORLD      Player.FPSCamera. Vertical field of view, Hor+: the angle is
//              measured top to bottom and the sides follow the display's
//              aspect ratio, so a wider display sees more (Matrix.
//              CreatePerspectiveFieldOfView(fov, viewport.AspectRatio, ...)).
//              Retail fixes it at 73 degrees; here it is the player's
//              "Field of view" setting (Settings::vertical_fov).
//
//   VIEWMODEL  Avatar.EyePointCamera / GunEyePointCamera: the whole body and
//              the held item seen from the head. Retail's 90 degrees (45
//              shouldered) at the default world FOV; both hands and weapon
//              scale together as the player widens or narrows that setting.
//
// Shouldering narrows the world camera. Retail lerps the ANGLE from DefaultFOV
// to DefaultFOV / ShoulderMagnification; what the player perceives is the
// resulting on-screen magnification, tan(73/2) / tan(zoomed/2). That ratio is
// what is kept at every setting (zoom_ratio), so a 2x gun magnifies the picture
// by retail's factor whether the player chose 60 or 105 degrees. At 73 degrees
// the result is retail's angle exactly.
//
// PRESENTATION ONLY. Nothing here may reach the simulation or the network:
// gameplay that reads the camera's angle (the guided launcher's lock cone) uses
// retail_lock_cone_radians(), which never sees the setting.
namespace openminerz::view
{

inline constexpr float pi = 3.14159265358979323846F;
inline constexpr float radians_per_degree = pi / 180.0F;

// PerspectiveCamera.FieldOfView / Player.DefaultFOV.
inline constexpr float retail_vertical_fov_degrees = 73.0F;
// Player.DefaultAvatarFOV, and ShoulderedAvatarFOV as its fixed share.
inline constexpr float retail_viewmodel_fov_degrees = 90.0F;
inline constexpr float retail_shouldered_viewmodel_ratio = 0.5F;
// Player.ControlSensitivity in every shouldering state (FPSRig.UpdateRotation
// multiplies the look input by it); 1 otherwise.
inline constexpr float retail_shouldered_look_scale = 0.25F;
// RocketLauncherGuidedInventoryItemClass: 0.3 * FPSCamera.FieldOfView.
inline constexpr float retail_lock_cone_share = 0.3F;

// The "Field of view" setting, in VERTICAL degrees. The default is the view
// OpenMinerZ has always opened with; the range is what the slider offers.
// Measured across a 16:9 display that is 60 to 133 degrees (see
// horizontal_fov_degrees); 105 was the previous maximum and is kept so no
// saved setting changes.
inline constexpr float default_vertical_fov_degrees = 74.0F;
inline constexpr float minimum_vertical_fov_degrees = 36.0F;
inline constexpr float maximum_vertical_fov_degrees = 105.0F;
inline constexpr float reference_aspect = 16.0F / 9.0F;

// The advanced, file-only Settings::viewmodel_fov.
inline constexpr float minimum_viewmodel_fov_degrees = 55.0F;
inline constexpr float maximum_viewmodel_fov_degrees = 110.0F;

// What a projection may be asked to draw once a zoom has narrowed the setting.
// Renderers clamp to this and to nothing tighter, so every pass of a frame
// agrees on the angle.
inline constexpr float minimum_presented_fov_degrees = 1.0F;
inline constexpr float maximum_presented_fov_degrees = 170.0F;

[[nodiscard]] inline float sanitized_fov_degrees(float degrees) noexcept
{
    if (!std::isfinite(degrees))
        return default_vertical_fov_degrees;
    return std::clamp(degrees, minimum_presented_fov_degrees, maximum_presented_fov_degrees);
}

// tan(fov / 2): half the picture's height at unit distance.
[[nodiscard]] inline float half_tangent(float fov_degrees) noexcept
{
    return std::tan(sanitized_fov_degrees(fov_degrees) * radians_per_degree * 0.5F);
}

[[nodiscard]] inline float fov_from_half_tangent(float tangent) noexcept
{
    return 2.0F * std::atan(tangent) / radians_per_degree;
}

// Hor+ conversions. `aspect` is width / height.
[[nodiscard]] inline float horizontal_fov_degrees(float vertical_degrees, float aspect) noexcept
{
    return fov_from_half_tangent(half_tangent(vertical_degrees) * std::max(aspect, 0.0001F));
}

[[nodiscard]] inline float vertical_fov_degrees(float horizontal_degrees, float aspect) noexcept
{
    return fov_from_half_tangent(half_tangent(horizontal_degrees) / std::max(aspect, 0.0001F));
}

// --- Shouldering -----------------------------------------------------------

// FPSCamera.FieldOfView while a gun is brought up:
// Angle.Lerp(DefaultFOV, DefaultFOV / ShoulderMagnification, progress).
[[nodiscard]] inline float retail_zoom_fov_degrees(float magnification, float progress) noexcept
{
    const float full = retail_vertical_fov_degrees / std::max(magnification, 0.01F);
    return std::lerp(retail_vertical_fov_degrees, full, std::clamp(progress, 0.0F, 1.0F));
}

// How many times larger the world is drawn at that point of the zoom.
[[nodiscard]] inline float zoom_ratio(float magnification, float progress) noexcept
{
    return half_tangent(retail_vertical_fov_degrees) /
           half_tangent(retail_zoom_fov_degrees(magnification, progress));
}

// The world camera's vertical field of view for this frame: the player's
// setting narrowed by retail's zoom ratio. Settings::normalize() keeps the
// stored setting inside the slider's range; a diagnostic still (--fov) may ask
// for any angle a projection can draw.
[[nodiscard]] inline float presented_vertical_fov_degrees(float setting_degrees,
                                                          float magnification,
                                                          float progress) noexcept
{
    const float setting = sanitized_fov_degrees(setting_degrees);
    if (progress <= 0.0F || magnification == 1.0F)
        return setting;
    return sanitized_fov_degrees(
        fov_from_half_tangent(half_tangent(setting) / zoom_ratio(magnification, progress)));
}

// Avatar.EyePointCamera.FieldOfView:
// Angle.Lerp(DefaultAvatarFOV, ShoulderedAvatarFOV, progress).
[[nodiscard]] inline float viewmodel_fov_degrees(float base_degrees, float progress) noexcept
{
    const float base = std::clamp(std::isfinite(base_degrees) ? base_degrees
                                                              : retail_viewmodel_fov_degrees,
                                  minimum_viewmodel_fov_degrees, maximum_viewmodel_fov_degrees);
    return std::lerp(base, base * retail_shouldered_viewmodel_ratio,
                     std::clamp(progress, 0.0F, 1.0F));
}

// Preserve the default framing and ADS magnification while scaling the entire
// first-person rig by the world setting's perspective ratio. Use the unzoomed
// setting here: feeding the zoomed world angle would apply ADS a second time.
[[nodiscard]] inline float presented_viewmodel_fov_degrees(float base_degrees,
                                                          float world_setting_degrees,
                                                          float progress) noexcept
{
    const float scale = half_tangent(world_setting_degrees) /
                        half_tangent(default_vertical_fov_degrees);
    return sanitized_fov_degrees(fov_from_half_tangent(
        half_tangent(viewmodel_fov_degrees(base_degrees, progress)) * scale));
}

// Look input is an ANGLE per mouse count (or per stick second) and does not
// depend on the field of view. Retail slows it to a quarter while a gun is
// shouldered, for every gun; because the zoom ratio above is retail's at every
// setting, that one factor feels the same at every setting too.
[[nodiscard]] constexpr float look_scale(bool shouldering) noexcept
{
    return shouldering ? retail_shouldered_look_scale : 1.0F;
}

// --- Gameplay (never reads the setting) -------------------------------------

// The guided launcher locks inside 0.3 of the FPS camera's field of view. That
// is a rule about where the player is facing, so it is evaluated on retail's
// camera whatever the player's own field of view is.
[[nodiscard]] inline float retail_lock_cone_radians(float magnification, float progress) noexcept
{
    return retail_zoom_fov_degrees(magnification, progress) * radians_per_degree *
           retail_lock_cone_share;
}

// --- HUD --------------------------------------------------------------------

// InGameHUD opens the reticle by (spread / FPSCamera.FieldOfView) of the screen
// width. That brackets a fixed cone of the world at retail's 73 degrees; to
// bracket the SAME cone at another setting the opening follows the picture's
// scale, which is the ratio of half-tangents. At `reference_degrees` (the
// default setting) the value is retail's, pixel for pixel.
[[nodiscard]] inline float reticle_fraction(float spread_radians, float setting_degrees,
                                            float reference_degrees = default_vertical_fov_degrees)
    noexcept
{
    const float retail = spread_radians / (retail_vertical_fov_degrees * radians_per_degree);
    return retail * half_tangent(reference_degrees) / half_tangent(setting_degrees);
}

// --- Viewmodel <-> world ------------------------------------------------------

// A point in a camera's own space: x right, y up, z the distance ahead.
struct ViewPoint final
{
    float x{};
    float y{};
    float z{};
};

// A point of the viewmodel pass together with the angle that pass was drawn
// with: everything needed to find its pixel again.
struct ViewmodelPoint final
{
    ViewPoint point;
    float fov_degrees{retail_viewmodel_fov_degrees};
};

// The two cameras share an eye and a direction but not a field of view, so the
// same view-space point lands on different pixels in each. Given a point of the
// VIEWMODEL (the weapon's muzzle), this is the point at the same distance in
// the WORLD camera that covers the same pixel: where a bolt has to be drawn to
// leave the muzzle the player can see. (Retail's Player.GetGunTipPosition
// reuses the coordinates unchanged, so its bolts start 1.35x further from the
// screen centre than the gun at the default 73/90 pair.)
[[nodiscard]] inline ViewPoint viewmodel_to_world_view(ViewPoint viewmodel_point,
                                                       float viewmodel_fov_degrees_now,
                                                       float world_fov_degrees_now) noexcept
{
    const float scale =
        half_tangent(world_fov_degrees_now) / half_tangent(viewmodel_fov_degrees_now);
    return {viewmodel_point.x * scale, viewmodel_point.y * scale, viewmodel_point.z};
}

// Where a view-space point lands, in normalised device coordinates (-1..1,
// y up). Used by the tests to state the reconciliation as "same pixel".
[[nodiscard]] inline std::array<float, 2> view_to_ndc(ViewPoint point, float fov_degrees,
                                                      float aspect) noexcept
{
    const float tangent = half_tangent(fov_degrees);
    const float depth = std::max(point.z, 0.0001F);
    return {point.x / (depth * tangent * std::max(aspect, 0.0001F)), point.y / (depth * tangent)};
}

// A projectile is simulated from one place and has to be SEEN leaving another
// (the visible muzzle). `offset` is (seen start - simulated start); it is worn
// off over the first `blend_distance` of travel so the drawn projectile joins
// its true path, and therefore still arrives where the shot really goes.
[[nodiscard]] inline float muzzle_blend(float travelled, float blend_distance) noexcept
{
    if (!(blend_distance > 0.0F))
        return 0.0F;
    const float t = std::clamp(travelled / blend_distance, 0.0F, 1.0F);
    return 1.0F - t * t * (3.0F - 2.0F * t);
}

// --- The setting as the player reads it --------------------------------------

// The slider moves in whole vertical degrees.
[[nodiscard]] inline float setting_normalized(float setting_degrees) noexcept
{
    return (std::clamp(setting_degrees, minimum_vertical_fov_degrees,
                       maximum_vertical_fov_degrees) -
            minimum_vertical_fov_degrees) /
           (maximum_vertical_fov_degrees - minimum_vertical_fov_degrees);
}

// What the same setting spans side to side on a 16:9 display, for the read-out.
[[nodiscard]] inline int reference_horizontal_degrees(float setting_degrees) noexcept
{
    return static_cast<int>(std::lround(horizontal_fov_degrees(setting_degrees, reference_aspect)));
}

struct VolumeBounds final
{
    float left{-1.0F}, right{1.0F}, bottom{-1.0F}, top{1.0F};
    bool visible{true};
};

// Conservative projected sphere bounds. A volume crossing the eye/near plane
// must cover the viewport: moving a billboard in front of its centre makes it
// vanish as the camera enters the smoke. The pixel shader resolves the sphere
// and opaque depth within these bounds, including when its centre is behind us.
[[nodiscard]] inline VolumeBounds volume_bounds(float x, float y, float z, float radius,
                                                float vertical_degrees, float aspect) noexcept
{
    if (z + radius <= 0.0F || radius <= 0.0F)
        return {0, 0, 0, 0, false};
    if (z - radius <= 0.12F)
        return {};
    const float ty = half_tangent(vertical_degrees);
    const float tx = ty * std::max(aspect, 0.01F);
    VolumeBounds bounds{1, -1, 1, -1, true};
    for (float depth : {z - radius, z + radius})
        for (float side : {-1.0F, 1.0F})
        {
            bounds.left = std::min(bounds.left, (x + side*radius) / (depth*tx));
            bounds.right = std::max(bounds.right, (x + side*radius) / (depth*tx));
            bounds.bottom = std::min(bounds.bottom, (y + side*radius) / (depth*ty));
            bounds.top = std::max(bounds.top, (y + side*radius) / (depth*ty));
        }
    bounds.left = std::max(-1.0F, bounds.left);
    bounds.right = std::min(1.0F, bounds.right);
    bounds.bottom = std::max(-1.0F, bounds.bottom);
    bounds.top = std::min(1.0F, bounds.top);
    bounds.visible = bounds.left < bounds.right && bounds.bottom < bounds.top;
    return bounds;
}

} // namespace openminerz::view
