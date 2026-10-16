// The in-world HUD of both retail builds, held to the originals.
//
// hud::layout() claims to be InGameHUD.OnDraw's arithmetic. This executable
// checks that claim against draw lists recorded from the shipped 1.6.3.8 and
// 1.9.9.8 assemblies by tools/OriginalClientInterop/HudParityProbe:
//
//   (no arguments)        the pinned test: for nine scripted player states at
//                         five window sizes, per build, the record of our
//                         layout must hash to the record of the original's
//                         (tests/hud_parity_pinned.hpp). Needs no game files.
//   --compare <dir>       the live test: compare line by line, number by
//                         number, with the probe's .tsv files in <dir>, and
//                         print the worst rectangle error per element kind.
//   --strings <c> <m>     write the (font, string) pairs the scenarios measure,
//                         for the probe's `measure` command.
//   --pin <dir> <c> <m> <header>
//                         regenerate hud_parity_pinned.hpp from the probe's
//                         records in <dir> and its two measurement files.
//
// The scenarios here are the probe's (Program.cs, Apply): keep them in step.

#include "openminerz/hud_layout.hpp"

#include "hud_parity_pinned.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;

namespace
{

void check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct WindowSize final
{
    int width{};
    int height{};
};
// 16:9 at three scales, 16:10 and 4:3.
constexpr std::array<WindowSize, 5> window_sizes{
    {{1280, 720}, {1920, 1080}, {2560, 1440}, {1920, 1200}, {1024, 768}}};
// ("dead" is recorded too but not compared: its only extra element is the
// respawn prompt, which the death screen draws, not hud::layout.)
constexpr std::array<std::string_view, 9> scenarios{
    "empty", "full", "hurt", "half", "day", "underwater", "endurance", "chat", "tray2"};

constexpr std::array<int, static_cast<std::size_t>(hud::Font::count)> classic_line_spacing{
    19, 29, 39, 58, 25};
constexpr std::array<int, static_cast<std::size_t>(hud::Font::count)> modern_line_spacing{
    21, 32, 40, 64, 25};

// SpriteFont.MeasureString as the original answered for these strings.
class RecordedMetrics final : public hud::TextMetrics
{
  public:
    explicit RecordedMetrics(bool modern) : modern_(modern) {}

    [[nodiscard]] hud::Extent measure(hud::Font font, std::string_view text) const override
    {
        requested_.emplace_back(font, std::string{text});
        for (const PinnedMeasure &entry : pinned_measures)
            if (entry.modern == modern_ && entry.font == font && entry.text == text)
                return {entry.width, entry.height};
        if (collecting_)
            return {};
        throw std::runtime_error(std::format("no recorded measurement for {} \"{}\" ({})",
                                             hud::font_asset_name(font), text,
                                             modern_ ? "modern" : "classic"));
    }
    [[nodiscard]] int line_spacing(hud::Font font) const override
    {
        return (modern_ ? modern_line_spacing
                        : classic_line_spacing)[static_cast<std::size_t>(font)];
    }

    void collect() noexcept { collecting_ = true; }
    [[nodiscard]] const std::vector<std::pair<hud::Font, std::string>> &requested() const noexcept
    {
        return requested_;
    }

