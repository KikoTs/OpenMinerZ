// Authority: the host decides in sessions only OpenMinerZ players can join.
//
// A host and two OpenMinerZ clients talk over the production LoopbackSession:
// admission with the extension hello for an "OpenMinerZ players only"
// manifest (no mod at all), stock gameplay messages on the stock channel, the
// authority's own messages on the mod channel. A simulated link on top adds
// latency and jitter (reliable traffic stays in order, unreliable may not).
// The host runs net::AuthorityHost the way the app does: every accepted stock
// message through observe_stock_message before it is relayed, the link on its
// ModNetServer, its world and enemies behind AuthorityWorld, its own tracer
// and enemy sightings fed in. Each client runs net::AuthorityClient.
//
// Hostile clients try god mode, inflated and impossible hits, hitting through
// walls and out of reach, rapid fire, spawning items, double pickups, reach,
// speed and teleport hacks and flying in Survival; each is refused with no
// effect on the other client, and STRICT kicks only on repetition. Honest
// clients at 200 ms with jitter play a minute without one correction or
// violation. And every session that is not authoritative keeps its bytes.

#include "openminerz/modding/net.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/loopback_session.hpp"
#include "openminerz/net/mod_catalog.hpp"
#include "openminerz/net/mod_handshake.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "openminerz/net/pickup_codec.hpp"
#include "openminerz/net/protocol.hpp"
#include "openminerz/net/session_authority.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
using namespace openminerz;
using namespace openminerz::net;
using modding::Handle;
using modding::HandleKind;

int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (condition)
        return;
    ++failures;
    std::cout << "[FAIL] " << message << '\n';
}

// ------------------------------------------------------------------- world

// Rock below y 1, air above, plus whatever a test builds. Enemies are the
// host's own copies, by (target gid, enemy id).
class FakeWorld final : public AuthorityWorld
{
  public:
    std::map<std::tuple<int, int, int>, world::BlockType> cells;
    std::map<std::pair<std::uint8_t, std::uint32_t>, AuthorityEnemy> enemies;

    [[nodiscard]] std::optional<world::BlockType> block(int x, int y, int z) const override
    {
        if (y < -64 || y > 127)
            return std::nullopt;
        const auto found = cells.find({x, y, z});
        if (found != cells.end())
            return found->second;
        return y < 1 ? world::BlockType::rock : world::BlockType::empty;
    }
    [[nodiscard]] std::optional<AuthorityEnemy> enemy(std::uint8_t target,
                                                      std::uint32_t id) const override
    {
        const auto found = enemies.find({target, id});
        return found == enemies.end() ? std::nullopt : std::optional<AuthorityEnemy>{found->second};
    }
    [[nodiscard]] float blast_line_of_sight(world::Vec3 impact, world::Vec3 target,
                                            world::DragonKind) const override
    {
        return clear_line(impact, target) ? 1.0F : 0.0F;
    }
    [[nodiscard]] bool clear_line(world::Vec3 from, world::Vec3 to) const override
    {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float dz = to.z - from.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        const int steps = std::max(1, static_cast<int>(length / 0.1F));
        for (int step = 1; step < steps; ++step)
        {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const auto cell = block(static_cast<int>(std::floor(from.x + dx * t)),
                                    static_cast<int>(std::floor(from.y + dy * t)),
                                    static_cast<int>(std::floor(from.z + dz * t)));
            if (cell.has_value() && world::blocks_player(*cell))
                return false;
        }
        return true;
    }
    void wall(int x, int y0, int y1, int z0, int z1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                cells[{x, y, z}] = world::BlockType::rock;
    }
};

// -------------------------------------------------------------------- link

// Latency and jitter over the LoopbackSession's queues. Reliable traffic
// arrives in order (the mod channel and stock reliable messages need that);
// unreliable datagrams may overtake each other.
struct LinkProfile final
{
    double delay_ms{};
    double jitter_ms{};
    // A lag spike: everything sent between these session times arrives
    // together after the spike, one delay later.
    double spike_from{-1.0};
    double spike_until{-1.0};
};

struct Arrival final
{
    double at{};
    std::uint64_t order{};
    bool to_host{};
    PeerId peer{};
    Datagram datagram;
};

class Link final
{
  public:
    Link(LinkProfile profile, std::uint32_t seed) : profile_{profile}, random_{seed}
    {
    }

    void schedule(double now, bool to_host, PeerId peer, Datagram datagram)
    {
        double at = now + profile_.delay_ms / 1000.0;
        if (profile_.jitter_ms > 0.0)
            at += (uniform() * 2.0 - 1.0) * profile_.jitter_ms / 1000.0;
        at = std::max(at, now);
        if (now >= profile_.spike_from && now < profile_.spike_until)
            at = std::max(at, profile_.spike_until + profile_.delay_ms / 1000.0);
        const bool ordered = datagram.delivery == MessageDeliveryPolicy::reliable_ordered ||
                             datagram.channel == openminerz_control_channel;
        if (ordered)
        {
            double &last = last_ordered_[{to_host, to_host ? datagram.sender : peer}];
            at = std::max(at, last);
            last = at;
        }
        queue_.push_back({at, ++order_, to_host, peer, std::move(datagram)});
    }

    [[nodiscard]] std::vector<Arrival> due(double now)
    {
        std::stable_sort(queue_.begin(), queue_.end(), [](const Arrival &a, const Arrival &b) {
            return std::tie(a.at, a.order) < std::tie(b.at, b.order);
        });
        std::vector<Arrival> ready;
        auto split = std::find_if(queue_.begin(), queue_.end(),
                                  [&](const Arrival &arrival) { return arrival.at > now; });
        for (auto entry = queue_.begin(); entry != split; ++entry)
            ready.push_back(std::move(*entry));
        queue_.erase(queue_.begin(), split);
        return ready;
    }

  private:
    double uniform()
    {
        return std::uniform_real_distribution<double>{0.0, 1.0}(random_);
    }

    LinkProfile profile_;
    std::mt19937 random_;
    std::vector<Arrival> queue_;
    std::map<std::pair<bool, PeerId>, double> last_ordered_;
    std::uint64_t order_{};
};

// ---------------------------------------------------------------- packets

std::vector<std::uint8_t> packet(ProtocolVersion protocol, std::string_view name,
                                 std::span<const std::uint8_t> payload)
{
    const MessageDescriptor *message = message_by_name(protocol, name);
    if (message == nullptr)
        throw std::runtime_error{"unknown message"};
    PacketError error{};
    const auto bytes = encode_packet(protocol, message->id, payload, error);
    if (!bytes.has_value())
        throw std::runtime_error{"packet encoding failed"};
    return *bytes;
}

MessageDeliveryPolicy delivery_of(ProtocolVersion protocol, std::string_view name,
                                  std::uint8_t flags = 0U)
{
    const MessageDescriptor *message = message_by_name(protocol, name);
    if (message->transport.delivery == MessageDeliveryPolicy::player_update_state)
        return (flags & (PlayerUpdatePayload::using_flag | PlayerUpdatePayload::reloading_flag)) != 0U
                   ? MessageDeliveryPolicy::reliable_ordered
                   : MessageDeliveryPolicy::unreliable_sequenced;
    return message->transport.delivery;
}

template <typename Encoded> std::vector<std::uint8_t> must(const Encoded &encoded)
{
    if (!encoded.has_value())
        throw std::runtime_error{"payload encoding failed"};
    return *encoded;
}

std::int16_t wire_item(InventoryItemId item, ProtocolVersion protocol)
{
    std::int16_t wire{};
    if (!inventory_item_to_wire(item, protocol, wire))
        throw std::runtime_error{"item has no wire id in this protocol"};
    return wire;
}

// ---------------------------------------------------------------- session

HostSessionSettings settings_for(ProtocolVersion protocol, bool openminerz_only,
                                 PlayerVersusPlayer pvp = PlayerVersusPlayer::everyone)
{
    HostSessionSettings settings;
    settings.server_name = "Authority";
    settings.protocol = protocol;
    settings.maximum_players = 8;
    settings.visibility = HostVisibility::public_lobby;
    settings.player_versus_player = pvp;
    settings.mod_manifest.protocol = protocol;
    settings.mod_manifest.openminerz_only = openminerz_only;
    return settings;
}

AdmissionStatus admit(LoopbackSession &session, const HostSessionSettings &settings, PeerId peer,
                      bool extension, std::int32_t mode = 1, std::int32_t difficulty = 1,
                      bool infinite = false)
{
    PeerAdmissionRequest request;
    request.peer = peer;
    request.properties = make_session_properties(settings, false, mode, difficulty, infinite);
    request.advertised_extension = extension;
    const AdmissionDecision decision = session.connect(request);
    if (!extension || !decision.accepted())
        return decision.status;
    ExtensionMessage hello;
    hello.kind = ExtensionMessageKind::client_hello;
    hello.nonce = 0x4155U + peer;
    hello.manifest = settings.mod_manifest;
    static_cast<void>(session.submit_from_peer(peer, encode_extension_message(hello),
                                               MessageDeliveryPolicy::reliable_ordered,
                                               openminerz_control_channel));
    static_cast<void>(session.receive_for_peer(peer)); // the accept
    return session.admission().status(peer);
}

