// Member functions and state of Application::Impl (Modding, visuals area): the
// icons of mod items (openminerz/modding/gameplay.hpp ClientItemType::icon).
// Included at the end of app_mod_graphics.inc, inside the class body.
//
// A stack whose InventoryStack::mod_item names a mod item the session's
// registry knows is drawn with that item's own icon - an asset of the enabled
// mods, resolved through the asset stack - wherever the game draws an item
// icon (the trays of all three HUD styles, the inventory, crafting and crate
// screens, the stack on the pointer), and in 3D as the icon in solid form
// (openminerz/modding/icon_model.hpp) for a dropped pickup and in the
// first-person hand. An icon that is missing or does not load leaves the
// fallback's retail icon in place and is reported once.
//
// PRESENTATION ONLY. A retail stack has mod_item 0, and every call site tests
// that first: without a mod item nothing here runs, loads or allocates.

struct ModItemIcon final
{
    // What it was made from: the item's key and icon reference, and the asset
    // stack's generation (+1; 0 = no stack installed).
    std::string source;
    std::uint64_t generation{};
    // Premultiplied, like the retail atlases: 64 px and 128 px cells.
    SDL_Texture *small{};
    SDL_Texture *large{};
    std::shared_ptr<const modding::graphics::IconModel> model;
};
std::map<std::uint32_t, ModItemIcon> mod_item_icons_;
std::set<std::string, std::less<>> mod_item_icons_reported_;
// The world renderer has been given the 3D models this session.
bool mod_item_visuals_installed_{};

// The client's type of a mod item, or nullptr: not a mod item, or no client
// half of a server mod is running.
[[nodiscard]] const modding::gameplay::ClientItemType *mod_item_type(std::uint32_t mod_item) const
{
    if (mod_item == 0U || mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
        return nullptr;
    return mod_gameplay_->client_item(mod_item);
}

// The name the HUD shows for a stack: the mod item's own, or `fallback`.
[[nodiscard]] std::string mod_item_label(const InventoryStack &stack, std::string_view fallback) const
{
    const modding::gameplay::ClientItemType *type =
        stack.mod_item != 0U ? mod_item_type(stack.mod_item) : nullptr;
    return std::string{type != nullptr && !type->name.empty() ? std::string_view{type->name} : fallback};
}
[[nodiscard]] std::string mod_item_label(const PlayerInventory::Slot &slot, std::string_view fallback) const
{
    return slot.has_value() ? mod_item_label(*slot, fallback) : std::string{fallback};
}

// (Polish) The inventory, crafting and crate screens' words for a stack: a
// mod item's name and description (items.define), the retail item's text
// otherwise. On 1.6.3.8's screens (Classic profile: two fixed lines) a long
// description takes both lines, broken at a space; 1.9.9.8's wrap it. The
// views live as long as the session's item registry.
[[nodiscard]] ItemText mod_item_words(const InventoryStack &stack, InventoryProfile profile) const
{
    const modding::gameplay::ClientItemType *type =
        stack.mod_item != 0U ? mod_item_type(stack.mod_item) : nullptr;
    if (type == nullptr)
        return item_text(stack.item, profile);
    ItemText words;
    words.name = type->name.empty() ? item_text(stack.item, profile).name : std::string_view{type->name};
    words.description1 = type->description;
    constexpr std::size_t line = 40U;
    if (profile == InventoryProfile::classic && words.description1.size() > line)
        if (const std::size_t space = words.description1.rfind(' ', line); space != std::string_view::npos)
        {
            words.description2 = words.description1.substr(space + 1U);
            words.description1 = words.description1.substr(0U, space);
        }
    return words;
}

void mod_item_icons_clear()
{
    for (auto &[mod_item, icon] : mod_item_icons_)
    {
        (void)mod_item;
        SDL_DestroyTexture(icon.small);
        SDL_DestroyTexture(icon.large);
    }
    mod_item_icons_.clear();
}

// The loaded icon of a mod item, or nullptr (then the fallback's icon is
// drawn). Loaded on first use and again when the item's icon or the installed
// packs change; a failure is remembered for as long as they do not.
[[nodiscard]] const ModItemIcon *mod_item_icon(std::uint32_t mod_item)
{
    const modding::gameplay::ClientItemType *type = mod_item_type(mod_item);
    if (type == nullptr)
        return nullptr;
    const auto assets = modding::graphics::presentation_assets();
    const std::uint64_t generation = assets != nullptr ? assets->stack.generation() + 1U : 0U;
    std::string source = type->key + '\n' + type->icon;
    if (const auto found = mod_item_icons_.find(mod_item);
        found != mod_item_icons_.end() && found->second.source == source &&
        found->second.generation == generation)
        return found->second.small != nullptr ? &found->second : nullptr;
    ModItemIcon &icon = mod_item_icons_[mod_item];
    SDL_DestroyTexture(icon.small);
    SDL_DestroyTexture(icon.large);
    icon = {};
    icon.source = std::move(source);
    icon.generation = generation;

    std::string error;
    std::optional<modding::graphics::Image> image;
    static constexpr std::array<std::string_view, 3> extensions{".png", ".bmp", ".tga"};
    if (type->icon.empty())
        error = "names no icon";
    else if (assets == nullptr)
        error = "cannot be found: no mod assets are installed";
    else if (const auto resolved = assets->stack.resolve(type->icon, extensions))
        image = modding::graphics::load_image(resolved->path, &error, {1024, 8U << 20U});
    else
        error = "\"" + type->icon + "\" is in no enabled mod";
    if (image.has_value() && image->empty())
    {
        error = "is empty";
        image.reset();
    }
    if (!image.has_value())
    {
        if (mod_item_icons_reported_.insert(icon.source).second)
            std::cerr << "Mod item " << type->key << ": its icon " << error
                      << " (drawing its fallback's icon)\n";
        return nullptr;
    }
    // A wide or tall icon keeps its proportions, centred in a square.
    if (image->width != image->height)
    {
        const int side = std::max(image->width, image->height);
        modding::graphics::Image square;
        square.width = side;
        square.height = side;
        square.rgba.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U, 0U);
        modding::graphics::blit(square, *image, (side - image->width) / 2, (side - image->height) / 2);
        image = std::move(square);
    }
    icon.model =
        std::make_shared<const modding::graphics::IconModel>(modding::graphics::icon_model(*image));
    // The 2D cells: premultiplied first, so shrinking never darkens an edge.
    modding::graphics::Image premultiplied = *image;
    for (std::size_t texel = 0U; texel + 3U < premultiplied.rgba.size(); texel += 4U)
        for (std::size_t channel = 0U; channel < 3U; ++channel)
            premultiplied.rgba[texel + channel] = static_cast<std::uint8_t>(
                (premultiplied.rgba[texel + channel] * premultiplied.rgba[texel + 3U] + 127U) / 255U);
    for (const bool large : {false, true})
    {
        const int cell = large ? 128 : 64;
        SDL_Surface *surface =
            surface_from_image(modding::graphics::resample(premultiplied, cell, cell));
        SDL_Texture *texture =
            surface != nullptr ? SDL_CreateTextureFromSurface(renderer_, surface) : nullptr;
        SDL_DestroySurface(surface);
        if (texture == nullptr)
            continue;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        (large ? icon.large : icon.small) = texture;
    }
    if (icon.small == nullptr)
    {
        if (mod_item_icons_reported_.insert(icon.source).second)
            std::cerr << "Mod item " << type->key << ": its icon could not be made a texture"
                      << " (drawing its fallback's icon)\n";
        return nullptr;
    }
    return &icon;
}

