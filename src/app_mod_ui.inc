// Included inside Application::Impl (src/app.cpp), after app_xbox_avatar.inc
// and before app_mod_runtime.inc, so the service is declared before (and so
// outlives) the script runtime that calls it.
//
// UI area: the modding API's interface service (openminerz/modding/ui.hpp) on
// the game. The service lays mod screens, HUD elements and notices out as draw
// lists; this file draws them with the game's own primitives (fill, stroke,
// menu_label, the item atlas), feeds them the pointer, keys and pad, and gives
// the game's own interface the hooks a theme restyles it through:
//
//   menu_ink() / rm_theme()   colours            mod_ui_menu_ink / mod_ui_rm_inks
//   menu_letter()             TrueType faces     mod_ui_theme_font
//   menu_label/width/...      the game's words   mod_ui_game_text
//   plate buttons, dialogs    nine-slice images  mod_ui_plate
//   refresh_screen_pointer()  the cursor         mod_ui_cursor
//   service_menu_sounds()     Click / Popup      mod_ui_menu_cue
//   the HUD                   hide / move        ModUiHudScope, mod_ui_move_hud
//
// Nothing here runs unless a mod used the UI: with no service, or a service
// with nothing loaded, every hook returns its input and the game draws exactly
// as it did before this file existed.

// ---- Backend: what the service asks of the game ----------------------------

class ModUiBackend final : public modding::ui::UiBackend
{
  public:
    explicit ModUiBackend(Impl &app) noexcept : app_{&app} {}

    [[nodiscard]] modding::ui::Style style() const override
    {
        return app_->mod_ui_style();
    }
    [[nodiscard]] std::uint32_t load_order(std::string_view mod) const override
    {
        const net::LoadedMod *found = app_->mod_ui_package(mod);
        return found != nullptr ? found->load_order : 0U;
    }
    // (Add-Ons) The name the Add-Ons screen shows ("Lighting Pack Settings").
    [[nodiscard]] std::string mod_title(std::string_view mod) const override
    {
        const modding::InstalledAddon *addon = modding::find_addon(app_->addon_scan_, mod);
        return addon != nullptr ? addon->info.name : std::string{};
    }
    [[nodiscard]] bool asset_exists(const modding::ui::AssetRef &asset) const override
    {
        const std::optional<std::filesystem::path> file = app_->mod_ui_file(asset.mod, asset.path);
        std::error_code ignored;
        return file.has_value() && std::filesystem::is_regular_file(*file, ignored);
    }
    [[nodiscard]] std::optional<std::string> read_mod_text(std::string_view mod, std::string_view path,
                                                           std::size_t limit) const override
    {
        const std::optional<std::filesystem::path> file = app_->mod_ui_file(mod, path);
        if (!file)
            return std::nullopt;
        return read_bounded(*file, limit);
    }
    [[nodiscard]] std::optional<std::string> read_local(std::string_view name) const override
    {
        return read_bounded(default_settings_path().parent_path() / std::string{name}, 1024U * 1024U);
    }
    void write_local(std::string_view name, std::string_view text) override
    {
        const std::filesystem::path file = default_settings_path().parent_path() / std::string{name};
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
        if (!write_file_atomically(file, std::span<const std::uint8_t>{
                                             reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}))
            std::cerr << "mod.ui could not save " << file.string() << '\n';
    }
    [[nodiscard]] bool item_exists(int item) const override
    {
        return item > 0 && item < static_cast<int>(InventoryItemId::item_count);
    }
    [[nodiscard]] std::string item_name(int item) const override
    {
        return item_exists(item) ? std::string{openminerz::item_name(static_cast<InventoryItemId>(item))}
                                 : std::string{};
    }
    [[nodiscard]] std::string canonical_key(std::string_view key) const override
    {
        if (key.empty() || key.size() > 32U)
            return {};
        const std::string name{key};
        const SDL_Keycode code = SDL_GetKeyFromName(name.c_str());
        if (code == SDLK_UNKNOWN)
            return {};
        const char *canonical = SDL_GetKeyName(code);
        return canonical != nullptr ? std::string{canonical} : std::string{};
    }
    [[nodiscard]] bool offline() const override
    {
        return !app_->online_gameplay_active();
    }
    [[nodiscard]] modding::Handle local_player() const override
    {
        // (Integration) The handle server scripts address this player by:
        // the channel's welcome, or Gameplay's admitted local player.
        const modding::Handle player = app_->mod_net_local_player();
        return player.valid() ? player : app_->mod_ui_local_player_;
    }
    void play(modding::ui::SoundKey sound) override
    {
        app_->mod_ui_play(sound);
    }

  private:
    [[nodiscard]] static std::optional<std::string> read_bounded(const std::filesystem::path &file,
                                                                 std::size_t limit)
    {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(file, error);
        if (error || size > limit)
            return std::nullopt;
        std::ifstream input{file, std::ios::binary};
        if (!input)
            return std::nullopt;
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    Impl *app_;
};

// The settings mods declared with mod.config.define (the script runtime's),
// for the Add-Ons screen's settings pages.
class ModUiConfig final : public modding::ui::ConfigSource
{
  public:
    explicit ModUiConfig(Impl &app) noexcept : app_{&app} {}
    [[nodiscard]] std::vector<modding::ConfigSetting> settings() const override
    {
        std::vector<modding::ConfigSetting> settings =
            app_->mod_runtime_ ? app_->mod_runtime_->config_settings() : std::vector<modding::ConfigSetting>{};
        // (Add-Ons) The runtime keeps settings of mods that ran before; a mod
        // turned off since has no settings page.
        std::erase_if(settings, [this](const modding::ConfigSetting &setting) {
            return !app_->mod_settings_shown(setting.mod_id);
        });
        return settings;
    }
    bool set(std::string_view mod, std::string_view key, const modding::Value &value, std::string &error) override
    {
        if (!app_->mod_runtime_)
        {
            error = "No mod scripts are running.";
            return false;
        }
        return app_->mod_runtime_->set_config(mod, key, value, error);
    }

  private:
    Impl *app_;
};

// The service's "omz.ui" messages over the Network area's mod channel
// (app_mod_net.inc). The channel objects are made and dropped per session, so
// this reads them at every call and never keeps a pointer.
class ModUiNetChannel final : public modding::ui::UiChannel
{
  public:
    explicit ModUiNetChannel(Impl &app) noexcept : app_{&app} {}
    void send_to_server(const modding::Value &message) override
    {
        if (app_->mod_net_client_ == nullptr || !app_->mod_net_client_->enabled())
            return;
        try
        {
            (void)app_->mod_net_client_->send("", modding::ui::ui_channel_name, message);
        }
        catch (const modding::ScriptError &error)
        {
            std::cerr << "mod.ui channel: " << error.what() << '\n';
        }
    }
    void send_to_player(modding::Handle player, const modding::Value &message) override
    {
        if (app_->mod_net_server_ == nullptr || !app_->mod_net_server_->enabled())
            return;
        try
        {
            (void)app_->mod_net_server_->send_to(player, "", modding::ui::ui_channel_name, message);
        }
        catch (const modding::ScriptError &error)
        {
            std::cerr << "mod.ui channel: " << error.what() << '\n';
        }
    }
    [[nodiscard]] std::vector<modding::Handle> players() const override
    {
        return app_->mod_net_server_ != nullptr && app_->mod_net_server_->enabled()
                   ? app_->mod_net_server_->ready_players()
                   : std::vector<modding::Handle>{};
    }

  private:
    Impl *app_;
};

// Widths of mod text, set the way it is drawn (menu_width).
class ModUiMeasure final : public modding::ui::TextMeasure
{
  public:
    explicit ModUiMeasure(Impl &app) noexcept : app_{&app} {}
    [[nodiscard]] float width(modding::ui::Face face, float cap, std::string_view text) const override
    {
        return app_->menu_width(text, cap, face == modding::ui::Face::display ? MenuFace::display : MenuFace::text);
    }

