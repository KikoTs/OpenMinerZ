#include "openminerz/retail_character_driver.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openminerz::rig
{
namespace
{

constexpr int lower_channel = 0;
constexpr int tilt_channel = 1;
constexpr int upper_channel = 2;
constexpr int action_channel = 3;
constexpr int head_channel = 4;
constexpr int death_channel = 5;

// Player.SetupAnimation.
void set_up(CharacterPose &pose, DriveState &state)
{
    Animator &animations = pose.animations;
    animations.play("Stand", lower_channel, 0.0);
    state.tilt = animations.play("Tilt", tilt_channel, 0.0);
    if (state.tilt != nullptr)
        state.tilt->pause();
    animations.play("GenericIdle", upper_channel, 0.0);
    if (ClipPlayer *head = animations.play("IdleHead", head_channel, 0.0))
        head->ping_pong = true;
    state.set_up = true;
}

void set_tilt(CharacterPose &pose, DriveState &state, float progress)
{
    if (state.tilt == nullptr)
        return;
    // Player.UpdateAnimation re-plays the paused tilt player every frame and
    // then writes its progress from the torso pitch.
    pose.animations.replay(tilt_channel, state.tilt, 0.0);
    state.tilt->set_progress(progress);
}

} // namespace

ModeClips mode_clips(const RetailRig &rig, PlayerAnimationMode mode) noexcept
{
    const auto gun = [](std::string_view idle, std::string_view walk, std::string_view use,
                        std::string_view shoulder, std::string_view shoulder_idle,
                        std::string_view shoulder_walk, std::string_view shoulder_use,
                        std::string_view reload) {
        return ModeClips{idle, walk, use, shoulder, shoulder_idle, shoulder_walk, shoulder_use, reload};
    };
    switch (mode)
    {
    case PlayerAnimationMode::grenade:
        return {"GrenadeIdle", "GrenadeWalk", "GenericUse", {}, {}, {}, {}, {}};
    case PlayerAnimationMode::fist:
        return {"FistIdle", "FistWalk", "FistUse", {}, {}, {}, {}, {}};
    case PlayerAnimationMode::pick:
        return {"PickIdle", "PickWalk", "PickUse", {}, {}, {}, {}, {}};
    case PlayerAnimationMode::block:
        return {"BlockIdle", "BlockWalk", "BlockUse", {}, {}, {}, {}, {}};
    case PlayerAnimationMode::assault:
        return gun("GunIdle", "GunRun", "GunShoot", "GunShoulder", "GunShoulderIdle", "GunShoulderWalk",
                   "GunShoulderShoot", "GunReload");
    case PlayerAnimationMode::bolt_rifle:
        return gun("RifleIdle", "RifleWalk", "RifleShoot", "RifleShoulder", "RifleShoulderIdle",
                   "RifleShoulderWalk", "RifleShoulderShoot", "RifleReload");
    case PlayerAnimationMode::smg:
        return gun("SMGIdle", "SMGWalk", "SMGShoot", "SMGShoulder", "SMGShoulderIdle", "SMGShoulderWalk",
                   "SMGShoulderShoot", "SMGReload");
    case PlayerAnimationMode::lmg:
        return gun("LMGIdle", "LMGWalk", "LMGShoot", "LMGShoulder", "LMGShoulderIdle", "LMGShoulderWalk",
                   "LMGShoulderShoot", "LMGReload");
    case PlayerAnimationMode::pistol:
        return gun("PistolIdle", "PistolWalk", "PistolShoot", "PistolShoulder", "PistolShoulderIdle",
                   "PistolShoulderWalk", "PistolShoulderShoot", "PistolReload");
    case PlayerAnimationMode::pump_shotgun:
        // 1.9.9.8 gave the shotgun its own stance clips; 1.6.3.8 borrows the rifle's.
        if (rig.find("PumpShotgunIdle") != nullptr)
            return gun("PumpShotgunIdle", "PumpShotgunRun", "PumpShotgunShoot", "PumpShotgunShoulder",
                       "PumpShotgunShoulderIdle", "PumpShotgunShoulderWalk", "PumpShotgunShoulderShoot",
                       "PumpShotgunReload");
        return gun("GunIdle", "GunRun", "PumpShotgunShoot", "GunShoulder", "GunShoulderIdle",
                   "GunShoulderWalk", "PumpShotgunShoulderShoot", "PumpShotgunReload");
    case PlayerAnimationMode::space_assault:
        return gun("LaserGunIdle", "LaserGunRun", "LaserGunShoot", "LaserGunShoulder", "LaserGunShoulderIdle",
                   "LaserGunShoulderWalk", "LaserGunShoulderShoot", "LaserGunReload");
    case PlayerAnimationMode::space_smg:
        return gun("LaserSMGIdle", "LaserSMGRun", "LaserSMGShoot", "LaserSMGShoulder", "LaserSMGShoulderIdle",
                   "LaserSMGShoulderWalk", "LaserSMGShoulderShoot", "LaserSMGReload");
    case PlayerAnimationMode::space_pistol:
        return gun("LaserPistolIdle", "LaserPistolRun", "LaserPistolShoot", "LaserPistolShoulder",
                   "LaserPistolShoulderIdle", "LaserPistolShoulderWalk", "LaserPistolShoulderShoot",
                   "LaserPistolReload");
    case PlayerAnimationMode::space_bolt_rifle:
        return gun("LaserRifleIdle", "LaserRifleRun", "LaserRifleShoot", "LaserRifleShoulder",
                   "LaserRifleShoulderIdle", "LaserRifleShoulderWalk", "LaserRifleShoulderShoot",
                   "LaserRifleReload");
    case PlayerAnimationMode::space_pump_shotgun:
        return gun("LaserGunIdle", "LaserGunRun", "LaserShotgunShoot", "LaserGunShoulder",
                   "LaserGunShoulderIdle", "LaserGunShoulderWalk", "LaserShotgunShoulderShoot",
                   "LaserShotgunReload");
    case PlayerAnimationMode::rpg:
        return gun("RPGIdle", "RPGWalk", "RPGShoot", "GunShoulder", "GunShoulderIdle", "GunShoulderWalk",
                   "PumpShotgunShoulderShoot", "PumpShotgunReload");
    case PlayerAnimationMode::generic:
    default:
        return {};
    }
}

void drive_third_person(CharacterPose &pose, DriveState &state, const ThirdPersonInput &input)
{
    Animator &animations = pose.animations;
    const bool first = !state.set_up;
    if (first)
        set_up(pose, state);
    const double quarter = input.snap || first ? 0.0 : 0.25;
    const ModeClips clips = mode_clips(pose.rig(), input.mode);

    // PlayerUpdateMessage.Apply feeds a dead player no movement and no tilt.
    const float walk = input.dead ? 0.0F : input.walk;
    const float strafe = input.dead ? 0.0F : input.strafe;
    const float forward_amount = std::abs(walk);
    const float strafe_amount = std::abs(strafe);
    const float amount = std::max(forward_amount, strafe_amount);
    const bool moving = amount >= 0.1F;
    const bool shouldering = input.shouldering && !input.dead;

    if (input.dead)
    {
        if (animations.active(death_channel) == nullptr)
            animations.play("Die", death_channel, quarter);
    }
    else if (animations.active(death_channel) != nullptr)
        animations.clear(death_channel, quarter);

    // Channel 3: the use/shoot clip, or the grenade cook and throw.
    const bool using_tool = input.using_tool && !input.dead;
    const bool play_grenade = input.play_grenade && !input.dead;
    ClipPlayer *action = animations.active(action_channel);
    if (using_tool)
    {
        const std::string_view use = shouldering && !clips.shoulder_use.empty() ? clips.shoulder_use : clips.use;
        // The flag stays raised for as long as the tool is held; one swing or
        // shot plays through before the next starts.
        if (!state.action_requested || action == nullptr || action->name() != use || action->finished())
        {
            if (ClipPlayer *started = animations.play(use, action_channel, 0.0))
                started->looping = false;
            state.using_animation_playing = true;
        }
    }
    else if (play_grenade && action == nullptr)
        animations.play("Grenade_Cook", action_channel, quarter);
    else if (action != nullptr)
    {
        action->looping = false;
        if (action->finished())
        {
            if (play_grenade)
            {
                if (action->name() != "Grenade_Throw" && input.ready_to_throw_grenade)
                    animations.play("Grenade_Throw", action_channel, 0.0);
            }
            else
            {
                animations.clear(action_channel, quarter);
                state.using_animation_playing = false;
            }
        }
    }
    state.action_requested = using_tool;

    if (input.underwater && !input.dead)
    {
        if (!animations.is(lower_channel, "Swim"))
            animations.play("Swim", lower_channel, quarter);
    }
    else
    {
        ClipPlayer *upper = animations.active(upper_channel);
        if (state.stance == Stance::reloading && (upper == nullptr || upper->finished()))
            state.stance = Stance::unshouldered;
        const bool has_stances = !clips.shoulder.empty();
        if (input.reloading && !input.dead)
        {
            if (state.stance == Stance::shouldered && !state.using_animation_playing && has_stances)
            {
                state.stance = Stance::unshouldering;
                if (ClipPlayer *player = animations.play(clips.shoulder, upper_channel, quarter))
                    player->reversed = true;
            }
            else if (state.stance == Stance::unshouldered && !state.using_animation_playing &&
                     !clips.reload.empty() && !animations.is(upper_channel, clips.reload))
            {
                state.stance = Stance::reloading;
                animations.play(clips.reload, upper_channel, quarter);
            }
        }
        else if (state.stance == Stance::reloading)
            state.stance = Stance::unshouldered;
        if (shouldering && has_stances && state.stance == Stance::unshouldered)
        {
            state.stance = Stance::shouldering;
            animations.play(clips.shoulder, upper_channel, 0.0);
        }
        if (!shouldering && has_stances && state.stance == Stance::shouldered)
        {
            state.stance = Stance::unshouldering;
            if (ClipPlayer *player = animations.play(clips.shoulder, upper_channel, 0.0))
                player->reversed = true;
        }
        if (!shouldering && !has_stances && state.stance != Stance::unshouldered &&
            state.stance != Stance::reloading)
            state.stance = Stance::unshouldered;
        upper = animations.active(upper_channel);
        if (state.stance == Stance::shouldering && (upper == nullptr || upper->finished()))
            state.stance = Stance::shouldered;
        if (state.stance == Stance::unshouldering && (upper == nullptr || upper->finished()))
            state.stance = Stance::unshouldered;
        const bool reloading = input.reloading && !input.dead;
        if (!reloading && (state.stance == Stance::unshouldered || state.stance == Stance::shouldered))
        {
            const bool aimed = state.stance == Stance::shouldered;
            const std::string_view walk_clip = aimed && !clips.shoulder_walk.empty() ? clips.shoulder_walk : clips.walk;
            const std::string_view idle_clip = aimed && !clips.shoulder_idle.empty() ? clips.shoulder_idle : clips.idle;
            if (moving)
            {
                if (!animations.is(upper_channel, walk_clip))
                    animations.play(walk_clip, upper_channel, aimed || first ? 0.0 : quarter);
                if (ClipPlayer *player = animations.active(upper_channel); player != nullptr && player->name() == walk_clip)
                    player->speed = amount;
            }
            else if (!animations.is(upper_channel, idle_clip))
                animations.play(idle_clip, upper_channel,
                                first || input.snap ? 0.0 : (aimed ? 0.10000000149011612 : 0.25));
        }

        if (!moving)
        {
            if (!animations.is(lower_channel, "Stand"))
                animations.play("Stand", lower_channel, quarter);
        }
        else
        {
            ClipPlayer *lower = nullptr;
            if (forward_amount > strafe_amount)
            {
                if (forward_amount < 0.8F)
                {
                    if (!animations.is(lower_channel, "Walk"))
                        animations.play("Walk", lower_channel, quarter);
                    lower = animations.active(lower_channel);
                    if (lower != nullptr)
                        lower->speed = (forward_amount - 0.1F) / 0.4F;
                }
                else
                {
                    if (!animations.is(lower_channel, "Run"))
                        animations.play("Run", lower_channel, quarter);
                    lower = animations.active(lower_channel);
                    if (lower != nullptr)
                        lower->speed = 0.8F + (forward_amount - 0.8F);
                }
            }
            else
            {
                if (strafe_amount > 0.1F)
                {
                    const std::string_view gait = amount > 0.8F ? "Run" : "Walk";
                    if (!animations.is(lower_channel, gait))
                        animations.play(gait, lower_channel, quarter);
                }
                lower = animations.active(lower_channel);
                if (lower != nullptr)
                    lower->speed = strafe_amount;
            }
            if (lower != nullptr)
                lower->reversed = walk < 0.0F;
        }
    }

    const float pitch_degrees = (input.dead ? 0.0F : input.torso_pitch_radians) * 180.0F / std::numbers::pi_v<float>;
    set_tilt(pose, state, (pitch_degrees + 90.0F) / 180.0F);
}

void drive_first_person(CharacterPose &pose, DriveState &state, const FirstPersonInput &input)
{
    Animator &animations = pose.animations;
    const bool first = !state.set_up;
    if (first)
        set_up(pose, state);
    const bool snap = first || input.snap;
    const ModeClips clips = mode_clips(pose.rig(), input.mode);
    const bool moving = input.movement > 0.025F;
    const float progress = std::clamp(input.cue_progress, 0.0F, 1.0F);

    // A clip positioned by the simulation's own progress: paused, never looping.
    const auto place = [&](std::string_view clip, int channel, double blend, float at, bool restart) {
        if (clip.empty())
            return;
        ClipPlayer *player = animations.active(channel);
        if (restart || player == nullptr || player->name() != clip)
            player = animations.play(clip, channel, snap ? 0.0 : blend);
        if (player == nullptr)
            return;
        player->looping = false;
        player->pause();
        player->set_progress(at);
    };
    // A free-running loop on the stance channel.
    const auto loop = [&](std::string_view clip, double blend, float speed) {
        if (clip.empty())
            return;
        ClipPlayer *player = animations.active(upper_channel);
        if (player == nullptr || player->name() != clip || !player->playing())
            player = animations.play(clip, upper_channel, snap ? 0.0 : blend);
        if (player != nullptr)
        {
            player->play();
            player->speed = speed;
        }
    };

    std::string_view action{};
    switch (input.cue)
    {
    case world::ViewmodelCue::shoulder:
        place(clips.shoulder.empty() ? clips.idle : clips.shoulder, upper_channel, 0.0, progress, false);
        break;
    case world::ViewmodelCue::unshoulder:
        // The original replays the shoulder clip reversed.
        place(clips.shoulder.empty() ? clips.idle : clips.shoulder, upper_channel, 0.0, 1.0F - progress, false);
        break;
    case world::ViewmodelCue::shoulder_idle:
    case world::ViewmodelCue::shoulder_use:
        if (moving && !clips.shoulder_walk.empty())
            loop(clips.shoulder_walk, 0.0, input.movement);
        else
            loop(clips.shoulder_idle.empty() ? clips.idle : clips.shoulder_idle, 0.10000000149011612, 1.0F);
        if (input.cue == world::ViewmodelCue::shoulder_use)
            action = clips.shoulder_use.empty() ? clips.use : clips.shoulder_use;
        break;
    case world::ViewmodelCue::reload:
        place(clips.reload.empty() ? clips.idle : clips.reload, upper_channel, 0.25, progress, false);
        break;
    case world::ViewmodelCue::grenade_cook:
        loop(moving ? clips.walk : clips.idle, 0.25, moving ? input.movement : 1.0F);
        action = "Grenade_Cook";
        break;
    case world::ViewmodelCue::grenade_throw:
        loop(moving ? clips.walk : clips.idle, 0.25, moving ? input.movement : 1.0F);
        action = "Grenade_Throw";
        break;
    case world::ViewmodelCue::primary_use:
        action = clips.use;
        [[fallthrough]];
    case world::ViewmodelCue::idle:
    default:
        loop(moving ? clips.walk : clips.idle, 0.25, moving ? input.movement : 1.0F);
        break;
    }

    if (!action.empty())
    {
        // Progress running backwards is the next swing of a held tool.
        const bool restart = !state.action_requested || progress + 0.25F < state.last_action_progress;
        place(action, action_channel, 0.0, progress, restart);
        state.action_requested = true;
        state.last_action_progress = progress;
    }
    else
    {
        if (animations.active(action_channel) != nullptr)
            animations.clear(action_channel, snap ? 0.0 : 0.25);
        state.action_requested = false;
        state.last_action_progress = 0.0F;
    }

    // Player.UpdateAnimation in FPSMode: the body stands, the idle head sweep is
    // removed and the torso stays level (the camera carries the pitch).
    if (!animations.is(lower_channel, "Stand"))
        animations.play("Stand", lower_channel, 0.0);
    animations.clear(head_channel, 0.0);
    set_tilt(pose, state, 0.5F);
}

} // namespace openminerz::rig
