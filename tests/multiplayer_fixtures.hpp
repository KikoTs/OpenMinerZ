#pragma once

#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/combat_codec.hpp"
#include "openminerz/net/dragon_codec.hpp"
#include "openminerz/net/enemy_codec.hpp"
#include "openminerz/net/misc_codec.hpp"
#include "openminerz/net/pickup_codec.hpp"
#include "openminerz/net/voice_codec.hpp"

#include <map>
#include <stdexcept>

namespace openminerz::test
{
using namespace net;
using PacketFixtures = std::map<std::string, std::vector<std::uint8_t>, std::less<>>;

inline PacketFixtures multiplayer_fixtures(ProtocolVersion protocol, std::uint8_t sender = 1U)
{
    PacketFixtures result;
    PayloadError error{};
    const auto add = [&](std::string name, std::optional<std::vector<std::uint8_t>> bytes) {
        if (!bytes)
            throw std::runtime_error{"Invalid fixture: " + name +
                                     " error=" + std::to_string(static_cast<int>(error))};
        result.emplace(std::move(name), std::move(*bytes));
    };
    const IntVector3 block{8, 3, -8};
    const Vector3 point{8.5F, 4.0F, -8.5F};
    const Vector3 forward{0.0F, 0.0F, -1.0F};
    const InventoryItemPayload item{2, 3, 1.0F}; // Original stone-block stack.
    const auto inventory = make_default_player_inventory(protocol, false);
    for (const auto name :
         {"ClientReadyForChunksMessage", "RequestInventoryMessage", "RequestWorldInfoMessage",
          "RemoveDragonMessage", "RestartLevelMessage"})
        add(name, std::vector<std::uint8_t>{});
    add("AppointServerMessage", std::vector<std::uint8_t>{0U});
    for (const auto name :
         {"AddExplosionEffectsMessage", "AddExplosiveFlashMessage", "DestroyCrateMessage"})
        add(name, encode_int_vector_payload(block, error));
    for (const auto name : {"ChatMessage", "BroadcastTextMessage"})
        add(name, encode_chat_payload("Packet coverage fixture", error));
    add("AlterBlockMessage", encode_alter_block_payload({block, 2}, protocol, error));
    add("ChangeCarriedItemMessage", std::vector<std::uint8_t>{50U, 0U});
    add("ConsumePickupMessage",
        encode_consume_pickup_payload({point, 7, sender, sender, item, true}, protocol, error));
    add("CrateFocusMessage", encode_crate_focus_payload({block, 2, 1}, error));
    add("CreatePickupMessage",
        encode_create_pickup_payload({point, forward, 7, item, true, true}, protocol, error));
    add("DetonateExplosiveMessage",
        encode_detonate_explosive_payload({block, true, 0U}, protocol, error));
    add("DetonateFireballMessage",
        encode_detonate_fireball_payload({point, static_cast<int>(sender) << 23, 0U, {block}},
                                         error));
    add("DetonateGrenadeMessage",
        encode_detonate_grenade_payload({point, 0U, true}, protocol, error));
    add("DetonateRocketMessage",
        encode_detonate_rocket_payload({point, false, 0U, 103U}, protocol, error));
    add("DigMessage", encode_dig_payload({false, point, {0.0F, 1.0F, 0.0F}, 2}, protocol, error));
    add("DoorOpenCloseMessage", encode_door_open_close_payload({block, true}, error));
    DragonWaypointPayload waypoint{point, forward, 3.0F, 0.1F, 0U, 0U};
    add("DragonAttackMessage",
        encode_dragon_attack_payload({waypoint, {10, 4, -10}, true, static_cast<int>(sender) << 23},
                                     error));
    add("EnemyGiveUpMessage", encode_enemy_target_payload({3, sender}, error));
    add("ExistingDragonMessage", encode_existing_dragon_payload({0U, 2U, true, 100.0F}, error));
    add("FireRocketMessage",
        encode_fire_rocket_payload({forward, point, false, 103}, protocol, error));
    add("GrenadeMessage", encode_grenade_payload({forward, point, 0U, 3.0F}, protocol, error));
    add("GunshotMessage", encode_gunshot_payload({forward, 50}, protocol, error));
    add("InventoryRetrieveFromServerMessage",
        encode_inventory_retrieve_payload({inventory, sender, false}, protocol, error));
    add("InventoryStoreOnServerMessage",
        encode_inventory_store_payload({inventory, true}, protocol, error));
    add("ItemCrateMessage", encode_item_crate_payload({block, 2, item}, protocol, error));
    add("KickMessage", encode_kick_payload({false, sender}, error));
    add("KillDragonMessage", encode_kill_dragon_payload({point, 50, sender}, protocol, error));
    add("KillEnemyMessage", encode_kill_enemy_payload({3, sender, 50, sender}, protocol, error));
    add("MeleePlayerMessage", encode_melee_player_payload({49, point}, protocol, error));
    MigrateDragonPayload migration;
    migration.migration.position = point;
    migration.migration.target = point;
    migration.target_gid = 2U;
    add("MigrateDragonMessage", encode_migrate_dragon_payload(migration, error));
    add("PlayerExistsMessage",
        encode_player_exists_payload({false, std::vector<std::uint8_t>(10), "Alice", {1U, 2U}},
                                     error));
    PlayerUpdatePayload update;
    update.local_position = point;
    update.torso_pitch = 128U;
    update.yaw = 64U;
    update.player_mode = 4U;
    update.packed_movement = 0xE7U;
    add("PlayerUpdateMessage", encode_player_update_payload(update, protocol, error));
    add("ProvideChunkMessage", encode_provide_chunk_payload({{0, -64, -16}, 1U, {}}, error));
    const std::array<std::int32_t, 2> deltas{0, 1};
    add("ProvideDeltaListMessage", encode_delta_list_payload(deltas, error));
    add("RemoveBlocksMessage", encode_remove_blocks_payload({{block}, true}, error));
    add("RequestChunkMessage", encode_request_chunk_payload({block, 1U}, error));
    add("RequestDragonMessage", encode_request_dragon_payload({0U, true}, error));
    add("RequestPickupMessage", encode_request_pickup_payload({7, sender}, error));
    ShotgunShotPayload shotgun;
    shotgun.directions.fill(forward);
    shotgun.item_id = 52;
    add("ShotgunShotMessage", encode_shotgun_shot_payload(shotgun, protocol, error));
    add("SpawnDragonMessage", encode_spawn_dragon_payload({0U, sender, true, 100.0F}, error));
    SpawnEnemyPayload enemy;
    enemy.spawn_position = point;
    if (protocol == ProtocolVersion::modern_v4)
    {
        enemy.spawner_position = point;
        enemy.player_name = "Alice";
    }
    enemy.enemy_id = 3;
    enemy.random_seed = 19;
    add("SpawnEnemyMessage", encode_spawn_enemy_payload(enemy, protocol, error));
    add("SpeedUpEnemyMessage", encode_enemy_target_payload({3, sender}, error));
    add("TimeConnectedMessage", encode_time_connected_payload(100'000'000, error));
    add("TimeOfDayMessage", encode_time_of_day_payload(0.4F, error));
    add("UpdateDragonMessage", encode_update_dragon_payload(waypoint, error));
    WorldInfoPayload world;
    world.seed = 1337;
    add("WorldInfoMessage", encode_world_info_payload(world, protocol, error));
    const std::array<std::uint8_t, voice_packet_samples> voice{};
    add("VoiceChatMessage", encode_voice_chat_payload(voice, error));
    if (protocol == ProtocolVersion::modern_v4)
    {
        add("DestroyCustomBlockMessage", encode_destroy_custom_block_payload({block, 1U}, error));
        add("UpdateSpawnerMessage", encode_update_spawner_payload({point, true}, error));
    }
    return result;
}
} // namespace openminerz::test