  private:
    Impl *app_;
};

// ---- State --------------------------------------------------------------------

struct ModUiFont final
{
    std::unique_ptr<FontRenderer> renderer;
    float cap_ratio{1.0F}; // capital height per pixel size
};
struct ModUiSound final
{
    SDL_AudioSpec spec{};
    std::vector<std::uint8_t> samples;
    SDL_AudioStream *stream{};
};

std::unique_ptr<ModUiBackend> mod_ui_backend_;
std::unique_ptr<ModUiConfig> mod_ui_config_;
std::unique_ptr<ModUiNetChannel> mod_ui_net_channel_;
// The mod channel objects the "omz.ui" handlers were put on (mod_net_generation_).
std::uint64_t mod_ui_channel_generation_{};
std::unique_ptr<modding::ui::UiService> mod_ui_;
// The local player's handle before any session names one (mod_net_local_player
// is asked first).
modding::Handle mod_ui_local_player_{modding::HandleKind::player, 1U};
// Mod text is the mod's own words: a theme's replacements apply only to the
// game's.
bool mod_ui_drawing_{};
std::map<std::string, SDL_Texture *> mod_ui_textures_;
std::uint64_t mod_ui_texture_pixels_{};
std::map<std::string, ModUiFont> mod_ui_fonts_;
std::map<std::string, SDL_Cursor *> mod_ui_cursors_;
std::map<std::string, ModUiSound> mod_ui_sounds_;
std::vector<SDL_Rect> mod_ui_clips_;
std::uint64_t mod_ui_menu_revision_{};
ui::MenuScreen mod_ui_menu_screen_{ui::MenuScreen::main};
bool mod_ui_menu_stale_{}; // (Add-Ons) the mods that are on changed
std::array<std::vector<std::string>, modding::ui::menu_slot_count> mod_ui_menu_text_;
bool mod_ui_had_screen_{};
bool mod_ui_text_input_{};
float mod_ui_font_age_{};
// --diagnostic driver (OPENMINERZ_DIAGNOSTIC_MOD_UI): its own registry and a
// host with no VM, used only to take captures before the runtime is wired.
std::unique_ptr<modding::ApiRegistry> mod_ui_diagnostic_registry_;
std::unique_ptr<modding::ScriptHost> mod_ui_diagnostic_host_;
bool mod_ui_diagnostic_done_{};
unsigned mod_ui_diagnostic_frames_{};
bool mod_ui_diagnostic_answer_{};

// ---- Registration ---------------------------------------------------------------

// The UI area's entry in register_mod_apis (app_mod_runtime.inc).
void register_mod_ui_api(modding::ApiRegistry &registry, modding::ScriptHost &host)
{
    modding::ui::UiService &service = mod_ui_service();
    service.register_api(registry, host);
}

modding::ui::UiService &mod_ui_service()
{
    if (!mod_ui_)
    {
        mod_ui_backend_ = std::make_unique<ModUiBackend>(*this);
        mod_ui_config_ = std::make_unique<ModUiConfig>(*this);
        mod_ui_ = std::make_unique<modding::ui::UiService>(*mod_ui_backend_);
        mod_ui_->set_config(mod_ui_config_.get());
        // The player's language: OPENMINERZ_LANGUAGE, else the system's first.
        if (const char *language = SDL_getenv("OPENMINERZ_LANGUAGE"); language != nullptr && *language != '\0')
            mod_ui_->set_language(language);
        else
        {
            int count{};
            SDL_Locale **locales = SDL_GetPreferredLocales(&count);
            if (locales != nullptr && count > 0 && locales[0] != nullptr && locales[0]->language != nullptr)
                mod_ui_->set_language(locales[0]->country != nullptr
                                          ? std::format("{}-{}", locales[0]->language, locales[0]->country)
                                          : std::string{locales[0]->language});
            SDL_free(locales);
        }
    }
    return *mod_ui_;
}

// Before the renderer goes (~Impl): every texture, font, cursor and stream.
void release_mod_ui()
{
    for (auto &[path, texture] : mod_ui_textures_)
        SDL_DestroyTexture(texture);
    mod_ui_textures_.clear();
    mod_ui_texture_pixels_ = 0U;
    mod_ui_fonts_.clear();
    for (auto &[path, cursor] : mod_ui_cursors_)
    {
        if (cursor == screens_pointer_cursor_)
        {
            SDL_SetCursor(SDL_GetDefaultCursor());
            screens_pointer_cursor_ = nullptr;
        }
        SDL_DestroyCursor(cursor);
    }
    mod_ui_cursors_.clear();
    for (auto &[path, sound] : mod_ui_sounds_)
        SDL_DestroyAudioStream(sound.stream);
    mod_ui_sounds_.clear();
}

// ---- Small conversions --------------------------------------------------------------

[[nodiscard]] static SDL_Color mod_ui_sdl(modding::ui::Color color) noexcept
{
    return {color.r, color.g, color.b, color.a};
}
[[nodiscard]] static modding::ui::Color mod_ui_color(SDL_Color color) noexcept
{
    return {color.r, color.g, color.b, color.a};
}

[[nodiscard]] modding::ui::Style mod_ui_style() const noexcept
{
    switch (interface_style())
    {
    case UiStyle::classic:
        return modding::ui::Style::classic;
    case UiStyle::modern:
        return modding::ui::Style::modern;
    default:
        return modding::ui::Style::remastered;
    }
}

// Whether a theme is restyling the game's own interface at all.
[[nodiscard]] bool mod_ui_themed() const noexcept
{
    return mod_ui_ != nullptr && !mod_ui_->theme().empty();
}

// The package a mod id names: the host's downloaded copy in a joined session,
// else the local add-on.
[[nodiscard]] const net::LoadedMod *mod_ui_package(std::string_view mod) const noexcept
{
    if (mod.empty())
        return nullptr;
    if (mod_join_session_mods_)
        for (const net::LoadedMod &loaded : *mod_join_session_mods_)
            if (loaded.descriptor.id == mod)
                return &loaded;
    for (const net::LoadedMod &loaded : mod_catalog_.mods)
        if (loaded.descriptor.id == mod)
            return &loaded;
    // (Add-Ons) Joined: a server mod that is off here but the host runs.
    if (mod_session_ == ModSessionKind::joined)
        for (const modding::InstalledAddon &addon : addon_scan_.addons)
            if (addon.usable() && addon.id() == mod && addon.mod.descriptor.scope != net::ModScope::client)
                return &addon.mod;
    return nullptr;
}

// A file inside a mod; the path was normalised by the service (never "..").
[[nodiscard]] std::optional<std::filesystem::path> mod_ui_file(std::string_view mod, std::string_view path) const
{
    const std::optional<std::string> normalized = modding::ui::normalize_asset_path(path);
    if (!normalized)
        return std::nullopt;
    if (const char *root = SDL_getenv("OPENMINERZ_DIAGNOSTIC_MOD_UI_ROOT");
        root != nullptr && *root != '\0' && mod == "uitest")
        return std::filesystem::path{root} / std::filesystem::path{*normalized};
    const net::LoadedMod *package = mod_ui_package(mod);
    if (package == nullptr)
        return std::nullopt;
    return package->root / std::filesystem::path{*normalized};
}

// ---- Theme hooks into the game's own interface --------------------------------------

// MenuInk with a theme's colours (app_menus.inc: menu_ink).
[[nodiscard]] MenuInk mod_ui_menu_ink(MenuInk ink) const noexcept
{
    if (!mod_ui_themed())
        return ink;
    using modding::ui::ColorKey;
    const modding::ui::ThemeStack &theme = mod_ui_->theme();
    const modding::ui::Style style = mod_ui_style();
    const auto apply = [&](SDL_Color &slot, ColorKey key) {
        if (const std::optional<modding::ui::Color> color = theme.color(key, style))
            slot = mod_ui_sdl(*color);
    };
    apply(ink.row, ColorKey::menu_row);
    apply(ink.focus, ColorKey::menu_focus);
    apply(ink.heading, ColorKey::menu_heading);
    apply(ink.text, ColorKey::menu_text);
    apply(ink.quiet, ColorKey::menu_quiet);
    apply(ink.disabled, ColorKey::menu_disabled);
    apply(ink.accent, ColorKey::menu_accent);
    apply(ink.warning, ColorKey::menu_warning);
    return ink;
}

// RmTheme with a theme's HUD colours (app_remastered.inc: rm_theme).
template <typename Theme> void mod_ui_rm_inks(Theme &theme) const noexcept
{
    if (!mod_ui_themed())
        return;
    using modding::ui::ColorKey;
    const modding::ui::ThemeStack &stack = mod_ui_->theme();
    const modding::ui::Style style = mod_ui_style();
    const auto apply = [&](SDL_Color &slot, ColorKey key) {
        if (const std::optional<modding::ui::Color> color = stack.color(key, style))
            slot = mod_ui_sdl(*color);
    };
    apply(theme.plate, ColorKey::hud_plate);
    apply(theme.panel, ColorKey::hud_panel);
    apply(theme.well, ColorKey::hud_well);
    apply(theme.slot, ColorKey::hud_slot);
    apply(theme.slot_full, ColorKey::hud_slot_full);
    apply(theme.band, ColorKey::hud_band);
    apply(theme.line, ColorKey::hud_line);
    apply(theme.health, ColorKey::hud_health);
    apply(theme.stamina, ColorKey::hud_stamina);
    apply(theme.oxygen, ColorKey::hud_oxygen);
    apply(theme.good, ColorKey::hud_good);
    apply(theme.caution, ColorKey::hud_caution);
}

// The game's words with a theme's replacements (menu_label and friends). Mod
// widgets' own text is never replaced.
[[nodiscard]] std::string_view mod_ui_game_text(std::string_view value) const noexcept
{
    if (mod_ui_drawing_ || mod_ui_ == nullptr || !mod_ui_->theme().has_strings())
        return value;
    const std::string *replaced = mod_ui_->theme().text(value, mod_ui_style());
    return replaced != nullptr ? std::string_view{*replaced} : value;
}

// A theme's TrueType face for one of the menus' families, loaded on first use
// (FreeType, as the built-in UI font). Sets the letter's size; false keeps
// the game's lettering.
bool mod_ui_theme_font(MenuFace face, MenuLetter &letter)
{
    if (!mod_ui_themed())
        return false;
    const modding::ui::AssetRef *asset = mod_ui_->theme().font(
        face == MenuFace::display ? modding::ui::Face::display : modding::ui::Face::text, mod_ui_style());
    if (asset == nullptr)
        return false;
    const std::optional<std::filesystem::path> file = mod_ui_file(asset->mod, asset->path);
    if (!file)
        return false;
    const std::string key = file->string();
    auto found = mod_ui_fonts_.find(key);
    if (found == mod_ui_fonts_.end())
    {
        ModUiFont loaded;
        std::error_code error;
        const std::uintmax_t bytes = std::filesystem::file_size(*file, error);
        try
        {
            if (error || bytes > 16U * 1024U * 1024U || mod_ui_fonts_.size() >= 8U)
                throw std::runtime_error{"missing, larger than 16 MiB, or more than 8 faces"};
            loaded.renderer = std::make_unique<FontRenderer>(renderer_, *file);
            loaded.cap_ratio = 100.0F / static_cast<float>(std::max(1, loaded.renderer->pixel_size_for_cap_height(100)));
        }
        catch (const std::exception &error)
        {
            std::cerr << "mod.ui font " << key << ": " << error.what() << '\n';
            loaded.renderer.reset();
        }
        found = mod_ui_fonts_.emplace(key, std::move(loaded)).first;
    }
    if (!found->second.renderer)
        return false;
    letter.ttf = found->second.renderer.get();
    letter.fonts = nullptr;
    letter.pixels = std::clamp(static_cast<int>(std::lround(letter.cap / found->second.cap_ratio)), 8, 256);
    return true;
}

// The size a PNG or BMP says it is, read from its header before anything is
// decoded (a mod can come from a host: a tiny file must not expand into
// gigabytes). Nothing for anything else.
[[nodiscard]] static std::optional<std::pair<std::uint32_t, std::uint32_t>>
mod_ui_image_size(const std::filesystem::path &file)
{
    std::ifstream input{file, std::ios::binary};
    std::array<unsigned char, 26> header{};
    if (!input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size())))
        return std::nullopt;
    const auto big = [&](std::size_t at) {
        return (std::uint32_t{header[at]} << 24U) | (std::uint32_t{header[at + 1U]} << 16U) |
               (std::uint32_t{header[at + 2U]} << 8U) | std::uint32_t{header[at + 3U]};
    };
    const auto little = [&](std::size_t at) {
        return std::uint32_t{header[at]} | (std::uint32_t{header[at + 1U]} << 8U) |
               (std::uint32_t{header[at + 2U]} << 16U) | (std::uint32_t{header[at + 3U]} << 24U);
    };
    if (header[0] == 0x89U && header[1] == 'P' && header[2] == 'N' && header[3] == 'G')
        return std::pair{big(16U), big(20U)};
    if (header[0] == 'B' && header[1] == 'M')
    {
        // A negative height is a top-down bitmap.
        const auto height = static_cast<std::int32_t>(little(22U));
        return std::pair{little(18U), static_cast<std::uint32_t>(height < 0 ? -static_cast<std::int64_t>(height) : height)};
    }
    return std::nullopt;
}

