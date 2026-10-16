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
    // CastleMinerZGame._processRestartLevelMessage checks neither the sender
    // nor the game mode in 1.6.3.8 or 1.9.9.8, and the message echoes: every
    // peer that receives it, the host included, returns to the start, and the
    // sender has already restarted itself. The shipped menus only send it from
    // the host, but a stock host relays it from anybody. Refusing a guest's
    // restart here left the sender restarted and everyone else where they
    // were, which is the Endurance run split in two.
    "RestartLevelMessage"sv,
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
    "SpawnDragonMessage"sv,   "TimeOfDayMessage"sv,     "UpdateSpawnerMessage"sv,
    "WorldInfoMessage"sv,
};

ClientGameplayDecision reject(std::string_view message, ClientGameplayAuthorityError error) noexcept
{
    return {ClientGameplayAction::reject, error, message};
}

bool broadcast_route(const LegacySteamHostInbound &inbound) noexcept
{
    return inbound.channel == 1U && inbound.recipient_gid == 0U;
}

bool bounded_world_point(Vector3 location) noexcept
{
    return location.x >= -1'000'000.0F && location.x <= 1'000'000.0F && location.y >= -128.0F &&
           location.y <= 128.0F && location.z >= -1'000'000.0F && location.z <= 1'000'000.0F;
}

} // namespace

bool valid_network_block_location(IntVector3 location) noexcept
{
    return location.x >= -1'000'000 && location.x <= 1'000'000 && location.y >= -64 &&
           location.y < 64 && location.z >= -1'000'000 && location.z <= 1'000'000;
}

