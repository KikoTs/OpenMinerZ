// Member functions and state of Application::Impl: the gameplay modding
// service (openminerz/modding/gameplay.hpp). Included inside the class body by
// app.cpp, after app_mod_runtime.inc.
//
// Two halves:
//   * ModGameplayBackend - the service's narrow view of the game: this
//     process's player, the authoritative world edit path, crates, pickups,
//     the clock, retail enemies and chat.
//   * the mod_gameplay_* hooks app.cpp calls at the points where the game
//     decides something a mod may want to see or change (damage, death, digs,
//     placements, item use, crafting, chat, enemy spawns and kills, other
//     players' edits and chat on the host, the simulation step).
//
// Retail parity: until the runtime has a server or client VM running, the
// service is not running and every hook returns at its first test with the
// game's own value; the difficulty and speed helpers return their input
// untouched unless a mod set them. Nothing here sends a stock message the game
// would not have sent itself: world edits use publish_local_block_edit,
// pickups the ordinary pickup path, chat BroadcastTextMessage.
//
// Wiring that belongs to app_mod_runtime.inc (the coordinator's): call
// register_mod_gameplay_api(registry, host) from register_mod_apis; the
// service then starts with the server VM (explicitly or on its first use) and
// stops through the runtime's teardown listener. The Network area attaches
// modding::gameplay::ModNetGameplaySync to mod_gameplay_->loopback().

class ModGameplayBackend final : public modding::gameplay::GameplayBackend
{
  public:
    explicit ModGameplayBackend(Impl &app) : app_{app}
    {
    }

    [[nodiscard]] std::int32_t world_seed() const override
    {
        return app_.world_seed_;
    }
    // (Integration) players.me() and item.used's aimed block.
    [[nodiscard]] std::optional<modding::gameplay::LocalView> local_view() const override
    {
        // The menus draw a world behind them too: only a world being played
        // has a player.
        if (app_.world_preview_ == nullptr || app_.mod_session_ == ModSessionKind::none)
            return std::nullopt;
        const world::PlayerState &state = app_.player_.state();
        modding::gameplay::LocalView view;
        view.eye = {state.position.x, state.position.y + world::PlayerSimulation::eye_height, state.position.z};
        view.look = to_mod(app_.player_.view_direction());
        view.yaw = state.yaw;
        view.pitch = state.pitch;
        if (state.target.has_value())
        {
            view.target = modding::gameplay::BlockPos{state.target->x, state.target->y, state.target->z};
            view.target_adjacent =
                modding::gameplay::BlockPos{state.target->place_x, state.target->place_y, state.target->place_z};
        }
        app_.mod_liquid_view(view); // (mod-liquid) app_mod_liquid.inc
        return view;
    }
    [[nodiscard]] bool modern_rules() const override
    {
        return app_.inventory_.profile() == InventoryProfile::modern;
    }
    [[nodiscard]] std::optional<std::uint8_t> local_player() const override
    {
        if (app_.world_preview_ == nullptr)
            return std::nullopt;
        return app_.online_gameplay_active() ? app_.online_local_player_gid() : std::uint8_t{};
    }
    // (fix) While the world loads (a mod generator's terrain too), the spawn
    // that ends the loading screen announces the player.
    [[nodiscard]] bool local_player_placed() const override
    {
        return app_.world_preview_ != nullptr && !app_.waiting_for_world_;
    }
    void players(std::vector<modding::gameplay::PlayerSnapshot> &out) const override
    {
        out.clear();
        const auto local = local_player();
        if (!local.has_value())
            return;
        const world::PlayerState &state = app_.player_.state();
        out.push_back({*local, app_.mod_gameplay_local_name(), true,
                       app_.player_life_ == PlayerLife::alive, to_mod(state.position), state.yaw,
                       state.health});
        if (!app_.online_gameplay_active())
            return;
        for (const auto &[gid, remote] : app_.online_remote_players_)
            if (gid != *local && remote.presence_announced && !remote.gamer_tag.empty())
            {
                // (Authority) The host's own health of the player, where it has one.
                const auto decided = app_.authority_host_ != nullptr ? app_.authority_host_->player(gid)
                                                                     : std::nullopt;
                out.push_back({gid, remote.gamer_tag, false,
                               decided.has_value() ? decided->alive : !remote.state.dead,
                               to_mod(remote.state.position), remote.state.yaw,
                               decided.has_value() ? decided->health : 100.0F});
            }
    }

