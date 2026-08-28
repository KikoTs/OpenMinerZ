#pragma once

#include "openminerz/settings.hpp"
#include "openminerz/settings_menu.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace openminerz::ui
{

enum class MenuScreen
{
    main,
    multiplayer,
    addons,
    settings,
    credits,
    host_setup,
    host_details,
    host_access,
    host_gameplay,
    join_browser,
    join_details,
    game_mode,
    difficulty,
    world_select,
    world_preview,
    pause,
    exit_confirmation
};
enum class MenuCommand
{
    none,
    continue_game,
    play_offline,
    open_multiplayer,
    host_online,
    join_online,
    open_host_details,
    open_host_access,
    open_host_gameplay,
    edit_host_name,
    select_host_protocol,
    select_host_max_players,
    select_host_visibility,
    edit_host_password,
    select_host_join_policy,
    select_host_pvp,
    toggle_host_anticheat,
    toggle_host_join_safety,
    select_host_spawn_protection,
    review_host_mods,
    confirm_host_settings,
    refresh_join_sessions,
    cycle_join_filter,
    direct_connect,
    select_join_session,
    connect_join_session,
    edit_join_password,
    open_addons,
    open_settings,
    open_credits,
    select_mode_endurance,
    select_mode_survival,
    select_mode_dragon_endurance,
    select_mode_creative,
    select_difficulty_no_enemies,
    select_difficulty_easy,
    select_difficulty_normal,
    select_difficulty_hardcore,
    create_new_world,
    select_world,
    resume_game,
    open_inventory,
    teleport_surface,
    teleport_start,
    return_to_main,
    request_exit,
    confirm_exit,
    cancel_exit,
    back
};

struct MenuEntry final
{
    std::string_view label;
    std::string_view description;
    MenuCommand command{MenuCommand::none};
    bool enabled{true};
    std::string_view badge;
};

struct MenuEvent final
{
    MenuCommand command{MenuCommand::none};
    bool screen_changed{false};
};

class MenuModel final
{
  public:
    [[nodiscard]] MenuScreen screen() const noexcept
    {
        return screen_;
    }
    [[nodiscard]] std::size_t selection() const noexcept
    {
        return selection_;
    }
    [[nodiscard]] SettingsTab settings_tab() const noexcept
    {
        return settings_tab_;
    }
    [[nodiscard]] std::size_t transition_serial() const noexcept
    {
        return transition_serial_;
    }

    [[nodiscard]] const std::vector<MenuEntry> &entries() const noexcept;
    [[nodiscard]] std::size_t selectable_count(const Settings &settings) const noexcept;

    void select_next(int direction, const Settings &settings);
    void set_selection(std::size_t index, const Settings &settings);
    void change_tab(int direction, const Settings &settings);
    MenuEvent adjust_selected(Settings &settings, int direction);
    MenuEvent activate(Settings &settings);
    MenuEvent back();
    void open(MenuScreen screen);
    // The world-select screen lists saved worlds at runtime; label/description
    // views must point into storage the caller keeps alive.
    void set_world_entries(std::vector<MenuEntry> entries);
    // Join-browser label/description views follow the same caller-owned
    // lifetime contract as saved-world entries.
    void set_join_entries(std::vector<MenuEntry> entries);

  private:
    void clamp_selection(const Settings &settings) noexcept;

    MenuScreen screen_{MenuScreen::main};
    std::vector<MenuEntry> world_entries_;
    std::vector<MenuEntry> join_entries_;
    std::size_t selection_{};
    SettingsTab settings_tab_{SettingsTab::display};
    MenuScreen return_screen_{MenuScreen::main};
    MenuScreen game_flow_origin_{MenuScreen::main};
    std::size_t transition_serial_{};
};

} // namespace openminerz::ui
