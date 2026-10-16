// Randomized multiplayer world-desync soak.
//
// Everything that carries world state here is production code: the stock
// `LegacySteamHostSession`/`LegacySteamClientSession` relay, the host's
// authoritative world (`AuthoritativeBootstrapHost` and its mutation API, the
// same calls `handle_online_gameplay` makes), the late-join transcript
// (`AuthoritativeBootstrapClient`), the stock payload codecs and
// `assess_client_gameplay_authority`/`ClientGameplayRateLimiter`.
//
// The soak drives random mining, placing, door toggles, concurrent crate
// transfers, placed explosives, Modern custom blocks and spawners, chunk
// requests racing live edits and a late join in the middle of all of it,
// through a link that delays, jitters, reorders, duplicates and retransmits.
// After every simulated tick it digests each peer's world state with the
// shared `tests/state_digest.hpp` tool and, at every quiescent tick, requires
// every peer to equal the host authority exactly. A mismatch names the
// diverged cells/crates/doors/spawners at the tick they appear.
//
// `WorldReplica` below is the reference receipt semantics, transcribed from
// the shipped Classic assembly: BlockTerrain.SetBlock (which has no admin or
// occupied-cell rule; that was the OpenClassic mod's), Explosive.HandleRemoveBlocksMessage,
// Explosive.HandleDetonateExplosiveMessage, ItemCrateMessage.Apply and
// CastleMinerZGame._processDestroyCrateMessage.

#include "openminerz/net/client_gameplay_authority.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/legacy_steam_client_session.hpp"
#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/misc_codec.hpp"
#include "openminerz/net/payload_codec.hpp"
#include "state_digest.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace openminerz;
using namespace openminerz::net;
using namespace openminerz::test;

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

// ------------------------------------------------------------- the link model
//
// Steam's reliable channels never lose a datagram, so "loss" here is a
// retransmission that still preserves per-channel ordering. Reordering between
// the reliable-ordered world stream and the reliable-unordered chunk stream is
// real, and it is where a chunk snapshot overtakes the edit it predates.

struct LinkProfile final
{
    double delay_ms{};
    double jitter_ms{};
    // Fraction of reliable datagrams that cost one retransmission timeout.
    double retransmit{};
    // Fraction of unordered datagrams held back behind later traffic.
    double reorder{};
    // Fraction of unordered datagrams delivered twice.
    double duplicate{};
    double retransmit_ms{200.0};
    // Head-of-line delay on the reliable-ordered channel only. One lost
    // reliable-ordered datagram holds back everything queued behind it while
    // the reliable-unordered channel keeps flowing, which is what lets a chunk
    // reply overtake the edit that preceded it.
    double ordered_head_of_line_ms{};
};

class Link final
{
  public:
    Link(LinkProfile profile, std::uint32_t seed) : profile_{profile}, random_{seed}
    {
    }

    void push(double now_ms, MessageDeliveryPolicy delivery, LegacySteamNativeSend send)
    {
        const bool ordered = delivery == MessageDeliveryPolicy::reliable_ordered ||
                             delivery == MessageDeliveryPolicy::reliable_sequenced;
        double arrival = now_ms + profile_.delay_ms;
        if (profile_.jitter_ms > 0.0)
            arrival += (uniform() * 2.0 - 1.0) * profile_.jitter_ms;
        if (profile_.retransmit > 0.0 && uniform() < profile_.retransmit)
            arrival += profile_.retransmit_ms;
        arrival = std::max(arrival, now_ms);
        if (ordered)
        {
            // A reliable-ordered channel cannot deliver out of order, whatever
            // the wire did: the arrival time can only move forward.
            arrival += profile_.ordered_head_of_line_ms;
            arrival = std::max(arrival, last_ordered_arrival_);
            last_ordered_arrival_ = arrival;
            ordered_.push_back({arrival, ++sequence_, std::move(send)});
            return;
        }
        if (profile_.reorder > 0.0 && uniform() < profile_.reorder)
            arrival += profile_.delay_ms + profile_.retransmit_ms;
        const bool duplicated = profile_.duplicate > 0.0 && uniform() < profile_.duplicate;
        if (duplicated)
            unordered_.push_back({arrival + profile_.delay_ms, ++sequence_, send});
        unordered_.push_back({arrival, ++sequence_, std::move(send)});
    }

    [[nodiscard]] std::vector<LegacySteamNativeSend> drain(double now_ms)
    {
        std::vector<Entry> ready;
        while (!ordered_.empty() && ordered_.front().arrival_ms <= now_ms)
        {
            ready.push_back(std::move(ordered_.front()));
            ordered_.pop_front();
        }
        const auto split = std::stable_partition(
            unordered_.begin(), unordered_.end(),
            [now_ms](const Entry &entry) { return entry.arrival_ms <= now_ms; });
        for (auto entry = unordered_.begin(); entry != split; ++entry)
            ready.push_back(std::move(*entry));
        unordered_.erase(unordered_.begin(), split);
        std::ranges::stable_sort(ready, [](const Entry &left, const Entry &right) {
            return std::tie(left.arrival_ms, left.sequence) <
                   std::tie(right.arrival_ms, right.sequence);
        });
        std::vector<LegacySteamNativeSend> result;
        result.reserve(ready.size());
        for (Entry &entry : ready)
            result.push_back(std::move(entry.send));
        return result;
    }

    [[nodiscard]] bool idle() const noexcept
    {
        return ordered_.empty() && unordered_.empty();
    }

  private:
    struct Entry final
    {
        double arrival_ms{};
        std::uint64_t sequence{};
        LegacySteamNativeSend send;
    };

    [[nodiscard]] double uniform()
    {
        return std::uniform_real_distribution<double>{0.0, 1.0}(random_);
    }

    LinkProfile profile_;
    std::mt19937 random_;
    std::deque<Entry> ordered_;
    std::vector<Entry> unordered_;
    double last_ordered_arrival_{};
    std::uint64_t sequence_{};
};

// -------------------------------------------- reference receipt semantics
//
// The baseline slab gives every cell the soak touches a defined block, so the
// original's occupied-cell rule is well defined without a terrain generator.

using Cell = std::array<int, 3>;

struct WorldReplica final
{
    std::map<Cell, std::uint8_t> blocks;
    std::map<Cell, InventoryStoragePayload> crates;
    WorldSaveDoorMap doors;
    WorldSaveSpawnerMap spawners;
    std::map<Cell, std::uint8_t> armed_explosives;

    [[nodiscard]] std::uint8_t block_at(const Cell &cell) const
    {
        const auto found = blocks.find(cell);
        return found == blocks.end() ? std::uint8_t{0} : found->second;
    }

    // BlockTerrain.SetBlock(worldIndex, type). The shipped Classic 1.6.3.8 and
    // Modern 1.9.9.8 assemblies have no `admin` argument and no occupied-cell
    // rule: _processAlterBlocksMessage applies every edit from every sender.
    // The rule this harness used to model (a non-host may not replace one
    // block with another) exists only in the OpenClassic mod's source, so it
    // is now always permitted and the counters below must stay at zero.
    [[nodiscard]] static bool set_block_allowed(std::uint8_t current, std::uint8_t block,
                                                bool admin, ProtocolVersion protocol)
    {
        static_cast<void>(current);
        static_cast<void>(block);
        static_cast<void>(admin);
        static_cast<void>(protocol);
        return true;
    }

    [[nodiscard]] bool alter_block(const Cell &cell, std::uint8_t block, bool admin,
                                   ProtocolVersion protocol)
    {
        if (!set_block_allowed(block_at(cell), block, admin, protocol))
            return false;
        blocks.insert_or_assign(cell, block);
        if (block != 20U && block != 21U)
            armed_explosives.erase(cell);
        return true;
    }

    // Explosive.HandleRemoveBlocksMessage: every cell becomes Empty, which the
    // non-admin rule always permits.
    void remove_blocks(std::span<const IntVector3> cells)
    {
        for (const IntVector3 cell : cells)
        {
            blocks.insert_or_assign(Cell{cell.x, cell.y, cell.z}, std::uint8_t{0});
            armed_explosives.erase(Cell{cell.x, cell.y, cell.z});
        }
    }

