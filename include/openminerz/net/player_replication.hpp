#pragma once

#include "openminerz/inventory.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/world/player.hpp"

#include <cstdint>

namespace openminerz::net
{

struct LocalPlayerReplicationInput final
{
    world::Vec3 position;
    world::Vec3 velocity;
    float yaw{};
    float torso_pitch{};
    float movement_x{};
    float movement_y{};
    PlayerAnimationMode animation_mode{PlayerAnimationMode::generic};
    bool using_tool{};
    bool dead{};
    bool shouldering{};
    bool reloading{};
    bool throwing_grenade{};
    bool ready_to_throw_grenade{};
};

struct RemotePlayerReplicationState final
{
    world::Vec3 position;
    world::Vec3 velocity;
    float yaw{};
    float torso_pitch{};
    PlayerAnimationMode animation_mode{PlayerAnimationMode::generic};
    float movement_x{};
    float movement_y{};
    bool using_tool{};
    bool dead{};
    bool shouldering{};
    bool reloading{};
    bool throwing_grenade{};
    bool ready_to_throw_grenade{};
};

// Visual correction only: packet state remains authoritative for gameplay.
// Extrapolation covers one missed packet at the observed arrival rate and never
// more than 250 ms; each correction is spread over that same observed interval
// so nothing is left to jump when the next packet lands. Teleports, death and
// respawn snap immediately.
class RemotePlayerPresentation final
{
  public:
    void receive(const RemotePlayerReplicationState &state, float seconds) noexcept;
    [[nodiscard]] RemotePlayerReplicationState sample(float seconds) const noexcept;

  private:
    // The stock idle cadence is 20 Hz; bound the measured interval either side.
    static constexpr float minimum_interval = 0.02F;
    static constexpr float maximum_interval = 0.25F;
    // The hard ceiling on extrapolating or smoothing a silent peer.
    static constexpr float maximum_horizon = 0.3F;

    RemotePlayerReplicationState latest_;
    world::Vec3 correction_;
    float yaw_correction_{};
    float pitch_correction_{};
    float received_seconds_{};
    float interval_{0.05F};
    bool initialized_{};
};

// CastleMinerZGame._processPlayerExistsMessage. The whole handler is wrapped in
// `if (message.Sender.Tag == null)`, so a second PlayerExists from a peer that
// already has a Player does nothing at all. For a peer it has NOT seen before
// and with the GameScreen up, the receiver answers with:
//   * PlayerExistsMessage(RequestResponse = false)  -- only when asked,
//   * TimeOfDayMessage                              -- only when asked and host,
//   * TimeConnectedMessage                          -- only when asked,
//   * ChangeCarriedItemMessage                      -- ALWAYS, and
//   * CrateFocusMessage                             -- ALWAYS.
// The last two sit outside the RequestResponse branch, and that is the only
// reason a joiner's carried item ever reaches the peers already in the session:
// it answers each PlayerExists(false) it receives from them.
struct PlayerDiscoveryResponse final
{
    bool presence{};
    bool time_of_day{};
    bool time_connected{};
    bool carried_item{};
    bool crate_focus{};

    [[nodiscard]] constexpr bool any() const noexcept
    {
        return presence || time_of_day || time_connected || carried_item || crate_focus;
    }
    [[nodiscard]] friend constexpr bool operator==(const PlayerDiscoveryResponse &,
                                                   const PlayerDiscoveryResponse &) = default;
};

[[nodiscard]] constexpr PlayerDiscoveryResponse player_discovery_response(
    bool newly_discovered, bool request_response, bool local_peer_is_host) noexcept
{
    if (!newly_discovered)
        return {};
    return {request_response, request_response && local_peer_is_host, request_response, true, true};
}

// MeleePlayerMessage carries the item the attacker held when it swung, which a
// peer only knows from ChangeCarriedItemMessage. The original cross-checks
// nothing; we do, so the check has to tolerate the two facts that make a
// truthful swing look wrong: a peer that has not announced an item yet, and a
// switch that followed the swing but travelled on a different delivery class
// and overtook it.
[[nodiscard]] constexpr bool melee_item_matches_announcement(
    std::uint16_t swung_item, std::uint16_t announced_item, bool announcement_seen,
    std::uint16_t previous_item, float seconds_since_change,
    float grace_seconds = 1.0F) noexcept
{
    if (!announcement_seen || swung_item == announced_item)
        return true;
    return previous_item == swung_item && seconds_since_change >= 0.0F &&
           seconds_since_change <= grace_seconds;
}

// The two grenade bits of PlayerUpdateMessage are Player.PlayGrenadeAnim and
// Player.ReadyToThrowGrenade, and the original's local player drives both from
// its own avatar animation (GrenadeItem.ProcessInput, Player.OnUpdate,
// Player.UpdateAnimation):
//
//   Use pressed              PlayGrenadeAnim; "Grenade_Cook" starts, not looping
//   Use released, or 4 s     ReadyToThrowGrenade; grenadeCookTime stops counting
//   cook clip done AND ready "Grenade_Throw" plays
//   throw clip done          GrenadeMessage is sent (fuse 5 - grenadeCookTime),
//                            PlayGrenadeAnim clears
//
// So the grenade leaves the hand when the throw swing ENDS, never on release,
// and a remote peer (which gates the same two clips on the same two bits) sees
// cook, swing, grenade in that order. ReadyToThrowGrenade is cleared only by
// the next press or by death, so a player who has thrown keeps sending it.
class GrenadeArm final
{
  public:
    // Authored lengths of the avatar's GrenadeCook and GrenadeThrow clips.
    static constexpr float cook_clip_seconds = 0.9833333F;
    static constexpr float throw_clip_seconds = 0.3166666F;
    static constexpr float maximum_cook_seconds = 4.0F;