// A mod image as a texture (PNG or BMP), loaded once: at most 2048 pixels a
// side and 32 million pixels across every mod image.
SDL_Texture *mod_ui_texture(const modding::ui::AssetRef &asset)
{
    const std::optional<std::filesystem::path> file = mod_ui_file(asset.mod, asset.path);
    if (!file)
        return nullptr;
    const std::string key = file->string();
    if (const auto found = mod_ui_textures_.find(key); found != mod_ui_textures_.end())
        return found->second;
    SDL_Texture *texture = nullptr;
    const auto size = mod_ui_image_size(*file);
    const std::uint64_t pixels = size ? std::uint64_t{size->first} * size->second : 0U;
    if (mod_ui_textures_.size() < 256U && size && size->first > 0U && size->second > 0U && size->first <= 2048U &&
        size->second <= 2048U && mod_ui_texture_pixels_ + pixels <= 32U * 1024U * 1024U)
        if (SDL_Surface *surface = SDL_LoadSurface(key.c_str()))
        {
            if (static_cast<std::uint32_t>(surface->w) == size->first)
                texture = SDL_CreateTextureFromSurface(renderer_, surface);
            SDL_DestroySurface(surface);
            if (texture != nullptr)
            {
                mod_ui_texture_pixels_ += pixels;
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            }
        }
    if (texture == nullptr)
        std::cerr << "mod.ui image " << key << " could not be loaded\n";
    mod_ui_textures_.emplace(key, texture);
    return texture;
}

// A theme's nine-slice image in place of one of the game's flat plates.
// False: the caller draws its own.
bool mod_ui_plate(Rect rect, modding::ui::ImageKey key)
{
    if (!mod_ui_themed() || rect.width <= 0.0F || rect.height <= 0.0F)
        return false;
    const modding::ui::NineSlice *image = mod_ui_->theme().image(key, mod_ui_style());
    if (image == nullptr && key == modding::ui::ImageKey::button_focus)
        image = mod_ui_->theme().image(modding::ui::ImageKey::button, mod_ui_style());
    if (image == nullptr)
        return false;
    SDL_Texture *texture = mod_ui_texture(image->image);
    if (texture == nullptr)
        return false;
    mod_ui_nine(texture, rect, image->slice, menu_unit() / 1.5F, {255U, 255U, 255U, 255U});
    if (key == modding::ui::ImageKey::button_focus && mod_ui_->theme().image(key, mod_ui_style()) == nullptr)
        menu_focus_band(rect); // one image for both: the band marks focus
    return true;
}

void mod_ui_nine(SDL_Texture *texture, Rect rect, modding::ui::Insets slice, float scale, SDL_Color tint)
{
    SDL_SetTextureColorMod(texture, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(texture, tint.a);
    const SDL_FRect target{rect.x, rect.y, rect.width, rect.height};
    if (slice.left + slice.right + slice.top + slice.bottom <= 0.0F)
    {
        SDL_RenderTexture(renderer_, texture, nullptr, &target);
        return;
    }
    // Corners keep their size times the interface scale, never more than half
    // of the rectangle.
    float width{};
    float height{};
    SDL_GetTextureSize(texture, &width, &height);
    const float fit = std::min({1.0F, rect.width / std::max(1.0F, (slice.left + slice.right) * scale),
                                rect.height / std::max(1.0F, (slice.top + slice.bottom) * scale)});
    SDL_RenderTexture9Grid(renderer_, texture, nullptr, std::min(slice.left, width), std::min(slice.right, width),
                           std::min(slice.top, height), std::min(slice.bottom, height), std::max(0.01F, scale * fit),
                           &target);
}

// The pointer a theme gives the game, while the game would show the system's.
SDL_Cursor *mod_ui_cursor()
{
    if (!mod_ui_themed())
        return nullptr;
    const modding::ui::CursorSpec *spec = mod_ui_->theme().cursor(mod_ui_style());
    if (spec == nullptr)
        return nullptr;
    const std::optional<std::filesystem::path> file = mod_ui_file(spec->image.mod, spec->image.path);
    if (!file)
        return nullptr;
    const std::string key = std::format("{}#{},{}", file->string(), spec->hot_x, spec->hot_y);
    if (const auto found = mod_ui_cursors_.find(key); found != mod_ui_cursors_.end())
        return found->second;
    SDL_Cursor *cursor = nullptr;
    const auto size = mod_ui_image_size(*file);
    if (mod_ui_cursors_.size() < 16U && size && size->first <= 256U && size->second <= 256U)
        if (SDL_Surface *surface = SDL_LoadSurface(file->string().c_str()))
        {
            if (surface->w <= 256 && surface->h <= 256)
                cursor = SDL_CreateColorCursor(surface, std::min(spec->hot_x, surface->w - 1),
                                               std::min(spec->hot_y, surface->h - 1));
            SDL_DestroySurface(surface);
        }
    mod_ui_cursors_.emplace(key, cursor);
    return cursor;
}

// The menus' cues, or what a theme plays instead (service_menu_sounds).
void mod_ui_menu_cue(std::string_view cue)
{
    const modding::ui::SoundKey key = cue == "Popup" ? modding::ui::SoundKey::popup
                                      : cue == "Error" ? modding::ui::SoundKey::error
                                                       : modding::ui::SoundKey::click;
    if (!mod_ui_themed() || !mod_ui_play_themed(key))
        sound_bank_->play_cue(cue);
}

void mod_ui_play(modding::ui::SoundKey sound)
{
    if (options_.screenshot_path.has_value() || sound_bank_ == nullptr)
        return;
    if (mod_ui_themed() && mod_ui_play_themed(sound))
        return;
    sound_bank_->play_cue(sound == modding::ui::SoundKey::popup   ? "Popup"
                          : sound == modding::ui::SoundKey::error ? "Error"
                                                                  : "Click");
}

// A theme's sound for a cue: another of the game's cues, or a WAV of the mod
// played through SDL at the effects volume. False when the theme has none.
bool mod_ui_play_themed(modding::ui::SoundKey sound)
{
    const modding::ui::SoundSpec *spec = mod_ui_->theme().sound(sound, mod_ui_style());
    if (spec == nullptr)
        return false;
    if (!spec->cue.empty())
    {
        sound_bank_->play_cue(spec->cue);
        return true;
    }
    const std::optional<std::filesystem::path> file = mod_ui_file(spec->file.mod, spec->file.path);
    if (!file)
        return false;
    const std::string key = file->string();
    auto found = mod_ui_sounds_.find(key);
    if (found == mod_ui_sounds_.end())
    {
        ModUiSound loaded;
        std::error_code error;
        const std::uintmax_t bytes = std::filesystem::file_size(*file, error);
        if (!error && bytes <= 4U * 1024U * 1024U && mod_ui_sounds_.size() < 32U &&
            (SDL_WasInit(SDL_INIT_AUDIO) != 0U || SDL_InitSubSystem(SDL_INIT_AUDIO)))
        {
            Uint8 *samples = nullptr;
            Uint32 length = 0U;
            if (SDL_LoadWAV(key.c_str(), &loaded.spec, &samples, &length))
            {
                if (length <= 4U * 1024U * 1024U)
                    loaded.samples.assign(samples, samples + length);
                SDL_free(samples);
            }
            if (!loaded.samples.empty())
                loaded.stream =
                    SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &loaded.spec, nullptr, nullptr);
        }
        found = mod_ui_sounds_.emplace(key, std::move(loaded)).first;
    }
    ModUiSound &clip = found->second;
    if (clip.stream == nullptr)
        return false;
    const float level = settings_.master_volume * settings_.effects_volume;
    SDL_SetAudioStreamGain(clip.stream, std::clamp(level, 0.0F, 1.0F));
    SDL_ClearAudioStream(clip.stream);
    SDL_PutAudioStreamData(clip.stream, clip.samples.data(), static_cast<int>(clip.samples.size()));
    SDL_ResumeAudioStreamDevice(clip.stream);
    return true;
}

