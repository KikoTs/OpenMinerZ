// Included inside Application::Impl (src/app.cpp).
//
// ---- The retail inventory, crate and crafting screens ----------------------
// screens::Session (openminerz/screens.hpp) is 1.6.3.8's BlockPickerScreen,
// CraftingUIScreen and CrateScreen and 1.9.9.8's CraftingScreen and
// CrateScreen: what they draw and what each input does. This renders its draw
// list with the build's own sprites, fonts and item icons and feeds it the
// pointer and keys. Which screens are shown is the one interface style
// (Settings::ui_style) - presentation only: the items, recipes, stack limits
// and trays are the world's, through ScreenHost.
//
// The Remastered style is drawn from the same session by app_remastered.inc.
// A content pack imported before the screen art existed has none of it; the
// Remastered screens (which need no retail art) are shown until it is
// re-imported.

// One build's screen sprites, indexed by screens::Sprite, and its icon atlas.
struct ScreenArt final
{
    std::array<SDL_Texture *, static_cast<std::size_t>(screens::Sprite::count)> textures{};
    SDL_Texture *frame{};       // screen_CtrlFrame.bmp: the button's nine pieces
    SDL_Texture *icons{};       // item_icons.bmp: InventoryItem._2DImages
    SDL_Texture *icons_large{}; // item_icons_large.bmp (1.9.9.8, 128 px cells)
    SDL_Cursor *pointer{};      // screen_MousePointer.bmp: the game's own arrow
    bool complete{};
};

// The original draws its 13x22 arrow last, straight to the window at the mouse
// position and never scaled - which is exactly what a system cursor made from
// the same image does, without a frame of lag.
[[nodiscard]] static SDL_Cursor *load_screen_pointer(const std::filesystem::path &file)
{
    SDL_Surface *loaded = SDL_LoadBMP(file.string().c_str());
    if (loaded == nullptr)
        return nullptr;
    SDL_Surface *surface = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(loaded);
    if (surface == nullptr)
        return nullptr;
    // XNA content is premultiplied; a cursor image is not.
    for (int y = 0; y < surface->h; ++y)
    {
        auto *row = reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(surface->pixels) +
                                                      static_cast<std::size_t>(y) *
                                                          static_cast<std::size_t>(surface->pitch));
        for (int x = 0; x < surface->w; ++x)
        {
            const std::uint32_t pixel = row[x];
            const std::uint32_t alpha = pixel >> 24U;
            if (alpha == 0U || alpha == 255U)
                continue;
            const auto straight = [alpha](std::uint32_t channel) {
                return std::min<std::uint32_t>(255U, (channel * 255U + alpha / 2U) / alpha);
            };
            row[x] = (alpha << 24U) | (straight((pixel >> 16U) & 255U) << 16U) |
                     (straight((pixel >> 8U) & 255U) << 8U) | straight(pixel & 255U);
        }
    }
    SDL_Cursor *cursor = SDL_CreateColorCursor(surface, 0, 0);
    SDL_DestroySurface(surface);
    return cursor;
}

// The game's arrow while one of its screens is up, the system's otherwise
// (and over the Remastered screens, which are not the game's).
void refresh_screen_pointer()
{
    SDL_Cursor *wanted = nullptr;
    if (inventory_open_ && screens_style() != UiStyle::remastered)
        wanted = (screens_modern_layout() ? modern_screen_art_ : classic_screen_art_).pointer;
    if (wanted == nullptr)
        wanted = mod_ui_cursor(); // UI area: a mod theme's pointer (app_mod_ui.inc)
    if (wanted == screens_pointer_cursor_)
        return;
    screens_pointer_cursor_ = wanted;
    SDL_SetCursor(wanted != nullptr ? wanted : SDL_GetDefaultCursor());
}