    void local_set_health(float health) override
    {
        if (app_.player_life_ != PlayerLife::alive)
            return;
        if (health <= 0.0F)
        {
            // Death goes through the game's own path, without asking the
            // damage handlers again.
            app_.mod_damage_bypass_ = true;
            app_.apply_player_damage(app_.player_.state().health + 1.0F, app_.player_.state().position,
                                     modding::gameplay::DamageCause::script);
            app_.mod_damage_bypass_ = false;
            return;
        }
        app_.player_.state().health = std::min(health, 100.0F);
    }
    void local_damage(float amount, modding::Vec3 source,
                      modding::gameplay::DamageCause cause) override
    {
        app_.apply_player_damage(amount, to_world(source), cause);
    }
    void local_teleport(modding::Vec3 feet) override
    {
        if (app_.world_preview_ == nullptr)
            return;
        const world::Vec3 target = to_world(feet);
        app_.snap_render_interpolation();
        app_.player_.state().position = target;
        app_.world_preview_->recenter_on(app_.player_.state());
        const auto &region = app_.world_preview_->region();
        const int x = static_cast<int>(std::floor(target.x));
        const int y = static_cast<int>(std::floor(target.y));
        const int z = static_cast<int>(std::floor(target.z));
        const bool clear = !world::blocks_player(region.block(x, y, z)) &&
                           !world::blocks_player(region.block(x, y + 1, z));
        // Exactly where the script asked when the body fits there; otherwise
        // the column's safe spot, as every teleport of the game does.
        app_.land_relocated_player(clear ? target : world::stock_safe_start_location(region, target),
                                   {});
        app_.online_player_update_timer_ = 0.0F;
    }
    std::uint16_t local_give(modding::gameplay::StackView stack, bool drop_rest) override
    {
        const auto item = static_cast<InventoryItemId>(stack.item);
        if (!valid_inventory_item_id(item) || item == InventoryItemId::bare_hands || stack.count == 0U)
            return stack.count;
        InventoryStack given{item, stack.count};
        given.mod_item = stack.mod_item;
        const std::uint16_t remainder = app_.inventory_.add_stack(given);
        if (remainder != 0U && drop_rest && app_.world_preview_ != nullptr)
            app_.world_preview_->spawn_dropped_stack(given.with_count(remainder), app_.player_.state());
        app_.sync_selected_item();
        return drop_rest ? std::uint16_t{} : remainder;
    }
    std::uint32_t local_take(std::uint16_t item, std::uint32_t mod_item, std::uint32_t count) override
    {
        std::uint32_t taken{};
        for_each_slot([&](PlayerInventory::Slot &slot, std::uint16_t) {
            if (taken >= count || !slot.has_value() || static_cast<std::uint16_t>(slot->item) != item ||
                slot->mod_item != mod_item)
                return;
            const std::uint32_t here = std::min<std::uint32_t>(slot->count, count - taken);
            slot->count = static_cast<std::uint16_t>(slot->count - here);
            taken += here;
            if (slot->count == 0U)
                slot.reset();
        });
        if (taken != 0U)
            app_.sync_selected_item();
        return taken;
    }
    [[nodiscard]] std::uint32_t local_count(std::uint16_t item, std::uint32_t mod_item) const override
    {
        std::uint32_t total{};
        const_cast<ModGameplayBackend *>(this)->for_each_slot([&](PlayerInventory::Slot &slot, std::uint16_t) {
            if (slot.has_value() && static_cast<std::uint16_t>(slot->item) == item && slot->mod_item == mod_item)
                total += slot->count;
        });
        return total;
    }
    void local_clear_inventory() override
    {
        for_each_slot([](PlayerInventory::Slot &slot, std::uint16_t) { slot.reset(); });
        app_.held_stack_.reset();
        app_.sync_selected_item();
    }
    [[nodiscard]] std::optional<modding::gameplay::StackView> local_held() const override
    {
        const PlayerInventory::Slot &held = app_.inventory_.selected();
        if (!held.has_value())
            return std::nullopt;
        return modding::gameplay::StackView{static_cast<std::uint16_t>(held->item), held->count,
                                            held->mod_item};
    }
    void local_slots(std::vector<std::pair<std::uint16_t, modding::gameplay::StackView>> &out) const override
    {
        out.clear();
        const_cast<ModGameplayBackend *>(this)->for_each_slot([&](PlayerInventory::Slot &slot, std::uint16_t index) {
            if (slot.has_value())
                out.emplace_back(index, modding::gameplay::StackView{static_cast<std::uint16_t>(slot->item),
                                                                     slot->count, slot->mod_item});
        });
    }
    bool local_tag_slot(std::uint16_t index, std::uint16_t item, std::uint32_t mod_item) override
    {
        bool tagged{};
        for_each_slot([&](PlayerInventory::Slot &slot, std::uint16_t at) {
            if (at == index && slot.has_value() && static_cast<std::uint16_t>(slot->item) == item)
            {
                slot->mod_item = mod_item;
                tagged = true;
            }
        });
        return tagged;
    }
    void local_set_speed(float multiplier) override
    {
        app_.mod_speed_multiplier_ = std::clamp(multiplier, 0.0F, 4.0F);
    }

    [[nodiscard]] std::optional<std::uint8_t> block(modding::gameplay::BlockPos cell) const override
    {
        if (app_.world_preview_ == nullptr)
            return std::nullopt;
        const auto block = app_.world_preview_->resolved_block(cell.x, cell.y, cell.z);
        if (!block.has_value())
            return std::nullopt;
        return static_cast<std::uint8_t>(*block);
    }
    bool set_block(modding::gameplay::BlockPos cell, std::uint8_t block) override
    {
        if (app_.world_preview_ == nullptr)
            return false;
        // The same steps a guest's AlterBlockMessage takes on the host, then
        // the same publication as the local player's own edit.
        const auto type = static_cast<world::BlockType>(block);
        auto &region = app_.world_preview_->region();
        if (!app_.publish_local_block_edit(cell.x, cell.y, cell.z, type))
            return false;
        app_.world_preview_->remember_edit(cell.x, cell.y, cell.z, type);
        if (region.contains(cell.x, cell.y, cell.z))
        {
            region.set_block(cell.x, cell.y, cell.z, type);
            app_.world_preview_->rebuild_block_edit(cell.x, cell.y, cell.z);
        }
        app_.dirty_ = true;
        return true;
    }
    [[nodiscard]] int biome(int x, int z) const override
    {
        // mod-world: a mod world generator's biomes (app_mod_world.inc).
        if (app_.mod_world_terrain_ != nullptr && app_.world_preview_ != nullptr)
            return app_.mod_world_biome(x, z);
        if (!generator_.has_value() || generator_seed_ != app_.world_seed_ ||
            generator_modern_ != modern_rules())
        {
            generator_.emplace(app_.world_seed_,
                               modern_rules() ? world::TerrainProfile::modern : world::TerrainProfile::classic,
                               app_.game_mode_);
            generator_seed_ = app_.world_seed_;
            generator_modern_ = modern_rules();
        }
        return static_cast<int>(generator_->biome_at(x, z));
    }
    // mod-world: a mod block a world generator put in the cell.
    [[nodiscard]] std::string generated_block(modding::gameplay::BlockPos cell,
                                              bool before_change) const override
    {
        return app_.mod_world_generated_block(cell.x, cell.y, cell.z, before_change);
    }
    [[nodiscard]] std::optional<std::array<float, 2>> light(modding::gameplay::BlockPos cell) const override
    {
        if (app_.world_preview_ == nullptr)
            return std::nullopt;
        const auto &region = app_.world_preview_->region();
        if (!region.contains(cell.x, cell.y, cell.z))
            return std::nullopt;
        const world::SimpleLight light = world::simple_light_at(region, cell.x, cell.y, cell.z);
        return std::array<float, 2>{light.sun, light.torch};
    }