  private:
    bool modern_{};
    bool collecting_{};
    mutable std::vector<std::pair<hud::Font, std::string>> requested_;
};

// The probe's StandardTray (InventoryItemIDs values are the same in both builds).
void standard_tray(hud::State &state)
{
    state.tray[0] = {true, 23, 1, true, 0.5F};   // StonePickAxe, half worn
    state.tray[1] = {true, 1, 64, false, 1.0F};  // DirtBlock x64
    state.tray[2] = {true, 14, 7, false, 1.0F};  // Torch x7
    state.tray[3] = {true, 51, 1, true, 1.0F};   // Pistol
    state.tray[4] = {true, 44, 250, false, 1.0F}; // Bullets x250
    state.tray[6] = {true, 31, 1, true, 0.92F};  // IronSpade
    state.tray[7] = {true, 3, 2, false, 1.0F};   // RockBlock x2
    state.item_text = state.item_name = "Stone PickAxe";
}

[[nodiscard]] hud::State scenario_state(bool modern, std::string_view scenario)
{
    hud::State state;
    // The probe runs Survival: 1.9.9.8 shows stamina and its second tray.
    state.has_stamina = modern;
    state.has_next_tray = modern;
    state.next_tray_alpha = 1.0F; // PlayerStats.SecondTrayFaded = false
    state.item_text = state.item_name = "Bare Hands";
    state.day_text = "Day 1";
    // A tool's reticle: half a degree over the 73 degree FPS camera.
    state.reticle_fraction = 0.00684931455F;
    if (scenario == "empty")
        return state;
    standard_tray(state);
    if (scenario == "full")
    {
        state.distance = 137;
        state.max_distance = 2048;
    }
    else if (scenario == "hurt")
    {
        state.selected = 3;
        state.item_text = "Pistol 5/0";
        state.item_name = "Pistol";
        state.health = 0.35F;
        state.stamina = 0.6F;
        state.inaccuracy = 0.5F;
        state.reticle_fraction = 0.02568493F;
        state.distance = 12;
        state.max_distance = 345;
        state.aiming_at_block = true;
        state.block_name = "Gold Ore";
        // Sources at +X and +Z of a player facing -Z, 0.5 s and 2.5 s old.
        state.damage.push_back({std::atan2(10.0F, -0.0F), 1.0F});
        state.damage.push_back({std::atan2(0.0F, -10.0F), (1.0F - 2.5F / 3.0F) * 3.0F});
    }
    else if (scenario == "half")
    {
        state.selected = 1;
        state.item_text = state.item_name = "Dirt";
        state.health = 0.5F;
        state.stamina = 0.25F;
        state.stamina_blocked = true;
        state.aiming_at_block = true;
        state.block_name = "Dirt";
    }
    else if (scenario == "day")
    {
        state.day_text = "Day 12";
        state.day_banner_seconds = 1.5F;
    }
    else if (scenario == "underwater")
    {
        state.underwater = true;
        state.oxygen = 0.6F;
    }
    else if (scenario == "endurance")
    {
        state.has_stamina = false; // IsEnduranceMode: health alone, centred
        state.health = 0.8F;
    }
    else if (scenario == "chat")
    {
        state.console = {{"Kiril has joined the game", 1.0F},
                         {"Kiril: hello there", 1.0F},
                         {"Todor Has Fallen", 1.0F}};
    }
    else if (scenario == "tray2")
    {
        state.selected = 7;
        state.item_text = state.item_name = "Rock";
        if (modern)
        {
            state.next_tray[0] = {true, 14, 12, false, 1.0F}; // Torch x12
            state.next_tray[5] = {true, 4, 30, false, 1.0F};  // LogBlock x30
        }
    }
    else
        throw std::runtime_error("unknown scenario " + std::string{scenario});
    return state;
}

[[nodiscard]] std::string our_record(bool modern, WindowSize size, std::string_view scenario,
                                     const hud::TextMetrics &metrics)
{
    const UiStyle style = modern ? UiStyle::modern : UiStyle::classic;
    return hud::record(hud::layout(style, scenario_state(modern, scenario), size.width,
                                   size.height, metrics));
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view text) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : text)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::vector<std::string> split(std::string_view text, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0U;
    while (true)
    {
        const std::size_t end = text.find(separator, start);
        parts.emplace_back(text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos)
            break;
        start = end + 1U;
    }
    return parts;
}

// A probe record as written, normalized to what hud::record() prints: no
// carriage returns, and no reticle quotient on the header line.
[[nodiscard]] std::string read_retail_record(const std::filesystem::path &path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::erase(text, '\r');
    if (text.starts_with("\xEF\xBB\xBF"))
        text.erase(0U, 3U);
    const std::size_t line_end = text.find('\n');
    std::vector<std::string> header = split(std::string_view{text}.substr(0U, line_end), '\t');
    if (header.size() > 7U)
        header.resize(7U);
    std::string first;
    for (std::size_t index = 0U; index < header.size(); ++index)
        first += (index == 0U ? "" : "\t") + header[index];
    return first + text.substr(line_end);
}

