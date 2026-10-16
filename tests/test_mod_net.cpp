// The modding network area (openminerz/modding/net.hpp): mod channel framing,
// the Value codec, RPC, replicated state, the content registry, client-visible
// mod download, the stock/no-mods wire guarantees, hostile input, script
// bindings, and throughput measurements.
//
// Two endpoints talk through the production LoopbackSession (admission, the
// extension hello, the control channel) wherever the case is about the
// session, and through the live-Steam LegacySteamRuntimeHost with its
// detached transport for the routing and stock-client cases.

#include "modding_test_harness.hpp"

#include "openminerz/modding/net.hpp"
#include "openminerz/net/legacy_steam_runtime_host.hpp"
#include "openminerz/net/loopback_session.hpp"
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/mod_channel.hpp"
#include "openminerz/net/mod_download.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
using namespace openminerz::net;
using modding::Handle;
using modding::HandleKind;
using modding::Map;
using modding::ModNetClient;
using modding::ModNetServer;
using modding::Side;
using modding::StoreRef;
using modding::Value;

void expect(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error{std::string{message}};
}

template <typename Function> bool throws(Function &&function)
{
    try
    {
        function();
    }
    catch (const std::exception &)
    {
        return true;
    }
    return false;
}

ModDigest digest_of(std::uint8_t seed)
{
    ModDigest digest{};
    for (std::size_t index = 0U; index < digest.size(); ++index)
        digest[index] = static_cast<std::uint8_t>(seed + index * 7U + 1U);
    return digest;
}

ModManifest modded_manifest(std::vector<std::string> ids = {"test", "other"})
{
    ModManifest manifest;
    manifest.protocol = ProtocolVersion::classic_v3;
    std::uint8_t seed = 1U;
    for (std::string &id : ids)
        manifest.mods.push_back({std::move(id), "1.0", digest_of(seed++), ModScope::server});
    return manifest;
}

ModManifest vanilla_manifest()
{
    ModManifest manifest;
    manifest.protocol = ProtocolVersion::classic_v3;
    // A client-scope mod alone never opens a mod channel.
    manifest.mods.push_back({"pretty.textures", "2.0", digest_of(9U), ModScope::client});
    return manifest;
}

Handle player(std::uint64_t serial)
{
    return Handle{HandleKind::player, serial};
}

Handle entity(std::uint64_t serial)
{
    return Handle{HandleKind::entity, serial};
}

// A server and its clients wired frame-by-frame with no session around them.
struct Link final
{
    ModNetServer &server;
    std::map<PeerId, ModNetClient *> clients;
    double now{};

    void pump(int rounds = 32)
    {
        for (int round = 0; round < rounds; ++round)
        {
            bool moved = false;
            for (auto &[peer, client] : clients)
            {
                client->update(now);
                for (const auto &frame : client->take_sends())
                {
                    server.receive(peer, frame, now);
                    moved = true;
                }
            }
            for (modding::ModNetSend &send : server.take_sends())
                if (const auto found = clients.find(send.peer); found != clients.end())
                {
                    found->second->receive(send.frame, now);
                    moved = true;
                }
            if (!moved)
                return;
        }
    }

    void tick(double seconds = 0.05)
    {
        now += seconds;
        server.update(now);
        pump();
    }

    void join(PeerId peer, ModNetClient &client, Handle handle)
    {
        clients[peer] = &client;
        expect(server.add_peer(peer, handle, now), "add_peer must accept an extension peer");
        pump();
        client.set_ready(now);
        pump();
        tick();
    }
};

std::vector<std::uint8_t> state_like_message()
{
    // Type 4 (state) as a client would forge it: flags, no keys, no drops, one
    // group for player 2 with an undefined key.
    return {4U, 0U, 0U, 0U, 1U, 1U, 1U, 2U, 1U, 0U, 3U, 10U};
}

// --- Value codec -------------------------------------------------------------

void value_codec_round_trips_and_refuses_hostile_input()
{
    const modding::ValueLimits limits;
    Map nested{
        {"name", Value{"steve"}},
        {"hp", Value{20}},
        {"exact", Value{0.1}},
        {"narrow", Value{0.5}},
        {"pos", Value{modding::Vec3{1.5, -2.0, 3.25}}},
        {"far", Value{modding::Vec3{0.1, 0.2, 0.3}}},
        {"who", Value{player(7U)}},
        {"list", Value{modding::Array{Value{true}, Value{false}, Value{}, Value{-12345678}}}},
        {"deep", Value{Map{{"x", Value{Map{{"y", Value{std::int64_t{-1}}}}}}}}}};
    const Value original{nested};
    std::vector<std::uint8_t> bytes;
    std::string error;
    expect(modding::encode_value(original, limits, bytes, error),
           "a plain value must encode: " + error);
    Value decoded;
    expect(modding::decode_value(bytes, limits, decoded, error), "it must decode: " + error);
    expect(decoded == original, "the round trip must be exact (doubles stay doubles)");

    bytes.clear();
    expect(!modding::encode_value(Value{Map{{"f", Value{modding::Callback{1U, 2U}}}}}, limits,
                                  bytes, error) &&
               bytes.empty() && error.find("function") != std::string::npos,
           "callbacks are refused and leave nothing behind");
    expect(!modding::encode_value(Value{std::numeric_limits<double>::quiet_NaN()}, limits, bytes,
                                  error),
           "NaN is refused");
    modding::ValueLimits tight;
    tight.max_depth = 3U;
    Value deep{Map{{"a", Value{Map{{"b", Value{Map{{"c", Value{1}}}}}}}}}};
    expect(!modding::encode_value(deep, tight, bytes, error), "depth is bounded on encode");
    tight = {};
    tight.max_string_bytes = 4U;
    expect(!modding::encode_value(Value{"too long"}, tight, bytes, error),
           "string length is bounded on encode");

    // Hostile bytes.
    const auto refused = [&](std::vector<std::uint8_t> input, std::string_view why) {
        Value ignored;
        std::string reason;
        expect(!modding::decode_value(input, limits, ignored, reason), why);
    };
    refused({}, "empty input is refused");
    refused({6U, 5U, 'a'}, "a truncated string is refused");
    refused({0U, 0U}, "trailing bytes are refused");
    refused({3U, 0x80U, 0x00U}, "a non-minimal varint is refused");
    refused({3U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x02U},
            "a varint overflow is refused");
    refused({10U, 0xFFU, 0xFFU, 0xFFU, 0x7FU}, "an array count larger than the input is refused");
    refused({11U, 2U, 1U, 'k', 0U, 1U, 'k', 0U}, "a map that repeats a key is refused");
    refused({5U, 0U, 0U, 0U, 0U, 0U, 0U, 0xF8U, 0x7FU}, "a NaN on the wire is refused");
    refused({42U}, "an unknown tag is refused");
    std::vector<std::uint8_t> nesting(40U, 10U);
    for (std::size_t index = 1U; index < nesting.size(); index += 2U)
        nesting[index] = 1U;
    refused(nesting, "deep nesting on the wire is refused");
}

void payload_schema_drops_bad_payloads()
{
    modding::PayloadSchema schema;
    std::string error;
    const Value spec{Map{
        {"item", Value{"string"}},
        {"count", Value{Map{{"type", Value{"integer"}}, {"min", Value{1}}, {"max", Value{64}}}}},
        {"note", Value{"string?"}},
        {"at", Value{Map{{"x", Value{"number"}}, {"y", Value{"number"}}}}}}};
    expect(modding::parse_payload_schema(spec, schema, error), "the schema must parse: " + error);
    const Value good{Map{{"item", Value{"torch"}},
                         {"count", Value{3}},
                         {"at", Value{Map{{"x", Value{1.0}}, {"y", Value{2}}}}}}};
    expect(modding::validate_payload(schema, good, error), "a matching payload passes: " + error);
    expect(
        !modding::validate_payload(schema,
                                   Value{Map{{"item", Value{"torch"}},
                                             {"count", Value{0}},
                                             {"at", Value{Map{{"x", Value{1}}, {"y", Value{2}}}}}}},
                                   error),
        "a value below min is dropped");
    expect(!modding::validate_payload(schema, Value{Map{{"item", Value{5}}}}, error),
           "a wrong type is dropped");
    Map extra = good.as_map();
    extra.emplace_back("admin", Value{true});
    expect(!modding::validate_payload(schema, Value{extra}, error) &&
               error.find("unexpected") != std::string::npos,
           "unlisted fields are dropped by default");
    expect(!modding::validate_payload(schema, Value{}, error), "nil is dropped");
    expect(!modding::parse_payload_schema(Value{Map{{"x", Value{"float"}}}}, schema, error),
           "an unknown type name fails to parse");
}

// --- Framing -----------------------------------------------------------------

