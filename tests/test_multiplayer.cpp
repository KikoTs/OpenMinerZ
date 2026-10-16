#include "multiplayer_fixtures.hpp"
#include "openminerz/net/bootstrap_gameplay_buffer.hpp"
#include "openminerz/net/client_gameplay_authority.hpp"
#include "openminerz/net/crate_focus.hpp"
#include "openminerz/net/inventory_save_barrier.hpp"
#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/peer_inventory_store.hpp"
#include "openminerz/net/player_replication.hpp"
#include "openminerz/net/steam_bridge_protocol.hpp"
#include "openminerz/net/voice_codec.hpp"
#include "openminerz/world/pickup_overflow_queue.hpp"

#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace
{
using namespace openminerz;
using namespace openminerz::net;
void check(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error{std::string{message}};
}

LegacySteamHostSessionOptions host_options(ProtocolVersion protocol)
{
    LegacySteamHostSessionOptions options;
    options.settings.protocol = protocol;
    options.settings.mod_manifest.protocol = protocol;
    options.settings.visibility = HostVisibility::public_lobby;
    options.host_gamer = {{9U, 9U}, "Host"};
    return options;
}

struct Network
{
    static constexpr PeerId host_peer = 900U, alice = 901U, bob = 902U;
    ProtocolVersion protocol;
    LegacySteamHostSession host;
    std::map<PeerId, LegacySteamClientSession> clients;
    explicit Network(ProtocolVersion p) : protocol{p}, host{host_options(p)}
    {
        join(alice, "Alice");
        join(bob, "Bob");
    }
    void join(PeerId peer, std::string name)
    {
        LegacySteamClientSessionOptions options;
        options.protocol = protocol;
        options.host_peer = host_peer;
        options.expected_host_name = "Host";
        options.session_properties = host.session_properties();
        options.local_gamer = {{static_cast<std::uint8_t>(peer), 1U}, std::move(name)};
        clients.erase(peer);
        auto &client = clients.emplace(peer, std::move(options)).first->second;
        std::string error;
        const auto request = client.begin(error);
        check(request.has_value(), error);
        const auto admission = host.process_wire_packet(peer, request->payload);
        check(!admission.audit && admission.inbound.size() == 1U, "stock admission failed");
        for (const auto &send : admission.sends)
        {
            const auto received = deliver(send);
            check(!received.audit, "client rejected admission/roster update");
        }
        check(client.connected() && client.local_gid() == host.gid_for(peer),
              "assigned GID differs between peers");
    }
    LegacySteamClientSessionResult deliver(const LegacySteamNativeSend &send)
    {
        return clients.at(send.remote_peer)
            .process_native_packet(host_peer, send.p2p_channel, send.payload);
    }
    LegacySteamHostInbound send(PeerId peer, const std::vector<std::uint8_t> &packet,
                                MessageDeliveryPolicy delivery,
                                std::optional<std::uint8_t> target = std::nullopt)
    {
        std::string error;
        const auto wire = clients.at(peer).create_outbound(target, packet, delivery, error);
        check(wire.has_value(), error);
        const auto received = host.process_wire_packet(peer, wire->payload);
        check(!received.audit && received.inbound.size() == 1U,
              "host rejected correctly routed typed packet");
        return received.inbound[0];
    }
    void reply(const LegacySteamOutbound &outbound, std::size_t expected_count,
               std::uint8_t expected_sender)
    {
        std::string error;
        const auto sends = host.create_outbound(outbound, error);
        check(sends.has_value(), error);
        check(sends->size() == expected_count, "wrong direct/broadcast recipient count");
        for (const auto &send : *sends)
        {
            const auto delivered = deliver(send);
            check(!delivered.audit && delivered.inbound.size() == 1U,
                  "client rejected host/relay packet");
            check(delivered.inbound[0].gid == expected_sender &&
                      delivered.inbound[0].payload == outbound.application_packet,
                  "relay changed originating player or packet bytes");
        }
    }
};

std::vector<std::uint8_t> frame(ProtocolVersion protocol, std::string_view name,
                                std::span<const std::uint8_t> payload)
{
    PacketError error{};
    const auto *type = message_by_name(protocol, name);
    check(type != nullptr, "fixture is absent from original registry");
    auto result = encode_packet(protocol, type->id, payload, error);
    check(result.has_value(), "fixture cannot be framed");
    return std::move(*result);
}

void all_packets(ProtocolVersion protocol)
{
    Network network{protocol};
    const auto fixtures = test::multiplayer_fixtures(protocol);
    check(fixtures.size() == message_manifest(protocol).size(),
          "fixture catalog must cover every stock packet");
    // RestartLevelMessage is deliberately NOT here: the original's handler
    // checks neither the sender nor the mode and a stock host relays it from
    // anyone, so a guest's restart must be relayed (and applied) like any
    // other broadcast instead of leaving that guest restarted alone.
    const std::set<std::string_view> host_only{
        "AppointServerMessage", "ConsumePickupMessage", "InventoryRetrieveFromServerMessage",
        "KickMessage",          "ProvideChunkMessage",  "ProvideDeltaListMessage",
        "SpawnDragonMessage",   "TimeOfDayMessage",     "UpdateSpawnerMessage",
        "WorldInfoMessage"};
    const std::set<std::string_view> direct{"ClientReadyForChunksMessage", "RequestChunkMessage",
                                            "RequestWorldInfoMessage", "MeleePlayerMessage"};
    std::size_t covered{};
    for (const auto &descriptor : message_manifest(protocol))
    {
        try
        {
            const auto &payload = fixtures.at(std::string{descriptor.name});
            PayloadError payload_error{};
            check(validate_known_payload(protocol, descriptor, payload, payload_error),
                  "typed fixture rejected");
            const auto packet = frame(protocol, descriptor.name, payload);
            auto delivery = descriptor.transport.delivery;
            if (delivery == MessageDeliveryPolicy::player_update_state)
                delivery = MessageDeliveryPolicy::unreliable_sequenced;
            if (delivery == MessageDeliveryPolicy::dragon_update_warmup)
                delivery = MessageDeliveryPolicy::unreliable;
            const auto target = direct.contains(descriptor.name)
                                    ? std::optional<std::uint8_t>{std::uint8_t{0}}
                                    : std::nullopt;
            const auto inbound = network.send(Network::alice, packet, delivery, target);
            const auto decision = assess_client_gameplay_authority(protocol, inbound);
            if (host_only.contains(descriptor.name))
            {
                check(!decision.accepted() &&
                          decision.error == ClientGameplayAuthorityError::host_only_message,
                      "client was allowed to impersonate session authority");
                LegacySteamOutbound outgoing;
                outgoing.application_packet = packet;
                outgoing.delivery = delivery;
                network.reply(outgoing, 2U, 0U);
                outgoing.recipient = Network::alice;
                network.reply(outgoing, 1U, 0U);
            }
            else
            {
                check(decision.accepted(),
                      "valid source role/route rejected by application authority");
                if (decision.should_relay())
                {
                    LegacySteamOutbound relay;
                    relay.application_packet = packet;
                    relay.delivery = delivery;
                    relay.client_relay = true;
                    relay.channel = 1U;
                    relay.sender_gid = 1U;
                    network.reply(relay, 1U, 1U);
                }
            }
            // Every packet must survive actual wire framing, but no corrupted
            // or unauthenticated copy may reach the application.
            std::string error;
            auto wire =
                network.clients.at(Network::alice).create_outbound(target, packet, delivery, error);
            check(wire.has_value(), error);
            check(network.host.process_wire_packet(999U, wire->payload).inbound.empty(),
                  "unadmitted peer injected gameplay");
            wire->payload.back() ^= 0x80U;
            const auto corrupt = network.host.process_wire_packet(Network::alice, wire->payload);
            check(corrupt.audit.has_value() && corrupt.inbound.empty(),
                  "corrupt packet reached application");
            ++covered;
            std::cout << static_cast<int>(protocol) << ' ' << descriptor.name << " PASS\n";
        }
        catch (const std::exception &error)
        {
            throw std::runtime_error{std::string{descriptor.name} + ": " + error.what()};
        }
    }
    // Direct peer-to-peer melee must keep the attacker identity.
    const auto melee = frame(protocol, "MeleePlayerMessage", fixtures.at("MeleePlayerMessage"));
    const auto attack = network.send(Network::alice, melee, MessageDeliveryPolicy::reliable_ordered,
                                     std::uint8_t{2});
    check(assess_client_gameplay_authority(protocol, attack).should_relay(),
          "direct melee route rejected");
    LegacySteamOutbound relay;
    relay.application_packet = melee;
    relay.client_relay = true;
    relay.channel = 1U;
    relay.sender_gid = 1U;
    relay.recipient_gid = 2U;
    network.reply(relay, 1U, 1U);
    // Leaving and returning invalidates the old generation on both sides.
    const auto drops = network.host.remove_peer(Network::alice);
    for (const auto &drop : drops)
        check(!network.deliver(drop).audit, "peer removal failed");
    check(network.clients.at(Network::bob).gamer_for(1U) == nullptr,
          "departed player remains in roster");
    network.join(Network::alice, "Alice");
    check(network.clients.at(Network::alice).local_gid() == 3U, "reconnect reused a retired GID");
    std::cout << "COVERAGE " << static_cast<int>(protocol) << ' ' << covered << '/'
              << fixtures.size() << "\n";
}

void saved_inventory_barrier(ProtocolVersion protocol)
{
    Network network{protocol};
    AuthoritativeBootstrapSnapshot snapshot;
    snapshot.inventory = make_default_player_inventory(protocol, false);
    snapshot.inventory_is_default = true;
    AuthoritativeBootstrapHost authority{protocol, snapshot};
    check(authority.register_peer(Network::alice, 1U), "bootstrap peer registration failed");
    const auto request = [&](std::string_view name, std::span<const std::uint8_t> payload,
                             bool broadcast) {
        const auto packet = frame(protocol, name, payload);
        const auto inbound =
            network.send(Network::alice, packet, MessageDeliveryPolicy::reliable_ordered,
                         broadcast ? std::nullopt : std::optional<std::uint8_t>{std::uint8_t{0}});
        return authority.handle_packet(inbound.peer, inbound.payload, inbound.channel, true);
    };
    check(request("RequestWorldInfoMessage", {}, false).handled(), "world bootstrap failed");
    PayloadError error{};
    const auto presence = encode_player_exists_payload({true, {}, "Alice", {1U, 2U}}, error);
    check(presence && request("PlayerExistsMessage", *presence, true).handled(),
          "presence bootstrap failed");
    check(request("RequestInventoryMessage", {}, true).handled(),
          "initial inventory request failed");
    auto inventory = snapshot.inventory;
    inventory.storage[0] = InventoryItemPayload{2, 7, 1.0F};
    InventorySaveBarrier barrier{protocol, 1U, inventory};
    const auto packets = barrier.packets();
    check(packets.has_value(), "final save barrier packets missing");
    check(!barrier.accepts(1U, (*packets)[0]), "local send was mistaken for a persisted save");
    const auto stored =
        network.send(Network::alice, (*packets)[0], MessageDeliveryPolicy::reliable_ordered);
    DecodedPacket decoded;
    PacketError packet_error{};
    InventoryStorePayload update;
    check(decode_packet(protocol, stored.payload, decoded, packet_error) &&
              decode_inventory_store_payload(decoded.payload, protocol, update, error) &&
              update.final_save,
          "final-save bit or inventory was lost through the original route");
    check(authority.store_peer_inventory(stored.peer, update.inventory),
          "host inventory store failed");
    const auto reply = request("RequestInventoryMessage", {}, true);
    check(reply.handled() && reply.replies.size() == 1U, "read-after-store request rejected");
    const auto &answer = reply.replies[0].application_packet;
    check(barrier.accepts(0U, answer) && !barrier.accepts(2U, answer),
          "save barrier accepted the wrong authority");
    InventorySaveBarrier stale{protocol, 1U, snapshot.inventory};
    check(!stale.accepts(0U, answer), "save barrier accepted a different inventory");
    InventorySaveBarrier wrong_player{protocol, 2U, inventory};
    check(!wrong_player.accepts(0U, answer), "save barrier accepted another player's inventory");
    LegacySteamOutbound response;
    response.recipient = Network::alice;
    response.application_packet = answer;
    network.reply(response, 1U, 0U);
}

WorldSaveData join_world(Network &network, AuthoritativeBootstrapHost &host, PeerId joining,
                         bool interleave_live_crate)
{
    AuthoritativeBootstrapClientOptions options;
    options.protocol = network.protocol;
    options.local_gid = *network.host.gid_for(joining);
    options.gamer_tag = "Bob";
    options.player_id = {2U, 7U};
    AuthoritativeBootstrapClient client{options};
    BootstrapClientError error{};
    const auto first = client.begin(error);
    check(first.has_value(), "client bootstrap could not begin");
    std::deque<AuthoritativeBootstrapClientSend> pending{*first};
    BootstrapGameplayBuffer live;
    bool injected{};
    std::size_t steps{};
    std::size_t inventory_requests{};
    std::vector<std::uint8_t> local_presence;
    while (!pending.empty())
    {
        check(++steps < 4096U, "bootstrap did not make bounded progress");
        const auto request = std::move(pending.front());
        pending.pop_front();
        const auto inbound = network.send(joining, request.application_packet, request.delivery,
                                          request.recipient_gid);
        DecodedPacket sent;
        PacketError sent_error{};
        check(decode_packet(network.protocol, request.application_packet, sent, sent_error) &&
                  sent.message != nullptr,
              "bootstrap request could not be decoded");
        if (sent.message->name == "RequestInventoryMessage")
            ++inventory_requests;
        if (!request.recipient_gid.has_value() && sent.message->name == "PlayerExistsMessage")
        {
            // App::send_online_bootstrap_packets echoes this broadcast locally
            // before any host reply arrives. The native host also returns it.
            local_presence = request.application_packet;
            const auto local_echo = client.handle_packet(options.local_gid, local_presence);
            check(local_echo.handled() && local_echo.outbound.size() == 1U,
                  "the immediate local presence echo must request inventory once");
            for (auto send : local_echo.outbound)
                pending.push_back(std::move(send));
        }
        const auto result = host.handle_packet(joining, inbound.payload, inbound.channel);
        check(result.handled(), "host bootstrap rejected a valid routed request");
        for (const auto &reply : result.replies)
        {
            LegacySteamOutbound outgoing;
            outgoing.recipient = reply.peer;
            outgoing.application_packet = reply.application_packet;
            outgoing.delivery = reply.delivery;
            if (reply.original_sender != 0U)
            {
                outgoing.client_relay = true;
                outgoing.channel = 1U;
                outgoing.sender_gid = *network.host.gid_for(reply.original_sender);
                outgoing.recipient_gid = *network.host.gid_for(reply.peer);
            }
            std::string send_error;
            const auto sends = network.host.create_outbound(outgoing, send_error);
            check(sends.has_value(), send_error);
            for (const auto &wire : *sends)
            {
                const auto received = network.deliver(wire);
                check(!received.audit && received.inbound.size() == 1U,
                      "bootstrap reply failed actual client transport");
                if (wire.remote_peer != joining)
                    continue;
                const auto &packet = received.inbound.front();
                const auto accepted = client.handle_packet(packet.gid, packet.payload);
                if (accepted.error == BootstrapClientError::unexpected_message)
                {
                    check(live.push(packet.gid, packet.payload),
                          "deferred gameplay queue overflowed");
                    continue;
                }
                check(accepted.handled(), "client bootstrap rejected host transcript");
                if (!local_presence.empty() && !client.complete())
                {
                    const auto duplicate = client.handle_packet(options.local_gid, local_presence);
                    check(duplicate.handled() && duplicate.outbound.empty(),
                          "a delayed identical presence echo must not reset bootstrap");
                }
                for (auto send : accepted.outbound)
                    pending.push_back(std::move(send));
                if (interleave_live_crate && !injected &&
                    accepted.input_message == "InventoryRetrieveFromServerMessage")
                {
                    // Another player changes a crate while this client's world
                    // chunks are still loading. Previously this failed the join.
                    PayloadError payload_error{};
                    const auto payload = encode_item_crate_payload(
                        {{8, 3, -8}, 2, InventoryItemPayload{2, 11, 0.75F}}, network.protocol,
                        payload_error);
                    check(payload.has_value(), "live crate fixture invalid");
                    const auto changed = frame(network.protocol, "ItemCrateMessage", *payload);
                    const auto mutation = network.send(Network::alice, changed,
                                                       MessageDeliveryPolicy::reliable_ordered);
                    check(
                        assess_client_gameplay_authority(network.protocol, mutation).should_relay(),
                        "live crate source rejected");
                    const auto classified = client.handle_packet(1U, changed);
                    check(classified.error == BootstrapClientError::unexpected_message &&
                              !client.failed(),
                          "live crate edit was mistaken for malformed initial crate replay");
                    check(live.push(1U, changed), "live crate was discarded during join");
                    injected = true;
                }
            }
        }
    }
    check(client.complete() && client.world_save(), "world bootstrap never became complete");
    auto save = *client.world_save();
    for (const auto &packet : live.take())
    {
        DecodedPacket decoded;
        PacketError packet_error{};
        PayloadError payload_error{};
        check(decode_packet(network.protocol, packet.packet, decoded, packet_error),
              "queued packet changed bytes");
        if (decoded.message->name == "ItemCrateMessage")
        {
            ItemCratePayload crate;
            check(
                decode_item_crate_payload(decoded.payload, network.protocol, crate, payload_error),
                "queued crate changed payload");
            check(inventory_slot_to_stack(
                      crate.item, network.protocol,
                      save.crates[{crate.location.x, crate.location.y, crate.location.z}]
                                 [static_cast<std::size_t>(crate.index)]),
                  "queued crate could not install its item state");
        }
    }
    check(inventory_requests == 1U,
          "the host's duplicate presence echo must not request inventory twice");
    if (interleave_live_crate)
        check(injected && save.crates.at({8, 3, -8})[2]->count == 11 &&
                  save.crates.at({8, 3, -8})[2]->durability == 0.75F,
              "live crate state was lost at world installation");
    return save;
}

void world_progress(ProtocolVersion protocol)
{
    Network network{protocol};
    WorldSaveData save;
    save.seed = 2019;
    save.inventory.set_profile(protocol == ProtocolVersion::modern_v4 ? InventoryProfile::modern
                                                                      : InventoryProfile::classic);
    save.crates[{8, 3, -8}][2] = InventoryStack{InventoryItemId::bullets, 31};
    save.edits = {{8, 3, -8, 2}, {17, -32, 15, 3}};
    BootstrapWorldIdentity identity;
    identity.owner_gamer_tag = "Host";
    identity.creator_gamer_tag = "Host";
    AuthoritativeBootstrapSnapshot snapshot;
    BootstrapSnapshotError error{};
    check(make_authoritative_bootstrap_snapshot(save, host_options(protocol).settings, identity,
                                                true, snapshot, error),
          "progress snapshot could not be built");
    AuthoritativeBootstrapHost authority{protocol, snapshot};
    check(authority.register_peer(Network::alice, 1U) && authority.register_peer(Network::bob, 2U),
          "progress peers not registered");
    if (protocol == ProtocolVersion::modern_v4)
    {
        check(save.inventory.register_spawn_point({4, 2, -8}) &&
                  save.inventory.register_teleport_station({8, 2, -8}, "Home"), "travel setup failed");
        PlayerInventoryPayload personal;
        check(player_inventory_to_payload(save.inventory, protocol, personal), "travel payload failed");
        personal.teleport_stations.push_back(std::nullopt); // Original sparse station lists are legal.
        check(authority.store_peer_inventory(Network::bob, personal), "travel checkpoint rejected");
    }
    const auto restored = join_world(network, authority, Network::bob, true);
    check(restored.seed == 2019 && restored.edits.size() == 2U,
          "late join lost seed or edits in separate chunks");
    if (protocol == ProtocolVersion::modern_v4)
    {
        check(restored.inventory.spawn_point() && restored.inventory.teleport_stations().size() == 1 &&
                  restored.inventory.teleport_stations()[0].waypoint_name == "Home",
              "late join lost personal travel destinations or rejected an empty station entry");
        save.inventory.remove_travel_destination({8, 2, -8});
        check(save.inventory.register_teleport_station({800, -52, 320}, "Hell"), "station replacement failed");
        PlayerInventoryPayload personal;
        check(player_inventory_to_payload(save.inventory, protocol, personal) &&
                  authority.store_peer_inventory(Network::bob, personal) &&
                  authority.update_hell_boss_progress(5, 50), "travel or Hell progress checkpoint failed");
    }
    // Apply changes through the production authoritative mutation API and then
    // use the real leave/admission/bootstrap path to verify the next snapshot.
    check(authority.apply_block_edit({8, 3, -8}, 0U) &&
              authority.apply_block_edit({-17, 4, 31}, 4U),
          "world edits rejected");
    check(authority.apply_crate_item({{8, 3, -8}, 2, InventoryItemPayload{2, 19, 0.5F}}),
          "crate progress update rejected");
    check(authority.apply_crate_item({{9, 3, -8}, 0, InventoryItemPayload{2, 63, 1.0F}}) &&
              authority.destroy_crate({9, 3, -8}),
          "crate removal rejected");
    if (protocol == ProtocolVersion::modern_v4)
        check(authority.set_custom_door({12, 2, -12}, 2U) &&
                  authority.update_spawner({14, 2, -12}, true),
              "Modern custom door/spawner progress rejected");
    for (const auto &drop : network.host.remove_peer(Network::bob))
        check(!network.deliver(drop).audit, "progress peer departure failed");
    authority.disconnect(Network::bob);
    network.join(Network::bob, "Bob");
    check(authority.register_peer(Network::bob, *network.host.gid_for(Network::bob)),
          "progress reconnect failed");
    const auto rejoined = join_world(network, authority, Network::bob, false);
    check(rejoined.edits.size() == 3U && rejoined.crates.at({8, 3, -8})[2]->count == 19 &&
              rejoined.crates.at({8, 3, -8})[2]->durability == 0.5F &&
              !rejoined.crates.contains({9, 3, -8}),
          "reconnect lost edits/crates or revived destroyed crate");
    if (protocol == ProtocolVersion::modern_v4)
        check(rejoined.doors.at({12, 2, -12}) == 2U && rejoined.spawners.contains({14, 2, -12}) &&
                  rejoined.inventory.spawn_point() && rejoined.inventory.teleport_stations().size() == 1 &&
                  rejoined.inventory.teleport_stations()[0].waypoint_name == "Hell" &&
                  rejoined.encounters.hell_bosses_spawned == 5 && rejoined.encounters.max_hell_boss_spawns == 50,
              "Modern reconnect lost custom objects");
}

void loading_and_crate_guards(ProtocolVersion protocol)
{
    const auto fixtures = test::multiplayer_fixtures(protocol);
    BootstrapGameplayBuffer loading;
    const auto state = frame(protocol, "PlayerUpdateMessage", fixtures.at("PlayerUpdateMessage"));
    const auto edit = frame(protocol, "AlterBlockMessage", fixtures.at("AlterBlockMessage"));
    check(loading.push(1U, state) && loading.push(1U, edit), "loading queue rejected valid state");
    loading.forget_player(protocol, 1U);
    const auto kept = loading.take();
    check(kept.size() == 1U && kept[0].packet == edit,
          "departing player removed a committed world mutation");
    for (std::size_t i = 0; i < 4096U; ++i)
        check(loading.push(1U, state), "bounded loading queue filled early");
    check(!loading.push(1U, state), "loading queue exceeded packet bound");
    loading.clear();
    check(loading.push(2U, state) && loading.take().size() == 1U, "loading queue failed to reset");
    CrateFocusTracker focus;
    focus.set(1U, {{8, 3, -8}, 2, 1});
    check(focus.locked({8, 3, -8}, 10) && !focus.locked({8, 3, -8}, 9) &&
              !focus.locked({8, 3, -8}, 10, std::uint8_t{1}),
          "crate lock has wrong peer or slot");
    focus.set(1U, {});
    check(!focus.locked({8, 3, -8}, 10), "closing the crate left a stale lock");
    focus.set(2U, {{8, 3, -8}, 7, 3});
    focus.forget(2U);
    check(!focus.locked({8, 3, -8}, 31), "disconnect left a stale crate lock");
    // The slot a focus names is `X + Y * columns` of the READER's grid (read
    // from both shipped exes): 1.6.3.8's CrateScreen and Crate.IsSlotLocked
    // count in eights; 1.9.9.8's CrateScreen counts in fours (a 4 x 8 grid, so
    // Y reaches 7) while its Crate.IsSlotLocked, which decides what a
    // destroyed crate ejects, still counts in eights. Rows 4..7 of a 1.9.9.8
    // focus used to be thrown away as "out of range".
    focus.set(3U, {{8, 3, -8}, 1, 6}); // a 1.9.9.8 player on the 4 x 8 grid's slot 25
    check(focus.locked({8, 3, -8}, 25, std::nullopt, 4), "1.9.9.8 crate screen ignores a focus below row 3");
    check(!focus.locked({8, 3, -8}, 25), "1.9.9.8 Crate.IsSlotLocked read a focus in fours");
    check(focus.locked({8, 3, -8}, 49) && !focus.locked({8, 3, -8}, 49, std::nullopt, 4),
          "Crate.IsSlotLocked does not read a focus as X + Y * 8");
    focus.set(3U, {{0, 0, 0}, 5, 4}); // the selector left the crate grid
    check(!focus.locked({8, 3, -8}, 25, std::nullopt, 4) && !focus.locked({0, 0, 0}, 37),
          "a focus outside the crate grid still locks a slot");
}

// ---------------------------------------------------------------- conservation
//
// The invariant every scenario below asserts: across any sequence of messages,
// the total count of each item over every player inventory, every crate slot,
// every live world pickup and every persisted record changes only by the exact
// amounts gameplay authorises. Durability, loaded rounds and tray selection
// travel with the item and must survive each hop unchanged.

using ItemTotals = std::map<std::int16_t, std::int64_t>;

void tally(ItemTotals &totals, const InventorySlotPayload &slot)
{
    if (slot.has_value())
        totals[slot->item_id] += slot->stack_count;
}

void tally(ItemTotals &totals, const PlayerInventoryPayload &inventory)
{
    for (const auto &slot : inventory.storage)
        tally(totals, slot);
    for (const auto &tray : inventory.trays)
        for (const auto &slot : tray)
            tally(totals, slot);
}

void tally(ItemTotals &totals, const std::array<InventorySlotPayload, 32U> &crate)
{
    for (const auto &slot : crate)
        tally(totals, slot);
}

// Delivery sequence for a frame stream on an impaired link. ItemCrateMessage is
// ReliableInOrder in both original generations, so a lost frame blocks its
// successors and is retransmitted in place: loss and jitter are invisible at the
// application layer, but a retransmission the sender never saw acknowledged can
// be delivered twice. The sequence therefore keeps order and repeats frames.
std::vector<std::size_t> impaired_order(std::size_t count, std::uint32_t seed)
{
    std::vector<std::size_t> order;
    std::uint32_t state = seed | 1U;
    const auto next = [&] {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    for (std::size_t index{}; index < count; ++index)
    {
        order.push_back(index);
        for (std::uint32_t repeat = next() % 3U; repeat > 0U; --repeat)
            order.push_back(index);
    }
    return order;
}

void item_conservation(ProtocolVersion protocol)
{
    const bool modern = protocol == ProtocolVersion::modern_v4;
    // ---- pure inventory accounting: nothing is created or destroyed ---------
    PlayerInventory inventory;
    inventory.set_profile(modern ? InventoryProfile::modern : InventoryProfile::classic);
    InventoryStack pistol{InventoryItemId::pistol, 1U};
    pistol.rounds_in_clip = 3U;
    InventoryStack pick{InventoryItemId::iron_pickaxe, 1U};
    pick.durability = 0.4F;
    check(inventory.add_stack(pistol) == 0U && inventory.add_stack(pick) == 0U &&
              inventory.add_item(InventoryItemId::iron, 40U) == 0U &&
              inventory.add_item(InventoryItemId::stick, 10U) == 0U,
          "seed inventory did not fit");
    const auto total_iron = inventory.count(InventoryItemId::iron);
    const auto total_stick = inventory.count(InventoryItemId::stick);
    // Moving a stack between tray and backpack conserves every item.
    for (std::size_t index{}; index < PlayerInventory::storage_size + PlayerInventory::tray_size;
         ++index)
        static_cast<void>(inventory.move_to_other_section(index));
    check(inventory.count(InventoryItemId::iron) == total_iron &&
              inventory.count(InventoryItemId::stick) == total_stick,
          "moving stacks between tray and backpack changed item totals");
    // A spent clip and a worn tool keep their per-instance state through a move.
    bool found_clip{}, found_wear{};
    for (std::size_t index{}; index < PlayerInventory::storage_size + PlayerInventory::tray_size;
         ++index)
    {
        const auto &slot = inventory.slot(index);
        if (slot && slot->item == InventoryItemId::pistol)
            found_clip = slot->rounds_in_clip == 3U;
        if (slot && slot->item == InventoryItemId::iron_pickaxe)
            found_wear = slot->durability == 0.4F;
    }
    check(found_clip && found_wear, "a move refilled a spent clip or repaired a worn tool");
    // AddInventoryItem's contract: the part that did not fit is reported so the
    // caller can throw it back into the world instead of destroying it.
    PlayerInventory full;
    full.set_profile(modern ? InventoryProfile::modern : InventoryProfile::classic);
    const std::uint16_t limit = item_stack_limit(InventoryItemId::dirt_block, full.profile());
    for (std::size_t index{}; index < PlayerInventory::storage_size; ++index)
        full.storage()[index] = InventoryStack{InventoryItemId::rock_block, limit};
    for (std::size_t tray{}; tray < (modern ? PlayerInventory::tray_count : 1U); ++tray)
        for (auto &slot : full.tray(tray))
            slot = InventoryStack{InventoryItemId::rock_block, limit};
    check(full.add_item(InventoryItemId::dirt_block, 17U) == 17U,
          "a full inventory silently swallowed a pickup instead of reporting the remainder");
    check(!inventory_checkpoint(full, PlayerInventory::Slot{InventoryStack{
                                          InventoryItemId::dirt_block, 5U}})
               .has_value(),
          "the final-save checkpoint folded a cursor stack that cannot fit");
    const auto folded = inventory_checkpoint(
        inventory, PlayerInventory::Slot{InventoryStack{InventoryItemId::iron, 7U}});
    check(folded.has_value() && folded->count(InventoryItemId::iron) == total_iron + 7U,
          "the final-save checkpoint lost the cursor stack");

    // ---- a full pickup table defers instead of destroying ------------------
    // PickupManager._createQueue, wired exactly as WorldPreviewRenderer wires it:
    // a spawn past the 1000-entity bound is deferred, and every removal releases
    // what was waiting behind it. A refused spawn is an item-destroying event,
    // because the stack has already left the inventory, crate or block.
    using PickupQueue = world::PickupOverflowQueue<InventoryStack>;
    PickupQueue pending;
    std::vector<InventoryStack> live;
    std::int64_t spawned{}, created{};
    const auto release = [&] {
        pending.release(live.size(), [&](InventoryStack &request) {
            live.push_back(request);
            ++created;
            return std::optional<std::size_t>{live.size()};
        });
    };
    const auto spawn = [&](const InventoryStack &stack) {
        spawned += stack.count;
        if (!pending.releasing() && PickupQueue::at_capacity(live.size()))
        {
            check(pending.defer(stack), "the deferral queue refused a spawn below its hard bound");
            return;
        }
        live.push_back(stack);
        ++created;
    };
    for (std::size_t index{}; index < PickupQueue::live_limit; ++index)
        spawn(InventoryStack{InventoryItemId::dirt_block, 1U});
    check(live.size() == PickupQueue::live_limit && pending.empty(),
          "the pickup table did not fill to the original bound");
    // A Hardcore DropAll plus two full crates ejected by one blast, all while the
    // table is full: not one stack may be lost.
    for (std::size_t index{}; index < 40U; ++index)
        spawn(InventoryStack{InventoryItemId::iron, 7U});
    for (std::size_t index{}; index < 64U; ++index)
        spawn(InventoryStack{InventoryItemId::bullets, 3U});
    check(pending.size() == 104U && live.size() == PickupQueue::live_limit,
          "spawns past the world bound were destroyed instead of deferred");
    // Each entity that leaves the world releases exactly one waiting request,
    // and the table never exceeds the original bound while doing so.
    std::size_t drained{};
    while (!pending.empty())
    {
        check(++drained <= 104U, "releasing deferred pickups did not make progress");
        live.erase(live.begin());
        release();
        check(live.size() == PickupQueue::live_limit,
              "a release left the pickup table over or under the original bound");
    }
    check(drained == 104U && created == static_cast<std::int64_t>(PickupQueue::live_limit) + 104,
          "deferred pickups were not released one per removal");
    std::int64_t recovered{};
    for (const auto &stack : live)
        if (stack.item != InventoryItemId::dirt_block)
            recovered += stack.count;
    check(recovered == 40 * 7 + 64 * 3,
          "a deferred death drop or crate ejection lost items on release");
    check(spawned == static_cast<std::int64_t>(PickupQueue::live_limit) + 40 * 7 + 64 * 3,
          "the spawn fixture miscounted");
    // Re-entrant releases cannot defer a released request back into the queue.
    pending.release(0U, [&](InventoryStack &) { return std::optional<std::size_t>{0U}; });
    check(pending.empty() && !pending.releasing(), "the deferral queue left itself suppressed");

    // ---- crate traffic conserves across an impaired reliable channel --------
    Network network{protocol};
    const IntVector3 crate_location{8, 3, -8};
    std::array<InventorySlotPayload, 32U> sender_crate{};
    std::array<InventorySlotPayload, 32U> receiver_crate{};
    PlayerInventoryPayload sender_inventory;
    check(player_inventory_to_payload(inventory, protocol, sender_inventory),
          "seed inventory is not representable on the wire");
    // Enough occupied backpack slots that one quick transfer is a real burst,
    // including a gun with a part-spent clip and a worn tool.
    for (std::size_t index{}; index < sender_inventory.storage.size(); ++index)
        sender_inventory.storage[index] =
            index % 4U == 0U
                ? InventoryItemPayload{51, 1, 1.0F, static_cast<std::int32_t>(index % 9U)}
            : index % 4U == 1U ? InventoryItemPayload{25, 1, 0.25F}
            : index % 4U == 2U
                ? InventoryItemPayload{20, static_cast<std::int16_t>(index % 16U + 2U), 1.0F}
                : InventorySlotPayload{};
    ItemTotals before;
    tally(before, sender_inventory);
    tally(before, sender_crate);
    // A whole-tray transfer: every occupied slot is rewritten in one frame,
    // exactly as CrateScreen's quick transfer does.
    std::vector<std::vector<std::uint8_t>> stream;
    PayloadError error{};
    for (std::size_t index{}; index < sender_inventory.storage.size(); ++index)
    {
        if (!sender_inventory.storage[index].has_value())
            continue;
        sender_crate[index] = sender_inventory.storage[index];
        sender_inventory.storage[index].reset();
        const auto payload = encode_item_crate_payload(
            {crate_location, static_cast<std::int32_t>(index), sender_crate[index]}, protocol,
            error);
        check(payload.has_value(), "crate transfer payload rejected");
        stream.push_back(frame(protocol, "ItemCrateMessage", *payload));
    }
    check(stream.size() >= 20U, "crate transfer produced too little traffic to be a burst");
    // Split the largest stack back out of the crate: the half that stays and the
    // half that leaves are two absolute slot assignments, and together they must
    // conserve the stack exactly.
    const std::size_t split_source = 2U;
    const std::size_t split_target = 3U;
    check(sender_crate[split_source].has_value() && !sender_crate[split_target].has_value(),
          "split fixture picked the wrong slots");
    const auto split_total = sender_crate[split_source]->stack_count;
    const auto kept = static_cast<std::int16_t>(split_total / 2);
    InventoryItemPayload moved = *sender_crate[split_source];
    moved.stack_count = static_cast<std::int16_t>(split_total - kept);
    sender_crate[split_source]->stack_count = kept;
    sender_crate[split_target] = moved;
    for (const std::size_t index : {split_source, split_target})
    {
        const auto payload = encode_item_crate_payload(
            {crate_location, static_cast<std::int32_t>(index), sender_crate[index]}, protocol,
            error);
        check(payload.has_value(), "crate split payload rejected");
        stream.push_back(frame(protocol, "ItemCrateMessage", *payload));
    }
    ItemTotals after;
    tally(after, sender_inventory);
    tally(after, sender_crate);
    check(after == before, "moving and splitting stacks in a crate changed item totals");
    // The host must accept every frame of one legitimate transfer, relay it and
    // apply it; a clipped frame leaves the crate permanently disagreeing.
    ClientGameplayRateLimiter limiter;
    for (const std::size_t index : impaired_order(stream.size(), 0x5EEDU))
    {
        const auto inbound =
            network.send(Network::alice, stream[index], MessageDeliveryPolicy::reliable_ordered);
        const auto decision = assess_client_gameplay_authority(protocol, inbound);
        check(decision.should_relay(), "a legitimate crate transfer was refused by authority");
        check(limiter.allow(Network::alice, decision.message, 1'000U),
              "a legitimate whole-tray crate transfer was clipped by the rate limiter");
        DecodedPacket decoded;
        PacketError packet_error{};
        ItemCratePayload crate;
        check(decode_packet(protocol, inbound.payload, decoded, packet_error) &&
                  decode_item_crate_payload(decoded.payload, protocol, crate, error) &&
                  crate.index >= 0 && crate.index < 32,
              "relayed crate frame changed on the wire");
        receiver_crate[static_cast<std::size_t>(crate.index)] = crate.item;
    }
    check(receiver_crate == sender_crate,
          "duplicated crate frames left the peers disagreeing or duplicated items");
    // Why the ordered delivery policy is load-bearing. ItemCrateMessage is an
    // absolute slot assignment, so two writes to one slot only compose in order:
    // take half the stack out and then the rest, and reversed delivery leaves the
    // first half behind forever. ItemCrateMessage must never become unordered.
    const auto remaining = sender_crate[split_source];
    std::array<InventorySlotPayload, 2U> same_slot{remaining, InventorySlotPayload{}};
    std::vector<std::vector<std::uint8_t>> ordered_pair;
    for (const auto &value : same_slot)
    {
        const auto payload = encode_item_crate_payload(
            {crate_location, static_cast<std::int32_t>(split_source), value}, protocol, error);
        check(payload.has_value(), "same-slot crate payload rejected");
        ordered_pair.push_back(frame(protocol, "ItemCrateMessage", *payload));
    }
    const auto apply_pair = [&](bool reversed) {
        auto slot = receiver_crate[split_source];
        for (std::size_t step{}; step < ordered_pair.size(); ++step)
        {
            const auto &packet = ordered_pair[reversed ? ordered_pair.size() - 1U - step : step];
            DecodedPacket decoded;
            PacketError packet_error{};
            ItemCratePayload crate;
            check(decode_packet(protocol, packet, decoded, packet_error) &&
                      decode_item_crate_payload(decoded.payload, protocol, crate, error),
                  "same-slot frame changed on the wire");
            slot = crate.item;
        }
        return slot;
    };
    check(!apply_pair(false).has_value() && apply_pair(true) == remaining,
          "ItemCrateMessage stopped being an ordered absolute slot assignment");

    // ---- a legitimate item burst is never clipped ---------------------------
    // Worst legitimate second: a Hardcore DropAll (40 stacks), two full crates
    // ejected by one blast (64), mined drops, four whole-tray crate transfers,
    // a second of player updates and a second of voice.
    ClientGameplayRateLimiter burst;
    std::size_t allowed{};
    for (std::size_t index{}; index < 114U; ++index)
        allowed += burst.allow(Network::alice, "CreatePickupMessage", 2'000U) ? 1U : 0U;
    for (std::size_t index{}; index < 128U; ++index)
        allowed += burst.allow(Network::alice, "ItemCrateMessage", 2'000U) ? 1U : 0U;
    for (std::size_t index{}; index < 4U; ++index)
        allowed += burst.allow(Network::alice, "DestroyCrateMessage", 2'000U) ? 1U : 0U;
    for (std::size_t index{}; index < 20U; ++index)
        allowed += burst.allow(Network::alice, "PlayerUpdateMessage", 2'000U) ? 1U : 0U;
    for (std::size_t index{}; index < 100U; ++index)
        allowed += burst.allow(Network::alice, "VoiceChatMessage", 2'000U) ? 1U : 0U;
    check(allowed == 366U, "a legitimate drop/eject/transfer burst lost item traffic");
    // The buckets still exist: a flood of either item message is refused.
    ClientGameplayRateLimiter flood;
    std::size_t pickups{}, crates{};
    for (std::size_t index{}; index < 20'000U; ++index)
    {
        pickups += flood.allow(Network::bob, "CreatePickupMessage", 3'000U) ? 1U : 0U;
        crates += flood.allow(Network::bob, "ItemCrateMessage", 3'000U) ? 1U : 0U;
    }
    // The pickup ceiling sits above one blast through a wall of crates (124
    // crates of 32 stacks, as the original executes it), not above real play.
    check(pickups <= 4'096U && crates <= 192U,
          "the item message flood guard was removed with the clipping fix");

    // ---- the host's refusal answer is a stock, unspoofable route ------------
    // A refused CreatePickupMessage is answered with a directed
    // ConsumePickupMessage naming the sender as the picker-upper, which stock
    // PickupManager turns back into AddInventoryItem. It is host-only, so a
    // client can never mint one for itself.
    const auto refund_payload = encode_consume_pickup_payload(
        {{8.5F, 4.0F, -8.5F}, 7, 1, 1, InventoryItemPayload{20, 40, 1.0F}, false}, protocol, error);
    check(refund_payload.has_value(), "pickup refund payload rejected");
    const auto refund = frame(protocol, "ConsumePickupMessage", *refund_payload);
    const auto spoofed =
        network.send(Network::alice, refund, MessageDeliveryPolicy::reliable_unordered);
    check(assess_client_gameplay_authority(protocol, spoofed).error ==
              ClientGameplayAuthorityError::host_only_message,
          "a client could mint its own pickup refund");
    LegacySteamOutbound directed;
    directed.recipient = Network::alice;
    directed.application_packet = refund;
    directed.delivery = MessageDeliveryPolicy::reliable_unordered;
    network.reply(directed, 1U, 0U);

    // ---- persisted guest records survive a new GID and a host restart -------
    const auto directory = std::filesystem::temp_directory_path() /
                           ("openminerz-pinv-" + std::to_string(static_cast<int>(protocol)));
    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
    std::filesystem::create_directories(directory);
    PlayerInventoryPayload stored = sender_inventory;
    stored.trays.front()[0] = InventoryItemPayload{51, 1, 1.0F, 3};  // part-spent pistol
    stored.trays.front()[1] = InventoryItemPayload{25, 1, 0.4F};     // worn iron pick
    stored.storage[0] = InventoryItemPayload{20, 40, 1.0F};
    check(write_peer_inventory(directory, protocol, Network::alice, stored),
          "guest inventory record could not be written");
    std::optional<PlayerInventoryPayload> reread;
    check(read_peer_inventory(directory, protocol, Network::alice, reread) && reread == stored,
          "guest inventory record lost items, durability or loaded rounds");
    ItemTotals persisted;
    tally(persisted, *reread);
    ItemTotals expected;
    tally(expected, stored);
    check(persisted == expected, "guest record round trip changed item totals");
    {
        // Host restart: a fresh authority over the same directory, and the guest
        // reconnects under a different GID.
        AuthoritativeBootstrapSnapshot snapshot;
        snapshot.inventory = make_default_player_inventory(protocol, false);
        snapshot.inventory_is_default = true;
        AuthoritativeBootstrapHost restarted{protocol, snapshot, directory};
        check(restarted.register_peer(Network::alice, 4U),
              "a reconnecting guest was refused by the restarted host");
        const auto reply = encode_inventory_retrieve_payload({*reread, 4U, false}, protocol, error);
        check(reply.has_value(), "retrieve payload rejected");
        InventorySaveBarrier barrier{protocol, 4U, *reread};
        const auto confirmation = frame(protocol, "InventoryRetrieveFromServerMessage", *reply);
        check(barrier.accepts(0U, confirmation),
              "the final-save barrier rejected the host's own readback after a restart");
        InventorySaveBarrier defaulted{protocol, 4U, snapshot.inventory};
        check(!defaulted.accepts(0U, confirmation),
              "the barrier accepted the default loadout as a persisted save");
    }
    std::filesystem::remove_all(directory, remove_error);
}

ProvideChunkPayload full_chunk(ProtocolVersion protocol)
{
    ProvideChunkPayload chunk;
    chunk.block_location = {0, -64, 0};
    for (int x = 0; x < 16; ++x)
        for (int y = -64; y < 64; ++y)
            for (int z = 0; z < 16; ++z)
            {
                std::int32_t entry{};
                check(encode_delta_entry({x, y, z}, 2U, protocol, entry),
                      "full chunk delta entry rejected");
                chunk.delta.push_back(entry);
            }
    return chunk;
}

void full_size_packets(ProtocolVersion protocol)
{
    Network network{protocol};
    const auto chunk = full_chunk(protocol);
    PayloadError error{};
    const auto payload = encode_provide_chunk_payload(chunk, error);
    check(payload && payload->size() > 128U * 1024U,
          "full chunk fixture did not exceed old IPC budget");
    LegacySteamOutbound outgoing;
    outgoing.application_packet = frame(protocol, "ProvideChunkMessage", *payload);
    outgoing.delivery = MessageDeliveryPolicy::reliable_unordered;
    network.reply(outgoing, 2U, 0U);
    std::string send_error;
    const auto sends = network.host.create_outbound(outgoing, send_error);
    check(sends && !sends->empty(), "full chunk native framing failed");
    SteamBridgeProtocolError bridge_error{};
    const auto &wire = sends->front();
    const auto bridge_payload = encode_steam_bridge_outbound_wire(
        {wire.remote_peer, wire.delivery, wire.p2p_channel, wire.payload}, bridge_error);
    check(bridge_payload.has_value(), "full chunk exceeded x86 helper envelope");
    const auto bridge_frame = encode_steam_bridge_frame(SteamBridgeMessageType::outbound_wire, 7U,
                                                        *bridge_payload, bridge_error);
    SteamBridgeFrame decoded_frame;
    SteamBridgeOutboundWire decoded_wire;
    check(
        bridge_frame && decode_steam_bridge_frame(*bridge_frame, decoded_frame, bridge_error) &&
            decode_steam_bridge_outbound_wire(decoded_frame.payload, decoded_wire, bridge_error) &&
            decoded_wire.wire == wire.payload,
        "full chunk changed across x64/x86 IPC");
    const std::vector<std::uint8_t> voice(voice_packet_samples, 0xD5U);
    const auto audio = encode_voice_chat_payload(voice, error);
    check(audio && audio->size() > 4096U,
          "PC voice fixture must exceed the original allocation size");
    const auto packet = frame(protocol, "VoiceChatMessage", *audio);
    const auto inbound = network.send(Network::alice, packet, MessageDeliveryPolicy::chat);
    check(assess_client_gameplay_authority(protocol, inbound).should_relay(),
          "full PC voice route rejected");
    outgoing.application_packet = packet;
    outgoing.delivery = MessageDeliveryPolicy::chat;
    outgoing.channel = 1U;
    outgoing.sender_gid = 1U;
    outgoing.client_relay = true;
    network.reply(outgoing, 1U, 1U);
}

// The Xbox Avatar add-on's extra message (openminerz/net/avatar_sync.hpp),
// driven through the production session classes. A client that does not know
// that message id is disconnected by its own reader, so the property under test
// is absolute: no path through either session class puts one on the wire for a
// peer that has not announced the add-on - and a stock client never announces.
void avatar_addon(ProtocolVersion protocol)
{
    const std::uint8_t natural = natural_avatar_message_id(protocol);
    // Where the add-on lands beside another mod that registers two message
    // types of its own: ids are positional, so nothing fixes them.
    const auto addon = static_cast<std::uint8_t>(natural + 2U);
    const AvatarSyncMessage hello;
    const auto packet_for = [&](std::uint8_t id) { return *encode_avatar_sync_packet(id, hello); };
    const auto presence_with = [&](std::optional<std::uint8_t> id, bool marker = true) {
        PlayerExistsPayload presence;
        presence.avatar_description.assign(10U, 0U);
        if (marker)
            presence.avatar_description =
                append_avatar_capability_marker(presence.avatar_description, id);
        presence.gamer_tag = "Avatar";
        presence.player_id = {1U, 2U};
        PayloadError payload_error{};
        const auto payload = encode_player_exists_payload(presence, payload_error);
        check(payload.has_value(), "presence fixture");
        return frame(protocol, "PlayerExistsMessage", *payload);
    };
    // Wires a client could put on the Steam link without going through OUR
    // client class: what an add-on client, or a hostile one, sends.
    const auto raw_proxy = [](std::uint8_t recipient, std::uint8_t sender,
                              const std::vector<std::uint8_t> &packet,
                              SteamDeliveryMethod delivery = SteamDeliveryMethod::reliable_ordered) {
        LegacySteamCodecError error{};
        const auto route = encode_legacy_client_proxy_direct(recipient, sender, delivery, packet, error);
        check(route.has_value(), "raw proxy route");
        return *encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, *route, error);
    };
    const auto raw_broadcast = [](std::uint8_t sender, const std::vector<std::uint8_t> &packet) {
        LegacySteamCodecError error{};
        const auto route = encode_legacy_client_broadcast(
            sender, SteamDeliveryMethod::reliable_ordered, packet, error);
        check(route.has_value(), "raw broadcast route");
        return *encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, *route, error);
    };
    const auto raw_to_host = [](std::uint8_t sender, const std::vector<std::uint8_t> &packet) {
        LegacySteamCodecError error{};
        const auto direct = encode_legacy_direct_frame({0U, sender, packet}, error);
        check(direct.has_value(), "raw direct frame");
        return *encode_legacy_steam_wire(SteamIncomingMessageType::data, 0U, *direct, error);
    };
    const auto dropped = [](const LegacySteamHostSessionResult &result, std::string_view reason) {
        return result.sends.empty() && result.inbound.empty() && result.audit.has_value() &&
               result.audit->reason == reason;
    };
    std::string error;

    // ---- 1. A session of stock clients: nobody has announced anything.
    {
        Network network{protocol};
        auto &alice = network.clients.at(Network::alice);
        const std::uint8_t alice_gid = *network.host.gid_for(Network::alice);
        const std::uint8_t bob_gid = *network.host.gid_for(Network::bob);
        for (const std::uint8_t id : {natural, addon})
        {
            for (const std::uint8_t gid : {std::uint8_t{0}, alice_gid, bob_gid, std::uint8_t{77}})
            {
                check(!network.host.create_avatar_outbound(gid, packet_for(id), error).has_value(),
                      "the host produced an avatar packet for a peer that never announced");
                check(!alice.create_avatar_outbound(gid, packet_for(id), error).has_value(),
                      "a client produced an avatar packet for a peer that never announced");
            }
            // The stock paths refuse the id outright, for every route.
            LegacySteamOutbound outgoing;
            outgoing.application_packet = packet_for(id);
            outgoing.delivery = MessageDeliveryPolicy::reliable_ordered;
            check(!network.host.create_outbound(outgoing, error).has_value(),
                  "the stock host broadcast path accepted an unknown id");
            outgoing.recipient = Network::bob;
            check(!network.host.create_outbound(outgoing, error).has_value(),
                  "the stock host direct path accepted an unknown id");
            outgoing.recipient.reset();
            outgoing.client_relay = true;
            outgoing.channel = 1U;
            outgoing.sender_gid = alice_gid;
            check(!network.host.create_outbound(outgoing, error).has_value(),
                  "the stock relay path accepted an unknown id");
            for (const auto target : {std::optional<std::uint8_t>{}, std::optional<std::uint8_t>{bob_gid},
                                      std::optional<std::uint8_t>{std::uint8_t{0}}})
                check(!alice
                           .create_outbound(target, packet_for(id),
                                            MessageDeliveryPolicy::reliable_ordered, error)
                           .has_value(),
                      "the stock client path accepted an unknown id");
            // Whatever a peer puts on the wire itself is dropped, never relayed.
            check(dropped(network.host.process_wire_packet(Network::alice,
                                                           raw_proxy(bob_gid, alice_gid, packet_for(id))),
                          "AvatarPacketFromUnannouncedPeer"),
                  "an unannounced peer's avatar packet was relayed");
            check(dropped(network.host.process_wire_packet(Network::alice,
                                                           raw_to_host(alice_gid, packet_for(id))),
                          "AvatarPacketFromUnannouncedPeer"),
                  "an unannounced peer's avatar packet reached the host");
            check(dropped(network.host.process_wire_packet(Network::alice,
                                                           raw_broadcast(alice_gid, packet_for(id))),
                          "AvatarPacketNotDirect"),
                  "an avatar packet was broadcast");
        }
        // A stock presence announces nothing, however often it is repeated.
        const auto inbound =
            network.send(Network::alice, presence_with(std::nullopt, false),
                         MessageDeliveryPolicy::reliable_ordered);
        check(inbound.kind == LegacySteamInboundKind::gameplay && !network.host.avatar_capable(alice_gid),
              "a stock presence was taken for an announcement");
    }

    // ---- 2. One add-on player among stock players.
    {
        Network network{protocol};
        auto &alice = network.clients.at(Network::alice);
        auto &bob = network.clients.at(Network::bob);
        const std::uint8_t alice_gid = *network.host.gid_for(Network::alice);
        const std::uint8_t bob_gid = *network.host.gid_for(Network::bob);
        // Alice announces: a duplicate stock PlayerExists whose avatar
        // description ends in the marker. It is an ordinary, valid, relayable
        // PlayerExists to everything that does not look for the marker.
        const auto announcement = presence_with(addon);
        const auto wire = alice.create_outbound(std::nullopt, announcement,
                                                MessageDeliveryPolicy::reliable_ordered, error);
        check(wire.has_value(), error);
        const auto received = network.host.process_wire_packet(Network::alice, wire->payload);
        check(!received.audit && received.sends.empty() && received.inbound.size() == 2U &&
                  received.inbound[0].kind == LegacySteamInboundKind::avatar_capability &&
                  received.inbound[0].sender_gid == alice_gid &&
                  received.inbound[0].payload == std::vector<std::uint8_t>{addon} &&
                  received.inbound[1].kind == LegacySteamInboundKind::gameplay &&
                  received.inbound[1].payload == announcement,
              "the marker was not read off the stock presence");
        check(assess_client_gameplay_authority(protocol, received.inbound[1]).should_relay(),
              "a marked presence must relay exactly like a stock one");
        check(network.host.avatar_capable(alice_gid) && network.host.avatar_message_id(alice_gid) == addon &&
                  !network.host.avatar_capable(bob_gid) && !network.host.avatar_capable(0U),
              "capability is per gamer");
        // The relay reaches Bob byte for byte; a stock Bob reads a duplicate
        // presence and ignores it.
        LegacySteamOutbound relay;
        relay.application_packet = announcement;
        relay.delivery = MessageDeliveryPolicy::reliable_ordered;
        relay.channel = 1U;
        relay.sender_gid = alice_gid;
        relay.client_relay = true;
        const auto relayed = network.host.create_outbound(relay, error);
        check(relayed.has_value() && relayed->size() == 1U && relayed->front().remote_peer == Network::bob,
              "the marked presence was not relayed to the other player");
        const auto at_bob = network.deliver(relayed->front());
        check(!at_bob.audit && at_bob.inbound.size() == 2U &&
                  at_bob.inbound[0].kind == LegacySteamClientInboundKind::avatar_capability &&
                  at_bob.inbound[0].gid == alice_gid &&
                  at_bob.inbound[1].kind == LegacySteamClientInboundKind::gameplay &&
                  at_bob.inbound[1].payload == announcement,
              "a relayed marked presence changed on the way");

        // Bob has announced nothing, so nothing may reach him - not from the
        // host, not relayed from Alice (who IS capable), not under any id.
        for (const std::uint8_t id : {natural, addon})
        {
            check(!network.host.create_avatar_outbound(bob_gid, packet_for(id), error).has_value(),
                  "the host sent an avatar packet to a stock player");
            check(!alice.create_avatar_outbound(bob_gid, packet_for(id), error).has_value(),
                  "an OpenMinerZ client addressed an avatar packet to a stock player");
            check(dropped(network.host.process_wire_packet(Network::alice,
                                                           raw_proxy(bob_gid, alice_gid, packet_for(id))),
                          "AvatarRecipientNotCapable"),
                  "THE GATE: the host relayed an avatar packet to a stock player");
        }
        // Alice can be spoken to, under her id and no other...
        check(!network.host.create_avatar_outbound(alice_gid, packet_for(natural), error).has_value(),
              "an avatar packet left under an id its recipient does not use");
        auto garbage = packet_for(addon);
        garbage.pop_back();
        check(!network.host.create_avatar_outbound(alice_gid, garbage, error).has_value(),
              "something that is not an avatar message left under the avatar id");
        const auto to_alice = network.host.create_avatar_outbound(alice_gid, packet_for(addon), error);
        check(to_alice.has_value() && to_alice->size() == 1U &&
                  to_alice->front().remote_peer == Network::alice &&
                  to_alice->front().delivery == SteamDeliveryMethod::reliable_ordered,
              "the host could not address the announced player");
        // ...and she can speak to the host, which hands the packet to the
        // avatar sync and to nothing else.
        const auto at_host =
            network.host.process_wire_packet(Network::alice, raw_to_host(alice_gid, packet_for(addon)));
        check(!at_host.audit && at_host.sends.empty() && at_host.inbound.size() == 1U &&
                  at_host.inbound[0].kind == LegacySteamInboundKind::avatar_sync &&
                  at_host.inbound[0].sender_gid == alice_gid && at_host.inbound[0].payload == packet_for(addon),
              "an announced player's avatar packet did not reach the host");
        check(dropped(network.host.process_wire_packet(Network::alice,
                                                       raw_to_host(alice_gid, packet_for(natural))),
                      "AvatarPacketIdMismatch"),
              "an avatar packet under somebody else's id was accepted");
        check(dropped(network.host.process_wire_packet(
                          Network::alice, raw_proxy(bob_gid, alice_gid, packet_for(addon),
                                                    SteamDeliveryMethod::unreliable)),
                      "AvatarPacketDelivery"),
              "an unreliable avatar packet was accepted");
        // The host has not announced to Alice, so OUR client class refuses the
        // packet above when it arrives: the gate holds in both directions.
        check(network.deliver(to_alice->front()).audit.has_value(),
              "a client accepted an avatar packet from a peer that never announced");

        // Bob announces under the same id: now the two can exchange, relayed.
        const auto bob_wire = bob.create_outbound(std::nullopt, announcement,
                                                  MessageDeliveryPolicy::reliable_ordered, error);
        check(bob_wire.has_value() &&
                  network.host.process_wire_packet(Network::bob, bob_wire->payload).inbound.size() == 2U &&
                  network.host.avatar_capable(bob_gid),
              "the second announcement was missed");
        const auto relayed_packet =
            network.host.process_wire_packet(Network::alice, raw_proxy(bob_gid, alice_gid, packet_for(addon)));
        check(!relayed_packet.audit && relayed_packet.inbound.empty() && relayed_packet.sends.size() == 1U &&
                  relayed_packet.sends.front().remote_peer == Network::bob,
              "two announced players could not exchange through the host");
        const auto arrived = network.deliver(relayed_packet.sends.front());
        check(!arrived.audit && arrived.inbound.size() == 1U &&
                  arrived.inbound[0].kind == LegacySteamClientInboundKind::avatar_sync &&
                  arrived.inbound[0].gid == alice_gid && arrived.inbound[0].payload == packet_for(addon),
              "a relayed avatar packet did not arrive intact");
        // A player cannot be sent their own packet back, nor one for a ghost.
        check(dropped(network.host.process_wire_packet(Network::alice,
                                                       raw_proxy(alice_gid, alice_gid, packet_for(addon))),
                      "InvalidProxyTarget") &&
                  dropped(network.host.process_wire_packet(Network::alice,
                                                           raw_proxy(99U, alice_gid, packet_for(addon))),
                          "InvalidProxyTarget"),
              "an avatar packet was routed to an invalid target");

        // Bob leaves: the capability goes with him, at the host and at Alice.
        for (const auto &drop : network.host.remove_peer(Network::bob))
            static_cast<void>(network.deliver(drop));
        network.clients.erase(Network::bob);
        check(!network.host.avatar_capable(bob_gid) &&
                  !network.host.create_avatar_outbound(bob_gid, packet_for(addon), error).has_value() &&
                  dropped(network.host.process_wire_packet(Network::alice,
                                                           raw_proxy(bob_gid, alice_gid, packet_for(addon))),
                          "InvalidProxyTarget"),
              "a departed player kept his capability");
        // Gamer ids are reused. Every id is held by an announced player in
        // turn; whoever takes it next starts as a stock player.
        constexpr PeerId carol = 903U;
        std::set<std::uint8_t> seen;
        bool reused{};
        for (int round = 0; round < 300; ++round)
        {
            network.join(carol, "Carol");
            const std::uint8_t gid = *network.host.gid_for(carol);
            reused = reused || !seen.insert(gid).second;
            check(!network.host.avatar_capable(gid) && !alice.avatar_capable(gid) &&
                      !network.host.create_avatar_outbound(gid, packet_for(addon), error).has_value() &&
                      !alice.create_avatar_outbound(gid, packet_for(addon), error).has_value(),
                  "a newcomer inherited the previous occupant's capability");
            const auto carol_wire = network.clients.at(carol).create_outbound(
                std::nullopt, announcement, MessageDeliveryPolicy::reliable_ordered, error);
            check(carol_wire.has_value(), error);
            static_cast<void>(network.host.process_wire_packet(carol, carol_wire->payload));
            relay.sender_gid = gid;
            const auto carol_relay = network.host.create_outbound(relay, error);
            check(carol_relay.has_value(), error);
            for (const auto &send : *carol_relay)
                static_cast<void>(network.deliver(send));
            check(network.host.avatar_capable(gid) && alice.avatar_capable(gid),
                  "an announcement after a rejoin was missed");
            for (const auto &drop : network.host.remove_peer(carol))
                static_cast<void>(network.deliver(drop));
            network.clients.erase(carol);
        }
        check(reused, "the gamer-id reuse case was never reached");
    }

    // ---- 3. Different ids in one session.
    {
        Network network{protocol};
        const std::uint8_t alice_gid = *network.host.gid_for(Network::alice);
        const std::uint8_t bob_gid = *network.host.gid_for(Network::bob);
        const auto announce = [&](PeerId peer, std::optional<std::uint8_t> id) {
            const auto wire = network.clients.at(peer).create_outbound(
                std::nullopt, presence_with(id), MessageDeliveryPolicy::reliable_ordered, error);
            check(wire.has_value(), error);
            static_cast<void>(network.host.process_wire_packet(peer, wire->payload));
        };
        announce(Network::alice, addon);   // an add-on client beside another mod
        announce(Network::bob, std::nullopt); // an older add-on build: the natural id
        check(network.host.avatar_message_id(alice_gid) == addon &&
                  network.host.avatar_message_id(bob_gid) == natural,
              "ids were not kept per gamer");
        // Alice's packets carry her id; Bob's client would index its message
        // table with it and throw. A stock host relays that; this one does not.
        check(dropped(network.host.process_wire_packet(Network::alice,
                                                       raw_proxy(bob_gid, alice_gid, packet_for(addon))),
                      "AvatarPacketIdMismatch"),
              "a packet was relayed to a player who reads another id");
        // A sender that speaks the RECIPIENT's id (OpenMinerZ does) is relayed.
        const auto spoken = network.host.process_wire_packet(
            Network::alice, raw_proxy(bob_gid, alice_gid, packet_for(natural)));
        check(!spoken.audit && spoken.sends.size() == 1U, "a packet under the recipient's id was refused");
        // An id inside the stock table never opens anything.
        announce(Network::bob, std::uint8_t{3});
        check(network.host.avatar_message_id(bob_gid) == natural,
              "a marker naming a stock id changed a player's capability");
        // An OpenMinerZ peer (one that completed the private extension hello)
        // reads the message under its sender's id, so Alice's reaches it.
        constexpr PeerId carol = 903U;
        check(network.host.authorize_extension(carol), "extension hello");
        network.join(carol, "Carol");
        const std::uint8_t carol_gid = *network.host.gid_for(carol);
        announce(carol, std::nullopt);
        const auto to_carol = network.host.process_wire_packet(
            Network::alice, raw_proxy(carol_gid, alice_gid, packet_for(addon)));
        check(!to_carol.audit && to_carol.sends.size() == 1U &&
                  to_carol.sends.front().remote_peer == carol,
              "an OpenMinerZ peer was denied a packet it can read");
        // The host addresses each player under that player's own id.
        check(network.host.create_avatar_outbound(alice_gid, packet_for(addon), error).has_value() &&
                  network.host.create_avatar_outbound(bob_gid, packet_for(natural), error).has_value() &&
                  !network.host.create_avatar_outbound(bob_gid, packet_for(addon), error).has_value(),
              "the host used the wrong id for a player");
    }
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 3 && std::string_view{argv[1]} == "--write-fixtures")
        {
            for (const auto protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
            {
                const auto directory =
                    std::filesystem::path{argv[2]} / std::to_string(static_cast<int>(protocol));
                std::filesystem::create_directories(directory);
                for (const auto &[name, payload] : test::multiplayer_fixtures(protocol))
                {
                    std::ofstream output{directory / (name + ".bin"), std::ios::binary};
                    output.write(reinterpret_cast<const char *>(payload.data()),
                                 static_cast<std::streamsize>(payload.size()));
                    check(static_cast<bool>(output), "fixture export failed");
                }
                PayloadError error{};
                const auto payload = encode_provide_chunk_payload(full_chunk(protocol), error);
                check(payload.has_value(), "full chunk fixture export failed");
                std::ofstream output{directory / "ProvideChunkMessage.full.bin", std::ios::binary};
                output.write(reinterpret_cast<const char *>(payload->data()),
                             static_cast<std::streamsize>(payload->size()));
                check(static_cast<bool>(output), "full chunk fixture export failed");
                std::ofstream companding{directory / "voice.alaw", std::ios::binary};
                for (int sample = -32768; sample <= 32767; ++sample)
                    companding.put(
                        static_cast<char>(encode_voice_sample(static_cast<std::int16_t>(sample))));
                for (int code = 0; code < 256; ++code)
                {
                    const auto sample = static_cast<std::uint16_t>(
                        decode_voice_sample(static_cast<std::uint8_t>(code)));
                    companding.put(static_cast<char>(sample & 255U));
                    companding.put(static_cast<char>(sample >> 8U));
                }
                check(static_cast<bool>(companding), "voice codebook fixture export failed");
            }
            return 0;
        }
        for (const auto protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
        {
            all_packets(protocol);
            saved_inventory_barrier(protocol);
            world_progress(protocol);
            loading_and_crate_guards(protocol);
            item_conservation(protocol);
            full_size_packets(protocol);
            avatar_addon(protocol);
        }
        std::cout << "PASS: all 108 profile/message combinations, authority, routes, corruption, "
                     "reconnect and final saves\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