void load_screen_art(ScreenArt &art, UiStyle style, const std::filesystem::path &derived_data)
{
    const auto load = [&](const std::string &file, SDL_ScaleMode scale) {
        SDL_Texture *texture = load_bmp_texture(renderer_, derived_data / file);
        if (texture != nullptr)
        {
            // XNA content is premultiplied and SpriteBatch blends it that way.
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            SDL_SetTextureScaleMode(texture, scale);
        }
        return texture;
    };
    art.complete = true;
    for (std::size_t index = 0U; index < art.textures.size(); ++index)
    {
        const auto sprite = static_cast<screens::Sprite>(index);
        const std::string_view name = screens::sprite_sheet_name(sprite);
        if (name.empty() || name.starts_with("CtrlFrame") ||
            screens::sprite_size(style, sprite).width == 0)
            continue;
        art.textures[index] = load(std::format("screen_{}.bmp", name), SDL_SCALEMODE_LINEAR);
        art.complete = art.complete && art.textures[index] != nullptr;
    }
    // The button frame's nine pieces share one 5x5 image; nearest sampling
    // keeps a stretched piece from picking up its neighbour.
    art.frame = load("screen_CtrlFrame.bmp", SDL_SCALEMODE_NEAREST);
    art.icons = load("item_icons.bmp", SDL_SCALEMODE_LINEAR);
    art.icons_large = load("item_icons_large.bmp", SDL_SCALEMODE_LINEAR);
    art.pointer = load_screen_pointer(derived_data / "screen_MousePointer.bmp");
    art.complete = art.complete && art.frame != nullptr && art.icons != nullptr;
}

// The item's icon as the game itself renders it: the item's cell of the
// imported atlas (InventoryItem._2DImages). An item the style's own build does
// not have (a 1.9.9.8 item in the Classic look) comes from the other build's
// atlas. False when no imported atlas has it.
bool draw_retail_item_icon(InventoryItemId item, UiStyle style, const SDL_FRect &destination,
                           screens::Color tint = {}, bool large = false)
{
    const UiStyle other = style == UiStyle::modern ? UiStyle::classic : UiStyle::modern;
    for (const UiStyle build : {style, other})
    {
        const int icon = screens::retail_icon(item, build);
        const ScreenArt &art = build == UiStyle::modern ? modern_screen_art_ : classic_screen_art_;
        if (icon < 0 || art.icons == nullptr)
            continue;
        const bool wide = large && art.icons_large != nullptr;
        SDL_Texture *texture = wide ? art.icons_large : art.icons;
        const float cell = wide ? 128.0F : 64.0F;
        const SDL_FRect source{static_cast<float>(icon & 7) * cell, static_cast<float>(icon / 8) * cell,
                               cell, cell};
        SDL_SetTextureColorMod(texture, tint.r, tint.g, tint.b);
        SDL_SetTextureAlphaMod(texture, tint.a);
        SDL_RenderTexture(renderer_, texture, &source, &destination);
        return true;
    }
    return false;
}

class ScreenMetrics final : public screens::TextMetrics
{
  public:
    explicit ScreenMetrics(const RetailFonts &fonts) : fonts_(fonts) {}
    [[nodiscard]] screens::Extent measure(screens::Font font, std::string_view value) const override
    {
        const TextExtent extent = fonts_.measure(retail_font(font), value);
        return {extent.width, extent.height};
    }
    [[nodiscard]] int line_spacing(screens::Font font) const override
    {
        const SpriteFont *loaded = fonts_.font(retail_font(font));
        return loaded != nullptr ? loaded->line_spacing() : 0;
    }
    [[nodiscard]] static RetailFont retail_font(screens::Font font) noexcept
    {
        switch (font)
        {
        case screens::Font::medium:
            return RetailFont::medium;
        case screens::Font::myriad_medium:
            return RetailFont::myriad_medium;
        case screens::Font::small:
        case screens::Font::count:
            break;
        }
        return RetailFont::small;
    }

  private:
    const RetailFonts &fonts_;
};

