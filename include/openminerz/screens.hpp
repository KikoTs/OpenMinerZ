#pragma once

#include "openminerz/inventory.hpp"
#include "openminerz/settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The inventory, crate and crafting screens of the two retail builds: what they
// draw and what each input does.
//
// 1.6.3.8 ("Classic") has three screens: BlockPickerScreen (the backpack and
// tray on a brushed-metal panel, with a Craft button), CraftingUIScreen (a
// vertical strip of discovered recipes beside the same panel) and CrateScreen.
// 1.9.9.8 ("Modern") has two: CraftingScreen (a tabbed recipe tree and the
// backpack side by side, both trays along the bottom) and CrateScreen.
//
// Session is those screens as a pure state machine. Its draw list is the
// original's SpriteBatch sequence - same rectangles, colours, strings and
// order, with the original's integer truncations - and its input handlers are
// the original's OnPlayerInput branches. It draws nothing and owns no items:
// the application renders the list and supplies the inventory through Host.
// tests/test_screens_parity.cpp holds both halves to recordings made by
// running the shipped screens (tools/OriginalClientInterop/ScreensParityProbe).
//
// PRESENTATION ONLY. Which screen layout is shown follows the one interface
// style (Settings::ui_style): the inventory, the crate and the crafting screen
// always belong to the same style, and there is no way to mix two. Which items
// and recipes exist, what can be crafted, stack limits and how many trays
// there are always come from the world (PlayerInventory::profile()), through
// Host. So a Classic world can be shown in Modern's layout and the reverse.
//
// The third style, Remastered, is OpenMinerZ's own arrangement (see
// remastered_layout.hpp): the same slots and the SAME operations - every
// click, wheel step, key and button ends in the functions the two retail
// layouts use (pick_or_place, split_or_place_one, quick_move, count_up/down,
// drop_selected, Host::craft) - reached by its own hit testing. It adds ways
// to reach an operation (a key for split, a button for craft-all) and never an
// operation of its own. tests/test_interface_styles.cpp runs the same action
// scripts through all three styles and requires identical inventories.
namespace openminerz::screens
{

enum class Sprite : std::uint8_t
{
    solid,           // the 1x1 white DummyTexture
    item_icon,       // InventoryItem._2DImages: a 64x64 cell of the icon atlas
    item_icon_large, // 1.9.9.8's 128x128 atlas, for icons drawn wider than 70 px
    block_ui_back,
    selector,
    single_grid,
    inventory_grid,
    hud_grid,
    tier2_back,     // 1.9.9.8
    craft_selector, // 1.9.9.8
    materials_icon, // 1.9.9.8
    tools_icon,     // 1.9.9.8
    weapons_icon,   // 1.9.9.8
    structures_icon, // 1.9.9.8
    frame_top_left, // ScalableFrame "CtrlFrame": the button's nine pieces
    frame_top_right,
    frame_bottom_left,
    frame_bottom_right,
    frame_top,
    frame_bottom,
    frame_left,
    frame_right,
    frame_center,
    count
};

enum class Font : std::uint8_t
{
    small,         // SmallFont: counts, descriptions, 1.9.9.8 labels
    medium,        // MedFont: names, hints, buttons
    myriad_medium, // MyriadMedium: the close box's "x"
    count
};

struct Size final
{
    int width{};
    int height{};
};
struct Extent final
{
    float width{};
    float height{};
};
struct Point final
{
    int x{};
    int y{};
    [[nodiscard]] constexpr bool operator==(const Point &) const noexcept = default;
};
// Microsoft.Xna.Framework.Rectangle, including its half-open Contains.
struct IntRect final
{
    int x{};
    int y{};
    int width{};
    int height{};
    [[nodiscard]] constexpr int right() const noexcept { return x + width; }
    [[nodiscard]] constexpr int bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr Point center() const noexcept { return {x + width / 2, y + height / 2}; }
    [[nodiscard]] constexpr bool contains(Point point) const noexcept
    {
        return x <= point.x && point.x < x + width && y <= point.y && point.y < y + height;
    }
    [[nodiscard]] constexpr bool operator==(const IntRect &) const noexcept = default;
};
// The four bytes the original hands to SpriteBatch. XNA blends premultiplied
// (result = colour + destination * (1 - alpha)); the dimmed recipe colour
// (64,64,64,128) relies on it, so the renderer must blend the same way.
struct Color final
{
    std::uint8_t r{255U};
    std::uint8_t g{255U};
    std::uint8_t b{255U};
    std::uint8_t a{255U};
    [[nodiscard]] constexpr bool operator==(const Color &) const noexcept = default;
};

// The sprite's key in the build's sheet ("BlockUIBack", "CtrlFrame_tl"); the
// importer writes screen_<name>.bmp (the nine frame pieces as one 5x5
// screen_CtrlFrame.bmp). Empty for solid and the icon atlases.
[[nodiscard]] std::string_view sprite_sheet_name(Sprite sprite) noexcept;
// Authored size in that build's sheet; {0,0} when the build has no such sprite.
[[nodiscard]] Size sprite_size(UiStyle style, Sprite sprite) noexcept;
// "SmallFont", "MedFont", "MyriadMedium".
[[nodiscard]] std::string_view font_asset_name(Font font) noexcept;

// SpriteFont.MeasureString / LineSpacing of the build whose layout is drawn.
class TextMetrics
{
  public:
    virtual ~TextMetrics() = default;
    [[nodiscard]] virtual Extent measure(Font font, std::string_view text) const = 0;
    [[nodiscard]] virtual int line_spacing(Font font) const = 0;
};

struct Draw final
{
    enum class Kind : std::uint8_t
    {
        sprite,
        text,
        // 1.9.9.8's screens call InGameHUD.DrawPlayerStats / DrawDistanceStr
        // at this point; the HUD draws them.
        hud_stats,
        hud_distance
    };
    Kind kind{Kind::sprite};
    Sprite sprite{Sprite::solid};
    // item_icon / item_icon_large: the build's InventoryItemIDs value (the atlas
    // cell), or -1 for an item no retail build has an icon for.
    int icon{};
    // The application's own item, for drawing an icon when no atlas has it.
    InventoryItemId item{InventoryItemId::bare_hands};
    // Modding, visuals area: the stack's mod item (InventoryStack::mod_item),
    // whose own icon the application draws when it has one; 0 for every retail
    // stack. Not part of record(): the retail parity grammar is unchanged.
    std::uint32_t mod_item{};
    float x{};
    float y{};
    float width{};
    float height{};
    Color color{};
    Font font{Font::small};
    std::string text;
    float scale{1.0F};
    int outline{};
    Color outline_color{0U, 0U, 0U, 255U};
};
using DrawList = std::vector<Draw>;
// One line in ScreensParityProbe's grammar ("S name=... dst=... rgba=...").
[[nodiscard]] std::string record(const Draw &draw);

// 1.6.3.8 composes every frame into a 1280x720 buffer and letterboxes it into
// the window (DNAGame.Draw); the Classic layouts are therefore in buffer
// pixels at every resolution. 1.9.9.8 lays out in window pixels, scaled by
// height / 720 and anchored to the window.
inline constexpr int classic_buffer_width = 1280;
inline constexpr int classic_buffer_height = 720;
[[nodiscard]] IntRect classic_buffer_destination(int window_width, int window_height) noexcept;
// DNAGame.ScreenToBuffer(Point): integer arithmetic, as the original's mouse.
[[nodiscard]] Point classic_window_to_buffer(Point window, IntRect destination) noexcept;

// The InventoryItemIDs value of `item` in the build `style` (the icon atlas
// cell); -1 when that build's atlas has no such item.
[[nodiscard]] int retail_icon(InventoryItemId item, UiStyle style) noexcept;

enum class ScreenId : std::uint8_t
{
    none,
    inventory,
    crafting, // only when the crafting layout is a screen of its own (Classic)
    crate
};

// Which screen is drawn for the open ScreenId.
//
//   style        inventory            crafting              crate
//   Classic      BlockPickerScreen    CraftingUIScreen      CrateScreen (1.6.3.8)
//   Modern       CraftingScreen       (part of inventory)   CrateScreen (1.9.9.8)
//   Remastered   inventory with the   (part of inventory)   crate beside the
//                crafting list                              backpack
enum class Layout : std::uint8_t
{
    classic_inventory,
    classic_crafting,
    classic_crate,
    modern_inventory,
    modern_crate,
    remastered_inventory,
    remastered_crate
};

enum class Cue : std::uint8_t
{
    click,
    error,
    craft,
    drop_item
};
// The XACT cue: "Click", "Error", "craft", "dropitem".
[[nodiscard]] std::string_view cue_name(Cue cue) noexcept;

enum class Button : std::uint8_t
{
    left,
    right
};
enum class Key : std::uint8_t
{
    escape,
    inventory, // E
    drop,      // Q
    enter,
    up,
    down,
    left,
    right,
    // Remastered only; the retail layouts read none of these. Each is another
    // way to an operation the pointer already has, for keyboard and gamepad.
    split,             // split / place one: what the right button does
    transfer,          // quick move: what Shift + left button does
    switch_pane,       // between the slots and the recipe list
    next_category,     // the crafting tabs
    previous_category
};

// Slots are addressed alike in every layout: 0..31 the backpack in array
// order, 32..39 the first tray, 40..47 the second tray (Modern worlds),
// 100..131 the open crate.
inline constexpr std::size_t backpack_slots = PlayerInventory::storage_size;
inline constexpr std::size_t tray_base = 32U;
inline constexpr std::size_t crate_base = 100U;
inline constexpr std::size_t crate_slots = 32U;

// The moments the original's CrateScreen sends CrateFocusMessage. It is not
// "whenever the selector moves": a slot another player's message names is
// LOCKED for everyone else (CrateScreen.IsSelectedSlotLocked) and is skipped
// when the crate is destroyed (Crate.EjectContents), so announcing more often
// than the original locks slots a stock player expects to be free.
enum class CrateFocusCause : std::uint8_t
{
    opened,  // OnPushed: the selector starts on the active tray slot
    key,     // an arrow-key or D-pad PRESS moved it (the held-key repeat sends nothing)
    pointer, // the pointer came to rest on a crate slot it was not on before
    closed   // Escape / Back: nothing is selected any more
};

// What CrateFocusMessage carries for one of those moments in a world of the
// given rules: whether the crate's own location is named (IntVector3.Zero
// otherwise) and CrateScreen._selectedLocation. Empty when that release sends
// nothing: 1.6.3.8 never announces a pointer move, 1.9.9.8 announces one only
// onto a crate slot. `selected` is Session::selected_slot().
//   1.6.3.8: an 8 x 4 grid, the tray is row 4.
//   1.9.9.8: a 4 x 8 grid, the tray is row 8.
struct CrateFocus final
{
    bool in_crate{};
    int x{};
    int y{};
    constexpr bool operator==(const CrateFocus &) const noexcept = default;
};
[[nodiscard]] std::optional<CrateFocus> crate_focus_announcement(CrateFocusCause cause, std::size_t selected,
                                                                 bool modern_rules) noexcept;

// Everything a screen needs from the game. Rules live behind this interface;
// nothing in Session decides what is craftable or how large a stack may be.
class Host
{
  public:
    virtual ~Host() = default;
    [[nodiscard]] virtual PlayerInventory &inventory() noexcept = 0;
    [[nodiscard]] virtual const PlayerInventory &inventory() const noexcept = 0;
    // The open crate's slot; nullptr when no crate is open.
    [[nodiscard]] virtual PlayerInventory::Slot *crate_slot(std::size_t index) noexcept = 0;
    [[nodiscard]] virtual const PlayerInventory::Slot *crate_slot(std::size_t index) const noexcept = 0;
    // Another player's cursor rests on it (CrateScreen.IsSlotLocked).
    [[nodiscard]] virtual bool crate_slot_locked(std::size_t index) const noexcept = 0;
    // PlayerInventory.CanCraft under the world's rules and game mode.
    [[nodiscard]] virtual bool can_craft(const CraftingRecipe &recipe) const noexcept = 0;
    // PlayerInventory.Craft, once, or (`all`) for as long as it stays craftable.
    virtual void craft(std::size_t recipe_index, bool all) = 0;
    // 1.9.9.8 ignores Shift in Creative (Tier2Item.CheckInput).
    [[nodiscard]] virtual bool creative() const noexcept = 0;
    virtual void play(Cue cue) = 0;
    // PickupManager.CreatePickup(stack, player position + 1, dropped: true).
    virtual void drop(const InventoryStack &stack) = 0;
    // A crate slot was written (ItemCrateMessage in the original).
    virtual void crate_changed(std::size_t index) = 0;
    // The crate screen reached one of the moments the original announces its
    // selector (see CrateFocusCause). `selected` is the selected slot.
    virtual void crate_focus(CrateFocusCause, std::size_t /*selected*/) {}
    // (Polish) The name and description lines shown for a stack: a mod
    // item's own (InventoryStack::mod_item), otherwise the retail item's.
    [[nodiscard]] virtual ItemText item_words(const InventoryStack &stack, InventoryProfile profile) const
    {
        return item_text(stack.item, profile);
    }
    // (Polish) The cookbook every screen lists and indexes (craft() takes an
    // index into it): crafting_recipes(profile), then the session's mod
    // recipes (CraftingRecipe::mod_recipe), so a retail recipe keeps its
    // index. Without mod recipes it IS crafting_recipes(profile).
    [[nodiscard]] virtual const std::vector<CraftingRecipe> &recipes() const
    {
        return crafting_recipes(inventory().profile());
    }
    // (Polish) How much of an ingredient the player has (a mod ingredient:
    // only stacks of that mod item).
    [[nodiscard]] virtual std::uint32_t ingredient_count(const CraftingIngredient &ingredient) const
    {
        return inventory().count(ingredient.item);
    }
};

// Arrow keys held this frame, for the original's auto-repeat.
struct HeldKeys final
{
    bool up{};
    bool down{};
    bool left{};
    bool right{};
};

// Remastered's crafting categories: everything the world lists, then 1.9.9.8's
// four groups. A Classic world's recipes are filed under the group the same
// result has in 1.9.9.8 (recipe_type_for), so every one is under some tab.
enum class RecipeTab : std::uint8_t
{
    all,
    materials,
    tools,
    weapons,
    structures
};
inline constexpr std::size_t recipe_tab_count = 5U;

// What the pointer rests on in the Remastered panel, other than a slot.
enum class Hot : std::uint8_t
{
    none,
    close,
    tab,        // index: the RecipeTab
    recipe,     // index: the row on screen (0 = top)
    row_craft,  // index: the row whose own craft button it is
    craft_one,
    craft_all,
    ingredient  // index: 0..3 of the selected recipe
};

class Session final
{
  public:
    // The resolved interface style (never `automatic`) and the back-buffer
    // size. The interface scale and the screen margins place the Remastered
    // panel (remastered_layout.hpp); the two retail layouts are exactly where
    // the original puts them and ignore both.
    void configure(UiStyle style, int width, int height, float ui_scale = 1.0F,
                   float safe_area = 1.0F) noexcept;
    [[nodiscard]] UiStyle style() const noexcept { return style_; }