    bool set_crate_slot(modding::gameplay::BlockPos crate, std::size_t index,
                        std::optional<modding::gameplay::StackView> stack) override
    {
        if (index >= world_save_crate_slot_count)
            return false;
        PlayerInventory::Slot slot;
        if (stack.has_value())
        {
            const auto item = static_cast<InventoryItemId>(stack->item);
            if (!valid_inventory_item_id(item) || item == InventoryItemId::bare_hands)
                return false;
            slot = InventoryStack{item, stack->count};
            slot->mod_item = stack->mod_item;
        }
        const CrateKey key{crate.x, crate.y, crate.z};
        app_.crate_inventories_[key][index] = slot;
        app_.dirty_ = true;
        if (app_.online_gameplay_active() && !app_.publish_local_crate_item(key, index, slot))
            std::cerr << "mod.gameplay.crate-item.failure\n";
        return true;
    }
    bool tag_crate_slot(modding::gameplay::BlockPos crate, std::size_t index, std::uint16_t item,
                        std::uint32_t mod_item) override
    {
        const auto found = app_.crate_inventories_.find(CrateKey{crate.x, crate.y, crate.z});
        if (found == app_.crate_inventories_.end() || index >= found->second.size())
            return false;
        PlayerInventory::Slot &slot = found->second[index];
        if (!slot.has_value() || static_cast<std::uint16_t>(slot->item) != item)
            return false;
        slot->mod_item = mod_item;
        return true;
    }
    bool spawn_pickup(modding::Vec3 position, modding::gameplay::StackView stack) override
    {
        const auto item = static_cast<InventoryItemId>(stack.item);
        if (app_.world_preview_ == nullptr || !valid_inventory_item_id(item) ||
            item == InventoryItemId::bare_hands || stack.count == 0U)
            return false;
        InventoryStack dropped{item, stack.count};
        dropped.mod_item = stack.mod_item;
        // The enemy-loot path: published to the session like any drop.
        app_.world_preview_->spawn_dropped_stack_at(dropped, to_world(position), 1.5F);
        return true;
    }

    [[nodiscard]] double day() const override
    {
        return app_.day_night_.day();
    }
    void set_time_of_day(float time_of_day) override
    {
        app_.day_night_.set_time(time_of_day);
        if (app_.online_host_ != nullptr && app_.online_host_->running())
            app_.publish_host_time_of_day();
    }

    bool spawn_enemy(int type, modding::Vec3 position) override
    {
        if (type < 0 || type >= world::enemy_type_count || app_.difficulty_ == GameDifficulty::no_enemies)
            return false;
        return app_.spawn_local_enemy(type, to_world(position), 0.5F);
    }

    void chat(std::optional<std::uint8_t> gid, std::string_view text) override
    {
        const std::string line{text};
        if (!gid.has_value() && app_.online_gameplay_active())
        {
            net::PayloadError payload_error{};
            const auto payload = net::encode_chat_payload(line, payload_error);
            std::string error;
            if (!payload.has_value() ||
                !app_.publish_online_broadcast("BroadcastTextMessage", *payload,
                                               net::MessageDeliveryPolicy::reliable_ordered, error))
                std::cerr << "mod.gameplay.chat.failure=" << error << '\n';
        }
        app_.push_online_chat(line);
        if (!app_.online_gameplay_active())
            app_.caption(line, 4.0F);
    }

    [[nodiscard]] std::filesystem::path sidecar_directory() const override
    {
        // <Worlds>/<name>.mods beside <Worlds>/<name>.omw: the same folder the
        // runtime keeps mod storage in (app_mod_runtime.inc).
        if (app_.active_world_path_.empty())
            return {};
        std::filesystem::path directory = app_.active_world_path_;
        directory += ".mods";
        return directory;
    }

    // (Authority) Another player, on a host that decides its health and
    // checks its moves (app_mod_auth.inc); false everywhere else.
    bool authority_damage(std::uint8_t gid, float amount, modding::gameplay::DamageCause cause,
                          modding::Vec3 source) override
    {
        return app_.authority_mod_damage(gid, amount, cause, to_world(source));
    }
    bool authority_heal(std::uint8_t gid, float amount) override
    {
        return app_.authority_mod_heal(gid, amount);
    }
    bool authority_set_health(std::uint8_t gid, float health) override
    {
        return app_.authority_mod_set_health(gid, health);
    }
    void authority_teleport(std::uint8_t gid, modding::Vec3 feet) override
    {
        app_.authority_mod_teleport(gid, to_world(feet));
    }
    void authority_give(std::uint8_t gid, std::uint16_t item, std::uint32_t count) override
    {
        app_.authority_mod_give(gid, item, count);
    }

    // mod-ai: creature AI (app_mod_ai.inc).
    [[nodiscard]] bool creature_block_damage_allowed(modding::gameplay::BlockPos cell) const override
    {
        return app_.mod_ai_block_damage_allowed(cell);
    }
    [[nodiscard]] bool projectile_block_breaking_allowed(modding::gameplay::BlockPos cell) const override
    {
        return app_.mod_ai_block_breaking_allowed(cell);
    }
    [[nodiscard]] std::optional<std::optional<float>> projectile_hits_player(std::uint8_t gid, modding::Vec3 tail,
                                                                             modding::Vec3 head) const override
    {
        return app_.mod_ai_projectile_hits_player(gid, to_world(tail), to_world(head));
    }
    [[nodiscard]] bool player_may_act(std::uint8_t gid) const override
    {
        return app_.mod_ai_player_may_act(gid);
    }

    [[nodiscard]] static modding::Vec3 to_mod(world::Vec3 value) noexcept
    {
        return {value.x, value.y, value.z};
    }
    [[nodiscard]] static world::Vec3 to_world(modding::Vec3 value) noexcept
    {
        return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
    }

  private:
    // Backpack slots 0..31, then each tray's slots: a stable index for a
    // stack's mod identity across saves and sessions.
    template <typename Visit>
    void for_each_slot(Visit &&visit)
    {
        std::uint16_t index{};
        for (PlayerInventory::Slot &slot : app_.inventory_.storage())
            visit(slot, index++);
        for (PlayerInventory::Tray &tray : app_.inventory_.trays())
            for (PlayerInventory::Slot &slot : tray)
                visit(slot, index++);
    }

    Impl &app_;
    mutable std::optional<world::WorldGenerator> generator_;
    mutable std::int32_t generator_seed_{};
    mutable bool generator_modern_{};
};