// The game as the screens see it. Every rule is answered by the world's own
// code; nothing here depends on which look is shown.
class ScreenHost final : public screens::Host
{
  public:
    explicit ScreenHost(Impl &app) : app_(app) {}
    [[nodiscard]] PlayerInventory &inventory() noexcept override { return app_.inventory_; }
    [[nodiscard]] const PlayerInventory &inventory() const noexcept override { return app_.inventory_; }
    [[nodiscard]] PlayerInventory::Slot *crate_slot(std::size_t index) noexcept override
    {
        if (!app_.active_crate_.has_value() || index >= crate_slot_count)
            return nullptr;
        return &app_.crate_inventories_[*app_.active_crate_][index];
    }
    [[nodiscard]] const PlayerInventory::Slot *crate_slot(std::size_t index) const noexcept override
    {
        if (!app_.active_crate_.has_value() || index >= crate_slot_count)
            return nullptr;
        const auto crate = app_.crate_inventories_.find(*app_.active_crate_);
        return crate != app_.crate_inventories_.end() ? &crate->second[index] : nullptr;
    }
    [[nodiscard]] bool crate_slot_locked(std::size_t index) const noexcept override
    {
        return app_.inventory_slot_locked(crate_slot_base + index);
    }
    [[nodiscard]] bool can_craft(const CraftingRecipe &recipe) const noexcept override
    {
        return app_.can_craft_recipe(recipe);
    }
    void craft(std::size_t recipe_index, bool all) override { app_.craft_recipe(recipe_index, all, false); }
    [[nodiscard]] bool creative() const noexcept override { return app_.game_mode_ == GameMode::creative; }
    void play(screens::Cue cue) override { app_.sound_bank_->play_cue(screens::cue_name(cue)); }
    void drop(const InventoryStack &stack) override
    {
        // PickupManager.CreatePickup at the player's feet; the screen plays
        // "dropitem" itself.
        if (app_.world_preview_ == nullptr)
            return;
        app_.world_preview_->spawn_dropped_stack(stack, app_.player_.state());
        app_.caption("[ ITEM DROPPED ]");
    }
    void crate_changed(std::size_t) override {}
    void crate_focus(screens::CrateFocusCause cause, std::size_t selected) override
    {
        app_.announce_crate_focus(cause, selected);
    }
    // (Polish) Mod items show their own name and description; mod recipes
    // follow the retail cookbook.
    [[nodiscard]] ItemText item_words(const InventoryStack &stack, InventoryProfile profile) const override
    {
        return app_.mod_item_words(stack, profile);
    }
    [[nodiscard]] const std::vector<CraftingRecipe> &recipes() const override
    {
        return app_.mod_gameplay_recipe_book();
    }
    [[nodiscard]] std::uint32_t ingredient_count(const CraftingIngredient &ingredient) const override
    {
        return app_.crafting_ingredient_count(ingredient);
    }

  private:
    Impl &app_;
};

[[nodiscard]] bool screens_modern_layout() const noexcept
{
    const screens::Layout layout = screens_.layout();
    return layout == screens::Layout::modern_inventory || layout == screens::Layout::modern_crate;
}

// The style the inventory, crate and crafting screens are drawn in: the
// interface style, or Remastered when that style is a retail copy whose
// sprites and fonts have not been imported (Remastered needs neither).
[[nodiscard]] UiStyle screens_style() noexcept
{
    const UiStyle wanted = interface_style();
    if (wanted == UiStyle::remastered)
        return wanted;
    const ScreenArt &art = wanted == UiStyle::modern ? modern_screen_art_ : classic_screen_art_;
    const RetailFonts *fonts = wanted == UiStyle::modern ? modern_fonts_.get() : classic_fonts_.get();
    if (art.complete && fonts != nullptr && fonts->complete())
        return wanted;
    if (!screens_stand_in_logged_)
    {
        screens_stand_in_logged_ = true;
        SDL_Log("Inventory screens: the content pack has no retail screen art (screen_*.bmp, "
                "item_icons.bmp, font_*.omfnt) for the %s style; showing the Remastered screens. "
                "Re-run the content import to get the original ones.",
                wanted == UiStyle::modern ? "Modern" : "Classic");
    }
    return UiStyle::remastered;
}

// Every style is drawn from screens::Session now, so this is always true. (It
// used to say whether the retail art was imported; without it a separate
// stand-in panel with its own input code was shown. Remastered replaced that.)
[[nodiscard]] bool retail_screens_ready() noexcept
{
    return true;
}