void frames_fragment_and_reassemble_strictly()
{
    std::vector<std::uint8_t> message(10000U);
    for (std::size_t index = 0U; index < message.size(); ++index)
        message[index] = static_cast<std::uint8_t>(index * 31U);
    const auto frames = encode_mod_message(7U, message);
    expect(frames.size() == 3U, "10000 bytes take three 4096-byte frames");
    for (const auto &frame : frames)
        expect(frame.size() <= maximum_extension_packet_size && is_mod_channel_packet(frame),
               "every fragment is one extension packet");
    ModMessageAssembler assembler{64U * 1024U, 10.0};
    std::vector<std::uint8_t> assembled;
    ModFrameError error{};
    for (std::size_t index = 0U; index < frames.size(); ++index)
    {
        ModFrame frame;
        expect(decode_mod_frame(frames[index], frame, error), "fragments decode");
        const auto result = assembler.accept(frame, 0.0, assembled, error);
        expect(result == (index + 1U == frames.size() ? ModMessageAssembler::Result::complete
                                                      : ModMessageAssembler::Result::pending),
               "fragments assemble in order");
    }
    expect(assembled == message, "the message is reassembled byte for byte");

    ModFrame frame;
    auto truncated = frames[0];
    truncated.resize(100U);
    expect(!decode_mod_frame(truncated, frame, error), "a truncated frame is refused");
    auto corrupt = frames[1];
    corrupt[30] ^= 0x40U;
    expect(!decode_mod_frame(corrupt, frame, error) && error == ModFrameError::checksum_mismatch,
           "a corrupted frame fails its CRC");
    auto oversized = frames[0];
    oversized.resize(maximum_extension_packet_size + 8U);
    expect(!decode_mod_frame(oversized, frame, error), "an oversized frame is refused");
    std::vector<std::uint8_t> garbage(64U, 0xA5U);
    expect(!decode_mod_frame(garbage, frame, error), "garbage is refused");

    // Out of order, replayed, too large.
    ModMessageAssembler strict{64U * 1024U, 10.0};
    expect(decode_mod_frame(frames[1], frame, error) &&
               strict.accept(frame, 0.0, assembled, error) ==
                   ModMessageAssembler::Result::rejected &&
               error == ModFrameError::out_of_order,
           "a message cannot start with its second fragment");
    ModMessageAssembler small{1024U, 10.0};
    expect(decode_mod_frame(frames[0], frame, error) &&
               small.accept(frame, 0.0, assembled, error) ==
                   ModMessageAssembler::Result::rejected &&
               error == ModFrameError::message_too_large && small.buffered_bytes() == 0U,
           "a declared size above the receiver's limit is refused before buffering");
    const auto single = encode_mod_message(3U, std::vector<std::uint8_t>{1U, 2U, 3U});
    ModMessageAssembler replay{1024U, 10.0};
    expect(decode_mod_frame(single[0], frame, error) &&
               replay.accept(frame, 0.0, assembled, error) ==
                   ModMessageAssembler::Result::complete &&
               replay.accept(frame, 0.0, assembled, error) == ModMessageAssembler::Result::rejected,
           "a replayed message number is refused");
    ModMessageAssembler stalled{64U * 1024U, 2.0};
    expect(decode_mod_frame(frames[0], frame, error) &&
               stalled.accept(frame, 0.0, assembled, error) ==
                   ModMessageAssembler::Result::pending &&
               !stalled.expire(1.0) && stalled.expire(2.5) && stalled.buffered_bytes() == 0U,
           "a stalled partial message is dropped after the timeout");
}

// --- RPC and replication -------------------------------------------------------

void rpc_namespaces_identity_and_schema()
{
    const ModManifest manifest = modded_manifest();
    ModNetServer server{manifest};
    ModNetClient client{manifest};
    Link link{server};
    link.join(10U, client, player(1U));
    expect(client.welcomed() && client.local_player() == player(1U) && server.peer_ready(10U),
           "the welcome names the player and the client becomes ready");

    std::vector<std::string> seen;
    Handle sender;
    server.on("test", "buy", [&](Handle who, const Value &payload) {
        sender = who;
        seen.push_back("test:" + payload.field_or("item", Value{"?"}).as_string());
    });
    server.on("other", "buy", [&](Handle, const Value &) { seen.push_back("other"); });
    // The payload claims to be someone else; the handler gets the transport's player.
    expect(client.send("test", "buy",
                       Value{Map{{"item", Value{"torch"}}, {"player", Value{player(99U)}}}}),
           "a client message queues");
    link.pump();
    expect(seen == std::vector<std::string>{"test:torch"} && sender == player(1U),
           "only the sending mod's handler runs, with the player from the transport");

    std::vector<std::string> logs;
    server.set_log_sink([&](std::string_view, modding::LogLevel, std::string_view message) {
        logs.emplace_back(message);
    });
    modding::PayloadSchema schema;
    std::string error;
    expect(modding::parse_payload_schema(Value{Map{{"n", Value{"integer"}}}}, schema, error),
           "schema parses");
    int handled = 0;
    server.on("test", "count", [&](Handle, const Value &) { ++handled; }, schema);
    expect(client.send("test", "count", Value{Map{{"n", Value{"lots"}}}}) &&
               client.send("test", "count", Value{Map{{"n", Value{4}}}}),
           "both queue");
    link.pump();
    expect(handled == 1 && server.counters().rpc_dropped >= 1U && !logs.empty(),
           "the schema drops the bad payload, logs it, and passes the good one");

    Value received;
    client.on("test", "toast", [&](const Value &payload) { received = payload; });
    expect(server.send_to(player(1U), "test", "toast", Value{"hello"}),
           "send_to reaches a ready player");
    expect(!server.send_to(player(42U), "test", "toast", Value{"x"}),
           "an unknown player has no channel");
    link.pump();
    expect(received == Value{"hello"}, "the client handler gets the payload");
    expect(server.broadcast("test", "toast", Value{"all"}) == 1U,
           "broadcast addresses ready players");
    link.pump();
    expect(received == Value{"all"}, "broadcast arrives");

    expect(throws([&] { server.on("nope", "x", [](Handle, const Value &) {}); }),
           "an unknown mod id is a script error");
    expect(throws([&] { static_cast<void>(client.send("test", "bad name!", Value{})); }),
           "names are validated");
    expect(throws([&] {
               static_cast<void>(
                   client.send("test", "x", Value{Map{{"f", Value{modding::Callback{1U, 1U}}}}}));
           }),
           "functions cannot be sent");
}

