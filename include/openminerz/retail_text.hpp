#pragma once

#include "openminerz/game_mode.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace openminerz
{

// The words CastleMiner Z shows, per shipped build. This is the ONE table every
// screen reads retail text from: the complete Globalization.Strings resource of
// both builds (plus DNA.Common's shared dialog words under `Common_`), keyed by
// the resource's own key, so `Strings.Click_To_Respawn` in the original is
// `RetailString::Click_To_Respawn` here.
//
// The table is generated from the owned installs by
// tools/OriginalClientInterop/PresentationParityProbe and held to them by
// tests/test_presentation_parity.cpp; never edit retail_strings.inc by hand.
//
// Item names and descriptions are in inventory.hpp (item_text), block names in
// world/block.hpp (block_name) and achievement text in player_progress.hpp;
// they come from the same probe run.
enum class RetailGeneration : std::uint8_t
{
    classic = 0, // 1.6.3.8
    modern = 1,  // 1.9.9.8
};

enum class RetailString : std::uint16_t
{
#define OPENMINERZ_RETAIL_STRING(identifier, key, classic, modern) identifier,
#include "openminerz/retail_strings.inc"
#undef OPENMINERZ_RETAIL_STRING
    count
};

inline constexpr std::size_t retail_string_count = static_cast<std::size_t>(RetailString::count);

// The text that build shows. 1.9.9.8 added about 150 entries (sprint, trays,
// spawners, the options tabs ...) and reworded seven; an entry the requested
// build lacks answers with the other build's wording, because a thing that
// exists in only one generation is named by that generation.
[[nodiscard]] std::string_view retail_text(RetailString id, RetailGeneration generation) noexcept;
// True when that build's resource has the entry.
[[nodiscard]] bool retail_text_exists(RetailString id, RetailGeneration generation) noexcept;
// The resource key exactly as shipped ("Loading_The_World____").
[[nodiscard]] std::string_view retail_string_key(RetailString id) noexcept;
[[nodiscard]] std::optional<RetailString> find_retail_string(std::string_view key) noexcept;

// GameModeMenu / the host-options header. Exploration and Scavenger exist only
// in 1.9.9.8 and are named by it in either presentation.
[[nodiscard]] std::string_view retail_game_mode_name(GameMode mode, RetailGeneration generation) noexcept;
// The paragraph GameModeMenu shows beside the mode.
[[nodiscard]] std::string_view retail_game_mode_description(GameMode mode,
                                                            RetailGeneration generation) noexcept;
// DifficultyLevelScreen: "No Enemies", "Easy", "Normal" (GameDifficulty::hard)
// and "Hardcore".
[[nodiscard]] std::string_view retail_difficulty_name(GameDifficulty difficulty,
                                                      RetailGeneration generation) noexcept;
[[nodiscard]] std::string_view retail_difficulty_description(GameDifficulty difficulty,
                                                             RetailGeneration generation) noexcept;

// The lines the original composes at run time. Each is the shipped expression,
// so spacing and capitalisation are the original's, including its oddities.
namespace retail_message
{
// BroadcastTextMessage.Send(gamer, gamer.Gamertag + " " + Strings.Has_Fallen)
// and the other one-name lines built the same way.
[[nodiscard]] std::string player_line(std::string_view gamertag, RetailString phrase,
                                      RetailGeneration generation);
// Strings.Player_Joined + ": " + gamertag  /  Strings.Player_Left + ": " + gamertag
[[nodiscard]] std::string player_joined(std::string_view gamertag, RetailGeneration generation);
[[nodiscard]] std::string player_left(std::string_view gamertag, RetailGeneration generation);
// gamertag + " " + Strings.Has_Killed_The + " " + dragon name
[[nodiscard]] std::string dragon_killed(std::string_view gamertag, std::string_view dragon_name,
                                        RetailGeneration generation);
// InGameHUD.UpdateAcheivements: gamertag + Strings.Has_earned + " '" + name + "'".
// 1.6.3.8 has NO space between the gamertag and "Has earned" (1.9.9.8 added
// it), and the line travels to every peer as typed.
[[nodiscard]] std::string achievement_earned(std::string_view gamertag,
                                             std::string_view achievement_name,
                                             RetailGeneration generation);
// GPSItem: gamertag + " " + Strings.Teleported_To + " " + locator name
[[nodiscard]] std::string teleported_to(std::string_view gamertag, std::string_view locator_name,
                                        RetailGeneration generation);
// InGameHUD's death prompt. While the 20 s respawn timer runs in an online
// Endurance game it reads Strings.Respawn_In + ": " + seconds; otherwise it is
// Strings.Click_To_Respawn, Click_To_Restart or Waiting_For_Host_To_Restart.
[[nodiscard]] std::string respawn_countdown(int seconds, RetailGeneration generation);
// The two Endurance lines above that prompt:
//   Strings.Distance_Traveled + ": " + distance
//   " " + Strings.In + " " + days + " " + (days != 1 ? Strings.Days : Strings.Day)
[[nodiscard]] std::string distance_traveled(int distance, RetailGeneration generation);
[[nodiscard]] std::string in_days(int days, RetailGeneration generation);
// FrontEndScreen's load screens: the two resource entries are concatenated as
// they are, so the first one's trailing space is the only separator
// ("Loading The World... Please Wait...").
[[nodiscard]] std::string loading_world(RetailGeneration generation);
[[nodiscard]] std::string connecting(RetailGeneration generation);
// Chat as the sender composes it: gamertag + ": " + text.
[[nodiscard]] std::string chat_line(std::string_view gamertag, std::string_view text);
} // namespace retail_message

} // namespace openminerz
