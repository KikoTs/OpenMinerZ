// Everything a peer simulates for itself, against the shipped client.
//
// Ordinary enemies are never position-replicated: each peer builds one from
// its SpawnEnemyMessage and simulates it alone. The local player's own client
// is authoritative for where it stands and how much health it has. Grenades,
// pickups and tracers fly on every peer separately. A stock client and
// OpenMinerZ therefore only agree about any of them if both run the same
// simulation.
//
// With no arguments this runs the pinned half: numeric expectations recorded
// from the original (tools/OriginalClientInterop/SimulationParityProbe), so
// parity stays pinned without the game installed.
//
//   --replay <trace> [--summary <file>] [--strict]
// replays every scenario of a probe trace through the native simulation with
// the same inputs and compares frame by frame. Scenarios named `base@ticks`
// are the same script stepped at another frame time by the ORIGINAL; the
// summary reports how far the original drifts from itself there, which is the
// floor no fixed-step port can beat.
#include "openminerz/world/enemy.hpp"
#include "openminerz/world/movement_probe.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/world.hpp"

#include "simulation_parity_pinned.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace openminerz;
using world::Vec3;

int failures = 0;

void check(bool value, std::string_view message)
{
    if (value)
        return;
    ++failures;
    std::cerr << "FAIL " << message << '\n';
}

