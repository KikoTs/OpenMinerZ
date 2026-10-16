// mod-ai: member functions of Application::Impl for creature AI. Included
// inside the class body by app.cpp, after app_mod_auth.inc.
//
// Two halves, both inert without a server mod (mod_gameplay_live()):
//
//   rules     a server mod's retail enemy rules (enemies.rules, replicated by
//             the gameplay service to every peer) on each retail spawn path
//             of this peer - its surface, cave and alien spawns
//             (mod_gameplay_enemy_vetoed), spawner blocks, the spawns other
//             peers announce, dragon requests - and the distance retail picks
//             tiers by; with clear_existing the enemies already about go. A
//             joined client spawns none of its own until the host's rules
//             are in. Only in sessions no stock player can join: offline,
//             hosting with server mods, or joined over a mod channel.
//   on screen projectiles in flight (drawn with the mod creatures, their
//             trails), impacts, boss bars (the UI service draws them) and
//             boss music.

std::string mod_ai_music_;
// (fix) The trail effect of each projectile in flight that has one (an
// invalid handle: none could be played, so it is not asked again).
std::unordered_map<std::uint64_t, modding::Handle> mod_ai_trails_;

// Whether a server mod's retail rules can apply in this session at all.
[[nodiscard]] bool mod_ai_rules_session() const
{
    if (!mod_gameplay_live())
        return false;
    if (online_host_ != nullptr)
        return net::session_authoritative(host_settings_.mod_manifest);
    if (online_client_ != nullptr)
        return mod_net_client_ != nullptr && mod_net_server_ == nullptr && online_client_->mod_channel_open();
    return true; // an offline world
}

// (fix) A joined client of a session with server mods whose gameplay half is
// not running yet: it spawns no retail enemy of its own (and starts no
// spawner) until the host's rules are in. Once the half runs,
// GameplayService::retail_spawn_allowed holds them until the rules arrive.
[[nodiscard]] bool mod_ai_joined_waiting() const
{
    return online_client_ != nullptr && online_host_ == nullptr && online_client_->mod_channel_open() &&
           (mod_gameplay_ == nullptr || !mod_gameplay_->client_running());
}

// A retail enemy of this type may not appear here: its family or Hell (below
// y -40) is switched off; a spawner block's wave member also when spawner
// blocks are. `own`: this peer's own spawn (held on a joined client until
// the host's rules are in), not one another peer announced.
[[nodiscard]] bool mod_ai_retail_refuses(int type, world::Vec3 position, bool from_spawner, bool own) const
{
    if (!mod_ai_rules_session())
        return false;
    using modding::gameplay::RetailEnemyClass;
    const modding::Vec3 at = mod_vec(position);
    const auto allowed = [&](RetailEnemyClass family) {
        return own ? mod_gameplay_->retail_spawn_allowed(family, at) : mod_gameplay_->retail_enemy_allowed(family, at);
    };
    if (position.y < -40.0F && !allowed(RetailEnemyClass::hell))
        return true;
    if (from_spawner && !allowed(RetailEnemyClass::spawner))
        return true;
    return !allowed(modding::gameplay::retail_enemy_class_of(type));
}

[[nodiscard]] bool mod_ai_dragon_refused(world::Vec3 position) const
{
    if (!mod_ai_rules_session())
        return false;
    const modding::Vec3 at = mod_vec(position);
    return !mod_gameplay_->retail_enemy_allowed(modding::gameplay::RetailEnemyClass::dragon, at) ||
           (position.y < -40.0F && !mod_gameplay_->retail_enemy_allowed(modding::gameplay::RetailEnemyClass::hell, at));
}

[[nodiscard]] bool mod_ai_spawner_refused(net::IntVector3 location, bool hell) const
{
    if (mod_ai_joined_waiting())
        return true;
    if (!mod_ai_rules_session())
        return false;
    const modding::Vec3 at{location.x + 0.5, static_cast<double>(location.y), location.z + 0.5};
    return !mod_gameplay_->retail_spawn_allowed(modding::gameplay::RetailEnemyClass::spawner, at) ||
           ((hell || location.y < -40) &&
            !mod_gameplay_->retail_spawn_allowed(modding::gameplay::RetailEnemyClass::hell, at));
}

