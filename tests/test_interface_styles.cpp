// The interface style is presentation only.
//
// Remastered, Classic and Modern are three arrangements of the same inventory,
// crate and crafting screens (screens::Session). This test holds them to that:
//
//   * The same scripted actions - pick up, put down, split, place one, wheel,
//     quick move, throw, drop, craft one, craft all - are run in every style
//     for a Classic-rules world and for a Modern-rules world, and after every
//     single action the backpack, the trays, the crate, the stack on the
//     cursor and the items thrown into the world must be identical in all
//     three. The reference is the world's own retail style, the one
//     tests/test_screens_parity.cpp pins to recordings of the shipped game.
//   * A few thousand pseudo-random actions do the same, so an arrangement
//     cannot hide a rule of its own in a corner the scripts miss.
//   * Remastered's keyboard and gamepad routes reach the same operations as
//     its pointer.
//   * The Remastered layout rule (remastered_layout.hpp) keeps every HUD
//     element and the whole inventory panel on screen, unclipped and not on
//     top of each other, from 1280x720 to 3840x2160, across 4:3, 16:10, 16:9
//     and 21:9, at every interface scale and screen margin; and a pointer
//     placed on a slot at any of those sizes selects that slot.
//
// It needs no game files.

#include "openminerz/remastered_layout.hpp"
#include "openminerz/screens.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace openminerz;

namespace
{

int failures = 0;

void check(bool condition, const std::string &message)
{
    if (condition)
        return;
    ++failures;
    if (failures <= 40)
        std::cerr << "FAIL: " << message << '\n';
}

constexpr float frame_seconds = 0.017F;

std::string_view style_name(UiStyle style) noexcept
{
    switch (style)
    {
    case UiStyle::classic:
        return "Classic";
    case UiStyle::modern:
        return "Modern";
    case UiStyle::remastered:
        return "Remastered";
    case UiStyle::automatic:
        break;
    }
    return "Automatic";
}

// ------------------------------------------------------------------- host

// The game as the screens see it: the rules of one world generation.
class WorldHost final : public screens::Host
{
  public:
    explicit WorldHost(bool modern) : modern_(modern)
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
        ++crafts;
        if (infinite)
        {
            const std::uint16_t rest = inventory_.add_item(recipe.result, recipe.result_count);
            if (rest != 0U)
                drop(InventoryStack{recipe.result, rest});
            return;
        }
        int guard = 0;
        do
        {
            const std::uint16_t rest = inventory_.craft(recipe);
            if (rest != 0U)
                drop(InventoryStack{recipe.result, rest});
        } while (all && can_craft(recipe) && ++guard < 100000);
        check(guard < 100000, "craft-all never ended: the style asked for it where the world forbids it");
    }
    [[nodiscard]] bool creative() const noexcept override { return creative_mode; }
    void play(screens::Cue cue) override { cues.push_back(cue); }
    void drop(const InventoryStack &stack) override
    {
        thrown += std::to_string(static_cast<int>(stack.item)) + "x" + std::to_string(stack.count) + " ";
    }
    void crate_changed(std::size_t) override { ++crate_writes; }

    std::array<PlayerInventory::Slot, 32> crate{};
    bool crate_open{};
    int locked{-1};
    bool creative_mode{};
    bool infinite{};
    int crafts{};
    int crate_writes{};
    std::string thrown;
    std::vector<screens::Cue> cues;

  private:
    bool modern_{};
    PlayerInventory inventory_;
};

std::string slot_text(const PlayerInventory::Slot &slot)
{
    if (!slot.has_value())
        return "-";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%dx%d@%.3f/%d", static_cast<int>(slot->item),
                  static_cast<int>(slot->count), static_cast<double>(slot->durability),
                  static_cast<int>(slot->rounds_in_clip));
    return buffer;
}

// Everything an action can change: every slot, the cursor, what was thrown.
std::string world_state(const WorldHost &host, const screens::Session &session)
{
    std::string text = "bag=";
    for (const auto &slot : host.inventory().storage())
        text += slot_text(slot) + ",";
    text += " trays=";
    for (const auto &tray : host.inventory().trays())
        for (const auto &slot : tray)
            text += slot_text(slot) + ",";
    text += " crate=";
    for (const auto &slot : host.crate)
        text += slot_text(slot) + ",";
    text += " held=" + slot_text(session.held());
    text += " thrown=" + host.thrown;
    return text;
}

// -------------------------------------------------------------------- one style

// One world shown in one style, driven by actions that mean the same thing in
// every style ("left click on slot 5", "craft recipe 12").
class Styled final
{
  public:
    Styled(UiStyle style, bool modern_world, int width = 1280, int height = 720)
        : style_(style), modern_world_(modern_world), host(modern_world)
    {
        session.configure(style, width, height);
    }

    void open(bool crate)
    {
        host.crate_open = crate;
        session.open(host, crate ? screens::ScreenId::crate : screens::ScreenId::inventory);
        frame();
    }

    void frame() { session.update(host, frame_seconds); }

    void point(screens::Point layout)
    {
        const screens::Point window = session.to_window(layout);
        session.pointer_moved(host, window.x, window.y);
        frame();
    }
    void point_slot(std::size_t slot)
    {
        const auto rect = session.slot_rect(slot);
        check(rect.has_value(), std::string{style_name(style_)} + ": slot " + std::to_string(slot) + " is not on screen");
        if (rect)
            point(rect->center());
    }
    void point_outside()
    {
        const screens::IntRect back = session.panel();
        point({back.x - 40, back.y + 60});
    }
    void click(screens::Button button, bool shift = false)
    {
        session.button_pressed(host, button, shift);
        frame();
        session.button_released(host, button);
        frame();
    }
    void wheel(int direction)
    {
        session.wheel(host, direction);
        frame();
    }
    void key(screens::Key key, bool shift = false)
    {
        session.key_pressed(host, key, shift);
        frame();
    }