[[nodiscard]] float distance(Vec3 left, Vec3 right) noexcept
{
    const float dx = left.x - right.x;
    const float dy = left.y - right.y;
    const float dz = left.z - right.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ------------------------------------------------------------- trace reader

using Fields = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] std::vector<std::string> split(std::string_view text, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos)
        {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

[[nodiscard]] Fields fields_of(const std::vector<std::string> &tokens, std::size_t first)
{
    Fields fields;
    for (std::size_t index = first; index < tokens.size(); ++index)
    {
        const std::size_t equals = tokens[index].find('=');
        if (equals != std::string::npos)
            fields[tokens[index].substr(0, equals)] = tokens[index].substr(equals + 1);
    }
    return fields;
}

[[nodiscard]] float number(const std::string &text)
{
    return std::strtof(text.c_str(), nullptr);
}

[[nodiscard]] Vec3 vector_of(const std::string &text)
{
    const std::vector<std::string> parts = split(text, ',');
    if (parts.size() != 3)
        return {};
    return {number(parts[0]), number(parts[1]), number(parts[2])};
}

[[nodiscard]] const std::string &field(const Fields &fields, std::string_view key)
{
    static const std::string empty;
    const auto found = fields.find(key);
    return found == fields.end() ? empty : found->second;
}

[[nodiscard]] float damage_in(const std::string &events);

struct Box final
{
    int x0{}, y0{}, z0{}, x1{}, y1{}, z1{}, block{};
};

struct EnemyOut final
{
    bool removed{};
    Vec3 position{};
    Vec3 velocity{};
    float yaw{};
    std::string state;
    std::string clip;
    std::int64_t clip_ticks{};
    float clip_speed{};
    bool on_ground{};
    bool touching_wall{};
    float frustration{};
    bool fast{};
    int miss_count{};
    std::string events;
};

struct EnemyFrame final
{
    Vec3 target{};
    Vec3 target_velocity{};
    bool target_contact{};
    bool target_dead{};
    std::string action;
    EnemyOut out;
};

struct EnemyScenario final
{
    std::string name;
    int type{};
    int id{};
    std::int32_t seed{};
    Vec3 spawn{};
    std::array<int, 5> init{};
    bool local{true};
    std::int64_t ticks{166667};
    bool modern{};
    bool fest{};
    std::vector<Box> boxes;
    std::map<std::string, std::int64_t, std::less<>> clips;
    EnemyOut initial;
    std::vector<EnemyFrame> frames;
};

[[nodiscard]] EnemyOut parse_enemy_out(const std::vector<std::string> &tokens)
{
    EnemyOut out;
    const Fields fields = fields_of(tokens, 2);
    out.events = field(fields, "ev");
    if (tokens.size() > 2 && tokens[2] == "removed")
    {
        out.removed = true;
        return out;
    }
    out.position = vector_of(field(fields, "p"));
    out.velocity = vector_of(field(fields, "v"));
    out.yaw = number(field(fields, "yaw"));
    out.state = field(fields, "st");
    out.clip = field(fields, "clip");
    out.clip_ticks = std::strtoll(field(fields, "ct").c_str(), nullptr, 10);
    out.clip_speed = number(field(fields, "spd"));
    out.on_ground = field(fields, "g") == "1";
    out.touching_wall = field(fields, "w") == "1";
    out.frustration = number(field(fields, "fr"));
    out.fast = field(fields, "fast") == "1";
    out.miss_count = std::atoi(field(fields, "mc").c_str());
    return out;
}

struct PlayerFrame final
{
    float strafe{};
    float forward{};
    bool jump{};
    bool sprint{};
    // The FlyMode key went down this frame; `original_fly_mode` is the
    // original's Player._flyMode after it handled the frame.
    bool fly_key{};
    bool original_fly_mode{};
    float stamina{1.0F};
    Vec3 position{};
    Vec3 velocity{};
    bool contact{};
    float damage{};
};

struct PlayerScenario final
{
    std::string name;
    Vec3 start{};
    float yaw{};
    bool auto_climb{true};
    std::int64_t ticks{166667};
    bool modern{};
    // CastleMinerZGame.GameMode == Creative, and FPSRig.TorsoPitch in radians.
    bool creative{};
    float pitch{};
    std::vector<Box> boxes;
    std::vector<Vec3> blockers;
    std::vector<PlayerFrame> frames;
};

struct PickupScenario final
{
    std::string name;
    Vec3 start{};
    Vec3 velocity{};
    std::int64_t ticks{166667};
    std::vector<Box> boxes;
    // Where the original PickupEntity was at the end of each frame.
    std::vector<Vec3> positions;
    std::vector<bool> on_ground;
};

struct Trace final
{
    std::vector<EnemyScenario> enemies;
    std::vector<PlayerScenario> players;
    std::vector<PickupScenario> pickups;
};

[[nodiscard]] std::optional<Trace> read_trace(const std::string &path)
{
    std::ifstream input{path};
    if (!input)
        return std::nullopt;
    Trace trace;
    std::string line;
    EnemyScenario *enemy = nullptr;
    PlayerScenario *player = nullptr;
    PickupScenario *pickup = nullptr;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::vector<std::string> tokens = split(line, ' ');
        if (tokens.empty())
            continue;
        if (tokens[0] == "SCENARIO" && tokens.size() >= 3)
        {
            enemy = nullptr;
            player = nullptr;
            pickup = nullptr;
            if (tokens[1] == "pickup")
            {
                trace.pickups.emplace_back();
                pickup = &trace.pickups.back();
                pickup->name = tokens[2];
            }
            if (tokens[1] == "enemy")
            {
                trace.enemies.emplace_back();
                enemy = &trace.enemies.back();
                enemy->name = tokens[2];
            }
            else if (tokens[1] == "player")
            {
                trace.players.emplace_back();
                player = &trace.players.back();
                player->name = tokens[2];
            }
            continue;
        }
        if (tokens[0] == "END")
        {
            enemy = nullptr;
            player = nullptr;
            pickup = nullptr;
            continue;
        }
        if (pickup != nullptr)
        {
            if (tokens[0] == "PICKUP")
            {
                const Fields fields = fields_of(tokens, 1);
                pickup->start = vector_of(field(fields, "pos"));
                pickup->velocity = vector_of(field(fields, "vel"));
                pickup->ticks = std::strtoll(field(fields, "ticks").c_str(), nullptr, 10);
            }
            else if (tokens[0] == "BOX" && tokens.size() == 8)
            {
                pickup->boxes.push_back(
                    {std::atoi(tokens[1].c_str()), std::atoi(tokens[2].c_str()),
                     std::atoi(tokens[3].c_str()), std::atoi(tokens[4].c_str()),
                     std::atoi(tokens[5].c_str()), std::atoi(tokens[6].c_str()),
                     std::atoi(tokens[7].c_str())});
            }
            else if (tokens[0] == "OUT")
            {
                const Fields fields = fields_of(tokens, 2);
                pickup->positions.push_back(vector_of(field(fields, "p")));
                pickup->on_ground.push_back(field(fields, "g") == "1");
            }
            continue;
        }
        if (player != nullptr)
        {
            if (tokens[0] == "PLAYER")
            {
                const Fields fields = fields_of(tokens, 1);
                player->start = vector_of(field(fields, "pos"));
                player->yaw = number(field(fields, "yaw"));
                player->auto_climb = field(fields, "autoclimb") == "1";
                player->ticks = std::strtoll(field(fields, "ticks").c_str(), nullptr, 10);
                player->modern = field(fields, "modern") == "1";
                player->creative = field(fields, "creative") == "1";
                player->pitch = field(fields, "pitch").empty() ? 0.0F : number(field(fields, "pitch"));
            }
            else if (tokens[0] == "BOX" && tokens.size() == 8)
            {
                player->boxes.push_back(
                    {std::atoi(tokens[1].c_str()), std::atoi(tokens[2].c_str()),
                     std::atoi(tokens[3].c_str()), std::atoi(tokens[4].c_str()),
                     std::atoi(tokens[5].c_str()), std::atoi(tokens[6].c_str()),
                     std::atoi(tokens[7].c_str())});
            }
            else if (tokens[0] == "BLOCKER" && tokens.size() == 2)
            {
                player->blockers.push_back(vector_of(tokens[1]));
            }
            else if (tokens[0] == "FRAME")
            {
                const Fields fields = fields_of(tokens, 2);
                PlayerFrame frame;
                frame.strafe = number(field(fields, "strafe"));
                frame.forward = number(field(fields, "forward"));
                frame.jump = field(fields, "jump") == "1";
                frame.sprint = field(fields, "sprint") == "1";
                frame.fly_key = field(fields, "fly") == "1";
                player->frames.push_back(frame);
            }
            else if (tokens[0] == "OUT" && !player->frames.empty())
            {
                const Fields fields = fields_of(tokens, 2);
                PlayerFrame &frame = player->frames.back();
                frame.position = vector_of(field(fields, "p"));
                frame.velocity = vector_of(field(fields, "v"));
                frame.contact = field(fields, "c") == "1";
                if (!field(fields, "stam").empty())
                    frame.stamina = number(field(fields, "stam"));
                frame.original_fly_mode = field(fields, "fm") == "1";
                frame.damage = damage_in(field(fields, "ev"));
            }
            continue;
        }
        if (enemy == nullptr)
            continue;
        if (tokens[0] == "ENEMY")
        {
            const Fields fields = fields_of(tokens, 1);
            enemy->type = std::atoi(field(fields, "type").c_str());
            enemy->id = std::atoi(field(fields, "id").c_str());
            enemy->seed = std::atoi(field(fields, "seed").c_str());
            enemy->spawn = vector_of(field(fields, "pos"));
            const std::vector<std::string> init = split(field(fields, "init"), ',');
            for (std::size_t index = 0; index < 5 && index < init.size(); ++index)
                enemy->init[index] = std::atoi(init[index].c_str());
            enemy->local = field(fields, "local") == "1";
            enemy->ticks = std::strtoll(field(fields, "ticks").c_str(), nullptr, 10);
            enemy->modern = field(fields, "modern") == "1";
            enemy->fest = field(fields, "fest") == "1";
        }
        else if (tokens[0] == "BOX" && tokens.size() == 8)
        {
            enemy->boxes.push_back({std::atoi(tokens[1].c_str()), std::atoi(tokens[2].c_str()),
                                    std::atoi(tokens[3].c_str()), std::atoi(tokens[4].c_str()),
                                    std::atoi(tokens[5].c_str()), std::atoi(tokens[6].c_str()),
                                    std::atoi(tokens[7].c_str())});
        }
        else if (tokens[0] == "CLIP" && tokens.size() == 3)
        {
            enemy->clips[tokens[1]] = std::strtoll(tokens[2].c_str(), nullptr, 10);
        }
        else if (tokens[0] == "FRAME")
        {
            const Fields fields = fields_of(tokens, 2);
            EnemyFrame frame;
            frame.target = vector_of(field(fields, "t"));
            frame.target_velocity = vector_of(field(fields, "tv"));
            frame.target_contact = field(fields, "tc") == "1";
            frame.target_dead = field(fields, "td") == "1";
            frame.action = field(fields, "act");
            enemy->frames.push_back(std::move(frame));
        }
        else if (tokens[0] == "OUT" && tokens.size() >= 3)
        {
            if (tokens[1] == "-1")
                enemy->initial = parse_enemy_out(tokens);
            else if (!enemy->frames.empty())
                enemy->frames.back().out = parse_enemy_out(tokens);
        }
    }
    return trace;
}

