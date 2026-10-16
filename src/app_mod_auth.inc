// Member functions and state of Application::Impl: the host's authority in
// sessions only OpenMinerZ players can join (net/session_authority.hpp).
// Included inside the class body by app.cpp, after app_mod_net.inc.
//
// Which sessions: exactly those whose manifest requires_extension() - server
// or core mods, or the host's "OpenMinerZ players only". Every other session,
// and every offline world, creates nothing here and each hook returns at its
// first test, so a session that may hold a stock player keeps retail's
// peer-owned model byte for byte (docs/retail-parity.md).
//
//   hosting   authority_host_ judges every remote player's stock messages in
//             handle_online_gameplay before they are applied or relayed,
//             decides their damage (melee, gunfire traced here, explosions,
//             fireballs, lava, mods), holds their own hurt reports against
//             this host's copy of the enemies, and answers over the mod
//             channel (authority_link_). The host's own player is simulated
//             as always and mirrored into it.
//   joined    authority_client_ shows each hit at once but never lowers the
//             health itself: it reports what only it saw (an enemy's swing,
//             an arrow, a fall) and takes health, death, respawn and position
//             corrections from the host.
//
// Hooks app.cpp calls, each marked "(Authority)": update_authority (every
// frame), tick_authority (every fixed step), authority_refuses
// (handle_online_gameplay), authority_relay_packet (its relay),
// authority_peer_joined / authority_peer_left (mod_net_peer_connected /
// _disconnected), authority_local_hit (apply_player_damage),
// authority_requests_respawn (respawn_player), authority_announce_teleport
// (the teleports), authority_enemy_events (update_enemies),
// authority_tracer_victim (update_tracers), authority_local_explosion
// (apply_explosion_splash), authority_fireball (apply_fireball_detonation),
// authority_local_melee (apply_melee_player_hit), authority_level_restart
// (restart_online_level), stop_authority_link (stop_mod_net_session).

// What the authority asks of this host's world: its terrain, its copy of the
// enemies, the blast probe. Main thread only, like the authority.
class AppAuthorityWorld final : public net::AuthorityWorld
{
  public:
    explicit AppAuthorityWorld(Impl &app) : app_{app}
    {
    }

    [[nodiscard]] std::optional<world::BlockType> block(int x, int y, int z) const override
    {
        return app_.online_block_at({x, y, z});
    }
    [[nodiscard]] std::optional<net::AuthorityEnemy> enemy(std::uint8_t target_gid,
                                                           std::uint32_t enemy_id) const override
    {
        for (const world::Enemy &enemy : app_.enemy_horde_.enemies())
            if (enemy.target_gid == target_gid && enemy.id == enemy_id)
                return net::AuthorityEnemy{enemy.type, enemy.position, enemy.target_gid,
                                           enemy.state != world::EnemyState::die &&
                                               enemy.state != world::EnemyState::despawn};
        return std::nullopt;
    }
    [[nodiscard]] float blast_line_of_sight(world::Vec3 impact, world::Vec3 target,
                                            world::DragonKind kind) const override
    {
        if (app_.world_preview_ == nullptr)
            return 1.0F;
        return world::dragon_fireball_damage_multiplier(app_.world_preview_->region(), kind, impact,
                                                         target);
    }
    [[nodiscard]] bool clear_line(world::Vec3 from, world::Vec3 to) const override
    {
        if (app_.world_preview_ == nullptr)
            return true;
        const world::Vec3 delta{to.x - from.x, to.y - from.y, to.z - from.z};
        const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (length < 0.001F)
            return true;
        const auto hit = world::PlayerSimulation::raycast_blocking(
            app_.world_preview_->region(), from,
            {delta.x / length, delta.y / length, delta.z / length}, length);
        return !hit.has_value() || hit->distance >= length - 0.05F;
    }
    // (mod-liquid) The host's own liquids, in its terrain window and beyond.
    [[nodiscard]] bool liquid(int x, int y, int z) const override
    {
        return app_.world_preview_ != nullptr && app_.world_preview_->resolved_liquid(x, y, z);
    }

  private:
    Impl &app_;
};

