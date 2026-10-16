#include "openminerz/net/session_authority.hpp"

#include "openminerz/modding/net.hpp"
#include "openminerz/net/authoritative_bootstrap.hpp"
#include "openminerz/net/bootstrap_codec.hpp"
#include "openminerz/net/dragon_codec.hpp"
#include "openminerz/net/enemy_codec.hpp"
#include "openminerz/net/pickup_codec.hpp"
#include "openminerz/world/block.hpp"
#include "openminerz/world/enemy.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace openminerz::net
{
namespace
{

constexpr double never = -1.0e12;

// Health regenerates (InGameHUD.HealthRecoverRate) 0.75 of maximum per second
// once three seconds passed without damage, as on every stock peer.
constexpr double regeneration_delay_seconds = 3.0;
constexpr float regeneration_per_second = 75.0F;
// InGameHUD: the death prompt (and the respawn it allows) comes three seconds
// after death; online Endurance waits twenty more.
constexpr double respawn_prompt_seconds = 3.0;
constexpr double endurance_respawn_wait_seconds = 20.0;
// Player.ResolveCollsion: damage (dv - T) / 15 of health above T = 15 (1.6.3.8)
// or 18 (1.9.9.8); g = 20.
constexpr float fall_gravity = 20.0F;
// Retail per-hit amounts (BaseTryAttack HitDamages, Tracer for arrows), in
// health points: the largest single swing is the alien's 0.8.
constexpr float largest_enemy_hit = 80.0F;
constexpr float arrow_hit = 35.0F;
// What one report may carry at most: several hits landing in one tick, times
// a server mod's enemy-damage scaling. Beyond it a report is nonsense.
constexpr float largest_report = 1000.0F;

// Violation weights: what one refusal adds to a player's score.
constexpr float weight_minor = 1.0F;
constexpr float weight_speed = 1.5F;
constexpr float weight_cheat = 2.0F;
constexpr float weight_blatant = 3.0F;

[[nodiscard]] bool finite(world::Vec3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] float distance(world::Vec3 a, world::Vec3 b) noexcept
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] float horizontal_distance(world::Vec3 a, world::Vec3 b) noexcept
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

[[nodiscard]] world::Vec3 eye_of(world::Vec3 feet) noexcept
{
    return {feet.x, feet.y + world::PlayerSimulation::eye_height, feet.z};
}

[[nodiscard]] world::Vec3 cell_center(IntVector3 cell) noexcept
{
    return {static_cast<float>(cell.x) + 0.5F, static_cast<float>(cell.y) + 0.5F,
            static_cast<float>(cell.z) + 0.5F};
}

// The slab test: the fraction of `delta` from `origin` where the segment
// enters the box, or nullopt when it misses.
[[nodiscard]] std::optional<float> segment_box_time(world::Vec3 origin, world::Vec3 delta,
                                                    world::Vec3 minimum,
                                                    world::Vec3 maximum) noexcept
{
    float in_time = 0.0F;
    float out_time = 1.0F;
    const auto clip = [&](float start, float movement, float low, float high) {
        if (std::abs(movement) <= 0.000001F)
            return start >= low && start <= high;
        float first = (low - start) / movement;
        float second = (high - start) / movement;
        if (first > second)
            std::swap(first, second);
        in_time = std::max(in_time, first);
        out_time = std::min(out_time, second);
        return in_time <= out_time;
    };
    if (!clip(origin.x, delta.x, minimum.x, maximum.x) ||
        !clip(origin.y, delta.y, minimum.y, maximum.y) ||
        !clip(origin.z, delta.z, minimum.z, maximum.z))
        return std::nullopt;
    return in_time;
}

[[nodiscard]] modding::Vec3 to_value(world::Vec3 value) noexcept
{
    return {value.x, value.y, value.z};
}

[[nodiscard]] std::optional<world::Vec3> vec3_field(const modding::Value &payload,
                                                    std::string_view key)
{
    const modding::Value *field = payload.field(key);
    if (field == nullptr || field->type() != modding::ValueType::vec3)
        return std::nullopt;
    const modding::Vec3 value = field->as_vec3();
    const world::Vec3 result{static_cast<float>(value.x), static_cast<float>(value.y),
                             static_cast<float>(value.z)};
    if (!finite(result) || std::abs(result.x) > 1.0e7F || std::abs(result.y) > 1.0e7F ||
        std::abs(result.z) > 1.0e7F)
        return std::nullopt;
    return result;
}

[[nodiscard]] std::optional<double> number_field(const modding::Value &payload,
                                                 std::string_view key)
{
    const modding::Value *field = payload.field(key);
    if (field == nullptr || !field->is_number())
        return std::nullopt;
    const double value = field->as_number();
    if (!std::isfinite(value))
        return std::nullopt;
    return value;
}

} // namespace

bool session_authoritative(const ModManifest &manifest) noexcept
{
    return requires_extension(manifest);
}

