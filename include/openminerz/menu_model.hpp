#pragma once

#include "openminerz/settings.hpp"
#include "openminerz/settings_menu.hpp"

#include <array>
#include <cstddef>
#include <optional>
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
    achievements,
    travel,
    game_profile,
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
    exit_confirmation,
    // The screens the original GameScreen pushes on top of InGameMenu:
    // TeleportMenu, HostOptions, and the SelectPlayerScreen both of them use.
    teleport,
    host_options,
    select_player
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
    // (Authority) Admit only OpenMinerZ players; the host then owns health,
    // damage, items and edits (net/session_authority.hpp).
    toggle_host_openminerz_only,
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
    open_achievements,
    use_teleport_station,
    select_profile_classic,
    select_profile_modern,
    select_mode_endurance,
    select_mode_survival,
    select_mode_dragon_endurance,
    select_mode_creative,
    select_mode_exploration,
    select_mode_scavenger,
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
    back,
    // InGameMenu, TeleportMenu and HostOptions rows (see InGameMenuFacts).
    open_teleport,
    teleport_to_player,
    open_controls,
    open_host_options,
    host_kick_player,
    host_ban_player,
    host_restart,
    host_cycle_pvp,
    host_set_password,
    host_server_message,
    host_clear_ban_list,
    host_cycle_join_policy,
    // A row of the player picker. The app maps the row to a session GID.
    choose_player,
    // The Add-Ons screen's "Xbox Avatars" rows (presentation only).
    toggle_avatar_mine,
    toggle_avatar_others,
    reload_avatar,
    // (Add-Ons) An installed mod's row, which turns it on or off (the app
    // asks selected_addon_entry() which one), and "Open Mods Folder".
    toggle_addon,
    open_mods_folder,
    // A row a mod added (modding/ui.hpp, UI area): the app asks
    // selected_mod_entry() which one.
    mod_entry
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

// Everything the shipped InGameMenu, TeleportMenu and HostOptions read to decide
// which rows exist (CastleMinerZ.exe 1.6.3.8 and 1.9.9.8, read from the
// binaries rather than from the OpenClassic source tree):
//   InGameMenu   Teleport only when GameMode != Endurance && Difficulty !=
//                HARDCORE; Host Options only for the online session's host.
//                1.6.3.8 lists Controls and Settings; 1.9.9.8 folds them into
//                one Options row. Invite Friends exists but is never visible.
//   TeleportMenu Teleport To Player only when IsOnlineGame, and in 1.9.9.8 only
//                while PVPState == Off.
//   HostOptions  Clear Ban List only while the ban list is not empty.
struct InGameMenuFacts final
{
    bool modern{};
    bool endurance{};
    bool hardcore{};
    bool online{};
    bool host{};
    // CastleMinerZGame.PVPEnum: 0 Off, 1 Everyone, 2 NotFriends.
    int pvp{};
    bool ban_list_empty{true};
    // JoinGamePolicy: 0 Anyone, 1 FriendsOnly, 2 InviteOnly.
    int join_policy{};

    bool operator==(const InGameMenuFacts &) const = default;
};