    // ItemCrateMessage.Apply -> WorldInfo.GetCreate(location, true): the crate
    // is created when it is missing.
    void apply_crate_item(const ItemCratePayload &crate)
    {
        auto &slots = crates[Cell{crate.location.x, crate.location.y, crate.location.z}];
        if (crate.index >= 0 && crate.index < static_cast<std::int32_t>(slots.size()))
            slots[static_cast<std::size_t>(crate.index)] = crate.item;
    }

    void destroy_crate(const IntVector3 &location)
    {
        crates.erase(Cell{location.x, location.y, location.z});
    }

    void destroy_custom_block(const IntVector3 &location)
    {
        doors.erase(Cell{location.x, location.y, location.z});
    }

    void set_custom_door(const Cell &location, std::uint8_t model)
    {
        doors.try_emplace(location, model);
    }

    void update_spawner(const Cell &location, bool started)
    {
        if (!started)
            return;
        if (spawners.contains(location))
            return;
        std::size_t ordinal{};
        std::string spawn_id;
        do
        {
            spawn_id = "spawner" + std::to_string(ordinal++);
        } while (std::ranges::any_of(
            spawners, [&](const auto &entry) { return entry.second == spawn_id; }));
        spawners.emplace(location, std::move(spawn_id));
    }
};

[[nodiscard]] StateDigest digest_of(std::string label, const WorldReplica &world,
                                    const DigestRegion &region)
{
    StateDigest digest{std::move(label)};
    record_world_blocks(digest, world.blocks, region);
    for (const auto &[location, slots] : world.crates)
        if (region.contains(location))
            record_crate(digest, location, slots);
    record_doors(digest, world.doors, region);
    record_spawners(digest, world.spawners, region);
    return digest;
}

// -------------------------------------------------------------- the harness

constexpr PeerId host_peer = 900U;
constexpr PeerId alice_peer = 901U;
constexpr PeerId bob_peer = 902U;
constexpr PeerId carol_peer = 903U;

[[nodiscard]] LegacySteamHostSessionOptions host_options(ProtocolVersion protocol)
{
    LegacySteamHostSessionOptions options;
    options.settings.protocol = protocol;
    options.settings.mod_manifest.protocol = protocol;
    options.settings.visibility = HostVisibility::public_lobby;
    options.settings.maximum_players = 8;
    options.host_gamer = {{9U, 9U}, "Host"};
    return options;
}

[[nodiscard]] std::vector<std::uint8_t> frame(ProtocolVersion protocol, std::string_view name,
                                              std::span<const std::uint8_t> payload)
{
    const MessageDescriptor *descriptor = message_by_name(protocol, name);
    PacketError error{};
    if (descriptor == nullptr)
        fail("the stock registry is missing a soak message");
    auto packet = encode_packet(protocol, descriptor->id, payload, error);
    if (!packet.has_value())
        fail("the soak could not frame a stock packet");
    return std::move(*packet);
}

[[nodiscard]] MessageDeliveryPolicy delivery_of(ProtocolVersion protocol, std::string_view name)
{
    const MessageDescriptor *descriptor = message_by_name(protocol, name);
    if (descriptor == nullptr)
        fail("the stock registry is missing a soak message");
    return descriptor->transport.delivery;
}

struct Guest final
{
    Guest(ProtocolVersion protocol, PeerId peer, std::string name, SessionProperties properties,
          LinkProfile profile, std::uint32_t seed)
        : session{[&] {
              LegacySteamClientSessionOptions options;
              options.protocol = protocol;
              options.host_peer = host_peer;
              options.expected_host_name = "Host";
              options.session_properties = properties;
              options.local_gamer = {{static_cast<std::uint8_t>(peer), 1U}, std::move(name)};
              return options;
          }()},
          to_host{profile, seed}, to_guest{profile, seed ^ 0x5BD1U}
    {
    }

    LegacySteamClientSession session;
    Link to_host;
    Link to_guest;
    WorldReplica world;
    std::uint8_t gid{};
    std::size_t actor{};
    bool playing{};
    // Whether this peer completed the OpenMinerZ extension hello. A false here
    // is a stock/OpenClassic client: it applies BlockTerrain.SetBlock's
    // non-admin rule for itself and never defers to the host.
    bool capable{true};
    // `requested_chunk_reconciliations_` and the "an edit was observed after
    // this request" guard from `apply_authoritative_chunk`.
    std::map<Cell, bool> requested_chunks;
    // `chunk_reconcile_overlay_`: cells edited since this corner's check was
    // armed, which outrank a possibly older authoritative snapshot.
    std::map<Cell, std::set<Cell>> reconcile_overlay;
};

// Two peers mutating the same cell, door or crate slot at the same moment
// diverge in the original too: a stock peer applies its own change through its
// local loopback and every other peer applies it on arrival, and nothing
// defines a global order. That quirk is not what this soak hunts, so each
// actor owns a disjoint slice of the mutable world and only concurrency the
// protocol does define is exercised.
constexpr std::size_t soak_actors = 4U; // Alice, Bob, Carol, and the host.

[[nodiscard]] int actor_slab_x(std::size_t actor, std::size_t offset)
{
    return static_cast<int>(actor * 6U + offset % 6U);
}

[[nodiscard]] Cell actor_crate(std::size_t actor)
{
    return {8 + static_cast<int>(actor), 4, -8};
}

[[nodiscard]] Cell actor_scratch_crate(std::size_t actor)
{
    return {8 + static_cast<int>(actor), 4, -12};
}

[[nodiscard]] Cell actor_door(std::size_t actor)
{
    return {2 + static_cast<int>(actor) * 4, 4, -4};
}

struct SoakReport final
{
    std::size_t ticks{};
    std::size_t actions{};
    std::size_t relayed{};
    std::size_t host_rejections{};
    std::size_t rate_rejections{};
    std::size_t non_admin_ignored{};
    std::size_t confirmations{};
    std::size_t corrections{};
    std::size_t quiescent_checks{};
    std::size_t divergences{};
    std::string first_divergence;
    std::size_t first_divergence_tick{};
};

class Soak final
{
  public:
    Soak(ProtocolVersion protocol, LinkProfile profile, std::uint32_t seed,
         bool bob_is_stock = false)
        : protocol_{protocol}, profile_{profile}, transport_{host_options(protocol)},
          authority_{protocol, baseline_snapshot(protocol)}, random_{seed}
    {
        host_.blocks = canonical_block_edits(baseline_edits());
        for (const auto &[location, slots] : baseline_crates(protocol))
            host_.crates.insert_or_assign(location, slots);
        admit(alice_peer, "Alice", seed ^ 0x11U, 0U);
        admit(bob_peer, "Bob", seed ^ 0x22U, 1U, !bob_is_stock);
        // Both guests start from the production late-join transcript, so the
        // soak's starting point is the one real players get.
        for (auto &[peer, guest] : guests_)
            bootstrap(peer, guest);
    }

    // ---- the baseline world every peer starts from
    // Every cell the soak touches has an explicit baseline block, so the
    // original's occupied-cell rule is well defined without a generator.
    static std::vector<std::array<int, 4>> baseline_edits()
    {
        std::vector<std::array<int, 4>> edits;
        for (int x = 0; x < static_cast<int>(soak_actors) * 6; ++x)
            for (int z = -24; z < 0; ++z)
                edits.push_back({x, 3, z, ((x + z) % 2 == 0) ? 2 : 3});
        for (std::size_t actor{}; actor < soak_actors; ++actor)
        {
            const Cell door = actor_door(actor);
            edits.push_back({door[0], door[1], door[2], 27});     // LowerDoorClosedZ
            edits.push_back({door[0], door[1] + 1, door[2], 29}); // UpperDoorClosed
            const Cell crate = actor_crate(actor);
            edits.push_back({crate[0], crate[1], crate[2], 26});  // Crate
            const Cell scratch = actor_scratch_crate(actor);
            edits.push_back({scratch[0], scratch[1], scratch[2], 26});
        }
        return edits;
    }

