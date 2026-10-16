// The inventory, crate and crafting screens against the shipped ones.
//
//   (no arguments)        the pinned test: every recorded scenario (five
//                         back-buffer sizes, both builds) must produce the
//                         recorded draw list, and every recorded input script
//                         the recorded inventory after each step
//                         (tests/screens_parity_pinned.hpp). Needs no game files.
//   --compare DIR         the same against a fresh recording: DIR/classic and
//                         DIR/modern as written by ScreensParityProbe. Prints
//                         the differing records and the worst rectangle error.
//   --dump DIR OUT        write our draw lists and state lines for DIR's
//                         scenarios under OUT, for a line-by-line diff.
//   --pin DIR HEADER      regenerate screens_parity_pinned.hpp from DIR.
//
// A scenario is replayed the way the probe drove the original: one
// Session::update per game frame (17 ms, XNA's TimeSpan-rounded 60 Hz step), a
// click as a press frame and a release frame, a key as two frames.

#include "openminerz/screens.hpp"

#include "screens_parity_pinned.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;
namespace fs = std::filesystem;

namespace
{

int failures = 0;

void check(bool condition, const std::string &message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::uint32_t fnv1a(std::string_view text) noexcept
{
    std::uint32_t hash = 2166136261U;
    for (const char c : text)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619U;
    }
    return hash;
}

std::vector<std::string> split(std::string_view text, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos)
        {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1U;
    }
    return parts;
}

std::string number(float value)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(value));
    std::string text{buffer};
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

// ---------------------------------------------------------------- metrics

struct Measure final
{
    std::string font;
    std::string text;
    float width{};
    float height{};
};

class RecordedMetrics final : public screens::TextMetrics
{
  public:
    RecordedMetrics(bool modern, std::function<const Measure *(bool, std::string_view, std::string_view)> lookup)
        : modern_(modern), lookup_(std::move(lookup))
    {
    }

    [[nodiscard]] screens::Extent measure(screens::Font font, std::string_view text) const override
    {
        if (text.empty())
            return {};
        if (const Measure *found = lookup_(modern_, screens::font_asset_name(font), text))
            return {found->width, found->height};
        missing.emplace_back(std::string{screens::font_asset_name(font)} + " \"" + std::string{text} + "\"");
        return {};
    }

    [[nodiscard]] int line_spacing(screens::Font font) const override
    {
        // SpriteFont.LineSpacing of SmallFont, MedFont and MyriadMedium.
        static constexpr std::array<int, 3> classic{19, 29, 31};
        static constexpr std::array<int, 3> modern{21, 32, 31};
        return (modern_ ? modern : classic)[static_cast<std::size_t>(font)];
    }

    mutable std::vector<std::string> missing;

  private:
    bool modern_{};
    std::function<const Measure *(bool, std::string_view, std::string_view)> lookup_;
};

// ------------------------------------------------------------------- host

std::string item_text(const PlayerInventory::Slot &slot, bool modern)
{
    if (!slot.has_value())
        return "-";
    std::string text =
        std::to_string(screens::retail_icon(slot->item, modern ? UiStyle::modern : UiStyle::classic)) + "x" +
        std::to_string(slot->count);
    if (slot->durability != 1.0F)
        text += "@" + number(slot->durability);
    return text;
}

class TestHost final : public screens::Host
{
  public:
    explicit TestHost(bool modern) : modern_(modern)
    {
        inventory_.set_profile(modern ? InventoryProfile::modern : InventoryProfile::classic);
        for (auto &slot : inventory_.storage())
            slot.reset();
        for (auto &tray : inventory_.trays())
            for (auto &slot : tray)
                slot.reset();
    }

    [[nodiscard]] PlayerInventory &inventory() noexcept override { return inventory_; }
    [[nodiscard]] const PlayerInventory &inventory() const noexcept override { return inventory_; }
    [[nodiscard]] PlayerInventory::Slot *crate_slot(std::size_t index) noexcept override
    {
        return crate_open && index < crate.size() ? &crate[index] : nullptr;
    }
    [[nodiscard]] const PlayerInventory::Slot *crate_slot(std::size_t index) const noexcept override
    {
        return crate_open && index < crate.size() ? &crate[index] : nullptr;
    }
    [[nodiscard]] bool crate_slot_locked(std::size_t index) const noexcept override
    {
        return locked == static_cast<int>(index);
    }
    [[nodiscard]] bool can_craft(const CraftingRecipe &recipe) const noexcept override
    {
        if (!recipe_listed(recipe))
            return false;
        // Only 1.9.9.8's CanCraft begins with the Creative bypass.
        if (modern_ && creative_mode)
            return true;
        return inventory_.can_craft(recipe);
    }
    void craft(std::size_t recipe_index, bool all) override
    {
        const CraftingRecipe &recipe = crafting_recipes(inventory_.profile())[recipe_index];
        if (infinite)
        {
            // PlayerInventory.Craft in InfiniteResourceMode adds the result and
            // takes nothing.
            const std::uint16_t rest = inventory_.add_item(recipe.result, recipe.result_count);
            if (rest != 0U)
                drop(InventoryStack{recipe.result, rest});
            return;
        }
        do
        {
            const std::uint16_t rest = inventory_.craft(recipe);
            if (rest != 0U)
                drop(InventoryStack{recipe.result, rest});
        } while (all && can_craft(recipe));
    }
    [[nodiscard]] bool creative() const noexcept override { return creative_mode; }
    void play(screens::Cue cue) override { events.push_back("sound " + std::string{screens::cue_name(cue)}); }
    void drop(const InventoryStack &stack) override
    {
        events.push_back("pickup " + item_text(PlayerInventory::Slot{stack}, modern_));
    }
    void crate_changed(std::size_t) override {}
    // CrateFocusMessage.Send as the probe records it: "focus <crate location
    // or 0,0,0> <x>,<y>". The probe's crate stands at (7, 12, -4).
    void crate_focus(screens::CrateFocusCause cause, std::size_t selected) override
    {
        const auto focus = screens::crate_focus_announcement(cause, selected, modern_);
        if (!focus.has_value())
            return;
        focus_events.push_back(std::string{"focus "} + (focus->in_crate ? "7,12,-4" : "0,0,0") + " " +
                               std::to_string(focus->x) + "," + std::to_string(focus->y));
    }

    std::array<PlayerInventory::Slot, 32> crate{};
    bool crate_open{};
    int locked{-1};
    bool creative_mode{};
    bool infinite{};
    std::vector<std::string> events;
    // Kept apart from `events`: the recorded states and their pinned hashes
    // hold sounds and pickups only.
    std::vector<std::string> focus_events;

  private:
    bool modern_{};
    PlayerInventory inventory_;
};

// ---------------------------------------------------------------- replay

