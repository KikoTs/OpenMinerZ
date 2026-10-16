#include "openminerz/hud_layout.hpp"

#include <algorithm>
#include <cmath>
#include <format>

// Every function below is one stretch of InGameHUD.OnDraw (or of a method it
// calls) from the shipped assemblies, kept in the original's order. The names
// in the comments are the original's. Two things are easy to "tidy" and must
// not be:
//
//   * Truncation. The original casts to int at specific points; trunc() is
//     that cast. Rectangle.Center is integer division. Moving a cast changes
//     a rectangle by a pixel at some resolution.
//   * Precision. The game is a 32-bit process: its float arithmetic runs on the
//     x87 unit in double precision and is only narrowed where a value is
//     stored into a float field (a Vector2, ScaleFactor). So products are
//     formed in double here and narrowed with f32() exactly where the original
//     stores them.
//
// tests/test_hud_layout.cpp holds this file to lists recorded from the
// original at five window sizes.
namespace openminerz::hud
{
namespace
{

[[nodiscard]] constexpr int trunc(double value) noexcept
{
    return static_cast<int>(value);
}

[[nodiscard]] constexpr float f32(double value) noexcept
{
    return static_cast<float>(value);
}

constexpr Color white{255U, 255U, 255U, 255U};
constexpr Color black{0U, 0U, 0U, 255U};
constexpr Color transparent{0U, 0U, 0U, 0U};
constexpr Color red{255U, 0U, 0U, 255U};
constexpr Color lime{0U, 255U, 0U, 255U};
constexpr Color orange{255U, 165U, 0U, 255U};
// InventoryItem.Draw2D's condition bar.
constexpr Color condition_green{67U, 188U, 0U, 255U};
// CMZColors.MenuAqua, and the three XNA named colours 1.9.9.8's meters use.
constexpr Color menu_aqua{53U, 170U, 253U, 255U};
constexpr Color dodger_blue{30U, 144U, 255U, 255U};
constexpr Color yellow{255U, 255U, 0U, 255U};
constexpr Color orange_red{255U, 69U, 0U, 255U};

struct SpriteInfo final
{
    std::string_view name;
    Size classic;
    Size modern;
};

// SpriteSheet.xnb source rectangles of both builds (sizes only).
constexpr std::array<SpriteInfo, static_cast<std::size_t>(Sprite::count)> sprite_table{{
    {"", {1, 1}, {1, 1}},                          // solid
    {"", {64, 64}, {64, 64}},                      // item_icon
    {"HudGrid", {485, 72}, {481, 68}},
    {"Selector", {68, 68}, {76, 76}},
    {"CrossHair", {29, 28}, {29, 28}},
    {"CrossHairTick", {0, 0}, {8, 3}},
    {"HealthBarEmpty", {246, 31}, {184, 50}},
    {"HealthBarFull", {246, 31}, {122, 50}},
    {"StaminaBarEmpty", {0, 0}, {184, 50}},
    {"BubbleBar", {240, 24}, {240, 24}},
    {"DamageArrow", {64, 64}, {64, 64}},
    {"SniperScope", {540, 540}, {540, 540}},
    {"MissleLock", {128, 128}, {128, 128}},
    {"MissleLocking", {128, 128}, {128, 128}},
}};

constexpr std::array<std::string_view, static_cast<std::size_t>(Font::count)> font_names{
    "SmallFont", "MedFont", "MedLargeFont", "LargeFont", "ConsoleFont"};

// Collects draws in submission order.
class Batch final
{
  public:
    Batch(UiStyle style, Frame &frame) : style_(style), frame_(frame) {}

    [[nodiscard]] Size size(Sprite sprite) const noexcept { return sprite_size(style_, sprite); }
    [[nodiscard]] bool modern() const noexcept { return style_ == UiStyle::modern; }

    // Sprite.Draw(batch, Rectangle destination, Color)
    Draw &sprite(Sprite sprite, IntRect destination, Color color)
    {
        const Size full = size(sprite);
        return add(sprite, {0, 0, full.width, full.height},
                   {static_cast<float>(destination.x), static_cast<float>(destination.y),
                    static_cast<float>(destination.width), static_cast<float>(destination.height)},
                   color);
    }

    // Sprite.Draw(batch, Rectangle destination, Rectangle source, Color)
    Draw &sprite(Sprite sprite, IntRect destination, IntRect source, Color color)
    {
        return add(sprite, source,
                   {static_cast<float>(destination.x), static_cast<float>(destination.y),
                    static_cast<float>(destination.width), static_cast<float>(destination.height)},
                   color);
    }