    static std::map<Cell, InventoryStoragePayload> baseline_crates(ProtocolVersion protocol)
    {
        static_cast<void>(protocol);
        std::map<Cell, InventoryStoragePayload> crates;
        for (std::size_t actor{}; actor < soak_actors; ++actor)
        {
            InventoryStoragePayload slots;
            slots[0] = InventoryItemPayload{2, 5, 1.0F};
            slots[7] = InventoryItemPayload{3, 9, 1.0F};
            crates.emplace(actor_crate(actor), slots);
        }
        return crates;
    }

    static AuthoritativeBootstrapSnapshot baseline_snapshot(ProtocolVersion protocol)
    {
        WorldSaveData save;
        save.seed = 777;
        save.inventory.set_profile(protocol == ProtocolVersion::modern_v4 ? InventoryProfile::modern
                                                                         : InventoryProfile::classic);
        save.edits = baseline_edits();
        for (const auto &[location, slots] : baseline_crates(protocol))
        {
            WorldSaveCrateSlots converted;
            for (std::size_t index{}; index < slots.size(); ++index)
                if (!inventory_slot_to_stack(slots[index], protocol, converted[index]))
                    fail("the soak baseline crate is not stock-representable");
            save.crates.emplace(location, converted);
        }
        BootstrapWorldIdentity identity;
        identity.owner_gamer_tag = "Host";
        identity.creator_gamer_tag = "Host";
        AuthoritativeBootstrapSnapshot snapshot;
        BootstrapSnapshotError error{};
        if (!make_authoritative_bootstrap_snapshot(save, host_options(protocol).settings, identity,
                                                   true, snapshot, error))
            fail("the soak baseline snapshot could not be built");
        return snapshot;
    }

    void admit(PeerId peer, std::string name, std::uint32_t seed, std::size_t actor,
               bool capable = true)
    {
        auto &guest = guests_
                          .emplace(peer, Guest{protocol_, peer, std::move(name),
                                               transport_.session_properties(), profile_, seed})
                          .first->second;
        guest.actor = actor;
        guest.capable = capable;
        // The production capability path: an OpenMinerZ client's extension
        // hello is authorized before admission, a stock client sends none.
        if (capable && !transport_.authorize_extension(peer))
            fail("the soak could not authorize an OpenMinerZ extension hello");
        std::string error;
        const auto request = guest.session.begin(error);
        if (!request.has_value())
            fail("the soak could not build a stock admission request");
        const auto admission = transport_.process_wire_packet(peer, request->payload);
        if (admission.audit.has_value() || admission.inbound.size() != 1U)
            fail("the production host session refused a soak peer");
        for (const LegacySteamNativeSend &send : admission.sends)
            if (guests_.at(send.remote_peer)
                    .session.process_native_packet(host_peer, send.p2p_channel, send.payload)
                    .audit.has_value())
                fail("a soak peer refused its own admission roster");
        guest.gid = guest.session.local_gid().value_or(0U);
        if (guest.gid == 0U || !authority_.register_peer(peer, guest.gid))
            fail("the soak peer was not registered with the world authority");
    }

    // The production late-join transcript, run to completion with the links
    // drained, so a joiner starts exactly from the host authority.
    void bootstrap(PeerId peer, Guest &guest)
    {
        AuthoritativeBootstrapClientOptions options;
        options.protocol = protocol_;
        options.local_gid = guest.gid;
        options.gamer_tag = guest.session.local_gamer().gamer_tag;
        options.player_id = {2U, 7U};
        AuthoritativeBootstrapClient client{options};
        BootstrapClientError error{};
        const auto first = client.begin(error);
        if (!first.has_value())
            fail("the soak joiner could not begin the stock bootstrap");
        std::deque<AuthoritativeBootstrapClientSend> pending{*first};
        std::vector<std::vector<std::uint8_t>> deferred;
        std::size_t steps{};
        while (!pending.empty())
        {
            if (++steps > 8192U)
                fail("the soak bootstrap did not make bounded progress");
            const auto request = std::move(pending.front());
            pending.pop_front();
            std::string send_error;
            const auto wire = guest.session.create_outbound(
                request.recipient_gid, request.application_packet, request.delivery, send_error);
            if (!wire.has_value())
                fail("the soak joiner could not route a bootstrap request");
            const auto received = transport_.process_wire_packet(peer, wire->payload);
            if (received.inbound.size() != 1U)
                fail("the production host session dropped a bootstrap request");
            const auto handled = authority_.handle_packet(peer, received.inbound[0].payload,
                                                          received.inbound[0].channel);
            if (!handled.handled())
                fail("the world authority rejected a valid bootstrap request");
            for (const AuthoritativeBootstrapReply &reply : handled.replies)
            {
                LegacySteamOutbound outgoing;
                outgoing.recipient = reply.peer;
                outgoing.application_packet = reply.application_packet;
                outgoing.delivery = reply.delivery;
                if (reply.original_sender != 0U)
                {
                    outgoing.client_relay = true;
                    outgoing.channel = 1U;
                    outgoing.sender_gid = transport_.gid_for(reply.original_sender).value_or(0U);
                    outgoing.recipient_gid = transport_.gid_for(reply.peer).value_or(0U);
                }
                std::string error_text;
                const auto sends = transport_.create_outbound(outgoing, error_text);
                if (!sends.has_value())
                    fail("the production host session could not route a bootstrap reply");
                for (const LegacySteamNativeSend &send : *sends)
                {
                    const auto delivered =
                        guests_.at(send.remote_peer)
                            .session.process_native_packet(host_peer, send.p2p_channel,
                                                           send.payload);
                    if (delivered.audit.has_value())
                        fail("a bootstrap reply failed the production client transport");
                    if (send.remote_peer != peer)
                        continue;
                    for (const LegacySteamClientInbound &inbound : delivered.inbound)
                    {
                        if (inbound.kind != LegacySteamClientInboundKind::gameplay)
                            continue;
                        const auto accepted = client.handle_packet(inbound.gid, inbound.payload);
                        if (accepted.error == BootstrapClientError::unexpected_message)
                        {
                            deferred.push_back(inbound.payload);
                            continue;
                        }
                        if (!accepted.handled())
                            fail("the soak joiner rejected the host bootstrap transcript");
                        for (auto outbound : accepted.outbound)
                            pending.push_back(std::move(outbound));
                    }
                }
            }
        }
        if (!client.complete() || !client.world_save().has_value())
            fail("the soak bootstrap never completed");
        const WorldSaveData &save = *client.world_save();
        guest.world.blocks = canonical_block_edits(save.edits);
        guest.world.doors = save.doors;
        guest.world.spawners = save.spawners;
        guest.world.crates.clear();
        for (const auto &[location, slots] : save.crates)
        {
            InventoryStoragePayload converted;
            for (std::size_t index{}; index < slots.size(); ++index)
                if (!inventory_slot_to_payload(slots[index], protocol_, converted[index]))
                    converted[index].reset();
            guest.world.crates.insert_or_assign(location, converted);
        }
        // Live world traffic that arrived during loading is applied after the
        // installation, exactly as the bootstrap gameplay buffer replays it.
        for (const auto &packet : deferred)
            apply_on_guest(guest, 0U, packet);
        guest.playing = true;
    }