// One game frame of the probe: TimeSpan.FromSeconds(1 / 60.0) is 17 ms.
constexpr float frame_seconds = 0.017F;

class Replay final
{
  public:
    Replay(bool modern, int width, int height, const screens::TextMetrics &metrics)
        : modern_(modern), host(modern), metrics_(metrics)
    {
        const UiStyle style = modern ? UiStyle::modern : UiStyle::classic;
        session.configure(style, width, height);
        height_ = height;
    }

    void give(std::string_view fill)
    {
        for (const std::string &entry : split(fill, ' '))
        {
            if (entry.empty())
                continue;
            const auto parts = split(entry, '=');
            const auto index = static_cast<std::size_t>(std::stoul(parts[0]));
            std::string item = parts[1];
            float health = 1.0F;
            if (const auto at = item.find('@'); at != std::string::npos)
            {
                health = std::stof(item.substr(at + 1U));
                item.resize(at);
            }
            const auto fields = split(item, 'x');
            InventoryStack stack{static_cast<InventoryItemId>(std::stoi(fields[0])),
                                 static_cast<std::uint16_t>(std::stoi(fields[1]))};
            stack.durability = health;
            if (index >= screens::crate_base)
                host.crate[index - screens::crate_base] = stack;
            else if (index < screens::backpack_slots)
                host.inventory().storage()[index] = stack;
            else
                host.inventory().tray((index - screens::tray_base) / 8U)[(index - screens::tray_base) % 8U] = stack;
        }
    }

    void open(std::string_view screen)
    {
        opened_ = std::string{screen};
        host.crate_open = screen == "crate";
        session.open(host, screen == "crate"      ? screens::ScreenId::crate
                           : screen == "crafting" ? screens::ScreenId::crafting
                                                  : screens::ScreenId::inventory);
        frame();
    }

    void frame(screens::HeldKeys held = {}) { session.update(host, frame_seconds, held); }

    void point(const std::string &target)
    {
        screens::Point layout{};
        const screens::IntRect back = session.panel();
        if (!target.empty() && std::isdigit(static_cast<unsigned char>(target[0])) != 0 &&
            target.find(',') == std::string::npos)
            layout = session.slot_rect(static_cast<std::size_t>(std::stoul(target))).value_or(screens::IntRect{}).center();
        else if (target == "out")
            layout = {back.x - 40, back.y + 60};
        else if (target == "panel")
            layout = {back.x + 12, back.bottom() - 12};
        else if (target == "close")
            layout = session.close_rect().value_or(screens::IntRect{}).center();
        else if (target == "button")
            layout = session.button_rect().value_or(screens::IntRect{}).center();
        else if (target.rfind("recipe", 0) == 0)
        {
            const int index = std::stoi(target.substr(6));
            layout = (modern_ ? session.recipe_rect(host, static_cast<std::size_t>(index)) : session.strip_rect(index))
                         .value_or(screens::IntRect{})
                         .center();
        }
        else if (target.rfind("ingredient", 0) == 0)
        {
            const int index = std::stoi(target.substr(10));
            if (modern_)
            {
                const float scale = static_cast<float>(height_) / 720.0F;
                layout = {static_cast<int>(static_cast<float>(back.x) + static_cast<float>(39 + 65 * index + 30) * scale),
                          static_cast<int>(static_cast<float>(back.y) + static_cast<float>(521 + 30) * scale)};
            }
            else
            {
                layout = session.ingredient_rect(host, index).value_or(screens::IntRect{}).center();
            }
        }
        else if (target.rfind("tier1:", 0) == 0)
            layout = session.group_rect(static_cast<std::size_t>(std::stoul(target.substr(6)))).value_or(screens::IntRect{}).center();
        else if (target.rfind("tier2:", 0) == 0)
            layout = session.type_rect(static_cast<std::size_t>(std::stoul(target.substr(6)))).value_or(screens::IntRect{}).center();
        else
        {
            const auto parts = split(target, ',');
            layout = {std::stoi(parts[0]), std::stoi(parts[1])};
        }
        const screens::Point window = session.to_window(layout);
        session.pointer_moved(host, window.x, window.y);
    }

    void act(const std::string &action)
    {
        const auto words = split(action, ' ');
        if (words[0] == "move")
        {
            point(words[1]);
            frame();
        }
        else if (words[0] == "click")
        {
            const bool shift = words.size() > 2U && words[2] == "shift";
            const screens::Button button = words[1] == "left" ? screens::Button::left : screens::Button::right;
            session.button_pressed(host, button, shift);
            frame();
            session.button_released(host, button);
            frame();
            if (shift)
                frame();
        }
        else if (words[0] == "wheel")
        {
            session.wheel(host, words[1] == "up" ? 1 : -1);
            frame();
            frame();
        }
        else if (words[0] == "key")
        {
            screens::HeldKeys held;
            screens::Key key = screens::Key::escape;
            if (words[1] == "E")
                key = screens::Key::inventory;
            else if (words[1] == "Q")
                key = screens::Key::drop;
            else if (words[1] == "Enter")
                key = screens::Key::enter;
            else if (words[1] == "Up")
                key = screens::Key::up, held.up = true;
            else if (words[1] == "Down")
                key = screens::Key::down, held.down = true;
            else if (words[1] == "Left")
                key = screens::Key::left, held.left = true;
            else if (words[1] == "Right")
                key = screens::Key::right, held.right = true;
            session.key_pressed(host, key);
            frame(held);
            frame();
        }
        else if (words[0] == "wait")
        {
            const int frames = static_cast<int>(std::lround(std::stod(words[1]) * 60.0));
            for (int index = 0; index < frames; ++index)
                frame();
        }
        else if (words[0] == "hold")
        {
            // A key kept down: its press on the first frame, then the screen's
            // own auto-repeat, then the release.
            screens::HeldKeys held;
            screens::Key key = screens::Key::right;
            if (words[1] == "Up")
                key = screens::Key::up, held.up = true;
            else if (words[1] == "Down")
                key = screens::Key::down, held.down = true;
            else if (words[1] == "Left")
                key = screens::Key::left, held.left = true;
            else
                held.right = true;
            const int frames = static_cast<int>(std::lround(std::stod(words[2]) * 60.0));
            session.key_pressed(host, key);
            for (int index = 0; index < frames; ++index)
                frame(held);
            frame();
        }
        else if (words[0] == "mode")
        {
            host.creative_mode = words[1] == "Creative";
            host.infinite = words.size() > 2U && words[2] == "infinite";
        }
        else if (words[0] == "lock")
        {
            host.locked = std::stoi(words[1]);
        }
    }