    // Make `recipe` (a cookbook index) once, or as many as the world allows,
    // the way this style's own controls do it. Nothing may be on the cursor.
    void craft(std::size_t recipe, bool all)
    {
        const InventoryProfile world = host.inventory().profile();
        const auto &book = crafting_recipes(world);
        switch (style_)
        {
        case UiStyle::classic:
        {
            // E opens CraftingUIScreen; the arrows walk the strip; a click on
            // the selected icon crafts (Shift: all); Escape goes back.
            key(screens::Key::inventory);
            const auto strip = session.strip_recipes(host);
            const auto found = std::find(strip.begin(), strip.end(), recipe);
            if (found != strip.end())
            {
                for (int guard = 0; guard < 400 && session.selected_recipe(host) != recipe; ++guard)
                {
                    const auto now = std::find(strip.begin(), strip.end(), session.selected_recipe(host).value_or(recipe));
                    key(now < found ? screens::Key::down : screens::Key::up);
                }
                point(session.strip_rect(2)->center());
                click(screens::Button::left, all);
            }
            key(screens::Key::escape);
            break;
        }
        case UiStyle::modern:
        {
            // Hover the group, the type and the recipe; a click crafts.
            const RecipeType type = screens::recipe_type_for(book[recipe], world);
            const auto &groups = modern_recipe_groups();
            for (std::size_t group = 0; group < groups.size(); ++group)
                for (std::size_t entry = 0; entry < groups[group].type_count; ++entry)
                {
                    if (groups[group].types[entry] != type)
                        continue;
                    point(session.group_rect(group)->center());
                    point(session.type_rect(entry)->center());
                    const auto list = session.type_recipes(host, type);
                    const auto found = std::find(list.begin(), list.end(), recipe);
                    if (found == list.end())
                        return;
                    point(session.recipe_rect(host, static_cast<std::size_t>(found - list.begin()))->center());
                    click(screens::Button::left, all);
                    return;
                }
            break;
        }
        case UiStyle::remastered:
        {
            // Choose it in the list; the Craft / Craft all buttons make it.
            const auto everything = session.tab_recipes(host, screens::RecipeTab::all);
            if (std::find(everything.begin(), everything.end(), recipe) == everything.end())
                return;
            session.select_recipe(host, recipe);
            frame();
            point(session.craft_rect(all)->center());
            click(screens::Button::left);
            break;
        }
        case UiStyle::automatic:
            break;
        }
    }

    void close()
    {
        // 1.9.9.8's first Escape only puts a held stack back; a second closes.
        for (int guard = 0; guard < 3 && session.is_open(); ++guard)
            key(screens::Key::escape);
    }

    [[nodiscard]] std::string state() const { return world_state(host, session); }
    [[nodiscard]] std::string label() const
    {
        return std::string{style_name(style_)} + " style, " + (modern_world_ ? "Modern" : "Classic") + " world";
    }

    screens::Session session;

  private:
    UiStyle style_{};
    bool modern_world_{};

  public:
    WorldHost host;
};

// ---------------------------------------------------------------- actions

struct Action final
{
    enum class Kind : std::uint8_t
    {
        point,       // slot
        outside,     // pointer off the panel
        left,        // left button (shift: quick move)
        right,       // right button
        wheel_up,
        wheel_down,
        drop,        // Q
        craft,       // slot = cookbook index (shift: all)
        tray,        // slot = tray in hand (the game's own tray switch)
        creative,    // slot = 0/1
        infinite,    // slot = 0/1
        lock         // slot = crate slot another player has selected, 99 none
    };
    Kind kind{};
    std::size_t slot{};
    bool shift{};
    [[nodiscard]] std::string text() const
    {
        static constexpr std::array<std::string_view, 12> names{
            "point", "outside", "left", "right", "wheel up", "wheel down",
            "drop", "craft", "tray", "creative", "infinite", "lock"};
        return std::string{names[static_cast<std::size_t>(kind)]} + " " + std::to_string(slot) +
               (shift ? " shift" : "");
    }
};
using K = Action::Kind;

void apply(Styled &view, const Action &action)
{
    switch (action.kind)
    {
    case K::point:
        view.point_slot(action.slot);
        break;
    case K::outside:
        view.point_outside();
        break;
    case K::left:
        view.click(screens::Button::left, action.shift);
        break;
    case K::right:
        view.click(screens::Button::right);
        break;
    case K::wheel_up:
        view.wheel(1);
        break;
    case K::wheel_down:
        view.wheel(-1);
        break;
    case K::drop:
        view.key(screens::Key::drop);
        break;
    case K::craft:
        view.craft(action.slot, action.shift);
        break;
    case K::tray:
        view.host.inventory().select_tray(action.slot);
        view.frame();
        break;
    case K::creative:
        view.host.creative_mode = action.slot != 0U;
        break;
    case K::infinite:
        view.host.infinite = action.slot != 0U;
        break;
    case K::lock:
        view.host.locked = action.slot == 99U ? -1 : static_cast<int>(action.slot);
        break;
    }
}

struct Given final
{
    std::size_t slot{};
    InventoryItemId item{};
    std::uint16_t count{};
    float durability{1.0F};
};

void give(WorldHost &host, const std::vector<Given> &items)
{
    for (const Given &entry : items)
    {
        InventoryStack stack{entry.item, entry.count};
        stack.durability = entry.durability;
        if (entry.slot >= screens::crate_base)
            host.crate[entry.slot - screens::crate_base] = stack;
        else if (entry.slot < screens::backpack_slots)
            host.inventory().storage()[entry.slot] = stack;
        else
            host.inventory().tray((entry.slot - screens::tray_base) / 8U)[(entry.slot - screens::tray_base) % 8U] = stack;
    }
}

constexpr std::array<UiStyle, 3> styles{UiStyle::classic, UiStyle::modern, UiStyle::remastered};

// Run one script in all three styles of one world; the states must agree
// after every action. Returns how many states were compared.
int run_everywhere(const std::string &name, bool modern_world, bool crate, const std::vector<Given> &items,
                   const std::vector<Action> &script)
{
    std::array<Styled, 3> views{Styled{styles[0], modern_world}, Styled{styles[1], modern_world},
                                Styled{styles[2], modern_world}};
    // The world's own retail style is the reference.
    const std::size_t reference = modern_world ? 1U : 0U;
    for (Styled &view : views)
    {
        give(view.host, items);
        view.open(crate);
    }
    int compared = 0;
    const auto compare = [&](const std::string &after) {
        const std::string expected = views[reference].state();
        for (std::size_t index = 0; index < views.size(); ++index)
        {
            if (index == reference)
                continue;
            const std::string got = views[index].state();
            check(got == expected, name + " (" + views[index].label() + ") after '" + after + "':\n  got      " + got +
                                       "\n  expected " + expected);
            ++compared;
        }
    };
    compare("open");
    for (const Action &action : script)
    {
        for (Styled &view : views)
            apply(view, action);
        compare(action.text());
    }
    for (Styled &view : views)
        view.close();
    compare("close");
    for (const Styled &view : views)
        check(!view.session.is_open() && !view.session.held().has_value(),
              name + " (" + view.label() + "): closing must leave no screen and nothing on the cursor");
    return compared;
}

// A recipe of this world by its result, for scripts that name what they make.
std::size_t recipe_for(bool modern_world, InventoryItemId result)
{
    const auto &book = crafting_recipes(modern_world ? InventoryProfile::modern : InventoryProfile::classic);
    for (std::size_t index = 0; index < book.size(); ++index)
        if (book[index].result == result && recipe_listed(book[index]))
            return index;
    check(false, "no recipe makes item " + std::to_string(static_cast<int>(result)));
    return 0U;
}

