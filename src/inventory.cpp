#include "openminerz/inventory.hpp"

#include <algorithm>
#include <limits>

namespace openminerz
{

std::string_view item_name(InventoryItemId item) noexcept
{
    switch (item)
    {
    case InventoryItemId::bare_hands:
        return "BARE HANDS";
    case InventoryItemId::dirt_block:
        return "DIRT";
    case InventoryItemId::sand_block:
        return "SAND";
    case InventoryItemId::rock_block:
        return "ROCK";
    case InventoryItemId::log_block:
        return "LOG";
    case InventoryItemId::wood_block:
        return "WOOD";
    case InventoryItemId::lantern_block:
        return "LANTERN";
    case InventoryItemId::blood_stone_block:
        return "BLOODSTONE";
    case InventoryItemId::space_rock:
        return "SPACE ROCK";
    case InventoryItemId::iron_wall:
        return "IRON WALL";
    case InventoryItemId::copper_wall:
        return "COPPER WALL";
    case InventoryItemId::golden_wall:
        return "GOLD WALL";
    case InventoryItemId::diamond_wall:
        return "DIAMOND WALL";
    case InventoryItemId::stick:
        return "STICK";
    case InventoryItemId::torch:
        return "TORCH";
    case InventoryItemId::coal:
        return "COAL";
    case InventoryItemId::copper_ore:
        return "COPPER ORE";
    case InventoryItemId::iron_ore:
        return "IRON ORE";
    case InventoryItemId::gold_ore:
        return "GOLD ORE";
    case InventoryItemId::diamond:
        return "DIAMOND";
    case InventoryItemId::iron:
        return "IRON BARS";
    case InventoryItemId::copper:
        return "COPPER BARS";
    case InventoryItemId::gold:
        return "GOLD BARS";
    case InventoryItemId::stone_pickaxe:
        return "STONE PICKAXE";
    case InventoryItemId::copper_pickaxe:
        return "COPPER PICKAXE";
    case InventoryItemId::iron_pickaxe:
        return "IRON PICKAXE";
    case InventoryItemId::gold_pickaxe:
        return "GOLD PICKAXE";
    case InventoryItemId::diamond_pickaxe:
        return "DIAMOND PICKAXE";
    case InventoryItemId::bloodstone_pickaxe:
        return "BLOODSTONE PICKAXE";
    case InventoryItemId::stone_spade:
        return "STONE SPADE";
    case InventoryItemId::copper_spade:
        return "COPPER SPADE";
    case InventoryItemId::iron_spade:
        return "IRON SPADE";
    case InventoryItemId::gold_spade:
        return "GOLD SPADE";
    case InventoryItemId::diamond_spade:
        return "DIAMOND SPADE";
    case InventoryItemId::stone_axe:
        return "STONE AXE";
    case InventoryItemId::copper_axe:
        return "COPPER AXE";
    case InventoryItemId::iron_axe:
        return "IRON AXE";
    case InventoryItemId::gold_axe:
        return "GOLD AXE";
    case InventoryItemId::diamond_axe:
        return "DIAMOND AXE";
    case InventoryItemId::compass:
        return "COMPASS";
    case InventoryItemId::clock:
        return "CLOCK";
    case InventoryItemId::brass_casing:
        return "BRASS CASING";
    case InventoryItemId::iron_casing:
        return "IRON CASING";
    case InventoryItemId::gold_casing:
        return "GOLD CASING";
    case InventoryItemId::diamond_casing:
        return "DIAMOND CASING";
    case InventoryItemId::bullets:
        return "BULLETS";
    case InventoryItemId::iron_bullets:
        return "IRON BULLETS";
    case InventoryItemId::gold_bullets:
        return "GOLD BULLETS";
    case InventoryItemId::diamond_bullets:
        return "DIAMOND BULLETS";
    case InventoryItemId::blood_stone_bullets:
        return "BLOODSTONE BULLETS";
    case InventoryItemId::knife:
        return "KNIFE";
    case InventoryItemId::assault_rifle:
        return "ASSAULT RIFLE";
    case InventoryItemId::pistol:
        return "PISTOL";
    case InventoryItemId::pump_shotgun:
        return "PUMP SHOTGUN";
    case InventoryItemId::bolt_action_rifle:
        return "BOLT-ACTION RIFLE";
    case InventoryItemId::smg:
        return "SUBMACHINE GUN";
    case InventoryItemId::gold_knife:
        return "GOLD KNIFE";
    case InventoryItemId::gold_assault_rifle:
        return "GOLD ASSAULT RIFLE";
    case InventoryItemId::gold_pistol:
        return "GOLD PISTOL";
    case InventoryItemId::gold_pump_shotgun:
        return "GOLD SHOTGUN";
    case InventoryItemId::gold_bolt_action_rifle:
        return "GOLD RIFLE";
    case InventoryItemId::gold_smg:
        return "GOLD SMG";
    case InventoryItemId::diamond_knife:
        return "DIAMOND KNIFE";
    case InventoryItemId::diamond_assault_rifle:
        return "DIAMOND ASSAULT RIFLE";
    case InventoryItemId::diamond_pistol:
        return "DIAMOND PISTOL";
    case InventoryItemId::diamond_pump_shotgun:
        return "DIAMOND SHOTGUN";
    case InventoryItemId::diamond_bolt_action_rifle:
        return "DIAMOND RIFLE";
    case InventoryItemId::diamond_smg:
        return "DIAMOND SMG";
    case InventoryItemId::blood_stone_knife:
        return "BLOODSTONE KNIFE";
    case InventoryItemId::blood_stone_assault_rifle:
        return "BLOODSTONE ASSAULT RIFLE";
    case InventoryItemId::blood_stone_pistol:
        return "BLOODSTONE PISTOL";
    case InventoryItemId::blood_stone_pump_shotgun:
        return "BLOODSTONE SHOTGUN";
    case InventoryItemId::blood_stone_bolt_action_rifle:
        return "BLOODSTONE RIFLE";
    case InventoryItemId::blood_stone_smg:
        return "BLOODSTONE SMG";
    case InventoryItemId::space_knife:
        return "SPACE KNIFE";
    case InventoryItemId::iron_space_assault_rifle:
        return "LASER ASSAULT RIFLE";
    case InventoryItemId::iron_space_pistol:
        return "LASER PISTOL";
    case InventoryItemId::iron_space_pump_shotgun:
        return "LASER SHOTGUN";
    case InventoryItemId::iron_space_bolt_action_rifle:
        return "LASER RIFLE";
    case InventoryItemId::iron_space_smg:
        return "LASER SMG";
    case InventoryItemId::copper_space_assault_rifle:
        return "GREEN LASER ASSAULT RIFLE";
    case InventoryItemId::copper_space_pistol:
        return "GREEN LASER PISTOL";
    case InventoryItemId::copper_space_pump_shotgun:
        return "GREEN LASER SHOTGUN";
    case InventoryItemId::copper_space_bolt_action_rifle:
        return "GREEN LASER RIFLE";
    case InventoryItemId::copper_space_smg:
        return "GREEN LASER SMG";
    case InventoryItemId::gold_space_assault_rifle:
        return "YELLOW LASER ASSAULT RIFLE";
    case InventoryItemId::gold_space_pistol:
        return "YELLOW LASER PISTOL";
    case InventoryItemId::gold_space_pump_shotgun:
        return "YELLOW LASER SHOTGUN";
    case InventoryItemId::gold_space_bolt_action_rifle:
        return "YELLOW LASER RIFLE";
    case InventoryItemId::gold_space_smg:
        return "YELLOW LASER SMG";
    case InventoryItemId::diamond_space_assault_rifle:
        return "BLUE LASER ASSAULT RIFLE";
    case InventoryItemId::diamond_space_pistol:
        return "BLUE LASER PISTOL";
    case InventoryItemId::diamond_space_pump_shotgun:
        return "BLUE LASER SHOTGUN";
    case InventoryItemId::diamond_space_bolt_action_rifle:
        return "BLUE LASER RIFLE";
    case InventoryItemId::diamond_space_smg:
        return "BLUE LASER SMG";
    case InventoryItemId::iron_laser_sword:
        return "LASER SWORD";
    case InventoryItemId::copper_laser_sword:
        return "GREEN LASER SWORD";
    case InventoryItemId::gold_laser_sword:
        return "YELLOW LASER SWORD";
    case InventoryItemId::diamond_laser_sword:
        return "BLUE LASER SWORD";
    case InventoryItemId::blood_stone_laser_sword:
        return "BLOODSTONE LASER SWORD";
    case InventoryItemId::gold_lmg:
        return "GOLD LIGHT MACHINE GUN";
    case InventoryItemId::diamond_lmg:
        return "DIAMOND LIGHT MACHINE GUN";
    case InventoryItemId::blood_stone_lmg:
        return "BLOODSTONE LIGHT MACHINE GUN";
    case InventoryItemId::crate:
        return "CRATE";
    case InventoryItemId::snow:
        return "SNOW";
    case InventoryItemId::ice:
        return "ICE";
    case InventoryItemId::door:
        return "DOOR";
    case InventoryItemId::gps:
        return "LOCATOR";
    case InventoryItemId::teleport_gps:
        return "TELEPORTER";
    case InventoryItemId::tnt:
        return "TNT";
    case InventoryItemId::c4:
        return "C4";
    case InventoryItemId::slime:
        return "SLIME";
    case InventoryItemId::rocket_launcher:
        return "ROCKET LAUNCHER";
    case InventoryItemId::guided_rocket_launcher:
        return "GUIDED ROCKET LAUNCHER";
    case InventoryItemId::rocket_ammo:
        return "ROCKETS";
    case InventoryItemId::gun_powder:
        return "GUNPOWDER";
    case InventoryItemId::space_rock_inventory:
        return "SPACE ROCK";
    case InventoryItemId::explosive_powder:
        return "EXPLOSIVE POWDER";
    case InventoryItemId::laser_bullets:
        return "LASER BULLETS";
    case InventoryItemId::grenade:
        return "GRENADE";
    case InventoryItemId::lmg:
        return "LIGHT MACHINE GUN";
    case InventoryItemId::grass_block:
        return "GRASS";
    case InventoryItemId::gold_block:
        return "GOLD BLOCK";
    case InventoryItemId::iron_block:
        return "IRON BLOCK";
    case InventoryItemId::copper_block:
        return "COPPER BLOCK";
    case InventoryItemId::coal_block:
        return "COAL BLOCK";
    case InventoryItemId::diamond_block:
        return "DIAMOND BLOCK";
    case InventoryItemId::surface_lava_block:
        return "LAVA";
    case InventoryItemId::leaves_block:
        return "LEAVES";
    case InventoryItemId::forced_air_block:
        return "FORCED AIR";
    case InventoryItemId::modern_chainsaw_1:
        return "CHAINSAW";
    case InventoryItemId::modern_chainsaw_2:
        return "CHAINSAW II";
    case InventoryItemId::modern_chainsaw_3:
        return "CHAINSAW III";
    case InventoryItemId::modern_sticky_grenade:
        return "STICKY GRENADE";
    case InventoryItemId::modern_stone_container:
        return "STONE CONTAINER";
    case InventoryItemId::modern_copper_container:
        return "COPPER CONTAINER";
    case InventoryItemId::modern_iron_container:
        return "IRON CONTAINER";
    case InventoryItemId::modern_gold_container:
        return "GOLD CONTAINER";
    case InventoryItemId::modern_diamond_container:
        return "DIAMOND CONTAINER";
    case InventoryItemId::modern_bloodstone_container:
        return "BLOODSTONE CONTAINER";
    case InventoryItemId::modern_spawn_basic:
        return "RESPAWN BEACON";
    case InventoryItemId::modern_spawn_combat:
        return "FOREMAN'S FLAG";
    case InventoryItemId::modern_spawn_explorer:
        return "FARAWAY FLAG";
    case InventoryItemId::modern_spawn_builder:
        return "FEARSOME FLAG";
    case InventoryItemId::modern_glass_window_wood:
        return "WOOD WINDOW";
    case InventoryItemId::modern_glass_window_iron:
        return "IRON WINDOW";
    case InventoryItemId::modern_glass_window_gold:
        return "BULLETPROOF WINDOW";
    case InventoryItemId::modern_glass_window_diamond:
        return "CLEAR WINDOW";
    case InventoryItemId::modern_basic_grenade_launcher:
        return "GRENADE LAUNCHER";
    case InventoryItemId::modern_advanced_grenade_launcher:
        return "ADVANCED GRENADE LAUNCHER";
    case InventoryItemId::modern_laser_drill:
        return "LASER DRILL";
    case InventoryItemId::modern_mega_pickaxe:
        return "MEGA PICKAXE";
    case InventoryItemId::modern_snowball:
        return "SNOWBALL GRENADE";
    case InventoryItemId::modern_iceball:
        return "ICEBALL GRENADE";
    case InventoryItemId::modern_diamond_door:
        return "DIAMOND DOOR";
    case InventoryItemId::modern_lantern_fancy_block:
        return "FANCY LANTERN";
    case InventoryItemId::modern_precision_laser:
        return "PRECISION LASER";
    case InventoryItemId::modern_multi_laser:
        return "MULTI LASER";
    case InventoryItemId::modern_teleport_station:
        return "TELEPORT STATION";
    case InventoryItemId::modern_monster_block:
        return "MONSTER SPAWNER";
    case InventoryItemId::modern_iron_door:
        return "IRON DOOR";
    case InventoryItemId::modern_tech_door:
        return "TECH DOOR";
    default:
        return "ITEM";
    }
}

PlayerAnimationMode item_player_animation_mode(InventoryItemId item) noexcept
{
    // InventoryItemClass defaults to Generic; only the concrete registrations
    // below override _playerMode in the original. Keep this table item based so
    // local rendering and the future PlayerUpdateMessage byte share one source.
    switch (item)
    {
    case InventoryItemId::bare_hands:
        return PlayerAnimationMode::fist;
    case InventoryItemId::torch:
    case InventoryItemId::stone_pickaxe:
    case InventoryItemId::copper_pickaxe:
    case InventoryItemId::iron_pickaxe:
    case InventoryItemId::gold_pickaxe:
    case InventoryItemId::diamond_pickaxe:
    case InventoryItemId::bloodstone_pickaxe:
    case InventoryItemId::stone_spade:
    case InventoryItemId::copper_spade:
    case InventoryItemId::iron_spade:
    case InventoryItemId::gold_spade:
    case InventoryItemId::diamond_spade:
    case InventoryItemId::stone_axe:
    case InventoryItemId::copper_axe:
    case InventoryItemId::iron_axe:
    case InventoryItemId::gold_axe:
    case InventoryItemId::diamond_axe:
    case InventoryItemId::knife:
    case InventoryItemId::gold_knife:
    case InventoryItemId::diamond_knife:
    case InventoryItemId::blood_stone_knife:
    case InventoryItemId::space_knife:
    case InventoryItemId::iron_laser_sword:
    case InventoryItemId::copper_laser_sword:
    case InventoryItemId::gold_laser_sword:
    case InventoryItemId::diamond_laser_sword:
    case InventoryItemId::blood_stone_laser_sword:
    case InventoryItemId::modern_chainsaw_1:
    case InventoryItemId::modern_chainsaw_2:
    case InventoryItemId::modern_chainsaw_3:
        return PlayerAnimationMode::pick;
    case InventoryItemId::assault_rifle:
    case InventoryItemId::gold_assault_rifle:
    case InventoryItemId::diamond_assault_rifle:
    case InventoryItemId::blood_stone_assault_rifle:
        return PlayerAnimationMode::assault;
    case InventoryItemId::bolt_action_rifle:
    case InventoryItemId::gold_bolt_action_rifle:
    case InventoryItemId::diamond_bolt_action_rifle:
    case InventoryItemId::blood_stone_bolt_action_rifle:
        return PlayerAnimationMode::bolt_rifle;
    case InventoryItemId::pistol:
    case InventoryItemId::gold_pistol:
    case InventoryItemId::diamond_pistol:
    case InventoryItemId::blood_stone_pistol:
        return PlayerAnimationMode::pistol;
    case InventoryItemId::pump_shotgun:
    case InventoryItemId::gold_pump_shotgun:
    case InventoryItemId::diamond_pump_shotgun:
    case InventoryItemId::blood_stone_pump_shotgun:
        return PlayerAnimationMode::pump_shotgun;
    case InventoryItemId::smg:
    case InventoryItemId::gold_smg:
    case InventoryItemId::diamond_smg:
    case InventoryItemId::blood_stone_smg:
        return PlayerAnimationMode::smg;
    case InventoryItemId::lmg:
    case InventoryItemId::gold_lmg:
    case InventoryItemId::diamond_lmg:
    case InventoryItemId::blood_stone_lmg:
        return PlayerAnimationMode::lmg;
    case InventoryItemId::iron_space_assault_rifle:
    case InventoryItemId::copper_space_assault_rifle:
    case InventoryItemId::gold_space_assault_rifle:
    case InventoryItemId::diamond_space_assault_rifle:
        return PlayerAnimationMode::space_assault;
    case InventoryItemId::iron_space_bolt_action_rifle:
    case InventoryItemId::copper_space_bolt_action_rifle:
    case InventoryItemId::gold_space_bolt_action_rifle:
    case InventoryItemId::diamond_space_bolt_action_rifle:
        return PlayerAnimationMode::space_bolt_rifle;
    case InventoryItemId::iron_space_pistol:
    case InventoryItemId::copper_space_pistol:
    case InventoryItemId::gold_space_pistol:
    case InventoryItemId::diamond_space_pistol:
        return PlayerAnimationMode::space_pistol;
    case InventoryItemId::iron_space_pump_shotgun:
    case InventoryItemId::copper_space_pump_shotgun:
    case InventoryItemId::gold_space_pump_shotgun:
    case InventoryItemId::diamond_space_pump_shotgun:
        return PlayerAnimationMode::space_pump_shotgun;
    case InventoryItemId::iron_space_smg:
    case InventoryItemId::copper_space_smg:
    case InventoryItemId::gold_space_smg:
    case InventoryItemId::diamond_space_smg:
        return PlayerAnimationMode::space_smg;
    case InventoryItemId::grenade:
    case InventoryItemId::modern_sticky_grenade:
    case InventoryItemId::modern_snowball:
    case InventoryItemId::modern_iceball:
        return PlayerAnimationMode::grenade;
    case InventoryItemId::rocket_launcher:
    case InventoryItemId::guided_rocket_launcher:
    case InventoryItemId::fired_rocket:
    case InventoryItemId::fired_guided_rocket:
    case InventoryItemId::modern_basic_grenade_launcher:
    case InventoryItemId::modern_advanced_grenade_launcher:
        return PlayerAnimationMode::rpg;
    case InventoryItemId::modern_laser_drill:
    case InventoryItemId::modern_precision_laser:
    case InventoryItemId::modern_multi_laser:
        return PlayerAnimationMode::space_assault;
    default:
        return is_block_item(item) ? PlayerAnimationMode::block : PlayerAnimationMode::generic;
    }
}

int item_category(InventoryItemId item) noexcept
{
    // Every GunInventoryItemClass, launchers included, registered with 3.
    if (gun_stats(item) != nullptr)
        return 3;
    switch (item)
    {
    case InventoryItemId::stone_pickaxe:
    case InventoryItemId::copper_pickaxe:
    case InventoryItemId::iron_pickaxe:
    case InventoryItemId::gold_pickaxe:
    case InventoryItemId::diamond_pickaxe:
    case InventoryItemId::bloodstone_pickaxe:
    case InventoryItemId::stone_spade:
    case InventoryItemId::copper_spade:
    case InventoryItemId::iron_spade:
    case InventoryItemId::gold_spade:
    case InventoryItemId::diamond_spade:
    case InventoryItemId::stone_axe:
    case InventoryItemId::copper_axe:
    case InventoryItemId::iron_axe:
    case InventoryItemId::gold_axe:
    case InventoryItemId::diamond_axe:
    case InventoryItemId::knife:
    case InventoryItemId::gold_knife:
    case InventoryItemId::diamond_knife:
    case InventoryItemId::blood_stone_knife:
    case InventoryItemId::space_knife:
    case InventoryItemId::iron_laser_sword:
    case InventoryItemId::copper_laser_sword:
    case InventoryItemId::gold_laser_sword:
    case InventoryItemId::diamond_laser_sword:
    case InventoryItemId::blood_stone_laser_sword:
    case InventoryItemId::modern_chainsaw_1:
    case InventoryItemId::modern_chainsaw_2:
    case InventoryItemId::modern_chainsaw_3:
    case InventoryItemId::modern_mega_pickaxe:
        return 1;
    case InventoryItemId::grenade:
    case InventoryItemId::compass:
    case InventoryItemId::clock:
    case InventoryItemId::gps:
    case InventoryItemId::teleport_gps:
    case InventoryItemId::modern_sticky_grenade:
    case InventoryItemId::modern_snowball:
    case InventoryItemId::modern_iceball:
        return 2;
    case InventoryItemId::bullets:
    case InventoryItemId::iron_bullets:
    case InventoryItemId::gold_bullets:
    case InventoryItemId::diamond_bullets:
    case InventoryItemId::blood_stone_bullets:
    case InventoryItemId::laser_bullets:
    case InventoryItemId::rocket_ammo:
        return 6;
    default:
        break;
    }
    if (is_block_item(item))
        return 5;
    // Stick, casings, powders, coal, ores, gems and bars all registered with 4.
    return 4;
}

std::string_view item_description(InventoryItemId item) noexcept
{
    switch (item)
    {
    case InventoryItemId::wood_block:
        return "Planks cut from logs";
    case InventoryItemId::rock_block:
        return "Commonly found underground";
    case InventoryItemId::log_block:
    case InventoryItemId::leaves_block:
        return "Comes from trees";
    case InventoryItemId::dirt_block:
    case InventoryItemId::sand_block:
    case InventoryItemId::grass_block:
    case InventoryItemId::snow:
    case InventoryItemId::ice:
        return "Found on the surface";
    case InventoryItemId::lantern_block:
        return "Lights the world, tougher than a torch";
    case InventoryItemId::blood_stone_block:
        return "Found in hell, very hard";
    case InventoryItemId::space_rock:
    case InventoryItemId::space_rock_inventory:
        return "Comes from space";
    case InventoryItemId::slime:
        return "Comes from space, used for alien weapons";
    case InventoryItemId::iron_wall:
    case InventoryItemId::copper_wall:
    case InventoryItemId::golden_wall:
    case InventoryItemId::diamond_wall:
        return "Strong wall, stops digging monsters";
    case InventoryItemId::crate:
        return "Stores items";
    case InventoryItemId::tnt:
        return "Blows up a large area";
    case InventoryItemId::c4:
        return "Placed explosive, destroys everything";
    case InventoryItemId::stick:
        return "Used to craft tools and torches";
    case InventoryItemId::torch:
        return "Lights your world, wards off monsters";
    case InventoryItemId::door:
        return "Open or close to keep monsters out";
    case InventoryItemId::coal:
        return "Fuel for smelting and crafting";
    case InventoryItemId::copper_ore:
    case InventoryItemId::iron_ore:
    case InventoryItemId::gold_ore:
        return "Smelt with coal or a log to make bars";
    case InventoryItemId::diamond:
        return "Very hard, used for diamond gear";
    case InventoryItemId::iron:
    case InventoryItemId::copper:
    case InventoryItemId::gold:
        return "Refined metal used to craft items";
    case InventoryItemId::stone_pickaxe:
    case InventoryItemId::copper_pickaxe:
    case InventoryItemId::iron_pickaxe:
    case InventoryItemId::gold_pickaxe:
    case InventoryItemId::diamond_pickaxe:
    case InventoryItemId::bloodstone_pickaxe:
        return "Breaks stone and ore faster";
    case InventoryItemId::stone_spade:
    case InventoryItemId::copper_spade:
    case InventoryItemId::iron_spade:
    case InventoryItemId::gold_spade:
    case InventoryItemId::diamond_spade:
        return "Digs dirt and sand, removes TNT and C4";
    case InventoryItemId::stone_axe:
    case InventoryItemId::copper_axe:
    case InventoryItemId::iron_axe:
    case InventoryItemId::gold_axe:
    case InventoryItemId::diamond_axe:
        return "Chops wood, basic melee defense";
    case InventoryItemId::knife:
    case InventoryItemId::gold_knife:
    case InventoryItemId::diamond_knife:
    case InventoryItemId::blood_stone_knife:
    case InventoryItemId::space_knife:
        return "Basic melee defense";
    case InventoryItemId::iron_laser_sword:
    case InventoryItemId::copper_laser_sword:
    case InventoryItemId::gold_laser_sword:
    case InventoryItemId::diamond_laser_sword:
    case InventoryItemId::blood_stone_laser_sword:
        return "Advanced melee and mining tool";
    case InventoryItemId::compass:
        return "Points back to the start point";
    case InventoryItemId::clock:
        return "Shows the time of day";
    case InventoryItemId::gps:
        return "Marks a spot so you can find it again";
    case InventoryItemId::teleport_gps:
        return "Teleports you to a marked spot";
    case InventoryItemId::brass_casing:
    case InventoryItemId::iron_casing:
    case InventoryItemId::gold_casing:
    case InventoryItemId::diamond_casing:
        return "Used for making ammunition";
    case InventoryItemId::bullets:
        return "Ammo for conventional weapons";
    case InventoryItemId::iron_bullets:
        return "Ammo for gold weapons";
    case InventoryItemId::gold_bullets:
        return "Ammo for diamond weapons";
    case InventoryItemId::diamond_bullets:
        return "Ammo for bloodstone weapons";
    case InventoryItemId::blood_stone_bullets:
        return "High grade ammunition";
    case InventoryItemId::laser_bullets:
        return "Ammo for laser weapons";
    case InventoryItemId::rocket_ammo:
        return "Ammo for rocket launchers";
    case InventoryItemId::gun_powder:
        return "A raw crafting material";
    case InventoryItemId::explosive_powder:
        return "Crafts explosives, dropped by dragons and demons";
    case InventoryItemId::grenade:
        return "Cook it and throw it";
    case InventoryItemId::rocket_launcher:
        return "Dumb-fired explosive projectile";
    case InventoryItemId::guided_rocket_launcher:
        return "Guided missile for killing dragons";
    case InventoryItemId::pistol:
    case InventoryItemId::gold_pistol:
    case InventoryItemId::diamond_pistol:
    case InventoryItemId::blood_stone_pistol:
    case InventoryItemId::iron_space_pistol:
    case InventoryItemId::copper_space_pistol:
    case InventoryItemId::gold_space_pistol:
    case InventoryItemId::diamond_space_pistol:
        return "Basic semi-automatic gun";
    case InventoryItemId::assault_rifle:
    case InventoryItemId::gold_assault_rifle:
    case InventoryItemId::diamond_assault_rifle:
    case InventoryItemId::blood_stone_assault_rifle:
    case InventoryItemId::iron_space_assault_rifle:
    case InventoryItemId::copper_space_assault_rifle:
    case InventoryItemId::gold_space_assault_rifle:
    case InventoryItemId::diamond_space_assault_rifle:
        return "High power, full auto";
    case InventoryItemId::pump_shotgun:
    case InventoryItemId::gold_pump_shotgun:
    case InventoryItemId::diamond_pump_shotgun:
    case InventoryItemId::blood_stone_pump_shotgun:
    case InventoryItemId::iron_space_pump_shotgun:
    case InventoryItemId::copper_space_pump_shotgun:
    case InventoryItemId::gold_space_pump_shotgun:
    case InventoryItemId::diamond_space_pump_shotgun:
        return "Short range burst fire";
    case InventoryItemId::bolt_action_rifle:
    case InventoryItemId::gold_bolt_action_rifle:
    case InventoryItemId::diamond_bolt_action_rifle:
    case InventoryItemId::blood_stone_bolt_action_rifle:
    case InventoryItemId::iron_space_bolt_action_rifle:
    case InventoryItemId::copper_space_bolt_action_rifle:
    case InventoryItemId::gold_space_bolt_action_rifle:
    case InventoryItemId::diamond_space_bolt_action_rifle:
        return "High power, very accurate";
    case InventoryItemId::smg:
    case InventoryItemId::gold_smg:
    case InventoryItemId::diamond_smg:
    case InventoryItemId::blood_stone_smg:
    case InventoryItemId::iron_space_smg:
    case InventoryItemId::copper_space_smg:
    case InventoryItemId::gold_space_smg:
    case InventoryItemId::diamond_space_smg:
        return "High rate of fire";
    case InventoryItemId::lmg:
    case InventoryItemId::gold_lmg:
    case InventoryItemId::diamond_lmg:
    case InventoryItemId::blood_stone_lmg:
        return "Powerful with a large clip";
    default:
        return "";
    }
}

float dig_time_seconds(InventoryItemId tool, world::BlockType block) noexcept
{
    using B = world::BlockType;
    constexpr float no_entry = -1.0F;
    const auto value = static_cast<std::uint16_t>(tool);

    // ToolMaterialTypes tiers, indexed 0 stone .. 5 bloodstone.
    enum class Tier
    {
        stone,
        copper,
        iron,
        gold,
        diamond,
        blood_stone,
    };

    const auto pick_table = [&](Tier tier) -> float {
        switch (tier)
        {
        case Tier::stone:
            switch (block)
            {
            case B::rock:
                return 2.0F;
            case B::iron_ore:
                return 9.0F;
            case B::copper_ore:
                return 6.0F;
            case B::coal_ore:
                return 3.0F;
            case B::ice:
                return 1.0F;
            default:
                return no_entry;
            }
        case Tier::copper:
            switch (block)
            {
            case B::rock:
                return 1.5F;
            case B::iron_ore:
                return 6.0F;
            case B::copper_ore:
                return 3.0F;
            case B::coal_ore:
                return 1.5F;
            case B::ice:
                return 0.75F;
            case B::copper_wall:
                return 3.0F;
            default:
                return no_entry;
            }
        case Tier::iron:
            switch (block)
            {
            case B::rock:
                return 0.5F;
            case B::gold_ore:
                return 6.0F;
            case B::iron_ore:
                return 3.0F;
            case B::copper_ore:
                return 1.5F;
            case B::coal_ore:
                return 1.0F;
            case B::ice:
                return 0.25F;
            case B::iron_wall:
                return 3.0F;
            case B::copper_wall:
                return 1.5F;
            default:
                return no_entry;
            }
        case Tier::gold:
            switch (block)
            {
            case B::rock:
                return 0.25F;
            case B::gold_ore:
                return 3.0F;
            case B::iron_ore:
                return 2.0F;
            case B::copper_ore:
                return 1.0F;
            case B::coal_ore:
                return 0.5F;
            case B::diamond_ore:
                return 5.0F;
            case B::ice:
                return 0.1F;
            case B::iron_wall:
                return 2.0F;
            case B::copper_wall:
                return 1.0F;
            case B::golden_wall:
                return 3.0F;
            default:
                return no_entry;
            }
        case Tier::diamond:
            switch (block)
            {
            case B::rock:
                return 0.1F;
            case B::gold_ore:
                return 2.0F;
            case B::iron_ore:
                return 1.0F;
            case B::copper_ore:
                return 0.5F;
            case B::coal_ore:
                return 0.25F;
            case B::diamond_ore:
                return 3.0F;
            case B::ice:
                return 0.05F;
            case B::blood_stone:
                return 10.0F;
            case B::iron_wall:
                return 1.0F;
            case B::copper_wall:
                return 0.5F;
            case B::golden_wall:
                return 2.0F;
            case B::diamond_wall:
                return 3.0F;
            default:
                return no_entry;
            }
        case Tier::blood_stone:
            switch (block)
            {
            case B::rock:
                return 0.01F;
            case B::gold_ore:
                return 1.0F;
            case B::iron_ore:
                return 0.5F;
            case B::copper_ore:
                return 0.2F;
            case B::coal_ore:
                return 0.1F;
            case B::diamond_ore:
                return 1.5F;
            case B::ice:
                return 0.01F;
            case B::blood_stone:
                return 3.0F;
            case B::space_rock:
                return 3.0F;
            case B::space_rock_inventory:
                return 3.0F;
            case B::iron_wall:
                return 0.5F;
            case B::copper_wall:
                return 0.2F;
            case B::golden_wall:
                return 1.0F;
            case B::diamond_wall:
                return 1.5F;
            case B::slime:
                return 6.0F;
            default:
                return no_entry;
            }
        }
        return no_entry;
    };

    const auto laser_table = [&]() -> float {
        switch (block)
        {
        case B::dirt:
        case B::grass:
        case B::sand:
        case B::rock:
        case B::snow:
        case B::leaves:
        case B::wood:
        case B::ice:
            return 0.01F;
        case B::gold_ore:
        case B::iron_ore:
        case B::diamond_ore:
            return 0.5F;
        case B::copper_ore:
            return 0.2F;
        case B::coal_ore:
            return 0.1F;
        case B::blood_stone:
            return 1.0F;
        case B::space_rock:
        case B::space_rock_inventory:
            return 2.0F;
        case B::iron_wall:
        case B::golden_wall:
        case B::diamond_wall:
            return 0.5F;
        case B::copper_wall:
            return 0.2F;
        case B::slime:
            return 4.0F;
        default:
            return no_entry;
        }
    };

    const auto spade_table = [&](Tier tier) -> float {
        const bool spade_block = block == B::dirt || block == B::grass || block == B::sand ||
                                 block == B::snow || block == B::tnt || block == B::c4;
        if (!spade_block)
            return no_entry;
        const bool soft = block == B::sand || block == B::snow;
        switch (tier)
        {
        case Tier::stone:
            return soft ? 0.75F : 1.0F;
        case Tier::copper:
            return 0.5F;
        case Tier::iron:
            return 0.25F;
        case Tier::gold:
            return 0.1F;
        default:
            return 0.0F;
        }
    };

    const auto axe_table = [&](Tier tier) -> float {
        switch (tier)
        {
        case Tier::stone:
            switch (block)
            {
            case B::log:
                return 2.0F;
            case B::leaves:
                return 0.5F;
            case B::wood:
                return 1.0F;
            default:
                return no_entry;
            }
        case Tier::copper:
            switch (block)
            {
            case B::log:
                return 1.0F;
            case B::leaves:
                return 0.1F;
            case B::wood:
                return 0.5F;
            default:
                return no_entry;
            }
        case Tier::iron:
            switch (block)
            {
            case B::log:
                return 0.5F;
            case B::leaves:
                return 0.0F;
            case B::wood:
                return 0.25F;
            default:
                return no_entry;
            }
        case Tier::gold:
            switch (block)
            {
            case B::log:
                return 0.25F;
            case B::leaves:
            case B::wood:
                return 0.0F;
            default:
                return no_entry;
            }
        default:
            switch (block)
            {
            case B::log:
            case B::leaves:
            case B::wood:
                return 0.0F;
            default:
                return no_entry;
            }
        }
    };

    float seconds = no_entry;
    if (tool == InventoryItemId::modern_chainsaw_1)
    {
        // The only registered chainsaw uses BloodStone material (tier six),
        // whose exact Log/Leaves/Wood branch is instantaneous.
        if (block == B::log || block == B::leaves || block == B::wood)
            return 0.0F;
    }
    else if (value >= 23U && value <= 28U) // Picks, stone through bloodstone.
    {
        seconds = pick_table(static_cast<Tier>(value - 23U));
    }
    else if (value >= 114U && value <= 118U) // Laser swords: laser table, then
    {                                        // the metal tier wins where it has
        seconds = laser_table();             // an entry, like the original.
        const Tier tier = tool == InventoryItemId::iron_laser_sword      ? Tier::iron
                          : tool == InventoryItemId::copper_laser_sword  ? Tier::copper
                          : tool == InventoryItemId::gold_laser_sword    ? Tier::gold
                          : tool == InventoryItemId::diamond_laser_sword ? Tier::diamond
                                                                         : Tier::blood_stone;
        const float metal = pick_table(tier);
        if (metal >= 0.0F)
            seconds = metal;
    }
    else if (value >= 29U && value <= 33U) // Spades, stone through diamond.
    {
        seconds = spade_table(static_cast<Tier>(value - 29U));
    }
    else if (value >= 34U && value <= 38U) // Axes, stone through diamond.
    {
        seconds = axe_table(static_cast<Tier>(value - 34U));
    }
    if (seconds >= 0.0F)
        return seconds;
    // TNT and C4 only come out with a spade; everything else defuses instead.
    if (block == B::tnt || block == B::c4)
        return std::numeric_limits<float>::infinity();
    return world::bare_hand_break_seconds(block);
}

float item_enemy_damage(InventoryItemId item) noexcept
{
    if (const GunStats *gun = gun_stats(item); gun != nullptr)
        return gun->enemy_damage;
    if (item == InventoryItemId::modern_chainsaw_1)
        return 5.0F;
    switch (static_cast<std::uint16_t>(item))
    {
    case 0U:
        return 0.025F; // BareHandInventoryItemClass.
    case 1U:           // Dirt and sand blocks.
    case 2U:
        return 0.01F;
    case 4U: // Log, wood, and lantern blocks.
    case 5U:
    case 6U:
        return 0.075F;
    case 7U: // Bloodstone and both space-rock inventory classes.
    case 8U:
    case 107U:
        return 0.15F;
    case 13U:
        return 0.05F; // StickInventoryItemClass.
    case 74U:         // Snow and ice blocks.
    case 75U:
        return 0.01F;
    case 102U:
        return 0.075F; // Slime block.
    case 112U:         // Fired-launcher pose classes are still RPG guns.
    case 113U:
        return 100.0F;
    case 23U:
        return 0.1F; // Pick tiers, stone through bloodstone.
    case 24U:
        return 0.2F;
    case 25U:
        return 0.4F;
    case 26U:
        return 0.8F;
    case 27U:
        return 1.6F;
    case 28U:
        return 3.0F;
    case 29U:
        return 0.1F; // Spade tiers, stone through diamond.
    case 30U:
        return 0.2F;
    case 31U:
        return 0.4F;
    case 32U:
        return 0.8F;
    case 33U:
        return 1.6F;
    case 34U:
        return 0.15F; // Axe tiers, stone through diamond.
    case 35U:
        return 0.3F;
    case 36U:
        return 0.5F;
    case 37U:
        return 1.0F;
    case 38U:
        return 2.0F;
    case 49U:
        return 0.5F; // Knife tiers.
    case 55U:
        return 1.0F;
    case 61U:
        return 2.0F;
    case 67U:
        return 4.0F;
    case 81U:
        return 4.0F;
    case 114U: // Laser swords all registered at 8.
    case 115U:
    case 116U:
    case 117U:
    case 118U:
        return 8.0F;
    default:
        return 0.1F; // InventoryItemClass default EnemyDamage.
    }
}

float item_player_melee_damage(InventoryItemId item) noexcept
{
    const std::uint16_t value = static_cast<std::uint16_t>(item);
    return value >= static_cast<std::uint16_t>(InventoryItemId::iron_laser_sword) &&
                   value <= static_cast<std::uint16_t>(InventoryItemId::blood_stone_laser_sword)
               ? 1.1F
               : 0.21F;
}

bool item_is_melee_weapon(InventoryItemId item) noexcept
{
    // InventoryItemClass.IsMeleeWeapon defaults to true. These are the only
    // original subclasses that override it: GunInventoryItemClass (including
    // the two fired-launcher pose IDs), Compass, Clock, GPS, and Grenade.
    return gun_stats(item) == nullptr && item != InventoryItemId::fired_rocket &&
           item != InventoryItemId::fired_guided_rocket && item != InventoryItemId::compass &&
           item != InventoryItemId::clock && item != InventoryItemId::gps &&
           item != InventoryItemId::teleport_gps && item != InventoryItemId::grenade &&
           item != InventoryItemId::modern_sticky_grenade;
}

std::optional<std::uint8_t> item_grenade_type(InventoryItemId item) noexcept
{
    if (item == InventoryItemId::grenade)
        return static_cast<std::uint8_t>(0U);
    if (item == InventoryItemId::modern_sticky_grenade)
        return static_cast<std::uint8_t>(3U);
    return std::nullopt;
}

world::BlockType item_block(InventoryItemId item) noexcept
{
    switch (item)
    {
    case InventoryItemId::dirt_block:
        return world::BlockType::dirt;
    case InventoryItemId::sand_block:
        return world::BlockType::sand;
    case InventoryItemId::rock_block:
        return world::BlockType::rock;
    case InventoryItemId::log_block:
        return world::BlockType::log;
    case InventoryItemId::wood_block:
        return world::BlockType::wood;
    case InventoryItemId::lantern_block:
        return world::BlockType::lantern;
    case InventoryItemId::blood_stone_block:
        return world::BlockType::blood_stone;
    case InventoryItemId::space_rock:
        return world::BlockType::space_rock;
    case InventoryItemId::iron_wall:
        return world::BlockType::iron_wall;
    case InventoryItemId::copper_wall:
        return world::BlockType::copper_wall;
    case InventoryItemId::golden_wall:
        return world::BlockType::golden_wall;
    case InventoryItemId::diamond_wall:
        return world::BlockType::diamond_wall;
    case InventoryItemId::torch:
        return world::BlockType::torch;
    case InventoryItemId::crate:
        return world::BlockType::crate;
    case InventoryItemId::snow:
        return world::BlockType::snow;
    case InventoryItemId::ice:
        return world::BlockType::ice;
    case InventoryItemId::door:
        return world::BlockType::lower_door_closed_z;
    case InventoryItemId::tnt:
        return world::BlockType::tnt;
    case InventoryItemId::c4:
        return world::BlockType::c4;
    case InventoryItemId::slime:
        return world::BlockType::slime;
    case InventoryItemId::space_rock_inventory:
        return world::BlockType::space_rock_inventory;
    case InventoryItemId::grass_block:
        return world::BlockType::grass;
    case InventoryItemId::gold_block:
        return world::BlockType::gold_ore;
    case InventoryItemId::iron_block:
        return world::BlockType::iron_ore;
    case InventoryItemId::copper_block:
        return world::BlockType::copper_ore;
    case InventoryItemId::coal_block:
        return world::BlockType::coal_ore;
    case InventoryItemId::diamond_block:
        return world::BlockType::diamond_ore;
    case InventoryItemId::surface_lava_block:
        return world::BlockType::surface_lava;
    case InventoryItemId::leaves_block:
        return world::BlockType::leaves;
    case InventoryItemId::modern_stone_container:
        return world::BlockType::crate_stone;
    case InventoryItemId::modern_copper_container:
        return world::BlockType::crate_copper;
    case InventoryItemId::modern_iron_container:
        return world::BlockType::crate_iron;
    case InventoryItemId::modern_gold_container:
        return world::BlockType::crate_gold;
    case InventoryItemId::modern_diamond_container:
        return world::BlockType::crate_diamond;
    case InventoryItemId::modern_bloodstone_container:
        return world::BlockType::crate_bloodstone;
    case InventoryItemId::modern_spawn_basic:
        return world::BlockType::spawn_point_basic;
    case InventoryItemId::modern_spawn_combat:
        return world::BlockType::spawn_point_combat;
    case InventoryItemId::modern_spawn_explorer:
        return world::BlockType::spawn_point_explorer;
    case InventoryItemId::modern_spawn_builder:
        return world::BlockType::spawn_point_builder;
    case InventoryItemId::modern_glass_window_wood:
        return world::BlockType::glass_basic;
    case InventoryItemId::modern_glass_window_iron:
        return world::BlockType::glass_iron;
    case InventoryItemId::modern_glass_window_gold:
        return world::BlockType::glass_strong;
    case InventoryItemId::modern_glass_window_diamond:
        return world::BlockType::glass_mystery;
    case InventoryItemId::modern_diamond_door:
    case InventoryItemId::modern_iron_door:
    case InventoryItemId::modern_tech_door:
        return world::BlockType::strong_lower_door;
    case InventoryItemId::modern_lantern_fancy_block:
        return world::BlockType::lantern_fancy;
    case InventoryItemId::modern_teleport_station:
        return world::BlockType::teleport_station;
    default:
        return world::BlockType::empty;
    }
}

bool is_block_item(InventoryItemId item) noexcept
{
    return item_block(item) != world::BlockType::empty || item == InventoryItemId::forced_air_block;
}

std::optional<std::uint8_t> door_model_name(InventoryItemId item) noexcept
{
    switch (item)
    {
    case InventoryItemId::door:
        return std::uint8_t{1U};
    case InventoryItemId::modern_iron_door:
        return std::uint8_t{2U};
    case InventoryItemId::modern_diamond_door:
        return std::uint8_t{3U};
    case InventoryItemId::modern_tech_door:
        return std::uint8_t{4U};
    default:
        return std::nullopt;
    }
}

std::optional<InventoryItemId> door_item_from_model(std::uint8_t model) noexcept
{
    switch (model)
    {
    case 1U:
        return InventoryItemId::door;
    case 2U:
        return InventoryItemId::modern_iron_door;
    case 3U:
        return InventoryItemId::modern_diamond_door;
    case 4U:
        return InventoryItemId::modern_tech_door;
    default:
        return std::nullopt;
    }
}

InventoryItemId block_item(world::BlockType block) noexcept
{
    switch (block)
    {
    case world::BlockType::dirt:
        return InventoryItemId::dirt_block;
    case world::BlockType::grass:
        return InventoryItemId::grass_block;
    case world::BlockType::sand:
        return InventoryItemId::sand_block;
    case world::BlockType::lantern:
    case world::BlockType::fixed_lantern:
        return InventoryItemId::lantern_block;
    case world::BlockType::rock:
        return InventoryItemId::rock_block;
    case world::BlockType::gold_ore:
        return InventoryItemId::gold_block;
    case world::BlockType::iron_ore:
        return InventoryItemId::iron_block;
    case world::BlockType::copper_ore:
        return InventoryItemId::copper_block;
    case world::BlockType::coal_ore:
        return InventoryItemId::coal_block;
    case world::BlockType::diamond_ore:
        return InventoryItemId::diamond_block;
    case world::BlockType::surface_lava:
        return InventoryItemId::surface_lava_block;
    case world::BlockType::snow:
        return InventoryItemId::snow;
    case world::BlockType::ice:
        return InventoryItemId::ice;
    case world::BlockType::log:
        return InventoryItemId::log_block;
    case world::BlockType::leaves:
        return InventoryItemId::leaves_block;
    case world::BlockType::wood:
        return InventoryItemId::wood_block;
    case world::BlockType::blood_stone:
        return InventoryItemId::blood_stone_block;
    case world::BlockType::space_rock:
        return InventoryItemId::space_rock;
    case world::BlockType::space_rock_inventory:
        return InventoryItemId::space_rock_inventory;
    case world::BlockType::iron_wall:
        return InventoryItemId::iron_wall;
    case world::BlockType::copper_wall:
        return InventoryItemId::copper_wall;
    case world::BlockType::golden_wall:
        return InventoryItemId::golden_wall;
    case world::BlockType::diamond_wall:
        return InventoryItemId::diamond_wall;
    case world::BlockType::torch:
    case world::BlockType::torch_pos_x:
    case world::BlockType::torch_neg_z:
    case world::BlockType::torch_neg_x:
    case world::BlockType::torch_pos_z:
    case world::BlockType::torch_pos_y:
    case world::BlockType::torch_neg_y:
        return InventoryItemId::torch;
    case world::BlockType::crate:
        return InventoryItemId::crate;
    case world::BlockType::lower_door_closed_z:
    case world::BlockType::lower_door_closed_x:
    case world::BlockType::lower_door:
    case world::BlockType::upper_door_closed:
    case world::BlockType::lower_door_open_z:
    case world::BlockType::lower_door_open_x:
    case world::BlockType::upper_door_open:
        return InventoryItemId::door;
    case world::BlockType::tnt:
        return InventoryItemId::tnt;
    case world::BlockType::c4:
        return InventoryItemId::c4;
    case world::BlockType::slime:
        return InventoryItemId::slime;
    case world::BlockType::glass_basic:
        return InventoryItemId::modern_glass_window_wood;
    case world::BlockType::glass_iron:
        return InventoryItemId::modern_glass_window_iron;
    case world::BlockType::glass_strong:
        return InventoryItemId::modern_glass_window_gold;
    case world::BlockType::glass_mystery:
        return InventoryItemId::modern_glass_window_diamond;
    case world::BlockType::crate_stone:
        return InventoryItemId::modern_stone_container;
    case world::BlockType::crate_copper:
        return InventoryItemId::modern_copper_container;
    case world::BlockType::crate_iron:
        return InventoryItemId::modern_iron_container;
    case world::BlockType::crate_gold:
        return InventoryItemId::modern_gold_container;
    case world::BlockType::crate_diamond:
        return InventoryItemId::modern_diamond_container;
    case world::BlockType::crate_bloodstone:
        return InventoryItemId::modern_bloodstone_container;
    case world::BlockType::spawn_point_basic:
        return InventoryItemId::modern_spawn_basic;
    case world::BlockType::spawn_point_builder:
        return InventoryItemId::modern_spawn_builder;
    case world::BlockType::spawn_point_combat:
        return InventoryItemId::modern_spawn_combat;
    case world::BlockType::spawn_point_explorer:
        return InventoryItemId::modern_spawn_explorer;
    case world::BlockType::strong_lower_door_closed_z:
    case world::BlockType::strong_lower_door_closed_x:
    case world::BlockType::strong_lower_door:
    case world::BlockType::strong_upper_door_closed:
    case world::BlockType::strong_lower_door_open_z:
    case world::BlockType::strong_lower_door_open_x:
    case world::BlockType::strong_upper_door_open:
        // BlockClasses[parent 62] is overwritten by the final Tech-door
        // registration. Properly placed doors use DoorClasses[model] instead.
        return InventoryItemId::modern_tech_door;
    case world::BlockType::lantern_fancy:
        return InventoryItemId::modern_lantern_fancy_block;
    case world::BlockType::teleport_station:
        return InventoryItemId::modern_teleport_station;
    default:
        return InventoryItemId::bare_hands;
    }
}

InventoryItemId mined_item(world::BlockType block) noexcept
{
    switch (block)
    {
    case world::BlockType::grass:
        return InventoryItemId::dirt_block;
    case world::BlockType::gold_ore:
        return InventoryItemId::gold_ore;
    case world::BlockType::iron_ore:
        return InventoryItemId::iron_ore;
    case world::BlockType::copper_ore:
        return InventoryItemId::copper_ore;
    case world::BlockType::coal_ore:
        return InventoryItemId::coal;
    case world::BlockType::diamond_ore:
        return InventoryItemId::diamond;
    case world::BlockType::surface_lava:
        return InventoryItemId::rock_block;
    case world::BlockType::space_rock:
        return InventoryItemId::space_rock_inventory;
    case world::BlockType::deep_lava:
    case world::BlockType::bedrock:
    case world::BlockType::empty:
    case world::BlockType::uninitialized:
        return InventoryItemId::bare_hands;
    default:
        return block_item(block);
    }
}

std::uint16_t item_stack_limit(InventoryItemId item) noexcept
{
    return item_stack_limit(item, InventoryProfile::classic);
}

std::uint16_t item_stack_limit(InventoryItemId item, InventoryProfile profile) noexcept
{
    const auto value = static_cast<std::uint16_t>(item);
    if (value >= static_cast<std::uint16_t>(InventoryItemId::modern_chainsaw_1))
    {
        switch (item)
        {
        case InventoryItemId::modern_sticky_grenade:
        case InventoryItemId::modern_snowball:
        case InventoryItemId::modern_iceball:
            return 10U;
        case InventoryItemId::modern_stone_container:
        case InventoryItemId::modern_copper_container:
        case InventoryItemId::modern_iron_container:
        case InventoryItemId::modern_gold_container:
        case InventoryItemId::modern_diamond_container:
        case InventoryItemId::modern_bloodstone_container:
        case InventoryItemId::modern_spawn_basic:
        case InventoryItemId::modern_spawn_combat:
        case InventoryItemId::modern_spawn_explorer:
        case InventoryItemId::modern_spawn_builder:
        case InventoryItemId::modern_glass_window_wood:
        case InventoryItemId::modern_glass_window_iron:
        case InventoryItemId::modern_glass_window_gold:
        case InventoryItemId::modern_glass_window_diamond:
        case InventoryItemId::modern_diamond_door:
        case InventoryItemId::modern_lantern_fancy_block:
        case InventoryItemId::modern_monster_block:
        case InventoryItemId::modern_iron_door:
        case InventoryItemId::modern_tech_door:
            return 999U;
        default:
            return 1U;
        }
    }

    if (profile == InventoryProfile::modern)
    {
        if (value >= 1U && value <= 12U)
            return 999U;
        if (value == 13U)
            return 64U;
        if (value == 14U)
            return 999U;
        if (value >= 15U && value <= 22U)
            return 255U;
        if (value >= 23U && value <= 40U)
            return 1U;
        if (value >= 41U && value <= 48U)
            return 5000U;
        if (value >= 49U && value <= 72U)
            return 1U;
        if (value >= 73U && value <= 76U)
            return 999U;
        if (value == 77U || value == 78U)
            return 1U;
        if (value == 79U || value == 80U)
            return 999U;
        if (value >= 81U && value <= 101U)
            return 1U;
        if (value == 102U)
            return 999U;
        if (value == 103U || value == 104U)
            return 1U;
        if (value == 105U || value == 109U || value == 111U)
            return 5000U;
        if (value == 106U || value == 108U)
            return 64U;
        if (value == 107U)
            return 999U;
        if (value == 110U)
            return 10U;
        if (value >= 112U && value <= 122U)
            return 1U;
    }

    switch (item)
    {
    case InventoryItemId::brass_casing:
    case InventoryItemId::iron_casing:
    case InventoryItemId::gold_casing:
    case InventoryItemId::diamond_casing:
    case InventoryItemId::bullets:
    case InventoryItemId::iron_bullets:
    case InventoryItemId::gold_bullets:
    case InventoryItemId::diamond_bullets:
    case InventoryItemId::blood_stone_bullets:
    case InventoryItemId::laser_bullets:
    case InventoryItemId::rocket_ammo:
        return 5000U;
    case InventoryItemId::grenade:
        return 10U; // GrenadeInventoryItemClass registers a max stack of 10.
    default:
        break;
    }
    const bool tool = (value >= 23U && value <= 40U) || (value >= 49U && value <= 72U) ||
                      (value >= 77U && value <= 78U) || (value >= 81U && value <= 104U) ||
                      (value >= 111U && value <= 122U);
    return tool || item == InventoryItemId::bare_hands ? 1U : 64U;
}

std::array<float, 3> item_material_tint(InventoryItemId item) noexcept
{
    const auto rgb = [](int red, int green, int blue) {
        constexpr float scale = 1.0F / 255.0F;
        return std::array<float, 3>{red * scale, green * scale, blue * scale};
    };
    const auto stone = rgb(169, 169, 169); // XNA Color.DarkGray
    const auto copper = rgb(184, 115, 51);
    const auto iron = rgb(128, 128, 128);
    const auto gold = rgb(255, 215, 0);
    const auto diamond = rgb(0, 255, 255); // XNA Color.Cyan
    const auto blood = rgb(139, 0, 0);     // XNA Color.DarkRed
    switch (item)
    {
    case InventoryItemId::stone_pickaxe:
    case InventoryItemId::stone_spade:
    case InventoryItemId::stone_axe:
        return stone;
    case InventoryItemId::copper_pickaxe:
    case InventoryItemId::copper_spade:
    case InventoryItemId::copper_axe:
        return copper;
    case InventoryItemId::iron_pickaxe:
    case InventoryItemId::iron_spade:
    case InventoryItemId::iron_axe:
    case InventoryItemId::knife:
        return iron;
    case InventoryItemId::gold_pickaxe:
    case InventoryItemId::gold_spade:
    case InventoryItemId::gold_axe:
    case InventoryItemId::gold_knife:
        return gold;
    case InventoryItemId::diamond_pickaxe:
    case InventoryItemId::diamond_spade:
    case InventoryItemId::diamond_axe:
    case InventoryItemId::diamond_knife:
        return diamond;
    case InventoryItemId::bloodstone_pickaxe:
    case InventoryItemId::blood_stone_knife:
    case InventoryItemId::modern_chainsaw_1:
    case InventoryItemId::modern_laser_drill:
    case InventoryItemId::modern_precision_laser:
        return blood;
    default:
        break;
    }
    switch (item)
    {
    // Laser swords tint through GetLaserMaterialcColor's blade colors.
    case InventoryItemId::iron_laser_sword:
        return rgb(255, 40, 40);
    case InventoryItemId::copper_laser_sword:
        return rgb(50, 205, 50);
    case InventoryItemId::gold_laser_sword:
        return rgb(255, 255, 0);
    case InventoryItemId::diamond_laser_sword:
        return rgb(60, 60, 255);
    case InventoryItemId::blood_stone_laser_sword:
        return blood;
    case InventoryItemId::space_knife:
        return rgb(50, 205, 50);
    case InventoryItemId::brass_casing:
        return rgb(234, 227, 150);
    case InventoryItemId::iron_casing:
        return iron;
    case InventoryItemId::gold_casing:
        return gold;
    case InventoryItemId::diamond_casing:
        return diamond;
    case InventoryItemId::explosive_powder:
        return rgb(255, 70, 40);
    default:
        break;
    }
    const auto value = static_cast<std::uint16_t>(item);
    if ((value >= 50U && value <= 54U) || (value >= 82U && value <= 86U) || value == 119U)
        return iron;
    if (value >= 87U && value <= 91U)
        return copper;
    if ((value >= 56U && value <= 60U) || (value >= 92U && value <= 96U) || value == 120U)
        return gold;
    if ((value >= 62U && value <= 66U) || (value >= 97U && value <= 101U) || value == 121U)
        return diamond;
    if ((value >= 68U && value <= 72U) || value == 122U)
        return blood;
    return {1.0F, 1.0F, 1.0F};
}

const GunStats *gun_stats(InventoryItemId item) noexcept
{
    // Field order: fire cooldown, reload seconds, shoulder magnification, clip capacity,
    // automatic trigger, ammo item (filled per tier below), shoot cue, reload cue. Values
    // come from the original *InventoryItemClass constructors.
    static const auto table = [] {
        std::array<std::optional<GunStats>, static_cast<std::size_t>(InventoryItemId::item_count)>
            result{};
        using Id = InventoryItemId;
        const auto set = [&](Id id, GunStats stats, Id ammo, float enemy_damage) {
            stats.ammo_item = ammo;
            stats.enemy_damage = enemy_damage;
            if (stats.rounds_per_reload == 0U)
                stats.rounds_per_reload = stats.clip_capacity;
            result[static_cast<std::size_t>(id)] = stats;
        };
        GunStats pistol{0.1F, 1.63F, 1.2F, 8U, false, Id::bare_hands, "GunShot4", "Reload"};
        pistol.inaccuracy = 0.05F;
        pistol.tracer_velocity = 75.0F;
        pistol.tracer_flight_seconds = 1.0F;
        GunStats assault{0.1F, 3.0F, 2.0F, 30U, true, Id::bare_hands, "GunShot3", "AssaultReload"};
        assault.inaccuracy = 0.02F;
        GunStats shotgun{1.0F, 0.567F, 1.0F, 6U, false, Id::bare_hands, "Shotgun", "ShotGunReload"};
        shotgun.rounds_per_reload = 1U;
        shotgun.inaccuracy = 0.1F;
        shotgun.recoil_radians = 0.17453293F;
        shotgun.tracer_velocity = 50.0F;
        shotgun.tracer_flight_seconds = 0.4F;
        GunStats bolt{0.0031746032F, 2.4F,           1.5F,       8U,
                      false,         Id::bare_hands, "GunShot1", "AssaultReload"};
        bolt.recoil_radians = 0.20943952F;
        bolt.tracer_velocity = 150.0F;
        GunStats smg{0.0640341515F, 2.0F, 1.1F, 20U, true, Id::bare_hands, "GunShot2", "Reload"};
        smg.inaccuracy = 0.1F;
        smg.tracer_flight_seconds = 1.0F;
        GunStats lmg{0.112F, 9.7F, 1.3F, 100U, true, Id::bare_hands, "GunShot2", "Reload"};
        lmg.inaccuracy = 0.1F;
        lmg.tracer_flight_seconds = 1.0F;
        GunStats rpg{1.0F, 0.567F, 1.0F, 1U, false, Id::bare_hands, "RPGLaunch", "ShotGunReload"};
        rpg.inaccuracy = 0.1F;
        rpg.recoil_radians = 0.17453293F;
        rpg.tracer_velocity = 50.0F;
        rpg.tracer_flight_seconds = 0.4F;
        GunStats laser_pistol{0.06F, 1.25F, 1.2F, 7U, false, Id::bare_hands, "LaserGun4", "Reload"};
        laser_pistol.inaccuracy = 0.05F;
        laser_pistol.needs_drop_compensation = false;
        GunStats laser_assault{0.06F, 2.5F,           2.0F,        30U,
                               true,  Id::bare_hands, "LaserGun3", "AssaultReload"};
        laser_assault.inaccuracy = 0.02F;
        laser_assault.needs_drop_compensation = false;
        GunStats laser_smg{0.06F, 2.05F, 1.1F, 20U, true, Id::bare_hands, "LaserGun2", "Reload"};
        laser_smg.inaccuracy = 0.1F;
        laser_smg.needs_drop_compensation = false;
        GunStats laser_rifle{0.26F, 2.95F,          2.5F,        10U,
                             false, Id::bare_hands, "LaserGun1", "AssaultReload"};
        laser_rifle.needs_drop_compensation = false;
        GunStats laser_shotgun{0.19F, 0.57F,          1.0F,        8U,
                               false, Id::bare_hands, "LaserGun5", "ShotGunReload"};
        laser_shotgun.rounds_per_reload = 1U;
        laser_shotgun.inaccuracy = 0.1F;
        laser_shotgun.needs_drop_compensation = false;
        // Conventional ammo ladder: base->Bullets, gold->IronBullets, diamond->GoldBullets,
        // bloodstone->DiamondBullets.
        set(Id::assault_rifle, assault, Id::bullets, 0.5F);
        set(Id::gold_assault_rifle, assault, Id::iron_bullets, 2.5F);
        set(Id::diamond_assault_rifle, assault, Id::gold_bullets, 6.0F);
        set(Id::blood_stone_assault_rifle, assault, Id::diamond_bullets, 12.0F);
        set(Id::pistol, pistol, Id::bullets, 0.3F);
        set(Id::gold_pistol, pistol, Id::iron_bullets, 1.0F);
        set(Id::diamond_pistol, pistol, Id::gold_bullets, 4.0F);
        set(Id::blood_stone_pistol, pistol, Id::diamond_bullets, 8.0F);
        set(Id::pump_shotgun, shotgun, Id::bullets, 0.3F);
        set(Id::gold_pump_shotgun, shotgun, Id::iron_bullets, 1.0F);
        set(Id::diamond_pump_shotgun, shotgun, Id::gold_bullets, 4.0F);
        set(Id::blood_stone_pump_shotgun, shotgun, Id::diamond_bullets, 8.0F);
        set(Id::bolt_action_rifle, bolt, Id::bullets, 0.5F);
        set(Id::gold_bolt_action_rifle, bolt, Id::iron_bullets, 2.5F);
        set(Id::diamond_bolt_action_rifle, bolt, Id::gold_bullets, 6.0F);
        set(Id::blood_stone_bolt_action_rifle, bolt, Id::diamond_bullets, 12.0F);
        set(Id::smg, smg, Id::bullets, 0.3F);
        set(Id::gold_smg, smg, Id::iron_bullets, 1.0F);
        set(Id::diamond_smg, smg, Id::gold_bullets, 4.0F);
        set(Id::blood_stone_smg, smg, Id::diamond_bullets, 8.0F);
        set(Id::lmg, lmg, Id::bullets, 0.5F);
        set(Id::gold_lmg, lmg, Id::iron_bullets, 2.5F);
        set(Id::diamond_lmg, lmg, Id::gold_bullets, 6.0F);
        set(Id::blood_stone_lmg, lmg, Id::diamond_bullets, 12.0F);
        // Every space weapon uses laser bullets.
        for (const Id id : {Id::iron_space_assault_rifle, Id::copper_space_assault_rifle,
                            Id::gold_space_assault_rifle, Id::diamond_space_assault_rifle})
            set(id, laser_assault, Id::laser_bullets, 15.0F);
        for (const Id id : {Id::iron_space_pistol, Id::copper_space_pistol, Id::gold_space_pistol,
                            Id::diamond_space_pistol})
            set(id, laser_pistol, Id::laser_bullets, 10.0F);
        for (const Id id : {Id::iron_space_pump_shotgun, Id::copper_space_pump_shotgun,
                            Id::gold_space_pump_shotgun, Id::diamond_space_pump_shotgun})
            set(id, laser_shotgun, Id::laser_bullets, 10.0F);
        for (const Id id : {Id::iron_space_bolt_action_rifle, Id::copper_space_bolt_action_rifle,
                            Id::gold_space_bolt_action_rifle, Id::diamond_space_bolt_action_rifle})
            set(id, laser_rifle, Id::laser_bullets, 15.0F);
        for (const Id id :
             {Id::iron_space_smg, Id::copper_space_smg, Id::gold_space_smg, Id::diamond_space_smg})
            set(id, laser_smg, Id::laser_bullets, 10.0F);
        set(Id::rocket_launcher, rpg, Id::rocket_ammo, 100.0F);
        GunStats guided_rpg = rpg;
        guided_rpg.shoulder_magnification = 2.0F;
        set(Id::guided_rocket_launcher, guided_rpg, Id::rocket_ammo, 100.0F);

        // Modern's three additional registered guns. Enum values that only
        // represent alternate/state items intentionally have no table entry.
        GunStats grenade_launcher = rpg;
        grenade_launcher.inaccuracy = 0.17453293F;     // authored maximum: 10 degrees
        grenade_launcher.recoil_radians = 0.20943952F; // 12 degrees
        set(Id::modern_basic_grenade_launcher, grenade_launcher, Id::rocket_ammo, 100.0F);

        GunStats laser_drill{0.05F, 3.0F,           1.15F,       200U,
                             true,  Id::bare_hands, "LaserGun3", "AssaultReload"};
        laser_drill.inaccuracy = 0.06981317F; // authored maximum: 4 degrees
        laser_drill.recoil_radians = 0.034906585F;
        laser_drill.needs_drop_compensation = false;
        set(Id::modern_laser_drill, laser_drill, Id::gold_bullets, 0.2F);

        GunStats precision_laser{0.05F, 3.0F,           3.45F,       10U,
                                 false, Id::bare_hands, "LaserGun3", "AssaultReload"};
        precision_laser.inaccuracy = 0.0034906585F; // authored maximum: .2 degrees
        precision_laser.recoil_radians = 0.0017453293F;
        precision_laser.needs_drop_compensation = false;
        set(Id::modern_precision_laser, precision_laser, Id::gold_bullets, 25.0F);
        return result;
    }();
    const auto index = static_cast<std::size_t>(item);
    if (index >= table.size() || !table[index].has_value())
        return nullptr;
    return &*table[index];
}

GunReloadCycle gun_reload_cycle(const GunStats &stats, std::uint16_t rounds_in_clip,
                                std::uint32_t reserve_ammo) noexcept
{
    if (rounds_in_clip >= stats.clip_capacity || reserve_ammo == 0U)
        return {};
    const std::uint32_t needed = stats.clip_capacity - rounds_in_clip;
    const std::uint32_t per_cycle = std::max<std::uint32_t>(1U, stats.rounds_per_reload);
    const auto loaded = static_cast<std::uint16_t>(std::min({needed, reserve_ammo, per_cycle}));
    return {loaded, rounds_in_clip + loaded < stats.clip_capacity && reserve_ammo > loaded};
}

GunRecoil gun_recoil(float amount_radians, float yaw_roll, float pitch_roll) noexcept
{
    const float yaw_sample = std::clamp(yaw_roll, 0.0F, 1.0F) * 0.5F - 0.25F;
    const float pitch_sample = 0.5F + std::clamp(pitch_roll, 0.0F, 1.0F) * 0.5F;
    return {yaw_sample * amount_radians, pitch_sample * amount_radians};
}

const std::vector<CraftingRecipe> &crafting_recipes() noexcept
{
    // The complete static Receipe.CookBook, in authored order.
    static const std::vector<CraftingRecipe> cookbook = [] {
        using Id = InventoryItemId;
        std::vector<CraftingRecipe> book;
        book.reserve(101U);
        const auto add = [&](Id result, std::uint16_t count,
                             std::initializer_list<CraftingIngredient> parts) {
            CraftingRecipe recipe;
            recipe.result = result;
            recipe.result_count = count;
            std::size_t index = 0;
            for (const CraftingIngredient &part : parts)
                recipe.ingredients[index++] = part;
            book.push_back(recipe);
        };
        add(Id::wood_block, 4U, {{Id::log_block, 1U}});
        add(Id::stick, 4U, {{Id::wood_block, 1U}});
        add(Id::torch, 4U, {{Id::coal, 1U}, {Id::stick, 1U}});
        add(Id::lantern_block, 4U,
            {{Id::torch, 1U}, {Id::iron, 1U}, {Id::sand_block, 4U}, {Id::coal, 2U}});
        add(Id::brass_casing, 200U, {{Id::copper, 1U}});
        add(Id::iron_casing, 200U, {{Id::iron, 1U}});
        add(Id::gold_casing, 200U, {{Id::gold, 1U}});
        add(Id::diamond_casing, 100U, {{Id::diamond, 1U}});
        add(Id::bullets, 100U, {{Id::rock_block, 1U}, {Id::brass_casing, 100U}, {Id::coal, 1U}});
        add(Id::iron_bullets, 100U, {{Id::iron, 1U}, {Id::brass_casing, 100U}, {Id::coal, 1U}});
        add(Id::gold_bullets, 100U, {{Id::gold, 1U}, {Id::iron_casing, 100U}, {Id::coal, 1U}});
        add(Id::diamond_bullets, 100U,
            {{Id::diamond, 1U}, {Id::gold_casing, 100U}, {Id::coal, 1U}});
        add(Id::laser_bullets, 100U, {{Id::space_rock_inventory, 1U}, {Id::diamond_casing, 100U}});
        add(Id::compass, 1U, {{Id::sand_block, 1U}, {Id::coal, 1U}, {Id::iron, 1U}});
        add(Id::clock, 1U,
            {{Id::sand_block, 1U}, {Id::coal, 1U}, {Id::wood_block, 1U}, {Id::copper, 1U}});
        add(Id::gps, 1U, {{Id::sand_block, 1U}, {Id::coal, 1U}, {Id::iron, 1U}, {Id::gold, 1U}});
        add(Id::teleport_gps, 1U,
            {{Id::sand_block, 1U}, {Id::coal, 1U}, {Id::blood_stone_block, 1U}, {Id::diamond, 1U}});
        add(Id::crate, 1U, {{Id::wood_block, 10U}, {Id::iron, 2U}});
        add(Id::stone_pickaxe, 1U, {{Id::rock_block, 4U}, {Id::stick, 2U}});
        add(Id::copper_pickaxe, 1U, {{Id::copper, 2U}, {Id::stick, 2U}});
        add(Id::iron_pickaxe, 1U, {{Id::iron, 2U}, {Id::stick, 2U}});
        add(Id::gold_pickaxe, 1U, {{Id::gold, 2U}, {Id::iron, 3U}});
        add(Id::diamond_pickaxe, 1U, {{Id::diamond, 2U}, {Id::gold, 3U}});
        add(Id::bloodstone_pickaxe, 1U, {{Id::blood_stone_block, 10U}, {Id::diamond, 3U}});
        add(Id::stone_spade, 1U, {{Id::rock_block, 2U}, {Id::stick, 2U}});
        add(Id::copper_spade, 1U, {{Id::copper, 1U}, {Id::stick, 2U}});
        add(Id::iron_spade, 1U, {{Id::iron, 1U}, {Id::stick, 2U}});
        add(Id::gold_spade, 1U, {{Id::gold, 1U}, {Id::iron, 2U}});
        add(Id::diamond_spade, 1U, {{Id::diamond, 1U}, {Id::gold, 2U}});
        add(Id::stone_axe, 1U, {{Id::rock_block, 4U}, {Id::stick, 2U}});
        add(Id::copper_axe, 1U, {{Id::copper, 2U}, {Id::stick, 2U}});
        add(Id::iron_axe, 1U, {{Id::iron, 2U}, {Id::stick, 2U}});
        add(Id::gold_axe, 1U, {{Id::gold, 2U}, {Id::iron, 2U}});
        add(Id::diamond_axe, 1U, {{Id::diamond, 2U}, {Id::gold, 2U}});
        add(Id::iron, 1U, {{Id::iron_ore, 2U}, {Id::coal, 1U}});
        add(Id::gold, 1U, {{Id::gold_ore, 2U}, {Id::coal, 1U}});
        add(Id::copper, 1U, {{Id::copper_ore, 2U}, {Id::coal, 1U}});
        add(Id::iron, 1U, {{Id::iron_ore, 2U}, {Id::log_block, 1U}});
        add(Id::gold, 1U, {{Id::gold_ore, 2U}, {Id::log_block, 1U}});
        add(Id::copper, 1U, {{Id::copper_ore, 2U}, {Id::log_block, 1U}});
        add(Id::knife, 1U, {{Id::iron, 1U}, {Id::wood_block, 1U}});
        add(Id::gold_knife, 1U, {{Id::gold, 1U}, {Id::iron, 1U}});
        add(Id::diamond_knife, 1U, {{Id::diamond, 1U}, {Id::gold, 1U}});
        add(Id::blood_stone_knife, 1U, {{Id::blood_stone_block, 10U}, {Id::diamond, 1U}});
        add(Id::iron_laser_sword, 1U, {{Id::iron, 2U}, {Id::space_rock_inventory, 4U}});
        add(Id::copper_laser_sword, 1U, {{Id::copper, 2U}, {Id::space_rock_inventory, 4U}});
        add(Id::gold_laser_sword, 1U, {{Id::gold, 2U}, {Id::space_rock_inventory, 4U}});
        add(Id::diamond_laser_sword, 1U, {{Id::diamond, 2U}, {Id::space_rock_inventory, 4U}});
        add(Id::blood_stone_laser_sword, 1U,
            {{Id::blood_stone_block, 2U}, {Id::space_rock_inventory, 4U}});
        add(Id::pistol, 1U, {{Id::iron, 2U}, {Id::wood_block, 2U}});
        add(Id::gold_pistol, 1U, {{Id::gold, 2U}, {Id::iron, 2U}});
        add(Id::diamond_pistol, 1U, {{Id::diamond, 2U}, {Id::gold, 2U}});
        add(Id::blood_stone_pistol, 1U, {{Id::blood_stone_block, 30U}, {Id::diamond, 2U}});
        add(Id::iron_space_pistol, 1U, {{Id::slime, 2U}, {Id::diamond, 2U}, {Id::iron, 1U}});
        add(Id::copper_space_pistol, 1U, {{Id::slime, 2U}, {Id::diamond, 2U}, {Id::copper, 1U}});
        add(Id::gold_space_pistol, 1U, {{Id::slime, 2U}, {Id::diamond, 2U}, {Id::gold, 1U}});
        add(Id::diamond_space_pistol, 1U, {{Id::slime, 2U}, {Id::diamond, 3U}});
        add(Id::lmg, 1U, {{Id::iron, 6U}, {Id::wood_block, 2U}});
        add(Id::gold_lmg, 1U, {{Id::gold, 6U}, {Id::iron, 3U}});
        add(Id::diamond_lmg, 1U, {{Id::diamond, 6U}, {Id::gold, 3U}});
        add(Id::blood_stone_lmg, 1U, {{Id::blood_stone_block, 60U}, {Id::diamond, 6U}});
        add(Id::smg, 1U, {{Id::iron, 3U}, {Id::wood_block, 2U}});
        add(Id::gold_smg, 1U, {{Id::gold, 3U}, {Id::iron, 2U}});
        add(Id::diamond_smg, 1U, {{Id::diamond, 3U}, {Id::gold, 2U}});
        add(Id::blood_stone_smg, 1U, {{Id::blood_stone_block, 20U}, {Id::diamond, 3U}});
        add(Id::iron_space_smg, 1U, {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::iron, 1U}});
        add(Id::copper_space_smg, 1U, {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::copper, 1U}});
        add(Id::gold_space_smg, 1U, {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::gold, 1U}});
        add(Id::diamond_space_smg, 1U, {{Id::slime, 3U}, {Id::diamond, 4U}});
        add(Id::bolt_action_rifle, 1U, {{Id::iron, 3U}, {Id::wood_block, 2U}});
        add(Id::gold_bolt_action_rifle, 1U, {{Id::gold, 3U}, {Id::iron, 2U}});
        add(Id::diamond_bolt_action_rifle, 1U, {{Id::diamond, 3U}, {Id::gold, 2U}});
        add(Id::blood_stone_bolt_action_rifle, 1U,
            {{Id::blood_stone_block, 20U}, {Id::diamond, 3U}});
        add(Id::iron_space_bolt_action_rifle, 1U,
            {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::iron, 1U}});
        add(Id::copper_space_bolt_action_rifle, 1U,
            {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::copper, 1U}});
        add(Id::gold_space_bolt_action_rifle, 1U,
            {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::gold, 1U}});
        add(Id::diamond_space_bolt_action_rifle, 1U, {{Id::slime, 3U}, {Id::diamond, 4U}});
        add(Id::pump_shotgun, 1U, {{Id::iron, 3U}, {Id::wood_block, 2U}});
        add(Id::gold_pump_shotgun, 1U, {{Id::gold, 3U}, {Id::iron, 2U}});
        add(Id::diamond_pump_shotgun, 1U, {{Id::diamond, 3U}, {Id::gold, 2U}});
        add(Id::blood_stone_pump_shotgun, 1U, {{Id::blood_stone_block, 20U}, {Id::diamond, 3U}});
        add(Id::iron_space_pump_shotgun, 1U, {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::iron, 1U}});
        add(Id::copper_space_pump_shotgun, 1U,
            {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::copper, 1U}});
        add(Id::gold_space_pump_shotgun, 1U, {{Id::slime, 3U}, {Id::diamond, 3U}, {Id::gold, 1U}});
        add(Id::diamond_space_pump_shotgun, 1U, {{Id::slime, 3U}, {Id::diamond, 4U}});
        add(Id::assault_rifle, 1U, {{Id::iron, 5U}, {Id::wood_block, 3U}});
        add(Id::gold_assault_rifle, 1U, {{Id::gold, 5U}, {Id::iron, 3U}});
        add(Id::diamond_assault_rifle, 1U, {{Id::diamond, 5U}, {Id::gold, 3U}});
        add(Id::blood_stone_assault_rifle, 1U, {{Id::blood_stone_block, 50U}, {Id::diamond, 6U}});
        add(Id::iron_space_assault_rifle, 1U, {{Id::slime, 5U}, {Id::diamond, 6U}, {Id::iron, 1U}});
        add(Id::copper_space_assault_rifle, 1U,
            {{Id::slime, 5U}, {Id::diamond, 6U}, {Id::copper, 1U}});
        add(Id::gold_space_assault_rifle, 1U, {{Id::slime, 5U}, {Id::diamond, 6U}, {Id::gold, 1U}});
        add(Id::diamond_space_assault_rifle, 1U, {{Id::slime, 5U}, {Id::diamond, 7U}});
        add(Id::copper_wall, 1U, {{Id::copper, 2U}});
        add(Id::copper, 1U, {{Id::copper_wall, 1U}});
        add(Id::iron_wall, 1U, {{Id::iron, 2U}});
        add(Id::iron, 1U, {{Id::iron_wall, 1U}});
        add(Id::golden_wall, 1U, {{Id::gold, 2U}});
        add(Id::gold, 1U, {{Id::golden_wall, 1U}});
        add(Id::diamond_wall, 1U, {{Id::diamond, 2U}});
        add(Id::diamond, 1U, {{Id::diamond_wall, 1U}});
        add(Id::tnt, 1U, {{Id::explosive_powder, 1U}, {Id::sand_block, 3U}, {Id::coal, 3U}});
        add(Id::c4, 1U, {{Id::explosive_powder, 3U}, {Id::sand_block, 3U}, {Id::coal, 3U}});
        add(Id::grenade, 1U, {{Id::explosive_powder, 1U}, {Id::iron, 1U}});
        add(Id::rocket_launcher, 1U,
            {{Id::explosive_powder, 2U}, {Id::iron, 3U}, {Id::wood_block, 2U}});
        add(Id::guided_rocket_launcher, 1U,
            {{Id::explosive_powder, 2U},
             {Id::iron, 3U},
             {Id::copper, 2U},
             {Id::space_rock_inventory, 1U}});
        add(Id::door, 1U, {{Id::wood_block, 5U}, {Id::iron, 1U}});
        return book;
    }();
    return cookbook;
}