bool valid_network_surface_normal(Vector3 direction) noexcept
{
    const float length_squared = direction.x * direction.x + direction.y * direction.y +
                                 direction.z * direction.z;
    // XNA Plane.Intersects produces a normalized plane normal. Allow modest
    // float/collision-shape tolerance while rejecting zero and scaled vectors.
    return length_squared >= 0.81F && length_squared <= 1.21F;
}

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
        // KillerID is NOT the sender. TracerManager and Explosive apply every
        // replicated shot and blast to each peer's own copy of the horde with
        // the SHOOTER's id, and BaseZombie.TakeDamage/TakeExplosiveDamage send
        // `KillEnemyMessage(LocalPlayer.Gamer, EnemyID, Target.Gamer.Id,
        // shooterID, item)` from whichever peer's copy reached zero. Executed
        // against the owned assemblies, peer 3 observing peer 7's bullet emits
        // sender 3 / killer 7. Treating that as a spoof made this host refuse
        // to relay it: the enemy stayed dead for the observer, who had already
        // consumed its own echo, and alive for everyone else. Whether the
        // named killer is a real session member is checked where the roster
        // is known, exactly as it already is for KillDragonMessage.
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
            !valid_network_block_location(edit.block_location))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "ItemCrateMessage")
    {
        ItemCratePayload crate;
        if (!decode_item_crate_payload(decoded.payload, protocol, crate, payload_error) ||
            !valid_network_block_location(crate.location))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "DestroyCrateMessage")
    {
        IntVector3 location;
        if (!decode_int_vector_payload(decoded.payload, location, payload_error) ||
            !valid_network_block_location(location))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "DestroyCustomBlockMessage")
    {
        DestroyCustomBlockPayload custom;
        if (!decode_destroy_custom_block_payload(decoded.payload, custom, payload_error) ||
            !valid_network_block_location(custom.location))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "RemoveBlocksMessage")
    {
        RemoveBlocksPayload removal;
        // An EMPTY removal is ordinary stock traffic, not a malformed packet:
        // Explosive.FindBlocksToRemove ends with an unconditional
        // RemoveBlocksMessage.Send, so a grenade bursting in open air or a
        // laser bolt striking something it cannot break broadcasts zero cells.
        if (!decode_remove_blocks_payload(decoded.payload, removal, payload_error))
            return reject(message, ClientGameplayAuthorityError::invalid_packet);
        for (const IntVector3 location : removal.blocks)
            if (!valid_network_block_location(location))
                return reject(message, ClientGameplayAuthorityError::invalid_packet);
    }
    else if (message == "DigMessage")
    {
        DigPayload dig;
        if (!decode_dig_payload(decoded.payload, protocol, dig, payload_error) ||
            !bounded_world_point(dig.location) || !valid_network_surface_normal(dig.direction))
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
            if (!valid_network_block_location(location))
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
    // Every budget below is a hostile-peer ceiling, never a gameplay rule. The
    // original host relays without counting, the sender has already applied
    // its own local echo, and nothing is ever re-sent: a refusal here is a
    // permanent disagreement between the sender and the rest of the session.
    // Each number therefore has to clear the largest burst the ORIGINAL can
    // legitimately produce in one second - a chain reaction through a TNT
    // field, a whole pile of drops walked over in one frame, a horde enraged
    // at midnight - rather than what a player ordinarily does.
    if (++window.total > 8'192U)
        return false;
    // CastleMinerZGame.Update's anti-cheat sweep bans a peer that sends more
    // than ten AlterBlockMessages, or kicks one that sends more than twenty
    // RemoveBlocksMessages, inside its one-second _safetyCheckTimer window -
    // but _processAlterBlocksMessage APPLIES the edit either way and the sweep
    // only decides whether to ban afterwards. Refusing the edit instead, at the
    // original's own threshold, turned ordinary fast placing and mining into
    // "the block I just placed popped back": a single door is two edits and a
    // held mouse button clears ten a second easily. These stay purely as a
    // hostile-peer ceiling, set where real play cannot reach them.
    if (message == "AlterBlockMessage")
        return ++window.block_edits <= 120U;
    // One per second refused the second of two grenades thrown a second
    // apart (the fuse is measured from the throw, so cooking one of them lands
    // both inside a window) and any rocket that followed a grenade. Everyone
    // but the thrower then saw terrain vanish with no explosion and took no
    // splash damage.
    if (message == "DetonateGrenadeMessage" || message == "DetonateRocketMessage")
        return ++window.explosions <= 32U;
    if (message == "RemoveBlocksMessage")
        return ++window.block_removals <= 60U;
    // PickupManager publishes one message per stack, and a refused one destroys
    // the item: the sender has already debited its inventory or crate. The
    // budget must therefore exceed DropAll plus a multi-crate ejection.
    // One blast can open many crates at once, each ejecting up to 32 stacks.
    if (message == "CreatePickupMessage")
        return ++window.pickup_creations <= 4'096U;
    // CrateScreen's quick transfers rewrite up to 32 slots per keypress and a
    // refused slot leaves the crate permanently disagreeing between peers.
    if (message == "ItemCrateMessage")
        return ++window.crate_items <= 192U;
    if (message == "DestroyCrateMessage")
        return ++window.crate_destructions <= 512U;
    // PickupEntity.OnUpdate requests EVERY pickup within two units in the same
    // frame and marks each `_pickedUp` for good, and the original host answers
    // them all (96 simultaneous requests executed against the owned assembly
    // produce 96 grants). A death pile or two burst crates is far more than
    // thirty, and each refused request was an item that player could never
    // pick up again. Matches the creation budget above.
    if (message == "RequestPickupMessage")
        return ++window.pickup_requests <= 4'096U;
    if (message == "GunshotMessage")
        return ++window.gunshots <= 60U;
    if (message == "ShotgunShotMessage")
        return ++window.shotgun_shots <= 10U;
    if (message == "MeleePlayerMessage")
        return ++window.melee_attacks <= 12U;
    if (message == "GrenadeMessage" || message == "FireRocketMessage")
        return ++window.projectile_launches <= 16U;
    // Explosive.ProcessOneExplosion sends one of these for EVERY charge a
    // chain reaction reaches, all in the frame the first one goes off.
    if (message == "DetonateExplosiveMessage")
        return ++window.placed_explosive_detonations <= 4'096U;
    // InGameHUD.SetFuseForExplosive: one flash per charge a digging enemy arms.
    if (message == "AddExplosionEffectsMessage" || message == "AddExplosiveFlashMessage")
        return ++window.explosive_effects <= 256U;
    // One dig effect accompanies every swing that lands, so this tracks the
    // block-edit budget rather than the original's unrelated ban threshold.
    if (message == "DigMessage")
        return ++window.dig_effects <= 60U;
    if (message == "DoorOpenCloseMessage")
        return ++window.door_events <= 30U;
    if (message == "DestroyCustomBlockMessage")
        return ++window.custom_block_events <= 16U;
    if (message == "SpawnEnemyMessage")
        return ++window.enemy_spawns <= 64U;
    // ZombieChase re-sends SpeedUpEnemyMessage every frame until its own echo
    // has been applied, for every enemy chasing the local player, and
    // EnemyManager.ZombieFestIsOn trips all of them in the same frame at
    // midnight; a death makes every one of them give up at once, and one blast
    // kills a crowd. Thirty a second left the refused enemies slow, or alive,
    // for everybody except the player they were chasing.
    if (message == "KillEnemyMessage" || message == "SpeedUpEnemyMessage" ||
        message == "EnemyGiveUpMessage")
        return ++window.enemy_events <= 1'024U;
    // DragonEntity sends one waypoint per 0.2 s of DRAGON time, not of wall
    // time: a stock owner catching up after a hitch runs many fixed steps in
    // one frame, and the transport delivers what a stall held back in a burst.
    // Eight a second clipped those bursts; this is a hostile-rate guard only.
    if (message == "UpdateDragonMessage")
        return ++window.dragon_updates <= 240U;
    // These shared one budget of eight, which ordinary play exhausts:
    //  * the owner sends one ExistingDragonMessage per newcomer, and several
    //    players admitted together arrive together;
    //  * DragonClientEntity.TakeDamage sends KillDragonMessage for every hit
    //    that lands before its own kill has echoed back (each shotgun pellet,
    //    each queued remote shot), all in one frame;
    //  * reliable attacks and detonations arrive in bursts after a stall.
    // A refused detonation is terrain every other peer never loses.
    if (message == "DetonateFireballMessage" || message == "DragonAttackMessage" ||
        message == "ExistingDragonMessage" || message == "KillDragonMessage" ||
        message == "MigrateDragonMessage" || message == "RemoveDragonMessage")
        return ++window.dragon_events <= 1'024U;
    // CastleMinerZGame.SendNetworkUpdates sends one every frame while the
    // player is using a tool or reloading (otherwise once per roster-sized
    // frame cycle), so the rate is the stock client's frame rate: a 144 or
    // 240 Hz player mining or firing was clipped at 90.
    if (message == "PlayerUpdateMessage")
        return ++window.player_updates <= 1'024U;
    // The original puts no limit on restarts either. One a second is far more
    // than its menus can produce (a restart needs a dead session or a menu
    // visit) and still stops a hostile guest pinning everyone to the start
    // every frame.
    if (message == "RestartLevelMessage")
        return ++window.level_restarts <= 1U;
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
