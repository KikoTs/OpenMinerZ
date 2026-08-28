#include "openminerz/world/spawner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace openminerz::world
{
namespace
{

constexpr std::array<SpawnerDefinition, 6> definitions{{
    {BlockType::enemy_spawn_off, BlockType::enemy_spawn_on, BlockType::enemy_spawn_dim, 1000, 2.0F,
     4, 5.0F, 1, 2, 20, 7, SpawnerPackageFlags::none},
    {BlockType::enemy_spawn_rare_off, BlockType::enemy_spawn_rare_on,
     BlockType::enemy_spawn_rare_dim, 1200, 0.5F, 3, 4.0F, 1, 3, 50, 10, SpawnerPackageFlags::none},
    {BlockType::alien_spawn_off, BlockType::alien_spawn_on, BlockType::alien_spawn_dim, 500, 4.0F,
     3, 7.0F, 3, 3, 50, 10, SpawnerPackageFlags::alien},
    {BlockType::alien_horde_off, BlockType::alien_horde_on, BlockType::alien_horde_dim, 1200, 2.0F,
     3, 8.0F, 3, 3, 50, 10, SpawnerPackageFlags::alien},
    {BlockType::hell_spawn_off, BlockType::hell_spawn_on, BlockType::hell_spawn_dim, 1500, 4.0F, 3,
     7.0F, 4, 7, 50, 10, SpawnerPackageFlags::hell},
    {BlockType::boss_spawn_off, BlockType::boss_spawn_on, BlockType::boss_spawn_dim, 1000, 4.0F, 1,
     3.0F, 4, 7, 50, 10,
     SpawnerPackageFlags::epic | SpawnerPackageFlags::hell | SpawnerPackageFlags::boss},
}};

constexpr SpawnerSpawnEntry entry(std::uint8_t enemy_type, int weight, int tier,
                                  int minimum_quantity = 1, int maximum_quantity = 1,
                                  int worth = 100,
                                  SpawnerPackageFlags flags = SpawnerPackageFlags::common) noexcept
{
    return {enemy_type,
            weight,
            tier,
            any_minimum_world_level,
            any_maximum_world_level,
            minimum_quantity,
            maximum_quantity,
            worth,
            flags};
}

constexpr std::array<SpawnerSpawnEntry, 19> spawn_entries{{
    entry(18, 100, 1),
    entry(18, 200, 1, 4, 4, 250),
    entry(23, 100, 3),
    entry(26, 100, 2),
    entry(27, 100, 2),
    entry(27, 120, 2, 2, 3, 200),
    entry(28, 100, 2),
    entry(29, 100, 3),
    entry(0, 100, 1),
    entry(0, 150, 1, 3, 3, 200),
    entry(1, 60, 1),
    entry(3, 35, 2),
    entry(3, 35, 2, 1, 1, 250, SpawnerPackageFlags::hell),
    entry(4, 15, 3),
    entry(52, 100, 3, 1, 1, 100, SpawnerPackageFlags::alien),
    entry(52, 20, 3, 2, 2, 200, SpawnerPackageFlags::alien),
    entry(52, 15, 4, 1, 1, 250, SpawnerPackageFlags::epic | SpawnerPackageFlags::alien),
    entry(50, 15, 4, 1, 1, 500, SpawnerPackageFlags::hell),
    entry(51, 10000, 5, 1, 1, 1000, SpawnerPackageFlags::boss),
}};

[[nodiscard]] constexpr bool has_all_flags(SpawnerPackageFlags available,
                                           SpawnerPackageFlags requested) noexcept
{
    const auto left = static_cast<std::uint32_t>(available);
    const auto right = static_cast<std::uint32_t>(requested);
    return requested == SpawnerPackageFlags::none || (left & right) == right;
}

[[nodiscard]] float lerp(float from, float to, float amount) noexcept
{
    return from + (to - from) * amount;
}

[[nodiscard]] std::int32_t to_fixed_point(float value) noexcept
{
    return static_cast<std::int32_t>(1000.0F * value);
}

} // namespace

std::span<const SpawnerDefinition> spawner_definitions() noexcept
{
    return definitions;
}

const SpawnerDefinition *find_spawner_definition(BlockType block) noexcept
{
    const auto found = std::find_if(definitions.begin(), definitions.end(), [block](const auto &d) {
        return d.off_block == block || d.on_block == block || d.dim_block == block;
    });
    return found == definitions.end() ? nullptr : &*found;
}

bool is_spawner_on_block(BlockType block) noexcept
{
    return std::ranges::any_of(definitions, [block](const auto &d) { return d.on_block == block; });
}

bool is_spawner_clickable(BlockType block) noexcept
{
    return block == BlockType::enemy_spawn_off || block == BlockType::enemy_spawn_rare_off ||
           block == BlockType::alien_spawn_off || block == BlockType::hell_spawn_off ||
           block == BlockType::boss_spawn_off;
}

bool is_spawner_preserved_loot(BlockType block) noexcept
{
    return block == BlockType::loot_block || block == BlockType::lucky_loot_block;
}

float spawner_soft_capped_value(float value, float soft_cap, float hard_cap, float factor) noexcept
{
    if (value > soft_cap)
        value += (value - soft_cap) / factor;
    if (value > hard_cap)
        value += (value - hard_cap) / (factor * 10.0F);
    return value;
}

std::optional<SpawnerRuntimeDefinition> apply_spawner_difficulty(
    const SpawnerDefinition &definition, int defeated_spawner_count, int difficulty,
    EnemyInstanceRng &random) noexcept
{
    if (defeated_spawner_count < 0 || difficulty < 0 || difficulty > 3 ||
        definition.wave_count <= 0)
        return std::nullopt;
    float spawn_points = static_cast<float>(definition.spawn_points) + 300.0F;
    spawn_points += spawner_soft_capped_value(static_cast<float>(defeated_spawner_count) * 150.0F,
                                              1000.0F, 2000.0F);
    if (difficulty == 1)
        spawn_points *= 1.5F;
    spawn_points = lerp(spawn_points * 0.8F, spawn_points * 1.2F, random.next01());
    return SpawnerRuntimeDefinition{definition, static_cast<int>(spawn_points),
                                    std::min(definition.maximum_tier + defeated_spawner_count, 10)};
}

std::span<const SpawnerSpawnEntry> spawner_spawn_entries() noexcept
{
    return spawn_entries;
}

bool is_spawner_spawn_candidate(const SpawnerSpawnEntry &candidate, int world_level,
                                int minimum_tier, int maximum_tier, bool rare_loot,
                                int maximum_value, SpawnerPackageFlags permitted_flags) noexcept
{
    const bool flags_match = permitted_flags == SpawnerPackageFlags::none
                                 ? candidate.package_flags == SpawnerPackageFlags::common
                                 : has_all_flags(permitted_flags, candidate.package_flags);
    return (!rare_loot || candidate.weight <= 20) && flags_match &&
           (candidate.minimum_world_level == any_minimum_world_level ||
            world_level >= candidate.minimum_world_level) &&
           (candidate.maximum_world_level == any_maximum_world_level ||
            world_level <= candidate.maximum_world_level) &&
           candidate.tier >= minimum_tier && candidate.tier <= maximum_tier &&
           candidate.worth <= maximum_value;
}

std::optional<SpawnerSpawnChoice> select_spawner_spawn(EnemyInstanceRng &random, int world_level,
                                                       int minimum_tier, int maximum_tier,
                                                       bool rare_loot, int maximum_value,
                                                       SpawnerPackageFlags permitted_flags) noexcept
{
    int total_weight{};
    for (const auto &candidate : spawn_entries)
        if (is_spawner_spawn_candidate(candidate, world_level, minimum_tier, maximum_tier,
                                       rare_loot, maximum_value, permitted_flags))
        {
            if (candidate.weight <= 0 ||
                total_weight > std::numeric_limits<int>::max() - candidate.weight)
                return std::nullopt;
            total_weight += candidate.weight;
        }
    if (total_weight <= 0)
        return std::nullopt;

    int roll = random.next_int(0, total_weight);
    for (const auto &candidate : spawn_entries)
    {
        if (!is_spawner_spawn_candidate(candidate, world_level, minimum_tier, maximum_tier,
                                        rare_loot, maximum_value, permitted_flags))
            continue;
        roll -= candidate.weight;
        // Preserve SpawnBlockType.GetSpawnPackage's inclusive comparison.
        if (roll <= 0)
            return SpawnerSpawnChoice{
                candidate.enemy_type,
                random.next_int(candidate.minimum_quantity, candidate.maximum_quantity),
                candidate.worth};
    }
    return std::nullopt;
}

std::optional<net::EnemyInitPayload> make_spawner_enemy_initialization(
    std::uint8_t enemy_type, float midnight, EnemyInstanceRng &random) noexcept
{
    if (enemy_type >= 56U || !std::isfinite(midnight) || midnight < 0.0F)
        return std::nullopt;
    const bool runs_fast = enemy_type <= 17U;
    float base_slow_speed{};
    float random_slow_speed{};
    float base_fast_speed{};
    float base_run_activation_time{};
    float spawn_animation_speed{};
    if (runs_fast)
    {
        const float progression = static_cast<float>(enemy_type) / 16.0F;
        base_slow_speed = 0.5F + progression * 1.5F;
        random_slow_speed = 0.5F;
        base_fast_speed = 6.5F + progression * 1.5F;
        base_run_activation_time = 4.0F - progression;
        spawn_animation_speed = 3.0F;
    }
    else
    {
        base_slow_speed = 2.0F;
        random_slow_speed = 3.5F;
        base_fast_speed = 6.0F;
        base_run_activation_time = 1000.0F;
        spawn_animation_speed = 1.0F;
    }

    const float slow_speed =
        lerp(base_slow_speed, base_slow_speed + random_slow_speed, random.next01());
    if (!runs_fast)
        return net::EnemyInitPayload{to_fixed_point(slow_speed), 0,
                                     to_fixed_point(spawn_animation_speed), 0, 0};

    const float fast_speed =
        lerp(base_fast_speed - 0.25F, base_fast_speed + 0.25F, random.next01());
    const float run_activation_time =
        lerp(base_run_activation_time, base_run_activation_time * 0.5F, random.next01() * midnight);
    float emerge_speed{};
    if (midnight > 0.8F)
        emerge_speed = spawn_animation_speed;
    else if (spawn_animation_speed == 1.0F)
        emerge_speed = 1.0F;
    else
        emerge_speed = lerp(spawn_animation_speed / 2.0F, spawn_animation_speed,
                            std::min(1.0F, midnight * 2.0F));
    return net::EnemyInitPayload{to_fixed_point(slow_speed), to_fixed_point(fast_speed),
                                 to_fixed_point(emerge_speed), to_fixed_point(run_activation_time),
                                 45'000};
}

SpawnerSimulation::SpawnerSimulation(net::IntVector3 location, SpawnerDefinition definition,
                                     EnemyInstanceRng &world_random, EnemyInstanceRng &enemy_random,
                                     int reward_tier) noexcept
    : location_(location), definition_(definition), world_random_(&world_random),
      enemy_random_(&enemy_random), reward_tier_(reward_tier),
      valid_(reward_tier >= 0 && reward_tier <= 4 && definition.wave_count > 0 &&
             definition.spawn_rate_seconds > 0.0F && definition.wave_intermission_seconds > 0.0F)
{
}

const net::IntVector3 &SpawnerSimulation::location() const noexcept
{
    return location_;
}

const SpawnerDefinition &SpawnerSimulation::definition() const noexcept
{
    return definition_;
}

int SpawnerSimulation::reward_tier() const noexcept
{
    return reward_tier_;
}

bool SpawnerSimulation::valid() const noexcept
{
    return valid_;
}

bool SpawnerSimulation::can_start() const noexcept
{
    return valid_ && state_ == SpawnerState::listening;
}

SpawnerSimulationSnapshot SpawnerSimulation::snapshot() const
{
    return {state_,
            light_state_,
            spawn_timer_,
            light_timer_,
            remaining_spawn_value_,
            active_enemy_count_,
            remaining_wave_count_,
            defeated_spawner_count_at_start_,
            runtime_definition_};
}

std::vector<SpawnerAction> SpawnerSimulation::start(int defeated_spawner_count, int difficulty)
{
    if (!can_start() || defeated_spawner_count < 0 || difficulty < 0 || difficulty > 3)
        return {};
    state_ = SpawnerState::start_triggered;
    pending_defeated_spawner_count_ = defeated_spawner_count;
    pending_difficulty_ = difficulty;
    return {SpawnerClearSurroundingsAction{location_, clear_radius},
            SpawnerTriggerLinkedAction{location_, linked_spawner_radius}, SpawnerActivateAction{}};
}

std::vector<SpawnerAction> SpawnerSimulation::complete_start()
{
    if (state_ != SpawnerState::start_triggered)
        return {};
    std::vector<SpawnerAction> actions;
    set_block_light(true, actions);
    runtime_definition_ = apply_spawner_difficulty(definition_, pending_defeated_spawner_count_,
                                                   pending_difficulty_, *world_random_);
    if (!runtime_definition_.has_value())
    {
        state_ = SpawnerState::reset;
        return reset_spawner();
    }
    defeated_spawner_count_at_start_ = pending_defeated_spawner_count_;
    active_enemy_count_ = 0;
    remaining_wave_count_ = definition_.wave_count;
    start_wave();
    return actions;
}

std::vector<SpawnerAction> SpawnerSimulation::advance(float elapsed_seconds,
                                                      std::span<const SpawnerTarget> nearby_targets,
                                                      const SpawnerTarget &fallback_target,
                                                      const NextEnemyId &next_enemy_id)
{
    if (!valid_ || !std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0F || !next_enemy_id ||
        state_ == SpawnerState::none || state_ == SpawnerState::spawning || spawn_timer_ == 0.0F)
        return {};
    std::vector<SpawnerAction> actions;
    spawn_timer_ -= elapsed_seconds;
    update_light(elapsed_seconds, actions);
    if (spawn_timer_ <= 0.0F)
        handle_spawn_event(nearby_targets, fallback_target, next_enemy_id, actions);
    return actions;
}

std::vector<SpawnerAction> SpawnerSimulation::handle_enemy_defeated(std::uint8_t killer_gid)
{
    last_killer_gid_ = killer_gid;
    --active_enemy_count_;
    if (state_ != SpawnerState::depleted || active_enemy_count_ > 0)
        return {};

    state_ = SpawnerState::completed;
    std::vector<SpawnerAction> actions;
    if (definition_.off_block == BlockType::alien_spawn_off)
        place_reward_block({location_.x, location_.y, location_.z + 1}, actions);
    if (definition_.off_block == BlockType::boss_spawn_off)
    {
        place_reward_block({location_.x + 1, location_.y, location_.z}, actions);
        place_reward_block({location_.x - 1, location_.y, location_.z}, actions);
        if (world_random_->next_int(0, 100) < 50)
            place_reward_block({location_.x, location_.y - 1, location_.z}, actions);
        if (world_random_->next_int(0, 100) < 50)
            place_reward_block({location_.x, location_.y + 1, location_.z}, actions);
    }
    place_reward_block(location_, actions);
    actions.emplace_back(SpawnerCompletedAction{last_killer_gid_});
    return actions;
}

std::vector<SpawnerAction> SpawnerSimulation::handle_enemy_removed()
{
    if (state_ == SpawnerState::listening)
        return {};
    state_ = SpawnerState::reset;
    return reset_spawner();
}

void SpawnerSimulation::handle_reward_collected() noexcept
{
    if (state_ == SpawnerState::completed)
        state_ = SpawnerState::reward_collected;
}

std::vector<SpawnerAction> SpawnerSimulation::stop_spawning()
{
    remaining_spawn_value_ = 0;
    spawn_timer_ = 0.0F;
    state_ = SpawnerState::depleted;
    std::vector<SpawnerAction> actions;
    set_block_light(false, actions);
    return actions;
}

void SpawnerSimulation::start_wave() noexcept
{
    if (!runtime_definition_.has_value())
        return;
    remaining_spawn_value_ += runtime_definition_->spawn_points / definition_.wave_count;
    state_ = SpawnerState::activated;
    spawn_timer_ = definition_.spawn_rate_seconds;
}

void SpawnerSimulation::handle_spawn_event(std::span<const SpawnerTarget> nearby_targets,
                                           const SpawnerTarget &fallback_target,
                                           const NextEnemyId &next_enemy_id,
                                           std::vector<SpawnerAction> &actions)
{
    if (state_ == SpawnerState::wave_intermission)
    {
        start_wave();
        return;
    }
    state_ = SpawnerState::spawning;
    auto choice = select_spawner_spawn(*world_random_, location_.y, 1, 5, false,
                                       remaining_spawn_value_, definition_.package_flags);
    if (!choice.has_value() || choice->count <= 0)
    {
        auto stopped = stop_spawning();
        actions.insert(actions.end(), stopped.begin(), stopped.end());
        return;
    }
    const int per_enemy_value = choice->value / choice->count;
    const int deducted_value = per_enemy_value * choice->count;
    for (int index = 0; index < choice->count; ++index)
    {
        const SpawnerTarget &target = random_target(nearby_targets, fallback_target);
        const auto enemy_id = next_enemy_id(target);
        const auto initialization =
            make_spawner_enemy_initialization(choice->enemy_type, 0.0F, *world_random_);
        if (!enemy_id.has_value() || !initialization.has_value())
        {
            auto stopped = stop_spawning();
            actions.insert(actions.end(), stopped.begin(), stopped.end());
            return;
        }
        const std::int32_t random_seed = enemy_random_->next_sample();
        actions.emplace_back(SpawnerEnemyAction{
            target, net::SpawnEnemyPayload{
                        {static_cast<float>(location_.x), static_cast<float>(location_.y + 1),
                         static_cast<float>(location_.z)},
                        choice->enemy_type,
                        *enemy_id,
                        random_seed,
                        *initialization,
                        {static_cast<float>(location_.x), static_cast<float>(location_.y),
                         static_cast<float>(location_.z)},
                        per_enemy_value,
                        target.gamer_tag}});
        ++active_enemy_count_;
    }
    deduct_spawn_value(deducted_value, actions);
}

const SpawnerTarget &SpawnerSimulation::random_target(std::span<const SpawnerTarget> nearby_targets,
                                                      const SpawnerTarget &fallback_target) noexcept
{
    if (nearby_targets.empty())
        return fallback_target;
    const int index = world_random_->next_int(0, static_cast<int>(nearby_targets.size()));
    return nearby_targets[std::min<std::size_t>(static_cast<std::size_t>(index),
                                                nearby_targets.size() - 1U)];
}

void SpawnerSimulation::deduct_spawn_value(int value, std::vector<SpawnerAction> &actions)
{
    remaining_spawn_value_ -= value;
    if (remaining_spawn_value_ < minimum_required_spawn_value)
        handle_wave_complete(actions);
    else
    {
        state_ = SpawnerState::activated;
        spawn_timer_ = definition_.spawn_rate_seconds;
    }
}

void SpawnerSimulation::handle_wave_complete(std::vector<SpawnerAction> &actions)
{
    --remaining_wave_count_;
    if (remaining_wave_count_ <= 0)
    {
        auto stopped = stop_spawning();
        actions.insert(actions.end(), stopped.begin(), stopped.end());
        return;
    }
    state_ = SpawnerState::wave_intermission;
    spawn_timer_ = definition_.wave_intermission_seconds;
}

std::vector<SpawnerAction> SpawnerSimulation::reset_spawner()
{
    state_ = SpawnerState::listening;
    spawn_timer_ = 0.0F;
    light_state_ = SpawnerLightState::off;
    return {SpawnerBlockAction{location_, definition_.off_block},
            SpawnerBlockAction{location_, definition_.off_block}, SpawnerResetBroadcastAction{},
            SpawnerResetEnemiesAction{location_}};
}

void SpawnerSimulation::update_light(float elapsed_seconds, std::vector<SpawnerAction> &actions)
{
    light_timer_ -= elapsed_seconds;
    if (light_timer_ <= 0.0F)
        set_block_light(light_state_ != SpawnerLightState::on, actions);
}

void SpawnerSimulation::set_block_light(bool enable, std::vector<SpawnerAction> &actions)
{
    if (enable)
    {
        actions.emplace_back(SpawnerBlockAction{location_, definition_.on_block});
        light_state_ = SpawnerLightState::on;
        light_timer_ = light_on_seconds;
    }
    else
    {
        actions.emplace_back(SpawnerBlockAction{location_, definition_.dim_block});
        light_state_ = SpawnerLightState::off;
        light_timer_ = light_off_seconds;
    }
}

void SpawnerSimulation::place_reward_block(net::IntVector3 location,
                                           std::vector<SpawnerAction> &actions)
{
    const int threshold =
        definition_.base_lucky_loot_chance + reward_tier_ * definition_.lucky_loot_chance_per_tier;
    // Preserve the shipped inverted-looking comparison exactly.
    const BlockType reward = threshold < world_random_->next_int(1, 100)
                                 ? BlockType::lucky_loot_block
                                 : BlockType::loot_block;
    actions.emplace_back(SpawnerBlockAction{location, reward});
}

} // namespace openminerz::world