// ---- The HUD's own elements: hidden for this frame, moved in Remastered -------

// While it lives, every HUD element a mod hid is switched off in settings_,
// the switches the HUD settings page flips; the player's own values come back
// when it ends. It only exists while the first-person HUD is drawn (never
// while the settings screen is shown), and nothing saves settings meanwhile.
class ModUiHudScope final
{
  public:
    ModUiHudScope(Impl &app, bool active) : app_{app}
    {
        if (!active || app.mod_ui_ == nullptr || !app.mod_ui_->any_hud_hidden())
            return;
        applied_ = true;
        Settings &settings = app.settings_;
        saved_vitals_ = settings.vitals_style;
        for (std::size_t index = 0U; index < switches_.size(); ++index)
            saved_[index] = settings.*switches_[index].field;
        using modding::ui::HudElement;
        for (const Switch &entry : switches_)
            if (app.mod_ui_->hud_hidden(entry.element))
                settings.*entry.field = false;
        if (app.mod_ui_->hud_hidden(HudElement::vitals))
            settings.vitals_style = VitalsStyle::none;
    }
    ~ModUiHudScope()
    {
        if (!applied_)
            return;
        Settings &settings = app_.settings_;
        settings.vitals_style = saved_vitals_;
        for (std::size_t index = 0U; index < switches_.size(); ++index)
            settings.*switches_[index].field = saved_[index];
    }
    ModUiHudScope(const ModUiHudScope &) = delete;
    ModUiHudScope &operator=(const ModUiHudScope &) = delete;

  private:
    struct Switch final
    {
        modding::ui::HudElement element;
        bool Settings::*field;
    };
    static constexpr std::array<Switch, 16> switches_{{
        {modding::ui::HudElement::crosshair, &Settings::hud_crosshair},
        {modding::ui::HudElement::hotbar, &Settings::hud_hotbar},
        {modding::ui::HudElement::item_name, &Settings::hud_item_name},
        {modding::ui::HudElement::block_name, &Settings::hud_target},
        {modding::ui::HudElement::compass, &Settings::hud_compass},
        {modding::ui::HudElement::clock, &Settings::hud_clock},
        {modding::ui::HudElement::distance, &Settings::hud_distance},
        {modding::ui::HudElement::oxygen, &Settings::hud_oxygen},
        {modding::ui::HudElement::chat, &Settings::hud_chat},
        {modding::ui::HudElement::player_list, &Settings::hud_roster},
        {modding::ui::HudElement::notifications, &Settings::hud_notifications},
        {modding::ui::HudElement::damage, &Settings::hud_damage},
        {modding::ui::HudElement::scope, &Settings::hud_scope},
        {modding::ui::HudElement::death, &Settings::hud_death},
        {modding::ui::HudElement::server, &Settings::hud_server},
        {modding::ui::HudElement::coordinates, &Settings::hud_coordinates},
    }};
    Impl &app_;
    bool applied_{};
    VitalsStyle saved_vitals_{};
    std::array<bool, switches_.size()> saved_{};
};

// The Remastered HUD's rectangles moved by ui.move_hud (app_remastered.inc).
void mod_ui_move_hud(remastered::HudLayout &hud) const noexcept
{
    if (mod_ui_ == nullptr || !mod_ui_->any_hud_moved())
        return;
    using modding::ui::HudElement;
    const auto shift = [&](HudElement element, remastered::Rect &rect) {
        const auto [dx, dy] = mod_ui_->hud_offset(element);
        if (!rect.empty())
        {
            rect.x += dx;
            rect.y += dy;
        }
    };
    shift(HudElement::hotbar, hud.tray);
    for (remastered::Rect &slot : hud.slots)
        shift(HudElement::hotbar, slot);
    shift(HudElement::hotbar, hud.other_tray);
    for (remastered::Rect &slot : hud.other_slots)
        shift(HudElement::hotbar, slot);
    shift(HudElement::vitals, hud.health);
    shift(HudElement::vitals, hud.stamina);
    shift(HudElement::item_name, hud.item_line);
    shift(HudElement::block_name, hud.target_line);
    shift(HudElement::compass, hud.compass);
    shift(HudElement::distance, hud.distance);
    shift(HudElement::oxygen, hud.oxygen);
    shift(HudElement::server, hud.server);
    shift(HudElement::coordinates, hud.coordinates);
    shift(HudElement::clock, hud.clock);
    shift(HudElement::notifications, hud.notice);
    shift(HudElement::chat, hud.chat);
    shift(HudElement::player_list, hud.roster);
}

// ---- Drawing the service's lists ----------------------------------------------------

[[nodiscard]] modding::ui::FrameContext mod_ui_frame()
{
    modding::ui::FrameContext frame;
    frame.width = std::max(metrics_.output_width, 1);
    frame.height = std::max(metrics_.output_height, 1);
    frame.ui_scale = settings_.ui_scale;
    frame.safe_area = settings_.safe_area;
    frame.style = mod_ui_style();
    // rm_cap(): Modern's condensed face needs more height for the same weight,
    // and Larger Text adds an eighth.
    frame.text_factor = (menu_generation() == RetailGeneration::modern ? 14.0F / 11.0F : 1.0F) *
                        (settings_.larger_text ? 1.12F : 1.0F);
    frame.time = static_cast<double>(elapsed_);
    frame.reduced_motion = settings_.reduced_motion;
    frame.gamepad = gamepad_input_active_;
    using modding::ui::ColorKey;
    const MenuInk ink = menu_ink();
    const RmTheme rm = rm_theme();
    const auto set = [&](ColorKey key, SDL_Color color) {
        frame.palette[static_cast<std::size_t>(key)] = mod_ui_color(color);
    };
    set(ColorKey::menu_row, ink.row);
    set(ColorKey::menu_focus, ink.focus);
    set(ColorKey::menu_heading, ink.heading);
    set(ColorKey::menu_text, ink.text);
    set(ColorKey::menu_quiet, ink.quiet);
    set(ColorKey::menu_disabled, ink.disabled);
    set(ColorKey::menu_accent, ink.accent);
    set(ColorKey::menu_warning, ink.warning);
    set(ColorKey::hud_plate, rm.plate);
    set(ColorKey::hud_panel, rm.panel);
    set(ColorKey::hud_well, rm.well);
    set(ColorKey::hud_slot, rm.slot);
    set(ColorKey::hud_slot_full, rm.slot_full);
    set(ColorKey::hud_band, rm.band);
    set(ColorKey::hud_line, rm.line);
    set(ColorKey::hud_health, rm.health);
    set(ColorKey::hud_stamina, rm.stamina);
    set(ColorKey::hud_oxygen, rm.oxygen);
    set(ColorKey::hud_good, rm.good);
    set(ColorKey::hud_caution, rm.caution);
    if (mod_ui_themed())
        for (std::size_t index = 0U; index < modding::ui::image_key_count; ++index)
            frame.images[index] = mod_ui_->theme().image(static_cast<modding::ui::ImageKey>(index), frame.style);
    return frame;
}

// One of the game's own sprites by name ("hud/HudGrid", "screen/BlockUIBack").
SDL_Texture *mod_ui_game_sprite(std::string_view name)
{
    const bool modern = hud_style() == UiStyle::modern;
    if (name.starts_with("hud/"))
    {
        const std::string_view sprite = name.substr(4U);
        const HudArt &art = modern ? modern_hud_art_ : classic_hud_art_;
        for (std::size_t index = 0U; index < art.textures.size(); ++index)
            if (hud::sprite_sheet_name(static_cast<hud::Sprite>(index)) == sprite)
                return art.textures[index];
    }
    if (name.starts_with("screen/"))
    {
        const std::string_view sprite = name.substr(7U);
        const ScreenArt &art = modern ? modern_screen_art_ : classic_screen_art_;
        for (std::size_t index = 0U; index < art.textures.size(); ++index)
            if (screens::sprite_sheet_name(static_cast<screens::Sprite>(index)) == sprite)
                return art.textures[index];
    }
    return nullptr;
}