// The local player's replicated mod stats, as the UI's stat-bound bars read
// them (modding::ui::StatSource): value and range from the stat's minimum.
class ModGameplayStats final : public modding::ui::StatSource
{
  public:
    explicit ModGameplayStats(Impl &app) : app_{app}
    {
    }
    [[nodiscard]] std::optional<modding::ui::StatValue> stat(std::string_view name) const override
    {
        if (app_.mod_gameplay_ == nullptr || !app_.mod_gameplay_->client_running())
            return std::nullopt;
        for (const modding::gameplay::ClientStat &stat : app_.mod_gameplay_->client_stats())
            if (stat.name == name)
                return modding::ui::StatValue{stat.value - stat.minimum, stat.maximum - stat.minimum};
        return std::nullopt;
    }

  private:
    Impl &app_;
};

std::unique_ptr<ModGameplayBackend> mod_gameplay_backend_;
std::unique_ptr<ModGameplayStats> mod_gameplay_stats_;
std::unique_ptr<modding::gameplay::GameplayService> mod_gameplay_;
// A mod's speed effect on this process's player (1 = none).
float mod_speed_multiplier_{1.0F};
// Set while the service itself kills the player (players.set_health(p, 0)).
bool mod_damage_bypass_{};
// What the use button held when a placement began (for block.placed).
modding::gameplay::StackView mod_place_held_{};

void register_mod_gameplay_api(modding::ApiRegistry &registry, modding::ScriptHost &host)
{
    mod_gameplay_backend_ = std::make_unique<ModGameplayBackend>(*this);
    mod_gameplay_ = modding::register_gameplay_api(registry, host, *mod_gameplay_backend_);
    // The UI's stat bars read the replicated stats (app_mod_ui.inc).
    mod_gameplay_stats_ = std::make_unique<ModGameplayStats>(*this);
    mod_ui_service().set_stats(mod_gameplay_stats_.get());
    // (Integration) Graphics follows players and mod creatures by the same
    // handles scripts hold (lights, models and effects attached to them).
    if (!mod_graphics_locator_)
        mod_graphics_locator_ = [this](modding::Handle target, modding::graphics::Vec3 &position,
                                       float &yaw) { return mod_gameplay_locate(target, position, yaw); };
}

// Where a player or mod entity handle is now (feet) and which way it faces.
[[nodiscard]] bool mod_gameplay_locate(modding::Handle target, modding::graphics::Vec3 &position,
                                       float &yaw) const
{
    if (target.kind == modding::HandleKind::entity)
    {
        if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
            return false;
        for (const modding::gameplay::ClientEntity &entity : mod_gameplay_->client_entities())
            if (entity.id == target.id)
            {
                position = {static_cast<float>(entity.position.x), static_cast<float>(entity.position.y),
                            static_cast<float>(entity.position.z)};
                yaw = entity.yaw;
                return true;
            }
        return false;
    }
    const std::optional<std::uint8_t> gid = mod_net_player_gid(target);
    if (!gid.has_value() || world_preview_ == nullptr)
        return false;
    const std::optional<std::uint8_t> local =
        mod_gameplay_backend_ != nullptr ? mod_gameplay_backend_->local_player() : std::nullopt;
    if (local.has_value() && *gid == *local)
    {
        const world::PlayerState &state = player_.state();
        position = {state.position.x, state.position.y, state.position.z};
        yaw = state.yaw;
        return true;
    }
    const auto remote = online_remote_players_.find(*gid);
    if (remote == online_remote_players_.end() || !remote->second.presence_announced)
        return false;
    position = {remote->second.state.position.x, remote->second.state.position.y,
                remote->second.state.position.z};
    yaw = remote->second.state.yaw;
    return true;
}

// A server or client half is running. Every hook below starts with this.
[[nodiscard]] bool mod_gameplay_live() const noexcept
{
    return mod_gameplay_ != nullptr && mod_gameplay_->running();
}

[[nodiscard]] std::string mod_gameplay_local_name() const
{
    return online_gameplay_active() ? local_online_gamer_tag() : profile_name_;
}

// (fix, wave 5) The name under the crosshair: a mod block's own - placed, or
// grown by a mod's world generator - instead of its retail fallback's ("Rock"
// for a stone oven). Retail's name whenever no client half of a mod runs.
[[nodiscard]] std::string mod_target_block_name(const world::VoxelRaycastHit &target) const
{
    const std::string_view retail_name = world::block_name(target.block);
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
        return std::string{retail_name};
    const auto block = static_cast<std::uint8_t>(target.block);
    if (const modding::gameplay::ClientBlockType *placed =
            mod_gameplay_->client_block_at({target.x, target.y, target.z}, block);
        placed != nullptr && !placed->name.empty())
        return placed->name;
    const std::string generated = mod_world_generated_block(target.x, target.y, target.z, false);
    if (!generated.empty())
    {
        for (const modding::gameplay::ClientBlockType &type : mod_gameplay_->client_block_types())
            if (type.key == generated && type.fallback == block && !type.name.empty())
                return type.name;
        if (const auto defined = mod_gameplay_->client_defined_block(generated);
            defined.has_value() && defined->fallback == block && !defined->name.empty())
            return defined->name;
    }
    return std::string{retail_name};
}

[[nodiscard]] modding::gameplay::StackView mod_gameplay_held() const
{
    const PlayerInventory::Slot &held = inventory_.selected();
    if (!held.has_value())
        return {};
    return {static_cast<std::uint16_t>(held->item), held->count, held->mod_item};
}

[[nodiscard]] static modding::Vec3 mod_vec(world::Vec3 value) noexcept
{
    return ModGameplayBackend::to_mod(value);
}

// The fixed simulation step, beside tick_mods. Starts the halves the runtime
// has VMs for (a session whose wiring never called start_*), then steps them.
void mod_gameplay_tick(float seconds)
{
    if (mod_gameplay_ == nullptr || !mod_runtime_)
        return;
    if (!mod_gameplay_->server_running() && mod_runtime_->running(modding::Side::server))
        mod_gameplay_->start_server();
    // (Integration) Only beside the mod channel: client-scope mods alone keep
    // every gameplay hook inert.
    if (!mod_gameplay_->client_running() && mod_gameplay_client_wanted())
        mod_gameplay_->start_client();
    if (mod_gameplay_->running())
        mod_gameplay_->tick(seconds);
}

// With every world save, beside persist_mods.
void mod_gameplay_save()
{
    if (mod_gameplay_live())
        mod_gameplay_->save();
}