float item_use_cooldown_seconds(InventoryItemId item) noexcept
{
    // InventoryItemClass._coolDownTime from each authored constructor.
    if (is_block_item(item))
        return 0.1F;
    if (const GunStats *gun = gun_stats(item); gun != nullptr)
        return gun->fire_cooldown_seconds;
    switch (item)
    {
    case InventoryItemId::bare_hands:
    case InventoryItemId::knife:
        return 0.5F;
    case InventoryItemId::gold_knife:
        return 0.4F;
    case InventoryItemId::diamond_knife:
        return 0.3F;
    case InventoryItemId::blood_stone_knife:
        return 0.25F;
    case InventoryItemId::grenade:
    case InventoryItemId::fired_rocket:
    case InventoryItemId::fired_guided_rocket:
        return 1.0F;
    default:
        // Pick, spade, axe, laser sword, stick, material, ammo, casing,
        // compass/clock/GPS, and the remaining ModelInventoryItemClass items.
        return 0.3F;
    }
}

float item_fire_animation_seconds(InventoryItemId item, bool shouldering) noexcept
{
    // Durations from the shipped avatar clips selected by Player.PlayerMode.
    // The shoulder variants are distinct assets for several space weapons.
    switch (item_player_animation_mode(item))
    {
    case PlayerAnimationMode::assault:
    case PlayerAnimationMode::bolt_rifle:
    case PlayerAnimationMode::smg:
    case PlayerAnimationMode::lmg:
        return 0.12F;
    case PlayerAnimationMode::pistol:
        return 0.15F;
    case PlayerAnimationMode::pump_shotgun:
        return 1.42F;
    case PlayerAnimationMode::space_assault:
    case PlayerAnimationMode::space_bolt_rifle:
        return shouldering ? 0.10F : 0.15F;
    case PlayerAnimationMode::space_pistol:
    case PlayerAnimationMode::space_smg:
        return 0.15F;
    case PlayerAnimationMode::space_pump_shotgun:
        return 0.98F;
    case PlayerAnimationMode::rpg:
        return shouldering ? 1.42F : 0.98F;
    default:
        return 0.15F;
    }
}