void mod_ui_render(const modding::ui::DrawList &draws)
{
    mod_ui_drawing_ = !draws.empty();
    if (draws.empty())
        return;
    for (const modding::ui::DrawCmd &draw : draws)
    {
        const Rect rect{draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height};
        switch (draw.kind)
        {
        case modding::ui::DrawCmd::Kind::fill:
            fill(rect, mod_ui_sdl(draw.color));
            break;
        case modding::ui::DrawCmd::Kind::frame:
            stroke(rect, mod_ui_sdl(draw.color), std::min({draw.thickness, rect.width * 0.5F, rect.height * 0.5F}));
            break;
        case modding::ui::DrawCmd::Kind::text:
            menu_label(draw.text, rect.x, rect.y, draw.cap, mod_ui_sdl(draw.color), TextAlign::left,
                       draw.face == modding::ui::Face::display ? MenuFace::display : MenuFace::text, draw.outline);
            break;
        case modding::ui::DrawCmd::Kind::image: {
            SDL_Texture *texture = draw.sprite.empty() ? mod_ui_texture(draw.image) : mod_ui_game_sprite(draw.sprite);
            if (texture == nullptr || rect.width <= 0.0F || rect.height <= 0.0F)
                break;
            if (draw.nine)
                mod_ui_nine(texture, rect, draw.slice, draw.slice_scale, mod_ui_sdl(draw.color));
            else
            {
                SDL_SetTextureColorMod(texture, draw.color.r, draw.color.g, draw.color.b);
                SDL_SetTextureAlphaMod(texture, draw.color.a);
                const SDL_FRect target{rect.x, rect.y, rect.width, rect.height};
                SDL_RenderTexture(renderer_, texture, nullptr, &target);
            }
            break;
        }
        case modding::ui::DrawCmd::Kind::item: {
            if (draw.item <= 0 || draw.item >= static_cast<int>(InventoryItemId::item_count))
                break;
            const auto item = static_cast<InventoryItemId>(draw.item);
            const auto channel = [](std::uint8_t value) { return value; };
            const screens::Color tint{channel(draw.color.a), channel(draw.color.a), channel(draw.color.a), draw.color.a};
            if (!draw_retail_item_icon(item, hud_style(), SDL_FRect{rect.x, rect.y, rect.width, rect.height}, tint,
                                       rect.width / 64.0F > 1.1F))
            {
                InventoryStack stack;
                stack.item = item;
                stack.count = static_cast<std::uint16_t>(std::clamp(draw.count, 1, 65535));
                draw_item_icon(stack, rect);
            }
            if (draw.count > 1)
            {
                const float cap = std::max(draw.cap, 8.0F);
                menu_label(std::to_string(draw.count), rect.x + rect.width, rect.y + rect.height - cap * 1.1F, cap,
                           with_alpha({255U, 255U, 255U, 255U}, static_cast<float>(draw.color.a) / 255.0F),
                           TextAlign::right, MenuFace::display, true);
            }
            break;
        }
        case modding::ui::DrawCmd::Kind::clip_push: {
            SDL_Rect clip{static_cast<int>(std::floor(rect.x)), static_cast<int>(std::floor(rect.y)),
                          static_cast<int>(std::ceil(rect.width)), static_cast<int>(std::ceil(rect.height))};
            if (!mod_ui_clips_.empty())
            {
                SDL_Rect inside{};
                if (!SDL_GetRectIntersection(&clip, &mod_ui_clips_.back(), &inside))
                    inside = {clip.x, clip.y, 0, 0};
                clip = inside;
            }
            mod_ui_clips_.push_back(clip);
            SDL_SetRenderClipRect(renderer_, &clip);
            break;
        }
        case modding::ui::DrawCmd::Kind::clip_pop:
            if (!mod_ui_clips_.empty())
                mod_ui_clips_.pop_back();
            SDL_SetRenderClipRect(renderer_, mod_ui_clips_.empty() ? nullptr : &mod_ui_clips_.back());
            break;
        }
    }
    if (!mod_ui_clips_.empty())
    {
        mod_ui_clips_.clear();
        SDL_SetRenderClipRect(renderer_, nullptr);
    }
    mod_ui_drawing_ = false;
}

// After the game's HUD, while it is shown: mod HUD elements and stat bars.
void draw_mod_ui_hud()
{
    if (mod_ui_ == nullptr || !mod_ui_->active())
        return;
    const ModUiMeasure measure{*this};
    modding::ui::DrawList draws;
    // Measured as drawn: the mod's own words, never a theme's replacements.
    mod_ui_drawing_ = true;
    mod_ui_->draw_hud(mod_ui_frame(), measure, draws);
    mod_ui_render(draws);
}

// Over the HUD: toasts and titles.
void draw_mod_ui_notices()
{
    if (mod_ui_ == nullptr || !mod_ui_->active())
        return;
    const ModUiMeasure measure{*this};
    modding::ui::DrawList draws;
    mod_ui_drawing_ = true;
    mod_ui_->draw_notices(mod_ui_frame(), measure, draws);
    mod_ui_render(draws);
}

// Over everything (the game's menus included): mod screens and dialogs.
void draw_mod_ui_screens()
{
    if (mod_ui_ == nullptr || !mod_ui_->screen_open())
        return;
    const ModUiMeasure measure{*this};
    modding::ui::DrawList draws;
    mod_ui_drawing_ = true;
    mod_ui_->draw_screens(mod_ui_frame(), measure, draws);
    mod_ui_render(draws);
}

// ---- Input ------------------------------------------------------------------------