// apply_player_damage, before anything is applied: the damage to apply (0 =
// none). Only the player this process simulates comes through here.
[[nodiscard]] float mod_gameplay_damage(float amount, world::Vec3 source,
                                        modding::gameplay::DamageCause cause)
{
    if (!mod_gameplay_live() || mod_damage_bypass_)
        return amount;
    return mod_gameplay_->local_player_damaging(amount, cause, mod_vec(source));
}

void mod_gameplay_died(modding::gameplay::DamageCause cause)
{
    if (mod_gameplay_live())
        mod_gameplay_->local_player_died(mod_vec(player_.state().position), cause);
}

void mod_gameplay_respawned()
{
    if (mod_gameplay_live())
        mod_gameplay_->local_player_respawned(mod_vec(player_.state().position));
}

void mod_gameplay_spawned()
{
    if (mod_gameplay_live())
        mod_gameplay_->local_player_spawned(mod_vec(player_.state().position));
}

// A dig completed (update_breaking already emptied the cells). False: a mod
// kept the block, and the cells are put back before anything was published.
[[nodiscard]] bool mod_gameplay_breaking(const world::BlockBreakUpdate &update)
{
    if (!mod_gameplay_live() || world_preview_ == nullptr)
        return true;
    if (mod_gameplay_->local_block_breaking({update.x, update.y, update.z},
                                           static_cast<std::uint8_t>(update.block)))
        return true;
    std::vector<std::array<int, 3>> restored;
    std::vector<world::BlockType> previous;
    for (const world::BlockRemovalEffect &effect : update.removed_blocks)
    {
        world_preview_->region().set_block(effect.x, effect.y, effect.z, effect.block);
        restored.push_back({effect.x, effect.y, effect.z});
        previous.push_back(world::BlockType::empty);
    }
    world_preview_->rebuild_block_edits(restored, previous);
    player_.refresh_target(world_preview_->region());
    return false;
}

void mod_gameplay_broken(const world::BlockBreakUpdate &update)
{
    if (mod_gameplay_live())
        mod_gameplay_->local_block_broken({update.x, update.y, update.z},
                                          static_cast<std::uint8_t>(update.block));
}

// Cells this process's explosives removed (after publication).
void mod_gameplay_blasted(std::span<const std::array<int, 3>> cells,
                          std::span<const world::BlockType> previous)
{
    if (!mod_gameplay_live() || cells.empty())
        return;
    std::vector<modding::gameplay::BlockPos> positions;
    std::vector<std::uint8_t> blocks;
    positions.reserve(cells.size());
    blocks.reserve(previous.size());
    for (const std::array<int, 3> &cell : cells)
        positions.push_back({cell[0], cell[1], cell[2]});
    for (const world::BlockType block : previous)
        blocks.push_back(static_cast<std::uint8_t>(block));
    mod_gameplay_->local_blocks_blasted(positions, blocks);
}

// Whether the game drops its own pickup for a dug cell (a mod block's own
// drops replace it).
[[nodiscard]] bool mod_gameplay_retail_drop(const world::BlockRemovalEffect &effect)
{
    if (!mod_gameplay_live())
        return true;
    return mod_gameplay_->local_block_drops({effect.x, effect.y, effect.z},
                                            static_cast<std::uint8_t>(effect.block));
}

[[nodiscard]] bool mod_gameplay_placing(int x, int y, int z, world::BlockType block)
{
    if (!mod_gameplay_live())
        return true;
    mod_place_held_ = mod_gameplay_held();
    return mod_gameplay_->local_block_placing({x, y, z}, static_cast<std::uint8_t>(block),
                                              mod_place_held_);
}

void mod_gameplay_placed(int x, int y, int z, world::BlockType block)
{
    if (mod_gameplay_live())
        mod_gameplay_->local_block_placed({x, y, z}, static_cast<std::uint8_t>(block), mod_place_held_);
}

// The use button, before the held item's own action. False: skip it (a mod
// cancelled it, or a mod item's own use ran). Mining tools and blocks are
// not "used" here: digs and placements have their own events.
[[nodiscard]] bool mod_gameplay_using()
{
    if (!mod_gameplay_live())
        return true;
    const modding::gameplay::StackView held = mod_gameplay_held();
    const auto item = static_cast<InventoryItemId>(held.item);
    if (held.mod_item == 0U && (held.count == 0U || item_uses_tool_input(item) || is_block_item(item)))
        return true;
    if (mod_gameplay_->local_item_using(held))
        return true;
    // (Items) The mod item's own use ran: the body plays the use clip.
    mod_items_used(held.mod_item);
    return false;
}

void mod_gameplay_crafted(const CraftingRecipe &recipe, std::uint32_t crafted)
{
    if (mod_gameplay_live() && crafted != 0U)
        mod_gameplay_->local_item_crafted(static_cast<std::uint16_t>(recipe.result), crafted, 0U);
}

// --- (Integration) mod item identity beside stock crate and pickup messages ---
// Stock messages carry the retail fallback; the identity rides the mod
// channel (src/modding/gameplay/containers.cpp). Inert without a server mod.

[[nodiscard]] static modding::gameplay::StackView mod_stack_view(const PlayerInventory::Slot &slot) noexcept
{
    if (!slot.has_value())
        return {};
    return {static_cast<std::uint16_t>(slot->item), slot->count, slot->mod_item};
}

// A crate slot a stock ItemCrateMessage just set on this peer.
void mod_gameplay_crate_received(const CrateKey &crate, std::size_t index)
{
    if (!mod_gameplay_live())
        return;
    const auto found = crate_inventories_.find(crate);
    if (found == crate_inventories_.end() || index >= found->second.size() || !found->second[index].has_value())
        return;
    InventoryStack &stack = *found->second[index];
    stack.mod_item = mod_gameplay_->crate_slot_identity({crate[0], crate[1], crate[2]}, index,
                                                        static_cast<std::uint16_t>(stack.item));
}

// A crate opens: every untagged stack in it that the session knows to be a
// mod item gets its identity (contents that came another way than an
// ItemCrateMessage, e.g. a joiner's world snapshot).
void mod_gameplay_crate_opened(const CrateKey &crate)
{
    if (!mod_gameplay_live())
        return;
    const auto found = crate_inventories_.find(crate);
    if (found == crate_inventories_.end())
        return;
    for (std::size_t index{}; index < found->second.size(); ++index)
        if (PlayerInventory::Slot &slot = found->second[index]; slot.has_value() && slot->mod_item == 0U)
            slot->mod_item = mod_gameplay_->crate_slot_identity({crate[0], crate[1], crate[2]}, index,
                                                                static_cast<std::uint16_t>(slot->item));
}

