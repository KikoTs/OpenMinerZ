// Included inside Application::Impl (src/app.cpp), after app_screens.inc and
// app_menus.inc.
//
// ---- The Remastered interface -----------------------------------------------
// OpenMinerZ's own look for the HUD and for the inventory, crate and crafting
// screens (UiStyle::remastered, the default). It is the third interface style
// beside the two exact retail copies, and like them it is presentation only:
//
//   * WHERE things go is remastered_layout.hpp: one rule from 1280x720 to
//     3840x2160 and from 4:3 to 21:9, on the same 720-unit reference the
//     retail styles measure in. Nothing in this file positions anything with
//     a pixel constant; every length is in canvas units and goes through a
//     RetailCanvas, the helper the retail screens place their text with.
//   * WHAT a click does is screens::Session, the operation layer the retail
//     screens run on. This file draws the session's state and never touches a
//     stack itself (tests/test_interface_styles.cpp holds the styles equal).
//   * HOW it reads is the menus' vocabulary (app_menus.inc): CastleMiner Z's
//     own lettering in the world's generation, Title Case, the menus' inks, a
//     black wash for a plate, a white band for the focused row. The item
//     icons, names and descriptions are the game's own (the imported icon
//     atlas, item_text). So the HUD, the inventory and the front end are one
//     product, and all of it follows the world being played.

// ---- Canvas and primitives ---------------------------------------------------

// Capital heights, in canvas units. 11 and 18 are sizes the shipped fonts
// exist at, so at 720p that lettering is the original bitmaps untouched;
// running text is never set smaller (a reduced bitmap face spaces unevenly).
// `tiny` is for single marks only: a slot's number, a compass point.
static constexpr float rm_cap_tiny = 9.0F;
static constexpr float rm_cap_note = 11.0F;
static constexpr float rm_cap_small = 11.0F;
static constexpr float rm_cap_name = 13.0F;
static constexpr float rm_cap_title = 18.0F;

struct RmTheme final
{
    SDL_Color plate;     // behind HUD elements, over the world: a black wash
    SDL_Color panel;     // the inventory panel, over the dimmed world
    SDL_Color well;      // a recessed area inside the panel
    SDL_Color slot;      // an empty slot
    SDL_Color slot_full; // a slot holding something
    SDL_Color band;      // the menus' focus band
    SDL_Color line;      // a thin rule
    SDL_Color text;      // lettering
    SDL_Color row;       // a row or label that can be chosen
    SDL_Color quiet;     // secondary lines
    SDL_Color disabled;
    SDL_Color accent;    // marks and bars
    SDL_Color focus;     // what a chosen row's lettering becomes
    SDL_Color health;
    SDL_Color stamina;
    SDL_Color oxygen;
    SDL_Color good;
    SDL_Color caution;
};

[[nodiscard]] RmTheme rm_theme() const noexcept
{
    const MenuInk ink = menu_ink();
    const bool strong = settings_.high_contrast;
    RmTheme theme;
    theme.plate = {0U, 0U, 0U, static_cast<Uint8>(strong ? 226U : 150U)};
    theme.panel = {0U, 0U, 0U, static_cast<Uint8>(strong ? 244U : 208U)};
    theme.well = {255U, 255U, 255U, static_cast<Uint8>(strong ? 16U : 8U)};
    theme.slot = {255U, 255U, 255U, static_cast<Uint8>(strong ? 22U : 12U)};
    theme.slot_full = {255U, 255U, 255U, static_cast<Uint8>(strong ? 46U : 30U)};
    theme.band = {255U, 255U, 255U, static_cast<Uint8>(strong ? 72U : 44U)};
    theme.line = {255U, 255U, 255U, static_cast<Uint8>(strong ? 130U : 46U)};
    theme.text = ink.text;
    theme.row = ink.row;
    theme.quiet = ink.quiet;
    theme.disabled = ink.disabled;
    theme.accent = ink.accent;
    theme.focus = ink.focus;
    theme.health = {232U, 52U, 56U, 255U};
    theme.stamina = {240U, 196U, 60U, 255U};
    theme.oxygen = {72U, 188U, 226U, 255U};
    theme.good = {110U, 220U, 124U, 255U};
    theme.caution = {255U, 189U, 0U, 255U};
    mod_ui_rm_inks(theme); // UI area: a mod theme's HUD inks (app_mod_ui.inc)
    return theme;
}

// A canvas position in window pixels, on a whole pixel.
[[nodiscard]] static float rm_px(float value) noexcept
{
    return std::round(value);
}

// The window rectangle of a canvas rectangle, edges on whole pixels and never
// thinner than one pixel (a one-unit rule is under a pixel below 100%).
[[nodiscard]] static Rect rm_window(const RetailCanvas &canvas, const remastered::Rect &rect) noexcept
{
    const float left = rm_px(canvas.wx(rect.x));
    const float top = rm_px(canvas.wy(rect.y));
    const float right = std::max(left + 1.0F, rm_px(canvas.wx(rect.right())));
    const float bottom = std::max(top + 1.0F, rm_px(canvas.wy(rect.bottom())));
    return {left, top, right - left, bottom - top};
}

void rm_fill(const RetailCanvas &canvas, const remastered::Rect &rect, SDL_Color color)
{
    if (rect.empty() || color.a == 0U)
        return;
    fill(rm_window(canvas, rect), color);
}

// A frame drawn inside `rect`, `thickness` units wide.
void rm_frame(const RetailCanvas &canvas, const remastered::Rect &rect, SDL_Color color,
              float thickness = 1.0F)
{
    if (rect.empty() || color.a == 0U)
        return;
    const Rect box = rm_window(canvas, rect);
    const float line = std::max(1.0F, rm_px(thickness * canvas.scale));
    stroke(box, color, std::min({line, box.width * 0.5F, box.height * 0.5F}));
}

// A capital height in window pixels, for one given in canvas units.
//
// The sizes in this file are 1.6.3.8's (its fonts are 11 and 18 tall). In a
// 1.9.9.8 world the lettering is that build's condensed face, which needs more
// height for the same weight on screen and ships at 14 and 22: the same ratio,
// so the same request lands on a size that build has, too.
[[nodiscard]] float rm_cap(const RetailCanvas &canvas, float cap) const noexcept
{
    const float face = menu_generation() == RetailGeneration::modern ? 14.0F / 11.0F : 1.0F;
    return cap * face * canvas.scale * (settings_.larger_text ? 1.12F : 1.0F);
}

// Width of one line, in canvas units.
[[nodiscard]] float rm_text_width(const RetailCanvas &canvas, std::string_view value, float cap,
                                  MenuFace face = MenuFace::display)
{
    return value.empty() ? 0.0F : menu_width(value, rm_cap(canvas, cap), face) / canvas.scale;
}

// One line in a box: capitals centred on its height, aligned, shortened with
// an ellipsis to its width. Over the world it is outlined, as every piece of
// the game's own lettering is; on a plate it does not need to be.
void rm_label(const RetailCanvas &canvas, std::string_view value, const remastered::Rect &box,
              float cap, SDL_Color color, TextAlign align = TextAlign::left, bool outline = false,
              MenuFace face = MenuFace::display)
{
    if (value.empty() || box.empty() || color.a == 0U)
        return;
    menu_label_in(value,
                  {canvas.wx(box.x), canvas.wy(box.y), box.width * canvas.scale,
                   box.height * canvas.scale},
                  rm_cap(canvas, cap), color, align, face, outline);
}

// (Polish) A line that must stay whole, a number: shrunk until it fits its
// box, down to half its size, before the ellipsis would cut it ("100" became
// "1..." under a wide theme face). Unchanged when it fits.
void rm_label_fit(const RetailCanvas &canvas, std::string_view value, const remastered::Rect &box,
                  float cap, SDL_Color color, TextAlign align = TextAlign::left, bool outline = false,
                  MenuFace face = MenuFace::display)
{
    float fitted = cap;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const float width = rm_text_width(canvas, value, fitted, face);
        if (width <= box.width || fitted <= cap * 0.5F)
            break;
        // Sizes are not continuous (sprite faces, whole TrueType pixels):
        // aim at the box, but always step down.
        fitted = std::max(cap * 0.5F, std::min(fitted * 0.92F, fitted * box.width / width));
    }
    rm_label(canvas, value, box, fitted, color, align, outline, face);
}

// Word-wrapped lines no wider than `width` units, at most `limit` of them (the
// last is shortened with an ellipsis when there is more).
[[nodiscard]] std::vector<std::string> rm_wrap(const RetailCanvas &canvas, std::string_view value,
                                               float cap, float width, std::size_t limit,
                                               MenuFace face = MenuFace::text)
{
    if (limit == 0U || width <= 0.0F || value.empty())
        return {};
    const float pixels = rm_cap(canvas, cap);
    std::vector<std::string> lines = menu_wrapped(value, width * canvas.scale, pixels, face);
    if (lines.size() > limit)
    {
        lines.resize(limit);
        lines.back() = menu_fitted(lines.back() + " ...", width * canvas.scale, pixels, face);
    }
    return lines;
}

// A 7x7 pixel-art symbol, each pixel `unit` canvas units; columns at or after
// `filled` * 7 take the `empty` colour (a partly drained heart).
void rm_glyph(const RetailCanvas &canvas, const std::array<std::string_view, 7> &pattern, float x,
              float y, float unit, SDL_Color color, float filled = 1.0F, SDL_Color empty = {})
{
    const float edge = filled * 7.0F;
    for (std::size_t row = 0U; row < pattern.size(); ++row)
        for (std::size_t column = 0U; column < 7U; ++column)
        {
            if (pattern[row][column] != '1')
                continue;
            const bool full = static_cast<float>(column) + 0.5F < edge;
            rm_fill(canvas,
                    {x + static_cast<float>(column) * unit, y + static_cast<float>(row) * unit, unit,
                     unit},
                    full ? color : empty);
        }
}

static constexpr std::array<std::string_view, 7> rm_heart{"0110110", "1111111", "1111111", "1111111",
                                                         "0111110", "0011100", "0001000"};
static constexpr std::array<std::string_view, 7> rm_bolt{"0001110", "0011100", "0111000", "1111110",
                                                        "0001100", "0011000", "0110000"};

// The item's icon as the game renders it (the imported atlas), dimmed for
// something that cannot be used right now.
void rm_icon(const RetailCanvas &canvas, const InventoryStack &stack, const remastered::Rect &rect,
             float brightness = 1.0F, float opacity = 1.0F)
{
    const Rect box = rm_window(canvas, rect);
    // The atlas is premultiplied: the tint's colour carries the alpha too.
    const auto channel = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    const screens::Color tint{channel(brightness * opacity), channel(brightness * opacity),
                              channel(brightness * opacity), channel(opacity)};
    // Modding, visuals area: a mod item's own icon (app_mod_graphics_items.inc).
    if (stack.mod_item != 0U && draw_mod_item_icon(stack.mod_item, SDL_FRect{box.x, box.y, box.width, box.height},
                                                   tint, box.width / 64.0F > 1.1F))
        return;
    if (draw_retail_item_icon(stack.item, hud_style(), SDL_FRect{box.x, box.y, box.width, box.height},
                              tint, box.width / 64.0F > 1.1F))
        return;
    // No atlas has it (a pack imported before the icons were): the built-in
    // drawing, dimmed with a veil since it takes no tint.
    draw_item_icon(stack, box);
    const float hidden = 1.0F - brightness * opacity;
    if (hidden > 0.01F)
        fill(box, {0U, 0U, 0U, channel(hidden * 0.8F)});
}

