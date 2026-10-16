#pragma once

#include "openminerz/world/block.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz
{

// Values 0..122 are shared by both original protocols. Values 123..131 are
// project-owned Classic block conveniences that predate the Modern core port;
// they are deliberately not serialized as Classic wire items. Modern's
// colliding wire ordinals 123..154 live in a separate internal range and are
// translated only at a versioned network boundary.
enum class InventoryItemId : std::uint16_t
{
    bare_hands = 0,
    dirt_block = 1,
    sand_block = 2,
    rock_block = 3,
    log_block = 4,
    wood_block = 5,
    lantern_block = 6,
    blood_stone_block = 7,
    space_rock = 8,
    iron_wall = 9,
    copper_wall = 10,
    golden_wall = 11,
    diamond_wall = 12,
    stick = 13,
    torch = 14,
    coal = 15,
    copper_ore = 16,
    iron_ore = 17,
    gold_ore = 18,
    diamond = 19,
    iron = 20,
    copper = 21,
    gold = 22,
    stone_pickaxe = 23,
    copper_pickaxe = 24,
    iron_pickaxe = 25,
    gold_pickaxe = 26,
    diamond_pickaxe = 27,
    bloodstone_pickaxe = 28,
    stone_spade = 29,
    copper_spade = 30,
    iron_spade = 31,
    gold_spade = 32,
    diamond_spade = 33,
    stone_axe = 34,
    copper_axe = 35,
    iron_axe = 36,
    gold_axe = 37,
    diamond_axe = 38,
    compass = 39,
    clock = 40,
    brass_casing = 41,
    iron_casing = 42,
    gold_casing = 43,
    bullets = 44,
    iron_bullets = 45,
    gold_bullets = 46,
    diamond_bullets = 47,
    blood_stone_bullets = 48,
    knife = 49,
    assault_rifle = 50,
    pistol = 51,
    pump_shotgun = 52,
    bolt_action_rifle = 53,
    smg = 54,
    gold_knife = 55,
    gold_assault_rifle = 56,
    gold_pistol = 57,
    gold_pump_shotgun = 58,
    gold_bolt_action_rifle = 59,
    gold_smg = 60,
    diamond_knife = 61,
    diamond_assault_rifle = 62,
    diamond_pistol = 63,
    diamond_pump_shotgun = 64,
    diamond_bolt_action_rifle = 65,
    diamond_smg = 66,
    blood_stone_knife = 67,
    blood_stone_assault_rifle = 68,
    blood_stone_pistol = 69,
    blood_stone_pump_shotgun = 70,
    blood_stone_bolt_action_rifle = 71,
    blood_stone_smg = 72,
    crate = 73,
    snow = 74,
    ice = 75,
    door = 76,
    gps = 77,
    teleport_gps = 78,
    tnt = 79,
    c4 = 80,
    space_knife = 81,
    iron_space_assault_rifle = 82,
    iron_space_pistol = 83,
    iron_space_pump_shotgun = 84,
    iron_space_bolt_action_rifle = 85,
    iron_space_smg = 86,
    copper_space_assault_rifle = 87,
    copper_space_pistol = 88,
    copper_space_pump_shotgun = 89,
    copper_space_bolt_action_rifle = 90,
    copper_space_smg = 91,
    gold_space_assault_rifle = 92,
    gold_space_pistol = 93,
    gold_space_pump_shotgun = 94,
    gold_space_bolt_action_rifle = 95,
    gold_space_smg = 96,
    diamond_space_assault_rifle = 97,
    diamond_space_pistol = 98,
    diamond_space_pump_shotgun = 99,
    diamond_space_bolt_action_rifle = 100,
    diamond_space_smg = 101,
    slime = 102,
    rocket_launcher = 103,
    guided_rocket_launcher = 104,
    rocket_ammo = 105,
    gun_powder = 106,
    space_rock_inventory = 107,
    explosive_powder = 108,
    laser_bullets = 109,
    grenade = 110,
    diamond_casing = 111,
    fired_rocket = 112,
    fired_guided_rocket = 113,
    iron_laser_sword = 114,
    copper_laser_sword = 115,
    gold_laser_sword = 116,
    diamond_laser_sword = 117,
    blood_stone_laser_sword = 118,
    lmg = 119,
    gold_lmg = 120,
    diamond_lmg = 121,
    blood_stone_lmg = 122,
    grass_block = 123,
    gold_block = 124,
    iron_block = 125,
    copper_block = 126,
    coal_block = 127,
    diamond_block = 128,
    surface_lava_block = 129,
    leaves_block = 130,
    forced_air_block = 131,

    modern_chainsaw_1 = 256,
    modern_chainsaw_2 = 257,
    modern_chainsaw_3 = 258,
    modern_sticky_grenade = 259,
    modern_stone_container = 260,
    modern_copper_container = 261,
    modern_iron_container = 262,
    modern_gold_container = 263,
    modern_diamond_container = 264,
    modern_bloodstone_container = 265,
    modern_spawn_basic = 266,
    modern_spawn_combat = 267,
    modern_spawn_explorer = 268,
    modern_spawn_builder = 269,
    modern_glass_window_wood = 270,
    modern_glass_window_iron = 271,
    modern_glass_window_gold = 272,
    modern_glass_window_diamond = 273,
    modern_basic_grenade_launcher = 274,
    modern_advanced_grenade_launcher = 275,
    modern_laser_drill = 276,
    modern_mega_pickaxe = 277,
    modern_snowball = 278,
    modern_iceball = 279,
    modern_diamond_door = 280,
    modern_lantern_fancy_block = 281,
    modern_precision_laser = 282,
    modern_multi_laser = 283,
    modern_teleport_station = 284,
    modern_monster_block = 285,
    modern_iron_door = 286,
    modern_tech_door = 287,
    item_count = 288,
};

enum class InventoryProfile : std::uint8_t
{
    classic = 0,
    modern = 1,
};

[[nodiscard]] constexpr bool valid_inventory_item_id(InventoryItemId item) noexcept
{
    const auto value = static_cast<std::uint16_t>(item);
    return value <= static_cast<std::uint16_t>(InventoryItemId::forced_air_block) ||
           (value >= static_cast<std::uint16_t>(InventoryItemId::modern_chainsaw_1) &&
            value <= static_cast<std::uint16_t>(InventoryItemId::modern_tech_door));
}

// CastleMinerZ.PlayerMode is serialized as one byte by PlayerUpdateMessage.
// Keep the original ordinal values here even while multiplayer remains gated:
// the same table drives the correct local first-person animation family now and
// will later prevent item poses from being translated differently on the wire.
enum class PlayerAnimationMode : std::uint8_t
{
    generic = 0,
    pick = 1,
    block = 2,
    fist = 3,
    assault = 4,
    bolt_rifle = 5,
    pistol = 6,
    pump_shotgun = 7,
    smg = 8,
    lmg = 9,
    space_assault = 10,
    space_bolt_rifle = 11,
    space_pistol = 12,
    space_pump_shotgun = 13,
    space_smg = 14,
    grenade = 15,
    rpg = 16,
};

// InventoryItemClass.Name as the shipped builds register it ("Stone PickAxe",
// "Sub Machine Gun", "Wood Stick"): mixed case, exactly what the original HUD,
// inventory and crafting screens draw. 1.6.3.8 and 1.9.9.8 give every item they
// share the same name, so no generation is needed. Note what retail really
// calls things: the four laser-gun tiers are all "Laser Rifle" etc., the three
// 1.9.9.8 doors are all "Strong Door", the fancy lantern is "Lantern" and both
// laser drills are "Laser Drill".
[[nodiscard]] std::string_view item_name(InventoryItemId item) noexcept;
// Name and description as one shipped build shows them. 1.6.3.8 keeps two
// lines (Description1 / Description2) and draws them one under the other;
// 1.9.9.8 registers a single Description (the same two joined with ". "), so
// for InventoryProfile::modern everything is in `description1` and
// `description2` is empty. An item only one build has is described by that
// build whichever profile asks. BareHands' lines are a single space, as shipped.
struct ItemText final
{
    std::string_view name;
    std::string_view description1;
    std::string_view description2;
};
[[nodiscard]] ItemText item_text(InventoryItemId item, InventoryProfile profile) noexcept;
[[nodiscard]] PlayerAnimationMode item_player_animation_mode(InventoryItemId item) noexcept;
// InventoryItemClass._category from the original registrations: 1 Tools,
// 2 Special, 3 Firearms, 4 Components, 5 Blocks, 6 Ammo (-1 matches everything).
[[nodiscard]] int item_category(InventoryItemId item) noexcept;
// 1.9.9.8's InventoryItemClass.Description: the whole description on one line
// (1.6.3.8's two lines joined with ". "). Use item_text for the two-line form.
[[nodiscard]] std::string_view item_description(InventoryItemId item) noexcept;
// InventoryItemClass.MaxStackCount. The two generations disagree for most
// items (blocks 64 vs 999, ores 64 vs 255, rocket launchers 1 vs 3), so there
// is deliberately no profile-less form: a caller that forgets the generation
// merges stacks with the other game's limits.
[[nodiscard]] std::uint16_t item_stack_limit(InventoryItemId item,
                                             InventoryProfile profile) noexcept;
// True when InventoryItem.Initalize registers a class for this id in that
// generation's shipped executable (122 classes in 1.6.3.8, 143 in 1.9.9.8).
// The enums are larger than the registrations: SpaceKnife exists in both enums
// and in neither game, and Modern never registers Chainsaw2/3, the three extra
// spawn flags, the advanced launcher, MegaPickAxe, Snowball, Iceball,
// MultiLaser or MonsterBlock. InventoryItem.CreateItem indexes AllItems
// directly, so a stock peer that is handed one of those ids throws while
// reading the message. Project-owned ids are never registered.
[[nodiscard]] bool item_registered(InventoryItemId item, InventoryProfile profile) noexcept;
// InventoryItem.ProcessInput (the base implementation) is the only code that
// digs, melees and lights TNT/C4 from the held item. BlockInventoryItem (every
// block, the torch and the doors), GunInventoryItem, GPSItem and GrenadeItem
// override it without calling the base, so with one of those in hand neither
// trigger can dig or strike anything. Picks, spades, axes, knives, laser
// swords, bare hands, compass, clock and every material use the base.
[[nodiscard]] bool item_uses_tool_input(InventoryItemId item) noexcept;
[[nodiscard]] float item_use_cooldown_seconds(InventoryItemId item) noexcept;
// Duration of the authored channel-3 firing clip chosen by Player.OnUpdate.
// Automatic fire may restart the clip before it completes, exactly as vanilla.
[[nodiscard]] float item_fire_animation_seconds(InventoryItemId item, bool shouldering) noexcept;
// InventoryItem.TimeToDig with the Pick/Spade/Axe and laser-sword overrides:
// seconds for `tool` to dig `block`; infinity when that tool can never break
// it (ores need the right pick tier, TNT/C4 need a spade, and so on).
//
// The generations use different rules. 1.6.3.8 has one authored table per
// pick material and a single laser-sword table that wins over the material.
// 1.9.9.8 replaced the pick tables with a tier formula (see
// modern_pick_dig_seconds), lets picks dig soft blocks, loot blocks and lower
// tier walls, and gives every item two seconds on glass, spawn points and
// teleport stations.
[[nodiscard]] float dig_time_seconds(InventoryItemId tool, world::BlockType block,
                                     InventoryProfile profile) noexcept;
// PickInventoryItem.ComputeDigTime (1.9.9.8) for a tool tier against a block
// tier it can dig, rounded to whole milliseconds as TimeSpan.FromSeconds does.
[[nodiscard]] float modern_pick_dig_seconds(int tool_tier, int block_tier) noexcept;
// Original melee contract: InventoryItemClass.EnemyDamage per item, including
// the authored bare-hand and BlockInventoryItemClass values. Modern laser
// swords deal 1 (SaberInventoryItemClass discards its damage argument), and
// Modern authored different values for Ice, TNT, the fancy lantern and doors.
[[nodiscard]] float item_enemy_damage(InventoryItemId item, InventoryProfile profile) noexcept;
// MeleePlayerMessage applies 0.21 normalized health for every item except the
// five laser swords, which intentionally deal 1.1 (an instant lethal hit).
[[nodiscard]] float item_player_melee_damage(InventoryItemId item) noexcept;
// InventoryItemClass.IsMeleeWeapon. The base class returns true; guns, the two
// navigation instruments, compass/clock, and grenades override it to false.
[[nodiscard]] bool item_is_melee_weapon(InventoryItemId item) noexcept;
// Registered GrenadeTypeEnum values. Modern's snowball/iceball enum slots were
// never registered as inventory classes and therefore deliberately return none.
[[nodiscard]] std::optional<std::uint8_t> item_grenade_type(InventoryItemId item) noexcept;
// DoorEntity.GetModelNameFromInventoryId / BlockInventoryItemClass.DoorClasses.
// Model zero is the missing-metadata sentinel and therefore has no inventory item.
[[nodiscard]] std::optional<std::uint8_t> door_model_name(InventoryItemId item) noexcept;
[[nodiscard]] std::optional<InventoryItemId> door_item_from_model(std::uint8_t model) noexcept;
[[nodiscard]] bool is_block_item(InventoryItemId item) noexcept;
[[nodiscard]] world::BlockType item_block(InventoryItemId item) noexcept;
[[nodiscard]] InventoryItemId block_item(world::BlockType block) noexcept;
[[nodiscard]] InventoryItemId mined_item(world::BlockType block) noexcept;

// Original GunInventoryItemClass data per gun item id: fire cooldown, reload duration,
// clip capacity, trigger mode, ammo item and the original XACT cue names.
struct GunStats final
{
    float fire_cooldown_seconds{};
    float reload_seconds{};
    float shoulder_magnification{1.2F};
    std::uint16_t clip_capacity{};
    bool automatic{};
    InventoryItemId ammo_item{InventoryItemId::bare_hands};
    const char *shoot_cue{""};
    const char *reload_cue{""};
    // EnemyDamage from the original registration, per material tier.
    float enemy_damage{0.5F};
    // GunInventoryItemClass.RoundsPerReload. Shotguns load one shell per
    // animation; magazine-fed weapons use their full clip capacity.
    std::uint16_t rounds_per_reload{};
    // Original GunInventoryItemClass aim contract. Conventional bullets add
    // the fixed drop-compensation lift; LaserGunInventoryItemClass opts out.
    float inaccuracy{};
    float recoil_radians{0.05235988F};
    bool needs_drop_compensation{true};
    float tracer_velocity{100.0F};
    float tracer_flight_seconds{2.0F};
};

// Returns nullptr for anything that is not a firearm/launcher.
[[nodiscard]] const GunStats *gun_stats(InventoryItemId item) noexcept;

// LaserGunInventoryItemClass shots bypass TracerManager and use BlasterShot:
// a no-gravity 200 u/s bolt with a three-second lifetime and three ricochets.
inline constexpr float laser_projectile_speed = 200.0F;
inline constexpr float laser_projectile_lifetime_seconds = 3.0F;
inline constexpr int laser_projectile_bounces = 3;
[[nodiscard]] constexpr bool is_laser_gun(InventoryItemId item) noexcept
{
    const auto value = static_cast<std::uint16_t>(item);
    return (value >= static_cast<std::uint16_t>(InventoryItemId::iron_space_assault_rifle) &&
            value <= static_cast<std::uint16_t>(InventoryItemId::diamond_space_smg)) ||
           item == InventoryItemId::modern_laser_drill ||
           item == InventoryItemId::modern_precision_laser;
}

struct GunReloadCycle final
{
    std::uint16_t loaded{};
    bool continue_reloading{};
};

// Pure form of GunInventoryItem.Reload: determine how many rounds one completed
// reload animation inserts and whether the original immediately loops it.
[[nodiscard]] GunReloadCycle gun_reload_cycle(const GunStats &stats, std::uint16_t rounds_in_clip,
                                              std::uint32_t reserve_ammo) noexcept;

// GunInventoryItem.ProcessInput reduced to its decision, so the trigger rules
// have exactly one implementation and can be exercised without a window.
//
// `reloading` is Player.Reloading (a request; the reload clip is tracked
// separately by GunReloadClip). `released` is the original's per-item
// gunReleased flag. Only a Reload press writes it (true when the trigger was up
// at that instant) and it is never cleared, so it stays with the item: until a
// gun has been reloaded by hand once, a trigger pull cannot interrupt its
// reloads at all.
struct GunTriggerState final
{
    bool reloading{};
    bool released{};
};

// One input sample. `use_pressed` is the trigger edge and implies `use_held`.
struct GunTriggerInput final
{
    bool use_pressed{};
    bool use_held{};
    bool reload_pressed{};
    // InventoryItem.CoolDownTimer.Expired.
    bool cooldown_expired{true};
    // False only for a guided launcher that has not locked on to a dragon.
    bool fire_permitted{true};
};

struct GunTriggerDecision final
{
    GunTriggerState state{};
    // The sample fires one round: the caller drains the clip, starts the
    // cooldown and applies wear. `state.reloading` already includes the
    // automatic reload the original requests after the last round.
    bool fire{};
};

[[nodiscard]] GunTriggerDecision gun_trigger_step(const GunStats &stats, GunTriggerState state,
                                                  std::uint16_t rounds_in_clip,
                                                  std::uint32_t reserve_ammo,
                                                  GunTriggerInput input) noexcept;

// Player.UpdateAnimation's channel-2 reload clip (currentAnimState ==
// Reloading). A requested reload may only begin once the weapon is fully
// unshouldered and the channel-3 fire clip has finished; it is abandoned, with
// its cue, the moment the request is withdrawn; and a request that is withdrawn
// and re-issued inside one input sample never disturbs a running clip.
struct GunReloadClip final
{
    bool active{};
    float seconds_left{};
};

enum class GunReloadEvent : std::uint8_t
{
    none,
    // Play the reload clip and cue from the start.
    started,
    // Stop the cue; no round was inserted.
    cancelled,
    // Player.FinishReload: insert this cycle's rounds via gun_reload_cycle.
    completed,
};

struct GunReloadStep final
{
    GunReloadClip clip{};
    GunReloadEvent event{GunReloadEvent::none};
};

[[nodiscard]] GunReloadStep gun_reload_step(GunReloadClip clip, bool reloading, bool unshouldered,
                                            bool fire_animation_playing, float reload_seconds,
                                            float delta_seconds) noexcept;

struct GunRecoil final
{
    float yaw_radians{};
    float pitch_radians{};
};

// Player.ApplyRecoil: yaw samples [-.25,.25] and pitch samples [.5,1]
// times the family recoil amount.
[[nodiscard]] GunRecoil gun_recoil(float amount_radians, float yaw_roll, float pitch_roll) noexcept;

// RocketLauncherGuidedInventoryItemClass lock timing. Distance is clamped to
// 250 cells; angle_limit is 30% of the current camera field of view.
[[nodiscard]] float guided_lock_seconds(float distance, float angle_radians,
                                        float angle_limit_radians) noexcept;
[[nodiscard]] float guided_lock_beep_seconds(float required_seconds,
                                             float elapsed_seconds) noexcept;

// CMZColors.GetMaterialcColor applied through the item's ToolMaterialTypes tier;
// white for untinted items. Recolor-flagged mesh parts multiply by this.
[[nodiscard]] std::array<float, 3> item_material_tint(InventoryItemId item) noexcept;
// Stable tier colours: display names deliberately omit several material names.
[[nodiscard]] std::array<float, 3> projectile_tint(InventoryItemId item) noexcept;

// One entry of Receipe.CookBook. Unused ingredient slots have count zero.
struct CraftingIngredient final
{
    InventoryItemId item{InventoryItemId::bare_hands};
    std::uint16_t count{};
    // (Polish) The mod item (InventoryStack::mod_item) whose fallback is
    // `item`, in a mod recipe; 0 in every retail recipe.
    std::uint32_t mod_item{};
};

// Recipe.RecipeTypes from 1.9.9.8, in enum order. Modern's crafting screen is
// a fixed two-level tree over these; 1.6.3.8's Receipe has no type at all, so
// every Classic recipe carries `none`.
enum class RecipeType : std::uint8_t
{
    ores = 0,
    components = 1,
    pickaxes = 2,
    spades = 3,
    axes = 4,
    special_tools = 5,
    ammo = 6,
    knives = 7,
    pistols = 8,
    shotguns = 9,
    rifles = 10,
    assault_rifles = 11,
    smgs = 12,
    lmgs = 13,
    rpg = 14,
    laser_swords = 15,
    walls = 16,
    doors = 17,
    other_structure = 18,
    explosives = 19,
    containers = 20,
    spawn_points = 21,
    // Declared for the Precision Laser recipe only. CraftingScreen builds no
    // tab for it, so the recipe exists in the cookbook and can never be made.
    advanced = 22,
    none = 255,
};

struct CraftingRecipe final
{
    InventoryItemId result{InventoryItemId::bare_hands};
    std::uint16_t result_count{1U};
    std::array<CraftingIngredient, 4> ingredients{};
    RecipeType type{RecipeType::none};
    // (Polish) A mod recipe (recipes.add) the crafting screens list after the
    // retail cookbook: its id for the server, and the mod item it makes (0: a
    // retail item; `result` is always a retail item, the fallback). Both 0 in
    // every retail recipe. PlayerInventory never crafts a mod recipe: the
    // server does (GameplayService::client_craft).
    std::uint32_t mod_recipe{};
    std::uint32_t result_mod_item{};
};

// CraftingScreen (1.9.9.8): the four Tier1Items and the Tier2Item recipe types
// each one lists, in the order the screen constructs them.
struct RecipeGroup final
{
    const char *label{""};
    std::array<RecipeType, 11> types{};
    std::size_t type_count{};
};
[[nodiscard]] const std::array<RecipeGroup, 4> &modern_recipe_groups() noexcept;
[[nodiscard]] const char *recipe_type_label(RecipeType type) noexcept;
// False for Modern's `advanced` type: no crafting tab lists it.
[[nodiscard]] constexpr bool recipe_listed(const CraftingRecipe &recipe) noexcept
{
    return recipe.type != RecipeType::advanced;
}

// The complete vanilla cookbook in authored order.
[[nodiscard]] const std::vector<CraftingRecipe> &crafting_recipes(
    InventoryProfile profile = InventoryProfile::classic) noexcept;

struct InventoryStack final
{
    InventoryItemId item{InventoryItemId::bare_hands};
    world::BlockType block{world::BlockType::empty};
    std::uint16_t count{};
    std::uint16_t rounds_in_clip{};
    // GunInventoryItem.gunReleased (see GunTriggerState::released). Runtime
    // only: like the original field it is never saved or sent.
    bool trigger_released{};
    // InventoryItem.ItemHealthLevel. One is pristine; item classes with
    // ItemSelfDamagePerUse display a bar and may remove themselves at zero.
    float durability{1.0F};
    // GPSItem's per-instance payload. The original serializes this beside the
    // item because every locator/teleporter can point to a different named cell.
    std::array<float, 3> waypoint{};
    std::string waypoint_name{"Alpha"};
    bool waypoint_valid{};
    // Modding, gameplay area (openminerz/modding/gameplay.hpp): the session id
    // of the mod item this stack is, 0 for every retail stack - which, with no
    // server mod running, is every stack. Like trigger_released it is never
    // saved in the world file or sent in a stock message: those carry `item`,
    // the mod item's retail fallback, and the identity lives in the gameplay
    // sidecar. Stacks only merge with stacks of the same mod item.
    std::uint32_t mod_item{};

    InventoryStack() noexcept = default;
    // A freshly created gun starts with a full clip, exactly like
    // GunInventoryItem's constructor (RoundsInClip = ClipCapacity).
    InventoryStack(InventoryItemId item_id, std::uint16_t stack_count) noexcept
        : item(item_id), block(item_block(item_id)), count(stack_count),
          rounds_in_clip(gun_stats(item_id) != nullptr ? gun_stats(item_id)->clip_capacity : 0U)
    {
    }
    InventoryStack(world::BlockType block_type, std::uint16_t stack_count) noexcept
        : item(block_item(block_type)), block(block_type), count(stack_count)
    {
    }

    // A copy of this stack with a different count, preserving per-instance state
    // such as a gun's loaded rounds. Splitting or moving an existing stack MUST
    // use this instead of the constructor, or a spent clip silently refills.
    // Whether two stacks may merge: the same item, and the same mod item.
    [[nodiscard]] bool same_kind(const InventoryStack &other) const noexcept
    {
        return item == other.item && mod_item == other.mod_item;
    }

    [[nodiscard]] InventoryStack with_count(std::uint16_t stack_count) const noexcept
    {
        InventoryStack result = *this;
        result.count = stack_count;
        return result;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return item != InventoryItemId::bare_hands && count > 0U;
    }
};

// (Polish) What a recipe makes, and one of its ingredients, as stacks that
// keep a mod recipe's mod items (for icons and names).
[[nodiscard]] inline InventoryStack recipe_result_stack(const CraftingRecipe &recipe) noexcept
{
    InventoryStack stack{recipe.result, recipe.result_count};
    stack.mod_item = recipe.result_mod_item;
    return stack;
}
[[nodiscard]] inline InventoryStack ingredient_stack(const CraftingIngredient &ingredient) noexcept
{
    InventoryStack stack{ingredient.item, ingredient.count};
    stack.mod_item = ingredient.mod_item;
    return stack;
}
// Whether two recipes make the same thing (the same item, the same mod item).
[[nodiscard]] constexpr bool same_result(const CraftingRecipe &left, const CraftingRecipe &right) noexcept
{
    return left.result == right.result && left.result_mod_item == right.result_mod_item;
}

// InventoryItemClass.ItemSelfDamagePerUse plus InventoryItem.InflictDamage.
// Covers tool material tiers, knives, conventional/laser guns and locators;
// the return value is true when the item reached zero and must be removed.
// (1.9.9.8 retuned one cell: its stone pickaxe wears 0.002 per use, not 0.0025.)
[[nodiscard]] float item_self_damage_per_use(InventoryItemId item,
                                             InventoryProfile profile) noexcept;
[[nodiscard]] bool inflict_item_damage(InventoryStack &stack, bool infinite_resource_mode,
                                       InventoryProfile profile) noexcept;

class PlayerInventory final
{
  public:
    static constexpr std::size_t storage_size = 32U;
    static constexpr std::size_t tray_size = 8U;
    static constexpr std::size_t tray_count = 2U;
    static constexpr std::uint16_t block_stack_limit = 64U;

    using Slot = std::optional<InventoryStack>;
    using Tray = std::array<Slot, tray_size>;
    using Trays = std::array<Tray, tray_count>;

    PlayerInventory();

    [[nodiscard]] const std::array<Slot, storage_size> &storage() const noexcept
    {
        return storage_;
    }
    [[nodiscard]] const Tray &tray() const noexcept
    {
        return trays_[current_tray_index_];
    }
    [[nodiscard]] std::array<Slot, storage_size> &storage() noexcept
    {
        return storage_;
    }
    [[nodiscard]] Tray &tray() noexcept
    {
        return trays_[current_tray_index_];
    }
    [[nodiscard]] const Tray &tray(std::size_t index) const noexcept
    {
        return trays_[std::min(index, tray_count - 1U)];
    }
    [[nodiscard]] Tray &tray(std::size_t index) noexcept
    {
        return trays_[std::min(index, tray_count - 1U)];
    }
    [[nodiscard]] const Trays &trays() const noexcept
    {
        return trays_;
    }
    [[nodiscard]] Trays &trays() noexcept
    {
        return trays_;
    }
    [[nodiscard]] std::size_t current_tray_index() const noexcept
    {
        return current_tray_index_;
    }
    void switch_current_tray() noexcept;
    void select_tray(std::size_t index) noexcept;

    [[nodiscard]] InventoryProfile profile() const noexcept
    {
        return profile_;
    }
    void set_profile(InventoryProfile profile) noexcept
    {
        profile_ = profile;
        if (profile_ == InventoryProfile::classic)
            current_tray_index_ = 0U;
    }

    [[nodiscard]] std::size_t selected_index() const noexcept
    {
        return selected_index_;
    }
    void select(std::size_t index) noexcept;
    void select_relative(int direction) noexcept;
    [[nodiscard]] const Slot &selected() const noexcept;
    [[nodiscard]] InventoryItemId selected_item() const noexcept;
    [[nodiscard]] world::BlockType selected_block() const noexcept;

    [[nodiscard]] std::uint16_t add_item(InventoryItemId item, std::uint16_t count = 1U) noexcept;
    [[nodiscard]] std::uint16_t add_stack(const InventoryStack &stack) noexcept;
    [[nodiscard]] std::uint16_t add(world::BlockType block, std::uint16_t count = 1U) noexcept;
    [[nodiscard]] bool consume_selected(std::uint16_t count = 1U) noexcept;
    [[nodiscard]] bool consume_item(InventoryItemId item, std::uint16_t count = 1U) noexcept;
    [[nodiscard]] std::uint32_t count(InventoryItemId item) const noexcept;
    [[nodiscard]] std::uint32_t count(world::BlockType block) const noexcept;

    [[nodiscard]] Slot &slot(std::size_t linear_index) noexcept;
    [[nodiscard]] const Slot &slot(std::size_t linear_index) const noexcept;
    [[nodiscard]] bool move_to_other_section(std::size_t linear_index) noexcept;
    // The inventory screen's shift-click targets (AddItemToTray /
    // AddItemToInventory): merge into that section only and return the count
    // that did not fit.
    [[nodiscard]] std::uint16_t add_stack_to_section(const InventoryStack &stack,
                                                     bool to_tray) noexcept;

    // Receipe/PlayerInventory crafting contract: CanCraft checks the aggregate
    // count of each ingredient; Craft removes ingredients (storage first, then
    // tray, like the original) and adds the result. Returns the part of the
    // result that did not fit so the caller can drop it as a pickup.
    [[nodiscard]] bool can_craft(const CraftingRecipe &recipe) const noexcept;
    [[nodiscard]] bool recipe_discovered(const CraftingRecipe &recipe) const noexcept;
    [[nodiscard]] std::uint16_t craft(const CraftingRecipe &recipe) noexcept;

    // Placed navigation objects belong to the player profile, not carried slots.
    [[nodiscard]] Slot &spawn_point() noexcept { return spawn_point_; }
    [[nodiscard]] const Slot &spawn_point() const noexcept { return spawn_point_; }
    [[nodiscard]] std::vector<InventoryStack> &teleport_stations() noexcept { return teleport_stations_; }
    [[nodiscard]] const std::vector<InventoryStack> &teleport_stations() const noexcept { return teleport_stations_; }
    [[nodiscard]] bool register_spawn_point(std::array<float, 3> point);
    [[nodiscard]] bool register_teleport_station(std::array<float, 3> point, std::string name);
    [[nodiscard]] bool valid_travel_destinations() const noexcept;
    void remove_travel_destination(std::array<float, 3> point);

  private:
    template <std::size_t Size>
    static void stack_into(std::array<Slot, Size> &slots, const InventoryStack &source,
                           std::uint16_t limit, std::uint16_t &remaining) noexcept;
    // AddInventoryItem's two phases: top up every existing stack first, and
    // only afterwards claim empty slots.
    template <std::size_t Size>
    static void stack_existing(std::array<Slot, Size> &slots, const InventoryStack &source,
                               std::uint16_t limit, std::uint16_t &remaining) noexcept;
    template <std::size_t Size>
    static void place_into_empty(std::array<Slot, Size> &slots, const InventoryStack &source,
                                 std::uint16_t limit, std::uint16_t &remaining) noexcept;

    std::array<Slot, storage_size> storage_{};
    Trays trays_{};
    std::size_t current_tray_index_{};
    std::size_t selected_index_{};
    InventoryProfile profile_{InventoryProfile::classic};
    Slot spawn_point_;
    std::vector<InventoryStack> teleport_stations_;
};

// Fold the UI cursor into a copy for the original fixed-slot PINV contract.
// Failure leaves both live inputs untouched, including a partially mergeable stack.
[[nodiscard]] std::optional<PlayerInventory> inventory_checkpoint(
    const PlayerInventory &inventory, const PlayerInventory::Slot &held);

} // namespace openminerz
