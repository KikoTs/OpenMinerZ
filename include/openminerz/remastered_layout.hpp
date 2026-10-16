#pragma once

#include "openminerz/settings.hpp"

#include <array>
#include <cstddef>
#include <optional>

// Where everything in the Remastered interface goes.
//
// Remastered is OpenMinerZ's own look for the HUD and the inventory, crate and
// crafting screens (UiStyle::remastered). It is laid out with ONE rule, the
// rule the two retail styles are built on:
//
//   * Lengths are authored in UNITS on a canvas that is 1280 x 720 units on a
//     16:9 window at 100% interface scale. That is the reference both retail
//     releases measure in: 1.6.3.8 draws into a 1280x720 buffer, 1.9.9.8
//     multiplies by height / 720.
//   * One unit is min(width / 1280, height / 720) x interface scale pixels,
//     the same min() that letterboxes the Classic buffer. So an element covers
//     the same fraction of the picture at 720p, 1080p and 2160p, and on a
//     window narrower than 16:9 (16:10, 4:3) the WIDTH decides, so nothing is
//     pushed off the sides.
//   * The canvas is the whole window measured in those units: wider than 1280
//     on 21:9, taller than 720 on 4:3. Elements anchor to its edges and its
//     centre, inside the screen-margin setting, the way the Modern HUD anchors
//     to the real window. Nothing is letterboxed and nothing is stretched.
//   * The scale is never so large that the content stops fitting: the HUD
//     always has hud_min_width x hud_min_height units to work with, and a
//     screen panel is shrunk until all of it is inside the margins.
//
// Everything here is plain arithmetic with no drawing and no game state, so
// the layout can be checked at any window size (tests/test_interface_styles).
namespace openminerz::remastered
{

inline constexpr float reference_width = 1280.0F;
inline constexpr float reference_height = 720.0F;
// The least room the HUD is ever given, in units. 150% interface scale on a
// 16:9 window is 853 x 480.
inline constexpr float hud_min_width = 800.0F;
inline constexpr float hud_min_height = 450.0F;

struct Rect final
{
    float x{};
    float y{};
    float width{};
    float height{};
    [[nodiscard]] constexpr float right() const noexcept { return x + width; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr float center_x() const noexcept { return x + width * 0.5F; }
    [[nodiscard]] constexpr float center_y() const noexcept { return y + height * 0.5F; }
    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0.0F || height <= 0.0F; }
    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
    [[nodiscard]] constexpr bool inside(const Rect &outer) const noexcept
    {
        return x >= outer.x && y >= outer.y && right() <= outer.right() && bottom() <= outer.bottom();
    }
    [[nodiscard]] constexpr bool overlaps(const Rect &other) const noexcept
    {
        return !empty() && !other.empty() && x < other.right() && other.x < right() &&
               y < other.bottom() && other.y < bottom();
    }
};

struct Canvas final
{
    // Window pixels per unit.
    float scale{1.0F};
    // The window, in units.
    float width{reference_width};
    float height{reference_height};
    // The part of it inside the screen margins, in units.
    Rect safe{0.0F, 0.0F, reference_width, reference_height};
};

// The canvas for a window. `min_width` x `min_height` is the room the content
// needs: the scale is lowered (never raised) until that much fits inside the
// screen margins.
[[nodiscard]] Canvas canvas(int window_width, int window_height, float ui_scale, float safe_area,
                            float min_width = hud_min_width,
                            float min_height = hud_min_height) noexcept;

// ---- The HUD ---------------------------------------------------------------

struct HudOptions final
{
    // From the world: a second item tray (1.9.9.8 rules), a stamina meter.
    bool two_trays{};
    bool stamina{};
    // From the settings page.
    VitalsStyle vitals{VitalsStyle::remastered};
    bool tray{true};
    bool item_label{true};
    bool compass{true};
    bool clock{};
    bool distance{true};
    bool oxygen{};       // shown now (the player is under water)
    bool server{};
    bool coordinates{};
    bool frame_counter{}; // the FPS read-out owns the top right corner
};

inline constexpr std::size_t tray_slots = 8U;

struct HudLayout final
{
    // The item tray: the plate and its eight slots.
    Rect tray{};
    std::array<Rect, tray_slots> slots{};
    // A small read-only strip of the tray that is not in hand (two-tray
    // worlds). Empty when there is no second tray or no room beside the tray.
    Rect other_tray{};
    std::array<Rect, tray_slots> other_slots{};
    // Health and stamina. `health` / `stamina` are the whole meter (icon,
    // number and bar); empty when hidden or the world has no stamina.
    Rect health{};
    Rect stamina{};
    // The selected item's line over the tray: name at its left end, ammunition
    // at its right.
    Rect item_line{};
    // The name of the block under the crosshair.
    Rect target_line{};
    // Top centre, top to bottom: the heading strip, the distance read-out, the
    // oxygen meter.
    Rect compass{};
    Rect distance{};
    Rect oxygen{};
    // Top left: server and coordinates cards.
    Rect server{};
    Rect coordinates{};
    // Top right: the clock, then notices under it.
    Rect clock{};
    Rect notice{};
    // The chat feed: lines stack upward from its bottom edge.
    Rect chat{};
    // The player list, centred.
    Rect roster{};
    // The centre of the crosshair.
    float center_x{};
    float center_y{};
};

[[nodiscard]] HudLayout hud_layout(const Canvas &canvas, const HudOptions &options) noexcept;

// ---- The inventory, crate and crafting panel ---------------------------------
//
// One panel, two columns of the same width. The right column is always the
// backpack over the item tray(s). The left column is the crafting list and its
// detail card on the inventory screen, the crate on the crate screen. All in
// units relative to the panel's top-left corner; screens::Session places the
// panel and does the hit testing with these same rectangles.

inline constexpr int slot_size = 56;
inline constexpr int slot_gap = 4;
inline constexpr int slot_pitch = slot_size + slot_gap;
inline constexpr int grid_columns = 8;
inline constexpr int grid_rows = 4;
inline constexpr int grid_width = grid_columns * slot_size + (grid_columns - 1) * slot_gap;  // 476
inline constexpr int grid_height = grid_rows * slot_size + (grid_rows - 1) * slot_gap;      // 236
inline constexpr int panel_padding = 24;
inline constexpr int column_gap = 28;
inline constexpr int panel_width = panel_padding * 2 + grid_width * 2 + column_gap;          // 1028
inline constexpr int header_height = 52;
inline constexpr int label_height = 24;
inline constexpr int footer_height = 34;
inline constexpr int left_x = panel_padding;
inline constexpr int right_x = panel_padding + grid_width + column_gap;
inline constexpr int content_y = header_height + 10;           // section labels
inline constexpr int grid_y = content_y + label_height;        // backpack and crate grids
inline constexpr int tray_label_y = grid_y + grid_height + 10; // "ITEM TRAY"
inline constexpr int tray_y = tray_label_y + label_height;
// Crafting: category tabs, the recipe list and the detail card beside it.
inline constexpr int tab_count = 5;
inline constexpr int tab_height = 30;
inline constexpr int tab_gap = 4;
inline constexpr int recipe_row_height = 34;
inline constexpr int recipe_list_width = 232;
inline constexpr int recipe_list_y = grid_y + tab_height + 8;
inline constexpr int scroll_width = 6;
inline constexpr int detail_x = left_x + recipe_list_width + 12;
inline constexpr int detail_width = grid_width - recipe_list_width - 12;
inline constexpr int craft_button_height = 34;

// The bottom of the tray rows: one row, or two in a two-tray world.
[[nodiscard]] constexpr int trays_bottom(int trays) noexcept
{
    return tray_y + slot_size + (trays > 1 ? slot_gap + slot_size : 0);
}
[[nodiscard]] constexpr int panel_height(int trays) noexcept
{
    return trays_bottom(trays) + 12 + footer_height + 12;
}
// How many recipe rows fit in the list.
[[nodiscard]] constexpr int recipe_rows(int trays) noexcept
{
    return (trays_bottom(trays) - recipe_list_y) / recipe_row_height;
}

} // namespace openminerz::remastered