    // Screen.OnPushed. Opening `crafting` when the crafting layout lives inside
    // the inventory screen opens the inventory.
    void open(Host &host, ScreenId screen);
    // Hand a held stack back (PlayerInventory.AddInventoryItem; what does not
    // fit is dropped) and close.
    void close(Host &host);
    // Forget the screen without touching an item: the caller has already dealt
    // with the held stack (death, a destroyed crate, leaving the world).
    void abandon() noexcept;
    // The controller's Y in the picker and crate screens, and Shift+click: send
    // the held stack, or else the selected one, to the other section.
    void quick_transfer(Host &host);
    [[nodiscard]] ScreenId screen() const noexcept { return screen_; }
    [[nodiscard]] bool is_open() const noexcept { return screen_ != ScreenId::none; }
    [[nodiscard]] Layout layout() const noexcept;

    // Input, in window pixels.
    // Where the pointer rests, without it having moved: the original reads the
    // mouse position every frame, but only a MOVE re-selects a slot.
    void place_pointer(int x, int y) noexcept;
    void pointer_moved(Host &host, int x, int y);
    void button_pressed(Host &host, Button button, bool shift);
    void button_released(Host &host, Button button);
    void wheel(Host &host, int direction); // +1 away from the user, -1 toward
    // `shift`: Shift is down (Enter on the selected Classic recipe crafts all).
    void key_pressed(Host &host, Key key, bool shift = false);
    void update(Host &host, float seconds, HeldKeys held = {});