// The wear bar of a tool, weapon or locator that has been used.
void rm_condition(const RetailCanvas &canvas, const InventoryStack &stack, const remastered::Rect &slot)
{
    if (item_self_damage_per_use(stack.item, inventory_.profile()) <= 0.0F || stack.durability >= 1.0F)
        return;
    const float health = std::clamp(stack.durability, 0.0F, 1.0F);
    const float inset = slot.width * 0.12F;
    const float height = std::max(3.0F, slot.height * 0.07F);
    const remastered::Rect track{slot.x + inset, slot.bottom() - height - slot.height * 0.07F,
                                 slot.width - inset * 2.0F, height};
    rm_fill(canvas, {track.x - 1.0F, track.y - 1.0F, track.width + 2.0F, track.height + 2.0F}, {0U, 0U, 0U, 220U});
    const auto blend = [&](float broken, float healthy) {
        return static_cast<Uint8>(std::lerp(broken, healthy, health));
    };
    rm_fill(canvas, {track.x, track.y, std::max(1.0F, track.width * health), track.height},
            {blend(232.0F, 96.0F), blend(64.0F, 204.0F), blend(48.0F, 84.0F), 255U});
}

// A stack's count, bottom right of its slot, in the game's own count lettering.
void rm_count(const RetailCanvas &canvas, const InventoryStack &stack, const remastered::Rect &slot,
              float cap, SDL_Color color)
{
    if (stack.count <= 1U)
        return;
    const float line = cap * 1.5F;
    rm_label_fit(canvas, std::to_string(stack.count),
             {slot.x + 2.0F, slot.bottom() - line - slot.height * 0.04F, slot.width - 5.0F, line}, cap,
             color, TextAlign::right, true);
}

// ---- HUD ---------------------------------------------------------------------

// Where the bottom-centre stack of the Remastered HUD begins (window pixels);
// captions are placed above it. Negative while another style is drawn.
float rm_caption_bottom_{-1.0F};
// The current notice reports a failure: shown whatever the interface style.
bool toast_urgent_{};

// A notice the player has to see even with an exact retail HUD on screen:
// something failed (a save, a session). Everything else is toast().
void alert(std::string title, std::string body)
{
    toast(std::move(title), std::move(body));
    toast_urgent_ = true;
}

// --hud-option notice=1 (an ordinary notice), notice=2 (a failure, which every
// style shows) and caption=1 (a sound caption): samples for a capture run,
// asserted every frame so nothing the world says in the meantime replaces them.
// A --screenshot run waits until they have been drawn (hud_samples_ready).
unsigned hud_sample_frames_{};
[[nodiscard]] bool hud_samples_ready() const
{
    for (const std::string &option : options_.hud_options)
        if (option.starts_with("notice=") || option.starts_with("caption="))
            return hud_sample_frames_ >= 3U;
    return true;
}
void show_hud_samples()
{
    ++hud_sample_frames_;
    for (const std::string &option : options_.hud_options)
    {
        if (option == "notice=1")
            toast("ITEM PICKED UP", "Stone PickAxe x1");
        else if (option == "notice=2")
            alert("SAVE FAILED", "The previous world save is intact. Check disk space and access.");
        else if (option == "caption=1")
        {
            subtitle_text_ = "[ ITEM CRAFTED ]";
            subtitle_time_ = 30.0F;
        }
        if (option.starts_with("notice="))
            toast_time_ = 3.0F; // past the fade-in by the time a capture is taken
    }
}

// A scoped weapon fully shouldered (the scope overlay replaces the view), and
// any weapon being shouldered (the crosshair leaves the screen).
[[nodiscard]] bool hud_scoped_aim() const
{
    const InventoryItemId held_item = inventory_.selected_item();
    const bool scoped_weapon = held_item == InventoryItemId::rocket_launcher ||
                               held_item == InventoryItemId::guided_rocket_launcher ||
                               held_item == InventoryItemId::iron_space_bolt_action_rifle ||
                               held_item == InventoryItemId::copper_space_bolt_action_rifle ||
                               held_item == InventoryItemId::gold_space_bolt_action_rifle ||
                               held_item == InventoryItemId::diamond_space_bolt_action_rifle;
    const bool preview_scoped_aim = options_.preview_viewmodel.has_value() &&
                                    (*options_.preview_viewmodel == "aim-idle" ||
                                     *options_.preview_viewmodel == "aim-fire" ||
                                     (*options_.preview_viewmodel == "aim" &&
                                      options_.preview_viewmodel_progress >= 0.999F));
    return scoped_weapon && (gun_aim_progress_ >= 0.999F || preview_scoped_aim);
}
[[nodiscard]] bool hud_shouldering() const
{
    const bool preview = options_.preview_viewmodel.has_value() &&
                         (*options_.preview_viewmodel == "aim" ||
                          *options_.preview_viewmodel == "aim-idle" ||
                          *options_.preview_viewmodel == "aim-fire");
    return shouldering_ || preview;
}

[[nodiscard]] remastered::Canvas rm_hud_canvas() const noexcept
{
    return remastered::canvas(metrics_.output_width, metrics_.output_height, settings_.ui_scale,
                              settings_.safe_area);
}

[[nodiscard]] remastered::HudOptions rm_hud_options() const
{
    const auto &player = player_.state();
    const bool modern_world = inventory_.profile() == InventoryProfile::modern;
    remastered::HudOptions options;
    options.two_trays = modern_world;
    // CastleMinerZGame.IsEnduranceMode: both Endurance modes have no stamina.
    options.stamina = modern_world && game_mode_ != GameMode::endurance &&
                      game_mode_ != GameMode::dragon_endurance;
    options.vitals = settings_.vitals_style;
    options.tray = settings_.hud_hotbar;
    options.item_label = settings_.hud_item_name;
    options.compass = settings_.hud_compass;
    options.clock = settings_.hud_clock;
    options.distance = settings_.hud_distance;
    options.oxygen = settings_.hud_oxygen && player_underwater(player);
    options.server = settings_.hud_server;
    options.coordinates = settings_.hud_coordinates;
    options.frame_counter = settings_.show_fps;
    return options;
}

// One meter: symbol, number, bar. `hearts` draws ten hearts in place of the
// number and bar.
void rm_meter(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &rect,
              float fraction, SDL_Color color, const std::array<std::string_view, 7> &symbol,
              bool hearts, bool over_world)
{
    if (rect.empty())
        return;
    fraction = std::clamp(fraction, 0.0F, 1.0F);
    constexpr float unit = 2.0F; // one pixel of the 7x7 symbols
    const float glyph = unit * 7.0F;
    const float top = rect.center_y() - glyph * 0.5F;
    // Low: the meter breathes, unless motion is reduced.
    const bool low = fraction > 0.0F && fraction < 0.25F;
    const float pulse = low && !settings_.reduced_motion ? 0.72F + 0.28F * std::sin(elapsed_ * 7.0F) : 1.0F;
    const SDL_Color lit{static_cast<Uint8>(std::lround(static_cast<float>(color.r) * pulse)),
                        static_cast<Uint8>(std::lround(static_cast<float>(color.g) * pulse)),
                        static_cast<Uint8>(std::lround(static_cast<float>(color.b) * pulse)), 255U};
    const SDL_Color shadow{0U, 0U, 0U, 190U};
    const SDL_Color drained{46U, 48U, 50U, 235U};
    if (hearts)
    {
        const float pitch = (rect.width - glyph) / 9.0F;
        for (int heart = 0; heart < 10; ++heart)
        {
            const float x = rect.x + static_cast<float>(heart) * pitch;
            rm_glyph(canvas, symbol, x + 1.0F, top + 1.0F, unit, shadow);
            rm_glyph(canvas, symbol, x, top, unit, lit,
                     std::clamp(fraction * 10.0F - static_cast<float>(heart), 0.0F, 1.0F), drained);
        }
        return;
    }
    rm_glyph(canvas, symbol, rect.x + 1.0F, top + 1.0F, unit, shadow);
    rm_glyph(canvas, symbol, rect.x, top, unit, lit);
    constexpr float number_width = 30.0F;
    rm_label_fit(canvas, std::to_string(static_cast<int>(std::lround(fraction * 100.0F))),
             {rect.x + glyph + 3.0F, rect.y, number_width, rect.height}, rm_cap_small, theme.text,
             TextAlign::right, over_world);
    const float bar_x = rect.x + glyph + 3.0F + number_width + 7.0F;
    const remastered::Rect track{bar_x, rect.center_y() - 5.0F, rect.right() - bar_x, 10.0F};
    rm_fill(canvas, track, {0U, 0U, 0U, 200U});
    const remastered::Rect inner{track.x + 1.0F, track.y + 1.0F, track.width - 2.0F, track.height - 2.0F};
    rm_fill(canvas, inner, drained);
    if (fraction > 0.0F)
    {
        const remastered::Rect level{inner.x, inner.y, std::max(1.0F, inner.width * fraction), inner.height};
        rm_fill(canvas, level, lit);
        rm_fill(canvas, {level.x, level.y, level.width, 2.0F}, {255U, 255U, 255U, 56U});
    }
}

void rm_draw_tray(const RetailCanvas &canvas, const RmTheme &theme, const remastered::HudLayout &hud)
{
    if (hud.tray.empty())
        return;
    rm_fill(canvas, hud.tray, theme.plate);
    const PlayerInventory::Tray &tray = inventory_.tray();
    const std::size_t selected = inventory_.selected_index();
    for (std::size_t index = 0U; index < remastered::tray_slots && index < tray.size(); ++index)
    {
        const remastered::Rect &slot = hud.slots[index];
        const bool chosen = index == selected;
        const bool filled = tray[index].has_value();
        rm_fill(canvas, slot, chosen ? theme.band : (filled ? theme.slot_full : theme.slot));
        if (filled)
        {
            const float inset = 4.0F;
            rm_icon(canvas, *tray[index],
                    {slot.x + inset, slot.y + inset, slot.width - inset * 2.0F, slot.height - inset * 2.0F});
            rm_condition(canvas, *tray[index], slot);
            rm_count(canvas, *tray[index], slot, rm_cap_small, theme.text);
        }
        // The slot in hand: a bright frame and the accent above it.
        if (chosen)
        {
            rm_frame(canvas, slot, theme.text, 2.0F);
            rm_fill(canvas, {slot.x, slot.y - 5.0F, slot.width, 3.0F}, theme.accent);
        }
        else
        {
            rm_frame(canvas, slot, theme.line);
        }
        if (settings_.hud_hints && !gamepad_input_active_)
            rm_label_fit(canvas, std::to_string(index + 1U), {slot.x + 4.0F, slot.y + 3.0F, 12.0F, 12.0F},
                     rm_cap_tiny, chosen ? theme.text : theme.quiet, TextAlign::left, true);
    }
    // The tray that is not in hand (two-tray worlds), read only.
    if (!hud.other_tray.empty())
    {
        const std::size_t other = 1U - inventory_.current_tray_index();
        const PlayerInventory::Tray &spare = inventory_.tray(other);
        rm_fill(canvas, hud.other_tray, theme.plate);
        for (std::size_t index = 0U; index < remastered::tray_slots && index < spare.size(); ++index)
        {
            const remastered::Rect &slot = hud.other_slots[index];
            rm_fill(canvas, slot, theme.slot);
            if (spare[index].has_value())
                rm_icon(canvas, *spare[index], {slot.x + 1.0F, slot.y + 1.0F, slot.width - 2.0F, slot.height - 2.0F},
                        0.9F, 0.85F);
        }
        const std::string label =
            settings_.hud_hints ? std::format("Tray {}  -  {}", other + 1U, gamepad_input_active_ ? "Switch" : "G")
                                : std::format("Tray {}", other + 1U);
        rm_label(canvas, label, {hud.other_tray.x, hud.other_tray.y - 17.0F, hud.other_tray.width, 15.0F},
                 rm_cap_note, theme.quiet, TextAlign::left, true, MenuFace::text);
    }
}