// The local player changed a crate (publish_crate_differences): every slot
// that held or holds a mod item is reported, even when the stock payloads
// match (a mod item swapped for its plain fallback).
void mod_gameplay_crate_changed(const CrateKey &crate, const CrateSlots &before)
{
    if (!mod_gameplay_live())
        return;
    const auto found = crate_inventories_.find(crate);
    if (found == crate_inventories_.end())
        return;
    for (std::size_t index{}; index < found->second.size() && index < before.size(); ++index)
    {
        const modding::gameplay::StackView was = mod_stack_view(before[index]);
        const modding::gameplay::StackView now = mod_stack_view(found->second[index]);
        if ((was.mod_item == 0U && now.mod_item == 0U) ||
            (was.item == now.item && was.count == now.count && was.mod_item == now.mod_item))
            continue;
        mod_gameplay_->local_crate_slot({crate[0], crate[1], crate[2]}, index, now);
    }
}

void mod_gameplay_crate_gone(const CrateKey &crate)
{
    if (mod_gameplay_live())
        mod_gameplay_->crate_destroyed({crate[0], crate[1], crate[2]});
}

// This peer published a CreatePickupMessage for `stack`.
void mod_gameplay_pickup_published(std::uint8_t spawner_gid, std::int32_t pickup_id, const InventoryStack &stack)
{
    if (mod_gameplay_live() && stack.mod_item != 0U)
        mod_gameplay_->local_pickup_published(spawner_gid, pickup_id,
                                              {static_cast<std::uint16_t>(stack.item), stack.count, stack.mod_item});
}

// A pickup this peer is credited with: its mod identity, when it had one.
void mod_gameplay_pickup_identity(InventoryStack &stack, std::int32_t spawner_id, std::int32_t pickup_id)
{
    if (!mod_gameplay_live() || stack.mod_item != 0U || spawner_id < 0 || spawner_id > 255)
        return;
    stack.mod_item = mod_gameplay_->take_pickup_identity(static_cast<std::uint8_t>(spawner_id), pickup_id,
                                                         static_cast<std::uint16_t>(stack.item));
}

// A retail recipe a mod switched off for this session (recipes.remove).
[[nodiscard]] bool mod_gameplay_recipe_disabled(const CraftingRecipe &recipe) const
{
    if (!mod_gameplay_live() || recipe.mod_recipe != 0U)
        return false;
    // (Polish) From the retail book, or the screens' copy of it that the mod
    // recipes follow (mod_gameplay_recipe_book): the same indices.
    const std::vector<CraftingRecipe> *books[]{&crafting_recipes(inventory_.profile()), &mod_recipe_book_};
    for (const std::vector<CraftingRecipe> *book : books)
        if (!book->empty() && &recipe >= &book->front() && &recipe <= &book->back())
            return mod_gameplay_->client_recipe_disabled(static_cast<std::size_t>(&recipe - &book->front()));
    return false;
}

// ---- (Polish) Mod recipes on the crafting screens -------------------------
//
// recipes.add recipes join the retail cookbook the screens list (Classic's
// strip, Modern's tree, Remastered's tabs), filed under the tab of their
// result's retail fallback. The server crafts them (client_craft): the screen
// sends the request and the items arrive from the server, as recipes.request
// does.

mutable std::vector<CraftingRecipe> mod_recipe_book_;
mutable std::uint64_t mod_recipe_book_revision_{};
mutable InventoryProfile mod_recipe_book_profile_{InventoryProfile::classic};

// The cookbook the screens list: crafting_recipes(profile) itself without
// mod recipes, so a retail session lists exactly what it always did. Within a
// session it only grows: a screen holds cookbook indices, so a recipe the
// server removes stays in its place, unlisted (RecipeType::advanced).
[[nodiscard]] const std::vector<CraftingRecipe> &mod_gameplay_recipe_book() const
{
    const InventoryProfile profile = inventory_.profile();
    const auto &retail = crafting_recipes(profile);
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
    {
        mod_recipe_book_.clear();
        return retail;
    }
    if (mod_recipe_book_.empty() && mod_gameplay_->client_recipes().empty())
        return retail;
    const std::uint64_t revision = mod_gameplay_->client_revision(modding::gameplay::ClientChange::registry);
    if (!mod_recipe_book_.empty() && mod_recipe_book_revision_ == revision && mod_recipe_book_profile_ == profile)
        return mod_recipe_book_;
    if (mod_recipe_book_.size() < retail.size() || mod_recipe_book_profile_ != profile)
        mod_recipe_book_ = retail;
    for (std::size_t index = retail.size(); index < mod_recipe_book_.size(); ++index)
        mod_recipe_book_[index].type = RecipeType::advanced; // until the registry still has it
    mod_recipe_book_revision_ = revision;
    mod_recipe_book_profile_ = profile;
    const auto place = [&](const CraftingRecipe &recipe) {
        const auto first_mod = mod_recipe_book_.begin() + static_cast<std::ptrdiff_t>(retail.size());
        const auto same = std::find_if(first_mod, mod_recipe_book_.end(), [&](const CraftingRecipe &held) {
            return held.mod_recipe == recipe.mod_recipe;
        });
        if (same != mod_recipe_book_.end())
            *same = recipe;
        else
            mod_recipe_book_.push_back(recipe);
    };
    // A recipe names retail items by name and mod items by key.
    const auto resolve = [&](const std::string &name) -> std::optional<std::pair<InventoryItemId, std::uint32_t>> {
        for (const modding::gameplay::ClientItemType &type : mod_gameplay_->client_item_types())
            if (type.key == name)
                return std::pair{static_cast<InventoryItemId>(type.fallback), type.id};
        if (const auto retail_item = modding::gameplay::retail_item_from_name(name))
            return std::pair{static_cast<InventoryItemId>(*retail_item), 0U};
        return std::nullopt;
    };
    std::size_t too_long{};
    for (const modding::gameplay::ClientRecipe &source : mod_gameplay_->client_recipes())
    {
        // The retail layouts have four ingredient places.
        if (source.ingredients.size() > 4U)
        {
            ++too_long;
            continue;
        }
        const auto result = resolve(source.result);
        if (!result || result->first == InventoryItemId::bare_hands)
            continue;
        CraftingRecipe recipe;
        recipe.result = result->first;
        recipe.result_mod_item = result->second;
        recipe.result_count = std::max<std::uint16_t>(source.count, 1U);
        recipe.mod_recipe = source.id;
        bool known = true;
        for (std::size_t index{}; index < source.ingredients.size(); ++index)
        {
            const auto part = resolve(source.ingredients[index].first);
            known = known && part.has_value();
            if (part)
                recipe.ingredients[index] = {part->first, source.ingredients[index].second, part->second};
        }
        if (!known)
            continue;
        CraftingRecipe kind;
        kind.result = recipe.result;
        recipe.type = screens::recipe_type_for(kind, InventoryProfile::classic);
        place(recipe);
    }
    if (too_long != 0U && mod_runtime_ != nullptr)
        mod_runtime_->log({}, modding::LogLevel::warning,
                          std::format("{} mod recipe(s) have more than four ingredients: recipes.request crafts "
                                      "them, the crafting screens do not list them",
                                      too_long));
    return mod_recipe_book_;
}