// (fix) A spawner block whose waves could only bring families the rules
// switch off stays silent: it does not start, gives nothing, stays dark.
[[nodiscard]] bool mod_ai_spawner_silent(net::IntVector3 location, const world::SpawnerDefinition &definition) const
{
    if (!mod_ai_rules_session())
        return false;
    const std::vector<std::uint8_t> types = world::spawner_wave_enemy_types(definition, location.y);
    // Where its waves appear (SpawnerSimulation: the cell above the block).
    return mod_gameplay_->retail_spawner_silent(types, {static_cast<double>(location.x),
                                                        static_cast<double>(location.y + 1),
                                                        static_cast<double>(location.z)});
}

// (fix) A member of this peer's own spawner wave whose family the rules
// switch off (spawner blocks themselves still on): it never appears, and
// counts as defeated so the wave can still end.
[[nodiscard]] bool mod_ai_spawner_member_refused(int type, world::Vec3 position) const
{
    return mod_ai_joined_waiting() || mod_ai_retail_refuses(type, position, true, true);
}

// (fix) enemies.rules{clear_existing = true}: the retail enemies about that
// the rules now switch off leave by retail's own give-up (no drops) - this
// peer announces its own (the host, everyone's, as a stock host may) and lets
// the others' go here; a dragon this peer flies departs. Every frame; nothing
// without such rules.
void mod_ai_clear_retail()
{
    if (mod_gameplay_ == nullptr || !mod_gameplay_->take_retail_clear() || !mod_ai_rules_session())
        return;
    const bool host_authoritative = online_host_ != nullptr && online_host_->running();
    const std::uint8_t local_gid = online_gameplay_active() ? online_local_player_gid() : std::uint8_t{};
    std::vector<world::EnemyIdentity> leaving;
    for (const world::Enemy &enemy : enemy_horde_.enemies())
        if (enemy.state != world::EnemyState::die && enemy.state != world::EnemyState::despawn &&
            enemy.state != world::EnemyState::give_up &&
            mod_ai_retail_refuses(enemy.type, enemy.position, enemy.spawner_sourced, false))
            leaving.push_back({enemy.target_gid, enemy.id});
    for (const world::EnemyIdentity identity : leaving)
    {
        if (!online_gameplay_active() || host_authoritative || identity.target_gid == local_gid)
            publish_local_enemy_target("EnemyGiveUpMessage", identity);
        else
            static_cast<void>(enemy_horde_.give_up_enemy(identity));
        remote_spawner_enemy_owners_.erase(identity);
        notify_local_spawner_enemy_removed(identity);
    }
    bool dragon_left{};
    if (dragon_.active() && (!online_gameplay_active() || dragon_.network_owner()) &&
        mod_ai_dragon_refused(dragon_.dragon().position))
    {
        publish_remove_dragon();
        dragon_.deactivate();
        note_dragon_removed();
        toast("DRAGON DEPARTS", "");
        dragon_left = true;
    }
    std::cout << "mod.ai.retail-clear enemies=" << leaving.size() << " dragon=" << dragon_left << '\n';
}

// The distance retail picks enemy tiers and dragons by: `distance` itself
// without rules.
[[nodiscard]] float mod_ai_tier_distance(float distance, world::Vec3 position) const
{
    if (!mod_ai_rules_session())
        return distance;
    return mod_gameplay_->retail_tier_distance(distance, mod_vec(position));
}

// A gunshot, explosion or other sound creatures may hear (the host's server
// half only; `gid` the player who made it).
void mod_ai_noise(world::Vec3 position, float loudness, std::string_view kind, std::optional<std::uint8_t> gid)
{
    if (!mod_gameplay_live() || !mod_gameplay_->server_running())
        return;
    mod_gameplay_->noise(mod_vec(position), loudness, kind, gid);
}

