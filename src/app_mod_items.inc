// (Items) Member functions and state of Application::Impl: held and dropped
// mod items and mod weapons (openminerz/modding/held_item.hpp,
// src/modding/gameplay/items.cpp). Included from app.cpp inside the class
// body, after app_mod_auth.inc.
//
//  * How a mod item looks in 3D: its glTF model (items.define `model`, from
//    the graphics service's model cache) or, without one, its icon in solid
//    form (app_mod_graphics_items.inc), with its hold style and size - in the
//    local hand, on pickups and in every other player's hand.
//  * Each hand's item clips (equip, idle, use) and the eat/drink use.
//  * Which retail arm clips a hold style plays: the local animator's mode, and
//    so the mode PlayerUpdateMessage carries in a session with server mods
//    (every peer there is OpenMinerZ and runs the same mods).
//  * A held mod weapon's damage, reach and rate on the local swing, and the
//    host's authority judging other players' swings by it.
//
// Without a running mod client mod_item_type() is null and every function
// here returns at its first test: retail items, packets and frames are
// exactly what they are without mods.

struct ModItemLook final
{
    // What it was made from, and the asset stack's generation (+1).
    std::string source;
    std::uint64_t generation{};
    std::shared_ptr<const modding::graphics::ItemLook> look;
};
std::map<std::uint32_t, ModItemLook> mod_item_looks_;
std::set<std::string, std::less<>> mod_item_models_reported_;
// The local hand's item clips, the uses the local animator started
// (step_local_animation) and how many of them the hand has seen.
modding::graphics::HeldItemAnimator mod_item_hand_;
std::uint64_t mod_item_uses_{};
std::uint64_t mod_item_uses_seen_{};
float mod_item_hand_clock_{-1.0F};
float mod_item_last_use_{-1.0e9F};
// Other players' hands, by gid.
struct ModItemRemoteHand final
{
    modding::graphics::HeldItemAnimator animator;
    bool using_tool{};
    float clock{-1.0F};
};
std::map<std::uint8_t, ModItemRemoteHand> mod_item_remote_hands_;

// How a mod item looks in 3D, or nullptr (its fallback's model is drawn).
// Made again when the item's definition or the installed packs change; a
// model that does not load is reported once and its icon drawn instead.
[[nodiscard]] std::shared_ptr<const modding::graphics::ItemLook> mod_item_look(std::uint32_t mod_item)
{
    const modding::gameplay::ClientItemType *type = mod_item_type(mod_item);
    if (type == nullptr)
        return nullptr;
    const auto assets = modding::graphics::presentation_assets();
    const std::uint64_t generation = assets != nullptr ? assets->stack.generation() + 1U : 0U;
    std::string source = std::format("{}\n{}\n{}\n{}\n{}", type->key, type->model, type->icon,
                                     modding::gameplay::hold_style_name(type->hold), type->scale);
    if (const auto found = mod_item_looks_.find(mod_item); found != mod_item_looks_.end() &&
                                                           found->second.source == source &&
                                                           found->second.generation == generation)
        return found->second.look;
    ModItemLook &entry = mod_item_looks_[mod_item];
    entry.source = std::move(source);
    entry.generation = generation;
    entry.look.reset();
    modding::graphics::ItemLook look;
    look.hold = modding::graphics::resolved_hold(type->hold, static_cast<InventoryItemId>(type->fallback));
    look.scale = type->scale;
    if (!type->model.empty())
    {
        std::string error;
        modding::graphics::GraphicsService &service = mod_graphics();
        look.model = service.model_cache().get(service.assets(), type->model, &error);
        if (look.model == nullptr && mod_item_models_reported_.insert(entry.source).second)
            std::cerr << "Mod item " << type->key << ": its model \"" << type->model << "\" "
                      << (error.empty() ? std::string{"did not load"} : error) << " (drawing its icon)\n";
    }
    if (look.model == nullptr)
        if (const ModItemIcon *icon = mod_item_icon(mod_item))
            look.icon = icon->model;
    if (!look.empty())
        entry.look = std::make_shared<const modding::graphics::ItemLook>(std::move(look));
    return entry.look;
}

