// Member functions and state of Application::Impl: world generators from
// server and core mods (mod-world; openminerz/modding/worldgen.hpp,
// openminerz/world/custom_terrain.hpp). Included inside the class body by
// app.cpp, before the runtime, so the service outlives the VMs that call it.
//
// When what happens (all of it inert unless a server or core mod of the
// session calls terrain.define):
//   * register_mod_world_api (register_mod_apis): the `terrain` and `noise`
//     libraries.
//   * mod_world_prepare_new / mod_world_can_open (start_new_world, the --world
//     diagnostic, load_world): whether the world is new, and what it records
//     (<world>.omw.mods/worldgen.omzw). A world whose generator mod is not
//     installed is refused before anything loads.
//   * mod_world_session_started (start_mod_session, once the VMs ran their
//     scripts): builds the generator, judges it against the record, and
//     installs it in the world renderer (chunks, far terrain), where the
//     gameplay backend reads it; a host publishes it for its clients.
//   * mod_world_update (every frame): a joined client follows its host's
//     choice; the camera's mod biome drives atmosphere layers and music; the
//     looks of generated mod blocks follow the block registry.
//   * mod_world_save (save_world): the record.
//   * mod_world_spawn_point (spawn_player): a new world's spawn, and the
//     --teleport diagnostic.
//
// Retail parity: with no world defined nothing is installed, recorded or
// published; the generator, the saves and the wire are exactly as before.

class ModWorldBackend final : public modding::worldgen::WorldgenBackend
{
  public:
    explicit ModWorldBackend(Impl &app) : app_{app}
    {
    }
    [[nodiscard]] std::optional<std::uint8_t> block_fallback(modding::Side side,
                                                             std::string_view key) const override
    {
        return app_.mod_gameplay_ != nullptr ? app_.mod_gameplay_->block_fallback(side, key)
                                             : std::nullopt;
    }
    [[nodiscard]] const world::WorldGenerator *generator() const override
    {
        return app_.world_preview_ != nullptr ? &app_.world_preview_->generator() : nullptr;
    }
    [[nodiscard]] std::int32_t world_seed() const override
    {
        return app_.world_seed_;
    }

  private:
    Impl &app_;
};

std::unique_ptr<ModWorldBackend> mod_world_backend_;
std::unique_ptr<modding::worldgen::WorldgenService> mod_world_;
// The generator installed in world_preview_ (null: retail terrain).
std::shared_ptr<const world::CustomTerrain> mod_world_terrain_;
// What the world being played records, and whether it is new (it takes the
// generator the session's mods define).
std::optional<modding::worldgen::GeneratorRecord> mod_world_record_;
// What the world records from its next save on: the generator it is played
// with (kept past the session's end, which comes before the last save).
std::optional<modding::worldgen::GeneratorRecord> mod_world_pending_record_;
bool mod_world_new_{};
bool mod_world_host_checked_{};
// Client presentation: the atmosphere layer of each mod biome, the biome whose
// music plays, the biome last announced.
std::map<std::string, std::uint64_t, std::less<>> mod_world_layers_;
std::string mod_world_music_;
std::string mod_world_biome_announced_;
// (wave 5) The biome about to be announced and since when (the clock below),
// and when each biome was last announced.
std::string mod_world_biome_candidate_;
double mod_world_biome_candidate_since_{};
std::map<std::string, double, std::less<>> mod_world_biome_toasted_;
double mod_world_clock_{};
float mod_world_presentation_seconds_{};
std::uint64_t mod_world_palette_registry_{~0ULL};
std::size_t mod_world_palette_size_{~std::size_t{}};
const WorldPreviewRenderer *mod_world_palette_preview_{};

// The Enhanced air of a mod world: its biomes' `like`, blended by weight,
// instead of retail's rings (world::biome_atmosphere_hook). Main thread only.
static const world::WorldGenerator *&mod_world_atmosphere_generator() noexcept
{
    static const world::WorldGenerator *generator{};
    return generator;
}