    [[nodiscard]] DrawList draw(const Host &host, const TextMetrics &metrics) const;

    // The stack on the cursor.
    [[nodiscard]] const std::optional<InventoryStack> &held() const noexcept { return held_; }
    [[nodiscard]] std::optional<InventoryStack> &held() noexcept { return held_; }
    // The slot the selector is on (always a valid slot), and whether the
    // original would draw the selector there.
    [[nodiscard]] std::size_t selected_slot() const noexcept { return selected_; }
    [[nodiscard]] bool selector_visible() const noexcept;
    // Classic crafting: the selected recipe (index into the world's cookbook).
    [[nodiscard]] std::optional<std::size_t> selected_recipe(const Host &host) const;
    [[nodiscard]] int selected_ingredient() const noexcept { return ingredient_; }
    // How many cells below (+) or above (-) the selected recipe the strip's
    // cursor is drawn while the pointer rests on another cell.
    [[nodiscard]] int strip_cursor(const Host &host) const { return draw_selector_ - strip_position(host); }
    // Modern crafting: open group, open type within it, hovered recipe.
    [[nodiscard]] std::size_t group() const noexcept { return group_; }
    [[nodiscard]] std::size_t type_in_group() const noexcept { return type_[group_]; }
    [[nodiscard]] std::size_t recipe_in_type() const noexcept;
    // True while the game's pointer should be shown over the screen.
    [[nodiscard]] Point pointer() const noexcept { return pointer_; }
    // The pointer was the last thing to choose a slot or a control (false once
    // an arrow key or the D-pad has moved the selector instead).
    [[nodiscard]] bool pointer_active() const noexcept { return pointer_active_; }

