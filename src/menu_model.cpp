#include "openminerz/menu_model.hpp"

#include <algorithm>

namespace openminerz::ui
{
namespace
{

const std::vector<MenuEntry> main_entries{
    {"PLAY SOLO", "Create a world or continue an existing local save.", MenuCommand::play_offline,
     true, "LOCAL"},
    {"MULTIPLAYER", "Host a Steam lobby or browse live sessions.", MenuCommand::open_multiplayer,
     true, "STEAM"},
    {"ADD-ONS", "Inspect installed content and load order.", MenuCommand::open_addons, true,
     "CONTENT"},
    {"SETTINGS", "Display, graphics, controls, audio, gameplay, and accessibility.",
     MenuCommand::open_settings, true, "OPTIONS"},
    {"CREDITS", "OpenMinerZ reconstruction notes and acknowledgements.", MenuCommand::open_credits,
     true, "ABOUT"},
    {"QUIT", "Close OpenMinerZ.", MenuCommand::request_exit, true, "DESKTOP"},
};

const std::vector<MenuEntry> multiplayer_entries{
    {"HOST A GAME", "Create a Steam lobby and choose its rules, visibility, and world.",
     MenuCommand::host_online, true, "CREATE"},
    {"FIND A GAME", "Browse live Steam lobbies or use direct connect.", MenuCommand::join_online,
     true, "BROWSE"},
    {"BACK", "Return to the main menu.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> exit_entries{
    {"STAY HERE", "Return to the main menu.", MenuCommand::cancel_exit, true, {}},
    {"EXIT TO DESKTOP", "Close OpenMinerZ now.", MenuCommand::confirm_exit, true, {}},
};

const std::vector<MenuEntry> pause_entries{
    {"RETURN TO GAME", "Return to the current world.", MenuCommand::resume_game, true, {}},
    {"INVENTORY",
     "Open the backpack, crafting, and equipment view.",
     MenuCommand::open_inventory,
     true,
     {}},
    {"TELEPORT TO SURFACE",
     "Climb out of whatever hole you dug.",
     MenuCommand::teleport_surface,
     true,
     {}},
    {"TELEPORT TO START", "Return to the origin tower.", MenuCommand::teleport_start, true, {}},
    {"SETTINGS",
     "Adjust display, controls, audio, gameplay, and accessibility.",
     MenuCommand::open_settings,
     true,
     {}},
    {"MAIN MENU",
     "Leave the current world and return to the frontend.",
     MenuCommand::return_to_main,
     true,
     {}},
};

const std::vector<MenuEntry> host_setup_entries{
    {"SERVER DETAILS", "Name the lobby and choose its network compatibility and capacity.",
     MenuCommand::open_host_details, true, "3 SETTINGS"},
    {"ACCESS & PRIVACY", "Control discovery, passwords, and who may enter.",
     MenuCommand::open_host_access, true, "3 SETTINGS"},
    {"RULES & SAFETY",
     "Configure player damage, authority checks, safe joins, and spawn protection.",
     MenuCommand::open_host_gameplay, true, "4 SETTINGS"},
    {"MOD PLAYLIST", "Review client-only, server, and core synchronization requirements.",
     MenuCommand::review_host_mods, true, "REVIEW"},
    {"CONTINUE", "Choose game mode, difficulty, and the world to host.",
     MenuCommand::confirm_host_settings, true, "NEXT"},
    {"BACK", "Return to multiplayer.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> host_details_entries{
    {"SERVER NAME",
     "The lobby name shown to browsers and invited players.",
     MenuCommand::edit_host_name,
     true,
     {}},
    {"COMPATIBILITY",
     "Host the original Classic v3 or Modern v4 wire profile.",
     MenuCommand::select_host_protocol,
     true,
     {}},
    {"MAX PLAYERS",
     "Stock clients support one through sixteen total players.",
     MenuCommand::select_host_max_players,
     true,
     {}},
    {"BACK", "Return to the server-settings overview.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> host_access_entries{
    {"VISIBILITY",
     "Choose friends, invite-only, public lobby, or local-network discovery.",
     MenuCommand::select_host_visibility,
     true,
     {}},
    {"PASSWORD",
     "Set or clear the session password without displaying it in the menu.",
     MenuCommand::edit_host_password,
     true,
     {}},
    {"JOIN POLICY",
     "Admit anyone, an allow list, or explicitly invited players.",
     MenuCommand::select_host_join_policy,
     true,
     {}},
    {"BACK", "Return to the server-settings overview.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> host_gameplay_entries{
    {"PLAYER DAMAGE",
     "Disable PvP, allow everyone, or exclude Steam friends.",
     MenuCommand::select_host_pvp,
     true,
     {}},
    {"ANTI-CHEAT",
     "Enable gameplay authority checks in addition to the always-on packet guard.",
     MenuCommand::toggle_host_anticheat,
     true,
     {}},
    {"SAFE JOIN",
     "Resolve late joins onto verified terrain instead of a stale host position.",
     MenuCommand::toggle_host_join_safety,
     true,
     {}},
    {"SPAWN PROTECTION",
     "Reject client block edits inside the protected origin radius.",
     MenuCommand::select_host_spawn_protection,
     true,
     {}},
    {"BACK", "Return to the server-settings overview.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> join_details_entries{
    {"JOIN SESSION", "Submit the selected session to the bounded admission path.",
     MenuCommand::connect_join_session, true, "CONNECT"},
    {"PASSWORD",
     "Set or clear the selected session password without displaying it.",
     MenuCommand::edit_join_password,
     true,
     {}},
    {"BACK", "Return to the discovered session list.", MenuCommand::back, true, {}},
};

// Original GameModeMenu order; Dragon Endurance unlocks by killing the undead
// dragon in Endurance in the original — here it is always selectable.
const std::vector<MenuEntry> game_mode_entries{
    {"ENDURANCE",
     "March or die: each day adds distance-difficulty, dying restarts the run, and the "
     "world is throwaway.",
     MenuCommand::select_mode_endurance,
     true,
     {}},
    {"SURVIVAL", "The classic mode: build, mine, and survive the nights.",
     MenuCommand::select_mode_survival, true, "CLASSIC"},
    {"DRAGON ENDURANCE",
     "Fend off wave after wave of dragons, each tougher than the last.",
     MenuCommand::select_mode_dragon_endurance,
     true,
     {}},
    {"CREATIVE",
     "Infinite resources, every recipe unlocked, and nothing can hurt you.",
     MenuCommand::select_mode_creative,
     true,
     {}},
    {"BACK", "Back to the main menu.", MenuCommand::back, true, {}},
};

// Original DifficultyLevelScreen order (default cursor on NORMAL).
const std::vector<MenuEntry> difficulty_entries{
    {"NO ENEMIES",
     "Build freely without enemy attacks.",
     MenuCommand::select_difficulty_no_enemies,
     true,
     {}},
    {"EASY",
     "Enemies do less damage and zombies only appear at night. Dragons will not "
     "damage your structures.",
     MenuCommand::select_difficulty_easy,
     true,
     {}},
    {"NORMAL",
     "Enemies appear when you cover fresh ground and swarm at night. Dragons will damage "
     "your structures.",
     MenuCommand::select_difficulty_normal,
     true,
     {}},
    {"HARDCORE",
     "Start with nothing and drop all items when you die.",
     MenuCommand::select_difficulty_hardcore,
     true,
     {}},
    {"BACK", "Back to game mode selection.", MenuCommand::back, true, {}},
};

const std::vector<MenuEntry> empty_entries{};

} // namespace

const std::vector<MenuEntry> &MenuModel::entries() const noexcept
{
    switch (screen_)
    {
    case MenuScreen::main:
        return main_entries;
    case MenuScreen::multiplayer:
        return multiplayer_entries;
    case MenuScreen::exit_confirmation:
        return exit_entries;
    case MenuScreen::pause:
        return pause_entries;
    case MenuScreen::game_mode:
        return game_mode_entries;
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
    case MenuScreen::addons:
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
    case MenuCommand::open_addons:
        open(MenuScreen::addons);
        return {command, true};
    case MenuCommand::open_settings:
        return_screen_ = screen_;
        open(MenuScreen::settings);
        clamp_selection(settings);
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
        open(game_flow_origin_);
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
    if (screen_ == MenuScreen::settings && return_screen_ == MenuScreen::pause)
    {
        return_screen_ = MenuScreen::main;
        open(MenuScreen::pause);
        return {MenuCommand::back, true};
    }
    open(MenuScreen::main);
    return {MenuCommand::back, true};
}

void MenuModel::open(MenuScreen screen)
{
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

void MenuModel::clamp_selection(const Settings &settings) noexcept
{
    const std::size_t count = selectable_count(settings);
    selection_ = count == 0U ? 0U : std::min(selection_, count - 1U);
}

} // namespace openminerz::ui