void replication_scopes_snapshot_delta_and_events()
{
    const ModManifest manifest = modded_manifest();
    ModNetServer server{manifest};
    ModNetClient alice{manifest};
    ModNetClient bob{manifest};
    Link link{server};
    const Handle alice_player = player(1U);
    const Handle bob_player = player(2U);

    // State that exists before anyone joins arrives in the join snapshot.
    server.set(StoreRef::world(), "test", "stage", Value{3});
    server.set(StoreRef::player(alice_player), "test", "hunger", Value{80.0});
    server.set(StoreRef::player(alice_player), "test", "team", Value{"red"},
               modding::ReplicatedKeyOptions{true, modding::ReplicationPriority::normal});
    server.set(StoreRef::entity(entity(5U)), "test", "mood", Value{"calm"});

    std::vector<std::string> alice_changes;
    alice.subscribe({}, [&](const modding::StateChange &change) {
        alice_changes.push_back(std::string{change.key} + "=" +
                                (change.value.is_nil() ? std::string{"nil"}
                                 : change.value.type() == modding::ValueType::string
                                     ? change.value.as_string()
                                     : std::to_string(change.value.as_number())));
    });
    link.join(10U, alice, alice_player);
    link.join(20U, bob, bob_player);
    expect(alice.ready() && bob.ready(), "both snapshots complete");

    const auto get = [](ModNetClient &client, const StoreRef &store, std::string_view key) {
        const Value *value = client.get(store, "test", key);
        return value == nullptr ? Value{} : *value;
    };
    expect(get(alice, StoreRef::world(), "stage") == Value{3} &&
               get(bob, StoreRef::world(), "stage") == Value{3},
           "world state reaches everyone");
    expect(get(alice, StoreRef::player(alice_player), "hunger") == Value{80.0},
           "a player's private state reaches that player");
    expect(get(bob, StoreRef::player(alice_player), "hunger").is_nil(),
           "a player's private state never reaches another player");
    expect(get(bob, StoreRef::player(alice_player), "team") == Value{"red"},
           "a public key reaches every player");
    expect(get(bob, StoreRef::entity(entity(5U)), "mood") == Value{"calm"},
           "entity state reaches interested players");
    expect(!alice_changes.empty(), "the snapshot fires change events on the client");

    // Deltas: only what changed is sent.
    alice_changes.clear();
    const std::uint64_t before = server.counters().replication_bytes;
    server.set(StoreRef::player(alice_player), "test", "hunger", Value{79.5});
    server.set(StoreRef::player(alice_player), "test", "hunger", Value{79.0}); // coalesced
    link.tick();
    expect(get(alice, StoreRef::player(alice_player), "hunger") == Value{79.0} &&
               alice_changes == std::vector<std::string>{"hunger=79.000000"},
           "two sets in a tick coalesce into one change");
    expect(server.counters().replication_bytes - before < 32U,
           "a one-key delta costs a few bytes, not a snapshot");

    // Removal and store drop.
    server.set(StoreRef::world(), "test", "stage", Value{});
    server.drop_store(StoreRef::entity(entity(5U)));
    link.tick();
    expect(get(bob, StoreRef::world(), "stage").is_nil() &&
               get(bob, StoreRef::entity(entity(5U)), "mood").is_nil() &&
               bob.stores(modding::StoreScope::entity).empty(),
           "removals and dropped stores reach clients");

    // Interest scoping: bob stops seeing entity 6 when it leaves his interest.
    server.set(StoreRef::entity(entity(6U)), "test", "hp", Value{10});
    link.tick();
    expect(get(bob, StoreRef::entity(entity(6U)), "hp") == Value{10}, "visible by default");
    server.set_entity_interest([&](Handle who, Handle) { return who != bob_player; });
    link.tick();
    expect(get(bob, StoreRef::entity(entity(6U)), "hp").is_nil() &&
               get(alice, StoreRef::entity(entity(6U)), "hp") == Value{10},
           "an entity outside a player's interest is dropped on that client only");
    server.set_entity_interest({});
    link.tick();
    expect(get(bob, StoreRef::entity(entity(6U)), "hp") == Value{10},
           "regaining interest sends the store whole");

    // Public -> private: the other player's copy is removed.
    server.set(StoreRef::player(alice_player), "test", "team", Value{"red"},
               modding::ReplicatedKeyOptions{false, modding::ReplicationPriority::normal});
    link.tick();
    expect(get(bob, StoreRef::player(alice_player), "team").is_nil() &&
               get(alice, StoreRef::player(alice_player), "team") == Value{"red"},
           "a key made private disappears from other players");

    // Clients cannot write: their store is read-only and invalid scopes throw.
    expect(throws([&] {
               server.set(StoreRef{modding::StoreScope::player, entity(1U)}, "test", "k", Value{1});
           }),
           "a player store needs a player handle");
}

void replication_respects_bandwidth_and_priority()
{
    const ModManifest manifest = modded_manifest();
    modding::ModNetLimits limits;
    limits.replication_bytes_per_second = 2000.0;
    limits.replication_burst_bytes = 400.0;
    ModNetServer server{manifest, limits};
    ModNetClient client{manifest, limits};
    Link link{server};
    link.join(10U, client, player(1U));
    for (int index = 0; index < 100; ++index)
        server.set(StoreRef::world(), "test", "bulk" + std::to_string(index),
                   Value{std::string(40U, 'x')});
    server.set(StoreRef::world(), "test", "alarm", Value{true},
               modding::ReplicatedKeyOptions{false, modding::ReplicationPriority::critical});
    link.tick();
    expect(client.get(StoreRef::world(), "test", "alarm") != nullptr,
           "the critical key goes first");
    expect(server.pending_replication(10U) > 50U, "the budget holds most bulk keys back");
    for (int tick = 0; tick < 200 && server.pending_replication(10U) > 0U; ++tick)
        link.tick(0.1);
    expect(server.pending_replication(10U) == 0U &&
               client.get(StoreRef::world(), "test", "bulk99") != nullptr,
           "the backlog drains at the configured rate");
}

void content_registry_reaches_clients()
{
    const ModManifest manifest = modded_manifest();
    ModNetServer server{manifest};
    modding::ContentRegistry registry;
    registry.tables.push_back({"block", {{"test:mossy", 200U}, {"test:glow", 201U}}});
    registry.tables.push_back({"item", {{"test:canteen", 1000U}}});
    registry.extra = Value{Map{{"fallbacks", Value{modding::Array{Value{1}, Value{2}}}}}};
    server.set_content_registry(registry);
    ModNetClient client{manifest};
    Link link{server};
    link.join(10U, client, player(1U));
    expect(client.content_registry().has_value() &&
               client.content_registry()->tables == registry.tables &&
               client.content_registry()->extra == registry.extra &&
               client.content_registry()->generation == 1U,
           "the registry arrives with the welcome");
    registry.tables[0].entries.push_back({"test:new", 202U});
    server.set_content_registry(registry);
    link.pump();
    expect(client.content_registry()->generation == 2U &&
               client.content_registry()->tables[0].entries.size() == 3U,
           "a changed registry is re-sent with a higher generation");
    modding::ContentRegistry bad;
    bad.tables.push_back({"bad category!", {}});
    expect(throws([&] { server.set_content_registry(bad); }), "an invalid registry is refused");
}

// --- Hostile peers ---------------------------------------------------------------

void hostile_clients_are_contained()
{
    const ModManifest manifest = modded_manifest();
    ModNetServer server{manifest};
    ModNetClient client{manifest};
    Link link{server};
    link.join(10U, client, player(1U));
    std::vector<modding::ModNetEvent> misbehaving;
    server.add_event_listener([&](const modding::ModNetEvent &event) {
        if (event.kind == modding::ModNetEventKind::peer_misbehaving)
            misbehaving.push_back(event);
    });
    int handled = 0;
    server.on("test", "ping", [&](Handle, const Value &) { ++handled; });

    // A forged state message: refused before anything applies it.
    for (const auto &frame : encode_mod_message(1000U, state_like_message()))
        server.receive(10U, frame, link.now);
    expect(server.violations(10U) == 1U, "replicated state from a client is a violation");

    // A message naming a slot that is not in the manifest (spoofing another mod).
    std::vector<std::uint8_t> spoof{3U, 9U, 4U, 'p', 'i', 'n', 'g', 0U};
    for (const auto &frame : encode_mod_message(1001U, spoof))
        server.receive(10U, frame, link.now);
    expect(server.violations(10U) == 2U && handled == 0, "an unknown mod slot is refused");

    // Out-of-order and fragment-bomb frames.
    std::vector<std::uint8_t> big(9000U, 0x11U);
    const auto fragments = encode_mod_message(1002U, big);
    server.receive(10U, fragments[1], link.now);
    expect(server.violations(10U) == 3U, "an out-of-order fragment is refused");
    std::vector<std::uint8_t> huge(64U * 1024U, 0x22U);
    for (std::uint32_t sequence = 2000U; sequence < 2004U; ++sequence)
        server.receive(10U, encode_mod_message(sequence, huge)[0], link.now);
    expect(server.violations(10U) >= 7U,
           "fragments of a message above the client limit are refused before buffering");

    // Garbage and a flood: the peer is reported and then ignored.
    std::vector<std::uint8_t> garbage(300U, 0x5AU);
    std::copy_n("OMZE", 4, garbage.begin());
    garbage[4] = 1U;
    garbage[5] = 16U;
    for (int index = 0; index < 5000; ++index)
        server.receive(10U, garbage, link.now);
    expect(misbehaving.size() == 1U && misbehaving[0].peer == 10U,
           "a flood is reported once for removal");
    const std::uint64_t received = server.counters().frames_received;
    for (const auto &frame :
         encode_mod_message(5000U, std::vector<std::uint8_t>{3U, 0U, 4U, 'p', 'i', 'n', 'g', 0U}))
        server.receive(10U, frame, link.now + 100.0);
    expect(handled == 0 && server.counters().frames_received == received,
           "nothing from a misbehaving peer is processed any more");

    // Frames from a peer that was never added cost nothing.
    const std::uint64_t rejected = server.counters().frames_rejected;
    server.receive(77U, garbage, link.now);
    expect(server.counters().frames_rejected == rejected + 1U && !server.has_peer(77U),
           "an unknown sender is ignored");
}

void client_rate_limit_is_paced_not_punished()
{
    const ModManifest manifest = modded_manifest();
    ModNetServer server{manifest};
    ModNetClient client{manifest};
    Link link{server};
    link.join(10U, client, player(1U));
    int handled = 0;
    server.on("test", "spam", [&](Handle, const Value &) { ++handled; });
    int queued = 0;
    const std::string filler(100U, 's');
    for (int index = 0; index < 2000; ++index)
        queued += client.send("test", "spam", Value{filler}) ? 1 : 0;
    expect(queued < 2000, "the client's own queue bounds a runaway script");
    for (int tick = 0; tick < 400 && client.queued_bytes() > 0U; ++tick)
        link.tick(0.05);
    expect(handled == queued && server.violations(10U) == 0U,
           "an honest client paces itself under the host's limits and never trips them");
}