    // Where things are, in LAYOUT pixels: the 1280x720 buffer for the Classic
    // layouts, window pixels for the Modern ones, and for Remastered its own
    // units measured from the panel's top-left corner (remastered_view()).
    // to_window() converts a point for callers that script the pointer (tests,
    // captures).
    [[nodiscard]] Point to_window(Point layout_point) const noexcept;

    // ---- Remastered ---------------------------------------------------------
    // Where the Remastered panel is: window pixels per layout unit, and the
    // window position of the panel's top-left corner (layout 0,0).
    struct RemasteredView final
    {
        float scale{1.0F};
        float origin_x{};
        float origin_y{};
        int trays{1}; // tray rows shown: 2 in a 1.9.9.8-rules world
    };
    [[nodiscard]] RemasteredView remastered_view() const noexcept;
    [[nodiscard]] RecipeTab recipe_tab() const noexcept { return rm_tab_; }
    // The recipes a tab lists, as cookbook indices: what the world's own
    // crafting screen lists (a Classic world: the discovered recipes of
    // strip_recipes; a Modern world: every recipe a 1.9.9.8 tab has), the ones
    // that can be made right now first.
    [[nodiscard]] std::vector<std::size_t> tab_recipes(const Host &host, RecipeTab tab) const;
    [[nodiscard]] std::vector<std::size_t> tab_recipes(const Host &host) const
    {
        return tab_recipes(host, rm_tab_);
    }
    // The first list entry on screen, and whether keys and the gamepad drive
    // the recipe list (true) or the slots.
    [[nodiscard]] std::size_t recipe_scroll() const noexcept { return rm_scroll_; }
    [[nodiscard]] bool recipes_focused() const noexcept { return rm_recipes_focused_; }
    // A key or the D-pad has moved the cursor since the screen opened and the
    // pointer has not taken it back: the cursor is where the player is looking.
    [[nodiscard]] bool keys_navigating() const noexcept { return rm_keys_moved_ && !pointer_active_; }
    [[nodiscard]] Hot hot() const noexcept { return rm_hot_; }
    [[nodiscard]] int hot_index() const noexcept { return rm_hot_index_; }
    [[nodiscard]] std::optional<IntRect> tab_rect(RecipeTab tab) const noexcept;
    [[nodiscard]] std::optional<IntRect> recipe_row_rect(std::size_t row) const noexcept;
    [[nodiscard]] std::optional<IntRect> row_craft_rect(std::size_t row) const noexcept;
    [[nodiscard]] std::optional<IntRect> craft_rect(bool all) const noexcept;
    [[nodiscard]] std::optional<IntRect> detail_rect() const noexcept;
    // Choose a tab / a recipe (by cookbook index; scrolled into view), as a
    // click on it does.
    void set_recipe_tab(const Host &host, RecipeTab tab);
    void select_recipe(const Host &host, std::size_t recipe_index);
    // Host::craft for the selected recipe, once or (`all`) for as long as the
    // world lets it be made, with the original's cue; the error cue when it
    // cannot be made or a stack is on the cursor (neither retail screen can
    // craft while holding one).
    void craft_selected(Host &host, bool all);
    [[nodiscard]] IntRect panel() const noexcept;
    [[nodiscard]] std::optional<IntRect> slot_rect(std::size_t slot) const noexcept;
    [[nodiscard]] std::optional<IntRect> close_rect() const noexcept;
    [[nodiscard]] std::optional<IntRect> button_rect() const noexcept;
    [[nodiscard]] std::optional<IntRect> strip_rect(int cell) const noexcept;       // Classic, 0..6
    [[nodiscard]] std::optional<IntRect> ingredient_rect(const Host &host, int index) const;
    [[nodiscard]] std::optional<IntRect> group_rect(std::size_t group) const noexcept; // Modern
    [[nodiscard]] std::optional<IntRect> type_rect(std::size_t type) const noexcept;   // Modern
    [[nodiscard]] std::optional<IntRect> recipe_rect(const Host &host, std::size_t index) const;