// ------------------------------------------------------------------ scripts

void the_same_actions_leave_the_same_inventory_in_every_style()
{
    using I = InventoryItemId;
    int compared = 0;
    for (const bool modern_world : {false, true})
    {
        const std::string world = modern_world ? "modern." : "classic.";
        const std::uint16_t limit = item_stack_limit(I::dirt_block, modern_world ? InventoryProfile::modern
                                                                               : InventoryProfile::classic);
        // Pick up, put down, swap, merge up to the WORLD's stack limit.
        compared += run_everywhere(
            world + "move", modern_world, false,
            {{0U, I::dirt_block, static_cast<std::uint16_t>(limit - 4U)}, {1U, I::dirt_block, 20U},
             {2U, I::rock_block, 7U}, {32U, I::stone_pickaxe, 1U, 0.6F}, {33U, I::pistol, 1U}},
            {{K::point, 1U}, {K::left}, {K::point, 0U}, {K::left},   // merge: 4 fit, 16 stay held
             {K::point, 5U}, {K::left},                               // put the rest down
             {K::point, 2U}, {K::left}, {K::point, 5U}, {K::left},   // swap rock and dirt
             {K::point, 9U}, {K::left},                               // dirt to an empty slot
             {K::point, 32U}, {K::left}, {K::point, 12U}, {K::left}, // a worn tool keeps its wear
             {K::point, 33U}, {K::left}, {K::point, 39U}, {K::left}, // a gun keeps its clip
             {K::point, 20U}, {K::left}});                            // a click on nothing
        // Split, place one, the wheel both ways.
        compared += run_everywhere(
            world + "split", modern_world, false,
            {{0U, I::stick, 11U}, {1U, I::coal, 1U}, {2U, I::stick, 3U}, {34U, I::torch, 5U}},
            {{K::point, 0U}, {K::right},                  // half (rounded down) to the cursor
             {K::point, 3U}, {K::right}, {K::right},      // one at a time
             {K::point, 2U}, {K::right},                  // one more onto a stack of the same
             {K::point, 1U}, {K::right},                  // another item: swapped
             {K::point, 4U}, {K::left},
             {K::point, 0U}, {K::wheel_up}, {K::wheel_up}, {K::wheel_up}, // three to the cursor
             {K::point, 6U}, {K::wheel_down}, {K::wheel_down},
             {K::point, 34U}, {K::wheel_down},            // not the same item: nothing
             {K::point, 7U}, {K::left},
             {K::point, 1U}, {K::right},                  // a single item goes whole
             {K::point, 8U}, {K::right}});
        // Quick move between the backpack and the tray, with and without a
        // stack on the cursor.
        compared += run_everywhere(
            world + "quick", modern_world, false,
            {{0U, I::dirt_block, 30U}, {1U, I::rock_block, 12U}, {2U, I::stick, 4U},
             {32U, I::dirt_block, 10U}, {33U, I::torch, 3U}, {34U, I::stone_pickaxe, 1U, 0.4F}},
            {{K::point, 0U}, {K::left, 0U, true},   // backpack -> tray, merging
             {K::point, 33U}, {K::left, 0U, true},  // tray -> backpack
             {K::point, 34U}, {K::left, 0U, true},
             {K::point, 1U}, {K::left}, {K::point, 36U}, {K::left, 0U, true}, // held: to the backpack
             {K::point, 2U}, {K::left}, {K::point, 10U}, {K::left, 0U, true}, // held: to the tray
             {K::point, 20U}, {K::left, 0U, true}});                          // an empty slot
        // Throwing: the whole stack, one item, and the drop key.
        compared += run_everywhere(
            world + "throw", modern_world, false,
            {{0U, I::dirt_block, 9U}, {1U, I::rock_block, 3U}, {2U, I::coal, 5U}, {35U, I::torch, 2U}},
            {{K::point, 0U}, {K::left}, {K::outside}, {K::right}, {K::right}, // two single items
             {K::outside}, {K::left},                                          // the rest
             {K::outside}, {K::left},                                          // nothing held: nothing
             {K::point, 1U}, {K::drop},                                        // the slot under the pointer
             {K::point, 2U}, {K::left}, {K::drop},                             // the stack on the cursor
             {K::point, 35U}, {K::drop}, {K::point, 20U}, {K::drop}});         // an empty slot
        // The crate: in, out, across with Shift, and a slot another player holds.
        compared += run_everywhere(
            world + "crate", modern_world, true,
            {{0U, I::dirt_block, 40U}, {1U, I::rock_block, 6U}, {32U, I::torch, 8U},
             {100U, I::dirt_block, 30U}, {101U, I::gold, 4U}, {131U, I::coal, 2U}},
            {{K::point, 0U}, {K::left}, {K::point, 100U}, {K::left},     // merge into the crate
             {K::point, 110U}, {K::left},                                 // the rest to an empty crate slot
             {K::point, 101U}, {K::left, 0U, true},                       // crate -> player
             {K::point, 1U}, {K::left, 0U, true},                         // player -> crate
             {K::point, 32U}, {K::left, 0U, true},
             {K::point, 131U}, {K::right}, {K::point, 120U}, {K::right},  // split inside the crate
             {K::point, 5U}, {K::left},
             {K::lock, 100U}, {K::point, 100U}, {K::left}, {K::right}, {K::left, 0U, true}, {K::drop},
             {K::wheel_up}, {K::lock, 99U}, {K::point, 100U}, {K::wheel_up}, {K::point, 6U}, {K::wheel_down},
             {K::point, 110U}, {K::drop}, {K::outside}, {K::left}});
        // The second tray, where the world has one.
        if (modern_world)
            compared += run_everywhere(
                world + "tray-two", true, false,
                {{0U, I::dirt_block, 5U}, {32U, I::torch, 3U}, {40U, I::stick, 6U}},
                {{K::tray, 1U}, {K::point, 0U}, {K::left}, {K::point, 41U}, {K::left},
                 {K::point, 40U}, {K::right}, {K::point, 47U}, {K::left},
                 {K::point, 41U}, {K::left, 0U, true}, {K::tray, 0U}, {K::point, 32U}, {K::left, 0U, true}});
        // Crafting: once, all, and what cannot be made. Sticks come from wood
        // in both releases; torches need sticks and coal.
        const std::size_t sticks = recipe_for(modern_world, I::stick);
        const std::size_t torches = recipe_for(modern_world, I::torch);
        const std::size_t pickaxe = recipe_for(modern_world, I::stone_pickaxe);
        compared += run_everywhere(
            world + "craft", modern_world, false,
            {{0U, I::wood_block, 6U}, {1U, I::coal, 3U}, {2U, I::rock_block, 40U}, {3U, I::stick, 1U},
             {4U, I::log_block, 5U}},
            {{K::craft, sticks}, {K::craft, torches}, {K::craft, torches, true}, // all the coal allows
             {K::craft, torches},                                                 // cannot be made now
             {K::craft, sticks, true}, {K::craft, pickaxe}, {K::craft, pickaxe, true},
             {K::point, 0U}, {K::left}, {K::point, 9U}, {K::left}});
        // Creative and infinite resources follow the world's rules, not the
        // style: 1.9.9.8 makes anything in Creative and ignores "all" there.
        compared += run_everywhere(
            world + "craft-modes", modern_world, false,
            {{0U, I::wood_block, 4U}, {1U, I::coal, 2U}, {2U, I::stick, 2U}},
            {{K::infinite, 1U}, {K::craft, sticks}, {K::craft, sticks, true}, {K::craft, torches, true},
             {K::infinite, 0U}, {K::creative, 1U}, {K::craft, pickaxe}, {K::craft, pickaxe, true},
             {K::creative, 0U}, {K::craft, pickaxe}, {K::craft, sticks, true}});
    }
    check(compared >= 400, "expected the scripts to compare several hundred states, compared " +
                               std::to_string(compared));
}