static world::BiomeAtmosphere mod_world_biome_atmosphere(float x, float z) noexcept
{
    const world::WorldGenerator *generator = mod_world_atmosphere_generator();
    const auto &terrain = generator != nullptr ? generator->custom_terrain() : nullptr;
    if (terrain == nullptr)
        return world::biome_atmosphere(world::BiomeKind::classic);
    std::array<world::BiomeWeight, world::maximum_biome_weights> weights{};
    const std::size_t count = terrain->biome_weights(*generator, static_cast<int>(std::floor(x)),
                                                     static_cast<int>(std::floor(z)), weights);
    world::BiomeAtmosphere result{};
    result.fog_start_ratio = 0.0F;
    result.saturation = 0.0F;
    result.grade_multiplier = {};
    for (std::size_t index = 0U; index < count; ++index)
    {
        const world::TerrainBiome &biome = terrain->definition().biomes[weights[index].biome];
        const world::BiomeKind kind =
            biome.retail == world::RetailReuse::world
                ? generator->retail_biome_at(static_cast<int>(x), static_cast<int>(z))
                : biome.like;
        const world::BiomeAtmosphere part = world::biome_atmosphere(kind);
        const float w = weights[index].weight;
        const auto add = [w](world::SkyColor &to, world::SkyColor from) {
            to.red += from.red * w;
            to.green += from.green * w;
            to.blue += from.blue * w;
        };
        add(result.fog_target, part.fog_target);
        add(result.horizon_target, part.horizon_target);
        add(result.zenith_target, part.zenith_target);
        add(result.grade_multiplier, part.grade_multiplier);
        result.fog_mix += part.fog_mix * w;
        result.horizon_mix += part.horizon_mix * w;
        result.zenith_mix += part.zenith_mix * w;
        result.fog_start_ratio += part.fog_start_ratio * w;
        result.grade_strength += part.grade_strength * w;
        result.saturation += part.saturation * w;
        result.storminess += part.storminess * w;
    }
    return result;
}

void register_mod_world_api(modding::ApiRegistry &registry, modding::ScriptHost &host)
{
    mod_world_backend_ = std::make_unique<ModWorldBackend>(*this);
    mod_world_ = modding::worldgen::register_worldgen_api(registry, host, *mod_world_backend_);
}

[[nodiscard]] static std::filesystem::path mod_world_sidecar(const std::filesystem::path &world)
{
    std::filesystem::path sidecar = world;
    sidecar += ".mods";
    return sidecar;
}

// A world made now: it takes the generator its mods define.
void mod_world_prepare_new()
{
    mod_world_new_ = true;
    mod_world_record_.reset();
    mod_world_pending_record_.reset();
    mod_world_host_checked_ = false;
}

// load_world, before anything is read into the game: a world whose terrain
// came from a mod this profile does not run is refused (opening it would
// generate retail terrain under its edits).
[[nodiscard]] bool mod_world_can_open(const std::filesystem::path &path)
{
    mod_world_new_ = false;
    mod_world_host_checked_ = false;
    mod_world_pending_record_.reset();
    std::string error;
    mod_world_record_ = modding::worldgen::read_generator_record(mod_world_sidecar(path), &error);
    if (!error.empty())
        std::cerr << "mod.world " << error << '\n';
    if (!mod_world_record_.has_value())
        return true;
    for (const net::LoadedMod &mod : mod_profile_mods(false))
        if (mod.descriptor.id == mod_world_record_->mod_id && mod.descriptor.scope != net::ModScope::client)
            return true;
    const std::string message = modding::worldgen::generator_message(
        modding::worldgen::GeneratorVerdict::missing, mod_world_record_, std::nullopt);
    std::cout << "mod.world refused " << message << '\n';
    // (Add-Ons) A prompt over the world list rather than a passing notice:
    // it says whether the mod is off or missing, and can turn it on.
    addon_prompt_world(path, *mod_world_record_);
    mod_world_record_.reset();
    return false;
}

[[nodiscard]] std::string mod_world_version(std::string_view id) const
{
    for (const net::LoadedMod &mod : mod_profile_mods(false))
        if (mod.descriptor.id == id)
            return mod.descriptor.version;
    if (mod_catalog_valid_)
        for (const net::LoadedMod &mod : mod_catalog_.mods)
            if (mod.descriptor.id == id)
                return mod.descriptor.version;
    return {};
}

