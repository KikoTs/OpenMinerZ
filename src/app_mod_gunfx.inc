// (GunFX) Member functions and state of Application::Impl: what the app tells
// the viewmodel and the remote animators about the client mods' weapon
// effects (openminerz/modding/gunfx.hpp, src/gunfx_view.inc). Included inside
// the class body by app.cpp.
//
// PRESENTATION ONLY. With no mod calling `gunfx.*` (an empty library) every
// function here leaves the game exactly as it was: the retail reload cues
// play, other players' reload clips keep retail's own speed, no flag is set.

// Your current reload started with an empty magazine (the "<Clip>Empty"
// variants and cue lists).
bool gunfx_reload_empty_{};
// OPENMINERZ_DIAGNOSTIC_GUNFX_AT=<seconds>: a --screenshot waits until the
// world has run that long (a moment of a running reload). -1 unread, 0 off.
float gunfx_capture_at_{-1.0F};
// OPENMINERZ_DIAGNOSTIC_GUNFX_RELOAD=<seconds>[,empty|,fire|,use]: at that world
// time the held gun (--item) reloads as a press of Reload does, with half a
// magazine left (`empty`: none; `fire`: one shot instead), and rounds for
// it in the pack. `use` presses primary action for 1.1 seconds, for tools
// and the grenade's complete cook/throw path. -1 unread, 0 off.
float gunfx_reload_at_{-1.0F};
int gunfx_reload_kind_{};
bool gunfx_reload_pressed_{};
bool gunfx_use_released_{};

// Capture every authored pose through the actual game renderer, so review
// includes the skin, native part cuts and first-person projection. Opt-in with
// OPENMINERZ_DIAGNOSTIC_GUNFX_SEQUENCE=GunReload,2.983,30 and --screenshot PATH.
// The output is PATH-stem-0000.bmp, etc.; the last frame includes the endpoint.
// This advances presentation poses only, not gameplay or physics at fixed time.
int gunfx_sequence_frame_{-1};
int gunfx_sequence_settle_{};
int gunfx_sequence_count_{};
double gunfx_sequence_duration_{};
double gunfx_sequence_fps_{};
std::string gunfx_sequence_clip_;

[[nodiscard]] bool gunfx_capture_sequence(const std::filesystem::path &path)
{
    const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_GUNFX_SEQUENCE");
    if (value == nullptr || *value == '\0')
        return false;
    if (gunfx_sequence_frame_ < 0)
    {
        const std::string text{value};
        const auto comma = text.find(',');
        const auto next = comma == std::string::npos ? comma : text.find(',', comma + 1U);
        if (comma == std::string::npos || next == std::string::npos)
            throw std::runtime_error{"GunFX sequence requires clip,duration,fps"};
        gunfx_sequence_clip_ = text.substr(0U, comma);
        gunfx_sequence_duration_ = std::stod(text.substr(comma + 1U, next - comma - 1U));
        gunfx_sequence_fps_ = std::stod(text.substr(next + 1U));
        if (gunfx_sequence_clip_.empty() || !std::isfinite(gunfx_sequence_duration_) ||
            !std::isfinite(gunfx_sequence_fps_) || gunfx_sequence_duration_ <= 0.0 ||
            gunfx_sequence_duration_ > 30.0 || gunfx_sequence_fps_ < 1.0 || gunfx_sequence_fps_ > 60.0)
            throw std::runtime_error{"GunFX sequence needs duration (0,30] and fps [1,60]"};
        gunfx_sequence_count_ = static_cast<int>(std::ceil(gunfx_sequence_duration_ * gunfx_sequence_fps_));
        gunfx_sequence_frame_ = 0;
    }
    if (gunfx_sequence_settle_ == 0)
    {
        const double time = std::min(gunfx_sequence_duration_, gunfx_sequence_frame_ / gunfx_sequence_fps_);
        const std::string pose = gunfx_sequence_clip_ + "," + std::to_string(time);
        if (!SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "OPENMINERZ_DIAGNOSTIC_GUNFX_POSE", pose.c_str(), true))
            throw std::runtime_error{"Unable to set the diagnostic pose"};
    }
    // Readback can return the previous present. Let the requested pose reach
    // both buffers before reading it, including at the first and last frame.
    if (++gunfx_sequence_settle_ < 3)
        return true;
    const std::string index = std::to_string(gunfx_sequence_frame_);
    capture(path.parent_path() / (path.stem().string() + "-" + std::string(4U - index.size(), '0') + index + ".bmp"));
    gunfx_sequence_settle_ = 0;
    if (++gunfx_sequence_frame_ > gunfx_sequence_count_)
        running_ = false;
    return true;
}

[[nodiscard]] const modding::gunfx::Library *gunfx_library() const noexcept
{
    return mod_graphics_ != nullptr && !mod_graphics_->gunfx().empty() ? &mod_graphics_->gunfx() : nullptr;
}