// --- Through the real admission path ------------------------------------------

HostSessionSettings session_settings(const ModManifest &manifest)
{
    HostSessionSettings settings;
    settings.server_name = "Mod Net";
    settings.protocol = ProtocolVersion::classic_v3;
    settings.maximum_players = 8;
    settings.visibility = HostVisibility::public_lobby;
    settings.mod_manifest = manifest;
    return settings;
}

// Admits `peer` through the extension hello. Returns the admission status.
AdmissionStatus admit(LoopbackSession &session, const HostSessionSettings &settings, PeerId peer,
                      const ModManifest &client_manifest, bool extension = true)
{
    PeerAdmissionRequest request;
    request.peer = peer;
    request.properties = make_session_properties(settings, false, 1, 1, false);
    request.advertised_extension = extension;
    const AdmissionDecision decision = session.connect(request);
    if (!extension || !decision.accepted())
        return decision.status;
    ExtensionMessage hello;
    hello.kind = ExtensionMessageKind::client_hello;
    hello.nonce = 0x1234U + peer;
    hello.manifest = client_manifest;
    static_cast<void>(session.submit_from_peer(peer, encode_extension_message(hello),
                                               MessageDeliveryPolicy::reliable_ordered,
                                               openminerz_control_channel));
    static_cast<void>(session.receive_for_peer(peer)); // the accept/reject reply
    return session.admission().status(peer);
}

// Moves mod frames across a LoopbackSession in both directions.
struct SessionLink final
{
    LoopbackSession &session;
    ModNetServer &server;
    std::map<PeerId, ModNetClient *> clients;
    double now{};
    std::size_t refused{};

    void pump()
    {
        for (int round = 0; round < 32; ++round)
        {
            bool moved = false;
            for (auto &[peer, client] : clients)
            {
                client->update(now);
                for (const auto &frame : client->take_sends())
                    refused += session.submit_from_peer(peer, frame,
                                                        MessageDeliveryPolicy::reliable_ordered,
                                                        openminerz_control_channel)
                                       .accepted()
                                   ? 0U
                                   : 1U;
            }
            while (auto datagram = session.receive_for_host())
            {
                expect(datagram->channel == openminerz_control_channel,
                       "mod frames stay on the control channel");
                server.receive(datagram->sender, datagram->payload, now);
                moved = true;
            }
            for (modding::ModNetSend &send : server.take_sends())
                refused += session.submit_from_host(send.peer, send.frame,
                                                    MessageDeliveryPolicy::reliable_ordered,
                                                    openminerz_control_channel)
                                   .accepted()
                               ? 0U
                               : 1U;
            for (auto &[peer, client] : clients)
                while (auto datagram = session.receive_for_peer(peer))
                {
                    client->receive(datagram->payload, now);
                    moved = true;
                }
            if (!moved)
                return;
        }
    }

    void tick()
    {
        now += 0.05;
        server.update(now);
        pump();
    }
};

void loopback_session_carries_the_mod_channel()
{
    const ModManifest manifest = modded_manifest();
    const HostSessionSettings settings = session_settings(manifest);
    LoopbackSession session{settings, false, 1, 1, false};
    expect(admit(session, settings, 101U, manifest) == AdmissionStatus::admitted,
           "an OpenMinerZ peer with the same manifest is admitted");
    ModNetServer server{manifest};
    ModNetClient client{manifest};
    SessionLink link{session, server};
    link.clients[101U] = &client;
    expect(server.add_peer(101U, player(1U), link.now), "the admitted peer gets a channel");
    link.pump();
    client.set_ready(link.now);
    link.pump();
    link.tick();
    expect(client.ready() && server.peer_ready(101U), "the channel opens through the session");

    Value echoed;
    server.on("test", "echo", [&](Handle who, const Value &payload) {
        server.send_to(who, "test", "echo", payload);
    });
    client.on("test", "echo", [&](const Value &payload) { echoed = payload; });
    // Four strings at the client's 4 KiB string limit: a three-fragment message.
    const Value long_payload{
        modding::Array{Value{std::string(3000U, 'a')}, Value{std::string(3000U, 'b')},
                       Value{std::string(3000U, 'c')}, Value{std::string(3000U, 'd')}}};
    expect(client.send("test", "echo", long_payload), "a fragmented message queues");
    for (int tick = 0; tick < 20 && echoed.is_nil(); ++tick)
        link.tick();
    expect(echoed == long_payload && link.refused == 0U,
           "a fragmented RPC crosses the real session both ways");
    server.set(StoreRef::player(player(1U)), "test", "thirst", Value{55});
    link.tick();
    const Value *thirst = client.get(StoreRef::player(player(1U)), "test", "thirst");
    expect(thirst != nullptr && *thirst == Value{55}, "replicated state crosses the session");

    // A stock (capability-silent) peer cannot enter a modded session at all.
    expect(admit(session, settings, 202U, {}, false) ==
               AdmissionStatus::rejected_extension_required,
           "a modded host rejects a stock client at admission");
    expect(!session
                .submit_from_host(202U, encode_mod_message(1U, std::vector<std::uint8_t>{1U})[0],
                                  MessageDeliveryPolicy::reliable_ordered,
                                  openminerz_control_channel)
                .accepted(),
           "and is never handed a mod frame");
}

void no_server_mods_means_no_new_bytes()
{
    // The same scripted stock exchange with and without a (disabled) mod layer
    // driven alongside must leave identical datagrams in both directions.
    const auto run = [](bool with_mod_layer) {
        const ModManifest manifest = vanilla_manifest();
        const HostSessionSettings settings = session_settings(manifest);
        LoopbackSession session{settings, false, 1, 1, false};
        std::vector<std::vector<std::uint8_t>> transcript;
        const auto record = [&](std::optional<Datagram> datagram, char tag) {
            while (datagram.has_value())
            {
                std::vector<std::uint8_t> entry{static_cast<std::uint8_t>(tag), datagram->channel,
                                                static_cast<std::uint8_t>(datagram->delivery)};
                entry.insert(entry.end(), datagram->payload.begin(), datagram->payload.end());
                transcript.push_back(std::move(entry));
                datagram = tag == 'h' ? session.receive_for_host() : session.receive_for_peer(301U);
            }
        };
        expect(admit(session, settings, 301U, manifest) == AdmissionStatus::admitted,
               "an OpenMinerZ peer joins a vanilla OpenMinerZ host");
        expect(admit(session, settings, 302U, {}, false) == AdmissionStatus::admitted,
               "a stock peer joins a host without server mods");
        std::optional<ModNetServer> server;
        std::optional<ModNetClient> client;
        if (with_mod_layer)
        {
            server.emplace(manifest);
            client.emplace(manifest);
            expect(!server->enabled() && !client->enabled(),
                   "no server mods: the layer is disabled");
            expect(!server->add_peer(301U, player(1U), 0.0), "and admits no peer");
            server->set(StoreRef::world(), "", "k", Value{1});
            expect(server->broadcast("", "x", Value{2}) == 0U, "broadcast addresses nobody");
            expect(!client->send("", "x", Value{3}), "a client send goes nowhere");
            client->set_ready(0.0);
        }
        PacketError packet_error{};
        const MessageDescriptor *chat = message_by_name(ProtocolVersion::classic_v3, "ChatMessage");
        const std::vector<std::uint8_t> chat_payload{5U, 'h', 'e', 'l', 'l', 'o'};
        const auto packet =
            encode_packet(ProtocolVersion::classic_v3, chat->id, chat_payload, packet_error);
        for (int step = 0; step < 3; ++step)
        {
            static_cast<void>(session.submit_from_peer(301U, *packet, chat->transport.delivery,
                                                       stock_gameplay_channel));
            static_cast<void>(session.submit_from_host(301U, *packet, chat->transport.delivery));
            if (with_mod_layer)
            {
                server->update(step);
                client->update(step);
                expect(server->take_sends().empty() && client->take_sends().empty(),
                       "the disabled layer never produces a frame");
            }
            record(session.receive_for_host(), 'h');
            record(session.receive_for_peer(301U), 'p');
        }
        // The stock peer cannot be handed a mod frame either.
        expect(!session
                    .submit_from_host(
                        302U, encode_mod_message(1U, std::vector<std::uint8_t>{1U})[0],
                        MessageDeliveryPolicy::reliable_ordered, openminerz_control_channel)
                    .accepted(),
               "no session without server mods carries a mod frame");
        return transcript;
    };
    const auto plain = run(false);
    const auto layered = run(true);
    expect(!plain.empty() && plain == layered,
           "the transcript is byte-identical with the mod layer present but idle");

    // The lobby advertisement for a manifest without server mods keeps its
    // bytes: the same frame the bitwise CRC of the original code produced.
    ExtensionMessage advertisement;
    advertisement.kind = ExtensionMessageKind::server_accept;
    advertisement.manifest = vanilla_manifest();
    const std::vector<std::uint8_t> packet = encode_extension_message(advertisement);
    const std::vector<std::uint8_t> expected_head{'O', 'M', 'Z', 'E', 1U, 2U, 3U, 0U, 0U, 0U,
                                                  0U,  0U,  0U,  0U,  0U, 0U, 0U, 0U, 0U};
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t value : expected_head)
    {
        crc ^= value;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    crc = ~crc;
    std::vector<std::uint8_t> expected = expected_head;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        expected.push_back(static_cast<std::uint8_t>((crc >> shift) & 0xFFU));
    expect(packet == expected, "the vanilla lobby advertisement is unchanged, byte for byte");
}

