#include "openminerz/remastered_layout.hpp"

#include <algorithm>
#include <cmath>

namespace openminerz::remastered
{

Canvas canvas(int window_width, int window_height, float ui_scale, float safe_area, float min_width,
              float min_height) noexcept
{
    const float width = static_cast<float>(std::max(window_width, 1));
    const float height = static_cast<float>(std::max(window_height, 1));
    const float margin = std::isfinite(safe_area) ? std::clamp(safe_area, 0.80F, 1.00F) : 1.0F;
    const float wanted = std::isfinite(ui_scale) ? std::clamp(ui_scale, 0.75F, 1.50F) : 1.0F;
    // The unit: the same min() that letterboxes 1.6.3.8's 1280x720 buffer and,
    // on 16:9 and wider, exactly 1.9.9.8's height / 720.
    float scale = std::min(width / reference_width, height / reference_height) * wanted;
    // Never so large that the content no longer fits inside the margins.
    scale = std::min({scale, width * margin / std::max(min_width, 1.0F),
                      height * margin / std::max(min_height, 1.0F)});
    scale = std::max(scale, 0.05F);
    Canvas result;
    result.scale = scale;
    result.width = width / scale;
    result.height = height / scale;
    const float inset_x = result.width * (1.0F - margin) * 0.5F;
    const float inset_y = result.height * (1.0F - margin) * 0.5F;
    result.safe = {inset_x, inset_y, result.width - inset_x * 2.0F, result.height - inset_y * 2.0F};
    return result;
}

HudLayout hud_layout(const Canvas &canvas, const HudOptions &options) noexcept
{
    HudLayout layout;
    const Rect &safe = canvas.safe;
    const float center = canvas.width * 0.5F;
    layout.center_x = center;
    layout.center_y = canvas.height * 0.5F;
    // A constant breathing space between the margin and anything anchored to it.
    constexpr float edge = 10.0F;
    constexpr float gap = 8.0F;

    // ---- Bottom centre: the tray, the meters over it, the item line over those.
    constexpr float slot = 54.0F;
    constexpr float slot_gap_units = 4.0F;
    constexpr float plate_pad = 5.0F;
    const float plate_width = slot * 8.0F + slot_gap_units * 7.0F + plate_pad * 2.0F; // 470
    const float plate_height = slot + plate_pad * 2.0F;                              // 64
    const Rect tray{std::round(center - plate_width * 0.5F),
                    std::round(safe.bottom() - edge - plate_height), plate_width, plate_height};
    float stack_top = safe.bottom() - edge; // top of whatever is lowest so far
    if (options.tray)
    {
        layout.tray = tray;
        for (std::size_t index = 0; index < tray_slots; ++index)
            layout.slots[index] = {tray.x + plate_pad + static_cast<float>(index) * (slot + slot_gap_units),
                                   tray.y + plate_pad, slot, slot};
        stack_top = tray.y;
        // The tray that is not in hand, as a small strip to the LEFT of the
        // tray: the first-person weapon owns the lower right of the picture.
        if (options.two_trays)
        {
            constexpr float mini = 20.0F;
            constexpr float mini_gap = 2.0F;
            constexpr float mini_pad = 4.0F;
            const float width = mini * 8.0F + mini_gap * 7.0F + mini_pad * 2.0F; // 182
            const float height = mini + mini_pad * 2.0F;                        // 28
            const Rect strip{tray.x - gap - width, tray.bottom() - height, width, height};
            if (strip.x >= safe.x + edge)
            {
                layout.other_tray = strip;
                for (std::size_t index = 0; index < tray_slots; ++index)
                    layout.other_slots[index] = {strip.x + mini_pad +
                                                     static_cast<float>(index) * (mini + mini_gap),
                                                 strip.y + mini_pad, mini, mini};
            }
        }
    }
    // The meters sit on the tray's own width whether or not the tray is shown,
    // so hiding the tray does not move them sideways.
    constexpr float meter_height = 18.0F;
    const float half = (plate_width - gap) * 0.5F;
    const bool over_tray =
        options.vitals == VitalsStyle::remastered || options.vitals == VitalsStyle::classic;
    if (over_tray)
    {
        const float y = stack_top - 6.0F - meter_height;
        layout.health = {tray.x, y, half, meter_height};
        if (options.stamina)
            layout.stamina = {tray.x + half + gap, y, half, meter_height};
        stack_top = y;
    }
    if (options.item_label)
    {
        constexpr float line = 22.0F;
        layout.item_line = {tray.x, stack_top - 4.0F - line, plate_width, line};
        stack_top = layout.item_line.y;
    }

    // ---- Top centre: meters (the "top" look), heading, distance, oxygen.
    // `central` is the widest of them; the corner cards stay clear of it.
    float top = safe.y + edge;
    float central = 0.0F;
    if (options.vitals == VitalsStyle::modern)
    {
        const float width = 210.0F;
        central = options.stamina ? width * 2.0F + gap : width;
        if (options.stamina)
        {
            layout.health = {center - gap * 0.5F - width, top, width, meter_height};
            layout.stamina = {center + gap * 0.5F, top, width, meter_height};
        }
        else
        {
            layout.health = {center - width * 0.5F, top, width, meter_height};
        }
        top += meter_height + gap;
    }
    if (options.compass)
    {
        const float width = 340.0F;
        central = std::max(central, width);
        layout.compass = {std::round(center - width * 0.5F), top, width, 26.0F};
        top += 26.0F + 6.0F;
    }
    if (options.distance)
    {
        const float width = 240.0F;
        central = std::max(central, width);
        layout.distance = {center - width * 0.5F, top, width, 18.0F};
        top += 18.0F + 6.0F;
    }
    if (options.oxygen)
    {
        const float width = 200.0F;
        central = std::max(central, width);
        layout.oxygen = {center - width * 0.5F, top, width, 16.0F};
        top += 16.0F + 6.0F;
    }

    // ---- Under the crosshair: what is being aimed at.
    constexpr float target_half = 160.0F;
    layout.target_line = {center - target_half, layout.center_y + 30.0F, target_half * 2.0F, 20.0F};

    // ---- Top left: session cards.
    const float side_width =
        std::clamp((canvas.width - central) * 0.5F - safe.x - edge * 2.0F, 120.0F, 280.0F);
    float left_top = safe.y + edge;
    if (options.server)
    {
        layout.server = {safe.x + edge, left_top, side_width, 48.0F};
        left_top += 48.0F + 6.0F;
    }
    if (options.coordinates)
    {
        layout.coordinates = {safe.x + edge, left_top, side_width, 26.0F};
        left_top += 26.0F + 6.0F;
    }

    // ---- Top right: the frame counter keeps its corner; the clock goes under
    // it and notices under the clock.
    float right_top = safe.y + edge + (options.frame_counter ? 34.0F : 0.0F);
    if (options.clock)
    {
        const float width = std::min(170.0F, side_width);
        layout.clock = {safe.right() - edge - width, right_top, width, 52.0F};
        right_top += 52.0F + 6.0F;
    }
    {
        const float width = std::min(300.0F, side_width);
        layout.notice = {safe.right() - edge - width, right_top, width, 62.0F};
    }

    // ---- Left: the chat feed, growing upward from above the tray band.
    {
        // No wider than the gap to the line under the crosshair.
        const float width =
            std::min({420.0F, canvas.width * 0.36F, center - target_half - gap - (safe.x + edge)});
        const float bottom = std::min(stack_top, tray.y) - 14.0F;
        const float height = std::min(8.0F * 20.0F, bottom - left_top - gap);
        layout.chat = {safe.x + edge, bottom - std::max(height, 20.0F), width, std::max(height, 20.0F)};
    }

    // ---- Centre: the player list.
    {
        const float width = std::min(420.0F, safe.width - edge * 2.0F);
        const float height = std::min(360.0F, safe.height - edge * 2.0F - 120.0F);
        layout.roster = {center - width * 0.5F, std::max(safe.y + edge, layout.center_y - height * 0.5F - 30.0F),
                         width, height};
    }
    return layout;
}

} // namespace openminerz::remastered
