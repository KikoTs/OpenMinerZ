#pragma once

namespace openminerz
{

// Original GameModeTypes / GameDifficultyTypes enums; both serialized by
// index, and the difficulty MENU order (No Enemies, Easy, Normal, Hardcore)
// deliberately differs from the enum order.  "Normal" maps to hard.
enum class GameMode : int
{
    endurance = 0,
    survival = 1,
    dragon_endurance = 2,
    creative = 3,
    // Modern v4 extends the deployed GameModeTypes enum with these two
    // values. They must survive discovery/bootstrap intact because terrain
    // loot generation depends on the exact mode.
    exploration = 4,
    scavenger = 5
};

enum class GameDifficulty : int
{
    easy = 0,
    hard = 1,
    no_enemies = 2,
    hardcore = 3
};

// Exploration preserves resources but still requires finding the crafting materials.
[[nodiscard]] constexpr bool has_unrestricted_resources(GameMode mode, bool infinite) noexcept
{
    return infinite && mode != GameMode::exploration;
}

// Stock InGameHUD.ApplyDamage is `if (!LocalPlayer.Dead) { ... PlayerHealth -=
// damageAmount; ... }` in BOTH shipped generations (Classic 1.6.3.8 and Modern
// 1.9.9.8): InfiniteResourceMode never makes a player immune, online or
// offline. The `!InfiniteResourceMode || (SessionProperties[0].Value == 3 &&
// IsOnlineGame)` guard this function used to model, and the matching guards on
// ammunition, resources and enemy blocking, exist only in the OpenClassic
// mod's source. Following the mod made a Creative OpenMinerZ player in a
// Modern session unkillable next to stock players who were not.
//
// The parameters are kept so call sites state what they know; none of them
// changes the answer.
[[nodiscard]] constexpr bool creative_damage_immunity(GameMode mode, bool infinite, bool online,
                                                      bool classic_network_version) noexcept
{
    static_cast<void>(mode);
    static_cast<void>(infinite);
    static_cast<void>(online);
    static_cast<void>(classic_network_version);
    return false;
}

// InGameHUD.RespawnPlayer reaches RestartLevelMessage - the clock reset, the
// music restart and the distance record - only through
//     GameMode <= Endurance && no living remote player && IsHost && !IsOnlineGame
// and offline there are never any remote players. Inside a session the day is
// the host's to broadcast, so a respawn must leave it alone.
[[nodiscard]] constexpr bool respawn_restarts_level(GameMode mode, bool online) noexcept
{
    return mode == GameMode::endurance && !online;
}

// EnemyManager.GetMinEnemySpawnTime's branch selector: Endurance and Dragon
// Endurance always use the fast table, otherwise Normal/Hardcore do.
[[nodiscard]] constexpr bool hard_spawn_branch(GameMode mode, GameDifficulty difficulty) noexcept
{
    return mode == GameMode::endurance || mode == GameMode::dragon_endurance ||
           difficulty == GameDifficulty::hard || difficulty == GameDifficulty::hardcore;
}

// The "fresh ground" zombie-pack spawner runs for Endurance/Normal/Hardcore,
// but never in Dragon Endurance (its whole ambient system is off).
[[nodiscard]] constexpr bool fresh_ground_packs(GameMode mode, GameDifficulty difficulty) noexcept
{
    return mode != GameMode::dragon_endurance &&
           (mode == GameMode::endurance || difficulty == GameDifficulty::hard ||
            difficulty == GameDifficulty::hardcore);
}

// Dragons break blocks on the hard branch only (EnemyManager.DetonateFireball).
[[nodiscard]] constexpr bool dragons_damage_structures(GameMode mode,
                                                      GameDifficulty difficulty) noexcept
{
    return hard_spawn_branch(mode, difficulty);
}

[[nodiscard]] constexpr const char *game_mode_label(GameMode mode) noexcept
{
    switch (mode)
    {
    case GameMode::endurance:
        return "ENDURANCE";
    case GameMode::survival:
        return "SURVIVAL";
    case GameMode::dragon_endurance:
        return "DRAGON ENDURANCE";
    case GameMode::creative:
        return "CREATIVE";
    case GameMode::exploration:
        return "EXPLORATION";
    case GameMode::scavenger:
        return "SCAVENGER";
    }
    return "SURVIVAL";
}

[[nodiscard]] constexpr const char *game_difficulty_label(GameDifficulty difficulty) noexcept
{
    switch (difficulty)
    {
    case GameDifficulty::no_enemies:
        return "NO ENEMIES";
    case GameDifficulty::easy:
        return "EASY";
    case GameDifficulty::hard:
        return "NORMAL";
    case GameDifficulty::hardcore:
        return "HARDCORE";
    }
    return "NORMAL";
}

} // namespace openminerz