    // ---------------------------------------------------- the host authority
    //
    // The accept/record/relay order mirrors `handle_online_gameplay`: stock
    // authority, then the rate window, then the world mutation into the
    // authoritative snapshot, then the relay to every other peer.
    void handle_on_host(PeerId peer, const LegacySteamHostInbound &inbound)
    {
        DecodedPacket decoded;
        PacketError packet_error{};
        if (!decode_packet(protocol_, inbound.payload, decoded, packet_error) ||
            decoded.message == nullptr)
        {
            ++report_.host_rejections;
            return;
        }
        const ClientGameplayDecision decision =
            assess_client_gameplay_authority(protocol_, inbound);
        if (!decision.accepted())
        {
            ++report_.host_rejections;
            return;
        }
        if (!authority_.gameplay_ready(peer, inbound.sender_gid))
        {
            ++report_.host_rejections;
            return;
        }
        if (decoded.message->name == "RequestChunkMessage")
        {
            // The production world authority answers the reconciliation
            // request. Its reply is reliable-unordered, so it can overtake or
            // trail the reliable-ordered edit stream.
            const auto handled =
                authority_.handle_packet(peer, inbound.payload, inbound.channel);
            if (!handled.handled())
                return;
            for (const AuthoritativeBootstrapReply &reply : handled.replies)
                send_from_host(reply.peer, reply.application_packet, reply.delivery);
            return;
        }
        if (!limiter_.allow(peer, decision.message, static_cast<std::uint64_t>(now_ms_)))
        {
            ++report_.rate_rejections;
            correct_rejected_mutation(peer, decoded);
            return;
        }

        const std::string_view message = decoded.message->name;
        PayloadError payload_error{};
        if (message == "AlterBlockMessage")
        {
            AlterBlockPayload edit;
            if (!decode_alter_block_payload(decoded.payload, protocol_, edit, payload_error))
                return;
            const Cell cell{edit.block_location.x, edit.block_location.y, edit.block_location.z};
            const auto block = static_cast<std::uint8_t>(edit.block_type);
            const bool occupied_cell_replacement =
                !WorldReplica::set_block_allowed(host_.block_at(cell), block, false, protocol_);
            // Enforce BlockTerrain.SetBlock's non-admin rule only while a peer
            // whose OpenMinerZ capability is unconfirmed is in the session.
            if (occupied_cell_replacement && !transport_.all_peers_extension_capable())
            {
                ++report_.non_admin_ignored;
                correct_block(peer, edit.block_location);
                return;
            }
            if (!authority_.apply_block_edit(edit.block_location, block))
            {
                correct_block(peer, edit.block_location);
                return;
            }
            static_cast<void>(host_.alter_block(cell, block, true, protocol_));
            // The sender never predicts an occupied-cell replacement, so the
            // accepted block is confirmed back to it.
            if (occupied_cell_replacement)
            {
                correct_block(peer, edit.block_location);
                ++report_.confirmations;
            }
        }
        else if (message == "RemoveBlocksMessage")
        {
            RemoveBlocksPayload removal;
            if (!decode_remove_blocks_payload(decoded.payload, removal, payload_error) ||
                removal.blocks.empty())
                return;
            if (!authority_.apply_block_removals(removal.blocks))
            {
                correct_rejected_mutation(peer, decoded);
                return;
            }
            host_.remove_blocks(removal.blocks);
        }
        else if (message == "DetonateExplosiveMessage")
        {
            DetonateExplosivePayload detonation;
            if (!decode_detonate_explosive_payload(decoded.payload, protocol_, detonation,
                                                   payload_error))
                return;
            const Cell cell{detonation.location.x, detonation.location.y, detonation.location.z};
            if (host_.block_at(cell) != 20U && host_.block_at(cell) != 21U)
                return;
            if (!authority_.apply_block_edit(detonation.location, 0U))
                return;
            host_.remove_blocks(std::array{detonation.location});
        }
        else if (message == "ItemCrateMessage")
        {
            ItemCratePayload crate;
            if (!decode_item_crate_payload(decoded.payload, protocol_, crate, payload_error))
                return;
            if (!authority_.apply_crate_item(crate))
                return;
            host_.apply_crate_item(crate);
        }
        else if (message == "DestroyCrateMessage")
        {
            IntVector3 location;
            if (!decode_int_vector_payload(decoded.payload, location, payload_error))
                return;
            if (!authority_.destroy_crate(location))
                return;
            host_.destroy_crate(location);
        }
        else if (message == "DestroyCustomBlockMessage")
        {
            DestroyCustomBlockPayload custom;
            if (!decode_destroy_custom_block_payload(decoded.payload, custom, payload_error))
                return;
            if (!authority_.destroy_custom_block(custom.location))
                return;
            host_.destroy_custom_block(custom.location);
        }

        if (!decision.should_relay())
            return;
        LegacySteamOutbound relay;
        relay.application_packet = inbound.payload;
        relay.delivery = inbound.delivery;
        relay.channel = inbound.channel;
        relay.sender_gid = inbound.sender_gid;
        relay.recipient_gid = inbound.recipient_gid;
        relay.client_relay = true;
        std::string error;
        const auto sends = transport_.create_outbound(relay, error);
        if (!sends.has_value())
            fail("the production host session could not relay a soak broadcast");
        for (const LegacySteamNativeSend &send : *sends)
        {
            ++report_.relayed;
            guests_.at(send.remote_peer).to_guest.push(now_ms_, inbound.delivery, send);
        }
    }

    void correct_block(PeerId peer, IntVector3 location)
    {
        // `send_authoritative_block_correction`: the host asserts its own block
        // to the peer whose mutation it refused. A refused mutation is never
        // relayed, so no other peer saw it.
        PayloadError payload_error{};
        const Cell cell{location.x, location.y, location.z};
        const auto payload = encode_alter_block_payload(
            {location, static_cast<std::int32_t>(host_.block_at(cell))}, protocol_, payload_error);
        if (!payload.has_value())
            return;
        send_from_host(peer, frame(protocol_, "AlterBlockMessage", *payload),
                       delivery_of(protocol_, "AlterBlockMessage"));
        ++report_.corrections;
    }

    void correct_rejected_mutation(PeerId peer, const DecodedPacket &decoded)
    {
        PayloadError payload_error{};
        if (decoded.message->name == "AlterBlockMessage")
        {
            AlterBlockPayload edit;
            if (decode_alter_block_payload(decoded.payload, protocol_, edit, payload_error))
                correct_block(peer, edit.block_location);
            return;
        }
        if (decoded.message->name == "RemoveBlocksMessage")
        {
            RemoveBlocksPayload removal;
            if (!decode_remove_blocks_payload(decoded.payload, removal, payload_error))
                return;
            for (std::size_t index{}; index < std::min<std::size_t>(removal.blocks.size(), 256U);
                 ++index)
                correct_block(peer, removal.blocks[index]);
            return;
        }
        if (decoded.message->name == "ItemCrateMessage")
        {
            // A refused crate transfer leaves the sender holding a slot nobody
            // else has. The host replays its own authoritative slot.
            ItemCratePayload crate;
            if (!decode_item_crate_payload(decoded.payload, protocol_, crate, payload_error))
                return;
            const auto found = host_.crates.find(Cell{crate.location.x, crate.location.y,
                                                      crate.location.z});
            ItemCratePayload authoritative{crate.location, crate.index, std::nullopt};
            if (found != host_.crates.end() && crate.index >= 0 &&
                crate.index < static_cast<std::int32_t>(found->second.size()))
                authoritative.item = found->second[static_cast<std::size_t>(crate.index)];
            const auto payload = encode_item_crate_payload(authoritative, protocol_, payload_error);
            if (!payload.has_value())
                return;
            send_from_host(peer, frame(protocol_, "ItemCrateMessage", *payload),
                           delivery_of(protocol_, "ItemCrateMessage"));
            ++report_.corrections;
        }
    }

    void send_from_host(std::optional<PeerId> peer, std::vector<std::uint8_t> packet,
                        MessageDeliveryPolicy delivery)
    {
        LegacySteamOutbound outgoing;
        outgoing.recipient = peer;
        outgoing.application_packet = std::move(packet);
        outgoing.delivery = delivery;
        std::string error;
        const auto sends = transport_.create_outbound(outgoing, error);
        if (!sends.has_value())
            fail("the production host session could not route a host-origin packet");
        for (const LegacySteamNativeSend &send : *sends)
            guests_.at(send.remote_peer).to_guest.push(now_ms_, delivery, send);
    }