    // The recipes the crafting layout lists, as indices into the cookbook.
    // Classic layout: PlayerInventory.DiscoverRecipies' order for a Classic
    // world; for a Modern world (which has no discovery) every listed recipe,
    // craftable ones first. Modern layout: the recipes of one type, in book
    // order; a Classic world's recipes are filed under the type the same
    // result has in 1.9.9.8 (see recipe_type_for).
    [[nodiscard]] std::vector<std::size_t> strip_recipes(const Host &host) const;
    [[nodiscard]] std::vector<std::size_t> type_recipes(const Host &host, RecipeType type) const;

  private:
    struct Geometry;
    [[nodiscard]] Geometry geometry() const noexcept;
    [[nodiscard]] Point to_layout(Point window) const noexcept;
    [[nodiscard]] bool modern_layout() const noexcept;
    [[nodiscard]] bool crafting_in_inventory() const noexcept;
    [[nodiscard]] bool recipe_tree_shown() const noexcept;
    [[nodiscard]] int strip_position(const Host &host) const;
    [[nodiscard]] std::size_t tray_count(const Host &host) const noexcept;
    [[nodiscard]] std::optional<std::size_t> slot_at(const Host &host, Point layout_point) const noexcept;
    [[nodiscard]] PlayerInventory::Slot *slot(Host &host, std::size_t index) const noexcept;
    [[nodiscard]] const PlayerInventory::Slot *slot(const Host &host, std::size_t index) const noexcept;
    void write(Host &host, std::size_t index, PlayerInventory::Slot value) const;
    [[nodiscard]] bool selected_locked(const Host &host) const noexcept;
    void quick_move(Host &host);
    void pick_or_place(Host &host);
    void split_or_place_one(Host &host);
    void drop_selected(Host &host);
    // A click outside the panel with a stack on the cursor: all of it (left
    // button) or `one` item (right) becomes a pickup at the player's feet.
    void throw_held(Host &host, bool one);
    [[nodiscard]] bool count_up(Host &host);
    [[nodiscard]] bool count_down(Host &host);
    void move_selection(Host &host, int horizontal, int vertical);
    void return_held(Host &host);
    void leave_crafting(Host &host);
    void classic_crafting_button(Host &host, bool shift, bool enter);
    void classic_crafting_hover(Host &host, bool moved);
    void classic_craft(Host &host, std::size_t recipe, bool all);
    void modern_crafting_hover(Host &host, bool pressed, bool shift);
    void update_popup(const Host &host, float seconds);
    void draw_classic_inventory(const Host &host, const TextMetrics &metrics, DrawList &out) const;
    void draw_classic_crafting(const Host &host, const TextMetrics &metrics, DrawList &out) const;
    void draw_classic_crate(const Host &host, const TextMetrics &metrics, DrawList &out) const;
    void draw_modern_inventory(const Host &host, const TextMetrics &metrics, DrawList &out) const;
    void draw_modern_crate(const Host &host, const TextMetrics &metrics, DrawList &out) const;
    // Remastered: hit testing and input routing only. Everything that changes
    // an inventory is one of the functions above.
    [[nodiscard]] bool remastered_layout() const noexcept { return style_ == UiStyle::remastered; }
    void remastered_hover(const Host &host, Point layout_point);
    void remastered_button(Host &host, Button button, bool shift);
    void remastered_key(Host &host, Key key, bool shift);
    void remastered_move(Host &host, int horizontal, int vertical);
    void remastered_step_recipe(Host &host, int direction);
    void remastered_show_recipe(const Host &host);
    void remastered_craft(Host &host, std::size_t recipe_index, bool all);
    [[nodiscard]] std::optional<std::size_t> remastered_recipe(const Host &host) const;
    [[nodiscard]] int remastered_ingredients(const Host &host) const;

