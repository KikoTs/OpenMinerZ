// Host-side join regressions. Every case drives the production host through
// LegacySteamRuntimeHost (session, bootstrap authority and join diagnostics) with
// the x86 helper replaced by an in-memory transport, for both stock protocols.
//
// The joiner is scripted from the unmodified clients' own order of operations
// (FrontEndScreen.JoinCallback -> GetWorldInfo -> BeginLoadTerrain ->
// ChunkCache.BECOMECLIENT -> StartGame), not from the OpenMinerZ client, because
// a host that only accepts the OpenMinerZ order strands every stock joiner.

#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/host_ban_list.hpp"
#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/legacy_steam_runtime_host.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
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

void expect(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error{std::string{message}};
}

constexpr PeerId host_peer = 900U;
constexpr PeerId alice = 101U;
constexpr PeerId bob = 202U;
constexpr PeerId carol = 303U;

// Gamer.PlayerID of a stock client with no connected Ethernet adapter:
// DNAGame.GetLocalID() hashes an empty MAC string, so it is MD5("").
const std::vector<std::uint8_t> wifi_player_id{0xD4U, 0x1DU, 0x8CU, 0xD9U, 0x8FU, 0x00U,
                                               0xB2U, 0x04U, 0xE9U, 0x80U, 0x09U, 0x98U,
                                               0xECU, 0xF8U, 0x42U, 0x7EU};

std::vector<std::uint8_t> frame(ProtocolVersion protocol, std::string_view name,
                                std::span<const std::uint8_t> payload = {})
{
    PacketError error{};
    const MessageDescriptor *descriptor = message_by_name(protocol, name);
    expect(descriptor != nullptr, "the message must exist in the stock registry");
    auto packet = encode_packet(protocol, descriptor->id, payload, error);
    expect(packet.has_value(), "the message must accept stock framing");
    return std::move(*packet);
}

// One datagram the host handed to the transport, decoded as its recipient would.
struct Delivered final
{
    PeerId peer{};
    std::uint8_t p2p_channel{};
    SteamDeliveryMethod delivery{SteamDeliveryMethod::reliable_ordered};
    std::vector<std::uint8_t> raw;
    // Denial on native channel one.
    std::optional<std::string> denial;
    // Channel-zero wire envelope contents.
    bool status_connected{};
    std::optional<LegacyConnectedResponse> roster;
    std::optional<std::pair<std::uint8_t, LegacyGamerIdentity>> new_peer;
    std::optional<std::uint8_t> dropped_gid;
    std::string message;
    std::uint8_t sender_gid{};
    std::uint8_t recipient_gid{};
    std::vector<std::uint8_t> payload;
};

Delivered decode_delivery(ProtocolVersion protocol, const SteamBridgeOutboundWire &wire)
{
    Delivered delivered;
    delivered.peer = wire.peer;
    delivered.p2p_channel = wire.p2p_channel;
    delivered.delivery = wire.delivery;
    delivered.raw = wire.wire;
    if (wire.p2p_channel == 1U)
    {
        // SteamWorks.Deny: a zero byte, the reason, and its C terminator.
        expect(wire.wire.size() >= 3U && wire.wire.front() == 0U && wire.wire.back() == 0U,
               "a denial must be the stock zero-prefixed, NUL-terminated reason");
        delivered.denial = std::string{wire.wire.begin() + 1, wire.wire.end() - 1};
        return delivered;
    }
    if (is_extension_packet(wire.wire))
    {
        delivered.message = "OpenMinerZExtension";
        return delivered;
    }
    LegacySteamWirePacket envelope;
    LegacySteamCodecError codec_error{};
    expect(decode_legacy_steam_wire(wire.wire, envelope, codec_error),
           "every channel-zero datagram must be a stock wire envelope");
    if (envelope.message_type == SteamIncomingMessageType::status_changed)
    {
        expect(envelope.channel == 0U && envelope.payload == std::vector<std::uint8_t>({5, 0, 0, 0}),
               "the only status the host sends is the Int32 'connected' value");
        delivered.status_connected = true;
        return delivered;
    }
    expect(envelope.message_type == SteamIncomingMessageType::data,
           "host traffic is either status or data");
    if (envelope.channel == 1U)
    {
        LegacyConnectedResponse roster;
        std::uint8_t gid{};
        LegacyGamerIdentity gamer;
        if (decode_legacy_connected_response(envelope.payload, roster, codec_error))
            delivered.roster = std::move(roster);
        else if (decode_legacy_new_peer(envelope.payload, gid, gamer, codec_error))
            delivered.new_peer = std::pair{gid, std::move(gamer)};
        else if (decode_legacy_drop_peer(envelope.payload, gid, codec_error))
            delivered.dropped_gid = gid;
        else
            expect(false, "an unknown internal session message left the host");
        return delivered;
    }
    LegacyDirectDataFrame direct;
    expect(decode_legacy_direct_frame(envelope.payload, direct, codec_error),
           "channel-zero data must be a recipient/sender direct frame");
    DecodedPacket decoded;
    PacketError packet_error{};
    expect(decode_packet(protocol, direct.application_packet, decoded, packet_error) &&
               decoded.message != nullptr,
           "host application data must keep stock framing");
    delivered.message = std::string{decoded.message->name};
    delivered.sender_gid = direct.sender_gid;
    delivered.recipient_gid = direct.recipient_gid;
    delivered.payload.assign(decoded.payload.begin(), decoded.payload.end());
    return delivered;
}