// Keep the session on the screen the game has open.
void sync_screens()
{
    screens_.configure(screens_style(), metrics_.output_width, metrics_.output_height,
                       settings_.ui_scale, settings_.safe_area);
    if (!inventory_open_)
    {
        if (screens_.is_open())
            screens_.abandon();
        return;
    }
    const bool crate = active_crate_.has_value();
    if (screens_.is_open() && crate == (screens_.screen() == screens::ScreenId::crate))
        return;
    ScreenHost host{*this};
    screens_.held() = held_stack_;
    screens_.open(host, crate ? screens::ScreenId::crate : screens::ScreenId::inventory);
    held_stack_ = screens_.held();
    // The pointer is wherever it was; opening a screen is not a mouse move, so
    // the selector starts on the active tray slot as it does in the original.
    screens_pointer_x_ = mouse_x_;
    screens_pointer_y_ = mouse_y_;
    screens_.place_pointer(static_cast<int>(mouse_x_), static_cast<int>(mouse_y_));
}

// Run one input through the session and carry its outcome back into the game:
// the held stack, a closed screen, crate traffic, the active item.
template <typename Action> void with_screens(Action &&action)
{
    sync_screens();
    if (!screens_.is_open())
        return;
    const std::optional<CrateKey> crate_location = active_crate_;
    std::optional<CrateSlots> crate_before;
    if (crate_location.has_value())
        crate_before = crate_inventories_[*crate_location];
    screens_.held() = held_stack_;
    ScreenHost host{*this};
    action(host);
    held_stack_ = screens_.held();
    const std::size_t selected = screens_.selected_slot();
    inventory_focus_ = selected >= screens::crate_base
                           ? crate_slot_base + (selected - screens::crate_base)
                           : selected < PlayerInventory::storage_size
                                 ? selected
                                 : PlayerInventory::storage_size + (selected - screens::tray_base) % 8U;
    if (crate_location.has_value() && crate_before.has_value())
        publish_crate_differences(*crate_location, *crate_before);
    sync_selected_item();
    if (!screens_.is_open())
        close_inventory();
}

void screens_button(float x, float y, Uint8 button, bool down)
{
    if (button != SDL_BUTTON_LEFT && button != SDL_BUTTON_RIGHT)
        return;
    const screens::Button which = button == SDL_BUTTON_LEFT ? screens::Button::left : screens::Button::right;
    const bool shift = (SDL_GetModState() & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)) != 0U;
    with_screens([&](ScreenHost &host) {
        if (x != screens_pointer_x_ || y != screens_pointer_y_)
        {
            screens_pointer_x_ = x;
            screens_pointer_y_ = y;
            screens_.pointer_moved(host, static_cast<int>(x), static_cast<int>(y));
        }
        if (down)
            screens_.button_pressed(host, which, shift);
        else
            screens_.button_released(host, which);
    });
}

void screens_key(screens::Key key)
{
    const bool shift = (SDL_GetModState() & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)) != 0U;
    with_screens([&](ScreenHost &host) { screens_.key_pressed(host, key, shift); });
}

// The keyboard, as the original screens read it. Returns false for a key they
// do not use.
bool screens_keycode(SDL_Keycode key)
{
    switch (key)
    {
    case SDLK_ESCAPE:
        screens_key(screens::Key::escape);
        return true;
    case SDLK_E:
        screens_key(screens::Key::inventory);
        return true;
    case SDLK_Q:
        screens_key(screens::Key::drop);
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        screens_key(screens::Key::enter);
        return true;
    case SDLK_UP:
        screens_key(screens::Key::up);
        return true;
    case SDLK_DOWN:
        screens_key(screens::Key::down);
        return true;
    case SDLK_LEFT:
        screens_key(screens::Key::left);
        return true;
    case SDLK_RIGHT:
        screens_key(screens::Key::right);
        return true;
    default:
        break;
    }
    // The Remastered screens add keys for what the retail ones leave to the
    // mouse. Each is an operation the pointer already has (screens::Key).
    if (screens_.style() != UiStyle::remastered)
        return false;
    switch (key)
    {
    case SDLK_W:
        screens_key(screens::Key::up);
        return true;
    case SDLK_S:
        screens_key(screens::Key::down);
        return true;
    case SDLK_A:
        screens_key(screens::Key::left);
        return true;
    case SDLK_D:
        screens_key(screens::Key::right);
        return true;
    case SDLK_SPACE:
        screens_key(screens::Key::enter);
        return true;
    case SDLK_F:
        screens_key(screens::Key::split);
        return true;
    case SDLK_T:
        screens_key(screens::Key::transfer);
        return true;
    case SDLK_TAB:
        screens_key(screens::Key::switch_pane);
        return true;
    case SDLK_PAGEDOWN:
    case SDLK_RIGHTBRACKET:
        screens_key(screens::Key::next_category);
        return true;
    case SDLK_PAGEUP:
    case SDLK_LEFTBRACKET:
        screens_key(screens::Key::previous_category);
        return true;
    default:
        return false;
    }
}