// What the player has of an ingredient. A mod item counts only its own
// stacks, in every slot, as the server counts them.
[[nodiscard]] std::uint32_t crafting_ingredient_count(const CraftingIngredient &ingredient) const
{
    if (ingredient.mod_item == 0U)
        return inventory_.count(ingredient.item);
    std::uint32_t total{};
    const auto add = [&](const auto &slots) {
        for (const PlayerInventory::Slot &slot : slots)
            if (slot.has_value() && slot->item == ingredient.item && slot->mod_item == ingredient.mod_item)
                total += slot->count;
    };
    add(inventory_.storage());
    for (const PlayerInventory::Tray &tray : inventory_.trays())
        add(tray);
    return total;
}

[[nodiscard]] std::uint32_t mod_gameplay_craftable_times(const CraftingRecipe &recipe) const
{
    std::uint32_t times = std::numeric_limits<std::uint32_t>::max();
    for (const CraftingIngredient &ingredient : recipe.ingredients)
        if (ingredient.count != 0U)
            times = std::min(times, crafting_ingredient_count(ingredient) / ingredient.count);
    return times == std::numeric_limits<std::uint32_t>::max() ? 0U : times;
}

// A mod recipe asked for from a crafting screen: once, or (`all`) once for
// every time the inventory makes it now; the server checks each request.
void mod_gameplay_craft(const CraftingRecipe &recipe, bool all, bool announce)
{
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
        return;
    const std::uint32_t times = all ? std::clamp(mod_gameplay_craftable_times(recipe), 1U, 64U) : 1U;
    for (std::uint32_t time{}; time < times; ++time)
        mod_gameplay_->client_craft(recipe.mod_recipe);
    caption("[ ITEM CRAFTED ]", 1.2F);
    if (announce)
    {
        sound_bank_->play_cue("craft");
        toast("CRAFTED", std::format("{} ×{}", mod_item_label(recipe_result_stack(recipe), item_name(recipe.result)),
                                     recipe.result_count * times));
    }
}

// The local chat line, before it is sent. True: a mod command or a cancelled
// line, so it must not go to the session (or the mod console).
[[nodiscard]] bool mod_gameplay_chat(std::string_view line)
{
    if (!mod_gameplay_live())
        return false;
    const modding::gameplay::ChatVerdict verdict = mod_gameplay_->local_chat(line);
    return verdict != modding::gameplay::ChatVerdict::pass;
}

// Before an EnemyManager spawn of this process's own. True: a mod refused it
// (the attempt is spent, as when the spot is over crash-site rock).
[[nodiscard]] bool mod_gameplay_enemy_vetoed(int type, world::Vec3 position)
{
    // mod-ai: the session's retail enemy rules first (app_mod_ai.inc); a
    // joined client holds its own until the host's are in.
    if (mod_ai_joined_waiting())
        return true;
    return mod_gameplay_live() && (mod_ai_retail_refuses(type, position, false, true) ||
                                   !mod_gameplay_->local_enemy_spawning(type, mod_vec(position)));
}

// Every enemy this peer creates, its own or announced by another peer: the
// replicated difficulty scales its health, and the server hears of it.
void mod_gameplay_enemy_appeared(world::Enemy &enemy, std::optional<std::uint8_t> remote_sender)
{
    if (!mod_gameplay_live())
        return;
    const modding::gameplay::DifficultyScaling &scaling = mod_gameplay_->client_difficulty();
    if (scaling.active)
        enemy.health *= scaling.health_for(enemy.type);
    if (remote_sender.has_value())
        mod_gameplay_->remote_enemy_spawned(*remote_sender, enemy.type, enemy.id, mod_vec(enemy.position));
    else
        mod_gameplay_->local_enemy_spawned(enemy.type, enemy.id, enemy.target_gid, mod_vec(enemy.position));
}

void mod_gameplay_enemy_killed(world::EnemyIdentity identity)
{
    if (!mod_gameplay_live())
        return;
    if (const auto index = enemy_horde_.find_enemy(identity))
    {
        const world::Enemy &enemy = enemy_horde_.enemies()[*index];
        mod_gameplay_->local_enemy_killed(enemy.type, enemy.id, enemy.target_gid, mod_vec(enemy.position));
    }
}

// Damage an enemy deals to this process's player, scaled by the replicated
// difficulty. Exactly `damage` without a mod.
[[nodiscard]] float mod_scaled_enemy_damage(float damage) const
{
    if (!mod_gameplay_live() || !mod_gameplay_->client_difficulty().active)
        return damage;
    return damage * mod_gameplay_->client_difficulty().enemy_damage;
}

// The spawn clocks' step, scaled by the replicated difficulty. Exactly
// `seconds` without a mod.
[[nodiscard]] float mod_scaled_spawn_step(float seconds) const
{
    if (!mod_gameplay_live() || !mod_gameplay_->client_difficulty().active)
        return seconds;
    return seconds * mod_gameplay_->client_difficulty().spawn_rate;
}

