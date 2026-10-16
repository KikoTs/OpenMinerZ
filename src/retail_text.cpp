#include "openminerz/retail_text.hpp"

#include <array>
#include <format>

namespace openminerz
{
namespace
{

struct Entry final
{
    const char *key;
    const char *classic;
    const char *modern;
};

constexpr std::array<Entry, retail_string_count> entries{{
#define OPENMINERZ_RETAIL_STRING(identifier, key, classic, modern) {key, classic, modern},
#include "openminerz/retail_strings.inc"
#undef OPENMINERZ_RETAIL_STRING
}};

using S = RetailString;

} // namespace

std::string_view retail_text(RetailString id, RetailGeneration generation) noexcept
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= entries.size())
        return {};
    const Entry &entry = entries[index];
    const char *wanted = generation == RetailGeneration::classic ? entry.classic : entry.modern;
    const char *other = generation == RetailGeneration::classic ? entry.modern : entry.classic;
    if (wanted != nullptr)
        return wanted;
    return other != nullptr ? std::string_view{other} : std::string_view{};
}

bool retail_text_exists(RetailString id, RetailGeneration generation) noexcept
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= entries.size())
        return false;
    return (generation == RetailGeneration::classic ? entries[index].classic
                                                    : entries[index].modern) != nullptr;
}

std::string_view retail_string_key(RetailString id) noexcept
{
    const auto index = static_cast<std::size_t>(id);
    return index < entries.size() ? std::string_view{entries[index].key} : std::string_view{};
}

std::optional<RetailString> find_retail_string(std::string_view key) noexcept
{
    // The game's own table precedes DNA.Common's, so a key both carry ("Cancel",
    // "Off") resolves to the game's entry, as it does in the original.
    for (std::size_t index = 0U; index < entries.size(); ++index)
        if (key == entries[index].key)
            return static_cast<RetailString>(index);
    return std::nullopt;
}

std::string_view retail_game_mode_name(GameMode mode, RetailGeneration generation) noexcept
{
    switch (mode)
    {
    case GameMode::endurance:
        return retail_text(S::Endurance, generation);
    case GameMode::survival:
        return retail_text(S::Survival, generation);
    case GameMode::dragon_endurance:
        return retail_text(S::Dragon_Endurance, generation);
    case GameMode::creative:
        return retail_text(S::Creative, generation);
    case GameMode::exploration:
        return retail_text(S::Exploration, generation);
    case GameMode::scavenger:
        return retail_text(S::Scavenger, generation);
    }
    return {};
}

std::string_view retail_game_mode_description(GameMode mode, RetailGeneration generation) noexcept
{
    switch (mode)
    {
    case GameMode::endurance:
        return retail_text(
            S::Earn_awards_by_seeing_how_far_you_can_travel_from_the_start_point_Changes_to_the_world_will_not_be_saved_in_this_mode,
            generation);
    case GameMode::survival:
        return retail_text(
            S::Mine_resources_and_build_your_fortress_while_defending_yourself_from_the_undead_horde_Your_creations_will_be_saved_in_this_mode_You_can_play_with_or_without_enemies,
            generation);
    case GameMode::dragon_endurance:
        return retail_text(
            S::Fend_off_wave_after_wave_of_dragons_Unlock_this_mode_by_defeating_the_undead_dragon_in_Endurance_Mode_Your_creations_will_be_saved_in_this_mode,
            generation);
    case GameMode::creative:
        return retail_text(
            S::Build_structures_with_unlimited_resources_You_can_play_with_or_without_enemies,
            generation);
    case GameMode::exploration:
        return retail_text(S::Exploration_description, generation);
    case GameMode::scavenger:
        return retail_text(S::Scavenger_description, generation);
    }
    return {};
}

std::string_view retail_difficulty_name(GameDifficulty difficulty,
                                        RetailGeneration generation) noexcept
{
    switch (difficulty)
    {
    case GameDifficulty::no_enemies:
        return retail_text(S::No_Enemies, generation);
    case GameDifficulty::easy:
        return retail_text(S::Easy, generation);
    case GameDifficulty::hard:
        return retail_text(S::Normal, generation);
    case GameDifficulty::hardcore:
        return retail_text(S::Hardcore, generation);
    }
    return {};
}

std::string_view retail_difficulty_description(GameDifficulty difficulty,
                                               RetailGeneration generation) noexcept
{
    switch (difficulty)
    {
    case GameDifficulty::no_enemies:
        return retail_text(S::Build_freely_without_enemy_attacks, generation);
    case GameDifficulty::easy:
        return retail_text(
            S::Enemies_do_less_damage_Zombies_only_appear_at_night_Dragons_will_not_damage_your_structures,
            generation);
    case GameDifficulty::hard:
        return retail_text(
            S::Enemies_appear_when_you_cover_fresh_ground_and_swarm_at_night_Dragons_will_damage_your_structures,
            generation);
    case GameDifficulty::hardcore:
        return retail_text(S::Start_with_nothing_and_drop_all_items_when_you_die, generation);
    }
    return {};
}

namespace retail_message
{

std::string player_line(std::string_view gamertag, RetailString phrase,
                        RetailGeneration generation)
{
    return std::format("{} {}", gamertag, retail_text(phrase, generation));
}

std::string player_joined(std::string_view gamertag, RetailGeneration generation)
{
    return std::format("{}: {}", retail_text(S::Player_Joined, generation), gamertag);
}

std::string player_left(std::string_view gamertag, RetailGeneration generation)
{
    return std::format("{}: {}", retail_text(S::Player_Left, generation), gamertag);
}

std::string dragon_killed(std::string_view gamertag, std::string_view dragon_name,
                          RetailGeneration generation)
{
    return std::format("{} {} {}", gamertag, retail_text(S::Has_Killed_The, generation),
                       dragon_name);
}

std::string achievement_earned(std::string_view gamertag, std::string_view achievement_name,
                               RetailGeneration generation)
{
    // 1.6.3.8 concatenates the gamertag and "Has earned" with no space between
    // them; 1.9.9.8 added it. The line is sent to every peer as typed, so a
    // Classic session really reads "SteveHas earned 'Tinkerer'".
    return std::format("{}{}{} '{}'", gamertag,
                       generation == RetailGeneration::classic ? "" : " ",
                       retail_text(S::Has_earned, generation), achievement_name);
}

std::string teleported_to(std::string_view gamertag, std::string_view locator_name,
                          RetailGeneration generation)
{
    return std::format("{} {} {}", gamertag, retail_text(S::Teleported_To, generation),
                       locator_name);
}

std::string respawn_countdown(int seconds, RetailGeneration generation)
{
    return std::format("{}: {}", retail_text(S::Respawn_In, generation), seconds);
}

std::string distance_traveled(int distance, RetailGeneration generation)
{
    return std::format("{}: {}", retail_text(S::Distance_Traveled, generation), distance);
}

std::string in_days(int days, RetailGeneration generation)
{
    return std::format(" {} {} {}", retail_text(S::In, generation), days,
                       retail_text(days != 1 ? S::Days : S::Day, generation));
}

std::string loading_world(RetailGeneration generation)
{
    return std::format("{}{}", retail_text(S::Loading_The_World, generation),
                       retail_text(S::Please_Wait, generation));
}

std::string connecting(RetailGeneration generation)
{
    return std::format("{}{}", retail_text(S::Connecting, generation),
                       retail_text(S::Please_Wait, generation));
}

std::string chat_line(std::string_view gamertag, std::string_view text)
{
    return std::format("{}: {}", gamertag, text);
}

} // namespace retail_message

} // namespace openminerz
