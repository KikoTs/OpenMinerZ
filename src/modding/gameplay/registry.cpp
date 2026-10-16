// Definitions (blocks, items, recipes, loot, creatures, stats, effects), the
// session registry snapshot, and the per-player stats and status effects.

#include "internal.hpp"

#include "openminerz/inventory.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/custom_terrain.hpp" // mod-world
#include "openminerz/world/enemy.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr std::size_t maximum_definitions = 4096U;

[[nodiscard]] bool valid_short_name(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 64U)
        return false;
    return std::ranges::all_of(name, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_' || character == '.' || character == '-';
    });
}

[[nodiscard]] std::uint16_t clamp_u16(double value, double minimum, double maximum)
{
    return static_cast<std::uint16_t>(std::clamp(value, minimum, maximum));
}

void collect_callbacks(const Value &value, std::vector<Callback> &out, int depth)
{
    if (depth > 16)
        return;
    switch (value.type())
    {
    case ValueType::callback:
        out.push_back(value.as_callback());
        break;
    case ValueType::array:
        for (const Value &item : value.as_array())
            collect_callbacks(item, out, depth + 1);
        break;
    case ValueType::map:
        for (const auto &[key, item] : value.as_map())
            collect_callbacks(item, out, depth + 1);
        break;
    default:
        break;
    }
}

[[nodiscard]] Brain brain_from(std::string_view name)
{
    if (name == "passive" || name == "animal")
        return Brain::passive;
    if (name == "neutral")
        return Brain::neutral;
    if (name == "hostile" || name == "monster")
        return Brain::hostile;
    if (name == "npc")
        return Brain::npc;
    throw ScriptError{std::format("unknown behaviour '{}' (passive, neutral, hostile, npc)", name)};
}

[[nodiscard]] int biome_from(std::string_view name)
{
    static constexpr std::array<std::string_view, 6> names{"classic", "lagoon", "desert",
                                                           "mountain", "arctic", "descent"};
    for (std::size_t index{}; index < names.size(); ++index)
        if (names[index] == name)
            return static_cast<int>(index);
    // mod-world: a mod biome ("mymod:redwood"), whether or not a world
    // generator of this session defines it.
    if (const auto colon = name.find(':'); colon != std::string_view::npos && colon > 0U &&
                                             colon + 1U < name.size())
        return world::biome_id(name);
    throw ScriptError{std::format(
        "unknown biome '{}' (classic, lagoon, desert, mountain, arctic, descent, or a mod "
        "biome key \"mod:name\")",
        name)};
}

} // namespace

// ---------------------------------------------------------------- lookups

const BlockDef *Registry::block(std::string_view key) const noexcept
{
    const auto found = std::ranges::find(blocks, key, &BlockDef::key);
    return found == blocks.end() ? nullptr : &*found;
}

const BlockDef *Registry::block(std::uint16_t id) const noexcept
{
    return id == 0U || id > blocks.size() ? nullptr : &blocks[id - 1U];
}

const ItemDef *Registry::item(std::string_view key) const noexcept
{
    const auto found = std::ranges::find(items, key, &ItemDef::key);
    return found == items.end() ? nullptr : &*found;
}

const ItemDef *Registry::item(std::uint32_t id) const noexcept
{
    return id == 0U || id > items.size() ? nullptr : &items[id - 1U];
}

const CreatureDef *Registry::creature(std::string_view key) const noexcept
{
    const auto found = std::ranges::find(creatures, key, &CreatureDef::key);
    return found == creatures.end() ? nullptr : &*found;
}

const CreatureDef *Registry::creature(std::uint32_t id) const noexcept
{
    return id == 0U || id > creatures.size() ? nullptr : &creatures[id - 1U];
}

// mod-world: what a world generator's palette needs of a mod block.
std::optional<std::uint8_t> GameplayService::block_fallback(Side side, std::string_view key) const
{
    const BlockDef *block = impl_->registry(side).block(key);
    return block != nullptr ? std::optional<std::uint8_t>{block->fallback} : std::nullopt;
}

std::optional<ClientBlockType> GameplayService::client_defined_block(std::string_view key) const
{
    const BlockDef *block = impl_->registry(Side::client).block(key);
    if (block == nullptr)
        return std::nullopt;
    ClientBlockType type;
    type.key = block->key;
    type.name = block->name;
    type.fallback = block->fallback;
    type.texture = block->texture;
    type.faces = block->faces;
    type.model = block->model;
    type.light = block->light;
    type.liquid = block->liquid; // (mod-liquid)
    return type;
}

// ------------------------------------------------------------ resolution

std::string GameplayService::Impl::qualify(std::string_view mod, std::string_view name) const
{
    const auto colon = name.find(':');
    if (colon != std::string_view::npos)
    {
        if (name.substr(0, colon) != mod || !valid_short_name(name.substr(colon + 1U)))
            throw ScriptError{std::format(
                "'{}': a mod defines names in its own namespace ('{}:name')", name, mod)};
        return std::string{name};
    }
    if (!valid_short_name(name))
        throw ScriptError{std::format(
            "'{}' is not a valid name (lower-case letters, digits, '_', '.', '-')", name)};
    return std::format("{}:{}", mod, name);
}

void GameplayService::Impl::release_unused(const Value &definition, std::span<const Callback> kept)
{
    std::vector<Callback> found;
    collect_callbacks(definition, found, 0);
    for (const Callback &callback : found)
        if (std::ranges::find(kept, callback) == kept.end())
            host.release(callback);
}

bool GameplayService::Impl::retail_block_valid(std::uint8_t block) const
{
    // What a stock peer of this generation can hold (apply_authoritative_chunk).
    return block <= (backend.modern_rules() ? 93U : 46U);
}

bool GameplayService::Impl::retail_item_valid(std::uint16_t item) const
{
    return item != 0U &&
           item_registered(static_cast<InventoryItemId>(item),
                           backend.modern_rules() ? InventoryProfile::modern
                                                  : InventoryProfile::classic);
}