void random_actions_never_separate_the_styles()
{
    using I = InventoryItemId;
    constexpr std::array<I, 8> items{I::dirt_block, I::rock_block, I::stick,  I::coal,
                                     I::torch,      I::iron,       I::bullets, I::stone_pickaxe};
    int compared = 0;
    for (const bool modern_world : {false, true})
        for (const bool crate : {false, true})
            for (std::uint32_t seed = 1U; seed <= 6U; ++seed)
            {
                std::uint32_t state = seed * 2654435761U + (modern_world ? 97U : 0U) + (crate ? 13U : 0U);
                const auto next = [&](std::uint32_t range) {
                    state = state * 1664525U + 1013904223U;
                    return (state >> 8U) % range;
                };
                const InventoryProfile profile = modern_world ? InventoryProfile::modern : InventoryProfile::classic;
                std::vector<Given> given;
                for (std::size_t slot = 0; slot < 40U; ++slot)
                    if (next(3U) != 0U)
                    {
                        const I item = items[next(static_cast<std::uint32_t>(items.size()))];
                        const std::uint16_t limit = item_stack_limit(item, profile);
                        given.push_back({slot, item, static_cast<std::uint16_t>(1U + next(std::min<std::uint32_t>(limit, 70U))),
                                         item == I::stone_pickaxe ? 0.5F : 1.0F});
                    }
                if (crate)
                    for (std::size_t slot = 0; slot < 32U; ++slot)
                        if (next(3U) == 0U)
                        {
                            const I item = items[next(static_cast<std::uint32_t>(items.size()))];
                            given.push_back({screens::crate_base + slot, item,
                                             static_cast<std::uint16_t>(1U + next(std::min<std::uint32_t>(
                                                                                 item_stack_limit(item, profile), 70U)))});
                        }
                std::vector<Action> script;
                for (int step = 0; step < 220; ++step)
                {
                    // A slot every style shows: the backpack, the tray in
                    // hand, and the crate when one is open.
                    const std::uint32_t where = next(crate ? 72U : 40U);
                    const std::size_t slot = where < 40U ? where : screens::crate_base + (where - 40U);
                    const std::uint32_t what = next(20U);
                    if (what == 0U)
                    {
                        script.push_back({K::outside});
                        script.push_back({next(2U) == 0U ? K::left : K::right});
                        continue;
                    }
                    script.push_back({K::point, slot});
                    if (what < 9U)
                        script.push_back({K::left});
                    else if (what < 13U)
                        script.push_back({K::right});
                    else if (what < 15U)
                        script.push_back({K::left, 0U, true});
                    else if (what < 17U)
                        script.push_back({K::wheel_up});
                    else if (what < 19U)
                        script.push_back({K::wheel_down});
                    else
                        script.push_back({K::drop});
                }
                compared += run_everywhere(std::string{modern_world ? "modern" : "classic"} +
                                               (crate ? ".random-crate." : ".random.") + std::to_string(seed),
                                           modern_world, crate, given, script);
            }
    check(compared >= 5000, "expected thousands of random states, compared " + std::to_string(compared));
}

// ------------------------------------------------- Remastered's other routes