// Draws a mod item's icon over `destination` like draw_retail_item_icon draws
// an atlas cell (the same premultiplied tint, the 128 px cell when `large`).
// False when it has none, and the caller draws the fallback's icon.
bool draw_mod_item_icon(std::uint32_t mod_item, const SDL_FRect &destination, screens::Color tint,
                        bool large)
{
    const ModItemIcon *icon = mod_item_icon(mod_item);
    if (icon == nullptr)
        return false;
    SDL_Texture *texture = large && icon->large != nullptr ? icon->large : icon->small;
    SDL_SetTextureColorMod(texture, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(texture, tint.a);
    SDL_RenderTexture(renderer_, texture, nullptr, &destination);
    return true;
}

// Before the world draws (app.cpp): the world renderer learns how mod items
// look in 3D - for pickups, and the one in the hand this frame. Without a
// running mod client this returns at its first test, and once a session ends
// the renderer is given back its unmodded state and the icons are freed.
void mod_graphics_items_before_world_draw()
{
    const bool running = mod_gameplay_ != nullptr && mod_gameplay_->client_running();
    if (!running)
    {
        if (!mod_item_visuals_installed_)
            return;
        mod_item_visuals_installed_ = false;
        if (world_preview_ != nullptr)
        {
            world_preview_->set_mod_item_models(nullptr);
            world_preview_->set_held_mod_item(nullptr);
        }
        mod_item_icons_clear();
        mod_items_clear(); // (Items)
        return;
    }
    if (world_preview_ == nullptr)
        return;
    // Restated every frame: a world renderer made since has none yet.
    // (Items) A look is the item's glTF model or this icon in solid form;
    // the hand gets the whole held item (app_mod_items.inc).
    mod_item_visuals_installed_ = true;
    mod_items_before_world_draw();
}