std::unique_ptr<net::AuthorityHost> authority_host_;
std::unique_ptr<AppAuthorityWorld> authority_world_;
std::unique_ptr<net::AuthorityServerLink> authority_link_;
std::unique_ptr<net::AuthorityClient> authority_client_;
// What the next local hit is, where the call site knows better than its
// DamageCause (an arrow, a fireball), and the enemy swings update_enemies
// found, one report each.
std::optional<net::HarmCause> authority_cause_hint_;
std::vector<net::HarmReport> authority_pending_reports_;
// When this client last showed a hit of its own, so the host's health drop
// for the same hit does not show it twice.
float authority_feedback_at_{-1.0e9F};
// mod-ai: the DamageCause of the mod damage being applied (authority_mod_damage).
std::optional<modding::gameplay::DamageCause> authority_mod_cause_;

[[nodiscard]] double authority_now() const
{
    return mod_net_now();
}

[[nodiscard]] static net::HarmCause authority_cause(modding::gameplay::DamageCause cause) noexcept
{
    switch (cause)
    {
    case modding::gameplay::DamageCause::enemy:
        return net::HarmCause::enemy;
    case modding::gameplay::DamageCause::fall:
        return net::HarmCause::fall;
    case modding::gameplay::DamageCause::lava:
        return net::HarmCause::lava;
    case modding::gameplay::DamageCause::player:
        return net::HarmCause::player_gun;
    case modding::gameplay::DamageCause::explosion:
        return net::HarmCause::explosion;
    case modding::gameplay::DamageCause::creature:
    case modding::gameplay::DamageCause::effect:
    case modding::gameplay::DamageCause::script:
        return net::HarmCause::mod;
    case modding::gameplay::DamageCause::drowning:
    case modding::gameplay::DamageCause::unknown:
        break;
    }
    return net::HarmCause::unknown;
}

[[nodiscard]] static modding::gameplay::DamageCause authority_damage_cause(net::HarmCause cause) noexcept
{
    switch (cause)
    {
    case net::HarmCause::enemy:
    case net::HarmCause::arrow:
    case net::HarmCause::fireball:
        return modding::gameplay::DamageCause::enemy;
    case net::HarmCause::fall:
        return modding::gameplay::DamageCause::fall;
    case net::HarmCause::lava:
        return modding::gameplay::DamageCause::lava;
    case net::HarmCause::player_melee:
    case net::HarmCause::player_gun:
        return modding::gameplay::DamageCause::player;
    case net::HarmCause::explosion:
        return modding::gameplay::DamageCause::explosion;
    case net::HarmCause::mod:
        return modding::gameplay::DamageCause::script;
    case net::HarmCause::unknown:
        break;
    }
    return modding::gameplay::DamageCause::unknown;
}

// --- lifecycle ------------------------------------------------------------------

// Every frame: starts and stops the two halves with their session, opens the
// mod channel a session with no server mod needs for them, and applies what
// the host said to this client.
void update_authority()
{
    const bool hosting = online_host_ != nullptr && online_host_->running() &&
                         net::session_authoritative(host_settings_.mod_manifest);
    if (!hosting && authority_host_ != nullptr)
    {
        stop_authority_link();
        authority_host_.reset();
        authority_world_.reset();
        enemy_horde_.set_observe_remote_strikes(false);
        std::cout << "authority.host stopped\n";
    }
    if (hosting && authority_host_ == nullptr && world_preview_ != nullptr)
        start_authority_host();
    if (authority_host_ != nullptr)
    {
        // A session with no server mod has no mod VM to open the channel.
        if (mod_net_server_ == nullptr)
            start_mod_net_session(true);
        if (authority_link_ == nullptr && mod_net_server_ != nullptr && mod_net_server_->enabled())
            authority_link_ = std::make_unique<net::AuthorityServerLink>(
                *authority_host_, *mod_net_server_,
                [this](const modding::Handle &player) { return mod_net_player_gid(player); },
                [this](std::uint8_t gid) -> std::optional<modding::Handle> {
                    if (mod_net_server_ == nullptr || online_host_ == nullptr)
                        return std::nullopt;
                    for (const modding::Handle &player : mod_net_server_->ready_players())
                        if (mod_net_player_gid(player) == gid)
                            return player;
                    return std::nullopt;
                },
                [this] { return authority_now(); });
    }

    const bool joined = online_client_ != nullptr && online_client_joined_ &&
                        online_client_->mod_channel_open() && mod_net_client_ != nullptr &&
                        mod_net_server_ == nullptr;
    if (!joined && authority_client_ != nullptr)
        authority_client_.reset();
    if (joined && authority_client_ == nullptr)
    {
        authority_client_ = std::make_unique<net::AuthorityClient>(*mod_net_client_);
        std::cout << "authority.client started\n";
    }
    if (authority_client_ == nullptr)
        return;
    // A session with no mod VM: the channel is ready once the world is in.
    if (!waiting_for_world_)
        mod_net_client_->set_ready(authority_now());
    for (const net::AuthorityClientEvent &event : authority_client_->take_events())
        apply_authority_event(event);
}