struct Harness final
{
    ProtocolVersion protocol;
    HostSessionSettings settings;
    LegacySteamRuntimeHost host;
    std::vector<SteamBridgeOutboundWire> outbox;
    std::chrono::steady_clock::time_point clock{std::chrono::seconds{1'000}};
    IntVector3 edited_block{17, 1, -1};

    explicit Harness(ProtocolVersion selected,
                     const std::function<void(HostSessionSettings &)> &configure = {})
        : protocol{selected}
    {
        settings.server_name = "Join Test";
        settings.protocol = protocol;
        settings.maximum_players = 8;
        settings.visibility = HostVisibility::public_lobby;
        settings.mod_manifest.protocol = protocol;
        if (configure)
            configure(settings);

        WorldSaveData save;
        save.seed = 2026;
        save.inventory.set_profile(protocol == ProtocolVersion::modern_v4
                                       ? InventoryProfile::modern
                                       : InventoryProfile::classic);
        save.edits = {{edited_block.x, edited_block.y, edited_block.z, 3}};
        BootstrapWorldIdentity identity;
        identity.name = "Join World";
        AuthoritativeBootstrapSnapshot snapshot;
        BootstrapSnapshotError snapshot_error{};
        expect(make_authoritative_bootstrap_snapshot(save, settings, identity, true, snapshot,
                                                     snapshot_error),
               "the hosted world snapshot must build");

        LegacySteamRuntimeHostOptions options;
        options.settings = settings;
        options.game_mode = 1;
        options.difficulty = 1;
        options.bootstrap_snapshot = std::move(snapshot);
        LegacySteamDetachedTransport transport;
        transport.send = [this](const SteamBridgeOutboundWire &wire, std::string &error) {
            outbox.push_back(wire);
            error.clear();
            return true;
        };
        transport.now = [this] { return clock; };
        std::string error;
        expect(host.start_detached(options, SteamBridgeHostReady{host_peer, 7'000'000U, "Host"},
                                   std::move(transport), error),
               error);
        expect(host.running(), "a detached host must report that it is running");
    }

    Harness(const Harness &) = delete;
    Harness &operator=(const Harness &) = delete;

    // What a stock client sends back: the eight slots it read out of the lobby
    // metadata with atoi(), so the two unpublished slots arrive as zero, not null.
    [[nodiscard]] SessionProperties stock_properties() const
    {
        SessionProperties properties = make_session_properties(settings, false, 1, 1, false);
        properties[6] = 0;
        properties[7] = 0;
        return properties;
    }

    void advance(std::chrono::seconds seconds)
    {
        clock += seconds;
    }

    void enter_lobby(PeerId peer, std::string persona)
    {
        SteamBridgeClientEvent event;
        event.kind = SteamBridgeClientEventKind::peer_entered;
        event.peer = peer;
        event.peer_name = std::move(persona);
        host.inject(std::move(event));
    }

    void leave_lobby(PeerId peer)
    {
        SteamBridgeClientEvent event;
        event.kind = SteamBridgeClientEventKind::peer_left;
        event.peer = peer;
        host.inject(std::move(event));
    }

    void receive(PeerId peer, std::vector<std::uint8_t> wire, std::uint8_t p2p_channel = 0U)
    {
        SteamBridgeClientEvent event;
        event.kind = SteamBridgeClientEventKind::inbound_wire;
        event.inbound = SteamBridgeInboundWire{peer, p2p_channel, std::move(wire)};
        host.inject(std::move(event));
    }

    // RequestConnectToHostMessage inside the ConnectionApproval envelope.
    void request_join(PeerId peer, std::string tag, std::vector<std::uint8_t> player_id,
                      std::optional<std::string> password = std::nullopt,
                      std::optional<std::int32_t> version = std::nullopt)
    {
        LegacyConnectionRequest request;
        request.network_version = version.value_or(static_cast<std::int32_t>(protocol));
        request.session_id = 1234;
        request.password = std::move(password);
        request.gamer = LegacyGamerIdentity{std::move(player_id), std::move(tag)};
        request.session_properties = stock_properties();
        LegacySteamCodecError error{};
        const auto payload = encode_legacy_connection_request(request, error);
        expect(payload.has_value(), "the join request must encode");
        const auto wire = encode_legacy_steam_wire(SteamIncomingMessageType::connection_approval,
                                                   0U, *payload, error);
        expect(wire.has_value(), "the join request envelope must encode");
        receive(peer, *wire);
    }

    // DoSend(sender, host): recipient GID zero, data channel zero.
    void send_direct(PeerId peer, std::uint8_t gid, std::string_view message,
                     std::span<const std::uint8_t> payload = {})
    {
        LegacySteamCodecError error{};
        const auto direct = encode_legacy_direct_frame(
            LegacyDirectDataFrame{0U, gid, frame(protocol, message, payload)}, error);
        expect(direct.has_value(), "the direct frame must encode");
        const auto wire =
            encode_legacy_steam_wire(SteamIncomingMessageType::data, 0U, *direct, error);
        expect(wire.has_value(), "the direct envelope must encode");
        receive(peer, *wire);
    }

    // DoSend(sender): InternalMessageTypes.BroadcastMessage on data channel one.
    void send_broadcast(PeerId peer, std::uint8_t gid, std::string_view message,
                        std::span<const std::uint8_t> payload = {})
    {
        LegacySteamCodecError error{};
        const auto route = encode_legacy_client_broadcast(
            gid, SteamDeliveryMethod::reliable_ordered, frame(protocol, message, payload), error);
        expect(route.has_value(), "the broadcast route must encode");
        const auto wire =
            encode_legacy_steam_wire(SteamIncomingMessageType::data, 1U, *route, error);
        expect(wire.has_value(), "the broadcast envelope must encode");
        receive(peer, *wire);
    }

    struct Step final
    {
        std::vector<LegacySteamRuntimeEvent> events;
        std::vector<Delivered> sent;

        [[nodiscard]] std::vector<Delivered> to(PeerId peer) const
        {
            std::vector<Delivered> result;
            for (const Delivered &delivered : sent)
                if (delivered.peer == peer)
                    result.push_back(delivered);
            return result;
        }
        [[nodiscard]] const Delivered *find(PeerId peer, std::string_view message) const
        {
            for (const Delivered &delivered : sent)
                if (delivered.peer == peer && delivered.message == message)
                    return &delivered;
            return nullptr;
        }
        [[nodiscard]] std::size_t count(LegacySteamRuntimeEventKind kind) const
        {
            std::size_t total{};
            for (const LegacySteamRuntimeEvent &event : events)
                total += event.kind == kind ? 1U : 0U;
            return total;
        }
        [[nodiscard]] const LegacySteamJoinNotice *notice(LegacySteamJoinNoticeKind kind) const
        {
            for (const LegacySteamRuntimeEvent &event : events)
                if (event.kind == LegacySteamRuntimeEventKind::join_notice &&
                    event.notice.kind == kind)
                    return &event.notice;
            return nullptr;
        }
    };

    // Pumps exactly as App::pump_online_host does: until a pump yields nothing.
    [[nodiscard]] Step pump()
    {
        Step step;
        for (std::size_t batch{}; batch < 256U; ++batch)
        {
            std::vector<LegacySteamRuntimeEvent> events;
            std::string error;
            expect(host.pump(events, error), "the host stopped: " + error);
            if (events.empty())
                break;
            for (LegacySteamRuntimeEvent &event : events)
                step.events.push_back(std::move(event));
        }
        for (const SteamBridgeOutboundWire &wire : outbox)
            step.sent.push_back(decode_delivery(protocol, wire));
        outbox.clear();
        return step;
    }

    // Admits one stock client and returns its GID.
    [[nodiscard]] std::uint8_t admit(PeerId peer, std::string tag,
                                     std::vector<std::uint8_t> player_id = wifi_player_id)
    {
        request_join(peer, std::move(tag), std::move(player_id));
        const Step step = pump();
        expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U,
               "the join request must be admitted");
        const std::optional<std::uint8_t> gid = host.gid_for(peer);
        expect(gid.has_value() && *gid != 0U, "an admitted peer must hold a non-zero GID");
        return *gid;
    }
};

// The complete stock join, in the stock client's own order. Before the fix the
// host answered neither ClientReadyForChunks nor RequestChunk until it had seen
// PlayerExists, which a stock client only sends after those answers arrive.
void stock_client_join_order_reaches_gameplay(ProtocolVersion protocol)
{
    Harness harness{protocol};
    harness.enter_lobby(alice, "Alice");
    Harness::Step step = harness.pump();
    const LegacySteamJoinNotice *entered =
        step.notice(LegacySteamJoinNoticeKind::entered_lobby);
    expect(entered != nullptr && entered->peer == alice && entered->name == "Alice" &&
               step.sent.empty(),
           "the host must be told who entered the lobby before any packet arrives");

    harness.request_join(alice, "Alice", wifi_player_id);
    step = harness.pump();
    const std::vector<Delivered> admission = step.to(alice);
    expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U &&
               step.count(LegacySteamRuntimeEventKind::rejected) == 0U &&
               admission.size() == 2U && admission[0].status_connected &&
               admission[0].p2p_channel == 0U &&
               admission[0].delivery == SteamDeliveryMethod::reliable_ordered &&
               admission[1].roster.has_value() && admission[1].roster->player_gid == 1U &&
               admission[1].roster->peer_gids == std::vector<std::uint8_t>({0}) &&
               admission[1].roster->peers[0].gamer_tag == "Host",
           "a stock request with all eight property slots present must be admitted with status "
           "5 followed by the host-only roster on native channel zero");
    const std::uint8_t gid = *harness.host.gid_for(alice);

