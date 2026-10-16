// Steady-state multiplayer latency and smoothness harness.
//
// Everything here runs the production stock session classes
// (`LegacySteamHostSession`, `LegacySteamClientSession`) and the production
// replication/presentation code through a deterministic simulated link, on a
// simulated clock. It measures what a second machine would otherwise be needed
// to see: end-to-end latency from a state change on one peer to its
// application on another through the host relay, remote-player visual
// smoothness, per-message packet/byte rates, link queue depth, and the cost of
// a host-side main-thread hitch.
//
// The one real-process measurement is `bridge_ipc_latency`: it launches the
// supervised x86 helper in `--protocol-test` mode (no Steam, no lobby) and
// measures the actual named-pipe round trip and frame throughput.

#include "state_digest.hpp"

#include "openminerz/game_mode.hpp"
#include "openminerz/net/client_gameplay_authority.hpp"
#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/player_replication.hpp"
#include "openminerz/net/steam_bridge_client.hpp"
#include "openminerz/world/sky.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
using namespace openminerz::net;
using openminerz::test::PlayerFact;
using openminerz::test::StateDigest;
using openminerz::test::StateDomain;

int failures = 0;

void expect(bool value, std::string_view message)
{
    if (value)
        return;
    ++failures;
    std::cout << "[FAIL] " << message << '\n';
}

[[noreturn]] void fail(std::string_view message)
{
    throw std::runtime_error{std::string{message}};
}

// ---------------------------------------------------------------- link model

struct LinkProfile final
{
    double delay_ms{};
    double jitter_ms{};
    double loss{};
    // Fraction of packets held back by a further `delay_ms`, which forces them
    // behind later traffic instead of merely arriving late.
    double reorder{};
};

struct LinkPacket final
{
    LegacySteamNativeSend send;
    // When the originating peer produced the application state this carries.
    double origin_ms{};
};

class SimulatedLink final
{
  public:
    SimulatedLink(LinkProfile profile, std::uint32_t seed) : profile_{profile}, random_{seed}
    {
    }

    void push(double now_ms, LinkPacket packet)
    {
        ++offered_;
        offered_bytes_ += packet.send.payload.size();
        if (profile_.loss > 0.0 && uniform() < profile_.loss)
        {
            ++dropped_;
            return;
        }
        double arrival = now_ms + profile_.delay_ms;
        if (profile_.jitter_ms > 0.0)
            arrival += (uniform() * 2.0 - 1.0) * profile_.jitter_ms;
        if (profile_.reorder > 0.0 && uniform() < profile_.reorder)
            arrival += profile_.delay_ms;
        arrival = std::max(arrival, now_ms);
        queue_.push_back({arrival, std::move(packet)});
        peak_depth_ = std::max(peak_depth_, queue_.size());
    }

    // Arrival order, not send order: jitter and the reorder knob decide it.
    [[nodiscard]] std::vector<LinkPacket> drain(double now_ms)
    {
        std::vector<LinkPacket> ready;
        std::ranges::stable_sort(queue_, {}, &Entry::arrival_ms);
        const auto split = std::ranges::find_if(
            queue_, [&](const Entry &entry) { return entry.arrival_ms > now_ms; });
        for (auto entry = queue_.begin(); entry != split; ++entry)
            ready.push_back(std::move(entry->packet));
        queue_.erase(queue_.begin(), split);
        return ready;
    }

    [[nodiscard]] std::size_t offered() const noexcept
    {
        return offered_;
    }
    [[nodiscard]] std::size_t dropped() const noexcept
    {
        return dropped_;
    }
    [[nodiscard]] std::size_t offered_bytes() const noexcept
    {
        return offered_bytes_;
    }
    [[nodiscard]] std::size_t peak_depth() const noexcept
    {
        return peak_depth_;
    }

  private:
    struct Entry final
    {
        double arrival_ms{};
        LinkPacket packet;
    };

    [[nodiscard]] double uniform()
    {
        return std::uniform_real_distribution<double>{0.0, 1.0}(random_);
    }

    LinkProfile profile_;
    std::mt19937 random_;
    std::vector<Entry> queue_;
    std::size_t offered_{};
    std::size_t dropped_{};
    std::size_t offered_bytes_{};
    std::size_t peak_depth_{};
};

// ------------------------------------------------------------- measurements

struct Statistics final
{
    double mean{};
    double deviation{};
    double p50{};
    double p95{};
    double maximum{};
};

[[nodiscard]] Statistics summarize(std::vector<double> values)
{
    Statistics result;
    if (values.empty())
        return result;
    std::ranges::sort(values);
    double total{};
    for (const double value : values)
        total += value;
    result.mean = total / static_cast<double>(values.size());
    double variance{};
    for (const double value : values)
        variance += (value - result.mean) * (value - result.mean);
    result.deviation = std::sqrt(variance / static_cast<double>(values.size()));
    result.p50 = values[values.size() / 2U];
    result.p95 = values[static_cast<std::size_t>(static_cast<double>(values.size() - 1U) * 0.95)];
    result.maximum = values.back();
    return result;
}

struct Scenario final
{
    std::string name;
    LinkProfile link;
    double duration_ms{20'000.0};
    double host_frame_ms{1000.0 / 60.0};
    double client_frame_ms{1000.0 / 60.0};
    double send_interval_ms{50.0};
    // A periodic host main-thread stall (a synchronous autosave, a meshing
    // spike): the host neither pumps nor relays for `hitch_ms`.
    double hitch_ms{};
    double hitch_period_ms{};
    bool smoothing{true};
};

struct ScenarioResult final
{
    Statistics latency;        // milliseconds, send on A to application on B
    Statistics step;           // metres of presented movement per render frame
    Statistics tracking_error; // metres behind A's true position
    double applied_fraction{};
    double frozen_frame_fraction{};
    double messages_per_second{};
    double bytes_per_second{};
    std::size_t peak_queue_depth{};
};

constexpr PeerId host_peer = 900U;
constexpr PeerId alice_peer = 901U;
constexpr PeerId bob_peer = 902U;
constexpr float walk_speed = 4.317F; // The original walking speed, metres/second.

[[nodiscard]] LegacySteamHostSessionOptions host_options(ProtocolVersion protocol)
{
    LegacySteamHostSessionOptions options;
    options.settings.protocol = protocol;
    options.settings.mod_manifest.protocol = protocol;
    options.settings.visibility = HostVisibility::public_lobby;
    options.host_gamer = {{9U, 9U}, "Host"};
    return options;
}

struct Peers final
{
    ProtocolVersion protocol;
    LegacySteamHostSession host;
    std::map<PeerId, LegacySteamClientSession> clients;

    explicit Peers(ProtocolVersion selected) : protocol{selected}, host{host_options(selected)}
    {
        join(alice_peer, "Alice");
        join(bob_peer, "Bob");
    }

