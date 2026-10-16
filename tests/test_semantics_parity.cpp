// Mixed-session semantics: does an unmodified Castle Miner Z peer and this
// client end up seeing the same thing?
//
// Earlier suites compared this host against this client, with the original
// represented by a transcription. Two ends that share a wrong reading agree
// with each other and both disagree with a real stock client. This file is the
// native half of a differential against the ORIGINAL's own code:
//
//   (no arguments)              expectations pinned from the original, so the
//                               behaviour stays fixed without the game present;
//   --write-facts DIR N         this client's tables and sampled sends, in the
//                               keys SemanticsParityProbe emits from the owned
//                               assemblies (N is the network version, 3 or 4);
//   --cross-feed DIR N          the ORIGINAL's captured payloads through this
//                               client's host authority, decoders and limits.
//
// tools/OriginalClientInterop/SemanticsParityProbe/Test-SemanticsParity.ps1
// runs both halves and compares them.

#include "openminerz/game_mode.hpp"
#include "openminerz/inventory.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/client_gameplay_authority.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/enemy_codec.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/net/pickup_codec.hpp"
#include "openminerz/net/protocol.hpp"
#include "openminerz/net/stock_parity.hpp"
#include "openminerz/retail_parity.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/explosive.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/stock_landing.hpp"
#include "openminerz/world/terrain_window.hpp"
#include "openminerz/world/world.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace openminerz;
using namespace openminerz::net;

int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL " << message << '\n';
}