const char *harm_cause_name(HarmCause cause) noexcept
{
    switch (cause)
    {
    case HarmCause::unknown:
        return "unknown";
    case HarmCause::enemy:
        return "enemy";
    case HarmCause::arrow:
        return "arrow";
    case HarmCause::fall:
        return "fall";
    case HarmCause::lava:
        return "lava";
    case HarmCause::player_melee:
        return "melee";
    case HarmCause::player_gun:
        return "gunfire";
    case HarmCause::explosion:
        return "explosion";
    case HarmCause::fireball:
        return "fireball";
    case HarmCause::mod:
        return "mod";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The host's records
// ---------------------------------------------------------------------------

class AuthorityHost::Impl final
{
  public:
    struct Sample final
    {
        double at{};
        world::Vec3 position{};
    };

    // One swing (or arrow) that hit: what the host's own copy of the enemy
    // saw, or what the victim reported.
    struct Hit final
    {
        double at{};
        std::optional<std::uint32_t> enemy;
        float amount{};
        HarmCause cause{HarmCause::enemy};
        bool matched{};
    };

    // Something a player did that may leave pickups behind it: a block it
    // broke, an enemy it killed, a crate it opened, an explosion it set off.
    enum class SourceKind : std::uint8_t
    {
        dig,
        kill,
        crate,
        blast,
        // A 1.9.9.8 loot block broken or a spawner's wave beaten: its reward
        // is the game's to roll, any item.
        loot
    };
    struct Source final
    {
        double at{};
        SourceKind kind{SourceKind::dig};
        world::Vec3 position{};
        world::BlockType block{world::BlockType::empty};
        std::vector<InventoryStack> contents;
    };

    struct Pickup final
    {
        InventoryStack stack;
        world::Vec3 position{};
        bool granted{};
    };

    struct TeleportAllowance final
    {
        TeleportKind kind{TeleportKind::start};
        std::optional<world::Vec3> target;
        double until{};
    };

    struct FallCheck final
    {
        float expected{};
        double landed_at{};
        double deadline{};
    };

    struct Player final
    {
        std::uint8_t gid{};
        bool local{};

        float health{100.0F};
        bool alive{true};
        double died_at{never};
        double last_harm_at{never};
        HarmCause last_cause{HarmCause::unknown};
        std::optional<world::Vec3> last_source;
        bool health_dirty{};
        bool health_urgent{};
        double health_sent_at{never};
        double alive_claimed_since{never};
        bool death_ignored{};

        bool has_position{};
        world::Vec3 accepted{};
        world::Vec3 grounded{};
        std::deque<Sample> history;
        Sample anchor{};
        bool airborne{};
        double air_since{};
        float air_peak{};
        double solid_since{never};
        bool correcting{};
        world::Vec3 fix_to{};
        double fix_at{};
        int fixes_ignored{};
        std::optional<TeleportAllowance> teleport;
        // A teleport-sized jump with no notice yet: the notice travels on
        // the mod channel, the move on the stock one, and they may cross.
        std::optional<Sample> pending_jump;
        float lava_time{world::lava_damage_period_seconds};

        std::optional<FallCheck> fall;
        std::deque<std::pair<double, float>> fall_reports;

        std::deque<Hit> sightings;
        std::deque<Hit> reports;
        double enforce_until{never};

        InventoryItemId carried{InventoryItemId::bare_hands};
        InventoryItemId previous_carried{InventoryItemId::bare_hands};
        bool carried_known{};
        double carried_changed_at{never};
        float shot_tokens{};
        double shot_tokens_at{never};
        std::unordered_map<std::uint16_t, std::uint32_t> rounds_since_reload;
        bool reloading{};
        float swing_tokens{};
        double swing_tokens_at{never};
        float place_tokens{};
        double place_tokens_at{never};
        float dig_seconds{};
        double dig_seconds_at{never};
        float removal_tokens{};
        double removal_tokens_at{never};
        std::deque<double> launches;
        // A laser gun's bolt breaks the cell it strikes, anywhere along its
        // flight (FindBlocksToRemove with a zero-range Laser): no detonation
        // message comes first.
        double laser_shot_at{never};

        std::deque<Source> sources;
        // What the player can have: its last stored inventory plus what the
        // host gave it since, minus what it placed and dropped.
        std::unordered_map<std::uint16_t, std::int64_t> holdings;
        bool holdings_known{};
        // Given lately: an inventory the client wrote before it heard of
        // these may still be on its way.
        std::deque<std::pair<double, InventoryStack>> recent_grants;
        std::uint32_t death_drops{};

        float score{};
        double score_at{};
        std::uint32_t violations{};
        std::uint32_t corrections{};
        bool kicked{};
    };

    Impl(AuthorityRules rules, AuthorityTuning tuning) : rules{rules}, tuning{tuning}
    {
        for (const CraftingRecipe &recipe : crafting_recipes(profile()))
            recipes_by_result[static_cast<std::uint16_t>(recipe.result)].push_back(&recipe);
    }

    AuthorityRules rules;
    AuthorityTuning tuning;
    const AuthorityWorld *world{};
    std::function<bool(std::uint8_t)> friend_of_host;
    std::function<float(std::uint8_t, float, HarmCause, world::Vec3)> damage_filter;
    std::function<bool(InventoryItemId)> craftable;
    std::function<float(std::uint8_t)> speed_scale;
    // (Items) set_melee_weapon.
    std::function<std::optional<AuthorityHost::MeleeWeapon>(std::uint8_t, InventoryItemId)> melee_weapon;
    // The retail cookbook of the session's generation, by what it makes.
    std::unordered_map<std::uint16_t, std::vector<const CraftingRecipe *>> recipes_by_result;
    std::map<std::uint8_t, Player> players;
    std::map<std::pair<std::uint8_t, std::uint32_t>, Pickup> pickups;
    std::vector<AuthorityEvent> events;
    double last_update{never};

    [[nodiscard]] Player *find(std::uint8_t gid) noexcept
    {
        const auto found = players.find(gid);
        return found == players.end() ? nullptr : &found->second;
    }
    [[nodiscard]] const Player *find(std::uint8_t gid) const noexcept
    {
        const auto found = players.find(gid);
        return found == players.end() ? nullptr : &found->second;
    }
    // A player the host corrects: not its own.
    [[nodiscard]] Player *remote(std::uint8_t gid) noexcept
    {
        Player *player = find(gid);
        return player != nullptr && !player->local ? player : nullptr;
    }

    [[nodiscard]] bool fly_allowed() const noexcept
    {
        return rules.mode == GameMode::creative || rules.infinite_resources;
    }
    [[nodiscard]] bool unrestricted() const noexcept
    {
        return has_unrestricted_resources(rules.mode, rules.infinite_resources);
    }
    // Creative and infinite resources: tools, digging time and held blocks are
    // not this host's to count.
    [[nodiscard]] bool lenient_building() const noexcept
    {
        return rules.mode == GameMode::creative || unrestricted();
    }
    [[nodiscard]] InventoryProfile profile() const noexcept
    {
        return rules.protocol == ProtocolVersion::modern_v4 ? InventoryProfile::modern
                                                            : InventoryProfile::classic;
    }
    [[nodiscard]] float fall_threshold() const noexcept
    {
        return rules.protocol == ProtocolVersion::modern_v4 ? 18.0F : 15.0F;
    }

    // --- score -----------------------------------------------------------------

    void decay(Player &player, double now)
    {
        const double elapsed = std::max(0.0, now - player.score_at);
        player.score =
            std::max(0.0F, player.score - static_cast<float>(elapsed) * tuning.score_decay_per_second);
        player.score_at = now;
    }

    void violation(Player &player, float weight, std::string reason, double now)
    {
        decay(player, now);
        player.score += weight;
        ++player.violations;
        AuthorityEvent event;
        event.kind = AuthorityEventKind::violation;
        event.gid = player.gid;
        event.value = player.score;
        event.reason = reason;
        events.push_back(std::move(event));
        if (rules.strict && !player.kicked && player.score >= tuning.kick_score)
        {
            player.kicked = true;
            AuthorityEvent kick;
            kick.kind = AuthorityEventKind::kick;
            kick.gid = player.gid;
            kick.value = player.score;
            kick.reason = std::move(reason);
            events.push_back(std::move(kick));
        }
    }

    // --- health ----------------------------------------------------------------

    void mark_health(Player &player, bool urgent)
    {
        player.health_dirty = true;
        player.health_urgent = player.health_urgent || urgent;
    }

    float harm(Player &player, float amount, HarmCause cause, world::Vec3 source, double now,
               std::optional<std::uint8_t> attacker)
    {
        if (player.local || !player.alive || !(amount > 0.0F) || !std::isfinite(amount))
            return 0.0F;
        if (damage_filter)
        {
            amount = damage_filter(player.gid, amount, cause, source);
            if (!(amount > 0.0F) || !std::isfinite(amount))
                return 0.0F;
            amount = std::min(amount, 10'000.0F);
        }
        player.health -= amount;
        player.last_harm_at = now;
        player.last_cause = cause;
        player.last_source = finite(source) ? std::optional<world::Vec3>{source} : std::nullopt;
        mark_health(player, true);
        if (player.health <= 0.0F)
            die(player, cause, source, now, attacker);
        return amount;
    }

    void die(Player &player, HarmCause cause, world::Vec3 source, double now,
             std::optional<std::uint8_t> attacker)
    {
        player.health = 0.0F;
        player.alive = false;
        player.died_at = now;
        player.alive_claimed_since = never;
        player.death_ignored = false;
        player.death_drops = 0U;
        player.fall.reset();
        player.airborne = false;
        player.correcting = false;
        mark_health(player, true);
        AuthorityEvent event;
        event.kind = AuthorityEventKind::died;
        event.gid = player.gid;
        event.cause = cause;
        event.alive = false;
        event.position = source;
        event.other = attacker;
        events.push_back(std::move(event));
    }

    void revive(Player &player, double now)
    {
        player.alive = true;
        player.health = 100.0F;
        player.last_harm_at = never;
        player.lava_time = world::lava_damage_period_seconds;
        player.airborne = false;
        player.fall.reset();
        player.fall_reports.clear();
        player.sightings.clear();
        player.reports.clear();
        player.solid_since = never;
        player.correcting = false;
        player.teleport = TeleportAllowance{TeleportKind::respawn, std::nullopt,
                                            now + tuning.teleport_window_seconds};
        // Hardcore hands out a fresh loadout (KillPlayer resets the
        // inventory): the next stored inventory is believed again.
        if (rules.difficulty == GameDifficulty::hardcore)
            player.holdings_known = false;
        mark_health(player, true);
        AuthorityEvent event;
        event.kind = AuthorityEventKind::respawned;
        event.gid = player.gid;
        event.value = player.health;
        events.push_back(std::move(event));
    }

    // --- movement --------------------------------------------------------------

    void rebase(Player &player, world::Vec3 position, double now)
    {
        player.has_position = true;
        player.accepted = position;
        player.grounded = position;
        player.airborne = false;
        player.solid_since = never;
        player.anchor = {now, position};
        remember(player, position, now);
    }

    // Movement is measured from an anchor one to two seconds old, not update
    // to update: unreliable updates may arrive out of order and in bursts,
    // which a step-by-step measure would add up as extra distance.
    void advance_anchor(Player &player, double now)
    {
        if (now - player.anchor.at <= 2.0)
            return;
        for (const Sample &sample : player.history)
            if (sample.at >= now - 1.0)
            {
                player.anchor = sample;
                return;
            }
    }

    [[nodiscard]] float rise_limit() const noexcept
    {
        return fly_allowed() ? std::max(tuning.rise_speed_limit, 20.0F) : tuning.rise_speed_limit;
    }

    void remember(Player &player, world::Vec3 position, double now)
    {
        player.history.push_back({now, position});
        while (player.history.size() > 2U &&
               player.history.front().at < now - tuning.position_history_seconds)
            player.history.pop_front();
        while (player.history.size() > 256U)
            player.history.pop_front();
    }

    void correct(Player &player, world::Vec3 to, std::string reason, double now)
    {
        player.correcting = true;
        player.fix_to = to;
        player.fix_at = now;
        player.fixes_ignored = 0;
        player.airborne = false;
        player.solid_since = never;
        ++player.corrections;
        AuthorityEvent event;
        event.kind = AuthorityEventKind::correction;
        event.gid = player.gid;
        event.position = to;
        event.reason = std::move(reason);
        events.push_back(std::move(event));
    }

    // Whether anything that stops a player is under the feet (the footprint,
    // a little below). Where the host cannot tell, it believes the player.
    // (mod-liquid) Whether the feet or the body's middle are in a mod liquid.
    [[nodiscard]] bool in_liquid(world::Vec3 feet) const
    {
        if (world == nullptr)
            return false;
        for (const float up : {0.05F, 0.9F})
            if (world->liquid(static_cast<int>(std::floor(feet.x)),
                              static_cast<int>(std::floor(feet.y + up)),
                              static_cast<int>(std::floor(feet.z))))
                return true;
        return false;
    }

    [[nodiscard]] bool supported(world::Vec3 feet) const
    {
        if (world == nullptr)
            return true;
        // (mod-liquid) A swimmer needs no ground.
        if (in_liquid(feet))
            return true;
        constexpr float reach = world::PlayerSimulation::body_half_width + 0.1F;
        for (const float depth : {0.05F, 0.7F})
            for (const float dx : {-reach, reach})
                for (const float dz : {-reach, reach})
                {
                    const auto block =
                        world->block(static_cast<int>(std::floor(feet.x + dx)),
                                     static_cast<int>(std::floor(feet.y - depth)),
                                     static_cast<int>(std::floor(feet.z + dz)));
                    if (!block.has_value() || world::blocks_player(*block))
                        return true;
                }
        return false;
    }

    [[nodiscard]] bool solid_at(world::Vec3 point) const
    {
        if (world == nullptr)
            return false;
        const int x = static_cast<int>(std::floor(point.x));
        const int y = static_cast<int>(std::floor(point.y));
        const int z = static_cast<int>(std::floor(point.z));
        const auto block = world->block(x, y, z);
        // (mod-liquid) A liquid stops nobody.
        return block.has_value() && world::blocks_player(*block) && !world->liquid(x, y, z);
    }

    // Feet and head both inside blocks that stop a player.
    [[nodiscard]] bool inside_solid(world::Vec3 feet) const
    {
        return solid_at({feet.x, feet.y + 0.25F, feet.z}) &&
               solid_at({feet.x, feet.y + 1.45F, feet.z});
    }

    // Where the player was over the last `window` seconds as the host heard it.
    template <typename Visit> void positions(const Player &player, double now, double window,
                                             Visit &&visit) const
    {
        bool any{};
        for (const Sample &sample : player.history)
            if (sample.at >= now - window)
            {
                visit(sample.position);
                any = true;
            }
        if (!any && player.has_position)
            visit(player.accepted);
    }

    [[nodiscard]] float nearest(const Player &player, world::Vec3 point, double now,
                                double window) const
    {
        float best = std::numeric_limits<float>::max();
        positions(player, now, window, [&](world::Vec3 position) {
            best = std::min(best, distance(position, point));
        });
        return best;
    }

    // --- rates ----------------------------------------------------------------

    // What may arrive at once at `rate` a second: a lag spike holds back a
    // second of honest actions and delivers them together.
    [[nodiscard]] float burst_for(float rate) const noexcept
    {
        return std::max(tuning.burst_actions, rate * tuning.rate_burst_seconds);
    }

    // A token bucket: `rate` actions a second, `burst` at once.
    [[nodiscard]] static bool take(float &tokens, double &at, double now, float rate, float burst)
    {
        if (at <= never)
            tokens = burst;
        else
            tokens = std::min(burst, tokens + static_cast<float>(std::max(0.0, now - at)) * rate);
        at = now;
        if (tokens < 1.0F)
            return false;
        tokens -= 1.0F;
        return true;
    }

    [[nodiscard]] bool holds(const Player &player, InventoryItemId item, double now) const
    {
        if (!player.carried_known)
            return true;
        if (player.carried == item)
            return true;
        // ChangeCarriedItem and the action travel on different delivery
        // classes; the switch may overtake the action it followed.
        return player.previous_carried == item && now - player.carried_changed_at <= 1.0;
    }

    // --- sources of pickups -------------------------------------------------------

    void add_source(Player &player, Source source)
    {
        player.sources.push_back(std::move(source));
        while (player.sources.size() > 512U)
            player.sources.pop_front();
    }

    void prune_sources(Player &player, double now)
    {
        while (!player.sources.empty() && player.sources.front().at < now - 30.0)
            player.sources.pop_front();
    }

    void hold(Player &player, const InventoryStack &stack, std::int64_t sign)
    {
        if (stack.count == 0U)
            return;
        std::int64_t &count = player.holdings[static_cast<std::uint16_t>(stack.item)];
        count = std::max<std::int64_t>(0, count + sign * static_cast<std::int64_t>(stack.count));
    }

    [[nodiscard]] std::int64_t held(const Player &player, std::uint16_t item) const
    {
        const auto found = player.holdings.find(item);
        return found != player.holdings.end() ? found->second : 0;
    }

    // How many of `item` the player could have: what it holds, plus what the
    // cookbook makes from what it holds, a few steps deep (a torch from a
    // stick from a plank from a log). Crafting is local and unannounced, so
    // this is the most the host can say; shared ingredients are counted for
    // every recipe, which only errs on the player's side.
    [[nodiscard]] std::int64_t potential(const Player &player, std::uint16_t item, int depth) const
    {
        const std::int64_t have = held(player, item);
        if (craftable && craftable(static_cast<InventoryItemId>(item)))
            return std::numeric_limits<std::int32_t>::max();
        const auto found = recipes_by_result.find(item);
        if (depth <= 0 || found == recipes_by_result.end())
            return have;
        std::int64_t best{};
        for (const CraftingRecipe *recipe : found->second)
        {
            std::int64_t times = std::numeric_limits<std::int64_t>::max();
            for (const CraftingIngredient &ingredient : recipe->ingredients)
                if (ingredient.count > 0U && ingredient.item != InventoryItemId::bare_hands)
                    times = std::min(times, potential(player, static_cast<std::uint16_t>(ingredient.item),
                                                      depth - 1) /
                                                static_cast<std::int64_t>(ingredient.count));
            if (times == std::numeric_limits<std::int64_t>::max())
                continue;
            best = std::max(best, std::min<std::int64_t>(times, 1'000'000) * recipe->result_count);
        }
        return have + best;
    }

    void grant(Player &player, const InventoryStack &stack, double now)
    {
        hold(player, stack, 1);
        player.recent_grants.emplace_back(now, stack);
        while (!player.recent_grants.empty() &&
               (player.recent_grants.front().first < now - 5.0 || player.recent_grants.size() > 256U))
            player.recent_grants.pop_front();
    }

    [[nodiscard]] bool may_have(const Player &player, const InventoryStack &stack) const
    {
        if (!player.holdings_known || unrestricted())
            return true;
        return potential(player, static_cast<std::uint16_t>(stack.item), 3) >= stack.count;
    }

    // --- enemy damage the host saw against what the victim said ---------------------

    static bool match(std::deque<Hit> &list, const Hit &hit, double window)
    {
        for (Hit &other : list)
            if (!other.matched && other.cause == hit.cause &&
                (other.enemy == hit.enemy || !other.enemy.has_value() || !hit.enemy.has_value()) &&
                std::abs(other.at - hit.at) <= window)
            {
                other.matched = true;
                return true;
            }
        return false;
    }

    void settle_sightings(Player &player, double now)
    {
        const double window = tuning.hit_match_seconds;
        while (!player.reports.empty() && player.reports.front().at < now - tuning.deficit_window_seconds)
            player.reports.pop_front();
        float missed{};
        std::size_t seen{};
        std::vector<const Hit *> overdue;
        for (const Hit &hit : player.sightings)
        {
            if (hit.at < now - tuning.deficit_window_seconds)
                continue;
            ++seen;
            if (!hit.matched && hit.at < now - window)
            {
                missed += hit.amount;
                overdue.push_back(&hit);
            }
        }
        const bool enforcing = now < player.enforce_until;
        const std::size_t reported = player.reports.size();
        const bool ignoring = missed >= tuning.deficit_points &&
                              static_cast<float>(reported) < 0.25F * static_cast<float>(seen);
        if ((enforcing || ignoring) && !overdue.empty() && player.alive)
        {
            float charged{};
            for (const Hit *hit : overdue)
                charged += hit->amount;
            if (ignoring && !enforcing)
            {
                violation(player, weight_cheat,
                          std::format("took {:.0f} points of enemy damage without reporting it",
                                      charged),
                          now);
                player.enforce_until = now + 30.0;
            }
            std::erase_if(player.sightings, [&](const Hit &hit) {
                return !hit.matched && hit.at < now - window;
            });
            harm(player, charged, HarmCause::enemy, player.accepted, now, std::nullopt);
        }
        while (!player.sightings.empty() &&
               player.sightings.front().at < now - tuning.deficit_window_seconds)
            player.sightings.pop_front();
    }

    void settle_fall(Player &player, double now)
    {
        if (!player.fall.has_value() || now < player.fall->deadline)
            return;
        const FallCheck check = *player.fall;
        player.fall.reset();
        float reported{};
        for (const auto &[at, amount] : player.fall_reports)
            if (at >= check.landed_at - 2.5 && at <= check.deadline)
                reported += amount;
        if (player.alive && check.expected - reported > tuning.fall_slack_points)
        {
            violation(player, weight_cheat,
                      std::format("left a {:.0f}-point fall unreported", check.expected), now);
            harm(player, check.expected - reported, HarmCause::fall, player.accepted, now,
                 std::nullopt);
        }
    }

    void landed(Player &player, world::Vec3 position, double now)
    {
        // (mod-liquid) A fall into a liquid deals no damage.
        if (fly_allowed() || player.teleport.has_value() || in_liquid(position))
            return;
        const float drop = player.air_peak - position.y;
        // The highest sample may miss the top of the arc: the estimate errs low.
        const float speed = std::sqrt(2.0F * fall_gravity * std::max(0.0F, drop - 0.5F));
        const float expected = (speed - fall_threshold()) * (100.0F / 15.0F);
        if (expected > tuning.fall_slack_points)
            player.fall = FallCheck{expected, now, now + tuning.hit_match_seconds + 1.5};
    }
};

// ---------------------------------------------------------------------------
// AuthorityHost
// ---------------------------------------------------------------------------

AuthorityHost::AuthorityHost(AuthorityRules rules, AuthorityTuning tuning)
    : impl_{std::make_unique<Impl>(rules, tuning)}
{
}

AuthorityHost::~AuthorityHost() = default;

const AuthorityRules &AuthorityHost::rules() const noexcept
{
    return impl_->rules;
}

const AuthorityTuning &AuthorityHost::tuning() const noexcept
{
    return impl_->tuning;
}

void AuthorityHost::set_world(const AuthorityWorld *world) noexcept
{
    impl_->world = world;
}

void AuthorityHost::set_friend_of_host(std::function<bool(std::uint8_t gid)> query)
{
    impl_->friend_of_host = std::move(query);
}

void AuthorityHost::set_damage_filter(
    std::function<float(std::uint8_t gid, float amount, HarmCause cause, world::Vec3 source)> filter)
{
    impl_->damage_filter = std::move(filter);
}

void AuthorityHost::set_craftable(std::function<bool(InventoryItemId item)> query)
{
    impl_->craftable = std::move(query);
}

void AuthorityHost::set_speed_scale(std::function<float(std::uint8_t gid)> query)
{
    impl_->speed_scale = std::move(query);
}

void AuthorityHost::set_melee_weapon(
    std::function<std::optional<MeleeWeapon>(std::uint8_t attacker, InventoryItemId item)> query)
{
    impl_->melee_weapon = std::move(query);
}

void AuthorityHost::add_player(std::uint8_t gid, double now, bool local)
{
    Impl::Player player;
    player.gid = gid;
    player.local = local;
    player.score_at = now;
    impl_->players.insert_or_assign(gid, std::move(player));
}

void AuthorityHost::remove_player(std::uint8_t gid)
{
    impl_->players.erase(gid);
}

bool AuthorityHost::has_player(std::uint8_t gid) const noexcept
{
    return impl_->find(gid) != nullptr;
}

std::optional<AuthorityPlayerView> AuthorityHost::player(std::uint8_t gid) const
{
    const Impl::Player *player = impl_->find(gid);
    if (player == nullptr)
        return std::nullopt;
    AuthorityPlayerView view;
    view.gid = player->gid;
    view.local = player->local;
    view.health = player->health;
    view.alive = player->alive;
    view.score = player->score;
    view.violations = player->violations;
    view.corrections = player->corrections;
    view.kicked = player->kicked;
    if (player->has_position)
        view.position = player->accepted;
    view.accepted = player->accepted;
    return view;
}

std::vector<std::uint8_t> AuthorityHost::players() const
{
    std::vector<std::uint8_t> result;
    for (const auto &[gid, player] : impl_->players)
        result.push_back(gid);
    return result;
}

void AuthorityHost::set_local_state(std::uint8_t gid, float health, bool alive,
                                    world::Vec3 position, double now)
{
    Impl::Player *player = impl_->find(gid);
    if (player == nullptr || !player->local)
        return;
    player->health = health;
    if (player->alive && !alive)
        player->died_at = now;
    player->alive = alive;
    if (finite(position))
    {
        player->has_position = true;
        player->accepted = position;
        impl_->remember(*player, position, now);
    }
}

bool AuthorityHost::may_act(std::uint8_t gid) const noexcept
{
    const Impl::Player *player = impl_->find(gid);
    return player == nullptr || (player->alive && !player->kicked);
}

float AuthorityHost::harm(std::uint8_t gid, float amount, HarmCause cause, world::Vec3 source,
                          double now, std::optional<std::uint8_t> attacker)
{
    Impl::Player *player = impl_->find(gid);
    return player == nullptr ? 0.0F : impl_->harm(*player, amount, cause, source, now, attacker);
}

void AuthorityHost::heal(std::uint8_t gid, float amount, double now)
{
    Impl::Player *player = impl_->remote(gid);
    if (player == nullptr || !player->alive || !(amount > 0.0F) || !std::isfinite(amount))
        return;
    static_cast<void>(now);
    player->health = std::min(100.0F, player->health + amount);
    impl_->mark_health(*player, true);
}

void AuthorityHost::set_health(std::uint8_t gid, float health, double now)
{
    Impl::Player *player = impl_->remote(gid);
    if (player == nullptr || !player->alive || !std::isfinite(health))
        return;
    health = std::clamp(health, 0.0F, 100.0F);
    if (health <= 0.0F)
    {
        impl_->die(*player, HarmCause::mod, player->accepted, now, std::nullopt);
        return;
    }
    player->health = health;
    impl_->mark_health(*player, true);
}

AuthorityVerdict AuthorityHost::report_harm(std::uint8_t gid, const HarmReport &report, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr)
        return AuthorityVerdict::refuse;
    if (!std::isfinite(report.amount) || report.amount <= 0.0F || report.amount > largest_report)
    {
        impl.violation(*player, weight_minor, "sent a malformed damage report", now);
        return AuthorityVerdict::refuse;
    }
    // A report that crossed the host's own decision on the wire.
    if (!player->alive)
        return AuthorityVerdict::refuse;
    const world::Vec3 source = report.source.value_or(player->accepted);
    switch (report.cause)
    {
    case HarmCause::enemy:
    case HarmCause::arrow: {
        const float single = report.cause == HarmCause::arrow ? arrow_hit : largest_enemy_hit;
        float amount = report.amount;
        if (report.enemy.has_value() && impl.world != nullptr)
        {
            // Only the target's peer tests an enemy's swing, so an enemy hits
            // nobody but its target; the host's copy must be within reach of
            // where the host has the player, give or take its lag.
            if (const auto enemy = impl.world->enemy(gid, *report.enemy); enemy.has_value())
            {
                if (player->has_position &&
                    impl.nearest(*player, enemy->position, now, 1.0) >
                        (report.cause == HarmCause::arrow ? 160.0F : impl.tuning.enemy_reach_slack))
                    return AuthorityVerdict::refuse;
            }
        }
        // Several swings may land in one tick and a server mod may scale enemy
        // damage; a report beyond that is cut to it.
        amount = std::min(amount, single * 8.0F);
        Impl::Hit hit{now, report.enemy, amount, report.cause, false};
        hit.matched = Impl::match(player->sightings, hit, impl.tuning.hit_match_seconds);
        player->reports.push_back(hit);
        while (player->reports.size() > 512U)
            player->reports.pop_front();
        impl.harm(*player, amount, report.cause, source, now, std::nullopt);
        return AuthorityVerdict::accept;
    }
    case HarmCause::fall:
        player->fall_reports.emplace_back(now, report.amount);
        while (!player->fall_reports.empty() && player->fall_reports.front().first < now - 10.0)
            player->fall_reports.pop_front();
        impl.harm(*player, report.amount, HarmCause::fall, source, now, std::nullopt);
        return AuthorityVerdict::accept;
    default:
        // Lava, explosions, fireballs, other players and mods are the host's
        // own to see; a client has nothing to report about them.
        return AuthorityVerdict::refuse;
    }
}

AuthorityVerdict AuthorityHost::request_respawn(std::uint8_t gid, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr)
        return AuthorityVerdict::refuse;
    // Every answer to a request is a health message: a refused client hears
    // that it is still dead (or alive) and may ask again.
    impl.mark_health(*player, true);
    if (player->alive)
        return AuthorityVerdict::refuse;
    const double wait =
        respawn_prompt_seconds +
        (impl.rules.mode == GameMode::endurance ? endurance_respawn_wait_seconds : 0.0);
    // The client counts from when the host's word reached it, later than this.
    if (now - player->died_at < wait - 0.25)
    {
        impl.violation(*player, weight_minor, "asked to come back before the respawn wait", now);
        return AuthorityVerdict::refuse;
    }
    impl.revive(*player, now);
    return AuthorityVerdict::accept;
}

void AuthorityHost::observe_enemy_hit(std::uint8_t gid, std::optional<std::uint32_t> enemy,
                                      float amount, world::Vec3 source, double now, HarmCause cause)
{
    static_cast<void>(source);
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr || !player->alive || !(amount > 0.0F) || !std::isfinite(amount))
        return;
    Impl::Hit hit{now, enemy, amount, cause, false};
    hit.matched = Impl::match(player->reports, hit, impl.tuning.hit_match_seconds);
    player->sightings.push_back(hit);
    while (player->sightings.size() > 512U)
        player->sightings.pop_front();
}