[[nodiscard]] std::string record_name(bool modern, WindowSize size, std::string_view scenario)
{
    return std::format("{}-{}x{}-{}.tsv", modern ? "modern" : "classic", size.width, size.height,
                       scenario);
}

// ---------------------------------------------------------------------------

void pinned_records_match_the_original()
{
    std::size_t compared{};
    for (const bool modern : {false, true})
    {
        const RecordedMetrics metrics{modern};
        for (const WindowSize size : window_sizes)
            for (const std::string_view scenario : scenarios)
            {
                const std::string ours = our_record(modern, size, scenario, metrics);
                const auto pinned = std::find_if(
                    std::begin(pinned_records), std::end(pinned_records),
                    [&](const PinnedRecord &entry) {
                        return entry.modern == modern && entry.width == size.width &&
                               entry.height == size.height && entry.scenario == scenario;
                    });
                const std::string name = record_name(modern, size, scenario);
                check(pinned != std::end(pinned_records), "no pinned record for " + name);
                const auto lines = static_cast<int>(std::count(ours.begin(), ours.end(), '\n'));
                if (fnv1a(ours) != pinned->hash || lines != pinned->lines)
                    throw std::runtime_error(std::format(
                        "{}: our draw list ({} lines) is not the original's ({} lines). Run "
                        "openminerz_original_hud_parity for the differing element. Ours:\n{}",
                        name, lines, pinned->lines, ours));
                ++compared;
            }
    }
    check(compared == 2U * window_sizes.size() * scenarios.size(), "every record was compared");
}

void classic_canvas_is_1280x720_letterboxed()
{
    const RecordedMetrics metrics{false};
    const hud::State state = scenario_state(false, "full");
    for (const auto &[size, present] :
         {std::pair{WindowSize{1280, 720}, hud::IntRect{0, 0, 1280, 720}},
          std::pair{WindowSize{1920, 1080}, hud::IntRect{0, 0, 1920, 1080}},
          std::pair{WindowSize{1920, 1200}, hud::IntRect{0, 60, 1920, 1080}},
          std::pair{WindowSize{1024, 768}, hud::IntRect{0, 96, 1024, 576}},
          std::pair{WindowSize{3440, 1440}, hud::IntRect{440, 0, 2560, 1440}}})
    {
        const hud::Frame frame = hud::layout(UiStyle::classic, state, size.width, size.height, metrics);
        check(frame.canvas_width == 1280 && frame.canvas_height == 720,
              "1.6.3.8 always lays out for its 1280x720 offscreen buffer");
        check(frame.present.x == present.x && frame.present.y == present.y &&
                  frame.present.width == present.width && frame.present.height == present.height,
              std::format("{}x{}: the buffer is centred with its aspect kept", size.width,
                          size.height));
        const hud::Rect tray = hud::to_window(frame, {398.0F, 648.0F, 485.0F, 72.0F});
        const float scale = hud::window_scale(frame);
        check(std::abs(tray.x - (static_cast<float>(present.x) + 398.0F * scale)) < 0.001F &&
                  std::abs(tray.y + tray.height -
                           static_cast<float>(present.y + present.height)) < 0.001F,
              "the tray sits on the bottom edge of the 16:9 picture");
    }
    const hud::Frame modern = hud::layout(UiStyle::modern, scenario_state(true, "full"), 1920, 1200,
                                          RecordedMetrics{true});
    check(modern.canvas_width == 1920 && modern.canvas_height == 1200 && modern.present.y == 0 &&
              modern.present.height == 1200 && hud::window_scale(modern) == 1.0F,
          "1.9.9.8 lays out for the real back buffer");
}

