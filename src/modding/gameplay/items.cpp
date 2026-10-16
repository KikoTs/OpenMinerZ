// (Items) Held mod items and mod weapons: the items.define fields `hold`,
// `scale` and `weapon`, the "held" messages that tell every client what each
// player has in its hand, and the server's checks of a swing made with a mod
// weapon.
//
// Stock messages carry the fallback of what a player holds (ChangeCarried-
// ItemMessage, MeleePlayerMessage, PlayerUpdateMessage's mode). A session
// with server mods admits only OpenMinerZ peers, so the mod item rides the
// mod channel beside them:
//
//   client -> server  held  {key}         the sender's hand now holds mod
//                                         item `key` ("" for none)
//   server -> client  held  {gid, item}   player gid holds registry item
//                                         `item` (0 for none)
//
// What a client shows is presentation (the item's model on that player); the
// server believes a mod weapon only when the player's stock hand holds its
// fallback and its last inventory report lists it, and then judges every
// claimed hit by the weapon's own damage, reach and rate - the server's copy
// of the definition, never the client's.

#include "internal.hpp"

#include "openminerz/inventory.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr std::uint32_t maximum_item_id = 4096U;

[[nodiscard]] double field_number(const Value &table, std::string_view key, double fallback)
{
    const Value *field = table.field(key);
    return field != nullptr && field->is_number() && std::isfinite(field->as_number())
               ? field->as_number()
               : fallback;
}

} // namespace

std::optional<HoldStyle> hold_style_from_name(std::string_view name) noexcept
{
    for (std::size_t index = 0U; index < hold_style_names.size(); ++index)
        if (hold_style_names[index] == name)
            return static_cast<HoldStyle>(index);
    return std::nullopt;
}

std::string_view hold_style_name(HoldStyle style) noexcept
{
    const auto index = static_cast<std::size_t>(style);
    return index < hold_style_names.size() ? hold_style_names[index] : hold_style_names[0];
}

void GameplayService::Impl::define_item_presentation(ItemDef &item, const Value &def)
{
    item.scale = static_cast<float>(std::clamp(number_or(def, "scale", 1.0), 0.05, 4.0));
    if (const Value *weapon = def.field("weapon"); weapon != nullptr && !weapon->is_nil())
    {
        // An empty table reads as an empty list.
        const bool table = weapon->type() == ValueType::map ||
                           (weapon->type() == ValueType::array && weapon->as_array().empty());
        if (!table)
            throw ScriptError{std::format("item '{}': weapon must be a table such as "
                                          "{{ damage = 1.5, reach = 5, cooldown = 0.5 }}",
                                          item.key)};
        const auto fallback = static_cast<InventoryItemId>(item.fallback);
        // The press that swings is the fallback's own: only items that strike
        // through the base input (InventoryItem.ProcessInput) can carry one.
        if (!item_is_melee_weapon(fallback) || !item_uses_tool_input(fallback) || is_block_item(fallback))
            throw ScriptError{std::format(
                "item '{}': a weapon's fallback must be an item that strikes, such as knife, "
                "stone_axe or stick (not a block, gun, grenade or instrument)",
                item.key)};
        const InventoryProfile profile =
            backend.modern_rules() ? InventoryProfile::modern : InventoryProfile::classic;
        ItemWeapon stats;
        stats.damage = static_cast<float>(
            std::clamp(field_number(*weapon, "damage", item_enemy_damage(fallback, profile)), 0.0, 1000.0));
        stats.player_damage = static_cast<float>(std::clamp(
            field_number(*weapon, "player_damage", item_player_melee_damage(fallback) * 100.0), 0.0, 1000.0));
        stats.reach = static_cast<float>(std::clamp(field_number(*weapon, "reach", 5.0), 1.0, 8.0));
        stats.cooldown = static_cast<float>(
            std::clamp(field_number(*weapon, "cooldown", item_use_cooldown_seconds(fallback)), 0.1, 10.0));
        item.weapon = stats;
    }
    const std::string hold = string_or(def, "hold", "");
    if (hold.empty())
        item.hold = item.weapon.has_value() ? HoldStyle::swing : HoldStyle::fallback;
    else if (const auto style = hold_style_from_name(hold))
        item.hold = *style;
    else
        throw ScriptError{std::format("item '{}': hold must be fallback, item, swing, tool, block, "
                                      "fist, eat or drink (not '{}')",
                                      item.key, hold)};
}

Value item_hand_value(const ItemDef &item)
{
    Map hand{{"hold", Value{std::string{hold_style_name(item.hold)}}},
             {"scale", Value{static_cast<double>(item.scale)}},
             {"cooldown", Value{static_cast<double>(item.cooldown)}}};
    if (item.weapon.has_value())
        hand.emplace_back("weapon", make_map({{"damage", static_cast<double>(item.weapon->damage)},
                                              {"player_damage", static_cast<double>(item.weapon->player_damage)},
                                              {"reach", static_cast<double>(item.weapon->reach)},
                                              {"cooldown", static_cast<double>(item.weapon->cooldown)}}));
    return Value{std::move(hand)};
}

void read_item_hand(ClientItemType &item, const Value *hand)
{
    if (hand == nullptr || hand->type() != ValueType::map)
        return;
    if (const Value *hold = hand->field("hold"); hold != nullptr && hold->type() == ValueType::string)
        item.hold = hold_style_from_name(hold->as_string()).value_or(HoldStyle::fallback);
    item.scale = static_cast<float>(std::clamp(field_number(*hand, "scale", 1.0), 0.05, 4.0));
    item.cooldown = static_cast<float>(std::clamp(field_number(*hand, "cooldown", 0.25), 0.05, 60.0));
    if (const Value *weapon = hand->field("weapon"); weapon != nullptr && weapon->type() == ValueType::map)
    {
        ItemWeapon stats;
        stats.damage = static_cast<float>(std::clamp(field_number(*weapon, "damage", 0.0), 0.0, 1000.0));
        stats.player_damage =
            static_cast<float>(std::clamp(field_number(*weapon, "player_damage", 21.0), 0.0, 1000.0));
        stats.reach = static_cast<float>(std::clamp(field_number(*weapon, "reach", 5.0), 1.0, 8.0));
        stats.cooldown = static_cast<float>(std::clamp(field_number(*weapon, "cooldown", 0.5), 0.1, 10.0));
        item.weapon = stats;
    }
}