// One joined OpenMinerZ client: its mod channel, its authority half, and
// the little of the game the tests need.
struct Client final
{
    PeerId peer{};
    std::uint8_t gid{};
    std::unique_ptr<modding::ModNetClient> channel;
    std::unique_ptr<AuthorityClient> authority;
    world::Vec3 position{};
    float health{100.0F};
    bool alive{true};
    std::uint32_t corrections{};
    std::uint32_t deaths{};
    std::uint32_t respawns{};
    std::uint32_t notices{};
    std::vector<std::string> received; // stock messages relayed to this client
    std::uint32_t consumed{};          // pickups the host gave it
    double last_health_at{-1.0};
    std::vector<double> health_latencies;
    double pending_report_at{-1.0};
};

// The host: authority, mod channel and the stock relay, as the app runs them.
struct Session final
{
    ProtocolVersion protocol;
    HostSessionSettings settings;
    LoopbackSession wire;
    Link link;
    FakeWorld world;
    std::unique_ptr<AuthorityHost> authority;
    std::unique_ptr<modding::ModNetServer> server;
    std::unique_ptr<AuthorityServerLink> server_link;
    std::map<PeerId, Client> clients;
    double now{};
    std::vector<AuthorityEvent> events;
    std::vector<std::string> refused;
    std::uint32_t pickups_granted{};
    std::uint32_t next_pickup{100U};
    AuthorityRules rules_;

    Session(ProtocolVersion protocol_version, AuthorityRules rules, LinkProfile profile,
            std::uint32_t seed = 7U, bool openminerz_only = true)
        : protocol{protocol_version},
          settings{settings_for(protocol_version, openminerz_only, rules.pvp)},
          wire{settings, false, static_cast<std::int32_t>(rules.mode),
               static_cast<std::int32_t>(rules.difficulty), rules.infinite_resources},
          link{profile, seed}
    {
        rules.protocol = protocol;
        rules_ = rules;
        authority = std::make_unique<AuthorityHost>(rules);
        authority->set_world(&world);
        authority->add_player(0U, now, true);
        server = std::make_unique<modding::ModNetServer>(settings.mod_manifest);
        server_link = std::make_unique<AuthorityServerLink>(
            *authority, *server,
            [this](const Handle &player) -> std::optional<std::uint8_t> {
                for (const auto &[peer, client] : clients)
                    if (player.id == client.gid)
                        return client.gid;
                return std::nullopt;
            },
            [this](std::uint8_t gid) -> std::optional<Handle> {
                for (const auto &[peer, client] : clients)
                    if (client.gid == gid)
                        return Handle{HandleKind::player, gid};
                return std::nullopt;
            },
            [this] { return now; });
    }
    ~Session()
    {
        server_link.reset();
        for (auto &[peer, client] : clients)
            client.authority.reset();
    }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    Client &join(PeerId peer, std::uint8_t gid, world::Vec3 position)
    {
        if (admit(wire, settings, peer, true, static_cast<std::int32_t>(rules_.mode),
                  static_cast<std::int32_t>(rules_.difficulty), rules_.infinite_resources) !=
            AdmissionStatus::admitted)
            throw std::runtime_error{"an OpenMinerZ client is admitted"};
        Client &client = clients[peer];
        client.peer = peer;
        client.gid = gid;
        client.position = position;
        client.channel = std::make_unique<modding::ModNetClient>(settings.mod_manifest);
        client.authority = std::make_unique<AuthorityClient>(*client.channel);
        if (!server->add_peer(peer, Handle{HandleKind::player, gid}, now))
            throw std::runtime_error{"the client gets a mod channel"};
        authority->add_player(gid, now);
        client.channel->set_ready(now);
        return client;
    }

    [[nodiscard]] Client &client(PeerId peer)
    {
        return clients.at(peer);
    }

    // A stock message from a client, through the session's own gates.
    bool send(PeerId peer, std::string_view name, std::span<const std::uint8_t> payload,
              std::uint8_t flags = 0U)
    {
        return wire.submit_from_peer(peer, packet(protocol, name, payload),
                                     delivery_of(protocol, name, flags), stock_gameplay_channel)
            .accepted();
    }

    void update_position(PeerId peer, world::Vec3 position, std::uint8_t flags = 0U)
    {
        clients.at(peer).position = position;
        PlayerUpdatePayload update;
        update.local_position = {position.x, position.y, position.z};
        update.state_flags = flags;
        PayloadError error{};
        static_cast<void>(send(peer, "PlayerUpdateMessage",
                               must(encode_player_update_payload(update, protocol, error)), flags));
    }

    void carry(PeerId peer, InventoryItemId item)
    {
        const std::uint16_t wire_id = static_cast<std::uint16_t>(wire_item(item, protocol));
        const std::array<std::uint8_t, 2> payload{static_cast<std::uint8_t>(wire_id & 0xFFU),
                                                  static_cast<std::uint8_t>(wire_id >> 8U)};
        static_cast<void>(send(peer, "ChangeCarriedItemMessage", payload));
    }

    void melee(PeerId attacker, PeerId victim, InventoryItemId item)
    {
        MeleePlayerPayload melee;
        melee.item_id = wire_item(item, protocol);
        const world::Vec3 at = clients.at(victim).position;
        melee.damage_source = {at.x, at.y + 1.0F, at.z};
        PayloadError error{};
        const auto bytes = packet(protocol, "MeleePlayerMessage",
                                  must(encode_melee_player_payload(melee, protocol, error)));
        // MeleePlayerMessage goes to the victim alone; the host routes it.
        pending_direct_.push_back({attacker, clients.at(victim).gid, bytes});
    }

    void gunshot(PeerId shooter, InventoryItemId item, world::Vec3 direction)
    {
        GunshotPayload shot;
        shot.direction = {direction.x, direction.y, direction.z};
        shot.item_id = wire_item(item, protocol);
        PayloadError error{};
        static_cast<void>(send(shooter, "GunshotMessage",
                               must(encode_gunshot_payload(shot, protocol, error))));
    }

    void alter_block(PeerId peer, IntVector3 cell, world::BlockType block)
    {
        AlterBlockPayload edit;
        edit.block_location = cell;
        edit.block_type = static_cast<std::int32_t>(block);
        PayloadError error{};
        static_cast<void>(send(peer, "AlterBlockMessage",
                               must(encode_alter_block_payload(edit, protocol, error))));
    }

    std::uint32_t create_pickup(PeerId peer, InventoryItemId item, std::uint16_t count,
                                world::Vec3 at)
    {
        CreatePickupPayload pickup;
        pickup.spawn_position = {at.x, at.y, at.z};
        pickup.pickup_id = static_cast<std::int32_t>(++next_pickup);
        pickup.item.item_id = wire_item(item, protocol);
        pickup.item.stack_count = static_cast<std::int16_t>(count);
        pickup.item.health = 1.0F;
        pickup.dropped = true;
        PayloadError error{};
        static_cast<void>(send(peer, "CreatePickupMessage",
                               must(encode_create_pickup_payload(pickup, protocol, error))));
        return next_pickup;
    }

    void request_pickup(PeerId peer, std::uint8_t spawner, std::uint32_t id)
    {
        RequestPickupPayload request;
        request.pickup_id = static_cast<std::int32_t>(id);
        request.spawner_id = spawner;
        PayloadError error{};
        static_cast<void>(send(peer, "RequestPickupMessage",
                               must(encode_request_pickup_payload(request, error))));
    }

    void store_inventory(PeerId peer, std::span<const InventoryStack> stacks)
    {
        InventoryStorePayload stored;
        stored.inventory.format_version = 3;
        stored.inventory.trays.resize(protocol == ProtocolVersion::modern_v4 ? 2U : 1U);
        std::size_t slot{};
        for (const InventoryStack &stack : stacks)
        {
            InventorySlotPayload payload;
            if (!inventory_slot_to_payload(PlayerInventory::Slot{stack}, protocol, payload))
                throw std::runtime_error{"inventory slot encoding"};
            stored.inventory.storage[slot++] = payload;
        }
        PayloadError error{};
        static_cast<void>(send(peer, "InventoryStoreOnServerMessage",
                               must(encode_inventory_store_payload(stored, protocol, error))));
    }

    // One step of every machine, then everything due on the link.
    void step(double seconds = 1.0 / 60.0)
    {
        now += seconds;
        for (const Direct &direct : std::exchange(pending_direct_, {}))
        {
            PeerId sender = direct.sender;
            // Steam carries a direct message to the host, which routes it.
            Datagram datagram;
            datagram.sender = sender;
            datagram.recipient = direct.recipient_gid;
            datagram.payload = direct.bytes;
            datagram.delivery = MessageDeliveryPolicy::reliable_ordered;
            datagram.channel = stock_gameplay_channel;
            link.schedule(now, true, sender, std::move(datagram));
        }
        for (auto &[peer, client] : clients)
        {
            client.channel->update(now);
            for (std::vector<std::uint8_t> &frame : client.channel->take_sends())
                static_cast<void>(wire.submit_from_peer(peer, frame, MessageDeliveryPolicy::reliable_ordered,
                                                        openminerz_control_channel));
        }
        collect();
        deliver();
        authority->update(now);
        for (AuthorityEvent &event : server_link->flush())
        {
            if (event.kind == AuthorityEventKind::kick)
                server_link->notice_all(std::format("removed by the host's anti-cheat: {}", event.reason));
            events.push_back(std::move(event));
        }
        server->update(now);
        for (modding::ModNetSend &send : server->take_sends())
            static_cast<void>(wire.submit_from_host(send.peer, send.frame,
                                                    MessageDeliveryPolicy::reliable_ordered,
                                                    openminerz_control_channel));
        collect();
        deliver();
        for (auto &[peer, client] : clients)
            take_client_events(client);
    }