    // Sprite.Draw(batch, Vector2 position, Rectangle source, Color, rotation,
    //             origin, scale, ...): the destination is source * scale.
    Draw &sprite_scaled(Sprite sprite, float x, float y, IntRect source, Color color, float scale,
                        float rotation = 0.0F, float origin_x = 0.0F, float origin_y = 0.0F)
    {
        Draw &draw = add(sprite, source,
                         {x, y, f32(static_cast<double>(source.width) * scale),
                          f32(static_cast<double>(source.height) * scale)},
                         color);
        draw.rotation = rotation;
        draw.origin_x = origin_x;
        draw.origin_y = origin_y;
        return draw;
    }

    // SpriteBatch.Draw(DummyTexture, Rectangle, Color)
    Draw &fill(IntRect destination, Color color, bool fills_canvas = false)
    {
        Draw &draw = sprite(Sprite::solid, destination, color);
        draw.fills_canvas = fills_canvas;
        return draw;
    }

    // SpriteBatch.DrawString(font, text, position, color, 0, Zero, scale, None, _)
    void text(Font font, std::string_view value, float x, float y, Color color, float scale = 1.0F)
    {
        Draw draw;
        draw.kind = Draw::Kind::text;
        draw.font = font;
        draw.text = std::string{value};
        draw.x = x;
        draw.y = y;
        draw.scale = scale;
        draw.color = color;
        frame_.draws.push_back(std::move(draw));
    }

    // DrawingTools.DrawOutlinedText: four diagonal copies in the outline
    // colour, offset by outlineWidth * scale, then the text itself.
    void outlined(Font font, std::string_view value, float x, float y, Color color, Color outline,
                  int width, float scale = 1.0F)
    {
        const float offset = f32(static_cast<double>(width) * scale);
        text(font, value, x + offset, y + offset, outline, scale);
        text(font, value, x - offset, y - offset, outline, scale);
        text(font, value, x - offset, y + offset, outline, scale);
        text(font, value, x + offset, y - offset, outline, scale);
        text(font, value, x, y, color, scale);
    }

  private:
    Draw &add(Sprite sprite, IntRect source, Rect destination, Color color)
    {
        Draw draw;
        draw.kind = Draw::Kind::sprite;
        draw.sprite = sprite;
        draw.source = source;
        draw.destination = destination;
        draw.color = color;
        frame_.draws.push_back(std::move(draw));
        return frame_.draws.back();
    }