AuthorityVerdict AuthorityHost::observe_player_update(std::uint8_t gid,
                                                      const PlayerUpdatePayload &update, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr)
        return AuthorityVerdict::accept;
    const world::Vec3 position{update.local_position.x, update.local_position.y,
                               update.local_position.z};
    if (!finite(position))
        return AuthorityVerdict::refuse;
    const bool says_dead = (update.state_flags & PlayerUpdatePayload::dead_flag) != 0U;
    if (!player->alive)
    {
        // The host's death reaches the client one trip later; a client still
        // claiming life well after that has ignored it. Its updates go on
        // (the app relays them with the Dead bit set).
        if (!says_dead)
        {
            if (player->alive_claimed_since <= never)
                player->alive_claimed_since = now;
            else if (!player->death_ignored && now - player->alive_claimed_since > 3.0)
            {
                player->death_ignored = true;
                impl.violation(*player, weight_cheat, "kept playing after the host said it died",
                               now);
            }
        }
        player->has_position = true;
        player->accepted = position;
        impl.remember(*player, position, now);
        return AuthorityVerdict::accept;
    }
    player->alive_claimed_since = never;
    if (player->correcting)
    {
        if (horizontal_distance(position, player->fix_to) <= 3.0F &&
            std::abs(position.y - player->fix_to.y) <= 4.0F)
        {
            player->correcting = false;
            impl.rebase(*player, position, now);
            return AuthorityVerdict::accept;
        }
        return AuthorityVerdict::refuse;
    }
    if (!player->has_position)
    {
        impl.rebase(*player, position, now);
        return AuthorityVerdict::accept;
    }
    if (player->teleport.has_value())
    {
        const Impl::TeleportAllowance &allowance = *player->teleport;
        if (now > allowance.until)
            player->teleport.reset();
        else
        {
            const bool arrived =
                !allowance.target.has_value() ||
                (allowance.kind == TeleportKind::surface
                     ? horizontal_distance(position, *allowance.target) <= 2.0F
                     : horizontal_distance(position, *allowance.target) <= 6.0F);
            if (arrived && distance(position, player->accepted) > 2.0F)
            {
                player->teleport.reset();
                impl.rebase(*player, position, now);
                return AuthorityVerdict::accept;
            }
        }
    }

    const float moved = horizontal_distance(position, player->accepted);
    if (moved > impl.tuning.teleport_distance)
    {
        // Held, not judged: its notice may still be on the way (update()
        // decides once the grace is over).
        if (!player->pending_jump.has_value())
            player->pending_jump = Impl::Sample{now, position};
        else
            player->pending_jump->position = position;
        return AuthorityVerdict::refuse;
    }
    // From the anchor: the game's top speed over the time since, plus what
    // a lag spike's burst and the anchor's own late arrival may add.
    impl.advance_anchor(*player, now);
    // A client that went quiet earns no more than a few seconds of travel:
    // silence is not a way to climb a tower in one update.
    const float age = static_cast<float>(std::clamp(now - player->anchor.at, 0.0, 3.0)) +
                      impl.tuning.movement_jitter_seconds;
    const float scale =
        impl.speed_scale ? std::clamp(impl.speed_scale(gid), 1.0F, 4.0F) : 1.0F;
    const float speed =
        (impl.fly_allowed() ? impl.tuning.fly_speed_limit : impl.tuning.walk_speed_limit) * scale;
    const float travelled = horizontal_distance(position, player->anchor.position);
    if (travelled > speed * age + impl.tuning.movement_slack_meters)
    {
        impl.violation(*player, weight_speed,
                       std::format("moved faster than the game allows ({:.1f} m in {:.1f} s)",
                                   travelled, age),
                       now);
        impl.correct(*player, player->accepted, "speed", now);
        return AuthorityVerdict::refuse;
    }
    const float rise = position.y - player->anchor.position.y;
    if (rise > impl.rise_limit() * age + impl.tuning.movement_slack_meters)
    {
        // Teleport > Surface is a jump straight up: held for its notice like
        // any other teleport (update() judges it if none comes).
        if (!player->pending_jump.has_value())
            player->pending_jump = Impl::Sample{now, position};
        else
            player->pending_jump->position = position;
        return AuthorityVerdict::refuse;
    }
    // Back within reach of where it was accepted: no jump is pending.
    player->pending_jump.reset();

    if (!impl.fly_allowed())
    {
        if (impl.supported(position))
        {
            if (player->airborne)
                impl.landed(*player, position, now);
            player->airborne = false;
            player->grounded = position;
        }
        else if (!player->airborne)
        {
            player->airborne = true;
            player->air_since = now;
            player->air_peak = std::max(position.y, player->accepted.y);
        }
        else
        {
            // The top of the last climb: a 1.9.9.8 second jump starts a new
            // arc, and the fall that counts is from its top.
            if (position.y > player->accepted.y)
                player->air_peak = position.y;
            else
                player->air_peak = std::max(player->air_peak, position.y);
            if (now - player->air_since > impl.tuning.hover_seconds &&
                player->air_peak - position.y < impl.tuning.hover_drop_meters)
            {
                impl.violation(*player, weight_cheat, "flew outside Creative", now);
                impl.correct(*player, player->grounded, "flying", now);
                return AuthorityVerdict::refuse;
            }
        }
    }
    if (impl.inside_solid(position))
    {
        if (player->solid_since <= never)
            player->solid_since = now;
        else if (now - player->solid_since > impl.tuning.inside_solid_seconds)
        {
            impl.violation(*player, weight_cheat, "moved through solid blocks", now);
            impl.correct(*player, player->grounded, "no-clip", now);
            return AuthorityVerdict::refuse;
        }
    }
    else
        player->solid_since = never;

    // A gun's magazine is refilled by its reload: the Reloading bit.
    const bool reloading = (update.state_flags & PlayerUpdatePayload::reloading_flag) != 0U;
    if (reloading && player->carried_known)
        player->rounds_since_reload.erase(static_cast<std::uint16_t>(player->carried));
    player->reloading = reloading;

    player->accepted = position;
    impl.remember(*player, position, now);
    return AuthorityVerdict::accept;
}