    void run(double seconds, const std::function<void()> &each_step = {})
    {
        const int steps = static_cast<int>(std::lround(seconds * 60.0));
        for (int index = 0; index < steps; ++index)
        {
            if (each_step)
                each_step();
            step();
        }
    }

    [[nodiscard]] std::size_t count(AuthorityEventKind kind, std::optional<std::uint8_t> gid = {}) const
    {
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [&](const AuthorityEvent &event) {
            return event.kind == kind && (!gid.has_value() || event.gid == *gid);
        }));
    }

    [[nodiscard]] AuthorityPlayerView view(PeerId peer) const
    {
        return *authority->player(clients.at(peer).gid);
    }

  private:
    struct Direct final
    {
        PeerId sender{};
        std::uint8_t recipient_gid{};
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Direct> pending_direct_;

    void collect()
    {
        while (auto datagram = wire.receive_for_host())
            link.schedule(now, true, datagram->sender, std::move(*datagram));
        for (auto &[peer, client] : clients)
            while (auto datagram = wire.receive_for_peer(peer))
                link.schedule(now, false, peer, std::move(*datagram));
    }

    void deliver()
    {
        for (Arrival &arrival : link.due(now))
        {
            if (arrival.to_host)
            {
                if (arrival.datagram.channel == openminerz_control_channel)
                    server->receive(arrival.datagram.sender, arrival.datagram.payload, now);
                else
                    host_stock(arrival.datagram);
                continue;
            }
            const auto found = clients.find(arrival.peer);
            if (found == clients.end())
                continue;
            Client &client = found->second;
            if (arrival.datagram.channel == openminerz_control_channel)
                client.channel->receive(arrival.datagram.payload, now);
            else
                client_stock(client, arrival.datagram);
        }
    }

    [[nodiscard]] std::uint8_t gid_of(PeerId peer) const
    {
        const auto found = clients.find(peer);
        return found == clients.end() ? 0U : found->second.gid;
    }

    // handle_online_gameplay: the authority first, then the relay.
    void host_stock(const Datagram &datagram)
    {
        DecodedPacket decoded;
        PacketError error{};
        if (!decode_packet(protocol, datagram.payload, decoded, error) || decoded.message == nullptr)
            return;
        const std::uint8_t sender = gid_of(datagram.sender);
        const std::uint8_t recipient = static_cast<std::uint8_t>(datagram.recipient);
        const AuthorityVerdict verdict = authority->observe_stock_message(
            sender, recipient, decoded.message->name, decoded.payload, now);
        if (verdict == AuthorityVerdict::refuse)
        {
            refused.emplace_back(decoded.message->name);
            return;
        }
        const std::string_view name = decoded.message->name;
        if (name == "GunshotMessage")
            trace_shot(sender, decoded.payload);
        if (name == "AlterBlockMessage")
        {
            // The host's own terrain takes the edit it accepted.
            AlterBlockPayload edit;
            PayloadError payload_error{};
            if (decode_alter_block_payload(decoded.payload, protocol, edit, payload_error))
                world.cells[{edit.block_location.x, edit.block_location.y, edit.block_location.z}] =
                    static_cast<world::BlockType>(edit.block_type);
        }
        if (name == "RequestPickupMessage")
        {
            RequestPickupPayload request;
            PayloadError payload_error{};
            if (decode_request_pickup_payload(decoded.payload, request, payload_error))
                grant_pickup(sender, request);
            return; // only the host acts on it
        }
        for (auto &[peer, client] : clients)
        {
            if (peer == datagram.sender)
                continue;
            if (recipient != 0U && client.gid != recipient)
                continue;
            if (name == "MeleePlayerMessage")
            {
                client.received.emplace_back(name);
                continue;
            }
            static_cast<void>(wire.relay_from_host(datagram.sender, peer, datagram.payload,
                                                   datagram.delivery));
        }
    }

    // update_tracers on the host: the shot from the shooter's eye along its
    // direction, stopped by terrain, against every other player.
    void trace_shot(std::uint8_t shooter, std::span<const std::uint8_t> payload)
    {
        GunshotPayload shot;
        PayloadError error{};
        if (!decode_gunshot_payload(payload, protocol, shot, error))
            return;
        world::Vec3 eye{};
        for (const auto &[peer, client] : clients)
            if (client.gid == shooter)
                if (const auto view = authority->player(shooter); view.has_value() && view->position)
                    eye = {view->position->x, view->position->y + 1.4F, view->position->z};
        world::Vec3 head{eye.x + shot.direction.x * 100.0F, eye.y + shot.direction.y * 100.0F,
                         eye.z + shot.direction.z * 100.0F};
        // Terrain first: shorten the streak to the first solid cell.
        for (float distance = 0.1F; distance < 100.0F; distance += 0.1F)
        {
            const world::Vec3 point{eye.x + shot.direction.x * distance,
                                    eye.y + shot.direction.y * distance,
                                    eye.z + shot.direction.z * distance};
            const auto block = world.block(static_cast<int>(std::floor(point.x)),
                                           static_cast<int>(std::floor(point.y)),
                                           static_cast<int>(std::floor(point.z)));
            if (block.has_value() && world::blocks_player(*block))
            {
                head = point;
                break;
            }
        }
        for (const auto &[peer, client] : clients)
            if (client.gid != shooter && authority->pvp_allows(shooter, client.gid) &&
                authority->tracer_hits_player(client.gid, eye, head, now).has_value())
                authority->harm(client.gid, 21.0F, HarmCause::player_gun, head, now, shooter);
    }

    void grant_pickup(std::uint8_t requester, const RequestPickupPayload &request)
    {
        ++pickups_granted;
        ConsumePickupPayload consume;
        consume.pickup_id = request.pickup_id;
        consume.spawner_id = request.spawner_id;
        consume.picker_upper = requester;
        consume.item.item_id = wire_item(InventoryItemId::diamond, protocol);
        consume.item.stack_count = 1;
        consume.item.health = 1.0F;
        PayloadError error{};
        const auto bytes = packet(protocol, "ConsumePickupMessage",
                                  must(encode_consume_pickup_payload(consume, protocol, error)));
        for (auto &[peer, client] : clients)
            static_cast<void>(wire.submit_from_host(peer, bytes,
                                                    delivery_of(protocol, "ConsumePickupMessage")));
    }

    void client_stock(Client &client, const Datagram &datagram)
    {
        DecodedPacket decoded;
        PacketError error{};
        if (!decode_packet(protocol, datagram.payload, decoded, error) || decoded.message == nullptr)
            return;
        client.received.emplace_back(decoded.message->name);
        if (decoded.message->name == "ConsumePickupMessage")
        {
            ConsumePickupPayload consume;
            PayloadError payload_error{};
            if (decode_consume_pickup_payload(decoded.payload, protocol, consume, payload_error) &&
                consume.picker_upper == client.gid)
                ++client.consumed;
        }
    }

    void take_client_events(Client &client)
    {
        for (const AuthorityClientEvent &event : client.authority->take_events())
            switch (event.kind)
            {
            case AuthorityClientEventKind::health:
                if (client.pending_report_at >= 0.0)
                {
                    client.health_latencies.push_back(now - client.pending_report_at);
                    client.pending_report_at = -1.0;
                }
                client.health = event.health;
                client.alive = event.alive;
                client.last_health_at = now;
                break;
            case AuthorityClientEventKind::died:
                client.alive = false;
                client.health = 0.0F;
                ++client.deaths;
                break;
            case AuthorityClientEventKind::respawned:
                client.alive = true;
                client.health = event.health;
                ++client.respawns;
                break;
            case AuthorityClientEventKind::correction:
                ++client.corrections;
                client.position = event.position;
                break;
            case AuthorityClientEventKind::notice:
                ++client.notices;
                break;
            }
    }
};

AuthorityRules survival_rules(PlayerVersusPlayer pvp = PlayerVersusPlayer::everyone, bool strict = true)
{
    AuthorityRules rules;
    rules.mode = GameMode::survival;
    rules.difficulty = GameDifficulty::hard;
    rules.pvp = pvp;
    rules.strict = strict;
    rules.spawn_point = {0.0F, 1.0F, 0.0F};
    return rules;
}

// Puts both clients in the world, standing, with their first updates in.
void settle(Session &session, PeerId a, world::Vec3 at_a, PeerId b, world::Vec3 at_b)
{
    session.join(a, 1U, at_a);
    session.join(b, 2U, at_b);
    session.run(0.3, [&] {
        session.update_position(a, at_a);
        session.update_position(b, at_b);
    });
}

// =========================================================== the sessions

