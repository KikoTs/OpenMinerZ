// (mod-ai) Retail enemy rules: which retail enemies may appear, where, and
// at which distance tier, for the whole session.
//
// CastleMiner Z has no host-side enemy spawning: every peer spawns the
// zombies, skeletons, aliens and spawner waves of its OWN player and tells the
// others (SpawnEnemyMessage), and the host alone decides dragons. So a rule
// only means something when every peer applies it. A server mod's rules are
// replicated to every client of the session ("retail" message) and each peer
// applies them to its own spawns and to the spawns it is sent; the host
// applies them to dragon requests. Server mods only run in sessions no stock
// player can join (net::session_authoritative), so no peer can ignore them
// unseen, and without rules (`active` false) nothing here is even consulted:
// the spawn paths are byte for byte retail's.

#include "internal.hpp"

#include "openminerz/world/custom_terrain.hpp"
#include "openminerz/world/enemy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace openminerz::modding::gameplay
{
namespace
{

constexpr std::array<std::string_view, static_cast<std::size_t>(RetailEnemyClass::count)> class_names{
    "zombie", "skeleton", "archer", "felguard", "alien", "dragon", "spawner", "hell"};

[[nodiscard]] std::uint16_t class_bit(RetailEnemyClass type) noexcept
{
    return static_cast<std::uint16_t>(1U << static_cast<unsigned>(type));
}

[[nodiscard]] bool region_matches(const RetailEnemyRegion &region, Vec3 position, std::string_view biome)
{
    if (region.has_box && (position.x < region.min_x || position.x > region.max_x ||
                           position.z < region.min_z || position.z > region.max_z))
        return false;
    if (region.has_heights && (position.y < region.min_y || position.y > region.max_y))
        return false;
    return region.biome.empty() || region.biome == biome;
}

// The family switches of a rules table or region: `zombie = false`, ...
void read_classes(const Value &table, std::uint16_t &set, std::uint16_t &on)
{
    for (std::size_t index{}; index < class_names.size(); ++index)
    {
        const Value *flag = table.field(class_names[index]);
        if (flag == nullptr || flag->is_nil())
            continue;
        if (flag->type() != ValueType::boolean)
            throw ScriptError{std::format("enemies.rules: '{}' must be true or false", class_names[index])};
        const auto bit = static_cast<std::uint16_t>(1U << index);
        set = static_cast<std::uint16_t>(set | bit);
        on = flag->as_bool() ? static_cast<std::uint16_t>(on | bit) : static_cast<std::uint16_t>(on & ~bit);
    }
}

[[nodiscard]] float finite_or(double value, float fallback) noexcept
{
    return std::isfinite(value) ? static_cast<float>(value) : fallback;
}

} // namespace

const char *retail_enemy_class_name(RetailEnemyClass type) noexcept
{
    const auto index = static_cast<std::size_t>(type);
    return index < class_names.size() ? class_names[index].data() : "zombie";
}

std::optional<RetailEnemyClass> retail_enemy_class_from_name(std::string_view name) noexcept
{
    for (std::size_t index{}; index < class_names.size(); ++index)
        if (class_names[index] == name)
            return static_cast<RetailEnemyClass>(index);
    return std::nullopt;
}

RetailEnemyClass retail_enemy_class_of(int type) noexcept
{
    if (type < 0 || type >= world::enemy_type_count)
        return RetailEnemyClass::zombie;
    switch (world::enemy_stats(type).enemy_class)
    {
    case world::EnemyClass::zombie:
        return RetailEnemyClass::zombie;
    case world::EnemyClass::archer:
        return RetailEnemyClass::archer;
    case world::EnemyClass::skeleton:
    case world::EnemyClass::skeleton_sword:
    case world::EnemyClass::skeleton_axes:
        return RetailEnemyClass::skeleton;
    case world::EnemyClass::felguard:
        return RetailEnemyClass::felguard;
    case world::EnemyClass::alien:
        return RetailEnemyClass::alien;
    }
    return RetailEnemyClass::zombie;
}

bool RetailEnemyRules::allows(RetailEnemyClass type, Vec3 position, std::string_view biome) const
{
    if (!active)
        return true;
    const std::uint16_t bit = class_bit(type);
    bool on = enabled && (classes & bit) != 0U;
    for (const RetailEnemyRegion &region : regions)
    {
        if (!region_matches(region, position, biome))
            continue;
        if (region.enabled.has_value())
            on = *region.enabled;
        if ((region.classes_set & bit) != 0U)
            on = (region.classes_on & bit) != 0U;
    }
    return on;
}

float RetailEnemyRules::tier_distance(float distance, Vec3 position, std::string_view biome) const
{
    if (!active)
        return distance;
    float result = distance * distance_scale + distance_offset;
    if (distance_maximum >= 0.0F)
        result = std::min(result, distance_maximum);
    for (const RetailEnemyRegion &region : regions)
        if (region.tier_distance.has_value() && region_matches(region, position, biome))
            result = *region.tier_distance;
    return std::max(0.0F, result);
}

bool RetailEnemyRules::needs_biome() const noexcept
{
    return std::ranges::any_of(regions, [](const RetailEnemyRegion &region) { return !region.biome.empty(); });
}

// ----------------------------------------------------------------- parsing

RetailEnemyRules GameplayService::Impl::parse_retail_rules(const Value &table) const
{
    RetailEnemyRules rules;
    rules.active = true;
    rules.enabled = bool_or(table, "enabled", true);
    if (const Value *clear = table.field("clear_existing"); clear != nullptr && !clear->is_nil())
    {
        if (clear->type() != ValueType::boolean)
            throw ScriptError{"enemies.rules: clear_existing must be true or false"};
        rules.clear_existing = clear->as_bool();
    }
    std::uint16_t set{};
    std::uint16_t on{0xFFFFU};
    read_classes(table, set, on);
    rules.classes = on;
    if (const Value *distance = table.field("distance"); distance != nullptr && !distance->is_nil())
    {
        if (distance->type() != ValueType::map)
            throw ScriptError{"enemies.rules: distance must be {scale, offset, max}"};
        rules.distance_scale = std::clamp(finite_or(number_or(*distance, "scale", 1.0), 1.0F), 0.0F, 100.0F);
        rules.distance_offset = std::clamp(finite_or(number_or(*distance, "offset", 0.0), 0.0F), -1.0e6F, 1.0e6F);
        rules.distance_maximum = std::clamp(finite_or(number_or(*distance, "max", -1.0), -1.0F), -1.0F, 1.0e6F);
    }
    if (const Value *regions = table.field("regions"); regions != nullptr && !regions->is_nil())
    {
        if (regions->type() != ValueType::array && !(regions->type() == ValueType::map && regions->as_map().empty()))
            throw ScriptError{"enemies.rules: regions must be a list"};
        if (regions->type() == ValueType::array && regions->as_array().size() > 256U)
            throw ScriptError{"enemies.rules: at most 256 regions"};
        if (regions->type() == ValueType::array)
            for (const Value &entry : regions->as_array())
            {
                if (entry.type() != ValueType::map)
                    throw ScriptError{"enemies.rules: each region is a table"};
                RetailEnemyRegion region;
                const Value *low = entry.field("min");
                const Value *high = entry.field("max");
                if ((low != nullptr && !low->is_nil()) != (high != nullptr && !high->is_nil()))
                    throw ScriptError{"enemies.rules: a region box needs both min and max"};
                if (low != nullptr && !low->is_nil())
                {
                    const Vec3 a = to_vec3(*low, "region min");
                    const Vec3 b = to_vec3(*high, "region max");
                    region.has_box = true;
                    region.min_x = std::min(a.x, b.x);
                    region.max_x = std::max(a.x, b.x);
                    region.min_z = std::min(a.z, b.z);
                    region.max_z = std::max(a.z, b.z);
                }
                if (entry.field("y_min") != nullptr || entry.field("y_max") != nullptr)
                {
                    region.has_heights = true;
                    region.min_y = number_or(entry, "y_min", -1.0e9);
                    region.max_y = number_or(entry, "y_max", 1.0e9);
                }
                region.biome = string_or(entry, "biome", "");
                if (const Value *enabled = entry.field("enabled"); enabled != nullptr && !enabled->is_nil())
                    region.enabled = enabled->as_bool();
                std::uint16_t region_on{};
                read_classes(entry, region.classes_set, region_on);
                region.classes_on = region_on;
                if (const Value *tier = entry.field("tier"); tier != nullptr && !tier->is_nil())
                    region.tier_distance = std::clamp(finite_or(tier->as_number(), 0.0F), 0.0F, 1.0e6F);
                rules.regions.push_back(std::move(region));
            }
    }
    return rules;
}

Value GameplayService::Impl::retail_rules_value(const RetailEnemyRules &rules) const
{
    Array regions;
    for (const RetailEnemyRegion &region : rules.regions)
    {
        Map entry{{"box", Value{region.has_box}}};
        if (region.has_box)
        {
            entry.emplace_back("x0", region.min_x);
            entry.emplace_back("z0", region.min_z);
            entry.emplace_back("x1", region.max_x);
            entry.emplace_back("z1", region.max_z);
        }
        if (region.has_heights)
        {
            entry.emplace_back("y0", region.min_y);
            entry.emplace_back("y1", region.max_y);
        }
        if (!region.biome.empty())
            entry.emplace_back("biome", region.biome);
        if (region.enabled.has_value())
            entry.emplace_back("on", *region.enabled);
        entry.emplace_back("set", static_cast<std::int64_t>(region.classes_set));
        entry.emplace_back("bits", static_cast<std::int64_t>(region.classes_on));
        if (region.tier_distance.has_value())
            entry.emplace_back("tier", *region.tier_distance);
        regions.emplace_back(std::move(entry));
    }
    return make_map({{"t", "retail"},
                     {"active", rules.active},
                     {"enabled", rules.enabled},
                     {"classes", static_cast<std::int64_t>(rules.classes)},
                     {"scale", rules.distance_scale},
                     {"offset", rules.distance_offset},
                     {"max", rules.distance_maximum},
                     {"clear", rules.clear_existing},
                     {"regions", Value{std::move(regions)}}});
}

RetailEnemyRules GameplayService::Impl::retail_rules_from_value(const Value &value) const
{
    // From the host, still bounded like anything a peer sends.
    RetailEnemyRules rules;
    rules.active = bool_or(value, "active", false);
    rules.enabled = bool_or(value, "enabled", true);
    rules.classes = static_cast<std::uint16_t>(std::clamp(number_or(value, "classes", 65535.0), 0.0, 65535.0));
    rules.distance_scale = std::clamp(finite_or(number_or(value, "scale", 1.0), 1.0F), 0.0F, 100.0F);
    rules.distance_offset = std::clamp(finite_or(number_or(value, "offset", 0.0), 0.0F), -1.0e6F, 1.0e6F);
    rules.distance_maximum = std::clamp(finite_or(number_or(value, "max", -1.0), -1.0F), -1.0F, 1.0e6F);
    rules.clear_existing = rules.active && bool_or(value, "clear", false);
    if (const Value *regions = value.field("regions"); regions != nullptr && regions->type() == ValueType::array)
        for (const Value &entry : regions->as_array())
        {
            if (rules.regions.size() >= 256U || entry.type() != ValueType::map)
                break;
            RetailEnemyRegion region;
            region.has_box = bool_or(entry, "box", false);
            region.min_x = number_or(entry, "x0", 0.0);
            region.min_z = number_or(entry, "z0", 0.0);
            region.max_x = number_or(entry, "x1", 0.0);
            region.max_z = number_or(entry, "z1", 0.0);
            region.has_heights = entry.field("y0") != nullptr;
            region.min_y = number_or(entry, "y0", -1.0e9);
            region.max_y = number_or(entry, "y1", 1.0e9);
            region.biome = string_or(entry, "biome", "").substr(0, 96U);
            if (const Value *on = entry.field("on"); on != nullptr && on->type() == ValueType::boolean)
                region.enabled = on->as_bool();
            region.classes_set = static_cast<std::uint16_t>(std::clamp(number_or(entry, "set", 0.0), 0.0, 65535.0));
            region.classes_on = static_cast<std::uint16_t>(std::clamp(number_or(entry, "bits", 0.0), 0.0, 65535.0));
            if (const Value *tier = entry.field("tier"); tier != nullptr && tier->is_number())
                region.tier_distance = std::clamp(finite_or(tier->as_number(), 0.0F), 0.0F, 1.0e6F);
            rules.regions.push_back(std::move(region));
        }
    return rules;
}

std::string GameplayService::Impl::biome_name(int x, int z) const
{
    std::string key = backend.biome_key(x, z);
    if (!key.empty())
        return key;
    // What world.biome answers: a retail name, or a mod world's biome key.
    return world::biome_name(backend.biome(x, z));
}

const RetailEnemyRules &GameplayService::Impl::effective_retail_rules() const noexcept
{
    return owner.server_running() ? ai.retail : ai_client.retail;
}

// ------------------------------------------------------- the public queries

bool GameplayService::retail_enemy_allowed(RetailEnemyClass type, Vec3 position) const
{
    const Impl &impl = *impl_;
    if (!server_running_ && !client_running_)
        return true;
    // (A joined client without the host's rules yet holds the inactive ones:
    // what another peer spawned, it spawned by the rules it had.)
    const RetailEnemyRules &rules = impl.effective_retail_rules();
    if (!rules.active)
        return true;
    const std::string biome = rules.needs_biome()
                                  ? impl.biome_name(static_cast<int>(std::floor(position.x)),
                                                    static_cast<int>(std::floor(position.z)))
                                  : std::string{};
    return rules.allows(type, position, biome);
}

float GameplayService::retail_tier_distance(float distance, Vec3 position) const
{
    if (!running())
        return distance;
    const Impl &impl = *impl_;
    const RetailEnemyRules &rules = impl.effective_retail_rules();
    if (!rules.active)
        return distance;
    const std::string biome = rules.needs_biome()
                                  ? impl.biome_name(static_cast<int>(std::floor(position.x)),
                                                    static_cast<int>(std::floor(position.z)))
                                  : std::string{};
    return rules.tier_distance(distance, position, biome);
}

bool GameplayService::retail_spawn_allowed(RetailEnemyClass type, Vec3 position) const
{
    // A joined client holds its own retail spawns until the host's rules are
    // in, however long that takes: they come with its full state, so in a
    // working session that is moments after its mod channel opens.
    if (!server_running_ && client_running_ && !retail_rules_known())
        return false;
    return retail_enemy_allowed(type, position);
}

bool GameplayService::retail_rules_known() const noexcept
{
    return server_running_ || (client_running_ && impl_->ai_client.retail_known);
}

bool GameplayService::retail_spawner_silent(std::span<const std::uint8_t> wave_types, Vec3 position) const
{
    if (wave_types.empty())
        return false;
    return std::ranges::all_of(wave_types, [&](std::uint8_t wire) {
        const std::optional<int> type = world::enemy_type_from_wire(wire, true);
        if (!type.has_value())
            return true;
        if (position.y < -40.0 && !retail_spawn_allowed(RetailEnemyClass::hell, position))
            return true;
        return !retail_spawn_allowed(retail_enemy_class_of(*type), position);
    });
}

bool GameplayService::take_retail_clear() noexcept
{
    return std::exchange(impl_->ai_client.retail_clear, false);
}

bool GameplayService::retail_rules_active() const noexcept
{
    return running() && impl_->effective_retail_rules().active;
}

} // namespace openminerz::modding::gameplay