// The selected item's line: its name at the left end of the tray, a gun's
// clip and reserve at the right.
void rm_draw_item_line(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &line)
{
    if (line.empty())
        return;
    const InventoryItemId item = inventory_.selected_item();
    const GunStats *gun = selected_gun();
    const bool launcher = item == InventoryItemId::rocket_launcher ||
                          item == InventoryItemId::guided_rocket_launcher;
    if (gun == nullptr || launcher || !inventory_.selected().has_value())
    {
        // Everything else says what the original HUD says about it (a
        // locator's name and distance, "Bare Hands" for an empty hand).
        rm_label(canvas, hud_item_text(), line, rm_cap_name, theme.text, TextAlign::left, true);
        return;
    }
    const InventoryStack &stack = *inventory_.selected();
    const std::uint32_t reserve = inventory_.count(gun->ammo_item);
    // Right to left: reserve, then the clip, larger.
    const std::string reserve_text = std::format("/ {}", reserve);
    const float reserve_width = rm_text_width(canvas, reserve_text, rm_cap_small) + 2.0F;
    const std::string clip_text = gun_reloading_ ? std::string{"Reloading"} : std::to_string(stack.rounds_in_clip);
    const float clip_cap = gun_reloading_ ? rm_cap_small : rm_cap_title;
    const float clip_width = rm_text_width(canvas, clip_text, clip_cap) + 2.0F;
    const SDL_Color clip_color = gun_reloading_             ? theme.caution
                                 : stack.rounds_in_clip == 0U ? theme.health
                                                              : theme.text;
    rm_label(canvas, reserve_text, {line.right() - reserve_width, line.y + 3.0F, reserve_width, line.height - 3.0F},
             rm_cap_small, reserve == 0U ? theme.health : theme.quiet, TextAlign::right, true);
    rm_label(canvas, clip_text,
             {line.right() - reserve_width - 5.0F - clip_width, line.y - 4.0F, clip_width, line.height + 4.0F},
             clip_cap, clip_color, TextAlign::right, true);
    const float name_width = line.width - reserve_width - clip_width - 18.0F;
    rm_label(canvas, mod_item_label(stack, item_name(item)), {line.x, line.y, name_width, line.height},
             rm_cap_name, theme.text, TextAlign::left, true);
    if (settings_.hud_hints && !gun_reloading_ && stack.rounds_in_clip == 0U && reserve > 0U)
        rm_label(canvas, gamepad_input_active_ ? "X  Reload" : "R  Reload",
                 {line.x, line.y - 18.0F, line.width, 15.0F}, rm_cap_note, theme.caution, TextAlign::right, true,
                 MenuFace::text);
}

void rm_draw_compass(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &strip)
{
    if (strip.empty())
        return;
    rm_fill(canvas, strip, theme.plate);
    constexpr std::array<std::string_view, 8> points{"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    constexpr float pi = 3.14159265358979323846F;
    const float heading = std::fmod(player_.state().yaw * 180.0F / pi + 360.0F, 360.0F);
    // 45 degrees take a fifth of the strip, so five points are in view.
    const float spacing = strip.width / 5.0F;
    const float center = strip.center_x();
    const int nearest = static_cast<int>(std::floor(heading / 15.0F));
    for (int offset = -8; offset <= 9; ++offset)
    {
        const int step = nearest + offset;
        const float x = center + (static_cast<float>(step) * 15.0F - heading) / 45.0F * spacing;
        if (x < strip.x + 8.0F || x > strip.right() - 8.0F)
            continue;
        const float emphasis = 1.0F - std::clamp(std::abs(x - center) / (strip.width * 0.5F), 0.0F, 1.0F);
        if (step % 3 != 0)
        {
            // A tick every fifteen degrees between the named points.
            rm_fill(canvas, {x - 0.5F, strip.bottom() - 6.0F, 1.0F, 4.0F},
                    with_alpha(theme.quiet, 0.25F + emphasis * 0.45F));
            continue;
        }
        const int point = ((step / 3) % 8 + 8) % 8;
        const bool main = emphasis > 0.8F;
        rm_label(canvas, points[static_cast<std::size_t>(point)],
                 {x - 20.0F, strip.y + 1.0F, 40.0F, strip.height - 4.0F}, main ? rm_cap_small : rm_cap_tiny,
                 main ? theme.text : with_alpha(theme.quiet, 0.55F + emphasis * 0.45F), TextAlign::center);
    }
    // The heading mark.
    rm_fill(canvas, {center - 1.0F, strip.y, 2.0F, 5.0F}, theme.accent);
    rm_fill(canvas, {center - 1.0F, strip.bottom() - 4.0F, 2.0F, 4.0F}, theme.accent);
}

// A red vignette that deepens as health falls and flashes on a hit. It stays
// at the edges, so the middle of the view is never tinted.
void rm_draw_hurt(float strength)
{
    if (strength <= 0.004F)
        return;
    const float width = static_cast<float>(metrics_.output_width);
    const float height = static_cast<float>(metrics_.output_height);
    const float reach = std::min(width, height) * 0.30F;
    const SDL_FColor edge{0.62F, 0.0F, 0.02F, std::clamp(strength, 0.0F, 0.85F)};
    const SDL_FColor clear{0.62F, 0.0F, 0.02F, 0.0F};
    const std::array<SDL_FPoint, 4> outer{{{0.0F, 0.0F}, {width, 0.0F}, {width, height}, {0.0F, height}}};
    const std::array<SDL_FPoint, 4> inner{
        {{reach, reach}, {width - reach, reach}, {width - reach, height - reach}, {reach, height - reach}}};
    std::array<SDL_Vertex, 24> vertices{};
    std::size_t count = 0U;
    for (std::size_t side = 0U; side < 4U; ++side)
    {
        const std::size_t next = (side + 1U) % 4U;
        for (const auto &[point, tint] : {std::pair{outer[side], edge}, std::pair{outer[next], edge},
                                          std::pair{inner[next], clear}, std::pair{outer[side], edge},
                                          std::pair{inner[next], clear}, std::pair{inner[side], clear}})
            vertices[count++] = SDL_Vertex{point, tint, {}};
    }
    SDL_RenderGeometry(renderer_, nullptr, vertices.data(), static_cast<int>(count), nullptr, 0);
}

void rm_draw_damage_direction(const RetailCanvas &canvas, const remastered::HudLayout &hud)
{
    const auto &player = player_.state();
    for (const auto &indicator : damage_indicators_)
    {
        const float fade = indicator.age < 2.0F ? 1.0F : std::clamp(3.0F - indicator.age, 0.0F, 1.0F);
        if (fade <= 0.0F)
            continue;
        const float angle = std::atan2(indicator.source.x - player.position.x,
                                       -(indicator.source.z - player.position.z)) -
                            player.yaw;
        const float dx = std::sin(angle);
        const float dy = -std::cos(angle);
        // A wedge on a ring around the crosshair, pointing at the attacker.
        const auto corner = [&](float along, float across) {
            return SDL_FPoint{canvas.wx(hud.center_x + dx * along - dy * across),
                              canvas.wy(hud.center_y + dy * along + dx * across)};
        };
        const auto wedge = [&](float base, float tip, float half, SDL_FColor color) {
            const std::array<SDL_Vertex, 3> triangle{SDL_Vertex{corner(tip, 0.0F), color, {}},
                                                     SDL_Vertex{corner(base, half), color, {}},
                                                     SDL_Vertex{corner(base, -half), color, {}}};
            SDL_RenderGeometry(renderer_, nullptr, triangle.data(), 3, nullptr, 0);
        };
        wedge(88.0F, 122.0F, 26.0F, {0.0F, 0.0F, 0.0F, 0.55F * fade});
        wedge(91.0F, 117.0F, 21.0F, {0.86F, 0.10F, 0.12F, 0.92F * fade});
    }
}

void rm_draw_crosshair(const RetailCanvas &canvas, const remastered::HudLayout &hud)
{
    // The opening follows the weapon's spread, as the original's does
    // (Innaccuracy times the picture height), and the picture's scale at
    // another field of view, as the retail reticles here do; it never closes
    // completely.
    float gap = 7.0F;
    if (const GunStats *gun = selected_gun(); gun != nullptr)
        gap = std::max(gap, gun->inaccuracy * canvas.height *
                                view::half_tangent(view::default_vertical_fov_degrees) /
                                view::half_tangent(settings_.vertical_fov));
    constexpr float arm = 8.0F;
    constexpr float thick = 2.0F;
    const SDL_Color ink = has_melee_target() ? SDL_Color{255U, 60U, 56U, 255U} : SDL_Color{255U, 255U, 255U, 240U};
    const SDL_Color edge{0U, 0U, 0U, 150U};
    const float x = hud.center_x;
    const float y = hud.center_y;
    const std::array<remastered::Rect, 5> marks{{{x - gap - arm, y - thick * 0.5F, arm, thick},
                                                 {x + gap, y - thick * 0.5F, arm, thick},
                                                 {x - thick * 0.5F, y - gap - arm, thick, arm},
                                                 {x - thick * 0.5F, y + gap, thick, arm},
                                                 {x - 1.0F, y - 1.0F, 2.0F, 2.0F}}};
    // A dark rim keeps it readable on sand and snow.
    for (const remastered::Rect &mark : marks)
        rm_fill(canvas, {mark.x - 1.0F, mark.y - 1.0F, mark.width + 2.0F, mark.height + 2.0F}, edge);
    for (const remastered::Rect &mark : marks)
        rm_fill(canvas, mark, ink);
}

// The scope of a fully shouldered scoped weapon: the lens fills the picture's
// height at every resolution, black beyond it.
void rm_draw_scope()
{
    if (sniper_scope_texture_ == nullptr)
        return;
    float texture_width{};
    float texture_height{};
    if (!SDL_GetTextureSize(sniper_scope_texture_, &texture_width, &texture_height) || texture_height <= 0.0F)
        return;
    const float window_width = static_cast<float>(metrics_.output_width);
    const float window_height = static_cast<float>(metrics_.output_height);
    const float scale = window_height / texture_height;
    const SDL_FRect scope{rm_px((window_width - texture_width * scale) * 0.5F), 0.0F,
                          rm_px(texture_width * scale), window_height};
    SDL_SetTextureColorMod(sniper_scope_texture_, 255U, 255U, 255U);
    SDL_SetTextureAlphaMod(sniper_scope_texture_, 255U);
    SDL_RenderTexture(renderer_, sniper_scope_texture_, nullptr, &scope);
    if (scope.x > 0.0F)
    {
        fill({0.0F, 0.0F, scope.x, window_height}, {0U, 0U, 0U, 255U});
        fill({scope.x + scope.w, 0.0F, window_width - scope.x - scope.w, window_height}, {0U, 0U, 0U, 255U});
    }
}

void rm_draw_notice(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &area)
{
    if (toast_time_ <= 0.0F || area.empty() || (toast_title_.empty() && toast_body_.empty()))
        return;
    const float opacity = smoothstep(std::min({1.0F, (4.2F - toast_time_) * 5.0F, toast_time_ * 4.0F}));
    constexpr float pad = 10.0F;
    constexpr float line = 18.0F;
    const float inner = area.width - pad * 2.0F - 3.0F;
    // (Notices are written in capitals; here they are set like everything
    // else: the title in Title Case, the line under it as a sentence.)
    const std::string title = menu_value_case(toast_title_);
    std::string sentence = toast_body_;
    if (std::ranges::none_of(sentence, [](char c) { return std::islower(static_cast<unsigned char>(c)) != 0; }))
    {
        bool start = true;
        for (char &c : sentence)
        {
            const auto letter = static_cast<unsigned char>(c);
            c = static_cast<char>(start ? std::toupper(letter) : std::tolower(letter));
            if (std::isalpha(letter) != 0)
                start = false;
            else if (c == '.' || c == '!' || c == '?')
                start = true;
        }
    }
    const auto body = rm_wrap(canvas, sentence, rm_cap_note, inner, 3U);
    const float height = pad * 2.0F + 16.0F + static_cast<float>(body.size()) * line + (body.empty() ? 0.0F : 4.0F);
    // Slides in from the edge it is anchored to.
    const float slide = settings_.reduced_motion ? 0.0F : (1.0F - opacity) * 14.0F;
    const remastered::Rect box{area.x + slide, area.y, area.width, height};
    rm_fill(canvas, box, with_alpha(theme.plate, opacity * 0.72F));
    rm_fill(canvas, {box.x, box.y, 3.0F, box.height}, with_alpha(toast_urgent_ ? theme.caution : theme.accent, opacity));
    rm_label(canvas, title, {box.x + pad + 3.0F, box.y + pad - 1.0F, inner, 16.0F}, rm_cap_small,
             with_alpha(theme.text, opacity));
    for (std::size_t index = 0U; index < body.size(); ++index)
        rm_label(canvas, body[index],
                 {box.x + pad + 3.0F, box.y + pad + 20.0F + static_cast<float>(index) * line, inner, line},
                 rm_cap_note, with_alpha(theme.quiet, opacity), TextAlign::left, false, MenuFace::text);
}

void rm_draw_chat(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &area)
{
    if (!settings_.hud_chat || !online_gameplay_active() || online_chat_lines_.empty() || area.empty())
        return;
    constexpr float cap = rm_cap_small;
    constexpr float line = 19.0F;
    constexpr float pad = 8.0F;
    struct Shown final
    {
        std::string text;
        float opacity{};
    };
    // Newest last; each message is solid for ten seconds and fades over three,
    // all of them solid while the chat entry is open.
    std::vector<Shown> lines;
    const auto capacity = static_cast<std::size_t>(std::max(1.0F, std::floor((area.height - pad) / line)));
    for (const OnlineChatLine &entry : online_chat_lines_)
    {
        const float age = elapsed_ - entry.received_at;
        const float opacity = chat_editing_ ? 1.0F : std::clamp(1.0F - (age - 10.0F) / 3.0F, 0.0F, 1.0F);
        if (opacity <= 0.0F)
            continue;
        for (std::string &part : rm_wrap(canvas, entry.message, cap, area.width - pad * 2.0F, 3U))
            lines.push_back({std::move(part), opacity});
    }
    if (lines.empty())
        return;
    if (lines.size() > capacity)
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(capacity));
    float strongest = 0.0F;
    for (const Shown &entry : lines)
        strongest = std::max(strongest, entry.opacity);
    const float height = static_cast<float>(lines.size()) * line + pad;
    const remastered::Rect box{area.x, area.bottom() - height, area.width, height};
    rm_fill(canvas, box, with_alpha(theme.plate, strongest * 0.42F));
    rm_fill(canvas, {box.x, box.y, 2.0F, box.height}, with_alpha(theme.accent, strongest));
    for (std::size_t index = 0U; index < lines.size(); ++index)
        rm_label(canvas, lines[index].text,
                 {box.x + pad, box.y + pad * 0.5F + static_cast<float>(index) * line, box.width - pad * 2.0F, line},
                 cap, with_alpha(theme.text, lines[index].opacity), TextAlign::left, true, MenuFace::text);
}