void openminerz_only_is_a_session_property_the_lobby_carries()
{
    HostSessionSettings settings = settings_for(ProtocolVersion::classic_v3, true);
    expect(requires_extension(settings.mod_manifest) && session_authoritative(settings.mod_manifest),
           "OpenMinerZ players only makes the session authoritative with no mod at all");
    // The lobby advertisement says it, so the browser and the joiner know.
    ExtensionMessage advertisement;
    advertisement.kind = ExtensionMessageKind::server_accept;
    advertisement.manifest = settings.mod_manifest;
    const std::vector<std::uint8_t> bytes = encode_extension_message(advertisement);
    ExtensionMessage decoded;
    ExtensionDecodeError error{};
    expect(decode_extension_message(bytes, decoded, error) && decoded.manifest.openminerz_only &&
               bytes.size() == 23U && bytes[7] == 2U,
           "the advertisement carries the rule in bit 1 of the flags byte");
    SessionModSelection selection = select_session_mods({}, {}, &decoded.manifest, ProtocolVersion::classic_v3);
    expect(selection.manifest.openminerz_only && requires_extension(selection.manifest),
           "the joiner's hello and mod channel follow the lobby's rule");
    const ModCompatibilityResult stock = compare_mod_manifests(settings.mod_manifest, nullptr, false);
    expect(stock.status == ModCompatibility::extension_required &&
               stock.detail == "This host admits OpenMinerZ players only.",
           "a capability-silent peer is told why");

    LoopbackSession wire{settings, false, 1, 1, false};
    expect(admit(wire, settings, 301U, false) == AdmissionStatus::rejected_extension_required,
           "a stock client is refused by an OpenMinerZ-only session");
    expect(admit(wire, settings, 302U, true) == AdmissionStatus::admitted &&
               wire.admission().may_exchange_mod_frames(302U),
           "an OpenMinerZ client is admitted and gets the mod channel");

    // The stock denial is the retail word.
    LegacySteamHostSessionOptions options;
    options.settings = settings;
    expect(legacy_modded_session_denial == "ConnectionDenied",
           "the stock client reads the retail ConnectionDenied");
}

void sessions_that_are_not_authoritative_keep_their_bytes()
{
    // The extension bytes of a vanilla and a modded host are what they were:
    // flags 0 and 1, never the new bit.
    for (const ProtocolVersion protocol : {ProtocolVersion::classic_v3, ProtocolVersion::modern_v4})
    {
        ExtensionMessage vanilla;
        vanilla.kind = ExtensionMessageKind::server_accept;
        vanilla.manifest.protocol = protocol;
        std::vector<std::uint8_t> expected{'O', 'M', 'Z', 'E', 1U, 2U, static_cast<std::uint8_t>(protocol),
                                           0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
        const std::uint32_t crc = extension_crc32(expected);
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
            expected.push_back(static_cast<std::uint8_t>((crc >> shift) & 0xFFU));
        expect(encode_extension_message(vanilla) == expected,
               "a vanilla host advertises exactly the bytes it always did");
        ModManifest modded;
        modded.protocol = protocol;
        std::array<std::uint8_t, 32> digest{};
        digest[3] = 9U;
        modded.mods.push_back({"story", "1.0", digest, ModScope::server});
        ExtensionMessage hello;
        hello.manifest = modded;
        const std::vector<std::uint8_t> modded_bytes = encode_extension_message(hello);
        expect(modded_bytes[7] == 1U, "a modded host's flags byte is unchanged");
        expect(session_authoritative(modded) && !session_authoritative(vanilla.manifest),
               "server mods make a session authoritative; a vanilla one is not");
    }
    // Stock peer present: a vanilla host admits a stock client and its
    // gameplay reaches the other peer byte for byte, and nothing on the
    // control channel ever flows. Pinned as a transcript hash.
    HostSessionSettings settings = settings_for(ProtocolVersion::classic_v3, false);
    LoopbackSession wire{settings, false, 1, 1, false};
    expect(admit(wire, settings, 401U, false) == AdmissionStatus::admitted &&
               admit(wire, settings, 402U, true) == AdmissionStatus::admitted,
           "a vanilla host admits a stock and an OpenMinerZ client");
    expect(!wire.admission().may_exchange_mod_frames(401U) &&
               !wire.admission().may_exchange_mod_frames(402U),
           "no mod channel exists in a vanilla session");
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](std::span<const std::uint8_t> bytes) {
        for (const std::uint8_t byte : bytes)
            hash = (hash ^ byte) * 1099511628211ULL;
    };
    PayloadError error{};
    PlayerUpdatePayload update;
    update.local_position = {12.5F, 3.0F, -4.25F};
    update.yaw = 17U;
    const auto update_bytes = packet(ProtocolVersion::classic_v3, "PlayerUpdateMessage",
                                     must(encode_player_update_payload(update, ProtocolVersion::classic_v3, error)));
    AlterBlockPayload edit;
    edit.block_location = {40, 2, 40};
    edit.block_type = static_cast<std::int32_t>(world::BlockType::rock);
    const auto edit_bytes = packet(ProtocolVersion::classic_v3, "AlterBlockMessage",
                                   must(encode_alter_block_payload(edit, ProtocolVersion::classic_v3, error)));
    for (const auto &bytes : {update_bytes, edit_bytes})
    {
        const MessageDescriptor *message = nullptr;
        DecodedPacket decoded;
        PacketError packet_error{};
        if (decode_packet(ProtocolVersion::classic_v3, bytes, decoded, packet_error))
            message = decoded.message;
        const MessageDeliveryPolicy delivery = delivery_of(ProtocolVersion::classic_v3, message->name);
        expect(wire.submit_from_peer(401U, bytes, delivery, stock_gameplay_channel).accepted(),
               "the stock peer's gameplay is accepted");
        auto received = wire.receive_for_host();
        expect(received.has_value() && received->payload == bytes, "and reaches the host unchanged");
        if (!received.has_value())
            continue;
        mix(received->payload);
        expect(wire.relay_from_host(401U, 402U, received->payload, received->delivery).accepted(),
               "and the host relays it");
        auto relayed = wire.receive_for_peer(402U);
        expect(relayed.has_value() && relayed->payload == bytes, "byte for byte");
        if (relayed.has_value())
            mix(relayed->payload);
    }
    const std::vector<std::uint8_t> control{'O', 'M', 'Z', 'E', 1U, 16U, 0U, 0U};
    expect(!wire.submit_from_peer(401U, control, MessageDeliveryPolicy::reliable_ordered,
                                  openminerz_control_channel)
                .accepted() &&
               !wire.submit_from_host(402U, control, MessageDeliveryPolicy::reliable_ordered,
                                      openminerz_control_channel)
                    .accepted(),
           "no mod frame passes either way in a vanilla session");
    std::cout << "stock peer present transcript: " << std::hex << hash << std::dec << '\n';
    expect(hash == 0x5436bee19a267cebULL, "the stock-peer transcript is byte for byte what it was");
}

// ------------------------------------------------------------ hostile ones

void god_mode_is_impossible()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {}};
    settle(session, 101U, {0.5F, 1.0F, 20.5F}, 102U, {10.5F, 1.0F, 20.5F});
    // The host's own copy of a zombie chasing client 1 swings at it twice a
    // second; client 1 never reports a hit (its damage is "off").
    session.world.enemies[{std::uint8_t{1U}, 7U}] = {1, {0.5F, 1.0F, 21.0F}, 1U, true};
    double next_swing = session.now;
    session.run(25.0, [&] {
        session.update_position(101U, {0.5F, 1.0F, 20.5F});
        if (session.now >= next_swing && session.view(101U).alive)
        {
            session.authority->observe_enemy_hit(1U, 7U, 40.0F, {0.5F, 1.0F, 21.0F}, session.now);
            next_swing = session.now + 0.5;
        }
    });
    const Client &cheater = session.client(101U);
    expect(!session.view(101U).alive && cheater.deaths >= 1U && !cheater.alive,
           "a client that ignores its damage dies anyway: the host charged what it saw");
    expect(session.view(101U).violations >= 1U, "and ignoring it counts against it");
    expect(session.view(102U).violations == 0U && session.client(102U).health == 100.0F,
           "the other client is untouched");

    // There is no message that claims health; a negative or absurd report is refused.
    Session second{ProtocolVersion::classic_v3, survival_rules(), {}};
    settle(second, 101U, {0.5F, 1.0F, 20.5F}, 102U, {10.5F, 1.0F, 20.5F});
    expect(second.authority->report_harm(1U, {HarmCause::enemy, -100.0F, std::nullopt, std::nullopt},
                                         second.now) == AuthorityVerdict::refuse &&
               second.authority->report_harm(1U, {HarmCause::lava, 25.0F, std::nullopt, std::nullopt},
                                             second.now) == AuthorityVerdict::refuse &&
               second.view(101U).health == 100.0F,
           "a client cannot heal itself through a report, nor report what the host decides");

    // Kept playing after death: its edits and swings are refused, the rest see it dead.
    second.authority->harm(1U, 500.0F, HarmCause::mod, {0.0F, 1.0F, 0.0F}, second.now);
    second.run(5.0, [&] { second.update_position(101U, {0.5F, 1.0F, 20.5F}); });
    second.alter_block(101U, {0, 0, 21}, world::BlockType::empty);
    second.carry(101U, InventoryItemId::stick);
    second.melee(101U, 102U, InventoryItemId::stick);
    second.run(1.0, [&] { second.update_position(101U, {0.5F, 1.0F, 20.5F}); });
    expect(!second.view(101U).alive && second.view(102U).health == 100.0F &&
               std::count(second.refused.begin(), second.refused.end(), "AlterBlockMessage") == 1 &&
               std::count(second.refused.begin(), second.refused.end(), "MeleePlayerMessage") == 1,
           "a dead player's edit and swing are refused and reach nobody");
    expect(second.view(101U).violations >= 1U, "playing on after death is a violation");
}