    // FrontEndScreen.JoinCallback -> CastleMinerZGame.GetWorldInfo.
    harness.send_direct(alice, gid, "RequestWorldInfoMessage");
    step = harness.pump();
    const Delivered *world = step.find(alice, "WorldInfoMessage");
    WorldInfoPayload world_info;
    PayloadError payload_error{};
    expect(world != nullptr && world->sender_gid == 0U && world->recipient_gid == gid &&
               decode_world_info_payload(world->payload, protocol, world_info, payload_error) &&
               world_info.seed == 2026,
           "the host must answer RequestWorldInfo with its world");

    // BlockTerrain.Init -> ChunkCache.MakeHost(world, false) -> BECOMECLIENT ->
    // BroadcastThatWereReady(): sent while loading, long before PlayerExists.
    harness.send_direct(alice, gid, "ClientReadyForChunksMessage");
    step = harness.pump();
    const Delivered *delta_list = step.find(alice, "ProvideDeltaListMessage");
    std::vector<std::int32_t> chunk_ids;
    expect(step.count(LegacySteamRuntimeEventKind::rejected) == 0U && delta_list != nullptr &&
               decode_delta_list_payload(delta_list->payload, chunk_ids, payload_error) &&
               chunk_ids == std::vector<std::int32_t>{chunk_id(chunk_corner(harness.edited_block))},
           "ClientReadyForChunks sent before PlayerExists must be answered with the modified "
           "chunk list; refusing it leaves a stock client on its loading screen forever");

    // CachedChunk.GetChunkFromHost for a modified and for an untouched chunk.
    const IntVector3 edited_corner = chunk_corner(harness.edited_block);
    const IntVector3 untouched_corner{160, -64, -320};
    const auto edited_request = encode_request_chunk_payload({edited_corner, 1U}, payload_error);
    const auto untouched_request =
        encode_request_chunk_payload({untouched_corner, 0U}, payload_error);
    expect(edited_request.has_value() && untouched_request.has_value(),
           "chunk requests must encode");
    harness.send_direct(alice, gid, "RequestChunkMessage", *edited_request);
    harness.send_direct(alice, gid, "RequestChunkMessage", *untouched_request);
    step = harness.pump();
    std::vector<ProvideChunkPayload> chunks;
    for (const Delivered &delivered : step.to(alice))
    {
        ProvideChunkPayload chunk;
        expect(delivered.message == "ProvideChunkMessage" &&
                   decode_provide_chunk_payload(delivered.payload, chunk, payload_error),
               "each chunk request must produce one ProvideChunk");
        chunks.push_back(std::move(chunk));
    }
    expect(step.count(LegacySteamRuntimeEventKind::rejected) == 0U && chunks.size() == 2U &&
               chunks[0].block_location == edited_corner && chunks[0].priority == 1U &&
               chunks[0].delta.size() == 1U && chunks[1].block_location == untouched_corner &&
               chunks[1].priority == 0U && chunks[1].delta.empty(),
           "chunk requests made while the terrain is loading must be served before presence");
    expect(!harness.host.gameplay_ready(alice, gid),
           "serving terrain must not open ordinary gameplay before PlayerExists");

    // Terrain loaded -> CastleMinerZGame.StartGame -> PlayerExistsMessage(request).
    const auto presence = encode_player_exists_payload(
        {true, std::vector<std::uint8_t>(10U, 0U), "Alice", wifi_player_id}, payload_error);
    expect(presence.has_value(), "the presence payload must encode");
    harness.send_broadcast(alice, gid, "PlayerExistsMessage", *presence);
    step = harness.pump();
    bool host_presence_seen{};
    for (const Delivered &delivered : step.to(alice))
    {
        if (delivered.message != "PlayerExistsMessage" || delivered.sender_gid != 0U)
            continue;
        PlayerExistsPayload host_presence;
        host_presence_seen =
            decode_player_exists_payload(delivered.payload, host_presence, payload_error) &&
            !host_presence.request_response && host_presence.gamer_tag == "Host" &&
            host_presence.avatar_description.size() == 10U;
    }
    expect(step.count(LegacySteamRuntimeEventKind::rejected) == 0U && host_presence_seen &&
               step.find(alice, "TimeOfDayMessage") != nullptr &&
               harness.host.gameplay_ready(alice, gid),
           "PlayerExists must be answered with the host's presence and time of day and must "
           "open gameplay for this GID");

