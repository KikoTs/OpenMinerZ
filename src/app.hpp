#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openminerz
{

struct LaunchOptions final
{
    int width{};
    int height{};
    bool open_settings{};
    bool open_ui_settings{};
    int diagnostic_settings_tab{-1};
    std::string diagnostic_menu;
    std::size_t settings_selection{};
    bool open_host_settings{};
    int host_settings_section{};
    bool open_join_browser{};
    bool open_join_details{};
    // Drives the real join-browser selection and connect commands for one
    // discovered lobby, then exits after world installation or a join failure.
    std::optional<std::uint64_t> ui_join_probe_lobby;
    bool diagnostic_join_browser{};
    bool diagnostic_join_details{};
    bool diagnostic_online_loading{};
    bool diagnostic_multiplayer{};
    std::optional<float> diagnostic_death_seconds;
    bool open_world{};
    bool open_world_select{};
    std::optional<std::filesystem::path> world_directory;
    std::optional<std::filesystem::path> world_file;
    std::size_t world_library_selection{1U};
    bool diagnostic_save_exit{};
    bool open_inventory{};
    bool no_enemies{};
    bool force_lightning{};
    int world_biome{};
    int view_distance{};
    std::optional<float> world_time;
    std::optional<float> world_pitch;
    std::optional<float> world_yaw;
    std::optional<std::string> preview_environment;
    std::optional<int> preview_item;
    std::optional<int> preview_pickup_item;
    std::optional<std::string> preview_viewmodel;
    float preview_viewmodel_progress{};
    std::optional<std::filesystem::path> screenshot_path;
    std::optional<std::filesystem::path> network_trace_path;
    std::optional<std::filesystem::path> state_dump_path;
    // Optional fixed world-space X/Z center for comparable interop barriers.
    // When absent, diagnostics retain the convenient local-player-centered capture.
    std::optional<std::array<int, 2>> state_region_center;
    std::optional<bool> craft_modern;
    // Diagnostic override of Settings::ui_style (the interface style) for this
    // run: 0 automatic, 1 classic, 2 modern, 3 remastered; negative keeps the
    // saved setting.
    int ui_style{-1};
    // Capture aids for the Remastered interface, none of them saved choices:
    // the vitals look (VitalsStyle's value; negative keeps the setting), the
    // interface scale (zero keeps it), single HUD switches as "name=0|1", and
    // controller prompts without a controller.
    int vitals_style{-1};
    float ui_scale{};
    std::vector<std::string> hud_options;
    bool gamepad_prompts{};
    // Drives the inventory / crate / crafting screen once the world is up
    // (see Application::Impl::run_screen_script for the commands).
    std::optional<std::string> screen_script;
    // A flat colour drawn in place of the world behind those screens, so a
    // capture can be compared pixel for pixel with a recording of the original.
    std::optional<std::array<std::uint8_t, 3>> screen_backdrop;
    // Startup-only content-pack override: 0 Classic, 1 Modern, -1 saved setting.
    int asset_style{-1};
    // Diagnostic presentation override: 0 Classic, 1 Rebirth, -1 saved setting.
    int presentation_profile{-1};
    int bench_frames{};
    int spawn_enemy{-1};
    int spawn_dragon{-1};
    // Tone curve override for comparison captures: 0 legacy, 1 Reinhard,
    // 2 filmic, 3 ACES. Negative keeps the saved setting.
    int tone_mapping{-1};
    // Composed stills (tools/Capture-MenuBackground.ps1). None of these is a
    // saved choice or touches the simulation: the player stays where it
    // spawned and only the drawn camera moves.
    //   world_seed     the --world preview's seed instead of the built-in one
    //   still_camera   world-space eye position the frame is drawn from
    //   still_fov      vertical field of view of that frame, in degrees
    //   clean_view     nothing but the world: no HUD, viewmodel, nametags,
    //                  notices or frame counter
    //   menu_backdrop  a specific picture behind the menus for this run instead
    //                  of the one in the local data folder (reviewing candidates)
    std::optional<std::int32_t> world_seed;
    std::optional<std::array<float, 3>> still_camera;
    // mod-world: --teleport X,Z starts the player at that column (on the
    // ground), terrain and all, for captures far from the origin.
    std::optional<std::array<float, 2>> teleport;
    std::optional<float> still_fov;
    bool clean_view{};
    std::optional<std::filesystem::path> menu_backdrop;
};

class Application final
{
  public:
    explicit Application(LaunchOptions options);
    ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    int run();

  private:
    class Impl;
    Impl *impl_{};
};

} // namespace openminerz
