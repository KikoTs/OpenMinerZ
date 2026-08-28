#pragma once

#include "openminerz/inventory.hpp"
#include "openminerz/net/protocol.hpp"
#include "openminerz/world/player.hpp"
#include "openminerz/world/world.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openminerz
{

enum class InteropStateRole : std::uint8_t
{
    offline,
    host,
    client
};

enum class InteropStatePhase : std::uint8_t
{
    frontend,
    loading,
    world,
    hosting,
    connected
};

struct InteropRegionFingerprint final
{
    int minimum_x{};
    int maximum_x{};
    int minimum_z{};
    int maximum_z{};
    std::uint64_t hash{};

    auto operator<=>(const InteropRegionFingerprint &) const = default;
};

struct InteropPlayerState final
{
    std::uint8_t gid{};
    bool local{};
    world::Vec3 position;
    float yaw{};
    float pitch{};
    std::optional<float> health;
    bool dead{};
};

struct InteropStateSnapshot final
{
    InteropStateRole role{InteropStateRole::offline};
    InteropStatePhase phase{InteropStatePhase::frontend};
    net::ProtocolVersion protocol{net::ProtocolVersion::classic_v3};
    std::int32_t world_seed{};
    InteropRegionFingerprint region;
    world::Vec3 player_position;
    float player_yaw{};
    float player_pitch{};
    float player_health{};
    std::uint64_t inventory_hash{};
    std::uint64_t edit_hash{};
    std::size_t edit_count{};
    std::size_t remote_player_count{};
    std::size_t enemy_count{};
    std::size_t pickup_count{};
    bool player_dead{};
    bool dragon_active{};
    std::vector<InteropPlayerState> players;
};

[[nodiscard]] std::uint64_t fingerprint_interop_inventory(
    const PlayerInventory &inventory) noexcept;
[[nodiscard]] std::uint64_t fingerprint_interop_edits(
    std::span<const std::array<int, 4>> edits);
[[nodiscard]] InteropRegionFingerprint fingerprint_interop_region(
    const world::WorldRegion &region, int center_x, int center_z, int radius = 16) noexcept;
[[nodiscard]] bool interop_region_fully_loaded(const world::WorldRegion &region, int center_x,
                                               int center_z, int radius = 16) noexcept;
[[nodiscard]] std::uint64_t fingerprint_interop_state(const InteropStateSnapshot &snapshot);
[[nodiscard]] std::optional<std::string> encode_interop_state_snapshot_json(
    std::int64_t unix_milliseconds, const InteropStateSnapshot &snapshot);
[[nodiscard]] bool write_interop_state_snapshot(const std::filesystem::path &path,
                                                const InteropStateSnapshot &snapshot,
                                                std::string &error);

} // namespace openminerz