ItemRef GameplayService::Impl::resolve_item(const Value &value, std::string_view mod,
                                            std::string_view what, Side side) const
{
    const Registry &definitions = side == Side::server ? server_registry : client_registry;
    if (value.is_number())
    {
        const std::int64_t id = value.as_integer();
        if (id < 1 || id > 65535 || !retail_item_valid(static_cast<std::uint16_t>(id)))
            throw ScriptError{std::format("{}: {} is not an item of this game", what, id)};
        return {static_cast<std::uint16_t>(id), 0U};
    }
    if (value.type() != ValueType::string)
        throw ScriptError{std::format("{}: expected an item name", what)};
    const std::string &name = value.as_string();
    if (name.find(':') != std::string::npos)
    {
        if (const ItemDef *item = definitions.item(name))
            return {item->fallback, item->id};
        throw ScriptError{std::format("{}: no mod item '{}'", what, name)};
    }
    if (!mod.empty())
        if (const ItemDef *item = definitions.item(std::format("{}:{}", mod, name)))
            return {item->fallback, item->id};
    if (const auto retail = retail_item_from_name(name);
        retail.has_value() && retail_item_valid(*retail))
        return {*retail, 0U};
    throw ScriptError{std::format("{}: no item '{}'", what, name)};
}

std::string GameplayService::Impl::item_label(ItemRef item) const
{
    if (const ItemDef *definition = server_registry.item(item.mod_item))
        return definition->key;
    return std::string{retail_item_name(item.item)};
}

std::string GameplayService::Impl::item_key(ItemRef item) const
{
    return item_label(item);
}

GameplayService::Impl::BlockSpec GameplayService::Impl::resolve_block(const Value &value,
                                                                      std::string_view mod,
                                                                      std::string_view what) const
{
    if (value.is_nil())
        return {0U, 0U};
    if (value.is_number())
    {
        const std::int64_t id = value.as_integer();
        if (id < 0 || id > 255 || !retail_block_valid(static_cast<std::uint8_t>(id)))
            throw ScriptError{std::format("{}: {} is not a block of this game", what, id)};
        return {static_cast<std::uint8_t>(id), 0U};
    }
    if (value.type() != ValueType::string)
        throw ScriptError{std::format("{}: expected a block name", what)};
    const std::string &name = value.as_string();
    if (name.find(':') != std::string::npos)
    {
        if (const BlockDef *block = server_registry.block(name))
            return {block->fallback, block->id};
        throw ScriptError{std::format("{}: no mod block '{}'", what, name)};
    }
    if (!mod.empty())
        if (const BlockDef *block = server_registry.block(std::format("{}:{}", mod, name)))
            return {block->fallback, block->id};
    if (const auto retail = retail_block_from_name(name);
        retail.has_value() && retail_block_valid(*retail))
        return {*retail, 0U};
    throw ScriptError{std::format("{}: no block '{}'", what, name)};
}

std::string GameplayService::Impl::block_label(std::uint8_t block, std::uint16_t mod_block) const
{
    if (const BlockDef *definition = server_registry.block(mod_block))
        return definition->key;
    const std::string_view name = retail_block_name(block);
    return name == "empty" ? std::string{"air"} : std::string{name};
}

// ------------------------------------------------------------ definitions

void GameplayService::Impl::define_block(Side side, std::string_view mod, std::string_view name,
                                         const Value &def)
{
    Registry &definitions = registry(side);
    const std::string key = qualify(mod, name);
    BlockDef block;
    block.key = key;
    block.mod = std::string{mod};
    block.name = string_or(def, "name", name);
    const Value *fallback = def.field("fallback");
    if (fallback == nullptr || fallback->is_nil())
        throw ScriptError{std::format("block '{}' needs a retail fallback block", key)};
    const auto retail = fallback->is_number()
                            ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(
                                  std::clamp<std::int64_t>(fallback->as_integer(), 0, 255))}
                            : retail_block_from_name(fallback->as_string());
    if (!retail.has_value() || *retail == 0U || !retail_block_valid(*retail))
        throw ScriptError{std::format("block '{}': its fallback must be a solid retail block of "
                                      "this game",
                                      key)};
    block.fallback = *retail;
    block.texture = string_or(def, "texture", "");
    if (const Value *faces = def.field("faces"); faces != nullptr && faces->type() == ValueType::map)
    {
        block.faces[0] = string_or(*faces, "top", "");
        block.faces[1] = string_or(*faces, "side", "");
        block.faces[2] = string_or(*faces, "bottom", "");
    }
    block.model = string_or(def, "model", "");
    block.light = static_cast<std::uint8_t>(std::clamp(number_or(def, "light", 0.0), 0.0, 15.0));
    block.drops = string_or(def, "drops", "");
    block.drop_count = clamp_u16(number_or(def, "drop_count", 1.0), 1.0, 999.0);
    // (mod-liquid) A liquid's fallback must let light through: the cells
    // around it are lit, meshed and seen through it by that block.
    block.liquid = bool_or(def, "liquid", false);
    if (block.liquid && world::occludes(static_cast<world::BlockType>(block.fallback)))
        throw ScriptError{std::format("block '{}': a liquid's fallback must be a block light passes "
                                      "through (ice, glass_basic, leaves)",
                                      key)};
    release_unused(def, {});
    const auto existing = std::ranges::find(definitions.blocks, key, &BlockDef::key);
    if (existing != definitions.blocks.end())
    {
        block.id = existing->id;
        *existing = std::move(block);
    }
    else
    {
        if (definitions.blocks.size() >= maximum_definitions)
            throw ScriptError{"too many mod blocks"};
        block.id = static_cast<std::uint16_t>(definitions.blocks.size() + 1U);
        definitions.blocks.push_back(std::move(block));
    }
    if (side == Side::server)
        refresh_liquids(); // (mod-liquid)
}

// (mod-liquid) The retail blocks the server's liquids fall back to: the fast
// first test of liquid_cell.
void GameplayService::Impl::refresh_liquids()
{
    liquid_fallbacks.fill(false);
    liquids_defined = false;
    for (const BlockDef &block : server_registry.blocks)
        if (block.liquid)
        {
            liquid_fallbacks[block.fallback] = true;
            liquids_defined = true;
        }
}

