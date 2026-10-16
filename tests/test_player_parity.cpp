// Player parity across a mixed stock / OpenMinerZ session.
//
// Three modes:
//   (no arguments)                  pinned expectations, no game needed;
//   --write-fixtures <dir>          our bytes and what we MEAN by them, for
//                                   tools/OriginalClientInterop/PlayerParityProbe;
//   --check-original <fix> <out>    decode the bytes the ORIGINAL sender produced
//                                   for the same states and require the same meaning.
//
// Every scripted state is described twice: in our own conventions (the values
// the application hands to make_player_update) and in convention-free terms
// (a world-space heading, pitch above the horizon, heading-relative movement).
// The second description is derived from OUR simulation, never typed in, so a
// shared misreading of a field cannot hide: the original has to agree with it.

#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/net/player_replication.hpp"
#include "openminerz/player_nametag.hpp"
#include "openminerz/world/player.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
using namespace openminerz::net;

constexpr float pi = std::numbers::pi_v<float>;

void check(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error{std::string{message}};
}

struct Scenario final
{
    std::string_view name;
    world::Vec3 position;
    float yaw{};
    float pitch{};
    world::Vec3 velocity;
    float strafe{};
    float forward{};
    bool using_tool{};
    bool dead{};
    bool shouldering{};
    bool reloading{};
    bool throwing_grenade{};
    bool ready_to_throw_grenade{};
    PlayerAnimationMode mode{PlayerAnimationMode::fist};
    // The original sender's bytes for this state, recorded from a
    // PlayerParityProbe run against the owned Classic 1.6.3.8 and Modern
    // 1.9.9.8 assemblies (both produce the same payload).
    std::string_view original;
};

constexpr float degrees(float value) noexcept
{
    return value * pi / 180.0F;
}