[[nodiscard]] bool gunfx_capture_ready()
{
    if (gunfx_capture_at_ < 0.0F)
    {
        const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_GUNFX_AT");
        gunfx_capture_at_ = value != nullptr ? static_cast<float>(std::clamp(std::atof(value), 0.0, 600.0)) : 0.0F;
        const char *reload = SDL_getenv("OPENMINERZ_DIAGNOSTIC_GUNFX_RELOAD");
        gunfx_reload_at_ = reload != nullptr ? static_cast<float>(std::clamp(std::atof(reload), 0.0, 600.0)) : 0.0F;
        if (reload != nullptr && std::strstr(reload, ",empty") != nullptr)
            gunfx_reload_kind_ = 1;
        else if (reload != nullptr && std::strstr(reload, ",fire") != nullptr)
            gunfx_reload_kind_ = 2;
        else if (reload != nullptr && std::strstr(reload, ",use") != nullptr)
            gunfx_reload_kind_ = 3;
    }
    if (gunfx_reload_at_ > 0.0F && !gunfx_reload_pressed_ && world_game_time_ >= gunfx_reload_at_ &&
        (selected_gun() != nullptr || gunfx_reload_kind_ == 3))
    {
        gunfx_reload_pressed_ = true;
        if (gunfx_reload_kind_ == 3)
        {
            std::cout << "diagnostic gunfx primary action at t=" << world_game_time_ << '\n';
            primary_action_pressed();
        }
        else if (const GunStats *gun = selected_gun(); gun != nullptr)
        {
            // Half a magazine (`empty`: none), and rounds in the pack to load.
            if (auto &slot = inventory_.tray()[inventory_.selected_index()]; slot.has_value())
                slot->rounds_in_clip = gunfx_reload_kind_ == 1 ? std::uint16_t{0}
                                                               : static_cast<std::uint16_t>(gun->clip_capacity / 2U);
            if (inventory_.count(gun->ammo_item) == 0U)
                (void)inventory_.add_item(gun->ammo_item, 64U);
            std::cout << "diagnostic gunfx reload at t=" << world_game_time_ << '\n';
            if (gunfx_reload_kind_ == 2)
                gun_input(true, false);
            else
                gun_input(false, true);
        }
    }
    if (gunfx_reload_kind_ == 3 && gunfx_reload_pressed_ && !gunfx_use_released_ &&
        world_game_time_ >= gunfx_reload_at_ + 1.1F)
    {
        gunfx_use_released_ = true;
        release_grenade();
        mine_button_down_ = false;
        std::cout << "diagnostic gunfx primary release at t=" << world_game_time_ << '\n';
    }
    return gunfx_capture_at_ <= 0.0F || world_game_time_ >= gunfx_capture_at_;
}

// Your reload starts: whether its magazine was empty, and whether the pack
// mutes the retail cue for the clip (and model) it plays.
[[nodiscard]] bool gunfx_local_reload_starts(InventoryItemId item, std::uint16_t rounds_in_clip,
                                             std::string_view cue)
{
    gunfx_reload_empty_ = rounds_in_clip == 0U;
    if (world_preview_ != nullptr)
        world_preview_->set_gunfx_reload_empty(gunfx_reload_empty_);
    const modding::gunfx::Library *library = gunfx_library();
    if (library == nullptr)
        return false;
    const auto &upper = local_animator_.channel(world::AnimationChannel::upper).current;
    if (!upper.has_value())
        return false;
    const bool pack_mutes = library->mutes(
        world::animation_clip_info(upper->clip).name, modding::gunfx::held_model_name(static_cast<std::uint16_t>(item)),
        gunfx_reload_empty_, cue,
        local_animator_.generation() == world::AnimationGeneration::modern ? "modern" : "classic");
    if (SDL_getenv("OPENMINERZ_DIAGNOSTIC_GUNFX") != nullptr)
        std::cout << "gunfx reload starts: retail cue " << cue << (pack_mutes ? " muted" : " played")
                  << (gunfx_reload_empty_ ? " (empty magazine)" : "") << '\n';
    return pack_mutes;
}

// Another player's reload: their clip is time-scaled to the gun they carry
// (presentation only), and the pack may mute their "Reload" cue.
void gunfx_remote_reload_input(world::PlayerAnimationInput &input, std::uint16_t carried_wire,
                               net::ProtocolVersion protocol) const
{
    if (gunfx_library() == nullptr || !input.reloading)
        return;
    InventoryItemId item{};
    if (carried_wire > static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max()) ||
        !net::inventory_item_from_wire(static_cast<std::int16_t>(carried_wire), protocol, item))
        return;
    if (const GunStats *gun = gun_stats(item); gun != nullptr && gun->reload_seconds > 0.0F)
    {
        input.reload_ticks = world::animation_ticks_from_seconds(static_cast<double>(gun->reload_seconds));
        input.presentation_reload = true;
    }
}