void the_style_never_reads_more_than_the_world_has()
{
    // A 1.6.3.8 world (no stamina, one tray) shown with the Modern widgets.
    const RecordedMetrics metrics{true};
    hud::State classic_world = scenario_state(true, "full");
    classic_world.has_stamina = false;
    classic_world.has_next_tray = false;
    const hud::Frame frame = hud::layout(UiStyle::modern, classic_world, 1280, 720, metrics);
    int grids{}, stamina{}, health{};
    for (const hud::Draw &draw : frame.draws)
    {
        if (draw.kind != hud::Draw::Kind::sprite)
            continue;
        grids += draw.sprite == hud::Sprite::hud_grid;
        stamina += draw.sprite == hud::Sprite::stamina_empty;
        health += draw.sprite == hud::Sprite::health_empty;
    }
    check(grids == 1 && stamina == 0 && health == 1,
          "a world without stamina or a second tray gets neither meter nor strip");

    // A 1.9.9.8 world shown with the Classic widgets: 1.6.3.8 has no stamina
    // meter and no second strip to draw, and the layout must not invent them.
    const RecordedMetrics classic_metrics{false};
    hud::State modern_world = scenario_state(false, "full");
    modern_world.has_stamina = true;
    modern_world.has_next_tray = true;
    modern_world.next_tray[2] = {true, 14, 12, false, 1.0F};
    const hud::Frame classic = hud::layout(UiStyle::classic, modern_world, 1280, 720, classic_metrics);
    check(hud::record(classic) == our_record(false, {1280, 720}, "full", classic_metrics),
          "the Classic HUD is the same for a world that also has stamina and a second tray");
}

void hide_switches_only_remove_elements()
{
    for (const bool modern : {false, true})
    {
        const RecordedMetrics metrics{modern};
        const UiStyle style = modern ? UiStyle::modern : UiStyle::classic;
        hud::State state = scenario_state(modern, "hurt");
        state.underwater = true;
        state.oxygen = 0.4F;
        state.console = {{"hello", 1.0F}};
        const hud::Frame full = hud::layout(style, state, 1920, 1080, metrics);
        const auto lines = [](const hud::Frame &frame) { return split(hud::record(frame), '\n'); };
        const std::vector<std::string> all = lines(full);
        const std::array<bool hud::State::*, 9> switches{
            &hud::State::show_vitals,     &hud::State::show_oxygen,   &hud::State::show_tray,
            &hud::State::show_item_text,  &hud::State::show_block_name, &hud::State::show_distance,
            &hud::State::show_crosshair,  &hud::State::show_damage,   &hud::State::show_console};
        for (const auto member : switches)
        {
            hud::State hidden = state;
            hidden.*member = false;
            const std::vector<std::string> fewer = lines(hud::layout(style, hidden, 1920, 1080, metrics));
            check(fewer.size() < all.size(), "a hide switch must remove something");
            // What remains is a subsequence of the full list: nothing moved.
            std::size_t cursor = 0U;
            for (const std::string &line : fewer)
            {
                while (cursor < all.size() && all[cursor] != line)
                    ++cursor;
                check(cursor < all.size(), "hiding one element moved another: " + line);
                ++cursor;
            }
        }
    }
}

void xna_colour_arithmetic()
{
    using hud::Color;
    check(hud::color_scale({53U, 170U, 253U, 255U}, 0.75F) == Color{39U, 127U, 189U, 191U},
          "CMZColors.MenuAqua * 0.75f truncates each channel");
    check(hud::color_scale({39U, 127U, 189U, 191U}, 0.5F) == Color{19U, 63U, 94U, 95U},
          "the Day banner at half fade");
    check(hud::color_from_floats(0.5F, 0.5F, 0.5F, 0.5F) == Color{128U, 128U, 128U, 128U} &&
              hud::color_from_floats(1.0F, 1.0F, 1.0F, 0.4F) == Color{255U, 255U, 255U, 102U} &&
              hud::color_from_floats(2.0F, -1.0F, 0.75F, 0.75F) == Color{255U, 0U, 191U, 191U},
          "new Color(float...) clamps and rounds half to even");
    check(hud::color_lerp({0U, 0U, 0U, 0U}, {255U, 0U, 0U, 255U}, 0.5F) == Color{127U, 0U, 0U, 127U} &&
              hud::color_lerp({0U, 0U, 0U, 0U}, {255U, 255U, 255U, 255U}, 1.0F) ==
                  Color{255U, 255U, 255U, 255U} &&
              hud::color_lerp({0U, 0U, 0U, 0U}, {0U, 0U, 0U, 255U}, 0.0F) == Color{0U, 0U, 0U, 0U},
          "Color.Lerp is 16.16 fixed point");
}