    // An edit seen after this chunk was requested makes the reply potentially
    // stale: the production client keeps the visible edit and asks again.
    static void note_guest_edit(Guest &guest, IntVector3 cell)
    {
        const IntVector3 corner = chunk_corner(cell);
        const Cell key{corner.x, corner.y, corner.z};
        const auto found = guest.requested_chunks.find(key);
        if (found != guest.requested_chunks.end())
            found->second = true;
        guest.reconcile_overlay[key].insert(Cell{cell.x, cell.y, cell.z});
    }

    // `apply_authoritative_chunk` -> `replace_chunk_edits`: the snapshot is the
    // whole truth for that chunk column, so every other edit in it is dropped.
    void install_authoritative_chunk(Guest &guest, const ProvideChunkPayload &chunk)
    {
        if (chunk.priority != 0U || chunk_corner(chunk.block_location) != chunk.block_location)
            return;
        const Cell corner{chunk.block_location.x, chunk.block_location.y, chunk.block_location.z};
        const auto requested = guest.requested_chunks.find(corner);
        if (requested == guest.requested_chunks.end())
            return; // Unrequested snapshots are refused.
        const bool stale = requested->second;
        guest.requested_chunks.erase(requested);
        if (stale)
            return;
        // Cells edited since the check was armed keep this peer's own block.
        std::map<Cell, std::uint8_t> retained;
        const auto overlay = guest.reconcile_overlay.find(corner);
        if (overlay != guest.reconcile_overlay.end())
        {
            for (const Cell &cell : overlay->second)
                retained.emplace(cell, guest.world.block_at(cell));
            guest.reconcile_overlay.erase(overlay);
        }
        std::erase_if(guest.world.blocks, [&](const auto &entry) {
            const IntVector3 cell{entry.first[0], entry.first[1], entry.first[2]};
            const IntVector3 other = chunk_corner(cell);
            return other.x == corner[0] && other.z == corner[2];
        });
        for (const auto &[cell, block] : canonical_chunk_deltas({{chunk.block_location,
                                                                 chunk.delta}}))
            guest.world.blocks.insert_or_assign(cell, block);
        for (const auto &[cell, block] : retained)
            guest.world.blocks.insert_or_assign(cell, block);
    }

    // --------------------------------------------------- the guest receipt
    void apply_on_guest(Guest &guest, std::uint8_t sender_gid,
                        std::span<const std::uint8_t> packet)
    {
        DecodedPacket decoded;
        PacketError packet_error{};
        if (!decode_packet(protocol_, packet, decoded, packet_error) || decoded.message == nullptr)
            return;
        const std::string_view message = decoded.message->name;
        const bool admin = sender_gid == 0U;
        PayloadError payload_error{};
        if (message == "AlterBlockMessage")
        {
            AlterBlockPayload edit;
            if (!decode_alter_block_payload(decoded.payload, protocol_, edit, payload_error))
                return;
            const Cell cell{edit.block_location.x, edit.block_location.y, edit.block_location.z};
            const auto block = static_cast<std::uint8_t>(edit.block_type);
            const bool own_loopback = sender_gid == guest.gid;
            // A capable peer trusts the OpenMinerZ host's arbitration for every
            // relayed edit and defers its own occupied-cell replacement to the
            // host's confirmation. A stock peer owns the rule in both cases.
            const bool effective_admin = admin || (guest.capable && !own_loopback);
            if (guest.capable && own_loopback &&
                !WorldReplica::set_block_allowed(guest.world.block_at(cell), block, false,
                                                 protocol_))
                return;
            if (guest.world.alter_block(cell, block, effective_admin, protocol_))
                note_guest_edit(guest, edit.block_location);
        }
        else if (message == "RemoveBlocksMessage")
        {
            RemoveBlocksPayload removal;
            if (decode_remove_blocks_payload(decoded.payload, removal, payload_error))
            {
                guest.world.remove_blocks(removal.blocks);
                for (const IntVector3 cell : removal.blocks)
                    note_guest_edit(guest, cell);
            }
        }
        else if (message == "ProvideChunkMessage")
        {
            ProvideChunkPayload chunk;
            if (decode_provide_chunk_payload(decoded.payload, chunk, payload_error))
                install_authoritative_chunk(guest, chunk);
        }
        else if (message == "DetonateExplosiveMessage")
        {
            DetonateExplosivePayload detonation;
            if (decode_detonate_explosive_payload(decoded.payload, protocol_, detonation,
                                                  payload_error))
            {
                const Cell cell{detonation.location.x, detonation.location.y,
                                detonation.location.z};
                if (guest.world.block_at(cell) == 20U || guest.world.block_at(cell) == 21U)
                {
                    guest.world.remove_blocks(std::array{detonation.location});
                    note_guest_edit(guest, detonation.location);
                }
            }
        }
        else if (message == "ItemCrateMessage")
        {
            ItemCratePayload crate;
            if (decode_item_crate_payload(decoded.payload, protocol_, crate, payload_error))
                guest.world.apply_crate_item(crate);
        }
        else if (message == "DestroyCrateMessage")
        {
            IntVector3 location;
            if (decode_int_vector_payload(decoded.payload, location, payload_error))
                guest.world.destroy_crate(location);
        }
        else if (message == "DestroyCustomBlockMessage")
        {
            DestroyCustomBlockPayload custom;
            if (decode_destroy_custom_block_payload(decoded.payload, custom, payload_error))
                guest.world.destroy_custom_block(custom.location);
        }
        else if (message == "UpdateSpawnerMessage")
        {
            UpdateSpawnerPayload update;
            if (decode_update_spawner_payload(decoded.payload, update, payload_error))
                guest.world.update_spawner({static_cast<int>(update.spawner_position.x),
                                            static_cast<int>(update.spawner_position.y),
                                            static_cast<int>(update.spawner_position.z)},
                                           update.is_started);
        }
    }

    // A guest broadcast: the stock stack applies the message to the sender's
    // own world through its local loopback in the same frame, then the host
    // forwards it to the other peers.
    void guest_broadcast(PeerId peer, Guest &guest, std::string_view name,
                         std::span<const std::uint8_t> payload)
    {
        const auto packet = frame(protocol_, name, payload);
        const auto delivery = delivery_of(protocol_, name);
        std::string error;
        const auto wire =
            guest.session.create_outbound(std::nullopt, packet, delivery, error);
        if (!wire.has_value())
            fail("the production client session refused a soak broadcast");
        apply_on_guest(guest, guest.gid, packet);
        guest.to_host.push(now_ms_, delivery, *wire);
        static_cast<void>(peer);
        ++report_.actions;
    }

    // ------------------------------------------------------------ the ticks
    void pump(double tick_ms)
    {
        now_ms_ += tick_ms;
        for (auto &[peer, guest] : guests_)
            for (const LegacySteamNativeSend &send : guest.to_host.drain(now_ms_))
            {
                const auto received = transport_.process_wire_packet(peer, send.payload);
                for (const LegacySteamHostInbound &inbound : received.inbound)
                    if (inbound.kind == LegacySteamInboundKind::gameplay)
                        handle_on_host(peer, inbound);
            }
        for (auto &[peer, guest] : guests_)
            for (const LegacySteamNativeSend &send : guest.to_guest.drain(now_ms_))
            {
                const auto delivered = guest.session.process_native_packet(
                    host_peer, send.p2p_channel, send.payload);
                for (const LegacySteamClientInbound &inbound : delivered.inbound)
                    if (inbound.kind == LegacySteamClientInboundKind::gameplay)
                        apply_on_guest(guest, inbound.gid, inbound.payload);
                static_cast<void>(peer);
            }
        ++report_.ticks;
    }

    [[nodiscard]] bool quiescent() const
    {
        return std::ranges::all_of(guests_, [](const auto &entry) {
            return entry.second.to_host.idle() && entry.second.to_guest.idle();
        });
    }