// A swing of the held tool: a mod entity in reach (nearer than the retail
// target) takes it. True when one did.
[[nodiscard]] bool mod_gameplay_melee()
{
    if (!mod_gameplay_live())
        return false;
    const world::Vec3 eye{player_.state().position.x,
                          player_.state().position.y + world::PlayerSimulation::eye_height,
                          player_.state().position.z};
    // (Items) A mod weapon reaches its own distance.
    float reach = held_mod_weapon() != nullptr ? held_melee_reach() : world::PlayerSimulation::interaction_reach;
    if (player_.state().target.has_value())
        reach = std::min(reach, player_.state().target->distance);
    if (const world::EnemyHorde::RayHit enemy = melee_target(); enemy)
        reach = std::min(reach, enemy.distance);
    const InventoryItemId item = inventory_.selected_item();
    if (!world::one_shot_expired(melee_cooldown_))
        return false;
    // (Items) A mod weapon: its damage and rate, and the host is told which
    // weapon struck.
    const modding::gameplay::StackView held = mod_gameplay_held();
    const auto hit = mod_gameplay_->local_attack(mod_vec(eye), mod_vec(player_.view_direction()), reach,
                                                 held_melee_damage(item), static_cast<std::uint16_t>(item),
                                                 held_mod_weapon() != nullptr ? held.mod_item : 0U);
    if (!hit.has_value())
        return false;
    caption("[ CREATURE STRUCK ]", 0.7F);
    melee_cooldown_ = held_melee_cooldown(item);
    animation_use_pulse_ = true;
    (void)damage_selected_item();
    return true;
}

// One fired round (each pellet): mod entities on its line, up to the first
// solid block, are struck at once.
void mod_gameplay_gunshot(world::Vec3 origin, world::Vec3 aim, float damage, InventoryItemId item)
{
    if (!mod_gameplay_live() || world_preview_ == nullptr)
        return;
    float reach = 128.0F;
    if (const auto blocked = world::PlayerSimulation::raycast_blocking(world_preview_->region(), origin,
                                                                       aim, reach))
        reach = blocked->distance;
    static_cast<void>(mod_gameplay_->local_attack(mod_vec(origin), mod_vec(aim), reach, damage,
                                                  static_cast<std::uint16_t>(item)));
}

// The activate button: talking to an NPC comes before any block activation.
[[nodiscard]] bool mod_gameplay_interact()
{
    if (!mod_gameplay_live())
        return false;
    const world::Vec3 eye{player_.state().position.x,
                          player_.state().position.y + world::PlayerSimulation::eye_height,
                          player_.state().position.z};
    float reach = world::PlayerSimulation::interaction_reach;
    if (player_.state().target.has_value())
        reach = std::min(reach, player_.state().target->distance + 0.5F);
    return mod_gameplay_->local_interact(mod_vec(eye), mod_vec(player_.view_direction()), reach);
}

// handle_online_gameplay on the host, after authority and rate limits and
// before anything is applied or relayed: other players' digs, builds and chat
// for server mods. False: refused (the sender has been corrected; the caller
// must neither apply nor relay it).
[[nodiscard]] bool mod_gameplay_remote_message(const net::LegacySteamHostInbound &inbound,
                                               const net::DecodedPacket &decoded)
{
    if (mod_gameplay_ == nullptr || !mod_gameplay_->server_running())
        return true;
    const std::string_view name = decoded.message->name;
    if (name == "AlterBlockMessage")
    {
        net::AlterBlockPayload edit;
        net::PayloadError error{};
        if (!net::decode_alter_block_payload(decoded.payload, host_settings_.protocol, edit, error))
            return true; // the game's own handler refuses it
        const auto current = online_block_at(edit.block_location);
        if (mod_gameplay_->remote_block_edit(
                inbound.sender_gid, {edit.block_location.x, edit.block_location.y, edit.block_location.z},
                static_cast<std::uint8_t>(current.value_or(world::BlockType::empty)),
                static_cast<std::uint8_t>(edit.block_type)))
            return true;
        static_cast<void>(send_authoritative_block_correction(inbound.peer, edit.block_location));
        return false;
    }
    if (name == "RemoveBlocksMessage")
    {
        net::RemoveBlocksPayload removal;
        net::PayloadError error{};
        if (!net::decode_remove_blocks_payload(decoded.payload, removal, error) || removal.blocks.empty())
            return true;
        std::vector<modding::gameplay::BlockPos> cells;
        cells.reserve(removal.blocks.size());
        for (const net::IntVector3 &cell : removal.blocks)
            cells.push_back({cell.x, cell.y, cell.z});
        if (mod_gameplay_->remote_blocks_removed(inbound.sender_gid, cells))
            return true;
        correct_rejected_block_mutation(inbound.peer, decoded);
        return false;
    }
    if (name == "BroadcastTextMessage" || name == "ChatMessage")
    {
        std::string text;
        net::PayloadError error{};
        if (!net::decode_chat_payload(decoded.payload, text, error))
            return true;
        // Player chat is "<tag>: <line>"; the game's notices are not chat.
        const auto sender = online_remote_players_.find(inbound.sender_gid);
        if (sender == online_remote_players_.end())
            return true;
        for (const std::string *tag : {&sender->second.gamer_tag, &sender->second.self_reported_tag})
        {
            if (tag->empty() || !text.starts_with(*tag + ": "))
                continue;
            const std::string_view line = std::string_view{text}.substr(tag->size() + 2U);
            return mod_gameplay_->remote_chat(inbound.sender_gid, line) ==
                   modding::gameplay::ChatVerdict::pass;
        }
        return true;
    }
    if (name == "KillEnemyMessage")
    {
        net::KillEnemyPayload kill;
        net::PayloadError error{};
        if (!net::decode_kill_enemy_payload(decoded.payload, host_settings_.protocol, kill, error))
            return true;
        const world::EnemyIdentity identity{static_cast<std::uint8_t>(kill.target_id),
                                            static_cast<std::uint32_t>(kill.enemy_id)};
        if (const auto index = enemy_horde_.find_enemy(identity))
        {
            const world::Enemy &enemy = enemy_horde_.enemies()[*index];
            mod_gameplay_->remote_enemy_killed(static_cast<std::uint8_t>(kill.killer_id), enemy.type, enemy.id,
                                               enemy.target_gid, mod_vec(enemy.position));
        }
        return true;
    }
    return true;
}
