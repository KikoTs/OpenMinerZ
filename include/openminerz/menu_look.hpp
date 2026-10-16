#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// The arithmetic behind the menus' look, kept free of drawing so it can be
// tested: how words are cased, which font sets a line, how the focused row
// pulses, and how the picture behind the menus is fitted to the window.
//
// The look itself is CastleMiner Z's front end as its shipped builds draw it
// (recorded by tools/OriginalClientInterop/MenusParityProbe); src/app_menus.inc
// applies it to OpenMinerZ's own screens.
namespace openminerz::ui
{

// Labels and values are Title Case in the originals ("View Distance",
// "Texture Quality", "Regular"). Every word is capitalised except the short
// joining words after the first; a word with a digit keeps its letters small
// ("8x", "1920 x 1080"); the few names that are spelt their own way keep it
// (FPS, HUD, HD, PVP, VSync).
[[nodiscard]] std::string menu_title_case(std::string_view value);
// The same for a value the settings model wrote in capitals. Anything already
// in mixed case - a device name, a server name - is returned as it is.
[[nodiscard]] std::string menu_value_case(std::string value);
// The shipped fonts hold Latin-1 and little else. A server message or a world
// name often carries typographic punctuation they lack (a bullet, a dash, a
// curly quote, an ellipsis); this writes those the plain way so the line can
// still be set in the game's own lettering. Everything else is left alone.
[[nodiscard]] std::string menu_plain_punctuation(std::string_view value);

// Which member of a font family sets lettering whose capitals are to be
// `wanted_cap` pixels tall, and at what scale.
//
// `family_caps` are the capital heights the family exists at, smallest first
// (1.6.3.8's display face: 11, 18, 36). The smallest member at least as large
// as the request is taken and reduced, which keeps bitmap glyphs sharp; only a
// request above the largest member is enlarged. A scale within six percent of
// one becomes exactly one, so the 1280x720 sizes are the shipped bitmaps
// untouched. A member with no capital height (a font that failed to load) is
// never chosen; with none usable, `member` is family_caps.size().
struct MenuLetterChoice final
{
    std::size_t member{};
    float scale{1.0F};
};
[[nodiscard]] MenuLetterChoice choose_menu_letter(std::span<const int> family_caps,
                                                  float wanted_cap) noexcept;

// MenuScreen.OnDraw's focus colour is Color.Lerp(row, selected, amount) with a
// quarter-second timer that runs one way and then the other: `amount` is one
// when the row takes the focus, falls to zero over 0.25 s and climbs back.
[[nodiscard]] float menu_focus_amount(float seconds_since_focus) noexcept;

// The part of a picture that covers a window without being stretched: the
// picture is scaled until both sides are filled and the overflow is cropped
// evenly from the two edges that have any. All four values are in picture
// pixels and the result always has the window's aspect ratio.
struct PictureCrop final
{
    float x{};
    float y{};
    float width{};
    float height{};
};
[[nodiscard]] PictureCrop cover_crop(float picture_width, float picture_height,
                                     float window_width, float window_height) noexcept;

} // namespace openminerz::ui