void rm_draw_roster(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &area)
{
    if (!online_gameplay_active() || area.empty())
        return;
    struct Entry final
    {
        std::uint8_t gid{};
        std::string name;
        std::string_view role;
        bool active{};
    };
    std::vector<Entry> entries;
    const bool local_host = online_host_ != nullptr && online_host_->running();
    entries.push_back({online_local_player_gid(),
                       online_persona_name_.empty() ? std::string{"OpenMinerZ"} : online_persona_name_,
                       local_host ? std::string_view{"Host - You"} : std::string_view{"You"}, true});
    for (const auto &[gid, remote] : online_remote_players_)
        entries.push_back({gid,
                           remote.gamer_tag.empty() ? std::format("Player {}", static_cast<int>(gid))
                                                    : remote.gamer_tag,
                           gid == 0U ? std::string_view{"Host"} : std::string_view{"Player"},
                           remote.has_update && elapsed_ - remote.last_update <= 10.0F});
    std::ranges::sort(entries, {}, &Entry::gid);
    constexpr float header = 46.0F;
    constexpr float row = 28.0F;
    constexpr float pad = 16.0F;
    const auto fitting = static_cast<std::size_t>(std::max(1.0F, std::floor((area.height - header - pad) / row)));
    const std::size_t shown = std::min(entries.size(), fitting);
    const remastered::Rect box{area.x, area.y, area.width, header + static_cast<float>(shown) * row + pad};
    rm_fill(canvas, box, with_alpha(theme.plate, 0.80F));
    rm_fill(canvas, {box.x, box.y, box.width, 3.0F}, theme.accent);
    rm_label(canvas, retail(RetailString::Players), {box.x + pad, box.y + 8.0F, box.width * 0.6F, header - 12.0F},
             rm_cap_title, theme.row);
    const int capacity = std::max(host_settings_.maximum_players, static_cast<int>(entries.size()));
    rm_label(canvas, std::format("{} / {}", entries.size(), capacity),
             {box.x + box.width * 0.6F, box.y + 8.0F, box.width * 0.4F - pad, header - 12.0F}, rm_cap_small,
             theme.quiet, TextAlign::right, false, MenuFace::text);
    rm_fill(canvas, {box.x + pad, box.y + header - 3.0F, box.width - pad * 2.0F, 1.0F}, theme.line);
    for (std::size_t index = 0U; index < shown; ++index)
    {
        const Entry &entry = entries[index];
        const float y = box.y + header + 4.0F + static_cast<float>(index) * row;
        rm_fill(canvas, {box.x + pad, y + row * 0.5F - 3.0F, 6.0F, 6.0F}, entry.active ? theme.good : theme.disabled);
        rm_label(canvas, entry.name, {box.x + pad + 16.0F, y, box.width * 0.64F - pad - 16.0F, row}, rm_cap_small,
                 entry.active ? theme.text : theme.disabled, TextAlign::left, false, MenuFace::text);
        rm_label(canvas, entry.role, {box.x + box.width * 0.64F, y, box.width * 0.36F - pad, row}, rm_cap_note,
                 entry.active ? theme.accent : theme.disabled, TextAlign::right, false, MenuFace::text);
    }
    if (shown < entries.size())
        rm_label(canvas, std::format("+ {} more", entries.size() - shown),
                 {box.x + pad, box.bottom() - pad - 1.0F, box.width - pad * 2.0F, pad}, rm_cap_note, theme.quiet,
                 TextAlign::right, false, MenuFace::text);
}

// The whole Remastered HUD.
void draw_remastered_hud()
{
    const RmTheme theme = rm_theme();
    const remastered::Canvas layout = rm_hud_canvas();
    const RetailCanvas canvas{layout.width, layout.height, 0.0F, 0.0F, layout.scale};
    const remastered::HudOptions options = rm_hud_options();
    remastered::HudLayout hud = remastered::hud_layout(layout, options);
    mod_ui_move_hud(hud); // UI area: elements a mod moved (app_mod_ui.inc)
    const auto &player = player_.state();
    const bool alive = player_life_ == PlayerLife::alive && !options_.diagnostic_death_seconds;
    const bool scoped_aim = hud_scoped_aim();
    const bool shouldering = hud_shouldering();
    const float health = std::clamp(player.health / 100.0F, 0.0F, 1.0F);
    const float window_width = static_cast<float>(metrics_.output_width);
    const float window_height = static_cast<float>(metrics_.output_height);

    // ---- Under everything: what the world is doing to the player.
    if (settings_.hud_damage)
    {
        float flash = 0.0F;
        for (const auto &indicator : damage_indicators_)
            flash = std::max(flash, std::clamp(1.0F - indicator.age / 0.45F, 0.0F, 1.0F));
        rm_draw_hurt((1.0F - health) * 0.62F + flash * 0.30F);
        // Lava is the original's: the picture reddens to solid as the player
        // goes under.
        const float lava = world::lava_submersion(player.position.y);
        if (lava > 0.0F)
            fill({0.0F, 0.0F, window_width, window_height}, {255U, 0U, 0U, static_cast<Uint8>(lava * 255.0F)});
    }
    if (settings_.hud_scope && scoped_aim)
        rm_draw_scope();
    if (settings_.hud_scope)
        draw_missile_lock(scoped_aim);
    if (settings_.hud_damage && alive)
        rm_draw_damage_direction(canvas, hud);
    if (settings_.hud_crosshair && alive && !shouldering)
        rm_draw_crosshair(canvas, hud);
    if (settings_.hud_target && alive && !scoped_aim && player.target.has_value())
        rm_label(canvas, mod_target_block_name(*player.target), hud.target_line, rm_cap_small,
                 with_alpha(theme.text, 0.92F), TextAlign::center, true);

    // The player list replaces the body of the HUD while its key is held.
    const bool roster = settings_.hud_roster && player_roster_visible_ && online_gameplay_active();

    // ---- Top centre.
    const auto meters = [&]() {
        if (settings_.vitals_style == VitalsStyle::none)
            return;
        rm_meter(canvas, theme, hud.health, health, theme.health, rm_heart,
                 settings_.vitals_style == VitalsStyle::classic, true);
        if (!hud.stamina.empty())
            rm_meter(canvas, theme, hud.stamina, std::clamp(player.stamina / 100.0F, 0.0F, 1.0F),
                     player_.stamina_blocked() ? SDL_Color{255U, 98U, 40U, 255U} : theme.stamina, rm_bolt, false,
                     true);
    };
    if (!roster)
    {
        rm_draw_compass(canvas, theme, hud.compass);
        if (!hud.distance.empty())
        {
            // "Distance 124   Max 300", in the game's own words, as one line.
            const std::string here = std::format("{} {}", retail(RetailString::Distance), current_distance_);
            const std::string best = std::format("{} {}", retail(RetailString::Max), max_distance_);
            const float here_width = rm_text_width(canvas, here, rm_cap_small) + 2.0F;
            const float best_width = rm_text_width(canvas, best, rm_cap_small) + 2.0F;
            const float left = hud.distance.center_x() - (here_width + 14.0F + best_width) * 0.5F;
            rm_label(canvas, here, {left, hud.distance.y, here_width, hud.distance.height}, rm_cap_small, theme.text,
                     TextAlign::left, true);
            rm_label(canvas, best, {left + here_width + 14.0F, hud.distance.y, best_width, hud.distance.height},
                     rm_cap_small, theme.quiet, TextAlign::left, true);
        }
        if (!hud.oxygen.empty())
        {
            const float air = std::clamp(player_oxygen_, 0.0F, 1.0F);
            rm_fill(canvas, hud.oxygen, theme.plate);
            rm_fill(canvas, {hud.oxygen.x + 2.0F, hud.oxygen.y + 2.0F, (hud.oxygen.width - 4.0F) * air, hud.oxygen.height - 4.0F},
                    with_alpha(theme.oxygen, air < 0.25F ? 1.0F : 0.82F));
            rm_label(canvas, "Oxygen", hud.oxygen, rm_cap_tiny, theme.text, TextAlign::center, true, MenuFace::text);
        }
        // The game's own "Day N" announcement, in this style.
        if (hud_day_banner_seconds_ < 9.0F)
        {
            const float shown = std::min({1.0F, hud_day_banner_seconds_ / 0.6F, (9.0F - hud_day_banner_seconds_) / 1.5F});
            rm_label(canvas, std::format("{} {}", retail(RetailString::Day), hud_banner_day_),
                     {hud.center_x - 240.0F, layout.height * 0.22F, 480.0F, 56.0F}, 36.0F,
                     with_alpha(theme.text, std::clamp(shown, 0.0F, 1.0F)), TextAlign::center, true);
        }
    }

    // ---- Top left: session cards.
    if (!hud.server.empty())
    {
        const std::string name =
            online_gameplay_active() ? (online_client_ != nullptr ? online_join_server_name_ : host_settings_.server_name)
                                     : (active_world_path_.empty() ? "Single Player" : active_world_path_.stem().string());
        const std::string detail =
            online_gameplay_active()
                ? std::format("{}  -  {} players", net::protocol_label(online_gameplay_protocol()),
                              online_remote_visuals_.size() + 1U)
                : menu_value_case(std::format("{}  -  {}", game_mode_label(game_mode_), game_difficulty_label(difficulty_)));
        rm_fill(canvas, hud.server, theme.plate);
        rm_label(canvas, name, {hud.server.x + 10.0F, hud.server.y + 5.0F, hud.server.width - 20.0F, 21.0F}, rm_cap_small,
                 theme.text);
        rm_label(canvas, detail, {hud.server.x + 10.0F, hud.server.y + 26.0F, hud.server.width - 20.0F, 17.0F}, rm_cap_note,
                 theme.quiet, TextAlign::left, false, MenuFace::text);
    }
    if (!hud.coordinates.empty())
    {
        rm_fill(canvas, hud.coordinates, theme.plate);
        rm_label(canvas,
                 std::format("X {}    Y {}    Z {}", static_cast<int>(std::floor(player.position.x)),
                             static_cast<int>(std::floor(player.position.y)), static_cast<int>(std::floor(player.position.z))),
                 {hud.coordinates.x + 10.0F, hud.coordinates.y, hud.coordinates.width - 20.0F, hud.coordinates.height},
                 rm_cap_note, theme.text, TextAlign::left, false, MenuFace::text);
    }

    // ---- Top right: clock.
    if (!hud.clock.empty())
    {
        const auto sky = current_sky_state();
        rm_fill(canvas, hud.clock, theme.plate);
        rm_label(canvas, menu_value_case(std::string{sky.period}),
                 {hud.clock.x + 10.0F, hud.clock.y + 4.0F, hud.clock.width * 0.58F, 16.0F}, rm_cap_note, theme.accent,
                 TextAlign::left, false, MenuFace::text);
        rm_label(canvas, std::format("{} {}", retail(RetailString::Day), static_cast<int>(std::floor(day_night_.day())) + 1),
                 {hud.clock.x + hud.clock.width * 0.58F, hud.clock.y + 4.0F, hud.clock.width * 0.42F - 10.0F, 16.0F},
                 rm_cap_note, theme.quiet, TextAlign::right, false, MenuFace::text);
        rm_label(canvas, day_night_.clock_text(), {hud.clock.x + 10.0F, hud.clock.y + 19.0F, hud.clock.width - 20.0F, 25.0F},
                 16.0F, theme.text);
        const remastered::Rect track{hud.clock.x + 10.0F, hud.clock.bottom() - 6.0F, hud.clock.width - 20.0F, 2.0F};
        rm_fill(canvas, track, {255U, 255U, 255U, 40U});
        rm_fill(canvas, {track.x, track.y, track.width * std::clamp(sky.time_of_day, 0.0F, 1.0F), track.height}, theme.accent);
    }

    // ---- Bottom centre: tray, meters, item line.
    if (!roster)
    {
        rm_draw_tray(canvas, theme, hud);
        meters();
        rm_draw_item_line(canvas, theme, hud.item_line);
    }
    else
    {
        meters();
    }
    // Captions go above whatever is highest in that stack.
    float stack_top = layout.safe.bottom();
    for (const remastered::Rect &part : {hud.tray, hud.item_line, hud.health})
        if (!part.empty() && part.y > layout.height * 0.5F)
            stack_top = std::min(stack_top, part.y);
    rm_caption_bottom_ = canvas.wy(stack_top - 8.0F);

    // ---- Left: chat.  Centre: the player list.
    rm_draw_chat(canvas, theme, hud.chat);
    if (roster)
        rm_draw_roster(canvas, theme, hud.roster);
}

