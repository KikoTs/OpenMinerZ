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