// The mod item type in the local hand, or nullptr.
[[nodiscard]] const modding::gameplay::ClientItemType *held_mod_item_type() const
{
    const PlayerInventory::Slot &held = inventory_.selected();
    return held.has_value() && held->mod_item != 0U ? mod_item_type(held->mod_item) : nullptr;
}

// The arm clips (Player._playerMode) for `item` in the local hand: a mod
// item's hold style, or the retail item's own.
[[nodiscard]] PlayerAnimationMode held_item_animation_mode(InventoryItemId item) const
{
    if (const modding::gameplay::ClientItemType *type = held_mod_item_type();
        type != nullptr && static_cast<InventoryItemId>(type->fallback) == item)
        return modding::graphics::hold_animation_mode(type->hold, item);
    return item_player_animation_mode(item);
}

// The mod weapon in the local hand (nullptr: the swing is the retail item's).
[[nodiscard]] const modding::gameplay::ItemWeapon *held_mod_weapon() const
{
    const modding::gameplay::ClientItemType *type = held_mod_item_type();
    return type != nullptr && type->weapon.has_value() ? &*type->weapon : nullptr;
}
[[nodiscard]] float held_melee_damage(InventoryItemId item) const
{
    const modding::gameplay::ItemWeapon *weapon = held_mod_weapon();
    return weapon != nullptr ? weapon->damage : item_enemy_damage(item, inventory_.profile());
}
[[nodiscard]] float held_melee_cooldown(InventoryItemId item) const
{
    const modding::gameplay::ItemWeapon *weapon = held_mod_weapon();
    return weapon != nullptr ? weapon->cooldown : item_use_cooldown_seconds(item);
}
// InGameHUD's ConstructionProbe is five blocks long; a mod weapon's reach.
[[nodiscard]] float held_melee_reach() const
{
    const modding::gameplay::ItemWeapon *weapon = held_mod_weapon();
    return weapon != nullptr ? weapon->reach : 5.0F;
}

// What a MeleePlayerMessage from `attacker` holding retail `item` does to
// this machine's player: the mod weapon's player damage when the attacker's
// hand holds one (the server's word on the host, the server's broadcast on a
// client), else retail's.
[[nodiscard]] float mod_items_melee_player_damage(std::uint8_t attacker, InventoryItemId item) const
{
    if (mod_gameplay_live())
    {
        if (mod_gameplay_->server_running())
        {
            if (const auto weapon = mod_gameplay_->held_weapon(attacker, static_cast<std::uint16_t>(item)))
                return weapon->player_damage;
        }
        else if (const modding::gameplay::ClientItemType *type =
                     mod_item_type(mod_gameplay_->client_held_item(attacker));
                 type != nullptr && type->weapon.has_value() &&
                 static_cast<InventoryItemId>(type->fallback) == item)
            return type->weapon->player_damage;
    }
    return item_player_melee_damage(item) * 100.0F;
}

// The authority's question (set_melee_weapon): the server mod's weapon in a
// swinging player's hand.
[[nodiscard]] std::optional<net::AuthorityHost::MeleeWeapon> mod_items_authority_weapon(std::uint8_t gid,
                                                                                        InventoryItemId item) const
{
    if (!mod_gameplay_live())
        return std::nullopt;
    const auto weapon = mod_gameplay_->held_weapon(gid, static_cast<std::uint16_t>(item));
    if (!weapon.has_value())
        return std::nullopt;
    return net::AuthorityHost::MeleeWeapon{weapon->player_damage, weapon->reach, weapon->cooldown};
}

// A use of the held item began (the animator's UsingTool pulse): the hand's
// use clip plays from the next frame.
void mod_items_note_use() noexcept
{
    ++mod_item_uses_;
}

// A mod item's own use ran (items.define `use`, mod_gameplay_using): the body
// plays its use clip - at most once per the item's cooldown - and so every
// other player sees it too.
void mod_items_used(std::uint32_t mod_item)
{
    const modding::gameplay::ClientItemType *type = mod_item_type(mod_item);
    if (type == nullptr || !type->usable || elapsed_ - mod_item_last_use_ < type->cooldown)
        return;
    mod_item_last_use_ = elapsed_;
    animation_use_pulse_ = true;
}

