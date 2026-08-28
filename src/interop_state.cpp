#include "openminerz/interop_state.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <system_error>

namespace openminerz
{
namespace
{

class Fingerprint final
{
  public:
    void u8(std::uint8_t value) noexcept
    {
        value_ = (value_ ^ value) * 1099511628211ULL;
    }

    void u16(std::uint16_t value) noexcept
    {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8U));
    }

    void u32(std::uint32_t value) noexcept
    {
        for (unsigned int shift{}; shift < 32U; shift += 8U)
            u8(static_cast<std::uint8_t>(value >> shift));
    }

    void u64(std::uint64_t value) noexcept
    {
        for (unsigned int shift{}; shift < 64U; shift += 8U)
            u8(static_cast<std::uint8_t>(value >> shift));
    }

    void float32(float value) noexcept
    {
        u32(std::bit_cast<std::uint32_t>(value));
    }

    void text(std::string_view value) noexcept
    {
        u64(value.size());
        for (const unsigned char character : value)
            u8(character);
    }

    [[nodiscard]] std::uint64_t value() const noexcept
    {
        return value_;
    }

  private:
    std::uint64_t value_{1469598103934665603ULL};
};

void add_stack(Fingerprint &hash, const PlayerInventory::Slot &slot) noexcept
{
    hash.u8(slot.has_value() ? 1U : 0U);
    if (!slot.has_value())
        return;
    hash.u16(static_cast<std::uint16_t>(slot->item));
    hash.u8(static_cast<std::uint8_t>(slot->block));
    hash.u16(slot->count);
    hash.u16(slot->rounds_in_clip);
    hash.float32(slot->durability);
    for (const float coordinate : slot->waypoint)
        hash.float32(coordinate);
    hash.text(slot->waypoint_name);
    hash.u8(slot->waypoint_valid ? 1U : 0U);
}

std::string_view role_label(InteropStateRole role) noexcept
{
    switch (role)
    {
    case InteropStateRole::offline:
        return "offline";
    case InteropStateRole::host:
        return "host";
    case InteropStateRole::client:
        return "client";
    }
    return "offline";
}

std::string_view phase_label(InteropStatePhase phase) noexcept
{
    switch (phase)
    {
    case InteropStatePhase::frontend:
        return "frontend";
    case InteropStatePhase::loading:
        return "loading";
    case InteropStatePhase::world:
        return "world";
    case InteropStatePhase::hosting:
        return "hosting";
    case InteropStatePhase::connected:
        return "connected";
    }
    return "frontend";
}

bool finite_snapshot(const InteropStateSnapshot &snapshot) noexcept
{
    if (!std::isfinite(snapshot.player_position.x) ||
        !std::isfinite(snapshot.player_position.y) ||
        !std::isfinite(snapshot.player_position.z) || !std::isfinite(snapshot.player_yaw) ||
        !std::isfinite(snapshot.player_pitch) || !std::isfinite(snapshot.player_health) ||
        snapshot.remote_player_count > 15U || snapshot.players.empty() ||
        snapshot.players.size() > 16U || snapshot.enemy_count > 1'000'000U ||
        snapshot.pickup_count > 1'000'000U || snapshot.edit_count > 10'000'000U ||
        snapshot.region.minimum_x > snapshot.region.maximum_x ||
        snapshot.region.minimum_z > snapshot.region.maximum_z)
        return false;
    std::array<bool, 256U> gids{};
    std::size_t local_count{};
    for (const InteropPlayerState &player : snapshot.players)
    {
        if (gids[player.gid] || !std::isfinite(player.position.x) ||
            !std::isfinite(player.position.y) || !std::isfinite(player.position.z) ||
            !std::isfinite(player.yaw) || !std::isfinite(player.pitch) ||
            (player.health.has_value() && !std::isfinite(*player.health)))
            return false;
        gids[player.gid] = true;
        local_count += player.local ? 1U : 0U;
    }
    return local_count == 1U && snapshot.remote_player_count + 1U == snapshot.players.size();
}

} // namespace

std::uint64_t fingerprint_interop_inventory(const PlayerInventory &inventory) noexcept
{
    Fingerprint hash;
    hash.u8(static_cast<std::uint8_t>(inventory.profile()));
    hash.u64(inventory.current_tray_index());
    hash.u64(inventory.selected_index());
    for (const PlayerInventory::Slot &slot : inventory.storage())
        add_stack(hash, slot);
    for (const PlayerInventory::Tray &tray : inventory.trays())
        for (const PlayerInventory::Slot &slot : tray)
            add_stack(hash, slot);
    return hash.value();
}