    // _processPlayerExistsMessage (own echo) -> RequestInventoryMessage.
    harness.send_broadcast(alice, gid, "RequestInventoryMessage");
    step = harness.pump();
    const Delivered *inventory = step.find(alice, "InventoryRetrieveFromServerMessage");
    InventoryRetrievePayload retrieved;
    expect(inventory != nullptr &&
               decode_inventory_retrieve_payload(inventory->payload, protocol, retrieved,
                                                 payload_error) &&
               retrieved.player_id == gid,
           "the inventory request must be answered for the joiner's own GID");

    // A client-side terrain reset (ChunkCache SHUTDOWN, then BECOMECLIENT again)
    // announces readiness a second time; the stock host answers every time.
    harness.send_direct(alice, gid, "ClientReadyForChunksMessage");
    step = harness.pump();
    expect(step.count(LegacySteamRuntimeEventKind::rejected) == 0U &&
               step.find(alice, "ProvideDeltaListMessage") != nullptr,
           "a repeated ClientReadyForChunks must be answered again, not dropped");

    // Nothing in this join is reported as stalled, even much later.
    harness.advance(std::chrono::seconds{600});
    step = harness.pump();
    expect(step.events.empty(), "a completed join must not produce any stall notice");
}

// Chunk traffic stays closed until the peer has been sent WorldInfo.
void terrain_requests_still_require_world_info(ProtocolVersion protocol)
{
    Harness harness{protocol};
    const std::uint8_t gid = harness.admit(alice, "Alice");
    PayloadError payload_error{};
    const auto request = encode_request_chunk_payload({{0, -64, 0}, 0U}, payload_error);
    harness.send_direct(alice, gid, "ClientReadyForChunksMessage");
    harness.send_direct(alice, gid, "RequestChunkMessage", *request);
    const Harness::Step step = harness.pump();
    std::size_t named{};
    for (const LegacySteamRuntimeEvent &event : step.events)
        if (event.kind == LegacySteamRuntimeEventKind::rejected &&
            (event.detail == "InvalidOrOutOfOrderBootstrapMessage:ClientReadyForChunksMessage" ||
             event.detail == "InvalidOrOutOfOrderBootstrapMessage:RequestChunkMessage"))
            ++named;
    expect(step.sent.empty() && named == 2U,
           "terrain requests before WorldInfo must be refused, and the refusal must name the "
           "message so a stalled join can be diagnosed from the host log");
}

// Identity is the Steam account. Neither the shared stock PlayerID nor an equal
// display tag may keep a second player out.
void identity_is_the_steam_account_not_the_player_id_or_name(ProtocolVersion protocol)
{
    Harness harness{protocol};
    expect(harness.admit(alice, "Sam", wifi_player_id) == 1U, "the first Sam must be admitted");

    // A second Wi-Fi-only stock client: same PlayerID blob, same display name.
    harness.request_join(bob, "Sam", wifi_player_id);
    Harness::Step step = harness.pump();
    const std::vector<Delivered> to_bob = step.to(bob);
    const std::vector<Delivered> to_alice = step.to(alice);
    expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U &&
               step.count(LegacySteamRuntimeEventKind::rejected) == 0U &&
               harness.host.gid_for(bob) == std::optional<std::uint8_t>{std::uint8_t{2}},
           "a second player with the same PlayerID and display name must be admitted");
    expect(to_bob.size() == 2U && to_bob[0].status_connected && to_bob[1].roster.has_value() &&
               to_bob[1].roster->peer_gids == std::vector<std::uint8_t>({0, 1}) &&
               to_bob[1].roster->peers[1].gamer_tag == "Sam",
           "the joiner's roster must list the existing player under the name it already has");
    expect(to_alice.size() == 1U && to_alice[0].new_peer.has_value() &&
               to_alice[0].new_peer->first == 2U &&
               to_alice[0].new_peer->second.gamer_tag == "Sam (2)" &&
               to_alice[0].new_peer->second.player_id == wifi_player_id,
           "existing players must be told about the newcomer under a session-unique tag");
    const LegacyGamerIdentity *listed = harness.host.gamer_for_gid(2U);
    const std::string *self_tag = harness.host.self_reported_tag_for_gid(2U);
    expect(listed != nullptr && listed->gamer_tag == "Sam (2)" && self_tag != nullptr &&
               *self_tag == "Sam",
           "the host must keep both the roster tag and the tag the client uses for itself");

    // The same name as the host, and a third collision.
    harness.request_join(carol, "Host", {1U, 2U, 3U});
    step = harness.pump();
    expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U &&
               harness.host.gamer_for_gid(3U) != nullptr &&
               harness.host.gamer_for_gid(3U)->gamer_tag == "Host (2)",
           "a player named like the host must be admitted and listed distinctly");
    harness.request_join(404U, "Sam", {4U});
    step = harness.pump();
    expect(harness.host.gamer_for_gid(4U) != nullptr &&
               harness.host.gamer_for_gid(4U)->gamer_tag == "Sam (3)",
           "each further collision must take the next free suffix");

    // Only byte-identical tags collide; the stock protocol can tell these apart.
    harness.request_join(505U, "sam", {5U});
    step = harness.pump();
    expect(harness.host.gamer_for_gid(5U) != nullptr &&
               harness.host.gamer_for_gid(5U)->gamer_tag == "sam",
           "a tag that differs in case is a different stock name and must not be rewritten");

    // The one true duplicate: the same Steam account asking again while playing.
    harness.request_join(alice, "Someone Else", {9U, 9U, 9U});
    step = harness.pump();
    const std::vector<Delivered> denial = step.to(alice);
    const LegacySteamJoinNotice *refused = step.notice(LegacySteamJoinNoticeKind::refused);
    expect(denial.size() == 1U && denial[0].p2p_channel == 1U &&
               denial[0].denial == std::optional<std::string>{"GamerAlreadyConnected"} &&
               step.count(LegacySteamRuntimeEventKind::peer_connected) == 0U &&
               harness.host.gid_for(alice) == std::optional<std::uint8_t>{std::uint8_t{1}} &&
               refused != nullptr && refused->peer == alice &&
               refused->reason == "GamerAlreadyConnected",
           "a Steam account that is already playing must get the stock denial and keep its "
           "existing session");

    // A de-duplicated tag never exceeds the stock bound or splits a code point.
    Harness long_names{protocol};
    std::string long_tag(123U, 'a');
    long_tag += "\xC3\xA9";
    long_tag += "aaa";
    static_cast<void>(long_names.admit(alice, long_tag));
    static_cast<void>(long_names.admit(bob, long_tag));
    const LegacyGamerIdentity *shortened = long_names.host.gamer_for_gid(2U);
    expect(long_tag.size() == 128U && shortened != nullptr &&
               shortened->gamer_tag == std::string(123U, 'a') + " (2)",
           "a de-duplicated tag must stay within 128 bytes without cutting a UTF-8 sequence");
}

