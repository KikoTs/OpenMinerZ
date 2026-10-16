// Member functions and state of Application::Impl: liquid mod blocks
// (mod-liquid; blocks.define{..., liquid = true}, openminerz/world/liquid.hpp).
// Included inside the class body by app.cpp, after app_mod_ai.inc.
//
// A liquid cell holds its retail fallback in the world, like every mod block;
// what makes it a liquid is its mod identity, which this peer knows from the
// session's block registry, the world generator's palette (generated lakes)
// and the overlay of mod blocks placed since. mod_liquid_update turns that
// into one world::LiquidTable whenever any of it changes and installs it
// process-wide (collision, swimming, the crosshair, retail enemies, the
// host's checks) and in the world renderer (meshing, the view from inside).
//
// Retail parity: without a client gameplay half (no server mod in the
// session) or without a liquid among its blocks no table is installed, and
// every liquid test in the game returns at its first check.

std::shared_ptr<const world::LiquidTable> mod_liquid_table_;
std::uint64_t mod_liquid_registry_seen_{~0ULL};
std::uint64_t mod_liquid_overlay_seen_{~0ULL};
std::size_t mod_liquid_looks_seen_{~std::size_t{}};
const world::CustomTerrain *mod_liquid_terrain_seen_{};
const WorldPreviewRenderer *mod_liquid_preview_seen_{};

// Every frame, before the simulation (update).
void mod_liquid_update()
{
    const bool running =
        mod_gameplay_ != nullptr && mod_gameplay_->client_running() && world_preview_ != nullptr;
    if (!running)
    {
        if (mod_liquid_table_ != nullptr)
        {
            mod_liquid_table_.reset();
            world::set_liquids(nullptr);
            if (world_preview_ != nullptr)
                world_preview_->set_liquids(nullptr);
        }
        mod_liquid_registry_seen_ = ~0ULL;
        mod_liquid_overlay_seen_ = ~0ULL;
        mod_liquid_looks_seen_ = ~std::size_t{};
        mod_liquid_terrain_seen_ = nullptr;
        mod_liquid_preview_seen_ = nullptr;
        return;
    }
    using modding::gameplay::ClientChange;
    const std::uint64_t registry = mod_gameplay_->client_revision(ClientChange::registry);
    const std::uint64_t overlay = mod_gameplay_->client_revision(ClientChange::overlay);
    const world::CustomTerrain *terrain = mod_world_terrain_.get();
    if (registry == mod_liquid_registry_seen_ && overlay == mod_liquid_overlay_seen_ &&
        mod_graphics_block_looks_.size() == mod_liquid_looks_seen_ && terrain == mod_liquid_terrain_seen_ &&
        world_preview_.get() == mod_liquid_preview_seen_)
        return;
    mod_liquid_registry_seen_ = registry;
    mod_liquid_overlay_seen_ = overlay;
    mod_liquid_looks_seen_ = mod_graphics_block_looks_.size();
    mod_liquid_terrain_seen_ = terrain;
    mod_liquid_preview_seen_ = world_preview_.get();

    auto table = std::make_shared<world::LiquidTable>();
    const auto &types = mod_gameplay_->client_block_types();
    // One kind per liquid block, in registry order; its colour is its
    // texture's mean colour once the graphics area has looked at it.
    const auto add_kind = [&](const modding::gameplay::ClientBlockType &type) -> std::uint8_t {
        for (std::size_t index = 0U; index < table->kinds().size(); ++index)
            if (table->kinds()[index].key == type.key)
                return static_cast<std::uint8_t>(index + 1U);
        world::LiquidTable::Kind kind;
        kind.key = type.key;
        kind.fallback = static_cast<world::BlockType>(type.fallback);
        if (type.id >= 1U && type.id <= mod_graphics_block_looks_.size())
        {
            const modding::graphics::Color mean = mod_graphics_block_looks_[type.id - 1U].color;
            kind.color = {mean.r, mean.g, mean.b};
        }
        return table->add_kind(std::move(kind));
    };
    std::vector<std::uint8_t> kinds(types.size() + 1U, 0U); // registry id -> kind
    for (const modding::gameplay::ClientBlockType &type : types)
        if (type.liquid && type.id >= 1U && type.id < kinds.size())
            kinds[type.id] = add_kind(type);
    // Generated lakes: the generator's palette by key (the client VM's own
    // definitions until the host's registry is in, as the looks do).
    if (terrain != nullptr)
    {
        const world::TerrainDefinition &definition = terrain->definition();
        for (std::size_t index = 0U; index < definition.palette.size(); ++index)
        {
            std::uint8_t kind{};
            const auto type = std::ranges::find(types, definition.palette[index],
                                                &modding::gameplay::ClientBlockType::key);
            if (type != types.end())
                kind = type->id < kinds.size() ? kinds[type->id] : std::uint8_t{0U};
            else if (const auto defined = mod_gameplay_->client_defined_block(definition.palette[index]);
                     defined.has_value() && defined->liquid)
                kind = add_kind(*defined);
            table->set_palette(index + 1U, kind, definition.palette_fallback[index]);
        }
    }
    // Placed since: every overlay cell on a liquid's fallback (one that is not
    // a liquid hides what was generated there).
    if (!table->empty())
        for (const auto &[cell, id] : mod_gameplay_->client_overlay())
        {
            if (id == 0U || id > types.size())
                continue;
            const auto fallback = static_cast<world::BlockType>(types[id - 1U].fallback);
            if (table->falls_back_to(fallback))
                table->set_placed(cell.x, cell.y, cell.z, kinds[id], fallback);
        }
    std::shared_ptr<const world::LiquidTable> installed =
        table->empty() ? nullptr : std::shared_ptr<const world::LiquidTable>{std::move(table)};
    if (installed == nullptr && mod_liquid_table_ == nullptr)
        return;
    if (installed != nullptr && mod_liquid_table_ != nullptr && installed->same_as(*mod_liquid_table_))
        return;
    const std::size_t liquid_count = installed != nullptr ? installed->kinds().size() : 0U;
    if (liquid_count != (mod_liquid_table_ != nullptr ? mod_liquid_table_->kinds().size() : 0U))
        std::cout << "mod.liquid kinds=" << liquid_count << '\n';
    mod_liquid_table_ = installed;
    world::set_liquids(installed);
    world_preview_->set_liquids(std::move(installed));
}

