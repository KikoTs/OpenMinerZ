// Dragon and fireball network parity.
//
// With no arguments this is a self-contained regression suite with explicit
// numeric expectations (it needs neither the game nor a GPU).
//
// With arguments it is the OpenMinerZ half of the differential test driven by
// tools/OriginalClientInterop/DragonParityProbe, which executes the ORIGINAL
// DragonEntity / DragonClientEntity / EnemyManager from the owned assemblies:
//
//   --write-owner <trace> <kind> <seconds> <seed> <gid>
//       our dragon owner, publishing the exact payload bytes it would send
//   --replica <owner trace> <replica trace> <latency ticks> <gid>
//       our watcher, fed a message stream (ours or the original's)
//   --compare <owner trace> <replica trace> [limits...]
//       where the watcher presents the dragon and its fireballs versus the owner
//   --tables <original tables>
//       DragonType constants (as dumped from the original statics) against ours
//
// The trace format is shared with the probe (see the header of its Program.cs).

#include "openminerz/net/dragon_codec.hpp"
#include "openminerz/world/dragon_brain.hpp"
#include "openminerz/world/enemy.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;

namespace
{

constexpr float tick_seconds = 1.0F / 60.0F;

struct TraceMessage final
{
    int tick{};
    std::string name;
    bool reliable{};
    std::uint8_t sender{};
    std::vector<std::uint8_t> payload;
};

struct TraceFireball final
{
    int tick{};
    std::int32_t index{};
    world::Vec3 origin{};
    world::Vec3 velocity{};
};

struct TraceSample final
{
    int tick{};
    world::Vec3 position{};
    bool visible{};
    bool dead{};
    float time{};
    // B lines only: the rest of the DragonEntity state.
    std::string state;
    float yaw{};
    float pitch{};
    float roll{};
    float velocity{};
    int animation{};
    int next_animation{};
    float flap_debt{};
    int shots_left{};
    int shot_pending{};
    float target_yaw{};
    int chances{};
};

struct Trace final
{
    int kind{};
    int owner_gid{};
    int ticks{};
    world::Vec3 player{};
    std::vector<TraceMessage> messages;
    std::vector<TraceSample> brain;     // B: the owner's flight brain
    std::vector<TraceSample> presented; // P (owner screen) or R (watcher screen)
    std::vector<TraceFireball> fireballs;
};

[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return "-";
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    text.reserve(bytes.size() * 2U);
    for (const std::uint8_t byte : bytes)
    {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 15U]);
    }
    return text;
}