AuthorityVerdict AuthorityHost::observe_teleport_notice(std::uint8_t gid, TeleportKind kind,
                                                        world::Vec3 target, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr || !finite(target))
        return AuthorityVerdict::refuse;
    bool plausible{};
    switch (kind)
    {
    case TeleportKind::surface:
        plausible = !player->has_position || horizontal_distance(target, player->accepted) <= 2.0F;
        break;
    case TeleportKind::start:
        plausible = horizontal_distance(target, impl.rules.spawn_point) <= 24.0F;
        break;
    case TeleportKind::player:
        for (const auto &[other_gid, other] : impl.players)
            if (other_gid != gid && other.alive && other.has_position &&
                impl.nearest(other, target, now, 1.0) <= 12.0F)
                plausible = true;
        break;
    case TeleportKind::locator:
        plausible = impl.holds(*player, InventoryItemId::gps, now) ||
                    impl.holds(*player, InventoryItemId::teleport_gps, now);
        break;
    case TeleportKind::station:
        if (impl.world == nullptr)
            plausible = true;
        else
            for (int dy = -2; dy <= 1 && !plausible; ++dy)
                for (int dx = -1; dx <= 1 && !plausible; ++dx)
                    for (int dz = -1; dz <= 1 && !plausible; ++dz)
                    {
                        const auto block = impl.world->block(
                            static_cast<int>(std::floor(target.x)) + dx,
                            static_cast<int>(std::floor(target.y)) + dy,
                            static_cast<int>(std::floor(target.z)) + dz);
                        plausible = !block.has_value() ||
                                    *block == world::BlockType::teleport_station ||
                                    (*block >= world::BlockType::spawn_point_basic &&
                                     *block <= world::BlockType::spawn_point_explorer);
                    }
        break;
    case TeleportKind::respawn:
        // Granted with the respawn itself.
        return player->teleport.has_value() ? AuthorityVerdict::accept : AuthorityVerdict::refuse;
    }
    if (!plausible)
    {
        impl.violation(*player, weight_cheat, "announced a teleport the game cannot make", now);
        return AuthorityVerdict::refuse;
    }
    player->teleport = Impl::TeleportAllowance{kind, target, now + impl.tuning.teleport_window_seconds};
    // The jump came first: it is the one announced.
    if (player->pending_jump.has_value() &&
        horizontal_distance(player->pending_jump->position, target) <= 6.0F)
    {
        const world::Vec3 arrived = player->pending_jump->position;
        player->pending_jump.reset();
        player->teleport.reset();
        impl.rebase(*player, arrived, now);
    }
    return AuthorityVerdict::accept;
}

void AuthorityHost::allow_teleport(std::uint8_t gid, std::optional<world::Vec3> target, double now)
{
    Impl::Player *player = impl_->remote(gid);
    if (player == nullptr)
        return;
    player->teleport = Impl::TeleportAllowance{TeleportKind::start, target,
                                               now + impl_->tuning.teleport_window_seconds};
    if (player->pending_jump.has_value() &&
        (!target.has_value() || horizontal_distance(player->pending_jump->position, *target) <= 6.0F))
    {
        const world::Vec3 arrived = player->pending_jump->position;
        player->pending_jump.reset();
        player->teleport.reset();
        impl_->rebase(*player, arrived, now);
    }
}

void AuthorityHost::observe_level_restart(double now)
{
    for (auto &[gid, player] : impl_->players)
    {
        if (player.local)
            continue;
        // _processRestartLevelMessage brings every player back alive.
        if (!player.alive)
            impl_->revive(player, now);
        player.correcting = false;
        player.teleport = Impl::TeleportAllowance{TeleportKind::start, std::nullopt,
                                                  now + impl_->tuning.teleport_window_seconds};
    }
}

void AuthorityHost::observe_carried_item(std::uint8_t gid, InventoryItemId item, double now)
{
    Impl::Player *player = impl_->find(gid);
    if (player == nullptr)
        return;
    if (!player->carried_known || player->carried != item)
    {
        player->previous_carried = player->carried;
        player->carried = item;
        player->carried_changed_at = now;
    }
    player->carried_known = true;
}

bool AuthorityHost::pvp_allows(std::uint8_t attacker, std::uint8_t victim) const
{
    if (attacker == victim)
        return false;
    const Impl::Player *target = impl_->find(victim);
    const bool victim_is_host = target != nullptr && target->local;
    // CastleMinerZGame.ProcessMeleePlayerMessage and the tracer's player test:
    // `PVPState == Everyone || (!IsHost && !IsFriend(Host))`, asked by the
    // victim about itself.
    switch (impl_->rules.pvp)
    {
    case PlayerVersusPlayer::everyone:
        return true;
    case PlayerVersusPlayer::not_friends:
        return !victim_is_host && !(impl_->friend_of_host && impl_->friend_of_host(victim));
    case PlayerVersusPlayer::off:
        return false;
    }
    return false;
}