// ModGameplayBackend::local_view: players.me's liquid fields and the liquid
// item.used's target names.
void mod_liquid_view(modding::gameplay::LocalView &view) const
{
    if (world::active_liquids() == nullptr || world_preview_ == nullptr)
        return;
    const world::WorldRegion &region = world_preview_->region();
    const world::PlayerState &state = player_.state();
    view.in_liquid = world::liquid_depth(region, state.position.x, state.position.y, state.position.z) > 0.0F;
    view.underwater = world_preview_->eye_liquid(state) != nullptr;
    // The first liquid cell along the look within reach; anything else that
    // is there ends the look (the cell the eye is in does not).
    const auto eye = world::Vec3{static_cast<float>(view.eye.x), static_cast<float>(view.eye.y),
                                 static_cast<float>(view.eye.z)};
    const auto look = world::Vec3{static_cast<float>(view.look.x), static_cast<float>(view.look.y),
                                  static_cast<float>(view.look.z)};
    constexpr float step = 0.05F;
    std::optional<std::array<int, 3>> last;
    for (float travelled = 0.0F; travelled <= world::PlayerSimulation::interaction_reach; travelled += step)
    {
        const std::array<int, 3> cell{static_cast<int>(std::floor(eye.x + look.x * travelled)),
                                      static_cast<int>(std::floor(eye.y + look.y * travelled)),
                                      static_cast<int>(std::floor(eye.z + look.z * travelled))};
        if (last.has_value() && *last == cell)
            continue;
        if (world::liquid_at(region, cell[0], cell[1], cell[2]))
        {
            view.liquid = modding::gameplay::BlockPos{cell[0], cell[1], cell[2]};
            return;
        }
        if (last.has_value() && world::is_solid(region.block(cell[0], cell[1], cell[2])))
            return;
        last = cell;
    }
}