void start_authority_host()
{
    net::AuthorityRules rules;
    rules.protocol = host_settings_.protocol;
    rules.mode = game_mode_;
    rules.difficulty = difficulty_;
    rules.infinite_resources = infinite_resources_;
    rules.pvp = host_settings_.player_versus_player;
    rules.strict = host_settings_.anti_cheat;
    rules.spawn_protection_radius = host_settings_.spawn_protection_radius;
    rules.spawn_point = world::default_start_location;
    authority_host_ = std::make_unique<net::AuthorityHost>(rules);
    authority_world_ = std::make_unique<AppAuthorityWorld>(*this);
    authority_host_->set_world(authority_world_.get());
    // A server mod has its say on every player's damage (player.damaged).
    authority_host_->set_damage_filter(
        [this](std::uint8_t gid, float amount, net::HarmCause cause, world::Vec3 source) {
            if (!mod_gameplay_live())
                return amount;
            // mod-ai: a mod's own damage keeps its cause (a creature's bite
            // is "creature", not "script").
            const modding::gameplay::DamageCause mod_cause =
                cause == net::HarmCause::mod && authority_mod_cause_.has_value() ? *authority_mod_cause_
                                                                                 : authority_damage_cause(cause);
            return mod_gameplay_->authority_player_damaging(gid, amount, mod_cause, mod_vec(source));
        });
    // A server mod's speed effect moves a player faster than the game does.
    authority_host_->set_speed_scale([this](std::uint8_t gid) {
        return mod_gameplay_live() ? mod_gameplay_->player_speed_multiplier(gid) : 1.0F;
    });
    // (Items) A server mod's weapon in a swinging player's hand.
    authority_host_->set_melee_weapon(
        [this](std::uint8_t gid, InventoryItemId item) { return mod_items_authority_weapon(gid, item); });
    authority_host_->add_player(online_local_player_gid(), authority_now(), true);
    for (const auto &[gid, remote] : online_remote_players_)
        authority_host_->add_player(gid, authority_now());
    if (mod_gameplay_ != nullptr)
        mod_gameplay_->set_authority(true);
    enemy_horde_.set_observe_remote_strikes(true);
    std::cout << "authority.host started strict=" << rules.strict
              << " spawn_protection=" << rules.spawn_protection_radius << '\n';
}

// Before the mod channel objects go (stop_mod_net_session): the link holds
// handlers on them. The authority itself stays with the hosted session.
void stop_authority_link()
{
    authority_link_.reset();
    authority_client_.reset();
    if (mod_gameplay_ != nullptr)
        mod_gameplay_->set_authority(false);
}

void authority_peer_joined(std::uint8_t gid)
{
    if (authority_host_ != nullptr && !authority_host_->has_player(gid))
        authority_host_->add_player(gid, authority_now());
}

void authority_peer_left(std::uint8_t gid)
{
    if (authority_host_ != nullptr)
        authority_host_->remove_player(gid);
}

// --- the host -------------------------------------------------------------------