    // The comparison itself: the host's authoritative snapshot (what a late
    // joiner would be told now) against the host's own world and every peer's.
    void compare(bool require_convergence)
    {
        StateDigest authority{"host-authority"};
        record_world_state(authority, authority_.snapshot(), protocol_, region_);
        const StateDigest host = digest_of("host-world", host_, region_);
        if (authority.hash() != host.hash())
            note_divergence(describe_divergence(authority, host));
        for (const auto &[peer, guest] : guests_)
        {
            if (!guest.playing)
                continue;
            const StateDigest replica =
                digest_of(guest.session.local_gamer().gamer_tag, guest.world, region_);
            if (authority.hash() == replica.hash())
                continue;
            if (require_convergence)
                note_divergence(describe_divergence(authority, replica));
            static_cast<void>(peer);
        }
        if (require_convergence)
            ++report_.quiescent_checks;
    }

    void note_divergence(std::string description)
    {
        ++report_.divergences;
        if (report_.first_divergence.empty())
        {
            report_.first_divergence = std::move(description);
            report_.first_divergence_tick = report_.ticks;
        }
    }

    // ------------------------------------------------------- random actions
    void random_action()
    {
        if (guests_.empty())
            return;
        auto entry = guests_.begin();
        std::advance(entry, pick(guests_.size()));
        Guest &guest = entry->second;
        if (!guest.playing)
            return;
        const std::size_t actor = guest.actor;
        PayloadError error{};
        switch (pick(9U))
        {
        case 0: // mine one block
        case 1:
        {
            const IntVector3 cell = random_slab_cell(actor);
            const auto payload = encode_alter_block_payload({cell, 0}, protocol_, error);
            if (payload.has_value())
                guest_broadcast(entry->first, guest, "AlterBlockMessage", *payload);
            break;
        }
        case 2: // place a block where this peer believes the cell is empty
        {
            const IntVector3 cell = random_slab_cell(actor);
            if (guest.world.block_at({cell.x, cell.y, cell.z}) != 0U)
                break;
            const auto payload =
                encode_alter_block_payload({cell, 2 + static_cast<std::int32_t>(pick(3U))},
                                           protocol_, error);
            if (payload.has_value())
                guest_broadcast(entry->first, guest, "AlterBlockMessage", *payload);
            break;
        }
        case 3: // a batch removal, the shape an explosion or a chainsaw sends
        {
            RemoveBlocksPayload removal;
            removal.do_dig_effects = pick(2U) == 0U;
            const std::size_t count = 1U + pick(6U);
            for (std::size_t index{}; index < count; ++index)
                removal.blocks.push_back(random_slab_cell(actor));
            const auto payload = encode_remove_blocks_payload(removal, error);
            if (payload.has_value())
                guest_broadcast(entry->first, guest, "RemoveBlocksMessage", *payload);
            break;
        }
        case 4: // toggle this actor's door, the non-empty to non-empty case
        {
            const Cell door_cell = actor_door(actor);
            const IntVector3 lower{door_cell[0], door_cell[1], door_cell[2]};
            const IntVector3 upper{door_cell[0], door_cell[1] + 1, door_cell[2]};
            const std::uint8_t current = guest.world.block_at({lower.x, lower.y, lower.z});
            const bool opening = current == 27U;
            const auto door = encode_door_open_close_payload({lower, opening}, error);
            if (door.has_value())
                guest_broadcast(entry->first, guest, "DoorOpenCloseMessage", *door);
            const auto lower_payload = encode_alter_block_payload(
                {lower, opening ? 25 : 27}, protocol_, error);
            const auto upper_payload = encode_alter_block_payload(
                {upper, opening ? 30 : 29}, protocol_, error);
            if (lower_payload.has_value())
                guest_broadcast(entry->first, guest, "AlterBlockMessage", *lower_payload);
            if (upper_payload.has_value())
                guest_broadcast(entry->first, guest, "AlterBlockMessage", *upper_payload);
            break;
        }
        case 5: // a crate transfer into this actor's crate
        {
            const Cell owned = actor_crate(actor);
            ItemCratePayload crate;
            crate.location = {owned[0], owned[1], owned[2]};
            crate.index = static_cast<std::int32_t>(pick(8U));
            if (pick(3U) != 0U)
                crate.item = InventoryItemPayload{static_cast<std::int16_t>(2 + pick(3U)),
                                                  static_cast<std::int16_t>(1 + pick(40U)), 1.0F};
            const auto payload = encode_item_crate_payload(crate, protocol_, error);
            if (payload.has_value())
                guest_broadcast(entry->first, guest, "ItemCrateMessage", *payload);
            break;
        }
        case 6: // arm and detonate a placed explosive
        {
            const IntVector3 cell = random_slab_cell(actor);
            if (guest.world.block_at({cell.x, cell.y, cell.z}) != 0U)
                break;
            const auto placed = encode_alter_block_payload({cell, 20}, protocol_, error);
            if (!placed.has_value())
                break;
            guest_broadcast(entry->first, guest, "AlterBlockMessage", *placed);
            const auto flash = encode_int_vector_payload(cell, error);
            if (flash.has_value())
                guest_broadcast(entry->first, guest, "AddExplosiveFlashMessage", *flash);
            const auto detonation =
                encode_detonate_explosive_payload({cell, true, 0U}, protocol_, error);
            if (detonation.has_value())
                guest_broadcast(entry->first, guest, "DetonateExplosiveMessage", *detonation);
            break;
        }
        case 7: // destroy and immediately recreate this actor's scratch crate
        {
            const Cell scratch = actor_scratch_crate(actor);
            const IntVector3 location{scratch[0], scratch[1], scratch[2]};
            if (pick(2U) == 0U)
            {
                const auto payload = encode_int_vector_payload(location, error);
                if (payload.has_value())
                    guest_broadcast(entry->first, guest, "DestroyCrateMessage", *payload);
            }
            else
            {
                ItemCratePayload crate{location, 1, InventoryItemPayload{2, 3, 1.0F}};
                const auto payload = encode_item_crate_payload(crate, protocol_, error);
                if (payload.has_value())
                    guest_broadcast(entry->first, guest, "ItemCrateMessage", *payload);
            }
            break;
        }
        default: // host-origin world traffic, in the host's own slice
        {
            if (protocol_ == ProtocolVersion::modern_v4 && pick(2U) == 0U)
            {
                host_spawner_update({actor_slab_x(soak_actors - 1U, 2U), 4, -14}, true);
                break;
            }
            const IntVector3 cell = random_slab_cell(soak_actors - 1U);
            const auto block = static_cast<std::uint8_t>(pick(2U) == 0U ? 0U : 4U);
            if (!authority_.apply_block_edit(cell, block))
                break;
            static_cast<void>(host_.alter_block({cell.x, cell.y, cell.z}, block, true, protocol_));
            const auto payload = encode_alter_block_payload(
                {cell, static_cast<std::int32_t>(block)}, protocol_, error);
            if (payload.has_value())
                send_from_host(std::nullopt, frame(protocol_, "AlterBlockMessage", *payload),
                               delivery_of(protocol_, "AlterBlockMessage"));
            ++report_.actions;
            break;
        }
        }
    }

    void host_spawner_update(IntVector3 location, bool started)
    {
        if (!authority_.update_spawner(location, started))
            return;
        host_.update_spawner({location.x, location.y, location.z}, started);
        PayloadError error{};
        const auto payload = encode_update_spawner_payload(
            {{static_cast<float>(location.x), static_cast<float>(location.y),
              static_cast<float>(location.z)},
             started},
            error);
        if (payload.has_value())
            send_from_host(std::nullopt, frame(protocol_, "UpdateSpawnerMessage", *payload),
                           delivery_of(protocol_, "UpdateSpawnerMessage"));
        ++report_.actions;
    }