    // The probe's state line, without the session traffic (focus and crate
    // messages) that is not the screen's own outcome.
    [[nodiscard]] std::string state()
    {
        std::string text = "state screen=";
        const screens::ScreenId screen = session.screen();
        const screens::Layout layout = session.layout();
        const bool classic_crafting = screen == screens::ScreenId::crafting && layout == screens::Layout::classic_crafting;
        if (screen == screens::ScreenId::none)
            text += "hud";
        else if (screen == screens::ScreenId::crate)
            text += "crate";
        else
            text += classic_crafting ? "crafting" : "inventory";
        if (screen != screens::ScreenId::none && !classic_crafting)
            text += " sel=" + (session.selector_visible() ? std::to_string(session.selected_slot()) : std::string{"none"});
        const auto &book = crafting_recipes(host.inventory().profile());
        if (classic_crafting)
        {
            const auto recipe = session.selected_recipe(host);
            text += " recipe=" + (recipe ? item_text(PlayerInventory::Slot{InventoryStack{book[*recipe].result,
                                                                                         book[*recipe].result_count}},
                                                     modern_)
                                         : std::string{"-"});
            text += " ingredient=" + std::to_string(session.selected_ingredient());
            text += " cursor=" + std::to_string(session.strip_cursor(host));
            text += std::string{" enabled="} + (recipe && host.can_craft(book[*recipe]) ? "1" : "0");
        }
        if (modern_ && screen == screens::ScreenId::inventory)
            text += " group=" + std::to_string(session.group()) + " type=" + std::to_string(session.type_in_group()) +
                    " recipe=" + std::to_string(session.recipe_in_type());
        text += " held=" + item_text(session.held(), modern_);
        text += " inv=";
        const std::size_t slots = modern_ ? 48U : 40U;
        for (std::size_t index = 0; index < slots; ++index)
        {
            const auto &slot = index < screens::backpack_slots
                                   ? host.inventory().storage()[index]
                                   : host.inventory().tray((index - screens::tray_base) / 8U)[(index - screens::tray_base) % 8U];
            text += (index == 0U ? "" : ",") + item_text(slot, modern_);
        }
        if (screen == screens::ScreenId::crate)
        {
            text += " crate=";
            for (std::size_t index = 0; index < host.crate.size(); ++index)
                text += (index == 0U ? "" : ",") + item_text(host.crate[index], modern_);
        }
        text += " events=[";
        for (std::size_t index = 0; index < host.events.size(); ++index)
            text += (index == 0U ? "" : "; ") + host.events[index];
        text += "]";
        host.events.clear();
        return text;
    }

    [[nodiscard]] std::vector<std::string> records() const
    {
        std::vector<std::string> lines;
        for (const screens::Draw &draw : session.draw(host, metrics_))
            lines.push_back(screens::record(draw));
        return lines;
    }

    screens::Session session;
    TestHost host;

  private:
    bool modern_{};
    int height_{720};
    std::string opened_;
    const screens::TextMetrics &metrics_;
};

// The probe's state line reduced to what the screen itself decides.
std::string normalise_state(const std::string &line, bool crate_screen)
{
    std::string text = line;
    // The crate's contents are compared while its screen is up.
    if (!crate_screen || text.rfind("state screen=crate", 0) != 0)
        if (const auto at = text.find(" crate="); at != std::string::npos)
        {
            const auto end = text.find(' ', at + 1U);
            text.erase(at, end == std::string::npos ? std::string::npos : end - at);
        }
    const auto open = text.find(" events=[");
    if (open == std::string::npos)
        return text;
    const auto close = text.rfind(']');
    const std::string inside = text.substr(open + 9U, close - open - 9U);
    std::string kept;
    for (std::string entry : split(inside, ';'))
    {
        while (!entry.empty() && entry.front() == ' ')
            entry.erase(entry.begin());
        if (entry.rfind("sound ", 0) != 0 && entry.rfind("pickup ", 0) != 0)
            continue;
        kept += (kept.empty() ? "" : "; ") + entry;
    }
    return text.substr(0, open) + " events=[" + kept + "]";
}

// --------------------------------------------------------------- scenario

struct Scenario final
{
    bool modern{};
    int width{};
    int height{};
    std::string name;
    std::string given;
    std::string screen;
    std::vector<std::string> actions;
};

std::vector<std::string> run_scenario(const Scenario &scenario, const screens::TextMetrics &metrics)
{
    Replay replay{scenario.modern, scenario.width, scenario.height, metrics};
    replay.give(scenario.given);
    replay.open(scenario.screen);
    for (const std::string &action : scenario.actions)
        replay.act(action);
    replay.frame();
    return replay.records();
}

bool screen_record(const std::string &line)
{
    return !line.empty() && line[0] != '#' && line.rfind("S name=@offscreen", 0) != 0 &&
           line.rfind("S name=@cursor", 0) != 0;
}

std::string join(const std::vector<std::string> &lines)
{
    std::string text;
    for (const std::string &line : lines)
        text += line + "\n";
    return text;
}

// Largest difference between the dst= rectangles of two records, in pixels.
float rectangle_error(const std::string &ours, const std::string &theirs)
{
    const auto parse = [](const std::string &line, const char *key) {
        std::vector<float> values;
        const auto at = line.find(key);
        if (at == std::string::npos)
            return values;
        const auto end = line.find(' ', at);
        for (const std::string &part : split(line.substr(at + std::strlen(key), end - at - std::strlen(key)), ','))
            values.push_back(std::stof(part));
        return values;
    };
    const char *key = ours.rfind("T ", 0) == 0 ? "pos=" : "dst=";
    const auto a = parse(ours, key);
    const auto b = parse(theirs, key);
    if (a.size() != b.size() || a.empty())
        return -1.0F;
    float worst = 0.0F;
    for (std::size_t index = 0; index < a.size(); ++index)
        worst = std::max(worst, std::fabs(a[index] - b[index]));
    return worst;
}

// ----------------------------------------------------------------- pinned

const Measure *pinned_measure(bool modern, std::string_view font, std::string_view text)
{
    static std::map<std::string, Measure> table;
    if (table.empty())
        for (const PinnedMeasure &entry : pinned_measures)
            table[std::string{entry.modern ? "m|" : "c|"} + entry.font + "|" + entry.text] =
                Measure{entry.font, entry.text, entry.width, entry.height};
    const auto found = table.find(std::string{modern ? "m|" : "c|"} + std::string{font} + "|" + std::string{text});
    return found == table.end() ? nullptr : &found->second;
}