// A refusal has to be visible on the host (who and why) and has to reach the
// joining client in the form both client families read.
void refusals_name_the_player_and_reach_the_client(ProtocolVersion protocol)
{
    Harness harness{protocol, [](HostSessionSettings &settings) { settings.password = "pw"; }};
    harness.enter_lobby(alice, "Mallory");
    static_cast<void>(harness.pump());

    harness.request_join(alice, "Mallory", wifi_player_id, std::string{"wrong"});
    Harness::Step step = harness.pump();
    const std::vector<Delivered> sent = step.to(alice);
    const LegacySteamJoinNotice *refused = step.notice(LegacySteamJoinNoticeKind::refused);
    expect(sent.size() == 1U && sent[0].p2p_channel == 1U &&
               sent[0].delivery == SteamDeliveryMethod::reliable_ordered &&
               sent[0].denial == std::optional<std::string>{"IncorrectPassword"} &&
               !harness.host.gid_for(alice).has_value(),
           "a wrong password must be answered with the stock denial on native channel one");
    expect(refused != nullptr && refused->peer == alice && refused->name == "Mallory" &&
               refused->reason == "IncorrectPassword" &&
               step.count(LegacySteamRuntimeEventKind::rejected) == 1U,
           "the host must receive a notice naming the refused player and the reason");
    const LegacySteamJoinNoticeText text = describe_join_notice(*refused);
    expect(text.title == "JOIN REFUSED" && text.body.find("Mallory") != std::string::npos &&
               text.body.find("password") != std::string::npos &&
               text.log.find("network.host.join refused") == 0U &&
               text.log.find("peer=101") != std::string::npos &&
               text.log.find("name=\"Mallory\"") != std::string::npos &&
               text.log.find("reason=IncorrectPassword") != std::string::npos,
           "the on-screen text and the log line must both say who was refused and why");

    // The OpenMinerZ client reads the same bytes as the reason.
    LegacySteamClientSessionOptions client_options;
    client_options.protocol = protocol;
    client_options.host_peer = host_peer;
    client_options.expected_host_name = "Host";
    client_options.session_properties = make_session_properties(harness.settings, false, 1, 1, false);
    client_options.password = "wrong";
    client_options.local_gamer = LegacyGamerIdentity{{1U, 2U}, "Mallory"};
    LegacySteamClientSession client{client_options};
    std::string client_error;
    expect(client.begin(client_error).has_value(), client_error);
    const LegacySteamClientSessionResult told =
        client.process_native_packet(host_peer, sent[0].p2p_channel, sent[0].raw);
    expect(told.rejection_reason == std::optional<std::string>{"IncorrectPassword"},
           "the production client must decode the host's refusal reason");

    // Wrong game version in each direction, with the stock reason for each.
    const bool modern = protocol == ProtocolVersion::modern_v4;
    harness.request_join(bob, "Bob", {7U}, std::string{"pw"}, modern ? 3 : 4);
    step = harness.pump();
    const LegacySteamJoinNotice *wrong_version = step.notice(LegacySteamJoinNoticeKind::refused);
    const char *expected_reason = modern ? "ServerHasNewerVersion" : "ServerHasOlderVersion";
    expect(step.to(bob).size() == 1U &&
               step.to(bob)[0].denial == std::optional<std::string>{expected_reason} &&
               wrong_version != nullptr && wrong_version->name == "Bob" &&
               wrong_version->reason == expected_reason &&
               describe_join_notice(*wrong_version).body.find(modern ? "Classic" : "Modern") !=
                   std::string::npos,
           "a client on the other protocol must be refused with the stock version reason and "
           "the host must be told which way round the mismatch is");

    // The refused player corrects the password and is admitted.
    harness.request_join(alice, "Mallory", wifi_player_id, std::string{"pw"});
    step = harness.pump();
    expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U &&
               harness.host.gid_for(alice).has_value(),
           "a refusal must not prevent a corrected retry");
}

// An OpenMinerZ client repeats its mod hello on every attempt. A refused first
// attempt must not turn the second hello into a "duplicate" and lock it out.
void a_refused_openminerz_client_can_retry_its_handshake(ProtocolVersion protocol)
{
    Harness harness{protocol, [](HostSessionSettings &settings) { settings.password = "pw"; }};
    ExtensionMessage hello;
    hello.kind = ExtensionMessageKind::client_hello;
    hello.manifest.protocol = protocol;
    const auto handshake = [&](std::uint64_t nonce) {
        hello.nonce = nonce;
        harness.receive(alice, encode_extension_message(hello));
        const Harness::Step step = harness.pump();
        const std::vector<Delivered> sent = step.to(alice);
        expect(sent.size() == 1U && sent[0].message == "OpenMinerZExtension",
               "the mod hello must be answered");
        ExtensionMessage response;
        ExtensionDecodeError decode_error{};
        expect(decode_extension_message(sent[0].raw, response, decode_error) &&
                   response.nonce == nonce,
               "the mod response must echo the nonce");
        return response.kind == ExtensionMessageKind::server_accept &&
               step.notice(LegacySteamJoinNoticeKind::refused) == nullptr;
    };
    expect(handshake(11U), "the first mod hello must be accepted");
    harness.request_join(alice, "Alice", {1U}, std::string{"wrong"});
    Harness::Step step = harness.pump();
    expect(step.to(alice).size() == 1U &&
               step.to(alice)[0].denial == std::optional<std::string>{"IncorrectPassword"},
           "the first attempt must be refused for its password");
    expect(handshake(12U),
           "after a refusal the retry's mod hello must be accepted, not called a duplicate");
    harness.request_join(alice, "Alice", {1U}, std::string{"pw"});
    step = harness.pump();
    expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U,
           "the corrected retry must be admitted");
}

