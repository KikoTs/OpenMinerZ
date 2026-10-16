#pragma once

// Shared desync detection tool.
//
// A deterministic, order-independent, domain-decomposed digest of one peer's
// authoritative state. Two peers that agree produce the same hash; two peers
// that disagree produce a readable diff that *names the diverged facts*, not
// just a hash mismatch, so a soak can print the cause at the tick it appears.
//
// Usage from another desync test (host and guest side, every simulated tick):
//
//   #include "state_digest.hpp"
//   using namespace openminerz::test;
//
//   StateDigest host{"host"};
//   record_world_state(host, authority.snapshot(), protocol, region);
//   StateDigest guest{"alice"};
//   record_world_state(guest, alice_save, protocol, region);
//   if (host.hash() != guest.hash())
//       std::cout << describe_divergence(host, guest) << '\n';
//
// Cost is O(facts) per call with no allocation beyond the fact maps, which is
// cheap enough to run on every simulated tick of a long soak.
//
// Extension points for the other desync domains. Each owner adds a
// `record_<domain>` overload beside the world ones below and records facts
// under its own StateDomain; nothing else in this header has to change, and
// the diff/compare machinery already reports the new domain by name:
//   * StateDomain::inventory  - item conservation (engineer 2)
//   * StateDomain::enemies    - enemies and dragons (engineer 3)
//   * StateDomain::player     - player/session state (engineer 4)