void pinned_draw_lists_match_the_original()
{
    int checked = 0;
    for (const PinnedScenario &pin : pinned_scenarios)
    {
        const RecordedMetrics metrics{pin.modern, pinned_measure};
        Scenario scenario{pin.modern, pin.width, pin.height, pin.name, pin.given, pin.screen, {}};
        for (const std::string &action : split(pin.actions, '|'))
            if (!action.empty())
                scenario.actions.push_back(action);
        const auto lines = run_scenario(scenario, metrics);
        const std::string label = std::string{pin.modern ? "modern." : "classic."} + pin.name + " " +
                                  std::to_string(pin.width) + "x" + std::to_string(pin.height);
        check(metrics.missing.empty(),
              label + ": asked for a measurement the original never made: " +
                  (metrics.missing.empty() ? std::string{} : metrics.missing.front()));
        check(static_cast<int>(lines.size()) == pin.lines && fnv1a(join(lines)) == pin.hash,
              label + ": draw list differs from the recorded original (" + std::to_string(lines.size()) + " records, " +
                  std::to_string(pin.lines) + " recorded); run --compare for the records");
        if (!pin.modern)
        {
            const screens::IntRect destination = screens::classic_buffer_destination(pin.width, pin.height);
            check(destination == screens::IntRect{pin.blit_x, pin.blit_y, pin.blit_width, pin.blit_height},
                  label + ": the 1280x720 buffer is letterboxed differently");
        }
        ++checked;
    }
    check(checked >= 100, "expected at least 100 pinned scenarios, found " + std::to_string(checked));
}

void pinned_interactions_match_the_original()
{
    int scripts = 0;
    int steps = 0;
    for (const PinnedScript &pin : pinned_scripts)
    {
        const RecordedMetrics metrics{pin.modern, pinned_measure};
        Replay replay{pin.modern, 1280, 720, metrics};
        replay.give(pin.given);
        replay.open(pin.screen);
        const std::string label = std::string{pin.modern ? "modern." : "classic."} + pin.name;
        const auto hashes = split(pin.states, ',');
        const auto actions = split(pin.steps, '|');
        std::size_t index = 0;
        const auto verify = [&](const std::string &after) {
            const std::string ours = replay.state();
            const bool known = std::any_of(std::begin(known_differences), std::end(known_differences),
                                           [&](const KnownDifference &entry) {
                                               return entry.modern == pin.modern && label.substr(label.find('.') + 1U) == entry.script &&
                                                      static_cast<int>(index) == entry.step;
                                           });
            if (index < hashes.size() && !known)
                check(std::to_string(fnv1a(ours)) == hashes[index],
                      label + " after '" + after + "': " + ours + " is not what the original did");
            ++index;
            ++steps;
        };
        verify("open");
        for (const std::string &action : actions)
        {
            if (action.empty())
                continue;
            replay.act(action);
            verify(action);
        }
        ++scripts;
    }
    check(scripts >= 60 && steps >= 500,
          "expected the pinned input scripts, found " + std::to_string(scripts) + " with " + std::to_string(steps) + " states");
}

// CrateFocusMessage traffic of the shipped crate screens, recorded by
// ScreensParityProbe (scripts crate.focus_keys and crate.focus_pointer; the
// crate stands at 7,12,-4). Each step: the action, the selector the original
// showed afterwards (`sel=` of its state line) and every message it sent.
//
// It matters because a slot another player's message names is locked for
// everyone else and is not ejected when the crate is destroyed: 1.6.3.8 sends
// on open, on an arrow-key press and on close, and NEVER for the mouse;
// 1.9.9.8 adds the pointer coming to rest on a crate slot it was not on.
struct FocusStep final
{
    const char *action; // "" = the state right after the screen opened
    const char *selected;
    const char *sent;
};

constexpr FocusStep classic_focus_keys[]{
    {"", "32", "focus 0,0,0 0,4"},
    {"key Down", "100", "focus 7,12,-4 0,0"},
    {"key Down", "108", "focus 7,12,-4 0,1"},
    {"key Right", "109", "focus 7,12,-4 1,1"},
    {"key Left", "108", "focus 7,12,-4 0,1"},
    {"key Left", "115", "focus 7,12,-4 7,1"},
    {"key Up", "107", "focus 7,12,-4 7,0"},
    {"key Up", "39", "focus 0,0,0 7,4"},
    {"key Up", "31", "focus 0,0,0 7,3"},
    {"key Right", "24", "focus 0,0,0 0,3"},
    {"move 105", "105", ""},
    {"move 106", "106", ""},
    {"move 105", "105", ""},
    {"move 3", "3", ""},
    {"move 100", "100", ""},
    {"move out", "none", ""},
    {"key Down", "108", "focus 7,12,-4 0,1"},
    {"key Right", "109", "focus 7,12,-4 1,1"},
    // The press sends; the three repeats that follow move the selector silently.
    {"hold Right 0.8", "113", "focus 7,12,-4 2,1"},
    {"key Escape", "", "focus 0,0,0 0,0"},
};

constexpr FocusStep modern_focus_keys[]{
    {"", "32", "focus 0,0,0 0,8"},
    // 1.9.9.8 reads Keys.Down as SelectUp and Keys.Up as SelectDown.
    {"key Down", "28", "focus 0,0,0 0,7"},
    {"key Down", "24", "focus 0,0,0 0,6"},
    {"key Right", "25", "focus 0,0,0 1,6"},
    {"key Left", "24", "focus 0,0,0 0,6"},
    {"key Left", "127", "focus 7,12,-4 3,6"},
    {"key Up", "131", "focus 7,12,-4 3,7"},
    // The tray row is in neither grid; the side is remembered across it.
    {"key Up", "35", "focus 0,0,0 3,8"},
    {"key Up", "103", "focus 7,12,-4 3,0"},
    {"key Right", "0", "focus 0,0,0 0,0"},
    {"move 105", "105", "focus 7,12,-4 1,1"},
    {"move 106", "106", "focus 7,12,-4 2,1"},
    {"move 105", "105", "focus 7,12,-4 1,1"},
    {"move 3", "3", ""},
    {"move 100", "100", "focus 7,12,-4 0,0"},
    {"move out", "none", ""},
    {"key Down", "32", "focus 0,0,0 0,8"},
    {"key Right", "33", "focus 0,0,0 1,8"},
    {"hold Right 0.8", "33", "focus 0,0,0 2,8"},
    {"key Escape", "", "focus 0,0,0 0,0"},
};

constexpr FocusStep classic_focus_pointer[]{
    {"", "32", "focus 0,0,0 0,4"}, {"move 100", "100", ""}, {"move 101", "101", ""},
    {"move 131", "131", ""},       {"click left", "131", ""}, {"move 5", "5", ""},
    {"click left", "5", ""},       {"move 131", "131", ""},   {"move 34", "34", ""},
    {"move 4", "4", ""},           {"click left", "4", ""},   {"key Escape", "", "focus 0,0,0 0,0"},
};