// A lobby member that never became a player is not a player disconnect. The
// application tears down GID zero's enemies, dragon and remote state for a
// disconnect that reports GID zero, and GID zero is the host.
void an_unadmitted_member_leaving_is_reported_as_a_failed_join(ProtocolVersion protocol)
{
    Harness harness{protocol};
    const std::uint8_t alice_gid = harness.admit(alice, "Alice");

    // Entered the lobby; Steam never delivers a join request.
    harness.enter_lobby(bob, "Bob");
    Harness::Step step = harness.pump();
    expect(step.notice(LegacySteamJoinNoticeKind::entered_lobby) != nullptr,
           "entering the lobby must be reported");
    harness.advance(legacy_steam_join_request_overdue - std::chrono::seconds{1});
    expect(harness.pump().events.empty(), "no stall may be reported before the deadline");
    harness.advance(std::chrono::seconds{2});
    step = harness.pump();
    const LegacySteamJoinNotice *overdue = step.notice(LegacySteamJoinNoticeKind::request_overdue);
    expect(overdue != nullptr && overdue->peer == bob && overdue->name == "Bob" &&
               overdue->reason == "NoJoinRequestReceived" && overdue->waited_seconds >= 10U &&
               describe_join_notice(*overdue).log.find("network.host.join stalled peer=202") == 0U,
           "a lobby member whose join request never arrives must be reported once, by name");
    harness.advance(std::chrono::seconds{30});
    expect(harness.pump().events.empty(), "the same stall must not be reported twice");

    harness.leave_lobby(bob);
    step = harness.pump();
    const LegacySteamJoinNotice *abandoned =
        step.notice(LegacySteamJoinNoticeKind::left_before_admission);
    expect(step.count(LegacySteamRuntimeEventKind::peer_disconnected) == 0U &&
               abandoned != nullptr && abandoned->peer == bob && abandoned->name == "Bob" &&
               abandoned->reason == "NoJoinRequestReceived" && step.sent.empty(),
           "a member that never held a GID must not be reported as a disconnected player");

    // Refused, then gone: the notice keeps the reason the player was given.
    Harness guarded{protocol, [](HostSessionSettings &settings) { settings.password = "pw"; }};
    guarded.request_join(carol, "Carol", {3U});
    static_cast<void>(guarded.pump());
    guarded.leave_lobby(carol);
    step = guarded.pump();
    const LegacySteamJoinNotice *refused_then_left =
        step.notice(LegacySteamJoinNoticeKind::left_before_admission);
    expect(step.count(LegacySteamRuntimeEventKind::peer_disconnected) == 0U &&
               refused_then_left != nullptr && refused_then_left->name == "Carol" &&
               refused_then_left->reason == "IncorrectPassword",
           "a refused player leaving must be reported with the reason it was refused");

    // A real player leaving is still a disconnect for exactly its own GID.
    static_cast<void>(harness.admit(carol, "Carol"));
    harness.leave_lobby(alice);
    step = harness.pump();
    const std::vector<Delivered> to_carol = step.to(carol);
    std::optional<std::uint8_t> departed_gid;
    for (const LegacySteamRuntimeEvent &event : step.events)
        if (event.kind == LegacySteamRuntimeEventKind::peer_disconnected)
            departed_gid = event.inbound.sender_gid;
    expect(step.count(LegacySteamRuntimeEventKind::peer_disconnected) == 1U &&
               departed_gid == std::optional<std::uint8_t>{alice_gid} && alice_gid != 0U &&
               step.notice(LegacySteamJoinNoticeKind::left_before_admission) == nullptr &&
               to_carol.size() == 1U && to_carol[0].dropped_gid == departed_gid &&
               !harness.host.gid_for(alice).has_value(),
           "an admitted player leaving must disconnect its own GID and drop it for the others");
}

// An admitted peer that never reaches PlayerExists is reported with the phase
// it is stuck in.
void an_admitted_peer_that_never_finishes_loading_is_reported(ProtocolVersion protocol)
{
    Harness harness{protocol};
    const std::uint8_t gid = harness.admit(alice, "Alice");
    harness.advance(legacy_steam_join_loading_overdue + std::chrono::seconds{1});
    Harness::Step step = harness.pump();
    const LegacySteamJoinNotice *silent = step.notice(LegacySteamJoinNoticeKind::loading_overdue);
    expect(silent != nullptr && silent->peer == alice && silent->name == "Alice" &&
               silent->reason == "NoWorldInfoRequest" &&
               describe_join_notice(*silent).title == "PLAYER STILL LOADING",
           "an admitted peer that never asks for the world must be reported");

    Harness loading{protocol};
    const std::uint8_t loading_gid = loading.admit(bob, "Bob");
    loading.send_direct(bob, loading_gid, "RequestWorldInfoMessage");
    loading.send_direct(bob, loading_gid, "ClientReadyForChunksMessage");
    static_cast<void>(loading.pump());
    loading.advance(legacy_steam_join_loading_overdue + std::chrono::seconds{1});
    step = loading.pump();
    const LegacySteamJoinNotice *slow = step.notice(LegacySteamJoinNoticeKind::loading_overdue);
    expect(slow != nullptr && slow->name == "Bob" && slow->reason == "StillLoadingTerrain",
           "a peer stuck after its terrain requests must be reported with that phase");
    static_cast<void>(gid);
}