void mod_world_install(std::shared_ptr<const world::CustomTerrain> terrain)
{
    if (world_preview_ == nullptr)
        return;
    const auto &installed = world_preview_->generator().custom_terrain();
    const bool changes = (installed == nullptr) != (terrain == nullptr) ||
                         (installed != nullptr && installed->digest() != terrain->digest());
    mod_world_terrain_ = terrain;
    if (mod_world_ != nullptr)
        mod_world_->set_active(terrain);
    world_preview_->set_custom_terrain(std::move(terrain));
    // (mod-liquid) Its lakes are meshed as liquids from the first section on.
    mod_liquid_update();
    mod_world_atmosphere_generator() =
        mod_world_terrain_ != nullptr ? &world_preview_->generator() : nullptr;
    world::biome_atmosphere_hook().store(mod_world_terrain_ != nullptr ? &mod_world_biome_atmosphere
                                                                       : nullptr);
    mod_world_biome_announced_.clear();
    if (changes)
    {
        // The whole window is generated and meshed again: the loading gate
        // waits for it, and the player is placed once it is ready.
        waiting_for_world_ = !world_preview_->preload_complete();
        std::cout << "mod.world terrain="
                  << (mod_world_terrain_ != nullptr
                          ? mod_world_terrain_->definition().mod_id + " digest=" +
                                mod_world_terrain_->digest().substr(0U, 16U)
                          : std::string{"retail"})
                  << '\n';
    }
}

// Leaves a world that cannot be played as it was made, without saving it.
void mod_world_refuse(std::string_view title, const std::string &message)
{
    std::cout << "mod.world refused " << message << '\n';
    active_world_path_.clear();
    stop_mod_session();
    shutdown_online_client();
    shutdown_online_host();
    cancel_world_action();
    close_inventory();
    SDL_SetWindowRelativeMouseMode(window_, false);
    menu_.open(ui::MenuScreen::main);
    sound_bank_->play_menu_music();
    toast(std::string{title}, message);
}

// After a session's VMs ran their scripts (start_mod_session).
void mod_world_session_started()
{
    mod_world_host_checked_ = false;
    if (mod_world_ == nullptr || !mod_runtime_ || world_preview_ == nullptr)
        return;
    const bool server = mod_runtime_->running(modding::Side::server);
    const modding::Side side = server ? modding::Side::server : modding::Side::client;
    if (!mod_world_->defines_world(side))
    {
        if (mod_world_terrain_ != nullptr)
            mod_world_install(nullptr);
        return;
    }
    std::string error;
    std::shared_ptr<const world::CustomTerrain> terrain = mod_world_->build(side, error);
    // The host's two VMs ran the same files: a definition that differs between
    // them depends on random numbers, settings or the clock, and would
    // differ on every joined player too.
    if (terrain != nullptr && server && mod_runtime_->running(modding::Side::client))
    {
        std::string client_error;
        const auto mirror = mod_world_->build(modding::Side::client, client_error);
        if (mirror == nullptr || mirror->digest() != terrain->digest())
        {
            error = "the world definition differs between the server and client VMs: it must not "
                    "depend on random numbers, settings or the time";
            terrain.reset();
        }
    }
    if (!error.empty())
    {
        std::cerr << "mod.world error " << error << '\n';
        toast("WORLD GENERATOR ERROR", error.substr(0U, 160U));
    }
    std::optional<modding::worldgen::GeneratorRecord> current;
    if (terrain != nullptr)
    {
        const std::string id = mod_world_->world_mod(side).first;
        current = modding::worldgen::GeneratorRecord{id, mod_world_version(id), terrain->digest()};
    }
    if (mod_session_ == ModSessionKind::joined)
    {
        // The host decides (mod_world_update); until its answer arrives the
        // world is generated the way the session's mods say.
        mod_world_install(std::move(terrain));
        return;
    }
    using modding::worldgen::GeneratorVerdict;
    const GeneratorVerdict verdict =
        modding::worldgen::judge_generator(mod_world_record_, current, mod_world_new_);
    const std::string message = modding::worldgen::generator_message(verdict, mod_world_record_, current);
    static constexpr std::array<std::string_view, 5> verdicts{"retail", "keep_retail", "use", "changed",
                                                              "missing"};
    std::cout << "mod.world verdict=" << verdicts[static_cast<std::size_t>(verdict)]
              << (mod_world_new_ ? " new" : "") << (message.empty() ? "" : " - ") << message << '\n';
    switch (verdict)
    {
    case GeneratorVerdict::missing:
        mod_world_refuse("WORLD NEEDS A MOD", message);
        return;
    case GeneratorVerdict::changed:
        toast("WORLD GENERATOR CHANGED", message);
        mod_world_install(terrain);
        // Once saved, the world is the new generator's (warned once).
        mod_world_pending_record_ = current;
        break;
    case GeneratorVerdict::keep_retail:
        toast("RETAIL TERRAIN", message);
        mod_world_install(nullptr);
        mod_world_pending_record_.reset();
        break;
    case GeneratorVerdict::use:
        mod_world_install(terrain);
        mod_world_pending_record_ = current;
        if (mod_world_new_)
            mod_world_save();
        break;
    case GeneratorVerdict::retail:
        mod_world_install(nullptr);
        mod_world_pending_record_.reset();
        break;
    }
    // Joined players generate what the host does (engine key, world store).
    if (mod_net_server_ != nullptr && mod_net_server_->enabled() && current.has_value())
    {
        modding::Value published{modding::Map{{"retail", modding::Value{true}}}};
        if (mod_world_terrain_ != nullptr)
            published = modding::Value{modding::Map{{"mod", modding::Value{current->mod_id}},
                                                    {"version", modding::Value{current->mod_version}},
                                                    {"digest", modding::Value{current->digest}}}};
        mod_net_server_->set(modding::StoreRef::world(), "", modding::worldgen::terrain_channel_key,
                             std::move(published),
                             modding::ReplicatedKeyOptions{false, modding::ReplicationPriority::critical});
    }
}

