// Member functions and state of Application::Impl: the modding graphics and
// assets service (openminerz/modding/graphics.hpp). Included inside the class
// body by app.cpp.
//
// PRESENTATION ONLY. Texture, sound and animation packs, glTF models, lighting
// props, effects, atmosphere layers, mod sounds and music, and camera effects.
// Nothing here touches the simulation, the wire or a save.
//
// With no mod installed nothing is created: no service, no audio device, no
// screen pass, no shader variant, and every hook below returns at once.
//
// The hooks app.cpp calls: mod_graphics_sync (every frame, before the world
// draws; follows the installed packs), mod_graphics_before_world_draw (sky,
// camera and lens adjustments of the frame) and release_mod_graphics (from
// ~Impl). The world renderer draws the models, lights, LUT grade and fade
// itself (WorldPreviewRenderer::set_mod_graphics). register_mod_graphics_api
// is called from register_mod_apis (app_mod_runtime.inc).

// The service's narrow view of the game.
class ModGraphicsBackend final : public modding::graphics::GraphicsBackend
{
  public:
    explicit ModGraphicsBackend(Impl &app) : app_(app) {}

    [[nodiscard]] bool locate(modding::Handle target, modding::graphics::Vec3 &position,
                              float &yaw) const override
    {
        // (Visuals) A mod creature is where it is drawn this frame, so what is
        // attached to it never slides against it.
        if (target.kind == modding::HandleKind::entity && app_.mod_graphics_ != nullptr)
            if (const modding::graphics::CreatureView *view = app_.mod_graphics_->creature(target.id))
            {
                position = view->position;
                yaw = view->yaw;
                return true;
            }
        // Entity and player handles belong to the gameplay area, which
        // installs mod_graphics_locator_; until it does nothing can be
        // followed and attached lights, models and effects stay dark.
        if (!app_.mod_graphics_locator_ || !app_.mod_graphics_locator_(target, position, yaw))
            return false;
        // (Visuals) The local player is where the camera drew it (the
        // simulation's position runs up to a tick ahead of the picture).
        if (target.kind == modding::HandleKind::player && app_.mod_graphics_camera_valid_ &&
            target == app_.mod_net_local_player())
        {
            position = app_.mod_graphics_camera_feet_;
            yaw = app_.mod_graphics_camera_yaw_;
        }
        return true;
    }
    [[nodiscard]] bool effects_available() const override
    {
        return app_.world_preview_ != nullptr && app_.world_preview_->enhanced_effects();
    }
    // (Visuals) Until a world has drawn twice (the first frame decides the
    // presentation), effects wait instead of being refused.
    [[nodiscard]] bool effects_pending() const override
    {
        return app_.world_preview_ == nullptr ||
               app_.world_preview_.get() != app_.mod_graphics_preview_ ||
               app_.mod_graphics_preview_frames_ < 3U;
    }
    vfx::EffectHandle spawn_effect(const vfx::EffectDesc &desc,
                                   const vfx::SpawnParams &params) override
    {
        return app_.world_preview_ != nullptr ? app_.world_preview_->spawn_effect(desc, params)
                                              : vfx::EffectHandle{};
    }
    [[nodiscard]] bool effect_alive(vfx::EffectHandle handle) const override
    {
        vfx::System *effects =
            app_.world_preview_ != nullptr ? app_.world_preview_->effects() : nullptr;
        return effects != nullptr && effects->alive(handle);
    }
    void move_effect(vfx::EffectHandle handle, modding::graphics::Vec3 position,
                     modding::graphics::Vec3 velocity) override
    {
        if (vfx::System *effects =
                app_.world_preview_ != nullptr ? app_.world_preview_->effects() : nullptr)
            effects->move(handle, position, velocity);
    }
    void kill_effect(vfx::EffectHandle handle, bool immediate) override
    {
        if (vfx::System *effects =
                app_.world_preview_ != nullptr ? app_.world_preview_->effects() : nullptr)
            effects->kill(handle, immediate);
    }
    [[nodiscard]] modding::graphics::Mixer *mixer(bool open) override
    {
        if (!open)
            return app_.mod_audio_ != nullptr && app_.mod_audio_->available()
                       ? &app_.mod_audio_->mixer()
                       : nullptr;
        return app_.mod_graphics_mixer();
    }

  private:
    Impl &app_;
};