// Every fixed step: the host's own player mirrored in, then the authority's
// clocks, then what it decided goes out.
void tick_authority()
{
    if (authority_host_ == nullptr)
        return;
    if (mod_gameplay_ != nullptr && !mod_gameplay_->authority())
        mod_gameplay_->set_authority(true);
    const double now = authority_now();
    authority_host_->set_local_state(online_local_player_gid(), player_.state().health,
                                     player_life_ == PlayerLife::alive, player_.state().position,
                                     now);
    authority_host_->update(now);
    const std::vector<net::AuthorityEvent> events =
        authority_link_ != nullptr ? authority_link_->flush() : authority_host_->take_events();
    for (const net::AuthorityEvent &event : events)
        handle_authority_event(event);
}

void handle_authority_event(const net::AuthorityEvent &event)
{
    const std::string name = online_gamer_tag(event.gid, std::format("player {}", event.gid));
    switch (event.kind)
    {
    case net::AuthorityEventKind::health:
        if (mod_gameplay_live())
            mod_gameplay_->authority_player_health(event.gid, event.value, event.alive,
                                                   mod_vec(event.position),
                                                   authority_damage_cause(event.cause));
        break;
    case net::AuthorityEventKind::died:
        std::cout << "authority.died gid=" << static_cast<int>(event.gid)
                  << " cause=" << net::harm_cause_name(event.cause) << '\n';
        if (mod_gameplay_live())
            mod_gameplay_->authority_player_health(event.gid, 0.0F, false, mod_vec(event.position),
                                                   authority_damage_cause(event.cause));
        break;
    case net::AuthorityEventKind::respawned:
        std::cout << "authority.respawned gid=" << static_cast<int>(event.gid) << '\n';
        if (mod_gameplay_live())
            mod_gameplay_->authority_player_health(event.gid, event.value, true, mod_vec(event.position),
                                                   modding::gameplay::DamageCause::unknown);
        break;
    case net::AuthorityEventKind::correction:
        std::cout << "authority.correction gid=" << static_cast<int>(event.gid) << " reason=" << event.reason
                  << " to=" << event.position.x << ',' << event.position.y << ',' << event.position.z << '\n';
        break;
    case net::AuthorityEventKind::violation:
        std::cerr << "authority.violation gid=" << static_cast<int>(event.gid) << " name=\"" << name
                  << "\" score=" << event.value << " reason=" << event.reason << '\n';
        break;
    case net::AuthorityEventKind::kick: {
        // STRICT: the reason goes to the host's log and chat and to everyone
        // still in the session, then the player is removed.
        const std::string line = std::format("{} was removed by the host's anti-cheat: {}", name, event.reason);
        std::cerr << "authority.kick gid=" << static_cast<int>(event.gid) << " reason=" << event.reason << '\n';
        push_online_chat(line);
        toast("ANTI-CHEAT", line);
        if (authority_link_ != nullptr)
            authority_link_->notice_all(line);
        send_host_kick(event.gid, false);
        break;
    }
    }
}

