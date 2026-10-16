#include "openminerz/menu_model.hpp"
#include "openminerz/player_progress.hpp"

#include <algorithm>

namespace openminerz::ui
{
namespace
{

const std::vector<MenuEntry> achievement_entries = [] {
    std::vector<MenuEntry> entries;
    for (const auto &achievement : achievement_definitions())
        entries.push_back({achievement.name, achievement.description, MenuCommand::none, true, {}});
    return entries;
}();

// Labels are worded and cased as CastleMiner Z's own menus are wherever it has
// the same entry (Play Offline, Host Online, Join Online, Options, Awards,
// Exit, Back: MainMenu and OptionsMenu of 1.6.3.8 and 1.9.9.8); the rest are
// OpenMinerZ's and follow the same Title Case. The description is shown beside
// the focused row, where GameModeMenu shows a mode's paragraph.
const std::vector<MenuEntry> main_entries{
    {"Play Offline", "Start a new world or carry on with one you have saved.",
     MenuCommand::play_offline, true, "LOCAL"},
    {"Multiplayer", "Host a game for your friends or join one over Steam.",
     MenuCommand::open_multiplayer, true, "STEAM"},
    {"Add-Ons", "See the add-ons installed in this copy of OpenMinerZ.", MenuCommand::open_addons,
     true, "CONTENT"},
    {"Options", "Change the display, graphics, controls, interface, sound and extras.",
     MenuCommand::open_settings, true, "OPTIONS"},
    {"Awards", "See your awards and how far along each one is.", MenuCommand::open_achievements,
     true, {}},
    {"Credits", "The people behind the game.", MenuCommand::open_credits, true, "ABOUT"},
    {"Exit", "Close OpenMinerZ.", MenuCommand::request_exit, true, "DESKTOP"},
};

const std::vector<MenuEntry> multiplayer_entries{
    {"Host Online", "Choose a world, set who may join, and open it to other players.",
     MenuCommand::host_online, true, "CREATE"},
    {"Join Online", "Find a Classic or Modern server and join it.", MenuCommand::join_online, true,
     "BROWSE"},
    {"Back", "Back to main menu.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> exit_entries{
    {"Cancel", "Return to the main menu.", MenuCommand::cancel_exit, true, {}},
    {"Exit", "Close OpenMinerZ now.", MenuCommand::confirm_exit, true, {}},
};

const std::vector<MenuEntry> host_setup_entries{
    {"Server Details", "The server's name, the game version it speaks and how many may play.",
     MenuCommand::open_host_details, true, ""},
    {"Access & Privacy", "Who can see the server, who can join it, and its password.",
     MenuCommand::open_host_access, true, ""},
    {"Game Rules", "Player damage, anti-cheat, safe joining and spawn protection.",
     MenuCommand::open_host_gameplay, true, ""},
    {"Add-Ons", "Check the add-ons a player needs before they can join.",
     MenuCommand::review_host_mods, true, "REVIEW"},
    {"Continue", "Choose the game mode, the difficulty and the world to host.",
     MenuCommand::confirm_host_settings, true, "NEXT"},
    {"Back", "Back to multiplayer.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> host_details_entries{
    {"Server Name",
     "The lobby name shown to browsers and invited players.",
     MenuCommand::edit_host_name,
     true,
     {}},
    {"Game Version", "Choose Classic or Modern.", MenuCommand::select_host_protocol, true, {}},
    {"Max Players",
     "Maximum players, including the host.",
     MenuCommand::select_host_max_players,
     true,
     {}},
    {"Back", "Back to the host overview.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> host_access_entries{
    {"Visibility",
     "Who can find the server: friends, invited players, everyone, or the local network.",
     MenuCommand::select_host_visibility,
     true,
     {}},
    {"Password",
     "Set or clear the server's password. It is never shown in the menu.",
     MenuCommand::edit_host_password,
     true,
     {}},
    {"Who Can Join",
     "Admit anyone, only an allow list, or only players you invited.",
     MenuCommand::select_host_join_policy,
     true,
     {}},
    // (Authority)
    {"Players",
     "Any player, or OpenMinerZ players only. With OpenMinerZ only, the original game cannot "
     "join, and the host decides health, damage, items and building for everyone, so cheats "
     "stop working. Server mods always work this way.",
     MenuCommand::toggle_host_openminerz_only,
     true,
     {}},
    {"Back", "Back to the host overview.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> host_gameplay_entries{
    {"PVP",
     "Whether players can hurt each other: nobody, everyone, or everyone except friends.",
     MenuCommand::select_host_pvp,
     true,
     {}},
    {"Anti-Cheat", "Refuse player actions the game's rules do not allow.",
     MenuCommand::toggle_host_anticheat, true, {}},
    {"Safe Join",
     "Put a joining player on safe ground.",
     MenuCommand::toggle_host_join_safety,
     true,
     {}},
    {"Spawn Protection",
     "Stop building and mining within this distance of the spawn point.",
     MenuCommand::select_host_spawn_protection,
     true,
     {}},
    {"Back", "Back to the host overview.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> join_details_entries{
    {"Join Game", "Join this game.", MenuCommand::connect_join_session, true, "CONNECT"},
    {"Password",
     "Enter or clear the password for this server. It is never shown.",
     MenuCommand::edit_join_password,
     true,
     {}},
    {"Back", "Back to the server list.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> game_profile_entries{
    {"Classic",
     "CastleMiner Z as it was in 1.6.3.8: the original survival rules, items and crafting. "
     "Compatible with Classic servers.",
     MenuCommand::select_profile_classic, true, {}},
    {"Modern",
     "CastleMiner Z as it is in 1.9.9.8: more items and crafting, stamina, two item trays, "
     "teleport stations and two more game modes. Compatible with Modern servers.",
     MenuCommand::select_profile_modern, true, {}},
    {"Back", "Back to main menu.", MenuCommand::back, true, {}},
};

// GameModeMenu's rows. The paragraph shown beside each is the shipped one
// (retail_game_mode_description); these are the fallbacks for a build that has
// none for the mode.
const std::vector<MenuEntry> game_mode_entries{
    {"Endurance",
     "Travel as far as you can. Death ends the run.",
     MenuCommand::select_mode_endurance,
     true,
     {}},
    {"Survival", "Build, mine and survive the nights.", MenuCommand::select_mode_survival, true,
     "CLASSIC"},
    {"Dragon Endurance",
     "Fend off wave after wave of dragons, each tougher than the last.",
     MenuCommand::select_mode_dragon_endurance,
     true,
     {}},
    {"Creative",
     "Build with unlimited resources.",
     MenuCommand::select_mode_creative,
     true,
     {}},
    {"Back", "Back to main menu.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> classic_locked_mode_entries = [] {
    auto entries = game_mode_entries;
    entries[2].enabled = false;
    entries[2].description = "Defeat the undead dragon in Endurance to unlock.";
    entries[2].badge = "LOCKED";
    return entries;
}();

const std::vector<MenuEntry> modern_mode_entries = [] {
    auto entries = game_mode_entries;
    entries[1].badge = {};
    entries.insert(entries.end() - 1,
                   {"Exploration", "Unlimited use of the materials you find.",
                    MenuCommand::select_mode_exploration, true, {}});
    entries.insert(entries.end() - 1,
                   {"Scavenger", "Survival with more loot and cave encounters.",
                    MenuCommand::select_mode_scavenger, true, {}});
    return entries;
}();

// Original DifficultyLevelScreen order (default cursor on Normal).
const std::vector<MenuEntry> difficulty_entries{
    {"No Enemies",
     "Build freely without enemy attacks.",
     MenuCommand::select_difficulty_no_enemies,
     true,
     {}},
    {"Easy",
     "Enemies do less damage and zombies only appear at night. Dragons will not "
     "damage your structures.",
     MenuCommand::select_difficulty_easy,
     true,
     {}},
    {"Normal",
     "Enemies appear when you cover fresh ground and swarm at night. Dragons will damage "
     "your structures.",
     MenuCommand::select_difficulty_normal,
     true,
     {}},
    {"Hardcore",
     "Start with nothing and drop all items when you die.",
     MenuCommand::select_difficulty_hardcore,
     true,
     {}},
    {"Back", "Back to main menu.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> empty_entries{};

} // namespace

// InGameMenu's constructor, in its AddMenuItem order. Labels are the shipped
// English strings. Left out on purpose: Invite Friends (the original adds it and
// sets Visible = false), and 1.6.3.8's Broadcasting (the Twitch settings screen
// of a service this client does not carry). Achievements is this client's own
// row; the original reaches its Awards screen from the main menu only.
std::vector<MenuEntry> in_game_menu_entries(const InGameMenuFacts &facts)
{
    std::vector<MenuEntry> entries;
    entries.push_back({"Return To Game", "Return to the current world.", MenuCommand::resume_game,
                       true, {}});
    entries.push_back({"Inventory", "Open the backpack, crafting, and equipment view.",
                       MenuCommand::open_inventory, true, {}});
    if (!facts.endurance && !facts.hardcore)
        entries.push_back({"Teleport", "Go to the surface, the start, or another player.",
                           MenuCommand::open_teleport, true, {}});
    if (facts.modern)
        entries.push_back({"Options", "Customize your game.", MenuCommand::open_settings, true, {}});
    else
    {
        entries.push_back({"Controls", "Mouse, keyboard and controller.",
                           MenuCommand::open_controls, true, {}});
        entries.push_back({"Settings", "Customize your game.", MenuCommand::open_settings, true,
                           {}});
    }
    entries.push_back({"Achievements", "Your milestones and progress.",
                       MenuCommand::open_achievements, true, {}});
    if (facts.online && facts.host)
        entries.push_back({"Host Options", "Kick, ban, restart and PVP.",
                           MenuCommand::open_host_options, true, {}});
    entries.push_back({"Main Menu", "Save and leave the world.", MenuCommand::return_to_main, true,
                       {}});
    return entries;
}

// TeleportMenu: Quit, Surface, Origin, Player.
std::vector<MenuEntry> teleport_menu_entries(const InGameMenuFacts &facts)
{
    std::vector<MenuEntry> entries{
        {"Return To Game", "Return to the current world.", MenuCommand::resume_game, true, {}},
        {"Teleport To Surface", "Stand on the highest ground above you.",
         MenuCommand::teleport_surface, true, {}},
        {"Teleport To Start", "Return to where the world began.", MenuCommand::teleport_start, true,
         {}},
    };
    if (facts.online && (!facts.modern || facts.pvp == 0))
        entries.push_back({"Teleport To Player", "Join another player where they stand.",
                           MenuCommand::teleport_to_player, true, {}});
    return entries;
}

std::string_view pvp_state_text(int pvp) noexcept
{
    switch (pvp)
    {
    case 1:
        return "PVP: Everyone";
    case 2:
        return "PVP: Non-Friends Only";
    default:
        return "PVP: Off";
    }
}

// HostOptions.OnUpdate: Strings.Who_can_join ("Who can join:") + " " + the
// policy's own string (Anyone / Friends Only / Invited Friends).
std::string_view join_policy_text(int join_policy) noexcept
{
    switch (join_policy)
    {
    case 1:
        return "Who can join: Friends Only";
    case 2:
        return "Who can join: Invited Friends";
    default:
        return "Who can join: Anyone";
    }
}

// HostOptions, in the order its constructor adds them (both generations):
// Return To Game, Kick Player, Ban Player, Restart, PVP, Set Password, Server
// Message, Clear Ban List, Who can join. Its first row pops one screen, so
// despite the label it returns to the in-game menu. Clear Ban List is only
// Visible while PlayerStats.BanList is not empty.
std::vector<MenuEntry> host_options_entries(const InGameMenuFacts &facts)
{
    std::vector<MenuEntry> entries{
        {"Return To Game", "Back to the game menu.", MenuCommand::back, true, {}},
        {"Kick Player", "Remove a player from this session.", MenuCommand::host_kick_player, true,
         {}},
        {"Ban Player", "Remove a player and refuse them from now on.",
         MenuCommand::host_ban_player, true, {}},
        {"Restart", "Return everyone to the start and restart the day.", MenuCommand::host_restart,
         true, {}},
        {pvp_state_text(facts.pvp), "Who players can hurt.", MenuCommand::host_cycle_pvp, true, {}},
        {"Set Password", "Require a password from players who join from now on.",
         MenuCommand::host_set_password, true, {}},
        {"Server Message", "The name this game has in the server list.",
         MenuCommand::host_server_message, true, {}},
    };
    if (!facts.ban_list_empty)
        entries.push_back({"Clear Ban List", "Let every banned player join again.",
                           MenuCommand::host_clear_ban_list, true, {}});
    entries.push_back({join_policy_text(facts.join_policy),
                       "Who may find and join this game.", MenuCommand::host_cycle_join_policy,
                       true, {}});
    return entries;
}

// The Add-Ons screen: the built-in "Xbox Avatars" add-on's three rows. (Add-Ons)
// The installed mods follow, one row each (set_addon_entries).
const std::vector<MenuEntry> addon_entries{
    {"Show My Avatar",
     "Wear your own Xbox avatar instead of the stock character. Players running the Xbox Avatar "
     "add-on or OpenMinerZ see it too; everyone else sees the stock character.",
     MenuCommand::toggle_avatar_mine, true, {}},
    {"Show Other Players' Avatars",
     "Draw the Xbox avatars other players share. Off shows every other player as the stock "
     "character.",
     MenuCommand::toggle_avatar_others, true, {}},
    {"Reload My Avatar",
     "Read your avatar file again now. A re-exported or replaced file is also picked up by "
     "itself within a second or two.",
     MenuCommand::reload_avatar, true, {}},
};

const std::vector<MenuEntry> &MenuModel::entries() const noexcept
{
    // (UI area) The list with the rows mods added, when there are any.
    if (const std::optional<std::size_t> slot = mod_slot(screen_); slot && !mod_entries_[*slot].empty())
        return merged_entries_[*slot];
    switch (screen_)
    {
    case MenuScreen::main:
        return main_entries;
    case MenuScreen::multiplayer:
        return multiplayer_entries;
    case MenuScreen::exit_confirmation:
        return exit_entries;
    case MenuScreen::pause:
        return in_game_entries_;
    case MenuScreen::teleport:
        return teleport_entries_;
    case MenuScreen::host_options:
        return host_option_entries_;
    case MenuScreen::select_player:
        return player_entries_;
    case MenuScreen::game_mode:
        return modern_gameplay_ ? modern_mode_entries
                               : dragon_unlocked_ ? game_mode_entries : classic_locked_mode_entries;
    case MenuScreen::game_profile:
        return game_profile_entries;
    case MenuScreen::difficulty:
        return difficulty_entries;
    case MenuScreen::host_setup:
        return host_setup_entries;
    case MenuScreen::host_details:
        return host_details_entries;
    case MenuScreen::host_access:
        return host_access_entries;
    case MenuScreen::host_gameplay:
        return host_gameplay_entries;
    case MenuScreen::join_browser:
        return join_entries_;
    case MenuScreen::join_details:
        return join_details_entries;
    case MenuScreen::world_select:
        return world_entries_;
    case MenuScreen::travel:
        return travel_entries_;
    case MenuScreen::achievements:
        return achievement_entries;
    case MenuScreen::addons:
        return addon_screen_entries_.empty() ? addon_entries : addon_screen_entries_;
    case MenuScreen::settings:
    case MenuScreen::credits:
    case MenuScreen::world_preview:
        return empty_entries;
    }
    return empty_entries;
}

std::size_t MenuModel::selectable_count(const Settings &) const noexcept
{
    if (screen_ == MenuScreen::settings)
    {
        return rows_for_tab(settings_tab_).size();
    }
    return entries().size();
}

void MenuModel::select_next(int direction, const Settings &settings)
{
    const std::size_t count = selectable_count(settings);
    if (count == 0U)
    {
        selection_ = 0U;
        return;
    }

    const int step = direction < 0 ? -1 : 1;
    for (std::size_t attempts = 0U; attempts < count; ++attempts)
    {
        const int next = (static_cast<int>(selection_) + step + static_cast<int>(count)) %
                         static_cast<int>(count);
        selection_ = static_cast<std::size_t>(next);
        if (screen_ == MenuScreen::settings || entries()[selection_].enabled)
        {
            return;
        }
    }
}

void MenuModel::set_selection(std::size_t index, const Settings &settings)
{
    const std::size_t count = selectable_count(settings);
    if (count == 0U)
    {
        selection_ = 0U;
        return;
    }
    selection_ = std::min(index, count - 1U);
    if (screen_ != MenuScreen::settings && !entries()[selection_].enabled)
    {
        select_next(1, settings);
    }
}

void MenuModel::change_tab(int direction, const Settings &settings)
{
    if (screen_ != MenuScreen::settings)
    {
        return;
    }
    settings_tab_ = next_tab(settings_tab_, direction);
    selection_ = 0U;
    clamp_selection(settings);
    ++transition_serial_;
}

MenuEvent MenuModel::adjust_selected(Settings &settings, int direction)
{
    if (screen_ != MenuScreen::settings)
    {
        return {};
    }
    const auto &rows = rows_for_tab(settings_tab_);
    if (selection_ >= rows.size())
    {
        return {};
    }
    adjust_setting(settings, rows[selection_].id, direction);
    return {MenuCommand::open_settings, false};
}

MenuEvent MenuModel::activate(Settings &settings)
{
    if (screen_ == MenuScreen::settings)
    {
        return adjust_selected(settings, 1);
    }
    const auto &current_entries = entries();
    if (selection_ >= current_entries.size() || !current_entries[selection_].enabled)
    {
        return {};
    }

    const MenuCommand command = current_entries[selection_].command;
    switch (command)
    {
    case MenuCommand::play_offline:
        game_flow_origin_ = MenuScreen::main;
        open(MenuScreen::game_profile);
        return {command, true};
    case MenuCommand::select_profile_classic:
    case MenuCommand::select_profile_modern:
        modern_gameplay_ = command == MenuCommand::select_profile_modern;
        open(MenuScreen::game_mode);
        return {command, true};
    case MenuCommand::open_multiplayer:
        open(MenuScreen::multiplayer);
        return {command, true};
    case MenuCommand::host_online:
        open(MenuScreen::host_setup);
        return {command, true};
    case MenuCommand::join_online:
        open(MenuScreen::join_browser);
        return {command, true};
    case MenuCommand::open_host_details:
        open(MenuScreen::host_details);
        return {command, true};
    case MenuCommand::open_host_access:
        open(MenuScreen::host_access);
        return {command, true};
    case MenuCommand::open_host_gameplay:
        open(MenuScreen::host_gameplay);
        return {command, true};
    case MenuCommand::confirm_host_settings:
        game_flow_origin_ = MenuScreen::host_setup;
        open(MenuScreen::game_mode);
        return {command, true};
    case MenuCommand::edit_host_name:
    case MenuCommand::select_host_protocol:
    case MenuCommand::select_host_max_players:
    case MenuCommand::select_host_visibility:
    case MenuCommand::edit_host_password:
    case MenuCommand::select_host_join_policy:
    case MenuCommand::toggle_host_openminerz_only: // (Authority)
    case MenuCommand::select_host_pvp:
    case MenuCommand::toggle_host_anticheat:
    case MenuCommand::toggle_host_join_safety:
    case MenuCommand::select_host_spawn_protection:
    case MenuCommand::review_host_mods:
        return {command, false};
    case MenuCommand::refresh_join_sessions:
    case MenuCommand::cycle_join_filter:
    case MenuCommand::direct_connect:
    case MenuCommand::select_join_session:
    case MenuCommand::connect_join_session:
    case MenuCommand::edit_join_password:
        return {command, false};
    // Original flow: Endurance starts a throwaway world immediately; Dragon
    // Endurance goes straight to the world list; Survival and Creative ask
    // for a difficulty (cursor on NORMAL) then the world list.
    case MenuCommand::select_mode_endurance:
        open(MenuScreen::world_preview);
        return {command, true};
    case MenuCommand::select_mode_dragon_endurance:
        open(MenuScreen::world_select);
        return {command, true};
    case MenuCommand::select_mode_survival:
    case MenuCommand::select_mode_creative:
    case MenuCommand::select_mode_exploration:
    case MenuCommand::select_mode_scavenger:
        open(MenuScreen::difficulty);
        selection_ = 2U;
        return {command, true};
    case MenuCommand::select_difficulty_no_enemies:
    case MenuCommand::select_difficulty_easy:
    case MenuCommand::select_difficulty_normal:
    case MenuCommand::select_difficulty_hardcore:
        open(MenuScreen::world_select);
        return {command, true};
    // World rows stay on this screen: the app reads selection(), acts, and
    // enters the world itself.
    case MenuCommand::create_new_world:
    case MenuCommand::select_world:
        return {command, false};
    case MenuCommand::open_inventory:
    case MenuCommand::teleport_surface:
    case MenuCommand::teleport_start:
        open(MenuScreen::world_preview);
        return {command, true};
    case MenuCommand::open_teleport:
        open(MenuScreen::teleport);
        return {command, true};
    case MenuCommand::open_host_options:
        open(MenuScreen::host_options);
        return {command, true};
    // SelectPlayerScreen. Cancelling a teleport pick pops TeleportMenu and
    // InGameMenu with it; cancelling a kick or ban pick returns to HostOptions.
    case MenuCommand::teleport_to_player:
    case MenuCommand::host_kick_player:
    case MenuCommand::host_ban_player:
        player_return_screen_ = command == MenuCommand::teleport_to_player
                                    ? MenuScreen::world_preview
                                    : MenuScreen::host_options;
        open(MenuScreen::select_player);
        return {command, true};
    // The app resolves the row to a player and then leaves the picker itself.
    case MenuCommand::choose_player:
    case MenuCommand::host_restart:
    case MenuCommand::host_cycle_pvp:
    case MenuCommand::host_set_password:
    case MenuCommand::host_server_message:
    case MenuCommand::host_clear_ban_list:
    case MenuCommand::host_cycle_join_policy:
        return {command, false};
    // 1.6.3.8's Controls row opens the bindings screen; here that is the
    // Controls page of the settings screen.
    case MenuCommand::open_controls:
        return_screen_ = screen_;
        open(MenuScreen::settings);
        settings_tab_ = SettingsTab::controls;
        clamp_selection(settings);
        return {MenuCommand::open_settings, true};
    case MenuCommand::open_addons:
        open(MenuScreen::addons);
        return {command, true};
    case MenuCommand::open_settings:
        return_screen_ = screen_;
        open(MenuScreen::settings);
        clamp_selection(settings);
        return {command, true};
    case MenuCommand::open_achievements:
        return_screen_ = screen_;
        open(MenuScreen::achievements);
        return {command, true};
    case MenuCommand::open_credits:
        open(MenuScreen::credits);
        return {command, true};
    case MenuCommand::request_exit:
        open(MenuScreen::exit_confirmation);
        return {command, true};
    case MenuCommand::resume_game:
        open(MenuScreen::world_preview);
        return {command, true};
    case MenuCommand::return_to_main:
        open(MenuScreen::main);
        return {command, true};
    case MenuCommand::cancel_exit:
        open(MenuScreen::main);
        return {command, true};
    // A clickable BACK row behaves exactly like pressing Escape.
    case MenuCommand::back:
        return back();
    default:
        return {command, false};
    }
}

MenuEvent MenuModel::back()
{
    if (screen_ == MenuScreen::main)
    {
        open(MenuScreen::exit_confirmation);
        return {MenuCommand::request_exit, true};
    }
    if (screen_ == MenuScreen::pause)
    {
        open(MenuScreen::world_preview);
        return {MenuCommand::resume_game, true};
    }
    if (screen_ == MenuScreen::select_player)
    {
        const MenuScreen target = player_return_screen_;
        open(target);
        return {target == MenuScreen::world_preview ? MenuCommand::resume_game : MenuCommand::back,
                true};
    }
    // Any screen reached from the pause menu goes back to it, whatever it is.
    if (pause_overlay_)
    {
        return_screen_ = MenuScreen::main;
        open(MenuScreen::pause);
        return {MenuCommand::back, true};
    }
    if (screen_ == MenuScreen::travel)
    {
        open(MenuScreen::world_preview);
        return {MenuCommand::resume_game, true};
    }
    if (screen_ == MenuScreen::multiplayer)
    {
        open(MenuScreen::main);
        return {MenuCommand::back, true};
    }
    if (screen_ == MenuScreen::difficulty)
    {
        open(MenuScreen::game_mode);
        return {MenuCommand::back, true};
    }
    if (screen_ == MenuScreen::game_mode)
    {
        open(game_flow_origin_ == MenuScreen::main ? MenuScreen::game_profile : game_flow_origin_);
        return {MenuCommand::back, true};
    }
    if (screen_ == MenuScreen::host_setup)
    {
        open(MenuScreen::multiplayer);
        return {MenuCommand::back, true};
    }
    if (screen_ == MenuScreen::host_details || screen_ == MenuScreen::host_access ||
        screen_ == MenuScreen::host_gameplay)
    {
        open(MenuScreen::host_setup);
        return {MenuCommand::back, true};
    }
    if (screen_ == MenuScreen::join_details)
    {
        open(MenuScreen::join_browser);
        return {MenuCommand::back, true};
    }
    if (screen_ == MenuScreen::join_browser)
    {
        open(MenuScreen::multiplayer);
        return {MenuCommand::back, true};
    }
    if (screen_ == MenuScreen::world_select)
    {
        open(MenuScreen::game_mode);
        return {MenuCommand::back, true};
    }
    open(MenuScreen::main);
    return {MenuCommand::back, true};
}

void MenuModel::open(MenuScreen screen)
{
    // The pause overlay starts at the pause menu and is inherited by whatever
    // is opened from there. It ends only where the session's menu ends: back in
    // the world, or on the main menu after leaving it.
    if (screen == MenuScreen::pause)
        pause_overlay_ = true;
    else if (screen == MenuScreen::world_preview || screen == MenuScreen::main)
        pause_overlay_ = false;
    screen_ = screen;
    selection_ = 0U;
    ++transition_serial_;
}

void MenuModel::set_world_entries(std::vector<MenuEntry> entries)
{
    world_entries_ = std::move(entries);
    if (screen_ == MenuScreen::world_select)
    {
        selection_ = std::min(selection_, world_entries_.empty() ? 0U : world_entries_.size() - 1U);
    }
}

void MenuModel::set_join_entries(std::vector<MenuEntry> entries)
{
    join_entries_ = std::move(entries);
    if (screen_ == MenuScreen::join_browser)
    {
        selection_ = std::min(selection_, join_entries_.empty() ? 0U : join_entries_.size() - 1U);
    }
}

void MenuModel::set_in_game_facts(const InGameMenuFacts &facts)
{
    if (facts == in_game_facts_)
        return;
    // Keep the cursor on the row it was on: a row appearing or disappearing
    // above it (Host Options, Clear Ban List, Teleport To Player) must not
    // slide a different command under the player's finger.
    const bool live = screen_ == MenuScreen::pause || screen_ == MenuScreen::teleport ||
                      screen_ == MenuScreen::host_options;
    const MenuCommand selected =
        live && selection_ < entries().size() ? entries()[selection_].command : MenuCommand::none;
    in_game_facts_ = facts;
    in_game_entries_ = in_game_menu_entries(facts);
    merge_mod_entries(1U); // (UI area) mod rows stay before Main Menu
    teleport_entries_ = teleport_menu_entries(facts);
    host_option_entries_ = host_options_entries(facts);
    if (!live)
        return;
    const auto &current = entries();
    const auto kept = std::find_if(current.begin(), current.end(), [selected](const MenuEntry &entry) {
        return entry.command == selected;
    });
    selection_ = kept != current.end() ? static_cast<std::size_t>(kept - current.begin())
                                       : std::min(selection_, current.empty() ? 0U : current.size() - 1U);
}

void MenuModel::set_player_entries(std::vector<MenuEntry> entries)
{
    player_entries_ = std::move(entries);
    if (screen_ == MenuScreen::select_player)
        selection_ = std::min(selection_, player_entries_.empty() ? 0U : player_entries_.size() - 1U);
}

void MenuModel::clamp_selection(const Settings &settings) noexcept
{
    const std::size_t count = selectable_count(settings);
    selection_ = count == 0U ? 0U : std::min(selection_, count - 1U);
}

// ---- Mod rows (UI area, modding/ui.hpp) --------------------------------------

std::optional<std::size_t> MenuModel::mod_slot(MenuScreen screen) noexcept
{
    switch (screen)
    {
    case MenuScreen::main:
        return 0U;
    case MenuScreen::pause:
        return 1U;
    case MenuScreen::addons:
        return 2U;
    default:
        return std::nullopt;
    }
}

void MenuModel::merge_mod_entries(std::size_t slot)
{
    if (slot >= mod_entries_.size())
        return;
    const std::vector<MenuEntry> &base = slot == 0U   ? main_entries
                                         : slot == 1U ? in_game_entries_
                                         : addon_screen_entries_.empty() ? addon_entries
                                                                         : addon_screen_entries_;
    std::vector<MenuEntry> &merged = merged_entries_[slot];
    merged = base;
    // Before Exit / Main Menu, which stay last; after the Add-Ons screen's own.
    const std::size_t at = slot == 2U || merged.empty() ? merged.size() : merged.size() - 1U;
    merged.insert(merged.begin() + static_cast<std::ptrdiff_t>(at), mod_entries_[slot].begin(),
                  mod_entries_[slot].end());
    mod_first_[slot] = at;
}

void MenuModel::set_mod_entries(MenuScreen screen, std::vector<MenuEntry> rows)
{
    const std::optional<std::size_t> slot = mod_slot(screen);
    if (!slot)
        return;
    for (MenuEntry &entry : rows)
        entry.command = MenuCommand::mod_entry;
    mod_entries_[*slot] = std::move(rows);
    merge_mod_entries(*slot);
    if (screen_ == screen)
    {
        const std::size_t count = entries().size();
        selection_ = count == 0U ? 0U : std::min(selection_, count - 1U);
    }
}

std::optional<std::size_t> MenuModel::selected_mod_entry() const noexcept
{
    const std::optional<std::size_t> slot = mod_slot(screen_);
    if (!slot || mod_entries_[*slot].empty())
        return std::nullopt;
    if (selection_ < mod_first_[*slot] || selection_ >= mod_first_[*slot] + mod_entries_[*slot].size())
        return std::nullopt;
    return selection_ - mod_first_[*slot];
}

// ---- Installed add-on rows (Add-Ons) -------------------------------------------

void MenuModel::set_addon_entries(std::vector<MenuEntry> entries)
{
    addon_screen_entries_ = addon_entries;
    addon_first_ = addon_screen_entries_.size();
    addon_count_ = entries.size();
    addon_screen_entries_.insert(addon_screen_entries_.end(), entries.begin(), entries.end());
    merge_mod_entries(2U);
    if (screen_ == MenuScreen::addons)
    {
        const std::size_t count = this->entries().size();
        selection_ = count == 0U ? 0U : std::min(selection_, count - 1U);
    }
}

std::optional<std::size_t> MenuModel::selected_addon_entry() const noexcept
{
    if (screen_ != MenuScreen::addons || selection_ < addon_first_ || selection_ >= addon_first_ + addon_count_)
        return std::nullopt;
    return selection_ - addon_first_;
}

} // namespace openminerz::ui