std::unique_ptr<ModGraphicsBackend> mod_graphics_backend_;
std::unique_ptr<modding::graphics::GraphicsService> mod_graphics_;
std::unique_ptr<ModAudioDevice> mod_audio_;
// (Gameplay wiring) where an entity or player handle is: feet position and yaw.
std::function<bool(modding::Handle, modding::graphics::Vec3 &, float &)> mod_graphics_locator_;
// The mods whose assets/ are installed, as "id\nroot\n..." (empty: none), and
// what they were chosen from (mod_graphics_sync).
std::string mod_graphics_installed_;
std::string mod_graphics_key_;
bool mod_graphics_post_applied_{};
bool mod_graphics_audio_failed_{};
// (Polish) OPENMINERZ_DIAGNOSTIC_MOD_AUDIO_WAV, read once: 0 unread, 1 on, 2 off.
int mod_audio_dump_state_{};
std::filesystem::path mod_audio_dump_path_;
double mod_audio_dump_seconds_{30.0};
bool mod_audio_dump_world_marked_{};
std::chrono::steady_clock::time_point mod_graphics_last_frame_{};
// (Visuals) the world renderer the frames below were counted for, and how
// many it has drawn (effects spawned before the second wait for it).
const WorldPreviewRenderer *mod_graphics_preview_{};
std::uint32_t mod_graphics_preview_frames_{};
// (Visuals) where this frame's camera drew the local player (feet).
modding::graphics::Vec3 mod_graphics_camera_feet_{};
float mod_graphics_camera_yaw_{};
bool mod_graphics_camera_valid_{};
// (Visuals) Gameplay's replicated mod entities as the frames draw them:
// smoothed between the 60 Hz client ticks; a death kept a few seconds.
struct ModCreatureTrack final
{
    modding::graphics::CreatureView view;
    float corpse_seconds{-1.0F};
    bool seen{};
};
std::unordered_map<std::uint64_t, ModCreatureTrack> mod_graphics_creatures_;
static constexpr float mod_graphics_corpse_seconds = 3.0F;
// (Visuals) the mod block looks by registry id - 1, and the client view
// revisions they were built from (~0: not yet).
std::vector<modding::graphics::ModBlockLook> mod_graphics_block_looks_;
std::map<std::string, modding::graphics::Color, std::less<>> mod_graphics_block_colors_;
std::set<std::string, std::less<>> mod_graphics_block_reported_;
std::uint64_t mod_graphics_registry_seen_{~0ULL};
std::uint64_t mod_graphics_overlay_seen_{~0ULL};
const WorldPreviewRenderer *mod_graphics_blocks_preview_{};
bool mod_graphics_blocks_active_{};

modding::graphics::GraphicsService &mod_graphics()
{
    if (mod_graphics_ == nullptr)
    {
        mod_graphics_backend_ = std::make_unique<ModGraphicsBackend>(*this);
        mod_graphics_ =
            std::make_unique<modding::graphics::GraphicsService>(*mod_graphics_backend_);
        // Scripts run at session start, before the next frame's sync: the
        // packs are brought up to date the first time one looks for an asset.
        mod_graphics_->set_source_refresh([this] { mod_graphics_sync(); });
        // Sound packs replace one-shot retail cues through the mod mixer.
        OriginalSoundBank::set_cue_override([this](std::string_view cue,
                                                   const world::Vec3 *position) {
            if (mod_graphics_ == nullptr || mod_graphics_->assets().empty())
                return false;
            const std::string name = "sounds/cues/" + std::string{cue};
            if (!modding::graphics::valid_asset_name(name))
                return false;
            const auto sound = mod_graphics_->sound(name);
            modding::graphics::Mixer *mixer = sound != nullptr ? mod_graphics_mixer() : nullptr;
            if (mixer == nullptr)
                return false;
            modding::graphics::VoiceParams params;
            if (position != nullptr)
                params.position = modding::graphics::Vec3{position->x, position->y, position->z};
            mixer->mark(name); // (Polish) names the cue in an audio dump
            return mixer->play(sound, params) != 0U;
        });
    }
    return *mod_graphics_;
}

// The mixer of mod sounds; the device opens the first time a mod plays one.
[[nodiscard]] modding::graphics::Mixer *mod_graphics_mixer()
{
    if (mod_audio_ == nullptr && !mod_graphics_audio_failed_)
    {
        mod_audio_ = std::make_unique<ModAudioDevice>();
        if (!mod_audio_->available())
        {
            std::cerr << "Mod audio unavailable: " << mod_audio_->error() << '\n';
            mod_graphics_audio_failed_ = true;
        }
        else if (mod_audio_dump_requested())
            mod_audio_->start_dump(mod_audio_dump_path_, mod_audio_dump_seconds_);
    }
    return mod_audio_ != nullptr && mod_audio_->available() ? &mod_audio_->mixer() : nullptr;
}

// (Polish) OPENMINERZ_DIAGNOSTIC_MOD_AUDIO_WAV=<file.wav>[,<seconds>] records
// what the mod mixer plays - mod sounds, music, retail cues a pack replaced;
// not the retail sound, which the XACT bridge process plays - from the first
// world frame for <seconds> (30 by default) into a 16-bit WAV, with a
// "<file.wav>.marks.txt" of every play/music/stop at the frame it took effect.
bool mod_audio_dump_requested()
{
    if (mod_audio_dump_state_ == 0)
    {
        mod_audio_dump_state_ = 2;
        if (const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_MOD_AUDIO_WAV");
            value != nullptr && *value != '\0')
        {
            std::string text{value};
            if (const std::size_t comma = text.rfind(','); comma != std::string::npos)
            {
                if (const double seconds = std::atof(text.c_str() + comma + 1U); seconds > 0.0)
                    mod_audio_dump_seconds_ = std::min(seconds, 600.0);
                text.resize(comma);
            }
            mod_audio_dump_path_ = std::filesystem::path{text};
            mod_audio_dump_state_ = 1;
        }
    }
    return mod_audio_dump_state_ == 1;
}

