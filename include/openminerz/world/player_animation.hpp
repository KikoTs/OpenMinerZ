#pragma once

#include "openminerz/inventory.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Which animation plays when.
//
// This is Player.UpdateAnimation, AvatarAnimationCollection, AnimBlender and
// AnimationPlayer of the shipped game (1.6.3.8 and 1.9.9.8), reduced to plain
// data: for every animation channel the clip that is playing, its clock in
// 100 ns ticks, its speed, and the clip it is cross-fading from. Nothing here
// samples a bone; a renderer reads PlayerAnimator::pose() and does that.
//
// Parity is held frame for frame against the original by
// tools/OriginalClientInterop/AnimationParityProbe (the live differential
// test) and tests/animation_parity_pinned.hpp (the same timelines, recorded).
namespace openminerz::world
{

enum class AnimationGeneration : std::uint8_t
{
    classic, // 1.6.3.8
    modern,  // 1.9.9.8
};

// Player.AnimChannels. Channels are applied in this order, each over the last.
enum class AnimationChannel : std::uint8_t
{
    lower = 0,     // Stand / Walk / Run / Swim: the whole body
    tilt = 1,      // the paused Tilt clip, positioned by the torso pitch
    upper = 2,     // the held item's idle, walk, shoulder and reload clips
    upper_use = 3, // the one-shot use, fire and grenade clips
    head = 4,      // IdleHead, third person only
    death = 5,     // Die
    count = 6,
};

// The bone list a clip was registered with.
enum class AnimationInfluence : std::uint8_t
{
    all,
    back_upper,
    collar_right,
    neck,
    // BackLower and below, except both collars and what hangs off them.
    back_lower_without_collars,
};

enum class AnimationClip : std::uint8_t
{
    none,
#define X(identifier, name, classic_path, modern_path, classic_ticks, modern_ticks, looping, influence)           \
    identifier,
#include "openminerz/world/player_animation_clips.inc"
#undef X
    count,
};

struct AnimationClipInfo final
{
    // The name the game registers and plays the clip under ("LaserGunShoot").
    std::string_view name;
    // The content path that name is loaded from in each generation.
    std::string_view classic_path;
    std::string_view modern_path;
    std::int64_t classic_ticks{};
    std::int64_t modern_ticks{};
    bool looping{};
    AnimationInfluence influence{AnimationInfluence::all};
};

[[nodiscard]] const AnimationClipInfo &animation_clip_info(AnimationClip clip) noexcept;
[[nodiscard]] AnimationClip animation_clip_from_name(std::string_view name) noexcept;
[[nodiscard]] std::int64_t animation_clip_ticks(AnimationClip clip,
                                                AnimationGeneration generation) noexcept;
[[nodiscard]] bool animation_clip_registered(AnimationClip clip,
                                             AnimationGeneration generation) noexcept;

// The five names Player.UpdateAnimation picks from PlayerMode and Shouldering.
struct AnimationClipSet final
{
    AnimationClip use{AnimationClip::GenericUse};
    AnimationClip idle{AnimationClip::GenericIdle};
    AnimationClip walk{AnimationClip::GenericWalk};
    AnimationClip shoulder{AnimationClip::none}; // none: the item cannot be shouldered
    AnimationClip reload{AnimationClip::none};   // none: the item has no reload clip
};

[[nodiscard]] AnimationClipSet animation_clip_set(PlayerAnimationMode mode, bool shouldering,
                                                  AnimationGeneration generation) noexcept;
// The same from the raw PlayerUpdateMessage byte (1.9.9.8 adds 17 and 18).
[[nodiscard]] AnimationClipSet animation_clip_set(std::uint8_t wire_mode, bool shouldering,
                                                  AnimationGeneration generation) noexcept;

inline constexpr std::int64_t animation_ticks_per_second = 10'000'000;
// XNA's fixed step: TargetElapsedTime is 166667 ticks, not 1/60 s.
inline constexpr std::int64_t animation_frame_ticks = 166'667;

// TimeSpan.FromSeconds: whole milliseconds, half away from zero. Every clip
// clock goes through it, which is why a clip advances 17 ms per 16.67 ms frame.
[[nodiscard]] std::int64_t animation_ticks_from_seconds(double seconds) noexcept;

// DNA.Drawing.Animation.AnimationPlayer.
struct AnimationPlayerState final
{
    AnimationClip clip{AnimationClip::none};
    std::int64_t duration{};
    std::int64_t ticks{};
    float speed{1.0F};
    bool looping{true};
    bool reversed{};
    bool ping_pong{};
    bool on_pong{};
    bool playing{true};