void sprite_tables_cover_both_builds()
{
    for (int index = 0; index < static_cast<int>(hud::Sprite::count); ++index)
    {
        const auto sprite = static_cast<hud::Sprite>(index);
        const hud::Size classic = hud::sprite_size(UiStyle::classic, sprite);
        const hud::Size modern = hud::sprite_size(UiStyle::modern, sprite);
        check(modern.width > 0 && modern.height > 0, "1.9.9.8 has every HUD sprite");
        const bool modern_only =
            sprite == hud::Sprite::crosshair_tick || sprite == hud::Sprite::stamina_empty;
        check((classic.width == 0) == modern_only, "only the tick and the stamina icon are new");
        if (sprite != hud::Sprite::solid && sprite != hud::Sprite::item_icon)
            check(!hud::sprite_sheet_name(sprite).empty(), "every sheet sprite has its sheet name");
    }
    check(hud::sprite_size(UiStyle::classic, hud::Sprite::hud_grid).width == 485 &&
              hud::sprite_size(UiStyle::modern, hud::Sprite::hud_grid).width == 481 &&
              hud::sprite_size(UiStyle::classic, hud::Sprite::health_full).width == 246 &&
              hud::sprite_size(UiStyle::modern, hud::Sprite::health_full).width == 122,
          "the two builds' sheets differ; each style uses its own");
    check(hud::font_asset_name(hud::Font::medium) == "MedFont" &&
              hud::font_asset_name(hud::Font::console) == "ConsoleFont",
          "fonts are named as the content names them");
}

void player_list_replaces_the_hud_body()
{
    for (const bool modern : {false, true})
    {
        const RecordedMetrics metrics{modern};
        hud::State state = scenario_state(modern, "full");
        state.console = {{"hello", 1.0F}};
        state.show_players = true;
        state.max_players = 8;
        state.roster = {{"Kiril", true}, {"Todor", false}};
        RecordedMetrics collecting{modern};
        collecting.collect();
        const hud::Frame frame = hud::layout(modern ? UiStyle::modern : UiStyle::classic, state,
                                             1280, 720, collecting);
        int sprites{}, texts{};
        bool local_red{}, header_orange{};
        for (const hud::Draw &draw : frame.draws)
        {
            sprites += draw.kind == hud::Draw::Kind::sprite;
            texts += draw.kind == hud::Draw::Kind::text;
            local_red |= draw.text == "Kiril" && draw.color == hud::Color{255U, 0U, 0U, 255U};
            header_orange |= draw.text == "Player" && draw.color == hud::Color{255U, 165U, 0U, 255U};
        }
        // console line + "Players 2/8" + "Player" + two gamertags, five draws each.
        check(sprites == 0 && texts == 25 && local_red && header_orange,
              "DrawPlayerList shows the console and the list, nothing else");
    }
}

// ---------------------------------------------------------------------------

struct Comparison final
{
    std::size_t records{};
    std::size_t elements{};
    std::size_t mismatched_records{};
    double worst_position{};
    double worst_size{};
    std::map<std::string, double> worst_by_kind;
};