    // A chunk request racing live edits: the reply is reliable-unordered and so
    // may overtake or trail the reliable-ordered edit stream.
    void request_chunk(PeerId peer, Guest &guest, IntVector3 cell)
    {
        PayloadError error{};
        const auto payload = encode_request_chunk_payload({cell, 0U}, error);
        if (!payload.has_value())
            return;
        const auto packet = frame(protocol_, "RequestChunkMessage", *payload);
        std::string send_error;
        const auto wire = guest.session.create_outbound(
            std::uint8_t{0}, packet, delivery_of(protocol_, "RequestChunkMessage"), send_error);
        if (!wire.has_value())
            return;
        guest.to_host.push(now_ms_, delivery_of(protocol_, "RequestChunkMessage"), *wire);
        const IntVector3 corner = chunk_corner(cell);
        guest.requested_chunks.insert_or_assign(Cell{corner.x, corner.y, corner.z}, false);
        static_cast<void>(peer);
    }

    [[nodiscard]] IntVector3 random_slab_cell(std::size_t actor)
    {
        return {actor_slab_x(actor, pick(6U)), 3, -24 + static_cast<int>(pick(24U))};
    }

    [[nodiscard]] std::size_t pick(std::size_t bound)
    {
        return bound == 0U ? 0U
                           : std::uniform_int_distribution<std::size_t>{0U, bound - 1U}(random_);
    }

    void run(std::size_t bursts, std::size_t burst_actions, bool late_join,
             bool carol_is_capable = true)
    {
        carol_is_capable_ = carol_is_capable;
        bool joined = !late_join;
        for (std::size_t burst{}; burst < bursts; ++burst)
        {
            for (std::size_t index{}; index < burst_actions; ++index)
            {
                random_action();
                // A chunk reconciliation request racing the live edit stream.
                if (index % 7U == 3U && !guests_.empty())
                {
                    auto entry = guests_.begin();
                    std::advance(entry, pick(guests_.size()));
                    if (entry->second.playing)
                        request_chunk(entry->first, entry->second,
                                      random_slab_cell(entry->second.actor));
                }
                // The stock rate windows are per second; a soak that fires an
                // edit every frame would spend its whole run being throttled.
                for (std::size_t step{}; step < 4U; ++step)
                {
                    pump(16.0);
                    compare(false);
                }
            }
            // Let the links drain, then require every peer to agree exactly.
            for (std::size_t index{}; index < 64U; ++index)
            {
                pump(16.0);
                compare(false);
                if (quiescent())
                    break;
            }
            if (!quiescent())
                fail("the soak link never drained");
            compare(true);
            if (!joined && burst + 1U == bursts / 2U)
            {
                joined = true;
                late_join_mid_soak();
            }
        }
    }

    // A third peer joins while the others keep editing, then must install
    // exactly the host authority.
    void late_join_mid_soak()
    {
        admit(carol_peer, "Carol", 0x1357U, 2U, carol_is_capable_);
        for (std::size_t index{}; index < 12U; ++index)
        {
            random_action();
            pump(16.0);
        }
        for (std::size_t index{}; index < 128U && !quiescent(); ++index)
            pump(16.0);
        bootstrap(carol_peer, guests_.at(carol_peer));
        StateDigest authority{"host-authority"};
        record_world_state(authority, authority_.snapshot(), protocol_, region_);
        const StateDigest joiner = digest_of("carol-install", guests_.at(carol_peer).world, region_);
        if (authority.hash() != joiner.hash())
        {
            note_divergence(describe_divergence(authority, joiner));
            expect(false, "a mid-soak late joiner did not install the host's world exactly");
        }
    }

    // The race the soak first caught: a chunk request travels
    // reliable-unordered and overtakes the peer's own reliable-ordered edit, so
    // the host answers from a snapshot that predates it. Replacing the column
    // with that snapshot un-mines a block the host is about to confirm.
    void race_chunk_request_against_edit()
    {
        Guest &guest = guests_.at(alice_peer);
        const IntVector3 cell{actor_slab_x(guest.actor, 1U), 3, -7};
        const std::uint8_t before = guest.world.block_at({cell.x, cell.y, cell.z});
        expect(before != 0U, "the race fixture needs a solid starting block");
        // The edit comes first and arms the reconciliation check, exactly as
        // `publish_local_block_edit` does, but the reliable-ordered channel is
        // held back while the request and its reply flow past it.
        PayloadError error{};
        const auto payload = encode_alter_block_payload({cell, 0}, protocol_, error);
        if (!payload.has_value())
            fail("the race fixture could not encode a stock block edit");
        guest_broadcast(alice_peer, guest, "AlterBlockMessage", *payload);
        expect(guest.world.block_at({cell.x, cell.y, cell.z}) == 0U,
               "the sender's own edit must apply through its local loopback");
        request_chunk(alice_peer, guest, cell);
        for (std::size_t index{}; index < 64U && !quiescent(); ++index)
            pump(16.0);
        pump(16.0);
        expect(guest.world.block_at({cell.x, cell.y, cell.z}) == 0U,
               "a chunk snapshot that predates a confirmed edit must not revert it");
        StateDigest authority{"host-authority"};
        record_world_state(authority, authority_.snapshot(), protocol_, region_);
        const StateDigest replica = digest_of("alice", guest.world, region_);
        if (authority.hash() != replica.hash())
            std::cout << describe_divergence(authority, replica);
        expect(authority.hash() == replica.hash(),
               "the peer and the host authority must agree after the chunk race");
    }

    // ------------------------------------------- the capability-mode sections

    void settle()
    {
        for (std::size_t index{}; index < 128U && !quiescent(); ++index)
            pump(16.0);
        pump(16.0);
        compare(true);
    }

    void remove_guest(PeerId peer)
    {
        for (const LegacySteamNativeSend &drop : transport_.remove_peer(peer))
            if (guests_.at(drop.remote_peer)
                    .session.process_native_packet(host_peer, drop.p2p_channel, drop.payload)
                    .audit.has_value())
                fail("a soak peer refused a roster drop");
        authority_.disconnect(peer);
        guests_.erase(peer);
    }

    // One guest door toggle, settled. Returns whether the door actually moved
    // for the toggling guest, for the host, and for every other peer.
    struct DoorOutcome final
    {
        bool sender{};
        bool host{};
        bool others{};
    };

    [[nodiscard]] DoorOutcome toggle_door(PeerId peer)
    {
        Guest &guest = guests_.at(peer);
        const Cell door = actor_door(guest.actor);
        const IntVector3 lower{door[0], door[1], door[2]};
        const IntVector3 upper{door[0], door[1] + 1, door[2]};
        const std::uint8_t before = host_.block_at(door);
        const bool opening = before == 27U;
        const std::uint8_t wanted_lower = opening ? std::uint8_t{25} : std::uint8_t{27};
        const std::uint8_t wanted_upper = opening ? std::uint8_t{30} : std::uint8_t{29};
        PayloadError error{};
        const auto sound = encode_door_open_close_payload({lower, opening}, error);
        if (sound.has_value())
            guest_broadcast(peer, guest, "DoorOpenCloseMessage", *sound);
        const auto lower_payload =
            encode_alter_block_payload({lower, wanted_lower}, protocol_, error);
        const auto upper_payload =
            encode_alter_block_payload({upper, wanted_upper}, protocol_, error);
        if (!lower_payload.has_value() || !upper_payload.has_value())
            fail("the soak could not encode a door toggle");
        guest_broadcast(peer, guest, "AlterBlockMessage", *lower_payload);
        guest_broadcast(peer, guest, "AlterBlockMessage", *upper_payload);
        settle();
        DoorOutcome outcome;
        outcome.sender = guests_.at(peer).world.block_at(door) == wanted_lower &&
                         guests_.at(peer).world.block_at({upper.x, upper.y, upper.z}) ==
                             wanted_upper;
        outcome.host = host_.block_at(door) == wanted_lower;
        outcome.others = true;
        for (const auto &[other, state] : guests_)
            if (other != peer && state.playing)
                outcome.others = outcome.others && state.world.block_at(door) == wanted_lower;
        return outcome;
    }

    [[nodiscard]] const SoakReport &report() const noexcept
    {
        return report_;
    }

  private:
    ProtocolVersion protocol_;
    LinkProfile profile_;
    DigestRegion region_{-32, -64, -32, 64, 63, 32};
    LegacySteamHostSession transport_;
    AuthoritativeBootstrapHost authority_;
    WorldReplica host_;
    ClientGameplayRateLimiter limiter_;
    std::map<PeerId, Guest> guests_;
    std::mt19937 random_;
    double now_ms_{};
    bool carol_is_capable_{true};
    SoakReport report_;
};