// ------------------------------------------------------------------ enemies

[[nodiscard]] std::string_view state_name(world::EnemyState state) noexcept
{
    switch (state)
    {
    case world::EnemyState::emerge:
        return "emerge";
    case world::EnemyState::chase:
        return "chase";
    case world::EnemyState::attack:
        return "attack";
    case world::EnemyState::idle:
        return "idle";
    case world::EnemyState::hit:
        return "hit";
    case world::EnemyState::die:
        return "die";
    case world::EnemyState::give_up:
        return "give_up";
    case world::EnemyState::dig:
        return "dig";
    case world::EnemyState::despawn:
        return "despawn";
    }
    return "?";
}

[[nodiscard]] float angle_gap(float left, float right) noexcept
{
    float gap = std::fmod(std::abs(left - right), 6.2831855F);
    if (gap > 3.1415927F)
        gap = 6.2831855F - gap;
    return gap;
}

// The sum of `dmg:` amounts and the count of one message in a frame's events.
[[nodiscard]] float damage_in(const std::string &events)
{
    float total{};
    for (const std::string &event : split(events, ';'))
        if (event.rfind("dmg:", 0) == 0)
            total += number(event.substr(4));
    return total;
}

[[nodiscard]] int count_in(const std::string &events, std::string_view what)
{
    int count{};
    for (const std::string &event : split(events, ';'))
        if (event == what)
            ++count;
    return count;
}

struct EnemySample final
{
    bool removed{};
    Vec3 position{};
    std::string state;
    std::string clip;
    float damage{};
};

struct EnemyResult final
{
    std::string name;
    std::int64_t ticks{};
    float max_gap{};
    int max_gap_frame{-1};
    float end_gap{};
    int state_mismatch_frames{};
    int first_state_mismatch{-1};
    int clip_mismatch_frames{};
    std::int64_t max_clip_tick_gap{};
    float max_yaw_gap{};
    float original_damage{};
    float native_damage{};
    int damage_frame_mismatches{};
    int original_speed_ups{};
    int native_speed_ups{};
    int original_give_ups{};
    int native_give_ups{};
    int original_arrows{};
    int native_arrows{};
    float max_arrow_gap{};
    int original_removed_frame{-1};
    int native_removed_frame{-1};
    int frames{};
    // What each side did, for the retail-versus-retail baseline.
    std::vector<EnemySample> original;
    std::vector<EnemySample> native;
};