    void update(std::int64_t elapsed_ticks) noexcept;
    [[nodiscard]] float progress() const noexcept;
    void set_progress(float value) noexcept;
    [[nodiscard]] bool finished() const noexcept;
    // Where in the clip the pose is read: Duration - time when reversed.
    [[nodiscard]] std::int64_t sample_ticks() const noexcept;
    // Interpolate towards the next fixed tick without changing the live clock.
    [[nodiscard]] AnimationPlayerState presented(float fraction) const noexcept;

  private:
    void advance_clock(std::int64_t step) noexcept;
};

// DNA.Drawing.Animation.AnimBlender.
struct AnimationBlender final
{
    std::optional<AnimationPlayerState> current;
    std::optional<AnimationPlayerState> previous;
    std::int64_t blend_total{2'500'000};
    std::int64_t blend_current{};

    void play(std::optional<AnimationPlayerState> next, std::int64_t blend_ticks) noexcept;
    void update(std::int64_t elapsed_ticks) noexcept;
    // The weight of `current` over `previous` (or over the pose beneath).
    [[nodiscard]] float weight() const noexcept;
};

// Player.AnimationState.
enum class ShoulderState : std::uint8_t
{
    unshouldered = 0,
    shouldering = 1,
    unshouldering = 2,
    shouldered = 3,
    reloading = 4,
};

struct PlayerAnimationInput final
{
    float walk{};   // Movement.Y
    float strafe{}; // Movement.X
    float torso_pitch_radians{};
    std::uint8_t mode{static_cast<std::uint8_t>(PlayerAnimationMode::fist)};
    bool using_tool{};
    bool shouldering{};
    bool reloading{};
    bool dead{};
    bool play_grenade_anim{};
    bool ready_to_throw_grenade{};
    // Player.FPSMode: the avatar hangs in the first-person scene. True for the
    // living local player only; a dead local player and every remote are false.
    bool fps_mode{};
    bool local{};
    bool fly_mode{};
    bool underwater{};
    // Local player only: the item in hand, as InGameHUD.ActiveInventoryItem.
    bool holding_gun{};
    std::int64_t reload_ticks{}; // GunClass.ReloadTime
    // (GunFX) Presentation only, never in retail: another player's reload clip
    // is time-scaled to `reload_ticks` of the gun they carry, as your own is,
    // so a pack's reload plays over their reload window once instead of
    // repeating at its own length. False (retail) unless a client mod asks.
    bool presentation_reload{};
    bool holding_grenade{};
    bool empty_handed{};
};

struct PlayerAnimationResult final
{
    // Player.Reloading and Player.PlayGrenadeAnim after the pass.
    bool reloading{};
    bool play_grenade_anim{};
    // Player.FinishReload ran: this cycle's rounds go into the gun now.
    bool finished_reload{};
    // GrenadeMessage.Send: the grenade leaves the hand on this frame.
    bool throw_grenade{};
    // SoundManager cues played from inside UpdateAnimation.
    bool grenade_arm_cue{};
    bool reload_cue_started{};
    bool reload_cue_stopped{};
};

// What a renderer needs for one channel.
struct AnimationLayerPose final
{
    AnimationClip clip{AnimationClip::none};
    std::int64_t sample_ticks{};
};

struct AnimationChannelPose final
{
    AnimationLayerPose current;
    AnimationLayerPose previous;
    // 1 when nothing is fading. Otherwise bones are interpolated from
    // `previous` (or, where it does not own a bone, from the channels beneath)
    // towards `current` (or back towards the channels beneath when it is none).
    float weight{1.0F};
};

using PlayerAnimationPose =
    std::array<AnimationChannelPose, static_cast<std::size_t>(AnimationChannel::count)>;

class PlayerAnimator final
{
  public:
    // GunInventoryItem.Reload for the local player's gun: inserts one cycle's
    // rounds and returns true when the gun wants another cycle (shotguns).
    using FinishReload = std::function<bool()>;

    explicit PlayerAnimator(AnimationGeneration generation = AnimationGeneration::classic) noexcept;

    [[nodiscard]] AnimationGeneration generation() const noexcept
    {
        return generation_;
    }

    // Player.SetupAnimation.
    void setup() noexcept;
    // Player.UpdateAnimation, minus the velocity it also writes.
    PlayerAnimationResult update(const PlayerAnimationInput &input,
                                 const FinishReload &finish_reload = {});
    // Avatar.OnUpdate -> LayeredAnimationPlayer.Update.
    void advance(std::int64_t elapsed_ticks = animation_frame_ticks) noexcept;
    // Player.ProcessGrenadeMessage: Play("Grenade_Reset", 3, 0).
    void play_grenade_reset() noexcept;
    // 1.9.9.8 PlayerInventory.ActiveInventoryItem: reading it while the hands
    // are empty forces the shoulder state back to Unshouldered.
    void active_item_read(bool empty_handed) noexcept;