    UiStyle style_;
    Frame &frame_;
};

// InventoryItem.Draw2D(spriteBatch, dest): condition bar, icon, stack count.
// 1.9.9.8 scales the icon and the count's font with the screen but left the
// bar's offsets and the count's position in unscaled pixels; that is kept.
void draw_item(Batch &batch, const TextMetrics &metrics, const TraySlot &item, IntRect dest,
               int tray, int slot, Color tint, bool condition_bar, float text_scale)
{
    if (condition_bar && item.wears)
    {
        const int bottom = dest.y + dest.height;
        batch.fill({dest.x + 9, bottom - 16, dest.width - 18, 7}, black);
        batch.fill({dest.x + 10, bottom - 15,
                    trunc(static_cast<double>(dest.width - 20) * item.condition), 5},
                   condition_green);
    }
    Draw &icon = batch.sprite(Sprite::item_icon, dest, tint);
    // InventoryItemClass.Draw2D: the atlas is 8 cells wide; 1.9.9.8 switches
    // to its double-size atlas once the cell is drawn more than 10% enlarged.
    const bool large = batch.modern() && static_cast<float>(dest.width) / 64.0F > 1.1F;
    const int cell = large ? 128 : 64;
    icon.source = {(item.icon & 7) * cell, (item.icon / 8) * cell, cell, cell};
    icon.tray = tray;
    icon.slot = slot;
    if (item.count > 1)
    {
        const int line = metrics.line_spacing(Font::small);
        batch.outlined(Font::small, std::to_string(item.count), static_cast<float>(dest.x + 8),
                       static_cast<float>(dest.y + dest.height - line), white, black, 1, text_scale);
    }
}

// ConsoleElement.OnDraw. The element sits at the top-left of the canvas and is
// (30% x 25%) of the BACK BUFFER, whatever the canvas is: in 1.6.3.8 that makes
// the chat column taller in canvas units the larger the window.
void draw_console(Batch &batch, const State &state, const TextMetrics &metrics, int window_height)
{
    if (!state.show_console)
        return;
    const int line = metrics.line_spacing(Font::console);
    const float height = f32(static_cast<double>(window_height) * 0.25F);
    float y = height - static_cast<float>(line);
    for (std::size_t index = state.console.size(); index-- > 0U;)
    {
        const ConsoleLine &message = state.console[index];
        if (message.visibility > 0.0F)
            batch.outlined(Font::console, message.text, 0.0F, y,
                           color_lerp(transparent, white, message.visibility),
                           color_lerp(transparent, black, message.visibility), 1);
        y -= static_cast<float>(line);
        if (y < 0.0F)
            break;
    }
}

// InGameHUD.DrawPlayerList, identical in both builds: 1.9.9.8 did not scale it.
void draw_player_list(Batch &batch, const State &state, const TextMetrics &metrics, int width,
                      int height)
{
    const std::string count = std::format("{} {}/{}", state.players_word, state.roster.size(),
                                          state.max_players);
    const Extent size = metrics.measure(Font::medium, count);
    batch.outlined(Font::medium, count, static_cast<float>(width) - size.width,
                   static_cast<float>(height) - size.height, white, black, 2);
    const float column = metrics.measure(Font::medium, "XXXXXXXXXXXXXXXXXXX ").width;
    const float x = (static_cast<float>(width) - column) / 2.0F;
    float y = 0.0F;
    batch.outlined(Font::medium, state.player_word, x, y, orange, black, 2);
    const float line = static_cast<float>(metrics.line_spacing(Font::medium));
    y += line;
    for (const RosterLine &player : state.roster)
    {
        batch.outlined(Font::medium, player.gamertag, x, y, player.local ? red : white, black, 2);
        y += line;
    }
}

// The red washes and the direction arrows, shared by both builds.
void draw_damage(Batch &batch, const State &state, int width, int height)
{
    if (!state.show_damage)
        return;
    const int alpha = trunc((1.0 - static_cast<double>(state.health)) * 120.0F);
    const int redness = trunc((1.0 - static_cast<double>(state.health)) * 102.0F);
    if (alpha > 0)
        batch.fill({0, 0, width, height},
                   {static_cast<std::uint8_t>(redness), 0U, 0U, static_cast<std::uint8_t>(alpha)},
                   true);
    if (state.lava > 0.0F)
        batch.fill({0, 0, width, height}, color_lerp(transparent, red, state.lava), true);
    if (state.lava >= 1.0F)
        batch.fill({0, 0, width, height}, red, true);
    const Size arrow = batch.size(Sprite::damage_arrow);
    for (const DamageArrow &indicator : state.damage)
    {
        const Color color{static_cast<std::uint8_t>(trunc(139.0 * indicator.fade)), 0U, 0U,
                          static_cast<std::uint8_t>(trunc(255.0 * indicator.fade))};
        batch.sprite_scaled(Sprite::damage_arrow, static_cast<float>(width / 2),
                            static_cast<float>(height / 2), {0, 0, arrow.width, arrow.height}, color,
                            0.75F, indicator.rotation, static_cast<float>(arrow.width / 2),
                            static_cast<float>(arrow.height + 150));
    }
}

// fadeInGameStart: one second from black as the world appears.
void draw_fade_in(Batch &batch, const State &state, int width, int height)
{
    if (state.fade_in_seconds >= 1.0F)
        return;
    // new Color(t, t, t, 1 - t) with t = fadeInGameStart's elapsed seconds, in
    // both builds. Blended premultiplied that is t + scene * t: the picture
    // rises out of black and washes towards white before the fade ends.
    const float elapsed = std::clamp(state.fade_in_seconds, 0.0F, 1.0F);
    batch.fill({0, 0, width, height},
               color_from_floats(elapsed, elapsed, elapsed, 1.0F - elapsed), true);
}

// drawDayTimer: nine seconds, the first three fading in and the last three out.
[[nodiscard]] bool day_banner_alpha(const State &state, float &alpha) noexcept
{
    if (state.day_banner_seconds >= 9.0F || state.dead)
        return false;
    const float complete = state.day_banner_seconds / 9.0F;
    constexpr float third = 1.0F / 3.0F;
    alpha = 1.0F;
    if (state.day_banner_seconds < 3.0F)
        alpha = complete / third;
    else if (state.day_banner_seconds > 6.0F)
        alpha = 1.0F - (complete - third * 2.0F) / third;
    return true;
}

void layout_classic(Batch &batch, const State &state, const TextMetrics &metrics,
                    int window_height)
{
    // Rectangle titleSafeArea = device.Viewport.TitleSafeArea: on a PC, the
    // whole 1280x720 offscreen buffer.
    constexpr int width = 1280;
    constexpr int height = 720;
    constexpr int centre_x = width / 2;
    constexpr int centre_y = height / 2;

    if (state.show_scope && !state.dead && state.scope)
    {
        if (state.lock_box)
            batch.sprite(state.locked ? Sprite::missile_lock : Sprite::missile_locking,
                         state.lock_rect, state.locked ? red : lime);
        const Size scope = batch.size(Sprite::sniper_scope);
        const int x = centre_x - scope.width / 2;
        const int y = centre_y - scope.height / 2;
        batch.sprite(Sprite::sniper_scope, {x, y, scope.width, scope.height}, white);
        batch.fill({0, 0, width, y}, black, true);
        batch.fill({0, y + scope.height, width, y}, black, true);
        batch.fill({0, y, x, scope.height}, black, true);
        batch.fill({width - x, y, x, scope.height}, black, true);
    }
    draw_console(batch, state, metrics, window_height);
    if (state.show_players)
    {
        draw_player_list(batch, state, metrics, width, height);
        return;
    }
    draw_damage(batch, state, width, height);

    if (state.show_distance)
    {
        const std::string label = state.distance_word + " - " + state.max_word;
        const Extent label_size = metrics.measure(Font::medium, label);
        batch.outlined(Font::medium, label, static_cast<float>(width) - label_size.width - 22.0F,
                       0.0F, white, black, 1);
        const std::string numbers = std::format("{} - {}", state.distance, state.max_distance);
        batch.outlined(Font::medium, numbers,
                       static_cast<float>(width) - label_size.width / 2.0F -
                           metrics.measure(Font::medium, numbers).width / 2.0F,
                       label_size.height, white, black, 1);
    }
    if (state.show_block_name && state.aiming_at_block)
        batch.outlined(Font::medium, state.block_name,
                       static_cast<float>(centre_x) -
                           metrics.measure(Font::medium, state.block_name).width / 2.0F,
                       0.0F, white, black, 1);

    const Size grid = batch.size(Sprite::hud_grid);
    const IntRect tray{centre_x - grid.width / 2, height - grid.height, grid.width, grid.height};
    if (state.show_tray)
        batch.sprite(Sprite::hud_grid, tray, white);
    // Vector2 position3 = (tray.Left, tray.Top - 30): the heart strip.
    const int hearts_y = tray.y - 30;
    if (state.show_vitals)
    {
        const Size hearts = batch.size(Sprite::health_empty);
        batch.sprite_scaled(Sprite::health_empty, static_cast<float>(tray.x),
                            static_cast<float>(hearts_y), {0, 0, hearts.width, hearts.height}, white,
                            1.0F);
        const Size full = batch.size(Sprite::health_full);
        batch.sprite_scaled(Sprite::health_full, static_cast<float>(tray.x),
                            static_cast<float>(hearts_y),
                            {0, 0, trunc(static_cast<double>(full.width) * state.health), full.height},
                            white, 1.0F);
    }
    if (state.show_oxygen && state.underwater)
    {
        const Size bubbles = batch.size(Sprite::bubble_bar);
        const int x = tray.x + tray.width / 2;
        const int shown = trunc(static_cast<double>(bubbles.width) * state.oxygen);
        const int hidden = trunc(static_cast<double>(bubbles.width) * (1.0F - state.oxygen));
        batch.sprite(Sprite::bubble_bar, {trunc(static_cast<double>(x) + static_cast<double>(bubbles.width) * (1.0F - state.oxygen)), hearts_y, shown, bubbles.height},
                     {hidden, 0, shown, bubbles.height}, white);
    }
    if (state.show_tray)
    {
        for (int index = 0; index < 8; ++index)
            if (state.tray[static_cast<std::size_t>(index)].occupied)
                draw_item(batch, metrics, state.tray[static_cast<std::size_t>(index)],
                          {59 * index + tray.x + 2, tray.y + 2, 64, 64}, 0, index, white, true, 1.0F);
        const Size selector = batch.size(Sprite::selector);
        batch.sprite(Sprite::selector,
                     {tray.x + 59 * state.selected, tray.y, selector.width, selector.height}, white);
    }
    if (state.show_item_text)
        batch.outlined(Font::medium, state.item_text, static_cast<float>(tray.x),
                       static_cast<float>(hearts_y) -
                           metrics.measure(Font::medium, state.item_name).height,
                       white, black, 2);
    if (state.show_crosshair && !state.dead && !state.shouldering)
    {
        const Size cross = batch.size(Sprite::crosshair);
        batch.sprite_scaled(Sprite::crosshair, static_cast<float>(centre_x - cross.width / 2),
                            static_cast<float>(centre_y - cross.height / 2),
                            {0, 0, cross.width, cross.height}, white, 1.0F);
    }
    draw_fade_in(batch, state, width, height);
    float alpha{};
    if (day_banner_alpha(state, alpha))
        batch.outlined(Font::large, state.day_text, 0.0F,
                       static_cast<float>(height) -
                           metrics.measure(Font::large, state.day_text).height,
                       color_from_floats(alpha, alpha, alpha, alpha),
                       color_from_floats(0.0F, 0.0F, 0.0F, alpha), 2);
}

void layout_modern(Batch &batch, const State &state, const TextMetrics &metrics, int width,
                   int height)
{
    // Screen.Adjuster: ScreenRect is the back buffer, ScaleFactor.Y its height
    // over the authored 720.
    const float scale = static_cast<float>(height) / 720.0F;
    const int centre_x = width / 2;
    const int centre_y = height / 2;
    const Color aqua = color_scale(menu_aqua, 0.75F);

    if (state.show_scope && !state.dead && state.scope)
    {
        if (state.lock_box)
            batch.sprite(state.locked ? Sprite::missile_lock : Sprite::missile_locking,
                         state.lock_rect, state.locked ? red : lime);
        const Size sprite = batch.size(Sprite::sniper_scope);
        const Size scope{trunc(static_cast<double>(sprite.width) * scale),
                         trunc(static_cast<double>(sprite.height) * scale)};
        const int x = centre_x - scope.width / 2;
        const int y = centre_y - scope.height / 2;
        batch.sprite(Sprite::sniper_scope, {x, y, scope.width, scope.height}, white);
        batch.fill({0, 0, width, y}, black, true);
        batch.fill({0, y + scope.height, width, y}, black, true);
        batch.fill({0, y, x, scope.height}, black, true);
        batch.fill({width - x, y, x, scope.height}, black, true);
    }
    draw_console(batch, state, metrics, height);
    if (state.show_players)
    {
        draw_player_list(batch, state, metrics, width, height);
        return;
    }
    draw_damage(batch, state, width, height);

    const float line = static_cast<float>(metrics.line_spacing(Font::medium));
    if (state.show_distance)
    {
        // DrawDistanceStr: right-aligned, ten authored pixels from the edge.
        batch.text(Font::medium, state.distance_word,
                   f32(static_cast<double>(width) -
                       (static_cast<double>(metrics.measure(Font::medium, state.distance_word).width) + 10.0) * scale),
                   0.0F, aqua, scale);
        const std::string numbers =
            std::format("{}-{} {}", state.distance, state.max_distance, state.max_word);
        batch.text(Font::medium, numbers,
                   f32(static_cast<double>(width) -
                       (static_cast<double>(metrics.measure(Font::medium, numbers).width) + 10.0) * scale),
                   f32(static_cast<double>(line) * scale), aqua, scale);
    }
    if (state.show_block_name && state.aiming_at_block)
        batch.text(Font::medium, state.block_name,
                   f32(static_cast<double>(width) -
                       (static_cast<double>(metrics.measure(Font::medium, state.block_name).width) + 10.0) * scale),
                   f32(static_cast<double>(line * 4.0F) * scale), aqua, scale);

    // The tray art twice: where the NEXT tray's items go (lower, dimmed), then
    // shifted up and left for the current tray.
    const Size grid_sprite = batch.size(Sprite::hud_grid);
    const Size grid{trunc(static_cast<double>(grid_sprite.width) * scale),
                    trunc(static_cast<double>(grid_sprite.height) * scale)};
    const IntRect next{centre_x - grid.width / 2, height - grid.height - trunc(5.0 * scale),
                       grid.width, grid.height};
    IntRect tray = next;
    tray.x += trunc(f32(-35.0 * scale));
    tray.y += trunc(f32(-68.0 * scale));
    const Color dim = color_from_floats(1.0F, 1.0F, 1.0F, state.next_tray_alpha);
    if (state.show_tray)
    {
        // A world without a second tray (1.6.3.8 rules) has nothing to put in
        // the lower strip, so it is left out rather than drawn empty.
        if (state.has_next_tray)
            batch.sprite(Sprite::hud_grid, next, dim);
        batch.sprite(Sprite::hud_grid, tray, white);
    }

    if (state.show_vitals)
    {
        // DrawPlayerStats -> DrawHealthBar / DrawStaminaBar -> DrawResourceBar.
        const auto meter = [&](Sprite icon, Color color, float ratio, IntRect bar) {
            batch.sprite(icon, bar, color);
            const Size full = batch.size(Sprite::health_full);
            const int x = bar.x + trunc(56.0 * scale);
            batch.sprite_scaled(Sprite::health_full, static_cast<float>(x),
                                static_cast<float>(bar.y),
                                {0, 0, trunc(static_cast<double>(full.width) * ratio), full.height},
                                color, scale);
            batch.text(Font::medium,
                       std::format("{}%", static_cast<long long>(std::trunc(static_cast<double>(ratio) * 100.0F))),
                       static_cast<float>(x), f32(static_cast<double>(bar.y) - 20.0 * scale), color,
                       scale);
        };
        const Size health = batch.size(Sprite::health_empty);
        const int top = trunc(20.0 * scale);
        const IntRect health_size{0, top, trunc(static_cast<double>(health.width) * scale),
                                  trunc(static_cast<double>(health.height) * scale)};
        if (!state.has_stamina)
        {
            meter(Sprite::health_empty, dodger_blue, state.health,
                  {trunc(static_cast<double>(centre_x) - static_cast<double>(health.width) * scale / 2.0),
                   top, health_size.width, health_size.height});
        }
        else
        {
            meter(Sprite::health_empty, dodger_blue, state.health,
                  {trunc(static_cast<double>(centre_x) -
                         static_cast<double>(health.width) * 2.2F * scale / 2.0),
                   top, health_size.width, health_size.height});
            const Size stamina = batch.size(Sprite::stamina_empty);
            meter(Sprite::stamina_empty, state.stamina_blocked ? orange_red : yellow, state.stamina,
                  {trunc(static_cast<double>(centre_x) +
                         static_cast<double>(stamina.width) * 0.2F * scale / 2.0),
                   top, trunc(static_cast<double>(stamina.width) * scale),
                   trunc(static_cast<double>(stamina.height) * scale)});
        }
    }
    if (state.show_oxygen && state.underwater)
    {
        // Positioned from the current tray, in unscaled pixels.
        const Size bubbles = batch.size(Sprite::bubble_bar);
        const int x = tray.x + tray.width / 2;
        const int y = tray.y - 30;
        const int shown = trunc(static_cast<double>(bubbles.width) * state.oxygen);
        const int hidden = trunc(static_cast<double>(bubbles.width) * (1.0F - state.oxygen));
        batch.sprite(Sprite::bubble_bar,
                     {trunc(static_cast<double>(x) + static_cast<double>(bubbles.width) * (1.0F - state.oxygen)), y, shown, bubbles.height},
                     {hidden, 0, shown, bubbles.height}, white);
    }

    const int cell = trunc(64.0 * scale);
    if (state.show_tray)
    {
        for (int index = 0; index < 8; ++index)
            if (state.tray[static_cast<std::size_t>(index)].occupied)
                draw_item(batch, metrics, state.tray[static_cast<std::size_t>(index)],
                          {trunc(59.0 * scale * index + static_cast<double>(tray.x) + 2.0 * scale),
                           tray.y + trunc(2.0 * scale), cell, cell},
                          0, index, white, true, scale);
        if (state.has_next_tray)
            for (int index = 0; index < 8; ++index)
                if (state.next_tray[static_cast<std::size_t>(index)].occupied)
                    draw_item(batch, metrics, state.next_tray[static_cast<std::size_t>(index)],
                              {trunc(59.0 * scale * index + static_cast<double>(next.x) + 2.0 * scale),
                               next.y + trunc(2.0 * scale), cell, cell},
                              1, index, dim, false, scale);
        batch.sprite(Sprite::selector,
                     {tray.x + trunc(7.0 * scale + 59.0 * scale * state.selected),
                      trunc(static_cast<double>(tray.y) + 7.0 * scale), cell, cell},
                     white);
    }
    if (state.show_item_text)
        batch.text(Font::medium, state.item_text, static_cast<float>(tray.x),
                   f32(static_cast<double>(tray.y) -
                       static_cast<double>(metrics.measure(Font::medium, state.item_name).height) * scale),
                   aqua, scale);

    if (state.show_crosshair && !state.dead && !state.shouldering)
    {
        // Four ticks that open with the weapon's current spread.
        const float open = f32(static_cast<double>(state.reticle_fraction) * width);
        const float tick_scale = std::max(1.0F, scale);
        const float shade = (1.0F - state.inaccuracy) / 2.0F + 0.5F;
        const Color color = color_from_floats(shade, shade, shade, shade);
        const Size tick = batch.size(Sprite::crosshair_tick);
        const IntRect source{0, 0, tick.width, tick.height};
        const Size scaled{trunc(static_cast<double>(tick.width) * tick_scale),
                          trunc(static_cast<double>(tick.height) * tick_scale)};
        const int row = trunc(static_cast<double>(centre_y) - 1.0 * tick_scale);
        batch.sprite(Sprite::crosshair_tick,
                     {trunc(static_cast<double>(centre_x) + open), row, scaled.width, scaled.height},
                     color);
        batch.sprite(Sprite::crosshair_tick,
                     {trunc(static_cast<double>(centre_x) - (9.0 * tick_scale + open)), row,
                      scaled.width, scaled.height},
                     color);
        constexpr float quarter_turn = 1.57079637F; // Angle.FromDegrees(90f).Radians
        const float column = static_cast<float>(trunc(static_cast<double>(centre_x) + 1.0 * tick_scale));
        batch.sprite_scaled(Sprite::crosshair_tick, column,
                            static_cast<float>(trunc(static_cast<double>(centre_y) - 8.0 * tick_scale - open)),
                            source, color, tick_scale, quarter_turn);
        batch.sprite_scaled(Sprite::crosshair_tick, column,
                            static_cast<float>(trunc(static_cast<double>(centre_y) + open + 1.0 * tick_scale)),
                            source, color, tick_scale, quarter_turn);
    }
    draw_fade_in(batch, state, width, height);
    float alpha{};
    if (day_banner_alpha(state, alpha))
        // Not scaled with the screen in the original.
        batch.text(Font::large, state.day_text, 0.0F,
                   static_cast<float>(height) - metrics.measure(Font::large, state.day_text).height,
                   color_scale(aqua, alpha));
}

[[nodiscard]] std::string number(float value)
{
    // Math.Round(value, 3).ToString("0.###", InvariantCulture)
    double rounded = std::nearbyint(static_cast<double>(value) * 1000.0) / 1000.0;
    if (rounded == 0.0)
        return "0";
    std::string text = std::format("{:.3f}", rounded);
    while (text.back() == '0')
        text.pop_back();
    if (text.back() == '.')
        text.pop_back();
    return text;
}

} // namespace

std::string_view sprite_sheet_name(Sprite sprite) noexcept
{
    const auto index = static_cast<std::size_t>(sprite);
    return index < sprite_table.size() ? sprite_table[index].name : std::string_view{};
}

Size sprite_size(UiStyle style, Sprite sprite) noexcept
{
    const auto index = static_cast<std::size_t>(sprite);
    if (index >= sprite_table.size())
        return {};
    return style == UiStyle::modern ? sprite_table[index].modern : sprite_table[index].classic;
}

std::string_view font_asset_name(Font font) noexcept
{
    const auto index = static_cast<std::size_t>(font);
    return index < font_names.size() ? font_names[index] : std::string_view{};
}

Color color_from_floats(float r, float g, float b, float a) noexcept
{
    // PackUtils.PackUNorm(255, value): clamp, scale, round half to even.
    const auto pack = [](float value) {
        const float scaled = std::clamp(value * 255.0F, 0.0F, 255.0F);
        return static_cast<std::uint8_t>(std::nearbyint(static_cast<double>(scaled)));
    };
    return {pack(r), pack(g), pack(b), pack(a)};
}

Color color_scale(Color color, float scale) noexcept
{
    // Color.operator*(Color, float): 16.16 fixed point, truncating.
    scale *= 65536.0F;
    const std::uint32_t factor =
        scale >= 0.0F ? (scale <= 16777215.0F ? static_cast<std::uint32_t>(scale) : 0xFFFFFFU) : 0U;
    const auto channel = [factor](std::uint8_t value) {
        return static_cast<std::uint8_t>(std::min<std::uint32_t>((value * factor) >> 16U, 255U));
    };
    return {channel(color.r), channel(color.g), channel(color.b), channel(color.a)};
}

Color color_lerp(Color from, Color to, float amount) noexcept
{
    // Color.Lerp: the amount becomes a 16.16 fraction, each channel moves by
    // (difference * fraction) >> 16 with an arithmetic shift.
    const int fraction = static_cast<int>(
        std::nearbyint(static_cast<double>(std::clamp(amount * 65536.0F, 0.0F, 65536.0F))));
    const auto channel = [fraction](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(a + (((static_cast<int>(b) - static_cast<int>(a)) * fraction) >> 16));
    };
    return {channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b),
            channel(from.a, to.a)};
}