// --- Live Steam host routing (detached transport) ------------------------------

struct SteamHost final
{
    HostSessionSettings settings;
    LegacySteamRuntimeHost host;
    std::vector<SteamBridgeOutboundWire> outbox;
    std::chrono::steady_clock::time_point clock{std::chrono::seconds{1000}};

    explicit SteamHost(const ModManifest &manifest)
    {
        settings = session_settings(manifest);
        LegacySteamRuntimeHostOptions options;
        options.settings = settings;
        options.game_mode = 1;
        options.difficulty = 1;
        LegacySteamDetachedTransport transport;
        transport.send = [this](const SteamBridgeOutboundWire &wire, std::string &error) {
            outbox.push_back(wire);
            error.clear();
            return true;
        };
        transport.now = [this] { return clock; };
        std::string error;
        expect(host.start_detached(options, SteamBridgeHostReady{900U, 7'000'000U, "Host"},
                                   std::move(transport), error),
               error);
    }

    void receive(PeerId peer, std::vector<std::uint8_t> wire, std::uint8_t channel = 0U)
    {
        SteamBridgeClientEvent event;
        event.kind = SteamBridgeClientEventKind::inbound_wire;
        event.inbound = SteamBridgeInboundWire{peer, channel, std::move(wire)};
        host.inject(std::move(event));
    }

    void request_join(PeerId peer, std::string tag)
    {
        LegacyConnectionRequest request;
        request.network_version = static_cast<std::int32_t>(settings.protocol);
        request.session_id = 1234;
        request.gamer = LegacyGamerIdentity{std::vector<std::uint8_t>(16U, 0x42U), std::move(tag)};
        request.session_properties = make_session_properties(settings, false, 1, 1, false);
        request.session_properties[6] = 0;
        request.session_properties[7] = 0;
        LegacySteamCodecError error{};
        const auto payload = encode_legacy_connection_request(request, error);
        expect(payload.has_value(), "the join request encodes");
        const auto wire = encode_legacy_steam_wire(SteamIncomingMessageType::connection_approval,
                                                   0U, *payload, error);
        expect(wire.has_value(), "the join envelope encodes");
        receive(peer, *wire);
    }

    std::vector<LegacySteamRuntimeEvent> pump()
    {
        std::vector<LegacySteamRuntimeEvent> all;
        for (int round = 0; round < 8; ++round)
        {
            std::vector<LegacySteamRuntimeEvent> events;
            std::string error;
            expect(host.pump(events, error), error);
            if (events.empty())
                break;
            all.insert(all.end(), events.begin(), events.end());
        }
        return all;
    }
};

void steam_host_routes_mod_frames_only_for_modded_extension_peers()
{
    const auto frame = encode_mod_message(1U, std::vector<std::uint8_t>{2U, 1U})[0];
    {
        // Vanilla host: a stock peer and a mod frame from it.
        SteamHost vanilla{vanilla_manifest()};
        vanilla.request_join(101U, "Stock");
        static_cast<void>(vanilla.pump());
        vanilla.outbox.clear();
        vanilla.receive(101U, frame);
        const auto events = vanilla.pump();
        // Exactly the pre-existing treatment of a stray OMZE packet: the hello
        // path refuses it. No mod layer is involved.
        expect(!events.empty() &&
                   std::none_of(events.begin(), events.end(),
                                [](const LegacySteamRuntimeEvent &event) {
                                    return event.kind == LegacySteamRuntimeEventKind::mod_channel ||
                                           event.kind == LegacySteamRuntimeEventKind::mod_download;
                                }) &&
                   events[0].kind == LegacySteamRuntimeEventKind::rejected,
               "a vanilla host treats a mod frame as it always treated a stray extension packet");
        for (const SteamBridgeOutboundWire &wire : vanilla.outbox)
            expect(!is_mod_channel_packet(wire.wire) && !is_mod_download_packet(wire.wire),
                   "a vanilla host answers with no mod frame");
        vanilla.outbox.clear();
        std::string error;
        expect(!vanilla.host.send_mod_frame(101U, frame, error) && vanilla.outbox.empty(),
               "a vanilla host never sends a mod frame");
    }
    SteamHost modded{modded_manifest()};
    // A stock client is refused at approval with the retail denial (Authority:
    // the owner's choice, "ConnectionDenied").
    modded.request_join(102U, "Stock");
    static_cast<void>(modded.pump());
    bool denied = false;
    for (const SteamBridgeOutboundWire &wire : modded.outbox)
        if (wire.peer == 102U && wire.p2p_channel == 1U)
        {
            expect(wire.wire.front() == 0U && wire.wire.back() == 0U &&
                       std::string(wire.wire.begin() + 1, wire.wire.end() - 1) ==
                           "ConnectionDenied",
                   "the stock denial is a zero byte, the retail ConnectionDenied and a NUL");
            denied = true;
        }
    expect(denied, "a stock client is denied by a modded host");
    std::string error;
    expect(!modded.host.send_mod_frame(102U, frame, error), "and never sent a mod frame");

    // An OpenMinerZ peer: hello, then approval, then the mod channel opens.
    modded.outbox.clear();
    ExtensionMessage hello;
    hello.kind = ExtensionMessageKind::client_hello;
    hello.nonce = 77U;
    hello.manifest = modded_manifest();
    modded.receive(103U, encode_extension_message(hello));
    static_cast<void>(modded.pump());
    expect(!modded.host.mod_channel_peer(103U), "the hello alone does not open the channel");
    modded.request_join(103U, "Modder");
    static_cast<void>(modded.pump());
    expect(modded.host.mod_channel_peer(103U), "an admitted extension peer has a mod channel");
    modded.receive(103U, frame);
    const auto events = modded.pump();
    expect(events.size() == 1U && events[0].kind == LegacySteamRuntimeEventKind::mod_channel &&
               events[0].peer == 103U && events[0].inbound.payload == frame,
           "its mod frames reach the app with the transport's peer id");
    modded.outbox.clear();
    expect(modded.host.send_mod_frame(103U, frame, error) && modded.outbox.size() == 1U &&
               modded.outbox[0].p2p_channel == 0U && modded.outbox[0].wire == frame,
           "the host can send it mod frames on the reliable path");

    // A download frame from a lobby member that is not admitted is still routed.
    ModDownloadRequest request{5U, {}, {"test"}};
    modded.receive(104U, encode_download_request(request));
    const auto download = modded.pump();
    expect(download.size() == 1U && download[0].kind == LegacySteamRuntimeEventKind::mod_download,
           "a download request reaches the app before admission");
}

// --- Download --------------------------------------------------------------------