PlayerInventory::PlayerInventory()
{
    // CastleMinerZGame.SecondaryLoad creates this exact non-hardcore loadout.
    const std::array defaults{
        InventoryStack{InventoryItemId::stone_pickaxe, 1U},
        InventoryStack{InventoryItemId::compass, 1U},
        InventoryStack{InventoryItemId::pistol, 1U},
        InventoryStack{InventoryItemId::knife, 1U},
        InventoryStack{InventoryItemId::bullets, 200U},
        InventoryStack{InventoryItemId::torch, 16U},
    };
    for (std::size_t index = 0; index < defaults.size(); ++index)
        trays_.front()[index] = defaults[index];
}

void PlayerInventory::select(std::size_t index) noexcept
{
    selected_index_ = std::min(index, tray_size - 1U);
}

void PlayerInventory::select_relative(int direction) noexcept
{
    const int size = static_cast<int>(tray_size);
    selected_index_ = static_cast<std::size_t>(
        (static_cast<int>(selected_index_) + (direction < 0 ? -1 : 1) + size) % size);
}

void PlayerInventory::switch_current_tray() noexcept
{
    if (profile_ == InventoryProfile::modern)
        current_tray_index_ = (current_tray_index_ + 1U) % tray_count;
}

void PlayerInventory::select_tray(std::size_t index) noexcept
{
    current_tray_index_ =
        profile_ == InventoryProfile::modern ? std::min(index, tray_count - 1U) : 0U;
}