void mod_world_session_stopped()
{
    // The renderer keeps its terrain (a reload installs the same again without
    // regenerating); what follows the session goes.
    world::biome_atmosphere_hook().store(nullptr);
    mod_world_atmosphere_generator() = nullptr;
    mod_world_terrain_.reset();
    if (mod_world_ != nullptr)
        mod_world_->set_active(nullptr);
    for (const auto &[key, layer] : mod_world_layers_)
        if (mod_graphics_ != nullptr)
            static_cast<void>(mod_graphics_->atmosphere().remove(layer, true));
    mod_world_layers_.clear();
    if (!mod_world_music_.empty() && mod_graphics_ != nullptr)
        mod_graphics_->stop_music(2.0F);
    mod_world_music_.clear();
    mod_world_biome_announced_.clear();
    mod_world_biome_candidate_.clear();
    mod_world_biome_toasted_.clear();
}

// save_world: the generator the world is played with. (Leaving a world stops
// the session before its last save, so this does not ask the session.)
void mod_world_save()
{
    if (!mod_world_pending_record_.has_value() || active_world_path_.empty() ||
        mod_world_record_ == mod_world_pending_record_)
        return;
    std::string error;
    if (!modding::worldgen::write_generator_record(mod_world_sidecar(active_world_path_),
                                                   *mod_world_pending_record_, &error))
        std::cerr << "mod.world record " << error << '\n';
    else
        mod_world_record_ = mod_world_pending_record_;
}

// spawn_player: where a player without a remembered position starts.
void mod_world_spawn_point(int &spawn_x, int &spawn_z)
{
    std::optional<std::array<int, 2>> target;
    if (options_.teleport.has_value())
        target = std::array{static_cast<int>(std::floor((*options_.teleport)[0])),
                            static_cast<int>(std::floor((*options_.teleport)[1]))};
    else if (mod_world_terrain_ != nullptr && !pending_load_position_.has_value() &&
             options_.world_biome == 0 && !options_.preview_environment.has_value())
        target = std::array{mod_world_terrain_->definition().spawn_x,
                            mod_world_terrain_->definition().spawn_z};
    if (!target.has_value() || world_preview_ == nullptr)
        return;
    spawn_x = (*target)[0];
    spawn_z = (*target)[1];
    if (!world_preview_->region_loaded(static_cast<float>(spawn_x) + 0.5F,
                                       static_cast<float>(spawn_z) + 0.5F))
    {
        world::PlayerState there;
        there.position = {static_cast<float>(spawn_x) + 0.5F, 32.0F, static_cast<float>(spawn_z) + 0.5F};
        world_preview_->recenter_on(there);
    }
}