// Once a frame while a screen is up: the pointer if it moved, then the
// screen's own update (recipe strip scroll, pop-up fades, key repeat).
void update_screens(float seconds)
{
    const bool *keys = SDL_GetKeyboardState(nullptr);
    screens::HeldKeys held;
    if (keys != nullptr && !text_editing())
    {
        held.up = keys[SDL_SCANCODE_UP];
        held.down = keys[SDL_SCANCODE_DOWN];
        held.left = keys[SDL_SCANCODE_LEFT];
        held.right = keys[SDL_SCANCODE_RIGHT];
        if (screens_.style() == UiStyle::remastered)
        {
            // Remastered also moves its cursor with W A S D and the D-pad.
            held.up = held.up || keys[SDL_SCANCODE_W];
            held.down = held.down || keys[SDL_SCANCODE_S];
            held.left = held.left || keys[SDL_SCANCODE_A];
            held.right = held.right || keys[SDL_SCANCODE_D];
        }
    }
    if (gamepad_ != nullptr && screens_.style() == UiStyle::remastered)
    {
        held.up = held.up || SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_DPAD_UP);
        held.down = held.down || SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        held.left = held.left || SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        held.right = held.right || SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    }
    with_screens([&](ScreenHost &host) {
        if (!gamepad_input_active_ && (mouse_x_ != screens_pointer_x_ || mouse_y_ != screens_pointer_y_))
        {
            screens_pointer_x_ = mouse_x_;
            screens_pointer_y_ = mouse_y_;
            screens_.pointer_moved(host, static_cast<int>(mouse_x_), static_cast<int>(mouse_y_));
        }
        screens_.update(host, seconds, held);
    });
}

