#pragma once

// WHICH clip plays WHEN on the retail character.
//
// This is the animation state machine's side of the seam described in
// retail_rig.hpp. It only ever talks to rig::Animator through the calls the
// original Player.UpdateAnimation makes on Avatar.Animations (Play,
// ClearAnimation, PlayAnimation, and the returned player's Speed / Reversed /
// Looping / Progress / Finished), with the game's registered clip names and
// channels, so the port of that state machine can replace these functions
// without touching sampling, blending, skinning or drawing.
//
//   drive_third_person   one Player.UpdateAnimation call for a player seen from
//                        outside, from the fields a PlayerUpdateMessage carries.
//   drive_first_person   the local player's arms. OpenMinerZ's simulation owns
//                        the timing of a swing, shot, shoulder and reload and
//                        reports it as a cue plus progress, so the matching clip
//                        is positioned by that progress instead of free-running.

#include "openminerz/inventory.hpp"
#include "openminerz/retail_rig.hpp"
#include "openminerz/world/player.hpp"

#include <string_view>

namespace openminerz::rig
{

// The names Player.UpdateAnimation selects for a PlayerMode (text, text2, id,
// text3, text4 and their shouldered variants). Empty = the mode has none.
struct ModeClips final
{
    std::string_view idle{"GenericIdle"};
    std::string_view walk{"GenericWalk"};
    std::string_view use{"GenericUse"};
    std::string_view shoulder{};
    std::string_view shoulder_idle{};
    std::string_view shoulder_walk{};
    std::string_view shoulder_use{};
    std::string_view reload{};
};
[[nodiscard]] ModeClips mode_clips(const RetailRig &rig, PlayerAnimationMode mode) noexcept;

// Player.currentAnimState.
enum class Stance : std::uint8_t
{
    unshouldered,
    shouldering,
    shouldered,
    unshouldering,
    reloading,
};

struct DriveState final
{
    Stance stance{Stance::unshouldered};
    ClipPlayer *tilt{};
    bool set_up{};
    bool using_animation_playing{};
    bool action_requested{};
    float last_action_progress{};
};

struct ThirdPersonInput final
{
    float walk{};   // forward positive, -1..1 (Player.UpdateAnimation's walkAmount)
    float strafe{}; // -1..1
    float torso_pitch_radians{};
    PlayerAnimationMode mode{PlayerAnimationMode::generic};
    bool using_tool{};
    bool dead{};
    bool shouldering{};
    bool reloading{};
    bool play_grenade{};
    bool ready_to_throw_grenade{};
    bool underwater{};
    // The first frame a character is seen: every clip starts without a blend.
    bool snap{};
};

void drive_third_person(CharacterPose &pose, DriveState &state, const ThirdPersonInput &input);

struct FirstPersonInput final
{
    PlayerAnimationMode mode{PlayerAnimationMode::generic};
    world::ViewmodelCue cue{world::ViewmodelCue::idle};
    float cue_progress{};
    // 0..1 share of walking speed; the stance walk clip plays at this speed.
    float movement{};
    bool snap{};
};

void drive_first_person(CharacterPose &pose, DriveState &state, const FirstPersonInput &input);

} // namespace openminerz::rig