// How long an eat or drink use lasts on screen: the item's cooldown, within
// what reads as a bite or a sip.
[[nodiscard]] static float mod_item_use_seconds(const modding::gameplay::ClientItemType *type) noexcept
{
    return type != nullptr ? std::clamp(type->cooldown, 0.8F, 2.0F) : 1.0F;
}

// Before the world draws (mod_graphics_items_before_world_draw): the looks for
// pickups and the local hand's item this frame.
void mod_items_before_world_draw()
{
    const float seconds =
        mod_item_hand_clock_ < 0.0F ? 0.0F : std::clamp(elapsed_ - mod_item_hand_clock_, 0.0F, 0.25F);
    mod_item_hand_clock_ = elapsed_;
    mod_items_capture_aids();
    world_preview_->set_mod_item_models(
        [this](std::uint32_t mod_item) { return mod_item_look(mod_item); });
    const PlayerInventory::Slot &held = inventory_.selected();
    const std::uint32_t item = held.has_value() ? held->mod_item : 0U;
    const auto look = item != 0U ? mod_item_look(item) : nullptr;
    const bool used = mod_item_uses_ != mod_item_uses_seen_;
    mod_item_uses_seen_ = mod_item_uses_;
    if (look == nullptr)
    {
        mod_item_hand_.update(0U, {}, false, 1.0F, seconds);
        world_preview_->set_held_mod_item(nullptr);
        return;
    }
    mod_item_hand_.update(item, *look, used, mod_item_use_seconds(mod_item_type(item)), seconds);
    modding::graphics::HeldItemPose pose = mod_item_hand_.pose();
    if (options_.preview_viewmodel.has_value() && *options_.preview_viewmodel == "use")
    {
        // A posed capture (--viewmodel use): the item at the same point of
        // its use as the arm.
        const float progress = std::clamp(options_.preview_viewmodel_progress, 0.0F, 1.0F);
        pose.use_progress = progress;
        if (look->model != nullptr)
            if (const int clip = modding::graphics::find_state_clip(*look->model, "use"); clip >= 0)
            {
                pose.layers[0] = {clip, progress * look->model->animations[static_cast<std::size_t>(clip)].duration,
                                  false, 1.0F};
                pose.layer_count = 1U;
            }
    }
    world_preview_->set_held_mod_item(std::make_shared<const modding::graphics::HeldItemPose>(std::move(pose)));
}

// After refresh_online_remote_visuals: each other player's mod item, when the
// server says it holds one whose fallback is what its stock hand carries
// (an identity that has fallen behind the stock hand shows nothing of it).
void mod_items_remote_visuals(std::vector<RemotePlayerVisual> &visuals)
{
    if (!mod_gameplay_live() || !mod_gameplay_->client_running())
    {
        mod_item_remote_hands_.clear();
        return;
    }
    // Capture aid: OPENMINERZ_DIAGNOSTIC_REMOTE_MOD_ITEM=<mod:item> puts that
    // item in every --diagnostic-multiplayer player's hand.
    std::uint32_t diagnostic{};
    if (options_.diagnostic_multiplayer)
        if (const char *key = SDL_getenv("OPENMINERZ_DIAGNOSTIC_REMOTE_MOD_ITEM"); key != nullptr)
            for (const modding::gameplay::ClientItemType &type : mod_gameplay_->client_item_types())
                if (type.key == key)
                    diagnostic = type.id;
    for (RemotePlayerVisual &visual : visuals)
    {
        const std::uint32_t item = diagnostic != 0U ? diagnostic : mod_gameplay_->client_held_item(visual.gid);
        const modding::gameplay::ClientItemType *type = mod_item_type(item);
        if (type != nullptr && diagnostic != 0U)
        {
            // ... and the arm clips its hold plays, as a real player's
            // PlayerUpdateMessage would carry them.
            visual.carried_item = static_cast<InventoryItemId>(type->fallback);
            if (const auto remote = online_remote_players_.find(visual.gid); remote != online_remote_players_.end())
                remote->second.state.animation_mode =
                    modding::graphics::hold_animation_mode(type->hold, visual.carried_item);
        }
        const auto look = type != nullptr && static_cast<InventoryItemId>(type->fallback) == visual.carried_item
                              ? mod_item_look(item)
                              : nullptr;
        if (look == nullptr)
        {
            mod_item_remote_hands_.erase(visual.gid);
            continue;
        }
        ModItemRemoteHand &hand = mod_item_remote_hands_[visual.gid];
        const float seconds = hand.clock < 0.0F ? 0.0F : std::clamp(elapsed_ - hand.clock, 0.0F, 0.25F);
        hand.clock = elapsed_;
        const bool started = visual.using_tool && !hand.using_tool;
        hand.using_tool = visual.using_tool;
        hand.animator.update(item, *look, started, mod_item_use_seconds(type), seconds);
        visual.mod_item = std::make_shared<const modding::graphics::HeldItemPose>(hand.animator.pose());
    }
    std::erase_if(mod_item_remote_hands_, [&](const auto &entry) {
        return std::ranges::none_of(visuals, [&](const RemotePlayerVisual &visual) { return visual.gid == entry.first; });
    });
}