[[nodiscard]] std::string bits(float value)
{
    return std::format("f:{:08x}", std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] std::vector<std::uint8_t> packet(ProtocolVersion protocol, std::string_view name,
                                               std::span<const std::uint8_t> payload)
{
    const MessageDescriptor *descriptor = message_by_name(protocol, name);
    PacketError error{};
    if (descriptor == nullptr)
        return {};
    const auto encoded = encode_packet(protocol, descriptor->id, payload, error);
    return encoded.value_or(std::vector<std::uint8_t>{});
}

// A broadcast from a non-host guest, as the host receives it.
[[nodiscard]] LegacySteamHostInbound guest_broadcast(std::vector<std::uint8_t> application_packet,
                                                     std::uint8_t sender_gid)
{
    return LegacySteamHostInbound{LegacySteamInboundKind::gameplay,
                                  1000U + sender_gid,
                                  std::move(application_packet),
                                  MessageDeliveryPolicy::reliable_ordered,
                                  1U,
                                  sender_gid,
                                  0U};
}

// The exact quantisation spawn_local_enemy applies before it broadcasts.
[[nodiscard]] std::int32_t milliseconds(float seconds)
{
    const double value =
        std::clamp(static_cast<double>(seconds) * 1'000.0, 0.0,
                   static_cast<double>(std::numeric_limits<std::int32_t>::max()));
    return static_cast<std::int32_t>(value);
}

constexpr std::array<float, 6> midnights{0.0F, 0.25F, 0.5F, 0.79F, 0.85F, 1.0F};
constexpr std::array<std::string_view, 5> init_names{"slow", "fast", "emerge", "run_activation",
                                                     "normal_activation"};

[[nodiscard]] std::array<std::int32_t, 5> spawn_sample(world::EnemyHorde &horde, int type,
                                                       float midnight, std::uint32_t id,
                                                       std::int32_t *seed = nullptr)
{
    horde.enemies().clear();
    if (!horde.spawn(type, {10.5F, 3.0F, -7.25F}, midnight, id, 3U))
        return {-1, -1, -1, -1, -1};
    const world::Enemy &enemy = horde.enemies().back();
    if (seed != nullptr)
        *seed = enemy.random_seed;
    return {milliseconds(enemy.slow_speed), milliseconds(enemy.fast_speed),
            milliseconds(enemy.emerge_speed), milliseconds(enemy.run_activation_time),
            milliseconds(enemy.normal_activation_time)};
}

// System.Random in the call shapes BaseZombie's states use; the same sequence
// SemanticsParityProbe records from the real runtime.
[[nodiscard]] std::string random_sequence(std::int32_t seed)
{
    world::EnemyInstanceRng random{seed};
    std::string text;
    const auto add = [&](const std::string &part) {
        if (!text.empty())
            text += ',';
        text += part;
    };
    for (int round = 0; round < 6; ++round)
    {
        add(std::to_string(random.next_int(0, 4)));
        add(bits(random.next01()));
        add(std::to_string(random.next_int(0, 2)));
        add(std::to_string(random.next_int(0, 5)));
        add(std::to_string(random.next_int(0, 3)));
        add(std::to_string(random.next_int(3, 5)));
        add(std::to_string(random.next_int(1, 3)));
        add(std::to_string(random.next_int(0, 7)));
        add(bits(1.0F + random.next01() * 2.0F));
    }
    return text;
}

[[nodiscard]] std::string_view enemy_class_name(world::EnemyClass value)
{
    switch (value)
    {
    case world::EnemyClass::zombie:
        return "ZombieEnemyType";
    case world::EnemyClass::archer:
        return "ArcherSkeletonEnemyType.ARCHER";
    case world::EnemyClass::skeleton:
        return "SkeletonEnemyType.ZOMBIE";
    case world::EnemyClass::skeleton_sword:
        return "SkeletonEnemyType.SWORD";
    case world::EnemyClass::skeleton_axes:
        return "SkeletonEnemyType.AXES";
    case world::EnemyClass::felguard:
        return "FelguardEnemyType";
    case world::EnemyClass::alien:
        return "AlienEnemyType";
    }
    return "unknown";
}

using Facts = std::map<std::string, std::string>;

// Player feet heights sampled on both sides of the water level the original
// world builder writes (1.5) and of the lava plane (-60 .. -62). The probe's
// "environment" scenario evaluates the original Player at the same heights.
constexpr std::array environment_sample_heights{
    64.0F, 2.0F, 1.5F, 1.0F, 0.5F, 0.0F, -0.5F, -10.0F, -59.0F, -60.0F, -60.5F, -61.0F, -62.0F, -63.5F};

// The column battery the probe's "landing" scenario runs the original
// FindSafeStartLocation and FindTopmostGroundLocation over (Environment.cs:
// LandingColumn, landingGuesses, LandingZ). Index 0..127 from the world floor.
constexpr int landing_columns = 24;
constexpr int landing_z = 3;
constexpr std::array landing_guesses{0, 1, 5, 20, 30, 31, 40, 45, 55, 64, 70, 90, 100, 126, 127};

[[nodiscard]] std::array<world::BlockType, 128> landing_column(int column)
{
    using world::BlockType;
    std::array<BlockType, 128> cells{};
    std::uint32_t state = 0x9E3779B9U ^ (static_cast<std::uint32_t>(column) * 0x85EBCA6BU);
    const std::uint32_t threshold = static_cast<std::uint32_t>(40 + (column % 4) * 40);
    for (int index = 0; index < 128; ++index)
    {
        state = state * 1664525U + 1013904223U;
        BlockType block = BlockType::empty;
        switch (column)
        {
        case 0:
            block = index <= 63 ? BlockType::rock : BlockType::empty;
            break;
        case 1:
            block = index <= 70 && index != 40 ? BlockType::rock : BlockType::empty;
            break;
        case 2:
            block = index <= 80 && index != 30 && index != 31 ? BlockType::rock : BlockType::empty;
            break;
        case 3:
            block = index <= 50 || (index >= 61 && index <= 70) ? BlockType::rock : BlockType::empty;
            break;
        case 4:
            block = BlockType::empty;
            break;
        case 5:
            block = BlockType::rock;
            break;
        case 6:
            block = index == 100 || index == 101 ? BlockType::empty : BlockType::rock;
            break;
        case 7:
            block = index <= 9    ? BlockType::rock
                    : index <= 19 ? BlockType::deep_lava
                    : index == 20 ? BlockType::surface_lava
                                  : BlockType::empty;
            break;
        default:
            block = (state >> 24U) < threshold ? BlockType::rock : BlockType::empty;
            break;
        }
        cells[static_cast<std::size_t>(index)] = block;
    }
    return cells;
}

[[nodiscard]] world::WorldRegion landing_region()
{
    world::WorldRegion region{0, 0, 2, 1};
    for (int column = 0; column < landing_columns; ++column)
    {
        const auto cells = landing_column(column);
        for (int index = 0; index < 128; ++index)
            region.set_block(column, world::world_min_y + index, landing_z,
                             cells[static_cast<std::size_t>(index)]);
    }
    return region;
}

void collect_facts(Facts &facts, ProtocolVersion protocol)
{
    const auto put = [&](std::string key, std::string value) { facts[std::move(key)] = std::move(value); };
    const auto flag = [](bool value) { return std::string{value ? "1" : "0"}; };

    for (const std::int32_t seed : {0, 1, 2, 77, 12345, 987654321, 2147483647, 1073741823})
        put(std::format("random.{}", seed), random_sequence(seed));

    put("enemy.count", std::to_string(world::enemy_type_count));
    for (int type = 0; type < world::enemy_type_count; ++type)
    {
        const world::EnemyStats &stats = world::enemy_stats(type);
        const std::string key = std::format("enemy.{}.", type);
        put(key + "class", std::string{enemy_class_name(stats.enemy_class)});
        put(key + "habitat", std::to_string(static_cast<int>(stats.habitat)));
        put(key + "health", bits(stats.starting_health));
        put(key + "bullet_chance", bits(stats.chance_of_bullet_strike));
        put(key + "spawn_radius", std::to_string(stats.spawn_radius));
        put(key + "attack_speed", bits(stats.attack_animation_speed));
        put(key + "die_speed", bits(stats.die_animation_speed));
        put(key + "hit_speed", bits(stats.hit_animation_speed));
        put(key + "spawn_speed", bits(stats.spawn_animation_speed));
        put(key + "run_fast", flag(stats.has_run_fast));
        put(key + "slow_base", bits(stats.base_slow_speed));
        put(key + "slow_random", bits(stats.random_slow_speed));
        put(key + "fast_base", bits(stats.base_fast_speed));
        put(key + "run_activation", bits(stats.base_run_activation_time));
        put(key + "run_activation_random", bits(stats.random_run_activation_time));
        put(key + "fast_jump", bits(stats.fast_jump_speed));
        put(key + "dig_multiplier", bits(stats.digging_multiplier));
        put(key + "hardest_block", std::to_string(stats.hardest_block_dug));
        for (const int damage : {1, 2, 4, 8, 16})
            for (const bool headshot : {false, true})
                put(std::format("{}damage.{}.{}", key, damage, headshot ? 1 : 0),
                    bits(world::enemy_damage_multiplier(
                        type, static_cast<world::DamageFlags>(damage), headshot)));
    }

    // What this client puts in SpawnEnemyMessage for every type and night band.
    world::EnemyHorde horde;
    for (int type = 0; type < world::enemy_type_count; ++type)
        for (std::size_t band = 0; band < midnights.size(); ++band)
        {
            std::array<std::int32_t, 5> low{};
            std::array<std::int32_t, 5> high{};
            low.fill(std::numeric_limits<std::int32_t>::max());
            high.fill(std::numeric_limits<std::int32_t>::min());
            for (std::uint32_t sample = 0; sample < 1024U; ++sample)
            {
                const auto values = spawn_sample(horde, type, midnights[band], sample);
                for (std::size_t field = 0; field < values.size(); ++field)
                {
                    low[field] = std::min(low[field], values[field]);
                    high[field] = std::max(high[field], values[field]);
                }
            }
            for (std::size_t field = 0; field < init_names.size(); ++field)
            {
                const std::string key = std::format("init.{}.{}.{}", type, band, init_names[field]);
                put(key + ".min", std::to_string(low[field]));
                put(key + ".max", std::to_string(high[field]));
            }
        }

    const InventoryProfile profile = protocol == ProtocolVersion::classic_v3
                                         ? InventoryProfile::classic
                                         : InventoryProfile::modern;
    for (int wire = 0; wire < 512; ++wire)
    {
        InventoryItemId item{};
        if (!inventory_item_from_wire(static_cast<std::int16_t>(wire), protocol, item))
            continue;
        const std::string key = std::format("item.{}.", wire);
        put(key + "max_stack", std::to_string(item_stack_limit(item, profile)));
        put(key + "enemy_damage", bits(item_enemy_damage(item, profile)));
        put(key + "self_damage", bits(item_self_damage_per_use(item, profile)));
        put(key + "cooldown", bits(item_use_cooldown_seconds(item)));
        put(key + "melee", flag(item_is_melee_weapon(item)));
        if (const auto grenade = item_grenade_type(item); grenade.has_value())
            put(key + "grenade_type", std::to_string(static_cast<int>(*grenade)));
        const GunStats *gun = gun_stats(item);
        if (gun == nullptr)
            continue;
        put(key + "gun.clip", std::to_string(gun->clip_capacity));
        put(key + "gun.rounds_per_reload", std::to_string(gun->rounds_per_reload));
        put(key + "gun.automatic", flag(gun->automatic));
        put(key + "gun.inaccuracy", bits(gun->inaccuracy));
        put(key + "gun.recoil", bits(gun->recoil_radians));
        put(key + "gun.drop_compensation", flag(gun->needs_drop_compensation));
        put(key + "gun.velocity", bits(gun->tracer_velocity));
        put(key + "gun.flight_time", bits(gun->tracer_flight_seconds));
        put(key + "gun.reload", bits(gun->reload_seconds));
        std::int16_t ammo{};
        put(key + "gun.ammo",
            inventory_item_to_wire(gun->ammo_item, protocol, ammo) &&
                    gun->ammo_item != InventoryItemId::bare_hands
                ? std::to_string(ammo)
                : std::string{"-1"});
        put(key + "gun.laser", flag(is_laser_gun(item)));
        // InGameHUD.Shoot's routing: which of the three fire messages this
        // weapon's shot travels in, as this client's own writers decide it.
        PayloadError route_error{};
        ShotgunShotPayload blast;
        blast.directions.fill({0.0F, 0.0F, -1.0F});
        blast.item_id = static_cast<std::int16_t>(wire);
        put(key + "gun.shotgun_message",
            flag(encode_shotgun_shot_payload(blast, protocol, route_error).has_value()));
        FireRocketPayload launch;
        launch.direction = {0.0F, 0.0F, -1.0F};
        launch.weapon_type = static_cast<std::int16_t>(wire);
        put(key + "gun.rocket",
            flag(encode_fire_rocket_payload(launch, protocol, route_error).has_value()));
    }

    // BlockTypeEnum.NumberOfBlocks: 45 real block ids in Classic, 94 in Modern.
    const int block_count = protocol == ProtocolVersion::classic_v3 ? 45 : 94;
    put("block.count", std::to_string(block_count));
    for (int block = 0; block < block_count; ++block)
    {
        const auto type = static_cast<world::BlockType>(block);
        const std::string key = std::format("block.{}.", block);
        put(key + "blocks_player", flag(world::blocks_player(type)));
        put(key + "restitution", bits(world::grenade_bounce_restitution(type)));
    }
    // InventoryItem.TimeToDig and CreatesWhenDug for every registered item
    // against every block: the complete mining table of this generation. The
    // probe executes the original methods for the same pairs (it skips blocks
    // whose CanBeDug is false, and only keys both sides wrote are compared).
    for (int wire = 0; wire < 512; ++wire)
    {
        InventoryItemId item{};
        if (!inventory_item_from_wire(static_cast<std::int16_t>(wire), protocol, item))
            continue;
        for (int block = 1; block < block_count; ++block)
        {
            const auto type = static_cast<world::BlockType>(block);
            const float seconds = dig_time_seconds(item, type, profile);
            const std::string key = std::format("{}.{}", wire, block);
            put("dig." + key, std::isinf(seconds) ? std::string{"never"} : bits(seconds));
            if (std::isinf(seconds))
                continue;
            const InventoryItemId dropped = mined_item(type);
            std::int16_t dropped_wire{};
            put("drop." + key,
                dropped != InventoryItemId::bare_hands &&
                        inventory_item_to_wire(dropped, protocol, dropped_wire)
                    ? std::to_string(dropped_wire)
                    : std::string{"-1"});
        }
    }
    // What a default game does about water and lava (probe scenario
    // "environment"). The native side reports its DEFAULT, retail-parity
    // behaviour: every enhancement off, exactly what a session plays with.
    const Settings defaults;
    const bool water_world =
        enhancement_active(defaults, Enhancement::lagoon_water, ParitySession::offline());
    put("environment.water_world", flag(water_world));
    for (std::size_t sample = 0; sample < environment_sample_heights.size(); ++sample)
    {
        const float feet = environment_sample_heights[sample];
        const std::string key = std::format("environment.{}.", sample);
        put(key + "feet_y", bits(feet));
        const float depth = water_world ? world::water_depth(feet) : 0.0F;
        put(key + "in_water", flag(water_world && depth > 0.0F));
        put(key + "underwater", flag(world::is_underwater(water_world, feet)));
        put(key + "water_submersion", bits(std::clamp(depth, 0.0F, 1.0F)));
        put(key + "in_lava", flag(world::lava_submersion(feet) > 0.0F));
        put(key + "under_lava", flag(world::is_under_lava(feet)));
        put(key + "lava_submersion", bits(world::lava_submersion(feet)));
    }

    // Where a relocated player lands (probe scenario "landing").
    const world::WorldRegion landing = landing_region();
    for (int column = 0; column < landing_columns; ++column)
    {
        const float x = static_cast<float>(column);
        const float z = static_cast<float>(landing_z);
        const auto in_column = [&](world::Vec3 position) {
            return position.x == x + 0.5F && position.z == z + 0.5F;
        };
        const world::Vec3 top =
            world::stock_topmost_ground_location(landing, {x + 0.3F, 10.3F, z + 0.7F});
        put(std::format("landing.top.{}", column), bits(top.y));
        put(std::format("landing.top_column.{}", column), flag(in_column(top)));
        for (const int guess : landing_guesses)
        {
            const world::Vec3 safe = world::stock_safe_start_location(
                landing, {x + 0.3F, static_cast<float>(guess - 64) + 0.3F, z + 0.7F});
            put(std::format("landing.safe.{}.{}", column, guess), bits(safe.y));
            put(std::format("landing.safe_column.{}.{}", column, guess), flag(in_column(safe)));
        }
    }

    // The ambient spawner's two starting points (probe scenario "spawn-rules"):
    // EnemyManager.GetMinEnemySpawnTime for every mode and difficulty, out past
    // the 3450 blocks where an unclamped Lerp keeps going, and
    // CalculateMidnight with and without the midnight frenzy.
    {
        struct NamedMode final
        {
            const char *name;
            GameMode mode;
        };
        struct NamedDifficulty final
        {
            const char *name;
            GameDifficulty difficulty;
        };
        constexpr std::array modes{NamedMode{"Endurance", GameMode::endurance},
                                   NamedMode{"Survival", GameMode::survival},
                                   NamedMode{"DragonEndurance", GameMode::dragon_endurance}};
        constexpr std::array difficulties{NamedDifficulty{"EASY", GameDifficulty::easy},
                                          NamedDifficulty{"HARD", GameDifficulty::hard},
                                          NamedDifficulty{"NOENEMIES", GameDifficulty::no_enemies},
                                          NamedDifficulty{"HARDCORE", GameDifficulty::hardcore}};
        constexpr std::array spawn_distances{0.0F, 1000.0F, 3450.0F, 4312.5F, 5000.0F, 8000.0F};
        for (const NamedMode &mode : modes)
            for (const NamedDifficulty &difficulty : difficulties)
                for (const float distance : spawn_distances)
                    put(std::format("spawn.min_time.{}.{}.{}", mode.name, difficulty.name, distance),
                        bits(world::minimum_enemy_spawn_seconds(
                            distance, hard_spawn_branch(mode.mode, difficulty.difficulty))));
        // percent midnight, distance, player height
        constexpr std::array<std::array<float, 3>, 14> midnight_samples{{
            {0.0F, 0.0F, 10.0F},    {0.5F, 0.0F, 10.0F},    {0.6F, 0.0F, 10.0F},
            {0.9F, 0.0F, 10.0F},    {1.0F, 0.0F, 10.0F},    {0.3F, 2500.0F, 10.0F},
            {0.6F, 2500.0F, 10.0F}, {1.0F, 2500.0F, 10.0F}, {0.1F, 5000.0F, 10.0F},
            {0.5F, 5000.0F, 10.0F}, {0.95F, 9000.0F, 10.0F}, {0.0F, 0.0F, -39.9F},
            {0.0F, 0.0F, -40.1F},   {1.0F, 4000.0F, -60.0F},
        }};
        for (std::size_t index = 0; index < midnight_samples.size(); ++index)
        {
            const auto &sample = midnight_samples[index];
            put(std::format("spawn.midnight.{}", index),
                bits(world::enemy_midnight_factor(sample[0], sample[1], sample[2], false)));
            put(std::format("spawn.midnight_fest.{}", index),
                bits(world::enemy_midnight_factor(sample[0], sample[1], sample[2], true)));
        }
    }

    constexpr std::array kinds{world::ExplosiveKind::tnt, world::ExplosiveKind::c4,
                               world::ExplosiveKind::rocket, world::ExplosiveKind::laser,
                               world::ExplosiveKind::he_grenade};
    for (std::size_t index = 0; index < kinds.size(); ++index)
    {
        const world::ExplosiveDefinition definition = world::explosive_definition(kinds[index]);
        const std::string key = std::format("explosive.{}.", index);
        put(key + "destruction", std::to_string(definition.destruction_range));
        put(key + "enemy_range", bits(definition.damage.enemy_range));
        put(key + "damage_range", bits(definition.damage.damage_range));
        put(key + "kill_range", bits(definition.damage.kill_range));
        for (int block = 0; block < block_count; ++block)
        {
            const auto type = static_cast<world::BlockType>(block);
            if (type == world::BlockType::tnt || type == world::BlockType::c4)
                continue; // Chain sources: detonated, not "broken".
            int radius = -1;
            for (int reach = 0; reach <= definition.destruction_range; ++reach)
                if (world::explosive_breaks_block(kinds[index], type, reach, 0, 0))
                    radius = reach;
            put(std::format("{}breaks.{}", key, block), std::to_string(radius));
        }
    }
}

[[nodiscard]] bool write_bytes(const std::filesystem::path &path, std::span<const std::uint8_t> data)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char *>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path)
{
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

int write_facts(const std::filesystem::path &directory, ProtocolVersion protocol)
{
    std::error_code ignored;
    std::filesystem::create_directories(directory / "native", ignored);
    Facts facts;
    collect_facts(facts, protocol);
    std::ofstream output{directory / "native-facts.txt", std::ios::trunc};
    for (const auto &[key, value] : facts)
        output << key << '=' << value << '\n';
    if (!output)
        return 2;

    // This client's own SpawnEnemyMessage payloads, for the original's reader.
    // The probe's field offsets are the Classic layout, so Classic only.
    if (protocol == ProtocolVersion::classic_v3)
    {
        world::EnemyHorde horde;
        for (int type = 0; type < world::enemy_type_count; ++type)
            for (std::size_t band = 0; band < midnights.size(); ++band)
                for (std::uint32_t sample = 0; sample < 8U; ++sample)
                {
                    std::int32_t seed{};
                    const auto values = spawn_sample(horde, type, midnights[band], sample, &seed);
                    SpawnEnemyPayload message;
                    message.spawn_position = {10.5F, 3.0F, -7.25F};
                    message.enemy_type = static_cast<std::uint8_t>(type);
                    message.enemy_id = static_cast<std::int32_t>(sample);
                    message.random_seed = seed;
                    message.initialization = {values[0], values[1], values[2], values[3], values[4]};
                    PayloadError error{};
                    const auto payload = encode_spawn_enemy_payload(message, protocol, error);
                    if (!payload.has_value() ||
                        !write_bytes(directory / "native" /
                                         std::format("SpawnEnemyMessage.{}.{}.{}.bin", type, band,
                                                     sample),
                                     *payload))
                        return 2;
                }
    }
    std::cout << "native semantics facts: " << facts.size() << '\n';
    return 0;
}

// Every payload the ORIGINAL emitted for a scripted event must survive this
// client's host: authority must relay it and the typed reader must accept it.
int cross_feed(const std::filesystem::path &directory, ProtocolVersion protocol)
{
    constexpr std::uint8_t sender_gid = 3U;
    int files = 0;
    ClientGameplayRateLimiter limiter;
    for (const auto &entry : std::filesystem::directory_iterator{directory})
    {
        if (entry.path().extension() != ".bin")
            continue;
        const std::string file = entry.path().filename().string();
        const std::string name = file.substr(0, file.find('.'));
        const std::vector<std::uint8_t> payload = read_bytes(entry.path());
        ++files;
        PayloadError error{};
        const auto framed = packet(protocol, name, payload);
        expect(!framed.empty(), std::format("{} could not be framed for this protocol", file));
        // ConsumePickupMessage is the game host's answer; a guest never sends it.
        if (name != "ConsumePickupMessage")
        {
            const ClientGameplayDecision decision =
                assess_client_gameplay_authority(protocol, guest_broadcast(framed, sender_gid));
            expect(decision.accepted() && decision.should_relay(),
                   std::format("{}: the original's own traffic was refused by host authority ({})",
                               file, client_gameplay_authority_error_label(decision.error)));
        }
        if (name == "SpawnEnemyMessage")
        {
            SpawnEnemyPayload message;
            expect(decode_spawn_enemy_payload(payload, protocol, message, error) &&
                       world::enemy_type_from_wire(message.enemy_type,
                                                   protocol == ProtocolVersion::modern_v4)
                           .has_value() &&
                       message.random_seed >= 0 &&
                       message.initialization.slow_speed_milliseconds <= 100'000 &&
                       message.initialization.fast_speed_milliseconds <= 100'000 &&
                       message.initialization.emerge_speed_milliseconds <= 100'000 &&
                       message.initialization.run_activation_milliseconds <= 3'600'000 &&
                       message.initialization.normal_activation_milliseconds <= 3'600'000,
                   std::format("{}: an original enemy spawn fails this client's admission", file));
        }
        else if (name == "KillEnemyMessage")
        {
            KillEnemyPayload message;
            expect(decode_kill_enemy_payload(payload, protocol, message, error) &&
                       message.killer_id != sender_gid,
                   std::format("{}: the original's cross-credited kill did not decode", file));
        }
        else if (name == "GrenadeMessage")
        {
            GrenadePayload message;
            expect(decode_grenade_payload(payload, protocol, message, error) &&
                       stock_grenade_fuse_valid(message.seconds_left),
                   std::format("{}: an original grenade throw is refused (fuse {})", file,
                               message.seconds_left));
        }
        else if (name == "CreatePickupMessage")
        {
            CreatePickupPayload message;
            expect(decode_create_pickup_payload(payload, protocol, message, error) &&
                       stock_pickup_spawn_valid(message.spawn_position, message.spawn_vector),
                   std::format("{}: an original pickup is refused", file));
        }
        else if (name == "RemoveBlocksMessage")
        {
            RemoveBlocksPayload message;
            expect(decode_remove_blocks_payload(payload, message, error),
                   std::format("{}: an original block removal did not decode", file));
        }
        else if (name == "GunshotMessage")
        {
            GunshotPayload message;
            expect(decode_gunshot_payload(payload, protocol, message, error),
                   std::format("{}: an original gunshot did not decode", file));
        }
        else if (name == "ShotgunShotMessage")
        {
            ShotgunShotPayload message;
            expect(decode_shotgun_shot_payload(payload, protocol, message, error),
                   std::format("{}: an original shotgun blast did not decode", file));
        }
        else if (name == "FireRocketMessage")
        {
            FireRocketPayload message;
            expect(decode_fire_rocket_payload(payload, protocol, message, error),
                   std::format("{}: an original rocket launch did not decode", file));
        }
        else if (name == "DetonateGrenadeMessage")
        {
            DetonateGrenadePayload message;
            expect(decode_detonate_grenade_payload(payload, protocol, message, error),
                   std::format("{}: an original grenade detonation did not decode", file));
        }
        else if (name == "DetonateRocketMessage")
        {
            DetonateRocketPayload message;
            expect(decode_detonate_rocket_payload(payload, protocol, message, error),
                   std::format("{}: an original rocket detonation did not decode", file));
        }
        else if (name == "DetonateExplosiveMessage")
        {
            DetonateExplosivePayload message;
            expect(decode_detonate_explosive_payload(payload, protocol, message, error),
                   std::format("{}: an original chained detonation did not decode", file));
        }
        else if (name == "ConsumePickupMessage")
        {
            ConsumePickupPayload message;
            expect(decode_consume_pickup_payload(payload, protocol, message, error),
                   std::format("{}: the original host's pickup grant did not decode", file));
        }
    }
    expect(files > 0, "no original fixtures were found to cross-feed");
    std::cout << "original payloads cross-fed: " << files << ", failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------ pinned

void enemy_random_draws_match_the_runtime()
{
    // Recorded from the .NET Framework System.Random the game runs on, by
    // SemanticsParityProbe, in the order BaseZombie's states draw: Next(4),
    // NextDouble, Next(2), Next(5), Next(0,3), Next(3,5), Next(1,3), Next(7),
    // 1 + NextDouble * 2. SpawnEnemyMessage.RandomSeed seeds one per enemy and
    // every peer draws its animation and attack choices from it.
    expect(random_sequence(77) ==
               "3,f:3ed234c7,1,4,2,4,1,0,f:40007a69,2,f:3ca52029,1,2,1,4,1,1,f:401e1e28,"
               "3,f:3e0478fa,0,3,0,4,1,0,f:3fa2cbb6,1,f:3f50b201,0,3,2,3,2,3,f:3f999900,"
               "3,f:3f5b3dea,1,3,1,3,1,6,f:40008bdc,2,f:3f08c189,1,3,1,3,1,4,f:3facc8e3",
           "the per-enemy generator must reproduce System.Random(77) draw for draw");
    expect(random_sequence(12345) ==
               "0,f:3d8fafca,1,2,2,4,1,5,f:3fc29d8a,2,f:3e6bcce0,0,1,0,3,2,1,f:40318a83,"
               "3,f:3eb7bd29,0,2,1,4,2,4,f:401dc273,2,f:3ecae16b,0,3,0,4,1,1,f:4022a8e2,"
               "0,f:3ef7c354,0,2,2,3,1,1,f:40114f97,1,f:3ebd3382,1,0,2,4,2,3,f:3fff2786",
           "the per-enemy generator must reproduce System.Random(12345) draw for draw");
    expect(random_sequence(2147483647) ==
               "2,f:3f513c3c,1,2,0,4,2,3,f:403d205a,1,f:3e9574be,0,3,1,4,1,6,f:403f6788,"
               "2,f:3ea1122c,1,4,2,3,2,3,f:40378dec,2,f:3f0bfc19,0,0,1,3,2,4,f:4021a8ca,"
               "0,f:3ec31306,0,4,1,4,1,1,f:40341bc9,3,f:3eaca048,0,0,0,3,2,4,f:3ffceb7a",
           "the per-enemy generator must reproduce System.Random(int.MaxValue) draw for draw");
}

void kill_credit_is_not_an_identity_claim()
{
    for (const ProtocolVersion protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
    {
        // The exact payload the original emits when peer 3's copy of gamer 5's
        // enemy 42 dies to gamer 7's TNT: EnemyID, TargetID, WeaponID, KillerID.
        PayloadError error{};
        const auto payload = encode_kill_enemy_payload({42, 5, 1, 7U}, protocol, error);
        expect(payload.has_value(), "the observed-kill payload must encode");
        const auto decision = assess_client_gameplay_authority(
            protocol, guest_broadcast(packet(protocol, "KillEnemyMessage", *payload), 3U));
        expect(decision.accepted() && decision.should_relay(),
               "a kill observed by one peer and credited to another must be relayed");
    }
    constexpr std::array<std::uint8_t, 11> original{0x2A, 0, 0, 0, 5, 0, 0, 0, 79, 0, 7};
    KillEnemyPayload decoded;
    PayloadError error{};
    expect(decode_kill_enemy_payload(original, ProtocolVersion::classic_v3, decoded, error) &&
               decoded.enemy_id == 42 && decoded.target_id == 5 && decoded.weapon_id == 79 &&
               decoded.killer_id == 7U,
           "the original's captured cross-credited kill must decode field for field");
}

void a_stock_host_relays_what_a_stock_guest_sends()
{
    // GrenadeMessage.SecondsLeft of a fully cooked grenade at 30, 60 and 144
    // frames a second, as the original's cook loop leaves it.
    for (const std::uint32_t pattern : {0x3F7777B8U, 0x3F7BBCC8U, 0x3F7E3A90U})
        expect(stock_grenade_fuse_valid(std::bit_cast<float>(pattern)),
               "a fully cooked stock grenade has less than one second left and must pass");
    expect(stock_grenade_fuse_valid(5.0F) && stock_grenade_fuse_valid(0.5F) &&
               !stock_grenade_fuse_valid(5.5F) && !stock_grenade_fuse_valid(-0.1F) &&
               !stock_grenade_fuse_valid(std::numeric_limits<float>::quiet_NaN()),
           "the fuse bound must be the original's 0..5 second range and reject garbage");

    // BaseZombie.CreatePickup for a kill 40 units away: (40, 7, -12), launched
    // by CreateUpwardPickup at 1.5 units a second.
    expect(stock_pickup_spawn_valid({40.0F, 7.0F, -12.0F}, {0.6F, 1.2F, -0.65F}),
           "enemy loot far from its killer is ordinary stock traffic");
    expect(stock_pickup_spawn_valid({300.0F, 60.0F, -250.0F}, {0.0F, 3.0F, 0.0F}) &&
               stock_pickup_spawn_valid({0.0F, 0.0F, 0.0F}, {2.4F, 0.4F, 3.2F}),
           "dragon loot, crate contents and a thrown stack must all be accepted");
    expect(!stock_pickup_spawn_valid({0.0F, 4'000.0F, 0.0F}, {}) &&
               !stock_pickup_spawn_valid({0.0F, 0.0F, 0.0F}, {0.0F, 500.0F, 0.0F}) &&
               !stock_pickup_spawn_valid({std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F}, {}),
           "an out-of-world or absurdly fast pickup must still be refused");

    expect(stock_host_grants_pickup(true, false) && !stock_host_grants_pickup(true, true) &&
               !stock_host_grants_pickup(false, false),
           "the host grants the first request for a pickup it holds, with no reach test");

    // Explosive.FindBlocksToRemove always ends in RemoveBlocksMessage: zero
    // cells in open air, and a whole chain reaction in one message.
    PayloadError error{};
    const auto empty = encode_remove_blocks_payload({}, error);
    expect(empty.has_value() &&
               assess_client_gameplay_authority(
                   ProtocolVersion::classic_v3,
                   guest_broadcast(packet(ProtocolVersion::classic_v3, "RemoveBlocksMessage", *empty), 3U))
                   .should_relay(),
           "an empty block removal is ordinary stock traffic and must relay");
    std::vector<std::uint8_t> large;
    constexpr std::int32_t cells = 12'000;
    for (int shift = 0; shift < 32; shift += 8)
        large.push_back(static_cast<std::uint8_t>((cells >> shift) & 0xFF));
    for (std::int32_t cell = 0; cell < cells; ++cell)
        for (const std::int32_t value : {cell % 24, (cell / 24) % 20 - 10, cell / 480})
            for (int shift = 0; shift < 32; shift += 8)
                large.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    large.push_back(0U);
    RemoveBlocksPayload removal;
    expect(decode_remove_blocks_payload(large, removal, error) && removal.blocks.size() == 12'000U,
           "one stock chain reaction's RemoveBlocksMessage must not be refused for its size");
}

void bursts_the_original_produces_are_never_clipped()
{
    ClientGameplayRateLimiter limiter;
    constexpr PeerId peer = 77U;
    std::size_t clipped{};
    const auto offer = [&](std::string_view message, std::size_t count, std::uint64_t at) {
        for (std::size_t index = 0; index < count; ++index)
            clipped += limiter.allow(peer, message, at) ? 0U : 1U;
    };
    // Two grenades and a rocket inside one second.
    offer("GrenadeMessage", 2U, 100U);
    offer("FireRocketMessage", 1U, 300U);
    offer("DetonateGrenadeMessage", 2U, 700U);
    offer("DetonateRocketMessage", 1U, 800U);
    expect(clipped == 0U, "three explosions in a second must all reach the other players");
    // One C4 in a wall of TNT and crates, as executed in the original: the
    // chain, 124 crates, their contents, the dependents, and one removal.
    clipped = 0U;
    offer("DetonateExplosiveMessage", 342U, 2'000U);
    offer("DestroyCrateMessage", 124U, 2'000U);
    offer("CreatePickupMessage", 1'200U, 2'000U);
    offer("RemoveBlocksMessage", 1U, 2'000U);
    expect(clipped == 0U, "one large chain reaction must relay in full");
    // Midnight: the whole local horde is enraged, then the player dies and
    // every one of them gives up; then a death pile is walked over.
    clipped = 0U;
    offer("SpeedUpEnemyMessage", 100U, 4'000U);
    offer("EnemyGiveUpMessage", 50U, 4'300U);
    offer("KillEnemyMessage", 50U, 4'600U);
    offer("RequestPickupMessage", 96U, 4'900U);
    expect(clipped == 0U, "a horde's speed-ups, give-ups and kills and a pile of pickups must pass");
    // A flood is still a flood.
    clipped = 0U;
    offer("DetonateGrenadeMessage", 20'000U, 6'000U);
    expect(clipped > 10'000U, "a hostile detonation flood must still be bounded");
}

void enemy_type_ids_are_translated_per_generation()
{
    // EnemyTypeEnum from both owned assemblies: Classic ALIEN = 51; Modern
    // HELL_LORD = 51 and ALIEN = 52, with Types[] holding 53 entries.
    for (std::uint8_t wire = 0; wire <= 51U; ++wire)
        expect(world::enemy_type_from_wire(wire, false) == std::optional<int>{wire} &&
                   world::enemy_type_to_wire(wire, false) == wire,
               "Classic enemy type ids are this client's ids");
    expect(!world::enemy_type_from_wire(52U, false).has_value(),
           "Classic has no enemy type 52");
    expect(world::enemy_type_from_wire(52U, true) == std::optional<int>{world::enemy_alien} &&
               world::enemy_type_to_wire(world::enemy_alien, true) == 52U,
           "a Modern Alien is wire type 52 in both directions");
    expect(world::enemy_type_from_wire(51U, true) == std::optional<int>{world::enemy_felguard},
           "a Modern Hell Lord must never be simulated as an Alien");
    expect(world::enemy_type_from_wire(50U, true) == std::optional<int>{world::enemy_felguard} &&
               world::enemy_type_to_wire(world::enemy_felguard, true) == 50U &&
               !world::enemy_type_from_wire(53U, true).has_value(),
           "Modern Felguards keep id 50 and unregistered types are refused");
}

void item_tables_match_the_original_classic_registration()
{
    // Values the differential found wrong, pinned from InventoryItem.Initalize
    // executed in the owned Classic assembly.
    expect(item_stack_limit(InventoryItemId::slime, InventoryProfile::classic) == 64U,
           "Slime is a block item and stacks to 64 in Classic");
    expect(item_enemy_damage(static_cast<InventoryItemId>(14), InventoryProfile::classic) ==
                   0.025F &&
               item_enemy_damage(static_cast<InventoryItemId>(76), InventoryProfile::classic) ==
                   0.025F,
           "a held torch or door deals 0.025 enemy damage, not the 0.1 default");
    const GunStats *bolt = gun_stats(static_cast<InventoryItemId>(53));
    const GunStats *smg = gun_stats(static_cast<InventoryItemId>(54));
    expect(bolt != nullptr && smg != nullptr && bolt->fire_cooldown_seconds == 0.003F &&
               smg->fire_cooldown_seconds == 0.064F,
           "fire rates are the millisecond-rounded TimeSpans the original stores");
    expect(world::grenade_bounce_restitution(world::BlockType::torch_neg_y) == 0.4F,
           "TorchNEGY carries the soft restitution the original authored for it");
}

// CastleMinerZGame._processRestartLevelMessage has no sender and no mode check
// in either shipped generation, and a stock host relays the message from any
// gamer. Online Endurance restarts flow through it.
void a_guests_level_restart_is_relayed_like_a_stock_host_relays_it()
{
    for (const ProtocolVersion protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
    {
        const auto restart = packet(protocol, "RestartLevelMessage", {});
        expect(!restart.empty(), "fixture: RestartLevelMessage must frame with no payload");
        const ClientGameplayDecision decision =
            assess_client_gameplay_authority(protocol, guest_broadcast(restart, 3U));
        expect(decision.accepted() && decision.action == ClientGameplayAction::relay &&
                   decision.should_relay(),
               "a guest's RestartLevelMessage must be relayed to every peer, not refused");
        // Still a broadcast only: the original never addresses it to one gamer.
        LegacySteamHostInbound direct = guest_broadcast(restart, 3U);
        direct.recipient_gid = 2U;
        expect(!assess_client_gameplay_authority(protocol, direct).accepted(),
               "a privately addressed restart is not something the original sends");
    }
    // What remains is a hostile-peer ceiling, not a rule: one restart a second
    // per peer. A restart is idempotent, so a refused repeat changes nothing.
    ClientGameplayRateLimiter limiter;
    expect(limiter.allow(1003U, "RestartLevelMessage", 0U) &&
               !limiter.allow(1003U, "RestartLevelMessage", 400U) &&
               limiter.allow(1004U, "RestartLevelMessage", 400U) &&
               limiter.allow(1003U, "RestartLevelMessage", 1000U),
           "restarts are limited per peer per second and never blocked outright");
}

// MakeAboveGround after a relocation: BlockTerrain.FindSafeStartLocation and
// FindTopmostGroundLocation. The probe executes the originals over the same
// columns; these are the cases worth reading.
void relocations_land_where_the_original_puts_them()
{
    const world::WorldRegion region = landing_region();
    const auto safe = [&](int column, int guess) {
        return world::stock_safe_start_location(
            region, {static_cast<float>(column) + 0.3F, static_cast<float>(guess - 64) + 0.3F,
                     static_cast<float>(landing_z) + 0.7F});
    };
    const auto top = [&](int column) {
        return world::stock_topmost_ground_location(
            region, {static_cast<float>(column) + 0.3F, 10.3F,
                     static_cast<float>(landing_z) + 0.7F});
    };
    // The feet height the original returns for standing in cell `index`.
    const auto at = [](int index) { return static_cast<float>(index - 64) + 0.1F; };

    for (int column = 0; column < landing_columns; ++column)
    {
        const float x = static_cast<float>(column) + 0.5F;
        const float z = static_cast<float>(landing_z) + 0.5F;
        expect(top(column).x == x && top(column).z == z,
               "FindTopmostGroundLocation must stay in the target's own column");
        for (const int guess : landing_guesses)
            expect(safe(column, guess).x == x && safe(column, guess).z == z,
                   "FindSafeStartLocation must stay in the target's own column");
    }
    // Flat ground up to index 63.
    expect(top(0).y == at(64) && safe(0, 90).y == at(64) && safe(0, 20).y == at(64),
           "from above, the player drops onto the ground; from inside it, climbs out");
    // A gap one cell high is not somewhere a body fits.
    expect(safe(1, 40).y == at(71) && top(1).y == at(71),
           "a one-high gap is skipped for the next two-high one");
    // A two-high pocket (30..31) under 50 blocks of rock.
    expect(safe(2, 31).y == at(30) && safe(2, 30).y == at(30) && safe(2, 45).y == at(81) &&
               top(2).y == at(81),
           "a player already in a cave stays in it; one inside the rock goes to the nearer gap");
    // An overhang: rock to 50, air 51..60, rock 61..70.
    expect(safe(3, 55).y == at(51) && safe(3, 90).y == at(71) && safe(3, 64).y == at(71) &&
               top(3).y == at(71),
           "under an overhang the player lands under it; FindTopmostGroundLocation goes on top");
    // Lava is not avoided: surface lava (index 20) is ground, deep lava is air.
    expect(safe(7, 64).y == at(21) && top(7).y == at(21) && safe(7, 5).y == at(10),
           "the original lands a player on surface lava and inside deep lava without a second look");
    // No blocks at all, and no gap at all.
    expect(safe(4, 64).y == at(1) && top(4).y == at(1) && safe(4, 0).y == at(0),
           "an empty column leaves the player one cell above the world floor");
    expect(top(5).y == at(128) && safe(5, 40).y == at(-1),
           "a column solid to the top: on top of the world, or the original's -1 fall-through");
    // A column solid to the top with one gap (100..101). From below, the climb
    // reaches the gap. From above, the climb stops on the solid top cell, the
    // upward scan finds nothing, and the gap below is then compared against a
    // "nearest so far" of -1 and loses: the original returns the -1 cell.
    // Executed against the shipped assembly, not inferred.
    expect(safe(6, 20).y == at(100) && safe(6, 126).y == at(-1),
           "the only gap in a solid column is found from below, and missed from above as the "
           "original misses it");
}

// CastleMinerZGame.SendNetworkUpdates: one PlayerUpdate every (players + 1)
// frames, every frame while UsingTool or Reloading.
void player_updates_follow_the_original_cadence()
{
    const auto sends_in = [](std::size_t gamers, int frames, bool using_tool, bool reloading,
                             std::size_t local_index = 0U) {
        StockPlayerUpdateCadence cadence;
        int sends = 0;
        for (int frame = 0; frame < frames; ++frame)
            sends += cadence.frame(gamers, using_tool, reloading, local_index) ? 1 : 0;
        return sends;
    };
    // 60 frames is one second of the original's vsynced Update loop.
    expect(sends_in(1U, 60, false, false) == 30 && sends_in(2U, 60, false, false) == 20 &&
               sends_in(3U, 60, false, false) == 15 && sends_in(4U, 60, false, false) == 12 &&
               sends_in(8U, 63, false, false) == 7 && sends_in(16U, 68, false, false) == 4,
           "an idle player must send one update every (players + 1) frames");
    expect(sends_in(2U, 60, true, false) == 60 && sends_in(8U, 60, false, true) == 60 &&
               sends_in(16U, 60, true, true) == 60,
           "a player using a tool or reloading must send every frame, whatever the session size");
    for (std::size_t gamers = 1U; gamers <= 16U; ++gamers)
        for (std::size_t index = 0U; index < gamers; ++index)
            expect(sends_in(gamers, static_cast<int>(gamers + 1U) * 6, false, false, index) == 6,
                   "the gamer's position in the roster changes only the phase, never the rate");
    // The exact slot: the counter is incremented first, wraps to zero after
    // AllGamers.Count, and the local gamer at index zero owns the wrap frame.
    StockPlayerUpdateCadence cadence;
    std::string pattern;
    for (int frame = 0; frame < 9; ++frame)
        pattern += cadence.frame(2U, false, false) ? '1' : '0';
    expect(pattern == "001001001", "two players: the third frame of every three sends");
    // A change of session size takes effect at once, as the original's bound does.
    StockPlayerUpdateCadence shrinking;
    for (int frame = 0; frame < 7; ++frame)
        static_cast<void>(shrinking.frame(8U, false, false));
    expect(shrinking.frame(1U, false, false),
           "when players leave, a counter beyond the new roster wraps on the next frame");
    // A use pulse of one frame is still sent, and does not disturb the slot.
    StockPlayerUpdateCadence pulse;
    std::string pulsed;
    for (int frame = 0; frame < 6; ++frame)
        pulsed += pulse.frame(2U, frame == 0, false) ? '1' : '0';
    expect(pulsed == "101001", "a one-frame tool use is sent in its own frame");
}

// BlockTerrain's 24-chunk window and BaseZombie's RegionIsLoaded cull.
void enemies_are_culled_by_the_local_terrain_window()
{
    using world::StockTerrainWindow;
    StockTerrainWindow window;
    // WorldInfo.DefaultStartLocation (8, 128, -8): SetWorldMin(center - 192)
    // truncates toward zero, so X starts at -176 and Z at -192.
    window.center_on(8.0F, -8.0F);
    expect(window.minimum_x() == -176 && window.minimum_z() == -192,
           "SetWorldMin must snap with truncating integer division");
    expect(window.contains(8.0F, -8.0F) && window.contains(207.9F, 191.9F) &&
               !window.contains(208.0F, 0.0F) && !window.contains(0.0F, 192.0F),
           "the window must be 384 blocks from its minimum corner");
    // GetChunkVectorIndex truncates too: fifteen blocks below the minimum edge
    // still land in chunk 0 and count as loaded.
    expect(window.contains(-176.0F, 0.0F) && window.contains(-191.0F, 0.0F) &&
               !window.contains(-192.0F, 0.0F) && window.contains(0.0F, -207.0F) &&
               !window.contains(0.0F, -208.0F),
           "RegionIsLoaded's truncating chunk index extends the low edge by fifteen blocks");
    // TryShiftTerrain keeps the eye at chunk 11 or 12: one chunk of slack when
    // walking negative, none when walking positive.
    StockTerrainWindow walking;
    walking.center_on(0.0F, 0.0F);
    expect(walking.minimum_x() == -192, "fixture: centred on the origin");
    walking.follow(15.9F, 0.0F);
    expect(walking.minimum_x() == -192, "inside the centre chunk nothing scrolls");
    walking.follow(16.0F, 0.0F);
    expect(walking.minimum_x() == -176, "walking positive scrolls as soon as chunk 13 is entered");
    walking.follow(0.0F, 0.0F);
    expect(walking.minimum_x() == -176, "one chunk back (index 11) is tolerated without a scroll");
    walking.follow(-0.1F, 0.0F);
    expect(walking.minimum_x() == -192, "two chunks back scrolls by one, leaving the eye at 11");
    walking.follow(5000.0F, -7000.0F);
    expect(walking.contains(5000.0F, -7000.0F) && !walking.contains(0.0F, 0.0F),
           "an eye outside the window altogether re-centres it (a teleport)");

    // The cull itself, in a session: a zombie chasing a remote player who is
    // far from that zombie stays alive on this peer as long as the zombie is
    // on terrain this peer holds. The former flat 100-block radius removed it.
    world::WorldRegion region{-2, -2, 4, 4};
    for (int z = region.minimum_z(); z <= region.maximum_z(); ++z)
        for (int x = region.minimum_x(); x <= region.maximum_x(); ++x)
            region.set_block(x, 0, z, world::BlockType::rock);
    world::PlayerState local;
    local.position = {0.5F, 1.001F, 0.5F};
    world::PlayerState remote;
    remote.position = {300.0F, 1.001F, 0.5F};
    const world::EnemyHorde::TargetLookup lookup =
        [&](std::uint8_t gid) -> std::optional<world::PlayerState> {
        return gid == 5U ? std::optional<world::PlayerState>{remote} : std::nullopt;
    };
    const world::EnemyInitialization initialization{1.0F, 3.0F, 1.0F, 20.0F, 45.0F};
    world::EnemyHorde horde;
    expect(horde.spawn_networked(0, {10.5F, 1.001F, 10.5F}, initialization, 1U, 7, 5U) &&
               horde.spawn_networked(0, {40.5F, 1.001F, 0.5F}, initialization, 2U, 7, 5U),
           "fixture: two zombies announced for a remote player");
    for (int tick = 0; tick < 30; ++tick)
        static_cast<void>(horde.update(region, local, 0U, lookup, 1.0F / 60.0F));
    const auto alive = [&](std::uint32_t id) {
        return std::any_of(horde.enemies().begin(), horde.enemies().end(),
                           [id](const world::Enemy &enemy) { return enemy.id == id; });
    };
    expect(alive(1U), "an enemy on loaded terrain must survive however far its target is");
    expect(!alive(2U), "an enemy outside the terrain this peer holds must be dropped locally");

    // Past the original's own window the enemy goes even if terrain were held.
    world::EnemyHorde far;
    world::PlayerState mover = local;
    expect(far.spawn_networked(0, {10.5F, 1.001F, 10.5F}, initialization, 3U, 7, 0U),
           "fixture: a zombie chasing the local player");
    static_cast<void>(far.update(region, mover, 0U, {}, 1.0F / 60.0F));
    expect(far.enemies().size() == 1U, "fixture: alive while the player is near");
    mover.position = {420.5F, 1.001F, 0.5F};
    static_cast<void>(far.update(region, mover, 0U, {}, 1.0F / 60.0F));
    expect(far.enemies().empty(),
           "when the local player's window scrolls away, the enemy is dropped on this peer");
}

// The resident window is four times the columns it used to be, so a whole one
// (a teleport, the first load) is generated across worker threads and adopted
// without an empty placeholder per column. Every peer generates the terrain
// for itself from the seed, so that must be the same blocks as one column at a
// time.
void a_streamed_window_is_the_same_blocks_as_a_generated_one()
{
    const world::WorldGenerator generator{20240917};
    const world::WorldRegion whole = generator.generate_region(-5, -4, 10, 9);
    const world::WorldRegion seed = generator.generate_region(-1, -1, 3, 3);
    // 81 of 90 columns are missing: the fanned-out path.
    const world::WorldRegion streamed = generator.stream_region(seed, -5, -4, 10, 9);
    expect(streamed.origin_chunk_x() == -5 && streamed.origin_chunk_z() == -4 &&
               streamed.chunks_x() == 10 && streamed.chunks_z() == 9 &&
               streamed.hash() == whole.hash(),
           "a window streamed across threads must hold exactly the generated blocks");
    // One row: the walking step, generated on the calling thread.
    const world::WorldRegion scrolled = generator.stream_region(streamed, -4, -4);
    const world::WorldRegion expected = generator.generate_region(-4, -4, 10, 9);
    expect(scrolled.hash() == expected.hash() && scrolled.chunks()[0].chunk_x() == -4 &&
               scrolled.chunks().back().chunk_x() == 5 && scrolled.chunks().back().chunk_z() == 4,
           "a one-row scroll must keep every reused column and generate the new row");
    bool rejected{};
    try
    {
        std::vector<world::Chunk> wrong_order{world::Chunk{1, 0}, world::Chunk{0, 0}};
        const world::WorldRegion bad{0, 0, 2, 1, std::move(wrong_order)};
        static_cast<void>(bad);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    expect(rejected, "adopting columns that are not the stated rectangle must be refused");
}

// The register's "Loaded terrain window" row. The shipped BlockTerrain is 24 x
// 24 chunks whatever DrawDistance is (the setting only limits which chunks are
// DRAWN), so the columns this client keeps resident must not depend on its
// View distance either: an enemy another player is fighting 150 blocks away is
// alive in a stock player's game and has to be alive here.
void the_simulated_area_does_not_depend_on_view_distance()
{
    using world::StockTerrainWindow;
    StockTerrainWindow window;
    window.center_on(8.0F, -8.0F); // WorldInfo.DefaultStartLocation
    const world::ChunkWindow reference = world::resident_chunk_window(window, 16);
    expect(reference.span == 24 && reference.origin_x == -11 && reference.origin_z == -12,
           "the resident window is the original's 24 x 24 chunks at its snapped minimum");

    world::WorldRegion resident{reference.origin_x, reference.origin_z, reference.span,
                                reference.span};
    for (int z = resident.minimum_z(); z <= resident.maximum_z(); ++z)
        for (int x = resident.minimum_x(); x <= resident.maximum_x(); ++x)
            resident.set_block(x, 0, z, world::BlockType::rock);
    world::PlayerState local;
    local.position = {8.0F, 1.001F, -8.0F};
    // Another player and the zombie chasing them, 150 blocks east of us.
    world::PlayerState remote;
    remote.position = {160.5F, 1.001F, -8.0F};
    const world::Vec3 far_zombie{158.5F, 1.001F, -8.0F};
    const world::EnemyHorde::TargetLookup lookup =
        [&](std::uint8_t gid) -> std::optional<world::PlayerState> {
        return gid == 5U ? std::optional<world::PlayerState>{remote} : std::nullopt;
    };
    const world::EnemyInitialization initialization{1.0F, 3.0F, 1.0F, 20.0F, 45.0F};
    const auto survives = [&](const world::WorldRegion &region) {
        world::EnemyHorde horde;
        horde.set_terrain_window(window);
        if (!horde.spawn_networked(0, far_zombie, initialization, 1U, 7, 5U))
            return false;
        for (int tick = 0; tick < 30; ++tick)
            static_cast<void>(horde.update(region, local, 0U, lookup, 1.0F / 60.0F));
        return horde.enemies().size() == 1U;
    };
    for (int view_distance = 4; view_distance <= 32; ++view_distance)
    {
        const std::string at = " at view distance " + std::to_string(view_distance);
        expect(world::resident_chunk_window(window, view_distance) == reference,
               "the resident window must not change with the view distance" + at);
        // What is meshed does follow the setting, and always fits inside what
        // is resident with a column to spare on every side for its light.
        const world::ChunkWindow meshed = world::meshed_chunk_window(0, -1, view_distance);
        expect(meshed.span >= 10 && meshed.span <= 20 &&
                   meshed.origin_x > reference.origin_x && meshed.origin_z > reference.origin_z &&
                   meshed.origin_x + meshed.span < reference.origin_x + reference.span &&
                   meshed.origin_z + meshed.span < reference.origin_z + reference.span,
               "the meshed window must sit strictly inside the resident one" + at);
        expect(survives(resident),
               "an enemy inside the original's window must survive" + at);
    }
    expect(world::meshed_span_chunks(8) == 10 && world::meshed_span_chunks(16) == 12 &&
               world::meshed_span_chunks(32) == 20,
           "the view distance still decides how much is meshed");

    // What sizing the resident window by the view distance did: at the default
    // setting the zombie's column was never held, so it was dropped here while
    // every stock peer kept simulating it.
    const world::ChunkWindow old_default = world::meshed_chunk_window(0, -1, 16);
    expect(!old_default.contains(9, -1),
           "fixture: 150 blocks is beyond what the default view distance meshes");
    world::WorldRegion view_sized{old_default.origin_x, old_default.origin_z, old_default.span,
                                  old_default.span};
    for (int z = view_sized.minimum_z(); z <= view_sized.maximum_z(); ++z)
        for (int x = view_sized.minimum_x(); x <= view_sized.maximum_x(); ++x)
            view_sized.set_block(x, 0, z, world::BlockType::rock);
    expect(!survives(view_sized),
           "fixture: a resident window sized by the view distance drops that enemy");

    // BlockTerrain.Teleport. A destination outside the window resets the whole
    // terrain, and until its chunks have geometry again RegionIsLoaded is false
    // everywhere: every BaseZombie removes itself, the ones near the arrival
    // point included. A move inside the window only scrolls it.
    world::EnemyHorde horde;
    horde.set_terrain_window(window);
    expect(horde.spawn_networked(0, {20.5F, 1.001F, -8.0F}, initialization, 2U, 7, 5U),
           "fixture: a zombie near the local player");
    static_cast<void>(horde.update(resident, local, 0U, lookup, 1.0F / 60.0F));
    expect(horde.enemies().size() == 1U, "fixture: alive before the move");
    StockTerrainWindow scrolled = window;
    scrolled.follow(60.0F, -8.0F);
    expect(scrolled.resets() == window.resets() && scrolled.contains(20.5F, -8.0F),
           "a move inside the window scrolls it without a reset");
    horde.set_terrain_window(scrolled);
    static_cast<void>(horde.update(resident, local, 0U, lookup, 1.0F / 60.0F));
    expect(horde.enemies().size() == 1U, "a scroll must not drop an enemy that is still inside");
    StockTerrainWindow reset = scrolled;
    reset.follow(380.0F, -8.0F);
    expect(reset.resets() == scrolled.resets() + 1U && reset.contains(380.0F, -8.0F),
           "a destination outside the window re-centres it and counts as a reset");
    // The first zombie is now far outside the window and would go anyway; the
    // one standing right where the player arrives shows the reset takes all.
    expect(horde.spawn_networked(0, {382.5F, 1.001F, -8.0F}, initialization, 3U, 7, 5U),
           "fixture: a zombie standing where the player arrives");
    horde.set_terrain_window(reset);
    static_cast<void>(horde.update(resident, local, 0U, lookup, 1.0F / 60.0F));
    expect(horde.enemies().empty(), "a terrain reset must drop every enemy on this peer");
}

// The shipped game has no water (IsWaterWorld is never set) and its surface
// lava is a solid block; both are verified against the assemblies by the
// probe's "environment" scenario and the block table comparison.
void a_default_game_has_no_water_and_solid_surface_lava()
{
    const Settings defaults;
    for (const EnhancementInfo &info : all_enhancements())
        expect(!enhancement_active(defaults, info.id, ParitySession::offline()),
               "a default game must not run any non-retail gameplay feature");
    expect(!world::is_underwater(false, -30.0F) && world::is_underwater(true, -30.0F),
           "swimming, oxygen and drowning must hang on the water-world flag alone");
    world::WorldRegion world{0, 0, 1, 1};
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            world.set_block(x, -20, z, world::BlockType::rock);
    world::PlayerSimulation dry;
    dry.state().position = {8.5F, -18.999F, 8.5F};
    dry.state().grounded = true;
    world::PlayerInput forward;
    forward.forward = 1.0F;
    for (int tick = 0; tick < 30; ++tick)
        dry.update(world, forward, 1.0F / 60.0F, false);
    // Full ground speed is 4: Player.UpdateAnimation replaces FPSRig's 5 on
    // the ground (executed by SimulationParityProbe); a swimmer would crawl at 1.894.
    expect(std::abs(std::abs(dry.state().velocity.z) - 4.0F) < 0.01F && dry.state().grounded,
           "below the old water level a default player walks at full speed on the lagoon floor");
    expect(world::blocks_player(world::BlockType::surface_lava) &&
               !world::blocks_player(world::BlockType::deep_lava) &&
               !world::surface_lava_passable(),
           "surface lava is solid and deep lava pass-through, as in the shipped block table");
    expect(world::lava_submersion(-60.0F) == 0.0F && world::lava_submersion(-61.0F) == 0.5F &&
               world::lava_submersion(-62.0F) == 1.0F && world::lava_damage_health == 25.0F &&
               world::lava_damage_period_seconds == 0.5F,
           "lava damage is the global plane below Y=-60: a quarter of health every half second");
}

void online_endurance_respawn_follows_the_original()
{
    using enum StockRespawnAction;
    // Offline, and every other mode: straight back to the start.
    expect(stock_respawn_action(GameMode::endurance, false, true, false, 0.0F) == respawn_at_start &&
               stock_respawn_action(GameMode::survival, true, false, true, 0.0F) == respawn_at_start &&
               stock_respawn_action(GameMode::creative, true, true, false, 0.0F) == respawn_at_start &&
               stock_respawn_action(GameMode::dragon_endurance, true, false, false, 0.0F) ==
                   respawn_at_start,
           "only online Endurance has the shared respawn rules");
    // Somebody is still alive: wait out the twenty seconds, then join them.
    expect(stock_respawn_action(GameMode::endurance, true, false, true, 0.0F) == wait &&
               stock_respawn_action(GameMode::endurance, true, true, true, 19.9F) == wait &&
               stock_respawn_action(GameMode::endurance, true, false, true, 20.0F) ==
                   respawn_at_teammate &&
               stock_respawn_action(GameMode::endurance, true, true, true, 45.0F) ==
                   respawn_at_teammate,
           "a dead Endurance player rejoins a living teammate after twenty seconds");
    // Nobody is alive: only the host can do anything, and it restarts the level.
    expect(stock_respawn_action(GameMode::endurance, true, true, false, 0.0F) == restart_level &&
               stock_respawn_action(GameMode::endurance, true, false, false, 0.0F) == wait &&
               stock_respawn_action(GameMode::endurance, true, false, false, 300.0F) == wait,
           "with everyone dead the host restarts the level and guests wait for it");
}

// Item and block RULES (as opposed to table cells) of the two shipped
// generations. Each expectation names the original method it was read from in
// a decompile of the owned executables; the dig and drop tables themselves are
// compared cell by cell against the executed originals by the differential
// (`dig.*` and `drop.*` facts).
void item_rules_match_the_shipped_generations()
{
    using Id = InventoryItemId;
    using B = world::BlockType;
    constexpr InventoryProfile classic = InventoryProfile::classic;
    constexpr InventoryProfile modern = InventoryProfile::modern;
    const auto near = [](float left, float right) { return std::abs(left - right) < 0.0001F; };

    // InGameHUD.Dig: `tool.CreatesWhenDug(block)` is null for a block no item
    // class is registered for, and then no pickup is created.
    expect(mined_item(B::leaves) == Id::bare_hands,
           "leaves drop nothing: neither generation registers a Leaves item");
    expect(mined_item(B::grass) == Id::dirt_block && mined_item(B::coal_ore) == Id::coal &&
               mined_item(B::space_rock) == Id::space_rock_inventory &&
               mined_item(B::surface_lava) == Id::rock_block,
           "the authored CreatesWhenDug substitutions must be unchanged");

    // InventoryItem.Initalize registrations (122 Classic, 143 Modern).
    int classic_count = 0;
    int modern_count = 0;
    for (int raw = 0; raw < static_cast<int>(Id::item_count); ++raw)
    {
        classic_count += item_registered(static_cast<Id>(raw), classic) ? 1 : 0;
        modern_count += item_registered(static_cast<Id>(raw), modern) ? 1 : 0;
    }
    expect(classic_count == 122 && modern_count == 143,
           "the registered item sets must be exactly the 122 and 143 classes the games create");
    expect(!item_registered(Id::space_knife, classic) && !item_registered(Id::space_knife, modern) &&
               !item_registered(Id::leaves_block, classic) &&
               !item_registered(Id::modern_mega_pickaxe, modern) &&
               !item_registered(Id::modern_chainsaw_1, classic) &&
               item_registered(Id::modern_chainsaw_1, modern),
           "enum members without a registered class are not items");
    std::int16_t wire{};
    Id decoded{};
    expect(!inventory_item_to_wire(Id::space_knife, ProtocolVersion::classic_v3, wire) &&
               !inventory_item_from_wire(81, ProtocolVersion::classic_v3, decoded) &&
               !inventory_item_from_wire(81, ProtocolVersion::modern_v4, decoded) &&
               !inventory_item_from_wire(144, ProtocolVersion::modern_v4, decoded) &&
               inventory_item_from_wire(80, ProtocolVersion::classic_v3, decoded) &&
               inventory_item_from_wire(143, ProtocolVersion::modern_v4, decoded),
           "an id the generation never registered must not cross the wire in either direction");

    // InventoryItem.ProcessInput is the only caller of hud.Dig and hud.Melee;
    // BlockInventoryItem, GunInventoryItem, GPSItem and GrenadeItem replace it.
    expect(item_uses_tool_input(Id::bare_hands) && item_uses_tool_input(Id::stone_pickaxe) &&
               item_uses_tool_input(Id::knife) && item_uses_tool_input(Id::compass) &&
               item_uses_tool_input(Id::coal) && item_uses_tool_input(Id::iron_laser_sword) &&
               !item_uses_tool_input(Id::dirt_block) && !item_uses_tool_input(Id::torch) &&
               !item_uses_tool_input(Id::door) && !item_uses_tool_input(Id::pistol) &&
               !item_uses_tool_input(Id::gps) && !item_uses_tool_input(Id::grenade),
           "a held block, torch, door, gun, locator or grenade can neither dig nor melee");

    // PickInventoryItem.TimeToDig, both generations.
    expect(near(dig_time_seconds(Id::iron_laser_sword, B::gold_ore, classic), 0.5F) &&
               near(dig_time_seconds(Id::copper_laser_sword, B::rock, classic), 0.01F) &&
               near(dig_time_seconds(Id::diamond_laser_sword, B::blood_stone, classic), 1.0F) &&
               near(dig_time_seconds(Id::stone_pickaxe, B::rock, classic), 2.0F) &&
               near(dig_time_seconds(Id::stone_pickaxe, B::dirt, classic), 1.5F) &&
               std::isinf(dig_time_seconds(Id::bare_hands, B::glass_basic, classic)) == false,
           "Classic: authored pick tables, and one laser table for every sword");
    expect(near(modern_pick_dig_seconds(3, 0), 0.35F) && near(modern_pick_dig_seconds(4, 1), 0.787F) &&
               near(modern_pick_dig_seconds(12, 0), 0.05F) &&
               near(modern_pick_dig_seconds(10, 0), 0.012F) &&
               near(modern_pick_dig_seconds(5, 5), 12.0F),
           "Modern: the tier formula in single precision, rounded to whole milliseconds");
    expect(near(dig_time_seconds(Id::stone_pickaxe, B::rock, modern), 0.35F) &&
               near(dig_time_seconds(Id::stone_pickaxe, B::dirt, modern), 0.35F) &&
               near(dig_time_seconds(Id::iron_pickaxe, B::gold_ore, modern), 12.0F) &&
               near(dig_time_seconds(Id::stone_pickaxe, B::iron_wall, modern), 10.0F) &&
               std::isinf(dig_time_seconds(Id::stone_pickaxe, B::gold_ore, modern)) &&
               near(dig_time_seconds(Id::iron_laser_sword, B::slime, modern), 2.5F),
           "Modern picks dig soft blocks and lower-tier walls and gate ores by tier");
    expect(near(dig_time_seconds(Id::stone_pickaxe, B::loot_block, modern), 10.0F) &&
               near(dig_time_seconds(Id::gold_pickaxe, B::lucky_loot_block, modern), 12.0F) &&
               std::isinf(dig_time_seconds(Id::iron_pickaxe, B::lucky_loot_block, modern)) &&
               std::isinf(dig_time_seconds(Id::bare_hands, B::loot_block, modern)),
           "Modern loot blocks open to a stone pick, lucky ones to a gold pick");
    expect(near(dig_time_seconds(Id::bare_hands, B::glass_basic, modern), 2.0F) &&
               near(dig_time_seconds(Id::dirt_block, B::spawn_point_basic, modern), 2.0F) &&
               near(dig_time_seconds(Id::bloodstone_pickaxe, B::teleport_station, modern), 2.0F) &&
               near(dig_time_seconds(Id::stone_spade, B::crate_iron, modern), 2.0F),
           "Modern structures and containers take any item two seconds");

    // The cells 1.9.9.8 changed.
    expect(item_enemy_damage(Id::iron_laser_sword, classic) == 8.0F &&
               item_enemy_damage(Id::iron_laser_sword, modern) == 1.0F &&
               item_enemy_damage(Id::ice, classic) == 0.01F &&
               item_enemy_damage(Id::ice, modern) == 0.1F &&
               item_enemy_damage(Id::tnt, classic) == 0.1F &&
               item_enemy_damage(Id::tnt, modern) == 0.01F &&
               item_enemy_damage(Id::modern_iron_door, modern) == 0.025F &&
               item_enemy_damage(Id::modern_lantern_fancy_block, modern) == 0.075F,
           "enemy damage is per generation where the registrations differ");
    expect(item_self_damage_per_use(Id::stone_pickaxe, classic) == 0.0025F &&
               item_self_damage_per_use(Id::stone_pickaxe, modern) == 0.002F &&
               item_self_damage_per_use(Id::stone_spade, modern) == 0.0025F,
           "only Modern's stone pickaxe wears 0.002 per use");
    expect(item_stack_limit(Id::rocket_launcher, classic) == 1U &&
               item_stack_limit(Id::rocket_launcher, modern) == 3U &&
               item_stack_limit(Id::fired_rocket, modern) == 3U &&
               item_stack_limit(Id::guided_rocket_launcher, modern) == 1U,
           "Modern stacks RocketLauncherInventoryItemClass items to three");
    expect(item_use_cooldown_seconds(Id::modern_sticky_grenade) == 1.0F,
           "the sticky grenade is a GrenadeInventoryItemClass with its one-second cooldown");

    // 1.6.3.8 PlayerInventory.Consume(itemClass, n): smallest stack first, tray
    // before backpack; 1.9.9.8 walks the slots in order.
    PlayerInventory ammo;
    for (auto &slot : ammo.tray())
        slot.reset();
    ammo.tray()[0] = InventoryStack{Id::bullets, 50U};
    ammo.tray()[1] = InventoryStack{Id::bullets, 7U};
    ammo.storage()[0] = InventoryStack{Id::bullets, 3U};
    expect(ammo.consume_item(Id::bullets, 10U) && !ammo.tray()[1].has_value() &&
               ammo.tray()[0].has_value() && ammo.tray()[0]->count == 47U &&
               ammo.storage()[0].has_value() && ammo.storage()[0]->count == 3U,
           "Classic drains the smallest tray stack, then the next, before the backpack");
    PlayerInventory ordered;
    ordered.set_profile(modern);
    for (auto &slot : ordered.tray())
        slot.reset();
    ordered.tray()[0] = InventoryStack{Id::bullets, 50U};
    ordered.tray()[1] = InventoryStack{Id::bullets, 7U};
    expect(ordered.consume_item(Id::bullets, 10U) && ordered.tray()[0]->count == 40U &&
               ordered.tray()[1]->count == 7U,
           "Modern drains in slot order");

    // Shift-click targets. 1.6.3.8 AddItemToTray knows one tray; 1.9.9.8's
    // InventoryTrayManager tops up both trays, then takes tray 0's first hole.
    PlayerInventory one_tray;
    for (auto &slot : one_tray.storage())
        slot.reset();
    for (auto &slot : one_tray.tray())
        slot = InventoryStack{Id::rock_block, 64U};
    expect(one_tray.add_stack_to_section(InventoryStack{Id::dirt_block, 5U}, true) == 5U &&
               one_tray.add_stack_to_section(InventoryStack{Id::dirt_block, 5U}, false) == 0U &&
               one_tray.count(Id::dirt_block) == 5U,
           "Classic: a full tray refuses the stack and the backpack takes it");
    PlayerInventory two_trays;
    two_trays.set_profile(modern);
    for (auto &slot : two_trays.tray(0U))
        slot = InventoryStack{Id::rock_block, 1U};
    for (auto &slot : two_trays.tray(1U))
        slot.reset();
    // Blocks stack to 999 in 1.9.9.8.
    const std::uint16_t block_limit = item_stack_limit(Id::dirt_block, modern);
    two_trays.tray(1U)[3] =
        InventoryStack{Id::dirt_block, static_cast<std::uint16_t>(block_limit - 4U)};
    expect(block_limit == 999U &&
               two_trays.add_stack_to_section(InventoryStack{Id::dirt_block, 10U}, true) == 0U &&
               two_trays.tray(1U)[3]->count == block_limit && two_trays.tray(1U)[0].has_value() &&
               two_trays.tray(1U)[0]->item == Id::dirt_block && two_trays.tray(1U)[0]->count == 6U,
           "Modern: the second tray's stacks are topped up and its first hole takes the rest");

    // Recipe.RecipeTypes: CraftingScreen builds a tab for every type except
    // Advanced, which holds the Precision Laser alone.
    const auto &book = crafting_recipes(modern);
    int advanced = 0;
    int untyped = 0;
    for (const CraftingRecipe &recipe : book)
    {
        advanced += recipe.type == RecipeType::advanced ? 1 : 0;
        untyped += recipe.type == RecipeType::none ? 1 : 0;
        if (recipe.type == RecipeType::advanced)
            expect(recipe.result == Id::modern_precision_laser,
                   "the one Advanced recipe is the Precision Laser");
    }
    std::size_t tabbed = 0;
    for (const RecipeGroup &group : modern_recipe_groups())
        tabbed += group.type_count;
    expect(advanced == 1 && untyped == 0 && tabbed == 22U &&
               book.front().type == RecipeType::components &&
               book.back().type == RecipeType::other_structure,
           "every Modern recipe carries its RecipeTypes value and 22 types have a tab");
    PlayerInventory rich;
    rich.set_profile(modern);
    static_cast<void>(rich.add_item(Id::iron, 64U));
    static_cast<void>(rich.add_item(Id::wood_block, 64U));
    for (const CraftingRecipe &recipe : book)
        if (recipe.type == RecipeType::advanced)
            expect(!rich.can_craft(recipe) && rich.craft(recipe) == 0U &&
                       rich.count(Id::modern_precision_laser) == 0U,
                   "the Precision Laser cannot be crafted: no crafting tab lists its type");
    for (const CraftingRecipe &recipe : crafting_recipes(classic))
        expect(recipe.type == RecipeType::none, "Classic recipes have no type");
}

// InventoryItem.ProcessInput reaches hud.Dig only inside
// `if (CoolDownTimer.Expired)`: a block breaks on a swing, never between swings.
void blocks_break_on_tool_swings()
{
    using B = world::BlockType;
    world::WorldRegion region{0, 0, 1, 1};
    for (int z = 0; z < world::chunk_depth; ++z)
        for (int x = 0; x < world::chunk_width; ++x)
            region.set_block(x, 0, z, B::rock);
    const auto completions = [&](float dig_seconds, int ticks) {
        world::PlayerSimulation player;
        region.set_block(5, 2, 2, B::dirt);
        player.spawn(region, 5, 5);
        player.refresh_target(region);
        std::vector<int> result;
        for (int tick = 0; tick < ticks; ++tick)
        {
            const auto update =
                player.update_breaking(region, true, 1.0F / 60.0F, 0.3F, 1U, dig_seconds);
            if (update.has_value() && update->completed)
            {
                expect(update->impact, "a block may only break on the tick a swing lands");
                result.push_back(tick);
                region.set_block(5, 2, 2, B::dirt);
                player.refresh_target(region);
            }
        }
        return result;
    };
    // A 0.01 s block with a 0.3 s tool: one per swing, not one per tick.
    const std::vector<int> fast = completions(0.01F, 120);
    expect(fast.size() >= 6U && fast.size() <= 7U && fast.front() == 0,
           "an instant block falls once per 0.3 s swing (about 3.3 a second), not sixty");
    for (std::size_t index = 1; index < fast.size(); ++index)
        expect(fast[index] - fast[index - 1] >= 17 && fast[index] - fast[index - 1] <= 19,
               "consecutive instant breaks are one tool cooldown apart");
    // 0.5 s needs the swing at 0.6 s; 2.0 s the swing at 2.1 s.
    const std::vector<int> half = completions(0.5F, 60);
    expect(half.size() == 1U && half.front() >= 35 && half.front() <= 37,
           "a 0.5 s block breaks on the third swing, at 0.6 s");
    const std::vector<int> two = completions(2.0F, 150);
    expect(two.size() == 1U && two.front() >= 125 && two.front() <= 128,
           "a 2.0 s block breaks on the swing at 2.1 s");
    // Zero-second blocks (torch, surface lava) still need a swing each.
    const std::vector<int> zero = completions(0.0F, 30);
    expect(zero.size() == 2U && zero.front() == 0, "a zero-second block still takes a swing");
}

// The RATE of swings is gameplay: a block only breaks on a swing and melee
// only lands on one, and a stock host accepts both from us without question.
// InventoryItem.ProcessInput (both generations) gates every swing on the
// item's own OneShotTimer --
//     if (CoolDownTimer.Expired) { CoolDownTimer.Reset(); UsingTool = true; ... Dig / Melee }
// -- which InventoryItem.Update advances by the frame's TimeSpan whether or
// not the button is held, and which is Expired at `Elapsed >= MaxTime`. So the
// first swing lands on the frame the button goes down, the hit is applied at
// the START of the swing, and a held button repeats every
// ceil(CoolDownTime / frame) frames, counted in 100 ns ticks. The CoolDownTime
// of every item class is compared with the shipped tables by
// SemanticsParityProbe (`item.N.cooldown`); this pins that the timer the app
// swings by lands on exactly those frames for every one of them.
void held_tools_swing_on_the_original_timer_frames()
{
    using B = world::BlockType;
    world::WorldRegion region{0, 0, 1, 1};
    for (int z = 0; z < world::chunk_depth; ++z)
        for (int x = 0; x < world::chunk_width; ++x)
            region.set_block(x, 0, z, B::rock);
    region.set_block(5, 2, 2, B::dirt);
    std::set<float> cooldowns;
    for (const InventoryProfile profile : {InventoryProfile::classic, InventoryProfile::modern})
        for (int id = 0; id < 256; ++id)
        {
            const auto item = static_cast<InventoryItemId>(id);
            if (item_registered(item, profile) && item_use_cooldown_seconds(item) > 0.0F)
                cooldowns.insert(item_use_cooldown_seconds(item));
        }
    expect(cooldowns.size() >= 4U && cooldowns.contains(0.3F),
           "fixture: the registered items have several distinct cooldowns, picks at 0.3 s");
    constexpr std::int64_t frame_ticks = 166667; // one 60 Hz XNA frame
    for (const float cooldown : cooldowns)
    {
        // TimeSpan.FromSeconds rounds to the millisecond; the timer's floor is
        // the 0.01 s the app clamps a period to (no item is that fast).
        const std::int64_t maximum_ticks =
            std::max<std::int64_t>(std::llround(static_cast<double>(cooldown) * 1000.0) * 10000,
                                   100000);
        world::PlayerSimulation player;
        player.spawn(region, 5, 5);
        player.refresh_target(region);
        std::vector<int> expected;
        std::vector<int> actual;
        std::int64_t elapsed = maximum_ticks; // in hand long enough to be ready
        for (int tick = 0; tick < 900; ++tick)
        {
            if (elapsed >= maximum_ticks)
            {
                expected.push_back(tick);
                elapsed = 0;
            }
            elapsed += frame_ticks;
            const auto update =
                player.update_breaking(region, true, 1.0F / 60.0F, cooldown, 1U, 1.0e9F);
            if (update.has_value() && update->impact)
                actual.push_back(tick);
        }
        expect(actual == expected,
               "a held tool with a " + std::to_string(cooldown) +
                   " s cooldown must swing on the original timer's frames (got " +
                   std::to_string(actual.size()) + " swings, the original makes " +
                   std::to_string(expected.size()) + "; second swing at frame " +
                   std::to_string(actual.size() > 1U ? actual[1] : -1) + " against " +
                   std::to_string(expected.size() > 1U ? expected[1] : -1) + ")");
    }
}

} // namespace

int main(int argc, char **argv)
{
    const auto protocol_from = [](std::string_view text) {
        return text == "4" ? ProtocolVersion::modern_v4 : ProtocolVersion::classic_v3;
    };
    if (argc == 4 && std::string_view{argv[1]} == "--write-facts")
        return write_facts(argv[2], protocol_from(argv[3]));
    if (argc == 4 && std::string_view{argv[1]} == "--cross-feed")
        return cross_feed(argv[2], protocol_from(argv[3]));
    if (argc != 1)
    {
        std::cerr << "usage: " << argv[0] << " [--write-facts DIR 3|4 | --cross-feed DIR 3|4]\n";
        return 2;
    }
    enemy_random_draws_match_the_runtime();
    kill_credit_is_not_an_identity_claim();
    a_stock_host_relays_what_a_stock_guest_sends();
    bursts_the_original_produces_are_never_clipped();
    a_guests_level_restart_is_relayed_like_a_stock_host_relays_it();
    relocations_land_where_the_original_puts_them();
    player_updates_follow_the_original_cadence();
    enemies_are_culled_by_the_local_terrain_window();
    the_simulated_area_does_not_depend_on_view_distance();
    a_streamed_window_is_the_same_blocks_as_a_generated_one();
    a_default_game_has_no_water_and_solid_surface_lava();
    online_endurance_respawn_follows_the_original();
    enemy_type_ids_are_translated_per_generation();
    item_tables_match_the_original_classic_registration();
    item_rules_match_the_shipped_generations();
    blocks_break_on_tool_swings();
    held_tools_swing_on_the_original_timer_frames();
    if (failures != 0)
    {
        std::cerr << failures << " mixed-session semantics expectation(s) failed\n";
        return 1;
    }
    std::cout << "mixed-session semantics: all expectations hold\n";
    return 0;
}
