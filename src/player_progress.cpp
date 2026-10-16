#include "openminerz/player_progress.hpp"
#include "openminerz/atomic_file.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <sstream>

namespace openminerz
{
namespace
{
using M = ProgressMetric;
using G = AchievementGroup;
// Steam API name, Name and HowToUnlock exactly as both shipped builds'
// CastleMinerZAchievementManager.CreateAcheivements constructs them (the two
// are identical), spelling included: "Veteren MinerZ", "Play Online For 1
// Hours", "Survive for 1 Days", "Craft 1 items". Generated from the owned
// installs by tools/OriginalClientInterop/PresentationParityProbe.
struct RetailAchievementText
{
    const char *id;
    const char *name;
    const char *how_to_unlock;
};
constexpr RetailAchievementText retail_achievements[]{
#define OPENMINERZ_RETAIL_ACHIEVEMENT(index, api, name, how) {api, name, how},
#include "retail_achievements.inc"
#undef OPENMINERZ_RETAIL_ACHIEVEMENT
};
// What unlocks each one here, in the same order.
struct AchievementRule
{
    ProgressMetric metric;
    double target;
    AchievementGroup group;
};
constexpr std::array<AchievementRule, achievement_count> rules{{
    {M::play_seconds, 3600, G::survival}, // ACH_TIME_PLAYED_1
    {M::play_seconds, 36000, G::survival}, // ACH_TIME_PLAYED_10
    {M::play_seconds, 360000, G::survival}, // ACH_TIME_PLAYED_100
    {M::distance, 50, G::exploration}, // ACH_DISTANCE_50
    {M::distance, 200, G::exploration}, // ACH_DISTANCE_200
    {M::distance, 1000, G::exploration}, // ACH_DISTANCE_1000
    {M::distance, 2300, G::exploration}, // ACH_DISTANCE_2300
    {M::distance, 3000, G::exploration}, // ACH_DISTANCE_3000
    {M::distance, 3600, G::exploration}, // ACH_DISTANCE_3600
    {M::distance, 5000, G::exploration}, // ACH_DISTANCE_5000
    {M::depth, 20, G::exploration}, // ACH_DEPTH_20
    {M::depth, 40, G::exploration}, // ACH_DEPTH_40
    {M::days, 1, G::survival}, // ACH_DAYS_1
    {M::days, 7, G::survival}, // ACH_DAYS_7
    {M::days, 28, G::survival}, // ACH_DAYS_28
    {M::days, 100, G::survival}, // ACH_DAYS_100
    {M::days, 196, G::survival}, // ACH_DAYS_196
    {M::days, 365, G::survival}, // ACH_DAYS_365
    {M::crafted, 1, G::crafting}, // ACH_CRAFTED_1
    {M::crafted, 100, G::crafting}, // ACH_CRAFTED_100
    {M::crafted, 1000, G::crafting}, // ACH_CRAFTED_1000
    {M::kills, 1, G::combat}, // ACH_TOTAL_KILLS_1
    {M::kills, 100, G::combat}, // ACH_TOTAL_KILLS_100
    {M::kills, 1000, G::combat}, // ACH_TOTAL_KILLS_1000
    {M::undead_dragon, 1, G::combat}, // ACH_UNDEAD_DRAGON_KILLED
    {M::alien, 1, G::exploration}, // ACH_ALIEN_ENCOUNTER
    {M::laser_kills, 1, G::combat}, // ACH_LASER_KILLS
    {M::tnt_crafted, 1, G::crafting}, // ACH_CRAFT_TNT
    {M::guided_dragon, 1, G::combat}, // ACH_GUIDED_MISSILE_KILL
    {M::tnt_kills, 1, G::combat}, // ACH_TNT_KILL
    {M::grenade_kills, 1, G::combat}, // ACH_GRENADE_KILL
}};
static_assert(std::size(retail_achievements) == achievement_count);
constexpr std::array<AchievementDefinition, achievement_count> definitions = [] {
    std::array<AchievementDefinition, achievement_count> result{};
    for (std::size_t index = 0; index < achievement_count; ++index)
        result[index] = {retail_achievements[index].id, retail_achievements[index].name,
                         retail_achievements[index].how_to_unlock, rules[index].metric,
                         rules[index].target, rules[index].group};
    return result;
}();

constexpr double limit = 1.0e12;
bool valid(double n) { return std::isfinite(n) && n >= 0 && n <= limit; }
void increment(double &n, double by = 1) { if (valid(by)) n = std::min(limit, n + by); }
std::uint64_t checksum(std::string_view text)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
    return hash;
}
std::string read_file(const std::filesystem::path &path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 65536) return {};
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
bool write_text(const std::filesystem::path &path, const std::string &text)
{
    return write_file_atomically(path, {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}
} // namespace

std::span<const AchievementDefinition, achievement_count> achievement_definitions() { return definitions; }
double PlayerProgress::value(ProgressMetric metric) const noexcept { return counters[static_cast<std::size_t>(metric)]; }
void PlayerProgress::add(ProgressMetric metric, double amount) noexcept { increment(counters[static_cast<std::size_t>(metric)], amount); }

void PlayerProgress::observe(GameMode mode, double seconds, float x, float y, float z,
                             float previous_day, float day, InventoryItemId held) noexcept
{
    if (!valid(seconds) || seconds > 1 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    add(M::play_seconds, seconds);
    if (valid_inventory_item_id(held)) increment(items[static_cast<std::size_t>(held)].held_seconds, seconds);
    if (mode != GameMode::endurance) return;
    counters[static_cast<std::size_t>(M::distance)] = std::max(value(M::distance), std::min(limit, std::hypot(double(x), double(z))));
    counters[static_cast<std::size_t>(M::depth)] = std::max(value(M::depth), std::clamp(-double(y), 0.0, 64.0));
    if (std::isfinite(day) && std::isfinite(previous_day) && day >= previous_day && day - previous_day < 0.1F)
    {
        // The first morning belongs to the first day, not a survived night.
        const auto mornings = [](float d) { return std::max(0.0, std::floor(double(d) - 0.4)); };
        add(M::days, mornings(day) - mornings(previous_day));
    }
}

void PlayerProgress::record_craft(GameMode mode, InventoryItemId item, std::uint32_t batches) noexcept
{
    if (!valid_inventory_item_id(item)) return;
    increment(items[static_cast<std::size_t>(item)].crafted, batches);
    add(M::all_crafted, batches);
    if (mode == GameMode::endurance) add(M::crafted, batches);
    if (item == InventoryItemId::tnt) add(M::tnt_crafted, batches);
}

void PlayerProgress::record_enemy_kill(GameMode mode, int type, InventoryItemId weapon) noexcept
{
    if (type < 0 || type >= int(enemy_kills.size()) || !valid_inventory_item_id(weapon)) return;
    increment(enemy_kills[static_cast<std::size_t>(type)]);
    increment(items[static_cast<std::size_t>(weapon)].kills);
    add(M::all_kills);
    if (mode != GameMode::endurance) return;
    add(M::kills);
    if (is_laser_gun(weapon)) add(M::laser_kills);
    if (weapon == InventoryItemId::tnt) add(M::tnt_kills);
    if (weapon == InventoryItemId::grenade) add(M::grenade_kills);
}

void PlayerProgress::record_dragon_kill(GameMode mode, std::size_t kind, bool local_killer,
                                       InventoryItemId weapon) noexcept
{
    if (kind >= dragon_kills.size()) return;
    if (mode == GameMode::endurance)
    {
        increment(dragon_kills[kind]);
        if (kind == 4) add(M::undead_dragon);
    }
    if (!local_killer || !valid_inventory_item_id(weapon)) return;
    increment(items[static_cast<std::size_t>(weapon)].kills);
    add(M::all_kills);
    if (mode == GameMode::endurance)
    {
        add(M::kills);
        if (weapon == InventoryItemId::guided_rocket_launcher) add(M::guided_dragon);
    }
}

std::vector<std::size_t> PlayerProgress::unlock_earned(std::int64_t now)
{
    std::vector<std::size_t> earned;
    for (std::size_t i = 0; i < definitions.size(); ++i)
        if (!unlocked_at[i] && value(definitions[i].metric) >= definitions[i].target)
        {
            unlocked_at[i] = std::max<std::int64_t>(1, now);
            earned.push_back(i);
        }
    return earned;
}
std::size_t PlayerProgress::unlocked_count() const noexcept
{
    return std::count_if(unlocked_at.begin(), unlocked_at.end(), [](auto at) { return at > 0; });
}

std::string serialize_player_progress(const PlayerProgress &p)
{
    std::ostringstream body;
    body.imbue(std::locale::classic());
    body << std::setprecision(17);
    for (double n : p.counters) body << n << ' ';
    body << '\n';
    for (auto n : p.unlocked_at) body << n << ' ';
    body << '\n';
    for (double n : p.enemy_kills) body << n << ' ';
    body << '\n';
    for (double n : p.dragon_kills) body << n << ' ';
    body << '\n';
    for (const auto &item : p.items)
        body << item.held_seconds << ' ' << item.crafted << ' ' << item.used << ' ' << item.hits << ' ' << item.kills << '\n';
    const auto payload = body.str();
    return "OMZ_PROGRESS 1\n" + std::to_string(checksum(payload)) + '\n' + payload;
}

std::optional<PlayerProgress> parse_player_progress(std::string_view text)
{
    if (text.size() > 65536 || !text.starts_with("OMZ_PROGRESS 1\n")) return std::nullopt;
    const auto end = text.find('\n', 15);
    if (end == std::string_view::npos) return std::nullopt;
    std::istringstream header(std::string{text.substr(15, end - 15)});
    std::uint64_t expected{};
    if (!(header >> expected) || expected != checksum(text.substr(end + 1))) return std::nullopt;
    std::istringstream body(std::string{text.substr(end + 1)});
    body.imbue(std::locale::classic());
    PlayerProgress p;
    const auto read = [&](double &n) { return bool(body >> n) && valid(n); };
    for (auto &n : p.counters) if (!read(n)) return std::nullopt;
    for (auto &n : p.unlocked_at) if (!(body >> n) || n < 0 || n > 32503680000LL) return std::nullopt;
    for (auto &n : p.enemy_kills) if (!read(n)) return std::nullopt;
    for (auto &n : p.dragon_kills) if (!read(n)) return std::nullopt;
    for (auto &item : p.items)
        if (!read(item.held_seconds) || !read(item.crafted) || !read(item.used) || !read(item.hits) || !read(item.kills)) return std::nullopt;
    body >> std::ws;
    if (!body.eof()) return std::nullopt;
    return p;
}

PlayerProgress load_player_progress(const std::filesystem::path &path)
{
    if (auto p = parse_player_progress(read_file(path))) return *p;
    auto backup = path; backup += ".bak";
    return parse_player_progress(read_file(backup)).value_or(PlayerProgress{});
}

bool save_player_progress(const std::filesystem::path &path, const PlayerProgress &progress)
{
    const auto text = serialize_player_progress(progress);
    if (!parse_player_progress(text)) return false;
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto previous = read_file(path);
    if (parse_player_progress(previous))
    {
        auto backup = path; backup += ".bak";
        if (!write_text(backup, previous)) return false;
    }
    return write_text(path, text);
}
} // namespace openminerz