    // The one interface style: classic, modern or remastered.
    UiStyle style_{UiStyle::classic};
    float ui_scale_{1.0F};
    float safe_area_{1.0F};
    int width_{classic_buffer_width};
    int height_{classic_buffer_height};
    ScreenId screen_{ScreenId::none};
    std::optional<InventoryStack> held_;
    // _selectedLocation / _selectorInCrateGrid, as one slot index.
    std::size_t selected_{tray_base};
    // _mousePointerActive and _hitTestTrue: both survive closing the screen in
    // 1.6.3.8, so the selector is where the player left it.
    bool pointer_active_{};
    bool hit_{};
    Point pointer_{-100, -100};
    Point last_pointer_{-100, -100};
    bool close_hover_{};
    // FrameButtonControl: Hovering and CaptureInput.
    bool button_hover_{};
    bool button_captured_{};
    // CraftingUIScreen.
    std::optional<std::size_t> recipe_; // cookbook index of _selectedRecipe
    int ingredient_{};                  // _selectedIngredientIndex (0 = the result)
    int draw_selector_{};               // _drawSelectorIndex
    int scroll_to_{-1};                 // _recipeToScrollTo
    float scroll_offset_{};             // _scrollOffsetAmt
    bool moved_during_scroll_{};        // mouseMovementDuringScroll
    bool hover_done_{};                 // the pointer already ran this frame's hover pass
    bool crafting_over_inventory_{};    // the crafting screen was pushed on the inventory screen
    // CraftingScreen (1.9.9.8).
    std::size_t group_{};
    std::array<std::size_t, 4> type_{};
    std::array<std::array<std::size_t, 11>, 4> recipe_in_type_{};
    // The item-name pop-up: _popUpTimer (0.25 s in CraftingScreen, 1 s in
    // CrateScreen), fade in and fade out (0.25 s each).
    float popup_wait_{};
    float popup_fade_in_{};
    float popup_fade_out_{2.0F};
    float popup_fade_out_length_{0.25F};
    std::optional<InventoryStack> popup_item_;
    std::optional<std::size_t> popup_next_;
    Point popup_location_{};
    // Arrow-key auto-repeat: waitScrollTimer (0.5 s) and autoScrollTimer (0.1 s).
    float repeat_wait_{};
    float repeat_tick_{};
    // 1.9.9.8 CrateScreen._lastSelectedLocation: the crate slot the pointer
    // last announced. It starts on slot 0 and outlives the screen.
    std::size_t crate_pointer_focus_{crate_base};
    // 1.9.9.8 CrateScreen._setSelectorToCrate (which grid the selector returns
    // to from the tray row) and _selectedLocation.Y while it is on a tray:
    // 8, or 9..11 when the pointer chose the slot (hover index / 4).
    bool modern_crate_side_{};
    int modern_tray_row_{8};
    // Remastered.
    int rm_trays_{1};
    RecipeTab rm_tab_{RecipeTab::all};
    std::optional<std::size_t> rm_recipe_; // cookbook index of the selected recipe
    std::size_t rm_scroll_{};
    bool rm_recipes_focused_{};
    bool rm_keys_moved_{};
    Hot rm_hot_{Hot::none};
    int rm_hot_index_{-1};
};

// The 1.9.9.8 recipe type a recipe is listed under in the Modern layout. A
// Modern world's recipes carry their own; a Classic world's take the type of
// the Modern recipe with the same result, or - for the few results 1.9.9.8 no
// longer crafts - the type of the nearest kind (see screens.cpp).
[[nodiscard]] RecipeType recipe_type_for(const CraftingRecipe &recipe, InventoryProfile world) noexcept;

} // namespace openminerz::screens