const PlayerInventory::Slot &PlayerInventory::selected() const noexcept
{
    return tray()[selected_index_];
}

InventoryItemId PlayerInventory::selected_item() const noexcept
{
    return selected().has_value() ? selected()->item : InventoryItemId::bare_hands;
}

world::BlockType PlayerInventory::selected_block() const noexcept
{
    return selected().has_value() ? selected()->block : world::BlockType::empty;
}

float item_self_damage_per_use(InventoryItemId item) noexcept
{
    switch (item)
    {
    case InventoryItemId::stone_pickaxe:
    case InventoryItemId::stone_spade:
    case InventoryItemId::stone_axe:
        return 0.0025F;
    case InventoryItemId::copper_pickaxe:
    case InventoryItemId::copper_spade:
    case InventoryItemId::copper_axe:
        return 0.00125F;
    case InventoryItemId::iron_pickaxe:
    case InventoryItemId::iron_spade:
    case InventoryItemId::iron_axe:
        return 0.0005F;
    case InventoryItemId::gold_pickaxe:
    case InventoryItemId::gold_spade:
    case InventoryItemId::gold_axe:
        return 0.00033333333F;
    case InventoryItemId::diamond_pickaxe:
    case InventoryItemId::diamond_spade:
    case InventoryItemId::diamond_axe:
        return 0.00025F;
    case InventoryItemId::bloodstone_pickaxe:
    case InventoryItemId::modern_chainsaw_1:
        return 0.0002F;
    case InventoryItemId::iron_laser_sword:
    case InventoryItemId::copper_laser_sword:
    case InventoryItemId::gold_laser_sword:
    case InventoryItemId::diamond_laser_sword:
    case InventoryItemId::blood_stone_laser_sword:
        return 0.005F;
    case InventoryItemId::knife:
        return 0.02F;
    case InventoryItemId::gold_knife:
        return 0.01F;
    case InventoryItemId::diamond_knife:
        return 0.005F;
    case InventoryItemId::blood_stone_knife:
        return 0.0033333334F;

    case InventoryItemId::assault_rifle:
    case InventoryItemId::pistol:
    case InventoryItemId::pump_shotgun:
    case InventoryItemId::bolt_action_rifle:
    case InventoryItemId::smg:
    case InventoryItemId::lmg:
        return 0.001F;
    case InventoryItemId::gold_assault_rifle:
    case InventoryItemId::gold_pistol:
    case InventoryItemId::gold_pump_shotgun:
    case InventoryItemId::gold_bolt_action_rifle:
    case InventoryItemId::gold_smg:
    case InventoryItemId::gold_lmg:
        return 0.00045454546F;
    case InventoryItemId::diamond_assault_rifle:
    case InventoryItemId::diamond_pistol:
    case InventoryItemId::diamond_pump_shotgun:
    case InventoryItemId::diamond_bolt_action_rifle:
    case InventoryItemId::diamond_smg:
    case InventoryItemId::diamond_lmg:
        return 0.00023923445F;
    case InventoryItemId::blood_stone_assault_rifle:
    case InventoryItemId::blood_stone_pistol:
    case InventoryItemId::blood_stone_pump_shotgun:
    case InventoryItemId::blood_stone_bolt_action_rifle:
    case InventoryItemId::blood_stone_smg:
    case InventoryItemId::blood_stone_lmg:
    case InventoryItemId::modern_laser_drill:
    case InventoryItemId::modern_precision_laser:
        return 0.0001F;
    case InventoryItemId::iron_space_assault_rifle:
    case InventoryItemId::iron_space_pistol:
    case InventoryItemId::iron_space_pump_shotgun:
    case InventoryItemId::iron_space_bolt_action_rifle:
    case InventoryItemId::iron_space_smg:
    case InventoryItemId::copper_space_assault_rifle:
    case InventoryItemId::copper_space_pistol:
    case InventoryItemId::copper_space_pump_shotgun:
    case InventoryItemId::copper_space_bolt_action_rifle:
    case InventoryItemId::copper_space_smg:
    case InventoryItemId::gold_space_assault_rifle:
    case InventoryItemId::gold_space_pistol:
    case InventoryItemId::gold_space_pump_shotgun:
    case InventoryItemId::gold_space_bolt_action_rifle:
    case InventoryItemId::gold_space_smg:
    case InventoryItemId::diamond_space_assault_rifle:
    case InventoryItemId::diamond_space_pistol:
    case InventoryItemId::diamond_space_pump_shotgun:
    case InventoryItemId::diamond_space_bolt_action_rifle:
    case InventoryItemId::diamond_space_smg:
        return 0.0005F;
    case InventoryItemId::rocket_launcher:
    case InventoryItemId::guided_rocket_launcher:
    case InventoryItemId::fired_rocket:
    case InventoryItemId::fired_guided_rocket:
    case InventoryItemId::modern_basic_grenade_launcher:
        return 1.0F;
    case InventoryItemId::gps:
        return 0.1F;
    case InventoryItemId::teleport_gps:
        return 1.0F;
    default:
        return 0.0F;
    }
}