AuthorityVerdict AuthorityHost::observe_melee(std::uint8_t attacker, std::uint8_t victim,
                                              const MeleePlayerPayload &melee, double now)
{
    Impl &impl = *impl_;
    Impl::Player *from = impl.find(attacker);
    Impl::Player *to = impl.find(victim);
    if (from == nullptr || to == nullptr || !to->alive)
        return AuthorityVerdict::refuse;
    const bool judged = !from->local;
    if (!from->alive || from->kicked)
    {
        if (judged && now - from->died_at > 1.0)
            impl.violation(*from, weight_minor, "swung at a player while dead", now);
        return AuthorityVerdict::refuse;
    }
    if (!pvp_allows(attacker, victim))
    {
        if (judged)
            impl.violation(*from, weight_cheat, "attacked a player the PVP rule protects", now);
        return AuthorityVerdict::refuse;
    }
    InventoryItemId item{};
    if (!inventory_item_from_wire(melee.item_id, impl.rules.protocol, item) ||
        !item_is_melee_weapon(item))
    {
        if (judged)
            impl.violation(*from, weight_blatant, "struck with something that is not a weapon", now);
        return AuthorityVerdict::refuse;
    }
    if (judged && !impl.holds(*from, item, now))
    {
        impl.violation(*from, weight_blatant,
                       std::format("struck with item {} it does not hold",
                                   static_cast<int>(item)),
                       now);
        return AuthorityVerdict::refuse;
    }
    // (Items) A server mod's weapon in that hand: its own reach, rate and damage.
    const std::optional<MeleeWeapon> weapon =
        impl.melee_weapon ? impl.melee_weapon(attacker, item) : std::nullopt;
    if (judged)
    {
        // The attacker's probe reaches 2.5 units; both pictures lag.
        float best = std::numeric_limits<float>::max();
        bool clear{};
        if (from->has_position)
            impl.positions(*to, now, impl.tuning.shot_compensation_seconds, [&](world::Vec3 spot) {
                const float gap = distance(eye_of(from->accepted),
                                           {spot.x, spot.y + 1.0F, spot.z});
                best = std::min(best, gap);
                if (impl.world == nullptr ||
                    impl.world->clear_line(eye_of(from->accepted), {spot.x, spot.y + 1.0F, spot.z}))
                    clear = true;
            });
        else
            best = 0.0F, clear = true;
        const float reach = weapon.has_value() ? weapon->reach * 0.5F : impl.tuning.melee_reach;
        if (best > reach + impl.tuning.melee_slack)
        {
            impl.violation(*from, weight_cheat,
                           std::format("struck a player {:.1f} m away", best), now);
            return AuthorityVerdict::refuse;
        }
        if (!clear)
        {
            impl.violation(*from, weight_cheat, "struck a player through a wall", now);
            return AuthorityVerdict::refuse;
        }
        const float cooldown =
            std::max(0.05F, weapon.has_value() ? weapon->cooldown_seconds : item_use_cooldown_seconds(item));
        if (!Impl::take(from->swing_tokens, from->swing_tokens_at, now, 1.05F / cooldown,
                        impl.burst_for(1.05F / cooldown)))
        {
            impl.violation(*from, weight_speed, "swung faster than its weapon allows", now);
            return AuthorityVerdict::refuse;
        }
    }
    if (!to->local)
        impl.harm(*to, weapon.has_value() ? weapon->player_damage : item_player_melee_damage(item) * 100.0F,
                  HarmCause::player_melee,
                  {melee.damage_source.x, melee.damage_source.y, melee.damage_source.z}, now,
                  attacker);
    return AuthorityVerdict::accept;
}

AuthorityVerdict AuthorityHost::observe_gunshot(std::uint8_t shooter, InventoryItemId item,
                                                std::span<const Vector3> directions, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(shooter);
    if (player == nullptr)
        return AuthorityVerdict::accept;
    if (!player->alive || player->kicked)
    {
        if (now - player->died_at > 1.0)
            impl.violation(*player, weight_minor, "fired while dead", now);
        return AuthorityVerdict::refuse;
    }
    const GunStats *stats = gun_stats(item);
    if (stats == nullptr || directions.empty())
    {
        impl.violation(*player, weight_blatant, "fired something that is not a gun", now);
        return AuthorityVerdict::refuse;
    }
    for (const Vector3 &direction : directions)
    {
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                       direction.z * direction.z);
        if (!std::isfinite(length) || length < 0.5F || length > 1.5F)
        {
            impl.violation(*player, weight_cheat, "fired in no direction", now);
            return AuthorityVerdict::refuse;
        }
    }
    if (!impl.holds(*player, item, now))
    {
        impl.violation(*player, weight_blatant,
                       std::format("fired gun {} it does not hold", static_cast<int>(item)), now);
        return AuthorityVerdict::refuse;
    }
    // GunInventoryItem.ProcessInput: one round per CoolDownTime.
    const float cooldown = std::max(0.02F, stats->fire_cooldown_seconds);
    if (!Impl::take(player->shot_tokens, player->shot_tokens_at, now, 1.05F / cooldown,
                    impl.burst_for(1.05F / cooldown)))
    {
        impl.violation(*player, weight_speed, "fired faster than its gun allows", now);
        return AuthorityVerdict::refuse;
    }
    if (is_laser_gun(item))
        player->laser_shot_at = now;
    if (!impl.unrestricted() && stats->clip_capacity > 0U)
    {
        std::uint32_t &fired = player->rounds_since_reload[static_cast<std::uint16_t>(item)];
        ++fired;
        if (fired > static_cast<std::uint32_t>(stats->clip_capacity) + 2U && !player->reloading)
        {
            impl.violation(*player, weight_speed,
                           std::format("fired {} rounds from a {}-round magazine", fired,
                                       stats->clip_capacity),
                           now);
            return AuthorityVerdict::refuse;
        }
    }
    return AuthorityVerdict::accept;
}

AuthorityVerdict AuthorityHost::observe_launch(std::uint8_t shooter, world::Vec3 position,
                                               world::Vec3 direction, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(shooter);
    if (player == nullptr)
        return AuthorityVerdict::accept;
    if (!player->alive || player->kicked || !finite(position) || !finite(direction))
        return AuthorityVerdict::refuse;
    if (player->has_position)
    {
        const float gap = impl.nearest(*player, position, now, impl.tuning.shot_compensation_seconds);
        // The projectile leaves the shooter's camera; the host's picture of
        // the shooter trails by its lag.
        if (gap > 3.0F + impl.tuning.walk_speed_limit * 0.5F)
        {
            impl.violation(*player, weight_cheat,
                           std::format("launched a projectile {:.0f} m from itself", gap), now);
            return AuthorityVerdict::refuse;
        }
    }
    player->launches.push_back(now);
    while (!player->launches.empty() && player->launches.front() < now - 20.0)
        player->launches.pop_front();
    return AuthorityVerdict::accept;
}

std::vector<std::uint8_t> AuthorityHost::apply_explosion(world::Vec3 location,
                                                         world::ExplosiveDamageBands bands,
                                                         std::optional<std::uint8_t> by, double now)
{
    Impl &impl = *impl_;
    std::vector<std::uint8_t> hurt;
    if (!finite(location))
        return hurt;
    for (auto &[gid, player] : impl.players)
    {
        if (player.local || !player.alive || !player.has_position)
            continue;
        // Explosive.ApplySplashDamageToLocalPlayerAndZombies, from the
        // player's position plus one; retail asks no PVP question here.
        // Retail asks no PVP question of an explosion; this client has always
        // spared other players from one while PVP is off (apply_explosion_
        // splash), and the authority keeps the game's rule as it is.
        if (by.has_value() && *by != gid && impl.rules.pvp == PlayerVersusPlayer::off)
            continue;
        const world::Vec3 ear{player.accepted.x, player.accepted.y + 1.0F, player.accepted.z};
        const float gap = distance(ear, location);
        if (gap >= bands.damage_range)
            continue;
        float fraction = world::explosive_damage_fraction(gap, bands);
        if (gap >= bands.kill_range && gap > 0.001F && impl.world != nullptr)
            fraction *= impl.world->blast_line_of_sight(location, ear, world::DragonKind::fire);
        if (impl.harm(player, std::clamp(fraction, 0.0F, 1.0F) * 100.0F, HarmCause::explosion,
                      location, now, by) > 0.0F)
            hurt.push_back(gid);
    }
    return hurt;
}

std::vector<std::uint8_t> AuthorityHost::apply_fireball(world::Vec3 location,
                                                        world::DragonKind kind, double now)
{
    Impl &impl = *impl_;
    std::vector<std::uint8_t> hurt;
    if (!finite(location))
        return hurt;
    const float damage = world::dragon_stats(kind).fireball_damage * 100.0F;
    for (auto &[gid, player] : impl.players)
    {
        if (player.local || !player.alive || !player.has_position)
            continue;
        // HandleDetonateFireballMessage: within five units of the chest, full
        // inside the first, behind the dragon's own line-of-sight probe.
        const world::Vec3 chest{player.accepted.x, player.accepted.y + 1.0F, player.accepted.z};
        const float gap = distance(chest, location);
        if (gap >= 5.0F)
            continue;
        const float sight =
            impl.world != nullptr ? impl.world->blast_line_of_sight(location, chest, kind) : 1.0F;
        const float amount =
            std::min(sight * (1.0F - std::max(gap - 1.0F, 0.0F) / 5.0F), 1.0F) * damage;
        if (impl.harm(player, amount, HarmCause::fireball, location, now, std::nullopt) > 0.0F)
            hurt.push_back(gid);
    }
    return hurt;
}

std::optional<float> AuthorityHost::tracer_hits_player(std::uint8_t victim, world::Vec3 tail,
                                                       world::Vec3 head, double now) const
{
    const Impl::Player *player = impl_->find(victim);
    if (player == nullptr || !player->alive || !player->has_position || !finite(tail) ||
        !finite(head))
        return std::nullopt;
    const world::Vec3 delta{head.x - tail.x, head.y - tail.y, head.z - tail.z};
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (length < 0.0001F)
        return std::nullopt;
    std::optional<float> best;
    constexpr float half = world::PlayerSimulation::body_half_width;
    const auto test = [&](world::Vec3 position) {
        const auto time =
            segment_box_time(tail, delta, {position.x - half, position.y, position.z - half},
                             {position.x + half, position.y + world::PlayerSimulation::body_height,
                              position.z + half});
        if (time.has_value() && (!best.has_value() || *time * length < *best))
            best = *time * length;
    };
    // Favour the shooter: the victim is hit where the shooter could have seen
    // it, anywhere along its path over the shooter's lag.
    const Impl::Sample *previous{};
    for (const Impl::Sample &sample : player->history)
    {
        if (sample.at < now - impl_->tuning.shot_compensation_seconds)
        {
            previous = &sample;
            continue;
        }
        test(sample.position);
        if (previous != nullptr)
            test({(previous->position.x + sample.position.x) * 0.5F,
                  (previous->position.y + sample.position.y) * 0.5F,
                  (previous->position.z + sample.position.z) * 0.5F});
        previous = &sample;
    }
    test(player->accepted);
    return best;
}