// This process's own player, as the gameplay service names it.
[[nodiscard]] std::optional<std::uint8_t> mod_ai_local_gid() const
{
    return mod_gameplay_backend_ != nullptr ? mod_gameplay_backend_->local_player() : std::nullopt;
}

// --- the gameplay backend's AI questions -------------------------------------------

// A creature's explosion digs blocks only where retail lets a dragon's
// fireball dig them (Endurance, Dragon Endurance, Hard, Hardcore) and never
// inside the host's spawn protection.
[[nodiscard]] bool mod_ai_block_damage_allowed(modding::gameplay::BlockPos cell) const
{
    return dragons_damage_structures(game_mode_, difficulty_) && mod_ai_block_breaking_allowed(cell);
}

// (fix) A projectile whose mod opted in (breaks_blocks) digs anywhere outside
// the host's spawn protection.
[[nodiscard]] bool mod_ai_block_breaking_allowed(modding::gameplay::BlockPos cell) const
{
    if (online_host_ != nullptr && host_settings_.spawn_protection_radius > 0)
    {
        const float dx = static_cast<float>(cell.x) + 0.5F - world::default_start_location.x;
        const float dz = static_cast<float>(cell.z) + 0.5F - world::default_start_location.z;
        if (std::hypot(dx, dz) <= static_cast<float>(host_settings_.spawn_protection_radius))
            return false;
    }
    return true;
}

// A projectile against a remote player: where the authority says the player
// was over its lag. The host's own player is tested by the service itself.
[[nodiscard]] std::optional<std::optional<float>> mod_ai_projectile_hits_player(std::uint8_t gid, world::Vec3 tail,
                                                                                world::Vec3 head) const
{
    if (authority_host_ == nullptr || !authority_host_->has_player(gid) || gid == online_local_player_gid())
        return std::nullopt;
    return authority_host_->tracer_hits_player(gid, tail, head, authority_now());
}

[[nodiscard]] bool mod_ai_player_may_act(std::uint8_t gid) const
{
    return authority_host_ == nullptr || !authority_host_->has_player(gid) || authority_host_->may_act(gid);
}

// --- on screen ---------------------------------------------------------------------

// Projectiles in flight join the creature pass: their model (or a small box)
// pointed along the flight.
void mod_ai_append_projectile_views(std::vector<modding::graphics::CreatureView> &views) const
{
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
        return;
    for (const modding::gameplay::ClientProjectile &projectile : mod_gameplay_->client_projectiles())
    {
        modding::graphics::CreatureView view;
        view.id = projectile.id | (1ULL << 62U); // never an entity's id
        view.kind = projectile.kind;
        view.model = projectile.model;
        view.texture = projectile.texture;
        view.state = "idle";
        const float size = std::max(0.08F, projectile.radius * 2.0F * projectile.scale);
        view.width = size;
        view.height = size;
        view.position = {static_cast<float>(projectile.position.x),
                         static_cast<float>(projectile.position.y) - (projectile.model.empty() ? size * 0.5F : 0.0F),
                         static_cast<float>(projectile.position.z)};
        view.yaw = projectile.yaw;
        view.pitch = projectile.pitch;
        view.scale = projectile.scale;
        views.push_back(std::move(view));
    }
}

// The boss bars of bosses within their range of this player, nearest first.
[[nodiscard]] std::vector<modding::ui::UiService::BossBar> mod_ai_boss_bars() const
{
    std::vector<modding::ui::UiService::BossBar> bars;
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running() || world_preview_ == nullptr)
        return bars;
    const world::Vec3 me = player_.state().position;
    std::vector<std::pair<double, const modding::gameplay::ClientBoss *>> near;
    for (const modding::gameplay::ClientBoss &boss : mod_gameplay_->client_bosses())
    {
        const double dx = boss.position.x - me.x;
        const double dy = boss.position.y - me.y;
        const double dz = boss.position.z - me.z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance <= boss.range)
            near.emplace_back(distance, &boss);
    }
    std::ranges::sort(near, {}, &std::pair<double, const modding::gameplay::ClientBoss *>::first);
    for (const auto &[distance, boss] : near)
        bars.push_back({boss->title, boss->phase, boss->fraction, modding::ui::parse_color(modding::Value{boss->color})});
    return bars;
}