// Keys and the gamepad reach the operations the pointer does: the same world,
// the same slots, once with the pointer and once without it.
void remastered_keys_do_what_its_pointer_does()
{
    using I = InventoryItemId;
    for (const bool modern_world : {false, true})
    {
        const std::vector<Given> items{{0U, I::dirt_block, 9U}, {1U, I::rock_block, 4U}, {9U, I::stick, 6U},
                                       {32U, I::torch, 3U},     {100U, I::coal, 5U}};
        for (const bool crate : {false, true})
        {
            Styled pointer{UiStyle::remastered, modern_world};
            Styled keys{UiStyle::remastered, modern_world};
            for (Styled *view : {&pointer, &keys})
            {
                give(view->host, items);
                view->open(crate);
            }
            const std::string label = std::string{modern_world ? "modern" : "classic"} + (crate ? " crate" : " inventory");
            // The cursor opens on the item in hand: tray slot 0 of tray 0.
            check(keys.session.selected_slot() == screens::tray_base && keys.session.selector_visible(),
                  label + ": the keyboard cursor must start on the item in hand");
            const auto same = [&](const std::string &after) {
                check(keys.state() == pointer.state(), label + " after " + after + ":\n  keys    " + keys.state() +
                                                           "\n  pointer " + pointer.state());
            };
            // Walk from the tray up to backpack slot 0 (row 0, column 0).
            const auto walk_to = [&](std::size_t target) {
                for (int guard = 0; guard < 64 && keys.session.selected_slot() != target; ++guard)
                {
                    const std::size_t at = keys.session.selected_slot();
                    const bool at_crate = at >= screens::crate_base;
                    const bool to_crate = target >= screens::crate_base;
                    if (at_crate != to_crate)
                    {
                        keys.key(screens::Key::switch_pane);
                        continue;
                    }
                    const auto row_of = [](std::size_t slot) {
                        const std::size_t local = slot >= screens::crate_base ? slot - screens::crate_base : slot;
                        return local < 32U ? local / 8U : 4U + (local - screens::tray_base) / 8U;
                    };
                    const auto column_of = [](std::size_t slot) {
                        const std::size_t local = slot >= screens::crate_base ? slot - screens::crate_base : slot;
                        return local < 32U ? local % 8U : (local - screens::tray_base) % 8U;
                    };
                    if (row_of(at) != row_of(target))
                        keys.key(row_of(at) < row_of(target) ? screens::Key::down : screens::Key::up);
                    else
                        keys.key(column_of(at) < column_of(target) ? screens::Key::right : screens::Key::left);
                }
                check(keys.session.selected_slot() == target,
                      label + ": the arrow keys could not reach slot " + std::to_string(target));
            };
            walk_to(0U);
            keys.key(screens::Key::enter); // pick up
            pointer.point_slot(0U);
            pointer.click(screens::Button::left);
            same("Enter / left click (pick up)");
            walk_to(1U);
            keys.key(screens::Key::enter); // swap with the rock
            pointer.point_slot(1U);
            pointer.click(screens::Button::left);
            same("Enter / left click (swap)");
            walk_to(2U);
            keys.key(screens::Key::split); // place one
            keys.key(screens::Key::split);
            pointer.point_slot(2U);
            pointer.click(screens::Button::right);
            pointer.click(screens::Button::right);
            same("split key / right click (place one)");
            walk_to(3U);
            keys.key(screens::Key::enter);
            pointer.point_slot(3U);
            pointer.click(screens::Button::left);
            same("Enter (put down)");
            walk_to(9U);
            keys.key(screens::Key::split); // half the sticks
            pointer.point_slot(9U);
            pointer.click(screens::Button::right);
            same("split key / right click (split)");
            walk_to(10U);
            keys.key(screens::Key::enter);
            pointer.point_slot(10U);
            pointer.click(screens::Button::left);
            walk_to(1U);
            keys.key(screens::Key::transfer); // quick move
            pointer.point_slot(1U);
            pointer.click(screens::Button::left, true);
            same("transfer key / Shift + left click");
            walk_to(2U);
            keys.key(screens::Key::enter, true); // Shift + Enter is the same quick move
            pointer.point_slot(2U);
            pointer.click(screens::Button::left, true);
            same("Shift + Enter / Shift + left click");
            if (crate)
            {
                walk_to(screens::crate_base);
                keys.key(screens::Key::transfer);
                pointer.point_slot(screens::crate_base);
                pointer.click(screens::Button::left, true);
                same("transfer key in the crate");
            }
            walk_to(9U);
            keys.key(screens::Key::drop);
            pointer.point_slot(9U);
            pointer.key(screens::Key::drop);
            same("drop key");
            keys.key(screens::Key::escape);
            pointer.key(screens::Key::inventory);
            same("Escape / E (close)");
            check(!keys.session.is_open() && !pointer.session.is_open(), label + ": Escape and E close the screen");
        }
        // Crafting from the list with keys, the row's own button and the big
        // buttons must all be the one Host::craft.
        const std::size_t sticks = recipe_for(modern_world, I::stick);
        std::array<Styled, 3> ways{Styled{UiStyle::remastered, modern_world}, Styled{UiStyle::remastered, modern_world},
                                   Styled{UiStyle::remastered, modern_world}};
        for (Styled &view : ways)
        {
            give(view.host, {{0U, I::wood_block, 5U}, {1U, I::coal, 2U}});
            view.host.crate_open = false;
            view.session.open(view.host, screens::ScreenId::crafting);
            view.frame();
            check(view.session.recipes_focused(), "asking for crafting puts the cursor in the recipe list");
            view.session.select_recipe(view.host, sticks);
            view.frame();
        }
        const std::string world = modern_world ? "modern" : "classic";
        // 1: Enter, then Shift + Enter.
        ways[0].key(screens::Key::enter);
        // 2: the row's own button.
        const auto list = ways[1].session.tab_recipes(ways[1].host);
        const auto position = static_cast<std::size_t>(std::find(list.begin(), list.end(), sticks) - list.begin());
        check(position < list.size() && position >= ways[1].session.recipe_scroll(),
              world + ": the selected recipe must be in view");
        ways[1].point(ways[1].session.row_craft_rect(position - ways[1].session.recipe_scroll())->center());
        ways[1].click(screens::Button::left);
        // 3: the Craft button.
        ways[2].point(ways[2].session.craft_rect(false)->center());
        ways[2].click(screens::Button::left);
        check(ways[0].state() == ways[1].state() && ways[0].state() == ways[2].state() && ways[0].host.crafts == 1 &&
                  ways[1].host.crafts == 1 && ways[2].host.crafts == 1,
              world + ": Enter, the row button and the Craft button must each craft exactly once");
        ways[0].key(screens::Key::enter, true);
        ways[1].session.select_recipe(ways[1].host, sticks);
        ways[1].frame();
        ways[1].point(ways[1].session.craft_rect(true)->center());
        ways[1].click(screens::Button::left);
        ways[2].point(ways[2].session.craft_rect(false)->center());
        ways[2].click(screens::Button::left, true);
        check(ways[0].state() == ways[1].state() && ways[0].state() == ways[2].state(),
              world + ": Shift + Enter, Craft all and Shift + Craft must make the same amount");
        check(ways[0].host.inventory().count(I::wood_block) == 0U,
              world + ": craft-all uses every ingredient the world lets it");
        // With a stack on the cursor nothing is crafted (neither retail
        // crafting screen can be used while holding one).
        Styled holding{UiStyle::remastered, modern_world};
        give(holding.host, {{0U, I::wood_block, 5U}, {1U, I::coal, 2U}});
        holding.open(false);
        holding.point_slot(1U);
        holding.click(screens::Button::left);
        holding.session.select_recipe(holding.host, sticks);
        holding.point(holding.session.craft_rect(false)->center());
        holding.click(screens::Button::left);
        check(holding.host.crafts == 0 && holding.session.held().has_value(),
              world + ": crafting with a stack on the cursor must do nothing");
    }
}

