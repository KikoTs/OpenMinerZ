#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace openminerz
{

struct LaunchOptions final
{
    int width{};
    int height{};
    bool open_settings{};
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
    bool open_world{};
    bool open_world_select{};
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