AuthorityVerdict AuthorityHost::observe_block_edit(std::uint8_t gid, IntVector3 cell,
                                                   world::BlockType previous,
                                                   world::BlockType block, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr)
        return AuthorityVerdict::accept;
    if (!player->alive || player->kicked)
    {
        if (now - player->died_at > 1.0)
            impl.violation(*player, weight_minor, "changed a block while dead", now);
        return AuthorityVerdict::refuse;
    }
    const world::Vec3 center = cell_center(cell);
    if (impl.rules.spawn_protection_radius > 0 &&
        horizontal_distance(center, impl.rules.spawn_point) <=
            static_cast<float>(impl.rules.spawn_protection_radius))
        return AuthorityVerdict::refuse; // a host rule, not a cheat
    if (player->has_position)
    {
        float best = std::numeric_limits<float>::max();
        impl.positions(*player, now, 0.6, [&](world::Vec3 feet) {
            best = std::min(best, distance(eye_of(feet), center));
        });
        if (best > impl.tuning.reach + impl.tuning.reach_slack)
        {
            impl.violation(*player, weight_cheat,
                           std::format("changed a block {:.1f} m away", best), now);
            return AuthorityVerdict::refuse;
        }
    }
    // (mod-liquid) Building into a liquid is building into an empty cell; a
    // liquid itself is never dug (the selection passes through it).
    const bool into_liquid =
        impl.world != nullptr && impl.world->liquid(cell.x, cell.y, cell.z) && block != previous;
    if (into_liquid && block == world::BlockType::empty)
        return AuthorityVerdict::refuse;
    const bool dig = block == world::BlockType::empty && previous != world::BlockType::empty;
    const bool place = (previous == world::BlockType::empty || into_liquid) &&
                       block != world::BlockType::empty;
    if (dig && !impl.lenient_building() && player->carried_known && world::can_be_dug(previous) &&
        !world::is_spawner(previous) && previous != world::BlockType::loot_block &&
        previous != world::BlockType::lucky_loot_block)
    {
        const float seconds = dig_time_seconds(player->carried, previous, impl.profile());
        const float before =
            dig_time_seconds(player->previous_carried, previous, impl.profile());
        const bool switched = now - player->carried_changed_at <= 1.0;
        if (!std::isfinite(seconds) && !(switched && std::isfinite(before)))
        {
            impl.violation(*player, weight_cheat,
                           std::format("broke block {} with a tool that cannot dig it",
                                       static_cast<int>(previous)),
                           now);
            return AuthorityVerdict::refuse;
        }
        // Digging time: what the player spent digging may be saved up for a
        // couple of seconds beyond this dig, so bunched messages pass.
        const float full = std::isfinite(seconds) ? seconds : before;
        const float needed = full * 0.6F;
        const float saved = std::max(2.5F, full + 1.0F);
        if (player->dig_seconds_at <= never)
            player->dig_seconds = saved;
        else
            player->dig_seconds = std::min(
                saved, player->dig_seconds +
                           static_cast<float>(std::max(0.0, now - player->dig_seconds_at)));
        player->dig_seconds_at = now;
        if (player->dig_seconds < needed)
        {
            impl.violation(*player, weight_speed, "dug faster than its tool allows", now);
            return AuthorityVerdict::refuse;
        }
        player->dig_seconds -= needed;
    }
    if (place)
    {
        if (player->carried_known && !impl.lenient_building())
        {
            const auto matches = [&](InventoryItemId item) {
                return is_block_item(item) &&
                       (item_block(item) == block || block_item(block) == item ||
                        (door_model_name(item).has_value() &&
                         (world::is_lower_door(block) || world::is_upper_door(block))));
            };
            const bool switched = now - player->carried_changed_at <= 1.0;
            if (!matches(player->carried) && !(switched && matches(player->previous_carried)) &&
                !world::is_spawner(block))
            {
                impl.violation(*player, weight_cheat,
                               std::format("placed block {} without holding it",
                                           static_cast<int>(block)),
                               now);
                return AuthorityVerdict::refuse;
            }
            const InventoryStack one{player->carried, 1U};
            if (matches(player->carried) && !impl.may_have(*player, one))
            {
                impl.violation(*player, weight_cheat,
                               std::format("placed block {} it never had", static_cast<int>(block)),
                               now);
                return AuthorityVerdict::refuse;
            }
            if (matches(player->carried))
                impl.hold(*player, one, -1);
        }
        const float cooldown = std::max(0.05F, item_use_cooldown_seconds(player->carried));
        if (!Impl::take(player->place_tokens, player->place_tokens_at, now, 1.1F / cooldown,
                        impl.burst_for(1.1F / cooldown)))
        {
            impl.violation(*player, weight_speed, "built faster than possible", now);
            return AuthorityVerdict::refuse;
        }
    }
    if (dig)
        impl.add_source(*player, {now, Impl::SourceKind::dig, center, previous, {}});
    // A loot block broken, a spawner switched by its wave: the game rolls the
    // reward the player then drops into the world.
    if (previous == world::BlockType::loot_block || previous == world::BlockType::lucky_loot_block ||
        world::is_spawner(previous) || world::is_spawner(block))
        impl.add_source(*player, {now, Impl::SourceKind::loot, center, previous, {}});
    return AuthorityVerdict::accept;
}

AuthorityVerdict AuthorityHost::observe_detonation(std::uint8_t gid, world::Vec3 location,
                                                   double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr)
        return AuthorityVerdict::accept;
    if (!finite(location))
        return AuthorityVerdict::refuse;
    impl.add_source(*player, {now, Impl::SourceKind::blast, location, world::BlockType::empty, {}});
    return AuthorityVerdict::accept;
}

AuthorityVerdict AuthorityHost::observe_block_removal(std::uint8_t gid,
                                                      std::span<const IntVector3> cells, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr || cells.empty())
        return AuthorityVerdict::accept;
    impl.prune_sources(*player, now);
    for (const IntVector3 &cell : cells)
    {
        const world::Vec3 center = cell_center(cell);
        if (impl.rules.spawn_protection_radius > 0 &&
            horizontal_distance(center, impl.rules.spawn_point) <=
                static_cast<float>(impl.rules.spawn_protection_radius))
            return AuthorityVerdict::refuse;
        // Explosive.FindBlocksToRemove: within a few cells of a blast this
        // player set off; or an enemy chasing this player digging next to it.
        bool explained{};
        for (const Impl::Source &source : player->sources)
            if (source.kind == Impl::SourceKind::blast && now - source.at <= 5.0 &&
                distance(source.position, center) <= 8.0F)
                explained = true;
        // A laser bolt's own cell, wherever it struck.
        if (!explained && cells.size() <= 2U && now - player->laser_shot_at <= 4.0)
            explained = true;
        // An enemy chasing this player digging its way (EnemyBreakBlocks on
        // the target's peer): near the player, at a digging pace.
        if (!explained && player->has_position &&
            impl.nearest(*player, center, now, 1.0) <= 24.0F)
            explained = Impl::take(player->removal_tokens, player->removal_tokens_at, now, 6.0F,
                                   24.0F);
        if (!explained)
        {
            impl.violation(*player, weight_cheat, "removed blocks no explosion or enemy explains",
                           now);
            return AuthorityVerdict::refuse;
        }
    }
    for (const IntVector3 &cell : cells)
    {
        const std::optional<world::BlockType> block =
            impl.world != nullptr ? impl.world->block(cell.x, cell.y, cell.z) : std::nullopt;
        impl.add_source(*player, {now, Impl::SourceKind::dig, cell_center(cell),
                                  block.value_or(world::BlockType::empty), {}});
    }
    return AuthorityVerdict::accept;
}

void AuthorityHost::observe_enemy_kill(std::uint8_t gid, std::optional<world::Vec3> position,
                                       double now)
{
    Impl::Player *player = impl_->remote(gid);
    if (player == nullptr)
        return;
    const world::Vec3 at = position.value_or(player->accepted);
    if (finite(at))
        impl_->add_source(*player, {now, Impl::SourceKind::kill, at, world::BlockType::empty, {}});
}

void AuthorityHost::observe_crate_opened(std::uint8_t gid, world::Vec3 position,
                                         std::span<const InventoryStack> contents, double now)
{
    Impl::Player *player = impl_->remote(gid);
    if (player != nullptr && finite(position))
        impl_->add_source(*player, {now, Impl::SourceKind::crate, position, world::BlockType::empty,
                                    std::vector<InventoryStack>(contents.begin(), contents.end())});
}

void AuthorityHost::observe_crate_slot(std::uint8_t gid, const InventoryStack &before,
                                       const InventoryStack &after, double now)
{
    Impl::Player *player = impl_->remote(gid);
    if (player == nullptr)
        return;
    // What left the crate went to the player, what arrived came from it.
    if (before.count > 0U && (after.count == 0U || after.item != before.item))
        impl_->grant(*player, before, now);
    else if (before.item == after.item && before.count > after.count)
        impl_->grant(*player, {before.item, static_cast<std::uint16_t>(before.count - after.count)},
                     now);
    if (after.count > 0U && (before.count == 0U || after.item != before.item))
        impl_->hold(*player, after, -1);
    else if (before.item == after.item && after.count > before.count)
        impl_->hold(*player, {after.item, static_cast<std::uint16_t>(after.count - before.count)}, -1);
}

bool AuthorityHost::may_put_in_crate(std::uint8_t gid, const InventoryStack &before,
                                     const InventoryStack &after) const
{
    const Impl::Player *player = impl_->find(gid);
    if (player == nullptr || player->local || after.count == 0U)
        return true;
    InventoryStack added = after;
    if (before.item == after.item)
    {
        if (after.count <= before.count)
            return true;
        added.count = static_cast<std::uint16_t>(after.count - before.count);
    }
    return impl_->may_have(*player, added);
}

void AuthorityHost::grant_items(std::uint8_t gid, const InventoryStack &stack, double now)
{
    Impl::Player *player = impl_->remote(gid);
    if (player != nullptr)
        impl_->grant(*player, stack, now);
}

AuthorityVerdict AuthorityHost::observe_inventory(std::uint8_t gid,
                                                  std::span<const InventoryStack> stacks, double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr)
        return AuthorityVerdict::accept;
    bool inflated{};
    std::unordered_map<std::uint16_t, std::int64_t> stored;
    for (const InventoryStack &stack : stacks)
        if (stack.count > 0U)
            stored[static_cast<std::uint16_t>(stack.item)] += stack.count;
    if (player->holdings_known && !impl.unrestricted())
    {
        // Crafting is the only thing that makes items without the host: an
        // item may not grow past what the host handed over plus what the
        // cookbook makes of that. (One violation per store, however much.)
        std::string claim;
        for (auto &[item, count] : stored)
        {
            const std::int64_t allowed = impl.potential(*player, item, 3);
            if (count <= allowed)
                continue;
            if (claim.empty())
                claim = std::format("claims {} of item {} the host never gave it", count - allowed,
                                    static_cast<int>(item));
            count = allowed;
        }
        if (!claim.empty())
        {
            impl.violation(*player, weight_blatant, std::move(claim), now);
            inflated = true;
        }
    }
    player->holdings = std::move(stored);
    player->holdings_known = true;
    for (const auto &[at, stack] : player->recent_grants)
        if (at >= now - 3.0)
            impl.hold(*player, stack, 1);
    // Refused: the host keeps the inventory it stored before, so items made
    // from nothing never reach the player's saved profile.
    return inflated ? AuthorityVerdict::refuse : AuthorityVerdict::accept;
}

AuthorityVerdict AuthorityHost::observe_pickup_created(std::uint8_t gid, std::uint32_t pickup_id,
                                                       InventoryStack stack, world::Vec3 position,
                                                       double now)
{
    Impl &impl = *impl_;
    Impl::Player *player = impl.remote(gid);
    if (player == nullptr)
    {
        register_pickup(gid, pickup_id, stack, position);
        return AuthorityVerdict::accept;
    }
    if (stack.count == 0U || !finite(position))
        return AuthorityVerdict::refuse;
    impl.prune_sources(*player, now);
    bool explained{};
    // KillPlayer: everything the player carried falls where it died.
    if (!player->alive && now - player->died_at <= 10.0 && player->death_drops < 96U &&
        impl.may_have(*player, stack))
    {
        ++player->death_drops;
        impl.hold(*player, stack, -1);
        explained = true;
    }
    for (Impl::Source &source : player->sources)
    {
        if (explained)
            break;
        const float gap = distance(source.position, position);
        switch (source.kind)
        {
        case Impl::SourceKind::dig:
            // Every block pickup starts at its cell's centre; a broken
            // container sheds what it held, and a torch or door that lost
            // its support next to the cell drops itself.
            explained = now - source.at <= 10.0 && gap <= 2.5F &&
                        (mined_item(source.block) == stack.item || world::is_container(source.block) ||
                         stack.item == InventoryItemId::torch ||
                         door_model_name(stack.item).has_value());
            break;
        case Impl::SourceKind::kill:
            explained = now - source.at <= 10.0 && gap <= 40.0F;
            break;
        case Impl::SourceKind::blast:
            explained = now - source.at <= 10.0 && gap <= 16.0F;
            break;
        case Impl::SourceKind::loot:
            explained = now - source.at <= 30.0 && gap <= 6.0F;
            break;
        case Impl::SourceKind::crate:
            if (now - source.at <= 10.0 && gap <= 4.0F)
                for (InventoryStack &held : source.contents)
                    if (held.item == stack.item && held.count >= stack.count)
                    {
                        held.count = static_cast<std::uint16_t>(held.count - stack.count);
                        explained = true;
                        break;
                    }
            break;
        }
    }
    // A drop from what the player carries (Q, a stack thrown off the screen).
    if (!explained && player->alive && player->has_position &&
        impl.nearest(*player, position, now, 1.0) <= 6.0F && impl.may_have(*player, stack))
    {
        impl.hold(*player, stack, -1);
        explained = true;
    }
    if (!explained)
    {
        impl.violation(*player, weight_blatant,
                       std::format("created {} of item {} from nothing", stack.count,
                                   static_cast<int>(stack.item)),
                       now);
        return AuthorityVerdict::refuse;
    }
    register_pickup(gid, pickup_id, std::move(stack), position);
    return AuthorityVerdict::accept;
}