// The gameplay backend's view (app_mod_gameplay.inc).
[[nodiscard]] int mod_world_biome(int x, int z) const
{
    const world::WorldGenerator &generator = world_preview_->generator();
    const world::TerrainBiome &biome =
        mod_world_terrain_->definition().biomes[mod_world_terrain_->biome_at(generator, x, z)];
    return biome.retail != world::RetailReuse::none ? static_cast<int>(generator.biome_at(x, z))
                                                    : world::biome_id(biome.key);
}

[[nodiscard]] std::string mod_world_generated_block(int x, int y, int z, bool before_change) const
{
    if (mod_world_terrain_ == nullptr || world_preview_ == nullptr)
        return {};
    const std::uint8_t palette = world_preview_->generated_mod_cell(x, y, z, before_change);
    const auto &keys = mod_world_terrain_->definition().palette;
    return palette == 0U || palette > keys.size() ? std::string{} : keys[palette - 1U];
}

// Every frame (update): the host's choice, the biome's presentation and the
// looks of generated mod blocks.
void mod_world_update(float delta)
{
    if (world_preview_ == nullptr || mod_session_ == ModSessionKind::none)
        return;
    mod_world_follow_host();
    if (mod_world_terrain_ == nullptr)
        return;
    mod_world_update_palette();
    mod_world_clock_ += delta;
    mod_world_presentation_seconds_ -= delta;
    if (mod_world_presentation_seconds_ > 0.0F)
        return;
    mod_world_presentation_seconds_ = 0.25F;
    mod_world_update_presentation();
}

// A joined client: once the host's snapshot is in, generate what it does.
void mod_world_follow_host()
{
    if (mod_session_ != ModSessionKind::joined || mod_world_host_checked_ ||
        mod_net_client_ == nullptr || !mod_net_client_->ready())
        return;
    mod_world_host_checked_ = true;
    const modding::Value *chosen =
        mod_net_client_->get(modding::StoreRef::world(), "", modding::worldgen::terrain_channel_key);
    const modding::Value *digest = chosen != nullptr ? chosen->field("digest") : nullptr;
    if (digest == nullptr || digest->type() != modding::ValueType::string)
    {
        // The host plays retail terrain (or its mods define no world).
        if (mod_world_terrain_ != nullptr)
            mod_world_install(nullptr);
        return;
    }
    if (mod_world_terrain_ != nullptr && mod_world_terrain_->digest() == digest->as_string())
        return;
    const modding::Value *mod = chosen->field("mod");
    mod_world_refuse("WORLD GENERATOR MISMATCH",
                     std::format("The host's world comes from '{}', but this game builds it differently. "
                                 "Install the same version of the mod as the host.",
                                 mod != nullptr && mod->type() == modding::ValueType::string
                                     ? mod->as_string()
                                     : std::string{"?"}));
}

void mod_world_update_palette()
{
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
        return;
    const std::uint64_t registry =
        mod_gameplay_->client_revision(modding::gameplay::ClientChange::registry);
    if (registry == mod_world_palette_registry_ && mod_graphics_block_looks_.size() == mod_world_palette_size_ &&
        mod_world_palette_preview_ == world_preview_.get())
        return;
    mod_world_palette_registry_ = registry;
    mod_world_palette_size_ = mod_graphics_block_looks_.size();
    mod_world_palette_preview_ = world_preview_.get();
    const world::TerrainDefinition &definition = mod_world_terrain_->definition();
    auto looks = std::make_shared<world::ModPaletteLooks>(definition.palette.size());
    const auto &types = mod_gameplay_->client_block_types();
    bool new_tiles = false;
    // The packs, which otherwise first follow the session at the first world
    // frame: the textures are looked for while the world still loads.
    mod_graphics_sync();
    for (std::size_t index = 0U; index < definition.palette.size(); ++index)
    {
        const auto type = std::ranges::find(types, definition.palette[index],
                                            &modding::gameplay::ClientBlockType::key);
        modding::graphics::ModBlockLook look;
        if (type != types.end() && type->id != 0U && type->id <= mod_graphics_block_looks_.size())
            look = mod_graphics_block_looks_[type->id - 1U];
        else if (const auto defined = mod_gameplay_->client_defined_block(definition.palette[index]))
            // (fix) Until the registry is in - all through the loading screen,
            // which otherwise ends on the fallbacks' faces and swaps them a few
            // frames later - the client VM's own definition of the block.
            look = mod_graphics_block_look(*defined, new_tiles);
        else
            continue;
        if (look.fallback != static_cast<std::uint8_t>(definition.palette_fallback[index]))
            continue;
        (*looks)[index] = {look.tiles, true, look.liquid}; // (mod-liquid) liquid
    }
    if (new_tiles)
        world_preview_->install_mod_block_tiles();
    world_preview_->set_mod_palette(std::move(looks));
}

