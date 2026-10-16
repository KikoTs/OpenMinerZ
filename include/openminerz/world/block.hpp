#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace openminerz::world
{

// Numeric values are the original CastleMiner Z save/network block contract.
enum class BlockType : std::uint8_t
{
    empty = 0,
    dirt = 1,
    grass = 2,
    sand = 3,
    lantern = 4,
    fixed_lantern = 5,
    rock = 6,
    gold_ore = 7,
    iron_ore = 8,
    copper_ore = 9,
    coal_ore = 10,
    diamond_ore = 11,
    surface_lava = 12,
    deep_lava = 13,
    bedrock = 14,
    snow = 15,
    ice = 16,
    log = 17,
    leaves = 18,
    wood = 19,
    blood_stone = 20,
    space_rock = 21,
    iron_wall = 22,
    copper_wall = 23,
    golden_wall = 24,
    diamond_wall = 25,
    torch = 26,
    torch_pos_x = 27,
    torch_neg_z = 28,
    torch_neg_x = 29,
    torch_pos_z = 30,
    torch_pos_y = 31,
    torch_neg_y = 32,
    crate = 33,
    lower_door_closed_z = 34,
    lower_door_closed_x = 35,
    lower_door = 36,
    upper_door_closed = 37,
    lower_door_open_z = 38,
    lower_door_open_x = 39,
    upper_door_open = 40,
    tnt = 41,
    c4 = 42,
    slime = 43,
    space_rock_inventory = 44,
    glass_basic = 45,
    glass_iron = 46,
    glass_strong = 47,
    glass_mystery = 48,
    crate_stone = 49,
    crate_copper = 50,
    crate_iron = 51,
    crate_gold = 52,
    crate_diamond = 53,
    crate_bloodstone = 54,
    crate_safe = 55,
    spawn_point_basic = 56,
    spawn_point_builder = 57,
    spawn_point_combat = 58,
    spawn_point_explorer = 59,
    strong_lower_door_closed_z = 60,
    strong_lower_door_closed_x = 61,
    strong_lower_door = 62,
    strong_upper_door_closed = 63,
    strong_lower_door_open_z = 64,
    strong_lower_door_open_x = 65,
    strong_upper_door_open = 66,
    lantern_fancy = 67,
    turret_block = 68,
    loot_block = 69,
    lucky_loot_block = 70,
    bomb_block = 71,
    enemy_spawn_on = 72,
    enemy_spawn_off = 73,
    enemy_spawn_rare_on = 74,
    enemy_spawn_rare_off = 75,
    enemy_spawn_altar = 76,
    teleport_station = 77,
    crafting_station = 78,
    hell_forge = 79,
    alien_spawn_on = 80,
    alien_spawn_off = 81,
    hell_spawn_on = 82,
    hell_spawn_off = 83,
    boss_spawn_on = 84,
    boss_spawn_off = 85,
    enemy_spawn_dim = 86,
    enemy_spawn_rare_dim = 87,
    alien_spawn_dim = 88,
    hell_spawn_dim = 89,
    boss_spawn_dim = 90,
    alien_horde_on = 91,
    alien_horde_off = 92,
    alien_horde_dim = 93,
    uninitialized = 94,
};

enum class FaceDirection : std::uint8_t
{
    positive_x,
    negative_z,
    negative_x,
    positive_z,
    positive_y,
    negative_y,
};

struct BlockColor final
{
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
};

struct DoorModelTransform final
{
    float offset_x{};
    float offset_y{};
    float offset_z{};
    float yaw_radians{};
};

[[nodiscard]] bool is_solid(BlockType type) noexcept;
// The shipped BlockType.BlockPlayer column: what stops players, enemies,
// pickups and grenades. Surface lava IS solid in retail; deep lava is not.
[[nodiscard]] bool blocks_player(BlockType type) noexcept;
// The default-off "Sinking lava" enhancement makes surface lava pass-through
// (the former behaviour of this client). It is process-wide because every
// collision query in the simulation goes through blocks_player. Only the
// application sets it, from enhancement_active() in retail_parity.hpp, which
// never permits it in an online session; everything else leaves it false.
void set_surface_lava_passable(bool passable) noexcept;
[[nodiscard]] bool surface_lava_passable() noexcept;
[[nodiscard]] bool occludes(BlockType type) noexcept;
// Item-block entities (torches and doors) are rendered from their original
// models and therefore contribute no terrain-atlas cube faces.
[[nodiscard]] bool renders_as_voxel(BlockType type) noexcept;
// Original BlockType.CanBeDug. This is a property of the voxel, independent
// of whether the currently held tool can actually dig it.
[[nodiscard]] bool can_be_dug(BlockType type) noexcept;
// Original BlockType.BouncesLasers table used by BlasterShot.
[[nodiscard]] bool bounces_lasers(BlockType type) noexcept;
// Original BlockType.CanBuildOn and Facing contracts. Facing is populated by
// attachable variants (wall/floor/ceiling torches and lower door halves).
[[nodiscard]] bool can_build_on(BlockType type) noexcept;
[[nodiscard]] std::optional<FaceDirection> attached_face(BlockType type) noexcept;
// Original BlockType.IsContainer. CrateSafe deliberately is not storage.
[[nodiscard]] bool is_container(BlockType type) noexcept;
// BlockType.IsStructure (1.9.9.8 only): glass, spawn points, teleport station.
// Any held item digs these in two seconds.
[[nodiscard]] bool is_structure(BlockType type) noexcept;
[[nodiscard]] bool is_lower_door(BlockType type) noexcept;
[[nodiscard]] bool is_upper_door(BlockType type) noexcept;
[[nodiscard]] bool is_strong_door(BlockType type) noexcept;
// DoorInventoryitem.GetDoorPiece: translate a classic door piece into the
// corresponding wood or strong-family voxel. Invalid requested pieces are empty.
[[nodiscard]] BlockType door_piece(BlockType base_door, BlockType classic_piece) noexcept;
// DoorEntity.SetPosition, expressed as the child-model transform relative to
// the lower door voxel. Upper halves do not own a model entity.
[[nodiscard]] std::optional<DoorModelTransform> door_model_transform(BlockType type) noexcept;
[[nodiscard]] bool is_spawner(BlockType type) noexcept;
[[nodiscard]] bool is_active_spawner(BlockType type) noexcept;
[[nodiscard]] bool emits_light(BlockType type) noexcept;
// Original BlockType.LightTransmission (0..16): how much light passes this cell.
[[nodiscard]] std::uint8_t light_transmission(BlockType type) noexcept;
// Original RenderChunk.LightAsTranslucent: solid blocks that still pass light
// (leaves, ice). Their faces take at least the block's own light level.
[[nodiscard]] bool lights_as_translucent(BlockType type) noexcept;
// Original BlockType.SelfIllumination (0..15): torch-channel light this cell emits.
[[nodiscard]] std::uint8_t self_illumination(BlockType type) noexcept;
// Original BlockType.DamageTransmision consumed by dragon DamageLOSProbe.
[[nodiscard]] float damage_transmission(BlockType type) noexcept;
[[nodiscard]] float bare_hand_break_seconds(BlockType type) noexcept;
// InventoryItem.CreatesWhenDug from the original game. Empty means the mined
// block intentionally has no collectible block-form drop.
[[nodiscard]] BlockType mined_drop(BlockType type) noexcept;
[[nodiscard]] std::uint8_t atlas_tile(BlockType type, FaceDirection face) noexcept;
[[nodiscard]] std::array<std::array<float, 2>, 4> atlas_uv_corners(FaceDirection face, float u0,
                                                                   float v0, float u1,
                                                                   float v1) noexcept;
[[nodiscard]] BlockColor fallback_color(BlockType type) noexcept;
// BlockType.Name as shipped ("Gold Ore", "Diamonds", "Golden Wall", "Monster
// Spawner (Right click to activate)"): the text of the original's aimed-block
// label, and the name every block item takes. Mixed case, generated from the
// owned installs; see block.cpp for the oddities it deliberately keeps.
[[nodiscard]] std::string_view block_name(BlockType type) noexcept;

} // namespace openminerz::world