// (fix) The game's own soundtrack (the songs the XACT bridge plays) fades out
// while a mod's music plays and back in once it stops, playing on beneath it
// all along: 1 when no mod music plays, 0 ducked. apply_audio_settings
// multiplies the music level by it. Every frame; nothing without mod audio.
float mod_music_duck_{1.0F};
float mod_music_duck_sent_{1.0F};

void mod_graphics_duck_retail_music(float delta)
{
    modding::graphics::Mixer *mixer =
        mod_audio_ != nullptr && mod_audio_->available() ? &mod_audio_->mixer() : nullptr;
    const float target = mixer != nullptr && mixer->music_playing() ? 0.0F : 1.0F;
    if (mod_music_duck_ == target)
        return;
    const float step = std::max(delta, 0.0F) / 1.5F;
    mod_music_duck_ = target < mod_music_duck_ ? std::max(target, mod_music_duck_ - step)
                                               : std::min(target, mod_music_duck_ + step);
    // The bridge hears of it in steps of 5%, and of the end exactly.
    if (std::abs(mod_music_duck_ - mod_music_duck_sent_) < 0.05F && mod_music_duck_ != target)
        return;
    mod_music_duck_sent_ = mod_music_duck_;
    apply_audio_settings();
    if (mixer != nullptr) // the audio dump's marks (OPENMINERZ_DIAGNOSTIC_MOD_AUDIO_WAV)
        mixer->mark(std::format("retail music level x{:.2f}", mod_music_duck_));
}

// Once a world frame: the device opens with the first one (so the recording
// starts there, sounds or not) and a finished recording is written.
void mod_audio_dump_frame()
{
    if (!mod_audio_dump_requested())
        return;
    if (modding::graphics::Mixer *mixer = mod_graphics_mixer();
        mixer != nullptr && !mod_audio_dump_world_marked_)
    {
        mixer->mark("first world frame");
        mod_audio_dump_world_marked_ = true;
    }
    if (mod_audio_ != nullptr)
        mod_audio_->poll_dump();
}

void register_mod_graphics_api(modding::ApiRegistry &registry, modding::ScriptHost &host)
{
    modding::graphics::register_graphics_api(registry, host, mod_graphics());
}

// From ~Impl, before SDL shuts down: the audio device goes while SDL still runs, and retail cues
// stop asking for replacements.
void release_mod_graphics()
{
    OriginalSoundBank::set_cue_override({});
    if (mod_graphics_ != nullptr)
        mod_graphics_->reset();
    mod_audio_.reset();
    if (world_preview_ != nullptr)
        world_preview_->set_mod_graphics(nullptr);
}

// Which mods' assets apply now: every client mod always (texture packs work on
// any server), plus the server and core mods of the running session.
[[nodiscard]] std::vector<modding::graphics::AssetSource> mod_graphics_wanted_sources() const
{
    std::vector<modding::graphics::AssetSource> sources;
    if (!mod_catalog_valid_)
        return sources;
    std::vector<std::string> session;
    if (mod_runtime_ != nullptr && mod_session_ != ModSessionKind::none)
        for (const modding::ModStatus &status : mod_runtime_->status())
            session.push_back(status.id);
    const bool classic = host_settings_.protocol == net::ProtocolVersion::classic_v3;
    // (Add-Ons) Joined, a server mod that is off here may still be one the
    // host runs (addon_join_catalog); client mods that are off never are.
    const net::ModCatalog catalog = mod_session_ == ModSessionKind::joined ? addon_join_catalog() : mod_catalog_;
    std::vector<const net::LoadedMod *> chosen;
    for (const net::LoadedMod &mod : catalog.mods)
    {
        const bool client = mod.descriptor.scope == net::ModScope::client;
        // Offline and hosting (and in the menus, ready for the next world) every
        // mod of the protocol applies, packs without scripts included; joined,
        // only the mods the session runs.
        const bool in_session =
            std::find(session.begin(), session.end(), mod.descriptor.id) != session.end() ||
            (mod_session_ != ModSessionKind::joined && (classic ? mod.classic : mod.modern));
        if (!client && !in_session)
            continue;
        chosen.push_back(&mod);
    }
    std::stable_sort(chosen.begin(), chosen.end(),
                     [](const net::LoadedMod *a, const net::LoadedMod *b) {
                         return a->load_order < b->load_order;
                     });
    for (const net::LoadedMod *mod : chosen)
    {
        std::error_code error;
        const std::filesystem::path assets = mod->root / "assets";
        if (std::filesystem::is_directory(assets, error))
            sources.push_back({mod->descriptor.id, assets});
    }
    return sources;
}