bool inflict_item_damage(InventoryStack &stack, bool infinite_resource_mode) noexcept
{
    const float damage = item_self_damage_per_use(stack.item);
    stack.durability -= damage;
    // Both shipped InventoryItem.InflictDamage bodies apply the same damage a
    // second time in InfiniteResourceMode. Preserve that odd original rule.
    if (infinite_resource_mode)
        stack.durability -= damage;
    return stack.durability <= 0.0F;
}

float guided_lock_seconds(float distance, float angle_radians, float angle_limit_radians) noexcept
{
    const float distance_fraction = std::clamp(distance / 250.0F, 0.0F, 1.0F);
    const float angle_fraction = angle_limit_radians > 0.000001F
                                     ? std::clamp(angle_radians / angle_limit_radians, 0.0F, 1.0F)
                                     : 1.0F;
    return 1.5F + 4.0F * distance_fraction + 4.0F * angle_fraction;
}

float guided_lock_beep_seconds(float required_seconds, float elapsed_seconds) noexcept
{
    return 0.156F + 0.844F * std::clamp((required_seconds - elapsed_seconds) / 9.5F, 0.0F, 1.0F);
}

template <std::size_t Size>
void PlayerInventory::stack_existing(std::array<Slot, Size> &slots, const InventoryStack &source,
                                     std::uint16_t limit, std::uint16_t &remaining) noexcept
{
    if (remaining == 0U)
        return;
    for (auto &slot : slots)
    {
        if (!slot.has_value() || slot->item != source.item || slot->count >= limit)
            continue;
        const auto available = static_cast<std::uint16_t>(limit - slot->count);
        const auto moved = std::min(available, remaining);
        slot->count = static_cast<std::uint16_t>(slot->count + moved);
        remaining = static_cast<std::uint16_t>(remaining - moved);
        if (remaining == 0U)
            return;
    }
}