constexpr FocusStep modern_focus_pointer[]{
    {"", "32", "focus 0,0,0 0,8"},
    // _lastSelectedLocation starts at (0,0): resting on slot 0 first sends nothing.
    {"move 100", "100", ""},
    {"move 101", "101", "focus 7,12,-4 1,0"},
    {"move 131", "131", "focus 7,12,-4 3,7"},
    {"click left", "131", ""},
    {"move 5", "5", ""},
    {"click left", "5", ""},
    // Back on the slot it announced last: nothing.
    {"move 131", "131", ""},
    {"move 34", "34", ""},
    {"move 4", "4", ""},
    {"click left", "4", ""},
    {"key Escape", "", "focus 0,0,0 0,0"},
};

void crate_focus_is_sent_when_the_original_sends_it()
{
    static constexpr const char *fill =
        "0=1x64 1=3x37 2=4x12 3=5x8 5=15x20 6=17x9 7=20x5 9=13x16 12=14x10 17=21x4 31=19x2 32=23x1@0.6 33=29x1 "
        "34=34x1@0.25 35=14x24 36=51x1@0.9 37=44x200 39=49x1 100=3x64 101=3x64 102=5x40 105=22x7 110=18x3 119=73x1 "
        "131=79x2";
    const auto run = [](bool modern, const char *name, std::span<const FocusStep> steps) {
        const RecordedMetrics metrics{modern, pinned_measure};
        Replay replay{modern, 1280, 720, metrics};
        replay.give(fill);
        for (const FocusStep &step : steps)
        {
            if (step.action[0] == '\0')
                replay.open("crate");
            else
                replay.act(step.action);
            const std::string label = std::string{modern ? "1.9.9.8 " : "1.6.3.8 "} + name + " after '" +
                                      (step.action[0] == '\0' ? "open" : step.action) + "'";
            std::string sent;
            for (const std::string &event : replay.host.focus_events)
                sent += (sent.empty() ? "" : "; ") + event;
            replay.host.focus_events.clear();
            check(sent == step.sent, label + ": sent [" + sent + "], the original sent [" + step.sent + "]");
            if (step.selected[0] == '\0')
            {
                check(replay.session.screen() == screens::ScreenId::none, label + ": the screen is still open");
                continue;
            }
            const std::string selected = replay.session.selector_visible()
                                             ? std::to_string(replay.session.selected_slot())
                                             : std::string{"none"};
            check(selected == step.selected,
                  label + ": the selector is on " + selected + ", the original's on " + step.selected);
        }
    };
    run(false, "crate.focus_keys", classic_focus_keys);
    run(true, "crate.focus_keys", modern_focus_keys);
    run(false, "crate.focus_pointer", classic_focus_pointer);
    run(true, "crate.focus_pointer", modern_focus_pointer);

    // What goes on the wire is the WORLD's grid, whatever the screen looks
    // like: the Classic look over a 1.9.9.8 world still names a crate slot in
    // fours, and its pointer still announces.
    using screens::CrateFocus;
    using screens::CrateFocusCause;
    check(screens::crate_focus_announcement(CrateFocusCause::pointer, screens::crate_base + 13U, false) == std::nullopt,
          "a 1.6.3.8 world announced a pointer move");
    check(screens::crate_focus_announcement(CrateFocusCause::pointer, 5U, true) == std::nullopt,
          "a 1.9.9.8 world announced the pointer over an inventory slot");
    check(screens::crate_focus_announcement(CrateFocusCause::key, screens::crate_base + 13U, false) ==
              CrateFocus{true, 5, 1},
          "1.6.3.8 crate slot 13 is (5,1)");
    check(screens::crate_focus_announcement(CrateFocusCause::pointer, screens::crate_base + 13U, true) ==
              CrateFocus{true, 1, 3},
          "1.9.9.8 crate slot 13 is (1,3)");
    check(screens::crate_focus_announcement(CrateFocusCause::closed, screens::crate_base + 13U, true) == CrateFocus{},
          "closing a crate names nothing");
}

// A retail style showing the OTHER release's world (the Modern interface on a
// Classic-rules world and the reverse) cannot be compared with an original;
// hold it to the promises the header makes instead. The style is one unit:
// its inventory, crate and crafting screens always come together.
void a_style_shows_either_world_completely()
{
    const RecordedMetrics metrics{true, pinned_measure};
    // Every Classic recipe is filed under a type one of Modern's tabs lists.
    const auto &groups = modern_recipe_groups();
    const auto &classic_book = crafting_recipes(InventoryProfile::classic);
    std::vector<int> seen(classic_book.size(), 0);
    TestHost classic{false};
    screens::Session modern_view;
    modern_view.configure(UiStyle::modern, 1280, 720);
    modern_view.open(classic, screens::ScreenId::inventory);
    for (const RecipeGroup &group : groups)
        for (std::size_t type = 0; type < group.type_count; ++type)
            for (const std::size_t index : modern_view.type_recipes(classic, group.types[type]))
                ++seen[index];
    for (std::size_t index = 0; index < seen.size(); ++index)
        check(seen[index] == 1, "Classic recipe " + std::string{item_name(classic_book[index].result)} +
                                    " is listed under " + std::to_string(seen[index]) + " Modern tabs");
    // A Modern world in the Classic strip: every recipe a tab lists is there
    // once, and the one recipe no tab lists stays unreachable.
    TestHost modern{true};
    screens::Session classic_view;
    classic_view.configure(UiStyle::classic, 1280, 720);
    classic_view.open(modern, screens::ScreenId::crafting);
    const auto &modern_book = crafting_recipes(InventoryProfile::modern);
    const auto strip = classic_view.strip_recipes(modern);
    std::size_t listed = 0;
    for (const CraftingRecipe &recipe : modern_book)
        listed += recipe_listed(recipe) ? 1U : 0U;
    check(strip.size() == listed, "the Classic strip lists " + std::to_string(strip.size()) + " of a Modern world's " +
                                      std::to_string(listed) + " listed recipes");
    for (const std::size_t index : strip)
        check(recipe_listed(modern_book[index]), "the Classic strip lists a recipe no Modern tab has");
    // The style never changes the rules: a Classic world keeps one tray and
    // its own stack limits in the Modern layout, and the reverse.
    check(!modern_view.slot_rect(screens::tray_base + 8U).has_value() ||
              modern_view.draw(classic, metrics).size() > 4U,
          "Modern layout of a Classic world draws");
    int grids = 0;
    for (const screens::Draw &draw : modern_view.draw(classic, metrics))
        grids += draw.kind == screens::Draw::Kind::sprite && draw.sprite == screens::Sprite::hud_grid ? 1 : 0;
    check(grids == 1, "a Classic world has one tray in the Modern layout, drew " + std::to_string(grids));
    classic.inventory().storage()[0] = InventoryStack{InventoryItemId::dirt_block, 64U};
    classic.inventory().storage()[1] = InventoryStack{InventoryItemId::dirt_block, 64U};
    const auto first = modern_view.slot_rect(0U)->center();
    const auto second = modern_view.slot_rect(1U)->center();
    modern_view.pointer_moved(classic, first.x, first.y);
    modern_view.button_pressed(classic, screens::Button::left, false);
    modern_view.pointer_moved(classic, second.x, second.y);
    modern_view.button_pressed(classic, screens::Button::left, false);
    check(classic.inventory().storage()[1]->count == 64U && modern_view.held().has_value(),
          "a full Classic stack of 64 takes no more in the Modern layout (1.9.9.8 stacks blocks to 999)");
    // One style, all of its screens: the Classic interface crafts on the
    // Classic crafting screen and the Modern one on its own recipe tree. No
    // control on either leads to the other style's screen.
    screens::Session whole;
    whole.configure(UiStyle::classic, 1280, 720);
    whole.open(classic, screens::ScreenId::inventory);
    check(whole.layout() == screens::Layout::classic_inventory, "Classic interface style shows the Classic inventory");
    whole.key_pressed(classic, screens::Key::inventory);
    check(whole.screen() == screens::ScreenId::crafting && whole.layout() == screens::Layout::classic_crafting,
          "the Classic inventory's Craft opens the Classic crafting screen");
    whole.key_pressed(classic, screens::Key::escape);
    check(whole.screen() == screens::ScreenId::inventory, "and Escape returns to the inventory");
    whole.open(classic, screens::ScreenId::crate);
    check(whole.layout() == screens::Layout::classic_crate, "and its crate screen is the Classic one");
    whole.configure(UiStyle::modern, 1280, 720);
    whole.open(classic, screens::ScreenId::inventory);
    check(whole.layout() == screens::Layout::modern_inventory && !whole.button_rect().has_value(),
          "the Modern interface shows its recipe tree and no button to another crafting screen");
    whole.open(classic, screens::ScreenId::crafting);
    check(whole.screen() == screens::ScreenId::inventory && whole.layout() == screens::Layout::modern_inventory,
          "asking the Modern interface for crafting opens its combined screen");
    whole.open(classic, screens::ScreenId::crate);
    check(whole.layout() == screens::Layout::modern_crate, "and its crate screen is the Modern one");
    int tree_rows = 0;
    whole.open(classic, screens::ScreenId::inventory);
    for (const screens::Draw &draw : whole.draw(classic, metrics))
        tree_rows += draw.kind == screens::Draw::Kind::sprite && draw.sprite == screens::Sprite::materials_icon ? 1 : 0;
    check(tree_rows == 1, "the Modern interface always draws its recipe tree");
}