    // Player.UsingAnimationPlaying: slot changes are refused while it holds.
    [[nodiscard]] bool using_animation_playing() const noexcept;
    [[nodiscard]] bool grenade_animation_playing() const noexcept;
    // The private usingAnimationPlaying flag alone: a use or fire clip is
    // running. A requested reload and the unshoulder before it wait on this.
    [[nodiscard]] bool use_clip_playing() const noexcept
    {
        return using_animation_playing_;
    }
    [[nodiscard]] ShoulderState shoulder_state() const noexcept
    {
        return state_;
    }
    // 0 at DefaultFOV, 1 at DefaultFOV / ShoulderMagnification.
    [[nodiscard]] float shoulder_zoom() const noexcept;
    // Player._isMoveing / _isRunning / _walkSpeed, which pace the footsteps.
    [[nodiscard]] bool moving() const noexcept
    {
        return moving_;
    }
    [[nodiscard]] bool running() const noexcept
    {
        return running_;
    }
    [[nodiscard]] float walk_speed() const noexcept
    {
        return walk_speed_;
    }

    [[nodiscard]] const AnimationBlender &channel(AnimationChannel channel) const noexcept
    {
        return channels_[static_cast<std::size_t>(channel)];
    }
    [[nodiscard]] PlayerAnimationPose pose() const noexcept;
    // Render-only snapshot; never runs gameplay transitions or emits events.
    [[nodiscard]] PlayerAnimator presented(float fraction) const noexcept;
    // One line in the probe's trace format, for differential tests.
    [[nodiscard]] std::string describe() const;

  private:
    AnimationPlayerState &play(AnimationClip clip, AnimationChannel channel,
                               std::int64_t blend_ticks) noexcept;
    void clear(AnimationChannel channel, std::int64_t blend_ticks) noexcept;
    [[nodiscard]] AnimationBlender &at(AnimationChannel channel) noexcept
    {
        return channels_[static_cast<std::size_t>(channel)];
    }

    AnimationGeneration generation_;
    std::array<AnimationBlender, static_cast<std::size_t>(AnimationChannel::count)> channels_{};
    ShoulderState state_{ShoulderState::unshouldered};
    bool using_animation_playing_{};
    bool moving_{};
    bool running_{};
    float walk_speed_{1.0F};
};

// Player.UpdateAudio's footstep clock: a OneShotTimer advanced by the frame
// time scaled with _walkSpeed, 0.5 s walking and 0.4 s running.
struct FootstepClock final
{
    std::int64_t elapsed_ticks{};
    std::int64_t max_ticks{5'000'000};

    // True on the frame a FootStep cue plays. `audible` is the original's
    // `!_flyMode && !InWater && InContact`; the clock restarts either way.
    [[nodiscard]] bool step(const PlayerAnimator &animator, bool active, bool audible,
                            std::int64_t frame_ticks = animation_frame_ticks) noexcept;
};

// PlayerUpdateMessage.RecieveData, the part that drives a remote player's
// animation: bytes 12..16 of the payload (pitch, yaw, flags, mode, movement).
struct RemoteAnimationUpdate final
{
    std::uint8_t pitch{};
    std::uint8_t flags{};
    std::uint8_t mode{};
    std::uint8_t movement{};
};

// PlayerUpdateMessage.Apply's UpdateAnimation arguments for a remote player.
[[nodiscard]] PlayerAnimationInput remote_animation_input(const RemoteAnimationUpdate &update) noexcept;

// The one-frame UsingTool pulse of InventoryItem.ProcessInput and
// BlockInventoryItem.ProcessInput: the item's CoolDownTimer (a OneShotTimer
// that InventoryItem.Update advances every frame) gates each swing, and
// UsingTool is true only on the frame the timer is found expired.
struct ItemCooldown final
{
    std::int64_t elapsed_ticks{};
    std::int64_t max_ticks{};

    void update(std::int64_t frame_ticks = animation_frame_ticks) noexcept
    {
        elapsed_ticks += frame_ticks;
    }
    [[nodiscard]] bool expired() const noexcept
    {
        return elapsed_ticks >= max_ticks;
    }
    void reset() noexcept
    {
        elapsed_ticks = 0;
    }
    // True when a swing (or placement) starts on this input sample.
    [[nodiscard]] bool trigger(bool wanted) noexcept
    {
        if (!wanted || !expired())
            return false;
        reset();
        return true;
    }
};

} // namespace openminerz::world
