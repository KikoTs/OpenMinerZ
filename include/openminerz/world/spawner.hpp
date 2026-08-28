#pragma once

#include "openminerz/net/enemy_codec.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/enemy.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace openminerz::world
{

enum class SpawnerState : std::uint8_t
{
    none,
    chargeable,
    listening,
    start_triggered,
    activated,
    spawning,
    wave_intermission,
    depleted,
    completed,
    reward_collected,
    reset,
};

enum class SpawnerLightState : std::uint8_t
{
    none,
    on,
    off,
};

enum class SpawnerPackageFlags : std::uint32_t
{
    none = 0,
    common = 1,
    normal = 2,
    rare = 4,
    epic = 8,
    legendary = 0x10,
    alien = 0x20,
    hell = 0x40,
    desert = 0x80,
    forest = 0x100,
    mountain = 0x200,
    volcano = 0x400,
    underground = 0x800,
    sky_island = 0x1000,
    dragon = 0x2000,
    champion = 0x4000,
    boss = 0x8000,
    undead_dragon = 0x10000,
    current_biome = 0x20000,
};

[[nodiscard]] constexpr SpawnerPackageFlags operator|(SpawnerPackageFlags left,
                                                      SpawnerPackageFlags right) noexcept
{
    return static_cast<SpawnerPackageFlags>(static_cast<std::uint32_t>(left) |
                                            static_cast<std::uint32_t>(right));
}

struct SpawnerDefinition final
{
    BlockType off_block{};
    BlockType on_block{};
    BlockType dim_block{};
    int spawn_points{};
    float spawn_rate_seconds{};
    int wave_count{};
    float wave_intermission_seconds{};
    int minimum_tier{};
    int maximum_tier{};
    int base_lucky_loot_chance{};
    int lucky_loot_chance_per_tier{};
    SpawnerPackageFlags package_flags{};

    bool operator==(const SpawnerDefinition &) const = default;
};

struct SpawnerRuntimeDefinition final
{
    SpawnerDefinition source;
    int spawn_points{};
    int maximum_tier{};

    bool operator==(const SpawnerRuntimeDefinition &) const = default;
};

[[nodiscard]] std::span<const SpawnerDefinition> spawner_definitions() noexcept;
[[nodiscard]] const SpawnerDefinition *find_spawner_definition(BlockType block) noexcept;
[[nodiscard]] bool is_spawner_on_block(BlockType block) noexcept;
// AlienHordeOff is deliberately absent: the shipped BlockType.IsSpawnerClickable
// implementation has the same omission.
[[nodiscard]] bool is_spawner_clickable(BlockType block) noexcept;
[[nodiscard]] bool is_spawner_preserved_loot(BlockType block) noexcept;
[[nodiscard]] float spawner_soft_capped_value(float value, float soft_cap, float hard_cap,
                                              float factor = 3.0F) noexcept;
[[nodiscard]] std::optional<SpawnerRuntimeDefinition> apply_spawner_difficulty(
    const SpawnerDefinition &definition, int defeated_spawner_count, int difficulty,
    EnemyInstanceRng &random) noexcept;

struct SpawnerSpawnEntry final
{
    std::uint8_t enemy_type{};
    int weight{};
    int tier{};
    int minimum_world_level{};
    int maximum_world_level{};
    int minimum_quantity{};
    int maximum_quantity{};
    int worth{};
    SpawnerPackageFlags package_flags{};

    bool operator==(const SpawnerSpawnEntry &) const = default;
};

struct SpawnerSpawnChoice final
{
    std::uint8_t enemy_type{};
    int count{};
    int value{};

    bool operator==(const SpawnerSpawnChoice &) const = default;
};

constexpr int any_minimum_world_level = -1000;
constexpr int any_maximum_world_level = 1000;

[[nodiscard]] std::span<const SpawnerSpawnEntry> spawner_spawn_entries() noexcept;
[[nodiscard]] bool is_spawner_spawn_candidate(const SpawnerSpawnEntry &entry, int world_level,
                                              int minimum_tier, int maximum_tier, bool rare_loot,
                                              int maximum_value,
                                              SpawnerPackageFlags permitted_flags) noexcept;
[[nodiscard]] std::optional<SpawnerSpawnChoice> select_spawner_spawn(
    EnemyInstanceRng &random, int world_level, int minimum_tier, int maximum_tier, bool rare_loot,
    int maximum_value, SpawnerPackageFlags permitted_flags) noexcept;
[[nodiscard]] std::optional<net::EnemyInitPayload> make_spawner_enemy_initialization(
    std::uint8_t enemy_type, float midnight, EnemyInstanceRng &random) noexcept;

struct SpawnerTarget final
{
    std::uint64_t peer{};
    std::uint8_t gid{};
    std::string gamer_tag;

    bool operator==(const SpawnerTarget &) const = default;
};

struct SpawnerClearSurroundingsAction final
{
    net::IntVector3 location;
    int radius{};
};
struct SpawnerTriggerLinkedAction final
{
    net::IntVector3 location;
    int radius{};
};
struct SpawnerActivateAction final
{
};
struct SpawnerBlockAction final
{
    net::IntVector3 location;
    BlockType block{};
};
struct SpawnerEnemyAction final
{
    SpawnerTarget target;
    net::SpawnEnemyPayload payload;
};
struct SpawnerCompletedAction final
{
    std::uint8_t last_killer_gid{};
};
struct SpawnerResetBroadcastAction final
{
};
struct SpawnerResetEnemiesAction final
{
    net::IntVector3 location;
};

using SpawnerAction =
    std::variant<SpawnerClearSurroundingsAction, SpawnerTriggerLinkedAction, SpawnerActivateAction,
                 SpawnerBlockAction, SpawnerEnemyAction, SpawnerCompletedAction,
                 SpawnerResetBroadcastAction, SpawnerResetEnemiesAction>;

struct SpawnerSimulationSnapshot final
{
    SpawnerState state{};
    SpawnerLightState light_state{};
    float spawn_timer_seconds{};
    float light_timer_seconds{};
    int remaining_spawn_value{};
    int active_enemy_count{};
    int remaining_wave_count{};
    int defeated_spawner_count_at_start{};
    std::optional<SpawnerRuntimeDefinition> runtime_definition;
};

class SpawnerSimulation final
{
  public:
    static constexpr float light_on_seconds = 0.8F;
    static constexpr float light_off_seconds = 0.3F;
    static constexpr int minimum_required_spawn_value = 100;
    static constexpr float target_range = 24.0F;
    static constexpr int clear_radius = 3;
    static constexpr int linked_spawner_radius = 10;

    using NextEnemyId = std::function<std::optional<std::int32_t>(const SpawnerTarget &)>;

    SpawnerSimulation(net::IntVector3 location, SpawnerDefinition definition,
                      EnemyInstanceRng &world_random, EnemyInstanceRng &enemy_random,
                      int reward_tier = 0) noexcept;

    [[nodiscard]] const net::IntVector3 &location() const noexcept;
    [[nodiscard]] const SpawnerDefinition &definition() const noexcept;
    [[nodiscard]] int reward_tier() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool can_start() const noexcept;
    [[nodiscard]] SpawnerSimulationSnapshot snapshot() const;

    [[nodiscard]] std::vector<SpawnerAction> start(int defeated_spawner_count, int difficulty);
    [[nodiscard]] std::vector<SpawnerAction> complete_start();
    [[nodiscard]] std::vector<SpawnerAction> advance(float elapsed_seconds,
                                                     std::span<const SpawnerTarget> nearby_targets,
                                                     const SpawnerTarget &fallback_target,
                                                     const NextEnemyId &next_enemy_id);
    [[nodiscard]] std::vector<SpawnerAction> handle_enemy_defeated(std::uint8_t killer_gid);
    [[nodiscard]] std::vector<SpawnerAction> handle_enemy_removed();
    void handle_reward_collected() noexcept;
    [[nodiscard]] std::vector<SpawnerAction> stop_spawning();

  private:
    void start_wave() noexcept;
    void handle_spawn_event(std::span<const SpawnerTarget> nearby_targets,
                            const SpawnerTarget &fallback_target, const NextEnemyId &next_enemy_id,
                            std::vector<SpawnerAction> &actions);
    [[nodiscard]] const SpawnerTarget &random_target(std::span<const SpawnerTarget> nearby_targets,
                                                     const SpawnerTarget &fallback_target) noexcept;
    void deduct_spawn_value(int value, std::vector<SpawnerAction> &actions);
    void handle_wave_complete(std::vector<SpawnerAction> &actions);
    [[nodiscard]] std::vector<SpawnerAction> reset_spawner();
    void update_light(float elapsed_seconds, std::vector<SpawnerAction> &actions);
    void set_block_light(bool enable, std::vector<SpawnerAction> &actions);
    void place_reward_block(net::IntVector3 location, std::vector<SpawnerAction> &actions);

    net::IntVector3 location_;
    SpawnerDefinition definition_;
    EnemyInstanceRng *world_random_{};
    EnemyInstanceRng *enemy_random_{};
    int reward_tier_{};
    bool valid_{};
    std::optional<SpawnerRuntimeDefinition> runtime_definition_;
    SpawnerState state_{SpawnerState::listening};
    SpawnerLightState light_state_{};
    float spawn_timer_{};
    float light_timer_{};
    int remaining_spawn_value_{};
    int active_enemy_count_{};
    int remaining_wave_count_{};
    int defeated_spawner_count_at_start_{};
    std::uint8_t last_killer_gid_{};
    int pending_defeated_spawner_count_{};
    int pending_difficulty_{};
};

} // namespace openminerz::world