    void join(PeerId peer, std::string name)
    {
        LegacySteamClientSessionOptions options;
        options.protocol = protocol;
        options.host_peer = host_peer;
        options.expected_host_name = "Host";
        options.session_properties = host.session_properties();
        options.local_gamer = {{static_cast<std::uint8_t>(peer), 1U}, std::move(name)};
        auto &client = clients.emplace(peer, std::move(options)).first->second;
        std::string error;
        const auto request = client.begin(error);
        if (!request.has_value())
            fail("the harness could not build a stock admission request");
        const auto admission = host.process_wire_packet(peer, request->payload);
        if (admission.audit.has_value() || admission.inbound.size() != 1U)
            fail("the harness could not admit a peer through the production host session");
        for (const LegacySteamNativeSend &send : admission.sends)
        {
            const auto delivered = clients.at(send.remote_peer)
                                       .process_native_packet(host_peer, send.p2p_channel,
                                                              send.payload);
            if (delivered.audit.has_value())
                fail("a client refused its own admission roster");
        }
        if (!client.connected())
            fail("the harness peer did not reach the connected state");
    }
};

[[nodiscard]] std::vector<std::uint8_t> player_update_packet(ProtocolVersion protocol,
                                                             world::Vec3 position,
                                                             world::Vec3 velocity, float yaw)
{
    LocalPlayerReplicationInput input;
    input.position = position;
    input.velocity = velocity;
    input.yaw = yaw;
    input.movement_y = 1.0F;
    PlayerUpdatePayload update;
    PayloadError payload_error{};
    if (!make_player_update(input, protocol, update, payload_error))
        fail("the harness could not build a stock PlayerUpdate");
    const auto payload = encode_player_update_payload(update, protocol, payload_error);
    const auto *descriptor = message_by_name(protocol, "PlayerUpdateMessage");
    PacketError packet_error{};
    if (!payload.has_value() || descriptor == nullptr)
        fail("PlayerUpdateMessage is absent from the stock registry");
    auto packet = encode_packet(protocol, descriptor->id, *payload, packet_error);
    if (!packet.has_value())
        fail("the harness could not frame a stock PlayerUpdate");
    return std::move(*packet);
}

// Drives Alice walking at a constant speed, relayed by the production host
// session to Bob, and samples what Bob would draw every render frame.
[[nodiscard]] ScenarioResult run_scenario(ProtocolVersion protocol, const Scenario &scenario)
{
    Peers peers{protocol};
    const std::uint8_t alice_gid = peers.host.gid_for(alice_peer).value_or(0U);
    const std::uint8_t bob_gid = peers.host.gid_for(bob_peer).value_or(0U);
    if (alice_gid == 0U || bob_gid == 0U)
        fail("the harness peers were not assigned stock GIDs");

    SimulatedLink uplink{scenario.link, 0x5EEDU};
    SimulatedLink downlink{scenario.link, 0xC0FFEEU};

    RemotePlayerPresentation presentation;
    RemotePlayerReplicationState applied;
    bool has_applied{};

    std::vector<double> latencies;
    std::vector<double> steps;
    std::vector<double> errors;
    std::size_t sent{};
    std::size_t received{};
    std::size_t frozen_frames{};
    std::optional<double> previous_presented;

    double next_send = 0.0;
    double next_host_frame = 0.0;
    double next_client_frame = 0.0;
    double next_hitch = scenario.hitch_period_ms > 0.0 ? scenario.hitch_period_ms : -1.0;
    double host_blocked_until = -1.0;

    const auto truth = [&](double now_ms) {
        return static_cast<double>(walk_speed) * now_ms / 1000.0;
    };

    for (double now = 0.0; now <= scenario.duration_ms; now += 1.0)
    {
        if (next_hitch >= 0.0 && now >= next_hitch)
        {
            host_blocked_until = now + scenario.hitch_ms;
            next_hitch += scenario.hitch_period_ms;
        }
        if (now >= next_send)
        {
            next_send += scenario.send_interval_ms;
            const world::Vec3 position{static_cast<float>(truth(now)), 2.0F, 0.0F};
            const world::Vec3 velocity{walk_speed, 0.0F, 0.0F};
            const auto packet = player_update_packet(protocol, position, velocity, 0.0F);
            std::string error;
            const auto wire = peers.clients.at(alice_peer).create_outbound(
                std::nullopt, packet, MessageDeliveryPolicy::unreliable_sequenced, error);
            if (!wire.has_value())
                fail("the production client session refused a stock broadcast");
            uplink.push(now, LinkPacket{*wire, now});
            ++sent;
        }
        if (now >= next_host_frame)
        {
            next_host_frame += scenario.host_frame_ms;
            if (now >= host_blocked_until)
            {
                for (LinkPacket &arrived : uplink.drain(now))
                {
                    const auto result =
                        peers.host.process_wire_packet(alice_peer, arrived.send.payload);
                    if (result.audit.has_value())
                        fail("the production host session rejected a relayed stock packet");
                    for (const LegacySteamHostInbound &inbound : result.inbound)
                    {
                        LegacySteamOutbound relay;
                        relay.application_packet = inbound.payload;
                        relay.delivery = inbound.delivery;
                        relay.channel = inbound.channel;
                        relay.sender_gid = inbound.sender_gid;
                        relay.recipient_gid = inbound.recipient_gid;
                        relay.client_relay = true;
                        std::string error;
                        const auto sends = peers.host.create_outbound(relay, error);
                        if (!sends.has_value())
                            fail("the production host session could not relay a stock packet");
                        for (const LegacySteamNativeSend &send : *sends)
                        {
                            if (send.remote_peer != bob_peer)
                                continue;
                            downlink.push(now, LinkPacket{send, arrived.origin_ms});
                        }
                    }
                }
            }
        }
        if (now >= next_client_frame)
        {
            next_client_frame += scenario.client_frame_ms;
            for (LinkPacket &arrived : downlink.drain(now))
            {
                const auto result = peers.clients.at(bob_peer).process_native_packet(
                    host_peer, arrived.send.p2p_channel, arrived.send.payload);
                if (result.audit.has_value())
                    fail("the production client session rejected a relayed stock packet");
                for (const LegacySteamClientInbound &inbound : result.inbound)
                {
                    if (inbound.kind != LegacySteamClientInboundKind::gameplay)
                        continue;
                    DecodedPacket decoded;
                    PacketError packet_error{};
                    PlayerUpdatePayload update;
                    PayloadError payload_error{};
                    if (!decode_packet(protocol, inbound.payload, decoded, packet_error) ||
                        !decode_player_update_payload(decoded.payload, protocol, update,
                                                      payload_error) ||
                        !apply_player_update(update, protocol, applied))
                        fail("a relayed stock PlayerUpdate did not decode on the receiver");
                    presentation.receive(applied, static_cast<float>(now / 1000.0));
                    has_applied = true;
                    ++received;
                    latencies.push_back(now - arrived.origin_ms);
                }
            }
            if (has_applied)
            {
                const double presented =
                    scenario.smoothing
                        ? static_cast<double>(
                              presentation.sample(static_cast<float>(now / 1000.0)).position.x)
                        : static_cast<double>(applied.position.x);
                if (previous_presented.has_value())
                {
                    const double step = presented - *previous_presented;
                    steps.push_back(step);
                    if (std::abs(step) < 0.1 * static_cast<double>(walk_speed) *
                                             scenario.client_frame_ms / 1000.0)
                        ++frozen_frames;
                }
                previous_presented = presented;
                errors.push_back(truth(now) - presented);
            }
        }
    }

    ScenarioResult result;
    result.latency = summarize(latencies);
    result.step = summarize(steps);
    result.tracking_error = summarize(errors);
    result.applied_fraction =
        sent == 0U ? 0.0 : static_cast<double>(received) / static_cast<double>(sent);
    result.frozen_frame_fraction =
        steps.empty() ? 0.0
                      : static_cast<double>(frozen_frames) / static_cast<double>(steps.size());
    const double seconds = scenario.duration_ms / 1000.0;
    result.messages_per_second = static_cast<double>(uplink.offered()) / seconds;
    result.bytes_per_second = static_cast<double>(uplink.offered_bytes()) / seconds;
    result.peak_queue_depth = std::max(uplink.peak_depth(), downlink.peak_depth());
    return result;
}

void report(const Scenario &scenario, const ScenarioResult &result)
{
    std::cout << "  " << scenario.name << ": latency p50=" << result.latency.p50
              << "ms p95=" << result.latency.p95 << "ms max=" << result.latency.maximum
              << "ms | applied=" << result.applied_fraction * 100.0
              << "% | step sd/mean=" << (result.step.mean == 0.0
                                             ? 0.0
                                             : result.step.deviation / std::abs(result.step.mean))
              << " frozen=" << result.frozen_frame_fraction * 100.0
              << "% | lag mean=" << result.tracking_error.mean
              << "m sd=" << result.tracking_error.deviation
              << "m max=" << result.tracking_error.maximum << "m | " << result.messages_per_second
              << " msg/s " << result.bytes_per_second << " B/s | queue="
              << result.peak_queue_depth << '\n';
}

// -------------------------------------------------------------- the sections

void link_model_is_deterministic_and_bounded()
{
    SimulatedLink clean{{}, 1U};
    LegacySteamNativeSend send;
    send.payload.assign(32U, 7U);
    clean.push(0.0, LinkPacket{send, 0.0});
    expect(clean.drain(0.0).size() == 1U, "a clean link must deliver in the same step");
    SimulatedLink impaired{{80.0, 20.0, 0.5, 0.25}, 7U};
    for (int index = 0; index < 1'000; ++index)
        impaired.push(0.0, LinkPacket{send, 0.0});
    const std::size_t delivered_early = impaired.drain(59.0).size();
    const std::size_t delivered = impaired.drain(1'000.0).size();
    expect(delivered_early == 0U, "no packet may arrive before the modelled minimum delay");
    expect(impaired.dropped() > 400U && impaired.dropped() < 600U,
           "the modelled loss rate must match the requested fraction");
    expect(delivered + impaired.dropped() == 1'000U,
           "every offered packet must be delivered or dropped exactly once");
}

void relay_latency_and_remote_smoothness(ProtocolVersion protocol)
{
    std::cout << "[DATA] relay latency and smoothness ("
              << (protocol == ProtocolVersion::classic_v3 ? "Classic v3" : "Modern v4") << ")\n";

    Scenario clean;
    clean.name = "clean link";
    const ScenarioResult clean_result = run_scenario(protocol, clean);
    report(clean, clean_result);

    Scenario clean_raw = clean;
    clean_raw.name = "clean link, no smoothing";
    clean_raw.smoothing = false;
    const ScenarioResult clean_raw_result = run_scenario(protocol, clean_raw);
    report(clean_raw, clean_raw_result);

    Scenario impaired;
    impaired.name = "80ms/20ms/2% link";
    impaired.link = {80.0, 20.0, 0.02, 0.05};
    const ScenarioResult impaired_result = run_scenario(protocol, impaired);
    report(impaired, impaired_result);

    Scenario impaired_raw = impaired;
    impaired_raw.name = "80ms/20ms/2% link, no smoothing";
    impaired_raw.smoothing = false;
    const ScenarioResult impaired_raw_result = run_scenario(protocol, impaired_raw);
    report(impaired_raw, impaired_raw_result);

    Scenario hitching;
    hitching.name = "host hitching 120ms every 2s";
    hitching.link = {40.0, 10.0, 0.0, 0.0};
    hitching.hitch_ms = 120.0;
    hitching.hitch_period_ms = 2'000.0;
    const ScenarioResult hitching_result = run_scenario(protocol, hitching);
    report(hitching, hitching_result);

    // One host frame plus one client frame of pumping, and nothing else, may sit
    // between a state change and its application on a clean link.
    expect(clean_result.latency.p95 <= 40.0,
           "a clean link must relay a movement update inside two frames");
    expect(clean_result.applied_fraction > 0.99,
           "a clean link must not lose movement updates in the production relay");
    expect(impaired_result.latency.p50 >= 2.0 * impaired.link.delay_ms,
           "an impaired link must pay both hops of the host relay");
    // Both hops, both jitter windows, one reordering hold-back and both pumps.
    expect(impaired_result.latency.maximum <=
               2.0 * (impaired.link.delay_ms + impaired.link.jitter_ms + impaired.link.delay_ms) +
                   2.0 * clean.host_frame_ms,
           "relay latency must stay bounded under jitter and reordering");

    // Smoothness: a remote player walking at a constant speed must advance by
    // the same distance every frame, on both links.
    const auto relative_deviation = [](const ScenarioResult &result) {
        return result.step.mean == 0.0 ? 1.0 : result.step.deviation / std::abs(result.step.mean);
    };
    expect(relative_deviation(clean_result) < 0.05,
           "a constant-speed remote player must have near-zero per-frame step variance");
    expect(relative_deviation(impaired_result) < 0.35,
           "jitter and loss must not turn constant remote movement into visible judder");
    expect(relative_deviation(impaired_result) < relative_deviation(impaired_raw_result),
           "presentation smoothing must beat snapping to the newest packet");
    expect(clean_result.frozen_frame_fraction < 0.02,
           "a clean link must not leave a remote player standing still between packets");
    expect(impaired_result.frozen_frame_fraction < 0.05,
           "a lost movement packet must not freeze a remote player's feet");
    expect(impaired_result.tracking_error.deviation < 0.25,
           "the presented remote position must not oscillate around the truth");
    expect(hitching_result.latency.maximum <= hitching.hitch_ms + 2.0 * 60.0,
           "a host hitch must delay a relay by the hitch and no more");
}

// ------------------------------------------- session-scoped scenario driver
//
// Carries application packets between the production host and client sessions
// through the same impairable link as the measurements above, and records what
// each peer actually applied. Used by the player/session scenarios below.

constexpr PeerId carol_peer = 903U;

struct Delivery final
{
    double arrival_ms{};
    PeerId target{};
    LegacySteamNativeSend send;
};

class SessionDriver final
{
  public:
    SessionDriver(ProtocolVersion protocol, LinkProfile profile)
        : peers_{protocol}, profile_{profile}, random_{0xA11CEU}
    {
    }

    Peers &peers() noexcept
    {
        return peers_;
    }

    [[nodiscard]] std::vector<std::uint8_t> frame(std::string_view name,
                                                  std::span<const std::uint8_t> payload) const
    {
        const auto *descriptor = message_by_name(peers_.protocol, name);
        PacketError packet_error{};
        if (descriptor == nullptr)
            fail("a scenario named a message the stock registry does not carry");
        auto packet = encode_packet(peers_.protocol, descriptor->id, payload, packet_error);
        if (!packet.has_value())
            fail("a scenario could not frame a stock packet");
        return std::move(*packet);
    }

    // A client broadcast: uplink to the host, host relay, downlink to the rest.
    void client_broadcast(PeerId from, std::string_view name,
                          std::span<const std::uint8_t> payload, MessageDeliveryPolicy delivery)
    {
        const auto packet = frame(name, payload);
        std::string error;
        const auto wire =
            peers_.clients.at(from).create_outbound(std::nullopt, packet, delivery, error);
        if (!wire.has_value())
            fail("the production client session refused a scenario broadcast");
        const auto accepted = peers_.host.process_wire_packet(from, wire->payload);
        if (accepted.audit.has_value())
            fail("the production host session rejected a scenario broadcast");
        for (const LegacySteamHostInbound &inbound : accepted.inbound)
        {
            LegacySteamOutbound relay;
            relay.application_packet = inbound.payload;
            relay.delivery = inbound.delivery;
            relay.channel = inbound.channel;
            relay.sender_gid = inbound.sender_gid;
            relay.recipient_gid = inbound.recipient_gid;
            relay.client_relay = true;
            const auto sends = peers_.host.create_outbound(relay, error);
            if (!sends.has_value())
                fail("the production host session could not relay a scenario broadcast");
            for (const LegacySteamNativeSend &send : *sends)
                queue(send);
        }
    }

    // An authoritative host broadcast (TimeOfDayMessage and friends).
    void host_broadcast(std::string_view name, std::span<const std::uint8_t> payload,
                        MessageDeliveryPolicy delivery)
    {
        LegacySteamOutbound outbound;
        outbound.application_packet = frame(name, payload);
        outbound.delivery = delivery;
        std::string error;
        const auto sends = peers_.host.create_outbound(outbound, error);
        if (!sends.has_value())
            fail("the production host session refused an authoritative scenario broadcast");
        for (const LegacySteamNativeSend &send : *sends)
            queue(send);
    }

    void pump(double now_ms)
    {
        now_ = now_ms;
        std::ranges::stable_sort(queue_, {}, &Delivery::arrival_ms);
        const auto split = std::ranges::find_if(
            queue_, [&](const Delivery &entry) { return entry.arrival_ms > now_ms; });
        std::vector<Delivery> ready{std::make_move_iterator(queue_.begin()),
                                    std::make_move_iterator(split)};
        queue_.erase(queue_.begin(), split);
        for (Delivery &entry : ready)
        {
            const auto result = peers_.clients.at(entry.target).process_native_packet(
                host_peer, entry.send.p2p_channel, entry.send.payload);
            if (result.audit.has_value())
                fail("the production client session rejected a relayed scenario packet");
            for (const LegacySteamClientInbound &inbound : result.inbound)
            {
                if (inbound.kind != LegacySteamClientInboundKind::gameplay)
                    continue;
                DecodedPacket decoded;
                PacketError packet_error{};
                if (!decode_packet(peers_.protocol, inbound.payload, decoded, packet_error) ||
                    decoded.message == nullptr)
                    fail("a relayed scenario packet did not decode on the receiver");
                applied_[entry.target].push_back({std::string{decoded.message->name},
                                                  inbound.gid,
                                                  {decoded.payload.begin(), decoded.payload.end()}});
            }
        }
    }

    struct Applied final
    {
        std::string message;
        std::uint8_t sender_gid{};
        std::vector<std::uint8_t> payload;
    };

    [[nodiscard]] const std::vector<Applied> &applied(PeerId peer)
    {
        return applied_[peer];
    }

    [[nodiscard]] std::size_t count(PeerId peer, std::string_view message,
                                    std::optional<std::uint8_t> sender = std::nullopt)
    {
        std::size_t total{};
        for (const Applied &entry : applied_[peer])
            if (entry.message == message && (!sender.has_value() || entry.sender_gid == *sender))
                ++total;
        return total;
    }

    [[nodiscard]] std::size_t offered() const noexcept
    {
        return offered_;
    }
    [[nodiscard]] std::size_t dropped() const noexcept
    {
        return dropped_;
    }

  private:
    // The downlink leg. The uplink leg is synchronous here because the host
    // relay is what these scenarios exercise, not the measurement of its delay.
    void queue(const LegacySteamNativeSend &send)
    {
        ++offered_;
        if (profile_.loss > 0.0 && uniform() < profile_.loss)
        {
            ++dropped_;
            return;
        }
        double arrival = now_ + profile_.delay_ms;
        if (profile_.jitter_ms > 0.0)
            arrival += (uniform() * 2.0 - 1.0) * profile_.jitter_ms;
        if (profile_.reorder > 0.0 && uniform() < profile_.reorder)
            arrival += profile_.delay_ms;
        queue_.push_back({std::max(arrival, now_), send.remote_peer, send});
    }

    [[nodiscard]] double uniform()
    {
        return std::uniform_real_distribution<double>{0.0, 1.0}(random_);
    }

    Peers peers_;
    LinkProfile profile_;
    std::mt19937 random_;
    std::vector<Delivery> queue_;
    std::map<PeerId, std::vector<Applied>> applied_;
    double now_{};
    std::size_t offered_{};
    std::size_t dropped_{};
};

// --------------------------------------------------------------- scenarios

void player_discovery_follows_the_original_handler(ProtocolVersion protocol)
{
    // The reply matrix of _processPlayerExistsMessage, which is the only reason
    // a joiner's carried item ever reaches the peers already in the session.
    expect(player_discovery_response(false, true, true) == PlayerDiscoveryResponse{},
           "a repeat PlayerExists must do nothing at all (Sender.Tag != null)");
    expect(player_discovery_response(true, true, false) ==
               PlayerDiscoveryResponse{true, false, true, true, true},
           "a guest answers a newcomer with presence, time connected, item and focus");
    expect(player_discovery_response(true, true, true) ==
               PlayerDiscoveryResponse{true, true, true, true, true},
           "only the host adds TimeOfDay to that answer");
    expect(player_discovery_response(true, false, false) ==
               PlayerDiscoveryResponse{false, false, false, true, true},
           "discovering a peer that did not ask still publishes item and focus");
    expect(player_discovery_response(true, false, true).carried_item,
           "the host publishes its own carried item for a peer it has just discovered");

    SessionDriver driver{protocol, {}};
    driver.peers().join(carol_peer, "Carol");
    const std::uint8_t alice_gid = driver.peers().host.gid_for(alice_peer).value_or(0U);
    const std::uint8_t carol_gid = driver.peers().host.gid_for(carol_peer).value_or(0U);
    expect(alice_gid != 0U && carol_gid != 0U && alice_gid != carol_gid,
           "the scenario peers must hold distinct stock GIDs");

    PayloadError payload_error{};
    const auto announcement = encode_player_exists_payload(
        {true, std::vector<std::uint8_t>(10), "Carol", {carol_gid, 1U}}, payload_error);
    const auto answer = encode_player_exists_payload(
        {false, std::vector<std::uint8_t>(10), "Alice", {alice_gid, 1U}}, payload_error);
    if (!announcement.has_value() || !answer.has_value())
        fail("the scenario could not encode a stock presence payload");
    const std::array<std::uint8_t, 2> carol_item{50U, 0U};
    const std::array<std::uint8_t, 2> alice_item{49U, 0U};

    // Carol announces herself; the host relays it to the peers already present.
    driver.client_broadcast(carol_peer, "PlayerExistsMessage", *announcement,
                            MessageDeliveryPolicy::reliable_ordered);
    driver.pump(0.0);
    expect(driver.count(alice_peer, "PlayerExistsMessage", carol_gid) == 1U,
           "a newcomer's presence must reach every peer already in the session");

    // Alice's answer: presence, then (unconditionally) her carried item.
    const PlayerDiscoveryResponse alice_reply = player_discovery_response(true, true, false);
    if (alice_reply.presence)
        driver.client_broadcast(alice_peer, "PlayerExistsMessage", *answer,
                                MessageDeliveryPolicy::reliable_ordered);
    if (alice_reply.carried_item)
        driver.client_broadcast(alice_peer, "ChangeCarriedItemMessage", alice_item,
                                MessageDeliveryPolicy::reliable_unordered);
    driver.pump(1.0);
    expect(driver.count(carol_peer, "ChangeCarriedItemMessage", alice_gid) == 1U,
           "a newcomer must be told what the players already present are holding");

    // Carol discovers Alice from that answer. RequestResponse is false, so the
    // regression this guards is Carol staying silent and appearing empty-handed.
    const PlayerDiscoveryResponse carol_reply = player_discovery_response(true, false, false);
    expect(!carol_reply.presence && carol_reply.carried_item,
           "answering a presence answer must publish the item without a presence loop");
    if (carol_reply.carried_item)
        driver.client_broadcast(carol_peer, "ChangeCarriedItemMessage", carol_item,
                                MessageDeliveryPolicy::reliable_unordered);
    driver.pump(2.0);
    expect(driver.count(alice_peer, "ChangeCarriedItemMessage", carol_gid) == 1U,
           "the peers already present must learn what a newcomer is holding");
    expect(driver.count(bob_peer, "ChangeCarriedItemMessage", carol_gid) == 1U,
           "every peer already present, not just the one that answered, learns it");
    expect(driver.count(carol_peer, "ChangeCarriedItemMessage", carol_gid) == 0U,
           "a peer must never be corrected by its own echoed carried item");
}

void a_real_building_burst_is_never_rate_clipped()
{
    // The original applies every AlterBlockMessage and only decides afterwards
    // whether the sender's one-second count deserves a ban, so a refusal here
    // has no counterpart in the game: the player just watches the block they
    // placed pop back. One busy second of building, mining and door use must
    // pass untouched, while an obviously hostile flood must still be clipped.
    ClientGameplayRateLimiter limiter;
    constexpr PeerId peer = alice_peer;
    std::size_t clipped{};
    const auto offer = [&](std::string_view message, std::size_t count, std::uint64_t at_ms) {
        for (std::size_t index = 0U; index < count; ++index)
            if (!limiter.allow(peer, message, at_ms))
                ++clipped;
    };
    // A held mouse button at 60 Hz, one edit every other frame, each with its
    // dig effect; two doors toggled twice; a blast's block removals; and the
    // player's own 30 Hz state going out underneath all of it.
    offer("AlterBlockMessage", 40U, 500U);
    offer("DigMessage", 40U, 520U);
    offer("DoorOpenCloseMessage", 4U, 540U);
    offer("RemoveBlocksMessage", 2U, 560U);
    offer("PlayerUpdateMessage", 30U, 580U);
    offer("ChangeCarriedItemMessage", 4U, 600U);
    expect(clipped == 0U, "a busy second of building and mining must never be rate clipped");

    // A door toggle is two block edits in the original's accounting, so the
    // old ten-per-second ceiling was five doors away from refusing real play.
    clipped = 0U;
    offer("AlterBlockMessage", 20U, 1'800U);
    expect(clipped == 0U, "ten doors' worth of block edits must still fit in one second");

    clipped = 0U;
    offer("AlterBlockMessage", 4'000U, 4'000U);
    expect(clipped > 0U, "a hostile block-edit flood must still be refused");

    // A fresh second starts a fresh budget rather than inheriting the flood.
    clipped = 0U;
    offer("AlterBlockMessage", 40U, 6'000U);
    offer("DigMessage", 40U, 6'010U);
    expect(clipped == 0U, "the window must reset instead of punishing the next second");
}

void melee_item_window_tolerates_the_delivery_race()
{
    expect(melee_item_matches_announcement(50U, 50U, true, 0U, 10.0F),
           "a swing with the announced item is always accepted");
    expect(melee_item_matches_announcement(50U, 0U, false, 0U, 0.0F),
           "a peer that has not announced an item yet is taken on the original's terms");
    expect(melee_item_matches_announcement(50U, 49U, true, 50U, 0.2F),
           "a switch that overtook the swing must not cancel the hit that happened");
    expect(!melee_item_matches_announcement(50U, 49U, true, 50U, 4.0F),
           "the grace window must close instead of accepting any past item forever");
    expect(!melee_item_matches_announcement(50U, 49U, true, 48U, 0.1F),
           "an item the peer never held must still be refused");
}

void creative_immunity_follows_the_network_version()
{
    // Stock ApplyDamage has no InfiniteResourceMode test in either shipped
    // generation; the network-version guard was the OpenClassic mod's. (The
    // test keeps its name so the history of the rule stays findable.)
    expect(!creative_damage_immunity(GameMode::creative, true, true, true),
           "an online Classic creative game takes damage like a stock client");
    expect(!creative_damage_immunity(GameMode::creative, true, true, false),
           "an online Modern creative game takes damage like a stock client");
    expect(!creative_damage_immunity(GameMode::creative, true, false, true) &&
               !creative_damage_immunity(GameMode::creative, true, false, false),
           "stock Creative is not immune offline either");
    expect(!creative_damage_immunity(GameMode::survival, false, true, true),
           "a survival player is never immune");
    expect(!creative_damage_immunity(GameMode::exploration, true, false, true),
           "Exploration keeps resources but not immunity");
}

void host_clock_does_not_drift_over_an_hour(ProtocolVersion protocol)
{
    // The original advances GameScreen.Day locally on every peer and the host
    // overwrites it every five seconds (CastleMinerZGame._worldUpdateTimer).
    // Drift therefore has to stay inside one broadcast interval, including when
    // a broadcast is lost and the next one has to do the correcting.
    constexpr double broadcast_ms = 5'000.0;
    constexpr double hour_ms = 3'600'000.0;
    constexpr double step_ms = 50.0;
    SessionDriver driver{protocol, {60.0, 15.0, 0.08, 0.1}};
    world::DayNightCycle host_clock{0.4F};
    world::DayNightCycle guest_clock{0.0F};
    double next_broadcast = 0.0;
    double worst = 0.0;
    double worst_last_ten_minutes = 0.0;
    double worst_apply_error = 0.0;
    double longest_gap_ms = 0.0;
    double last_apply_ms = 0.0;
    std::size_t broadcasts{};
    std::size_t received{};
    std::size_t consumed{};

    for (double now = 0.0; now <= hour_ms; now += step_ms)
    {
        // The guest's frame is 10% slow, so its own advance cannot keep up on
        // its own: only the broadcast keeps the two clocks together.
        host_clock.update(static_cast<float>(step_ms / 1000.0));
        guest_clock.update(static_cast<float>(step_ms / 1000.0) * 0.9F);
        if (now >= next_broadcast)
        {
            next_broadcast += broadcast_ms;
            PayloadError payload_error{};
            const auto payload = encode_time_of_day_payload(host_clock.day(), payload_error);
            if (!payload.has_value())
                fail("the host could not encode a stock TimeOfDay");
            driver.host_broadcast("TimeOfDayMessage", *payload,
                                  MessageDeliveryPolicy::reliable_ordered);
            ++broadcasts;
        }
        driver.pump(now);
        const std::vector<SessionDriver::Applied> &history = driver.applied(alice_peer);
        for (std::size_t index = consumed; index < history.size(); ++index)
        {
            if (history[index].message != "TimeOfDayMessage" || history[index].sender_gid != 0U)
                continue;
            // _processTimeOfDayMessage: the guest takes the host's day verbatim.
            float authoritative{};
            PayloadError payload_error{};
            if (!decode_time_of_day_payload(history[index].payload, authoritative, payload_error))
                fail("a relayed stock TimeOfDay did not decode on the guest");
            guest_clock = world::DayNightCycle{authoritative};
            worst_apply_error =
                std::max(worst_apply_error,
                         std::abs(static_cast<double>(guest_clock.day() - authoritative)));
            if (received != 0U)
                longest_gap_ms = std::max(longest_gap_ms, now - last_apply_ms);
            last_apply_ms = now;
            ++received;
        }
        consumed = history.size();
        if (received == 0U)
            continue; // Before the first broadcast lands the guest has nothing to follow.
        const double divergence =
            std::abs(static_cast<double>(host_clock.day() - guest_clock.day()));
        worst = std::max(worst, divergence);
        if (now >= hour_ms - 600'000.0)
            worst_last_ten_minutes = std::max(worst_last_ten_minutes, divergence);
    }

    const double day_seconds = static_cast<double>(world::DayNightCycle::minutes_per_day) * 60.0;
    // The longest run of lost broadcasts, at the guest's 10% clock error, plus
    // the in-flight time of the broadcast that finally corrected it.
    const double allowed = (longest_gap_ms * 0.1 + 200.0) / 1000.0 / day_seconds;
    std::cout << "[DATA] clock over one simulated hour: broadcasts=" << broadcasts
              << " applied=" << received << " longest gap=" << longest_gap_ms
              << "ms worst drift=" << worst << " day (" << worst * day_seconds
              << "s of day) last ten minutes=" << worst_last_ten_minutes
              << " day allowed=" << allowed << " day\n";
    expect(received > 0U && received < broadcasts,
           "the scenario must actually lose some TimeOfDay broadcasts to be worth running");
    expect(worst <= allowed,
           "a lost TimeOfDay must leave the guest clock bounded, not drifting");
    // The real no-drift property: an hour in, the disagreement is no larger
    // than it was at the start. Nothing accumulates.
    expect(worst_last_ten_minutes <= worst + 1.0e-9,
           "an hour of play must not grow the clock disagreement");
    // The link model also drops reliable traffic, so this run loses four
    // broadcasts in a row where the real transport would retransmit. Even then
    // the two clocks stay within a few seconds of day-time of each other.
    expect(worst * day_seconds < 4.0,
           "host and guest must never read more than a few seconds of day apart");
    expect(worst_apply_error < 1.0e-6,
           "applying a TimeOfDay must put the guest exactly on the broadcast day");
    // Let the broadcast in flight land, then compare the one clock fact the
    // stock protocol can actually keep identical: the day counter.
    for (double now = hour_ms; now <= hour_ms + 600.0; now += step_ms)
    {
        host_clock.update(static_cast<float>(step_ms / 1000.0));
        guest_clock.update(static_cast<float>(step_ms / 1000.0) * 0.9F);
        driver.pump(now);
        const std::vector<SessionDriver::Applied> &history = driver.applied(alice_peer);
        for (std::size_t index = consumed; index < history.size(); ++index)
        {
            float authoritative{};
            PayloadError payload_error{};
            if (history[index].message == "TimeOfDayMessage" &&
                decode_time_of_day_payload(history[index].payload, authoritative, payload_error))
                guest_clock = world::DayNightCycle{authoritative};
        }
        consumed = history.size();
    }
    StateDigest host_view{"host"};
    StateDigest guest_view{"alice"};
    openminerz::test::record_session_clock(host_view, host_clock.day());
    openminerz::test::record_session_clock(guest_view, guest_clock.day());
    const auto clock_differences = openminerz::test::compare_state(host_view, guest_view);
    if (!clock_differences.empty())
        std::cout << openminerz::test::describe_divergence(host_view, guest_view) << '\n';
    expect(clock_differences.empty(),
           "host and guest must agree on the day number after an hour of play");
    // The local Endurance restart must never fire inside a session: it rewound
    // the guest's day to 0.4 and, from a hosting player, everybody's night.
    expect(respawn_restarts_level(GameMode::endurance, false),
           "an offline Endurance respawn still restarts the level");
    expect(!respawn_restarts_level(GameMode::endurance, true),
           "an Endurance respawn inside a session must not restart the clock");
    expect(!respawn_restarts_level(GameMode::survival, false),
           "only Endurance reaches RestartLevelMessage");
}

void death_state_converges_under_loss_and_reordering(ProtocolVersion protocol)
{
    // PlayerUpdateMessage is InOrder-unreliable unless Using or Reloading, so
    // the dead flag rides an unreliable channel. It is re-sent every tick, so a
    // peer must never be left believing a living player is dead, or the other
    // way round, for longer than a couple of send intervals.
    constexpr double send_ms = 1000.0 / 30.0;
    SessionDriver driver{protocol, {70.0, 25.0, 0.3, 0.2}};
    const std::uint8_t alice_gid = driver.peers().host.gid_for(alice_peer).value_or(0U);
    RemotePlayerReplicationState applied;
    bool truth_dead = false;
    double wrong_since = -1.0;
    double worst_wrong = 0.0;
    std::size_t consumed{};
    double next_send = 0.0;

    for (double now = 0.0; now <= 12'000.0; now += 1.0)
    {
        // Death, respawn, death again, each racing a position update.
        const bool next_truth = (now >= 2'000.0 && now < 5'000.0) || now >= 9'000.0;
        if (next_truth != truth_dead)
        {
            truth_dead = next_truth;
            next_send = now; // KillPlayer/RespawnPlayer flush the send timer.
        }
        if (now >= next_send)
        {
            next_send += send_ms;
            LocalPlayerReplicationInput input;
            input.position = {static_cast<float>(walk_speed * now / 1000.0), 2.0F, 0.0F};
            input.velocity = {truth_dead ? 0.0F : walk_speed, 0.0F, 0.0F};
            input.movement_y = truth_dead ? 0.0F : 1.0F;
            input.dead = truth_dead;
            PlayerUpdatePayload update;
            PayloadError payload_error{};
            if (!make_player_update(input, protocol, update, payload_error))
                fail("the scenario could not build a stock PlayerUpdate");
            const auto payload = encode_player_update_payload(update, protocol, payload_error);
            if (!payload.has_value())
                fail("the scenario could not encode a stock PlayerUpdate");
            expect(((update.state_flags & PlayerUpdatePayload::dead_flag) != 0U) == truth_dead,
                   "the dead flag must survive the local replication input");
            driver.client_broadcast(alice_peer, "PlayerUpdateMessage", *payload,
                                    player_update_delivery(update));
        }
        driver.pump(now);
        const std::vector<SessionDriver::Applied> &history = driver.applied(bob_peer);
        for (std::size_t index = consumed; index < history.size(); ++index)
        {
            if (history[index].message != "PlayerUpdateMessage" ||
                history[index].sender_gid != alice_gid)
                continue;
            PlayerUpdatePayload decoded;
            PayloadError payload_error{};
            if (!decode_player_update_payload(history[index].payload, protocol, decoded,
                                              payload_error) ||
                !apply_player_update(decoded, protocol, applied))
                fail("a relayed stock PlayerUpdate did not apply on the receiver");
        }
        consumed = history.size();
        if (applied.dead == truth_dead)
        {
            wrong_since = -1.0;
        }
        else
        {
            if (wrong_since < 0.0)
                wrong_since = now;
            worst_wrong = std::max(worst_wrong, now - wrong_since);
        }
    }

    // Engineer 1's shared digest, under the player domain, so a disagreement
    // is reported as the named fact rather than a hash mismatch.
    StateDigest sender_view{"alice"};
    StateDigest receiver_view{"bob"};
    openminerz::test::record_player(sender_view, alice_gid,
                                    PlayerFact{"Alice", std::uint16_t{50U}, truth_dead, true});
    openminerz::test::record_player(receiver_view, alice_gid,
                                    PlayerFact{"Alice", std::uint16_t{50U}, applied.dead, true});
    const auto differences = openminerz::test::compare_state(sender_view, receiver_view);
    std::cout << "[DATA] death state under 30% loss and reordering: longest disagreement="
              << worst_wrong << "ms, dropped=" << driver.dropped() << " of " << driver.offered()
              << '\n';
    if (!differences.empty())
        std::cout << openminerz::test::describe_divergence(sender_view, receiver_view) << '\n';
    expect(differences.empty() && applied.dead == truth_dead,
           "both peers must end a death/respawn sequence agreeing who is alive");
    // Two hops of delay and jitter, one reordering hold-back, and a run of lost
    // updates at 30% loss: well under a second, and never permanent.
    expect(worst_wrong < 900.0,
           "a lost death or respawn update must not leave a peer permanently wrong");
}

void teleport_and_respawn_snap_immediately()
{
    RemotePlayerPresentation view;
    RemotePlayerReplicationState packet;
    packet.position = {10.0F, 2.0F, 0.0F};
    packet.velocity = {walk_speed, 0.0F, 0.0F};
    view.receive(packet, 1.0F);
    static_cast<void>(view.sample(1.04F));
    packet.position = {400.0F, 2.0F, 0.0F};
    view.receive(packet, 1.05F);
    expect(std::abs(view.sample(1.05F).position.x - 400.0F) < 0.001F,
           "a teleport must snap the remote body to the new position in the same frame");
    packet.dead = true;
    view.receive(packet, 1.1F);
    expect(std::abs(view.sample(2.0F).position.x - 400.0F) < 0.001F,
           "a dead remote body must not drift on its last velocity");
    packet.dead = false;
    packet.position = {-20.0F, 2.0F, 0.0F};
    view.receive(packet, 2.1F);
    expect(std::abs(view.sample(2.1F).position.x + 20.0F) < 0.001F,
           "a respawn must snap instead of sliding across the world");
}

// The in-game menu's three teleports (and any mod command that moves a player)
// reach a peer as nothing more than the next PlayerUpdateMessage with a new
// position. Whatever the size or direction of the jump, the body is at the
// destination in the frame the packet lands and never revisits where it was.
void menu_teleports_arrive_at_once_and_never_drag_back()
{
    struct Jump final
    {
        const char *name;
        openminerz::world::Vec3 from;
        openminerz::world::Vec3 to;
    };
    // Teleport To Surface is a purely vertical move in the player's own column;
    // Teleport To Start and Teleport To Player cross the world.
    const std::array jumps{
        Jump{"surface", {120.5F, -38.9F, 44.5F}, {120.5F, 12.1F, 44.5F}},
        Jump{"start", {2600.5F, 4.1F, -1800.5F}, {8.5F, 9.1F, -7.5F}},
        Jump{"player", {-300.5F, 20.1F, 15.5F}, {912.5F, -20.9F, 640.5F}},
    };
    for (const Jump &jump : jumps)
    {
        RemotePlayerPresentation view;
        RemotePlayerReplicationState packet;
        packet.position = jump.from;
        // Walking when the menu was opened: the last pre-teleport packets carry
        // a velocity that must not be carried across the jump as a correction.
        packet.velocity = {walk_speed, 0.0F, 0.0F};
        float now = 1.0F;
        for (int update = 0; update < 6; ++update, now += 0.05F)
        {
            packet.position.x = jump.from.x + static_cast<float>(walk_speed) * 0.05F *
                                                  static_cast<float>(update);
            view.receive(packet, now);
            static_cast<void>(view.sample(now + 0.02F));
        }
        // The retail client leaves the velocity it had; ours sends zero. Either
        // way the body must be exactly at the destination when the packet lands.
        packet.position = jump.to;
        packet.velocity = {};
        view.receive(packet, now);
        const auto arrived = view.sample(now).position;
        expect(std::abs(arrived.x - jump.to.x) < 0.001F &&
                   std::abs(arrived.y - jump.to.y) < 0.001F &&
                   std::abs(arrived.z - jump.to.z) < 0.001F,
               std::string{"a "} + jump.name +
                   " teleport must show the player at the destination in the same frame");
        // Standing still afterwards, sampled every frame for a second and
        // across later updates: the body stays put.
        float worst = 0.0F;
        for (int frame = 1; frame <= 60; ++frame)
        {
            const float time = now + static_cast<float>(frame) / 60.0F;
            if (frame % 3 == 0)
                view.receive(packet, time);
            const auto shown = view.sample(time).position;
            worst = std::max({worst, std::abs(shown.x - jump.to.x), std::abs(shown.y - jump.to.y),
                              std::abs(shown.z - jump.to.z)});
        }
        expect(worst < 0.001F, std::string{"after a "} + jump.name +
                                   " teleport nothing may pull the body back toward where it was");
    }
}

void presentation_is_bounded_and_does_not_mutate_authority()
{
    RemotePlayerPresentation view;
    RemotePlayerReplicationState packet;
    packet.position = {0.0F, 2.0F, 0.0F};
    packet.velocity = {walk_speed, 0.0F, 0.0F};
    view.receive(packet, 0.0F);
    // A peer that stops sending must not be extrapolated across the world.
    const float far_future = view.sample(30.0F).position.x;
    expect(far_future <= static_cast<float>(walk_speed) * 0.5F + 0.001F,
           "extrapolation of a silent peer must stay bounded");
    expect(std::abs(packet.position.x) < 0.001F,
           "presentation must never write back into authoritative packet state");
}

void bridge_ipc_latency()
{
#if defined(_WIN32)
    const std::filesystem::path helper =
        std::filesystem::path{__FILE__}.parent_path().parent_path() / "out" / "steam-bridge32" /
        "OpenMinerZ.SteamBridge32.exe";
    if (!std::filesystem::is_regular_file(helper))
    {
        std::cout << "[SKIP] the x86 helper binary is not built; IPC latency not measured\n";
        return;
    }
    SteamBridgeIpcProbe probe;
    std::string error;
    const bool measured = SteamBridgeClient::ipc_probe(helper, 200U, probe, error);
    expect(measured, std::string{"the x86 helper IPC probe failed: "} + error);
    if (!measured)
        return;
    std::cout << "[DATA] bridge IPC: round trip p50=" << probe.round_trip_p50_ms
              << "ms p95=" << probe.round_trip_p95_ms << "ms max=" << probe.round_trip_maximum_ms
              << "ms | burst throughput=" << probe.burst_frames_per_second << " frames/s over "
              << probe.samples << " frames\n";
    // One scheduler tick per datagram is the failure this guards: the stock
    // profile needs well over a hundred host datagrams a second.
    expect(probe.round_trip_p95_ms < 5.0,
           "a bridge round trip must not cost a Windows scheduler tick");
    expect(probe.burst_frames_per_second > 1'000.0,
           "the bridge must carry far more datagrams per second than a full lobby produces");
#else
    std::cout << "[SKIP] the x86 helper exists only on Windows\n";
#endif
}

void run(std::string_view name, void (*section)())
{
    const int before = failures;
    try
    {
        section();
    }
    catch (const std::exception &exception)
    {
        ++failures;
        std::cout << "[FAIL] " << name << ": " << exception.what() << '\n';
    }
    if (failures == before)
        std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main()
{
    run("link_model_is_deterministic_and_bounded", link_model_is_deterministic_and_bounded);
    run("classic_relay_latency_and_remote_smoothness",
        [] { relay_latency_and_remote_smoothness(ProtocolVersion::classic_v3); });
    run("modern_relay_latency_and_remote_smoothness",
        [] { relay_latency_and_remote_smoothness(ProtocolVersion::modern_v4); });
    run("classic_player_discovery_follows_the_original_handler",
        [] { player_discovery_follows_the_original_handler(ProtocolVersion::classic_v3); });
    run("modern_player_discovery_follows_the_original_handler",
        [] { player_discovery_follows_the_original_handler(ProtocolVersion::modern_v4); });
    run("a_real_building_burst_is_never_rate_clipped", a_real_building_burst_is_never_rate_clipped);
    run("melee_item_window_tolerates_the_delivery_race",
        melee_item_window_tolerates_the_delivery_race);
    run("creative_immunity_follows_the_network_version",
        creative_immunity_follows_the_network_version);
    run("classic_host_clock_does_not_drift_over_an_hour",
        [] { host_clock_does_not_drift_over_an_hour(ProtocolVersion::classic_v3); });
    run("modern_host_clock_does_not_drift_over_an_hour",
        [] { host_clock_does_not_drift_over_an_hour(ProtocolVersion::modern_v4); });
    run("classic_death_state_converges_under_loss_and_reordering",
        [] { death_state_converges_under_loss_and_reordering(ProtocolVersion::classic_v3); });
    run("modern_death_state_converges_under_loss_and_reordering",
        [] { death_state_converges_under_loss_and_reordering(ProtocolVersion::modern_v4); });
    run("teleport_and_respawn_snap_immediately", teleport_and_respawn_snap_immediately);
    run("menu_teleports_arrive_at_once_and_never_drag_back",
        menu_teleports_arrive_at_once_and_never_drag_back);
    run("presentation_is_bounded_and_does_not_mutate_authority",
        presentation_is_bounded_and_does_not_mutate_authority);
    run("bridge_ipc_latency", bridge_ipc_latency);
    if (failures == 0)
    {
        std::cout << "all multiplayer latency sections passed\n";
        return 0;
    }
    std::cout << failures << " multiplayer latency check(s) failed\n";
    return 1;
}
