// (Integration) Mod item identity where stock messages carry only the retail
// fallback: crate slots (ItemCrateMessage) and network pickups
// (CreatePickupMessage / ConsumePickupMessage).
//
// A session with server mods admits only OpenMinerZ peers, so the identity
// rides the mod channel beside the stock message, and the stock bytes stay
// exactly what they are without mods:
//
//   client -> server  crate   {x, y, z, s, m}   the sender changed slot s of
//                                               the crate at x,y,z (m: mod
//                                               item id, 0 = a retail stack)
//                     pickup  {p, m}            the sender's pickup p is m
//   server -> client  crate   {cells=[[x,y,z,s,m], ...]} or {gone=[x,y,z]}
//                     pickup  {s, p, m}         spawner s's pickup p is m
//
// The server keeps the identities (crate ones persist in the sidecar), every
// client a copy; the app tags its own crate copy through
// GameplayBackend::tag_crate_slot and asks crate_slot_identity /
// take_pickup_identity whenever a stock message hands it a stack. An identity
// applies only while the slot or pickup holds the mod item's fallback, so a
// stale one can never turn a different retail item into a mod item. Like the
// player inventory tags, a client's report is taken on trust (CastleMiner Z
// inventories are peer-owned): it names an item the session defines and a
// crate within the sender's reach.

#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr std::size_t maximum_crate_tags = 65'536U;
constexpr std::size_t maximum_pickup_tags = 1'024U;
constexpr std::size_t crate_slots = 32U;
constexpr std::size_t crate_cells_per_message = 512U;
// The stock crate handler's reach (ItemCrateMessage within 10 blocks), with
// room for the replicated position lagging behind.
constexpr double crate_reach = 12.0;
constexpr int maximum_coordinate = 1 << 24;

[[nodiscard]] std::optional<std::int64_t> whole(const Value &message, std::string_view key) noexcept
{
    const Value *field = message.field(key);
    if (field == nullptr || field->type() != ValueType::integer)
        return std::nullopt;
    return std::get<std::int64_t>(field->storage());
}

[[nodiscard]] bool valid_cell(BlockPos cell) noexcept
{
    return std::abs(cell.x) < maximum_coordinate && std::abs(cell.y) < maximum_coordinate &&
           std::abs(cell.z) < maximum_coordinate;
}

[[nodiscard]] Value cell_entry(BlockPos cell, std::uint8_t slot, std::uint32_t mod_item)
{
    return Value{Array{Value{cell.x}, Value{cell.y}, Value{cell.z}, Value{static_cast<std::int64_t>(slot)},
                       Value{static_cast<std::int64_t>(mod_item)}}};
}

template <typename Key>
void remember(std::map<Key, std::uint32_t> &tags, std::deque<Key> &order, const Key &key,
              std::uint32_t mod_item)
{
    if (mod_item == 0U)
    {
        tags.erase(key);
        return;
    }
    if (tags.insert_or_assign(key, mod_item).second)
        order.push_back(key);
    while (order.size() > maximum_pickup_tags)
    {
        tags.erase(order.front());
        order.pop_front();
    }
}

} // namespace

// ------------------------------------------------------------------ server

bool GameplayService::Impl::set_crate_tag(BlockPos crate, std::uint8_t slot, std::uint32_t mod_item)
{
    const CrateSlot key{crate, slot};
    if (mod_item == 0U)
    {
        if (crate_tags.erase(key) == 0U)
            return false;
    }
    else
    {
        const ItemDef *item = server_registry.item(mod_item);
        if (item == nullptr || (!crate_tags.contains(key) && crate_tags.size() >= maximum_crate_tags))
            return false;
        crate_tags.insert_or_assign(key, mod_item);
        static_cast<void>(backend.tag_crate_slot(crate, slot, item->fallback, mod_item));
    }
    out().to_all(make_map({{"t", "crate"}, {"cells", Value{Array{cell_entry(crate, slot, mod_item)}}}}),
                 true);
    return true;
}

void GameplayService::Impl::server_crate_message(PlayerRecord &player, const Value &message)
{
    const auto x = whole(message, "x");
    const auto y = whole(message, "y");
    const auto z = whole(message, "z");
    const auto slot = whole(message, "s");
    const auto mod_item = whole(message, "m");
    if (!x || !y || !z || !slot || !mod_item || *slot < 0 || *slot >= static_cast<std::int64_t>(crate_slots) ||
        *mod_item < 0 || *mod_item > 0xFFFFFFFF)
        return;
    const BlockPos crate{static_cast<int>(std::clamp<std::int64_t>(*x, -maximum_coordinate, maximum_coordinate)),
                         static_cast<int>(std::clamp<std::int64_t>(*y, -maximum_coordinate, maximum_coordinate)),
                         static_cast<int>(std::clamp<std::int64_t>(*z, -maximum_coordinate, maximum_coordinate))};
    const Vec3 centre{crate.x + 0.5, crate.y + 0.5, crate.z + 0.5};
    if (!valid_cell(crate) || distance_squared(centre, player.position) > crate_reach * crate_reach)
    {
        log({}, LogLevel::debug, std::format("gameplay: crate identity from player {} refused", player.gid));
        return;
    }
    static_cast<void>(set_crate_tag(crate, static_cast<std::uint8_t>(*slot), static_cast<std::uint32_t>(*mod_item)));
}