void inflated_and_impossible_hits_are_refused()
{
    Session session{ProtocolVersion::modern_v4, survival_rules(), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {2.0F, 1.0F, 0.5F});
    session.carry(101U, InventoryItemId::stick);
    session.run(0.2);
    // A laser sword (1.1 = instant death) while holding a stick.
    session.melee(101U, 102U, InventoryItemId::iron_laser_sword);
    session.run(0.3);
    expect(session.view(102U).health == 100.0F &&
               std::count(session.client(102U).received.begin(), session.client(102U).received.end(),
                          "MeleePlayerMessage") == 0,
           "a swing with a weapon the attacker does not hold does nothing and is not relayed");
    // A gun is not a melee weapon.
    session.melee(101U, 102U, InventoryItemId::pistol);
    session.run(0.3);
    expect(session.view(102U).health == 100.0F, "a gun is not a melee weapon");
    // The honest swing: 0.21 of health, applied by the host, sent to the victim.
    session.melee(101U, 102U, InventoryItemId::stick);
    session.run(0.3);
    expect(std::abs(session.view(102U).health - 79.0F) < 0.01F &&
               std::abs(session.client(102U).health - 79.0F) < 0.01F,
           "an honest swing costs the victim 21 health, decided by the host");
    expect(session.view(101U).violations == 2U, "both impossible swings counted");
}

void walls_and_reach_stop_hits()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {2.5F, 1.0F, 0.5F});
    session.carry(101U, InventoryItemId::stick);
    session.world.wall(1, 1, 3, -2, 2);
    session.melee(101U, 102U, InventoryItemId::stick);
    session.run(0.3);
    expect(session.view(102U).health == 100.0F, "a swing through a wall is refused");
    session.world.cells.clear();
    // Client 2 walks off to twenty metres.
    const auto walk = [&](PeerId peer, float to_x, double seconds) {
        session.run(seconds, [&] {
            world::Vec3 at = session.client(peer).position;
            at.x += std::clamp(to_x - at.x, -8.0F / 60.0F, 8.0F / 60.0F);
            session.update_position(peer, at);
        });
    };
    walk(102U, 20.5F, 3.0);
    session.melee(101U, 102U, InventoryItemId::stick);
    session.run(0.3);
    expect(session.view(102U).health == 100.0F, "a swing at a player twenty metres away is refused");
    expect(session.view(101U).violations == 2U, "both were counted");
    // A gunshot through a wall: the host's own trace stops at the rock.
    walk(102U, 6.5F, 2.5);
    session.carry(101U, InventoryItemId::pistol);
    session.world.wall(3, 1, 4, -3, 3);
    session.run(0.2);
    session.gunshot(101U, InventoryItemId::pistol, {1.0F, 0.0F, 0.0F});
    session.run(0.3);
    expect(session.view(102U).health == 100.0F, "a shot into a wall hits nobody behind it");
    session.world.cells.clear();
    session.gunshot(101U, InventoryItemId::pistol, {1.0F, 0.0F, 0.0F});
    session.run(0.3);
    expect(std::abs(session.view(102U).health - 79.0F) < 0.01F,
           "the same shot in the open costs the victim 21");
}

void rapid_fire_is_refused()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(PlayerVersusPlayer::off), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {30.5F, 1.0F, 0.5F});
    session.carry(101U, InventoryItemId::pistol);
    session.run(0.2);
    const float cooldown = gun_stats(InventoryItemId::pistol)->fire_cooldown_seconds;
    // Five shots per cooldown for two seconds.
    std::size_t sent{};
    double next_shot = session.now;
    session.run(2.0, [&] {
        while (session.now >= next_shot)
        {
            next_shot += cooldown / 5.0F;
            session.gunshot(101U, InventoryItemId::pistol, {0.0F, 0.0F, 1.0F});
            ++sent;
        }
    });
    session.run(0.3);
    const auto refused = std::count(session.refused.begin(), session.refused.end(), "GunshotMessage");
    const auto relayed = std::count(session.client(102U).received.begin(),
                                    session.client(102U).received.end(), "GunshotMessage");
    expect(refused > 0 && static_cast<std::size_t>(relayed) + static_cast<std::size_t>(refused) == sent,
           "shots beyond the gun's rate are refused");
    expect(static_cast<double>(relayed) <= 2.0 / cooldown * 1.1 + 5.0,
           std::format("no more than the gun's own rate reaches the others ({} of {})", relayed, sent));
    expect(session.view(101U).violations > 0U, "rapid fire counts");
}

void items_from_nothing_are_refused()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {10.5F, 1.0F, 0.5F});
    const std::vector<InventoryStack> start{{InventoryItemId::rock_block, 10U},
                                            {InventoryItemId::stone_pickaxe, 1U}};
    session.store_inventory(101U, start);
    session.run(0.3);
    session.create_pickup(101U, InventoryItemId::diamond, 64U, {0.5F, 1.0F, 0.5F});
    session.run(0.3);
    expect(std::count(session.refused.begin(), session.refused.end(), "CreatePickupMessage") == 1 &&
               std::count(session.client(102U).received.begin(), session.client(102U).received.end(),
                          "CreatePickupMessage") == 0,
           "sixty-four diamonds from nothing are refused and nobody sees them");
    // Dropping what it carries is fine; so is what a dug block leaves.
    session.create_pickup(101U, InventoryItemId::rock_block, 5U, {0.5F, 1.0F, 0.5F});
    session.carry(101U, InventoryItemId::stone_pickaxe);
    session.run(0.5);
    session.alter_block(101U, {1, 0, 1}, world::BlockType::empty);
    session.run(0.2);
    session.create_pickup(101U, mined_item(world::BlockType::rock), 1U, {1.5F, 0.5F, 1.5F});
    session.run(0.3);
    expect(std::count(session.client(102U).received.begin(), session.client(102U).received.end(),
                      "CreatePickupMessage") == 2,
           "a drop from the inventory and a dug block's pickup are relayed");
    // An inventory that grows a non-craftable item the host never gave.
    const std::vector<InventoryStack> grown{{InventoryItemId::rock_block, 5U},
                                            {InventoryItemId::stone_pickaxe, 1U},
                                            {InventoryItemId::diamond, 50U}};
    session.store_inventory(101U, grown);
    session.run(0.3);
    session.create_pickup(101U, InventoryItemId::diamond, 50U, {0.5F, 1.0F, 0.5F});
    session.run(0.3);
    expect(std::count(session.refused.begin(), session.refused.end(), "CreatePickupMessage") == 2,
           "diamonds it wrote into its own inventory still cannot leave it");
    expect(std::count(session.refused.begin(), session.refused.end(), "InventoryStoreOnServerMessage") == 1,
           "and that inventory is not stored as the player's profile");
    expect(session.view(101U).violations == 3U, "each attempt counted");
    // What the host's own pickup rule handed over (a pickup the host made)
    // is the player's to drop again.
    session.authority->pickup_given(1U, 0U, 900U, {InventoryItemId::iron_ore, 3U}, session.now);
    session.create_pickup(101U, InventoryItemId::iron_ore, 3U, {0.5F, 1.0F, 0.5F});
    session.run(0.3);
    expect(std::count(session.client(102U).received.begin(), session.client(102U).received.end(),
                      "CreatePickupMessage") == 3 &&
               session.view(101U).violations == 3U,
           "an item the host gave is believed");
}

void a_pickup_has_one_winner()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {1.5F, 1.0F, 0.5F});
    session.authority->register_pickup(0U, 55U, {InventoryItemId::diamond, 1U}, {1.0F, 1.0F, 0.5F});
    session.request_pickup(101U, 0U, 55U);
    session.request_pickup(102U, 0U, 55U);
    session.request_pickup(101U, 0U, 55U);
    session.run(0.5);
    expect(session.pickups_granted == 1U, "one pickup, three requests, one grant");
    expect(session.client(101U).consumed + session.client(102U).consumed == 1U,
           "exactly one player gets the item");
    // Out of reach.
    session.authority->register_pickup(0U, 56U, {InventoryItemId::diamond, 1U}, {60.0F, 1.0F, 0.5F});
    session.request_pickup(101U, 0U, 56U);
    session.run(0.3);
    expect(session.pickups_granted == 1U, "a pickup sixty metres away cannot be asked for");
}