Frame layout(UiStyle style, const State &state, int window_width, int window_height,
             const TextMetrics &metrics)
{
    Frame frame;
    window_width = std::max(window_width, 1);
    window_height = std::max(window_height, 1);
    if (style == UiStyle::modern)
    {
        frame.canvas_width = window_width;
        frame.canvas_height = window_height;
        frame.present = {0, 0, window_width, window_height};
        Batch batch{UiStyle::modern, frame};
        layout_modern(batch, state, metrics, window_width, window_height);
        return frame;
    }
    frame.canvas_width = 1280;
    frame.canvas_height = 720;
    // DNAGame.Draw: the offscreen buffer keeps its aspect and is centred.
    if (static_cast<float>(window_width) / static_cast<float>(window_height) > 1280.0F / 720.0F)
    {
        const int width = window_height * 1280 / 720;
        frame.present = {(window_width - width) / 2, 0, width, window_height};
    }
    else
    {
        const int height = window_width * 720 / 1280;
        frame.present = {0, (window_height - height) / 2, window_width, height};
    }
    Batch batch{UiStyle::classic, frame};
    layout_classic(batch, state, metrics, window_height);
    return frame;
}

float window_scale(const Frame &frame) noexcept
{
    return frame.canvas_width > 0
               ? static_cast<float>(frame.present.width) / static_cast<float>(frame.canvas_width)
               : 1.0F;
}