[[nodiscard]] EnemyResult replay_enemy(const EnemyScenario &scenario, bool pinned_clips)
{
    EnemyResult result;
    result.name = scenario.name;
    result.ticks = scenario.ticks;
    world::WorldRegion region{-4, -4, 8, 8};
    for (const Box &box : scenario.boxes)
        for (int z = box.z0; z <= box.z1; ++z)
            for (int x = box.x0; x <= box.x1; ++x)
                for (int y = box.y0; y <= box.y1; ++y)
                    region.set_block(x, y, z, static_cast<world::BlockType>(box.block));

    world::EnemyHorde horde;
    if (!pinned_clips)
        horde.set_clip_durations([&scenario](world::EnemyModelKind, std::string_view clip) {
            // The archer's idle clip is "idle_archer1" in 1.6.3.8's rig and
            // "idle-archer1" in 1.9.9.8's.
            auto found = scenario.clips.find(clip);
            if (found == scenario.clips.end() && clip == "idle-archer1")
                found = scenario.clips.find("idle_archer1");
            return found == scenario.clips.end()
                       ? 0.0F
                       : static_cast<float>(static_cast<double>(found->second) * 1e-7);
        });
    horde.seed_dig_random(scenario.seed);
    horde.set_zombie_fest(scenario.fest);
    horde.set_modern_rules(scenario.modern);
    const std::optional<int> type =
        world::enemy_type_from_wire(static_cast<std::uint8_t>(scenario.type), scenario.modern);
    constexpr std::uint8_t local_gid = 3;
    constexpr std::uint8_t remote_gid = 9;
    const std::uint8_t target_gid = scenario.local ? local_gid : remote_gid;
    // EnemyType.InitPackage.Read: five millisecond integers.
    const world::EnemyInitialization initialization{
        static_cast<float>(scenario.init[0]) / 1000.0F, static_cast<float>(scenario.init[1]) / 1000.0F,
        static_cast<float>(scenario.init[2]) / 1000.0F, static_cast<float>(scenario.init[3]) / 1000.0F,
        static_cast<float>(scenario.init[4]) / 1000.0F};
    if (!type.has_value() ||
        !horde.spawn_networked(*type, scenario.spawn, initialization,
                               static_cast<std::uint32_t>(scenario.id), scenario.seed, target_gid))
    {
        check(false, scenario.name + ": the native horde refused the spawn");
        return result;
    }
    const world::EnemyIdentity identity{target_gid, static_cast<std::uint32_t>(scenario.id)};
    const float delta = static_cast<float>(static_cast<double>(scenario.ticks) * 1e-7);

    const auto compare = [&](const EnemyOut &expected, int frame, const world::EnemyEvents *events) {
        const bool removed = horde.enemies().empty();
        if (expected.removed && result.original_removed_frame < 0)
            result.original_removed_frame = frame;
        if (removed && result.native_removed_frame < 0)
            result.native_removed_frame = frame;
        EnemySample original{expected.removed, expected.position, expected.state, expected.clip,
                             damage_in(expected.events)};
        EnemySample native{removed, {}, "removed", "", events != nullptr ? events->player_damage : 0.0F};
        if (!removed)
        {
            native.position = horde.enemies().front().position;
            native.state = state_name(horde.enemies().front().state);
            native.clip = horde.enemies().front().clip == "idle-archer1"
                              ? "idle_archer1"
                              : std::string{horde.enemies().front().clip};
        }
        result.original.push_back(original);
        result.native.push_back(native);
        if (events != nullptr)
        {
            const float original_damage = damage_in(expected.events);
            result.original_damage += original_damage;
            result.native_damage += events->player_damage;
            if (std::abs(original_damage - events->player_damage) > 1e-4F)
                ++result.damage_frame_mismatches;
            result.original_speed_ups += count_in(expected.events, "msg:SpeedUpEnemyMessage");
            result.native_speed_ups += static_cast<int>(events->speed_up_requests.size());
            result.original_give_ups += count_in(expected.events, "msg:EnemyGiveUpMessage");
            result.native_give_ups += static_cast<int>(events->give_up_requests.size());
            // arrow:<origin>:<velocity> as TracerManager.AddArrow received it.
            std::size_t arrow{};
            for (const std::string &event : split(expected.events, ';'))
            {
                if (event.rfind("arrow:", 0) != 0)
                    continue;
                ++result.original_arrows;
                const std::vector<std::string> parts = split(event, ':');
                if (parts.size() == 3 && arrow < events->arrow_origins.size())
                    result.max_arrow_gap = std::max(
                        {result.max_arrow_gap,
                         distance(vector_of(parts[1]), events->arrow_origins[arrow]),
                         distance(vector_of(parts[2]), events->arrow_velocities[arrow])});
                ++arrow;
            }
            result.native_arrows += events->arrows_fired;
        }
        if (expected.removed || removed)
        {
            if (expected.removed != removed)
            {
                ++result.state_mismatch_frames;
                if (result.first_state_mismatch < 0)
                    result.first_state_mismatch = frame;
            }
            return;
        }
        const world::Enemy &enemy = horde.enemies().front();
        const float gap = distance(enemy.position, expected.position);
        if (gap > result.max_gap)
        {
            result.max_gap = gap;
            result.max_gap_frame = frame;
        }
        result.end_gap = gap;
        if (state_name(enemy.state) != expected.state)
        {
            ++result.state_mismatch_frames;
            if (result.first_state_mismatch < 0)
                result.first_state_mismatch = frame;
        }
        const std::string_view clip = enemy.clip == "idle-archer1" ? "idle_archer1" : enemy.clip;
        if (clip != (expected.clip == "idle-archer1" ? "idle_archer1" : expected.clip))
            ++result.clip_mismatch_frames;
        else
            result.max_clip_tick_gap = std::max<std::int64_t>(
                result.max_clip_tick_gap, std::llabs(enemy.clip_ticks - expected.clip_ticks));
        result.max_yaw_gap = std::max(result.max_yaw_gap, angle_gap(enemy.yaw, expected.yaw));
    };

    compare(scenario.initial, -1, nullptr);
    bool kill_arrives{};
    std::vector<world::EnemyIdentity> speed_ups_arriving;
    int frame_index{};
    for (const EnemyFrame &frame : scenario.frames)
    {
        // The echo of last frame's KillEnemyMessage and SpeedUpEnemyMessage:
        // the original reads its echo queue before the scene updates.
        if (kill_arrives)
        {
            static_cast<void>(horde.kill_enemy(identity, false));
            kill_arrives = false;
        }
        for (const world::EnemyIdentity request : speed_ups_arriving)
            static_cast<void>(horde.speed_up_enemy(request));
        speed_ups_arriving.clear();
        world::PlayerState target;
        target.position = frame.target;
        target.velocity = frame.target_velocity;
        target.grounded = frame.target_contact;
        target.health = frame.target_dead ? 0.0F : 100.0F;
        world::PlayerState bystander;
        bystander.position = {0.5F, 0.001F, 30.5F};
        bystander.grounded = true;
        if (frame.action == "kill")
            kill_arrives = true;
        else if (frame.action.rfind("hit:", 0) == 0 && !horde.enemies().empty())
            horde.damage_enemy(0, number(frame.action.substr(4)), world::DamageFlags::none, false,
                               false);
        const world::EnemyEvents events = horde.update(
            region, scenario.local ? target : bystander, local_gid,
            [&](std::uint8_t gid) -> std::optional<world::PlayerState> {
                return gid == remote_gid ? std::optional{target} : std::nullopt;
            },
            delta);
        // What the app does with the frame's events before the next one.
        speed_ups_arriving = events.speed_up_requests;
        for (const auto &block : events.dug_blocks)
            region.set_block(block[0], block[1], block[2], world::BlockType::empty);
        compare(frame.out, frame_index, &events);
        ++frame_index;
    }
    result.frames = frame_index;
    return result;
}

void print_enemy(std::ostream &output, const EnemyResult &result)
{
    output << std::fixed << std::setprecision(4) << "enemy " << result.name << " frames="
           << result.frames << " max_gap=" << result.max_gap << "@" << result.max_gap_frame
           << " end_gap=" << result.end_gap << " state_mismatch_frames="
           << result.state_mismatch_frames << " first=" << result.first_state_mismatch
           << " clip_mismatch_frames=" << result.clip_mismatch_frames << " max_clip_ticks="
           << result.max_clip_tick_gap << " max_yaw=" << result.max_yaw_gap << " damage="
           << result.native_damage << "/" << result.original_damage << " damage_frame_mismatches="
           << result.damage_frame_mismatches << " speed_ups=" << result.native_speed_ups << "/"
           << result.original_speed_ups << " give_ups=" << result.native_give_ups << "/"
           << result.original_give_ups << " arrows=" << result.native_arrows << "/"
           << result.original_arrows << " max_arrow_gap=" << result.max_arrow_gap << " removed="
           << result.native_removed_frame << "/" << result.original_removed_frame << '\n';
}

// How far the ORIGINAL drifts from itself when the same script is stepped at
// another frame time, sampled at matching instants.
void print_baseline(std::ostream &output, const EnemyResult &base, const EnemyResult &other)
{
    float max_gap{};
    double state_seconds{};
    const double base_dt = static_cast<double>(base.ticks) * 1e-7;
    const double other_dt = static_cast<double>(other.ticks) * 1e-7;
    for (std::size_t index = 1; index < base.original.size(); ++index)
    {
        const double time = static_cast<double>(index) * base_dt;
        const auto other_index =
            static_cast<std::size_t>(std::llround(time / other_dt));
        if (other_index >= other.original.size())
            break;
        const EnemySample &left = base.original[index];
        const EnemySample &right = other.original[other_index];
        if (left.removed || right.removed)
        {
            if (left.removed != right.removed)
                state_seconds += base_dt;
            continue;
        }
        max_gap = std::max(max_gap, distance(left.position, right.position));
        if (left.state != right.state)
            state_seconds += base_dt;
    }
    output << std::fixed << std::setprecision(4) << "baseline " << base.name << " original@"
           << base.ticks << " vs original@" << other.ticks << " max_gap=" << max_gap
           << " seconds_in_different_state=" << state_seconds << '\n';
}