struct TempDir final
{
    std::filesystem::path path;
    explicit TempDir(std::string_view name)
    {
        path = std::filesystem::temp_directory_path() /
               ("omz-mod-net-" + std::string{name} + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void write_file(const std::filesystem::path &path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void make_mod(const std::filesystem::path &root, std::string_view id, bool with_server,
              std::size_t asset_bytes = 0U)
{
    write_file(root / "openminerz.mod",
               "id=" + std::string{id} +
                   "\nversion=1.2.0\nscope=server\nprotocol=both\norder=10\n");
    write_file(root / "shared" / "items.luau", "return { canteen = true }\n");
    write_file(root / "client" / "hud.luau", "-- draws the thirst bar\n");
    write_file(root / "lang" / "en.txt", "thirst=Thirst\n");
    if (asset_bytes != 0U)
    {
        std::string bytes(asset_bytes, '\0');
        for (std::size_t index = 0U; index < bytes.size(); ++index)
            bytes[index] = static_cast<char>((index * 131U) & 0xFFU);
        write_file(root / "assets" / "big.png", bytes);
    }
    if (with_server)
    {
        write_file(root / "server" / "secret.luau", "-- loot tables nobody may read\n");
        write_file(root / "tools" / "build.ps1", "Write-Host hi\n");
        write_file(root / "client" / "evil.dll", "MZ not really\n");
    }
}

LoadedMod load_one(const std::filesystem::path &addons)
{
    ModCatalog catalog;
    std::string error;
    expect(load_mod_catalog(addons, catalog, error) && catalog.mods.size() == 1U,
           "the test mod loads: " + error);
    return catalog.mods[0];
}

void client_visible_digest_and_archive()
{
    TempDir temp{"digest"};
    make_mod(temp.path / "addons" / "survival", "survival", true);
    const LoadedMod mod = load_one(temp.path / "addons");
    ModArchive archive;
    std::string error;
    expect(build_client_archive(mod.root, default_maximum_mod_download_bytes, archive, error),
           "the client part packs: " + error);
    for (const ModArchiveFile &file : archive.files)
        expect(!file.path.starts_with("server/") && !file.path.starts_with("tools/") &&
                   !file.path.ends_with(".dll"),
               "server/, other folders and executables never enter the archive");
    expect(archive.files.size() == 4U, "descriptor, shared, client and lang are packed");
    ModDigest client{};
    std::uint64_t bytes{};
    expect(client_visible_digest(mod.root, client, bytes, error) &&
               client == archive.client_digest && bytes == archive.bytes.size(),
           "the archive's SHA-256 is the client-visible digest");
    expect(client != mod.descriptor.content_digest,
           "a mod with server files has a different client digest");

    make_mod(temp.path / "pure" / "pure", "pure", false);
    const LoadedMod pure = load_one(temp.path / "pure");
    ModArchive pure_archive;
    expect(build_client_archive(pure.root, default_maximum_mod_download_bytes, pure_archive, error),
           "a client-only mod packs");
    expect(pure_archive.client_digest == pure.descriptor.content_digest,
           "for a mod with only client-visible files the two digests are equal");

    std::vector<ModArchiveFile> parsed;
    expect(parse_client_archive(archive.bytes, parsed, error) && parsed.size() == 4U,
           "a host archive parses: " + error);
}

std::vector<std::uint8_t> forged_archive(std::vector<std::pair<std::string, std::string>> files)
{
    std::vector<std::uint8_t> bytes;
    for (const auto &[path, content] : files)
    {
        bytes.insert(bytes.end(), path.begin(), path.end());
        bytes.push_back(0U);
        const std::uint64_t size = content.size();
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
            bytes.push_back(static_cast<std::uint8_t>((size >> shift) & 0xFFU));
        bytes.insert(bytes.end(), content.begin(), content.end());
    }
    return bytes;
}

void hostile_archives_are_refused()
{
    const std::string descriptor = "id=x\nversion=1\nscope=server\nprotocol=both\norder=1\n";
    const auto refused = [&](std::vector<std::pair<std::string, std::string>> extra,
                             std::string_view why) {
        std::vector<std::pair<std::string, std::string>> files = std::move(extra);
        files.emplace_back("openminerz.mod", descriptor);
        std::sort(files.begin(), files.end());
        std::vector<ModArchiveFile> parsed;
        std::string error;
        expect(!parse_client_archive(forged_archive(files), parsed, error), why);
    };
    refused({{"../evil.luau", "x"}}, "a parent traversal is refused");
    refused({{"client/../../evil.luau", "x"}}, "an embedded traversal is refused");
    refused({{"/client/a.luau", "x"}}, "an absolute path is refused");
    refused({{"client\\a.luau", "x"}}, "a backslash is refused");
    refused({{"C:/client/a.luau", "x"}}, "a drive letter is refused");
    refused({{"server/main.luau", "x"}}, "server/ is never accepted from a host");
    refused({{"client/run.dll", "MZ"}}, "an executable type is refused");
    refused({{"client/con.luau", "x"}}, "a Windows device name is refused");
    refused({{"client/a.luau.", "x"}}, "a trailing dot is refused");
    refused({{"client/A.luau", "x"}}, "upper case (never produced by the host) is refused");
    refused({{"client/x.luau", "x"}, {"client/x.luau/y.luau", "y"}},
            "a file used as a folder is refused");
    std::vector<ModArchiveFile> parsed;
    std::string error;
    auto unordered = forged_archive({{"openminerz.mod", descriptor}, {"client/a.luau", "x"}});
    expect(!parse_client_archive(unordered, parsed, error), "out-of-order paths are refused");
    auto truncated = forged_archive({{"client/a.luau", "xyz"}, {"openminerz.mod", descriptor}});
    truncated.resize(truncated.size() - 3U);
    expect(!parse_client_archive(truncated, parsed, error), "a truncated archive is refused");
    expect(!parse_client_archive(forged_archive({{"client/a.luau", "x"}}), parsed, error),
           "an archive without a descriptor is refused");
}

// Runs a download between a host and a joiner through the LoopbackSession's
// pre-admission control channel. `tamper` may corrupt host frames in transit.
struct DownloadRun final
{
    LoopbackSession &session;
    ModDownloadServer &server;
    ModDownloadClient &client;
    PeerId peer{};
    double now{};
    std::function<void(std::vector<std::uint8_t> &)> tamper;

    void run(bool accept, int limit = 4000)
    {
        client.start(now);
        for (int step = 0; step < limit && !client.status().finished(); ++step)
        {
            now += 0.01;
            if (accept && client.status().phase == ModDownloadPhase::awaiting_consent)
                client.accept(now);
            for (const auto &frame : client.take_sends())
                expect(session
                           .submit_from_peer(peer, frame, MessageDeliveryPolicy::reliable_ordered,
                                             openminerz_control_channel)
                           .accepted(),
                       "download requests pass the pre-admission gate");
            while (auto datagram = session.receive_for_host())
                server.receive(datagram->sender, datagram->payload, now);
            server.update(now);
            for (ModDownloadSend &send : server.take_sends())
            {
                if (tamper)
                    tamper(send.frame);
                static_cast<void>(session.submit_from_host(send.peer, send.frame,
                                                           MessageDeliveryPolicy::reliable_ordered,
                                                           openminerz_control_channel));
            }
            while (auto datagram = session.receive_for_peer(peer))
                client.receive(datagram->payload, now);
            client.update(now);
        }
    }
};

void mod_download_on_join_end_to_end()
{
    TempDir temp{"download"};
    make_mod(temp.path / "host" / "survival", "survival", true, 50000U);
    const LoadedMod host_mod = load_one(temp.path / "host");
    ModManifest manifest;
    manifest.protocol = ProtocolVersion::classic_v3;
    manifest.mods.push_back(host_mod.descriptor);
    const HostSessionSettings settings = session_settings(manifest);

    // The joiner has nothing: the browser preflight names what is missing.
    const SessionModSelection before =
        select_session_mods({}, {}, &manifest, ProtocolVersion::classic_v3);
    expect(before.missing.size() == 1U && before.missing[0] == host_mod.descriptor,
           "the missing server mod is identified from the lobby manifest");

    LoopbackSession session{settings, false, 1, 1, false};
    PeerAdmissionRequest request;
    request.peer = 501U;
    request.properties = make_session_properties(settings, false, 1, 1, false);
    request.advertised_extension = true;
    expect(session.connect(request).status == AdmissionStatus::awaiting_extension,
           "the joiner waits for its hello");

    ModDownloadServer server{manifest, {host_mod}};
    std::string error;
    expect(server.prepare(error), "the host packs its client parts: " + error);
    ModDownloadClientOptions options;
    options.cache_root = temp.path / "cache";
    ModDownloadClient client{before.missing, options};
    DownloadRun run{session, server, client, 501U};
    client.start(run.now);
    // Stop at the prompt: sizes are known before anything is downloaded.
    for (int step = 0; step < 50 && client.status().phase != ModDownloadPhase::awaiting_consent;
         ++step)
    {
        for (const auto &frame : client.take_sends())
            static_cast<void>(session.submit_from_peer(
                501U, frame, MessageDeliveryPolicy::reliable_ordered, openminerz_control_channel));
        while (auto datagram = session.receive_for_host())
            server.receive(datagram->sender, datagram->payload, run.now);
        for (ModDownloadSend &send : server.take_sends())
            static_cast<void>(session.submit_from_host(send.peer, send.frame,
                                                       MessageDeliveryPolicy::reliable_ordered,
                                                       openminerz_control_channel));
        while (auto datagram = session.receive_for_peer(501U))
            client.receive(datagram->payload, run.now);
    }
    const ModDownloadStatus &prompt = client.status();
    expect(prompt.phase == ModDownloadPhase::awaiting_consent && prompt.mods.size() == 1U &&
               prompt.total_bytes > 50000U && prompt.received_bytes == 0U,
           "the prompt shows the mod and its size before any byte of it is sent");
    const ModDownloadPromptText text = describe_mod_download(prompt);
    expect(text.body.find("survival") != std::string::npos && !text.accept_label.empty(),
           "the text fallback names the mod and offers a choice");
    client.accept(run.now);
    for (int step = 0; step < 4000 && !client.status().finished(); ++step)
    {
        run.now += 0.01;
        for (const auto &frame : client.take_sends())
            static_cast<void>(session.submit_from_peer(
                501U, frame, MessageDeliveryPolicy::reliable_ordered, openminerz_control_channel));
        while (auto datagram = session.receive_for_host())
            server.receive(datagram->sender, datagram->payload, run.now);
        server.update(run.now);
        for (ModDownloadSend &send : server.take_sends())
            static_cast<void>(session.submit_from_host(send.peer, send.frame,
                                                       MessageDeliveryPolicy::reliable_ordered,
                                                       openminerz_control_channel));
        while (auto datagram = session.receive_for_peer(501U))
            client.receive(datagram->payload, run.now);
        client.update(run.now);
    }
    expect(client.status().phase == ModDownloadPhase::complete,
           std::string{"the download completes: "} + client.status().detail);

    const ModCacheLoad cache = load_mod_cache(options.cache_root);
    expect(cache.catalog.mods.size() == 1U && cache.skipped.empty() &&
               cache.catalog.mods[0].descriptor == host_mod.descriptor,
           "the cache holds it under the host's content digest");
    expect(!std::filesystem::exists(cache.catalog.mods[0].root / "server") &&
               !std::filesystem::exists(cache.catalog.mods[0].root / "tools"),
           "server/ never reached the joiner");
    const SessionModSelection after =
        select_session_mods({}, cache.catalog, &manifest, ProtocolVersion::classic_v3);
    expect(after.missing.empty(), "nothing is missing any more");

    // And the hello with that manifest is admitted.
    ExtensionMessage hello;
    hello.kind = ExtensionMessageKind::client_hello;
    hello.nonce = 99U;
    hello.manifest = after.manifest;
    static_cast<void>(session.submit_from_peer(501U, encode_extension_message(hello),
                                               MessageDeliveryPolicy::reliable_ordered,
                                               openminerz_control_channel));
    expect(session.admission().status(501U) == AdmissionStatus::admitted,
           "the downloaded mod satisfies admission");

    // A tampered cache entry is skipped, never loaded.
    write_file(cache.catalog.mods[0].root / "client" / "hud.luau", "-- tampered\n");
    const ModCacheLoad tampered = load_mod_cache(options.cache_root);
    expect(tampered.catalog.mods.empty() && tampered.skipped.size() == 1U,
           "a modified cache entry fails its client digest and is skipped");
}

void hostile_downloads_fail_safely()
{
    TempDir temp{"hostile"};
    make_mod(temp.path / "host" / "survival", "survival", true, 20000U);
    const LoadedMod host_mod = load_one(temp.path / "host");
    ModManifest manifest;
    manifest.protocol = ProtocolVersion::classic_v3;
    manifest.mods.push_back(host_mod.descriptor);
    HostSessionSettings settings = session_settings(manifest);

    const auto attempt = [&](ModDownloadServerOptions server_options,
                             ModDownloadClientOptions client_options,
                             std::function<void(std::vector<std::uint8_t> &)> tamper) {
        LoopbackSession session{settings, false, 1, 1, false};
        PeerAdmissionRequest request;
        request.peer = 601U;
        request.properties = make_session_properties(settings, false, 1, 1, false);
        request.password = settings.password;
        request.advertised_extension = true;
        static_cast<void>(session.connect(request));
        ModDownloadServer server{manifest, {host_mod}, std::move(server_options)};
        client_options.cache_root = temp.path / "cache";
        ModDownloadClient client{{host_mod.descriptor}, std::move(client_options)};
        DownloadRun run{session, server, client, 601U, 0.0, std::move(tamper)};
        run.run(true);
        return client.status();
    };

    // A chunk corrupted in transit (CRC recomputed, as a hostile host would).
    const ModDownloadStatus corrupted = attempt({}, {}, [](std::vector<std::uint8_t> &frame) {
        if (extension_message_kind(frame) != ExtensionMessageKind::download_chunk)
            return;
        ModChunk chunk;
        if (!decode_chunk(frame, chunk) || chunk.index != 2U)
            return;
        chunk.bytes[10] ^= 0xFFU;
        frame = encode_chunk(chunk);
    });
    expect(corrupted.phase == ModDownloadPhase::failed &&
               corrupted.failure == ModDownloadFailure::digest_mismatch,
           "a digest mismatch fails the download");
    expect(!std::filesystem::exists(temp.path / "cache" /
                                    digest_hex(host_mod.descriptor.content_digest)),
           "and nothing is installed");

    ModDownloadClientOptions small;
    small.maximum_mod_bytes = 1000U;
    const ModDownloadStatus oversize = attempt({}, small, {});
    expect(oversize.phase == ModDownloadPhase::failed &&
               oversize.failure == ModDownloadFailure::too_large && oversize.received_bytes == 0U,
           "an oversize mod is refused at the offer, before any chunk");

    settings.password = "secret";
    ModDownloadServerOptions locked;
    locked.password = "secret";
    const ModDownloadStatus wrong = attempt(locked, {}, {});
    expect(wrong.phase == ModDownloadPhase::failed &&
               wrong.failure == ModDownloadFailure::wrong_password,
           "a password-protected host refuses a joiner without the password");
    ModDownloadClientOptions with_password;
    with_password.password = "secret";
    const ModDownloadStatus right = attempt(locked, with_password, {});
    expect(right.phase == ModDownloadPhase::complete, "the right password downloads");
    settings.password.clear();

    // An offer whose content digest differs from the lobby's is refused.
    ModDownloadClientOptions plain;
    plain.cache_root = temp.path / "cache2";
    ModDescriptor advertised = host_mod.descriptor;
    advertised.content_digest[0] ^= 1U;
    {
        LoopbackSession session{settings, false, 1, 1, false};
        PeerAdmissionRequest request;
        request.peer = 602U;
        request.properties = make_session_properties(settings, false, 1, 1, false);
        request.advertised_extension = true;
        static_cast<void>(session.connect(request));
        ModDownloadServer server{manifest, {host_mod}};
        ModDownloadClient client{{advertised}, plain};
        DownloadRun run{session, server, client, 602U};
        run.run(true);
        expect(client.status().failure == ModDownloadFailure::offer_mismatch,
               "an offer that differs from the advertisement is refused");
    }

    // Host-side abuse: stray chunk requests, oversized windows, request floods.
    ModDownloadServer server{manifest, {host_mod}};
    server.receive(700U, encode_chunk_request({1U, 0U, 0U, 4U}), 0.0);
    expect(server.take_sends().empty(), "a chunk request without a request gets no answer");
    server.receive(700U, encode_download_request({9U, {}, {"survival"}}), 0.0);
    expect(server.take_sends().size() == 1U, "a request is answered with one offer");
    server.receive(700U, encode_chunk_request({9U, 0U, 0U, 1000U}), 0.0);
    const auto refusal = server.take_sends();
    ModDownloadRefusal decoded;
    expect(refusal.size() == 1U && decode_download_refusal(refusal[0].frame, decoded) &&
               decoded.reason == ModDownloadRefusalReason::protocol && server.active_peers() == 0U,
           "an oversized window is refused and the state dropped");
    for (int index = 0; index < 10; ++index)
        server.receive(
            701U,
            encode_download_request({static_cast<std::uint32_t>(index + 1), {}, {"survival"}}),
            0.0);
    bool quota = false;
    for (const ModDownloadSend &send : server.take_sends())
        if (decode_download_refusal(send.frame, decoded) &&
            decoded.reason == ModDownloadRefusalReason::quota_exceeded)
            quota = true;
    expect(quota, "a request flood runs into the per-peer request quota");
    ModDownloadServerOptions crowded;
    crowded.maximum_concurrent_peers = 1U;
    ModDownloadServer busy{manifest, {host_mod}, crowded};
    busy.receive(800U, encode_download_request({1U, {}, {"survival"}}), 0.0);
    static_cast<void>(busy.take_sends());
    busy.receive(801U, encode_download_request({2U, {}, {"survival"}}), 0.0);
    const auto busy_reply = busy.take_sends();
    expect(busy_reply.size() == 1U && decode_download_refusal(busy_reply[0].frame, decoded) &&
               decoded.reason == ModDownloadRefusalReason::busy,
           "concurrent downloads are capped");
    ModDownloadServer vanilla{vanilla_manifest(), {}};
    vanilla.receive(900U, encode_download_request({1U, {}, {"survival"}}), 0.0);
    expect(!vanilla.enabled() && vanilla.take_sends().empty(),
           "a host without server mods serves nothing");
}

// --- Script bindings -------------------------------------------------------------

struct LinkedBackend final : modding::NetBackend
{
    ModNetServer *server_{};
    ModNetClient *client_{};
    ModNetServer *server() noexcept override
    {
        return server_;
    }
    ModNetClient *client() noexcept override
    {
        return client_;
    }
};

void scripts_use_net_and_replicated()
{
    const ModManifest manifest = modded_manifest({"test"});
    ModNetServer server{manifest};
    ModNetClient client{manifest};
    LinkedBackend backend;
    backend.server_ = &server;
    backend.client_ = &client;
    modding::testing::Harness harness;
    modding::register_net_api(harness.registry(), harness.host(), backend);
    modding::connect_net_events(harness.host(), &server, &client);
    double now = 0.0;
    const auto pump = [&] {
        now += 0.05;
        server.update(now);
        modding::pump_mod_net_loopback(server, client, modding::local_mod_peer, now, {}, 32U);
    };

    auto result = harness.run(R"(
        ready_players = {}
        events.on("net.player_ready", function(e) table.insert(ready_players, e.player) end)
        net.on("drink", function(player, payload)
            local thirst = replicated.get(player, "thirst") or 100
            replicated.set(player, "thirst", thirst - payload.amount)
            net.send_to(player, "drank", { left = thirst - payload.amount })
        end, { amount = { type = "integer", min = 1, max = 50 } })
        replicated.set("world", "day", 1)
        return true
    )",
                              Side::server);
    expect(result.ok, "server script: " + result.error);
    expect(server.add_peer(modding::local_mod_peer, player(1U), now), "the local player joins");
    result = harness.run(R"(
        changes = {}
        replies = {}
        replicated.on_change("player", "thirst", function(value, old, key, owner)
            table.insert(changes, value)
        end)
        net.on("drank", function(payload) table.insert(replies, payload.left) end)
        return true
    )",
                         Side::client);
    expect(result.ok, "client script: " + result.error);
    pump();
    client.set_ready(now);
    pump();
    pump();
    result = harness.run("return #ready_players", Side::server);
    expect(result.ok && result.value == Value{1}, "net.player_ready fired on the server");
    result = harness.run(R"(
        assert(net.is_ready(), "snapshot complete")
        assert(replicated.get("world", "day") == 1, "world state")
        net.send("drink", { amount = 30 })
        net.send("drink", { amount = 500 })  -- out of range: dropped before the handler
        return net.local_player()
    )",
                         Side::client);
    expect(result.ok && result.value == Value{player(1U)}, "client sends: " + result.error);
    pump();
    pump();
    result = harness.run(R"(
        return replicated.get(net.local_player(), "thirst"), #changes, replies[1]
    )",
                         Side::client);
    expect(result.ok, result.error);
    expect(result.value == Value{70}, "the server handler wrote replicated state the client reads");
    result =
        harness.run("return #changes == 1 and replies[1] == 70 and #replies == 1", Side::client);
    expect(result.ok && result.value == Value{true},
           "on_change and the reply ran once; the out-of-range payload never reached the handler");
    result = harness.run(R"(net.send("x", { f = function() end }))", Side::client);
    expect(!result.ok, "a function in a payload is a script error");
    result = harness.run(R"(replicated.set("world", "k", 1))", Side::client);
    expect(!result.ok, "clients cannot write replicated state");
}

