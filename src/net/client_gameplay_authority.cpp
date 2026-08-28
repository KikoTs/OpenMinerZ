#include "openminerz/net/client_gameplay_authority.hpp"

#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/dragon_codec.hpp"
#include "openminerz/net/enemy_codec.hpp"
#include "openminerz/net/misc_codec.hpp"
#include "openminerz/net/payload_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace openminerz::net
{
namespace
{

using namespace std::literals;

template <std::size_t Size>
bool contains(const std::array<std::string_view, Size> &values, std::string_view value) noexcept
{
    for (const std::string_view candidate : values)
        if (candidate == value)
            return true;
    return false;
}

constexpr std::array direct_host_requests{
    "ClientReadyForChunksMessage"sv,
    "RequestChunkMessage"sv,
    "RequestWorldInfoMessage"sv,
};

constexpr std::array broadcast_host_requests{
    "InventoryStoreOnServerMessage"sv,
    "RequestDragonMessage"sv,
    "RequestInventoryMessage"sv,
    "RequestPickupMessage"sv,
};

constexpr std::array client_broadcasts{
    "AddExplosionEffectsMessage"sv,
    "AddExplosiveFlashMessage"sv,
    "AlterBlockMessage"sv,
    "BroadcastTextMessage"sv,
    "ChangeCarriedItemMessage"sv,
    "CrateFocusMessage"sv,
    "CreatePickupMessage"sv,
    "DestroyCrateMessage"sv,
    "DestroyCustomBlockMessage"sv,
    "DetonateExplosiveMessage"sv,
    "DetonateFireballMessage"sv,
    "DetonateGrenadeMessage"sv,
    "DetonateRocketMessage"sv,
    "DigMessage"sv,
    "DoorOpenCloseMessage"sv,
    "DragonAttackMessage"sv,
    "EnemyGiveUpMessage"sv,
    "ExistingDragonMessage"sv,
    "FireRocketMessage"sv,
    "GrenadeMessage"sv,
    "GunshotMessage"sv,
    "ItemCrateMessage"sv,
    "KillDragonMessage"sv,
    "KillEnemyMessage"sv,
    "MigrateDragonMessage"sv,
    "PlayerExistsMessage"sv,
    "PlayerUpdateMessage"sv,
    "RemoveBlocksMessage"sv,
    "RemoveDragonMessage"sv,
    "ShotgunShotMessage"sv,
    "SpawnEnemyMessage"sv,
    "SpeedUpEnemyMessage"sv,
    "TimeConnectedMessage"sv,
    "UpdateDragonMessage"sv,
    "ChatMessage"sv,
    "VoiceChatMessage"sv,
};

constexpr std::array host_only_messages{
    "AppointServerMessage"sv, "ConsumePickupMessage"sv, "InventoryRetrieveFromServerMessage"sv,
    "KickMessage"sv,          "ProvideChunkMessage"sv,  "ProvideDeltaListMessage"sv,
    "RestartLevelMessage"sv,  "SpawnDragonMessage"sv,   "TimeOfDayMessage"sv,
    "UpdateSpawnerMessage"sv, "WorldInfoMessage"sv,
};

ClientGameplayDecision reject(std::string_view message, ClientGameplayAuthorityError error) noexcept
{
    return {ClientGameplayAction::reject, error, message};
}

bool broadcast_route(const LegacySteamHostInbound &inbound) noexcept
{
    return inbound.channel == 1U && inbound.recipient_gid == 0U;
}

bool bounded_block_location(IntVector3 location) noexcept
{
    return location.x >= -1'000'000 && location.x <= 1'000'000 && location.y >= -64 &&
           location.y < 64 && location.z >= -1'000'000 && location.z <= 1'000'000;
}

bool bounded_world_point(Vector3 location) noexcept
{
    return location.x >= -1'000'000.0F && location.x <= 1'000'000.0F && location.y >= -128.0F &&
           location.y <= 128.0F && location.z >= -1'000'000.0F && location.z <= 1'000'000.0F;
}

bool face_normal(Vector3 direction) noexcept
{
    const auto axis = [](float value) { return std::abs(std::abs(value) - 1.0F) <= 0.001F; };
    const auto zero = [](float value) { return std::abs(value) <= 0.001F; };
    return (axis(direction.x) && zero(direction.y) && zero(direction.z)) ||
           (zero(direction.x) && axis(direction.y) && zero(direction.z)) ||
           (zero(direction.x) && zero(direction.y) && axis(direction.z));
}

} // namespace

ClientGameplayDecision assess_client_gameplay_authority(
    ProtocolVersion protocol, const LegacySteamHostInbound &inbound) noexcept
{
    DecodedPacket decoded;
    PacketError packet_error{};
    if (inbound.sender_gid == 0U ||
        !decode_packet(protocol, inbound.payload, decoded, packet_error) ||
        decoded.message == nullptr)
        return reject({}, ClientGameplayAuthorityError::invalid_packet);

    const std::string_view message = decoded.message->name;
    if (contains(host_only_messages, message))
        return reject(message, ClientGameplayAuthorityError::host_only_message);

    if (contains(direct_host_requests, message))
    {
        if (inbound.channel != 0U || inbound.recipient_gid != 0U)
            return reject(message, ClientGameplayAuthorityError::invalid_route);
        return {ClientGameplayAction::consume_on_host, ClientGameplayAuthorityError::none, message};
    }

    if (contains(broadcast_host_requests, message))
    {
        if (!broadcast_route(inbound))
            return reject(message, ClientGameplayAuthorityError::invalid_route);
        return {ClientGameplayAction::relay_and_consume, ClientGameplayAuthorityError::none,
                message};
    }

    if (message == "MeleePlayerMessage")
    {
        if (inbound.recipient_gid == 0U)
        {
            if (inbound.channel != 0U)
                return reject(message, ClientGameplayAuthorityError::invalid_route);
            return {ClientGameplayAction::consume_on_host, ClientGameplayAuthorityError::none,
                    message};
        }
        if (inbound.channel != 1U || inbound.recipient_gid == inbound.sender_gid)
            return reject(message, ClientGameplayAuthorityError::invalid_route);
        return {ClientGameplayAction::relay, ClientGameplayAuthorityError::none, message};
    }

    if (!contains(client_broadcasts, message))
        return reject(message, ClientGameplayAuthorityError::unsupported_message);
    if (!broadcast_route(inbound))
        return reject(message, ClientGameplayAuthorityError::invalid_route);

    PayloadError payload_error{};
    if (message == "KillEnemyMessage")
    {
        KillEnemyPayload kill;
        if (!decode_kill_enemy_payload(decoded.payload, protocol, kill, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
        if (kill.killer_id != inbound.sender_gid)
            return reject(message, ClientGameplayAuthorityError::identity_spoof);
    }
    else if (message == "KillDragonMessage")
    {
        KillDragonPayload kill;
        if (!decode_kill_dragon_payload(decoded.payload, protocol, kill, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "PlayerExistsMessage")
    {
        PlayerExistsPayload presence;
        if (!decode_player_exists_payload(decoded.payload, presence, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
        if (presence.request_response)
            return reject(message, ClientGameplayAuthorityError::duplicate_presence);
    }
    else if (message == "AlterBlockMessage")
    {
        AlterBlockPayload edit;
        if (!decode_alter_block_payload(decoded.payload, protocol, edit, payload_error) ||
            !bounded_block_location(edit.block_location))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "ItemCrateMessage")
    {
        ItemCratePayload crate;
        if (!decode_item_crate_payload(decoded.payload, protocol, crate, payload_error) ||
            !bounded_block_location(crate.location))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "DestroyCrateMessage")
    {
        IntVector3 location;
        if (!decode_int_vector_payload(decoded.payload, location, payload_error) ||
            !bounded_block_location(location))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "DestroyCustomBlockMessage")
    {
        DestroyCustomBlockPayload custom;
        if (!decode_destroy_custom_block_payload(decoded.payload, custom, payload_error) ||
            !bounded_block_location(custom.location))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "RemoveBlocksMessage")
    {
        RemoveBlocksPayload removal;
        if (!decode_remove_blocks_payload(decoded.payload, removal, payload_error) ||
            removal.blocks.empty())
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
        for (const IntVector3 location : removal.blocks)
            if (!bounded_block_location(location))
                return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "DigMessage")
    {
        DigPayload dig;
        if (!decode_dig_payload(decoded.payload, protocol, dig, payload_error) ||
            !bounded_world_point(dig.location) || !face_normal(dig.direction))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "PlayerUpdateMessage")
    {
        PlayerUpdatePayload update;
        if (!decode_player_update_payload(decoded.payload, protocol, update, payload_error) ||
            update.local_position.x < -1'000'000.0F || update.local_position.x > 1'000'000.0F ||
            update.local_position.y < -64.0F || update.local_position.y >= 64.0F ||
            update.local_position.z < -1'000'000.0F || update.local_position.z > 1'000'000.0F)
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "SpawnEnemyMessage")
    {
        SpawnEnemyPayload spawn;
        if (!decode_spawn_enemy_payload(decoded.payload, protocol, spawn, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "EnemyGiveUpMessage" || message == "SpeedUpEnemyMessage")
    {
        EnemyTargetPayload target;
        if (!decode_enemy_target_payload(decoded.payload, target, payload_error) ||
            target.target_id != static_cast<std::int32_t>(inbound.sender_gid))
            return reject(message, ClientGameplayAuthorityError::identity_spoof);
    }
    else if (message == "UpdateDragonMessage")
    {
        DragonWaypointPayload waypoint;
        if (!decode_update_dragon_payload(decoded.payload, waypoint, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "DragonAttackMessage")
    {
        DragonAttackPayload attack;
        if (!decode_dragon_attack_payload(decoded.payload, attack, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
        const std::uint8_t encoded_owner = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(attack.fireball_index) >> 23U) & 0xFFU);
        if (encoded_owner != inbound.sender_gid)
            return reject(message, ClientGameplayAuthorityError::identity_spoof);
    }
    else if (message == "DetonateFireballMessage")
    {
        DetonateFireballPayload detonation;
        if (!decode_detonate_fireball_payload(decoded.payload, detonation, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
        const std::uint8_t encoded_owner = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(detonation.fireball_index) >> 23U) & 0xFFU);
        if (encoded_owner != inbound.sender_gid)
            return reject(message, ClientGameplayAuthorityError::identity_spoof);
        for (const IntVector3 location : detonation.blocks_to_remove)
            if (!bounded_block_location(location))
                return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "ExistingDragonMessage")
    {
        ExistingDragonPayload existing;
        if (!decode_existing_dragon_payload(decoded.payload, existing, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "MigrateDragonMessage")
    {
        MigrateDragonPayload migration;
        if (!decode_migrate_dragon_payload(decoded.payload, migration, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }

    return {ClientGameplayAction::relay, ClientGameplayAuthorityError::none, message};
}

std::string_view client_gameplay_authority_error_label(ClientGameplayAuthorityError error) noexcept
{
    switch (error)
    {
    case ClientGameplayAuthorityError::none:
        return "none";
    case ClientGameplayAuthorityError::invalid_packet:
        return "invalid-packet";
    case ClientGameplayAuthorityError::invalid_route:
        return "invalid-route";
    case ClientGameplayAuthorityError::host_only_message:
        return "host-only-message";
    case ClientGameplayAuthorityError::identity_spoof:
        return "identity-spoof";
    case ClientGameplayAuthorityError::duplicate_presence:
        return "duplicate-presence";
    case ClientGameplayAuthorityError::unsupported_message:
        return "unsupported-message";
    }
    return "unknown";
}

bool ClientGameplayRateLimiter::allow(PeerId peer, std::string_view message,
                                      std::uint64_t now_milliseconds)
{
    if (peer == 0U)
        return false;
    Window &window = windows_[peer];
    if (window.total == 0U || now_milliseconds - window.started_milliseconds >= 1'000U)
        window = Window{now_milliseconds};
    if (message == "SpawnerClearAlterBlockMessage")
    {
        if (window.spawner_block_edits_remaining == 0U)
            return false;
        --window.spawner_block_edits_remaining;
        return ++window.total <= 10'000U;
    }
    if (++window.total > 240U)
        return false;
    if (message == "AlterBlockMessage")
        return ++window.block_edits <= 10U;
    if (message == "DetonateGrenadeMessage" || message == "DetonateRocketMessage")
        return ++window.explosions <= 1U;
    if (message == "RemoveBlocksMessage")
        return ++window.block_removals <= 20U;
    if (message == "CreatePickupMessage")
        return ++window.pickup_creations <= 60U;
    if (message == "RequestPickupMessage")
        return ++window.pickup_requests <= 30U;
    if (message == "GunshotMessage")
        return ++window.gunshots <= 60U;
    if (message == "ShotgunShotMessage")
        return ++window.shotgun_shots <= 10U;
    if (message == "MeleePlayerMessage")
        return ++window.melee_attacks <= 12U;
    if (message == "GrenadeMessage" || message == "FireRocketMessage")
        return ++window.projectile_launches <= 4U;
    if (message == "DetonateExplosiveMessage")
        return ++window.placed_explosive_detonations <= 64U;
    if (message == "AddExplosionEffectsMessage" || message == "AddExplosiveFlashMessage")
        return ++window.explosive_effects <= 16U;
    if (message == "DigMessage")
        return ++window.dig_effects <= 12U;
    if (message == "DoorOpenCloseMessage")
        return ++window.door_events <= 12U;
    if (message == "DestroyCustomBlockMessage")
        return ++window.custom_block_events <= 16U;
    if (message == "SpawnEnemyMessage")
        return ++window.enemy_spawns <= 64U;
    if (message == "KillEnemyMessage" || message == "SpeedUpEnemyMessage" ||
        message == "EnemyGiveUpMessage")
        return ++window.enemy_events <= 30U;
    if (message == "UpdateDragonMessage")
        return ++window.dragon_updates <= 8U;
    if (message == "DetonateFireballMessage" || message == "DragonAttackMessage" ||
        message == "ExistingDragonMessage" || message == "KillDragonMessage" ||
        message == "MigrateDragonMessage" || message == "RemoveDragonMessage")
        return ++window.dragon_events <= 8U;
    if (message == "PlayerUpdateMessage")
        return ++window.player_updates <= 90U;
    if (message == "ChatMessage" || message == "BroadcastTextMessage")
        return ++window.text_messages <= 8U;
    if (message == "VoiceChatMessage")
        return ++window.voice_messages <= 120U;
    return true;
}

bool ClientGameplayRateLimiter::allow_spawner_trigger(PeerId peer, std::uint64_t now_milliseconds)
{
    if (peer == 0U)
        return false;
    Window &window = windows_[peer];
    if (window.total == 0U || now_milliseconds - window.started_milliseconds >= 1'000U)
        window = Window{now_milliseconds};
    if (++window.total > 10'000U || ++window.spawner_triggers > 64U)
        return false;
    constexpr std::uint16_t edits_per_trigger = 128U;
    constexpr std::uint16_t maximum_credit = 8'192U;
    window.spawner_block_edits_remaining = static_cast<std::uint16_t>(std::min<unsigned int>(
        maximum_credit,
        static_cast<unsigned int>(window.spawner_block_edits_remaining) + edits_per_trigger));
    return true;
}

void ClientGameplayRateLimiter::disconnect(PeerId peer) noexcept
{
    windows_.erase(peer);
}

void ClientGameplayRateLimiter::clear() noexcept
{
    windows_.clear();
}

} // namespace openminerz::net