// ------------------------------------------------------------------- player

struct PlayerResult final
{
    std::string name;
    std::int64_t ticks{};
    int frames{};
    float max_gap{};
    int max_gap_frame{-1};
    float end_gap{};
    float max_velocity_gap{};
    int contact_mismatch_frames{};
    // Frames on which fly mode was on here and off in the original, or the
    // other way round, and how many frames the original flew at all.
    int fly_mode_mismatch_frames{};
    int original_fly_frames{};
    float original_damage{};
    float native_damage{};
    int damage_frame_mismatches{};
    float original_apex{};
    float native_apex{};
    float max_stamina_gap{};
    std::vector<Vec3> original;
    struct Sample final
    {
        Vec3 position{};
        bool contact{};
        float damage{};
    };
    std::vector<Sample> native;
};

// Player.ResolveCollsion's fall damage: a landing that absorbs more than the
// threshold of vertical speed costs (change - threshold) / 15 of full health.
[[nodiscard]] float fall_damage(float speed_change, float vertical_velocity, bool modern) noexcept
{
    const float threshold = modern ? 18.0F : 15.0F;
    return speed_change > threshold && vertical_velocity < 0.1F
               ? (speed_change - threshold) * (1.0F / 15.0F)
               : 0.0F;
}

[[nodiscard]] PlayerResult replay_player(const PlayerScenario &scenario)
{
    PlayerResult result;
    result.name = scenario.name;
    result.ticks = scenario.ticks;
    world::WorldRegion region{-4, -4, 8, 8};
    for (const Box &box : scenario.boxes)
        for (int z = box.z0; z <= box.z1; ++z)
            for (int x = box.x0; x <= box.x1; ++x)
                for (int y = box.y0; y <= box.y1; ++y)
                    region.set_block(x, y, z, static_cast<world::BlockType>(box.block));
    // Blocking enemies that target the local player (gid 0), as the probe
    // left its real zombies: where they spawned, in their emerge state.
    world::EnemyHorde horde;
    for (const Vec3 blocker : scenario.blockers)
    {
        world::Enemy enemy;
        enemy.position = blocker;
        enemy.blocking = true;
        enemy.health = 1.0F;
        horde.enemies().push_back(enemy);
    }
    world::PlayerSimulation simulation;
    simulation.state().position = scenario.start;
    simulation.state().yaw = scenario.yaw;
    simulation.state().pitch = scenario.pitch;
    simulation.state().grounded = true;
    simulation.set_velocity_attenuation([&horde](Vec3 forward, Vec3 position) {
        return horde.attenuate_velocity(0U, forward, position);
    });
    const float delta = static_cast<float>(static_cast<double>(scenario.ticks) * 1e-7);
    int frame_index{};
    for (const PlayerFrame &frame : scenario.frames)
    {
        // Player.ProcessInput flips fly mode after FPSRig has handled the
        // frame's input and before UpdateAnimation chooses the velocity, and
        // only where the generation and the game mode let it.
        if (frame.fly_key && world::retail_fly_key_toggles(scenario.modern, scenario.creative))
            simulation.state().fly_mode = !simulation.state().fly_mode;
        if (simulation.state().fly_mode != frame.original_fly_mode)
            ++result.fly_mode_mismatch_frames;
        if (frame.original_fly_mode)
            ++result.original_fly_frames;
        // The sprint key only exists in 1.9.9.8.
        simulation.update(region,
                          {frame.strafe, frame.forward, frame.jump, frame.sprint && scenario.modern},
                          delta,
                          scenario.auto_climb, false, world::PlayerSimulation::fly_speed,
                          scenario.modern);
        const world::PlayerState &state = simulation.state();
        const float gap = distance(state.position, frame.position);
        if (gap > result.max_gap)
        {
            result.max_gap = gap;
            result.max_gap_frame = frame_index;
        }
        result.end_gap = gap;
        result.max_velocity_gap =
            std::max(result.max_velocity_gap, distance(state.velocity, frame.velocity));
        if (state.grounded != frame.contact)
            ++result.contact_mismatch_frames;
        const float damage =
            fall_damage(simulation.landing_speed_change(), state.velocity.y, scenario.modern);
        result.original_damage += frame.damage;
        result.native_damage += damage;
        if (std::abs(damage - frame.damage) > 1e-3F)
            ++result.damage_frame_mismatches;
        // The original keeps stamina as 0..1, the native state as 0..100.
        result.max_stamina_gap =
            std::max(result.max_stamina_gap, std::abs(state.stamina * 0.01F - frame.stamina));
        result.original_apex = std::max(result.original_apex, frame.position.y);
        result.native_apex = std::max(result.native_apex, state.position.y);
        result.original.push_back(frame.position);
        result.native.push_back({state.position, state.grounded, damage});
        ++frame_index;
    }
    result.frames = frame_index;
    return result;
}

void print_player(std::ostream &output, const PlayerResult &result)
{
    output << std::fixed << std::setprecision(4) << "player " << result.name << " frames="
           << result.frames << " max_gap=" << result.max_gap << "@" << result.max_gap_frame
           << " end_gap=" << result.end_gap << " max_velocity_gap=" << result.max_velocity_gap
           << " contact_mismatch_frames=" << result.contact_mismatch_frames << " apex="
           << result.native_apex << "/" << result.original_apex << " damage="
           << result.native_damage << "/" << result.original_damage
           << " damage_frame_mismatches=" << result.damage_frame_mismatches
           << " max_stamina_gap=" << result.max_stamina_gap << " fly_frames="
           << result.original_fly_frames << " fly_mode_mismatch_frames="
           << result.fly_mode_mismatch_frames << '\n';
}