// Every recipe the world's own crafting screen lists is under exactly one of
// Remastered's four category tabs and in "All"; nothing else is listed.
void remastered_lists_what_the_world_lists()
{
    for (const bool modern_world : {false, true})
    {
        Styled view{UiStyle::remastered, modern_world};
        const InventoryProfile profile = modern_world ? InventoryProfile::modern : InventoryProfile::classic;
        const auto &book = crafting_recipes(profile);
        // Hold one of everything any recipe needs, so a Classic world has
        // discovered its whole cookbook.
        std::size_t slot = 0;
        for (const CraftingRecipe &recipe : book)
            for (const CraftingIngredient &ingredient : recipe.ingredients)
                if (ingredient.count != 0U && view.host.inventory().count(ingredient.item) == 0U && slot < 32U)
                    view.host.inventory().storage()[slot++] = InventoryStack{ingredient.item, 1U};
        view.open(false);
        const std::string world = modern_world ? "modern" : "classic";
        const auto everything = view.session.tab_recipes(view.host, screens::RecipeTab::all);
        std::vector<int> seen(book.size(), 0);
        for (const screens::RecipeTab tab : {screens::RecipeTab::materials, screens::RecipeTab::tools,
                                             screens::RecipeTab::weapons, screens::RecipeTab::structures})
            for (const std::size_t index : view.session.tab_recipes(view.host, tab))
                ++seen[index];
        std::size_t listed = 0;
        for (const std::size_t index : everything)
        {
            check(recipe_listed(book[index]), world + ": a recipe no crafting screen lists is in Remastered's list");
            check(seen[index] == 1, world + ": " + std::string{item_name(book[index].result)} + " is under " +
                                        std::to_string(seen[index]) + " category tabs");
            ++listed;
        }
        std::size_t expected = 0;
        if (modern_world)
            for (const CraftingRecipe &recipe : book)
                expected += recipe_listed(recipe) ? 1U : 0U;
        else
            expected = view.session.strip_recipes(view.host).size();
        check(listed == expected, world + ": Remastered lists " + std::to_string(listed) + " recipes, the world's own screen " +
                                      std::to_string(expected));
        // What can be made comes first, and only that is marked as makeable.
        bool reached_unmakeable = false;
        for (const std::size_t index : everything)
        {
            const bool can = view.host.can_craft(book[index]);
            check(!(can && reached_unmakeable), world + ": a makeable recipe is listed after one that is not");
            reached_unmakeable = reached_unmakeable || !can;
        }
    }
    // A Classic world lists only what has been discovered, as its own screen does.
    Styled fresh{UiStyle::remastered, false};
    fresh.host.inventory().storage()[0] = InventoryStack{InventoryItemId::wood_block, 2U};
    fresh.open(false);
    const auto &book = crafting_recipes(InventoryProfile::classic);
    auto listed = fresh.session.tab_recipes(fresh.host, screens::RecipeTab::all);
    auto discovered = fresh.session.strip_recipes(fresh.host);
    std::sort(listed.begin(), listed.end());
    std::sort(discovered.begin(), discovered.end());
    check(listed == discovered, "a Classic world's Remastered list is exactly its discovered recipes");
    check(!listed.empty() && listed.size() < book.size(),
          "a Classic world with one item has not discovered the whole cookbook");
}

// ------------------------------------------------------------------- layout

struct Window final
{
    int width{};
    int height{};
};

constexpr std::array<Window, 14> windows{{
    {1280, 720},  {1366, 768},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3840, 2160}, // 16:9
    {1280, 800},  {1920, 1200}, {2560, 1600},                                            // 16:10
    {2560, 1080}, {3440, 1440},                                                          // 21:9
    {1024, 768},  {1600, 1200}, {960, 540},                                              // 4:3 and the minimum window
}};
constexpr std::array<float, 4> scales{0.75F, 1.0F, 1.25F, 1.5F};
constexpr std::array<float, 3> margins{1.0F, 0.96F, 0.80F};

void one_unit_is_the_same_fraction_of_every_16_9_picture()
{
    namespace rm = remastered;
    const rm::Canvas base = rm::canvas(1280, 720, 1.0F, 1.0F);
    check(std::fabs(base.scale - 1.0F) < 1e-5F && std::fabs(base.width - 1280.0F) < 1e-3F &&
              std::fabs(base.height - 720.0F) < 1e-3F,
          "1280x720 at 100% is one pixel per unit on a 1280x720 canvas");
    for (const Window size : {Window{1920, 1080}, Window{2560, 1440}, Window{3840, 2160}})
    {
        const rm::Canvas canvas = rm::canvas(size.width, size.height, 1.0F, 1.0F);
        check(std::fabs(canvas.scale - static_cast<float>(size.height) / 720.0F) < 1e-4F,
              "a 16:9 window scales by height / 720, as both retail styles do");
        check(std::fabs(canvas.width - 1280.0F) < 0.01F && std::fabs(canvas.height - 720.0F) < 0.01F,
              "every 16:9 window is the same 1280x720 canvas: nothing is smaller at 4K");
    }
    // Narrower than 16:9 the width decides (the Classic buffer's own rule), so
    // the canvas is 1280 wide and taller; wider than 16:9 the height decides.
    const rm::Canvas four_three = rm::canvas(1600, 1200, 1.0F, 1.0F);
    check(std::fabs(four_three.scale - 1.25F) < 1e-4F && std::fabs(four_three.width - 1280.0F) < 0.01F &&
              four_three.height > 720.0F,
          "4:3 keeps the full 1280-unit width");
    const rm::Canvas ultrawide = rm::canvas(3440, 1440, 1.0F, 1.0F);
    check(std::fabs(ultrawide.scale - 2.0F) < 1e-4F && ultrawide.width > 1280.0F &&
              std::fabs(ultrawide.height - 720.0F) < 0.01F,
          "21:9 keeps the full 720-unit height and gains width");
    // The interface scale multiplies the unit until the content would no
    // longer fit, and never past that.
    check(std::fabs(rm::canvas(1920, 1080, 1.25F, 1.0F).scale - 1.875F) < 1e-4F, "interface scale multiplies the unit");
    for (const Window size : windows)
        for (const float scale : scales)
            for (const float margin : margins)
            {
                const rm::Canvas canvas = rm::canvas(size.width, size.height, scale, margin);
                check(canvas.safe.width >= rm::hud_min_width - 0.01F && canvas.safe.height >= rm::hud_min_height - 0.01F,
                      "the HUD always has its minimum room inside the margins");
                check(canvas.scale > 0.0F && std::fabs(canvas.width * canvas.scale - static_cast<float>(size.width)) < 0.01F &&
                          std::fabs(canvas.height * canvas.scale - static_cast<float>(size.height)) < 0.01F,
                      "the canvas is the whole window");
            }
    // Garbage in, a usable canvas out.
    const rm::Canvas damaged = rm::canvas(0, -5, std::nanf(""), std::nanf(""));
    check(damaged.scale > 0.0F && std::isfinite(damaged.width) && std::isfinite(damaged.height),
          "a damaged size or setting still gives a finite canvas");
}

