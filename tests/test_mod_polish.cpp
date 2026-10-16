// Polish (modding wave 3): mod items and mod recipes on the retail crafting
// rules and screens. The runtime's overrun policy and the ignored editor
// files are pinned in test_mod_runtime.cpp.

#include "openminerz/inventory.hpp"
#include "openminerz/screens.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
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

constexpr std::uint32_t canteen = 99U; // a mod item: fallback compass
constexpr std::uint32_t herb = 77U;    // a mod item: fallback stick

void clear(PlayerInventory &inventory)
{
    for (auto &slot : inventory.storage())
        slot.reset();
    for (auto &tray : inventory.trays())
        for (auto &slot : tray)
            slot.reset();
}

InventoryStack mod_stack(InventoryItemId fallback, std::uint16_t count, std::uint32_t mod_item)
{
    InventoryStack stack{fallback, count};
    stack.mod_item = mod_item;
    return stack;
}

std::size_t retail_recipe(InventoryProfile profile, InventoryItemId result)
{
    const auto &book = crafting_recipes(profile);
    for (std::size_t index = 0; index < book.size(); ++index)
        if (book[index].result == result && recipe_listed(book[index]))
            return index;
    throw std::runtime_error("no retail recipe makes item " + std::to_string(static_cast<int>(result)));
}

// A retail recipe never uses up, nor counts, a mod item standing on its
// fallback (a Trail Ration is not a stick).
void retail_crafting_leaves_mod_items_alone()
{
    for (const InventoryProfile profile : {InventoryProfile::classic, InventoryProfile::modern})
    {
        PlayerInventory inventory;
        inventory.set_profile(profile);
        clear(inventory);
        const CraftingRecipe &recipe = crafting_recipes(profile)[retail_recipe(profile, InventoryItemId::stick)];
        const CraftingIngredient &wood = recipe.ingredients[0];
        inventory.storage()[0] = mod_stack(wood.item, 50U, herb);
        check(!inventory.can_craft(recipe), "a mod item does not count as its fallback");
        inventory.storage()[1] = InventoryStack{wood.item, wood.count};
        check(inventory.can_craft(recipe), "the real ingredient does");
        static_cast<void>(inventory.craft(recipe));
        check(inventory.storage()[0].has_value() && inventory.storage()[0]->count == 50U &&
                  inventory.storage()[0]->mod_item == herb,
              "crafting took the real ingredient, never the mod item");
        check(!inventory.storage()[1].has_value(), "and used the real one up");

        CraftingRecipe mod_recipe = recipe;
        mod_recipe.mod_recipe = 1U;
        check(!inventory.can_craft(mod_recipe) && inventory.craft(mod_recipe) == 0U,
              "PlayerInventory never crafts a mod recipe (the server does)");
    }
}

// The cookbook of a world with one mod recipe: a canteen (fallback compass)
// from two herbs (fallback stick). No retail recipe can use a herb.
std::vector<CraftingRecipe> book_with_mod_recipe(InventoryProfile profile)
{
    std::vector<CraftingRecipe> book = crafting_recipes(profile);
    CraftingRecipe recipe;
    recipe.result = InventoryItemId::compass;
    recipe.result_mod_item = canteen;
    recipe.result_count = 1U;
    recipe.ingredients[0] = {InventoryItemId::stick, 2U, herb};
    recipe.mod_recipe = 41U;
    CraftingRecipe kind;
    kind.result = recipe.result;
    recipe.type = screens::recipe_type_for(kind, InventoryProfile::classic);
    book.push_back(recipe);
    return book;
}