// A sound caption ("[ ITEM CRAFTED ]"), set like the rest of this HUD: its
// words as a sentence on a wash, centred above the tray stack.
void rm_draw_caption()
{
    const remastered::Canvas layout = rm_hud_canvas();
    const RetailCanvas canvas{layout.width, layout.height, 0.0F, 0.0F, layout.scale};
    const RmTheme theme = rm_theme();
    std::string words = subtitle_text_;
    if (words.size() >= 4U && words.starts_with("[ ") && words.ends_with(" ]"))
        words = words.substr(2U, words.size() - 4U);
    bool start = true;
    for (char &c : words)
    {
        const auto letter = static_cast<unsigned char>(c);
        c = static_cast<char>(start ? std::toupper(letter) : std::tolower(letter));
        if (std::isalpha(letter) != 0)
            start = false;
    }
    const float cap = rm_cap_small * std::clamp(settings_.subtitle_size, 0.75F, 1.5F);
    const float width = std::min(rm_text_width(canvas, words, cap, MenuFace::text) + 28.0F, layout.safe.width - 20.0F);
    const float height = cap * 2.3F;
    const float bottom = rm_caption_bottom_ / canvas.scale;
    const float opacity = std::clamp(subtitle_time_ * 4.0F, 0.0F, 1.0F);
    const remastered::Rect box{layout.width * 0.5F - width * 0.5F, bottom - height, width, height};
    rm_fill(canvas, box, with_alpha(theme.plate, opacity * 0.72F));
    rm_label(canvas, words, {box.x + 10.0F, box.y, box.width - 20.0F, box.height}, cap, with_alpha(theme.text, opacity),
             TextAlign::center, false, MenuFace::text);
}

// The notice (toast) while in the world. Remastered draws it as part of its
// HUD. The Classic and Modern styles are exact copies of a HUD that has no
// such thing, so there only a failure the player must know about is shown.
void draw_hud_notice()
{
    if (retail_hud_ready())
    {
        if (toast_urgent_)
            draw_toast();
        return;
    }
    if (!settings_.hud_notifications && !toast_urgent_)
        return;
    const remastered::Canvas layout = rm_hud_canvas();
    const RetailCanvas canvas{layout.width, layout.height, 0.0F, 0.0F, layout.scale};
    remastered::HudLayout hud = remastered::hud_layout(layout, rm_hud_options());
    mod_ui_move_hud(hud); // UI area: a moved notice (app_mod_ui.inc)
    rm_draw_notice(canvas, rm_theme(), hud.notice);
}

// ---- Inventory, crate and crafting -------------------------------------------

[[nodiscard]] static remastered::Rect rm_rect(const screens::IntRect &rect) noexcept
{
    return {static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
            static_cast<float>(rect.height)};
}

// A key or button cap followed by what it does; returns where the next one
// starts, or `x` unchanged when it would not fit before `limit`.
float rm_prompt(const RetailCanvas &canvas, const RmTheme &theme, float x, float limit, float center_y,
                std::string_view key, std::string_view action)
{
    const float key_width = std::max(20.0F, rm_text_width(canvas, key, rm_cap_note, MenuFace::text) + 12.0F);
    const float action_width = rm_text_width(canvas, action, rm_cap_note, MenuFace::text) + 2.0F;
    const float total = key_width + 6.0F + action_width;
    if (x + total > limit)
        return x;
    const remastered::Rect cap{x, center_y - 10.0F, key_width, 20.0F};
    rm_fill(canvas, cap, {255U, 255U, 255U, 34U});
    rm_frame(canvas, cap, {255U, 255U, 255U, 80U});
    rm_label(canvas, key, cap, rm_cap_note, theme.text, TextAlign::center, false, MenuFace::text);
    rm_label(canvas, action, {cap.right() + 6.0F, center_y - 10.0F, action_width, 20.0F}, rm_cap_note, theme.quiet,
             TextAlign::left, false, MenuFace::text);
    return x + total + 16.0F;
}

// What a slot says about the stack in it: the retail name and description, and
// the numbers that matter for that kind of item.
struct RmItemFacts final
{
    std::string name;
    std::string description;
    std::string numbers;
};
[[nodiscard]] RmItemFacts rm_item_facts(const InventoryStack &stack) const
{
    const InventoryProfile world = inventory_.profile();
    const ItemText words = mod_item_words(stack, world); // (Polish) a mod item's own words
    RmItemFacts facts;
    facts.name = std::string{words.name};
    facts.description = std::string{words.description1};
    if (!words.description2.empty() && words.description2 != " ")
        facts.description += (facts.description.empty() ? "" : ". ") + std::string{words.description2};
    while (!facts.description.empty() && facts.description.front() == ' ')
        facts.description.erase(facts.description.begin());
    const auto *type = stack.mod_item != 0U ? mod_item_type(stack.mod_item) : nullptr;
    if (type != nullptr)
        facts.description = type->description; // whole, not as two retail lines
    // A mod item's own stack size (items.define `stack`), not its fallback's.
    const std::uint16_t limit =
        type != nullptr && type->stack_limit != 0U ? type->stack_limit : item_stack_limit(stack.item, world);
    if (const GunStats *gun = gun_stats(stack.item); gun != nullptr && gun->clip_capacity > 0U)
        facts.numbers = std::format("Clip {} / {}    {} {}", stack.rounds_in_clip, gun->clip_capacity,
                                    item_name(gun->ammo_item), inventory_.count(gun->ammo_item));
    else if (limit > 1U)
        facts.numbers = std::format("Stack {} / {}", stack.count, limit);
    if (item_self_damage_per_use(stack.item, world) > 0.0F)
        facts.numbers += std::format("{}Condition {}%", facts.numbers.empty() ? "" : "    ",
                                     static_cast<int>(std::lround(std::clamp(stack.durability, 0.0F, 1.0F) * 100.0F)));
    return facts;
}

void rm_draw_slot(const RetailCanvas &canvas, const RmTheme &theme, const screens::IntRect &hit,
                  const PlayerInventory::Slot *slot, bool cursor, bool in_hand, bool locked)
{
    // The slot is the hit rectangle less half the gap on every side.
    const float half = static_cast<float>(remastered::slot_gap) * 0.5F;
    const remastered::Rect box{static_cast<float>(hit.x) + half, static_cast<float>(hit.y) + half,
                               static_cast<float>(hit.width) - half * 2.0F, static_cast<float>(hit.height) - half * 2.0F};
    const bool filled = slot != nullptr && slot->has_value();
    rm_fill(canvas, box, cursor ? theme.band : (filled ? theme.slot_full : theme.slot));
    if (filled)
    {
        constexpr float inset = 5.0F;
        rm_icon(canvas, **slot, {box.x + inset, box.y + inset, box.width - inset * 2.0F, box.height - inset * 2.0F});
        rm_condition(canvas, **slot, box);
        rm_count(canvas, **slot, box, rm_cap_small, theme.text);
    }
    if (locked)
    {
        // Another player has this crate slot selected.
        rm_fill(canvas, box, {120U, 22U, 24U, 150U});
        rm_label(canvas, "In Use", box, rm_cap_tiny, theme.text, TextAlign::center, true, MenuFace::text);
    }
    // The item in hand keeps the accent under it, as on the HUD's tray.
    if (in_hand)
        rm_fill(canvas, {box.x, box.bottom() - 3.0F, box.width, 3.0F}, theme.accent);
    if (cursor)
        rm_frame(canvas, box, held_stack_.has_value() ? theme.caution : theme.text, 2.0F);
    else
        rm_frame(canvas, box, theme.line);
}