// Every frame: follows the installed packs. When they change, loaders see the
// new stack and what was loaded from the old one is loaded again.
void mod_graphics_sync()
{
    if (!mod_catalog_valid_ && mod_graphics_ == nullptr)
        return;
    // What decides the packs, cheaply: the catalog, the session and (joined)
    // the mods it runs. Only when that changes is the disk looked at.
    std::string key = std::to_string(static_cast<int>(mod_session_)) +
                      (mod_catalog_valid_ ? "v" : "x") +
                      std::to_string(static_cast<int>(host_settings_.protocol)) + '\n';
    if (mod_catalog_valid_)
        for (const net::LoadedMod &mod : mod_catalog_.mods)
            key += mod.descriptor.id + '\n' + mod.root.string() + '\n';
    if (mod_session_ == ModSessionKind::joined && mod_runtime_ != nullptr)
        for (const modding::ModStatus &status : mod_runtime_->status())
            key += status.id + '\n';
    if (key == mod_graphics_key_)
        return;
    mod_graphics_key_ = std::move(key);
    const std::vector<modding::graphics::AssetSource> wanted = mod_graphics_wanted_sources();
    std::string signature;
    for (const modding::graphics::AssetSource &source : wanted)
        signature += source.mod_id + '\n' + source.root.string() + '\n';
    if (signature == mod_graphics_installed_)
        return;
    mod_graphics_installed_ = signature;
    mod_graphics().set_sources(wanted);
    std::cout << "mod.graphics packs=" << wanted.size() << '\n';
    if (world_preview_ != nullptr)
    {
        world_preview_->set_mod_graphics(mod_graphics_.get());
        world_preview_->reload_mod_textures();
    }
    mod_graphics_reload_interface();
}