// ------------------------------------------------------------ probe output

struct Recording final
{
    std::vector<Measure> classic_measures;
    std::vector<Measure> modern_measures;
    std::map<std::string, Measure> lookup;

    void load_measures(const fs::path &root)
    {
        for (const bool modern : {false, true})
        {
            const fs::path directory = root / (modern ? "modern" : "classic");
            if (!fs::is_directory(directory))
                continue;
            for (const auto &entry : fs::directory_iterator{directory})
            {
                const std::string name = entry.path().filename().string();
                if (name.rfind("measures.", 0) != 0)
                    continue;
                std::ifstream file{entry.path()};
                for (std::string line; std::getline(file, line);)
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    // "<Font> <width> <height> "<text>""
                    const auto first = line.find(' ');
                    const auto second = line.find(' ', first + 1U);
                    const auto third = line.find(' ', second + 1U);
                    if (third == std::string::npos)
                        continue;
                    Measure measure{line.substr(0, first), {}, std::stof(line.substr(first + 1U, second - first - 1U)),
                                    std::stof(line.substr(second + 1U, third - second - 1U))};
                    std::string quoted = line.substr(third + 1U);
                    for (std::size_t index = 1; index + 1U < quoted.size(); ++index)
                    {
                        if (quoted[index] == '\\' && index + 2U < quoted.size())
                        {
                            ++index;
                            measure.text.push_back(quoted[index] == 'n' ? '\n' : quoted[index]);
                        }
                        else
                        {
                            measure.text.push_back(quoted[index]);
                        }
                    }
                    lookup[std::string{modern ? "m|" : "c|"} + measure.font + "|" + measure.text] = measure;
                }
            }
        }
    }

    [[nodiscard]] const Measure *find(bool modern, std::string_view font, std::string_view text) const
    {
        const auto found = lookup.find(std::string{modern ? "m|" : "c|"} + std::string{font} + "|" + std::string{text});
        return found == lookup.end() ? nullptr : &found->second;
    }
};

struct RecordedScenario final
{
    Scenario scenario;
    std::vector<std::string> lines;
    screens::IntRect blit{};
};

std::vector<RecordedScenario> load_scenarios(const fs::path &root)
{
    std::vector<RecordedScenario> scenarios;
    for (const bool modern : {false, true})
    {
        const fs::path directory = root / (modern ? "modern" : "classic") / "draws";
        if (!fs::is_directory(directory))
            continue;
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator{directory})
            files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        for (const fs::path &path : files)
        {
            RecordedScenario recorded;
            recorded.scenario.modern = modern;
            std::ifstream file{path};
            int header = 0;
            for (std::string line; std::getline(file, line);)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty() && line[0] == '#')
                {
                    const std::string body = line.substr(2);
                    if (header == 0)
                    {
                        // "<generation>.<name> <W>x<H>"
                        const auto space = body.rfind(' ');
                        const std::string full = body.substr(0, space);
                        recorded.scenario.name = full.substr(full.find('.') + 1U);
                        const auto size = split(body.substr(space + 1U), 'x');
                        recorded.scenario.width = std::stoi(size[0]);
                        recorded.scenario.height = std::stoi(size[1]);
                    }
                    else if (body.rfind("given", 0) == 0)
                        recorded.scenario.given = body.size() > 6U ? body.substr(6) : std::string{};
                    else if (body.rfind("open ", 0) == 0)
                        recorded.scenario.screen = body.substr(5);
                    else if (body.rfind("state ", 0) != 0)
                        recorded.scenario.actions.push_back(body);
                    ++header;
                    continue;
                }
                if (line.rfind("S name=@offscreen dst=", 0) == 0)
                {
                    const auto values = split(line.substr(22, line.find(' ', 22) - 22), ',');
                    recorded.blit = {std::stoi(values[0]), std::stoi(values[1]), std::stoi(values[2]), std::stoi(values[3])};
                }
                if (screen_record(line))
                    recorded.lines.push_back(line);
            }
            scenarios.push_back(std::move(recorded));
        }
    }
    return scenarios;
}

