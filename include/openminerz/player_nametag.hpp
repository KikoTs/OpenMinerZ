#pragma once

#include "openminerz/world/player.hpp"

#include <array>
#include <cmath>
#include <optional>

namespace openminerz
{

// GameScreen.gameScreen_AfterDraw, the original's only world nametag:
//
//   for every gamer in the session that has a Player and is not local:
//       if (player.Visible)
//           vector  = player.LocalPosition + (0, 2, 0)
//           vector2 = Vector4.Transform(vector, mainView.View * Projection)
//           if (vector2.Z > 0)
//               screen = viewport * (0.5 + 0.5 * x/w, 0.5 - 0.5 * y/w)
//               size   = _nameTagFont.MeasureString(gamer.Gamertag)
//               DrawOutlinedText(_nameTagFont, Gamertag, screen - size / 2,
//                                Color.White, Color.Black, 1)
//
// That is the whole feature. There is no distance scale, no fade, no maximum
// distance and no occlusion test: the tag is a fixed-size sprite drawn after
// the scene, so it shows through terrain at any range. Player.Visible is never
// cleared (only Avatar.Visible is), so a dead player keeps its tag, and nothing
// marks the host, a team or a talking player. The local player has none.

// The camera a frame was drawn with. `valid` is false when the frame did not
// use the first-person camera, so nothing may be anchored to it.
struct NametagCamera final
{
    world::Vec3 eye;
    float yaw{};
    float pitch{};
    float vertical_fov_degrees{73.0F};
    float width{};
    float height{};
    bool valid{};
};

struct NametagAnchor final
{
    float x{};
    float y{};
};

// Player.LocalPosition is the feet; the tag is anchored two blocks above them.
inline constexpr float nametag_height_above_feet = 2.0F;
// PerspectiveCamera.NearPlane. `vector2.Z > 0` is exactly "further than this".
inline constexpr float nametag_near_plane = 0.01F;
// NameTagFont as shipped. One bitmap font is loaded at every resolution and
// drawn unscaled, so a tag is the same number of pixels on any display.
// Measured from the owned installs by tools/OriginalClientInterop/
// PlayerParityProbe (XNA SpriteFont.MeasureString over the shipped glyph
// table): the cell MeasureString reports, the height of a capital letter and
// how far below the cell's top capitals start.
struct NametagFontMetrics final
{
    int cell_height{};
    int cap_height{};
    int cap_top{};
};
inline constexpr NametagFontMetrics classic_nametag_font{27, 14, 5};
inline constexpr NametagFontMetrics modern_nametag_font{27, 15, 4};

// The cell is centred on the anchor, which puts the baseline this far below it.
[[nodiscard]] constexpr float nametag_baseline_below_anchor(
    const NametagFontMetrics &font) noexcept
{
    return static_cast<float>(font.cap_top + font.cap_height) -
           static_cast<float>(font.cell_height) * 0.5F;
}
// DrawOutlinedText(..., outlineWidth: 1): four diagonal copies in the outline
// colour, then the text itself. Order matters: the last entry is the fill.
struct NametagPass final
{
    float dx{};
    float dy{};
    bool outline{};
};
inline constexpr std::array<NametagPass, 5> nametag_passes{{
    {1.0F, 1.0F, true},
    {-1.0F, -1.0F, true},
    {-1.0F, 1.0F, true},
    {1.0F, -1.0F, true},
    {0.0F, 0.0F, false},
}};

[[nodiscard]] inline std::optional<NametagAnchor> project_nametag(const NametagCamera &camera,
                                                                  world::Vec3 feet) noexcept
{
    if (!camera.valid || camera.width <= 0.0F || camera.height <= 0.0F)
        return std::nullopt;
    const float dx = feet.x - camera.eye.x;
    const float dy = feet.y + nametag_height_above_feet - camera.eye.y;
    const float dz = feet.z - camera.eye.z;
    const float sine_yaw = std::sin(camera.yaw);
    const float cosine_yaw = std::cos(camera.yaw);
    const float sine_pitch = std::sin(camera.pitch);
    const float cosine_pitch = std::cos(camera.pitch);
    const float right = dx * cosine_yaw + dz * sine_yaw;
    const float level_depth = dx * sine_yaw - dz * cosine_yaw;
    const float up = dy * cosine_pitch - level_depth * sine_pitch;
    const float depth = level_depth * cosine_pitch + dy * sine_pitch;
    if (!(depth > nametag_near_plane))
        return std::nullopt;
    constexpr float degrees_to_radians = 3.14159265358979323846F / 180.0F;
    const float focal_length =
        camera.height * 0.5F / std::tan(camera.vertical_fov_degrees * degrees_to_radians * 0.5F);
    return NametagAnchor{camera.width * 0.5F + right * focal_length / depth,
                         camera.height * 0.5F - up * focal_length / depth};
}

// `screen - size / 2`: the text block is centred on the anchor on both axes.
[[nodiscard]] constexpr NametagAnchor nametag_text_origin(NametagAnchor anchor, float text_width,
                                                          float text_height) noexcept
{
    return {anchor.x - text_width * 0.5F, anchor.y - text_height * 0.5F};
}

} // namespace openminerz