// The interface art a pack may replace, loaded again when the packs change:
// both builds' HUD sprites, screen sprites and item-icon atlases (with the
// packs' per-item icons). The screens' pointer cursor is kept as it is.
void mod_graphics_reload_interface()
{
    const auto classic_hud = interface_data_path(AssetStyle::classic);
    const auto modern_hud = interface_data_path(AssetStyle::modern);
    for (const auto &[art, style, directory] :
         {std::tuple{&classic_hud_art_, UiStyle::classic, classic_hud},
          std::tuple{&modern_hud_art_, UiStyle::modern, modern_hud}})
    {
        for (SDL_Texture *&texture : art->textures)
        {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        load_hud_art(*art, style, directory);
    }
    for (const auto &[art, style, directory] :
         {std::tuple{&classic_screen_art_, UiStyle::classic, classic_hud},
          std::tuple{&modern_screen_art_, UiStyle::modern, modern_hud}})
    {
        for (SDL_Texture *&texture : art->textures)
        {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        for (SDL_Texture **texture : {&art->frame, &art->icons, &art->icons_large})
        {
            SDL_DestroyTexture(*texture);
            *texture = nullptr;
        }
        SDL_Cursor *pointer = art->pointer;
        load_screen_art(*art, style, directory);
        if (art->pointer != nullptr && art->pointer != pointer)
            SDL_DestroyCursor(art->pointer);
        art->pointer = pointer;
        mod_graphics_item_icons(*art, style, directory);
    }
}

// Writes the packs' textures/items/<item id>.png into an item-icon atlas
// (64-pixel cells, 128 in the large one), at the item's cell for that build.
void mod_graphics_item_icons(ScreenArt &art, UiStyle style, const std::filesystem::path &directory)
{
    const auto assets = modding::graphics::presentation_assets();
    if (assets == nullptr)
        return;
    static constexpr std::array<std::string_view, 3> extensions{".png", ".bmp", ".tga"};
    const std::vector<modding::graphics::AssetStack::Resolved> icons =
        assets->stack.list("textures/items", extensions);
    if (icons.empty())
        return;
    for (const bool large : {false, true})
    {
        SDL_Texture *&texture = large ? art.icons_large : art.icons;
        if (texture == nullptr)
            continue;
        const int cell = large ? 128 : 64;
        SDL_Surface *loaded = load_content_surface(
            directory / (large ? "item_icons_large.bmp" : "item_icons.bmp"), ContentKind::ui);
        SDL_Surface *surface =
            loaded != nullptr ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
        SDL_DestroySurface(loaded);
        if (surface == nullptr)
            continue;
        modding::graphics::Image atlas;
        atlas.width = surface->w;
        atlas.height = surface->h;
        atlas.rgba.resize(static_cast<std::size_t>(surface->w) * surface->h * 4U);
        for (int y = 0; y < surface->h; ++y)
            std::memcpy(&atlas.rgba[static_cast<std::size_t>(y) * surface->w * 4U],
                        static_cast<const std::uint8_t *>(surface->pixels) +
                            static_cast<std::size_t>(y) * surface->pitch,
                        static_cast<std::size_t>(surface->w) * 4U);
        for (const modding::graphics::AssetStack::Resolved &file : icons)
        {
            const std::string stem = std::filesystem::path{file.name}.stem().string();
            if (stem.empty() || stem.size() > 4U ||
                !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            const int index =
                screens::retail_icon(static_cast<InventoryItemId>(std::stoi(stem)), style);
            if (index < 0 || ((index & 7) + 1) * cell > atlas.width ||
                (index / 8 + 1) * cell > atlas.height)
                continue;
            std::string error;
            auto image = modding::graphics::load_image(file.path, &error, {1024, 4U << 20U});
            if (!image)
            {
                std::cerr << "Item icon " << file.name << ": " << error << '\n';
                continue;
            }
            modding::graphics::Image icon = modding::graphics::resample(*image, cell, cell);
            // The atlas is premultiplied, as XNA's sprites are.
            for (std::size_t texel = 0U; texel + 3U < icon.rgba.size(); texel += 4U)
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                    icon.rgba[texel + channel] = static_cast<std::uint8_t>(
                        (icon.rgba[texel + channel] * icon.rgba[texel + 3U] + 127U) / 255U);
            modding::graphics::blit(atlas, icon, (index & 7) * cell, (index / 8) * cell);
        }
        for (int y = 0; y < surface->h; ++y)
            std::memcpy(static_cast<std::uint8_t *>(surface->pixels) +
                            static_cast<std::size_t>(y) * surface->pitch,
                        &atlas.rgba[static_cast<std::size_t>(y) * surface->w * 4U],
                        static_cast<std::size_t>(surface->w) * 4U);
        SDL_Texture *replaced = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_DestroySurface(surface);
        if (replaced == nullptr)
            continue;
        SDL_SetTextureBlendMode(replaced, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(replaced, SDL_SCALEMODE_LINEAR);
        SDL_DestroyTexture(texture);
        texture = replaced;
    }
}

// Before the world draws: the service's clocks, the atmosphere layers over the
// sky and the lens, the camera effects, the listener and the volumes.
void mod_graphics_before_world_draw(world::SkyState &sky, world::PlayerState &camera, float &fov)
{
    mod_graphics_diagnostics(camera);
    mod_audio_dump_frame(); // (Polish)
    if (mod_graphics_ == nullptr || world_preview_ == nullptr)
        return;
    world_preview_->set_mod_graphics(mod_graphics_.get());
    const auto now = std::chrono::steady_clock::now();
    const float delta =
        mod_graphics_last_frame_.time_since_epoch().count() == 0
            ? 0.0F
            : std::clamp(std::chrono::duration<float>(now - mod_graphics_last_frame_).count(), 0.0F,
                         0.25F);
    mod_graphics_last_frame_ = now;
    modding::graphics::GraphicsService &service = *mod_graphics_;
    // (Visuals) the frame this world draws, the camera it draws from, the
    // mod creatures where they stand and the mod blocks' tiles.
    if (mod_graphics_preview_ != world_preview_.get())
    {
        mod_graphics_preview_ = world_preview_.get();
        mod_graphics_preview_frames_ = 0U;
    }
    mod_graphics_preview_frames_ = std::min(mod_graphics_preview_frames_ + 1U, 1000U);
    mod_graphics_camera_feet_ = {camera.position.x, camera.position.y, camera.position.z};
    mod_graphics_camera_yaw_ = camera.yaw;
    mod_graphics_camera_valid_ = true;
    mod_graphics_update_creatures(delta);
    mod_graphics_update_blocks();
    const modding::graphics::Vec3 eye{camera.position.x,
                                      camera.position.y + world::PlayerSimulation::eye_height,
                                      camera.position.z};
    service.step(delta, eye);
    if (modding::graphics::Mixer *mixer =
            mod_audio_ != nullptr && mod_audio_->available() ? &mod_audio_->mixer() : nullptr)
    {
        mixer->set_listener(eye, camera.yaw);
        mixer->set_levels({settings_.master_volume, settings_.music_volume,
                           settings_.effects_volume, settings_.ambience_volume,
                           settings_.mute_unfocused && !window_focused_});
    }
    // Atmosphere layers over the sky, and over the lens and exposure while
    // any applies; the player's own settings come back when none does.
    const bool atmosphere = service.atmosphere().active();
    if (atmosphere)
    {
        service.atmosphere().apply(sky);
        PostEffects effects = resolve_post_effects(settings_);
        float exposure = settings_.exposure_compensation;
        service.atmosphere().apply(effects, exposure);
        world_preview_->set_post_effects(effects);
        world_preview_->set_grading(static_cast<int>(settings_.tone_mapping),
                                    settings_.auto_exposure, exposure, settings_.color_grading);
        mod_graphics_post_applied_ = true;
    }
    else if (mod_graphics_post_applied_)
    {
        world_preview_->set_post_effects(resolve_post_effects(settings_));
        apply_grading_settings();
        mod_graphics_post_applied_ = false;
    }
    // Camera effects, within the accessibility settings.
    if (service.camera().active())
    {
        const modding::graphics::CameraOutput output =
            service.camera().output({settings_.reduced_motion, settings_.graphics.explosion_shake});
        camera.yaw += output.yaw;
        camera.pitch = std::clamp(camera.pitch + output.pitch, -1.55F, 1.55F);
        fov = std::clamp(fov + output.fov, 10.0F, 120.0F);
    }
}

// Capture aid: OPENMINERZ_DIAGNOSTIC_MOD_LIGHTS=<count>[,<radius>[,<spacing>]]
// places that many coloured lights in rows of 16 ahead of the camera, spacing
// blocks apart, through the same service a script uses (for --bench and
// --screenshot; never in play).
void mod_graphics_diagnostics(const world::PlayerState &camera)
{
    static bool done{};
    const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_MOD_LIGHTS");
    if (done || value == nullptr || world_preview_ == nullptr)
        return;
    done = true;
    char *end{};
    const long count = std::strtol(value, &end, 10);
    float radius = 7.0F;
    float spacing = 2.5F;
    if (end != nullptr && *end == ',')
    {
        radius = std::clamp(std::strtof(end + 1, &end), 0.5F, 64.0F);
        if (end != nullptr && *end == ',')
            spacing = std::clamp(std::strtof(end + 1, nullptr), 1.0F, 32.0F);
    }
    if (count <= 0)
        return;
    modding::graphics::GraphicsService &service = mod_graphics();
    world_preview_->set_mod_graphics(&service);
    const float forward_x = std::sin(camera.yaw);
    const float forward_z = -std::cos(camera.yaw);
    for (int index = 0; index < static_cast<int>(std::min(count, 4096L)); ++index)
    {
        const int row = index / 16;
        const int column = index % 16;
        const int columns = static_cast<int>(std::min(count, 16L));
        modding::graphics::LightDesc light;
        const float ahead = 3.0F + static_cast<float>(row) * spacing;
        const float side =
            (static_cast<float>(column) - static_cast<float>(columns - 1) * 0.5F) * spacing;
        light.position = {camera.position.x + forward_x * ahead - forward_z * side,
                          camera.position.y + 1.0F + static_cast<float>(index % 3),
                          camera.position.z + forward_z * ahead + forward_x * side};
        static constexpr std::array<modding::graphics::Color, 4> colours{
            {{1.0F, 0.45F, 0.15F}, {0.2F, 0.55F, 1.0F}, {0.3F, 1.0F, 0.4F}, {1.0F, 0.2F, 0.7F}}};
        light.color = colours[static_cast<std::size_t>(index) % colours.size()];
        light.intensity = 1.6F;
        light.radius = radius;
        light.flicker = index % 2 == 0 ? 0.25F : 0.0F;
        (void)service.lights().create("diagnostic", light);
    }
    std::cout << "mod.graphics diagnostic lights=" << service.lights().size() << '\n';
}

// (Visuals) Gameplay's replicated mod entities into the service's creature
// list: smoothed towards each 60 Hz client sample so they glide at any frame
// rate, and a death kept as a corpse for a few seconds so it can be played.
void mod_graphics_update_creatures(float delta)
{
    mod_ai_presentation(); // mod-ai: projectile impacts, boss music
    std::vector<modding::graphics::CreatureView> &views = mod_graphics_->creatures();
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
    {
        views.clear();
        mod_graphics_creatures_.clear();
        return;
    }
    const std::vector<modding::gameplay::ClientEntity> &entities = mod_gameplay_->client_entities();
    if (entities.empty() && mod_graphics_creatures_.empty())
    {
        views.clear();
        mod_ai_append_projectile_views(views); // mod-ai: projectiles in flight
        return;
    }
    for (auto &[id, track] : mod_graphics_creatures_)
    {
        (void)id;
        track.seen = false;
    }
    const float follow = 1.0F - std::exp(-std::max(delta, 0.0F) * 30.0F);
    for (const modding::gameplay::ClientEntity &entity : entities)
    {
        const auto [found, fresh] = mod_graphics_creatures_.try_emplace(entity.id);
        ModCreatureTrack &track = found->second;
        modding::graphics::CreatureView &view = track.view;
        track.seen = true;
        track.corpse_seconds = -1.0F;
        const modding::graphics::Vec3 target{static_cast<float>(entity.position.x),
                                             static_cast<float>(entity.position.y),
                                             static_cast<float>(entity.position.z)};
        if (fresh || vfx::length(target - view.position) > 4.0F)
        {
            // New, or teleported: no glide.
            view.position = target;
            view.yaw = entity.yaw;
        }
        else
        {
            view.position += (target - view.position) * follow;
            float turn = entity.yaw - view.yaw;
            turn -= std::round(turn / 6.2831853F) * 6.2831853F;
            view.yaw += turn * follow;
        }
        view.id = entity.id;
        view.kind = entity.kind;
        view.model = entity.model;
        view.texture = entity.texture;
        view.state = entity.animation.empty() ? "idle" : entity.animation;
        view.name = entity.name;
        view.health = entity.health_fraction;
        view.width = entity.width;
        view.height = entity.height;
    }
    for (auto track = mod_graphics_creatures_.begin(); track != mod_graphics_creatures_.end();)
    {
        if (!track->second.seen)
        {
            if (track->second.corpse_seconds < 0.0F && mod_gameplay_->client_entity_died(track->first))
            {
                track->second.corpse_seconds = 0.0F;
                track->second.view.state = "death";
                track->second.view.health = 0.0F;
                track->second.view.name.clear();
            }
            else if (track->second.corpse_seconds >= 0.0F)
                track->second.corpse_seconds += delta;
            if (track->second.corpse_seconds < 0.0F ||
                track->second.corpse_seconds > mod_graphics_corpse_seconds)
            {
                track = mod_graphics_creatures_.erase(track);
                continue;
            }
        }
        ++track;
    }
    views.clear();
    views.reserve(mod_graphics_creatures_.size());
    for (const auto &[id, track] : mod_graphics_creatures_)
    {
        (void)id;
        views.push_back(track.view);
    }
    mod_ai_append_projectile_views(views); // mod-ai: projectiles in flight
}

// (Visuals) How one mod block type is drawn: its textures as tiles of the
// terrain arrays (BlockTextureTable; `new_tiles` set when one was added, so
// the arrays need installing again) and the dust colour of its side.
[[nodiscard]] modding::graphics::ModBlockLook mod_graphics_block_look(
    const modding::gameplay::ClientBlockType &type, bool &new_tiles)
{
    modding::graphics::GraphicsService &service = mod_graphics();
    modding::graphics::BlockTextureTable &table = service.block_tiles();
    static constexpr std::array<std::string_view, 4> extensions{".png", ".bmp", ".tga", ".jpg"};
    static constexpr std::array<world::FaceDirection, 3> slot_faces{
        world::FaceDirection::positive_y, world::FaceDirection::positive_x,
        world::FaceDirection::negative_y};
    modding::graphics::ModBlockLook look;
    look.fallback = type.fallback;
    look.liquid = type.liquid; // (mod-liquid)
    const world::BlockColor retail = world::fallback_color(static_cast<world::BlockType>(type.fallback));
    look.color = {retail.red / 255.0F, retail.green / 255.0F, retail.blue / 255.0F};
    for (std::size_t slot = 0U; slot < 3U; ++slot)
    {
        // A face without a texture of its own, or one that cannot be
        // found, keeps the fallback's face.
        look.tiles[slot] = world::atlas_tile(static_cast<world::BlockType>(type.fallback),
                                             slot_faces[slot]);
        const std::string &name = !type.faces[slot].empty() ? type.faces[slot] : type.texture;
        if (name.empty())
            continue;
        std::optional<std::uint8_t> tile = table.find(name);
        const auto resolved = service.assets().resolve(name, extensions);
        if (!tile && resolved)
        {
            tile = table.tile_for(name);
            new_tiles = new_tiles || tile.has_value();
        }
        if (!tile)
        {
            if (mod_graphics_block_reported_.insert(name).second)
                std::cerr << "Mod block " << type.key << ": texture '" << name << "' "
                          << (resolved ? "does not fit (192 mod tiles)" : "not found")
                          << "; its fallback's face is drawn\n";
            continue;
        }
        look.tiles[slot] = *tile;
        // The dust a dig raises: the side texture's mean colour.
        if (slot == 1U && resolved)
        {
            auto colour = mod_graphics_block_colors_.find(name);
            if (colour == mod_graphics_block_colors_.end())
            {
                modding::graphics::Color mean = look.color;
                if (const auto image = modding::graphics::load_image(resolved->path, nullptr,
                                                                     {1024, 8U << 20U}))
                {
                    double sum[3]{};
                    double weight{};
                    for (std::size_t texel = 0U; texel + 3U < image->rgba.size(); texel += 4U)
                    {
                        const double alpha = image->rgba[texel + 3U] / 255.0;
                        for (std::size_t channel = 0U; channel < 3U; ++channel)
                            sum[channel] += image->rgba[texel + channel] / 255.0 * alpha;
                        weight += alpha;
                    }
                    if (weight > 0.0)
                        mean = {static_cast<float>(sum[0] / weight),
                                static_cast<float>(sum[1] / weight),
                                static_cast<float>(sum[2] / weight)};
                }
                colour = mod_graphics_block_colors_.emplace(name, mean).first;
            }
            look.color = colour->second;
        }
    }
    return look;
}

// (Visuals) The mod blocks the terrain draws: the registry's textures as tiles
// of the terrain arrays (BlockTextureTable), and the overlay's cells as a
// snapshot for the mesh workers. Follows the client view by its revisions.
void mod_graphics_update_blocks()
{
    const bool running = mod_gameplay_ != nullptr && mod_gameplay_->client_running();
    if (mod_graphics_blocks_preview_ != world_preview_.get())
    {
        // A new world renderer has no overlay yet.
        mod_graphics_blocks_preview_ = world_preview_.get();
        mod_graphics_registry_seen_ = ~0ULL;
        mod_graphics_overlay_seen_ = ~0ULL;
        mod_graphics_blocks_active_ = false;
    }
    if (!running)
    {
        if (mod_graphics_blocks_active_)
            world_preview_->set_mod_block_overlay(nullptr);
        mod_graphics_blocks_active_ = false;
        mod_graphics_block_looks_.clear();
        mod_graphics_registry_seen_ = ~0ULL;
        mod_graphics_overlay_seen_ = ~0ULL;
        return;
    }
    using modding::gameplay::ClientChange;
    const std::uint64_t registry = mod_gameplay_->client_revision(ClientChange::registry);
    const std::uint64_t overlay = mod_gameplay_->client_revision(ClientChange::overlay);
    if (registry == mod_graphics_registry_seen_ && overlay == mod_graphics_overlay_seen_)
        return;
    if (registry != mod_graphics_registry_seen_)
    {
        mod_graphics_registry_seen_ = registry;
        mod_graphics_block_looks_.clear();
        bool new_tiles = false;
        for (const modding::gameplay::ClientBlockType &type : mod_gameplay_->client_block_types())
            mod_graphics_block_looks_.push_back(mod_graphics_block_look(type, new_tiles));
        if (new_tiles)
            world_preview_->install_mod_block_tiles();
    }
    mod_graphics_overlay_seen_ = overlay;
    auto snapshot = std::make_shared<modding::graphics::ModBlockOverlay>();
    for (const auto &[cell, id] : mod_gameplay_->client_overlay())
        if (id >= 1U && id <= mod_graphics_block_looks_.size())
            snapshot->set(cell.x, cell.y, cell.z, mod_graphics_block_looks_[id - 1U]);
    world_preview_->set_mod_block_overlay(std::move(snapshot));
    mod_graphics_blocks_active_ = true;
}

// (Visuals) The names of mod NPCs over their heads, drawn like the players'
// gamertags (white with a black outline) but only within 32 blocks, fading
// out from 24, and not through terrain.
void mod_graphics_draw_nametags()
{
    if (mod_graphics_ == nullptr || mod_graphics_->creatures().empty() || world_preview_ == nullptr ||
        font_ == nullptr)
        return;
    const NametagCamera &camera = world_preview_->overlay_camera();
    if (!camera.valid)
        return;
    const world::WorldRegion &region = world_preview_->region();
    // Whether terrain stands between the eye and a point (one cell per step).
    const auto hidden = [&](world::Vec3 to) {
        const world::Vec3 from = camera.eye;
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float dz = to.z - from.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        const int steps = std::min(static_cast<int>(length * 2.0F), 96);
        for (int step = 1; step < steps; ++step)
        {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const int x = static_cast<int>(std::floor(from.x + dx * t));
            const int y = static_cast<int>(std::floor(from.y + dy * t));
            const int z = static_cast<int>(std::floor(from.z + dz * t));
            if (region.contains(x, y, z) && world::occludes(region.block(x, y, z)))
                return true;
        }
        return false;
    };
    const bool classic = online_gameplay_active() &&
                         online_gameplay_protocol() == net::ProtocolVersion::classic_v3;
    const NametagFontMetrics &shipped = classic ? classic_nametag_font : modern_nametag_font;
    NametagFontSize &size = nametag_font_sizes_[classic ? 0U : 1U];
    if (size.pixels == 0)
    {
        size.pixels = font_->pixel_size_for_cap_height(shipped.cap_height);
        size.baseline = font_->baseline(size.pixels);
    }
    for (const modding::graphics::CreatureView &view : mod_graphics_->creatures())
    {
        if (view.name.empty())
            continue;
        const world::Vec3 head{view.position.x, view.position.y + view.height + 0.35F, view.position.z};
        const float dx = head.x - camera.eye.x;
        const float dy = head.y - camera.eye.y;
        const float dz = head.z - camera.eye.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > 32.0F || hidden(head))
            continue;
        const auto alpha = static_cast<std::uint8_t>(
            std::lround(255.0F * std::clamp((32.0F - distance) / 8.0F, 0.0F, 1.0F)));
        // project_nametag anchors two blocks above the point it is given.
        const auto anchor = project_nametag(
            camera, {head.x, head.y - nametag_height_above_feet, head.z});
        if (!anchor.has_value())
            continue;
        if (RetailFonts *fonts = retail_fonts())
        {
            const float tag_scale = retail_canvas().scale;
            const TextExtent cell = fonts->measure(RetailFont::name_tag, view.name, tag_scale);
            if (anchor->x < -cell.width || anchor->x > camera.width + cell.width ||
                anchor->y < -cell.height || anchor->y > camera.height + cell.height)
                continue;
            fonts->draw_outlined(RetailFont::name_tag, view.name, anchor->x - cell.width * 0.5F,
                                 anchor->y - cell.height * 0.5F, {255U, 255U, 255U, alpha},
                                 {0U, 0U, 0U, alpha}, 1, tag_scale);
            continue;
        }
        const TextExtent extent = font_->measure(view.name, size.pixels);
        if (anchor->x < -extent.width || anchor->x > camera.width + extent.width ||
            anchor->y < -extent.height || anchor->y > camera.height + extent.height)
            continue;
        const NametagAnchor origin = nametag_text_origin(*anchor, extent.width - 4.0F, 0.0F);
        const float top = anchor->y + nametag_baseline_below_anchor(shipped) -
                          static_cast<float>(size.baseline);
        for (const NametagPass &pass : nametag_passes)
            font_->draw(view.name, origin.x - 1.0F + pass.dx, top + pass.dy, size.pixels,
                        pass.outline ? SDL_Color{0U, 0U, 0U, alpha}
                                     : SDL_Color{255U, 255U, 255U, alpha});
    }
}

// Mod item icons in the HUD, the screens, pickups and the hand.
#include "app_mod_graphics_items.inc"