[[nodiscard]] std::vector<std::uint8_t> unhex(std::string_view text)
{
    std::vector<std::uint8_t> bytes;
    if (text == "-")
        return bytes;
    for (std::size_t index{}; index + 1U < text.size(); index += 2U)
    {
        unsigned value{};
        std::from_chars(text.data() + index, text.data() + index + 2U, value, 16);
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

[[nodiscard]] std::string number(float value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.9g", static_cast<double>(value));
    return buffer;
}

[[nodiscard]] std::string vector(world::Vec3 value)
{
    return number(value.x) + " " + number(value.y) + " " + number(value.z);
}

[[nodiscard]] bool read_trace(const std::string &path, Trace &trace)
{
    std::ifstream input{path};
    if (!input)
        return false;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::istringstream fields{line};
        std::string tag;
        fields >> tag;
        if (tag == "H")
        {
            float delta{};
            fields >> delta >> trace.kind >> trace.owner_gid >> trace.ticks >> trace.player.x >>
                trace.player.y >> trace.player.z;
        }
        else if (tag == "M")
        {
            TraceMessage message;
            int reliable{};
            int sender{};
            std::string payload;
            fields >> message.tick >> message.name >> reliable >> sender >> payload;
            message.reliable = reliable != 0;
            message.sender = static_cast<std::uint8_t>(sender);
            message.payload = unhex(payload);
            trace.messages.push_back(std::move(message));
        }
        else if (tag == "B")
        {
            TraceSample sample;
            fields >> sample.tick >> sample.time >> sample.position.x >> sample.position.y >>
                sample.position.z >> sample.state >> sample.yaw >> sample.pitch >> sample.roll >>
                sample.velocity >> sample.animation >> sample.next_animation >>
                sample.flap_debt >> sample.shots_left >> sample.shot_pending >>
                sample.target_yaw >> sample.chances;
            sample.visible = true;
            trace.brain.push_back(sample);
        }
        else if (tag == "P" || tag == "R")
        {
            TraceSample sample;
            int visible{};
            int dead{};
            fields >> sample.tick >> sample.position.x >> sample.position.y >> sample.position.z >>
                visible >> sample.time >> dead;
            sample.visible = visible != 0;
            sample.dead = dead != 0;
            trace.presented.push_back(sample);
        }
        else if (tag == "F")
        {
            TraceFireball fireball;
            fields >> fireball.tick >> fireball.index >> fireball.origin.x >> fireball.origin.y >>
                fireball.origin.z >> fireball.velocity.x >> fireball.velocity.y >>
                fireball.velocity.z;
            trace.fireballs.push_back(fireball);
        }
    }
    return true;
}

[[nodiscard]] float distance(world::Vec3 left, world::Vec3 right) noexcept
{
    const float dx = left.x - right.x;
    const float dy = left.y - right.y;
    const float dz = left.z - right.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] net::Vector3 wire(world::Vec3 value) noexcept
{
    return {value.x, value.y, value.z};
}

[[nodiscard]] world::Vec3 local(net::Vector3 value) noexcept
{
    return {value.x, value.y, value.z};
}

// ---------------------------------------------------------------------------
// OpenMinerZ owner: DragonFlight is the brain, the echo replica and the wire.
// ---------------------------------------------------------------------------
int write_owner(const std::string &path, int kind, float seconds, std::uint64_t seed,
                std::uint8_t gid)
{
    std::ofstream output{path, std::ios::binary};
    if (!output)
        return 2;
    const int ticks = static_cast<int>(std::lround(seconds * 60.0F));
    world::PlayerState player;
    player.position = {8.5F, 64.0F, 8.5F};
    world::EnemyRng rng;
    rng.state = seed;
    output << "# openminerz.dragon-trace.v1 source=openminerz\n";
    output << "H " << number(tick_seconds) << ' ' << kind << ' ' << static_cast<int>(gid) << ' '
           << ticks << ' ' << vector(player.position) << '\n';
    net::PayloadError error{};
    const auto spawn = net::encode_spawn_dragon_payload(
        {static_cast<std::uint8_t>(kind), gid, false, -1.0F}, error);
    output << "M 0 SpawnDragonMessage 1 0 " << hex(*spawn) << '\n';

    world::DragonFlight dragon;
    dragon.spawn(static_cast<world::DragonKind>(kind), player.position, rng);
    dragon.begin_network_ownership(gid, 0U);
    for (int tick = 1; tick <= ticks; ++tick)
    {
        static_cast<void>(dragon.update(player, tick_seconds, rng, true));
        for (const world::DragonOwnerMessage &message : dragon.take_owner_messages())
        {
            net::DragonWaypointPayload waypoint;
            waypoint.position = wire(message.position);
            waypoint.velocity = wire(message.velocity);
            waypoint.host_time = message.host_time;
            waypoint.target_roll = message.target_roll;
            waypoint.animation = message.animation;
            waypoint.sound = message.sound;
            if (message.attack)
            {
                const auto payload = net::encode_dragon_attack_payload(
                    {waypoint, wire(message.target), false, message.fireball_index}, error);
                output << "M " << tick << " DragonAttackMessage 1 " << static_cast<int>(gid) << ' '
                       << hex(*payload) << '\n';
            }
            else
            {
                const auto payload = net::encode_update_dragon_payload(waypoint, error);
                output << "M " << tick << " UpdateDragonMessage " << (message.reliable ? 1 : 0)
                       << ' ' << static_cast<int>(gid) << ' ' << hex(*payload) << '\n';
            }
        }
        output << "B " << tick << ' ' << number(dragon.owner_time()) << ' '
               << vector(dragon.brain().position) << " brain\n";
        output << "P " << tick << ' ' << vector(dragon.dragon().position) << ' '
               << (dragon.visible() ? 1 : 0) << ' ' << number(dragon.replica_time()) << " 0\n";
        for (const world::DragonReplicaEvent &event : dragon.take_network_replica_events())
        {
            if (event.kind != world::DragonReplicaEventKind::fireball)
                continue;
            const world::Vec3 velocity = world::dragon_fireball_velocity(
                static_cast<world::DragonKind>(kind), event.position, event.target);
            output << "F " << tick << ' ' << event.fireball_id << ' ' << vector(event.position)
                   << ' ' << vector(velocity) << '\n';
        }
    }
    return output ? 0 : 2;
}

// ---------------------------------------------------------------------------
// OpenMinerZ watcher.
// ---------------------------------------------------------------------------
int write_replica(const std::string &input_path, const std::string &output_path, int latency)
{
    Trace trace;
    if (!read_trace(input_path, trace))
        return 2;
    std::ofstream output{output_path, std::ios::binary};
    if (!output)
        return 2;
    output << "# openminerz.dragon-replica.v1 source=openminerz latency_ticks=" << latency << '\n';
    world::DragonFlight dragon;
    std::size_t next{};
    net::PayloadError error{};
    world::EnemyRng death_rng;
    for (int tick = 0; tick <= trace.ticks; ++tick)
    {
        for (; next < trace.messages.size() && trace.messages[next].tick + latency <= tick; ++next)
        {
            const TraceMessage &message = trace.messages[next];
            if (message.name == "SpawnDragonMessage")
            {
                net::SpawnDragonPayload spawn;
                if (!net::decode_spawn_dragon_payload(message.payload, spawn, error))
                    return 3;
                dragon.install_replica(static_cast<world::DragonKind>(spawn.dragon_type),
                                       spawn.health);
            }
            else if (message.name == "UpdateDragonMessage")
            {
                net::DragonWaypointPayload waypoint;
                if (!net::decode_update_dragon_payload(message.payload, waypoint, error))
                    return 3;
                dragon.apply_network_waypoint(local(waypoint.position), local(waypoint.velocity),
                                              waypoint.host_time, waypoint.target_roll,
                                              waypoint.animation, waypoint.sound);
            }
            else if (message.name == "KillDragonMessage")
                dragon.damage(1.0e9F);
            else if (message.name == "DragonAttackMessage")
            {
                net::DragonAttackPayload attack;
                if (!net::decode_dragon_attack_payload(message.payload, attack, error))
                    return 3;
                static_cast<void>(dragon.apply_network_attack_waypoint(
                    local(attack.waypoint.position), local(attack.waypoint.velocity),
                    attack.waypoint.host_time, attack.waypoint.target_roll,
                    attack.waypoint.animation, attack.waypoint.sound, local(attack.target),
                    attack.fireball_index, attack.animated_attack));
            }
        }
        if (tick == 0)
            continue;
        // HandleKillDragonMessage -> DragonClientEntity.Kill, then UpdateWhileDead.
        if (dragon.dragon().state == world::DragonState::dying)
            static_cast<void>(dragon.update(world::PlayerState{}, tick_seconds, death_rng, false));
        else
            dragon.extrapolate_network_replica(tick_seconds);
        output << "R " << tick << ' ' << vector(dragon.dragon().position) << ' '
               << (dragon.visible() ? 1 : 0) << ' ' << number(dragon.replica_time()) << " 0\n";
        for (const world::DragonReplicaEvent &event : dragon.take_network_replica_events())
        {
            if (event.kind != world::DragonReplicaEventKind::fireball)
                continue;
            const world::Vec3 velocity = world::dragon_fireball_velocity(
                static_cast<world::DragonKind>(trace.kind), event.position, event.target);
            output << "F " << tick << ' ' << event.fireball_id << ' ' << vector(event.position)
                   << ' ' << vector(velocity) << '\n';
        }
    }
    return output ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Comparison: what a watcher presents versus what the owner presents.
// ---------------------------------------------------------------------------
struct Comparison final
{
    int samples{};
    float presented_max{};
    double presented_sum{};
    float path_max{};
    double path_sum{};
    int fireballs_owner{};
    int fireballs_replica{};
    int fireballs_matched{};
    int fireball_tick_max{};
    float fireball_origin_max{};
    float fireball_velocity_max{};
    int first_visible_owner{-1};
    int first_visible_replica{-1};
};

[[nodiscard]] Comparison compare(const Trace &owner, const Trace &replica)
{
    Comparison result;
    std::map<int, const TraceSample *> owner_presented;
    for (const TraceSample &sample : owner.presented)
    {
        owner_presented[sample.tick] = &sample;
        if (sample.visible && result.first_visible_owner < 0)
            result.first_visible_owner = sample.tick;
    }
    for (const TraceSample &sample : replica.presented)
    {
        if (!sample.visible)
            continue;
        if (result.first_visible_replica < 0)
            result.first_visible_replica = sample.tick;
        const auto found = owner_presented.find(sample.tick);
        if (found == owner_presented.end() || !found->second->visible)
            continue;
        ++result.samples;
        const float gap = distance(sample.position, found->second->position);
        result.presented_max = std::max(result.presented_max, gap);
        result.presented_sum += gap;
        // How far the watcher is from the path the brain actually flew,
        // independent of how far behind along that path it is.
        // (Two watchers compared with each other carry no brain samples.)
        float nearest = owner.brain.empty() ? 0.0F : 1.0e30F;
        const int first = std::max(0, sample.tick - 240);
        const int last = std::min(static_cast<int>(owner.brain.size()) - 1, sample.tick + 30);
        for (int index = first; index <= last; ++index)
            nearest = std::min(nearest,
                               distance(sample.position,
                                        owner.brain[static_cast<std::size_t>(index)].position));
        result.path_max = std::max(result.path_max, nearest);
        result.path_sum += nearest;
    }
    result.fireballs_owner = static_cast<int>(owner.fireballs.size());
    result.fireballs_replica = static_cast<int>(replica.fireballs.size());
    for (const TraceFireball &mine : owner.fireballs)
        for (const TraceFireball &theirs : replica.fireballs)
        {
            if (mine.index != theirs.index)
                continue;
            ++result.fireballs_matched;
            result.fireball_tick_max =
                std::max(result.fireball_tick_max, std::abs(theirs.tick - mine.tick));
            result.fireball_origin_max =
                std::max(result.fireball_origin_max, distance(mine.origin, theirs.origin));
            result.fireball_velocity_max =
                std::max(result.fireball_velocity_max, distance(mine.velocity, theirs.velocity));
            break;
        }
    return result;
}

int run_compare(int argc, char **argv)
{
    Trace owner;
    Trace replica;
    if (!read_trace(argv[2], owner) || !read_trace(argv[3], replica))
    {
        std::cerr << "Could not read traces\n";
        return 2;
    }
    const Comparison result = compare(owner, replica);
    const float presented_mean =
        result.samples > 0 ? static_cast<float>(result.presented_sum / result.samples) : 0.0F;
    const float path_mean =
        result.samples > 0 ? static_cast<float>(result.path_sum / result.samples) : 0.0F;
    std::cout << "samples=" << result.samples << " presented_gap_max=" << result.presented_max
              << " presented_gap_mean=" << presented_mean << " path_error_max=" << result.path_max
              << " path_error_mean=" << path_mean
              << " first_visible_owner=" << result.first_visible_owner
              << " first_visible_watcher=" << result.first_visible_replica
              << " fireballs_owner=" << result.fireballs_owner
              << " fireballs_watcher=" << result.fireballs_replica
              << " fireballs_matched=" << result.fireballs_matched
              << " fireball_tick_delta_max=" << result.fireball_tick_max
              << " fireball_origin_delta_max=" << result.fireball_origin_max
              << " fireball_velocity_delta_max=" << result.fireball_velocity_max << '\n';
    // Optional limits: presented max, path max, fireball ticks, origin, velocity.
    const auto limit = [&](int index, float fallback) {
        return argc > index ? std::strtof(argv[index], nullptr) : fallback;
    };
    bool passed = result.samples > 0;
    passed = passed && result.presented_max <= limit(4, 1.0e30F);
    passed = passed && result.path_max <= limit(5, 1.0e30F);
    passed = passed && static_cast<float>(result.fireball_tick_max) <= limit(6, 1.0e30F);
    passed = passed && result.fireball_origin_max <= limit(7, 1.0e30F);
    passed = passed && result.fireball_velocity_max <= limit(8, 1.0e30F);
    // A shot published in the trace's last half second may still be in flight
    // towards the watcher's waypoint when the trace ends.
    if (argc > 4)
        passed = passed && result.fireballs_owner > 0 &&
                 result.fireballs_matched == result.fireballs_replica &&
                 result.fireballs_owner - result.fireballs_matched <= 1;
    std::cout << (passed ? "PASS" : "FAIL") << " dragon presentation parity\n";
    return passed ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Brain differential: our port of DragonEntity beside the original, in the
// probe's own scenario and on the original's own random stream.
// ---------------------------------------------------------------------------
// The probe's session: nobody FindBestTarget can see (it has no NetworkSession
// to enumerate), MathTools.Rnd seeded, the player's position injected as a
// remembered TravelTarget once the dragon is inside MaxViewDistance.
class ProbeSession final : public world::DragonBrainSession
{
  public:
    ProbeSession(std::int32_t seed, std::uint8_t gid, world::Vec3 player) noexcept
        : random_{seed}, gid_{gid}, player_{player}
    {
    }
    std::vector<world::DragonTargetCandidate> players(world::Vec3) override
    {
        return {};
    }
    std::optional<world::Vec3> living_position(std::uint8_t) override
    {
        return std::nullopt;
    }
    bool can_see(world::Vec3, world::Vec3) override
    {
        return true;
    }
    bool region_loaded(world::Vec3) override
    {
        return true;
    }
    std::uint8_t local_gid() override
    {
        return gid_;
    }
    bool local_valid() override
    {
        return true;
    }
    world::Vec3 local_position() override
    {
        return player_;
    }
    float random_float() override
    {
        return random_.next01();
    }
    int random_int(int minimum, int maximum_exclusive) override
    {
        return random_.next_int(minimum, maximum_exclusive);
    }

  private:
    world::EnemyInstanceRng random_;
    std::uint8_t gid_;
    world::Vec3 player_;
};

// XNA's fixed step as DragonEntity sees it: (float)TimeSpan.FromTicks(166667).TotalSeconds.
constexpr float xna_step = 0.0166667F;

int write_retail_owner(const std::string &path, int kind, float seconds, std::int32_t seed,
                       bool modern_content,
                       std::uint8_t gid)
{
    std::ofstream output{path, std::ios::binary};
    if (!output)
        return 2;
    const int ticks = static_cast<int>(std::lround(seconds * 60.0F));
    const world::Vec3 player{8.5F, 64.0F, 8.5F};
    const auto dragon_kind = static_cast<world::DragonKind>(kind);
    ProbeSession session{seed, gid, player};
    output << "# openminerz.dragon-trace.v1 source=openminerz-brain\n";
    output << "H " << number(tick_seconds) << ' ' << kind << ' ' << static_cast<int>(gid) << ' '
           << ticks << ' ' << vector(player) << '\n';
    // HandleSpawnDragonMessage: InitSpawnState draws the heading, then
    // RecalculateDragonBox draws its jitter.
    const float heading = world::dragon_brain_spawn_heading(session);
    static_cast<void>(session.random_float());
    world::EnemyRng unused;
    world::DragonFlight dragon;
    dragon.spawn(dragon_kind, player, unused);
    dragon.set_modern_content(modern_content);
    dragon.begin_network_ownership(gid, 0U);
    world::DragonBrainState &brain = dragon.brain_state();
    world::dragon_brain_spawn(brain, dragon_kind, player, heading);
    net::PayloadError error{};
    output << "M 0 SpawnDragonMessage 1 0 "
           << hex(*net::encode_spawn_dragon_payload(
                  {static_cast<std::uint8_t>(kind), gid, false, -1.0F}, error))
           << '\n';
    bool acquired{};
    for (int tick = 1; tick <= ticks; ++tick)
    {
        static_cast<void>(dragon.update_owner(session, xna_step));
        for (const world::DragonOwnerMessage &message : dragon.take_owner_messages())
        {
            const net::DragonWaypointPayload waypoint{wire(message.position),
                                                      wire(message.velocity),
                                                      message.host_time,
                                                      message.target_roll,
                                                      message.animation,
                                                      message.sound};
            if (message.attack)
                output << "M " << tick << " DragonAttackMessage 1 " << static_cast<int>(gid) << ' '
                       << hex(*net::encode_dragon_attack_payload(
                              {waypoint, wire(message.target), false, message.fireball_index},
                              error))
                       << '\n';
            else
                output << "M " << tick << " UpdateDragonMessage " << (message.reliable ? 1 : 0)
                       << ' ' << static_cast<int>(gid) << ' '
                       << hex(*net::encode_update_dragon_payload(waypoint, error)) << '\n';
        }
        output << "B " << tick << ' ' << number(brain.dragon_time) << ' ' << vector(brain.position)
               << ' ' << world::dragon_brain_mode_name(brain.mode) << ' ' << number(brain.yaw)
               << ' ' << number(brain.pitch) << ' ' << number(brain.roll) << ' '
               << number(brain.velocity) << ' ' << static_cast<int>(brain.current_animation)
               << ' ' << static_cast<int>(brain.next_animation) << ' ' << number(brain.flap_debt)
               << ' ' << brain.shots_left << ' ' << (brain.shot_pending ? 1 : 0) << ' '
               << number(brain.target_yaw) << ' ' << brain.chances_to_not_attack << '\n';
        // The probe's injection, field for field.
        if (!acquired && distance(brain.position, player) < 300.0F)
        {
            acquired = true;
            brain.travel_target = player;
            brain.next_sound = 1U;
            brain.chances_to_not_attack = 0;
            brain.time_left_til_shots_heard = 0.0F;
            brain.loiters_left = 1'000'000;
            world::dragon_brain_change_state(
                brain, world::dragon_brain_next_attack_type(brain, session), session);
        }
        output << "P " << tick << ' ' << vector(dragon.dragon().position) << ' '
               << (dragon.visible() ? 1 : 0) << ' ' << number(dragon.replica_time()) << " 0\n";
        for (const world::DragonReplicaEvent &event : dragon.take_network_replica_events())
            if (event.kind == world::DragonReplicaEventKind::fireball)
                output << "F " << tick << ' ' << event.fireball_id << ' ' << vector(event.position)
                       << ' '
                       << vector(world::dragon_fireball_velocity(dragon_kind, event.position,
                                                                 event.target))
                       << '\n';
    }
    return output ? 0 : 2;
}

[[nodiscard]] float angle_gap(float left, float right) noexcept
{
    float gap = std::abs(left - right);
    if (gap > 3.14159274F)
        gap = 6.28318548F - gap;
    return gap;
}

// The original DragonEntity versus ours over the same scenario: the state on
// every tick and every message it sent.
int compare_brain(int argc, char **argv)
{
    Trace original;
    Trace ours;
    if (!read_trace(argv[2], original) || !read_trace(argv[3], ours))
        return 2;
    const std::size_t ticks = std::min(original.brain.size(), ours.brain.size());
    int state_mismatches{};
    int first_state_mismatch{-1};
    int animation_mismatches{};
    int shot_mismatches{};
    float position_max{};
    float attitude_max{};
    float velocity_max{};
    float flap_max{};
    std::map<std::string, int> visited;
    for (std::size_t index{}; index < ticks; ++index)
    {
        const TraceSample &theirs = original.brain[index];
        const TraceSample &mine = ours.brain[index];
        ++visited[theirs.state];
        if (theirs.state != mine.state)
        {
            ++state_mismatches;
            if (first_state_mismatch < 0)
                first_state_mismatch = theirs.tick;
        }
        animation_mismatches += theirs.animation != mine.animation ||
                                        theirs.next_animation != mine.next_animation
                                    ? 1
                                    : 0;
        shot_mismatches += theirs.shots_left != mine.shots_left ||
                                   theirs.shot_pending != mine.shot_pending ||
                                   theirs.chances != mine.chances
                               ? 1
                               : 0;
        position_max = std::max(position_max, distance(theirs.position, mine.position));
        attitude_max = std::max({attitude_max, angle_gap(theirs.yaw, mine.yaw),
                                 std::abs(theirs.pitch - mine.pitch),
                                 std::abs(theirs.roll - mine.roll)});
        velocity_max = std::max(velocity_max, std::abs(theirs.velocity - mine.velocity));
        flap_max = std::max(flap_max, std::abs(theirs.flap_debt - mine.flap_debt));
    }
    // Messages: same tick, same kind, same bytes apart from float noise.
    int message_mismatches = std::abs(static_cast<int>(original.messages.size()) -
                                      static_cast<int>(ours.messages.size()));
    int byte_identical{};
    float waypoint_max{};
    int attacks{};
    net::PayloadError error{};
    const std::size_t messages = std::min(original.messages.size(), ours.messages.size());
    for (std::size_t index{}; index < messages; ++index)
    {
        const TraceMessage &theirs = original.messages[index];
        const TraceMessage &mine = ours.messages[index];
        if (theirs.tick != mine.tick || theirs.name != mine.name ||
            theirs.reliable != mine.reliable)
        {
            ++message_mismatches;
            continue;
        }
        byte_identical += theirs.payload == mine.payload ? 1 : 0;
        net::DragonAttackPayload left;
        net::DragonAttackPayload right;
        if (theirs.name == "DragonAttackMessage")
        {
            ++attacks;
            if (!net::decode_dragon_attack_payload(theirs.payload, left, error) ||
                !net::decode_dragon_attack_payload(mine.payload, right, error) ||
                left.fireball_index != right.fireball_index ||
                left.animated_attack != right.animated_attack ||
                distance(local(left.target), local(right.target)) > 0.001F)
            {
                ++message_mismatches;
                continue;
            }
        }
        else if (theirs.name == "UpdateDragonMessage")
        {
            if (!net::decode_update_dragon_payload(theirs.payload, left.waypoint, error) ||
                !net::decode_update_dragon_payload(mine.payload, right.waypoint, error))
            {
                ++message_mismatches;
                continue;
            }
        }
        else
            continue;
        if (left.waypoint.animation != right.waypoint.animation ||
            left.waypoint.sound != right.waypoint.sound)
            ++message_mismatches;
        waypoint_max = std::max({waypoint_max,
                                 distance(local(left.waypoint.position),
                                          local(right.waypoint.position)),
                                 distance(local(left.waypoint.velocity),
                                          local(right.waypoint.velocity)),
                                 std::abs(left.waypoint.host_time - right.waypoint.host_time),
                                 std::abs(left.waypoint.target_roll - right.waypoint.target_roll)});
    }
    std::cout << "ticks=" << ticks << " states_visited=" << visited.size()
              << " state_mismatch_ticks=" << state_mismatches
              << " first_state_mismatch=" << first_state_mismatch
              << " animation_mismatch_ticks=" << animation_mismatches
              << " shot_state_mismatch_ticks=" << shot_mismatches
              << " position_max=" << position_max << " attitude_max=" << attitude_max
              << " velocity_max=" << velocity_max << " flap_debt_max=" << flap_max
              << " messages=" << original.messages.size() << '/' << ours.messages.size()
              << " attacks=" << attacks << " message_mismatches=" << message_mismatches
              << " byte_identical=" << byte_identical << " waypoint_field_max=" << waypoint_max
              << '\n';
    // Strict (one tolerance): identical on every tick. The original is 32-bit
    // .NET, whose x87 arithmetic we can only approximate, so a 0.01 snap in
    // MoveTowardTarget or the FlapDebt +-1 threshold can fall a frame apart on
    // some seeds. Loose (a second argument, the share of ticks allowed to
    // differ in animation or shot bookkeeping) still demands the same state
    // on every tick and the same number of messages and attacks.
    const float tolerance = argc > 4 ? std::strtof(argv[4], nullptr) : 1.0e30F;
    const float loose = argc > 5 ? std::strtof(argv[5], nullptr) : 0.0F;
    const auto allowed = static_cast<int>(loose * static_cast<float>(ticks));
    const bool passed =
        ticks > 0 &&
        (argc <= 4 ||
         (state_mismatches == 0 && animation_mismatches <= allowed &&
          shot_mismatches <= allowed && attacks > 0 &&
          original.messages.size() == ours.messages.size() &&
          message_mismatches <= (loose > 0.0F ? static_cast<int>(messages / 20U) : 0) &&
          position_max <= tolerance && (loose > 0.0F || waypoint_max <= tolerance)));
    std::cout << (passed ? "PASS" : "FAIL") << " dragon brain parity\n";
    return passed ? 0 : 1;
}

// ---------------------------------------------------------------------------
// A short hand-written stream (climbing left turn, then a quick shot) used to
// pin our watcher against numbers the ORIGINAL DragonClientEntity produced.
// ---------------------------------------------------------------------------
struct GoldenWaypoint final
{
    int tick{};
    world::Vec3 position{};
    world::Vec3 velocity{};
    float host_time{};
    float target_roll{};
    std::uint8_t animation{};
    bool attack{};
};

constexpr std::array<GoldenWaypoint, 8> golden_stream{{
    {1, {100.0F, 90.0F, 50.0F}, {0.0F, 0.0F, -20.0F}, 30.0F, 0.0F, 1U, false},
    {13, {100.0F, 90.0F, 46.0F}, {-2.0F, 1.0F, -19.8F}, 30.2F, 0.3F, 1U, false},
    {25, {99.2F, 90.4F, 42.1F}, {-6.0F, 2.0F, -18.9F}, 30.4F, 0.6F, 1U, false},
    {37, {97.6F, 91.0F, 38.5F}, {-10.0F, 2.0F, -17.2F}, 30.6F, 0.8F, 0U, false},
    {49, {95.2F, 91.4F, 35.3F}, {-14.0F, 1.0F, -14.2F}, 30.8F, 0.8F, 0U, false},
    {61, {92.1F, 91.5F, 32.8F}, {-17.0F, 0.0F, -10.5F}, 31.0F, 0.5F, 0U, true},
    {73, {88.5F, 91.5F, 30.9F}, {-19.0F, 0.0F, -6.2F}, 31.2F, 0.2F, 3U, false},
    {85, {84.6F, 91.5F, 29.9F}, {-20.0F, 0.0F, -2.0F}, 31.4F, 0.0F, 3U, false},
}};
constexpr world::Vec3 golden_target{20.0F, 64.0F, -30.0F};
constexpr std::int32_t golden_fireball = (3 << 23) | 7;
// One tick short of the collision phase, which the probe cannot run.
constexpr int golden_ticks = 181;
// KillDragonMessage (location, weapon id, killer gid) as a stock peer sends it.
constexpr int golden_kill_tick = 100;

[[nodiscard]] std::vector<std::uint8_t> golden_payload(const GoldenWaypoint &waypoint)
{
    net::PayloadError error{};
    const net::DragonWaypointPayload base{wire(waypoint.position), wire(waypoint.velocity),
                                          waypoint.host_time, waypoint.target_roll,
                                          waypoint.animation, 0U};
    return waypoint.attack ? *net::encode_dragon_attack_payload(
                                 {base, wire(golden_target), false, golden_fireball}, error)
                           : *net::encode_update_dragon_payload(base, error);
}

int write_golden(const std::string &path)
{
    std::ofstream output{path, std::ios::binary};
    if (!output)
        return 2;
    net::PayloadError error{};
    output << "# openminerz.dragon-trace.v1 source=golden\n";
    output << "H " << number(tick_seconds) << " 3 3 " << golden_ticks << " 20 64 -30\n";
    output << "M 0 SpawnDragonMessage 1 0 "
           << hex(*net::encode_spawn_dragon_payload({3U, 3U, false, -1.0F}, error)) << '\n';
    for (const GoldenWaypoint &waypoint : golden_stream)
        output << "M " << waypoint.tick << ' '
               << (waypoint.attack ? "DragonAttackMessage" : "UpdateDragonMessage") << " 1 3 "
               << hex(golden_payload(waypoint)) << '\n';
    // Killed by somebody else (gid 9), so no peer here rolls the loot.
    std::vector<std::uint8_t> kill;
    for (const float component : {87.5F, 91.5F, 30.6F})
    {
        std::uint32_t bits{};
        std::memcpy(&bits, &component, sizeof bits);
        for (int shift = 0; shift < 32; shift += 8)
            kill.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    kill.insert(kill.end(), {0U, 0U, 9U});
    output << "M " << golden_kill_tick << " KillDragonMessage 1 9 " << hex(kill) << '\n';
    return output ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Self-contained regressions.
// ---------------------------------------------------------------------------
int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

[[nodiscard]] bool near(float left, float right, float tolerance = 0.002F) noexcept
{
    return std::abs(left - right) <= tolerance;
}

void original_step_function_is_proportional_below_its_rate()
{
    using world::dragon_move_toward_target;
    // MathTools.MoveTowardTarget: step = clamp(error, +-rate) * dt.
    expect(near(dragon_move_toward_target(0.0F, 1.0F, 0.2F, 0.5F), 0.1F),
           "above the rate the step is rate * dt");
    expect(near(dragon_move_toward_target(0.9F, 1.0F, 1.0F, 0.5F), 0.95F),
           "below the rate the step is error * dt, not rate * dt");
    expect(dragon_move_toward_target(0.9925F, 1.0F, 1.0F, 0.5F) == 1.0F,
           "a result within 0.01 of the target snaps onto it");
    expect(near(dragon_move_toward_target(20.0F, 0.25F, 10.0F, 1.0F / 60.0F), 19.833334F),
           "hover deceleration starts at MaxAccel");
    expect(dragon_move_toward_target(0.4F, 0.4F, 1.0F, 1.0F) == 0.4F, "no error, no movement");
}

void fireball_leaves_the_measured_muzzle_at_the_authored_speed()
{
    const world::Vec3 soar = world::dragon_muzzle_offset(0U);
    const world::Vec3 flap = world::dragon_muzzle_offset(1U);
    const world::Vec3 hover = world::dragon_muzzle_offset(3U);
    expect(near(soar.x, 0.021F) && near(soar.y, 0.453F) && near(soar.z, -6.833F) &&
               near(flap.x, 0.014F) && near(flap.y, 0.764F) && near(flap.z, -8.266F) &&
               near(hover.x, -0.395F) && near(hover.y, 3.340F) && near(hover.z, -6.232F),
           "the muzzle is the measured ponytail bone, per presented animation");
    const world::Vec3 ice = world::dragon_fireball_velocity(world::DragonKind::ice,
                                                            {0.0F, 70.0F, 0.0F},
                                                            {0.0F, 10.0F, -80.0F});
    const world::Vec3 fire = world::dragon_fireball_velocity(world::DragonKind::fire,
                                                             {0.0F, 70.0F, 0.0F},
                                                             {0.0F, 10.0F, -80.0F});
    expect(near(ice.x, 0.0F) && near(ice.y, -36.0F) && near(ice.z, -48.0F) &&
               near(fire.y, -18.0F) && near(fire.z, -24.0F),
           "FireballEntity flies straight at the target at 60 (ice/undead) or 30 (fire)");
    // A detonation's block list belongs to the fireball's owner; a receiver
    // can only bound it: 3 (blast) + 1 (attached to a face) + 1 (door top).
    const world::Vec3 centre{0.5F, 0.5F, 0.5F};
    expect(world::dragon_blast_reaches(centre, 0, 0, 0) &&
               world::dragon_blast_reaches(centre, 5, 0, 0) &&
               world::dragon_blast_reaches(centre, 3, 4, 0) &&
               !world::dragon_blast_reaches(centre, 6, 0, 0) &&
               !world::dragon_blast_reaches(centre, 3, 4, 1) &&
               world::dragon_blast_reaches({10.2F, 64.9F, -3.4F}, 13, 64, -4) &&
               !world::dragon_blast_reaches({10.2F, 64.9F, -3.4F}, 16, 64, -4),
           "a fireball can remove blocks whose centre is within 5 of the impact, no further");
    expect(world::dragon_animation_index("flying_idle") == 0U &&
               world::dragon_animation_index("fly_forward") == 1U &&
               world::dragon_animation_index("attack") == 2U &&
               world::dragon_animation_index("Idle") == 3U,
           "DragonAnimEnum order is SOAR, FLAP, ATTACK, HOVER");
}

// The numbers below were produced by the ORIGINAL 1.6.3.8 DragonClientEntity
// (tools/OriginalClientInterop/DragonParityProbe, `replica` on --write-golden,
// zero latency). They are what a stock client shows for this stream.
void watcher_matches_positions_recorded_from_the_original_replica()
{
    world::DragonFlight watcher;
    watcher.install_replica(world::DragonKind::ice, -1.0F);
    world::PlayerState bystander;
    world::EnemyRng rng;
    struct Expected final
    {
        int tick;
        world::Vec3 position;
    };
    // From tick 100 on these are the original's dead dragon: KillDragonMessage
    // arrived, and the corpse flies on at the velocity it had under 10 m/s^2
    // for death_air_1's first 82 frames (the part needing no terrain).
    constexpr std::array<Expected, 10> expected{{
        {25, {100.0F, 90.0F, 50.0F}},
        {40, {99.8750153F, 90.07187F, 45.0110245F}},
        {55, {98.50006F, 90.6999741F, 40.257637F}},
        {70, {95.87514F, 91.32811F, 36.0382919F}},
        {85, {92.10026F, 91.5F, 32.80016F}},
        {99, {87.8642654F, 91.5F, 30.6917877F}},
        {100, {87.54482F, 91.49722F, 30.58762F}},
        {110, {84.3503952F, 91.3166656F, 29.54594F}},
        {140, {74.76711F, 89.10832F, 26.4209023F}},
        {181, {61.66993F, 82.04718F, 22.1500168F}},
    }};
    std::size_t next{};
    std::size_t check{};
    int fireball_tick{-1};
    world::Vec3 fireball_origin{};
    for (int tick = 1; tick <= golden_ticks; ++tick)
    {
        for (; next < golden_stream.size() && golden_stream[next].tick <= tick; ++next)
        {
            const GoldenWaypoint &waypoint = golden_stream[next];
            if (waypoint.attack)
                expect(watcher.apply_network_attack_waypoint(
                           waypoint.position, waypoint.velocity, waypoint.host_time,
                           waypoint.target_roll, waypoint.animation, 0U, golden_target,
                           golden_fireball, false),
                       "the quick shot must enter the queue");
            else
                watcher.apply_network_waypoint(waypoint.position, waypoint.velocity,
                                               waypoint.host_time, waypoint.target_roll,
                                               waypoint.animation);
        }
        if (tick == golden_kill_tick)
            watcher.damage(1.0e9F);
        if (watcher.dragon().state == world::DragonState::dying)
            static_cast<void>(watcher.update(bystander, tick_seconds, rng, false));
        else
            watcher.extrapolate_network_replica(tick_seconds);
        if (tick < 25)
            expect(!watcher.visible(), "hidden until the third waypoint has arrived");
        for (const world::DragonReplicaEvent &event : watcher.take_network_replica_events())
            if (event.kind == world::DragonReplicaEventKind::fireball)
            {
                fireball_tick = tick;
                fireball_origin = event.position;
                expect(event.fireball_id == golden_fireball, "the fireball keeps the wire index");
            }
        if (check < expected.size() && expected[check].tick == tick)
        {
            const float error = distance(watcher.dragon().position, expected[check].position);
            if (error > 0.02F)
                std::cerr << "tick " << tick << " differs from the original by " << error << '\n';
            expect(watcher.visible() && error <= 0.02F,
                   "our watcher must present the dragon where the original replica did");
            ++check;
        }
    }
    expect(check == expected.size(), "every recorded sample must be reached");
    // Original: fired at tick 86 from (86.2356, 91.8895, 29.3233871),
    // its animated ponytail bone. Ours uses that bone's mean position.
    expect(fireball_tick >= 85 && fireball_tick <= 87,
           "the quick shot fires when interpolation reaches its waypoint (original: tick 86)");
    expect(distance(fireball_origin, {86.2356F, 91.8895F, 29.3233871F}) <= 0.5F,
           "the fireball starts at the original's ponytail bone, not below the chest");
}

struct OwnerRun final
{
    Trace owner;
    Trace watcher;
};

// Our owner for 180 seconds, and an independent OpenMinerZ watcher fed the
// encoded payloads 6 ticks (100 ms) later.
[[nodiscard]] OwnerRun run_owner_and_watcher()
{
    OwnerRun run;
    world::PlayerState player;
    player.position = {8.5F, 64.0F, 8.5F};
    world::EnemyRng rng;
    world::DragonFlight owner;
    world::DragonFlight watcher;
    owner.spawn(world::DragonKind::undead, player.position, rng);
    owner.begin_network_ownership(2U, 40U);
    watcher.install_replica(world::DragonKind::undead, -1.0F);
    struct Queued final
    {
        int tick;
        world::DragonOwnerMessage message;
    };
    std::vector<Queued> wire_queue;
    std::size_t delivered{};
    constexpr int ticks = 60 * 180;
    constexpr int latency = 6;
    for (int tick = 1; tick <= ticks; ++tick)
    {
        static_cast<void>(owner.update(player, tick_seconds, rng, true));
        for (const world::DragonOwnerMessage &message : owner.take_owner_messages())
        {
            wire_queue.push_back({tick, message});
            TraceMessage record;
            record.tick = tick;
            record.name = message.attack ? "DragonAttackMessage" : "UpdateDragonMessage";
            record.reliable = message.reliable;
            run.owner.messages.push_back(record);
        }
        run.owner.brain.push_back({tick, owner.brain().position, true, false, owner.owner_time()});
        run.owner.presented.push_back(
            {tick, owner.dragon().position, owner.visible(), false, owner.replica_time()});
        for (const world::DragonReplicaEvent &event : owner.take_network_replica_events())
            if (event.kind == world::DragonReplicaEventKind::fireball)
                run.owner.fireballs.push_back(
                    {tick, event.fireball_id, event.position,
                     world::dragon_fireball_velocity(world::DragonKind::undead, event.position,
                                                     event.target)});
        for (; delivered < wire_queue.size() && wire_queue[delivered].tick + latency <= tick;
             ++delivered)
        {
            const world::DragonOwnerMessage &message = wire_queue[delivered].message;
            if (message.attack)
                static_cast<void>(watcher.apply_network_attack_waypoint(
                    message.position, message.velocity, message.host_time, message.target_roll,
                    message.animation, message.sound, message.target, message.fireball_index,
                    false));
            else
                watcher.apply_network_waypoint(message.position, message.velocity,
                                               message.host_time, message.target_roll,
                                               message.animation, message.sound);
        }
        watcher.extrapolate_network_replica(tick_seconds);
        run.watcher.presented.push_back(
            {tick, watcher.dragon().position, watcher.visible(), false, watcher.replica_time()});
        for (const world::DragonReplicaEvent &event : watcher.take_network_replica_events())
            if (event.kind == world::DragonReplicaEventKind::fireball)
                run.watcher.fireballs.push_back(
                    {tick, event.fireball_id, event.position,
                     world::dragon_fireball_velocity(world::DragonKind::undead, event.position,
                                                     event.target)});
    }
    return run;
}

void owner_publishes_the_original_cadence_and_presents_its_own_replica()
{
    const OwnerRun run = run_owner_and_watcher();
    const std::vector<TraceMessage> &messages = run.owner.messages;
    expect(messages.size() > 800U && messages.size() < 900U,
           "one message per 0.2-second slot over 180 seconds");
    expect(messages[0].tick == 1 && messages[0].reliable && messages[1].reliable &&
               !messages[2].reliable && messages[0].name == "UpdateDragonMessage",
           "the first two updates after a spawn are reliable, the rest are not");
    int attacks{};
    bool cadence{true};
    for (std::size_t index = 1; index < messages.size(); ++index)
    {
        const int gap = messages[index].tick - messages[index - 1U].tick;
        cadence = cadence && gap >= 12 && gap <= 13;
        if (messages[index].name == "DragonAttackMessage")
        {
            ++attacks;
            expect(messages[index].reliable, "an attack is always reliable");
        }
    }
    expect(cadence, "an attack REPLACES its slot's update; nothing is sent off the 0.2 s grid");
    expect(attacks >= 8, "an engaged dragon keeps attacking");

    // The owner shows its replica, never its brain: hidden until three of its
    // own waypoints have echoed back, then ~0.4 s behind the brain on its path.
    const Comparison self = compare(run.owner, run.owner);
    expect(self.first_visible_owner == 27, "the owner sees its own dragon from tick 27");
    float lag_minimum = 1.0e9F;
    float lag_maximum{};
    for (int tick = 600; tick <= 1200; ++tick)
    {
        const float lag =
            distance(run.owner.brain[static_cast<std::size_t>(tick - 1)].position,
                     run.owner.presented[static_cast<std::size_t>(tick - 1)].position);
        lag_minimum = std::min(lag_minimum, lag);
        lag_maximum = std::max(lag_maximum, lag);
    }
    expect(lag_minimum > 4.0F && lag_maximum < 9.0F,
           "at cruise the presented dragon trails the brain by 0.2-0.45 s (4-9 blocks)");
    expect(self.path_max < 0.05F, "and stays on the path the brain actually flew");

    // A second OpenMinerZ peer 100 ms away shows the same dragon and fireballs.
    const Comparison remote = compare(run.owner, run.watcher);
    expect(remote.presented_max <= 1.75F,
           "two peers disagree by latency * speed only (5 ticks * 20 m/s = 1.67 blocks)");
    expect(remote.fireballs_owner >= 8 && remote.fireballs_matched == remote.fireballs_owner &&
               remote.fireballs_replica == remote.fireballs_owner,
           "every fireball exists on both peers under the same index");
    expect(remote.fireball_tick_max <= 6 && remote.fireball_origin_max < 0.01F &&
               remote.fireball_velocity_max < 0.01F,
           "and leaves the same point with the same velocity, one latency later");
    expect(attacks - remote.fireballs_owner <= 1,
           "the owner fires from its replica: one fireball per published attack");
    expect(run.owner.fireballs.front().index == ((2 << 23) | 40) &&
               run.owner.fireballs[1].index == ((2 << 23) | 41),
           "fireball indices are (gid << 23) | DragonEntity.NextFireballIndex++");
}

void migration_carries_the_original_fields_and_never_resets_a_replica()
{
    world::PlayerState player;
    player.position = {0.0F, 64.0F, 0.0F};
    world::EnemyRng rng;
    world::DragonFlight owner;
    owner.spawn(world::DragonKind::forest, player.position, rng);
    const float spawn_heading = owner.dragon().yaw;
    owner.begin_network_ownership(0U, 5U);
    for (int tick = 0; tick < 400; ++tick)
    {
        static_cast<void>(owner.update(player, tick_seconds, rng, true));
        static_cast<void>(owner.take_owner_messages());
    }
    const world::DragonMigrationState state = owner.migration_state();
    expect(near(state.next_dragon_time, owner.owner_time() + 0.4F, 0.0001F),
           "NextDragonTime = DragonTime + GUESS_AT_LATENCY");
    expect(state.next_update_time > state.next_dragon_time - 0.001F &&
               state.next_update_time <= state.next_dragon_time + 0.2F,
           "NextUpdateTime is the pending slot, shifted by the same 0.4 s");
    expect(near(state.default_heading, spawn_heading, 0.0001F),
           "DefaultHeading is the spawn heading, not the current yaw");
    expect(near(state.position.x, owner.brain().position.x) &&
               near(state.yaw, owner.brain().yaw) && near(state.velocity, 20.0F) &&
               near(state.target_velocity, 20.0F) && state.next_fireball_index == 5 &&
               distance(state.target, owner.brain_state().travel_target) == 0.0F,
           "the brain's pose, the static fireball counter and its TravelTarget");

    // Old owner: the brain goes, the presented replica flies on untouched.
    const world::Vec3 presented = owner.dragon().position;
    const float presented_time = owner.replica_time();
    owner.end_network_ownership();
    expect(!owner.network_owner() && owner.visible() &&
               distance(owner.dragon().position, presented) == 0.0F &&
               owner.replica_time() == presented_time && owner.take_owner_messages().empty(),
           "handing the dragon away does not move or re-seed what this peer shows");

    // New owner: a brain appears beside the replica it already had.
    world::DragonFlight target;
    target.install_replica(world::DragonKind::forest, 80.0F);
    target.reset_network_replica({10.0F, 90.0F, 10.0F}, {0.0F, 0.0F, -20.0F}, 6.0F, 0.0F, 1U);
    ProbeSession adopting{7, 4U, player.position};
    target.adopt_network_ownership(4U, state, adopting);
    expect(target.network_owner() && near(target.dragon().position.x, 10.0F) &&
               near(target.brain().position.x, state.position.x) &&
               near(target.brain().yaw, state.yaw) && near(target.dragon().health, 80.0F) &&
               near(target.owner_time(), state.next_dragon_time, 0.0001F) &&
               target.next_fireball_index() == 5U,
           "InitAfterHostChange seeds the brain only; the replica keeps its own state");
    float first_time{};
    for (int tick = 0; tick < 30 && first_time == 0.0F; ++tick)
    {
        static_cast<void>(target.update(player, tick_seconds, rng, true));
        for (const world::DragonOwnerMessage &message : target.take_owner_messages())
            first_time = message.host_time;
    }
    expect(first_time > state.next_update_time && first_time < state.next_update_time + 0.05F,
           "the new owner's first waypoint is the migrated slot, on the migrated clock");
}

void starved_replica_times_out_like_the_original()
{
    world::DragonFlight watcher;
    watcher.install_replica(world::DragonKind::fire, -1.0F);
    watcher.apply_network_waypoint({0.0F, 90.0F, 0.0F}, {0.0F, 0.0F, -20.0F}, 5.0F, 0.0F, 1U);
    watcher.apply_network_waypoint({0.0F, 90.0F, -4.0F}, {0.0F, 0.0F, -20.0F}, 5.2F, 0.0F, 1U);
    watcher.apply_network_waypoint({0.0F, 90.0F, -8.0F}, {0.0F, 0.0F, -20.0F}, 5.4F, 0.0F, 1U);
    int timeouts{};
    const auto step = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick)
        {
            watcher.extrapolate_network_replica(tick_seconds);
            for (const world::DragonReplicaEvent &event : watcher.take_network_replica_events())
                timeouts += event.kind == world::DragonReplicaEventKind::timeout ? 1 : 0;
        }
    };
    step(60 * 9);
    expect(timeouts == 0, "nine silent seconds are tolerated");
    // Starved, the replica runs its clock at 0.8 and flies the last velocity:
    // 0.4 s of stream, then (9 - 0.3) * 0.8 more seconds at 20 m/s.
    expect(near(watcher.replica_time(), 5.3F + 8.7F * 0.8F, 0.05F) &&
               near(watcher.dragon().position.z, -8.0F - 20.0F * (watcher.replica_time() - 5.4F),
                    0.05F),
           "a starved replica extrapolates at 0.8x along the last velocity");
    // A stale sample is discarded by AddActionWaypoint but still feeds the watchdog.
    watcher.apply_network_waypoint({0.0F, 90.0F, 0.0F}, {0.0F, 0.0F, -20.0F}, 4.0F, 0.0F, 1U);
    step(60 * 9);
    expect(timeouts == 0, "even a discarded waypoint resets TimeoutTimer");
    step(60 * 2);
    expect(timeouts == 1, "ten silent seconds raise exactly one RemoveDragon");
    step(60 * 30);
    expect(timeouts == 1, "TimeoutTimer = -3600 afterwards: it is not repeated");
}

// DragonType.Init, as dumped from the original statics by the probe.
int check_tables(const std::string &path)
{
    std::ifstream input{path};
    if (!input)
        return 2;
    std::map<std::pair<int, std::string>, float> original;
    std::string tag;
    int lookups{};
    while (input >> tag)
    {
        std::string rest;
        if (tag == "K")
        {
            // DragonType.BreakLookup[type, block]: true means PROTECTED. Classic
            // has 46 block ids, which is our BlockType numbering one for one.
            int index{};
            std::string row;
            input >> index >> row;
            if (row.size() != 46U)
                continue;
            // (Classic's last id, 45, is its Uninitialized sentinel.)
            for (std::size_t block{}; block < row.size(); ++block)
                expect((row[block] == '1') !=
                           world::dragon_breaks_block(
                               static_cast<world::DragonKind>(index),
                               block == 45U ? world::BlockType::uninitialized
                                            : static_cast<world::BlockType>(block)),
                       "DragonType.BreakLookup differs for type " + std::to_string(index) +
                           " block " + std::to_string(block));
            ++lookups;
            continue;
        }
        if (tag != "T")
        {
            std::getline(input, rest);
            continue;
        }
        int index{};
        std::string name;
        float value{};
        input >> index >> name >> value;
        original[{index, name}] = value;
    }
    int checked{};
    for (int kind = 0; kind < 5; ++kind)
    {
        const world::DragonStats &ours = world::dragon_stats(static_cast<world::DragonKind>(kind));
        const std::array<std::pair<const char *, float>, 10> fields{{
            {"StartingHealth", ours.starting_health},
            {"FireballVelocity", ours.fireball_velocity},
            {"FireballDamage", ours.fireball_damage},
            {"Speed", ours.speed},
            {"StrafeFireRate", ours.strafe_fire_seconds},
            {"HoverFireRate", ours.hover_fire_seconds},
            {"HuntingAltitude", ours.hunting_altitude},
            {"LoiterAltitude", ours.loiter_altitude},
            {"MinAttackDistance", ours.min_attack_distance},
            {"MaxAttackDistance", ours.max_attack_distance},
        }};
        for (const auto &[name, value] : fields)
        {
            const auto found = original.find({kind, name});
            expect(found != original.end() && found->second == value,
                   std::string{"DragonType."} + name + " differs for type " +
                       std::to_string(kind));
            ++checked;
        }
    }
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " original DragonType constants (" << checked
              << " checked, " << lookups << " BreakLookup rows)\n";
    return failures == 0 ? 0 : 1;
}

void dead_dragon_falls_like_a_projectile_and_drops_its_loot_where_it_lands()
{
    // A stone plain whose top face is y = 41, far below the dragon.
    world::WorldRegion plain{-12, -6, 20, 12};
    for (int z = plain.minimum_z(); z <= plain.maximum_z(); ++z)
        for (int x = plain.minimum_x(); x <= plain.maximum_x(); ++x)
            plain.set_block(x, 40, z, world::BlockType::rock);
    world::PlayerState bystander;
    world::EnemyRng rng;
    world::DragonFlight dragon;
    dragon.install_replica(world::DragonKind::fire, 5.0F);
    // Level flight due -X at 20 m/s, 49 units above the plain.
    dragon.reset_network_replica({100.0F, 90.0F, 10.0F}, {-20.0F, 0.0F, 0.0F}, 3.0F, 0.0F, 0U);
    dragon.damage(5.0F);
    expect(dragon.dragon().state == world::DragonState::dying && dragon.dragon().pitch == 0.0F &&
               dragon.dragon().roll == 0.0F,
           "Kill levels the body on its heading");
    world::DragonEvents events;
    int ticks{};
    int landed_tick{-1};
    world::Vec3 at_one_second{};
    for (; ticks < 60 * 30 && dragon.active(); ++ticks)
    {
        events = dragon.update(bystander, tick_seconds, rng, false, &plain);
        if (ticks == 59)
            at_one_second = dragon.dragon().position;
        if (landed_tick < 0 && dragon.dragon().position.y < 41.01F)
            landed_tick = ticks + 1;
    }
    // One second of FIRST_REACTION: x = 100 - 20 t, y = 90 - 5 t (t + dt).
    expect(near(at_one_second.x, 80.0F, 0.01F) && near(at_one_second.y, 90.0F - 5.0833F, 0.01F) &&
               near(at_one_second.z, 10.0F, 0.01F),
           "the corpse keeps the velocity it died with and accelerates at 10 m/s^2");
    // 49 units under 10 m/s^2 is 3.13 s, during which it covers ~62 more units.
    expect(landed_tick >= 186 && landed_tick <= 190,
           "it reaches the ground after sqrt(2 * 49 / 10) seconds, not after a fixed timer");
    const world::Vec3 rest = dragon.dragon().position;
    expect(events.died && !dragon.active() && near(rest.y, 41.0F, 0.01F) && rest.x < 39.0F &&
               rest.x > 33.0F && near(rest.z, 10.0F, 0.01F),
           "it lands some sixty units downrange of where it was killed and skids to a stop");
    // Clip resumes on the ground (2.9667 s long, paused at 1.3667 s, finished
    // with 0.5 s to spare), then TimeToWaitAfterStop = 5 s.
    expect(ticks >= landed_tick + 60 * 6 && ticks <= landed_tick + 60 * 6 + 20,
           "the corpse is removed about 6.1 s after touching down");
    // BoxOffset (0, 0, 7): seven units behind a dragon that was heading -X.
    expect(near(events.loot_position.x, rest.x + 7.0F, 0.01F) &&
               near(events.loot_position.y, rest.y, 0.01F) &&
               near(events.loot_position.z, rest.z, 0.01F),
           "loot is rolled at the dead body box, where the corpse came to rest");
}

void death_reaction_cannot_skip_a_nearby_hell_floor()
{
    world::WorldRegion cavern{-4, -4, 8, 8};
    for (int z = cavern.minimum_z(); z <= cavern.maximum_z(); ++z)
        for (int x = cavern.minimum_x(); x <= cavern.maximum_x(); ++x)
            cavern.set_block(x, -60, z, world::BlockType::rock);
    for (float height : {-58.0F, -54.0F, -50.0F})
        for (float tick : {1.0F/30.0F, 1.0F/60.0F, 1.0F/120.0F})
        {
            world::DragonFlight dragon;
            world::EnemyRng rng;
            dragon.install_replica(world::DragonKind::undead, 5.0F);
            dragon.reset_network_replica({0, height, 0}, {0, -2, -3}, 3.0F, 0, 0U);
            dragon.damage(5.0F);
            bool landed{};
            world::DragonEvents events;
            for (int frame = 0; frame < static_cast<int>(20.0F/tick) && dragon.active(); ++frame)
            {
                events = dragon.update({}, tick, rng, false, &cavern);
                expect(dragon.dragon().position.y >= -59.001F,
                       "collision must hold from the first death frame, even before the reaction clip pauses");
                landed = landed || dragon.dragon().position.y < -58.98F;
            }
            expect(landed && events.died && near(events.loot_position.y, -59.0F, 0.02F),
                   "a low Hell death must settle and drop loot on the floor, never below it");
        }
}

void brain_targets_the_best_player_in_the_session()
{
    using world::DragonTargetCandidate;
    const world::Vec3 dragon{0.0F, 70.0F, 0.0F};
    const world::Vec3 forward{0.0F, 0.0F, -1.0F};
    const auto choose = [&](std::initializer_list<DragonTargetCandidate> players) {
        const std::vector<DragonTargetCandidate> list{players};
        return world::dragon_find_best_target(dragon, forward, list);
    };
    // gid, feet, alive, region loaded, sunlight, line of sight.
    const DragonTargetCandidate host{0U, {0.0F, 60.0F, -200.0F}, true, true, 1.0F, true};
    const DragonTargetCandidate guest{4U, {30.0F, 60.0F, -100.0F}, true, true, 1.0F, true};
    expect(choose({host, guest}).gid == std::optional<std::uint8_t>{4U},
           "the nearer of two visible players is attacked, whoever owns the dragon");
    DragonTargetCandidate shaded = guest;
    shaded.sunlight = 0.25F;
    expect(choose({host, shaded}).gid == std::optional<std::uint8_t>{0U},
           "the score is distance^2 / sunlight^2: a player in shade counts as four times as far");
    DragonTargetCandidate hidden = guest;
    hidden.line_of_sight = false;
    DragonTargetCandidate unloaded = guest;
    unloaded.region_loaded = false;
    DragonTargetCandidate dark = guest;
    dark.sunlight = 0.0F;
    DragonTargetCandidate dead = guest;
    dead.alive = false;
    expect(choose({host, hidden}).gid == std::optional<std::uint8_t>{0U} &&
               choose({host, unloaded}).gid == std::optional<std::uint8_t>{0U} &&
               choose({host, dark}).gid == std::optional<std::uint8_t>{0U} &&
               choose({host, dead}).gid == std::optional<std::uint8_t>{0U},
           "cover, unloaded terrain, darkness and death each rule a player out");
    const DragonTargetCandidate behind{5U, {0.0F, 60.0F, 150.0F}, true, true, 1.0F, true};
    const DragonTargetCandidate beside{6U, {250.0F, 68.5F, -30.0F}, true, true, 1.0F, true};
    const world::DragonTargetChoice unseen = choose({behind, beside});
    expect(!unseen.gid.has_value() && !unseen.remove_dragon,
           "players outside the 0.17 view cone are not chosen, but keep the dragon around");
    const DragonTargetCandidate distant{7U, {0.0F, 60.0F, -900.0F}, true, true, 1.0F, true};
    const DragonTargetCandidate gone{8U, {0.0F, 60.0F, -1000.0F}, true, true, 1.0F, true};
    expect(!choose({distant}).gid.has_value() && !choose({distant}).remove_dragon &&
               choose({gone}).remove_dragon && choose({dead}).remove_dragon && choose({}).remove_dragon,
           "nobody within 937.5 units (1.25 * SpawnDistance) removes the dragon");
    expect(world::dragon_target_requires_migration({151.0F, 60.0F, 0.0F}, {0.0F, 60.0F, 0.0F}) &&
               !world::dragon_target_requires_migration({149.0F, 60.0F, 0.0F},
                                                        {0.0F, 60.0F, 0.0F}),
           "a new target more than 150 units from the owner takes the dragon over");
}

void timed_dragons_follow_the_original_ladder_on_every_peer()
{
    world::EnemyRng rng;
    world::DragonSchedule schedule;
    const world::Vec3 home{10.0F, 60.0F, 10.0F};
    expect(!schedule.allowed() && !schedule.check_box(home, tick_seconds, false, rng).has_value(),
           "the first frame only lays the box out");
    // RecalculateDragonBox for FIRE: 32 minutes +- 4.
    expect(schedule.seconds_until_next() >= 28.0F * 60.0F &&
               schedule.seconds_until_next() <= 36.0F * 60.0F && schedule.next_timed_type() == 0,
           "the fire dragon is due 28-36 minutes after the box is laid out");
    for (int second = 0; second < 4; ++second)
        static_cast<void>(schedule.check_box(home, 1.0F, false, rng));
    expect(!schedule.allowed(), "nothing may be asked for in a world's first five seconds");
    static_cast<void>(schedule.check_box(home, 1.5F, false, rng));
    expect(schedule.allowed(), "then AskForDragon is open");
    std::optional<world::DragonKind> asked;
    float waited{};
    while (!asked.has_value() && waited < 40.0F * 60.0F)
    {
        asked = schedule.check_box(home, 1.0F, false, rng);
        waited += 1.0F;
    }
    expect(asked == std::optional{world::DragonKind::fire} && waited >= 27.0F * 60.0F &&
               schedule.next_timed_type() == 1,
           "the timer asks for FIRE and moves the ladder on");
    expect(near(schedule.seconds_until_next(), 59.5F, 1.0F),
           "an unanswered request is repeated a minute later (NextSpawnDragonTime += 1 min)");
    // Every peer applies the spawn: SetCurrentTimedDragonType + RecalculateDragonBox.
    schedule.dragon_installed(world::DragonKind::forest, false, home, rng);
    expect(schedule.next_timed_type() == 2 && schedule.seconds_until_next() >= 60.0F * 60.0F &&
               schedule.seconds_until_next() <= 68.0F * 60.0F,
           "a timed forest dragon anywhere puts SAND (64 +- 4 minutes) next on this peer too");
    schedule.dragon_installed(world::DragonKind::undead, true, home, rng);
    expect(schedule.next_timed_type() == 2, "a biome dragon does not move the timed ladder");
    schedule.dragon_removed(home, rng);
    expect(!schedule.allowed(), "RemoveDragonEntity closes AskForDragon for eight seconds");
    static_cast<void>(schedule.check_box(home, 8.5F, false, rng));
    expect(schedule.allowed(), "and reopens it afterwards");
    // A dragon already present swallows the slot but still advances the ladder.
    asked.reset();
    for (int second = 0; second < 70 * 60 && schedule.next_timed_type() == 2; ++second)
        asked = schedule.check_box(home, 1.0F, true, rng);
    expect(!asked.has_value() && schedule.next_timed_type() == 3,
           "AskForDragon is a no-op while a dragon exists; the type still moves on");
    static_cast<void>(schedule.check_box({700.0F, 60.0F, 10.0F}, 1.0F, false, rng));
    expect(schedule.next_timed_type() == 0,
           "walking out of the 1000-unit box starts the ladder again at FIRE");
    // Past 3600 units every timed dragon is the undead one, even after COUNT.
    world::DragonSchedule far;
    const world::Vec3 wilds{3700.0F, 60.0F, 0.0F};
    asked.reset();
    for (int second = 0; second < 40 * 60 && !asked.has_value(); ++second)
        asked = far.check_box(wilds, 1.0F, false, rng);
    expect(asked == std::optional{world::DragonKind::undead},
           "beyond 3600 units AskForDragon turns every request into SKELETON");
}

struct BlastBlock final
{
    int x;
    int y;
    int z;
};

void detonations_are_bounded_to_the_blast_and_to_their_sender()
{
    using world::DragonDetonationVerdict;
    const world::Vec3 impact{2'000.4F, 31.2F, -900.7F}; // nowhere near our loaded terrain
    const std::int32_t index = (5 << 23) | 12;
    const std::vector<BlastBlock> crater{{2'000, 31, -901}, {2'003, 31, -901}, {1'997, 30, -902},
                                         {2'000, 35, -901}};
    const world::DragonDetonationFireball ours{5U, world::DragonKind::undead,
                                               {2'010.0F, 40.0F, -905.0F}};
    const auto assess = [&](std::uint8_t sender, const std::vector<BlastBlock> &blocks,
                            bool applied, const world::DragonDetonationFireball *fireball,
                            std::optional<std::uint8_t> owner, world::Vec3 location) {
        return world::dragon_assess_detonation(sender, index, 4U, location, blocks, applied,
                                               fireball, owner);
    };
    expect(assess(5U, crater, false, &ours, std::uint8_t{5}, impact) == DragonDetonationVerdict::apply,
           "the fireball owner's crater is applied wherever it is, loaded here or not");
    expect(assess(5U, crater, false, nullptr, std::uint8_t{5}, impact) == DragonDetonationVerdict::apply,
           "the dragon's owner is believed even for a fireball this peer never fired");
    expect(assess(5U, crater, false, nullptr, std::uint8_t{3}, impact) == DragonDetonationVerdict::ignore &&
               assess(5U, crater, false, nullptr, std::nullopt, impact) ==
                   DragonDetonationVerdict::ignore,
           "anybody else's unknown fireball is not applied (and the host does not relay it)");
    expect(assess(5U, crater, true, &ours, std::uint8_t{5}, impact) == DragonDetonationVerdict::ignore,
           "a fireball detonates once");
    expect(assess(6U, crater, false, &ours, std::uint8_t{6}, impact) == DragonDetonationVerdict::reject,
           "the index must carry the sender's own id: nobody detonates another peer's fireball");
    std::vector<BlastBlock> reaching = crater;
    reaching.push_back({2'000, 31, -895}); // centre 6.2 from the impact
    expect(assess(5U, reaching, false, &ours, std::uint8_t{5}, impact) == DragonDetonationVerdict::reject,
           "one block beyond five units of the impact refuses the whole message");
    expect(assess(5U, crater, false, &ours, std::uint8_t{5}, {2'200.0F, 31.2F, -900.7F}) ==
               DragonDetonationVerdict::reject,
           "a known fireball cannot detonate further away than it could have flown");
    expect(assess(5U, crater, false, &ours, std::uint8_t{5}, {2'000.4F, 400.0F, -900.7F}) ==
               DragonDetonationVerdict::reject &&
               world::dragon_assess_detonation(5U, index, 9U, impact, crater, false, &ours,
                                               std::optional<std::uint8_t>{5U}) ==
                   DragonDetonationVerdict::reject,
           "an impact outside the world or an unknown dragon type is refused");
}

void owner_ignores_its_own_waypoints_coming_back_through_the_session()
{
    world::PlayerState player;
    player.position = {0.0F, 64.0F, 0.0F};
    world::EnemyRng rng;
    world::DragonFlight owner;
    owner.spawn(world::DragonKind::sand, player.position, rng);
    owner.begin_network_ownership(1U, 0U);
    for (int tick = 0; tick < 120; ++tick)
        static_cast<void>(owner.update(player, tick_seconds, rng, true));
    const world::Vec3 before = owner.dragon().position;
    const float time = owner.replica_time();
    // A stock host relays everything, the sender's own messages included.
    owner.apply_network_waypoint({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, 500.0F, 0.0F, 0U);
    expect(!owner.apply_network_attack_waypoint({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, 500.0F,
                                                0.0F, 0U, 0U, {1.0F, 1.0F, 1.0F}, 99, false),
           "an owner's replica is fed by its own echo only");
    static_cast<void>(owner.update(player, tick_seconds, rng, true));
    expect(distance(owner.dragon().position, before) < 1.0F &&
               owner.replica_time() < time + 0.05F && owner.take_network_replica_events().empty(),
           "a relayed copy neither moves the presented dragon nor fires a second fireball");
}

// The ORIGINAL 1.6.3.8 DragonEntity over 180 seconds (probe `owner`, undead,
// MathTools.Rnd seeded 12345, a remembered target at (8.5, 64, 8.5) once inside
// MaxViewDistance): every state change with its tick, the attacks, the message
// count and where it ended up. Our brain has to live the same life.
void brain_lives_the_life_recorded_from_the_original_dragon_entity()
{
    using world::DragonBrainMode;
    constexpr std::array<std::pair<int, DragonBrainMode>, 21> recorded{{
        {1, DragonBrainMode::default_state},   {1367, DragonBrainMode::strafe_attack},
        {2162, DragonBrainMode::loiter},       {2411, DragonBrainMode::loiter_left},
        {2744, DragonBrainMode::hover_attack}, {3678, DragonBrainMode::hover},
        {4064, DragonBrainMode::loiter},       {4339, DragonBrainMode::loiter_left},
        {4612, DragonBrainMode::hover_attack}, {5506, DragonBrainMode::hover},
        {6008, DragonBrainMode::loiter},       {6282, DragonBrainMode::loiter_left},
        {6492, DragonBrainMode::hover_attack}, {7348, DragonBrainMode::hover},
        {7840, DragonBrainMode::loiter},       {8114, DragonBrainMode::loiter_left},
        {8388, DragonBrainMode::strafe_attack}, {9313, DragonBrainMode::loiter},
        {9563, DragonBrainMode::loiter_right}, {9783, DragonBrainMode::hover_attack},
        {10645, DragonBrainMode::hover},
    }};
    const world::Vec3 player{8.5F, 64.0F, 8.5F};
    ProbeSession session{12345, 1U, player};
    const float heading = world::dragon_brain_spawn_heading(session);
    static_cast<void>(session.random_float()); // RecalculateDragonBox
    world::EnemyRng unused;
    world::DragonFlight dragon;
    dragon.spawn(world::DragonKind::undead, player, unused);
    dragon.begin_network_ownership(1U, 0U);
    world::DragonBrainState &brain = dragon.brain_state();
    world::dragon_brain_spawn(brain, world::DragonKind::undead, player, heading);
    std::vector<std::pair<int, DragonBrainMode>> lived;
    std::vector<int> attacks;
    std::vector<world::DragonOwnerMessage> attack_messages;
    int messages{};
    int hover_shots{-1};
    bool acquired{};
    for (int tick = 1; tick <= 10'800; ++tick)
    {
        static_cast<void>(dragon.update_owner(session, xna_step));
        for (const world::DragonOwnerMessage &message : dragon.take_owner_messages())
        {
            ++messages;
            if (message.attack)
            {
                attacks.push_back(tick);
                attack_messages.push_back(message);
            }
        }
        if (lived.empty() || lived.back().second != brain.mode)
            lived.emplace_back(tick, brain.mode);
        if (tick == 3678)
            hover_shots = brain.shots_left;
        static_cast<void>(dragon.take_network_replica_events());
        if (!acquired && distance(brain.position, player) < 300.0F)
        {
            acquired = true;
            brain.travel_target = player;
            brain.next_sound = 1U;
            brain.chances_to_not_attack = 0;
            brain.time_left_til_shots_heard = 0.0F;
            brain.loiters_left = 1'000'000;
            world::dragon_brain_change_state(
                brain, world::dragon_brain_next_attack_type(brain, session), session);
        }
    }
    bool same_life = lived.size() == recorded.size();
    for (std::size_t index{}; same_life && index < recorded.size(); ++index)
        same_life = lived[index] == recorded[index];
    expect(same_life, "every state change must fall on the tick the original made it");
    expect(messages == 858 && attacks.size() == 26U && attacks.front() == 1795 &&
               attacks[3] == 3758 && attacks.back() == 10'783,
           "858 messages, 26 of them attacks, in the original's slots");
    expect(hover_shots == 5, "the first hover volley is five shots (Rnd.Next(4, 8))");
    expect(!attack_messages.empty() && attack_messages.front().fireball_index == (1 << 23) &&
               attack_messages.back().fireball_index == ((1 << 23) | 25) &&
               distance(attack_messages.front().target, player) == 0.0F,
           "shots are aimed at the last position the target was seen at, seen now or not");
    expect(distance(brain.position, {-17.8030987F, 70.4802246F, 2.71073174F}) < 0.01F,
           "and after 180 seconds it is where the original is");
}

int run_regressions()
{
    original_step_function_is_proportional_below_its_rate();
    fireball_leaves_the_measured_muzzle_at_the_authored_speed();
    watcher_matches_positions_recorded_from_the_original_replica();
    owner_publishes_the_original_cadence_and_presents_its_own_replica();
    migration_carries_the_original_fields_and_never_resets_a_replica();
    starved_replica_times_out_like_the_original();
    dead_dragon_falls_like_a_projectile_and_drops_its_loot_where_it_lands();
    death_reaction_cannot_skip_a_nearby_hell_floor();
    brain_targets_the_best_player_in_the_session();
    timed_dragons_follow_the_original_ladder_on_every_peer();
    detonations_are_bounded_to_the_blast_and_to_their_sender();
    owner_ignores_its_own_waypoints_coming_back_through_the_session();
    brain_lives_the_life_recorded_from_the_original_dragon_entity();
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " dragon parity regressions (" << failures
              << " failures)\n";
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (argc == 1)
        return run_regressions();
    if (mode == "--write-retail-owner" && argc >= 7)
        return write_retail_owner(argv[2], std::atoi(argv[3]), std::strtof(argv[4], nullptr),
                                  std::atoi(argv[5]), argc > 7 && std::atoi(argv[7]) != 0,
                                  static_cast<std::uint8_t>(std::atoi(argv[6])));
    if (mode == "--compare-brain" && argc >= 4)
        return compare_brain(argc, argv);
    if (mode == "--write-golden" && argc == 3)
        return write_golden(argv[2]);
    if (mode == "--tables" && argc == 3)
        return check_tables(argv[2]);
    if (mode == "--write-owner" && argc == 7)
        return write_owner(argv[2], std::atoi(argv[3]), std::strtof(argv[4], nullptr),
                           std::strtoull(argv[5], nullptr, 10),
                           static_cast<std::uint8_t>(std::atoi(argv[6])));
    if (mode == "--replica" && argc >= 5)
        return write_replica(argv[2], argv[3], std::atoi(argv[4]));
    if (mode == "--compare" && argc >= 4)
        return run_compare(argc, argv);
    std::cerr << "usage: see the header of tests/test_dragon_parity.cpp\n";
    return 2;
}