void GameplayService::Impl::define_item(Side side, std::string_view mod, std::string_view name,
                                        const Value &def)
{
    Registry &definitions = registry(side);
    const std::string key = qualify(mod, name);
    ItemDef item;
    item.key = key;
    item.mod = std::string{mod};
    item.name = string_or(def, "name", name);
    item.description = string_or(def, "description", "");
    const Value *fallback = def.field("fallback");
    if (fallback == nullptr || fallback->is_nil())
        throw ScriptError{std::format("item '{}' needs a retail fallback item", key)};
    const auto retail = fallback->is_number()
                            ? std::optional<std::uint16_t>{static_cast<std::uint16_t>(
                                  std::clamp<std::int64_t>(fallback->as_integer(), 0, 65535))}
                            : retail_item_from_name(fallback->as_string());
    if (!retail.has_value() || !retail_item_valid(*retail))
        throw ScriptError{std::format("item '{}': its fallback must be an item this game has", key)};
    item.fallback = *retail;
    const std::uint16_t limit = item_stack_limit(
        static_cast<InventoryItemId>(item.fallback),
        backend.modern_rules() ? InventoryProfile::modern : InventoryProfile::classic);
    // The stack rule is the fallback's: stock code merges and splits by it.
    item.stack_limit = clamp_u16(number_or(def, "stack", limit), 1.0, limit);
    item.icon = string_or(def, "icon", "");
    item.model = string_or(def, "model", "");
    item.cooldown = static_cast<float>(std::clamp(number_or(def, "cooldown", 0.25), 0.05, 60.0));
    define_item_presentation(item, def); // (Items) hold, scale, weapon
    if (const Value *places = def.field("places"); places != nullptr && places->type() == ValueType::string)
    {
        const std::string &block_name = places->as_string();
        const BlockDef *placed = definitions.block(
            block_name.find(':') != std::string::npos ? block_name : std::format("{}:{}", mod, block_name));
        if (placed == nullptr)
            throw ScriptError{std::format("item '{}': 'places' names no mod block '{}' (define the "
                                          "block first)",
                                          key, block_name)};
        if (!is_block_item(static_cast<InventoryItemId>(item.fallback)) ||
            item_block(static_cast<InventoryItemId>(item.fallback)) != static_cast<world::BlockType>(placed->fallback))
            throw ScriptError{std::format("item '{}': to place '{}' its fallback must be the block "
                                          "item of that block's fallback",
                                          key, placed->key)};
        item.places_block = placed->id;
    }
    std::vector<Callback> kept;
    if (side == Side::server)
    {
        item.on_use = callback_or_none(def, "use");
        if (item.on_use.valid())
        {
            kept.push_back(item.on_use);
            hold(side, item.on_use);
        }
    }
    release_unused(def, kept);
    const auto existing = std::ranges::find(definitions.items, key, &ItemDef::key);
    if (existing != definitions.items.end())
    {
        item.id = existing->id;
        *existing = std::move(item);
        return;
    }
    if (definitions.items.size() >= maximum_definitions)
        throw ScriptError{"too many mod items"};
    item.id = static_cast<std::uint32_t>(definitions.items.size() + 1U);
    definitions.items.push_back(std::move(item));
}

std::uint32_t GameplayService::Impl::add_recipe(Side side, std::string_view mod, const Value &def)
{
    Registry &definitions = registry(side);
    RecipeDef recipe;
    recipe.mod = std::string{mod};
    recipe.category = string_or(def, "category", "Mods");
    const Value *result = def.field("result");
    if (result == nullptr)
        throw ScriptError{"a recipe needs a result"};
    recipe.result = resolve_item(*result, mod, "result", side);
    recipe.count = clamp_u16(number_or(def, "count", 1.0), 1.0, 999.0);
    const Value *ingredients = def.field("ingredients");
    if (ingredients == nullptr)
        throw ScriptError{"a recipe needs ingredients"};
    const auto add = [&](const Value &item, double amount) {
        if (recipe.ingredients.size() >= 8U)
            throw ScriptError{"a recipe takes at most 8 ingredients"};
        recipe.ingredients.emplace_back(resolve_item(item, mod, "ingredient", side),
                                        clamp_u16(amount, 1.0, 999.0));
    };
    if (ingredients->type() == ValueType::map)
        for (const auto &[item, amount] : ingredients->as_map())
            add(Value{item}, amount.as_number());
    else
        for (const Value &entry : ingredients->as_array())
        {
            if (entry.type() == ValueType::array && entry.as_array().size() == 2U)
                add(entry.as_array()[0], entry.as_array()[1].as_number());
            else if (entry.type() == ValueType::map)
                add(entry.field_or("item", Value{}), number_or(entry, "count", 1.0));
            else
                throw ScriptError{"an ingredient is {item, count} or {item = ..., count = ...}"};
        }
    if (recipe.ingredients.empty())
        throw ScriptError{"a recipe needs at least one ingredient"};
    release_unused(def, {});
    if (definitions.recipes.size() >= maximum_definitions)
        throw ScriptError{"too many mod recipes"};
    recipe.id = side == Side::server ? next_recipe_id++
                                     : static_cast<std::uint32_t>(definitions.recipes.size() + 1U);
    definitions.recipes.push_back(std::move(recipe));
    return definitions.recipes.back().id;
}

bool GameplayService::Impl::remove_recipe(Side side, std::string_view mod, const Value &what)
{
    Registry &definitions = registry(side);
    if (what.is_number())
    {
        const std::int64_t id = what.as_integer();
        return std::erase_if(definitions.recipes, [&](const RecipeDef &recipe) {
                   return static_cast<std::int64_t>(recipe.id) == id;
               }) != 0U;
    }
    // A retail recipe, by what it makes: switched off for this session.
    const ItemRef result = resolve_item(what, mod, "recipe result", side);
    if (result.mod_item != 0U)
        return std::erase_if(definitions.recipes, [&](const RecipeDef &recipe) {
                   return recipe.result == result;
               }) != 0U;
    const auto &book = crafting_recipes(backend.modern_rules() ? InventoryProfile::modern
                                                               : InventoryProfile::classic);
    bool removed{};
    for (std::size_t index{}; index < book.size(); ++index)
        if (static_cast<std::uint16_t>(book[index].result) == result.item)
            removed = definitions.disabled_retail_recipes.insert(index).second || removed;
    return removed;
}

