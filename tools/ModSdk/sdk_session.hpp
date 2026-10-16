#pragma once

// The modding SDK's stand-in for the game.
//
// Sandbox registers every area's script API exactly as the game does
// (src/app_mod_runtime.inc, register_mod_apis: gameplay, graphics, UI, then
// network) on small fake backends, so the registry it holds is the one mods
// see in the game. The API reference, the Luau type definitions and the
// validator read that registry; nothing about the API is written by hand.
//
// It also runs mods headless, the way an offline world does: a server VM and a
// client VM in one process, the mod channel over the in-process loopback,
// Gameplay's replication, the UI's channel and HUD, and the graphics service
// with the mods' asset folders. The fake world is a flat plain with one local
// player, so example mods and `openminerz_mod_sdk run` can be stepped and
// checked without a window, a GPU or the game's content.

#include "openminerz/inventory.hpp"
#include "openminerz/modding/api.hpp"
#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/gameplay_net.hpp"
#include "openminerz/modding/graphics.hpp"
#include "openminerz/modding/net.hpp"
#include "openminerz/modding/runtime.hpp"
#include "openminerz/modding/ui.hpp"
#include "openminerz/modding/worldgen.hpp" // mod-world
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/world/custom_terrain.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openminerz::mod_sdk
{

// A flat world: bedrock deep down, rock, three layers of dirt, grass at y = 0
// and air above; one local player standing at (8.5, 1, 8.5) with an empty
// inventory. Edits, pickups, chat lines and damage are recorded for checks.
class SandboxWorld final : public modding::gameplay::GameplayBackend
{
  public:
    SandboxWorld();

    std::int32_t seed{20261014};
    bool modern{true};
    std::string local_name{"Sandbox"};
    modding::Vec3 position{8.5, 1.0, 8.5};
    // Radians; yaw 0 looks along -Z, pi/2 along +X; pitch up is positive.
    float yaw{};
    float pitch{};
    // The block the crosshair is on (tests set it; nothing by default).
    std::optional<modding::gameplay::BlockPos> aimed;
    float health{100.0F};
    bool alive{true};
    float speed{1.0F};
    double clock{1.30}; // day 1, 07:12
    PlayerInventory inventory;
    std::map<modding::gameplay::BlockPos, std::uint8_t> edits;
    std::vector<std::pair<modding::Vec3, modding::gameplay::StackView>> pickups;
    std::vector<std::string> chat_lines;
    std::vector<int> spawned_enemies;
    std::vector<std::pair<float, modding::gameplay::DamageCause>> damage_taken;
    std::filesystem::path sidecar;
    // The game's damage path asks the service first, like apply_player_damage.
    modding::gameplay::GameplayService *service{};
    // mod-world: biomes come from the mods' world generator when they define
    // one (the plain's own bands otherwise).
    const world::WorldGenerator *generator{};

    // Day length of the retail game in seconds of play.
    static constexpr double day_seconds = 960.0;
    void advance(double seconds) noexcept
    {
        clock += seconds / day_seconds;
    }
    [[nodiscard]] static std::uint8_t terrain(modding::gameplay::BlockPos cell) noexcept;

    [[nodiscard]] std::optional<modding::gameplay::LocalView> local_view() const override;
    [[nodiscard]] std::int32_t world_seed() const override;
    [[nodiscard]] bool modern_rules() const override;
    [[nodiscard]] std::optional<std::uint8_t> local_player() const override;
    void players(std::vector<modding::gameplay::PlayerSnapshot> &out) const override;
    void local_set_health(float value) override;
    void local_damage(float amount, modding::Vec3 source, modding::gameplay::DamageCause cause) override;
    void local_teleport(modding::Vec3 feet) override;
    std::uint16_t local_give(modding::gameplay::StackView stack, bool drop_rest) override;
    std::uint32_t local_take(std::uint16_t item, std::uint32_t mod_item, std::uint32_t count) override;
    [[nodiscard]] std::uint32_t local_count(std::uint16_t item, std::uint32_t mod_item) const override;
    void local_clear_inventory() override;
    [[nodiscard]] std::optional<modding::gameplay::StackView> local_held() const override;
    void local_slots(std::vector<std::pair<std::uint16_t, modding::gameplay::StackView>> &out) const override;
    bool local_tag_slot(std::uint16_t slot, std::uint16_t item, std::uint32_t mod_item) override;
    void local_set_speed(float multiplier) override;
    [[nodiscard]] std::optional<std::uint8_t> block(modding::gameplay::BlockPos cell) const override;
    bool set_block(modding::gameplay::BlockPos cell, std::uint8_t block) override;
    [[nodiscard]] int biome(int x, int z) const override;
    [[nodiscard]] std::optional<std::array<float, 2>> light(modding::gameplay::BlockPos cell) const override;
    bool set_crate_slot(modding::gameplay::BlockPos crate, std::size_t index,
                        std::optional<modding::gameplay::StackView> stack) override;
    bool spawn_pickup(modding::Vec3 at, modding::gameplay::StackView stack) override;
    [[nodiscard]] double day() const override;
    void set_time_of_day(float time_of_day) override;
    bool spawn_enemy(int type, modding::Vec3 at) override;
    void chat(std::optional<std::uint8_t> gid, std::string_view text) override;
    [[nodiscard]] std::filesystem::path sidecar_directory() const override;

  private:
    template <typename Visit>
    void visit(Visit &&function);
};

struct SandboxOptions final
{
    // Where world and machine storage go; empty keeps everything in memory.
    std::filesystem::path data_dir;
    // Wall-clock budgets as in the game (2 s per file, 100 ms per call).
    modding::RuntimeLimits limits{};
};

// What one headless run drew and logged, for checks and the CLI.
struct SandboxReport final
{
    std::size_t errors{};
    std::size_t warnings{};
    std::size_t hud_commands{};    // UiService::draw_hud + draw_notices
    std::size_t screen_commands{}; // UiService::draw_screens
    std::size_t lights{};
    std::size_t atmosphere_layers{};
    std::size_t models{};
    std::size_t entities{}; // mod creatures the client sees
};

class Sandbox final
{
  public:
    explicit Sandbox(SandboxOptions options = {});
    ~Sandbox();
    Sandbox(const Sandbox &) = delete;
    Sandbox &operator=(const Sandbox &) = delete;

    [[nodiscard]] modding::ApiRegistry &registry() noexcept;
    [[nodiscard]] modding::ModRuntime &runtime() noexcept;
    [[nodiscard]] modding::gameplay::GameplayService &gameplay() noexcept;
    [[nodiscard]] modding::ui::UiService &ui() noexcept;
    [[nodiscard]] modding::graphics::GraphicsService &graphics() noexcept;
    [[nodiscard]] SandboxWorld &world() noexcept;
    [[nodiscard]] modding::ModNetServer *net_server() noexcept;
    [[nodiscard]] modding::ModNetClient *net_client() noexcept;
    // mod-world: the world generator service, and the world the mods defined
    // (built once their scripts loaded; null when none did, or it failed - a
    // script error is logged then). terrain_generator() generates it with the
    // sandbox's seed.
    [[nodiscard]] modding::worldgen::WorldgenService &worldgen() noexcept;
    [[nodiscard]] const std::shared_ptr<const world::CustomTerrain> &terrain() const noexcept;
    [[nodiscard]] const world::WorldGenerator &terrain_generator() const noexcept;

    // Starts an offline session with `mods` (catalog order), as the game
    // starts one for an offline world: mod channel, server VM, client VM,
    // Gameplay's halves, world.loaded and the player's spawn. False when no
    // VM started (`error` says why); refused mods are in `refused`.
    bool start(std::span<const net::LoadedMod> mods, std::string &error);
    // Steps `seconds` of play in fixed 1/60 s simulation steps, each followed
    // by a client frame, the UI and graphics update and a HUD draw.
    void run(double seconds);
    void stop();
    [[nodiscard]] bool running() const noexcept;

    // Every log line so far, oldest first (also printed when `echo`).
    [[nodiscard]] const std::vector<modding::LogEntry> &log() const noexcept;
    void set_echo(bool echo) noexcept;
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>> &refused() const noexcept;
    [[nodiscard]] SandboxReport report() const;
    // A chat line typed by the local player (mod commands start with '/').
    modding::gameplay::ChatVerdict chat(std::string_view line);

    class Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

// One mod folder as the game's catalog sees it (descriptor, digest), without
// requiring its siblings to be valid: the folder is loaded through its parent
// root, or through a private copy when a sibling breaks that catalog.
[[nodiscard]] bool load_mod_folder(const std::filesystem::path &folder, net::LoadedMod &mod,
                                   std::string &error);

} // namespace openminerz::mod_sdk