void rm_draw_item_card(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &card,
                       const InventoryStack *stack, std::string_view empty_text)
{
    rm_fill(canvas, card, theme.well);
    rm_frame(canvas, card, theme.line);
    constexpr float pad = 12.0F;
    if (stack == nullptr)
    {
        rm_label(canvas, empty_text, {card.x + pad, card.y, card.width - pad * 2.0F, card.height}, rm_cap_note,
                 theme.quiet, TextAlign::center, false, MenuFace::text);
        return;
    }
    const RmItemFacts facts = rm_item_facts(*stack);
    const float icon = std::min(56.0F, card.height - pad * 2.0F);
    const remastered::Rect plate{card.x + pad, card.y + pad, icon, icon};
    rm_fill(canvas, plate, theme.slot_full);
    rm_frame(canvas, plate, theme.line);
    rm_icon(canvas, *stack, {plate.x + 4.0F, plate.y + 4.0F, icon - 8.0F, icon - 8.0F});
    const float x = plate.right() + pad;
    const float width = card.right() - pad - x;
    rm_label(canvas, facts.name, {x, card.y + pad - 3.0F, width, 22.0F}, rm_cap_name, theme.row);
    float y = card.y + pad + 22.0F;
    if (!facts.numbers.empty())
    {
        rm_label(canvas, facts.numbers, {x, y, width, 18.0F}, rm_cap_note, theme.caution, TextAlign::left, false,
                 MenuFace::text);
        y += 19.0F;
    }
    const auto room = static_cast<std::size_t>(std::max(0.0F, std::floor((card.bottom() - pad * 0.5F - y) / 18.0F)));
    const auto lines = rm_wrap(canvas, facts.description, rm_cap_note, width, room);
    for (std::size_t index = 0U; index < lines.size(); ++index)
        rm_label(canvas, lines[index], {x, y + static_cast<float>(index) * 18.0F, width, 18.0F}, rm_cap_note, theme.quiet,
                 TextAlign::left, false, MenuFace::text);
}

// A floating card beside the pointer, or beside the slot the keys are on.
void rm_draw_tooltip(const RetailCanvas &canvas, const RmTheme &theme, const InventoryStack &stack,
                     const screens::IntRect &slot, bool at_pointer)
{
    const RmItemFacts facts = rm_item_facts(stack);
    constexpr float pad = 10.0F;
    constexpr float width = 290.0F;
    constexpr float line = 18.0F;
    const auto lines = rm_wrap(canvas, facts.description, rm_cap_note, width - pad * 2.0F, 4U);
    const float height = pad * 2.0F + 20.0F + (facts.numbers.empty() ? 0.0F : 19.0F) + static_cast<float>(lines.size()) * line;
    // In canvas units; kept inside the window.
    const float window_right = (static_cast<float>(metrics_.output_width) - canvas.left) / canvas.scale;
    const float window_bottom = (static_cast<float>(metrics_.output_height) - canvas.top) / canvas.scale;
    const float window_left = -canvas.left / canvas.scale;
    const float window_top = -canvas.top / canvas.scale;
    const float pointer_x = (mouse_x_ - canvas.left) / canvas.scale;
    const float pointer_y = (mouse_y_ - canvas.top) / canvas.scale;
    float x = at_pointer ? pointer_x + 14.0F : static_cast<float>(slot.right()) + 6.0F;
    float y = at_pointer ? pointer_y + 16.0F : static_cast<float>(slot.y);
    if (x + width > window_right - 4.0F)
        x = (at_pointer ? pointer_x - 10.0F : static_cast<float>(slot.x) - 6.0F) - width;
    x = std::clamp(x, window_left + 4.0F, std::max(window_left + 4.0F, window_right - width - 4.0F));
    y = std::clamp(y, window_top + 4.0F, std::max(window_top + 4.0F, window_bottom - height - 4.0F));
    const remastered::Rect box{x, y, width, height};
    rm_fill(canvas, {box.x + 3.0F, box.y + 3.0F, box.width, box.height}, {0U, 0U, 0U, 96U});
    rm_fill(canvas, box, {14U, 16U, 18U, 248U});
    rm_frame(canvas, box, {255U, 255U, 255U, 80U});
    rm_label(canvas, facts.name, {box.x + pad, box.y + pad - 2.0F, box.width - pad * 2.0F, 20.0F}, rm_cap_small, theme.row);
    float line_y = box.y + pad + 20.0F;
    if (!facts.numbers.empty())
    {
        rm_label(canvas, facts.numbers, {box.x + pad, line_y, box.width - pad * 2.0F, 18.0F}, rm_cap_note, theme.caution,
                 TextAlign::left, false, MenuFace::text);
        line_y += 19.0F;
    }
    for (std::size_t index = 0U; index < lines.size(); ++index)
        rm_label(canvas, lines[index], {box.x + pad, line_y + static_cast<float>(index) * line, box.width - pad * 2.0F, line},
                 rm_cap_note, theme.quiet, TextAlign::left, false, MenuFace::text);
}

// A text button on a black plate, as the menus' are: the pointed-at one runs
// to the focus colour.
void rm_draw_button(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &box,
                    std::string_view label, bool enabled, bool hot)
{
    rm_fill(canvas, box, {0U, 0U, 0U, static_cast<Uint8>(enabled && hot ? 215U : 165U)});
    if (enabled && hot)
        rm_fill(canvas, box, theme.band);
    rm_frame(canvas, box, enabled ? (hot ? theme.text : SDL_Color{255U, 255U, 255U, 100U}) : theme.line);
    rm_label(canvas, label, {box.x + 4.0F, box.y, box.width - 8.0F, box.height}, rm_cap_small,
             !enabled ? theme.disabled : (hot ? menu_focus_color() : theme.row), TextAlign::center);
}

// How many times a recipe can be made from what is carried (PlayerInventory's
// own aggregate counts); zero when it cannot.
[[nodiscard]] std::uint32_t rm_craftable_times(const CraftingRecipe &recipe) const
{
    std::uint32_t times = std::numeric_limits<std::uint32_t>::max();
    for (const CraftingIngredient &ingredient : recipe.ingredients)
        if (ingredient.count != 0U)
            times = std::min(times, crafting_ingredient_count(ingredient) / ingredient.count); // (Polish)
    return times == std::numeric_limits<std::uint32_t>::max() ? 0U : times;
}