void reach_rate_tools_and_spawn_protection()
{
    // Anti-cheat OFF here: the refusals pile up and STRICT would remove the
    // player before the last rule is tried.
    AuthorityRules rules = survival_rules(PlayerVersusPlayer::everyone, false);
    rules.spawn_protection_radius = 8;
    Session session{ProtocolVersion::classic_v3, rules, {}};
    settle(session, 101U, {30.5F, 1.0F, 30.5F}, 102U, {2.5F, 1.0F, 2.5F});
    session.carry(101U, InventoryItemId::bare_hands);
    session.run(0.2);
    session.alter_block(101U, {30, 0, 60}, world::BlockType::empty);
    session.run(0.2);
    expect(std::count(session.refused.begin(), session.refused.end(), "AlterBlockMessage") == 1,
           "a block thirty metres away cannot be changed");
    // Bare hands cannot dig rock in 1.6.3.8.
    session.alter_block(101U, {30, 0, 31}, world::BlockType::empty);
    session.run(0.2);
    expect(std::count(session.refused.begin(), session.refused.end(), "AlterBlockMessage") ==
               (std::isfinite(dig_time_seconds(InventoryItemId::bare_hands, world::BlockType::rock,
                                                InventoryProfile::classic))
                    ? 1
                    : 2),
           "a tool that cannot dig a block cannot break it");
    session.carry(101U, InventoryItemId::stone_pickaxe);
    session.run(0.5);
    session.alter_block(101U, {31, 0, 30}, world::BlockType::empty);
    session.run(0.2);
    const auto before = std::count(session.refused.begin(), session.refused.end(), "AlterBlockMessage");
    // Twenty digs at once.
    for (int index = 0; index < 20; ++index)
        session.alter_block(101U, {29, 0, 28 + index % 5}, world::BlockType::empty);
    session.run(0.3);
    expect(std::count(session.refused.begin(), session.refused.end(), "AlterBlockMessage") > before + 10,
           "digging faster than the pick allows is refused");
    // Placing without the block in hand, and inside spawn protection.
    session.alter_block(101U, {31, 1, 31}, world::BlockType::rock);
    session.run(0.2);
    const auto after = std::count(session.refused.begin(), session.refused.end(), "AlterBlockMessage");
    expect(after > before + 10, "placing rock while holding a pick is refused");
    // Client 2 stands at the spawn point with a pick.
    session.carry(102U, InventoryItemId::stone_pickaxe);
    session.run(0.3);
    session.alter_block(102U, {3, 0, 3}, world::BlockType::empty);
    session.run(0.2);
    expect(std::count(session.refused.begin(), session.refused.end(), "AlterBlockMessage") == after + 1 &&
               session.view(102U).violations == 0U,
           "spawn protection refuses the edit without calling it a cheat");
    expect(session.world.block(3, 0, 3) == world::BlockType::rock, "and the spawn keeps its rock");
}

void speed_teleport_and_flying_are_corrected()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {10.5F, 1.0F, 0.5F});
    // Teleport fifty metres. The host holds the jump for its notice a moment,
    // then puts the player back.
    session.update_position(101U, {50.5F, 1.0F, 0.5F});
    session.run(2.0, [&] {
        session.update_position(101U, session.client(101U).corrections == 0U
                                          ? world::Vec3{50.5F, 1.0F, 0.5F}
                                          : session.client(101U).position);
    });
    expect(session.client(101U).corrections == 1U &&
               std::abs(session.client(101U).position.x - 0.5F) < 0.01F,
           "an unannounced teleport is put back where the host last accepted it");
    const auto relayed_before = std::count(session.client(102U).received.begin(),
                                           session.client(102U).received.end(), "PlayerUpdateMessage");
    // The client keeps sending from where it was until the correction lands;
    // those are dropped, not counted twice.
    session.run(0.5, [&] { session.update_position(101U, session.client(101U).position); });
    expect(session.view(101U).violations == 1U, "the correction's round trip is not a second offence");
    // Speed: 25 m/s for two seconds.
    float x = session.client(101U).position.x;
    session.run(2.0, [&] {
        x += 25.0F / 60.0F;
        session.update_position(101U, {x, 1.0F, 0.5F});
    });
    expect(session.client(101U).corrections >= 2U && session.view(101U).violations >= 2U,
           "running at 25 m/s is corrected");
    // An announced teleport to another player is fine.
    session.run(3.0, [&] { session.update_position(101U, session.client(101U).position); });
    const std::uint32_t corrections = session.client(101U).corrections;
    session.client(101U).authority->announce_teleport(TeleportKind::player, {10.5F, 1.0F, 0.5F});
    session.run(0.2, [&] { session.update_position(101U, session.client(101U).position); });
    session.run(0.3, [&] { session.update_position(101U, {10.5F, 1.0F, 1.0F}); });
    expect(session.client(101U).corrections == corrections,
           "a teleport announced to a player's side is accepted");
    static_cast<void>(relayed_before);

    // Flying in Survival: hovering four metres up for three seconds.
    const std::uint32_t before = session.client(101U).corrections;
    session.run(3.5, [&] { session.update_position(101U, {10.5F, 5.0F, 1.0F}); });
    expect(session.client(101U).corrections > before, "hovering in Survival is corrected");
    // The same in Creative is allowed.
    AuthorityRules creative = survival_rules();
    creative.mode = GameMode::creative;
    Session free{ProtocolVersion::modern_v4, creative, {}};
    settle(free, 101U, {0.5F, 1.0F, 0.5F}, 102U, {10.5F, 1.0F, 0.5F});
    free.run(4.0, [&] { free.update_position(101U, {0.5F, 6.0F, 0.5F}); });
    expect(free.client(101U).corrections == 0U && free.view(101U).violations == 0U,
           "Creative may fly");
    // Through walls: standing inside rock.
    const std::uint32_t inside = session.client(101U).corrections;
    session.run(3.0, [&] { session.update_position(101U, session.client(101U).position); });
    session.run(1.5, [&] { session.update_position(101U, {session.client(101U).position.x, -3.0F, 1.0F}); });
    expect(session.client(101U).corrections > inside, "walking inside solid rock is corrected");
}

// The notice rides the mod channel and the move the stock one: the move may
// arrive first. The host waits for the notice instead of correcting.
void a_move_may_overtake_its_teleport_notice()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {150.0, 40.0}, 11U};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {10.5F, 1.0F, 0.5F});
    float x = 0.5F;
    session.run(5.0, [&] {
        x = std::min(40.5F, x + 8.0F / 60.0F);
        session.update_position(101U, {x, 1.0F, 0.5F});
    });
    // Teleport > Start: the move goes now, the notice half a second later.
    session.run(0.5, [&] { session.update_position(101U, {0.5F, 1.0F, 1.5F}); });
    session.client(101U).authority->announce_teleport(TeleportKind::start, {0.5F, 1.0F, 1.5F});
    session.run(2.5, [&] { session.update_position(101U, {0.5F, 1.0F, 1.5F}); });
    // Teleport > Surface goes straight up thirty metres, its notice second too.
    session.world.cells[{0, 29, 1}] = world::BlockType::rock;
    session.run(0.5, [&] { session.update_position(101U, {0.5F, 30.0F, 1.5F}); });
    session.client(101U).authority->announce_teleport(TeleportKind::surface, {0.5F, 1.0F, 1.5F});
    session.run(2.5, [&] { session.update_position(101U, {0.5F, 30.0F, 1.5F}); });
    expect(session.view(101U).position.has_value() && session.view(101U).position->y > 29.0F,
           "a teleport to the surface is accepted");
    // The same leap up with no notice at all is put back once the wait is over.
    session.world.cells[{10, 59, 0}] = world::BlockType::rock;
    session.run(0.5, [&] { session.update_position(102U, {10.5F, 60.0F, 0.5F}); });
    session.run(2.0, [&] {
        session.update_position(102U, session.client(102U).corrections == 0U ? world::Vec3{10.5F, 60.0F, 0.5F}
                                                                              : session.client(102U).position);
    });
    expect(session.client(102U).corrections == 1U && session.view(102U).violations == 1U &&
               session.client(102U).position.y < 2.0F,
           std::format("an unannounced leap straight up is put back ({} corrections, {} violations, y {:.1f})",
                       session.client(102U).corrections, session.view(102U).violations,
                       session.client(102U).position.y));
    expect(session.client(101U).corrections == 0U && session.view(101U).violations == 0U &&
               session.view(101U).position.has_value() && session.view(101U).position->x < 1.0F,
           std::format("a teleport whose notice arrives after the move is accepted ({} corrections, x {:.1f})",
                       session.client(101U).corrections,
                       session.view(101U).position.value_or(world::Vec3{}).x));
}

void strict_kicks_and_off_only_corrects()
{
    for (const bool strict : {true, false})
    {
        Session session{ProtocolVersion::classic_v3, survival_rules(PlayerVersusPlayer::everyone, strict), {}};
        settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {10.5F, 1.0F, 0.5F});
        // A speed hack at 30 m/s that takes each correction and runs again.
        session.run(40.0, [&] {
            const world::Vec3 at = session.client(101U).position;
            session.update_position(101U, {std::fmod(at.x + 0.5F, 200.0F), 1.0F, 0.5F});
        });
        const bool kicked = session.count(AuthorityEventKind::kick, std::uint8_t{1U}) == 1U;
        expect(strict ? kicked : !kicked,
               strict ? "STRICT kicks a player that keeps cheating, once"
                      : "with anti-cheat OFF nobody is kicked");
        expect(session.client(101U).corrections > 0U, "corrections happen either way");
        if (strict)
            expect(session.client(102U).notices == 1U && session.count(AuthorityEventKind::kick, std::uint8_t{2U}) == 0U,
                   "the kick's reason reaches everyone; the honest player is never kicked");
    }
}