Rect to_window(const Frame &frame, Rect canvas) noexcept
{
    const float horizontal = window_scale(frame);
    const float vertical =
        frame.canvas_height > 0
            ? static_cast<float>(frame.present.height) / static_cast<float>(frame.canvas_height)
            : 1.0F;
    return {static_cast<float>(frame.present.x) + canvas.x * horizontal,
            static_cast<float>(frame.present.y) + canvas.y * vertical, canvas.width * horizontal,
            canvas.height * vertical};
}

std::string record(const Frame &frame)
{
    std::string text = std::format("C\t{}\t{}\t{}\t{}\t{}\t{}\n", frame.canvas_width,
                                   frame.canvas_height, frame.present.x, frame.present.y,
                                   frame.present.width, frame.present.height);
    for (const Draw &draw : frame.draws)
    {
        if (draw.kind == Draw::Kind::text)
        {
            std::string escaped;
            for (const char character : draw.text)
            {
                if (character == '\\' || character == '"')
                    escaped += '\\';
                if (character == '\n')
                    escaped += "\\n";
                else
                    escaped += character;
            }
            text += std::format("T\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t0\t0\t0\t\"{}\"\n",
                                font_asset_name(draw.font), number(draw.x), number(draw.y),
                                number(draw.scale), number(draw.scale), draw.color.r, draw.color.g,
                                draw.color.b, draw.color.a, escaped);
            continue;
        }
        std::string_view texture = "sheet";
        std::string_view name = sprite_sheet_name(draw.sprite);
        if (draw.sprite == Sprite::solid)
        {
            texture = "solid";
            name = "-";
        }
        else if (draw.sprite == Sprite::item_icon)
        {
            texture = draw.source.width > 64 ? "items2x" : "items";
            name = "-";
        }
        text += std::format("S\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t-\n",
                            texture, name, draw.source.x, draw.source.y, draw.source.width,
                            draw.source.height, number(draw.destination.x),
                            number(draw.destination.y), number(draw.destination.width),
                            number(draw.destination.height), draw.color.r, draw.color.g,
                            draw.color.b, draw.color.a, number(draw.rotation),
                            number(draw.origin_x), number(draw.origin_y));
    }
    return text;
}

} // namespace openminerz::hud