void report(std::string_view name, const SoakReport &result)
{
    std::cout << "  " << name << ": ticks=" << result.ticks << " actions=" << result.actions
              << " relayed=" << result.relayed << " authority-rejects=" << result.host_rejections
              << " rate-rejects=" << result.rate_rejections
              << " stock-ignored=" << result.non_admin_ignored
              << " confirmed=" << result.confirmations
              << " corrections=" << result.corrections << " drains=" << result.quiescent_checks
              << " divergences=" << result.divergences << '\n';
    if (!result.first_divergence.empty())
        std::cout << "  first divergence at tick " << result.first_divergence_tick << ":\n"
                  << result.first_divergence;
}

// --------------------------------------------------------------- the sections

constexpr LinkProfile clean_link{};
constexpr LinkProfile impaired_link{80.0, 25.0, 0.12, 0.25, 0.05, 220.0};

void digest_tool_is_deterministic_and_names_the_divergence()
{
    DigestRegion region;
    WorldReplica left;
    WorldReplica right;
    for (int index = 0; index < 32; ++index)
    {
        left.blocks[{index, 3, -index}] = static_cast<std::uint8_t>(2 + index % 4);
        right.blocks[{31 - index, 3, -(31 - index)}] =
            static_cast<std::uint8_t>(2 + (31 - index) % 4);
    }
    expect(digest_of("a", left, region).hash() == digest_of("b", right, region).hash(),
           "the digest must not depend on the order facts were recorded in");

    right.blocks[{4, 3, -4}] = 9U;
    right.blocks[{4000, 3, -4}] = 9U; // Outside the compared window.
    const StateDigest first = digest_of("host", left, region);
    const StateDigest second = digest_of("guest", right, region);
    expect(first.hash() != second.hash(), "a changed block must change the digest");
    expect(first.hash(StateDomain::crates) == second.hash(StateDomain::crates),
           "a block divergence must not disturb another domain's digest");
    const auto differences = compare_state(first, second);
    expect(differences.size() == 1U && differences[0].domain == StateDomain::world_blocks &&
               differences[0].key == "4,3,-4",
           "the comparison must name the one diverged cell and ignore cells outside the region");

    InventoryStoragePayload slots;
    left.crates.emplace(Cell{1, 2, 3}, slots);
    const StateDigest third = digest_of("host", left, region);
    expect(third.hash(StateDomain::crates) != first.hash(StateDomain::crates),
           "a crate that exists only on one peer must diverge even when it is empty");
    const auto crate_differences = compare_state(third, first);
    expect(crate_differences.size() == 1U && crate_differences[0].domain == StateDomain::crates &&
               crate_differences[0].right == "<absent>",
           "an absent crate must be reported as absent, not as a hash mismatch");
}

void world_soak(ProtocolVersion protocol, std::string_view label, LinkProfile profile,
                std::uint32_t seed, bool late_join, bool stock_peer_present)
{
    Soak soak{protocol, profile, seed, stock_peer_present};
    soak.run(12U, 24U, late_join, !stock_peer_present);
    report(label, soak.report());
    expect(soak.report().divergences == 0U,
           std::string{label} + ": the world diverged between peers");
    expect(soak.report().actions > 150U,
           std::string{label} + ": the soak did not generate enough world traffic");
    if (protocol != ProtocolVersion::classic_v3)
        return;
    // Stock applies every AlterBlockMessage, so nothing is ever refused for
    // replacing an occupied cell and nothing needs confirming, whoever is in
    // the session.
    expect(soak.report().non_admin_ignored == 0U && soak.report().confirmations == 0U,
           std::string{label} +
               ": no edit may be refused or held back by the OpenClassic-only occupied-cell rule");
}

// A guest's door toggle is a non-empty over non-empty AlterBlockMessage, and in
// stock 1.6.3.8 it simply applies on every peer. Who else is in the session,
// stock or OpenMinerZ, must make no difference to it.
void a_stock_peer_joining_flips_classic_enforcement()
{
    Soak soak{ProtocolVersion::classic_v3, clean_link, 0x5A1DU};
    const auto all_capable = soak.toggle_door(alice_peer);
    expect(all_capable.sender && all_capable.host && all_capable.others,
           "in an all-OpenMinerZ session a guest's door must open for everyone");

    // A stock client joins: the door must keep moving for everyone, the stock
    // peer included, exactly as it does between two stock clients.
    soak.admit(carol_peer, "Carol", 0x2B2BU, 2U, false);
    const auto with_stock = soak.toggle_door(alice_peer);
    expect(with_stock.sender && with_stock.host && with_stock.others,
           "a stock peer in the session must not turn a guest's door toggle sound-only");

    soak.remove_guest(carol_peer);
    const auto capable_again = soak.toggle_door(alice_peer);
    expect(capable_again.sender && capable_again.host && capable_again.others,
           "a guest's door must still open for everyone after the stock peer leaves");

    report("Classic, guest door with and without a stock peer", soak.report());
    expect(soak.report().divergences == 0U,
           "the world diverged while the session's composition changed");
    expect(soak.report().non_admin_ignored == 0U && soak.report().confirmations == 0U,
           "the OpenClassic-only occupied-cell rule must never fire");
}

void stale_chunk_snapshot_cannot_revert_a_confirmed_edit(ProtocolVersion protocol)
{
    // One retransmission on the reliable-ordered channel; the chunk request and
    // its reply are unaffected and so arrive first.
    LinkProfile held{};
    held.ordered_head_of_line_ms = 400.0;
    Soak soak{protocol, held, 0x7AC3U};
    soak.race_chunk_request_against_edit();
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
    run("digest_tool_is_deterministic_and_names_the_divergence",
        digest_tool_is_deterministic_and_names_the_divergence);
    run("classic_world_soak_clean_link_all_capable", [] {
        world_soak(ProtocolVersion::classic_v3, "Classic, clean link, all OpenMinerZ", clean_link,
                   0xC1EAU, false, false);
    });
    run("classic_world_soak_impaired_link_all_capable", [] {
        world_soak(ProtocolVersion::classic_v3, "Classic, impaired link, all OpenMinerZ",
                   impaired_link, 0xBAD1U, true, false);
    });
    run("classic_world_soak_clean_link_stock_peer_present", [] {
        world_soak(ProtocolVersion::classic_v3, "Classic, clean link, stock peer present",
                   clean_link, 0xC1EAU, false, true);
    });
    run("classic_world_soak_impaired_link_stock_peer_present", [] {
        world_soak(ProtocolVersion::classic_v3, "Classic, impaired link, stock peer present",
                   impaired_link, 0xBAD1U, true, true);
    });
    run("a_stock_peer_joining_flips_classic_enforcement",
        a_stock_peer_joining_flips_classic_enforcement);
    run("modern_world_soak_clean_link", [] {
        world_soak(ProtocolVersion::modern_v4, "Modern, clean link", clean_link, 0x3EEDU, false,
                   false);
    });
    run("modern_world_soak_impaired_link", [] {
        world_soak(ProtocolVersion::modern_v4, "Modern, 80ms/25ms jitter, retransmit, reorder",
                   impaired_link, 0x9A17U, true, false);
    });
    run("classic_stale_chunk_snapshot_cannot_revert_a_confirmed_edit",
        [] { stale_chunk_snapshot_cannot_revert_a_confirmed_edit(ProtocolVersion::classic_v3); });
    run("modern_stale_chunk_snapshot_cannot_revert_a_confirmed_edit",
        [] { stale_chunk_snapshot_cannot_revert_a_confirmed_edit(ProtocolVersion::modern_v4); });
    if (failures == 0)
    {
        std::cout << "all multiplayer world desync sections passed\n";
        return 0;
    }
    std::cout << failures << " multiplayer world desync check(s) failed\n";
    return 1;
}