#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/world/enemy.hpp"
#include "openminerz/world_save.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::test
{

enum class StateDomain : std::uint8_t
{
    world_blocks,
    crates,
    doors,
    spawners,
    explosives,
    inventory,
    enemies,
    player,
    domain_count
};

inline constexpr std::size_t state_domain_count =
    static_cast<std::size_t>(StateDomain::domain_count);

[[nodiscard]] inline std::string_view state_domain_name(StateDomain domain) noexcept
{
    switch (domain)
    {
    case StateDomain::world_blocks:
        return "world-blocks";
    case StateDomain::crates:
        return "crates";
    case StateDomain::doors:
        return "doors";
    case StateDomain::spawners:
        return "spawners";
    case StateDomain::explosives:
        return "explosives";
    case StateDomain::inventory:
        return "inventory";
    case StateDomain::enemies:
        return "enemies";
    case StateDomain::player:
        return "player";
    case StateDomain::domain_count:
        break;
    }
    return "unknown";
}

// The fixed world window a block comparison covers. Block state outside it is
// deliberately ignored: a soak edits inside one window so that a divergence is
// attributable, and an unbounded window would make the digest unbounded too.
struct DigestRegion final
{
    int min_x{-128};
    int min_y{-64};
    int min_z{-128};
    int max_x{127};
    int max_y{63};
    int max_z{127};

    [[nodiscard]] bool contains(int x, int y, int z) const noexcept
    {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y && z >= min_z && z <= max_z;
    }
    [[nodiscard]] bool contains(const net::IntVector3 &location) const noexcept
    {
        return contains(location.x, location.y, location.z);
    }
    [[nodiscard]] bool contains(const std::array<int, 3> &location) const noexcept
    {
        return contains(location[0], location[1], location[2]);
    }
};

// One peer's state as a set of domain-tagged (key, value) facts. A key is a
// canonical identity ("12,3,-8"); a value is everything that must match. Facts
// live in sorted maps, so insertion order never changes the hash.
class StateDigest final
{
  public:
    explicit StateDigest(std::string label) : label_{std::move(label)}
    {
    }

    void record(StateDomain domain, std::string key, std::string value)
    {
        facts_[static_cast<std::size_t>(domain)].insert_or_assign(std::move(key), std::move(value));
    }

    [[nodiscard]] const std::map<std::string, std::string> &facts(StateDomain domain) const noexcept
    {
        return facts_[static_cast<std::size_t>(domain)];
    }

    [[nodiscard]] std::size_t size(StateDomain domain) const noexcept
    {
        return facts(domain).size();
    }

    [[nodiscard]] std::uint64_t hash(StateDomain domain) const noexcept
    {
        std::uint64_t value = 0xCBF29CE484222325ULL;
        const auto mix = [&value](std::string_view text) {
            for (const char character : text)
            {
                value ^= static_cast<std::uint8_t>(character);
                value *= 0x100000001B3ULL;
            }
            value ^= 0xFFU;
            value *= 0x100000001B3ULL;
        };
        for (const auto &[key, fact] : facts(domain))
        {
            mix(key);
            mix(fact);
        }
        return value;
    }

    [[nodiscard]] std::uint64_t hash() const noexcept
    {
        std::uint64_t value = 0x9E3779B97F4A7C15ULL;
        for (std::size_t index{}; index < state_domain_count; ++index)
        {
            value ^= hash(static_cast<StateDomain>(index));
            value *= 0x100000001B3ULL;
        }
        return value;
    }

    [[nodiscard]] const std::string &label() const noexcept
    {
        return label_;
    }

  private:
    std::string label_;
    std::array<std::map<std::string, std::string>, state_domain_count> facts_;
};

struct DigestDifference final
{
    StateDomain domain{};
    std::string key;
    // "<absent>" when the peer does not know this fact at all.
    std::string left;
    std::string right;
};

// Every fact the two peers disagree about, in (domain, key) order, capped so a
// per-tick comparison can never print an unbounded report.
[[nodiscard]] inline std::vector<DigestDifference> compare_state(const StateDigest &left,
                                                                 const StateDigest &right,
                                                                 std::size_t limit = 8U)
{
    static const std::string absent{"<absent>"};
    std::vector<DigestDifference> differences;
    for (std::size_t index{}; index < state_domain_count; ++index)
    {
        const auto domain = static_cast<StateDomain>(index);
        if (left.hash(domain) == right.hash(domain))
            continue;
        const auto &first = left.facts(domain);
        const auto &second = right.facts(domain);
        auto a = first.begin();
        auto b = second.begin();
        while ((a != first.end() || b != second.end()) && differences.size() < limit)
        {
            if (b == second.end() || (a != first.end() && a->first < b->first))
            {
                differences.push_back({domain, a->first, a->second, absent});
                ++a;
            }
            else if (a == first.end() || b->first < a->first)
            {
                differences.push_back({domain, b->first, absent, b->second});
                ++b;
            }
            else
            {
                if (a->second != b->second)
                    differences.push_back({domain, a->first, a->second, b->second});
                ++a;
                ++b;
            }
        }
        if (differences.size() >= limit)
            break;
    }
    return differences;
}

[[nodiscard]] inline std::string describe_divergence(const StateDigest &left,
                                                     const StateDigest &right,
                                                     std::size_t limit = 8U)
{
    std::ostringstream out;
    out << "state divergence " << left.label() << " vs " << right.label() << '\n';
    for (std::size_t index{}; index < state_domain_count; ++index)
    {
        const auto domain = static_cast<StateDomain>(index);
        if (left.hash(domain) == right.hash(domain))
            continue;
        out << "  domain " << state_domain_name(domain) << ": " << left.size(domain) << " vs "
            << right.size(domain) << " facts\n";
    }
    const auto differences = compare_state(left, right, limit);
    for (const DigestDifference &difference : differences)
        out << "    " << state_domain_name(difference.domain) << ' ' << difference.key << ": "
            << left.label() << '=' << difference.left << ' ' << right.label() << '='
            << difference.right << '\n';
    if (differences.size() >= limit)
        out << "    (first " << limit << " divergences only)\n";
    return out.str();
}

// ------------------------------------------------------- canonical fact text

[[nodiscard]] inline std::string digest_cell_key(int x, int y, int z)
{
    std::ostringstream out;
    out << x << ',' << y << ',' << z;
    return out.str();
}

[[nodiscard]] inline std::string digest_cell_key(const net::IntVector3 &location)
{
    return digest_cell_key(location.x, location.y, location.z);
}

[[nodiscard]] inline std::string digest_cell_key(const std::array<int, 3> &location)
{
    return digest_cell_key(location[0], location[1], location[2]);
}

// Everything about a crate slot that must match between two peers. Written
// from the wire payload on both sides so a native stack and a replicated
// stack can never disagree merely about representation.
[[nodiscard]] inline std::string describe_slot(const net::InventorySlotPayload &slot)
{
    if (!slot.has_value())
        return "empty";
    std::ostringstream out;
    out << "item=" << slot->item_id << " count=" << slot->stack_count << " health=" << slot->health;
    if (slot->rounds_in_clip.has_value())
        out << " clip=" << *slot->rounds_in_clip;
    if (slot->point_to_location.has_value())
        out << " point=" << slot->point_to_location->x << ',' << slot->point_to_location->y << ','
            << slot->point_to_location->z;
    if (slot->custom_name.has_value())
        out << " name=" << *slot->custom_name;
    return out.str();
}

// ------------------------------------------------------------- world domains

// Blocks are compared as the authoritative edit set, which is exactly what a
// late joiner is told: a cell absent from both peers is unmodified terrain and
// generates identically from the shared seed.
inline void record_world_blocks(StateDigest &digest,
                                const std::map<std::array<int, 3>, std::uint8_t> &blocks,
                                const DigestRegion &region)
{
    for (const auto &[location, block] : blocks)
        if (region.contains(location))
            digest.record(StateDomain::world_blocks, digest_cell_key(location),
                          std::to_string(static_cast<unsigned>(block)));
}

// Later entries win, exactly as a replayed edit list applies.
[[nodiscard]] inline std::map<std::array<int, 3>, std::uint8_t> canonical_block_edits(
    std::span<const std::array<int, 4>> edits)
{
    std::map<std::array<int, 3>, std::uint8_t> result;
    for (const std::array<int, 4> &edit : edits)
        result.insert_or_assign(std::array<int, 3>{edit[0], edit[1], edit[2]},
                                static_cast<std::uint8_t>(edit[3]));
    return result;
}

[[nodiscard]] inline std::map<std::array<int, 3>, std::uint8_t> canonical_chunk_deltas(
    const std::map<net::IntVector3, std::vector<std::int32_t>> &chunk_deltas)
{
    std::map<std::array<int, 3>, std::uint8_t> result;
    for (const auto &[corner, delta] : chunk_deltas)
        for (const std::int32_t signed_entry : delta)
        {
            const auto entry = static_cast<std::uint32_t>(signed_entry);
            const std::array<int, 3> location{
                corner.x + static_cast<int>((entry >> 16U) & 0xFFU),
                corner.y + static_cast<int>((entry >> 8U) & 0xFFU),
                corner.z + static_cast<int>(entry & 0xFFU)};
            result.insert_or_assign(location, static_cast<std::uint8_t>(entry >> 24U));
        }
    return result;
}

// A crate's identity (it exists) is a fact of its own, so a crate that exists
// only on one peer diverges even when every slot is empty on both.
inline void record_crate(StateDigest &digest, const std::array<int, 3> &location,
                         const net::InventoryStoragePayload &slots)
{
    const std::string key = digest_cell_key(location);
    digest.record(StateDomain::crates, key, "present");
    for (std::size_t index{}; index < slots.size(); ++index)
        if (slots[index].has_value())
            digest.record(StateDomain::crates, key + '/' + std::to_string(index),
                          describe_slot(slots[index]));
}

inline void record_crate(StateDigest &digest, const std::array<int, 3> &location,
                         const WorldSaveCrateSlots &slots, net::ProtocolVersion protocol)
{
    net::InventoryStoragePayload converted;
    for (std::size_t index{}; index < slots.size(); ++index)
        if (!net::inventory_slot_to_payload(slots[index], protocol, converted[index]))
            converted[index].reset();
    record_crate(digest, location, converted);
}

inline void record_doors(StateDigest &digest, const WorldSaveDoorMap &doors,
                         const DigestRegion &region)
{
    for (const auto &[location, model] : doors)
        if (region.contains(location))
            digest.record(StateDomain::doors, digest_cell_key(location),
                          "model=" + std::to_string(static_cast<unsigned>(model)));
}

inline void record_spawners(StateDigest &digest, const WorldSaveSpawnerMap &spawners,
                            const DigestRegion &region)
{
    for (const auto &[location, spawn_id] : spawners)
        if (region.contains(location))
            digest.record(StateDomain::spawners, digest_cell_key(location), "id=" + spawn_id);
}

// Placed explosives are armed-fuse state rather than world blocks: the block
// itself lives in the block domain, this records that a peer believes a fuse
// is burning at that cell.
inline void record_armed_explosives(StateDigest &digest,
                                    const std::map<std::array<int, 3>, std::uint8_t> &armed,
                                    const DigestRegion &region)
{
    for (const auto &[location, kind] : armed)
        if (region.contains(location))
            digest.record(StateDomain::explosives, digest_cell_key(location),
                          "kind=" + std::to_string(static_cast<unsigned>(kind)));
}

// --------------------------------------------- whole-peer world projections

// What the host would tell a late joiner right now. Crates travel either
// inside WorldInfo or as an ItemCrate replay list; both are folded here the
// same way the bootstrap transcript folds them.
inline void record_world_state(StateDigest &digest,
                               const net::AuthoritativeBootstrapSnapshot &snapshot,
                               net::ProtocolVersion protocol, const DigestRegion &region)
{
    record_world_blocks(digest, canonical_chunk_deltas(snapshot.chunk_deltas), region);

    std::map<std::array<int, 3>, net::InventoryStoragePayload> crates;
    for (const net::WorldCratePayload &crate : snapshot.world.crates)
        crates.insert_or_assign({crate.location.x, crate.location.y, crate.location.z},
                                crate.inventory);
    for (const net::ItemCratePayload &item : snapshot.crate_replay)
    {
        auto &slots = crates[{item.location.x, item.location.y, item.location.z}];
        if (item.index >= 0 && item.index < static_cast<std::int32_t>(slots.size()))
            slots[static_cast<std::size_t>(item.index)] = item.item;
    }
    for (const auto &[location, slots] : crates)
        if (region.contains(location))
            record_crate(digest, location, slots);

    WorldSaveDoorMap doors;
    for (const net::WorldDoorPayload &door : snapshot.world.doors)
        doors.insert_or_assign({door.location.x, door.location.y, door.location.z},
                               door.model_name);
    record_doors(digest, doors, region);

    WorldSaveSpawnerMap spawners;
    for (const net::WorldSpawnerPayload &spawner : snapshot.world.spawners)
        spawners.insert_or_assign({spawner.location.x, spawner.location.y, spawner.location.z},
                                  spawner.spawn_id);
    record_spawners(digest, spawners, region);
    static_cast<void>(protocol);
}

// What a joiner installed, or what a save round trip would restore.
inline void record_world_state(StateDigest &digest, const WorldSaveData &save,
                               net::ProtocolVersion protocol, const DigestRegion &region)
{
    record_world_blocks(digest, canonical_block_edits(save.edits), region);
    for (const auto &[location, slots] : save.crates)
        if (region.contains(location))
            record_crate(digest, location, slots, protocol);
    record_doors(digest, save.doors, region);
    record_spawners(digest, save.spawners, region);
}

// ------------------------------------------------- enemies and dragons

// The enemy facts that must agree between peers. Identity is the original's
// source pair (target GID, sender-local enemy ID) rather than a list index, so
// the comparison survives differing spawn order and differing cull order.
//
// Liveness and the enraged/give-up/dying progression are authoritative: every
// peer runs the same BaseZombie state machine against the same replicated
// target, so these must match. Two things are deliberately NOT recorded. Exact
// positions drift by a fraction of a block because BaseZombie is simulated
// independently on each peer. And the loot debt is per-peer by design: only the
// peer named by KillerID runs CreatePickup, so a matching debt would be the
// bug, not the agreement.
[[nodiscard]] inline std::string describe_enemy(const world::Enemy &enemy)
{
    const char *phase = "alive";
    switch (enemy.state)
    {
    case world::EnemyState::die:
        phase = "dying";
        break;
    case world::EnemyState::give_up:
        phase = "givingup";
        break;
    case world::EnemyState::despawn:
        phase = "removed";
        break;
    default:
        break;
    }
    std::ostringstream out;
    out << "type=" << enemy.type << " phase=" << phase << " seed=" << enemy.random_seed
        << " enraged=" << (enemy.running ? 1 : 0);
    return out.str();
}

inline void record_enemies(StateDigest &digest, const world::EnemyHorde &horde)
{
    for (const world::Enemy &enemy : horde.enemies())
    {
        std::ostringstream key;
        key << static_cast<unsigned>(enemy.target_gid) << '/' << enemy.id;
        digest.record(StateDomain::enemies, key.str(), describe_enemy(enemy));
    }
}

// The dragon is one entity per session, so its identity is its owner. Health
// is deliberately excluded: DragonClientEntity.Health is per-peer in the
// original and two peers legitimately disagree about it, which is exactly why
// the kill is a message rather than a health comparison.
inline void record_dragon(StateDigest &digest, const world::DragonFlight &dragon,
                          std::optional<std::uint8_t> owner_gid)
{
    if (!dragon.active())
    {
        digest.record(StateDomain::enemies, "dragon", "absent");
        return;
    }
    std::ostringstream out;
    out << "kind=" << static_cast<unsigned>(dragon.dragon().kind) << " owner="
        << (owner_gid.has_value() ? std::to_string(static_cast<unsigned>(*owner_gid))
                                  : std::string{"local"})
        << " dying=" << (dragon.dragon().state == world::DragonState::dying ? 1 : 0);
    digest.record(StateDomain::enemies, "dragon", out.str());
}

// ------------------------------------------------- player and session state
//
// What every peer must agree about a person in the session. Position, yaw and
// pitch are deliberately excluded: PlayerUpdateMessage is unreliable and the
// presentation layer smooths what it draws, so two peers legitimately hold
// coordinates a packet apart. What they may never disagree about is the
// discrete state the original keys behaviour off - who is in the roster, who is
// dead, and what each person is holding - because all of it is change-only or
// re-sent state that a lost packet can leave permanently wrong.
struct PlayerFact final
{
    std::string gamer_tag;
    // ChangeCarriedItemMessage's wire item; nullopt before the peer announces.
    std::optional<std::uint16_t> carried_item;
    bool dead{};
    bool present{true};
};

[[nodiscard]] inline std::string describe_player(const PlayerFact &player)
{
    std::ostringstream out;
    if (!player.present)
        return "absent";
    out << "tag=" << player.gamer_tag << " dead=" << (player.dead ? 1 : 0) << " carried=";
    if (player.carried_item.has_value())
        out << *player.carried_item;
    else
        out << "unannounced";
    return out.str();
}

inline void record_player(StateDigest &digest, std::uint8_t gid, const PlayerFact &player)
{
    digest.record(StateDomain::player, std::to_string(static_cast<unsigned>(gid)),
                  describe_player(player));
}

// GameScreen.Day, as its DAY NUMBER only.
//
// The stock protocol cannot offer finer agreement than that: TimeOfDayMessage
// goes out every five seconds and each peer advances GameScreen.Day on its own
// frame clock in between, and a day is sixteen real minutes, so five seconds is
// seven and a half game minutes. Two stock peers legitimately read a few game
// minutes apart; a numeric drift bound is the right assertion for that part.
// What they must never disagree about is the day counter itself, because it
// drives the HUD's "DAY n", the horror stinger on rollover and enemy
// difficulty - a peer a whole day out is a different game.
inline void record_session_clock(StateDigest &digest, float day)
{
    digest.record(StateDomain::player, "day",
                  std::to_string(static_cast<int>(std::floor(day))));
}

} // namespace openminerz::test