std::uint64_t fingerprint_interop_edits(std::span<const std::array<int, 4>> edits)
{
    std::vector<std::array<int, 4>> ordered{edits.begin(), edits.end()};
    std::ranges::sort(ordered);
    Fingerprint hash;
    hash.u64(ordered.size());
    for (const auto &edit : ordered)
        for (const int field : edit)
            hash.u32(static_cast<std::uint32_t>(field));
    return hash.value();
}

InteropRegionFingerprint fingerprint_interop_region(const world::WorldRegion &region, int center_x,
                                                     int center_z, int radius) noexcept
{
    radius = std::clamp(radius, 0, 64);
    const std::int64_t requested_minimum_x = static_cast<std::int64_t>(center_x) - radius;
    const std::int64_t requested_maximum_x = static_cast<std::int64_t>(center_x) + radius;
    const std::int64_t requested_minimum_z = static_cast<std::int64_t>(center_z) - radius;
    const std::int64_t requested_maximum_z = static_cast<std::int64_t>(center_z) + radius;
    InteropRegionFingerprint result;
    const std::int64_t minimum_x =
        std::max<std::int64_t>(region.minimum_x(), requested_minimum_x);
    const std::int64_t maximum_x =
        std::min<std::int64_t>(region.maximum_x(), requested_maximum_x);
    const std::int64_t minimum_z =
        std::max<std::int64_t>(region.minimum_z(), requested_minimum_z);
    const std::int64_t maximum_z =
        std::min<std::int64_t>(region.maximum_z(), requested_maximum_z);
    if (minimum_x > maximum_x || minimum_z > maximum_z)
    {
        result.minimum_x = result.maximum_x = center_x;
        result.minimum_z = result.maximum_z = center_z;
        return result;
    }
    result.minimum_x = static_cast<int>(minimum_x);
    result.maximum_x = static_cast<int>(maximum_x);
    result.minimum_z = static_cast<int>(minimum_z);
    result.maximum_z = static_cast<int>(maximum_z);
    Fingerprint hash;
    hash.u32(static_cast<std::uint32_t>(result.minimum_x));
    hash.u32(static_cast<std::uint32_t>(result.maximum_x));
    hash.u32(static_cast<std::uint32_t>(result.minimum_z));
    hash.u32(static_cast<std::uint32_t>(result.maximum_z));
    for (int x = result.minimum_x; x <= result.maximum_x; ++x)
        for (int z = result.minimum_z; z <= result.maximum_z; ++z)
            for (int y = world::world_min_y; y < world::world_min_y + world::chunk_height; ++y)
                hash.u8(static_cast<std::uint8_t>(region.block(x, y, z)));
    result.hash = hash.value();
    return result;
}

bool interop_region_fully_loaded(const world::WorldRegion &region, int center_x, int center_z,
                                 int radius) noexcept
{
    radius = std::clamp(radius, 0, 64);
    const std::int64_t minimum_x = static_cast<std::int64_t>(center_x) - radius;
    const std::int64_t maximum_x = static_cast<std::int64_t>(center_x) + radius;
    const std::int64_t minimum_z = static_cast<std::int64_t>(center_z) - radius;
    const std::int64_t maximum_z = static_cast<std::int64_t>(center_z) + radius;
    return minimum_x >= region.minimum_x() && maximum_x <= region.maximum_x() &&
           minimum_z >= region.minimum_z() && maximum_z <= region.maximum_z();
}

std::uint64_t fingerprint_interop_state(const InteropStateSnapshot &snapshot)
{
    Fingerprint hash;
    hash.u8(static_cast<std::uint8_t>(snapshot.role));
    hash.u8(static_cast<std::uint8_t>(snapshot.phase));
    hash.u8(static_cast<std::uint8_t>(snapshot.protocol));
    hash.u32(static_cast<std::uint32_t>(snapshot.world_seed));
    hash.u32(static_cast<std::uint32_t>(snapshot.region.minimum_x));
    hash.u32(static_cast<std::uint32_t>(snapshot.region.maximum_x));
    hash.u32(static_cast<std::uint32_t>(snapshot.region.minimum_z));
    hash.u32(static_cast<std::uint32_t>(snapshot.region.maximum_z));
    hash.u64(snapshot.region.hash);
    hash.float32(snapshot.player_position.x);
    hash.float32(snapshot.player_position.y);
    hash.float32(snapshot.player_position.z);
    hash.float32(snapshot.player_yaw);
    hash.float32(snapshot.player_pitch);
    hash.float32(snapshot.player_health);
    hash.u64(snapshot.inventory_hash);
    hash.u64(snapshot.edit_hash);
    hash.u64(snapshot.edit_count);
    hash.u64(snapshot.remote_player_count);
    hash.u64(snapshot.enemy_count);
    hash.u64(snapshot.pickup_count);
    hash.u8(snapshot.player_dead ? 1U : 0U);
    hash.u8(snapshot.dragon_active ? 1U : 0U);
    std::vector<InteropPlayerState> players = snapshot.players;
    std::ranges::sort(players, {}, &InteropPlayerState::gid);
    hash.u64(players.size());
    for (const InteropPlayerState &player : players)
    {
        hash.u8(player.gid);
        hash.u8(player.local ? 1U : 0U);
        hash.float32(player.position.x);
        hash.float32(player.position.y);
        hash.float32(player.position.z);
        hash.float32(player.yaw);
        hash.float32(player.pitch);
        hash.u8(player.health.has_value() ? 1U : 0U);
        if (player.health.has_value())
            hash.float32(*player.health);
        hash.u8(player.dead ? 1U : 0U);
    }
    return hash.value();
}