// One line per thing a peer can see another player do.
const std::array scenarios{
    Scenario{"standing", {8.5F, 34.0F, -8.5F}, 0.0F, 0.0F, {}, 0.0F, 0.0F, false, false, false,
             false, false, false, PlayerAnimationMode::fist, "0000084100000842000008c18000000377000000000000"},
    Scenario{"walk-forward", {12.25F, 40.0F, 3.75F}, degrees(90.0F), 0.0F, {5.0F, 0.0F, 0.0F},
             0.0F, 1.0F, false, false, false, false, false, false, PlayerAnimationMode::pick, "00004441000020420000704080bf0001e7004500000000"},
    Scenario{"walk-backward", {-3.5F, 20.5F, 100.25F}, degrees(-90.0F), 0.0F,
             {5.0F, 0.0F, 0.0F}, 0.0F, -1.0F, false, false, false, false, false, false,
             PlayerAnimationMode::block, "000060c00000a4410080c8428040000207004500000000"},
    Scenario{"strafe-right", {0.5F, 10.0F, 0.5F}, 0.0F, 0.0F, {5.0F, 0.0F, 0.0F}, 1.0F, 0.0F,
             false, false, false, false, false, false, PlayerAnimationMode::generic, "0000003f000020410000003f800000007e004500000000"},
    Scenario{"strafe-left", {0.5F, 10.0F, 0.5F}, degrees(180.0F), 0.0F, {5.0F, 0.0F, 0.0F},
             -1.0F, 0.0F, false, false, false, false, false, false, PlayerAnimationMode::fist, "0000003f000020410000003f807f000370004500000000"},
    Scenario{"walk-diagonal", {64.0F, 5.0F, -64.0F}, degrees(45.0F), 0.0F,
             {5.0F, 0.0F, 0.0F}, 1.0F, 1.0F, false, false, false, false, false, false,
             PlayerAnimationMode::fist, "000080420000a040000080c280df0003ee004500000000"},
    Scenario{"analog-half", {1.0F, 2.0F, 3.0F}, degrees(30.0F), 0.0F, {1.25F, 0.0F, -2.165F},
             0.0F, 0.5F, false, false, false, false, false, false, PlayerAnimationMode::fist, "0000803f000000400000404080ea0003a7003d000054c0"},
    Scenario{"turn-just-below-pi", {1.0F, 2.0F, 3.0F}, pi - 0.002F, 0.0F, {}, 0.0F, 0.0F, false,
             false, false, false, false, false, PlayerAnimationMode::fist, "0000803f00000040000040408080000377000000000000"},
    Scenario{"turn-just-above-minus-pi", {1.0F, 2.0F, 3.0F}, -pi + 0.002F, 0.0F, {}, 0.0F, 0.0F,
             false, false, false, false, false, false, PlayerAnimationMode::fist, "0000803f0000004000004040807f000377000000000000"},
    Scenario{"turn-quarter-left", {1.0F, 2.0F, 3.0F}, degrees(-45.0F), 0.0F, {}, 0.0F, 0.0F,
             false, false, false, false, false, false, PlayerAnimationMode::fist, "0000803f00000040000040408020000377000000000000"},
    Scenario{"turn-small-right", {1.0F, 2.0F, 3.0F}, degrees(3.0F), 0.0F, {}, 0.0F, 0.0F, false,
             false, false, false, false, false, PlayerAnimationMode::fist, "0000803f000000400000404080fd000377000000000000"},
    Scenario{"look-up-limit", {1.0F, 2.0F, 3.0F}, degrees(120.0F), degrees(89.0F), {}, 0.0F,
             0.0F, false, false, false, false, false, false, PlayerAnimationMode::fist, "0000803f0000004000004040feaa000377000000000000"},
    Scenario{"look-down-limit", {1.0F, 2.0F, 3.0F}, degrees(-120.0F), degrees(-89.0F), {}, 0.0F,
             0.0F, false, false, false, false, false, false, PlayerAnimationMode::fist, "0000803f00000040000040400155000377000000000000"},
    Scenario{"look-up-30", {1.0F, 2.0F, 3.0F}, degrees(10.0F), degrees(30.0F), {}, 0.0F, 0.0F,
             false, false, false, false, false, false, PlayerAnimationMode::fist, "0000803f0000004000004040aaf8000377000000000000"},
    Scenario{"jump-rising", {8.5F, 35.2F, -8.5F}, degrees(15.0F), degrees(5.0F),
             {1.294F, 9.5F, -4.83F}, 0.0F, 1.0F, false, false, false, false, false, false,
             PlayerAnimationMode::fist, "00000841cdcc0c42000008c187f40003e72d3dc048d4c4"},
    Scenario{"falling-fast", {8.5F, 60.0F, -8.5F}, degrees(15.0F), degrees(-40.0F),
             {0.0F, -38.5F, 0.0F}, 0.0F, 0.0F, false, false, false, false, false, false,
             PlayerAnimationMode::fist, "0000084100007042000008c147f40003770000d0d00000"},
    Scenario{"swimming", {20.0F, -32.75F, 20.0F}, degrees(200.0F), degrees(-10.0F),
             {-0.648F, 0.5F, 1.78F}, 0.0F, 1.0F, false, false, false, false, false, false,
             PlayerAnimationMode::fist, "0000a041000003c20000a04171710003e72fb900381f3f"},
    Scenario{"mining", {3.0F, 12.0F, 3.0F}, degrees(60.0F), degrees(-35.0F), {}, 0.0F, 0.0F, true,
             false, false, false, false, false, PlayerAnimationMode::pick, "0000404000004041000040404ed4010177000000000000"},
    Scenario{"placing-block", {3.0F, 12.0F, 3.0F}, degrees(60.0F), degrees(-50.0F), {}, 0.0F, 0.0F,
             true, false, false, false, false, false, PlayerAnimationMode::block, "00004040000040410000404039d4010277000000000000"},
    Scenario{"rifle-hip-run", {3.0F, 12.0F, 3.0F}, degrees(300.0F), 0.0F, {-4.33F, 0.0F, -2.5F},
             0.0F, 1.0F, false, false, false, false, false, false, PlayerAnimationMode::assault,
             "000040400000404100004040802a0004e754c4000000c1"},
    Scenario{"rifle-shouldered", {3.0F, 12.0F, 3.0F}, degrees(300.0F), degrees(2.0F), {}, 0.0F,
             0.0F, false, false, true, false, false, false, PlayerAnimationMode::assault, "000040400000404100004040822a040477000000000000"},
    Scenario{"rifle-shouldered-fire", {3.0F, 12.0F, 3.0F}, degrees(300.0F), degrees(2.0F), {},
             0.0F, 0.0F, true, false, true, false, false, false, PlayerAnimationMode::assault, "000040400000404100004040822a050477000000000000"},
    Scenario{"rifle-reload", {3.0F, 12.0F, 3.0F}, degrees(300.0F), 0.0F, {}, 0.0F, 0.0F, false,
             false, false, true, false, false, PlayerAnimationMode::assault, "000040400000404100004040802a080477000000000000"},
    Scenario{"bolt-rifle-fire", {3.0F, 12.0F, 3.0F}, degrees(77.0F), 0.0F, {}, 0.0F, 0.0F, true,
             false, false, false, false, false, PlayerAnimationMode::bolt_rifle, "00004040000040410000404080c8010577000000000000"},
    Scenario{"pistol-fire", {3.0F, 12.0F, 3.0F}, degrees(77.0F), 0.0F, {}, 0.0F, 0.0F, true,
             false, false, false, false, false, PlayerAnimationMode::pistol, "00004040000040410000404080c8010677000000000000"},
    Scenario{"shotgun-shouldered", {3.0F, 12.0F, 3.0F}, degrees(77.0F), 0.0F, {}, 0.0F, 0.0F,
             false, false, true, false, false, false, PlayerAnimationMode::pump_shotgun, "00004040000040410000404080c8040777000000000000"},
    Scenario{"smg-reload", {3.0F, 12.0F, 3.0F}, degrees(77.0F), 0.0F, {}, 0.0F, 0.0F, false,
             false, false, true, false, false, PlayerAnimationMode::smg, "00004040000040410000404080c8080877000000000000"},
    Scenario{"lmg-fire", {3.0F, 12.0F, 3.0F}, degrees(77.0F), 0.0F, {}, 0.0F, 0.0F, true, false,
             false, false, false, false, PlayerAnimationMode::lmg, "00004040000040410000404080c8010977000000000000"},
    Scenario{"grenade-cook", {3.0F, 12.0F, 3.0F}, degrees(-10.0F), degrees(20.0F), {}, 0.0F, 0.0F,
             false, false, false, false, true, false, PlayerAnimationMode::generic, "0000404000004041000040409c07100077000000000000"},
    Scenario{"grenade-throw", {3.0F, 12.0F, 3.0F}, degrees(-10.0F), degrees(20.0F), {}, 0.0F,
             0.0F, false, false, false, false, true, true, PlayerAnimationMode::generic, "0000404000004041000040409c07300077000000000000"},
    Scenario{"grenade-thrown", {3.0F, 12.0F, 3.0F}, degrees(-10.0F), degrees(20.0F), {}, 0.0F,
             0.0F, false, false, false, false, false, true, PlayerAnimationMode::generic, "0000404000004041000040409c07200077000000000000"},
    Scenario{"dead", {3.0F, 12.0F, 3.0F}, degrees(-10.0F), degrees(-60.0F), {}, 0.0F, 0.0F, false,
             true, false, false, false, false, PlayerAnimationMode::fist, "0000404000004041000040402b07020377000000000000"},
};