void AuthorityHost::register_pickup(std::uint8_t spawner, std::uint32_t pickup_id,
                                    InventoryStack stack, world::Vec3 position)
{
    Impl &impl = *impl_;
    if (impl.pickups.size() >= 8192U)
        impl.pickups.erase(impl.pickups.begin());
    impl.pickups.insert_or_assign({spawner, pickup_id}, Impl::Pickup{std::move(stack), position, false});
}

AuthorityVerdict AuthorityHost::request_pickup(std::uint8_t gid, std::uint8_t spawner,
                                               std::uint32_t pickup_id, double now)
{
    Impl &impl = *impl_;
    const auto found = impl.pickups.find({spawner, pickup_id});
    Impl::Player *player = impl.find(gid);
    if (found == impl.pickups.end())
        // Not one the host registered (it was there before the authority, or
        // the host's own path made it): the app's own one-winner rule decides.
        return player != nullptr && !player->alive ? AuthorityVerdict::refuse
                                                   : AuthorityVerdict::accept;
    // Two players touching one pickup at once is ordinary (PendingPickupList):
    // the second asks before it hears the first was given it.
    if (found->second.granted)
        return AuthorityVerdict::refuse;
    if (player != nullptr && !player->local)
    {
        if (!player->alive || player->kicked)
            return AuthorityVerdict::refuse;
        // PickupEntity: requested inside two units of the player's centre on
        // the requester's own picture; both pictures lag here.
        if (player->has_position &&
            impl.nearest(*player, found->second.position, now, 1.0) > 10.0F)
        {
            impl.violation(*player, weight_cheat, "asked for a pickup out of its reach", now);
            return AuthorityVerdict::refuse;
        }
        impl.grant(*player, found->second.stack, now);
    }
    // Kept, granted, so a late or repeated request still finds it taken.
    found->second.granted = true;
    return AuthorityVerdict::accept;
}

AuthorityVerdict AuthorityHost::observe_stock_message(std::uint8_t sender, std::uint8_t recipient,
                                                      std::string_view name,
                                                      std::span<const std::uint8_t> payload,
                                                      double now)
{
    Impl &impl = *impl_;
    const ProtocolVersion protocol = impl.rules.protocol;
    PayloadError error{};
    if (name == "PlayerUpdateMessage")
    {
        PlayerUpdatePayload update;
        if (!decode_player_update_payload(payload, protocol, update, error))
            return AuthorityVerdict::accept; // the session's own checks own malformed bytes
        return observe_player_update(sender, update, now);
    }
    if (name == "ChangeCarriedItemMessage")
    {
        if (payload.size() == 2U)
        {
            const auto wire = static_cast<std::int16_t>(static_cast<std::uint16_t>(payload[0]) |
                                                        (static_cast<std::uint16_t>(payload[1]) << 8U));
            InventoryItemId item{};
            if (inventory_item_from_wire(wire, protocol, item))
                observe_carried_item(sender, item, now);
        }
        return AuthorityVerdict::accept;
    }
    if (name == "MeleePlayerMessage")
    {
        MeleePlayerPayload melee;
        if (!decode_melee_player_payload(payload, protocol, melee, error))
            return AuthorityVerdict::accept;
        return observe_melee(sender, recipient, melee, now);
    }
    if (name == "GunshotMessage" || name == "ShotgunShotMessage")
    {
        std::vector<Vector3> directions;
        std::int16_t wire{};
        if (name == "GunshotMessage")
        {
            GunshotPayload shot;
            if (!decode_gunshot_payload(payload, protocol, shot, error))
                return AuthorityVerdict::accept;
            directions.push_back(shot.direction);
            wire = shot.item_id;
        }
        else
        {
            ShotgunShotPayload shot;
            if (!decode_shotgun_shot_payload(payload, protocol, shot, error))
                return AuthorityVerdict::accept;
            directions.assign(shot.directions.begin(), shot.directions.end());
            wire = shot.item_id;
        }
        InventoryItemId item{};
        if (!inventory_item_from_wire(wire, protocol, item))
            return AuthorityVerdict::refuse;
        return observe_gunshot(sender, item, directions, now);
    }
    if (name == "GrenadeMessage")
    {
        GrenadePayload grenade;
        if (!decode_grenade_payload(payload, protocol, grenade, error))
            return AuthorityVerdict::accept;
        return observe_launch(sender, {grenade.position.x, grenade.position.y, grenade.position.z},
                              {grenade.direction.x, grenade.direction.y, grenade.direction.z}, now);
    }
    if (name == "FireRocketMessage")
    {
        FireRocketPayload rocket;
        if (!decode_fire_rocket_payload(payload, protocol, rocket, error))
            return AuthorityVerdict::accept;
        return observe_launch(sender, {rocket.position.x, rocket.position.y, rocket.position.z},
                              {rocket.direction.x, rocket.direction.y, rocket.direction.z}, now);
    }
    if (name == "DetonateGrenadeMessage")
    {
        DetonateGrenadePayload blast;
        if (!decode_detonate_grenade_payload(payload, protocol, blast, error))
            return AuthorityVerdict::accept;
        const world::Vec3 location{blast.location.x, blast.location.y, blast.location.z};
        if (observe_detonation(sender, location, now) == AuthorityVerdict::refuse)
            return AuthorityVerdict::refuse;
        // Only HE (ordinal zero) explodes; smoke and flash do nothing.
        if (blast.grenade_type == 0U)
            static_cast<void>(apply_explosion(location, world::he_grenade_damage_bands, sender, now));
        return AuthorityVerdict::accept;
    }
    if (name == "DetonateRocketMessage")
    {
        DetonateRocketPayload blast;
        if (!decode_detonate_rocket_payload(payload, protocol, blast, error))
            return AuthorityVerdict::accept;
        const world::Vec3 location{blast.location.x, blast.location.y, blast.location.z};
        if (observe_detonation(sender, location, now) == AuthorityVerdict::refuse)
            return AuthorityVerdict::refuse;
        if (!blast.hit_dragon)
            static_cast<void>(apply_explosion(location, world::rocket_damage_bands, sender, now));
        return AuthorityVerdict::accept;
    }
    if (name == "DetonateExplosiveMessage")
    {
        DetonateExplosivePayload blast;
        if (!decode_detonate_explosive_payload(payload, protocol, blast, error))
            return AuthorityVerdict::accept;
        const world::Vec3 location{static_cast<float>(blast.location.x),
                                   static_cast<float>(blast.location.y),
                                   static_cast<float>(blast.location.z)};
        if (observe_detonation(sender, location, now) == AuthorityVerdict::refuse)
            return AuthorityVerdict::refuse;
        const world::ExplosiveKind kind =
            blast.explosive_type == 0U ? world::ExplosiveKind::tnt : world::ExplosiveKind::c4;
        static_cast<void>(
            apply_explosion(location, world::explosive_definition(kind).damage, sender, now));
        return AuthorityVerdict::accept;
    }
    if (name == "AlterBlockMessage")
    {
        AlterBlockPayload edit;
        if (!decode_alter_block_payload(payload, protocol, edit, error))
            return AuthorityVerdict::accept;
        const std::optional<world::BlockType> previous =
            impl.world != nullptr ? impl.world->block(edit.block_location.x, edit.block_location.y,
                                                      edit.block_location.z)
                                  : std::nullopt;
        if (!previous.has_value())
            return AuthorityVerdict::accept;
        return observe_block_edit(sender, edit.block_location, *previous,
                                  static_cast<world::BlockType>(edit.block_type), now);
    }
    if (name == "RemoveBlocksMessage")
    {
        RemoveBlocksPayload removal;
        if (!decode_remove_blocks_payload(payload, removal, error))
            return AuthorityVerdict::accept;
        return observe_block_removal(sender, removal.blocks, now);
    }
    if (name == "KillEnemyMessage")
    {
        KillEnemyPayload kill;
        if (decode_kill_enemy_payload(payload, protocol, kill, error))
        {
            // The loot lands where the enemy died; where the host has no copy
            // of it (far from the host) the killer's own position stands in.
            std::optional<world::Vec3> at;
            if (impl.world != nullptr && kill.target_id >= 0 && kill.target_id <= 255 &&
                kill.enemy_id >= 0)
                if (const auto enemy = impl.world->enemy(static_cast<std::uint8_t>(kill.target_id),
                                                         static_cast<std::uint32_t>(kill.enemy_id)))
                    at = enemy->position;
            observe_enemy_kill(kill.killer_id, at, now);
        }
        return AuthorityVerdict::accept;
    }
    if (name == "KillDragonMessage")
    {
        KillDragonPayload kill;
        if (decode_kill_dragon_payload(payload, protocol, kill, error))
            observe_enemy_kill(kill.killer_gid,
                               world::Vec3{kill.location.x, kill.location.y, kill.location.z}, now);
        return AuthorityVerdict::accept;
    }
    if (name == "CreatePickupMessage")
    {
        CreatePickupPayload pickup;
        if (!decode_create_pickup_payload(payload, protocol, pickup, error))
            return AuthorityVerdict::accept;
        PlayerInventory::Slot slot;
        if (!inventory_slot_to_stack(InventorySlotPayload{pickup.item}, protocol, slot) ||
            !slot.has_value())
            return AuthorityVerdict::refuse;
        return observe_pickup_created(
            sender, static_cast<std::uint32_t>(pickup.pickup_id), std::move(*slot),
            {pickup.spawn_position.x, pickup.spawn_position.y, pickup.spawn_position.z}, now);
    }
    if (name == "RequestPickupMessage")
    {
        RequestPickupPayload request;
        if (!decode_request_pickup_payload(payload, request, error) || request.spawner_id < 0 ||
            request.spawner_id > 255)
            return AuthorityVerdict::accept;
        return request_pickup(sender, static_cast<std::uint8_t>(request.spawner_id),
                              static_cast<std::uint32_t>(request.pickup_id), now);
    }
    if (name == "InventoryStoreOnServerMessage")
    {
        InventoryStorePayload stored;
        if (!decode_inventory_store_payload(payload, protocol, stored, error))
            return AuthorityVerdict::accept;
        std::vector<InventoryStack> stacks;
        const auto add = [&](const InventorySlotPayload &source) {
            PlayerInventory::Slot slot;
            if (inventory_slot_to_stack(source, protocol, slot) && slot.has_value())
                stacks.push_back(std::move(*slot));
        };
        for (const InventorySlotPayload &source : stored.inventory.storage)
            add(source);
        for (const InventoryTrayPayload &tray : stored.inventory.trays)
            for (const InventorySlotPayload &source : tray)
                add(source);
        return observe_inventory(sender, stacks, now);
    }
    if (name == "RestartLevelMessage")
    {
        observe_level_restart(now);
        return AuthorityVerdict::accept;
    }
    return AuthorityVerdict::accept;
}

void AuthorityHost::pickup_given(std::uint8_t gid, std::uint8_t spawner, std::uint32_t pickup_id,
                                 const InventoryStack &stack, double now)
{
    Impl &impl = *impl_;
    if (impl.pickups.contains({spawner, pickup_id}))
        return;
    if (Impl::Player *player = impl.remote(gid))
        impl.grant(*player, stack, now);
}