void print_player_baseline(std::ostream &output, const PlayerResult &base,
                           const PlayerResult &other)
{
    float max_gap{};
    const double base_dt = static_cast<double>(base.ticks) * 1e-7;
    const double other_dt = static_cast<double>(other.ticks) * 1e-7;
    for (std::size_t index = 0; index < base.original.size(); ++index)
    {
        const double time = static_cast<double>(index + 1) * base_dt;
        const auto other_index = static_cast<std::size_t>(std::llround(time / other_dt));
        if (other_index == 0 || other_index > other.original.size())
            continue;
        max_gap = std::max(max_gap,
                           distance(base.original[index], other.original[other_index - 1]));
    }
    output << std::fixed << std::setprecision(4) << "baseline " << base.name << " original@"
           << base.ticks << " vs original@" << other.ticks << " max_gap=" << max_gap << " apex="
           << base.original_apex << "/" << other.original_apex << " damage="
           << base.original_damage << "/" << other.original_damage << '\n';
}

// The pinned half: every scenario of simulation_parity_pinned.hpp replayed with
// the built-in clip lengths, and required to reproduce the original's state and
// clip timeline frame for frame, its position at each change, every hit on the
// player, and its message and arrow counts.
void check_pinned_enemies()
{
    for (const simulation_parity_pinned::Enemy &pinned : simulation_parity_pinned::enemies)
    {
        EnemyScenario scenario;
        scenario.name = pinned.name;
        scenario.type = pinned.type;
        scenario.id = pinned.id;
        scenario.seed = pinned.seed;
        scenario.spawn = {pinned.spawn[0], pinned.spawn[1], pinned.spawn[2]};
        for (std::size_t index = 0; index < 5; ++index)
            scenario.init[index] = pinned.init[index];
        scenario.local = pinned.local;
        scenario.fest = pinned.fest;
        for (const simulation_parity_pinned::Box &box : pinned.boxes)
            scenario.boxes.push_back({box.x0, box.y0, box.z0, box.x1, box.y1, box.z1, box.block});
        std::size_t target{};
        std::size_t action{};
        for (int frame = 0; frame < pinned.frames; ++frame)
        {
            while (target + 1 < pinned.targets.size() && pinned.targets[target + 1].frame <= frame)
                ++target;
            const simulation_parity_pinned::Target &sample = pinned.targets[target];
            EnemyFrame input;
            input.target = {sample.x, sample.y, sample.z};
            input.target_velocity = {sample.vx, sample.vy, sample.vz};
            input.target_contact = sample.contact;
            input.target_dead = sample.dead;
            input.action = "-";
            if (action < pinned.actions.size() && pinned.actions[action].frame == frame)
            {
                input.action = pinned.actions[action].kill
                                   ? std::string{"kill"}
                                   : "hit:" + std::to_string(pinned.actions[action].damage);
                ++action;
            }
            scenario.frames.push_back(std::move(input));
        }
        const EnemyResult result = replay_enemy(scenario, true);
        const std::string name = pinned.name;

        // result.native[0] is the freshly spawned enemy, [f + 1] the end of frame f.
        std::size_t step{};
        std::string previous;
        float worst_position{};
        bool timeline_matches = true;
        std::string where;
        for (std::size_t index = 0; index < result.native.size(); ++index)
        {
            const EnemySample &sample = result.native[index];
            if (sample.removed)
                break;
            const std::string key = sample.state + "/" + sample.clip;
            if (key == previous)
                continue;
            previous = key;
            const int frame = static_cast<int>(index) - 1;
            if (step >= pinned.steps.size() || pinned.steps[step].frame != frame ||
                sample.state != pinned.steps[step].state || sample.clip != pinned.steps[step].clip)
            {
                if (timeline_matches)
                    where = " (native " + key + " at frame " + std::to_string(frame) + ", step " +
                            std::to_string(step) + ")";
                timeline_matches = false;
                break;
            }
            worst_position = std::max(
                worst_position, distance(sample.position, {pinned.steps[step].x, pinned.steps[step].y,
                                                           pinned.steps[step].z}));
            ++step;
        }
        check(timeline_matches && step == pinned.steps.size(),
              name + ": the state and clip timeline must be the original's frame for frame" + where);
        check(worst_position <= 0.002F,
              name + ": the enemy must stand where the original stood at every state change");

        std::size_t damage{};
        bool damages_match = true;
        for (std::size_t index = 1; index < result.native.size(); ++index)
        {
            if (result.native[index].damage <= 0.0F)
                continue;
            const int frame = static_cast<int>(index) - 1;
            // Two hits can land on one frame; the pinned list has one row each.
            float expected{};
            bool any{};
            while (damage < pinned.damages.size() && pinned.damages[damage].frame == frame)
            {
                expected += pinned.damages[damage].amount;
                any = true;
                ++damage;
            }
            if (!any || std::abs(expected - result.native[index].damage) > 1e-4F)
                damages_match = false;
        }
        check(damages_match && damage == pinned.damages.size(),
              name + ": every hit on the player must land on the original's frame");
        check(result.native_removed_frame == pinned.removed_frame,
              name + ": the enemy must be removed on the original's frame");
        check(result.native_speed_ups == pinned.speed_ups,
              name + ": SpeedUpEnemyMessage count must match the original");
        check(result.native_give_ups == pinned.give_ups,
              name + ": EnemyGiveUpMessage count must match the original");
        check(result.native_arrows == pinned.arrows, name + ": arrows fired must match the original");
    }
}