template <std::size_t Size>
void PlayerInventory::place_into_empty(std::array<Slot, Size> &slots, const InventoryStack &source,
                                       std::uint16_t limit, std::uint16_t &remaining) noexcept
{
    // Entering the empty-slot loop with nothing left used to plant a phantom
    // zero-count stack that blocked the slot and rendered as a ghost item.
    if (remaining == 0U)
        return;
    for (auto &slot : slots)
    {
        if (slot.has_value())
            continue;
        const auto moved = std::min(limit, remaining);
        // with_count keeps per-instance state (a gun's loaded rounds); building a
        // fresh stack here silently refilled a spent clip when items were moved.
        slot = source.with_count(moved);
        remaining = static_cast<std::uint16_t>(remaining - moved);
        if (remaining == 0U)
            return;
    }
}

template <std::size_t Size>
void PlayerInventory::stack_into(std::array<Slot, Size> &slots, const InventoryStack &source,
                                 std::uint16_t limit, std::uint16_t &remaining) noexcept
{
    stack_existing(slots, source, limit, remaining);
    place_into_empty(slots, source, limit, remaining);
}

std::uint16_t PlayerInventory::add_item(InventoryItemId item, std::uint16_t count) noexcept
{
    if (item == InventoryItemId::bare_hands)
        return count;
    const InventoryStack fresh{item, count};
    std::uint16_t remaining = count;
    const std::uint16_t limit = item_stack_limit(item, profile_);
    // AddInventoryItem: top up every existing tray stack, then every existing
    // backpack stack, and only then claim the first empty tray/backpack slot —
    // pickups coalesce instead of splitting into fresh slots.
    const std::size_t tray_limit = profile_ == InventoryProfile::modern ? tray_count : 1U;
    for (std::size_t index{}; index < tray_limit; ++index)
        stack_existing(trays_[index], fresh, limit, remaining);
    stack_existing(storage_, fresh, limit, remaining);
    for (std::size_t index{}; index < tray_limit; ++index)
        place_into_empty(trays_[index], fresh, limit, remaining);
    place_into_empty(storage_, fresh, limit, remaining);
    return remaining;
}