// --- Measurements -------------------------------------------------------------------

void measure_throughput_and_bandwidth()
{
    const modding::ValueLimits limits;
    const Value delta{Map{{"hunger", Value{73.5}},
                          {"thirst", Value{41}},
                          {"stamina", Value{0.875}},
                          {"temperature", Value{36.6}}}};
    std::vector<std::uint8_t> bytes;
    std::string error;
    constexpr int iterations = 200000;
    auto start = std::chrono::steady_clock::now();
    std::size_t encoded_bytes{};
    for (int index = 0; index < iterations; ++index)
    {
        bytes.clear();
        static_cast<void>(modding::encode_value(delta, limits, bytes, error));
        encoded_bytes += bytes.size();
    }
    const double encode_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Value decoded;
    start = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations; ++index)
        static_cast<void>(modding::decode_value(bytes, limits, decoded, error));
    const double decode_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    expect(decoded == delta, "the measured delta round-trips");
    std::cout << std::format("measure: value delta (4 stats) = {} bytes; encode {:.0f}/s ({:.1f} "
                             "MB/s), decode {:.0f}/s ({:.1f} MB/s)\n",
                             bytes.size(), iterations / encode_seconds,
                             encoded_bytes / encode_seconds / 1.0e6, iterations / decode_seconds,
                             encoded_bytes / decode_seconds / 1.0e6);

    // 16 players, 4 stats each, every stat changing every tick.
    for (const bool is_public : {false, true})
    {
        const ModManifest manifest = modded_manifest({"survival"});
        modding::ModNetLimits net_limits;
        net_limits.replication_bytes_per_second = 1.0e9;
        net_limits.replication_burst_bytes = 1.0e9;
        ModNetServer server{manifest, net_limits};
        std::vector<std::unique_ptr<ModNetClient>> clients;
        Link link{server};
        for (std::uint64_t index = 1U; index <= 16U; ++index)
        {
            clients.push_back(std::make_unique<ModNetClient>(manifest, net_limits));
            link.join(1000U + index, *clients.back(), player(index));
        }
        const char *stats[]{"hunger", "thirst", "stamina", "temperature"};
        std::uint64_t bytes_total{};
        constexpr int ticks = 200;
        start = std::chrono::steady_clock::now();
        for (int tick = 0; tick < ticks; ++tick)
        {
            for (std::uint64_t index = 1U; index <= 16U; ++index)
                for (int stat = 0; stat < 4; ++stat)
                    server.set(StoreRef::player(player(index)), "survival", stats[stat],
                               Value{100.0 - tick * 0.25 - stat - static_cast<double>(index) * 0.5},
                               modding::ReplicatedKeyOptions{is_public,
                                                             modding::ReplicationPriority::normal});
            link.now += 0.05;
            server.update(link.now);
            for (modding::ModNetSend &send : server.take_sends())
            {
                bytes_total += send.frame.size();
                link.clients[send.peer]->receive(send.frame, link.now);
            }
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const Value *last = clients[3]->get(StoreRef::player(player(16U)), "survival", "thirst");
        expect(is_public ? last != nullptr : last == nullptr,
               "public stats reach every player, private ones only their owner");
        std::cout << std::format(
            "measure: 16 players x 4 {} stats, all changing every tick: {:.0f} bytes/tick on the "
            "wire ({:.1f} KiB/s at 20 Hz), {:.0f} bytes/tick per player; host+16 clients "
            "{:.1f} us/tick\n",
            is_public ? "public" : "private", static_cast<double>(bytes_total) / ticks,
            static_cast<double>(bytes_total) / ticks * 20.0 / 1024.0,
            static_cast<double>(bytes_total) / ticks / 16.0, seconds / ticks * 1.0e6);
    }
}

} // namespace