void GameplayService::Impl::define_loot(Side side, std::string_view mod, std::string_view name,
                                        const Value &def)
{
    Registry &definitions = registry(side);
    LootTable table;
    table.key = qualify(mod, name);
    if (const Value *rolls = def.field("rolls"); rolls != nullptr && !rolls->is_nil())
    {
        if (rolls->is_number())
            table.rolls_min = table.rolls_max = static_cast<int>(std::clamp(rolls->as_number(), 0.0, 64.0));
        else
        {
            const Array &range = rolls->as_array();
            if (range.size() != 2U)
                throw ScriptError{"rolls is a number or {min, max}"};
            table.rolls_min = static_cast<int>(std::clamp(range[0].as_number(), 0.0, 64.0));
            table.rolls_max = static_cast<int>(std::clamp(range[1].as_number(), 0.0, 64.0));
            if (table.rolls_max < table.rolls_min)
                std::swap(table.rolls_min, table.rolls_max);
        }
    }
    const Value *entries = def.field("entries");
    if (entries == nullptr || entries->type() != ValueType::array)
        throw ScriptError{"a loot table needs entries = { {item = ..., weight = ...}, ... }"};
    for (const Value &entry : entries->as_array())
    {
        if (table.entries.size() >= 256U)
            throw ScriptError{"a loot table holds at most 256 entries"};
        LootEntry loot;
        loot.item = resolve_item(entry.field_or("item", Value{}), mod, "loot item", side);
        loot.weight = static_cast<int>(std::clamp(number_or(entry, "weight", 1.0), 0.0, 1'000'000.0));
        loot.minimum = static_cast<int>(std::clamp(number_or(entry, "min", 1.0), 1.0, 999.0));
        loot.maximum = static_cast<int>(std::clamp(number_or(entry, "max", loot.minimum), 1.0, 999.0));
        if (loot.maximum < loot.minimum)
            std::swap(loot.minimum, loot.maximum);
        table.entries.push_back(loot);
    }
    release_unused(def, {});
    definitions.loot.insert_or_assign(table.key, std::move(table));
}

void GameplayService::Impl::add_enemy_loot(Side side, std::string_view mod, const Value &enemy,
                                           const Value &table)
{
    Registry &definitions = registry(side);
    std::string key = table.as_string();
    if (key.find(':') == std::string::npos)
        key = std::format("{}:{}", mod, key);
    if (!definitions.loot.contains(key))
        throw ScriptError{std::format("no loot table '{}'", key)};
    std::vector<int> types;
    if (enemy.is_number())
    {
        const std::int64_t type = enemy.as_integer();
        if (type < 0 || type >= world::enemy_type_count)
            throw ScriptError{"no such enemy type"};
        types.push_back(static_cast<int>(type));
    }
    else
    {
        const std::string &name = enemy.as_string();
        for (int type = 0; type < world::enemy_type_count; ++type)
        {
            const world::EnemyClass enemy_class = world::enemy_stats(type).enemy_class;
            const bool matches =
                name == "any" || (name == "zombie" && enemy_class == world::EnemyClass::zombie) ||
                (name == "archer" && enemy_class == world::EnemyClass::archer) ||
                (name == "skeleton" && (enemy_class == world::EnemyClass::skeleton ||
                                        enemy_class == world::EnemyClass::skeleton_sword ||
                                        enemy_class == world::EnemyClass::skeleton_axes)) ||
                (name == "felguard" && enemy_class == world::EnemyClass::felguard) ||
                (name == "alien" && enemy_class == world::EnemyClass::alien);
            if (matches)
                types.push_back(type);
        }
        if (types.empty())
            throw ScriptError{std::format(
                "unknown enemy '{}' (zombie, archer, skeleton, felguard, alien, any, or a type id)",
                name)};
    }
    for (const int type : types)
        definitions.enemy_loot.emplace(type, key);
}