// handle_online_gameplay, once a remote message passed the session's own
// gates: the authority's verdict. True: refused - not applied, not relayed,
// the sender's own picture corrected where the message changed it.
[[nodiscard]] bool authority_refuses(const net::LegacySteamHostInbound &inbound,
                                     const net::DecodedPacket &decoded)
{
    if (authority_host_ == nullptr || decoded.message == nullptr)
        return false;
    if (!authority_host_->has_player(inbound.sender_gid))
        authority_host_->add_player(inbound.sender_gid, authority_now());
    const std::string_view name = decoded.message->name;
    // A crate slot the player takes from or fills: the host's crate says what
    // moved. Filling it with what the player cannot have is refused.
    std::optional<std::pair<InventoryStack, InventoryStack>> crate_change;
    if (name == "ItemCrateMessage")
    {
        net::ItemCratePayload crate;
        net::PayloadError payload_error{};
        if (net::decode_item_crate_payload(decoded.payload, host_settings_.protocol, crate, payload_error))
        {
            const CrateKey key{crate.location.x, crate.location.y, crate.location.z};
            InventoryStack before;
            if (const auto found = crate_inventories_.find(key);
                found != crate_inventories_.end() && crate.index >= 0 &&
                static_cast<std::size_t>(crate.index) < found->second.size() &&
                found->second[static_cast<std::size_t>(crate.index)].has_value())
                before = *found->second[static_cast<std::size_t>(crate.index)];
            InventoryStack after;
            if (PlayerInventory::Slot slot;
                net::inventory_slot_to_stack(crate.item, host_settings_.protocol, slot) && slot.has_value())
                after = *slot;
            if (!authority_host_->may_put_in_crate(inbound.sender_gid, before, after))
            {
                std::cerr << "authority.refused gid=" << static_cast<int>(inbound.sender_gid)
                          << " message=ItemCrateMessage\n";
                static_cast<void>(correct_refused_crate_slot(inbound.peer, crate.location, crate.index));
                return true;
            }
            crate_change = std::pair{before, after};
        }
    }
    if (name == "DestroyCrateMessage")
    {
        net::IntVector3 location;
        net::PayloadError payload_error{};
        if (net::decode_int_vector_payload(decoded.payload, location, payload_error))
            if (const auto found = crate_inventories_.find(CrateKey{location.x, location.y, location.z});
                found != crate_inventories_.end())
            {
                std::vector<InventoryStack> contents;
                for (const PlayerInventory::Slot &slot : found->second)
                    if (slot.has_value())
                        contents.push_back(*slot);
                authority_host_->observe_crate_opened(
                    inbound.sender_gid,
                    {static_cast<float>(location.x) + 0.5F, static_cast<float>(location.y) + 0.5F,
                     static_cast<float>(location.z) + 0.5F},
                    contents, authority_now());
            }
    }
    const net::AuthorityVerdict verdict = authority_host_->observe_stock_message(
        inbound.sender_gid, inbound.recipient_gid, name, decoded.payload, authority_now());
    if (verdict == net::AuthorityVerdict::accept)
    {
        if (crate_change.has_value())
            authority_host_->observe_crate_slot(inbound.sender_gid, crate_change->first,
                                                crate_change->second, authority_now());
        return false;
    }
    std::cerr << "authority.refused gid=" << static_cast<int>(inbound.sender_gid) << " message=" << name
              << '\n';
    if (name == "AlterBlockMessage" || name == "RemoveBlocksMessage")
        correct_rejected_block_mutation(inbound.peer, decoded);
    return true;
}

// The relay of a remote player's update: while the authority says the player
// is dead, every peer is told so whatever its own client claims.
void authority_relay_packet(std::uint8_t sender_gid, const net::DecodedPacket &decoded,
                            std::vector<std::uint8_t> &packet) const
{
    if (authority_host_ == nullptr || decoded.message == nullptr ||
        decoded.message->name != "PlayerUpdateMessage")
        return;
    const auto view = authority_host_->player(sender_gid);
    if (!view.has_value() || view->alive)
        return;
    net::PlayerUpdatePayload update;
    net::PayloadError payload_error{};
    if (!net::decode_player_update_payload(decoded.payload, host_settings_.protocol, update, payload_error) ||
        (update.state_flags & net::PlayerUpdatePayload::dead_flag) != 0U)
        return;
    update.state_flags = static_cast<std::uint8_t>(update.state_flags | net::PlayerUpdatePayload::dead_flag);
    const auto payload = net::encode_player_update_payload(update, host_settings_.protocol, payload_error);
    net::PacketError packet_error{};
    if (payload.has_value())
        if (auto rewritten =
                net::encode_packet(host_settings_.protocol, decoded.message->id, *payload, packet_error))
            packet = std::move(*rewritten);
}

// update_enemies: this host's copies of the enemies that chase remote players
// landed these swings; held against those players' own reports.
void authority_enemy_events(const world::EnemyEvents &events)
{
    if (authority_host_ != nullptr)
        for (const world::EnemyStrike &strike : events.remote_strikes)
            authority_host_->observe_enemy_hit(strike.target_gid, strike.enemy_id,
                                               mod_scaled_enemy_damage(strike.damage * 100.0F),
                                               strike.source, authority_now());
    if (authority_client_ != nullptr)
        for (const world::EnemyStrike &strike : events.strikes)
            authority_pending_reports_.push_back(
                {net::HarmCause::enemy, mod_scaled_enemy_damage(strike.damage * 100.0F), strike.enemy_id,
                 strike.source});
}