[[nodiscard]] std::vector<MenuEntry> in_game_menu_entries(const InGameMenuFacts &facts);
[[nodiscard]] std::vector<MenuEntry> teleport_menu_entries(const InGameMenuFacts &facts);
[[nodiscard]] std::vector<MenuEntry> host_options_entries(const InGameMenuFacts &facts);
// HostOptions' "PVP: <state>" row, which is also the line it broadcasts.
[[nodiscard]] std::string_view pvp_state_text(int pvp) noexcept;
[[nodiscard]] std::string_view join_policy_text(int join_policy) noexcept;

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
    // The in-game pause menu, or ANY screen reached from it. Opening the pause
    // menu starts the overlay and every screen opened after that inherits it,
    // with nothing to register, until navigation lands back in the world or on
    // the main menu (see open()). The original never pops InGameMenu under the
    // screens it pushes, so this is the one fact the pause contract in
    // input_contract.hpp needs: an online world keeps simulating behind all of
    // them with the controls released, a private one stays frozen, and Back
    // from any of them returns to the pause menu.
    [[nodiscard]] bool pause_overlay_active() const noexcept
    {
        return pause_overlay_;
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
    void set_travel_entries(std::vector<MenuEntry> entries) { travel_entries_ = std::move(entries); }
    // Rebuilds the in-game menu, the teleport menu and host options from the
    // session as it is now. Cheap when nothing changed; the app calls it every
    // frame, so a row never outlives the condition that showed it.
    void set_in_game_facts(const InGameMenuFacts &facts);
    // The player picker's rows, one per remote player; label views follow the
    // caller-owned lifetime contract of set_world_entries.
    void set_player_entries(std::vector<MenuEntry> entries);
    // (UI area) Rows mods add: on the main menu before Exit, on the in-game
    // menu before Main Menu, on the Add-Ons screen after its own rows. Their
    // command is mod_entry; label views follow the caller-owned lifetime
    // contract of set_world_entries. With none, every list is exactly as before.
    void set_mod_entries(MenuScreen screen, std::vector<MenuEntry> entries);
    // The selected row's index in the list given to set_mod_entries, when the
    // selection is on a mod row.
    [[nodiscard]] std::optional<std::size_t> selected_mod_entry() const noexcept;
    // (Add-Ons) The Add-Ons screen's rows for installed mods (and its "Open
    // Mods Folder" row), right after the Xbox Avatars rows and before the
    // rows mods add. Label views follow the caller-owned contract of
    // set_world_entries. The selection stays on the row index it was on.
    void set_addon_entries(std::vector<MenuEntry> entries);
    // The selected row's index in the list given to set_addon_entries.
    [[nodiscard]] std::optional<std::size_t> selected_addon_entry() const noexcept;
    void set_gameplay_profile(bool modern, bool dragon_unlocked = true) noexcept
    {
        modern_gameplay_ = modern;
        dragon_unlocked_ = dragon_unlocked;
    }

  private:
    void clamp_selection(const Settings &settings) noexcept;

    MenuScreen screen_{MenuScreen::main};
    bool modern_gameplay_{};
    bool dragon_unlocked_{true};
    std::vector<MenuEntry> world_entries_;
    std::vector<MenuEntry> join_entries_;
    std::vector<MenuEntry> travel_entries_;
    InGameMenuFacts in_game_facts_;
    std::vector<MenuEntry> in_game_entries_{in_game_menu_entries({})};
    std::vector<MenuEntry> teleport_entries_{teleport_menu_entries({})};
    std::vector<MenuEntry> host_option_entries_{host_options_entries({})};
    std::vector<MenuEntry> player_entries_;
    // Where Back leaves the player picker: the original pops straight to the
    // world when it was opened for a teleport and to HostOptions otherwise.
    MenuScreen player_return_screen_{MenuScreen::pause};
    std::size_t selection_{};
    SettingsTab settings_tab_{SettingsTab::display};
    MenuScreen return_screen_{MenuScreen::main};
    bool pause_overlay_{};
    MenuScreen game_flow_origin_{MenuScreen::main};
    std::size_t transition_serial_{};
    // (UI area) Mod rows for the main menu, the in-game menu and the Add-Ons
    // screen, and each of those lists with them merged in (empty: no rows).
    [[nodiscard]] static std::optional<std::size_t> mod_slot(MenuScreen screen) noexcept;
    void merge_mod_entries(std::size_t slot);
    std::array<std::vector<MenuEntry>, 3> mod_entries_;
    std::array<std::vector<MenuEntry>, 3> merged_entries_;
    std::array<std::size_t, 3> mod_first_{};
    // (Add-Ons) The Add-Ons screen's own rows: Xbox Avatars, then the
    // installed mods (addon_count_ rows from addon_first_).
    std::vector<MenuEntry> addon_screen_entries_;
    std::size_t addon_first_{};
    std::size_t addon_count_{};
};

} // namespace openminerz::ui