void rm_draw_crafting(const RetailCanvas &canvas, const RmTheme &theme, const ScreenHost &host)
{
    namespace rm = remastered;
    const InventoryProfile world = inventory_.profile();
    const auto &book = host.recipes(); // (Polish) mod recipes follow the retail ones
    const auto list = screens_.tab_recipes(host);
    const auto selected = screens_.selected_recipe(host);
    const screens::Hot hot = screens_.hot();
    const int hot_index = screens_.hot_index();
    const bool keys_here = screens_.recipes_focused() && !screens_.pointer_active();
    const int rows = rm::recipe_rows(screens_.remastered_view().trays);

    // Section label, with how much can be made right now.
    std::size_t makeable = 0U;
    for (const std::size_t index : screens_.tab_recipes(host, screens::RecipeTab::all))
        makeable += host.can_craft(book[index]) ? 1U : 0U;
    rm_label(canvas, retail_text(RetailString::Crafting, RetailGeneration::modern),
             {static_cast<float>(rm::left_x), static_cast<float>(rm::content_y), 200.0F, static_cast<float>(rm::label_height)},
             rm_cap_small, theme.quiet);
    rm_label(canvas, makeable == 0U ? std::string{"Nothing can be made yet"} : std::format("{} can be made now", makeable),
             {static_cast<float>(rm::left_x + 200), static_cast<float>(rm::content_y), static_cast<float>(rm::grid_width - 200),
              static_cast<float>(rm::label_height)},
             rm_cap_note, makeable == 0U ? theme.quiet : theme.good, TextAlign::right, false, MenuFace::text);

    // Category tabs: the current one on the focus band with the accent under
    // it, as the settings pages are marked.
    static constexpr std::array<RetailString, 4> group_names{RetailString::Materials, RetailString::Tools,
                                                             RetailString::Weapons, RetailString::Structures};
    for (std::size_t tab = 0U; tab < screens::recipe_tab_count; ++tab)
    {
        const auto rect = screens_.tab_rect(static_cast<screens::RecipeTab>(tab));
        if (!rect)
            continue;
        const remastered::Rect box = rm_rect(*rect);
        const bool open = static_cast<screens::RecipeTab>(tab) == screens_.recipe_tab();
        const bool lit = hot == screens::Hot::tab && hot_index == static_cast<int>(tab);
        rm_fill(canvas, box, open ? theme.band : (lit ? theme.slot_full : theme.slot));
        if (open)
            rm_fill(canvas, {box.x, box.bottom() - 3.0F, box.width, 3.0F}, theme.accent);
        const std::string_view name =
            tab == 0U ? std::string_view{"All"} : retail_text(group_names[tab - 1U], RetailGeneration::modern);
        rm_label(canvas, name, {box.x + 3.0F, box.y, box.width - 6.0F, box.height - 3.0F}, rm_cap_small,
                 open || lit ? theme.text : theme.row, TextAlign::center);
    }

    // The list.
    const remastered::Rect well{static_cast<float>(rm::left_x), static_cast<float>(rm::recipe_list_y),
                                static_cast<float>(rm::recipe_list_width),
                                static_cast<float>(rows * rm::recipe_row_height)};
    rm_fill(canvas, well, theme.well);
    if (list.empty())
        rm_label(canvas, "No recipes here yet", well, rm_cap_note, theme.quiet, TextAlign::center, false,
                 MenuFace::text);
    const std::size_t scroll = screens_.recipe_scroll();
    for (int row = 0; row < rows && scroll + static_cast<std::size_t>(row) < list.size(); ++row)
    {
        const auto rect = screens_.recipe_row_rect(static_cast<std::size_t>(row));
        if (!rect)
            break;
        const remastered::Rect box = rm_rect(*rect);
        const std::size_t index = list[scroll + static_cast<std::size_t>(row)];
        const CraftingRecipe &recipe = book[index];
        const bool can = host.can_craft(recipe);
        const bool chosen = selected == index;
        const bool lit = (hot == screens::Hot::recipe || hot == screens::Hot::row_craft) && hot_index == row;
        if (chosen)
        {
            rm_fill(canvas, box, theme.band);
            rm_fill(canvas, {box.x, box.y, 3.0F, box.height}, theme.accent);
        }
        else if (lit)
        {
            rm_fill(canvas, box, theme.slot_full);
        }
        const float icon = box.height - 4.0F;
        rm_icon(canvas, recipe_result_stack(recipe), {box.x + 7.0F, box.y + 2.0F, icon, icon},
                can ? 1.0F : 0.5F, can ? 1.0F : 0.75F);
        // The right end: the row's own craft button, or how far off it is.
        const auto button = screens_.row_craft_rect(static_cast<std::size_t>(row));
        float right = box.right() - 6.0F;
        if (can && button)
        {
            const remastered::Rect plus = rm_rect(*button);
            const bool pressed = hot == screens::Hot::row_craft && hot_index == row;
            rm_fill(canvas, plus, with_alpha(theme.good, pressed ? 0.95F : 0.24F));
            rm_frame(canvas, plus, with_alpha(theme.good, pressed ? 1.0F : 0.75F));
            const SDL_Color ink = pressed ? SDL_Color{6U, 16U, 8U, 255U} : theme.good;
            rm_fill(canvas, {plus.center_x() - 5.0F, plus.center_y() - 1.0F, 10.0F, 2.0F}, ink);
            rm_fill(canvas, {plus.center_x() - 1.0F, plus.center_y() - 5.0F, 2.0F, 10.0F}, ink);
            right = plus.x - 6.0F;
        }
        else if (!can)
        {
            std::uint32_t have = 0U;
            std::uint32_t need = 0U;
            for (const CraftingIngredient &ingredient : recipe.ingredients)
                if (ingredient.count != 0U)
                {
                    ++need;
                    have += host.ingredient_count(ingredient) >= ingredient.count ? 1U : 0U;
                }
            const std::string progress = std::format("{}/{}", have, need);
            const float width = rm_text_width(canvas, progress, rm_cap_note, MenuFace::text) + 3.0F;
            rm_label(canvas, progress, {right - width, box.y, width, box.height}, rm_cap_note, theme.disabled,
                     TextAlign::right, false, MenuFace::text);
            right -= width + 6.0F;
        }
        const float name_x = box.x + 7.0F + icon + 8.0F;
        std::string name = mod_item_label(recipe_result_stack(recipe), item_name(recipe.result));
        if (recipe.result_count > 1U)
            name += std::format("  x{}", recipe.result_count);
        // The chosen row is marked by the band and the accent; under the keys
        // it also gets a frame. Its lettering stays as readable as the rest.
        if (chosen && keys_here)
            rm_frame(canvas, box, theme.text, 2.0F);
        const SDL_Color ink = chosen ? theme.text : (can ? theme.row : theme.disabled);
        rm_label(canvas, name, {name_x, box.y, right - name_x, box.height}, rm_cap_small, ink, TextAlign::left, false,
                 MenuFace::text);
    }
    // Scroll mark, as the menus' lists have.
    if (list.size() > static_cast<std::size_t>(rows))
    {
        const remastered::Rect track{well.right() - static_cast<float>(rm::scroll_width) + 2.0F, well.y, 3.0F, well.height};
        rm_fill(canvas, track, {255U, 255U, 255U, 48U});
        const float share = static_cast<float>(rows) / static_cast<float>(list.size());
        const float thumb = std::max(12.0F, track.height * share);
        const float at = static_cast<float>(scroll) / static_cast<float>(list.size() - static_cast<std::size_t>(rows));
        rm_fill(canvas, {track.x, track.y + (track.height - thumb) * at, track.width, thumb}, theme.accent);
    }

    // The detail card.
    const auto card_rect = screens_.detail_rect();
    if (!card_rect)
        return;
    const remastered::Rect card = rm_rect(*card_rect);
    rm_fill(canvas, card, theme.well);
    if (!selected)
    {
        rm_label(canvas, "Pick up materials", {card.x, card.center_y() - 20.0F, card.width, 20.0F}, rm_cap_note,
                 theme.quiet, TextAlign::center, false, MenuFace::text);
        rm_label(canvas, "to discover recipes", {card.x, card.center_y(), card.width, 20.0F}, rm_cap_note,
                 theme.quiet, TextAlign::center, false, MenuFace::text);
        return;
    }
    const CraftingRecipe &recipe = book[*selected];
    const bool can = host.can_craft(recipe);
    constexpr float pad = 10.0F;
    const remastered::Rect plate{card.x + pad, card.y + pad, 52.0F, 52.0F};
    rm_fill(canvas, plate, theme.slot_full);
    rm_frame(canvas, plate, can ? with_alpha(theme.good, 0.85F) : theme.line);
    rm_icon(canvas, recipe_result_stack(recipe), {plate.x + 4.0F, plate.y + 4.0F, 44.0F, 44.0F});
    const float text_x = plate.right() + pad;
    const float text_width = card.right() - pad - text_x;
    const auto title = rm_wrap(canvas, mod_item_label(recipe_result_stack(recipe), item_name(recipe.result)), rm_cap_small,
                               text_width, 2U, MenuFace::display);
    for (std::size_t index = 0U; index < title.size(); ++index)
        rm_label(canvas, title[index], {text_x, card.y + pad - 3.0F + static_cast<float>(index) * 18.0F, text_width, 18.0F},
                 rm_cap_small, theme.row);
    const std::uint32_t owned = host.ingredient_count({recipe.result, 0U, recipe.result_mod_item});
    rm_label(canvas,
             recipe.result_count > 1U ? std::format("Makes {}  -  have {}", recipe.result_count, owned)
                                      : std::format("You have {}", owned),
             {text_x, card.y + pad - 1.0F + static_cast<float>(title.size()) * 18.0F, text_width, 16.0F}, rm_cap_note,
             theme.quiet, TextAlign::left, false, MenuFace::text);
    // Ingredients, stacked upward from the buttons; the description takes
    // what is left between them and the title.
    float ingredients_top = card.bottom();
    int drawn = 0;
    for (const CraftingIngredient &ingredient : recipe.ingredients)
    {
        if (ingredient.count == 0U)
            continue;
        const auto rect = screens_.ingredient_rect(host, drawn);
        if (!rect)
            break;
        const remastered::Rect line = rm_rect(*rect);
        ingredients_top = std::min(ingredients_top, line.y);
        const std::uint32_t have = host.ingredient_count(ingredient);
        const bool enough = have >= ingredient.count;
        if (hot == screens::Hot::ingredient && hot_index == drawn)
            rm_fill(canvas, line, theme.slot_full);
        rm_icon(canvas, ingredient_stack(ingredient), {line.x + 2.0F, line.y + 1.0F, 20.0F, 20.0F},
                enough ? 1.0F : 0.7F);
        const std::string amounts = std::format("{} / {}", std::min<std::uint32_t>(have, 9999U), ingredient.count);
        const float amounts_width = rm_text_width(canvas, amounts, rm_cap_note, MenuFace::text) + 3.0F;
        rm_label(canvas, amounts, {line.right() - amounts_width - 2.0F, line.y, amounts_width, line.height}, rm_cap_note,
                 enough ? theme.good : theme.health, TextAlign::right, false, MenuFace::text);
        rm_label(canvas, mod_item_label(ingredient_stack(ingredient), item_name(ingredient.item)),
                 {line.x + 28.0F, line.y, line.width - 28.0F - amounts_width - 8.0F, line.height}, rm_cap_note,
                 enough ? theme.text : theme.quiet, TextAlign::left, false, MenuFace::text);
        ++drawn;
    }
    if (drawn > 0)
    {
        rm_fill(canvas, {card.x + pad, ingredients_top - 5.0F, card.width - pad * 2.0F, 1.0F}, theme.line);
        ingredients_top -= 8.0F;
    }
    const float description_y = card.y + pad + 52.0F + 8.0F;
    const auto room = static_cast<std::size_t>(std::max(0.0F, std::floor((ingredients_top - 2.0F - description_y) / 18.0F)));
    const ItemText words = item_text(recipe.result, world);
    std::string description{words.description1};
    if (!words.description2.empty() && words.description2 != " ")
        description += (description.empty() ? "" : ". ") + std::string{words.description2};
    if (const auto *type = mod_item_type(recipe.result_mod_item); type != nullptr)
        description = type->description; // (Polish) a mod item's own
    const auto lines = rm_wrap(canvas, description, rm_cap_note, card.width - pad * 2.0F, room);
    for (std::size_t index = 0U; index < lines.size(); ++index)
        rm_label(canvas, lines[index],
                 {card.x + pad, description_y + static_cast<float>(index) * 18.0F, card.width - pad * 2.0F, 18.0F},
                 rm_cap_note, theme.quiet, TextAlign::left, false, MenuFace::text);
    // Craft one / craft all. Neither can be used with a stack on the cursor,
    // and "all" is one in a world whose rules say so (1.9.9.8 Creative, any
    // world with infinite resources).
    const bool free_hands = !held_stack_.has_value();
    const bool many = can && !(world == InventoryProfile::modern && host.creative()) && !infinite_resources_ &&
                      rm_craftable_times(recipe) > 1U;
    if (const auto one = screens_.craft_rect(false))
        rm_draw_button(canvas, theme, rm_rect(*one), "Craft", can && free_hands, hot == screens::Hot::craft_one);
    if (const auto all = screens_.craft_rect(true))
        rm_draw_button(canvas, theme, rm_rect(*all),
                       many ? std::format("All  x{}", rm_craftable_times(recipe)) : std::string{"Craft All"},
                       many && free_hands, hot == screens::Hot::craft_all);
}

// The line of prompts along the bottom of the panel: what the mouse, the keys
// or the controller do right now.
void rm_draw_prompts(const RetailCanvas &canvas, const RmTheme &theme, const remastered::Rect &bar, bool crate)
{
    rm_fill(canvas, {bar.x, bar.y - 5.0F, bar.width, 1.0F}, theme.line);
    const float y = bar.center_y();
    const float limit = bar.right();
    float x = bar.x;
    const bool holding = held_stack_.has_value();
    const bool in_list = !crate && screens_.recipes_focused() && !screens_.pointer_active();
    const auto add = [&](std::string_view key, std::string_view action) {
        x = rm_prompt(canvas, theme, x, limit, y, key, action);
    };
    if (gamepad_input_active_)
    {
        if (in_list)
        {
            add("A", "Craft");
            add("Y", "Craft all");
            add("LB  RB", "Category");
            add("D-pad", "Recipe, right for slots");
        }
        else
        {
            add("A", holding ? "Place" : "Pick up");
            add("R3", holding ? "Place one" : "Split");
            add("Y", crate ? "Send across" : "Quick move");
            add("X", "Drop");
            if (!crate)
                add("LB  RB", "Category");
        }
        add("B", "Close");
        return;
    }
    if (!screens_.pointer_active())
    {
        // The keyboard has the cursor.
        if (in_list)
        {
            add("Up  Down", "Recipe");
            add("Enter", "Craft");
            add("Shift+Enter", "Craft all");
            add("PgUp  PgDn", "Category");
            add("Tab", "Slots");
        }
        else
        {
            add("Arrows", "Move");
            add("Enter", holding ? "Place" : "Pick up");
            add("F", holding ? "Place one" : "Split");
            add("T", crate ? "Send across" : "Quick move");
            add("Q", "Drop");
            add("Tab", crate ? "Other side" : "Recipes");
        }
        add("Esc", holding ? "Put back, close" : "Close");
        return;
    }
    // The mouse.
    const screens::Hot hot = screens_.hot();
    if (!crate && !holding && hot != screens::Hot::none && hot != screens::Hot::close)
    {
        // Over the crafting side.
        add("LMB", "Choose a recipe");
        add("+", "Craft it");
        add("Shift + click", "Craft all");
        add("Wheel", "Scroll");
        add("Esc", "Close");
        add("LMB on a part", "Go to its recipe");
        return;
    }
    if (holding)
    {
        add("LMB", "Place");
        add("RMB", "Place one");
        add("Wheel", "One at a time");
        add("Q", "Drop");
        add("Click outside", "Drop");
        add("Esc", "Put back, close");
        return;
    }
    add("LMB", "Pick up");
    add("RMB", "Split");
    add("Shift+LMB", crate ? "Send across" : "Quick move");
    add("Wheel", "Take one");
    add("Q", "Drop");
    add("Tab", crate ? "Other side" : "Recipes");
    add("Esc", "Close");
}