void AuthorityHost::update(double now)
{
    Impl &impl = *impl_;
    const double elapsed = impl.last_update <= never ? 0.0 : std::max(0.0, now - impl.last_update);
    impl.last_update = now;
    for (auto &[gid, player] : impl.players)
    {
        if (player.local)
            continue;
        impl.decay(player, now);
        if (player.teleport.has_value() && now > player.teleport->until)
            player.teleport.reset();
        if (player.pending_jump.has_value() &&
            now - player.pending_jump->at > impl.tuning.teleport_grace_seconds)
        {
            const float moved = distance(player.pending_jump->position, player.accepted);
            player.pending_jump.reset();
            impl.violation(player, weight_blatant,
                           std::format("moved {:.0f} m at once without a teleport", moved), now);
            impl.correct(player, player.accepted, "teleport", now);
        }
        if (player.alive)
        {
            // InGameHUD.HealthRecoverRate.
            if (now - player.last_harm_at > regeneration_delay_seconds && player.health < 100.0F)
            {
                player.health = std::min(
                    100.0F, player.health + regeneration_per_second * static_cast<float>(elapsed));
                impl.mark_health(player, false);
            }
            // InGameHUD's lava timer: 25 every half second below y -60.
            if (player.has_position && world::lava_submersion(player.accepted.y) > 0.0F)
            {
                player.lava_time -= static_cast<float>(elapsed);
                if (player.lava_time <= 0.0001F)
                {
                    player.lava_time = world::lava_damage_period_seconds;
                    world::Vec3 source = player.accepted;
                    source.y -= 10.0F;
                    impl.harm(player, world::lava_damage_health, HarmCause::lava, source, now,
                              std::nullopt);
                }
            }
            else
                player.lava_time = world::lava_damage_period_seconds;
        }
        if (player.alive)
            impl.settle_fall(player, now);
        if (player.alive)
            impl.settle_sightings(player, now);
        if (player.correcting && now - player.fix_at > impl.tuning.correction_grace_seconds)
        {
            // Still where it was told to leave: say it again; a client that
            // keeps refusing is refusing on purpose.
            ++player.fixes_ignored;
            player.fix_at = now;
            ++player.corrections;
            if (player.fixes_ignored >= 2)
                impl.violation(player, weight_cheat, "ignored the host's position correction", now);
            AuthorityEvent event;
            event.kind = AuthorityEventKind::correction;
            event.gid = gid;
            event.position = player.fix_to;
            event.reason = "repeat";
            impl.events.push_back(std::move(event));
        }
        if (player.health_dirty &&
            (player.health_urgent || now - player.health_sent_at >= impl.tuning.health_interval_seconds))
        {
            player.health_dirty = false;
            player.health_urgent = false;
            player.health_sent_at = now;
            AuthorityEvent event;
            event.kind = AuthorityEventKind::health;
            event.gid = gid;
            event.value = player.health;
            event.alive = player.alive;
            event.cause = player.last_cause;
            if (player.last_source.has_value())
                event.position = *player.last_source;
            impl.events.push_back(std::move(event));
        }
    }
}

std::vector<AuthorityEvent> AuthorityHost::take_events()
{
    return std::exchange(impl_->events, {});
}

// ---------------------------------------------------------------------------
// Over the mod channel
// ---------------------------------------------------------------------------

AuthorityServerLink::AuthorityServerLink(AuthorityHost &authority, modding::ModNetServer &server,
                                         GidOf gid_of, HandleOf handle_of,
                                         std::function<double()> clock)
    : authority_{authority}, server_{server}, gid_of_{std::move(gid_of)},
      handle_of_{std::move(handle_of)}, clock_{std::move(clock)}
{
    // Every client message arrives here after the channel's rate, size and
    // decode limits; nothing in it is believed beyond what is checked.
    server_.on("", auth_hurt_message, [this](modding::Handle player, const modding::Value &payload) {
        const std::optional<std::uint8_t> gid = gid_of_(player);
        if (!gid.has_value())
            return;
        HarmReport report;
        const std::optional<double> cause = number_field(payload, "c");
        const std::optional<double> amount = number_field(payload, "a");
        if (!cause.has_value() || !amount.has_value() || *cause < 0.0 || *cause >= harm_cause_count)
            return;
        report.cause = static_cast<HarmCause>(static_cast<int>(*cause));
        report.amount = static_cast<float>(std::clamp(*amount, -1.0, 1.0e6));
        if (const std::optional<double> enemy = number_field(payload, "e");
            enemy.has_value() && *enemy >= 0.0 && *enemy <= 2147483647.0)
            report.enemy = static_cast<std::uint32_t>(*enemy);
        report.source = vec3_field(payload, "s");
        static_cast<void>(authority_.report_harm(*gid, report, clock_()));
    });
    server_.on("", auth_respawn_message, [this](modding::Handle player, const modding::Value &) {
        if (const std::optional<std::uint8_t> gid = gid_of_(player))
            static_cast<void>(authority_.request_respawn(*gid, clock_()));
    });
    server_.on("", auth_teleport_message, [this](modding::Handle player, const modding::Value &payload) {
        const std::optional<std::uint8_t> gid = gid_of_(player);
        const std::optional<double> kind = number_field(payload, "k");
        const std::optional<world::Vec3> target = vec3_field(payload, "p");
        if (!gid.has_value() || !kind.has_value() || !target.has_value() || *kind < 0.0 ||
            *kind > static_cast<double>(TeleportKind::respawn))
            return;
        static_cast<void>(authority_.observe_teleport_notice(
            *gid, static_cast<TeleportKind>(static_cast<int>(*kind)), *target, clock_()));
    });
}

AuthorityServerLink::~AuthorityServerLink()
{
    server_.off("", auth_hurt_message);
    server_.off("", auth_respawn_message);
    server_.off("", auth_teleport_message);
}

std::vector<AuthorityEvent> AuthorityServerLink::flush()
{
    std::vector<AuthorityEvent> events = authority_.take_events();
    for (const AuthorityEvent &event : events)
    {
        const std::optional<AuthorityPlayerView> view = authority_.player(event.gid);
        if (!view.has_value() || view->local)
            continue;
        const std::optional<modding::Handle> handle = handle_of_(event.gid);
        if (!handle.has_value())
            continue;
        try
        {
            switch (event.kind)
            {
            case AuthorityEventKind::health: {
                modding::Map payload{{"h", modding::Value{static_cast<double>(event.value)}},
                                     {"d", modding::Value{!event.alive}},
                                     {"c", modding::Value{static_cast<int>(event.cause)}}};
                if (finite(event.position))
                    payload.emplace_back("s", modding::Value{to_value(event.position)});
                static_cast<void>(server_.send_to(*handle, "", auth_health_message,
                                                  modding::Value{std::move(payload)}));
                break;
            }
            case AuthorityEventKind::died:
                static_cast<void>(server_.send_to(
                    *handle, "", auth_died_message,
                    modding::Value{modding::Map{{"c", modding::Value{static_cast<int>(event.cause)}},
                                                {"s", modding::Value{to_value(event.position)}}}}));
                break;
            case AuthorityEventKind::respawned:
                static_cast<void>(server_.send_to(
                    *handle, "", auth_live_message,
                    modding::Value{modding::Map{{"h", modding::Value{static_cast<double>(event.value)}}}}));
                break;
            case AuthorityEventKind::correction:
                static_cast<void>(server_.send_to(
                    *handle, "", auth_fix_message,
                    modding::Value{modding::Map{{"p", modding::Value{to_value(event.position)}},
                                                {"r", modding::Value{event.reason}}}}));
                break;
            case AuthorityEventKind::violation:
            case AuthorityEventKind::kick:
                break;
            }
        }
        catch (const modding::ScriptError &)
        {
            // A payload the channel refuses is the host's bug, never a peer's.
        }
    }
    return events;
}

void AuthorityServerLink::notice_all(std::string_view text)
{
    try
    {
        static_cast<void>(server_.broadcast(
            "", auth_notice_message,
            modding::Value{modding::Map{{"t", modding::Value{std::string{text.substr(0U, 200U)}}}}}));
    }
    catch (const modding::ScriptError &)
    {
    }
}

AuthorityClient::AuthorityClient(modding::ModNetClient &client) : client_{client}
{
    client_.on("", auth_health_message, [this](const modding::Value &payload) {
        const std::optional<double> health = number_field(payload, "h");
        if (!health.has_value())
            return;
        AuthorityClientEvent event;
        event.kind = AuthorityClientEventKind::health;
        event.health = static_cast<float>(std::clamp(*health, 0.0, 100.0));
        const modding::Value *dead = payload.field("d");
        event.alive = dead == nullptr || dead->type() != modding::ValueType::boolean || !dead->as_bool();
        if (const std::optional<double> cause = number_field(payload, "c");
            cause.has_value() && *cause >= 0.0 && *cause < harm_cause_count)
            event.cause = static_cast<HarmCause>(static_cast<int>(*cause));
        event.source = vec3_field(payload, "s");
        health_ = event.health;
        alive_ = event.alive;
        // The host answers a respawn request with the life it decided.
        respawn_pending_ = false;
        events_.push_back(std::move(event));
    });
    client_.on("", auth_died_message, [this](const modding::Value &payload) {
        AuthorityClientEvent event;
        event.kind = AuthorityClientEventKind::died;
        event.health = 0.0F;
        event.alive = false;
        if (const std::optional<double> cause = number_field(payload, "c");
            cause.has_value() && *cause >= 0.0 && *cause < harm_cause_count)
            event.cause = static_cast<HarmCause>(static_cast<int>(*cause));
        event.source = vec3_field(payload, "s");
        health_ = 0.0F;
        alive_ = false;
        respawn_pending_ = false;
        events_.push_back(std::move(event));
    });
    client_.on("", auth_live_message, [this](const modding::Value &payload) {
        AuthorityClientEvent event;
        event.kind = AuthorityClientEventKind::respawned;
        event.health = static_cast<float>(std::clamp(number_field(payload, "h").value_or(100.0), 0.0, 100.0));
        health_ = event.health;
        alive_ = true;
        respawn_pending_ = false;
        events_.push_back(std::move(event));
    });
    client_.on("", auth_fix_message, [this](const modding::Value &payload) {
        const std::optional<world::Vec3> position = vec3_field(payload, "p");
        if (!position.has_value())
            return;
        AuthorityClientEvent event;
        event.kind = AuthorityClientEventKind::correction;
        event.position = *position;
        if (const modding::Value *reason = payload.field("r");
            reason != nullptr && reason->type() == modding::ValueType::string)
            event.text = reason->as_string().substr(0U, 64U);
        events_.push_back(std::move(event));
    });
    client_.on("", auth_notice_message, [this](const modding::Value &payload) {
        const modding::Value *text = payload.field("t");
        if (text == nullptr || text->type() != modding::ValueType::string)
            return;
        AuthorityClientEvent event;
        event.kind = AuthorityClientEventKind::notice;
        event.text = text->as_string().substr(0U, 200U);
        events_.push_back(std::move(event));
    });
}

AuthorityClient::~AuthorityClient()
{
    for (const std::string_view name : {auth_health_message, auth_died_message, auth_live_message,
                                        auth_fix_message, auth_notice_message})
        client_.off("", name);
}

void AuthorityClient::report_harm(const HarmReport &report)
{
    if (!(report.amount > 0.0F) || !std::isfinite(report.amount))
        return;
    modding::Map payload{{"c", modding::Value{static_cast<int>(report.cause)}},
                         {"a", modding::Value{static_cast<double>(report.amount)}}};
    if (report.enemy.has_value())
        payload.emplace_back("e", modding::Value{static_cast<std::int64_t>(*report.enemy)});
    if (report.source.has_value() && finite(*report.source))
        payload.emplace_back("s", modding::Value{to_value(*report.source)});
    try
    {
        static_cast<void>(client_.send("", auth_hurt_message, modding::Value{std::move(payload)}));
    }
    catch (const modding::ScriptError &)
    {
    }
}

void AuthorityClient::request_respawn()
{
    if (respawn_pending_)
        return;
    try
    {
        respawn_pending_ = client_.send("", auth_respawn_message, modding::Value{modding::Map{}});
    }
    catch (const modding::ScriptError &)
    {
    }
}

void AuthorityClient::announce_teleport(TeleportKind kind, world::Vec3 target)
{
    if (!finite(target))
        return;
    try
    {
        static_cast<void>(client_.send(
            "", auth_teleport_message,
            modding::Value{modding::Map{{"k", modding::Value{static_cast<int>(kind)}},
                                        {"p", modding::Value{to_value(target)}}}}));
    }
    catch (const modding::ScriptError &)
    {
    }
}

std::vector<AuthorityClientEvent> AuthorityClient::take_events()
{
    return std::exchange(events_, {});
}

float AuthorityClient::health() const noexcept
{
    return health_;
}

bool AuthorityClient::alive() const noexcept
{
    return alive_;
}

bool AuthorityClient::respawn_pending() const noexcept
{
    return respawn_pending_;
}

} // namespace openminerz::net