// The local player, pinned: Player.ProcessInput and Player.ResolveCollsion as
// the shipped 1.6.3.8 client ran them over each scripted input.
void check_pinned_players()
{
    for (const simulation_parity_pinned::Player &pinned : simulation_parity_pinned::players)
    {
        PlayerScenario scenario;
        scenario.name = pinned.name;
        scenario.start = {pinned.start[0], pinned.start[1], pinned.start[2]};
        scenario.yaw = pinned.yaw;
        scenario.auto_climb = pinned.auto_climb;
        for (const simulation_parity_pinned::Box &box : pinned.boxes)
            scenario.boxes.push_back({box.x0, box.y0, box.z0, box.x1, box.y1, box.z1, box.block});
        for (const simulation_parity_pinned::Point &blocker : pinned.blockers)
            scenario.blockers.push_back({blocker.x, blocker.y, blocker.z});
        std::size_t input{};
        std::size_t jump{};
        for (int frame = 0; frame < pinned.frames; ++frame)
        {
            while (input + 1 < pinned.inputs.size() && pinned.inputs[input + 1].frame <= frame)
                ++input;
            PlayerFrame step;
            step.strafe = pinned.inputs[input].strafe;
            step.forward = pinned.inputs[input].forward;
            if (jump < pinned.jumps.size() && pinned.jumps[jump] == frame)
            {
                step.jump = true;
                ++jump;
            }
            scenario.frames.push_back(step);
        }
        const PlayerResult result = replay_player(scenario);
        const std::string name = pinned.name;
        float worst{};
        bool contact_matches = true;
        for (const simulation_parity_pinned::PlayerCheck &expected : pinned.checks)
        {
            const PlayerResult::Sample &sample =
                result.native[static_cast<std::size_t>(expected.frame)];
            worst = std::max(worst, distance(sample.position, {expected.x, expected.y, expected.z}));
            contact_matches = contact_matches && sample.contact == expected.contact;
        }
        check(worst <= 0.002F, name + ": the player must stand where the original player stood");
        check(contact_matches, name + ": ground contact must match the original");
        std::size_t damage{};
        bool damage_matches = true;
        for (std::size_t frame = 0; frame < result.native.size(); ++frame)
        {
            if (result.native[frame].damage <= 0.0F)
                continue;
            if (damage >= pinned.damages.size() ||
                pinned.damages[damage].frame != static_cast<int>(frame) ||
                std::abs(pinned.damages[damage].amount - result.native[frame].damage) > 1e-3F)
                damage_matches = false;
            ++damage;
        }
        check(damage_matches && damage == pinned.damages.size(),
              name + ": fall damage must land on the original frame with its amount");
    }

    // 1.9.9.8, recorded from the shipped client by the same probe: sprinting
    // runs at 8 on the ground and in the air, an airborne player keeps 4 where
    // 1.6.3.8 carries 5, a second jump costs a quarter of the stamina bar, and
    // the fall damage threshold is 18 instead of 15.
    world::WorldRegion floor{-1, -1, 2, 2};
    for (int z = -16; z < 16; ++z)
        for (int x = -16; x < 16; ++x)
            floor.set_block(x, -1, z, world::BlockType::rock);
    constexpr float frame = 1.0F / 60.0F;
    const auto modern_player = [] {
        world::PlayerSimulation simulation;
        simulation.state().position = {0.5F, 0.001F, 0.5F};
        simulation.state().yaw = 1.5707964F;
        simulation.state().grounded = true;
        return simulation;
    };
    world::PlayerSimulation sprinter = modern_player();
    for (int update = 0; update < 10; ++update)
        sprinter.update(floor, {0.0F, 1.0F, false, true}, frame, true, false,
                        world::PlayerSimulation::fly_speed, true);
    check(std::abs(sprinter.state().velocity.x - 8.0F) < 1e-4F && sprinter.state().sprinting,
          "a 1.9.9.8 sprint must run at 8");
    check(std::abs(sprinter.state().stamina - 90.0F) < 0.01F,
          "sprinting must cost 0.6 of the stamina bar a second");
    world::PlayerSimulation jumper = modern_player();
    jumper.update(floor, {0.0F, 1.0F, true}, frame, true, false,
                  world::PlayerSimulation::fly_speed, true);
    jumper.update(floor, {0.0F, 1.0F, false}, frame, true, false,
                  world::PlayerSimulation::fly_speed, true);
    check(!jumper.state().grounded && std::abs(jumper.state().velocity.x - 4.0F) < 1e-4F,
          "an airborne 1.9.9.8 player must keep the ground run speed of 4");
    jumper.update(floor, {0.0F, 1.0F, true}, frame, true, false,
                  world::PlayerSimulation::fly_speed, true);
    check(std::abs(jumper.state().stamina - 75.0F) < 0.01F && jumper.state().velocity.y > 15.0F,
          "the 1.9.9.8 second jump must cost a quarter of the stamina bar");
    jumper.update(floor, {0.0F, 1.0F, false}, frame, true, false,
                  world::PlayerSimulation::fly_speed, true);
    jumper.update(floor, {0.0F, 1.0F, true}, frame, true, false,
                  world::PlayerSimulation::fly_speed, true);
    check(std::abs(jumper.state().stamina - 75.0F) < 0.01F,
          "a third jump in one flight must be refused");
    world::PlayerSimulation classic_jumper = modern_player();
    classic_jumper.update(floor, {0.0F, 1.0F, true}, frame, true);
    classic_jumper.update(floor, {0.0F, 1.0F, false}, frame, true);
    check(std::abs(classic_jumper.state().velocity.x - 5.0F) < 1e-4F,
          "an airborne 1.6.3.8 player must carry FPSRig's speed of 5");
    check(std::abs(fall_damage(22.0F, 0.0F, true) - 4.0F / 15.0F) < 1e-6F &&
              std::abs(fall_damage(22.0F, 0.0F, false) - 7.0F / 15.0F) < 1e-6F &&
              fall_damage(17.9F, 0.0F, true) == 0.0F && fall_damage(14.9F, 0.0F, false) == 0.0F,
          "fall damage is (change - 15) / 15 in 1.6.3.8 and (change - 18) / 15 in 1.9.9.8");
}

// Dropped items, pinned: where the shipped PickupEntity came to rest after five
// seconds at 60 Hz, for a thrown stack, one thrown at a wall and a mined
// block's upward hop off a pillar. Every peer flies a pickup itself, and a
// player may collect whatever its own copy rests within two blocks of.
void check_pinned_pickups()
{
    world::WorldRegion region{-4, -4, 8, 8};
    const auto fill = [&region](int x0, int y0, int z0, int x1, int y1, int z1) {
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                    region.set_block(x, y, z, world::BlockType::rock);
    };
    fill(-40, -3, -40, 60, -1, 40);
    fill(6, 0, -6, 6, 3, 6);
    fill(-3, 0, -1, -3, 1, 1);
    struct Throw final
    {
        std::string_view name;
        Vec3 start;
        Vec3 velocity;
        Vec3 rest;
    };
    constexpr std::array<Throw, 3> throws{{
        {"thrown", {0.5F, 1.4F, 0.5F}, {3.9F, 0.4F, 0.8F}, {3.22479129F, 0.3000014F, 1.0463233F}},
        {"thrown at a wall", {4.2F, 1.4F, 0.5F}, {3.6F, 0.4F, -1.7F},
         {5.69995451F, 0.3000014F, -0.6785392F}},
        {"upward from a pillar", {-2.5F, 2.5F, 0.5F}, {1.06F, 0.15F, -1.05F},
         {-2.07634068F, 2.30000138F, 0.0803378F}},
    }};
    for (const Throw &thrown : throws)
    {
        Vec3 position = thrown.start;
        Vec3 velocity = thrown.velocity;
        bool on_ground{};
        for (int frame = 0; frame < 300; ++frame)
        {
            const world::BoxSlide moved =
                world::advance_pickup(region, position, velocity, 1.0F / 60.0F);
            position = moved.position;
            velocity = moved.velocity;
            on_ground = moved.on_ground;
        }
        check(distance(position, thrown.rest) <= 0.002F && on_ground,
              std::string{thrown.name} + ": a pickup must come to rest where the original does");
    }
}