// consume_pickup_as_host gave a remote player a pickup: it now carries it,
// whoever made the pickup.
void authority_pickup_given(std::uint8_t gid, std::int32_t spawner_id, std::int32_t pickup_id,
                            const InventoryStack &stack)
{
    if (authority_host_ != nullptr && spawner_id >= 0 && spawner_id <= 255 && pickup_id >= 0)
        authority_host_->pickup_given(gid, static_cast<std::uint8_t>(spawner_id),
                                      static_cast<std::uint32_t>(pickup_id), stack, authority_now());
}

// An archer's arrow this host flies at a remote player reached it.
void authority_arrow_struck(std::uint8_t target_gid, world::Vec3 at)
{
    if (authority_host_ != nullptr)
        authority_host_->observe_enemy_hit(target_gid, std::nullopt, mod_scaled_enemy_damage(35.0F), at,
                                           authority_now(), net::HarmCause::arrow);
}

// update_tracers: where a remote player was over the shooter's lag, not only
// where it is now. nullopt: missed.
[[nodiscard]] std::optional<float> authority_tracer_victim(std::uint8_t victim, world::Vec3 tail,
                                                           world::Vec3 head) const
{
    if (authority_host_ == nullptr)
        return std::nullopt;
    return authority_host_->tracer_hits_player(victim, tail, head, authority_now());
}

// A tracer struck a remote player: the host's call, with the retail PVP rule.
void authority_tracer_hit(std::uint8_t shooter, std::uint8_t victim, world::Vec3 impact)
{
    if (authority_host_ != nullptr && authority_host_->pvp_allows(shooter, victim))
        authority_host_->harm(victim, 21.0F, net::HarmCause::player_gun, impact, authority_now(), shooter);
}

// apply_explosion_splash for an explosion this host set off: every remote
// player in range (remote players' own explosions come through
// authority_refuses).
void authority_local_explosion(world::Vec3 location, world::ExplosiveDamageBands bands)
{
    if (authority_host_ != nullptr)
        static_cast<void>(authority_host_->apply_explosion(location, bands, online_local_player_gid(),
                                                           authority_now()));
}

// apply_fireball_detonation, whoever owns the dragon.
void authority_fireball(world::Vec3 location, world::DragonKind kind)
{
    if (authority_host_ != nullptr)
        static_cast<void>(authority_host_->apply_fireball(location, kind, authority_now()));
}

// apply_melee_player_hit on the host: its own swing at a remote player.
void authority_local_melee(std::uint8_t victim, std::int16_t wire_item, world::Vec3 intersection)
{
    if (authority_host_ == nullptr)
        return;
    net::MeleePlayerPayload melee;
    melee.item_id = wire_item;
    melee.damage_source = {intersection.x, intersection.y, intersection.z};
    static_cast<void>(authority_host_->observe_melee(online_local_player_gid(), victim, melee, authority_now()));
}

// restart_online_level on the host: everyone returns to the start.
void authority_level_restart()
{
    if (authority_host_ != nullptr)
        authority_host_->observe_level_restart(authority_now());
}

// --- the joined client ------------------------------------------------------------

// apply_player_damage while the host decides: the hit shows at once, what
// only this client saw is reported, and the health waits for the host.
void authority_local_hit(float amount, world::Vec3 source, modding::gameplay::DamageCause cause)
{
    damage_indicators_.push_back({source, 0.0F});
    if (damage_indicators_.size() > 12U)
        damage_indicators_.erase(damage_indicators_.begin());
    sound_bank_->play_cue("Hit");
    last_player_damage_time_ = world_game_time_;
    authority_feedback_at_ = elapsed_;
    const net::HarmCause harm = authority_cause_hint_.value_or(authority_cause(cause));
    if (harm == net::HarmCause::enemy && !authority_pending_reports_.empty())
    {
        for (const net::HarmReport &report : authority_pending_reports_)
            authority_client_->report_harm(report);
        authority_pending_reports_.clear();
        return;
    }
    // Only the client sees an enemy's swing land (BaseTryAttack), an arrow
    // reach it or its own landing; the rest the host saw itself.
    if (harm == net::HarmCause::enemy || harm == net::HarmCause::arrow || harm == net::HarmCause::fall)
        authority_client_->report_harm({harm, amount, std::nullopt, source});
}