void explosions_fireballs_lava_and_falls_are_the_hosts()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(PlayerVersusPlayer::everyone), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {4.5F, 1.0F, 0.5F});
    // A grenade 2 m from client 2: inside the kill radius.
    session.authority->apply_explosion({6.0F, 1.5F, 0.5F}, world::he_grenade_damage_bands, std::uint8_t{1U}, session.now);
    session.run(0.3);
    expect(!session.view(102U).alive && session.client(102U).deaths == 1U,
           "a grenade at a player's feet kills it, the host's decision");
    expect(session.view(101U).health == 100.0F, "the thrower four metres away is outside the blast");
    // A fireball behind a wall does nothing.
    session.world.wall(2, 1, 3, -3, 3);
    session.authority->apply_fireball({3.5F, 2.0F, 0.5F}, world::DragonKind::fire, session.now);
    session.run(0.2);
    expect(session.view(101U).health == 100.0F, "a fireball behind rock does no damage");
    session.world.cells.clear();
    session.authority->apply_fireball({0.5F, 2.5F, 0.5F}, world::DragonKind::fire, session.now);
    session.run(0.2);
    expect(std::abs(session.view(101U).health - 60.0F) < 0.5F, "an open fireball hit costs 40");
    // Lava: below y -60, 25 every half second, decided by the host.
    session.run(4.0, [&] { session.update_position(101U, {0.5F, 1.0F, 0.5F}); });
    const float healthy = session.view(101U).health;
    expect(healthy == 100.0F, "health came back after three quiet seconds");
    session.world.cells[{0, -62, 0}] = world::BlockType::empty;
    session.world.cells[{0, -61, 0}] = world::BlockType::empty;
    session.client(101U).position = {0.5F, -62.0F, 0.5F};
    session.authority->allow_teleport(1U, std::nullopt, session.now);
    session.run(1.2, [&] { session.update_position(101U, {0.5F, -62.0F, 0.5F}); });
    expect(session.view(101U).health <= 50.0F && session.view(101U).health >= 25.0F,
           std::format("lava burns 25 every half second ({:.0f} left)", session.view(101U).health));
}

void falls_must_be_reported()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {}};
    // A tower: rock up to y 20; both start on top of their own.
    for (int y = 1; y < 20; ++y)
    {
        session.world.cells[{0, y, 0}] = world::BlockType::rock;
        session.world.cells[{10, y, 0}] = world::BlockType::rock;
    }
    settle(session, 101U, {0.5F, 20.0F, 0.5F}, 102U, {10.5F, 20.0F, 0.5F});
    // Both step off and fall nineteen metres; one reports it, one does not.
    const auto fall = [&](PeerId peer, float x0) {
        float y = 20.0F;
        float vy = 0.0F;
        float x = x0;
        while (y > 1.0F)
        {
            vy -= 20.0F / 60.0F;
            y = std::max(1.0F, y + vy / 60.0F);
            x += 2.0F / 60.0F;
            session.update_position(peer, {x, y, 0.5F});
            session.step();
        }
        return std::sqrt(2.0F * 20.0F * 19.0F);
    };
    const float impact = fall(101U, 0.5F);
    const float damage = (impact - 15.0F) * 100.0F / 15.0F;
    session.client(101U).authority->report_harm({HarmCause::fall, damage, std::nullopt, std::nullopt});
    static_cast<void>(fall(102U, 10.5F));
    session.run(4.0, [&] {
        session.update_position(101U, session.client(101U).position);
        session.update_position(102U, session.client(102U).position);
    });
    expect(session.view(101U).violations == 0U, "an honest fall report is accepted");
    expect(session.view(102U).violations == 1U && session.view(102U).health < 100.0F - 30.0F,
           "an unreported fall is charged by the host's own estimate");
}

void respawn_is_the_hosts_to_grant()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {10.5F, 1.0F, 0.5F});
    session.authority->harm(1U, 200.0F, HarmCause::mod, {0.5F, 1.0F, 0.5F}, session.now);
    session.run(0.3);
    session.client(101U).authority->request_respawn();
    session.run(0.3);
    expect(!session.view(101U).alive && session.client(101U).respawns == 0U,
           "asking at once is refused");
    session.run(3.0);
    session.client(101U).authority->request_respawn();
    session.run(0.3);
    expect(session.view(101U).alive && session.client(101U).respawns == 1U &&
               session.client(101U).health == 100.0F,
           "after the prompt's three seconds the host brings it back at full health");
    // The respawn's jump to the start is accepted.
    session.run(0.3, [&] { session.update_position(101U, {0.5F, 1.0F, 60.5F}); });
    expect(session.client(101U).corrections == 0U, "the respawn teleport is not a cheat");
}

void a_mod_decides_every_players_damage()
{
    Session session{ProtocolVersion::classic_v3, survival_rules(), {}};
    settle(session, 101U, {0.5F, 1.0F, 0.5F}, 102U, {1.5F, 1.0F, 0.5F});
    std::vector<std::uint8_t> asked;
    session.authority->set_damage_filter([&](std::uint8_t gid, float amount, HarmCause cause, world::Vec3) {
        asked.push_back(gid);
        return cause == HarmCause::player_melee ? 0.0F : amount * 0.5F; // no PVP, half the rest
    });
    session.carry(101U, InventoryItemId::stick);
    session.run(0.2);
    session.melee(101U, 102U, InventoryItemId::stick);
    session.run(0.3);
    session.client(102U).authority->report_harm({HarmCause::enemy, 40.0F, std::nullopt, std::nullopt});
    session.run(0.5);
    expect(asked.size() == 2U && asked[0] == 2U && std::abs(session.view(102U).health - 80.0F) < 0.01F,
           "player.damaged can cancel or change a remote player's damage");
}

// ------------------------------------------------------------- honest play

