#include "openminerz/world/block.hpp"

#include <limits>

namespace openminerz::world
{

bool is_solid(BlockType type) noexcept
{
    return type != BlockType::empty && type != BlockType::uninitialized;
}

bool blocks_player(BlockType type) noexcept
{
    // A rendered/touchable voxel is not necessarily a full collision cube. This mirrors the
    // original BlockPlayer contract, with both lava variants intentionally pass-through for the
    // Rebirth gameplay rule requested by the project owner.
    switch (type)
    {
    case BlockType::empty:
    case BlockType::surface_lava:
    case BlockType::deep_lava:
    case BlockType::leaves:
    case BlockType::torch:
    case BlockType::torch_pos_x:
    case BlockType::torch_neg_z:
    case BlockType::torch_neg_x:
    case BlockType::torch_pos_z:
    case BlockType::torch_pos_y:
    case BlockType::torch_neg_y:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::upper_door_open:
    case BlockType::strong_lower_door_open_z:
    case BlockType::strong_lower_door_open_x:
    case BlockType::strong_upper_door_open:
    case BlockType::uninitialized:
        return false;
    default:
        return true;
    }
}

bool can_be_dug(BlockType type) noexcept
{
    // The original table disables digging only for air, the fixed lantern,
    // bedrock and the NumberOfBlocks sentinel. Ores and both lava variants are
    // still CanBeDug; the held item decides whether the attempt can succeed.
    return type != BlockType::empty && type != BlockType::fixed_lantern &&
           type != BlockType::bedrock && type != BlockType::uninitialized;
}

bool bounces_lasers(BlockType type) noexcept
{
    return type == BlockType::bedrock || type == BlockType::blood_stone ||
           type == BlockType::diamond_wall ||
           (type >= BlockType::glass_basic && type <= BlockType::glass_mystery);
}

bool occludes(BlockType type) noexcept
{
    if (!is_solid(type) || light_transmission(type) != 0U)
        return false;
    switch (type)
    {
    case BlockType::leaves:
    case BlockType::ice:
    case BlockType::surface_lava:
    case BlockType::deep_lava:
    // Torches and doors are thin fixtures, not full cubes: they must not cull
    // the faces behind them, darken corner occlusion, or count as column tops.
    case BlockType::torch:
    case BlockType::torch_pos_x:
    case BlockType::torch_neg_z:
    case BlockType::torch_neg_x:
    case BlockType::torch_pos_z:
    case BlockType::torch_pos_y:
    case BlockType::torch_neg_y:
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::upper_door_open:
    case BlockType::glass_basic:
    case BlockType::glass_iron:
    case BlockType::glass_strong:
    case BlockType::glass_mystery:
    case BlockType::strong_lower_door_closed_z:
    case BlockType::strong_lower_door_closed_x:
    case BlockType::strong_lower_door:
    case BlockType::strong_upper_door_closed:
    case BlockType::strong_lower_door_open_z:
    case BlockType::strong_lower_door_open_x:
    case BlockType::strong_upper_door_open:
        return false;
    default:
        return true;
    }
}

bool renders_as_voxel(BlockType type) noexcept
{
    if (type >= BlockType::torch && type <= BlockType::torch_neg_y)
        return false;
    return !is_lower_door(type) && !is_upper_door(type);
}

bool can_build_on(BlockType type) noexcept
{
    // BlockType.blockTypes: empty, torches and both door halves are the only
    // ordinary cells that reject construction on their selected face.
    if (type == BlockType::empty || type == BlockType::uninitialized)
        return false;
    if (type >= BlockType::torch && type <= BlockType::torch_neg_y)
        return false;
    return !is_lower_door(type) && !is_upper_door(type);
}

bool is_container(BlockType type) noexcept
{
    // BlockType.IsContainer in 1.9.9.8 checks these seven numeric values. The
    // unused CrateSafe block (55) intentionally does not participate.
    return type == BlockType::crate || type == BlockType::crate_stone ||
           type == BlockType::crate_copper || type == BlockType::crate_iron ||
           type == BlockType::crate_gold || type == BlockType::crate_diamond ||
           type == BlockType::crate_bloodstone;
}

std::optional<FaceDirection> attached_face(BlockType type) noexcept
{
    switch (type)
    {
    case BlockType::torch_pos_x:
        return FaceDirection::positive_x;
    case BlockType::torch_neg_z:
        return FaceDirection::negative_z;
    case BlockType::torch_neg_x:
        return FaceDirection::negative_x;
    case BlockType::torch_pos_z:
        return FaceDirection::positive_z;
    case BlockType::torch_pos_y:
        return FaceDirection::positive_y;
    case BlockType::torch_neg_y:
        return FaceDirection::negative_y;
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::strong_lower_door_closed_z:
    case BlockType::strong_lower_door_closed_x:
    case BlockType::strong_lower_door_open_z:
    case BlockType::strong_lower_door_open_x:
        return FaceDirection::positive_y;
    default:
        return std::nullopt;
    }
}

bool is_lower_door(BlockType type) noexcept
{
    return type == BlockType::lower_door_closed_z || type == BlockType::lower_door_closed_x ||
           type == BlockType::lower_door || type == BlockType::lower_door_open_z ||
           type == BlockType::lower_door_open_x || type == BlockType::strong_lower_door_closed_z ||
           type == BlockType::strong_lower_door_closed_x || type == BlockType::strong_lower_door ||
           type == BlockType::strong_lower_door_open_z ||
           type == BlockType::strong_lower_door_open_x;
}

bool is_upper_door(BlockType type) noexcept
{
    return type == BlockType::upper_door_closed || type == BlockType::upper_door_open ||
           type == BlockType::strong_upper_door_closed || type == BlockType::strong_upper_door_open;
}

bool is_strong_door(BlockType type) noexcept
{
    switch (type)
    {
    case BlockType::strong_lower_door_closed_z:
    case BlockType::strong_lower_door_closed_x:
    case BlockType::strong_lower_door:
    case BlockType::strong_upper_door_closed:
    case BlockType::strong_lower_door_open_z:
    case BlockType::strong_lower_door_open_x:
    case BlockType::strong_upper_door_open:
        return true;
    default:
        return false;
    }
}

BlockType door_piece(BlockType base_door, BlockType classic_piece) noexcept
{
    if (base_door == BlockType::lower_door ||
        ((is_lower_door(base_door) || is_upper_door(base_door)) && !is_strong_door(base_door)))
    {
        switch (classic_piece)
        {
        case BlockType::lower_door_closed_z:
        case BlockType::lower_door_closed_x:
        case BlockType::upper_door_closed:
        case BlockType::lower_door_open_z:
        case BlockType::lower_door_open_x:
        case BlockType::upper_door_open:
            return classic_piece;
        default:
            return BlockType::empty;
        }
    }
    if (!is_strong_door(base_door))
        return BlockType::empty;
    switch (classic_piece)
    {
    case BlockType::lower_door_closed_z:
        return BlockType::strong_lower_door_closed_z;
    case BlockType::lower_door_closed_x:
        return BlockType::strong_lower_door_closed_x;
    case BlockType::upper_door_closed:
        return BlockType::strong_upper_door_closed;
    case BlockType::lower_door_open_z:
        return BlockType::strong_lower_door_open_z;
    case BlockType::lower_door_open_x:
        return BlockType::strong_lower_door_open_x;
    case BlockType::upper_door_open:
        return BlockType::strong_upper_door_open;
    default:
        return BlockType::empty;
    }
}

std::optional<DoorModelTransform> door_model_transform(BlockType type) noexcept
{
    constexpr float half_pi = 1.5707964F;
    switch (type)
    {
    case BlockType::lower_door_closed_x:
    case BlockType::strong_lower_door_closed_x:
        return DoorModelTransform{-0.5F, -0.5F, 0.0F, 0.0F};
    case BlockType::lower_door_open_x:
    case BlockType::strong_lower_door_open_x:
        return DoorModelTransform{-0.5F, -0.5F, 0.0F, half_pi};
    case BlockType::lower_door_closed_z:
    case BlockType::strong_lower_door_closed_z:
        return DoorModelTransform{0.0F, -0.5F, -0.5F, -half_pi};
    case BlockType::lower_door_open_z:
    case BlockType::strong_lower_door_open_z:
        return DoorModelTransform{0.0F, -0.5F, -0.5F, 0.0F};
    default:
        return std::nullopt;
    }
}

bool is_spawner(BlockType type) noexcept
{
    switch (type)
    {
    case BlockType::enemy_spawn_on:
    case BlockType::enemy_spawn_off:
    case BlockType::enemy_spawn_rare_on:
    case BlockType::enemy_spawn_rare_off:
    case BlockType::alien_spawn_on:
    case BlockType::alien_spawn_off:
    case BlockType::hell_spawn_on:
    case BlockType::hell_spawn_off:
    case BlockType::boss_spawn_on:
    case BlockType::boss_spawn_off:
    case BlockType::enemy_spawn_dim:
    case BlockType::enemy_spawn_rare_dim:
    case BlockType::alien_spawn_dim:
    case BlockType::hell_spawn_dim:
    case BlockType::boss_spawn_dim:
    case BlockType::alien_horde_on:
    case BlockType::alien_horde_off:
    case BlockType::alien_horde_dim:
        return true;
    default:
        return false;
    }
}

bool is_active_spawner(BlockType type) noexcept
{
    return type == BlockType::enemy_spawn_on || type == BlockType::enemy_spawn_rare_on ||
           type == BlockType::alien_spawn_on || type == BlockType::hell_spawn_on ||
           type == BlockType::boss_spawn_on || type == BlockType::alien_horde_on;
}

bool emits_light(BlockType type) noexcept
{
    return self_illumination(type) != 0U;
}

float damage_transmission(BlockType type) noexcept
{
    switch (type)
    {
    case BlockType::dirt:
    case BlockType::grass:
    case BlockType::log:
    case BlockType::wood:
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
    case BlockType::strong_lower_door_closed_z:
    case BlockType::strong_lower_door_closed_x:
    case BlockType::strong_lower_door:
    case BlockType::strong_upper_door_closed:
        return 0.8F;
    case BlockType::sand:
        return 0.7F;
    case BlockType::rock:
    case BlockType::gold_ore:
    case BlockType::iron_ore:
    case BlockType::copper_ore:
    case BlockType::coal_ore:
        return 0.5F;
    case BlockType::diamond_ore:
        return 0.4F;
    case BlockType::bedrock:
    case BlockType::iron_wall:
    case BlockType::copper_wall:
    case BlockType::golden_wall:
        return 0.3F;
    case BlockType::blood_stone:
    case BlockType::diamond_wall:
    case BlockType::glass_basic:
    case BlockType::glass_iron:
    case BlockType::glass_strong:
    case BlockType::glass_mystery:
        return 0.2F;
    case BlockType::space_rock:
    case BlockType::space_rock_inventory:
        return 0.1F;
    case BlockType::ice:
        return 0.9F;
    default:
        return 1.0F;
    }
}

std::uint8_t light_transmission(BlockType type) noexcept
{
    // BlockType.blockTypes: air/torches/doors transmit fully (16), ice 0.9 (14),
    // leaves 0.4 (6); every opaque cube and both lavas transmit nothing.
    switch (type)
    {
    case BlockType::empty:
    case BlockType::uninitialized:
    case BlockType::torch:
    case BlockType::torch_pos_x:
    case BlockType::torch_neg_z:
    case BlockType::torch_neg_x:
    case BlockType::torch_pos_z:
    case BlockType::torch_pos_y:
    case BlockType::torch_neg_y:
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::upper_door_open:
        return 16U;
    case BlockType::glass_basic:
    case BlockType::glass_iron:
        return 12U;
    case BlockType::glass_strong:
    case BlockType::glass_mystery:
        return 15U;
    case BlockType::spawn_point_builder:
    case BlockType::spawn_point_combat:
    case BlockType::spawn_point_explorer:
    case BlockType::teleport_station:
    case BlockType::crafting_station:
    case BlockType::hell_forge:
        return 11U;
    case BlockType::strong_upper_door_closed:
        return 8U;
    case BlockType::strong_lower_door_open_z:
    case BlockType::strong_lower_door_open_x:
    case BlockType::strong_upper_door_open:
        return 16U;
    case BlockType::ice:
        return 14U;
    case BlockType::leaves:
        return 6U;
    default:
        return 0U;
    }
}

bool lights_as_translucent(BlockType type) noexcept
{
    // The authored flag is narrower than non-zero light transmission: respawn
    // beacons and stations transmit light without using translucent face light.
    return type == BlockType::leaves || type == BlockType::ice ||
           (type >= BlockType::glass_basic && type <= BlockType::glass_mystery);
}

std::uint8_t self_illumination(BlockType type) noexcept
{
    // Torches, lanterns, both lavas and space goo carry SelfIllumination 1.0 (15)
    // in the original block table; nothing else emits.
    switch (type)
    {
    case BlockType::lantern:
    case BlockType::fixed_lantern:
    case BlockType::surface_lava:
    case BlockType::deep_lava:
    case BlockType::slime:
    case BlockType::torch:
    case BlockType::torch_pos_x:
    case BlockType::torch_neg_z:
    case BlockType::torch_neg_x:
    case BlockType::torch_pos_z:
    case BlockType::torch_pos_y:
    case BlockType::torch_neg_y:
    case BlockType::lantern_fancy:
    case BlockType::turret_block:
    case BlockType::bomb_block:
        return 15U;
    case BlockType::spawn_point_basic:
        return 10U;
    case BlockType::loot_block:
    case BlockType::lucky_loot_block:
        return 8U;
    case BlockType::enemy_spawn_on:
    case BlockType::enemy_spawn_rare_on:
    case BlockType::alien_spawn_on:
    case BlockType::hell_spawn_on:
    case BlockType::boss_spawn_on:
    case BlockType::alien_horde_on:
        return 6U;
    case BlockType::enemy_spawn_altar:
        return 3U;
    case BlockType::boss_spawn_off:
        return 13U;
    default:
        return 0U;
    }
}

float bare_hand_break_seconds(BlockType type) noexcept
{
    // InventoryItem.TimeToDig from the original bare-hand/base-item contract.
    // Its first branch returns two seconds for every IsContainer block, before
    // considering any of the ordinary material cases below.
    if (is_container(type))
        return 2.0F;
    switch (type)
    {
    case BlockType::dirt:
    case BlockType::grass:
        return 1.5F;
    case BlockType::sand:
    case BlockType::snow:
    case BlockType::leaves:
        return 1.0F;
    case BlockType::lantern:
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::upper_door_open:
    case BlockType::strong_lower_door_closed_z:
    case BlockType::strong_lower_door_closed_x:
    case BlockType::strong_lower_door:
    case BlockType::strong_upper_door_closed:
    case BlockType::strong_lower_door_open_z:
    case BlockType::strong_lower_door_open_x:
    case BlockType::strong_upper_door_open:
    case BlockType::lantern_fancy:
        return 2.0F;
    case BlockType::rock:
        return 10.0F;
    case BlockType::ice:
        return 5.0F;
    case BlockType::log:
        return 4.0F;
    case BlockType::wood:
        return 3.0F;
    case BlockType::surface_lava:
    case BlockType::torch:
    case BlockType::torch_pos_x:
    case BlockType::torch_neg_z:
    case BlockType::torch_neg_x:
    case BlockType::torch_pos_z:
    case BlockType::torch_pos_y:
    case BlockType::torch_neg_y:
        return 0.0F;
    default:
        return std::numeric_limits<float>::infinity();
    }
}

BlockType mined_drop(BlockType type) noexcept
{
    switch (type)
    {
    case BlockType::empty:
    case BlockType::uninitialized:
    case BlockType::deep_lava:
    case BlockType::bedrock:
        return BlockType::empty;
    case BlockType::grass:
        return BlockType::dirt;
    case BlockType::surface_lava:
        return BlockType::rock;
    case BlockType::space_rock:
        return BlockType::space_rock_inventory;
    default:
        return type;
    }
}

std::uint8_t atlas_tile(BlockType type, FaceDirection face) noexcept
{
    switch (type)
    {
    case BlockType::dirt:
        return 0U;
    case BlockType::grass:
        if (face == FaceDirection::positive_y)
        {
            return 2U;
        }
        return face == FaceDirection::negative_y ? 0U : 1U;
    case BlockType::sand:
        return 3U;
    case BlockType::lantern:
    case BlockType::fixed_lantern:
        return 4U;
    case BlockType::rock:
        return 5U;
    case BlockType::gold_ore:
        return 6U;
    case BlockType::iron_ore:
        return 7U;
    case BlockType::copper_ore:
        return 8U;
    case BlockType::coal_ore:
        return 9U;
    case BlockType::diamond_ore:
        return 10U;
    case BlockType::surface_lava:
    case BlockType::deep_lava:
        return 11U;
    case BlockType::bedrock:
        return 12U;
    case BlockType::snow:
        return 13U;
    case BlockType::ice:
        return 14U;
    case BlockType::log:
        return face == FaceDirection::positive_y || face == FaceDirection::negative_y ? 16U : 15U;
    case BlockType::leaves:
        return 17U;
    case BlockType::wood:
        return 18U;
    case BlockType::blood_stone:
        return 19U;
    case BlockType::space_rock:
    case BlockType::space_rock_inventory:
        return 20U;
    case BlockType::iron_wall:
        return 21U;
    case BlockType::copper_wall:
        return 22U;
    case BlockType::golden_wall:
        return 23U;
    case BlockType::diamond_wall:
        return 24U;
    case BlockType::crate:
        return 25U;
    case BlockType::slime:
        return 26U;
    case BlockType::tnt:
        return face == FaceDirection::positive_y || face == FaceDirection::negative_y ? 28U : 27U;
    case BlockType::c4:
        return face == FaceDirection::positive_y || face == FaceDirection::negative_y ? 30U : 29U;
    case BlockType::glass_basic:
        return 32U;
    case BlockType::glass_iron:
        return 33U;
    case BlockType::glass_strong:
    case BlockType::glass_mystery:
        return 31U;
    case BlockType::crate_stone:
    case BlockType::crate_safe:
        return 34U;
    case BlockType::crate_copper:
        return 35U;
    case BlockType::crate_iron:
        return 36U;
    case BlockType::crate_gold:
        return 37U;
    case BlockType::crate_diamond:
        return 38U;
    case BlockType::crate_bloodstone:
        return 39U;
    case BlockType::spawn_point_basic:
    case BlockType::spawn_point_builder:
    case BlockType::spawn_point_combat:
    case BlockType::spawn_point_explorer:
    case BlockType::crafting_station:
    case BlockType::hell_forge:
        return 40U;
    case BlockType::lantern_fancy:
        return 41U;
    case BlockType::turret_block:
    case BlockType::bomb_block:
        return 26U;
    case BlockType::loot_block:
        return 42U;
    case BlockType::lucky_loot_block:
        return 43U;
    case BlockType::teleport_station:
        return 44U;
    case BlockType::enemy_spawn_rare_on:
    case BlockType::enemy_spawn_rare_off:
    case BlockType::enemy_spawn_altar:
    case BlockType::enemy_spawn_rare_dim:
        return 45U;
    case BlockType::enemy_spawn_on:
    case BlockType::enemy_spawn_off:
    case BlockType::enemy_spawn_dim:
        return 46U;
    case BlockType::alien_spawn_on:
    case BlockType::alien_spawn_off:
    case BlockType::alien_spawn_dim:
    case BlockType::alien_horde_on:
    case BlockType::alien_horde_off:
        return 48U;
    case BlockType::hell_spawn_on:
    case BlockType::hell_spawn_off:
    case BlockType::hell_spawn_dim:
        return 49U;
    case BlockType::boss_spawn_on:
    case BlockType::boss_spawn_off:
    case BlockType::boss_spawn_dim:
    case BlockType::alien_horde_dim:
        return 50U;
    default:
        return 5U;
    }
}

std::array<std::array<float, 2>, 4> atlas_uv_corners(FaceDirection face, float u0, float v0,
                                                     float u1, float v1) noexcept
{
    // CastleMiner Z's BlockVertex assigns UVs in face-local vertex order. Our mesh corners use a
    // winding-friendly order, so each face needs this explicit remap to keep tops horizontal and
    // the grass strip at the upper edge of every side instead of rotating it onto two walls.
    switch (face)
    {
    case FaceDirection::positive_x:
    case FaceDirection::negative_x:
        return {{{{u1, v1}}, {{u1, v0}}, {{u0, v0}}, {{u0, v1}}}};
    case FaceDirection::negative_z:
    case FaceDirection::positive_z:
        return {{{{u0, v1}}, {{u1, v1}}, {{u1, v0}}, {{u0, v0}}}};
    case FaceDirection::positive_y:
        return {{{{u0, v0}}, {{u1, v0}}, {{u1, v1}}, {{u0, v1}}}};
    case FaceDirection::negative_y:
        return {{{{u1, v1}}, {{u0, v1}}, {{u0, v0}}, {{u1, v0}}}};
    }
    return {{{{u0, v1}}, {{u1, v1}}, {{u1, v0}}, {{u0, v0}}}};
}

BlockColor fallback_color(BlockType type) noexcept
{
    switch (type)
    {
    case BlockType::dirt:
        return {104U, 72U, 45U};
    case BlockType::grass:
        return {76U, 122U, 48U};
    case BlockType::sand:
        return {196U, 174U, 103U};
    case BlockType::lantern:
    case BlockType::fixed_lantern:
        return {255U, 211U, 68U};
    case BlockType::rock:
        return {91U, 95U, 91U};
    case BlockType::gold_ore:
        return {196U, 158U, 47U};
    case BlockType::iron_ore:
        return {154U, 143U, 128U};
    case BlockType::copper_ore:
        return {164U, 91U, 54U};
    case BlockType::coal_ore:
        return {48U, 50U, 48U};
    case BlockType::diamond_ore:
        return {74U, 195U, 201U};
    case BlockType::surface_lava:
    case BlockType::deep_lava:
        return {238U, 74U, 21U};
    case BlockType::bedrock:
        return {43U, 45U, 46U};
    case BlockType::snow:
        return {221U, 232U, 233U};
    case BlockType::ice:
        return {117U, 183U, 207U};
    case BlockType::log:
        return {98U, 68U, 39U};
    case BlockType::leaves:
        return {48U, 94U, 39U};
    case BlockType::wood:
        return {132U, 91U, 49U};
    case BlockType::blood_stone:
        return {101U, 24U, 29U};
    case BlockType::space_rock:
    case BlockType::space_rock_inventory:
        return {72U, 75U, 79U};
    case BlockType::iron_wall:
        return {121U, 128U, 131U};
    case BlockType::copper_wall:
        return {156U, 91U, 58U};
    case BlockType::golden_wall:
        return {207U, 169U, 47U};
    case BlockType::diamond_wall:
        return {70U, 183U, 191U};
    case BlockType::crate:
        return {128U, 86U, 45U};
    case BlockType::tnt:
        return {181U, 48U, 36U};
    case BlockType::c4:
        return {77U, 82U, 78U};
    case BlockType::slime:
        return {87U, 170U, 59U};
    case BlockType::glass_basic:
        return {164U, 139U, 102U};
    case BlockType::glass_iron:
        return {123U, 137U, 143U};
    case BlockType::glass_strong:
        return {112U, 166U, 181U};
    case BlockType::glass_mystery:
        return {171U, 202U, 210U};
    case BlockType::crate_stone:
    case BlockType::crate_safe:
        return {96U, 98U, 96U};
    case BlockType::crate_copper:
        return {156U, 91U, 58U};
    case BlockType::crate_iron:
        return {121U, 128U, 131U};
    case BlockType::crate_gold:
        return {207U, 169U, 47U};
    case BlockType::crate_diamond:
        return {70U, 183U, 191U};
    case BlockType::crate_bloodstone:
        return {101U, 24U, 29U};
    case BlockType::loot_block:
        return {91U, 168U, 208U};
    case BlockType::lucky_loot_block:
        return {229U, 188U, 55U};
    default:
        return {115U, 119U, 116U};
    }
}

std::string_view block_name(BlockType type) noexcept
{
    switch (type)
    {
    case BlockType::empty:
        return "AIR";
    case BlockType::dirt:
        return "DIRT";
    case BlockType::grass:
        return "GRASS";
    case BlockType::sand:
        return "SAND";
    case BlockType::lantern:
        return "LANTERN";
    case BlockType::fixed_lantern:
        return "FIXED LANTERN";
    case BlockType::rock:
        return "ROCK";
    case BlockType::gold_ore:
        return "GOLD ORE";
    case BlockType::iron_ore:
        return "IRON ORE";
    case BlockType::copper_ore:
        return "COPPER ORE";
    case BlockType::coal_ore:
        return "COAL ORE";
    case BlockType::diamond_ore:
        return "DIAMOND ORE";
    case BlockType::surface_lava:
        return "SURFACE LAVA";
    case BlockType::deep_lava:
        return "DEEP LAVA";
    case BlockType::snow:
        return "SNOW";
    case BlockType::ice:
        return "ICE";
    case BlockType::log:
        return "LOG";
    case BlockType::leaves:
        return "LEAVES";
    case BlockType::wood:
        return "WOOD";
    case BlockType::bedrock:
        return "BEDROCK";
    case BlockType::blood_stone:
        return "BLOOD STONE";
    case BlockType::space_rock:
    case BlockType::space_rock_inventory:
        return "SPACE ROCK";
    case BlockType::iron_wall:
        return "IRON WALL";
    case BlockType::copper_wall:
        return "COPPER WALL";
    case BlockType::golden_wall:
        return "GOLD WALL";
    case BlockType::diamond_wall:
        return "DIAMOND WALL";
    case BlockType::torch:
    case BlockType::torch_pos_x:
    case BlockType::torch_neg_z:
    case BlockType::torch_neg_x:
    case BlockType::torch_pos_z:
    case BlockType::torch_pos_y:
    case BlockType::torch_neg_y:
        return "TORCH";
    case BlockType::crate:
        return "CRATE";
    case BlockType::lower_door_closed_z:
    case BlockType::lower_door_closed_x:
    case BlockType::lower_door:
    case BlockType::upper_door_closed:
    case BlockType::lower_door_open_z:
    case BlockType::lower_door_open_x:
    case BlockType::upper_door_open:
        return "DOOR";
    case BlockType::tnt:
        return "TNT";
    case BlockType::c4:
        return "C4";
    case BlockType::slime:
        return "SLIME";
    case BlockType::glass_basic:
        return "STICK WINDOW";
    case BlockType::glass_iron:
        return "IRON WINDOW";
    case BlockType::glass_strong:
        return "BULLETPROOF GLASS";
    case BlockType::glass_mystery:
        return "CLEAR GLASS";
    case BlockType::crate_stone:
        return "STONE CRATE";
    case BlockType::crate_copper:
        return "COPPER CRATE";
    case BlockType::crate_iron:
        return "IRON CRATE";
    case BlockType::crate_gold:
        return "GOLD CRATE";
    case BlockType::crate_diamond:
        return "DIAMOND CRATE";
    case BlockType::crate_bloodstone:
        return "BLOODSTONE CRATE";
    case BlockType::crate_safe:
        return "SAFE CRATE";
    case BlockType::spawn_point_basic:
    case BlockType::spawn_point_builder:
    case BlockType::spawn_point_combat:
    case BlockType::spawn_point_explorer:
        return "RESPAWN BEACON";
    case BlockType::strong_lower_door_closed_z:
    case BlockType::strong_lower_door_closed_x:
    case BlockType::strong_lower_door:
    case BlockType::strong_upper_door_closed:
    case BlockType::strong_lower_door_open_z:
    case BlockType::strong_lower_door_open_x:
    case BlockType::strong_upper_door_open:
        return "STRONG DOOR";
    case BlockType::lantern_fancy:
        return "FANCY LANTERN";
    case BlockType::turret_block:
        return "TURRET BLOCK";
    case BlockType::loot_block:
        return "LOOT BLOCK";
    case BlockType::lucky_loot_block:
        return "LUCKY LOOT BLOCK";
    case BlockType::bomb_block:
        return "BOMB BLOCK";
    case BlockType::enemy_spawn_on:
    case BlockType::enemy_spawn_off:
    case BlockType::enemy_spawn_rare_on:
    case BlockType::enemy_spawn_rare_off:
    case BlockType::enemy_spawn_altar:
    case BlockType::alien_spawn_on:
    case BlockType::alien_spawn_off:
    case BlockType::hell_spawn_on:
    case BlockType::hell_spawn_off:
    case BlockType::boss_spawn_on:
    case BlockType::boss_spawn_off:
    case BlockType::enemy_spawn_dim:
    case BlockType::enemy_spawn_rare_dim:
    case BlockType::alien_spawn_dim:
    case BlockType::hell_spawn_dim:
    case BlockType::boss_spawn_dim:
    case BlockType::alien_horde_on:
    case BlockType::alien_horde_off:
    case BlockType::alien_horde_dim:
        return "MONSTER SPAWNER";
    case BlockType::teleport_station:
        return "TELEPORT STATION";
    case BlockType::crafting_station:
        return "CRAFTING STATION";
    case BlockType::hell_forge:
        return "HELL FORGE";
    case BlockType::uninitialized:
        return "UNINITIALIZED";
    default:
        return "BLOCK";
    }
}

} // namespace openminerz::world