void GameplayService::Impl::define_creature(Side side, std::string_view mod, std::string_view name,
                                            const Value &def)
{
    Registry &definitions = registry(side);
    CreatureDef creature;
    creature.key = qualify(mod, name);
    creature.mod = std::string{mod};
    creature.name = string_or(def, "name", name);
    creature.model = string_or(def, "model", "");
    creature.texture = string_or(def, "texture", "");
    creature.health = static_cast<float>(std::clamp(number_or(def, "health", 4.0), 0.01, 100'000.0));
    creature.speed = static_cast<float>(std::clamp(number_or(def, "speed", 2.0), 0.0, 20.0));
    creature.run_speed = static_cast<float>(std::clamp(number_or(def, "run_speed", creature.speed * 2.0), 0.0, 30.0));
    creature.damage = static_cast<float>(std::clamp(number_or(def, "damage", 10.0), 0.0, 1000.0));
    creature.attack_range = static_cast<float>(std::clamp(number_or(def, "attack_range", 1.6), 0.5, 32.0));
    creature.attack_cooldown = static_cast<float>(std::clamp(number_or(def, "attack_cooldown", 1.0), 0.1, 60.0));
    creature.view_range = static_cast<float>(std::clamp(number_or(def, "view_range", 16.0), 1.0, 64.0));
    if (const Value *size = def.field("size"); size != nullptr && size->type() == ValueType::array &&
                                               size->as_array().size() == 2U)
    {
        creature.width = static_cast<float>(std::clamp(size->as_array()[0].as_number(), 0.2, 6.0));
        creature.height = static_cast<float>(std::clamp(size->as_array()[1].as_number(), 0.2, 8.0));
    }
    creature.brain = brain_from(string_or(def, "behaviour", "passive"));
    creature.drops = string_or(def, "drops", "");
    creature.interactable = bool_or(def, "interact", creature.brain == Brain::npc);
    creature.dialogue = string_or(def, "dialogue", "");
    creature.show_name = bool_or(def, "show_name", creature.brain == Brain::npc);
    if (const Value *spawn = def.field("spawn"); spawn != nullptr && spawn->type() == ValueType::map)
    {
        SpawnRule &rule = creature.spawn;
        rule.enabled = true;
        if (const Value *biomes = spawn->field("biomes"); biomes != nullptr && biomes->type() == ValueType::array)
            for (const Value &biome : biomes->as_array())
                rule.biomes.push_back(biome_from(biome.as_string()));
        rule.light_min = static_cast<float>(std::clamp(number_or(*spawn, "light_min", 0.0), 0.0, 1.0));
        rule.light_max = static_cast<float>(std::clamp(number_or(*spawn, "light_max", 1.0), 0.0, 1.0));
        const std::string time = string_or(*spawn, "time", "any");
        rule.time = time == "day" ? SpawnTime::day : time == "night" ? SpawnTime::night : SpawnTime::any;
        rule.surface = bool_or(*spawn, "surface", true);
        rule.chance = static_cast<float>(std::clamp(number_or(*spawn, "chance", 0.05), 0.0, 1.0));
        rule.max_near_player = static_cast<int>(std::clamp(number_or(*spawn, "max", 4.0), 0.0, 64.0));
        rule.group_min = static_cast<int>(std::clamp(number_or(*spawn, "group_min", 1.0), 1.0, 16.0));
        rule.group_max = static_cast<int>(std::clamp(number_or(*spawn, "group_max", rule.group_min), 1.0, 16.0));
        rule.y_min = static_cast<int>(std::clamp(number_or(*spawn, "y_min", -64.0), -64.0, 63.0));
        rule.y_max = static_cast<int>(std::clamp(number_or(*spawn, "y_max", 63.0), -64.0, 63.0));
    }
    // mod-ai: brain, senses, team, attacks, movement, boss (ai.cpp); the
    // ability callbacks it keeps.
    std::vector<Callback> kept;
    define_creature_ai(side, mod, creature, def, kept);
    release_unused(def, kept);
    const auto existing = std::ranges::find(definitions.creatures, creature.key, &CreatureDef::key);
    if (existing != definitions.creatures.end())
    {
        creature.id = existing->id;
        *existing = std::move(creature);
        return;
    }
    if (definitions.creatures.size() >= maximum_definitions)
        throw ScriptError{"too many creature types"};
    creature.id = static_cast<std::uint32_t>(definitions.creatures.size() + 1U);
    definitions.creatures.push_back(std::move(creature));
}

void GameplayService::Impl::define_stat(Side side, std::string_view mod, std::string_view name,
                                        const Value &def)
{
    Registry &definitions = registry(side);
    if (!valid_short_name(name))
        throw ScriptError{std::format("'{}' is not a valid stat name", name)};
    const auto existing = definitions.stats.find(name);
    if (existing != definitions.stats.end() && existing->second.mod != mod)
        throw ScriptError{std::format("stat '{}' is already defined by '{}'", name, existing->second.mod)};
    StatDef stat;
    stat.name = std::string{name};
    stat.mod = std::string{mod};
    stat.minimum = number_or(def, "min", 0.0);
    stat.maximum = number_or(def, "max", 100.0);
    if (!(stat.maximum > stat.minimum))
        throw ScriptError{"a stat's max must be above its min"};
    stat.initial = std::clamp(number_or(def, "initial", stat.maximum), stat.minimum, stat.maximum);
    stat.decay = number_or(def, "decay", 0.0);
    stat.is_public = bool_or(def, "public", false);
    stat.persist = bool_or(def, "persist", true);
    if (const Value *hud = def.field("hud"); hud != nullptr && !hud->is_nil())
    {
        stat.hud = hud->type() == ValueType::boolean ? hud->as_bool() : true;
        if (hud->type() == ValueType::map)
        {
            stat.label = string_or(*hud, "label", name);
            stat.color = string_or(*hud, "color", "");
            stat.icon = string_or(*hud, "icon", "");
        }
    }
    if (stat.label.empty())
        stat.label = std::string{name};
    release_unused(def, {});
    definitions.stats.insert_or_assign(stat.name, std::move(stat));
    // Players already in the session get the new stat at its initial value.
    if (side == Side::server)
        for (PlayerRecord &player : players)
            if (!player.stats.contains(name))
                player.stats[std::string{name}] = definitions.stats[std::string{name}].initial;
}

void GameplayService::Impl::define_effect(Side side, std::string_view mod, std::string_view name,
                                          const Value &def)
{
    Registry &definitions = registry(side);
    if (!valid_short_name(name))
        throw ScriptError{std::format("'{}' is not a valid effect name", name)};
    static constexpr std::array<std::string_view, 4> builtin{"speed", "regen", "damage", "stat"};
    if (std::ranges::find(builtin, name) != builtin.end())
        throw ScriptError{std::format("'{}' is a built-in effect", name)};
    EffectDef effect;
    effect.name = std::string{name};
    effect.mod = std::string{mod};
    effect.interval = static_cast<float>(std::clamp(number_or(def, "interval", 1.0), 0.05, 3600.0));
    effect.icon = string_or(def, "icon", "");
    std::vector<Callback> kept;
    if (side == Side::server)
    {
        effect.on_start = callback_or_none(def, "start");
        effect.on_tick = callback_or_none(def, "tick");
        effect.on_end = callback_or_none(def, "stop");
        for (const Callback &callback : {effect.on_start, effect.on_tick, effect.on_end})
            if (callback.valid())
            {
                kept.push_back(callback);
                hold(side, callback);
            }
    }
    release_unused(def, kept);
    definitions.effects.insert_or_assign(effect.name, std::move(effect));
}

std::vector<std::pair<ItemRef, std::uint16_t>> GameplayService::Impl::roll_loot(std::string_view key)
{
    std::vector<std::pair<ItemRef, std::uint16_t>> stacks;
    const auto table = server_registry.loot.find(key);
    if (table == server_registry.loot.end())
        return stacks;
    int total{};
    for (const LootEntry &entry : table->second.entries)
        total += entry.weight;
    if (total <= 0)
        return stacks;
    const int rolls = random_int(table->second.rolls_min, table->second.rolls_max);
    for (int roll = 0; roll < rolls; ++roll)
    {
        int pick = random_int(0, total - 1);
        for (const LootEntry &entry : table->second.entries)
        {
            pick -= entry.weight;
            if (pick >= 0)
                continue;
            const auto amount = static_cast<std::uint16_t>(random_int(entry.minimum, entry.maximum));
            const auto merged = std::ranges::find(stacks, entry.item, &std::pair<ItemRef, std::uint16_t>::first);
            if (merged != stacks.end())
                merged->second = static_cast<std::uint16_t>(std::min(999, merged->second + amount));
            else
                stacks.emplace_back(entry.item, amount);
            break;
        }
    }
    return stacks;
}

Value GameplayService::Impl::stacks_value(
    const std::vector<std::pair<ItemRef, std::uint16_t>> &stacks) const
{
    Array list;
    for (const auto &[item, amount] : stacks)
        list.push_back(make_map({{"item", item_label(item)}, {"count", static_cast<std::int64_t>(amount)}}));
    return Value{std::move(list)};
}

// ------------------------------------------------------------- snapshot

Value GameplayService::Impl::registry_snapshot() const
{
    const Registry &definitions = server_registry;
    Array blocks;
    for (const BlockDef &block : definitions.blocks)
        blocks.push_back(make_map({{"id", static_cast<std::int64_t>(block.id)},
                                   {"key", block.key},
                                   {"name", block.name},
                                   {"fallback", static_cast<std::int64_t>(block.fallback)},
                                   {"texture", block.texture},
                                   {"top", block.faces[0]},
                                   {"side", block.faces[1]},
                                   {"bottom", block.faces[2]},
                                   {"model", block.model},
                                   {"light", static_cast<std::int64_t>(block.light)},
                                   {"liquid", block.liquid}})); // (mod-liquid)
    Array items;
    for (const ItemDef &item : definitions.items)
        items.push_back(make_map({{"id", static_cast<std::int64_t>(item.id)},
                                  {"key", item.key},
                                  {"name", item.name},
                                  {"description", item.description},
                                  {"fallback", static_cast<std::int64_t>(item.fallback)},
                                  {"icon", item.icon},
                                  {"model", item.model},
                                  {"stack", static_cast<std::int64_t>(item.stack_limit)},
                                  {"usable", item.on_use.valid()},
                                  {"hand", item_hand_value(item)}, // (Items)
                                  {"places", item.places_block != 0U && definitions.block(item.places_block) != nullptr
                                                 ? definitions.block(item.places_block)->key
                                                 : std::string{}}}));
    Array recipes;
    for (const RecipeDef &recipe : definitions.recipes)
    {
        Array ingredients;
        for (const auto &[item, amount] : recipe.ingredients)
            ingredients.push_back(make_map({{"item", item_label(item)}, {"count", static_cast<std::int64_t>(amount)}}));
        recipes.push_back(make_map({{"id", static_cast<std::int64_t>(recipe.id)},
                                    {"category", recipe.category},
                                    {"result", item_label(recipe.result)},
                                    {"count", static_cast<std::int64_t>(recipe.count)},
                                    {"ingredients", Value{std::move(ingredients)}}}));
    }
    Array disabled;
    for (const std::size_t index : definitions.disabled_retail_recipes)
        disabled.emplace_back(static_cast<std::int64_t>(index));
    Array stats;
    for (const auto &[name, stat] : definitions.stats)
        stats.push_back(make_map({{"name", name},
                                  {"min", stat.minimum},
                                  {"max", stat.maximum},
                                  {"hud", stat.hud},
                                  {"label", stat.label},
                                  {"color", stat.color},
                                  {"icon", stat.icon}}));
    Array effects;
    for (const auto &[name, effect] : definitions.effects)
        effects.push_back(make_map({{"name", name}, {"icon", effect.icon}}));
    // Creatures as a client may want to load them ahead (ent+ names the
    // model again for each entity).
    Array creatures;
    for (const CreatureDef &creature : definitions.creatures)
        creatures.push_back(make_map({{"id", static_cast<std::int64_t>(creature.id)},
                                      {"key", creature.key},
                                      {"name", creature.name},
                                      {"model", creature.model},
                                      {"texture", creature.texture}}));
    return make_map({{"t", "registry"},
                     {"version", 1},
                     {"blocks", Value{std::move(blocks)}},
                     {"items", Value{std::move(items)}},
                     {"creatures", Value{std::move(creatures)}},
                     {"recipes", Value{std::move(recipes)}},
                     {"disabled", Value{std::move(disabled)}},
                     {"stats", Value{std::move(stats)}},
                     {"effects", Value{std::move(effects)}}});
}

void GameplayService::Impl::apply_registry(const Value &snapshot)
{
    const auto text = [](const Value &entry, std::string_view key) {
        const Value *field = entry.field(key);
        return field != nullptr && field->type() == ValueType::string ? field->as_string().substr(0, 256U)
                                                                      : std::string{};
    };
    const auto number = [](const Value &entry, std::string_view key, double fallback) {
        const Value *field = entry.field(key);
        return field != nullptr && field->is_number() ? field->as_number() : fallback;
    };
    const auto list = [&](std::string_view key) -> const Array * {
        const Value *field = snapshot.field(key);
        return field != nullptr && field->type() == ValueType::array ? &field->as_array() : nullptr;
    };
    std::vector<ClientBlockType> blocks;
    if (const Array *entries = list("blocks"))
        for (const Value &entry : *entries)
        {
            if (blocks.size() >= maximum_definitions)
                break;
            ClientBlockType block;
            block.id = static_cast<std::uint16_t>(blocks.size() + 1U);
            if (static_cast<std::uint16_t>(number(entry, "id", 0.0)) != block.id)
                throw ScriptError{"registry block ids are out of order"};
            block.key = text(entry, "key");
            block.name = text(entry, "name");
            block.fallback = static_cast<std::uint8_t>(std::clamp(number(entry, "fallback", 0.0), 0.0, 255.0));
            block.texture = text(entry, "texture");
            block.faces = {text(entry, "top"), text(entry, "side"), text(entry, "bottom")};
            block.model = text(entry, "model");
            block.light = static_cast<std::uint8_t>(std::clamp(number(entry, "light", 0.0), 0.0, 15.0));
            block.liquid = bool_or(entry, "liquid", false) && // (mod-liquid)
                           !world::occludes(static_cast<world::BlockType>(block.fallback));
            blocks.push_back(std::move(block));
        }
    std::vector<ClientItemType> items;
    if (const Array *entries = list("items"))
        for (const Value &entry : *entries)
        {
            if (items.size() >= maximum_definitions)
                break;
            ClientItemType item;
            item.id = static_cast<std::uint32_t>(items.size() + 1U);
            if (static_cast<std::uint32_t>(number(entry, "id", 0.0)) != item.id)
                throw ScriptError{"registry item ids are out of order"};
            item.key = text(entry, "key");
            item.name = text(entry, "name");
            item.description = text(entry, "description");
            item.fallback = static_cast<std::uint16_t>(std::clamp(number(entry, "fallback", 0.0), 0.0, 65535.0));
            item.icon = text(entry, "icon");
            item.model = text(entry, "model");
            item.stack_limit = static_cast<std::uint16_t>(std::clamp(number(entry, "stack", 1.0), 1.0, 999.0));
            item.usable = bool_or(entry, "usable", false);
            item.places = text(entry, "places");
            read_item_hand(item, entry.field("hand")); // (Items)
            items.push_back(std::move(item));
        }
    std::vector<ClientRecipe> recipes;
    if (const Array *entries = list("recipes"))
        for (const Value &entry : *entries)
        {
            if (recipes.size() >= maximum_definitions)
                break;
            ClientRecipe recipe;
            recipe.id = static_cast<std::uint32_t>(std::clamp(number(entry, "id", 0.0), 0.0, 4.0e9));
            recipe.category = text(entry, "category");
            recipe.result = text(entry, "result");
            recipe.count = static_cast<std::uint16_t>(std::clamp(number(entry, "count", 1.0), 1.0, 999.0));
            if (const Value *parts = entry.field("ingredients"); parts != nullptr && parts->type() == ValueType::array)
                for (const Value &part : parts->as_array())
                    if (recipe.ingredients.size() < 8U)
                        recipe.ingredients.emplace_back(
                            text(part, "item"),
                            static_cast<std::uint16_t>(std::clamp(number(part, "count", 1.0), 1.0, 999.0)));
            recipes.push_back(std::move(recipe));
        }
    std::set<std::size_t> disabled;
    if (const Array *entries = list("disabled"))
        for (const Value &entry : *entries)
            if (entry.is_number() && disabled.size() < 1024U)
                disabled.insert(static_cast<std::size_t>(std::clamp(entry.as_number(), 0.0, 1.0e6)));
    std::vector<ClientStat> stats;
    if (const Array *entries = list("stats"))
        for (const Value &entry : *entries)
        {
            if (stats.size() >= 64U)
                break;
            ClientStat stat;
            stat.name = text(entry, "name");
            stat.minimum = number(entry, "min", 0.0);
            stat.maximum = number(entry, "max", 100.0);
            stat.hud = bool_or(entry, "hud", false);
            stat.label = text(entry, "label");
            stat.color = text(entry, "color");
            stat.icon = text(entry, "icon");
            const auto previous = std::ranges::find(client_stats, stat.name, &ClientStat::name);
            stat.value = previous != client_stats.end() ? previous->value : stat.maximum;
            stats.push_back(std::move(stat));
        }
    client_blocks = std::move(blocks);
    client_items = std::move(items);
    client_recipes = std::move(recipes);
    client_disabled_recipes = std::move(disabled);
    client_stats = std::move(stats);
    client_block_ids.clear();
    for (const ClientBlockType &block : client_blocks)
        client_block_ids[block.key] = block.id;
    client_item_ids.clear();
    for (const ClientItemType &item : client_items)
        client_item_ids[item.key] = item.id;
}

// ------------------------------------------------------- stats and effects

double GameplayService::Impl::stat(PlayerRecord &player, std::string_view name)
{
    const auto definition = server_registry.stats.find(name);
    if (definition == server_registry.stats.end())
        throw ScriptError{std::format("no stat '{}' (stats.define it first)", name)};
    const auto value = player.stats.find(name);
    if (value != player.stats.end())
        return value->second;
    return player.stats[std::string{name}] = definition->second.initial;
}

double GameplayService::Impl::set_stat(PlayerRecord &player, std::string_view name, double value,
                                       bool notify)
{
    const auto definition = server_registry.stats.find(name);
    if (definition == server_registry.stats.end())
        throw ScriptError{std::format("no stat '{}' (stats.define it first)", name)};
    if (!std::isfinite(value))
        throw ScriptError{"a stat value must be finite"};
    const StatDef &stat_definition = definition->second;
    const double previous = stat(player, name);
    const double next = std::clamp(value, stat_definition.minimum, stat_definition.maximum);
    if (next == previous)
        return next;
    player.stats[std::string{name}] = next;
    if (notify)
        emit(Side::server, "stat.changed",
             make_map({{"player", handle_of(player)}, {"stat", std::string{name}},
                       {"value", next}, {"previous", previous}}));
    if (next <= stat_definition.minimum && previous > stat_definition.minimum)
        emit(Side::server, "stat.depleted",
             make_map({{"player", handle_of(player)}, {"stat", std::string{name}}}));
    else if (next >= stat_definition.maximum && previous < stat_definition.maximum)
        emit(Side::server, "stat.filled",
             make_map({{"player", handle_of(player)}, {"stat", std::string{name}}}));
    return next;
}

void GameplayService::Impl::apply_effect(PlayerRecord &player, std::string_view kind,
                                         const Value &options)
{
    static constexpr std::array<std::string_view, 4> builtin{"speed", "regen", "damage", "stat"};
    const bool is_builtin = std::ranges::find(builtin, kind) != builtin.end();
    const auto custom = server_registry.effects.find(kind);
    if (!is_builtin && custom == server_registry.effects.end())
        throw ScriptError{std::format("no effect '{}' (speed, regen, damage, stat or one defined "
                                      "with effects.define)",
                                      kind)};
    ActiveEffect effect;
    effect.kind = std::string{kind};
    effect.remaining = static_cast<float>(std::clamp(number_or(options, "duration", -1.0), -1.0, 1.0e6));
    effect.strength = static_cast<float>(number_or(options, "strength", kind == "speed" ? 1.5 : 1.0));
    if (kind == "speed")
        effect.strength = std::clamp(effect.strength, 0.0F, 4.0F);
    else
        effect.strength = std::clamp(effect.strength, -1000.0F, 1000.0F);
    if (kind == "stat")
    {
        effect.stat = string_or(options, "stat", "");
        static_cast<void>(stat(player, effect.stat)); // validates the name
    }
    effect.interval_left = custom != server_registry.effects.end() ? custom->second.interval : 1.0F;
    const bool stacked = bool_or(options, "stack", false);
    const auto existing = std::ranges::find_if(player.effects, [&](const ActiveEffect &active) {
        return active.kind == kind && active.stat == effect.stat;
    });
    const bool started = existing == player.effects.end() || stacked;
    if (started)
    {
        if (player.effects.size() >= 64U)
            throw ScriptError{"a player carries at most 64 effects"};
        player.effects.push_back(effect);
    }
    else
    {
        // Re-applying refreshes: the longer duration and the new strength.
        existing->remaining = existing->remaining < 0.0F || effect.remaining < 0.0F
                                  ? -1.0F
                                  : std::max(existing->remaining, effect.remaining);
        existing->strength = effect.strength;
    }
    player.effects_dirty = true;
    if (started)
    {
        if (custom != server_registry.effects.end() && custom->second.on_start.valid())
        {
            const std::array<Value, 2> call{Value{handle_of(player)}, Value{effect.strength}};
            static_cast<void>(host.invoke_as(handle_of(player), custom->second.on_start, call));
        }
        emit(Side::server, "effect.started",
             make_map({{"player", handle_of(player)}, {"effect", effect.kind},
                       {"strength", effect.strength}, {"duration", effect.remaining}}));
    }
}

bool GameplayService::Impl::remove_effect(PlayerRecord &player, std::string_view kind)
{
    bool removed{};
    for (std::size_t index = player.effects.size(); index-- > 0U;)
    {
        if (player.effects[index].kind != kind)
            continue;
        const ActiveEffect effect = player.effects[index];
        player.effects.erase(player.effects.begin() + static_cast<std::ptrdiff_t>(index));
        removed = true;
        const auto custom = server_registry.effects.find(kind);
        if (custom != server_registry.effects.end() && custom->second.on_end.valid())
        {
            const std::array<Value, 2> call{Value{handle_of(player)}, Value{effect.strength}};
            static_cast<void>(host.invoke_as(handle_of(player), custom->second.on_end, call));
        }
        emit(Side::server, "effect.ended",
             make_map({{"player", handle_of(player)}, {"effect", std::string{kind}}}));
    }
    if (removed)
        player.effects_dirty = true;
    return removed;
}

void GameplayService::Impl::tick_stats_and_effects(PlayerRecord &player, float seconds)
{
    if (player.alive)
        for (const auto &[name, definition] : server_registry.stats)
            if (definition.decay != 0.0)
                static_cast<void>(set_stat(player, name, stat(player, name) - definition.decay * seconds,
                                           false));
    std::vector<std::string> expired;
    // Effects may add or remove effects from their callbacks: walk a copy of
    // the kinds and look each one up again.
    const std::size_t count = player.effects.size();
    for (std::size_t index{}; index < count && index < player.effects.size(); ++index)
    {
        ActiveEffect &effect = player.effects[index];
        const auto custom = server_registry.effects.find(effect.kind);
        const float interval = custom != server_registry.effects.end() ? custom->second.interval : 1.0F;
        effect.interval_left -= seconds;
        // A whole number of intervals in a duration ticks that many times,
        // whatever the step's rounding.
        if (effect.interval_left <= 1.0e-4F)
        {
            effect.interval_left += interval;
            const float strength = effect.strength;
            const std::string kind = effect.kind;
            const std::string stat_name = effect.stat;
            if (player.alive)
            {
                if (kind == "regen")
                    heal(player, strength * interval);
                else if (kind == "damage")
                    damage(player, strength * interval, DamageCause::effect, player.position);
                else if (kind == "stat" && server_registry.stats.contains(stat_name))
                    static_cast<void>(set_stat(player, stat_name,
                                               stat(player, stat_name) + strength * interval, false));
                else if (custom != server_registry.effects.end() && custom->second.on_tick.valid())
                {
                    const std::array<Value, 2> call{Value{handle_of(player)}, Value{strength}};
                    static_cast<void>(host.invoke_as(handle_of(player), custom->second.on_tick, call));
                }
            }
            if (index >= player.effects.size())
                break;
        }
        ActiveEffect &current = player.effects[index];
        if (current.remaining >= 0.0F)
        {
            current.remaining -= seconds;
            if (current.remaining <= 1.0e-4F)
                expired.push_back(current.kind);
        }
    }
    for (const std::string &kind : expired)
        static_cast<void>(remove_effect(player, kind));
}

float GameplayService::Impl::speed_multiplier(const PlayerRecord &player) const
{
    float multiplier = 1.0F;
    for (const ActiveEffect &effect : player.effects)
        if (effect.kind == "speed")
            multiplier *= effect.strength;
    return std::clamp(multiplier, 0.0F, 4.0F);
}

void GameplayService::Impl::replicate_player(PlayerRecord &player, bool force)
{
    // Stats: what changed by a visible amount, at most ten times a second.
    if (force || player.stats_full_resend || replicate_due)
    {
        Map changed;
        for (const auto &[name, value] : player.stats)
        {
            const auto sent = player.stats_sent.find(name);
            if (player.stats_full_resend || sent == player.stats_sent.end() ||
                std::abs(sent->second - value) >= 0.05)
            {
                changed.emplace_back(name, value);
                player.stats_sent[name] = value;
            }
        }
        if (!changed.empty() || player.stats_full_resend)
            out().to_player(player.gid,
                            make_map({{"t", "stats"},
                                      {"values", Value{std::move(changed)}},
                                      {"full", player.stats_full_resend}}),
                            true);
        player.stats_full_resend = false;
    }
    if (player.effects_dirty || force)
    {
        player.effects_dirty = false;
        Array list;
        for (const ActiveEffect &effect : player.effects)
        {
            const auto custom = server_registry.effects.find(effect.kind);
            list.push_back(make_map({{"kind", effect.kind},
                                     {"remaining", effect.remaining},
                                     {"strength", effect.strength},
                                     {"icon", custom != server_registry.effects.end() ? custom->second.icon : std::string{}}}));
        }
        out().to_player(player.gid,
                        make_map({{"t", "effects"}, {"list", Value{std::move(list)}},
                                  {"speed", speed_multiplier(player)}}),
                        true);
    }
}

} // namespace openminerz::modding::gameplay