int replay(const std::string &path, const std::string &summary_path, bool strict)
{
    const std::optional<Trace> trace = read_trace(path);
    if (!trace.has_value())
    {
        std::cerr << "cannot read " << path << '\n';
        return 2;
    }
    std::ostringstream summary;
    std::vector<EnemyResult> results;
    for (const EnemyScenario &scenario : trace->enemies)
    {
        results.push_back(replay_enemy(scenario, false));
        print_enemy(summary, results.back());
    }
    for (const EnemyResult &other : results)
    {
        const std::size_t at = other.name.find('@');
        if (at == std::string::npos)
            continue;
        for (const EnemyResult &base : results)
            if (base.name == other.name.substr(0, at))
                print_baseline(summary, base, other);
    }
    if (strict)
        for (const EnemyResult &result : results)
        {
            check(result.max_gap <= 0.05F, result.name + ": position gap versus the original");
            check(result.state_mismatch_frames <= 2,
                  result.name + ": state timeline versus the original");
            check(std::abs(result.native_damage - result.original_damage) < 1e-3F,
                  result.name + ": damage dealt versus the original");
        }
    std::vector<PlayerResult> players;
    for (const PlayerScenario &scenario : trace->players)
    {
        players.push_back(replay_player(scenario));
        print_player(summary, players.back());
    }
    for (const PlayerResult &other : players)
    {
        const std::size_t at = other.name.find('@');
        if (at == std::string::npos)
            continue;
        for (const PlayerResult &base : players)
            if (base.name == other.name.substr(0, at))
                print_player_baseline(summary, base, other);
    }
    // Pickups: world::advance_pickup against the original PickupEntity.
    struct PickupResult final
    {
        std::string name;
        std::int64_t ticks{};
        float max_gap{};
        float rest_gap{};
        int ground_mismatch_frames{};
        const PickupScenario *scenario{};
    };
    std::vector<PickupResult> pickups;
    for (const PickupScenario &scenario : trace->pickups)
    {
        world::WorldRegion region{-4, -4, 8, 8};
        for (const Box &box : scenario.boxes)
            for (int z = box.z0; z <= box.z1; ++z)
                for (int x = box.x0; x <= box.x1; ++x)
                    for (int y = box.y0; y <= box.y1; ++y)
                        region.set_block(x, y, z, static_cast<world::BlockType>(box.block));
        PickupResult result{scenario.name, scenario.ticks, 0.0F, 0.0F, 0, &scenario};
        const float delta = static_cast<float>(static_cast<double>(scenario.ticks) * 1e-7);
        Vec3 position = scenario.start;
        Vec3 velocity = scenario.velocity;
        for (std::size_t frame = 0; frame < scenario.positions.size(); ++frame)
        {
            const world::BoxSlide moved = world::advance_pickup(region, position, velocity, delta);
            position = moved.position;
            velocity = moved.velocity;
            result.rest_gap = distance(position, scenario.positions[frame]);
            result.max_gap = std::max(result.max_gap, result.rest_gap);
            if (moved.on_ground != scenario.on_ground[frame])
                ++result.ground_mismatch_frames;
        }
        summary << std::fixed << std::setprecision(4) << "pickup " << result.name << " frames="
                << scenario.positions.size() << " max_gap=" << result.max_gap << " rest_gap="
                << result.rest_gap << " ground_mismatch_frames=" << result.ground_mismatch_frames
                << '\n';
        pickups.push_back(result);
    }
    for (const PickupResult &other : pickups)
    {
        const std::size_t at = other.name.find('@');
        if (at == std::string::npos || other.scenario->positions.empty())
            continue;
        for (const PickupResult &base : pickups)
            if (base.name == other.name.substr(0, at) && !base.scenario->positions.empty())
                summary << std::fixed << std::setprecision(4) << "baseline " << base.name
                        << " original@" << base.ticks << " vs original@" << other.ticks
                        << " rest_gap="
                        << distance(base.scenario->positions.back(),
                                    other.scenario->positions.back())
                        << '\n';
    }
    if (strict)
        for (const PickupResult &result : pickups)
            check(result.max_gap <= 0.02F,
                  result.name + ": pickup trajectory versus the original");
    if (strict)
        for (const PlayerResult &result : players)
        {
            check(result.max_gap <= 0.02F,
                  result.name + ": player position gap versus the original");
            check(result.contact_mismatch_frames <= 1,
                  result.name + ": ground contact versus the original");
            check(result.fly_mode_mismatch_frames == 0,
                  result.name + ": fly mode versus the original");
            check(std::abs(result.native_damage - result.original_damage) < 2e-3F,
                  result.name + ": fall damage versus the original");
        }
    std::cout << summary.str();
    if (!summary_path.empty())
        std::ofstream{summary_path} << summary.str();
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    std::string replay_path;
    std::string summary_path;
    bool strict{};
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--replay" && index + 1 < argc)
            replay_path = argv[++index];
        else if (argument == "--summary" && index + 1 < argc)
            summary_path = argv[++index];
        else if (argument == "--strict")
            strict = true;
    }
    if (!replay_path.empty())
        return replay(replay_path, summary_path, strict);
    check_pinned_enemies();
    check_pinned_players();
    check_pinned_pickups();
    if (failures != 0)
    {
        std::cerr << failures << " simulation parity check(s) failed\n";
        return 1;
    }
    std::cout << "openminerz_simulation_parity_tests: all checks passed\n";
    return 0;
}