void GameplayService::Impl::server_pickup_message(PlayerRecord &player, const Value &message)
{
    const auto pickup = whole(message, "p");
    const auto mod_item = whole(message, "m");
    if (!pickup || !mod_item || *pickup < std::numeric_limits<std::int32_t>::min() ||
        *pickup > std::numeric_limits<std::int32_t>::max() || *mod_item <= 0 || *mod_item > 0xFFFFFFFF ||
        server_registry.item(static_cast<std::uint32_t>(*mod_item)) == nullptr)
        return;
    // The spawner is the sender, by the channel's attribution.
    record_pickup(player.gid, static_cast<std::int32_t>(*pickup), static_cast<std::uint32_t>(*mod_item));
}

void GameplayService::Impl::record_pickup(std::uint8_t spawner, std::int32_t pickup, std::uint32_t mod_item)
{
    // (mod-world: qualified, so Impl::remember of the entity minds does not hide it.)
    gameplay::remember(pickup_tags, pickup_order, PickupKey{spawner, pickup}, mod_item);
    out().to_all(make_map({{"t", "pickup"},
                           {"s", static_cast<std::int64_t>(spawner)},
                           {"p", static_cast<std::int64_t>(pickup)},
                           {"m", static_cast<std::int64_t>(mod_item)}}),
                 true);
}

void GameplayService::Impl::send_crate_tags(std::uint8_t gid)
{
    Array cells;
    for (const auto &[key, mod_item] : crate_tags)
    {
        cells.push_back(cell_entry(key.first, key.second, mod_item));
        if (cells.size() >= crate_cells_per_message)
        {
            out().to_player(gid, make_map({{"t", "crate"}, {"cells", Value{std::move(cells)}}}), true);
            cells = {};
        }
    }
    if (!cells.empty())
        out().to_player(gid, make_map({{"t", "crate"}, {"cells", Value{std::move(cells)}}}), true);
}

void GameplayService::Impl::apply_crate_tags()
{
    for (const auto &[key, mod_item] : crate_tags)
        if (const ItemDef *item = server_registry.item(mod_item))
            static_cast<void>(backend.tag_crate_slot(key.first, key.second, item->fallback, mod_item));
}

// ------------------------------------------------------------------ client

void GameplayService::Impl::client_crate_message(const Value &message)
{
    const auto apply = [&](BlockPos crate, std::uint8_t slot, std::uint32_t mod_item) {
        const CrateSlot key{crate, slot};
        if (mod_item == 0U || mod_item > client_items.size())
        {
            client_crate_tags.erase(key);
            return;
        }
        if (!client_crate_tags.contains(key) && client_crate_tags.size() >= maximum_crate_tags)
            return;
        client_crate_tags.insert_or_assign(key, mod_item);
        static_cast<void>(backend.tag_crate_slot(crate, slot, client_items[mod_item - 1U].fallback, mod_item));
    };
    if (const Value *gone = message.field("gone"); gone != nullptr)
    {
        if (const auto at = try_vec3(*gone); at.has_value() && finite(*at))
        {
            const BlockPos crate = floor_cell(*at);
            std::erase_if(client_crate_tags, [&](const auto &entry) { return entry.first.first == crate; });
        }
        return;
    }
    const Value *cells = message.field("cells");
    if (cells == nullptr || cells->type() != ValueType::array)
        return;
    for (const Value &entry : cells->as_array())
    {
        if (entry.type() != ValueType::array || entry.as_array().size() != 5U)
            continue;
        const Array &fields = entry.as_array();
        if (std::ranges::any_of(fields, [](const Value &field) { return field.type() != ValueType::integer; }))
            continue;
        const auto number = [&](std::size_t index) { return std::get<std::int64_t>(fields[index].storage()); };
        const BlockPos crate{static_cast<int>(std::clamp<std::int64_t>(number(0), -maximum_coordinate, maximum_coordinate)),
                             static_cast<int>(std::clamp<std::int64_t>(number(1), -maximum_coordinate, maximum_coordinate)),
                             static_cast<int>(std::clamp<std::int64_t>(number(2), -maximum_coordinate, maximum_coordinate))};
        if (number(3) < 0 || number(3) >= static_cast<std::int64_t>(crate_slots) || number(4) < 0 ||
            number(4) > 0xFFFFFFFF)
            continue;
        apply(crate, static_cast<std::uint8_t>(number(3)), static_cast<std::uint32_t>(number(4)));
    }
}