std::optional<std::string> encode_interop_state_snapshot_json(
    std::int64_t unix_milliseconds, const InteropStateSnapshot &snapshot)
{
    if (unix_milliseconds < 0 || !finite_snapshot(snapshot))
        return std::nullopt;
    std::vector<InteropPlayerState> players = snapshot.players;
    std::ranges::sort(players, {}, &InteropPlayerState::gid);
    std::string player_records{"["};
    for (std::size_t index{}; index < players.size(); ++index)
    {
        const InteropPlayerState &player = players[index];
        if (index != 0U)
            player_records += ',';
        const std::string health = player.health.has_value()
                                       ? std::format("{:.6f}", *player.health)
                                       : std::string{"null"};
        player_records += std::format(
            "{{\"gid\":{},\"local\":{},\"x\":{:.6f},\"y\":{:.6f},\"z\":{:.6f},"
            "\"yaw\":{:.6f},\"pitch\":{:.6f},\"health\":{},\"dead\":{}}}",
            static_cast<unsigned int>(player.gid), player.local ? "true" : "false",
            player.position.x, player.position.y, player.position.z, player.yaw, player.pitch,
            health, player.dead ? "true" : "false");
    }
    player_records += ']';
    return std::format(
        "{{\"schema\":\"openminerz.interop-state.v1\",\"unix_ms\":{},\"role\":\"{}\","
        "\"phase\":\"{}\",\"protocol\":{},\"world_seed\":{},"
        "\"region\":{{\"min_x\":{},\"max_x\":{},\"min_z\":{},\"max_z\":{},"
        "\"hash\":\"{:016x}\"}},\"player\":{{\"x\":{:.6f},\"y\":{:.6f},"
        "\"z\":{:.6f},\"yaw\":{:.6f},\"pitch\":{:.6f},\"health\":{:.6f},"
        "\"dead\":{}}},\"inventory_hash\":\"{:016x}\",\"edit_hash\":\"{:016x}\","
        "\"edit_count\":{},\"remote_players\":{},\"enemies\":{},\"pickups\":{},"
        "\"dragon_active\":{},\"players\":{},\"state_hash\":\"{:016x}\"}}",
        unix_milliseconds, role_label(snapshot.role), phase_label(snapshot.phase),
        static_cast<unsigned int>(snapshot.protocol), snapshot.world_seed,
        snapshot.region.minimum_x, snapshot.region.maximum_x, snapshot.region.minimum_z,
        snapshot.region.maximum_z, snapshot.region.hash, snapshot.player_position.x,
        snapshot.player_position.y, snapshot.player_position.z, snapshot.player_yaw,
        snapshot.player_pitch, snapshot.player_health, snapshot.player_dead ? "true" : "false",
        snapshot.inventory_hash, snapshot.edit_hash, snapshot.edit_count,
        snapshot.remote_player_count, snapshot.enemy_count, snapshot.pickup_count,
        snapshot.dragon_active ? "true" : "false", player_records,
        fingerprint_interop_state(snapshot));
}

bool write_interop_state_snapshot(const std::filesystem::path &path,
                                  const InteropStateSnapshot &snapshot, std::string &error)
{
    if (path.empty())
    {
        error = "The interop state path is empty";
        return false;
    }
    const auto now = std::chrono::system_clock::now();
    const auto unix_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto encoded = encode_interop_state_snapshot_json(unix_milliseconds, snapshot);
    if (!encoded.has_value())
    {
        error = "The interop state contains an invalid or unbounded value";
        return false;
    }
    std::error_code filesystem_error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error)
    {
        error = "The interop state directory could not be created";
        return false;
    }
    std::ofstream output{path, std::ios::binary | std::ios::out | std::ios::trunc};
    if (!output || !(output << *encoded << '\n' << std::flush))
    {
        error = "The interop state snapshot could not be written";
        return false;
    }
    error.clear();
    return true;
}

} // namespace openminerz