// --screen-script: drive the open screen the way ScreensParityProbe drives the
// original, so a capture shows the same scripted state. Commands are separated
// by ';':
//   clear                                   empty the backpack and trays
//   give <slot>=<id>x<count>[@health] ...   0..31 backpack, 32..47 trays, 100..131 crate
//   open inventory|crafting|crate
//   move <slot>|recipe<n>|ingredient<n>|tier1:<n>|tier2:<n>|close|button|out|panel|<x>,<y>
//   click left|right [shift]     wheel up|down     wait <seconds>
//   key Escape|E|Q|Enter|Up|Down|Left|Right
void run_screen_script(std::string_view script)
{
    const auto words_of = [](std::string_view text) {
        std::vector<std::string> words;
        std::size_t start = 0U;
        while (start < text.size())
        {
            while (start < text.size() && text[start] == ' ')
                ++start;
            std::size_t end = text.find(' ', start);
            if (end == std::string_view::npos)
                end = text.size();
            if (end > start)
                words.emplace_back(text.substr(start, end - start));
            start = end;
        }
        return words;
    };
    const auto frame = [&]() { update_screens(0.017F); };
    std::size_t begin = 0U;
    while (begin <= script.size())
    {
        std::size_t end = script.find(';', begin);
        if (end == std::string_view::npos)
            end = script.size();
        const auto words = words_of(script.substr(begin, end - begin));
        begin = end + 1U;
        if (words.empty())
            continue;
        const std::string &verb = words[0];
        if (verb == "give")
        {
            for (std::size_t index = 1U; index < words.size(); ++index)
            {
                const std::string &entry = words[index];
                const auto equals = entry.find('=');
                const auto times = entry.find('x', equals == std::string::npos ? 0U : equals);
                if (equals == std::string::npos || times == std::string::npos)
                    continue;
                const auto at = entry.find('@', times);
                const auto slot = static_cast<std::size_t>(std::atoi(entry.c_str()));
                InventoryStack stack{static_cast<InventoryItemId>(std::atoi(entry.c_str() + equals + 1U)),
                                     static_cast<std::uint16_t>(std::atoi(entry.c_str() + times + 1U))};
                if (at != std::string::npos)
                    stack.durability = static_cast<float>(std::atof(entry.c_str() + at + 1U));
                if (!valid_inventory_item_id(stack.item))
                    continue;
                if (slot >= screens::crate_base && slot < screens::crate_base + crate_slot_count)
                    crate_inventories_[active_crate_.value_or(CrateKey{0, 0, 0})][slot - screens::crate_base] =
                        stack;
                else if (slot < PlayerInventory::storage_size)
                    inventory_.storage()[slot] = stack;
                else if (slot < screens::tray_base + 16U)
                    inventory_.tray((slot - screens::tray_base) / 8U)[(slot - screens::tray_base) % 8U] = stack;
            }
            sync_selected_item();
        }
        else if (verb == "clear")
        {
            for (auto &slot : inventory_.storage())
                slot.reset();
            for (auto &tray : inventory_.trays())
                for (auto &slot : tray)
                    slot.reset();
            sync_selected_item();
        }
        else if (verb == "open" && words.size() > 1U)
        {
            inventory_open_ = true;
            if (words[1] == "crate")
                active_crate_ = CrateKey{0, 0, 0};
            else
                active_crate_.reset();
            screens_.abandon();
            sync_screens();
            if (words[1] == "crafting")
            {
                ScreenHost host{*this};
                screens_.open(host, screens::ScreenId::crafting);
            }
            frame();
        }
        else if (verb == "move" && words.size() > 1U)
        {
            sync_screens();
            ScreenHost host{*this};
            const std::string &target = words[1];
            const screens::IntRect back = screens_.panel();
            screens::Point place{};
            const auto number_after = [&](std::size_t offset) { return std::atoi(target.c_str() + offset); };
            if (target == "out")
                place = {back.x - 40, back.y + 60};
            else if (target == "panel")
                place = {back.x + 12, back.bottom() - 12};
            else if (target == "close")
                place = screens_.close_rect().value_or(screens::IntRect{}).center();
            else if (target == "button")
                place = screens_.button_rect().value_or(screens::IntRect{}).center();
            else if (target == "craft" || target == "craftall")
                place = screens_.craft_rect(target == "craftall").value_or(screens::IntRect{}).center();
            else if (target.starts_with("tab:"))
                place = screens_.tab_rect(static_cast<screens::RecipeTab>(number_after(4U)))
                            .value_or(screens::IntRect{})
                            .center();
            else if (target.starts_with("rowcraft"))
                place = screens_.row_craft_rect(static_cast<std::size_t>(number_after(8U)))
                            .value_or(screens::IntRect{})
                            .center();
            else if (target.starts_with("recipe"))
                place = (screens_modern_layout() || screens_.style() == UiStyle::remastered
                             ? screens_.recipe_rect(host, static_cast<std::size_t>(number_after(6U)))
                             : screens_.strip_rect(number_after(6U)))
                            .value_or(screens::IntRect{})
                            .center();
            else if (target.starts_with("ingredient"))
                place = screens_.ingredient_rect(host, number_after(10U)).value_or(screens::IntRect{}).center();
            else if (target.starts_with("tier1:"))
                place = screens_.group_rect(static_cast<std::size_t>(number_after(6U)))
                            .value_or(screens::IntRect{})
                            .center();
            else if (target.starts_with("tier2:"))
                place = screens_.type_rect(static_cast<std::size_t>(number_after(6U)))
                            .value_or(screens::IntRect{})
                            .center();
            else if (const auto comma = target.find(','); comma != std::string::npos)
                place = {std::atoi(target.c_str()), number_after(comma + 1U)};
            else
                place = screens_.slot_rect(static_cast<std::size_t>(std::atoi(target.c_str())))
                            .value_or(screens::IntRect{})
                            .center();
            const screens::Point window = screens_.to_window(place);
            mouse_x_ = static_cast<float>(window.x);
            mouse_y_ = static_cast<float>(window.y);
            gamepad_input_active_ = false;
            frame();
        }
        else if (verb == "click" && words.size() > 1U)
        {
            const screens::Button which = words[1] == "right" ? screens::Button::right : screens::Button::left;
            const bool shift = words.size() > 2U && words[2] == "shift";
            with_screens([&](ScreenHost &host) { screens_.button_pressed(host, which, shift); });
            frame();
            with_screens([&](ScreenHost &host) { screens_.button_released(host, which); });
            frame();
        }
        else if (verb == "wheel" && words.size() > 1U)
        {
            with_screens([&](ScreenHost &host) { screens_.wheel(host, words[1] == "up" ? 1 : -1); });
            frame();
        }
        else if (verb == "key" && words.size() > 1U)
        {
            const std::string &name = words[1];
            // --gamepad-prompts: treat scripted keys as controller input, so a
            // capture shows the Remastered controller prompts.
            if (options_.gamepad_prompts)
                gamepad_input_active_ = true;
            screens_key(name == "E"       ? screens::Key::inventory
                        : name == "Q"     ? screens::Key::drop
                        : name == "Enter" ? screens::Key::enter
                        : name == "Up"    ? screens::Key::up
                        : name == "Down"  ? screens::Key::down
                        : name == "Left"  ? screens::Key::left
                        : name == "Right" ? screens::Key::right
                        : name == "F"     ? screens::Key::split
                        : name == "T"     ? screens::Key::transfer
                        : name == "Tab"   ? screens::Key::switch_pane
                        : name == "PageDown" ? screens::Key::next_category
                        : name == "PageUp"   ? screens::Key::previous_category
                                          : screens::Key::escape);
            frame();
        }
        else if (verb == "wait" && words.size() > 1U)
        {
            const int frames = static_cast<int>(std::lround(std::atof(words[1].c_str()) * 60.0));
            for (int index = 0; index < frames; ++index)
                frame();
        }
    }
}