void mod_world_update_presentation()
{
    if (mod_world_ == nullptr || !mod_runtime_ || !mod_runtime_->running(modding::Side::client))
        return;
    const world::PlayerState &player = player_.state();
    const world::WorldGenerator &generator = world_preview_->generator();
    std::array<world::BiomeWeight, world::maximum_biome_weights> weights{};
    const std::size_t count = mod_world_terrain_->biome_weights(
        generator, static_cast<int>(std::floor(player.position.x)),
        static_cast<int>(std::floor(player.position.z)), weights);
    const auto &biomes = mod_world_terrain_->definition().biomes;
    const auto &looks = mod_world_->biomes(modding::Side::client);
    const auto weight_of = [&](std::string_view key) {
        for (std::size_t index = 0U; index < count; ++index)
            if (biomes[weights[index].biome].key == key)
                return weights[index].weight;
        return 0.0F;
    };
    modding::graphics::GraphicsService &graphics = mod_graphics();
    // Atmosphere: each mod biome's layer follows its weight under the player
    // (and fades in and out over its fade times when it starts or ends).
    for (const modding::worldgen::BiomePresentation &look : looks)
    {
        if (look.atmosphere.is_nil())
            continue;
        const float weight = weight_of(look.key);
        auto found = mod_world_layers_.find(look.key);
        if (weight <= 0.0F)
        {
            if (found != mod_world_layers_.end())
            {
                static_cast<void>(graphics.atmosphere().remove(found->second));
                mod_world_layers_.erase(found);
            }
            continue;
        }
        modding::graphics::AtmosphereLayerDesc desc;
        try
        {
            desc = modding::graphics::atmosphere_from_value(look.atmosphere);
        }
        catch (const modding::ScriptError &failure)
        {
            std::cerr << "mod.world " << look.key << " atmosphere: " << failure.what() << '\n';
            continue;
        }
        desc.strength *= weight;
        if (found != mod_world_layers_.end() && graphics.atmosphere().update(found->second, desc))
            continue;
        if (const auto layer = graphics.atmosphere().create(look.mod, desc))
            mod_world_layers_.insert_or_assign(look.key, *layer);
    }
    // Music and the "entering" notice: the biome that clearly dominates.
    const world::TerrainBiome &dominant = biomes[weights[0].biome];
    if (count == 0U || weights[0].weight < 0.65F)
        return;
    if (dominant.key != mod_world_biome_announced_)
    {
        // (wave 5) Announced once it has held for a moment, and not again
        // within a minute: where a mod's biomes are altitude zones of one
        // landscape (a snow line), walking along the line would otherwise
        // announce every few steps.
        if (dominant.key != mod_world_biome_candidate_)
        {
            mod_world_biome_candidate_ = dominant.key;
            mod_world_biome_candidate_since_ = mod_world_clock_;
        }
        else if (mod_world_clock_ - mod_world_biome_candidate_since_ >= 1.5)
        {
            const auto last = mod_world_biome_toasted_.find(dominant.key);
            if (!mod_world_biome_announced_.empty() && dominant.retail == world::RetailReuse::none &&
                (last == mod_world_biome_toasted_.end() || mod_world_clock_ - last->second >= 60.0))
            {
                toast(std::format("ENTERING {}", dominant.name), "");
                mod_world_biome_toasted_.insert_or_assign(dominant.key, mod_world_clock_);
            }
            mod_world_biome_announced_ = dominant.key;
        }
    }
    else
        mod_world_biome_candidate_ = dominant.key;
    const auto look = std::ranges::find(looks, dominant.key, &modding::worldgen::BiomePresentation::key);
    const std::string wanted = look != looks.end() && !look->music.empty() ? dominant.key : std::string{};
    if (wanted == mod_world_music_)
        return;
    if (wanted.empty())
        graphics.stop_music(3.0F);
    else
        static_cast<void>(graphics.play_music(look->mod, look->music, 3.0F,
                                              static_cast<float>(look->music_volume), true));
    mod_world_music_ = wanted;
}
