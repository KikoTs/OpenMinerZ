// Which animation plays when, against the shipped client.
//
// tools/OriginalClientInterop/AnimationParityProbe drives the ORIGINAL Player
// through scripted input for every item family and records, per frame and per
// animation channel, the clip, its clock in ticks, its speed and its cross-fade,
// for the local first-person player and for the remote copy another peer
// animates from the PlayerUpdateMessage stream.
//
// With no arguments this replays the pinned half (animation_parity_pinned.hpp,
// the same timelines recorded from both shipped generations) through
// world::PlayerAnimator, so parity holds on a machine without the game.
//
//   --replay <trace> [--summary <file>] [--strict]
// replays every scenario of a probe trace and compares every frame exactly.
// Scenarios named `base@ticks` are the same script stepped by the ORIGINAL at
// another frame time; the summary reports how far the original's own event
// times move with the frame rate, which no fixed-step port can beat.
#include "openminerz/inventory.hpp"
#include "openminerz/world/player_animation.hpp"

#include "animation_parity_pinned.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace openminerz;
using world::AnimationGeneration;
using world::PlayerAnimator;

int failures = 0;

void check(bool value, std::string_view message)
{
    if (value)
        return;
    ++failures;
    std::cerr << "FAIL " << message << '\n';
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos)
        {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

[[nodiscard]] long long integer(std::string_view text, int base = 10)
{
    long long value{};
    std::from_chars(text.data(), text.data() + text.size(), value, base);
    return value;
}

[[nodiscard]] float float_bits(std::string_view hex)
{
    const auto bits = static_cast<std::uint32_t>(integer(hex, 16));
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] std::string_view field(std::string_view line, std::string_view key)
{
    const std::string needle = " " + std::string{key} + "=";
    const std::size_t at = line.find(needle);
    if (at == std::string_view::npos)
        return {};
    const std::size_t start = at + needle.size();
    const std::size_t end = line.find(' ', start);
    return line.substr(start, end == std::string_view::npos ? end : end - start);
}

struct ItemFacts final
{
    int mode{};
    bool gun{};
    bool grenade{};
    std::int64_t reload_ticks{};
    int capacity{};
};

// One frame of what the original code was given, and what it then showed.
struct Frame final
{
    float strafe{};
    float forward{};
    // The state the previous frame ended in: what UpdateAnimation reads.
    std::uint8_t flags{}; // UsingTool, Shouldering, Reloading, Dead, PlayGrenadeAnim, Ready
    bool fps{true};
    int item{};
    // This frame's own.
    std::uint8_t mode{};
    int active_item{};
    int rounds{-1};
    bool grenade_reset{};
    bool has_update{};
    world::RemoteAnimationUpdate update{};
};

struct Expected final
{
    std::string local;
    std::string remote;
};

[[nodiscard]] std::uint64_t fnv(std::uint64_t hash, std::string_view text) noexcept
{
    for (const char letter : text)
    {
        hash ^= static_cast<unsigned char>(letter);
        hash *= 1099511628211ULL;
    }
    return hash;
}

constexpr std::uint64_t fnv_seed = 14695981039346656037ULL;

struct Replay final
{
    AnimationGeneration generation{AnimationGeneration::classic};
    std::int64_t ticks{world::animation_frame_ticks};
    PlayerAnimator local{AnimationGeneration::classic};
    PlayerAnimator remote{AnimationGeneration::classic};
    world::FootstepClock footsteps;
    int previous_rounds{-1};
    int previous_active{-1};
    // Event frames, for the frame-rate baseline.
    std::vector<int> reload_finishes;
    std::vector<int> use_clears;

    Replay(AnimationGeneration which, std::int64_t frame_ticks)
        : generation{which}, ticks{frame_ticks}, local{which}, remote{which}
    {
    }

    [[nodiscard]] Expected step(int frame_index, const Frame &frame,
                                const std::map<int, ItemFacts> &items)
    {
        const auto fact = [&](int id) {
            const auto found = items.find(id);
            return found == items.end() ? ItemFacts{} : found->second;
        };
        // ---- ProcessNetworkMessages ----
        if (frame.grenade_reset)
        {
            local.play_grenade_reset();
            remote.play_grenade_reset();
        }
        world::PlayerAnimationResult remote_result;
        if (frame.has_update)
            remote_result = remote.update(world::remote_animation_input(frame.update));

        // ---- Player.ProcessInput -> UpdateAnimation ----
        const ItemFacts held = fact(frame.item);
        world::PlayerAnimationInput input;
        input.using_tool = (frame.flags & 1U) != 0U;
        input.shouldering = (frame.flags & 2U) != 0U;
        input.reloading = (frame.flags & 4U) != 0U;
        input.dead = (frame.flags & 8U) != 0U;
        input.play_grenade_anim = (frame.flags & 16U) != 0U;
        input.ready_to_throw_grenade = (frame.flags & 32U) != 0U;
        // A dead player's ProcessInput calls UpdateAnimation(0, 0, ...).
        input.walk = input.dead ? 0.0F : frame.forward;
        input.strafe = input.dead ? 0.0F : frame.strafe;
        input.mode = frame.mode;
        input.fps_mode = frame.fps;
        input.local = true;
        input.holding_gun = held.gun;
        input.reload_ticks = held.reload_ticks;
        input.holding_grenade = held.grenade;
        input.empty_handed = frame.item == 0;
        const bool was_use_playing = local.channel(world::AnimationChannel::upper_use).current.has_value();
        const world::PlayerAnimationResult result = local.update(
            input, [&] { return frame.rounds >= 0 && frame.rounds < fact(frame.active_item).capacity; });
        // The HUD reads ActiveInventoryItem again and again through the frame.
        local.active_item_read(frame.active_item == 0);
        if (result.finished_reload && held.gun)
            reload_finishes.push_back(frame_index);
        if (was_use_playing && !local.channel(world::AnimationChannel::upper_use).current.has_value())
            use_clears.push_back(frame_index);

        // ---- the scene ----
        const bool footstep = footsteps.step(local, true, true, ticks);
        local.advance(ticks);
        remote.advance(ticks);

        const bool inserted = frame.active_item == previous_active && frame.rounds > previous_rounds &&
                              previous_rounds >= 0;
        check(inserted == (result.finished_reload && held.gun && frame.rounds > previous_rounds),
              "frame " + std::to_string(frame_index) + ": FinishReload on the frame the rounds went in");
        previous_rounds = frame.rounds;
        previous_active = frame.active_item;

        Expected shown;
        shown.local = "L" + local.describe() + " st=" +
                      std::to_string(static_cast<int>(local.shoulder_state())) + " uap=" +
                      (local.use_clip_playing() ? "1" : "0") + " e=" + (footstep ? "1" : "0") +
                      (result.grenade_arm_cue ? "1" : "0") + (result.reload_cue_started ? "1" : "0");
        shown.remote = "R" + remote.describe() + " st=" +
                       std::to_string(static_cast<int>(remote.shoulder_state())) + " uap=" +
                       (remote.use_clip_playing() ? "1" : "0") + " e=" +
                       (remote_result.grenade_arm_cue ? "1" : "0") +
                       (remote_result.reload_cue_started ? "1" : "0");
        return shown;
    }
};

// ------------------------------------------------------------- trace replay

[[nodiscard]] std::string channels_of(std::string_view line)
{
    const std::size_t start = line.find(" c0=");
    if (start == std::string_view::npos)
        return {};
    const std::size_t end = line.find(" ev=", start);
    return std::string{line.substr(start, end == std::string_view::npos ? end : end - start)};
}

[[nodiscard]] std::uint8_t flag_bits(std::string_view text)
{
    std::uint8_t bits{};
    for (std::size_t index = 0; index < text.size() && index < 6U; ++index)
        if (text[index] == '1')
            bits = static_cast<std::uint8_t>(bits | (1U << index));
    return bits;
}

struct ScenarioResult final
{
    std::string name;
    std::int64_t ticks{};
    int frames{};
    int first_difference{-1};
    std::string detail;
    std::vector<double> reload_seconds;
    std::vector<double> clear_seconds;
};

int replay_trace(const std::string &path, const std::string &summary_path, bool strict)
{
    std::ifstream input{path};
    if (!input)
    {
        std::cerr << "cannot read " << path << '\n';
        return 2;
    }
    AnimationGeneration generation = AnimationGeneration::classic;
    std::map<int, ItemFacts> items;
    std::vector<ScenarioResult> results;
    std::string line;
    std::string in_line;
    std::string local_line;
    Replay replay{generation, world::animation_frame_ticks};
    ScenarioResult current;
    bool active = false;
    Frame carried;
    int clip_checks = 0;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string_view text{line};
        if (text.starts_with("VERSION "))
        {
            generation = field(text, "modern") == "1" ? AnimationGeneration::modern
                                                      : AnimationGeneration::classic;
        }
        else if (text.starts_with("CLIP "))
        {
            // The registry compiled into the game must be the one the shipped
            // executable registers.
            const std::vector<std::string_view> tokens = split(text, ' ');
            const world::AnimationClip clip = world::animation_clip_from_name(tokens[1]);
            check(clip != world::AnimationClip::none, "clip registered natively: " + std::string{tokens[1]});
            check(world::animation_clip_ticks(clip, generation) == integer(field(text, "ticks")),
                  "clip duration: " + std::string{tokens[1]});
            check(world::animation_clip_info(clip).looping == (field(text, "loop") == "1"),
                  "clip looping: " + std::string{tokens[1]});
            ++clip_checks;
        }
        else if (text.starts_with("ITEM "))
        {
            const std::vector<std::string_view> tokens = split(text, ' ');
            const int id = static_cast<int>(integer(tokens[1]));
            ItemFacts facts;
            facts.mode = static_cast<int>(integer(field(text, "mode")));
            facts.gun = !field(text, "reload").empty();
            facts.reload_ticks = facts.gun ? integer(field(text, "reload")) : 0;
            facts.capacity = facts.gun ? static_cast<int>(integer(field(text, "capacity"))) : 0;
            facts.grenade = field(text, "class").find("Grenade") != std::string_view::npos &&
                            !facts.gun;
            items[id] = facts;
            // InventoryItemClass.PlayerAnimationMode against the native table.
            // Ids 0..131 are shared; 1.9.9.8's extra ids are internal 256+.
            if (generation == AnimationGeneration::classic || id <= 122)
                check(static_cast<int>(item_player_animation_mode(static_cast<InventoryItemId>(id))) ==
                          facts.mode,
                      "PlayerMode of item " + std::to_string(id) + " is " + std::to_string(facts.mode));
        }
        else if (text.starts_with("SCENARIO "))
        {
            const std::vector<std::string_view> tokens = split(text, ' ');
            current = {};
            current.name = std::string{tokens[1]};
            current.ticks = integer(field(text, "ticks"));
            current.frames = static_cast<int>(integer(field(text, "frames")));
            replay = Replay{generation, current.ticks};
            carried = {};
            const std::string_view tray = field(text, "tray");
            carried.item = static_cast<int>(integer(split(tray, ',').front()));
            active = true;
        }
        else if (text.starts_with("IN "))
        {
            in_line = line;
        }
        else if (text.starts_with("L "))
        {
            local_line = line;
        }
        else if (text.starts_with("R ") && active)
        {
            const std::string_view in{in_line};
            const std::string_view local{local_line};
            const int frame_index = static_cast<int>(integer(split(local, ' ')[1]));
            Frame frame = carried;
            const std::vector<std::string_view> move = split(field(in, "mv"), ',');
            frame.strafe = float_bits(move[0]);
            frame.forward = float_bits(move[1]);
            frame.mode = static_cast<std::uint8_t>(integer(field(local, "m")));
            const std::vector<std::string_view> item = split(field(local, "item"), '/');
            frame.active_item = static_cast<int>(integer(item[0]));
            frame.rounds = item.size() > 1U ? static_cast<int>(integer(item[1])) : -1;
            frame.grenade_reset = field(local, "nt").find("grenade:") != std::string_view::npos;
            const std::string_view received = field(text, "rx");
            frame.has_update = received != "-" && received.size() >= 10U;
            if (frame.has_update)
            {
                frame.update.pitch = static_cast<std::uint8_t>(integer(received.substr(0, 2), 16));
                frame.update.flags = static_cast<std::uint8_t>(integer(received.substr(4, 2), 16));
                frame.update.mode = static_cast<std::uint8_t>(integer(received.substr(6, 2), 16));
                frame.update.movement = static_cast<std::uint8_t>(integer(received.substr(8, 2), 16));
                // The receiver's own decode of the movement nibbles.
                const std::vector<std::string_view> parts = split(received, ':');
                const world::PlayerAnimationInput decoded = world::remote_animation_input(frame.update);
                if (!decoded.dead && parts.size() == 3U)
                    check(decoded.strafe == float_bits(parts[1]) && decoded.walk == float_bits(parts[2]),
                          current.name + ": PlayerUpdateMessage movement decode");
            }
            const std::string_view events = field(local, "ev");
            const Expected shown = replay.step(frame_index, frame, items);
            const std::string wanted_local =
                "L" + channels_of(local) + " st=" + std::string{field(local, "st")} + " uap=" +
                std::string{field(local, "uap")} + " e=" +
                (events.find("cue:FootStep") != std::string_view::npos ? "1" : "0") +
                (events.find("cue:GrenadeArm") != std::string_view::npos ? "1" : "0") +
                (events.find("Reload") != std::string_view::npos ? "1" : "0");
            const std::string_view remote_events = field(text, "ev");
            const bool remote_reload = remote_events.find("Reload") != std::string_view::npos;
            // Player.ReloadSound is only ever set on the local player, so a
            // remote player's reload is always the generic "Reload" cue.
            if (remote_reload)
                check(remote_events.find("cue:Reload") != std::string_view::npos,
                      current.name + ": a remote reload plays the generic Reload cue");
            const std::string wanted_remote =
                "R" + channels_of(text) + " st=" + std::string{field(text, "st")} + " uap=" +
                std::string{field(text, "uap")} + " e=" +
                (remote_events.find("cue:GrenadeArm") != std::string_view::npos ? "1" : "0") +
                (remote_reload ? "1" : "0");
            if (current.first_difference < 0 &&
                (shown.local != wanted_local || shown.remote != wanted_remote))
            {
                current.first_difference = frame_index;
                current.detail = shown.local != wanted_local
                                     ? "  original " + wanted_local + "\n  native   " + shown.local
                                     : "  original " + wanted_remote + "\n  native   " + shown.remote;
            }
            // What the next frame's UpdateAnimation will read.
            carried.flags = flag_bits(field(local, "f"));
            carried.fps = field(local, "fps") == "1";
            carried.item = frame.active_item;
        }
        else if (text == "END" && active)
        {
            for (const int frame : replay.reload_finishes)
                current.reload_seconds.push_back(static_cast<double>(frame + 1) *
                                                 static_cast<double>(current.ticks) * 1e-7);
            for (const int frame : replay.use_clears)
                current.clear_seconds.push_back(static_cast<double>(frame + 1) *
                                                static_cast<double>(current.ticks) * 1e-7);
            results.push_back(current);
            active = false;
        }
    }

    int matched = 0;
    std::ostringstream summary;
    summary << "generation " << (generation == AnimationGeneration::modern ? "1.9.9.8" : "1.6.3.8")
            << ", " << clip_checks << " registered clips, " << items.size() << " items, "
            << results.size() << " scenarios\n";
    for (const ScenarioResult &result : results)
    {
        if (result.first_difference < 0)
        {
            ++matched;
            continue;
        }
        summary << "DIFFERS " << result.name << " at frame " << result.first_difference << " of "
                << result.frames << "\n" << result.detail << "\n";
        check(!strict, result.name + " differs from the original at frame " +
                           std::to_string(result.first_difference));
    }
    summary << "matched " << matched << " of " << results.size()
            << " scenarios frame for frame (every channel's clip, clock, speed and cross-fade)\n";
    // How far the original's own event times move with its frame rate.
    double worst_reload = 0.0;
    double worst_clear = 0.0;
    for (const ScenarioResult &variant : results)
    {
        const std::size_t at = variant.name.find('@');
        if (at == std::string::npos)
            continue;
        const std::string base_name = variant.name.substr(0, at);
        const auto base = std::find_if(results.begin(), results.end(), [&](const ScenarioResult &other) {
            return other.name == base_name;
        });
        if (base == results.end())
            continue;
        const auto spread = [](const std::vector<double> &left, const std::vector<double> &right) {
            double worst = 0.0;
            for (std::size_t index = 0; index < std::min(left.size(), right.size()); ++index)
                worst = std::max(worst, std::abs(left[index] - right[index]));
            return worst;
        };
        const double reload = spread(base->reload_seconds, variant.reload_seconds);
        const double clear = spread(base->clear_seconds, variant.clear_seconds);
        worst_reload = std::max(worst_reload, reload);
        worst_clear = std::max(worst_clear, clear);
        summary << "baseline " << variant.name << ": reload completion moves " << reload
                << " s, use-clip end moves " << clear << " s against the 60 Hz run\n";
    }
    summary << "baseline worst: reload completion " << worst_reload << " s, use-clip end "
            << worst_clear << " s (the original against itself across frame rates)\n";
    std::cout << summary.str();
    if (!summary_path.empty())
    {
        std::ofstream out{summary_path};
        out << summary.str();
    }
    check(clip_checks >= 100, "the trace carried the clip registry");
    check(!results.empty(), "the trace carried scenarios");
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------ pinned replay

// "count*token;count*token;..." -> one token per frame.
[[nodiscard]] std::vector<std::string_view> expand(std::string_view stream)
{
    std::vector<std::string_view> frames;
    for (const std::string_view run : split(stream, ';'))
    {
        if (run.empty())
            continue;
        const std::size_t star = run.find('*');
        const long long count = integer(run.substr(0, star));
        for (long long index = 0; index < count; ++index)
            frames.push_back(run.substr(star + 1));
    }
    return frames;
}

void replay_pinned(const animation_parity_pinned::Generation &pinned)
{
    const AnimationGeneration generation =
        pinned.modern ? AnimationGeneration::modern : AnimationGeneration::classic;
    std::map<int, ItemFacts> items;
    for (const animation_parity_pinned::Item &item : pinned.items)
    {
        items[item.id] = {item.mode, item.gun, item.grenade, item.reload_ticks, item.capacity};
        if (!pinned.modern || item.id <= 122)
            check(static_cast<int>(item_player_animation_mode(static_cast<InventoryItemId>(item.id))) ==
                      item.mode,
                  std::string{pinned.version} + ": PlayerMode of item " + std::to_string(item.id));
    }
    for (const animation_parity_pinned::Scenario &scenario : pinned.scenarios)
    {
        const std::vector<std::string_view> local = expand(scenario.local);
        check(static_cast<int>(local.size()) == scenario.frames,
              std::string{scenario.name} + ": pinned stream covers every frame");
        // Remote updates: "count*gap:bytes;" where gap is frames since the last.
        std::map<int, std::string_view> updates;
        int at = -1;
        for (const std::string_view run : expand(scenario.remote))
        {
            const std::size_t colon = run.find(':');
            at += static_cast<int>(integer(run.substr(0, colon)));
            updates[at] = run.substr(colon + 1);
        }
        Replay replay{generation, scenario.ticks};
        Frame carried;
        carried.item = scenario.item;
        std::uint64_t hash = fnv_seed;
        for (int index = 0; index < static_cast<int>(local.size()); ++index)
        {
            // strafe,forward,flags,fps,mode,item,rounds,grenade
            const std::vector<std::string_view> part = split(local[static_cast<std::size_t>(index)], ',');
            if (part.size() != 8U)
            {
                check(false, std::string{scenario.name} + ": malformed pinned frame");
                break;
            }
            Frame frame = carried;
            frame.strafe = float_bits(part[0]);
            frame.forward = float_bits(part[1]);
            frame.mode = static_cast<std::uint8_t>(integer(part[4]));
            frame.active_item = static_cast<int>(integer(part[5]));
            frame.rounds = static_cast<int>(integer(part[6]));
            frame.grenade_reset = part[7] == "1";
            const auto update = updates.find(index);
            frame.has_update = update != updates.end();
            if (frame.has_update)
            {
                const std::string_view bytes = update->second;
                frame.update.pitch = static_cast<std::uint8_t>(integer(bytes.substr(0, 2), 16));
                frame.update.flags = static_cast<std::uint8_t>(integer(bytes.substr(2, 2), 16));
                frame.update.mode = static_cast<std::uint8_t>(integer(bytes.substr(4, 2), 16));
                frame.update.movement = static_cast<std::uint8_t>(integer(bytes.substr(6, 2), 16));
            }
            const Expected shown = replay.step(index, frame, items);
            hash = fnv(fnv(hash, shown.local), "\n");
            hash = fnv(fnv(hash, shown.remote), "\n");
            carried.flags = flag_bits(part[2]);
            carried.fps = part[3] == "1";
            carried.item = frame.active_item;
        }
        check(hash == scenario.hash, std::string{pinned.version} + " " + std::string{scenario.name} +
                                         ": every channel of every frame matches the original");
    }
}

// The reported bug: a laser weapon's trigger swung the gun like a pickaxe.
// Every firearm family must fire with its own registered Shoot clip on the
// use channel, held from the shoulder down, and never with a melee clip.
void presentation_interpolates_without_advancing_gameplay()
{
    // Exercise playback boundaries as well as speeds whose per-tick step is
    // rounded by the original. Render sampling must meet that exact next tick.
    for (const float speed : {0.25F, 0.7F, 1.0F, 1.35F, 2.0F})
        for (const bool reversed : {false, true})
            for (const bool ping_pong : {false, true})
            {
                world::AnimationPlayerState clip;
                clip.duration = 1'000'000;
                clip.ticks = 950'000;
                clip.speed = speed;
                clip.reversed = reversed;
                clip.ping_pong = ping_pong;
                auto next = clip;
                next.update(world::animation_frame_ticks);
                const auto shown = clip.presented(1.0F);
                check(shown.sample_ticks() == next.sample_ticks() && shown.on_pong == next.on_pong,
                      "render interpolation meets the next tick at looping/reversed boundaries");
                check(clip.ticks == 950'000 && !clip.on_pong,
                      "render sampling leaves the authoritative clip untouched");
                clip.playing = false;
                check(clip.presented(0.5F).sample_ticks() == clip.sample_ticks(),
                      "paused pose clips do not drift during render interpolation");
            }
    for (const auto generation : {AnimationGeneration::classic, AnimationGeneration::modern})
    {
        PlayerAnimator animator{generation};
        world::PlayerAnimationInput input;
        input.mode = static_cast<std::uint8_t>(item_player_animation_mode(InventoryItemId::pistol));
        input.fps_mode = true;
        input.local = true;
        input.holding_gun = true;
        static_cast<void>(animator.update(input));
        animator.advance();
        input.shouldering = true;
        static_cast<void>(animator.update(input));
        animator.advance();
        const auto original = animator.describe();
        const auto upper_index = static_cast<std::size_t>(world::AnimationChannel::upper);
        const auto first = animator.pose()[upper_index].current.sample_ticks;
        auto next = animator;
        next.advance();
        const auto last = next.pose()[upper_index].current.sample_ticks;
        check(last > first, "the aim fixture advances its shoulder clip");
        for (const int fps : {60, 120, 144, 240})
        {
            std::int64_t previous = first;
            for (int frame = 1; frame <= 4; ++frame)
            {
                const float alpha = std::min(1.0F, static_cast<float>(frame) * 60.0F / fps);
                const auto shown = animator.presented(alpha);
                const auto sample = shown.pose()[upper_index].current.sample_ticks;
                check(sample >= previous && (alpha >= 1.0F || sample > previous),
                      "aim pose advances between fixed ticks at high display refresh rates");
                const auto expected = first + static_cast<std::int64_t>(std::llround((last - first) * alpha));
                check(std::abs(sample - expected) <= 1,
                      "aim pose uses a continuous clock between authoritative samples");
                previous = sample;
            }
        }
        check(animator.describe() == original,
              "presentation does not change firing/reload timing or the animation state machine");
        const auto end = animator.presented(1.0F).pose();
        const auto advanced = next.pose();
        for (std::size_t channel = 0; channel < end.size(); ++channel)
            check(end[channel].current.sample_ticks == advanced[channel].current.sample_ticks &&
                      end[channel].previous.sample_ticks == advanced[channel].previous.sample_ticks &&
                      std::abs(end[channel].weight - advanced[channel].weight) < 1e-6F,
                  "clip and crossfade clocks meet the next simulation tick together");
    }
}

void laser_guns_shoot()
{
    using Clip = world::AnimationClip;
    using Id = InventoryItemId;
    struct Case final
    {
        Id item;
        Clip idle, walk, shoot, shoulder, shoulder_shoot, reload;
    };
    const Case classic[]{
        {Id::iron_space_assault_rifle, Clip::LaserGunIdle, Clip::LaserGunRun, Clip::LaserGunShoot,
         Clip::LaserGunShoulder, Clip::LaserGunShoulderShoot, Clip::LaserGunReload},
        {Id::copper_space_assault_rifle, Clip::LaserGunIdle, Clip::LaserGunRun, Clip::LaserGunShoot,
         Clip::LaserGunShoulder, Clip::LaserGunShoulderShoot, Clip::LaserGunReload},
        {Id::gold_space_pistol, Clip::LaserPistolIdle, Clip::LaserPistolRun, Clip::LaserPistolShoot,
         Clip::LaserPistolShoulder, Clip::LaserPistolShoulderShoot, Clip::LaserPistolReload},
        {Id::diamond_space_pump_shotgun, Clip::LaserGunIdle, Clip::LaserGunRun,
         Clip::LaserShotgunShoot, Clip::LaserGunShoulder, Clip::LaserShotgunShoulderShoot,
         Clip::LaserShotgunReload},
        {Id::iron_space_bolt_action_rifle, Clip::LaserRifleIdle, Clip::LaserRifleRun,
         Clip::LaserRifleShoot, Clip::LaserRifleShoulder, Clip::LaserRifleShoulderShoot,
         Clip::LaserRifleReload},
        {Id::copper_space_smg, Clip::LaserSMGIdle, Clip::LaserSMGRun, Clip::LaserSMGShoot,
         Clip::LaserSMGShoulder, Clip::LaserSMGShoulderShoot, Clip::LaserSMGReload},
        {Id::assault_rifle, Clip::GunIdle, Clip::GunRun, Clip::GunShoot, Clip::GunShoulder,
         Clip::GunShoulderShoot, Clip::GunReload},
        {Id::pistol, Clip::PistolIdle, Clip::PistolWalk, Clip::PistolShoot, Clip::PistolShoulder,
         Clip::PistolShoulderShoot, Clip::PistolReload},
        {Id::bolt_action_rifle, Clip::RifleIdle, Clip::RifleWalk, Clip::RifleShoot,
         Clip::RifleShoulder, Clip::RifleShoulderShoot, Clip::RifleReload},
        {Id::smg, Clip::SMGIdle, Clip::SMGWalk, Clip::SMGShoot, Clip::SMGShoulder,
         Clip::SMGShoulderShoot, Clip::SMGReload},
        {Id::lmg, Clip::LMGIdle, Clip::LMGWalk, Clip::LMGShoot, Clip::LMGShoulder,
         Clip::LMGShoulderShoot, Clip::LMGReload},
        {Id::pump_shotgun, Clip::GunIdle, Clip::GunRun, Clip::PumpShotgunShoot, Clip::GunShoulder,
         Clip::PumpShotgunShoulderShoot, Clip::PumpShotgunReload},
        {Id::rocket_launcher, Clip::RPGIdle, Clip::RPGWalk, Clip::RPGShoot, Clip::GunShoulder,
         Clip::PumpShotgunShoulderShoot, Clip::PumpShotgunReload},
    };
    for (const AnimationGeneration generation :
         {AnimationGeneration::classic, AnimationGeneration::modern})
    {
        for (const Case &expected : classic)
        {
            const bool modern_shotgun = generation == AnimationGeneration::modern &&
                                        expected.item == Id::pump_shotgun;
            const std::string name = std::string{item_name(expected.item)} +
                                     (generation == AnimationGeneration::modern ? " (1.9.9.8)"
                                                                                : " (1.6.3.8)");
            check(gun_stats(expected.item) != nullptr && !item_uses_tool_input(expected.item) &&
                      !item_is_melee_weapon(expected.item),
                  name + " is a gun: its trigger never reaches the dig or melee path");
            const PlayerAnimationMode mode = item_player_animation_mode(expected.item);
            const world::AnimationClipSet hip = world::animation_clip_set(mode, false, generation);
            const world::AnimationClipSet aim = world::animation_clip_set(mode, true, generation);
            if (!modern_shotgun)
            {
                check(hip.idle == expected.idle && hip.walk == expected.walk,
                      name + " is held with its own idle and walk clips");
                check(hip.shoulder == expected.shoulder && hip.reload == expected.reload,
                      name + " shoulders and reloads with its own clips");
            }
            check(hip.use == expected.shoot && aim.use == expected.shoulder_shoot,
                  name + " fires with its own Shoot clip");
            for (const world::AnimationClipSet &set : {hip, aim})
                for (const Clip clip : {set.use, set.idle, set.walk, set.shoulder, set.reload})
                {
                    check(world::animation_clip_registered(clip, generation),
                          name + " plays a clip the shipped game registers: " +
                              std::string{world::animation_clip_info(clip).name});
                    check(clip != Clip::PickUse && clip != Clip::FistUse && clip != Clip::PickIdle &&
                              clip != Clip::GenericUse && clip != Clip::BlockUse,
                          name + " never borrows a melee or tool clip");
                    check(world::animation_clip_info(clip).influence ==
                              world::AnimationInfluence::back_upper,
                          name + " clips own the body from BackUpper down");
                }

            // One trigger pull, exactly as GunInventoryItem.ProcessInput raises
            // UsingTool for a single frame.
            PlayerAnimator animator{generation};
            world::PlayerAnimationInput input;
            input.mode = static_cast<std::uint8_t>(mode);
            input.fps_mode = true;
            input.local = true;
            input.holding_gun = true;
            input.reload_ticks = 30'000'000;
            for (int frame = 0; frame < 20; ++frame)
            {
                static_cast<void>(animator.update(input));
                animator.advance();
            }
            check(animator.channel(world::AnimationChannel::upper).current->clip == expected.idle ||
                      modern_shotgun,
                  name + " rests on its idle clip");
            check(!animator.channel(world::AnimationChannel::upper_use).current.has_value(),
                  name + " has nothing on the use channel at rest");
            input.using_tool = true;
            static_cast<void>(animator.update(input));
            animator.advance();
            input.using_tool = false;
            const auto &use = animator.channel(world::AnimationChannel::upper_use).current;
            check(use.has_value() && use->clip == expected.shoot,
                  name + " plays its Shoot clip the frame after the trigger");
            check(animator.using_animation_playing(), name + " blocks slot changes while it fires");
            int frames = 1;
            while (animator.channel(world::AnimationChannel::upper_use).current.has_value() &&
                   frames < 600)
            {
                static_cast<void>(animator.update(input));
                animator.advance();
                ++frames;
            }
            // The clip runs out once, at 17 ms a frame, and is then cleared.
            const std::int64_t duration = world::animation_clip_ticks(expected.shoot, generation);
            check(frames == static_cast<int>((duration + 169'999) / 170'000) + 1,
                  name + " fire clip lasts its authored length on the original's clock");
            check(!animator.using_animation_playing(), name + " releases the slot keys afterwards");
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    std::string replay;
    std::string summary;
    bool strict = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "--replay" && index + 1 < argc)
            replay = argv[++index];
        else if (argument == "--summary" && index + 1 < argc)
            summary = argv[++index];
        else if (argument == "--strict")
            strict = true;
    }
    if (!replay.empty())
        return replay_trace(replay, summary, strict);

    presentation_interpolates_without_advancing_gameplay();
    laser_guns_shoot();
    for (const animation_parity_pinned::Generation &generation : animation_parity_pinned::generations)
        replay_pinned(generation);
    if (failures != 0)
    {
        std::cerr << failures << " animation parity check(s) failed\n";
        return 1;
    }
    std::cout << "animation parity: pinned timelines of both generations match\n";
    return 0;
}