// The app's ScreenHost, as far as recipes go: the retail book plus mod
// recipes, mod ingredients counted by identity, mod items named by the mod.
class ModHost final : public screens::Host
{
  public:
    explicit ModHost(InventoryProfile profile) : book_(book_with_mod_recipe(profile))
    {
        inventory_.set_profile(profile);
        clear(inventory_);
    }
    [[nodiscard]] PlayerInventory &inventory() noexcept override { return inventory_; }
    [[nodiscard]] const PlayerInventory &inventory() const noexcept override { return inventory_; }
    [[nodiscard]] PlayerInventory::Slot *crate_slot(std::size_t) noexcept override { return nullptr; }
    [[nodiscard]] const PlayerInventory::Slot *crate_slot(std::size_t) const noexcept override { return nullptr; }
    [[nodiscard]] bool crate_slot_locked(std::size_t) const noexcept override { return false; }
    [[nodiscard]] bool can_craft(const CraftingRecipe &recipe) const noexcept override
    {
        if (!recipe_listed(recipe))
            return false;
        if (recipe.mod_recipe == 0U)
            return inventory_.can_craft(recipe);
        for (const CraftingIngredient &ingredient : recipe.ingredients)
            if (ingredient.count != 0U && ingredient_count(ingredient) < ingredient.count)
                return false;
        return true;
    }
    void craft(std::size_t recipe_index, bool all) override { crafted.emplace_back(recipe_index, all); }
    [[nodiscard]] bool creative() const noexcept override { return false; }
    void play(screens::Cue) override {}
    void drop(const InventoryStack &) override {}
    void crate_changed(std::size_t) override {}
    [[nodiscard]] ItemText item_words(const InventoryStack &stack, InventoryProfile profile) const override
    {
        if (stack.mod_item == canteen)
            return {"Canteen", "Holds water", ""};
        if (stack.mod_item == herb)
            return {"Wild Herb", "", ""};
        return item_text(stack.item, profile);
    }
    [[nodiscard]] const std::vector<CraftingRecipe> &recipes() const override { return book_; }
    [[nodiscard]] std::uint32_t ingredient_count(const CraftingIngredient &ingredient) const override
    {
        if (ingredient.mod_item == 0U)
            return inventory_.count(ingredient.item);
        std::uint32_t total{};
        for (const auto &slot : inventory_.storage())
            if (slot && slot->item == ingredient.item && slot->mod_item == ingredient.mod_item)
                total += slot->count;
        return total;
    }

    std::vector<std::pair<std::size_t, bool>> crafted;

  private:
    std::vector<CraftingRecipe> book_;
    PlayerInventory inventory_;
};

class Metrics final : public screens::TextMetrics
{
  public:
    [[nodiscard]] screens::Extent measure(screens::Font, std::string_view text) const override
    {
        return {static_cast<float>(text.size()) * 8.0F, 16.0F};
    }
    [[nodiscard]] int line_spacing(screens::Font) const override { return 16; }
};

bool contains(const std::vector<std::size_t> &list, std::size_t index)
{
    return std::find(list.begin(), list.end(), index) != list.end();
}

// A stack discovers by what it is: herbs discover the canteen, never a retail
// recipe that wants sticks.
void discovery_follows_mod_identity()
{
    const std::vector<CraftingRecipe> book = book_with_mod_recipe(InventoryProfile::classic);
    PlayerInventory inventory;
    inventory.set_profile(InventoryProfile::classic);
    clear(inventory);
    inventory.storage()[0] = mod_stack(InventoryItemId::stick, 2U, herb);
    check(inventory.recipe_discovered(book.back()), "a herb discovers the recipe that names herbs");
    for (std::size_t index = 0; index + 1U < book.size(); ++index)
        check(!inventory.recipe_discovered(book[index]),
              "a herb discovers no retail recipe (" + std::to_string(index) + ")");
    inventory.storage()[1] = mod_stack(InventoryItemId::compass, 1U, canteen);
    inventory.storage()[0].reset();
    check(inventory.recipe_discovered(book.back()), "holding the result discovers it too");
}