std::string hex(std::span<const std::uint8_t> bytes)
{
    std::string text;
    for (const std::uint8_t byte : bytes)
        text += std::format("{:02x}", byte);
    return text;
}

std::string hex(std::string_view text)
{
    return hex(std::span{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

std::vector<std::uint8_t> unhex(std::string_view text)
{
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0U; index + 1U < text.size(); index += 2U)
        bytes.push_back(static_cast<std::uint8_t>(
            std::stoul(std::string{text.substr(index, 2U)}, nullptr, 16)));
    return bytes;
}

// What the application passes for this state (publish_local_player_update).
LocalPlayerReplicationInput replication_input(const Scenario &scenario)
{
    LocalPlayerReplicationInput input;
    input.position = scenario.position;
    input.velocity = scenario.velocity;
    input.yaw = scenario.yaw;
    input.torso_pitch = scenario.pitch;
    input.movement_x = scenario.strafe;
    input.movement_y = scenario.forward;
    input.animation_mode = scenario.mode;
    input.using_tool = scenario.using_tool;
    input.dead = scenario.dead;
    input.shouldering = scenario.shouldering;
    input.reloading = scenario.reloading;
    input.throwing_grenade = scenario.throwing_grenade;
    input.ready_to_throw_grenade = scenario.ready_to_throw_grenade;
    return input;
}

std::vector<std::uint8_t> our_bytes(const Scenario &scenario, ProtocolVersion protocol)
{
    PlayerUpdatePayload payload;
    PayloadError error{};
    check(make_player_update(replication_input(scenario), protocol, payload, error),
          "our sender refused a scripted state");
    const auto encoded = encode_player_update_payload(payload, protocol, error);
    check(encoded.has_value(), "our encoder refused a scripted state");
    return *encoded;
}

// The world-space direction our own simulation looks along at this yaw/pitch.
world::Vec3 view_direction(float yaw, float pitch)
{
    world::PlayerSimulation simulation;
    simulation.state().yaw = yaw;
    simulation.state().pitch = pitch;
    return simulation.view_direction();
}

struct Heading final
{
    float x{};
    float z{};
};

Heading heading(float yaw)
{
    const world::Vec3 level = view_direction(yaw, 0.0F);
    const float length = std::hypot(level.x, level.z);
    return {level.x / length, level.z / length};
}

float pitch_up_degrees(float yaw, float pitch)
{
    return std::asin(view_direction(yaw, pitch).y) * 180.0F / pi;
}

float heading_error_degrees(Heading left, Heading right)
{
    return std::acos(std::clamp(left.x * right.x + left.z * right.z, -1.0F, 1.0F)) * 180.0F / pi;
}

// One ordinary (not flying) simulation step: the horizontal velocity our
// player gets from a movement input.
world::Vec3 walk_velocity(float yaw, float strafe, float forward)
{
    const world::WorldRegion empty{0, 0, 1, 1};
    world::PlayerSimulation simulation;
    simulation.state().position = {8.0F, 100.0F, 8.0F};
    simulation.state().yaw = yaw;
    simulation.update(empty, {strafe, forward}, 1.0F / 60.0F, false);
    return simulation.state().velocity;
}

// "strafe" and "forward" mean what our simulation does with them: forward
// moves along the heading, strafe along the heading turned a quarter clockwise
// seen from above (the player's right).
void check_movement_axes()
{
    const world::WorldRegion empty{0, 0, 1, 1};
    for (const float yaw : {0.0F, 0.7F, -2.4F, 3.0F})
    {
        const Heading facing = heading(yaw);
        const Heading right{-facing.z, facing.x};
        for (const auto &[strafe, forward] :
             std::array{std::pair{1.0F, 0.0F}, std::pair{0.0F, 1.0F}, std::pair{-1.0F, 0.0F},
                        std::pair{0.0F, -1.0F}})
        {
            world::PlayerSimulation simulation;
            simulation.state().position = {8.0F, 100.0F, 8.0F};
            simulation.state().yaw = yaw;
            // The Free flight model moves along the pure axes; the original's
            // flight adds the stroll formula's backward drift to a strafe.
            simulation.set_flight_model(world::PlayerSimulation::FlightModel::free);
            simulation.state().fly_mode = true;
            simulation.update(empty, {strafe, forward}, 1.0F / 60.0F, false);
            const world::Vec3 velocity = simulation.state().velocity;
            const float along = velocity.x * facing.x + velocity.z * facing.z;
            const float across = velocity.x * right.x + velocity.z * right.z;
            check((strafe > 0.0F && across > 1.0F) || (strafe < 0.0F && across < -1.0F) ||
                      (strafe == 0.0F && std::abs(across) < 1e-3F),
                  "our strafe axis does not move to the player's right");
            check((forward > 0.0F && along > 1.0F) || (forward < 0.0F && along < -1.0F) ||
                      (forward == 0.0F && std::abs(along) < 1e-3F),
                  "our forward axis does not move along the heading");
        }
    }
}

// Decode a PlayerUpdate payload with OUR receiver and require the meaning the
// scenario was written with. Tolerances are the wire's own quantisation.
void check_meaning(const Scenario &scenario, std::span<const std::uint8_t> bytes,
                   ProtocolVersion protocol, std::string_view source)
{
    const auto fail = [&](std::string_view what) {
        return std::format("{} {}: {}", source, scenario.name, what);
    };
    PlayerUpdatePayload payload;
    PayloadError error{};
    check(decode_player_update_payload(bytes, protocol, payload, error),
          fail("payload does not decode"));
    RemotePlayerReplicationState state;
    check(apply_player_update(payload, protocol, state), fail("receiver refused the payload"));
    check(state.position.x == scenario.position.x && state.position.y == scenario.position.y &&
              state.position.z == scenario.position.z,
          fail("feet position"));
    // 255 steps per turn: half a step is 0.706 degrees.
    check(heading_error_degrees(heading(state.yaw), heading(scenario.yaw)) <= 0.72F,
          fail("facing"));
    check(state.yaw > -pi - 1e-4F && state.yaw <= pi + 1e-4F, fail("yaw range"));
    // 255 steps across 180 degrees: half a step is 0.353 degrees.
    check(std::abs(pitch_up_degrees(state.yaw, state.torso_pitch) -
                   pitch_up_degrees(scenario.yaw, scenario.pitch)) <= 0.36F,
          fail("torso pitch"));
    const auto close = [](float left, float right) {
        return std::abs(left - right) <= 0.001F * (1.0F + std::abs(right));
    };
    check(close(state.velocity.x, scenario.velocity.x) &&
              close(state.velocity.y, scenario.velocity.y) &&
              close(state.velocity.z, scenario.velocity.z),
          fail("world velocity"));
    // Fifteen truncated levels per axis.
    const auto level = [](float sent, float meant) {
        return sent <= meant + 1e-4F && sent >= meant - 1.0F / 7.0F - 1e-4F;
    };
    check(level(state.movement_x, scenario.strafe) && level(state.movement_y, scenario.forward),
          fail("movement right/forward"));
    check(state.using_tool == scenario.using_tool, fail("using"));
    check(state.dead == scenario.dead, fail("dead"));
    check(state.shouldering == scenario.shouldering, fail("shouldering"));
    check(state.reloading == scenario.reloading, fail("reloading"));
    check(state.throwing_grenade == scenario.throwing_grenade, fail("throwing grenade"));
    check(state.ready_to_throw_grenade == scenario.ready_to_throw_grenade,
          fail("ready to throw grenade"));
    check(state.animation_mode == scenario.mode, fail("player mode"));
}

struct RosterFixture final
{
    std::vector<std::uint8_t> connected_response;
    std::uint8_t last_gid{};
    std::map<std::uint8_t, std::string> listed;
    std::vector<std::pair<std::vector<std::uint8_t>, std::pair<std::uint8_t, std::string>>>
        new_peers;
};

constexpr std::string_view cyrillic_tag = "\xD0\xA2\xD0\xBE\xD0\xB4\xD0\xBE\xD1\x80";
constexpr std::string_view accented_tag = "Jos\xC3\xA9 M\xC3\xBCller";
constexpr std::string_view kanji_tag = "\xE5\x90\x8D\xE5\x89\x8D";

// Run OUR host through real admissions and keep exactly what it sends: the
// roster each joiner is handed and the NewPeer every earlier player receives.
RosterFixture roster_fixture(ProtocolVersion protocol)
{
    LegacySteamHostSessionOptions options;
    options.settings.protocol = protocol;
    options.settings.mod_manifest.protocol = protocol;
    options.settings.visibility = HostVisibility::public_lobby;
    options.host_gamer = {{9U, 9U}, "KikoTs"};
    LegacySteamHostSession host{options};
    RosterFixture fixture;
    fixture.listed[0U] = "KikoTs";
    PeerId peer = 901U;
    // The second "KikoTs" collides with the host and the second "Stock" with
    // an earlier guest; a stock host would refuse both (GamerAlreadyConnected).
    for (const std::string_view requested :
         {std::string_view{"Stock"}, std::string_view{"KikoTs"}, std::string_view{"Stock"},
          accented_tag, cyrillic_tag, kanji_tag})
    {
        LegacySteamClientSessionOptions client_options;
        client_options.protocol = protocol;
        client_options.host_peer = 900U;
        client_options.expected_host_name = "KikoTs";
        client_options.session_properties = host.session_properties();
        client_options.local_gamer = {{static_cast<std::uint8_t>(peer), 1U},
                                      std::string{requested}};
        LegacySteamClientSession client{std::move(client_options)};
        std::string error;
        const auto request = client.begin(error);
        check(request.has_value(), error);
        const auto admission = host.process_wire_packet(peer, request->payload);
        check(!admission.audit && admission.inbound.size() == 1U, "our host refused a player");
        const std::uint8_t gid = *host.gid_for(peer);
        const LegacyGamerIdentity *listed = host.gamer_for_gid(gid);
        check(listed != nullptr && !listed->gamer_tag.empty(), "our host listed an empty tag");
        for (const auto &send : admission.sends)
        {
            LegacySteamWirePacket packet;
            LegacySteamCodecError codec_error{};
            check(decode_legacy_steam_wire(send.payload, packet, codec_error),
                  "our host sent an undecodable wire packet");
            if (packet.message_type != SteamIncomingMessageType::data || packet.channel != 1U)
                continue;
            if (send.remote_peer == peer)
                fixture.connected_response = packet.payload;
            else if (fixture.new_peers.empty() || fixture.new_peers.back().second.first != gid)
                fixture.new_peers.push_back({packet.payload, {gid, listed->gamer_tag}});
        }
        fixture.last_gid = gid;
        // The roster handed to a joiner lists everyone ALREADY present.
        if (peer != 906U)
            fixture.listed[gid] = listed->gamer_tag;
        ++peer;
    }
    return fixture;
}

void check_roster_names()
{
    const RosterFixture fixture = roster_fixture(ProtocolVersion::classic_v3);
    std::vector<std::string> names;
    for (const auto &[gid, name] : fixture.listed)
        names.push_back(name);
    check(names == std::vector<std::string>{"KikoTs", "Stock", "KikoTs (2)", "Stock (2)",
                                            std::string{accented_tag}, std::string{cyrillic_tag}},
          "our host's roster names differ from the pinned set");
    LegacyConnectedResponse response;
    LegacySteamCodecError error{};
    check(decode_legacy_connected_response(fixture.connected_response, response, error) &&
              response.peers.size() == 6U && response.peer_gids.size() == 6U,
          "the joiner's roster does not list every player already present");
    for (const LegacyGamerIdentity &gamer : response.peers)
        check(!gamer.gamer_tag.empty(), "a roster entry has an empty gamertag");
    check(fixture.new_peers.size() == 5U && fixture.new_peers.back().second.second == kanji_tag,
          "earlier players were not told the newcomer's gamertag");
}

struct NametagCase final
{
    std::string_view name;
    world::Vec3 feet;
    float yaw{};
    float pitch{};
    float fov{};
    float width{};
    float height{};
    world::Vec3 target;
};

const std::array nametag_cases{
    NametagCase{"ahead", {8.0F, 30.0F, -8.0F}, 0.0F, 0.0F, 73.0F, 1280.0F, 720.0F,
                {8.0F, 30.0F, -12.0F}},
    NametagCase{"right-and-below", {8.0F, 30.0F, -8.0F}, 0.6F, -0.2F, 73.0F, 1920.0F, 1080.0F,
                {14.0F, 27.5F, -15.0F}},
    NametagCase{"left-and-above", {-20.5F, 12.0F, 40.25F}, -2.2F, 0.35F, 90.0F, 2560.0F,
                1440.0F, {-31.0F, 19.0F, 47.0F}},
    NametagCase{"far", {0.0F, 0.0F, 0.0F}, 1.2F, 0.05F, 73.0F, 1280.0F, 720.0F,
                {180.0F, 6.0F, -70.0F}},
    NametagCase{"point-blank", {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 73.0F, 1280.0F, 720.0F,
                {0.0F, 0.0F, -0.4F}},
    NametagCase{"behind", {8.0F, 30.0F, -8.0F}, 0.0F, 0.0F, 73.0F, 1280.0F, 720.0F,
                {8.0F, 30.0F, 4.0F}},
    NametagCase{"behind-looking-up", {8.0F, 30.0F, -8.0F}, 2.0F, 1.2F, 73.0F, 1280.0F, 720.0F,
                {2.0F, 30.0F, -9.0F}},
};

NametagCamera nametag_camera(const NametagCase &test)
{
    return {{test.feet.x, test.feet.y + world::PlayerSimulation::eye_height, test.feet.z},
            test.yaw, test.pitch, test.fov, test.width, test.height, true};
}

void check_nametag_layout()
{
    // The tag sits two blocks above the feet and is centred on that point.
    const NametagCase &ahead = nametag_cases[0];
    const auto anchor = project_nametag(nametag_camera(ahead), ahead.target);
    check(anchor.has_value() && std::abs(anchor->x - 640.0F) < 0.01F, "nametag is not centred");
    // Eye 1.4 above the feet, tag 2.0 above the other feet, four blocks away.
    const float focal = 360.0F / std::tan(degrees(73.0F) * 0.5F);
    check(std::abs(anchor->y - (360.0F - 0.6F * focal / 4.0F)) < 0.01F,
          "nametag is not two blocks above the feet");
    check(!project_nametag(nametag_camera(nametag_cases[5]), nametag_cases[5].target),
          "a player behind the camera got a nametag");
    // No distance scale or cut-off: a far player still has an anchor.
    check(project_nametag(nametag_camera(nametag_cases[3]), nametag_cases[3].target).has_value(),
          "a distant player lost its nametag");
    NametagCamera invalid = nametag_camera(ahead);
    invalid.valid = false;
    check(!project_nametag(invalid, ahead.target), "an invalid camera projected a nametag");
    const NametagAnchor origin = nametag_text_origin({100.0F, 50.0F}, 60.0F, 20.0F);
    check(origin.x == 70.0F && origin.y == 40.0F, "nametag text is not centred on its anchor");
    // DrawOutlinedText(width 1): four diagonal outline copies, fill last.
    check(nametag_passes.size() == 5U && !nametag_passes.back().outline &&
              nametag_passes.back().dx == 0.0F && nametag_passes.back().dy == 0.0F,
          "nametag fill pass must be drawn last");
    for (std::size_t index = 0U; index + 1U < nametag_passes.size(); ++index)
        check(nametag_passes[index].outline && std::abs(nametag_passes[index].dx) == 1.0F &&
                  std::abs(nametag_passes[index].dy) == 1.0F,
              "nametag outline is not the original one-pixel diagonal set");
    // Shipped NameTagFont metrics (probe: FONT NameTagFont ... 'H'=12x14@top5,
    // measure(H)=15x27 on Classic; 'H'=13x15@top4 on Modern).
    check(nametag_height_above_feet == 2.0F && classic_nametag_font.cell_height == 27 &&
              classic_nametag_font.cap_height == 14 && modern_nametag_font.cap_height == 15 &&
              nametag_baseline_below_anchor(classic_nametag_font) == 5.5F &&
              nametag_baseline_below_anchor(modern_nametag_font) == 5.5F,
          "nametag constants changed");
}

// The grenade bits over time, as Player.UpdateAnimation sequences them.
void check_grenade_arm()
{
    constexpr float tick = 1.0F / 60.0F;
    const auto run = [&](GrenadeArm &arm, float seconds) {
        // Returns the time at which the grenade left the hand, or -1.
        float left_at = -1.0F;
        for (float elapsed = 0.0F; elapsed < seconds; elapsed += tick)
            if (arm.advance(tick) && left_at < 0.0F)
                left_at = elapsed + tick;
        return left_at;
    };
    // Quick tap: the cook clip still plays out, then the swing, then the throw.
    GrenadeArm tap;
    check(!tap.playing() && !tap.ready(), "an idle arm sets grenade bits");
    tap.press();
    check(tap.playing() && !tap.ready(), "press must set PlayGrenadeAnim only");
    check(run(tap, 0.2F) < 0.0F, "the grenade left while Use was held");
    tap.release();
    check(tap.playing() && tap.ready() && !tap.swinging(), "release must set ReadyToThrow");
    const float tapped = run(tap, 3.0F);
    const float expected = GrenadeArm::cook_clip_seconds + GrenadeArm::throw_clip_seconds - 0.2F;
    check(std::abs(tapped - expected) <= 3.0F * tick,
          "a tapped grenade must leave after the cook and throw clips");
    // The fuse only burns while Use is held.
    check(std::abs(tap.cook_seconds() - 0.2F) <= 2.0F * tick, "cook time ran after release");
    // After the throw the ready bit stays set, as the original keeps sending.
    check(!tap.playing() && tap.ready(), "ReadyToThrow must stay latched after a throw");
    tap.press();
    check(tap.playing() && !tap.ready() && tap.cook_seconds() == 0.0F,
          "the next press must clear ReadyToThrow and the cook time");

    // Held past the cook clip: the swing starts on release.
    GrenadeArm held;
    held.press();
    check(run(held, 2.0F) < 0.0F && !held.swinging(), "a held grenade swung by itself");
    held.release();
    check(std::abs(run(held, 1.0F) - GrenadeArm::throw_clip_seconds) <= 3.0F * tick,
          "a held grenade must leave one throw clip after release");

    // Never released: armed at four seconds, thrown one swing later.
    GrenadeArm forgotten;
    forgotten.press();
    const float automatic = run(forgotten, 6.0F);
    check(std::abs(automatic -
                   (GrenadeArm::maximum_cook_seconds + GrenadeArm::throw_clip_seconds)) <=
              3.0F * tick,
          "a cooked-out grenade must be thrown automatically");
    check(std::abs(forgotten.cook_seconds() - GrenadeArm::maximum_cook_seconds) <= 2.0F * tick,
          "cook time must stop at four seconds");

    // Death clears both bits; putting the grenade away throws nothing.
    GrenadeArm killed;
    killed.press();
    killed.release();
    killed.kill();
    check(!killed.playing() && !killed.ready() && run(killed, 3.0F) < 0.0F,
          "a dead player still threw or still advertised a grenade");
    GrenadeArm cancelled;
    cancelled.press();
    cancelled.cancel();
    check(!cancelled.playing() && run(cancelled, 3.0F) < 0.0F, "a cancelled grenade was thrown");
}

std::string floats(std::initializer_list<float> values)
{
    std::string text;
    for (const float value : values)
        text += (text.empty() ? "" : " ") + std::format("{}", value);
    return text;
}

void write_fixtures(const std::filesystem::path &root)
{
    for (const auto protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
    {
        const auto directory = root / std::to_string(static_cast<int>(protocol));
        std::filesystem::create_directories(directory);
        std::ofstream updates{directory / "player_updates.txt", std::ios::binary};
        for (const Scenario &scenario : scenarios)
        {
            const Heading facing = heading(scenario.yaw);
            // Where OUR simulation walks for this input at this heading.
            const world::Vec3 walked =
                walk_velocity(scenario.yaw, scenario.strafe, scenario.forward);
            updates << scenario.name << '|'
                    << floats({scenario.position.x, scenario.position.y, scenario.position.z})
                    << '|' << floats({facing.x, facing.z}) << '|'
                    << floats({pitch_up_degrees(scenario.yaw, scenario.pitch)}) << '|'
                    << floats({scenario.velocity.x, scenario.velocity.y, scenario.velocity.z})
                    << '|' << floats({scenario.strafe, scenario.forward}) << '|'
                    << (scenario.using_tool ? '1' : '0') << (scenario.dead ? '1' : '0')
                    << (scenario.shouldering ? '1' : '0') << (scenario.reloading ? '1' : '0')
                    << (scenario.throwing_grenade ? '1' : '0')
                    << (scenario.ready_to_throw_grenade ? '1' : '0') << '|'
                    << static_cast<int>(scenario.mode) << '|' << hex(our_bytes(scenario, protocol))
                    << '|' << floats({walked.x, walked.z}) << '|' << floats({scenario.pitch})
                    << '\n';
        }
        check(static_cast<bool>(updates), "player update fixture export failed");

        // Every heading and pitch our sender can be asked to pack.
        std::ofstream sweep{directory / "angle_sweep.txt", std::ios::binary};
        constexpr int sweep_steps = 6000;
        for (int step = 0; step <= sweep_steps; ++step)
        {
            const float fraction = static_cast<float>(step) / static_cast<float>(sweep_steps);
            Scenario probe = scenarios[0];
            probe.yaw = -pi + 2.0F * pi * fraction;
            probe.pitch = 0.0F;
            const Heading facing = heading(probe.yaw);
            sweep << "yaw|" << floats({probe.yaw}) << '|' << floats({facing.x, facing.z}) << '|'
                  << static_cast<int>(our_bytes(probe, protocol)[13]) << '\n';
            probe.yaw = 0.0F;
            probe.pitch = degrees(-89.0F + 178.0F * fraction);
            sweep << "pitch|" << floats({probe.pitch}) << "|-|"
                  << static_cast<int>(our_bytes(probe, protocol)[12]) << '\n';
        }

        std::ofstream exists{directory / "player_exists.txt", std::ios::binary};
        int index = 0;
        for (const std::string_view tag :
             {std::string_view{"KikoTs"}, std::string_view{"KikoTs (2)"}, accented_tag,
              cyrillic_tag, kanji_tag})
        {
            PlayerExistsPayload presence;
            presence.request_response = index % 2 == 0;
            // CastleMinerZGame.SetupNewGamer: new AvatarDescription(new byte[10]).
            presence.avatar_description.assign(10U, 0U);
            presence.gamer_tag = std::string{tag};
            presence.player_id = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
                                  9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U};
            PayloadError error{};
            const auto payload = encode_player_exists_payload(presence, error);
            check(payload.has_value(), "player exists fixture export failed");
            exists << "exists-" << index << '|' << (presence.request_response ? '1' : '0') << '|'
                   << hex(tag) << '|' << hex(presence.player_id) << '|'
                   << hex(presence.avatar_description) << '|' << hex(*payload) << '\n';
            ++index;
        }

        const RosterFixture fixture = roster_fixture(protocol);
        std::ofstream roster{directory / "roster.txt", std::ios::binary};
        roster << "connected|" << hex(fixture.connected_response) << '|'
               << static_cast<int>(fixture.last_gid) << '|';
        for (const auto &[gid, name] : fixture.listed)
            roster << static_cast<int>(gid) << ':' << hex(name) << ',';
        roster << '\n';
        for (const auto &[payload, entry] : fixture.new_peers)
            roster << "newpeer|" << hex(payload) << "|-|" << static_cast<int>(entry.first) << ':'
                   << hex(entry.second) << '\n';

        std::ofstream nametags{directory / "nametag.txt", std::ios::binary};
        for (const NametagCase &test : nametag_cases)
        {
            const Heading facing = heading(test.yaw);
            const auto anchor = project_nametag(nametag_camera(test), test.target);
            nametags << test.name << '|' << floats({test.feet.x, test.feet.y, test.feet.z}) << '|'
                     << floats({facing.x, facing.z}) << '|'
                     << floats({pitch_up_degrees(test.yaw, test.pitch)}) << '|'
                     << floats({test.fov}) << '|' << floats({test.width, test.height}) << '|'
                     << floats({test.target.x, test.target.y, test.target.z}) << '|'
                     << (anchor.has_value() ? '1' : '0') << '|'
                     << floats({anchor.has_value() ? anchor->x : 0.0F,
                                anchor.has_value() ? anchor->y : 0.0F})
                     << '\n';
        }
    }
}

std::map<std::string, std::string> read_pairs(const std::filesystem::path &path)
{
    std::ifstream input{path, std::ios::binary};
    check(static_cast<bool>(input), "original probe output is missing: " + path.string());
    std::map<std::string, std::string> pairs;
    for (std::string line; std::getline(input, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto bar = line.find('|');
        if (bar != std::string::npos)
            pairs[line.substr(0U, bar)] = line.substr(bar + 1U);
    }
    return pairs;
}

void check_original(const std::filesystem::path &output, ProtocolVersion protocol)
{
    const auto updates = read_pairs(output / "original_player_updates.txt");
    for (const Scenario &scenario : scenarios)
    {
        const auto found = updates.find(std::string{scenario.name});
        check(found != updates.end(), "the original sender produced no bytes for a state");
        check_meaning(scenario, unhex(found->second), protocol, "original->ours");
        if (!scenario.original.empty())
            check(found->second == scenario.original,
                  std::format("pinned original bytes for {} are stale", scenario.name));
    }
    // The original's PlayerExists must decode to the same identity with ours.
    for (const auto &[name, payload_hex] : read_pairs(output / "original_player_exists.txt"))
    {
        PlayerExistsPayload presence;
        PayloadError error{};
        const auto bytes = unhex(payload_hex);
        check(decode_player_exists_payload(bytes, presence, error) && !presence.gamer_tag.empty(),
              "original PlayerExists " + name + " did not decode to a named player");
        check(presence.avatar_description.size() == 10U,
              "original PlayerExists avatar description is not the stock ten bytes");
    }
    // The nametag constants must be the shipped font's own numbers.
    const NametagFontMetrics &expected =
        protocol == ProtocolVersion::modern_v4 ? modern_nametag_font : classic_nametag_font;
    bool font_seen = false;
    std::ifstream fonts{output / "original_fonts.txt", std::ios::binary};
    for (std::string line; std::getline(fonts, line);)
    {
        if (!line.starts_with("font|NameTagFont|"))
            continue;
        std::vector<std::string> field;
        std::istringstream stream{line};
        for (std::string part; std::getline(stream, part, '|');)
            field.push_back(part);
        check(field.size() >= 9U && std::stoi(field[6]) == expected.cell_height &&
                  std::stoi(field[7]) == expected.cap_height &&
                  std::stoi(field[8]) == expected.cap_top,
              "nametag font constants differ from the shipped NameTagFont");
        // Unknown glyphs fall back to '*': an odd name can never throw in the
        // stock client's nametag draw.
        check(std::stoi(field[5]) == '*', "shipped NameTagFont has no default character");
        font_seen = true;
    }
    check(font_seen, "the probe did not report the shipped NameTagFont");
    std::cout << "original->ours: " << scenarios.size() << " player states agree\n";
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 3 && std::string_view{argv[1]} == "--write-fixtures")
        {
            write_fixtures(argv[2]);
            return 0;
        }
        if (argc == 4 && std::string_view{argv[1]} == "--check-original")
        {
            check_original(argv[2], static_cast<ProtocolVersion>(std::stoi(argv[3])));
            return 0;
        }
        check_movement_axes();
        check_grenade_arm();
        check_nametag_layout();
        check_roster_names();
        for (const auto protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
            for (const Scenario &scenario : scenarios)
            {
                // Our own round trip, and the recorded original sender's bytes.
                const auto ours = our_bytes(scenario, protocol);
                check_meaning(scenario, ours, protocol, "ours->ours");
                check(!scenario.original.empty(), "a state has no recorded original bytes");
                const auto recorded = unhex(scenario.original);
                check_meaning(scenario, recorded, protocol, "recorded original->ours");
                // Our sender must produce the original sender's bytes. Yaw is
                // the one field the original derives through a quaternion, so
                // a heading that sits exactly between two wire steps (180
                // degrees is 127.5) may land on either neighbour.
                check(ours.size() == recorded.size(), "our payload length differs");
                for (std::size_t index = 0U; index < ours.size(); ++index)
                    check(ours[index] == recorded[index] ||
                              (index == 13U && std::abs(static_cast<int>(ours[index]) -
                                                        static_cast<int>(recorded[index])) == 1),
                          std::format("our sender's byte {} differs from the original for {}",
                                      index, scenario.name));
            }
        // -60 degrees of pitch is 42.5 wire steps in single precision and just
        // above it in the original's double-precision product: 43, not 42.
        check(our_bytes(scenarios.back(), ProtocolVersion::classic_v3)[12] == 0x2BU,
              "pitch packing no longer rounds like the original");
        std::cout << "player parity: " << scenarios.size() << " states, "
                  << nametag_cases.size() << " nametag cameras\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "player parity failed: " << error.what() << '\n';
        return 1;
    }
}