[[nodiscard]] bool gunfx_mutes_remote_reload(const world::PlayerAnimator &animator, std::uint16_t carried_wire,
                                             net::ProtocolVersion protocol) const
{
    const modding::gunfx::Library *library = gunfx_library();
    if (library == nullptr)
        return false;
    const auto &upper = animator.channel(world::AnimationChannel::upper).current;
    InventoryItemId item{};
    if (!upper.has_value() || carried_wire > static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max()) ||
        !net::inventory_item_from_wire(static_cast<std::int16_t>(carried_wire), protocol, item))
        return false;
    return library->mutes(world::animation_clip_info(upper->clip).name,
                          modding::gunfx::held_model_name(static_cast<std::uint16_t>(item)), false, "Reload",
                          animator.generation() == world::AnimationGeneration::modern ? "modern" : "classic");
}

// OPENMINERZ_DIAGNOSTIC_GUNFX_MOVE=walk,strafe,sprint,jump,turn,switch,aim
// (any of them): the local player is driven so a capture shows the
// procedural motion live (walk forward, strafe left and right, sprint, hop
// every 1.4 s, sweep the view from side to side, the next tray slot every
// 2 s, the sights raised). Inert without it.
int gunfx_move_{-1};
float gunfx_move_last_{};

void gunfx_diagnostic_input(world::PlayerInput &input, float delta)
{
    if (gunfx_move_ < 0)
    {
        gunfx_move_ = 0;
        if (const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_GUNFX_MOVE"))
        {
            const std::string_view text{value};
            const auto has = [&](std::string_view word) { return text.find(word) != std::string_view::npos; };
            gunfx_move_ = (has("walk") ? 1 : 0) | (has("strafe") ? 2 : 0) | (has("sprint") ? 4 : 0) |
                          (has("jump") ? 8 : 0) | (has("turn") ? 16 : 0) | (has("switch") ? 64 : 0) |
                          (has("aim") ? 128 : 0) | 32;
        }
    }
    // Everything starts a second into the world, once a capture can see it.
    if (gunfx_move_ == 0 || !world_ready() || world_game_time_ < 1.0F)
        return;
    const float t = world_game_time_;
    if ((gunfx_move_ & 1) != 0)
        input.forward = 1.0F;
    if ((gunfx_move_ & 2) != 0)
        input.strafe = std::sin(t * 1.7F) > 0.0F ? 1.0F : -1.0F;
    // sprint: with stamina kept full, so a capture finds it running.
    if ((gunfx_move_ & 4) != 0)
    {
        input.forward = 1.0F;
        input.sprint = true;
        player_.state().stamina = 100.0F;
    }
    if ((gunfx_move_ & 8) != 0)
        input.jump = std::fmod(t, 1.4F) < 0.05F;
    if ((gunfx_move_ & 16) != 0)
        player_.look(delta * 1.6F * std::sin(t * 1.3F), 0.0F);
    // switch: the next slot of the tray every 2 s (the new item comes up
    // from below); aim: the sights raised from 1 s on.
    if ((gunfx_move_ & 64) != 0 && std::floor(t / 2.0F) != std::floor(gunfx_move_last_ / 2.0F) && t > 1.0F)
        select_hotbar(1);
    gunfx_move_last_ = t;
    if ((gunfx_move_ & 128) != 0)
        shouldering_ = t >= 1.0F;
}

// With --diagnostic-multiplayer and OPENMINERZ_DIAGNOSTIC_AVATAR_CASE=
// reload|fire|hold, the test players are animated as a joined game animates
// another player: their state goes through world::PlayerAnimator every tick
// (as a PlayerUpdateMessage would), with the reload time-scale and the
// muted "Reload" cue of a pack, so captures show the real remote path.
void gunfx_diagnostic_remotes()
{
    if (!options_.diagnostic_multiplayer)
        return;
    static const bool wanted = [] {
        const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_AVATAR_CASE");
        const std::string_view text = value != nullptr ? value : "";
        return text == "reload" || text == "fire" || text == "hold";
    }();
    if (!wanted)
        return;
    const net::ProtocolVersion protocol = inventory_.profile() == InventoryProfile::modern
                                              ? net::ProtocolVersion::modern_v4
                                              : net::ProtocolVersion::classic_v3;
    for (auto &[gid, remote] : online_remote_players_)
    {
        (void)gid;
        const world::AnimationGeneration generation = protocol == net::ProtocolVersion::modern_v4
                                                          ? world::AnimationGeneration::modern
                                                          : world::AnimationGeneration::classic;
        if (!remote.animator_started || remote.animator.generation() != generation)
        {
            remote.animator = world::PlayerAnimator{generation};
            remote.animator_started = true;
        }
        world::PlayerAnimationInput input;
        input.mode = static_cast<std::uint8_t>(remote.state.animation_mode);
        input.using_tool = remote.state.using_tool && std::fmod(world_game_time_, 0.5F) < 0.02F;
        input.shouldering = remote.state.shouldering;
        input.reloading = remote.state.reloading;
        input.torso_pitch_radians = remote.state.torso_pitch;
        gunfx_remote_reload_input(input, remote.carried_item, protocol);
        const world::PlayerAnimationResult cues = remote.animator.update(input);
        if (cues.reload_cue_started && !gunfx_mutes_remote_reload(remote.animator, remote.carried_item, protocol))
            play_positional("Reload", remote.state.position);
    }
}