// A mod screen or dialog is up: it takes the pointer, the keys and the pad,
// as the game's own screens do. True when the event was used.
bool mod_ui_event(const SDL_Event &event)
{
    if (mod_ui_ == nullptr || !mod_ui_->screen_open())
        return false;
    using modding::ui::NavKey;
    bool used = true;
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_MOTION:
        note_mouse_activity();
        mouse_x_ = event.motion.x;
        mouse_y_ = event.motion.y;
        (void)mod_ui_->pointer_move(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        note_mouse_button(event.button.button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        if (event.button.button == SDL_BUTTON_LEFT)
            (void)mod_ui_->pointer_button(event.button.x, event.button.y, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        note_mouse_activity();
        (void)mod_ui_->wheel(mouse_x_, mouse_y_, event.wheel.y);
        break;
    case SDL_EVENT_TEXT_INPUT:
        (void)mod_ui_->text(event.text.text);
        break;
    case SDL_EVENT_KEY_DOWN: {
        note_keyboard_activity();
        const SDL_Keycode key = event.key.key;
        if (mod_ui_->capturing_key())
        {
            if (!event.key.repeat)
                mod_ui_->capture_key(SDL_GetKeyName(key));
            break;
        }
        const bool editing = mod_ui_->editing_text();
        const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0U;
        std::optional<NavKey> nav;
        switch (key)
        {
        case SDLK_UP:
            nav = NavKey::up;
            break;
        case SDLK_DOWN:
            nav = NavKey::down;
            break;
        case SDLK_LEFT:
            nav = NavKey::left;
            break;
        case SDLK_RIGHT:
            nav = NavKey::right;
            break;
        case SDLK_W:
            nav = editing ? std::nullopt : std::optional{NavKey::up};
            break;
        case SDLK_S:
            nav = editing ? std::nullopt : std::optional{NavKey::down};
            break;
        case SDLK_A:
            nav = editing ? std::nullopt : std::optional{NavKey::left};
            break;
        case SDLK_D:
            nav = editing ? std::nullopt : std::optional{NavKey::right};
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            nav = NavKey::activate;
            break;
        case SDLK_SPACE:
            nav = editing ? std::nullopt : std::optional{NavKey::activate};
            break;
        case SDLK_ESCAPE:
            nav = NavKey::back;
            break;
        case SDLK_TAB:
            nav = shift ? NavKey::previous : NavKey::next;
            break;
        case SDLK_BACKSPACE:
            nav = NavKey::backspace;
            break;
        case SDLK_DELETE:
            nav = NavKey::delete_key;
            break;
        default:
            if (!editing && key >= SDLK_1 && key <= SDLK_9 && !event.key.repeat)
                (void)mod_ui_->choice_key(static_cast<int>(key - SDLK_1) + 1);
            break;
        }
        // Held keys repeat for moving and deleting, never for choosing.
        if (nav && (!event.key.repeat || (*nav != NavKey::activate && *nav != NavKey::back)))
            (void)mod_ui_->key(*nav);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        gamepad_input_active_ = true;
        switch (event.gbutton.button)
        {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            (void)mod_ui_->key(NavKey::up);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            (void)mod_ui_->key(NavKey::down);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            (void)mod_ui_->key(NavKey::left);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            (void)mod_ui_->key(NavKey::right);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            (void)mod_ui_->key(NavKey::activate);
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
            // B cancels a key capture (a pad cannot give a key) or goes back.
            if (mod_ui_->capturing_key())
                mod_ui_->capture_key("Escape");
            else
                (void)mod_ui_->key(NavKey::back);
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            (void)mod_ui_->key(NavKey::previous);
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            (void)mod_ui_->key(NavKey::next);
            break;
        default:
            break;
        }
        break;
    }
    default:
        used = false;
        break;
    }
    mod_ui_sync_text_input();
    return used;
}

// SDL text input runs while a mod text field has the keyboard (and only then:
// the game's own chat and name fields start and stop their own).
void mod_ui_sync_text_input()
{
    const bool wanted = mod_ui_ != nullptr && mod_ui_->editing_text();
    if (wanted == mod_ui_text_input_)
        return;
    mod_ui_text_input_ = wanted;
    if (wanted)
        SDL_StartTextInput(window_);
    else if (!text_editing())
        SDL_StopTextInput(window_);
}

// A key the game does not read in the world (process_world_key's default):
// a mod's binding on it, if any.
bool mod_ui_world_key(SDL_Keycode key)
{
    if (mod_ui_ == nullptr || mod_ui_->bindings().empty())
        return false;
    return mod_ui_->world_key(SDL_GetKeyName(key));
}

[[nodiscard]] bool mod_ui_has_bindings() const noexcept
{
    return mod_ui_ != nullptr && !mod_ui_->bindings().empty();
}

// ---- Every frame ----------------------------------------------------------------------

// Near the top of update(): the service's clocks, the pointer given back to
// the world when the last mod screen over it closes, the menus' mod rows.
void update_mod_ui(float delta)
{
    if (mod_ui_ == nullptr)
    {
        run_mod_ui_diagnostic();
        return;
    }
    mod_ui_->update(static_cast<double>(delta));
    sync_mod_ui_channel();
    sync_mod_ui_gameplay();
    run_mod_ui_diagnostic();
    const bool open = mod_ui_->screen_open();
    if (mod_ui_had_screen_ && !open && menu_.screen() == ui::MenuScreen::world_preview && !waiting_for_world_ &&
        !inventory_open_ && !inspector_view_ && !text_editing() && !options_.screenshot_path.has_value() &&
        pointer_capture_allowed(world_overlay()))
        SDL_SetWindowRelativeMouseMode(window_, true);
    if (!mod_ui_had_screen_ && open)
        cancel_world_action(true);
    mod_ui_had_screen_ = open;
    mod_ui_sync_text_input();
    // Faces made for mod text keep a texture per string; start them afresh
    // now and then so changing text cannot grow them without end.
    mod_ui_font_age_ += delta;
    if (mod_ui_font_age_ > 30.0F)
    {
        mod_ui_font_age_ = 0.0F;
        for (auto &[path, font] : mod_ui_fonts_)
            if (font.renderer)
                font.renderer->clear_cache();
    }
}

// Server UI requests travel over the mod channel while a modded session has
// one; without, a world this process hosts answers in process. The handlers
// go on each session's channel objects once (Integration: not every frame;
// mod_net_generation_ counts the channels attach_mod_net_objects made).
void sync_mod_ui_channel()
{
    const bool server = mod_net_server_ != nullptr && mod_net_server_->enabled();
    const bool client = mod_net_client_ != nullptr && mod_net_client_->enabled();
    if (!server && !client)
    {
        mod_ui_->set_channel(nullptr);
        return;
    }
    if (!mod_ui_net_channel_)
        mod_ui_net_channel_ = std::make_unique<ModUiNetChannel>(*this);
    mod_ui_->set_channel(mod_ui_net_channel_.get());
    if (mod_ui_channel_generation_ == mod_net_generation_)
        return;
    mod_ui_channel_generation_ = mod_net_generation_;
    try
    {
        if (server)
            mod_net_server_->on("", modding::ui::ui_channel_name,
                                [this](modding::Handle player, const modding::Value &payload) {
                                    if (mod_ui_ != nullptr)
                                        mod_ui_->receive_from_client(player, payload, static_cast<double>(elapsed_));
                                });
        if (client)
            mod_net_client_->on("", modding::ui::ui_channel_name, [this](const modding::Value &payload) {
                if (mod_ui_ != nullptr)
                    mod_ui_->receive_from_server(payload);
            });
    }
    catch (const modding::ScriptError &error)
    {
        std::cerr << "mod.ui channel: " << error.what() << '\n';
    }
}

// The Network area's join download (mod_join_download_): which add-ons the
// host needs, their sizes and progress, as the game's dialog over the join
// panel. The answer stays the Network area's input (Enter accepts, Escape
// cancels the join).
void draw_mod_download_dialog()
{
    if (mod_join_download_ == nullptr || mod_join_download_->shared == nullptr)
        return;
    const net::ModDownloadStatus status = mod_join_download_->shared->status();
    if (status.phase == net::ModDownloadPhase::idle || status.finished())
        return;
    const net::ModDownloadPromptText text = net::describe_mod_download(status);
    const MenuInk ink = menu_ink();
    const float unit = menu_unit();
    const Rect panel = draw_menu_dialog(text.title, 640.0F, 380.0F);
    const float text_cap = menu_cap(14.0F);
    const float small_cap = menu_cap(menu_cap_small);
    const Rect inner{panel.x + 24.0F * unit, panel.y + 16.0F * unit + menu_cap(menu_cap_text) + 28.0F * unit,
                     panel.width - 48.0F * unit, 0.0F};
    float y = inner.y;
    y += menu_paragraph(text.body, {inner.x, y, inner.width, menu_line(text_cap) * 3.0F}, text_cap, ink.text,
                        MenuFace::text) +
         14.0F * unit;
    const auto megabytes = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
    const float row = menu_line(small_cap) + 4.0F * unit;
    for (std::size_t index = 0U; index < status.mods.size() && index < 6U; ++index)
    {
        const net::ModDownloadItem &item = status.mods[index];
        menu_label_in(std::format("{} {}", item.descriptor.id, item.descriptor.version),
                      {inner.x, y, inner.width * 0.62F, row}, small_cap, ink.text, TextAlign::left, MenuFace::text, false);
        const std::string size = item.installed ? std::string{"Installed"}
                                 : item.received > 0U
                                     ? std::format("{:.1f} / {:.1f} MB", megabytes(item.received), megabytes(item.bytes))
                                     : std::format("{:.1f} MB", megabytes(item.bytes));
        menu_label_in(size, {inner.x + inner.width * 0.62F, y, inner.width * 0.38F, row}, small_cap,
                      item.installed ? ink.accent : ink.quiet, TextAlign::right, MenuFace::text, false);
        y += row;
    }
    if (status.mods.size() > 6U)
    {
        menu_label(std::format("and {} more", status.mods.size() - 6U), inner.x, y, small_cap, ink.quiet,
                   TextAlign::left, MenuFace::text, false);
        y += row;
    }
    y += 8.0F * unit;
    if (status.phase == net::ModDownloadPhase::downloading && status.total_bytes > 0U)
    {
        const float share = static_cast<float>(std::clamp(static_cast<double>(status.received_bytes) /
                                                              static_cast<double>(status.total_bytes),
                                                          0.0, 1.0));
        const Rect bar{inner.x, y, inner.width, std::max(6.0F, std::round(10.0F * unit))};
        fill(bar, {0U, 0U, 0U, 255U});
        fill({bar.x + 1.0F, bar.y + 1.0F, (bar.width - 2.0F) * share, bar.height - 2.0F}, ink.accent);
    }
    const bool pad = gamepad_input_active_;
    std::vector<MenuKeyHint> hints;
    if (status.phase == net::ModDownloadPhase::awaiting_consent)
        hints = {{pad ? "A" : "Enter", text.accept_label}, {pad ? "B" : "Esc", text.decline_label}};
    else
        hints = {{pad ? "B" : "Esc", "Cancel"}};
    menu_key_hints(hints, inner.x, panel.y + panel.height - 20.0F * unit - small_cap, inner.width);
}

// The Gameplay service's client view (modding/gameplay.hpp), drawn by the UI:
// its `hud` stats as bars, its NPC dialogue (answered with client_choose) and
// its quest log as the journal. Every frame; nothing without a gameplay client.
std::uint64_t mod_ui_dismissed_dialogue_{};
std::string mod_ui_quest_signature_;

void sync_mod_ui_gameplay()
{
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
        return;
    modding::ui::UiService &service = mod_ui_service();
    std::vector<modding::ui::UiService::HudStat> bars;
    for (const modding::gameplay::ClientStat &stat : mod_gameplay_->client_stats())
        if (stat.hud)
            bars.push_back({stat.name, stat.label, modding::ui::parse_color(modding::Value{stat.color}), stat.value,
                            stat.minimum, stat.maximum});
    service.set_hud_stats(std::move(bars));
    service.set_boss_bars(mod_ai_boss_bars()); // mod-ai: bosses near this player

    const std::optional<modding::gameplay::ClientDialogue> &dialogue = mod_gameplay_->client_dialogue();
    if (dialogue.has_value() && dialogue->session != service.native_dialogue_session() &&
        dialogue->session != mod_ui_dismissed_dialogue_)
    {
        const std::uint64_t session = dialogue->session;
        modding::ui::UiService::NativeDialogue shown;
        shown.session = session;
        shown.speaker = dialogue->speaker.empty() ? dialogue->title : dialogue->speaker;
        shown.text = dialogue->text;
        shown.choices = dialogue->choices;
        shown.choose = [this, session](std::size_t choice) {
            if (mod_gameplay_ != nullptr)
                mod_gameplay_->client_choose(session, choice);
        };
        // Closed unanswered: not shown again until the server opens another.
        shown.closed = [this, session] { mod_ui_dismissed_dialogue_ = session; };
        service.show_native_dialogue(std::move(shown));
    }
    else if (!dialogue.has_value() && service.native_dialogue_session() != 0U)
        service.close_native_dialogue(service.native_dialogue_session());

    // The quest log, re-sent to the journal only when it changed.
    const std::vector<modding::gameplay::ClientQuest> &quests = mod_gameplay_->client_quests();
    std::string signature;
    for (const modding::gameplay::ClientQuest &quest : quests)
    {
        signature += std::format("{}\x1f{}\x1f{}\x1f{}\x1f{}", quest.id, quest.title, quest.description, quest.stage,
                                 quest.completed ? 1 : 0);
        for (const modding::gameplay::ClientQuestObjective &step : quest.objectives)
            signature += std::format("\x1e{}\x1f{}\x1f{}\x1f{}", step.text, step.progress, step.required, step.done ? 1 : 0);
        signature += '\x1d';
    }
    if (signature == mod_ui_quest_signature_)
        return;
    mod_ui_quest_signature_ = std::move(signature);
    modding::Array list;
    for (const modding::gameplay::ClientQuest &quest : quests)
    {
        modding::Array steps;
        for (const modding::gameplay::ClientQuestObjective &step : quest.objectives)
            steps.push_back(modding::Value{modding::Map{
                {"text", modding::Value{step.required > 1U ? std::format("{} ({}/{})", step.text, step.progress,
                                                                         step.required)
                                                           : step.text}},
                {"done", modding::Value{step.done}},
                {"progress", modding::Value{step.required > 1U ? static_cast<double>(step.progress) /
                                                                     static_cast<double>(step.required)
                                                               : (step.done ? 1.0 : 0.0)}}}});
        list.push_back(modding::Value{modding::Map{
            {"id", modding::Value{quest.id}},
            {"title", modding::Value{quest.title}},
            {"text", modding::Value{quest.stage.empty() ? quest.description : quest.description + "\n" + quest.stage}},
            {"state", modding::Value{quest.completed ? "done" : "active"}},
            {"objectives", modding::Value{std::move(steps)}}}});
    }
    try
    {
        service.set_quests(modding::Value{std::move(list)});
    }
    catch (const modding::ScriptError &error)
    {
        std::cerr << "mod.ui quests: " << error.what() << '\n';
    }
}

// The rows mods add to the main menu, the in-game menu and the Add-Ons screen
// (refresh_in_game_menus, every frame). The labels are copied here, so the
// menu's views stay valid whatever scripts do in between.
void sync_mod_menu_entries()
{
    if (mod_ui_ == nullptr)
        return;
    // The Add-Ons rows also follow mod.config pages, which change without a
    // revision of the UI's own: refresh those whenever the screen is entered.
    // (Add-Ons) So does turning a mod on or off (apply_addons).
    const bool entered = menu_.screen() != mod_ui_menu_screen_ || std::exchange(mod_ui_menu_stale_, false);
    mod_ui_menu_screen_ = menu_.screen();
    if (mod_ui_->menu_revision() == mod_ui_menu_revision_ && !(entered && menu_.screen() == ui::MenuScreen::addons))
        return;
    mod_ui_menu_revision_ = mod_ui_->menu_revision();
    constexpr std::array<std::pair<modding::ui::MenuSlot, ui::MenuScreen>, 3> slots{
        {{modding::ui::MenuSlot::main, ui::MenuScreen::main},
         {modding::ui::MenuSlot::pause, ui::MenuScreen::pause},
         {modding::ui::MenuSlot::addons, ui::MenuScreen::addons}}};
    for (const auto &[slot, screen] : slots)
    {
        std::vector<std::string> &text = mod_ui_menu_text_[static_cast<std::size_t>(slot)];
        std::vector<std::string> next;
        for (const modding::ui::MenuItem &item : mod_ui_->menu_items(slot))
        {
            next.push_back(item.label);
            next.push_back(item.description);
        }
        if (next == text)
            continue;
        // Swap the menu's rows first: its views point into the old strings.
        menu_.set_mod_entries(screen, {});
        text = std::move(next);
        std::vector<ui::MenuEntry> entries;
        for (std::size_t index = 0U; index + 1U < text.size(); index += 2U)
            entries.push_back({text[index], text[index + 1U], ui::MenuCommand::mod_entry, true, {}});
        menu_.set_mod_entries(screen, std::move(entries));
    }
}

// MenuCommand::mod_entry from handle().
void activate_mod_menu_entry()
{
    const std::optional<std::size_t> index = menu_.selected_mod_entry();
    if (mod_ui_ == nullptr || !index)
        return;
    const ui::MenuScreen screen = menu_.screen();
    const modding::ui::MenuSlot slot = screen == ui::MenuScreen::pause    ? modding::ui::MenuSlot::pause
                                       : screen == ui::MenuScreen::addons ? modding::ui::MenuSlot::addons
                                                                          : modding::ui::MenuSlot::main;
    mod_ui_->activate_menu_item(slot, *index);
}

// ---- Diagnostic captures ----------------------------------------------------------------
//
// OPENMINERZ_DIAGNOSTIC_MOD_UI=<scenario>[,<scenario>...] drives the service
// through its registered script functions, exactly as a mod's calls would
// arrive, as the mod "uitest" whose files are OPENMINERZ_DIAGNOSTIC_MOD_UI_ROOT.
// For captures (--screenshot) before the script runtime is wired; nothing here
// runs without the variable. Scenarios: theme, hud, screen, dialog, dialogue,
// journal, menu, bindings.

class ModUiDiagnosticHost final : public modding::ScriptHost
{
  public:
    bool emit(modding::Side, std::string_view, const modding::Value &) override { return true; }
    modding::Value invoke(const modding::Callback &, std::span<const modding::Value>) override { return {}; }
    void release(const modding::Callback &) noexcept override {}
    [[nodiscard]] std::string_view owner_of(const modding::Callback &) const noexcept override { return "uitest"; }
    void log(std::string_view, modding::LogLevel, std::string_view message) override
    {
        std::cout << "mod.ui.diagnostic " << message << '\n';
    }
};

// A --screenshot run waits until the scenario has been on screen for a few
// frames (the read-back is the frame before the last present).
[[nodiscard]] bool mod_ui_diagnostic_ready() const
{
    const char *wanted = SDL_getenv("OPENMINERZ_DIAGNOSTIC_MOD_UI");
    return wanted == nullptr || *wanted == '\0' || (mod_ui_diagnostic_done_ && mod_ui_diagnostic_frames_ >= 3U);
}

void run_mod_ui_diagnostic()
{
    if (mod_ui_diagnostic_done_)
    {
        ++mod_ui_diagnostic_frames_;
        // "answer": the first story dialogue that comes up is answered with
        // its first choice, as pressing 1 would.
        if (mod_ui_diagnostic_answer_ && mod_ui_ != nullptr && mod_ui_->screen_open() &&
            mod_ui_->choice_key(1))
        {
            mod_ui_diagnostic_answer_ = false;
            std::cout << "mod.ui.diagnostic answered a dialogue\n";
        }
        return;
    }
    const char *wanted = SDL_getenv("OPENMINERZ_DIAGNOSTIC_MOD_UI");
    if (wanted == nullptr || *wanted == '\0')
    {
        mod_ui_diagnostic_done_ = true;
        return;
    }
    const std::string scenarios{wanted};
    // With --world everything waits until the world has run a couple of
    // frames (a pausing screen stops it; the capture needs those frames).
    if (options_.open_world &&
        (menu_.screen() != ui::MenuScreen::world_preview || waiting_for_world_ || world_ready_frames_ < 2U))
        return;
    mod_ui_diagnostic_done_ = true;
    modding::ui::UiService &service = mod_ui_service();
    // "answer" alone drives a real mod session's dialogue: it must not take
    // the service over from the runtime with this VM-less host.
    if (scenarios == "answer")
    {
        mod_ui_diagnostic_answer_ = true;
        return;
    }
    if (!mod_ui_diagnostic_registry_)
    {
        mod_ui_diagnostic_registry_ = std::make_unique<modding::ApiRegistry>();
        mod_ui_diagnostic_host_ = std::make_unique<ModUiDiagnosticHost>();
        service.register_api(*mod_ui_diagnostic_registry_, *mod_ui_diagnostic_host_);
    }
    using modding::Array;
    using modding::Map;
    using modding::Value;
    const auto call = [&](std::string_view name, std::vector<Value> args, modding::Side side = modding::Side::client) {
        const modding::ApiRegistry::Function *function = mod_ui_diagnostic_registry_->find_function(name);
        if (function == nullptr)
            return;
        modding::CallContext context{"uitest", side,
                                     side == modding::Side::client
                                         ? modding::capability::presentation | modding::capability::local_storage
                                         : modding::capability::network | modding::capability::gameplay,
                                     {}};
        try
        {
            (void)function->call(context, args);
        }
        catch (const std::exception &error)
        {
            std::cerr << "mod.ui.diagnostic " << name << ": " << error.what() << '\n';
        }
    };
    const auto has = [&](std::string_view name) {
        std::size_t start = 0U;
        while (start <= scenarios.size())
        {
            const std::size_t end = std::min(scenarios.find(',', start), scenarios.size());
            if (std::string_view{scenarios}.substr(start, end - start) == name)
                return true;
            start = end + 1U;
        }
        return false;
    };
    const auto map = [](std::initializer_list<std::pair<std::string, Value>> fields) { return Value{Map(fields)}; };
    mod_ui_diagnostic_answer_ = has("answer");
    const modding::Callback inert{1U, 1U};
    if (has("theme"))
        call("ui.theme",
             {map({{"colors", map({{"menu.focus", Value{"#FFB020"}},
                                   {"menu.row", Value{"#F0E6C8"}},
                                   {"menu.heading", Value{"#FFD27A"}},
                                   {"menu.accent", Value{"#FFB020"}},
                                   {"hud.health", Value{"#3CC85A"}},
                                   {"hud.plate", Value{"#2A1A0ACC"}}})},
                   {"fonts", map({{"display", Value{"fonts/display.ttf"}}})},
                   {"images", map({{"button", map({{"path", Value{"ui/button.png"}}, {"slice", Value{8}}})},
                                   {"dialog", map({{"path", Value{"ui/panel.png"}}, {"slice", Value{12}}})},
                                   {"panel", map({{"path", Value{"ui/panel.png"}}, {"slice", Value{12}}})}})},
                   {"strings", map({{"Play Offline", Value{"Begin the Journey"}}, {"Options", Value{"Settings"}}})}})});
    if (has("menu"))
    {
        call("ui.add_menu_entry", {map({{"menu", Value{"main"}}, {"label", Value{"Story Mode"}},
                                        {"description", Value{"A row a mod added to the title screen."}},
                                        {"on_select", Value{inert}}})});
        call("ui.add_menu_entry", {map({{"menu", Value{"pause"}}, {"label", Value{"Quest Journal"}},
                                        {"description", Value{"A row a mod added to the in-game menu."}},
                                        {"on_select", Value{inert}}})});
    }
    if (has("hud"))
    {
        service.set_hud_stats({{"survival.hunger", "Hunger", modding::ui::Color{208U, 140U, 60U, 255U}, 62.0, 0.0, 100.0},
                               {"survival.thirst", "Thirst", modding::ui::Color{70U, 160U, 230U, 255U}, 35.0, 0.0, 100.0}});
        call("ui.stat_bar", {map({{"stat", Value{"local.sanity"}}, {"label", Value{"Sanity"}}, {"anchor", Value{"top_left"}},
                                  {"y", Value{60}}, {"color", Value{"#B060E0"}}})});
        call("ui.set_stat", {Value{"local.sanity"}, Value{0.7}, Value{1.0}});
        call("ui.add_hud", {map({{"kind", Value{"stack"}},
                                 {"anchor", Value{"top_left"}},
                                 {"y", Value{100}},
                                 {"background", Value{"plate"}},
                                 {"padding", Value{8}},
                                 {"gap", Value{4}},
                                 {"children", Value{Array{map({{"kind", Value{"label"}}, {"text", Value{"[c=#FFD27A]Day 3[/c] - The Old Mine"}}}),
                                                          map({{"kind", Value{"stack"}}, {"direction", Value{"horizontal"}}, {"gap", Value{4}},
                                                               {"children", Value{Array{map({{"kind", Value{"item_slot"}}, {"item", Value{36}}, {"count", Value{12}}, {"width", Value{40}}, {"height", Value{40}}}),
                                                                                        map({{"kind", Value{"item_slot"}}, {"item", Value{3}}, {"count", Value{64}}, {"width", Value{40}}, {"height", Value{40}}})}}}})}}}})});
        call("ui.toast", {Value{"Quest Updated"}, Value{"Find the old miner before nightfall."}, map({{"item", Value{36}}, {"seconds", Value{30}}})});
        call("ui.title", {Value{"Chapter One"}, Value{"The Old Mine"}, map({{"fade_in", Value{0}}, {"stay", Value{30}}})});
        call("ui.objective", {Value{"wood"}, Value{"Collect wood (6/10)"}, map({{"progress", Value{0.6}}})});
        call("ui.objective", {Value{"torch"}, Value{"Craft a torch"}, map({{"done", Value{true}}})});
        call("ui.hide_hud", {Value{"crosshair"}});
        call("ui.move_hud", {Value{"compass"}, Value{0}, Value{40}});
    }
    if (has("screen"))
        call("ui.open_screen",
             {map({{"title", Value{"Trader"}},
                   {"children",
                    Value{Array{
                        map({{"kind", Value{"label"}},
                             {"text", Value{"Rich text: [c=#FFB020]gold[/c], [s=1.5]larger[/s] and [f=display]display face[/f]. "
                                            "Long lines wrap inside the panel and stay readable at any interface scale."}},
                             {"width", Value{"fill"}}}),
                        map({{"kind", Value{"grid"}},
                             {"columns", Value{6}},
                             {"gap", Value{6}},
                             {"children", Value{Array{map({{"kind", Value{"item_slot"}}, {"item", Value{36}}, {"count", Value{3}}, {"on_click", Value{inert}}}),
                                                      map({{"kind", Value{"item_slot"}}, {"item", Value{60}}, {"on_click", Value{inert}}}),
                                                      map({{"kind", Value{"item_slot"}}, {"item", Value{3}}, {"count", Value{64}}, {"on_click", Value{inert}}}),
                                                      map({{"kind", Value{"item_slot"}}}),
                                                      map({{"kind", Value{"item_slot"}}}),
                                                      map({{"kind", Value{"item_slot"}}})}}}}),
                        map({{"kind", Value{"toggle"}}, {"text", Value{"Sell stacks"}}, {"value", Value{true}}}),
                        map({{"kind", Value{"slider"}}, {"min", Value{0}}, {"max", Value{64}}, {"step", Value{1}}, {"value", Value{16}},
                             {"show_value", Value{true}}, {"width", Value{320}}}),
                        map({{"kind", Value{"text_input"}}, {"placeholder", Value{"Name your price"}}, {"width", Value{320}}}),
                        map({{"kind", Value{"progress"}}, {"value", Value{0.4}}, {"text", Value{"Reputation"}}, {"width", Value{320}}, {"height", Value{18}}}),
                        map({{"kind", Value{"stack"}},
                             {"direction", Value{"horizontal"}},
                             {"gap", Value{10}},
                             {"children", Value{Array{map({{"kind", Value{"button"}}, {"text", Value{"Buy"}}, {"on_click", Value{inert}}}),
                                                      map({{"kind", Value{"button"}}, {"text", Value{"Sell"}}, {"on_click", Value{inert}}}),
                                                      map({{"kind", Value{"button"}}, {"text", Value{"Leave"}}, {"enabled", Value{false}}})}}}})}}}})});
    if (has("dialog"))
        call("ui.open_dialog", {map({{"title", Value{"Leave the Mine?"}},
                                     {"text", Value{"Your torch is almost out. Turn back now, or press on into the dark?"}},
                                     {"buttons", Value{Array{map({{"text", Value{"Turn Back"}}}), map({{"text", Value{"Press On"}}})}}}})});
    if (has("dialogue"))
        call("ui.show_dialogue",
             {Value{mod_ui_local_player_},
              map({{"id", Value{"miner"}},
                   {"speaker", Value{"Old Miner"}},
                   {"portrait_item", Value{2}},
                   {"text", Value{"You came at last. The lower tunnels have gone quiet, and quiet is never good down there. "
                                  "Will you take a look?"}},
                   {"choices", Value{Array{map({{"id", Value{"yes"}}, {"text", Value{"I'll go down."}}}),
                                           map({{"id", Value{"trade"}}, {"text", Value{"What's in it for me?"}}}),
                                           map({{"id", Value{"no"}}, {"text", Value{"Not today."}}})}}}})},
             modding::Side::server);
    if (has("journal"))
    {
        call("ui.set_journal",
             {Value{Array{map({{"id", Value{"mine"}},
                               {"title", Value{"The Old Mine"}},
                               {"text", Value{"The miner asked you to find out why the lower tunnels went quiet."}},
                               {"objectives", Value{Array{map({{"text", Value{"Talk to the old miner"}}, {"done", Value{true}}}),
                                                          map({{"text", Value{"Reach the lower tunnels"}}, {"progress", Value{0.35}}})}}}}),
                          map({{"id", Value{"water"}}, {"title", Value{"Clean Water"}}, {"state", Value{"done"}}}),
                          map({{"id", Value{"lost"}}, {"title", Value{"The Lost Caravan"}}, {"state", Value{"failed"}}})}}});
        call("ui.open_journal", {});
    }
    if (has("download"))
    {
        // A join's download prompt as the Network area publishes it (with
        // --diagnostic-online-loading, which shows the join panel).
        auto context = std::make_shared<net::ModJoinDownloadContext>();
        context->shared = std::make_shared<net::ModJoinDownloads>();
        net::ModDownloadStatus status;
        status.phase = net::ModDownloadPhase::awaiting_consent;
        for (const auto &[id, bytes, installed] :
             {std::tuple{"survival-overhaul", 3U * 1024U * 1024U, false}, std::tuple{"dungeons", 12U * 1024U * 1024U, false},
              std::tuple{"story-act-one", 700U * 1024U, true}})
        {
            net::ModDownloadItem item;
            item.descriptor.id = id;
            item.descriptor.version = "1.2.0";
            item.bytes = bytes;
            item.installed = installed;
            status.total_bytes += installed ? 0U : bytes;
            status.mods.push_back(std::move(item));
        }
        context->shared->publish(status);
        mod_join_download_ = context;
    }
    if (has("bindings"))
    {
        call("input.bind", {Value{"journal"}, Value{"J"}, Value{inert}, map({{"label", Value{"Open Journal"}}})});
        call("input.bind", {Value{"sprint"}, Value{"Left Shift"}, Value{inert}, map({{"label", Value{"Sprint Toggle"}}})});
        call("input.bind", {Value{"map"}, Value{"M"}, Value{inert}, map({{"label", Value{"World Map"}}})});
        service.open_bindings_screen();
    }
}