void GameplayService::Impl::client_pickup_message(const Value &message)
{
    const auto spawner = whole(message, "s");
    const auto pickup = whole(message, "p");
    const auto mod_item = whole(message, "m");
    if (!spawner || !pickup || !mod_item || *spawner < 0 || *spawner > 255 ||
        *pickup < std::numeric_limits<std::int32_t>::min() || *pickup > std::numeric_limits<std::int32_t>::max() ||
        *mod_item < 0 || *mod_item > 0xFFFFFFFF)
        return;
    gameplay::remember(client_pickup_tags, client_pickup_order,
             PickupKey{static_cast<std::uint8_t>(*spawner), static_cast<std::int32_t>(*pickup)},
             static_cast<std::uint32_t>(*mod_item));
}

// ------------------------------------------------------------------ the hooks

void GameplayService::local_crate_slot(BlockPos crate, std::size_t index, StackView stack)
{
    if (index >= crate_slots || !valid_cell(crate))
        return;
    Impl &impl = *impl_;
    const auto slot = static_cast<std::uint8_t>(index);
    if (server_running_)
    {
        const std::uint32_t mod_item =
            impl.server_registry.item(stack.mod_item) != nullptr ? stack.mod_item : 0U;
        static_cast<void>(impl.set_crate_tag(crate, slot, mod_item));
        return;
    }
    if (!client_running_)
        return;
    const Impl::CrateSlot key{crate, slot};
    const std::uint32_t mod_item = client_item(stack.mod_item) != nullptr ? stack.mod_item : 0U;
    if (mod_item == 0U && !impl.client_crate_tags.contains(key))
        return; // a retail stack where the server knows of no mod item
    if (mod_item == 0U)
        impl.client_crate_tags.erase(key);
    else
        impl.client_crate_tags.insert_or_assign(key, mod_item);
    impl.out().to_server(make_map({{"t", "crate"},
                                   {"x", crate.x},
                                   {"y", crate.y},
                                   {"z", crate.z},
                                   {"s", static_cast<std::int64_t>(slot)},
                                   {"m", static_cast<std::int64_t>(mod_item)}}));
}

std::uint32_t GameplayService::crate_slot_identity(BlockPos crate, std::size_t index,
                                                   std::uint16_t item) const noexcept
{
    if (index >= crate_slots)
        return 0U;
    const Impl &impl = *impl_;
    const Impl::CrateSlot key{crate, static_cast<std::uint8_t>(index)};
    if (server_running_)
    {
        const auto found = impl.crate_tags.find(key);
        if (found == impl.crate_tags.end())
            return 0U;
        const ItemDef *definition = impl.server_registry.item(found->second);
        return definition != nullptr && definition->fallback == item ? found->second : 0U;
    }
    if (!client_running_)
        return 0U;
    const auto found = impl.client_crate_tags.find(key);
    if (found == impl.client_crate_tags.end())
        return 0U;
    const ClientItemType *definition = client_item(found->second);
    return definition != nullptr && definition->fallback == item ? found->second : 0U;
}

void GameplayService::crate_destroyed(BlockPos crate)
{
    Impl &impl = *impl_;
    if (server_running_)
    {
        if (std::erase_if(impl.crate_tags, [&](const auto &entry) { return entry.first.first == crate; }) != 0U)
            impl.out().to_all(make_map({{"t", "crate"}, {"gone", cell_value(crate)}}), true);
        return;
    }
    if (client_running_)
        std::erase_if(impl.client_crate_tags, [&](const auto &entry) { return entry.first.first == crate; });
}

void GameplayService::local_pickup_published(std::uint8_t spawner_gid, std::int32_t pickup_id, StackView stack)
{
    if (stack.mod_item == 0U)
        return;
    Impl &impl = *impl_;
    if (server_running_)
    {
        if (impl.server_registry.item(stack.mod_item) != nullptr)
            impl.record_pickup(spawner_gid, pickup_id, stack.mod_item);
        return;
    }
    if (client_running_ && client_item(stack.mod_item) != nullptr)
        impl.out().to_server(make_map({{"t", "pickup"},
                                       {"p", static_cast<std::int64_t>(pickup_id)},
                                       {"m", static_cast<std::int64_t>(stack.mod_item)}}));
}

std::uint32_t GameplayService::take_pickup_identity(std::uint8_t spawner_gid, std::int32_t pickup_id,
                                                    std::uint16_t item)
{
    Impl &impl = *impl_;
    const Impl::PickupKey key{spawner_gid, pickup_id};
    std::map<Impl::PickupKey, std::uint32_t> &tags = server_running_ ? impl.pickup_tags : impl.client_pickup_tags;
    const auto found = tags.find(key);
    if (found == tags.end())
        return 0U;
    const std::uint32_t mod_item = found->second;
    tags.erase(found);
    if (server_running_)
    {
        const ItemDef *definition = impl.server_registry.item(mod_item);
        return definition != nullptr && definition->fallback == item ? mod_item : 0U;
    }
    const ClientItemType *definition = client_item(mod_item);
    return definition != nullptr && definition->fallback == item ? mod_item : 0U;
}

} // namespace openminerz::modding::gameplay