std::uint16_t PlayerInventory::add_stack(const InventoryStack &stack) noexcept
{
    if (!stack.valid())
        return stack.count;
    std::uint16_t remaining = stack.count;
    const std::uint16_t limit = item_stack_limit(stack.item, profile_);
    const std::size_t tray_limit = profile_ == InventoryProfile::modern ? tray_count : 1U;
    for (std::size_t index{}; index < tray_limit; ++index)
        stack_existing(trays_[index], stack, limit, remaining);
    stack_existing(storage_, stack, limit, remaining);
    for (std::size_t index{}; index < tray_limit; ++index)
        place_into_empty(trays_[index], stack, limit, remaining);
    place_into_empty(storage_, stack, limit, remaining);
    return remaining;
}

std::uint16_t PlayerInventory::add(world::BlockType block, std::uint16_t count) noexcept
{
    return add_item(mined_item(block), count);
}

bool PlayerInventory::consume_selected(std::uint16_t count) noexcept
{
    auto &active = tray()[selected_index_];
    if (!active.has_value() || active->count < count)
        return false;
    active->count = static_cast<std::uint16_t>(active->count - count);
    if (active->count == 0U)
        active.reset();
    return true;
}

bool PlayerInventory::consume_item(InventoryItemId item, std::uint16_t amount) noexcept
{
    if (count(item) < amount)
        return false;
    auto consume_from = [&](auto &slots) {
        for (auto &slot : slots)
        {
            if (amount == 0U)
                return;
            if (!slot.has_value() || slot->item != item)
                continue;
            const auto moved = std::min(slot->count, amount);
            slot->count = static_cast<std::uint16_t>(slot->count - moved);
            amount = static_cast<std::uint16_t>(amount - moved);
            if (slot->count == 0U)
                slot.reset();
        }
    };
    const std::size_t tray_limit = profile_ == InventoryProfile::modern ? tray_count : 1U;
    for (std::size_t index{}; index < tray_limit; ++index)
        consume_from(trays_[index]);
    consume_from(storage_);
    return true;
}