// Every style lists the mod recipe where the world's own screen would, and
// crafting it asks the Host for its cookbook index.
void mod_recipes_reach_every_style()
{
    const Metrics metrics;
    for (const InventoryProfile world : {InventoryProfile::classic, InventoryProfile::modern})
    {
        const std::string label = world == InventoryProfile::modern ? "modern world" : "classic world";
        ModHost host{world};
        host.inventory().storage()[0] = mod_stack(InventoryItemId::stick, 2U, herb);
        const std::size_t mod_index = host.recipes().size() - 1U;
        const CraftingRecipe &mod = host.recipes()[mod_index];
        check(host.can_craft(mod), label + ": two herbs make a canteen");

        // Classic: the discovered strip, the only craftable recipe first.
        {
            screens::Session session;
            session.configure(UiStyle::classic, 1280, 720);
            session.open(host, screens::ScreenId::crafting);
            const auto strip = session.strip_recipes(host);
            check(!strip.empty() && strip.front() == mod_index, label + ": the Classic strip lists it first");
            check(session.selected_recipe(host) == mod_index, label + ": and selects it");
            const screens::DrawList drawn = session.draw(host, metrics);
            const bool icon = std::any_of(drawn.begin(), drawn.end(), [](const screens::Draw &draw) {
                return draw.kind == screens::Draw::Kind::sprite && draw.mod_item == canteen;
            });
            const bool herb_icon = std::any_of(drawn.begin(), drawn.end(), [](const screens::Draw &draw) {
                return draw.kind == screens::Draw::Kind::sprite && draw.mod_item == herb;
            });
            const bool named = std::any_of(drawn.begin(), drawn.end(), [](const screens::Draw &draw) {
                return draw.kind == screens::Draw::Kind::text && draw.text == "Canteen";
            });
            check(icon && herb_icon, label + ": Classic draws the canteen and herb icons");
            check(named, label + ": and names the canteen by its mod name");
            // The frame's "Create Item" button.
            const auto button = session.button_rect();
            check(button.has_value(), label + ": Classic crafting has its Create Item button");
            const screens::Point at = session.to_window({button->x + button->width / 2, button->y + button->height / 2});
            session.pointer_moved(host, at.x, at.y);
            session.button_pressed(host, screens::Button::left, false);
            session.button_released(host, screens::Button::left);
            check(!host.crafted.empty() && host.crafted.back().first == mod_index,
                  label + ": Create Item crafts it through the Host");
            session.abandon();
        }
        // Modern: the recipe tree files it under its fallback's type.
        {
            screens::Session session;
            session.configure(UiStyle::modern, 1280, 720);
            session.open(host, screens::ScreenId::inventory);
            check(recipe_listed(mod) && screens::recipe_type_for(mod, world) == mod.type,
                  label + ": the mod recipe has a listed type");
            check(contains(session.type_recipes(host, mod.type), mod_index),
                  label + ": Modern's tree lists it under that type");
            session.abandon();
        }
        // Remastered: in All and in its group's tab, craftable first.
        {
            screens::Session session;
            session.configure(UiStyle::remastered, 1280, 720);
            session.open(host, screens::ScreenId::crafting);
            const auto all = session.tab_recipes(host, screens::RecipeTab::all);
            check(!all.empty() && all.front() == mod_index, label + ": Remastered lists it first under All");
            bool grouped{};
            for (std::size_t tab = 1U; tab < screens::recipe_tab_count; ++tab)
                grouped = grouped || contains(session.tab_recipes(host, static_cast<screens::RecipeTab>(tab)), mod_index);
            check(grouped, label + ": and under one group tab");
            session.select_recipe(host, mod_index);
            const std::size_t before = host.crafted.size();
            session.craft_selected(host, false);
            check(host.crafted.size() == before + 1U && host.crafted.back().first == mod_index,
                  label + ": Craft crafts it through the Host");
            session.abandon();
        }
    }
}

struct TestCase final
{
    const char *name;
    void (*body)();
};

} // namespace

int main()
{
    const std::vector<TestCase> tests{
        {"retail_crafting_leaves_mod_items_alone", retail_crafting_leaves_mod_items_alone},
        {"discovery_follows_mod_identity", discovery_follows_mod_identity},
        {"mod_recipes_reach_every_style", mod_recipes_reach_every_style},
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