void draw_retail_screens()
{
    sync_screens();
    const bool modern = screens_modern_layout();
    const UiStyle style = modern ? UiStyle::modern : UiStyle::classic;
    const ScreenArt &art = modern ? modern_screen_art_ : classic_screen_art_;
    RetailFonts *fonts = modern ? modern_fonts_.get() : classic_fonts_.get();
    if (fonts == nullptr)
        return;
    const ScreenMetrics metrics{*fonts};
    const ScreenHost host{*this};
    screens_.held() = held_stack_;
    const screens::DrawList list = screens_.draw(host, metrics);
    // 1.6.3.8 lays out in its 1280x720 buffer, letterboxed into the window
    // exactly like its HUD (hud::to_window); 1.9.9.8 in window pixels.
    hud::Frame canvas;
    canvas.canvas_width = metrics_.output_width;
    canvas.canvas_height = metrics_.output_height;
    canvas.present = {0, 0, metrics_.output_width, metrics_.output_height};
    if (!modern)
    {
        const screens::IntRect present =
            screens::classic_buffer_destination(metrics_.output_width, metrics_.output_height);
        canvas.canvas_width = screens::classic_buffer_width;
        canvas.canvas_height = screens::classic_buffer_height;
        canvas.present = {present.x, present.y, present.width, present.height};
    }
    const float scale = hud::window_scale(canvas);
    if (options_.screen_backdrop.has_value())
    {
        // --screen-backdrop: a flat colour in place of the world, so a capture
        // can be compared pixel for pixel with a ScreensParityProbe frame.
        const auto &colour = *options_.screen_backdrop;
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, colour[0], colour[1], colour[2], 255U);
        SDL_RenderFillRect(renderer_, nullptr);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    }
    static constexpr std::array<SDL_FRect, 9> frame_pieces{{
        {0.0F, 0.0F, 2.0F, 2.0F}, // top left
        {3.0F, 0.0F, 2.0F, 2.0F}, // top right
        {0.0F, 3.0F, 2.0F, 2.0F}, // bottom left
        {3.0F, 3.0F, 2.0F, 2.0F}, // bottom right
        {2.0F, 0.0F, 1.0F, 2.0F}, // top
        {2.0F, 3.0F, 1.0F, 2.0F}, // bottom
        {0.0F, 2.0F, 2.0F, 1.0F}, // left
        {3.0F, 2.0F, 2.0F, 1.0F}, // right
        {2.0F, 2.0F, 1.0F, 1.0F}, // centre
    }};
    for (const screens::Draw &draw : list)
    {
        if (draw.kind == screens::Draw::Kind::hud_stats)
        {
            // InGameHUD.DrawPlayerStats and DrawDistanceStr: the HUD's own
            // meters and distance read-out, and nothing else of it.
            if (hud_style() != UiStyle::modern || !retail_hud_ready())
                continue;
            hud::State state = hud_state(false, false);
            state.show_oxygen = false;
            state.show_tray = false;
            state.show_item_text = false;
            state.show_block_name = false;
            state.show_crosshair = false;
            state.show_damage = false;
            state.show_scope = false;
            state.show_console = false;
            state.show_players = false;
            state.fade_in_seconds = 1.0F;
            state.day_banner_seconds = 9.0F;
            state.lava = 0.0F;
            state.underwater = false;
            state.dead = false;
            draw_retail_hud(state);
            continue;
        }
        if (draw.kind == screens::Draw::Kind::hud_distance)
            continue; // drawn with the meters above
        if (draw.kind == screens::Draw::Kind::text)
        {
            // The list holds XNA's premultiplied colours; RetailFonts takes a
            // straight colour and alpha.
            const auto straight = [](screens::Color color) {
                const auto channel = [&](std::uint8_t value) {
                    return color.a == 0U
                               ? Uint8{0U}
                               : static_cast<Uint8>(
                                     std::min(255, (static_cast<int>(value) * 255 + color.a / 2) / color.a));
                };
                return SDL_Color{channel(color.r), channel(color.g), channel(color.b), color.a};
            };
            const hud::Rect origin = hud::to_window(canvas, {draw.x, draw.y, 0.0F, 0.0F});
            const RetailFont font = ScreenMetrics::retail_font(draw.font);
            if (draw.outline != 0)
                fonts->draw_outlined(font, draw.text, origin.x, origin.y, straight(draw.color),
                                     straight(draw.outline_color), draw.outline, draw.scale * scale);
            else
                fonts->draw(font, draw.text, origin.x, origin.y, straight(draw.color), draw.scale * scale);
            continue;
        }
        const hud::Rect target = hud::to_window(canvas, {draw.x, draw.y, draw.width, draw.height});
        const SDL_FRect destination{target.x, target.y, target.width, target.height};
        if (draw.sprite == screens::Sprite::solid)
        {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            SDL_SetRenderDrawColor(renderer_, draw.color.r, draw.color.g, draw.color.b, draw.color.a);
            SDL_RenderFillRect(renderer_, &destination);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            continue;
        }
        if (draw.sprite == screens::Sprite::item_icon || draw.sprite == screens::Sprite::item_icon_large)
        {
            // Modding, visuals area: a mod item's own icon (app_mod_graphics_items.inc).
            if (draw.mod_item != 0U &&
                draw_mod_item_icon(draw.mod_item, destination, draw.color,
                                   draw.sprite == screens::Sprite::item_icon_large))
                continue;
            if (!draw_retail_item_icon(draw.item, style, destination, draw.color,
                                       draw.sprite == screens::Sprite::item_icon_large))
                draw_item_icon(InventoryStack{draw.item, 1U}, {target.x, target.y, target.width, target.height});
            continue;
        }
        const auto piece = static_cast<std::size_t>(draw.sprite);
        const auto first_piece = static_cast<std::size_t>(screens::Sprite::frame_top_left);
        const bool framed = piece >= first_piece && piece < first_piece + frame_pieces.size();
        SDL_Texture *texture = framed ? art.frame : art.textures[piece];
        if (texture == nullptr)
            continue;
        SDL_SetTextureColorMod(texture, draw.color.r, draw.color.g, draw.color.b);
        SDL_SetTextureAlphaMod(texture, draw.color.a);
        SDL_RenderTexture(renderer_, texture, framed ? &frame_pieces[piece - first_piece] : nullptr,
                          &destination);
    }
}