// Creatures wearing a retail look ("retail:zombie|...") chase with the voice
// the retail enemy chases with, from the player's own sound bank (inside
// update_audio_environment's voice frame). Ids apart from the horde's.
void mod_ai_retail_voices()
{
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
        return;
    for (const modding::gameplay::ClientEntity &entity : mod_gameplay_->client_entities())
    {
        if (entity.animation != "run" || entity.model.find("retail:") == std::string::npos)
            continue;
        std::string_view family = std::string_view{entity.model}.substr(entity.model.find("retail:") + 7U);
        family = family.substr(0U, std::min(family.find(':'), family.find('|')));
        const char *voice = family == "zombie"     ? "ZombieGrowl"
                            : family == "alien"    ? "Alien"
                            : family == "felguard" ? "Felguard"
                                                   : "Skeleton";
        sound_bank_->sync_enemy_voice(0x80000000U | static_cast<std::uint32_t>(entity.id & 0x7FFFFFFFU), voice,
                                      {static_cast<float>(entity.position.x), static_cast<float>(entity.position.y),
                                       static_cast<float>(entity.position.z)});
    }
}

// The mod a projectile kind ("mod:name") belongs to: its effect recipes are
// looked up as that mod's.
[[nodiscard]] static std::string mod_ai_projectile_mod(std::string_view kind)
{
    return std::string{kind.substr(0U, std::min(kind.find(':'), kind.size()))};
}

// (fix) Each projectile in flight with an `fx` trails it (Enhanced
// presentation): the effect starts with the projectile, follows it and stops
// emitting when it is gone, leaving what it emitted to fade.
void mod_ai_projectile_trails()
{
    if (mod_graphics_ == nullptr)
        return;
    std::set<std::uint64_t> flying;
    for (const modding::gameplay::ClientProjectile &projectile : mod_gameplay_->client_projectiles())
    {
        if (projectile.fx.empty())
            continue;
        flying.insert(projectile.id);
        const modding::graphics::Vec3 at{static_cast<float>(projectile.position.x),
                                         static_cast<float>(projectile.position.y),
                                         static_cast<float>(projectile.position.z)};
        const modding::graphics::Vec3 velocity{static_cast<float>(projectile.velocity.x),
                                               static_cast<float>(projectile.velocity.y),
                                               static_cast<float>(projectile.velocity.z)};
        auto trail = mod_ai_trails_.find(projectile.id);
        if (trail == mod_ai_trails_.end())
        {
            modding::Handle handle;
            try
            {
                vfx::SpawnParams params;
                params.position = at;
                params.velocity = velocity;
                params.scale = projectile.scale;
                const float speed = vfx::length(velocity);
                if (speed > 0.001F)
                    params.direction = velocity * (-1.0F / speed); // the trail streams out behind
                handle = mod_graphics_->spawn_effect(mod_ai_projectile_mod(projectile.kind), projectile.fx, params,
                                                     std::nullopt);
            }
            catch (const modding::ScriptError &error)
            {
                std::cerr << "mod.ai.fx " << projectile.kind << ": " << error.what() << '\n';
            }
            mod_ai_trails_.emplace(projectile.id, handle);
            continue;
        }
        if (trail->second.valid())
            static_cast<void>(mod_graphics_->move_effect(trail->second, at, velocity));
    }
    for (auto trail = mod_ai_trails_.begin(); trail != mod_ai_trails_.end();)
    {
        if (flying.contains(trail->first))
        {
            ++trail;
            continue;
        }
        if (trail->second.valid())
            static_cast<void>(mod_graphics_->stop_effect(trail->second, false));
        trail = mod_ai_trails_.erase(trail);
    }
}