// Two honest clients, a minute at 200 ms with 50 ms of jitter: walking,
// jumping, digging, building, picking up, shooting each other with PVP on,
// taking enemy hits they report and the host sees, a fall, a death and a
// respawn. Not one correction, violation or kick.
void honest_play(LinkProfile profile, std::uint32_t seed)
{
    Session session{ProtocolVersion::classic_v3, survival_rules(PlayerVersusPlayer::everyone),
                    profile, seed};
    for (int y = 1; y < 12; ++y)
        session.world.cells[{40, y, 40}] = world::BlockType::rock;
    settle(session, 101U, {12.5F, 1.0F, 0.5F}, 102U, {8.5F, 1.0F, 0.5F});
    session.store_inventory(101U, std::vector<InventoryStack>{{InventoryItemId::rock_block, 64U},
                                                               {InventoryItemId::stone_pickaxe, 1U},
                                                               {InventoryItemId::pistol, 1U}});
    session.store_inventory(102U, std::vector<InventoryStack>{{InventoryItemId::rock_block, 64U},
                                                               {InventoryItemId::stone_pickaxe, 1U}});
    session.carry(101U, InventoryItemId::stone_pickaxe);
    session.carry(102U, InventoryItemId::rock_block);
    session.run(1.0, [&] {
        session.update_position(101U, {12.5F, 1.0F, 0.5F});
        session.update_position(102U, {8.5F, 1.0F, 0.5F});
    });

    const GunStats &pistol = *gun_stats(InventoryItemId::pistol);
    double angle{};
    double jump_at = session.now + 1.0;
    double jump_started = -1.0;
    double dig_at = session.now + 0.5;
    double place_at = session.now + 0.7;
    double shot_at{};
    int shots{};
    int dug{};
    const double start = session.now;
    double next_update = session.now;
    double fall_start = -1.0;
    float fall_y = 12.0F;
    float fall_v = 0.0F;
    bool fell{};
    bool reported_fall{};
    float lowest_health_two = 100.0F;
    std::vector<double> pending_sightings;
    double death_seen = -1.0;
    session.run(60.0, [&] {
        const double t = session.now - start;
        // Client 1 runs a circle of radius 12 at 4 m/s, jumping every three seconds.
        angle += 4.0 / 60.0 / 12.0;
        float y1 = 1.0F;
        if (session.now >= jump_at)
        {
            jump_started = session.now;
            jump_at = session.now + 3.0;
        }
        if (jump_started >= 0.0 && session.now - jump_started < 1.0)
        {
            const float s = static_cast<float>(session.now - jump_started);
            y1 = 1.0F + 10.0F * s - 10.0F * s * s;
        }
        const world::Vec3 p1{static_cast<float>(12.0 * std::cos(angle)) + 0.5F, std::max(1.0F, y1),
                             static_cast<float>(12.0 * std::sin(angle)) + 0.5F};
        // Client 2 walks back and forth along a line; at 30 s a server mod
        // moves it onto a tower, from which it walks off and falls 11 m.
        const double leg = std::fmod(t, 10.0);
        world::Vec3 p2{8.5F + static_cast<float>(leg < 5.0 ? leg : 10.0 - leg), 1.0F, 0.5F};
        if (t >= 30.0 && !fell)
        {
            if (fall_start < 0.0)
            {
                fall_start = session.now;
                session.authority->allow_teleport(2U, world::Vec3{40.5F, 12.0F, 40.5F}, session.now);
            }
            if (session.now - fall_start < 1.0)
                p2 = {40.5F, 12.0F, 40.5F};
            else
            {
                fall_v -= 20.0F / 60.0F;
                fall_y = std::max(1.0F, fall_y + fall_v / 60.0F);
                p2 = {41.6F, fall_y, 40.5F};
                if (fall_y <= 1.0F)
                {
                    fell = true;
                    const float impact = std::sqrt(2.0F * 20.0F * 11.0F);
                    session.client(102U).authority->report_harm(
                        {HarmCause::fall, (impact - 15.0F) * 100.0F / 15.0F, std::nullopt, std::nullopt});
                    reported_fall = true;
                }
            }
        }
        else if (fell)
            p2 = {41.6F, 1.0F, 40.5F};
        if (session.now >= next_update)
        {
            next_update = session.now + 0.05;
            session.update_position(101U, p1);
            if (session.client(102U).alive)
                session.update_position(102U, p2);
            else
                session.update_position(102U, session.client(102U).position, PlayerUpdatePayload::dead_flag);
        }
        // Client 1 digs a rock cell inside its circle every 1.5 s and the
        // rock it dug drops at the cell.
        if (t < 20.0 && session.now >= dig_at)
        {
            dig_at = session.now + 1.5;
            const IntVector3 cell{static_cast<int>(std::floor(p1.x * 0.75F)), 0,
                                  static_cast<int>(std::floor(p1.z * 0.75F))};
            if (session.world.block(cell.x, cell.y, cell.z) == world::BlockType::rock)
            {
                session.alter_block(101U, cell, world::BlockType::empty);
                session.create_pickup(101U, mined_item(world::BlockType::rock), 1U,
                                      {static_cast<float>(cell.x) + 0.5F, 0.5F,
                                       static_cast<float>(cell.z) + 0.5F});
                ++dug;
            }
        }
        // Client 2 builds a block beside itself twice a second.
        if (t < 25.0 && session.now >= place_at)
        {
            place_at = session.now + 0.5;
            const IntVector3 cell{static_cast<int>(std::floor(p2.x)), 1, 2};
            if (session.world.block(cell.x, cell.y, cell.z) == world::BlockType::empty)
                session.alter_block(102U, cell, world::BlockType::rock);
        }
        // From 20 s client 1 shoots client 2 four times at the pistol's rate.
        if (t >= 20.0 && shots == 0 && shot_at == 0.0)
        {
            session.carry(101U, InventoryItemId::pistol);
            shot_at = session.now + 0.4;
        }
        if (shots < 4 && shot_at > 0.0 && session.now >= shot_at)
        {
            shot_at = session.now + pistol.fire_cooldown_seconds * 1.02;
            const world::Vec3 to{p2.x - p1.x, p2.y + 0.8F - p1.y - 1.4F, p2.z - p1.z};
            const float length = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
            session.gunshot(101U, InventoryItemId::pistol, {to.x / length, to.y / length, to.z / length});
            ++shots;
        }
        lowest_health_two = std::min(lowest_health_two, session.client(102U).health);
        // 40-50 s: a zombie swings at client 2 every 0.8 s. The client
        // reports what its own copy landed; the host's copy sees the same
        // swing a moment later. Four of them kill it.
        if (t >= 40.0 && t < 50.0 && session.client(102U).alive && session.client(102U).deaths == 0U &&
            std::fmod(t - 40.0, 0.8) < 1.0 / 60.0)
        {
            session.world.enemies[{std::uint8_t{2U}, 9U}] = {1, {p2.x + 0.8F, 1.0F, p2.z}, 2U, true};
            session.client(102U).authority->report_harm({HarmCause::enemy, 30.0F, 9U, std::nullopt});
            session.client(102U).pending_report_at = session.now;
            pending_sightings.push_back(session.now + 0.12);
        }
        while (!pending_sightings.empty() && pending_sightings.front() <= session.now)
        {
            pending_sightings.erase(pending_sightings.begin());
            session.authority->observe_enemy_hit(2U, 9U, 30.0F, {p2.x + 0.8F, 1.0F, p2.z}, session.now);
        }
        // Dead: ask to come back once the prompt is up.
        if (!session.client(102U).alive && death_seen < 0.0)
            death_seen = session.now;
        if (session.client(102U).alive)
            death_seen = -1.0;
        if (!session.client(102U).alive && death_seen >= 0.0 && session.now - death_seen > 3.5)
            session.client(102U).authority->request_respawn();
    });
    session.run(3.0);
    const AuthorityPlayerView one = session.view(101U);
    const AuthorityPlayerView two = session.view(102U);
    for (const AuthorityEvent &event : session.events)
        if (event.kind == AuthorityEventKind::violation || event.kind == AuthorityEventKind::correction)
            std::cout << "  unexpected " << (event.kind == AuthorityEventKind::violation ? "violation" : "correction")
                      << " gid=" << static_cast<int>(event.gid) << ' ' << event.reason << '\n';
    expect(one.violations == 0U && two.violations == 0U,
           std::format("honest players commit no violation ({} and {})", one.violations, two.violations));
    expect(session.client(101U).corrections == 0U && session.client(102U).corrections == 0U,
           "honest players are never corrected");
    expect(session.count(AuthorityEventKind::kick) == 0U, "and never kicked");
    expect(session.refused.empty(), std::format("nothing an honest player sends is refused ({})",
                                                session.refused.empty() ? "" : session.refused.front()));
    expect(dug > 5, std::format("client 1 dug and dropped its rock ({} cells)", dug));
    expect(shots == 4 && lowest_health_two < 100.0F,
           "client 1's shots reached client 2 through the host's lag-compensated trace");
    expect(session.client(102U).deaths == 1U && session.client(102U).respawns == 1U && two.alive,
           "client 2 died to the zombie and came back");
    expect(reported_fall, "client 2 fell and reported it");
    // What the players feel: a reported hit comes back as health one round
    // trip later.
    double worst{};
    double total{};
    for (const double latency : session.client(102U).health_latencies)
    {
        worst = std::max(worst, latency);
        total += latency;
    }
    const double mean = session.client(102U).health_latencies.empty()
                            ? 0.0
                            : total / static_cast<double>(session.client(102U).health_latencies.size());
    std::cout << std::format("  reported hit -> health on screen: mean {:.0f} ms, worst {:.0f} ms over {} hits\n",
                             mean * 1000.0, worst * 1000.0, session.client(102U).health_latencies.size());
    expect(mean > 0.35 && mean < 0.55 && worst < 0.65,
           "a reported hit shows on the health bar one round trip later (about 400 ms at 200 ms each way)");
}

void honest_clients_at_200ms_with_jitter_play_normally()
{
    honest_play({200.0, 50.0}, 1234U);
}

// The same minute with a 0.8 s freeze while both dig, build and run: what was
// held arrives at once, and nothing of it is mistaken for a cheat.
void honest_clients_ride_out_a_lag_spike()
{
    honest_play({200.0, 50.0, 16.0, 16.8}, 99U);
}

void run(std::string_view name, void (*section)())
{
    const int before = failures;
    try
    {
        section();
    }
    catch (const std::exception &error)
    {
        ++failures;
        std::cout << "[FAIL] " << name << " threw: " << error.what() << '\n';
    }
    std::cout << (failures == before ? "[PASS] " : "[FAIL] ") << name << '\n';
}

} // namespace

int main()
{
    run("openminerz_only_is_a_session_property_the_lobby_carries",
        openminerz_only_is_a_session_property_the_lobby_carries);
    run("sessions_that_are_not_authoritative_keep_their_bytes",
        sessions_that_are_not_authoritative_keep_their_bytes);
    run("god_mode_is_impossible", god_mode_is_impossible);
    run("inflated_and_impossible_hits_are_refused", inflated_and_impossible_hits_are_refused);
    run("walls_and_reach_stop_hits", walls_and_reach_stop_hits);
    run("rapid_fire_is_refused", rapid_fire_is_refused);
    run("items_from_nothing_are_refused", items_from_nothing_are_refused);
    run("a_pickup_has_one_winner", a_pickup_has_one_winner);
    run("reach_rate_tools_and_spawn_protection", reach_rate_tools_and_spawn_protection);
    run("speed_teleport_and_flying_are_corrected", speed_teleport_and_flying_are_corrected);
    run("a_move_may_overtake_its_teleport_notice", a_move_may_overtake_its_teleport_notice);
    run("strict_kicks_and_off_only_corrects", strict_kicks_and_off_only_corrects);
    run("explosions_fireballs_lava_and_falls_are_the_hosts",
        explosions_fireballs_lava_and_falls_are_the_hosts);
    run("falls_must_be_reported", falls_must_be_reported);
    run("respawn_is_the_hosts_to_grant", respawn_is_the_hosts_to_grant);
    run("a_mod_decides_every_players_damage", a_mod_decides_every_players_damage);
    run("honest_clients_at_200ms_with_jitter_play_normally",
        honest_clients_at_200ms_with_jitter_play_normally);
    run("honest_clients_ride_out_a_lag_spike", honest_clients_ride_out_a_lag_spike);
    std::cout << (failures == 0 ? "all authority tests passed\n" : "authority tests failed\n");
    return failures == 0 ? 0 : 1;
}