void compare_record(const std::string &name, const std::string &ours, const std::string &theirs,
                    Comparison &result)
{
    const std::vector<std::string> a = split(ours, '\n');
    const std::vector<std::string> b = split(theirs, '\n');
    ++result.records;
    bool reported = false;
    const auto report = [&](const std::string &what) {
        if (!reported)
            ++result.mismatched_records;
        reported = true;
        std::cerr << "[DIFF] " << name << ": " << what << '\n';
    };
    if (a.size() != b.size())
        report(std::format("{} draws, the original has {}", a.size(), b.size()));
    for (std::size_t line = 0U; line < std::min(a.size(), b.size()); ++line)
    {
        if (a[line].empty() && b[line].empty())
            continue;
        const std::vector<std::string> x = split(a[line], '\t');
        const std::vector<std::string> y = split(b[line], '\t');
        if (x.size() != y.size() || x[0] != y[0])
        {
            report(std::format("line {}: ours `{}` original `{}`", line + 1U, a[line], b[line]));
            continue;
        }
        ++result.elements;
        // Columns holding a position or size, per record kind.
        const bool sprite = x[0] == "S";
        const std::size_t first_number = sprite ? 7U : 2U;
        const std::size_t last_number = sprite ? 10U : 5U;
        const std::string kind = x[0] == "C" ? "canvas" : (sprite ? (x[2] == "-" ? x[1] : x[2]) : x[1]);
        for (std::size_t column = 1U; column < x.size(); ++column)
        {
            if (x[column] == y[column])
                continue;
            if (x[0] != "C" && column >= first_number && column <= last_number)
            {
                const double error = std::abs(std::stod(x[column]) - std::stod(y[column]));
                double &worst = result.worst_by_kind[kind];
                worst = std::max(worst, error);
                if (!sprite || column < 9U)
                    result.worst_position = std::max(result.worst_position, error);
                else
                    result.worst_size = std::max(result.worst_size, error);
            }
            report(std::format("line {} column {}: ours `{}` original `{}`\n       ours     {}\n"
                               "       original {}",
                               line + 1U, column + 1U, x[column], y[column], a[line], b[line]));
            break;
        }
        result.worst_by_kind.try_emplace(kind, 0.0);
    }
}

int compare(const std::filesystem::path &directory)
{
    Comparison result;
    for (const bool modern : {false, true})
    {
        const RecordedMetrics metrics{modern};
        for (const WindowSize size : window_sizes)
            for (const std::string_view scenario : scenarios)
            {
                const std::string name = record_name(modern, size, scenario);
                compare_record(name, our_record(modern, size, scenario, metrics),
                               read_retail_record(directory / name), result);
            }
    }
    std::cout << std::format("{} records, {} elements compared; {} records differ\n", result.records,
                             result.elements, result.mismatched_records);
    std::cout << std::format("worst position error {} px, worst size error {} px\n",
                             result.worst_position, result.worst_size);
    for (const auto &[kind, worst] : result.worst_by_kind)
        std::cout << std::format("  {:<16} {} px\n", kind, worst);
    return result.mismatched_records == 0U ? 0 : 1;
}

int write_strings(const std::filesystem::path &classic, const std::filesystem::path &modern_path)
{
    for (const bool modern : {false, true})
    {
        RecordedMetrics metrics{modern};
        metrics.collect();
        for (const std::string_view scenario : scenarios)
            (void)our_record(modern, {1280, 720}, scenario, metrics);
        std::vector<std::pair<hud::Font, std::string>> wanted = metrics.requested();
        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
        std::ofstream output{modern ? modern_path : classic, std::ios::binary};
        for (const auto &[font, text] : wanted)
            output << hud::font_asset_name(font) << '\t' << text << '\n';
    }
    return 0;
}