std::uint32_t PlayerInventory::count(InventoryItemId item) const noexcept
{
    std::uint32_t result{};
    const auto add_matches = [&](const auto &slots) {
        for (const auto &slot : slots)
            if (slot.has_value() && slot->item == item)
                result += slot->count;
    };
    const std::size_t tray_limit = profile_ == InventoryProfile::modern ? tray_count : 1U;
    for (std::size_t index{}; index < tray_limit; ++index)
        add_matches(trays_[index]);
    add_matches(storage_);
    return result;
}

std::uint32_t PlayerInventory::count(world::BlockType block) const noexcept
{
    return count(block_item(block));
}

PlayerInventory::Slot &PlayerInventory::slot(std::size_t linear_index) noexcept
{
    return linear_index < storage_size ? storage_[linear_index]
                                       : tray()[linear_index - storage_size];
}

const PlayerInventory::Slot &PlayerInventory::slot(std::size_t linear_index) const noexcept
{
    return linear_index < storage_size ? storage_[linear_index]
                                       : tray()[linear_index - storage_size];
}

bool PlayerInventory::move_to_other_section(std::size_t linear_index) noexcept
{
    Slot &source = slot(linear_index);
    if (!source.has_value())
        return false;
    const InventoryStack moving = *source;
    std::uint16_t remaining = moving.count;
    const std::uint16_t limit = item_stack_limit(moving.item, profile_);
    if (linear_index < storage_size)
        stack_into(tray(), moving, limit, remaining);
    else
        stack_into(storage_, moving, limit, remaining);
    if (remaining == moving.count)
        return false;
    if (remaining == 0U)
        source.reset();
    else
        source->count = remaining;
    return true;
}

bool PlayerInventory::can_craft(const CraftingRecipe &recipe) const noexcept
{
    if (recipe.result == InventoryItemId::bare_hands)
        return false;
    for (const CraftingIngredient &ingredient : recipe.ingredients)
    {
        if (ingredient.count == 0U)
            continue;
        if (count(ingredient.item) < ingredient.count)
            return false;
    }
    return true;
}

std::uint16_t PlayerInventory::craft(const CraftingRecipe &recipe) noexcept
{
    if (!can_craft(recipe))
        return 0U;
    // The original removes from the backpack first and the tray second.
    for (const CraftingIngredient &ingredient : recipe.ingredients)
    {
        std::uint16_t needed = ingredient.count;
        const auto take_from = [&](auto &slots) {
            for (auto &slot : slots)
            {
                if (needed == 0U)
                    return;
                if (!slot.has_value() || slot->item != ingredient.item)
                    continue;
                const auto moved = std::min(slot->count, needed);
                slot->count = static_cast<std::uint16_t>(slot->count - moved);
                needed = static_cast<std::uint16_t>(needed - moved);
                if (slot->count == 0U)
                    slot.reset();
            }
        };
        take_from(storage_);
        const std::size_t tray_limit = profile_ == InventoryProfile::modern ? tray_count : 1U;
        for (std::size_t index{}; index < tray_limit; ++index)
            take_from(trays_[index]);
    }
    return add_item(recipe.result, recipe.result_count);
}

} // namespace openminerz
