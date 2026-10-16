#pragma once

#include "openminerz/game_mode.hpp"
#include "openminerz/net/payload_codec.hpp"

#include <cstddef>
#include <cstdint>

// Decisions an unmodified Castle Miner Z peer makes, expressed as pure
// functions so they can be pinned by tests and compared with the original
// assemblies (tools/OriginalClientInterop/SemanticsParityProbe executes the
// original code these are checked against).
//
// The stock session host is a blind relay: DNA.Net forwards whatever a guest
// broadcasts, and every rule that looks like authority (who may pick an item
// up, where an explosion happened, who killed what) lives in the gameplay
// handlers that EVERY peer runs. A native host that second-guesses a stock
// guest with rules the original does not have therefore does not make the
// session safer, it makes it disagree: the guest has already applied its own
// local echo, and everyone else never hears about it.
namespace openminerz::net
{

// GrenadeMessage.SecondsLeft is `5f - grenadeCookTime`. Player.OnUpdate adds
// whole frames to the cook time and only stops at the first frame where it is
// at least four seconds, so a held grenade leaves a stock hand with slightly
// LESS than one second (0.983 s at 60 fps, 0.967 s at 30 fps, lower across a
// frame hitch). Requiring a full second refused every fully cooked stock
// grenade, and with it the explosion that followed.
[[nodiscard]] constexpr bool stock_grenade_fuse_valid(float seconds_left) noexcept
{
    return seconds_left >= 0.0F && seconds_left <= 5.0F;
}

// PickupManager.HandleRequestPickupMessage: the game host answers the first
// request for a pickup it still holds and ignores requests for one that is
// already pending. It never measures the distance to the requester. The
// requesting client measured it already, against its own simulation of that
// pickup, and marks the entity `_pickedUp` for good when it asks: a request the
// host refuses is an item that player can never pick up again.
[[nodiscard]] constexpr bool stock_host_grants_pickup(bool pickup_exists,
                                                      bool already_pending) noexcept
{
    return pickup_exists && !already_pending;
}

// CreatePickupMessage carries where the pickup starts, and that is frequently
// nowhere near the sender: BaseZombie.CreatePickup drops loot at the ENEMY
// (plus one unit up) on behalf of whoever is credited with the kill, dragon
// loot appears where the dragon died, and Explosive.ProcessExplosionDependents
// drops torches and doors at the blast. Only the finite, in-world shape of the
// message is checked; the launch speed bound is a hostile ceiling well above
// PickupManager's 1.5, 3 and 4 units a second.
[[nodiscard]] constexpr bool stock_pickup_spawn_valid(Vector3 position, Vector3 velocity) noexcept
{
    const float speed_squared =
        velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
    return position.x >= -1'000'000.0F && position.x <= 1'000'000.0F && position.y >= -128.0F &&
           position.y <= 128.0F && position.z >= -1'000'000.0F && position.z <= 1'000'000.0F &&
           speed_squared <= 400.0F;
}

// What InGameHUD does when a dead player presses the respawn input, once the
// three-second prompt is up.
enum class StockRespawnAction : std::uint8_t
{
    // Nothing happens: either the 20-second online Endurance timer is still
    // running, or everybody is dead and only the session host may restart.
    wait,
    // The session host sends RestartLevelMessage (and the "Has Restarted The
    // Game" notice); every peer returns to the start and the day rewinds.
    restart_level,
    // TeleportToLocation(DefaultStartLocation, spawnOnTop: true).
    respawn_at_start,
    // TeleportToLocation(the living remote player nearest the world origin).
    respawn_at_teammate,
};

inline constexpr float stock_endurance_respawn_wait_seconds = 20.0F;

// InGameHUD.OnPlayerInput + RespawnPlayer + WaitToRespawn + AllPlayersDead.
// `any_remote_player_alive` counts announced remote players that are not dead;
// `seconds_since_prompt` is the time since the respawn prompt appeared, which
// is when the original starts counting its 20 seconds.
//
// Offline Endurance also restarts the level, but through the local respawn
// (see respawn_restarts_level), so it reports respawn_at_start here.
[[nodiscard]] constexpr StockRespawnAction stock_respawn_action(GameMode mode, bool online,
                                                                bool session_host,
                                                                bool any_remote_player_alive,
                                                                float seconds_since_prompt) noexcept
{
    if (mode != GameMode::endurance || !online)
        return StockRespawnAction::respawn_at_start;
    if (!any_remote_player_alive)
        return session_host ? StockRespawnAction::restart_level : StockRespawnAction::wait;
    if (seconds_since_prompt < stock_endurance_respawn_wait_seconds)
        return StockRespawnAction::wait;
    return StockRespawnAction::respawn_at_teammate;
}

// CastleMinerZGame.SendNetworkUpdates, identical in 1.6.3.8 and 1.9.9.8:
//
//     _currentFrameNumber++;
//     if (_currentFrameNumber > session.AllGamers.Count) _currentFrameNumber = 0;
//     for (int i = 0; i < session.AllGamers.Count; i++)
//         if (session.AllGamers[i].IsLocal && i != _currentFrameNumber &&
//             !_localPlayer.UsingTool && !_localPlayer.Reloading)
//             return;
//     PlayerUpdateMessage.Send(...);
//
// The counter takes AllGamers.Count + 1 values and the local gamer owns exactly
// one of them, so an idle player sends ONE update every (players + 1) frames:
// 30 a second alone, 20 with one other player, 6.7 in a session of eight, and
// every frame while UsingTool or Reloading. Nothing else hurries it; a movement
// or state edge, a death and a teleport all wait for the gamer's turn. The
// original runs one Update per vsynced frame (IsFixedTimeStep is false), so a
// 60 Hz simulation tick is the frame here.
//
// `local_index` is the local gamer's position in AllGamers. It only chooses the
// phase, which no peer can observe, so callers may leave it at zero.
class StockPlayerUpdateCadence final
{
  public:
    // One call per frame. `all_gamers` counts every gamer in the session,
    // the local one included. True when this frame sends a PlayerUpdate.
    [[nodiscard]] constexpr bool frame(std::size_t all_gamers, bool using_tool, bool reloading,
                                       std::size_t local_index = 0U) noexcept
    {
        ++frame_;
        if (frame_ > all_gamers)
            frame_ = 0U;
        return local_index == frame_ || using_tool || reloading;
    }

    constexpr void reset() noexcept
    {
        frame_ = 0U;
    }

  private:
    std::size_t frame_{};
};

} // namespace openminerz::net