// The `focus ...` entries of a recorded line's events=[...], in order: every
// CrateFocusMessage the original sent during that step.
std::string focus_events_of(const std::string &line)
{
    const auto open = line.find(" events=[");
    const auto close = line.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close < open + 9U)
        return {};
    std::string kept;
    for (std::string entry : split(line.substr(open + 9U, close - open - 9U), ';'))
    {
        while (!entry.empty() && entry.front() == ' ')
            entry.erase(entry.begin());
        if (entry.rfind("focus ", 0) == 0)
            kept += (kept.empty() ? "" : "; ") + entry;
    }
    return kept;
}

std::string joined(const std::vector<std::string> &entries)
{
    std::string text;
    for (const std::string &entry : entries)
        text += (text.empty() ? "" : "; ") + entry;
    return text;
}

struct RecordedScript final
{
    bool modern{};
    std::string name;
    std::string given;
    std::string screen;
    std::vector<std::string> steps;
    std::vector<std::string> states;
    // CrateFocusMessage traffic: what opening the screen sent, then per state.
    bool has_opened{};
    std::string opened_focus;
    std::vector<std::string> focus;
};

std::vector<RecordedScript> load_scripts(const fs::path &root)
{
    std::vector<RecordedScript> scripts;
    for (const bool modern : {false, true})
    {
        const std::string generation = modern ? "modern" : "classic";
        std::ifstream file{root / generation / ("interaction." + generation + ".txt")};
        for (std::string line; std::getline(file, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("script ", 0) == 0)
            {
                scripts.push_back({});
                scripts.back().modern = modern;
                scripts.back().name = line.substr(7);
            }
            else if (scripts.empty())
                continue;
            else if (line.rfind("given", 0) == 0)
                scripts.back().given = line.size() > 6U ? line.substr(6) : std::string{};
            else if (line.rfind("open ", 0) == 0)
                scripts.back().screen = line.substr(5);
            else if (line.rfind("opened ", 0) == 0)
            {
                scripts.back().has_opened = true;
                scripts.back().opened_focus = focus_events_of(line);
            }
            else if (line.rfind("step ", 0) == 0)
                scripts.back().steps.push_back(line.substr(5));
            else if (line.rfind("state ", 0) == 0)
            {
                scripts.back().states.push_back(normalise_state(line, scripts.back().screen == "crate"));
                scripts.back().focus.push_back(focus_events_of(line));
            }
        }
    }
    return scripts;
}

bool known_difference(bool modern, const std::string &script, std::size_t step)
{
    return std::any_of(std::begin(known_differences), std::end(known_differences), [&](const KnownDifference &entry) {
        return entry.modern == modern && script == entry.script && static_cast<int>(step) == entry.step;
    });
}

int compare(const fs::path &root, const fs::path *dump)
{
    Recording recording;
    recording.load_measures(root);
    const auto lookup = [&](bool modern, std::string_view font, std::string_view text) {
        return recording.find(modern, font, text);
    };
    int different = 0;
    int total = 0;
    float worst = 0.0F;
    std::string worst_where;
    if (dump != nullptr)
        fs::create_directories(*dump);
    for (const RecordedScenario &recorded : load_scenarios(root))
    {
        const RecordedMetrics metrics{recorded.scenario.modern, lookup};
        const auto ours = run_scenario(recorded.scenario, metrics);
        const std::string label = std::string{recorded.scenario.modern ? "modern." : "classic."} + recorded.scenario.name + "." +
                                  std::to_string(recorded.scenario.width) + "x" + std::to_string(recorded.scenario.height);
        ++total;
        if (dump != nullptr)
        {
            std::ofstream out{*dump / (label + ".txt")};
            out << join(ours);
        }
        bool same = ours.size() == recorded.lines.size() && metrics.missing.empty();
        int shown = 0;
        for (std::size_t index = 0; index < std::min(ours.size(), recorded.lines.size()); ++index)
        {
            if (ours[index] == recorded.lines[index])
                continue;
            same = false;
            const float error = rectangle_error(ours[index], recorded.lines[index]);
            if (error > worst)
            {
                worst = error;
                worst_where = label;
            }
            if (shown++ < 4)
                std::cout << "  " << label << " record " << index << "\n    original: " << recorded.lines[index]
                          << "\n    ours:     " << ours[index] << '\n';
        }
        if (!recorded.scenario.modern)
        {
            const screens::IntRect destination =
                screens::classic_buffer_destination(recorded.scenario.width, recorded.scenario.height);
            if (!(destination == recorded.blit))
            {
                same = false;
                std::cout << "  " << label << " letterbox: original " << recorded.blit.x << "," << recorded.blit.y << ","
                          << recorded.blit.width << "," << recorded.blit.height << " ours " << destination.x << ","
                          << destination.y << "," << destination.width << "," << destination.height << '\n';
            }
        }
        if (!same)
        {
            ++different;
            std::cout << "DIFFERENT " << label << ": " << ours.size() << " records, original " << recorded.lines.size();
            if (!metrics.missing.empty())
                std::cout << "; unmeasured " << metrics.missing.front();
            std::cout << '\n';
        }
    }
    std::cout << "draw lists: " << (total - different) << "/" << total << " identical to the original; worst rectangle error "
              << worst << " px" << (worst_where.empty() ? "" : " in " + worst_where) << '\n';

    int states = 0;
    int wrong = 0;
    int accepted = 0;
    int focus_states = 0;
    int focus_wrong = 0;
    std::ofstream state_dump;
    if (dump != nullptr)
        state_dump.open(*dump / "interaction.txt");
    for (const RecordedScript &script : load_scripts(root))
    {
        const RecordedMetrics metrics{script.modern, lookup};
        Replay replay{script.modern, 1280, 720, metrics};
        replay.give(script.given);
        replay.open(script.screen);
        const std::string label = std::string{script.modern ? "modern." : "classic."} + script.name;
        if (state_dump.is_open())
            state_dump << "script " << label << '\n';
        for (std::size_t index = 0; index <= script.steps.size() && index < script.states.size(); ++index)
        {
            if (index > 0U)
                replay.act(script.steps[index - 1U]);
            const std::string ours = replay.state();
            // CrateFocusMessage traffic, step by step. (A recording made
            // before the probe wrote its `opened` line has nothing to say
            // about the message OnPushed sends.)
            const std::string our_focus = joined(replay.host.focus_events);
            replay.host.focus_events.clear();
            if (script.screen == "crate" && (index > 0U || script.has_opened))
            {
                std::string their_focus = index == 0U ? script.opened_focus : std::string{};
                if (!script.focus[index].empty())
                    their_focus += (their_focus.empty() ? "" : "; ") + script.focus[index];
                ++focus_states;
                if (our_focus != their_focus)
                {
                    ++focus_wrong;
                    std::cout << "DIFFERENT " << label << " crate focus after '"
                              << (index == 0U ? std::string{"open"} : script.steps[index - 1U]) << "'\n    original: ["
                              << their_focus << "]\n    ours:     [" << our_focus << "]\n";
                }
            }
            if (state_dump.is_open())
                state_dump << (index == 0U ? std::string{"open"} : script.steps[index - 1U]) << "\n" << ours << '\n';
            ++states;
            if (ours == script.states[index])
                continue;
            if (known_difference(script.modern, script.name, index))
            {
                ++accepted;
                continue;
            }
            ++wrong;
            std::cout << "DIFFERENT " << label << " after '" << (index == 0U ? std::string{"open"} : script.steps[index - 1U])
                      << "'\n    original: " << script.states[index] << "\n    ours:     " << ours << '\n';
        }
    }
    std::cout << "input scripts: " << (states - wrong - accepted) << "/" << states << " states identical to the original, "
              << accepted << " documented differences, " << wrong << " unexplained\n";
    std::cout << "crate focus messages: " << (focus_states - focus_wrong) << "/" << focus_states
              << " crate-screen steps send what the original sent\n";
    return different == 0 && wrong == 0 && focus_wrong == 0 ? 0 : 1;
}