void apply_authority_event(const net::AuthorityClientEvent &event)
{
    switch (event.kind)
    {
    case net::AuthorityClientEventKind::health:
        if (player_life_ != PlayerLife::alive || !event.alive)
            break;
        // A drop this client did not show (the host saw a blast, a shot or
        // the lava first): show it now.
        if (event.health < player_.state().health - 0.5F && elapsed_ - authority_feedback_at_ > 0.5F)
        {
            damage_indicators_.push_back({event.source.value_or(player_.state().position), 0.0F});
            if (damage_indicators_.size() > 12U)
                damage_indicators_.erase(damage_indicators_.begin());
            sound_bank_->play_cue("Hit");
            last_player_damage_time_ = world_game_time_;
        }
        player_.state().health = event.health;
        break;
    case net::AuthorityClientEventKind::died:
        if (player_life_ == PlayerLife::alive)
            kill_local_player(authority_damage_cause(event.cause));
        break;
    case net::AuthorityClientEventKind::respawned:
        if (player_life_ != PlayerLife::alive)
            complete_local_respawn();
        player_.state().health = event.health;
        break;
    case net::AuthorityClientEventKind::correction:
        if (world_preview_ == nullptr || player_life_ != PlayerLife::alive)
            break;
        // The host did not accept a move: back to where it last did.
        snap_render_interpolation();
        player_.state().position = event.position;
        player_.state().velocity = {};
        world_preview_->recenter_on(player_.state());
        online_player_update_timer_ = 0.0F;
        caption("[ POSITION CORRECTED BY THE HOST ]", 1.5F);
        std::cout << "authority.corrected reason=" << event.text << '\n';
        break;
    case net::AuthorityClientEventKind::notice:
        push_online_chat(event.text);
        break;
    }
}

// respawn_player while the host decides: ask, and come back when it says so.
[[nodiscard]] bool authority_requests_respawn()
{
    if (authority_client_ == nullptr)
        return false;
    authority_client_->request_respawn();
    return true;
}

// Before this client moves itself far: the host accepts the jump it was told of.
void authority_announce_teleport(net::TeleportKind kind, world::Vec3 target)
{
    if (authority_client_ != nullptr)
        authority_client_->announce_teleport(kind, target);
}

// --- the gameplay service's view (ModGameplayBackend's authority_* hooks) ---------

[[nodiscard]] bool authority_mod_damage(std::uint8_t gid, float amount, modding::gameplay::DamageCause cause,
                                        world::Vec3 source)
{
    if (authority_host_ == nullptr || !authority_host_->has_player(gid))
        return false;
    // Through the damage filter like any other damage the host decides;
    // player.damaged runs once, there.
    authority_mod_cause_ = cause; // mod-ai
    static_cast<void>(authority_host_->harm(gid, amount, authority_cause(cause) == net::HarmCause::unknown
                                                             ? net::HarmCause::mod
                                                             : authority_cause(cause),
                                            source, authority_now()));
    authority_mod_cause_.reset();
    return true;
}

[[nodiscard]] bool authority_mod_heal(std::uint8_t gid, float amount)
{
    if (authority_host_ == nullptr || !authority_host_->has_player(gid))
        return false;
    authority_host_->heal(gid, amount, authority_now());
    return true;
}

[[nodiscard]] bool authority_mod_set_health(std::uint8_t gid, float health)
{
    if (authority_host_ == nullptr || !authority_host_->has_player(gid))
        return false;
    authority_host_->set_health(gid, health, authority_now());
    return true;
}

void authority_mod_teleport(std::uint8_t gid, world::Vec3 feet)
{
    if (authority_host_ != nullptr)
        authority_host_->allow_teleport(gid, feet, authority_now());
}

void authority_mod_give(std::uint8_t gid, std::uint16_t item, std::uint32_t count)
{
    if (authority_host_ == nullptr || count == 0U)
        return;
    authority_host_->grant_items(
        gid, InventoryStack{static_cast<InventoryItemId>(item), static_cast<std::uint16_t>(std::min(count, 65535U))},
        authority_now());
}