// The helper-to-host "entered the lobby" record.
void lobby_member_records_are_bounded()
{
    SteamBridgeProtocolError error{};
    const auto encoded = encode_steam_bridge_member({76'561'198'000'000'001ULL, "Alice"}, error);
    SteamBridgeMember decoded;
    expect(encoded.has_value() && decode_steam_bridge_member(*encoded, decoded, error) &&
               decoded.peer == 76'561'198'000'000'001ULL && decoded.persona_name == "Alice",
           "a lobby member must round-trip through the bridge codec");
    const auto unnamed = encode_steam_bridge_member({42U, {}}, error);
    expect(unnamed.has_value() && decode_steam_bridge_member(*unnamed, decoded, error) &&
               decoded.persona_name.empty(),
           "a member whose persona is not resolved yet must still be reported");
    expect(!encode_steam_bridge_member({0U, "Nobody"}, error).has_value() &&
               !encode_steam_bridge_member({42U, std::string(129U, 'x')}, error).has_value() &&
               !encode_steam_bridge_member({42U, std::string{"bad\nname"}}, error).has_value(),
           "an invalid Steam ID, an oversized name and control characters must be refused");
    auto trailing = *encoded;
    trailing.push_back(0U);
    expect(!decode_steam_bridge_member(trailing, decoded, error) &&
               error == SteamBridgeProtocolError::trailing_data &&
               !decode_steam_bridge_member(std::span<const std::uint8_t>{encoded->data(), 4U},
                                           decoded, error),
           "malformed member records must fail closed");
    const auto framed =
        encode_steam_bridge_frame(SteamBridgeMessageType::peer_entered, 3U, *encoded, error);
    SteamBridgeFrame bridge_frame;
    expect(framed.has_value() && decode_steam_bridge_frame(*framed, bridge_frame, error) &&
               bridge_frame.type == SteamBridgeMessageType::peer_entered,
           "the member record must travel in a valid bridge frame");
}

// HostOptions as the shipped host carries it out. Kick and Ban are a KickMessage
// broadcast naming the player's GID (the named client leaves by itself). The
// PVP row is session property 5: SteamNetworkSessionProvider.UpdateHostSession
// stores it and rewrites the lobby data, and sends connected peers nothing.
void host_options_reach_the_session_as_the_original_host_sends_them(ProtocolVersion protocol)
{
    Harness harness{protocol};
    const std::uint8_t alice_gid = harness.admit(alice, "Alice");
    const std::uint8_t bob_gid = harness.admit(bob, "Bob");
    static_cast<void>(harness.pump());
    expect(alice_gid != bob_gid, "two admitted peers must hold different GIDs");

    for (const bool banned : {false, true})
    {
        KickPayload kick;
        kick.banned = banned;
        kick.player_id = bob_gid;
        PayloadError payload_error{};
        const auto payload = encode_kick_payload(kick, payload_error);
        expect(payload.has_value(), "the kick must encode");
        LegacySteamOutbound outbound;
        outbound.application_packet = frame(protocol, "KickMessage", *payload);
        std::string error;
        expect(harness.host.send_authoritative(outbound, error), error);
        const Harness::Step step = harness.pump();
        for (const PeerId peer : {alice, bob})
        {
            const Delivered *delivered = step.find(peer, "KickMessage");
            KickPayload decoded;
            expect(delivered != nullptr && delivered->sender_gid == 0U &&
                       decode_kick_payload(delivered->payload, decoded, payload_error) &&
                       decoded == kick,
                   "a kick or ban must reach every peer from the host, naming the player's GID");
        }
        expect(harness.host.gid_for(bob) == bob_gid,
               "the host must not drop the peer itself: the named client leaves on its own");
    }

    const SessionProperties before = harness.host.session_properties();
    expect(before[5] == 0, "a session hosted with PVP off must publish slot 5 as zero");
    std::string error;
    expect(harness.host.update_session_property(5U, 1, error), error);
    Harness::Step step = harness.pump();
    SessionProperties expected = before;
    expected[5] = 1;
    expect(step.sent.empty() && harness.host.session_properties() == expected,
           "a PVP change must update slot 5 only and send connected peers nothing");
    expect(!harness.host.update_session_property(8U, 1, error) && !error.empty() &&
               harness.host.session_properties() == expected,
           "an out-of-range session property must be refused and change nothing");

    // The next joiner reads the rewritten lobby and is admitted with it.
    harness.settings.player_versus_player = PlayerVersusPlayer::everyone;
    harness.request_join(carol, "Carol", wifi_player_id);
    step = harness.pump();
    expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U &&
               step.count(LegacySteamRuntimeEventKind::rejected) == 0U,
           "a client that read the new PVP state from the lobby must be admitted");
}

// HostOptions' three lobby rows. In the shipped host each one ends in
// SteamWorks.UpdateHostLobbyData, which rewrites the lobby's Name / Protected /
// JoinPolicy keys and its lobby type, and none of them sends the connected
// session anything (read from 1.6.3.8 and 1.9.9.8: GameScreen
// ._hostOptionsMenu_MenuItemSelected, SteamNetworkSessionProvider
// .UpdateHostSession / UpdateHostSessionJoinPolicy).
void set_password_server_message_and_who_can_join_rewrite_the_lobby_only(ProtocolVersion protocol)
{
    Harness harness{protocol};
    static_cast<void>(harness.admit(alice, "Alice"));
    static_cast<void>(harness.pump());
    const auto lobby = [&] {
        const auto advertised = harness.host.advertised_lobby();
        expect(advertised.has_value(), "a running host must advertise a lobby");
        return *advertised;
    };
    const SteamBridgeHostConfig created = lobby();
    expect(created.server_name == "Join Test" && !created.password_protected &&
               created.join_policy == SteamBridgeJoinPolicy::anyone,
           "fixture: an open, public lobby named by the host");
    std::string error;

    // Server Message: the lobby Name. A blank entry is ignored.
    expect(harness.host.update_server_message("Night shift, bring torches", error), error);
    Harness::Step step = harness.pump();
    SteamBridgeHostConfig expected = created;
    expected.server_name = "Night shift, bring torches";
    expect(step.sent.empty() && lobby().server_name == expected.server_name &&
               lobby().password_protected == created.password_protected &&
               lobby().join_policy == created.join_policy &&
               lobby().session_properties == created.session_properties,
           "Server Message must change the lobby Name only and send the session nothing");
    expect(harness.host.update_server_message("   ", error) &&
               lobby().server_name == expected.server_name,
           "a blank Server Message must leave the lobby Name as it was");

    // Set Password: Protected, and the password every LATER join must carry.
    expect(harness.host.update_password("s3cret pass", error), error);
    step = harness.pump();
    expect(step.sent.empty() && lobby().password_protected &&
               harness.host.gid_for(alice).has_value(),
           "Set Password must mark the lobby Protected, tell nobody and drop nobody");
    harness.enter_lobby(bob, "Bob");
    harness.request_join(bob, "Bob", wifi_player_id);
    step = harness.pump();
    const LegacySteamJoinNotice *refused = step.notice(LegacySteamJoinNoticeKind::refused);
    expect(refused != nullptr && refused->reason == "IncorrectPassword" &&
               !harness.host.gid_for(bob).has_value(),
           "a join without the new password must get the stock IncorrectPassword denial");
    harness.request_join(bob, "Bob", wifi_player_id, std::string{"s3cret pass"});
    step = harness.pump();
    expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U &&
               harness.host.gid_for(bob).has_value(),
           "a join carrying the new password must be admitted");
    // Blank (or all whitespace) clears it: `string.IsNullOrWhiteSpace`.
    expect(harness.host.update_password(" \t ", error), error);
    step = harness.pump();
    expect(step.sent.empty() && !lobby().password_protected,
           "a blank password must clear Protected and send the session nothing");
    harness.enter_lobby(carol, "Carol");
    harness.request_join(carol, "Carol", wifi_player_id);
    step = harness.pump();
    expect(step.count(LegacySteamRuntimeEventKind::peer_connected) == 1U,
           "once the password is cleared a join without one must be admitted");

    // Who can join: JoinPolicy 0 -> 1 -> 2 -> 0 (Anyone, FriendsOnly, InviteOnly).
    for (const SteamBridgeJoinPolicy policy :
         {SteamBridgeJoinPolicy::friends_only, SteamBridgeJoinPolicy::invite_only,
          SteamBridgeJoinPolicy::anyone})
    {
        expect(harness.host.update_join_policy(policy, error), error);
        step = harness.pump();
        expect(step.sent.empty() && lobby().join_policy == policy &&
                   lobby().server_name == expected.server_name && !lobby().password_protected,
               "Who can join must change the lobby JoinPolicy only and send the session nothing");
    }
    expect(!harness.host.update_join_policy(static_cast<SteamBridgeJoinPolicy>(7), error) &&
               lobby().join_policy == SteamBridgeJoinPolicy::anyone,
           "an unknown join policy must be refused and change nothing");
    // The lobby the helper would publish is still one the stock contract takes.
    SteamBridgeProtocolError protocol_error{};
    expect(encode_steam_bridge_host_config(lobby(), protocol_error).has_value(),
           "the rewritten lobby must still encode for the Steam helper");
}

