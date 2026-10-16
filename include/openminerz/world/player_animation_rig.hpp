#pragma once

#include "openminerz/retail_rig.hpp"
#include "openminerz/world/player_animation.hpp"

// The seam between "which animation plays when" (world::PlayerAnimator) and
// the retail character rig that evaluates it (rig::CharacterPose): the plain
// per-channel list of clip, clip position and cross-fade.
//
//     rig::CharacterPose pose{rig};
//     pose.apply(world::to_channel_states(rig, animator.pose()));
//
// Clips are matched by the name the game registers them under. A name this
// content pack's character.omrc does not carry leaves its layer empty (the
// original would have thrown); it is never replaced by another clip.
namespace openminerz::world
{

[[nodiscard]] inline rig::ChannelStates to_channel_states(const rig::RetailRig &rig,
                                                         const PlayerAnimationPose &pose) noexcept
{
    rig::ChannelStates channels{};
    for (std::size_t index = 0; index < pose.size() && index < channels.size(); ++index)
    {
        const AnimationChannelPose &from = pose[index];
        rig::ChannelState &to = channels[index];
        if (from.current.clip != AnimationClip::none)
        {
            to.current = rig.find(animation_clip_info(from.current.clip).name);
            to.current_position = from.current.sample_ticks;
        }
        if (from.previous.clip != AnimationClip::none)
        {
            to.previous = rig.find(animation_clip_info(from.previous.clip).name);
            to.previous_position = from.previous.sample_ticks;
        }
        to.blend = from.weight;
    }
    return channels;
}

} // namespace openminerz::world