void draw_remastered_screens()
{
    namespace rm = remastered;
    sync_screens();
    const ScreenHost host{*this};
    screens_.held() = held_stack_;
    const RmTheme theme = rm_theme();
    const auto view = screens_.remastered_view();
    // Layout units measured from the panel's corner: the session's own space.
    const RetailCanvas canvas{0.0F, 0.0F, view.origin_x, view.origin_y, view.scale};
    const bool crate = screens_.screen() == screens::ScreenId::crate;
    const float window_width = static_cast<float>(metrics_.output_width);
    const float window_height = static_cast<float>(metrics_.output_height);
    if (options_.screen_backdrop.has_value())
    {
        // --screen-backdrop: a flat colour in place of the world, as for the
        // retail screens' captures.
        const auto &colour = *options_.screen_backdrop;
        fill({0.0F, 0.0F, window_width, window_height}, {colour[0], colour[1], colour[2], 255U});
    }
    else
    {
        // The in-game menu's wash over the world.
        fill({0.0F, 0.0F, window_width, window_height}, {0U, 0U, 0U, 128U});
    }
    const remastered::Rect panel = rm_rect(screens_.panel());
    rm_fill(canvas, panel, theme.panel);
    rm_fill(canvas, {panel.x, panel.y, panel.width, 3.0F}, theme.accent);

    // ---- Header: title, the meters (the world does not stop), close.
    const float pad = static_cast<float>(rm::panel_padding);
    rm_label(canvas, retail_text(crate ? RetailString::Crate : RetailString::Inventory, RetailGeneration::modern),
             {pad, 8.0F, 300.0F, static_cast<float>(rm::header_height) - 10.0F}, rm_cap_title, theme.row);
    const auto close = screens_.close_rect();
    if (close)
    {
        const remastered::Rect box = rm_rect(*close);
        const bool lit = screens_.hot() == screens::Hot::close;
        rm_fill(canvas, box, lit ? theme.band : theme.slot_full);
        rm_frame(canvas, box, lit ? theme.text : theme.line);
        // An X of two strokes, drawn as short steps so it is crisp at any size.
        const SDL_Color ink = lit ? theme.text : theme.quiet;
        for (int step = -5; step <= 5; ++step)
        {
            const auto offset = static_cast<float>(step);
            rm_fill(canvas, {box.center_x() + offset - 1.0F, box.center_y() + offset - 1.0F, 2.0F, 2.0F}, ink);
            rm_fill(canvas, {box.center_x() + offset - 1.0F, box.center_y() - offset - 1.0F, 2.0F, 2.0F}, ink);
        }
    }
    if (settings_.vitals_style != VitalsStyle::none)
    {
        const auto &player = player_.state();
        const bool stamina = inventory_.profile() == InventoryProfile::modern && game_mode_ != GameMode::endurance &&
                             game_mode_ != GameMode::dragon_endurance;
        const float meter_width = 170.0F;
        float right = (close ? static_cast<float>(close->x) : panel.right() - pad) - 18.0F;
        const float meter_y = static_cast<float>(rm::header_height) * 0.5F - 7.0F;
        if (stamina)
        {
            rm_meter(canvas, theme, {right - meter_width, meter_y, meter_width, 18.0F},
                     std::clamp(player.stamina / 100.0F, 0.0F, 1.0F),
                     player_.stamina_blocked() ? SDL_Color{255U, 98U, 40U, 255U} : theme.stamina, rm_bolt, false, false);
            right -= meter_width + 16.0F;
        }
        rm_meter(canvas, theme, {right - meter_width, meter_y, meter_width, 18.0F},
                 std::clamp(player.health / 100.0F, 0.0F, 1.0F), theme.health, rm_heart, false, false);
    }
    rm_fill(canvas, {pad, static_cast<float>(rm::header_height), panel.width - pad * 2.0F, 1.0F}, theme.line);

    // ---- Slots.
    const std::size_t selected = screens_.selected_slot();
    const bool cursor_shown = screens_.selector_visible();
    const screens::Point pointer{static_cast<int>(std::floor((mouse_x_ - view.origin_x) / view.scale)),
                                 static_cast<int>(std::floor((mouse_y_ - view.origin_y) / view.scale))};
    const auto selected_rect = screens_.slot_rect(selected);
    const bool pointer_on_slot =
        !gamepad_input_active_ && cursor_shown && selected_rect.has_value() && selected_rect->contains(pointer);
    const std::size_t in_hand = screens::tray_base + 8U * inventory_.current_tray_index() + inventory_.selected_index();
    const auto slot_of = [&](std::size_t index) -> const PlayerInventory::Slot * {
        if (index >= screens::crate_base)
            return host.crate_slot(index - screens::crate_base);
        if (index < screens::backpack_slots)
            return &inventory_.storage()[index];
        return &inventory_.tray((index - screens::tray_base) / 8U)[(index - screens::tray_base) % 8U];
    };
    const auto draw_slot = [&](std::size_t index) {
        const auto rect = screens_.slot_rect(index);
        if (!rect)
            return;
        rm_draw_slot(canvas, theme, *rect, slot_of(index), cursor_shown && index == selected, index == in_hand,
                     index >= screens::crate_base && host.crate_slot_locked(index - screens::crate_base));
    };
    rm_label(canvas, "Backpack",
             {static_cast<float>(rm::right_x), static_cast<float>(rm::content_y), 200.0F, static_cast<float>(rm::label_height)},
             rm_cap_small, theme.quiet);
    for (std::size_t index = 0U; index < screens::backpack_slots; ++index)
        draw_slot(index);
    const bool two_trays = view.trays > 1;
    rm_label(canvas, two_trays ? "Item Trays" : "Item Tray",
             {static_cast<float>(rm::right_x), static_cast<float>(rm::tray_label_y), 160.0F,
              static_cast<float>(rm::label_height)},
             rm_cap_small, theme.quiet);
    rm_label(canvas,
             two_trays ? std::format("Tray {} is in hand  -  {} switches", inventory_.current_tray_index() + 1U,
                                     gamepad_input_active_ ? "the tray button" : "G")
                       : std::string{"The marked slot is in hand"},
             {static_cast<float>(rm::right_x + 160), static_cast<float>(rm::tray_label_y),
              static_cast<float>(rm::grid_width - 160), static_cast<float>(rm::label_height)},
             rm_cap_note, with_alpha(theme.quiet, 0.85F), TextAlign::right, false, MenuFace::text);
    for (std::size_t index = 0U; index < 8U * static_cast<std::size_t>(view.trays); ++index)
        draw_slot(screens::tray_base + index);
    if (two_trays)
        for (std::size_t tray = 0U; tray < 2U; ++tray)
        {
            // A numbered tab in the gutter left of each tray row.
            const auto first = screens_.slot_rect(screens::tray_base + 8U * tray);
            if (!first)
                continue;
            const bool active = tray == inventory_.current_tray_index();
            const remastered::Rect tab{static_cast<float>(first->x) - 17.0F, static_cast<float>(first->y) + 4.0F, 15.0F,
                                       static_cast<float>(first->height) - 8.0F};
            rm_fill(canvas, tab, active ? theme.accent : SDL_Color{255U, 255U, 255U, 24U});
            rm_label_fit(canvas, std::to_string(tray + 1U), tab, rm_cap_tiny,
                     active ? SDL_Color{255U, 255U, 255U, 255U} : theme.quiet, TextAlign::center, false, MenuFace::text);
        }

    // ---- The left column.
    const remastered::Rect footer{pad, panel.bottom() - 12.0F - static_cast<float>(rm::footer_height) + 8.0F,
                                  panel.width - pad * 2.0F, static_cast<float>(rm::footer_height) - 8.0F};
    const PlayerInventory::Slot *chosen = cursor_shown ? slot_of(selected) : nullptr;
    const InventoryStack *chosen_stack = chosen != nullptr && chosen->has_value() ? &**chosen : nullptr;
    if (crate)
    {
        rm_label(canvas, retail_text(RetailString::Crate, RetailGeneration::modern),
                 {static_cast<float>(rm::left_x), static_cast<float>(rm::content_y), 200.0F,
                  static_cast<float>(rm::label_height)},
                 rm_cap_small, theme.quiet);
        for (std::size_t index = 0U; index < screens::crate_slots; ++index)
            draw_slot(screens::crate_base + index);
        // Under the crate: what the selected stack is.
        rm_draw_item_card(canvas, theme,
                          {static_cast<float>(rm::left_x), static_cast<float>(rm::tray_label_y) + 6.0F,
                           static_cast<float>(rm::grid_width),
                           static_cast<float>(rm::trays_bottom(view.trays) - rm::tray_label_y) - 6.0F},
                          held_stack_.has_value() ? &*held_stack_ : chosen_stack, "Choose an item to see what it is");
    }
    else
    {
        rm_draw_crafting(canvas, theme, host);
    }
    rm_draw_prompts(canvas, theme, footer, crate);

    // ---- On top: the tooltip, then the stack on the cursor.
    // (Beside the pointer; or beside the cursor once the keys have moved it -
    // not for the slot it merely opened on.)
    if (!crate && chosen_stack != nullptr && !held_stack_.has_value() && selected_rect &&
        (pointer_on_slot || screens_.keys_navigating()))
        rm_draw_tooltip(canvas, theme, *chosen_stack, *selected_rect, pointer_on_slot);
    if (held_stack_.has_value())
    {
        // With the pointer it rides on the pointer; with keys or the pad it
        // sits over the selected slot, so that slot's own contents stay
        // visible beneath it.
        // (Hung a little below and right of the pointer, so the slot it is
        // about to go into is not hidden under it.)
        constexpr float size = 48.0F;
        remastered::Rect box{};
        if (pointer_on_slot || !cursor_shown || !selected_rect)
            box = {static_cast<float>(pointer.x) - 10.0F, static_cast<float>(pointer.y) - 8.0F, size, size};
        else
            box = {static_cast<float>(selected_rect->right()) - 22.0F, static_cast<float>(selected_rect->y) - 14.0F, size, size};
        rm_fill(canvas, {box.x + 4.0F, box.y + 5.0F, box.width, box.height}, {0U, 0U, 0U, 110U});
        rm_fill(canvas, box, {32U, 34U, 36U, 240U});
        rm_frame(canvas, box, theme.caution, 2.0F);
        rm_icon(canvas, *held_stack_, {box.x + 4.0F, box.y + 4.0F, box.width - 8.0F, box.height - 8.0F});
        rm_condition(canvas, *held_stack_, box);
        rm_count(canvas, *held_stack_, box, rm_cap_small, theme.text);
    }
}

// The controller in the Remastered screens. A picks up and places (or crafts
// in the recipe list), X drops, Y sends a stack across (or crafts all), the
// right stick's click splits, the bumpers change category, B closes. Returns
// false for a button this does not use.
bool remastered_screens_button(SDL_GamepadButton button)
{
    const bool in_list = screens_.screen() == screens::ScreenId::inventory && screens_.recipes_focused() &&
                         !screens_.pointer_active();
    switch (button)
    {
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        screens_key(screens::Key::left);
        return true;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        screens_key(screens::Key::right);
        return true;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        screens_key(screens::Key::up);
        return true;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        screens_key(screens::Key::down);
        return true;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        screens_key(screens::Key::enter);
        return true;
    case SDL_GAMEPAD_BUTTON_WEST:
        screens_key(screens::Key::drop);
        return true;
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_START:
        screens_key(screens::Key::escape);
        return true;
    case SDL_GAMEPAD_BUTTON_NORTH:
        if (in_list)
            with_screens([&](ScreenHost &host) { screens_.craft_selected(host, true); });
        else
            screens_key(screens::Key::transfer);
        return true;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        screens_key(screens::Key::split);
        return true;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
    case SDL_GAMEPAD_BUTTON_BACK:
        screens_key(screens::Key::switch_pane);
        return true;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        screens_key(screens::Key::previous_category);
        return true;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        screens_key(screens::Key::next_category);
        return true;
    default:
        return false;
    }
}