// PlayerStats.BanList: saved the moment it changes, per player rather than per
// world, keyed by Steam ID with the UTC time of the ban.
void the_ban_list_is_saved_when_it_changes_and_survives_a_restart()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "openminerz-host-ban-list-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    const std::filesystem::path file = directory / "nested" / "host-bans.cfg";
    {
        HostBanList list{file};
        expect(list.empty() && !list.contains(bob), "a missing file is an empty ban list");
        expect(list.ban(bob, 638'000'000'000'000'000LL) && list.ban(carol) &&
                   std::filesystem::exists(file),
               "a ban must reach the disk at once, creating the folder if it has to");
        expect(!list.ban(0U), "the zero Steam ID is nobody and cannot be banned");
    }
    {
        // A new session, another world: the same player's list.
        HostBanList list{file};
        expect(list.size() == 2U && list.contains(bob) && list.contains(carol) &&
                   !list.contains(alice) && list.banned_at(bob) == 638'000'000'000'000'000LL &&
                   list.banned_at(carol) > HostBanList::ticks_at_unix_epoch,
               "bans must survive a restart with their Steam ID and UTC ticks");
        expect(list.clear() && list.empty(), "Clear Ban List must empty the list");
    }
    {
        HostBanList list{file};
        expect(list.empty(), "a cleared ban list must stay cleared after a restart");
    }
    {
        // Damage: skip what cannot be read, keep what can.
        std::ofstream{file} << "# header\nnot a number 1\n0 5\n303 77\n101 9 extra\n202 -4\n";
        HostBanList list{file};
        expect(list.size() == 1U && list.contains(carol) && list.banned_at(carol) == 77,
               "a damaged ban list must load its readable lines and nothing else");
    }
    {
        HostBanList unsaved;
        expect(unsaved.ban(alice) && unsaved.contains(alice) && unsaved.clear(),
               "a list with no file still works for the session");
    }
    std::filesystem::remove_all(directory, ignored);
}

struct TestCase final
{
    std::string_view name;
    std::function<void()> body;
};
} // namespace

int main()
{
    const auto both = [](void (*test)(ProtocolVersion)) {
        return [test] {
            test(ProtocolVersion::classic_v3);
            test(ProtocolVersion::modern_v4);
        };
    };
    const std::vector<TestCase> tests{
        {"stock_client_join_order_reaches_gameplay",
         both(stock_client_join_order_reaches_gameplay)},
        {"terrain_requests_still_require_world_info",
         both(terrain_requests_still_require_world_info)},
        {"identity_is_the_steam_account_not_the_player_id_or_name",
         both(identity_is_the_steam_account_not_the_player_id_or_name)},
        {"refusals_name_the_player_and_reach_the_client",
         both(refusals_name_the_player_and_reach_the_client)},
        {"a_refused_openminerz_client_can_retry_its_handshake",
         both(a_refused_openminerz_client_can_retry_its_handshake)},
        {"an_unadmitted_member_leaving_is_reported_as_a_failed_join",
         both(an_unadmitted_member_leaving_is_reported_as_a_failed_join)},
        {"an_admitted_peer_that_never_finishes_loading_is_reported",
         both(an_admitted_peer_that_never_finishes_loading_is_reported)},
        {"lobby_member_records_are_bounded", lobby_member_records_are_bounded},
        {"host_options_reach_the_session_as_the_original_host_sends_them",
         both(host_options_reach_the_session_as_the_original_host_sends_them)},
        {"set_password_server_message_and_who_can_join_rewrite_the_lobby_only",
         both(set_password_server_message_and_who_can_join_rewrite_the_lobby_only)},
        {"the_ban_list_is_saved_when_it_changes_and_survives_a_restart",
         the_ban_list_is_saved_when_it_changes_and_survives_a_restart},
    };
    std::size_t failures{};
    for (const TestCase &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