    constexpr void press() noexcept
    {
        *this = {};
        playing_ = true;
    }
    constexpr void release() noexcept
    {
        if (playing_)
            ready_ = true;
    }
    // The sequence was abandoned (the grenade was put away): nothing is thrown.
    constexpr void cancel() noexcept
    {
        playing_ = false;
        swinging_ = false;
    }
    // InGameHUD.KillPlayer clears both bits.
    constexpr void kill() noexcept
    {
        *this = {};
    }
    // True on the tick the grenade leaves the hand.
    [[nodiscard]] constexpr bool advance(float seconds) noexcept
    {
        if (!playing_)
            return false;
        if (!ready_)
        {
            cook_seconds_ += seconds;
            if (cook_seconds_ >= maximum_cook_seconds)
                ready_ = true;
        }
        if (!swinging_)
        {
            clip_seconds_ += seconds;
            if (ready_ && clip_seconds_ >= cook_clip_seconds)
            {
                swinging_ = true;
                swing_seconds_ = 0.0F;
            }
            return false;
        }
        swing_seconds_ += seconds;
        if (swing_seconds_ < throw_clip_seconds)
            return false;
        playing_ = false;
        swinging_ = false;
        return true;
    }

    // Player.OnUpdate's cook clock alone: the fuse burns until Use is released
    // or four seconds pass. For a caller whose swing is timed by the avatar's
    // own Grenade_Cook and Grenade_Throw clips (world::PlayerAnimator).
    constexpr void cook(float seconds) noexcept
    {
        if (!playing_ || ready_)
            return;
        cook_seconds_ += seconds;
        if (cook_seconds_ >= maximum_cook_seconds)
            ready_ = true;
    }
    // The Grenade_Throw clip ran out and GrenadeMessage was sent:
    // PlayGrenadeAnim = false. ReadyToThrowGrenade stays set, as in the original.
    constexpr void thrown() noexcept
    {
        playing_ = false;
        swinging_ = false;
    }

    // PlayerUpdatePayload::throwing_grenade_flag / ready_to_throw_grenade_flag.
    [[nodiscard]] constexpr bool playing() const noexcept { return playing_; }
    [[nodiscard]] constexpr bool ready() const noexcept { return ready_; }
    // Player.grenadeCookTime: how long Use was held, the fuse already burnt.
    [[nodiscard]] constexpr float cook_seconds() const noexcept { return cook_seconds_; }
    [[nodiscard]] constexpr bool swinging() const noexcept { return swinging_; }
    [[nodiscard]] constexpr float cook_clip_time() const noexcept { return clip_seconds_; }
    [[nodiscard]] constexpr float throw_clip_time() const noexcept { return swing_seconds_; }

  private:
    bool playing_{};
    bool ready_{};
    bool swinging_{};
    float cook_seconds_{};
    float clip_seconds_{};
    float swing_seconds_{};
};

[[nodiscard]] bool pack_half_single(float value, std::uint16_t &packed) noexcept;
[[nodiscard]] float unpack_half_single(std::uint16_t packed) noexcept;

[[nodiscard]] bool make_player_update(const LocalPlayerReplicationInput &input,
                                      ProtocolVersion protocol, PlayerUpdatePayload &payload,
                                      PayloadError &error);
[[nodiscard]] bool apply_player_update(const PlayerUpdatePayload &payload, ProtocolVersion protocol,
                                       RemotePlayerReplicationState &state);

[[nodiscard]] MessageDeliveryPolicy player_update_delivery(
    const PlayerUpdatePayload &payload) noexcept;

} // namespace openminerz::net