// A world still waits for OPENMINERZ_DIAGNOSTIC_MOD_AI_SECONDS of game time
// (creatures need a few seconds to hunt, shoot and gather); unset, at once.
[[nodiscard]] bool mod_ai_capture_ready() const
{
    const char *seconds = SDL_getenv("OPENMINERZ_DIAGNOSTIC_MOD_AI_SECONDS");
    return seconds == nullptr || *seconds == '\0' ||
           world_game_time_ >= static_cast<float>(std::clamp(std::atof(seconds), 0.0, 120.0));
}

// Every frame (from mod_graphics_update_creatures): impacts, boss music.
void mod_ai_presentation()
{
    if (mod_gameplay_ == nullptr || !mod_gameplay_->client_running())
    {
        if (!mod_ai_music_.empty() && mod_graphics_ != nullptr)
            mod_graphics_->stop_music(1.0F);
        mod_ai_music_.clear();
        mod_ai_trails_.clear();
        return;
    }
    for (const modding::gameplay::ClientImpact &impact : mod_gameplay_->take_client_impacts())
    {
        const world::Vec3 at{static_cast<float>(impact.position.x), static_cast<float>(impact.position.y),
                             static_cast<float>(impact.position.z)};
        std::cout << "mod.ai.impact kind=" << impact.kind << " hit=" << impact.hit << " at=" << at.x << ','
                  << at.y << ',' << at.z << '\n';
        // (fix) The projectile's impact_fx (Enhanced presentation)...
        bool own_effect{};
        if (!impact.fx.empty() && mod_graphics_ != nullptr)
            try
            {
                vfx::SpawnParams params;
                params.position = {at.x, at.y, at.z};
                own_effect = mod_graphics_->spawn_effect(mod_ai_projectile_mod(impact.kind), impact.fx, params,
                                                         std::nullopt)
                                 .valid();
            }
            catch (const modding::ScriptError &error)
            {
                std::cerr << "mod.ai.impact_fx " << impact.kind << ": " << error.what() << '\n';
            }
        if (impact.explosion > 0.0F)
        {
            play_positional("Explosion", at);
            // ...stands for the game's own blast (a dragon fireball's size)
            // where it plays. Where it cannot (the Original presentation has
            // no effects layer), a mod that asked for its own look gets the
            // game's brief flash and a puff of smoke instead of that blast's
            // five-second cloud.
            if (world_preview_ != nullptr && !own_effect)
            {
                if (!impact.fx.empty() && settings_.presentation != PresentationProfile::rebirth)
                {
                    world_preview_->spawn_flash_effect(at);
                    world_preview_->spawn_impact_smoke(at, {1.0F, 1.0F, 1.0F});
                }
                else
                    world_preview_->spawn_fireball_explosion(at, false, false, ExplosionStyle::fireball);
            }
        }
        else if (impact.hit == "player" || impact.hit == "entity")
            play_positional("BulletHitHuman", at);
    }
    mod_ai_projectile_trails();
    // The music of the nearest boss whose bar shows (its phase may change it).
    std::string wanted;
    std::string mod;
    std::string track;
    const world::Vec3 me = player_.state().position;
    double best = std::numeric_limits<double>::max();
    for (const modding::gameplay::ClientBoss &boss : mod_gameplay_->client_bosses())
    {
        const double dx = boss.position.x - me.x;
        const double dz = boss.position.z - me.z;
        const double distance = std::sqrt(dx * dx + dz * dz);
        if (boss.music.empty() || distance > boss.range || distance >= best)
            continue;
        best = distance;
        mod = boss.mod;
        track = boss.music;
        wanted = boss.mod + '|' + boss.music;
    }
    if (wanted == mod_ai_music_ || mod_graphics_ == nullptr)
        return;
    mod_ai_music_ = wanted;
    try
    {
        if (wanted.empty())
            mod_graphics_->stop_music(2.0F);
        else
        {
            const bool playing = mod_graphics_->play_music(mod, track, 1.5F, 1.0F, true);
            std::cout << "mod.ai.music track=" << track << " playing=" << playing << '\n';
        }
    }
    catch (const modding::ScriptError &error)
    {
        std::cerr << "mod.ai.music " << error.what() << '\n';
    }
}