void the_hud_fits_every_window()
{
    namespace rm = remastered;
    int layouts = 0;
    for (const Window size : windows)
        for (const float scale : scales)
            for (const float margin : margins)
                for (const VitalsStyle vitals : {VitalsStyle::remastered, VitalsStyle::classic, VitalsStyle::modern,
                                                 VitalsStyle::none})
                    for (const bool modern_world : {false, true})
                        for (const bool extras : {false, true})
                        {
                            const rm::Canvas canvas = rm::canvas(size.width, size.height, scale, margin);
                            rm::HudOptions options;
                            options.two_trays = modern_world;
                            options.stamina = modern_world;
                            options.vitals = vitals;
                            options.compass = true;
                            options.clock = extras;
                            options.oxygen = extras;
                            options.server = extras;
                            options.coordinates = extras;
                            options.frame_counter = extras;
                            const rm::HudLayout hud = rm::hud_layout(canvas, options);
                            const std::string label =
                                std::to_string(size.width) + "x" + std::to_string(size.height) + " scale " +
                                std::to_string(scale) + " margin " + std::to_string(margin) + " vitals " +
                                std::to_string(static_cast<int>(vitals)) + (modern_world ? " modern" : " classic") +
                                (extras ? " extras" : "");
                            struct Named final
                            {
                                const char *name;
                                rm::Rect rect;
                            };
                            std::vector<Named> parts{{"tray", hud.tray},           {"other tray", hud.other_tray},
                                                     {"health", hud.health},       {"stamina", hud.stamina},
                                                     {"item line", hud.item_line}, {"compass", hud.compass},
                                                     {"distance", hud.distance},   {"oxygen", hud.oxygen},
                                                     {"server", hud.server},       {"coordinates", hud.coordinates},
                                                     {"clock", hud.clock},         {"notice", hud.notice},
                                                     {"chat", hud.chat},           {"roster", hud.roster},
                                                     {"target line", hud.target_line}};
                            // Slightly larger than the margins for float noise.
                            const rm::Rect safe{canvas.safe.x - 0.01F, canvas.safe.y - 0.01F, canvas.safe.width + 0.02F,
                                                canvas.safe.height + 0.02F};
                            for (const Named &part : parts)
                                check(part.rect.empty() || part.rect.inside(safe),
                                      label + ": the " + part.name + " leaves the screen margins");
                            for (std::size_t index = 0; index < rm::tray_slots; ++index)
                            {
                                check(hud.slots[index].inside(hud.tray), label + ": a tray slot is outside the tray");
                                check(index == 0U || !hud.slots[index].overlaps(hud.slots[index - 1U]),
                                      label + ": tray slots overlap");
                                if (!hud.other_tray.empty())
                                    check(hud.other_slots[index].inside(hud.other_tray),
                                          label + ": a slot of the other tray is outside its strip");
                            }
                            // Nothing that is on screen together may overlap.
                            // (The roster replaces the HUD body while it is
                            // held open, and the target line sits alone under
                            // the crosshair.)
                            for (std::size_t first = 0; first < parts.size(); ++first)
                                for (std::size_t second = first + 1U; second < parts.size(); ++second)
                                {
                                    if (std::string_view{parts[first].name} == "roster" ||
                                        std::string_view{parts[second].name} == "roster")
                                        continue;
                                    check(!parts[first].rect.overlaps(parts[second].rect),
                                          label + ": the " + parts[first].name + " and the " + parts[second].name +
                                              " overlap");
                                }
                            check(!hud.tray.empty() && hud.health.empty() == (vitals == VitalsStyle::none) &&
                                      hud.stamina.empty() == (vitals == VitalsStyle::none || !modern_world),
                                  label + ": the meters shown must follow the setting and the world");
                            // The tray is centred and the crosshair is the
                            // centre of the picture.
                            check(std::fabs(hud.tray.center_x() - canvas.width * 0.5F) <= 0.5F &&
                                      std::fabs(hud.center_x - canvas.width * 0.5F) < 0.01F &&
                                      std::fabs(hud.center_y - canvas.height * 0.5F) < 0.01F,
                                  label + ": the tray and the crosshair are centred");
                            ++layouts;
                        }
    check(layouts >= 2000, "expected thousands of HUD layouts, checked " + std::to_string(layouts));
    // Not microscopic at 4K: the tray is the same share of a 16:9 picture.
    const rm::HudLayout small = rm::hud_layout(rm::canvas(1280, 720, 1.0F, 0.96F), {});
    const rm::Canvas big_canvas = rm::canvas(3840, 2160, 1.0F, 0.96F);
    const rm::HudLayout big = rm::hud_layout(big_canvas, {});
    check(std::fabs(small.tray.width - big.tray.width) < 0.01F &&
              std::fabs(big.slots[0].width * big_canvas.scale - small.slots[0].width * 3.0F) < 0.05F,
          "a tray slot at 3840x2160 is three times its 1280x720 size in pixels");
}