std::string literal(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        if (c == '\n')
            out += "\\n";
        else
            out.push_back(c);
    }
    return out + "\"";
}

std::string float_literal(float value)
{
    std::string text = number(value);
    if (text.find('.') == std::string::npos)
        text += ".0";
    return text + "F";
}

int pin(const fs::path &root, const fs::path &header)
{
    Recording recording;
    recording.load_measures(root);
    std::ofstream out{header, std::ios::binary};
    out << "#pragma once\n\n"
           "// GENERATED by `openminerz_screens_parity_tests --pin` from a ScreensParityProbe\n"
           "// run over the owned 1.6.3.8 and 1.9.9.8 installs. Do not edit by hand.\n"
           "//\n"
           "// pinned_measures   SpriteFont.MeasureString answers the shipped screens asked for\n"
           "// pinned_scenarios  per scripted screen and back-buffer size: the record count and\n"
           "//                   FNV-1a hash of the original's draw list, and (1.6.3.8) where its\n"
           "//                   1280x720 buffer was blitted\n"
           "// pinned_scripts    per input script: the steps and the hash of the original's\n"
           "//                   state after each\n\n"
           "struct PinnedMeasure final\n{\n    bool modern;\n    const char *font;\n    const char *text;\n"
           "    float width;\n    float height;\n};\n\n"
           "inline constexpr PinnedMeasure pinned_measures[]{\n";
    // Only what the three screen fonts were asked.
    for (const auto &[key, measure] : recording.lookup)
        if (measure.font == "SmallFont" || measure.font == "MedFont" || measure.font == "MyriadMedium")
            out << "    {" << (key[0] == 'm' ? "true" : "false") << ", \"" << measure.font << "\", " << literal(measure.text)
                << ", " << float_literal(measure.width) << ", " << float_literal(measure.height) << "},\n";
    out << "};\n\nstruct PinnedScenario final\n{\n    bool modern;\n    int width;\n    int height;\n    const char *name;\n"
           "    const char *given;\n    const char *screen;\n    const char *actions;\n    int lines;\n"
           "    unsigned hash;\n    int blit_x;\n    int blit_y;\n    int blit_width;\n    int blit_height;\n};\n\n"
           "inline constexpr PinnedScenario pinned_scenarios[]{\n";
    for (const RecordedScenario &recorded : load_scenarios(root))
    {
        std::string actions;
        for (const std::string &action : recorded.scenario.actions)
            actions += (actions.empty() ? "" : "|") + action;
        out << "    {" << (recorded.scenario.modern ? "true" : "false") << ", " << recorded.scenario.width << ", "
            << recorded.scenario.height << ", " << literal(recorded.scenario.name) << ", " << literal(recorded.scenario.given)
            << ",\n     " << literal(recorded.scenario.screen) << ", " << literal(actions) << ", " << recorded.lines.size()
            << ", " << fnv1a(join(recorded.lines)) << "U, " << recorded.blit.x << ", " << recorded.blit.y << ", "
            << recorded.blit.width << ", " << recorded.blit.height << "},\n";
    }
    out << "};\n\nstruct PinnedScript final\n{\n    bool modern;\n    const char *name;\n    const char *given;\n"
           "    const char *screen;\n    const char *steps;\n    const char *states;\n};\n\n"
           "inline constexpr PinnedScript pinned_scripts[]{\n";
    for (const RecordedScript &script : load_scripts(root))
    {
        std::string steps;
        for (const std::string &step : script.steps)
            steps += (steps.empty() ? "" : "|") + step;
        std::string states;
        for (const std::string &state : script.states)
            states += (states.empty() ? "" : ",") + std::to_string(fnv1a(state));
        out << "    {" << (script.modern ? "true" : "false") << ", " << literal(script.name) << ", " << literal(script.given)
            << ",\n     " << literal(script.screen) << ", " << literal(steps) << ",\n     " << literal(states) << "},\n";
    }
    out << "};\n\n"
           "// States where this client deliberately does not do what the original did.\n"
           "// `step` counts the states of the script from 0 (the state after opening).\n"
           "struct KnownDifference final\n{\n    bool modern;\n    const char *script;\n    int step;\n    const char *why;\n};\n\n"
           "inline constexpr KnownDifference known_differences[]{\n"
           "    // 1.6.3.8's CrateScreen clears _holdingItem on Escape without returning it:\n"
           "    // the stack on the cursor is destroyed. 1.9.9.8 hands it back; so do we.\n"
           "    {false, \"crate.escape_with_held\", 3, \"the held stack is returned to the inventory, not destroyed\"},\n"
           "};\n";
    std::cout << "pinned " << header.string() << '\n';
    return out.good() ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "--compare" && argc > 2)
        return compare(argv[2], nullptr);
    if (mode == "--dump" && argc > 3)
    {
        const fs::path out{argv[3]};
        return compare(argv[2], &out);
    }
    if (mode == "--pin" && argc > 3)
        return pin(argv[2], argv[3]);
    const std::pair<const char *, void (*)()> tests[]{
        {"pinned_draw_lists_match_the_original", pinned_draw_lists_match_the_original},
        {"pinned_interactions_match_the_original", pinned_interactions_match_the_original},
        {"crate_focus_is_sent_when_the_original_sends_it", crate_focus_is_sent_when_the_original_sends_it},
        {"a_style_shows_either_world_completely", a_style_shows_either_world_completely},
    };
    for (const auto &[name, test] : tests)
    {
        const int before = failures;
        test();
        std::cout << (failures == before ? "PASS " : "FAIL ") << name << '\n';
    }
    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "screens parity: all checks passed\n";
    return 0;
}