// The session's mod items are gone: drop every look and hand.
void mod_items_clear()
{
    mod_item_looks_.clear();
    mod_item_remote_hands_.clear();
    mod_item_hand_ = {};
    mod_item_hand_clock_ = -1.0F;
}

// Capture aids, each once the session's items are in, as --item and
// --pickup-item are for retail items: OPENMINERZ_DIAGNOSTIC_HOLD_MOD_ITEM=
// <mod:item> puts that item in the hand (the first tray slot, selected) and
// OPENMINERZ_DIAGNOSTIC_DROP_MOD_ITEM=<mod:item> drops one three blocks ahead.
// OPENMINERZ_DIAGNOSTIC_MOD_SWING=1 swings the held item at whatever is in
// reach every frame its cooldown allows (the press a player makes).
std::optional<std::string> mod_item_capture_hold_;
std::optional<std::string> mod_item_capture_drop_;
bool mod_item_capture_swing_{};
bool mod_item_capture_read_{};
void mod_items_capture_aids()
{
    if (!mod_item_capture_read_)
    {
        mod_item_capture_read_ = true;
        if (const char *key = SDL_getenv("OPENMINERZ_DIAGNOSTIC_HOLD_MOD_ITEM"))
            mod_item_capture_hold_ = key;
        if (const char *key = SDL_getenv("OPENMINERZ_DIAGNOSTIC_DROP_MOD_ITEM"))
            mod_item_capture_drop_ = key;
        mod_item_capture_swing_ = SDL_getenv("OPENMINERZ_DIAGNOSTIC_MOD_SWING") != nullptr;
    }
    if (mod_item_capture_swing_ && !mod_item_capture_hold_.has_value() && player_life_ == PlayerLife::alive)
        static_cast<void>(try_melee());
    if (!mod_item_capture_hold_.has_value() && !mod_item_capture_drop_.has_value())
        return;
    const auto stack_of = [&](const std::string &key) -> std::optional<InventoryStack> {
        for (const modding::gameplay::ClientItemType &type : mod_gameplay_->client_item_types())
            if (type.key == key)
            {
                InventoryStack stack{static_cast<InventoryItemId>(type.fallback), 1U};
                stack.mod_item = type.id;
                return stack;
            }
        return std::nullopt;
    };
    if (mod_item_capture_hold_.has_value())
        if (const auto stack = stack_of(*mod_item_capture_hold_))
        {
            mod_item_capture_hold_.reset();
            inventory_.select(0U);
            inventory_.tray()[0] = *stack;
            sync_selected_item();
        }
    if (mod_item_capture_drop_.has_value())
        if (const auto stack = stack_of(*mod_item_capture_drop_))
        {
            mod_item_capture_drop_.reset();
            // Ahead along the ground, out of the player's reach to collect it.
            const world::Vec3 look = player_.view_direction();
            const float level = std::max(0.001F, std::hypot(look.x, look.z));
            world_preview_->spawn_created_stack_at(
                *stack, {player_.state().position.x + look.x / level * 3.0F, player_.state().position.y + 1.2F,
                         player_.state().position.z + look.z / level * 3.0F});
        }
}