void the_panel_fits_every_window_and_the_pointer_finds_every_slot()
{
    namespace rm = remastered;
    int checked = 0;
    for (const Window size : windows)
        for (const float scale : scales)
            for (const float margin : margins)
                for (const bool modern_world : {false, true})
                    for (const bool crate : {false, true})
                    {
                        WorldHost host{modern_world};
                        host.crate_open = crate;
                        screens::Session session;
                        session.configure(UiStyle::remastered, size.width, size.height, scale, margin);
                        session.open(host, crate ? screens::ScreenId::crate : screens::ScreenId::inventory);
                        const std::string label = std::to_string(size.width) + "x" + std::to_string(size.height) + " scale " +
                                                  std::to_string(scale) + " margin " + std::to_string(margin) +
                                                  (modern_world ? " modern" : " classic") + (crate ? " crate" : " inventory");
                        const auto view = session.remastered_view();
                        const screens::IntRect panel = session.panel();
                        const float left = view.origin_x;
                        const float top = view.origin_y;
                        const float right = left + static_cast<float>(panel.width) * view.scale;
                        const float bottom = top + static_cast<float>(panel.height) * view.scale;
                        const float inset_x = static_cast<float>(size.width) * (1.0F - margin) * 0.5F;
                        const float inset_y = static_cast<float>(size.height) * (1.0F - margin) * 0.5F;
                        check(left >= inset_x - 1.0F && top >= inset_y - 1.0F &&
                                  right <= static_cast<float>(size.width) - inset_x + 1.0F &&
                                  bottom <= static_cast<float>(size.height) - inset_y + 1.0F,
                              label + ": the panel leaves the screen margins");
                        check(view.trays == (modern_world ? 2 : 1), label + ": one tray row per tray the world has");
                        // The panel is centred.
                        check(std::fabs((left + right) * 0.5F - static_cast<float>(size.width) * 0.5F) <= 1.0F &&
                                  std::fabs((top + bottom) * 0.5F - static_cast<float>(size.height) * 0.5F) <= 1.0F,
                              label + ": the panel is centred");
                        // Every slot is inside the panel, none overlaps another
                        // and the pointer placed on it selects it.
                        std::vector<std::size_t> slots;
                        for (std::size_t slot = 0; slot < 32U; ++slot)
                            slots.push_back(slot);
                        for (std::size_t slot = 0; slot < (modern_world ? 16U : 8U); ++slot)
                            slots.push_back(screens::tray_base + slot);
                        if (crate)
                            for (std::size_t slot = 0; slot < 32U; ++slot)
                                slots.push_back(screens::crate_base + slot);
                        std::vector<screens::IntRect> rects;
                        for (const std::size_t slot : slots)
                        {
                            const auto rect = session.slot_rect(slot);
                            check(rect.has_value(), label + ": slot " + std::to_string(slot) + " has no place");
                            if (!rect)
                                continue;
                            check(rect->x >= 0 && rect->y >= 0 && rect->right() <= panel.width && rect->bottom() <= panel.height,
                                  label + ": slot " + std::to_string(slot) + " is outside the panel");
                            for (const screens::IntRect &other : rects)
                                check(rect->right() <= other.x || other.right() <= rect->x || rect->bottom() <= other.y ||
                                          other.bottom() <= rect->y,
                                      label + ": slot " + std::to_string(slot) + " overlaps another");
                            rects.push_back(*rect);
                            // Its middle and two opposite corners, two window
                            // pixels in (below 100% scale a unit is less than
                            // a pixel, so "one unit in" is not a pixel inside).
                            const int inset = static_cast<int>(std::ceil(2.0F / view.scale)) + 1;
                            for (const screens::Point at :
                                 {rect->center(), screens::Point{rect->x + inset, rect->y + inset},
                                  screens::Point{rect->right() - 1 - inset, rect->bottom() - 1 - inset}})
                            {
                                const screens::Point window = session.to_window(at);
                                session.pointer_moved(host, window.x, window.y);
                                check(session.selected_slot() == slot && session.selector_visible(),
                                      label + ": the pointer on slot " + std::to_string(slot) + " selected slot " +
                                          std::to_string(session.selected_slot()));
                            }
                            ++checked;
                        }
                        if (!modern_world)
                            check(!session.slot_rect(screens::tray_base + 8U).has_value(),
                                  label + ": a one-tray world shows no second tray");
                        if (crate)
                            continue;
                        // The crafting controls are inside the panel, clear of
                        // the slots and of each other.
                        std::vector<screens::IntRect> controls;
                        for (std::size_t tab = 0; tab < screens::recipe_tab_count; ++tab)
                            controls.push_back(*session.tab_rect(static_cast<screens::RecipeTab>(tab)));
                        for (int row = 0; row < rm::recipe_rows(view.trays); ++row)
                            controls.push_back(*session.recipe_row_rect(static_cast<std::size_t>(row)));
                        controls.push_back(*session.craft_rect(false));
                        controls.push_back(*session.craft_rect(true));
                        controls.push_back(*session.close_rect());
                        check(rm::recipe_rows(view.trays) >= 8, label + ": at least eight recipes are in view");
                        for (std::size_t first = 0; first < controls.size(); ++first)
                        {
                            const screens::IntRect &a = controls[first];
                            check(a.x >= 0 && a.y >= 0 && a.right() <= panel.width && a.bottom() <= panel.height,
                                  label + ": a crafting control is outside the panel");
                            for (const screens::IntRect &slot : rects)
                                check(a.right() <= slot.x || slot.right() <= a.x || a.bottom() <= slot.y || slot.bottom() <= a.y,
                                      label + ": a crafting control overlaps a slot");
                            for (std::size_t second = first + 1U; second < controls.size(); ++second)
                            {
                                const screens::IntRect &b = controls[second];
                                check(a.right() <= b.x || b.right() <= a.x || a.bottom() <= b.y || b.bottom() <= a.y,
                                      label + ": two crafting controls overlap");
                            }
                        }
                    }
    check(checked >= 20000, "expected every slot at every size, checked " + std::to_string(checked));
}

// The retail styles are where the original puts them whatever the interface
// scale and margins say: those two settings belong to Remastered and menus.
void retail_layouts_ignore_the_remastered_settings()
{
    for (const UiStyle style : {UiStyle::classic, UiStyle::modern})
        for (const bool crate : {false, true})
        {
            WorldHost host{style == UiStyle::modern};
            host.crate_open = crate;
            screens::Session plain;
            screens::Session scaled;
            plain.configure(style, 1920, 1080);
            scaled.configure(style, 1920, 1080, 1.5F, 0.8F);
            for (screens::Session *session : {&plain, &scaled})
                session->open(host, crate ? screens::ScreenId::crate : screens::ScreenId::inventory);
            check(plain.panel() == scaled.panel(), std::string{style_name(style)} + ": the panel moved with the interface scale");
            for (std::size_t slot = 0; slot < 40U; ++slot)
                check(plain.slot_rect(slot) == scaled.slot_rect(slot),
                      std::string{style_name(style)} + ": a slot moved with the interface scale");
        }
    // One style per session: there is no second, crafting, style to cross it with.
    WorldHost classic{false};
    screens::Session session;
    session.configure(UiStyle::classic, 1280, 720);
    session.open(classic, screens::ScreenId::inventory);
    session.key_pressed(classic, screens::Key::inventory);
    check(session.layout() == screens::Layout::classic_crafting, "the Classic style crafts on the Classic crafting screen");
    session.configure(UiStyle::modern, 1280, 720);
    session.open(classic, screens::ScreenId::crafting);
    check(session.layout() == screens::Layout::modern_inventory && !session.button_rect().has_value(),
          "the Modern style crafts on the Modern screen's own recipe tree, with no button to another style");
    session.configure(UiStyle::remastered, 1280, 720);
    session.open(classic, screens::ScreenId::crafting);
    check(session.layout() == screens::Layout::remastered_inventory, "the Remastered style crafts on its own screen");
    session.configure(UiStyle::automatic, 1280, 720);
    check(session.style() == UiStyle::classic, "an unresolved style reaching a session is a retail look, never ours");
}

struct TestCase final
{
    const char *name;
    void (*body)();
};

} // namespace

int main()
{
    const std::array tests{
        TestCase{"the same actions leave the same inventory in every style",
                 the_same_actions_leave_the_same_inventory_in_every_style},
        TestCase{"random actions never separate the styles", random_actions_never_separate_the_styles},
        TestCase{"Remastered keys do what its pointer does", remastered_keys_do_what_its_pointer_does},
        TestCase{"Remastered lists what the world lists", remastered_lists_what_the_world_lists},
        TestCase{"one unit is the same fraction of every 16:9 picture",
                 one_unit_is_the_same_fraction_of_every_16_9_picture},
        TestCase{"the HUD fits every window", the_hud_fits_every_window},
        TestCase{"the panel fits every window and the pointer finds every slot",
                 the_panel_fits_every_window_and_the_pointer_finds_every_slot},
        TestCase{"retail layouts ignore the Remastered settings", retail_layouts_ignore_the_remastered_settings},
    };
    for (const TestCase &test : tests)
    {
        const int before = failures;
        test.body();
        std::cout << (failures == before ? "ok   " : "FAIL ") << test.name << '\n';
    }
    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