int pin(const std::filesystem::path &directory, const std::filesystem::path &classic,
        const std::filesystem::path &modern_path, const std::filesystem::path &header)
{
    std::ostringstream out;
    out << "// GENERATED by `openminerz_hud_layout_tests --pin` from the draw lists and\n"
           "// SpriteFont measurements tools/OriginalClientInterop/HudParityProbe recorded from\n"
           "// the shipped CastleMiner Z 1.6.3.8 and 1.9.9.8 assemblies. Do not edit by hand:\n"
           "// rerun tools/OriginalClientInterop/HudParityProbe/Test-HudParity.ps1 -Pin.\n"
           "#pragma once\n\n#include \"openminerz/hud_layout.hpp\"\n\n#include <cstdint>\n"
           "#include <string_view>\n\n"
           "struct PinnedMeasure final\n{\n    bool modern;\n    openminerz::hud::Font font;\n"
           "    std::string_view text;\n    float width;\n    float height;\n};\n\n"
           "// SpriteFont.MeasureString(text) of that build's font.\n"
           "inline constexpr PinnedMeasure pinned_measures[]{\n";
    for (const bool modern : {false, true})
    {
        std::ifstream input{modern ? modern_path : classic, std::ios::binary};
        std::string line;
        while (std::getline(input, line))
        {
            std::erase(line, '\r');
            const std::vector<std::string> fields = split(line, '\t');
            if (fields.size() < 4U)
                continue;
            int font = -1;
            for (int index = 0; index < static_cast<int>(hud::Font::count); ++index)
                if (hud::font_asset_name(static_cast<hud::Font>(index)) == fields[0])
                    font = index;
            check(font >= 0, "unknown font in measurement file: " + fields[0]);
            out << std::format("    {{{}, static_cast<openminerz::hud::Font>({}), \"{}\", {}F, {}F}},\n",
                               modern ? "true" : "false", font, fields[3],
                               std::format("{:.1f}", std::stod(fields[1])),
                               std::format("{:.1f}", std::stod(fields[2])));
        }
    }
    out << "};\n\nstruct PinnedRecord final\n{\n    bool modern;\n    int width;\n    int height;\n"
           "    std::string_view scenario;\n    // FNV-1a 64 of the record text, and its line count.\n"
           "    std::uint64_t hash;\n    int lines;\n};\n\n"
           "inline constexpr PinnedRecord pinned_records[]{\n";
    for (const bool modern : {false, true})
        for (const WindowSize size : window_sizes)
            for (const std::string_view scenario : scenarios)
            {
                const std::string text =
                    read_retail_record(directory / record_name(modern, size, scenario));
                out << std::format("    {{{}, {}, {}, \"{}\", 0x{:016X}ULL, {}}},\n",
                                   modern ? "true" : "false", size.width, size.height, scenario,
                                   fnv1a(text), std::count(text.begin(), text.end(), '\n'));
            }
    out << "};\n";
    std::ofstream output{header, std::ios::binary};
    output << out.str();
    return output ? 0 : 1;
}

struct TestCase final
{
    const char *name;
    void (*body)();
};

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        if (mode == "--compare" && argc == 3)
            return compare(argv[2]);
        if (mode == "--strings" && argc == 4)
            return write_strings(argv[2], argv[3]);
        if (mode == "--pin" && argc == 6)
            return pin(argv[2], argv[3], argv[4], argv[5]);
        if (!mode.empty())
        {
            std::cerr << "Usage: openminerz_hud_layout_tests [--compare <dir> | --strings <classic> "
                         "<modern> | --pin <dir> <classic> <modern> <header>]\n";
            return 2;
        }
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }

    const std::vector<TestCase> tests{
        {"pinned_records_match_the_original", pinned_records_match_the_original},
        {"classic_canvas_is_1280x720_letterboxed", classic_canvas_is_1280x720_letterboxed},
        {"the_style_never_reads_more_than_the_world_has",
         the_style_never_reads_more_than_the_world_has},
        {"hide_switches_only_remove_elements", hide_switches_only_remove_elements},
        {"xna_colour_arithmetic", xna_colour_arithmetic},
        {"sprite_tables_cover_both_builds", sprite_tables_cover_both_builds},
        {"player_list_replaces_the_hud_body", player_list_replaces_the_hud_body},
    };
    std::size_t failures{};
    for (const auto &test : tests)
    {
        try
        {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
