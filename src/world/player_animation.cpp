#include "openminerz/world/player_animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace openminerz::world
{
namespace
{

using Clip = AnimationClip;
using Channel = AnimationChannel;

constexpr std::array<AnimationClipInfo, static_cast<std::size_t>(Clip::count)> clip_table{{
    {"", "", "", 0, 0, false, AnimationInfluence::all},
#define X(identifier, name, classic_path, modern_path, classic_ticks, modern_ticks, looping, influence)           \
    {name, classic_path, modern_path, classic_ticks, modern_ticks, looping, AnimationInfluence::influence},
#include "openminerz/world/player_animation_clips.inc"
#undef X
}};

// TimeSpan.TotalSeconds: ticks times 1e-7, as a double.
constexpr double seconds_per_tick = 1.0 / 10000000.0;

[[nodiscard]] constexpr double seconds(std::int64_t ticks) noexcept
{
    return static_cast<double>(ticks) * seconds_per_tick;
}

constexpr std::int64_t quarter_second = 2'500'000;
constexpr std::int64_t tenth_second = 1'000'000;

// The shipped game is 32-bit: float expressions are evaluated at double
// precision and narrowed where the result is stored.
[[nodiscard]] float narrow(double value) noexcept
{
    return static_cast<float>(value);
}

[[nodiscard]] std::string describe_player(const std::optional<AnimationPlayerState> &player)
{
    if (!player.has_value())
        return "-";
    std::uint32_t bits{};
    std::memcpy(&bits, &player->speed, sizeof(bits));
    static constexpr char digits[] = "0123456789abcdef";
    std::string text{animation_clip_info(player->clip).name};
    text += '@';
    text += std::to_string(player->ticks);
    text += 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
        text += digits[(bits >> shift) & 0xFU];
    text += player->looping ? 'L' : 'l';
    text += player->reversed ? 'R' : 'r';
    text += player->ping_pong ? 'P' : 'p';
    text += player->on_pong ? 'O' : 'o';
    if (!player->playing)
        text += 'S';
    return text;
}

} // namespace

const AnimationClipInfo &animation_clip_info(AnimationClip clip) noexcept
{
    const auto index = static_cast<std::size_t>(clip);
    return clip_table[index < clip_table.size() ? index : 0U];
}

AnimationClip animation_clip_from_name(std::string_view name) noexcept
{
    for (std::size_t index = 1; index < clip_table.size(); ++index)
        if (clip_table[index].name == name)
            return static_cast<AnimationClip>(index);
    return AnimationClip::none;
}

std::int64_t animation_clip_ticks(AnimationClip clip, AnimationGeneration generation) noexcept
{
    const AnimationClipInfo &info = animation_clip_info(clip);
    return generation == AnimationGeneration::modern ? info.modern_ticks : info.classic_ticks;
}

bool animation_clip_registered(AnimationClip clip, AnimationGeneration generation) noexcept
{
    return clip != AnimationClip::none && animation_clip_ticks(clip, generation) > 0;
}

AnimationClipSet animation_clip_set(PlayerAnimationMode mode, bool shouldering,
                                    AnimationGeneration generation) noexcept
{
    return animation_clip_set(static_cast<std::uint8_t>(mode), shouldering, generation);
}

AnimationClipSet animation_clip_set(std::uint8_t wire_mode, bool shouldering,
                                    AnimationGeneration generation) noexcept
{
    const bool modern = generation == AnimationGeneration::modern;
    AnimationClipSet set;
    // `gun(shoulder, reload, shouldered idle/walk/use, hip idle/walk/use)`.
    const auto gun = [&](Clip shoulder, Clip reload, Clip shoulder_idle, Clip shoulder_walk,
                         Clip shoulder_use, Clip idle, Clip walk, Clip use) {
        set.shoulder = shoulder;
        set.reload = reload;
        set.idle = shouldering ? shoulder_idle : idle;
        set.walk = shouldering ? shoulder_walk : walk;
        set.use = shouldering ? shoulder_use : use;
    };
    switch (wire_mode)
    {
    case 15: // Grenade keeps GenericUse.
        set.idle = Clip::GrenadeIdle;
        set.walk = Clip::GrenadeWalk;
        break;
    case 3: // Fist
        set.idle = Clip::FistIdle;
        set.walk = Clip::FistWalk;
        set.use = Clip::FistUse;
        break;
    case 17: // Chainsaw, 1.9.9.8 only; 1.6.3.8 has no such case and stays Generic.
        if (!modern)
            break;
        [[fallthrough]];
    case 1: // Pick
        set.idle = Clip::PickIdle;
        set.walk = Clip::PickWalk;
        set.use = Clip::PickUse;
        break;
    case 2: // Block
        set.idle = Clip::BlockIdle;
        set.walk = Clip::BlockWalk;
        set.use = Clip::BlockUse;
        break;
    case 4: // Assault
        gun(Clip::GunShoulder, Clip::GunReload, Clip::GunShoulderIdle, Clip::GunShoulderWalk,
            Clip::GunShoulderShoot, Clip::GunIdle, Clip::GunRun, Clip::GunShoot);
        break;
    case 5: // BoltRifle
        gun(Clip::RifleShoulder, Clip::RifleReload, Clip::RifleShoulderIdle,
            Clip::RifleShoulderWalk, Clip::RifleShoulderShoot, Clip::RifleIdle, Clip::RifleWalk,
            Clip::RifleShoot);
        break;
    case 8: // SMG
        gun(Clip::SMGShoulder, Clip::SMGReload, Clip::SMGShoulderIdle, Clip::SMGShoulderWalk,
            Clip::SMGShoulderShoot, Clip::SMGIdle, Clip::SMGWalk, Clip::SMGShoot);
        break;
    case 9: // LMG
        gun(Clip::LMGShoulder, Clip::LMGReload, Clip::LMGShoulderIdle, Clip::LMGShoulderWalk,
            Clip::LMGShoulderShoot, Clip::LMGIdle, Clip::LMGWalk, Clip::LMGShoot);
        break;
    case 6: // Pistol
        gun(Clip::PistolShoulder, Clip::PistolReload, Clip::PistolShoulderIdle,
            Clip::PistolShoulderWalk, Clip::PistolShoulderShoot, Clip::PistolIdle,
            Clip::PistolWalk, Clip::PistolShoot);
        break;
    case 7: // PumpShotgun: 1.6.3.8 borrows the assault rifle's hold; 1.9.9.8 has its own.
        if (modern)
            gun(Clip::PumpShotgunShoulder, Clip::PumpShotgunReload, Clip::PumpShotgunShoulderIdle,
                Clip::PumpShotgunShoulderWalk, Clip::PumpShotgunShoulderShoot,
                Clip::PumpShotgunIdle, Clip::PumpShotgunRun, Clip::PumpShotgunShoot);
        else
            gun(Clip::GunShoulder, Clip::PumpShotgunReload, Clip::GunShoulderIdle,
                Clip::GunShoulderWalk, Clip::PumpShotgunShoulderShoot, Clip::GunIdle, Clip::GunRun,
                Clip::PumpShotgunShoot);
        break;
    case 18: // LaserDrill, 1.9.9.8 only. No item carries this mode (the Laser Drill
             // itself is SpaceAssault) and its "LaserDrill*" names are never
             // registered, so the original would throw; a peer that sends it
             // gets the laser rifle hold instead.
        if (!modern)
            break;
        [[fallthrough]];
    case 10: // SpaceAssault
        gun(Clip::LaserGunShoulder, Clip::LaserGunReload, Clip::LaserGunShoulderIdle,
            Clip::LaserGunShoulderWalk, Clip::LaserGunShoulderShoot, Clip::LaserGunIdle,
            Clip::LaserGunRun, Clip::LaserGunShoot);
        break;
    case 14: // SpaceSMG
        gun(Clip::LaserSMGShoulder, Clip::LaserSMGReload, Clip::LaserSMGShoulderIdle,
            Clip::LaserSMGShoulderWalk, Clip::LaserSMGShoulderShoot, Clip::LaserSMGIdle,
            Clip::LaserSMGRun, Clip::LaserSMGShoot);
        break;
    case 12: // SpacePistol
        gun(Clip::LaserPistolShoulder, Clip::LaserPistolReload, Clip::LaserPistolShoulderIdle,
            Clip::LaserPistolShoulderWalk, Clip::LaserPistolShoulderShoot, Clip::LaserPistolIdle,
            Clip::LaserPistolRun, Clip::LaserPistolShoot);
        break;
    case 11: // SpaceBoltRifle
        gun(Clip::LaserRifleShoulder, Clip::LaserRifleReload, Clip::LaserRifleShoulderIdle,
            Clip::LaserRifleShoulderWalk, Clip::LaserRifleShoulderShoot, Clip::LaserRifleIdle,
            Clip::LaserRifleRun, Clip::LaserRifleShoot);
        break;
    case 13: // SpacePumpShotgun
        gun(Clip::LaserGunShoulder, Clip::LaserShotgunReload, Clip::LaserGunShoulderIdle,
            Clip::LaserGunShoulderWalk, Clip::LaserShotgunShoulderShoot, Clip::LaserGunIdle,
            Clip::LaserGunRun, Clip::LaserShotgunShoot);
        break;
    case 16: // RPG
        gun(Clip::GunShoulder, Clip::PumpShotgunReload, Clip::GunShoulderIdle,
            Clip::GunShoulderWalk, Clip::PumpShotgunShoulderShoot, Clip::RPGIdle, Clip::RPGWalk,
            Clip::RPGShoot);
        break;
    default: // Generic
        break;
    }
    return set;
}

std::int64_t animation_ticks_from_seconds(double value) noexcept
{
    const double milliseconds = value * 1000.0 + (value >= 0.0 ? 0.5 : -0.5);
    return static_cast<std::int64_t>(milliseconds) * 10'000;
}

void AnimationPlayerState::update(std::int64_t elapsed_ticks) noexcept
{
    const std::int64_t step =
        animation_ticks_from_seconds(seconds(elapsed_ticks) * static_cast<double>(speed));
    advance_clock(step);
}

AnimationPlayerState AnimationPlayerState::presented(float fraction) const noexcept
{
    auto shown = *this;
    const double alpha = std::isfinite(fraction) ? std::clamp(fraction, 0.0F, 1.0F) : 0.0;
    // Interpolate the actual next tick's rounded step, not a separately rounded
    // render delta. This meets the next simulation sample exactly at every speed.
    const auto step = animation_ticks_from_seconds(seconds(animation_frame_ticks) * speed);
    shown.advance_clock(static_cast<std::int64_t>(std::llround(static_cast<double>(step) * alpha)));
    return shown;
}

void AnimationPlayerState::advance_clock(std::int64_t step) noexcept
{
    if (playing)
        ticks += on_pong ? -step : step;
    if (duration <= 0)
    {
        ticks = 0;
        return;
    }
    if (ticks > duration)
    {
        if (ping_pong)
        {
            ticks = duration - (ticks - duration);
            on_pong = true;
        }
        else if (looping)
        {
            while (ticks > duration)
                ticks -= duration;
        }
        else
        {
            ticks = duration;
        }
    }
    else if (ticks < 0)
    {
        if (ping_pong)
        {
            if (looping)
            {
                on_pong = false;
                ticks = -ticks;
            }
            else
            {
                ticks = 0;
            }
        }
        else if (looping)
        {
            while (ticks < 0)
                ticks += duration;
        }
        else
        {
            ticks = 0;
        }
    }
}

float AnimationPlayerState::progress() const noexcept
{
    if (duration <= 0)
        return 1.0F;
    return narrow(seconds(ticks) / seconds(duration));
}

void AnimationPlayerState::set_progress(float value) noexcept
{
    on_pong = false;
    ticks = animation_ticks_from_seconds(static_cast<double>(value) * seconds(duration));
}

bool AnimationPlayerState::finished() const noexcept
{
    if (looping)
        return false;
    if (ping_pong && on_pong)
        return progress() <= 0.0F;
    return progress() >= 1.0F;
}

std::int64_t AnimationPlayerState::sample_ticks() const noexcept
{
    return reversed ? duration - ticks : ticks;
}

void AnimationBlender::play(std::optional<AnimationPlayerState> next,
                            std::int64_t blend_ticks) noexcept
{
    previous = current;
    current = next;
    blend_total = blend_ticks;
    blend_current = 0;
}

void AnimationBlender::update(std::int64_t elapsed_ticks) noexcept
{
    if (!current.has_value() && !previous.has_value())
        return;
    if (current.has_value())
        current->update(elapsed_ticks);
    if (previous.has_value())
        previous->update(elapsed_ticks);
    blend_current += elapsed_ticks;
    if (weight() >= 1.0F)
        previous.reset();
}

float AnimationBlender::weight() const noexcept
{
    if (blend_total <= 0)
        return 1.0F;
    return std::min(1.0F, narrow(seconds(blend_current) / seconds(blend_total)));
}

PlayerAnimator::PlayerAnimator(AnimationGeneration generation) noexcept : generation_{generation}
{
    setup();
}

AnimationPlayerState &PlayerAnimator::play(AnimationClip clip, AnimationChannel channel,
                                           std::int64_t blend_ticks) noexcept
{
    // AvatarAnimationManager.GetAnimation -> AnimationPlayer.SetClip: a fresh
    // clock at speed 1, with the looping flag the clip was registered with.
    AnimationPlayerState next;
    next.clip = clip;
    next.duration = animation_clip_ticks(clip, generation_);
    next.looping = animation_clip_info(clip).looping;
    at(channel).play(next, blend_ticks);
    return *at(channel).current;
}

void PlayerAnimator::clear(AnimationChannel channel, std::int64_t blend_ticks) noexcept
{
    at(channel).play(std::nullopt, blend_ticks);
}

void PlayerAnimator::setup() noexcept
{
    channels_ = {};
    state_ = ShoulderState::unshouldered;
    using_animation_playing_ = false;
    moving_ = false;
    running_ = false;
    walk_speed_ = 1.0F;
    play(Clip::Stand, Channel::lower, 0);
    play(Clip::Tilt, Channel::tilt, 0).playing = false;
    play(Clip::GenericIdle, Channel::upper, 0);
    play(Clip::IdleHead, Channel::head, 0).ping_pong = true;
}

void PlayerAnimator::play_grenade_reset() noexcept
{
    play(Clip::Grenade_Reset, Channel::upper_use, 0);
}

void PlayerAnimator::active_item_read(bool empty_handed) noexcept
{
    if (generation_ == AnimationGeneration::modern && empty_handed)
        state_ = ShoulderState::unshouldered;
}

bool PlayerAnimator::grenade_animation_playing() const noexcept
{
    const auto &use = channel(Channel::upper_use).current;
    return use.has_value() && (use->clip == Clip::Grenade_Reset || use->clip == Clip::Grenade_Throw ||
                               use->clip == Clip::Grenade_Cook);
}

bool PlayerAnimator::using_animation_playing() const noexcept
{
    return using_animation_playing_ || grenade_animation_playing();
}

float PlayerAnimator::shoulder_zoom() const noexcept
{
    const auto &upper = channel(Channel::upper).current;
    const float progress = upper.has_value() ? std::clamp(upper->progress(), 0.0F, 1.0F) : 0.0F;
    switch (state_)
    {
    case ShoulderState::shouldered:
        return 1.0F;
    case ShoulderState::shouldering:
        return progress;
    case ShoulderState::unshouldering:
        return 1.0F - progress;
    case ShoulderState::unshouldered:
    case ShoulderState::reloading:
    default:
        return 0.0F;
    }
}

PlayerAnimationResult PlayerAnimator::update(const PlayerAnimationInput &input,
                                             const FinishReload &finish_reload)
{
    PlayerAnimationResult result;
    bool reloading = input.reloading;
    bool play_grenade = input.play_grenade_anim;
    const auto finish = [&] {
        // Player.FinishReload.
        reloading = input.local && input.holding_gun && finish_reload && finish_reload();
        result.finished_reload = true;
    };

    const float forward = std::fabs(input.walk);
    const float sideways = std::fabs(input.strafe);
    const float amount = std::max(forward, sideways);
    running_ = false;
    moving_ = amount >= 0.1F;
    const AnimationClipSet set = animation_clip_set(input.mode, input.shouldering, generation_);

    AnimationBlender &lower = at(Channel::lower);
    AnimationBlender &upper = at(Channel::upper);
    AnimationBlender &use = at(Channel::upper_use);
    AnimationBlender &death = at(Channel::death);

    if (input.dead)
    {
        if (!death.current.has_value())
            play(Clip::Die, Channel::death, quarter_second);
    }
    else if (death.current.has_value())
    {
        clear(Channel::death, quarter_second);
    }

    if (input.using_tool)
    {
        play(set.use, Channel::upper_use, 0);
        using_animation_playing_ = true;
    }
    else if (play_grenade && !use.current.has_value())
    {
        result.grenade_arm_cue = true;
        play(Clip::Grenade_Cook, Channel::upper_use, quarter_second);
    }
    else if (use.current.has_value())
    {
        use.current->looping = false;
        if (use.current->finished())
        {
            if (play_grenade)
            {
                if (use.current->clip == Clip::Grenade_Throw)
                {
                    result.throw_grenade = input.local && input.holding_grenade;
                    play_grenade = false;
                }
                else if (input.ready_to_throw_grenade)
                {
                    play(Clip::Grenade_Throw, Channel::upper_use, 0);
                }
            }
            else
            {
                clear(Channel::upper_use, quarter_second);
                using_animation_playing_ = false;
            }
        }
    }

    if (input.underwater && !input.fps_mode)
    {
        if (!lower.current.has_value() || lower.current->clip != Clip::Swim)
            play(Clip::Swim, Channel::lower, quarter_second);
        if (amount < 0.1F)
            moving_ = false;
    }
    else
    {
        const auto upper_finished = [&] {
            return upper.current.has_value() && upper.current->finished();
        };
        if (state_ == ShoulderState::reloading && upper_finished())
        {
            state_ = ShoulderState::unshouldered;
            finish();
        }
        if (reloading)
        {
            if (state_ == ShoulderState::shouldered && !using_animation_playing_)
            {
                state_ = ShoulderState::unshouldering;
                play(set.shoulder, Channel::upper, quarter_second).reversed = true;
            }
            else if (state_ == ShoulderState::unshouldered && !using_animation_playing_)
            {
                if (set.reload != Clip::none)
                {
                    state_ = ShoulderState::reloading;
                    AnimationPlayerState &clip = play(set.reload, Channel::upper, quarter_second);
                    if (((input.local && input.holding_gun) || input.presentation_reload) &&
                        input.reload_ticks > 0) // (GunFX) presentation_reload
                        clip.speed = narrow(seconds(clip.duration) / seconds(input.reload_ticks));
                    // `InGameHUD.Instance.ActiveInventoryItem is GunInventoryItem`.
                    if (input.local)
                        active_item_read(input.empty_handed);
                    result.reload_cue_started = true;
                }
                else
                {
                    finish();
                }
            }
        }
        else if (state_ == ShoulderState::reloading)
        {
            state_ = ShoulderState::unshouldered;
            result.reload_cue_stopped = true;
        }
        const bool can_shoulder = set.shoulder != Clip::none;
        if (input.shouldering && can_shoulder && state_ == ShoulderState::unshouldered)
        {
            state_ = ShoulderState::shouldering;
            play(set.shoulder, Channel::upper, 0);
        }
        if (!input.shouldering && can_shoulder && state_ == ShoulderState::shouldered)
        {
            state_ = ShoulderState::unshouldering;
            play(set.shoulder, Channel::upper, 0).reversed = true;
        }
        if (!input.shouldering && !can_shoulder && state_ == ShoulderState::shouldered)
            state_ = ShoulderState::unshouldered;
        if (state_ == ShoulderState::shouldering && upper_finished())
            state_ = ShoulderState::shouldered;
        if (state_ == ShoulderState::unshouldering && upper_finished())
            state_ = ShoulderState::unshouldered;
        if (!reloading &&
            (state_ == ShoulderState::unshouldered || state_ == ShoulderState::shouldered))
        {
            if (moving_)
            {
                if (!upper.current.has_value() || upper.current->clip != set.walk)
                    play(set.walk, Channel::upper, input.shouldering ? 0 : quarter_second);
                if (upper.current.has_value() && upper.current->clip == set.walk)
                    upper.current->speed = std::max(forward, sideways);
            }
            else if (!upper.current.has_value() || upper.current->clip != set.idle)
            {
                play(set.idle, Channel::upper, input.shouldering ? tenth_second : quarter_second);
            }
        }

        if (amount < 0.1F)
        {
            if (!input.fps_mode &&
                (!lower.current.has_value() || lower.current->clip != Clip::Stand))
                play(Clip::Stand, Channel::lower, quarter_second);
            moving_ = false;
        }
        else
        {
            const bool run = !(forward < 0.8F);
            if (run)
                running_ = true;
            // (num - 0.1f) / 0.4f below 0.8, and 0.8f + (num - 0.8f) above it.
            const float gait =
                run ? narrow(static_cast<double>(0.8F) +
                             (static_cast<double>(forward) - static_cast<double>(0.8F)))
                    : narrow((static_cast<double>(forward) - static_cast<double>(0.1F)) /
                             static_cast<double>(0.4F));
            if (input.fps_mode)
            {
                walk_speed_ = gait;
            }
            else if (!input.fly_mode)
            {
                const auto require = [&](Clip clip) {
                    if (!lower.current.has_value() || lower.current->clip != clip)
                        play(clip, Channel::lower, quarter_second);
                };
                if (forward > sideways)
                {
                    require(run ? Clip::Run : Clip::Walk);
                    lower.current->speed = gait;
                    walk_speed_ = gait;
                }
                else
                {
                    if (sideways > 0.1F)
                        require(amount > 0.8F ? Clip::Run : Clip::Walk);
                    if (lower.current.has_value())
                        lower.current->speed = sideways;
                }
                if (lower.current.has_value())
                    lower.current->reversed = input.walk < 0.0F;
            }
        }
        if (input.fps_mode)
            active_item_read(input.empty_handed);
    }

    // PlayAnimation(1, _torsoPitchAnimation, 0) hands the blender the paused
    // player it already holds, with a zero blend: nothing to fade from.
    AnimationBlender &tilt = at(Channel::tilt);
    tilt.previous.reset();
    tilt.blend_total = 0;
    tilt.blend_current = 0;
    if (input.fps_mode)
    {
        if (!lower.current.has_value() || lower.current->clip != Clip::Stand)
            play(Clip::Stand, Channel::lower, 0);
        clear(Channel::head, 0);
        if (tilt.current.has_value())
            tilt.current->set_progress(0.5F);
    }
    else if (tilt.current.has_value())
    {
        // Angle.Degrees, then (degrees + 90) / 180.
        const float degrees =
            narrow(static_cast<double>(input.torso_pitch_radians) * 180.0 / 3.14159265358979323846);
        tilt.current->set_progress(narrow((static_cast<double>(degrees) + 90.0) / 180.0));
    }

    result.reloading = reloading;
    result.play_grenade_anim = play_grenade;
    return result;
}

void PlayerAnimator::advance(std::int64_t elapsed_ticks) noexcept
{
    for (AnimationBlender &blender : channels_)
        blender.update(elapsed_ticks);
}

PlayerAnimationPose PlayerAnimator::pose() const noexcept
{
    PlayerAnimationPose pose{};
    for (std::size_t index = 0; index < channels_.size(); ++index)
    {
        const AnimationBlender &blender = channels_[index];
        if (blender.current.has_value())
            pose[index].current = {blender.current->clip, blender.current->sample_ticks()};
        if (blender.previous.has_value())
            pose[index].previous = {blender.previous->clip, blender.previous->sample_ticks()};
        // AnimBlender.Update fades a clip in over its blend time even when the
        // channel was empty before it (Die on channel 5, Grenade_Cook on 3): the
        // bones go from whatever the channels beneath wrote towards the clip.
        // The weight is therefore the blend clock's, with or without a previous.
        pose[index].weight = blender.weight();
    }
    return pose;
}

PlayerAnimator PlayerAnimator::presented(float fraction) const noexcept
{
    auto shown = *this;
    const float alpha = std::isfinite(fraction) ? std::clamp(fraction, 0.0F, 1.0F) : 0.0F;
    for (AnimationBlender &blender : shown.channels_)
    {
        if (!blender.current.has_value() && !blender.previous.has_value())
            continue;
        if (blender.current.has_value())
            blender.current = blender.current->presented(alpha);
        if (blender.previous.has_value())
            blender.previous = blender.previous->presented(alpha);
        blender.blend_current += static_cast<std::int64_t>(
            std::llround(static_cast<double>(animation_frame_ticks) * alpha));
        if (blender.weight() >= 1.0F)
            blender.previous.reset();
    }
    return shown;
}

std::string PlayerAnimator::describe() const
{
    std::string text;
    for (std::size_t index = 0; index < channels_.size(); ++index)
    {
        const AnimationBlender &blender = channels_[index];
        text += " c";
        text += std::to_string(index);
        text += '=';
        text += describe_player(blender.current);
        if (blender.previous.has_value())
        {
            text += '|';
            text += describe_player(blender.previous);
            text += '|';
            text += std::to_string(blender.blend_current);
            text += '/';
            text += std::to_string(blender.blend_total);
        }
    }
    return text;
}

bool FootstepClock::step(const PlayerAnimator &animator, bool active, bool audible,
                         std::int64_t frame_ticks) noexcept
{
    if (!animator.moving() || !active)
        return false;
    elapsed_ticks += animation_ticks_from_seconds(seconds(frame_ticks) *
                                                  static_cast<double>(animator.walk_speed()));
    if (elapsed_ticks < max_ticks)
        return false;
    elapsed_ticks = 0;
    max_ticks = animator.running() ? 4'000'000 : 5'000'000;
    return audible;
}

PlayerAnimationInput remote_animation_input(const RemoteAnimationUpdate &update) noexcept
{
    constexpr double pi = 3.14159265358979323846;
    PlayerAnimationInput input;
    input.using_tool = (update.flags & 0x01U) != 0U;
    input.dead = (update.flags & 0x02U) != 0U;
    input.shouldering = (update.flags & 0x04U) != 0U;
    input.reloading = (update.flags & 0x08U) != 0U;
    input.play_grenade_anim = (update.flags & 0x10U) != 0U;
    input.ready_to_throw_grenade = (update.flags & 0x20U) != 0U;
    input.mode = update.mode;
    if (input.dead)
        return input;
    // Angle.FromRevolutions(pitch / 510f) - Angle.FromDegrees(90f).
    const float revolutions = narrow(static_cast<double>(update.pitch) / 510.0);
    const float raised = narrow(static_cast<double>(revolutions) * 2.0 * pi);
    const float quarter = narrow(90.0 * pi / 180.0);
    input.torso_pitch_radians = narrow(static_cast<double>(raised) - static_cast<double>(quarter));
    const auto axis = [](unsigned nibble) {
        return narrow(static_cast<double>(nibble) / 14.0 * 2.0 - 1.0);
    };
    input.strafe = axis(update.movement & 0x0FU);
    input.walk = axis(static_cast<unsigned>(update.movement) >> 4U);
    return input;
}

} // namespace openminerz::world