int main()
{
    const std::pair<const char *, void (*)()> tests[]{
        {"value codec round-trips and refuses hostile input",
         value_codec_round_trips_and_refuses_hostile_input},
        {"payload schemas drop bad payloads", payload_schema_drops_bad_payloads},
        {"frames fragment and reassemble strictly", frames_fragment_and_reassemble_strictly},
        {"rpc namespaces, identity and schema", rpc_namespaces_identity_and_schema},
        {"replication scopes, snapshot, delta and events",
         replication_scopes_snapshot_delta_and_events},
        {"replication respects bandwidth and priority",
         replication_respects_bandwidth_and_priority},
        {"content registry reaches clients", content_registry_reaches_clients},
        {"hostile clients are contained", hostile_clients_are_contained},
        {"client rate limit is paced, not punished", client_rate_limit_is_paced_not_punished},
        {"loopback session carries the mod channel", loopback_session_carries_the_mod_channel},
        {"no server mods means no new bytes", no_server_mods_means_no_new_bytes},
        {"steam host routes mod frames only for modded extension peers",
         steam_host_routes_mod_frames_only_for_modded_extension_peers},
        {"client-visible digest and archive", client_visible_digest_and_archive},
        {"hostile archives are refused", hostile_archives_are_refused},
        {"mod download on join, end to end", mod_download_on_join_end_to_end},
        {"hostile downloads fail safely", hostile_downloads_fail_safely},
        {"scripts use net and replicated", scripts_use_net_and_replicated},
        {"measure throughput and bandwidth", measure_throughput_and_bandwidth},
    };
    int failed = 0;
    for (const auto &[name, test] : tests)
    {
        try
        {
            test();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception &exception)
        {
            ++failed;
            std::cout << "FAIL " << name << ": " << exception.what() << '\n';
        }
    }
    std::cout << (std::size(tests) - static_cast<std::size_t>(failed)) << "/" << std::size(tests)
              << " mod net tests passed\n";
    return failed == 0 ? 0 : 1;
}