// ------------------------------------------------------------ the server

void GameplayService::Impl::server_held_message(PlayerRecord &player, const Value &message)
{
    std::uint32_t held{};
    if (const Value *key = message.field("key"); key != nullptr && key->type() == ValueType::string &&
                                                 !key->as_string().empty())
    {
        const ItemDef *item = server_registry.item(std::string_view{key->as_string()}.substr(0U, 256U));
        if (item == nullptr)
            return;
        held = item->id;
    }
    if (player.held_item == held)
        return;
    player.held_item = held;
    out().to_all(make_map({{"t", "held"},
                           {"gid", static_cast<std::int64_t>(player.gid)},
                           {"item", static_cast<std::int64_t>(held)}}),
                 true);
}

void GameplayService::Impl::send_held_items(std::uint8_t gid)
{
    for (const PlayerRecord &player : players)
        if (player.present && player.held_item != 0U)
            out().to_player(gid,
                            make_map({{"t", "held"},
                                      {"gid", static_cast<std::int64_t>(player.gid)},
                                      {"item", static_cast<std::int64_t>(player.held_item)}}),
                            true);
}

void GameplayService::Impl::forget_held_item(const PlayerRecord &player)
{
    // The gid may be someone else's next.
    if (player.held_item != 0U)
        out().to_all(make_map({{"t", "held"}, {"gid", static_cast<std::int64_t>(player.gid)}, {"item", std::int64_t{0}}}),
                     true);
}

const ItemDef *GameplayService::Impl::held_weapon_def(const PlayerRecord &player, std::uint16_t item) const
{
    const ItemDef *held = nullptr;
    if (player.local)
    {
        // This process's own player: its hand, as it is now.
        const std::optional<StackView> stack = backend.local_held();
        if (!stack.has_value() || stack->mod_item == 0U || stack->item != item)
            return nullptr;
        held = server_registry.item(stack->mod_item);
    }
    else
    {
        if (player.held_item == 0U)
            return nullptr;
        held = server_registry.item(player.held_item);
        // ... and the player has one, by its own last inventory report.
        const auto found = held != nullptr ? player.inventory_counts.find(held->key)
                                           : player.inventory_counts.end();
        if (found == player.inventory_counts.end() || found->second == 0U)
            return nullptr;
    }
    if (held == nullptr || !held->weapon.has_value() || held->fallback != item)
        return nullptr;
    return held;
}

bool GameplayService::Impl::take_weapon_swing(PlayerRecord &player, const ItemWeapon &weapon)
{
    // A swing every `cooldown`, and a lag spike's worth bunched together.
    const double rate = 1.05 / std::max(0.1, static_cast<double>(weapon.cooldown));
    const double burst = 2.0 + rate * 0.5;
    if (player.weapon_tokens_at < 0.0)
        player.weapon_tokens = static_cast<float>(burst);
    else
        player.weapon_tokens = static_cast<float>(
            std::min(burst, player.weapon_tokens + std::max(0.0, server_time - player.weapon_tokens_at) * rate));
    player.weapon_tokens_at = server_time;
    if (player.weapon_tokens < 1.0F)
        return false;
    player.weapon_tokens -= 1.0F;
    return true;
}

std::optional<ItemWeapon> GameplayService::held_weapon(std::uint8_t gid, std::uint16_t item) const
{
    if (!server_running_)
        return std::nullopt;
    for (const PlayerRecord &player : impl_->players)
        if (player.present && player.gid == gid)
        {
            const ItemDef *weapon = impl_->held_weapon_def(player, item);
            return weapon != nullptr ? weapon->weapon : std::nullopt;
        }
    return std::nullopt;
}

// ------------------------------------------------------------ the client

void GameplayService::Impl::client_held_message(const Value &message)
{
    const Value *gid = message.field("gid");
    const Value *item = message.field("item");
    if (gid == nullptr || item == nullptr || !gid->is_number() || !item->is_number())
        return;
    const double player = gid->as_number();
    const double id = item->as_number();
    if (!(player >= 0.0 && player <= 255.0) || !(id >= 0.0 && id <= maximum_item_id))
        return;
    if (id < 1.0)
        client_held.erase(static_cast<std::uint8_t>(player));
    else
        client_held[static_cast<std::uint8_t>(player)] = static_cast<std::uint32_t>(id);
}

void GameplayService::Impl::client_report_held()
{
    const std::optional<StackView> stack = backend.local_held();
    const ClientItemType *type =
        stack.has_value() && stack->mod_item != 0U ? owner.client_item(stack->mod_item) : nullptr;
    const std::uint32_t held = type != nullptr ? type->id : 0U;
    if (client_held_known && held == client_held_sent)
        return;
    client_held_known = true;
    client_held_sent = held;
    out().to_server(make_map({{"t", "held"}, {"key", type != nullptr ? type->key : std::string{}}}));
}

std::uint32_t GameplayService::client_held_item(std::uint8_t gid) const noexcept
{
    if (!client_running_)
        return 0U;
    const auto found = impl_->client_held.find(gid);
    return found != impl_->client_held.end() ? found->second : 0U;
}

} // namespace openminerz::modding::gameplay
