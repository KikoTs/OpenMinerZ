#pragma once

#include "openminerz/net/legacy_steam_host_session.hpp"
#include "openminerz/net/payload_codec.hpp"

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace openminerz::net
{

enum class ClientGameplayAction : std::uint8_t
{
    reject,
    consume_on_host,
    relay,
    relay_and_consume
};

enum class ClientGameplayAuthorityError : std::uint8_t
{
    none,
    invalid_packet,
    invalid_route,
    host_only_message,
    identity_spoof,
    duplicate_presence,
    unsupported_message
};

struct ClientGameplayDecision final
{
    ClientGameplayAction action{ClientGameplayAction::reject};
    ClientGameplayAuthorityError error{ClientGameplayAuthorityError::unsupported_message};
    std::string_view message;

    [[nodiscard]] bool accepted() const noexcept
    {
        return action != ClientGameplayAction::reject;
    }

    [[nodiscard]] bool should_relay() const noexcept
    {
        return action == ClientGameplayAction::relay ||
               action == ClientGameplayAction::relay_and_consume;
    }

    [[nodiscard]] bool should_consume() const noexcept
    {
        return action == ClientGameplayAction::consume_on_host ||
               action == ClientGameplayAction::relay_and_consume;
    }
};

// Second-stage authority gate for application packets that already passed the
// stock wire/session/type/delivery boundary. It is deliberately exhaustive:
// every deployed message is assigned an original client route and authority
// domain, and host/terrain-server output is never accepted from a remote GID.
[[nodiscard]] ClientGameplayDecision assess_client_gameplay_authority(
    ProtocolVersion protocol, const LegacySteamHostInbound &inbound) noexcept;

// Shared semantic bounds for both the host authority gate and the receiving
// client's application layer. Original DigMessage surface normals are not
// restricted to voxel axes; model/custom-block collision planes may be sloped.
[[nodiscard]] bool valid_network_block_location(IntVector3 location) noexcept;
[[nodiscard]] bool valid_network_surface_normal(Vector3 direction) noexcept;

[[nodiscard]] std::string_view client_gameplay_authority_error_label(
    ClientGameplayAuthorityError error) noexcept;

class ClientGameplayRateLimiter final
{
  public:
    [[nodiscard]] bool allow(PeerId peer, std::string_view message, std::uint64_t now_milliseconds);
    // Exact Modern spawner announcements open a bounded transcript budget for
    // the source's 108-cell clear operation. Call only after decoding one of
    // the two captured trigger strings.
    [[nodiscard]] bool allow_spawner_trigger(PeerId peer, std::uint64_t now_milliseconds);
    void disconnect(PeerId peer) noexcept;
    void clear() noexcept;

  private:
    struct Window final
    {
        std::uint64_t started_milliseconds{};
        std::uint16_t total{};
        std::uint16_t block_edits{};
        std::uint16_t spawner_triggers{};
        std::uint16_t spawner_block_edits_remaining{};
        std::uint16_t explosions{};
        std::uint16_t block_removals{};
        std::uint16_t pickup_creations{};
        std::uint16_t pickup_requests{};
        std::uint16_t crate_items{};
        std::uint16_t crate_destructions{};
        std::uint16_t gunshots{};
        std::uint16_t shotgun_shots{};
        std::uint16_t melee_attacks{};
        std::uint16_t projectile_launches{};
        std::uint16_t placed_explosive_detonations{};
        std::uint16_t explosive_effects{};
        std::uint16_t dig_effects{};
        std::uint16_t door_events{};
        std::uint16_t custom_block_events{};
        std::uint16_t enemy_spawns{};
        std::uint16_t enemy_events{};
        std::uint16_t dragon_updates{};
        std::uint16_t dragon_events{};
        std::uint16_t player_updates{};
        std::uint16_t text_messages{};
        std::uint16_t voice_messages{};
        std::uint16_t level_restarts{};
    };

    std::unordered_map<PeerId, Window> windows_;
};

} // namespace openminerz::net
